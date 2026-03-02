#include "inference/ModelCatalog.h"

#include <filesystem>

namespace zsoda::inference {

ModelCatalog::ModelCatalog() {
  models_.push_back({
      "depth-anything-v3-small",
      "Depth Anything v3 Small",
      "depth-anything-v3/depth_anything_v3_small.onnx",
      "https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_small.onnx",
      true,
  });
  models_.push_back({
      "depth-anything-v3-base",
      "Depth Anything v3 Base",
      "depth-anything-v3/depth_anything_v3_base.onnx",
      "https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_base.onnx",
      false,
  });
  models_.push_back({
      "depth-anything-v3-large",
      "Depth Anything v3 Large",
      "depth-anything-v3/depth_anything_v3_large.onnx",
      "https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_large.onnx",
      false,
  });
  models_.push_back({
      "midas-dpt-large",
      "MiDaS DPT Large",
      "midas/dpt_large_384.onnx",
      "https://github.com/isl-org/MiDaS/releases/download/v3_1/dpt_large_384.onnx",
      false,
  });
}

const std::vector<ModelSpec>& ModelCatalog::models() const {
  return models_;
}

const ModelSpec* ModelCatalog::FindById(std::string_view model_id) const {
  for (const auto& model : models_) {
    if (model.id == model_id) {
      return &model;
    }
  }
  return nullptr;
}

std::string ModelCatalog::DefaultModelId() const {
  for (const auto& model : models_) {
    if (model.preferred_default) {
      return model.id;
    }
  }
  return models_.empty() ? "" : models_.front().id;
}

std::string ModelCatalog::ResolveModelPath(const std::string& model_root,
                                           const std::string& model_id) const {
  const auto* model = FindById(model_id);
  if (model == nullptr) {
    return {};
  }
  return (std::filesystem::path(model_root) / model->relative_path).string();
}

}  // namespace zsoda::inference
