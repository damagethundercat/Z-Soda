#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ae/AeParams.h"
#include "core/CompatMutex.h"
#include "core/RenderPipeline.h"
#include "inference/InferenceEngine.h"

namespace zsoda::ae {

enum class AeCommand {
  kAbout,
  kGlobalSetup,
  kParamsSetup,
  kUpdateParams,
  kRender,
  kUnknown,
};

struct AeHostCommandContext {
  int command_id = -1;
  void* in_data = nullptr;
  void* out_data = nullptr;
  void* params = nullptr;
  void* output = nullptr;
  void* extra = nullptr;
};

struct RenderRequest {
  zsoda::core::FrameBuffer source;
  std::optional<AeParamValues> params_override;
  std::shared_ptr<zsoda::core::RenderPipelineState> pipeline_state;
  std::uint64_t frame_hash = 0;
  std::uint64_t render_state_token = 0;
};

struct RenderResponse {
  zsoda::core::FrameBuffer output;
  zsoda::core::RenderStatus status = zsoda::core::RenderStatus::kSafeOutput;
  std::string message;
};

struct AeCommandContext {
  AeCommand command = AeCommand::kUnknown;
  const AeHostCommandContext* host = nullptr;
  const AeParamValues* params_update = nullptr;
  const RenderRequest* render_request = nullptr;
  RenderResponse* render_response = nullptr;
  std::string* error = nullptr;
};

class AeCommandRouter {
 public:
  AeCommandRouter(std::shared_ptr<zsoda::core::RenderPipeline> pipeline,
                  std::shared_ptr<zsoda::inference::IInferenceEngine> engine);

  bool Handle(const AeCommandContext& context);
  bool UpdateParams(const AeParamValues& params, std::string* error);
  [[nodiscard]] AeParamValues CurrentParams() const;
  [[nodiscard]] const std::vector<std::string>& ModelMenu() const;

 private:
  [[nodiscard]] AeParamValues SnapshotParams() const;
  bool RefreshModelMenu(std::string* error);

  std::shared_ptr<zsoda::core::RenderPipeline> pipeline_;
  std::shared_ptr<zsoda::inference::IInferenceEngine> engine_;
  mutable zsoda::core::CompatMutex params_mutex_{};
  AeParamValues current_params_{};
  std::vector<std::string> model_menu_;
};

}  // namespace zsoda::ae
