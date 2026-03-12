#include "ae/AeCommandRouter.h"

#include <algorithm>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace zsoda::ae {
namespace {

void AppendRouterTrace(const char* stage, const char* detail = nullptr) {
#if defined(_WIN32)
  char temp_path[MAX_PATH] = {};
  const DWORD written = ::GetTempPathA(MAX_PATH, temp_path);
  if (written == 0 || written >= MAX_PATH) {
    return;
  }

  char log_path[MAX_PATH] = {};
  std::snprintf(log_path, sizeof(log_path), "%s%s", temp_path, "ZSoda_AE_Runtime.log");
  FILE* file = std::fopen(log_path, "ab");
  if (file == nullptr) {
    return;
  }

  SYSTEMTIME now = {};
  ::GetLocalTime(&now);
  const unsigned long tid = static_cast<unsigned long>(::GetCurrentThreadId());
  std::fprintf(file,
               "%04u-%02u-%02u %02u:%02u:%02u.%03u | RouterTrace | tid=%lu, stage=%s, detail=%s\r\n",
               static_cast<unsigned>(now.wYear),
               static_cast<unsigned>(now.wMonth),
               static_cast<unsigned>(now.wDay),
               static_cast<unsigned>(now.wHour),
               static_cast<unsigned>(now.wMinute),
               static_cast<unsigned>(now.wSecond),
               static_cast<unsigned>(now.wMilliseconds),
               tid,
               stage != nullptr ? stage : "<null>",
               (detail != nullptr && detail[0] != '\0') ? detail : "<none>");
  std::fclose(file);
#else
  (void)stage;
  (void)detail;
#endif
}

}  // namespace

AeCommandRouter::AeCommandRouter(std::shared_ptr<zsoda::core::RenderPipeline> pipeline,
                                 std::shared_ptr<zsoda::inference::IInferenceEngine> engine)
    : pipeline_(std::move(pipeline)), engine_(std::move(engine)), current_params_(DefaultAeParams()) {}

AeParamValues AeCommandRouter::SnapshotParams() const {
  zsoda::core::CompatLockGuard lock(params_mutex_);
  return current_params_;
}

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
      AppendRouterTrace("render_enter");
      if (pipeline_ == nullptr) {
        AppendRouterTrace("render_invalid_pipeline");
        if (context.error) {
          *context.error = "invalid render command arguments";
        }
        return false;
      }
      if (context.render_request == nullptr || context.render_response == nullptr) {
        // Accept host-only render bridge invocation. PF_Cmd payload wiring will fill
        // render_request/render_response once AE SDK integration is enabled.
        if (context.host != nullptr) {
          AppendRouterTrace("render_host_only_bridge");
          return true;
        }
        AppendRouterTrace("render_missing_request_response");
        if (context.error) {
          *context.error = "invalid render command arguments";
        }
        return false;
      }

      const bool using_params_override = context.render_request->params_override.has_value();
      const AeParamValues params =
          using_params_override ? *context.render_request->params_override : SnapshotParams();
      auto render_params = ToRenderParams(params);
      render_params.frame_hash = context.render_request->frame_hash;
      render_params.render_state_token = context.render_request->render_state_token;

      const std::string before_detail =
          "param_source=" + std::string(using_params_override ? "override" : "current") +
          ", model=" + render_params.model_id + ", quality=" +
          std::to_string(render_params.quality) + ", render_state_token=" +
          std::to_string(render_params.render_state_token) +
          ", preserve_ratio=" + std::to_string(render_params.preserve_aspect_ratio ? 1 : 0) +
          ", output=" + std::to_string(static_cast<int>(params.output)) +
          ", slice_mode=" + std::to_string(static_cast<int>(params.slice_mode)) +
          ", slice_position=" + std::to_string(params.slice_position) +
          ", slice_range=" + std::to_string(params.slice_range) +
          ", slice_softness=" + std::to_string(params.slice_softness);
      AppendRouterTrace("render_before_pipeline", before_detail.c_str());
      const auto output = pipeline_->Render(context.render_request->source,
                                           render_params,
                                           context.render_request->pipeline_state.get());
      const std::string after_detail =
          "status=" + std::to_string(static_cast<int>(output.status)) +
          ", message=" + (output.message.empty() ? std::string("<none>") : output.message);
      AppendRouterTrace("render_after_pipeline", after_detail.c_str());
      context.render_response->output = output.frame;
      context.render_response->status = output.status;
      context.render_response->message = output.message;
      AppendRouterTrace("render_response_written");
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
  const std::string update_detail =
      "model=" + params.model_id + ", quality=" + std::to_string(params.quality) +
      ", preserve_ratio=" + std::to_string(params.preserve_ratio ? 1 : 0) +
      ", output=" + std::to_string(static_cast<int>(params.output)) +
      ", slice_mode=" + std::to_string(static_cast<int>(params.slice_mode)) +
      ", slice_position=" + std::to_string(params.slice_position) +
      ", slice_range=" + std::to_string(params.slice_range) +
      ", slice_softness=" + std::to_string(params.slice_softness);
  AppendRouterTrace("params_update_enter", update_detail.c_str());
  if (engine_) {
    const auto menu = BuildModelMenu(*engine_);
    if (std::find(menu.begin(), menu.end(), params.model_id) == menu.end()) {
      if (error) {
        *error = "unsupported model id: " + params.model_id;
      }
      AppendRouterTrace("params_update_rejected", params.model_id.c_str());
      return false;
    }
  }
  {
    zsoda::core::CompatLockGuard lock(params_mutex_);
    current_params_ = params;
  }
  AppendRouterTrace("params_update_applied", update_detail.c_str());
  return true;
}

AeParamValues AeCommandRouter::CurrentParams() const {
  return SnapshotParams();
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
  AeParamValues params = SnapshotParams();
  if (std::find(model_menu_.begin(), model_menu_.end(), params.model_id) ==
      model_menu_.end()) {
    params.model_id = model_menu_.front();
    zsoda::core::CompatLockGuard lock(params_mutex_);
    current_params_ = params;
  }
  return true;
}

}  // namespace zsoda::ae
