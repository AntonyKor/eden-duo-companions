// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only state diagnostics. No map/marker availability until live contracts
// pass.
#include "belmont_reader.h"
#include "belmont_asset_callbacks.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include <memory>

namespace {
bool Valid(const EdenDsmodHostApi* host) {
    return dsmod_sdk::HostAbiMatches(host) && host->is_mapped && host->read_memory &&
           host->publish_i64 && host->publish_text;
}
belmont::Identity Identity(const EdenDsmodHostApi& host) {
    belmont::Identity id{host.title_id, host.main_base, host.main_size, {}};
    std::memcpy(id.build.data(), host.build_id, id.build.size());
    return id;
}
struct Module {
    belmont_assets::Callbacks assets;
    explicit Module(const EdenDsmodHostApi& host) : assets(host, true) {}
};
EdenDsmodBool Supports(const char* s) { return belmont::Supports(s); }
void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!Valid(host) || host->title_id != belmont::Title ||
            std::memcmp(host->build_id, belmont::Build.data(), belmont::Build.size()))
            return nullptr;
        return new Module{*host};
    } catch (...) {
        return nullptr;
    }
}
void Destroy(void* p) {
    try {
        delete static_cast<Module*>(p);
    } catch (...) {
    }
}
void Publish(const EdenDsmodHostApi& host, const belmont::Snapshot& s) {
    const auto i = [&](const char* key, std::int64_t value) {
        host.publish_i64(host.userdata, key, value);
    };
    i("belmont.build_supported", s.supported);
    i("belmont.state_coherent", s.coherent);
    i("belmont.manager_ready", s.manager_ready);
    i("belmont.entity_ready", s.entity_ready);
    i("belmont.application_loading", s.application_loading);
    i("belmont.room_loading", s.room_loading);
    i("belmont.account_initialized", s.account_initialized);
    i("belmont.initial_identity", s.initial_identity);
    i("belmont.loading_identity", s.loading_identity);
    i("belmont.paused", s.paused);
    i("belmont.diag.components_ready", s.components_ready);
    i("belmont.diag.explorer_candidate", s.explorer_candidate);
    i("belmont.diag.room_candidate", s.room_candidate);
    i("belmont.diag.position_candidate", s.position_candidate);
    i("belmont.diag.exploration_count", s.exploration_count);
    i("belmont.diag.exploration_ready", s.exploration_ready);
    std::int64_t revealed = 0;
    for (const auto& room : s.exploration)
        for (auto cell : room.cells)
            revealed += cell == 2 || cell == 3;
    i("belmont.diag.revealed_chunks", revealed);
    i("belmont.diag.room_state", s.room_state);
    host.publish_text(host.userdata, "belmont.diag.room_id", s.room_id.c_str());
    if (host.publish_f64) {
        host.publish_f64(host.userdata, "belmont.diag.target_x", s.target_position[0]);
        host.publish_f64(host.userdata, "belmont.diag.target_y", s.target_position[1]);
        host.publish_f64(host.userdata, "belmont.diag.target_z", s.target_position[2]);
    }
    // Always overwrite availability; never retain a previous gameplay snapshot.
    i("belmont.map_ready", 0);
    i("belmont.marker_ready", 0);
    host.publish_text(host.userdata, "belmont.state_reason", s.reason.c_str());
}
void Sample(void* p, const EdenDsmodHostApi* host) {
    if (!p || !Valid(host))
        return;
    try {
        auto read = [&](std::uint64_t at, void* dst, std::size_t size) {
            return at <= UINT64_MAX - size && host->is_mapped(host->userdata, at, size) &&
                   host->read_memory(host->userdata, at, dst, size);
        };
        belmont::Decoder decoder(read);
        Publish(*host, decoder.Sample(Identity(*host)));
    } catch (...) {
        try {
            Publish(*host, belmont::Snapshot{});
        } catch (...) {
        }
    }
}
void Tick(void*, const EdenDsmodHostApi*) {}
void Configure(void* p, const EdenDsmodHostExtensions* ext) {
    try {
        if (p) static_cast<Module*>(p)->assets.Configure(ext);
    } catch (...) {}
}
EdenDsmodBool LoadImage(void* p, const EdenDsmodHostApi*, const char* key,
                       void* receiver, EdenDsmodImageSink sink) {
    if (!p) return EDEN_DSMOD_FALSE;
    return static_cast<Module*>(p)->assets.LoadImage(key, receiver, sink);
}
EdenDsmodBool DecodeFont(void* p, const std::uint8_t* bytes, std::size_t size,
                        void* receiver, EdenDsmodFontSink sink) {
    if (!p) return EDEN_DSMOD_FALSE;
    return static_cast<Module*>(p)->assets.DecodeFont(bytes, size, receiver, sink);
}
const EdenDsmodModuleApi Api{EDEN_DSMOD_MODULE_ABI_VERSION,
                             sizeof(EdenDsmodModuleApi),
                             0,
                             EDEN_DSMOD_MODULE_ABI_HASH,
                             belmont::Title,
                             "Belmont state prototype",
                             EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
                             &Supports,
                             &Create,
                             &Destroy,
                             &Sample,
                             &Tick};
const EdenDsmodModuleExtensions Extensions{EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH, &Configure, nullptr, &LoadImage};
const EdenDsmodFontExtensions Fonts{EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions),
    EDEN_DSMOD_FONT_EXT_HASH, &DecodeFont};
} // namespace
DSMOD_SDK_EXPORT_MODULE(Api)
DSMOD_SDK_EXPORT_EXTENSIONS(Extensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(Fonts)
