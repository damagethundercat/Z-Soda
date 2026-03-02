#include "core/BufferPool.h"

#include <algorithm>

namespace zsoda::core {

BufferPool::BufferPool(std::size_t max_buffers) : max_buffers_(std::max<std::size_t>(1, max_buffers)) {}

std::shared_ptr<FrameBuffer> BufferPool::Acquire(const FrameDesc& desc) {
  std::scoped_lock lock(mutex_);

  for (auto it = free_list_.begin(); it != free_list_.end(); ++it) {
    const auto candidate = *it;
    if (candidate->desc().width == desc.width && candidate->desc().height == desc.height &&
        candidate->desc().channels == desc.channels) {
      free_list_.erase(it);
      candidate->Fill(0.0F);
      return candidate;
    }
  }

  auto fresh = std::make_shared<FrameBuffer>(desc);
  return fresh;
}

void BufferPool::Release(std::shared_ptr<FrameBuffer> buffer) {
  if (!buffer) {
    return;
  }
  std::scoped_lock lock(mutex_);
  if (free_list_.size() >= max_buffers_) {
    return;
  }
  free_list_.push_back(std::move(buffer));
}

}  // namespace zsoda::core
