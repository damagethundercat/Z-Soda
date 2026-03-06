#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace zsoda::inference {

enum class RuntimeBackend {
  kAuto,
  kCpu,
  kTensorRT,
  kCuda,
  kDirectML,
  kMetal,
  kCoreML,
  kRemote,
};

enum class PreprocessResizeMode {
  kUpperBoundLetterbox,
  kLowerBoundCenterCrop,
  kStretch,
};

struct RuntimeOptions {
  RuntimeBackend preferred_backend = RuntimeBackend::kAuto;
  PreprocessResizeMode preprocess_resize_mode = PreprocessResizeMode::kUpperBoundLetterbox;
  std::string model_manifest_path;
  std::string onnxruntime_library_path;
  std::string onnxruntime_library_dir;
  int onnxruntime_api_version = 0;
  bool auto_download_missing_models = true;
  bool remote_inference_enabled = false;
  std::string remote_endpoint;
  std::string remote_api_key;
  int remote_timeout_ms = 0;
};

[[nodiscard]] inline const char* RuntimeBackendName(RuntimeBackend backend) {
  switch (backend) {
    case RuntimeBackend::kAuto:
      return "auto";
    case RuntimeBackend::kCpu:
      return "cpu";
    case RuntimeBackend::kTensorRT:
      return "tensorrt";
    case RuntimeBackend::kCuda:
      return "cuda";
    case RuntimeBackend::kDirectML:
      return "directml";
    case RuntimeBackend::kMetal:
      return "metal";
    case RuntimeBackend::kCoreML:
      return "coreml";
    case RuntimeBackend::kRemote:
      return "remote";
  }
  return "auto";
}

[[nodiscard]] inline RuntimeBackend ParseRuntimeBackend(std::string_view value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (const char ch : value) {
    if (ch == '-' || ch == '_' || std::isspace(static_cast<unsigned char>(ch))) {
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  if (normalized == "cpu") {
    return RuntimeBackend::kCpu;
  }
  if (normalized == "tensorrt" || normalized == "trt") {
    return RuntimeBackend::kTensorRT;
  }
  if (normalized == "cuda") {
    return RuntimeBackend::kCuda;
  }
  if (normalized == "directml" || normalized == "dml") {
    return RuntimeBackend::kDirectML;
  }
  if (normalized == "metal") {
    return RuntimeBackend::kMetal;
  }
  if (normalized == "coreml") {
    return RuntimeBackend::kCoreML;
  }
  if (normalized == "remote") {
    return RuntimeBackend::kRemote;
  }
  return RuntimeBackend::kAuto;
}

[[nodiscard]] inline const char* PreprocessResizeModeName(PreprocessResizeMode mode) {
  switch (mode) {
    case PreprocessResizeMode::kUpperBoundLetterbox:
      return "upper_bound_letterbox";
    case PreprocessResizeMode::kLowerBoundCenterCrop:
      return "lower_bound_center_crop";
    case PreprocessResizeMode::kStretch:
      return "stretch";
  }
  return "upper_bound_letterbox";
}

[[nodiscard]] inline PreprocessResizeMode ParsePreprocessResizeMode(std::string_view value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (const char ch : value) {
    if (ch == '-' || ch == '_' || ch == '+' || std::isspace(static_cast<unsigned char>(ch))) {
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  if (normalized == "upperboundletterbox" || normalized == "upperbound" ||
      normalized == "letterbox" || normalized == "upper") {
    return PreprocessResizeMode::kUpperBoundLetterbox;
  }
  if (normalized == "lowerboundcentercrop" || normalized == "lowerboundcrop" ||
      normalized == "lowerbound" || normalized == "lower" ||
      normalized == "centercrop" || normalized == "crop") {
    return PreprocessResizeMode::kLowerBoundCenterCrop;
  }
  if (normalized == "stretch" || normalized == "fill" || normalized == "directresize") {
    return PreprocessResizeMode::kStretch;
  }
  return PreprocessResizeMode::kUpperBoundLetterbox;
}

}  // namespace zsoda::inference
