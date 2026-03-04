#pragma once

#include <string>
#include <vector>

#include "core/RenderPipeline.h"
#include "inference/InferenceEngine.h"

namespace zsoda::ae {

enum class AeParamId {
  kModel = 1,
  kQuality = 2,
  kOutputMode = 3,
  kInvert = 4,
  kMinDepth = 5,
  kMaxDepth = 6,
  kSoftness = 7,
  kCacheEnable = 8,
  kTileSize = 9,
  kOverlap = 10,
  kVramBudgetMb = 11,
  kFreezeEnable = 12,
  kExtractDepthMap = 13,
};

enum class AeOutputMode {
  kDepthMap = 0,
  kSlicing = 1,
};

struct AeParamValues {
  std::string model_id = "depth-anything-v3-small";
  int quality = 1;
  AeOutputMode output_mode = AeOutputMode::kDepthMap;
  bool invert = false;
  float min_depth = 0.25F;
  float max_depth = 0.75F;
  float softness = 0.1F;
  bool cache_enabled = true;
  int tile_size = 512;
  int overlap = 32;
  int vram_budget_mb = 0;
  bool freeze_enabled = false;
  int extract_token = 0;
};

AeParamValues DefaultAeParams();
zsoda::core::RenderParams ToRenderParams(const AeParamValues& input);
std::vector<std::string> BuildModelMenu(const zsoda::inference::IInferenceEngine& engine);

}  // namespace zsoda::ae
