#include "inference/InferenceEngine.h"

#include <cstdlib>
#include <memory>

#include "inference/ManagedInferenceEngine.h"

namespace zsoda::inference {

std::shared_ptr<IInferenceEngine> CreateDefaultEngine() {
  const char* env_model_root = std::getenv("ZSODA_MODEL_ROOT");
  const std::string model_root = (env_model_root != nullptr && env_model_root[0] != '\0')
                                     ? env_model_root
                                     : "models";

  auto engine = std::make_shared<ManagedInferenceEngine>(model_root);
  std::string error;
  if (!engine->Initialize("depth-anything-v3-small", &error)) {
    return nullptr;
  }
  return engine;
}

}  // namespace zsoda::inference
