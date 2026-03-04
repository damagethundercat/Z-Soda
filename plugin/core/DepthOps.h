#pragma once

#include <string_view>

#include "core/Frame.h"

namespace zsoda::core {

enum class DepthMappingMode {
  kRaw,
  kNormalize,
  kGuided,
};

struct GuidedDepthMappingState {
  bool initialized = false;
  float near_value = 0.0F;
  float far_value = 1.0F;
};

struct DepthMappingParams {
  DepthMappingMode mode = DepthMappingMode::kRaw;
  bool invert = false;
  float guided_low_percentile = 0.05F;
  float guided_high_percentile = 0.95F;
  float guided_update_alpha = 0.15F;
};

[[nodiscard]] const char* DepthMappingModeName(DepthMappingMode mode);
[[nodiscard]] DepthMappingMode ParseDepthMappingMode(std::string_view value);

void ClampDepth(FrameBuffer* depth, bool invert);
void NormalizeDepth(FrameBuffer* depth, bool invert);
void ApplyDepthMapping(FrameBuffer* depth,
                       const DepthMappingParams& params,
                       GuidedDepthMappingState* guided_state);
FrameBuffer BuildSliceMatte(const FrameBuffer& normalized_depth,
                            float min_depth,
                            float max_depth,
                            float softness);

}  // namespace zsoda::core
