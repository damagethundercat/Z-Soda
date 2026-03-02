#include "inference/ManagedInferenceEngine.h"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace zsoda::inference {

ManagedInferenceEngine::ManagedInferenceEngine(std::string model_root)
    : ManagedInferenceEngine(std::move(model_root), RuntimeOptions{}) {}

ManagedInferenceEngine::ManagedInferenceEngine(std::string model_root, RuntimeOptions options)
    : model_root_(std::move(model_root)), options_(std::move(options)) {
  ConfigureBackend();
  LoadManifest();
}

bool ManagedInferenceEngine::Initialize(const std::string& model_id, std::string* error) {
  std::scoped_lock lock(mutex_);
  const std::string target = model_id.empty() ? catalog_.DefaultModelId() : model_id;
  return SelectModelLocked(target, error);
}

bool ManagedInferenceEngine::SelectModel(const std::string& model_id, std::string* error) {
  std::scoped_lock lock(mutex_);
  if (model_id.empty()) {
    if (error) {
      *error = "model id cannot be empty";
    }
    return false;
  }
  if (model_id == active_model_id_) {
    return true;
  }
  return SelectModelLocked(model_id, error);
}

std::vector<std::string> ManagedInferenceEngine::ListModelIds() const {
  std::scoped_lock lock(mutex_);
  std::vector<std::string> result;
  result.reserve(catalog_.models().size());
  for (const auto& model : catalog_.models()) {
    result.push_back(model.id);
  }
  return result;
}

std::string ManagedInferenceEngine::ActiveModelId() const {
  std::scoped_lock lock(mutex_);
  return active_model_id_;
}

bool ManagedInferenceEngine::Run(const InferenceRequest& request,
                                 zsoda::core::FrameBuffer* out_depth,
                                 std::string* error) const {
  std::scoped_lock lock(mutex_);
  if (active_model_id_.empty()) {
    if (error) {
      *error = "model is not selected";
    }
    return false;
  }

  std::string dummy_error;
  if (!fallback_engine_.Run(request, out_depth, &dummy_error)) {
    if (error) {
      *error = dummy_error;
    }
    return false;
  }

  // Apply a small model-specific curve shift so model switching has visible effect,
  // even before ONNX Runtime backend wiring is added.
  const auto& desc = out_depth->desc();
  const float bias = ModelBias();
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      float value = out_depth->at(x, y, 0);
      value = std::clamp(value + bias, 0.0F, 1.0F);
      out_depth->at(x, y, 0) = value;
    }
  }

  if (!model_file_exists_ && error != nullptr) {
    *error = "selected model file is not installed; using fallback depth path";
  } else if (error != nullptr) {
    error->clear();
  }
  return true;
}

RuntimeBackend ManagedInferenceEngine::RequestedBackend() const {
  return options_.preferred_backend;
}

RuntimeBackend ManagedInferenceEngine::ActiveBackend() const {
  return active_backend_;
}

bool ManagedInferenceEngine::UsingFallbackEngine() const {
  return using_fallback_engine_;
}

void ManagedInferenceEngine::ConfigureBackend() {
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  using_fallback_engine_ = false;
  active_backend_ = options_.preferred_backend == RuntimeBackend::kAuto
                        ? RuntimeBackend::kCpu
                        : options_.preferred_backend;
#else
  using_fallback_engine_ = true;
  active_backend_ = RuntimeBackend::kCpu;
#endif
}

void ManagedInferenceEngine::LoadManifest() {
  std::string manifest_error;
  if (!options_.model_manifest_path.empty()) {
    catalog_.LoadManifestFile(options_.model_manifest_path, &manifest_error);
    return;
  }
  catalog_.LoadManifestFromRoot(model_root_, &manifest_error);
}

bool ManagedInferenceEngine::SelectModelLocked(const std::string& model_id, std::string* error) {
  const auto* model = catalog_.FindById(model_id);
  if (model == nullptr) {
    if (error) {
      *error = "unknown model id: " + model_id;
    }
    return false;
  }

  const std::string model_path = catalog_.ResolveModelPath(model_root_, model_id);
  const bool exists = std::filesystem::exists(model_path);
  std::string init_error;
  if (!fallback_engine_.Initialize(model_id, &init_error)) {
    if (error) {
      *error = init_error;
    }
    return false;
  }

  active_model_id_ = model_id;
  active_model_path_ = model_path;
  model_file_exists_ = exists;
  if (error) {
    if (exists) {
      error->clear();
    } else {
      *error = "model file not found: " + model_path;
    }
  }
  return true;
}

float ManagedInferenceEngine::ModelBias() const {
  if (active_model_id_ == "depth-anything-v3-large") {
    return 0.08F;
  }
  if (active_model_id_ == "depth-anything-v3-base") {
    return 0.04F;
  }
  if (active_model_id_ == "depth-anything-v3-small") {
    return 0.02F;
  }
  if (active_model_id_ == "midas-dpt-large") {
    return -0.03F;
  }
  return 0.0F;
}

}  // namespace zsoda::inference
