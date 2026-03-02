#pragma once

#include <memory>
#include <string>

#include "core/BufferPool.h"
#include "core/Cache.h"
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
  OutputMode output_mode = OutputMode::kDepthMap;
  float min_depth = 0.25F;
  float max_depth = 0.75F;
  float softness = 0.1F;
  bool cache_enabled = true;
  int tile_size = 512;
  int overlap = 32;
  std::uint64_t frame_hash = 0;
};

enum class RenderStatus {
  kCacheHit,
  kInference,
  kFallback,
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
  bool RunInference(const FrameBuffer& source, int quality, FrameBuffer* depth, std::string* error) const;
  bool RunTiledInference(const FrameBuffer& source,
                         int quality,
                         int tile_size,
                         int overlap,
                         FrameBuffer* depth,
                         std::string* error) const;
  FrameBuffer BuildOutput(const FrameBuffer& normalized_depth, const RenderParams& params) const;
  RenderOutput SafeOutput(const FrameBuffer& source, const std::string& message) const;

  std::shared_ptr<inference::IInferenceEngine> engine_;
  mutable DepthCache cache_{64};
  mutable BufferPool pool_{8};
};

}  // namespace zsoda::core
