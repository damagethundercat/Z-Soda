#include <cassert>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/RenderPipeline.h"
#include "inference/InferenceEngine.h"

namespace {

class ScriptedInferenceEngine final : public zsoda::inference::IInferenceEngine {
 public:
  enum class Behavior {
    kSucceed,
    kFail,
    kThrow,
  };

  const char* Name() const override { return "ScriptedInferenceEngine"; }

  bool Initialize(const std::string& model_id, std::string* error) override {
    return SelectModel(model_id, error);
  }

  bool SelectModel(const std::string& model_id, std::string* error) override {
    if (model_id.empty()) {
      if (error != nullptr) {
        *error = "model id cannot be empty";
      }
      return false;
    }
    active_model_id_ = model_id;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  std::vector<std::string> ListModelIds() const override {
    return {"depth-anything-v3-small", "depth-anything-v3-large"};
  }

  std::string ActiveModelId() const override { return active_model_id_; }

  bool Run(const zsoda::inference::InferenceRequest& request,
           zsoda::core::FrameBuffer* out_depth,
           std::string* error) const override {
    if (request.source == nullptr || out_depth == nullptr) {
      if (error != nullptr) {
        *error = "invalid inference request";
      }
      return false;
    }

    const int width = request.source->desc().width;
    run_widths_.push_back(width);
    run_qualities_.push_back(request.quality);

    const Rule* rule = &default_rule_;
    const auto it = width_rules_.find(width);
    if (it != width_rules_.end()) {
      rule = &it->second;
    }

    if (rule->behavior == Behavior::kThrow) {
      throw std::runtime_error(rule->error.empty() ? "forced inference exception" : rule->error);
    }
    if (rule->behavior == Behavior::kFail) {
      if (error != nullptr) {
        *error = rule->error.empty() ? "forced inference failure" : rule->error;
      }
      return false;
    }

    auto desc = request.source->desc();
    desc.channels = 1;
    desc.format = zsoda::core::PixelFormat::kGray32F;
    out_depth->Resize(desc);

    const float model_bias = (active_model_id_ == "depth-anything-v3-large") ? 0.65F : 0.25F;
    for (int y = 0; y < desc.height; ++y) {
      for (int x = 0; x < desc.width; ++x) {
        out_depth->at(x, y, 0) = model_bias + static_cast<float>(x + y) * 0.01F;
      }
    }

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  void SetDefaultBehavior(Behavior behavior, std::string error_message = std::string()) {
    default_rule_.behavior = behavior;
    default_rule_.error = std::move(error_message);
  }

  void SetBehaviorForWidth(int width, Behavior behavior, std::string error_message = std::string()) {
    Rule rule;
    rule.behavior = behavior;
    rule.error = std::move(error_message);
    width_rules_[width] = std::move(rule);
  }

  [[nodiscard]] int RunCount() const { return static_cast<int>(run_widths_.size()); }
  [[nodiscard]] const std::vector<int>& RunWidths() const { return run_widths_; }
  [[nodiscard]] const std::vector<int>& RunQualities() const { return run_qualities_; }

 private:
  struct Rule {
    Behavior behavior = Behavior::kSucceed;
    std::string error;
  };

  mutable std::vector<int> run_widths_;
  mutable std::vector<int> run_qualities_;
  Rule default_rule_{};
  std::unordered_map<int, Rule> width_rules_;
  std::string active_model_id_ = "depth-anything-v3-small";
};

class SequenceInferenceEngine final : public zsoda::inference::IInferenceEngine {
 public:
  explicit SequenceInferenceEngine(std::vector<float> sequence)
      : sequence_(std::move(sequence)) {
    if (sequence_.empty()) {
      sequence_.push_back(0.0F);
    }
  }

  const char* Name() const override { return "SequenceInferenceEngine"; }

  bool Initialize(const std::string& model_id, std::string* error) override {
    active_model_id_ = model_id.empty() ? "depth-anything-v3-small" : model_id;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool SelectModel(const std::string& model_id, std::string* error) override {
    return Initialize(model_id, error);
  }

  std::vector<std::string> ListModelIds() const override {
    return {"depth-anything-v3-small"};
  }

  std::string ActiveModelId() const override { return active_model_id_; }

  bool Run(const zsoda::inference::InferenceRequest& request,
           zsoda::core::FrameBuffer* out_depth,
           std::string* error) const override {
    if (request.source == nullptr || out_depth == nullptr) {
      if (error != nullptr) {
        *error = "invalid inference request";
      }
      return false;
    }

    auto desc = request.source->desc();
    desc.channels = 1;
    desc.format = zsoda::core::PixelFormat::kGray32F;
    out_depth->Resize(desc);

    const std::size_t index = std::min(cursor_, sequence_.size() - 1U);
    const float value = sequence_[index];
    for (int y = 0; y < desc.height; ++y) {
      for (int x = 0; x < desc.width; ++x) {
        out_depth->at(x, y, 0) = value;
      }
    }
    if (cursor_ + 1U < sequence_.size()) {
      ++cursor_;
    }
    ++run_count_;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  [[nodiscard]] int RunCount() const { return run_count_; }

 private:
  mutable std::size_t cursor_ = 0U;
  mutable int run_count_ = 0;
  std::vector<float> sequence_;
  std::string active_model_id_ = "depth-anything-v3-small";
};

class LowFrequencyBiasInferenceEngine final : public zsoda::inference::IInferenceEngine {
 public:
  const char* Name() const override { return "LowFrequencyBiasInferenceEngine"; }

  bool Initialize(const std::string& model_id, std::string* error) override {
    active_model_id_ = model_id.empty() ? "depth-anything-v3-small" : model_id;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool SelectModel(const std::string& model_id, std::string* error) override {
    return Initialize(model_id, error);
  }

  std::vector<std::string> ListModelIds() const override {
    return {"depth-anything-v3-small"};
  }

  std::string ActiveModelId() const override { return active_model_id_; }

  bool Run(const zsoda::inference::InferenceRequest& request,
           zsoda::core::FrameBuffer* out_depth,
           std::string* error) const override {
    if (request.source == nullptr || out_depth == nullptr) {
      if (error != nullptr) {
        *error = "invalid inference request";
      }
      return false;
    }

    auto desc = request.source->desc();
    desc.channels = 1;
    desc.format = zsoda::core::PixelFormat::kGray32F;
    out_depth->Resize(desc);

    const bool reference_pass = request.quality <= 2;
    for (int y = 0; y < desc.height; ++y) {
      for (int x = 0; x < desc.width; ++x) {
        if (reference_pass) {
          out_depth->at(x, y, 0) = 0.5F;
          continue;
        }
        const float source_value = request.source->at(x, y, 0);
        out_depth->at(x, y, 0) = 0.5F + source_value * 0.02F;
      }
    }

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

 private:
  std::string active_model_id_ = "depth-anything-v3-small";
};

class HighFrequencyDetailInferenceEngine final : public zsoda::inference::IInferenceEngine {
 public:
  const char* Name() const override { return "HighFrequencyDetailInferenceEngine"; }

  bool Initialize(const std::string& model_id, std::string* error) override {
    active_model_id_ = model_id.empty() ? "depth-anything-v3-small" : model_id;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool SelectModel(const std::string& model_id, std::string* error) override {
    return Initialize(model_id, error);
  }

  std::vector<std::string> ListModelIds() const override {
    return {"depth-anything-v3-small"};
  }

  std::string ActiveModelId() const override { return active_model_id_; }

  bool Run(const zsoda::inference::InferenceRequest& request,
           zsoda::core::FrameBuffer* out_depth,
           std::string* error) const override {
    if (request.source == nullptr || out_depth == nullptr) {
      if (error != nullptr) {
        *error = "invalid inference request";
      }
      return false;
    }

    auto desc = request.source->desc();
    desc.channels = 1;
    desc.format = zsoda::core::PixelFormat::kGray32F;
    out_depth->Resize(desc);

    const bool reference_pass = request.quality <= 2;
    for (int y = 0; y < desc.height; ++y) {
      for (int x = 0; x < desc.width; ++x) {
        if (reference_pass) {
          out_depth->at(x, y, 0) = 0.5F;
          continue;
        }
        const int parity =
            static_cast<int>(std::lround(request.source->at(x, y, 0))) & 1;
        out_depth->at(x, y, 0) = parity == 0 ? 0.35F : 0.65F;
      }
    }

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

 private:
  std::string active_model_id_ = "depth-anything-v3-small";
};

zsoda::core::FrameBuffer MakeSourceFrame(int width, int height) {
  zsoda::core::FrameDesc desc;
  desc.width = width;
  desc.height = height;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;
  zsoda::core::FrameBuffer frame(desc);

  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      frame.at(x, y, 0) = static_cast<float>(x + y);
    }
  }
  return frame;
}

bool Contains(const std::string& source, const std::string& needle) {
  return source.find(needle) != std::string::npos;
}

void AssertFrameAllZero(const zsoda::core::FrameBuffer& frame) {
  const auto& desc = frame.desc();
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      assert(frame.at(x, y, 0) == 0.0F);
    }
  }
}

void TestFallbackSequenceDownscaledAfterTiledFailure() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  engine->SetBehaviorForWidth(10, ScriptedInferenceEngine::Behavior::kFail, "direct stage forced");
  engine->SetBehaviorForWidth(6, ScriptedInferenceEngine::Behavior::kFail, "tile stage forced");

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(10, 10);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 101;
  params.tile_size = 6;
  params.overlap = 0;

  const auto output = pipeline.Render(src, params);
  assert(output.status == zsoda::core::RenderStatus::kFallbackDownscaled);
  assert(Contains(output.message, "downscaled fallback succeeded"));

  const auto& run_widths = engine->RunWidths();
  assert(run_widths.size() >= 3U);
  assert(run_widths[0] == 10);
  assert(run_widths[1] == 6);
  assert(run_widths[2] == 5);
}

void TestAdaptiveFallbackRetriesRecordStageDiagnostics() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  engine->SetBehaviorForWidth(256, ScriptedInferenceEngine::Behavior::kFail, "direct oom");
  engine->SetBehaviorForWidth(200, ScriptedInferenceEngine::Behavior::kFail, "tile[200] oom");
  engine->SetBehaviorForWidth(100, ScriptedInferenceEngine::Behavior::kFail, "tile[100] oom");

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(256, 256);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 102;
  params.quality = 3;
  params.tile_size = 200;
  params.overlap = 0;

  const auto output = pipeline.Render(src, params);
  assert(output.status == zsoda::core::RenderStatus::kFallbackDownscaled);
  assert(Contains(output.message, "direct inference failed: direct oom"));
  assert(Contains(output.message, "tile=200 failed (tile[200] oom)"));
  assert(Contains(output.message, "tile=100 failed (tile[100] oom)"));

  const auto& run_widths = engine->RunWidths();
  const auto& run_qualities = engine->RunQualities();
  assert(run_widths.size() == run_qualities.size());
  assert(run_widths.size() >= 4U);
  assert(run_widths[0] == 256);
  assert(run_widths[1] == 200);
  assert(run_widths[2] == 100);
  assert(run_widths.back() == 128);
  assert(run_qualities[0] == 3);
  assert(run_qualities[1] == 3);
  assert(run_qualities[2] == 3);
  assert(run_qualities.back() == 2);
}

void TestAdaptiveFallbackHandlesInvalidTileConfiguration() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  engine->SetBehaviorForWidth(10, ScriptedInferenceEngine::Behavior::kFail, "direct stage forced");

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(10, 10);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 103;
  params.quality = 2;
  params.tile_size = 0;
  params.overlap = 0;

  const auto output = pipeline.Render(src, params);
  assert(output.status == zsoda::core::RenderStatus::kFallbackTiled);
  assert(Contains(output.message, "tile=1"));
  assert(Contains(output.message, "tiled attempts: tile=1 succeeded"));

  const auto& run_widths = engine->RunWidths();
  const auto& run_qualities = engine->RunQualities();
  assert(run_widths.size() >= 2U);
  assert(run_widths[0] == 10);
  for (std::size_t i = 1; i < run_widths.size(); ++i) {
    assert(run_widths[i] == 1);
  }
  assert(run_qualities.size() == run_widths.size());
  assert(run_qualities[0] == 2);
  for (std::size_t i = 1; i < run_qualities.size(); ++i) {
    assert(run_qualities[i] == 2);
  }
}

void TestDownscaledFallbackUsesVramBudgetHint() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  engine->SetBehaviorForWidth(1024, ScriptedInferenceEngine::Behavior::kFail, "direct oom");
  engine->SetBehaviorForWidth(400, ScriptedInferenceEngine::Behavior::kFail, "tile[400] oom");
  engine->SetBehaviorForWidth(200, ScriptedInferenceEngine::Behavior::kFail, "tile[200] oom");
  engine->SetBehaviorForWidth(100, ScriptedInferenceEngine::Behavior::kFail, "tile[100] oom");

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(1024, 1024);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 104;
  params.quality = 3;
  params.tile_size = 400;
  params.overlap = 0;
  params.vram_budget_mb = 1;

  const auto output = pipeline.Render(src, params);
  assert(output.status == zsoda::core::RenderStatus::kFallbackDownscaled);
  assert(Contains(output.message, "downscaled fallback succeeded"));

  const auto& run_widths = engine->RunWidths();
  const auto& run_qualities = engine->RunQualities();
  assert(run_widths.size() == run_qualities.size());
  assert(run_widths.size() >= 5U);
  assert(run_widths[0] == 1024);
  assert(run_widths[1] == 400);
  assert(run_widths[2] == 200);
  assert(run_widths[3] == 100);
  assert(run_widths.back() == 256);
  assert(run_qualities.back() == 1);
}

void TestFallbackOutputCachingSeparatedByModel() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  engine->SetBehaviorForWidth(10, ScriptedInferenceEngine::Behavior::kFail, "direct stage forced");

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(10, 10);

  zsoda::core::RenderParams small_params;
  small_params.model_id = "depth-anything-v3-small";
  small_params.frame_hash = 777;
  small_params.tile_size = 6;
  small_params.overlap = 0;

  const auto first = pipeline.Render(src, small_params);
  assert(first.status == zsoda::core::RenderStatus::kFallbackTiled);
  assert(Contains(first.message, "tiled fallback succeeded"));
  const int run_count_after_first = engine->RunCount();

  const auto second = pipeline.Render(src, small_params);
  assert(second.status == zsoda::core::RenderStatus::kCacheHit);
  assert(engine->RunCount() == run_count_after_first);

  zsoda::core::RenderParams large_params = small_params;
  large_params.model_id = "depth-anything-v3-large";
  const auto third = pipeline.Render(src, large_params);
  assert(third.status != zsoda::core::RenderStatus::kCacheHit);
  assert(engine->RunCount() > run_count_after_first);
}

void TestSliceParametersInvalidateCacheKey() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(10, 10);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 9001;
  params.output_mode = zsoda::core::OutputMode::kSlicing;
  params.min_depth = 0.2F;
  params.max_depth = 0.8F;
  params.softness = 0.05F;

  const auto first = pipeline.Render(src, params);
  assert(first.status == zsoda::core::RenderStatus::kInference);
  const int run_count_after_first = engine->RunCount();

  params.min_depth = 0.4F;
  const auto second = pipeline.Render(src, params);
  assert(second.status != zsoda::core::RenderStatus::kCacheHit);
  assert(engine->RunCount() > run_count_after_first);
}

void TestTileParametersInvalidateCacheKey() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  engine->SetBehaviorForWidth(10, ScriptedInferenceEngine::Behavior::kFail, "direct stage forced");

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(10, 10);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 9002;
  params.tile_size = 6;
  params.overlap = 0;

  const auto first = pipeline.Render(src, params);
  assert(first.status == zsoda::core::RenderStatus::kFallbackTiled);
  const int run_count_after_first = engine->RunCount();

  params.tile_size = 8;
  const auto second = pipeline.Render(src, params);
  assert(second.status != zsoda::core::RenderStatus::kCacheHit);
  assert(engine->RunCount() > run_count_after_first);
}

void TestQualityBoostUsesReferenceAndTilePasses() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(16, 16);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 9005;
  params.cache_enabled = false;
  params.mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  params.temporal_alpha = 1.0F;
  params.edge_enhancement = 0.0F;
  params.quality = 3;
  params.quality_boost = 4;

  const auto output = pipeline.Render(src, params);
  assert(output.status == zsoda::core::RenderStatus::kInference);
  assert(Contains(output.message, "quality boost applied"));

  const auto& run_qualities = engine->RunQualities();
  assert(run_qualities.size() == 17U);
  assert(run_qualities.front() == 2);
  for (std::size_t i = 1; i < run_qualities.size(); ++i) {
    assert(run_qualities[i] == 3);
  }
}

void TestQualityBoostResidualFusionSuppressesLowFrequencyTileBias() {
  auto engine = std::make_shared<LowFrequencyBiasInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(16, 16);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 9015;
  params.cache_enabled = false;
  params.mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  params.temporal_alpha = 1.0F;
  params.edge_enhancement = 0.0F;
  params.quality = 3;
  params.quality_boost = 2;

  const auto output = pipeline.Render(src, params);
  assert(output.status == zsoda::core::RenderStatus::kInference);
  assert(Contains(output.message, "fusion=residual"));

  const float center = output.frame.at(8, 8, 0);
  const float lower_right = output.frame.at(13, 13, 0);
  assert(std::fabs(center - 0.5F) < 0.05F);
  assert(std::fabs(lower_right - 0.5F) < 0.08F);
}

void TestQualityBoostResidualFusionPreservesHighFrequencyDetail() {
  auto engine = std::make_shared<HighFrequencyDetailInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(16, 16);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 9016;
  params.cache_enabled = false;
  params.mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  params.temporal_alpha = 1.0F;
  params.edge_enhancement = 0.0F;
  params.quality = 3;
  params.quality_boost = 2;

  const auto output = pipeline.Render(src, params);
  assert(output.status == zsoda::core::RenderStatus::kInference);
  assert(Contains(output.message, "fusion=residual"));

  const float even = output.frame.at(7, 7, 0);
  const float odd = output.frame.at(8, 7, 0);
  assert(std::fabs(even - odd) > 0.08F);
}

void TestQualityBoostSkipsTemporalSequenceModels() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-large-multiview", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(16, 16);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-large-multiview";
  params.frame_hash = 9007;
  params.cache_enabled = false;
  params.mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  params.temporal_alpha = 1.0F;
  params.edge_enhancement = 0.0F;
  params.quality = 3;
  params.quality_boost = 4;

  const auto output = pipeline.Render(src, params);
  assert(output.status == zsoda::core::RenderStatus::kInference);
  assert(Contains(output.message, "detail boost skipped: temporal sequence model"));

  const auto& run_qualities = engine->RunQualities();
  assert(run_qualities.size() == 1U);
  assert(run_qualities.front() == 3);
}

void TestQualityBoostInvalidatesCacheKey() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(16, 16);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 9006;
  params.mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  params.temporal_alpha = 1.0F;
  params.edge_enhancement = 0.0F;
  params.quality = 3;
  params.quality_boost = 0;

  const auto first = pipeline.Render(src, params);
  assert(first.status == zsoda::core::RenderStatus::kInference);
  const int run_count_after_first = engine->RunCount();

  const auto second = pipeline.Render(src, params);
  assert(second.status == zsoda::core::RenderStatus::kCacheHit);
  assert(engine->RunCount() == run_count_after_first);

  params.quality_boost = 2;
  const auto third = pipeline.Render(src, params);
  assert(third.status != zsoda::core::RenderStatus::kCacheHit);
  assert(engine->RunCount() > run_count_after_first);
}

void TestExtractTokenInvalidatesCacheKey() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(8, 8);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 9004;
  params.cache_enabled = true;
  params.freeze_enabled = false;
  params.mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  params.temporal_alpha = 1.0F;
  params.extract_token = 0;

  const auto first = pipeline.Render(src, params);
  assert(first.status == zsoda::core::RenderStatus::kInference);
  const int run_count_after_first = engine->RunCount();

  const auto second = pipeline.Render(src, params);
  assert(second.status == zsoda::core::RenderStatus::kCacheHit);
  assert(engine->RunCount() == run_count_after_first);

  params.extract_token = 1;
  const auto third = pipeline.Render(src, params);
  assert(third.status != zsoda::core::RenderStatus::kCacheHit);
  assert(engine->RunCount() > run_count_after_first);
}

void TestStatefulPostProcessDisablesCache() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(8, 8);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 9003;
  params.temporal_alpha = 0.5F;
  params.temporal_scene_cut_threshold = 1.0F;

  const auto first = pipeline.Render(src, params);
  assert(first.status == zsoda::core::RenderStatus::kInference);
  const int run_count_after_first = engine->RunCount();

  const auto second = pipeline.Render(src, params);
  assert(second.status == zsoda::core::RenderStatus::kInference);
  assert(engine->RunCount() > run_count_after_first);
}

void TestZeroFrameHashDisablesCache() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(8, 8);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 0;

  const auto first = pipeline.Render(src, params);
  assert(first.status == zsoda::core::RenderStatus::kInference);
  const int run_count_after_first = engine->RunCount();

  const auto second = pipeline.Render(src, params);
  assert(second.status == zsoda::core::RenderStatus::kInference);
  assert(engine->RunCount() > run_count_after_first);
}

void TestDepthMappingModeRawVsNormalize() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(8, 8);

  zsoda::core::RenderParams raw_params;
  raw_params.model_id = "depth-anything-v3-small";
  raw_params.frame_hash = 5101;
  raw_params.cache_enabled = false;
  raw_params.mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  raw_params.temporal_alpha = 1.0F;
  raw_params.edge_enhancement = 0.0F;

  const auto raw_output = pipeline.Render(src, raw_params);
  assert(raw_output.status == zsoda::core::RenderStatus::kInference);
  const float raw_value = raw_output.frame.at(0, 0, 0);
  assert(raw_value > 0.2F && raw_value < 0.3F);

  zsoda::core::RenderParams normalized_params = raw_params;
  normalized_params.frame_hash = 5102;
  normalized_params.mapping_mode = zsoda::core::DepthMappingMode::kNormalize;
  const auto normalized_output = pipeline.Render(src, normalized_params);
  assert(normalized_output.status == zsoda::core::RenderStatus::kInference);
  const float normalized_value = normalized_output.frame.at(0, 0, 0);
  assert(normalized_value < 1e-3F);
}

void TestTemporalSmoothingBlendsFrames() {
  auto engine = std::make_shared<SequenceInferenceEngine>(std::vector<float>{0.2F, 0.8F});
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(6, 6);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.cache_enabled = false;
  params.mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  params.temporal_alpha = 0.5F;
  params.temporal_edge_aware = false;
  params.temporal_scene_cut_threshold = 1.0F;
  params.edge_enhancement = 0.0F;

  params.frame_hash = 6101;
  const auto first = pipeline.Render(src, params);
  assert(first.status == zsoda::core::RenderStatus::kInference);
  assert(std::fabs(first.frame.at(0, 0, 0) - 0.2F) < 1e-3F);

  params.frame_hash = 6102;
  const auto second = pipeline.Render(src, params);
  assert(second.status == zsoda::core::RenderStatus::kInference);
  const float blended = second.frame.at(0, 0, 0);
  assert(blended > 0.45F && blended < 0.55F);
}

void TestFreezeModeReusesCapturedDepthUntilTokenChanges() {
  auto engine = std::make_shared<SequenceInferenceEngine>(std::vector<float>{0.2F, 0.8F, 0.9F});
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(6, 6);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.cache_enabled = false;
  params.freeze_enabled = true;
  params.extract_token = 0;
  params.mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  params.temporal_alpha = 1.0F;
  params.edge_enhancement = 0.0F;
  params.temporal_scene_cut_threshold = 1.0F;

  const auto first = pipeline.Render(src, params);
  assert(first.status == zsoda::core::RenderStatus::kInference);
  const float first_value = first.frame.at(0, 0, 0);
  assert(std::fabs(first_value - 0.2F) < 1e-3F);
  assert(engine->RunCount() == 1);

  const auto second = pipeline.Render(src, params);
  assert(second.status == zsoda::core::RenderStatus::kCacheHit);
  const float second_value = second.frame.at(0, 0, 0);
  assert(std::fabs(second_value - first_value) < 1e-5F);
  assert(engine->RunCount() == 1);

  params.extract_token = 1;
  const auto third = pipeline.Render(src, params);
  assert(third.status == zsoda::core::RenderStatus::kInference);
  const float third_value = third.frame.at(0, 0, 0);
  assert(std::fabs(third_value - 0.8F) < 1e-3F);
  assert(engine->RunCount() == 2);
}

void TestFreezeModeBypassesFrameCacheAndTracksFrameHashChanges() {
  auto engine = std::make_shared<SequenceInferenceEngine>(std::vector<float>{0.3F, 0.7F, 0.9F});
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(6, 6);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.cache_enabled = true;
  params.freeze_enabled = true;
  params.extract_token = 0;
  params.mapping_mode = zsoda::core::DepthMappingMode::kRaw;
  params.temporal_alpha = 1.0F;
  params.edge_enhancement = 0.0F;
  params.temporal_scene_cut_threshold = 1.0F;

  params.frame_hash = 7101;
  const auto first = pipeline.Render(src, params);
  assert(first.status == zsoda::core::RenderStatus::kInference);
  assert(std::fabs(first.frame.at(0, 0, 0) - 0.3F) < 1e-3F);
  assert(engine->RunCount() == 1);

  params.frame_hash = 7102;
  const auto second = pipeline.Render(src, params);
  assert(second.status == zsoda::core::RenderStatus::kCacheHit);
  assert(std::fabs(second.frame.at(0, 0, 0) - 0.3F) < 1e-3F);
  assert(engine->RunCount() == 1);

  params.extract_token = 1;
  const auto third = pipeline.Render(src, params);
  assert(third.status == zsoda::core::RenderStatus::kInference);
  assert(std::fabs(third.frame.at(0, 0, 0) - 0.7F) < 1e-3F);
  assert(engine->RunCount() == 2);
}

void TestSafeOutputAfterAllStagesFail() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  engine->SetDefaultBehavior(ScriptedInferenceEngine::Behavior::kFail, "forced failure");

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(10, 10);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 333;
  params.tile_size = 6;
  params.overlap = 0;

  const auto output = pipeline.Render(src, params);
  assert(output.status == zsoda::core::RenderStatus::kSafeOutput);
  assert(Contains(output.message, "all inference stages failed"));
  assert(output.frame.desc().width == 10);
  assert(output.frame.desc().height == 10);
  assert(output.frame.desc().channels == 1);
  AssertFrameAllZero(output.frame);
}

void TestSafeOutputOnException() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  engine->SetDefaultBehavior(ScriptedInferenceEngine::Behavior::kThrow, "forced exception");

  zsoda::core::RenderPipeline pipeline(engine);
  const auto src = MakeSourceFrame(10, 10);

  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";
  params.frame_hash = 909;
  params.tile_size = 6;
  params.overlap = 0;

  const auto output = pipeline.Render(src, params);
  assert(output.status == zsoda::core::RenderStatus::kSafeOutput);
  assert(Contains(output.message, "render exception"));
  AssertFrameAllZero(output.frame);
}

void TestEmptySourceReturnsSafeOutput() {
  auto engine = std::make_shared<ScriptedInferenceEngine>();
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));

  zsoda::core::RenderPipeline pipeline(engine);
  zsoda::core::FrameBuffer empty_source;
  zsoda::core::RenderParams params;
  params.model_id = "depth-anything-v3-small";

  const auto output = pipeline.Render(empty_source, params);
  assert(output.status == zsoda::core::RenderStatus::kSafeOutput);
  assert(output.frame.desc().width == 1);
  assert(output.frame.desc().height == 1);
  assert(output.frame.desc().channels == 1);
  assert(output.frame.at(0, 0, 0) == 0.0F);
}

}  // namespace

void RunRenderPipelineTests() {
  TestFallbackSequenceDownscaledAfterTiledFailure();
  TestAdaptiveFallbackRetriesRecordStageDiagnostics();
  TestAdaptiveFallbackHandlesInvalidTileConfiguration();
  TestDownscaledFallbackUsesVramBudgetHint();
  TestFallbackOutputCachingSeparatedByModel();
  TestSliceParametersInvalidateCacheKey();
  TestTileParametersInvalidateCacheKey();
  TestQualityBoostUsesReferenceAndTilePasses();
  TestQualityBoostResidualFusionSuppressesLowFrequencyTileBias();
  TestQualityBoostResidualFusionPreservesHighFrequencyDetail();
  TestQualityBoostSkipsTemporalSequenceModels();
  TestQualityBoostInvalidatesCacheKey();
  TestExtractTokenInvalidatesCacheKey();
  TestStatefulPostProcessDisablesCache();
  TestZeroFrameHashDisablesCache();
  TestDepthMappingModeRawVsNormalize();
  TestTemporalSmoothingBlendsFrames();
  TestFreezeModeReusesCapturedDepthUntilTokenChanges();
  TestFreezeModeBypassesFrameCacheAndTracksFrameHashChanges();
  TestSafeOutputAfterAllStagesFail();
  TestSafeOutputOnException();
  TestEmptySourceReturnsSafeOutput();
}
