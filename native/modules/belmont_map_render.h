// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "belmont_assets.h"
#include "belmont_map_fog.h"
#include "belmont_map_tiles.h"
namespace belmont_map_render {
struct ExploredRoom {
  std::string room_id;
  std::vector<std::int32_t> entered_zone_ids;
};
struct Request {
  std::string current_room_id;
  std::vector<ExploredRoom> explored_rooms;
  std::vector<std::int64_t> culled_navigation_zone_ids;
  float player_x{}, player_y{};
  std::uint32_t width{1024}, height{640};
  // Optional orthographic world rectangle; absent => fit actual entered shapes.
  std::optional<belmont_map_fog::Rect> world_bounds;
};
struct Frame {
  ce_unity::Image image;
  belmont_map_fog::Rect world_bounds;
  std::uint32_t revealed_zone_count{};
  // Frozen-time decorative approximation; exact native shader parity is not
  // claimed.
  bool decorative_approximation{true};
};
// Asset-free, read-only initial slice: StreetsPoor FirstRoom only. Unknown
// rooms, bad zone IDs and absent native sources return unavailable; no all-map
// fallback.
class Renderer {
public:
  explicit Renderer(ce_unity::RangeReader romfs,
                    ce_unity::AstcDecoder astc = {});
  explicit Renderer(const EdenDsmodHostApi *host,
                    ce_unity::AstcDecoder astc = {});
  std::optional<Frame> Render(const Request &request);
  void ClearCache();

private:
  struct State;
  std::shared_ptr<State> state;
};
// Standard authored distance primitives used for source-authored visibility.
float ShapeDistance(const belmont_map_fog::Shape &shape, float x, float y);
float SmoothUnion(float a, float b, float smoothness);
} // namespace belmont_map_render
