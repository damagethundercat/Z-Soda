#include "core/RenderPipeline.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "inference/InferenceEngine.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace zsoda::core {
namespace {

constexpr int kMaxAdaptiveTileAttempts = 4;
constexpr int kMinAdaptiveRetryTileSize = 64;
constexpr int kMinDownscaleDivisor = 2;
constexpr int kMaxDownscaleDivisor = 8;
constexpr std::size_t kFallbackWorkingSetBytesPerPixelEstimate = 16U;
constexpr float kEdgeAwareUpsampleGuidanceSigma = 0.12F;

int ParseIntEnvOrDefault(const char* name, int default_value, int min_value, int max_value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return default_value;
  }
  char* end = nullptr;
  const long parsed = std::strtol(raw, &end, 10);
  if (end == raw || (end != nullptr && *end != '\0')) {
    return default_value;
  }
  if (parsed < static_cast<long>(min_value) || parsed > static_cast<long>(max_value)) {
    return default_value;
  }
  return static_cast<int>(parsed);
}

float ParseFloatEnvOrDefault(const char* name,
                             float default_value,
                             float min_value,
                             float max_value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == 0) {
    return default_value;
  }
  char* end = nullptr;
  const float parsed = std::strtof(raw, &end);
  if (end == raw || (end != nullptr && *end != 0)) {
    return default_value;
  }
  if (!std::isfinite(parsed) || parsed < min_value || parsed > max_value) {
    return default_value;
  }
  return parsed;
}

bool ParseBoolEnvOrDefault(const char* name, bool default_value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr) {
    return default_value;
  }

  std::string normalized;
  while (*raw != '\0') {
    const char ch = *raw++;
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '-' || ch == '_') {
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  if (normalized.empty()) {
    return default_value;
  }
  if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
    return false;
  }
  return default_value;
}

std::uint64_t HashCombine64(std::uint64_t lhs, std::uint64_t rhs) {
  return lhs ^ (rhs + 0x9e3779b97f4a7c15ULL + (lhs << 6ULL) + (lhs >> 2ULL));
}

std::uint64_t HashFromBool(bool value) {
  return value ? 1ULL : 0ULL;
}

std::uint64_t HashFromInt(int value) {
  return static_cast<std::uint64_t>(static_cast<std::uint32_t>(value));
}

std::uint64_t HashFromPermille(float value) {
  const int clipped = static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 1000.0F));
  return HashFromInt(std::max(0, clipped));
}

const char* SafeCStr(const char* value, const char* fallback = "<null>") {
  return value != nullptr ? value : fallback;
}

void AppendPipelineTrace(const char* stage, const char* detail = nullptr) {
#if defined(_WIN32)
  static const bool enabled = []() -> bool {
    const char* raw = std::getenv("ZSODA_PIPELINE_TRACE");
    if (raw == nullptr) {
      return false;
    }
    std::string normalized;
    while (*raw != '\0') {
      const char ch = *raw++;
      if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
        continue;
      }
      normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return normalized == "1" || normalized == "true" || normalized == "on" ||
           normalized == "yes";
  }();
  if (!enabled) {
    return;
  }

  char temp_path[MAX_PATH] = {};
  const DWORD written = ::GetTempPathA(MAX_PATH, temp_path);
  if (written == 0 || written >= MAX_PATH) {
    return;
  }

  char log_path[MAX_PATH] = {};
  std::snprintf(log_path, sizeof(log_path), "%s%s", temp_path, "ZSoda_AE_Runtime.log");
  FILE* file = std::fopen(log_path, "ab");
  if (file == nullptr) {
    return;
  }

  SYSTEMTIME now = {};
  ::GetLocalTime(&now);
  const unsigned long tid = static_cast<unsigned long>(::GetCurrentThreadId());
  std::fprintf(file,
               "%04u-%02u-%02u %02u:%02u:%02u.%03u | PipelineTrace | tid=%lu, stage=%s, detail=%s\r\n",
               static_cast<unsigned>(now.wYear),
               static_cast<unsigned>(now.wMonth),
               static_cast<unsigned>(now.wDay),
               static_cast<unsigned>(now.wHour),
               static_cast<unsigned>(now.wMinute),
               static_cast<unsigned>(now.wSecond),
               static_cast<unsigned>(now.wMilliseconds),
               tid,
               stage != nullptr ? stage : "<null>",
               (detail != nullptr && detail[0] != '\0') ? detail : "<none>");
  std::fclose(file);
#else
  (void)stage;
  (void)detail;
#endif
}

FrameBuffer CropGray(const FrameBuffer& source, const TileRect& tile) {
  FrameDesc desc = source.desc();
  desc.width = tile.width;
  desc.height = tile.height;
  FrameBuffer cropped(desc);

  for (int y = 0; y < tile.height; ++y) {
    for (int x = 0; x < tile.width; ++x) {
      const int sx = tile.x + x;
      const int sy = tile.y + y;
      for (int c = 0; c < desc.channels; ++c) {
        cropped.at(x, y, c) = source.at(sx, sy, c);
      }
    }
  }
  return cropped;
}

FrameBuffer ResizeBilinear(const FrameBuffer& source, int output_width, int output_height) {
  FrameDesc desc = source.desc();
  desc.width = output_width;
  desc.height = output_height;
  FrameBuffer resized(desc);

  const int source_width = source.desc().width;
  const int source_height = source.desc().height;
  const int channels = source.desc().channels;

  for (int y = 0; y < output_height; ++y) {
    const float src_y = (static_cast<float>(y) + 0.5F) *
                            (static_cast<float>(source_height) /
                             static_cast<float>(std::max(1, output_height))) -
                        0.5F;
    const float clamped_y =
        std::clamp(src_y, 0.0F, static_cast<float>(std::max(0, source_height - 1)));
    const int y0 = static_cast<int>(clamped_y);
    const int y1 = std::min(y0 + 1, source_height - 1);
    const float ty = clamped_y - static_cast<float>(y0);
    for (int x = 0; x < output_width; ++x) {
      const float src_x = (static_cast<float>(x) + 0.5F) *
                              (static_cast<float>(source_width) /
                               static_cast<float>(std::max(1, output_width))) -
                          0.5F;
      const float clamped_x =
          std::clamp(src_x, 0.0F, static_cast<float>(std::max(0, source_width - 1)));
      const int x0 = static_cast<int>(clamped_x);
      const int x1 = std::min(x0 + 1, source_width - 1);
      const float tx = clamped_x - static_cast<float>(x0);
      for (int c = 0; c < channels; ++c) {
        const float p00 = source.at(x0, y0, c);
        const float p01 = source.at(x1, y0, c);
        const float p10 = source.at(x0, y1, c);
        const float p11 = source.at(x1, y1, c);
        const float top = p00 + (p01 - p00) * tx;
        const float bottom = p10 + (p11 - p10) * tx;
        resized.at(x, y, c) = top + (bottom - top) * ty;
      }
    }
  }
  return resized;
}

FrameBuffer BuildSourceLuma(const FrameBuffer& source) {
  FrameDesc luma_desc;
  luma_desc.width = source.desc().width;
  luma_desc.height = source.desc().height;
  luma_desc.channels = 1;
  luma_desc.format = PixelFormat::kGray32F;
  FrameBuffer luma(luma_desc);
  if (source.empty()) {
    return luma;
  }

  const int channels = source.desc().channels;
  for (int y = 0; y < luma_desc.height; ++y) {
    for (int x = 0; x < luma_desc.width; ++x) {
      if (channels <= 1) {
        luma.at(x, y, 0) = std::clamp(source.at(x, y, 0), 0.0F, 1.0F);
        continue;
      }
      const float r = source.at(x, y, 0);
      const float g = source.at(x, y, std::min(1, channels - 1));
      const float b = source.at(x, y, std::min(2, channels - 1));
      const float luminance = r * 0.2126F + g * 0.7152F + b * 0.0722F;
      luma.at(x, y, 0) = std::clamp(luminance, 0.0F, 1.0F);
    }
  }
  return luma;
}

void ApplySourceAlphaMask(FrameBuffer* output, const FrameBuffer& source, float outside_value) {
  if (output == nullptr || output->empty() || source.empty()) {
    return;
  }
  const FrameDesc& out_desc = output->desc();
  const FrameDesc& src_desc = source.desc();
  if (src_desc.channels < 4 || src_desc.width != out_desc.width ||
      src_desc.height != out_desc.height) {
    return;
  }

  const float alpha_threshold =
      ParseFloatEnvOrDefault("ZSODA_ALPHA_MASK_THRESHOLD", 0.01F, 0.0F, 1.0F);
  const bool soft_blend =
      ParseBoolEnvOrDefault("ZSODA_ALPHA_MASK_SOFT_BLEND", true);
  const float clamped_outside = std::clamp(outside_value, 0.0F, 1.0F);
  for (int y = 0; y < out_desc.height; ++y) {
    for (int x = 0; x < out_desc.width; ++x) {
      float alpha = source.at(x, y, 3);
      if (!std::isfinite(alpha)) {
        output->at(x, y, 0) = clamped_outside;
        continue;
      }
      alpha = std::clamp(alpha, 0.0F, 1.0F);
      if (alpha <= alpha_threshold) {
        output->at(x, y, 0) = clamped_outside;
      } else if (soft_blend && alpha < 1.0F) {
        const float value = output->at(x, y, 0);
        output->at(x, y, 0) = value * alpha + clamped_outside * (1.0F - alpha);
      }
    }
  }
}

float ComputeMeanAbsoluteDifference(const FrameBuffer& lhs, const FrameBuffer& rhs) {
  if (lhs.empty() || rhs.empty() || lhs.desc().width != rhs.desc().width ||
      lhs.desc().height != rhs.desc().height || lhs.desc().channels != 1 ||
      rhs.desc().channels != 1) {
    return 1.0F;
  }

  const auto& desc = lhs.desc();
  const std::size_t pixel_count =
      static_cast<std::size_t>(desc.width) * static_cast<std::size_t>(desc.height);
  if (pixel_count == 0U) {
    return 0.0F;
  }

  constexpr std::size_t kMaxSamples = 65536U;
  const std::size_t stride = std::max<std::size_t>(1U, pixel_count / kMaxSamples);
  std::size_t sampled = 0U;
  float total = 0.0F;
  std::size_t linear = 0U;
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x, ++linear) {
      if ((linear % stride) != 0U) {
        continue;
      }
      total += std::fabs(lhs.at(x, y, 0) - rhs.at(x, y, 0));
      ++sampled;
    }
  }
  if (sampled == 0U) {
    return 0.0F;
  }
  return total / static_cast<float>(sampled);
}

FrameBuffer BlurGray3x3(const FrameBuffer& source) {
  if (source.empty() || source.desc().channels != 1) {
    return source;
  }

  FrameBuffer blurred(source.desc());
  const auto& desc = source.desc();
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      float sum = 0.0F;
      float weight = 0.0F;
      for (int ky = -1; ky <= 1; ++ky) {
        const int sy = std::clamp(y + ky, 0, desc.height - 1);
        for (int kx = -1; kx <= 1; ++kx) {
          const int sx = std::clamp(x + kx, 0, desc.width - 1);
          const float kernel = (kx == 0 && ky == 0) ? 4.0F : ((kx == 0 || ky == 0) ? 2.0F : 1.0F);
          sum += source.at(sx, sy, 0) * kernel;
          weight += kernel;
        }
      }
      blurred.at(x, y, 0) = (weight > 1e-6F) ? (sum / weight) : source.at(x, y, 0);
    }
  }
  return blurred;
}

FrameBuffer ApplySliceMatteToSource(const FrameBuffer& source, const FrameBuffer& matte) {
  if (source.empty() || matte.empty() || source.desc().width != matte.desc().width ||
      source.desc().height != matte.desc().height || matte.desc().channels < 1) {
    return matte;
  }

  FrameDesc output_desc = source.desc();
  if (output_desc.channels < 4) {
    output_desc.channels = 4;
    output_desc.format = PixelFormat::kRGBA32F;
  }
  FrameBuffer output(output_desc);

  for (int y = 0; y < output_desc.height; ++y) {
    for (int x = 0; x < output_desc.width; ++x) {
      const float mask = std::clamp(matte.at(x, y, 0), 0.0F, 1.0F);
      const float src_r = source.at(x, y, 0);
      const float src_g = source.at(x, y, std::min(1, source.desc().channels - 1));
      const float src_b = source.at(x, y, std::min(2, source.desc().channels - 1));
      const float src_a =
          source.desc().channels >= 4 ? source.at(x, y, 3) : 1.0F;

      output.at(x, y, 0) = std::clamp(src_r * mask, 0.0F, 1.0F);
      output.at(x, y, 1) = std::clamp(src_g * mask, 0.0F, 1.0F);
      output.at(x, y, 2) = std::clamp(src_b * mask, 0.0F, 1.0F);
      if (output_desc.channels >= 4) {
        output.at(x, y, 3) = std::clamp(src_a * mask, 0.0F, 1.0F);
      }
    }
  }
  return output;
}

struct SliceWindow {
  float min_depth = 0.0F;
  float max_depth = 1.0F;
};

FrameBuffer BuildSliceDepthInput(const FrameBuffer& depth, const RenderParams& params) {
  if (params.slice_normalize) {
    FrameBuffer normalized = depth;
    NormalizeDepth(&normalized, false);
    return normalized;
  }
  return depth;
}

SliceWindow ResolveSliceWindow(const RenderParams& params) {
  SliceWindow window;
  window.min_depth = std::clamp(std::min(params.min_depth, params.max_depth), 0.0F, 1.0F);
  window.max_depth = std::clamp(std::max(params.min_depth, params.max_depth), 0.0F, 1.0F);

  if (params.slice_normalize) {
    return window;
  }

  const float band_width = std::max(0.0F, window.max_depth - window.min_depth);
  const float band_center =
      std::clamp(params.slice_absolute_depth / 1000.0F, 0.0F, 1.0F);
  const float half_width = 0.5F * band_width;
  window.min_depth = std::clamp(band_center - half_width, 0.0F, 1.0F);
  window.max_depth = std::clamp(band_center + half_width, 0.0F, 1.0F);
  return window;
}

FrameBuffer BuildTemporalLowFrequency(const FrameBuffer& source) {
  if (source.empty() || source.desc().channels != 1) {
    return source;
  }
  // Use a wider effective support than a single 3x3 pass so the temporal path
  // stabilizes larger low-frequency shapes while leaving current-frame detail intact.
  return BlurGray3x3(BlurGray3x3(source));
}

float ComputeLocalGradientMagnitude(const FrameBuffer& source, int x, int y) {
  const auto& desc = source.desc();
  if (source.empty() || desc.channels != 1) {
    return 0.0F;
  }
  const int xm = std::max(0, x - 1);
  const int xp = std::min(desc.width - 1, x + 1);
  const int ym = std::max(0, y - 1);
  const int yp = std::min(desc.height - 1, y + 1);
  const float gx = std::fabs(source.at(xp, y, 0) - source.at(xm, y, 0));
  const float gy = std::fabs(source.at(x, yp, 0) - source.at(x, ym, 0));
  return 0.5F * (gx + gy);
}

struct RunningMoments {
  double sum = 0.0;
  double sum_sq = 0.0;
  std::size_t count = 0U;

  void Add(float value) {
    sum += static_cast<double>(value);
    sum_sq += static_cast<double>(value) * static_cast<double>(value);
    ++count;
  }

  [[nodiscard]] float Mean() const {
    if (count == 0U) {
      return 0.0F;
    }
    return static_cast<float>(sum / static_cast<double>(count));
  }

  [[nodiscard]] float StdDev() const {
    if (count == 0U) {
      return 0.0F;
    }
    const double mean = sum / static_cast<double>(count);
    const double variance =
        std::max(0.0, (sum_sq / static_cast<double>(count)) - mean * mean);
    return static_cast<float>(std::sqrt(variance));
  }
};

void AccumulateFrameMoments(const FrameBuffer& source, RunningMoments* moments) {
  if (source.empty() || moments == nullptr || source.desc().channels != 1) {
    return;
  }
  const auto& desc = source.desc();
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      moments->Add(source.at(x, y, 0));
    }
  }
}

FrameBuffer AlignHistoryLowFrequency(const FrameBuffer& current_low,
                                     const FrameBuffer& history_low,
                                     const FrameBuffer& current_luma,
                                     const FrameBuffer& history_luma,
                                     float stability_threshold,
                                     float edge_threshold) {
  FrameBuffer aligned(history_low.desc());
  if (current_low.empty() || history_low.empty() || current_luma.empty() || history_luma.empty() ||
      current_low.desc().width != history_low.desc().width ||
      current_low.desc().height != history_low.desc().height) {
    return history_low;
  }

  RunningMoments current_stats;
  RunningMoments history_stats;
  const auto& desc = current_low.desc();
  const float clamped_stability = std::max(0.005F, stability_threshold);
  const float clamped_edge = std::max(0.01F, edge_threshold);
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float luma_delta =
          std::fabs(current_luma.at(x, y, 0) - history_luma.at(x, y, 0));
      if (luma_delta > clamped_stability) {
        continue;
      }
      const float current_gradient = ComputeLocalGradientMagnitude(current_luma, x, y);
      const float history_gradient = ComputeLocalGradientMagnitude(history_luma, x, y);
      if (std::max(current_gradient, history_gradient) > clamped_edge) {
        continue;
      }
      current_stats.Add(current_low.at(x, y, 0));
      history_stats.Add(history_low.at(x, y, 0));
    }
  }

  if (current_stats.count < 64U || history_stats.count < 64U) {
    current_stats = {};
    history_stats = {};
    AccumulateFrameMoments(current_low, &current_stats);
    AccumulateFrameMoments(history_low, &history_stats);
  }

  const float current_mean = current_stats.Mean();
  const float history_mean = history_stats.Mean();
  const float current_std = current_stats.StdDev();
  const float history_std = history_stats.StdDev();
  const float scale =
      (history_std > 1e-4F && current_std > 1e-4F)
          ? std::clamp(current_std / history_std, 0.5F, 2.0F)
          : 1.0F;
  const float bias = current_mean - history_mean * scale;

  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      aligned.at(x, y, 0) =
          std::clamp(history_low.at(x, y, 0) * scale + bias, 0.0F, 1.0F);
    }
  }
  return aligned;
}

struct PercentileRange {
  float low = 0.0F;
  float high = 1.0F;
  bool valid = false;
};

PercentileRange ComputeDepthPercentileRange(const FrameBuffer& depth,
                                            float low_quantile,
                                            float high_quantile) {
  PercentileRange range;
  if (depth.empty() || depth.desc().channels != 1) {
    return range;
  }

  const auto& desc = depth.desc();
  const std::size_t pixel_count =
      static_cast<std::size_t>(desc.width) * static_cast<std::size_t>(desc.height);
  if (pixel_count == 0U) {
    return range;
  }

  constexpr std::size_t kMaxSamples = 131072U;
  const std::size_t stride = std::max<std::size_t>(1U, pixel_count / kMaxSamples);
  std::vector<float> values;
  values.reserve(std::min<std::size_t>(pixel_count, kMaxSamples));

  std::size_t linear = 0U;
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x, ++linear) {
      if ((linear % stride) != 0U) {
        continue;
      }
      const float value = depth.at(x, y, 0);
      if (std::isfinite(value)) {
        values.push_back(value);
      }
    }
  }

  if (values.size() < 16U) {
    return range;
  }

  const float q_low = std::clamp(std::min(low_quantile, high_quantile), 0.0F, 1.0F);
  const float q_high = std::clamp(std::max(low_quantile, high_quantile), 0.0F, 1.0F);
  const std::size_t low_index =
      static_cast<std::size_t>(q_low * static_cast<float>(values.size() - 1U));
  const std::size_t high_index =
      static_cast<std::size_t>(q_high * static_cast<float>(values.size() - 1U));

  std::nth_element(values.begin(),
                   values.begin() + static_cast<std::ptrdiff_t>(low_index),
                   values.end());
  range.low = values[low_index];

  std::nth_element(values.begin(),
                   values.begin() + static_cast<std::ptrdiff_t>(high_index),
                   values.end());
  range.high = values[high_index];
  range.valid = std::isfinite(range.low) && std::isfinite(range.high) &&
                (range.high > range.low + 1e-6F);
  return range;
}

struct DetailBoostStats {
  float mean = 0.0F;
  float stddev = 0.0F;
  bool valid = false;
};

DetailBoostStats ComputeFiniteMeanStd(const FrameBuffer& frame,
                                      int x0,
                                      int y0,
                                      int x1,
                                      int y1) {
  DetailBoostStats stats;
  if (frame.empty()) {
    return stats;
  }

  const int begin_x = std::clamp(std::min(x0, x1), 0, frame.desc().width);
  const int end_x = std::clamp(std::max(x0, x1), 0, frame.desc().width);
  const int begin_y = std::clamp(std::min(y0, y1), 0, frame.desc().height);
  const int end_y = std::clamp(std::max(y0, y1), 0, frame.desc().height);
  if (begin_x >= end_x || begin_y >= end_y) {
    return stats;
  }

  double sum = 0.0;
  double sum_sq = 0.0;
  std::size_t count = 0U;
  for (int y = begin_y; y < end_y; ++y) {
    for (int x = begin_x; x < end_x; ++x) {
      const float value = frame.at(x, y, 0);
      if (!std::isfinite(value)) {
        continue;
      }
      sum += static_cast<double>(value);
      sum_sq += static_cast<double>(value) * static_cast<double>(value);
      ++count;
    }
  }

  if (count == 0U) {
    return stats;
  }

  const double mean = sum / static_cast<double>(count);
  const double variance = std::max(0.0, sum_sq / static_cast<double>(count) - mean * mean);
  stats.mean = static_cast<float>(mean);
  stats.stddev = static_cast<float>(std::sqrt(variance));
  stats.valid = std::isfinite(stats.mean) && std::isfinite(stats.stddev);
  return stats;
}

float ComputeDetailBoostFeatherWeight(int x,
                                      int y,
                                      int core_x1,
                                      int core_y1,
                                      int core_x2,
                                      int core_y2,
                                      int pad_x1,
                                      int pad_y1,
                                      int pad_x2,
                                      int pad_y2) {
  float weight = 1.0F;

  const int left_margin = std::max(0, core_x1 - pad_x1);
  if (left_margin > 0) {
    weight *= std::clamp(static_cast<float>(x - pad_x1) / static_cast<float>(left_margin),
                         0.0F,
                         1.0F);
  }

  const int right_margin = std::max(0, pad_x2 - core_x2);
  if (right_margin > 0) {
    weight *= std::clamp(static_cast<float>((pad_x2 - 1) - x) / static_cast<float>(right_margin),
                         0.0F,
                         1.0F);
  }

  const int top_margin = std::max(0, core_y1 - pad_y1);
  if (top_margin > 0) {
    weight *= std::clamp(static_cast<float>(y - pad_y1) / static_cast<float>(top_margin),
                         0.0F,
                         1.0F);
  }

  const int bottom_margin = std::max(0, pad_y2 - core_y2);
  if (bottom_margin > 0) {
    weight *= std::clamp(static_cast<float>((pad_y2 - 1) - y) / static_cast<float>(bottom_margin),
                         0.0F,
                         1.0F);
  }

  return std::max(1.0e-3F, weight);
}

float ComputeFiniteBoxBlur(const FrameBuffer& frame, int x, int y, int radius) {
  if (frame.empty()) {
    return 0.0F;
  }

  const int begin_x = std::max(0, x - radius);
  const int end_x = std::min(frame.desc().width - 1, x + radius);
  const int begin_y = std::max(0, y - radius);
  const int end_y = std::min(frame.desc().height - 1, y + radius);

  double sum = 0.0;
  std::size_t count = 0U;
  for (int sample_y = begin_y; sample_y <= end_y; ++sample_y) {
    for (int sample_x = begin_x; sample_x <= end_x; ++sample_x) {
      const float value = frame.at(sample_x, sample_y, 0);
      if (!std::isfinite(value)) {
        continue;
      }
      sum += static_cast<double>(value);
      ++count;
    }
  }

  if (count == 0U) {
    const float fallback = frame.at(x, y, 0);
    return std::isfinite(fallback) ? fallback : 0.0F;
  }
  return static_cast<float>(sum / static_cast<double>(count));
}

FrameBuffer UpsampleDepthWithGuide(const FrameBuffer& lowres_depth,
                                   const FrameBuffer& lowres_luma,
                                   const FrameBuffer& fullres_luma,
                                   float guidance_sigma) {
  if (lowres_depth.empty() || lowres_luma.empty() || fullres_luma.empty()) {
    return lowres_depth;
  }

  FrameDesc output_desc = lowres_depth.desc();
  output_desc.width = fullres_luma.desc().width;
  output_desc.height = fullres_luma.desc().height;
  output_desc.channels = 1;
  output_desc.format = PixelFormat::kGray32F;
  FrameBuffer upsampled(output_desc);

  const int low_w = lowres_depth.desc().width;
  const int low_h = lowres_depth.desc().height;
  const int out_w = output_desc.width;
  const int out_h = output_desc.height;
  const float sigma = std::max(0.01F, guidance_sigma);
  const float inv_sigma = 1.0F / sigma;

  for (int y = 0; y < out_h; ++y) {
    const float src_y = (static_cast<float>(y) + 0.5F) *
                            (static_cast<float>(low_h) / static_cast<float>(std::max(1, out_h))) -
                        0.5F;
    const float clamped_y = std::clamp(src_y, 0.0F, static_cast<float>(std::max(0, low_h - 1)));
    const int y0 = static_cast<int>(clamped_y);
    const int y1 = std::min(y0 + 1, low_h - 1);
    const float ty = clamped_y - static_cast<float>(y0);
    for (int x = 0; x < out_w; ++x) {
      const float src_x = (static_cast<float>(x) + 0.5F) *
                              (static_cast<float>(low_w) /
                               static_cast<float>(std::max(1, out_w))) -
                          0.5F;
      const float clamped_x =
          std::clamp(src_x, 0.0F, static_cast<float>(std::max(0, low_w - 1)));
      const int x0 = static_cast<int>(clamped_x);
      const int x1 = std::min(x0 + 1, low_w - 1);
      const float tx = clamped_x - static_cast<float>(x0);

      const float full_luma = fullres_luma.at(x, y, 0);
      const float l00 = lowres_luma.at(x0, y0, 0);
      const float l01 = lowres_luma.at(x1, y0, 0);
      const float l10 = lowres_luma.at(x0, y1, 0);
      const float l11 = lowres_luma.at(x1, y1, 0);

      const float w00 = (1.0F - tx) * (1.0F - ty) * std::exp(-std::fabs(full_luma - l00) * inv_sigma);
      const float w01 = tx * (1.0F - ty) * std::exp(-std::fabs(full_luma - l01) * inv_sigma);
      const float w10 = (1.0F - tx) * ty * std::exp(-std::fabs(full_luma - l10) * inv_sigma);
      const float w11 = tx * ty * std::exp(-std::fabs(full_luma - l11) * inv_sigma);

      const float d00 = lowres_depth.at(x0, y0, 0);
      const float d01 = lowres_depth.at(x1, y0, 0);
      const float d10 = lowres_depth.at(x0, y1, 0);
      const float d11 = lowres_depth.at(x1, y1, 0);

      const float sum_w = w00 + w01 + w10 + w11;
      if (sum_w <= 1e-6F) {
        upsampled.at(x, y, 0) = (d00 + d01 + d10 + d11) * 0.25F;
      } else {
        upsampled.at(x, y, 0) = (w00 * d00 + w01 * d01 + w10 * d10 + w11 * d11) / sum_w;
      }
    }
  }

  return upsampled;
}

std::vector<int> BuildAdaptiveTileSizes(int requested_tile_size, const FrameDesc& source_desc) {
  std::vector<int> tile_sizes;
  if (!IsValid(source_desc)) {
    return tile_sizes;
  }

  const int max_extent = std::max(source_desc.width, source_desc.height);
  int tile_size = std::max(1, requested_tile_size);
  tile_size = std::min(tile_size, max_extent);
  tile_sizes.push_back(tile_size);

  while (static_cast<int>(tile_sizes.size()) < kMaxAdaptiveTileAttempts) {
    const int next_tile_size = tile_size / 2;
    if (next_tile_size < kMinAdaptiveRetryTileSize || next_tile_size == tile_size) {
      break;
    }
    tile_sizes.push_back(next_tile_size);
    tile_size = next_tile_size;
  }

  return tile_sizes;
}

std::string JoinAttemptDetails(const std::vector<std::string>& attempts) {
  std::string joined;
  for (std::size_t i = 0; i < attempts.size(); ++i) {
    if (i > 0) {
      joined += ", ";
    }
    joined += attempts[i];
  }
  return joined;
}

bool IsTemporalSequenceModelId(std::string_view model_id) {
  return model_id.rfind("video-depth-anything", 0) == 0;
}

int ComputeDownscaleDivisor(const FrameDesc& source_desc, int vram_budget_mb) {
  if (!IsValid(source_desc)) {
    return kMinDownscaleDivisor;
  }
  if (vram_budget_mb <= 0) {
    return kMinDownscaleDivisor;
  }

  const std::size_t width = static_cast<std::size_t>(source_desc.width);
  const std::size_t height = static_cast<std::size_t>(source_desc.height);
  if (width == 0 || height == 0) {
    return kMinDownscaleDivisor;
  }

  const std::size_t max_size = std::numeric_limits<std::size_t>::max();
  if (width > max_size / height) {
    return kMaxDownscaleDivisor;
  }
  const std::size_t pixel_count = width * height;
  if (pixel_count > max_size / kFallbackWorkingSetBytesPerPixelEstimate) {
    return kMaxDownscaleDivisor;
  }

  const std::size_t estimated_bytes = pixel_count * kFallbackWorkingSetBytesPerPixelEstimate;
  const std::size_t budget_bytes = static_cast<std::size_t>(vram_budget_mb) * 1024U * 1024U;
  if (budget_bytes == 0 || estimated_bytes <= budget_bytes) {
    return kMinDownscaleDivisor;
  }

  const double ratio = static_cast<double>(estimated_bytes) / static_cast<double>(budget_bytes);
  const int divisor = static_cast<int>(std::ceil(std::sqrt(ratio)));
  return std::clamp(divisor, kMinDownscaleDivisor, kMaxDownscaleDivisor);
}

struct PooledFrame {
  BufferPool* pool = nullptr;
  std::shared_ptr<FrameBuffer> buffer;

  ~PooledFrame() {
    if (pool != nullptr && buffer != nullptr) {
      pool->Release(buffer);
    }
  }

  FrameBuffer* get() const {
    return buffer.get();
  }
};

}  // namespace

RenderPipeline::RenderPipeline(std::shared_ptr<inference::IInferenceEngine> engine)
    : engine_(std::move(engine)) {}

std::shared_ptr<RenderPipelineState> RenderPipeline::CreateState() const {
  return std::make_shared<RenderPipelineState>();
}

RenderPipelineState* RenderPipeline::ResolveState(RenderPipelineState* state) const {
  return state != nullptr ? state : &shared_state_;
}

RenderOutput RenderPipeline::Render(const FrameBuffer& source,
                                    const RenderParams& params,
                                    RenderPipelineState* state) {
  try {
    RenderPipelineState* render_state = ResolveState(state);
    const std::string enter_detail =
        "model=" + params.model_id + ", q=" + std::to_string(params.quality) +
        ", src=" + std::to_string(source.desc().width) + "x" +
        std::to_string(source.desc().height);
    AppendPipelineTrace("render_enter", enter_detail.c_str());
    if (source.empty()) {
      AppendPipelineTrace("render_source_empty");
      return SafeOutput(source, "empty source frame");
    }

    if (!engine_) {
      AppendPipelineTrace("render_missing_engine");
      return SafeOutput(source, "missing inference engine");
    }

    if (!params.freeze_enabled) {
      ClearFrozenOutput(render_state);
    }

    AppendPipelineTrace("select_model_begin");
    std::string model_selection_error;
    if (!engine_->SelectModel(params.model_id, &model_selection_error)) {
      AppendPipelineTrace("select_model_failed",
                          model_selection_error.empty() ? "<none>" : model_selection_error.c_str());
      return SafeOutput(source, "model selection failed: " + model_selection_error);
    }
    AppendPipelineTrace("select_model_ok");

    const bool use_cache = ShouldUseCache(params);
    const RenderCacheKey key = use_cache ? BuildCacheKey(source, params) : RenderCacheKey{};
    if (use_cache) {
      FrameBuffer cached;
      if (cache_.Find(key, &cached)) {
        AppendPipelineTrace("cache_hit");
        return {RenderStatus::kCacheHit, cached, "cache hit (" + engine_->ActiveModelId() + ")"};
      }
    }

    if (params.freeze_enabled) {
      FrameBuffer frozen;
      if (TryGetFrozenOutput(source, params, render_state, &frozen)) {
        AppendPipelineTrace("freeze_hit");
        return {RenderStatus::kCacheHit, std::move(frozen), "frozen depth hit (" + engine_->ActiveModelId() +
                                                        ")"};
      }
    }

    FrameDesc depth_desc = source.desc();
    depth_desc.channels = 1;
    depth_desc.format = PixelFormat::kGray32F;
    PooledFrame depth;
    depth.pool = &pool_;
    depth.buffer = pool_.Acquire(depth_desc);

    auto finalize_output = [&](RenderStatus status, std::string message) -> RenderOutput {
      const auto postprocess_started = std::chrono::steady_clock::now();
      ApplyPostProcess(depth.get(), source, params, render_state);
      FrameBuffer output = BuildOutput(*depth.get(), source, params);
      const auto postprocess_finished = std::chrono::steady_clock::now();
      const double postprocess_ms =
          static_cast<double>(
              std::chrono::duration_cast<std::chrono::microseconds>(postprocess_finished -
                                                                    postprocess_started)
                  .count()) /
          1000.0;
      if (ParseBoolEnvOrDefault("ZSODA_PIPELINE_TRACE", false)) {
        std::ostringstream detail;
        detail.setf(std::ios::fixed);
        detail.precision(2);
        detail << "postprocess_ms=" << postprocess_ms;
        AppendPipelineTrace("postprocess_timing", detail.str().c_str());
      }
      const float outside_value =
          params.output_mode == OutputMode::kSlicing ? 0.0F : (params.invert ? 1.0F : 0.0F);
      ApplySourceAlphaMask(&output, source, outside_value);
      if (use_cache) {
        cache_.Insert(key, output);
      }
      if (params.freeze_enabled) {
        StoreFrozenOutput(source, params, render_state, output);
      }
      if (!model_selection_error.empty()) {
        message += " - " + model_selection_error;
      }
      return {status, std::move(output), std::move(message)};
    };
    std::string direct_error;
    AppendPipelineTrace("direct_inference_begin");
    if (RunInference(source, params, params.quality, depth.get(), &direct_error)) {
      AppendPipelineTrace("direct_inference_ok");
      std::string message = "direct inference succeeded (" + engine_->ActiveModelId() + ")";
      if (!direct_error.empty()) {
        message += " - backend note: " + direct_error;
      }
      return finalize_output(RenderStatus::kInference, std::move(message));
    }
    AppendPipelineTrace("direct_inference_failed",
                        direct_error.empty() ? "<none>" : direct_error.c_str());

    std::string tiled_error;
    std::vector<std::string> tiled_attempts;
    int successful_tile_size = 0;
    const bool skip_tiled_fallback = IsTemporalSequenceModelId(params.model_id);
    if (skip_tiled_fallback) {
      tiled_error = "tiled fallback skipped for temporal sequence model";
      AppendPipelineTrace("tiled_fallback_skipped_sequence_model", params.model_id.c_str());
    } else {
      const auto tiled_retry_sizes = BuildAdaptiveTileSizes(params.tile_size, source.desc());
      for (const int candidate_tile_size : tiled_retry_sizes) {
        std::string attempt_error;
        if (RunTiledInference(source,
                              params,
                              params.quality,
                              candidate_tile_size,
                              params.overlap,
                              depth.get(),
                              &attempt_error)) {
          successful_tile_size = candidate_tile_size;
          tiled_attempts.push_back("tile=" + std::to_string(candidate_tile_size) + " succeeded");
          break;
        }

        std::string attempt_detail = "tile=" + std::to_string(candidate_tile_size) + " failed";
        if (!attempt_error.empty()) {
          attempt_detail += " (" + attempt_error + ")";
        }
        tiled_attempts.push_back(std::move(attempt_detail));
      }
    }

    if (successful_tile_size > 0) {
      AppendPipelineTrace("tiled_fallback_ok");
      std::string message = "tiled fallback succeeded after direct failure (" + engine_->ActiveModelId() +
                            ", tile=" + std::to_string(successful_tile_size) + ")";
      if (!direct_error.empty()) {
        message += " - direct inference failed: " + direct_error;
      }
      if (!tiled_attempts.empty()) {
        message += " - tiled attempts: " + JoinAttemptDetails(tiled_attempts);
      }
      return finalize_output(RenderStatus::kFallbackTiled, message);
    }
    if (!tiled_attempts.empty()) {
      tiled_error = JoinAttemptDetails(tiled_attempts);
    } else if (tiled_error.empty()) {
      tiled_error = "no tiled attempts generated";
    }

    std::string downscaled_error;
    if (RunDownscaledInference(source, params, depth.get(), &downscaled_error)) {
      AppendPipelineTrace("downscaled_fallback_ok");
      std::string message =
          "downscaled fallback succeeded after direct/tiled failures (" + engine_->ActiveModelId() +
          ")";
      if (!direct_error.empty()) {
        message += " - direct inference failed: " + direct_error;
      }
      if (!tiled_error.empty()) {
        message += " - tiled inference failed: " + tiled_error;
      }
      return finalize_output(RenderStatus::kFallbackDownscaled, message);
    }

    std::string message = "all inference stages failed";
    AppendPipelineTrace("all_stages_failed");
    if (!direct_error.empty()) {
      message += " - direct inference failed: " + direct_error;
    }
    if (!tiled_error.empty()) {
      message += " - tiled inference failed: " + tiled_error;
    }
    if (!downscaled_error.empty()) {
      message += " - downscaled inference failed: " + downscaled_error;
    }
    message += " - returning safe output";
    if (!model_selection_error.empty()) {
      message += " - " + model_selection_error;
    }
    return SafeOutput(source, message);
  } catch (const std::exception& ex) {
    AppendPipelineTrace("render_exception", SafeCStr(ex.what()));
    return SafeOutput(source, std::string("render exception: ") + SafeCStr(ex.what()));
  } catch (...) {
    AppendPipelineTrace("render_exception_unknown");
    return SafeOutput(source, "render exception: unknown");
  }
}

void RenderPipeline::SetCacheLimit(std::size_t limit) {
  cache_.SetLimit(limit);
}

void RenderPipeline::PurgeCache() {
  cache_.Clear();
  ClearFrozenOutput(&shared_state_);
}

bool RenderPipeline::ShouldUseCache(const RenderParams& params) const {
  if (!params.cache_enabled) {
    return false;
  }
  if (params.freeze_enabled) {
    return false;
  }
  if (params.frame_hash == 0) {
    return false;
  }
  if (IsStatefulPostProcess(params)) {
    return false;
  }
  return true;
}

bool RenderPipeline::IsStatefulPostProcess(const RenderParams& params) {
  const bool temporal_active = std::clamp(params.temporal_alpha, 0.0F, 1.0F) < 0.999F;
  const bool guided_active = params.mapping_mode == DepthMappingMode::kGuided;
  return temporal_active || guided_active;
}

std::uint64_t RenderPipeline::BuildPostprocessStateHash(const RenderParams& params) const {
  const auto to_permille = [](float value) -> std::uint64_t {
    const int clipped = static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 1000.0F));
    return static_cast<std::uint64_t>(std::max(0, clipped));
  };
  const auto mix = [](std::uint64_t lhs, std::uint64_t rhs) -> std::uint64_t {
    return lhs ^ (rhs + 0x9e3779b97f4a7c15ULL + (lhs << 6ULL) + (lhs >> 2ULL));
  };

  std::uint64_t h = static_cast<std::uint64_t>(static_cast<int>(params.mapping_mode));
  h = mix(h, static_cast<std::uint64_t>(params.invert ? 1 : 0));
  h = mix(h, to_permille(params.guided_low_percentile));
  h = mix(h, to_permille(params.guided_high_percentile));
  h = mix(h, to_permille(params.guided_update_alpha));
  h = mix(h, to_permille(params.temporal_alpha));
  h = mix(h, static_cast<std::uint64_t>(params.temporal_edge_aware ? 1 : 0));
  h = mix(h, to_permille(params.temporal_edge_threshold));
  h = mix(h, to_permille(params.temporal_scene_cut_threshold));
  h = mix(h, to_permille(params.edge_enhancement));
  h = mix(h, to_permille(params.edge_guidance_sigma));
  h = mix(h, static_cast<std::uint64_t>(params.edge_aware_upsample ? 1 : 0));
  h = mix(h, static_cast<std::uint64_t>(params.freeze_enabled ? 1 : 0));
  h = mix(h, static_cast<std::uint64_t>(std::max(0, params.extract_token)));
  h = mix(h, params.render_state_token);
  return h;
}

RenderCacheKey RenderPipeline::BuildCacheKey(const FrameBuffer& source, const RenderParams& params) const {
  const auto to_permille = [](float value) -> int {
    return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 1000.0F));
  };

  RenderCacheKey key;
  key.width = source.desc().width;
  key.height = source.desc().height;
  key.quality = params.quality;
  key.preserve_aspect_ratio = params.preserve_aspect_ratio;
  key.invert = params.invert;
  key.mapping_mode = static_cast<int>(params.mapping_mode);
  key.guided_low_permille = to_permille(params.guided_low_percentile);
  key.guided_high_permille = to_permille(params.guided_high_percentile);
  key.guided_alpha_permille = to_permille(params.guided_update_alpha);
  key.temporal_alpha_permille = to_permille(params.temporal_alpha);
  key.temporal_edge_aware = params.temporal_edge_aware;
  key.temporal_edge_threshold_permille = to_permille(params.temporal_edge_threshold);
  key.temporal_scene_cut_threshold_permille = to_permille(params.temporal_scene_cut_threshold);
  key.edge_enhancement_permille = to_permille(params.edge_enhancement);
  key.edge_guidance_sigma_permille = to_permille(params.edge_guidance_sigma);
  key.edge_aware_upsample = params.edge_aware_upsample;
  key.slice_mode = params.output_mode == OutputMode::kSlicing;
  key.slice_normalize = params.slice_normalize;
  key.slice_absolute_depth =
      static_cast<int>(std::lround(std::clamp(params.slice_absolute_depth, 0.0F, 1000.0F)));
  key.slice_min_permille = to_permille(params.min_depth);
  key.slice_max_permille = to_permille(params.max_depth);
  key.slice_softness_permille = to_permille(params.softness);
  key.tile_size = std::max(1, params.tile_size);
  key.overlap = std::max(0, std::min(params.overlap, key.tile_size / 2));
  key.vram_budget_mb = std::max(0, params.vram_budget_mb);
  key.extract_token = std::max(0, params.extract_token);
  key.model_hash = static_cast<std::uint64_t>(std::hash<std::string>{}(params.model_id));
  key.frame_hash = params.frame_hash;
  key.render_state_token = params.render_state_token;
  return key;
}

bool RenderPipeline::RunInference(const FrameBuffer& source,
                                  const RenderParams& params,
                                  int quality,
                                  FrameBuffer* depth,
                                  std::string* error) const {
  if (!engine_) {
    if (error) {
      *error = "missing inference engine";
    }
    return false;
  }
  inference::InferenceRequest request;
  request.source = &source;
  request.quality = quality;
  request.resize_mode = params.preserve_aspect_ratio
                            ? inference::PreprocessResizeMode::kUpperBoundLetterbox
                            : inference::PreprocessResizeMode::kStretch;
  request.frame_hash = params.frame_hash;
  return engine_->Run(request, depth, error);
}

bool RenderPipeline::RunTiledInference(const FrameBuffer& source,
                                       const RenderParams& params,
                                       int quality,
                                       int tile_size,
                                       int overlap,
                                       FrameBuffer* depth,
                                       std::string* error) const {
  if (depth == nullptr) {
    if (error) {
      *error = "depth output is null";
    }
    return false;
  }

  std::vector<TileResult> tile_results;
  const auto tiles = BuildTiles(source.desc().width, source.desc().height, tile_size, overlap);
  if (tiles.empty()) {
    if (error != nullptr) {
      *error = "no tiles generated";
    }
    return false;
  }

  for (const auto& tile : tiles) {
    const FrameBuffer tile_src = CropGray(source, tile);
    FrameDesc tile_depth_desc = tile_src.desc();
    tile_depth_desc.channels = 1;
    tile_depth_desc.format = PixelFormat::kGray32F;
    FrameBuffer tile_depth(tile_depth_desc);

    inference::InferenceRequest request;
    request.source = &tile_src;
    request.quality = quality;
    request.resize_mode = params.preserve_aspect_ratio
                              ? inference::PreprocessResizeMode::kUpperBoundLetterbox
                              : inference::PreprocessResizeMode::kStretch;
    request.frame_hash = params.frame_hash;
    if (!engine_->Run(request, &tile_depth, error)) {
      return false;
    }

    tile_results.push_back({tile, std::move(tile_depth)});
  }

  *depth = ComposeTiles(depth->desc(), tile_results);
  return true;
}

bool RenderPipeline::RunDownscaledInference(const FrameBuffer& source,
                                            const RenderParams& params,
                                            FrameBuffer* depth,
                                            std::string* error) const {
  if (depth == nullptr) {
    if (error != nullptr) {
      *error = "depth output is null";
    }
    return false;
  }
  if (source.empty()) {
    if (error != nullptr) {
      *error = "source frame is empty";
    }
    return false;
  }

  const int downscale_divisor = ComputeDownscaleDivisor(source.desc(), params.vram_budget_mb);
  const int downscaled_width = std::max(1, source.desc().width / downscale_divisor);
  const int downscaled_height = std::max(1, source.desc().height / downscale_divisor);
  if (downscaled_width == source.desc().width && downscaled_height == source.desc().height) {
    if (error != nullptr) {
      *error = "source frame is too small for downscaled fallback";
    }
    return false;
  }

  const FrameBuffer downscaled_source = ResizeBilinear(source, downscaled_width, downscaled_height);
  FrameDesc downscaled_depth_desc = downscaled_source.desc();
  downscaled_depth_desc.channels = 1;
  downscaled_depth_desc.format = PixelFormat::kGray32F;
  FrameBuffer downscaled_depth(downscaled_depth_desc);

  std::string downscaled_run_error;
  const int quality_penalty = std::max(1, downscale_divisor / 2);
  const int downscaled_quality = std::max(1, params.quality - quality_penalty);
  if (!RunInference(downscaled_source, params, downscaled_quality, &downscaled_depth, &downscaled_run_error)) {
    if (error != nullptr) {
      *error = downscaled_run_error.empty() ? "downscaled run failed" : downscaled_run_error;
    }
    return false;
  }

  if (params.edge_aware_upsample) {
    const FrameBuffer source_luma = BuildSourceLuma(source);
    const FrameBuffer downscaled_luma = ResizeBilinear(source_luma, downscaled_width, downscaled_height);
    *depth = UpsampleDepthWithGuide(downscaled_depth,
                                    downscaled_luma,
                                    source_luma,
                                    std::max(0.01F,
                                             params.edge_guidance_sigma > 0.0F
                                                 ? params.edge_guidance_sigma
                                                 : kEdgeAwareUpsampleGuidanceSigma));
  } else {
    *depth = ResizeBilinear(downscaled_depth, source.desc().width, source.desc().height);
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool RenderPipeline::TryGetFrozenOutput(const FrameBuffer& source,
                                        const RenderParams& params,
                                        RenderPipelineState* state,
                                        FrameBuffer* output) const {
  if (output == nullptr) {
    return false;
  }
  RenderPipelineState* render_state = ResolveState(state);
  const std::uint64_t requested_hash = BuildFrozenStateHash(source, params);
  CompatLockGuard lock(render_state->frozen_mutex_);
  if (!render_state->frozen_has_output_ || render_state->frozen_output_.empty() ||
      requested_hash != render_state->frozen_state_hash_) {
    return false;
  }
  *output = render_state->frozen_output_;
  return true;
}

void RenderPipeline::StoreFrozenOutput(const FrameBuffer& source,
                                       const RenderParams& params,
                                       RenderPipelineState* state,
                                       const FrameBuffer& output) const {
  if (output.empty()) {
    return;
  }
  RenderPipelineState* render_state = ResolveState(state);
  const std::uint64_t state_hash = BuildFrozenStateHash(source, params);
  CompatLockGuard lock(render_state->frozen_mutex_);
  render_state->frozen_output_ = output;
  render_state->frozen_state_hash_ = state_hash;
  render_state->frozen_has_output_ = true;
}

void RenderPipeline::ClearFrozenOutput(RenderPipelineState* state) const {
  RenderPipelineState* render_state = ResolveState(state);
  CompatLockGuard lock(render_state->frozen_mutex_);
  if (!render_state->frozen_has_output_ && render_state->frozen_output_.empty()) {
    return;
  }
  render_state->frozen_output_ = FrameBuffer();
  render_state->frozen_state_hash_ = 0;
  render_state->frozen_has_output_ = false;
}

std::uint64_t RenderPipeline::BuildFrozenStateHash(const FrameBuffer& source,
                                                   const RenderParams& params) const {
  std::uint64_t hash = HashFromInt(source.desc().width);
  hash = HashCombine64(hash, HashFromInt(source.desc().height));
  hash = HashCombine64(hash, HashFromInt(source.desc().channels));
  hash = HashCombine64(hash, HashFromInt(static_cast<int>(source.desc().format)));

  hash = HashCombine64(hash, HashFromInt(params.quality));
  hash = HashCombine64(hash, HashFromBool(params.preserve_aspect_ratio));
  hash = HashCombine64(hash, HashFromBool(params.invert));
  hash = HashCombine64(hash, HashFromInt(static_cast<int>(params.mapping_mode)));
  hash = HashCombine64(hash, HashFromPermille(params.guided_low_percentile));
  hash = HashCombine64(hash, HashFromPermille(params.guided_high_percentile));
  hash = HashCombine64(hash, HashFromPermille(params.guided_update_alpha));
  hash = HashCombine64(hash, HashFromPermille(params.temporal_alpha));
  hash = HashCombine64(hash, HashFromBool(params.temporal_edge_aware));
  hash = HashCombine64(hash, HashFromPermille(params.temporal_edge_threshold));
  hash = HashCombine64(hash, HashFromPermille(params.temporal_scene_cut_threshold));
  hash = HashCombine64(hash, HashFromPermille(params.edge_enhancement));
  hash = HashCombine64(hash, HashFromPermille(params.edge_guidance_sigma));
  hash = HashCombine64(hash, HashFromBool(params.edge_aware_upsample));
  hash = HashCombine64(hash, HashFromInt(static_cast<int>(params.output_mode)));
  hash = HashCombine64(hash, HashFromBool(params.slice_normalize));
  hash = HashCombine64(hash, HashFromInt(static_cast<int>(
                                   std::lround(std::clamp(params.slice_absolute_depth, 0.0F, 1000.0F)))));
  hash = HashCombine64(hash, HashFromPermille(params.min_depth));
  hash = HashCombine64(hash, HashFromPermille(params.max_depth));
  hash = HashCombine64(hash, HashFromPermille(params.softness));
  hash = HashCombine64(hash, HashFromInt(std::max(1, params.tile_size)));
  hash = HashCombine64(
      hash,
      HashFromInt(std::max(0, std::min(params.overlap, std::max(1, params.tile_size) / 2))));
  hash = HashCombine64(hash, HashFromInt(std::max(0, params.vram_budget_mb)));
  hash = HashCombine64(hash, HashFromInt(std::max(0, params.extract_token)));
  hash = HashCombine64(
      hash,
      static_cast<std::uint64_t>(std::hash<std::string>{}(params.model_id)));
  hash = HashCombine64(hash, params.render_state_token);
  return hash;
}

void RenderPipeline::ApplyPostProcess(FrameBuffer* depth,
                                      const FrameBuffer& source,
                                      const RenderParams& params,
                                      RenderPipelineState* state) const {
  if (depth == nullptr || depth->empty()) {
    return;
  }
  RenderPipelineState* render_state = ResolveState(state);

  const FrameBuffer source_luma = BuildSourceLuma(source);
  const std::uint64_t model_hash = static_cast<std::uint64_t>(std::hash<std::string>{}(params.model_id));
  const std::uint64_t state_hash = BuildPostprocessStateHash(params);
  const auto& depth_desc = depth->desc();

  CompatLockGuard lock(render_state->postprocess_mutex_);
  if (!render_state->postprocess_initialized_ ||
      model_hash != render_state->postprocess_model_hash_ ||
      state_hash != render_state->postprocess_state_hash_ ||
      depth_desc.width != render_state->temporal_depth_.desc().width ||
      depth_desc.height != render_state->temporal_depth_.desc().height) {
    render_state->guided_mapping_state_ = {};
    render_state->temporal_depth_ = FrameBuffer(depth_desc);
    render_state->temporal_source_luma_ = FrameBuffer(source_luma.desc());
    render_state->postprocess_model_hash_ = model_hash;
    render_state->postprocess_state_hash_ = state_hash;
    render_state->postprocess_initialized_ = true;
    render_state->temporal_has_state_ = false;
  }

  DepthMappingParams mapping_params;
  mapping_params.mode = params.mapping_mode;
  mapping_params.invert = params.invert;
  mapping_params.guided_low_percentile = params.guided_low_percentile;
  mapping_params.guided_high_percentile = params.guided_high_percentile;
  mapping_params.guided_update_alpha = params.guided_update_alpha;
  ApplyDepthMapping(depth, mapping_params, &render_state->guided_mapping_state_);
  ApplyTemporalSmoothing(depth, source_luma, params, render_state);
  ApplyEdgeAwareEnhancement(depth, source_luma, params);
}

void RenderPipeline::ApplyTemporalSmoothing(FrameBuffer* depth,
                                            const FrameBuffer& source_luma,
                                            const RenderParams& params,
                                            RenderPipelineState* state) const {
  if (depth == nullptr || depth->empty() || source_luma.empty()) {
    return;
  }
  RenderPipelineState* render_state = ResolveState(state);

  const float alpha = std::clamp(params.temporal_alpha, 0.0F, 1.0F);
  if (!render_state->temporal_depth_.empty() && !render_state->temporal_source_luma_.empty()) {
    const bool size_mismatch =
        render_state->temporal_depth_.desc().width != depth->desc().width ||
        render_state->temporal_depth_.desc().height != depth->desc().height ||
        render_state->temporal_source_luma_.desc().width != source_luma.desc().width ||
        render_state->temporal_source_luma_.desc().height != source_luma.desc().height;
    if (size_mismatch) {
      render_state->temporal_depth_.Resize(depth->desc());
      render_state->temporal_source_luma_.Resize(source_luma.desc());
      render_state->temporal_has_state_ = false;
    }
  }

  if (!render_state->temporal_has_state_) {
    render_state->temporal_depth_ = *depth;
    render_state->temporal_source_luma_ = source_luma;
    render_state->temporal_has_state_ = true;
    return;
  }

  const float scene_cut_threshold = std::clamp(params.temporal_scene_cut_threshold, 0.0F, 1.0F);
  const float mean_abs_diff =
      ComputeMeanAbsoluteDifference(source_luma, render_state->temporal_source_luma_);
  if (mean_abs_diff >= scene_cut_threshold) {
    render_state->temporal_depth_ = *depth;
    render_state->temporal_source_luma_ = source_luma;
    return;
  }

  if (alpha < 1.0F) {
    const auto& desc = depth->desc();
    const float edge_threshold =
        std::max(1e-4F, std::clamp(params.temporal_edge_threshold, 0.0F, 1.0F));
    const float stability_threshold =
        std::max(0.03F, std::min(scene_cut_threshold * 0.9F, 0.18F));
    const FrameBuffer current_low = BuildTemporalLowFrequency(*depth);
    const FrameBuffer history_low = BuildTemporalLowFrequency(render_state->temporal_depth_);
    const FrameBuffer aligned_history_low = AlignHistoryLowFrequency(current_low,
                                                                     history_low,
                                                                     source_luma,
                                                                     render_state->temporal_source_luma_,
                                                                     stability_threshold,
                                                                     edge_threshold);
    for (int y = 0; y < desc.height; ++y) {
      for (int x = 0; x < desc.width; ++x) {
        float local_alpha = alpha;
        if (params.temporal_edge_aware) {
          const float edge_strength =
              std::clamp(ComputeLocalGradientMagnitude(source_luma, x, y) / edge_threshold,
                         0.0F,
                         1.0F);
          const float luma_delta = std::fabs(
              source_luma.at(x, y, 0) - render_state->temporal_source_luma_.at(x, y, 0));
          const float motion_strength =
              std::clamp(luma_delta / stability_threshold, 0.0F, 1.0F);
          local_alpha = alpha + (1.0F - alpha) * std::max(edge_strength, motion_strength);
        }

        const float blended_low = aligned_history_low.at(x, y, 0) +
                                  (current_low.at(x, y, 0) - aligned_history_low.at(x, y, 0)) *
                                      local_alpha;
        const float detail = depth->at(x, y, 0) - current_low.at(x, y, 0);
        depth->at(x, y, 0) = std::clamp(blended_low + detail, 0.0F, 1.0F);
      }
    }
  }

  render_state->temporal_depth_ = *depth;
  render_state->temporal_source_luma_ = source_luma;
}

void RenderPipeline::ApplyEdgeAwareEnhancement(FrameBuffer* depth,
                                               const FrameBuffer& source_luma,
                                               const RenderParams& params) const {
  if (depth == nullptr || depth->empty() || source_luma.empty()) {
    return;
  }

  const float strength = std::clamp(params.edge_enhancement, 0.0F, 1.0F);
  if (strength <= 1e-4F) {
    return;
  }

  const float sigma = std::max(0.01F, std::clamp(params.edge_guidance_sigma, 0.0F, 1.0F));
  const float inv_sigma = 1.0F / sigma;
  FrameBuffer base = *depth;
  const auto& desc = depth->desc();

  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float guide_center = source_luma.at(x, y, 0);
      float sum_weights = 0.0F;
      float sum_depth = 0.0F;
      for (int ky = -1; ky <= 1; ++ky) {
        const int sy = std::clamp(y + ky, 0, desc.height - 1);
        for (int kx = -1; kx <= 1; ++kx) {
          const int sx = std::clamp(x + kx, 0, desc.width - 1);
          const float spatial_weight = (kx == 0 && ky == 0) ? 1.0F : 0.7071F;
          const float guide_neighbor = source_luma.at(sx, sy, 0);
          const float range_weight =
              std::exp(-std::fabs(guide_center - guide_neighbor) * inv_sigma);
          const float weight = spatial_weight * range_weight;
          sum_weights += weight;
          sum_depth += weight * base.at(sx, sy, 0);
        }
      }

      const float blurred = (sum_weights > 1e-6F) ? (sum_depth / sum_weights) : base.at(x, y, 0);
      const float enhanced = base.at(x, y, 0) + strength * (base.at(x, y, 0) - blurred);
      depth->at(x, y, 0) = std::clamp(enhanced, 0.0F, 1.0F);
    }
  }
}

FrameBuffer RenderPipeline::BuildOutput(const FrameBuffer& normalized_depth,
                                        const FrameBuffer& source,
                                        const RenderParams& params) const {
  if (params.output_mode == OutputMode::kSlicing) {
    const FrameBuffer slice_depth = BuildSliceDepthInput(normalized_depth, params);
    const SliceWindow slice_window = ResolveSliceWindow(params);
    const FrameBuffer slice_matte =
        BuildSliceMatte(slice_depth, slice_window.min_depth, slice_window.max_depth, params.softness);
    return ApplySliceMatteToSource(source, slice_matte);
  }
  return normalized_depth;
}

RenderOutput RenderPipeline::SafeOutput(const FrameBuffer& source, const std::string& message) const {
  FrameDesc desc = source.desc();
  if (!IsValid(desc)) {
    desc.width = 1;
    desc.height = 1;
  }
  desc.channels = 1;
  desc.format = PixelFormat::kGray32F;
  FrameBuffer empty(desc);
  empty.Fill(0.0F);
  return {RenderStatus::kSafeOutput, empty, message};
}

}  // namespace zsoda::core
