#include "ae/AeParams.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace zsoda::ae {
namespace {

constexpr const char* kDefaultLockedModelId = "distill-any-depth-base";
constexpr std::array<int, 8> kQualityResolutions = {256, 512, 768, 1024, 1280, 1536, 1920, 2048};

int ClampQualitySelectionInternal(int selection) {
  return std::clamp(selection, 1, static_cast<int>(kQualityResolutions.size()));
}

AeOutputSelection ClampOutputSelectionInternal(int selection) {
  return selection <= static_cast<int>(AeOutputSelection::kDepthMap)
             ? AeOutputSelection::kDepthMap
             : AeOutputSelection::kDepthSlice;
}

AeSliceModeSelection ClampSliceModeSelectionInternal(int selection) {
  switch (selection) {
    case static_cast<int>(AeSliceModeSelection::kNear):
      return AeSliceModeSelection::kNear;
    case static_cast<int>(AeSliceModeSelection::kFar):
      return AeSliceModeSelection::kFar;
    default:
      return AeSliceModeSelection::kBand;
  }
}

std::string ReadEnvOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return {};
  }
  return value;
}

std::string ResolveLockedModelId() {
  std::string model_id = ReadEnvOrEmpty("ZSODA_LOCKED_MODEL_ID");
  if (model_id.empty()) {
    // Keep the older alias readable so existing automation/env setups do not
    // break, but route everything to the DAD-only production path.
    model_id = ReadEnvOrEmpty("ZSODA_HQ_MODEL_ID");
  }
  if (model_id.empty()) {
    model_id = kDefaultLockedModelId;
  }
  return model_id;
}

bool IsDistillAnyDepthModelId(std::string_view model_id) {
  return model_id.rfind("distill-any-depth", 0) == 0;
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

  const int clamped_quality = ClampQualitySelectionInternal(params->quality);
  params->quality = clamped_quality;
  const bool distill_any_depth = IsDistillAnyDepthModelId(params->model_id);
  params->temporal_alpha = 1.0F;
  if (clamped_quality >= 6) {
    params->edge_enhancement = distill_any_depth ? 0.035F : 0.06F;
    params->guided_low_percentile = distill_any_depth ? 0.006F : 0.008F;
    params->guided_high_percentile = distill_any_depth ? 0.994F : 0.992F;
  } else if (clamped_quality >= 3) {
    params->edge_enhancement = distill_any_depth ? 0.028F : 0.045F;
    params->guided_low_percentile = distill_any_depth ? 0.010F : 0.012F;
    params->guided_high_percentile = distill_any_depth ? 0.990F : 0.988F;
  } else {
    params->edge_enhancement = distill_any_depth ? 0.020F : 0.03F;
    params->guided_low_percentile = distill_any_depth ? 0.015F : 0.02F;
    params->guided_high_percentile = distill_any_depth ? 0.985F : 0.98F;
  }

  params->mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  params->guided_update_alpha = 0.10F;
  params->temporal_edge_aware = false;
  params->temporal_edge_threshold = 0.08F;
  params->temporal_scene_cut_threshold = 0.18F;
  params->edge_guidance_sigma = 0.10F;
  params->edge_aware_upsample = true;
}

void ApplySliceDefaults(const AeParamValues& input, zsoda::core::RenderParams* params) {
  if (params == nullptr) {
    return;
  }

  params->output_mode = input.output == AeOutputSelection::kDepthSlice
                            ? zsoda::core::OutputMode::kSlicing
                            : zsoda::core::OutputMode::kDepthMap;
  params->slice_normalize = true;
  params->slice_absolute_depth = 500.0F;
  params->softness = std::clamp(input.slice_softness, 0.0F, 1.0F);

  const float position = std::clamp(input.slice_position, 0.0F, 1.0F);
  const float range = std::clamp(input.slice_range, 0.0F, 1.0F);

  switch (ClampSliceModeSelectionInternal(static_cast<int>(input.slice_mode))) {
    case AeSliceModeSelection::kNear:
      params->min_depth = position;
      params->max_depth = 1.0F;
      break;
    case AeSliceModeSelection::kFar:
      params->min_depth = 0.0F;
      params->max_depth = position;
      break;
    case AeSliceModeSelection::kBand:
    default: {
      const float half_range = 0.5F * range;
      params->min_depth = std::clamp(position - half_range, 0.0F, 1.0F);
      params->max_depth = std::clamp(position + half_range, 0.0F, 1.0F);
      break;
    }
  }

  if (params->max_depth < params->min_depth) {
    std::swap(params->max_depth, params->min_depth);
  }
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
  params.model_id = ResolveLockedModelId();
  params.quality = ClampQualitySelectionInternal(input.quality);
  params.preserve_aspect_ratio = input.preserve_ratio;
  ApplyQualityDefaults(&params);
  ApplySliceDefaults(input, &params);
  ApplyEnvironmentOverrides(&params);
  return params;
}

std::vector<std::string> BuildModelMenu(const zsoda::inference::IInferenceEngine& engine) {
  const std::vector<std::string> available = engine.ListModelIds();
  if (available.empty()) {
    return {};
  }

  const std::string locked_model = ResolveLockedModelId();
  if (std::find(available.begin(), available.end(), locked_model) != available.end()) {
    return {locked_model};
  }

  return {available.front()};
}

int ClampQualitySelection(int selection) {
  return ClampQualitySelectionInternal(selection);
}

int QualitySelectionToResolution(int selection) {
  return kQualityResolutions[static_cast<std::size_t>(ClampQualitySelectionInternal(selection) - 1)];
}

AeOutputSelection ClampOutputSelection(int selection) {
  return ClampOutputSelectionInternal(selection);
}

AeSliceModeSelection ClampSliceModeSelection(int selection) {
  return ClampSliceModeSelectionInternal(selection);
}

}  // namespace zsoda::ae
