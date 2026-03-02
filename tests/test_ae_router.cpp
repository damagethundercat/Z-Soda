#include <cassert>
#include <memory>

#include "ae/AeCommandRouter.h"
#include "inference/ManagedInferenceEngine.h"

namespace {

zsoda::core::FrameBuffer MakeFrame() {
  zsoda::core::FrameDesc desc;
  desc.width = 8;
  desc.height = 8;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;
  zsoda::core::FrameBuffer frame(desc);
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      frame.at(x, y, 0) = static_cast<float>(x + y);
    }
  }
  return frame;
}

void TestParamSetupAndModelMenu() {
  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  assert(router.Handle(zsoda::ae::AeCommand::kGlobalSetup, nullptr, nullptr, &error));
  assert(!router.ModelMenu().empty());

  auto params = router.CurrentParams();
  params.model_id = "depth-anything-v3-large";
  assert(router.UpdateParams(params, &error));
}

void TestRenderUsesCurrentParams() {
  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);
  assert(router.Handle(zsoda::ae::AeCommand::kGlobalSetup, nullptr, nullptr, &error));

  auto params = router.CurrentParams();
  params.model_id = "depth-anything-v3-large";
  assert(router.UpdateParams(params, &error));

  zsoda::ae::RenderRequest request;
  request.source = MakeFrame();
  request.frame_hash = 9090;

  zsoda::ae::RenderResponse response;
  assert(router.Handle(zsoda::ae::AeCommand::kRender, &request, &response, &error));
  assert(!response.message.empty());
}

}  // namespace

void RunAeRouterTests() {
  TestParamSetupAndModelMenu();
  TestRenderUsesCurrentParams();
}
