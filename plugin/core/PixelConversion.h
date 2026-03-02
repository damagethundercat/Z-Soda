#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "core/Frame.h"

namespace zsoda::core {

enum class PixelConversionStatus {
  kOk = 0,
  kInvalidArgument,
  kInvalidDimensions,
  kDimensionMismatch,
  kInvalidStride,
  kUnsupportedFormat,
  kFormatMismatch,
};

// Host buffer view assumptions:
// - Interleaved RGBA layout in native endianness (R, G, B, A order).
// - Positive top-down row order only.
// - row_bytes may include padding and must be >= width * bytes_per_pixel(format).
struct HostBufferView {
  const void* pixels = nullptr;
  int width = 0;
  int height = 0;
  std::size_t row_bytes = 0;
  PixelFormat format = PixelFormat::kRGBA8;
};

// Same layout contract as HostBufferView, but writable.
struct MutableHostBufferView {
  void* pixels = nullptr;
  int width = 0;
  int height = 0;
  std::size_t row_bytes = 0;
  PixelFormat format = PixelFormat::kRGBA8;
};

[[nodiscard]] inline const char* PixelConversionStatusString(PixelConversionStatus status) {
  switch (status) {
    case PixelConversionStatus::kOk:
      return "ok";
    case PixelConversionStatus::kInvalidArgument:
      return "invalid argument";
    case PixelConversionStatus::kInvalidDimensions:
      return "invalid dimensions";
    case PixelConversionStatus::kDimensionMismatch:
      return "dimension mismatch";
    case PixelConversionStatus::kInvalidStride:
      return "invalid stride";
    case PixelConversionStatus::kUnsupportedFormat:
      return "unsupported format";
    case PixelConversionStatus::kFormatMismatch:
      return "format mismatch";
  }
  return "unknown";
}

namespace detail {

constexpr std::size_t kRGBAChannels = 4;
constexpr float kInv255 = 1.0F / 255.0F;
constexpr float kInv65535 = 1.0F / 65535.0F;
constexpr float kLumaR = 0.2126F;
constexpr float kLumaG = 0.7152F;
constexpr float kLumaB = 0.0722F;

[[nodiscard]] inline std::size_t BytesPerPixel(PixelFormat format) {
  switch (format) {
    case PixelFormat::kRGBA8:
      return kRGBAChannels * sizeof(std::uint8_t);
    case PixelFormat::kRGBA16:
      return kRGBAChannels * sizeof(std::uint16_t);
    case PixelFormat::kRGBA32F:
      return kRGBAChannels * sizeof(float);
    default:
      return 0;
  }
}

[[nodiscard]] inline float ClampUnit(float value) {
  if (!std::isfinite(value)) {
    return 0.0F;
  }
  return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] inline float LumaFromRgb(float r, float g, float b) {
  return ClampUnit(r * kLumaR + g * kLumaG + b * kLumaB);
}

[[nodiscard]] inline std::uint16_t LoadU16(const std::uint8_t* src) {
  std::uint16_t value = 0;
  std::memcpy(&value, src, sizeof(value));
  return value;
}

[[nodiscard]] inline float LoadF32(const std::uint8_t* src) {
  float value = 0.0F;
  std::memcpy(&value, src, sizeof(value));
  return value;
}

inline void StoreU16(std::uint8_t* dst, std::uint16_t value) {
  std::memcpy(dst, &value, sizeof(value));
}

inline void StoreF32(std::uint8_t* dst, float value) {
  std::memcpy(dst, &value, sizeof(value));
}

[[nodiscard]] inline std::uint8_t QuantizeToU8(float value) {
  const float scaled = ClampUnit(value) * 255.0F + 0.5F;
  return static_cast<std::uint8_t>(std::clamp(static_cast<int>(scaled), 0, 255));
}

[[nodiscard]] inline std::uint16_t QuantizeToU16(float value) {
  const float scaled = ClampUnit(value) * 65535.0F + 0.5F;
  return static_cast<std::uint16_t>(std::clamp(static_cast<int>(scaled), 0, 65535));
}

template <typename TView>
[[nodiscard]] inline PixelConversionStatus ValidateHostView(const TView& view,
                                                            std::size_t* out_bytes_per_pixel) {
  if (view.pixels == nullptr || out_bytes_per_pixel == nullptr) {
    return PixelConversionStatus::kInvalidArgument;
  }
  if (view.width <= 0 || view.height <= 0) {
    return PixelConversionStatus::kInvalidDimensions;
  }

  const std::size_t bytes_per_pixel = BytesPerPixel(view.format);
  if (bytes_per_pixel == 0) {
    return PixelConversionStatus::kUnsupportedFormat;
  }
  const std::size_t width_size = static_cast<std::size_t>(view.width);
  const std::size_t height_size = static_cast<std::size_t>(view.height);
  const std::size_t max_size = std::numeric_limits<std::size_t>::max();

  if (width_size > max_size / bytes_per_pixel) {
    return PixelConversionStatus::kInvalidDimensions;
  }
  const std::size_t min_row_bytes = width_size * bytes_per_pixel;
  if (view.row_bytes < min_row_bytes) {
    return PixelConversionStatus::kInvalidStride;
  }
  if (height_size > 0 && view.row_bytes > max_size / height_size) {
    return PixelConversionStatus::kInvalidStride;
  }

  *out_bytes_per_pixel = bytes_per_pixel;
  return PixelConversionStatus::kOk;
}

}  // namespace detail

// Converts host RGBA8/16/32F into normalized Gray32F.
// Integer inputs map full-range [0..max] to [0..1]. Float inputs are sanitized
// (NaN/inf -> 0) and clamped to [0..1] before luma conversion.
[[nodiscard]] inline PixelConversionStatus ConvertHostToGray32F(const HostBufferView& source,
                                                                FrameBuffer* out_gray) {
  if (out_gray == nullptr) {
    return PixelConversionStatus::kInvalidArgument;
  }

  std::size_t bytes_per_pixel = 0;
  const PixelConversionStatus source_status = detail::ValidateHostView(source, &bytes_per_pixel);
  if (source_status != PixelConversionStatus::kOk) {
    return source_status;
  }

  FrameDesc gray_desc;
  gray_desc.width = source.width;
  gray_desc.height = source.height;
  gray_desc.channels = 1;
  gray_desc.format = PixelFormat::kGray32F;
  out_gray->Resize(gray_desc);

  const auto* base = static_cast<const std::uint8_t*>(source.pixels);
  for (int y = 0; y < source.height; ++y) {
    const auto* row = base + static_cast<std::size_t>(y) * source.row_bytes;
    for (int x = 0; x < source.width; ++x) {
      const auto* pixel = row + static_cast<std::size_t>(x) * bytes_per_pixel;

      float r = 0.0F;
      float g = 0.0F;
      float b = 0.0F;
      switch (source.format) {
        case PixelFormat::kRGBA8: {
          r = static_cast<float>(pixel[0]) * detail::kInv255;
          g = static_cast<float>(pixel[1]) * detail::kInv255;
          b = static_cast<float>(pixel[2]) * detail::kInv255;
          break;
        }
        case PixelFormat::kRGBA16: {
          r = static_cast<float>(detail::LoadU16(pixel + sizeof(std::uint16_t) * 0)) *
              detail::kInv65535;
          g = static_cast<float>(detail::LoadU16(pixel + sizeof(std::uint16_t) * 1)) *
              detail::kInv65535;
          b = static_cast<float>(detail::LoadU16(pixel + sizeof(std::uint16_t) * 2)) *
              detail::kInv65535;
          break;
        }
        case PixelFormat::kRGBA32F: {
          r = detail::ClampUnit(detail::LoadF32(pixel + sizeof(float) * 0));
          g = detail::ClampUnit(detail::LoadF32(pixel + sizeof(float) * 1));
          b = detail::ClampUnit(detail::LoadF32(pixel + sizeof(float) * 2));
          break;
        }
        default:
          return PixelConversionStatus::kUnsupportedFormat;
      }

      out_gray->at(x, y, 0) = detail::LumaFromRgb(r, g, b);
    }
  }

  return PixelConversionStatus::kOk;
}

// Converts normalized Gray32F into host RGBA8/16/32F.
// Output is clamped to [0..1] before quantization; alpha is written as fully opaque.
[[nodiscard]] inline PixelConversionStatus ConvertGray32FToHost(const FrameBuffer& gray,
                                                                const MutableHostBufferView& dest) {
  const FrameDesc& gray_desc = gray.desc();
  if (gray.empty() || !IsValid(gray_desc)) {
    return PixelConversionStatus::kInvalidArgument;
  }
  if (gray_desc.channels < 1 || gray_desc.format != PixelFormat::kGray32F) {
    return PixelConversionStatus::kFormatMismatch;
  }

  std::size_t bytes_per_pixel = 0;
  const PixelConversionStatus dest_status = detail::ValidateHostView(dest, &bytes_per_pixel);
  if (dest_status != PixelConversionStatus::kOk) {
    return dest_status;
  }
  if (dest.width != gray_desc.width || dest.height != gray_desc.height) {
    return PixelConversionStatus::kDimensionMismatch;
  }

  auto* base = static_cast<std::uint8_t*>(dest.pixels);
  for (int y = 0; y < gray_desc.height; ++y) {
    auto* row = base + static_cast<std::size_t>(y) * dest.row_bytes;
    for (int x = 0; x < gray_desc.width; ++x) {
      auto* pixel = row + static_cast<std::size_t>(x) * bytes_per_pixel;
      const float value = detail::ClampUnit(gray.at(x, y, 0));

      switch (dest.format) {
        case PixelFormat::kRGBA8: {
          const std::uint8_t q = detail::QuantizeToU8(value);
          pixel[0] = q;
          pixel[1] = q;
          pixel[2] = q;
          pixel[3] = std::numeric_limits<std::uint8_t>::max();
          break;
        }
        case PixelFormat::kRGBA16: {
          const std::uint16_t q = detail::QuantizeToU16(value);
          detail::StoreU16(pixel + sizeof(std::uint16_t) * 0, q);
          detail::StoreU16(pixel + sizeof(std::uint16_t) * 1, q);
          detail::StoreU16(pixel + sizeof(std::uint16_t) * 2, q);
          detail::StoreU16(pixel + sizeof(std::uint16_t) * 3,
                           std::numeric_limits<std::uint16_t>::max());
          break;
        }
        case PixelFormat::kRGBA32F: {
          detail::StoreF32(pixel + sizeof(float) * 0, value);
          detail::StoreF32(pixel + sizeof(float) * 1, value);
          detail::StoreF32(pixel + sizeof(float) * 2, value);
          detail::StoreF32(pixel + sizeof(float) * 3, 1.0F);
          break;
        }
        default:
          return PixelConversionStatus::kUnsupportedFormat;
      }
    }
  }

  return PixelConversionStatus::kOk;
}

}  // namespace zsoda::core
