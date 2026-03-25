#pragma once

#include <filesystem>
#include <string>

#include "inference/RuntimeOptions.h"

namespace zsoda::inference {

enum class PythonTorchCapability {
  kUnavailable,
  kTorchCpu,
  kTorchCuda,
  kTorchMps,
};

struct PythonAutostartSelection {
  std::filesystem::path python_path;
  PythonTorchCapability capability = PythonTorchCapability::kUnavailable;
  std::string probe_output;
  bool used_explicit_override = false;
};

PythonTorchCapability ProbePythonTorchCapability(const std::filesystem::path& python_path,
                                                 std::string* probe_output);

PythonAutostartSelection ResolvePythonAutostartSelection(const RuntimeOptions& options);

std::string ResolvePythonCommandForAutostart(const RuntimeOptions& options);

}  // namespace zsoda::inference
