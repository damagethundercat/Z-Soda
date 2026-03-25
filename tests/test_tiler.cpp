#include <cassert>
#include <cmath>

#include "core/Tiler.h"

namespace {

void TestBuildTiles() {
  const auto tiles = zsoda::core::BuildTiles(1920, 1080, 512, 32);
  assert(!tiles.empty());
}

void TestBuildTilesAvoidsSkinnyEdgeTiles() {
  const int tile_size = 624;
  const auto tiles = zsoda::core::BuildTiles(1366, 768, tile_size, 124);
  assert(!tiles.empty());
  for (const auto& tile : tiles) {
    assert(tile.width == tile_size);
    assert(tile.height == tile_size);
  }
}

void TestBuildTilesSmallFrameStaysSingleTile() {
  const auto tiles = zsoda::core::BuildTiles(320, 180, 624, 124);
  assert(tiles.size() == 1U);
  assert(tiles[0].x == 0);
  assert(tiles[0].y == 0);
  assert(tiles[0].width == 320);
  assert(tiles[0].height == 180);
}

void TestComposeTiles() {
  zsoda::core::FrameDesc desc;
  desc.width = 4;
  desc.height = 1;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;

  zsoda::core::TileResult left;
  left.tile = {0, 0, 2, 1};
  left.depth = zsoda::core::FrameBuffer({2, 1, 1, zsoda::core::PixelFormat::kGray32F});
  left.depth.at(0, 0, 0) = 0.2F;
  left.depth.at(1, 0, 0) = 0.4F;

  zsoda::core::TileResult right;
  right.tile = {2, 0, 2, 1};
  right.depth = zsoda::core::FrameBuffer({2, 1, 1, zsoda::core::PixelFormat::kGray32F});
  right.depth.at(0, 0, 0) = 0.6F;
  right.depth.at(1, 0, 0) = 0.8F;

  const auto composed = zsoda::core::ComposeTiles(desc, {left, right});
  assert(std::fabs(composed.at(0, 0, 0) - 0.2F) < 1e-6F);
  assert(std::fabs(composed.at(3, 0, 0) - 0.8F) < 1e-6F);
}

}  // namespace

void RunTilerTests() {
  TestBuildTiles();
  TestBuildTilesAvoidsSkinnyEdgeTiles();
  TestBuildTilesSmallFrameStaysSingleTile();
  TestComposeTiles();
}
