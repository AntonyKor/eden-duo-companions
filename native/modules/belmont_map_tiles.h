// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "chained_echoes_unity.h"
#include <array>
#include <mutex>
#include <optional>
#include <vector>

namespace belmont_map_tiles {
enum class Source { Geometry, SecretGeometry, Background };
struct Point {
  float x{}, y{}, z{};
};
struct Tile {
  std::int64_t renderer_id{}, mesh_id{};
  std::uint32_t layer{};
  ce_unity::Rectf bounds;
  Point origin;
  std::vector<Point> vertices, uv0;
  std::vector<std::uint32_t> triangles;
};
struct Layer {
  // Native packed mask/distance/skyline bytes, not final shader-colored art.
  // Image origin is top-left; native mesh UV origin is bottom-left.
  ce_unity::Image image;
  std::int32_t graphics_format{};
  std::uint32_t depth{}, index{};
  bool srgb{};
};
class Reader {
public:
  explicit Reader(ce_unity::RangeReader romfs, ce_unity::AstcDecoder astc = {});
  explicit Reader(const EdenDsmodHostApi *host,
                  ce_unity::AstcDecoder astc = {});
  // Exact authored render tiles/UVs. These quads are NOT room contours.
  std::optional<std::vector<Tile>> LoadStreetsPoorLayout();
  // Reads/decompresses one native array slice; never loads all slices into RAM.
  // Background ASTC requires a decoder callback; missing decoder is
  // unavailable.
  std::optional<Layer> LoadStreetsPoorLayer(Source source, std::uint32_t layer);
  void ClearCache();

private:
  std::recursive_mutex mutex;
  ce_unity::Reader unity;
  ce_unity::AstcDecoder astc;
  std::optional<std::vector<Tile>> layout;
};
} // namespace belmont_map_tiles
