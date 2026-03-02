#include "inference/InferenceEngine.h"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <memory>

#include "inference/DummyInferenceEngine.h"
#include "inference/ManagedInferenceEngine.h"
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

}  // namespace

std::shared_ptr<IInferenceEngine> CreateDefaultEngine() {
  const char* env_model_root = std::getenv("ZSODA_MODEL_ROOT");
  const std::string model_root = (env_model_root != nullptr && env_model_root[0] != '\0')
                                     ? env_model_root
                                     : "models";

  RuntimeOptions options;
  const char* env_backend = std::getenv("ZSODA_INFERENCE_BACKEND");
  if (env_backend != nullptr && env_backend[0] != '\0') {
    options.preferred_backend = ParseRuntimeBackend(env_backend);
  }

  const char* env_manifest_path = std::getenv("ZSODA_MODEL_MANIFEST");
  if (env_manifest_path != nullptr && env_manifest_path[0] != '\0') {
    options.model_manifest_path = env_manifest_path;
  }

  const char* env_ort_library = std::getenv("ZSODA_ONNXRUNTIME_LIBRARY");
  if (env_ort_library != nullptr && env_ort_library[0] != '\0') {
    options.onnxruntime_library_path = env_ort_library;
  }

  const char* env_ort_api = std::getenv("ZSODA_ONNXRUNTIME_API_VERSION");
  options.onnxruntime_api_version = ParsePositiveIntEnvOrDefault(env_ort_api, 0);

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
