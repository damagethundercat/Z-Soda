#include <cassert>

#include "core/Cache.h"

namespace {

void TestCacheInsertFind() {
  zsoda::core::DepthCache cache(2);
  zsoda::core::RenderCacheKey key;
  key.width = 1920;
  key.height = 1080;
  key.quality = 2;
  key.frame_hash = 42;

  zsoda::core::FrameDesc desc;
  desc.width = 2;
  desc.height = 2;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;
  zsoda::core::FrameBuffer frame(desc);
  frame.at(0, 0, 0) = 0.5F;

  cache.Insert(key, frame);

  zsoda::core::FrameBuffer out;
  assert(cache.Find(key, &out));
  assert(out.at(0, 0, 0) == 0.5F);
}

void TestCacheEviction() {
  zsoda::core::DepthCache cache(1);

  zsoda::core::RenderCacheKey a;
  a.width = 1;
  a.frame_hash = 10;
  zsoda::core::RenderCacheKey b;
  b.width = 2;
  b.frame_hash = 20;

  zsoda::core::FrameDesc desc;
  desc.width = 1;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;
  zsoda::core::FrameBuffer frame(desc);
  frame.at(0, 0, 0) = 1.0F;

  cache.Insert(a, frame);
  cache.Insert(b, frame);

  zsoda::core::FrameBuffer out;
  assert(!cache.Find(a, &out));
  assert(cache.Find(b, &out));
}

}  // namespace

void RunCacheTests() {
  TestCacheInsertFind();
  TestCacheEviction();
}
