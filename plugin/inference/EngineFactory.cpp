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

#include "inference/DummyInferenceEngine.h"
#include "inference/ManagedInferenceEngine.h"
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

std::string ResolveBundledDistillServiceScriptPath(const RuntimeOptions& options) {
  if (options.plugin_directory.empty()) {
    return {};
  }

  const std::filesystem::path plugin_dir(options.plugin_directory);
  const std::array<std::filesystem::path, 3> candidates = {
      plugin_dir / "zsoda_py" / "distill_any_depth_remote_service.py",
      plugin_dir / "tools" / "distill_any_depth_remote_service.py",
      plugin_dir / "distill_any_depth_remote_service.py",
  };
  for (const auto& candidate : candidates) {
    if (IsExistingFile(candidate)) {
      return candidate.string();
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

  std::string module_dir_error;
  path_hints.plugin_directory = TryResolveCurrentModuleDirectory(&module_dir_error);
  const RuntimePathResolution runtime_paths = ResolveRuntimePaths(path_hints);
  const std::string model_root = runtime_paths.model_root.empty() ? "models" : runtime_paths.model_root;
  const std::string locked_model_id = ResolveLockedModelIdOrDefault();

  RuntimeOptions options;
  const std::string env_backend = ReadEnvOrEmpty("ZSODA_INFERENCE_BACKEND");
  const bool has_explicit_backend = !env_backend.empty();
  const bool has_explicit_remote_enable = HasExplicitEnvValue("ZSODA_REMOTE_INFERENCE_ENABLED");
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
  options.remote_service_python = ReadEnvOrEmpty("ZSODA_REMOTE_SERVICE_PYTHON");
  options.remote_service_script_path = ReadEnvOrEmpty("ZSODA_REMOTE_SERVICE_SCRIPT");
  options.remote_service_log_path = ReadEnvOrEmpty("ZSODA_REMOTE_SERVICE_LOG");
  if (path_hints.plugin_directory.has_value()) {
    options.plugin_directory = *path_hints.plugin_directory;
  }

  const bool implicit_distill_remote_default =
      !has_explicit_backend && IsDistillAnyDepthModelId(locked_model_id) &&
      (!has_explicit_remote_enable || options.remote_inference_enabled);
  if (implicit_distill_remote_default) {
    options.preferred_backend = RuntimeBackend::kRemote;
    options.remote_inference_enabled = true;
    if (options.remote_endpoint.empty() && !has_explicit_remote_autostart) {
      options.remote_service_autostart = true;
    }
    if (options.remote_service_script_path.empty()) {
      options.remote_service_script_path = ResolveBundledDistillServiceScriptPath(options);
    }
  }

  if (options.preferred_backend == RuntimeBackend::kRemote) {
    options.remote_inference_enabled = true;
    if (options.remote_endpoint.empty()) {
      options.remote_service_autostart =
          ParseBoolEnvOrDefault(std::getenv("ZSODA_REMOTE_SERVICE_AUTOSTART"), true);
    }
  } else if (!has_explicit_backend &&
             (options.remote_inference_enabled || !options.remote_endpoint.empty())) {
    options.preferred_backend = RuntimeBackend::kRemote;
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
  auto engine = std::make_shared<ManagedInferenceEngine>(model_root, options);
  std::string error;
  if (engine->Initialize("", &error)) {
    return engine;
  }

  // Log why the managed engine failed so the user can diagnose runtime issues.
  std::fprintf(stderr,
               "[Z-Soda] ManagedInferenceEngine init failed, falling back to DummyInferenceEngine: %s\n",
               error.empty() ? "<no error detail>" : error.c_str());

  auto fallback = std::make_shared<DummyInferenceEngine>();
  if (!fallback->Initialize("dummy", &error)) {
    return nullptr;
  }
  return fallback;
}

}  // namespace zsoda::inference
