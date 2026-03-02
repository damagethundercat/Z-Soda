#include "core/RenderPipeline.h"

#include <algorithm>
#include <functional>

#include "inference/InferenceEngine.h"

namespace zsoda::core {
namespace {

FrameBuffer CropGray(const FrameBuffer& source, const TileRect& tile) {
  FrameDesc desc;
  desc.width = tile.width;
  desc.height = tile.height;
  desc.channels = 1;
  desc.format = PixelFormat::kGray32F;
  FrameBuffer cropped(desc);

  for (int y = 0; y < tile.height; ++y) {
    for (int x = 0; x < tile.width; ++x) {
      const int sx = tile.x + x;
      const int sy = tile.y + y;
      const float value = source.at(sx, sy, 0);
      cropped.at(x, y, 0) = value;
    }
  }
  return cropped;
}

}  // namespace

RenderPipeline::RenderPipeline(std::shared_ptr<inference::IInferenceEngine> engine)
    : engine_(std::move(engine)) {}

RenderOutput RenderPipeline::Render(const FrameBuffer& source, const RenderParams& params) {
  if (source.empty()) {
    return SafeOutput(source, "empty source frame");
  }

  std::string model_selection_error;
  if (!engine_) {
    return SafeOutput(source, "missing inference engine");
  }
  if (!engine_->SelectModel(params.model_id, &model_selection_error)) {
    return SafeOutput(source, "model selection failed: " + model_selection_error);
  }

  const RenderCacheKey key = BuildCacheKey(source, params);
  if (params.cache_enabled) {
    FrameBuffer cached;
    if (cache_.Find(key, &cached)) {
      return {RenderStatus::kCacheHit, cached, "cache hit (" + engine_->ActiveModelId() + ")"};
    }
  }

  FrameDesc depth_desc = source.desc();
  depth_desc.channels = 1;
  depth_desc.format = PixelFormat::kGray32F;
  auto depth_ptr = pool_.Acquire(depth_desc);
  std::string error;

  if (!RunInference(source, params.quality, depth_ptr.get(), &error)) {
    if (!RunTiledInference(source, params.quality, params.tile_size, params.overlap, depth_ptr.get(),
                           &error)) {
      pool_.Release(depth_ptr);
      return SafeOutput(source, "inference failed: " + error);
    }

    NormalizeDepth(depth_ptr.get(), params.invert);
    FrameBuffer output = BuildOutput(*depth_ptr, params);
    pool_.Release(depth_ptr);
    if (params.cache_enabled) {
      cache_.Insert(key, output);
    }
    std::string message = "tiled fallback used (" + engine_->ActiveModelId() + ")";
    if (!error.empty()) {
      message += " - " + error;
    }
    if (!model_selection_error.empty()) {
      message += " - " + model_selection_error;
    }
    return {RenderStatus::kFallback, output, message};
  }

  NormalizeDepth(depth_ptr.get(), params.invert);
  FrameBuffer output = BuildOutput(*depth_ptr, params);
  pool_.Release(depth_ptr);

  if (params.cache_enabled) {
    cache_.Insert(key, output);
  }
  std::string message = "inference completed (" + engine_->ActiveModelId() + ")";
  if (!error.empty()) {
    message += " - " + error;
  }
  if (!model_selection_error.empty()) {
    message += " - " + model_selection_error;
  }
  return {RenderStatus::kInference, output, message};
}

void RenderPipeline::SetCacheLimit(std::size_t limit) {
  cache_.SetLimit(limit);
}

void RenderPipeline::PurgeCache() {
  cache_.Clear();
}

RenderCacheKey RenderPipeline::BuildCacheKey(const FrameBuffer& source, const RenderParams& params) const {
  RenderCacheKey key;
  key.width = source.desc().width;
  key.height = source.desc().height;
  key.quality = params.quality;
  key.invert = params.invert;
  key.slice_mode = params.output_mode == OutputMode::kSlicing;
  key.model_hash = static_cast<std::uint64_t>(std::hash<std::string>{}(params.model_id));
  key.frame_hash = params.frame_hash;
  return key;
}

bool RenderPipeline::RunInference(const FrameBuffer& source,
                                  int quality,
                                  FrameBuffer* depth,
                                  std::string* error) const {
  if (!engine_) {
    if (error) {
      *error = "missing inference engine";
    }
    return false;
  }
  inference::InferenceRequest request;
  request.source = &source;
  request.quality = quality;
  return engine_->Run(request, depth, error);
}

bool RenderPipeline::RunTiledInference(const FrameBuffer& source,
                                       int quality,
                                       int tile_size,
                                       int overlap,
                                       FrameBuffer* depth,
                                       std::string* error) const {
  if (depth == nullptr) {
    if (error) {
      *error = "depth output is null";
    }
    return false;
  }

  std::vector<TileResult> tile_results;
  const auto tiles = BuildTiles(source.desc().width, source.desc().height, tile_size, overlap);
  for (const auto& tile : tiles) {
    const FrameBuffer tile_src = CropGray(source, tile);
    FrameDesc tile_depth_desc = tile_src.desc();
    tile_depth_desc.channels = 1;
    tile_depth_desc.format = PixelFormat::kGray32F;
    FrameBuffer tile_depth(tile_depth_desc);

    inference::InferenceRequest request;
    request.source = &tile_src;
    request.quality = quality;
    if (!engine_->Run(request, &tile_depth, error)) {
      return false;
    }

    tile_results.push_back({tile, tile_depth});
  }

  *depth = ComposeTiles(depth->desc(), tile_results);
  return true;
}

FrameBuffer RenderPipeline::BuildOutput(const FrameBuffer& normalized_depth, const RenderParams& params) const {
  if (params.output_mode == OutputMode::kSlicing) {
    return BuildSliceMatte(normalized_depth, params.min_depth, params.max_depth, params.softness);
  }
  return normalized_depth;
}

RenderOutput RenderPipeline::SafeOutput(const FrameBuffer& source, const std::string& message) const {
  FrameDesc desc = source.desc();
  desc.channels = 1;
  desc.format = PixelFormat::kGray32F;
  FrameBuffer empty(desc);
  empty.Fill(0.0F);
  return {RenderStatus::kSafeOutput, empty, message};
}

}  // namespace zsoda::core
