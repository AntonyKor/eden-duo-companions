// SPDX-License-Identifier: GPL-3.0-or-later
// Unreleased companion; guest reads only, native menu actions via host buttons.
// Worker input is a coherent copied
// snapshot; workers never read guest memory or publish host values.
#include "belmont_reader.h"
#include "belmont_presentation.h"
#include "belmont_pins.h"
#include "belmont_inventory.h"
#include "belmont_equipment_control.h"
#include "belmont_asset_callbacks.h"
#include "belmont_map_assets.h"
#include "belmont_map_fog.h"
#include "belmont_map_labels.h"
#include "belmont_map_landmarks.h"
#include "belmont_landmark_render.h"
#include "belmont_equipment.h"
#include "belmont_equipment_assets.h"
#include "belmont_equipment_stats.h"
#include "belmont_objective_labels.h"
#include "belmont_world_map.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <locale>
#include <memory>
#include <mutex>
#include <optional>
#include <deque>
#include <set>
#include <sstream>
#include <thread>

namespace {
std::string AreaKey(const belmont_map_sources::SourceRecipe& recipe) {
    return recipe.id == belmont_map_sources::Id::StreetsPoor ? "belmont_streetspoor" : std::string(recipe.world_name);
}
constexpr std::string_view ObjectivePrefix="module:belmont:objective:";
constexpr std::string_view UpgradePrefix="module:belmont:upgrade:";
constexpr std::string_view PickerPrefix="module:belmont:picker:";
constexpr std::string_view EquipmentPrefix="module:belmont:equipment:";
constexpr std::string_view LandmarkPrefix="module:belmont:landmark:";
constexpr std::string_view MapPrefix = "module:belmont:map:";
bool Valid(const EdenDsmodHostApi* h) {
    return dsmod_sdk::HostAbiMatches(h) && h->is_mapped && h->read_memory &&
           h->publish_i64 && h->publish_text;
}
bool SupportedHost(const EdenDsmodHostApi* h) {
    return dsmod_sdk::HostAbiMatches(h) && h->title_id == belmont::Title &&
           std::memcmp(h->build_id, belmont::Build.data(), belmont::Build.size()) == 0;
}
belmont::Identity Identity(const EdenDsmodHostApi& h) {
    belmont::Identity id{h.title_id, h.main_base, h.main_size, {}};
    std::memcpy(id.build.data(), h.build_id, id.build.size());
    return id;
}
std::optional<std::string_view> Key(const char* s) {
    if (!s) return {};
    std::size_t n{};
    while (n <= 4096 && s[n]) ++n;
    if (!n || n > 4096) return {};
    return std::string_view{s, n};
}
bool World(const belmont::Snapshot& s) {
    return s.supported && s.coherent && s.entity_ready && s.components_ready &&
           s.explorer_candidate && s.room_candidate && s.position_candidate &&
           s.exploration_ready && s.exploration_zones_ready &&
           s.application_loading == 0 && s.room_loading == 0 &&
           s.loading_identity == 0 && (s.initial_identity == 1 || s.paused == 1);
}
bool SameRoom(const belmont::ExplorationRoom& a, const belmont::ExplorationRoom& b) {
    return a.room_id == b.room_id && a.width == b.width && a.height == b.height &&
           a.entered_zones_ready == b.entered_zones_ready &&
           a.entered_zone_ids == b.entered_zone_ids;
}
bool SameObjective(const belmont_objectives::Snapshot& a,const belmont_objectives::Snapshot& b) {
    return a.ready==b.ready && a.objective_guid==b.objective_guid &&
      a.description.table_type==b.description.table_type && a.description.table_guid==b.description.table_guid &&
      a.description.table_name==b.description.table_name && a.description.entry_type==b.description.entry_type &&
      a.description.entry_id==b.description.entry_id && a.description.entry_key==b.description.entry_key;
}
bool SameEquipment(const belmont_equipment::Snapshot& a,const belmont_equipment::Snapshot& b) {
    const auto same=[](const auto& x,const auto& y){return x.equipped==y.equipped&&x.id==y.id;};
    return a.ready==b.ready&&a.coherent==b.coherent&&a.entity==b.entity&&same(a.weapon,b.weapon)&&same(a.spell,b.spell)&&same(a.skin,b.skin)&&
      a.charms.size()==b.charms.size()&&std::equal(a.charms.begin(),a.charms.end(),b.charms.begin(),same);
}
std::shared_ptr<const ce_unity::Image> SquareIcon(const ce_unity::Image& image) {
    const auto side=std::max(image.width,image.height);
    if(side<1||side>2048||image.rgba.size()!=std::size_t(image.width)*image.height*4)return {};
    auto out=std::make_shared<ce_unity::Image>();out->width=out->height=side;out->rgba.resize(std::size_t(side)*side*4);
    const auto ox=(side-image.width)/2,oy=(side-image.height)/2;
    for(unsigned y=0;y<image.height;++y)std::copy_n(image.rgba.data()+std::size_t(y)*image.width*4,std::size_t(image.width)*4,out->rgba.data()+(std::size_t(y+oy)*side+ox)*4);
    return out;
}
std::shared_ptr<const ce_unity::Image> Portrait(const ce_unity::Image& image,unsigned aspect_x,unsigned aspect_y) {
    if(!image.width||!image.height||image.rgba.size()!=std::size_t(image.width)*image.height*4)return {};
    const auto scale=std::max((image.width+aspect_x-1)/aspect_x,(image.height+aspect_y-1)/aspect_y);
    const auto width=scale*aspect_x,height=scale*aspect_y;
    if(!scale||width>2048||height>2048||std::size_t(width)*height*4>16u*1024*1024)return {};
    auto out=std::make_shared<ce_unity::Image>();out->width=width;out->height=height;out->rgba.resize(std::size_t(width)*height*4);
    const auto ox=(width-image.width)/2,oy=(height-image.height)/2;
    for(unsigned y=0;y<image.height;++y)std::copy_n(image.rgba.data()+std::size_t(y)*image.width*4,std::size_t(image.width)*4,out->rgba.data()+(std::size_t(y+oy)*width+ox)*4);
    return out;
}
struct EquipmentImage {std::string slot,name,key;std::shared_ptr<const ce_unity::Image> image;};
void JsonText(std::ostream& out,std::string_view s) {
    out << '"';for(unsigned char c:s){if(c=='"'||c=='\\')out<<'\\'<<c;else if(c>=32)out<<c;}out<<'"';
}
struct LandmarkMeta {
    belmont_map_landmarks::Landmark source;
    belmont_landmark_render::Placement art,name;
    std::string english;
};
struct LandmarkImage { std::string guid,kind,key; std::shared_ptr<const ce_unity::Image> image; float x{},y{}; };
struct Fingerprint {
    belmont::U64 manager{}, entity{};
    std::string room;
    std::vector<belmont::ExplorationRoom> exploration;
    bool landmarks_ready{};
    std::vector<std::string> discovered;
    belmont_objectives::Snapshot objective;
    belmont_equipment::Snapshot equipment;
    explicit Fingerprint(const belmont::Snapshot& s, const belmont_objectives::Snapshot& ob,const belmont_equipment::Snapshot& eq)
        : manager(s.manager), entity(s.entity), room(s.room_id), exploration(s.exploration),
          landmarks_ready(s.landmarks_ready),discovered(s.discovered_landmarks),objective(ob),equipment(eq) {}
    bool operator==(const Fingerprint& b) const {
        return manager == b.manager && entity == b.entity && room == b.room && landmarks_ready==b.landmarks_ready && discovered==b.discovered && SameObjective(objective,b.objective) && SameEquipment(equipment,b.equipment) &&
            exploration.size() == b.exploration.size() &&
            std::equal(exploration.begin(), exploration.end(), b.exploration.begin(), SameRoom);
    }
    bool DisplayCompatible(const belmont::Snapshot& s,const belmont_world_map::Frame* frame=nullptr) const {
        if(manager!=s.manager||entity!=s.entity||!belmont_presentation::ExplorationGrows(exploration,s.exploration))return false;
        if(room==s.room_id)return true;
        // A coherent successor room can retain this atlas only when native
        // source membership proves the same world and navigation group.
        const auto* source=belmont_map_sources::FindSourceForRoom(s.room_id);
        return World(s)&&frame&&source&&source->id==frame->source_id&&
            std::find(frame->navigation_rooms.begin(),frame->navigation_rooms.end(),s.room_id)!=frame->navigation_rooms.end();
    }
    bool MapMatches(const belmont::Snapshot& s) const {
        if(manager!=s.manager||entity!=s.entity||room!=s.room_id||landmarks_ready!=s.landmarks_ready||discovered!=s.discovered_landmarks||exploration.size()!=s.exploration.size())return false;
        for(std::size_t n=0;n<exploration.size();++n)if(!SameRoom(exploration[n],s.exploration[n]))return false;
        return true;
    }
    bool Matches(const belmont::Snapshot& s, const belmont_objectives::Snapshot& ob,const belmont_equipment::Snapshot& eq) const {
        if (!MapMatches(s) || !SameObjective(objective,ob) || !SameEquipment(equipment,eq)) return false;
        for (std::size_t n = 0; n < exploration.size(); ++n)
            if (!SameRoom(exploration[n], s.exploration[n])) return false;
        return true;
    }
};
bool Finite(const belmont_map_fog::Rect& r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
           std::isfinite(r.height) && r.width > 0 && r.height > 0 &&
           r.width <= 4096 && r.height <= 4096;
}
bool Contains(const belmont_map_fog::Rect& r, float x, float y) {
    return std::isfinite(x) && std::isfinite(y) && x >= r.x && y >= r.y &&
           x <= r.x + r.width && y <= r.y + r.height;
}
struct Work {
    std::uint64_t generation{};
    Fingerprint fingerprint;
    belmont_world_map::Request request;
    Work(std::uint64_t g, const belmont::Snapshot& s, const belmont_objectives::Snapshot& ob,const belmont_equipment::Snapshot& eq) : generation(g), fingerprint(s,ob,eq) {
        request.current_room_id = s.room_id;
        request.player_x = s.target_position[0];
        request.player_y = s.target_position[1];
        for (const auto& room : s.exploration)
            request.explored_rooms.push_back({room.room_id, room.entered_zone_ids});
    }
};
struct Rendered {
    std::uint64_t generation{};
    std::shared_ptr<const belmont_world_map::Frame> frame;
    std::shared_ptr<const Fingerprint> fingerprint;
    belmont_map_fog::Rect navigation_bounds;
    bool navigation_proven{}, equipment_prepared{};
    std::string area_name, area_key, biome_name;
    std::string objective_name,objective_key;
    std::shared_ptr<const ce_unity::Image> objective_image;
    std::vector<LandmarkImage> overlays;
    std::vector<EquipmentImage> equipment_images;
};
struct LiveBundle {
    belmont::Snapshot base;
    belmont_objectives::Snapshot objective;
    belmont_equipment::Snapshot equipment;
    belmont_equipment_stats::Snapshot stats;
};
struct Module {
    // Copied table is used only for read_romfs on workers. userdata is kept alive
    // by the host until Destroy; stop/join every worker before destroying assets.
    EdenDsmodHostApi io_host;
    belmont_assets::Callbacks assets;
    std::atomic<bool> allowed{true}, marker_native_ready{false}, areas_served{false}, stopping{false};
    std::mutex map_mutex, layout_mutex;
    std::condition_variable wake;
    std::optional<Fingerprint> requested;
    std::optional<Work> pending;
    std::shared_ptr<const Rendered> completed;
    struct MapImage {std::string key;std::shared_ptr<const belmont_world_map::Frame> frame;};
    std::deque<MapImage> recent_maps;
    struct FadeWork {
        std::uint64_t generation{};
        std::shared_ptr<const Rendered> target;
        std::shared_ptr<const belmont_world_map::Frame> previous;
        unsigned step{};
    };
    std::optional<FadeWork> fade_work,staged_fade;
    std::chrono::steady_clock::time_point fade_due{};
    std::shared_ptr<const Rendered> staged_map,active_reveal_target,finish_reveal;
    std::string staged_map_key;
    std::uint64_t sample_epoch{},map_image_epoch{},staged_served_epoch{};
    bool staged_served{};
    void StageMapLocked(std::shared_ptr<const Rendered> value,std::optional<FadeWork> fade={}){
        staged_map=std::move(value);staged_fade=std::move(fade);
        if(!staged_fade)active_reveal_target.reset();
        staged_map_key=std::string(MapPrefix)+std::to_string(generation)+":"+std::to_string(++map_image_epoch);
        staged_served=false;staged_served_epoch=0;
    }
    void AdvanceMapLocked(std::chrono::steady_clock::time_point now){
        ++sample_epoch;
        // Host drains its completed image queue before Sample. Sink completion
        // is not an explicit GPU-cache-ready API, so wait two further samples
        // and preserve recent immutable keys for in-flight callback requests.
        if(!staged_map||!staged_served||sample_epoch-staged_served_epoch<2)return;
        if(staged_map->generation!=generation){staged_map.reset();staged_map_key.clear();staged_fade.reset();return;}
        if(completed)RememberMapLocked(current_key,completed->frame);
        completed=std::move(staged_map);current_key=std::move(staged_map_key);staged_map_key.clear();
        RememberMapLocked(current_key,completed->frame);
        if(staged_fade&&staged_fade->step<4){
            fade_work=*staged_fade;++fade_work->step;fade_due=now+std::chrono::milliseconds(40);wake.notify_one();
        }
        if(staged_fade&&staged_fade->step>=4)active_reveal_target.reset();
        staged_fade.reset();staged_served=false;
    }
    void RememberMapLocked(std::string key,std::shared_ptr<const belmont_world_map::Frame> frame){
        if(key.empty()||!frame)return;
        for(const auto& old:recent_maps)if(old.key==key)return;
        recent_maps.push_back({std::move(key),std::move(frame)});
        while(recent_maps.size()>3)recent_maps.pop_front();
    }
    std::map<belmont_map_sources::Id, belmont_map_fog::Rect> tile_bounds;
    bool areas_initialized{};
    std::map<belmont_map_sources::Id,std::vector<LandmarkMeta>> landmark_meta;
    std::map<belmont_map_sources::Id,belmont_map_landmarks::Data> map_item_meta;
    std::vector<std::pair<std::string,std::string>> published_overlays;
    std::string areas_json, current_key;
    std::optional<belmont::Snapshot> full_source;
    LiveBundle details;
    belmont_presentation::Retainer<LiveBundle> presentation;
    std::chrono::steady_clock::time_point full_sample_at{}, details_at{};
    belmont_pins::Store pins;
    std::int64_t map_tap_sequence{-1};
    bool presentation_dim{},control_ready{},picker_exposed{},equipment_visible{};
    belmont_inventory::Snapshot inventory;
    belmont_equipment_control::Controller equipment_controller;
    std::chrono::steady_clock::time_point inventory_at{},inventory_control_at{};
    // Separate worker-safe readers: image callbacks decode only demanded visible
    // cells, without blocking the map worker or motion sampling mutex.
    std::shared_ptr<belmont_equipment_assets::Reader> lazy_equipment_assets;
    std::shared_ptr<belmont_assets::Reader> lazy_picker_ui;
    std::vector<EquipmentImage> picker_images;
    std::vector<belmont_inventory::Slot> picker_slots;
    std::string picker_signature;
    std::uint64_t picker_generation{};
    std::uint64_t generation{};
    bool in_flight{}, exposed{}, equipment_exposed{}, map_retry_required{};
    std::chrono::steady_clock::time_point retry_after{};
    std::thread font_worker, map_worker;
    explicit Module(const EdenDsmodHostApi& h) : io_host(h), assets(h, true) {}
    ~Module() { Stop(); }
    void Stop() {
        stopping.store(true); wake.notify_all();
        if (font_worker.joinable()) font_worker.join();
        if (map_worker.joinable()) map_worker.join();
    }
    void InvalidateLocked() {
        if (requested || pending || completed || !current_key.empty()) ++generation;
        requested.reset(); pending.reset(); completed.reset(); recent_maps.clear(); staged_map.reset();staged_map_key.clear();staged_fade.reset();fade_work.reset();active_reveal_target.reset();finish_reveal.reset(); current_key.clear(); exposed = false; equipment_exposed = false;map_retry_required=false;
    }
    std::optional<belmont_map_fog::Rect> Bounds(const belmont_map_sources::SourceRecipe& recipe) {
        // areas_src is fetched once. Prepare geometric metadata for every native
        // source here; sampling never locks or performs ROMFS I/O. Optional
        // unavailable worlds cannot supply invented bounds or block StreetsPoor.
        std::lock_guard lock{layout_mutex};
        if (!allowed.load() || !io_host.read_romfs || !belmont_map_sources::Canonical(recipe)) return {};
        if (!areas_initialized) {
            belmont_world_map::Renderer source(&io_host);
            std::map<belmont_map_sources::Id, belmont_map_fog::Rect> found;
            std::map<belmont_map_sources::Id,std::vector<LandmarkMeta>> found_landmarks;
            std::map<belmont_map_sources::Id,belmont_map_landmarks::Data> found_map_items;
            belmont_map_assets::Reader source_registry(&io_host);
            belmont_map_landmarks::Reader source_landmarks(&io_host);
            belmont_map_labels::Reader source_labels(&io_host);
            belmont_landmark_render::Renderer placement(&io_host);
            for (const auto& candidate : belmont_map_sources::Recipes) {
                const auto bounds = source.Bounds(candidate);
                if (bounds && Finite(*bounds)) {
                    found.emplace(candidate.id, *bounds);
                    const auto world=source_registry.LoadWorld(candidate);
                    if(!world||world->rooms.empty())continue;
                    if(auto saves=source_landmarks.LoadMapItemsForRoom(world->rooms.front().id))
                        found_map_items.emplace(candidate.id,std::move(*saves));
                    const auto landmarks=source_landmarks.LoadForRoom(world->rooms.front().id);
                    if(!landmarks)continue;
                    for(const auto& item:landmarks->landmarks) {
                        if(!item.unconditional_policy)continue;
                        const auto name=source_labels.LoadLandmarkEnglish(world->rooms.front().id,item.name_table,item.name_key_id);
                        const auto art=belmont_landmark_render::ArtPlacement(item);
                        const auto label=name?placement.NamePlacement(item,*name):std::nullopt;
                        if(art&&label&&name)found_landmarks[candidate.id].push_back({item,*art,*label,*name});
                    }
                }
            }
            if (!found.contains(belmont_map_sources::Id::StreetsPoor)) return {};
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out << std::setprecision(std::numeric_limits<float>::max_digits10) << '{';
            bool first = true;
            for (const auto& candidate : belmont_map_sources::Recipes) {
                const auto it = found.find(candidate.id);
                if (it == found.end()) continue;
                if (!first) out << ',';
                first = false;
                const auto& r = it->second;
                out << '\"' << AreaKey(candidate) << "\":{\"image\":\"module:belmont:map:0\",\"min\":["
                    << r.x << ',' << r.y << "],\"max\":[" << r.x + r.width << ',' << r.y + r.height
                    << "],\"clamp_view\":true,\"no_pin\":false,\"dynamic_markers\":[";
                bool first_marker=true;
                for(const auto& landmark:found_landmarks[candidate.id]) for(const auto* kind:{"art","name"}) {
                    if(!first_marker)out<<',';
                    first_marker=false;
                    const auto prefix="belmont.landmark."+landmark.source.guid+"."+kind;
                    out<<"{\"count\":1,\"x\":";JsonText(out,prefix+".x");out<<",\"y\":";JsonText(out,prefix+".y");
                    out<<",\"show_bind\":";JsonText(out,prefix+".ready");out<<",\"icon_src_bind\":";JsonText(out,prefix+".image");
                    if(std::string_view(kind)=="art")out<<",\"size_world\":"<<landmark.art.size;
                    else out<<",\"size\":"<<std::lround(landmark.name.size);
                    out<<",\"anchor\":[0.5,0.5]}";
                }
                if(auto saves=found_map_items.find(candidate.id);saves!=found_map_items.end()) {
                    for(const auto& point:saves->second.save_points) {
                        for(const auto* kind:{"normal","last"}) {
                            const auto& art=std::string_view(kind)=="last"?point.last_used_art:point.normal_art;
                            if(art.sprites.size()!=1)continue;
                            // Native base responsive size; pinch has no proven
                            // equivalence to the game's separate zoom coefficient.
                            const float size=1080.f*point.screen_percentage*point.icon_size_factor;
                            if(!std::isfinite(size)||size<=0||size>1080)continue;
                            if(!first_marker)out<<',';
                            first_marker=false;
                            const auto prefix="belmont.savepoint."+std::to_string(art.source_id)+"."+kind;
                            out<<"{\"group\":\"native_savepoints\",\"count\":1,\"x\":";JsonText(out,prefix+".x");
                            out<<",\"y\":";JsonText(out,prefix+".y");
                            out<<",\"show_bind\":";JsonText(out,prefix+".ready");
                            out<<",\"icon_src_bind\":";JsonText(out,prefix+".image");
                            out<<",\"size\":"<<size<<",\"anchor\":["<<point.icon_anchor_x<<','<<point.icon_anchor_y<<"]}";
                        }
                    }
                }
                if(auto items=found_map_items.find(candidate.id);items!=found_map_items.end()) {
                    for(const auto& point:items->second.elevators) {
                        if(point.secret_zone_id!=-1||point.art.sprites.size()!=1)continue;
                        const auto prefix="belmont.elevator."+std::to_string(point.art.source_id);
                        const float size=1080.f*point.screen_percentage*point.icon_size_factor;
                        if(!std::isfinite(size)||size<=0||size>1080)continue;
                        if(!first_marker)out<<',';
                        first_marker=false;
                        out<<"{\"group\":\"native_elevators\",\"count\":1,\"x\":";JsonText(out,prefix+".x");
                        out<<",\"y\":";JsonText(out,prefix+".y");
                        out<<",\"show_bind\":";JsonText(out,prefix+".ready");
                        out<<",\"icon_src_bind\":";JsonText(out,prefix+".image");
                        out<<",\"size\":"<<size<<",\"anchor\":["<<point.icon_anchor_x<<','<<point.icon_anchor_y<<"]}";
                    }
                }
                if(!first_marker)out<<',';
                out<<"{\"group\":\"companion_pins\",\"count\":32,\"x\":\"belmont.pin.{i}.x\",\"y\":\"belmont.pin.{i}.y\",\"kind\":\"belmont.pin.{i}.ready\",\"hide_when_kind\":0,\"icon_src_bind\":\"belmont.pin.{i}.image\",\"size\":56,\"anchor\":[0.5,0.5]}";
                out<<"]}";
            }
            out << '}';
            areas_json = out.str(); tile_bounds = std::move(found);
            landmark_meta = std::move(found_landmarks); map_item_meta = std::move(found_map_items); areas_initialized = true;
        }
        const auto it = tile_bounds.find(recipe.id);
        return it == tile_bounds.end() ? std::nullopt : std::optional(it->second);
    }
    void Queue(const belmont::Snapshot& s,const belmont_objectives::Snapshot& ob,const belmont_equipment::Snapshot& eq) {
        const bool eligible = World(s);
        std::lock_guard lock{map_mutex};
        if (!eligible || !allowed.load()) { InvalidateLocked(); return; }
        if (!requested || !requested->Matches(s,ob,eq)) {
            const bool keep=completed&&completed->frame&&completed->fingerprint&&completed->fingerprint->DisplayCompatible(s,completed->frame.get());
            if(keep&&completed&&active_reveal_target&&completed->frame!=active_reveal_target->frame)
                finish_reveal=active_reveal_target;
            else if(!keep)finish_reveal.reset();
            active_reveal_target.reset();
            ++generation;requested.emplace(s,ob,eq);staged_map.reset();staged_map_key.clear();staged_fade.reset();fade_work.reset();
            if(!keep){completed.reset();recent_maps.clear();current_key.clear();}
            exposed=false;equipment_exposed=false;
            pending.emplace(generation, s,ob,eq); retry_after = {}; wake.notify_one();
        } else if ((!completed||map_retry_required) && !pending && !in_flight && !staged_map && !fade_work &&
                   std::chrono::steady_clock::now() >= retry_after) {
            pending.emplace(generation, s,ob,eq); wake.notify_one();
        }
    }
    void Run(ce_unity::AstcDecoder astc) {
        belmont_world_map::Renderer renderer(&io_host, astc);
        belmont_map_assets::Reader registry(&io_host);
        belmont_map_fog::Reader fog(&io_host);
        belmont_map_labels::Reader labels(&io_host);
        belmont_landmark_render::Renderer overlay_renderer(&io_host,astc);
        belmont_objectives::EnglishReader objective_labels(&io_host);
        belmont_equipment_assets::Reader equipment_assets(&io_host,astc);
        while (!stopping.load()) {
            std::optional<Work> job;
            std::optional<FadeWork> fade_job;
            {
                std::unique_lock lock{map_mutex};
                for(;;){
                    if(stopping.load())return;
                    if(pending){job=std::move(pending);pending.reset();break;}
                    if(fade_work&&!staged_map){
                        if(std::chrono::steady_clock::now()>=fade_due){fade_job=std::move(fade_work);fade_work.reset();break;}
                        wake.wait_until(lock,fade_due);
                    }else wake.wait(lock);
                }
                in_flight=true;
            }
            if(fade_job){
                std::shared_ptr<Rendered> step;
                try{
                    auto frame=belmont_world_map::BlendReveal(fade_job->previous,fade_job->target->frame,float(fade_job->step)/4.f);
                    if(frame){step=std::make_shared<Rendered>(*fade_job->target);step->frame=std::move(frame);}
                }catch(...){}
                std::lock_guard lock{map_mutex};in_flight=false;
                if(fade_job->generation==generation&&allowed.load()&&!stopping.load()){
                    if(step)StageMapLocked(std::move(step),*fade_job);
                    else StageMapLocked(fade_job->target);
                }
                continue;
            }
            std::shared_ptr<Rendered> result;
            try {
                result=std::make_shared<Rendered>();result->generation=job->generation;result->fingerprint=std::make_shared<Fingerprint>(job->fingerprint);
                // Optional equipped-art failures cannot make the verified map unavailable.
                const auto& eq=job->fingerprint.equipment;
                const auto item=[&](std::string slot,belmont_equipment_assets::Type type,const belmont_equipment::Item& equipped) {
                    if(!eq.ready||!equipped.equipped)return;
                    try {if(auto source=equipment_assets.ResolveEnglish(type,equipped.id)) {
                        auto image=SquareIcon(source->image);if(!image)return;
                        auto key=std::string(EquipmentPrefix)+std::to_string(job->generation)+":"+slot;
                        result->equipment_images.push_back({slot,source->name,std::move(key),std::move(image)});
                        const auto portrait=[&](const char* suffix,const auto& source_image,const std::string& name,unsigned ax,unsigned ay) {
                            if(!source_image)return;
                            if(auto padded=Portrait(*source_image,ax,ay))result->equipment_images.push_back({suffix,name,std::string(EquipmentPrefix)+std::to_string(job->generation)+":"+suffix,std::move(padded)});
                        };
                        if(slot=="weapon")portrait("weapon_card",source->illustration,source->name,11,20);
                        if(slot=="spell")portrait("arcana",source->arcana,source->arcana_name,1,2);
                    }}catch(...){}
                };
                item("weapon",belmont_equipment_assets::Type::Weapon,eq.weapon);
                item("spell",belmont_equipment_assets::Type::Spell,eq.spell);
                item("skin",belmont_equipment_assets::Type::Skin,eq.skin);
                for(std::size_t n=0;n<std::min<std::size_t>(3,eq.charms.size());++n)item("charm_"+std::to_string(n),belmont_equipment_assets::Type::Charm,eq.charms[n]);
                result->equipment_prepared=job->fingerprint.equipment.ready;
                const auto* recipe = belmont_map_sources::FindSourceForRoom(job->request.current_room_id);
                const auto bounds = recipe ? Bounds(*recipe) : std::nullopt;
                const auto world = recipe ? registry.LoadWorld(*recipe) : std::nullopt;
                const auto native_fog = recipe ? fog.Load(*recipe) : std::nullopt;
                if (bounds && world && native_fog && allowed.load()) {
                    const auto room = std::find_if(world->rooms.begin(), world->rooms.end(),
                        [&](const auto& r) { return r.id == job->request.current_room_id; });
                    const auto zones = std::find_if(native_fog->rooms.begin(), native_fog->rooms.end(),
                        [&](const auto& r) { return r.id == job->request.current_room_id; });
                    if (room != world->rooms.end() && zones != native_fog->rooms.end() &&
                        !zones->zones.empty()) {
                        // Companion Follow uses the current source room's unique
                        // navigation, independently of the top-map cursor. The
                        // world renderer also validates selected native navigation.
                        // Room bounds, zone/navigation IDs and the atlas extent
                        // are decoded; no physical coordinates are authored.
                        const auto nav = zones->zones.front().navigation_zone_id;
                        const bool same_nav = nav > 0 && std::all_of(zones->zones.begin(),
                            zones->zones.end(), [&](const auto& z) { return z.navigation_zone_id == nav; });
                        belmont_map_fog::Rect physical{float(room->simulation_origin.x),
                            float(room->simulation_origin.y),
                            float(double(room->width) * room->chunk_size.x),
                            float(double(room->height) * room->chunk_size.y)};
                        if (same_nav && Finite(physical)) {
                            std::set<std::int64_t> culled;
                            for (const auto& r : native_fog->rooms)
                                for (const auto& z : r.zones)
                                    if (z.navigation_zone_id > 0 && z.navigation_zone_id != nav)
                                        culled.insert(z.navigation_zone_id);
                            job->request.culled_navigation_zone_ids.assign(culled.begin(), culled.end());
                            // Exploration persists across worlds. This renderer
                            // receives only rooms from its source-native registry.
                            std::erase_if(job->request.explored_rooms, [&](const auto& explored) {
                                return std::none_of(world->rooms.begin(), world->rooms.end(),
                                    [&](const auto& source) { return source.id == explored.room_id; });
                            });
                            auto frame = renderer.Render(*recipe, job->request);
                            if (frame && frame->source_id == recipe->id && frame->image.width > 0 && frame->image.width <= 4096 &&
                                frame->image.height > 0 && frame->image.height <= 4096 &&
                                frame->image.rgba.size() == std::size_t(frame->image.width) *
                                    frame->image.height * 4 && frame->image.rgba.size() <= 16u * 1024u * 1024u &&
                                Finite(frame->world_bounds) && frame->world_bounds.x == bounds->x &&
                                frame->world_bounds.y == bounds->y && frame->world_bounds.width == bounds->width &&
                                frame->world_bounds.height == bounds->height) {
                                result->frame = std::move(frame);
                                result->navigation_bounds = physical;
                                result->navigation_proven = true;
                                result->area_key = AreaKey(*recipe);
                                // Explicit English fixture only; no locale autodetection claim.
                                if (const auto name = labels.LoadAreaEnglish(job->request.current_room_id)) result->area_name = *name;
                                if (const auto biome = labels.LoadBiomeEnglish(job->request.current_room_id)) result->biome_name = *biome;
                                if (const auto objective=objective_labels.Load(job->fingerprint.objective)){
                                    result->objective_name=*objective;
                                    if(auto image=overlay_renderer.RenderText(*objective,1100,64,24)){
                                        result->objective_image=std::make_shared<const ce_unity::Image>(std::move(*image));
                                        result->objective_key=std::string(ObjectivePrefix)+std::to_string(job->generation);
                                    }
                                }
                                if(job->fingerprint.landmarks_ready) {
                                    std::vector<LandmarkMeta> metadata;
                                    {std::lock_guard lock{layout_mutex};metadata=landmark_meta[recipe->id];}
                                    for(const auto& landmark:metadata) {
                                        if(!belmont_map_landmarks::Visible(landmark.source,job->fingerprint.discovered,job->request.culled_navigation_zone_ids))continue;
                                        auto art=overlay_renderer.RenderArt(landmark.source);
                                        auto name=overlay_renderer.RenderName(landmark.source,landmark.english);
                                        if(!art||!name)continue;
                                        const auto key=std::string(LandmarkPrefix)+std::to_string(job->generation)+":"+landmark.source.guid;
                                        result->overlays.push_back({landmark.source.guid,"art",key+":art",std::make_shared<ce_unity::Image>(std::move(art->image)),art->world_center_x,art->world_center_y});
                                        result->overlays.push_back({landmark.source.guid,"name",key+":name",std::make_shared<ce_unity::Image>(std::move(name->image)),name->world_center_x,name->world_center_y});
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (...) {
                if(!result||!result->equipment_prepared)result.reset();
            }
            if(result&&!result->frame&&!result->equipment_prepared)result.reset();
            {
                std::lock_guard lock{map_mutex};
                in_flight = false;
                if (job->generation == generation && requested &&
                    *requested == job->fingerprint && allowed.load() && !stopping.load()) {
                    map_retry_required=!result||!result->frame;
                    if(result&&result->frame){
                        if(completed&&completed->frame==result->frame){
                            // Metadata/equipment-only update keeps the already
                            // loaded map key and immutable pixels.
                            completed=std::move(result);
                        }else if(completed&&completed->frame&&completed->frame->reveal_intermediate&&
                                 belmont_world_map::IsNewReveal(*completed->frame,*result->frame)){
                            // Retarget from the pixels currently on screen. The
                            // renderer carries unfinished dirty regions forward,
                            // so walking into another zone cannot snap the old
                            // reveal to its endpoint before the next fade.
                            active_reveal_target=result;finish_reveal.reset();
                            fade_work=FadeWork{generation,result,completed->frame,1};fade_due=std::chrono::steady_clock::now();wake.notify_one();
                        }else if(finish_reveal&&finish_reveal->frame&&
                                 belmont_world_map::IsNewReveal(*finish_reveal->frame,*result->frame)){
                            // A new discovery interrupted the previous fade.
                            // Finish its exact native endpoint first; never
                            // blend disjoint dirty tiles from an unfinished
                            // arbitrary intermediate into another reveal.
                            auto finish=std::make_shared<Rendered>(*finish_reveal);
                            finish->generation=generation;
                            StageMapLocked(std::move(finish),FadeWork{generation,result,finish_reveal->frame,0});
                            active_reveal_target=result;finish_reveal.reset();
                        }else if(completed&&completed->frame&&belmont_world_map::IsNewReveal(*completed->frame,*result->frame)){
                            active_reveal_target=result;finish_reveal.reset();
                            fade_work=FadeWork{generation,result,completed->frame,1};fade_due=std::chrono::steady_clock::now();wake.notify_one();
                        }else {finish_reveal.reset();StageMapLocked(std::move(result));}
                    }else if(!completed||!completed->frame)completed=std::move(result);
                    retry_after=std::chrono::steady_clock::now()+std::chrono::seconds(1);
                }
            }
        }
    }
};
EdenDsmodBool Supports(const char* s) { return belmont::Supports(s); }
void* Create(const EdenDsmodHostApi* h, const char*) {
    try { return Valid(h) && SupportedHost(h) ? new Module{*h} : nullptr; }
    catch (...) { return nullptr; }
}
void Destroy(void* p) { try { delete static_cast<Module*>(p); } catch (...) {} }
void Publish(const EdenDsmodHostApi& h, const belmont::Snapshot& s, Module* m = nullptr, const belmont_objectives::Snapshot& ob = {}, const belmont_equipment::Snapshot& equipment = {},const belmont_equipment_stats::Snapshot& stats = {},bool dim=false,const belmont::Snapshot* live=nullptr) {
    const auto i = [&](const char* key, std::int64_t v) { h.publish_i64(h.userdata, key, v); };
    const auto t = [&](const char* key, const char* v) { h.publish_text(h.userdata, key, v); };
    const auto f = [&](const char* key, double v) { if (h.publish_f64) h.publish_f64(h.userdata, key, v); };
    i("belmont.equipment_input_button",0);i("belmont.equipment_control_ready",0);
    i("belmont.equipment_visible",m&&m->equipment_visible);
    i("belmont.picker_ready",0);i("belmont.pin_picker",0);i("belmont.pin_capture_pending",0);i("belmont.pin_armed",0);
    if(!m){
        i("belmont.pin_editing",0);i("belmont.picker_count",0);i("belmont.picker_can_prev",0);i("belmont.picker_can_next",0);

    }
    i("belmont.presentation_dim",dim);
    i("belmont.live_world",live?World(*live):World(s));
    t("belmont.live_state_reason",live?live->reason.c_str():s.reason.c_str());
    i("belmont.build_supported", s.supported); i("belmont.state_coherent", s.coherent);
    i("belmont.manager_ready", s.manager_ready); i("belmont.entity_ready", s.entity_ready);
    i("belmont.application_loading", s.application_loading); i("belmont.room_loading", s.room_loading);
    i("belmont.account_initialized", s.account_initialized); i("belmont.initial_identity", s.initial_identity);
    i("belmont.loading_identity", s.loading_identity); i("belmont.paused", s.paused);
    i("belmont.diag.components_ready", s.components_ready); i("belmont.diag.explorer_candidate", s.explorer_candidate);
    i("belmont.diag.room_candidate", s.room_candidate); i("belmont.diag.position_candidate", s.position_candidate);
    i("belmont.diag.exploration_count", s.exploration_count); i("belmont.diag.exploration_ready", s.exploration_ready);
    i("belmont.diag.exploration_zones_ready", s.exploration_zones_ready);
    std::int64_t revealed{};
    for (const auto& r : s.exploration) for (auto cell : r.cells) revealed += cell == 2 || cell == 3;
    i("belmont.diag.revealed_chunks", revealed); i("belmont.diag.room_state", s.room_state);
    t("belmont.diag.room_id", s.room_id.c_str());
    f("belmont.diag.target_x", s.target_position[0]); f("belmont.diag.target_y", s.target_position[1]);
    f("belmont.diag.target_z", s.target_position[2]);
    const bool native = s.supported && m && m->assets.FontReady();
    const bool loading = s.supported && s.coherent && (s.application_loading == 1 ||
        s.room_loading == 1 || s.loading_identity == 1);
    const bool world = World(s);
    // Pre-game RoomManager TypeInfo may still be an encoded metadata handle.
    // No world is displayed; do not require that lazy root merely to show title art.
    const bool title = s.supported && s.coherent && !loading && s.manager_ready &&
        s.initial_identity == 0 && s.loading_identity == 0 && !s.entity_ready &&
        s.application_loading == 0;
    bool ready{};
    std::shared_ptr<const Rendered> rendered;
    std::string key,prefetch_key;
    if (m) {
        std::lock_guard lock{m->map_mutex};
        rendered = m->completed;prefetch_key=m->staged_map_key;
        ready = native && world && h.publish_f64 && m->requested &&
            rendered && rendered->frame && rendered->fingerprint && rendered->fingerprint->DisplayCompatible(s,rendered->frame.get()) &&
            rendered->navigation_proven && !rendered->area_name.empty() && !rendered->area_key.empty() && m->areas_served.load() && m->marker_native_ready.load() &&
            (rendered->fingerprint->room!=s.room_id||Contains(rendered->navigation_bounds, s.target_position[0], s.target_position[1])) &&
            Contains(rendered->frame->world_bounds, s.target_position[0], s.target_position[1]);
        m->exposed = ready;
        m->equipment_exposed=world&&equipment.ready&&rendered&&rendered->fingerprint&&rendered->fingerprint->entity==s.entity&&SameEquipment(rendered->fingerprint->equipment,equipment)&&rendered->equipment_prepared;
        if (ready) key = m->current_key;
    }
    t("belmont.pending_map_key",prefetch_key.c_str());
    if(m) {
        m->presentation_dim=dim;
        if(title)m->pins.Clear();
        const auto bounds=ready?rendered->frame->world_bounds:belmont_map_fog::Rect{};
        m->pins.Context(ready?rendered->area_key:"",{bounds.x,bounds.y,bounds.x+bounds.width,bounds.y+bounds.height},ready&&!dim);
        const auto sequence=h.get_i64?h.get_i64(h.userdata,"@map_tap_seq",-1):-1;
        if(m->pins.Waiting()&&h.get_f64)m->pins.Capture(sequence,h.get_f64(h.userdata,"@map_tap_x",NAN),h.get_f64(h.userdata,"@map_tap_y",NAN));
        m->map_tap_sequence=sequence;
        i("belmont.pin_capture_pending",m->pins.Waiting());i("belmont.pin_armed",m->pins.Armed());
        i("belmont.pin_picker",m->pins.Picker());i("belmont.pin_editing",m->pins.Editing());
        for(std::size_t n=0;n<belmont_pins::KindCount;++n){const auto name="belmont.pin_kind_"+std::to_string(n)+"_image";const auto image=belmont_pins::Image(n);t(name.c_str(),image.c_str());}
        for(std::size_t n=0;n<belmont_pins::Capacity;++n){
            const auto& pin=m->pins.All()[n];const auto prefix="belmont.pin."+std::to_string(n);
            const bool show=ready&&pin&&pin->area==rendered->area_key;
            i((prefix+".ready").c_str(),show);f((prefix+".x").c_str(),show?pin->x:0);f((prefix+".y").c_str(),show?pin->y:0);
            const auto image=show?belmont_pins::Image(pin->kind):"";t((prefix+".image").c_str(),image.c_str());
        }
    }
    if(m){
        const auto& inv=m->inventory;const auto& control=inv.control;
        const bool fresh=world&&!dim&&inv.ready&&inv.coherent&&inv.entity==s.entity;
        m->control_ready=fresh&&control.ready;
        i("belmont.equipment_control_ready",m->control_ready);
        t("belmont.equipment_control_reason",control.reason.c_str());
        i("belmont.equipment_control_busy",m->equipment_controller.busy());
        t("belmont.equipment_action_reason",m->equipment_controller.reason());
        const bool picker=fresh&&control.ready&&control.picker&&control.equipment_tab;
        std::vector<belmont_inventory::Slot> slots;
        if(picker)for(const auto& slot:control.slots)
            if(slot.item.type==control.category &&
               (control.holding_charm ? (slot.index<0&&slot.enabled) :
                (slot.index>=0&&!slot.item.id.empty()&&
                 (slot.enabled||(control.category==3&&slot.native_page>=0&&slot.native_page<control.native_pages)))))
                slots.push_back(slot);
        // Membership identity deliberately excludes native focus/page/enable
        // flags. Scrolling remains stable while the controller routes a relic
        // to its source page and waits for fresh UIButton selection.
        std::string signature=std::to_string(inv.entity)+":"+std::to_string(control.category)+":"+std::to_string(control.holding_charm);
        for(const auto& slot:slots)signature+=":"+std::to_string(slot.index)+":"+std::to_string(slot.item.object)+":"+std::to_string(slot.item.type)+":"+slot.item.id;
        std::lock_guard lock{m->map_mutex};
        const bool lost=!world||dim||!inv.ready||!inv.coherent||inv.entity!=s.entity||(control.ready&&!control.picker);
        if(!picker&&lost){
            if(!m->picker_signature.empty())++m->picker_generation;
            m->picker_signature.clear();m->picker_slots.clear();m->picker_images.clear();
        } else if(picker){
            if(signature!=m->picker_signature){m->picker_signature=signature;m->picker_images.clear();++m->picker_generation;}
            m->picker_slots=slots;
        }
        m->picker_exposed=picker;
        i("belmont.picker_ready",picker);i("belmont.picker_category",picker?control.category:0);i("belmont.picker_count",picker?slots.size():0);
        i("belmont.picker_reset_epoch",m->picker_generation);
        i("belmont.picker_can_prev",0);i("belmont.picker_can_next",0);
        const char* picker_title=control.holding_charm?"Choose relic slot":control.category==1?"Armoury":control.category==2?"Arcanas":"Relics";
        t("belmont.picker_title",picker?picker_title:"");t("belmont.picker_page_text","");
        i("belmont.picker_selected_ready",0);t("belmont.picker_selected_name","");t("belmont.picker_selected_image","");
        for(std::size_t n=0;n<slots.size();++n){
            const EquipmentImage* image=nullptr;for(const auto& entry:m->picker_images)if(entry.slot==std::to_string(n)){image=&entry;break;}
            const auto prefix="belmont.picker_slot_"+std::to_string(n);
            const bool entry=picker&&n<slots.size();const bool selected=entry&&slots[n].selected;
            const auto image_key=entry?std::string(PickerPrefix)+std::to_string(m->picker_generation)+":"+std::to_string(n):"";
            i((prefix+"_ready").c_str(),entry&&image);i((prefix+"_selected").c_str(),selected);
            i((prefix+"_token").c_str(),entry?m->picker_generation*128+n:0);
            t((prefix+"_name").c_str(),entry&&image?image->name.c_str():"");
            // Keys must be exposed before art-ready: visible repeats trigger
            // the host asset worker to resolve this exact generation/index.
            t((prefix+"_image").c_str(),image_key.c_str());
            if(selected&&image){i("belmont.picker_selected_ready",1);t("belmont.picker_selected_name",image->name.c_str());t("belmont.picker_selected_image",image->key.c_str());}
        }
    }
    i("belmont.map_ready", ready); i("belmont.marker_ready", ready);
    i("belmont.native_assets_ready", native); i("belmont.ui_title", title);
    i("belmont.ui_loading", loading); i("belmont.ui_world", world);
    i("belmont.ui_menu", world && s.paused == 1); i("belmont.place_ready", 0);
    i("__font_epoch", native ? 1 : 0);
    i("belmont.diag.render_candidate_ready", bool(rendered));
    i("belmont.diag.navigation_proven", rendered && rendered->navigation_proven);
    i("belmont.diag.render_decorative_approximation", rendered && rendered->frame && rendered->frame->decorative_approximation);
    i("belmont.diag.locale_fixture_only", 1);
    t("belmont.map_area", ready ? rendered->area_key.c_str() : ""); t("belmont.map_image", ready ? key.c_str() : "");
    t("belmont.area_name", ready ? rendered->area_name.c_str() : ""); t("belmont.selected_place", "");
    t("belmont.biome_name", ready ? rendered->biome_name.c_str() : "");
    // Home width is a layout scale, not authored geometry. Source world bounds
    // and visibility remain native; host pan/zoom owns the independent view.
    // Sub-area home: a fixed world window at the full 1240x1080 map widget.
    constexpr double aspect = 1080.0 / 1240.0, width = 96.0, height = width * aspect;
    const double x = ready ? s.target_position[0] : 0, y = ready ? s.target_position[1] : 0;
    f("belmont.player_x", x); f("belmont.player_y", y);
    // rt22 hashes visible floats in coarse world-unit buckets. Scale only
    // these presentation bindings; the Map widget applies the exact inverse.
    // Dyadic multiplication retains the original position's float precision.
    f("belmont.marker_x",x*256);f("belmont.marker_y",y*256);
    f("belmont.view_x0", ready ? x - width / 2 : 0); f("belmont.view_y0", ready ? y - height / 2 : 0);
    f("belmont.view_x1", ready ? x + width / 2 : 0); f("belmont.view_y1", ready ? y + height / 2 : 0);
    // Area home: everything revealed in this navigation group (plus a margin),
    // at least twice the sub-area window, fitted to the widget aspect.
    double ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;
    if (ready) {
        constexpr double margin = 24.0;
        auto b = rendered->frame->revealed_bounds;
        if (b.width <= 0 || b.height <= 0) b = rendered->navigation_bounds;
        double w = std::max<double>(b.width + 2 * margin, 2 * width),
               h = std::max<double>(b.height + 2 * margin, 2 * height);
        if (h / w > aspect) w = h / aspect; else h = w * aspect;
        const double cx = b.x + b.width * .5, cy = b.y + b.height * .5;
        ax0 = cx - w / 2; ax1 = cx + w / 2; ay0 = cy - h / 2; ay1 = cy + h / 2;
    }
    f("belmont.area_x0", ax0); f("belmont.area_y0", ay0);
    f("belmont.area_x1", ax1); f("belmont.area_y1", ay1);
    i("belmont.landmarks_ready",s.landmarks_ready&&world);
    i("belmont.landmark_overlay_count",ready?rendered->overlays.size()/2:0);
    if(m) {
        for(const auto& [prefix,key]:m->published_overlays) { i((prefix+".ready").c_str(),0);t((prefix+".image").c_str(),"");f((prefix+".x").c_str(),0);f((prefix+".y").c_str(),0); }
        m->published_overlays.clear();
        if(ready&&s.landmarks_ready)for(const auto& overlay:rendered->overlays) {
            if(std::find(s.discovered_landmarks.begin(),s.discovered_landmarks.end(),overlay.guid)==s.discovered_landmarks.end())continue;
            const auto prefix="belmont.landmark."+overlay.guid+"."+overlay.kind;
            i((prefix+".ready").c_str(),1);t((prefix+".image").c_str(),overlay.key.c_str());f((prefix+".x").c_str(),overlay.x);f((prefix+".y").c_str(),overlay.y);
            m->published_overlays.emplace_back(prefix,overlay.key);
        }
    }
    // Save-point discovery is independent of landmark discovery and fog.
    // Join native room usage to exact authored source/navigation; unknown last
    // save identity cannot choose a normal/last-used variant safely.
    unsigned savepoint_count=0;
    if(m&&ready&&world&&s.last_used_save_room_ready) {
        std::lock_guard lock{m->layout_mutex};
        const auto metadata=m->map_item_meta.find(rendered->frame->source_id);
        if(metadata!=m->map_item_meta.end())for(const auto& point:metadata->second.save_points) {
            const auto& source=point.normal_art;
            const auto room=std::find_if(s.exploration.begin(),s.exploration.end(),[&](const auto& value){return value.room_id==source.room_id;});
            if(room==s.exploration.end()||!belmont_map_landmarks::Visible(point,metadata->second.map_revealed_policy,room->used_save_point,rendered->frame->navigation_zone_id))continue;
            const bool last=s.last_used_save_room==source.room_id;
            const auto& art=last?point.last_used_art:point.normal_art;
            if(art.sprites.size()!=1)continue;
            const auto prefix="belmont.savepoint."+std::to_string(source.source_id)+(last?".last":".normal");
            i((prefix+".ready").c_str(),1);
            const auto& sprite=art.sprites.front();
            const auto image="module:belmont:map-icon:"+sprite.bundle+"#"+std::to_string(sprite.id);
            t((prefix+".image").c_str(),image.c_str());
            f((prefix+".x").c_str(),art.x);
            f((prefix+".y").c_str(),art.y);
            ++savepoint_count;
        }
    }
    i("belmont.savepoint_overlay_count",savepoint_count);
    // Native elevator discovery proves visibility independently of usage.
    // Secret-linked and used-only reveals need separate readers and stay hidden.
    unsigned elevator_count=0;
    if(m&&ready&&world) {
        std::lock_guard lock{m->layout_mutex};
        const auto metadata=m->map_item_meta.find(rendered->frame->source_id);
        if(metadata!=m->map_item_meta.end())for(const auto& point:metadata->second.elevators) {
            const auto& art=point.art;
            const auto room=std::find_if(s.exploration.begin(),s.exploration.end(),[&](const auto& value){return value.room_id==art.room_id;});
            if(room==s.exploration.end()||!room->entered_zones_ready)continue;
            const int entered=std::find(room->entered_zone_ids.begin(),room->entered_zone_ids.end(),art.exploration_zone_id)!=room->entered_zone_ids.end()?1:0;
            if(!belmont_map_landmarks::Visible(point,metadata->second.map_revealed_policy,entered,rendered->frame->navigation_zone_id)||art.sprites.size()!=1)continue;
            const auto prefix="belmont.elevator."+std::to_string(art.source_id);
            const auto& sprite=art.sprites.front();
            const auto image="module:belmont:map-icon:"+sprite.bundle+"#"+std::to_string(sprite.id);
            i((prefix+".ready").c_str(),1);t((prefix+".image").c_str(),image.c_str());
            f((prefix+".x").c_str(),art.x);f((prefix+".y").c_str(),art.y);
            ++elevator_count;
        }
    }
    i("belmont.elevator_overlay_count",elevator_count);
    const bool objective_ready=ready&&ob.ready&&rendered->fingerprint&&SameObjective(rendered->fingerprint->objective,ob)&&!rendered->objective_name.empty()&&rendered->objective_image;
    i("belmont.objective_ready",objective_ready);t("belmont.objective",objective_ready?rendered->objective_name.c_str():"");
    t("belmont.objective_image",objective_ready?rendered->objective_key.c_str():"");
    const bool equipment_ready=world&&equipment.ready&&equipment.coherent;
    i("belmont.equipment_ready",equipment_ready);
    t("belmont.equipment_weapon",equipment_ready?equipment.weapon.id.c_str():"");
    t("belmont.equipment_spell",equipment_ready?equipment.spell.id.c_str():"");
    t("belmont.equipment_skin",equipment_ready?equipment.skin.id.c_str():"");
    i("belmont.equipment_charm_slots",equipment_ready?equipment.charms.size():0);
    for(unsigned n=0;n<16;++n) {const auto key="belmont.equipment_charm_"+std::to_string(n);t(key.c_str(),equipment_ready&&n<equipment.charms.size()?equipment.charms[n].id.c_str():"");}
    const bool assets_ready=equipment_ready&&rendered&&rendered->equipment_prepared&&m&&m->equipment_exposed;
    i("belmont.equipment_assets_ready",assets_ready);
    const auto publish_item=[&](const std::string& slot,const belmont_equipment::Item& item) {
        const auto prefix="belmont.equipment_"+slot;
        const EquipmentImage* image=nullptr;
        if(assets_ready)for(const auto& candidate:rendered->equipment_images)if(candidate.slot==slot){image=&candidate;break;}
        i((prefix+"_ready").c_str(),equipment_ready&&(!item.equipped||image));
        i((prefix+"_equipped").c_str(),equipment_ready&&item.equipped);
        t((prefix+"_name").c_str(),image?image->name.c_str():"");t((prefix+"_image").c_str(),image?image->key.c_str():"");
    };
    publish_item("weapon",equipment.weapon);publish_item("spell",equipment.spell);publish_item("skin",equipment.skin);
    for(unsigned n=0;n<3;++n)publish_item("charm_"+std::to_string(n),n<equipment.charms.size()?equipment.charms[n]:belmont_equipment::Item{});
    for(const auto* slot:{"weapon_card","arcana"}) {
        const EquipmentImage* image=nullptr;
        if(assets_ready)for(const auto& candidate:rendered->equipment_images)if(candidate.slot==slot){image=&candidate;break;}
        const auto prefix=std::string("belmont.equipment_")+slot;
        i((prefix+"_ready").c_str(),image!=nullptr);t((prefix+"_image").c_str(),image?image->key.c_str():"");
        t((prefix+"_name").c_str(),image?image->name.c_str():"");
    }
    const bool stat_ready=world&&stats.coherent&&stats.entity==s.entity;
    const auto value=[&](const char* name,const belmont_equipment_stats::Value& v) {
        const auto prefix=std::string("belmont.equipment_")+name;
        i(prefix.c_str(),stat_ready&&v.ready?v.value:0);i((prefix+"_ready").c_str(),stat_ready&&v.ready);
    };
    value("atk",stats.atk);value("def",stats.defense);value("atk_bonus",stats.atk_bonus);value("def_bonus",stats.defense_bonus);
    value("current_hp",stats.current_hp);value("max_hp",stats.max_hp);value("current_mp",stats.current_mp);value("max_mp",stats.max_mp);value("level",stats.level);
    const auto attack=[&](const char* name,const auto& v) {
        const auto prefix=std::string("belmont.equipment_")+name;const bool valid=stat_ready&&v.ready;
        const auto text=valid?std::to_string(v.value):"";
        i(prefix.c_str(),valid?v.value:0);i((prefix+"_ready").c_str(),valid);t((prefix+"_text").c_str(),text.c_str());
    };
    attack("atk",stats.atk_display);attack("def",stats.defense_display);
    const auto pair=[&](const char* name,const auto& v,const auto& max) {
        const auto prefix=std::string("belmont.equipment_")+name;const bool valid=stat_ready&&v.ready&&max.ready;
        const auto text=valid?std::to_string(v.value)+" / "+std::to_string(max.value):"";
        i((prefix+"_ready").c_str(),valid);t((prefix+"_text").c_str(),text.c_str());
    };
    pair("hp",stats.current_hp,stats.max_hp);pair("mp",stats.current_mp,stats.max_mp);
    for (unsigned kind=0;kind<2;++kind) {
        const auto& completed=kind?stats.mana_upgrades:stats.health_upgrades;
        const auto& fragments=kind?stats.mana_fragments:stats.health_fragments;
        const auto& needed=kind?stats.mana_fragments_needed:stats.health_fragments_needed;
        const std::string prefix=kind?"belmont.mana_upgrade_":"belmont.health_upgrade_";
        bool row_ready=stat_ready;
        for(unsigned n=0;n<6;++n){
            const auto pieces=belmont_equipment_stats::UpgradePieces(completed,fragments,needed,n);
            row_ready=row_ready&&pieces.ready;
            const auto key=stat_ready&&pieces.ready?std::string(UpgradePrefix)+std::to_string(kind)+":"+std::to_string(pieces.value):"";
            t((prefix+std::to_string(n)+"_image").c_str(),key.c_str());
        }
        i(kind?"belmont.mana_upgrades_ready":"belmont.health_upgrades_ready",row_ready);
    }
    t("belmont.state_reason", s.reason.c_str());
}
void Sample(void* p, const EdenDsmodHostApi* h) {
    if (!p || !Valid(h)) return;
    auto& m = *static_cast<Module*>(p);
    try {
        auto read = [&](std::uint64_t at, void* dst, std::size_t n) {
            return at <= UINT64_MAX - n && h->is_mapped(h->userdata, at, n) &&
                   h->read_memory(h->userdata, at, dst, n);
        };
        const auto now=std::chrono::steady_clock::now();
        {std::lock_guard lock{m.map_mutex};m.AdvanceMapLocked(now);}
        belmont::Decoder decoder(read);
        bool full=!m.full_source||now-m.full_sample_at>=std::chrono::milliseconds(100);
        auto s=full?decoder.Sample(Identity(*h)):decoder.SampleMotion(Identity(*h),*m.full_source);
        if(!full&&s.reason=="motion_refresh_required"){s=decoder.Sample(Identity(*h));full=true;}
        if(full){m.full_sample_at=now;if(World(s))m.full_source=s;else m.full_source.reset();}
        LiveBundle next;next.base=s;
        if(World(s)) {
            const bool refresh=m.details.base.entity!=s.entity||m.details.base.room_id!=s.room_id||
                now-m.details_at>=std::chrono::milliseconds(120)||!World(m.details.base);
            if(refresh){
                belmont_objectives::Decoder objective_reader(read);next.objective=objective_reader.Sample(Identity(*h),s);
                if(m.equipment_visible){
                    belmont_equipment::Decoder equipment_reader(read);next.equipment=equipment_reader.Sample(Identity(*h),s);
                    belmont_equipment_stats::Decoder stats_reader(read);next.stats=stats_reader.Sample(Identity(*h),s);
                }
                m.details=next;m.details_at=now;
            } else {next.objective=m.details.objective;next.equipment=m.details.equipment;next.stats=m.details.stats;}
        } else {m.details={};m.full_source.reset();}
        auto display=m.presentation.Select(std::move(next),World(s),now);
        belmont_equipment_control::Button input=belmont_equipment_control::Button::None;
        if(World(s)&&m.equipment_visible){
            const bool control_due=m.equipment_controller.busy()||!m.inventory.ready||
                m.inventory.entity!=s.entity||now-m.inventory_control_at>=std::chrono::milliseconds(100);
            if(control_due){
                belmont_inventory::Decoder inventory_reader(read);
                bool refresh=!m.inventory.ready||m.inventory.entity!=s.entity||now-m.inventory_at>=std::chrono::milliseconds(500);
                auto inventory=refresh?inventory_reader.Sample(Identity(*h),s):inventory_reader.SampleControl(Identity(*h),s,m.inventory);
                if(!refresh&&inventory.reason=="inventory_changed"){inventory=inventory_reader.Sample(Identity(*h),s);refresh=true;}
                if(refresh)m.inventory_at=now;
                m.inventory_control_at=now;m.inventory=std::move(inventory);
            }
            input=m.equipment_controller.Update(m.inventory,true,std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        }else if(display.dim){
            // Compatible presentation interruption: emit no inputs and require
            // a full fresh inventory before resuming the observed native step.
            m.inventory_at={};
        }else {m.inventory={};m.equipment_controller.Cancel("equipment_hidden_or_world_transition");}
        m.allowed.store(s.supported);m.Queue(display.value.base,display.value.objective,display.value.equipment);
        Publish(*h,display.value.base,&m,display.value.objective,display.value.equipment,display.value.stats,display.dim,&s);
        h->publish_i64(h->userdata,"belmont.equipment_input_button",static_cast<std::int32_t>(input));
    } catch (...) {
        try {
            std::lock_guard lock{m.map_mutex}; m.InvalidateLocked();
        } catch (...) {}
        try { Publish(*h, belmont::Snapshot{}); } catch (...) {}
    }
}
void Tick(void*, const EdenDsmodHostApi*) {}
void Configure(void* p, const EdenDsmodHostExtensions* ext) {
    if (!p) return;
    try {
        auto& m = *static_cast<Module*>(p); m.Stop();
        { std::lock_guard lock{m.map_mutex}; m.InvalidateLocked(); }
        m.marker_native_ready.store(false); m.assets.Configure(ext);m.presentation.Clear();m.full_source.reset();m.details={};m.pins.Clear();
        ce_unity::AstcDecoder astc;
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->struct_size == sizeof(*ext) &&
            ext->abi_hash == EDEN_DSMOD_EXT_HASH && ext->decode_astc)
            astc = ce_unity::MakeHostAstcDecoder(ext->userdata, ext->decode_astc);
        if (!m.allowed.load() || !m.io_host.read_romfs) return;
        {
            std::lock_guard lock{m.map_mutex};
            m.lazy_equipment_assets=std::make_shared<belmont_equipment_assets::Reader>(&m.io_host,astc);
            m.lazy_picker_ui=std::make_shared<belmont_assets::Reader>(&m.io_host,astc);
            m.picker_exposed=false;m.picker_signature.clear();m.picker_slots.clear();m.picker_images.clear();++m.picker_generation;
        }
        m.stopping.store(false);
        m.font_worker = std::thread([&m] {
            while (!m.stopping.load() && m.allowed.load()) {
                try {
                    if (m.assets.PrepareFont()) {
                        bool valid{};
                        const auto sink = +[](void* p, std::uint32_t w, std::uint32_t h,
                            const std::uint8_t* rgba, std::size_t n) {
                            *static_cast<bool*>(p) = rgba && w == 128 && h == 128 && n == 128u * 128u * 4u;
                        };
                        if (m.assets.LoadImage("module:belmont:rose-marker", &valid, sink) && valid) {
                            m.marker_native_ready.store(true); return;
                        }
                    }
                } catch (...) {}
                for (unsigned n = 0; n < 20 && !m.stopping.load(); ++n)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
        m.map_worker = std::thread([&m, astc] {
            // Constructor/cache allocation failure must never escape a worker
            // and terminate the emulator process.
            try { m.Run(astc); }
            catch (...) {
                try {
                    std::lock_guard lock{m.map_mutex};
                    m.in_flight = false; m.InvalidateLocked();
                    m.retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                } catch (...) {}
            }
        });
    } catch (...) {
        try {
            auto& m = *static_cast<Module*>(p); m.Stop();
            std::lock_guard lock{m.map_mutex}; m.InvalidateLocked();
        } catch (...) {}
    }
}
EdenDsmodBool LoadImage(void* p, const EdenDsmodHostApi* h, const char* key,
                        void* receiver, EdenDsmodImageSink sink) {
    if (!p || !sink || !SupportedHost(h)) return EDEN_DSMOD_FALSE;
    auto& m = *static_cast<Module*>(p);
    if (!m.allowed.load()) return EDEN_DSMOD_FALSE;
    const auto k = Key(key);
    if (!k) return EDEN_DSMOD_FALSE;
    constexpr std::string_view MapIconPrefix="module:belmont:map-icon:";
    if(k->starts_with(MapIconPrefix)){
        try{
            const auto source_key=k->substr(MapIconPrefix.size());
            std::optional<belmont_map_landmarks::Sprite> metadata;
            {
                std::lock_guard lock{m.layout_mutex};
                const auto find=[&](const auto& art){for(const auto& sprite:art.sprites)
                    if(sprite.bundle+"#"+std::to_string(sprite.id)==source_key)metadata=sprite;};
                for(const auto& [id,items]:m.map_item_meta){
                    for(const auto& point:items.save_points){find(point.normal_art);find(point.last_used_art);}
                    for(const auto& point:items.elevators)find(point.art);
                }
            }
            if(!metadata)return EDEN_DSMOD_FALSE;
            ce_unity::Image source;
            const auto receive=+[](void* out,std::uint32_t w,std::uint32_t h,const std::uint8_t* bytes,std::size_t size){
                if(!bytes||!w||!h||w>2048||h>2048||size!=std::size_t(w)*h*4)return;
                auto& image=*static_cast<ce_unity::Image*>(out);image.width=w;image.height=h;image.rgba.assign(bytes,bytes+size);
            };
            const auto native="module:native:"+std::string(source_key);
            if(!m.assets.LoadImage(native.c_str(),&source,receive)||source.rgba.empty())return EDEN_DSMOD_FALSE;
            const auto& sprite=*metadata;
            const long width=std::lround(sprite.width),height=std::lround(sprite.height);
            const long x=std::lround(sprite.texture_rect_offset_x),
                       y=height-std::lround(sprite.texture_rect_offset_y)-source.height;
            if(width<1||height<1||width>2048||height>2048||x<0||y<0||
               x+source.width>width||y+source.height>height)return EDEN_DSMOD_FALSE;
            ce_unity::Image image{static_cast<unsigned>(width),static_cast<unsigned>(height),{}};
            image.rgba.resize(std::size_t(width)*height*4);
            for(unsigned row=0;row<source.height;++row)
                std::copy_n(source.rgba.data()+std::size_t(row)*source.width*4,std::size_t(source.width)*4,
                    image.rgba.data()+(std::size_t(y+row)*image.width+x)*4);
            sink(receiver,image.width,image.height,image.rgba.data(),image.rgba.size());
            return EDEN_DSMOD_TRUE;
        }catch(...){return EDEN_DSMOD_FALSE;}
    }
    constexpr std::string_view PinPrefix="module:belmont:pin-icon:";
    if(k->starts_with(PinPrefix)){
        try{
            unsigned kind{};const auto number=k->substr(PinPrefix.size());
            const auto parsed=std::from_chars(number.data(),number.data()+number.size(),kind);
            if(parsed.ec!=std::errc{}||parsed.ptr!=number.data()+number.size()||kind>=belmont_pins::KindCount)return EDEN_DSMOD_FALSE;
            ce_unity::Image source;
            const auto receive=+[](void* out,std::uint32_t w,std::uint32_t h,const std::uint8_t* bytes,std::size_t size){
                if(!bytes||!w||!h||w>2048||h>2048||size!=std::size_t(w)*h*4)return;
                auto& image=*static_cast<ce_unity::Image*>(out);image.width=w;image.height=h;image.rgba.assign(bytes,bytes+size);
            };
            const auto key=belmont_pins::NativeImage(kind);
            if(!m.assets.LoadImage(key.c_str(),&source,receive))return EDEN_DSMOD_FALSE;
            const auto image=SquareIcon(source);if(!image)return EDEN_DSMOD_FALSE;
            sink(receiver,image->width,image->height,image->rgba.data(),image->rgba.size());return EDEN_DSMOD_TRUE;
        }catch(...){return EDEN_DSMOD_FALSE;}
    }
    if(k->starts_with(MapPrefix)){
        try{
            std::shared_ptr<const belmont_world_map::Frame> frame;
            {
                std::lock_guard lock{m.map_mutex};
                if(m.stopping.load())return EDEN_DSMOD_FALSE;
                if(m.staged_map&&*k==m.staged_map_key)frame=m.staged_map->frame;
                else if(m.exposed){
                    if(m.completed&&*k==m.current_key)frame=m.completed->frame;
                    else for(const auto& prior:m.recent_maps)if(prior.key==*k){frame=prior.frame;break;}
                }
            }
            if(!frame)return EDEN_DSMOD_FALSE;
            sink(receiver,frame->image.width,frame->image.height,frame->image.rgba.data(),frame->image.rgba.size());
            {
                std::lock_guard lock{m.map_mutex};
                if(m.staged_map&&*k==m.staged_map_key&&m.staged_map->frame==frame&&!m.staged_served){
                    m.staged_served=true;m.staged_served_epoch=m.sample_epoch;
                }
            }
            return EDEN_DSMOD_TRUE;
        }catch(...){return EDEN_DSMOD_FALSE;}
    }
    if(k->starts_with(PickerPrefix)||k->starts_with(UpgradePrefix)){
        try {
            const bool upgrade=k->starts_with(UpgradePrefix);
            const auto tail=k->substr(upgrade?UpgradePrefix.size():PickerPrefix.size());
            const auto separator=tail.find(':');
            if(separator==std::string_view::npos)return EDEN_DSMOD_FALSE;
            std::uint64_t first{},second{};
            const auto parse=[](std::string_view v,std::uint64_t& n){auto p=std::from_chars(v.data(),v.data()+v.size(),n);return p.ec==std::errc{}&&p.ptr==v.data()+v.size();};
            if(!parse(tail.substr(0,separator),first)||!parse(tail.substr(separator+1),second))return EDEN_DSMOD_FALSE;
            std::shared_ptr<belmont_equipment_assets::Reader> reader;
            std::shared_ptr<belmont_assets::Reader> ui;
            belmont_inventory::Slot slot;
            std::shared_ptr<const ce_unity::Image> output;
            {
                std::lock_guard lock{m.map_mutex};
                reader=m.lazy_equipment_assets;ui=m.lazy_picker_ui;
                if(!reader||m.stopping.load())return EDEN_DSMOD_FALSE;
                if(!upgrade){
                    if(!m.picker_exposed||first!=m.picker_generation||second>=m.picker_slots.size())return EDEN_DSMOD_FALSE;
                    slot=m.picker_slots[second];
                    for(const auto& image:m.picker_images)if(image.key==*k){output=image.image;break;}
                }
            }
            std::string name;
            if(upgrade){
                if(first>1||second>4)return EDEN_DSMOD_FALSE;
                if(auto image=reader->ResolveUpgrade(first!=0,unsigned(second)))output=std::make_shared<ce_unity::Image>(std::move(*image));
            }else if(!output){
                if(slot.index<0&&slot.item.id.empty()){
                    if(ui)if(auto image=ui->LoadImage("native:ui-items_assets_all_b43d65be14dfeab613bf4484f610b996.bundle#3721789137048354554"))output=SquareIcon(*image);
                    name="Empty slot";
                }else if(auto item=reader->ResolveEnglish(static_cast<belmont_equipment_assets::Type>(slot.item.type),slot.item.id)){
                    output=SquareIcon(item->image);name=item->name;
                }
                if(!output)return EDEN_DSMOD_FALSE;
                // Decoding may overlap category/held-stage changes. Never
                // install pixels/names from an older logical item generation.
                std::lock_guard lock{m.map_mutex};
                if(!m.picker_exposed||first!=m.picker_generation||second>=m.picker_slots.size()||
                   m.picker_slots[second].index!=slot.index||m.picker_slots[second].item.object!=slot.item.object||
                   m.picker_slots[second].item.id!=slot.item.id||m.stopping.load())return EDEN_DSMOD_FALSE;
                m.picker_images.push_back({std::to_string(second),name,std::string(*k),output});
            }
            if(!output)return EDEN_DSMOD_FALSE;
            sink(receiver,output->width,output->height,output->rgba.data(),output->rgba.size());return EDEN_DSMOD_TRUE;
        }catch(...){return EDEN_DSMOD_FALSE;}
    }
    if (!k->starts_with(ObjectivePrefix) && !k->starts_with(MapPrefix) && !k->starts_with(LandmarkPrefix) && !k->starts_with(EquipmentPrefix) && !k->starts_with(PickerPrefix)) return m.assets.LoadImage(key, receiver, sink);
    try {
        std::shared_ptr<const ce_unity::Image> output;
        {
            // Validate the generation under the lock, retain immutable pixels,
            // then let the host copy without blocking motion/state sampling.
            std::lock_guard lock{m.map_mutex};
            if(k->starts_with(PickerPrefix)){
                if(!m.picker_exposed)return EDEN_DSMOD_FALSE;
                for(const auto& item:m.picker_images)if(item.key==*k){output=item.image;break;}
            }else{
                const auto frame=m.completed;
                if(!frame)return EDEN_DSMOD_FALSE;
                if(k->starts_with(EquipmentPrefix)){
                    if(!m.equipment_exposed)return EDEN_DSMOD_FALSE;
                    for(const auto& item:frame->equipment_images)if(item.key==*k){output=item.image;break;}
                }else{
                    if(!m.exposed||!frame->frame)return EDEN_DSMOD_FALSE;
                    if(k->starts_with(ObjectivePrefix)){
                        if(*k==frame->objective_key)output=frame->objective_image;
                    }else if(k->starts_with(LandmarkPrefix)){
                        for(const auto& item:frame->overlays)if(item.key==*k){output=item.image;break;}
                    }else {
                        if(*k==m.current_key)output=std::shared_ptr<const ce_unity::Image>(frame->frame,&frame->frame->image);
                        else for(const auto& prior:m.recent_maps)if(prior.key==*k){output=std::shared_ptr<const ce_unity::Image>(prior.frame,&prior.frame->image);break;}
                    }
                }
            }
        }
        if(!output)return EDEN_DSMOD_FALSE;
        sink(receiver,output->width,output->height,output->rgba.data(),output->rgba.size());
        return EDEN_DSMOD_TRUE;
    } catch (...) { return EDEN_DSMOD_FALSE; }
}
EdenDsmodBool LoadData(void* p, const EdenDsmodHostApi* h, const char* key,
                       void* receiver, EdenDsmodDataSink sink) {
    if (!p || !sink || !SupportedHost(h)) return EDEN_DSMOD_FALSE;
    auto& m = *static_cast<Module*>(p);
    const auto k = Key(key);
    if (!m.allowed.load() || !k || *k != "module:belmont:areas") return EDEN_DSMOD_FALSE;
    try {
        if (!m.Bounds(belmont_map_sources::StreetsPoor())) return EDEN_DSMOD_FALSE;
        std::lock_guard lock{m.layout_mutex};
        if (!m.allowed.load()) return EDEN_DSMOD_FALSE;
        sink(receiver, reinterpret_cast<const std::uint8_t*>(m.areas_json.data()), m.areas_json.size());
        m.areas_served.store(true); return EDEN_DSMOD_TRUE;
    } catch (...) { return EDEN_DSMOD_FALSE; }
}
EdenDsmodBool DecodeFont(void* p, const std::uint8_t* bytes, std::size_t n,
                        void* receiver, EdenDsmodFontSink sink) {
    if (!p || !static_cast<Module*>(p)->allowed.load()) return EDEN_DSMOD_FALSE;
    return static_cast<Module*>(p)->assets.DecodeFont(bytes, n, receiver, sink);
}
EdenDsmodBool Action(void* p,const char* action,std::int64_t argument){
    if(!p||!action)return EDEN_DSMOD_FALSE;
    try {
        auto& m=*static_cast<Module*>(p);const std::string_view name(action);
        if(!m.allowed.load())return EDEN_DSMOD_FALSE;
        if(name=="equipment_visibility"){
            if(argument!=0&&argument!=1)return EDEN_DSMOD_FALSE;
            const bool visible=argument==1;
            if(m.equipment_visible!=visible){
                m.equipment_visible=visible;m.details_at={};m.inventory_at={};m.inventory_control_at={};
                if(!visible){m.inventory={};m.control_ready=false;m.equipment_controller.Cancel("equipment_hidden");}
            }
            return EDEN_DSMOD_TRUE;
        }
        if(name=="pin_cancel"){m.pins.Cancel();return EDEN_DSMOD_TRUE;}
        if(m.presentation_dim)return EDEN_DSMOD_FALSE;
        if(name=="equipment_category"){
            if(!m.control_ready||m.equipment_controller.busy()||argument<1||argument>3)return EDEN_DSMOD_FALSE;
            m.equipment_controller.RequestCategory(static_cast<std::int32_t>(argument));return EDEN_DSMOD_TRUE;
        }
        if(name=="equipment_back"){
            if(!m.control_ready)return EDEN_DSMOD_FALSE;
            m.equipment_controller.Back();return EDEN_DSMOD_TRUE;
        }
        if(name=="equipment_pick"){
            if(!m.control_ready||m.equipment_controller.busy())return EDEN_DSMOD_FALSE;
            std::int32_t selected{};
            {
                std::lock_guard lock{m.map_mutex};
                if(argument<0||std::uint64_t(argument)/128!=m.picker_generation)return EDEN_DSMOD_FALSE;
                argument=std::int64_t(std::uint64_t(argument)%128);
                if(!m.picker_exposed||argument>=std::int64_t(m.picker_slots.size()))return EDEN_DSMOD_FALSE;
                bool art=false;for(const auto& image:m.picker_images)if(image.slot==std::to_string(argument))art=true;
                if(!art)return EDEN_DSMOD_FALSE;
                selected=m.picker_slots[std::size_t(argument)].index;
            }
            m.equipment_controller.RequestPick(selected);return EDEN_DSMOD_TRUE;
        }
        if(name=="pin_open")return m.pins.Open();
        if(name=="pin_begin")return m.pins.Begin(m.map_tap_sequence);
        if(name=="pin_kind")return m.pins.Choose(argument);
        if(name=="pin_select")return m.pins.Select(argument);
        if(name=="pin_delete")return m.pins.Remove();
        return EDEN_DSMOD_FALSE;
    }catch(...){return EDEN_DSMOD_FALSE;}
}
const EdenDsmodModuleApi Api{EDEN_DSMOD_MODULE_ABI_VERSION, sizeof(EdenDsmodModuleApi), 0,
    EDEN_DSMOD_MODULE_ABI_HASH, belmont::Title, "Belmont companion prototype",
    EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
    &Supports, &Create, &Destroy, &Sample, &Tick};
const EdenDsmodModuleExtensions Extensions{EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH, &Configure, &Action, &LoadImage};
const EdenDsmodFontExtensions Fonts{EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions),
    EDEN_DSMOD_FONT_EXT_HASH, &DecodeFont};
const EdenDsmodModuleDataExtensions Data{EDEN_DSMOD_DATA_EXT_VERSION, sizeof(EdenDsmodModuleDataExtensions),
    EDEN_DSMOD_DATA_EXT_HASH, &LoadData};
} // namespace
DSMOD_SDK_EXPORT_MODULE(Api)
DSMOD_SDK_EXPORT_EXTENSIONS(Extensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(Fonts)
DSMOD_SDK_EXPORT_DATA_EXTENSIONS(Data)
