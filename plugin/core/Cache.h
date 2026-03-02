#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "core/Frame.h"

namespace zsoda::core {

struct RenderCacheKey {
  int width = 0;
  int height = 0;
  int quality = 0;
  bool invert = false;
  bool slice_mode = false;
  int vram_budget_mb = 0;
  std::uint64_t model_hash = 0;
  std::uint64_t frame_hash = 0;

  bool operator==(const RenderCacheKey& other) const {
    return width == other.width && height == other.height && quality == other.quality &&
           invert == other.invert && slice_mode == other.slice_mode &&
           vram_budget_mb == other.vram_budget_mb && model_hash == other.model_hash &&
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
  void Touch(const RenderCacheKey& key);
  void TrimIfNeeded();

  std::size_t max_entries_ = 32;
  mutable std::mutex mutex_;
  std::unordered_map<RenderCacheKey, FrameBuffer, RenderCacheKeyHash> entries_;
  std::deque<RenderCacheKey> lru_;
};

}  // namespace zsoda::core
