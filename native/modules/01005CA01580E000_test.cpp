// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Public-ABI tests over synthetic guest memory. The only instruction words taken from the game
// build are the accessor words the readers match or decode (the same words are in the readers'
// match lists); every word a reader does not check is NOP, fingerprinted code ranges are filler
// forged to the readers' FNV-1a hashes, and all data is made up. This does NOT verify real
// gameplay behaviour.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "p5r_data2_reader.h"
#include "p5r_romfs_local.h"
#include "p5r_social_menu_reader.h"
using U = uint64_t;
// Game-text tables (names, arcana, music titles) come from the game's romfs at run time
// (p5r_game_text.h). The fake host serves the local CPKs the way host.read_romfs serves the
// running game's: P5R_ROMFS, else ./romfs (<root>/CPK/ALL_USEU.CPK, PATCH1.CPK).
std::string RomfsRoot() {
    if (const char* v = std::getenv("P5R_ROMFS"))
        return v;
    return "romfs";
}
size_t LocalRomfs(void*, const char* path, uint64_t offset, void* out, size_t size) {
    static const p5r_assets::ReadFn local = p5r_assets::LocalDirReader(RomfsRoot());
    std::string_view p{path ? path : ""};
    if (p.starts_with("file:"))
        return 0; // no package files in these tests
    if (p.starts_with("romfs:"))
        p.remove_prefix(6);
    return local(std::string{p}.c_str(), offset, out, size);
}
// Pre-0.8 tests read every list at once: open the diagnostic "all pages" view (ui_open 99).
void* OpenAll(void* r) {
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    if (r && ext && ext->on_action)
        ext->on_action(r, "ui_open", 99);
    return r;
}
void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL " << what << '\n';
        std::exit(1);
    }
}
struct Host {
    EdenDsmodHostApi api{};
    std::map<U, uint8_t> ram;
    std::map<std::string, int64_t> nums;
    std::map<std::string, double> floats;
    std::map<std::string, std::string> texts;
    U base = 0x80400000, units = 0x226ee60, money = 0x22725cc, date = 0x22162c8, scene = 0x22c9160;
    U task = 0x90000000, ctx = 0x90000100, dateptr = 0x90000200;
    U fieldtask = 0x90001000, fieldctx = 0x90002000;
    unsigned writes = 0, begins = 0, ends = 0, party_reads = 0;
    bool tear = false;
    U tear_address{};
    unsigned targeted_reads{}, registry_reads{};
    bool tear_registry{};
    U social_tear{};
    unsigned social_tear_reads{};
    template <class T>
    void put(U at, T value) {
        auto* p = reinterpret_cast<uint8_t*>(&value);
        for (size_t i = 0; i < sizeof(T); ++i)
            ram[at + i] = p[i];
    }
    void code(U pc, std::initializer_list<uint32_t> words) {
        for (auto w : words) {
            put(base + pc, w);
            pc += 4;
        }
    }
    static EdenDsmodBool mapped(void* p, U at, U n) {
        auto& h = *static_cast<Host*>(p);
        for (U i = 0; i < n; ++i)
            if (!h.ram.count(at + i))
                return 0;
        return 1;
    }
    static EdenDsmodBool read(void* p, U at, void* out, size_t n) {
        auto& h = *static_cast<Host*>(p);
        if (!mapped(p, at, n))
            return 0;
        for (size_t i = 0; i < n; ++i)
            static_cast<uint8_t*>(out)[i] = h.ram.at(at + i);
        if (at == h.base + h.units + 0x1ce0 && ++h.party_reads == 2 && h.tear)
            static_cast<uint8_t*>(out)[0] = 9;
        if (at == h.tear_address && n == 0x1c && ++h.targeted_reads == 2)
            static_cast<uint8_t*>(out)[8] ^= 1;
        if (at == h.social_tear && ++h.social_tear_reads == 2)
            static_cast<uint8_t*>(out)[n - 1] ^= 1;
        if (at == h.base + 0x2209f50 && h.tear_registry && ++h.registry_reads == 3)
            static_cast<uint8_t*>(out)[0] ^= 8;
        return 1;
    }
    Host() {
        api.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
        api.struct_size = sizeof(api);
        api.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
        api.title_id = 0x01005ca01580e000;
        uint8_t bid[] = {0xd4, 0xb1, 0x50, 0xb2, 0x9a, 0x93, 0x1c, 0xd3, 0x81, 0xe2, 0xa3,
                         0x03, 0x18, 0xfc, 0x29, 0x9f, 0x2b, 0x0e, 0xe3, 0x6d, 0,    0,
                         0,    0,    0,    0,    0,    0,    0,    0,    0,    0};
        std::memcpy(api.build_id, bid, sizeof(bid));
        api.userdata = this;
        api.main_base = base;
        api.main_size = 0x3200000;
        api.is_mapped = mapped;
        api.read_memory = read;
        api.read_romfs = LocalRomfs;
        api.write_memory = [](void* p, U, const void*, size_t) -> EdenDsmodBool {
            ++static_cast<Host*>(p)->writes;
            return 0;
        };
        api.begin_output = [](void* p) {
            auto& h = *static_cast<Host*>(p);
            ++h.begins;
            h.nums.clear();
            h.texts.clear();
            h.floats.clear();
        };
        api.end_output = [](void* p) { ++static_cast<Host*>(p)->ends; };
        api.publish_i64 = [](void* p, const char* k, int64_t v) {
            static_cast<Host*>(p)->nums[k] = v;
        };
        api.publish_f64 = [](void* p, const char* key, double v) {
            static_cast<Host*>(p)->floats[key] = v;
        };
        api.publish_text = [](void* p, const char* k, const char* v) {
            static_cast<Host*>(p)->texts[k] = v;
        };
        reset();
    }
    void reset() {
        ram.clear();
        party_reads = 0;
        tear = false;
        code(0x8808c0, {0xd000cf88, 0xb945cd00, 0xd65f03c0});
        code(0x8805c0, {0x92403c08, 0xd000cf69, 0x91398129, 0x5280540a, 0x9b0a2500, 0xd65f03c0});
        code(0x720da0, {0xd000d7a8, 0xf9416508, 0x79400100, 0xd65f03c0});
        code(0xce5740, {0x9000af28, 0xf940b108, 0xf9402508, 0xb9400500, 0xd65f03c0});
        code(0xad0eb0, {0xb000bfb5, 0xf94236b3});
        code(0xad0ebc, {0xb000bfb4, 0xf9423288});
        code(0xedcd18, {0xd000a1e8, 0x911f6108});
        code(0xedcdbc, {0x90007e88, 0x2a1f03e0, 0xb90f3913});
        put(base + 0x1eacf38, int32_t(-1));
        put(base + 0x231a820, std::array<U, 512>{});
        put(base + 0x2209f50, U(0));
        put(base + 0x22c5468, fieldtask);
        put(base + 0x22c5460, U(117));
        put(fieldtask, uint32_t(2));
        put(fieldtask + 0xb8, U(117));
        put(fieldtask + 0x48, fieldctx);
        put(fieldctx + 0x1f0, uint16_t(3));
        put(fieldctx + 0x1f2, uint16_t(2));
        put(fieldctx + 0x1fe, uint16_t(0));
        put(base + money, uint32_t(87398));
        put(base + date, dateptr);
        put(dateptr, uint16_t(42));
        put(dateptr + 2, uint8_t(5));
        put(base + scene, task);
        put(task + 0x48, ctx);
        put(ctx, uint32_t(0));
        put(ctx + 4, int32_t(4));
        put(base + units + 0x1ce0, std::array<uint16_t, 10>{1, 2, 3, 4, 0, 0, 0, 0, 0, 0});
        for (unsigned id = 1; id <= 4; ++id) {
            U u = base + units + 0x2a0 * id;
            put(u + 4, uint16_t(1));
            put(u + 8, uint32_t(id));
            put(u + 12, uint32_t(194 + id));
            put(u + 16, uint32_t(96 + id));
            put(u + 24, uint16_t(id == 1 ? 14 : 1));
            put(u + 0x40, uint16_t(2));
            put(u + 0x44 + 0x60, uint8_t(1));
            put(u + 0x48 + 0x60, uint8_t(17 + id));
        }
    }
};

// ---- Social stats + confidants (p5r_social_reader.h) ----
// Synthetic save states and tables in the runtime layouts: cmmPC_PARAM_Name.ctd (char[5][20]),
// cmmPC_PARAM_Help.ctd (rank titles char[5][5][20]), cmmPC_PARAM_EXP.ctd (rank increments
// u16[5][4]), the points u16[5] and the confidant table (u16, then 24 x 16-byte slots).
constexpr std::array<const char*, 5> StatNames{"Wisdom", "Nerve", "Skill", "Warmth", "Style"};
std::string StatTitle(unsigned stat, unsigned rank) {
    return std::string(StatNames[stat]) + " rank " + std::to_string(rank);
}
// Cumulative thresholds 30/70/120/180, 10/30/60/100, 15/40/75/120, 11/31/61/101, 12/36/72/120.
constexpr std::array<uint16_t, 20> StatIncrements{30, 40, 50, 60, 10, 20, 30, 40, 15, 25,
                                                  35, 45, 11, 20, 30, 40, 12, 24, 36, 48};
struct SocialSave {
    std::array<uint16_t, 5> points;
    std::vector<std::pair<uint16_t, uint16_t>> confidants; // {id, rank}, slots 0.. in order
};
// Save A: every stat at rank 2 (stat 3 exactly on its first threshold), eight confidants.
const SocialSave SaveA{{40, 12, 17, 11, 16},
                       {{7, 3}, {1, 2}, {35, 1}, {2, 4}, {14, 2}, {8, 5}, {19, 3}, {6, 1}}};
// Save B: every stat at rank 1, four confidants.
const SocialSave SaveB{{4, 0, 3, 0, 0}, {{2, 3}, {1, 2}, {8, 1}, {7, 2}}};
std::vector<uint8_t> StatNameTable() {
    std::vector<uint8_t> v(5 * 20, 0);
    for (size_t s = 0; s < 5; ++s)
        std::memcpy(v.data() + 20 * s, StatNames[s], std::strlen(StatNames[s]));
    return v;
}
std::vector<uint8_t> StatTitleTable() {
    std::vector<uint8_t> v(5 * 5 * 20, 0);
    for (unsigned s = 0; s < 5; ++s)
        for (unsigned r = 1; r <= 5; ++r) {
            const auto t = StatTitle(s, r);
            std::memcpy(v.data() + 100 * s + 20 * (r - 1), t.data(), t.size());
        }
    return v;
}
std::vector<uint8_t> StatIncrementTable() {
    std::vector<uint8_t> v(sizeof(StatIncrements));
    std::memcpy(v.data(), StatIncrements.data(), v.size());
    return v;
}
std::vector<uint8_t> SavePoints(const SocialSave& save) {
    std::vector<uint8_t> v(sizeof(save.points));
    std::memcpy(v.data(), save.points.data(), v.size());
    return v;
}
std::vector<uint8_t> SaveConfidants(const SocialSave& save) {
    std::vector<uint8_t> v(2 + 0x10 * 24, 0);
    for (size_t i = 0; i < save.confidants.size(); ++i) {
        std::memcpy(v.data() + 2 + 0x10 * i + 4, &save.confidants[i].first, 2);
        std::memcpy(v.data() + 2 + 0x10 * i + 6, &save.confidants[i].second, 2);
    }
    return v;
}

std::vector<uint8_t> Hex(const char* text) {
    std::vector<uint8_t> out;
    for (size_t i = 0; text[i] && text[i + 1]; i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(std::string(text + i, 2), nullptr, 16)));
    return out;
}
constexpr U CtdThresholds = 0x94000000, CtdTitles = 0x94001000, CtdNames = 0x94002000;
constexpr U SocialPoints = 0x226ee60 + 0x1394c, ConfidantTable = 0x2282624;
void PutBytes(Host& h, U at, const std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i < bytes.size(); ++i)
        h.ram[at + i] = bytes[i];
}
// Runtime (already byte-swapped) CTD: header 0x20, one block at +0x20, data at +0x30.
void PutCtd(Host& h, U at, uint32_t size, const std::vector<uint8_t>& data) {
    h.put(at, uint32_t(0x100));
    h.put(at + 4, uint32_t(0x46544430));
    h.put(at + 8, uint32_t(size + 0x30));
    h.put(at + 0xc, uint32_t(1));
    h.put(at + 0x10, uint32_t(0x20));
    h.put(at + 0x20, uint32_t(0));
    h.put(at + 0x24, size);
    h.put(at + 0x28, uint32_t(5));
    h.put(at + 0x2c, uint32_t(0));
    PutBytes(h, at + 0x30, data);
}
void InstallSocial(Host& h, const SocialSave& save) {
    // The accessor instruction words the social reader matches or decodes.
    h.code(0x886fb0, {0x12003c08, 0x7100051f, 0x54000101, 0x9000cf48, 0x91398108, 0x8b21a508,
                      0x52872989, 0x72a00029, 0x78696900, 0xd65f03c0});
    h.code(0x869850, {0x51000428, 0x12003d08, 0x71000d1f, 0x54000069, 0x2a1f03e0, 0xd65f03c0,
                      0xb000d028, 0xf9463508, 0xb9401109, 0x8b090108, 0x12003c29, 0x8b20ad08,
                      0x51000529, 0x8b294508, 0x79402100, 0xd65f03c0});
    h.code(0x869820, {0xb000d028, 0xf945ed08, 0xb9401109, 0x52800c8a, 0x8b090108, 0x93403c09,
                      0x9b0a2128, 0x93403c29, 0x5280028a, 0x9b0a2128, 0xd1001100, 0xd65f03c0});
    h.code(0x869970, {0xb000d028, 0xf9460508, 0xb9401109, 0x8b090108, 0x0b000809, 0x531e7529,
                      0x8b090109, 0xb9400908, 0x91004129, 0x6b00011f, 0x9a9f8120, 0xd65f03c0});
    h.code(0x857990, {0x72003c1f, 0x54000100, 0xf000d0a8, 0xf9458d08, 0x79400d09, 0x6b20213f,
                      0x540000a1, 0x91000900});
    h.put(h.base + 0x226ec68, CtdThresholds);
    h.put(h.base + 0x226ebd8, CtdTitles);
    h.put(h.base + 0x226ec08, CtdNames);
    h.put(h.base + 0x226eb18, h.base + ConfidantTable);
    PutCtd(h, CtdThresholds, 0x28, StatIncrementTable());
    PutCtd(h, CtdTitles, 0x1f4, StatTitleTable());
    PutCtd(h, CtdNames, 0x64, StatNameTable());
    PutBytes(h, h.base + SocialPoints, SavePoints(save));
    PutBytes(h, h.base + ConfidantTable, SaveConfidants(save));
}
void PutConfidant(Host& h, unsigned slot, uint16_t id, uint16_t rank, uint16_t flags = 0) {
    const U entry = h.base + ConfidantTable + 2 + 0x10 * slot;
    h.put(entry + 2, flags);
    h.put(entry + 4, id);
    h.put(entry + 6, rank);
}
struct Expect {
    const char* arcana;
    int64_t index, rank;
};
void CheckConfidants(Host& h, std::initializer_list<Expect> rows, const char* what) {
    check(h.nums["confidant.ready"] == 1, what);
    check(h.nums["confidant.count"] == int64_t(rows.size()), what);
    size_t i = 0;
    for (const auto& row : rows) {
        const auto p = "confidant." + std::to_string(i++) + ".";
        check(h.nums[p + "present"] == 1 && h.texts[p + "name"] == row.arcana &&
                  h.nums[p + "arcana"] == row.index && h.nums[p + "rank"] == row.rank,
              what);
    }
    check(!h.nums["confidant." + std::to_string(i) + ".present"], what);
}
// ---- Persona stock / ally personas / skill help (p5r_persona_reader.h) ----
#include "core/mods/dsmod_module_extensions.h"
#include "p5r_persona_fixtures.inc"
#include "p5r_roster_fixtures.inc"
// Camp roster routines (fingerprinted bytes) + joined flags 0x4000002F+id for `joined`. Uses the
// BIT_CHK section table already in the fixture (main+1D834C8), creating sections 0/4 if absent.
void InstallRoster(Host& h, std::initializer_list<uint16_t> joined) {
    for (const auto& c : RosterCodeFixture())
        PutBytes(h, h.base + c.offset, c.bytes);
    auto u64at = [&](U at) {
        U v = 0;
        for (int i = 0; i < 8; ++i)
            v |= U(h.ram.count(at + i) ? h.ram.at(at + i) : 0) << (8 * i);
        return v;
    };
    const U table = h.base + 0x1d834c8;
    const std::array<std::pair<unsigned, U>, 2> own{{{0, 0x2f00000}, {4, 0x2f00200}}};
    // Sections 0 and 4 move into main (the live table's words are main .bss, which BIT_CHK's
    // reproduction requires) with 0xC00 bits; existing bits are carried over.
    for (const auto& [sec, words] : own) {
        const U old_words = u64at(table + 16 * sec), old_bits = u64at(table + 16 * sec + 8);
        std::vector<uint8_t> bytes(0xc00 / 8, 0);
        for (U i = 0; old_words && i < std::min<U>(old_bits / 8, bytes.size()); ++i)
            bytes[i] = h.ram.count(old_words + i) ? h.ram.at(old_words + i) : 0;
        PutBytes(h, h.base + words, bytes);
        h.put(table + 16 * sec, h.base + words);
        h.put(table + 16 * sec + 8, U(0xc00));
    }
    for (uint16_t id : joined) {
        static constexpr std::array<uint32_t, 11> Flag{
            0,          0,          0x40000030, 0x40000031, 0x40000032, 0x40000033,
            0x40000034, 0x40000035, 0x40000036, 0x40000037, 0x40000038};
        const uint32_t f = Flag[id];
        const U at = u64at(table + 16 * (f >> 28)) + 4 * ((f >> 5) & 0x7fffff);
        uint32_t w = 0;
        for (int i = 0; i < 4; ++i)
            w |= uint32_t(h.ram.count(at + i) ? h.ram.at(at + i) : 0) << (8 * i);
        h.put(at, w | (1u << (f & 31)));
    }
}
void InstallPersona(Host& h) {
    for (const auto& c : PersonaCodeWords) {
        U pc = c.offset;
        for (uint32_t w : c.words) {
            h.put(h.base + pc, w);
            pc += 4;
        }
    }
    for (const auto& blob : PersonaData)
        PutBytes(h, blob.main ? h.base + blob.at : blob.at, Hex(blob.hex));
    // The fixture replaces units 1/2 wholesale; restore the party fields the live gate reads.
    for (unsigned id = 1; id <= 2; ++id) {
        const U u = h.base + h.units + 0x2a0 * id;
        h.put(u + 12, uint32_t(194 + id));
        h.put(u + 16, uint32_t(96 + id));
    }
    h.put(h.base + h.units + 0x2a0 + 24, uint16_t(14));
    // 882310 counters 0xFE / 0xFF (840D00 cost overrides) read by the SKILL confirm verdict.
    h.put(h.base + h.units + 0x3db0 + 4 * 0xfe, int32_t(0));
    h.put(h.base + h.units + 0x3db0 + 4 * 0xff, int32_t(0));
}
// Camp rosters (7E3970 SKILL/ITEM, 7E3BB0 STATS/EQUIP) with reserve members, Futaba and the
// Kasumi -> Sumire rename (flag 0x849).
void RosterTests(const EdenDsmodModuleApi* m) {
    Host h;
    // active party: protagonist, Ann, Kasumi, Akechi; joined: 2..7, 8 (Futaba), 9, 10
    h.put(h.base + h.units + 0x1ce0, std::array<uint16_t, 10>{1, 4, 10, 9, 0, 0, 0, 0, 0, 0});
    for (unsigned id = 5; id <= 10; ++id) {
        const U u = h.base + h.units + 0x2a0 * id;
        h.put(u + 4, uint16_t(1));
        h.put(u + 8, uint32_t(id));
        h.put(u + 12, uint32_t(500 + id));
        h.put(u + 16, uint32_t(300 + id));
        h.put(u + 0x40, uint16_t(0));
        h.put(u + 0x44, uint8_t(1));
        h.put(u + 0x48, uint8_t(80 + id));
    }
    InstallRoster(h, {2, 3, 4, 5, 6, 7, 8, 9, 10});
    void* r = OpenAll(m->create(&h.api, "{}"));
    check(r, "roster create");
    m->sample(r, &h.api);
    check(h.nums["live.ready"] == 1 && h.nums["roster.ready"] == 1, "roster ready");
    const std::array<int, 9> a{1, 2, 3, 4, 5, 6, 7, 9, 10};
    check(h.nums["roster.count"] == 9, "SKILL roster: protagonist + 8 (no Futaba)");
    for (size_t i = 0; i < a.size(); ++i)
        check(h.nums["roster." + std::to_string(i) + ".id"] == a[i], "SKILL roster order");
    check(h.nums["roster.0.party"] == 2 && h.nums["roster.3.party"] == 1 &&
              h.nums["roster.1.party"] == 0 && h.nums["roster.8.party"] == 1 &&
              h.nums["roster.7.party"] == 1,
          "roster party badges");
    check(h.nums["eroster.count"] == 10 && h.nums["eroster.9.id"] == 8 &&
              h.nums["eroster.9.navi"] == 1 && h.nums["eroster.8.id"] == 10,
          "STATS roster: Futaba last");
    check(h.texts["roster.8.name"] == "Kasumi" && h.texts["party.2.name"] == "Kasumi",
          "Kasumi before the rename");
    // flag 0x849 (section 0): Sumire
    {
        const U t = h.base + 0x1d834c8;
        U words = 0;
        for (int i = 0; i < 8; ++i)
            words |= U(h.ram[t + i]) << (8 * i);
        const U at = words + 4 * (0x849 >> 5);
        uint32_t w = 0;
        for (int i = 0; i < 4; ++i)
            w |= uint32_t(h.ram[at + i]) << (8 * i);
        h.put(at, w | (1u << (0x849 & 31)));
    }
    m->sample(r, &h.api);
    check(h.texts["roster.8.name"] == "Sumire" && h.texts["eroster.8.name"] == "Sumire" &&
              h.texts["party.2.name"] == "Sumire",
          "Sumire after flag 0x849");
    // A member not joined but in the party array is on the SKILL roster only (880710).
    InstallRoster(h, {});
    {
        const U t = h.base + 0x1d834c8 + 64;
        U words = 0;
        for (int i = 0; i < 8; ++i)
            words |= U(h.ram[t + i]) << (8 * i);
        h.put(words + 4, uint32_t(0)); // clear 0x40000030..3F
    }
    m->sample(r, &h.api);
    check(h.nums["roster.count"] == 4 && h.nums["roster.1.id"] == 4 && h.nums["roster.2.id"] == 9 &&
              h.nums["roster.3.id"] == 10 && h.nums["eroster.count"] == 1,
          "party-array members without the joined flag");
    m->destroy(r);
}
void PersonaTests(const EdenDsmodModuleApi* m) {
    Host h;
    InstallPersona(h);
    InstallRoster(h, {2, 3, 4});
    void* r = OpenAll(m->create(&h.api, "{}"));
    check(r, "persona create");
    m->sample(r, &h.api);
    check(!h.writes && h.begins == h.ends, "persona read only");
    check(h.nums["live.ready"] == 1, "persona fixture keeps live gate");
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    check(ext && ext->on_action, "extensions action hook");
    auto ext_skill_select = [&](void* inst, int64_t arg) {
        return ext->on_action(inst, "skill_select", arg);
    };
    check(h.nums["persona.ready"] == 1 && h.nums["persona.count"] == 4, "stock rows");
    check(h.nums["persona.current"] == 1 && h.nums["persona.derived"] == 1, "equipped row");
    const std::array<const char*, 4> names{"PsnOne", "PsTwo", "PsnThree", "PsFiv"};
    const std::array<const char*, 4> arcana{"Arc1", "Arcana Number2", "Arc0", "Arcana8"};
    const std::array<int, 4> levels{11, 10, 8, 9};
    for (size_t i = 0; i < 4; ++i) {
        const auto p = "persona." + std::to_string(i) + ".";
        check(h.nums[p + "present"] == 1 && h.texts[p + "name"] == names[i] &&
                  h.texts[p + "arcana_name"] == arcana[i] && h.nums[p + "level"] == levels[i],
              "stock row identity");
    }
    check(!h.nums.count("persona.4.id") && h.nums["persona.4.present"] == 0, "no stale row");
    check(h.nums["persona.0.skill_count"] == 2 && h.texts["persona.0.skill.0.name"] == "Sk01" &&
              h.texts["persona.0.skill.1.name"] == "Skill02" &&
              h.texts["persona.0.skill.2.name"].empty() && h.nums["persona.0.skill.2.icon"] == -1,
          "skill rows (empty slots -1)");
    // EC49F0 -> EC4CC0 over the fixture's icon tables: wind -> cell 23, support (0x15) -> 31,
    // passive flag -> 32 (ICON.DDS, 6 columns).
    check(h.nums["persona.0.skill.0.icon"] == 23 && h.nums["persona.0.skill.1.icon"] == 31 &&
              h.nums["persona.1.skill.2.icon"] == 32,
          "native skill icon cells");
    // Native 890840 / 892400 / 88FE60 results over the same layout (fixture generator).
    auto expect = [&](const std::string& p, const PersonaExpect& e, const char* what) {
        static constexpr std::array<const char*, 5> Stat{"st", "ma", "en", "ag", "lu"};
        for (size_t i = 0; i < 10; ++i)
            check(h.nums[p + "aff." + std::to_string(i)] == e.aff[i], what);
        for (size_t i = 0; i < 5; ++i)
            check(h.nums[p + Stat[i]] == e.stats[i], what);
        check(h.nums[p + "next"] == e.next, what);
    };
    const std::array<std::string, 4> rows{"persona.0.", "persona.1.", "persona.2.", "persona.3."};
    for (size_t i = 0; i < 4; ++i)
        expect(rows[i], PersonaExpected[i], "native-derived stock values");
    check(h.nums["ally.1.persona.present"] == 1 &&
              h.texts["ally.1.persona.name"] == "Persona Four" &&
              h.texts["ally.1.persona.arcana_name"] == "Arcana7" &&
              h.nums["ally.1.persona.level"] == 14,
          "ally persona");
    expect("ally.1.persona.", PersonaExpected[4], "native-derived ally values");
    // Camp SKILL lists = native 80C150 over the same layout: the persona with the Spell Master
    // passive ("Passive No 8" in the fixture) wins the duplicate of skill 300 (cost 1),
    // the gap at stock slot 3 hides slot 4 (native loop bound), order.
    check(h.nums["skill.ready"] == 1, "skill lists ready");
    for (const auto& list : PersonaSkillLists) {
        const std::string p = "skill." + std::to_string(list.member - 1) + ".";
        check(h.nums[p + "count"] == int64_t(list.rows.size()), "native skill list size");
        for (size_t i = 0; i < list.rows.size(); ++i) {
            const auto r = p + std::to_string(i) + ".";
            check(h.nums[r + "id"] == list.rows[i].first &&
                      h.nums[r + "cost"] == list.rows[i].second && h.nums[r + "cost_hp"] == 0,
                  "native skill list rows");
        }
    }
    check(h.texts["skill.0.0.name"] == "Sk004" && h.nums["skill.0.0.icon"] == 30,
          "skill list names/icons");
    // SKILL confirm verdict (812990 -> 840D00 -> 873680): SP 97/98 pays for every row; the
    // native list never dims a row (grey 0 whatever the verdict).
    for (const auto& list : PersonaSkillLists)
        for (size_t i = 0; i < list.rows.size(); ++i) {
            const auto k =
                "skill." + std::to_string(list.member - 1) + "." + std::to_string(i) + ".";
            check(h.nums[k + "usable"] == 1 && h.nums[k + "grey"] == 0, "skill rows payable");
        }
    {
        const U u1 = h.base + h.units + 0x2a0;
        auto resample = [&] {
            for (int i = 0; i < 10; ++i)
                m->sample(r, &h.api);
        };
        // SP 2: skill 305 (3 SP) is refused (0xFE), skill 300 (1 SP) still accepted.
        h.put(u1 + 16, uint32_t(2));
        resample();
        check(h.nums["skill.0.0.usable"] == 0 && h.nums["skill.0.1.usable"] == 1 &&
                  h.nums["skill.0.0.grey"] == 0,
              "SP short -> refused, not dimmed");
        check(ext->on_action(r, "skill_select", 0 * 16 + 0), "select skill 305");
        m->sample(r, &h.api);
        check(h.nums["skill.sel.can_use"] == 0, "no USE for a refused skill");
        check(!ext->on_action(r, "skill_use", (0 * 16 + 0) * 16 + 1) &&
                  !ext->on_action(r, "skill_use_sel", 1),
              "refused skill is never driven");
        // 840D00: unit kind 1 with counter 0xFF > 0 lifts the SP refusal.
        h.put(h.base + h.units + 0x3db0 + 4 * 0xff, int32_t(1));
        resample();
        check(h.nums["skill.0.0.usable"] == 1, "counter 0xFF lifts 0xFE");
        h.put(h.base + h.units + 0x3db0 + 4 * 0xff, int32_t(0));
        // 873680: status bit 0x40 (unit +0x14) refuses every row (0xFC) before the cost.
        h.put(u1 + 16, uint32_t(97));
        h.put(u1 + 0x14, uint32_t(0x40));
        resample();
        check(h.nums["skill.0.0.usable"] == 0 && h.nums["skill.0.1.usable"] == 0 &&
                  h.nums["skill.1.0.usable"] == 1,
              "status 0x40 -> refused (that member only)");
        h.put(u1 + 0x14, uint32_t(0));
        resample();
        check(h.nums["skill.0.0.usable"] == 1 && ext->on_action(r, "skill_select", -1),
              "verdict follows the live state");
        m->sample(r, &h.api);
    }
    check(ext_skill_select(r, 0 * 16 + 1), "select SKILL row");
    m->sample(r, &h.api);
    check(h.nums["skill.sel"] == 1 && h.nums["skill.desc.id"] == -1,
          "SKILL row echo (no help for skill 300)");
    check(h.nums["ally.0.persona.present"] == 0, "protagonist not repeated");
    check(h.nums["ally.2.persona.present"] == 0, "ally without a valid persona hidden");
    // Help text for a selected skill via the module action.
    check(h.nums["skill.desc.id"] == -1 && h.texts["skill.desc"].empty(), "no selection");
    check(!ext->on_action(r, "persona_skill_select", 5000), "selection bounds");
    check(ext->on_action(r, "persona_skill_select", 0 * 8 + 0), "select skill 30");
    m->sample(r, &h.api);
    check(h.nums["skill.desc.id"] == 30 && h.texts["skill.desc"] == "Help text line\nsecond row.",
          "native skill help");
    check(ext->on_action(r, "persona_skill_select", 0 * 8 + 5), "select empty skill");
    m->sample(r, &h.api);
    check(h.nums["skill.desc.id"] == -1 && h.texts["skill.desc"].empty(), "empty slot clears");
    // 0.8 "_sel" actions use the stored selection; the equipped row is never re-equipped.
    check(!ext->on_action(r, "persona_change_sel", 0), "persona_change_sel needs a selection");
    check(ext->on_action(r, "persona_select", 1) && !ext->on_action(r, "persona_change_sel", 0),
          "equipped persona refused");
    check(ext->on_action(r, "persona_select", 0) && ext->on_action(r, "persona_change_sel", 0),
          "persona_change_sel starts the drive");
    check(!ext->on_action(r, "persona_change_sel", 0) && !ext->on_action(r, "item_use_sel", 0),
          "one drive at a time");
    check(ext->on_action(r, "persona_change", -1), "cancel");
    m->sample(r, &h.api);
    check(h.nums["pdrv.kind"] == 1 && h.texts["pdrv.text"] == "Change persona: cancelled" &&
              h.nums["persona.sel"] == 0,
          "persona drive status");
    check(ext->on_action(r, "skill_select", 1) && !ext->on_action(r, "skill_use_sel", 4),
          "skill_use_sel target bounds");
    m->destroy(r);
    // A persona change between the read and the re-check drops the whole stock sample.
    h.social_tear = h.base + h.units + 0x2a0 + 0x40;
    h.social_tear_reads = 0;
    r = OpenAll(m->create(&h.api, "{}"));
    m->sample(r, &h.api);
    check(h.nums["live.ready"] == 1 && h.nums["persona.ready"] == 0 &&
              h.nums["persona.0.present"] == 0,
          "torn stock rejected");
    m->destroy(r);
    h.social_tear = 0;
    // Code drift in a reproduced routine keeps identity rows but withholds derived values.
    h.ram[h.base + 0x890840 + 0x100] ^= 1;
    r = OpenAll(m->create(&h.api, "{}"));
    m->sample(r, &h.api);
    check(h.nums["persona.ready"] == 1 && h.nums["persona.derived"] == 0 &&
              h.texts["persona.0.name"] == "PsnOne" && !h.nums.count("persona.0.st") &&
              !h.nums.count("persona.0.aff.0") && !h.nums.count("persona.0.next"),
          "fingerprint mismatch withholds derived values");
    m->destroy(r);
}
void SocialTests(const EdenDsmodModuleApi* m) {
    Host h;
    void* r{};
    auto fresh = [&] {
        if (r)
            m->destroy(r);
        r = OpenAll(m->create(&h.api, "{}"));
        check(r, "social create");
        h.social_tear_reads = 0;
        m->sample(r, &h.api);
        check(!h.writes && h.begins == h.ends, "social read only");
    };
    auto stats = [&](std::initializer_list<int64_t> ranks, std::initializer_list<int64_t> points,
                     std::initializer_list<int64_t> next, const char* what) {
        check(h.nums["social.ready"] == 1, what);
        for (unsigned i = 0; i < 5; ++i) {
            const auto p = "social." + std::to_string(i) + ".";
            const auto rank = ranks.begin()[i];
            check(h.nums[p + "rank"] == rank && h.nums[p + "points"] == points.begin()[i] &&
                      h.nums[p + "next"] == next.begin()[i] &&
                      h.texts[p + "name"] == StatNames[i] &&
                      h.texts[p + "title"] == StatTitle(i, unsigned(rank)),
                  what);
        }
    };
    // Save A: every stat at rank 2; the native Confidant menu order is by category, not slot.
    InstallSocial(h, SaveA);
    fresh();
    check(h.nums["live.ready"] == 1, "social fixture keeps live gate");
    stats({2, 2, 2, 2, 2}, {40, 12, 17, 11, 16}, {30, 18, 23, 20, 20},
          "save A social stats (stat 3 == threshold crosses)");
    CheckConfidants(h,
                    {{"Fool", 1, 2},
                     {"Magician", 2, 4},
                     {"Hierophant", 6, 1},
                     {"Lovers", 7, 3},
                     {"Chariot", 8, 5},
                     {"Death", 14, 2},
                     {"Moon", 19, 3},
                     {"Councillor", 30, 1}},
                    "save A native confidant order");
    check(h.nums["confidant.7.id"] == 35 && !h.nums["confidant.7.max"], "Councillor raw id");
    // Save B: all rank 1; four confidants.
    h.reset();
    InstallSocial(h, SaveB);
    fresh();
    stats({1, 1, 1, 1, 1}, {4, 0, 3, 0, 0}, {26, 10, 12, 11, 12}, "save B social");
    CheckConfidants(h, {{"Fool", 1, 2}, {"Magician", 2, 3}, {"Lovers", 7, 2}, {"Chariot", 8, 1}},
                    "save B native confidant order");
    // Rank 5 cap: points beyond the last cumulative threshold (stat 0: 180) -> next 0.
    h.put(h.base + SocialPoints, uint16_t(190));
    h.put(h.base + SocialPoints + 2, uint16_t(29));
    fresh();
    check(h.nums["social.0.rank"] == 5 && !h.nums["social.0.next"] &&
              h.texts["social.0.title"] == StatTitle(0, 5),
          "rank 5 cap");
    check(h.nums["social.1.rank"] == 2 && h.nums["social.1.next"] == 1, "one below threshold");
    // Native list builder: one row per category 0..24, lowest enrolled ID wins, ID 22 hidden,
    // Faith (category 23) before Councillor (24), flags from CMM_CHK_BROKEN/REVERSE, rank 10 max.
    h.reset();
    InstallSocial(h, SaveB);
    for (unsigned slot = 0; slot < 24; ++slot)
        PutConfidant(h, slot, 0, 0);
    PutConfidant(h, 0, 35, 10);
    PutConfidant(h, 1, 36, 2);
    PutConfidant(h, 2, 22, 3);
    PutConfidant(h, 3, 23, 7);
    PutConfidant(h, 4, 33, 4, 1);
    PutConfidant(h, 9, 3, 5, 2);
    fresh();
    CheckConfidants(h, {{"High Priestess", 3, 5}, {"Faith", 29, 4}, {"Councillor", 30, 10}},
                    "native category order");
    check(h.nums["confidant.0.broken"] && !h.nums["confidant.0.reversed"] &&
              h.nums["confidant.1.reversed"] && h.nums["confidant.2.max"] &&
              !h.nums["confidant.1.max"] && h.nums["confidant.1.id"] == 33,
          "confidant flags");
    // Rejections: no guessed values are published.
    PutConfidant(h, 5, 7, 11);
    fresh();
    check(!h.nums["confidant.ready"] && !h.nums["confidant.count"] &&
              !h.nums["confidant.0.present"] && h.nums["social.ready"] == 1,
          "rank above 10 hides confidants only");
    PutConfidant(h, 5, 35, 3);
    fresh();
    check(!h.nums["confidant.ready"], "duplicate ID rejected");
    PutConfidant(h, 5, 38, 3);
    fresh();
    check(!h.nums["confidant.ready"], "unknown ID rejected");
    PutConfidant(h, 5, 0, 0);
    h.social_tear = h.base + ConfidantTable;
    fresh();
    check(!h.nums["confidant.ready"] && h.nums["social.ready"] == 1, "torn confidant table");
    m->sample(r, &h.api);
    check(h.nums["confidant.ready"] == 1, "incomplete sample retried next frame");
    h.social_tear = h.base + SocialPoints;
    fresh();
    check(!h.nums["social.ready"] && !h.nums["social.0.rank"] && h.texts["social.0.name"].empty(),
          "torn social points");
    h.social_tear = 0;
    h.put(h.base + SocialPoints + 4, uint16_t(0x8000));
    fresh();
    check(!h.nums["social.ready"] && h.nums["confidant.ready"] == 1, "negative points rejected");
    h.put(h.base + SocialPoints + 4, uint16_t(3));
    h.put(CtdThresholds + 4, uint32_t(0x30445446));
    fresh();
    check(!h.nums["social.ready"], "thresholds CTD magic");
    h.put(CtdThresholds + 4, uint32_t(0x46544430));
    h.put(CtdThresholds + 0x28, uint32_t(4));
    fresh();
    check(!h.nums["social.ready"], "thresholds CTD entry count");
    h.put(CtdThresholds + 0x28, uint32_t(5));
    h.put(CtdThresholds + 0x30 + 8, uint16_t(0));
    fresh();
    check(!h.nums["social.ready"], "zero threshold increment");
    h.put(CtdThresholds + 0x30 + 8, uint16_t(10));
    // Optional label tables: names fall back to the verified asset order, missing titles hide.
    h.put(h.base + 0x226ec08, U(0));
    h.put(h.base + 0x226ebd8, U(0));
    fresh();
    check(h.nums["social.ready"] == 1 && h.texts["social.3.name"] == "Guts" &&
              h.texts["social.3.title"].empty(),
          "label fallback");
    h.put(h.base + 0x226ebd8, CtdTitles);
    h.put(CtdTitles + 0x30 + 0x64 * 3, std::array<char, 20>{'W', 'a', 'r', 'm', '\n'});
    fresh();
    check(!h.nums["social.ready"], "unterminated/control title rejected");
    PutCtd(h, CtdTitles, 0x1f4, StatTitleTable());
    h.put(h.base + 0x226ec08, CtdNames);
    fresh();
    check(h.nums["social.ready"] == 1, "restored");
    // Gameplay gate: valid save tables are hidden outside gameplay.
    h.put(h.ctx + 4, int32_t(2));
    fresh();
    check(!h.nums["live.ready"] && !h.nums["social.ready"] && !h.nums["confidant.ready"],
          "gameplay gate");
    h.put(h.ctx + 4, int32_t(4));
    // Accessor decode: move the thresholds slot by one LDR immediate (0xc68 -> 0xc70).
    h.code(0x86986c, {0xf9463908});
    h.put(h.base + 0x226ec70, CtdThresholds);
    h.put(h.base + 0x226ec68, U(0x94100000));
    fresh();
    check(h.nums["social.ready"] == 1 && h.nums["social.0.points"] == 4, "LDR immediate decoded");
    // Signature mismatch: social outputs hide, the rest of the module is unaffected.
    h.code(0x886fd0, {0x78696901});
    fresh();
    check(h.nums["live.ready"] == 1 && !h.nums["social.ready"] && !h.nums["confidant.ready"],
          "social signature mismatch is isolated");
    m->destroy(r);
}

// ---- Confidant detail (p5r_social_menu_reader.h) ----
// The accessor words are the reader's own match lists (p5r_social_menu::code). Table layouts are
// the runtime CTD layouts (as loader 869120 swaps them); names, descriptions, function ids,
// ranks and flags are synthetic.
constexpr U CtdName = 0x95000000, CtdExtra = 0x95001000, CtdMember = 0x95002000,
            CtdFormat = 0x95004000, CtdFuncTable = 0x95008000, CtdFuncInfo = 0x9500a000,
            CtdFuncName = 0x9500c000, CtdOpen = 0x95014000, HelpBmd = 0x95016000,
            CtdStory = 0x95018000, CtdStoryExtra = 0x95019000, CtdProfile = 0x9501a000,
            CtdProfileExtra = 0x9501b000;
constexpr U FlagWords0 = 0x22725d0, FlagWords1 = 0x2272750, FlagWords4 = 0x2272b90;
void PutCtdRows(Host& h, U at, uint32_t entry, uint32_t count) {
    h.put(at, uint32_t(0x100));
    h.put(at + 4, uint32_t(0x46544430));
    h.put(at + 8, uint32_t(entry * count + 0x30));
    h.put(at + 0xc, uint32_t(1));
    h.put(at + 0x10, uint32_t(0x20));
    h.put(at + 0x20, uint32_t(0));
    h.put(at + 0x24, entry * count);
    h.put(at + 0x28, count);
    h.put(at + 0x2c, uint32_t(0));
    PutBytes(h, at + 0x30, std::vector<uint8_t>(size_t(entry) * count, 0));
}
void PutLabel(Host& h, U at, const char* text) {
    std::array<char, 64> row{};
    std::strncpy(row.data(), text, row.size() - 1);
    h.put(at, row);
}
void SetFlag(Host& h, uint32_t flag, bool on) {
    const U words = (flag >> 28) == 0 ? FlagWords0 : (flag >> 28) == 1 ? FlagWords1 : FlagWords4;
    const U at = h.base + words + 4 * ((flag >> 5) & 0x7fffff);
    uint32_t w = 0;
    for (int i = 0; i < 4; ++i)
        w |= uint32_t(h.ram[at + i]) << (8 * i);
    w = on ? (w | (1u << (flag & 31))) : (w & ~(1u << (flag & 31)));
    h.put(at, w);
}
// Raw big-endian MSG1 as the CMM loader keeps it ("1GSM" at +8), one single-page
// message per text; offsets are relative to +0x20, +0x10 = relocation table (end of text).
void PutBe32(Host& h, U at, uint32_t v) {
    PutBytes(h, at, {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)});
}
void PutHelp(Host& h, std::initializer_list<const char*> texts) {
    const uint32_t count = uint32_t(texts.size());
    PutBytes(h, HelpBmd, std::vector<uint8_t>(0x20 + 8 * count + 0x28 * count, 0));
    PutBytes(h, HelpBmd, {7, 0, 0, 0});
    PutBytes(h, HelpBmd + 8, {'1', 'G', 'S', 'M'});
    PutBe32(h, HelpBmd + 0x18, count);
    U text_at = HelpBmd + 0x20 + 8 * count + 0x28 * count;
    uint32_t i = 0;
    for (const char* t : texts) {
        const U entry = HelpBmd + 0x20 + 8 * i, rec = HelpBmd + 0x20 + 8 * count + 0x28 * i;
        PutBe32(h, entry, 0);
        PutBe32(h, entry + 4, uint32_t(rec - HelpBmd - 0x20));
        PutBytes(h, rec + 0x18, {0, 1, 0xff, 0xff});
        PutBe32(h, rec + 0x1c, uint32_t(text_at - HelpBmd - 0x20));
        std::vector<uint8_t> bytes{0xf2, 0x05, 0xff, 0xff, 0xf1, 0x41};
        for (const char* c = t; *c; ++c)
            bytes.push_back(uint8_t(*c));
        bytes.push_back(0x0a);
        bytes.push_back(0);
        PutBytes(h, text_at, bytes);
        text_at += bytes.size();
        ++i;
    }
    PutBytes(h, text_at, std::vector<uint8_t>(0x200, 0));
    PutBe32(h, HelpBmd + 0x10, uint32_t(text_at - HelpBmd + 0x10));
}
void InstallMenu(Host& h) {
    namespace c = p5r_social_menu::code;
    auto words = [&](U pc, const auto& a) {
        for (size_t i = 0; i < a.size(); ++i)
            h.put(h.base + pc + 4 * i, a[i]);
    };
    words(c::NameGetter, c::Name);
    words(c::MemberGetter, c::Member);
    words(c::FormatGetter, c::Format);
    words(c::TableGetter, c::Table);
    words(c::InfoGetter, c::Info);
    words(c::NameFnGetter, c::NameFn);
    words(c::HelpGetter, c::Help);
    words(c::OpenCount, c::Count);
    words(c::OpenGetter, c::Open);
    words(c::BitCheck, c::Bit);
    words(c::Builder, c::Reveal);
    words(c::StoryGetter, c::Story);
    words(c::StoryExtraGetter, c::StoryExtra);
    words(c::ProfileGetter, c::Profile);
    words(c::ProfileExtraGetter, c::ProfileExtra);
    words(c::HelpBmdGetter, c::HelpBmd);
    h.put(h.base + 0x226ec50, CtdStory);
    h.put(h.base + 0x226ec58, CtdStoryExtra);
    h.put(h.base + 0x226ec60, CtdProfile);
    h.put(h.base + 0x226ed00, CtdProfileExtra);
    h.put(h.base + 0x226ebb8, HelpBmd);
    PutCtdRows(h, CtdStory, 0x18, 38);
    PutCtdRows(h, CtdStoryExtra, 0x18, 2);
    PutCtdRows(h, CtdProfile, 0x14, 34);
    PutCtdRows(h, CtdProfileExtra, 0x14, 1);
    // id 8 story rank 3 -> message 1; id 8 profile (chara 2) rank 3 -> message 0.
    h.put(CtdStory + 0x30 + 0x18 * 8 + 2 * 2, int16_t(1));
    h.put(CtdProfile + 0x30 + 0x14 * 2 + 2 * 2, int16_t(0));
    h.put(h.base + 0x226ebc8, CtdName);
    h.put(h.base + 0x226ebd0, CtdExtra);
    h.put(h.base + 0x226ec00, CtdMember);
    h.put(h.base + 0x226ec30, CtdFormat);
    h.put(h.base + 0x226ecd8, CtdFuncTable);
    h.put(h.base + 0x226ece0, CtdFuncInfo);
    h.put(h.base + 0x226ec20, CtdFuncName);
    h.put(h.base + 0x226ece8, CtdOpen);
    h.put(h.base + 0x226ebc0, HelpBmd);
    // BIT_CHK section table (main+1D834C8): {u32* words, u64 bits}[6].
    const std::array<std::pair<U, U>, 6> sections{{{FlagWords0, 0xc00},
                                                   {FlagWords1, 0xc00},
                                                   {0x22728d0, 0x1400},
                                                   {0x2272b50, 0x200},
                                                   {FlagWords4, 0x200},
                                                   {0x2272bd0, 0x200}}};
    for (size_t i = 0; i < sections.size(); ++i) {
        h.put(h.base + 0x1d834c8 + 16 * i, h.base + sections[i].first);
        h.put(h.base + 0x1d834c8 + 16 * i + 8, sections[i].second);
        PutBytes(h, h.base + sections[i].first, std::vector<uint8_t>(sections[i].second / 8, 0));
    }
    // 78DF44 arcana reveal flags {hide_help, reveal}: arcana 8 = {0x227, 0x23F}.
    for (unsigned a = 0; a < 32; ++a)
        h.put(h.base + 0x15e1200 + 8 * a, std::array<uint32_t, 2>{0x220 + a - 1, 0x238 + a - 1});
    PutCtdRows(h, CtdName, 64, 38);
    PutCtdRows(h, CtdExtra, 64, 2);
    PutCtdRows(h, CtdMember, 64, 38);
    PutCtdRows(h, CtdFormat, 0xbc, 38);
    PutCtdRows(h, CtdFuncTable, 0x78, 38);
    PutCtdRows(h, CtdFuncInfo, 8, 290);
    PutCtdRows(h, CtdFuncName, 64, 290);
    PutCtdRows(h, CtdOpen, 8, 61);
    const U names = CtdName + 0x30, shorts = CtdMember + 0x30, format = CtdFormat + 0x30;
    PutLabel(h, names + 64 * 8, "Aki Moriyama");
    PutLabel(h, names + 64 * 12, "Gate Keepers");
    PutLabel(h, names + 64 * 15, "Rei Takano");
    PutLabel(h, CtdExtra + 0x30, "Extra One");
    PutLabel(h, CtdExtra + 0x30 + 64, "Extra Two");
    PutLabel(h, shorts + 64 * 8, "Aki");
    PutLabel(h, shorts + 64 * 12, "Gate Keepers");
    PutLabel(h, shorts + 64 * 15, "Takano");
    auto chara = [&](unsigned id, uint8_t arcana, std::initializer_list<uint32_t> list) {
        h.put(format + 0xbc * id + 8, arcana);
        unsigned k = 0;
        for (uint32_t c : list)
            h.put(format + 0xbc * id + 0x28 + 8 * k++, c);
    };
    chara(8, 8, {2});
    chara(12, 12, {0x13, 0x12});
    chara(15, 15, {22});
    // id 8 row (runtime LE): {kind, rank, func, pad, flag} x 10.
    const std::array<std::tuple<uint16_t, int16_t, int16_t, uint32_t>, 10> row8{
        {{0, 2, 101, 0},
         {0, 3, 102, 0},
         {0, 4, 103, 0},
         {0, 6, 104, 0},
         {0, 7, 105, 0},
         {0, 8, 106, 0},
         {0, 9, 107, 0},
         {0, 10, 108, 0},
         {8, 10, 109, 0x100000f0},
         {0, -1, 0, 0}}};
    for (size_t k = 0; k < row8.size(); ++k) {
        const U e = CtdFuncTable + 0x30 + 0x78 * 8 + 12 * k;
        h.put(e, std::get<0>(row8[k]));
        h.put(e + 2, std::get<1>(row8[k]));
        h.put(e + 4, std::get<2>(row8[k]));
        h.put(e + 8, std::get<3>(row8[k]));
    }
    for (unsigned id : {12u, 15u})
        h.put(CtdFuncTable + 0x30 + 0x78 * id + 2, int16_t(-1));
    for (int16_t f : {101, 102, 103, 104, 105, 106, 107, 108, 109}) {
        PutLabel(h, CtdFuncName + 0x30 + 64 * f, ("Func " + std::to_string(f)).c_str());
        h.put(CtdFuncInfo + 0x30 + 8 * f + 2, int16_t(f == 101 ? 0 : f == 102 ? 1 : 2));
    }
    // OpenSP (85A170): func 103 opens early with flag 0x750.
    h.put(CtdOpen + 0x30 + 8 * 23, int16_t(103));
    h.put(CtdOpen + 0x30 + 8 * 23 + 4, uint32_t(0x750));
    PutHelp(h, {"Help zero.", "Help one.", "Help two."});
}
void MenuTests(const EdenDsmodModuleApi* m) {
    Host h;
    InstallSocial(h, SaveA);
    for (unsigned slot = 0; slot < 24; ++slot)
        PutConfidant(h, slot, 0, 0);
    PutConfidant(h, 0, 8, 3);
    PutConfidant(h, 1, 12, 1);
    PutConfidant(h, 2, 15, 2);
    InstallMenu(h);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    check(ext && ext->on_action, "menu action export");
    void* r = OpenAll(m->create(&h.api, "{}"));
    check(r, "menu create");
    auto run = [&] { // >= SocialPeriod samples: the detail refreshes with the social cache
        for (int i = 0; i < 9; ++i)
            m->sample(r, &h.api);
        check(!h.writes, "menu read only");
    };
    struct A {
        const char* name;
        int64_t rank, unlocked, early;
        const char* desc;
    };
    auto abilities = [&](std::initializer_list<A> rows, const char* what) {
        if (std::getenv("MENU_DEBUG"))
            for (auto& [k, v] : h.nums)
                if (k.rfind("confidant.sel", 0) == 0 && v)
                    std::cerr << k << "=" << v << " " << h.texts[k] << '\n';
        if (std::getenv("MENU_DEBUG"))
            for (auto& [k, v] : h.texts)
                if (k.rfind("confidant.sel", 0) == 0 && !v.empty())
                    std::cerr << k << "='" << v << "'\n";
        check(h.nums["confidant.sel.ready"] == 1, what);
        check(h.nums["confidant.sel.ability.count"] == int64_t(rows.size()), what);
        size_t i = 0;
        for (const auto& a : rows) {
            const auto p = "confidant.sel.ability." + std::to_string(i++) + ".";
            check(h.nums[p + "present"] == 1 && h.texts[p + "name"] == a.name &&
                      h.nums[p + "rank"] == a.rank && h.nums[p + "unlocked"] == a.unlocked &&
                      h.nums[p + "early"] == a.early && h.texts[p + "desc"] == a.desc &&
                      !h.nums[p + "hidden"],
                  what);
        }
        check(!h.nums["confidant.sel.ability." + std::to_string(i) + ".present"], what);
    };
    run();
    check(h.nums["confidant.count"] == 3 && h.nums["confidant.0.detail"] == 1 &&
              h.texts["confidant.0.person"] == "Aki Moriyama" &&
              h.texts["confidant.0.short"] == "Aki" &&
              h.texts["confidant.0.portrait_key"] == "c_chara_02" &&
              h.nums["confidant.0.portrait"] == 2,
          "row name + portrait");
    check(h.texts["confidant.1.person"] == "Gate Keepers" &&
              h.texts["confidant.1.portrait_key"] == "c_chara_18" &&
              h.texts["confidant.2.portrait_key"] == "c_chara_22b",
          "id 12 + id 15 b variant");
    check(h.nums["confidant.sel.index"] == 0 && h.nums["confidant.sel.id"] == 8 &&
              h.texts["confidant.sel.person"] == "Aki Moriyama" &&
              h.texts["confidant.sel.name"] == "Chariot" &&
              h.texts["confidant.sel.story"] == "Help one." &&
              h.texts["confidant.sel.profile"] == "Help zero.",
          "default selection + story/profile");
    // Rank 3: ranks <= 3 unlocked, then only the first locked rank (4) as upcoming.
    abilities({{"Func 101", 2, 1, 0, "Help zero."},
               {"Func 102", 3, 1, 0, "Help one."},
               {"Func 103", 4, 0, 0, "Help two."}},
              "rank 3 native lists");
    // OpenSP flag: 103 moves to the unlocked list early, the next locked rank becomes 6.
    SetFlag(h, 0x750, true);
    run();
    for (int i = 0; i < 8; ++i)
        m->sample(r, &h.api);
    abilities({{"Func 101", 2, 1, 0, "Help zero."},
               {"Func 102", 3, 1, 0, "Help one."},
               {"Func 103", 4, 1, 1, "Help two."},
               {"Func 104", 6, 0, 0, "Help two."}},
              "early unlock");
    // Reveal flag (arcana 8 -> 0x23F): every later rank; the flag-gated rank-10 entry stays out.
    SetFlag(h, 0x23f, true);
    for (int i = 0; i < 9; ++i)
        m->sample(r, &h.api);
    abilities({{"Func 101", 2, 1, 0, "Help zero."},
               {"Func 102", 3, 1, 0, "Help one."},
               {"Func 103", 4, 1, 1, "Help two."},
               {"Func 104", 6, 0, 0, "Help two."},
               {"Func 105", 7, 0, 0, "Help two."},
               {"Func 106", 8, 0, 0, "Help two."},
               {"Func 107", 9, 0, 0, "Help two."},
               {"Func 108", 10, 0, 0, "Help two."}},
              "reveal flag");
    // Names-only flag (0x227): later ranks listed without descriptions (help -1).
    SetFlag(h, 0x23f, false);
    SetFlag(h, 0x227, true);
    SetFlag(h, 0x100000f0, true);
    for (int i = 0; i < 9; ++i)
        m->sample(r, &h.api);
    abilities({{"Func 101", 2, 1, 0, "Help zero."},
               {"Func 102", 3, 1, 0, "Help one."},
               {"Func 103", 4, 1, 1, "Help two."},
               {"Func 104", 6, 0, 0, "Help two."},
               {"Func 105", 7, 0, 0, ""},
               {"Func 106", 8, 0, 0, ""},
               {"Func 107", 9, 0, 0, ""},
               {"Func 108", 10, 0, 0, ""},
               {"Func 109", 10, 0, 0, ""}},
              "names-only flag + flag-gated entry");
    // id 12 name/portrait switch on flag 0x4000009B (section 4).
    SetFlag(h, 0x4000009b, true);
    check(ext->on_action(r, "confidant_select", 1), "select accepted");
    check(!ext->on_action(r, "confidant_select", 24) && !ext->on_action(r, "other", 1),
          "bad selections rejected");
    run();
    check(h.nums["confidant.sel.index"] == 1 && h.nums["confidant.sel.id"] == 12 &&
              h.texts["confidant.sel.person"] == "Extra Two" &&
              h.texts["confidant.1.portrait_key"] == "c_chara_33" &&
              h.nums["confidant.sel.ability.count"] == 0 && h.nums["confidant.sel.ready"] == 1,
          "switched name + selection");
    // Selection past the list clamps to the last row.
    check(ext->on_action(r, "confidant_select", 9), "select 9");
    run();
    check(h.nums["confidant.sel.index"] == 2 && h.nums["confidant.sel.id"] == 15, "clamped");
    // In-place registered form (987BB0 -> EE0790, seen live after opening the camp menu):
    // little-endian "MSG1", self-relative offsets. Same texts must come out.
    {
        auto be = [&](U at) {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
                v = v << 8 | h.ram[at + i];
            return v;
        };
        const uint32_t count = be(HelpBmd + 0x18), limit = be(HelpBmd + 0x10);
        for (uint32_t i = 0; i < count; ++i) {
            const U entry = HelpBmd + 0x20 + 8 * i;
            const U rec = HelpBmd + 0x20 + be(entry + 4);
            const U page = HelpBmd + 0x20 + be(rec + 0x1c);
            h.put(entry, uint32_t(0));
            h.put(entry + 4, int32_t(rec - (entry + 4)));
            h.put(rec + 0x18, uint16_t(1));
            h.put(rec + 0x1c, int32_t(page - (rec + 0x1c)));
        }
        h.put(HelpBmd + 8, uint32_t(0x3147534d));
        h.put(HelpBmd + 0x10, limit);
        h.put(HelpBmd + 0x18, count);
        check(ext->on_action(r, "confidant_select", 0), "select 0 (LE)");
        run();
        check(h.texts["confidant.sel.ability.0.desc"] == "Help zero." &&
                  h.texts["confidant.sel.story"] == "Help one.",
              "registered little-endian BMD form");
    }
    // Broken help table: descriptions clear, the list stays.
    check(ext->on_action(r, "confidant_select", 0), "select 0");
    h.put(HelpBmd + 8, uint32_t(0));
    run();
    check(h.nums["confidant.sel.ready"] == 1 && h.texts["confidant.sel.ability.0.desc"].empty() &&
              h.texts["confidant.sel.ability.0.name"] == "Func 101",
          "help table rejected, names kept");
    // Name table count mismatch: rows and detail hide, arcana list stays.
    h.put(CtdName + 0x28, uint32_t(37));
    run();
    check(h.nums["confidant.ready"] == 1 && !h.nums["confidant.0.detail"] &&
              h.texts["confidant.0.person"].empty() && !h.nums["confidant.sel.ready"],
          "name CTD rejected");
    h.put(CtdName + 0x28, uint32_t(38));
    // Accessor signature mismatch disables only the detail.
    h.put(h.base + 0x86b170, uint32_t(0xd503201f));
    m->destroy(r);
    r = OpenAll(m->create(&h.api, "{}"));
    run();
    check(h.nums["confidant.ready"] == 1 && !h.nums["confidant.sel.ready"] &&
              !h.nums["confidant.0.detail"],
          "menu signature mismatch isolated");
    m->destroy(r);
}

// ---- Inventory / equipment / party status (p5r_inventory_reader.h) ----
// Accessor words: the instruction words the inventory reader matches or decodes (the words it
// does not check are NOP). Table contents are synthetic.
// Skill icon kind -> ICON.DDS cell rows {u16, s8 column, s8 row} (synthetic: kind k -> cell k + 6).
constexpr size_t IconKinds = 81;
std::vector<uint8_t> IconTable() {
    std::vector<uint8_t> t(4 * IconKinds);
    for (size_t k = 0; k < IconKinds; ++k) {
        t[4 * k] = uint8_t(k);
        t[4 * k + 1] = 0x90;
        t[4 * k + 2] = uint8_t(k % 6);
        t[4 * k + 3] = uint8_t(k / 6 + 1);
    }
    return t;
}

constexpr U InvHeap = 0x95000000;
struct InvLayout {
    U rec[9]{}, names[9]{}, bmd{}, kinds{}, flag0{}, flag4{}, hpsp{}, exp{}, persona{}, skills{};
};
// Category -> record table slot / count / name-table order used by the D4B1 getters.
constexpr std::array<U, 9> RecSlot{0x22ab580, 0x22ab588, 0x22ab590, 0x22ab598, 0x22ab5a0,
                                   0x22ab5a8, 0x22ab5b8, 0x22ab5b0, 0x22ab5c0};
constexpr std::array<U, 9> RecCount{0x22ab4e0, 0x22ab4e4, 0x22ab4e8, 0x22ab4ec, 0x22ab4f0,
                                    0x22ab4f4, 0x22ab4fc, 0x22ab4f8, 0x22ab500};
constexpr std::array<U, 9> NameSlot{0x22ab678, 0x22ab688, 0x22ab698, 0x22ab6a8, 0x22ab6b8,
                                    0x22ab6c8, 0x22ab6e8, 0x22ab6d8, 0x22ab6f8};
constexpr std::array<uint32_t, 9> Stride{0x30, 0x30, 0x40, 0x30, 0xc, 0x2c, 0x18, 0x20, 0x84};
U InvRecord(const InvLayout& l, uint16_t id) {
    return l.rec[id >> 12] + (id & 0xfff) * Stride[id >> 12];
}
InvLayout InstallInventory(Host& h) {
    h.code(0x8808a0, {0x9000cf80, 0x912d5000, 0xd65f03c0});
    h.code(0x88cf70, {0xf000a7a8, 0x911b6108, 0x530c3c09, 0xf8695901, 0x12002c00, 0xd61f0020});
    h.code(0x88cf30, {0xf000a7a8, 0x911a4108, 0x92403c09, 0xf8697900, 0xd61f0000});
    h.code(0x88bfa0, {0x9000a7c8, 0x9116e108, 0x530c3c09, 0xf8695901, 0x12002c00, 0xd61f0020});
    h.code(0x89beb0,
           {0x9000d088, 0x911be108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100, 0xd65f03c0});
    h.code(0x7ec9e0, {0xf000ac88, 0x91014108, 0x8b20d108, 0xf9400500, 0xd503201f});
    h.code(0x7eca00, {0xf000ac88, 0x91014108, 0x8b20d108, 0xb9400500, 0xd503201f});
    h.code(0x7ec9d0, {0x52800100, 0xd65f03c0});
    h.code(0x881550, {0xb000d020, 0x912d0000, 0xd65f03c0});
    h.code(0x89b450, {0xf000cb6a, 0xf940914a, 0x93403c28, 0x52800589, 0xd503201f, 0x8b204908,
                      0x785d4100, 0xd503201f});
    h.code(0x89b430, {0xf000cb68, 0xf9408d08});
    h.code(0x89b6f0, {0xf000cb6a, 0xf940b94a});
    h.code(0x89b720, {0xf000cb6a, 0xf940bd4a});
    h.code(0x89b6b0, {0xf000cb69, 0xf940b129});
    h.code(0x89b520, {0xf000cb69, 0xf940a529, 0x92403c08, 0x5280060a});
    h.code(0x987da0, {0xd000a32a, 0x9112c14a, 0xd503201f, 0x93403c0b, 0xf86b794a});
    h.code(0x987be4, {0xd000a328, 0x9104d108});
    h.code(0xee3ff0,
           {0xf000a1a8, 0x911f6108, 0x93407c09, 0x8b091908, 0xf9402508, 0xf9400508, 0xd503201f});
    h.code(0xec4ac0, {0x71027c1f, 0xd503201f, 0xd503201f, 0xd503201f, 0x2a0003e8, 0xb0003d69,
                      0x9129e129, 0xd37ef508, 0x78686920, 0xd503201f});
    h.code(0xce4880, {0xd000a928, 0xf944e908, 0xb9400100, 0xd65f03c0});
    h.code(0xce1a10, {0xb000a948, 0xf944e108, 0xb9400100, 0xd65f03c0});
    h.code(0x88bd44, {0x9000a7c8, 0x9138a108});
    h.code(0x88bd68, {0x9000a7c9, 0x913a0129});
    h.code(0x88bd54, {0x9000a7c8, 0x913b6108});
    h.code(0x88bd60, {0x9000a7c8, 0x913cc108});
    h.code(0x88bd74, {0x9000a7c8, 0x913e2108});
    h.code(0x88bd80, {0x9000a7c8, 0x913f8108});
    h.code(0x88bd8c, {0xb000a7c8, 0x9100e108});
    h.code(0x88bd98, {0xb000a7c8, 0x91024108});
    h.code(0x88cf90, {0xa9bf7bfd, 0x910003fd, 0x94003a4e, 0xb9400000, 0xd503201f});
    h.code(0x88cfb0, {0xa9bf7bfd, 0x910003fd, 0x94003a52, 0xb9400000, 0xd503201f});
    h.code(0x88cfd0, {0xa9bf7bfd, 0x910003fd, 0x94003a56, 0xb9400000, 0xd503201f});
    h.code(0x88cff0, {0xa9bf7bfd, 0x910003fd, 0x94003a5a, 0xb9400000, 0xd503201f});
    h.code(0x88d010, {0xa9bf7bfd, 0x910003fd, 0x94003a5e, 0xb9400000, 0xd503201f});
    h.code(0x88d030, {0xa9bf7bfd, 0x910003fd, 0x94003a62, 0xb9400000, 0xd503201f});
    h.code(0x88d050, {0xa9bf7bfd, 0x910003fd, 0x94003a72, 0xb9400000, 0xd503201f});
    h.code(0x88d070, {0xa9bf7bfd, 0x910003fd, 0x94003a5e, 0xb9400000, 0xd503201f});
    h.code(0x88d090, {0xa9bf7bfd, 0x910003fd, 0x94003a6e, 0xb9400000, 0xd503201f});
    h.code(0x89b8d0, {0x9000d088, 0xf942c108, 0x92403c09, 0x5280060a, 0x9b0a2120});
    h.code(0x89b900, {0x9000d088, 0xf942c508, 0x92403c09, 0x5280060a, 0x9b0a2120});
    h.code(0x89b930, {0x9000d088, 0xf942c908, 0x92403c09, 0x8b091900, 0xd503201f});
    h.code(0x89b960, {0x9000d088, 0xf942cd08, 0x92403c09, 0x5280060a, 0x9b0a2120});
    h.code(0x89b990, {0x9000d088, 0xf942d108, 0x92403c09, 0x5280018a, 0x9b0a2120});
    h.code(0x89b9c0, {0x9000d088, 0xf942d508, 0x92403c09, 0x5280058a, 0x9b0a2120});
    h.code(0x89ba20, {0x9000d088, 0xf942dd08, 0x92403c09, 0x5280030a, 0x9b0a2120});
    h.code(0x89b9f0, {0x9000d088, 0xf942d908, 0x92403c09, 0x8b091500, 0xd503201f});
    h.code(0x89ba50, {0x9000d088, 0xf942e108, 0x92403c09, 0x5280108a, 0x9b0a2120});
    h.code(0x89b8f0, {0x9000d088, 0x7949c100, 0xd65f03c0});
    h.code(0x89b920, {0x9000d088, 0x7949c900, 0xd65f03c0});
    h.code(0x89b950, {0x9000d088, 0x7949d100, 0xd65f03c0});
    h.code(0x89b980, {0x9000d088, 0x7949d900, 0xd65f03c0});
    h.code(0x89b9b0, {0x9000d088, 0x7949e100, 0xd65f03c0});
    h.code(0x89b9e0, {0x9000d088, 0x7949e900, 0xd65f03c0});
    h.code(0x89ba40, {0x9000d088, 0x7949f900, 0xd65f03c0});
    h.code(0x89ba10, {0x9000d088, 0x7949f100, 0xd65f03c0});
    h.code(0x89ba70, {0x9000d088, 0x794a0100, 0xd65f03c0});
    h.code(0x89bdb0,
           {0x9000d088, 0x9119e108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100, 0xd65f03c0});
    h.code(0x89bdd0,
           {0x9000d088, 0x911a2108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100, 0xd65f03c0});
    h.code(0x89bdf0,
           {0x9000d088, 0x911a6108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100, 0xd65f03c0});
    h.code(0x89be10,
           {0x9000d088, 0x911aa108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100, 0xd65f03c0});
    h.code(0x89be30,
           {0x9000d088, 0x911ae108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100, 0xd65f03c0});
    h.code(0x89be50,
           {0x9000d088, 0x911b2108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100, 0xd65f03c0});
    h.code(0x89be90,
           {0x9000d088, 0x911ba108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100, 0xd65f03c0});
    h.code(0x89be70,
           {0x9000d088, 0x911b6108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100, 0xd65f03c0});

    h.code(0x89bd30,
           {0x9000d088, 0x9118a108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100, 0xd65f03c0});
    {
        const U strings = 0x94f00000, offsets = 0x94f00100;
        h.put(h.base + 0x22ab628, std::array<U, 2>{strings, offsets});
        h.put(offsets + 2 * 0x10, uint16_t(0));
        h.put(strings, std::array<char, 7>{'P', 's', 'n', 'O', 'n', 'e', 0});
    }
    h.code(0x7ed190, {0xd000ac88, 0x91034108, 0x8b20d108, 0xf9400500});
    h.code(0x7ed1b0, {0xd503201f, 0xd503201f, 0x8b20d108, 0xb9400500});
    h.code(0x7c4980, {0xd00070e8, 0x913ae108, 0x8b150908, 0x91005109});
    h.code(0x7c4a90, {0xd00070e9, 0x913f4129, 0xb8687928, 0xd503201f});
    h.code(0x8426f4, {0xb0006d28, 0x911aa908, 0x92403e69, 0x78697901});
    h.put(h.base + 0x15e2eb8, std::array<int32_t, 10>{0, 1, 2, 3, 4, 0, 2, 3, 4, -1});
    h.put(h.base + 0x15e2fd0, std::array<uint32_t, 5>{0, 8, 1, 2, 7});
    h.put(h.base + 0x15e76aa, std::array<uint16_t, 5>{0, 4, 1, 2, 3});
    for (unsigned k = 0; k < 5; ++k)
        h.put(h.base + 0x1d7f0d0 + 16 * k, std::array<U, 2>{0, 0});
    h.code(0x7df17c, {0xd000d1c0, 0x91018000, 0x528a5e01, 0x72a000a1});
    h.code(0x7de620, {0x5289a209, 0x72a000a9, 0xd503201f});
    for (U i = 0; i < 0x38; ++i)
        h.ram[h.base + 0x2219060 + i] = 0;
    const std::array<U, 9> leaf{0x88cf90, 0x88cfb0, 0x88cfd0, 0x88cff0, 0x88d010,
                                0x88d030, 0x88d050, 0x88d070, 0x88d090};
    const std::array<U, 9> cnt{0x89b8f0, 0x89b920, 0x89b950, 0x89b980, 0x89b9b0,
                               0x89b9e0, 0x89ba40, 0x89ba10, 0x89ba70};
    const std::array<U, 8> nm{0x89bdb0, 0x89bdd0, 0x89bdf0, 0x89be10,
                              0x89be30, 0x89be50, 0x89be90, 0x89be70};
    for (unsigned c = 0; c < 9; ++c) {
        h.put(h.base + 0x1d836d8 + 8 * c, h.base + leaf[c]);
        h.put(h.base + 0x1d83690 + 8 * c, h.base + cnt[c]);
        if (c < 8)
            h.put(h.base + 0x1d835b8 + 8 * c, h.base + nm[c]);
    }
    InvLayout l;
    U heap = InvHeap;
    for (unsigned c = 0; c < 9; ++c) {
        l.rec[c] = heap;
        heap += 0x4000;
        h.put(h.base + RecSlot[c], l.rec[c]);
        h.put(h.base + RecCount[c], uint16_t(16));
        for (U i = 0; i < 16 * Stride[c]; ++i)
            h.ram[l.rec[c] + i] = 0;
        // Name table {char* base, u16* offsets}: "C<c>I<i>" for every index.
        const U strings = heap, offsets = heap + 0x200;
        heap += 0x400;
        for (unsigned i = 0; i < 16; ++i) {
            const std::string s = "C" + std::to_string(c) + "I" + std::to_string(i);
            h.put(offsets + 2 * i, uint16_t(i * 8));
            for (size_t k = 0; k <= s.size(); ++k)
                h.ram[strings + i * 8 + k] = k < s.size() ? uint8_t(s[k]) : 0;
        }
        h.put(h.base + NameSlot[c], std::array<U, 2>{strings, offsets});
        l.names[c] = strings;
    }
    // Counts + recent slots, Iwai levels (all 0).
    for (U i = 0; i < 0x1a78; ++i)
        h.ram[h.base + 0x2270b54 + i] = 0;
    for (U i = 0; i < 0x100; ++i)
        h.ram[h.base + 0x2286b40 + i] = 0;
    // Help handles: category -> slot (1DED4B0) -> handle (1DED134 array).
    constexpr std::array<unsigned, 9> HelpSlot{0, 2, 3, 5, 6, 8, 10, 11, 1};
    for (unsigned c = 0; c < 9; ++c) {
        h.put(h.base + 0x1ded4b0 + 8 * c, h.base + 0x1ded134 + 4 * HelpSlot[c]);
        h.put(h.base + 0x1ded134 + 4 * HelpSlot[c], int32_t(-1));
    }
    // One help BMD (consumables, handle 5): 2 messages, message 1 = "Restores 20 HP.\n".
    l.bmd = heap;
    heap += 0x1000;
    const U object = heap;
    heap += 0x100;
    h.put(h.base + 0x1ded134 + 4 * 5, uint32_t(5));
    h.put(h.base + 0x231a7d8 + 5 * 0x40 + 0x48, object);
    h.put(object + 8, l.bmd);
    const std::vector<uint8_t> text{0xf2, 0x05, 0xff, 0xff, 0xf1, 0x41, 'R', 'e',
                                    's',  't',  'o',  'r',  'e',  's',  ' ', '2',
                                    '0',  ' ',  'H',  'P',  '.',  '\n', 0};
    for (U i = 0; i < 0x200; ++i)
        h.ram[l.bmd + i] = 0;
    h.put(l.bmd + 4, uint32_t(0x200));
    h.put(l.bmd + 8, uint32_t(0x3147534d));
    h.put(l.bmd + 0x18, uint32_t(2));
    const U rec1 = l.bmd + 0x40;
    h.put(l.bmd + 0x28, uint32_t(0));
    h.put(l.bmd + 0x2c, uint32_t(rec1 - (l.bmd + 0x2c)));
    h.put(rec1 + 0x18, uint16_t(1));
    h.put(rec1 + 0x1c, uint32_t(0x40));
    PutBytes(h, rec1 + 0x1c + 0x40, text);
    // Flag sections (1D834C8): section 0 (0x856 trait flag) and 4 (party roster flags).
    l.flag0 = heap;
    heap += 0x200;
    l.flag4 = heap;
    heap += 0x100;
    for (U i = 0; i < 0x180; ++i)
        h.ram[l.flag0 + i] = 0;
    for (U i = 0; i < 0x40; ++i)
        h.ram[l.flag4 + i] = 0;
    h.put(h.base + 0x1d834c8, std::array<U, 2>{l.flag0, 0xc00});
    h.put(h.base + 0x1d834c8 + 64, std::array<U, 2>{l.flag4, 0x200});
    // Stat tables.
    l.hpsp = heap;
    heap += 0x2000;
    l.exp = heap;
    heap += 0x200;
    l.persona = heap;
    heap += 0x1000;
    l.skills = heap;
    heap += 0x2000;
    h.put(h.base + 0x220a120, l.hpsp);
    h.put(h.base + 0x220a118, l.exp);
    h.put(h.base + 0x220a160, l.persona);
    h.put(h.base + 0x220a148, l.skills);
    h.put(h.base + 0x220a170, U(0)); // growth tables unused (non-party personas)
    h.put(h.base + 0x220a178, U(0));
    PutBytes(h, h.base + 0x1671a78, IconTable());
    // Language slots (dialogue reader shares them).
    h.put(h.base + 0x220a9d0, heap);
    h.put(heap, uint32_t(1));
    h.put(h.base + 0x220a9c0, heap + 8);
    h.put(heap + 8, uint32_t(0));
    heap += 0x10;
    // Kinds table: 8 x {mask, ranges, ptr}.
    l.kinds = heap;
    heap += 0x100;
    constexpr std::array<uint32_t, 8> Masks{0x400000,  0x1000000,  0x2000000, 0x40000000,
                                            0x8000000, 0x10000000, 0x800000,  0x20000000};
    for (unsigned k = 0; k < 8; ++k) {
        h.put(h.base + 0x1d7f050 + 16 * k, Masks[k]);
        h.put(h.base + 0x1d7f050 + 16 * k + 4, uint32_t(0));
        h.put(h.base + 0x1d7f050 + 16 * k + 8, U(0));
    }
    return l;
}
void InvKind(Host& h, U& heap, unsigned k, std::initializer_list<std::pair<uint16_t, uint16_t>> r) {
    h.put(h.base + 0x1d7f050 + 16 * k + 4, uint32_t(r.size()));
    h.put(h.base + 0x1d7f050 + 16 * k + 8, heap);
    for (const auto& [lo, hi] : r) {
        h.put(heap, lo);
        h.put(heap + 2, hi);
        heap += 4;
    }
}
int IconCellOf(unsigned icon) {
    const auto t = IconTable();
    return t[4 * icon + 3] * 6 + t[4 * icon + 2];
}
void InventoryTests(const EdenDsmodModuleApi* m) {
    Host h;
    const InvLayout l = InstallInventory(h);
    for (unsigned id = 1; id <= 4; ++id)
        for (U i = 0; i < 0x2a0; ++i)
            h.ram.emplace(h.base + 0x226ee60 + 0x2a0 * id + i, uint8_t(0));
    U heap = 0x96000000;
    auto count = [&](uint16_t id, uint8_t n) {
        constexpr std::array<U, 9> Base{0,     0x400,  0x800,  0xa00, 0xe00,
                                        0xf00, 0x1300, 0x1700, 0x1900};
        h.ram[h.base + 0x2270b54 + Base[id >> 12] + (id & 0xfff)] = n;
    };
    auto flags = [&](uint16_t id, uint32_t f) { h.put(InvRecord(l, id), f); };
    // Items: kind0 0x3001, kind4 0x3002, kind7 0x3003, kind2 0x6005, kind6 0x4001, kind1 0x5002.
    flags(0x3001, 0x400000);
    h.put(InvRecord(l, 0x3001) + 0xc, uint16_t(0x10)); // consumable skill
    for (U i = 0; i < 0x30; ++i)
        h.ram[l.skills + 0x10 * 0x30 + i] = 0;
    h.put(l.skills + 0x10 * 0x30 + 0x1a, uint8_t(2)); // skill type -> icon 0x31
    h.put(InvRecord(l, 0x3001) + 0xa, uint16_t(2));   // use bit (88D470 bit 1): not dimmed
    h.put(l.skills + 0x10 * 0x30 + 4, uint8_t(1));    // skill usable in the field (896CA0)
    flags(0x3002, 0x8000000);                         // tool, no use bit: dimmed
    flags(0x3006, 0x8000000);                         // tool with the use bit
    h.put(InvRecord(l, 0x3006) + 0xa, uint16_t(2));
    flags(0x3003, 0x20000000);
    h.put(InvRecord(l, 0x3003) + 0xa, uint16_t(1 << 5)); // bit29 kind -> icon 0x2b
    flags(0x6005, 0x2000000);
    h.put(InvRecord(l, 0x6005) + 0xa, uint16_t(0x20)); // skill card: teaches skill 0x20
    flags(0x0005, 0x1); // equippable by nobody who joined: not in the recent tab
    h.put(InvRecord(l, 0x0005) + 8, uint32_t(1u << 9));
    count(0x0005, 1);
    flags(0x4001, 0x800000);
    flags(0x5002, 0x1000000);
    flags(0x0003, 0x1); // a melee weapon (equipment flag) only member 2 can equip
    h.put(InvRecord(l, 0x0003) + 8, uint32_t(1u << 2));
    count(0x3001, 3);
    count(0x3002, 1);
    count(0x3006, 1);
    count(0x3003, 2);
    count(0x6005, 1);
    count(0x4001, 1);
    count(0x5002, 5);
    count(0x0003, 1);
    count(0x3004, 7); // in no kind range: never listed
    InvKind(h, heap, 0, {{0x3001, 0x3001}});
    InvKind(h, heap, 1, {{0x5002, 0x5002}});
    InvKind(h, heap, 2, {{0x6005, 0x6005}});
    InvKind(h, heap, 4, {{0x3002, 0x3002}, {0x3006, 0x3006}});
    InvKind(h, heap, 6, {{0x4001, 0x4001}});
    InvKind(h, heap, 7, {{0x3003, 0x3003}});
    const U recent = h.base + 0x2270b54 + 0x1a00;
    h.put(recent, uint32_t(0x3002 | 1u << 16));
    h.put(recent + 4, uint32_t(0x6005));
    h.put(recent + 8, uint32_t(0x0003));
    h.put(recent + 12, uint32_t(0x3004 + 0x100)); // count 0: dropped
    h.put(recent + 16, uint32_t(0x0005));
    h.put(l.flag4 + 4, uint32_t(1u << 16)); // 0x40000030: member 2 joined
    // Equipment: protagonist (unit 1).
    const U u1 = h.base + 0x226ee60 + 0x2a0;
    h.put(u1 + 0x284, std::array<uint16_t, 5>{0x0002, 0x1003, 0x2004, 0x7000, 0x8001});
    h.put(InvRecord(l, 0x0002) + 0x10, uint16_t(60));
    h.put(InvRecord(l, 0x0002) + 0x12, uint16_t(90));
    h.put(InvRecord(l, 0x0002) + 0x1a, uint16_t(1)); // HP +10 effect
    h.put(InvRecord(l, 0x1003) + 0xe, uint16_t(30));
    h.put(InvRecord(l, 0x1003) + 0x10, uint16_t(5));
    h.put(InvRecord(l, 0x1003) + 0x18, uint16_t(7));     // SP +10 effect
    h.put(InvRecord(l, 0x2004) + 0x14, uint16_t(0x409)); // +10% HP skill
    h.put(InvRecord(l, 0x8001) + 0x10, uint16_t(40));
    h.put(InvRecord(l, 0x8001) + 0x12, uint16_t(80));
    h.put(InvRecord(l, 0x8001) + 0x14, uint16_t(8));
    h.put(u1 + 0x1c, uint32_t(1000));
    h.put(u1 + 0x29c, int16_t(5));
    h.put(u1 + 0x29e, int16_t(0));
    for (unsigned id = 1; id <= 4; ++id) {
        const U u = h.base + 0x226ee60 + 0x2a0 * id;
        h.put(u + 0x44 + 0x60 + 2, uint16_t(0x10)); // persona id (formula EXP path)
        if (id != 1) {
            h.put(u + 0x284, std::array<uint16_t, 5>{0x0001, 0x1001, 0x2000, 0x7000, 0x8000});
            h.put(u + 0x44 + 0x60 + 8, uint32_t(100));
            h.put(u + 0x29c, int16_t(0));
            h.put(u + 0x29e, int16_t(0));
        }
        const unsigned level = id == 1 ? 14 : 17 + id;
        h.put(l.hpsp + (level - 1) * 0x2c + id * 4, std::array<uint16_t, 2>{100, 50});
    }
    h.put(l.persona + 0x10 * 0xe + 3, uint8_t(10));
    h.put(l.persona + 0x10 * 0xe + 2, uint8_t(18));
    h.put(l.exp + 4 * 14, uint32_t(1500));
    h.put(h.base + 0x1d7f0d0 + 4, uint32_t(1));
    h.put(h.base + 0x1d7f0d0 + 8, heap);
    h.put(heap, std::array<uint16_t, 2>{0x0001, 0x0005});
    heap += 4;
    count(0x0004, 2); // equippable by the protagonist
    flags(0x0004, 0x1);
    h.put(InvRecord(l, 0x0004) + 8, uint32_t(1u << 1));
    InstallRoster(h, {2, 3, 4});
    void* r = OpenAll(m->create(&h.api, "{}"));
    check(r, "inventory create");
    m->sample(r, &h.api);
    check(!h.writes, "inventory read only");
    check(h.nums["live.ready"] == 1 && h.nums["item.ready"] == 1, "item.ready");
    check(h.nums["item.tab.count"] == 6, "six native tabs");
    auto row = [&](unsigned t, unsigned n, const char* f) {
        return "item.tab." + std::to_string(t) + "." + std::to_string(n) + "." + f;
    };
    check(h.nums["item.tab.0.count"] == 3 && h.nums[row(0, 0, "id")] == 0x3002 &&
              h.nums[row(0, 0, "new")] == 1 && h.nums[row(0, 1, "id")] == 0x6005 &&
              h.nums[row(0, 2, "id")] == 0x0003,
          "recent tab: slot order, zero count dropped, equipment via joined member");
    check(h.nums["item.tab.1.count"] == 1 && h.nums[row(1, 0, "id")] == 0x3001 &&
              h.nums[row(1, 0, "qty")] == 3 && h.texts[row(1, 0, "name")] == "C3I1",
          "items tab");
    // Row descriptions are published only for the selected row (item.sel.desc).
    check(!h.texts.count(row(1, 0, "desc")), "no per-row help text");
    check(eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH)
              ->on_action(r, "item_select", 1000),
          "select items row");
    m->sample(r, &h.api);
    check(h.texts["item.sel.desc"] == "Restores 20 HP." && h.nums["item.sel.key"] == 1000,
          "help text from BMD (selected row)");
    check(h.nums[row(1, 0, "icon")] == IconCellOf(0x31), "consumable skill icon");
    check(h.nums["item.tab.2.count"] == 2 && h.nums[row(2, 0, "id")] == 0x3002 &&
              h.nums[row(2, 1, "id")] == 0x3006,
          "tools tab");
    check(h.nums["item.tab.3.count"] == 1 && h.nums[row(3, 0, "id")] == 0x6005, "skill cards");
    check(h.nums["item.tab.4.count"] == 1 && h.nums[row(4, 0, "qty")] == 5, "treasure tab");
    check(h.nums["item.tab.5.count"] == 2 && h.nums[row(5, 0, "id")] == 0x4001 &&
              h.nums[row(5, 1, "id")] == 0x3003 && h.nums[row(5, 1, "icon")] == IconCellOf(0x2b),
          "key items tab: kind order");
    // Native row state: grey = the list's dimmed style (7D92A0: flags & 0x08400000 without the
    // use bit), usable = the A handler's item rules (7DC850).
    auto state = [&](unsigned t, unsigned n, int grey, int usable) {
        return h.nums.count(row(t, n, "grey")) && h.nums[row(t, n, "grey")] == grey &&
               h.nums[row(t, n, "usable")] == usable;
    };
    check(state(1, 0, 0, 1), "field-usable consumable: white, usable");
    check(state(2, 0, 1, 0) && state(0, 0, 1, 0), "tool without the use bit: dimmed, refused");
    check(state(3, 0, 0, 1), "skill card: white, usable");
    check(state(4, 0, 0, 0) && state(5, 0, 0, 0) && state(5, 1, 0, 0),
          "treasure / key items: white, refused (no use skill)");
    check(state(0, 2, 0, 0), "equipment row: white, refused");
    check(h.nums["item.sel.grey"] == 0 && h.nums["item.sel.usable"] == 1, "selected usable row");
    {
        auto* e = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
        check(state(2, 1, 0, 1), "tool with the use bit: white, usable");
        check(e->on_action(r, "item_tab", 2) && e->on_action(r, "item_select", 2000), "select");
        m->sample(r, &h.api);
        check(h.nums["item.sel.grey"] == 1 && h.nums["item.sel.usable"] == 0 &&
                  h.nums["item.sel.can_use"] == 0,
              "dimmed row: no USE");
        check(!e->on_action(r, "item_use", 20000) && !e->on_action(r, "item_use_sel", 0),
              "a refused row is never driven");
        check(e->on_action(r, "item_select", 2001), "select usable tool");
        m->sample(r, &h.api);
        check(h.nums["item.sel.usable"] == 1 && h.nums["item.sel.grey"] == 0, "usable row");
        check(e->on_action(r, "item_tab", -1) && e->on_action(r, "item_select", 1000), "restore");
        // the consumable's skill becomes battle-only (896CA0: +4 bit 0 clear): white, refused
        h.put(l.skills + 0x10 * 0x30 + 4, uint8_t(0));
        count(0x3001, 4);           // a count change rebuilds the lists
        for (int i = 0; i < 8; ++i) // full pass every InvSlowPeriod samples
            m->sample(r, &h.api);
        check(state(1, 0, 0, 0), "battle-only skill: white, refused");
        h.put(l.skills + 0x10 * 0x30 + 4, uint8_t(1));
        count(0x3001, 3);
        for (int i = 0; i < 8; ++i) // full pass every InvSlowPeriod samples
            m->sample(r, &h.api);
        check(state(1, 0, 0, 1) && h.nums[row(1, 0, "qty")] == 3, "restored");
    }
    check(h.nums["pstat.ready"] == 1 && h.nums["pstat.0.stats"] == 1, "party stats");
    check(h.nums["pstat.0.hp_max"] == 126, "max HP: base+effect+stored, +10% accessory skill");
    check(h.nums["pstat.0.sp_max"] == 60, "max SP: base + armor effect");
    check(h.texts["pstat.1.persona_name"] == "PsnOne" && h.nums["pstat.1.persona_arcana"] == 18 &&
              h.nums["pstat.1.persona_level"] == 19,
          "equipped persona");
    check(h.nums["pstat.0.exp"] == 1000 && h.nums["pstat.0.next"] == 500, "protagonist next");
    const float x = 20.0f, k = 10.0f * -0.019f + 3.7f;
    (void)k;
    check(h.nums["pstat.1.level"] == 19 && h.nums["pstat.1.exp"] == 100 &&
              h.nums["pstat.1.next"] > 0 && h.nums["pstat.1.next"] < int64_t(x * x * x * 4),
          "persona formula next");
    check(h.nums["equip.0.0.atk"] == 60 && h.nums["equip.0.0.acc"] == 90 &&
              h.texts["equip.0.0.name"] == "C0I2",
          "melee");
    check(h.nums["equip.0.1.def"] == 30 && h.nums["equip.0.1.eva"] == 5 &&
              !h.nums["equip.0.1.has_atk"],
          "armor");
    check(h.nums["equip.0.4.atk"] == 40 && h.nums["equip.0.4.rounds"] == 8, "gun");
    check(h.nums["pstat.0.melee"] == 60 && h.nums["pstat.0.ranged"] == 40 &&
              h.nums["pstat.0.rounds"] == 8 && h.nums["pstat.0.defense"] == 30,
          "status melee/ranged/rounds/defense");
    // Module actions (companion-side selection only).
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    check(ext && ext->on_action, "on_action export");
    check(ext->on_action(r, "equip_select", 0) && !ext->on_action(r, "equip_select", 7) &&
              !ext->on_action(r, "item_tab", 6) && !ext->on_action(r, "nope", 0),
          "action argument validation");
    m->sample(r, &h.api);
    check(h.nums["equip.cand.ready"] == 1 && h.nums["equip.cand.count"] == 2 &&
              h.nums["equip.cand.0.id"] == 0x0002 && h.nums["equip.cand.0.equipped"] == 1 &&
              h.nums["equip.cand.0.qty"] == 1 && h.nums["equip.cand.1.id"] == 0x0004 &&
              h.nums["equip.cand.1.qty"] == 2 && h.texts["equip.cand.1.name"] == "C0I4",
          "native melee candidates: equipped + equippable-by-protagonist only");
    check(ext->on_action(r, "item_tab", 1) && ext->on_action(r, "item_select", 5001), "select");
    m->sample(r, &h.api);
    check(h.nums["item.view"] == 1 && h.nums["item.tab.5.count"] == 2 &&
              !h.nums.count(row(5, 0, "id")) && h.nums.count(row(1, 0, "id")),
          "single-tab mode keeps counts, drops other rows");
    check(h.nums["item.sel.id"] == 0x3003 && h.texts["item.sel.name"] == "C3I3", "item.sel");
    check(ext->on_action(r, "item_tab", -2), "no-rows mode");
    m->sample(r, &h.api);
    check(h.nums["item.tab.1.count"] == 1 && !h.nums.count(row(1, 0, "id")), "counts only");
    check(h.nums["pdrv.pending"] == 0 && h.nums["pdrv.state"] == 0, "driver idle");
    check(ext->on_action(r, "item_use", 10000) && !ext->on_action(r, "item_use", 10000),
          "one drive at a time");
    check(!ext->on_action(r, "persona_change", 0) && !ext->on_action(r, "skill_use", 0),
          "a persona/skill drive is refused while an item drive runs");
    check(ext->on_action(r, "item_use", -1), "cancel");
    m->sample(r, &h.api);
    check(h.nums["pdrv.state"] == 3 && h.texts["pdrv.status"] == "cancelled" &&
              h.nums["pdrv.kind"] == 2 && !h.nums["pdrv.pending"],
          "cancelled");
    // ---- 0.8 lazy outputs: lists only for the companion page named by ui_open.
    auto fresh_sample = [&] {
        h.nums.clear();
        h.texts.clear();
        m->sample(r, &h.api);
    };
    check(ext->on_action(r, "ui_open", 1) && !ext->on_action(r, "ui_open", 12) &&
              !ext->on_action(r, "ui_open", -1),
          "ui_open validation");
    fresh_sample();
    check(h.nums["ui.page"] == 1 && h.nums["item.ready"] == 0 && !h.nums.count(row(1, 0, "id")) &&
              h.nums["pstat.ready"] == 0 && !h.nums.count("equip.0.0.id"),
          "hub: no item/equipment lists");
    check(ext->on_action(r, "ui_open", 3), "open ITEM");
    fresh_sample();
    check(h.nums["ui.page"] == 3 && h.nums["item.ready"] == 1 && h.nums["pstat.ready"] == 0 &&
              h.nums["item.view"] == 1 && h.nums.count(row(1, 0, "id")) &&
              !h.nums.count(row(0, 0, "id")),
          "ITEM page: opens on the native Items tab, the viewed tab's rows only");
    check(ext->on_action(r, "ui_open", 3), "reopen ITEM");
    fresh_sample();
    check(h.nums["ui.page"] == -2, "reopen republishes the page edge");
    fresh_sample();
    check(h.nums["ui.page"] == 3, "page edge");
    check(!ext->on_action(r, "item_use_sel", 4), "item_use_sel target bounds");
    check(ext->on_action(r, "item_select", 1000) && ext->on_action(r, "item_use_sel", 2),
          "item_use_sel uses the stored row");
    check(!ext->on_action(r, "item_use_sel", 2) && !ext->on_action(r, "equip_change_sel", 0),
          "refused while a drive runs");
    check(ext->on_action(r, "item_use", -1), "cancel item_use_sel");
    fresh_sample();
    check(h.nums["pdrv.busy"] == 0 && h.nums["pdrv.show"] == 1 &&
              h.texts["pdrv.text"] == "Use item: cancelled",
          "status line");
    check(ext->on_action(r, "ui_open", 6), "open STATS");
    fresh_sample();
    check(h.nums["pstat.ready"] == 1 && h.nums["item.ready"] == 0 &&
              h.nums["equip.cand.ready"] == 0,
          "STATS page: party block only");
    check(ext->on_action(r, "equip_pick", 0), "equip_pick");
    fresh_sample();
    check(h.nums["ui.page"] == 11 && h.nums["equip.cand.ready"] == 1 && h.nums["equip.sel"] == 0,
          "equip_pick opens the candidate list");
    check(h.nums["equip.cand.0.equipped"] == 1 && h.nums["equip.cand.0.can"] == 0 &&
              h.nums.count("equip.cand.1.can"),
          "equipped candidate: no change to make");
    check(!ext->on_action(r, "equip_change_sel", 0), "the equipped row is never driven");
    check(ext->on_action(r, "equip_change_sel", 1), "equip_change_sel uses the stored slot");
    check(ext->on_action(r, "equip_change", -1), "cancel equip_change_sel");
    m->sample(r, &h.api);
    // Tear: a count changing between the two reads keeps the previous lists hidden.
    m->destroy(r);
    // A signature mismatch hides only item/equip/pstat outputs.
    h.code(0x88cf30, {0});
    r = OpenAll(m->create(&h.api, "{}"));
    m->sample(r, &h.api);
    check(h.nums["live.ready"] == 1 && !h.nums["item.ready"] && !h.nums["pstat.ready"],
          "inventory signature mismatch is isolated");
    m->destroy(r);
}

// ---- DATA2 extras (p5r_data2_reader.h): header-level checks on synthetic memory ----
// Table layouts follow the D4B1 runtime copies (EN/INIT/CMPTABLE.BIN after the loader swap);
// all values are synthetic.
struct Mem {
    std::map<U, uint8_t> ram;
    template <class T>
    void put(U at, T v) {
        auto* p = reinterpret_cast<uint8_t*>(&v);
        for (size_t i = 0; i < sizeof(T); ++i)
            ram[at + i] = p[i];
    }
    void ctd(U slot, U at, uint32_t entry, uint32_t count) {
        put(slot, at);
        put(at + 4, uint32_t(0x46544430));
        put(at + 0xc, uint32_t(1));
        put(at + 0x10, uint32_t(0x20));
        put(at + 0x24, entry * count);
        put(at + 0x28, count);
    }
    bool operator()(U at, void* dst, size_t n) const {
        auto* d = static_cast<uint8_t*>(dst);
        for (size_t i = 0; i < n; ++i) {
            const auto it = ram.find(at + i);
            d[i] = it == ram.end() ? 0 : it->second;
        }
        return true;
    }
};
void Data2Tests(const EdenDsmodModuleApi* m) {
    using namespace p5r_data2;
    Roots r;
    r.month_lengths = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    check(DayIndex(r, 4, 1) == 0 && DayIndex(r, 5, 13) == 42 && DayIndex(r, 1, 31) == 305 &&
              DayIndex(r, 3, 31) == 364 && !DayIndex(r, 0, 1) && !DayIndex(r, 13, 1),
          "data2 day index (721690)");
    check(std::string(GradeLetter(0)) == "?" && std::string(GradeLetter(1)) == "D" &&
              std::string(GradeLetter(5)) == "S" && std::string(GradeLetter(6)).empty(),
          "data2 grade letters (sprites 0514..0519)");
    // Jobs (7779C0): arbeit rows {u16 id, u16, u16 weekday mask, u8 kind bits, u8, s16 name,
    // ..., s16 order @+0xC}: row1 id2 Job Two (day), row2 id1 Job One (day), row3 id3 Job Three
    // (night), row4 id4 Job Four (night, never unlocked here).
    Mem mem;
    r.jobs = true;
    r.arbeit = 0x1000, r.arbeit_names = 0x1008, r.drawoff = 0x1010, r.arbeit_records = 0x1100;
    mem.ctd(r.arbeit, 0x10000, 0x30, 5);
    const std::array<std::array<uint16_t, 5>, 5> rows{{{0, 1, 0, 0, 0},
                                                       {2, 0x7f, 1, 2, 0},
                                                       {1, 0x7f, 1, 1, 0},
                                                       {3, 0x7f, 2, 3, 0},
                                                       {4, 0x7f, 2, 4, 0}}};
    for (unsigned i = 0; i < rows.size(); ++i) {
        const U row = 0x10030 + 0x30ull * i;
        mem.put(row, rows[i][0]);
        mem.put(row + 4, rows[i][1]);
        mem.put(row + 6, uint8_t(rows[i][2]));
        mem.put(row + 8, int16_t(rows[i][3]));
        mem.put(row + 0xc, int16_t(rows[i][4]));
    }
    mem.ctd(r.arbeit_names, 0x11000, 64, 5);
    const std::array<const char*, 5> names{"NONE", "Job One", "Job Two", "Job Three", "Job Four"};
    for (unsigned i = 0; i < names.size(); ++i)
        for (size_t c = 0; names[i][c]; ++c)
            mem.put(0x11030 + 64ull * i + c, uint8_t(names[i][c]));
    // Draw-off rows {u8 month, u8 day, u8 type, u8, u32 flag}: 2/7 whole day, 2/8 night only.
    mem.ctd(r.drawoff, 0x12000, 8, 2);
    mem.put(0x12030, std::array<uint8_t, 8>{2, 7, 0, 0, 0, 0, 0, 0});
    mem.put(0x12038, std::array<uint8_t, 8>{2, 8, 2, 0, 0, 0, 0, 0});
    // Unlock records {u8 flags, u8, u8 month, u8 day} at +4 + 4*id: ids 1,2 from 4/20, id 3 from
    // 2/1.
    mem.put(r.arbeit_records + 4 + 4 * 1, std::array<uint8_t, 4>{1, 0, 4, 20});
    mem.put(r.arbeit_records + 4 + 4 * 2, std::array<uint8_t, 4>{1, 0, 4, 20});
    mem.put(r.arbeit_records + 4 + 4 * 3, std::array<uint8_t, 4>{1, 0, 2, 1});
    const p5r_social_menu::Roots flags{};
    auto names_of = [](const std::vector<Job>& jobs) {
        std::string s;
        for (const auto& j : jobs)
            s += j.name + ";";
        return s;
    };
    auto jobs = SampleJobs(mem, r, flags, 0, ~U{0}, 1, 31);
    check(jobs.ready && jobs.weekday == 2 && names_of(jobs.day_jobs) == "Job One;Job Two;" &&
              jobs.night_jobs.empty(),
          "data2 jobs 1/31 (night job unlocks 2/1)");
    jobs = SampleJobs(mem, r, flags, 0, ~U{0}, 2, 14);
    check(jobs.ready && names_of(jobs.night_jobs) == "Job Three;" && jobs.day_jobs.size() == 2,
          "data2 jobs 2/14");
    jobs = SampleJobs(mem, r, flags, 0, ~U{0}, 2, 7);
    check(jobs.ready && jobs.drawoff == 4 && jobs.day_jobs.empty() && jobs.night_jobs.empty(),
          "data2 jobs draw-off whole day");
    jobs = SampleJobs(mem, r, flags, 0, ~U{0}, 2, 8);
    check(jobs.ready && jobs.drawoff == 2 && jobs.day_jobs.size() == 2 && jobs.night_jobs.empty(),
          "data2 jobs draw-off night");
    mem.put(0x10030 + 0x30 * 3 + 0xc, int16_t(5)); // higher +0xC key sorts first (descending)
    mem.put(0x10030 + 0x30 * 3 + 6, uint8_t(3));   // Job Three in both lists
    jobs = SampleJobs(mem, r, flags, 0, ~U{0}, 2, 14);
    // exchange sort as 777D88: [One, Two, Three] -> swap(0,2) -> [Three, Two, One]
    check(names_of(jobs.day_jobs) == "Job Three;Job Two;Job One;", "data2 jobs order");
    mem.put(0x10030 + 0x30 * 3 + 4, uint16_t(0x7f & ~(1u << 2))); // not on Tuesdays
    jobs = SampleJobs(mem, r, flags, 0, ~U{0}, 2, 14);
    check(names_of(jobs.day_jobs) == "Job One;Job Two;", "data2 jobs weekday mask");
    r.jobs = false;
    check(!SampleJobs(mem, r, flags, 0, ~U{0}, 2, 14).ready, "data2 jobs unresolved");

    // Daily Log (77AF2C): u16 day records (high byte = line 0), cmmNetReportTable rows
    // {u32 category, char text[48]}; synthetic row texts ("Log row <id>") at the table's real ids
    // and categories: 1/23 two plain lines, 1/20 a red line (category 0) + a plain line,
    // 4/20 Palace (red, Morgana sticker on the even day), 12/20 line 0 only, 11/20 line 1 only.
    r.jobs = true;
    r.log = true;
    r.log_records = 0x40000, r.net_report = 0x1018;
    mem.ctd(r.net_report, 0x41000, 0x34, 224);
    const std::vector<std::pair<unsigned, std::pair<uint32_t, const char*>>> report{
        {1, {2, "Log row 1"}},
        {3, {1, "Log row 3"}},
        {51, {2, "Log row 51"}},
        {88, {0, "Log row 88"}},
        {89, {0, "Log row 89"}},
        {91, {0, "Log row 91"}},
        {99, {2, "Log row 99"}},
        {100, {2, "Log row 100"}},
        {158, {2, "Log row 158"}},
        {219, {2, "Log row 219"}}};
    for (const auto& [id, row] : report) {
        const U at = 0x41030 + 0x34ull * id;
        mem.put(at, row.first);
        for (size_t c = 0; row.second[c]; ++c)
            mem.put(at + 4 + c, uint8_t(row.second[c]));
    }
    auto day_record = [&](int month, int day, uint16_t raw) {
        mem.put(r.log_records + 2ull * *DayIndex(r, month, day), raw);
    };
    day_record(1, 23, 0x6463);
    day_record(1, 20, 0x5963);
    day_record(4, 20, 0x5833);
    day_record(12, 20, 0xdb00);
    day_record(11, 20, 0x009e);
    day_record(1, 21, 0x5963); // odd day: Mementos bus sticker
    day_record(12, 24, 0x5b00);
    day_record(12, 22, 0x5b58); // Treasure wins over Palace: Done!! + Morgana (even day)
    day_record(2, 2, 0xf000);   // id past the table's 224 rows -> row 1 (86B330)
    auto log = SampleDailyLog(mem, r, 1, 23);
    check(log.ready && log.line[0].text == "Log row 100" &&
              log.line[1].text == "Log row 99" && !log.line[0].red && !log.bus &&
              !log.morgana && !log.done,
          "data2 daily log 1/23");
    log = SampleDailyLog(mem, r, 1, 20);
    check(log.ready && log.line[0].text == "Log row 89" && log.line[0].red &&
              !log.line[1].red && !log.bus,
          "data2 daily log 1/20 (red Mementos row, no bus on an even day)");
    check(SampleDailyLog(mem, r, 1, 21).bus, "data2 daily log bus sticker on odd days");
    log = SampleDailyLog(mem, r, 4, 20);
    check(log.ready && log.line[0].red && log.line[1].text == "Log row 51" && log.morgana &&
              !log.done,
          "data2 daily log 4/20 Palace (Morgana)");
    log = SampleDailyLog(mem, r, 12, 20);
    check(log.ready && log.line[0].text == "Log row 219" && log.line[1].id == 0 &&
              log.line[1].text.empty(),
          "data2 daily log 12/20 one line");
    log = SampleDailyLog(mem, r, 11, 20);
    check(log.ready && log.line[0].id == 0 && log.line[1].text == "Log row 158",
          "data2 daily log 11/20 second line only");
    log = SampleDailyLog(mem, r, 12, 24);
    check(log.done && !log.morgana, "data2 daily log 12/24 Treasure: Done!! without Morgana");
    log = SampleDailyLog(mem, r, 12, 22);
    check(log.done && log.morgana, "data2 daily log Treasure on an even day");
    check(SampleDailyLog(mem, r, 2, 2).line[0].text == "Log row 1",
          "data2 daily log id past the table reads row 1");
    log = SampleDailyLog(mem, r, 3, 30);
    check(log.ready && !log.line[0].id && !log.line[1].id, "data2 daily log empty day");
    mem.put(0x41030 + 0x34ull * 100 + 4, uint8_t(0x01)); // unreadable text fails closed
    check(!SampleDailyLog(mem, r, 1, 23).ready, "data2 daily log bad text fails closed");
    r.log = false;
    check(!SampleDailyLog(mem, r, 1, 20).ready, "data2 daily log unresolved");
    // Month grid (7775E0 / 77CE78) and the cursor day's plan list (7777F0), December as in the
    // game's cmpCalTable: 12/23 holiday; 12/31 holiday row + "Winter Break" row (vacation 1):
    // the grid keeps 12/31 unred because the vacation bit wins; 12/17 lists the deadline first.
    {
        namespace cal = p5r_social_menu::calendar;
        cal::Roots c;
        c.table = 0x5000, c.names = 0x5008, c.weather_days = 0x5010;
        const U date = 0x5020;
        mem.put(date, uint16_t(266)); // 12/23
        mem.put(date + 2, uint8_t(3));
        struct Row {
            uint16_t month, day, name;
            uint8_t hol, brk;
            int16_t kind;
            uint16_t mark;
        };
        const std::vector<Row> rows{{12, 17, 1, 0, 0, 0, 0}, {12, 17, 2, 0, 0, 1, 0},
                                    {12, 23, 3, 1, 0, 0, 0}, {12, 31, 4, 1, 0, 0, 0},
                                    {12, 31, 5, 0, 1, 0, 2}, {1, 2, 6, 1, 0, 0, 0}};
        mem.ctd(c.table, 0x50000, 20, uint32_t(rows.size()));
        for (size_t i = 0; i < rows.size(); ++i) {
            const U at = 0x50030 + 20 * i;
            mem.put(at, rows[i].month);
            mem.put(at + 2, rows[i].day);
            mem.put(at + 8, rows[i].name);
            mem.put(at + 0xa, rows[i].hol);
            mem.put(at + 0xb, rows[i].brk);
            mem.put(at + 0xc, rows[i].kind);
            mem.put(at + 0xe, rows[i].mark);
        }
        mem.ctd(c.names, 0x51000, 64, 70);
        const std::array<const char*, 6> plan{"",        "Event A", "Deadline B",
                                              "Emperor", "Eve",     "Break"};
        for (size_t i = 1; i < plan.size(); ++i)
            for (size_t k = 0; plan[i][k]; ++k)
                mem.put(0x51030 + 64 * i + k, uint8_t(plan[i][k]));
        // Per-day records (74CC00): byte 0 = holiday (720F20); 12/23 and 12/31 set.
        mem.put(c.weather_days, U(0x52000));
        mem.put(0x52000 + 4 * 266, uint8_t(1));
        mem.put(0x52000 + 4 * 274, uint8_t(1));
        const p5r_social_menu::Roots none{};
        const auto month = cal::Sample(mem, c, none, 0, ~U{0}, date);
        check(month.ready && month.month == 12 && month.days_in_month == 31 &&
                  month.first_weekday == 4 && month.holiday[23] && !month.holiday[31] &&
                  !month.holiday[17] && month.vacation[31] == 1,
              "calendar grid holiday = day record byte 0 unless vacation (77CE78)");
        const auto plans = cal::DayPlans(month, 17);
        check(plans.size() == 2 && plans[0]->label == "Deadline B" && plans[1]->label == "Event A",
              "calendar plan list: deadlines first (7777F0 exchange sort)");
    }
    check(StepMonth(r, 1, 31, 1) == 228 && StepMonth(r, 1, 20, -1) == 1220 &&
              StepMonth(r, 12, 31, 1) == 131 && StepMonth(r, 4, 20, 1) == 520 &&
              !StepMonth(r, 4, 20, -1) && !StepMonth(r, 3, 20, 1) && !StepMonth(r, 3, 20, 2),
          "data2 calendar month step (L/R: April .. March, day clamped)");
    r.jobs = false;

    // Requests (synthetic): ids in cmpQuestSortTable order, state, difficulty.
    r.requests = true;
    r.prio = {6, 3, 1, 1, 2, 4, 5};
    p5r_social_menu::request::Roots q;
    q.sort = 0x2000, q.data = 0x2008, q.states = 0x20000;
    struct Q {
        uint16_t id;
        uint8_t state, diff, isnew;
        uint16_t recent;
    };
    const std::vector<Q> quests{{4, 5, 1, 0, 1},   {9, 5, 2, 0, 3},   {10, 5, 1, 0, 2},
                                {13, 5, 1, 0, 4},  {79, 6, 1, 0, 0},  {66, 6, 1, 0, 0},
                                {75, 6, 4, 1, 20}, {30, 6, 4, 1, 19}, {45, 6, 2, 0, 12}};
    mem.ctd(q.sort, 0x21000, 4, uint32_t(quests.size()));
    mem.ctd(q.data, 0x22000, 0x24, 100);
    p5r_social_menu::request::List list;
    list.ready = true;
    for (size_t i = 0; i < quests.size(); ++i) {
        const auto& e = quests[i];
        mem.put(0x21030 + 4 * i, int16_t(e.id));
        mem.put(q.states + 8ull * e.id + 4, uint8_t(e.isnew));
        mem.put(q.states + 8ull * e.id + 5, e.state);
        p5r_social_menu::request::Entry en;
        en.id = e.id;
        en.state = e.state;
        en.difficulty = e.diff;
        en.recent = e.recent;
        list.entries.push_back(en);
    }
    std::stable_sort(list.entries.begin(), list.entries.end(),
                     [](const auto& a, const auto& b) { return a.recent > b.recent; });
    const auto req = SampleRequests(mem, r, q, list);
    auto ids = [&](const std::vector<uint8_t>& order) {
        std::string s;
        for (auto k : order)
            s += std::to_string(list.entries[k].id) + ",";
        return s;
    };
    check(req.ready && ids(req.progress) == "4,9,10,13,79,66,75,30,45,",
          "data2 request Progress tab (7FEDC0)");
    check(ids(req.difficulty) == "4,10,13,79,66,9,45,30,75,",
          "data2 request Difficulty tab (7FEF40)");
    check(req.extra[0].isnew && req.extra[0].grade == 4 && !req.extra.back().isnew,
          "data2 request NEW/grade");
    list.entries[0].state = 1;
    check(SampleRequests(mem, r, q, list).extra[0].grade == 0, "data2 request state 1 draws ?");
    list.entries.pop_back();
    check(!SampleRequests(mem, r, q, list).ready, "data2 request list mismatch fails closed");

    // Confidant function state (85A1E0): cmmFunctionTable row 20 (synthetic) with the reader's
    // Down Shot (0xA1) at rank 2 and Cheap Shot (0xA6) at rank 5; confidant 20 record in slot 3.
    r.func_table = 0x3000, r.func_info = 0x3008, r.func_open = 0x3010, r.cmm = 0x3018;
    r.remap_a.fill(2);
    r.remap_b.fill(2);
    mem.ctd(r.func_table, 0x30000, 0x78, 38);
    mem.ctd(r.func_info, 0x31000, 8, 290);
    mem.ctd(r.func_open, 0x32000, 8, 0);
    const U row20 = 0x30030 + 0x78ull * 20;
    mem.put(row20 + 2, int16_t(2));
    mem.put(row20 + 4, int16_t(0xa1));
    mem.put(row20 + 12 + 2, int16_t(5));
    mem.put(row20 + 12 + 4, int16_t(0xa6));
    mem.put(row20 + 24 + 2, int16_t(-1));
    for (unsigned row = 1; row < 38; ++row)
        if (row != 20)
            mem.put(0x30030 + 0x78ull * row + 2, int16_t(-1));
    mem.put(r.cmm, U(0x33000));
    auto confidant = [&](uint16_t rank, uint16_t cflags) {
        mem.put(0x33000 + 2 + 16 * 3 + 2, cflags);
        mem.put(0x33000 + 2 + 16 * 3 + 4, uint16_t(20));
        mem.put(0x33000 + 2 + 16 * 3 + 6, rank);
    };
    auto state = [&](int16_t f) { return FunctionState(mem, r, flags, 0, ~U{0}, f); };
    confidant(6, 0);
    check(state(0xa1) == 1 && state(0xa6) == 1 && state(0x10) == -3, "data2 function state r6");
    confidant(3, 1); // reversed: kind 0 rows are not gated
    check(state(0xa1) == 1 && state(0xa6) == 0, "data2 function state r3");
    mem.put(0x33000 + 2 + 16 * 3 + 4, uint16_t(0));
    check(state(0xa1) == 0, "data2 function state not met");
    r.remap_a[20 - 3] = 9;
    check(!state(0xa1), "data2 function state remapped id fails closed");

    // Public ABI: no DATA2 code in memory -> the blocks publish not-ready, actions validate.
    Host h;
    auto* reader = m->create(&h.api, "{}");
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    check(ext && ext->on_action, "data2 extensions");
    check(ext->on_action(reader, "calendar_select", 214) &&
              ext->on_action(reader, "calendar_select", -1) &&
              !ext->on_action(reader, "calendar_select", 1300) &&
              !ext->on_action(reader, "calendar_select", 200) &&
              ext->on_action(reader, "request_tab", 2) &&
              !ext->on_action(reader, "request_tab", 3) &&
              !ext->on_action(reader, "calendar_month", 1) &&
              !ext->on_action(reader, "calendar_month", 2),
          "data2 action validation");
    m->destroy(reader);
}
// ---- Now-playing BGM (p5r_bgm_reader.h) ----
// Accessor words: the instruction words the BGM reader matches or decodes (the words it does not
// check are NOP). Slot/voice contents are synthetic in the runtime layout (BGM slot 255, voice
// +0x10 playback counter, +0x18 cue).
constexpr U BgmSlots = 0x2309a78, BgmVoicePtr = 0x220cf28, BgmPack = 0x2304b68;
constexpr U BgmVoices = 0x95000000;
void InstallBgm(Host& h) {
    h.code(0xeaa140,
           {0x2a0003e8, 0x2a1f03e0, 0xaa1f03ea, 0xf000a2eb, 0x9129e16b, 0x14000005, 0x9101814a,
            0x11000400, 0xf140195f, 0x540002e0, 0x386a6969, 0x34ffff69, 0x8b0a0169, 0xb940092c,
            0x6b08019f, 0x54fffee1, 0xb9400d2c, 0x6b01019f, 0x54fffe81, 0xb980112b});
    h.code(0xeaa244, {0xb900267f, 0xb9003e74});
    h.code(0xea8ab0, {0x12800008, 0x2a1303e0, 0x52800023, 0x2a1f03e4, 0x2907a341, 0xd503201f});
    h.code(0x10a5a70, {0xf0008b29, 0xf9479529, 0x937b7c08, 0x38686920, 0xd65f03c0});
    h.code(0x10a6d00, {0xb90012c0, 0x52800020, 0xb9001ad3, 0x390002c0});
    h.code(0xea6ff0, {0xd000a2e9, 0x912da129, 0xf940052a, 0x79400288, 0xb9000148, 0xb9400129});
    PutBytes(h, h.base + BgmSlots, std::vector<uint8_t>(0x100 * 0x60));
    PutBytes(h, BgmVoices, std::vector<uint8_t>(0x100 * 0x20));
    h.put(h.base + BgmVoicePtr, BgmVoices);
    h.put(h.base + BgmPack, uint32_t(0));
    // other sound players occupy slots too (player 3 = SYSTEM.ACB, player 60 = ambience)
    h.put(h.base + BgmSlots + 251 * 0x60, uint8_t(1));
    h.put(h.base + BgmSlots + 251 * 0x60 + 8, std::array<int32_t, 3>{3, 0, 6});
    h.put(h.base + BgmSlots + 249 * 0x60, uint8_t(1));
    h.put(h.base + BgmSlots + 249 * 0x60 + 8, std::array<int32_t, 3>{60, 10, 6});
}
// BGM slot S: active, player 0, acb 0, kind 0, state, cue (+0x3C), next (+0x40); voice S.
void PutBgm(Host& h, unsigned s, int32_t state, int32_t cue, int32_t next, uint8_t playing,
            uint32_t playback, int32_t voice_cue) {
    const U e = h.base + BgmSlots + s * 0x60, v = BgmVoices + s * 0x20;
    h.put(e, uint8_t(1));
    h.put(e + 8, std::array<int32_t, 3>{0, 0, 0});
    h.put(e + 0x24, state);
    h.put(e + 0x3c, std::array<int32_t, 2>{cue, next});
    h.put(v, playing);
    h.put(v + 0x10, playback);
    h.put(v + 0x18, voice_cue);
}
void BgmTests(const EdenDsmodModuleApi* m) {
    Host h;
    InstallBgm(h);
    void* r = m->create(&h.api, "{}");
    check(r, "bgm create");
    auto sample = [&] {
        h.social_tear_reads = 0;
        m->sample(r, &h.api);
        check(!h.writes && h.begins == h.ends, "bgm read only");
    };
    auto expect = [&](int64_t playing, int64_t cue, const char* title, int64_t known, int64_t index,
                      int64_t change, int64_t fade, const char* what) {
        check(h.nums["live.ready"] == 1 && h.nums["bgm.ready"] == 1, what);
        check(h.nums["bgm.playing"] == playing && h.nums["bgm.cue"] == cue &&
                  h.texts["bgm.title"] == title && h.nums["bgm.known"] == known &&
                  h.nums["bgm.index"] == index && h.nums["bgm.change"] == change &&
                  h.nums["bgm.fade"] == fade,
              what);
    };
    // Leblanc attic, evening (live: slot 255 cue 640, voice playback 0xe cue 640)
    PutBgm(h, 255, 0, 640, 0, 1, 14, 640);
    sample();
    expect(1, 640, "Beneath the Mask", 1, 34, 1, 0, "field BGM with its Thieves Den title");
    sample();
    expect(1, 640, "Beneath the Mask", 1, 34, 1, 0, "same playback: change counter steady");
    // safe room 495 -> Palace 400: state 3 fade-out keeps 495 audible, next queued
    PutBgm(h, 255, 0, 495, -1, 1, 16, 495);
    sample();
    expect(1, 495, "Have a Short Rest", 1, 63, 2, 0, "new playback bumps change");
    PutBgm(h, 255, 3, 495, 400, 1, 16, 495);
    sample();
    expect(1, 495, "Have a Short Rest", 1, 63, 2, -1, "fade-out to the queued cue");
    PutBgm(h, 255, 4, 495, 400, 0, 16, -1);
    sample();
    expect(0, -1, "", 0, -1, 2, 0, "switch gap: voice stopped, nothing audible");
    PutBgm(h, 255, 0, 400, -1, 1, 17, 400);
    sample();
    expect(1, 400, "King, Queen, and Slaves", 1, 64, 3, 0, "next cue started");
    PutBgm(h, 255, 2, 400, -1, 1, 17, 400);
    sample();
    expect(1, 400, "King, Queen, and Slaves", 1, 64, 3, 1, "fade-in");
    // load screen (live): voice cleared, +0x3C keeps the stale cue
    PutBgm(h, 255, 0, 400, -1, 0, 17, -1);
    sample();
    expect(0, -1, "", 0, -1, 3, 0, "stopped: stale slot cue is not published");
    // the same cue restarted from the top is a new playback
    PutBgm(h, 255, 0, 400, -1, 1, 18, 400);
    sample();
    expect(1, 400, "King, Queen, and Slaves", 1, 64, 4, 0, "restart of the same cue");
    // cue 34 (Sae, 12/24 event, live) = BGM.ACB waveform 30, the same waveform as 33 Alleycat
    PutBgm(h, 255, 0, 34, -1, 1, 19, 34);
    sample();
    expect(1, 34, "Alleycat", 1, 24, 5, 0, "untitled cue inherits the same-waveform title");
    PutBgm(h, 255, 0, 26, -1, 1, 20, 26);
    sample();
    expect(1, 26, "", 0, -1, 6, 0, "untitled waveform: generic label");
    PutBgm(h, 255, 0, 958, -1, 1, 21, 958);
    sample();
    // Rows >= 78: the romfs name table holds dev placeholders, the title exists only as the
    // MUSIC_TITLE_001.SPD sprite of the row -> bgm.title empty, known + index (the page draws the
    // row's sprite). Formerly embedded transcriptions shown in the comments.
    expect(1, 958, "", 1, 105, 7, 0, "cue 958: the listed row 105 (sprite title, art only)");
    PutBgm(h, 255, 0, 957, -1, 1, 21, 957);
    sample();
    expect(1, 957, "", 1, 87, 8, 0, "placeholder row 87: sprite title (art only)");
    PutBgm(h, 255, 0, 0, -1, 1, 21, 0);
    sample();
    expect(1, 0, "My Homie", 1, 12, 9, 0, "cue 0 is a real cue");
    PutBgm(h, 255, 0, 910, -1, 1, 21, 910);
    sample();
    expect(0, 910, "", 0, -1, 10, 0, "silent placeholder cue: not playing");
    PutBgm(h, 255, 0, 960, -1, 1, 22, 960);
    sample();
    expect(1, 960, "", 1, 104, 11, 0, "row 104: sprite title (art only)");
    PutBgm(h, 255, 0, 924, -1, 1, 23, 924);
    sample();
    expect(1, 924, "", 1, 93, 12, 0, "placeholder name -> sprite title (art only)");
    // a DLC costume BGM pack redefines 60300/60340/60907: base titles no longer apply
    h.put(h.base + BgmPack, uint32_t(3));
    PutBgm(h, 255, 0, 300, -1, 1, 24, 300);
    sample();
    expect(1, 300, "", 0, -1, 13, 0, "DLC pack overrides the battle cue title");
    PutBgm(h, 255, 0, 400, -1, 1, 25, 400);
    sample();
    expect(1, 400, "King, Queen, and Slaves", 1, 64, 14, 0, "pack leaves other cues alone");
    h.put(h.base + BgmPack, uint32_t(0));
    PutBgm(h, 255, 0, 300, -1, 1, 26, 300);
    sample();
    expect(1, 300, "Last Surprise", 1, 53, 15, 0, "no pack: base battle title");
    // voice/slot cue disagreement (mid-switch) is not published as playing
    PutBgm(h, 255, 0, 300, -1, 1, 26, 907);
    sample();
    expect(0, -1, "", 0, -1, 15, 0, "slot and voice disagree -> not playing");
    // torn second read: the previous frame is held
    PutBgm(h, 255, 0, 907, -1, 1, 27, 907);
    sample();
    expect(1, 907, "", 1, 94, 16, 0, "battle cue (row 94: sprite title, art only)");
    h.social_tear = BgmVoices + 255 * 0x20;
    PutBgm(h, 255, 0, 340, -1, 1, 28, 340);
    sample();
    expect(1, 907, "", 1, 94, 16, 0, "torn sample holds the last frame");
    h.social_tear = 0;
    sample();
    expect(1, 340, "Triumph", 1, 54, 17, 0, "next clean sample");
    // the BGM slot moves (slot table rebuilt): found again by a scan, never guessed
    h.put(h.base + BgmSlots + 255 * 0x60, uint8_t(0));
    PutBgm(h, 7, 0, 480, -1, 1, 29, 480);
    sample();
    expect(1, 480, "Mementos", 1, 77, 18, 0, "relocated BGM slot");
    // two BGM slots = ambiguous: cleared after the hold window
    PutBgm(h, 9, 0, 610, -1, 1, 30, 610);
    h.put(h.base + BgmSlots + 7 * 0x60 + 0x10, int32_t(1)); // slot 7 no longer kind 0
    sample();
    expect(1, 610, "Tokyo Daylight", 1, 38, 19, 0, "rescan after the cached slot changed");
    h.put(h.base + BgmSlots + 7 * 0x60 + 0x10, int32_t(0));
    h.put(h.base + BgmSlots + 9 * 0x60, uint8_t(0));
    sample(); // cached slot 9 gone -> scan finds slot 7 only
    expect(1, 480, "Mementos", 1, 77, 20, 0, "back to slot 7");
    h.put(h.base + BgmSlots + 9 * 0x60, uint8_t(1));
    h.put(h.base + BgmSlots + 7 * 0x60, uint8_t(0));
    h.put(h.base + BgmSlots + 7 * 0x60, uint8_t(1));
    h.put(h.base + BgmSlots + 7 * 0x60 + 8, int32_t(5)); // slot 7 now another player
    h.put(h.base + BgmSlots + 250 * 0x60, uint8_t(1));   // and two BGM-shaped slots appear
    h.put(h.base + BgmSlots + 250 * 0x60 + 8, std::array<int32_t, 3>{0, 0, 0});
    for (int i = 0; i < 40; ++i)
        sample();
    check(h.nums["bgm.ready"] == 0 && h.nums["bgm.playing"] == 0 && h.nums["bgm.cue"] == -1 &&
              h.texts["bgm.title"].empty() && h.nums["live.ready"] == 1,
          "ambiguous BGM slots clear bgm.* only");
    m->destroy(r);
    // accessor mismatch: bgm.ready 0, the rest of the module unaffected
    Host bad;
    InstallBgm(bad);
    PutBgm(bad, 255, 0, 640, 0, 1, 14, 640);
    bad.code(0xeaa140 + 4 * 13, {0xb940052c}); // ldr w12,[x9,#4] instead of #8
    r = m->create(&bad.api, "{}");
    m->sample(r, &bad.api);
    check(bad.nums["live.ready"] == 1 && bad.nums["bgm.ready"] == 0 &&
              bad.nums["bgm.playing"] == 0 && bad.nums["bgm.cue"] == -1,
          "BGM accessor mismatch hides bgm.* only");
    m->destroy(r);
    // a gameplay gate blip (event scene change) does not count as a new cue
    {
        Host g;
        InstallBgm(g);
        PutBgm(g, 255, 0, 23, 0, 1, 5, 23);
        void* q = m->create(&g.api, "{}");
        m->sample(q, &g.api);
        check(g.nums["bgm.cue"] == 23 && g.nums["bgm.change"] == 1, "event cue");
        g.put(g.ctx + 4, int32_t(1));
        m->sample(q, &g.api);
        check(g.nums["bgm.ready"] == 0 && g.nums["bgm.change"] == 1, "gate closed");
        g.put(g.ctx + 4, int32_t(4));
        m->sample(q, &g.api);
        check(g.nums["bgm.cue"] == 23 && g.nums["bgm.change"] == 1, "same playback after blip");
        m->destroy(q);
    }
    // outside gameplay (scene 1, title) bgm.ready is 0 even though a cue plays
    Host title;
    InstallBgm(title);
    PutBgm(title, 255, 0, 901, 0, 1, 1, 901);
    title.put(title.ctx + 4, int32_t(1));
    r = m->create(&title.api, "{}");
    m->sample(r, &title.api);
    check(title.nums["live.ready"] == 0 && title.nums["bgm.ready"] == 0 &&
              title.nums["bgm.cue"] == -1,
          "no bgm outside the gameplay gate");
    m->destroy(r);
}

// ---- battle2.* (p5r_battle2_reader.h): enemy affinities with native discovery masking, battle
// party, turn (actor state handlers) and target (battle camera), over synthetic battle states.
#include "p5r_battle2_fixtures.inc"
struct Battle2Setup {
    U ctx{}, camera{};
    std::vector<U> party_units, enemy_units;
};
Battle2Setup InstallBattle2(Host& h, const Battle2Fixture& f) {
    h.reset();
    Battle2Setup s;
    for (const auto& c : Battle2CodeFixture())
        PutBytes(h, h.base + c.offset, c.bytes);
    const U mgr = 0x95000000, battle = 0x95004000, bc = 0x95005000, name = 0x9500a000;
    const U cam = 0x95020000, etab = 0x95030000, atab = 0x95060000, order = 0x9500b000;
    const U vt = h.base + 0x18b9c08;
    s.ctx = bc;
    s.camera = cam;
    h.put(h.base + 0x2209f50, mgr);
    h.put(mgr + 0x2c40, battle);
    h.put(mgr + 0x2c30, U(0));
    h.put(battle, uint32_t(2));
    h.put(battle + 0x80, U(0));
    h.put(battle + 0x18, name);
    h.put(name, std::array<char, 7>{'b', 'a', 't', 't', 'l', 'e', 0});
    h.put(battle + 0x48, bc);
    h.put(h.base + 0x22ab51c, uint16_t(783));
    h.put(vt + 0x60, h.base + 0x3ad934);
    h.put(vt + 0x28, h.base + 0x1c80c);
    h.put(h.base + 0x2207e20, h.base + 0x1c848);
    h.put(h.base + 0x2207e28, h.base + 0x43b90);
    h.put(h.base + 0x2207e58, h.base + 0xcd1a0);
    h.put(h.base + 0x2207e08, h.base + 0x1408);
    std::vector<uint16_t> ids;
    auto list = [&](U head_at, const std::vector<Battle2Unit>& us, bool party, U area) {
        h.put(head_at + 0x10, uint32_t(us.size()));
        for (size_t i = 0; i < us.size(); ++i) {
            const auto& u = us[i];
            const U node = area + i * 0x1000, ctl = node + 0x200;
            const U unit = party ? h.base + h.units + 0x2a0 * u.id : node + 0x400;
            h.put(i ? area + (i - 1) * 0x1000 : head_at, node);
            h.put(node, U(0));
            h.put(node + 0x28, u.actor);
            h.put(u.actor, vt);
            h.put(u.actor + 8, u.actor_flags);
            h.put(u.actor + 0x30, ctl);
            h.put(u.actor + 0x230, h.base + u.state_fn);
            h.put(u.actor + 0x238, u.state_adj);
            h.put(ctl + 8, unit);
            // the unit (0x2A0 bytes): the fields p5r_enemy / p5r_battle2 / the party gate read
            PutBytes(h, unit, std::vector<uint8_t>(0x2a0, 0));
            h.put(unit, u.flags);
            h.put(unit + 4, u.kind);
            h.put(unit + 8, u.id);
            h.put(unit + 0xc, u.hp);
            h.put(unit + 0x10, u.sp);
            h.put(unit + 0x14, u.status);
            h.put(unit + 0x18, u.level);
            h.put(unit + 0x20, uint16_t(u.id)); // affinity id == unit id
            if (party) {
                h.put(unit + 0x44, uint8_t(1)); // persona slot 0 valid (unit+0x40 = 0)
                h.put(unit + 0x48, uint8_t(u.level));
            }
            (party ? s.party_units : s.enemy_units).push_back(unit);
            if (party)
                ids.push_back(static_cast<uint16_t>(u.id));
        }
    };
    list(bc + 0x110, f.party, true, 0x95100000);
    list(bc + 0x130, f.enemies, false, 0x95200000);
    // party array (units+0x1CE0) = the HUD order
    std::array<uint16_t, 10> party_array{};
    for (size_t i = 0; i < f.party_array.size() && i < 4; ++i)
        party_array[i] = f.party_array[i];
    h.put(h.base + h.units + 0x1ce0, party_array);
    // camera: vtable, the framed actors and the target list
    PutBytes(h, cam, std::vector<uint8_t>(0x240, 0));
    h.put(cam, h.base + 0x19056a8);
    h.put(cam + 0x98, f.camera_acting);
    h.put(cam + 0xb8, f.camera_input);
    h.put(bc + 0xe0, cam);
    h.put(cam + 0x110, U(0));
    for (size_t i = 0; i < f.targets.size(); ++i) {
        const U node = cam + 0x1000 + i * 0x100;
        h.put(i ? cam + 0x1000 + (i - 1) * 0x100 : cam + 0x110, node);
        h.put(node, U(0));
        h.put(node + 0x28, f.targets[i]);
    }
    h.put(cam + 0x120, uint32_t(f.targets.size()));
    // enemy tables, discovery words, element order
    h.put(h.base + 0x220a188, etab);
    h.put(h.base + 0x220a198, atab);
    h.put(h.base + 0x2209a70, order);
    h.put(order, f.order);
    for (const auto& t : f.tables) {
        const U rec = etab + U(t.id) * 0x44;
        PutBytes(h, rec, std::vector<uint8_t>(0x44, 0));
        h.put(rec + 8, t.max_hp);
        h.put(rec + 0xc, t.max_sp);
        h.put(rec + 0x16, t.skills);
        h.put(atab + U(t.id) * 0x28, t.row); // affinity id == unit id
        // the reader loads the two words holding bits id*11 .. id*11+10
        for (unsigned k = 0; k < 2; ++k) {
            const U word = h.base + 0x2278910 + (t.id * 11 / 32 + k) * 4;
            if (!h.ram.count(word))
                h.put(word, uint32_t(0));
        }
        for (unsigned e = 0; e < 10; ++e) {
            if (!((t.known >> e) & 1))
                continue;
            const unsigned bit = t.id * 11 + e;
            const U word = h.base + 0x2278910 + (bit / 32) * 4;
            uint32_t w = 0;
            for (int i = 0; i < 4; ++i)
                w |= uint32_t(h.ram.count(word + i) ? h.ram.at(word + i) : 0) << (8 * i);
            h.put(word, w | (1u << (bit % 32)));
        }
    }
    return s;
}
void Battle2Tests(const EdenDsmodModuleApi* m, Host& h) {
    auto run = [&](void* r) {
        m->sample(r, &h.api);
        check(!h.writes, "battle2 read only");
    };
    auto fresh = [&] { return OpenAll(m->create(&h.api, "{}")); };
    // One enemy (id 321), member 4's command menu, the target cursor on the enemy.
    {
        const auto& f = Fixture_single_input();
        auto s = InstallBattle2(h, f);
        void* r = fresh();
        run(r);
        check(h.nums["battle.ready"] && h.nums["enemy.count"] == 1, "single battle roster");
        check(h.nums["battle2.stage"] == 0 && h.nums["bparty.ready"] == 1, "single battle2 ready");
        // elements 0, 1, 2, 3, 6, 8 discovered: normal, normal (pct), normal (flag bits), weak;
        // 4, 5, 7, 9 unknown
        const std::array<int, 10> aff{0, 0, 0, 5, -1, -1, 0, -1, 0, -1};
        for (unsigned e = 0; e < 10; ++e)
            check(h.nums["enemy.0.aff." + std::to_string(e)] == aff[e], "single enemy affinities");
        check(h.nums["enemy.0.hp_max"] == 2400 && h.nums["enemy.0.sp_max"] == 900,
              "single enemy max HP/SP (record)");
        check(h.nums["battle.turn.side"] == 1 && h.nums["battle.turn.actor"] == 1 &&
                  h.nums["battle.phase"] == 1,
              "member 4 (party slot 1) is choosing a command");
        check(h.nums["battle.target.side"] == 2 && h.nums["battle.target.slot"] == 0,
              "cursor on enemy 0");
        check(h.nums["bparty.0.id"] == 1 && h.nums["bparty.1.id"] == 4 &&
                  h.nums["bparty.2.id"] == 7 && h.nums["bparty.3.id"] == 10,
              "battle party in party order");
        check(h.texts["bparty.2.name"] == "Haru", "battle party name");
        check(h.nums["bparty.1.hp"] == 250 && h.nums["bparty.3.sp"] == 140, "party HP/SP");
        check(!h.nums["bparty.0.down"] && !h.nums["bparty.0.ailment"] && !h.nums["bparty.0.ko"],
              "no statuses");
        // camera frames another unit -> no target (the camera lags a new turn)
        h.put(s.camera + 0xb8, f.party[3].actor);
        run(r);
        check(h.nums["battle.turn.actor"] == 1 && h.nums["battle.target.side"] == 0 &&
                  h.nums["battle.target.slot"] == -1,
              "camera not on the turn unit: target cleared");
        h.put(s.camera + 0xb8, f.party[0].actor);
        // turn moves: member 4 (list 0) idle, protagonist (list 3) choosing
        h.put(f.party[0].actor + 0x230, h.base + 0x1c848);
        h.put(f.party[3].actor + 0x230, h.base + 0xcd1a0);
        run(r);
        check(h.nums["battle.turn.side"] == 1 && h.nums["battle.turn.actor"] == 0,
              "turn follows the actor state handler");
        // a KO'd member (handler 1408) is not a turn
        h.put(f.party[1].actor + 0x230, h.base + 0x1408);
        run(r);
        check(h.nums["battle.turn.actor"] == 0 && h.nums["battle.turn.count"] == 1,
              "incapacitated handler ignored");
        // two party members acting at once -> slot -2 (0x2468: any non-idle, non-choosing handler)
        h.put(f.party[2].actor + 0x230, h.base + 0x2468);
        run(r);
        check(h.nums["battle.turn.side"] == 1 && h.nums["battle.turn.actor"] == -2 &&
                  h.nums["battle.phase"] == 2,
              "several acting party units");
        h.put(f.party[2].actor + 0x230, h.base + 0x1c848);
        // status word: down + shock
        h.put(s.enemy_units[0] + 0x14, uint32_t(0x100004));
        run(r);
        check(h.nums["enemy.0.down"] == 1 && h.nums["enemy.0.ailment"] == 3 &&
                  h.nums["enemy.0.ailments"] == 4,
              "enemy down + shock (bit 2 -> id 3)");
        // clearing a discovery bit hides that affinity (enemy id 321: bit 321*11 + 0)
        {
            const U word = h.base + 0x2278910 + (321 * 11 / 32) * 4;
            uint32_t w{};
            for (int i = 0; i < 4; ++i)
                w |= uint32_t(h.ram.at(word + i)) << (8 * i);
            h.put(word, w & ~(1u << ((321 * 11) % 32)));
            for (int k = 0; k < 4; ++k) // affinities are refreshed every 4th sample
                run(r);
            check(h.nums["enemy.0.aff.0"] == -1 && h.nums["enemy.0.aff.1"] == 0,
                  "undiscovered element published as -1");
        }
        // a wrong IsIdle vtable slot -> the class is unknown: no turn
        h.put(h.base + 0x18b9c08 + 0x28, h.base + 0x1c848);
        run(r);
        check(h.nums["battle.turn.side"] == 0 && h.nums["battle.turn.actor"] == -1 &&
                  h.nums["battle.phase"] == 0,
              "unknown actor class publishes no turn");
        m->destroy(r);
        // one changed code byte -> battle2 hidden, roster unaffected
        InstallBattle2(h, f);
        h.put(h.base + 0x8a3e10 + 0x40, uint8_t(0));
        r = fresh();
        run(r);
        check(h.nums["battle.ready"] && h.nums["battle2.stage"] == 1 && !h.nums["bparty.ready"] &&
                  !h.nums["enemy.0.aff_ready"] && !h.nums.count("enemy.0.aff.0") &&
                  h.nums["battle.turn.side"] == 0,
              "fingerprint mismatch hides battle2");
        m->destroy(r);
    }
    // The same battle, the enemy acting on member 7 (party slot 2).
    {
        const auto& f = Fixture_single_enemy();
        InstallBattle2(h, f);
        void* r = fresh();
        run(r);
        check(h.nums["battle.turn.side"] == 2 && h.nums["battle.turn.actor"] == 0 &&
                  h.nums["battle.phase"] == 2,
              "enemy action turn");
        check(h.nums["battle.target.side"] == 1 && h.nums["battle.target.slot"] == 2,
              "enemy targets member 7");
        m->destroy(r);
    }
    // Three enemies: id 55 + two of id 88, member 2's command menu, cursor on enemy row 1.
    // Id 55: resist, normal, weak, normal (flag bits), null, normal (pct), '?', normal, '?', '?'.
    // Id 88 (both rows): weak, normal, null, resist, normal, weak, normal, weak (pct + flag),
    // resist, null.
    {
        const auto& f = Fixture_three();
        InstallBattle2(h, f);
        void* r = fresh();
        run(r);
        const std::array<int, 10> first{4, 0, 5, 0, 3, 0, -1, 0, -1, -1};
        const std::array<int, 10> second{5, 0, 3, 4, 0, 5, 0, 5, 4, 3};
        for (unsigned e = 0; e < 10; ++e) {
            check(h.nums["enemy.0.aff." + std::to_string(e)] == first[e], "three: enemy 0");
            check(h.nums["enemy.1.aff." + std::to_string(e)] == second[e], "three: enemy 1");
            check(h.nums["enemy.2.aff." + std::to_string(e)] == second[e], "three: enemy 2");
        }
        check(h.nums["enemy.1.hp_max"] == 150 && h.nums["enemy.1.sp_max"] == 20 &&
                  h.nums["enemy.0.hp_max"] == 480,
              "three: max HP/SP (record)");
        check(h.nums["bparty.0.id"] == 1 && h.nums["bparty.1.id"] == 3 &&
                  h.nums["bparty.2.id"] == 2 && h.nums["bparty.3.id"] == 4,
              "three: battle party follows the party array, not the battle list");
        check(h.nums["battle.turn.side"] == 1 && h.nums["battle.turn.actor"] == 2 &&
                  h.nums["battle.phase"] == 1,
              "three: member 2 (HUD slot 2) choosing");
        check(h.nums["battle.target.side"] == 2 && h.nums["battle.target.slot"] == 1,
              "three: cursor on enemy row 1");
        m->destroy(r);
    }
}
// Fail closed: without a readable romfs no game name is published (no embedded fallback), and
// every live value stays intact.
void TextFailClosedTests(const EdenDsmodModuleApi* m) {
    Host h;
    h.api.read_romfs = nullptr;
    void* r = OpenAll(m->create(&h.api, "{}"));
    check(r, "create without romfs");
    m->sample(r, &h.api);
    check(h.nums["live.ready"] == 1 && h.nums["live.money"] == 87398, "live values without romfs");
    check(h.nums["party.2.id"] == 3 || h.texts.count("party.2.name"), "party published");
    check(h.texts["party.2.name"].empty(), "no member name without romfs tables");
    m->destroy(r);
    // a romfs whose tables are not P5R's (empty CPK dir): same
    Host g;
    g.api.read_romfs = [](void*, const char*, uint64_t, void*, size_t) -> size_t { return 0; };
    r = OpenAll(m->create(&g.api, "{}"));
    m->sample(r, &g.api);
    check(g.nums["live.ready"] == 1 && g.texts["party.2.name"].empty(),
          "unreadable romfs: no names");
    m->destroy(r);
}

// Package-driven output filter + worker-thread text load: with a package
// manifest attached, indexed families the package never names are not published (keys without
// digits always are), and the game-text tables arrive from the worker thread a few samples later.
void PackageConfigTests(const EdenDsmodModuleApi* m) {
    Host h;
    const char* config =
        R"({"pages":[{"id":"p","widgets":[{"type":"label","bind_text":"party.{i}.name"},)"
        R"({"type":"value","bind":"party.2.hp","show_bind":"!party.1.present+live.ready"}]}],)"
        R"("derived":[{"name":"x","cmp":"eq","a":"enemy.{i}.hp","b":0}],)"
        R"("module_outputs":["party.0.level","enemy.0.id"],"_about":"party.0.sp"})";
    void* r = OpenAll(m->create(&h.api, config));
    check(r, "create with a package config");
    bool named = false;
    for (int i = 0; i < 500 && !named; ++i) {
        m->sample(r, &h.api);
        named = h.texts["party.2.name"] == "Morgana";
        if (!named)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    check(named, "worker-thread game text arrives");
    check(h.nums["live.ready"] == 1 && h.nums["live.money"] == 87398, "unindexed outputs flow");
    check(h.nums.count("party.2.hp") && h.nums.count("party.0.hp") &&
              h.nums.count("party.1.present"),
          "named families flow (literal, template, gate term)");
    check(h.nums.count("enemy.0.hp") && h.nums.count("enemy.4.hp") &&
              !h.nums.count("enemy.4.present"),
          "derived term family flows, its unnamed siblings do not");
    check(!h.nums.count("party.0.level") && !h.nums.count("party.0.sp") &&
              !h.nums.count("enemy.0.id"),
          "families named only by module_outputs / _about are not published");
    check(h.nums.count("item.stage") && h.nums.count("roster.stage"), "diagnostics always flow");
    m->destroy(r);
    // destroy while the worker is still loading: joins cleanly
    r = OpenAll(m->create(&h.api, config));
    m->sample(r, &h.api);
    m->destroy(r);
}

int main() {
    // The game-text checks read the player's own romfs (P5R_ROMFS); without one the test is
    // skipped (exit code 77 = CTest SKIP_RETURN_CODE).
    if (!std::ifstream{RomfsRoot() + "/CPK/ALL_USEU.CPK"}) {
        std::cout << "SKIP p5r reader tests: set P5R_ROMFS to a romfs dump (CPK/ALL_USEU.CPK)\n";
        return 77;
    }
    const auto* m =
        eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    check(m, "export");
    check(!eden_dsmod_get_module(99, EDEN_DSMOD_MODULE_ABI_HASH), "wrong ABI");
    check(!m->supports_build("708BCACA873EA32B"), "wrong BID");
    Host h;
    void* r = OpenAll(m->create(&h.api, "{}"));
    check(r, "create");
    auto sample = [&] {
        h.party_reads = 0;
        h.targeted_reads = 0;
        h.registry_reads = 0;
        m->sample(r, &h.api);
        check(h.begins == h.ends, "balanced outputs");
        check(!h.writes, "read only");
    };
    sample();
    check(h.nums["live.ready"] == 1, "valid sample");
    check(h.nums["live.money"] == 87398, "money");
    check(h.nums["party.0.level"] == 14, "Joker level");
    check(h.nums["party.1.level"] == 19, "selected ally Persona slot");
    check(h.texts["party.2.name"] == "Morgana", "ID name");
    check(h.texts["live.date"] == "5/13 FRI  Evening", "calendar");
    // Natural encounters remain scene4. Native actor lifecycle, not scene5, gates roster.
    {
        const U mgr = 0x93000000, battle = 0x93004000, bc = 0x93005000, node = 0x93006000;
        const U actor = 0x93007000, ctl = 0x93008000, unit = 0x93009000, name = 0x9300a000;
        const U vt = h.base + 0x18b9c08;
        h.put(h.base + 0x2209f50, mgr);
        h.put(mgr + 0x2c40, battle);
        h.put(mgr + 0x2c30, U(0));
        h.put(battle, uint32_t(2));
        h.put(battle + 0x80, U(0));
        h.put(battle + 0x18, name);
        h.put(name, std::array<char, 7>{'b', 'a', 't', 't', 'l', 'e', 0});
        h.put(battle + 0x48, bc);
        h.put(bc + 0x130, node);
        h.put(bc + 0x140, uint32_t(1));
        h.put(h.base + 0x22ab51c, uint16_t(783));
        h.put(node, U(0));
        h.put(node + 0x28, actor);
        h.put(actor, vt);
        h.put(vt + 0x60, h.base + 0x3ad934);
        h.put(actor + 8, U(0x100040000020081));
        h.put(actor + 0x30, ctl);
        h.put(ctl + 8, unit);
        h.put(unit, std::array<uint8_t, 0x1c>{});
        h.put(unit, uint32_t(0xa001));
        h.put(unit + 4, uint16_t(2));
        h.put(unit + 8, uint32_t(436));
        h.put(unit + 12, uint32_t(2));
        h.put(unit + 16, uint32_t(15));
        h.put(unit + 20, uint32_t(0x100000));
        h.put(unit + 24, uint16_t(5));
        const U flags = 0x9300b000, strings = 0x9300c000, offsets = 0x9300d000;
        h.put(h.base + 0x1d834f8, flags);
        h.put(flags + 0x20, uint32_t(1U << 28));
        h.put(h.base + 0x22ab618, std::array<U, 2>{strings, offsets});
        h.put(offsets + 436 * 2, uint16_t(0));
        h.put(strings, std::array<char, 21>{'S', 'h', 'a', 'd', 'o', 'w', ' ', 'A', 'l', 'i', 'a',
                                            's', ' ', 'N', 'a', 'm', 'e', ' ', '0', '1', 0});

        sample();
        check(h.nums["battle.active"] && h.nums["battle.ready"] && h.nums["enemy.count"] == 1 &&
                  h.nums["enemy.0.id"] == 436 && h.nums["enemy.0.hp"] == 2,
              "field-embedded battle roster");
        check(h.texts["enemy.0.name"] == "Shadow Alias Name 01", "native alias disclosure gate");
        check(h.nums["state.mode"] == 3 && !h.nums["controls.field"] && !h.nums["map.ready"],
              "battle suppresses field map");
        {
            const U at = 0x93100000, ac = 0x93101000, uit = 0x93102000, uic = 0x93103000,
                    an = 0x93104000;
            h.put(battle + 0x80, at);
            h.put(at, uint32_t(2));
            h.put(at + 0x80, uit);
            h.put(at + 0x48, ac);
            h.put(at + 0x18, an);
            h.put(an, std::array<char, 18>{'B', 'T', 'L', '_', 'E', 'N', 'E', 'M', 'Y', '_', 'A',
                                           'N', 'A', 'L', 'Y', 'Z', 'E', 0});
            h.put(ac + 2, int16_t(4));
            h.put(ac + 4, int32_t(0));
            h.put(ac + 8, uint32_t(1));
            h.put(ac + 0x10, unit);
            h.put(ac + 0x60, uit);
            h.put(uit, uint32_t(2));
            h.put(uit + 0x80, U(0));
            h.put(uit + 0x48, uic);
            h.put(uic + 8, uint32_t(6));
            std::array<uint8_t, 0x74> model{};
            model[0] = 1;
            model[6] = 436 & 255;
            model[7] = 436 >> 8;
            model[0xb] = 5;
            const std::array<uint32_t, 10> aff{100,        0x10000032, 0x80000064, 0x80000064,
                                               0x10000032, 0x0800007d, 0x04000064, 0x02000064,
                                               0x01000064, 100};
            std::memcpy(model.data() + 0x2c, aff.data(), 40);
            model[0x54] = 1;
            model[0x6a] = 150;
            model[0x6e] = 54;
            h.put(uic + 0x3c, model);
            sample();
            check(h.nums["analysis.ready"] && h.nums["state.mode"] == 6 &&
                      !h.nums["controls.battle"],
                  "native Analyze takes battle mode");
            check(h.nums["analysis.hp"] == 150 && h.nums["analysis.sp"] == 54 &&
                      h.nums["analysis.id"] == 436,
                  "native Analyze displayed stats");
            const std::array<int, 10> categories{6, 4, 0, 0, 4, 5, 1, 2, 3, 0};
            for (unsigned i = 0; i < 10; ++i)
                check(h.nums["analysis.affinity." + std::to_string(i)] == categories[i],
                      "native affinity categories and unknown mask");
            const U message = 0x93106000;
            h.put(h.base + 0x1eacf38, int32_t(0));
            h.put(h.base + 0x231a820, message);
            h.put(message + 0x38, U(0x93107000));
            h.put(message + 0x40, int8_t(1));
            sample();
            check(h.nums["state.mode"] == 4 && !h.nums["analysis.ready"] &&
                      !h.nums["controls.battle"],
                  "dialogue overlay disables Analyze close and battle controls");
            h.put(h.base + 0x1eacf38, int32_t(-1));
            h.put(h.base + 0x231a820, U(0));
            h.put(ac + 0x10, unit + 0x100);
            h.put(unit + 0x104, uint16_t(2));
            h.put(unit + 0x108, uint32_t(436));
            sample();
            check(!h.nums["analysis.ready"] && h.nums["controls.battle"],
                  "Analyze selected unit must belong to current battle");
            h.put(ac + 0x10, unit);
            h.put(ac + 2, int16_t(5));
            sample();
            check(!h.nums["analysis.ready"], "closing Analyze is hidden");
            h.put(ac + 2, int16_t(4));
            h.put(uic + 8, uint32_t(5));
            sample();
            check(!h.nums["analysis.ready"], "Analyze nested kind exact");
            h.put(uic + 8, uint32_t(6));
            h.put(battle + 0x80, U(0));
            sample();
            check(!h.nums["analysis.ready"] && h.nums["analysis.affinity.1"] == 6 &&
                      h.texts["analysis.name"].empty(),
                  "removed Analyze clears affinities");
        }
        h.tear_registry = true;
        sample();
        check(!h.nums["battle.ready"] && !h.nums["controls.field"],
              "changed task manager rejected");
        h.tear_registry = false;
        h.tear_address = unit;
        sample();
        check(h.nums["live.ready"] && !h.nums["battle.ready"], "torn enemy identity isolated");
        h.tear_address = 0;
        h.put(unit + 12, uint32_t(0));
        sample();
        check(h.nums["battle.ready"] && !h.nums["enemy.count"] && !h.nums["enemy.0.present"],
              "native invalid actor filtered");
        h.put(unit + 12, uint32_t(2));
        h.put(unit + 8, uint32_t(783));
        sample();
        check(h.nums["live.ready"] && h.nums["battle.active"] && !h.nums["battle.ready"] &&
                  !h.nums["enemy.0.present"],
              "enemy name count bounds fail closed");
        h.put(unit + 8, uint32_t(436));
        h.put(node, node);
        sample();
        check(!h.nums["battle.ready"], "cyclic enemy list rejected");
        h.put(node, U(0));
        h.put(vt + 0x60, h.base + 0x3ad938);
        sample();
        check(!h.nums["battle.ready"], "unknown actor predicate rejected");
        h.put(vt + 0x60, h.base + 0x3ad934);
        h.put(battle, uint32_t(1));
        sample();
        check(h.nums["battle.active"] && !h.nums["battle.ready"], "creating battle has no roster");
        h.put(battle, uint32_t(3));
        sample();
        check(!h.nums["battle.active"] && !h.nums["enemy.0.present"],
              "dead battle task clears roster");
        h.ram.erase(mgr + 0x2c40);
        sample();
        check(!h.nums["controls.field"] && !h.nums["map.ready"],
              "failed registry scan suppresses field controls");
        h.reset();
        sample();
    }
    // Analyze fixtures (synthetic): the native Analyze context head, the nested UI view model and
    // the enemy unit headers in their runtime layouts, with made-up ids and values. two_types*: one
    // unit of id 200 and two of id 120, the Analyze roster cycled from index 0 to 1 (the second
    // model has four undiscovered elements). hidden_words: two units of one id whose model still
    // carries Resist/Weak words for undiscovered elements, which must never be published.
    {
        struct Enemy {
            U flags;
            uint32_t id, hp, sp;
            uint16_t level;
        };
        struct Model {
            uint16_t id, hp, sp;
            uint8_t level;
            std::array<uint32_t, 10> words;
            std::array<uint8_t, 10> unknown;
        };
        struct Fixture {
            const char* name;
            int32_t selected;
            uint32_t count;
            Model model;
            std::array<int, 5> selected_units;
            std::vector<Enemy> enemies;
        };
        constexpr U Plain = 0x100040000020081ULL, Other = 0x101840000020081ULL,
                    Flagged = 0x100040080020081ULL;
        constexpr uint32_t Null = 0x04000064, Repel = 0x02000064, Drain = 0x01000064,
                           Resist = 0x10000032, Weak = 0x08000064;
        const std::vector<Enemy> two_types{
            {Plain, 200, 480, 60, 11}, {Plain, 120, 210, 30, 4}, {Other, 120, 210, 30, 4}};
        const std::vector<Fixture> fixtures{
            {"two_types",
             0,
             2,
             {120, 210, 30, 4, {Null, 0, Repel, Drain, 0, Resist, Weak, 0, 0, 0}, {}},
             {1, 0, -1, -1, -1},
             two_types},
            {"two_types_r",
             1,
             2,
             {200,
              480,
              60,
              11,
              {0, Repel, Resist, Weak, 0, 0, Null, Drain, Weak, Repel},
              {0, 0, 0, 0, 0, 0, 1, 1, 1, 1}},
             {1, 0, -1, -1, -1},
             two_types},
            {"hidden_words",
             0,
             1,
             {90,
              130,
              44,
              7,
              {0, Repel, 0, Weak, Resist, Null, 0, 0, 0, 0},
              {0, 1, 0, 1, 0, 1, 0, 0, 0, 0}},
             {0, -1, -1, -1, -1},
             {{Flagged, 90, 130, 44, 7}, {Plain, 90, 130, 44, 7}}},
        };
        auto unit_bytes = [](const Enemy& e) {
            std::vector<uint8_t> v(0x1c, 0);
            const uint32_t head = 0xa001;
            const uint16_t kind = 2;
            std::memcpy(v.data(), &head, 4);
            std::memcpy(v.data() + 4, &kind, 2);
            std::memcpy(v.data() + 8, &e.id, 4);
            std::memcpy(v.data() + 0xc, &e.hp, 4);
            std::memcpy(v.data() + 0x10, &e.sp, 4);
            std::memcpy(v.data() + 0x18, &e.level, 2);
            return v;
        };
        auto context_bytes = [](const Fixture& f) {
            std::vector<uint8_t> v(0x70, 0);
            const int16_t phase = 4;
            std::memcpy(v.data() + 2, &phase, 2);
            std::memcpy(v.data() + 4, &f.selected, 4);
            std::memcpy(v.data() + 8, &f.count, 4);
            return v;
        };
        auto model_bytes = [](const Model& m) {
            std::vector<uint8_t> v(0x74, 0);
            v[0] = 1;
            std::memcpy(v.data() + 6, &m.id, 2);
            v[0xb] = m.level;
            std::memcpy(v.data() + 0x2c, m.words.data(), 40);
            std::memcpy(v.data() + 0x54, m.unknown.data(), 10);
            std::memcpy(v.data() + 0x6a, &m.hp, 2);
            std::memcpy(v.data() + 0x6e, &m.sp, 2);
            return v;
        };
        auto load = [&](const Fixture& f) {
            h.reset();
            const U mgr = 0x94000000, battle = 0x94004000, bc = 0x94005000, name = 0x9400a000;
            const U at = 0x94100000, ac = 0x94101000, uit = 0x94102000, uic = 0x94103000,
                    an = 0x94104000;
            const U vt = h.base + 0x18b9c08;
            h.put(h.base + 0x2209f50, mgr);
            h.put(mgr + 0x2c40, battle);
            h.put(mgr + 0x2c30, U(0));
            h.put(battle, uint32_t(2));
            h.put(battle + 0x80, at);
            h.put(battle + 0x18, name);
            h.put(name, std::array<char, 7>{'b', 'a', 't', 't', 'l', 'e', 0});
            h.put(battle + 0x48, bc);
            h.put(bc + 0x140, uint32_t(f.enemies.size()));
            h.put(h.base + 0x22ab51c, uint16_t(783));
            h.put(vt + 0x60, h.base + 0x3ad934);
            std::vector<U> units;
            for (size_t i = 0; i < f.enemies.size(); ++i) {
                const U node = 0x94010000 + i * 0x1000, actor = node + 0x100, ctl = node + 0x200,
                        unit = node + 0x300;
                h.put(i ? 0x94010000 + (i - 1) * 0x1000 : bc + 0x130, node);
                h.put(node, U(0));
                h.put(node + 0x28, actor);
                h.put(actor, vt);
                h.put(actor + 8, f.enemies[i].flags);
                h.put(actor + 0x30, ctl);
                h.put(ctl + 8, unit);
                const auto u = unit_bytes(f.enemies[i]);
                check(u.size() == 0x1c, "fixture unit header size");
                for (size_t k = 0; k < u.size(); ++k)
                    h.put(unit + k, u[k]);
                units.push_back(unit);
            }
            h.put(at, uint32_t(2));
            h.put(at + 0x80, uit);
            h.put(at + 0x48, ac);
            h.put(at + 0x18, an);
            h.put(an, std::array<char, 18>{'B', 'T', 'L', '_', 'E', 'N', 'E', 'M', 'Y', '_', 'A',
                                           'N', 'A', 'L', 'Y', 'Z', 'E', 0});
            const auto c = context_bytes(f);
            check(c.size() == 0x70, "fixture Analyze context size");
            for (size_t k = 0; k < c.size(); ++k)
                h.put(ac + k, c[k]);
            for (size_t k = 0; k < 5; ++k)
                h.put(ac + 0x10 + k * 8,
                      f.selected_units[k] < 0 ? U(0) : units[f.selected_units[k]]);
            h.put(ac + 0x60, uit);
            h.put(uit, uint32_t(2));
            h.put(uit + 0x80, U(0));
            h.put(uit + 0x48, uic);
            h.put(uic + 8, uint32_t(6));
            const auto model = model_bytes(f.model);
            check(model.size() == 0x74, "fixture view model size");
            for (size_t k = 0; k < model.size(); ++k)
                h.put(uic + 0x3c + k, model[k]);
            return ac;
        };
        auto affinities = [&](const std::array<int, 10>& want) {
            for (unsigned i = 0; i < 10; ++i)
                if (h.nums["analysis.affinity." + std::to_string(i)] != want[i])
                    return false;
            return true;
        };
        auto ac = load(fixtures[0]);
        sample();
        check(h.nums["analysis.ready"] && h.nums["state.mode"] == 6 && h.nums["enemy.count"] == 3,
              "two-type Analyze ready");
        check(h.nums["analysis.id"] == 120 && h.nums["analysis.level"] == 4 &&
                  h.nums["analysis.hp"] == 210 && h.nums["analysis.sp"] == 30,
              "selected model (id 120)");
        check(affinities({1, 0, 2, 3, 0, 4, 5, 0, 0, 0}), "id 120 affinities");
        check(h.nums["analyze.index"] == 0 && h.nums["analyze.count"] == 2 &&
                  h.nums["analyze.number"] == 1 && h.nums["analyze.cycle"] == 1 &&
                  h.nums["analyze.slot"] == 1 && h.nums["analysis.index"] == 0 &&
                  h.nums["analysis.count"] == 2,
              "native Analyze roster is per enemy type; selected row follows unit");
        // Index written before the view model is rebuilt: never publish a mixed frame.
        h.put(ac + 4, int32_t(1));
        sample();
        check(!h.nums["analysis.ready"] && !h.nums["analyze.cycle"] &&
                  h.nums["analyze.slot"] == -1 && h.nums["analyze.count"] == 0 &&
                  h.nums["analysis.affinity.1"] == 6,
              "torn cycle (index ahead of model) fails closed");
        load(fixtures[1]);
        sample();
        check(h.nums["analysis.ready"] && h.nums["analysis.id"] == 200 &&
                  h.nums["analysis.level"] == 11 && h.nums["analysis.hp"] == 480 &&
                  h.nums["analysis.sp"] == 60,
              "R cycle selects the other type (id 200)");
        check(affinities({0, 2, 4, 5, 0, 0, 6, 6, 6, 6}), "undiscovered elements stay UNKNOWN");
        check(h.nums["analyze.index"] == 1 && h.nums["analyze.number"] == 2 &&
                  h.nums["analyze.cycle"] == 1 && h.nums["analyze.slot"] == 0,
              "cycled selection follows native index");
        load(fixtures[2]);
        sample();
        check(h.nums["analysis.ready"] && h.nums["analysis.id"] == 90 &&
                  h.nums["analysis.hp"] == 130 && h.nums["analysis.sp"] == 44,
              "masked single-type model");
        check(affinities({0, 6, 0, 6, 4, 6, 0, 0, 0, 0}),
              "undiscovered Resist/Weak words stay UNKNOWN");
        check(h.nums["analyze.count"] == 1 && !h.nums["analyze.cycle"] &&
                  h.nums["analyze.slot"] == 0,
              "single-type Analyze cannot cycle");
        h.reset();
        sample();
        check(!h.nums["analyze.cycle"] && h.nums["analyze.slot"] == -1,
              "no battle clears Analyze cycling");
    }
    // Public boundary: optional map state never invalidates mandatory party/money.
    {
        const U manager = 0x91000000, map_task = 0x91004000, mapctx = 0x91005000;
        const U row = 0x91007000, tex = 0x91007100, name = 0x91007200;
        h.put(h.fieldctx + 0x1fa, uint8_t(1));
        h.put(h.base + 0x2209f50, manager);
        h.put(manager + 0x2c40, map_task);
        h.put(manager + 0x2c30, U(0));
        h.put(map_task, std::array<uint8_t, 0xc0>{});
        h.put(map_task, uint32_t(2));
        h.put(map_task + 0x18, name);
        h.put(map_task + 0x48, mapctx);
        const char task_name[] = "road map(FLD)";
        for (size_t i = 0; i < sizeof(task_name); ++i)
            h.put(name + i, task_name[i]);
        h.put(mapctx, std::array<uint8_t, 0xe10>{});
        h.put(mapctx + 0x10, uint32_t(7));
        h.put(mapctx + 0x2e0, h.fieldctx);
        h.put(mapctx + 0x2e8, std::array<uint16_t, 2>{3, 2});
        h.put(mapctx + 0x2f2, uint16_t(1));
        h.put(mapctx + 0x78, row);
        h.put(mapctx + 0x80, tex);
        h.put(row,
              std::array<uint16_t, 8>{3, 0x102, 575, 101, 161, 0, 3, 0}); // +C bit0: draw pointer
        h.put(tex, std::array<uint8_t, 16>{});
        h.put(tex, std::array<uint16_t, 2>{9, 2});
        h.put(tex + 4, uint32_t(1));
        h.put(tex + 8, 23.44f);
        const float scale = 23.44f / 1.5f;
        h.put(mapctx + 0x1f0, scale);
        h.put(mapctx + 0x1f4, int32_t(0));
        h.put(h.fieldctx + 0x2a10, std::array<float, 3>{scale * 25, 0, scale * 50});
        h.put(mapctx + 0x228, 0.8f);
        h.put(mapctx + 0x1f8, std::array<float, 2>{10, 20});
        h.put(mapctx + 4, std::array<float, 2>{600 * 0.8f + 10, 151 * 0.8f + 20});
        sample();
        check(h.nums["map.ready"] && h.nums["live.ready"], "optional map available");
        check(h.texts["map.area"] == "rmap_9_2_0", "live selected asset key");
        check(h.nums["field.key"] == 3002001 && h.nums["controls.field"], "field subvariant key");
        check(std::abs(h.floats.at("map.x") - 600) < 0.001 &&
                  std::abs(h.floats.at("map.y") + 151) < 0.001,
              "map projection and Y inversion");
        // The native pin (ctx+4) is only refreshed while a map is drawn (e.g. no minimap in the
        // Leblanc attic): the marker is reproduced from the C34010 rule, not from that pin.
        h.put(mapctx + 4, std::array<float, 2>{0, 0});
        sample();
        check(h.nums["map.ready"] && std::abs(h.floats.at("map.x") - 600) < 0.001 &&
                  std::abs(h.floats.at("map.y") + 151) < 0.001,
              "marker kept while the native map is not drawn");
        // ctx+18 bits 0x40'00008020: native C36F4C skips the moving pointer (the override
        // source C34010 uses is not drawn as a pointer), so only the marker disappears.
        for (const uint64_t bits : {uint64_t(0x20), uint64_t(0x8000), uint64_t(0x4000000000)}) {
            h.floats.erase("map.x");
            h.floats.erase("map.y");
            h.put(mapctx + 0x18, bits);
            sample();
            check(h.nums["map.ready"] && !h.floats.count("map.x") && !h.floats.count("map.y"),
                  "pointer suppressed by ctx+18 flags");
        }
        h.put(mapctx + 0x18, uint64_t(0));
        // ctx+1A bit0 with point E8 != EC: per-point anchor offset [ctx+88] + idx*16 + C/E.
        const U anchor_index = 0x91007500, anchors = 0x91007600;
        h.put(mapctx + 0x1a, uint8_t(1));
        h.put(mapctx + 0xe8, std::array<int32_t, 2>{2, 0});
        h.put(mapctx + 0x90, anchor_index);
        h.put(anchor_index, std::array<int16_t, 3>{-1, -1, 1});
        h.put(mapctx + 0x88, anchors);
        h.put(anchors + 16 + 0xc, std::array<int16_t, 2>{5, -7});
        sample();
        check(std::abs(h.floats.at("map.x") - 605) < 0.001 &&
                  std::abs(h.floats.at("map.y") + 144) < 0.001,
              "per-point anchor offset");
        h.put(mapctx + 0x1a, uint8_t(0));
        // Row +C bit0 clear (interior rows, e.g. the Leblanc attic): native C36F4C draws no moving
        // pointer; the overview stays and only the marker goes.
        h.floats.erase("map.x");
        h.floats.erase("map.y");
        h.put(row + 0xc, uint8_t(2));
        sample();
        check(h.nums["map.ready"] && !h.floats.count("map.x") && !h.floats.count("map.y"),
              "interior row hides the moving pointer");
        h.put(row + 0xc, uint8_t(3));
        h.put(tex, uint16_t(999));
        sample();
        check(!h.nums["map.ready"] && h.nums["live.ready"] && h.nums["controls.map_unavailable"],
              "unsupported asset optional and fail closed");
        h.put(tex, uint16_t(9));
        h.put(mapctx + 0x2f2, uint16_t(0));
        sample();
        check(!h.nums["map.ready"] && h.nums["live.ready"], "stale same-field variant rejected");
        h.put(mapctx + 0x2f2, uint16_t(1));
        h.put(row + 3, uint8_t(0));
        sample();
        check(!h.nums["map.ready"], "stale roadmap variant rejected");
        h.put(row + 3, uint8_t(1));
        h.put(mapctx + 0x1f0, std::numeric_limits<float>::quiet_NaN());
        sample();
        check(!h.nums["map.ready"] && h.nums["live.ready"],
              "nonfinite map does not invalidate stats");
        h.put(mapctx + 0x1f0, scale);
        h.put(map_task, uint32_t(3));
        sample();
        check(!h.nums["map.ready"], "dead map task rejected");
        h.put(map_task, uint32_t(2));
        h.put(name, char('X'));
        h.put(map_task + 0x80, map_task);
        sample();
        check(!h.nums["map.ready"] && h.nums["live.ready"], "cyclic task list terminates");
        check(!h.nums["controls.field"], "cyclic registry cannot prove field controls safe");
        h.reset();
        sample();
        check(!h.nums["map.ready"] && h.texts["map.area"].empty() && !h.floats.count("map.x"),
              "lost map clears all optional output");
    }

    check(h.nums["state.mode"] == 1, "town mode");
    // Synthetic MSG fixture exercises the public outputs and touch gates without game blobs.
    {
        constexpr U msg = 0x90004000, bmd = 0x91000000;
        h.put(msg, std::array<uint8_t, 0x90>{});
        h.put(bmd, std::array<uint8_t, 0x400>{});
        h.put(bmd + 4, uint32_t(0x400));
        h.put(bmd + 8, uint32_t(0x3147534d));
        h.put(bmd + 0x18, uint32_t(2));
        h.put(bmd + 0x24, int32_t(0x100 - 0x24));
        h.put(bmd + 0x28, uint32_t(1));
        h.put(bmd + 0x2c, int32_t(0x200 - 0x2c));
        h.put(bmd + 0x30, int32_t(0x80 - 0x30));
        h.put(bmd + 0x34, uint32_t(1));
        h.put(bmd + 0x80, int32_t(0x90 - 0x80));
        h.put(bmd + 0x90, std::array<char, 6>{'G', 'u', 'i', 'd', 'e', 0});
        h.put(bmd + 0x118, uint16_t(2));
        h.put(bmd + 0x11c, int32_t(0x180 - 0x11c));
        h.put(bmd + 0x120, int32_t(0x1a0 - 0x120));
        h.put(bmd + 0x180, std::array<char, 6>{'F', 'i', 'r', 's', 't', 0});
        h.put(bmd + 0x1a0, std::array<char, 7>{'S', 'e', 'c', 'o', 'n', 'd', 0});
        h.put(bmd + 0x21a, uint16_t(3));
        for (unsigned i = 0; i < 3; ++i) {
            h.put(bmd + 0x220 + 4 * i, int32_t(0x280 + 16 * i - (0x220 + 4 * i)));
            h.put(bmd + 0x280 + 16 * i, std::array<char, 2>{char('A' + i), 0});
        }
        h.put(msg + 8, bmd);
        h.put(msg + 0x30, bmd + 0x100);
        h.put(msg + 0x38, U(0x92000000));
        h.put(msg + 0x40, int8_t(1));
        h.put(msg + 0x4c, uint16_t(2));
        h.put(h.base + 0x231a820 + 30 * 0x40, msg);
        h.put(h.base + 0x1eacf38, int32_t(30));
        sample();
        check(h.texts["dialogue.text"] == "First" && h.texts["dialogue.speaker"] == "Guide" &&
                  h.nums["controls.message"] && !h.nums["controls.choice"],
              "message and advance gate");
        h.put(bmd + 0x180, std::array<uint8_t, 16>{0xf5, 0x99, 2, 1, 0, 0, 1, 1, 1, 1, 'F', 'i',
                                                   'r', 's', 't', 0});
        sample();
        check(h.texts["dialogue.text"] == "First" && h.nums["controls.message"],
              "ten-byte F599 effect control preserves text and advance gate");
        h.put(msg + 0x4a, uint16_t(1));
        sample();
        check(h.texts["dialogue.text"] == "Second", "page advances");
        h.put(msg + 0x40, int8_t(0));
        h.put(msg + 0x4a, uint16_t(2));
        h.put(msg + 0x68, uint32_t(1));
        h.put(msg + 0x70, U(0x92000100));
        h.put(msg + 0x78, uint32_t(2));
        h.put(msg + 0x80, int16_t(2));
        h.put(msg + 0x82, uint16_t(1));
        h.put(msg + 0x84, uint16_t(2));
        h.put(msg + 0x86, uint16_t(2));
        h.put(h.base + 0x1eacf38, int32_t(-1));
        sample();
        check(h.nums["controls.choice"] && !h.nums["controls.message"] &&
                  h.nums["dialogue.choice_count"] == 2 && h.nums["choice.1.selected"] &&
                  h.texts["choice.0.text"] == "A" && h.texts["choice.1.text"] == "C" &&
                  h.texts["dialogue.text"] == "Second",
              "masked choices and live selection");
        h.put(bmd + 0x1a0, std::array<uint8_t, 3>{0xf1, 0x81, 0});
        sample();
        check(!h.nums["controls.choice"] && !h.nums["choice.0.present"],
              "unsupported substitution disables stale choice controls");
        h.put(msg + 0x80, int16_t(0));
        sample();
        check(!h.nums["dialogue.ready"] && h.texts["dialogue.text"].empty() &&
                  !h.nums["controls.choice"] && !h.nums["controls.message"],
              "dialogue closure clears");
        h.reset();
    }
    h.put(h.fieldctx + 0x1f0, uint16_t(151));
    sample();
    check(h.nums["state.mode"] == 2, "dungeon mode");
    h.put(h.fieldctx + 0x1f0, uint16_t(3));
    constexpr U message = 0x90004000, manager = 0x90005000, menu_task = 0x90008000,
                name = 0x90008200;
    h.put(h.base + 0x1eacf38, int32_t(30));
    h.put(h.base + 0x231a820 + 30 * 0x40, message);
    h.put(message + 0x38, U(0x90009000));
    h.put(message + 0x40, int8_t(1));
    sample();
    check(h.nums["state.mode"] == 4, "normal message overlay");
    h.put(h.base + 0x1eacf38, int32_t(-1));
    h.put(message + 0x40, int8_t(0));
    h.put(message + 0x70, U(0x90009000));
    h.put(message + 0x80, int16_t(2));
    sample();
    check(h.nums["state.mode"] == 4, "choice overlay without normal MSG index");
    h.put(message + 0x80, int16_t(-1));
    sample();
    check(h.nums["state.mode"] == 1, "ended choice returns to town");
    h.put(h.base + 0x231a820 + 63 * 0x40, message + 0x100);
    h.put(message + 0x170, U(0x90009000));
    h.put(message + 0x180, int16_t(2));
    sample();
    check(h.nums["state.mode"] == 4, "last choice slot");
    h.put(message + 0x170, U(0));
    sample();
    check(h.nums["state.mode"] == 1, "choice requires renderer");
    h.put(h.base + 0x2209f50, manager);
    h.put(manager + 0x2c40, menu_task);
    h.put(manager + 0x2c30, U(0));
    h.put(menu_task, uint32_t(2));
    h.put(menu_task + 0x18, name);
    h.put(menu_task + 0x80, U(0));
    h.put(name, std::array<char, 10>{'C', 'A', 'M', 'P', '_', 'M', 'A', 'I', 'N', 0});
    sample();
    check(h.nums["state.mode"] == 5, "registered menu task");
    h.put(menu_task, uint32_t(3));
    sample();
    check(h.nums["state.mode"] == 1, "dead registered menu ignored");
    h.put(menu_task, uint32_t(2));
    h.put(manager + 0x2c40, U(0));
    h.put(manager + 0x2c30, menu_task);
    h.put(menu_task + 0x70, U(0));
    sample();
    check(h.nums["state.mode"] == 5, "second task list menu");
    h.put(manager + 0x2c30, U(0));
    sample();
    check(h.nums["state.mode"] == 1, "unregistered cached menu is ignored");
    h.put(manager + 0x2c40, menu_task);
    h.put(menu_task + 0x18, U(0));
    h.put(menu_task + 0x80, menu_task + 0x100);
    h.put(menu_task + 0x100, uint32_t(2));
    h.put(menu_task + 0x118, U(0));
    h.put(menu_task + 0x180, menu_task + 0x200);
    h.put(menu_task + 0x200, uint32_t(2));
    h.put(menu_task + 0x218, U(0));
    h.put(menu_task + 0x280, menu_task + 0x100);
    sample();
    check(h.nums["state.mode"] == 1, "non-head task cycle bounded");
    h.put(manager + 0x2c40, U(0));
    h.put(h.fieldtask, uint32_t(3));
    sample();
    check(!h.nums["live.ready"] && !h.nums["party.0.present"], "dying field task clears");
    h.put(h.fieldtask, uint32_t(2));
    h.put(h.fieldtask + 0xb8, U(118));
    sample();
    check(!h.nums["live.ready"], "stale field token rejected");
    h.put(h.ctx + 4, int32_t(5));
    sample();
    check(h.nums["live.ready"] && h.nums["state.mode"] == 3, "battle independent of field task");
    h.put(h.ctx + 4, int32_t(4));
    h.put(h.fieldtask + 0xb8, U(117));
    h.put(h.ctx + 4, int32_t(3));
    sample();
    check(!h.nums["live.ready"] && !h.nums["live.money"] && !h.nums["party.0.present"] &&
              h.texts["live.date"].empty(),
          "loading clears previous");
    h.put(h.ctx + 4, int32_t(4));
    h.put(h.ctx, uint32_t(1));
    sample();
    check(!h.nums["live.ready"], "pending transition");
    h.put(h.ctx, uint32_t(0));
    h.tear = true;
    sample();
    check(!h.nums["live.ready"], "torn party");
    h.tear = false;
    h.put(h.base + h.units + 0x1ce2, uint16_t(1));
    sample();
    check(!h.nums["live.ready"], "duplicate party");
    h.put(h.base + h.units + 0x1ce2, uint16_t(2));
    h.put(h.dateptr + 2, uint8_t(99));
    sample();
    check(h.nums["live.ready"] && !h.nums["date.ready"], "invalid date isolated");
    h.put(h.dateptr + 2, uint8_t(5));
    h.put(h.base + h.scene, U(-0x40));
    sample();
    check(!h.nums["live.ready"], "pointer overflow");
    h.put(h.base + h.scene, h.task);
    h.api.build_id[0] = 0;
    sample();
    check(!h.nums["live.ready"], "cached BID invalidation");
    h.api.build_id[0] = 0xd4;
    sample();
    check(h.nums["live.ready"], "valid identity recovery");
    h.api.main_size = 0x1000;
    sample();
    check(!h.nums["live.ready"], "cached main size invalidation");
    h.api.main_size = 0x3200000;
    h.base += 0x100000000;
    h.api.main_base = h.base;
    h.reset();
    sample();
    check(h.nums["live.ready"], "ASLR rebase");
    // Change only money LDR immediate: 0x5cc -> 0x6cc. Old global stays plausible,
    // new global differs, forcing a real decode rather than fixed offset success.
    m->destroy(r);
    h.code(0x8808c4, {0xb946cd00});
    h.put(h.base + h.money + 0x100, uint32_t(765432));
    r = OpenAll(m->create(&h.api, "{}"));
    sample();
    check(h.nums["live.money"] == 765432, "LDR immediate decoded");
    m->destroy(r);
    // ADRP one additional page (immlo wraps 2->3): money page +0x1000.
    h.reset();
    h.code(0x8808c0, {0xf000cf88});
    h.put(h.base + h.money + 0x1000, uint32_t(456789));
    r = OpenAll(m->create(&h.api, "{}"));
    sample();
    check(h.nums["live.money"] == 456789, "ADRP immediate decoded");
    m->destroy(r);
    h.reset();
    h.code(0x8808c0, {0x90ffc408});
    h.put(h.base + 0x1005cc, uint32_t(345678));
    r = OpenAll(m->create(&h.api, "{}"));
    sample();
    check(h.nums["live.money"] == 345678, "negative ADRP sign extension");
    m->destroy(r);
    h.reset();
    h.code(0x8808c8, {0xd503201f});
    r = OpenAll(m->create(&h.api, "{}"));
    sample();
    check(!h.nums["live.ready"], "signature mismatch");
    m->destroy(r);
    SocialTests(m);
    BgmTests(m);
    TextFailClosedTests(m);
    PackageConfigTests(m);
    RosterTests(m);
    PersonaTests(m);
    MenuTests(m);
    InventoryTests(m);
    Data2Tests(m);
    // Dialogue substitutions resolve their globals from the D4B1 accessors (the words ResolveText
    // matches or decodes).
    auto substitution_page = [&](bool break_name_accessor) {
        h.reset();
        h.code(0xce1a10, {0xb000a948, 0xf944e108, 0xb9400100, 0xd65f03c0});
        h.code(0xce4880, {0xd000a928, 0xf944e908, 0xb9400100, 0xd65f03c0});
        h.code(0x880a8c, {0xd000d013, 0x9122ba73, 0x52800501});
        h.code(0x880ac0, {0x39c24274});
        h.code(0x880b1c, {0xd000d013, 0x91221a73, 0x52800281});
        h.code(0x880b34, {0x39c2e274});
        h.code(0x880bac, {0xd000d013, 0x91226a73, 0x52800281});
        h.code(0x880bc4, {0x39c29274});
        h.code(0x88bfa0, {0x9000a7c8, 0x9116e108, 0x530c3c09, 0xf8695901, 0x12002c00, 0xd61f0020});
        h.code(0x89bdb0, {0x9000d088, 0x9119e108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100,
                          0xd65f03c0});
        h.code(0x89bdd0, {0x9000d088, 0x911a2108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100,
                          0xd65f03c0});
        h.code(0x89bdf0, {0x9000d088, 0x911a6108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100,
                          0xd65f03c0});
        h.code(0x89be10, {0x9000d088, 0x911aa108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100,
                          0xd65f03c0});
        h.code(0x89be30, {0x9000d088, 0x911ae108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100,
                          0xd65f03c0});
        h.code(0x89be50, {0x9000d088, 0x911b2108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100,
                          0xd65f03c0});
        h.code(0x89be70, {0x9000d088, 0x911b6108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100,
                          0xd65f03c0});
        h.code(0x89be90, {0x9000d088, 0x911ba108, 0x92403c0a, 0xa9402508, 0x786a7929, 0x8b090100,
                          0xd65f03c0});
        h.code(0x869970, {0xb000d028, 0xf9460508, 0xb9401109, 0x8b090108});
        h.code(0x869820, {0xb000d028, 0xf945ed08, 0xb9401109, 0x52800c8a});
        // 88BFA0 dispatch table (main+1D835B8): category -> name getter
        h.put(h.base + 0x1d835b8, U(h.base + 0x89bdb0));
        h.put(h.base + 0x1d835c0, U(h.base + 0x89bdd0));
        h.put(h.base + 0x1d835c8, U(h.base + 0x89bdf0));
        h.put(h.base + 0x1d835d0, U(h.base + 0x89be10));
        h.put(h.base + 0x1d835d8, U(h.base + 0x89be30));
        h.put(h.base + 0x1d835e0, U(h.base + 0x89be50));
        h.put(h.base + 0x1d835e8, U(h.base + 0x89be90));
        h.put(h.base + 0x1d835f0, U(h.base + 0x89be70));
        h.put(h.base + 0x1d835f8, U(h.base + 0x88bce0));
        if (break_name_accessor)
            h.code(0x880b34, {0x39c2e275});
        const U msg = 0x90004000, bmd = 0x91000000, strings = 0x91800000;
        h.put(h.base + 0x2282886, std::array<char, 0x14>{'K', 'a', 'i', 0});
        h.put(h.base + 0x228289a, std::array<char, 0x14>{'T', 'a', 'n', 'a', 'k', 'a', 0});
        h.put(h.base + 0x228293e, int8_t(-1));
        h.put(h.base + 0x220a9c0, U(strings));
        h.put(h.base + 0x220a9d0, U(strings));
        h.put(strings, uint32_t(0));
        h.put(h.base + 0x22ab6a8, U(strings + 0x100)); // item category 3: {names, offsets}
        h.put(h.base + 0x22ab6b0, U(strings + 0x200));
        h.put(strings + 0x100, std::array<char, 6>{'B', 'r', 'e', 'a', 'd', 0});
        h.put(strings + 0x200 + 2 * 5, uint16_t(0));
        h.put(strings + 0x300, std::array<char, 3>{'2', '0', 0});
        h.put(msg, std::array<uint8_t, 0x90>{});
        h.put(msg + 0x198, std::array<U, 32>{strings + 0x300});
        h.put(bmd, std::array<uint8_t, 0x400>{});
        h.put(bmd + 4, uint32_t(0x400));
        h.put(bmd + 8, uint32_t(0x3147534d));
        h.put(bmd + 0x18, uint32_t(1));
        h.put(bmd + 0x24, int32_t(0x100 - 0x24));
        h.put(bmd + 0x118, uint16_t(1));
        h.put(bmd + 0x11a, uint16_t(0xffff));
        h.put(bmd + 0x11c, int32_t(0x180 - 0x11c));
        h.put(bmd + 0x180, std::array<uint8_t, 24>{'H',  'i', ' ', 0xf1, 0x83, ',',  ' ', 0xf2,
                                                   0x44, 1,   1,   ' ',  0xf4, 0x84, 1,   1,
                                                   1,    1,   6,   0x31, 0xf1, 0x21, 0,   0});
        h.put(msg + 8, bmd);
        h.put(msg + 0x30, bmd + 0x100);
        h.put(msg + 0x38, U(0x92000000));
        h.put(msg + 0x40, int8_t(1));
        h.put(h.base + 0x231a820 + 30 * 0x40, msg);
        h.put(h.base + 0x1eacf38, int32_t(30));
        r = OpenAll(m->create(&h.api, "{}"));
        sample();
        const bool ready = h.nums["dialogue.ready"] == 1;
        const bool advance = h.nums["controls.message"] == 1;
        const std::string text = h.texts["dialogue.text"];
        m->destroy(r);
        return std::make_tuple(ready, advance, text);
    };
    {
        auto [ready, advance, text] = substitution_page(false);
        check(ready && advance && text == "Hi Kai Tanaka, 20 Bread",
              "resolved name, variable and item substitutions");
        auto [broken_ready, broken_advance, broken] = substitution_page(true);
        check(broken_ready && !broken_advance && broken == "Read the dialogue on the main screen.",
              "a mismatched name accessor only disables that substitution");
    }
    Battle2Tests(m, h);
    std::cout
        << "PASS public ABI synthetic reader tests (matched accessor words; synthetic data)\n";
}
