// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belmont_map_render.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
namespace belmont_map_render {
namespace {
constexpr std::string_view FirstRoom =
    "World_Biome_01_Streets_Poor_StreetsPoor_FirstRoom_uid73";
constexpr std::string_view Art =
    "art-01_streetspoor_assets_all_e33a704a7ea92e47f2c375f443aacbf1.bundle";
constexpr std::string_view Menus =
    "ui-gamemenus_assets_all_866519f7d6a7057a8ba1c459a5b3f694.bundle";
struct Color {
  float r{}, g{}, b{}, a{};
};
float Clamp(float v) { return std::clamp(v, 0.f, 1.f); }
float Smooth(float v) {
  v = Clamp(v);
  return v * v * (3.f - 2.f * v);
}
float Linear(float v) {
  return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
}
float Gamma(float v) {
  v = Clamp(v);
  return v <= .0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - .055f;
}
Color Scale(Color v, float a) { return {v.r * a, v.g * a, v.b * a, v.a}; }
Color Mix(Color a, Color b, float t) {
  t = Clamp(t);
  return {std::lerp(a.r, b.r, t), std::lerp(a.g, b.g, t),
          std::lerp(a.b, b.b, t), std::lerp(a.a, b.a, t)};
}
float Dot(Color a, Color b) {
  return a.r * b.r + a.g * b.g + a.b * b.b + a.a * b.a;
}
float TexelLinear(std::uint8_t v) {
  static const auto table = [] {
    std::array<float, 256> a{};
    for (unsigned i = 0; i < 256; ++i)
      a[i] = Linear(float(i) / 255.f);
    return a;
  }();
  return table[v];
}
Color Sample(const ce_unity::Image &im, float u, float v, bool repeat,
             bool srgb) {
  const float px = u * float(im.width) - .5f,
              py = (1.f - v) * float(im.height) - .5f;
  const auto x0 = static_cast<int>(std::floor(px)),
             y0 = static_cast<int>(std::floor(py));
  auto index = [repeat](int i, int n) {
    if (repeat) {
      i %= n;
      if (i < 0)
        i += n;
      return i;
    }
    return std::clamp(i, 0, n - 1);
  };
  auto texel = [&](int x, int y) {
    const auto o = (std::size_t(index(y, int(im.height))) * im.width +
                    std::size_t(index(x, int(im.width)))) *
                   4;
    Color c{im.rgba[o] / 255.f, im.rgba[o + 1] / 255.f, im.rgba[o + 2] / 255.f,
            im.rgba[o + 3] / 255.f};
    if (srgb) {
      c.r = TexelLinear(im.rgba[o]);
      c.g = TexelLinear(im.rgba[o + 1]);
      c.b = TexelLinear(im.rgba[o + 2]);
    }
    return c;
  };
  return Mix(Mix(texel(x0, y0), texel(x0 + 1, y0), px - float(x0)),
             Mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), px - float(x0)),
             py - float(y0));
}
bool ImageValid(const ce_unity::Image &i) {
  return i.width && i.height && i.width <= 2048 && i.height <= 2048 &&
         i.rgba.size() == std::size_t(i.width) * i.height * 4;
}
bool Finite(belmont_map_fog::Rect r) {
  return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
         std::isfinite(r.height) && r.width > 0 && r.height > 0 &&
         r.width <= 512 && r.height <= 512;
}
} // namespace
float ShapeDistance(const belmont_map_fog::Shape &s, float x, float y) {
  if (s.kind == belmont_map_fog::ShapeKind::Box) {
    const float qx = std::abs(x - s.position.x) - s.size.x * .5f,
                qy = std::abs(y - s.position.y) - s.size.y * .5f;
    return std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) +
           std::min(std::max(qx, qy), 0.f);
  }
  float best = std::numeric_limits<float>::max();
  bool inside = false, degenerate = false;
  for (unsigned i = 0; i < 4; ++i) {
    const auto a = s.vertices[i], b = s.vertices[(i + 1) % 4];
    const float dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy;
    if (length == 0) {
      degenerate = true;
      continue;
    }
    const float t = Clamp(((x - a.x) * dx + (y - a.y) * dy) / length);
    best = std::min(best, std::hypot(x - a.x - t * dx, y - a.y - t * dy));
    if ((a.y > y) != (b.y > y)) {
      const float cross = (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x;
      if (x < cross)
        inside = !inside;
    }
  }
  // Native authored triangles are four-vertex quads with a repeated edge.
  // The native four-edge sign test produces zero on their interior.
  return inside ? (degenerate ? 0.f : -best) : best;
}
float SmoothUnion(float a, float b, float k) {
  if (k <= 0)
    return std::min(a, b);
  const float h = std::max(k - std::abs(a - b), 0.f);
  return std::min(a, b) - h * h / (4.f * k);
}
struct Renderer::State {
  std::recursive_mutex mutex;
  ce_unity::Reader unity;
  belmont_assets::Reader assets;
  belmont_map_tiles::Reader tiles;
  belmont_map_fog::Reader fog;
  std::optional<belmont_map_fog::Data> fog_data;
  std::optional<belmont_map_tiles::Layer> geometry, skyline;
  std::optional<ce_unity::Image> background, noise;
  std::optional<belmont_map_tiles::Tile> tile;
  std::map<std::string, float, std::less<>> numbers;
  std::map<std::string, Color, std::less<>> vectors;
  bool ready{};
  State(ce_unity::RangeReader range, ce_unity::AstcDecoder astc)
      : unity(range, astc), assets(range, astc), tiles(range, astc),
        fog(range) {}
  bool Load() {
    if (ready)
      return true;
    auto d = fog.LoadStreetsPoor();
    auto l = tiles.LoadStreetsPoorLayout();
    if (!d || !l)
      return false;
    auto t = std::find_if(l->begin(), l->end(),
                          [](const auto &q) { return q.layer == 16; });
    if (t == l->end())
      return false;
    auto g = tiles.LoadStreetsPoorLayer(belmont_map_tiles::Source::Geometry,
                                        16),
         s = tiles.LoadStreetsPoorLayer(belmont_map_tiles::Source::Background,
                                        16);
    const auto prefix = "native:" + std::string(Menus) + "#";
    auto bg = assets.LoadImage(prefix + "-4869051870819595246"),
         n = assets.LoadImage(prefix + "1330824986764308962");
    auto material = unity.ReadObject(Art, -1839235872175495265LL);
    if (!g || !s || !bg || !n || !material || !ImageValid(g->image) ||
        !ImageValid(s->image) || !ImageValid(*bg) || !ImageValid(*n))
      return false;
    const auto *fp = material->Path("m_SavedProperties.m_Floats");
    const auto *cp = material->Path("m_SavedProperties.m_Colors");
    if (!fp || !cp || fp->items.size() > 256 || cp->items.size() > 256)
      return false;
    numbers.clear();
    vectors.clear();
    for (const auto &pair : fp->items) {
      if (pair.items.size() != 2)
        return false;
      const float v = float(pair.items[1].AsFloat());
      if (!std::isfinite(v))
        return false;
      numbers.emplace(std::string(pair.items[0].AsString()), v);
    }
    for (const auto &pair : cp->items) {
      if (pair.items.size() != 2)
        return false;
      const auto &v = pair.items[1];
      Color q;
      const char *keys[] = {"r", "g", "b", "a"};
      float vals[4];
      for (int j = 0; j < 4; ++j) {
        const auto *f = v.Get(keys[j]);
        if (!f)
          return false;
        vals[j] = float(f->AsFloat());
        if (!std::isfinite(vals[j]))
          return false;
      }
      q = {vals[0], vals[1], vals[2], vals[3]};
      vectors.emplace(std::string(pair.items[0].AsString()), q);
    }
    for (const auto name :
         {"_SelectedBackColor", "_SelectedEdgeColor", "_LineColor",
          "_BackgroundST", "_SDGMaskNoiseTexST", "_BakcgroundNoiseMask"})
      if (!vectors.contains(name))
        return false;
    for (const auto name :
         {"_EdgeDetectMin", "_EdgeDetectMax", "_AlphaSDFRange",
          "_AlphaSDFContrast", "_CloudSDFMaskRange", "_CloudSDFMaskContrast"})
      if (!numbers.contains(name))
        return false;
    fog_data = std::move(d);
    tile = *t;
    geometry = std::move(g);
    skyline = std::move(s);
    background = std::move(bg);
    noise = std::move(n);
    ready = true;
    return true;
  }
  Color Property(std::string_view key, bool is_color = false) const {
    auto p = vectors.find(key);
    Color v = p == vectors.end() ? Color{} : p->second;
    if (is_color) {
      v.r = Linear(v.r);
      v.g = Linear(v.g);
      v.b = Linear(v.b);
    }
    return v;
  }
  float Number(std::string_view key) const {
    auto p = numbers.find(key);
    return p == numbers.end() ? 0.f : p->second;
  }
};
Renderer::Renderer(ce_unity::RangeReader r, ce_unity::AstcDecoder a)
    : state(std::make_shared<State>(std::move(r), std::move(a))) {}
Renderer::Renderer(const EdenDsmodHostApi *h, ce_unity::AstcDecoder a)
    : Renderer(ce_unity::MakeHostRangeReader(h), std::move(a)) {}
void Renderer::ClearCache() {
  std::lock_guard lock(state->mutex);
  state->ready = false;
  state->fog_data.reset();
  state->geometry.reset();
  state->skyline.reset();
  state->background.reset();
  state->noise.reset();
  state->tile.reset();
  state->unity.ClearCache();
  state->assets.ClearCache();
  state->tiles.ClearCache();
  state->fog.ClearCache();
}
std::optional<Frame> Renderer::Render(const Request &req) {
  if (req.current_room_id != FirstRoom || !std::isfinite(req.player_x) ||
      !std::isfinite(req.player_y) || req.width < 32 || req.height < 32 ||
      req.width > 1240 || req.height > 1080 || req.explored_rooms.size() > 64 ||
      req.culled_navigation_zone_ids.size() > 256)
    return {};
  std::lock_guard lock(state->mutex);
  if (!state->Load())
    return {};
  auto &s = *state;
  const auto room =
      std::find_if(s.fog_data->rooms.begin(), s.fog_data->rooms.end(),
                   [](const auto &r) { return r.id == FirstRoom; });
  if (room == s.fog_data->rooms.end())
    return {};
  // Follow the current room's native navigation district. The initial slice
  // has one unambiguous navigation identity across all source zones; an
  // unknown/multiple identity is unavailable rather than guessed from UI state.
  if (room->zones.empty() || room->zones.front().navigation_zone_id == 0)
    return {};
  const auto navigation = room->zones.front().navigation_zone_id;
  if (!std::all_of(room->zones.begin(), room->zones.end(),
                   [&](const auto &zone) {
                     return zone.navigation_zone_id == navigation;
                   }))
    return {};
  if (std::find(req.culled_navigation_zone_ids.begin(),
                req.culled_navigation_zone_ids.end(),
                navigation) != req.culled_navigation_zone_ids.end())
    return {};

  std::set<int> ids;
  for (const auto &e : req.explored_rooms) {
    if (e.room_id != FirstRoom)
      continue;
    if (e.entered_zone_ids.size() > 256)
      return {};
    for (auto id : e.entered_zone_ids)
      ids.insert(id);
  }
  if (ids.empty())
    return {};
  std::vector<const belmont_map_fog::Shape *> boxes, quads;
  unsigned zones = 0;
  float minx = std::numeric_limits<float>::max(), miny = minx, maxx = -minx,
        maxy = -minx;
  for (int id : ids) {
    auto z = std::find_if(room->zones.begin(), room->zones.end(),
                          [&](const auto &v) { return v.id == id; });
    if (z == room->zones.end())
      return {};
    if (std::find(req.culled_navigation_zone_ids.begin(),
                  req.culled_navigation_zone_ids.end(),
                  z->navigation_zone_id) !=
        req.culled_navigation_zone_ids.end())
      continue;
    ++zones;
    for (const auto &sh : z->shapes) {
      (sh.kind == belmont_map_fog::ShapeKind::Box ? boxes : quads)
          .push_back(&sh);
      minx = std::min(minx, sh.bounds.x);
      miny = std::min(miny, sh.bounds.y);
      maxx = std::max(maxx, sh.bounds.x + sh.bounds.width);
      maxy = std::max(maxy, sh.bounds.y + sh.bounds.height);
    }
  }
  if (!zones || boxes.size() + quads.size() > 128)
    return {};
  auto settings = std::find_if(
      s.fog_data->tiles.begin(), s.fog_data->tiles.end(),
      [&](const auto &t) { return t.renderer_id == s.tile->renderer_id; });
  if (settings == s.fog_data->tiles.end() || settings->smoothness <= 0 ||
      settings->smoothness > 16 || settings->box_roundness != 0 ||
      settings->quad_roundness != 0)
    return {};
  const float aspect = float(req.width) / float(req.height);
  float ww = std::max(96.f, maxx - minx + 2 * s.fog_data->tile_bounds_inflate),
        hh = std::max(52.f, maxy - miny + 2 * s.fog_data->tile_bounds_inflate);
  if (ww / hh < aspect)
    ww = hh * aspect;
  else
    hh = ww / aspect;
  belmont_map_fog::Rect view = req.world_bounds.value_or(belmont_map_fog::Rect{
      (minx + maxx - ww) * .5f, (miny + maxy - hh) * .5f, ww, hh});
  if (!Finite(view))
    return {};
  auto distance = [&](float x, float y) {
    float d = std::numeric_limits<float>::max();
    for (const auto *shape : boxes)
      d = SmoothUnion(d, ShapeDistance(*shape, x, y), settings->smoothness);
    for (const auto *shape : quads)
      d = SmoothUnion(d, ShapeDistance(*shape, x, y), settings->smoothness);
    return d;
  };
  // Source-authored world SDF, sampled at the native .1 screen resolution,
  // quantized R8 and bilinearly reconstructed. No captured/derived map pixels.
  const unsigned mw = std::max(
                     1u, unsigned(std::nearbyint(float(req.width) * .1f))),
                 mh = std::max(
                     1u, unsigned(std::nearbyint(float(req.height) * .1f)));
  ce_unity::Image mask{mw, mh,
                       std::vector<std::uint8_t>(std::size_t(mw) * mh * 4)};
  for (unsigned y = 0; y < mh; ++y)
    for (unsigned x = 0; x < mw; ++x) {
      const float wx = view.x + (float(x) + .5f) * view.width / float(mw),
                  wy = view.y + (float(mh - y) - .5f) * view.height / float(mh);
      const auto b =
          std::uint8_t(std::lround(Clamp(distance(wx, wy) * .1f) * 255.f));
      const auto o = (std::size_t(y) * mw + x) * 4;
      mask.rgba[o] = mask.rgba[o + 1] = mask.rgba[o + 2] = b;
      mask.rgba[o + 3] = 255;
    }
  Frame frame{
      {req.width, req.height,
       std::vector<std::uint8_t>(std::size_t(req.width) * req.height * 4)},
      view,
      zones,
      true};
  const auto back = s.Property("_SelectedBackColor", true),
             edge = s.Property("_SelectedEdgeColor", true),
             bgst = s.Property("_BackgroundST"),
             nst = s.Property("_SDGMaskNoiseTexST"),
             noise_weights = s.Property("_BakcgroundNoiseMask");
  const auto &tb = s.tile->bounds;
  const float emin = s.Number("_EdgeDetectMin"),
              emax = s.Number("_EdgeDetectMax");
  if (emax <= emin)
    return {};
  for (unsigned y = 0; y < req.height; ++y)
    for (unsigned x = 0; x < req.width; ++x) {
      const float vx = (float(x) + .5f) / float(req.width),
                  vy = (float(req.height - y) - .5f) / float(req.height),
                  wx = view.x + vx * view.width, wy = view.y + vy * view.height;
      const float u = (wx - tb.x) / tb.width, v = (wy - tb.y) / tb.height;
      const auto noise = Sample(*s.noise, wx * nst.r, wy * nst.g, true, false);
      Color color = Sample(*s.background, wx * bgst.r, wy * bgst.g, true, true);
      color = Scale(color, Clamp(1.f - .25f * Dot(noise, noise_weights)));
      if (u >= 0 && u <= 1 && v >= 0 && v <= 1) {
        const auto skyline =
            Sample(s.skyline->image, u, v, false, s.skyline->srgb);
        color = Scale(color, skyline.r);
        const auto fields =
            Sample(s.geometry->image, u, v, false, s.geometry->srgb);
        const float fog = Sample(mask, vx, vy, false, false).r * 10.f;
        // Native contour/color roles. Still decorative composition
        // intentionally omits animated water, moving/displaced noise and SDG
        // border ornaments.
        const float coverage = 1.f - fields.r,
                    outline = Smooth((fields.b - emin) / (emax - emin));
        const float opacity =
            1.f -
            Smooth((fields.g - std::max(s.Number("_AlphaSDFRange"), .001f)) /
                   std::max(.0001f, 1.f - s.Number("_AlphaSDFContrast")));
        if (fog <= 0.f) {
          Color ink = Mix(Scale(back, coverage), edge, outline);
          ink = Scale(ink, (1.f - .1f * noise.g));
          color = Mix(color, ink, opacity);
        }
      }
      const auto o = (std::size_t(y) * req.width + x) * 4;
      frame.image.rgba[o] = std::uint8_t(std::lround(Gamma(color.r) * 255.f));
      frame.image.rgba[o + 1] =
          std::uint8_t(std::lround(Gamma(color.g) * 255.f));
      frame.image.rgba[o + 2] =
          std::uint8_t(std::lround(Gamma(color.b) * 255.f));
      frame.image.rgba[o + 3] = 255;
    }
  return frame;
}
} // namespace belmont_map_render
