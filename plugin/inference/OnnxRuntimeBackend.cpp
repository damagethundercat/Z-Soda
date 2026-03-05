#include "inference/OnnxRuntimeBackend.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#undef ORT_API_MANUAL_INIT
#include "inference/OrtDynamicLoader.h"
#endif

namespace zsoda::inference {
namespace {

constexpr int kMinimumModelInputSize = 32;
constexpr std::uint32_t kDefaultOrtApiVersionFloor = 17U;
constexpr int kDefaultGpuDeviceId = 0;

struct RuntimeProviderCapabilities {
  std::vector<std::string> providers;
  bool has_cpu = false;
  bool has_cuda = false;
  bool has_tensorrt = false;
  bool has_directml = false;
  bool has_coreml = false;
};

bool ParseBoolEnvOrDefault(const char* name, bool default_value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return default_value;
  }
  std::string normalized;
  while (*raw != '\0') {
    const char ch = *raw++;
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '-' || ch == '_') {
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

int ParseIntEnvOrDefault(const char* name, int default_value, int min_value, int max_value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return default_value;
  }
  char* end = nullptr;
  const long parsed = std::strtol(raw, &end, 10);
  if (end == raw || (end != nullptr && *end != '\0')) {
    return default_value;
  }
  if (parsed < static_cast<long>(min_value) || parsed > static_cast<long>(max_value)) {
    return default_value;
  }
  return static_cast<int>(parsed);
}

float ParseFloatEnvOrDefault(const char* name,
                             float default_value,
                             float min_value,
                             float max_value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return default_value;
  }
  char* end = nullptr;
  const float parsed = std::strtof(raw, &end);
  if (end == raw || (end != nullptr && *end != '\0')) {
    return default_value;
  }
  if (!std::isfinite(parsed) || parsed < min_value || parsed > max_value) {
    return default_value;
  }
  return parsed;
}

bool ShouldPreferPreloadedOrt(RuntimeBackend requested_backend) {
  const bool prefer_gpu_when_auto = true;
  const bool default_prefer_preloaded =
      (requested_backend != RuntimeBackend::kCpu) ||
      (requested_backend == RuntimeBackend::kAuto && prefer_gpu_when_auto);
  return ParseBoolEnvOrDefault("ZSODA_ORT_PREFER_PRELOADED", default_prefer_preloaded);
}

std::string NormalizeProviderName(std::string_view provider_name) {
  std::string normalized;
  normalized.reserve(provider_name.size());
  for (const char ch : provider_name) {
    if (std::isalnum(static_cast<unsigned char>(ch)) == 0) {
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return normalized;
}

bool ProviderNameMatches(const std::string& normalized_name, std::string_view target) {
  return normalized_name == NormalizeProviderName(target);
}

RuntimeBackend SelectBestBackendForAuto(const RuntimeProviderCapabilities& capabilities) {
  if (capabilities.has_tensorrt) {
    return RuntimeBackend::kTensorRT;
  }
  if (capabilities.has_cuda) {
    return RuntimeBackend::kCuda;
  }
  if (capabilities.has_directml) {
    return RuntimeBackend::kDirectML;
  }
  if (capabilities.has_coreml) {
    return RuntimeBackend::kCoreML;
  }
  return RuntimeBackend::kCpu;
}

bool SupportsRequestedBackend(const RuntimeProviderCapabilities& capabilities,
                              RuntimeBackend backend) {
  switch (backend) {
    case RuntimeBackend::kCpu:
      return capabilities.has_cpu;
    case RuntimeBackend::kTensorRT:
      return capabilities.has_tensorrt;
    case RuntimeBackend::kCuda:
      return capabilities.has_cuda;
    case RuntimeBackend::kDirectML:
      return capabilities.has_directml;
    case RuntimeBackend::kCoreML:
      return capabilities.has_coreml;
    case RuntimeBackend::kAuto:
      return true;
    case RuntimeBackend::kMetal:
    case RuntimeBackend::kRemote:
      return false;
  }
  return false;
}

RuntimeBackend SelectActiveBackend(RuntimeBackend requested_backend,
                                   const RuntimeProviderCapabilities& capabilities,
                                   std::string* selection_note) {
  if (selection_note != nullptr) {
    selection_note->clear();
  }

  if (requested_backend == RuntimeBackend::kAuto) {
    const RuntimeBackend selected = SelectBestBackendForAuto(capabilities);
    if (selection_note != nullptr && selected != RuntimeBackend::kCpu) {
      *selection_note = "auto-selected provider=" + std::string(RuntimeBackendName(selected));
    }
    return selected;
  }

  if (SupportsRequestedBackend(capabilities, requested_backend)) {
    return requested_backend;
  }

  const RuntimeBackend fallback = SelectBestBackendForAuto(capabilities);
  if (selection_note != nullptr) {
    *selection_note = "requested provider unavailable: " +
                      std::string(RuntimeBackendName(requested_backend)) +
                      ", fallback=" + RuntimeBackendName(fallback);
  }
  return fallback;
}

struct ModelPipelineProfile {
  int input_width = 384;
  int input_height = 384;
  int input_frame_count = 1;
  std::array<float, 3> normalize_mean = {0.5F, 0.5F, 0.5F};
  std::array<float, 3> normalize_std = {0.5F, 0.5F, 0.5F};
  bool invert_depth = false;
  bool prefer_latest_output_map = false;
};

struct PreparedModelInput {
  int source_width = 0;
  int source_height = 0;
  int tensor_width = 0;
  int tensor_height = 0;
  int tensor_channels = 3;
  float resize_scale = 1.0F;
  float resize_offset_x = 0.0F;
  float resize_offset_y = 0.0F;
  PreprocessResizeMode resize_mode = PreprocessResizeMode::kUpperBoundLetterbox;
  float normalized_min = 0.0F;
  float normalized_max = 0.0F;
  float normalized_mean = 0.0F;
  std::vector<float> nchw_values;
};

struct RawDepthOutput {
  int width = 0;
  int height = 0;
  float min_depth = 0.0F;
  float max_depth = 1.0F;
  std::vector<float> depth_values;
};

std::string ToLowerCopy(std::string_view text) {
  std::string lowered;
  lowered.reserve(text.size());
  for (const char ch : text) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered;
}

std::string ReadEnvOrEmpty(const char* name) {
  if (name == nullptr || name[0] == '\0') {
    return {};
  }
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return {};
  }
  return std::string(value);
}

int ScoreOutputNameForDepth(std::string_view output_name) {
  const std::string lowered = ToLowerCopy(output_name);
  int score = 0;
  if (lowered.find("depth") != std::string::npos) {
    score += 80;
  }
  if (lowered.find("pred") != std::string::npos ||
      lowered.find("prediction") != std::string::npos) {
    score += 30;
  }
  if (lowered.find("disp") != std::string::npos ||
      lowered.find("disparity") != std::string::npos) {
    score += 20;
  }
  if (lowered == "output" || lowered.find("out") != std::string::npos) {
    score += 5;
  }
  if (lowered.find("conf") != std::string::npos ||
      lowered.find("confidence") != std::string::npos ||
      lowered.find("uncert") != std::string::npos ||
      lowered.find("sigma") != std::string::npos ||
      lowered.find("variance") != std::string::npos ||
      lowered.find("var") != std::string::npos ||
      lowered.find("mask") != std::string::npos ||
      lowered.find("prob") != std::string::npos) {
    score -= 120;
  }
  return score;
}

std::size_t SelectDepthOutputIndexByName(const std::vector<std::string>& output_names) {
  if (output_names.empty()) {
    return 0U;
  }

  std::size_t best_index = 0U;
  int best_score = std::numeric_limits<int>::min();
  for (std::size_t i = 0U; i < output_names.size(); ++i) {
    const int score = ScoreOutputNameForDepth(output_names[i]);
    if (score > best_score) {
      best_score = score;
      best_index = i;
    }
  }
  return best_index;
}

std::size_t ResolveOutputMapIndex(std::size_t preferred_map_index,
                                  std::size_t map_count,
                                  int temporal_frame_count_hint) {
  const int forced_output_map_index =
      ParseIntEnvOrDefault("ZSODA_ORT_OUTPUT_MAP_INDEX", -1, -1, 4096);
  if (forced_output_map_index >= 0) {
    return static_cast<std::size_t>(forced_output_map_index);
  }

  if (preferred_map_index != std::numeric_limits<std::size_t>::max()) {
    return preferred_map_index;
  }

  if (temporal_frame_count_hint > 1 &&
      map_count == static_cast<std::size_t>(temporal_frame_count_hint)) {
    return map_count - 1U;
  }
  return 0U;
}

const char* SafeCStr(const char* value, const char* fallback = "<null>") {
  return value != nullptr ? value : fallback;
}

void AppendOrtTrace(const char* stage, const char* detail = nullptr) {
#if defined(_WIN32)
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
               "%04u-%02u-%02u %02u:%02u:%02u.%03u | OrtTrace | tid=%lu, stage=%s, detail=%s\r\n",
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

std::string BuildBackendName(RuntimeBackend active_backend,
                             std::string_view runtime_version = {},
                             std::string_view runtime_library_path = {},
                             std::string_view provider_note = {}) {
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
  std::string name = "OnnxRuntimeBackend[";
#else
  std::string name = "OnnxRuntimeBackendScaffold[";
#endif
  name.append(RuntimeBackendName(active_backend));
  name.push_back(']');
  if (!runtime_version.empty() || !runtime_library_path.empty() || !provider_note.empty()) {
    name.append(" (");
    bool has_segment = false;
    if (!runtime_version.empty()) {
      name.append("runtime=");
      name.append(runtime_version);
      has_segment = true;
    }
    if (!runtime_library_path.empty()) {
      if (has_segment) {
        name.append(", ");
      }
      name.append("library=");
      name.append(runtime_library_path);
      has_segment = true;
    }
    if (!provider_note.empty()) {
      if (has_segment) {
        name.append(", ");
      }
      name.append(provider_note);
    }
    name.push_back(')');
  }
  return name;
}

std::string BuildProviderNote(RuntimeBackend requested_backend,
                              RuntimeBackend active_backend,
                              std::string_view selection_note = {}) {
  std::string note;
  if (requested_backend != RuntimeBackend::kAuto || active_backend != RuntimeBackend::kCpu) {
    note = "provider_request=";
    note.append(RuntimeBackendName(requested_backend));
    note.append("->");
    note.append(RuntimeBackendName(active_backend));
  }
  if (!selection_note.empty()) {
    if (!note.empty()) {
      note.append("; ");
    }
    note.append(selection_note);
  }
  return note;
}

bool ModelIdIndicatesMultiview(std::string_view model_id) {
  constexpr std::string_view kVideoDepthPrefix = "video-depth-anything";
  return model_id.find("multiview") != std::string_view::npos ||
         model_id.rfind(kVideoDepthPrefix, 0) == 0;
}

void AppendNoteSegment(std::string* note, std::string_view segment) {
  if (note == nullptr || segment.empty()) {
    return;
  }
  if (!note->empty()) {
    note->append("; ");
  }
  note->append(segment);
}

std::string BuildMultiviewDiagnosticsNote(bool requested_multiview,
                                          int resolved_input_frame_count,
                                          bool multiview_active,
                                          bool multiview_warning) {
  std::ostringstream oss;
  oss << "requested_multiview=" << (requested_multiview ? "1" : "0")
      << ", resolved_input_frame_count=" << std::max(1, resolved_input_frame_count)
      << ", multiview_active=" << (multiview_active ? "1" : "0");
  if (multiview_warning) {
    oss << ", warning=multiview_model_degraded_to_single_frame";
  }
  return oss.str();
}

float LinearToSrgb(float linear) {
  const float clamped = std::clamp(linear, 0.0F, 1.0F);
  if (clamped <= 0.0031308F) {
    return clamped * 12.92F;
  }
  return 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
}

bool UseImagenetNormalizationForDa3() {
  return ParseBoolEnvOrDefault("ZSODA_DA3_IMAGENET_NORM", true);
}

bool IsDa3MultiviewModelId(std::string_view model_id) {
  return model_id.rfind("depth-anything-v3", 0) == 0 &&
         model_id.find("multiview") != std::string_view::npos;
}

int ResolveDa3MultiviewFrameCount(std::string_view model_id) {
  const int default_frames = IsDa3MultiviewModelId(model_id) ? 5 : 1;
  return ParseIntEnvOrDefault("ZSODA_DA3_MULTIVIEW_FRAMES", default_frames, 1, 128);
}

float ComputeSampledMeanAbsDiff(const float* lhs, const float* rhs, std::size_t count) {
  if (lhs == nullptr || rhs == nullptr || count == 0U) {
    return 1.0F;
  }
  constexpr std::size_t kMaxSamples = 65536U;
  const std::size_t stride = std::max<std::size_t>(1U, count / kMaxSamples);
  std::size_t sampled = 0U;
  float total = 0.0F;
  for (std::size_t i = 0U; i < count; i += stride) {
    total += std::fabs(lhs[i] - rhs[i]);
    ++sampled;
  }
  if (sampled == 0U) {
    return 0.0F;
  }
  return total / static_cast<float>(sampled);
}

#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
std::string JoinProviders(const std::vector<std::string>& providers) {
  if (providers.empty()) {
    return "<none>";
  }
  std::ostringstream oss;
  for (std::size_t i = 0; i < providers.size(); ++i) {
    if (i > 0U) {
      oss << "|";
    }
    oss << providers[i];
  }
  return oss.str();
}

std::string ConsumeOrtStatusMessage(const OrtApi* api, OrtStatus* status) {
  if (api == nullptr || status == nullptr) {
    return "unknown ORT status error";
  }
  const char* raw = api->GetErrorMessage != nullptr ? api->GetErrorMessage(status) : nullptr;
  std::string message = (raw != nullptr && raw[0] != '\0') ? raw : "unknown ORT status error";
  if (api->ReleaseStatus != nullptr) {
    api->ReleaseStatus(status);
  }
  return message;
}

bool QueryRuntimeCapabilities(const OrtApi* api,
                              RuntimeProviderCapabilities* capabilities,
                              std::string* capability_note,
                              std::string* error) {
  if (capabilities != nullptr) {
    *capabilities = {};
  }
  if (capability_note != nullptr) {
    capability_note->clear();
  }
  if (api == nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime capability probe failed: api pointer is null";
    }
    return false;
  }

  char** providers_raw = nullptr;
  int provider_length = 0;
  OrtStatus* providers_status = api->GetAvailableProviders != nullptr
                                    ? api->GetAvailableProviders(&providers_raw, &provider_length)
                                    : nullptr;
  if (providers_status != nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime capability probe failed: GetAvailableProviders failed: " +
               ConsumeOrtStatusMessage(api, providers_status);
    }
    return false;
  }

  RuntimeProviderCapabilities local_capabilities;
  local_capabilities.providers.reserve(provider_length > 0 ? static_cast<std::size_t>(provider_length)
                                                           : 0U);
  for (int i = 0; i < provider_length; ++i) {
    const char* provider = (providers_raw != nullptr) ? providers_raw[i] : nullptr;
    const std::string provider_name = SafeCStr(provider, "<null provider>");
    local_capabilities.providers.push_back(provider_name);
    const std::string normalized = NormalizeProviderName(provider_name);
    if (ProviderNameMatches(normalized, "CPUExecutionProvider") || ProviderNameMatches(normalized, "CPU")) {
      local_capabilities.has_cpu = true;
    }
    if (ProviderNameMatches(normalized, "CUDAExecutionProvider") || ProviderNameMatches(normalized, "CUDA")) {
      local_capabilities.has_cuda = true;
    }
    if (ProviderNameMatches(normalized, "TensorrtExecutionProvider") ||
        ProviderNameMatches(normalized, "TensorRTExecutionProvider") ||
        ProviderNameMatches(normalized, "TensorRT") ||
        ProviderNameMatches(normalized, "TRT")) {
      local_capabilities.has_tensorrt = true;
    }
    if (ProviderNameMatches(normalized, "DmlExecutionProvider") ||
        ProviderNameMatches(normalized, "DirectMLExecutionProvider") ||
        ProviderNameMatches(normalized, "DML")) {
      local_capabilities.has_directml = true;
    }
    if (ProviderNameMatches(normalized, "CoreMLExecutionProvider") ||
        ProviderNameMatches(normalized, "CoreML")) {
      local_capabilities.has_coreml = true;
    }
  }

  if (api->ReleaseAvailableProviders != nullptr) {
    api->ReleaseAvailableProviders(providers_raw, provider_length);
  }

  if (!local_capabilities.has_cpu) {
    if (error != nullptr) {
      *error = "onnx runtime capability probe failed: CPUExecutionProvider is unavailable";
    }
    return false;
  }

  std::ostringstream capability;
  capability << "providers=" << JoinProviders(local_capabilities.providers);
  if (api->GetBuildInfoString != nullptr) {
    const char* build_info = api->GetBuildInfoString();
    if (build_info != nullptr && build_info[0] != '\0') {
      capability << ", build_info=" << build_info;
    }
  }

  if (capabilities != nullptr) {
    *capabilities = std::move(local_capabilities);
  }
  if (capability_note != nullptr) {
    *capability_note = capability.str();
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool AppendCudaExecutionProvider(const OrtApi* api,
                                 OrtSessionOptions* session_options,
                                 std::string* detail,
                                 std::string* error) {
  if (api == nullptr || session_options == nullptr) {
    if (error != nullptr) {
      *error = "internal error: CUDA EP append arguments are null";
    }
    return false;
  }
  if (api->SessionOptionsAppendExecutionProvider_CUDA == nullptr) {
    if (error != nullptr) {
      *error = "CUDA execution provider append API is unavailable in this runtime";
    }
    return false;
  }

  OrtCUDAProviderOptions options;
  options.device_id = ParseIntEnvOrDefault("ZSODA_ORT_CUDA_DEVICE_ID", kDefaultGpuDeviceId, 0, 64);
  OrtStatus* status = api->SessionOptionsAppendExecutionProvider_CUDA(session_options, &options);
  if (status != nullptr) {
    if (error != nullptr) {
      *error = "CUDA EP append failed: " + ConsumeOrtStatusMessage(api, status);
    }
    return false;
  }
  if (detail != nullptr) {
    *detail = "cuda(device_id=" + std::to_string(options.device_id) + ")";
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool AppendTensorRtExecutionProvider(const OrtApi* api,
                                     OrtSessionOptions* session_options,
                                     std::string* detail,
                                     std::string* error) {
  if (api == nullptr || session_options == nullptr) {
    if (error != nullptr) {
      *error = "internal error: TensorRT EP append arguments are null";
    }
    return false;
  }
  if (api->SessionOptionsAppendExecutionProvider_TensorRT == nullptr) {
    if (error != nullptr) {
      *error = "TensorRT execution provider append API is unavailable in this runtime";
    }
    return false;
  }

  OrtTensorRTProviderOptions options{};
  options.device_id = ParseIntEnvOrDefault("ZSODA_ORT_TRT_DEVICE_ID", kDefaultGpuDeviceId, 0, 64);
  options.trt_max_partition_iterations = 1000;
  options.trt_min_subgraph_size = 1;
  options.trt_max_workspace_size = static_cast<std::size_t>(1) << 30;
  options.trt_fp16_enable = ParseBoolEnvOrDefault("ZSODA_ORT_TRT_FP16", true) ? 1 : 0;
  options.trt_engine_cache_enable = ParseBoolEnvOrDefault("ZSODA_ORT_TRT_ENGINE_CACHE", true) ? 1 : 0;
  options.trt_engine_cache_path = nullptr;

  const std::string engine_cache_path = []() -> std::string {
    const char* raw = std::getenv("ZSODA_ORT_TRT_ENGINE_CACHE_PATH");
    if (raw == nullptr || raw[0] == '\0') {
      return {};
    }
    return std::string(raw);
  }();
  if (!engine_cache_path.empty()) {
    options.trt_engine_cache_path = engine_cache_path.c_str();
  }

  OrtStatus* status = api->SessionOptionsAppendExecutionProvider_TensorRT(session_options, &options);
  if (status != nullptr) {
    if (error != nullptr) {
      *error = "TensorRT EP append failed: " + ConsumeOrtStatusMessage(api, status);
    }
    return false;
  }
  if (detail != nullptr) {
    *detail = "tensorrt(device_id=" + std::to_string(options.device_id) +
              ", fp16=" + std::string(options.trt_fp16_enable != 0 ? "1" : "0") + ")";
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool AppendCoreMlExecutionProvider(const OrtApi* api,
                                   OrtSessionOptions* session_options,
                                   std::string* detail,
                                   std::string* error) {
  if (api == nullptr || session_options == nullptr) {
    if (error != nullptr) {
      *error = "internal error: CoreML EP append arguments are null";
    }
    return false;
  }
  if (api->SessionOptionsAppendExecutionProvider == nullptr) {
    if (error != nullptr) {
      *error = "generic execution provider append API is unavailable";
    }
    return false;
  }

  OrtStatus* status = api->SessionOptionsAppendExecutionProvider(
      session_options, "CoreML", nullptr, nullptr, 0U);
  if (status != nullptr) {
    if (error != nullptr) {
      *error = "CoreML EP append failed: " + ConsumeOrtStatusMessage(api, status);
    }
    return false;
  }
  if (detail != nullptr) {
    *detail = "coreml";
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

#if defined(_WIN32)
using OrtSessionOptionsAppendExecutionProviderDmlFn =
    OrtStatus* (ORT_API_CALL*)(OrtSessionOptions* options, int device_id);

bool AppendDirectMlExecutionProvider(const OrtApi* api,
                                     void* module_handle,
                                     OrtSessionOptions* session_options,
                                     std::string* detail,
                                     std::string* error) {
  if (api == nullptr || session_options == nullptr) {
    if (error != nullptr) {
      *error = "internal error: DirectML EP append arguments are null";
    }
    return false;
  }
  HMODULE module = reinterpret_cast<HMODULE>(module_handle);
  if (module == nullptr) {
    if (error != nullptr) {
      *error = "DirectML EP append failed: ORT module handle is null";
    }
    return false;
  }

  const int device_id =
      ParseIntEnvOrDefault("ZSODA_ORT_DML_DEVICE_ID", kDefaultGpuDeviceId, 0, 64);
  FARPROC symbol = ::GetProcAddress(module, "OrtSessionOptionsAppendExecutionProvider_DML");
  if (symbol != nullptr) {
    const auto append_dml =
        reinterpret_cast<OrtSessionOptionsAppendExecutionProviderDmlFn>(symbol);
    OrtStatus* status = append_dml(session_options, device_id);
    if (status == nullptr) {
      if (detail != nullptr) {
        *detail = "directml(device_id=" + std::to_string(device_id) + ")";
      }
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }
    if (error != nullptr) {
      *error = "DirectML EP append failed: " + ConsumeOrtStatusMessage(api, status);
    }
    return false;
  }

  if (api->SessionOptionsAppendExecutionProvider != nullptr) {
    const char* provider_names[] = {"DML", "DmlExecutionProvider", "DirectMLExecutionProvider"};
    for (const char* provider_name : provider_names) {
      OrtStatus* status = api->SessionOptionsAppendExecutionProvider(
          session_options, provider_name, nullptr, nullptr, 0U);
      if (status == nullptr) {
        if (detail != nullptr) {
          *detail = "directml(device_id=" + std::to_string(device_id) + ")";
        }
        if (error != nullptr) {
          error->clear();
        }
        return true;
      }
      const std::string status_message = ConsumeOrtStatusMessage(api, status);
      if (error != nullptr) {
        *error = "DirectML generic append failed (" + std::string(provider_name) +
                 "): " + status_message;
      }
    }
  }

  if (error != nullptr) {
    *error =
        "DirectML EP append failed: required symbol OrtSessionOptionsAppendExecutionProvider_DML is "
        "not exported";
  }
  return false;
}
#endif

bool ConfigureExecutionProvider(const OrtApi* api,
                                void* module_handle,
                                RuntimeBackend backend,
                                OrtSessionOptions* session_options,
                                std::string* applied_detail,
                                std::string* error) {
  if (applied_detail != nullptr) {
    applied_detail->clear();
  }
  if (error != nullptr) {
    error->clear();
  }

  if (backend == RuntimeBackend::kCpu) {
    if (applied_detail != nullptr) {
      *applied_detail = "cpu";
    }
    return true;
  }

  switch (backend) {
    case RuntimeBackend::kTensorRT:
      return AppendTensorRtExecutionProvider(api, session_options, applied_detail, error);
    case RuntimeBackend::kCuda:
      return AppendCudaExecutionProvider(api, session_options, applied_detail, error);
    case RuntimeBackend::kDirectML:
#if defined(_WIN32)
      return AppendDirectMlExecutionProvider(api, module_handle, session_options, applied_detail, error);
#else
      if (error != nullptr) {
        *error = "DirectML execution provider is only supported on Windows";
      }
      return false;
#endif
    case RuntimeBackend::kCoreML:
      return AppendCoreMlExecutionProvider(api, session_options, applied_detail, error);
    case RuntimeBackend::kAuto:
    case RuntimeBackend::kCpu:
      if (applied_detail != nullptr) {
        *applied_detail = "cpu";
      }
      return true;
    case RuntimeBackend::kMetal:
    case RuntimeBackend::kRemote:
      if (error != nullptr) {
        *error = "requested backend is not supported by ONNX runtime backend";
      }
      return false;
  }

  if (error != nullptr) {
    *error = "unknown backend selected";
  }
  return false;
}
#endif

#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
std::string ResolveConfiguredOrtLibraryPath(const RuntimeOptions& options) {
  if (!options.onnxruntime_library_path.empty()) {
    return options.onnxruntime_library_path;
  }
#if defined(ZSODA_ONNXRUNTIME_DLL_PATH_HINT_SET) && ZSODA_ONNXRUNTIME_DLL_PATH_HINT_SET
  const char* hint = ZSODA_ONNXRUNTIME_DLL_PATH_HINT;
  return hint != nullptr ? std::string(hint) : std::string();
#else
  return {};
#endif
}
#endif

ModelPipelineProfile ResolvePipelineProfile(const std::string& model_id) {
  if (model_id.rfind("depth-anything-v3", 0) == 0) {
    ModelPipelineProfile profile;
    profile.input_width = 518;
    profile.input_height = 518;
    profile.input_frame_count = ResolveDa3MultiviewFrameCount(model_id);
    profile.invert_depth = false;
    profile.prefer_latest_output_map = profile.input_frame_count > 1;
    if (UseImagenetNormalizationForDa3()) {
      profile.normalize_mean = {0.485F, 0.456F, 0.406F};
      profile.normalize_std = {0.229F, 0.224F, 0.225F};
    } else {
      profile.normalize_mean = {0.5F, 0.5F, 0.5F};
      profile.normalize_std = {0.5F, 0.5F, 0.5F};
    }
    return profile;
  }
  if (model_id.rfind("video-depth-anything", 0) == 0) {
    ModelPipelineProfile profile;
    profile.input_width = 512;
    profile.input_height = 288;
    profile.input_frame_count = ParseIntEnvOrDefault("ZSODA_VDA_CLIP_LENGTH", 32, 2, 128);
    profile.normalize_mean = {0.485F, 0.456F, 0.406F};
    profile.normalize_std = {0.229F, 0.224F, 0.225F};
    profile.invert_depth = false;
    profile.prefer_latest_output_map = true;
    return profile;
  }
  if (model_id.rfind("midas-", 0) == 0) {
    return {
        384,
        384,
        1,
        {0.5F, 0.5F, 0.5F},
        {0.5F, 0.5F, 0.5F},
        true,
        false,
    };
  }
  return {};
}

bool ShouldConvertLinearInputToSrgb() {
  return ParseBoolEnvOrDefault("ZSODA_INPUT_LINEAR_TO_SRGB", false);
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
  const auto& src_desc = request.source->desc();
  if (src_desc.width <= 0 || src_desc.height <= 0 || src_desc.channels <= 0) {
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

bool HasOnnxExtension(const std::filesystem::path& path) {
  return ToLowerCopy(path.extension().string()) == ".onnx";
}

bool IsRegularFile(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

std::filesystem::path BuildLegacyExternalDataPath(const std::filesystem::path& model_path) {
  std::filesystem::path legacy_path = model_path;
  legacy_path += "_data";
  return legacy_path;
}

bool EnsureDirectoryExists(const std::filesystem::path& path, std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to create directory: " + path.string() + " (" + ec.message() + ")";
    }
    return false;
  }
  return true;
}

bool CopyFileOverwrite(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       std::string* error) {
  std::error_code ec;
  std::filesystem::copy_file(
      source, destination, std::filesystem::copy_options::overwrite_existing, ec);
  if (!ec) {
    return true;
  }
  if (error != nullptr) {
    *error = "failed to copy file from " + source.string() + " to " + destination.string() +
             " (" + ec.message() + ")";
  }
  return false;
}

bool CreateHardLinkOrCopy(const std::filesystem::path& source,
                          const std::filesystem::path& destination,
                          std::string* error) {
  std::error_code ec;
  std::filesystem::remove(destination, ec);
  ec.clear();
  std::filesystem::create_hard_link(source, destination, ec);
  if (!ec) {
    return true;
  }
  return CopyFileOverwrite(source, destination, error);
}

// Some model packs ship external tensor data as "<model>.onnx_data" while ORT expects
// "model.onnx_data" relative to the loaded .onnx. Stage a compatibility copy in %TEMP%
// so we do not mutate installation directories at runtime.
bool PrepareOrtModelPath(const std::filesystem::path& candidate_path,
                         std::string_view model_id,
                         std::filesystem::path* out_session_model_path,
                         std::string* error) {
  if (out_session_model_path == nullptr) {
    if (error != nullptr) {
      *error = "internal error: session model output path is null";
    }
    return false;
  }

  *out_session_model_path = candidate_path;
  const std::filesystem::path expected_external_path =
      candidate_path.parent_path() / "model.onnx_data";
  if (IsRegularFile(expected_external_path)) {
    return true;
  }

  const std::filesystem::path legacy_external_path = BuildLegacyExternalDataPath(candidate_path);
  if (!IsRegularFile(legacy_external_path)) {
    return true;
  }

  std::error_code temp_dir_ec;
  const std::filesystem::path temp_root = std::filesystem::temp_directory_path(temp_dir_ec);
  if (temp_dir_ec) {
    if (error != nullptr) {
      *error = "failed to resolve temporary directory for model staging (" +
               temp_dir_ec.message() + ")";
    }
    return false;
  }
  const std::filesystem::path stage_dir =
      temp_root / "zsoda-ort-staged-models" / std::string(model_id);
  if (!EnsureDirectoryExists(stage_dir, error)) {
    return false;
  }

  const std::filesystem::path staged_model_path = stage_dir / "model.onnx";
  const std::filesystem::path staged_external_path = stage_dir / "model.onnx_data";
  if (!CopyFileOverwrite(candidate_path, staged_model_path, error)) {
    return false;
  }
  if (!CreateHardLinkOrCopy(legacy_external_path, staged_external_path, error)) {
    return false;
  }

  *out_session_model_path = staged_model_path;
  return true;
}

bool PrepareInputForModel(const InferenceRequest& request,
                          const ModelPipelineProfile& profile,
                          int preferred_tensor_width,
                          int preferred_tensor_height,
                          PreprocessResizeMode resize_mode,
                          PreparedModelInput* prepared_input,
                          std::string* error) {
  if (prepared_input == nullptr) {
    if (error != nullptr) {
      *error = "internal error: prepared input pointer is null";
    }
    return false;
  }

  const auto& source = *request.source;
  const auto& src_desc = source.desc();
  if (src_desc.width <= 0 || src_desc.height <= 0 || src_desc.channels <= 0) {
    if (error != nullptr) {
      *error = "invalid inference request: source frame descriptor is invalid";
    }
    return false;
  }
  prepared_input->source_width = src_desc.width;
  prepared_input->source_height = src_desc.height;

  const int fallback_width = std::max(kMinimumModelInputSize, profile.input_width);
  const int fallback_height = std::max(kMinimumModelInputSize, profile.input_height);
  prepared_input->tensor_width =
      std::max(kMinimumModelInputSize,
               preferred_tensor_width > 0 ? preferred_tensor_width : fallback_width);
  prepared_input->tensor_height =
      std::max(kMinimumModelInputSize,
               preferred_tensor_height > 0 ? preferred_tensor_height : fallback_height);
  prepared_input->tensor_channels = 3;
  prepared_input->resize_mode = resize_mode;
  prepared_input->resize_scale = 1.0F;
  prepared_input->resize_offset_x = 0.0F;
  prepared_input->resize_offset_y = 0.0F;

  const int tensor_width = prepared_input->tensor_width;
  const int tensor_height = prepared_input->tensor_height;
  const std::size_t pixel_count =
      static_cast<std::size_t>(tensor_width) * static_cast<std::size_t>(tensor_height);
  if (pixel_count == 0U) {
    if (error != nullptr) {
      *error = "invalid inference request: source frame has no pixels";
    }
    return false;
  }

  prepared_input->nchw_values.assign(pixel_count * 3U, 0.0F);
  constexpr float kPaddingNormalizedValue = 0.0F;
  const bool convert_linear_to_srgb = ShouldConvertLinearInputToSrgb();

  const float src_width = static_cast<float>(src_desc.width);
  const float src_height = static_cast<float>(src_desc.height);
  const float tensor_width_f = static_cast<float>(tensor_width);
  const float tensor_height_f = static_cast<float>(tensor_height);
  const float scale_x = tensor_width_f / src_width;
  const float scale_y = tensor_height_f / src_height;
  const bool use_letterbox = resize_mode != PreprocessResizeMode::kLowerBoundCenterCrop;
  const float resize_scale = use_letterbox ? std::min(scale_x, scale_y) : std::max(scale_x, scale_y);
  const float safe_resize_scale = resize_scale > 0.0F ? resize_scale : 1.0F;
  const float resized_width = src_width * safe_resize_scale;
  const float resized_height = src_height * safe_resize_scale;

  float transform_offset_x = 0.0F;
  float transform_offset_y = 0.0F;
  float letterbox_min_x = 0.0F;
  float letterbox_max_x = tensor_width_f;
  float letterbox_min_y = 0.0F;
  float letterbox_max_y = tensor_height_f;
  bool apply_letterbox_padding = false;
  if (use_letterbox) {
    const float pad_x = (tensor_width_f - resized_width) * 0.5F;
    const float pad_y = (tensor_height_f - resized_height) * 0.5F;
    transform_offset_x = -pad_x;
    transform_offset_y = -pad_y;
    letterbox_min_x = pad_x;
    letterbox_max_x = pad_x + resized_width;
    letterbox_min_y = pad_y;
    letterbox_max_y = pad_y + resized_height;
    apply_letterbox_padding = true;
  } else {
    const float crop_x = (resized_width - tensor_width_f) * 0.5F;
    const float crop_y = (resized_height - tensor_height_f) * 0.5F;
    transform_offset_x = crop_x;
    transform_offset_y = crop_y;
  }
  prepared_input->resize_scale = safe_resize_scale;
  prepared_input->resize_offset_x = transform_offset_x;
  prepared_input->resize_offset_y = transform_offset_y;

  const auto sample_channel = [&](float fx, float fy, int channel) -> float {
    const float clamped_x =
        std::clamp(fx, 0.0F, static_cast<float>(std::max(0, src_desc.width - 1)));
    const float clamped_y =
        std::clamp(fy, 0.0F, static_cast<float>(std::max(0, src_desc.height - 1)));
    const int x0 = static_cast<int>(clamped_x);
    const int y0 = static_cast<int>(clamped_y);
    const int x1 = std::min(x0 + 1, src_desc.width - 1);
    const int y1 = std::min(y0 + 1, src_desc.height - 1);
    const float tx = clamped_x - static_cast<float>(x0);
    const float ty = clamped_y - static_cast<float>(y0);
    const int source_channel = std::min(channel, src_desc.channels - 1);

    const float p00 = source.at(x0, y0, source_channel);
    const float p01 = source.at(x1, y0, source_channel);
    const float p10 = source.at(x0, y1, source_channel);
    const float p11 = source.at(x1, y1, source_channel);
    const float top = p00 + (p01 - p00) * tx;
    const float bottom = p10 + (p11 - p10) * tx;
    return top + (bottom - top) * ty;
  };

  float running_min = std::numeric_limits<float>::infinity();
  float running_max = -std::numeric_limits<float>::infinity();
  float running_sum = 0.0F;

  for (int y = 0; y < tensor_height; ++y) {
    const float target_y = static_cast<float>(y) + 0.5F;
    const bool inside_y =
        !apply_letterbox_padding ||
        (target_y >= letterbox_min_y && target_y < letterbox_max_y);
    const float src_y = ((target_y + transform_offset_y) / safe_resize_scale) - 0.5F;
    for (int x = 0; x < tensor_width; ++x) {
      const float target_x = static_cast<float>(x) + 0.5F;
      const bool inside_x =
          !apply_letterbox_padding ||
          (target_x >= letterbox_min_x && target_x < letterbox_max_x);
      const bool use_padding_value = apply_letterbox_padding && (!inside_x || !inside_y);
      const float src_x = ((target_x + transform_offset_x) / safe_resize_scale) - 0.5F;
      const std::size_t output_index =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(tensor_width) + x;

      float normalized_channels[3] = {};
      for (int channel = 0; channel < 3; ++channel) {
        float normalized = kPaddingNormalizedValue;
        if (!use_padding_value) {
          float sampled = std::clamp(sample_channel(src_x, src_y, channel), 0.0F, 1.0F);
          if (convert_linear_to_srgb) {
            sampled = LinearToSrgb(sampled);
          }
          const std::size_t channel_index = static_cast<std::size_t>(channel);
          const float mean = profile.normalize_mean[channel_index];
          const float std_dev = std::max(1e-6F, profile.normalize_std[channel_index]);
          normalized = (sampled - mean) / std_dev;
        }
        prepared_input->nchw_values[static_cast<std::size_t>(channel) * pixel_count + output_index] =
            normalized;
        normalized_channels[channel] = normalized;
      }

      const float normalized_luma = normalized_channels[0] * 0.2126F +
                                    normalized_channels[1] * 0.7152F +
                                    normalized_channels[2] * 0.0722F;
      running_min = std::min(running_min, normalized_luma);
      running_max = std::max(running_max, normalized_luma);
      running_sum += normalized_luma;
    }
  }

  prepared_input->normalized_min = running_min;
  prepared_input->normalized_max = running_max;
  prepared_input->normalized_mean = running_sum / static_cast<float>(pixel_count);

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool RunPlaceholderInference(const std::string& backend_name,
                             const std::string& model_id,
                             const std::string& model_path,
                             const PreparedModelInput& prepared_input,
                             RawDepthOutput* raw_output,
                             std::string* error) {
  if (raw_output != nullptr) {
    raw_output->width = prepared_input.tensor_width;
    raw_output->height = prepared_input.tensor_height;
    raw_output->min_depth = prepared_input.normalized_min;
    raw_output->max_depth = prepared_input.normalized_max;
    raw_output->depth_values.clear();
  }

  if (error != nullptr) {
    std::ostringstream oss;
    oss << backend_name
        << ": execution is not available in this build"
        << " (model_id=" << model_id << ", model_path=" << model_path
        << ", prepared_input=" << prepared_input.tensor_width << "x"
        << prepared_input.tensor_height << "x" << prepared_input.tensor_channels << ")";
    *error = oss.str();
  }
  return false;
}

bool PostprocessDepthForModel(const RawDepthOutput& raw_output,
                              const ModelPipelineProfile& profile,
                              const PreparedModelInput* prepared_input,
                              const zsoda::core::FrameDesc& target_desc,
                              zsoda::core::FrameBuffer* out_depth,
                              std::string* error) {
  if (out_depth == nullptr) {
    if (error != nullptr) {
      *error = "internal error: output buffer is null";
    }
    return false;
  }
  if (raw_output.width <= 0 || raw_output.height <= 0) {
    if (error != nullptr) {
      *error = "postprocess failed: model output has invalid shape";
    }
    return false;
  }

  auto output_desc = target_desc;
  output_desc.channels = 1;
  output_desc.format = zsoda::core::PixelFormat::kGray32F;
  out_depth->Resize(output_desc);

  const float near_value = raw_output.min_depth;
  const float far_value = raw_output.max_depth;
  const float range = (far_value - near_value == 0.0F) ? 1.0F : (far_value - near_value);

  const auto read_raw_depth = [&](int x, int y) -> float {
    if (raw_output.depth_values.empty()) {
      const float gradient =
          (static_cast<float>(x) / static_cast<float>(std::max(1, raw_output.width - 1)) +
           static_cast<float>(y) / static_cast<float>(std::max(1, raw_output.height - 1))) *
          0.5F;
      return near_value + gradient * range;
    }
    const std::size_t index =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(raw_output.width) + x;
    return raw_output.depth_values[index];
  };

  const bool has_preprocess_mapping =
      prepared_input != nullptr &&
      prepared_input->source_width > 0 &&
      prepared_input->source_height > 0 &&
      prepared_input->tensor_width > 0 &&
      prepared_input->tensor_height > 0 &&
      prepared_input->resize_scale > 0.0F;

  const float source_scale_x =
      has_preprocess_mapping
          ? static_cast<float>(prepared_input->source_width) /
                static_cast<float>(std::max(1, output_desc.width))
          : 1.0F;
  const float source_scale_y =
      has_preprocess_mapping
          ? static_cast<float>(prepared_input->source_height) /
                static_cast<float>(std::max(1, output_desc.height))
          : 1.0F;
  const float tensor_to_raw_scale_x =
      has_preprocess_mapping
          ? static_cast<float>(raw_output.width) /
                static_cast<float>(std::max(1, prepared_input->tensor_width))
          : 1.0F;
  const float tensor_to_raw_scale_y =
      has_preprocess_mapping
          ? static_cast<float>(raw_output.height) /
                static_cast<float>(std::max(1, prepared_input->tensor_height))
          : 1.0F;

  for (int y = 0; y < output_desc.height; ++y) {
    float src_y = 0.0F;
    if (has_preprocess_mapping) {
      const float source_center_y = (static_cast<float>(y) + 0.5F) * source_scale_y;
      const float tensor_center_y = source_center_y * prepared_input->resize_scale -
                                    prepared_input->resize_offset_y;
      src_y = tensor_center_y * tensor_to_raw_scale_y - 0.5F;
    } else {
      src_y = (static_cast<float>(y) + 0.5F) *
                  (static_cast<float>(raw_output.height) /
                   static_cast<float>(output_desc.height)) -
              0.5F;
    }
    for (int x = 0; x < output_desc.width; ++x) {
      float src_x = 0.0F;
      if (has_preprocess_mapping) {
        const float source_center_x = (static_cast<float>(x) + 0.5F) * source_scale_x;
        const float tensor_center_x = source_center_x * prepared_input->resize_scale -
                                      prepared_input->resize_offset_x;
        src_x = tensor_center_x * tensor_to_raw_scale_x - 0.5F;
      } else {
        src_x = (static_cast<float>(x) + 0.5F) *
                    (static_cast<float>(raw_output.width) /
                     static_cast<float>(output_desc.width)) -
                0.5F;
      }

      const float clamped_x =
          std::clamp(src_x, 0.0F, static_cast<float>(std::max(0, raw_output.width - 1)));
      const float clamped_y =
          std::clamp(src_y, 0.0F, static_cast<float>(std::max(0, raw_output.height - 1)));
      const int x0 = static_cast<int>(clamped_x);
      const int y0 = static_cast<int>(clamped_y);
      const int x1 = std::min(x0 + 1, raw_output.width - 1);
      const int y1 = std::min(y0 + 1, raw_output.height - 1);
      const float tx = clamped_x - static_cast<float>(x0);
      const float ty = clamped_y - static_cast<float>(y0);

      const float p00 = read_raw_depth(x0, y0);
      const float p01 = read_raw_depth(x1, y0);
      const float p10 = read_raw_depth(x0, y1);
      const float p11 = read_raw_depth(x1, y1);
      const float top = p00 + (p01 - p00) * tx;
      const float bottom = p10 + (p11 - p10) * tx;
      const float resized_depth = top + (bottom - top) * ty;

      float depth_value = resized_depth;
      if (profile.invert_depth) {
        depth_value = -depth_value;
      }
      if (!std::isfinite(depth_value)) {
        depth_value = 0.0F;
      }
      out_depth->at(x, y, 0) = depth_value;
    }
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
bool ResolveSessionIo(const Ort::Session& session,
                      std::string* input_name,
                      std::string* output_name,
                      int fallback_input_frame_count,
                      int* input_width,
                      int* input_height,
                      int* input_frame_count,
                      bool* input_has_image_dimension,
                      std::string* error) {
  if (input_name == nullptr || output_name == nullptr || input_width == nullptr ||
      input_height == nullptr || input_frame_count == nullptr ||
      input_has_image_dimension == nullptr) {
    if (error != nullptr) {
      *error = "internal error: invalid io metadata pointers";
    }
    return false;
  }

  const std::size_t input_count = session.GetInputCount();
  const std::size_t output_count = session.GetOutputCount();
  if (input_count == 0U || output_count == 0U) {
    if (error != nullptr) {
      *error = "onnx runtime session must expose at least one input and output";
    }
    return false;
  }

  Ort::AllocatorWithDefaultOptions allocator;
  auto input_name_alloc = session.GetInputNameAllocated(0U, allocator);
  *input_name = input_name_alloc.get() != nullptr ? input_name_alloc.get() : "";
  if (input_name->empty()) {
    if (error != nullptr) {
      *error = "onnx runtime session has empty input/output names";
    }
    return false;
  }

  std::vector<std::string> output_names;
  output_names.reserve(output_count);
  for (std::size_t output_index = 0U; output_index < output_count; ++output_index) {
    auto output_name_alloc = session.GetOutputNameAllocated(output_index, allocator);
    output_names.emplace_back(output_name_alloc.get() != nullptr ? output_name_alloc.get() : "");
  }

  std::size_t selected_output_index = 0U;
  const int forced_output_index = ParseIntEnvOrDefault("ZSODA_ORT_OUTPUT_INDEX", -1, -1, 4096);
  if (forced_output_index >= 0) {
    selected_output_index = static_cast<std::size_t>(forced_output_index);
    if (selected_output_index >= output_names.size()) {
      if (error != nullptr) {
        *error = "onnx runtime forced output index is out of range";
      }
      return false;
    }
  } else {
    const std::string forced_output_name = ToLowerCopy(ReadEnvOrEmpty("ZSODA_ORT_OUTPUT_NAME"));
    if (!forced_output_name.empty()) {
      bool found = false;
      for (std::size_t i = 0U; i < output_names.size(); ++i) {
        if (ToLowerCopy(output_names[i]) == forced_output_name) {
          selected_output_index = i;
          found = true;
          break;
        }
      }
      if (!found) {
        if (error != nullptr) {
          *error = "onnx runtime forced output name was not found in session outputs";
        }
        return false;
      }
    } else {
      selected_output_index = SelectDepthOutputIndexByName(output_names);
    }
  }

  *output_name = output_names[selected_output_index];
  if (output_name->empty()) {
    if (error != nullptr) {
      *error = "onnx runtime selected output name is empty";
    }
    return false;
  }
  {
    std::ostringstream detail;
    detail << "output_count=" << output_count << ", selected_index=" << selected_output_index
           << ", selected_name=" << *output_name;
    const std::string detail_text = detail.str();
    AppendOrtTrace("resolve_io_output_select", detail_text.c_str());
  }

  const auto input_info = session.GetInputTypeInfo(0U).GetTensorTypeAndShapeInfo();
  if (input_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    if (error != nullptr) {
      *error = "onnx runtime session input must be float tensor";
    }
    return false;
  }

  const std::vector<int64_t> input_shape = input_info.GetShape();
  const auto BuildShapeString = [](const std::vector<int64_t>& shape) -> std::string {
    std::ostringstream oss;
    oss << "[";
    for (std::size_t i = 0; i < shape.size(); ++i) {
      if (i != 0U) {
        oss << ", ";
      }
      oss << shape[i];
    }
    oss << "]";
    return oss.str();
  };

  int64_t batch = 0;
  int64_t num_images = 1;
  int64_t channels = 0;
  int64_t height = 0;
  int64_t width = 0;
  bool has_image_dimension = false;
  if (input_shape.size() == 4U) {
    // Standard NCHW tensor.
    batch = input_shape[0];
    channels = input_shape[1];
    height = input_shape[2];
    width = input_shape[3];
  } else if (input_shape.size() == 5U) {
    // Some DA3 exports use [batch, num_images, channels, height, width].
    has_image_dimension = true;
    batch = input_shape[0];
    num_images = input_shape[1];
    channels = input_shape[2];
    height = input_shape[3];
    width = input_shape[4];
  } else {
    if (error != nullptr) {
      *error = "onnx runtime session input rank is unsupported: shape=" +
               BuildShapeString(input_shape);
    }
    return false;
  }

  if (batch == 0) {
    if (error != nullptr) {
      *error = "onnx runtime session batch dimension cannot be zero";
    }
    return false;
  }
  if (batch > static_cast<int64_t>(std::numeric_limits<int>::max())) {
    if (error != nullptr) {
      *error = "onnx runtime session batch dimension exceeds supported range";
    }
    return false;
  }
  if (channels > 0 && channels != 3) {
    if (error != nullptr) {
      *error = "onnx runtime session input channel must be 3";
    }
    return false;
  }
  if (width == 0 || height == 0) {
    if (error != nullptr) {
      *error = "onnx runtime session input width/height cannot be zero";
    }
    return false;
  }

  const auto output_info =
      session.GetOutputTypeInfo(selected_output_index).GetTensorTypeAndShapeInfo();
  if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    if (error != nullptr) {
      *error = "onnx runtime session output must be float tensor";
    }
    return false;
  }

  *input_width = width > 0 ? static_cast<int>(width) : 0;
  *input_height = height > 0 ? static_cast<int>(height) : 0;
  if (has_image_dimension) {
    if (num_images == 0) {
      if (error != nullptr) {
        *error = "onnx runtime session BNCHW input has zero num_images dimension";
      }
      return false;
    }
    if (num_images > static_cast<int64_t>(std::numeric_limits<int>::max())) {
      if (error != nullptr) {
        *error = "onnx runtime session BNCHW num_images exceeds supported range";
      }
      return false;
    }
    const int resolved_frame_count = num_images > 0
                                         ? static_cast<int>(num_images)
                                         : std::max(1, fallback_input_frame_count);
    *input_frame_count = std::max(1, resolved_frame_count);
  } else {
    const int resolved_batch_frame_count = batch > 0
                                               ? static_cast<int>(batch)
                                               : std::max(1, fallback_input_frame_count);
    *input_frame_count = std::max(1, resolved_batch_frame_count);
  }
  *input_has_image_dimension = has_image_dimension;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

std::string BuildTensorShapeString(const std::vector<int64_t>& shape) {
  std::ostringstream oss;
  oss << "[";
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (i != 0U) {
      oss << ", ";
    }
    oss << shape[i];
  }
  oss << "]";
  return oss.str();
}

bool CheckedMultiplySize(std::size_t lhs, std::size_t rhs, std::size_t* out_value) {
  if (out_value == nullptr) {
    return false;
  }
  if (lhs == 0U || rhs == 0U) {
    *out_value = 0U;
    return true;
  }
  if (lhs > (std::numeric_limits<std::size_t>::max() / rhs)) {
    return false;
  }
  *out_value = lhs * rhs;
  return true;
}

bool CopyTensorPrefix(const float* data,
                      std::size_t value_count,
                      std::vector<float>* out_values,
                      std::string* error) {
  if (out_values == nullptr) {
    if (error != nullptr) {
      *error = "internal error: output vector for tensor copy is null";
    }
    return false;
  }
  if (data == nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor data pointer is null";
    }
    return false;
  }

  out_values->assign(value_count, 0.0F);
  if (value_count == 0U) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  std::size_t byte_count = 0U;
  if (!CheckedMultiplySize(value_count, sizeof(float), &byte_count)) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor byte count overflow";
    }
    return false;
  }

  std::memcpy(out_values->data(), data, byte_count);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool ExtractDepthOutputFromOrtValue(const OrtApi* api,
                                    const OrtValue* output,
                                    std::size_t preferred_map_index,
                                    int temporal_frame_count_hint,
                                    RawDepthOutput* raw_output,
                                    std::string* error) {
  AppendOrtTrace("extract_fn_enter");
  if (api == nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime api pointer is null during output extraction";
    }
    return false;
  }
  if (output == nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime output is null";
    }
    return false;
  }
  if (raw_output == nullptr) {
    if (error != nullptr) {
      *error = "internal error: raw output is null";
    }
    return false;
  }
  if (api->IsTensor == nullptr ||
      api->GetTensorTypeAndShape == nullptr ||
      api->GetTensorElementType == nullptr ||
      api->GetDimensionsCount == nullptr ||
      api->GetDimensions == nullptr ||
      api->GetTensorShapeElementCount == nullptr ||
      api->GetTensorMutableData == nullptr ||
      api->ReleaseTensorTypeAndShapeInfo == nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime output extraction API surface is incomplete";
    }
    return false;
  }

  int is_tensor = 0;
  if (OrtStatus* status = api->IsTensor(output, &is_tensor); status != nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime output IsTensor failed: " + ConsumeOrtStatusMessage(api, status);
    }
    return false;
  }
  if (is_tensor == 0) {
    if (error != nullptr) {
      *error = "onnx runtime output is not a tensor";
    }
    return false;
  }

  OrtTensorTypeAndShapeInfo* tensor_info = nullptr;
  if (OrtStatus* status = api->GetTensorTypeAndShape(output, &tensor_info); status != nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime GetTensorTypeAndShape failed: " + ConsumeOrtStatusMessage(api, status);
    }
    return false;
  }

  auto release_tensor_info = [&]() {
    if (tensor_info != nullptr) {
      api->ReleaseTensorTypeAndShapeInfo(tensor_info);
      tensor_info = nullptr;
    }
  };

  ONNXTensorElementDataType element_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  if (OrtStatus* status = api->GetTensorElementType(tensor_info, &element_type); status != nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime GetTensorElementType failed: " + ConsumeOrtStatusMessage(api, status);
    }
    release_tensor_info();
    return false;
  }
  if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor must be float";
    }
    release_tensor_info();
    return false;
  }

  std::size_t dimensions_count = 0U;
  if (OrtStatus* status = api->GetDimensionsCount(tensor_info, &dimensions_count); status != nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime GetDimensionsCount failed: " + ConsumeOrtStatusMessage(api, status);
    }
    release_tensor_info();
    return false;
  }

  std::vector<int64_t> shape(dimensions_count, 0);
  if (dimensions_count > 0U) {
    if (OrtStatus* status = api->GetDimensions(tensor_info, shape.data(), dimensions_count);
        status != nullptr) {
      if (error != nullptr) {
        *error = "onnx runtime GetDimensions failed: " + ConsumeOrtStatusMessage(api, status);
      }
      release_tensor_info();
      return false;
    }
  }
  {
    const std::string shape_detail = "shape=" + BuildTensorShapeString(shape);
    AppendOrtTrace("extract_shape", shape_detail.c_str());
  }

  std::size_t tensor_element_count = 0U;
  if (OrtStatus* status = api->GetTensorShapeElementCount(tensor_info, &tensor_element_count);
      status != nullptr) {
    if (error != nullptr) {
      *error =
          "onnx runtime GetTensorShapeElementCount failed: " + ConsumeOrtStatusMessage(api, status);
    }
    release_tensor_info();
    return false;
  }
  release_tensor_info();

  void* data_raw = nullptr;
  if (OrtStatus* status =
          api->GetTensorMutableData(const_cast<OrtValue*>(output), &data_raw);
      status != nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime GetTensorMutableData failed: " + ConsumeOrtStatusMessage(api, status);
    }
    return false;
  }
  const float* data = reinterpret_cast<const float*>(data_raw);
  AppendOrtTrace("extract_output", data != nullptr ? "ptr=non_null" : "ptr=null");
  if (data == nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor data pointer is null";
    }
    AppendOrtTrace("extract_tensor_data_null");
    return false;
  }

  int width = 0;
  int height = 0;
  raw_output->depth_values.clear();
  if (shape.size() < 2U) {
    if (error != nullptr) {
      *error = "onnx runtime output shape is unsupported";
    }
    return false;
  }

  const int64_t output_height = shape[shape.size() - 2U];
  const int64_t output_width = shape[shape.size() - 1U];
  if (output_height < 1 || output_width < 1) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor shape is invalid";
    }
    return false;
  }
  if (output_height > std::numeric_limits<int>::max() ||
      output_width > std::numeric_limits<int>::max()) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor shape exceeds supported dimensions";
    }
    return false;
  }

  width = static_cast<int>(output_width);
  height = static_cast<int>(output_height);
  std::size_t pixel_count = 0U;
  if (!CheckedMultiplySize(static_cast<std::size_t>(width),
                           static_cast<std::size_t>(height),
                           &pixel_count)) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor pixel count overflow";
    }
    return false;
  }
  if (pixel_count == 0U || tensor_element_count < pixel_count ||
      (tensor_element_count % pixel_count) != 0U) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor element count is incompatible with output shape";
    }
    return false;
  }

  const std::size_t map_count = tensor_element_count / pixel_count;
  if (map_count == 0U) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor has no maps";
    }
    return false;
  }

  const std::size_t selected_map =
      ResolveOutputMapIndex(preferred_map_index, map_count, temporal_frame_count_hint);
  if (selected_map >= map_count) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor map index is out of range";
    }
    return false;
  }
  {
    std::ostringstream detail;
    detail << "map_count=" << map_count << ", selected_map=" << selected_map
           << ", temporal_hint=" << temporal_frame_count_hint;
    const std::string detail_text = detail.str();
    AppendOrtTrace("extract_output_map_select", detail_text.c_str());
  }

  const float* map_data = data + selected_map * pixel_count;
  if (!CopyTensorPrefix(map_data, pixel_count, &raw_output->depth_values, error)) {
    return false;
  }

  float min_depth = std::numeric_limits<float>::infinity();
  float max_depth = -std::numeric_limits<float>::infinity();
  for (const float value : raw_output->depth_values) {
    min_depth = std::min(min_depth, value);
    max_depth = std::max(max_depth, value);
  }

  raw_output->width = width;
  raw_output->height = height;
  raw_output->min_depth = min_depth;
  raw_output->max_depth = max_depth;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

#endif

class OnnxRuntimeBackendScaffold final : public IOnnxRuntimeBackend {
 public:
  explicit OnnxRuntimeBackendScaffold(RuntimeOptions options)
      : options_(std::move(options)),
        requested_backend_(options_.preferred_backend),
        active_backend_(RuntimeBackend::kCpu),
        backend_name_(BuildBackendName(active_backend_)) {}

  const char* Name() const override { return backend_name_.c_str(); }

  bool Initialize(std::string* error) override {
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
    const std::string configured_library_path = ResolveConfiguredOrtLibraryPath(options_);
#if defined(ZSODA_ONNXRUNTIME_DIRECT_LINK) && !ZSODA_ONNXRUNTIME_DIRECT_LINK
    if (configured_library_path.empty()) {
      initialized_ = false;
      if (error != nullptr) {
        *error = "onnx runtime dynamic loader failed: configured library path is empty in "
                 "structural mode";
      }
      return false;
    }
#endif

    const std::uint32_t header_api_version = static_cast<std::uint32_t>(ORT_API_VERSION);
    const std::uint32_t default_api_version =
        std::min(header_api_version, kDefaultOrtApiVersionFloor);
    const std::uint32_t requested_api_version =
        options_.onnxruntime_api_version > 0
            ? static_cast<std::uint32_t>(options_.onnxruntime_api_version)
            : default_api_version;

    loader_ = std::make_unique<OrtDynamicLoader>();
    const bool prefer_preloaded_ort = ShouldPreferPreloadedOrt(requested_backend_);
    std::string loader_error;
    if (!loader_->Load(configured_library_path,
                       requested_api_version,
                       &loader_error,
                       prefer_preloaded_ort)) {
      initialized_ = false;
      if (error != nullptr) {
        *error = loader_error.empty() ? "onnx runtime dynamic loader failed" : loader_error;
      }
      return false;
    }
    loader_diagnostics_ = loader_->Diagnostics();

    RuntimeProviderCapabilities capabilities;
    std::string capability_note;
    std::string capability_error;
    if (!QueryRuntimeCapabilities(
            loader_->Api(), &capabilities, &capability_note, &capability_error)) {
      initialized_ = false;
      if (error != nullptr) {
        const std::string capability_detail =
            capability_error.empty() ? "onnx runtime capability probe failed" : capability_error;
        *error = WithRuntimeDiagnostics(capability_detail);
      }
      return false;
    }

    std::string selection_note;
    active_backend_ = SelectActiveBackend(requested_backend_, capabilities, &selection_note);
    provider_note_ = BuildProviderNote(requested_backend_, active_backend_, selection_note);
    if (!capability_note.empty()) {
      if (provider_note_.empty()) {
        provider_note_ = capability_note;
      } else {
        provider_note_.append("; ");
        provider_note_.append(capability_note);
      }
    }

    Ort::InitApi(loader_->Api());

    try {
      env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "zsoda-ort");
      session_options_ = std::make_unique<Ort::SessionOptions>();
      session_options_->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      const int intra_threads = ParseIntEnvOrDefault("ZSODA_ORT_INTRA_THREADS", 0, 0, 256);
      const int inter_threads = ParseIntEnvOrDefault("ZSODA_ORT_INTER_THREADS", 0, 0, 256);
      if (intra_threads > 0) {
        session_options_->SetIntraOpNumThreads(intra_threads);
      }
      if (inter_threads > 0) {
        session_options_->SetInterOpNumThreads(inter_threads);
      }

      OrtSessionOptions* native_session_options = static_cast<OrtSessionOptions*>(*session_options_);
      RuntimeBackend configured_backend = active_backend_;
      std::string provider_apply_detail;
      std::string provider_apply_error;
      if (!ConfigureExecutionProvider(loader_->Api(),
                                      loader_->NativeModuleHandle(),
                                      configured_backend,
                                      native_session_options,
                                      &provider_apply_detail,
                                      &provider_apply_error)) {
        if (configured_backend == RuntimeBackend::kCpu) {
          initialized_ = false;
          if (error != nullptr) {
            const std::string detail = provider_apply_error.empty()
                                           ? "onnx runtime CPU provider setup failed"
                                           : provider_apply_error;
            *error = WithRuntimeDiagnostics(detail);
          }
          return false;
        }

        std::string cpu_apply_detail;
        std::string cpu_apply_error;
        if (!ConfigureExecutionProvider(loader_->Api(),
                                        loader_->NativeModuleHandle(),
                                        RuntimeBackend::kCpu,
                                        native_session_options,
                                        &cpu_apply_detail,
                                        &cpu_apply_error)) {
          initialized_ = false;
          if (error != nullptr) {
            const std::string gpu_detail =
                provider_apply_error.empty() ? "<no provider error>" : provider_apply_error;
            const std::string cpu_detail =
                cpu_apply_error.empty() ? "<no cpu fallback error>" : cpu_apply_error;
            *error = WithRuntimeDiagnostics("onnx runtime provider setup failed: requested=" +
                                            std::string(RuntimeBackendName(configured_backend)) +
                                            ", detail=" + gpu_detail +
                                            ", cpu_fallback_detail=" + cpu_detail);
          }
          return false;
        }

        if (!provider_note_.empty()) {
          provider_note_.append("; ");
        }
        provider_note_.append("provider_setup_failed=");
        provider_note_.append(provider_apply_error.empty() ? "<none>" : provider_apply_error);
        provider_note_.append(", fallback=cpu");
        active_backend_ = RuntimeBackend::kCpu;
        provider_apply_detail = cpu_apply_detail;
      }

      if (!provider_apply_detail.empty()) {
        if (!provider_note_.empty()) {
          provider_note_.append("; ");
        }
        provider_note_.append("applied_provider=");
        provider_note_.append(provider_apply_detail);
      }
      provider_note_base_ = provider_note_;
    } catch (const Ort::Exception& ex) {
      initialized_ = false;
      if (error != nullptr) {
        *error = WithRuntimeDiagnostics(std::string("onnx runtime initialize failed: ") +
                                        SafeCStr(ex.what()));
      }
      return false;
    }
    RefreshBackendName();
#else
    provider_note_base_.clear();
    loader_diagnostics_.clear();
#endif

    initialized_ = true;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool SelectModel(const std::string& model_id,
                   const std::string& model_path,
                   std::string* error) override {
    if (!initialized_) {
      if (error != nullptr) {
        *error = "onnx runtime backend is not initialized";
      }
      return false;
    }
    if (model_id.empty()) {
      if (error != nullptr) {
        *error = "model id cannot be empty";
      }
      return false;
    }
    if (model_path.empty()) {
      if (error != nullptr) {
        *error = "model path cannot be empty";
      }
      return false;
    }

    const std::filesystem::path candidate_path(model_path);
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(candidate_path, filesystem_error);
    if (filesystem_error || !exists) {
      if (error != nullptr) {
        *error = "model path does not exist: " + model_path;
      }
      return false;
    }
    if (!std::filesystem::is_regular_file(candidate_path, filesystem_error)) {
      if (error != nullptr) {
        *error = "model path is not a regular file: " + model_path;
      }
      return false;
    }
    if (!HasOnnxExtension(candidate_path)) {
      if (error != nullptr) {
        *error = "model file extension must be .onnx: " + model_path;
      }
      return false;
    }

    const ModelPipelineProfile target_profile = ResolvePipelineProfile(model_id);
    const std::string target_model_path = candidate_path.lexically_normal().string();
    int target_input_width = std::max(kMinimumModelInputSize, target_profile.input_width);
    int target_input_height = std::max(kMinimumModelInputSize, target_profile.input_height);
    int target_input_frame_count = std::max(1, target_profile.input_frame_count);
    std::filesystem::path session_model_path = candidate_path;
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
    input_has_image_dimension_ = false;
#endif

#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
    if (env_ == nullptr || session_options_ == nullptr) {
      if (error != nullptr) {
        *error = WithRuntimeDiagnostics("onnx runtime backend is not initialized");
      }
      return false;
    }
    std::string staged_model_error;
    if (!PrepareOrtModelPath(candidate_path, model_id, &session_model_path, &staged_model_error)) {
      if (error != nullptr) {
        *error = WithRuntimeDiagnostics(staged_model_error);
      }
      return false;
    }

    try {
#if defined(_WIN32)
      const std::wstring ort_path = session_model_path.native();
      auto session = std::make_unique<Ort::Session>(*env_, ort_path.c_str(), *session_options_);
#else
      const std::string ort_path = session_model_path.string();
      auto session = std::make_unique<Ort::Session>(*env_, ort_path.c_str(), *session_options_);
#endif
      std::string resolved_input_name;
      std::string resolved_output_name;
      int resolved_input_width = 0;
      int resolved_input_height = 0;
      int resolved_input_frame_count = 1;
      bool resolved_input_has_image_dimension = false;
      std::string io_error;
      if (!ResolveSessionIo(*session,
                            &resolved_input_name,
                            &resolved_output_name,
                            target_input_frame_count,
                            &resolved_input_width,
                            &resolved_input_height,
                            &resolved_input_frame_count,
                            &resolved_input_has_image_dimension,
                            &io_error)) {
        if (error != nullptr) {
          *error = WithRuntimeDiagnostics(io_error);
        }
        return false;
      }

      session_ = std::move(session);
      input_name_ = std::move(resolved_input_name);
      output_name_ = std::move(resolved_output_name);
      if (resolved_input_width > 0) {
        target_input_width = resolved_input_width;
      }
      if (resolved_input_height > 0) {
        target_input_height = resolved_input_height;
      }
      if (resolved_input_frame_count > 0) {
        target_input_frame_count = resolved_input_frame_count;
      }
      input_has_image_dimension_ = resolved_input_has_image_dimension;
    } catch (const Ort::Exception& ex) {
      session_.reset();
      input_name_.clear();
      output_name_.clear();
      input_has_image_dimension_ = false;
      if (error != nullptr) {
        *error = WithRuntimeDiagnostics(std::string("onnx runtime session create failed: ") +
                                        SafeCStr(ex.what()));
      }
      return false;
    }
#endif

    active_model_profile_ = target_profile;
    active_model_id_ = model_id;
    active_model_path_ = target_model_path;
    model_input_width_ = target_input_width;
    model_input_height_ = target_input_height;
    model_input_frame_count_ = target_input_frame_count;
    const bool model_id_requests_multiview = ModelIdIndicatesMultiview(model_id);
    requested_multiview_ = model_id_requests_multiview || target_profile.input_frame_count > 1;
    resolved_input_frame_count_ = std::max(1, model_input_frame_count_);
    multiview_active_ = resolved_input_frame_count_ > 1;
    multiview_warning_ = model_id_requests_multiview && !multiview_active_;
    const std::string multiview_note =
        BuildMultiviewDiagnosticsNote(requested_multiview_,
                                      resolved_input_frame_count_,
                                      multiview_active_,
                                      multiview_warning_);
    provider_note_ = provider_note_base_;
    AppendNoteSegment(&provider_note_, multiview_note);
    {
      std::string multiview_detail = "model_id=" + model_id + ", " + multiview_note;
      AppendOrtTrace(multiview_warning_ ? "multiview_warning" : "multiview_status",
                     multiview_detail.c_str());
    }
    RefreshBackendName();
    ResetTemporalHistory();

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool Run(const InferenceRequest& request,
           zsoda::core::FrameBuffer* out_depth,
           std::string* error) const override {
    AppendOrtTrace("run_enter", active_model_id_.empty() ? "<no_model>" : active_model_id_.c_str());
    if (!initialized_) {
      if (error != nullptr) {
        *error = "onnx runtime backend is not initialized";
      }
      AppendOrtTrace("run_not_initialized");
      return false;
    }
    if (active_model_id_.empty()) {
      if (error != nullptr) {
        *error = "onnx runtime backend has no active model";
      }
      AppendOrtTrace("run_no_active_model");
      return false;
    }
    if (active_model_path_.empty()) {
      if (error != nullptr) {
        *error = "onnx runtime backend has no active model path";
      }
      AppendOrtTrace("run_no_model_path");
      return false;
    }
    if (!ValidateRequest(request, out_depth, error)) {
      AppendOrtTrace("run_validate_failed", (error != nullptr && !error->empty()) ? error->c_str()
                                                                                   : "<none>");
      return false;
    }

    PreparedModelInput prepared_input;
    if (!PrepareInputForModel(request,
                              active_model_profile_,
                              model_input_width_,
                              model_input_height_,
                              options_.preprocess_resize_mode,
                              &prepared_input,
                              error)) {
      AppendOrtTrace("run_prepare_input_failed",
                     (error != nullptr && !error->empty()) ? error->c_str() : "<none>");
      return false;
    }
    {
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
      const bool has_image_dimension = input_has_image_dimension_;
#else
      const bool has_image_dimension = false;
#endif
      const int frame_count = std::max(1, model_input_frame_count_);
      const std::string prepared_detail =
          "tensor=" + std::to_string(prepared_input.tensor_width) + "x" +
          std::to_string(prepared_input.tensor_height) + "x" +
          std::to_string(prepared_input.tensor_channels) +
          ", frames=" + std::to_string(frame_count) +
          ", image_dim=" + std::string(has_image_dimension ? "1" : "0") +
          ", requested_multiview=" + std::string(requested_multiview_ ? "1" : "0") +
          ", resolved_input_frame_count=" + std::to_string(std::max(1, resolved_input_frame_count_)) +
          ", multiview_active=" + std::string(multiview_active_ ? "1" : "0") +
          ", multiview_warning=" + std::string(multiview_warning_ ? "1" : "0") +
          ", preprocess_resize_mode=" + std::string(PreprocessResizeModeName(options_.preprocess_resize_mode));
      AppendOrtTrace("run_prepare_input_ok", prepared_detail.c_str());
    }

    RawDepthOutput raw_output;
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
    if (session_ == nullptr || input_name_.empty() || output_name_.empty()) {
      if (error != nullptr) {
        *error = WithRuntimeDiagnostics("onnx runtime backend has no active session");
      }
      return false;
    }
    if (prepared_input.nchw_values.empty()) {
      if (error != nullptr) {
        *error = "onnx runtime backend prepared input is empty";
      }
      return false;
    }
    if (loader_ == nullptr || loader_->Api() == nullptr) {
      if (error != nullptr) {
        *error = WithRuntimeDiagnostics("onnx runtime backend API handle is unavailable");
      }
      return false;
    }

    std::vector<float> temporal_input_values;
    const std::vector<float>* run_input_values = &prepared_input.nchw_values;
    const int input_frame_count = std::max(1, model_input_frame_count_);
    if (input_frame_count > 1) {
      if (!BuildTemporalInput(prepared_input,
                              input_frame_count,
                              request.frame_hash,
                              &temporal_input_values,
                              error)) {
        return false;
      }
      run_input_values = &temporal_input_values;
    }

    const OrtApi* api = loader_->Api();
    if (api->CreateCpuMemoryInfo == nullptr ||
        api->ReleaseMemoryInfo == nullptr ||
        api->CreateTensorWithDataAsOrtValue == nullptr ||
        api->ReleaseValue == nullptr ||
        api->Run == nullptr) {
      if (error != nullptr) {
        *error = WithRuntimeDiagnostics("onnx runtime run API surface is incomplete");
      }
      return false;
    }

    const std::array<const char*, 1> input_names = {input_name_.c_str()};
    const std::array<const char*, 1> output_names = {output_name_.c_str()};

    OrtMemoryInfo* memory_info = nullptr;
    OrtValue* input_tensor = nullptr;
    OrtValue* output_tensor = nullptr;
    auto release_run_resources = [&]() {
      if (output_tensor != nullptr) {
        api->ReleaseValue(output_tensor);
        output_tensor = nullptr;
      }
      if (input_tensor != nullptr) {
        api->ReleaseValue(input_tensor);
        input_tensor = nullptr;
      }
      if (memory_info != nullptr) {
        api->ReleaseMemoryInfo(memory_info);
        memory_info = nullptr;
      }
    };

    const std::array<int64_t, 5> input_shape_bnchw = {
        1,
        static_cast<int64_t>(input_frame_count),
        prepared_input.tensor_channels,
        prepared_input.tensor_height,
        prepared_input.tensor_width,
    };
    const std::array<int64_t, 4> input_shape_nchw = {
        static_cast<int64_t>(input_frame_count),
        prepared_input.tensor_channels,
        prepared_input.tensor_height,
        prepared_input.tensor_width,
    };
    const int64_t* input_shape = input_has_image_dimension_ ? input_shape_bnchw.data()
                                                            : input_shape_nchw.data();
    const std::size_t input_rank =
        input_has_image_dimension_ ? input_shape_bnchw.size() : input_shape_nchw.size();
    AppendOrtTrace(input_has_image_dimension_ ? "session_run_begin_bnchw" : "session_run_begin_nchw");

    std::size_t input_byte_count = 0U;
    if (!CheckedMultiplySize(run_input_values->size(), sizeof(float), &input_byte_count)) {
      if (error != nullptr) {
        *error = "onnx runtime input tensor byte count overflow";
      }
      return false;
    }

    try {
      if (OrtStatus* status = api->CreateCpuMemoryInfo(
              OrtArenaAllocator, OrtMemTypeDefault, &memory_info);
          status != nullptr) {
        if (error != nullptr) {
          *error = WithRuntimeDiagnostics(std::string("onnx runtime CreateCpuMemoryInfo failed: ") +
                                          ConsumeOrtStatusMessage(api, status));
        }
        release_run_resources();
        return false;
      }

      if (OrtStatus* status = api->CreateTensorWithDataAsOrtValue(
              memory_info,
              const_cast<float*>(run_input_values->data()),
              input_byte_count,
              input_shape,
              input_rank,
              ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
              &input_tensor);
          status != nullptr) {
        if (error != nullptr) {
          *error =
              WithRuntimeDiagnostics(std::string("onnx runtime CreateTensorWithDataAsOrtValue failed: ") +
                                     ConsumeOrtStatusMessage(api, status));
        }
        release_run_resources();
        return false;
      }

      int input_is_tensor = 0;
      if (api->IsTensor == nullptr ||
          (api->IsTensor(input_tensor, &input_is_tensor) != nullptr) ||
          input_is_tensor == 0) {
        if (error != nullptr) {
          *error = WithRuntimeDiagnostics("onnx runtime input tensor validation failed");
        }
        release_run_resources();
        return false;
      }

      if (OrtStatus* status = api->Run(*session_,
                                       nullptr,
                                       input_names.data(),
                                       &input_tensor,
                                       input_names.size(),
                                       output_names.data(),
                                       output_names.size(),
                                       &output_tensor);
          status != nullptr) {
        if (error != nullptr) {
          *error = WithRuntimeDiagnostics(std::string("onnx runtime run failed: ") +
                                          ConsumeOrtStatusMessage(api, status));
        }
        AppendOrtTrace("session_run_exception", (error != nullptr && !error->empty()) ? error->c_str()
                                                                                       : "<none>");
        release_run_resources();
        return false;
      }

      const std::string run_end_detail = "outputs=" + std::to_string(output_tensor != nullptr ? 1 : 0);
      AppendOrtTrace("session_run_end", run_end_detail.c_str());
      if (output_tensor == nullptr) {
        if (error != nullptr) {
          *error = "onnx runtime run returned no outputs";
        }
        AppendOrtTrace("session_run_no_outputs");
        release_run_resources();
        return false;
      }

      AppendOrtTrace("extract_output_begin");
      const std::size_t output_map_index =
          active_model_profile_.prefer_latest_output_map ? std::numeric_limits<std::size_t>::max()
                                                         : 0U;
      if (!ExtractDepthOutputFromOrtValue(
              api, output_tensor, output_map_index, input_frame_count, &raw_output, error)) {
        AppendOrtTrace("extract_output_failed",
                       (error != nullptr && !error->empty()) ? error->c_str() : "<none>");
        release_run_resources();
        return false;
      }
      {
        const std::string out_detail =
            "depth=" + std::to_string(raw_output.width) + "x" + std::to_string(raw_output.height);
        AppendOrtTrace("extract_output_ok", out_detail.c_str());
      }
    } catch (const std::exception& ex) {
      if (error != nullptr) {
        *error = WithRuntimeDiagnostics(std::string("onnx runtime run threw exception: ") +
                                        SafeCStr(ex.what()));
      }
      AppendOrtTrace("session_run_exception", SafeCStr(ex.what()));
      release_run_resources();
      return false;
    } catch (...) {
      if (error != nullptr) {
        *error = WithRuntimeDiagnostics("onnx runtime run threw unknown exception");
      }
      AppendOrtTrace("session_run_exception", "unknown");
      release_run_resources();
      return false;
    }

    release_run_resources();
#else
    std::string runtime_error;
    if (!RunPlaceholderInference(backend_name_,
                                 active_model_id_,
                                 active_model_path_,
                                 prepared_input,
                                 &raw_output,
                                 &runtime_error)) {
      if (error != nullptr) {
        *error = runtime_error;
      }
      return false;
    }
#endif

    if (!PostprocessDepthForModel(raw_output,
                                  active_model_profile_,
                                  &prepared_input,
                                  request.source->desc(),
                                  out_depth,
                                  error)) {
      AppendOrtTrace("postprocess_failed",
                     (error != nullptr && !error->empty()) ? error->c_str() : "<none>");
      return false;
    }
    AppendOrtTrace("postprocess_ok");

    if (error != nullptr) {
      error->clear();
    }
    AppendOrtTrace("run_exit_ok");
    return true;
  }

  RuntimeBackend ActiveBackend() const override { return active_backend_; }

 private:
  void RefreshBackendName() {
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
    const std::string runtime_version =
        (loader_ != nullptr) ? loader_->RuntimeVersionString() : std::string();
    const std::string loaded_library_path =
        (loader_ != nullptr) ? loader_->LoadedDllPath() : std::string();
    backend_name_ =
        BuildBackendName(active_backend_, runtime_version, loaded_library_path, provider_note_);
#else
    backend_name_ = BuildBackendName(active_backend_, {}, {}, provider_note_);
#endif
  }

  [[nodiscard]] std::string WithRuntimeDiagnostics(std::string_view message) const {
    if (loader_diagnostics_.empty()) {
      return std::string(message);
    }
    std::string combined(message);
    combined.append(" [");
    combined.append(loader_diagnostics_);
    combined.push_back(']');
    return combined;
  }

  void ResetTemporalHistory() const {
    temporal_history_.clear();
    temporal_history_capacity_frames_ = 0;
    temporal_history_valid_frames_ = 0;
    temporal_history_head_index_ = 0;
    temporal_history_frame_plane_size_ = 0U;
    temporal_history_last_frame_hash_ = 0U;
  }

  bool BuildTemporalInput(const PreparedModelInput& prepared_input,
                          int frame_count,
                          std::uint64_t frame_hash,
                          std::vector<float>* out_temporal_input,
                          std::string* error) const {
    if (out_temporal_input == nullptr) {
      if (error != nullptr) {
        *error = "internal error: temporal input output pointer is null";
      }
      return false;
    }

    const int clamped_frame_count = std::max(1, frame_count);
    if (clamped_frame_count <= 1) {
      *out_temporal_input = prepared_input.nchw_values;
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    const std::size_t frame_plane_size = prepared_input.nchw_values.size();
    if (frame_plane_size == 0U) {
      if (error != nullptr) {
        *error = "onnx runtime temporal input cannot be built from empty frame";
      }
      return false;
    }

    const bool requires_reset =
        temporal_history_capacity_frames_ != clamped_frame_count ||
        temporal_history_frame_plane_size_ != frame_plane_size;
    if (requires_reset) {
      temporal_history_capacity_frames_ = clamped_frame_count;
      temporal_history_valid_frames_ = 0;
      temporal_history_head_index_ = 0;
      temporal_history_frame_plane_size_ = frame_plane_size;
      temporal_history_.assign(
          static_cast<std::size_t>(clamped_frame_count) * frame_plane_size, 0.0F);
    } else if (temporal_history_.size() !=
               static_cast<std::size_t>(clamped_frame_count) * frame_plane_size) {
      temporal_history_.assign(
          static_cast<std::size_t>(clamped_frame_count) * frame_plane_size, 0.0F);
      temporal_history_valid_frames_ = 0;
      temporal_history_head_index_ = 0;
    }

    const bool duplicate_frame_hash =
        frame_hash != 0U && temporal_history_last_frame_hash_ != 0U &&
        frame_hash == temporal_history_last_frame_hash_ && temporal_history_valid_frames_ > 0;

    if (!duplicate_frame_hash && temporal_history_valid_frames_ > 0 &&
        temporal_history_capacity_frames_ > 0) {
      const int latest_slot =
          (temporal_history_head_index_ + temporal_history_valid_frames_ - 1) %
          temporal_history_capacity_frames_;
      const float* previous_frame =
          temporal_history_.data() + static_cast<std::size_t>(latest_slot) * frame_plane_size;
      const float mean_abs_diff = ComputeSampledMeanAbsDiff(
          previous_frame, prepared_input.nchw_values.data(), frame_plane_size);
      const float scene_cut_threshold =
          ParseFloatEnvOrDefault("ZSODA_DA3_MULTIVIEW_SCENE_CUT", 0.20F, 0.0F, 1.0F);
      if (mean_abs_diff >= scene_cut_threshold) {
        temporal_history_valid_frames_ = 0;
        temporal_history_head_index_ = 0;
        std::fill(temporal_history_.begin(), temporal_history_.end(), 0.0F);
        const std::string detail =
            "diff=" + std::to_string(mean_abs_diff) + ", threshold=" +
            std::to_string(scene_cut_threshold);
        AppendOrtTrace("temporal_history_reset_scene_cut", detail.c_str());
      }
    }

    if (!duplicate_frame_hash || temporal_history_valid_frames_ == 0) {
      int write_slot = 0;
      if (temporal_history_valid_frames_ < temporal_history_capacity_frames_) {
        write_slot = (temporal_history_head_index_ + temporal_history_valid_frames_) %
                     temporal_history_capacity_frames_;
        ++temporal_history_valid_frames_;
      } else {
        write_slot = temporal_history_head_index_;
        temporal_history_head_index_ =
            (temporal_history_head_index_ + 1) % temporal_history_capacity_frames_;
      }

      std::copy(prepared_input.nchw_values.begin(),
                prepared_input.nchw_values.end(),
                temporal_history_.begin() +
                    static_cast<std::size_t>(write_slot) * frame_plane_size);
    } else {
      AppendOrtTrace("temporal_history_reuse_duplicate_frame");
    }

    out_temporal_input->assign(
        static_cast<std::size_t>(clamped_frame_count) * frame_plane_size, 0.0F);
    const int pad_count = std::max(0, clamped_frame_count - temporal_history_valid_frames_);
    for (int t = 0; t < clamped_frame_count; ++t) {
      int history_index = 0;
      if (t >= pad_count) {
        history_index = t - pad_count;
      }
      history_index = std::clamp(history_index, 0, std::max(0, temporal_history_valid_frames_ - 1));
      const int slot =
          (temporal_history_head_index_ + history_index) % temporal_history_capacity_frames_;
      const float* src =
          temporal_history_.data() + static_cast<std::size_t>(slot) * frame_plane_size;
      float* dst = out_temporal_input->data() + static_cast<std::size_t>(t) * frame_plane_size;
      std::memcpy(dst, src, frame_plane_size * sizeof(float));
    }

    if (frame_hash != 0U) {
      temporal_history_last_frame_hash_ = frame_hash;
    }

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  RuntimeOptions options_;
  RuntimeBackend requested_backend_ = RuntimeBackend::kAuto;
  RuntimeBackend active_backend_ = RuntimeBackend::kCpu;
  std::string provider_note_base_;
  std::string provider_note_;
  std::string backend_name_;
  std::string loader_diagnostics_;
  bool initialized_ = false;
  std::string active_model_id_;
  std::string active_model_path_;
  ModelPipelineProfile active_model_profile_;
  int model_input_width_ = 0;
  int model_input_height_ = 0;
  int model_input_frame_count_ = 1;
  bool requested_multiview_ = false;
  int resolved_input_frame_count_ = 1;
  bool multiview_active_ = false;
  bool multiview_warning_ = false;
  mutable std::vector<float> temporal_history_;
  mutable int temporal_history_capacity_frames_ = 0;
  mutable int temporal_history_valid_frames_ = 0;
  mutable int temporal_history_head_index_ = 0;
  mutable std::size_t temporal_history_frame_plane_size_ = 0U;
  mutable std::uint64_t temporal_history_last_frame_hash_ = 0U;
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
  std::unique_ptr<OrtDynamicLoader> loader_;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::SessionOptions> session_options_;
  std::unique_ptr<Ort::Session> session_;
  std::string input_name_;
  std::string output_name_;
  bool input_has_image_dimension_ = false;
#endif
};

}  // namespace

std::unique_ptr<IOnnxRuntimeBackend> CreateOnnxRuntimeBackend(const RuntimeOptions& options,
                                                              std::string* error) {
  auto backend = std::make_unique<OnnxRuntimeBackendScaffold>(options);
  std::string init_error;
  if (!backend->Initialize(&init_error)) {
    if (error != nullptr) {
      *error = init_error;
    }
    return nullptr;
  }

  if (error != nullptr) {
    error->clear();
  }
  return backend;
}

}  // namespace zsoda::inference
