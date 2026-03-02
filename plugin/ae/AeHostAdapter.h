#pragma once

#include <string>

#include "ae/AeCommandRouter.h"

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

AeCommand MapStubCommandId(int command_id);
bool BuildStubDispatch(int command_id, AeDispatchContext* dispatch, std::string* error);

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK

struct AeSdkEntryPayload {
  PF_Cmd command = static_cast<PF_Cmd>(0);
  PF_InData* in_data = nullptr;
  PF_OutData* out_data = nullptr;
  PF_ParamDef** params = nullptr;
  PF_LayerDef* output = nullptr;
  void* extra = nullptr;
};

AeCommand MapPfCommand(PF_Cmd command);
bool BuildSdkDispatch(const AeSdkEntryPayload& payload,
                      AeDispatchContext* dispatch,
                      std::string* error);

#endif

}  // namespace zsoda::ae
