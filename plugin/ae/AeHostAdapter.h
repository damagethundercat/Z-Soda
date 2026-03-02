#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "ae/AeCommandRouter.h"
#include "core/PixelConversion.h"

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
#include "AE_Effect.h"
#endif

namespace zsoda::ae {

struct AeDispatchContext {
  AeHostCommandContext host{};
  AeCommandContext command{};

  // Optional payload storage for future SDK wiring.
  AeParamValues params_update{};
  RenderRequest render_request{};
  RenderResponse render_response{};
};

struct AeHostRenderBridgePayload {
  zsoda::core::HostBufferView source{};
  zsoda::core::MutableHostBufferView destination{};
  std::optional<AeParamValues> params_override{};
  std::uint64_t frame_hash = 0;
};

struct AeHostRenderBridgeResult {
  zsoda::core::RenderStatus status = zsoda::core::RenderStatus::kSafeOutput;
  std::string message;
};

AeCommand MapStubCommandId(int command_id);
bool BuildStubDispatch(int command_id, AeDispatchContext* dispatch, std::string* error);
bool BuildHostBufferRenderDispatch(const AeHostRenderBridgePayload& payload,
                                   AeDispatchContext* dispatch,
                                   std::string* error);
bool ExecuteHostBufferRenderBridge(AeCommandRouter* router,
                                   const AeHostRenderBridgePayload& payload,
                                   AeHostRenderBridgeResult* result,
                                   std::string* error);

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK

struct AeSdkEntryPayload {
  PF_Cmd command = static_cast<PF_Cmd>(0);
  PF_InData* in_data = nullptr;
  PF_OutData* out_data = nullptr;
  PF_ParamDef** params = nullptr;
  PF_LayerDef* output = nullptr;
  void* extra = nullptr;
};

struct AeSdkRenderPayloadScaffold {
  // Populated once PF_Cmd_RENDER world extraction is wired.
  AeHostRenderBridgePayload host_render{};
  bool has_host_buffers = false;
};

AeCommand MapPfCommand(PF_Cmd command);
bool BuildSdkDispatch(const AeSdkEntryPayload& payload,
                      AeDispatchContext* dispatch,
                      std::string* error);
bool TryExtractPfCmdRenderPayload(const AeSdkEntryPayload& payload,
                                  AeSdkRenderPayloadScaffold* scaffold,
                                  std::string* error);

#endif

}  // namespace zsoda::ae
