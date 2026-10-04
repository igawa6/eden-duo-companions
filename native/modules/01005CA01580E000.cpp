// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// P5R live companion. Globals are resolved from the game's own accessor instructions;
// no heap scan, save-file fallback, or per-title emulator changes. Guest writes are limited to
// guarded direct state changes (USE ITEM / CHANGE EQUIPMENT / CHANGE PERSONA, p5r_direct_write.h):
// each write goes through the host's bounded write_memory after re-checking the expected bytes,
// and the persona stock rotation is applied as one write_batch when the host's write extension is
// present. Default on; "p5r_direct_writes": false or EDEN_DSMOD_P5R_DIRECT_WRITES=0 turns them off.
// This file: the ABI glue (module table, create / destroy / sample, the extension tables) and the
// exported C entry points. The Reader is declared in p5r_reader.h and defined in the
// p5r_reader_*.cpp area files.
#include "01005CA01580E000.h"
#include "p5r_reader.h"

namespace {
using namespace p5r_module;

EdenDsmodBool SupportsBuild(const char* id) {
    if (!id)
        return false;
    const std::string_view value{id};
    return value == BuildId;
}
void* Create(const EdenDsmodHostApi* host, const char* config) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId ||
            !std::equal(BuildBytes.begin(), BuildBytes.end(), host->build_id))
            return nullptr;
        auto reader = std::make_unique<Reader>(*host);
        const auto t0 = std::chrono::steady_clock::now();
        reader->text_async = config && std::strstr(config, "\"pages\"") != nullptr;
        // Direct state changes: default ON; the package ("p5r_direct_writes": false) or
        // EDEN_DSMOD_P5R_DIRECT_WRITES=0 selects the native-menu drives only.
        if (config && std::strstr(config, "\"p5r_direct_writes\":false"))
            reader->SetDirectWrites(false);
        if (const char* env = std::getenv("EDEN_DSMOD_P5R_DIRECT_WRITES"); env && env[0] == '0')
            reader->SetDirectWrites(false);
        if (reader->text_async)
            reader->prewarm_ids = p5r_prewarm::RecipeOrder(config);
        // EDEN_DSMOD_P5R_ALL_OUTPUTS=1 (desktop debugging): publish the whole contract anyway.
        const char* all_outputs = std::getenv("EDEN_DSMOD_P5R_ALL_OUTPUTS");
        if ((!all_outputs || std::strcmp(all_outputs, "1") != 0) &&
            reader->publish_filter.Configure(config) && host->log) {
            char line[160];
            std::snprintf(
                line, sizeof(line),
                "DSMod P5R outputs: %zu package name families; unnamed indexed outputs are "
                "not published (%.1f ms)",
                reader->publish_filter.Size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count());
            host->log(host->userdata, EDEN_DSMOD_LOG_INFO, line);
        }
        return reader.release();
    } catch (...) {
        return nullptr;
    }
}
void Destroy(void* p) {
    delete static_cast<Reader*>(p);
}
void Sample(void* p, const EdenDsmodHostApi* host) {
    try {
        if (p && host)
            static_cast<Reader*>(p)->Sample(*host);
    } catch (...) {
        if (host && host->publish_i64) {
            for (const char* key : {"live.ready",
                                    "date.ready",
                                    "live.money",
                                    "state.mode",
                                    "party.0.present",
                                    "party.1.present",
                                    "party.2.present",
                                    "party.3.present",
                                    "dialogue.ready",
                                    "dialogue.has_speaker",
                                    "controls.choice",
                                    "controls.message",
                                    "map.ready",
                                    "map.overlay.ready",
                                    "map.radar.ready",
                                    "map.enemy.count",
                                    "map.poi.count",
                                    "field.key",
                                    "controls.field",
                                    "controls.map_unavailable",
                                    "analysis.ready",
                                    "analyze.cycle",
                                    "controls.battle",
                                    "battle.active",
                                    "battle.ready",
                                    "enemy.count",
                                    "enemy.0.present",
                                    "enemy.1.present",
                                    "enemy.2.present",
                                    "enemy.3.present",
                                    "enemy.4.present",
                                    "bparty.ready",
                                    "bparty.0.present",
                                    "bparty.1.present",
                                    "bparty.2.present",
                                    "bparty.3.present",
                                    "battle.turn.side",
                                    "battle.target.side",
                                    "battle.phase",
                                    "social.ready",
                                    "confidant.ready",
                                    "confidant.count",
                                    "confidant.sel.ready",
                                    "calendar.ready",
                                    "request.ready",
                                    "request.count",
                                    "persona.ready",
                                    "persona.count",
                                    "item.ready",
                                    "item.tab.count",
                                    "pstat.ready",
                                    "equip.ready",
                                    "pdrv.pending",
                                    "pdrv.x",
                                    "pdrv.a",
                                    "pdrv.b",
                                    "pdrv.up",
                                    "pdrv.down",
                                    "pdrv.l",
                                    "pdrv.r",
                                    "ally.0.persona.present",
                                    "ally.1.persona.present",
                                    "ally.2.persona.present",
                                    "ally.3.persona.present"})
                host->publish_i64(host->userdata, key, 0);
        }
        if (host && host->publish_text)
            host->publish_text(host->userdata, "status", "Waiting for gameplay");
    }
}
void Tick(void*, const EdenDsmodHostApi*) {}
const EdenDsmodModuleApi Api{EDEN_DSMOD_MODULE_ABI_VERSION,
                             sizeof(EdenDsmodModuleApi),
                             0,
                             EDEN_DSMOD_MODULE_ABI_HASH,
                             TitleId,
                             "Persona 5 Royal live companion",
                             0,
                             SupportsBuild,
                             Create,
                             Destroy,
                             Sample,
                             Tick};

void ConfigureExtensions(void*, const EdenDsmodHostExtensions*) {}
// Write extension: persona stock rotations are stored as one unit with guest threads stalled.
void ConfigureWrite(void* p, const EdenDsmodHostWriteApi* h) {
    if (!p || !h || h->version != EDEN_DSMOD_WRITE_EXT_VERSION ||
        h->struct_size != sizeof(EdenDsmodHostWriteApi) ||
        h->abi_hash != EDEN_DSMOD_WRITE_EXT_HASH || !h->write_batch)
        return;
    static_cast<Reader*>(p)->SetWriteApi(*h);
}
// Serialized with sample/tick by the host; records UI selections, starts native-menu button
// drives or queues one direct-write request (applied by the next sample, p5r_direct_write.h).
// The handler chain and its order: Reader::DispatchAction (p5r_reader_actions.cpp).
EdenDsmodBool OnAction(void* p, const char* action, int64_t argument) {
    try {
        if (!p || !action)
            return false;
        return static_cast<Reader*>(p)->DispatchAction(action, argument);
    } catch (...) {
        return false;
    }
}
const EdenDsmodModuleExtensions Extensions{EDEN_DSMOD_EXT_VERSION,
                                           sizeof(EdenDsmodModuleExtensions),
                                           EDEN_DSMOD_EXT_HASH,
                                           ConfigureExtensions,
                                           OnAction,
                                           LoadImage};
const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,
                                             sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, DecodeFont};
const EdenDsmodModuleWriteExtensions WriteExtensions{EDEN_DSMOD_WRITE_EXT_VERSION,
                                                     sizeof(EdenDsmodModuleWriteExtensions),
                                                     EDEN_DSMOD_WRITE_EXT_HASH, ConfigureWrite};
} // namespace
DSMOD_SDK_EXPORT_MODULE(Api)
DSMOD_SDK_EXPORT_EXTENSIONS(Extensions)
DSMOD_SDK_EXPORT_WRITE_EXTENSIONS(WriteExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
