#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

#include "core/CompatMutex.h"
#include "core/Frame.h"

namespace zsoda::core {

struct RenderCacheKey {
  int width = 0;
  int height = 0;
  int quality = 0;
  int quality_boost = 0;
  bool preserve_aspect_ratio = true;
  bool invert = false;
  int mapping_mode = 0;
  int guided_low_permille = 50;
  int guided_high_permille = 950;
  int guided_alpha_permille = 150;
  int temporal_alpha_permille = 1000;
  bool temporal_edge_aware = false;
  int temporal_edge_threshold_permille = 80;
  int temporal_scene_cut_threshold_permille = 180;
  int edge_enhancement_permille = 0;
  int edge_guidance_sigma_permille = 120;
  bool edge_aware_upsample = true;
  bool slice_mode = false;
  int slice_min_permille = 250;
  int slice_max_permille = 750;
  int slice_softness_permille = 100;
  int tile_size = 512;
  int overlap = 32;
  int vram_budget_mb = 0;
  int extract_token = 0;
  std::uint64_t model_hash = 0;
  std::uint64_t frame_hash = 0;

  bool operator==(const RenderCacheKey& other) const {
    return width == other.width && height == other.height && quality == other.quality &&
           quality_boost == other.quality_boost &&
           preserve_aspect_ratio == other.preserve_aspect_ratio &&
           invert == other.invert && mapping_mode == other.mapping_mode &&
           guided_low_permille == other.guided_low_permille &&
           guided_high_permille == other.guided_high_permille &&
           guided_alpha_permille == other.guided_alpha_permille &&
           temporal_alpha_permille == other.temporal_alpha_permille &&
           temporal_edge_aware == other.temporal_edge_aware &&
           temporal_edge_threshold_permille == other.temporal_edge_threshold_permille &&
           temporal_scene_cut_threshold_permille == other.temporal_scene_cut_threshold_permille &&
           edge_enhancement_permille == other.edge_enhancement_permille &&
           edge_guidance_sigma_permille == other.edge_guidance_sigma_permille &&
           edge_aware_upsample == other.edge_aware_upsample &&
           slice_mode == other.slice_mode &&
           slice_min_permille == other.slice_min_permille &&
           slice_max_permille == other.slice_max_permille &&
           slice_softness_permille == other.slice_softness_permille &&
           tile_size == other.tile_size && overlap == other.overlap &&
           vram_budget_mb == other.vram_budget_mb && extract_token == other.extract_token &&
           model_hash == other.model_hash &&
           frame_hash == other.frame_hash;
  }
};

struct RenderCacheKeyHash {
  std::size_t operator()(const RenderCacheKey& key) const;
};

class DepthCache {
 public:
  explicit DepthCache(std::size_t max_entries);

  bool Find(const RenderCacheKey& key, FrameBuffer* out) const;
  void Insert(const RenderCacheKey& key, const FrameBuffer& value);
  void Clear();
  void SetLimit(std::size_t max_entries);
  [[nodiscard]] std::size_t size() const;

 private:
  void Touch(const RenderCacheKey& key) const;
  void TrimIfNeeded();

  std::size_t max_entries_ = 32;
  mutable CompatMutex mutex_;
  std::unordered_map<RenderCacheKey, FrameBuffer, RenderCacheKeyHash> entries_;
  mutable std::deque<RenderCacheKey> lru_;
};

}  // namespace zsoda::core
