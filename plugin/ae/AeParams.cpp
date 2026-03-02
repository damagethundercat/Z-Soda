#include "ae/AeParams.h"

#include <algorithm>

namespace zsoda::ae {

AeParamValues DefaultAeParams() {
  return {};
}

zsoda::core::RenderParams ToRenderParams(const AeParamValues& input) {
  zsoda::core::RenderParams params;
  params.model_id = input.model_id;
  params.quality = std::clamp(input.quality, 1, 3);
  params.invert = input.invert;
  params.output_mode =
      input.output_mode == AeOutputMode::kSlicing ? zsoda::core::OutputMode::kSlicing
                                                  : zsoda::core::OutputMode::kDepthMap;
  params.min_depth = std::clamp(input.min_depth, 0.0F, 1.0F);
  params.max_depth = std::clamp(input.max_depth, 0.0F, 1.0F);
  if (params.max_depth < params.min_depth) {
    std::swap(params.max_depth, params.min_depth);
  }
  params.softness = std::clamp(input.softness, 0.0F, 1.0F);
  params.cache_enabled = input.cache_enabled;
  params.tile_size = std::max(64, input.tile_size);
  params.overlap = std::clamp(input.overlap, 0, params.tile_size / 2);
  params.vram_budget_mb = std::max(0, input.vram_budget_mb);
  return params;
}

std::vector<std::string> BuildModelMenu(const zsoda::inference::IInferenceEngine& engine) {
  return engine.ListModelIds();
}

}  // namespace zsoda::ae
