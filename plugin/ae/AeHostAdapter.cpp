#include "ae/AeHostAdapter.h"

namespace zsoda::ae {
namespace {

AeCommand MapLegacyCommandId(int command_id) {
  switch (command_id) {
    case 0:
      return AeCommand::kAbout;
    case 1:
      return AeCommand::kGlobalSetup;
    case 2:
      return AeCommand::kParamsSetup;
    case 3:
      return AeCommand::kRender;
    default:
      return AeCommand::kUnknown;
  }
}

void InitializeBaseContext(AeDispatchContext* dispatch, AeCommand command, std::string* error) {
  dispatch->command = {};
  dispatch->command.command = command;
  dispatch->command.host = &dispatch->host;
  dispatch->command.error = error;
}

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
bool WireParamSetupPayload(const AeSdkEntryPayload& payload,
                           AeDispatchContext* dispatch,
                           std::string* error) {
  (void)payload;
  (void)dispatch;
  (void)error;

  // Extension point: decode PF_Cmd_PARAMS_SETUP host payload and populate
  // dispatch->command.params_update when parameter defaults need synchronization.
  return true;
}

bool WireRenderPayload(const AeSdkEntryPayload& payload,
                       AeDispatchContext* dispatch,
                       std::string* error) {
  (void)payload;
  (void)dispatch;
  (void)error;

  // Extension point: decode PF_Cmd_RENDER inputs into dispatch->render_request
  // and point dispatch->command.render_request/render_response at local storage.
  // Router currently accepts host-only render commands as a safe no-op bridge.
  return true;
}
#endif

}  // namespace

AeCommand MapStubCommandId(int command_id) {
  return MapLegacyCommandId(command_id);
}

bool BuildStubDispatch(int command_id, AeDispatchContext* dispatch, std::string* error) {
  if (dispatch == nullptr) {
    if (error) {
      *error = "missing dispatch output";
    }
    return false;
  }

  dispatch->host = {};
  dispatch->host.command_id = command_id;
  InitializeBaseContext(dispatch, MapStubCommandId(command_id), error);
  return true;
}

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK

AeCommand MapPfCommand(PF_Cmd command) {
  switch (command) {
    case PF_Cmd_ABOUT:
      return AeCommand::kAbout;
    case PF_Cmd_GLOBAL_SETUP:
      return AeCommand::kGlobalSetup;
    case PF_Cmd_PARAMS_SETUP:
      return AeCommand::kParamsSetup;
    case PF_Cmd_RENDER:
      return AeCommand::kRender;
    default:
      return AeCommand::kUnknown;
  }
}

bool BuildSdkDispatch(const AeSdkEntryPayload& payload,
                      AeDispatchContext* dispatch,
                      std::string* error) {
  if (dispatch == nullptr) {
    if (error) {
      *error = "missing dispatch output";
    }
    return false;
  }

  dispatch->host = {};
  dispatch->host.command_id = static_cast<int>(payload.command);
  dispatch->host.in_data = payload.in_data;
  dispatch->host.out_data = payload.out_data;
  dispatch->host.params = payload.params;
  dispatch->host.output = payload.output;
  dispatch->host.extra = payload.extra;

  const AeCommand mapped = MapPfCommand(payload.command);
  InitializeBaseContext(dispatch, mapped, error);

  switch (mapped) {
    case AeCommand::kParamsSetup:
      return WireParamSetupPayload(payload, dispatch, error);
    case AeCommand::kRender:
      return WireRenderPayload(payload, dispatch, error);
    default:
      return true;
  }
}

#endif

}  // namespace zsoda::ae
