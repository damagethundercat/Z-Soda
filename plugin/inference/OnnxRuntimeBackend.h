#pragma once

#include <memory>
#include <string>

#include "inference/InferenceEngine.h"
#include "inference/RuntimeOptions.h"

namespace zsoda::inference {

class IOnnxRuntimeBackend {
 public:
  virtual ~IOnnxRuntimeBackend() = default;

  virtual const char* Name() const = 0;
  virtual bool Initialize(std::string* error) = 0;
  virtual bool SelectModel(const std::string& model_id,
                           const std::string& model_path,
                           std::string* error) = 0;
  virtual bool Run(const InferenceRequest& request,
                   zsoda::core::FrameBuffer* out_depth,
                   std::string* error) const = 0;
  virtual RuntimeBackend ActiveBackend() const = 0;
};

std::unique_ptr<IOnnxRuntimeBackend> CreateOnnxRuntimeBackend(const RuntimeOptions& options,
                                                              std::string* error);
std::unique_ptr<IOnnxRuntimeBackend> CreateRemoteInferenceBackend(const RuntimeOptions& options,
                                                                  std::string* error);

}  // namespace zsoda::inference
