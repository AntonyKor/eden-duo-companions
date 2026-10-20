// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "belmont_reader.h"
#include <chrono>
#include <optional>
namespace belmont_presentation {
// Presentation-only retention. Retained data never authorizes gameplay actions.
// Explicit loading/title/entity changes invalidate immediately; a short incoherent
// read may dim the last view instead of flashing the loading illustration.
inline bool ExplorationGrows(const std::vector<belmont::ExplorationRoom>& old,const std::vector<belmont::ExplorationRoom>& current){
 for(const auto& room:old){
  const auto found=std::find_if(current.begin(),current.end(),[&](const auto& value){return value.room_id==room.room_id;});
  if(found==current.end()||room.width!=found->width||room.height!=found->height||!found->entered_zones_ready)return false;
  for(auto zone:room.entered_zone_ids)if(std::find(found->entered_zone_ids.begin(),found->entered_zone_ids.end(),zone)==found->entered_zone_ids.end())return false;
 }
 return true;
}
template<class Bundle> class Retainer {
    std::optional<Bundle> last;
    std::chrono::steady_clock::time_point observed{};
public:
    struct Result { Bundle value; bool dim{}; };
    Result Select(Bundle value,bool world,std::chrono::steady_clock::time_point now) {
        if(world) {last=value;observed=now;return {std::move(value),false};}
        const auto& s=value.base;
        if(last&&s.supported) {
            const auto& old=last->base;
            const bool interruption=s.coherent&&s.manager_ready&&s.entity_ready&&
                s.manager==old.manager&&s.entity==old.entity&&s.application_loading==0&&
                s.room_loading==0&&s.loading_identity==0&&s.initial_identity==0&&s.paused==0;
            const bool race=!s.coherent&&s.reason=="incoherent"&&
                now-observed<=std::chrono::milliseconds(180);
            if(interruption){observed=now;return {*last,true};}
            if(race)return {*last,true};
        }
        last.reset();return {std::move(value),false};
    }
    void Clear(){last.reset();}
};
}
