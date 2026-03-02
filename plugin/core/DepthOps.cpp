#include "core/DepthOps.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace zsoda::core {
namespace {

float Clamp01(float v) {
  return std::clamp(v, 0.0F, 1.0F);
}

float Smoothstep(float edge0, float edge1, float x) {
  if (edge0 == edge1) {
    return x >= edge1 ? 1.0F : 0.0F;
  }
  const float t = Clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3.0F - 2.0F * t);
}

}  // namespace

void NormalizeDepth(FrameBuffer* depth, bool invert) {
  if (depth == nullptr || depth->empty()) {
    return;
  }

  float min_value = std::numeric_limits<float>::max();
  float max_value = std::numeric_limits<float>::lowest();
  const auto& desc = depth->desc();

  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float value = depth->at(x, y, 0);
      min_value = std::min(min_value, value);
      max_value = std::max(max_value, value);
    }
  }

  const float range = std::max(max_value - min_value, 1e-6F);
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      float normalized = (depth->at(x, y, 0) - min_value) / range;
      normalized = Clamp01(normalized);
      depth->at(x, y, 0) = invert ? (1.0F - normalized) : normalized;
    }
  }
}

FrameBuffer BuildSliceMatte(const FrameBuffer& normalized_depth,
                            float min_depth,
                            float max_depth,
                            float softness) {
  FrameDesc matte_desc = normalized_depth.desc();
  matte_desc.channels = 1;
  matte_desc.format = PixelFormat::kGray32F;

  FrameBuffer matte(matte_desc);
  if (normalized_depth.empty()) {
    return matte;
  }

  min_depth = Clamp01(min_depth);
  max_depth = Clamp01(max_depth);
  if (max_depth < min_depth) {
    std::swap(max_depth, min_depth);
  }

  softness = Clamp01(softness);
  const float feather = softness * 0.5F;
  const float in_start = std::max(0.0F, min_depth - feather);
  const float in_end = min_depth;
  const float out_start = max_depth;
  const float out_end = std::min(1.0F, max_depth + feather);

  const auto& desc = normalized_depth.desc();
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float z = Clamp01(normalized_depth.at(x, y, 0));
      const float rise = Smoothstep(in_start, in_end, z);
      const float fall = 1.0F - Smoothstep(out_start, out_end, z);
      matte.at(x, y, 0) = Clamp01(rise * fall);
    }
  }

  return matte;
}

}  // namespace zsoda::core
