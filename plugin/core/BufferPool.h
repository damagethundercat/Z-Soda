#pragma once

#include <memory>
#include <vector>

#include "core/CompatMutex.h"
#include "core/Frame.h"

namespace zsoda::core {

class BufferPool {
 public:
  explicit BufferPool(std::size_t max_buffers);
  std::shared_ptr<FrameBuffer> Acquire(const FrameDesc& desc);
  void Release(std::shared_ptr<FrameBuffer> buffer);

 private:
  std::size_t max_buffers_ = 8;
  CompatMutex mutex_;
  std::vector<std::shared_ptr<FrameBuffer>> free_list_;
};

}  // namespace zsoda::core
