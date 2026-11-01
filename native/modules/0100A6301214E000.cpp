// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem Engage module: ABI glue.
//   fee_reader.{h,cpp}  game-memory reader (scene flags, War page battle data), offsets from
//                       the pinned constants (fee_pins.inc); no direct guest writes
//   fee_map.{h,cpp}     content-keyed war-map images (terrain + game-computed danger bitmap +
//                       unit markers), served as module:fee:map/war/<hash>
//   fee_hud.{h,cpp}     native top-screen HUD hide/restore through the load plan's gates and
//                       call stub (tools/fire-emblem-engage/gen_load_plan.py), mailbox only
//   fee_font.{h,cpp}    the game's own FOT-Humming SDF menu font, decoded from the player's
//                       romfs (decode_font: package request -> glyph run; fee:font/<face> ->
//                       the matching atlas image)
// Supported build: Ver. 2.0.0 only (8C08B9719E085F91847B5E0F935D9488 + zeros).
// Reader actions: ui_open, war_mode/danger/select/pick/card/charp, war_pan/pinch/view;
// hub/roster actions are handled by fee_hub. hud_show re-shows native HUD elements.
// Map selection is inspect-only and never moves the native cursor.
// Read-only game calls (RouteCalls): through the HUD call stub, one per frame after the HUD's
// own: Unit.GetSimpleHit for the selected unit (fee_reader CalcWanted) and the Somniel map's
// GameObject/Component.get_transform, Transform.get_position, HubAccess.get_AccessCount.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "fee_font.h"
#include "fee_hub.h"
#include "fee_hud.h"
#include "fee_reader.h"
#include "fee_pins.inc"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <atomic>
#include <cstdint>
#include <vector>

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x0100A6301214E000);

EdenDsmodBool SupportsBuild(const char* build_id) {
    return FeeReader::SupportsBuildHex(build_id) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

/// One module instance: the game-memory reader plus the romfs text/face library it owns.
struct Module {
    Module(const EdenDsmodHostApi& api, const char* config)
        : reader{api, config}, fonts{fee::MakeRomfsReader(api), std::string{fee::FeeAssetRoot}},
          hub{api}, host_api{api},
          hub_unity{fee::MakeRomfsReader(api), {}, std::string{fee::FeeAssetRoot}, 3} {
        hub.SetUnity(&hub_unity);
        hub.SetText([this](const std::string& key) { return reader.Assets().Text(key); });
        hub.SetUnitExtra([this](std::uint64_t at, fee_hub::RosterRow& row) {
            const auto b = reader.Brief(at);
            if (!b.ok)
                return;
            row.mhp = b.mhp;
            row.eng = b.eng;
            row.weapon = b.weapon;
            row.face = b.face;
            row.rface = b.rface;
            row.ring = b.ring;
            row.engimg = b.engimg;
            row.dot = b.dot;
            row.pid8 = b.pid;
        });
        hub.SetSpriteRender([this](std::string_view key, std::vector<std::uint8_t>& rgba,
                                   std::uint32_t& w, std::uint32_t& h) {
            return reader.Assets().SpritesLib().Render(key, rgba, w, h);
        });
    }
    ~Module() {
        hud.Shutdown(); // give the game back every HUD element this module hid
    }

    EdenDsmodBool LoadImage(const EdenDsmodHostApi* /*image_host*/, const char* key,
                            void* receiver, EdenDsmodImageSink sink) {
        if (!key || !sink)
            return EDEN_DSMOD_FALSE;
        std::string_view k{key};
        if (k.starts_with("module:"))
            k.remove_prefix(7);
        if (k == "fee:font/regular" || k == "fee:font/bold") {
            const auto image = fonts.Atlas(k.back() == 'd' ? fee_font::Face::Bold
                                                            : fee_font::Face::Regular);
            if (!image || !image->width || !image->height || image->rgba.empty())
                return EDEN_DSMOD_FALSE;
            sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        std::vector<std::uint8_t> rgba;
        std::uint32_t w = 0, h = 0;
        if (k.starts_with("fee:face/")) {
            // The player's own facethumb sprite: <fid> from PersonData / GodData.FaceIconName.
            const auto image = reader.Assets().Face(k.substr(9));
            if (!image || !image->width || !image->height || image->rgba.empty())
                return EDEN_DSMOD_FALSE;
            sink(receiver, image->width, image->height, image->rgba.data(),
                 image->rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (k.starts_with("fee:faceround/")) {
            // D2 07 commander portrait: the facethumb's centre square in a round red ring.
            std::string_view rest = k.substr(14);
            // optional "/b": the player's blue ring (D2 06 attacker plate) instead of red
            const bool blue = rest.ends_with("/b");
            if (blue)
                rest.remove_suffix(2);
            const auto slash = rest.rfind('/');
            if (slash == std::string_view::npos)
                return EDEN_DSMOD_FALSE;
            const int d = std::clamp(std::atoi(std::string{rest.substr(slash + 1)}.c_str()), 16, 256);
            const auto image = reader.Assets().Face(rest.substr(0, slash));
            if (!image || !image->width || !image->height || image->rgba.empty())
                return EDEN_DSMOD_FALSE;
            const std::uint32_t side = std::min(image->width, image->height);
            const std::uint32_t x0 = (image->width - side) / 2, y0 = (image->height - side) / 2;
            rgba.assign(std::size_t(d) * d * 4, 0);
            const float r = d / 2.0f, ring = std::max(3.0f, d / 18.0f);
            for (int y = 0; y < d; ++y)
                for (int x = 0; x < d; ++x) {
                    const float dx = x + 0.5f - r, dy = y + 0.5f - r;
                    const float dist = std::sqrt(dx * dx + dy * dy);
                    std::uint8_t* o = &rgba[(std::size_t(y) * d + x) * 4];
                    if (dist > r)
                        continue;
                    if (dist > r - ring) { // faction ring (D2 #D23A3A) with a dark edge
                        const bool edge = dist > r - 1.2f;
                        if (blue)
                            o[0] = edge ? 0x10 : 0x3A, o[1] = edge ? 0x20 : 0x8C, o[2] = edge ? 0x40 : 0xE6;
                        else
                            o[0] = edge ? 0x40 : 0xD2, o[1] = edge ? 0x10 : 0x3A, o[2] = edge ? 0x14 : 0x3A;
                        o[3] = 255;
                        continue;
                    }
                    const std::uint32_t sx = x0 + std::uint32_t(x) * side / std::uint32_t(d);
                    const std::uint32_t sy = y0 + std::uint32_t(y) * side / std::uint32_t(d);
                    const std::uint8_t* p = &image->rgba[(std::size_t(sy) * image->width + sx) * 4];
                    // under the portrait: the ring's dark fill (facethumbs have transparent edges)
                    const float a = p[3] / 255.0f;
                    o[0] = std::uint8_t(p[0] * a + 0x2A * (1 - a));
                    o[1] = std::uint8_t(p[1] * a + 0x10 * (1 - a));
                    o[2] = std::uint8_t(p[2] * a + 0x18 * (1 - a));
                    o[3] = 255;
                }
            sink(receiver, std::uint32_t(d), std::uint32_t(d), rgba.data(), rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (k.starts_with("fee:hub/")) {
            if (!hub.Render(k, rgba, w, h) || !w || !h || rgba.size() != std::size_t(w) * h * 4)
                return EDEN_DSMOD_FALSE;
            sink(receiver, w, h, rgba.data(), rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (k.starts_with("fee:sprite/")) {
            // The player's own UI art (fee_sprites; SPRITE-KEYS contract).
            if (!reader.Assets().SpritesLib().Render(k.substr(11), rgba, w, h) || !w || !h ||
                rgba.size() != static_cast<std::size_t>(w) * h * 4)
                return EDEN_DSMOD_FALSE;
            sink(receiver, w, h, rgba.data(), rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (k.starts_with("fee:mapcursor/")) {
            // The native-style map cursor marker for one tile size (fee_map::RenderCursor).
            if (!fee_map::RenderCursor(std::atoi(std::string{k.substr(14)}.c_str()), rgba, w, h))
                return EDEN_DSMOD_FALSE;
            sink(receiver, w, h, rgba.data(), rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (!k.starts_with("fee:"))
            return EDEN_DSMOD_FALSE;
        if (!reader.Maps().Render(k, rgba, w, h) || !w || !h ||
            rgba.size() != static_cast<std::size_t>(w) * h * 4)
            return EDEN_DSMOD_FALSE;
        sink(receiver, w, h, rgba.data(), rgba.size());
        return EDEN_DSMOD_TRUE;
    }

    void Configure(const EdenDsmodHostExtensions* ext) {
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->abi_hash == EDEN_DSMOD_EXT_HASH &&
            ext->struct_size >= sizeof(*ext) && ext->decode_astc) {
            reader.Assets().SetAstcDecoder(lp_unity::MakeHostAstcDecoder(ext->userdata, ext->decode_astc));
            fonts.SetAstcDecoder(lp_unity::MakeHostAstcDecoder(ext->userdata, ext->decode_astc));
            hub_unity.SetAstcDecoder(lp_unity::MakeHostAstcDecoder(ext->userdata, ext->decode_astc));
        }
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->abi_hash == EDEN_DSMOD_EXT_HASH &&
            ext->struct_size >= sizeof(*ext))
            hud.SetIo(fee_hud::MakeIo(host_api, ext));
    }

    /// Native top-screen HUD hide (fee_hud.h): hidden by default, settings re-show.
    void TickHud(const EdenDsmodHostApi& host) {
        if (!hud.Ready())
            return;
        const bool aux = !host.get_i64 || host.get_i64(host.userdata, "__aux_present", 1) != 0;
        auto show = fee_hud::ReadSettings(host);
        for (int e = 0; e < fee_hud::ElementCount; ++e) // runtimes without "@flag:" reads
            show[e] = show[e] || ((hud_show_mask >> e) & 1);
        RouteCalls();
        hud.Tick(hud.Resolve(), show, aux);
    }

    /// Read-only game calls through the HUD call stub (one per frame, HUD first): the selected
    /// unit's combat stats (fee_reader CalcWanted), then the Somniel map positions (fee_hub).
    void RouteCalls() {
        if (auto r = hud.TakeResult()) {
            // One line each in the host log (logcat on Android): whether the guest call stub
            // serves calls on this device at all.
            if (host_api.log && !(logged & (r->lost ? 2 : 1))) {
                logged |= r->lost ? 2 : 1;
                host_api.log(host_api.userdata, r->lost ? EDEN_DSMOD_LOG_ERROR : EDEN_DSMOD_LOG_INFO,
                             r->lost ? "FE Engage DSMod: a game call timed out (call stub not serving)"
                                     : "FE Engage DSMod: game call stub serving calls");
            }
            if (r->lost) {
                if (r->tag >= fee_hub::CallTagBase)
                    hub.OnCallLost(); // lets the hub's gated periodic calls ask again
            } else if (r->tag == TagCalc)
                reader.CalcDone(calc_unit);
            else if (r->tag == TagForecast)
                reader.ForecastDone(fc_pair[0], fc_pair[1], r->scratch.data(), r->scratch.size());
            else if (r->tag == TagSortie)
                reader.SortieLimitDone(std::int32_t(r->x0 & 0xFFFFFFFFu));
            else if (r->tag >= fee_hub::CallTagBase)
                hub.OnCall(r->tag, r->x0, r->scratch.data(), r->scratch.size());
        }
        if (hud.CallBusy())
            return;
        if (const std::uint64_t u = reader.CalcWanted()) {
            calc_unit = u;
            hud.Submit({fee_pins::UnitGetSimpleHit, {u, 0, 0, 0}, TagCalc});
            return;
        }
        if (const std::uint64_t u = reader.RosterCalcWanted(hub.SelectedUnit(), hub.UnitCallsSafe())) {
            calc_unit = u; // Roster unit page in the Somniel: the same game calculation
            hud.Submit({fee_pins::UnitGetSimpleHit, {u, 0, 0, 0}, TagCalc});
            return;
        }
        if (hud.ScratchAddr() && reader.ForecastWanted(fc_pair[0], fc_pair[1])) {
            hud.Submit({fee_pins::ForecastHelper, {fc_pair[0], fc_pair[1], hud.ScratchAddr(),
                                                    reader.ForecastItem()},
                        TagForecast});
            return;
        }
        if (reader.SortieLimitWanted()) {
            hud.Submit({fee_pins::SortieUtilGetSortieUnitMax, {0, 0, 0, 0}, TagSortie});
            return;
        }
        if (auto c = hub.NextCall(hud.ScratchAddr()))
            hud.Submit({c->fn, c->args, c->tag});
    }
    static constexpr std::uint32_t TagCalc = 1, TagSortie = 3, TagForecast = 4;
    std::uint64_t calc_unit = 0;
    unsigned logged = 0; // 1 = first served call logged, 2 = first lost call logged
    std::uint64_t fc_pair[2]{}; // offense, defense of the forecast in flight

    EdenDsmodBool DecodeFont(const std::uint8_t* bytes, std::size_t size, void* receiver,
                            EdenDsmodFontSink sink) {
        return fonts.DecodeFont(bytes, size, receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    }

    FeeReader::Reader reader;
    fee_font::FontCache fonts;
    fee_hub::Hub hub; // Somniel/world-map/roster reader (fee_hub.h); isolated from `reader`
    EdenDsmodHostApi host_api; // function pointers + userdata for the HUD mailbox Io
    lp_unity::Reader hub_unity; // Somniel map art (fee_hub Render)
    fee_hud::Hider hud;
    std::int64_t hud_show_mask = 0; // module action hud_show <mask>
};

void* Create(const EdenDsmodHostApi* host, const char* config) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->is_mapped ||
            !host->read_memory)
            return nullptr;
        // create() re-checks all 32 build-id bytes (supports_build saw the hex form).
        if (!FeeReader::SupportsBuildId(host->build_id))
            return nullptr;
        return new Module{*host, config};
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
void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (p && host) {
            static_cast<Module*>(p)->reader.Sample(*host);
            static_cast<Module*>(p)->hub.Sample(*host);
            // Roster unit page (D2 10b): the Char Details readers for the roster selection.
            if (const std::uint64_t at = static_cast<Module*>(p)->hub.SelectedUnit())
                static_cast<Module*>(p)->reader.PublishRosterUnit(*host, at);
        }
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "FE Engage DSMod sample failed");
    }
}
void TickCallback(void* p, const EdenDsmodHostApi* host) {
    try {
        if (p && host)
            static_cast<Module*>(p)->TickHud(*host);
    } catch (...) {
    }
}
void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    try {
        if (p && ext)
            static_cast<Module*>(p)->Configure(ext);
    } catch (...) {
    }
}
EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t argument) {
    try {
        if (!p)
            return EDEN_DSMOD_FALSE;
        auto* m = static_cast<Module*>(p);
        if (m->hub.OnAction(action, argument))
            return EDEN_DSMOD_TRUE;
        if (action && std::string_view{action} == "hud_show") { // bit e re-shows element e
            m->hud_show_mask = argument;
            return EDEN_DSMOD_TRUE;
        }
        return m->reader.OnAction(action, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}
EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi* host, const char* key,
                                void* receiver, EdenDsmodImageSink sink) {
    try {
        if (p)
            return static_cast<Module*>(p)->LoadImage(host, key, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}
EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t* bytes, std::size_t size,
                                 void* receiver, EdenDsmodFontSink sink) {
    try {
        if (p)
            return static_cast<Module*>(p)->DecodeFont(bytes, size, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Fire Emblem Engage DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{EDEN_DSMOD_EXT_VERSION,
                                                 sizeof(EdenDsmodModuleExtensions),
                                                 EDEN_DSMOD_EXT_HASH,
                                                 &ConfigureCallback,
                                                 &ActionCallback,
                                                 &LoadImageCallback};
const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,
                                             sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, &DecodeFontCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
