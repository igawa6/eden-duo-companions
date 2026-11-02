// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the two pieces of sw_* that are pure data/logic and need no live game:
//   1. sw_pk8::DecryptInPlace / DecodeParty on synthetic PK8s (sw_test_fixtures.h) and, with
//      --private-data, against a real known-good vector: party slot 1 of a post-game Sword 1.3.2
//      save (SW_FIXTURES_DIR/party_slot1_mirror.bin, never in this repository), whose Python
//      save-reading script's "info" reports species #150 Lv.100. That checks the C++ port
//      bit-for-bit against the script's own port of PKHeX's PokeCrypto, not just "does it run".
//   2. sw_type_chart's 18x18 effectiveness table, spot-checked against well-known matchups
//      (including the classic Electric-vs-Gyarados and Ice-vs-dragon/flying quad-damage cases).

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "sw_addresses.h"
#include "sw_build.h"
#include "sw_helper_mailbox.h"
#include "sw_dex_index.h"
#include "sw_evolution.h"
#include "sw_item_pouch.h"
#include "sw_effectiveness_text.h"
#include "sw_level.h"
#include "sw_move_data.h"
#include "sw_move_names.h"
#include "sw_personal_stats.h"
#include "sw_pk8.h"
#include "sw_party_details.h"
#include "sw_species_icon_table.h"
#include "sw_species_icons.h"
#include "sw_species_names.h"
#include "sw_test_fixtures.h"
#include "sw_encounter_fixture.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include "sw_text.h"
#include "sw_type_chart.h"
#include "sw_zone.h"
#include "sw_zukan.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The module's own exported getter (0100ABF008968000.cpp, compiled into this same test binary --
// see CMakeLists.txt). Declared extern "C" exactly as DSMOD_SDK_EXPORT_MODULE generates it.
extern "C" const EdenDsmodModuleApi* eden_dsmod_get_module(std::uint32_t host_abi_version,
                                                           std::uint64_t host_abi_hash);

namespace {

// Real bytes for the --private-data checks, loaded by LoadPrivateData() from SW_FIXTURES_DIR (a
// private save's blocks, see sw_test_fixtures.h) and SW_ROMFS_DIR (the game's own files). None of
// them is in this repository; without the two directories those checks are skipped.
struct PrivateData {
    std::vector<unsigned char> party_slot1;  // encrypted 0x158 KParty slot 0 (Mewtwo Lv.100)
    std::vector<unsigned char> stored[5];    // encrypted 0x148 "stored" blobs of KParty slots 0-4
    std::vector<unsigned char> k_item;       // decrypted KItem block
    std::vector<unsigned char> k_zukan, k_zukan_r1, k_zukan_r2; // decrypted Galar/Armor/Crown Zukan8
    std::vector<unsigned char> halfdex;      // K_ZUKAN of the derived half-dex save
    std::vector<unsigned char> personal;     // romfs:/bin/pml/personal/personal_total.bin
};
PrivateData g_real;

void LoadPrivateData() {
    g_real.party_slot1 = sw_test::Fixture("party_slot1_mirror.bin", sw_pk8::kSizeParty);
    const char* stored[5]{"live_stored_mewtwo.bin", "live_stored_eternatus.bin", "live_stored_zacian.bin",
                          "live_stored_zamazenta.bin", "live_stored_zarude.bin"};
    for (int i = 0; i < 5; ++i) g_real.stored[i] = sw_test::Fixture(stored[i], sw_pk8::kSizeStored);
    g_real.k_item = sw_test::Fixture("k_item.bin", sw_item_pouch::kBlockBytes);
    g_real.k_zukan = sw_test::Fixture("k_zukan.bin", 19200);
    g_real.k_zukan_r1 = sw_test::Fixture("k_zukan_r1.bin", 10128);
    g_real.k_zukan_r2 = sw_test::Fixture("k_zukan_r2.bin", 10080);
    g_real.halfdex = sw_test::Fixture("halfdex_k_zukan.bin", 19200);
    g_real.personal = sw_test::RomfsFile("/bin/pml/personal/personal_total.bin");
}

// Synthetic party blob (sw_test_fixtures.h): every field DecodeParty reads round-trips through the
// encrypt/shuffle inverse, for a shuffle-selecting EC (sv != 0) and the identity shuffle (sv == 0).
void TestPk8DecryptSyntheticParty() {
    for (const std::uint32_t ec : {0x5EED2A01u, 0x00001FFFu}) {
        sw_test::SynMon s = sw_test::SynPartyMember(0);
        s.ec = ec;
        s.tid = 0x1234; s.sid = 0x5678;
        s.moves = {33, 45, 52, 55};
        s.pp = {35, 40, 25, 25};
        s.level = 100;
        s.stats = {356, 200, 190, 180, 250, 170};
        s.nickname = u"Synth";
        const auto enc = sw_test::MakeParty(s);
        const auto plain = sw_test::PlainParty(s);
        assert(std::memcmp(enc.data() + 8, plain.data() + 8, sw_pk8::kSizeParty - 8) != 0); // really encrypted
        const sw_pk8::PartyMon m = sw_pk8::DecodeParty(enc.data());
        assert(m.species == 150 && m.level == 100 && m.experience == 1250000);
        assert(m.encryption_constant == ec && m.pid == s.pid && m.tid == 0x1234 && m.sid == 0x5678);
        assert(m.checksum_valid && m.species_in_range);
        assert(m.max_hp == 356);
        assert(m.moves[0] == 33 && m.moves[1] == 45 && m.moves[2] == 52 && m.moves[3] == 55);
        assert(m.pp[0] == 35 && m.pp[1] == 40 && m.pp[2] == 25 && m.pp[3] == 25);
        assert(m.current_hp_at_save == 240 && m.status_at_save == 0 && !m.egg);
        assert(m.nickname == "Synth");
        std::array<unsigned char, sw_pk8::kSizeParty> copy = enc;
        sw_pk8::DecryptInPlace(copy.data(), copy.size());
        assert(std::memcmp(copy.data(), plain.data(), copy.size()) == 0);
        // A flipped checksum must no longer validate.
        auto bad = enc;
        bad[6] ^= 0xFF;
        assert(!sw_pk8::DecodeParty(bad.data()).checksum_valid);
    }
    std::puts("sw_pk8: a synthetic party blob decrypts back to its own fields (shuffled and identity order)");
}

// --private-data: the real save's party slot 1 (SW_FIXTURES_DIR/party_slot1_mirror.bin).
void TestPk8DecryptKnownVector() {
    const sw_pk8::PartyMon m = sw_pk8::DecodeParty(g_real.party_slot1.data());
    assert(m.species == 150);                 // Mewtwo
    assert(m.level == 100);
    assert(m.experience == 1250000);           // level-100 slow-growth experience total
    assert(m.encryption_constant == 0x0942B8EDu);
    assert(m.checksum == 0x7E7E);
    assert(m.checksum_valid);                  // PokeCrypto.GetCHK recomputes to the same 0x7E7E
    assert(m.species_in_range);                // 150 <= kMaxKnownSpeciesSwSh
    // Party-stat-tail fields, live-verified against LIVE-READERS-132.md's 2026-10-24 session
    // (slot 1 of the same gb-635571 fixture: MaxHP matched the native 06-party.png screenshot's
    // x/max value exactly; moves/PP are the recorded live values for that boot's slot 1).
    assert(m.max_hp == 356);
    assert(m.moves[0] == 396);
    assert(m.moves[1] == 540);
    assert(m.moves[2] == 427);
    assert(m.moves[3] == 94);
    assert(m.pp[0] == 29);
    assert(m.pp[1] == 16);
    assert(m.pp[2] == 32);
    assert(m.pp[3] == 16);
    // Save-file-time-only snapshot fields (+0x8A/+0x94) -- NOT live, see sw_pk8.h's PartyMon
    // comment. Live-verified 2026-10-25 (LIVE-READERS-132.md "gate" session): this exact slot 1
    // decoded current_hp_at_save=240 right after a fresh gb-635571 import, matching the native
    // party screen's "240/356" exactly; status_at_save=0 (healthy).
    assert(m.current_hp_at_save == 240);
    assert(m.status_at_save == 0);

    // DecodeParty must not mutate its input (callers read live/guest memory and must be able to
    // retry): the source array above is `const`, so this is also enforced by the compiler.
    unsigned char copy[sw_pk8::kSizeParty];
    std::memcpy(copy, g_real.party_slot1.data(), sizeof copy);
    assert(std::memcmp(copy, g_real.party_slot1.data(), sizeof copy) == 0);

    // DecryptInPlace (the lower-level entry point DecodeParty wraps) must reproduce the same
    // species/level when called directly, confirming the two layers agree.
    sw_pk8::DecryptInPlace(copy, sw_pk8::kSizeParty);
    assert(sw_pk8::Rd16(copy + 0x08) == 150);
    assert(copy[0x148] == 100);

    std::puts("sw_pk8: known-good Sword 1.3.2 party slot 1 (Mewtwo Lv.100) decrypts correctly");
}

void TestPk8EmptySlotHasZeroSpecies() {
    // An all-zero slot (pv = 0, sv = 0, no shuffle) must decrypt to species 0, matching
    // the save-decoding script's "(empty slot data)" branch for an unused party slot.
    unsigned char empty[sw_pk8::kSizeParty]{};
    const sw_pk8::PartyMon m = sw_pk8::DecodeParty(empty);
    assert(m.species == 0);
    assert(!m.species_in_range); // 0 is not a valid species id -- a reader must treat this as empty, not garbage
    std::puts("sw_pk8: an all-zero slot decodes as empty (species 0)");
}

// sw_pk8::DecodeLiveStored on synthetic "stored" blobs: every field the live reader uses.
void TestDecodeLiveStoredSynthetic() {
    sw_test::SynMon s = sw_test::SynPartyMember(2);
    s.iv = {31, 0, 15, 7, 30, 1};
    s.ev = {252, 4, 0, 0, 252, 0};
    s.pp_ups = {3, 0, 1, 2};
    s.nature = 3; s.stat_nature = 10; s.gender = 1; s.form = 1;
    s.status = 0x40;
    const auto enc = sw_test::MakeStored(s);
    const sw_pk8::LivePartyMon m = sw_pk8::DecodeLiveStored(enc.data());
    assert(m.checksum_valid && m.species_in_range && m.species == 888 && m.form == 1);
    assert(m.experience == 1250000u && m.current_hp == 328 && m.status == 0x40);
    assert(m.iv_hp == 31 && m.ev_hp == 252 && m.iv[2] == 15 && m.iv[5] == 1 && m.ev[4] == 252);
    assert(m.held_item == 1 && m.ability == 3 && m.pid == s.pid && m.nature == 3 && m.stat_nature == 10);
    assert(m.gender == 1 && !m.egg);
    for (int i = 0; i < 4; ++i)
        assert(m.moves[i] == s.moves[static_cast<std::size_t>(i)] && m.pp[i] == 10 &&
               m.pp_ups[i] == s.pp_ups[static_cast<std::size_t>(i)]);
    s.egg = true;
    assert(sw_pk8::DecodeLiveStored(sw_test::MakeStored(s).data()).egg);
    std::puts("sw_pk8::DecodeLiveStored: a synthetic stored blob decodes every live-reader field");
}

// --private-data: sw_pk8::DecodeLiveStored against the 5 real encrypted "stored" blobs
// (SW_FIXTURES_DIR/live_stored_*.bin) -- the live party array's own slot format, as opposed to
// TestPk8DecryptKnownVector's save-mirror 0x158 format above. Confirms species/experience/IVs/
// EVs/current HP/status all decode correctly and the checksum validates, for every one of the
// gb-635571 fixture's 5 party members.
void TestDecodeLiveStoredAgainstRealBytes() {
    struct Case {
        const unsigned char* bytes;
        std::uint16_t species;
        std::uint16_t current_hp;
        std::uint8_t ev_hp;
    };
    const Case cases[]{
        {g_real.stored[0].data(), 150, 240, 14},
        {g_real.stored[1].data(), 890, 424, 14},
        {g_real.stored[2].data(), 888, 328, 14},
        {g_real.stored[3].data(), 889, 328, 14},
        {g_real.stored[4].data(), 893, 351, 0},
    };
    for (const auto& c : cases) {
        const sw_pk8::LivePartyMon m = sw_pk8::DecodeLiveStored(c.bytes);
        assert(m.checksum_valid);
        assert(m.species_in_range);
        assert(m.species == c.species);
        assert(m.experience == 1250000u); // every fixture member is Slow-growth, level 100
        assert(m.iv_hp == 31);
        assert(m.ev_hp == c.ev_hp);
        assert(m.current_hp == c.current_hp);
        assert(m.status == 0);
    }
    std::puts("sw_pk8::DecodeLiveStored: all 5 gb-635571 fixture party members' real encrypted "
              "'stored' blobs decode correctly (species/EXP/IV/EV/current HP/status)");
}

// End-to-end cross-check (the task's own required gate): sw_level::LevelFromExp +
// sw_level::ComputeMaxHp, fed the real EXP/IV/EV from the live-stored blobs above plus each
// species' real base HP/growth rate from the actual personal_total.bin file, must reproduce the
// save-snapshot mirror's own Level/MaxHP EXACTLY for all 5 fixture party members (Mewtwo 356
// Lv.100, Eternatus 424, Zacian 328, Zamazenta 328, Zarude 351) -- otherwise the computed MaxHP
// must never ship. It does, for all 5.
void TestPartySummaryStatsAndMatchups() {
    EdenDsmodHostApi host{};
    host.read_romfs = [](void*, const char* path, std::uint64_t offset, void* out, std::size_t size) -> std::size_t {
        if (std::strcmp(path, sw_personal_stats::kPersonalTablePath) != 0 ||
            offset > g_real.personal.size() || size > g_real.personal.size() - offset) return 0;
        if (out) std::memcpy(out, g_real.personal.data() + offset, size);
        return size;
    };
    auto mon = sw_pk8::DecodeLiveStored(g_real.stored[0].data());
    sw_party_details::Personal personal;
    assert(sw_party_details::LoadPersonal(host, mon.species, mon.form, personal));
    std::array<std::uint16_t, 6> stats{};
    assert(sw_party_details::Stats(mon, personal, 100, stats));
    std::array<unsigned char, sw_pk8::kSizeParty> actual{};
    std::memcpy(actual.data(), g_real.party_slot1.data(), actual.size());
    sw_pk8::DecryptInPlace(actual.data(), actual.size());
    // Independent native save party-stat tail, not a duplicate of the new formula.
    for (std::size_t i = 0; i < stats.size(); ++i)
        assert(stats[i] == sw_pk8::Rd16(actual.data() + 0x14A + 2 * i));
    assert(sw_party_details::LoadPersonal(host, 888, 1, personal));
    assert(personal.base[1] == 170 && personal.type1 == 17 && personal.type2 == 8);
    assert(!sw_party_details::LoadPersonal(host, 888, 255, personal));
    std::puts("sw_party_details: all six summary stats match the private save's native party tail; "
              "the real personal table serves a form row");
}

// Stat formula (natures, mints, hyper training, eggs) and type matchup text, no real data.
void TestPartyStatFormulasAndMatchups() {
    sw_party_details::Personal personal{};
    std::array<std::uint16_t, 6> stats{};
    personal.base.fill(100);
    sw_pk8::LivePartyMon sample;
    sample.stat_nature = 3; // Adamant: +Atk/-SpA, native nature order includes Speed third.
    assert(sw_party_details::Stats(sample, personal, 50, stats));
    assert(stats[0] == 160 && stats[1] == 115 && stats[3] == 105 && stats[4] == 94);
    sample.hyper_train = (1u << 0) | (1u << 5); // HP and Speed; bit5 is not SpD.
    assert(sw_party_details::Stats(sample, personal, 50, stats));
    assert(stats[0] == 175 && stats[3] == 120 && stats[5] == 105);
    sample.nature = 3; sample.stat_nature = 0; // Neutral mint overrides original Adamant.
    assert(sw_party_details::Stats(sample, personal, 50, stats) && stats[1] == 105 && stats[4] == 105);
    sample.egg = true;
    assert(!sw_party_details::Stats(sample, personal, 50, stats));
    assert(sw_party_details::OneLine("Native\\nability\\rdescription") == "Native ability description");
    const auto charizard = sw_party_details::TypeMatchups(9, 2);
    assert(charizard.weak.find(std::string(sw_str::TypeName(5)) + " ×4") != std::string::npos);
    assert(charizard.resist.find(std::string(sw_str::TypeName(11)) + " ×¼") != std::string::npos);
    assert(charizard.immune == std::string(sw_str::TypeName(4)) + " ×0");
    const auto pure_fire = sw_party_details::TypeMatchups(9, 9);
    assert(pure_fire.weak.find(std::string(sw_str::TypeName(10)) + " ×2") != std::string::npos);
    assert(pure_fire.weak.find(std::string(sw_str::TypeName(10)) + " ×4") == std::string::npos);
    std::puts("sw_party_details: mints/hypertraining/egg stat rules and dual-type matchup text pass");
}

void TestComputeMaxHpMatchesSaveMirror() {
    struct Case {
        const unsigned char* bytes;
        std::uint8_t base_hp;   // personal_total.bin byte +0x00, read directly this cycle
        int expect_level;
        std::uint16_t expect_max_hp;
    };
    const Case cases[]{
        {g_real.stored[0].data(), 106, 100, 356},
        {g_real.stored[1].data(), 140, 100, 424},
        {g_real.stored[2].data(), 92, 100, 328},
        {g_real.stored[3].data(), 92, 100, 328},
        {g_real.stored[4].data(), 105, 100, 351},
    };
    for (const auto& c : cases) {
        const sw_pk8::LivePartyMon m = sw_pk8::DecodeLiveStored(c.bytes);
        assert(m.checksum_valid);
        // personal_total.bin byte +0x15 == 5 (Slow) for all 5 of these species, confirmed
        // directly against the real file this cycle (sw_level.h's own file header comment).
        const auto growth = sw_level::GrowthRateFromByte(5);
        const int level = sw_level::LevelFromExp(growth, m.experience);
        assert(level == c.expect_level);
        const std::uint16_t max_hp = sw_level::ComputeMaxHp(c.base_hp, m.iv_hp, m.ev_hp, level);
        assert(max_hp == c.expect_max_hp);
    }
    std::puts("sw_level: computed Level+MaxHP from real EXP/IV/EV/base-HP matches the save "
              "mirror's own Level/MaxHP exactly for all 5 private fixture party members");
}

// Level + MaxHP from synthetic stored blobs (the standard Gen 3+ HP formula, worked by hand), and
// every growth rate's level-100 threshold.
void TestComputeMaxHpSynthetic() {
    struct Case {
        std::uint8_t base_hp, iv, ev;
        std::uint16_t expect_max_hp; // floor((2b + iv + ev/4) * 100/100) + 100 + 10
    };
    const Case cases[]{{106, 31, 14, 356}, {100, 0, 0, 310}, {106, 31, 252, 416}, {50, 10, 40, 230}};
    for (const auto& c : cases) {
        sw_test::SynMon s = sw_test::SynPartyMember(0);
        s.iv.fill(c.iv);
        s.ev = {c.ev, 0, 0, 0, 0, 0};
        const auto m = sw_pk8::DecodeLiveStored(sw_test::MakeStored(s).data());
        const int level = sw_level::LevelFromExp(sw_level::GrowthRateFromByte(5), m.experience);
        assert(level == 100);
        assert(sw_level::ComputeMaxHp(c.base_hp, m.iv_hp, m.ev_hp, level) == c.expect_max_hp);
    }
    // Erratic/Fluctuating/Fast/MediumFast/MediumSlow have no fixture case to cross-check
    // (every fixture member is Slow) -- exercise their EXP-threshold formulas against
    // the standard published values directly (Bulbapedia "Experience" page / PKHeX's own
    // Experience.cs tables) so a future game-session/fixture with one of these growth rates has
    // at least a formula-level regression check, not just "never tested".
    assert(sw_level::ExpThreshold(sw_level::GrowthRate::kFast, 100) == 800000u);
    assert(sw_level::ExpThreshold(sw_level::GrowthRate::kMediumFast, 100) == 1000000u);
    assert(sw_level::ExpThreshold(sw_level::GrowthRate::kMediumSlow, 100) == 1059860u);
    assert(sw_level::ExpThreshold(sw_level::GrowthRate::kSlow, 100) == 1250000u);
    assert(sw_level::ExpThreshold(sw_level::GrowthRate::kErratic, 100) == 600000u);
    assert(sw_level::ExpThreshold(sw_level::GrowthRate::kFluctuating, 100) == 1640000u);
    std::puts("sw_level: synthetic Level+MaxHP match the HP formula; all 6 growth rates' level-100 "
              "EXP thresholds match the published tables");
}

void TestTypeChartSingleType() {
    using sw_type_chart::Type;
    using sw_type_chart::EffectivenessPercent;
    using sw_type_chart::EffectivenessPercentDual;

    assert(EffectivenessPercent(Type::Water, Type::Fire) == 200);
    assert(EffectivenessPercent(Type::Fire, Type::Water) == 50);
    assert(EffectivenessPercent(Type::Electric, Type::Ground) == 0);
    assert(EffectivenessPercent(Type::Normal, Type::Ghost) == 0);
    assert(EffectivenessPercent(Type::Ghost, Type::Normal) == 0);
    assert(EffectivenessPercent(Type::Dragon, Type::Fairy) == 0);
    assert(EffectivenessPercent(Type::Fighting, Type::Ghost) == 0);
    assert(EffectivenessPercent(Type::Psychic, Type::Dark) == 0);
    assert(EffectivenessPercent(Type::Ground, Type::Flying) == 0);
    assert(EffectivenessPercent(Type::Poison, Type::Steel) == 0);
    assert(EffectivenessPercent(Type::Steel, Type::Fairy) == 200);
    assert(EffectivenessPercent(Type::Fairy, Type::Fire) == 50);
    assert(EffectivenessPercentDual(Type::Fairy, Type::Fire, Type::Flying) == 50);
    assert(EffectivenessPercentDual(Type::Fairy, Type::Fire, Type::Steel) == 25);
    assert(EffectivenessPercent(Type::Rock, Type::Flying) == 200);
    assert(EffectivenessPercent(Type::Grass, Type::Water) == 200);

    // A same-type single-type lookup collapses correctly through the dual-type helper.
    assert(EffectivenessPercentDual(Type::Fire, Type::Grass, Type::Grass) == 200);

    std::puts("sw_type_chart: single-type matchups match the standard Gen 6+ chart");
}

void TestTypeChartDualType() {
    using sw_type_chart::Type;
    using sw_type_chart::EffectivenessPercentDual;

    // Electric vs. a Water/Flying target (the textbook Gyarados case): 2x * 2x = 4x.
    assert(EffectivenessPercentDual(Type::Electric, Type::Water, Type::Flying) == 400);
    // Ice vs. a Dragon/Flying target: 2x * 2x = 4x.
    assert(EffectivenessPercentDual(Type::Ice, Type::Dragon, Type::Flying) == 400);
    // Fighting vs. a Ghost/Dark target: 0x (immune) regardless of the second type.
    assert(EffectivenessPercentDual(Type::Fighting, Type::Ghost, Type::Dark) == 0);
    // Normal vs. a Rock/Steel target: 0.5x * 0.5x = 0.25x.
    assert(EffectivenessPercentDual(Type::Normal, Type::Rock, Type::Steel) == 25);

    std::puts("sw_type_chart: dual-type matchups compose multiplicatively as expected");
}

// CRITICAL packaging check (LIVE-PACKAGE-TEST.md section 2): the host's `_build_match` config
// key does not mean "this module's own build-id check passed" -- it means "a per-build
// `<BUILD16>.json` data file was found in the package" (mod_module_host.cpp:488,
// `config["_build_match"] = !manifest.build_id_file.empty();`, and mod_manifest.cpp's
// `<BUILD16>.json` / `<build16_lower>.json` / `data.json` search). Every manifest page that
// gates its "Update Required" overlay on `hide_bind:"build_match"` is therefore permanently
// stuck showing that overlay unless the package ships that file -- which is exactly the bug this
// test guards against regressing. The file must be named for *this* build only (no bundled
// `data.json` fallback), so an installed update to any other build finds no match and the
// overlay correctly stays up for it.
void TestPerBuildDataFileExists() {
    // native/modules/0100ABF008968000_test.cpp -> native/modules -> native -> packages/...
    const std::filesystem::path here(__FILE__);
    const std::filesystem::path package_dir =
        here.parent_path().parent_path().parent_path() / "packages" / "PokemonSword" / "dualscreen";
    const std::string filename = std::string(sw_build::kBuildId16) + ".json";
    const std::filesystem::path data_file = package_dir / filename;

    assert(std::filesystem::exists(data_file) &&
           "per-build data file is missing -- the map page's build_match overlay will never hide "
           "(see LIVE-PACKAGE-TEST.md section 2)");

    std::ifstream in(data_file, std::ios::binary);
    assert(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const auto parsed = nlohmann::json::parse(ss.str(), /*cb=*/nullptr, /*allow_exceptions=*/false);
    assert(parsed.is_object() && "per-build data file must parse as a JSON object");

    const auto it = parsed.find("_build_id");
    assert(it != parsed.end() && it->is_string());
    const std::string build_id = it->get<std::string>();
    assert(build_id.rfind(sw_build::kBuildId16, 0) == 0 &&
           "per-build data file's _build_id must start with this module's own <BUILD16> prefix");

    // This module's readers are fully native (get_heap_begin()+offset, hardcoded in
    // sw_addresses.h) -- the per-build file exists only to satisfy the host's build_match probe,
    // so it must declare none of the manifest-replacing keys ParseManifestJson only reads from a
    // data file (PACKAGE_FORMAT.md section 4): adding them here would be silent, unused clutter
    // at best and a second, divergent source of truth at worst.
    for (const char* unexpected : {"points", "symbols", "spies", "patches", "metadata_anchor", "il2cpp"}) {
        assert(!parsed.contains(unexpected) &&
               "this module's per-build file should stay metadata-only -- see the comment above");
    }

    std::puts("packaging: the per-build data file exists and matches this module's build id "
              "(build_match will be 1 on 1.3.2 and 0 on any other build)");
}

// sw_text::ReadDat against a small, hand-built GameFreak .dat fixture (header + a 2-line section,
// each line XOR-stream-encrypted with the real kBaseKey/kAdvanceKey/RotateLeft3 schedule sw_text.h
// itself implements -- the cipher is its own inverse, so "encrypting" here is just calling the
// same transform forward). This is a synthetic fixture built to exercise the real on-disk format,
// not an excerpt of the real monsname.dat (which this module never ships) -- see
// sw_species_names.h for the real file this decoder reads from the player's own romfs at runtime.
// Line 0 stands in for the real file's own "Egg" placeholder (empty/unused-species slot); line 1
// stands in for a real species name.
void TestSpeciesNameDecode() {
    constexpr unsigned char kFixture[64]{
        0x01, 0x00, 0x02, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
        0x2C, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x00,
        0x0A, 0x00, 0x00, 0x00, 0xCC, 0x7C, 0x2C, 0xE4, 0x38, 0x22, 0xF9, 0x12, 0x4E, 0xA6, 0x10, 0x30,
        0x45, 0x83, 0x2E, 0x19, 0x01, 0xCA, 0x75, 0x53, 0x53, 0x98, 0xE1, 0xC1, 0xD4, 0x0C, 0x30, 0x65,
    };
    std::vector<std::string> strings;
    const bool ok = sw_text::ReadDat(kFixture, sizeof kFixture, strings);
    assert(ok);
    assert(strings.size() == 2);
    assert(strings[0] == "Egg");
    assert(strings[1] == "Bulbasaur");

    // sw_species_names::SpeciesName on top of that decoded table: species 0 ("Egg", the table's
    // own placeholder slot) must fail closed -- it is never a real party member's species --
    // species 1 must resolve, and an index past the decoded table must fail closed too.
    std::string name;
    assert(!sw_species_names::SpeciesName(strings, 0, name));
    assert(sw_species_names::SpeciesName(strings, 1, name));
    assert(name == "Bulbasaur");
    name.clear();
    assert(!sw_species_names::SpeciesName(strings, 2, name));
    assert(name.empty());

    std::puts("sw_text/sw_species_names: a real-format monsname.dat fixture decodes correctly "
              "and SpeciesName() fails closed for species 0 and an out-of-range index");
}

// Same decoder, same fixture bytes as TestSpeciesNameDecode above (sw_text::ReadDat does not
// care which .dat it is decoding), exercised through sw_move_names::MoveName instead of
// sw_species_names::SpeciesName -- battle.own.move<i>.name's fail-closed path in sw_reader.h.
void TestMoveNameDecode() {
    constexpr unsigned char kFixture[64]{
        0x01, 0x00, 0x02, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
        0x2C, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x00,
        0x0A, 0x00, 0x00, 0x00, 0xCC, 0x7C, 0x2C, 0xE4, 0x38, 0x22, 0xF9, 0x12, 0x4E, 0xA6, 0x10, 0x30,
        0x45, 0x83, 0x2E, 0x19, 0x01, 0xCA, 0x75, 0x53, 0x53, 0x98, 0xE1, 0xC1, 0xD4, 0x0C, 0x30, 0x65,
    };
    std::vector<std::string> strings;
    assert(sw_text::ReadDat(kFixture, sizeof kFixture, strings));
    assert(strings.size() == 2);

    std::string name;
    // Move id 0 is the table's own "no move"/placeholder slot -- an empty battle move slot must
    // never show a name, same rule as species id 0.
    assert(!sw_move_names::MoveName(strings, 0, name));
    assert(sw_move_names::MoveName(strings, 1, name));
    assert(name == "Bulbasaur"); // fixture content is a stand-in, not a real move name
    name.clear();
    assert(!sw_move_names::MoveName(strings, 2, name));
    assert(name.empty());

    std::puts("sw_text/sw_move_names: the same real-format .dat decoder works for wazaname.dat "
              "and MoveName() fails closed for move id 0 and an out-of-range index");
}

// --private-data: sw_move_data.h's FlatBuffers-table decode, against the REAL wazabin files of the
// player's romfs (SW_ROMFS_DIR/bin/pml/waza/waza%04u.wazabin, not hand-built) -- Thunderbolt/Growl/
// Earthquake/Pound, chosen to exercise every combination this task's field-presence convention
// needs: a non-default Type (Electric/Ground, field explicitly present) vs. the schema-default
// Normal (field omitted); a non-Status category vs. Status (Growl, field omitted); a present
// Power vs. a status move's omitted-defaults-to-0 Power.
void TestMoveDataDecode() {
    sw_move_data::MoveRecord r;
    const auto kWazaThunderbolt = sw_test::RomfsFile("/bin/pml/waza/waza0085.wazabin");
    const auto kWazaGrowl = sw_test::RomfsFile("/bin/pml/waza/waza0045.wazabin");
    const auto kWazaEarthquake = sw_test::RomfsFile("/bin/pml/waza/waza0089.wazabin");
    const auto kWazaPound = sw_test::RomfsFile("/bin/pml/waza/waza0001.wazabin");

    assert(sw_move_data::DecodeRecord(kWazaThunderbolt.data(), kWazaThunderbolt.size(), r));
    assert(r.valid);
    assert(r.type == 12);      // Electric, personal-table order
    assert(r.category == 2);   // Special
    assert(r.power == 90);
    assert(r.pp == 15);

    assert(sw_move_data::DecodeRecord(kWazaGrowl.data(), kWazaGrowl.size(), r));
    assert(r.valid);
    assert(r.type == 0);       // Normal -- field omitted (schema default), not a decode failure
    assert(r.category == 0);   // Status -- field omitted
    assert(r.power == 0);      // no power field at all for a status move
    assert(r.pp == 40);

    assert(sw_move_data::DecodeRecord(kWazaEarthquake.data(), kWazaEarthquake.size(), r));
    assert(r.valid);
    assert(r.type == 4);       // Ground
    assert(r.category == 1);   // Physical
    assert(r.power == 100);
    assert(r.pp == 10);

    assert(sw_move_data::DecodeRecord(kWazaPound.data(), kWazaPound.size(), r));
    assert(r.valid);
    assert(r.type == 0);       // Normal, omitted
    assert(r.category == 1);   // Physical, present
    assert(r.power == 40);
    assert(r.pp == 35);

    std::puts("sw_move_data: FlatBuffers Type/Category/Power/PP decode matches 4 real, "
              "well-known moves of the player's romfs (incl. the omitted-field-means-schema-default "
              "convention)");
}

// Fail-closed: a buffer too short to hold even a root offset (the synthetic full-record decode
// runs in 0100ABF008968000_reader_test.cpp's battle tests).
void TestMoveDataFailsClosed() {
    const unsigned char tiny[4]{0, 0, 0, 0};
    sw_move_data::MoveRecord bad;
    assert(!sw_move_data::DecodeRecord(tiny, sizeof tiny, bad));
    assert(!bad.valid);
    std::puts("sw_move_data: fails closed on a truncated buffer");
}

// sw_type_chart + sw_effectiveness_text, end to end as sw_reader.h's SampleBattle combines them
// for one move vs. a (possibly dual-typed) foe -- the same computation the battle page's per-move
// effectiveness column drives, exercised here without any live memory.
void TestMoveEffectivenessComputation() {
    using sw_type_chart::Type;

    // Thunderbolt (Electric) vs. a mono-Water foe: 2x, "Super effective".
    {
        const auto percent = sw_type_chart::EffectivenessPercentDual(Type::Electric, Type::Water, Type::Water);
        assert(percent == 200);
        assert(sw_effectiveness_text::TierForPercent(percent) == sw_effectiveness_text::kIdxSuper);
        assert(std::strcmp(sw_effectiveness_text::MultiplierText(percent), "\xc3\x97""2") == 0);
    }
    // Earthquake (Ground) vs. a Flying foe: immune, 0x, "No effect".
    {
        const auto percent = sw_type_chart::EffectivenessPercentDual(Type::Ground, Type::Flying, Type::Flying);
        assert(percent == 0);
        assert(sw_effectiveness_text::TierForPercent(percent) == sw_effectiveness_text::kIdxNone);
        assert(std::strcmp(sw_effectiveness_text::MultiplierText(percent), "\xc3\x97""0") == 0);
    }
    // Electric vs. a dual Water/Flying foe (e.g. Gyarados): 2x * 2x = 4x, still "Super effective"
    // (the game's own 4-tier word collapses x2/x4 into one word -- that collapse is intentional,
    // not a bug: the task's own multiplier text is what carries the x4-vs-x2 distinction).
    {
        const auto percent = sw_type_chart::EffectivenessPercentDual(Type::Electric, Type::Water, Type::Flying);
        assert(percent == 400);
        assert(sw_effectiveness_text::TierForPercent(percent) == sw_effectiveness_text::kIdxSuper);
        assert(std::strcmp(sw_effectiveness_text::MultiplierText(percent), "\xc3\x97""4") == 0);
    }
    // Normal vs. a mono-Rock foe: 0.5x, "Not very effective".
    {
        const auto percent = sw_type_chart::EffectivenessPercentDual(Type::Normal, Type::Rock, Type::Rock);
        assert(percent == 50);
        assert(sw_effectiveness_text::TierForPercent(percent) == sw_effectiveness_text::kIdxNotVery);
        assert(std::strcmp(sw_effectiveness_text::MultiplierText(percent), "\xc3\x97\xc2\xbd") == 0); // "×½"
    }
    // Normal vs. a dual Grass/Fire foe: 1x * 1x = 1x, "Effective" (the neutral word, still shown
    // -- the task asks for ×1 to be a real tier, not a blank).
    {
        const auto percent = sw_type_chart::EffectivenessPercentDual(Type::Normal, Type::Grass, Type::Fire);
        assert(percent == 100);
        assert(sw_effectiveness_text::TierForPercent(percent) == sw_effectiveness_text::kIdxNeutral);
        assert(std::strcmp(sw_effectiveness_text::MultiplierText(percent), "\xc3\x97""1") == 0);
    }
    // The quarter tier: Fire vs. dual Water/Rock -> 0.5x * 0.5x = 0.25x, "Not very effective".
    {
        const auto percent = sw_type_chart::EffectivenessPercentDual(Type::Fire, Type::Water, Type::Rock);
        assert(percent == 25);
        assert(sw_effectiveness_text::TierForPercent(percent) == sw_effectiveness_text::kIdxNotVery);
        assert(std::strcmp(sw_effectiveness_text::MultiplierText(percent), "\xc3\x97\xc2\xbc") == 0); // "×¼"
    }

    std::puts("sw_type_chart + sw_effectiveness_text: all 6 multiplier tiers (x4/x2/x1/half/"
              "quarter/x0) map to the correct native word index and multiplier glyph");
}

// sw_effectiveness_text.h's own .dat decode against a hand-built, format-faithful fixture (same
// style as TestSpeciesNameDecode/TestMoveNameDecode -- a small fixture exercising the real file
// format, not an excerpt of the real file, since the real btl_app.dat is ~200 lines and only
// Label()'s fail-closed behaviour needs checking here, not the full real content).
void TestEffectivenessLabelFailsClosed() {
    std::vector<std::string> strings = {"a", "b"};
    std::string out;
    assert(!sw_effectiveness_text::Label(strings, -1, out));
    assert(!sw_effectiveness_text::Label(strings, 2, out));
    assert(sw_effectiveness_text::Label(strings, 0, out));
    assert(out == "a");

    std::puts("sw_effectiveness_text::Label fails closed for a negative or out-of-range index");
}

// Current-zone place-name decode + tag validation (sw_zone::DecodeZoneName, sw_addresses.h's
// "location.zone_name" anchor) -- the 8-byte tag immediately preceding the live UTF-16LE place-
// name buffer must validate before anything after it is trusted, and the decode must stop at the
// first UTF-16 double-null rather than reading the buffer's full nominal capacity (the exact
// stale-trailing-bytes failure mode LIVE-READERS-132.md's 2026-10-25 session saw directly:
// "Wyndon" flown-to from "East Lake Axewell" left "...e Axewell" sitting right after the new
// name's own terminator, in the same reused buffer).
void TestZoneNameDecodeAndTagValidation() {
    auto put_u16 = [](unsigned char* buf, std::size_t off, char c) {
        buf[off] = static_cast<unsigned char>(c);
        buf[off + 1] = 0;
    };

    // Happy path: a real tag followed by "Wyndon", null-terminated well before the buffer ends.
    {
        unsigned char buf[0x98]{};
        std::memcpy(buf, sw_zone::kZoneTag, sizeof sw_zone::kZoneTag);
        const char* name = "Wyndon";
        for (std::size_t i = 0; name[i] != '\0'; ++i) put_u16(buf, 8 + i * 2, name[i]);
        std::string out;
        assert(sw_zone::DecodeZoneName(buf, sizeof buf, &out));
        assert(out == "Wyndon");
    }

    // Stale trailing bytes: the exact bug LIVE-READERS-132.md documented -- a shorter new name
    // ("Wyndon") reusing a buffer whose tail still holds the end of a longer previous name
    // ("...e Axewell"). The decode must stop at "Wyndon"'s own double-null, never append the
    // leftover bytes past it.
    {
        unsigned char buf[0x98]{};
        std::memcpy(buf, sw_zone::kZoneTag, sizeof sw_zone::kZoneTag);
        const char* stale_tail = "e Axewell";
        for (std::size_t i = 0; i < std::strlen(stale_tail); ++i)
            put_u16(buf, 8 + i * 2, stale_tail[i]);
        const char* name = "Wyndon"; // shorter than the stale tail -- overwritten in place
        for (std::size_t i = 0; name[i] != '\0'; ++i) put_u16(buf, 8 + i * 2, name[i]);
        put_u16(buf, 8 + std::strlen(name) * 2, '\0'); // the real double-null terminator
        std::string out;
        assert(sw_zone::DecodeZoneName(buf, sizeof buf, &out));
        assert(out == "Wyndon"); // not "Wyndone Axewell" or similar
    }

    // Tag mismatch: a single flipped byte in the 8-byte tag must refuse the whole record, not
    // decode the name bytes anyway -- this is the fail-closed gate that stands in for "this
    // might not be the {tag, name} record we think it is" (a torn read, a different build, a
    // relocated struct).
    {
        unsigned char buf[0x98]{};
        std::memcpy(buf, sw_zone::kZoneTag, sizeof sw_zone::kZoneTag);
        buf[7] ^= 0xFF;
        const char* name = "Wyndon";
        for (std::size_t i = 0; name[i] != '\0'; ++i) put_u16(buf, 8 + i * 2, name[i]);
        std::string out = "untouched";
        assert(!sw_zone::DecodeZoneName(buf, sizeof buf, &out));
        assert(out == "untouched"); // `out` must be left alone on a failed decode
    }

    // Too-short buffer (tag only, no room for even one UTF-16 code unit after it) fails closed
    // rather than reading past `len`.
    {
        unsigned char buf[8]{};
        std::memcpy(buf, sw_zone::kZoneTag, sizeof sw_zone::kZoneTag);
        std::string out;
        assert(!sw_zone::DecodeZoneName(buf, sizeof buf, &out));
    }

    // Bounded length: a buffer with NO terminator at all within the read window must stop at
    // kMaxZoneNameChars, not walk off the end of `len` looking for one.
    {
        std::vector<unsigned char> buf(8 + sw_zone::kMaxZoneNameChars * 2 + 40, 'A');
        std::memcpy(buf.data(), sw_zone::kZoneTag, sizeof sw_zone::kZoneTag);
        for (std::size_t i = 8; i + 1 < buf.size(); i += 2) { buf[i] = 'A'; buf[i + 1] = 0; }
        std::string out;
        assert(sw_zone::DecodeZoneName(buf.data(), buf.size(), &out));
        assert(out.size() == sw_zone::kMaxZoneNameChars);
    }

    // A lone UTF-16 surrogate code unit refuses rather than guesses at a combine -- no real
    // Galar/Isle of Armor/Crown Tundra place name needs one.
    {
        unsigned char buf[0x98]{};
        std::memcpy(buf, sw_zone::kZoneTag, sizeof sw_zone::kZoneTag);
        buf[8] = 0x00; buf[9] = 0xD8; // 0xD800, a high surrogate
        std::string out;
        assert(!sw_zone::DecodeZoneName(buf, sizeof buf, &out));
    }

    // sw_zone::FindZoneId / kZoneRegion cross-check (the den-table's own zone lookup): a Wild
    // Area zone's romfs name (place_name line kZonePlaceLine, fake tables here) resolves to its
    // region; a town or any other non-den zone correctly misses.
    {
        sw_zone::PlaceLines lines;
        for (auto& table : lines) table.assign(249, "");
        lines[0][sw_zone::kZonePlaceLine[2]] = "Watchtower Ruins";
        lines[0][sw_zone::kZonePlaceLine[32]] = "Slippery Slope";
        lines[0][100] = "Wyndon";
        assert(sw_zone::FindZoneId("Watchtower Ruins", lines) == 2);
        assert(sw_zone::kZoneRegion[2] == sw_str::Region::kGalar);
        assert(sw_zone::FindZoneId("Slippery Slope", lines) == 32);
        assert(sw_zone::kZoneRegion[32] == sw_str::Region::kCrown);
        assert(sw_zone::kZoneRegion[17] == sw_str::Region::kArmor && sw_zone::kZoneRegion[31] == sw_str::Region::kArmor);
        assert(sw_zone::FindZoneId("Wyndon", lines) == -1);
    }

    std::puts("sw_zone: DecodeZoneName validates the 8-byte tag, stops at the first double-null "
              "(no stale trailing bytes), bounds its own length, refuses a lone surrogate, and "
              "FindZoneId resolves real zones while missing towns");
}

// FULL SPECIES->ICON REMAP FIX (2026-10-25 task): sw_species_icon_table.h is now generated from
// the real romfs (tools/pokemon-sword/gen_species_icon_table.py decoded personal_total.bin's own
// per-species icon-id field, byte offset 76), replacing the old table that only covered species
// 1..649 and therefore showed no sprite at all for the fixture party's own Mewtwo-and-up
// legendaries. Spot-checks a representative few: the box-art legendary trio + Zarude (icon ids
// remapped far from their dex numbers), two more remapped Galar-native species, the one
// documented form override (Mr. Mime), a pass-through low-numbered species, and a "Dexit" species
// that must still fail closed (not in this game's dex at all).
void TestSpeciesIconLookup() {
    std::uint16_t icon_id = 0;
    std::uint8_t form = 0xFF;
    char suffix = 0;

    const auto check = [&](std::uint16_t species, std::uint16_t want_icon, std::uint8_t want_form,
                            char want_suffix) {
        icon_id = 0; form = 0xFF; suffix = 0;
        assert(sw_species_icon_table::Lookup(species, icon_id, form, suffix));
        assert(icon_id == want_icon);
        assert(form == want_form);
        assert(suffix == want_suffix);
    };
    check(888, 938, 0, 'n'); // Zacian
    check(889, 939, 0, 'n'); // Zamazenta
    check(890, 940, 0, 'n'); // Eternatus
    check(893, 988, 0, 'n'); // Zarude
    check(839, 935, 0, 's'); // Coalossal (Galar-native, remapped)
    check(872, 945, 0, 's'); // Snom (Galar-native, remapped)
    check(122, 122, 1, 's'); // Mr. Mime: box icon id == dex number, but form 1 (Galarian look)
    check(1, 1, 0, 's');     // Bulbasaur: a plain pass-through, icon id == dex number

    // Weedle (13): cut from Sword/Shield's dex entirely ("Dexit") -- no romfs icon exists, must
    // fail closed, never guess a neighbouring species' icon.
    icon_id = 0; form = 0xFF; suffix = 0;
    assert(!sw_species_icon_table::Lookup(13, icon_id, form, suffix));

    char src[160]{};
    assert(sw_species_icons::IconSource(888, src, sizeof src));
    assert(std::string(src).find("poke_icon_0938_00n_n.bntx#poke_icon_0938_00n_n") != std::string::npos);
    std::memset(src, 0, sizeof src);
    assert(sw_species_icons::IconSource(122, src, sizeof src));
    assert(std::string(src).find("poke_icon_0122_01s_n.bntx#poke_icon_0122_01s_n") != std::string::npos);
    assert(!sw_species_icons::IconSource(13, src, sizeof src)); // still fails closed end-to-end
    assert(!sw_species_icons::IconSource(0, src, sizeof src));  // species 0 is never a real mon

    std::puts("sw_species_icon_table/sw_species_icons: full romfs-generated species->icon remap "
              "resolves the box-art legendary trio + Zarude + other Galar-native remaps and the "
              "Mr. Mime form override, and still fails closed for a 'Dexit' species");
}

// CRITICAL (the live check's finding #1, LIVE-PACKAGE-TEST.md section 2): the host sets
// config["_build_match"] (mod_module_host.cpp:488) but only ever *publishes* its own
// "build_match" snapshot int on an error path -- on every normal tick the module itself must
// read "_build_match" back out of its create() config and republish "build_match" every sample,
// the same way MetroidDread's module does. Drives the module's real exported entry points
// (eden_dsmod_get_module()->create()/sample()/destroy(), not a reimplementation) against a
// minimal fake host that answers "nothing is mapped" for every guest-memory call, so every other
// domain reader fails closed and this test can focus on build_match alone.
void TestBuildMatchPublishedEverySample() {
    const EdenDsmodModuleApi* api =
        eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    assert(api && "this module's own ABI version/hash must match the SDK headers it was built with");

    struct Ctx {
        std::map<std::string, std::int64_t> ints;
    } ctx;

    EdenDsmodHostApi host{};
    host.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    host.struct_size = sizeof(EdenDsmodHostApi);
    host.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    host.userdata = &ctx;
    host.title_id = sw_build::kSwordTitleId;
    std::memcpy(host.build_id, sw_build::kBuildId, sw_build::kKnownBuildIdBytes);
    host.get_heap_begin = [](void*) -> std::uint64_t { return 0; };
    host.is_mapped = [](void*, std::uint64_t, std::uint64_t) -> EdenDsmodBool { return EDEN_DSMOD_FALSE; };
    host.read_memory = [](void*, std::uint64_t, void*, std::size_t) -> EdenDsmodBool { return EDEN_DSMOD_FALSE; };
    host.publish_i64 = [](void* ud, const char* name, std::int64_t v) {
        static_cast<Ctx*>(ud)->ints[name] = v;
    };
    host.publish_text = [](void*, const char*, const char*) {};

    auto run = [&](const char* config_json, std::int64_t expect) {
        ctx.ints.clear();
        void* inst = api->create(&host, config_json);
        assert(inst && "create() must still succeed on the supported build regardless of _build_match");
        api->sample(inst, &host);
        assert(ctx.ints.count("build_match") == 1 && "build_match must be published on every sample");
        assert(ctx.ints.at("build_match") == expect);
        api->destroy(inst);
    };

    run(R"({"_build_match":true})", 1);   // the host found this build's per-build data file
    run(R"({"_build_match":false})", 0);  // host-side mismatch: must never be reported as 1
    run("{}", 0);                        // field absent entirely: fail closed, not "assume true"
    run("", 0);                          // no config at all: same fail-closed default

    std::puts("0100ABF008968000: _build_match is read from the host's create() config and "
              "republished as build_match (1/0) on every sample -- the map page's Update "
              "Required overlay now hides correctly on the supported build");
}

// Exercise the public module image contract and serialize a session reset against a
// blocked encounter read. The real host passes keys including the "module:" prefix.
void TestRuntimeImagesAndSessionReset() {
    const auto* api=eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION,EDEN_DSMOD_MODULE_ABI_HASH);
    const auto* ext=eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION,EDEN_DSMOD_EXT_HASH);
    assert(api && ext && ext->load_image && ext->configure);
    struct Context {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered=false,release=false;
        std::atomic<bool> configuring=false,configured=false;
    } ctx;
    sw_test_encounters::Fixtures(false);
    EdenDsmodHostApi host{};
    host.abi_version=EDEN_DSMOD_MODULE_ABI_VERSION;host.struct_size=sizeof host;
    host.abi_hash=EDEN_DSMOD_MODULE_ABI_HASH;host.title_id=sw_build::kSwordTitleId;
    std::memcpy(host.build_id,sw_build::kBuildId,sw_build::kKnownBuildIdBytes);
    host.userdata=&ctx;
    host.get_heap_begin=[](void*)->std::uint64_t{return 0;};
    host.is_mapped=[](void*,std::uint64_t,std::uint64_t)->EdenDsmodBool{return EDEN_DSMOD_FALSE;};
    host.read_memory=[](void*,std::uint64_t,void*,std::size_t)->EdenDsmodBool{return EDEN_DSMOD_FALSE;};
    host.read_romfs=[](void* u,const char* path,std::uint64_t offset,void* out,std::size_t size)->std::size_t {
        auto& c=*static_cast<Context*>(u);
        if(std::string_view(path)=="romfs:/bin/archive/field/resident/data_table.gfpak" && !out) {
            std::unique_lock lock(c.mutex);c.entered=true;c.cv.notify_all();
            c.cv.wait(lock,[&]{return c.release;});
        }
        return sw_test_encounters::Read(nullptr,path,offset,out,size);
    };
    void* instance=api->create(&host,R"({"_build_match":true})");assert(instance);
    bool received=false;
    const auto sink=+[](void* u,std::uint32_t w,std::uint32_t h,const std::uint8_t*,std::size_t n) {
        assert(w==124 && h==124 && n==124*124*4);*static_cast<bool*>(u)=true;
    };
    assert(ext->load_image(instance,&host,"module:sw:ui/disc_white",&received,sink)==EDEN_DSMOD_TRUE && received);
    assert(ext->load_image(instance,&host,"sw:ui/disc_white",&received,sink)==EDEN_DSMOD_FALSE);
    std::thread sample([&]{api->sample(instance,&host);});
    {
        std::unique_lock lock(ctx.mutex);
        assert(ctx.cv.wait_for(lock,std::chrono::seconds(10),[&]{return ctx.entered;}));
    }
    std::thread configure([&]{ctx.configuring=true;ext->configure(instance,nullptr);ctx.configured=true;});
    while(!ctx.configuring) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(!ctx.configured);
    { std::lock_guard lock(ctx.mutex);ctx.release=true;ctx.cv.notify_all(); }
    sample.join();configure.join();assert(ctx.configured);
    api->destroy(instance);
    std::puts("module UI: full image-key dispatch; configure waits for encounter sampling PASS");
}

// The same decoders on synthetic blocks: KItem slot bit layout, pouch bounds and the Rotom Bike
// forms; Zukan8 seen/caught; and the HOME resolution that gives Crown Tundra its display count
// (a species shown in one region's list with its flags in another region's block).
void TestBagPouchDecodeSynthetic() {
    using sw_item_pouch::DecodeSlotAt;
    std::vector<unsigned char> block(sw_item_pouch::kBlockBytes, 0);
    auto put = [&](int pouch, std::size_t slot, std::uint32_t item, std::uint32_t count, bool is_new, bool fav) {
        const std::uint32_t v = item | (count << 15) | (is_new ? 1u << 30 : 0u) | (fav ? 1u << 31 : 0u);
        sw_test::Put32(block.data() + sw_item_pouch::kPouches[pouch].byte_offset + slot * 4, v);
    };
    put(0, 0, 17, 995, false, false);
    put(0, 1, 18, 7, true, false);
    put(0, 6, 23, 1, false, true);
    put(sw_item_pouch::kKeyItemsPouch, 3, 1266, 1, false, false);
    put(4, 549, 50, 3, false, false); // last slot of the largest pouch
    const auto a = DecodeSlotAt(block.data(), block.size(), 0, 0);
    assert(a.item_id == 17 && a.count == 995 && !a.is_new && !a.is_favorite);
    const auto b = DecodeSlotAt(block.data(), block.size(), 0, 1);
    assert(b.item_id == 18 && b.count == 7 && b.is_new && !b.is_favorite);
    const auto c = DecodeSlotAt(block.data(), block.size(), 0, 6);
    assert(c.item_id == 23 && c.count == 1 && c.is_favorite);
    assert(DecodeSlotAt(block.data(), block.size(), 4, 549).item_id == 50);
    assert(DecodeSlotAt(block.data(), block.size(), 0, 59).item_id == 0);
    assert(DecodeSlotAt(block.data(), block.size(), 9, 0).item_id == 0);
    assert(DecodeSlotAt(block.data(), block.size(), 0, 999).item_id == 0);
    assert(DecodeSlotAt(block.data(), 16, 0, 0).item_id == 0);
    assert(sw_item_pouch::HasItem(block.data(), block.size(), sw_item_pouch::kKeyItemsPouch, 1266));
    assert(!sw_item_pouch::HasItem(block.data(), block.size(), 0, 1266));
    assert(sw_item_pouch::HasRotomBike(block.data(), block.size()));
    put(sw_item_pouch::kKeyItemsPouch, 3, 0, 0, false, false);
    assert(!sw_item_pouch::HasRotomBike(block.data(), block.size()));
    put(sw_item_pouch::kKeyItemsPouch, 0, sw_item_pouch::kRotomBikeItemId, 0, false, false); // count 0: not held
    assert(!sw_item_pouch::HasRotomBike(block.data(), block.size()));
    std::puts("sw_item_pouch: synthetic KItem block decodes id/count/new/favorite, bounds fail closed, "
              "every Rotom Bike form counts");
}

void TestZukanAndDexHomeSynthetic() {
    std::vector<unsigned char> galar(sw_dex_index::kGalarCount * sw_zukan::kEntrySize, 0);
    std::vector<unsigned char> armor(sw_dex_index::kArmorCount * sw_zukan::kEntrySize, 0);
    std::vector<unsigned char> crown(sw_dex_index::kCrownCount * sw_zukan::kEntrySize, 0);
    auto mark = [](std::vector<unsigned char>& blk, std::size_t i, int seen_region, bool caught) {
        if (seen_region >= 0) blk[i * sw_zukan::kEntrySize + static_cast<std::size_t>(seen_region) * 8 + 3] = 0x10;
        if (caught) blk[i * sw_zukan::kEntrySize + sw_zukan::kOfsCaught] = 1;
    };
    mark(galar, 0, 0, true);   // No. 1: seen + caught
    mark(galar, 1, 3, false);  // No. 2: seen (in the 4th seen bitfield only)
    mark(armor, 0, 1, true);
    mark(crown, 1, 2, false);
    const auto g = sw_zukan::CountEntries(galar.data(), galar.size(), sw_dex_index::kGalarCount);
    assert(g.total == 400 && g.seen == 2 && g.caught == 1);
    assert(!sw_zukan::DecodeEntry(galar.data(), galar.size(), 2).seen);
    assert(!sw_zukan::DecodeEntry(galar.data(), galar.size(), 9999).seen);

    // Personal table: species 10 Galar No. 1; 11 Armor No. 1 + Crown No. 1 (Armor home);
    // 12 Crown No. 2 only; 13 Galar No. 2 + Crown No. 3 (Galar home).
    std::vector<unsigned char> personal((sw_dex_index::kMaxKnownSpecies + 1) * sw_dex_index::kRecordSize, 0);
    auto dex = [&](int species, std::size_t ofs, std::uint16_t no) {
        sw_test::Put16(personal.data() + static_cast<std::size_t>(species) * sw_dex_index::kRecordSize + ofs, no);
    };
    dex(10, sw_dex_index::kGalarOfs, 1);
    dex(11, sw_dex_index::kArmorOfs, 1); dex(11, sw_dex_index::kCrownOfs, 1);
    dex(12, sw_dex_index::kCrownOfs, 2);
    dex(13, sw_dex_index::kGalarOfs, 2); dex(13, sw_dex_index::kCrownOfs, 3);
    sw_dex_index::ReverseIndex idx{};
    sw_dex_index::BuildReverseIndexFromBytes(personal.data(), personal.size(), idx);
    assert(idx.galar_home[0] == 10 && idx.galar_home[1] == 13 && idx.armor_home[0] == 11 && idx.crown_home[1] == 12);
    assert(idx.crown_display[0] == 11 && idx.crown_display[1] == 12 && idx.crown_display[2] == 13);
    assert(idx.home_region[11] == sw_dex_index::Home::kArmor && idx.home_index[11] == 1);
    assert(idx.home_region[13] == sw_dex_index::Home::kGalar && idx.home_index[13] == 2);
    auto resolve = [&](std::uint16_t species) -> sw_zukan::EntryState {
        const std::uint16_t hi = species ? idx.home_index[species] : 0;
        if (hi == 0) return {};
        switch (idx.home_region[species]) {
            case sw_dex_index::Home::kGalar: return sw_zukan::DecodeEntry(galar.data(), galar.size(), hi - 1);
            case sw_dex_index::Home::kArmor: return sw_zukan::DecodeEntry(armor.data(), armor.size(), hi - 1);
            case sw_dex_index::Home::kCrown: return sw_zukan::DecodeEntry(crown.data(), crown.size(), hi - 1);
            default: return {};
        }
    };
    int seen = 0, caught = 0;
    for (const auto species : idx.crown_display) {
        const auto st = resolve(species);
        seen += st.seen; caught += st.caught;
    }
    assert(seen == 3 && caught == 1); // a per-block-only read of the Crown block gives 1 / 0
    const auto raw = sw_zukan::CountEntries(crown.data(), crown.size(), sw_dex_index::kCrownCount);
    assert(raw.seen == 1 && raw.caught == 0);
    std::puts("sw_zukan/sw_dex_index: synthetic blocks decode seen/caught and Crown Tundra's display "
              "list resolves each species through its own HOME block");
}

// --private-data: sw_item_pouch::DecodeSlotAt against the REAL gb-635571 fixture's decrypted KItem
// block (SW_FIXTURES_DIR/k_item.bin -- dumped via tools/swsh_save.py, not hand-built).
// Spot-checks the Medicine pouch's maxed-out stack (matches the native 08-08-bag.png screenshot
// exactly: Potion/Antidote/.../Full Restore all x995), one Balls-pouch item, one Key-Items-pouch
// item (count=1, a "has it" flag not a real stack per PKHeX's own InventoryItem8 semantics), and
// the IsNew/IsFavorite flag bits, then cross-checks the *total* nonzero-slot count across all 9
// pouches against the number LIVE-READERS-132.md's own session already reported for this exact
// fixture (658 of 1214 slots) -- an independent agreement this test's pouch-offset table is
// complete and in the right order, not just that 7 known ids happen to decode right.
void TestBagPouchDecode() {
    using sw_item_pouch::DecodeSlotAt;

    // Medicine pouch (index 0): ids 17-23 (Potion..Full Restore) all count=995, matching the
    // native screenshot. Potion (slot 0) is the only slot with IsNew=false in this fixture.
    {
        const auto potion = DecodeSlotAt(g_real.k_item.data(), g_real.k_item.size(), 0, 0);
        assert(potion.item_id == 17);
        assert(potion.count == 995);
        assert(!potion.is_new);
        assert(!potion.is_favorite);

        const auto antidote = DecodeSlotAt(g_real.k_item.data(), g_real.k_item.size(), 0, 1);
        assert(antidote.item_id == 18);
        assert(antidote.count == 995);
        assert(antidote.is_new);

        const auto full_restore = DecodeSlotAt(g_real.k_item.data(), g_real.k_item.size(), 0, 6);
        assert(full_restore.item_id == 23); // Full Restore
        assert(full_restore.count == 995);
    }

    // An empty Medicine slot (past the 36 real items) must decode to item_id 0, never a stale
    // nonzero id read past the real data.
    {
        const auto empty = DecodeSlotAt(g_real.k_item.data(), g_real.k_item.size(), 0, 59);
        assert(empty.item_id == 0);
        assert(empty.count == 0);
    }

    // Rotom Bike obtained signal: the real fixture holds key item 1081 (and 1080 Fishing Rod);
    // the same block with that slot cleared (a pre-Bike bag) does not.
    {
        using sw_item_pouch::HasItem;
        assert(HasItem(g_real.k_item.data(), g_real.k_item.size(), sw_item_pouch::kKeyItemsPouch,
                       sw_item_pouch::kRotomBikeItemId));
        assert(!HasItem(g_real.k_item.data(), g_real.k_item.size(), 0, sw_item_pouch::kRotomBikeItemId));
        std::vector<unsigned char> pre = g_real.k_item;
        const auto& kp = sw_item_pouch::kPouches[sw_item_pouch::kKeyItemsPouch];
        for (std::size_t i = 0; i < kp.slot_count; ++i)
            if (sw_item_pouch::DecodeSlotAt(pre.data(), pre.size(), sw_item_pouch::kKeyItemsPouch, i).item_id ==
                sw_item_pouch::kRotomBikeItemId)
                std::fill_n(pre.begin() + kp.byte_offset + i * 4, 4, 0);
        assert(!HasItem(pre.data(), pre.size(), sw_item_pouch::kKeyItemsPouch, sw_item_pouch::kRotomBikeItemId));
        assert(!HasItem(pre.data(), 16, sw_item_pouch::kKeyItemsPouch, sw_item_pouch::kRotomBikeItemId));
        // Any Rotom Bike form counts: this fixture also holds 1266/1585/1586; with only the
        // water-upgraded 1266 left (Shield fixture gbt-576619's bag) the bike is still owned, and
        // with every form cleared it is not.
        assert(sw_item_pouch::HasRotomBike(pre.data(), pre.size()));
        std::vector<unsigned char> only1266 = pre, none = pre;
        for (std::size_t i = 0; i < kp.slot_count; ++i) {
            const auto id = sw_item_pouch::DecodeSlotAt(pre.data(), pre.size(), sw_item_pouch::kKeyItemsPouch, i).item_id;
            const bool bike = id == 1081 || id == 1266 || id == 1585 || id == 1586;
            if (bike && id != 1266) std::fill_n(only1266.begin() + kp.byte_offset + i * 4, 4, 0);
            if (bike) std::fill_n(none.begin() + kp.byte_offset + i * 4, 4, 0);
        }
        assert(sw_item_pouch::HasRotomBike(only1266.data(), only1266.size()));
        assert(!sw_item_pouch::HasRotomBike(none.data(), none.size()));
    }

    // Out-of-range pouch/slot indices fail closed (empty slot), never an out-of-bounds read.
    {
        const auto bad_pouch = DecodeSlotAt(g_real.k_item.data(), g_real.k_item.size(), 9, 0);
        assert(bad_pouch.item_id == 0);
        const auto bad_slot = DecodeSlotAt(g_real.k_item.data(), g_real.k_item.size(), 0, 999);
        assert(bad_slot.item_id == 0);
    }

    // Total nonzero slots across all 9 pouches, independently cross-checked against
    // LIVE-READERS-132.md's own count for this exact fixture ("658 nonzero" of 1214 slots).
    int total_nonzero = 0;
    for (int p = 0; p < sw_item_pouch::kPouchCount; ++p) {
        const auto& pd = sw_item_pouch::kPouches[p];
        for (std::size_t s = 0; s < pd.slot_count; ++s) {
            if (DecodeSlotAt(g_real.k_item.data(), g_real.k_item.size(), p, s).item_id != 0)
                ++total_nonzero;
        }
    }
    assert(total_nonzero == 658);

    std::puts("sw_item_pouch: real gb-635571 KItem block decodes Medicine x995 (matching the "
              "native 08-08-bag.png screenshot), fails closed for an empty slot and an "
              "out-of-range pouch/slot, and the full 9-pouch nonzero count (658) matches "
              "LIVE-READERS-132.md's independently reported total");
}

// --private-data: sw_zukan::CountEntries against the REAL gb-635571 fixture's decrypted Galar/Isle
// of Armor/Crown Tundra Zukan8 blocks (SW_FIXTURES_DIR/k_zukan{,_r1,_r2}.bin). This is the
// LOW-LEVEL per-block decode only -- a raw block's own N entries are NOT the same thing as a
// region's full "No. 1..N" Pokédex DISPLAY list (see sw_dex_index.h's header comment and
// TestDexRegionCountsViaHome below for the fix/full pipeline this low-level piece feeds into).
// For Galar there is no difference (every Galar-dex member is always Galar-homed), so this
// doubles as a real region-count check; for Crown Tundra it deliberately is NOT 210 here --
// 136 of the 210 Crown Tundra Pokédex entries are ALSO Galar/Isle-of-Armor dex members and have
// their real flags stored in THOSE blocks instead (Zukan8's single-home-per-species model),
// leaving only the 74 Crown-EXCLUSIVE species with any flag set in this raw Crown Tundra block.
void TestZukanDexCounts() {
    const auto galar = sw_zukan::CountEntries(g_real.k_zukan.data(), g_real.k_zukan.size(),
                                              sw_dex_index::kGalarCount);
    assert(galar.total == 400);
    assert(galar.seen == 400);
    assert(galar.caught == 400);

    const auto armor_raw = sw_zukan::CountEntries(g_real.k_zukan_r1.data(), g_real.k_zukan_r1.size(),
                                                   sw_dex_index::kArmorCount);
    assert(armor_raw.total == 211);

    const auto crown_raw = sw_zukan::CountEntries(g_real.k_zukan_r2.data(), g_real.k_zukan_r2.size(),
                                                   sw_dex_index::kCrownCount);
    assert(crown_raw.total == 210);
    assert(crown_raw.seen == 74);   // the Crown-EXCLUSIVE subset only -- see the comment above
    assert(crown_raw.caught == 74);

    // A single entry's decode, spot-checked: Galar entry index 0 must be both seen and caught
    // (the whole Galar region is 400/400 in this fixture, so this one entry is a representative
    // sample of CountEntries' own per-entry DecodeEntry, not a separate species claim).
    const auto e0 = sw_zukan::DecodeEntry(g_real.k_zukan.data(), g_real.k_zukan.size(), 0);
    assert(e0.seen && e0.caught);
    // Out-of-range entry index fails closed.
    const auto bad = sw_zukan::DecodeEntry(g_real.k_zukan.data(), g_real.k_zukan.size(), 9999);
    assert(!bad.seen && !bad.caught);

    std::puts("sw_zukan: real gb-635571 Zukan8 blocks decode Galar 400/400 seen/caught and the "
              "raw Isle of Armor/Crown Tundra blocks' own entry counts (211/210) correctly -- "
              "region DISPLAY totals (what the native screen shows) need the home-resolution "
              "pipeline, see TestDexRegionCountsViaHome");
}

// FIX for the Crown Tundra 74-vs-210 bug a game-session review caught live (fresh native capture
// of THIS exact fixture, 20261024T1752+0700-g0-g0-native: Galar 400/400, Isle of Armor 211/211,
// Crown Tundra Caught 210 / Seen 210). Exercises the REAL pipeline sw_reader.h's SampleDex now
// uses end to end against real data: sw_dex_index::BuildReverseIndexFromBytes on the real
// personal_total.bin (SW_ROMFS_DIR, --private-data) builds each region's
// DISPLAY list (every species with that region's own DexIndex field nonzero, independent of
// home) plus the per-species HOME resolution (Zukan8's single-home-per-species priority:
// Galar > Armor > Crown), then each displayed species' real seen/caught state is read from
// sw_zukan::DecodeEntry on WHICHEVER block (k_zukan/_r1/_r2) actually owns it -- not
// necessarily the region currently being displayed.
void TestDexRegionCountsViaHome() {
    sw_dex_index::ReverseIndex idx{};
    sw_dex_index::BuildReverseIndexFromBytes(g_real.personal.data(), g_real.personal.size(), idx);

    auto resolve = [&](std::uint16_t species) -> sw_zukan::EntryState {
        if (species == 0) return {};
        const auto home = idx.home_region[species];
        const std::uint16_t hi = idx.home_index[species];
        if (hi == 0) return {};
        switch (home) {
            case sw_dex_index::Home::kGalar:
                return sw_zukan::DecodeEntry(g_real.k_zukan.data(), g_real.k_zukan.size(), hi - 1);
            case sw_dex_index::Home::kArmor:
                return sw_zukan::DecodeEntry(g_real.k_zukan_r1.data(), g_real.k_zukan_r1.size(), hi - 1);
            case sw_dex_index::Home::kCrown:
                return sw_zukan::DecodeEntry(g_real.k_zukan_r2.data(), g_real.k_zukan_r2.size(), hi - 1);
            default:
                return {};
        }
    };

    auto count = [&](const std::uint16_t* display, int n, int& seen, int& caught) {
        seen = caught = 0;
        for (int i = 0; i < n; ++i) {
            const auto st = resolve(display[i]);
            if (st.seen) ++seen;
            if (st.caught) ++caught;
        }
    };

    int galar_seen, galar_caught, armor_seen, armor_caught, crown_seen, crown_caught;
    count(idx.galar_home.data(), sw_dex_index::kGalarCount, galar_seen, galar_caught);
    assert(galar_seen == 400);
    assert(galar_caught == 400);

    count(idx.armor_display.data(), sw_dex_index::kArmorCount, armor_seen, armor_caught);
    assert(armor_seen == 211);
    assert(armor_caught == 211);

    // The headline fix: Crown Tundra's 210-entry DISPLAY list now resolves to 210/210, matching
    // the fresh native capture exactly -- not the 74/210 a per-block-only read gives (see
    // TestZukanDexCounts above). Of the 210: 74 are Crown-homed (read from k_zukan_r2
    // directly), 135 are Galar-homed (read from k_zukan, all caught since Galar itself
    // is 400/400 in this fixture), and 1 is Armor-homed (read from k_zukan_r1).
    count(idx.crown_display.data(), sw_dex_index::kCrownCount, crown_seen, crown_caught);
    assert(crown_seen == 210);
    assert(crown_caught == 210);

    std::puts("sw_dex_index: real personal_total.bin + the real Zukan8 blocks, resolved through "
              "each displayed species' own HOME block, give Galar 400/400, Isle of Armor "
              "211/211 and Crown Tundra 210/210 -- matching the fresh native Pokedex capture "
              "exactly (fixes the 74/210 a per-block-only read gave)");
}

// One 72-byte evo_NNN.bin built from edges (8-byte records: method u16, argument u16, species u16,
// form u8, level u8 -- sw_evolution.h's own layout), zero after the last edge.
std::vector<unsigned char> EvoFile(std::initializer_list<sw_evolution::Edge> edges) {
    std::vector<unsigned char> f(72, 0);
    std::size_t o = 0;
    for (const auto& e : edges) {
        sw_test::Put16(f.data() + o, static_cast<std::uint16_t>(e.method));
        sw_test::Put16(f.data() + o + 2, static_cast<std::uint16_t>(e.argument));
        sw_test::Put16(f.data() + o + 4, static_cast<std::uint16_t>(e.species));
        f[o + 6] = static_cast<unsigned char>(e.form);
        f[o + 7] = static_cast<unsigned char>(e.level);
        o += 8;
    }
    return f;
}

// Evolution decode, the task's own worked example built from the published chain (Swinub(220)
// -> Piloswine(221) at level 33, method 4 "Level"; Piloswine -> Mamoswine(473) knowing move id 246,
// method 21 "KnowsMove", which `sw_move_names`'s `wazaname.dat` decode resolves to "Ancient
// Power" -- exactly the D2 mockup's own worked example); Mamoswine has no further entries (a
// fully-evolved species). With --private-data, TestEvolutionRealFiles checks the same edges
// against the player's romfs files.
void TestEvolutionDecodeSwinubChain() {
    const auto kEvoSwinub = EvoFile({{4, 0, 221, 0, 33}});
    const auto kEvoPiloswine = EvoFile({{21, 246, 473, 0, 0}});
    const auto kEvoMamoswine = EvoFile({}); // fully evolved: no entries

    const auto swinub_edges = sw_evolution::DecodeFile(kEvoSwinub.data(), kEvoSwinub.size());
    assert(swinub_edges.size() == 1);
    assert(swinub_edges[0].method == 4);
    assert(swinub_edges[0].species == 221);
    assert(swinub_edges[0].level == 33);

    const auto piloswine_edges = sw_evolution::DecodeFile(kEvoPiloswine.data(), kEvoPiloswine.size());
    assert(piloswine_edges.size() == 1);
    assert(piloswine_edges[0].method == 21);
    assert(piloswine_edges[0].argument == 246);
    assert(piloswine_edges[0].species == 473);

    const auto mamoswine_edges = sw_evolution::DecodeFile(kEvoMamoswine.data(), kEvoMamoswine.size());
    assert(mamoswine_edges.empty());

    sw_evolution::Table t{};
    sw_evolution::AddToTable(t, 220, swinub_edges);
    sw_evolution::AddToTable(t, 221, piloswine_edges);
    sw_evolution::AddToTable(t, 473, mamoswine_edges);
    assert(t.parent[221] == 220);
    assert(t.parent[473] == 221);
    assert(t.parent[220] == 0); // Swinub is the first stage

    // Chain built from ANY member must agree: back to Swinub, forward through all 3.
    for (const int start : {220, 221, 473}) {
        const auto chain = sw_evolution::BuildChain(t, start);
        assert(chain.size() == 3);
        assert(chain[0].species == 220 && chain[0].depth == 0);
        assert(chain[1].species == 221 && chain[1].depth == 1);
        assert(chain[1].via.method == 4 && chain[1].via.level == 33);
        assert(chain[2].species == 473 && chain[2].depth == 2);
        assert(chain[2].via.method == 21 && chain[2].via.argument == 246);
    }

    // Method text, with and without a name lookup available -- the fallback must stay honest
    // ("Lv. 33" / generic "Level up, knows a move"), the real lookup must produce the D2 text.
    const auto no_names = [](int, std::string&) { return false; };
    assert(sw_evolution::MethodText(sw_evolution::Edge{4, 0, 221, 0, 33}, no_names, no_names) == "Lv. 33");
    assert(sw_evolution::MethodText(sw_evolution::Edge{21, 246, 473, 0, 0}, no_names, no_names) ==
           "Level up, knows a move");
    const auto ancient_power = [](int move_id, std::string& out) {
        if (move_id != 246) return false;
        out = "Ancient Power";
        return true;
    };
    assert(sw_evolution::MethodText(sw_evolution::Edge{21, 246, 473, 0, 0}, no_names, ancient_power) ==
           "Ancient Power");
    // Eevee -> Sylveon (700): native method 29 checks friendship and a currently known move of
    // argument 17's type (Fairy), together.
    const auto sylveon_edge = EvoFile({{29, 17, 700, 0, 0}});
    const auto sylveon = sw_evolution::DecodeFile(sylveon_edge.data(), 8);
    assert(sylveon.size() == 1 && sylveon[0].species == 700);
    assert(sw_evolution::MethodText(sylveon[0], no_names, no_names) == "Friendship + Fairy move");
    assert(sw_evolution::MethodText(sw_evolution::Edge{29, 0, 700, 0, 0}, no_names, no_names) ==
           "Friendship + required move type"); // never guess Fairy for an unknown argument
    // Preserve required thresholds and sex/item constraints from shipped evolution rows.
    assert(sw_evolution::MethodText(sw_evolution::Edge{9, 0, 106, 0, 20}, no_names, no_names) ==
           "Lv. 20: Atk > Def");
    assert(sw_evolution::MethodText(sw_evolution::Edge{10, 0, 237, 0, 20}, no_names, no_names) ==
           "Lv. 20: Atk = Def");
    assert(sw_evolution::MethodText(sw_evolution::Edge{11, 0, 107, 0, 20}, no_names, no_names) ==
           "Lv. 20: Atk < Def");
    assert(sw_evolution::MethodText(sw_evolution::Edge{28, 0, 687, 0, 30}, no_names, no_names) ==
           "Lv. 30 (upside down)");
    assert(sw_evolution::MethodText(sw_evolution::Edge{30, 0, 675, 0, 32}, no_names, no_names) ==
           "Lv. 32: Dark-type in party");
    assert(sw_evolution::MethodText(sw_evolution::Edge{31, 0, 706, 0, 50}, no_names, no_names) ==
           "Lv. 50 in rain");
    assert(sw_evolution::MethodText(sw_evolution::Edge{34, 0, 678, 1, 25}, no_names, no_names) ==
           "Lv. 25 (female)");
    const auto dawn_stone = [](int id, std::string& out) {
        if (id != 109) return false;
        out = "Dawn Stone"; return true;
    };
    assert(sw_evolution::MethodText(sw_evolution::Edge{17, 109, 475, 0, 30}, dawn_stone, no_names) ==
           "Dawn Stone (male)");
    assert(sw_evolution::MethodText(sw_evolution::Edge{18, 109, 478, 0, 42}, dawn_stone, no_names) ==
           "Dawn Stone (female)");
    const auto razor_claw = [](int id, std::string& out) {
        if (id != 326) return false;
        out = "Razor Claw"; return true;
    };
    assert(sw_evolution::MethodText(sw_evolution::Edge{20, 326, 461, 0, 0}, razor_claw, no_names) ==
           "Level up, hold Razor Claw (Night)");

    std::puts("sw_evolution: evo_220/221/473.bin records decode to the Swinub -(Lv. 33)-> "
              "Piloswine -(knows move 246)-> Mamoswine chain exactly, from any starting member, "
              "with method text resolving to real game text when a name lookup is available");
}

// --private-data: the player's own romfs evolution files hold exactly the edges above.
void TestEvolutionRealFiles() {
    auto edges = [](int species) {
        char path[64];
        std::snprintf(path, sizeof path, "/bin/pml/evolution/evo_%03d.bin", species);
        const auto f = sw_test::RomfsFile(path);
        return sw_evolution::DecodeFile(f.data(), f.size());
    };
    assert(edges(220) == std::vector<sw_evolution::Edge>{{4, 0, 221, 0, 33}});
    assert(edges(221) == std::vector<sw_evolution::Edge>{{21, 246, 473, 0, 0}});
    assert(edges(473).empty());
    bool sylveon = false;
    for (const auto& e : edges(133)) sylveon |= e == sw_evolution::Edge{29, 17, 700, 0, 0};
    assert(sylveon);
    std::puts("sw_evolution: the romfs evo_220/221/473/133.bin files decode to the edges the synthetic test uses");
}

// --private-data: spoiler gating with the derived half-dex bytes (SW_FIXTURES_DIR/
// halfdex_k_zukan.bin; see
// private-data/pokemon-sword/saves/derived/gb-635571-halfdex/DERIVED.md): `set-dex` cleared every
// even 1-based Galar entry of the real gb-635571 fixture, which for Galar is also the native
// "No. NNN" display order (home == display, sw_dex_index.h), giving a genuine
// SEEN -> UNSEEN -> SEEN sandwich at Swinub/Piloswine/Mamoswine's REAL Galar regional dex numbers
// -- No. 075/076/077 (read live from `personal_total.bin`'s own PokeDexIndex field this session;
// NOT No. 007/008/009 as the D2 mockup's illustrative numbering suggested -- that mockup used
// placeholder numbers, not the real game's regional dex order, which interleaves the starter
// families first: confirmed live, label g0, the real No. 007 is Sobble). This proves the C++
// decoder (the same sw_zukan::DecodeEntry SampleDex's `resolve()` lambda calls) reports the
// derived fixture's bytes the way the spoiler rule needs, independent of any live boot.
void TestEvolutionSpoilerGatingHalfDex() {
    const auto counts = sw_zukan::CountEntries(g_real.halfdex.data(), g_real.halfdex.size(),
                                               sw_dex_index::kGalarCount);
    assert(counts.total == 400);
    assert(counts.seen == 200); // exactly half, by construction (every even entry zeroed)
    assert(counts.caught == 200);

    // 0-based entry index == (native "No. NNN") - 1 for Galar. Swinub/Piloswine/Mamoswine's real
    // Galar dex numbers (75/76/77), not the D2 mockup's illustrative 7/8/9 -- see comment above.
    const auto swinub = sw_zukan::DecodeEntry(g_real.halfdex.data(), g_real.halfdex.size(), 74); // No. 075
    const auto piloswine = sw_zukan::DecodeEntry(g_real.halfdex.data(), g_real.halfdex.size(), 75); // No. 076
    const auto mamoswine = sw_zukan::DecodeEntry(g_real.halfdex.data(), g_real.halfdex.size(), 76); // No. 077
    assert(swinub.seen && swinub.caught);       // odd entry: untouched by set-dex, stays seen
    assert(!piloswine.seen && !piloswine.caught); // even entry: zeroed by set-dex, now unseen
    assert(mamoswine.seen && mamoswine.caught); // odd entry: untouched by set-dex, stays seen

    // Even entries generally: spot-check a handful beyond the worked example, confirming the
    // even/odd pattern holds across the block, not just at this one chain's 3 positions.
    for (int one_based : {2, 4, 50, 100, 400}) {
        const auto e = sw_zukan::DecodeEntry(g_real.halfdex.data(), g_real.halfdex.size(), one_based - 1);
        assert(!e.seen && !e.caught);
    }
    for (int one_based : {1, 3, 51, 101, 399}) {
        const auto e = sw_zukan::DecodeEntry(g_real.halfdex.data(), g_real.halfdex.size(), one_based - 1);
        assert(e.seen && e.caught);
    }

    std::puts("sw_zukan: the derived gb-635571-halfdex Galar block decodes to exactly 200/400 "
              "seen+caught (every even No. cleared), with No. 075 Swinub seen / No. 076 "
              "Piloswine unseen / No. 077 Mamoswine seen -- the evolution row's own "
              "SEEN->UNSEEN->SEEN spoiler-gating verification case");
}

// Review M6: Eevee's chain has 9 members; the card shows 6 and the selected member is always
// among them (first stage + a window of later members around it).
void TestEvolutionEeveeWindow() {
    sw_evolution::Table t{};
    std::vector<sw_evolution::Edge> eevee;
    const int evos[] = {134, 135, 136, 196, 197, 470, 471, 700};
    for (int e : evos) eevee.push_back({6, 0, static_cast<std::uint16_t>(e), 0, 0});
    sw_evolution::AddToTable(t, 133, eevee);
    for (int e : evos) sw_evolution::AddToTable(t, e, {});
    const auto full = sw_evolution::BuildChain(t, 700);
    assert(full.size() == 9 && full[0].species == 133 && full[8].species == 700);
    for (int sel : {133, 134, 197, 470, 471, 700}) {
        const auto vis = sw_evolution::VisibleChain(sw_evolution::BuildChain(t, sel), sel);
        assert(static_cast<int>(vis.size()) == sw_evolution::kMaxChainMembers);
        assert(vis[0].species == 133);
        bool found = false;
        for (const auto& m : vis) found = found || m.species == sel;
        assert(found);
        for (std::size_t k = 1; k < vis.size(); ++k) assert(vis[k].via.species == vis[k].species);
    }
    assert(sw_evolution::VisibleChain(sw_evolution::BuildChain(t, 700), 700).back().species == 700);
    std::puts("sw_evolution: Eevee's 9-member chain keeps Eevee and the selected member in the "
              "6-slot evolution row (Leafeon/Glaceon/Sylveon no longer cut off)");
}

// ---- Module-glue (DirectAction/DirectUpkeep): dead-alias dispatch + selected_command reset ----
// Review gap (B-helper.md finding #5): every test above drives sw_reader::Reader directly or
// (TestBuildMatchPublishedEverySample) the module's real create()/sample()/destroy() entry points
// against a host that fails every guest read closed. Nothing exercises DirectAction/DirectUpkeep
// -- the OpenBag/OpenPokemon staging path and the module's action-name whitelist -- at the
// module-glue level. This drives the real eden_dsmod_get_module()/eden_dsmod_get_extensions()
// entry points (create/configure/sample/on_action/destroy), with the SMALLEST synthetic guest-
// memory set that reaches native_battle_command_waiting()==true and
// battle_view().{in_battle,own_ok}==true: one own BattleMon struct (no foe, no moves -- neither
// CheckOpenMenu nor this test needs them), SampleBattleRaid's rule!=2 fast path (no roster/
// party-array chain), and the native command-menu FSM state (main+0x8021D0 step 5) at a synthetic
// battle_client_. This is NOT a full reader-pipeline test (0100ABF008968000_reader_test.cpp's
// job) -- just enough for the module's own battle-command glue.
namespace direct_fixture {

void PutU16(std::vector<unsigned char>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<unsigned char>(v);
    b[off + 1] = static_cast<unsigned char>(v >> 8);
}
void PutU32(std::vector<unsigned char>& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + static_cast<std::size_t>(i)] = static_cast<unsigned char>(v >> (8 * i));
}
void PutU64(std::vector<unsigned char>& b, std::size_t off, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[off + static_cast<std::size_t>(i)] = static_cast<unsigned char>(v >> (8 * i));
}

struct Region {
    std::uint64_t base;
    std::vector<unsigned char> data;
};

// Guest memory + publish sink (same technique as 0100ABF008968000_reader_test.cpp's FakeHost,
// duplicated here rather than shared -- this tree's existing per-test-file convention).
struct Host {
    std::uint64_t heap_begin = 0, main_base = 0;
    std::vector<Region> regions;
    std::map<std::string, std::int64_t> ints;
    std::map<std::string, std::string> texts;

    const Region* Find(std::uint64_t addr, std::uint64_t size) const {
        for (const auto& r : regions)
            if (addr >= r.base && size <= r.data.size() && addr - r.base <= r.data.size() - size)
                return &r;
        return nullptr;
    }
};

EdenDsmodBool IsMapped(void* ud, std::uint64_t addr, std::uint64_t size) {
    return static_cast<Host*>(ud)->Find(addr, size) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}
EdenDsmodBool ReadMemory(void* ud, std::uint64_t addr, void* out, std::size_t size) {
    auto* h = static_cast<Host*>(ud);
    const Region* r = h->Find(addr, size);
    if (!r) return EDEN_DSMOD_FALSE;
    std::memcpy(out, r->data.data() + (addr - r->base), size);
    return EDEN_DSMOD_TRUE;
}
std::uint64_t HeapBegin(void* ud) { return static_cast<Host*>(ud)->heap_begin; }
void PublishI64(void* ud, const char* name, std::int64_t v) { static_cast<Host*>(ud)->ints[name] = v; }
void PublishText(void* ud, const char* name, const char* v) { static_cast<Host*>(ud)->texts[name] = v ? v : ""; }

// Guest mailbox model (same technique as sw_helper_mailbox_test.cpp's Fake/GuestFrame/AliveFrame,
// duplicated here -- this file cannot include that file's anonymous-namespace helpers). Unlike
// that file's own Fake, GuestFrame here always honors `answer` for a pending request: the native
// legality re-checks sw_guest_helper.S performs are sw_helper_mailbox_test.cpp's own job; this
// model only needs to drive the mailbox protocol (request/ack) the module-glue code reacts to.
struct Mailbox {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(sw_helper::MailboxSize, 0);
    std::uint32_t Get(std::uint32_t o) const {
        std::uint32_t v;
        std::memcpy(&v, bytes.data() + o, 4);
        return v;
    }
    void Set(std::uint32_t o, std::uint32_t v) { std::memcpy(bytes.data() + o, &v, 4); }
    std::uint32_t kind = sw_helper::BattleKindAllowed; // wild single, offline

    void GuestFrame(sw_helper::Result answer = sw_helper::Result::Ok, std::uint32_t value = 0) {
        Set(sw_helper::OffsetGuestMagic, sw_helper::GuestMagic);
        Set(sw_helper::OffsetWaitBeat, Get(sw_helper::OffsetWaitBeat) + 1);
        Set(sw_helper::OffsetBattleKind, kind);
        if (Get(sw_helper::OffsetModuleMagic) != sw_helper::ModuleMagic) return;
        const std::uint32_t seq = Get(sw_helper::OffsetReqSeq);
        if (seq == Get(sw_helper::OffsetAckSeq)) return;
        Set(sw_helper::OffsetResultCode, static_cast<std::uint32_t>(answer));
        Set(sw_helper::OffsetResultValue, answer == sw_helper::Result::Ok ? value : 0);
        Set(sw_helper::OffsetAckBeat, Get(sw_helper::OffsetWaitBeat));
        Set(sw_helper::OffsetAckSeq, seq);
    }
    void AliveFrame(std::uint64_t client) {
        Set(sw_helper::OffsetGuestMagic, sw_helper::GuestMagic);
        Set(sw_helper::OffsetAliveBeat, Get(sw_helper::OffsetAliveBeat) + 1);
        Set(sw_helper::OffsetAliveClient, static_cast<std::uint32_t>(client));
        Set(sw_helper::OffsetAliveClient + 4, static_cast<std::uint32_t>(client >> 32));
    }
};

EdenDsmodBool LoadU32(void* ud, std::uint32_t o, std::uint32_t* v) {
    auto* m = static_cast<Mailbox*>(ud);
    if (o % 4 || static_cast<std::size_t>(o) + 4 > m->bytes.size()) return EDEN_DSMOD_FALSE;
    *v = m->Get(o);
    return EDEN_DSMOD_TRUE;
}
EdenDsmodBool StoreU32(void* ud, std::uint32_t o, std::uint32_t v) {
    auto* m = static_cast<Mailbox*>(ud);
    if (o % 4 || static_cast<std::size_t>(o) + 4 > m->bytes.size()) return EDEN_DSMOD_FALSE;
    m->Set(o, v);
    return EDEN_DSMOD_TRUE;
}

constexpr std::uint64_t kHeapBegin = 0x2105400000ull; // boot 1's real HeapLow() (shared convention)
constexpr std::uint64_t kMainBase = 0x7100000000ull;
constexpr std::uint64_t kClient = 0x9000000000ull; // opaque; distinct from heap/main ranges above

// Builds the guest-memory regions SampleBattle needs for in_battle=1/own_ok=1 with the native
// command menu waiting (main+0x8021D0 step 5) -- the smallest set that satisfies CheckOpenMenu.
Host BuildHost() {
    Host h;
    h.heap_begin = kHeapBegin;
    h.main_base = kMainBase;

    const auto* a_zone = sw_addresses::Find("location.zone_name");
    const auto* a_screen = sw_addresses::Find("location.current_screen");
    const auto* a_own = sw_addresses::Find("battle.own_mon");
    assert(a_zone && a_zone->verified && a_screen && a_screen->verified && a_own && a_own->verified);

    // zone_cache_ready_ (kTitle -> kLoading): the 8-byte tag (sw_zone.h) + a non-empty name.
    std::vector<unsigned char> zone_buf(a_zone->size, 0);
    static constexpr unsigned char kZoneTag[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0x08, 0x00, 0x03, 0x00};
    std::memcpy(zone_buf.data(), kZoneTag, sizeof kZoneTag);
    const char* name = "Wyndon";
    for (std::size_t i = 0; name[i] != '\0'; ++i)
        PutU16(zone_buf, 8 + i * 2, static_cast<unsigned char>(name[i]));
    h.regions.push_back({h.heap_begin + a_zone->sysbot_offset, std::move(zone_buf)});

    // CurrentScreen: the battle sentinel from the very first tick (sw_reader.h's kTitle case
    // never looks at it; kLoading's does, taking the instant kLoading -> kBattle path).
    std::vector<unsigned char> screen_buf(4, 0);
    PutU32(screen_buf, 0, 0xFFFFFFFFu);
    h.regions.push_back({h.heap_begin + a_screen->sysbot_offset, std::move(screen_buf)});

    // Own BattleMon (sw_battle.h::Decode): species/max_hp/cur_hp/level only -- no moves, no foe.
    std::vector<unsigned char> own_buf(a_own->size, 0);
    PutU16(own_buf, 0x70, 150); // species (Mewtwo)
    PutU16(own_buf, 0x72, 200); // max_hp
    PutU16(own_buf, 0x74, 200); // cur_hp
    own_buf[0x7C] = 100;        // level
    h.regions.push_back({h.heap_begin + a_own->sysbot_offset, std::move(own_buf)});

    // Native command-menu FSM (client+0x560): state_fn=main+0x8021D0, state_step=5 -- "the
    // command menu waits" (sw_reader.h's battle_command_waiting_).
    std::vector<unsigned char> state_buf(0x14, 0);
    PutU64(state_buf, 0, kMainBase + 0x8021D0);
    PutU32(state_buf, 0x10, 5);
    h.regions.push_back({kClient + 0x560, std::move(state_buf)});

    // sw_battle_raid::Read's rule!=2 fast path (kind_known=true, not a raid): client+0x58 ->
    // main (any nonzero pointer), main+0x150 -> rule (0, not 2) -- no roster at all needed.
    const std::uint64_t raid_main = kClient + 0x100000;
    std::vector<unsigned char> raid_ptr(8, 0);
    PutU64(raid_ptr, 0, raid_main);
    h.regions.push_back({kClient + 0x58, std::move(raid_ptr)});
    h.regions.push_back({raid_main + 0x150, std::vector<unsigned char>(4, 0)}); // rule = 0

    return h;
}

EdenDsmodHostApi MakeHostApi(Host& h) {
    EdenDsmodHostApi host{};
    host.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    host.struct_size = sizeof(EdenDsmodHostApi);
    host.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    host.userdata = &h;
    host.title_id = sw_build::kSwordTitleId;
    std::memcpy(host.build_id, sw_build::kBuildId, sw_build::kKnownBuildIdBytes);
    host.main_base = h.main_base;
    host.get_heap_begin = &HeapBegin;
    host.is_mapped = &IsMapped;
    host.read_memory = &ReadMemory;
    host.publish_i64 = &PublishI64;
    host.publish_text = &PublishText;
    return host;
}

} // namespace direct_fixture

void TestDirectBattleSelectedCommandResetAndDeadAliases() {
    using namespace direct_fixture;
    const EdenDsmodModuleApi* api =
        eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const EdenDsmodModuleExtensions* ext =
        eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    assert(api && ext && ext->on_action && ext->configure);

    Host fake = BuildHost();
    const EdenDsmodHostApi host = MakeHostApi(fake);

    void* inst = api->create(&host, nullptr);
    assert(inst && "create() must succeed on the supported build/title");

    Mailbox mailbox;
    mailbox.Set(sw_helper::EpochOffset, 1); // "the host owns the epoch word" -- live throughout
    EdenDsmodHostExtensions hx{};
    hx.version = EDEN_DSMOD_EXT_VERSION;
    hx.struct_size = sizeof hx;
    hx.abi_hash = EDEN_DSMOD_EXT_HASH;
    hx.userdata = &mailbox;
    hx.mailbox_address = 0x1000; // opaque; nothing in this test reads through it directly
    hx.mailbox_size = sw_helper::MailboxSize;
    hx.load_u32 = &LoadU32;
    hx.store_u32 = &StoreU32;
    ext->configure(inst, &hx); // Initialize()s the battle/HUD/field bridges against `mailbox`

    // Two guest frames: the first establishes the beat/alive baseline (sw_helper::Bridge's own
    // "the first value read is history, not a beat" rule), the second makes menu_waiting/
    // battle_alive true -- the same two ticks that carry the reader kTitle -> kLoading -> kBattle
    // (SampleState: zone valid -> kLoading; the battle sentinel -> kBattle, instant).
    mailbox.GuestFrame();
    mailbox.AliveFrame(kClient);
    api->sample(inst, &host);
    mailbox.GuestFrame();
    mailbox.AliveFrame(kClient);
    api->sample(inst, &host);

    assert(fake.ints.at("sw.state.in_battle") == 1);
    assert(fake.ints.at("sw.direct.helper") == 1);
    assert(fake.ints.at("sw.battle.native_command_waiting") == 1);
    assert(fake.ints.at("sw.battle.command.index") == 0); // nothing staged yet

    // Dead aliases (B-helper.md finding #3): "battle_bag_open"/"battle_pokemon_open" must no
    // longer be in DirectAction's/ActionCallback's whitelist -- the SAME fixture that lets the
    // real action names stage below must refuse these outright (no mailbox request at all), not
    // merely fail for some unrelated reason.
    const auto req_seq_before = mailbox.Get(sw_helper::OffsetReqSeq);
    assert(ext->on_action(inst, "battle_bag_open", 0) == EDEN_DSMOD_FALSE);
    assert(ext->on_action(inst, "battle_pokemon_open", 0) == EDEN_DSMOD_FALSE);
    assert(mailbox.Get(sw_helper::OffsetReqSeq) == req_seq_before); // no Stage() call happened

    // The real action name stages OpenBag and selects the Bag capsule (bug #1's setup).
    assert(ext->on_action(inst, "battle_open_bag", 0) == EDEN_DSMOD_TRUE);
    assert(mailbox.Get(sw_helper::OffsetReqSeq) != req_seq_before); // Stage() did run

    mailbox.AliveFrame(kClient); // keep battle_alive fresh; no GuestFrame yet -- still pending
    api->sample(inst, &host);
    assert(fake.ints.at("sw.battle.command.index") == 2);
    assert(fake.ints.at("sw.battle.command.selected2") == 1);
    assert(fake.ints.at("sw.battle.command.selected0") == 0);

    // The guest refuses the staged request later (a race: the menu stopped waiting between tap
    // and guest execution, B-helper.md finding #1). DirectUpkeep's outcome handling must clear
    // selected_command in the SAME tick it reports the refusal -- the fix this test guards.
    mailbox.GuestFrame(sw_helper::Result::NotWaiting);
    mailbox.AliveFrame(kClient);
    api->sample(inst, &host);

    assert(fake.ints.at("sw.direct.status_ready") == 1);
    assert(fake.texts.at("sw.direct.status") == sw_helper::ResultText(sw_helper::Result::NotWaiting));
    assert(fake.ints.at("sw.battle.command.index") == 0);
    assert(fake.ints.at("sw.battle.command.selected2") == 0);

    // Raid Cheer On / veiled Run (locked_cmd) never stage: refused haptic plus a toast.
    const auto req_seq_locked = mailbox.Get(sw_helper::OffsetReqSeq);
    assert(ext->on_action(inst, "locked_cmd", 0) == EDEN_DSMOD_FALSE);
    assert(mailbox.Get(sw_helper::OffsetReqSeq) == req_seq_locked);
    mailbox.AliveFrame(kClient);
    api->sample(inst, &host);
    assert(fake.ints.at("sw.direct.status_ready") == 1);
    assert(fake.texts.at("sw.direct.status") == "Choose this on the main screen");

    // Party reorder (Pokemon page drag) is exported but fails closed outside the free field:
    // in battle it is never ready, a drop is refused and nothing is written.
    const EdenDsmodModuleWriteExtensions* wx =
        eden_dsmod_get_write_extensions(EDEN_DSMOD_WRITE_EXT_VERSION, EDEN_DSMOD_WRITE_EXT_HASH);
    assert(wx && wx->configure);
    static int writes = 0;
    EdenDsmodHostWriteApi wapi{};
    wapi.version = EDEN_DSMOD_WRITE_EXT_VERSION;
    wapi.struct_size = sizeof wapi;
    wapi.abi_hash = EDEN_DSMOD_WRITE_EXT_HASH;
    wapi.write_batch = [](void*, const EdenDsmodWriteOp*, std::uint32_t) -> EdenDsmodBool { ++writes; return EDEN_DSMOD_TRUE; };
    wx->configure(inst, &wapi);
    mailbox.AliveFrame(kClient);
    api->sample(inst, &host);
    assert(fake.ints.at("sw.party.swap.ready") == 0);
    assert(ext->on_action(inst, "party_swap1", 0) == EDEN_DSMOD_FALSE);
    assert(ext->on_action(inst, "party_swap9", 0) == EDEN_DSMOD_FALSE);
    api->sample(inst, &host);
    assert(writes == 0);

    api->destroy(inst);
    std::puts("0100ABF008968000: battle_bag_open/battle_pokemon_open are dead (the real "
              "battle_open_bag/battle_open_pokemon stage instead), and a staged OpenBag the "
              "guest later refuses clears sw.battle.command.index/selectedN back to 0");
}

} // namespace

int main(int argc, char** argv) {
    // --private-data: only the checks against real bytes (a private save's blocks and the
    // player's romfs, sw_test_fixtures.h); skipped (exit 77) when those are not set up.
    if (argc > 1 && std::strcmp(argv[1], "--private-data") == 0) {
        if (!sw_test::PrivateDataAvailable()) return sw_test::kSkip;
        LoadPrivateData();
        TestPk8DecryptKnownVector();
        TestDecodeLiveStoredAgainstRealBytes();
        TestComputeMaxHpMatchesSaveMirror();
        TestPartySummaryStatsAndMatchups();
        TestMoveDataDecode();
        TestBagPouchDecode();
        TestZukanDexCounts();
        TestDexRegionCountsViaHome();
        TestEvolutionRealFiles();
        TestEvolutionSpoilerGatingHalfDex();
        std::puts("0100ABF008968000_test --private-data: all checks passed");
        return 0;
    }
    TestPk8DecryptSyntheticParty();
    TestPk8EmptySlotHasZeroSpecies();
    TestDecodeLiveStoredSynthetic();
    TestComputeMaxHpSynthetic();
    TestPartyStatFormulasAndMatchups();
    TestTypeChartSingleType();
    TestTypeChartDualType();
    TestPerBuildDataFileExists();
    TestSpeciesIconLookup();
    TestSpeciesNameDecode();
    TestMoveNameDecode();
    TestMoveDataFailsClosed();
    TestMoveEffectivenessComputation();
    TestEffectivenessLabelFailsClosed();
    TestZoneNameDecodeAndTagValidation();
    TestBuildMatchPublishedEverySample();
    TestRuntimeImagesAndSessionReset();
    TestBagPouchDecodeSynthetic();
    TestZukanAndDexHomeSynthetic();
    TestEvolutionDecodeSwinubChain();
    TestEvolutionEeveeWindow();
    TestDirectBattleSelectedCommandResetAndDeadAliases();
    std::puts("0100ABF008968000_test: all checks passed");
    return 0;
}
