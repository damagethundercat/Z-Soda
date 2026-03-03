#include <cassert>

#include "core/Tiler.h"

void RunCacheTests();
void RunAeParamTests();
void RunAeRouterTests();
void RunDepthOpsTests();
void RunInferenceEngineTests();
void RunRenderPipelineTests();
void RunRuntimePathResolverTests();

namespace {

void TestBuildTiles() {
  const auto tiles = zsoda::core::BuildTiles(1920, 1080, 512, 32);
  assert(!tiles.empty());
}

void TestComposeTiles() {
  zsoda::core::FrameDesc desc;
  desc.width = 4;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;

  zsoda::core::TileResult left;
  left.tile = {0, 0, 2, 1};
  left.depth = zsoda::core::FrameBuffer({2, 1, 1, zsoda::core::PixelFormat::kGray32F});
  left.depth.at(0, 0, 0) = 0.2F;
  left.depth.at(1, 0, 0) = 0.4F;

  zsoda::core::TileResult right;
  right.tile = {2, 0, 2, 1};
  right.depth = zsoda::core::FrameBuffer({2, 1, 1, zsoda::core::PixelFormat::kGray32F});
  right.depth.at(0, 0, 0) = 0.6F;
  right.depth.at(1, 0, 0) = 0.8F;

  const auto composed = zsoda::core::ComposeTiles(desc, {left, right});
  assert(composed.at(0, 0, 0) == 0.2F);
  assert(composed.at(3, 0, 0) == 0.8F);
}

}  // namespace

int main() {
  RunCacheTests();
  RunAeParamTests();
  RunAeRouterTests();
  RunDepthOpsTests();
  RunInferenceEngineTests();
  RunRenderPipelineTests();
  RunRuntimePathResolverTests();
  TestBuildTiles();
  TestComposeTiles();
  return 0;
}
