#include "core/Cache.h"

#include <algorithm>
#include <functional>

namespace zsoda::core {
namespace {

inline std::size_t HashCombine(std::size_t lhs, std::size_t rhs) {
  return lhs ^ (rhs + 0x9e3779b9 + (lhs << 6U) + (lhs >> 2U));
}

}  // namespace

std::size_t RenderCacheKeyHash::operator()(const RenderCacheKey& key) const {
  std::size_t h = std::hash<int>{}(key.width);
  h = HashCombine(h, std::hash<int>{}(key.height));
  h = HashCombine(h, std::hash<int>{}(key.quality));
  h = HashCombine(h, std::hash<bool>{}(key.invert));
  h = HashCombine(h, std::hash<bool>{}(key.slice_mode));
  h = HashCombine(h, std::hash<int>{}(key.vram_budget_mb));
  h = HashCombine(h, std::hash<std::uint64_t>{}(key.model_hash));
  h = HashCombine(h, std::hash<std::uint64_t>{}(key.frame_hash));
  return h;
}

DepthCache::DepthCache(std::size_t max_entries) : max_entries_(std::max<std::size_t>(1, max_entries)) {}

bool DepthCache::Find(const RenderCacheKey& key, FrameBuffer* out) const {
  if (out == nullptr) {
    return false;
  }
  std::scoped_lock lock(mutex_);
  const auto it = entries_.find(key);
  if (it == entries_.end()) {
    return false;
  }
  *out = it->second;
  return true;
}

void DepthCache::Insert(const RenderCacheKey& key, const FrameBuffer& value) {
  std::scoped_lock lock(mutex_);
  entries_[key] = value;
  Touch(key);
  TrimIfNeeded();
}

void DepthCache::Clear() {
  std::scoped_lock lock(mutex_);
  entries_.clear();
  lru_.clear();
}

void DepthCache::SetLimit(std::size_t max_entries) {
  std::scoped_lock lock(mutex_);
  max_entries_ = std::max<std::size_t>(1, max_entries);
  TrimIfNeeded();
}

std::size_t DepthCache::size() const {
  std::scoped_lock lock(mutex_);
  return entries_.size();
}

void DepthCache::Touch(const RenderCacheKey& key) {
  auto it = std::find(lru_.begin(), lru_.end(), key);
  if (it != lru_.end()) {
    lru_.erase(it);
  }
  lru_.push_back(key);
}

void DepthCache::TrimIfNeeded() {
  while (entries_.size() > max_entries_ && !lru_.empty()) {
    const RenderCacheKey old_key = lru_.front();
    lru_.pop_front();
    entries_.erase(old_key);
  }
}

}  // namespace zsoda::core
