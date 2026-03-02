#include "ae/AeHostAdapter.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ae/AeParams.h"
#include "core/RenderPipeline.h"
#include "inference/InferenceEngine.h"

namespace zsoda::ae {
namespace {

constexpr int kGrayStubChannels = 4;

std::optional<zsoda::core::PixelFormat> ParseHostPixelFormat(int pixel_format) {
  switch (pixel_format) {
    case static_cast<int>(zsoda::core::PixelFormat::kRGBA8):
      return zsoda::core::PixelFormat::kRGBA8;
    case static_cast<int>(zsoda::core::PixelFormat::kRGBA16):
      return zsoda::core::PixelFormat::kRGBA16;
    case static_cast<int>(zsoda::core::PixelFormat::kRGBA32F):
      return zsoda::core::PixelFormat::kRGBA32F;
    default:
      return std::nullopt;
  }
}

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

extern "C" int ZSodaRenderHostBufferStub(const void* src,
                                         int width,
                                         int height,
                                         int src_row_bytes,
                                         int pixel_format,
                                         std::uint64_t frame_hash,
                                         void* out,
                                         int out_row_bytes);

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
  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (out_size < 0 || static_cast<std::size_t>(out_size) < pixel_count) {
    return -1;
  }

  std::vector<float> rgba_source(pixel_count * zsoda::ae::kGrayStubChannels, 0.0F);
  std::vector<float> rgba_output(pixel_count * zsoda::ae::kGrayStubChannels, 0.0F);
  for (std::size_t i = 0; i < pixel_count; ++i) {
    const float gray = src[i];
    const std::size_t offset = i * zsoda::ae::kGrayStubChannels;
    rgba_source[offset + 0] = gray;
    rgba_source[offset + 1] = gray;
    rgba_source[offset + 2] = gray;
    rgba_source[offset + 3] = 1.0F;
  }

  const std::size_t row_bytes_size =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(zsoda::ae::kGrayStubChannels) *
      sizeof(float);
  if (row_bytes_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return -1;
  }
  const int row_bytes = static_cast<int>(row_bytes_size);
  if (ZSodaRenderHostBufferStub(rgba_source.data(),
                                width,
                                height,
                                row_bytes,
                                static_cast<int>(zsoda::core::PixelFormat::kRGBA32F),
                                frame_hash,
                                rgba_output.data(),
                                row_bytes) != 0) {
    return -1;
  }

  for (std::size_t i = 0; i < pixel_count; ++i) {
    out[i] = rgba_output[i * zsoda::ae::kGrayStubChannels];
  }
  return 0;
}

extern "C" int ZSodaRenderHostBufferStub(const void* src,
                                         int width,
                                         int height,
                                         int src_row_bytes,
                                         int pixel_format,
                                         std::uint64_t frame_hash,
                                         void* out,
                                         int out_row_bytes) {
  if (src == nullptr || out == nullptr || width <= 0 || height <= 0 || src_row_bytes <= 0 ||
      out_row_bytes <= 0) {
    return -1;
  }

  const auto format = zsoda::ae::ParseHostPixelFormat(pixel_format);
  if (!format.has_value()) {
    return -1;
  }

  zsoda::ae::AeHostRenderBridgePayload payload;
  payload.source.pixels = src;
  payload.source.width = width;
  payload.source.height = height;
  payload.source.row_bytes = static_cast<std::size_t>(src_row_bytes);
  payload.source.format = *format;
  payload.destination.pixels = out;
  payload.destination.width = width;
  payload.destination.height = height;
  payload.destination.row_bytes = static_cast<std::size_t>(out_row_bytes);
  payload.destination.format = *format;
  payload.frame_hash = frame_hash;

  std::string error;
  return zsoda::ae::ExecuteHostBufferRenderBridge(&zsoda::ae::GetRouter(), payload, nullptr, &error)
             ? 0
             : -1;
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
