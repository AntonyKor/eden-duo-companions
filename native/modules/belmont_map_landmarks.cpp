// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belmont_map_landmarks.h"
#include "belmont_map_refs.h"
#include <algorithm>
#include <cmath>
#include <set>
namespace belmont_map_landmarks {
namespace {
using V = ce_unity::Value;
constexpr std::string_view Savable = "savablescriptableobject_assets_all_"
                                     "5b62e0c70cf11954564852274ab4ce5a.bundle";
constexpr std::string_view SavableCab = "CAB-7268582d7b52b05a701bb84d36e366c4";
constexpr std::string_view Menu =
    "ui-gamemenus_assets_all_866519f7d6a7057a8ba1c459a5b3f694.bundle";
constexpr std::string_view Common =
    "ui-commonsingame_assets_all_acb23e9b0608a368aad72d2fe488a679.bundle";
constexpr std::string_view CommonCab = "CAB-5d3eb16e2bd4e768158db17f3956cfa9";
constexpr std::string_view MenuCab = "CAB-27e7a4fd03161a1208808cab8fc89755";
bool Int(const V *v, std::int64_t &n) {
  if (!v)
    return false;
  if (v->kind == V::Kind::Int) {
    n = v->i;
    return true;
  }
  if (v->kind == V::Kind::UInt && v->u <= INT64_MAX) {
    n = v->u;
    return true;
  }
  return false;
}
bool Num(const V *v, float &f) {
  if (!v || v->kind != V::Kind::Float || !std::isfinite(v->f) ||
      std::abs(v->f) > 1e7)
    return false;
  f = v->f;
  return true;
}
bool Ref(const V *v, std::int64_t &id, std::int64_t &file) {
  return v && Int(v->Get("m_FileID"), file) && Int(v->Get("m_PathID"), id);
}
std::int64_t Local(const V *v) {
  std::int64_t id{}, f{};
  return Ref(v, id, f) && f == 0 ? id : 0;
}
const std::vector<V> *Array(const V *v, std::size_t max = 256) {
  return v && v->kind == V::Kind::Array && v->items.size() <= max ? &v->items
                                                                  : nullptr;
}
std::optional<std::string> Text(const V *v, std::size_t max = 256) {
  if (!v || v->kind != V::Kind::String || v->s.size() > max ||
      v->s.find('\0') != std::string::npos)
    return {};
  return v->s;
}
std::shared_ptr<const ce_unity::SerializedFile>
File(ce_unity::Reader &r, std::string_view bundle, std::string_view cab) {
  auto b = r.OpenBundle(bundle);
  auto n = b ? b->FindNode(cab) : nullptr;
  if (!n || b->engine_version != "2022.3.62f2")
    return {};
  auto f = ce_unity::SerializedFile::Open(b->NodeSource(*n));
  return f && f->Version() == 22 && f->Platform() == 38 && f->HasTypeTrees()
             ? f
             : nullptr;
}
std::optional<V> Obj(const std::shared_ptr<const ce_unity::SerializedFile> &f,
                     std::int64_t id, int cls) {
  auto o = f ? f->Find(id) : nullptr;
  return o && o->class_id == cls && o->byte_size < 4 * 1024 * 1024
             ? f->ReadObject(*o)
             : std::nullopt;
}
bool Metadata(const V &s, Sprite &art) {
  auto sn = Text(s.Get("m_Name"));
  std::int64_t tf{};
  if (!sn || !Num(s.Path("m_Rect.width"), art.width) ||
      !Num(s.Path("m_Rect.height"), art.height) ||
      !Num(s.Path("m_Rect.x"), art.rect.x) ||
      !Num(s.Path("m_Rect.y"), art.rect.y) ||
      !Num(s.Path("m_Pivot.x"), art.pivot_x) ||
      !Num(s.Path("m_Pivot.y"), art.pivot_y) ||
      !Num(s.Get("m_PixelsToUnits"), art.pixels_per_unit) ||
      art.pixels_per_unit <= 0 ||
      !Ref(s.Path("m_RD.texture"), art.texture_id, tf) || tf != 0 ||
      !art.texture_id)
    return false;
  art.name = *sn;
  art.rect.width = art.width;
  art.rect.height = art.height;
  auto tr = s.Path("m_RD.textureRect");
  if (!tr || !Num(tr->Get("x"), art.texture_rect.x) ||
      !Num(tr->Get("y"), art.texture_rect.y) ||
      !Num(tr->Get("width"), art.texture_rect.width) ||
      !Num(tr->Get("height"), art.texture_rect.height))
    return false;
  std::int64_t settings{};
  if (!Num(s.Path("m_RD.textureRectOffset.x"), art.texture_rect_offset_x) ||
      !Num(s.Path("m_RD.textureRectOffset.y"), art.texture_rect_offset_y) ||
      !Int(s.Path("m_RD.settingsRaw"), settings) || settings < 0 ||
      std::uint64_t(settings) > UINT32_MAX)
    return false;
  art.settings_raw = static_cast<std::uint32_t>(settings);
  const char *c[] = {"x", "y", "z", "w"};
  auto border = s.Get("m_Border");
  for (unsigned k = 0; k < 4; ++k)
    if (!border || !Num(border->Get(c[k]), art.border[k]))
      return false;
  return true;
}
// Managed-reference payloads are not interpreted. This bounded prefix schema
// reads identity/localization and rejects every condition-dependent policy.
struct NativeData {
  std::string guid, table;
  std::int64_t key{};
  bool unconditional{};
};
std::optional<NativeData>
LandmarkData(const std::shared_ptr<const ce_unity::SerializedFile> &sf,
             std::int64_t id) {
  auto o = sf->Find(id);
  if (!o || o->class_id != 114 || o->byte_size > 65536)
    return {};
  auto raw = sf->ReadRaw(*o);
  if (!raw)
    return {};
  struct Cursor {
    std::span<const std::uint8_t> b;
    std::size_t p{};
    bool ok{true};
    std::uint32_t U() {
      if (p > b.size() || b.size() - p < 4) {
        ok = false;
        return 0;
      }
      auto v = std::uint32_t(b[p]) | std::uint32_t(b[p + 1]) << 8 |
               std::uint32_t(b[p + 2]) << 16 | std::uint32_t(b[p + 3]) << 24;
      p += 4;
      return v;
    }
    std::int64_t I() {
      auto lo = U();
      auto hi = U();
      return std::int64_t(std::uint64_t(lo) | std::uint64_t(hi) << 32);
    }
    std::string S(std::size_t max) {
      auto n = U();
      if (n > max || p > b.size() || n > b.size() - p) {
        ok = false;
        return {};
      }
      std::string s(reinterpret_cast<const char *>(b.data() + p), n);
      p = (p + n + 3) & ~std::size_t(3);
      if (p > b.size() || s.find('\0') != std::string::npos)
        ok = false;
      return s;
    }
    std::uint32_t Count(unsigned stride) {
      auto n = U();
      if (n > 128 || p > b.size() || std::uint64_t(n) * stride > b.size() - p) {
        ok = false;
        return 0;
      }
      p += n * stride;
      return n;
    }
  } c{*raw};
  if (c.U() != 0 || c.I() != 0 || c.U() != 1 || c.U() != 1 ||
      c.I() != 7762847280348797211LL || !c.S(256).starts_with("Landmark_"))
    return {};
  NativeData d;
  d.guid = c.S(32);
  d.table = c.S(64);
  d.key = c.I();
  c.S(256);
  auto fallback = c.U();
  auto wait = c.U();
  auto vars = c.U();
  if (fallback > 2 || wait > 1 || vars != 0)
    return {};
  auto reveal = c.I();
  auto steps = c.Count(8), sub = c.Count(8), names = c.Count(4),
       subnames = c.Count(4);
  if (!c.ok)
    return {};
  d.unconditional = reveal == -2 && !steps && !sub && !names && !subnames;
  return d;
}
struct Transform {
  float x{}, y{}, sx{1}, sy{1};
};
struct Scene {
  std::shared_ptr<const ce_unity::SerializedFile> file;
  std::map<std::int64_t, V> values;
  std::map<std::int64_t, std::int64_t> transforms;
  const V *Get(std::int64_t id) {
    auto it = values.find(id);
    if (it != values.end())
      return &it->second;
    auto o = file->Find(id);
    auto v = o ? file->ReadObject(*o) : std::nullopt;
    if (!v)
      return nullptr;
    return &values.emplace(id, std::move(*v)).first->second;
  }
  std::optional<Transform> World(std::int64_t id,
                                 std::set<std::int64_t> seen = {}) {
    if (seen.size() > 64 || !seen.insert(id).second)
      return {};
    auto t = Get(id);
    if (!t)
      return {};
    Transform a;
    float z{}, qx{}, qy{}, qz{}, qw{};
    if (!Num(t->Path("m_LocalPosition.x"), a.x) ||
        !Num(t->Path("m_LocalPosition.y"), a.y) ||
        !Num(t->Path("m_LocalScale.x"), a.sx) ||
        !Num(t->Path("m_LocalScale.y"), a.sy) ||
        !Num(t->Path("m_LocalScale.z"), z) ||
        !Num(t->Path("m_LocalRotation.x"), qx) ||
        !Num(t->Path("m_LocalRotation.y"), qy) ||
        !Num(t->Path("m_LocalRotation.z"), qz) ||
        !Num(t->Path("m_LocalRotation.w"), qw) || qx != 0 || qy != 0 ||
        qz != 0 || std::abs(qw) != 1 || a.sx <= 0 || a.sy <= 0)
      return {};
    auto parent = Local(t->Get("m_Father"));
    if (parent) {
      auto b = World(parent, std::move(seen));
      if (!b)
        return {};
      a.x = b->x + a.x * b->sx;
      a.y = b->y + a.y * b->sy;
      a.sx *= b->sx;
      a.sy *= b->sy;
    }
    return a;
  }
};
bool ExternalIs(const std::vector<std::string> &cabs, const V *v,
                std::string_view cab, std::int64_t &id) {
  std::int64_t f{};
  return Ref(v, id, f) && id && f > 0 && std::uint64_t(f) <= cabs.size() &&
         cabs[f - 1] == cab;
}
} // namespace
bool Visible(const Landmark &l, std::span<const std::string> known,
             std::span<const std::int64_t> culled) {
  return l.unconditional_policy && !l.guid.empty() &&
         std::find(known.begin(), known.end(), l.guid) != known.end() &&
         std::find(culled.begin(), culled.end(), l.navigation_zone_id) ==
             culled.end();
}
Reader::Reader(ce_unity::RangeReader r) : unity(std::move(r)) {}
Reader::Reader(const EdenDsmodHostApi *h)
    : Reader(ce_unity::MakeHostRangeReader(h)) {}
void Reader::ClearCache() {
  std::lock_guard lock(mutex);
  cache.reset();
  unity.ClearCache();
}
std::optional<Data> Reader::LoadForRoom(std::string_view room) {
  std::lock_guard lock(mutex);
  try {
    auto source = belmont_map_sources::FindSourceForRoom(room);
    if (!source)
      return {};
    if (cache && cache->world_name == source->world_name)
      return cache;
    auto sf = File(unity, source->map_bundle, source->map_cab);
    auto data = File(unity, Savable, SavableCab);
    auto menu = File(unity, Menu, MenuCab);
    auto common = File(unity, Common, CommonCab);
    auto b = unity.OpenBundle(source->map_bundle);
    auto node = b ? b->FindNode(source->map_cab) : nullptr;
    auto cabs = node ? belmont_map_refs::ExternalCabs(b->NodeSource(*node))
                     : std::nullopt;
    if (!sf || !data || !menu || !common || !cabs)
      return {};
    Scene scene{sf, {}, {}};
    Data out{std::string(source->world_name), {}};
    for (const auto &o : sf->Objects())
      if (o.class_id == 4 || o.class_id == 224) {
        auto t = scene.Get(o.path_id);
        auto go = t ? Local(t->Get("m_GameObject")) : 0;
        if (go && !scene.transforms.emplace(go, o.path_id).second)
          return {};
      }
    for (const auto &o : sf->Objects()) {
      if (o.class_id != 114)
        continue;
      auto v = scene.Get(o.path_id);
      if (!v || !v->Get("landmarkData"))
        continue;
      Landmark l;
      l.source_id = o.path_id;
      auto roomname = Text(v->Get("itemWorldRoomId"));
      std::int64_t dataid{}, zone{};
      auto go = Local(v->Get("m_GameObject"));
      auto g = scene.Get(go);
      auto name = g ? Text(g->Get("m_Name")) : std::nullopt;
      if (!roomname || !roomname->starts_with(source->world_name) || !name ||
          !name->starts_with("LevelMap_Landmark_") ||
          Local(v->Get("context")) != source->map_context_id ||
          !ExternalIs(*cabs, v->Get("landmarkData"), SavableCab, dataid) ||
          !Int(v->Get("itemExplorationZoneId"), zone) || zone < 0 ||
          zone > 65535)
        return {};
      auto d = LandmarkData(data, dataid);
      if (!d || d->guid.size() != 32 ||
          !std::all_of(d->guid.begin(), d->guid.end(),
                       [](char c) {
                         return (c >= '0' && c <= '9') ||
                                (c >= 'a' && c <= 'f');
                       }) ||
          !d->table.starts_with("GUID:") || d->table.size() != 37 || d->key < 0)
        return {};
      l.guid = d->guid;
      l.room_id = *roomname;
      l.source_name = *name;
      l.name_table = d->table;
      l.name_key_id = d->key;
      l.exploration_zone_id = zone;
      l.navigation_zone_id = Local(v->Get("navigationZone"));
      l.unconditional_policy = d->unconditional;
      auto ti = scene.transforms.find(go);
      auto pos =
          ti != scene.transforms.end() ? scene.World(ti->second) : std::nullopt;
      if (!pos)
        return {};
      l.x = pos->x;
      l.y = pos->y;
      l.name_x = l.x;
      l.name_y = l.y;
      // Find only native step zero under the linked display hierarchy. Other
      // states need condition readers and are deliberately not guessed from
      // active flags.
      std::set<std::int64_t> zero_roots, excluded_roots;
      auto display = scene.Get(Local(v->Get("display")));
      auto displays = display ? Array(display->Get("displays")) : nullptr;
      if (displays)
        for (const auto &dr : *displays) {
          auto dv = scene.Get(Local(&dr));
          if (!dv)
            continue;
          if (auto steps = Array(dv->Get("stepBasedObjects"))) {
            for (std::size_t i = 0; i < steps->size(); ++i)
              (i ? excluded_roots : zero_roots).insert(Local(&(*steps)[i]));
          }
          if (dv->Get("landmarkNameText")) {
            auto ng = Local(dv->Get("m_GameObject"));
            auto nt = scene.transforms.find(ng);
            auto np = nt != scene.transforms.end() ? scene.World(nt->second)
                                                   : std::nullopt;
            if (np) {
              l.name_x = np->x;
              l.name_y = np->y;
            }
            auto localized = scene.Get(Local(dv->Get("landmarkNameText")));
            auto text = localized
                            ? scene.Get(Local(localized->Get("textDisplay")))
                            : nullptr;
            if (text) {
              Num(text->Get("m_fontSize"), l.name_style.font_size);
              Int(text->Get("m_fontStyle"), l.name_style.font_style);
              std::int64_t ff{};
              if (Ref(text->Get("m_fontAsset"), l.name_style.font_asset_id,
                      ff) &&
                  ff > 0 && std::uint64_t(ff) <= cabs->size())
                l.name_style.font_asset_cab = (*cabs)[ff - 1];
              Int(text->Get("m_HorizontalAlignment"),
                  l.name_style.horizontal_alignment);
              Int(text->Get("m_VerticalAlignment"),
                  l.name_style.vertical_alignment);
              const char *c[] = {"r", "g", "b", "a"};
              const char *m[] = {"x", "y", "z", "w"};
              for (unsigned k = 0; k < 4; ++k) {
                auto color = text->Get("m_fontColor");
                auto margin = text->Get("m_margin");
                if (color)
                  Num(color->Get(c[k]), l.name_style.color[k]);
                if (margin)
                  Num(margin->Get(m[k]), l.name_style.margin[k]);
              }
              auto tg = Local(text->Get("m_GameObject"));
              auto tt = scene.transforms.find(tg);
              auto tp = tt != scene.transforms.end() ? scene.World(tt->second)
                                                     : std::nullopt;
              if (tp) {
                l.name_style.scale_x = tp->sx;
                l.name_style.scale_y = tp->sy;
              }
            }
            // Native UI Image and responsive sizing components under this
            // display wrapper. Layout fitter sizes ribbon from real text.
            for (const auto &candidate : sf->Objects())
              if (candidate.class_id == 114) {
                auto cv = scene.Get(candidate.path_id);
                if (!cv)
                  continue;
                auto cg = Local(cv->Get("m_GameObject"));
                auto ct = scene.transforms.find(cg);
                if (ct == scene.transforms.end())
                  continue;
                auto at = ct->second;
                std::set<std::int64_t> ancestors;
                bool child = false;
                while (at && ancestors.size() < 64 &&
                       ancestors.insert(at).second) {
                  auto tr = scene.Get(at);
                  if (!tr)
                    break;
                  child |= Local(tr->Get("m_GameObject")) == ng;
                  at = Local(tr->Get("m_Father"));
                }
                if (!child)
                  continue;
                if (cv->Get("screenPercentage")) {
                  Num(cv->Get("screenPercentage"),
                      l.name_style.screen_percentage);
                  auto rs = scene.World(Local(cv->Get("sizableObject")));
                  if (rs) {
                    l.name_style.responsive_scale_x = rs->sx;
                    l.name_style.responsive_scale_y = rs->sy;
                  }
                }
                if (cv->Get("m_PixelsPerUnitMultiplier")) {
                  std::int64_t file{};
                  Ref(cv->Get("m_Sprite"), l.name_style.ribbon_sprite_id, file);
                  std::int64_t ribbon_id{};
                  if (ExternalIs(*cabs, cv->Get("m_Sprite"), CommonCab,
                                 ribbon_id)) {
                    auto native = Obj(common, ribbon_id, 213);
                    Sprite ribbon;
                    ribbon.id = ribbon_id;
                    ribbon.bundle = Common;
                    ribbon.cab = CommonCab;
                    if (native && Metadata(*native, ribbon)) {
                      auto rp = scene.World(ct->second);
                      if (rp) {
                        ribbon.x = rp->x;
                        ribbon.y = rp->y;
                        ribbon.scale_x = rp->sx;
                        ribbon.scale_y = rp->sy;
                      }
                      l.name_style.ribbon = std::move(ribbon);
                    }
                  }
                  Num(cv->Get("m_PixelsPerUnitMultiplier"),
                      l.name_style.ribbon_pixels_per_unit_multiplier);
                  const char *channels[] = {"r", "g", "b", "a"};
                  auto color = cv->Get("m_Color");
                  for (unsigned k = 0; k < 4; ++k)
                    if (color)
                      Num(color->Get(channels[k]),
                          l.name_style.ribbon_color[k]);
                }
              }
          }
        }
      for (const auto &so : sf->Objects())
        if (so.class_id == 212) {
          auto sr = scene.Get(so.path_id);
          auto sg = sr ? Local(sr->Get("m_GameObject")) : 0;
          auto st = scene.transforms.find(sg);
          if (st == scene.transforms.end())
            continue;
          std::set<std::int64_t> seen;
          auto at = st->second;
          bool under = false, zero = false, excluded = false;
          while (at && seen.size() < 64 && seen.insert(at).second) {
            auto t = scene.Get(at);
            if (!t)
              break;
            auto ag = Local(t->Get("m_GameObject"));
            under |= ag == go;
            zero |= zero_roots.contains(ag);
            excluded |= excluded_roots.contains(ag);
            at = Local(t->Get("m_Father"));
          }
          if (!under || !zero || excluded)
            continue;
          std::int64_t sid{};
          if (!ExternalIs(*cabs, sr->Get("m_Sprite"), MenuCab, sid))
            continue;
          auto s = Obj(menu, sid, 213);
          auto p = scene.World(st->second);
          Sprite art;
          art.id = sid;
          art.bundle = Menu;
          art.cab = MenuCab;
          if (!s || !p || !Metadata(*s, art))
            return {};
          art.x = p->x;
          art.y = p->y;
          art.scale_x = p->sx;
          art.scale_y = p->sy;
          const char *channels[] = {"r", "g", "b", "a"};
          for (unsigned c = 0; c < 4; ++c) {
            auto col = sr->Get("m_Color");
            if (!col || !Num(col->Get(channels[c]), art.color[c]))
              return {};
          }
          l.sprites.push_back(std::move(art));
        }
      out.landmarks.push_back(std::move(l));
      if (out.landmarks.size() > 128)
        return {};
    }
    cache = out;
    unity.ClearCache();
    return out;
  } catch (...) {
    return {};
  }
}
} // namespace belmont_map_landmarks
