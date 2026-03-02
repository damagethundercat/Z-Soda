#include "ae/AeCommandRouter.h"

#include <algorithm>

namespace zsoda::ae {

AeCommandRouter::AeCommandRouter(std::shared_ptr<zsoda::core::RenderPipeline> pipeline,
                                 std::shared_ptr<zsoda::inference::IInferenceEngine> engine)
    : pipeline_(std::move(pipeline)), engine_(std::move(engine)), current_params_(DefaultAeParams()) {}

bool AeCommandRouter::Handle(const AeCommandContext& context) {
  switch (context.command) {
    case AeCommand::kAbout:
      return true;

    case AeCommand::kGlobalSetup:
    case AeCommand::kParamsSetup:
      return RefreshModelMenu(context.error);

    case AeCommand::kUpdateParams:
      if (context.params_update == nullptr) {
        if (context.error) {
          *context.error = "missing params update payload";
        }
        return false;
      }
      return UpdateParams(*context.params_update, context.error);

    case AeCommand::kRender: {
      if (pipeline_ == nullptr) {
        if (context.error) {
          *context.error = "invalid render command arguments";
        }
        return false;
      }
      if (context.render_request == nullptr || context.render_response == nullptr) {
        // Accept host-only render bridge invocation. PF_Cmd payload wiring will fill
        // render_request/render_response once AE SDK integration is enabled.
        if (context.host != nullptr) {
          return true;
        }
        if (context.error) {
          *context.error = "invalid render command arguments";
        }
        return false;
      }

      const AeParamValues params = context.render_request->params_override.has_value()
                                       ? *context.render_request->params_override
                                       : current_params_;
      auto render_params = ToRenderParams(params);
      render_params.frame_hash = context.render_request->frame_hash;

      const auto output = pipeline_->Render(context.render_request->source, render_params);
      context.render_response->output = output.frame;
      context.render_response->status = output.status;
      context.render_response->message = output.message;
      return true;
    }

    case AeCommand::kUnknown:
    default:
      if (context.error) {
        *context.error = "unknown command";
      }
      return false;
  }
}

bool AeCommandRouter::UpdateParams(const AeParamValues& params, std::string* error) {
  if (engine_) {
    const auto menu = BuildModelMenu(*engine_);
    if (std::find(menu.begin(), menu.end(), params.model_id) == menu.end()) {
      if (error) {
        *error = "unsupported model id: " + params.model_id;
      }
      return false;
    }
  }
  current_params_ = params;
  return true;
}

AeParamValues AeCommandRouter::CurrentParams() const {
  return current_params_;
}

const std::vector<std::string>& AeCommandRouter::ModelMenu() const {
  return model_menu_;
}

bool AeCommandRouter::RefreshModelMenu(std::string* error) {
  if (engine_ == nullptr) {
    return true;
  }
  model_menu_ = BuildModelMenu(*engine_);
  if (model_menu_.empty()) {
    if (error) {
      *error = "model menu is empty";
    }
    return false;
  }
  if (std::find(model_menu_.begin(), model_menu_.end(), current_params_.model_id) ==
      model_menu_.end()) {
    current_params_.model_id = model_menu_.front();
  }
  return true;
}

}  // namespace zsoda::ae
