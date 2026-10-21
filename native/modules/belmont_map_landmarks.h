// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "belmont_map_sources.h"
#include "chained_echoes_unity.h"
#include <mutex>
namespace belmont_map_landmarks {
struct Sprite {
  std::string bundle, cab, name;
  std::int64_t id{}, texture_id{};
  float x{}, y{}, scale_x{1}, scale_y{1};
  float width{}, height{}, pivot_x{}, pivot_y{}, pixels_per_unit{};
  ce_unity::Rectf rect, texture_rect;
  float texture_rect_offset_x{}, texture_rect_offset_y{};
  std::uint32_t settings_raw{};
  std::array<float, 4> border{};
  std::array<float, 4> color{1, 1, 1, 1};
};
struct NameStyle {
  float font_size{}, scale_x{1}, scale_y{1}, screen_percentage{};
  float responsive_scale_x{1}, responsive_scale_y{1};
  std::int64_t horizontal_alignment{}, vertical_alignment{};
  std::int64_t font_style{}, ribbon_sprite_id{};
  std::int64_t font_asset_id{};
  std::string font_asset_cab;
  float ribbon_pixels_per_unit_multiplier{};
  std::array<float, 4> color{}, margin{}, ribbon_color{};
  std::optional<Sprite> ribbon;
};
struct Landmark {
  std::string guid, room_id, source_name, name_table;
  std::int64_t source_id{}, navigation_zone_id{}, name_key_id{};
  std::int32_t exploration_zone_id{};
  float x{}, y{}, name_x{}, name_y{};
  // Only null extra reveal, empty step/subelement conditions and no name hiding
  // are supported. Unknown policies remain records but may never be shown.
  bool unconditional_policy{};
  NameStyle name_style;
  std::vector<Sprite> sprites; // Native step zero, not all authored variants.
};
struct SavePoint {
  Landmark normal_art, last_used_art; // Native variants, without name ribbons.
  bool visible_in_unrevealed_map{};
  float screen_percentage{}, screen_percentage_at_zoom_max{};
};
struct Data {
  std::string world_name;
  std::vector<Landmark> landmarks;
  std::vector<SavePoint> save_points;
  // Native ResetContext defaults true; only an absent reveal condition proves
  // this without guest progression data. Conditional/missing data stays -1.
  int map_revealed_policy{-1};
};
// Native SavePointItem visibility: used room, map revelation and navigation.
// Unknown live booleans are represented by -1 and fail closed.
bool Visible(const SavePoint &, int map_revealed, int used_save_point,
             std::int64_t active_navigation);
// Source records only; never infer discovery from fog/position. Callers must
// join GUIDs to a coherent live ExplorerComponent.discoveredLandmarks snapshot
// and apply native navigation culling before exposing art or names.
bool Visible(const Landmark &, std::span<const std::string> discovered_guids,
             std::span<const std::int64_t> culled_navigation_ids);
class Reader {
public:
  explicit Reader(ce_unity::RangeReader romfs);
  explicit Reader(const EdenDsmodHostApi *host);
  std::optional<Data> LoadForRoom(std::string_view room_id);
  void ClearCache();

private:
  std::mutex mutex;
  ce_unity::Reader unity;
  std::optional<Data> cache;
};
} // namespace belmont_map_landmarks
