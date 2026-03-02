#include <cassert>
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

  zsoda::core::RenderParams small;
  small.model_id = "depth-anything-v3-small";
  small.frame_hash = 777;
  small.tile_size = 6;
  small.overlap = 0;

  const auto first = pipeline.Render(src, small);
  assert(first.status == zsoda::core::RenderStatus::kFallbackTiled);
  assert(Contains(first.message, "tiled fallback succeeded"));
  const int run_count_after_first = engine->RunCount();

  const auto second = pipeline.Render(src, small);
  assert(second.status == zsoda::core::RenderStatus::kCacheHit);
  assert(engine->RunCount() == run_count_after_first);

  zsoda::core::RenderParams large = small;
  large.model_id = "depth-anything-v3-large";
  const auto third = pipeline.Render(src, large);
  assert(third.status != zsoda::core::RenderStatus::kCacheHit);
  assert(engine->RunCount() > run_count_after_first);
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
  TestSafeOutputAfterAllStagesFail();
  TestSafeOutputOnException();
  TestEmptySourceReturnsSafeOutput();
}
