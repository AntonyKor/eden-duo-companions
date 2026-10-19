// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belmont_map_tiles.h"
#include <cmath>
#include <cstring>
#include <set>
#include <string>

namespace belmont_map_tiles {
namespace {
using V = ce_unity::Value;
constexpr std::string_view Root = "Data/StreamingAssets/aa/Switch/";
constexpr std::string_view MapBundle =
    "maps_scenes_01_streets_poor_map_1_d8baa1dae0a4b34f7e696a7a9212e535.bundle";
constexpr std::string_view MapCab = "CAB-88d9fd29421b7f4c3de59deff8f49986";
constexpr std::string_view ArtBundle =
    "art-01_streetspoor_assets_all_e33a704a7ea92e47f2c375f443aacbf1.bundle";
constexpr std::string_view MenuBundle =
    "ui-gamemenus_assets_all_866519f7d6a7057a8ba1c459a5b3f694.bundle";
struct Recipe {
  std::string_view bundle, cab, name;
  std::int64_t id;
};
std::optional<Recipe> RecipeFor(Source source) {
  switch (source) {
  case Source::Geometry:
    return Recipe{ArtBundle, "CAB-e585dad378d0230776da9c16b58103cc",
                  "LevelMap_01_Streets_Poor_img", -5583816239183366218LL};
  case Source::SecretGeometry:
    return Recipe{ArtBundle, "CAB-e585dad378d0230776da9c16b58103cc",
                  "LevelMap_01_Streets_Poor_img_secret",
                  -7269222058237514142LL};
  case Source::Background:
    return Recipe{MenuBundle, "CAB-27e7a4fd03161a1208808cab8fc89755",
                  "T_UI_01_BiomeShape_StreetPoor", -164429785721050144LL};
  }
  return {};
}
bool N(const V *v, std::int64_t &n) {
  if (!v)
    return false;
  if (v->kind == V::Kind::Int) {
    n = v->i;
    return true;
  }
  if (v->kind == V::Kind::UInt && v->u <= std::uint64_t(INT64_MAX)) {
    n = static_cast<std::int64_t>(v->u);
    return true;
  }
  return false;
}
bool F(const V *v, float &f) {
  if (!v || v->kind != V::Kind::Float || !std::isfinite(v->f))
    return false;
  f = static_cast<float>(v->f);
  return std::isfinite(f);
}
bool P(const V *v, Point &p) {
  return v && F(v->Get("x"), p.x) && F(v->Get("y"), p.y) && F(v->Get("z"), p.z);
}
bool Ref(const V *v, std::int64_t &id) {
  std::int64_t file{};
  return v && N(v->Get("m_FileID"), file) && file == 0 &&
         N(v->Get("m_PathID"), id);
}
std::optional<V>
Object(const std::shared_ptr<const ce_unity::SerializedFile> &sf,
       std::int64_t id, int type) {
  const auto *o = sf ? sf->Find(id) : nullptr;
  if (!o || o->class_id != type || o->byte_size > 4 * 1024 * 1024)
    return {};
  return sf->ReadObject(*o);
}
std::shared_ptr<const ce_unity::SerializedFile>
File(ce_unity::Reader &r, std::string_view bundle, std::string_view cab) {
  auto b = r.OpenBundle(bundle);
  if (!b || b->engine_version != "2022.3.62f2")
    return {};
  const auto *node = b->FindNode(cab);
  if (!node)
    return {};
  auto sf = !b->Nodes().empty() && b->Nodes().front().path == cab
                ? r.OpenSerialized(bundle)
                : std::shared_ptr<const ce_unity::SerializedFile>(
                      ce_unity::SerializedFile::Open(b->NodeSource(*node)));
  return sf && sf->Version() == 22 && sf->Platform() == 38 && sf->HasTypeTrees()
             ? sf
             : nullptr;
}
std::uint32_t U32(const unsigned char *p) {
  return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
         std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
bool FloatAt(const std::string &bytes, std::size_t at, float &f) {
  if (at > bytes.size() || 4 > bytes.size() - at)
    return false;
  auto bits = U32(reinterpret_cast<const unsigned char *>(bytes.data() + at));
  std::memcpy(&f, &bits, 4);
  return std::isfinite(f);
}
bool Mesh(const V &mesh, Tile &tile) {
  std::int64_t compression{}, format{}, vertices{}, stream_size{};
  const auto *stream = mesh.Get("m_StreamData");
  const auto *vd = mesh.Get("m_VertexData");
  const auto *channels = vd ? vd->Get("m_Channels") : nullptr;
  const auto *data = vd ? vd->Get("m_DataSize") : nullptr;
  const auto *indices = mesh.Get("m_IndexBuffer");
  if (!N(mesh.Get("m_MeshCompression"), compression) || compression != 0 ||
      !N(mesh.Get("m_IndexFormat"), format) || format != 0 || !stream ||
      !N(stream->Get("size"), stream_size) || stream_size != 0 || !vd ||
      !N(vd->Get("m_VertexCount"), vertices) || vertices <= 0 ||
      vertices > 65536 || !channels || channels->kind != V::Kind::Array ||
      channels->items.size() < 5 || channels->items.size() > 32 || !data ||
      data->kind != V::Kind::Bytes || !indices ||
      indices->kind != V::Kind::Bytes)
    return false;
  std::size_t stride{};
  std::array<std::size_t, 5> offsets{};
  for (std::size_t i = 0; i < channels->items.size(); ++i) {
    const auto &ch = channels->items[i];
    std::int64_t dim{}, fmt{}, s{}, off{};
    if (!N(ch.Get("dimension"), dim) || dim < 0 || dim > 4)
      return false;
    if (!dim)
      continue;
    if ((i != 0 && i != 1 && i != 4) || dim != 3 || !N(ch.Get("format"), fmt) ||
        fmt != 0 || !N(ch.Get("stream"), s) || s != 0 ||
        !N(ch.Get("offset"), off) || off < 0 || off > 256)
      return false;
    offsets[i] = static_cast<std::size_t>(off);
    stride = std::max(stride, offsets[i] + 12);
  }
  // The supported native layout is float3 position, normal, UV0.xyz.
  for (auto i : {0u, 1u, 4u}) {
    std::int64_t dim{};
    if (!N(channels->items[i].Get("dimension"), dim) || dim != 3)
      return false;
  }
  if (stride == 0 || stride > 512 ||
      std::uint64_t(vertices) * stride != data->s.size() ||
      data->s.size() > 4 * 1024 * 1024)
    return false;
  tile.vertices.resize(static_cast<std::size_t>(vertices));
  tile.uv0.resize(tile.vertices.size());
  for (std::size_t i = 0; i < tile.vertices.size(); ++i) {
    auto &p = tile.vertices[i];
    auto &uv = tile.uv0[i];
    const auto at = i * stride;
    if (!FloatAt(data->s, at + offsets[0], p.x) ||
        !FloatAt(data->s, at + offsets[0] + 4, p.y) ||
        !FloatAt(data->s, at + offsets[0] + 8, p.z) ||
        !FloatAt(data->s, at + offsets[4], uv.x) ||
        !FloatAt(data->s, at + offsets[4] + 4, uv.y) ||
        !FloatAt(data->s, at + offsets[4] + 8, uv.z) || uv.z < 0 ||
        uv.z > 127 || std::floor(uv.z) != uv.z)
      return false;
    if (i == 0)
      tile.layer = static_cast<std::uint32_t>(uv.z);
    else if (uv.z != static_cast<float>(tile.layer))
      return false;
  }
  const auto *subs = mesh.Get("m_SubMeshes");
  if (!subs || subs->kind != V::Kind::Array || subs->items.size() != 1)
    return false;
  std::int64_t first{}, count{}, topology{}, base{};
  const auto &sub = subs->items[0];
  if (!N(sub.Get("firstByte"), first) || first != 0 ||
      !N(sub.Get("indexCount"), count) || count <= 0 || count > 196608 ||
      count % 3 || !N(sub.Get("topology"), topology) || topology != 0 ||
      !N(sub.Get("baseVertex"), base) || base != 0 ||
      std::uint64_t(count) * 2 != indices->s.size())
    return false;
  for (std::int64_t i = 0; i < count; ++i) {
    auto *p =
        reinterpret_cast<const unsigned char *>(indices->s.data() + 2 * i);
    auto idx = std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8;
    if (idx >= tile.vertices.size())
      return false;
    tile.triangles.push_back(idx);
  }
  return true;
}
bool Origin(const std::shared_ptr<const ce_unity::SerializedFile> &sf,
            std::int64_t go, Tile &tile) {
  auto object = Object(sf, go, 1);
  const auto *cs = object ? object->Get("m_Component") : nullptr;
  if (!cs || cs->kind != V::Kind::Array || cs->items.size() > 64)
    return false;
  std::int64_t transform{};
  for (const auto &c : cs->items) {
    std::int64_t id{};
    if (!Ref(c.Get("component"), id))
      return false;
    const auto *info = sf->Find(id);
    if (!info)
      return false;
    if (info->class_id == 4) {
      if (transform)
        return false;
      transform = id;
    }
  }
  if (!transform)
    return false;
  std::set<std::int64_t> seen;
  tile.origin = {};
  while (transform) {
    if (seen.size() >= 64 || !seen.insert(transform).second)
      return false;
    auto t = Object(sf, transform, 4);
    Point p, scale, q;
    float qw{};
    if (!t || !P(t->Get("m_LocalPosition"), p) ||
        !P(t->Get("m_LocalScale"), scale) || !P(t->Get("m_LocalRotation"), q) ||
        !F(t->Get("m_LocalRotation")->Get("w"), qw) || scale.x != 1 ||
        scale.y != 1 || scale.z != 1 || q.x != 0 || q.y != 0 || q.z != 0 ||
        qw != 1)
      return false;
    tile.origin.x += p.x;
    tile.origin.y += p.y;
    tile.origin.z += p.z;
    if (!std::isfinite(tile.origin.x) || !std::isfinite(tile.origin.y) ||
        !std::isfinite(tile.origin.z))
      return false;
    if (!Ref(t->Get("m_Father"), transform))
      return false;
  }
  return tile.origin.x == tile.bounds.x && tile.origin.y == tile.bounds.y;
}
} // namespace
Reader::Reader(ce_unity::RangeReader romfs, ce_unity::AstcDecoder decode)
    : unity(std::move(romfs), decode, std::string(Root), 3),
      astc(std::move(decode)) {
  unity.SetUseBuiltinPins(false);
}
Reader::Reader(const EdenDsmodHostApi *host, ce_unity::AstcDecoder decode)
    : Reader(ce_unity::MakeHostRangeReader(host), std::move(decode)) {}
void Reader::ClearCache() {
  std::lock_guard lock(mutex);
  layout.reset();
  unity.ClearCache();
}
std::optional<std::vector<Tile>> Reader::LoadStreetsPoorLayout() {
  std::lock_guard lock(mutex);
  try {
    if (layout)
      return layout;
    auto sf = File(unity, MapBundle, MapCab);
    auto renderer = Object(sf, 5634, 114);
    const auto *refs = renderer ? renderer->Get("tileRenderers") : nullptr;
    if (!refs || refs->kind != V::Kind::Array || refs->items.empty() ||
        refs->items.size() > 128)
      return {};
    std::vector<Tile> decoded;
    std::set<std::uint32_t> layers;
    for (const auto &ref : refs->items) {
      Tile tile;
      std::int64_t filter{}, go{}, filter_go{};
      if (!Ref(&ref, tile.renderer_id) || !tile.renderer_id)
        return {};
      auto tr = Object(sf, tile.renderer_id, 114);
      const auto *b = tr ? tr->Get("bounds") : nullptr;
      if (!tr || !Ref(tr->Get("tileMeshFilter"), filter) ||
          !Ref(tr->Get("m_GameObject"), go) || !b ||
          !F(b->Get("x"), tile.bounds.x) || !F(b->Get("y"), tile.bounds.y) ||
          !F(b->Get("width"), tile.bounds.width) ||
          !F(b->Get("height"), tile.bounds.height) || tile.bounds.width <= 0 ||
          tile.bounds.height <= 0)
        return {};
      auto mf = Object(sf, filter, 33);
      if (!mf || !Ref(mf->Get("m_GameObject"), filter_go) ||
          !Ref(mf->Get("m_Mesh"), tile.mesh_id))
        return {};
      auto mesh = Object(sf, tile.mesh_id, 43);
      if (!mesh || !Mesh(*mesh, tile) || !Origin(sf, filter_go, tile) ||
          !layers.insert(tile.layer).second)
        return {};
      decoded.push_back(std::move(tile));
    }
    layout = decoded;
    return decoded;
  } catch (...) {
    return {};
  }
}
std::optional<Layer> Reader::LoadStreetsPoorLayer(Source source,
                                                  std::uint32_t index) {
  std::lock_guard lock(mutex);
  try {
    auto recipe = RecipeFor(source);
    if (!recipe)
      return {};
    auto sf = File(unity, recipe->bundle, recipe->cab);
    auto t = Object(sf, recipe->id, 187);
    const auto *name = t ? t->Get("m_Name") : nullptr;
    std::int64_t w{}, h{}, d{}, mips{}, fmt{}, size{}, off{}, stream_size{};
    if (!t || !name || name->kind != V::Kind::String ||
        name->s != recipe->name || !N(t->Get("m_Width"), w) ||
        !N(t->Get("m_Height"), h) || w <= 0 || h <= 0 || w > 2048 || h > 2048 ||
        !N(t->Get("m_Depth"), d) || d <= 0 || d > 128 || index >= d ||
        !N(t->Get("m_MipCount"), mips) || mips != 1 ||
        !N(t->Get("m_Format"), fmt) || !N(t->Get("m_DataSize"), size) ||
        size <= 0 || std::uint64_t(size) > ce_unity::MaxAlloc)
      return {};
    std::uint64_t layer_bytes{};
    int texture_format{};
    if (fmt == 4 || fmt == 8) {
      layer_bytes = std::uint64_t(w) * h * 4;
      texture_format = 4;
    } else if (fmt == 139) {
      if (!astc)
        return {};
      layer_bytes = std::uint64_t((w + 11) / 12) * ((h + 11) / 12) * 16;
      texture_format = 59;
    } else
      return {};
    if (layer_bytes * d != std::uint64_t(size) ||
        layer_bytes > ce_unity::MaxAlloc)
      return {};
    const auto *stream = t->Get("m_StreamData");
    const auto *path = stream ? stream->Get("path") : nullptr;
    if (!stream || !path || path->kind != V::Kind::String ||
        !N(stream->Get("offset"), off) || off < 0 ||
        !N(stream->Get("size"), stream_size) || stream_size != size)
      return {};
    auto bundle = unity.OpenBundle(recipe->bundle);
    const auto *node = bundle ? bundle->FindNode(path->s) : nullptr;
    if (!node || std::uint64_t(off) > node->size ||
        std::uint64_t(size) > node->size - std::uint64_t(off))
      return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(layer_bytes));
    if (!bundle->Read(node->offset + std::uint64_t(off) + layer_bytes * index,
                      bytes.data(), bytes.size()))
      return {};
    ce_unity::TextureInfo info;
    info.width = static_cast<int>(w);
    info.height = static_cast<int>(h);
    info.format = texture_format;
    auto image = ce_unity::DecodeTextureData(bytes, info, astc, 0);
    if (!image)
      return {};
    return Layer{std::move(*image), static_cast<std::int32_t>(fmt),
                 static_cast<std::uint32_t>(d), index, fmt == 4 || fmt == 139};
  } catch (...) {
    return {};
  }
}
} // namespace belmont_map_tiles
