#include "ae/AeParams.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace zsoda::ae {
namespace {

std::string ReadEnvOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return {};
  }
  return value;
}

bool TryParseFloat(std::string_view text, float* out_value) {
  if (out_value == nullptr || text.empty()) {
    return false;
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return false;
  }

  const std::string owned(text);
  char* end = nullptr;
  const float parsed = std::strtof(owned.c_str(), &end);
  if (end == owned.c_str() || (end != nullptr && *end != '\0') || !std::isfinite(parsed)) {
    return false;
  }
  *out_value = parsed;
  return true;
}

bool TryParseBool(std::string_view text, bool* out_value) {
  if (out_value == nullptr || text.empty()) {
    return false;
  }
  std::string normalized;
  normalized.reserve(text.size());
  for (const char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  if (normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes") {
    *out_value = true;
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "off" || normalized == "no") {
    *out_value = false;
    return true;
  }
  return false;
}

void ApplyQualityDefaults(zsoda::core::RenderParams* params) {
  if (params == nullptr) {
    return;
  }

  switch (params->quality) {
    case 1:
      params->temporal_alpha = 0.72F;
      params->edge_enhancement = 0.08F;
      break;
    case 2:
      params->temporal_alpha = 0.56F;
      params->edge_enhancement = 0.14F;
      break;
    case 3:
    default:
      params->temporal_alpha = 0.42F;
      params->edge_enhancement = 0.22F;
      break;
  }

  params->mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  params->guided_low_percentile = 0.05F;
  params->guided_high_percentile = 0.95F;
  params->guided_update_alpha = 0.15F;
  params->temporal_edge_aware = true;
  params->temporal_edge_threshold = 0.08F;
  params->temporal_scene_cut_threshold = 0.18F;
  params->edge_guidance_sigma = 0.12F;
  params->edge_aware_upsample = true;
}

void ApplyEnvironmentOverrides(zsoda::core::RenderParams* params) {
  if (params == nullptr) {
    return;
  }

  const std::string mapping_mode = ReadEnvOrEmpty("ZSODA_DEPTH_MAPPING_MODE");
  if (!mapping_mode.empty()) {
    params->mapping_mode = zsoda::core::ParseDepthMappingMode(mapping_mode);
  }

  float parsed_float = 0.0F;
  if (TryParseFloat(ReadEnvOrEmpty("ZSODA_GUIDED_LOW_PERCENTILE"), &parsed_float)) {
    params->guided_low_percentile = std::clamp(parsed_float, 0.0F, 1.0F);
  }
  if (TryParseFloat(ReadEnvOrEmpty("ZSODA_GUIDED_HIGH_PERCENTILE"), &parsed_float)) {
    params->guided_high_percentile = std::clamp(parsed_float, 0.0F, 1.0F);
  }
  if (TryParseFloat(ReadEnvOrEmpty("ZSODA_GUIDED_UPDATE_ALPHA"), &parsed_float)) {
    params->guided_update_alpha = std::clamp(parsed_float, 0.0F, 1.0F);
  }
  if (TryParseFloat(ReadEnvOrEmpty("ZSODA_TEMPORAL_ALPHA"), &parsed_float)) {
    params->temporal_alpha = std::clamp(parsed_float, 0.0F, 1.0F);
  }
  if (TryParseFloat(ReadEnvOrEmpty("ZSODA_TEMPORAL_EDGE_THRESHOLD"), &parsed_float)) {
    params->temporal_edge_threshold = std::clamp(parsed_float, 0.0F, 1.0F);
  }
  std::string temporal_scene_cut = ReadEnvOrEmpty("ZSODA_TEMPORAL_SCENE_CUT");
  if (temporal_scene_cut.empty()) {
    temporal_scene_cut = ReadEnvOrEmpty("ZSODA_TEMPORAL_SCENE_CUT_THRESHOLD");
  }
  if (TryParseFloat(temporal_scene_cut, &parsed_float)) {
    params->temporal_scene_cut_threshold = std::clamp(parsed_float, 0.0F, 1.0F);
  }
  if (TryParseFloat(ReadEnvOrEmpty("ZSODA_EDGE_ENHANCEMENT"), &parsed_float)) {
    params->edge_enhancement = std::clamp(parsed_float, 0.0F, 1.0F);
  }
  if (TryParseFloat(ReadEnvOrEmpty("ZSODA_EDGE_GUIDANCE_SIGMA"), &parsed_float)) {
    params->edge_guidance_sigma = std::clamp(parsed_float, 0.0F, 1.0F);
  }

  bool parsed_bool = false;
  if (TryParseBool(ReadEnvOrEmpty("ZSODA_TEMPORAL_EDGE_AWARE"), &parsed_bool)) {
    params->temporal_edge_aware = parsed_bool;
  }
  if (TryParseBool(ReadEnvOrEmpty("ZSODA_EDGE_AWARE_UPSAMPLE"), &parsed_bool)) {
    params->edge_aware_upsample = parsed_bool;
  }
}

}  // namespace

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
  params.freeze_enabled = input.freeze_enabled;
  params.extract_token = std::max(0, input.extract_token);
  ApplyQualityDefaults(&params);
  ApplyEnvironmentOverrides(&params);
  return params;
}

std::vector<std::string> BuildModelMenu(const zsoda::inference::IInferenceEngine& engine) {
  return engine.ListModelIds();
}

}  // namespace zsoda::ae
