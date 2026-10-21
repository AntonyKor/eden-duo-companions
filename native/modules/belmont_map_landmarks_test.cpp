// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belmont_map_landmarks.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main(int argc, char **argv) {
  using namespace belmont_map_landmarks;
  Landmark l;
  l.guid = "known";
  l.unconditional_policy = true;
  l.navigation_zone_id = 7;
  std::vector<std::string> known{"known"}, none;
  std::vector<std::int64_t> cull{7};
  assert(!Visible(l, none, {}));
  assert(Visible(l, known, {}));
  assert(!Visible(l, known, cull));
  l.unconditional_policy = false;
  assert(!Visible(l, known, {}));
  SavePoint save;save.normal_art.navigation_zone_id=7;
  assert(!Visible(save,-1,1,7)&&!Visible(save,0,1,7));
  assert(!Visible(save,1,-1,7)&&!Visible(save,1,0,7));
  assert(!Visible(save,1,1,8)&&Visible(save,1,1,7));
  save.visible_in_unrevealed_map=true;assert(Visible(save,0,1,7));
  Reader missing(ce_unity::RangeReader{});
  assert(!missing.LoadForRoom("unknown"));
  if (argc != 2)
    return 0;
  Reader reader(ce_unity::MakeDirectoryRangeReader(argv[1]));
  const std::string room =
      "World_Biome_01_Streets_Poor_StreetsPoor_FirstRoom_uid73";
  auto data = reader.LoadForRoom(room);
  if (!data) {
    std::cerr << "native landmarks unavailable\n";
    return 1;
  }
  assert(data->landmarks.size() == 5);
  std::cout<<"native_save_points="<<data->save_points.size()<<" map_revealed_policy="<<data->map_revealed_policy<<'\n';
  assert(data->save_points.size()==5&&data->map_revealed_policy==1);
  for(const auto& point:data->save_points){
    assert(point.normal_art.sprites.size()==1&&point.last_used_art.sprites.size()==1);
    assert(point.normal_art.sprites[0].id==6230585524095202680LL);
    assert(point.last_used_art.sprites[0].id==9035961807606165270LL);
    assert(!point.visible_in_unrevealed_map);
    assert(std::abs(point.screen_percentage-.06f)<1e-7f);
    assert(std::abs(point.screen_percentage_at_zoom_max-.09f)<1e-7f);
  }
  bool docks = false;
  for (const auto &item : data->landmarks) {
    std::cout << item.source_name << " " << item.guid
              << " policy=" << item.unconditional_policy
              << " sprites=" << item.sprites.size() << "\n";
    if (item.room_id == room) {
      docks = true;
      assert(item.guid == "417a32b0b7028a347b0b367fb4ea46ea");
      assert(item.x == -347.5f && item.y == 4);
      assert(item.unconditional_policy);
      assert(item.sprites.size() == 1);
      const auto &s = item.sprites[0];
      assert(s.x == -350 && s.y == -1.5f);
      assert(s.name == "T_UI_Map_Landmark_Docks");
      assert(!(s.settings_raw & 1));
      assert(((s.settings_raw >> 2) & 15) == 0);
      assert(s.texture_rect_offset_x >= 0 && s.texture_rect_offset_y >= 0);
      assert(item.name_style.font_asset_id == 443565887117910916LL);
      assert(!item.name_style.font_asset_cab.empty());
      std::cout << "Docks fontCAB=" << item.name_style.font_asset_cab
                << " trimOffset=" << s.texture_rect_offset_x << ","
                << s.texture_rect_offset_y << " settings=" << s.settings_raw
                << "\n";
      assert(s.width == 512 && s.height == 512 && s.pixels_per_unit == 16);
      assert(item.name_key_id == 2799249536LL);
      assert(item.name_x == -350 && item.name_y == -1.5f);
      assert(item.name_style.font_size == 1);
      assert(item.name_style.font_style == 32);
      assert(item.name_style.scale_x == 1.5f &&
             item.name_style.scale_y == 1.5f);
      assert(std::abs(item.name_style.screen_percentage - .03f) < 1e-7f);
      assert(item.name_style.ribbon_sprite_id == 8769341631646220334LL);
      assert(item.name_style.ribbon_pixels_per_unit_multiplier == 12);
      assert(item.name_style.ribbon);
      assert(item.name_style.ribbon->border[0] == 156);
      assert(item.name_style.ribbon->border[2] == 153);
      assert(item.name_style.responsive_scale_x == 1.5f);
      assert(item.name_style.horizontal_alignment == 2);
      assert(item.name_style.vertical_alignment == 512);
      assert(item.name_style.margin[0] == 1 && item.name_style.margin[2] == 1);
      assert(!Visible(item, none, {}));
    }
  }
  assert(docks);
  reader.ClearCache();
  assert(reader.LoadForRoom(room));
  std::cout << "landmark source and discovery tests pass\n";
}
