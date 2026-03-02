#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/Frame.h"

namespace zsoda::inference {

struct InferenceRequest {
  const zsoda::core::FrameBuffer* source = nullptr;
  int quality = 1;
};

class IInferenceEngine {
 public:
  virtual ~IInferenceEngine() = default;

  virtual const char* Name() const = 0;
  virtual bool Initialize(const std::string& model_id, std::string* error) = 0;
  virtual bool SelectModel(const std::string& model_id, std::string* error) = 0;
  virtual std::vector<std::string> ListModelIds() const = 0;
  virtual std::string ActiveModelId() const = 0;
  virtual bool Run(const InferenceRequest& request,
                   zsoda::core::FrameBuffer* out_depth,
                   std::string* error) const = 0;
};

std::shared_ptr<IInferenceEngine> CreateDefaultEngine();

}  // namespace zsoda::inference
