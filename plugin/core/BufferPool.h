#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "core/Frame.h"

namespace zsoda::core {

class BufferPool {
 public:
  explicit BufferPool(std::size_t max_buffers);
  std::shared_ptr<FrameBuffer> Acquire(const FrameDesc& desc);
  void Release(std::shared_ptr<FrameBuffer> buffer);

 private:
  std::size_t max_buffers_ = 8;
  std::mutex mutex_;
  std::vector<std::shared_ptr<FrameBuffer>> free_list_;
};

}  // namespace zsoda::core
