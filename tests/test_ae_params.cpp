#include <cassert>
#include <cmath>

#include "ae/AeParams.h"
#include "inference/ManagedInferenceEngine.h"

namespace {

void TestRenderParamConversion() {
  zsoda::ae::AeParamValues ae;
  ae.quality = 12;
  ae.preserve_ratio = false;
  ae.output = zsoda::ae::AeOutputSelection::kDepthSlice;
  ae.slice_mode = zsoda::ae::AeSliceModeSelection::kBand;
  ae.slice_position = 0.68F;
  ae.slice_range = 0.02F;
  ae.slice_softness = 0.05F;

  const auto render = zsoda::ae::ToRenderParams(ae);
  assert(render.model_id == "distill-any-depth-base");
  assert(render.quality == 8);
  assert(!render.preserve_aspect_ratio);
  assert(render.output_mode == zsoda::core::OutputMode::kSlicing);
  assert(render.slice_normalize);
  assert(std::fabs(render.slice_absolute_depth - 500.0F) < 1e-6F);
  assert(!render.invert);
  assert(std::fabs(render.min_depth - 0.67F) < 1e-6F);
  assert(std::fabs(render.max_depth - 0.69F) < 1e-6F);
  assert(std::fabs(render.softness - 0.05F) < 1e-6F);
  assert(render.cache_enabled);
  assert(render.tile_size == 512);
  assert(render.overlap == 32);
  assert(render.vram_budget_mb == 0);
  assert(!render.freeze_enabled);
  assert(render.extract_token == 0);
  assert(render.mapping_mode == zsoda::core::DepthMappingMode::kRaw);
  assert(render.edge_enhancement > 0.0F);
  assert(render.edge_aware_upsample);
}

void TestSliceWindowConversions() {
  zsoda::ae::AeParamValues ae;
  ae.output = zsoda::ae::AeOutputSelection::kDepthSlice;
  ae.slice_mode = zsoda::ae::AeSliceModeSelection::kFar;
  ae.slice_position = 0.20F;
  ae.slice_softness = 0.25F;

  const auto render = zsoda::ae::ToRenderParams(ae);
  assert(render.slice_normalize);
  assert(std::fabs(render.min_depth - 0.0F) < 1e-6F);
  assert(std::fabs(render.max_depth - 0.20F) < 1e-6F);
  assert(std::fabs(render.softness - 0.25F) < 1e-6F);
}

void TestQualitySelectionHelpers() {
  assert(zsoda::ae::ClampQualitySelection(-1) == 1);
  assert(zsoda::ae::ClampQualitySelection(99) == 8);
  assert(zsoda::ae::QualitySelectionToResolution(1) == 256);
  assert(zsoda::ae::QualitySelectionToResolution(2) == 512);
  assert(zsoda::ae::QualitySelectionToResolution(8) == 2048);
  assert(zsoda::ae::ClampOutputSelection(-1) == zsoda::ae::AeOutputSelection::kDepthMap);
  assert(zsoda::ae::ClampOutputSelection(99) == zsoda::ae::AeOutputSelection::kDepthSlice);
  assert(zsoda::ae::ClampSliceModeSelection(1) == zsoda::ae::AeSliceModeSelection::kNear);
  assert(zsoda::ae::ClampSliceModeSelection(2) == zsoda::ae::AeSliceModeSelection::kFar);
  assert(zsoda::ae::ClampSliceModeSelection(99) == zsoda::ae::AeSliceModeSelection::kBand);
}

void TestModelMenu() {
  zsoda::inference::ManagedInferenceEngine engine("models");
  const auto menu = zsoda::ae::BuildModelMenu(engine);
  assert(!menu.empty());
  assert(menu[0] == "distill-any-depth-base");
}

}  // namespace

void RunAeParamTests() {
  TestRenderParamConversion();
  TestSliceWindowConversions();
  TestQualitySelectionHelpers();
  TestModelMenu();
}
