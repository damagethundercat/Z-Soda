#pragma once

#include "core/Frame.h"

namespace zsoda::core {

void NormalizeDepth(FrameBuffer* depth, bool invert);
FrameBuffer BuildSliceMatte(const FrameBuffer& normalized_depth,
                            float min_depth,
                            float max_depth,
                            float softness);

}  // namespace zsoda::core
