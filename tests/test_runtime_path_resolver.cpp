#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "inference/RuntimePathResolver.h"

namespace {

class TempDir {
 public:
  TempDir() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
    path_ = std::filesystem::temp_directory_path() /
            ("zsoda-runtime-path-test-" + std::to_string(stamp) + "-" + std::to_string(id));
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteTextFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path);
  assert(stream.is_open());
  stream << content;
}

void TestEnvironmentOverridesPluginDefaults() {
  zsoda::inference::RuntimePathHints hints;
  hints.model_root_env = "/env/models";
  hints.model_manifest_env = "/env/models.custom.manifest";
  hints.onnxruntime_library_env = "/env/runtime/onnxruntime.custom";
  hints.plugin_directory = "/plugin/dir";

  const auto resolved = zsoda::inference::ResolveRuntimePaths(hints);
  assert(resolved.model_root == "/env/models");
  assert(resolved.model_manifest_path == "/env/models.custom.manifest");
  assert(resolved.onnxruntime_library_path == "/env/runtime/onnxruntime.custom");
  assert(resolved.onnxruntime_library_dir == "/env/runtime");
}

void TestPluginAdjacentDefaults() {
  TempDir temp_dir;
  const auto plugin_dir = temp_dir.path() / "plugin";
  const auto models_dir = plugin_dir / "models";
  const auto runtime_dir = plugin_dir / "runtime";
  const auto manifest_path = models_dir / "models.manifest";
  const auto ort_runtime_path = runtime_dir / zsoda::inference::DefaultOnnxRuntimeLibraryFileName();

  WriteTextFile(manifest_path, "# test");
  WriteTextFile(ort_runtime_path, "dll");

  zsoda::inference::RuntimePathHints hints;
  hints.plugin_directory = plugin_dir.string();

  const auto resolved = zsoda::inference::ResolveRuntimePaths(hints);
  assert(std::filesystem::path(resolved.model_root) == models_dir);
  assert(std::filesystem::path(resolved.model_manifest_path) == manifest_path);
  assert(std::filesystem::path(resolved.onnxruntime_library_path) == ort_runtime_path);
  assert(std::filesystem::path(resolved.onnxruntime_library_dir) == runtime_dir);
}

void TestPluginRootOrtFallback() {
  TempDir temp_dir;
  const auto plugin_dir = temp_dir.path() / "plugin";
  const auto models_dir = plugin_dir / "models";
  const auto ort_path = plugin_dir / zsoda::inference::DefaultOnnxRuntimeLibraryFileName();

  std::filesystem::create_directories(models_dir);
  WriteTextFile(ort_path, "dll");

  zsoda::inference::RuntimePathHints hints;
  hints.plugin_directory = plugin_dir.string();

  const auto resolved = zsoda::inference::ResolveRuntimePaths(hints);
  assert(std::filesystem::path(resolved.model_root) == models_dir);
  assert(std::filesystem::path(resolved.onnxruntime_library_path) == ort_path);
  assert(std::filesystem::path(resolved.onnxruntime_library_dir) == plugin_dir);
}

void TestPluginIsolatedOrtPreferredOverRoot() {
  TempDir temp_dir;
  const auto plugin_dir = temp_dir.path() / "plugin";
  const auto models_dir = plugin_dir / "models";
  const auto root_ort_path = plugin_dir / zsoda::inference::DefaultOnnxRuntimeLibraryFileName();
  const auto isolated_ort_path =
      plugin_dir / "zsoda_ort" / zsoda::inference::DefaultOnnxRuntimeLibraryFileName();

  std::filesystem::create_directories(models_dir);
  WriteTextFile(root_ort_path, "root-dll");
  WriteTextFile(isolated_ort_path, "isolated-dll");

  zsoda::inference::RuntimePathHints hints;
  hints.plugin_directory = plugin_dir.string();

  const auto resolved = zsoda::inference::ResolveRuntimePaths(hints);
  assert(std::filesystem::path(resolved.onnxruntime_library_path) == isolated_ort_path);
  assert(std::filesystem::path(resolved.onnxruntime_library_dir) == isolated_ort_path.parent_path());
}

void TestEnvironmentOrtPathPrefersSiblingIsolatedDirectory() {
  TempDir temp_dir;
  const auto env_root = temp_dir.path() / "env";
  const auto configured_ort_path = env_root / "onnxruntime.dll";
  const auto isolated_ort_path = env_root / "zsoda_ort" / "onnxruntime.dll";

  WriteTextFile(configured_ort_path, "configured-dll");
  WriteTextFile(isolated_ort_path, "isolated-dll");

  zsoda::inference::RuntimePathHints hints;
  hints.onnxruntime_library_env = configured_ort_path.string();

  const auto resolved = zsoda::inference::ResolveRuntimePaths(hints);
  assert(std::filesystem::path(resolved.onnxruntime_library_path) == isolated_ort_path);
  assert(std::filesystem::path(resolved.onnxruntime_library_dir) == isolated_ort_path.parent_path());
}

void TestFallbackToRepositoryRelativeDefaults() {
  zsoda::inference::RuntimePathHints hints;
  const auto resolved = zsoda::inference::ResolveRuntimePaths(hints);
  assert(resolved.model_root == "models");
  const auto default_manifest = std::filesystem::path("models") / "models.manifest";
  std::error_code ec;
  if (std::filesystem::is_regular_file(default_manifest, ec)) {
    assert(std::filesystem::path(resolved.model_manifest_path) == default_manifest);
  } else {
    assert(resolved.model_manifest_path.empty());
  }
  assert(resolved.onnxruntime_library_path.empty());
  assert(resolved.onnxruntime_library_dir.empty());
}

void TestPluginDirectoryWithoutModelsUsesAbsoluteFallback() {
  TempDir temp_dir;
  const auto plugin_dir = temp_dir.path() / "plugin";
  std::filesystem::create_directories(plugin_dir);

  zsoda::inference::RuntimePathHints hints;
  hints.plugin_directory = plugin_dir.string();

  const auto resolved = zsoda::inference::ResolveRuntimePaths(hints);
  assert(std::filesystem::path(resolved.model_root) == (plugin_dir / "models"));
}

void TestEffectsInstallFallsBackToAdobeMediaCoreModels() {
  TempDir temp_dir;
  const auto adobe_root = temp_dir.path() / "Program Files" / "Adobe";
  const auto effects_plugin_dir =
      adobe_root / "Adobe After Effects 2025" / "Support Files" / "Plug-ins" / "Effects";
  const auto media_core_models = adobe_root / "Common" / "Plug-ins" / "7.0" / "MediaCore" / "models";
  const auto manifest = media_core_models / "models.manifest";

  std::filesystem::create_directories(effects_plugin_dir);
  WriteTextFile(manifest, "# media core manifest");

  zsoda::inference::RuntimePathHints hints;
  hints.plugin_directory = effects_plugin_dir.string();

  const auto resolved = zsoda::inference::ResolveRuntimePaths(hints);
  assert(std::filesystem::path(resolved.model_root) == media_core_models);
  assert(std::filesystem::path(resolved.model_manifest_path) == manifest);
}

void TestMacBundleModuleDirectoryUsesResources() {
  TempDir temp_dir;
  const auto bundle_root = temp_dir.path() / "MediaCore" / "ZSoda.plugin";
  const auto module_dir = bundle_root / "Contents" / "MacOS";
  const auto resources_dir = bundle_root / "Contents" / "Resources";
  const auto models_dir = resources_dir / "models";
  const auto manifest_path = models_dir / "models.manifest";
  const auto ort_path =
      resources_dir / "zsoda_ort" / zsoda::inference::DefaultOnnxRuntimeLibraryFileName();

  std::filesystem::create_directories(module_dir);
  WriteTextFile(manifest_path, "# bundle manifest");
  WriteTextFile(ort_path, "bundle-runtime");

  zsoda::inference::RuntimePathHints hints;
  hints.plugin_directory = module_dir.string();

  const auto resolved = zsoda::inference::ResolveRuntimePaths(hints);
  assert(std::filesystem::path(resolved.model_root) == models_dir);
  assert(std::filesystem::path(resolved.model_manifest_path) == manifest_path);
  assert(std::filesystem::path(resolved.onnxruntime_library_path) == ort_path);
  assert(std::filesystem::path(resolved.onnxruntime_library_dir) == ort_path.parent_path());
}

void TestEmbeddedPayloadAssetRootTakesPrecedence() {
  TempDir temp_dir;
  const auto plugin_dir = temp_dir.path() / "plugin";
  const auto embedded_root = temp_dir.path() / "payload-cache" / "deadbeef";
  const auto embedded_models_dir = embedded_root / "models";
  const auto embedded_manifest = embedded_models_dir / "models.manifest";
  const auto embedded_ort =
      embedded_root / "zsoda_ort" / zsoda::inference::DefaultOnnxRuntimeLibraryFileName();
  const auto adjacent_models_dir = plugin_dir / "models";

  std::filesystem::create_directories(plugin_dir);
  WriteTextFile(adjacent_models_dir / "models.manifest", "# plugin manifest");
  WriteTextFile(embedded_manifest, "# embedded manifest");
  WriteTextFile(embedded_ort, "embedded runtime");

  zsoda::inference::RuntimePathHints hints;
  hints.plugin_directory = plugin_dir.string();
  hints.bundled_asset_root = embedded_root.string();

  const auto resolved = zsoda::inference::ResolveRuntimePaths(hints);
  assert(std::filesystem::path(resolved.model_root) == embedded_models_dir);
  assert(std::filesystem::path(resolved.model_manifest_path) == embedded_manifest);
  assert(std::filesystem::path(resolved.onnxruntime_library_path) == embedded_ort);
  assert(std::filesystem::path(resolved.onnxruntime_library_dir) == embedded_ort.parent_path());
}

}  // namespace

void RunRuntimePathResolverTests() {
  TestEnvironmentOverridesPluginDefaults();
  TestPluginAdjacentDefaults();
  TestPluginRootOrtFallback();
  TestPluginIsolatedOrtPreferredOverRoot();
  TestEnvironmentOrtPathPrefersSiblingIsolatedDirectory();
  TestFallbackToRepositoryRelativeDefaults();
  TestPluginDirectoryWithoutModelsUsesAbsoluteFallback();
  TestEffectsInstallFallsBackToAdobeMediaCoreModels();
  TestMacBundleModuleDirectoryUsesResources();
  TestEmbeddedPayloadAssetRootTakesPrecedence();
}
