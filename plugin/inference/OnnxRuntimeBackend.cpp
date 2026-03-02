#include "inference/OnnxRuntimeBackend.h"

#include <memory>
#include <string>
#include <utility>

namespace zsoda::inference {
namespace {

RuntimeBackend ResolveActiveBackend(RuntimeBackend requested_backend) {
  if (requested_backend == RuntimeBackend::kAuto) {
    return RuntimeBackend::kCpu;
  }
  return requested_backend;
}

class OnnxRuntimeBackendPlaceholder final : public IOnnxRuntimeBackend {
 public:
  explicit OnnxRuntimeBackendPlaceholder(RuntimeBackend requested_backend)
      : active_backend_(ResolveActiveBackend(requested_backend)) {}

  const char* Name() const override { return "OnnxRuntimeBackendPlaceholder"; }

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

    active_model_id_ = model_id;
    active_model_path_ = model_path;
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
    if (request.source == nullptr || out_depth == nullptr) {
      if (error != nullptr) {
        *error = "invalid inference request";
      }
      return false;
    }

    if (error != nullptr) {
      *error = "onnx runtime backend scaffold is enabled but execution is not wired";
    }
    return false;
  }

  RuntimeBackend ActiveBackend() const override {
    return active_backend_;
  }

 private:
  RuntimeBackend active_backend_ = RuntimeBackend::kCpu;
  bool initialized_ = false;
  std::string active_model_id_;
  std::string active_model_path_;
};

}  // namespace

std::unique_ptr<IOnnxRuntimeBackend> CreateOnnxRuntimeBackend(const RuntimeOptions& options,
                                                              std::string* error) {
  auto backend = std::make_unique<OnnxRuntimeBackendPlaceholder>(options.preferred_backend);
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
