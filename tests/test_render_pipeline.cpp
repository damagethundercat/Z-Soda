#include <cassert>
#include <memory>

#include "core/RenderPipeline.h"
#include "inference/ManagedInferenceEngine.h"

namespace {

zsoda::core::FrameBuffer MakeSourceFrame() {
  zsoda::core::FrameDesc desc;
  desc.width = 4;
  desc.height = 4;
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

void TestCacheIsSeparatedByModel() {
  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame();

  zsoda::core::RenderParams small;
  small.model_id = "depth-anything-v3-small";
  small.frame_hash = 777;

  auto first = pipeline.Render(src, small);
  assert(first.status == zsoda::core::RenderStatus::kInference ||
         first.status == zsoda::core::RenderStatus::kFallback);

  auto second = pipeline.Render(src, small);
  assert(second.status == zsoda::core::RenderStatus::kCacheHit);

  zsoda::core::RenderParams large = small;
  large.model_id = "depth-anything-v3-large";
  auto third = pipeline.Render(src, large);
  assert(third.status != zsoda::core::RenderStatus::kCacheHit);
}

}  // namespace

void RunRenderPipelineTests() {
  TestCacheIsSeparatedByModel();
}
