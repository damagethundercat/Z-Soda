#include "ae/AeHostAdapter.h"

#include <cstdint>
#include <memory>
#include <string>

#include "ae/AeParams.h"
#include "core/RenderPipeline.h"
#include "inference/InferenceEngine.h"

namespace zsoda::ae {
namespace {

std::shared_ptr<zsoda::inference::IInferenceEngine> GetEngine() {
  static std::shared_ptr<zsoda::inference::IInferenceEngine> engine =
      zsoda::inference::CreateDefaultEngine();
  return engine;
}

std::shared_ptr<zsoda::core::RenderPipeline> GetPipeline() {
  static auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(GetEngine());
  return pipeline;
}

zsoda::ae::AeCommandRouter& GetRouter() {
  static zsoda::ae::AeCommandRouter router(GetPipeline(), GetEngine());
  return router;
}

int Dispatch(const AeDispatchContext& dispatch) {
  return GetRouter().Handle(dispatch.command) ? 0 : -1;
}

}  // namespace
}  // namespace zsoda::ae

extern "C" int ZSodaEffectMainStub(int command_id) {
  std::string error;
  zsoda::ae::AeDispatchContext dispatch;
  if (!zsoda::ae::BuildStubDispatch(command_id, &dispatch, &error)) {
    return -1;
  }
  return zsoda::ae::Dispatch(dispatch);
}

extern "C" int ZSodaSetModelIdStub(const char* model_id) {
  if (model_id == nullptr) {
    return -1;
  }
  auto params = zsoda::ae::GetRouter().CurrentParams();
  params.model_id = model_id;
  std::string error;
  zsoda::ae::AeCommandContext context;
  context.command = zsoda::ae::AeCommand::kUpdateParams;
  context.params_update = &params;
  context.error = &error;
  return zsoda::ae::GetRouter().Handle(context) ? 0 : -1;
}

extern "C" int ZSodaRenderGrayFrameStub(const float* src,
                                        int width,
                                        int height,
                                        std::uint64_t frame_hash,
                                        float* out,
                                        int out_size) {
  if (src == nullptr || out == nullptr || width <= 0 || height <= 0) {
    return -1;
  }
  if (out_size < width * height) {
    return -1;
  }

  zsoda::core::FrameDesc desc;
  desc.width = width;
  desc.height = height;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;

  zsoda::core::FrameBuffer source(desc);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int idx = y * width + x;
      source.at(x, y, 0) = src[idx];
    }
  }

  zsoda::ae::RenderRequest request;
  request.source = source;
  request.frame_hash = frame_hash;

  zsoda::ae::RenderResponse response;
  std::string error;
  zsoda::ae::AeHostCommandContext host_context;
  host_context.command_id = 3;
  zsoda::ae::AeCommandContext context;
  context.command = zsoda::ae::AeCommand::kRender;
  context.host = &host_context;
  context.render_request = &request;
  context.render_response = &response;
  context.error = &error;
  if (!zsoda::ae::GetRouter().Handle(context)) {
    return -1;
  }

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int idx = y * width + x;
      out[idx] = response.output.at(x, y, 0);
    }
  }
  return 0;
}

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK

#ifndef DllExport
#define DllExport
#endif

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd,
                                       PF_InData* in_data,
                                       PF_OutData* out_data,
                                       PF_ParamDef* params[],
                                       PF_LayerDef* output,
                                       void* extra) {
  std::string error;

  zsoda::ae::AeDispatchContext dispatch;
  zsoda::ae::AeSdkEntryPayload payload;
  payload.command = cmd;
  payload.in_data = in_data;
  payload.out_data = out_data;
  payload.params = params;
  payload.output = output;
  payload.extra = extra;

  if (!zsoda::ae::BuildSdkDispatch(payload, &dispatch, &error)) {
    return PF_Err_INTERNAL_STRUCT_DAMAGED;
  }

  if (dispatch.command.command == zsoda::ae::AeCommand::kUnknown) {
    // Skeleton entrypoint ignores unsupported commands until individual handlers
    // are wired.
    return PF_Err_NONE;
  }

  return zsoda::ae::Dispatch(dispatch) == 0 ? PF_Err_NONE : PF_Err_INTERNAL_STRUCT_DAMAGED;
}

#endif
