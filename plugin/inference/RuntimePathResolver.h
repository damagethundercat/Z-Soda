#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zsoda::inference {

struct RuntimePathHints {
  std::string model_root_env;
  std::string model_manifest_env;
  std::string onnxruntime_library_env;
  std::string bundled_asset_root;
  std::optional<std::string> plugin_directory;
};

struct RuntimePathResolution {
  std::string model_root;
  std::string model_manifest_path;
  std::string onnxruntime_library_path;
  std::string onnxruntime_library_dir;
};

[[nodiscard]] const char* DefaultOnnxRuntimeLibraryFileName();
[[nodiscard]] std::vector<std::filesystem::path> BuildRuntimeAssetSearchRoots(
    const std::filesystem::path& plugin_directory,
    const std::filesystem::path& extra_asset_root = {});
[[nodiscard]] RuntimePathResolution ResolveRuntimePaths(const RuntimePathHints& hints);
[[nodiscard]] std::optional<std::string> TryResolveCurrentModulePath(std::string* error);
[[nodiscard]] std::optional<std::string> TryResolveCurrentModuleDirectory(std::string* error);

}  // namespace zsoda::inference
