#include "inference/InferenceEngine.h"

#include <cstdlib>
#include <memory>

#include "inference/DummyInferenceEngine.h"
#include "inference/ManagedInferenceEngine.h"
#include "inference/RuntimeOptions.h"

namespace zsoda::inference {

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
