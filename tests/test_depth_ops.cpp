#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "core/DepthOps.h"
#include "core/PixelConversion.h"

namespace {

bool NearlyEqual(float lhs, float rhs, float eps = 1e-5F) {
  return std::fabs(lhs - rhs) <= eps;
}

std::uint16_t ReadU16(const std::uint8_t* src) {
  std::uint16_t value = 0;
  std::memcpy(&value, src, sizeof(value));
  return value;
}

float ReadF32(const std::uint8_t* src) {
  float value = 0.0F;
  std::memcpy(&value, src, sizeof(value));
  return value;
}

void WriteF32(std::uint8_t* dst, float value) {
  std::memcpy(dst, &value, sizeof(value));
}

void TestNormalizeDepth() {
  zsoda::core::FrameDesc desc;
  desc.width = 3;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;

  zsoda::core::FrameBuffer depth(desc);
  depth.at(0, 0, 0) = 10.0F;
  depth.at(1, 0, 0) = 20.0F;
  depth.at(2, 0, 0) = 30.0F;

  zsoda::core::NormalizeDepth(&depth, false);
  assert(depth.at(0, 0, 0) == 0.0F);
  assert(depth.at(2, 0, 0) == 1.0F);
}

void TestNormalizeDepthInvertAndFlatInput() {
  zsoda::core::FrameDesc desc;
  desc.width = 2;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;

  zsoda::core::FrameBuffer depth(desc);
  depth.at(0, 0, 0) = 5.0F;
  depth.at(1, 0, 0) = 5.0F;
  zsoda::core::NormalizeDepth(&depth, false);
  assert(depth.at(0, 0, 0) == 0.0F);
  assert(depth.at(1, 0, 0) == 0.0F);

  zsoda::core::FrameBuffer inverted(desc);
  inverted.at(0, 0, 0) = 5.0F;
  inverted.at(1, 0, 0) = 5.0F;
  zsoda::core::NormalizeDepth(&inverted, true);
  assert(inverted.at(0, 0, 0) == 1.0F);
  assert(inverted.at(1, 0, 0) == 1.0F);
}

void TestApplyDepthMappingRawAndNormalizeModes() {
  zsoda::core::FrameDesc desc;
  desc.width = 2;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;

  zsoda::core::FrameBuffer raw(desc);
  raw.at(0, 0, 0) = 0.2F;
  raw.at(1, 0, 0) = 0.8F;

  zsoda::core::DepthMappingParams raw_params;
  raw_params.mode = zsoda::core::DepthMappingMode::kRaw;
  raw_params.invert = false;
  zsoda::core::ApplyDepthMapping(&raw, raw_params, nullptr);
  assert(NearlyEqual(raw.at(0, 0, 0), 0.2F));
  assert(NearlyEqual(raw.at(1, 0, 0), 0.8F));

  zsoda::core::FrameBuffer normalized(desc);
  normalized.at(0, 0, 0) = 0.2F;
  normalized.at(1, 0, 0) = 0.8F;

  zsoda::core::DepthMappingParams norm_params;
  norm_params.mode = zsoda::core::DepthMappingMode::kNormalize;
  zsoda::core::ApplyDepthMapping(&normalized, norm_params, nullptr);
  assert(NearlyEqual(normalized.at(0, 0, 0), 0.0F));
  assert(NearlyEqual(normalized.at(1, 0, 0), 1.0F));
}

void TestApplyDepthMappingRawHandlesUnboundedInput() {
  zsoda::core::FrameDesc desc;
  desc.width = 4;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;

  zsoda::core::FrameBuffer raw(desc);
  raw.at(0, 0, 0) = 2.0F;
  raw.at(1, 0, 0) = 4.0F;
  raw.at(2, 0, 0) = 6.0F;
  raw.at(3, 0, 0) = 8.0F;

  zsoda::core::DepthMappingParams raw_params;
  raw_params.mode = zsoda::core::DepthMappingMode::kRaw;
  raw_params.guided_low_percentile = 0.0F;
  raw_params.guided_high_percentile = 1.0F;
  zsoda::core::ApplyDepthMapping(&raw, raw_params, nullptr);

  assert(NearlyEqual(raw.at(0, 0, 0), 0.0F));
  assert(NearlyEqual(raw.at(3, 0, 0), 1.0F));
  assert(raw.at(1, 0, 0) > raw.at(0, 0, 0));
  assert(raw.at(2, 0, 0) > raw.at(1, 0, 0));
}

void TestApplyDepthMappingGuidedModeWithState() {
  zsoda::core::FrameDesc desc;
  desc.width = 4;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;

  zsoda::core::DepthMappingParams guided_params;
  guided_params.mode = zsoda::core::DepthMappingMode::kGuided;
  guided_params.guided_low_percentile = 0.0F;
  guided_params.guided_high_percentile = 1.0F;
  guided_params.guided_update_alpha = 0.5F;
  zsoda::core::GuidedDepthMappingState state;

  zsoda::core::FrameBuffer first(desc);
  first.at(0, 0, 0) = 0.0F;
  first.at(1, 0, 0) = 1.0F;
  first.at(2, 0, 0) = 2.0F;
  first.at(3, 0, 0) = 3.0F;
  zsoda::core::ApplyDepthMapping(&first, guided_params, &state);
  assert(NearlyEqual(first.at(0, 0, 0), 0.0F));
  assert(NearlyEqual(first.at(3, 0, 0), 1.0F));
  assert(state.initialized);

  zsoda::core::FrameBuffer second(desc);
  second.at(0, 0, 0) = 1.0F;
  second.at(1, 0, 0) = 2.0F;
  second.at(2, 0, 0) = 3.0F;
  second.at(3, 0, 0) = 4.0F;
  zsoda::core::ApplyDepthMapping(&second, guided_params, &state);
  // With guided state smoothing, first value should not collapse to exact zero.
  assert(second.at(0, 0, 0) > 0.1F);
  assert(second.at(0, 0, 0) < 0.3F);
}

void TestApplyDepthMappingSanitizesNonFiniteInputs() {
  zsoda::core::FrameDesc desc;
  desc.width = 3;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;

  zsoda::core::FrameBuffer raw(desc);
  raw.at(0, 0, 0) = std::numeric_limits<float>::quiet_NaN();
  raw.at(1, 0, 0) = 0.5F;
  raw.at(2, 0, 0) = std::numeric_limits<float>::infinity();
  zsoda::core::DepthMappingParams raw_params;
  raw_params.mode = zsoda::core::DepthMappingMode::kRaw;
  zsoda::core::ApplyDepthMapping(&raw, raw_params, nullptr);
  assert(std::isfinite(raw.at(0, 0, 0)));
  assert(std::isfinite(raw.at(1, 0, 0)));
  assert(std::isfinite(raw.at(2, 0, 0)));
  assert(raw.at(0, 0, 0) == 0.0F);
  assert(NearlyEqual(raw.at(1, 0, 0), 0.5F));
  assert(raw.at(2, 0, 0) == 0.0F);

  zsoda::core::FrameBuffer normalized(desc);
  normalized.at(0, 0, 0) = std::numeric_limits<float>::quiet_NaN();
  normalized.at(1, 0, 0) = 2.0F;
  normalized.at(2, 0, 0) = 4.0F;
  zsoda::core::DepthMappingParams norm_params;
  norm_params.mode = zsoda::core::DepthMappingMode::kNormalize;
  zsoda::core::ApplyDepthMapping(&normalized, norm_params, nullptr);
  assert(std::isfinite(normalized.at(0, 0, 0)));
  assert(std::isfinite(normalized.at(1, 0, 0)));
  assert(std::isfinite(normalized.at(2, 0, 0)));
  assert(NearlyEqual(normalized.at(1, 0, 0), 0.0F));
  assert(NearlyEqual(normalized.at(2, 0, 0), 1.0F));
}

void TestSliceMatte() {
  zsoda::core::FrameDesc desc;
  desc.width = 3;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;
  zsoda::core::FrameBuffer depth(desc);
  depth.at(0, 0, 0) = 0.1F;
  depth.at(1, 0, 0) = 0.5F;
  depth.at(2, 0, 0) = 0.9F;

  auto matte = zsoda::core::BuildSliceMatte(depth, 0.3F, 0.7F, 0.0F);
  assert(matte.at(0, 0, 0) == 0.0F);
  assert(matte.at(1, 0, 0) == 1.0F);
  assert(matte.at(2, 0, 0) == 0.0F);
}

void TestPixelConversionStatusStrings() {
  using Status = zsoda::core::PixelConversionStatus;
  assert(std::string(zsoda::core::PixelConversionStatusString(Status::kOk)) == "ok");
  assert(std::string(zsoda::core::PixelConversionStatusString(Status::kInvalidArgument)) ==
         "invalid argument");
  assert(std::string(zsoda::core::PixelConversionStatusString(Status::kInvalidDimensions)) ==
         "invalid dimensions");
  assert(std::string(zsoda::core::PixelConversionStatusString(Status::kDimensionMismatch)) ==
         "dimension mismatch");
  assert(std::string(zsoda::core::PixelConversionStatusString(Status::kInvalidStride)) ==
         "invalid stride");
  assert(std::string(zsoda::core::PixelConversionStatusString(Status::kUnsupportedFormat)) ==
         "unsupported format");
  assert(std::string(zsoda::core::PixelConversionStatusString(Status::kFormatMismatch)) ==
         "format mismatch");
}

void TestConvertHostToGray32FRgba8WithStridePadding() {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  constexpr std::size_t kRowBytes = 12;
  std::vector<std::uint8_t> rgba(kRowBytes * kHeight, 0);

  auto* row0 = rgba.data();
  row0[0] = 255;
  row0[1] = 0;
  row0[2] = 0;
  row0[3] = 0;
  row0[4] = 0;
  row0[5] = 255;
  row0[6] = 0;
  row0[7] = 0;

  auto* row1 = rgba.data() + kRowBytes;
  row1[0] = 0;
  row1[1] = 0;
  row1[2] = 255;
  row1[3] = 0;
  row1[4] = 255;
  row1[5] = 255;
  row1[6] = 255;
  row1[7] = 255;

  zsoda::core::HostBufferView view;
  view.pixels = rgba.data();
  view.width = kWidth;
  view.height = kHeight;
  view.row_bytes = kRowBytes;
  view.format = zsoda::core::PixelFormat::kRGBA8;

  zsoda::core::FrameBuffer gray;
  const auto status = zsoda::core::ConvertHostToGray32F(view, &gray);
  assert(status == zsoda::core::PixelConversionStatus::kOk);
  assert(gray.desc().width == kWidth);
  assert(gray.desc().height == kHeight);
  assert(gray.desc().channels == 1);
  assert(gray.desc().format == zsoda::core::PixelFormat::kGray32F);

  assert(NearlyEqual(gray.at(0, 0, 0), 0.2126F, 1e-4F));
  assert(NearlyEqual(gray.at(1, 0, 0), 0.7152F, 1e-4F));
  assert(NearlyEqual(gray.at(0, 1, 0), 0.0722F, 1e-4F));
  assert(NearlyEqual(gray.at(1, 1, 0), 1.0F, 1e-4F));
}

void TestConvertHostToGray32FRgba32FSanitize() {
  constexpr int kWidth = 2;
  constexpr int kHeight = 1;
  constexpr std::size_t kBytesPerPixel = sizeof(float) * 4;
  constexpr std::size_t kRowBytes = kWidth * kBytesPerPixel;
  std::vector<std::uint8_t> rgba(kRowBytes, 0);

  std::uint8_t* p0 = rgba.data();
  WriteF32(p0 + sizeof(float) * 0, -1.0F);
  WriteF32(p0 + sizeof(float) * 1, 2.0F);
  WriteF32(p0 + sizeof(float) * 2, std::numeric_limits<float>::quiet_NaN());
  WriteF32(p0 + sizeof(float) * 3, 0.25F);

  std::uint8_t* p1 = p0 + kBytesPerPixel;
  WriteF32(p1 + sizeof(float) * 0, std::numeric_limits<float>::infinity());
  WriteF32(p1 + sizeof(float) * 1, 0.25F);
  WriteF32(p1 + sizeof(float) * 2, 0.75F);
  WriteF32(p1 + sizeof(float) * 3, 1.0F);

  zsoda::core::HostBufferView view;
  view.pixels = rgba.data();
  view.width = kWidth;
  view.height = kHeight;
  view.row_bytes = kRowBytes;
  view.format = zsoda::core::PixelFormat::kRGBA32F;

  zsoda::core::FrameBuffer gray;
  const auto status = zsoda::core::ConvertHostToGray32F(view, &gray);
  assert(status == zsoda::core::PixelConversionStatus::kOk);
  assert(NearlyEqual(gray.at(0, 0, 0), 0.7152F, 1e-4F));
  assert(NearlyEqual(gray.at(1, 0, 0), 0.23295F, 1e-4F));
}

void TestConvertHostToGray32FValidation() {
  using Status = zsoda::core::PixelConversionStatus;

  std::uint8_t pixel[16] = {0};
  zsoda::core::HostBufferView view;
  view.pixels = pixel;
  view.width = 1;
  view.height = 1;
  view.row_bytes = 4;
  view.format = zsoda::core::PixelFormat::kRGBA8;

  zsoda::core::FrameBuffer gray;
  assert(zsoda::core::ConvertHostToGray32F(view, nullptr) == Status::kInvalidArgument);

  zsoda::core::HostBufferView null_pixels = view;
  null_pixels.pixels = nullptr;
  assert(zsoda::core::ConvertHostToGray32F(null_pixels, &gray) == Status::kInvalidArgument);

  zsoda::core::HostBufferView bad_dims = view;
  bad_dims.width = 0;
  assert(zsoda::core::ConvertHostToGray32F(bad_dims, &gray) == Status::kInvalidDimensions);

  zsoda::core::HostBufferView bad_stride = view;
  bad_stride.row_bytes = 3;
  assert(zsoda::core::ConvertHostToGray32F(bad_stride, &gray) == Status::kInvalidStride);

  zsoda::core::HostBufferView unsupported = view;
  unsupported.row_bytes = 16;
  unsupported.format = zsoda::core::PixelFormat::kGray32F;
  assert(zsoda::core::ConvertHostToGray32F(unsupported, &gray) == Status::kUnsupportedFormat);
}

void TestConvertHostToRgb32FRgba8AndUnpremultiply() {
  constexpr int kWidth = 2;
  constexpr int kHeight = 1;
  constexpr std::size_t kRowBytes = kWidth * 4U;
  std::vector<std::uint8_t> rgba(kRowBytes * kHeight, 0);

  // Premultiplied red (straight 0.5 red with alpha 0.5 -> stored red ~= 0.25).
  rgba[0] = 64;
  rgba[1] = 0;
  rgba[2] = 0;
  rgba[3] = 128;
  // Opaque green.
  rgba[4] = 0;
  rgba[5] = 255;
  rgba[6] = 0;
  rgba[7] = 255;

  zsoda::core::HostBufferView view;
  view.pixels = rgba.data();
  view.width = kWidth;
  view.height = kHeight;
  view.row_bytes = kRowBytes;
  view.format = zsoda::core::PixelFormat::kRGBA8;

  zsoda::core::FrameBuffer rgb;
  auto status = zsoda::core::ConvertHostToRgb32F(view, &rgb);
  assert(status == zsoda::core::PixelConversionStatus::kOk);
  assert(rgb.desc().channels == 3);
  assert(rgb.desc().format == zsoda::core::PixelFormat::kRGBA32F);
  assert(NearlyEqual(rgb.at(0, 0, 0), 0.5F, 2e-2F));
  assert(NearlyEqual(rgb.at(0, 0, 1), 0.0F, 1e-5F));
  assert(NearlyEqual(rgb.at(0, 0, 2), 0.0F, 1e-5F));
  assert(NearlyEqual(rgb.at(1, 0, 0), 0.0F, 1e-5F));
  assert(NearlyEqual(rgb.at(1, 0, 1), 1.0F, 1e-5F));
  assert(NearlyEqual(rgb.at(1, 0, 2), 0.0F, 1e-5F));

  zsoda::core::FrameBuffer rgb_premult;
  status = zsoda::core::ConvertHostToRgb32F(view, &rgb_premult, false);
  assert(status == zsoda::core::PixelConversionStatus::kOk);
  assert(NearlyEqual(rgb_premult.at(0, 0, 0), 64.0F / 255.0F, 1e-5F));
}

void TestConvertHostToRgb32FValidation() {
  using Status = zsoda::core::PixelConversionStatus;

  std::uint8_t pixel[16] = {0};
  zsoda::core::HostBufferView view;
  view.pixels = pixel;
  view.width = 1;
  view.height = 1;
  view.row_bytes = 4;
  view.format = zsoda::core::PixelFormat::kRGBA8;

  zsoda::core::FrameBuffer rgb;
  assert(zsoda::core::ConvertHostToRgb32F(view, nullptr) == Status::kInvalidArgument);

  zsoda::core::HostBufferView null_pixels = view;
  null_pixels.pixels = nullptr;
  assert(zsoda::core::ConvertHostToRgb32F(null_pixels, &rgb) == Status::kInvalidArgument);

  zsoda::core::HostBufferView bad_dims = view;
  bad_dims.width = 0;
  assert(zsoda::core::ConvertHostToRgb32F(bad_dims, &rgb) == Status::kInvalidDimensions);

  zsoda::core::HostBufferView bad_stride = view;
  bad_stride.row_bytes = 3;
  assert(zsoda::core::ConvertHostToRgb32F(bad_stride, &rgb) == Status::kInvalidStride);

  zsoda::core::HostBufferView unsupported = view;
  unsupported.row_bytes = 16;
  unsupported.format = zsoda::core::PixelFormat::kGray32F;
  assert(zsoda::core::ConvertHostToRgb32F(unsupported, &rgb) == Status::kUnsupportedFormat);
}

void TestConvertGray32FToHostFormats() {
  zsoda::core::FrameDesc desc;
  desc.width = 3;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;
  zsoda::core::FrameBuffer gray(desc);
  gray.at(0, 0, 0) = -0.1F;
  gray.at(1, 0, 0) = 0.5F;
  gray.at(2, 0, 0) = 1.2F;

  std::vector<std::uint8_t> rgba8(desc.width * 4U, 0);
  zsoda::core::MutableHostBufferView out8;
  out8.pixels = rgba8.data();
  out8.width = desc.width;
  out8.height = desc.height;
  out8.row_bytes = desc.width * 4U;
  out8.format = zsoda::core::PixelFormat::kRGBA8;
  assert(zsoda::core::ConvertGray32FToHost(gray, out8) == zsoda::core::PixelConversionStatus::kOk);

  assert(rgba8[0] == 0);
  assert(rgba8[1] == 0);
  assert(rgba8[2] == 0);
  assert(rgba8[3] == 255);
  assert(rgba8[4] == 128);
  assert(rgba8[5] == 128);
  assert(rgba8[6] == 128);
  assert(rgba8[7] == 255);
  assert(rgba8[8] == 255);
  assert(rgba8[9] == 255);
  assert(rgba8[10] == 255);
  assert(rgba8[11] == 255);

  std::vector<std::uint8_t> rgba16(desc.width * sizeof(std::uint16_t) * 4U, 0);
  zsoda::core::MutableHostBufferView out16;
  out16.pixels = rgba16.data();
  out16.width = desc.width;
  out16.height = desc.height;
  out16.row_bytes = desc.width * sizeof(std::uint16_t) * 4U;
  out16.format = zsoda::core::PixelFormat::kRGBA16;
  assert(zsoda::core::ConvertGray32FToHost(gray, out16) == zsoda::core::PixelConversionStatus::kOk);

  assert(ReadU16(rgba16.data() + 0) == 0);
  assert(ReadU16(rgba16.data() + 2) == 0);
  assert(ReadU16(rgba16.data() + 4) == 0);
  assert(ReadU16(rgba16.data() + 6) == 65535);
  assert(ReadU16(rgba16.data() + 8) == 32768);
  assert(ReadU16(rgba16.data() + 10) == 32768);
  assert(ReadU16(rgba16.data() + 12) == 32768);
  assert(ReadU16(rgba16.data() + 14) == 65535);
  assert(ReadU16(rgba16.data() + 16) == 65535);
  assert(ReadU16(rgba16.data() + 18) == 65535);
  assert(ReadU16(rgba16.data() + 20) == 65535);
  assert(ReadU16(rgba16.data() + 22) == 65535);

  std::vector<std::uint8_t> rgba32(desc.width * sizeof(float) * 4U, 0);
  zsoda::core::MutableHostBufferView out32;
  out32.pixels = rgba32.data();
  out32.width = desc.width;
  out32.height = desc.height;
  out32.row_bytes = desc.width * sizeof(float) * 4U;
  out32.format = zsoda::core::PixelFormat::kRGBA32F;
  assert(zsoda::core::ConvertGray32FToHost(gray, out32) == zsoda::core::PixelConversionStatus::kOk);

  assert(NearlyEqual(ReadF32(rgba32.data() + 0), 0.0F));
  assert(NearlyEqual(ReadF32(rgba32.data() + sizeof(float) * 1), 0.0F));
  assert(NearlyEqual(ReadF32(rgba32.data() + sizeof(float) * 2), 0.0F));
  assert(NearlyEqual(ReadF32(rgba32.data() + sizeof(float) * 3), 1.0F));
  assert(NearlyEqual(ReadF32(rgba32.data() + sizeof(float) * 4), 0.5F));
  assert(NearlyEqual(ReadF32(rgba32.data() + sizeof(float) * 5), 0.5F));
  assert(NearlyEqual(ReadF32(rgba32.data() + sizeof(float) * 6), 0.5F));
  assert(NearlyEqual(ReadF32(rgba32.data() + sizeof(float) * 7), 1.0F));
  assert(NearlyEqual(ReadF32(rgba32.data() + sizeof(float) * 8), 1.0F));
  assert(NearlyEqual(ReadF32(rgba32.data() + sizeof(float) * 9), 1.0F));
  assert(NearlyEqual(ReadF32(rgba32.data() + sizeof(float) * 10), 1.0F));
  assert(NearlyEqual(ReadF32(rgba32.data() + sizeof(float) * 11), 1.0F));
}

void TestConvertGray32FToHostValidation() {
  using Status = zsoda::core::PixelConversionStatus;

  std::uint8_t rgba8[8] = {0};
  zsoda::core::MutableHostBufferView view;
  view.pixels = rgba8;
  view.width = 1;
  view.height = 1;
  view.row_bytes = 4;
  view.format = zsoda::core::PixelFormat::kRGBA8;

  zsoda::core::FrameBuffer empty;
  assert(zsoda::core::ConvertGray32FToHost(empty, view) == Status::kInvalidArgument);

  zsoda::core::FrameDesc wrong_desc;
  wrong_desc.width = 1;
  wrong_desc.height = 1;
  wrong_desc.channels = 1;
  wrong_desc.format = zsoda::core::PixelFormat::kRGBA32F;
  zsoda::core::FrameBuffer wrong_format(wrong_desc);
  wrong_format.at(0, 0, 0) = 0.3F;
  assert(zsoda::core::ConvertGray32FToHost(wrong_format, view) == Status::kFormatMismatch);

  zsoda::core::FrameDesc gray_desc;
  gray_desc.width = 1;
  gray_desc.height = 1;
  gray_desc.channels = 1;
  gray_desc.format = zsoda::core::PixelFormat::kGray32F;
  zsoda::core::FrameBuffer gray(gray_desc);
  gray.at(0, 0, 0) = 0.5F;

  zsoda::core::MutableHostBufferView null_pixels = view;
  null_pixels.pixels = nullptr;
  assert(zsoda::core::ConvertGray32FToHost(gray, null_pixels) == Status::kInvalidArgument);

  zsoda::core::MutableHostBufferView mismatch = view;
  mismatch.width = 2;
  mismatch.row_bytes = 8;
  assert(zsoda::core::ConvertGray32FToHost(gray, mismatch) == Status::kDimensionMismatch);

  zsoda::core::MutableHostBufferView bad_stride = view;
  bad_stride.row_bytes = 3;
  assert(zsoda::core::ConvertGray32FToHost(gray, bad_stride) == Status::kInvalidStride);

  zsoda::core::MutableHostBufferView unsupported = view;
  unsupported.row_bytes = 16;
  unsupported.format = zsoda::core::PixelFormat::kGray32F;
  assert(zsoda::core::ConvertGray32FToHost(gray, unsupported) == Status::kUnsupportedFormat);
}

}  // namespace

void RunDepthOpsTests() {
  TestNormalizeDepth();
  TestNormalizeDepthInvertAndFlatInput();
  TestApplyDepthMappingRawAndNormalizeModes();
  TestApplyDepthMappingRawHandlesUnboundedInput();
  TestApplyDepthMappingGuidedModeWithState();
  TestApplyDepthMappingSanitizesNonFiniteInputs();
  TestSliceMatte();
  TestPixelConversionStatusStrings();
  TestConvertHostToGray32FRgba8WithStridePadding();
  TestConvertHostToGray32FRgba32FSanitize();
  TestConvertHostToGray32FValidation();
  TestConvertHostToRgb32FRgba8AndUnpremultiply();
  TestConvertHostToRgb32FValidation();
  TestConvertGray32FToHostFormats();
  TestConvertGray32FToHostValidation();
}
