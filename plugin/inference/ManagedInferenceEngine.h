#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/CompatMutex.h"
#include "inference/DummyInferenceEngine.h"
#include "inference/InferenceEngine.h"
#include "inference/ModelCatalog.h"
#include "inference/RuntimeOptions.h"
#if defined(ZSODA_WITH_ONNX_RUNTIME)
#include "inference/OnnxRuntimeBackend.h"
#endif

namespace zsoda::inference {

struct InferenceBackendStatus {
  RuntimeBackend requested_backend = RuntimeBackend::kAuto;
  RuntimeBackend active_backend = RuntimeBackend::kCpu;
  bool using_fallback_engine = true;
  bool last_run_used_fallback = true;
  std::string active_backend_name;
  std::string engine_name;
  std::string fallback_reason;
};

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
  [[nodiscard]] InferenceBackendStatus BackendStatus() const;
  [[nodiscard]] std::string BackendStatusString() const;

 private:
  void ConfigureBackend();
  void LoadManifest();
  bool SelectModelLocked(const std::string& model_id, std::string* error);
  void TryPromoteActiveModelToOnnxLocked();
  void MaybeQueueModelDownloadLocked(const ModelSpec& model, const std::string& model_path);
  float ModelBias() const;

  std::string model_root_;
  RuntimeOptions options_;
  ModelCatalog catalog_;
  mutable zsoda::core::CompatMutex mutex_;
  std::string active_model_id_;
  std::string active_model_path_;
  bool model_file_exists_ = false;
  RuntimeBackend active_backend_ = RuntimeBackend::kCpu;
  bool using_fallback_engine_ = true;
  mutable bool last_run_used_fallback_ = true;
  mutable std::string fallback_reason_;
  DummyInferenceEngine fallback_engine_;
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  std::unique_ptr<IOnnxRuntimeBackend> onnx_backend_;
#endif
};

}  // namespace zsoda::inference
