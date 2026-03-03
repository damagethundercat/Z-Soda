#include "inference/InferenceEngine.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>

#include "inference/DummyInferenceEngine.h"
#include "inference/ManagedInferenceEngine.h"
#include "inference/RuntimePathResolver.h"
#include "inference/RuntimeOptions.h"

namespace zsoda::inference {
namespace {

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

  RuntimeOptions options;
  const std::string env_backend = ReadEnvOrEmpty("ZSODA_INFERENCE_BACKEND");
  const bool has_explicit_backend = !env_backend.empty();
  if (!env_backend.empty()) {
    options.preferred_backend = ParseRuntimeBackend(env_backend);
  }

  options.remote_endpoint =
      ReadEnvWithFallback("ZSODA_REMOTE_INFERENCE_ENDPOINT", "ZSODA_REMOTE_INFERENCE_URL");
  options.remote_api_key = ReadEnvWithFallback("ZSODA_REMOTE_INFERENCE_API_KEY", "ZSODA_REMOTE_API_KEY");
  options.remote_inference_enabled =
      ParseBoolEnvOrDefault(std::getenv("ZSODA_REMOTE_INFERENCE_ENABLED"), false);
  options.remote_timeout_ms =
      ParsePositiveIntEnvOrDefault(std::getenv("ZSODA_REMOTE_INFERENCE_TIMEOUT_MS"), 0);

  if (options.preferred_backend == RuntimeBackend::kRemote) {
    options.remote_inference_enabled = true;
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

  auto fallback = std::make_shared<DummyInferenceEngine>();
  if (!fallback->Initialize("dummy", &error)) {
    return nullptr;
  }
  return fallback;
}

}  // namespace zsoda::inference
