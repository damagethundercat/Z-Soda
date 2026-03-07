#include "core/DepthOps.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace zsoda::core {
namespace {

float Clamp01(float v) {
  return std::clamp(v, 0.0F, 1.0F);
}

float SanitizeDepthValue(float value, float fallback = 0.0F) {
  return std::isfinite(value) ? value : fallback;
}

float Smoothstep(float edge0, float edge1, float x) {
  if (edge0 == edge1) {
    return x >= edge1 ? 1.0F : 0.0F;
  }
  const float t = Clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3.0F - 2.0F * t);
}

std::vector<float> CollectDepthSamples(const FrameBuffer& depth) {
  std::vector<float> samples;
  if (depth.empty()) {
    return samples;
  }

  const auto& desc = depth.desc();
  const std::size_t pixel_count =
      static_cast<std::size_t>(desc.width) * static_cast<std::size_t>(desc.height);
  if (pixel_count == 0U) {
    return samples;
  }

  constexpr std::size_t kMaxSamples = 65536U;
  const std::size_t stride = std::max<std::size_t>(1U, pixel_count / kMaxSamples);
  samples.reserve(std::min<std::size_t>(pixel_count, kMaxSamples));
  std::size_t linear = 0U;
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x, ++linear) {
      if ((linear % stride) != 0U) {
        continue;
      }
      const float value = depth.at(x, y, 0);
      if (std::isfinite(value)) {
        samples.push_back(value);
      }
    }
  }

  return samples;
}

float ComputeQuantile(std::vector<float>* values, float quantile) {
  if (values == nullptr || values->empty()) {
    return 0.0F;
  }
  quantile = Clamp01(quantile);
  const std::size_t index =
      static_cast<std::size_t>(quantile * static_cast<float>(values->size() - 1U));
  std::nth_element(values->begin(), values->begin() + static_cast<std::ptrdiff_t>(index),
                   values->end());
  return (*values)[index];
}

float Lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

float ParseFloatEnvOrDefault(const char* name,
                             float default_value,
                             float min_value,
                             float max_value) {
  if (name == nullptr || name[0] == '\0') {
    return default_value;
  }
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return default_value;
  }
  char* end = nullptr;
  const float parsed = std::strtof(raw, &end);
  if (end == raw || (end != nullptr && *end != '\0') || !std::isfinite(parsed)) {
    return default_value;
  }
  return std::clamp(parsed, min_value, max_value);
}

bool IsDepthWithinUnitRange(const FrameBuffer& depth) {
  if (depth.empty()) {
    return true;
  }

  const auto& desc = depth.desc();
  float min_value = std::numeric_limits<float>::infinity();
  float max_value = -std::numeric_limits<float>::infinity();
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float value = depth.at(x, y, 0);
      if (!std::isfinite(value)) {
        continue;
      }
      min_value = std::min(min_value, value);
      max_value = std::max(max_value, value);
    }
  }

  if (!std::isfinite(min_value) || !std::isfinite(max_value)) {
    return true;
  }
  return min_value >= -1e-3F && max_value <= 1.001F;
}

void ApplyQuantileDepthMapping(FrameBuffer* depth,
                               const DepthMappingParams& params,
                               GuidedDepthMappingState* guided_state) {
  if (depth == nullptr || depth->empty()) {
    return;
  }

  std::vector<float> samples = CollectDepthSamples(*depth);
  if (samples.empty()) {
    NormalizeDepth(depth, params.invert);
    return;
  }

  const float low_q = std::clamp(params.guided_low_percentile, 0.0F, 1.0F);
  const float high_q = std::clamp(params.guided_high_percentile, 0.0F, 1.0F);
  const float quantile_low = std::min(low_q, high_q);
  const float quantile_high = std::max(low_q, high_q);

  float near_value = ComputeQuantile(&samples, quantile_low);
  float far_value = ComputeQuantile(&samples, quantile_high);
  if (!std::isfinite(near_value) || !std::isfinite(far_value)) {
    NormalizeDepth(depth, params.invert);
    return;
  }
  if (far_value <= near_value + 1e-6F) {
    far_value = near_value + 1e-6F;
  }

  if (guided_state != nullptr) {
    const float alpha = Clamp01(params.guided_update_alpha);
    if (!guided_state->initialized || !std::isfinite(guided_state->near_value) ||
        !std::isfinite(guided_state->far_value) ||
        guided_state->far_value <= guided_state->near_value + 1e-6F) {
      guided_state->near_value = near_value;
      guided_state->far_value = far_value;
      guided_state->initialized = true;
    } else {
      guided_state->near_value = Lerp(guided_state->near_value, near_value, alpha);
      guided_state->far_value = Lerp(guided_state->far_value, far_value, alpha);
      if (guided_state->far_value <= guided_state->near_value + 1e-6F) {
        guided_state->far_value = guided_state->near_value + 1e-6F;
      }
    }
    near_value = guided_state->near_value;
    far_value = guided_state->far_value;
  }

  const float range = std::max(far_value - near_value, 1e-6F);
  const auto& desc = depth->desc();
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float sanitized = SanitizeDepthValue(depth->at(x, y, 0), near_value);
      float normalized = (sanitized - near_value) / range;
      normalized = Clamp01(normalized);
      depth->at(x, y, 0) = params.invert ? (1.0F - normalized) : normalized;
    }
  }
}

void ApplyV2StyleDepthMapping(FrameBuffer* depth, const DepthMappingParams& params) {
  if (depth == nullptr || depth->empty()) {
    return;
  }

  std::vector<float> depth_samples = CollectDepthSamples(*depth);
  if (depth_samples.empty()) {
    NormalizeDepth(depth, params.invert);
    return;
  }

  std::vector<float> disparity_samples;
  disparity_samples.reserve(depth_samples.size());
  for (const float value : depth_samples) {
    if (!std::isfinite(value) || value <= 1e-6F) {
      continue;
    }
    disparity_samples.push_back(1.0F / value);
  }
  if (disparity_samples.size() < 8U) {
    NormalizeDepth(depth, params.invert);
    return;
  }

  const float low_q = std::clamp(params.guided_low_percentile, 0.0F, 1.0F);
  const float high_q = std::clamp(params.guided_high_percentile, 0.0F, 1.0F);
  const float quantile_low = std::min(low_q, high_q);
  const float quantile_high = std::max(low_q, high_q);
  float disp_min = ComputeQuantile(&disparity_samples, quantile_low);
  float disp_max = ComputeQuantile(&disparity_samples, quantile_high);
  if (!std::isfinite(disp_min) || !std::isfinite(disp_max)) {
    NormalizeDepth(depth, params.invert);
    return;
  }
  if (disp_max <= disp_min + 1e-6F) {
    disp_max = disp_min + 1e-6F;
  }

  const float gamma = ParseFloatEnvOrDefault("ZSODA_V2STYLE_GAMMA", 1.35F, 0.2F, 4.0F);
  const float range = std::max(disp_max - disp_min, 1e-6F);
  const auto& desc = depth->desc();
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float raw_depth = depth->at(x, y, 0);
      float disparity = disp_min;
      if (std::isfinite(raw_depth) && raw_depth > 1e-6F) {
        disparity = 1.0F / raw_depth;
      }
      float normalized = (disparity - disp_min) / range;
      normalized = Clamp01(normalized);
      normalized = std::pow(normalized, gamma);
      if (!std::isfinite(normalized)) {
        normalized = 0.0F;
      }
      depth->at(x, y, 0) = params.invert ? (1.0F - normalized) : normalized;
    }
  }
}

}  // namespace

const char* DepthMappingModeName(DepthMappingMode mode) {
  switch (mode) {
    case DepthMappingMode::kRaw:
      return "raw";
    case DepthMappingMode::kNormalize:
      return "normalize";
    case DepthMappingMode::kGuided:
      return "guided";
    case DepthMappingMode::kV2Style:
      return "v2_style";
  }
  return "raw";
}

DepthMappingMode ParseDepthMappingMode(std::string_view value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (const char ch : value) {
    if (ch == '-' || ch == '_' || std::isspace(static_cast<unsigned char>(ch))) {
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  if (normalized == "normalize" || normalized == "norm") {
    return DepthMappingMode::kNormalize;
  }
  if (normalized == "guided" || normalized == "guide") {
    return DepthMappingMode::kGuided;
  }
  if (normalized == "v2style" || normalized == "v2" || normalized == "qd3style" ||
      normalized == "disparity") {
    return DepthMappingMode::kV2Style;
  }
  return DepthMappingMode::kRaw;
}

void ClampDepth(FrameBuffer* depth, bool invert) {
  if (depth == nullptr || depth->empty()) {
    return;
  }

  const auto& desc = depth->desc();
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      float value = Clamp01(SanitizeDepthValue(depth->at(x, y, 0)));
      if (invert) {
        value = 1.0F - value;
      }
      depth->at(x, y, 0) = value;
    }
  }
}

void NormalizeDepth(FrameBuffer* depth, bool invert) {
  if (depth == nullptr || depth->empty()) {
    return;
  }

  float min_value = std::numeric_limits<float>::max();
  float max_value = std::numeric_limits<float>::lowest();
  const auto& desc = depth->desc();

  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float value = depth->at(x, y, 0);
      if (!std::isfinite(value)) {
        continue;
      }
      min_value = std::min(min_value, value);
      max_value = std::max(max_value, value);
    }
  }

  if (!std::isfinite(min_value) || !std::isfinite(max_value)) {
    const float fallback = invert ? 1.0F : 0.0F;
    for (int y = 0; y < desc.height; ++y) {
      for (int x = 0; x < desc.width; ++x) {
        depth->at(x, y, 0) = fallback;
      }
    }
    return;
  }

  const float range = std::max(max_value - min_value, 1e-6F);
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float sanitized = SanitizeDepthValue(depth->at(x, y, 0), min_value);
      float normalized = (sanitized - min_value) / range;
      normalized = Clamp01(normalized);
      depth->at(x, y, 0) = invert ? (1.0F - normalized) : normalized;
    }
  }
}

void ApplyDepthMapping(FrameBuffer* depth,
                       const DepthMappingParams& params,
                       GuidedDepthMappingState* guided_state) {
  if (depth == nullptr || depth->empty()) {
    return;
  }

  if (params.mode == DepthMappingMode::kRaw) {
    if (!IsDepthWithinUnitRange(*depth)) {
      ApplyQuantileDepthMapping(depth, params, nullptr);
      return;
    }
    ClampDepth(depth, params.invert);
    return;
  }

  if (params.mode == DepthMappingMode::kNormalize) {
    NormalizeDepth(depth, params.invert);
    return;
  }

  if (params.mode == DepthMappingMode::kGuided) {
    ApplyQuantileDepthMapping(depth, params, guided_state);
    return;
  }

  ApplyV2StyleDepthMapping(depth, params);
}

FrameBuffer BuildSliceMatte(const FrameBuffer& normalized_depth,
                            float min_depth,
                            float max_depth,
                            float softness) {
  FrameDesc matte_desc = normalized_depth.desc();
  matte_desc.channels = 1;
  matte_desc.format = PixelFormat::kGray32F;

  FrameBuffer matte(matte_desc);
  if (normalized_depth.empty()) {
    return matte;
  }

  min_depth = Clamp01(min_depth);
  max_depth = Clamp01(max_depth);
  if (max_depth < min_depth) {
    std::swap(max_depth, min_depth);
  }

  softness = Clamp01(softness);
  const float feather = softness * 0.5F;
  const float in_start = std::max(0.0F, min_depth - feather);
  const float in_end = min_depth;
  const float out_start = max_depth;
  const float out_end = std::min(1.0F, max_depth + feather);

  const auto& desc = normalized_depth.desc();
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float z = Clamp01(normalized_depth.at(x, y, 0));
      const float rise = Smoothstep(in_start, in_end, z);
      const float fall = 1.0F - Smoothstep(out_start, out_end, z);
      matte.at(x, y, 0) = Clamp01(rise * fall);
    }
  }

  return matte;
}

}  // namespace zsoda::core
