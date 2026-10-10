// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes module: ABI glue.
//   chained_echoes_unity.{h,cpp}  Unity bundle / texture / sprite / TMP font reader (romfs)
//   ce_draw.{h,cpp}               the accepted mockups' drawing kit (3x native, CE font)
//   ce_pages.{h,cpp}              battle B1 and the secondary-state pictures
//   ce_reader.{h,cpp}             live battle readers and the top-screen HUD switches
//   ce_mech_reader / ce_page_skyarmor  Sky Armor battle reader and picture (sky:<hash>)
//   ce_field_reader.{h,cpp}       exploration readers (Field Home slice); ce_page_field.{h,cpp}
//                                 the Field Home / Board pictures
//   ce_skill_reader.{h,cpp}       Skills / progression reader + plan state; ce_page_skills.{h,cpp}
//                                 the Skills page
//   ce_crystal_reader.{h,cpp}     crystal / equipment reader + planner state; ce_page_crystals.{h,cpp}
//                                 the Crystals page (planner, transfer checklist, smith / pause mirror)
// Supported build: 1.41 (update v1310720), main 17B6A110CC529FBE702B09F1BA1DA61D. The base game
// without the update (main 3D730AA2C1DADC4C) loads only to show the "unsupported version"
// picture: no memory reads, no writes. Every other build is refused by the manifest's build ids
// (the package then shows the host-font notice).
//
// Image keys ("module:ce:..."): title, hold, unsupported:base, settings:<0-31> (bit 4 = gear badge,
// bit 3 = Ultra bar, bit 2 = turn order, bit 1 = party panel, bit 0 = Overdrive gauge hidden), b1:<hash> (a battle model the reader registered),
// sprite/<alias>/<name>[@<scale>], font/CE (the host font atlas). Actions: hud_ctb / hud_party /
// hud_od / hud_ultra / hud_gear (argument 1 = hide), sel_chip (argument = chip slot), sheet. Field Home: fh:<hash> (a field
// model), fmark:<0-4> (the live player marker by facing); actions fpage (0 home, 1 crystals,
// 2 skills, 3 board), btile (board cell), bsec (0 previous / 1 next section). Skills page: sk:<hash>,
// skrow:<a|b>:<id> (a draggable spare passive row), skglow:<0|1>, skshadow:<a|b>, sksel:<a|b>; actions sk_member,
// sk_next / sk_prev, sk_plan<slot> (argument passive id + 1000), sk_unplan<slot>. Crystals page: cr:<hash>,
// crc:<n> (a grid cell, the drag widgets), crshadow; actions cr_* (ce_crystal_reader.cpp).

#include "ce_crystal_reader.h"
#include "ce_draw.h"
#include "ce_field_reader.h"
#include "ce_page_field.h"
#include "ce_page_skyarmor.h"
#include "ce_pages.h"
#include "ce_parse.h"
#include "ce_reader.h"
#include "ce_skill_reader.h"
#include "chained_echoes_unity.h"
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x0100C510166F0000);
constexpr u8 BuildId[8]{0x17, 0xB6, 0xA1, 0x10, 0xCC, 0x52, 0x9F, 0xBE};     // 1.41
constexpr u8 BaseBuildId[8]{0x3D, 0x73, 0x0A, 0xA2, 0xC1, 0xDA, 0xDC, 0x4C}; // 1.0 base, no update

bool ParseHex8(const char* s, u8 (&out)[8]) {
    if (!s || std::strlen(s) < 16)
        return false;
    for (int i = 0; i < 16; ++i) {
        const int c = std::toupper(static_cast<unsigned char>(s[i]));
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0)
            return false;
        out[i / 2] = static_cast<u8>((i % 2 ? out[i / 2] << 4 : 0) | v);
    }
    return true;
}

EdenDsmodBool SupportsBuild(const char* build_id) {
    u8 id[8]{};
    if (!ParseHex8(build_id, id))
        return EDEN_DSMOD_FALSE;
    return std::memcmp(id, BuildId, 8) == 0 || std::memcmp(id, BaseBuildId, 8) == 0
               ? EDEN_DSMOD_TRUE
               : EDEN_DSMOD_FALSE;
}

struct Module {
    EdenDsmodHostApi host{}; ///< copy: read_romfs + userdata stay valid for the module's life
    std::unique_ptr<ce_draw::Art> art;
    std::unique_ptr<ce_reader::Reader> reader;
    std::unique_ptr<ce_field::Reader> field;
    std::unique_ptr<ce_skills::Reader> skills;
    std::unique_ptr<ce_crystal::Reader> crystals;
    bool unsupported{};
};

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->is_mapped ||
            !host->read_memory || !host->read_romfs)
            return nullptr;
        const bool supported = std::memcmp(host->build_id, BuildId, 8) == 0;
        const bool base = std::memcmp(host->build_id, BaseBuildId, 8) == 0;
        if (!supported && !base)
            return nullptr;
        auto m = std::make_unique<Module>();
        m->host = *host;
        m->unsupported = !supported;
        m->art = std::make_unique<ce_draw::Art>(ce_unity::MakeHostRangeReader(&m->host));
        m->reader = std::make_unique<ce_reader::Reader>(m->unsupported);
        if (!m->unsupported) {
            m->field = std::make_unique<ce_field::Reader>(*m->art);
            m->reader->idle_key = [f = m->field.get()] { return f->IdleKey(); };
            m->skills = std::make_unique<ce_skills::Reader>(*m->art);
            m->field->skills_key = [k = m->skills.get()] { return k->Key(); };
            m->field->skills_mirror = [k = m->skills.get()] { return k->Mirror(); };
            m->field->skills_alarm = [k = m->skills.get()] { return k->Alarm(); };
            m->crystals = std::make_unique<ce_crystal::Reader>(*m->art);
            m->field->crystals_key = [c = m->crystals.get()] { return c->Key(); };
            m->field->crystals_mirror = [c = m->crystals.get()] { return c->Mirror(); };
            m->reader->plan_ticks = [k = m->skills.get(), c = m->crystals.get()] {
                return k->TickSeq() + c->TickSeq();
            };
            m->crystals->go_home = [f = m->field.get()] { f->Action("fpage", 0); };
        }
        return m.release();
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* p) {
    try {
        auto* m = static_cast<Module*>(p);
        if (m)
            m->reader.reset(); // restores the game's HUD first
        delete m;
    } catch (...) {
    }
}

void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (p && host) {
            auto* m = static_cast<Module*>(p);
            if (m->field)
                m->field->Sample(*host, m->reader->State() == 2);
            if (m->skills)
                m->skills->Sample(*host, m->reader->State() == 2);
            if (m->crystals)
                m->crystals->Sample(*host, m->reader->State() == 2);
            m->reader->Sample(*host);
            if (m->field)
                m->field->PublishFor(*host, m->reader->Shown(), m->reader->LoadingShown());
            if (m->skills)
                m->skills->PublishFor(*host, m->reader->Shown(), m->reader->LoadingShown());
            if (m->crystals)
                m->crystals->PublishFor(*host, m->reader->Shown(), m->reader->LoadingShown());
        }
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "Chained Echoes DSMod sample failed");
    }
}

// Nothing to do in either, but the host requires both: GameModule::Load rejects a module whose
// tick is null, and the base extensions' configure is their required member.
void TickCallback(void*, const EdenDsmodHostApi*) {}
void ConfigureCallback(void*, const EdenDsmodHostExtensions*) {}

EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t argument) {
    try {
        if (!p || !action)
            return EDEN_DSMOD_FALSE;
        auto* m = static_cast<Module*>(p);
        if (m->unsupported) // nothing to hide or select on an unsupported build
            return std::string_view{action}.starts_with("hud_") ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        if (std::string_view{action}.starts_with("sk_")) // Skills page (refused while mirroring)
            return m->skills && m->skills->Action(action, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        if (std::string_view{action}.starts_with("cr_")) // Crystals page (editing refused while mirroring)
            return m->crystals && m->crystals->Action(action, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        if (m->field && m->field->Action(action, argument))
            return EDEN_DSMOD_TRUE;
        return m->reader->Action(action, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

std::optional<ce_draw::Image> Compose(Module& m, std::string_view key) {
    if (!key.starts_with("module:ce:"))
        return std::nullopt;
    key.remove_prefix(10);
    auto& art = *m.art;
    if (key == "font/CE")
        return art.HostFontAtlas();
    if (key == "title")
        return ce_pages::ComposeTitle(art);
    if (key == "hold")
        return ce_pages::ComposeHolding(art);
    if (key == "unsupported:base")
        return ce_pages::ComposeUnsupported(art, "No update");
    if (key.starts_with("settings:") && (key.size() == 10 || key.size() == 11)) {
        int v = 0;
        for (const char ch : key.substr(9))
            v = ch >= '0' && ch <= '9' ? v * 10 + (ch - '0') : 99;
        if (v > 31)
            return std::nullopt;
        return ce_pages::ComposeSettings(art, (v & 4) != 0, (v & 2) != 0, (v & 1) != 0,
                                         (v & 8) != 0, (v & 16) != 0);
    }
    if (key.starts_with("b1:")) {
        if (m.unsupported)
            return std::nullopt;
        const auto view = m.reader->Model("module:ce:" + std::string{key});
        if (!view)
            return std::nullopt;
        return ce_pages::ComposeBattle(art, *view);
    }
    if (key.starts_with("fm:")) { // native Formation mirror
        if (m.unsupported)
            return std::nullopt;
        const auto view = m.reader->FormationModel("module:ce:" + std::string{key});
        if (!view)
            return std::nullopt;
        return ce_pages::ComposeFormation(art, *view);
    }
    if (key.starts_with("strip:")) { // turn strip slide overlay
        const auto st = m.reader->StripModel("module:ce:" + std::string{key});
        if (!st)
            return std::nullopt;
        return ce_pages::ComposeStrip(art, st->first, st->second);
    }
    if (key.starts_with("sky:")) { // Sky Armor battle (ce_page_skyarmor)
        if (m.unsupported)
            return std::nullopt;
        const auto view = m.reader->SkyModel("module:ce:" + std::string{key});
        if (!view)
            return std::nullopt;
        return ce_sky::ComposeSkyArmor(art, *view);
    }
    if (key.starts_with("fh:")) {
        if (!m.field)
            return std::nullopt;
        const auto view = m.field->Model("module:ce:" + std::string{key});
        if (!view)
            return std::nullopt;
        return ce_page_field::ComposeField(art, *view);
    }
    if (key.starts_with("sk:")) { // Skills page (ce_page_skills)
        if (!m.skills)
            return std::nullopt;
        const auto view = m.skills->Model("module:ce:" + std::string{key});
        if (!view)
            return std::nullopt;
        return ce_page_skills::ComposeSkills(art, *view);
    }
    if (key.starts_with("skrow:"))
        return m.skills ? m.skills->Row(key.substr(6)) : std::nullopt;
    if (key == "skglow:0" || key == "skglow:1")
        return ce_page_skills::ComposeGlow(art, key[7] - '0');
    if (key == "sksel:a" || key == "sksel:b")
        return ce_page_skills::ComposeRowSelected(art, key[6] == 'b');
    if (key == "skshadow:a" || key == "skshadow:b")
        return ce_page_skills::ComposeRowShadow(key[9] == 'b');
    if (key.starts_with("cr:")) { // Crystals page (ce_page_crystals)
        if (!m.crystals)
            return std::nullopt;
        const auto view = m.crystals->Model("module:ce:" + std::string{key});
        if (!view)
            return std::nullopt;
        return ce_page_crystals::ComposeCrystals(art, *view);
    }
    if (key.starts_with("crc:"))
        return m.crystals ? m.crystals->CellImage(key.substr(4)) : std::nullopt;
    if (key == "crshadow")
        return ce_page_crystals::ComposeCellShadow();
    if (key.starts_with("fmark:") && key.size() == 7 && key[6] >= '0' && key[6] <= '4')
        return ce_page_field::ComposeMarker(art, key[6] - '0');
    if (key.starts_with("sprite/")) {
        key.remove_prefix(7);
        const auto slash = key.find('/');
        if (slash == std::string_view::npos)
            return std::nullopt;
        std::string_view name = key.substr(slash + 1);
        int scale = 1;
        if (const auto at = name.rfind('@'); at != std::string_view::npos) {
            const auto parsed = ce_parse::Index(name.substr(at + 1));
            if (!parsed)
                return std::nullopt;
            scale = *parsed;
            name = name.substr(0, at);
            if (scale < 1 || scale > 8)
                return std::nullopt;
        }
        const auto img = art.Sprite(key.substr(0, slash), name);
        if (!img)
            return std::nullopt;
        return ce_draw::Scale(*img, scale);
    }
    return std::nullopt;
}

EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi*, const char* key, void* receiver,
                                EdenDsmodImageSink sink) {
    try {
        if (!p || !key || !sink)
            return EDEN_DSMOD_FALSE;
        auto& m = *static_cast<Module*>(p);
        const auto img = Compose(m, key);
        if (!img || img->rgba.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, img->width, img->height, img->rgba.data(), img->rgba.size());
        m.reader->MarkDelivered(key);
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t*, size_t, void* receiver,
                                 EdenDsmodFontSink sink) {
    try {
        if (!p || !sink)
            return EDEN_DSMOD_FALSE;
        std::uint32_t first{}, line{};
        std::vector<EdenDsmodFontGlyph> glyphs;
        if (!static_cast<Module*>(p)->art->HostFont(first, glyphs, line) || glyphs.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, line, first, glyphs.data(), static_cast<std::uint32_t>(glyphs.size()));
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

void ConfigureWriteCallback(void* p, const EdenDsmodHostWriteApi* api) {
    try {
        if (p)
            static_cast<Module*>(p)->reader->ConfigureWrites(api);
    } catch (...) {
    }
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Chained Echoes DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ |
                                       EDEN_DSMOD_CAP_WRITE_MEMORY,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    &ConfigureCallback,     &ActionCallback,                   &LoadImageCallback};
const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,
                                             sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, &DecodeFontCallback};
const EdenDsmodModuleWriteExtensions WriteExtensions{
    EDEN_DSMOD_WRITE_EXT_VERSION, sizeof(EdenDsmodModuleWriteExtensions), EDEN_DSMOD_WRITE_EXT_HASH,
    &ConfigureWriteCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
DSMOD_SDK_EXPORT_WRITE_EXTENSIONS(WriteExtensions)
