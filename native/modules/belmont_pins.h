// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
namespace belmont_pins {
inline constexpr std::size_t Capacity=32,KindCount=9;
// Native MapMarker_*_DisplayData.markerIcon PPtrs (map-common external CAB2).
// Asset identities only: no game text, pixels or progression table is shipped.
inline constexpr std::array<std::int64_t,KindCount> Sprites{
 1912900965121370566LL,7944782504571480518LL,-1464126068056282682LL,
 6194193013819419078LL,-8184545778678978106LL,5697089712629524934LL,
 932508020663953862LL,5463948360878696902LL,-8868760338130556474LL};
inline std::string NativeImage(std::size_t kind){
 if(kind>=KindCount)return {};
 return "module:native:ui-gamemenus_assets_all_866519f7d6a7057a8ba1c459a5b3f694.bundle#"+std::to_string(Sprites[kind]);
}
inline std::string Image(std::size_t kind){return kind<KindCount?"module:belmont:pin-icon:"+std::to_string(kind):"";}
struct Pin {std::string area;double x{},y{};std::size_t kind{};};
struct Bounds {double x0{},y0{},x1{},y1{};
 bool Contains(double x,double y)const{return std::isfinite(x)&&std::isfinite(y)&&
 std::isfinite(x0)&&std::isfinite(y0)&&std::isfinite(x1)&&std::isfinite(y1)&&x1>x0&&y1>y0&&x>=x0&&x<=x1&&y>=y0&&y<=y1;}};
class Store {
 std::array<std::optional<Pin>,Capacity> pins{};
 Pin pending{};
 bool has_pending{};
 std::string area;
 Bounds bounds;
 bool interactive{},waiting{};
 std::int64_t old_sequence{-1};
 int editing{-1};
public:
 void Context(std::string current,Bounds b,bool enabled){
  if(!enabled||area!=current)Cancel();
  area=std::move(current);bounds=b;interactive=enabled&&!area.empty();
 }
 bool Begin(std::int64_t sequence){
  if(!interactive)return false;
  Cancel();waiting=true;old_sequence=sequence;return true;
 }
 bool Capture(std::int64_t sequence,double x,double y){
  if(!interactive||!waiting||sequence<=old_sequence)return false;
  waiting=false;
  if(!bounds.Contains(x,y))return false;
  pending=Pin{area,x,y,0};has_pending=true;return true;
 }
 bool Select(std::int64_t index){
  if(!interactive||index<0||index>=std::int64_t(Capacity)||!pins[index]||pins[index]->area!=area)return false;
  Cancel();editing=int(index);pending=*pins[index];has_pending=true;return true;
 }
 bool Choose(std::int64_t kind){
  if(!interactive||!has_pending||pending.area!=area||kind<0||kind>=std::int64_t(KindCount))return false;
  int target=editing;
  if(target<0){for(std::size_t n=0;n<Capacity;++n)if(!pins[n]){target=int(n);break;}}
  if(target<0)return false;
  pending.kind=std::size_t(kind);pins[std::size_t(target)].emplace(pending);Cancel();return true;
 }
 bool Remove(){
  if(!interactive||!has_pending||editing<0)return false;
  pins[std::size_t(editing)].reset();Cancel();return true;
 }
 void Cancel(){pending={};has_pending=false;waiting=false;editing=-1;}
 void Clear(){Cancel();for(auto& pin:pins)pin.reset();}
 bool Waiting()const{return waiting;}
 bool Picker()const{return has_pending;}
 bool Editing()const{return Picker()&&editing>=0;}
 const auto& All()const{return pins;}
};
}
