#pragma once

#include <vector>

#include "core/Frame.h"

namespace zsoda::core {

struct TileRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct TileResult {
  TileRect tile;
  FrameBuffer depth;
};

std::vector<TileRect> BuildTiles(int width, int height, int tile_size, int overlap);
FrameBuffer ComposeTiles(const FrameDesc& output_desc, const std::vector<TileResult>& tiles);

}  // namespace zsoda::core
