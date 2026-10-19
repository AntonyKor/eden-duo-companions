// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belmont_map_labels.h"
#include <set>
namespace belmont_map_labels {
namespace {
using V = ce_unity::Value;
constexpr std::string_view Root = "Data/StreamingAssets/aa/Switch/";
constexpr std::string_view DistrictBundle =
    "district-districtdata_streets-savablescriptableobject_assets_all_"
    "a7360e4b14d252869ffd592c17efe004.bundle";
constexpr std::string_view DisplayBundle =
    "district-districtdata_streets_assets_all_a67fbddd6cfbd9d4b54812a06cb661e6."
    "bundle";
constexpr std::string_view EnglishBundle =
    "localization-string-tables-english(en)_assets_all_"
    "aa06f0f9074c49a1bdd9e6b274bdec25.bundle";
constexpr std::string_view SharedBundle =
    "localization-assets-shared_assets_all_c0102878b4f0e869eaa8db1ab681b72a."
    "bundle";
bool I(const V *v, std::int64_t &out) {
  if (!v)
    return false;
  if (v->kind == V::Kind::Int) {
    out = v->i;
    return true;
  }
  if (v->kind == V::Kind::UInt && v->u <= std::uint64_t(INT64_MAX)) {
    out = static_cast<std::int64_t>(v->u);
    return true;
  }
  return false;
}
std::optional<std::string_view> Text(const V *v, std::size_t max = 512) {
  if (!v || v->kind != V::Kind::String || v->s.size() > max ||
      v->s.find('\0') != std::string::npos)
    return {};
  return v->s;
}
bool Equals(const V *v, std::string_view s) {
  auto t = Text(v);
  return t && *t == s;
}
const std::vector<V> *Array(const V *v, std::size_t max) {
  return v && v->kind == V::Kind::Array && v->items.size() <= max ? &v->items
                                                                  : nullptr;
}
bool Ref(const V *v, std::int64_t id, std::int32_t file = 0) {
  std::int64_t f{}, p{};
  return v && I(v->Get("m_FileID"), f) && f == file &&
         I(v->Get("m_PathID"), p) && p == id;
}
bool PlainUtf8(std::string_view s) {
  if (s.empty())
    return false;
  for (std::size_t i = 0; i < s.size();) {
    auto a = static_cast<unsigned char>(s[i++]);
    std::uint32_t cp{};
    unsigned n{};
    if (a < 128) {
      cp = a;
      n = 0;
    } else if (a >= 0xc2 && a <= 0xdf) {
      cp = a & 31;
      n = 1;
    } else if (a >= 0xe0 && a <= 0xef) {
      cp = a & 15;
      n = 2;
    } else if (a >= 0xf0 && a <= 0xf4) {
      cp = a & 7;
      n = 3;
    } else
      return false;
    if (n > s.size() - i)
      return false;
    for (unsigned j = 0; j < n; ++j) {
      auto b = static_cast<unsigned char>(s[i++]);
      if ((b & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (b & 63);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
        (n == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff) || cp < 32 || cp == 127 || cp == '<' ||
        cp == '>')
      return false;
  }
  return true;
}
std::optional<std::string> Lookup(const V &display, const V &shared,
                                  const V &english) {
  auto ref = display.Path(
      "districtDisplayName.m_TableReference.m_TableCollectionName");
  auto guid = Text(shared.Get("m_TableCollectionNameGuidString"), 32);
  auto collection = Text(ref, 37);
  std::int64_t key{};
  if (!guid || guid->size() != 32 || !collection ||
      *collection != std::string("GUID:") + std::string(*guid) ||
      !Equals(shared.Get("m_TableCollectionName"), "World_StreetPoor") ||
      !Equals(english.Get("m_Name"), "World_StreetPoor_en") ||
      !Equals(english.Path("m_LocaleId.m_Code"), "en") ||
      !Ref(english.Get("m_SharedData"), -7953798063310951156LL, 2) ||
      !I(display.Path("districtDisplayName.m_TableEntryReference.m_KeyId"),
         key) ||
      key <= 0 ||
      !Equals(display.Path("districtDisplayName.m_TableEntryReference.m_Key"),
              ""))
    return {};
  const auto *shared_rows = Array(shared.Get("m_Entries"), 4096);
  const auto *rows = Array(english.Get("m_TableData"), 4096);
  if (!shared_rows || !rows)
    return {};
  bool shared_found = false;
  std::set<std::int64_t> keys;
  for (const auto &r : *shared_rows) {
    std::int64_t k{};
    if (!I(r.Get("m_Id"), k) || k <= 0 || !keys.insert(k).second)
      return {};
    if (k == key)
      shared_found = true;
  }
  if (!shared_found)
    return {};
  keys.clear();
  std::optional<std::string> result;
  for (const auto &r : *rows) {
    std::int64_t k{};
    if (!I(r.Get("m_Id"), k) || k <= 0 || !keys.insert(k).second)
      return {};
    if (k == key) {
      auto t = Text(r.Get("m_Localized"));
      if (!t || !PlainUtf8(*t))
        return {};
      result = std::string(*t);
    }
  }
  return result;
}
std::optional<V> Object(ce_unity::Reader &reader, std::string_view bundle,
                        std::string_view cab, std::int64_t id,
                        std::string_view name) {
  auto b = reader.OpenBundle(bundle);
  auto sf = reader.OpenSerialized(bundle);
  if (!b || !sf || b->engine_version != "2022.3.62f2" || b->Nodes().empty() ||
      b->Nodes().front().path != cab || sf->Version() != 22 ||
      sf->Platform() != 38 || !sf->HasTypeTrees())
    return {};
  auto info = sf->Find(id);
  if (!info || info->class_id != 114 || info->byte_size > 1024 * 1024)
    return {};
  auto v = sf->ReadObject(*info);
  if (!v || !Equals(v->Get("m_Name"), name))
    return {};
  return v;
}
} // namespace
Reader::Reader(ce_unity::RangeReader romfs)
    : unity(std::move(romfs), {}, std::string(Root)) {}
Reader::Reader(const EdenDsmodHostApi *host)
    : Reader(ce_unity::MakeHostRangeReader(host)) {}
void Reader::ClearCache() {
  std::lock_guard lock(mutex);
  cache.reset();
  unity.ClearCache();
}
std::optional<std::string> Reader::LoadFirstRoomAreaEnglish() {
  std::lock_guard lock(mutex);
  if (cache)
    return cache;
  try {
    auto district =
        Object(unity, DistrictBundle, "CAB-2bdcac95e489306fe0cbfd0017b71428",
               -1035143152989277996LL, "StreetsPoor_District01");
    if (!district || !Equals(district->Path("displayData.m_AssetGUID"),
                             "cf5ca7526ea50a54a9f5fdaef5581335"))
      return {};
    const auto *rooms = Array(district->Get("districtRooms"), 128);
    if (!rooms || rooms->size() != 1 ||
        !Equals((*rooms)[0].Get("worldRoomUniqueID"),
                "World_Biome_01_Streets_Poor_StreetsPoor_FirstRoom_uid73"))
      return {};
    // Addressables GUID -> bundle/container object recipe, verified privately.
    // Localized entry ID and label remain dynamic source fields.
    auto display =
        Object(unity, DisplayBundle, "CAB-9e91d3982e85d3bcae075d5fbad22610",
               -4907188047308610772LL, "StreetsPoor_District01_Display");
    auto shared =
        Object(unity, SharedBundle, "CAB-187dd10d55d33bb2e6150ae0122d9552",
               -7953798063310951156LL, "World_StreetPoor Shared Data");
    auto english =
        Object(unity, EnglishBundle, "CAB-e34d1e205cce7543c57e1e8468e132b0",
               5783849446827825953LL, "World_StreetPoor_en");
    if (!display || !shared || !english)
      return {};
    auto label = Lookup(*display, *shared, *english);
    if (!label)
      return {};
    cache = *label;
    return cache;
  } catch (...) {
    return {};
  }
}
} // namespace belmont_map_labels
