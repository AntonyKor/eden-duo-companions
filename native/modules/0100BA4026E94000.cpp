// SPDX-License-Identifier: GPL-3.0-or-later
// Unreleased read-only companion slice. Worker input is a coherent copied
// snapshot; workers never read guest memory or publish host values.
#include "belmont_reader.h"
#include "belmont_asset_callbacks.h"
#include "belmont_map_assets.h"
#include "belmont_map_fog.h"
#include "belmont_map_labels.h"
#include "belmont_map_render.h"
#include "core/mods/modules/dsmod_module_sdk.h"
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
constexpr std::string_view FirstRoom =
    "World_Biome_01_Streets_Poor_StreetsPoor_FirstRoom_uid73";
constexpr std::string_view Area = "belmont_first_room";
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
           a.pending_validation == b.pending_validation && a.cells == b.cells &&
           a.entered_zones_ready == b.entered_zones_ready &&
           a.entered_zone_ids == b.entered_zone_ids;
}
struct Fingerprint {
    belmont::U64 manager{}, entity{};
    std::string room;
    std::vector<belmont::ExplorationRoom> exploration;
    explicit Fingerprint(const belmont::Snapshot& s)
        : manager(s.manager), entity(s.entity), room(s.room_id), exploration(s.exploration) {}
    bool operator==(const Fingerprint& b) const {
        return manager == b.manager && entity == b.entity && room == b.room &&
            exploration.size() == b.exploration.size() &&
            std::equal(exploration.begin(), exploration.end(), b.exploration.begin(), SameRoom);
    }
    bool Matches(const belmont::Snapshot& s) const {
        if (manager != s.manager || entity != s.entity || room != s.room_id ||
            exploration.size() != s.exploration.size()) return false;
        for (std::size_t n = 0; n < exploration.size(); ++n)
            if (!SameRoom(exploration[n], s.exploration[n])) return false;
        return true;
    }
};
bool Finite(const belmont_map_fog::Rect& r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
           std::isfinite(r.height) && r.width > 0 && r.height > 0 &&
           r.width <= 512 && r.height <= 512;
}
bool Contains(const belmont_map_fog::Rect& r, float x, float y) {
    return std::isfinite(x) && std::isfinite(y) && x >= r.x && y >= r.y &&
           x <= r.x + r.width && y <= r.y + r.height;
}
struct Work {
    std::uint64_t generation{};
    Fingerprint fingerprint;
    belmont_map_render::Request request;
    Work(std::uint64_t g, const belmont::Snapshot& s) : generation(g), fingerprint(s) {
        request.current_room_id = s.room_id;
        request.player_x = s.target_position[0];
        request.player_y = s.target_position[1];
        request.width = request.height = 768;
        for (const auto& room : s.exploration)
            if (room.room_id == s.room_id)
                request.explored_rooms.push_back({room.room_id, room.entered_zone_ids});
    }
};
struct Rendered {
    std::uint64_t generation{};
    belmont_map_render::Frame frame;
    belmont_map_fog::Rect navigation_bounds;
    bool navigation_proven{};
    std::string area_name;
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
    std::optional<belmont_map_fog::Rect> tile_bounds;
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
    std::optional<belmont_map_fog::Rect> Bounds() {
        // Data extension may load this synchronously because areas_src is fetched
        // once. Sampling never takes this lock or performs these RomFS reads.
        std::lock_guard lock{layout_mutex};
        if (tile_bounds) return tile_bounds;
        if (!allowed.load() || !io_host.read_romfs) return {};
        belmont_map_tiles::Reader source(&io_host);
        const auto tiles = source.LoadStreetsPoorLayout();
        if (!tiles) return {};
        const auto tile = std::find_if(tiles->begin(), tiles->end(),
                                      [](const auto& t) { return t.layer == 16; });
        if (tile == tiles->end()) return {};
        const auto b = tile->bounds;
        belmont_map_fog::Rect r{b.x, b.y, b.width, b.height};
        if (!Finite(r)) return {};
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<float>::max_digits10)
            << "{\"" << Area << "\":{\"image\":\"module:belmont:map:0\",\"min\":["
            << r.x << ',' << r.y << "],\"max\":[" << r.x + r.width << ',' << r.y + r.height
            << "],\"clamp_view\":true,\"no_pin\":false}}";
        areas_json = out.str(); tile_bounds = r;
        return r;
    }
    void Queue(const belmont::Snapshot& s) {
        const bool eligible = World(s) && s.room_id == FirstRoom &&
            std::any_of(s.exploration.begin(), s.exploration.end(), [&](const auto& r) {
                return r.room_id == s.room_id && r.entered_zones_ready && !r.entered_zone_ids.empty();
            });
        std::lock_guard lock{map_mutex};
        if (!eligible || !allowed.load()) { InvalidateLocked(); return; }
        if (!requested || !requested->Matches(s)) {
            ++generation; requested.emplace(s); completed.reset(); exposed = false;
            current_key = std::string(MapPrefix) + std::to_string(generation);
            pending.emplace(generation, s); retry_after = {}; wake.notify_one();
        } else if (!completed && !pending && !in_flight &&
                   std::chrono::steady_clock::now() >= retry_after) {
            pending.emplace(generation, s); wake.notify_one();
        }
    }
    void Run(ce_unity::AstcDecoder astc) {
        belmont_map_render::Renderer renderer(&io_host, astc);
        belmont_map_assets::Reader registry(&io_host);
        belmont_map_fog::Reader fog(&io_host);
        belmont_map_labels::Reader labels(&io_host);
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
                const auto bounds = Bounds();
                const auto world = registry.LoadStreetsPoor();
                const auto native_fog = fog.LoadStreetsPoor();
                if (bounds && world && native_fog && allowed.load()) {
                    const auto room = std::find_if(world->rooms.begin(), world->rooms.end(),
                        [&](const auto& r) { return r.id == job->request.current_room_id; });
                    const auto zones = std::find_if(native_fog->rooms.begin(), native_fog->rooms.end(),
                        [&](const auto& r) { return r.id == job->request.current_room_id; });
                    if (room != world->rooms.end() && zones != native_fog->rooms.end() &&
                        !zones->zones.empty()) {
                        // Scoped companion Follow navigation, not the top-map cursor.
                        // Pinned native current-player initializer and FirstRoom
                        // district membership are independently source-proven.
                        // Native room origin/size and all zone nav IDs are decoded;
                        // no polygon, physical coordinates or nav ID is authored.
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
                            job->request.world_bounds = *bounds;
                            auto frame = renderer.Render(job->request);
                            if (frame && frame->image.width == 768 && frame->image.height == 768 &&
                                frame->image.rgba.size() == 768u * 768u * 4u &&
                                Finite(frame->world_bounds) && frame->world_bounds.x == bounds->x &&
                                frame->world_bounds.y == bounds->y && frame->world_bounds.width == bounds->width &&
                                frame->world_bounds.height == bounds->height) {
                                result = std::make_shared<Rendered>();
                                result->generation = job->generation;
                                result->frame = std::move(*frame);
                                result->navigation_bounds = physical;
                                result->navigation_proven = true;
                                // Explicit English fixture only; no locale autodetection claim.
                                if (const auto name = labels.LoadFirstRoomAreaEnglish()) result->area_name = *name;
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
void Publish(const EdenDsmodHostApi& h, const belmont::Snapshot& s, Module* m = nullptr) {
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
            m->requested->Matches(s) && rendered && rendered->generation == m->generation &&
            rendered->navigation_proven && m->areas_served.load() && m->marker_native_ready.load() &&
            Contains(rendered->navigation_bounds, s.target_position[0], s.target_position[1]) &&
            Contains(rendered->frame.world_bounds, s.target_position[0], s.target_position[1]);
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
    i("belmont.diag.render_decorative_approximation", rendered && rendered->frame.decorative_approximation);
    i("belmont.diag.locale_fixture_only", 1);
    t("belmont.map_area", ready ? Area.data() : ""); t("belmont.map_image", ready ? key.c_str() : "");
    t("belmont.area_name", ready ? rendered->area_name.c_str() : ""); t("belmont.selected_place", "");
    // Home width is a layout scale, not authored geometry. Source world bounds
    // and visibility remain native; host pan/zoom owns the independent view.
    constexpr double width = 96.0, height = width * 524.0 / 1124.0;
    const double x = ready ? s.target_position[0] : 0, y = ready ? s.target_position[1] : 0;
    f("belmont.player_x", x); f("belmont.player_y", y);
    f("belmont.view_x0", ready ? x - width / 2 : 0); f("belmont.view_y0", ready ? y - height / 2 : 0);
    f("belmont.view_x1", ready ? x + width / 2 : 0); f("belmont.view_y1", ready ? y + height / 2 : 0);
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
        m.allowed.store(s.supported); m.Queue(s); Publish(*h, s, &m);
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
    if (!k->starts_with(MapPrefix)) return m.assets.LoadImage(key, receiver, sink);
    try {
        // The sole frame is immutable and bounded to768². Keep the short sink
        // copy serialized with invalidation, never a RomFS/render operation.
        std::lock_guard lock{m.map_mutex};
        const auto frame = m.completed;
        if (!m.exposed || !frame || frame->generation != m.generation || *k != m.current_key)
            return EDEN_DSMOD_FALSE;
        sink(receiver, frame->frame.image.width, frame->frame.image.height,
             frame->frame.image.rgba.data(), frame->frame.image.rgba.size());
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
        if (!m.Bounds()) return EDEN_DSMOD_FALSE;
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
