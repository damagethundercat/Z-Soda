#include "ae/AeHostAdapter.h"

namespace zsoda::ae {
namespace {

constexpr int kLegacyRenderCommandId = 3;

void SetError(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
}

AeCommand MapLegacyCommandId(int command_id) {
  switch (command_id) {
    case 0:
      return AeCommand::kAbout;
    case 1:
      return AeCommand::kGlobalSetup;
    case 2:
      return AeCommand::kParamsSetup;
    case kLegacyRenderCommandId:
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
  AeSdkRenderPayloadScaffold scaffold{};
  if (!TryExtractPfCmdRenderPayload(payload, &scaffold, nullptr) || !scaffold.has_host_buffers) {
    // TODO(ae): Wire PF_Cmd_RENDER payload extraction to populate scaffold.host_render:
    // - map input PF_EffectWorld to HostBufferView (pixel pointer/size/row_bytes/format),
    // - map output PF_EffectWorld to MutableHostBufferView,
    // - read frame hash/time and parameter overrides from in_data/params.
    // Until that exists, keep host-only render handling as a safe no-op bridge.
    return true;
  }

  if (!BuildHostBufferRenderDispatch(scaffold.host_render, dispatch, error)) {
    return false;
  }

  // BuildHostBufferRenderDispatch sets stub metadata; restore the original SDK host payload.
  dispatch->host.command_id = static_cast<int>(payload.command);
  dispatch->host.in_data = payload.in_data;
  dispatch->host.out_data = payload.out_data;
  dispatch->host.params = payload.params;
  dispatch->host.output = payload.output;
  dispatch->host.extra = payload.extra;
  dispatch->command.host = &dispatch->host;
  return true;
}
#endif

}  // namespace

AeCommand MapStubCommandId(int command_id) {
  return MapLegacyCommandId(command_id);
}

bool BuildStubDispatch(int command_id, AeDispatchContext* dispatch, std::string* error) {
  if (dispatch == nullptr) {
    SetError(error, "missing dispatch output");
    return false;
  }

  dispatch->host = {};
  dispatch->host.command_id = command_id;
  InitializeBaseContext(dispatch, MapStubCommandId(command_id), error);
  return true;
}

bool BuildHostBufferRenderDispatch(const AeHostRenderBridgePayload& payload,
                                   AeDispatchContext* dispatch,
                                   std::string* error) {
  if (dispatch == nullptr) {
    SetError(error, "missing dispatch output");
    return false;
  }

  dispatch->host = {};
  dispatch->host.command_id = kLegacyRenderCommandId;
  InitializeBaseContext(dispatch, AeCommand::kRender, error);

  dispatch->render_request = {};
  dispatch->render_response = {};
  const auto source_status =
      zsoda::core::ConvertHostToGray32F(payload.source, &dispatch->render_request.source);
  if (source_status != zsoda::core::PixelConversionStatus::kOk) {
    SetError(error,
             std::string("host->gray conversion failed: ") +
                 zsoda::core::PixelConversionStatusString(source_status));
    return false;
  }

  dispatch->render_request.params_override = payload.params_override;
  dispatch->render_request.frame_hash = payload.frame_hash;
  dispatch->command.render_request = &dispatch->render_request;
  dispatch->command.render_response = &dispatch->render_response;
  return true;
}

bool ExecuteHostBufferRenderBridge(AeCommandRouter* router,
                                   const AeHostRenderBridgePayload& payload,
                                   AeHostRenderBridgeResult* result,
                                   std::string* error) {
  if (router == nullptr) {
    SetError(error, "missing command router");
    return false;
  }

  AeDispatchContext dispatch;
  if (!BuildHostBufferRenderDispatch(payload, &dispatch, error)) {
    return false;
  }
  if (!router->Handle(dispatch.command)) {
    return false;
  }

  const auto output_status =
      zsoda::core::ConvertGray32FToHost(dispatch.render_response.output, payload.destination);
  if (output_status != zsoda::core::PixelConversionStatus::kOk) {
    SetError(error,
             std::string("gray->host conversion failed: ") +
                 zsoda::core::PixelConversionStatusString(output_status));
    return false;
  }

  if (result != nullptr) {
    result->status = dispatch.render_response.status;
    result->message = dispatch.render_response.message;
  }
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

bool TryExtractPfCmdRenderPayload(const AeSdkEntryPayload& payload,
                                  AeSdkRenderPayloadScaffold* scaffold,
                                  std::string* error) {
  (void)payload;
  if (scaffold == nullptr) {
    SetError(error, "missing sdk render payload scaffold");
    return false;
  }
  *scaffold = {};

  // TODO(ae): Decode PF_Cmd_RENDER payload into scaffold->host_render, including:
  // - PF_EffectWorld input/output pixel buffers and row bytes,
  // - host pixel format selection for 8/16/32 bpc,
  // - frame hash and optional parameter override extraction.
  // Returning false keeps BuildSdkDispatch on the safe host-only no-op render path.
  return false;
}

#endif

}  // namespace zsoda::ae
