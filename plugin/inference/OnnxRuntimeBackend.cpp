#include "inference/OnnxRuntimeBackend.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
#include <onnxruntime_cxx_api.h>
#endif

namespace zsoda::inference {
namespace {

constexpr int kMinimumModelInputSize = 32;

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

std::string BuildBackendName(RuntimeBackend active_backend) {
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
  std::string name = "OnnxRuntimeBackend[";
#else
  std::string name = "OnnxRuntimeBackendScaffold[";
#endif
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
                      std::string* error) {
  if (input_name == nullptr || output_name == nullptr || input_width == nullptr ||
      input_height == nullptr) {
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
  if (input_shape.size() != 4U) {
    if (error != nullptr) {
      *error = "onnx runtime session input shape must be NCHW";
    }
    return false;
  }

  const int64_t batch = input_shape[0];
  const int64_t channels = input_shape[1];
  const int64_t height = input_shape[2];
  const int64_t width = input_shape[3];
  if (batch > 0 && batch != 1) {
    if (error != nullptr) {
      *error = "onnx runtime session only supports batch size 1";
    }
    return false;
  }
  if (channels > 0 && channels != 3) {
    if (error != nullptr) {
      *error = "onnx runtime session input channel must be 3 for NCHW preprocessing";
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
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool ExtractDepthOutput(const Ort::Value& output, RawDepthOutput* raw_output, std::string* error) {
  if (raw_output == nullptr) {
    if (error != nullptr) {
      *error = "internal error: raw output is null";
    }
    return false;
  }
  if (!output.IsTensor()) {
    if (error != nullptr) {
      *error = "onnx runtime output is not a tensor";
    }
    return false;
  }

  const auto tensor_info = output.GetTensorTypeAndShapeInfo();
  if (tensor_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor must be float";
    }
    return false;
  }

  const std::vector<int64_t> shape = tensor_info.GetShape();
  const float* data = output.GetTensorData<float>();
  if (data == nullptr) {
    if (error != nullptr) {
      *error = "onnx runtime output tensor data pointer is null";
    }
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

    width = static_cast<int>(output_width);
    height = static_cast<int>(output_height);
    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    raw_output->depth_values.assign(pixel_count, 0.0F);

    const std::size_t channel_stride = pixel_count;
    const std::size_t batch_stride =
        static_cast<std::size_t>(channels) * static_cast<std::size_t>(height) *
        static_cast<std::size_t>(width);
    const std::size_t base_index = 0U * batch_stride + 0U * channel_stride;

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t output_index =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x;
        raw_output->depth_values[output_index] =
            data[base_index + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x];
      }
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

    width = static_cast<int>(output_width);
    height = static_cast<int>(output_height);
    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    raw_output->depth_values.assign(pixel_count, 0.0F);

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t output_index =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x;
        raw_output->depth_values[output_index] = data[output_index];
      }
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

    width = static_cast<int>(output_width);
    height = static_cast<int>(output_height);
    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    raw_output->depth_values.assign(pixel_count, 0.0F);

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t output_index =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x;
        raw_output->depth_values[output_index] = data[output_index];
      }
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
  explicit OnnxRuntimeBackendScaffold(RuntimeBackend requested_backend)
      : active_backend_(ResolveActiveBackend(requested_backend)),
        backend_name_(BuildBackendName(active_backend_)) {}

  const char* Name() const override { return backend_name_.c_str(); }

  bool Initialize(std::string* error) override {
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
    try {
      env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "zsoda-ort");
      session_options_ = std::make_unique<Ort::SessionOptions>();
      session_options_->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      session_options_->SetIntraOpNumThreads(1);
      session_options_->SetInterOpNumThreads(1);
    } catch (const Ort::Exception& ex) {
      initialized_ = false;
      if (error != nullptr) {
        *error = std::string("onnx runtime initialize failed: ") + ex.what();
      }
      return false;
    }
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

#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
    if (env_ == nullptr || session_options_ == nullptr) {
      if (error != nullptr) {
        *error = "onnx runtime backend is not initialized";
      }
      return false;
    }

    try {
#if defined(_WIN32)
      const std::wstring ort_path = candidate_path.native();
      auto session = std::make_unique<Ort::Session>(*env_, ort_path.c_str(), *session_options_);
#else
      const std::string ort_path = candidate_path.string();
      auto session = std::make_unique<Ort::Session>(*env_, ort_path.c_str(), *session_options_);
#endif
      std::string resolved_input_name;
      std::string resolved_output_name;
      int resolved_input_width = 0;
      int resolved_input_height = 0;
      std::string io_error;
      if (!ResolveSessionIo(*session,
                            &resolved_input_name,
                            &resolved_output_name,
                            &resolved_input_width,
                            &resolved_input_height,
                            &io_error)) {
        if (error != nullptr) {
          *error = io_error;
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
    } catch (const Ort::Exception& ex) {
      session_.reset();
      input_name_.clear();
      output_name_.clear();
      if (error != nullptr) {
        *error = std::string("onnx runtime session create failed: ") + ex.what();
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
    if (!PrepareInputForModel(request,
                              active_model_profile_,
                              model_input_width_,
                              model_input_height_,
                              &prepared_input,
                              error)) {
      return false;
    }

    RawDepthOutput raw_output;
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
    if (session_ == nullptr || input_name_.empty() || output_name_.empty()) {
      if (error != nullptr) {
        *error = "onnx runtime backend has no active session";
      }
      return false;
    }
    if (prepared_input.nchw_values.empty()) {
      if (error != nullptr) {
        *error = "onnx runtime backend prepared input is empty";
      }
      return false;
    }

    const std::array<int64_t, 4> input_shape = {
        1,
        prepared_input.tensor_channels,
        prepared_input.tensor_height,
        prepared_input.tensor_width,
    };
    const std::array<const char*, 1> input_names = {input_name_.c_str()};
    const std::array<const char*, 1> output_names = {output_name_.c_str()};

    try {
      Ort::MemoryInfo memory_info =
          Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
          memory_info,
          const_cast<float*>(prepared_input.nchw_values.data()),
          prepared_input.nchw_values.size(),
          input_shape.data(),
          input_shape.size());
      auto outputs = session_->Run(Ort::RunOptions{nullptr},
                                   input_names.data(),
                                   &input_tensor,
                                   input_names.size(),
                                   output_names.data(),
                                   output_names.size());
      if (outputs.empty()) {
        if (error != nullptr) {
          *error = "onnx runtime run returned no outputs";
        }
        return false;
      }
      if (!ExtractDepthOutput(outputs.front(), &raw_output, error)) {
        return false;
      }
    } catch (const Ort::Exception& ex) {
      if (error != nullptr) {
        *error = std::string("onnx runtime run failed: ") + ex.what();
      }
      return false;
    }
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
      return false;
    }

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  RuntimeBackend ActiveBackend() const override { return active_backend_; }

 private:
  RuntimeBackend active_backend_ = RuntimeBackend::kCpu;
  std::string backend_name_;
  bool initialized_ = false;
  std::string active_model_id_;
  std::string active_model_path_;
  ModelPipelineProfile active_model_profile_;
  int model_input_width_ = 0;
  int model_input_height_ = 0;
#if defined(ZSODA_WITH_ONNX_RUNTIME_API) && ZSODA_WITH_ONNX_RUNTIME_API
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::SessionOptions> session_options_;
  std::unique_ptr<Ort::Session> session_;
  std::string input_name_;
  std::string output_name_;
#endif
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
