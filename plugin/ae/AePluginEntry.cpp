#include "ae/AeCommandRouter.h"

#include <cstdint>
#include <memory>
#include <string>

#include "ae/AeParams.h"
#include "core/RenderPipeline.h"
#include "inference/InferenceEngine.h"

namespace zsoda::ae {
namespace {

AeCommand MapCommand(int command_id) {
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

std::shared_ptr<zsoda::inference::IInferenceEngine> GetEngine() {
  static std::shared_ptr<zsoda::inference::IInferenceEngine> engine = zsoda::inference::CreateDefaultEngine();
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

}  // namespace
}  // namespace zsoda::ae

extern "C" int ZSodaEffectMainStub(int command_id) {
  std::string error;
  const auto command = zsoda::ae::MapCommand(command_id);
  if (command == zsoda::ae::AeCommand::kRender) {
    // AE SDK integration path will supply render payload through PF_Cmd_RENDER.
    return 0;
  }
  return zsoda::ae::GetRouter().Handle(command, nullptr, nullptr, &error) ? 0 : -1;
}

extern "C" int ZSodaSetModelIdStub(const char* model_id) {
  if (model_id == nullptr) {
    return -1;
  }
  auto params = zsoda::ae::GetRouter().CurrentParams();
  params.model_id = model_id;
  std::string error;
  return zsoda::ae::GetRouter().UpdateParams(params, &error) ? 0 : -1;
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
  if (!zsoda::ae::GetRouter().Handle(zsoda::ae::AeCommand::kRender, &request, &response, &error)) {
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
