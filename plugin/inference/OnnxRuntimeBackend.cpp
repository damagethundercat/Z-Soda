#include "inference/OnnxRuntimeBackend.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace zsoda::inference {
namespace {

constexpr int kMinimumModelInputSize = 32;

RuntimeBackend ResolveActiveBackend(RuntimeBackend requested_backend) {
  if (requested_backend == RuntimeBackend::kAuto) {
    return RuntimeBackend::kCpu;
  }
  return requested_backend;
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
};

struct RawDepthOutput {
  int width = 0;
  int height = 0;
  float min_depth = 0.0F;
  float max_depth = 1.0F;
};

std::string ToLowerCopy(std::string_view text) {
  std::string lowered;
  lowered.reserve(text.size());
  for (const char ch : text) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered;
}

std::string BuildBackendName(RuntimeBackend active_backend) {
  std::string name = "OnnxRuntimeBackendScaffold[";
  name.append(RuntimeBackendName(active_backend));
  name.push_back(']');
  return name;
}

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

bool PrepareInputForModel(const InferenceRequest& request,
                          const ModelPipelineProfile& profile,
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
  prepared_input->tensor_width = std::max(kMinimumModelInputSize, profile.input_width);
  prepared_input->tensor_height = std::max(kMinimumModelInputSize, profile.input_height);
  prepared_input->tensor_channels = 3;

  const float denom = (profile.normalize_scale == 0.0F) ? 1.0F : profile.normalize_scale;
  float running_sum = 0.0F;
  float running_min = std::numeric_limits<float>::infinity();
  float running_max = -std::numeric_limits<float>::infinity();
  const int pixel_count = src_desc.width * src_desc.height;

  for (int y = 0; y < src_desc.height; ++y) {
    for (int x = 0; x < src_desc.width; ++x) {
      const float red = source.at(x, y, 0);
      const float green = source.at(x, y, std::min(1, src_desc.channels - 1));
      const float blue = source.at(x, y, std::min(2, src_desc.channels - 1));
      const float luma = red * 0.2126F + green * 0.7152F + blue * 0.0722F;
      const float normalized = (luma - profile.normalize_bias) / denom;
      running_sum += normalized;
      running_min = std::min(running_min, normalized);
      running_max = std::max(running_max, normalized);
    }
  }

  if (pixel_count <= 0) {
    if (error != nullptr) {
      *error = "invalid inference request: source frame has no pixels";
    }
    return false;
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
  const float inv_w = 1.0F / static_cast<float>(std::max(1, output_desc.width - 1));
  const float inv_h = 1.0F / static_cast<float>(std::max(1, output_desc.height - 1));

  for (int y = 0; y < output_desc.height; ++y) {
    for (int x = 0; x < output_desc.width; ++x) {
      const float blend = (static_cast<float>(x) * inv_w + static_cast<float>(y) * inv_h) * 0.5F;
      float normalized = (near_value + blend * range - near_value) / range;
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

class OnnxRuntimeBackendScaffold final : public IOnnxRuntimeBackend {
 public:
  explicit OnnxRuntimeBackendScaffold(RuntimeBackend requested_backend)
      : active_backend_(ResolveActiveBackend(requested_backend)),
        backend_name_(BuildBackendName(active_backend_)) {}

  const char* Name() const override { return backend_name_.c_str(); }

  bool Initialize(std::string* error) override {
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

    active_model_profile_ = ResolvePipelineProfile(model_id);
    active_model_id_ = model_id;
    active_model_path_ = candidate_path.lexically_normal().string();
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool Run(const InferenceRequest& request,
           zsoda::core::FrameBuffer* out_depth,
           std::string* error) const override {
    if (!initialized_) {
      if (error != nullptr) {
        *error = "onnx runtime backend is not initialized";
      }
      return false;
    }
    if (active_model_id_.empty()) {
      if (error != nullptr) {
        *error = "onnx runtime backend has no active model";
      }
      return false;
    }
    if (active_model_path_.empty()) {
      if (error != nullptr) {
        *error = "onnx runtime backend has no active model path";
      }
      return false;
    }
    if (!ValidateRequest(request, out_depth, error)) {
      return false;
    }

    PreparedModelInput prepared_input;
    if (!PrepareInputForModel(request, active_model_profile_, &prepared_input, error)) {
      return false;
    }

    RawDepthOutput raw_output;
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

    if (!PostprocessDepthForModel(raw_output,
                                  active_model_profile_,
                                  request.source->desc(),
                                  out_depth,
                                  error)) {
      return false;
    }

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  RuntimeBackend ActiveBackend() const override {
    return active_backend_;
  }

 private:
  RuntimeBackend active_backend_ = RuntimeBackend::kCpu;
  std::string backend_name_;
  bool initialized_ = false;
  std::string active_model_id_;
  std::string active_model_path_;
  ModelPipelineProfile active_model_profile_;
};

}  // namespace

std::unique_ptr<IOnnxRuntimeBackend> CreateOnnxRuntimeBackend(const RuntimeOptions& options,
                                                              std::string* error) {
  auto backend = std::make_unique<OnnxRuntimeBackendScaffold>(options.preferred_backend);
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
