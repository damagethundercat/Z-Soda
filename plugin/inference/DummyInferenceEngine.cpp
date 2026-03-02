#include "inference/DummyInferenceEngine.h"

#include <algorithm>

namespace zsoda::inference {

bool DummyInferenceEngine::Initialize(const std::string& model_id, std::string* error) {
  if (model_id.empty()) {
    if (error) {
      *error = "model id cannot be empty";
    }
    return false;
  }
  model_id_ = model_id;
  return true;
}

bool DummyInferenceEngine::SelectModel(const std::string& model_id, std::string* error) {
  return Initialize(model_id, error);
}

std::vector<std::string> DummyInferenceEngine::ListModelIds() const {
  return {model_id_};
}

std::string DummyInferenceEngine::ActiveModelId() const {
  return model_id_;
}

bool DummyInferenceEngine::Run(const InferenceRequest& request,
                               zsoda::core::FrameBuffer* out_depth,
                               std::string* error) const {
  if (request.source == nullptr || out_depth == nullptr) {
    if (error) {
      *error = "invalid inference request";
    }
    return false;
  }

  const auto& src = *request.source;
  if (src.empty()) {
    if (error) {
      *error = "source frame is empty";
    }
    return false;
  }

  auto desc = src.desc();
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;
  out_depth->Resize(desc);

  const float quality_scale = std::clamp(static_cast<float>(request.quality), 1.0F, 3.0F);
  const float inv_w = 1.0F / static_cast<float>(std::max(1, desc.width - 1));
  const float inv_h = 1.0F / static_cast<float>(std::max(1, desc.height - 1));

  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      const float fx = static_cast<float>(x) * inv_w;
      const float fy = static_cast<float>(y) * inv_h;
      out_depth->at(x, y, 0) = (fx * 0.6F + fy * 0.4F) / quality_scale;
    }
  }
  return true;
}

}  // namespace zsoda::inference
