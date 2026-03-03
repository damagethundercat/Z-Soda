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

}  // namespace

void RunRuntimePathResolverTests() {
  TestEnvironmentOverridesPluginDefaults();
  TestPluginAdjacentDefaults();
  TestPluginRootOrtFallback();
  TestFallbackToRepositoryRelativeDefaults();
}
