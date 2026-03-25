#include "inference/InferenceEngine.h"

#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "inference/EmbeddedPayload.h"
#include "inference/ManagedInferenceEngine.h"
#include "inference/ModelCatalog.h"
#include "inference/RuntimePathResolver.h"
#include "inference/RuntimeOptions.h"

namespace zsoda::inference {
namespace {

constexpr char kDefaultLockedModelId[] = "distill-any-depth-base";

int ParsePositiveIntEnvOrDefault(const char* value, int default_value) {
  if (value == nullptr || value[0] == '\0') {
    return default_value;
  }
  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (errno != 0 || end == value || (end != nullptr && *end != '\0') || parsed <= 0 ||
      parsed > std::numeric_limits<int>::max()) {
    return default_value;
  }
  return static_cast<int>(parsed);
}

bool ParseBoolEnvOrDefault(const char* value, bool default_value) {
  if (value == nullptr || value[0] == '\0') {
    return default_value;
  }
  std::string normalized;
  for (const char ch : std::string(value)) {
    if (ch == '-' || ch == '_' || ch == ' ' || ch == '\t') {
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
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
  if (value == nullptr || value[0] == '\0') {
    return {};
  }
  return value;
}

std::string ReadEnvWithFallback(const char* primary, const char* fallback) {
  std::string value = ReadEnvOrEmpty(primary);
  if (!value.empty()) {
    return value;
  }
  return ReadEnvOrEmpty(fallback);
}

bool HasExplicitEnvValue(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0';
}

std::string ResolveLockedModelIdOrDefault() {
  std::string model_id = ReadEnvOrEmpty("ZSODA_LOCKED_MODEL_ID");
  if (model_id.empty()) {
    // Legacy alias kept for backwards compatibility with older local setups.
    model_id = ReadEnvOrEmpty("ZSODA_HQ_MODEL_ID");
  }
  if (model_id.empty()) {
    model_id = kDefaultLockedModelId;
  }
  return model_id;
}

bool IsDistillAnyDepthModelId(std::string_view model_id) {
  return model_id.rfind("distill-any-depth", 0) == 0;
}

bool IsExistingFile(const std::filesystem::path& path) {
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) && !ec;
}

bool AreResolvedModelAssetsPresent(const std::vector<ResolvedModelAsset>& assets) {
  if (assets.empty()) {
    return false;
  }
  for (const auto& asset : assets) {
    if (!IsExistingFile(std::filesystem::path(asset.absolute_path))) {
      return false;
    }
  }
  return true;
}

bool HasNativeOnnxModelAssets(const RuntimePathResolution& runtime_paths,
                              std::string_view model_id) {
  if (runtime_paths.onnxruntime_library_path.empty() || runtime_paths.model_root.empty()) {
    return false;
  }

  ModelCatalog catalog;
  return AreResolvedModelAssetsPresent(
      catalog.ResolveModelAssets(runtime_paths.model_root, std::string(model_id)));
}

std::string ResolveBundledDistillServiceScriptPath(const RuntimeOptions& options) {
  if (options.plugin_directory.empty() && options.runtime_asset_root.empty()) {
    return {};
  }

  std::vector<std::filesystem::path> search_roots;
  if (!options.plugin_directory.empty()) {
    const std::filesystem::path plugin_dir(options.plugin_directory);
    search_roots = BuildRuntimeAssetSearchRoots(plugin_dir, std::filesystem::path(options.runtime_asset_root));
  } else {
    search_roots.push_back(std::filesystem::path(options.runtime_asset_root));
  }

  for (const auto& root : search_roots) {
    const std::array<std::filesystem::path, 3> candidates = {
        root / "zsoda_py" / "distill_any_depth_remote_service.py",
        root / "tools" / "distill_any_depth_remote_service.py",
        root / "distill_any_depth_remote_service.py",
    };
    for (const auto& candidate : candidates) {
      if (IsExistingFile(candidate)) {
        return candidate.string();
      }
    }
  }
  return {};
}

}  // namespace

std::shared_ptr<IInferenceEngine> CreateDefaultEngine() {
  RuntimePathHints path_hints;
  path_hints.model_root_env = ReadEnvOrEmpty("ZSODA_MODEL_ROOT");
  path_hints.model_manifest_env = ReadEnvOrEmpty("ZSODA_MODEL_MANIFEST");
  path_hints.onnxruntime_library_env = ReadEnvOrEmpty("ZSODA_ONNXRUNTIME_LIBRARY");
  const std::string locked_model_id = ResolveLockedModelIdOrDefault();

  std::string module_path_error;
  const auto module_path = TryResolveCurrentModulePath(&module_path_error);
  std::string module_dir_error;
  path_hints.plugin_directory = TryResolveCurrentModuleDirectory(&module_dir_error);
  RuntimePathResolution runtime_paths = ResolveRuntimePaths(path_hints);
  const bool has_native_sidecar_assets =
      !runtime_paths.onnxruntime_library_path.empty() &&
      HasNativeOnnxModelAssets(runtime_paths, locked_model_id);
  if (!has_native_sidecar_assets && module_path.has_value() && !module_path->empty()) {
    std::string payload_error;
    const EmbeddedPayloadInfo payload =
        EnsureEmbeddedPayloadAvailable(*module_path, &payload_error);
    if (payload.extracted && !payload.asset_root.empty()) {
      path_hints.bundled_asset_root = payload.asset_root;
      runtime_paths = ResolveRuntimePaths(path_hints);
    } else if (!payload_error.empty()) {
      std::fprintf(stderr,
                   "[Z-Soda] Embedded payload preparation failed: %s\n",
                   payload_error.c_str());
    }
  }
  const std::string model_root = runtime_paths.model_root.empty() ? "models" : runtime_paths.model_root;

  RuntimeOptions options;
  const std::string env_backend = ReadEnvOrEmpty("ZSODA_INFERENCE_BACKEND");
  const bool has_explicit_remote_autostart = HasExplicitEnvValue("ZSODA_REMOTE_SERVICE_AUTOSTART");
  if (!env_backend.empty()) {
    options.preferred_backend = ParseRuntimeBackend(env_backend);
  }
  const std::string env_preprocess_resize_mode = ReadEnvOrEmpty("ZSODA_PREPROCESS_RESIZE_MODE");
  if (!env_preprocess_resize_mode.empty()) {
    options.preprocess_resize_mode = ParsePreprocessResizeMode(env_preprocess_resize_mode);
  }
  const std::string env_remote_protocol = ReadEnvOrEmpty("ZSODA_REMOTE_PROTOCOL");
  if (!env_remote_protocol.empty()) {
    options.remote_transport_protocol = ParseRemoteTransportProtocol(env_remote_protocol);
  }

  options.remote_endpoint =
      ReadEnvWithFallback("ZSODA_REMOTE_INFERENCE_ENDPOINT", "ZSODA_REMOTE_INFERENCE_URL");
  options.remote_api_key = ReadEnvWithFallback("ZSODA_REMOTE_INFERENCE_API_KEY", "ZSODA_REMOTE_API_KEY");
  options.remote_inference_enabled =
      ParseBoolEnvOrDefault(std::getenv("ZSODA_REMOTE_INFERENCE_ENABLED"), false);
  options.remote_timeout_ms =
      ParsePositiveIntEnvOrDefault(std::getenv("ZSODA_REMOTE_INFERENCE_TIMEOUT_MS"), 0);
  options.remote_service_autostart =
      ParseBoolEnvOrDefault(std::getenv("ZSODA_REMOTE_SERVICE_AUTOSTART"), false);
  options.remote_service_host = ReadEnvOrEmpty("ZSODA_REMOTE_SERVICE_HOST");
  if (options.remote_service_host.empty()) {
    options.remote_service_host = "127.0.0.1";
  }
  options.remote_service_port =
      ParsePositiveIntEnvOrDefault(std::getenv("ZSODA_REMOTE_SERVICE_PORT"), 8345);
  options.remote_service_port_explicit = HasExplicitEnvValue("ZSODA_REMOTE_SERVICE_PORT");
  options.remote_service_python = ReadEnvOrEmpty("ZSODA_REMOTE_SERVICE_PYTHON");
  options.remote_service_script_path = ReadEnvOrEmpty("ZSODA_REMOTE_SERVICE_SCRIPT");
  options.remote_service_log_path = ReadEnvOrEmpty("ZSODA_REMOTE_SERVICE_LOG");
  if (path_hints.plugin_directory.has_value()) {
    options.plugin_directory = *path_hints.plugin_directory;
  }
  if (!path_hints.bundled_asset_root.empty()) {
    options.runtime_asset_root = path_hints.bundled_asset_root;
  }

  const bool allows_remote_backend =
      options.preferred_backend == RuntimeBackend::kRemote || options.remote_inference_enabled ||
      !options.remote_endpoint.empty() || !options.remote_service_python.empty() ||
      !options.remote_service_script_path.empty() || options.remote_service_autostart;

  if ((options.preferred_backend == RuntimeBackend::kRemote || options.remote_inference_enabled) &&
      options.remote_endpoint.empty() && !has_explicit_remote_autostart) {
    options.remote_service_autostart =
        ParseBoolEnvOrDefault(std::getenv("ZSODA_REMOTE_SERVICE_AUTOSTART"), true);
  }

  if (allows_remote_backend && options.remote_service_script_path.empty() &&
      IsDistillAnyDepthModelId(locked_model_id) &&
      (options.preferred_backend == RuntimeBackend::kRemote ||
       !HasNativeOnnxModelAssets(runtime_paths, locked_model_id))) {
    options.remote_service_script_path = ResolveBundledDistillServiceScriptPath(options);
  }

  if (options.preferred_backend == RuntimeBackend::kRemote) {
    options.remote_inference_enabled = true;
  }

  if (!runtime_paths.model_manifest_path.empty()) {
    options.model_manifest_path = runtime_paths.model_manifest_path;
  }

  if (!runtime_paths.onnxruntime_library_path.empty()) {
    options.onnxruntime_library_path = runtime_paths.onnxruntime_library_path;
  }
  if (!runtime_paths.onnxruntime_library_dir.empty()) {
    options.onnxruntime_library_dir = runtime_paths.onnxruntime_library_dir;
  }

  const char* env_ort_api = std::getenv("ZSODA_ONNXRUNTIME_API_VERSION");
  options.onnxruntime_api_version = ParsePositiveIntEnvOrDefault(env_ort_api, 0);
  const char* env_auto_download = std::getenv("ZSODA_AUTO_DOWNLOAD_MODELS");
  options.auto_download_missing_models = ParseBoolEnvOrDefault(env_auto_download, true);

  (void)module_dir_error;
  (void)module_path_error;
  auto engine = std::make_shared<ManagedInferenceEngine>(model_root, options);
  std::string error;
  if (engine->Initialize("", &error)) {
    return engine;
  }

  // Keep the managed engine alive so runtime/setup failures surface as safe output
  // instead of an implicit dummy-depth success path.
  std::fprintf(stderr,
               "[Z-Soda] ManagedInferenceEngine init failed; keeping managed engine active: %s\n",
               error.empty() ? "<no error detail>" : error.c_str());
  return engine;
}

}  // namespace zsoda::inference
