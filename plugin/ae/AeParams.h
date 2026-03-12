#pragma once

#include <string>
#include <vector>

#include "core/RenderPipeline.h"
#include "inference/InferenceEngine.h"

namespace zsoda::ae {

enum class AeParamId {
  kQuality = 1,
  kPreserveRatio = 2,
  kOutput = 3,
  kSliceMode = 4,
  kSlicePosition = 5,
  kSliceRange = 6,
  kSliceSoftness = 7,
  kLast = kSliceSoftness,
};

enum class AeOutputSelection {
  kDepthMap = 1,
  kDepthSlice = 2,
};

enum class AeSliceModeSelection {
  kNear = 1,
  kFar = 2,
  kBand = 3,
};

struct AeParamValues {
  std::string model_id = "distill-any-depth-base";
  int quality = 2;
  bool preserve_ratio = true;
  AeOutputSelection output = AeOutputSelection::kDepthMap;
  AeSliceModeSelection slice_mode = AeSliceModeSelection::kBand;
  float slice_position = 0.5F;
  float slice_range = 0.1F;
  float slice_softness = 0.05F;
};

AeParamValues DefaultAeParams();
zsoda::core::RenderParams ToRenderParams(const AeParamValues& input);
std::vector<std::string> BuildModelMenu(const zsoda::inference::IInferenceEngine& engine);
int ClampQualitySelection(int selection);
int QualitySelectionToResolution(int selection);
AeOutputSelection ClampOutputSelection(int selection);
AeSliceModeSelection ClampSliceModeSelection(int selection);

}  // namespace zsoda::ae
