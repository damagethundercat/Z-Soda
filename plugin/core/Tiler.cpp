#include "core/Tiler.h"

#include <algorithm>
#include <cmath>

namespace zsoda::core {
namespace {

float ComputeFeatherWeight(int local_x, int local_y, const TileRect& tile) {
  if (tile.width <= 1 || tile.height <= 1) {
    return 1.0F;
  }

  const float feather_x = std::max(1.0F, std::min(64.0F, static_cast<float>(tile.width) * 0.20F));
  const float feather_y = std::max(1.0F, std::min(64.0F, static_cast<float>(tile.height) * 0.20F));

  const float dist_left = static_cast<float>(local_x);
  const float dist_right = static_cast<float>(tile.width - 1 - local_x);
  const float dist_top = static_cast<float>(local_y);
  const float dist_bottom = static_cast<float>(tile.height - 1 - local_y);

  const float wx = std::clamp((std::min(dist_left, dist_right) + 1.0F) / feather_x, 0.0F, 1.0F);
  const float wy = std::clamp((std::min(dist_top, dist_bottom) + 1.0F) / feather_y, 0.0F, 1.0F);

  // Keep a tiny floor to avoid zero-weight gaps when neighboring tiles do not overlap.
  return std::max(1e-3F, wx * wy);
}

std::vector<int> BuildTileAxisStarts(int extent, int tile_size, int overlap) {
  std::vector<int> starts;
  if (extent <= 0 || tile_size <= 0) {
    return starts;
  }

  const int clamped_tile = std::max(1, std::min(tile_size, extent));
  const int step = std::max(1, clamped_tile - std::max(0, overlap));

  starts.push_back(0);
  if (extent <= clamped_tile) {
    return starts;
  }

  for (int pos = step; pos + clamped_tile < extent; pos += step) {
    starts.push_back(pos);
  }

  const int tail_start = extent - clamped_tile;
  if (starts.back() != tail_start) {
    starts.push_back(tail_start);
  }

  return starts;
}

}  // namespace

std::vector<TileRect> BuildTiles(int width, int height, int tile_size, int overlap) {
  std::vector<TileRect> tiles;
  if (width <= 0 || height <= 0 || tile_size <= 0) {
    return tiles;
  }

  const auto xs = BuildTileAxisStarts(width, tile_size, overlap);
  const auto ys = BuildTileAxisStarts(height, tile_size, overlap);
  for (const int y : ys) {
    for (const int x : xs) {
      TileRect tile;
      tile.x = x;
      tile.y = y;
      tile.width = std::min(tile_size, width);
      tile.height = std::min(tile_size, height);
      tiles.push_back(tile);
    }
  }
  return tiles;
}

FrameBuffer ComposeTiles(const FrameDesc& output_desc, const std::vector<TileResult>& tiles) {
  FrameBuffer composed(output_desc);
  composed.Fill(0.0F);
  FrameBuffer weights(output_desc);
  weights.Fill(0.0F);

  for (const auto& item : tiles) {
    const auto& td = item.depth.desc();
    if (td.channels < 1) {
      continue;
    }
    for (int y = 0; y < item.tile.height; ++y) {
      for (int x = 0; x < item.tile.width; ++x) {
        const int out_x = item.tile.x + x;
        const int out_y = item.tile.y + y;
        if (out_x < 0 || out_x >= output_desc.width || out_y < 0 || out_y >= output_desc.height) {
          continue;
        }
        const float weight = ComputeFeatherWeight(x, y, item.tile);
        composed.at(out_x, out_y, 0) += item.depth.at(x, y, 0) * weight;
        weights.at(out_x, out_y, 0) += weight;
      }
    }
  }

  for (int y = 0; y < output_desc.height; ++y) {
    for (int x = 0; x < output_desc.width; ++x) {
      const float w = weights.at(x, y, 0);
      if (w > 0.0F) {
        composed.at(x, y, 0) /= w;
      }
    }
  }

  return composed;
}

}  // namespace zsoda::core
