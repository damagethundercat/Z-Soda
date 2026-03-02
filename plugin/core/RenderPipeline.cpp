#include "core/RenderPipeline.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <string>
#include <utility>

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

FrameBuffer ResizeNearest(const FrameBuffer& source, int output_width, int output_height) {
  FrameDesc desc = source.desc();
  desc.width = output_width;
  desc.height = output_height;
  FrameBuffer resized(desc);

  const int source_width = source.desc().width;
  const int source_height = source.desc().height;
  const int channels = source.desc().channels;

  for (int y = 0; y < output_height; ++y) {
    const int sy = std::min(source_height - 1, (y * source_height) / std::max(1, output_height));
    for (int x = 0; x < output_width; ++x) {
      const int sx = std::min(source_width - 1, (x * source_width) / std::max(1, output_width));
      for (int c = 0; c < channels; ++c) {
        resized.at(x, y, c) = source.at(sx, sy, c);
      }
    }
  }
  return resized;
}

struct PooledFrame {
  BufferPool* pool = nullptr;
  std::shared_ptr<FrameBuffer> buffer;

  ~PooledFrame() {
    if (pool != nullptr && buffer != nullptr) {
      pool->Release(buffer);
    }
  }

  FrameBuffer* get() const {
    return buffer.get();
  }
};

}  // namespace

RenderPipeline::RenderPipeline(std::shared_ptr<inference::IInferenceEngine> engine)
    : engine_(std::move(engine)) {}

RenderOutput RenderPipeline::Render(const FrameBuffer& source, const RenderParams& params) {
  try {
    if (source.empty()) {
      return SafeOutput(source, "empty source frame");
    }

    if (!engine_) {
      return SafeOutput(source, "missing inference engine");
    }

    std::string model_selection_error;
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
    PooledFrame depth;
    depth.pool = &pool_;
    depth.buffer = pool_.Acquire(depth_desc);

    auto finalize_output = [&](RenderStatus status, std::string message) -> RenderOutput {
      NormalizeDepth(depth.get(), params.invert);
      FrameBuffer output = BuildOutput(*depth.get(), params);
      if (params.cache_enabled) {
        cache_.Insert(key, output);
      }
      if (!model_selection_error.empty()) {
        message += " - " + model_selection_error;
      }
      return {status, std::move(output), std::move(message)};
    };

    std::string direct_error;
    if (RunInference(source, params.quality, depth.get(), &direct_error)) {
      return finalize_output(RenderStatus::kInference,
                             "direct inference succeeded (" + engine_->ActiveModelId() + ")");
    }

    std::string tiled_error;
    if (RunTiledInference(source, params.quality, params.tile_size, params.overlap, depth.get(),
                          &tiled_error)) {
      std::string message = "tiled fallback succeeded after direct failure (" + engine_->ActiveModelId() +
                            ")";
      if (!direct_error.empty()) {
        message += " - direct inference failed: " + direct_error;
      }
      return finalize_output(RenderStatus::kFallbackTiled, message);
    }

    std::string downscaled_error;
    if (RunDownscaledInference(source, params.quality, depth.get(), &downscaled_error)) {
      std::string message =
          "downscaled fallback succeeded after direct/tiled failures (" + engine_->ActiveModelId() +
          ")";
      if (!direct_error.empty()) {
        message += " - direct inference failed: " + direct_error;
      }
      if (!tiled_error.empty()) {
        message += " - tiled inference failed: " + tiled_error;
      }
      return finalize_output(RenderStatus::kFallbackDownscaled, message);
    }

    std::string message = "all inference stages failed";
    if (!direct_error.empty()) {
      message += " - direct inference failed: " + direct_error;
    }
    if (!tiled_error.empty()) {
      message += " - tiled inference failed: " + tiled_error;
    }
    if (!downscaled_error.empty()) {
      message += " - downscaled inference failed: " + downscaled_error;
    }
    message += " - returning safe output";
    if (!model_selection_error.empty()) {
      message += " - " + model_selection_error;
    }
    return SafeOutput(source, message);
  } catch (const std::exception& ex) {
    return SafeOutput(source, std::string("render exception: ") + ex.what());
  } catch (...) {
    return SafeOutput(source, "render exception: unknown");
  }
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
  if (tiles.empty()) {
    if (error != nullptr) {
      *error = "no tiles generated";
    }
    return false;
  }

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

bool RenderPipeline::RunDownscaledInference(const FrameBuffer& source,
                                            int quality,
                                            FrameBuffer* depth,
                                            std::string* error) const {
  if (depth == nullptr) {
    if (error != nullptr) {
      *error = "depth output is null";
    }
    return false;
  }
  if (source.empty()) {
    if (error != nullptr) {
      *error = "source frame is empty";
    }
    return false;
  }

  const int downscaled_width = std::max(1, source.desc().width / 2);
  const int downscaled_height = std::max(1, source.desc().height / 2);
  if (downscaled_width == source.desc().width && downscaled_height == source.desc().height) {
    if (error != nullptr) {
      *error = "source frame is too small for downscaled fallback";
    }
    return false;
  }

  const FrameBuffer downscaled_source = ResizeNearest(source, downscaled_width, downscaled_height);
  FrameDesc downscaled_depth_desc = downscaled_source.desc();
  downscaled_depth_desc.channels = 1;
  downscaled_depth_desc.format = PixelFormat::kGray32F;
  FrameBuffer downscaled_depth(downscaled_depth_desc);

  std::string downscaled_run_error;
  const int downscaled_quality = std::max(1, quality - 1);
  if (!RunInference(downscaled_source, downscaled_quality, &downscaled_depth, &downscaled_run_error)) {
    if (error != nullptr) {
      *error = downscaled_run_error.empty() ? "downscaled run failed" : downscaled_run_error;
    }
    return false;
  }

  *depth = ResizeNearest(downscaled_depth, source.desc().width, source.desc().height);
  if (error != nullptr) {
    error->clear();
  }
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
  if (!IsValid(desc)) {
    desc.width = 1;
    desc.height = 1;
  }
  desc.channels = 1;
  desc.format = PixelFormat::kGray32F;
  FrameBuffer empty(desc);
  empty.Fill(0.0F);
  return {RenderStatus::kSafeOutput, empty, message};
}

}  // namespace zsoda::core
