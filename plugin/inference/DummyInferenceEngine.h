#pragma once

#include <string>

#include "inference/InferenceEngine.h"

namespace zsoda::inference {

class DummyInferenceEngine final : public IInferenceEngine {
 public:
  const char* Name() const override { return "DummyDepthEngine"; }
  bool Initialize(const std::string& model_id, std::string* error) override;
  bool SelectModel(const std::string& model_id, std::string* error) override;
  std::vector<std::string> ListModelIds() const override;
  std::string ActiveModelId() const override;
  bool Run(const InferenceRequest& request,
           zsoda::core::FrameBuffer* out_depth,
           std::string* error) const override;

 private:
  std::string model_id_ = "dummy";
};

}  // namespace zsoda::inference
