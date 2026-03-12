#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
int main() {
  std::cerr << "zsoda_still_quality_harness is only available on Windows.\n";
  return 1;
}
#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include "core/DepthOps.h"
#include "core/RenderPipeline.h"
#include "inference/ManagedInferenceEngine.h"
#include "inference/RuntimeOptions.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

using zsoda::core::DepthMappingMode;
using zsoda::core::FrameBuffer;
using zsoda::core::FrameDesc;
using zsoda::core::PixelFormat;
using zsoda::core::RenderOutput;
using zsoda::core::RenderParams;
using zsoda::core::RenderPipeline;
using zsoda::core::RenderStatus;
using zsoda::inference::InferenceBackendStatus;
using zsoda::inference::InferenceRequest;
using zsoda::inference::ManagedInferenceEngine;
using zsoda::inference::ParsePreprocessResizeMode;
using zsoda::inference::ParseRuntimeBackend;
using zsoda::inference::PreprocessResizeMode;
using zsoda::inference::ParseRemoteTransportProtocol;
using zsoda::inference::RuntimeBackend;
using zsoda::inference::RuntimeBackendName;
using zsoda::inference::RuntimeOptions;

struct CliOptions {
  fs::path input_path;
  fs::path output_dir;
  fs::path model_root = "models";
  std::string model_id = "distill-any-depth-base";
  RuntimeBackend backend = RuntimeBackend::kAuto;
  PreprocessResizeMode resize_mode = PreprocessResizeMode::kUpperBoundLetterbox;
  DepthMappingMode mapping_mode = DepthMappingMode::kRaw;
  int quality = 1;
  bool invert = false;
  float guided_low_percentile = 0.02F;
  float guided_high_percentile = 0.98F;
  float guided_update_alpha = 0.12F;
  float temporal_alpha = 1.0F;
  float edge_enhancement = 0.18F;
  int tile_size = 512;
  int overlap = 32;
  int vram_budget_mb = 0;
  bool raw_visual_minmax = true;
  bool quiet = false;
};

struct LoadedImage {
  FrameBuffer frame;
  std::vector<std::uint8_t> rgba8;
};

struct FrameStats {
  int width = 0;
  int height = 0;
  int channels = 0;
  float minimum = 0.0F;
  float maximum = 0.0F;
  float mean = 0.0F;
  float stddev = 0.0F;
};

struct TraceCapture {
  fs::path path;
  std::uintmax_t start_offset = 0;
};

std::string JsonEscape(std::string_view value) {
  std::ostringstream oss;
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        oss << "\\\\";
        break;
      case '"':
        oss << "\\\"";
        break;
      case '\n':
        oss << "\\n";
        break;
      case '\r':
        oss << "\\r";
        break;
      case '\t':
        oss << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec;
        } else {
          oss << ch;
        }
        break;
    }
  }
  return oss.str();
}

std::string PathString(const fs::path& path) {
  return path.generic_string();
}

int ParsePositiveIntEnvOrDefault(const char* name, int default_value) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return default_value;
  }
  try {
    const int parsed = std::stoi(value);
    return parsed > 0 ? parsed : default_value;
  } catch (...) {
    return default_value;
  }
}

bool ParseBoolEnvOrDefault(const char* name, bool default_value) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return default_value;
  }
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
    return false;
  }
  return default_value;
}

std::string ReadEnvOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

std::string FormatHresult(HRESULT hr) {
  std::ostringstream oss;
  oss << "HRESULT=0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
  return oss.str();
}

const char* RenderStatusName(RenderStatus status) {
  switch (status) {
    case RenderStatus::kCacheHit:
      return "cache_hit";
    case RenderStatus::kInference:
      return "inference";
    case RenderStatus::kFallbackTiled:
      return "fallback_tiled";
    case RenderStatus::kFallbackDownscaled:
      return "fallback_downscaled";
    case RenderStatus::kSafeOutput:
      return "safe_output";
  }
  return "unknown";
}

class ScopedComApartment {
 public:
  ScopedComApartment() {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
      ok_ = true;
      should_uninitialize_ = true;
      return;
    }
    if (hr == RPC_E_CHANGED_MODE) {
      ok_ = true;
      return;
    }
    error_ = "CoInitializeEx failed: " + FormatHresult(hr);
  }

  ~ScopedComApartment() {
    if (should_uninitialize_) {
      ::CoUninitialize();
    }
  }

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }

 private:
  bool ok_ = false;
  bool should_uninitialize_ = false;
  std::string error_;
};

bool CreateWicFactory(ComPtr<IWICImagingFactory>* out_factory, std::string* error) {
  ComPtr<IWICImagingFactory> factory;
  const HRESULT hr = ::CoCreateInstance(
      CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "CoCreateInstance(CLSID_WICImagingFactory) failed: " + FormatHresult(hr);
    }
    return false;
  }
  *out_factory = std::move(factory);
  return true;
}

bool LoadImageRgba32F(IWICImagingFactory* factory,
                      const fs::path& input_path,
                      LoadedImage* out_image,
                      std::string* error) {
  ComPtr<IWICBitmapDecoder> decoder;
  HRESULT hr = factory->CreateDecoderFromFilename(
      input_path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "CreateDecoderFromFilename failed for " + PathString(input_path) + ": " +
               FormatHresult(hr);
    }
    return false;
  }

  ComPtr<IWICBitmapFrameDecode> frame;
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "IWICBitmapDecoder::GetFrame failed: " + FormatHresult(hr);
    }
    return false;
  }

  UINT width = 0;
  UINT height = 0;
  hr = frame->GetSize(&width, &height);
  if (FAILED(hr) || width == 0 || height == 0) {
    if (error != nullptr) {
      *error = "IWICBitmapSource::GetSize failed: " + FormatHresult(hr);
    }
    return false;
  }

  ComPtr<IWICFormatConverter> converter;
  hr = factory->CreateFormatConverter(&converter);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "CreateFormatConverter failed: " + FormatHresult(hr);
    }
    return false;
  }

  hr = converter->Initialize(frame.Get(),
                             GUID_WICPixelFormat32bppRGBA,
                             WICBitmapDitherTypeNone,
                             nullptr,
                             0.0,
                             WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "IWICFormatConverter::Initialize failed: " + FormatHresult(hr);
    }
    return false;
  }

  const UINT stride = width * 4U;
  std::vector<std::uint8_t> rgba8(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height));
  hr = converter->CopyPixels(
      nullptr, stride, static_cast<UINT>(rgba8.size()), reinterpret_cast<BYTE*>(rgba8.data()));
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "IWICBitmapSource::CopyPixels failed: " + FormatHresult(hr);
    }
    return false;
  }

  FrameDesc desc;
  desc.width = static_cast<int>(width);
  desc.height = static_cast<int>(height);
  desc.channels = 4;
  desc.format = PixelFormat::kRGBA32F;
  out_image->frame.Resize(desc);
  out_image->rgba8 = rgba8;

  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(desc.width) +
                                 static_cast<std::size_t>(x)) * 4U;
      for (int channel = 0; channel < 4; ++channel) {
        out_image->frame.at(x, y, channel) =
            static_cast<float>(rgba8[index + static_cast<std::size_t>(channel)]) / 255.0F;
      }
    }
  }
  return true;
}

bool SaveRgbaPng(IWICImagingFactory* factory,
                 const fs::path& output_path,
                 const std::vector<std::uint8_t>& rgba8,
                 int width,
                 int height,
                 std::string* error) {
  std::error_code ec;
  fs::create_directories(output_path.parent_path(), ec);

  ComPtr<IWICStream> stream;
  HRESULT hr = factory->CreateStream(&stream);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "CreateStream failed: " + FormatHresult(hr);
    }
    return false;
  }

  hr = stream->InitializeFromFilename(output_path.c_str(), GENERIC_WRITE);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "InitializeFromFilename failed for " + PathString(output_path) + ": " +
               FormatHresult(hr);
    }
    return false;
  }

  ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "CreateEncoder(GUID_ContainerFormatPng) failed: " + FormatHresult(hr);
    }
    return false;
  }

  hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "IWICBitmapEncoder::Initialize failed: " + FormatHresult(hr);
    }
    return false;
  }

  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  hr = encoder->CreateNewFrame(&frame, &props);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "IWICBitmapEncoder::CreateNewFrame failed: " + FormatHresult(hr);
    }
    return false;
  }

  hr = frame->Initialize(props.Get());
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "IWICBitmapFrameEncode::Initialize failed: " + FormatHresult(hr);
    }
    return false;
  }

  hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "IWICBitmapFrameEncode::SetSize failed: " + FormatHresult(hr);
    }
    return false;
  }

  WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
  hr = frame->SetPixelFormat(&format);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "IWICBitmapFrameEncode::SetPixelFormat failed: " + FormatHresult(hr);
    }
    return false;
  }

  const UINT stride = static_cast<UINT>(width) * 4U;
  hr = frame->WritePixels(static_cast<UINT>(height),
                          stride,
                          static_cast<UINT>(rgba8.size()),
                          const_cast<BYTE*>(reinterpret_cast<const BYTE*>(rgba8.data())));
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "IWICBitmapFrameEncode::WritePixels failed: " + FormatHresult(hr);
    }
    return false;
  }

  hr = frame->Commit();
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "IWICBitmapFrameEncode::Commit failed: " + FormatHresult(hr);
    }
    return false;
  }

  hr = encoder->Commit();
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "IWICBitmapEncoder::Commit failed: " + FormatHresult(hr);
    }
    return false;
  }
  return true;
}

FrameStats ComputeFrameStats(const FrameBuffer& frame) {
  FrameStats stats;
  const auto& desc = frame.desc();
  stats.width = desc.width;
  stats.height = desc.height;
  stats.channels = desc.channels;
  if (frame.empty()) {
    return stats;
  }

  float minimum = std::numeric_limits<float>::infinity();
  float maximum = -std::numeric_limits<float>::infinity();
  long double sum = 0.0;
  long double sum_sq = 0.0;
  for (std::size_t i = 0; i < frame.size(); ++i) {
    const float value = frame.data()[i];
    minimum = std::min(minimum, value);
    maximum = std::max(maximum, value);
    sum += value;
    sum_sq += static_cast<long double>(value) * static_cast<long double>(value);
  }
  const long double count = static_cast<long double>(frame.size());
  const long double mean = sum / count;
  const long double variance = std::max(0.0L, (sum_sq / count) - (mean * mean));
  stats.minimum = minimum;
  stats.maximum = maximum;
  stats.mean = static_cast<float>(mean);
  stats.stddev = static_cast<float>(std::sqrt(variance));
  return stats;
}

std::vector<std::uint8_t> VisualizeSingleChannel(const FrameBuffer& frame,
                                                 bool minmax_normalize,
                                                 FrameStats* out_stats = nullptr) {
  FrameStats stats = ComputeFrameStats(frame);
  if (out_stats != nullptr) {
    *out_stats = stats;
  }

  const auto& desc = frame.desc();
  std::vector<std::uint8_t> rgba8(static_cast<std::size_t>(desc.width) *
                                      static_cast<std::size_t>(desc.height) * 4U,
                                  255U);

  float minimum = minmax_normalize ? stats.minimum : 0.0F;
  float maximum = minmax_normalize ? stats.maximum : 1.0F;
  if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum + 1e-6F) {
    minimum = 0.0F;
    maximum = 1.0F;
  }
  const float range = std::max(1e-6F, maximum - minimum);

  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float raw = frame.at(x, y, 0);
      float normalized = (raw - minimum) / range;
      normalized = std::clamp(normalized, 0.0F, 1.0F);
      const auto value = static_cast<std::uint8_t>(std::lround(normalized * 255.0F));
      const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(desc.width) +
                                 static_cast<std::size_t>(x)) * 4U;
      rgba8[index + 0U] = value;
      rgba8[index + 1U] = value;
      rgba8[index + 2U] = value;
      rgba8[index + 3U] = 255U;
    }
  }
  return rgba8;
}

bool SaveNpyFloat32(const fs::path& output_path, const FrameBuffer& frame, std::string* error) {
  std::error_code ec;
  fs::create_directories(output_path.parent_path(), ec);

  std::ofstream stream(output_path, std::ios::binary);
  if (!stream.is_open()) {
    if (error != nullptr) {
      *error = "failed to open npy output: " + PathString(output_path);
    }
    return false;
  }

  const auto& desc = frame.desc();
  std::ostringstream shape;
  if (desc.channels == 1) {
    shape << "(" << desc.height << ", " << desc.width << ")";
  } else {
    shape << "(" << desc.height << ", " << desc.width << ", " << desc.channels << ")";
  }
  std::string header =
      "{'descr': '<f4', 'fortran_order': False, 'shape': " + shape.str() + ", }";
  const std::size_t preamble_size = 10U;
  const std::size_t padding = (16U - ((preamble_size + header.size() + 1U) % 16U)) % 16U;
  header.append(padding, ' ');
  header.push_back('\n');

  const std::uint16_t header_size = static_cast<std::uint16_t>(header.size());
  const unsigned char magic[] = {0x93, 'N', 'U', 'M', 'P', 'Y', 0x01, 0x00};
  stream.write(reinterpret_cast<const char*>(magic), sizeof(magic));
  stream.put(static_cast<char>(header_size & 0xFFU));
  stream.put(static_cast<char>((header_size >> 8U) & 0xFFU));
  stream.write(header.data(), static_cast<std::streamsize>(header.size()));
  stream.write(reinterpret_cast<const char*>(frame.data()),
               static_cast<std::streamsize>(frame.size() * sizeof(float)));
  return static_cast<bool>(stream);
}

void PrintUsage() {
  std::cout
      << "Usage: zsoda_still_quality_harness --input <image> --output-dir <dir> [options]\n"
      << "  --model-root <dir>\n"
      << "  --model-id <id>\n"
      << "  --backend auto|cpu|cuda|tensorrt|directml|coreml|remote\n"
      << "  --resize-mode upper_bound_letterbox|lower_bound_center_crop\n"
      << "  --quality <1..4>\n"
      << "  --mapping-mode raw|normalize|guided|v2-style\n"
      << "  --invert\n"
      << "  --guided-low <0..1>\n"
      << "  --guided-high <0..1>\n"
      << "  --guided-alpha <0..1>\n"
      << "  --temporal-alpha <0..1>\n"
      << "  --edge-enhancement <0..1>\n"
      << "  --tile-size <int>\n"
      << "  --overlap <int>\n"
      << "  --vram-budget-mb <int>\n"
      << "  --raw-visualization minmax|clamp\n"
      << "  --quiet\n";
}

bool ParseInt(std::string_view text, int* out) {
  try {
    std::size_t consumed = 0;
    const int value = std::stoi(std::string(text), &consumed);
    if (consumed != text.size()) {
      return false;
    }
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseFloat(std::string_view text, float* out) {
  try {
    std::size_t consumed = 0;
    const float value = std::stof(std::string(text), &consumed);
    if (consumed != text.size()) {
      return false;
    }
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseArgs(int argc, char** argv, CliOptions* options, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      std::exit(0);
    }
    if (arg == "--quiet") {
      options->quiet = true;
      continue;
    }
    if (arg == "--invert") {
      options->invert = true;
      continue;
    }

    if (i + 1 >= argc) {
      if (error != nullptr) {
        *error = "missing value for " + arg;
      }
      return false;
    }

    const std::string value(argv[++i]);
    if (arg == "--input") {
      options->input_path = value;
    } else if (arg == "--output-dir") {
      options->output_dir = value;
    } else if (arg == "--model-root") {
      options->model_root = value;
    } else if (arg == "--model-id") {
      options->model_id = value;
    } else if (arg == "--backend") {
      options->backend = ParseRuntimeBackend(value);
    } else if (arg == "--resize-mode") {
      options->resize_mode = ParsePreprocessResizeMode(value);
    } else if (arg == "--quality") {
      if (!ParseInt(value, &options->quality)) {
        if (error != nullptr) {
          *error = "invalid --quality value";
        }
        return false;
      }
    } else if (arg == "--mapping-mode") {
      options->mapping_mode = zsoda::core::ParseDepthMappingMode(value);
    } else if (arg == "--guided-low") {
      if (!ParseFloat(value, &options->guided_low_percentile)) {
        if (error != nullptr) {
          *error = "invalid --guided-low value";
        }
        return false;
      }
    } else if (arg == "--guided-high") {
      if (!ParseFloat(value, &options->guided_high_percentile)) {
        if (error != nullptr) {
          *error = "invalid --guided-high value";
        }
        return false;
      }
    } else if (arg == "--guided-alpha") {
      if (!ParseFloat(value, &options->guided_update_alpha)) {
        if (error != nullptr) {
          *error = "invalid --guided-alpha value";
        }
        return false;
      }
    } else if (arg == "--temporal-alpha") {
      if (!ParseFloat(value, &options->temporal_alpha)) {
        if (error != nullptr) {
          *error = "invalid --temporal-alpha value";
        }
        return false;
      }
    } else if (arg == "--edge-enhancement") {
      if (!ParseFloat(value, &options->edge_enhancement)) {
        if (error != nullptr) {
          *error = "invalid --edge-enhancement value";
        }
        return false;
      }
    } else if (arg == "--tile-size") {
      if (!ParseInt(value, &options->tile_size)) {
        if (error != nullptr) {
          *error = "invalid --tile-size value";
        }
        return false;
      }
    } else if (arg == "--overlap") {
      if (!ParseInt(value, &options->overlap)) {
        if (error != nullptr) {
          *error = "invalid --overlap value";
        }
        return false;
      }
    } else if (arg == "--vram-budget-mb") {
      if (!ParseInt(value, &options->vram_budget_mb)) {
        if (error != nullptr) {
          *error = "invalid --vram-budget-mb value";
        }
        return false;
      }
    } else if (arg == "--raw-visualization") {
      if (value == "minmax") {
        options->raw_visual_minmax = true;
      } else if (value == "clamp") {
        options->raw_visual_minmax = false;
      } else {
        if (error != nullptr) {
          *error = "--raw-visualization must be minmax or clamp";
        }
        return false;
      }
    } else {
      if (error != nullptr) {
        *error = "unknown option: " + arg;
      }
      return false;
    }
  }

  if (options->input_path.empty()) {
    if (error != nullptr) {
      *error = "--input is required";
    }
    return false;
  }
  if (options->output_dir.empty()) {
    if (error != nullptr) {
      *error = "--output-dir is required";
    }
    return false;
  }
  return true;
}

TraceCapture BeginTraceCapture() {
  TraceCapture capture;
  char temp_path[MAX_PATH] = {};
  const DWORD written = ::GetTempPathA(MAX_PATH, temp_path);
  if (written == 0 || written >= MAX_PATH) {
    return capture;
  }
  capture.path = fs::path(temp_path) / "ZSoda_AE_Runtime.log";
  std::error_code ec;
  capture.start_offset = fs::exists(capture.path, ec) ? fs::file_size(capture.path, ec) : 0;
  return capture;
}

std::string ReadTraceDelta(const TraceCapture& capture) {
  if (capture.path.empty()) {
    return std::string();
  }
  std::error_code ec;
  if (!fs::exists(capture.path, ec)) {
    return std::string();
  }
  std::ifstream stream(capture.path, std::ios::binary);
  if (!stream.is_open()) {
    return std::string();
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff end = stream.tellg();
  std::streamoff start = static_cast<std::streamoff>(capture.start_offset);
  if (end < start) {
    start = 0;
  }
  stream.seekg(start, std::ios::beg);
  std::string text(static_cast<std::size_t>(end - start), '\0');
  if (!text.empty()) {
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
  }
  return text;
}

std::string CaptureEnvValue(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

std::string BuildEnvJson() {
  static constexpr const char* kEnvNames[] = {
      "ZSODA_PREPROCESS_RESIZE_MODE",
      "ZSODA_INPUT_LINEAR_TO_SRGB",
      "ZSODA_DETAIL_BOOST",
      "ZSODA_V2STYLE_GAMMA",
      "ZSODA_ORT_PREFER_PRELOADED",
      "ZSODA_ONNXRUNTIME_LIBRARY",
      "ZSODA_INFERENCE_BACKEND",
      "ZSODA_REMOTE_INFERENCE_ENABLED",
      "ZSODA_REMOTE_INFERENCE_ENDPOINT",
      "ZSODA_REMOTE_INFERENCE_URL",
      "ZSODA_REMOTE_SERVICE_AUTOSTART",
      "ZSODA_REMOTE_SERVICE_HOST",
      "ZSODA_REMOTE_SERVICE_PORT",
      "ZSODA_REMOTE_SERVICE_PYTHON",
      "ZSODA_REMOTE_SERVICE_SCRIPT",
      "ZSODA_REMOTE_SERVICE_LOG",
      "ZSODA_LOCKED_MODEL_ID",
      "ZSODA_REMOTE_INFERENCE_COMMAND",
  };

  std::ostringstream oss;
  oss << "{";
  bool first = true;
  for (const char* name : kEnvNames) {
    const std::string value = CaptureEnvValue(name);
    if (value.empty()) {
      continue;
    }
    if (!first) {
      oss << ",";
    }
    first = false;
    oss << "\n      \"" << JsonEscape(name) << "\": \"" << JsonEscape(value) << "\"";
  }
  if (!first) {
    oss << "\n    ";
  }
  oss << "}";
  return oss.str();
}

std::string BuildStatsJson(const FrameStats& stats) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(6);
  oss << "{"
      << "\"width\":" << stats.width << ","
      << "\"height\":" << stats.height << ","
      << "\"channels\":" << stats.channels << ","
      << "\"min\":" << stats.minimum << ","
      << "\"max\":" << stats.maximum << ","
      << "\"mean\":" << stats.mean << ","
      << "\"stddev\":" << stats.stddev
      << "}";
  return oss.str();
}

std::string BuildBackendJson(const InferenceBackendStatus& status) {
  std::ostringstream oss;
  oss << "{"
      << "\"requested_backend\":\"" << JsonEscape(RuntimeBackendName(status.requested_backend))
      << "\","
      << "\"active_backend\":\"" << JsonEscape(RuntimeBackendName(status.active_backend)) << "\","
      << "\"using_fallback_engine\":" << (status.using_fallback_engine ? "true" : "false") << ","
      << "\"last_run_used_fallback\":" << (status.last_run_used_fallback ? "true" : "false")
      << ","
      << "\"active_backend_name\":\"" << JsonEscape(status.active_backend_name) << "\","
      << "\"engine_name\":\"" << JsonEscape(status.engine_name) << "\","
      << "\"fallback_reason\":\"" << JsonEscape(status.fallback_reason) << "\""
      << "}";
  return oss.str();
}

RuntimeOptions BuildRuntimeOptions(const CliOptions& options) {
  RuntimeOptions runtime;
  runtime.preferred_backend = options.backend;
  runtime.preprocess_resize_mode = options.resize_mode;
  runtime.auto_download_missing_models = false;
  runtime.remote_inference_enabled = options.backend == RuntimeBackend::kRemote;
  runtime.remote_endpoint = ReadEnvOrEmpty("ZSODA_REMOTE_INFERENCE_ENDPOINT");
  if (runtime.remote_endpoint.empty()) {
    runtime.remote_endpoint = ReadEnvOrEmpty("ZSODA_REMOTE_INFERENCE_URL");
  }
  runtime.remote_timeout_ms =
      ParsePositiveIntEnvOrDefault("ZSODA_REMOTE_INFERENCE_TIMEOUT_MS", 0);
  runtime.remote_service_autostart =
      ParseBoolEnvOrDefault("ZSODA_REMOTE_SERVICE_AUTOSTART", false);
  runtime.remote_service_host = ReadEnvOrEmpty("ZSODA_REMOTE_SERVICE_HOST");
  if (runtime.remote_service_host.empty()) {
    runtime.remote_service_host = "127.0.0.1";
  }
  runtime.remote_service_port =
      ParsePositiveIntEnvOrDefault("ZSODA_REMOTE_SERVICE_PORT", 8345);
  runtime.remote_service_python = ReadEnvOrEmpty("ZSODA_REMOTE_SERVICE_PYTHON");
  runtime.remote_service_script_path = ReadEnvOrEmpty("ZSODA_REMOTE_SERVICE_SCRIPT");
  runtime.remote_service_log_path = ReadEnvOrEmpty("ZSODA_REMOTE_SERVICE_LOG");
  runtime.remote_transport_protocol =
      ParseRemoteTransportProtocol(ReadEnvOrEmpty("ZSODA_REMOTE_PROTOCOL"));
  return runtime;
}

RenderParams BuildRenderParams(const CliOptions& options) {
  RenderParams params;
  params.model_id = options.model_id;
  params.quality = options.quality;
  params.invert = options.invert;
  params.mapping_mode = options.mapping_mode;
  params.guided_low_percentile = options.guided_low_percentile;
  params.guided_high_percentile = options.guided_high_percentile;
  params.guided_update_alpha = options.guided_update_alpha;
  params.temporal_alpha = options.temporal_alpha;
  params.edge_enhancement = options.edge_enhancement;
  params.tile_size = options.tile_size;
  params.overlap = options.overlap;
  params.vram_budget_mb = options.vram_budget_mb;
  params.cache_enabled = false;
  params.freeze_enabled = false;
  params.frame_hash = 1;
  return params;
}

bool WriteTextFile(const fs::path& path, const std::string& content, std::string* error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    if (error != nullptr) {
      *error = "failed to open output file: " + PathString(path);
    }
    return false;
  }
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  return static_cast<bool>(stream);
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;
  std::string error;
  if (!ParseArgs(argc, argv, &options, &error)) {
    std::cerr << "Argument error: " << error << "\n";
    PrintUsage();
    return 1;
  }

  options.input_path = fs::absolute(options.input_path);
  options.output_dir = fs::absolute(options.output_dir);
  options.model_root = fs::absolute(options.model_root);

  ScopedComApartment com_apartment;
  if (!com_apartment.ok()) {
    std::cerr << com_apartment.error() << "\n";
    return 1;
  }

  ComPtr<IWICImagingFactory> wic_factory;
  if (!CreateWicFactory(&wic_factory, &error)) {
    std::cerr << error << "\n";
    return 1;
  }

  LoadedImage loaded_image;
  if (!LoadImageRgba32F(wic_factory.Get(), options.input_path, &loaded_image, &error)) {
    std::cerr << error << "\n";
    return 1;
  }

  const fs::path input_copy_path = options.output_dir / "input_source.png";
  if (!SaveRgbaPng(wic_factory.Get(),
                   input_copy_path,
                   loaded_image.rgba8,
                   loaded_image.frame.desc().width,
                   loaded_image.frame.desc().height,
                   &error)) {
    std::cerr << error << "\n";
    return 1;
  }

  const FrameStats input_stats = ComputeFrameStats(loaded_image.frame);
  const RuntimeOptions runtime_options = BuildRuntimeOptions(options);
  const RenderParams render_params = BuildRenderParams(options);
  const TraceCapture trace_capture = BeginTraceCapture();

  const fs::path raw_png_path = options.output_dir / "zsoda_raw_depth.png";
  const fs::path raw_npy_path = options.output_dir / "zsoda_raw_depth.npy";
  const fs::path pipeline_png_path = options.output_dir / "zsoda_pipeline_depth.png";
  const fs::path pipeline_npy_path = options.output_dir / "zsoda_pipeline_depth.npy";
  const fs::path trace_tail_path = options.output_dir / "zsoda_runtime_trace_tail.txt";
  const fs::path config_path = options.output_dir / "resolved_config.json";

  auto engine = std::make_shared<ManagedInferenceEngine>(PathString(options.model_root), runtime_options);

  bool initialize_ok = false;
  bool raw_ok = false;
  bool raw_png_ok = false;
  bool raw_npy_ok = false;
  bool pipeline_png_ok = false;
  bool pipeline_npy_ok = false;
  std::string initialize_error;
  std::string raw_error;
  std::string pipeline_file_error;
  std::string trace_error;
  FrameBuffer raw_depth;
  FrameStats raw_stats{};
  FrameStats pipeline_stats{};
  InferenceBackendStatus raw_backend{};
  InferenceBackendStatus pipeline_backend{};
  RenderOutput pipeline_output;

  initialize_ok = engine->Initialize(options.model_id, &initialize_error);
  raw_backend = engine->BackendStatus();

  if (initialize_ok) {
    InferenceRequest request;
    request.source = &loaded_image.frame;
    request.quality = options.quality;
    request.frame_hash = 1;
    raw_ok = engine->Run(request, &raw_depth, &raw_error);
    raw_backend = engine->BackendStatus();

    RenderPipeline pipeline(engine);
    pipeline_output = pipeline.Render(loaded_image.frame, render_params);
    pipeline_backend = engine->BackendStatus();
  } else {
    pipeline_output.status = RenderStatus::kSafeOutput;
    pipeline_output.message = initialize_error;
    pipeline_backend = raw_backend;
  }

  if (raw_ok && !raw_depth.empty() && raw_depth.desc().channels >= 1) {
    const std::vector<std::uint8_t> raw_visual =
        VisualizeSingleChannel(raw_depth, options.raw_visual_minmax, &raw_stats);
    raw_png_ok = SaveRgbaPng(
        wic_factory.Get(),
        raw_png_path,
        raw_visual,
        raw_depth.desc().width,
        raw_depth.desc().height,
        &raw_error);
    raw_npy_ok = SaveNpyFloat32(raw_npy_path, raw_depth, &raw_error);
  }

  if (!pipeline_output.frame.empty() && pipeline_output.frame.desc().channels >= 1 &&
      pipeline_output.frame.desc().format == PixelFormat::kGray32F) {
    const std::vector<std::uint8_t> pipeline_visual =
        VisualizeSingleChannel(pipeline_output.frame, true, &pipeline_stats);
    pipeline_png_ok = SaveRgbaPng(wic_factory.Get(),
                                  pipeline_png_path,
                                  pipeline_visual,
                                  pipeline_output.frame.desc().width,
                                  pipeline_output.frame.desc().height,
                                  &pipeline_file_error);
    pipeline_npy_ok = SaveNpyFloat32(pipeline_npy_path, pipeline_output.frame, &pipeline_file_error);
  }

  std::string trace_tail = ReadTraceDelta(trace_capture);
  if (!WriteTextFile(trace_tail_path, trace_tail, &trace_error)) {
    trace_tail.clear();
  }

  std::ostringstream resolved;
  resolved << "{\n";
  resolved << "  \"tool\": \"zsoda_still_quality_harness\",\n";
  resolved << "  \"input\": {\n";
  resolved << "    \"path\": \"" << JsonEscape(PathString(options.input_path)) << "\",\n";
  resolved << "    \"copy_png\": \"" << JsonEscape(PathString(input_copy_path)) << "\",\n";
  resolved << "    \"stats\": " << BuildStatsJson(input_stats) << "\n";
  resolved << "  },\n";
  resolved << "  \"runtime\": {\n";
  resolved << "    \"model_root\": \"" << JsonEscape(PathString(options.model_root)) << "\",\n";
  resolved << "    \"model_id\": \"" << JsonEscape(options.model_id) << "\",\n";
  resolved << "    \"backend\": \"" << JsonEscape(RuntimeBackendName(options.backend)) << "\",\n";
  resolved << "    \"resize_mode\": \""
           << JsonEscape(zsoda::inference::PreprocessResizeModeName(options.resize_mode)) << "\",\n";
  resolved << "    \"quality\": " << options.quality << ",\n";
  resolved << "    \"mapping_mode\": \""
           << JsonEscape(zsoda::core::DepthMappingModeName(options.mapping_mode)) << "\",\n";
  resolved << "    \"invert\": " << (options.invert ? "true" : "false") << ",\n";
  resolved << "    \"guided_low_percentile\": " << options.guided_low_percentile << ",\n";
  resolved << "    \"guided_high_percentile\": " << options.guided_high_percentile << ",\n";
  resolved << "    \"guided_update_alpha\": " << options.guided_update_alpha << ",\n";
  resolved << "    \"temporal_alpha\": " << options.temporal_alpha << ",\n";
  resolved << "    \"edge_enhancement\": " << options.edge_enhancement << ",\n";
  resolved << "    \"tile_size\": " << options.tile_size << ",\n";
  resolved << "    \"overlap\": " << options.overlap << ",\n";
  resolved << "    \"vram_budget_mb\": " << options.vram_budget_mb << "\n";
  resolved << "  },\n";
  resolved << "  \"environment\": " << BuildEnvJson() << ",\n";
  resolved << "  \"raw_inference\": {\n";
  resolved << "    \"initialized\": " << (initialize_ok ? "true" : "false") << ",\n";
  resolved << "    \"run_ok\": " << (raw_ok ? "true" : "false") << ",\n";
  resolved << "    \"error\": \"" << JsonEscape(initialize_ok ? raw_error : initialize_error) << "\",\n";
  resolved << "    \"backend\": " << BuildBackendJson(raw_backend) << ",\n";
  resolved << "    \"stats\": " << BuildStatsJson(raw_stats) << ",\n";
  resolved << "    \"artifacts\": {\n";
  resolved << "      \"png\": \"" << JsonEscape(raw_png_ok ? PathString(raw_png_path) : std::string()) << "\",\n";
  resolved << "      \"npy\": \"" << JsonEscape(raw_npy_ok ? PathString(raw_npy_path) : std::string()) << "\"\n";
  resolved << "    }\n";
  resolved << "  },\n";
  resolved << "  \"pipeline\": {\n";
  resolved << "    \"status\": \"" << JsonEscape(RenderStatusName(pipeline_output.status)) << "\",\n";
  resolved << "    \"message\": \"" << JsonEscape(pipeline_output.message) << "\",\n";
  resolved << "    \"backend\": " << BuildBackendJson(pipeline_backend) << ",\n";
  resolved << "    \"stats\": " << BuildStatsJson(pipeline_stats) << ",\n";
  resolved << "    \"artifacts\": {\n";
  resolved << "      \"png\": \""
           << JsonEscape(pipeline_png_ok ? PathString(pipeline_png_path) : std::string()) << "\",\n";
  resolved << "      \"npy\": \""
           << JsonEscape(pipeline_npy_ok ? PathString(pipeline_npy_path) : std::string()) << "\"\n";
  resolved << "    }\n";
  resolved << "  },\n";
  resolved << "  \"trace_tail\": {\n";
  resolved << "    \"path\": \"" << JsonEscape(PathString(trace_tail_path)) << "\",\n";
  resolved << "    \"write_error\": \"" << JsonEscape(trace_error) << "\"\n";
  resolved << "  }\n";
  resolved << "}\n";

  if (!WriteTextFile(config_path, resolved.str(), &error)) {
    std::cerr << error << "\n";
    return 1;
  }

  if (!options.quiet) {
    std::cout << "input: " << PathString(options.input_path) << "\n";
    std::cout << "output_dir: " << PathString(options.output_dir) << "\n";
    std::cout << "raw_ok: " << (raw_ok ? "true" : "false")
              << ", pipeline_status: " << RenderStatusName(pipeline_output.status) << "\n";
    std::cout << "config: " << PathString(config_path) << "\n";
  }

  return 0;
}
#endif
