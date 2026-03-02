#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "inference/DummyInferenceEngine.h"
#include "inference/InferenceEngine.h"
#include "inference/ModelCatalog.h"

namespace zsoda::inference {

class ManagedInferenceEngine final : public IInferenceEngine {
 public:
  explicit ManagedInferenceEngine(std::string model_root);

  const char* Name() const override { return "ManagedInferenceEngine"; }
  bool Initialize(const std::string& model_id, std::string* error) override;
  bool SelectModel(const std::string& model_id, std::string* error) override;
  std::vector<std::string> ListModelIds() const override;
  std::string ActiveModelId() const override;
  bool Run(const InferenceRequest& request,
           zsoda::core::FrameBuffer* out_depth,
           std::string* error) const override;

 private:
  bool SelectModelLocked(const std::string& model_id, std::string* error);
  float ModelBias() const;

  std::string model_root_;
  ModelCatalog catalog_;
  mutable std::mutex mutex_;
  std::string active_model_id_;
  std::string active_model_path_;
  bool model_file_exists_ = false;
  DummyInferenceEngine fallback_engine_;
};

}  // namespace zsoda::inference
