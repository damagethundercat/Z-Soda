#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace zsoda::inference {

enum class RuntimeBackend {
  kAuto,
  kCpu,
  kCuda,
  kDirectML,
  kMetal,
  kCoreML,
};

struct RuntimeOptions {
  RuntimeBackend preferred_backend = RuntimeBackend::kAuto;
  std::string model_manifest_path;
  std::string onnxruntime_library_path;
  int onnxruntime_api_version = 0;
};

[[nodiscard]] inline const char* RuntimeBackendName(RuntimeBackend backend) {
  switch (backend) {
    case RuntimeBackend::kAuto:
      return "auto";
    case RuntimeBackend::kCpu:
      return "cpu";
    case RuntimeBackend::kCuda:
      return "cuda";
    case RuntimeBackend::kDirectML:
      return "directml";
    case RuntimeBackend::kMetal:
      return "metal";
    case RuntimeBackend::kCoreML:
      return "coreml";
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
  return RuntimeBackend::kAuto;
}

}  // namespace zsoda::inference
