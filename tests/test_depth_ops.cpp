#include <cassert>

#include "core/DepthOps.h"

namespace {

void TestNormalizeDepth() {
  zsoda::core::FrameDesc desc;
  desc.width = 3;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;

  zsoda::core::FrameBuffer depth(desc);
  depth.at(0, 0, 0) = 10.0F;
  depth.at(1, 0, 0) = 20.0F;
  depth.at(2, 0, 0) = 30.0F;

  zsoda::core::NormalizeDepth(&depth, false);
  assert(depth.at(0, 0, 0) == 0.0F);
  assert(depth.at(2, 0, 0) == 1.0F);
}

void TestSliceMatte() {
  zsoda::core::FrameDesc desc;
  desc.width = 3;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;
  zsoda::core::FrameBuffer depth(desc);
  depth.at(0, 0, 0) = 0.1F;
  depth.at(1, 0, 0) = 0.5F;
  depth.at(2, 0, 0) = 0.9F;

  auto matte = zsoda::core::BuildSliceMatte(depth, 0.3F, 0.7F, 0.0F);
  assert(matte.at(0, 0, 0) == 0.0F);
  assert(matte.at(1, 0, 0) == 1.0F);
  assert(matte.at(2, 0, 0) == 0.0F);
}

}  // namespace

void RunDepthOpsTests() {
  TestNormalizeDepth();
  TestSliceMatte();
}
