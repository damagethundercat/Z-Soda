#include <cassert>

#include "inference/ManagedInferenceEngine.h"

namespace {

void TestModelList() {
  zsoda::inference::ManagedInferenceEngine engine("models");
  const auto models = engine.ListModelIds();
  assert(!models.empty());
  assert(models.front() == "depth-anything-v3-small");
}

void TestModelSelection() {
  zsoda::inference::ManagedInferenceEngine engine("models");
  std::string error;
  assert(engine.Initialize("depth-anything-v3-small", &error));
  assert(engine.ActiveModelId() == "depth-anything-v3-small");

  error.clear();
  assert(!engine.SelectModel("unknown-model", &error));
  assert(!error.empty());

  error.clear();
  assert(engine.SelectModel("depth-anything-v3-large", &error));
  assert(engine.ActiveModelId() == "depth-anything-v3-large");
}

}  // namespace

void RunInferenceEngineTests() {
  TestModelList();
  TestModelSelection();
}
