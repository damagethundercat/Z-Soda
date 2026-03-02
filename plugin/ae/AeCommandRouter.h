#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ae/AeParams.h"
#include "core/RenderPipeline.h"
#include "inference/InferenceEngine.h"

namespace zsoda::ae {

enum class AeCommand {
  kAbout,
  kGlobalSetup,
  kParamsSetup,
  kRender,
  kUnknown,
};

struct RenderRequest {
  zsoda::core::FrameBuffer source;
  std::optional<AeParamValues> params_override;
  std::uint64_t frame_hash = 0;
};

struct RenderResponse {
  zsoda::core::FrameBuffer output;
  zsoda::core::RenderStatus status = zsoda::core::RenderStatus::kSafeOutput;
  std::string message;
};

class AeCommandRouter {
 public:
  AeCommandRouter(std::shared_ptr<zsoda::core::RenderPipeline> pipeline,
                  std::shared_ptr<zsoda::inference::IInferenceEngine> engine);

  bool Handle(AeCommand command,
              const RenderRequest* render_request,
              RenderResponse* render_response,
              std::string* error);
  bool UpdateParams(const AeParamValues& params, std::string* error);
  [[nodiscard]] AeParamValues CurrentParams() const;
  [[nodiscard]] const std::vector<std::string>& ModelMenu() const;

 private:
  bool RefreshModelMenu(std::string* error);

  std::shared_ptr<zsoda::core::RenderPipeline> pipeline_;
  std::shared_ptr<zsoda::inference::IInferenceEngine> engine_;
  AeParamValues current_params_{};
  std::vector<std::string> model_menu_;
};

}  // namespace zsoda::ae
