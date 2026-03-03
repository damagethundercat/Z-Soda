#pragma once

#include <optional>
#include <string>

namespace zsoda::inference {

struct RuntimePathHints {
  std::string model_root_env;
  std::string model_manifest_env;
  std::string onnxruntime_library_env;
  std::optional<std::string> plugin_directory;
};

struct RuntimePathResolution {
  std::string model_root;
  std::string model_manifest_path;
  std::string onnxruntime_library_path;
  std::string onnxruntime_library_dir;
};

[[nodiscard]] const char* DefaultOnnxRuntimeLibraryFileName();
[[nodiscard]] RuntimePathResolution ResolveRuntimePaths(const RuntimePathHints& hints);
[[nodiscard]] std::optional<std::string> TryResolveCurrentModuleDirectory(std::string* error);

}  // namespace zsoda::inference
