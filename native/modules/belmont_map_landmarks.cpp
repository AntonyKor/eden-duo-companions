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
constexpr std::string_view LevelData="leveldata_assets_all_940f672e456a1dd57099d3d5c994fbc7.bundle";
constexpr std::string_view LevelDataCab="CAB-18af95c67205941bc13db59d8c142572";
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
bool Flag(const V *v, bool &out) {
  if (!v) return false;
  if (v->kind == V::Kind::Bool) { out=v->i!=0; return true; }
  std::int64_t n{};if(!Int(v,n)||(n!=0&&n!=1))return false;
  out=n!=0;return true;
}
const std::vector<V>* Calls(const V *v, std::string_view event) {
  auto e=v?v->Get(event):nullptr;
  return e?Array(e->Path("m_PersistentCalls.m_Calls"),4):nullptr;
}
std::int64_t EventTarget(const V *v,std::string_view event,std::string_view method) {
  auto calls=Calls(v,event);if(!calls||calls->size()!=1)return 0;
  const auto& c=calls->front();auto name=Text(c.Get("m_MethodName"));std::int64_t mode{},state{};
  return name&&*name==method&&Int(c.Get("m_Mode"),mode)&&mode==0&&
    Int(c.Get("m_CallState"),state)&&state==1?Local(c.Get("m_Target")):0;
}
// Follow the serialized BooleanUIView event graph, rather than using prefab
// active flags or assuming the last-used variant shares the normal sprite.
std::optional<std::pair<std::int64_t,std::int64_t>> VariantRoots(const V *v,
    std::string_view true_event="onSetTrue",std::string_view false_event="onSetFalse",int call_state=1) {
  auto yes=Calls(v,true_event),no=Calls(v,false_event);
  if(!yes||!no||yes->size()!=2||no->size()!=2)return {};
  std::map<std::int64_t,bool> a,b;
  for(unsigned side=0;side<2;++side)for(const auto& c:side?*no:*yes){
    auto method=Text(c.Get("m_MethodName"));std::int64_t mode{},state{};bool value{};
    const auto id=Local(c.Get("m_Target"));
    if(!id||!method||*method!="SetActive"||!Int(c.Get("m_Mode"),mode)||mode!=6||
      !Int(c.Get("m_CallState"),state)||state!=call_state||!Flag(c.Path("m_Arguments.m_BoolArgument"),value)||
      !(side?b:a).emplace(id,value).second)return {};
  }
  std::int64_t normal{},last{};
  for(const auto& [id,value]:a){auto it=b.find(id);if(it==b.end()||it->second==value)return {}; (value?last:normal)=id;}
  return normal&&last?std::optional{std::pair{normal,last}}:std::nullopt;
}
std::optional<SavePoint> ParseSavePoint(Scene& scene,const V& v,
    const belmont_map_sources::SourceRecipe& source,const std::vector<std::string>& cabs,
    const std::shared_ptr<const ce_unity::SerializedFile>& menu,std::int64_t item_id) {
  SavePoint result;auto& base=result.normal_art;
  auto room=Text(v.Get("itemWorldRoomId"));const auto go=Local(v.Get("m_GameObject"));
  auto g=scene.Get(go);auto name=g?Text(g->Get("m_Name")):std::nullopt;
  std::int64_t script{},file{},zone{},secret{};
  if(!room||!room->starts_with(source.world_name)||!name||*name!="LevelMap_SavePoint_Item(Clone)"||
    Local(v.Get("context"))!=source.map_context_id||
    !Ref(v.Get("m_Script"),script,file)||script!=3538717887839853159LL||file!=1||
    !Int(v.Get("itemExplorationZoneId"),zone)||zone<0||zone>65535||
    !Int(v.Get("itemLinkedSecretZoneId"),secret)||secret!=-1||
    !Flag(v.Get("visibleInUnrevealedMap"),result.visible_in_unrevealed_map))return {};
  base.source_id=item_id;base.source_name=*name;base.room_id=*room;base.exploration_zone_id=std::int32_t(zone);
  base.navigation_zone_id=Local(v.Get("navigationZone"));base.unconditional_policy=true;
  auto tr=scene.transforms.find(go);auto pos=tr!=scene.transforms.end()?scene.World(tr->second):std::nullopt;
  if(!pos||!base.navigation_zone_id)return {};
  base.x=pos->x;base.y=pos->y;
  auto display=scene.Get(Local(v.Get("display")));
  const auto used=EventTarget(display,"onSetUpUsed","SetActive");
  auto variants=VariantRoots(scene.Get(EventTarget(display,"onSetUpIsLastUsed","SetBool")));
  if(!used||!variants)return {};
  auto components=g?Array(g->Get("m_Component"),32):nullptr;bool responsive=false;
  std::optional<Transform> sizable;
  if(components)for(const auto& c:*components){auto m=scene.Get(Local(c.Get("component")));
    if(m&&Local(m->Get("levelMapItem"))==item_id&&m->Get("screenPercentage")){
      if(responsive||!Num(m->Get("screenPercentage"),result.screen_percentage)||
        !Num(m->Get("screenPercentageAtZoomMax"),result.screen_percentage_at_zoom_max)||
        result.screen_percentage<=0||result.screen_percentage>.25f||
        result.screen_percentage_at_zoom_max<=0||result.screen_percentage_at_zoom_max>.25f)return {};
      responsive=true;
      sizable=scene.World(Local(m->Get("sizableObject")));
    }}
  if(!responsive||!sizable||sizable->x!=base.x||sizable->y!=base.y)return {};
  result.last_used_art=base;
  for(const auto& object:scene.file->Objects())if(object.class_id==212){
    auto sr=scene.Get(object.path_id);auto sg=sr?Local(sr->Get("m_GameObject")):0;
    auto st=scene.transforms.find(sg);if(st==scene.transforms.end())continue;
    bool under=false,in_used=false,normal=false,last=false;std::set<std::int64_t> seen;auto at=st->second;
    while(at&&seen.size()<64&&seen.insert(at).second){auto t=scene.Get(at);if(!t)break;auto ag=Local(t->Get("m_GameObject"));
      under|=ag==go;in_used|=ag==used;normal|=ag==variants->first;last|=ag==variants->second;at=Local(t->Get("m_Father"));}
    if(!under||!in_used||normal==last)continue;
    std::int64_t sid{};if(!ExternalIs(cabs,sr->Get("m_Sprite"),MenuCab,sid))return {};
    auto sprite=Obj(menu,sid,213);auto p=scene.World(st->second);Sprite art;
    if(!sprite||!p||!Metadata(*sprite,art))return {};
    art.id=sid;art.bundle=Menu;art.cab=MenuCab;art.x=p->x;art.y=p->y;art.scale_x=p->sx;art.scale_y=p->sy;
    const char* channels[]={"r","g","b","a"};for(unsigned n=0;n<4;++n)
      if(!Num(sr->Path(std::string("m_Color.")+channels[n]),art.color[n]))return {};
    (last?result.last_used_art:result.normal_art).sprites.push_back(std::move(art));
  }
  if(result.normal_art.sprites.size()!=1||result.last_used_art.sprites.size()!=1)return {};
  const auto& normal=result.normal_art.sprites[0];
  const auto& alternate=result.last_used_art.sprites[0];
  const float w=normal.width/normal.pixels_per_unit*normal.scale_x/sizable->sx;
  const float h=normal.height/normal.pixels_per_unit*normal.scale_y/sizable->sy;
  if(w<=0||w>4||std::abs(w-h)>1e-6f||normal.x!=alternate.x||normal.y!=alternate.y||
     normal.width!=alternate.width||normal.height!=alternate.height||
     normal.pixels_per_unit!=alternate.pixels_per_unit||normal.scale_x!=alternate.scale_x||
     normal.scale_y!=alternate.scale_y||normal.pivot_x!=alternate.pivot_x||normal.pivot_y!=alternate.pivot_y)return {};
  const float cx=(normal.x-sizable->x)/sizable->sx+(.5f-normal.pivot_x)*w;
  const float cy=(normal.y-sizable->y)/sizable->sy+(.5f-normal.pivot_y)*h;
  result.icon_size_factor=w;
  result.icon_anchor_x=.5f-cx/w;result.icon_anchor_y=.5f+cy/h;
  if(!std::isfinite(result.icon_anchor_x)||!std::isfinite(result.icon_anchor_y)||
    std::abs(result.icon_anchor_x)>4||std::abs(result.icon_anchor_y)>4)return {};
  return result;
}
std::optional<Elevator> ParseElevator(Scene& scene,const V& v,
    const belmont_map_sources::SourceRecipe& source,const std::vector<std::string>& cabs,
    const std::shared_ptr<const ce_unity::SerializedFile>& menu,std::int64_t item_id) {
  Elevator result;auto& art=result.art;
  auto room=Text(v.Get("itemWorldRoomId"));const auto go=Local(v.Get("m_GameObject"));
  auto g=scene.Get(go);auto name=g?Text(g->Get("m_Name")):std::nullopt;
  std::int64_t script{},file{},zone{},secret{};bool before_reveal{};
  if(!room||!room->starts_with(source.world_name)||!name||*name!="LevelMap_Elevator_Item(Clone)"||
    Local(v.Get("context"))!=source.map_context_id||
    !Ref(v.Get("m_Script"),script,file)||script!=-6054241008690704164LL||file!=1||
    !Int(v.Get("itemExplorationZoneId"),zone)||zone<0||zone>65535||
    !Int(v.Get("itemLinkedSecretZoneId"),secret)||secret < -1||secret>65535||
    !Flag(v.Get("visibleInUnrevealedMap"),before_reveal)||before_reveal)return {};
  art.source_id=item_id;art.source_name=*name;art.room_id=*room;
  art.exploration_zone_id=std::int32_t(zone);art.navigation_zone_id=Local(v.Get("navigationZone"));
  art.unconditional_policy=true;result.secret_zone_id=int(secret);
  auto tr=scene.transforms.find(go);auto pos=tr!=scene.transforms.end()?scene.World(tr->second):std::nullopt;
  if(!pos||!art.navigation_zone_id)return {};
  art.x=pos->x;art.y=pos->y;
  auto display=scene.Get(Local(v.Get("display")));
  auto roots=VariantRoots(display,"onHover","onUnHover",2);
  const auto visibility=display?Local(display->Get("visibilityParent")):0;
  if(!roots||!visibility)return {};
  std::optional<Transform> sizable;bool responsive=false;
  auto components=Array(g->Get("m_Component"),32);
  if(components)for(const auto& c:*components){auto m=scene.Get(Local(c.Get("component")));
    if(m&&Local(m->Get("levelMapItem"))==item_id&&m->Get("screenPercentage")){
      if(responsive||!Num(m->Get("screenPercentage"),result.screen_percentage)||
        !Num(m->Get("screenPercentageAtZoomMax"),result.screen_percentage_at_zoom_max)||
        result.screen_percentage<=0||result.screen_percentage>.25f||
        result.screen_percentage_at_zoom_max<=0||result.screen_percentage_at_zoom_max>.25f)return {};
      responsive=true;sizable=scene.World(Local(m->Get("sizableObject")));
    }}
  if(!responsive||!sizable||sizable->x!=art.x||sizable->y!=art.y)return {};
  bool normal_active{},hover_active{};
  const auto normal_go=scene.Get(roots->first),hover_go=scene.Get(roots->second);
  if(!normal_go||!hover_go||!Flag(normal_go->Get("m_IsActive"),normal_active)||
     !Flag(hover_go->Get("m_IsActive"),hover_active)||!normal_active||hover_active)return {};
  for(const auto& object:scene.file->Objects())if(object.class_id==212){
    auto sr=scene.Get(object.path_id);const auto sg=sr?Local(sr->Get("m_GameObject")):0;
    auto st=scene.transforms.find(sg);if(st==scene.transforms.end())continue;
    bool under=false,in_visible=false,normal=false,hover=false;std::set<std::int64_t> seen;auto at=st->second;
    while(at&&seen.size()<64&&seen.insert(at).second){auto t=scene.Get(at);if(!t)break;
      auto ag=Local(t->Get("m_GameObject"));under|=ag==go;in_visible|=ag==visibility;
      normal|=ag==roots->first;hover|=ag==roots->second;at=Local(t->Get("m_Father"));}
    if(!under||!in_visible||!normal||hover)continue;
    bool enabled{},flip_x{},flip_y{};std::int64_t draw{};
    if(!Flag(sr->Get("m_Enabled"),enabled)||!enabled||!Flag(sr->Get("m_FlipX"),flip_x)||flip_x||
      !Flag(sr->Get("m_FlipY"),flip_y)||flip_y||!Int(sr->Get("m_DrawMode"),draw)||draw!=0)return {};
    std::int64_t sid{};if(!ExternalIs(cabs,sr->Get("m_Sprite"),MenuCab,sid))return {};
    auto sprite=Obj(menu,sid,213);auto p=scene.World(st->second);Sprite glyph;
    if(!sprite||!p||!Metadata(*sprite,glyph))return {};
    glyph.id=sid;glyph.bundle=Menu;glyph.cab=MenuCab;glyph.x=p->x;glyph.y=p->y;
    glyph.scale_x=p->sx;glyph.scale_y=p->sy;
    const char* channels[]={"r","g","b","a"};for(unsigned n=0;n<4;++n)
      if(!Num(sr->Path(std::string("m_Color.")+channels[n]),glyph.color[n])||glyph.color[n]!=1)return {};
    art.sprites.push_back(std::move(glyph));
  }
  if(art.sprites.size()!=1)return {};
  const auto& glyph=art.sprites[0];
  const float w=glyph.width/glyph.pixels_per_unit*glyph.scale_x/sizable->sx;
  const float h=glyph.height/glyph.pixels_per_unit*glyph.scale_y/sizable->sy;
  if(w<=0||w>4||std::abs(w-h)>1e-6f)return {};
  const float cx=(glyph.x-sizable->x)/sizable->sx+(.5f-glyph.pivot_x)*w;
  const float cy=(glyph.y-sizable->y)/sizable->sy+(.5f-glyph.pivot_y)*h;
  result.icon_size_factor=w;result.icon_anchor_x=.5f-cx/w;result.icon_anchor_y=.5f+cy/h;
  if(!std::isfinite(result.icon_anchor_x)||!std::isfinite(result.icon_anchor_y)||
    std::abs(result.icon_anchor_x)>4||std::abs(result.icon_anchor_y)>4)return {};
  return result;
}
} // namespace
bool Visible(const SavePoint& point,int revealed,int used,std::int64_t navigation){
  return used==1&&(revealed==1||point.visible_in_unrevealed_map)&&
    navigation>0&&point.normal_art.navigation_zone_id==navigation;
}
bool Visible(const Elevator& point,int revealed,int entered,std::int64_t navigation){
  return revealed==1&&entered==1&&point.secret_zone_id==-1&&navigation>0&&point.art.navigation_zone_id==navigation;
}
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
  save_cache.reset();
  unity.ClearCache();
}
std::optional<Data> Reader::LoadForRoom(std::string_view room) {
  return Load(room,false);
}
std::optional<Data> Reader::LoadSavePointsForRoom(std::string_view room) {
  return LoadMapItemsForRoom(room);
}
std::optional<Data> Reader::LoadMapItemsForRoom(std::string_view room) {
  return Load(room,true);
}
std::optional<Data> Reader::Load(std::string_view room,bool save_points_only) {
  std::lock_guard lock(mutex);
  try {
    auto source = belmont_map_sources::FindSourceForRoom(room);
    if (!source)
      return {};
    auto& selected_cache=save_points_only?save_cache:cache;
    if (selected_cache && selected_cache->world_name == source->world_name)
      return selected_cache;
    auto sf = File(unity, source->map_bundle, source->map_cab);
    auto data = save_points_only ? nullptr : File(unity, Savable, SavableCab);
    auto menu = File(unity, Menu, MenuCab);
    auto common = save_points_only ? nullptr : File(unity, Common, CommonCab);
    auto b = unity.OpenBundle(source->map_bundle);
    auto node = b ? b->FindNode(source->map_cab) : nullptr;
    auto cabs = node ? belmont_map_refs::ExternalCabs(b->NodeSource(*node))
                     : std::nullopt;
    if (!sf || !menu || !cabs || (!save_points_only&&(!data||!common)))
      return {};
    Scene scene{sf, {}, {}};
    Data out{std::string(source->world_name), {}, {}, -1, {}};
    // ResetContext writes true to revealedMap; InitializeContext overwrites
    // it only when LevelUIDisplayData.mapRevealCondition is non-null. Do not
    // approximate conditional Hall of Mirrors revelation from explored fog.
    if(auto levels=File(unity,LevelData,LevelDataCab)){
      for(const auto& object:levels->Objects())if(object.class_id==114){
        auto value=Obj(levels,object.path_id,114);
        auto name=value?Text(value->Get("m_Name")):std::nullopt;
        if(name&&*name==out.world_name+"_UIDisplay"){
          std::int64_t rid{};
          if(Int(value->Path("mapRevealCondition.rid"),rid)&&rid==-2)out.map_revealed_policy=1;
          break;
        }
      }
    }
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
      if(v){
        std::int64_t sid{},file{};
        if(Ref(v->Get("m_Script"),sid,file)&&sid==3538717887839853159LL&&file==1)
          if(auto save=ParseSavePoint(scene,*v,*source,*cabs,menu,o.path_id))out.save_points.push_back(std::move(*save));
        if(sid==-6054241008690704164LL&&file==1)
          if(auto lift=ParseElevator(scene,*v,*source,*cabs,menu,o.path_id))out.elevators.push_back(std::move(*lift));
      }
      if(save_points_only)continue;
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
    selected_cache = out;
    unity.ClearCache();
    return out;
  } catch (...) {
    return {};
  }
}
} // namespace belmont_map_landmarks
