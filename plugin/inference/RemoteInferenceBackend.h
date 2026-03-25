#pragma once

#include <memory>
#include <string>

#include "core/CompatMutex.h"
#include "inference/OnnxRuntimeBackend.h"
#include "inference/RuntimeOptions.h"

namespace zsoda::inference {

struct RemoteBackendCommandConfig {
  std::string command_template;
};

class RemoteInferenceBackend final : public IOnnxRuntimeBackend {
 public:
  RemoteInferenceBackend(RuntimeOptions options, RemoteBackendCommandConfig command_config);

  const char* Name() const override;
  bool Initialize(std::string* error) override;
  bool SelectModel(const std::string& model_id,
                   const std::string& model_path,
                   std::string* error) override;
  bool Run(const InferenceRequest& request,
           zsoda::core::FrameBuffer* out_depth,
           std::string* error) const override;
  RuntimeBackend ActiveBackend() const override;

 private:
  bool ResolveEndpointConfigurationLocked(std::string* error);
  bool EnsureAutoStartedServiceReadyLocked(std::string* error);

  RuntimeOptions options_;
  RemoteBackendCommandConfig command_config_;
  RuntimeBackend active_backend_ = RuntimeBackend::kCpu;
  bool initialized_ = false;
  bool service_autostart_enabled_ = false;
  std::string backend_name_;
  std::string active_model_id_;
  std::string active_model_path_;
  std::string resolved_service_host_;
  int resolved_service_port_ = 0;
  std::string resolved_remote_endpoint_;
  std::string resolved_status_endpoint_;
  mutable zsoda::core::CompatMutex mutex_;
};

std::unique_ptr<IOnnxRuntimeBackend> CreateRemoteInferenceBackendWithCommand(
    const RuntimeOptions& options,
    RemoteBackendCommandConfig command_config,
    std::string* error);

std::unique_ptr<IOnnxRuntimeBackend> CreateRemoteInferenceBackend(const RuntimeOptions& options,
                                                                  std::string* error);

}  // namespace zsoda::inference
