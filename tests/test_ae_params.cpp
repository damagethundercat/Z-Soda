#include <cassert>

#include "ae/AeParams.h"
#include "inference/ManagedInferenceEngine.h"

namespace {

void TestRenderParamConversion() {
  zsoda::ae::AeParamValues ae;
  ae.model_id = "depth-anything-v3-large";
  ae.quality = 4;
  ae.output_mode = zsoda::ae::AeOutputMode::kSlicing;
  ae.invert = true;
  ae.min_depth = 0.8F;
  ae.max_depth = 0.2F;
  ae.softness = 2.0F;
  ae.cache_enabled = false;
  ae.tile_size = 32;
  ae.overlap = 300;
  ae.vram_budget_mb = -24;
  ae.freeze_enabled = true;
  ae.extract_token = -11;

  const auto render = zsoda::ae::ToRenderParams(ae);
  assert(render.model_id == "depth-anything-v3-large");
  assert(render.quality == 3);
  assert(render.output_mode == zsoda::core::OutputMode::kSlicing);
  assert(render.invert);
  assert(render.min_depth == 0.2F);
  assert(render.max_depth == 0.8F);
  assert(render.softness == 1.0F);
  assert(!render.cache_enabled);
  assert(render.tile_size == 64);
  assert(render.overlap == 32);
  assert(render.vram_budget_mb == 0);
  assert(render.freeze_enabled);
  assert(render.extract_token == 0);
  assert(render.mapping_mode == zsoda::core::DepthMappingMode::kRaw);
  assert(render.temporal_alpha > 0.0F && render.temporal_alpha < 1.0F);
  assert(render.temporal_edge_aware);
  assert(render.edge_enhancement > 0.0F);
  assert(render.edge_aware_upsample);
}

void TestModelMenu() {
  zsoda::inference::ManagedInferenceEngine engine("models");
  const auto menu = zsoda::ae::BuildModelMenu(engine);
  assert(!menu.empty());
  assert(menu[0] == "depth-anything-v3-small");
}

}  // namespace

void RunAeParamTests() {
  TestRenderParamConversion();
  TestModelMenu();
}
