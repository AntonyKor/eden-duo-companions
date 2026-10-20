// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "belmont_assets.h"
#include "belmont_map_landmarks.h"
namespace belmont_landmark_render {
struct Placement {
  float center_x{}, center_y{}, size{};
};
// Metadata only, no RomFS calls or pixel allocation.
std::optional<Placement> ArtPlacement(const belmont_map_landmarks::Landmark &);
struct WorldFrame {
  ce_unity::Image image;
  float world_center_x{}, world_center_y{}, size_world{};
};
struct NameFrame {
  ce_unity::Image image;
  float world_center_x{}, world_center_y{}, size_px{};
  bool accessibility_enlarged{};
};
// Worker only. Discovery and navigation culling belong to the caller. Art is a
// world-space square; the native responsive ribbon is a separate pixel-size
// square so host zoom never stretches the name with the building engraving.
class Renderer {
public:
  explicit Renderer(ce_unity::RangeReader romfs,
                    ce_unity::AstcDecoder astc = {});
  explicit Renderer(const EdenDsmodHostApi *host,
                    ce_unity::AstcDecoder astc = {});
  std::optional<WorldFrame> RenderArt(const belmont_map_landmarks::Landmark &);
  std::optional<NameFrame> RenderName(const belmont_map_landmarks::Landmark &,
                                      std::string_view english,
                                      float canvas_height_px = 1080);
  // No landmark/ribbon textures; a cold call prepares the shared font once.
  std::optional<Placement>
  NamePlacement(const belmont_map_landmarks::Landmark &,
                std::string_view english, float canvas_height_px = 1080);
  void ClearCache();

private:
  std::optional<NameFrame> Name(const belmont_map_landmarks::Landmark &,
                                std::string_view, float, bool metadata_only);
  std::mutex mutex;
  belmont_assets::Reader assets;
  std::optional<belmont_assets::HostFont> font;
};
} // namespace belmont_landmark_render
