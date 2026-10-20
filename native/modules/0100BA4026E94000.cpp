// SPDX-License-Identifier: GPL-3.0-or-later
// Unreleased read-only companion slice. Worker input is a coherent copied
// snapshot; workers never read guest memory or publish host values.
#include "belmont_reader.h"
#include "belmont_asset_callbacks.h"
#include "belmont_map_assets.h"
#include "belmont_map_fog.h"
#include "belmont_map_labels.h"
#include "belmont_map_landmarks.h"
#include "belmont_landmark_render.h"
#include "belmont_equipment.h"
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
#include <set>
#include <sstream>
#include <thread>

namespace {
std::string AreaKey(const belmont_map_sources::SourceRecipe& recipe) {
    return recipe.id == belmont_map_sources::Id::StreetsPoor ? "belmont_streetspoor" : std::string(recipe.world_name);
}
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
    explicit Fingerprint(const belmont::Snapshot& s, const belmont_objectives::Snapshot& ob)
        : manager(s.manager), entity(s.entity), room(s.room_id), exploration(s.exploration),
          landmarks_ready(s.landmarks_ready),discovered(s.discovered_landmarks),objective(ob) {}
    bool operator==(const Fingerprint& b) const {
        return manager == b.manager && entity == b.entity && room == b.room && landmarks_ready==b.landmarks_ready && discovered==b.discovered && SameObjective(objective,b.objective) &&
            exploration.size() == b.exploration.size() &&
            std::equal(exploration.begin(), exploration.end(), b.exploration.begin(), SameRoom);
    }
    bool Matches(const belmont::Snapshot& s, const belmont_objectives::Snapshot& ob) const {
        if (manager != s.manager || entity != s.entity || room != s.room_id || landmarks_ready!=s.landmarks_ready || discovered!=s.discovered_landmarks || !SameObjective(objective,ob) ||
            exploration.size() != s.exploration.size()) return false;
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
    Work(std::uint64_t g, const belmont::Snapshot& s, const belmont_objectives::Snapshot& ob) : generation(g), fingerprint(s,ob) {
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
    belmont_map_fog::Rect navigation_bounds;
    bool navigation_proven{};
    std::string area_name, area_key, biome_name;
    std::string objective_name;
    std::vector<LandmarkImage> overlays;
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
    std::map<belmont_map_sources::Id, belmont_map_fog::Rect> tile_bounds;
    bool areas_initialized{};
    std::map<belmont_map_sources::Id,std::vector<LandmarkMeta>> landmark_meta;
    std::vector<std::pair<std::string,std::string>> published_overlays;
    std::string areas_json, current_key;
    std::uint64_t generation{};
    bool in_flight{}, exposed{};
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
        requested.reset(); pending.reset(); completed.reset(); current_key.clear(); exposed = false;
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
                out<<"]}";
            }
            out << '}';
            areas_json = out.str(); tile_bounds = std::move(found);
            landmark_meta = std::move(found_landmarks); areas_initialized = true;
        }
        const auto it = tile_bounds.find(recipe.id);
        return it == tile_bounds.end() ? std::nullopt : std::optional(it->second);
    }
    void Queue(const belmont::Snapshot& s,const belmont_objectives::Snapshot& ob) {
        const bool eligible = World(s) && belmont_map_sources::FindSourceForRoom(s.room_id) &&
            std::any_of(s.exploration.begin(), s.exploration.end(), [&](const auto& r) {
                return r.room_id == s.room_id && r.entered_zones_ready && !r.entered_zone_ids.empty();
            });
        std::lock_guard lock{map_mutex};
        if (!eligible || !allowed.load()) { InvalidateLocked(); return; }
        if (!requested || !requested->Matches(s,ob)) {
            ++generation; requested.emplace(s,ob); completed.reset(); exposed = false;
            current_key = std::string(MapPrefix) + std::to_string(generation);
            pending.emplace(generation, s,ob); retry_after = {}; wake.notify_one();
        } else if (!completed && !pending && !in_flight &&
                   std::chrono::steady_clock::now() >= retry_after) {
            pending.emplace(generation, s,ob); wake.notify_one();
        }
    }
    void Run(ce_unity::AstcDecoder astc) {
        belmont_world_map::Renderer renderer(&io_host, astc);
        belmont_map_assets::Reader registry(&io_host);
        belmont_map_fog::Reader fog(&io_host);
        belmont_map_labels::Reader labels(&io_host);
        belmont_landmark_render::Renderer overlay_renderer(&io_host,astc);
        belmont_objectives::EnglishReader objective_labels(&io_host);
        while (!stopping.load()) {
            std::optional<Work> job;
            {
                std::unique_lock lock{map_mutex};
                wake.wait(lock, [&] { return stopping.load() || pending.has_value(); });
                if (stopping.load()) return;
                job = std::move(pending); pending.reset(); in_flight = true;
            }
            std::shared_ptr<Rendered> result;
            try {
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
                                result = std::make_shared<Rendered>();
                                result->generation = job->generation;
                                result->frame = std::move(frame);
                                result->navigation_bounds = physical;
                                result->navigation_proven = true;
                                result->area_key = AreaKey(*recipe);
                                // Explicit English fixture only; no locale autodetection claim.
                                if (const auto name = labels.LoadAreaEnglish(job->request.current_room_id)) result->area_name = *name;
                                if (const auto biome = labels.LoadBiomeEnglish(job->request.current_room_id)) result->biome_name = *biome;
                                if (const auto objective=objective_labels.Load(job->fingerprint.objective))result->objective_name=*objective;
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
            } catch (...) { result.reset(); }
            {
                std::lock_guard lock{map_mutex};
                in_flight = false;
                if (job->generation == generation && requested &&
                    *requested == job->fingerprint && allowed.load() && !stopping.load()) {
                    completed = std::move(result);
                    retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
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
void Publish(const EdenDsmodHostApi& h, const belmont::Snapshot& s, Module* m = nullptr, const belmont_objectives::Snapshot& ob = {}, const belmont_equipment::Snapshot& equipment = {}) {
    const auto i = [&](const char* key, std::int64_t v) { h.publish_i64(h.userdata, key, v); };
    const auto t = [&](const char* key, const char* v) { h.publish_text(h.userdata, key, v); };
    const auto f = [&](const char* key, double v) { if (h.publish_f64) h.publish_f64(h.userdata, key, v); };
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
    std::string key;
    if (m) {
        std::lock_guard lock{m->map_mutex};
        rendered = m->completed;
        ready = native && world && h.publish_f64 && m->requested &&
            m->requested->Matches(s,ob) && rendered && rendered->generation == m->generation &&
            rendered->navigation_proven && !rendered->area_name.empty() && !rendered->area_key.empty() && m->areas_served.load() && m->marker_native_ready.load() &&
            Contains(rendered->navigation_bounds, s.target_position[0], s.target_position[1]) &&
            Contains(rendered->frame->world_bounds, s.target_position[0], s.target_position[1]);
        m->exposed = ready;
        if (ready) key = m->current_key;
    }
    i("belmont.map_ready", ready); i("belmont.marker_ready", ready);
    i("belmont.native_assets_ready", native); i("belmont.ui_title", title);
    i("belmont.ui_loading", loading); i("belmont.ui_world", world);
    i("belmont.ui_menu", world && s.paused == 1); i("belmont.place_ready", 0);
    i("__font_epoch", native ? 1 : 0);
    i("belmont.diag.render_candidate_ready", bool(rendered));
    i("belmont.diag.navigation_proven", rendered && rendered->navigation_proven);
    i("belmont.diag.render_decorative_approximation", rendered && rendered->frame->decorative_approximation);
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
        if(ready)for(const auto& overlay:rendered->overlays) {
            const auto prefix="belmont.landmark."+overlay.guid+"."+overlay.kind;
            i((prefix+".ready").c_str(),1);t((prefix+".image").c_str(),overlay.key.c_str());f((prefix+".x").c_str(),overlay.x);f((prefix+".y").c_str(),overlay.y);
            m->published_overlays.emplace_back(prefix,overlay.key);
        }
    }
    const bool objective_ready=ready&&ob.ready&&!rendered->objective_name.empty();
    i("belmont.objective_ready",objective_ready);t("belmont.objective",objective_ready?rendered->objective_name.c_str():"");
    const bool equipment_ready=world&&equipment.ready&&equipment.coherent;
    i("belmont.equipment_ready",equipment_ready);
    t("belmont.equipment_weapon",equipment_ready?equipment.weapon.id.c_str():"");
    t("belmont.equipment_spell",equipment_ready?equipment.spell.id.c_str():"");
    t("belmont.equipment_skin",equipment_ready?equipment.skin.id.c_str():"");
    i("belmont.equipment_charm_slots",equipment_ready?equipment.charms.size():0);
    for(unsigned n=0;n<16;++n) {const auto key="belmont.equipment_charm_"+std::to_string(n);t(key.c_str(),equipment_ready&&n<equipment.charms.size()?equipment.charms[n].id.c_str():"");}
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
        belmont::Decoder decoder(read);
        const auto s = decoder.Sample(Identity(*h));
        belmont_objectives::Snapshot objective;
        belmont_equipment::Snapshot equipment;
        if(World(s)) {
            belmont_objectives::Decoder objective_reader(read);objective=objective_reader.Sample(Identity(*h),s);
            belmont_equipment::Decoder equipment_reader(read);equipment=equipment_reader.Sample(Identity(*h),s);
        }
        m.allowed.store(s.supported); m.Queue(s,objective); Publish(*h, s, &m,objective,equipment);
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
        m.marker_native_ready.store(false); m.assets.Configure(ext);
        ce_unity::AstcDecoder astc;
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->struct_size == sizeof(*ext) &&
            ext->abi_hash == EDEN_DSMOD_EXT_HASH && ext->decode_astc)
            astc = ce_unity::MakeHostAstcDecoder(ext->userdata, ext->decode_astc);
        if (!m.allowed.load() || !m.io_host.read_romfs) return;
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
    if (!k->starts_with(MapPrefix) && !k->starts_with(LandmarkPrefix)) return m.assets.LoadImage(key, receiver, sink);
    try {
        // The sole frame is immutable and bounded to the SDK image cap. Keep the sink
        // copy serialized with invalidation, never a RomFS/render operation.
        std::lock_guard lock{m.map_mutex};
        const auto frame = m.completed;
        if (!m.exposed || !frame || frame->generation != m.generation)
            return EDEN_DSMOD_FALSE;
        if(k->starts_with(LandmarkPrefix)) {
            for(const auto& overlay:frame->overlays)if(*k==overlay.key) {
                sink(receiver,overlay.image->width,overlay.image->height,overlay.image->rgba.data(),overlay.image->rgba.size());return EDEN_DSMOD_TRUE;
            }
            return EDEN_DSMOD_FALSE;
        }
        if(*k!=m.current_key)return EDEN_DSMOD_FALSE;
        sink(receiver, frame->frame->image.width, frame->frame->image.height,
             frame->frame->image.rgba.data(), frame->frame->image.rgba.size());
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
const EdenDsmodModuleApi Api{EDEN_DSMOD_MODULE_ABI_VERSION, sizeof(EdenDsmodModuleApi), 0,
    EDEN_DSMOD_MODULE_ABI_HASH, belmont::Title, "Belmont companion prototype",
    EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
    &Supports, &Create, &Destroy, &Sample, &Tick};
const EdenDsmodModuleExtensions Extensions{EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH, &Configure, nullptr, &LoadImage};
const EdenDsmodFontExtensions Fonts{EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions),
    EDEN_DSMOD_FONT_EXT_HASH, &DecodeFont};
const EdenDsmodModuleDataExtensions Data{EDEN_DSMOD_DATA_EXT_VERSION, sizeof(EdenDsmodModuleDataExtensions),
    EDEN_DSMOD_DATA_EXT_HASH, &LoadData};
} // namespace
DSMOD_SDK_EXPORT_MODULE(Api)
DSMOD_SDK_EXPORT_EXTENSIONS(Extensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(Fonts)
DSMOD_SDK_EXPORT_DATA_EXTENSIONS(Data)
