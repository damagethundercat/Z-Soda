#pragma once

#include <memory>
#include <string>

#include "core/BufferPool.h"
#include "core/Cache.h"
#include "core/CompatMutex.h"
#include "core/DepthOps.h"
#include "core/Frame.h"
#include "core/Tiler.h"

namespace zsoda::inference {
class IInferenceEngine;
}

namespace zsoda::core {

enum class OutputMode {
  kDepthMap,
  kSlicing,
};

struct RenderParams {
  std::string model_id = "depth-anything-v3-small";
  int quality = 1;
  bool invert = false;
  DepthMappingMode mapping_mode = DepthMappingMode::kRaw;
  float guided_low_percentile = 0.05F;
  float guided_high_percentile = 0.95F;
  float guided_update_alpha = 0.15F;
  float temporal_alpha = 1.0F;
  bool temporal_edge_aware = false;
  float temporal_edge_threshold = 0.08F;
  float temporal_scene_cut_threshold = 0.18F;
  float edge_enhancement = 0.0F;
  float edge_guidance_sigma = 0.12F;
  bool edge_aware_upsample = true;
  OutputMode output_mode = OutputMode::kDepthMap;
  float min_depth = 0.25F;
  float max_depth = 0.75F;
  float softness = 0.1F;
  bool cache_enabled = true;
  int tile_size = 512;
  int overlap = 32;
  int vram_budget_mb = 0;
  bool freeze_enabled = false;
  int extract_token = 0;
  std::uint64_t frame_hash = 0;
};

enum class RenderStatus {
  kCacheHit,
  kInference,
  kFallbackTiled,
  kFallbackDownscaled,
  kSafeOutput,
};

struct RenderOutput {
  RenderStatus status = RenderStatus::kSafeOutput;
  FrameBuffer frame;
  std::string message;
};

class RenderPipeline {
 public:
  explicit RenderPipeline(std::shared_ptr<inference::IInferenceEngine> engine);

  RenderOutput Render(const FrameBuffer& source, const RenderParams& params);
  void SetCacheLimit(std::size_t limit);
  void PurgeCache();

 private:
  RenderCacheKey BuildCacheKey(const FrameBuffer& source, const RenderParams& params) const;
  bool ShouldUseCache(const RenderParams& params) const;
  static bool IsStatefulPostProcess(const RenderParams& params);
  std::uint64_t BuildPostprocessStateHash(const RenderParams& params) const;
  bool RunInference(const FrameBuffer& source, int quality, FrameBuffer* depth, std::string* error) const;
  bool RunTiledInference(const FrameBuffer& source,
                         int quality,
                         int tile_size,
                         int overlap,
                         FrameBuffer* depth,
                         std::string* error) const;
  bool RunDownscaledInference(const FrameBuffer& source,
                              const RenderParams& params,
                              FrameBuffer* depth,
                              std::string* error) const;
  bool TryGetFrozenOutput(const FrameBuffer& source,
                          const RenderParams& params,
                          FrameBuffer* output) const;
  void StoreFrozenOutput(const FrameBuffer& source,
                         const RenderParams& params,
                         const FrameBuffer& output) const;
  void ClearFrozenOutput() const;
  std::uint64_t BuildFrozenStateHash(const FrameBuffer& source,
                                     const RenderParams& params) const;
  void ApplyPostProcess(FrameBuffer* depth, const FrameBuffer& source, const RenderParams& params) const;
  void ApplyTemporalSmoothing(FrameBuffer* depth,
                              const FrameBuffer& source_luma,
                              const RenderParams& params) const;
  void ApplyEdgeAwareEnhancement(FrameBuffer* depth,
                                 const FrameBuffer& source_luma,
                                 const RenderParams& params) const;
  FrameBuffer BuildOutput(const FrameBuffer& normalized_depth, const RenderParams& params) const;
  RenderOutput SafeOutput(const FrameBuffer& source, const std::string& message) const;

  std::shared_ptr<inference::IInferenceEngine> engine_;
  mutable DepthCache cache_{64};
  mutable BufferPool pool_{8};
  mutable CompatMutex postprocess_mutex_;
  mutable GuidedDepthMappingState guided_mapping_state_{};
  mutable FrameBuffer temporal_depth_;
  mutable FrameBuffer temporal_source_luma_;
  mutable std::uint64_t postprocess_model_hash_ = 0;
  mutable std::uint64_t postprocess_state_hash_ = 0;
  mutable bool postprocess_initialized_ = false;
  mutable bool temporal_has_state_ = false;
  mutable CompatMutex frozen_mutex_;
  mutable FrameBuffer frozen_output_;
  mutable std::uint64_t frozen_state_hash_ = 0;
  mutable bool frozen_has_output_ = false;
};

}  // namespace zsoda::core
