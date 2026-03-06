#pragma once

#include <string>
#include <vector>

#include "core/RenderPipeline.h"
#include "inference/InferenceEngine.h"

namespace zsoda::ae {

enum class AeParamId {
  kQuality = 1,
  kPreserveRatio = 2,
  kQualityBoostEnable = 3,
  kQualityBoostLevel = 4,
  kTimeConsistency = 5,
  kAdvancedGroupStart = 6,
  kModel = 7,
  kOutputMode = 8,
  kInvert = 9,
  kMinDepth = 10,
  kMaxDepth = 11,
  kSoftness = 12,
  kCacheEnable = 13,
  kTileSize = 14,
  kOverlap = 15,
  kVramBudgetMb = 16,
  kFreezeEnable = 17,
  kExtractDepthMap = 18,
  kAdvancedGroupEnd = 19,
};

enum class AeOutputMode {
  kDepthMap = 0,
  kSlicing = 1,
};

struct AeParamValues {
  std::string model_id = "depth-anything-v3-large";
  int quality = 2;
  bool preserve_ratio = true;
  bool quality_boost_enabled = false;
  int quality_boost_level = 4;
  bool time_consistency = false;
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
int ClampQualitySelection(int selection);
int QualitySelectionToResolution(int selection);

}  // namespace zsoda::ae
