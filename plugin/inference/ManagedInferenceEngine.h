#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "inference/DummyInferenceEngine.h"
#include "inference/InferenceEngine.h"
#include "inference/ModelCatalog.h"
#include "inference/RuntimeOptions.h"
#if defined(ZSODA_WITH_ONNX_RUNTIME)
#include "inference/OnnxRuntimeBackend.h"
#endif

namespace zsoda::inference {

class ManagedInferenceEngine final : public IInferenceEngine {
 public:
  explicit ManagedInferenceEngine(std::string model_root);
  ManagedInferenceEngine(std::string model_root, RuntimeOptions options);

  const char* Name() const override { return "ManagedInferenceEngine"; }
  bool Initialize(const std::string& model_id, std::string* error) override;
  bool SelectModel(const std::string& model_id, std::string* error) override;
  std::vector<std::string> ListModelIds() const override;
  std::string ActiveModelId() const override;
  bool Run(const InferenceRequest& request,
           zsoda::core::FrameBuffer* out_depth,
           std::string* error) const override;
  [[nodiscard]] RuntimeBackend RequestedBackend() const;
  [[nodiscard]] RuntimeBackend ActiveBackend() const;
  [[nodiscard]] bool UsingFallbackEngine() const;

 private:
  void ConfigureBackend();
  void LoadManifest();
  bool SelectModelLocked(const std::string& model_id, std::string* error);
  float ModelBias() const;

  std::string model_root_;
  RuntimeOptions options_;
  ModelCatalog catalog_;
  mutable std::mutex mutex_;
  std::string active_model_id_;
  std::string active_model_path_;
  bool model_file_exists_ = false;
  RuntimeBackend active_backend_ = RuntimeBackend::kCpu;
  bool using_fallback_engine_ = true;
  DummyInferenceEngine fallback_engine_;
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  std::unique_ptr<IOnnxRuntimeBackend> onnx_backend_;
#endif
};

}  // namespace zsoda::inference
