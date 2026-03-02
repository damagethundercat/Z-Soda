#include "core/Tiler.h"

#include <algorithm>

namespace zsoda::core {

std::vector<TileRect> BuildTiles(int width, int height, int tile_size, int overlap) {
  std::vector<TileRect> tiles;
  if (width <= 0 || height <= 0 || tile_size <= 0) {
    return tiles;
  }

  const int step = std::max(1, tile_size - std::max(0, overlap));
  for (int y = 0; y < height; y += step) {
    for (int x = 0; x < width; x += step) {
      TileRect tile;
      tile.x = x;
      tile.y = y;
      tile.width = std::min(tile_size, width - x);
      tile.height = std::min(tile_size, height - y);
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
        composed.at(out_x, out_y, 0) += item.depth.at(x, y, 0);
        weights.at(out_x, out_y, 0) += 1.0F;
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
