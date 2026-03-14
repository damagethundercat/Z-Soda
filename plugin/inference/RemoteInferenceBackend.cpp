#include "inference/RemoteInferenceBackend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "inference/RuntimePathResolver.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winhttp.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace zsoda::inference {
namespace {

constexpr char kRemoteCommandEnv[] = "ZSODA_REMOTE_INFERENCE_COMMAND";
constexpr char kRemoteCommandEnvLegacy[] = "ZSODA_REMOTE_BACKEND_COMMAND";
constexpr char kRequestSchema[] = "zsoda.remote_depth.v2";
constexpr char kDefaultLocalRemoteHost[] = "127.0.0.1";
constexpr int kDefaultLocalRemotePort = 8345;
constexpr char kBinaryContentType[] = "application/octet-stream";
constexpr char kHeaderModelId[] = "X-ZSoda-Model-Id";
constexpr char kHeaderQuality[] = "X-ZSoda-Quality";
constexpr char kHeaderResizeMode[] = "X-ZSoda-Resize-Mode";
constexpr char kHeaderFrameHash[] = "X-ZSoda-Frame-Hash";
constexpr char kHeaderWidth[] = "X-ZSoda-Source-Width";
constexpr char kHeaderHeight[] = "X-ZSoda-Source-Height";
constexpr char kHeaderChannels[] = "X-ZSoda-Source-Channels";
constexpr char kHeaderResponseWidth[] = "X-ZSoda-Depth-Width";
constexpr char kHeaderResponseHeight[] = "X-ZSoda-Depth-Height";
constexpr char kHeaderResponseElapsedMs[] = "X-ZSoda-Elapsed-Ms";
constexpr char kHeaderResponseModelId[] = "X-ZSoda-Resolved-Model-Id";

std::atomic<std::uint64_t> g_temp_file_counter{0U};

using SteadyClock = std::chrono::steady_clock;

struct BinaryRequestMetadata {
  std::string model_id;
  int quality = 1;
  PreprocessResizeMode resize_mode = PreprocessResizeMode::kUpperBoundLetterbox;
  std::uint64_t frame_hash = 0;
  int width = 0;
  int height = 0;
  int channels = 3;
};

struct BinaryEndpointResponse {
  int width = 0;
  int height = 0;
  float elapsed_ms = 0.0F;
  std::string resolved_model_id;
  std::string body;
};

struct RemoteTimingBreakdown {
  double host_convert_ms = 0.0;
  double request_encode_ms = 0.0;
  double service_ms = 0.0;
  double response_decode_ms = 0.0;
  double total_ms = 0.0;
};

std::string ReadEnvOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return {};
  }
  return value;
}

bool ReadEnvBoolOrDefault(const char* name, bool default_value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return default_value;
  }

  std::string normalized;
  while (*raw != '\0') {
    const char ch = *raw++;
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
    return false;
  }
  return default_value;
}

bool RemoteTimingTraceEnabled() {
  static const bool enabled =
      ReadEnvBoolOrDefault("ZSODA_REMOTE_TRACE", false) ||
      ReadEnvBoolOrDefault("ZSODA_PIPELINE_TRACE", false);
  return enabled;
}

void AppendRemoteTrace(const char* stage, const char* detail = nullptr) {
#if defined(_WIN32)
  if (!RemoteTimingTraceEnabled()) {
    return;
  }
  char temp_path[MAX_PATH] = {};
  const DWORD written = ::GetTempPathA(MAX_PATH, temp_path);
  if (written == 0 || written >= MAX_PATH) {
    return;
  }

  char log_path[MAX_PATH] = {};
  std::snprintf(log_path, sizeof(log_path), "%s%s", temp_path, "ZSoda_AE_Runtime.log");
  FILE* file = std::fopen(log_path, "ab");
  if (file == nullptr) {
    return;
  }

  SYSTEMTIME now = {};
  ::GetLocalTime(&now);
  const unsigned long tid = static_cast<unsigned long>(::GetCurrentThreadId());
  std::fprintf(file,
               "%04u-%02u-%02u %02u:%02u:%02u.%03u | RemoteTrace | tid=%lu, stage=%s, detail=%s\r\n",
               static_cast<unsigned>(now.wYear),
               static_cast<unsigned>(now.wMonth),
               static_cast<unsigned>(now.wDay),
               static_cast<unsigned>(now.wHour),
               static_cast<unsigned>(now.wMinute),
               static_cast<unsigned>(now.wSecond),
               static_cast<unsigned>(now.wMilliseconds),
               tid,
               stage != nullptr ? stage : "<null>",
               (detail != nullptr && detail[0] != '\0') ? detail : "<none>");
  std::fclose(file);
#else
  (void)stage;
  (void)detail;
#endif
}

double ElapsedMilliseconds(SteadyClock::time_point started) {
  return std::chrono::duration<double, std::milli>(SteadyClock::now() - started).count();
}

std::string ResolveCommandTemplate(const RemoteBackendCommandConfig& command_config) {
  if (!command_config.command_template.empty()) {
    return command_config.command_template;
  }
  const std::string configured = ReadEnvOrEmpty(kRemoteCommandEnv);
  if (!configured.empty()) {
    return configured;
  }
  return ReadEnvOrEmpty(kRemoteCommandEnvLegacy);
}

std::string BuildBackendName(RuntimeBackend backend) {
  std::string name = "RemoteInferenceBackend[";
  name.append(RuntimeBackendName(backend));
  name.push_back(']');
  return name;
}

std::string ResolvePreloadModelId(const RuntimeOptions& options,
                                  const std::filesystem::path& script_path) {
  std::string model_id = ReadEnvOrEmpty("ZSODA_LOCKED_MODEL_ID");
  if (model_id.empty()) {
    // Legacy alias kept for backwards compatibility with older local setups.
    model_id = ReadEnvOrEmpty("ZSODA_HQ_MODEL_ID");
  }
  if (model_id.empty() &&
      script_path.filename().string().find("distill_any_depth_remote_service.py") !=
          std::string::npos) {
    model_id = "distill-any-depth-base";
  }
  return model_id;
}

bool IsExistingFile(const std::filesystem::path& path) {
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) && !ec;
}

std::string NormalizeLoopbackHost(std::string host) {
  if (host.empty()) {
    return kDefaultLocalRemoteHost;
  }
  return host;
}

std::string BuildLocalInferenceEndpoint(std::string_view host, int port) {
  std::ostringstream endpoint;
  endpoint << "http://" << host << ":" << port << "/zsoda/depth";
  return endpoint.str();
}

std::string BuildLocalStatusEndpoint(std::string_view host, int port) {
  std::ostringstream endpoint;
  endpoint << "http://" << host << ":" << port << "/status";
  return endpoint.str();
}

std::filesystem::path ResolveRemoteServiceScriptPath(const RuntimeOptions& options) {
  if (!options.remote_service_script_path.empty()) {
    const std::filesystem::path explicit_path(options.remote_service_script_path);
    if (IsExistingFile(explicit_path)) {
      return explicit_path;
    }
  }

  if (!options.plugin_directory.empty()) {
    const std::filesystem::path plugin_dir(options.plugin_directory);
    for (const auto& root : BuildRuntimeAssetSearchRoots(
             plugin_dir, std::filesystem::path(options.runtime_asset_root))) {
      const std::array<std::filesystem::path, 3> candidates = {
          root / "zsoda_py" / "distill_any_depth_remote_service.py",
          root / "tools" / "distill_any_depth_remote_service.py",
          root / "distill_any_depth_remote_service.py",
      };
      for (const auto& candidate : candidates) {
        if (IsExistingFile(candidate)) {
          return candidate;
        }
      }
    }
  }

  return {};
}

bool ValidateRequest(const InferenceRequest& request,
                     zsoda::core::FrameBuffer* out_depth,
                     std::string* error) {
  if (request.source == nullptr || out_depth == nullptr) {
    if (error != nullptr) {
      *error = "invalid inference request: source and output buffers are required";
    }
    return false;
  }
  if (request.source->empty()) {
    if (error != nullptr) {
      *error = "invalid inference request: source frame is empty";
    }
    return false;
  }
  const auto& source_desc = request.source->desc();
  if (source_desc.width <= 0 || source_desc.height <= 0 || source_desc.channels <= 0) {
    if (error != nullptr) {
      *error = "invalid inference request: source frame descriptor is invalid";
    }
    return false;
  }
  if (request.quality <= 0) {
    if (error != nullptr) {
      *error = "invalid inference request: quality must be greater than zero";
    }
    return false;
  }
  return true;
}

float SanitizeFinite(float value) {
  return std::isfinite(value) ? value : 0.0F;
}

const char* ResizeModeName(PreprocessResizeMode mode) {
  switch (mode) {
    case PreprocessResizeMode::kUpperBoundLetterbox:
      return "upper_bound_letterbox";
    case PreprocessResizeMode::kLowerBoundCenterCrop:
      return "lower_bound_center_crop";
    case PreprocessResizeMode::kStretch:
      return "stretch";
  }
  return "upper_bound_letterbox";
}

unsigned char QuantizeUnitFloat(float value) {
  const float sanitized = std::clamp(SanitizeFinite(value), 0.0F, 1.0F);
  return static_cast<unsigned char>(std::lround(sanitized * 255.0F));
}

std::vector<std::uint8_t> EncodeRgb8Payload(const zsoda::core::FrameBuffer& source) {
  const auto& desc = source.desc();
  const std::size_t pixel_count =
      static_cast<std::size_t>(std::max(0, desc.width)) * static_cast<std::size_t>(std::max(0, desc.height));
  std::vector<std::uint8_t> payload;
  payload.resize(pixel_count * 3U);
  std::size_t offset = 0U;
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      float red = 0.0F;
      float green = 0.0F;
      float blue = 0.0F;
      if (desc.channels == 1) {
        red = green = blue = source.at(x, y, 0);
      } else {
        red = source.at(x, y, 0);
        green = source.at(x, y, std::min(1, desc.channels - 1));
        blue = source.at(x, y, std::min(2, desc.channels - 1));
      }
      payload[offset++] = QuantizeUnitFloat(red);
      payload[offset++] = QuantizeUnitFloat(green);
      payload[offset++] = QuantizeUnitFloat(blue);
    }
  }
  return payload;
}

bool WritePortablePixmap(const zsoda::core::FrameBuffer& source,
                         const std::filesystem::path& path,
                         std::string* error) {
  const auto& desc = source.desc();
  if (desc.width <= 0 || desc.height <= 0 || desc.channels <= 0) {
    if (error != nullptr) {
      *error = "source frame descriptor is invalid for ppm serialization";
    }
    return false;
  }

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    if (error != nullptr) {
      *error = "failed to open source image temp file: " + path.string();
    }
    return false;
  }

  stream << "P6\n" << desc.width << " " << desc.height << "\n255\n";
  if (!stream.good()) {
    if (error != nullptr) {
      *error = "failed to write source image header: " + path.string();
    }
    return false;
  }

  std::array<char, 3> pixel = {};
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      float red = 0.0F;
      float green = 0.0F;
      float blue = 0.0F;
      if (desc.channels == 1) {
        red = green = blue = source.at(x, y, 0);
      } else {
        red = source.at(x, y, 0);
        green = source.at(x, y, std::min(1, desc.channels - 1));
        blue = source.at(x, y, std::min(2, desc.channels - 1));
      }

      pixel[0] = static_cast<char>(QuantizeUnitFloat(red));
      pixel[1] = static_cast<char>(QuantizeUnitFloat(green));
      pixel[2] = static_cast<char>(QuantizeUnitFloat(blue));
      stream.write(pixel.data(), static_cast<std::streamsize>(pixel.size()));
      if (!stream.good()) {
        if (error != nullptr) {
          *error = "failed to write source image pixel data: " + path.string();
        }
        return false;
      }
    }
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

std::string EscapeJsonString(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 8U);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        escaped.append("\\\\");
        break;
      case '"':
        escaped.append("\\\"");
        break;
      case '\b':
        escaped.append("\\b");
        break;
      case '\f':
        escaped.append("\\f");
        break;
      case '\n':
        escaped.append("\\n");
        break;
      case '\r':
        escaped.append("\\r");
        break;
      case '\t':
        escaped.append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          escaped.push_back(' ');
        } else {
          escaped.push_back(ch);
        }
        break;
    }
  }
  return escaped;
}

bool SerializeRequestPayload(const InferenceRequest& request,
                             std::string_view model_id,
                             std::string_view model_path,
                             std::string_view source_path,
                             std::string* out_payload,
                             std::string* error) {
  if (out_payload == nullptr) {
    if (error != nullptr) {
      *error = "internal error: request payload output is null";
    }
    return false;
  }
  if (request.source == nullptr || request.source->empty()) {
    if (error != nullptr) {
      *error = "internal error: source frame is unavailable";
    }
    return false;
  }

  const auto& source = *request.source;
  const auto& source_desc = source.desc();
  std::ostringstream payload;
  payload.precision(9);
  payload << "{";
  payload << "\"schema\":\"" << kRequestSchema << "\",";
  payload << "\"model_id\":\"" << EscapeJsonString(model_id) << "\",";
  payload << "\"model_path\":\"" << EscapeJsonString(model_path) << "\",";
  payload << "\"quality\":" << request.quality << ",";
  payload << "\"resize_mode\":\"" << EscapeJsonString(ResizeModeName(request.resize_mode)) << "\",";
  payload << "\"frame_hash\":" << request.frame_hash << ",";
  payload << "\"source\":{";
  payload << "\"width\":" << source_desc.width << ",";
  payload << "\"height\":" << source_desc.height << ",";
  payload << "\"channels\":" << source_desc.channels << ",";
  payload << "\"format\":\"ppm\",";
  payload << "\"path\":\"" << EscapeJsonString(source_path) << "\"";
  payload << "}";
  payload << "}";

  *out_payload = payload.str();
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

std::uint64_t ProcessIdentifier() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

bool MakeUniqueTempFilePath(std::string_view prefix,
                            std::string_view extension,
                            std::filesystem::path* out_path,
                            std::string* error) {
  if (out_path == nullptr) {
    if (error != nullptr) {
      *error = "internal error: temp file output path is null";
    }
    return false;
  }

  std::error_code temp_error;
  const std::filesystem::path temp_root = std::filesystem::temp_directory_path(temp_error);
  if (temp_error) {
    if (error != nullptr) {
      *error = "failed to resolve temp directory (" + temp_error.message() + ")";
    }
    return false;
  }

  for (int attempt = 0; attempt < 16; ++attempt) {
    const auto time_seed = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    const std::uint64_t sequence = g_temp_file_counter.fetch_add(1U) + 1U;
    std::ostringstream filename;
    filename << prefix << "-" << ProcessIdentifier() << "-" << time_seed << "-" << sequence
             << extension;
    const std::filesystem::path candidate = temp_root / filename.str();
    std::error_code exists_error;
    if (!std::filesystem::exists(candidate, exists_error)) {
      if (exists_error) {
        if (error != nullptr) {
          *error = "failed to test temp file path: " + candidate.string() + " (" +
                   exists_error.message() + ")";
        }
        return false;
      }
      *out_path = candidate;
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }
  }

  if (error != nullptr) {
    *error = "failed to allocate a unique temp file path after retries";
  }
  return false;
}

bool WriteTextFile(const std::filesystem::path& path, std::string_view payload, std::string* error) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    if (error != nullptr) {
      *error = "failed to open file for writing: " + path.string();
    }
    return false;
  }
  stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (!stream.good()) {
    if (error != nullptr) {
      *error = "failed to write request payload: " + path.string();
    }
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool ReadTextFile(const std::filesystem::path& path, std::string* payload, std::string* error) {
  if (payload == nullptr) {
    if (error != nullptr) {
      *error = "internal error: response payload output is null";
    }
    return false;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    if (error != nullptr) {
      *error = "failed to open response payload: " + path.string();
    }
    return false;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  if (!stream.good() && !stream.eof()) {
    if (error != nullptr) {
      *error = "failed while reading response payload: " + path.string();
    }
    return false;
  }
  *payload = buffer.str();
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

struct ScopedTempFile {
  explicit ScopedTempFile(std::filesystem::path path) : path_(std::move(path)) {}
  ~ScopedTempFile() {
    std::error_code cleanup_error;
    std::filesystem::remove(path_, cleanup_error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string QuoteShellArgument(std::string_view argument) {
  std::string quoted;
  quoted.reserve(argument.size() + 4U);
  quoted.push_back('"');
  for (const char ch : argument) {
    if (ch == '"') {
      quoted.append("\\\"");
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('"');
  return quoted;
}

std::filesystem::path DefaultRemoteServiceLogPath() {
  std::error_code temp_error;
  const std::filesystem::path temp_root = std::filesystem::temp_directory_path(temp_error);
  if (!temp_error) {
    return temp_root / "ZSoda_RemoteService.log";
  }
  return std::filesystem::path("ZSoda_RemoteService.log");
}

std::string BuildPythonRuntimeProbeScript() {
  return "import torch, PIL, transformers; "
         "from transformers import AutoImageProcessor, AutoModelForDepthEstimation; "
         "print('MPS=1' if getattr(getattr(torch,'backends',None),'mps',None) is not None and "
         "torch.backends.mps.is_available() else ('CUDA=1' if torch.cuda.is_available() else 'CPU=1'))";
}

std::string CollapseWhitespace(std::string_view text) {
  std::string collapsed;
  collapsed.reserve(text.size());
  bool last_was_space = false;
  for (const char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!last_was_space) {
        collapsed.push_back(' ');
        last_was_space = true;
      }
      continue;
    }
    collapsed.push_back(ch);
    last_was_space = false;
  }
  return collapsed;
}

std::string SummarizePythonProbeOutput(std::string_view output) {
  std::string summary = CollapseWhitespace(output);
  while (!summary.empty() && std::isspace(static_cast<unsigned char>(summary.front())) != 0) {
    summary.erase(summary.begin());
  }
  while (!summary.empty() && std::isspace(static_cast<unsigned char>(summary.back())) != 0) {
    summary.pop_back();
  }
  constexpr std::size_t kMaxSummaryLength = 320U;
  if (summary.size() > kMaxSummaryLength) {
    summary.resize(kMaxSummaryLength);
    summary.append("...");
  }
  return summary;
}

std::string ReadLogTail(const std::filesystem::path& path, std::size_t max_bytes = 2048U) {
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return {};
  }

  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return {};
  }

  const std::streamoff bytes_to_read =
      static_cast<std::streamoff>(std::min<std::uintmax_t>(size, max_bytes));
  if (bytes_to_read > 0) {
    stream.seekg(-bytes_to_read, std::ios::end);
  }
  std::string content(static_cast<std::size_t>(bytes_to_read), '\0');
  if (bytes_to_read > 0) {
    stream.read(content.data(), bytes_to_read);
    content.resize(static_cast<std::size_t>(stream.gcount()));
  } else {
    content.clear();
  }
  return content;
}

#if defined(_WIN32)
bool WideToUtf8(const std::wstring& wide, std::string* utf8, std::string* error) {
  if (utf8 == nullptr) {
    if (error != nullptr) {
      *error = "internal error: utf8 output pointer is null";
    }
    return false;
  }
  utf8->clear();
  if (wide.empty()) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  const int required = ::WideCharToMultiByte(CP_UTF8,
                                             0,
                                             wide.data(),
                                             static_cast<int>(wide.size()),
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
  if (required <= 0) {
    if (error != nullptr) {
      *error = "WideCharToMultiByte failed: " + std::to_string(::GetLastError());
    }
    return false;
  }

  utf8->assign(static_cast<std::size_t>(required), '\0');
  const int written = ::WideCharToMultiByte(CP_UTF8,
                                            0,
                                            wide.data(),
                                            static_cast<int>(wide.size()),
                                            utf8->data(),
                                            required,
                                            nullptr,
                                            nullptr);
  if (written != required) {
    if (error != nullptr) {
      *error = "WideCharToMultiByte produced unexpected length";
    }
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool Utf8ToWide(std::string_view utf8, std::wstring* wide, std::string* error) {
  if (wide == nullptr) {
    if (error != nullptr) {
      *error = "internal error: wide output pointer is null";
    }
    return false;
  }
  wide->clear();
  if (utf8.empty()) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  const int required = ::MultiByteToWideChar(CP_UTF8,
                                             0,
                                             utf8.data(),
                                             static_cast<int>(utf8.size()),
                                             nullptr,
                                             0);
  if (required <= 0) {
    if (error != nullptr) {
      *error = "MultiByteToWideChar failed: " + std::to_string(::GetLastError());
    }
    return false;
  }

  wide->assign(static_cast<std::size_t>(required), L'\0');
  const int written = ::MultiByteToWideChar(CP_UTF8,
                                            0,
                                            utf8.data(),
                                            static_cast<int>(utf8.size()),
                                            wide->data(),
                                            required);
  if (written != required) {
    if (error != nullptr) {
      *error = "MultiByteToWideChar produced unexpected length";
    }
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool PostJsonToEndpoint(std::string_view endpoint,
                        std::string_view payload,
                        std::string_view api_key,
                        int timeout_ms,
                        std::string* response_payload,
                        std::string* error) {
  if (response_payload == nullptr) {
    if (error != nullptr) {
      *error = "internal error: response payload output is null";
    }
    return false;
  }

  std::wstring endpoint_wide;
  if (!Utf8ToWide(endpoint, &endpoint_wide, error)) {
    return false;
  }

  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(endpoint_wide.c_str(),
                         static_cast<DWORD>(endpoint_wide.size()),
                         0,
                         &components)) {
    if (error != nullptr) {
      *error = "WinHttpCrackUrl failed for endpoint: " + std::string(endpoint) +
               " (" + std::to_string(::GetLastError()) + ")";
    }
    return false;
  }

  const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  const std::wstring host(components.lpszHostName,
                          static_cast<std::size_t>(components.dwHostNameLength));
  std::wstring path(components.lpszUrlPath,
                    static_cast<std::size_t>(components.dwUrlPathLength));
  if (path.empty()) {
    path = L"/";
  }
  if (components.dwExtraInfoLength > 0U && components.lpszExtraInfo != nullptr) {
    path.append(components.lpszExtraInfo, static_cast<std::size_t>(components.dwExtraInfoLength));
  }

  HINTERNET session = ::WinHttpOpen(L"ZSoda/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
  if (session == nullptr) {
    if (error != nullptr) {
      *error = "WinHttpOpen failed (" + std::to_string(::GetLastError()) + ")";
    }
    return false;
  }

  const int effective_timeout = timeout_ms > 0 ? timeout_ms : 30000;
  ::WinHttpSetTimeouts(session,
                       effective_timeout,
                       effective_timeout,
                       effective_timeout,
                       effective_timeout);

  HINTERNET connection = ::WinHttpConnect(session, host.c_str(), components.nPort, 0);
  if (connection == nullptr) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpConnect failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  const DWORD request_flags = secure ? WINHTTP_FLAG_SECURE : 0U;
  HINTERNET request_handle = ::WinHttpOpenRequest(connection,
                                                  L"POST",
                                                  path.c_str(),
                                                  nullptr,
                                                  WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  request_flags);
  if (request_handle == nullptr) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpOpenRequest failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  std::wstring headers = L"Content-Type: application/json\r\n";
  if (!api_key.empty()) {
    std::wstring api_key_wide;
    if (!Utf8ToWide(api_key, &api_key_wide, error)) {
      ::WinHttpCloseHandle(request_handle);
      ::WinHttpCloseHandle(connection);
      ::WinHttpCloseHandle(session);
      return false;
    }
    headers.append(L"Authorization: Bearer ");
    headers.append(api_key_wide);
    headers.append(L"\r\n");
  }

  const BOOL send_ok = ::WinHttpSendRequest(request_handle,
                                            headers.c_str(),
                                            static_cast<DWORD>(headers.size()),
                                            const_cast<char*>(payload.data()),
                                            static_cast<DWORD>(payload.size()),
                                            static_cast<DWORD>(payload.size()),
                                            0);
  if (!send_ok) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(request_handle);
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpSendRequest failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  if (!::WinHttpReceiveResponse(request_handle, nullptr)) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(request_handle);
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpReceiveResponse failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  DWORD status_code = 0;
  DWORD status_code_size = sizeof(status_code);
  if (!::WinHttpQueryHeaders(request_handle,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status_code,
                             &status_code_size,
                             WINHTTP_NO_HEADER_INDEX)) {
    status_code = 0U;
  }

  response_payload->clear();
  while (true) {
    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(request_handle, &available)) {
      const DWORD win_error = ::GetLastError();
      ::WinHttpCloseHandle(request_handle);
      ::WinHttpCloseHandle(connection);
      ::WinHttpCloseHandle(session);
      if (error != nullptr) {
        *error = "WinHttpQueryDataAvailable failed (" + std::to_string(win_error) + ")";
      }
      return false;
    }
    if (available == 0U) {
      break;
    }

    std::string chunk(static_cast<std::size_t>(available), '\0');
    DWORD read = 0;
    if (!::WinHttpReadData(request_handle, chunk.data(), available, &read)) {
      const DWORD win_error = ::GetLastError();
      ::WinHttpCloseHandle(request_handle);
      ::WinHttpCloseHandle(connection);
      ::WinHttpCloseHandle(session);
      if (error != nullptr) {
        *error = "WinHttpReadData failed (" + std::to_string(win_error) + ")";
      }
      return false;
    }
    chunk.resize(static_cast<std::size_t>(read));
    response_payload->append(chunk);
  }

  ::WinHttpCloseHandle(request_handle);
  ::WinHttpCloseHandle(connection);
  ::WinHttpCloseHandle(session);

  if (status_code < 200U || status_code >= 300U) {
    if (error != nullptr) {
      *error = "remote endpoint returned HTTP " + std::to_string(status_code) +
               (response_payload->empty() ? std::string() : ": " + *response_payload);
    }
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool QueryCustomHeader(HINTERNET request_handle,
                       std::wstring_view header_name,
                       std::wstring* value,
                       std::string* error) {
  if (value == nullptr) {
    if (error != nullptr) {
      *error = "internal error: header output is null";
    }
    return false;
  }
  value->clear();

  DWORD bytes = 0;
  if (::WinHttpQueryHeaders(request_handle,
                            WINHTTP_QUERY_CUSTOM,
                            const_cast<wchar_t*>(header_name.data()),
                            WINHTTP_NO_OUTPUT_BUFFER,
                            &bytes,
                            WINHTTP_NO_HEADER_INDEX)) {
    // no-op, bytes now contains required size.
  } else {
    const DWORD header_error = ::GetLastError();
    if (header_error == ERROR_INSUFFICIENT_BUFFER && bytes > 0U) {
      // Expected probe path.
    } else if (header_error == ERROR_WINHTTP_HEADER_NOT_FOUND) {
      return true;
    } else {
      if (error != nullptr) {
        *error = "WinHttpQueryHeaders failed for custom header (" +
                 std::to_string(header_error) + ")";
      }
      return false;
    }
  }

  if (bytes == 0U) {
    return true;
  }

  std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
  if (!::WinHttpQueryHeaders(request_handle,
                             WINHTTP_QUERY_CUSTOM,
                             const_cast<wchar_t*>(header_name.data()),
                             buffer.data(),
                             &bytes,
                             WINHTTP_NO_HEADER_INDEX)) {
    const DWORD header_error = ::GetLastError();
    if (header_error == ERROR_WINHTTP_HEADER_NOT_FOUND) {
      return true;
    }
    if (error != nullptr) {
      *error = "WinHttpQueryHeaders read failed for custom header (" +
               std::to_string(header_error) + ")";
    }
    return false;
  }

  if (!buffer.empty() && buffer.back() == L'\0') {
    buffer.pop_back();
  }
  *value = std::move(buffer);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool ParseWideInt(std::wstring_view text, int* value, std::string* error) {
  if (value == nullptr) {
    if (error != nullptr) {
      *error = "internal error: integer output is null";
    }
    return false;
  }
  if (text.empty()) {
    if (error != nullptr) {
      *error = "required integer header is empty";
    }
    return false;
  }
  try {
    const int parsed = std::stoi(std::wstring(text));
    if (parsed <= 0) {
      throw std::invalid_argument("non-positive");
    }
    *value = parsed;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception&) {
    if (error != nullptr) {
      *error = "invalid integer header value";
    }
    return false;
  }
}

bool ParseWideFloat(std::wstring_view text, float* value, std::string* error) {
  if (value == nullptr) {
    if (error != nullptr) {
      *error = "internal error: float output is null";
    }
    return false;
  }
  if (text.empty()) {
    *value = 0.0F;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }
  try {
    const float parsed = std::stof(std::wstring(text));
    *value = std::isfinite(parsed) ? parsed : 0.0F;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception&) {
    if (error != nullptr) {
      *error = "invalid float header value";
    }
    return false;
  }
}

bool PostBinaryToEndpoint(std::string_view endpoint,
                          const BinaryRequestMetadata& metadata,
                          const std::vector<std::uint8_t>& payload,
                          std::string_view api_key,
                          int timeout_ms,
                          BinaryEndpointResponse* response,
                          std::string* error) {
  if (response == nullptr) {
    if (error != nullptr) {
      *error = "internal error: binary response output is null";
    }
    return false;
  }
  response->body.clear();
  response->width = 0;
  response->height = 0;
  response->elapsed_ms = 0.0F;
  response->resolved_model_id.clear();

  std::wstring endpoint_wide;
  if (!Utf8ToWide(endpoint, &endpoint_wide, error)) {
    return false;
  }

  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(endpoint_wide.c_str(),
                         static_cast<DWORD>(endpoint_wide.size()),
                         0,
                         &components)) {
    if (error != nullptr) {
      *error = "WinHttpCrackUrl failed for endpoint: " + std::string(endpoint) +
               " (" + std::to_string(::GetLastError()) + ")";
    }
    return false;
  }

  const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  const std::wstring host(components.lpszHostName,
                          static_cast<std::size_t>(components.dwHostNameLength));
  std::wstring path(components.lpszUrlPath,
                    static_cast<std::size_t>(components.dwUrlPathLength));
  if (path.empty()) {
    path = L"/";
  }
  if (components.dwExtraInfoLength > 0U && components.lpszExtraInfo != nullptr) {
    path.append(components.lpszExtraInfo, static_cast<std::size_t>(components.dwExtraInfoLength));
  }

  HINTERNET session = ::WinHttpOpen(L"ZSoda/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
  if (session == nullptr) {
    if (error != nullptr) {
      *error = "WinHttpOpen failed (" + std::to_string(::GetLastError()) + ")";
    }
    return false;
  }

  const int effective_timeout = timeout_ms > 0 ? timeout_ms : 30000;
  ::WinHttpSetTimeouts(session,
                       effective_timeout,
                       effective_timeout,
                       effective_timeout,
                       effective_timeout);

  HINTERNET connection = ::WinHttpConnect(session, host.c_str(), components.nPort, 0);
  if (connection == nullptr) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpConnect failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  const DWORD request_flags = secure ? WINHTTP_FLAG_SECURE : 0U;
  HINTERNET request_handle = ::WinHttpOpenRequest(connection,
                                                  L"POST",
                                                  path.c_str(),
                                                  nullptr,
                                                  WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  request_flags);
  if (request_handle == nullptr) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpOpenRequest failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  std::wstring model_id_wide;
  if (!Utf8ToWide(metadata.model_id, &model_id_wide, error)) {
    ::WinHttpCloseHandle(request_handle);
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    return false;
  }
  std::wstring resize_mode_wide;
  if (!Utf8ToWide(ResizeModeName(metadata.resize_mode), &resize_mode_wide, error)) {
    ::WinHttpCloseHandle(request_handle);
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    return false;
  }

  std::wstring headers = L"Content-Type: application/octet-stream\r\n";
  headers.append(L"Accept: application/octet-stream\r\n");
  headers.append(L"X-ZSoda-Model-Id: ");
  headers.append(model_id_wide);
  headers.append(L"\r\nX-ZSoda-Quality: ");
  headers.append(std::to_wstring(metadata.quality));
  headers.append(L"\r\nX-ZSoda-Resize-Mode: ");
  headers.append(resize_mode_wide);
  headers.append(L"\r\nX-ZSoda-Frame-Hash: ");
  headers.append(std::to_wstring(metadata.frame_hash));
  headers.append(L"\r\nX-ZSoda-Source-Width: ");
  headers.append(std::to_wstring(metadata.width));
  headers.append(L"\r\nX-ZSoda-Source-Height: ");
  headers.append(std::to_wstring(metadata.height));
  headers.append(L"\r\nX-ZSoda-Source-Channels: ");
  headers.append(std::to_wstring(metadata.channels));
  headers.append(L"\r\n");
  if (!api_key.empty()) {
    std::wstring api_key_wide;
    if (!Utf8ToWide(api_key, &api_key_wide, error)) {
      ::WinHttpCloseHandle(request_handle);
      ::WinHttpCloseHandle(connection);
      ::WinHttpCloseHandle(session);
      return false;
    }
    headers.append(L"Authorization: Bearer ");
    headers.append(api_key_wide);
    headers.append(L"\r\n");
  }

  const BOOL send_ok = ::WinHttpSendRequest(request_handle,
                                            headers.c_str(),
                                            static_cast<DWORD>(headers.size()),
                                            payload.empty() ? WINHTTP_NO_REQUEST_DATA
                                                            : const_cast<std::uint8_t*>(payload.data()),
                                            static_cast<DWORD>(payload.size()),
                                            static_cast<DWORD>(payload.size()),
                                            0);
  if (!send_ok) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(request_handle);
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpSendRequest failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  if (!::WinHttpReceiveResponse(request_handle, nullptr)) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(request_handle);
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpReceiveResponse failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  DWORD status_code = 0;
  DWORD status_code_size = sizeof(status_code);
  if (!::WinHttpQueryHeaders(request_handle,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status_code,
                             &status_code_size,
                             WINHTTP_NO_HEADER_INDEX)) {
    status_code = 0U;
  }

  while (true) {
    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(request_handle, &available)) {
      const DWORD win_error = ::GetLastError();
      ::WinHttpCloseHandle(request_handle);
      ::WinHttpCloseHandle(connection);
      ::WinHttpCloseHandle(session);
      if (error != nullptr) {
        *error = "WinHttpQueryDataAvailable failed (" + std::to_string(win_error) + ")";
      }
      return false;
    }
    if (available == 0U) {
      break;
    }

    std::string chunk(static_cast<std::size_t>(available), '\0');
    DWORD read = 0;
    if (!::WinHttpReadData(request_handle, chunk.data(), available, &read)) {
      const DWORD win_error = ::GetLastError();
      ::WinHttpCloseHandle(request_handle);
      ::WinHttpCloseHandle(connection);
      ::WinHttpCloseHandle(session);
      if (error != nullptr) {
        *error = "WinHttpReadData failed (" + std::to_string(win_error) + ")";
      }
      return false;
    }
    chunk.resize(static_cast<std::size_t>(read));
    response->body.append(chunk);
  }

  std::wstring width_header;
  std::wstring height_header;
  std::wstring elapsed_header;
  std::wstring resolved_model_header;
  const bool width_ok = QueryCustomHeader(request_handle, L"X-ZSoda-Depth-Width", &width_header, error);
  const bool height_ok =
      QueryCustomHeader(request_handle, L"X-ZSoda-Depth-Height", &height_header, error);
  const bool elapsed_ok =
      QueryCustomHeader(request_handle, L"X-ZSoda-Elapsed-Ms", &elapsed_header, error);
  const bool resolved_model_ok =
      QueryCustomHeader(request_handle, L"X-ZSoda-Resolved-Model-Id", &resolved_model_header, error);

  ::WinHttpCloseHandle(request_handle);
  ::WinHttpCloseHandle(connection);
  ::WinHttpCloseHandle(session);

  if (!width_ok || !height_ok || !elapsed_ok || !resolved_model_ok) {
    return false;
  }

  if (status_code < 200U || status_code >= 300U) {
    if (error != nullptr) {
      *error = "remote endpoint returned HTTP " + std::to_string(status_code) +
               (response->body.empty() ? std::string() : ": " + response->body);
    }
    return false;
  }

  if (!ParseWideInt(width_header, &response->width, error) ||
      !ParseWideInt(height_header, &response->height, error) ||
      !ParseWideFloat(elapsed_header, &response->elapsed_ms, error)) {
    return false;
  }
  if (!resolved_model_header.empty() &&
      !WideToUtf8(resolved_model_header, &response->resolved_model_id, error)) {
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool GetEndpointText(std::string_view endpoint,
                     int timeout_ms,
                     std::string* response_payload,
                     std::string* error) {
  if (response_payload == nullptr) {
    if (error != nullptr) {
      *error = "internal error: response payload output is null";
    }
    return false;
  }

  std::wstring endpoint_wide;
  if (!Utf8ToWide(endpoint, &endpoint_wide, error)) {
    return false;
  }

  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(endpoint_wide.c_str(),
                         static_cast<DWORD>(endpoint_wide.size()),
                         0,
                         &components)) {
    if (error != nullptr) {
      *error = "WinHttpCrackUrl failed for endpoint: " + std::string(endpoint) +
               " (" + std::to_string(::GetLastError()) + ")";
    }
    return false;
  }

  const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  const std::wstring host(components.lpszHostName,
                          static_cast<std::size_t>(components.dwHostNameLength));
  std::wstring path(components.lpszUrlPath,
                    static_cast<std::size_t>(components.dwUrlPathLength));
  if (path.empty()) {
    path = L"/";
  }
  if (components.dwExtraInfoLength > 0U && components.lpszExtraInfo != nullptr) {
    path.append(components.lpszExtraInfo, static_cast<std::size_t>(components.dwExtraInfoLength));
  }

  HINTERNET session = ::WinHttpOpen(L"ZSoda/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
  if (session == nullptr) {
    if (error != nullptr) {
      *error = "WinHttpOpen failed (" + std::to_string(::GetLastError()) + ")";
    }
    return false;
  }

  const int effective_timeout = timeout_ms > 0 ? timeout_ms : 5000;
  ::WinHttpSetTimeouts(session,
                       effective_timeout,
                       effective_timeout,
                       effective_timeout,
                       effective_timeout);

  HINTERNET connection = ::WinHttpConnect(session, host.c_str(), components.nPort, 0);
  if (connection == nullptr) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpConnect failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  const DWORD request_flags = secure ? WINHTTP_FLAG_SECURE : 0U;
  HINTERNET request_handle = ::WinHttpOpenRequest(connection,
                                                  L"GET",
                                                  path.c_str(),
                                                  nullptr,
                                                  WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  request_flags);
  if (request_handle == nullptr) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpOpenRequest failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  if (!::WinHttpSendRequest(request_handle,
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0)) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(request_handle);
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpSendRequest failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  if (!::WinHttpReceiveResponse(request_handle, nullptr)) {
    const DWORD win_error = ::GetLastError();
    ::WinHttpCloseHandle(request_handle);
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    if (error != nullptr) {
      *error = "WinHttpReceiveResponse failed (" + std::to_string(win_error) + ")";
    }
    return false;
  }

  DWORD status_code = 0;
  DWORD status_code_size = sizeof(status_code);
  if (!::WinHttpQueryHeaders(request_handle,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status_code,
                             &status_code_size,
                             WINHTTP_NO_HEADER_INDEX)) {
    status_code = 0U;
  }

  response_payload->clear();
  while (true) {
    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(request_handle, &available)) {
      const DWORD win_error = ::GetLastError();
      ::WinHttpCloseHandle(request_handle);
      ::WinHttpCloseHandle(connection);
      ::WinHttpCloseHandle(session);
      if (error != nullptr) {
        *error = "WinHttpQueryDataAvailable failed (" + std::to_string(win_error) + ")";
      }
      return false;
    }
    if (available == 0U) {
      break;
    }

    std::string chunk(static_cast<std::size_t>(available), '\0');
    DWORD read = 0;
    if (!::WinHttpReadData(request_handle, chunk.data(), available, &read)) {
      const DWORD win_error = ::GetLastError();
      ::WinHttpCloseHandle(request_handle);
      ::WinHttpCloseHandle(connection);
      ::WinHttpCloseHandle(session);
      if (error != nullptr) {
        *error = "WinHttpReadData failed (" + std::to_string(win_error) + ")";
      }
      return false;
    }
    chunk.resize(static_cast<std::size_t>(read));
    response_payload->append(chunk);
  }

  ::WinHttpCloseHandle(request_handle);
  ::WinHttpCloseHandle(connection);
  ::WinHttpCloseHandle(session);

  if (status_code < 200U || status_code >= 300U) {
    if (error != nullptr) {
      *error = "remote endpoint returned HTTP " + std::to_string(status_code) +
               (response_payload->empty() ? std::string() : ": " + *response_payload);
    }
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

#if defined(_WIN32)
enum class PythonTorchCapability {
  kUnavailable,
  kTorchCpu,
  kTorchCuda,
};

std::optional<std::filesystem::path> ResolveExecutableOnPath(const wchar_t* executable) {
  if (executable == nullptr || executable[0] == L'\0') {
    return std::nullopt;
  }
  const DWORD required =
      ::SearchPathW(nullptr, executable, nullptr, 0, nullptr, nullptr);
  if (required == 0U) {
    return std::nullopt;
  }
  std::wstring buffer(required, L'\0');
  const DWORD written =
      ::SearchPathW(nullptr, executable, nullptr, required, buffer.data(), nullptr);
  if (written == 0U) {
    return std::nullopt;
  }
  buffer.resize(written);
  return std::filesystem::path(buffer);
}

void PushUniquePythonCandidate(const std::filesystem::path& path,
                               std::vector<std::filesystem::path>* candidates) {
  if (candidates == nullptr || path.empty()) {
    return;
  }
  std::error_code exists_error;
  if (!std::filesystem::exists(path, exists_error) || exists_error) {
    return;
  }
  const auto normalized = path.lexically_normal();
  for (const auto& existing : *candidates) {
    if (existing.lexically_normal() == normalized) {
      return;
    }
  }
  candidates->push_back(normalized);
}

std::vector<std::filesystem::path> GatherPythonAutostartCandidates(const RuntimeOptions& options) {
  std::vector<std::filesystem::path> candidates;
  if (!options.plugin_directory.empty()) {
    const std::filesystem::path plugin_dir(options.plugin_directory);
    for (const auto& root : BuildRuntimeAssetSearchRoots(
             plugin_dir, std::filesystem::path(options.runtime_asset_root))) {
      PushUniquePythonCandidate(root / "zsoda_py" / "python.exe", &candidates);
      PushUniquePythonCandidate(root / "zsoda_py" / "python" / "python.exe", &candidates);
      PushUniquePythonCandidate(root / "zsoda_py" / "runtime" / "python.exe", &candidates);
    }
  }

  if (const auto path_python = ResolveExecutableOnPath(L"python.exe")) {
    PushUniquePythonCandidate(*path_python, &candidates);
  }
  if (const auto path_python3 = ResolveExecutableOnPath(L"python3.exe")) {
    PushUniquePythonCandidate(*path_python3, &candidates);
  }

  const std::filesystem::path local_appdata = ReadEnvOrEmpty("LOCALAPPDATA");
  const auto collect_python_children = [&](const std::filesystem::path& root,
                                           const auto& directory_name_filter) {
    std::error_code iter_error;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::directory_iterator iter(root, options, iter_error);
    std::filesystem::directory_iterator end;
    while (!iter_error && iter != end) {
      const auto entry_path = iter->path();
      std::error_code status_error;
      const bool is_directory = iter->is_directory(status_error);
      if (!status_error && is_directory) {
        const std::wstring directory_name = entry_path.filename().wstring();
        if (directory_name_filter(directory_name)) {
          PushUniquePythonCandidate(entry_path / "python.exe", &candidates);
        }
      }

      iter.increment(iter_error);
    }
  };

  if (!local_appdata.empty()) {
    const std::filesystem::path windows_apps = local_appdata / "Microsoft" / "WindowsApps";
    collect_python_children(windows_apps, [](const std::wstring& directory_name) {
      return directory_name.find(L"PythonSoftwareFoundation.Python.") != std::wstring::npos;
    });

    const std::filesystem::path programs_python = local_appdata / "Programs" / "Python";
    collect_python_children(programs_python,
                            [](const std::wstring& /*directory_name*/) { return true; });
  }

  const std::filesystem::path system_drive = "C:\\";
  collect_python_children(system_drive, [](const std::wstring& directory_name) {
    return directory_name.rfind(L"Python", 0) == 0;
  });

  return candidates;
}

PythonTorchCapability ProbePythonTorchCapability(const std::filesystem::path& python_path,
                                                 std::string* probe_output) {
  if (probe_output != nullptr) {
    probe_output->clear();
  }
  if (python_path.empty()) {
    return PythonTorchCapability::kUnavailable;
  }

  std::error_code temp_error;
  const std::filesystem::path temp_root = std::filesystem::temp_directory_path(temp_error);
  if (temp_error) {
    return PythonTorchCapability::kUnavailable;
  }
  const auto tick_count = static_cast<unsigned long long>(::GetTickCount64());
  const auto process_id = static_cast<unsigned long long>(::GetCurrentProcessId());
  const std::filesystem::path probe_log =
      temp_root / ("ZSoda_PythonProbe_" + std::to_string(process_id) + "_" + std::to_string(tick_count) + ".log");

  SECURITY_ATTRIBUTES security_attributes = {};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;
  HANDLE output_handle =
      ::CreateFileW(probe_log.wstring().c_str(),
                    GENERIC_WRITE | GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    &security_attributes,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                    nullptr);
  if (output_handle == INVALID_HANDLE_VALUE) {
    return PythonTorchCapability::kUnavailable;
  }

  const std::wstring python_wide = python_path.wstring();
  std::wstring probe_script;
  if (!Utf8ToWide(BuildPythonRuntimeProbeScript(), &probe_script, nullptr)) {
    ::CloseHandle(output_handle);
    return PythonTorchCapability::kUnavailable;
  }

  std::wstring command_line = L"\"";
  command_line.append(python_wide);
  command_line.append(L"\" -c \"");
  command_line.append(probe_script);
  command_line.append(L"\"");

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = output_handle;
  startup_info.hStdError = output_handle;

  PROCESS_INFORMATION process_info = {};
  std::wstring mutable_command_line = command_line;
  const BOOL create_ok = ::CreateProcessW(nullptr,
                                          mutable_command_line.data(),
                                          nullptr,
                                          nullptr,
                                          TRUE,
                                          CREATE_NO_WINDOW,
                                          nullptr,
                                          python_path.parent_path().wstring().c_str(),
                                          &startup_info,
                                          &process_info);
  if (!create_ok) {
    ::CloseHandle(output_handle);
    return PythonTorchCapability::kUnavailable;
  }

  const DWORD wait_result = ::WaitForSingleObject(process_info.hProcess, 15000);
  if (wait_result == WAIT_TIMEOUT) {
    ::TerminateProcess(process_info.hProcess, 1);
    ::WaitForSingleObject(process_info.hProcess, 2000);
  }
  DWORD exit_code = 1;
  ::GetExitCodeProcess(process_info.hProcess, &exit_code);
  ::CloseHandle(process_info.hThread);
  ::CloseHandle(process_info.hProcess);

  ::SetFilePointer(output_handle, 0, nullptr, FILE_BEGIN);
  std::string captured;
  std::array<char, 256> buffer{};
  DWORD bytes_read = 0;
  while (::ReadFile(output_handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) &&
         bytes_read > 0) {
    captured.append(buffer.data(), static_cast<std::size_t>(bytes_read));
  }
  ::CloseHandle(output_handle);
  if (probe_output != nullptr) {
    *probe_output = captured;
  }

  if (exit_code != 0) {
    return PythonTorchCapability::kUnavailable;
  }
  if (captured.find("CUDA=1") != std::string::npos) {
    return PythonTorchCapability::kTorchCuda;
  }
  if (captured.find("CPU=1") != std::string::npos) {
    return PythonTorchCapability::kTorchCpu;
  }
  return PythonTorchCapability::kUnavailable;
}

std::string ResolvePythonCommandForAutostart(const RuntimeOptions& options) {
  if (!options.remote_service_python.empty()) {
    return options.remote_service_python;
  }

  try {
    const auto candidates = GatherPythonAutostartCandidates(options);
    std::filesystem::path cpu_fallback;
    for (const auto& candidate : candidates) {
      std::string probe_output;
      const auto capability = ProbePythonTorchCapability(candidate, &probe_output);
      if (capability == PythonTorchCapability::kTorchCuda) {
        return candidate.string();
      }
      if (capability == PythonTorchCapability::kTorchCpu && cpu_fallback.empty()) {
        cpu_fallback = candidate;
      }
    }

    if (!cpu_fallback.empty()) {
      return cpu_fallback.string();
    }
  } catch (...) {
    // Autodiscovery must never break AE loader setup. Fall back to PATH python.
  }
  return "python";
}
#endif

bool StartDetachedPythonService(const RuntimeOptions& options,
                                std::string_view status_endpoint,
                                std::string* error) {
  const std::filesystem::path script_path = ResolveRemoteServiceScriptPath(options);
  if (script_path.empty()) {
    if (error != nullptr) {
      *error = "remote service script was not found";
    }
    return false;
  }

  std::wstring python_wide;
  const std::string python_command = ResolvePythonCommandForAutostart(options);
  std::string python_probe_output;
  const auto python_capability =
      ProbePythonTorchCapability(std::filesystem::path(python_command), &python_probe_output);
  if (python_capability == PythonTorchCapability::kUnavailable) {
    if (error != nullptr) {
      *error = "remote inference python runtime is unavailable or missing required packages (python=" +
               python_command + ")" +
               (python_probe_output.empty()
                    ? std::string()
                    : ": " + SummarizePythonProbeOutput(python_probe_output));
    }
    return false;
  }
  {
    const std::string trace_detail =
        std::string("python=") + python_command + ", script=" + script_path.string();
    AppendRemoteTrace("service_autostart_python", trace_detail.c_str());
  }
  if (!Utf8ToWide(python_command, &python_wide, error)) {
    return false;
  }

  std::wstring script_wide;
  if (!Utf8ToWide(script_path.string(), &script_wide, error)) {
    return false;
  }

  std::wstring host_wide;
  if (!Utf8ToWide(NormalizeLoopbackHost(options.remote_service_host), &host_wide, error)) {
    return false;
  }

  std::wstring preload_model_wide;
  const std::string preload_model_id = ResolvePreloadModelId(options, script_path);
  if (!preload_model_id.empty() && !Utf8ToWide(preload_model_id, &preload_model_wide, error)) {
    return false;
  }

  std::filesystem::path log_path = options.remote_service_log_path.empty()
                                       ? DefaultRemoteServiceLogPath()
                                       : std::filesystem::path(options.remote_service_log_path);
  std::wstring log_path_wide;
  if (!Utf8ToWide(log_path.string(), &log_path_wide, error)) {
    return false;
  }

  std::wstring command_line;
  command_line.reserve(512);
  command_line.push_back(L'"');
  command_line.append(python_wide);
  command_line.append(L"\" \"");
  command_line.append(script_wide);
  command_line.append(L"\" --host \"");
  command_line.append(host_wide);
  command_line.append(L"\" --port ");
  command_line.append(std::to_wstring(options.remote_service_port > 0 ? options.remote_service_port
                                                                      : kDefaultLocalRemotePort));
  if (!preload_model_wide.empty()) {
    command_line.append(L" --preload-model-id \"");
    command_line.append(preload_model_wide);
    command_line.push_back(L'"');
  }

  const std::filesystem::path working_directory = script_path.parent_path();
  std::wstring working_directory_wide;
  if (!Utf8ToWide(working_directory.string(), &working_directory_wide, error)) {
    return false;
  }

  SECURITY_ATTRIBUTES security_attributes = {};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;
  HANDLE log_handle = ::CreateFileW(log_path_wide.c_str(),
                                    FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security_attributes,
                                    OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
  if (log_handle == INVALID_HANDLE_VALUE) {
    if (error != nullptr) {
      *error = "CreateFileW failed for service log (" + std::to_string(::GetLastError()) + ")";
    }
    return false;
  }

  ::SetFilePointer(log_handle, 0, nullptr, FILE_END);
  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = log_handle;
  startup_info.hStdError = log_handle;

  PROCESS_INFORMATION process_info = {};
  std::wstring mutable_command_line = command_line;
  const BOOL create_ok = ::CreateProcessW(nullptr,
                                          mutable_command_line.data(),
                                          nullptr,
                                          nullptr,
                                          TRUE,
                                          CREATE_NO_WINDOW | DETACHED_PROCESS,
                                          nullptr,
                                          working_directory_wide.c_str(),
                                          &startup_info,
                                          &process_info);
  ::CloseHandle(log_handle);
  if (!create_ok) {
    if (error != nullptr) {
      *error = "CreateProcessW failed for remote inference service (" +
               std::to_string(::GetLastError()) + ")";
    }
    return false;
  }

  ::CloseHandle(process_info.hThread);
  ::CloseHandle(process_info.hProcess);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  std::string status_payload;
  std::string status_error;
  while (std::chrono::steady_clock::now() < deadline) {
    if (GetEndpointText(status_endpoint, 2000, &status_payload, &status_error)) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  if (error != nullptr) {
    *error = "remote inference service did not become healthy at " + std::string(status_endpoint) +
             " (python=" + python_command + ", log=" + log_path.string() + ")" +
             (status_error.empty() ? std::string() : ": " + status_error);
    const std::string log_tail = SummarizePythonProbeOutput(ReadLogTail(log_path));
    if (!log_tail.empty()) {
      *error += " | service_log=" + log_tail;
    }
  }
  return false;
}
#endif

#if !defined(_WIN32)
struct ParsedHttpUrl {
  std::string connect_host;
  std::string host_header;
  std::string port = "80";
  std::string path = "/";
};

struct SimpleHttpHeader {
  std::string name;
  std::string value;
};

struct SimpleHttpResponse {
  int status_code = 0;
  std::vector<SimpleHttpHeader> headers;
  std::string body;
};

std::string TrimAsciiWhitespace(std::string_view text) {
  std::size_t start = 0U;
  while (start < text.size() &&
         std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(text[end - 1U])) != 0) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

std::string NormalizeHttpHeaderName(std::string_view name) {
  std::string normalized;
  normalized.reserve(name.size());
  for (const char ch : name) {
    normalized.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return normalized;
}

bool TryGetHttpHeaderValue(const SimpleHttpResponse& response,
                           std::string_view name,
                           std::string* value) {
  if (value == nullptr) {
    return false;
  }
  const std::string normalized_name = NormalizeHttpHeaderName(name);
  for (const auto& header : response.headers) {
    if (header.name == normalized_name) {
      *value = header.value;
      return true;
    }
  }
  value->clear();
  return false;
}

bool ParseHttpEndpointUrl(std::string_view endpoint,
                          ParsedHttpUrl* parsed,
                          std::string* error) {
  if (parsed == nullptr) {
    if (error != nullptr) {
      *error = "internal error: parsed URL output is null";
    }
    return false;
  }

  constexpr std::string_view kHttpPrefix = "http://";
  if (endpoint.compare(0, kHttpPrefix.size(), kHttpPrefix) != 0) {
    if (error != nullptr) {
      *error = "non-Windows remote service only supports http:// endpoints";
    }
    return false;
  }

  std::string_view remainder = endpoint.substr(kHttpPrefix.size());
  if (remainder.empty()) {
    if (error != nullptr) {
      *error = "HTTP endpoint is missing a host";
    }
    return false;
  }

  const std::size_t slash_pos = remainder.find('/');
  const std::string_view authority =
      slash_pos == std::string_view::npos ? remainder : remainder.substr(0, slash_pos);
  const std::string_view raw_path =
      slash_pos == std::string_view::npos ? std::string_view() : remainder.substr(slash_pos);
  if (authority.empty()) {
    if (error != nullptr) {
      *error = "HTTP endpoint is missing an authority";
    }
    return false;
  }

  parsed->port = "80";
  parsed->path = raw_path.empty() ? "/" : std::string(raw_path);
  if (authority.front() == '[') {
    const std::size_t bracket_end = authority.find(']');
    if (bracket_end == std::string_view::npos || bracket_end <= 1U) {
      if (error != nullptr) {
        *error = "HTTP endpoint contains an invalid IPv6 host";
      }
      return false;
    }
    parsed->connect_host = std::string(authority.substr(1U, bracket_end - 1U));
    parsed->host_header = std::string(authority.substr(0U, bracket_end + 1U));
    if (bracket_end + 1U < authority.size()) {
      if (authority[bracket_end + 1U] != ':') {
        if (error != nullptr) {
          *error = "HTTP endpoint contains an invalid IPv6 authority";
        }
        return false;
      }
      parsed->port = std::string(authority.substr(bracket_end + 2U));
    }
  } else {
    const std::size_t colon_pos = authority.rfind(':');
    const bool has_single_port_delimiter =
        colon_pos != std::string_view::npos &&
        authority.find(':') == colon_pos;
    if (has_single_port_delimiter) {
      parsed->connect_host = std::string(authority.substr(0U, colon_pos));
      parsed->host_header = parsed->connect_host;
      parsed->port = std::string(authority.substr(colon_pos + 1U));
    } else {
      parsed->connect_host = std::string(authority);
      parsed->host_header = parsed->connect_host;
    }
  }

  if (parsed->connect_host.empty()) {
    if (error != nullptr) {
      *error = "HTTP endpoint is missing a host";
    }
    return false;
  }
  if (parsed->port.empty()) {
    if (error != nullptr) {
      *error = "HTTP endpoint is missing a port";
    }
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool ConfigureSocketTimeouts(int socket_fd, int timeout_ms) {
  if (socket_fd < 0) {
    return false;
  }
  const int effective_timeout = timeout_ms > 0 ? timeout_ms : 5000;
  timeval timeout = {};
  timeout.tv_sec = effective_timeout / 1000;
  timeout.tv_usec = (effective_timeout % 1000) * 1000;
  if (::setsockopt(socket_fd,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &timeout,
                   static_cast<socklen_t>(sizeof(timeout))) != 0) {
    return false;
  }
  if (::setsockopt(socket_fd,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   &timeout,
                   static_cast<socklen_t>(sizeof(timeout))) != 0) {
    return false;
  }
#if defined(SO_NOSIGPIPE)
  int no_sigpipe = 1;
  (void)::setsockopt(socket_fd,
                     SOL_SOCKET,
                     SO_NOSIGPIPE,
                     &no_sigpipe,
                     static_cast<socklen_t>(sizeof(no_sigpipe)));
#endif
  return true;
}

bool SendAllOnSocket(int socket_fd,
                     const std::string& payload,
                     std::string* error) {
  std::size_t sent = 0U;
  while (sent < payload.size()) {
    const ssize_t written = ::send(socket_fd,
                                   payload.data() + sent,
                                   payload.size() - sent,
                                   0);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error != nullptr) {
        *error = "socket send failed (" + std::to_string(errno) + ")";
      }
      return false;
    }
    if (written == 0) {
      if (error != nullptr) {
        *error = "socket send produced no progress";
      }
      return false;
    }
    sent += static_cast<std::size_t>(written);
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool ParseSimpleHttpResponse(const std::string& raw_response,
                             SimpleHttpResponse* response,
                             std::string* error) {
  if (response == nullptr) {
    if (error != nullptr) {
      *error = "internal error: HTTP response output is null";
    }
    return false;
  }

  const std::size_t header_end = raw_response.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    if (error != nullptr) {
      *error = "HTTP response is missing a header terminator";
    }
    return false;
  }
  const std::size_t status_end = raw_response.find("\r\n");
  if (status_end == std::string::npos || status_end > header_end) {
    if (error != nullptr) {
      *error = "HTTP response is missing a status line";
    }
    return false;
  }

  const std::string_view status_line(raw_response.data(), status_end);
  const std::size_t first_space = status_line.find(' ');
  const std::size_t second_space =
      first_space == std::string_view::npos
          ? std::string_view::npos
          : status_line.find(' ', first_space + 1U);
  if (first_space == std::string_view::npos) {
    if (error != nullptr) {
      *error = "HTTP response status line is malformed";
    }
    return false;
  }
  const std::size_t code_length =
      second_space == std::string_view::npos ? status_line.size() - first_space - 1U
                                             : second_space - first_space - 1U;
  const std::string code_text(status_line.substr(first_space + 1U, code_length));
  try {
    response->status_code = std::stoi(code_text);
  } catch (const std::exception&) {
    if (error != nullptr) {
      *error = "HTTP response contains an invalid status code";
    }
    return false;
  }

  response->headers.clear();
  std::size_t cursor = status_end + 2U;
  while (cursor < header_end) {
    const std::size_t line_end = raw_response.find("\r\n", cursor);
    if (line_end == std::string::npos || line_end > header_end) {
      break;
    }
    const std::string_view line(raw_response.data() + cursor, line_end - cursor);
    if (!line.empty()) {
      const std::size_t colon_pos = line.find(':');
      if (colon_pos != std::string_view::npos) {
        response->headers.push_back(
            {.name = NormalizeHttpHeaderName(line.substr(0U, colon_pos)),
             .value = TrimAsciiWhitespace(line.substr(colon_pos + 1U))});
      }
    }
    cursor = line_end + 2U;
  }

  response->body.assign(raw_response.data() + header_end + 4U,
                        raw_response.size() - header_end - 4U);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool PerformSimpleHttpRequest(std::string_view method,
                              std::string_view endpoint,
                              const std::vector<SimpleHttpHeader>& headers,
                              std::string_view body,
                              int timeout_ms,
                              SimpleHttpResponse* response,
                              std::string* error) {
  if (response == nullptr) {
    if (error != nullptr) {
      *error = "internal error: HTTP response output is null";
    }
    return false;
  }

  ParsedHttpUrl parsed;
  if (!ParseHttpEndpointUrl(endpoint, &parsed, error)) {
    return false;
  }

  addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* addr_list = nullptr;
  const int addr_status =
      ::getaddrinfo(parsed.connect_host.c_str(), parsed.port.c_str(), &hints, &addr_list);
  if (addr_status != 0) {
    if (error != nullptr) {
      *error = "getaddrinfo failed for " + parsed.connect_host + ":" + parsed.port +
               " (" + std::string(::gai_strerror(addr_status)) + ")";
    }
    return false;
  }

  int socket_fd = -1;
  int last_error = 0;
  for (addrinfo* current = addr_list; current != nullptr; current = current->ai_next) {
    socket_fd = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
    if (socket_fd < 0) {
      last_error = errno;
      continue;
    }
    (void)ConfigureSocketTimeouts(socket_fd, timeout_ms);
    if (::connect(socket_fd, current->ai_addr, current->ai_addrlen) == 0) {
      break;
    }
    last_error = errno;
    ::close(socket_fd);
    socket_fd = -1;
  }
  ::freeaddrinfo(addr_list);
  if (socket_fd < 0) {
    if (error != nullptr) {
      *error = "socket connect failed for " + parsed.connect_host + ":" + parsed.port +
               " (" + std::to_string(last_error) + ")";
    }
    return false;
  }

  std::string request;
  request.reserve(256U + body.size());
  request.append(method);
  request.push_back(' ');
  request.append(parsed.path);
  request.append(" HTTP/1.1\r\nHost: ");
  request.append(parsed.host_header);
  request.append("\r\nConnection: close\r\n");
  for (const auto& header : headers) {
    request.append(header.name);
    request.append(": ");
    request.append(header.value);
    request.append("\r\n");
  }
  if (!body.empty()) {
    request.append("Content-Length: ");
    request.append(std::to_string(body.size()));
    request.append("\r\n");
  }
  request.append("\r\n");
  request.append(body.data(), body.size());

  if (!SendAllOnSocket(socket_fd, request, error)) {
    ::close(socket_fd);
    return false;
  }

  std::string raw_response;
  std::array<char, 8192> buffer{};
  while (true) {
    const ssize_t read = ::recv(socket_fd, buffer.data(), buffer.size(), 0);
    if (read == 0) {
      break;
    }
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(socket_fd);
      if (error != nullptr) {
        *error = "socket receive failed (" + std::to_string(errno) + ")";
      }
      return false;
    }
    raw_response.append(buffer.data(), static_cast<std::size_t>(read));
  }
  ::close(socket_fd);

  if (raw_response.empty()) {
    if (error != nullptr) {
      *error = "HTTP request returned an empty response";
    }
    return false;
  }
  return ParseSimpleHttpResponse(raw_response, response, error);
}

bool ParsePositiveHeaderValue(std::string_view text,
                              int* value,
                              std::string* error) {
  if (value == nullptr) {
    if (error != nullptr) {
      *error = "internal error: integer output is null";
    }
    return false;
  }
  const std::string normalized = TrimAsciiWhitespace(text);
  if (normalized.empty()) {
    if (error != nullptr) {
      *error = "required integer header is empty";
    }
    return false;
  }
  try {
    const int parsed = std::stoi(normalized);
    if (parsed <= 0) {
      throw std::invalid_argument("non-positive");
    }
    *value = parsed;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception&) {
    if (error != nullptr) {
      *error = "invalid integer header value";
    }
    return false;
  }
}

bool ParseFiniteHeaderValue(std::string_view text,
                            float* value,
                            std::string* error) {
  if (value == nullptr) {
    if (error != nullptr) {
      *error = "internal error: float output is null";
    }
    return false;
  }
  const std::string normalized = TrimAsciiWhitespace(text);
  if (normalized.empty()) {
    *value = 0.0F;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }
  try {
    const float parsed = std::stof(normalized);
    *value = std::isfinite(parsed) ? parsed : 0.0F;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception&) {
    if (error != nullptr) {
      *error = "invalid float header value";
    }
    return false;
  }
}

bool PostJsonToEndpoint(std::string_view endpoint,
                        std::string_view payload,
                        std::string_view api_key,
                        int timeout_ms,
                        std::string* response_payload,
                        std::string* error) {
  if (response_payload == nullptr) {
    if (error != nullptr) {
      *error = "internal error: response payload output is null";
    }
    return false;
  }

  std::vector<SimpleHttpHeader> headers = {
      {.name = "Content-Type", .value = "application/json"},
  };
  if (!api_key.empty()) {
    headers.push_back({.name = "Authorization", .value = "Bearer " + std::string(api_key)});
  }

  SimpleHttpResponse response;
  if (!PerformSimpleHttpRequest("POST", endpoint, headers, payload, timeout_ms, &response, error)) {
    return false;
  }
  *response_payload = std::move(response.body);
  if (response.status_code < 200 || response.status_code >= 300) {
    if (error != nullptr) {
      *error = "remote endpoint returned HTTP " + std::to_string(response.status_code) +
               (response_payload->empty() ? std::string() : ": " + *response_payload);
    }
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool PostBinaryToEndpoint(std::string_view endpoint,
                          const BinaryRequestMetadata& metadata,
                          const std::vector<std::uint8_t>& payload,
                          std::string_view api_key,
                          int timeout_ms,
                          BinaryEndpointResponse* response,
                          std::string* error) {
  if (response == nullptr) {
    if (error != nullptr) {
      *error = "internal error: binary response output is null";
    }
    return false;
  }
  response->body.clear();
  response->width = 0;
  response->height = 0;
  response->elapsed_ms = 0.0F;
  response->resolved_model_id.clear();

  std::vector<SimpleHttpHeader> headers = {
      {.name = "Content-Type", .value = kBinaryContentType},
      {.name = "Accept", .value = kBinaryContentType},
      {.name = kHeaderModelId, .value = metadata.model_id},
      {.name = kHeaderQuality, .value = std::to_string(metadata.quality)},
      {.name = kHeaderResizeMode, .value = ResizeModeName(metadata.resize_mode)},
      {.name = kHeaderFrameHash, .value = std::to_string(metadata.frame_hash)},
      {.name = kHeaderWidth, .value = std::to_string(metadata.width)},
      {.name = kHeaderHeight, .value = std::to_string(metadata.height)},
      {.name = kHeaderChannels, .value = std::to_string(metadata.channels)},
  };
  if (!api_key.empty()) {
    headers.push_back({.name = "Authorization", .value = "Bearer " + std::string(api_key)});
  }

  const std::string payload_view(reinterpret_cast<const char*>(payload.data()), payload.size());
  SimpleHttpResponse http_response;
  if (!PerformSimpleHttpRequest(
          "POST", endpoint, headers, payload_view, timeout_ms, &http_response, error)) {
    return false;
  }

  response->body = std::move(http_response.body);
  if (http_response.status_code < 200 || http_response.status_code >= 300) {
    if (error != nullptr) {
      *error = "remote endpoint returned HTTP " + std::to_string(http_response.status_code) +
               (response->body.empty() ? std::string() : ": " + response->body);
    }
    return false;
  }

  std::string width_header;
  std::string height_header;
  std::string elapsed_header;
  std::string resolved_model_header;
  const bool width_ok = TryGetHttpHeaderValue(http_response, kHeaderResponseWidth, &width_header);
  const bool height_ok = TryGetHttpHeaderValue(http_response, kHeaderResponseHeight, &height_header);
  const bool elapsed_ok =
      TryGetHttpHeaderValue(http_response, kHeaderResponseElapsedMs, &elapsed_header);
  const bool resolved_model_ok =
      TryGetHttpHeaderValue(http_response, kHeaderResponseModelId, &resolved_model_header);
  if (!width_ok || !height_ok || !elapsed_ok || !resolved_model_ok) {
    if (error != nullptr) {
      *error = "remote endpoint omitted required depth response headers";
    }
    return false;
  }
  if (!ParsePositiveHeaderValue(width_header, &response->width, error) ||
      !ParsePositiveHeaderValue(height_header, &response->height, error) ||
      !ParseFiniteHeaderValue(elapsed_header, &response->elapsed_ms, error)) {
    return false;
  }
  response->resolved_model_id = resolved_model_header;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool GetEndpointText(std::string_view endpoint,
                     int timeout_ms,
                     std::string* response_payload,
                     std::string* error) {
  if (response_payload == nullptr) {
    if (error != nullptr) {
      *error = "internal error: response payload output is null";
    }
    return false;
  }

  SimpleHttpResponse response;
  if (!PerformSimpleHttpRequest("GET", endpoint, {}, {}, timeout_ms, &response, error)) {
    return false;
  }
  *response_payload = std::move(response.body);
  if (response.status_code < 200 || response.status_code >= 300) {
    if (error != nullptr) {
      *error = "remote endpoint returned HTTP " + std::to_string(response.status_code) +
               (response_payload->empty() ? std::string() : ": " + *response_payload);
    }
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

enum class PythonTorchCapability {
  kUnavailable,
  kTorchCpu,
  kTorchCuda,
  kTorchMps,
};

void PushUniquePythonCandidate(const std::filesystem::path& path,
                               std::vector<std::filesystem::path>* candidates) {
  if (candidates == nullptr || path.empty()) {
    return;
  }
  std::error_code status_error;
  if (!std::filesystem::is_regular_file(path, status_error) || status_error) {
    return;
  }
  if (::access(path.c_str(), X_OK) != 0) {
    return;
  }
  const auto normalized = path.lexically_normal();
  for (const auto& existing : *candidates) {
    if (existing.lexically_normal() == normalized) {
      return;
    }
  }
  candidates->push_back(normalized);
}

std::optional<std::filesystem::path> ResolveExecutableOnPath(const char* executable) {
  if (executable == nullptr || executable[0] == '\0') {
    return std::nullopt;
  }

  const std::string executable_name(executable);
  const std::string path_env = ReadEnvOrEmpty("PATH");
  std::size_t start = 0U;
  while (start <= path_env.size()) {
    const std::size_t delimiter = path_env.find(':', start);
    const std::string_view entry =
        delimiter == std::string::npos
            ? std::string_view(path_env).substr(start)
            : std::string_view(path_env).substr(start, delimiter - start);
    if (!entry.empty()) {
      const std::filesystem::path candidate =
          std::filesystem::path(std::string(entry)) / executable_name;
      if (::access(candidate.c_str(), X_OK) == 0) {
        return candidate;
      }
    }
    if (delimiter == std::string::npos) {
      break;
    }
    start = delimiter + 1U;
  }
  return std::nullopt;
}

std::vector<std::filesystem::path> GatherPythonAutostartCandidates(const RuntimeOptions& options) {
  std::vector<std::filesystem::path> candidates;
  if (!options.plugin_directory.empty()) {
    const std::filesystem::path plugin_dir(options.plugin_directory);
    for (const auto& root : BuildRuntimeAssetSearchRoots(
             plugin_dir, std::filesystem::path(options.runtime_asset_root))) {
      PushUniquePythonCandidate(root / "zsoda_py" / "bin" / "python3", &candidates);
      PushUniquePythonCandidate(root / "zsoda_py" / "bin" / "python", &candidates);
      PushUniquePythonCandidate(root / "zsoda_py" / "python" / "bin" / "python3", &candidates);
      PushUniquePythonCandidate(root / "zsoda_py" / "python" / "bin" / "python", &candidates);
    }
  }

  if (const auto python3 = ResolveExecutableOnPath("python3")) {
    PushUniquePythonCandidate(*python3, &candidates);
  }
  if (const auto python = ResolveExecutableOnPath("python")) {
    PushUniquePythonCandidate(*python, &candidates);
  }

  const std::array<std::filesystem::path, 4> known_candidates = {
      "/opt/homebrew/bin/python3",
      "/usr/local/bin/python3",
      "/usr/bin/python3",
      "/Library/Frameworks/Python.framework/Versions/Current/bin/python3",
  };
  for (const auto& candidate : known_candidates) {
    PushUniquePythonCandidate(candidate, &candidates);
  }
  return candidates;
}

PythonTorchCapability ProbePythonTorchCapability(const std::filesystem::path& python_path,
                                                 std::string* probe_output) {
  if (probe_output != nullptr) {
    probe_output->clear();
  }
  if (python_path.empty()) {
    return PythonTorchCapability::kUnavailable;
  }

  const std::string command = QuoteShellArgument(python_path.string()) + " -c " +
                              QuoteShellArgument(BuildPythonRuntimeProbeScript()) + " 2>&1";
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return PythonTorchCapability::kUnavailable;
  }

  std::string captured;
  std::array<char, 256> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    captured.append(buffer.data());
  }
  const int close_result = ::pclose(pipe);
  if (probe_output != nullptr) {
    *probe_output = captured;
  }
  if (close_result != 0) {
    return PythonTorchCapability::kUnavailable;
  }
  if (captured.find("MPS=1") != std::string::npos) {
    return PythonTorchCapability::kTorchMps;
  }
  if (captured.find("CUDA=1") != std::string::npos) {
    return PythonTorchCapability::kTorchCuda;
  }
  if (captured.find("CPU=1") != std::string::npos) {
    return PythonTorchCapability::kTorchCpu;
  }
  return PythonTorchCapability::kUnavailable;
}

std::string ResolvePythonCommandForAutostart(const RuntimeOptions& options) {
  if (!options.remote_service_python.empty()) {
    return options.remote_service_python;
  }

  try {
    const auto candidates = GatherPythonAutostartCandidates(options);
    std::filesystem::path mps_fallback;
    std::filesystem::path cuda_fallback;
    std::filesystem::path cpu_fallback;
    for (const auto& candidate : candidates) {
      std::string probe_output;
      const auto capability = ProbePythonTorchCapability(candidate, &probe_output);
      if (capability == PythonTorchCapability::kTorchMps) {
        return candidate.string();
      }
      if (capability == PythonTorchCapability::kTorchCuda && cuda_fallback.empty()) {
        cuda_fallback = candidate;
      }
      if (capability == PythonTorchCapability::kTorchCpu && cpu_fallback.empty()) {
        cpu_fallback = candidate;
      }
      if (capability != PythonTorchCapability::kUnavailable && mps_fallback.empty()) {
        mps_fallback = candidate;
      }
    }

    if (!cuda_fallback.empty()) {
      return cuda_fallback.string();
    }
    if (!cpu_fallback.empty()) {
      return cpu_fallback.string();
    }
    if (!mps_fallback.empty()) {
      return mps_fallback.string();
    }
  } catch (...) {
    // Autodiscovery must never break AE loader setup. Fall back to PATH python.
  }
  return "python3";
}

bool StartDetachedPythonService(const RuntimeOptions& options,
                                std::string_view status_endpoint,
                                std::string* error) {
  const std::filesystem::path script_path = ResolveRemoteServiceScriptPath(options);
  if (script_path.empty()) {
    if (error != nullptr) {
      *error = "remote service script was not found";
    }
    return false;
  }

  const std::string python_command = ResolvePythonCommandForAutostart(options);
  std::string python_probe_output;
  const auto python_capability =
      ProbePythonTorchCapability(std::filesystem::path(python_command), &python_probe_output);
  if (python_capability == PythonTorchCapability::kUnavailable) {
    if (error != nullptr) {
      *error = "remote inference python runtime is unavailable or missing required packages (python=" +
               python_command + ")" +
               (python_probe_output.empty()
                    ? std::string()
                    : ": " + SummarizePythonProbeOutput(python_probe_output));
    }
    return false;
  }
  const std::string host = NormalizeLoopbackHost(options.remote_service_host);
  const int port =
      options.remote_service_port > 0 ? options.remote_service_port : kDefaultLocalRemotePort;
  const std::string preload_model_id = ResolvePreloadModelId(options, script_path);
  const std::filesystem::path log_path = options.remote_service_log_path.empty()
                                             ? DefaultRemoteServiceLogPath()
                                             : std::filesystem::path(options.remote_service_log_path);
  {
    const std::string trace_detail =
        std::string("python=") + python_command + ", script=" + script_path.string();
    AppendRemoteTrace("service_autostart_python", trace_detail.c_str());
  }

  const auto log_parent = log_path.parent_path();
  if (!log_parent.empty()) {
    std::error_code create_error;
    std::filesystem::create_directories(log_parent, create_error);
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    if (error != nullptr) {
      *error = "fork failed for remote inference service";
    }
    return false;
  }

  if (pid == 0) {
    const std::filesystem::path working_directory = script_path.parent_path();
    if (!working_directory.empty()) {
      (void)::chdir(working_directory.c_str());
    }

    (void)::setsid();

    const int stdin_handle = ::open("/dev/null", O_RDONLY);
    if (stdin_handle >= 0) {
      (void)::dup2(stdin_handle, STDIN_FILENO);
      if (stdin_handle > STDERR_FILENO) {
        (void)::close(stdin_handle);
      }
    }

    const int log_handle = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_handle >= 0) {
      (void)::dup2(log_handle, STDOUT_FILENO);
      (void)::dup2(log_handle, STDERR_FILENO);
      if (log_handle > STDERR_FILENO) {
        (void)::close(log_handle);
      }
    }

    std::vector<std::string> args_storage;
    args_storage.reserve(preload_model_id.empty() ? 6U : 8U);
    args_storage.push_back(python_command);
    args_storage.push_back(script_path.string());
    args_storage.push_back("--host");
    args_storage.push_back(host);
    args_storage.push_back("--port");
    args_storage.push_back(std::to_string(port));
    if (!preload_model_id.empty()) {
      args_storage.push_back("--preload-model-id");
      args_storage.push_back(preload_model_id);
    }

    std::vector<char*> argv;
    argv.reserve(args_storage.size() + 1U);
    for (auto& value : args_storage) {
      argv.push_back(value.data());
    }
    argv.push_back(nullptr);
    ::execvp(python_command.c_str(), argv.data());
    std::fprintf(stderr, "execvp failed for remote inference service\n");
    _exit(127);
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  std::string status_payload;
  std::string status_error;
  while (std::chrono::steady_clock::now() < deadline) {
    if (GetEndpointText(status_endpoint, 2000, &status_payload, &status_error)) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  if (error != nullptr) {
    *error = "remote inference service did not become healthy at " + std::string(status_endpoint) +
             " (python=" + python_command + ", log=" + log_path.string() + ")" +
             (status_error.empty() ? std::string() : ": " + status_error);
    const std::string log_tail = SummarizePythonProbeOutput(ReadLogTail(log_path));
    if (!log_tail.empty()) {
      *error += " | service_log=" + log_tail;
    }
  }
  return false;
}
#endif

bool ReplaceToken(std::string* text, std::string_view token, std::string_view replacement) {
  if (text == nullptr || token.empty()) {
    return false;
  }
  bool replaced = false;
  std::size_t cursor = 0U;
  while (true) {
    const std::size_t pos = text->find(token, cursor);
    if (pos == std::string::npos) {
      break;
    }
    text->replace(pos, token.size(), replacement.data(), replacement.size());
    cursor = pos + replacement.size();
    replaced = true;
  }
  return replaced;
}

bool BuildCommandLine(const std::string& command_template,
                      const std::filesystem::path& request_path,
                      const std::filesystem::path& response_path,
                      std::string* out_command,
                      std::string* error) {
  if (out_command == nullptr) {
    if (error != nullptr) {
      *error = "internal error: command output is null";
    }
    return false;
  }
  if (command_template.empty()) {
    if (error != nullptr) {
      *error = "remote command is empty";
    }
    return false;
  }

  const std::string request_arg = QuoteShellArgument(request_path.string());
  const std::string response_arg = QuoteShellArgument(response_path.string());
  std::string command = command_template;
  const bool replaced_request = ReplaceToken(&command, "{request}", request_arg);
  const bool replaced_response = ReplaceToken(&command, "{response}", response_arg);
  if (!replaced_request) {
    command.push_back(' ');
    command.append(request_arg);
  }
  if (!replaced_response) {
    command.push_back(' ');
    command.append(response_arg);
  }

  *out_command = std::move(command);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

int NormalizeProcessExitCode(int raw_exit_code) {
#if defined(_WIN32)
  return raw_exit_code;
#else
  if (raw_exit_code == -1) {
    return -1;
  }
  if (WIFEXITED(raw_exit_code)) {
    return WEXITSTATUS(raw_exit_code);
  }
  if (WIFSIGNALED(raw_exit_code)) {
    return 128 + WTERMSIG(raw_exit_code);
  }
  return raw_exit_code;
#endif
}

bool IsJsonWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

void SkipJsonWhitespace(std::string_view text, std::size_t* cursor) {
  if (cursor == nullptr) {
    return;
  }
  while (*cursor < text.size() && IsJsonWhitespace(text[*cursor])) {
    ++(*cursor);
  }
}

bool FindMatchingDelimiter(std::string_view text,
                           std::size_t open_pos,
                           char open_char,
                           char close_char,
                           std::size_t* close_pos) {
  if (close_pos == nullptr || open_pos >= text.size() || text[open_pos] != open_char) {
    return false;
  }

  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t index = open_pos; index < text.size(); ++index) {
    const char ch = text[index];
    if (in_string) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == open_char) {
      ++depth;
      continue;
    }
    if (ch == close_char) {
      --depth;
      if (depth == 0) {
        *close_pos = index;
        return true;
      }
    }
  }
  return false;
}

bool FindJsonKeyValueOffset(std::string_view text,
                            std::string_view key,
                            std::size_t* out_value_offset) {
  if (out_value_offset == nullptr) {
    return false;
  }
  std::string quoted_key;
  quoted_key.reserve(key.size() + 2U);
  quoted_key.push_back('"');
  quoted_key.append(key);
  quoted_key.push_back('"');

  std::size_t search_from = 0U;
  while (true) {
    const std::size_t key_pos = text.find(quoted_key, search_from);
    if (key_pos == std::string::npos) {
      return false;
    }
    const std::size_t colon_pos = text.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos) {
      return false;
    }
    std::size_t value_offset = colon_pos + 1U;
    SkipJsonWhitespace(text, &value_offset);
    if (value_offset >= text.size()) {
      return false;
    }
    *out_value_offset = value_offset;
    return true;
  }
}

bool ParseBoolAtOffset(std::string_view text, std::size_t offset, bool* out_value) {
  if (out_value == nullptr) {
    return false;
  }
  std::size_t cursor = offset;
  SkipJsonWhitespace(text, &cursor);
  if (cursor + 4U <= text.size() && text.substr(cursor, 4U) == "true") {
    *out_value = true;
    return true;
  }
  if (cursor + 5U <= text.size() && text.substr(cursor, 5U) == "false") {
    *out_value = false;
    return true;
  }
  return false;
}

bool ParseIntAtOffset(std::string_view text,
                      std::size_t offset,
                      int* out_value,
                      std::string* error) {
  if (out_value == nullptr) {
    if (error != nullptr) {
      *error = "internal error: integer parse output is null";
    }
    return false;
  }
  std::size_t cursor = offset;
  SkipJsonWhitespace(text, &cursor);
  if (cursor >= text.size()) {
    if (error != nullptr) {
      *error = "failed to parse integer value: unexpected end of payload";
    }
    return false;
  }

  const std::size_t start = cursor;
  if (text[cursor] == '-' || text[cursor] == '+') {
    ++cursor;
  }
  const std::size_t digits_start = cursor;
  while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
    ++cursor;
  }
  if (digits_start == cursor) {
    if (error != nullptr) {
      *error = "failed to parse integer value";
    }
    return false;
  }

  const std::string token(text.substr(start, cursor - start));
  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(token.c_str(), &end, 10);
  if (errno != 0 || end == token.c_str() || (end != nullptr && *end != '\0') ||
      parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
    if (error != nullptr) {
      *error = "failed to parse integer value: " + token;
    }
    return false;
  }

  *out_value = static_cast<int>(parsed);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool ParseStringAtOffset(std::string_view text,
                         std::size_t offset,
                         std::string* out_value,
                         std::string* error) {
  if (out_value == nullptr) {
    if (error != nullptr) {
      *error = "internal error: string parse output is null";
    }
    return false;
  }
  std::size_t cursor = offset;
  SkipJsonWhitespace(text, &cursor);
  if (cursor >= text.size() || text[cursor] != '"') {
    if (error != nullptr) {
      *error = "failed to parse string value";
    }
    return false;
  }

  ++cursor;
  out_value->clear();
  bool escaped = false;
  while (cursor < text.size()) {
    const char ch = text[cursor++];
    if (escaped) {
      switch (ch) {
        case '\\':
        case '"':
        case '/':
          out_value->push_back(ch);
          break;
        case 'b':
          out_value->push_back('\b');
          break;
        case 'f':
          out_value->push_back('\f');
          break;
        case 'n':
          out_value->push_back('\n');
          break;
        case 'r':
          out_value->push_back('\r');
          break;
        case 't':
          out_value->push_back('\t');
          break;
        case 'u': {
          if (cursor + 4U > text.size()) {
            if (error != nullptr) {
              *error = "failed to parse unicode escape in response";
            }
            return false;
          }
          cursor += 4U;
          out_value->push_back('?');
          break;
        }
        default:
          out_value->push_back(ch);
          break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }
    out_value->push_back(ch);
  }

  if (error != nullptr) {
    *error = "failed to parse string value: unterminated quote";
  }
  return false;
}

bool ParseFloatArrayAtOffset(std::string_view text,
                             std::size_t offset,
                             std::vector<float>* out_values,
                             std::string* error) {
  if (out_values == nullptr) {
    if (error != nullptr) {
      *error = "internal error: float array output is null";
    }
    return false;
  }
  std::size_t cursor = offset;
  SkipJsonWhitespace(text, &cursor);
  if (cursor >= text.size() || text[cursor] != '[') {
    if (error != nullptr) {
      *error = "failed to parse float array: expected '['";
    }
    return false;
  }

  std::size_t array_end = 0U;
  if (!FindMatchingDelimiter(text, cursor, '[', ']', &array_end)) {
    if (error != nullptr) {
      *error = "failed to parse float array: unmatched bracket";
    }
    return false;
  }

  out_values->clear();
  std::size_t item_cursor = cursor + 1U;
  while (item_cursor < array_end) {
    while (item_cursor < array_end &&
           (IsJsonWhitespace(text[item_cursor]) || text[item_cursor] == ',')) {
      ++item_cursor;
    }
    if (item_cursor >= array_end) {
      break;
    }

    std::size_t token_end = item_cursor;
    while (token_end < array_end && text[token_end] != ',' &&
           !IsJsonWhitespace(text[token_end])) {
      ++token_end;
    }
    const std::string token(text.substr(item_cursor, token_end - item_cursor));
    if (token.empty()) {
      if (error != nullptr) {
        *error = "failed to parse float array: empty token";
      }
      return false;
    }

    errno = 0;
    char* parse_end = nullptr;
    const float parsed = std::strtof(token.c_str(), &parse_end);
    if (errno != 0 || parse_end == token.c_str() || (parse_end != nullptr && *parse_end != '\0')) {
      if (error != nullptr) {
        *error = "failed to parse float value in array: " + token;
      }
      return false;
    }
    out_values->push_back(SanitizeFinite(parsed));
    item_cursor = token_end;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

struct ParsedDepthResponse {
  int width = 0;
  int height = 0;
  std::vector<float> values;
};

bool ParseBinaryDepthResponse(const BinaryEndpointResponse& response,
                              ParsedDepthResponse* parsed,
                              std::string* error) {
  if (parsed == nullptr) {
    if (error != nullptr) {
      *error = "internal error: parsed response output is null";
    }
    return false;
  }
  if (response.width <= 0 || response.height <= 0) {
    if (error != nullptr) {
      *error = "binary response depth dimensions are invalid";
    }
    return false;
  }

  const std::size_t expected_values =
      static_cast<std::size_t>(response.width) * static_cast<std::size_t>(response.height);
  const std::size_t expected_bytes = expected_values * sizeof(float);
  if (response.body.size() != expected_bytes) {
    if (error != nullptr) {
      *error = "binary response payload size mismatch (expected=" +
               std::to_string(expected_bytes) + ", actual=" +
               std::to_string(response.body.size()) + ")";
    }
    return false;
  }

  parsed->width = response.width;
  parsed->height = response.height;
  parsed->values.resize(expected_values);
  if (expected_bytes > 0U) {
    std::memcpy(parsed->values.data(), response.body.data(), expected_bytes);
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool ParseDepthObject(std::string_view object,
                      ParsedDepthResponse* response,
                      std::string* error) {
  if (response == nullptr) {
    if (error != nullptr) {
      *error = "internal error: depth response output is null";
    }
    return false;
  }

  std::size_t width_offset = 0U;
  std::size_t height_offset = 0U;
  if (!FindJsonKeyValueOffset(object, "width", &width_offset) ||
      !FindJsonKeyValueOffset(object, "height", &height_offset)) {
    if (error != nullptr) {
      *error = "remote response is missing depth width/height";
    }
    return false;
  }
  if (!ParseIntAtOffset(object, width_offset, &response->width, error)) {
    return false;
  }
  if (!ParseIntAtOffset(object, height_offset, &response->height, error)) {
    return false;
  }

  std::size_t data_offset = 0U;
  if (!FindJsonKeyValueOffset(object, "data", &data_offset)) {
    if (!FindJsonKeyValueOffset(object, "values", &data_offset)) {
      if (error != nullptr) {
        *error = "remote response is missing depth data array";
      }
      return false;
    }
  }
  if (!ParseFloatArrayAtOffset(object, data_offset, &response->values, error)) {
    return false;
  }

  if (response->width <= 0 || response->height <= 0) {
    if (error != nullptr) {
      *error = "remote response depth dimensions are invalid";
    }
    return false;
  }
  const std::size_t expected =
      static_cast<std::size_t>(response->width) * static_cast<std::size_t>(response->height);
  if (response->values.size() != expected) {
    if (error != nullptr) {
      *error = "remote response depth payload size mismatch (expected=" +
               std::to_string(expected) + ", actual=" +
               std::to_string(response->values.size()) + ")";
    }
    return false;
  }
  return true;
}

bool ParseDepthResponsePayload(const std::string& payload,
                               ParsedDepthResponse* response,
                               std::string* error) {
  if (response == nullptr) {
    if (error != nullptr) {
      *error = "internal error: parsed response output is null";
    }
    return false;
  }
  if (payload.empty()) {
    if (error != nullptr) {
      *error = "remote response payload is empty";
    }
    return false;
  }

  std::size_t ok_offset = 0U;
  if (FindJsonKeyValueOffset(payload, "ok", &ok_offset)) {
    bool ok = true;
    if (!ParseBoolAtOffset(payload, ok_offset, &ok)) {
      if (error != nullptr) {
        *error = "remote response contains invalid 'ok' field";
      }
      return false;
    }
    if (!ok) {
      std::string remote_error;
      std::size_t error_offset = 0U;
      if (FindJsonKeyValueOffset(payload, "error", &error_offset)) {
        std::string parse_error;
        if (ParseStringAtOffset(payload, error_offset, &remote_error, &parse_error) &&
            !remote_error.empty()) {
          if (error != nullptr) {
            *error = "remote backend returned error: " + remote_error;
          }
          return false;
        }
      }
      if (error != nullptr) {
        *error = "remote backend returned error";
      }
      return false;
    }
  }

  std::size_t depth_offset = 0U;
  if (!FindJsonKeyValueOffset(payload, "depth", &depth_offset)) {
    if (error != nullptr) {
      *error = "remote response is missing 'depth' field";
    }
    return false;
  }
  if (depth_offset >= payload.size()) {
    if (error != nullptr) {
      *error = "remote response depth field is malformed";
    }
    return false;
  }

  if (payload[depth_offset] == '{') {
    std::size_t depth_end = 0U;
    if (!FindMatchingDelimiter(payload, depth_offset, '{', '}', &depth_end)) {
      if (error != nullptr) {
        *error = "remote response depth object is malformed";
      }
      return false;
    }
    return ParseDepthObject(
        std::string_view(payload).substr(depth_offset, depth_end - depth_offset + 1U),
        response,
        error);
  }

  if (payload[depth_offset] == '[') {
    std::size_t width_offset = 0U;
    std::size_t height_offset = 0U;
    if (!FindJsonKeyValueOffset(payload, "width", &width_offset) ||
        !FindJsonKeyValueOffset(payload, "height", &height_offset)) {
      if (error != nullptr) {
        *error = "remote response depth array requires top-level width/height";
      }
      return false;
    }
    if (!ParseIntAtOffset(payload, width_offset, &response->width, error)) {
      return false;
    }
    if (!ParseIntAtOffset(payload, height_offset, &response->height, error)) {
      return false;
    }
    if (!ParseFloatArrayAtOffset(payload, depth_offset, &response->values, error)) {
      return false;
    }
    const std::size_t expected =
        static_cast<std::size_t>(response->width) * static_cast<std::size_t>(response->height);
    if (response->values.size() != expected) {
      if (error != nullptr) {
        *error = "remote response depth payload size mismatch (expected=" +
                 std::to_string(expected) + ", actual=" +
                 std::to_string(response->values.size()) + ")";
      }
      return false;
    }
    return true;
  }

  if (error != nullptr) {
    *error = "remote response depth field must be an object or array";
  }
  return false;
}

bool WriteNormalizedDepthToOutput(const ParsedDepthResponse& response,
                                  const zsoda::core::FrameDesc& source_desc,
                                  zsoda::core::FrameBuffer* out_depth,
                                  std::string* error) {
  if (out_depth == nullptr) {
    if (error != nullptr) {
      *error = "internal error: output depth buffer is null";
    }
    return false;
  }
  if (source_desc.width <= 0 || source_desc.height <= 0) {
    if (error != nullptr) {
      *error = "invalid source descriptor for depth output";
    }
    return false;
  }
  if (response.width <= 0 || response.height <= 0 || response.values.empty()) {
    if (error != nullptr) {
      *error = "remote response depth buffer is empty";
    }
    return false;
  }

  float min_depth = std::numeric_limits<float>::infinity();
  float max_depth = -std::numeric_limits<float>::infinity();
  for (const float value : response.values) {
    min_depth = std::min(min_depth, value);
    max_depth = std::max(max_depth, value);
  }
  if (!std::isfinite(min_depth) || !std::isfinite(max_depth)) {
    if (error != nullptr) {
      *error = "remote response depth buffer contains non-finite values";
    }
    return false;
  }
  const float depth_range = max_depth - min_depth;

  zsoda::core::FrameDesc output_desc = source_desc;
  output_desc.channels = 1;
  output_desc.format = zsoda::core::PixelFormat::kGray32F;
  out_depth->Resize(output_desc);

  const auto sample_depth = [&](int x, int y) -> float {
    const std::size_t index =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(response.width) + x;
    return response.values[index];
  };

  for (int y = 0; y < output_desc.height; ++y) {
    const float src_y = (static_cast<float>(y) + 0.5F) *
                            (static_cast<float>(response.height) /
                             static_cast<float>(output_desc.height)) -
                        0.5F;
    for (int x = 0; x < output_desc.width; ++x) {
      const float src_x = (static_cast<float>(x) + 0.5F) *
                              (static_cast<float>(response.width) /
                               static_cast<float>(output_desc.width)) -
                          0.5F;

      const float clamped_x =
          std::clamp(src_x, 0.0F, static_cast<float>(std::max(0, response.width - 1)));
      const float clamped_y =
          std::clamp(src_y, 0.0F, static_cast<float>(std::max(0, response.height - 1)));
      const int x0 = static_cast<int>(clamped_x);
      const int y0 = static_cast<int>(clamped_y);
      const int x1 = std::min(x0 + 1, response.width - 1);
      const int y1 = std::min(y0 + 1, response.height - 1);
      const float tx = clamped_x - static_cast<float>(x0);
      const float ty = clamped_y - static_cast<float>(y0);

      const float p00 = sample_depth(x0, y0);
      const float p01 = sample_depth(x1, y0);
      const float p10 = sample_depth(x0, y1);
      const float p11 = sample_depth(x1, y1);
      const float top = p00 + (p01 - p00) * tx;
      const float bottom = p10 + (p11 - p10) * tx;
      const float sampled_depth = top + (bottom - top) * ty;
      float normalized = 0.0F;
      if (depth_range > std::numeric_limits<float>::epsilon()) {
        normalized = (sampled_depth - min_depth) / depth_range;
      } else {
        normalized = sampled_depth;
      }
      out_depth->at(x, y, 0) = std::clamp(normalized, 0.0F, 1.0F);
    }
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

}  // namespace

RemoteInferenceBackend::RemoteInferenceBackend(RuntimeOptions options,
                                               RemoteBackendCommandConfig command_config)
    : options_(std::move(options)), command_config_(std::move(command_config)) {
  active_backend_ = options_.preferred_backend == RuntimeBackend::kAuto
                        ? RuntimeBackend::kCpu
                        : options_.preferred_backend;
  backend_name_ = BuildBackendName(active_backend_);
}

const char* RemoteInferenceBackend::Name() const {
  return backend_name_.empty() ? "RemoteInferenceBackend" : backend_name_.c_str();
}

bool RemoteInferenceBackend::ResolveEndpointConfigurationLocked(std::string* error) {
  resolved_remote_endpoint_.clear();
  resolved_status_endpoint_.clear();
  service_autostart_enabled_ = false;

  if (!options_.remote_endpoint.empty()) {
    resolved_remote_endpoint_ = options_.remote_endpoint;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  service_autostart_enabled_ = options_.remote_service_autostart;
  if (service_autostart_enabled_) {
    const std::string host = NormalizeLoopbackHost(options_.remote_service_host);
    const int port =
        options_.remote_service_port > 0 ? options_.remote_service_port : kDefaultLocalRemotePort;
    resolved_remote_endpoint_ = BuildLocalInferenceEndpoint(host, port);
    resolved_status_endpoint_ = BuildLocalStatusEndpoint(host, port);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool RemoteInferenceBackend::EnsureAutoStartedServiceReadyLocked(std::string* error) {
  if (!service_autostart_enabled_ || resolved_remote_endpoint_.empty() ||
      resolved_status_endpoint_.empty()) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  std::string status_payload;
  std::string status_error;
  if (GetEndpointText(resolved_status_endpoint_, 2000, &status_payload, &status_error)) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  return StartDetachedPythonService(options_, resolved_status_endpoint_, error);
}

bool RemoteInferenceBackend::Initialize(std::string* error) {
  zsoda::core::CompatLockGuard lock(mutex_);
  if (initialized_) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  if (!ResolveEndpointConfigurationLocked(error)) {
    return false;
  }
  if (!EnsureAutoStartedServiceReadyLocked(error)) {
    return false;
  }

  if (!resolved_remote_endpoint_.empty()) {
    initialized_ = true;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  command_config_.command_template = ResolveCommandTemplate(command_config_);
  if (command_config_.command_template.empty()) {
    if (error != nullptr) {
      *error = "remote inference command is not configured "
               "(set ZSODA_REMOTE_INFERENCE_COMMAND)";
    }
    return false;
  }

  initialized_ = true;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool RemoteInferenceBackend::SelectModel(const std::string& model_id,
                                         const std::string& model_path,
                                         std::string* error) {
  zsoda::core::CompatLockGuard lock(mutex_);
  if (!initialized_) {
    if (error != nullptr) {
      *error = "remote backend is not initialized";
    }
    return false;
  }
  if (model_id.empty()) {
    if (error != nullptr) {
      *error = "remote backend model id cannot be empty";
    }
    return false;
  }

  active_model_id_ = model_id;
  active_model_path_ = model_path;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool RemoteInferenceBackend::Run(const InferenceRequest& request,
                                 zsoda::core::FrameBuffer* out_depth,
                                 std::string* error) const {
  const auto total_started = SteadyClock::now();
  std::string command_template;
  std::string model_id;
  std::string model_path;
  std::string backend_name;
  std::string remote_endpoint;
  std::string remote_api_key;
  int remote_timeout_ms = 0;
  RemoteTransportProtocol remote_protocol = RemoteTransportProtocol::kBinary;
  {
    zsoda::core::CompatLockGuard lock(mutex_);
    if (!initialized_) {
      if (error != nullptr) {
        *error = "remote backend is not initialized";
      }
      return false;
    }
    command_template = command_config_.command_template;
    model_id = active_model_id_;
    model_path = active_model_path_;
    backend_name = backend_name_;
    remote_endpoint = resolved_remote_endpoint_;
    remote_api_key = options_.remote_api_key;
    remote_timeout_ms = options_.remote_timeout_ms;
    remote_protocol = options_.remote_transport_protocol;
  }

  if (model_id.empty()) {
    if (error != nullptr) {
      *error = "remote backend has no active model";
    }
    return false;
  }
  if (command_template.empty() && remote_endpoint.empty()) {
    if (error != nullptr) {
      *error = "remote backend has neither command nor endpoint configured";
    }
    return false;
  }
  if (!ValidateRequest(request, out_depth, error)) {
    return false;
  }

  std::string response_payload;
  ParsedDepthResponse parsed_response;
  RemoteTimingBreakdown timings;

  if (!remote_endpoint.empty() && remote_protocol == RemoteTransportProtocol::kBinary) {
    const auto convert_started = SteadyClock::now();
    const std::vector<std::uint8_t> rgb_payload = EncodeRgb8Payload(*request.source);
    timings.host_convert_ms = ElapsedMilliseconds(convert_started);

    const auto encode_started = SteadyClock::now();
    BinaryRequestMetadata metadata;
    metadata.model_id = model_id;
    metadata.quality = request.quality;
    metadata.resize_mode = request.resize_mode;
    metadata.frame_hash = request.frame_hash;
    metadata.width = request.source->desc().width;
    metadata.height = request.source->desc().height;
    metadata.channels = 3;
    timings.request_encode_ms = ElapsedMilliseconds(encode_started);

    BinaryEndpointResponse binary_response;
    if (!PostBinaryToEndpoint(remote_endpoint,
                              metadata,
                              rgb_payload,
                              remote_api_key,
                              remote_timeout_ms,
                              &binary_response,
                              error)) {
      if (error != nullptr && !error->empty()) {
        *error = backend_name + ": " + *error;
      }
      return false;
    }
    timings.service_ms = std::max(0.0F, binary_response.elapsed_ms);

    const auto decode_started = SteadyClock::now();
    if (!ParseBinaryDepthResponse(binary_response, &parsed_response, error)) {
      if (error != nullptr && !error->empty()) {
        *error = backend_name + ": " + *error;
      }
      return false;
    }
    timings.response_decode_ms = ElapsedMilliseconds(decode_started);
  } else {
    std::filesystem::path source_path;
    if (!MakeUniqueTempFilePath("zsoda-remote-source", ".ppm", &source_path, error)) {
      return false;
    }
    std::filesystem::path request_path;
    if (!MakeUniqueTempFilePath("zsoda-remote-request", ".json", &request_path, error)) {
      return false;
    }
    std::filesystem::path response_path;
    if (!MakeUniqueTempFilePath("zsoda-remote-response", ".json", &response_path, error)) {
      return false;
    }
    ScopedTempFile source_file(source_path);
    ScopedTempFile request_file(request_path);
    ScopedTempFile response_file(response_path);

    const auto convert_started = SteadyClock::now();
    if (!WritePortablePixmap(*request.source, source_file.path(), error)) {
      return false;
    }
    timings.host_convert_ms = ElapsedMilliseconds(convert_started);

    const auto encode_started = SteadyClock::now();
    std::string request_payload;
    if (!SerializeRequestPayload(
            request, model_id, model_path, source_file.path().string(), &request_payload, error)) {
      return false;
    }
    if (!WriteTextFile(request_file.path(), request_payload, error)) {
      return false;
    }
    timings.request_encode_ms = ElapsedMilliseconds(encode_started);
    std::error_code remove_error;
    std::filesystem::remove(response_file.path(), remove_error);

    std::string command_line;
    if (!remote_endpoint.empty()) {
      const auto service_started = SteadyClock::now();
      if (!PostJsonToEndpoint(remote_endpoint,
                              request_payload,
                              remote_api_key,
                              remote_timeout_ms,
                              &response_payload,
                              error)) {
        if (error != nullptr && !error->empty()) {
          *error = backend_name + ": " + *error;
        }
        return false;
      }
      timings.service_ms = ElapsedMilliseconds(service_started);
    } else {
      if (!BuildCommandLine(
              command_template, request_file.path(), response_file.path(), &command_line, error)) {
        return false;
      }

      const auto service_started = SteadyClock::now();
      const int raw_exit_code = std::system(command_line.c_str());
      timings.service_ms = ElapsedMilliseconds(service_started);
      if (raw_exit_code == -1) {
        if (error != nullptr) {
          *error = backend_name + ": failed to invoke remote command";
        }
        return false;
      }
      const int exit_code = NormalizeProcessExitCode(raw_exit_code);
      if (exit_code != 0) {
        if (error != nullptr) {
          *error = backend_name + ": remote command failed (exit_code=" +
                   std::to_string(exit_code) + ")";
        }
        return false;
      }

      if (!ReadTextFile(response_file.path(), &response_payload, error)) {
        return false;
      }
    }
    const auto decode_started = SteadyClock::now();
    if (!ParseDepthResponsePayload(response_payload, &parsed_response, error)) {
      return false;
    }
    timings.response_decode_ms = ElapsedMilliseconds(decode_started);
  }

  if (!WriteNormalizedDepthToOutput(parsed_response, request.source->desc(), out_depth, error)) {
    return false;
  }

  timings.total_ms = ElapsedMilliseconds(total_started);
  if (RemoteTimingTraceEnabled()) {
    std::ostringstream detail;
    detail.setf(std::ios::fixed);
    detail.precision(2);
    detail << "model=" << model_id << ", protocol=" << RemoteTransportProtocolName(remote_protocol)
           << ", host_convert_ms=" << timings.host_convert_ms
           << ", request_encode_ms=" << timings.request_encode_ms
           << ", service_ms=" << timings.service_ms
           << ", response_decode_ms=" << timings.response_decode_ms
           << ", total_ms=" << timings.total_ms;
    AppendRemoteTrace("run_timing", detail.str().c_str());
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

RuntimeBackend RemoteInferenceBackend::ActiveBackend() const {
  zsoda::core::CompatLockGuard lock(mutex_);
  return active_backend_;
}

std::unique_ptr<IOnnxRuntimeBackend> CreateRemoteInferenceBackendWithCommand(
    const RuntimeOptions& options,
    RemoteBackendCommandConfig command_config,
    std::string* error) {
  auto backend = std::make_unique<RemoteInferenceBackend>(options, std::move(command_config));
  std::string initialize_error;
  if (!backend->Initialize(&initialize_error)) {
    if (error != nullptr) {
      *error = initialize_error;
    }
    return nullptr;
  }
  if (error != nullptr) {
    error->clear();
  }
  return backend;
}

std::unique_ptr<IOnnxRuntimeBackend> CreateRemoteInferenceBackend(const RuntimeOptions& options,
                                                                  std::string* error) {
  return CreateRemoteInferenceBackendWithCommand(options, {}, error);
}

}  // namespace zsoda::inference
