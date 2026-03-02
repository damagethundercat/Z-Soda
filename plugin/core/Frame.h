#pragma once

#include <cassert>
#include <cstddef>
#include <algorithm>
#include <vector>

namespace zsoda::core {

enum class PixelFormat {
  kRGBA8,
  kRGBA16,
  kRGBA32F,
  kGray32F,
};

struct FrameDesc {
  int width = 0;
  int height = 0;
  int channels = 1;
  PixelFormat format = PixelFormat::kGray32F;
};

inline bool IsValid(const FrameDesc& desc) {
  return desc.width > 0 && desc.height > 0 && desc.channels > 0;
}

inline std::size_t ElementCount(const FrameDesc& desc) {
  return static_cast<std::size_t>(desc.width) * static_cast<std::size_t>(desc.height) *
         static_cast<std::size_t>(desc.channels);
}

class FrameBuffer {
 public:
  FrameBuffer() = default;
  explicit FrameBuffer(FrameDesc desc) { Resize(desc); }

  void Resize(FrameDesc desc) {
    assert(IsValid(desc));
    desc_ = desc;
    pixels_.assign(ElementCount(desc_), 0.0F);
  }

  [[nodiscard]] const FrameDesc& desc() const { return desc_; }
  [[nodiscard]] bool empty() const { return pixels_.empty(); }
  [[nodiscard]] std::size_t size() const { return pixels_.size(); }

  [[nodiscard]] float* data() { return pixels_.data(); }
  [[nodiscard]] const float* data() const { return pixels_.data(); }

  [[nodiscard]] float& at(int x, int y, int channel = 0) {
    return pixels_[Offset(x, y, channel)];
  }

  [[nodiscard]] const float& at(int x, int y, int channel = 0) const {
    return pixels_[Offset(x, y, channel)];
  }

  void Fill(float value) { std::fill(pixels_.begin(), pixels_.end(), value); }

 private:
  [[nodiscard]] std::size_t Offset(int x, int y, int channel) const {
    assert(x >= 0 && x < desc_.width);
    assert(y >= 0 && y < desc_.height);
    assert(channel >= 0 && channel < desc_.channels);
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(desc_.width) +
            static_cast<std::size_t>(x)) *
               static_cast<std::size_t>(desc_.channels) +
           static_cast<std::size_t>(channel);
  }

  FrameDesc desc_{};
  std::vector<float> pixels_;
};

}  // namespace zsoda::core
