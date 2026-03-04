#include "inference/OnnxRuntimeBackend.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdint>
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

RuntimeBackend ResolveActiveBackend(RuntimeBackend requested_backend) {
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
  // API-enabled path currently executes on CPU first.
  (void)requested_backend;
  return RuntimeBackend::kCpu;
#else
  if (requested_backend == RuntimeBackend::kAuto) {
    return RuntimeBackend::kCpu;
  }
  return requested_backend;
#endif
}

struct ModelPipelineProfile {
  int input_width = 384;
  int input_height = 384;
  float normalize_bias = 0.5F;
  float normalize_scale = 0.5F;
  bool invert_depth = false;
};

struct PreparedModelInput {
  int source_width = 0;
  int source_height = 0;
  int tensor_width = 0;
  int tensor_height = 0;
  int tensor_channels = 3;
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

std::string BuildProviderNote(RuntimeBackend requested_backend, RuntimeBackend active_backend) {
  if (requested_backend == RuntimeBackend::kAuto || requested_backend == active_backend) {
    return {};
  }
  std::string note = "provider_request=";
  note.append(RuntimeBackendName(requested_backend));
  note.append("->");
  note.append(RuntimeBackendName(active_backend));
  note.append(" (provider wiring pending)");
  return note;
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
                              RuntimeBackend active_backend,
                              std::string* capability_note,
                              std::string* error) {
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

  std::vector<std::string> providers;
  providers.reserve(provider_length > 0 ? static_cast<std::size_t>(provider_length) : 0U);
  bool has_cpu_provider = false;
  for (int i = 0; i < provider_length; ++i) {
    const char* provider = (providers_raw != nullptr) ? providers_raw[i] : nullptr;
    const std::string provider_name = SafeCStr(provider, "<null provider>");
    providers.push_back(provider_name);
    if (ToLowerCopy(provider_name) == "cpuexecutionprovider") {
      has_cpu_provider = true;
    }
  }

  if (api->ReleaseAvailableProviders != nullptr) {
    api->ReleaseAvailableProviders(providers_raw, provider_length);
  }

  if (active_backend == RuntimeBackend::kCpu && !has_cpu_provider) {
    if (error != nullptr) {
      *error = "onnx runtime capability probe failed: CPUExecutionProvider is unavailable";
    }
    return false;
  }

  std::ostringstream capability;
  capability << "providers=" << JoinProviders(providers);
  if (api->GetBuildInfoString != nullptr) {
    const char* build_info = api->GetBuildInfoString();
    if (build_info != nullptr && build_info[0] != '\0') {
      capability << ", build_info=" << build_info;
    }
  }

  if (capability_note != nullptr) {
    *capability_note = capability.str();
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
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
    return {
        518,
        518,
        0.5F,
        0.5F,
        false,
    };
  }
  if (model_id.rfind("midas-", 0) == 0) {
    return {
        384,
        384,
        0.5F,
        0.5F,
        true,
    };
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
  const float normalize_denom = (profile.normalize_scale == 0.0F) ? 1.0F : profile.normalize_scale;

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
    const float src_y = (static_cast<float>(y) + 0.5F) *
                            (static_cast<float>(src_desc.height) /
                             static_cast<float>(tensor_height)) -
                        0.5F;
    for (int x = 0; x < tensor_width; ++x) {
      const float src_x = (static_cast<float>(x) + 0.5F) *
                              (static_cast<float>(src_desc.width) /
                               static_cast<float>(tensor_width)) -
                          0.5F;
      const std::size_t output_index =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(tensor_width) + x;

      float normalized_channels[3] = {};
      for (int channel = 0; channel < 3; ++channel) {
        const float sampled = std::clamp(sample_channel(src_x, src_y, channel), 0.0F, 1.0F);
        const float normalized = (sampled - profile.normalize_bias) / normalize_denom;
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

  for (int y = 0; y < output_desc.height; ++y) {
    const float src_y = (static_cast<float>(y) + 0.5F) *
                            (static_cast<float>(raw_output.height) /
                             static_cast<float>(output_desc.height)) -
                        0.5F;
    for (int x = 0; x < output_desc.width; ++x) {
      const float src_x = (static_cast<float>(x) + 0.5F) *
                              (static_cast<float>(raw_output.width) /
                               static_cast<float>(output_desc.width)) -
                          0.5F;

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

      float normalized = (resized_depth - near_value) / range;
      if (profile.invert_depth) {
        normalized = 1.0F - normalized;
      }
      out_depth->at(x, y, 0) = std::clamp(normalized, 0.0F, 1.0F);
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
                      int* input_width,
                      int* input_height,
                      bool* input_has_image_dimension,
                      std::string* error) {
  if (input_name == nullptr || output_name == nullptr || input_width == nullptr ||
      input_height == nullptr || input_has_image_dimension == nullptr) {
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
  auto output_name_alloc = session.GetOutputNameAllocated(0U, allocator);
  *input_name = input_name_alloc.get() != nullptr ? input_name_alloc.get() : "";
  *output_name = output_name_alloc.get() != nullptr ? output_name_alloc.get() : "";
  if (input_name->empty() || output_name->empty()) {
    if (error != nullptr) {
      *error = "onnx runtime session has empty input/output names";
    }
    return false;
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

  if (batch > 0 && batch != 1) {
    if (error != nullptr) {
      *error = "onnx runtime session only supports batch size 1";
    }
    return false;
  }
  if (has_image_dimension && num_images > 0 && num_images != 1) {
    if (error != nullptr) {
      *error = "onnx runtime session only supports num_images 1 for BNCHW input";
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

  const auto output_info = session.GetOutputTypeInfo(0U).GetTensorTypeAndShapeInfo();
  if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    if (error != nullptr) {
      *error = "onnx runtime session output must be float tensor";
    }
    return false;
  }

  *input_width = width > 0 ? static_cast<int>(width) : 0;
  *input_height = height > 0 ? static_cast<int>(height) : 0;
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

  if (shape.size() == 4U) {
    const int64_t batch = shape[0];
    const int64_t channels = shape[1];
    const int64_t output_height = shape[2];
    const int64_t output_width = shape[3];
    if (batch < 1 || channels < 1 || output_height < 1 || output_width < 1) {
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
    if (tensor_element_count < pixel_count) {
      if (error != nullptr) {
        *error = "onnx runtime output tensor element count is smaller than required depth pixels";
      }
      return false;
    }
    if (!CopyTensorPrefix(data, pixel_count, &raw_output->depth_values, error)) {
      return false;
    }
  } else if (shape.size() == 3U) {
    const int64_t batch = shape[0];
    const int64_t output_height = shape[1];
    const int64_t output_width = shape[2];
    if (batch < 1 || output_height < 1 || output_width < 1) {
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
    if (tensor_element_count < pixel_count) {
      if (error != nullptr) {
        *error = "onnx runtime output tensor element count is smaller than required depth pixels";
      }
      return false;
    }
    if (!CopyTensorPrefix(data, pixel_count, &raw_output->depth_values, error)) {
      return false;
    }
  } else if (shape.size() == 2U) {
    const int64_t output_height = shape[0];
    const int64_t output_width = shape[1];
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
    if (tensor_element_count < pixel_count) {
      if (error != nullptr) {
        *error = "onnx runtime output tensor element count is smaller than required depth pixels";
      }
      return false;
    }
    if (!CopyTensorPrefix(data, pixel_count, &raw_output->depth_values, error)) {
      return false;
    }
  } else {
    if (error != nullptr) {
      *error = "onnx runtime output shape is unsupported";
    }
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
        active_backend_(ResolveActiveBackend(requested_backend_)),
        provider_note_(BuildProviderNote(requested_backend_, active_backend_)),
        backend_name_(BuildBackendName(active_backend_, "", "", provider_note_)) {}

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
    std::string loader_error;
    if (!loader_->Load(configured_library_path, requested_api_version, &loader_error)) {
      initialized_ = false;
      if (error != nullptr) {
        *error = loader_error.empty() ? "onnx runtime dynamic loader failed" : loader_error;
      }
      return false;
    }

    std::string capability_note;
    std::string capability_error;
    if (!QueryRuntimeCapabilities(loader_->Api(), active_backend_, &capability_note, &capability_error)) {
      initialized_ = false;
      if (error != nullptr) {
        const std::string capability_detail =
            capability_error.empty() ? "onnx runtime capability probe failed" : capability_error;
        *error = capability_detail + " [" + loader_->Diagnostics() + "]";
      }
      return false;
    }

    if (!capability_note.empty()) {
      if (provider_note_.empty()) {
        provider_note_ = capability_note;
      } else {
        provider_note_.append("; ");
        provider_note_.append(capability_note);
      }
    }

    Ort::InitApi(loader_->Api());
    loader_diagnostics_ = loader_->Diagnostics();
    backend_name_ = BuildBackendName(active_backend_,
                                     loader_->RuntimeVersionString(),
                                     loader_->LoadedDllPath(),
                                     provider_note_);

    try {
      env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "zsoda-ort");
      session_options_ = std::make_unique<Ort::SessionOptions>();
      session_options_->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      session_options_->SetIntraOpNumThreads(1);
      session_options_->SetInterOpNumThreads(1);
    } catch (const Ort::Exception& ex) {
      initialized_ = false;
      if (error != nullptr) {
        *error = WithRuntimeDiagnostics(std::string("onnx runtime initialize failed: ") +
                                        SafeCStr(ex.what()));
      }
      return false;
    }
#else
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
      bool resolved_input_has_image_dimension = false;
      std::string io_error;
      if (!ResolveSessionIo(*session,
                            &resolved_input_name,
                            &resolved_output_name,
                            &resolved_input_width,
                            &resolved_input_height,
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
      const std::string prepared_detail =
          "tensor=" + std::to_string(prepared_input.tensor_width) + "x" +
          std::to_string(prepared_input.tensor_height) + "x" +
          std::to_string(prepared_input.tensor_channels) +
          ", image_dim=" + std::string(has_image_dimension ? "1" : "0");
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
        1,
        prepared_input.tensor_channels,
        prepared_input.tensor_height,
        prepared_input.tensor_width,
    };
    const std::array<int64_t, 4> input_shape_nchw = {
        1,
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
    if (!CheckedMultiplySize(prepared_input.nchw_values.size(), sizeof(float), &input_byte_count)) {
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
              const_cast<float*>(prepared_input.nchw_values.data()),
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
      if (!ExtractDepthOutputFromOrtValue(api, output_tensor, &raw_output, error)) {
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

  RuntimeOptions options_;
  RuntimeBackend requested_backend_ = RuntimeBackend::kAuto;
  RuntimeBackend active_backend_ = RuntimeBackend::kCpu;
  std::string provider_note_;
  std::string backend_name_;
  std::string loader_diagnostics_;
  bool initialized_ = false;
  std::string active_model_id_;
  std::string active_model_path_;
  ModelPipelineProfile active_model_profile_;
  int model_input_width_ = 0;
  int model_input_height_ = 0;
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
