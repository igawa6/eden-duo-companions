// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end unit test for sw_reader::Reader::Sample() against a *synthetic* guest heap: a fake
// EdenDsmodHostApi backed by in-memory buffers at the exact heap-relative addresses sw_addresses.h
// now marks `verified` (MyStatus8 trainer fields, CaptureSight Party), built from
//   - a synthetic encrypted party slot (sw_test_fixtures.h: species #150 Lv.100, MaxHP 356) and
//     synthetic live "stored" blobs and den slots. With --private-data the same suite runs again
//     on the real bytes those stand in for (SW_FIXTURES_DIR, never in this repository): the
//     gb-635571 fixture save's party slot 1 that LIVE-READERS-132.md's 2026-10-24 session read
//     live from heap+0x450C68B0 and cross-checked against the native 06-party.png screenshot,
//     its five live-format party blobs and its raid den slots;
//   - synthetic MyStatus8 bytes matching that same session's recorded live values (OT "Player",
//     TID/SID 0/0, Watt 9999999, game byte 44/SW).
// This does not touch an emulator or the harness -- it proves sw_reader.h's own plumbing
// (anchor lookup -> heap_begin()+offset -> torn-read double-read -> sw_pk8 decode -> per-slot
// validation -> publish) against bytes known to be correct, including the heap_begin() value
// itself (boot 1's real HeapLow(), 0x2105400000, from LIVE-READERS-132.md's Task 1 table).

#include "core/mods/dsmod_module_abi.h"
#include "sw_addresses.h"
#include "sw_battle.h"
#include "sw_battle_gauge.h"
#include "sw_effectiveness_text.h"
#include "sw_move_data.h"
#include "sw_move_names.h"
#include "sw_reader.h"
#include "sw_encounter_fixture.h"
#include "sw_species_types.h"
#include "sw_test_fixtures.h"
#include "sw_zone.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

// The party and den bytes every test below builds its heap from. main() fills them with the
// synthetic stand-ins (sw_test_fixtures.h) or, with --private-data, with the real fixture save's
// bytes (SW_FIXTURES_DIR) -- the suite's expectations hold for both.
//   g_party_slot1: the save mirror's 0x158 slot 0 (Mewtwo Lv.100, MaxHP 356);
//   g_mewtwo..g_zarude: live 0x148 "stored" blobs (MaxHP 356/424/328/328/351 at level 100,
//     current HP 240/424/328/328/351);
//   g_den_galar12 (active, Rare, rank 4), g_den_galar20 (active, Common, rank 2), g_den_galar0
//     (inactive): 0x18-byte RaidSpawnDetail slots.
unsigned char g_party_slot1[sw_pk8::kSizeParty];
unsigned char g_mewtwo[sw_pk8::kSizeStored], g_eternatus[sw_pk8::kSizeStored], g_zacian[sw_pk8::kSizeStored],
    g_zamazenta[sw_pk8::kSizeStored], g_zarude[sw_pk8::kSizeStored];
unsigned char g_den_galar12[0x18], g_den_galar20[0x18], g_den_galar0[0x18];

// RaidSpawnDetail (0x18): u64 hash, u64 seed, rank (stars - 1) +0x10, flag roll +0x11, DenType +0x12.
void SynDenSlot(unsigned char* out, std::uint64_t hash, std::uint64_t seed, std::uint8_t rank,
                std::uint8_t roll, std::uint8_t den_type) {
    std::memset(out, 0, 0x18);
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<unsigned char>(hash >> (8 * i));
        out[8 + i] = static_cast<unsigned char>(seed >> (8 * i));
    }
    out[0x10] = rank;
    out[0x11] = roll;
    out[0x12] = den_type;
}

void FillSyntheticFixtures() {
    sw_test::SynMon slot1 = sw_test::SynPartyMember(0);
    slot1.level = 100;
    slot1.stats = {356, 201, 186, 247, 300, 186};
    const auto party = sw_test::MakeParty(slot1);
    std::memcpy(g_party_slot1, party.data(), party.size());
    unsigned char* stored[5]{g_mewtwo, g_eternatus, g_zacian, g_zamazenta, g_zarude};
    for (int i = 0; i < 5; ++i) {
        const auto blob = sw_test::MakeStored(sw_test::SynPartyMember(i));
        std::memcpy(stored[i], blob.data(), blob.size());
    }
    SynDenSlot(g_den_galar12, 0x0123456789ABCDEFull, 0x1111111111111111ull, 4, 0x10, 2);
    SynDenSlot(g_den_galar20, 0x0FEDCBA987654321ull, 0x2222222222222222ull, 2, 0x20, 1);
    SynDenSlot(g_den_galar0, 0x1357913579135791ull, 0x3333333333333333ull, 2, 0x30, 0);
}

void LoadPrivateFixtures() {
    auto load = [](unsigned char* out, const char* name, std::size_t size) {
        const auto bytes = sw_test::Fixture(name, size);
        std::memcpy(out, bytes.data(), size);
    };
    load(g_party_slot1, "party_slot1_mirror.bin", sw_pk8::kSizeParty);
    load(g_mewtwo, "live_stored_mewtwo.bin", sw_pk8::kSizeStored);
    load(g_eternatus, "live_stored_eternatus.bin", sw_pk8::kSizeStored);
    load(g_zacian, "live_stored_zacian.bin", sw_pk8::kSizeStored);
    load(g_zamazenta, "live_stored_zamazenta.bin", sw_pk8::kSizeStored);
    load(g_zarude, "live_stored_zarude.bin", sw_pk8::kSizeStored);
    load(g_den_galar12, "den_galar_12.bin", 0x18);
    load(g_den_galar20, "den_galar_20.bin", 0x18);
    load(g_den_galar0, "den_galar_0.bin", 0x18);
}

void PutU16(std::vector<unsigned char>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<unsigned char>(v);
    b[off + 1] = static_cast<unsigned char>(v >> 8);
}
void PutU32(std::vector<unsigned char>& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = static_cast<unsigned char>(v >> (8 * i));
}
void PutU64(std::vector<unsigned char>& b, std::size_t off, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[off + i] = static_cast<unsigned char>(v >> (8 * i));
}

// A handful of flat byte buffers, each anchored at its own absolute (heap-adjusted) address --
// enough to stand in for the real guest heap's scattered layout without allocating the ~1 GiB+
// span the real sysbot offsets would otherwise imply.
struct FakeRegion {
    std::uint64_t base;
    std::vector<unsigned char> data;
};

struct FakeHost {
    std::uint64_t heap_begin = 0;
    std::uint64_t main_base = 0;
    std::vector<FakeRegion> regions;
    std::map<std::string, std::int64_t> ints;
    std::map<std::string, std::string> texts;
    std::map<std::string, double> floats;
    // Stands in for one species' row of romfs:/bin/pml/personal/personal_total.bin without
    // shipping the real 205 KiB file in a unit test: Type1/Type2 (sw_species_types.h, +0x06/
    // +0x07), base HP (sw_personal_stats.h, +0x00) and raw EXPGrowth byte (+0x15). A species not
    // listed in FakeHost::personal simulates "no live-heap-adjacent romfs lookup available"
    // (FakeReadRomfs returns 0), exercising both headers' fail-closed paths the same way a
    // missing file or an old host would. Aggregate-initialisable with just {t1,t2} (existing
    // type-only tests) -- base_hp/growth default to 0, which a type-only test never reads.
    struct PersonalRec {
        unsigned char t1 = 0, t2 = 0, base_hp = 0, growth = 0;
    };
    std::map<int, PersonalRec> personal;
    // Generic plain-file romfs stand-ins (path -> whole-file bytes), e.g. wazaname.dat for the
    // battle-reader test below -- serves both the "query total size" call (offset=0, out=null,
    // size=0, same convention sw_species_names::LoadNames/sw_move_names::LoadNames use) and the
    // real read that follows.
    std::map<std::string, std::vector<unsigned char>> dat_files;
    // Move id -> whole wazabin file bytes (sw_move_data.h's one-file-per-move convention) --
    // separate from dat_files because the path is move-id-parameterised, not a fixed name.
    std::map<std::uint16_t, std::vector<unsigned char>> waza_files;
    // Fault injection: a read this returns true for fails (a torn / unmapped read on the device).
    std::function<bool(std::uint64_t, std::size_t)> fail_read;

    const FakeRegion* Find(std::uint64_t addr, std::uint64_t size) const {
        for (const auto& r : regions) {
            if (addr >= r.base && size <= r.data.size() && addr - r.base <= r.data.size() - size)
                return &r;
        }
        return nullptr;
    }
};

EdenDsmodBool FakeIsMapped(void* ud, std::uint64_t addr, std::uint64_t size) {
    return static_cast<FakeHost*>(ud)->Find(addr, size) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}
EdenDsmodBool FakeReadMemory(void* ud, std::uint64_t addr, void* out, std::size_t size) {
    auto* h = static_cast<FakeHost*>(ud);
    const FakeRegion* r = h->Find(addr, size);
    if (!r || (h->fail_read && h->fail_read(addr, size))) return EDEN_DSMOD_FALSE;
    std::memcpy(out, r->data.data() + (addr - r->base), size);
    return EDEN_DSMOD_TRUE;
}
std::uint64_t FakeHeapBegin(void* ud) {
    return static_cast<FakeHost*>(ud)->heap_begin;
}
void FakePublishI64(void* ud, const char* name, std::int64_t v) {
    static_cast<FakeHost*>(ud)->ints[name] = v;
}
void FakePublishText(void* ud, const char* name, const char* v) {
    static_cast<FakeHost*>(ud)->texts[name] = v ? v : "";
}
void FakePublishF64(void* ud, const char* name, double value) {
    static_cast<FakeHost*>(ud)->floats[name] = value;
}
// Stands in for the host's romfs range-read over the real, uncompressed personal-table file --
// see sw_species_types.h's header. Only ever asked for the Type1/Type2 pair at
// `species * kRecordSize + kTypeOffset`, so this just inverts that formula and looks the species
// up in FakeHost::personal, matching how the real file is one flat species-indexed array.
std::size_t FakeReadRomfs(void* ud, const char* path, std::uint64_t offset, void* out,
                          std::size_t size) {
    auto* h = static_cast<FakeHost*>(ud);
    if (!path) return 0;
    if (std::strcmp(path, sw_species_types::kPersonalTablePath) == 0) {
        // Full table fixtures also exercise reverse regional dex lookup, rather than only
        // the small type/base-stat range reads used by the party tests.
        const auto table = h->dat_files.find(path);
        if (table != h->dat_files.end()) {
            const auto& bytes = table->second;
            if (size == 0 && out == nullptr) return bytes.size();
            if (offset > bytes.size() || size > bytes.size() - offset) return 0;
            if (out) std::memcpy(out, bytes.data() + offset, size);
            return size;
        }
        // Generic record-relative serving: builds one species' full 176-byte row on demand (the
        // 4 fields FakeHost::PersonalRec knows about, zero elsewhere) and slices whatever
        // [offset, offset+size) window the caller asked for -- serves both
        // sw_species_types::GetTypes() (2 bytes at +0x06) and sw_personal_stats::
        // GetBaseHpAndGrowth() (1 byte each at +0x00/+0x15) from the one map.
        if (!out || size == 0) return 0;
        const int species = static_cast<int>(offset / sw_species_types::kRecordSize);
        const std::size_t field_off = offset % sw_species_types::kRecordSize;
        if (field_off + size > sw_species_types::kRecordSize) return 0;
        const auto it = h->personal.find(species);
        if (it == h->personal.end()) return 0;
        unsigned char rec[sw_species_types::kRecordSize]{};
        rec[sw_species_types::kTypeOffset] = it->second.t1;
        rec[sw_species_types::kTypeOffset + 1] = it->second.t2;
        rec[0x00] = it->second.base_hp;
        rec[0x15] = it->second.growth;
        std::memcpy(out, rec + field_off, size);
        return size;
    }
    // Move-id-parameterised wazabin path (sw_move_data.h's kWazaPathFmt) -- tries every move id
    // this fixture actually stocked rather than parsing the id back out of the path string.
    for (const auto& [move_id, bytes] : h->waza_files) {
        char candidate[96];
        std::snprintf(candidate, sizeof candidate, sw_move_data::kWazaPathFmt,
                      static_cast<unsigned>(move_id));
        if (std::strcmp(path, candidate) != 0) continue;
        if (size == 0 && out == nullptr) return bytes.size();
        if (offset > bytes.size() || size > bytes.size() - offset) return 0;
        if (out) std::memcpy(out, bytes.data() + offset, size);
        return size;
    }
    const auto it = h->dat_files.find(path);
    if (it == h->dat_files.end()) return 0;
    const auto& bytes = it->second;
    if (size == 0 && out == nullptr) return bytes.size(); // size-query call
    if (offset > bytes.size() || size > bytes.size() - offset) return 0;
    if (out) std::memcpy(out, bytes.data() + offset, size);
    return size;
}

/// Builds a FakeHost whose trainer + party regions mirror LIVE-READERS-132.md's 2026-10-24
/// recorded live values, at the real anchors' addresses under boot 1's real HeapLow() value. If
/// `corrupt_checksum` is set, slot 0's stored checksum (+0x06, plaintext) is flipped -- species/
/// level/MaxHP still decode "fine" from the surrounding bytes, but the checksum must no longer
/// match, exercising the fail-closed path rather than another happy-path read.
FakeHost BuildFixture(bool corrupt_checksum, bool bad_money = false) {
    const auto* a_watt = sw_addresses::Find("mystatus.watt");
    const auto* a_ids = sw_addresses::Find("mystatus.tid_sid");
    const auto* a_ot = sw_addresses::Find("mystatus.ot_name");
    const auto* a_game = sw_addresses::Find("mystatus.game");
    const auto* a_party = sw_addresses::Find("party.save_mirror");
    const auto* a_misc = sw_addresses::Find("misc8.money_badges");
    assert(a_watt && a_ids && a_ot && a_game && a_party && a_misc);
    assert(a_watt->verified && a_ids->verified && a_ot->verified && a_game->verified &&
           a_party->verified && a_misc->verified);
    // The mirror's count byte sits exactly at the slot-6 boundary, inside the anchor's own span.
    assert(a_party->size == 0x158 * 6 + 1);

    const std::uint64_t trainer_base =
        std::min({a_watt->sysbot_offset, a_ids->sysbot_offset, a_ot->sysbot_offset,
                  a_game->sysbot_offset});
    const std::uint64_t trainer_end =
        std::max({a_watt->sysbot_offset + a_watt->size, a_ids->sysbot_offset + a_ids->size,
                  a_ot->sysbot_offset + a_ot->size, a_game->sysbot_offset + a_game->size});
    std::vector<unsigned char> trainer_buf(trainer_end - trainer_base, 0);
    PutU32(trainer_buf, a_watt->sysbot_offset - trainer_base, 9999999u);
    PutU16(trainer_buf, a_ids->sysbot_offset - trainer_base + 0, 0);      // TID16
    PutU16(trainer_buf, a_ids->sysbot_offset - trainer_base + 2, 0);      // SID16
    trainer_buf[a_game->sysbot_offset - trainer_base] = 44;               // PKHeX GameVersion.SW
    {
        const char* name = "Player";
        const std::size_t ot_off = a_ot->sysbot_offset - trainer_base;
        for (std::size_t i = 0; name[i] != '\0'; ++i)
            PutU16(trainer_buf, ot_off + i * 2, static_cast<std::uint16_t>(name[i]));
        // remaining bytes (NUL terminator + padding) stay zero
    }

    std::vector<unsigned char> party_buf(a_party->size, 0);
    std::memcpy(party_buf.data(), g_party_slot1, sizeof g_party_slot1); // slot 0
    // slots 1-5 stay all-zero: decrypts to species 0 (empty), matching sw_pk8's own empty-slot
    // test case -- these must publish ready=0, never a fabricated mon.
    party_buf[0x158 * 6] = 1; // the count byte: 1 real member in this synthetic fixture
    if (corrupt_checksum) party_buf[6] ^= 0xFF; // checksum is plaintext at +0x06, never encrypted

    // Misc8: Badges (all 8), Money and BP at their verified offsets (sw_addresses.h / the
    // 2026-10-25 LIVE-READERS-132.md session) -- the task's own documented expected values.
    std::vector<unsigned char> misc_buf(a_misc->size, 0);
    misc_buf[0x00] = 0xFF;
    PutU32(misc_buf, 0x04, bad_money ? 10000000u : 9999999u);
    PutU16(misc_buf, 0x11C, 9999);

    FakeHost fake;
    fake.heap_begin = 0x2105400000ULL; // boot 1's real HeapLow(), LIVE-READERS-132.md Task 1
    fake.regions.push_back({fake.heap_begin + trainer_base, std::move(trainer_buf)});
    fake.regions.push_back({fake.heap_begin + a_party->sysbot_offset, std::move(party_buf)});
    fake.regions.push_back({fake.heap_begin + a_misc->sysbot_offset, std::move(misc_buf)});
    fake.personal[150] = {13, 13}; // Mewtwo: mono Psychic (personal-table type id 13)
    return fake;
}

EdenDsmodHostApi MakeHostApi(FakeHost& fake) {
    EdenDsmodHostApi host{};
    host.userdata = &fake;
    host.main_base = fake.main_base;
    host.get_heap_begin = &FakeHeapBegin;
    host.is_mapped = &FakeIsMapped;
    host.read_memory = &FakeReadMemory;
    host.publish_i64 = &FakePublishI64;
    host.publish_text = &FakePublishText;
    host.publish_f64 = &FakePublishF64;
    host.read_romfs = &FakeReadRomfs;
    return host;
}

void TestFullPipelineAgainstLiveVerifiedValues() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);

    assert(fake.ints.at("sw.trainer.ready") == 1);
    assert(fake.texts.at("sw.trainer.ot_name") == "Player");
    assert(fake.ints.at("sw.trainer.tid") == 0);
    assert(fake.ints.at("sw.trainer.sid") == 0);
    assert(fake.ints.at("sw.trainer.watt") == 9999999);
    assert(fake.ints.find("sw.trainer.game") == fake.ints.end()); // dead key, no page binds it (review D)

    assert(fake.ints.at("sw.party.ready") == 1);
    assert(fake.ints.at("sw.party.count") == 1);
    assert(fake.ints.at("sw.party.0.ready") == 1);
    assert(fake.ints.at("sw.party.0.species") == 150);  // Mewtwo
    assert(fake.ints.at("sw.party.0.level") == 100);
    assert(fake.ints.at("sw.party.0.max_hp") == 356);   // matches 06-party.png exactly
    // Keys no page binds are no longer published (review D5): moves/PP/summary of the mirror.
    assert(fake.ints.find("sw.party.0.move1") == fake.ints.end());
    assert(fake.texts.find("sw.party.0.summary") == fake.texts.end());
    // The fixture Mewtwo is a Chinese-language mon: its stored (Chinese) name has no
    // glyphs in the package font, so the species name is used -- and this fixture's
    // FakeReadRomfs has no monsname.dat, so no name publishes at all.
    assert(fake.texts.find("sw.party.0.name") == fake.texts.end());
    assert(fake.texts.at("sw.party.0.level_text") == sw_str::Level(100));
    // The icon lookup (sw_species_icons.h) is a static verified table, not a romfs read, so it is
    // unaffected by this fixture's bare-bones FakeReadRomfs -- species 150 (Mewtwo) is one of the
    // table's verified entries and must publish a real icon source.
    assert(fake.ints.at("sw.party.0.icon_ready") == 1);
    assert(fake.texts.at("sw.party.0.icon_src").find("poke_icon_0150_00n_n") != std::string::npos);

    // Misc8 (money/badges/BP) -- the task's own documented expected values, read through the
    // same torn-read + sanity-bound path as trainer/party.
    assert(fake.ints.at("sw.misc8.ready") == 1);
    assert(fake.ints.at("sw.misc8.badges") == 0xFF);
    assert(fake.ints.at("sw.misc8.badge_count") == 8);
    assert(fake.ints.at("sw.misc8.money") == 9999999);
    assert(fake.ints.at("sw.misc8.bp") == 9999);
    // Plain "P", not the Unicode Peso sign the pre-font-fix code used -- the real game font has no
    // Pokédollar glyph (confirmed by the mockup team's own font research, sw_reader.h's comment).
    assert(fake.texts.at("sw.misc8.money_text") == "P9,999,999");
    assert(fake.texts.at("sw.misc8.bp_text") == "9999 BP");
    assert(fake.texts.at("sw.misc8.badge_text") == "8/8");

    // Team weakness board: species -> types read live via the fake romfs, matchups computed
    // against sw_type_chart.h. Mewtwo is mono Psychic (personal type id 13): Bug, Ghost and Dark
    // are the super-effective attacking types in the standard chart (each 200%), and nothing is
    // a 0x immunity against a pure Psychic target.
    assert(fake.ints.at("sw.party.0.types_ready") == 1);
    assert(fake.ints.at("sw.party.0.type1") == 13);
    assert(fake.ints.at("sw.party.0.type2") == 13);
    assert(fake.ints.at("sw.party.0.mono_type") == 1);
    assert(fake.texts.at("sw.party.0.weak_text") == std::string(sw_str::TypeName(6)) + ", " + std::string(sw_str::TypeName(7)) + ", " + std::string(sw_str::TypeName(16)));
    assert(fake.texts.at("sw.party.0.immune_text") == "—");
    assert(fake.texts.at("sw.party.0.type1_src").find("typeIcon_13_esper_00^t") != std::string::npos);
    // mono-typed: no type2_src is published at all (nothing for a page to bind a second icon to).
    assert(fake.texts.find("sw.party.0.type2_src") == fake.texts.end());
    // Weakness-board DISCS (D2's own "type-icon discs" requirement): sw_type_chart::Type and
    // sw_species_types's personal-table type id use different orders -- this checks
    // PublishWeaknessBoard's ChartTypeToPersonalId conversion actually lands on the right texture,
    // not just the right English word. Mewtwo (mono Psychic) is weak to Bug/Ghost/Dark, in that
    // chart-enum-order sequence (sw_type_chart::Type: ... Bug=11, Ghost=13, ... Dark=15).
    assert(fake.ints.at("sw.party.0.weak_count") == 3);
    assert(fake.texts.at("sw.party.0.weak0_src").find("typeIcon_06_mushi_00^t") != std::string::npos);
    assert(fake.texts.at("sw.party.0.weak1_src").find("typeIcon_07_ghost_00^t") != std::string::npos);
    assert(fake.texts.at("sw.party.0.weak2_src").find("typeIcon_16_aku_00^t") != std::string::npos);
    // Per-disc personal-table type id, for the manifest's tint_bind/tint_colors (the disc texture
    // itself is a colourless BC5 mask -- see sw_reader.h's comment on this field).
    assert(fake.ints.at("sw.party.0.weak0_type") == 6);  // Bug
    assert(fake.ints.at("sw.party.0.weak1_type") == 7);  // Ghost
    assert(fake.ints.at("sw.party.0.weak2_type") == 16); // Dark
    assert(fake.ints.at("sw.party.0.immune_count") == 0);

    // Slots 1-5 are all-zero in this fixture (only one real member provided): must fail closed,
    // never show a fabricated/empty mon as if it were real.
    for (int i = 1; i < 6; ++i) {
        char key[40];
        std::snprintf(key, sizeof key, "sw.party.%d.ready", i);
        assert(fake.ints.at(key) == 0);
    }
    // No current-HP value is ever published for any slot -- CaptureSight Party has no such
    // field (LIVE-READERS-132.md: exhaustively searched, not found).
    for (const auto& [k, v] : fake.ints) {
        (void)v;
        assert(k.find("current_hp") == std::string::npos);
    }

    std::puts("sw_reader: full pipeline (synthetic heap, real Mewtwo party bytes) matches "
              "LIVE-READERS-132.md's recorded live values");
}

// Builds a synthetic heap for sw_addresses.h's "party.array_root" chain: the container struct
// itself (6 slot pointers +0x58..+0x80, count +0x88) plus `blobs.size()` separate slot-object
// regions (each `kBlobOffset`-aligned so slot_ptr+0xD0 lands inside it), one real encrypted
// "stored" blob per slot -- the exact live layout LIVE-READERS-132.md's "Live party objects
// FOUND" session documents. `personal` must already carry a PersonalRec (base_hp/growth) for
// every species a blob decodes to, or sw_personal_stats::GetBaseHpAndGrowth fails closed and
// SampleLiveParty skips that slot entirely (same "can't compute level/max_hp" path real
// hardware would hit for an unreadable romfs).
FakeHost BuildLivePartyFixture(const std::vector<const unsigned char*>& blobs, std::uint8_t index_byte = 0) {
    const auto* a_root = sw_addresses::Find("party.array_root");
    assert(a_root && a_root->verified);

    FakeHost fake;
    fake.heap_begin = 0x2105400000ULL; // same real HeapLow() value BuildFixture uses

    constexpr std::uint64_t kSlotPtrBase = 0x58, kSlotPtrStride = 8, kCountOffset = 0x88,
                             kBlobOffset = 0xD0;
    constexpr std::size_t kSlotRegionSize = kBlobOffset + sw_pk8::kSizeStored;

    std::vector<unsigned char> root_buf(0x90, 0);
    for (std::size_t i = 0; i < blobs.size() && i < 6; ++i) {
        const std::uint64_t slot_addr = fake.heap_begin + 0x9000'0000ull + i * 0x1000ull;
        PutU64(root_buf, kSlotPtrBase + i * kSlotPtrStride, slot_addr);

        std::vector<unsigned char> slot_buf(kSlotRegionSize, 0);
        std::memcpy(slot_buf.data() + kBlobOffset, blobs[i], sw_pk8::kSizeStored);
        fake.regions.push_back({slot_addr, std::move(slot_buf)});
    }
    PutU64(root_buf, kCountOffset, blobs.size());
    root_buf[kCountOffset + 1] = index_byte; // +0x89: the party's own member index (native exchange)
    fake.regions.push_back({fake.heap_begin + a_root->sysbot_offset, std::move(root_buf)});
    return fake;
}

// PokeCrypto.Encrypt8 for a test blob: the inverse of sw_pk8::DecryptInPlace (shuffle the four
// sub-blocks back, then the same XOR keystream). The permutation is read off UnshuffleBlocks itself.
void EncryptStoredForTest(unsigned char* member) {
    const std::uint32_t pv = sw_pk8::Rd32(member);
    const std::uint32_t sv = (pv >> 13) & 31;
    constexpr std::size_t kBlock = sw_pk8::kBlockSize, kBlocks = sw_pk8::kBlockCount;
    unsigned char tags[kBlocks * kBlock];
    for (std::size_t b = 0; b < kBlocks; ++b) std::memset(tags + b * kBlock, static_cast<int>(b), kBlock);
    sw_pk8::UnshuffleBlocks(tags, sv);  // decrypted block i came from shuffled block tags[i * kBlock]
    unsigned char shuffled[kBlocks * kBlock];
    for (std::size_t i = 0; i < kBlocks; ++i)
        std::memcpy(shuffled + tags[i * kBlock] * kBlock, member + 8 + i * kBlock, kBlock);
    sw_pk8::CryptArrayU16(shuffled, sizeof shuffled, pv);
    std::memcpy(member + 8, shuffled, sizeof shuffled);
}

// A copy of a real stored blob with its PID made shiny for its own TID/SID (and optionally the
// Egg flag set), checksum fixed, re-encrypted.
std::vector<unsigned char> ShinyVariant(const unsigned char* encrypted, bool egg) {
    std::vector<unsigned char> buf(encrypted, encrypted + sw_pk8::kSizeStored);
    sw_pk8::DecryptInPlace(buf.data(), sw_pk8::kSizeStored);
    const std::uint16_t tid = sw_pk8::Rd16(buf.data() + 0x0C), sid = sw_pk8::Rd16(buf.data() + 0x0E);
    const std::uint32_t lo = sw_pk8::Rd32(buf.data() + 0x1C) & 0xFFFFu;
    const std::uint32_t pid = (static_cast<std::uint32_t>(tid ^ sid ^ lo) << 16) | lo;  // xor 0: "square"
    for (int b = 0; b < 4; ++b) buf[0x1C + b] = static_cast<unsigned char>(pid >> (8 * b));
    if (egg) buf[0x8F] |= 0x40;   // IV32 bit 30
    sw_pk8::Wr16(buf.data() + 0x06, sw_pk8::ComputeChecksum16(buf.data(), sw_pk8::kSizeStored));
    EncryptStoredForTest(buf.data());
    return buf;
}

// Shiny mark (owner 2026-11-01): a member whose PID halves XOR its OT TID/SID below 16 publishes
// sw.party.N.shiny = 1; the real (non-shiny) fixture members 0; an Egg never does (item E).
void TestPartyShinyFromPk8() {
    // the shiny rule itself: < 16 shiny (0 = square, 1..15 = star), 16 and up not
    assert(sw_pk8::ShinyXor(0xABCD1234u, 0x0000, 0xABCD ^ 0x1234) == 0 && sw_pk8::IsShiny(0xABCD1234u, 0, 0xABCD ^ 0x1234));
    assert(sw_pk8::IsShiny(0xABCD1234u, 0, (0xABCD ^ 0x1234) ^ 15));
    assert(!sw_pk8::IsShiny(0xABCD1234u, 0, (0xABCD ^ 0x1234) ^ 16));
    // the real fixture members decode their PID/TID/SID and are not shiny
    for (const unsigned char* blob : {g_mewtwo, g_eternatus, g_zacian}) {
        const auto m = sw_pk8::DecodeLiveStored(blob);
        assert(m.checksum_valid && m.pid != 0 && !sw_pk8::IsShiny(m.pid, m.tid, m.sid));
    }
    const auto shiny = ShinyVariant(g_mewtwo, false);
    const auto shiny_egg = ShinyVariant(g_zacian, true);
    const auto dm = sw_pk8::DecodeLiveStored(shiny.data());
    assert(dm.checksum_valid && dm.species == 150 && sw_pk8::IsShiny(dm.pid, dm.tid, dm.sid));
    const auto de = sw_pk8::DecodeLiveStored(shiny_egg.data());
    assert(de.checksum_valid && de.egg && sw_pk8::IsShiny(de.pid, de.tid, de.sid));

    FakeHost fake = BuildLivePartyFixture({shiny.data(), g_eternatus, shiny_egg.data()});
    fake.personal[150] = {13, 13, 106, 5};
    fake.personal[890] = {9, 15, 140, 5};
    fake.personal[888] = {17, 17, 92, 5};
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);
    assert(fake.ints.at("sw.party.0.ready") == 1 && fake.ints.at("sw.party.0.species") == 150);
    assert(fake.ints.at("sw.party.0.shiny") == 1);
    assert(fake.ints.at("sw.party.1.shiny") == 0);
    assert(fake.ints.at("sw.party.2.egg") == 1 && fake.ints.at("sw.party.2.shiny") == 0);
    std::puts("sw_reader: a constructed shiny PK8 publishes sw.party.N.shiny, the real members and a "
              "shiny Egg do not");
}

void TestLiveStoredPartyChainReadsThreeRealMembers() {
    FakeHost fake = BuildLivePartyFixture({g_mewtwo, g_eternatus, g_zacian});
    // personal_total.bin's real base HP + growth byte for these 3 species (read directly this
    // cycle -- sw_level.h/sw_personal_stats.h's own file header comments).
    fake.personal[150] = {13, 13, 106, 5}; // Mewtwo: mono Psychic, base HP 106, Slow
    fake.personal[890] = {9, 15, 140, 5};  // Eternatus: Poison/Dragon, base HP 140, Slow
    fake.personal[888] = {17, 17, 92, 5};  // Zacian (Hero of Many Battles): mono Fairy, base HP 92, Slow
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);

    assert(fake.ints.at("sw.party.count") == 3);
    assert(fake.ints.at("sw.party.six") == 0);

    assert(fake.ints.at("sw.party.0.ready") == 1);
    assert(fake.ints.at("sw.party.0.species") == 150);
    assert(fake.ints.at("sw.party.0.level") == 100);
    assert(fake.ints.at("sw.party.0.max_hp") == 356);
    assert(fake.ints.at("sw.party.0.current_hp") == 240); // matches the native 240/356 exactly
    assert(fake.ints.at("sw.party.0.hp_pct") == 67);       // round(240*100/356) truncated = 67
    assert(fake.ints.at("sw.party.0.hpg") == 1);           // 67% > 50 -> green
    assert(fake.ints.at("sw.party.0.hpy") == 0);
    assert(fake.ints.at("sw.party.0.hpr") == 0);
    assert(fake.ints.at("sw.party.0.status") == 0);
    assert(fake.texts.at("sw.party.0.hp_pct_text") == "67%");

    assert(fake.ints.at("sw.party.1.species") == 890);
    assert(fake.ints.at("sw.party.1.current_hp") == 424);
    assert(fake.ints.at("sw.party.1.max_hp") == 424);
    assert(fake.ints.at("sw.party.1.hp_pct") == 100);
    assert(fake.ints.at("sw.party.1.hpg") == 1);

    assert(fake.ints.at("sw.party.2.species") == 888);
    assert(fake.ints.at("sw.party.2.current_hp") == 328);
    assert(fake.ints.at("sw.party.2.max_hp") == 328);

    // Slots 3-5 were not placed in this fixture (only 3 blobs, count==3): they publish "not
    // ready" (their tiles lock), and nothing claims a 4th/5th live member exists.
    assert(fake.ints.find("sw.party.3.current_hp") == fake.ints.end());
    assert(fake.ints.at("sw.party.3.ready") == 0 && fake.ints.at("sw.party.5.ready") == 0);
    // The medicine target identity is the live slot's encryption constant (PK8 +0x00).
    std::uint32_t ec0 = 0;
    std::memcpy(&ec0, g_mewtwo, 4);
    assert(reader.live_party_ec(0) == ec0 && reader.live_party_slot_ok(0) && !reader.live_party_slot_ok(3));

    std::puts("sw_reader: SampleLiveParty's full array_root pointer chain (3 synthetic slot "
              "objects, real encrypted blobs) reads species/current HP and computes level/"
              "MaxHP/hp_pct/zone correctly, matching the real fixture's known values");
}

// Review M1: a 6-member party is read live through slot 5 (+0x80), the count gates the rest.
void TestPartyDetailSelectionFreshnessAndPrivacy() {
    FakeHost fake = BuildLivePartyFixture({g_mewtwo});
    auto& personal = fake.dat_files[sw_personal_stats::kPersonalTablePath];
    personal.resize(151 * sw_personal_stats::kRecordSize);
    const std::size_t offset = 150 * sw_personal_stats::kRecordSize;
    const unsigned char bases[]{106, 110, 90, 130, 154, 90};
    std::memcpy(personal.data() + offset, bases, 6);
    personal[offset + 6] = 13; personal[offset + 7] = 13;
    personal[offset + 0x15] = 5; personal[offset + 0x20] = 1;
    const auto host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);
    assert(fake.ints.at("sw.party.detail.open") == 0);
    assert(!reader.DispatchAction("party_detail", 1)); // no live member
    assert(reader.DispatchAction("party_detail", 0));
    fake.ints.clear(); fake.texts.clear();
    reader.Sample(host);
    assert(fake.ints.at("sw.party.detail.open") == 1);
    assert(fake.ints.at("sw.party.detail.stats_ready") == 1);
    assert(fake.ints.at("sw.party.detail.stat.hp") == 356);
    assert(fake.texts.at("sw.party.detail.weak_text").find(std::string(sw_str::TypeName(7)) + " ×2") != std::string::npos);
    // Psychic's actual chart entries use native personal-table IDs, not chart-enum IDs.
    assert(fake.ints.at("sw.party.detail.weak_count") == 3);
    assert(fake.ints.at("sw.party.detail.resist_count") == 2);
    assert(fake.ints.at("sw.party.detail.immune_count") == 0);
    assert(fake.ints.at("sw.party.detail.weak0.type_id") == 6); // Bug
    assert(fake.ints.at("sw.party.detail.weak1.type_id") == 7); // Ghost
    assert(fake.ints.at("sw.party.detail.weak2.type_id") == 16); // Dark
    assert(fake.ints.at("sw.party.detail.resist0.type_id") == 1); // Fighting
    assert(fake.ints.at("sw.party.detail.resist1.type_id") == 13); // Psychic
    assert(fake.texts.at("sw.party.detail.weak0.text") == std::string(sw_str::TypeName(6)) + " ×2");
    char expected_type_src[160]{};
    assert(sw_species_types::TypeImageSource(6, false, expected_type_src, sizeof expected_type_src));
    assert(fake.texts.at("sw.party.detail.weak0.src") == expected_type_src);
    for (int j = 0; j < sw_species_types::kPersonalTypeCount; ++j) {
        assert(fake.ints.at("sw.party.detail.weak" + std::to_string(j) + ".ready") == (j < 3 ? 1 : 0));
        assert(fake.ints.at("sw.party.detail.resist" + std::to_string(j) + ".ready") == (j < 2 ? 1 : 0));
        assert(fake.ints.at("sw.party.detail.immune" + std::to_string(j) + ".ready") == 0);
    }

    assert(fake.texts.at("sw.party.detail.matchup_note").find("not included") != std::string::npos);
    assert(!reader.DispatchAction("party_detail_move", 0)); // Stats tab cannot open a hidden move
    assert(reader.DispatchAction("party_detail_tab", 1));
    assert(reader.DispatchAction("party_detail_move", 0));
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.party.detail.move_open") == 1);
    assert(fake.ints.at("sw.party.detail.move_index") == 0);
    assert(fake.ints.at("sw.party.detail.move_selected0") == 1);
    assert(fake.ints.at("sw.party.detail.move_selected1") == 0);
    assert(!reader.DispatchAction("party_detail_move", 4));
    assert(reader.DispatchAction("party_detail_move", -1));
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.party.detail.move_open") == 0);
    assert(reader.DispatchAction("party_detail_move", 0));
    assert(reader.DispatchAction("party_detail_tab", 2));
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.party.detail.tab2") == 1);
    assert(fake.ints.at("sw.party.detail.tab0") == 0);
    assert(fake.ints.at("sw.party.detail.move_open") == 0);
    assert(reader.DispatchAction("party_detail_tab", 1));
    assert(reader.DispatchAction("party_detail_move", 0));
    assert(reader.DispatchAction("party_detail", 0)); // inspecting a member anew resets the popup
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.party.detail.move_open") == 0);
    assert(!reader.DispatchAction("party_detail_tab", 3));
    assert(reader.DispatchAction("party_detail_close", 0));
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.party.detail.open") == 0);
    assert(fake.texts.find("sw.party.detail.weak_text") == fake.texts.end());
    assert(reader.DispatchAction("party_detail", 0));

    // Native hyper-training applies to displayed maxHP, not the stored IV itself.
    std::vector<unsigned char> blob(std::begin(g_mewtwo), std::end(g_mewtwo));
    sw_pk8::DecryptInPlace(blob.data(), blob.size());
    PutU32(blob, 0, 0);
    PutU32(blob, 0x8C, sw_pk8::Rd32(blob.data() + 0x8C) & ~31u);
    blob[0x126] |= 1u;
    PutU16(blob, 6, sw_pk8::ComputeChecksum16(blob.data()));
    sw_pk8::CryptArrayU16(blob.data() + 8, blob.size() - 8, 0);
    auto* region = const_cast<FakeRegion*>(fake.Find(fake.heap_begin + 0x90000000ull, 1));
    assert(region);
    std::memcpy(region->data.data() + 0xD0, blob.data(), blob.size());
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.party.0.max_hp") == 356);
    assert(fake.ints.at("sw.party.detail.open") == 0); // changed identity closes the old card
    assert(reader.DispatchAction("party_detail", 0));
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.party.detail.stats_ready") == 1);

    // A valid new Egg takes this slot: withdraw every expanded summary field immediately.
    sw_pk8::DecryptInPlace(blob.data(), blob.size());
    PutU32(blob, 0x8C, sw_pk8::Rd32(blob.data() + 0x8C) | (1u << 30));
    PutU16(blob, 6, sw_pk8::ComputeChecksum16(blob.data()));
    sw_pk8::CryptArrayU16(blob.data() + 8, blob.size() - 8, 0);
    std::memcpy(region->data.data() + 0xD0, blob.data(), blob.size());
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.party.detail.open") == 0);
    assert(!reader.DispatchAction("party_detail", 0));
    assert(fake.ints.find("sw.party.detail.stat.attack") == fake.ints.end());
    assert(fake.texts.find("sw.party.detail.weak0.text") == fake.texts.end());
    assert(fake.texts.find("sw.party.detail.weak0.src") == fake.texts.end());
    assert(fake.ints.find("sw.party.detail.weak_count") == fake.ints.end());

    reader.ResetSession();
    std::puts("sw_reader: expanded member summaries follow selection/tabs; close and changed Egg withdraw private data");
}

void TestLivePartySixMembers() {
    FakeHost fake = BuildLivePartyFixture({g_mewtwo, g_eternatus, g_zacian,
                                           g_zarude, g_mewtwo, g_zarude});
    fake.personal[150] = {13, 13, 106, 5};
    fake.personal[890] = {9, 15, 140, 5};
    fake.personal[888] = {17, 17, 92, 5};
    fake.personal[893] = {11, 11, 105, 5};
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);
    assert(fake.ints.at("sw.party.count") == 6 && fake.ints.at("sw.party.six") == 1);
    assert(fake.ints.at("sw.party.5.ready") == 1 && fake.ints.at("sw.party.5.species") == 893);
    assert(reader.live_party_slot_ok(5));
    std::puts("sw_reader: a 6-member live party publishes slot 5 from the live array");
}

// Review M1: with the live array readable, a slot whose live read fails is "not ready" -- the
// save mirror's slot i (another order) is never published in its place.
void TestLivePartyFailedSlotNeverFallsBackToMirror() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false); // the mirror: Mewtwo in slot 0
    FakeHost live = BuildLivePartyFixture({g_mewtwo});
    for (auto& r : live.regions) fake.regions.push_back(r);
    fake.personal[150] = {13, 13, 106, 5};
    FakeRegion* slot0 = const_cast<FakeRegion*>(fake.Find(fake.heap_begin + 0x9000'0000ull, 1));
    assert(slot0);
    slot0->data[0xD0 + 0x06] ^= 0xFF; // the live slot fails its checksum
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);
    assert(fake.ints.at("sw.party.0.ready") == 0);
    assert(fake.ints.find("sw.party.0.species") == fake.ints.end());
    assert(!reader.live_party_slot_ok(0) && reader.live_party_ec(0) == 0);
    std::puts("sw_reader: a failed live slot locks its tile, never shows the mirror's slot");
}

// Review H3: an unchanged tick replays exactly what the last render published (the host clears
// its snapshot every tick, so a replay must republish every key, identically).
void TestUnchangedTickReplaysIdenticalKeys() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    FakeHost live = BuildLivePartyFixture({g_mewtwo, g_eternatus});
    for (auto& r : live.regions) fake.regions.push_back(r);
    fake.personal[150] = {13, 13, 106, 5};
    fake.personal[890] = {9, 15, 140, 5};
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);
    const auto ints1 = fake.ints;
    const auto texts1 = fake.texts;
    for (int k = 0; k < 3; ++k) {
        fake.ints.clear();
        fake.texts.clear();
        reader.Sample(host);
        assert(fake.ints == ints1 && fake.texts == texts1);
    }
    assert(fake.ints.at("sw.party.1.species") == 890);
    std::puts("sw_reader: unchanged ticks replay the identical key set (party/trainer/misc8)");
}

void TestLivePartyReflectsAChangeOnTheVeryNextSample() {
    // Freshness check: SampleLiveParty must re-read live memory every Sample() call, never
    // caching a slot's decode across ticks -- the same property the task's own live-hardware
    // verification exercises (a Potion's HP change shows up on the very next read, no reboot).
    // This unit test cannot re-encrypt a modified blob (no encrypt-direction port exists, only
    // decrypt -- see sw_pk8.h), so it proxies the same "did this tick's read actually happen"
    // property by swapping which REAL encrypted member occupies slot 0's memory between two
    // Sample() calls and checking the second call's published values follow the new bytes, not
    // the first call's -- real hardware/the g0 live-verify step is what proves the specific
    // "same mon, HP decreases then a Potion heals it" case end to end.
    FakeHost fake = BuildLivePartyFixture({g_mewtwo});
    fake.personal[150] = {13, 13, 106, 5};
    fake.personal[893] = {11, 11, 105, 5}; // Zarude: mono Dark, base HP 105, Slow
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);
    assert(fake.ints.at("sw.party.0.species") == 150);
    assert(fake.ints.at("sw.party.0.current_hp") == 240);

    // Overwrite slot 0's live memory in place (no re-import/rebuild of the fixture) with a
    // different real member's blob, exactly as a changed guest heap would look between ticks.
    FakeRegion* slot0 = const_cast<FakeRegion*>(fake.Find(fake.heap_begin + 0x9000'0000ull, 1));
    assert(slot0);
    std::memcpy(slot0->data.data() + 0xD0, g_zarude, sw_pk8::kSizeStored);

    fake.ints.clear();
    fake.texts.clear();
    reader.Sample(host);
    assert(fake.ints.at("sw.party.0.species") == 893);
    assert(fake.ints.at("sw.party.0.current_hp") == 351);
    assert(fake.ints.at("sw.party.0.max_hp") == 351);

    std::puts("sw_reader: SampleLiveParty reflects a change to live memory on the very next "
              "Sample() call, with no stale caching across ticks");
}

// Re-encrypts a decrypted 0x148 PK8 (test-only inverse of sw_pk8::DecryptInPlace): recompute the
// checksum, shuffle the blocks (UnshuffleBlocks is a permutation of 4 blocks, so applying it 11
// more times is its inverse: every such permutation's order divides 12), then XOR the stream.
std::array<unsigned char, sw_pk8::kSizeStored> EncryptStored(std::array<unsigned char, sw_pk8::kSizeStored> d) {
    const std::uint16_t chk = sw_pk8::ComputeChecksum16(d.data(), sw_pk8::kSizeStored);
    d[6] = static_cast<unsigned char>(chk & 0xFF);
    d[7] = static_cast<unsigned char>(chk >> 8);
    std::uint32_t pv = 0;
    std::memcpy(&pv, d.data(), 4);
    for (int k = 0; k < 11; ++k) sw_pk8::UnshuffleBlocks(d.data() + 8, (pv >> 13) & 31);
    sw_pk8::CryptArrayU16(d.data() + 8, sw_pk8::kBlockCount * sw_pk8::kBlockSize, pv);
    return d;
}

std::array<unsigned char, sw_pk8::kSizeStored> DecryptedMewtwo() {
    std::array<unsigned char, sw_pk8::kSizeStored> d{};
    std::memcpy(d.data(), g_mewtwo, d.size());
    sw_pk8::DecryptInPlace(d.data(), d.size());
    return d;
}

// Owner 2026-10-31 ("recheck to not spoiler egg, Lumi had this bug before"): an Egg slot shows
// only "Egg" + the generic egg icon; nothing that reveals what hatches. avg_level/able skip it.
void TestEggSlotNeverRevealsContents() {
    auto egg = DecryptedMewtwo();
    egg[0x8C + 3] |= 0x40; // IV32 bit 30 = IsEgg
    const auto enc_egg = EncryptStored(egg);
    {   // round-trip sanity of the test encryptor itself
        const auto m = sw_pk8::DecodeLiveStored(enc_egg.data());
        assert(m.checksum_valid && m.species == 150 && m.egg);
        assert(!sw_pk8::DecodeLiveStored(g_mewtwo).egg);
    }
    FakeHost fake = BuildLivePartyFixture({g_eternatus, enc_egg.data(), g_zacian});
    fake.personal[150] = {13, 13, 106, 5};
    fake.personal[890] = {9, 15, 140, 5};
    fake.personal[888] = {17, 17, 92, 5};
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);

    assert(fake.ints.at("sw.party.count") == 3);
    assert(fake.ints.at("sw.party.1.ready") == 1);
    assert(fake.ints.at("sw.party.1.egg") == 1);
    assert(fake.ints.at("sw.party.0.egg") == 0 && fake.ints.at("sw.party.2.egg") == 0);
    assert(fake.texts.at("sw.party.1.name") == sw_str::S(sw_str::Id::kEgg)); // the game's own word, never the species
    assert(fake.ints.at("sw.party.1.icon_ready") == 1);
    assert(fake.texts.at("sw.party.1.icon_src") ==
           "romfs:/bin/appli/icon_pokemon/poke_icon_0000_00s_n.bntx#poke_icon_0000_00s_n");
    assert(fake.ints.at("sw.party.1.species") == 0);
    assert(fake.ints.at("sw.party.1.level") == 0);
    assert(fake.texts.at("sw.party.1.level_text").empty());
    assert(fake.ints.at("sw.party.1.hp_ready") == 0);
    assert(fake.ints.at("sw.party.1.types_ready") == 0);
    assert(fake.ints.at("sw.party.1.item_ready") == 0);
    assert(fake.ints.at("sw.party.1.status_ready") == 0);
    for (int m = 1; m <= 4; ++m) assert(fake.ints.at("sw.party.1.move" + std::to_string(m) + ".ready") == 0);
    assert(fake.ints.find("sw.party.1.type1") == fake.ints.end());
    assert(fake.ints.find("sw.party.1.move1.id") == fake.ints.end());
    for (const auto& [k, v] : fake.texts)
        if (k.rfind("sw.party.1.", 0) == 0) assert(v.find("\xE8\xB6\x85") == std::string::npos);

    // Never a medicine/PP target or a summary; party reorder still sees a valid slot.
    assert(!reader.live_party_slot_ok(1) && reader.live_party_slot_ok(0));
    assert(!reader.DispatchAction("party_detail", 1));

    // Eternatus Lv100 + Zacian Lv70-ish: the egg's level never enters the mean.
    const int l0 = static_cast<int>(fake.ints.at("sw.party.0.level"));
    const int l2 = static_cast<int>(fake.ints.at("sw.party.2.level"));
    assert(fake.ints.at("sw.party.avg_ready") == 1);
    assert(fake.ints.at("sw.party.avg_level") == (l0 + l2 + 1) / 2);
    assert(fake.ints.at("sw.party.able_ready") == 1);
    assert(fake.ints.at("sw.party.able") == 2);

    // Only an Egg: nothing to average.
    FakeHost only = BuildLivePartyFixture({enc_egg.data()});
    only.personal[150] = {13, 13, 106, 5};
    const EdenDsmodHostApi host2 = MakeHostApi(only);
    sw_reader::Reader reader2;
    reader2.Sample(host2);
    assert(only.ints.at("sw.party.avg_ready") == 0 && only.ints.at("sw.party.avg_level") == 0);
    assert(only.ints.at("sw.party.able") == 0);
    std::puts("sw_reader: an Egg slot publishes only Egg/name/icon (no species/level/HP/types/"
              "moves/item), is no medicine/summary target, and avg_level/able skip it");
}

// Item N: own Pokemon show their nickname (PK8 +0x58 UTF-16LE), incl. non-ASCII.
void TestPartyNicknameDecoded() {
    auto d = DecryptedMewtwo();
    // The fixture Mewtwo stores a Chinese name there (synthetic: "\u6D4B\u8BD5"; the real
    // fixture save's: its Chinese species name), which the package font cannot show.
    const std::string stored = sw_pk8::DecodeNicknameUtf8(d.data());
    assert(stored.size() == 6 && !sw_font::CoversText(stored));
    const char16_t nick[] = u"Renée";
    std::fill(d.begin() + 0x58, d.begin() + 0x72, 0);
    for (std::size_t i = 0; nick[i]; ++i) {
        d[0x58 + i * 2] = static_cast<unsigned char>(nick[i] & 0xFF);
        d[0x58 + i * 2 + 1] = static_cast<unsigned char>(nick[i] >> 8);
    }
    const auto enc = EncryptStored(d);
    FakeHost fake = BuildLivePartyFixture({enc.data(), g_eternatus});
    fake.personal[150] = {13, 13, 106, 5};
    fake.personal[890] = {9, 15, 140, 5};
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);
    assert(fake.texts.at("sw.party.0.name") == "Ren\xC3\xA9" "e");
    // Eternatus' stored (Chinese) name has no glyphs in the package font: species fallback (none
    // in this fixture), never alternate-glyph boxes.
    assert(fake.texts.find("sw.party.1.name") == fake.texts.end());
    assert(sw_font::CoversText("Ren\xC3\xA9" "e") && !sw_font::CoversText("\xE8\xB6\x85") &&
           !sw_font::CoversText("A\xE2\x99\x82"));
    assert(fake.ints.at("sw.party.0.species") == 150 && fake.ints.at("sw.party.0.egg") == 0);
    // Private-use male sign E08E maps to U+2642 too; no terminator within 13 units = rejected.
    std::array<unsigned char, sw_pk8::kSizeStored> raw{};
    raw[0x58] = 'A'; raw[0x5A] = 0x8E; raw[0x5B] = 0xE0;
    assert(sw_pk8::DecodeNicknameUtf8(raw.data()) == "A\xE2\x99\x82");
    for (int i = 0; i < 13; ++i) raw[0x58 + i * 2] = 'B', raw[0x59 + i * 2] = 0;
    assert(sw_pk8::DecodeNicknameUtf8(raw.data()).empty());
    std::puts("sw_reader: party names use the PK8 nickname (UTF-16LE -> UTF-8, non-ASCII + gender "
              "glyphs), species name when not nicknamed");
}

void TestLivePartySlotWithBadChecksumSkipped() {
    FakeHost fake = BuildLivePartyFixture({g_mewtwo});
    fake.personal[150] = {13, 13, 106, 5};
    FakeRegion* slot0 = const_cast<FakeRegion*>(fake.Find(fake.heap_begin + 0x9000'0000ull, 1));
    assert(slot0);
    slot0->data[0xD0 + 0x06] ^= 0xFF; // checksum is plaintext at +0x06, never encrypted
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);

    // SampleParty's own save-mirror anchor is unresolved in this fixture (no party.live region
    // built), so this slot has nothing else to fall back to -- it must simply never appear.
    assert(fake.ints.find("sw.party.0.species") == fake.ints.end());
    assert(fake.ints.find("sw.party.0.current_hp") == fake.ints.end());

    std::puts("sw_reader: a live party slot with a mismatched PK8 checksum is skipped, not "
              "published");
}

void TestCorruptChecksumFailsClosed() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/true);
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);

    // The domain itself is still "ready" (the anchor is verified and the torn-read succeeded),
    // but the one corrupted slot must be refused -- and nothing from it published.
    assert(fake.ints.at("sw.party.ready") == 1);
    assert(fake.ints.at("sw.party.0.ready") == 0);
    assert(fake.ints.find("sw.party.0.species") == fake.ints.end());
    assert(fake.texts.find("sw.party.0.summary") == fake.texts.end());

    std::puts("sw_reader: a slot with a mismatched PK8 checksum is refused, not published");
}

void TestMisc8OutOfRangeFailsClosed() {
    // Money above the game's own 9,999,999 cap is a torn/garbage read, not a real save state
    // (sw_reader.h's SampleMisc8 sanity bound) -- the domain stays "ready" (its anchor is
    // verified and the bytes read cleanly), but none of this tick's values are published.
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false, /*bad_money=*/true);
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);

    assert(fake.ints.at("sw.misc8.ready") == 1);
    assert(fake.ints.find("sw.misc8.money") == fake.ints.end());
    assert(fake.ints.find("sw.misc8.badges") == fake.ints.end());
    assert(fake.ints.find("sw.misc8.bp") == fake.ints.end());
    assert(fake.texts.find("sw.misc8.money_text") == fake.texts.end());

    std::puts("sw_reader: an out-of-cap Misc8 money value is refused, not published");
}

void TestSpeciesTypesMissingRomfsFailsClosed() {
    // A species with no entry in FakeHost::personal stands in for a missing/too-old romfs --
    // sw_species_types::GetTypes() must fail closed and the party loop must still publish the
    // slot's own ready/summary fields untouched (the type lookup is gated independently).
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    fake.personal.erase(150);
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);

    assert(fake.ints.at("sw.party.0.ready") == 1); // unaffected
    assert(fake.ints.at("sw.party.0.types_ready") == 0);
    assert(fake.ints.find("sw.party.0.type1") == fake.ints.end());
    assert(fake.texts.find("sw.party.0.weak_text") == fake.texts.end());

    std::puts("sw_reader: a species with no live type lookup fails the weakness board closed, "
              "without disturbing its own ready/summary");
}

// Same hand-built, format-faithful 2-line .dat fixture 0100ABF008968000_test.cpp's
// TestSpeciesNameDecode uses for monsname.dat (decodes to "Egg"/"Bulbasaur") -- reused here as a
// stand-in for wazaname.dat. The *content* is nonsense for a move table, but the decoder
// (sw_text::ReadDat) and the index convention (line id == table id, line 0 is the "no
// value"/placeholder slot) are byte-for-byte the real on-disk format, which is what this test
// exercises: sw_reader.h's move-name plumbing, not real move names.
constexpr unsigned char kWazaNameFixture[64]{
    0x01, 0x00, 0x02, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x2C, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x00, 0xCC, 0x7C, 0x2C, 0xE4, 0x38, 0x22, 0xF9, 0x12, 0x4E, 0xA6, 0x10, 0x30,
    0x45, 0x83, 0x2E, 0x19, 0x01, 0xCA, 0x75, 0x53, 0x53, 0x98, 0xE1, 0xC1, 0xD4, 0x0C, 0x30, 0x65,
};

// Hand-built, minimal-but-real-format wazabin buffer (sw_move_data.h's single-object FlatBuffers
// table -- see that header's own comment for the field-id convention) used as move id 1's data in
// the battle tests below: Type=12 (Electric), Category=2 (Special), Power=90, PP=40 -- distinct
// from every move id the real-byte fixture in 0100ABF008968000_test.cpp (sw_move_data_test_
// fixture.inc) exercises, so a mismatch between the two decode paths cannot hide. Hand-derivation
// verified offline against the same decode this file's DecodeRecord uses before being pasted in.
constexpr unsigned char kSyntheticElectricSpecialMove[34] = {
    0x1A,0x00,0x00,0x00,0x16,0x00,0x08,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x04,0x00,0x00,0x00,0x05,0x00,0x06,0x00,0x00,0x00,
    0x07,0x00,0x16,0x00,0x00,0x00,0x0C,0x02,0x5A,0x28,
};

// Hand-built, real-format btl_app.dat stand-in (same TextFile container sw_text::ReadDat decodes
// for every other .dat in this project; generated by a one-off offline script using the project's
// own msgtext.py encode-is-decode stream cipher, then round-trip-verified against msgtext.py's
// own read_dat() before being pasted in -- not a byte-for-byte excerpt of the real ~200-line
// btl_app.dat, just the 4 lines sw_effectiveness_text.h actually indexes (17-20), padded with
// placeholder lines 0-16 to reach the right line count/offsets). Decodes to indices
// 17/18/19/20 = "Super effective"/"Effective"/"Not very effective"/"No effect", matching
// sw_effectiveness_text.h's kIdxSuper/kIdxNeutral/kIdxNotVery/kIdxNone exactly.
constexpr unsigned char kBtlAppFixture[366] = {
    0x01,0x00,0x15,0x00,0x5E,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x5E,0x01,0x00,0x00,0xAC,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0xB0,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0xB4,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xB8,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0xBC,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0xC0,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xC4,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0xC8,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0xCC,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xD0,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0xD4,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0xD8,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xDC,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0xE0,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0xE4,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xE8,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0xEC,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0xF0,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x10,0x01,0x00,0x00,
    0x0A,0x00,0x00,0x00,0x24,0x01,0x00,0x00,0x13,0x00,0x00,0x00,
    0x4A,0x01,0x00,0x00,0x0A,0x00,0x00,0x00,0xA4,0x7C,0x4B,0xE4,
    0x21,0xA6,0x65,0x30,0xA2,0xCF,0x7E,0x7C,0x3F,0xF9,0x97,0xC8,
    0xB8,0x22,0xA9,0x14,0x35,0x4C,0xC2,0x60,0xB6,0x75,0xDB,0xAC,
    0x33,0x9F,0xF4,0xF8,0x8C,0xC8,0x0E,0x45,0x09,0xF2,0x27,0x91,
    0x8A,0x1B,0x38,0xDD,0x07,0x45,0x52,0x29,0x80,0x6E,0x6B,0x75,
    0x1D,0x98,0x84,0xC1,0x9E,0xC1,0x9E,0x0D,0x1B,0xEB,0xB7,0x59,
    0x94,0x14,0xC8,0xA5,0x6F,0x3E,0x94,0xF1,0x7F,0x8F,0x19,0x78,
    0x91,0xC3,0x3E,0x1F,0x95,0xF8,0xE1,0xC7,0x58,0x3C,0x94,0xE1,
    0xEC,0x0F,0x0C,0x7C,0xAA,0xE3,0x69,0x1E,0x9D,0xF0,0xC7,0x87,
    0xFA,0x67,0x9D,0x3D,0xBF,0xEF,0xAA,0x7E,0x18,0xF6,0xAB,0xB3,
    0x94,0x9E,0x9A,0xF7,0x02,0xBF,0x3D,0xFB,0x0C,0x91,0x7B,0x8A,
    0xD0,0x50,0x02,0x85,0x62,0x29,0xC4,0x48,0x78,0x45,0x2B,0x28,
    0xB1,0x42,0xEF,0x14,0x36,0xA4,0xE3,0x22,0x4C,0x14,0x2B,0xA1,
    0x31,0x0A,0x41,0x52,0x34,0x91,0x71,0x8A,0xA4,0x50,0x8B,0xBA,
    0x42,0xD6,0x4E,0xB1,0x10,0x8B,0xCA,0x5B,0x04,0xDD,0x73,0xEB,
    0xD4,0x58,0xCE,0xC5,0xD6,0x2D,
};

/// Adds a live battle (own + foe BattleMon, sw_battle.h's layout) and a CurrentScreen value to an
/// already-built trainer/party/misc8 fixture (BuildFixture above), at the real verified anchors'
/// heap-relative addresses. `own_species`/`own_level` default to the shared party fixture's own
/// Mewtwo (species 150, level 100) so the move-identity match in sw_reader.h's SampleBattle finds
/// exactly one party slot -- the task's own sanctioned move-id source.
void AddBattleFixture(FakeHost& fake, std::uint32_t current_screen, std::uint16_t own_cur_hp,
                       std::uint16_t own_max_hp, std::uint16_t foe_species, std::uint16_t foe_level,
                       std::uint16_t foe_cur_hp, std::uint16_t foe_max_hp) {
    const auto* a_cs = sw_addresses::Find("location.current_screen");
    const auto* a_own = sw_addresses::Find("battle.own_mon");
    assert(a_cs && a_cs->verified && a_own && a_own->verified);

    std::vector<unsigned char> cs_buf(4, 0);
    PutU32(cs_buf, 0, current_screen);
    fake.regions.push_back({fake.heap_begin + a_cs->sysbot_offset, std::move(cs_buf)});

    // own_mon directly followed by foe_mon at +kFoeOffset -- one contiguous region big enough for
    // both, matching the real struct geometry sw_addresses.h's anchor note records.
    std::vector<unsigned char> battle_buf(sw_battle::kFoeOffset + sw_battle::kStructBytes, 0);
    // Move slot triple: {move_id, pp_current, pp_max} -- move_id now comes straight from the
    // live struct (sw_battle.h's Move::move_id, found+verified this task at +0x2D4 + i*0x1C),
    // not matched against a party slot any more.
    struct MoveSlot { std::uint16_t move_id; std::uint8_t cur, max; };
    const auto put_mon = [&](std::size_t base, std::uint16_t species, std::uint16_t max_hp,
                              std::uint16_t cur_hp, std::uint8_t level,
                              std::initializer_list<MoveSlot> moves) {
        PutU16(battle_buf, base + 0x70, species);
        PutU16(battle_buf, base + 0x72, max_hp);
        PutU16(battle_buf, base + 0x74, cur_hp);
        battle_buf[base + 0x7C] = level;
        int i = 0;
        for (const auto& mv : moves) {
            const std::size_t slot = base + 0x2D8 + static_cast<std::size_t>(i) * 0x1C;
            battle_buf[slot] = mv.cur;
            battle_buf[slot + 1] = mv.max;
            PutU16(battle_buf, base + 0x2D4 + static_cast<std::size_t>(i) * 0x1C, mv.move_id);
            ++i;
        }
    };
    // Own Mewtwo, 4 move slots exercising every gating case this task's rewrite needs:
    //   slot1: move_id 999 -- no wazaname/wazabin entry in this fixture -> name/type/
    //          effectiveness all fail closed to PP-only, still `ready` (a real, used slot).
    //   slot2: move_id 1 -- resolves via kWazaNameFixture ("Bulbasaur", a stand-in name) AND
    //          kSyntheticElectricSpecialMove (Electric/Special/90/40) -> name, type disc and
    //          (against the Electric-weak-to-Ground-only foe below) an effectiveness word.
    //   slot3: move_id 50 (no name/data fixture) with pp_current=0 -- a real, used, now-empty-PP
    //          move must still read `ready=1` (PP from the live struct is authoritative).
    //   slot4: move_id 0 but a nonzero PP byte pair -- this task's own new fail-closed rule
    //          (`slot_ready = move_id!=0 && pp_max>0`) must treat this as NOT ready (an
    //          inconsistent/torn-looking read), where the prior pp_max-only rule would have
    //          wrongly shown it.
    put_mon(0, 150, own_max_hp, own_cur_hp, 100,
            {{999, 5, 20}, {1, 3, 16}, {50, 0, 32}, {0, 16, 16}});
    put_mon(sw_battle::kFoeOffset, foe_species, foe_max_hp, foe_cur_hp,
            static_cast<std::uint8_t>(foe_level), {{10, 10, 10}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}});
    fake.regions.push_back({fake.heap_begin + a_own->sysbot_offset, std::move(battle_buf)});

    fake.dat_files[sw_move_names::WazaNameDatPath()] =
        std::vector<unsigned char>(std::begin(kWazaNameFixture), std::end(kWazaNameFixture));
    fake.dat_files[sw_effectiveness_text::BtlAppDatPath()] =
        std::vector<unsigned char>(std::begin(kBtlAppFixture), std::end(kBtlAppFixture));
    fake.waza_files[1] = std::vector<unsigned char>(std::begin(kSyntheticElectricSpecialMove),
                                                     std::end(kSyntheticElectricSpecialMove));
}

// Overwrites an already-fixtured CurrentScreen region in place (same address AddBattleFixture
// wrote it at) -- simulates the game's own value changing between two Sample() ticks on one
// persistent Reader, the way a real emulator session would.
// The guest helper's field hook beating a few frames (FieldStatus::frames): with the helper, a
// battle ends only once the field runs again, not on a silent battle beat alone (a stall).
std::uint64_t g_field_frames = 0;
void FieldRan(sw_reader::Reader& reader) { reader.SetFieldFrames(g_field_frames += 10); }

void SetCurrentScreen(FakeHost& fake, std::uint32_t value) {
    const auto* a_cs = sw_addresses::Find("location.current_screen");
    assert(a_cs && a_cs->verified);
    const std::uint64_t addr = fake.heap_begin + a_cs->sysbot_offset;
    for (auto& r : fake.regions) {
        if (r.base != addr || r.data.size() < 4) continue;
        PutU32(r.data, 0, value);
        return;
    }
    assert(false && "CurrentScreen region not found -- call AddBattleFixture first");
}

// Writes sw_addresses.h's "location.zone_name" anchor region (the real 8-byte tag + a UTF-16LE
// name, zero-padded to the anchor's own size) into `fake`, creating the region on first use and
// overwriting it in place on every later call -- mirroring a real Fly/warp overwriting the SAME
// live buffer (sw_zone.h's own header comment: a reused mutable buffer, not a fresh allocation).
// An empty `name` writes the valid tag followed immediately by a double-null:
// sw_zone::DecodeZoneName's own documented "empty decode" fail-closed path -- "the tag's region
// exists but no real place name has been decided yet", the exact live-confirmed title-screen
// reading this task's design-revision-2 verified (`sw.location.zone_ready` read ABSENT for the
// entire title-idle wait, 2 independent boots -- see SampleState's header comment).
void SetZoneName(FakeHost& fake, const std::string& name) {
    const auto* a_zone = sw_addresses::Find("location.zone_name");
    assert(a_zone && a_zone->verified);
    std::vector<unsigned char> buf(a_zone->size, 0);
    std::memcpy(buf.data(), sw_zone::kZoneTag, sizeof sw_zone::kZoneTag);
    for (std::size_t i = 0; i < name.size(); ++i)
        PutU16(buf, 8 + i * 2, static_cast<std::uint16_t>(static_cast<unsigned char>(name[i])));
    const std::uint64_t addr = fake.heap_begin + a_zone->sysbot_offset;
    for (auto& r : fake.regions) {
        if (r.base != addr) continue;
        r.data = std::move(buf);
        return;
    }
    fake.regions.push_back({addr, std::move(buf)});
}

// Merges ONE valid, checksum-correct live party slot (the same real encrypted Mewtwo blob +
// personal-table record `TestLiveStoredPartyChainReadsThreeRealMembers` already uses) into an
// existing fixture, so `AnyLivePartySlotValid()` reads true -- matching real hardware, where the
// live party checksum-validates even AT THE TITLE SCREEN (the title screen's own cosmetic
// stadium render decrypts the real save's party to pose it; live-verified this task, design-
// revision-2 -- see SampleState's header comment). Phase-machine tests below use this so
// party_valid is never the thing actually under test; `zone_cache_ready_` (via SetZoneName) is.
void AddLiveParty(FakeHost& fake) {
    FakeHost live = BuildLivePartyFixture({g_mewtwo});
    for (auto& r : live.regions) fake.regions.push_back(std::move(r));
    fake.personal[150] = {13, 13, 106, 5}; // Mewtwo: mono Psychic, base HP 106, Slow (real romfs values)
}

// Drives `reader` through a synthetic title->loading->field sequence (zone becomes valid, then
// the field sentinel confirms it -- the instant path, see SampleState's kLoading case) so the
// field/battle sentinels are trusted again, matching how the real module is always used (title
// always precedes any battle). Several existing tests below set up a battle/field fixture and
// expect it classified on the Reader's very first Sample() call; that is unrealistic in isolation
// now that a freshly-constructed Reader always starts at kTitle, so they prime through this
// helper first. The caller's fixture must already have `AddLiveParty(fake)` applied.
void PrimeWorldEntered(FakeHost& fake, const EdenDsmodHostApi& host, sw_reader::Reader& reader) {
    SetZoneName(fake, "Wyndon");
    SetCurrentScreen(fake, 0xFFFF5127u);
    reader.Sample(host); // kTitle -> kLoading (zone_cache_ready_ becomes true)
    reader.Sample(host); // kLoading -> kField (party+zone valid, field sentinel confirms instantly)
}

void TestNativeBattleMenuFsmClassification() {
    FakeHost fake = BuildFixture(false);
    fake.main_base = 0x7100000000ull;
    AddBattleFixture(fake, 0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    AddLiveParty(fake);
    const auto host = MakeHostApi(fake);
    sw_reader::Reader reader;
    PrimeWorldEntered(fake, host, reader);
    const std::uint64_t client = fake.heap_begin + 0xA1000000ull;
    fake.regions.push_back({client, std::vector<unsigned char>(0x580)});
    auto& cl = fake.regions.back().data;
    auto state = [&](std::uint64_t function, unsigned step) {
        PutU64(cl, 0x560, fake.main_base + function);
        PutU64(cl, 0x568, 0);
        PutU32(cl, 0x570, step);
    };
    auto sample = [&] {
        fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    };
    SetCurrentScreen(fake, 0xFFFFFFFFu);
    reader.SetBattleHint(true, true, client);
    state(0x8021D0, 5); sample(); // exact select-action command wait
    assert(reader.native_battle_command_waiting());
    assert(fake.ints.at("sw.battle.native_command_waiting") == 1);
    assert(fake.ints.at("sw.battle.native_window_ready") == 1);
    assert(fake.ints.at("sw.battle.native_bag") == 0 && fake.ints.at("sw.battle.native_party") == 0);
    state(0x8039C0, 2); sample(); // Bag picker result wait, no command entry
    assert(fake.ints.at("sw.battle.native_bag") == 1 && !reader.native_battle_command_waiting());
    state(0x8039C0, 1); sample(); // Bag opening transition is not its result wait
    assert(fake.ints.at("sw.battle.native_bag") == 0 && !reader.native_battle_command_waiting());
    state(0x8032C0, 1); sample();
    assert(fake.ints.at("sw.battle.native_party") == 1 && !reader.native_battle_command_waiting());
    assert(fake.ints.at("sw.battle.native_party_forced") == 0);
    state(0x8032C0, 0); sample();
    assert(fake.ints.at("sw.battle.native_party") == 0 && !reader.native_battle_command_waiting());

    // Forced replacement has an outer event and can leave the ordinary command FSM stale.
    state(0x8021D0, 5);
    cl[0x2A8] = 1;
    PutU64(cl, 0x548, fake.main_base + 0x7FF830);
    PutU64(cl, 0x550, 0);
    PutU64(cl, 0x558, 3);
    sample();
    assert(fake.ints.at("sw.battle.native_party") == 1);
    assert(fake.ints.at("sw.battle.native_party_forced") == 1);
    assert(fake.ints.at("sw.battle.native_command_waiting") == 0 && !reader.native_battle_command_waiting());
    // Transition/event lookalikes must not claim the native party picker.
    PutU64(cl, 0x558, 2); state(0x8021D0, 4); sample();
    assert(fake.ints.at("sw.battle.native_party_forced") == 0 && fake.ints.at("sw.battle.native_party") == 0);
    assert(!reader.native_battle_command_waiting());
    cl[0x2A8] = 0;
    state(0x8021D0, 5); sample();
    assert(reader.native_battle_command_waiting()); // exact ordinary wait returns after event
    cl.resize(0x560); sample(); // missing state span fails closed rather than retaining last wait
    assert(fake.ints.at("sw.battle.native_window_ready") == 0);
    assert(!reader.native_battle_command_waiting());
    assert(fake.ints.at("sw.battle.native_party") == 0 && fake.ints.at("sw.battle.native_bag") == 0);
    std::puts("sw_reader: native command/Bag/party/forced-replacement FSM states, transitions and missing-span guards");
}

void TestBattleBallIdentityPagingAndViewReset() {
    FakeHost fake = BuildFixture(false);
    fake.main_base = 0x7100000000ull;
    AddBattleFixture(fake, 0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    AddLiveParty(fake);
    const auto* bag_anchor = sw_addresses::Find("bag.item_pouch");
    assert(bag_anchor && bag_anchor->verified);
    const std::uint64_t bag = fake.heap_begin + bag_anchor->sysbot_offset;
    const std::uint64_t client = fake.heap_begin + 0xA2000000ull, main = client + 0x1000, setup = client + 0x2000;
    std::vector<unsigned char> cl(0x580), ma(0x200), se(0x400);
    PutU64(cl, 0x58, main); PutU64(ma, 0x88, setup);
    PutU32(ma, 0x150, 0); PutU32(se, 0, 0); se[0x3B8] = 0; // offline wild single, same native gates as catalog
    PutU64(cl, 0x560, fake.main_base + 0x8021D0); PutU32(cl, 0x570, 5);
    fake.regions.push_back({client, cl}); fake.regions.push_back({main, ma}); fake.regions.push_back({setup, se});
    fake.regions.push_back({bag, std::vector<unsigned char>(sw_item_pouch::kBlockBytes)});
    auto set_balls = [&](std::initializer_list<std::pair<unsigned, unsigned>> balls) {
        auto* region = const_cast<FakeRegion*>(fake.Find(bag, sw_item_pouch::kBlockBytes));
        assert(region);
        std::fill(region->data.begin(), region->data.end(), 0);
        std::size_t offset = sw_item_pouch::kPouches[1].byte_offset;
        for (const auto& [id, count] : balls) {
            PutU32(region->data, offset, id | (count << 15)); offset += 4;
        }
    };
    // These synthetic item slots exercise catalog identity, not item-name rendering.
    set_balls({{4,10},{1,2},{576,3},{851,4},{500,1},{17,9}});
    const auto host = MakeHostApi(fake);
    sw_reader::Reader reader;
    PrimeWorldEntered(fake, host, reader);
    SetCurrentScreen(fake, 0xFFFFFFFFu); reader.SetBattleHint(true, true, client);
    auto sample = [&] { fake.ints.clear(); fake.texts.clear(); reader.Sample(host); reader.PublishBattleScreen(host); };
    sample();
    assert(reader.battle_has_balls() && fake.ints.at("sw.battle.ball.total") == 4);
    assert(reader.battle_ball_id() == 4); // excludes Park500 and medicine17 despite owning both
    assert(fake.ints.at("sw.bike.owned_known") == 1 && fake.ints.at("sw.bike.owned") == 0); // no 1081
    assert(!reader.CycleBattleBall(1)); // command view cannot page the invisible selector
    reader.SetBattleScreen(2); sample();
    assert(fake.ints.at("sw.battle.view.balls") == 1);
    assert(fake.ints.count("sw.battle.ball.ready") && fake.ints.count("sw.battle.ball.first.ready"));
    sample(); // unchanged pair: the selected/first ball keys replay from their tape
    assert(fake.ints.count("sw.battle.ball.ready") && fake.ints.count("sw.battle.ball.first.ready"));
    assert(!fake.ints.count("sw.battle.ball.0.ready")); // no per-index slots (none are bound)
    assert(reader.CycleBattleBall(1) && reader.battle_ball_id() == 1);
    set_balls({{851,4},{4,10},{576,3},{1,2}}); sample();
    assert(reader.battle_ball_id() == 1 && fake.ints.at("sw.battle.ball.index") == 3);
    set_balls({{851,4},{576,3},{1,2}}); sample();
    assert(reader.battle_ball_id() == 1 && fake.ints.at("sw.battle.ball.index") == 2); // no row-index retargeting
    set_balls({{851,4},{576,3},{1,0}}); sample();
    assert(reader.battle_screen() == 0); // selected native item depleted: close, don't pick another throw silently
    reader.SetBattleScreen(2); sample();
    assert(reader.battle_ball_id() == 851);
    assert(reader.CycleBattleBall(-1) && reader.battle_ball_id() == 576);
    assert(reader.CycleBattleBall(1) && reader.battle_ball_id() == 851);
    reader.SetBattleScreen(1); reader.SetBattleMoveDetailTab(3); sample();
    assert(fake.ints.at("sw.battle.view.moves") == 1 && fake.ints.at("sw.battle.view.balls") == 0);
    assert(fake.ints.at("sw.battle.move_detail.tab") == 3);
    reader.SetBattleScreen(0); reader.SetBattleScreen(1); sample();
    assert(fake.ints.at("sw.battle.move_detail.tab") == 0); // reopened move view restores its detail tab
    reader.SetBattleScreen(2); sample();
    auto* setup_region = const_cast<FakeRegion*>(fake.Find(setup, 0x400));
    assert(setup_region); PutU32(setup_region->data, 0, 1); // trainer now: no capture catalog
    sample();
    assert(!reader.battle_has_balls() && reader.battle_screen() == 0 && reader.battle_ball_id() == 0);
    assert(fake.ints.at("sw.battle.ball.empty") == 1);
    PutU32(setup_region->data, 0, 0); sample();
    reader.SetBattleScreen(2); assert(reader.battle_screen() == 2);
    reader.SetBattleHint(false, true, 0); // a silent beat, no field frame: an emulation stall keeps it
    assert(reader.battle_screen() == 2 && reader.battle_client() == client);
    FieldRan(reader);
    reader.SetBattleHint(false, true, 0); // the battle is over (field runs): forget the subview immediately
    assert(reader.battle_screen() == 0 && reader.battle_ball_id() == 0 && reader.battle_client() == 0);
    reader.ResetSession();
    assert(reader.battle_screen() == 0 && !reader.battle_has_balls() && reader.battle_ball_id() == 0);
    std::puts("sw_reader: ball native-ID selection survives reorder/removal before it, depletion closes, view/client/session reset");
}

void TestBattleReaderInWildBattle() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    // Pikachu (species 25, Electric/personal type id 12) as the foe -- distinct from the own
    // Mewtwo fixture, and Electric gives a real, checkable weakness (Ground, 2x in the standard
    // chart) for the matchup card's PublishWeaknessBoard call.
    fake.personal[25] = {12, 12};
    AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, /*own_cur_hp=*/200, /*own_max_hp=*/356,
                      /*foe_species=*/25, /*foe_level=*/55, /*foe_cur_hp=*/40, /*foe_max_hp=*/120);
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    AddLiveParty(fake);
    PrimeWorldEntered(fake, host, reader); // the title gate requires leaving title for real first
    SetCurrentScreen(fake, 0xFFFFFFFFu);   // PrimeWorldEntered left current_screen at the field sentinel
    reader.Sample(host);

    assert(fake.ints.at("sw.state.ready") == 1);
    assert(fake.ints.at("sw.state.in_battle") == 1);

    assert(fake.ints.at("sw.battle.own.ready") == 1);
    assert(fake.ints.at("sw.battle.own.species") == 150);
    assert(fake.ints.at("sw.battle.own.level") == 100);
    assert(fake.ints.at("sw.battle.own.cur_hp") == 200);
    assert(fake.ints.at("sw.battle.own.max_hp") == 356);
    assert(fake.ints.at("sw.battle.own.hp_pct") == 200 * 100 / 356);
    assert(fake.texts.at("sw.battle.own.hp_text") == "200/356");
    // Live PP from the battle struct, NOT the party's static pp1 (29) -- see AddBattleFixture.
    // slot1 (move_id 999): PP is real, but nothing resolves (no wazaname/wazabin entry for this
    // id in the fixture) -- name/type/effectiveness all fail closed to PP-only, never invented.
    assert(fake.ints.at("sw.battle.own.move1.ready") == 1);
    assert(fake.texts.at("sw.battle.own.move1.pp_text") == "5/20");
    assert(fake.ints.at("sw.battle.own.move1.name_ready") == 0);
    assert(fake.texts.find("sw.battle.own.move1.name") == fake.texts.end());
    assert(fake.ints.at("sw.battle.own.move1.type_ready") == 0);
    assert(fake.ints.at("sw.battle.own.move1.effectiveness_ready") == 0);
    assert(fake.ints.at("sw.battle.own.move1.catpow_ready") == 0);

    // slot2 (move_id 1): resolves a name ("Bulbasaur", the tiny fixture's stand-in text -- not a
    // real move name, see kWazaNameFixture's own comment), move data (Electric/Special/90/40 via
    // kSyntheticElectricSpecialMove) and, against the mono-Electric Pikachu foe below (the
    // standard chart: Electric resists Electric, 50%), a real effectiveness word+multiplier.
    assert(fake.ints.at("sw.battle.own.move2.ready") == 1);
    assert(fake.texts.at("sw.battle.own.move2.pp_text") == "3/16");
    assert(fake.ints.at("sw.battle.own.move2.name_ready") == 1);
    assert(fake.texts.at("sw.battle.own.move2.name") == "Bulbasaur");
    assert(fake.ints.at("sw.battle.own.move2.type_ready") == 1);
    assert(fake.ints.at("sw.battle.own.move2.type_id") == 12); // Electric, personal-table order
    assert(fake.ints.at("sw.battle.own.move2.effectiveness_ready") == 1);
    // D3 collapsed pills: category · power from the same move record (damaging -> a real power).
    assert(fake.ints.at("sw.battle.own.move2.catpow_ready") == 1);
    assert(fake.texts.at("sw.battle.own.move2.catpow_text") == "Special \xc2\xb7 90");
    assert(fake.texts.at("sw.battle.own.move2.effectiveness_text") ==
           "Not very effective \xc3\x97\xc2\xbd"); // "Not very effective x1/2"
    assert(fake.texts.at("sw.battle.own.move2.effectiveness_multiplier") == "\xc3\x97\xc2\xbd");

    // slot3 (move_id 50, pp_current=0): a real, used, now-empty-PP move must still read ready=1
    // (PP from the live struct is authoritative) -- no name/data fixture for id 50, PP-only.
    assert(fake.ints.at("sw.battle.own.move3.ready") == 1);
    assert(fake.texts.at("sw.battle.own.move3.pp_text") == "0/32");
    assert(fake.ints.at("sw.battle.own.move3.name_ready") == 0);

    // slot4 (move_id 0, but a nonzero PP byte pair): this task's own new fail-closed rule
    // (`slot_ready = move_id!=0 && pp_max>0`) must treat this as NOT ready -- the prior
    // pp_max-only rule would have wrongly shown "16/16" for an empty slot's stale/torn bytes.
    assert(fake.ints.at("sw.battle.own.move4.ready") == 0);
    assert(fake.texts.find("sw.battle.own.move4.pp_text") == fake.texts.end());

    assert(fake.ints.at("sw.battle.foe.ready") == 1);
    assert(fake.ints.at("sw.battle.foe.species") == 25);
    // Foe tab move rows (owner 2026-10-31, Lumi's Foe page): the foe's own live moves, PP-only
    // when nothing resolves, empty slots not ready.
    assert(fake.ints.at("sw.battle.foe.move1.ready") == 1);
    assert(fake.texts.at("sw.battle.foe.move1.pp_text") == "10/10");
    assert(fake.ints.at("sw.battle.foe.move1.name_ready") == 0);
    assert(fake.ints.at("sw.battle.foe.move1.show_fallback") == 1);
    assert(fake.ints.at("sw.battle.foe.move1.effectiveness_ready") == 0);
    assert(fake.ints.at("sw.battle.foe.move2.ready") == 0);
    assert(fake.ints.at("sw.battle.foe.level") == 55);
    assert(fake.ints.at("sw.battle.foe.hp_pct") == 40 * 100 / 120);
    assert(fake.texts.at("sw.battle.foe.hp_pct_text") == std::to_string(40 * 100 / 120) + "%");
    // Owner now requests exact HP and a proportional sprite pill on the companion.
    assert(fake.ints.at("sw.battle.foe.current_hp") == 40);
    assert(fake.ints.at("sw.battle.foe.max_hp") == 120);
    assert(fake.texts.at("sw.battle.foe.hp_text") == "40/120");
    assert(fake.ints.at("sw.battle.foe.types_ready") == 1);
    assert(fake.ints.at("sw.battle.foe.type1") == 12);
    assert(fake.ints.at("sw.battle.foe.mono_type") == 1);
    assert(fake.texts.at("sw.battle.foe.weak_text").find(sw_str::TypeName(4)) != std::string::npos);
    // D2 port additions: native type label, the "Not very effective" row (Electric resists
    // Flying, Steel and Electric in the standard chart), box icons for own + foe (Pikachu 25 is a
    // pre-Galar species -> icon id = dex number, male form-00 icon), and the bench strip (the
    // Mewtwo Lv.100 active mon matches party slot 0 exactly; the fixture has no other valid slot).
    assert(fake.texts.at("sw.battle.foe.type1_label_src").find("typeIcon_text_12_denki_00^o") != std::string::npos);
    assert(fake.ints.at("sw.battle.foe.resist_count") == 3);
    assert(fake.ints.at("sw.battle.foe.type2_show") == 0);   // mono-type: no second disc/label
    assert(fake.ints.at("sw.battle.foe.resist0_ready") == 1);
    assert(fake.ints.at("sw.battle.foe.resist3_ready") == 0);
    assert(fake.ints.at("sw.battle.foe.icon_ready") == 1);
    assert(fake.texts.at("sw.battle.foe.icon_src").find("poke_icon_0025_00m_n.bntx#poke_icon_0025_00m_n") != std::string::npos);
    assert(fake.ints.at("sw.battle.own.icon_ready") == 1);
    assert(fake.texts.at("sw.battle.own.icon_src").find("poke_icon_0150_00n_n") != std::string::npos);
    assert(fake.ints.at("sw.battle.bench.0.ready") == 0);

    std::puts("sw_reader: battle struct (own+foe) decodes and publishes correctly in a wild "
              "battle; move identity comes straight from the live struct's own move_id (PP, type "
              "disc and effectiveness all fail closed independently when unresolved); a move_id=0 "
              "slot with stray PP bytes is correctly NOT ready");
}

// BATTLE AUTO-SWITCH RELIABILITY FIX (2026-10-25 task): reproduces the exact failure mode that
// made the live battle<->field page switch fire "once, then not reliably" -- see sw_reader.h's
// SampleState comment for the full root-cause writeup. Drives one persistent Reader across a
// sequence of ticks that includes an "unclassified" CurrentScreen reading (neither sentinel --
// stands in for the field<->battle transition frame, or a torn read) between two real state
// values, and asserts `sw.state.in_battle`/`sw.state.ready` are published on EVERY tick, holding
// the last real classification, never missing -- which is exactly what keeps
// `ModRuntime::DrivePageBinds`'s edge-arming from resetting mid-transition.
void TestInBattleStickyAcrossUnclassifiedTick() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    fake.personal[25] = {12, 12};
    AddBattleFixture(fake, /*current_screen=*/0xFFFF5127u, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader; // one Reader instance across every tick below -- sticky state lives
                              // on the Reader (last_in_battle_), not in the per-tick snapshot.

    // This test is specifically about the battle<->field toggle, not the title/loading transition
    // (covered separately by the TestPhase* tests) -- prime past title/loading first (the title
    // gate would otherwise ignore the very first field reading below, see PrimeWorldEntered).
    AddLiveParty(fake);
    PrimeWorldEntered(fake, host, reader);
    SetCurrentScreen(fake, 0xFFFF5127u); // PrimeWorldEntered already leaves this value set; explicit
                                          // here so "tick 1" below reads as the narrative intends.

    reader.Sample(host); // tick 1: field, first classification this "boot"
    assert(fake.ints.at("sw.state.ready") == 1);
    assert(fake.ints.at("sw.state.in_battle") == 0);

    SetCurrentScreen(fake, 0x00000001u); // tick 2: unclassified (neither sentinel) -- e.g. the
                                          // transition frame, or a one-tick read hiccup
    fake.ints.erase("sw.state.in_battle"); // prove THIS tick republishes it, not stale std::map
    fake.ints.erase("sw.state.ready");      // state left over from the previous Sample() call
    reader.Sample(host);
    assert(fake.ints.at("sw.state.ready") == 1 &&
           "an unclassified tick must still republish ready (sticky), or DrivePageBinds disarms");
    assert(fake.ints.at("sw.state.in_battle") == 0 &&
           "an unclassified tick must republish the LAST real classification (still field here)");

    SetCurrentScreen(fake, 0xFFFFFFFFu); // tick 3: the real edge into battle
    reader.Sample(host);
    assert(fake.ints.at("sw.state.ready") == 1);
    assert(fake.ints.at("sw.state.in_battle") == 1);
    // SampleBattle's own in-battle re-derivation is strict (not sticky): the live battle struct
    // must actually be read and published now that we're really in battle.
    assert(fake.ints.at("sw.battle.own.ready") == 1);

    SetCurrentScreen(fake, 0x00000002u); // tick 4: unclassified again, now mid-battle
    fake.ints.erase("sw.state.in_battle");
    fake.ints.erase("sw.state.ready");
    reader.Sample(host);
    assert(fake.ints.at("sw.state.ready") == 1);
    assert(fake.ints.at("sw.state.in_battle") == 1 &&
           "an unclassified tick mid-battle must not silently drop back to field -- this is the "
           "exact edge DrivePageBinds would otherwise swallow (fires once, then not reliably)");

    SetCurrentScreen(fake, 0xFFFF5127u); // tick 5: the real edge back out of battle
    reader.Sample(host);
    assert(fake.ints.at("sw.state.in_battle") == 0);
    assert(fake.ints.at("sw.battle.own.ready") == 0); // battle struct no longer shown post-battle

    std::puts("sw_reader: sw.state.in_battle stays published (sticky, last known classification) "
              "across an unclassified tick on both the entry and exit edge -- the fix for the "
              "unreliable battle<->field auto-switch");
}

// Direct battle commands (2026-10-26): CurrentScreen was seen reading 0 (not the 0xFFFFFFFF
// sentinel) with the native command menu up; the guest helper's "command menu waiting" beat
// (Reader::SetBattleHint, set by the module) must make the battle struct readable anyway, and
// battle_view() must carry the move ids/PP the module's tap-time checks and FIGHT request use.
void TestBattleHintFromHelperAndBattleView() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    fake.personal[25] = {12, 12};
    AddBattleFixture(fake, /*current_screen=*/0x00000000u, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    AddLiveParty(fake);
    PrimeWorldEntered(fake, host, reader);
    SetCurrentScreen(fake, 0x00000000u);

    reader.Sample(host);
    assert(fake.ints.at("sw.battle.own.ready") == 0); // no sentinel, no hint: fail closed
    assert(!reader.battle_view().in_battle && !reader.battle_view().own_ok);

    reader.SetBattleHint(true, true, 0);
    reader.Sample(host);
    assert(fake.ints.at("sw.battle.own.ready") == 1);
    assert(fake.ints.at("sw.state.in_battle") == 1);
    const auto& v = reader.battle_view();
    assert(v.in_battle && v.own_ok);
    assert(v.move_id[0] == 999 && v.pp[0] == 5);
    assert(v.move_id[1] == 1 && v.pp[1] == 3);
    assert(v.move_id[2] == 50 && v.pp[2] == 0); // real move, no PP -> CheckFight refuses NoPp
    assert(v.move_id[3] == 0);                  // torn/unused slot -> CheckFight refuses NoMove
    sw_helper::Status st;
    st.helper_present = st.menu_waiting = st.offline_single = true;
    assert(sw_helper::CheckFight(st, v, 0) == sw_helper::Refusal::None);
    assert(sw_helper::CheckFight(st, v, 2) == sw_helper::Refusal::NoPp);
    assert(sw_helper::CheckFight(st, v, 3) == sw_helper::Refusal::NoMove);

    FieldRan(reader); // the guest runs the field again (battle over, not a stall)
    reader.SetBattleHint(false, true, 0);
    SetCurrentScreen(fake, 0xFFFF5127u); // back to the field
    reader.Sample(host);
    assert(fake.ints.at("sw.battle.own.ready") == 0);
    assert(!reader.battle_view().own_ok);
    std::puts("sw_reader: helper battle hint reads the battle struct without the CurrentScreen "
              "sentinel; battle_view() feeds the direct-command checks");
}


// Phase 2 (2026-10-26): with the helper's battle-alive beat the battle page keeps live data
// through turn animations (CurrentScreen 0) and the native party/bag screens (overlay value), the
// active battler follows the client's battle party (switches), the bench carries pokeIDs and
// fainted flags for the SWITCH/ITEM checks, and the battle ends only after the beat stops.
std::vector<unsigned char> NativeMapLayoutFixture() {
    std::vector<unsigned char> bytes(0x14);
    auto append_section = [&](const char* kind, std::size_t length) {
        const auto offset = bytes.size(); bytes.resize(offset + length);
        std::memcpy(bytes.data() + offset, kind, 4); PutU32(bytes, offset + 4, length);
        return offset;
    };
    auto pane = [&](const char* kind, const char* name, float x, float y, float w, float h, unsigned origin) {
        const auto p = append_section(kind, 8 + 0x4C) + 8;
        bytes[p] = 1; bytes[p + 1] = origin;
        std::memcpy(bytes.data() + p + 4, name, std::strlen(name));
        const float scale = 1;
        for (const auto& [offset, value] : std::vector<std::pair<std::size_t, float>>{
                 {0x24, x}, {0x28, y}, {0x3C, scale}, {0x40, scale}, {0x44, w}, {0x48, h}})
            std::memcpy(bytes.data() + p + offset, &value, sizeof value);
    };
    pane("pan1", "RootPane", 0, -540, 0, 0, 0); append_section("pas1", 8);
    pane("pic1", "P_bg_00", 0, 0, 1920, 4932, 8);
    pane("pic1", "P_bg_01", 960, 2466, 1540, 1540, 1);
    pane("pic1", "P_bg_02", 0, 0, 1920, 2060, 4);
    pane("pan1", "N_zones", 0, 0, 0, 0, 0); append_section("pas1", 8);
    pane("pan1", "b_z_t0101", 17, 162, 64, 64, 0);
    pane("pan1", "b_z_wr0201", 1750, 2180, 30, 40, 0);
    pane("pan1", "b_z_wr0301", -396, -786, 30, 40, 0);
    append_section("pae1", 8); append_section("pae1", 8);
    std::memcpy(bytes.data(), "FLYT", 4); PutU16(bytes, 4, 0xFEFF);
    PutU16(bytes, 6, 0x14); PutU32(bytes, 0xC, bytes.size());
    return bytes;
}
void TestNativeMapGeometryAndHash() {
    auto bytes = NativeMapLayoutFixture();
    sw_town_map::Table table;
    assert(sw_town_map::Decode(bytes.data(), bytes.size(), table));
    assert(table.points.size() == 3);
    assert(table.areas[0].width == 1920 && table.areas[0].height == 4932);
    assert(table.areas[1].width == 1540 && table.areas[1].height == 1540); // square IoA art
    assert(table.areas[2].height == 2060);
    assert(sw_town_map::ZoneHash("z_wr0301") == 0x87E14B7187BC1CC1ull); // native FNV seed, not standard
    const auto* postwick = sw_town_map::Find(table, sw_town_map::ZoneHash("z_t0101"));
    const auto* armor = sw_town_map::Find(table, sw_town_map::ZoneHash("z_wr0201"));
    const auto* crown = sw_town_map::Find(table, sw_town_map::ZoneHash("z_wr0301"));
    assert(postwick && postwick->area == 0 && postwick->x == 977 && postwick->y == 162);
    assert(armor && armor->area == 1 && armor->x == 790 && armor->y == 484);
    assert(crown && crown->area == 2 && crown->x == 564 && crown->y == 1274);
    auto view = sw_town_map::Follow(table, *postwick);
    assert(view.x0 == 0 && view.x1 == 1920 && view.y0 == 0 && view.y1 == 1410);
    view = sw_town_map::Follow(table, *crown);
    assert(view.y0 == 569 && view.y1 == 1979); // clamped nativebitmap Y-UP, not a second inversion
    assert(!sw_town_map::Find(table, sw_town_map::ZoneHash("unknown_indoor_alias")));
    bytes[4] ^= 0xFF; assert(!sw_town_map::Decode(bytes.data(), bytes.size(), table)); bytes[4] ^= 0xFF;
    assert(!sw_town_map::Decode(bytes.data(), bytes.size() - 1, table));
    PutU32(bytes, 0x14 + 4, 0xFFFFFFF0u);
    assert(!sw_town_map::Decode(bytes.data(), bytes.size(), table)); // no oversized-section read
    std::puts("sw_reader: nativeBFLYT origins/parents/bitmaps map zonecentres correctly, reject malformed geometry");
}

void TestOverworldWeatherPositionAndBikePipeline() {
    FakeHost fake = BuildFixture(false);
    AddLiveParty(fake);
    AddBattleFixture(fake, 0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    fake.main_base = 0x80000000;
    const auto slot = fake.heap_begin + 0xB0000000ull;
    const auto global = slot + 0x1000, manager = slot + 0x2000;
    const auto weather = slot + 0x3000, parameters = slot + 0x4000;
    const auto world_slot = slot + 0x10000, world = world_slot + 0x1000;
    const auto player = slot + 0x100000, vtable = player + 0x1000;
    const auto wrapper = player + 0x2000, component = player + 0x3000, vector = player + 0x4000;
    auto add_pointer = [&](std::uint64_t address, std::uint64_t value) {
        std::vector<unsigned char> bytes(8); PutU64(bytes, 0, value);
        fake.regions.push_back({address, std::move(bytes)});
    };
    add_pointer(fake.main_base + 0x2610848, slot); add_pointer(slot, global);
    std::vector<unsigned char> gl(0x98), man(0x410), we(0x400), pa(0x64);
    PutU64(gl, 0x90, manager); PutU64(man, 0x408, weather); PutU64(we, 0x3F8, parameters);
    PutU32(pa, 0x60, 2); // overworld Rain ->native weather_icon_s_03
    fake.regions.push_back({global, gl}); fake.regions.push_back({manager, man});
    fake.regions.push_back({weather, we}); fake.regions.push_back({parameters, pa});
    add_pointer(fake.main_base + 0x26194D8, world_slot); add_pointer(world_slot, world);
    std::vector<unsigned char> wo(0xC0), ob(0x600), wrap(0x68), comp(0x499), objects(16);
    PutU64(wo, 0xB0, vector); PutU64(wo, 0xB8, vector + 16);
    PutU64(ob, 0x50, vtable); PutU64(ob, 0x588, wrapper);
    std::array<float, 3> xyz{18945.25f, 1724.5f, 53708.75f};
    std::memcpy(ob.data() + 0xB0, xyz.data(), sizeof xyz);
    PutU64(wrap, 0x60, component); comp[0x498] = 0; PutU64(objects, 0, player);
    fake.regions.push_back({world, wo}); fake.regions.push_back({player, ob});
    fake.regions.push_back({wrapper, wrap}); fake.regions.push_back({component, comp});
    fake.regions.push_back({vector, objects}); add_pointer(vtable, fake.main_base + 0xDA2490);
    const auto registry_slot = slot + 0x20000, registry = registry_slot + 0x1000;
    const auto registry_vector = registry_slot + 0x2000, registry_row = registry_slot + 0x3000;
    add_pointer(fake.main_base + 0x2617C48, registry_slot); add_pointer(registry_slot, registry);
    std::vector<unsigned char> reg(0x158), row(0x3A8);
    PutU64(reg, 0x148, registry_vector); PutU64(reg, 0x150, registry_vector + 8);
    PutU64(row, 0x3A0, sw_town_map::ZoneHash("z_wr0301"));
    fake.regions.push_back({registry, reg}); fake.regions.push_back({registry_row, row});
    add_pointer(registry_vector, registry_row);
    fake.dat_files[sw_town_map::kLayoutPath] = NativeMapLayoutFixture();
    const auto host = MakeHostApi(fake);
    sw_reader::Reader reader;
    PrimeWorldEntered(fake, host, reader);
    assert(fake.ints.at("sw.location.weather.ready") == 1);
    assert(fake.ints.at("sw.location.weather.id") == 2);
    assert(fake.texts.at("sw.location.weather.icon_src").find("weather_icon_s_03^t") != std::string::npos);
    assert(reader.bike_ready() && !reader.bike_riding());
    assert(fake.ints.at("sw.bike.state_ready") == 1 && fake.ints.at("sw.bike.riding") == 0);
    assert(fake.ints.at("sw.location.map.ready") == 1 && fake.ints.at("sw.location.map.area_id") == 2);
    assert(fake.floats.at("sw.location.map.x") == 564 && fake.floats.at("sw.location.map.y") == 1274);
    assert(fake.floats.at("sw.location.map.area_h") > 1274);
    for (auto& region : fake.regions) {
        if (region.base == player) {
            xyz[0] -= 70; std::memcpy(region.data.data() + 0xB0, xyz.data(), sizeof xyz);
        }
        if (region.base == component) region.data[0x498] = 1;
        if (region.base == registry_row) PutU64(region.data, 0x3A0, sw_town_map::ZoneHash("z_t0101"));
    }
    fake.ints.clear(); fake.texts.clear(); fake.floats.clear(); reader.Sample(host);
    assert(reader.bike_ready() && reader.bike_riding()); // live movement on very next tick
    assert(fake.ints.at("sw.location.map.area_id") == 0);
    assert(fake.floats.at("sw.location.map.x") == 977 && fake.floats.at("sw.location.map.y") == 162);
    for (auto& region : fake.regions) {
        if (region.base == parameters) PutU32(region.data, 0x60, 9); // invalid enum, never guessed
        if (region.base == component) region.data[0x498] = 2;
    }
    fake.ints.clear(); fake.texts.clear(); fake.floats.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.location.weather.ready") == 0); // weather failure does not block bike/player work
    assert(!reader.bike_ready() && fake.ints.at("sw.bike.state_ready") == 0);
    for (auto& region : fake.regions) if (region.base == player) {
        xyz[0] = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(region.data.data() + 0xB0, xyz.data(), sizeof xyz);
    }
    for (auto& region : fake.regions) if (region.base == component) region.data[0x498] = 0; // valid mount byte
    fake.ints.clear(); fake.texts.clear(); fake.floats.clear(); reader.Sample(host);
    // Position is no longer published (sw.location.position.* was dead -- 0 manifest binds), but
    // the underlying xyz read/validate still gates trusting world_player_ for the bike byte.
    assert(!reader.bike_ready() && fake.ints.at("sw.bike.state_ready") == 0);
    for (auto& region : fake.regions) if (region.base == registry_row)
        PutU64(region.data, 0x3A0, sw_town_map::ZoneHash("unknown_indoor_alias"));
    fake.ints.clear(); fake.texts.clear(); fake.floats.clear(); reader.Sample(host);
    // Door transition: an unresolved interior holds the last outdoor pin (no zoom-out flash).
    assert(fake.ints.at("sw.location.map.ready") == 1 && fake.ints.at("sw.location.map.area_id") == 0);
    assert(fake.floats.at("sw.location.map.x") == 977 && fake.floats.at("sw.location.map.y") == 162);
    const auto interior = sw_town_map::ZoneHash("unknown_indoor_alias");
    reader.SetMapZoneHint(interior, sw_town_map::ZoneHash("z_wr0301"));
    fake.ints.clear(); fake.texts.clear(); fake.floats.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.location.map.ready") == 1 && fake.ints.at("sw.location.map.area_id") == 2);
    // A real zone change invalidates the hint; returning to that interior cannot resurrect it.
    for (auto& region : fake.regions) if (region.base == registry_row)
        PutU64(region.data, 0x3A0, sw_town_map::ZoneHash("z_t0101"));
    reader.Sample(host);
    for (auto& region : fake.regions) if (region.base == registry_row) PutU64(region.data, 0x3A0, interior);
    fake.ints.clear(); fake.texts.clear(); fake.floats.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.location.map.area_id") == 0); // held z_t0101 pin, stale hint unused
    reader.SetMapZoneHint(sw_town_map::ZoneHash("different_interior"), sw_town_map::ZoneHash("z_wr0301"));
    reader.Sample(host); assert(fake.ints.at("sw.location.map.area_id") == 0); // mismatched hint
    reader.SetMapZoneHint(interior, sw_town_map::ZoneHash("z_wr0301"));
    reader.ResetSession(); PrimeWorldEntered(fake, host, reader);
    fake.ints.clear(); fake.texts.clear(); fake.floats.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.location.map.ready") == 0); // configure/reset removes the alias
    SetZoneName(fake, ""); reader.ResetSession();
    fake.ints.clear(); fake.texts.clear(); fake.floats.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.state.title") == 1);
    assert(fake.ints.at("sw.location.weather.ready") == 0);
    assert(!reader.bike_ready());
    std::puts("sw_reader: native overworld roots follow weather/player movement/bike and hide invalid/title data");
}

void TestLivePartyMovesForPpMedicinePicker() {
    FakeHost fake = BuildLivePartyFixture({g_mewtwo});
    fake.personal[150] = {13, 13, 106, 5};
    fake.waza_files[1] = std::vector<unsigned char>(std::begin(kSyntheticElectricSpecialMove),
                                                 std::end(kSyntheticElectricSpecialMove));
    fake.dat_files[sw_move_names::WazaNameDatPath()] =
        std::vector<unsigned char>(std::begin(kWazaNameFixture), std::end(kWazaNameFixture));
    auto replace_move = [&](unsigned pp, unsigned pp_ups, unsigned status = 0) {
        std::vector<unsigned char> blob(std::begin(g_mewtwo), std::end(g_mewtwo));
        sw_pk8::DecryptInPlace(blob.data(), blob.size());
        PutU32(blob, 0, 0); // shuffle0 is identity, so encoding is only the LCG cipher below
        for (int i = 0; i < 4; ++i) {
            PutU16(blob, 0x72 + 2 * i, i == 0 ? 1 : 0);
            blob[0x7A + i] = i == 0 ? pp : 0;
            blob[0x7E + i] = i == 0 ? pp_ups : 0;
        }
        PutU32(blob, 0x94, status);
        PutU16(blob, 6, sw_pk8::ComputeChecksum16(blob.data()));
        sw_pk8::CryptArrayU16(blob.data() + 8, blob.size() - 8, 0);
        auto* region = const_cast<FakeRegion*>(fake.Find(fake.heap_begin + 0x90000000ull, 1));
        assert(region);
        std::memcpy(region->data.data() + 0xD0, blob.data(), blob.size());
    };
    replace_move(3, 2); // base40 PP, two PP Ups ->56 max
    const auto host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);
    assert(fake.ints.at("sw.party.0.move1.ready") == 1);
    assert(fake.ints.at("sw.party.0.move1.id") == 1);
    assert(fake.ints.at("sw.party.0.move1.pp") == 3);
    assert(fake.ints.at("sw.party.0.move1.pp_max") == 56);
    assert(fake.ints.at("sw.party.0.move1.needs_pp") == 1);
    assert(fake.texts.at("sw.party.0.move1.pp_text") == "3/56");
    assert(fake.ints.at("sw.party.0.move1.name_ready") == 1);
    assert(fake.texts.at("sw.party.0.move1.name") == "Bulbasaur"); // fixture line1, not real move text
    assert(fake.ints.at("sw.party.0.move2.ready") == 0);

    replace_move(56, 2); // direct PP medicine effect, no save needed to refresh
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.party.0.move1.needs_pp") == 0);
    assert(fake.texts.at("sw.party.0.move1.pp_text") == "56/56");
    replace_move(57, 2); // impossible current PP must not produce a selectable move
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.party.0.move1.ready") == 0);
    replace_move(10, 4); // invalid PP Up count also fails closed
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.party.0.move1.ready") == 0);
    // Sword stores a sequential ailment enum, unlike the old bitmask assumption. Check the
    // actual stored-blob -> Reader -> native badge path for every independently proven value.
    const char* ailments[] = {"", "mahi", "nemuri", "koori", "yakedo", "doku"};
    for (unsigned status = 1; status <= 5; ++status) {
        replace_move(10, 2, status);
        fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
        assert(fake.ints.at("sw.party.0.status_ready") == 1);
        assert(fake.texts.at("sw.party.0.status_src").find(std::string("iconSick_") + ailments[status]) !=
               std::string::npos);
    }
    for (unsigned unknown : {6u, 8u, 0x80u, 0x105u}) {
        replace_move(10, 2, unknown);
        fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
        assert(fake.ints.at("sw.party.0.status_ready") == 0);
        assert(fake.texts.find("sw.party.0.status_src") == fake.texts.end());
    }
    assert(sw_status_icons::FromStatus(1, 0, 356) == sw_status_icons::Code::kFainted);
    assert(sw_status_icons::FromStatus(1, 0, 0) == sw_status_icons::Code::kParalysis);
    std::puts("sw_reader: live party move/PP picker uses stored PK8 PP and PP Ups, updates immediately");
}

// Synthetic native-format message table for Field plumbing tests. The message decoder is
// already checked against real game-file bytes elsewhere; this fixture supplies chosen rows
// without bundling another complete extracted table.
std::vector<unsigned char> GameMessageFixture(const std::vector<std::string>& lines) {
    std::size_t bytes = 16 + 4 + 8 * lines.size();
    for (const auto& line : lines) bytes += (line.size() + 1) * 2;
    std::vector<unsigned char> out(bytes);
    PutU16(out, 0, 1); PutU16(out, 2, lines.size());
    PutU32(out, 4, bytes - 16); PutU32(out, 12, 16); PutU32(out, 16, bytes - 16);
    std::size_t offset = 4 + 8 * lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& text = lines[i];
        PutU32(out, 20 + i * 8, offset); PutU16(out, 20 + i * 8 + 4, text.size() + 1);
        std::uint16_t key = static_cast<std::uint16_t>(sw_text::kBaseKey + i * sw_text::kAdvanceKey);
        for (std::size_t j = 0; j <= text.size(); ++j) {
            const std::uint16_t value = j == text.size() ? 0 : static_cast<unsigned char>(text[j]);
            PutU16(out, 16 + offset + j * 2, value ^ key);
            key = sw_text::RotateLeft3(key);
        }
        offset += (text.size() + 1) * 2;
    }
    return out;
}

std::vector<unsigned char> FieldMessageFixture() {
    std::vector<std::string> lines(166);
    for (const auto& [state, name] : std::vector<std::pair<int, std::string>>{
             {2, "Rain"}, {5, "Electric Terrain"}, {8, "Trick Room"}, {45, "Reflect"}}) {
        lines[state * 2] = name;
        lines[state * 2 + 1] = "Native field description";
    }
    lines[2 * 2 + 1] = "Native\\nweather description";
    return GameMessageFixture(lines);
}

// Companion language (sw_lang.h): a change re-reads the message tables from the new language's
// folder and republishes every change-driven (taped) domain, with no game-state change at all.
void TestLanguageChangeReloadsAndRepublishes() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    FakeHost live = BuildLivePartyFixture({g_mewtwo, g_eternatus});
    for (auto& r : live.regions) fake.regions.push_back(r);
    fake.personal[150] = {13, 13, 106, 5};
    fake.personal[890] = {9, 15, 140, 5};
    std::vector<std::string> en(891, "x"), fr(891, "x");
    en[150] = "Mewtwo"; en[890] = "Eternatus";
    fr[150] = "Mewtwo"; fr[890] = "Ethernatos";
    fake.dat_files[sw_lang::MessagePath(sw_lang::Lang::English, "monsname.dat")] = GameMessageFixture(en);
    fake.dat_files[sw_lang::MessagePath(sw_lang::Lang::French, "monsname.dat")] = GameMessageFixture(fr);
    // the French level prefix is the game's own line (btl_app.dat 7), loaded with the language
    std::vector<std::string> btl(8, "x");
    btl[7] = "N.";
    fake.dat_files[sw_lang::MessagePath(sw_lang::Lang::French, "btl_app.dat")] = GameMessageFixture(btl);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_str::UnloadGameForTest();
    sw_str::LoadGame(host, sw_lang::Lang::French);
    sw_reader::Reader reader;
    assert(!reader.SetLanguage(sw_lang::Lang::English)); // first value: nothing to drop yet
    reader.Sample(host);
    assert(fake.texts.at("sw.party.1.name") == "Eternatus");
    assert(fake.texts.at("sw.party.0.level_text") == sw_str::Level(100));
    reader.Sample(host); // unchanged tick: replayed from the tapes
    assert(!reader.SetLanguage(sw_lang::Lang::English));
    assert(reader.SetLanguage(sw_lang::Lang::French));
    fake.texts.clear();
    reader.Sample(host);
    assert(fake.texts.at("sw.party.1.name") == "Ethernatos");
    assert(fake.texts.at("sw.party.0.level_text") == "N.100");
    assert(reader.SetLanguage(sw_lang::Lang::English));
    reader.Sample(host);
    assert(fake.texts.at("sw.party.1.name") == "Eternatus");
    sw_str::UnloadGameForTest();
    std::puts("sw_reader: a language change reloads the message tables and republishes");
}

void TestDmaxPreviewPreservesNativeActionIdentity() {
    FakeHost fake = BuildFixture(false);
    AddLiveParty(fake); SetZoneName(fake, "Wyndon");
    AddBattleFixture(fake, 0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    const auto client = fake.heap_begin + 0xA0000000ull, main = client + 0x1000, party = client + 0x2000;
    const auto own_base = fake.heap_begin + sw_addresses::Find("battle.own_mon")->sysbot_offset;
    const auto actor = own_base + 0x18910; // native party/query uses PassB; display reads PassA
    std::vector<unsigned char> cl(0x350), ma(0x200);
    std::vector<unsigned char> pt(0x40);
    PutU64(cl, 0x58, main); PutU32(ma, 0x150, 0);
    PutU64(cl, 0x340, party); PutU64(pt, 0, actor); pt[0x30] = 1;
    fake.regions.push_back({client, std::move(cl)}); fake.regions.push_back({main, std::move(ma)});
    fake.regions.push_back({party, std::move(pt)});
    fake.regions.push_back({actor, std::vector<unsigned char>(sw_battle::kStructBytes)});
    fake.personal[25] = {12, 12};
    fake.dat_files[sw_move_names::WazaNameDatPath()] = GameMessageFixture({"None", "Base fixture", "Max fixture", "Guard fixture"});
    auto max_move = fake.waza_files[1]; max_move[30] = 9; // Fire; deliberately distinct display metadata
    fake.waza_files[2] = max_move;
    auto guard = max_move; guard[30] = 0; guard[31] = 0; guard[32] = 0;
    fake.waza_files[3] = guard;
    std::vector<std::string> gnames(33), ginfo(33);
    gnames[0] = "Gigantic fixture"; ginfo[0] = "Description\nline two";
    fake.dat_files["romfs:/bin/message/English/common/gwazaname.dat"] = GameMessageFixture(gnames);
    fake.dat_files["romfs:/bin/message/English/common/gwazainfo.dat"] = GameMessageFixture(ginfo);
    const auto host = MakeHostApi(fake);
    sw_reader::Reader reader; PrimeWorldEntered(fake, host, reader);
    SetCurrentScreen(fake, 0xFFFFFFFFu); reader.SetBattleHint(true, true, client); reader.Sample(host);
    assert(fake.texts.at("sw.battle.own.move2.name") == "Base fixture");
    assert(reader.battle_own_actor() == actor);
    auto* native_actor = const_cast<FakeRegion*>(fake.Find(actor, sw_battle::kStructBytes));
    assert(native_actor && !reader.battle_dmax_active());
    native_actor->data[0x298] = 1; reader.Sample(host);
    assert(reader.battle_dmax_active());
    native_actor->data[0x298] = 0; reader.Sample(host);
    assert(!reader.battle_dmax_active());
    native_actor->data[0x298] = 2; reader.Sample(host);
    assert(!reader.battle_dmax_active());
    native_actor->data.resize(0x298); reader.Sample(host); // actual native flag unreadable
    assert(!reader.battle_dmax_active());
    native_actor->data.resize(sw_battle::kStructBytes);
    native_actor->data[0x298] = 0;
    const std::array<std::uint32_t, 4> base{999, 1, 50, 0}, ids{0, 2, 0, 0}, powers{0, 130, 0, 0};
    const std::array<std::int32_t, 4> no_gmax{-1, -1, -1, -1};
    reader.SetDmaxPreview(true, ids, powers, no_gmax, base);
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.battle.own.move2.preview_ready") == 1);
    assert(fake.texts.at("sw.battle.own.move2.name") == "Max fixture");
    assert(fake.ints.at("sw.battle.own.move2.type_id") == 9);
    assert(fake.ints.at("sw.battle.own.move2.power_ready") == 1 && fake.ints.at("sw.battle.own.move2.power") == 130);
    assert(fake.texts.at("sw.battle.own.move2.pp_text") == "3/16");
    assert(reader.battle_view().move_id[1] == 1 && reader.battle_view().pp[1] == 3);
    assert(fake.ints.at("sw.battle.own.move2.base_id") == 1 && fake.ints.at("sw.battle.own.move2.display_id") == 2);
    assert(fake.texts.at("sw.battle.own.move2.effectiveness_multiplier") == "\xc3\x97\x31"); // Fire vs Electric
    reader.SetDmaxPreview(true, ids, powers, no_gmax, base, actor + sw_battle::kStructBytes);
    reader.Sample(host); // source IDs match; the query's actor identity does not
    assert(fake.ints.at("sw.battle.own.move2.preview_ready") == 0);
    assert(fake.ints.at("sw.battle.own.move2.name_ready") == 0);
    assert(reader.battle_view().move_id[1] == 1);
    auto guard_ids = ids; guard_ids[1] = 3;
    reader.SetDmaxPreview(true, guard_ids, powers, no_gmax, base);
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.texts.at("sw.battle.own.move2.name") == "Guard fixture");
    assert(fake.ints.at("sw.battle.own.move2.type_id") == 0);
    assert(fake.ints.at("sw.battle.own.move2.effectiveness_ready") == 0 && fake.ints.at("sw.battle.own.move2.power_ready") == 0);
    auto special = no_gmax; special[1] = 0; // native index0 is a real G-Max message, not moveID0
    reader.SetDmaxPreview(true, ids, powers, special, base);
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.texts.at("sw.battle.own.move2.name") == "Gigantic fixture");
    assert(fake.ints.at("sw.battle.own.move2.power_ready") == 0); // generic power is not G-Max power proof
    assert(fake.texts.at("sw.battle.own.move2.description") == "Description line two");
    // The host keeps published values across samples. A failed own read must clear all
    // independently bound metadata gates, including the raid header's matchup widgets.
    auto* own_region = const_cast<FakeRegion*>(fake.Find(own_base, sw_battle::kStructBytes));
    assert(own_region);
    PutU16(own_region->data, 0x74, 999); reader.Sample(host);
    assert(reader.battle_own_actor() == 0);
    for (const auto* suffix : {"ready", "name_ready", "type_ready", "effectiveness_ready",
                               "preview_ready", "power_ready", "description_ready"})
        assert(fake.ints.at(std::string("sw.battle.own.move2.") + suffix) == 0);
    PutU16(own_region->data, 0x74, 200); reader.Sample(host);
    assert(reader.battle_own_actor() == actor);
    assert(fake.ints.at("sw.battle.own.move2.description_ready") == 1);
    PutU16(own_region->data, 0x2D4 + 0x1C, 0); reader.Sample(host);
    for (const auto* suffix : {"ready", "name_ready", "type_ready", "effectiveness_ready",
                               "preview_ready", "power_ready", "description_ready"})
        assert(fake.ints.at(std::string("sw.battle.own.move2.") + suffix) == 0);
    PutU16(own_region->data, 0x2D4 + 0x1C, 1);
    auto* party_region = const_cast<FakeRegion*>(fake.Find(party, 0x40));
    assert(party_region);
    native_actor->data[0x298] = 1;
    party_region->data[0x30] = 0; reader.Sample(host);
    assert(reader.battle_own_actor() == 0);
    assert(!reader.battle_dmax_active());
    assert(fake.ints.at("sw.battle.own.move2.preview_ready") == 0);
    party_region->data[0x30] = 1;
    FieldRan(reader); // the guest runs the field again (battle over, not a stall)
    reader.SetBattleHint(false, true, 0); SetCurrentScreen(fake, 0xFFFF5127u); reader.Sample(host);
    assert(!reader.battle_dmax_active() && reader.battle_own_actor() == 0);
    reader.SetBattleHint(true, true, client); SetCurrentScreen(fake, 0xFFFFFFFFu);
    native_actor->data[0x298] = 0;
    auto changed = base; changed[1] = 85;
    reader.SetDmaxPreview(true, ids, powers, no_gmax, changed);
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.battle.own.move2.preview_ready") == 0);
    assert(fake.ints.at("sw.battle.own.move2.name_ready") == 0 && fake.ints.at("sw.battle.own.move2.type_ready") == 0);
    assert(fake.texts.at("sw.battle.own.move2.pp_text") == "3/16" && reader.battle_view().move_id[1] == 1);
    reader.SetDmaxPreview(false);
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.texts.at("sw.battle.own.move2.name") == "Base fixture" && fake.ints.at("sw.battle.own.move2.type_id") == 12);
    reader.SetDmaxPreview(true, ids, powers, no_gmax, base); reader.ResetSession();
    PrimeWorldEntered(fake, host, reader); SetCurrentScreen(fake, 0xFFFFFFFFu);
    reader.SetBattleHint(true, true, client);
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.battle.own.move2.preview_ready") == 0);
    assert(fake.texts.at("sw.battle.own.move2.name") == "Base fixture");
    assert(!reader.battle_dmax_active() && !reader.battle_raid_ready());
    std::puts("sw_reader: native Max/G-Max previews preserve base action IDs/PP, reject stale IDs, and clear on reset");
}

void TestBattleFieldPointerAndPublishPipeline() {
    FakeHost fake = BuildFixture(false);
    AddLiveParty(fake);
    AddBattleFixture(fake, 0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    const auto client = fake.heap_begin + 0xA0000000ull;
    const auto field = client + 0x1000, pass = client + 0x2000, main = client + 0x3000;
    const auto manager = client + 0x4000, condition = client + 0x5000;
    std::vector<unsigned char> cl(0x400), raw(sw_battle_field::kBytes), pa(0x100), ma(0x200);
    std::vector<unsigned char> slots(sw_battle_field::kSideTableBytes), record(0x20);
    PutU64(cl, 0xE0, field); PutU64(cl, 0x70, pass); PutU64(cl, 0x58, main);
    cl[0x2A6] = 0; // local participant0 maps to side0 by the native singles table
    raw[0] = 2; PutU32(raw, 4, 5); PutU32(raw, 0xC, 1); // Rain, four turns left
    raw[0x11] = 3; PutU64(raw, 0x58, 0x502); PutU32(raw, 0x88, 2); // Electric Terrain
    PutU32(raw, 0x180 + 4, 1); PutU64(raw, 0x18 + 8, 0x502); PutU32(raw, 0x68 + 4, 1);
    PutU64(pa, 0x48, manager); PutU32(ma, 0x150, 0);
    PutU64(slots, 8, condition); // side0 key0 Reflect shared-pointer wrapper
    PutU32(record, 0x10, 0x502); PutU32(record, 0x18, 1); PutU32(record, 0x1C, 1);
    fake.regions.push_back({client, cl}); fake.regions.push_back({field, raw});
    fake.regions.push_back({pass, pa}); fake.regions.push_back({main, ma});
    fake.regions.push_back({manager, slots}); fake.regions.push_back({condition, record});
    fake.dat_files[sw_battle_field::StateTextPath()] = FieldMessageFixture();
    const auto host = MakeHostApi(fake);
    sw_reader::Reader reader;
    PrimeWorldEntered(fake, host, reader);
    reader.SetBattleHint(true, true, client);
    reader.Sample(host);
    if (!sw_battle_field::kRuntimeVerified) {
        assert(fake.ints.at("sw.battle.field.ready") == 0);
        assert(fake.ints.at("sw.battle.field.row_count") == 0);
        return;
    }
    assert(fake.ints.at("sw.battle.field.ready") == 1);
    assert(fake.texts.at("sw.battle.field.weather.name") == "Rain");
    assert(fake.texts.at("sw.battle.field.weather.description") == "Native weather description");
    assert(fake.texts.at("sw.battle.field.weather.turns_text") == "4 turns");
    assert(fake.texts.at("sw.battle.field.terrain.name") == "Electric Terrain");
    assert(fake.texts.at("sw.battle.field.row.0.name") == "Rain");
    assert(fake.texts.at("sw.battle.field.row.1.name") == "Electric Terrain");
    assert(fake.texts.at("sw.battle.field.row.2.name") == "Trick Room");
    assert(fake.texts.at("sw.battle.field.side.0.effect.0.name") == "Reflect");
    if (sw_battle_field::kSideIdentityVerified) {
        assert(fake.ints.at("sw.battle.field.own_side") == 0);
        assert(fake.ints.at("sw.battle.field.row_count") == 4);
        assert(fake.texts.at("sw.battle.field.row.3.name") == "Your side: Reflect");
    } else assert(fake.ints.at("sw.battle.field.row_count") == 3);

    // Ended effects update on the next sample; no previous-battle values survive a missing root.
    for (auto& region : fake.regions) if (region.base == field) {
        region.data[0] = 0; region.data[0x11] = 0; PutU32(region.data, 0x180 + 4, 0);
    }
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.texts.at("sw.battle.field.row.0.name") == "No weather");
    assert(fake.texts.at("sw.battle.field.row.1.name") == "No terrain");
    for (auto& region : fake.regions) if (region.base == client) PutU64(region.data, 0xE0, 0);
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.battle.field.ready") == 0);
    assert(fake.ints.at("sw.battle.field.row_count") == 0);
    assert(fake.texts.find("sw.battle.field.row.0.name") == fake.texts.end());
    std::puts("sw_reader: battle Field follows native client/field/side roots and clears unavailable data");
}

void TestBattleFieldDecodeAndNativeEnumMap() {
    using namespace sw_battle_field;
    std::vector<unsigned char> raw(kBytes);
    raw[0] = 3; // exact native enum: Hail, not Sandstorm
    PutU32(raw, 4, 5); PutU32(raw, 0xC, 2);
    raw[0x11] = 3; // exact native terrain enum: Electric, not Misty
    PutU64(raw, 0x58, 2 | (8ull << 8)); PutU32(raw, 0x88, 2);
    PutU32(raw, 0x180 + 4, 1); // Trick Room
    PutU64(raw, 0x18 + 8, 4 | (5ull << 8)); PutU32(raw, 0x68 + 4, 1);
    PutU32(raw, 0x180 + 4 * 3, 1); // Imprison is presence-only
    PutU64(raw, 0x18 + 8 * 3, 4 | (5ull << 8));
    auto field = Decode(raw.data(), raw.size());
    assert(field.valid && field.weather == 3 && field.terrain == 3);
    assert(WeatherTextState(field.weather) == 4);
    assert(TerrainTextState(field.terrain) == 5);
    assert(field.weather_timer.known && field.weather_timer.remaining == 3);
    assert(field.terrain_timer.known && field.terrain_timer.remaining == 6);
    assert(field.effects[0] && field.effect_timers[0].remaining == 4);
    assert(field.effects[2] && !field.effect_timers[2].known);
    assert(!field.effects[1]);
    PutU32(raw, 4, 255); PutU32(raw, 0xC, 300);
    field = Decode(raw.data(), raw.size());
    assert(field.weather_timer.known && field.weather_timer.indefinite);
    assert(!SourceTimer(1 | (8ull << 8), 0).known); // unrecognised duration kind
    assert(!SourceTimer(2, 0).known); // source has no finite duration
    assert(SourceTimer(2 | (5ull << 8), 9).remaining == 0); // no underflow
    assert(!Decode(raw.data(), raw.size() - 1).valid);
    raw[0] = 8; assert(!Decode(raw.data(), raw.size()).valid);
    raw[0] = 0; raw[0x11] = 5; assert(!Decode(raw.data(), raw.size()).valid);
    const int weather_states[] = {0, 1, 2, 4, 3, 70, 69, 71};
    for (unsigned i = 0; i < 8; ++i) assert(WeatherTextState(i) == weather_states[i]);
    assert(WeatherTextState(8) == 0 && TerrainTextState(5) == 0);
    assert(SideForParticipant(0, 0, 0) == 0 && SideForParticipant(0, 0, 1) == 1);
    assert(SideForParticipant(0, 0, 2) == -1 && SideForParticipant(0, 0, 5) == -1);
    assert(SideForParticipant(1, 2, 2) == 0 && SideForParticipant(1, 2, 3) == 1);
    assert(SideForParticipant(1, 4, 2) == 0 && SideForParticipant(1, 4, 3) == 1);
    assert(SideForParticipant(1, 5, 3) == -1);
    assert(SideForParticipant(2, 0, 3) == 0 && SideForParticipant(2, 0, 4) == 1);
    assert(SideForParticipant(2, 3, 1) == -1 && SideForParticipant(2, 3, 4) == 1);
    assert(SideForParticipant(2, 8, 0) == -1 && SideForParticipant(3, 0, 0) == -1);

    std::vector<unsigned char> record(0x20);
    PutU32(record, 0x1C, 3); // Spikes layers, not three turns
    PutU32(record, 0x10, 8 << 8); PutU32(record, 0x18, 3);
    auto effect = DecodeSideEffect(record.data(), record.size(), 3);
    assert(effect.active && effect.layers == 3 && !effect.timer.known);
    effect = DecodeSideEffect(record.data(), record.size(), 4);
    assert(effect.active && effect.timer.known && effect.timer.remaining == 5);
    PutU32(record, 0x1C, 0);
    effect = DecodeSideEffect(record.data(), record.size(), 4);
    assert(!effect.active && !effect.timer.known);
    assert(!DecodeSideEffect(record.data(), 0x1F, 4).active);

    std::vector<std::string> strings(166);
    strings[4 * 2] = "Hail"; strings[4 * 2 + 1] = "Native description";
    std::string name, description;
    assert(StateStrings(strings, WeatherTextState(3), name, description));
    assert(name == "Hail" && description == "Native description");
    assert(!StateStrings(strings, 0, name, description));
    assert(!StateStrings(strings, 83, name, description));
    std::puts("sw_reader: native battle Field enums, timed effects and side layers decode without guesses");
}

// Review L9: a player's discovered species must not reveal how many unknown stages remain.
// Exercise the actual Reader publish path, including the throttled/replayed dex domain and
// dynamically unlocked DLC region access. Small synthetic personal/dex blocks use the same
// byte layout as the separately tested real fixtures.
void TestDexDiscoveryDoesNotRevealUnknownStagesOrDlcTotals() {
    FakeHost fake = BuildFixture(false);
    auto& personal = fake.dat_files[sw_dex_index::kPersonalTablePath];
    personal.resize((sw_dex_index::kMaxKnownSpecies + 1) * sw_dex_index::kRecordSize);
    const int species[] = {220, 221, 473};
    for (int i = 0; i < 3; ++i) {
        const std::size_t record = species[i] * sw_dex_index::kRecordSize;
        PutU16(personal, record + sw_dex_index::kGalarOfs, i + 1);
        PutU16(personal, record + sw_dex_index::kArmorOfs, i + 1);
        PutU16(personal, record + sw_dex_index::kCrownOfs, i + 1);
    }
    for (const char* anchor : {"dex.galar", "dex.isle_of_armor", "dex.crown_tundra"}) {
        const auto* a = sw_addresses::Find(anchor);
        std::vector<unsigned char> raw(a->size, 0);
        if (std::strcmp(anchor, "dex.galar") == 0) {
            raw[0] = 1; // Swinub known
            raw[2 * sw_zukan::kEntrySize] = 1; // Mamoswine known, Piloswine unknown
        }
        fake.regions.push_back({fake.heap_begin + a->sysbot_offset, std::move(raw)});
    }
    auto& swinub = fake.dat_files["romfs:/bin/pml/evolution/evo_220.bin"];
    swinub.resize(sw_evolution::kFileSize);
    PutU16(swinub, 0, 4); PutU16(swinub, 4, 221); swinub[7] = 33;
    auto& piloswine = fake.dat_files["romfs:/bin/pml/evolution/evo_221.bin"];
    piloswine.resize(sw_evolution::kFileSize);
    PutU16(piloswine, 0, 21); PutU16(piloswine, 2, 246); PutU16(piloswine, 4, 473);

    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    for (int i = 0; i < 25; ++i) reader.Sample(host); // evolution table finishes loading
    assert(fake.ints.at("sw.dex.detail.ready") == 1);
    assert(fake.ints.at("sw.dex.detail.evo_count") == 2); // only the two discovered endpoints
    assert(fake.ints.at("sw.dex.detail.evo.0.seen") == 1);
    assert(fake.ints.at("sw.dex.detail.evo.1.seen") == 1);
    assert(fake.ints.at("sw.dex.detail.evo.1.link_ready") == 0); // no false direct evolution
    assert(fake.ints.at("sw.dex.detail.evo.1.method_ready") == 0);
    assert(fake.ints.at("sw.dex.detail.evo.2.ready") == 0); // no silhouette for unknown stage
    // Detail tabs: a tab change re-publishes even inside the slow-refresh replay window.
    assert(fake.ints.at("sw.dex.detail.tab0") == 1);
    assert(reader.DispatchAction("dex_detail_tab", 2) && !reader.DispatchAction("dex_detail_tab", 3));
    reader.Sample(host);
    assert(fake.ints.at("sw.dex.detail.tab2") == 1 && fake.ints.at("sw.dex.detail.tab0") == 0);
    assert(fake.ints.at("sw.dex.region.1.available") == 0);
    assert(fake.ints.at("sw.dex.region.2.available") == 0);
    assert(fake.ints.find("sw.dex.region.1.total") == fake.ints.end());
    assert(fake.ints.find("sw.dex.region.2.caught") == fake.ints.end());
    assert(!reader.DispatchAction("dex_region", 1));
    assert(!reader.DispatchAction("dex_region", 2));

    reader.SetDexRegionAccess(true, false); // successful native discovery proves Armor access
    fake.ints.clear(); fake.texts.clear();
    reader.Sample(host);
    assert(fake.ints.at("sw.dex.region.1.available") == 1);
    assert(fake.ints.at("sw.dex.region.1.total") == sw_dex_index::kArmorCount);
    assert(fake.ints.at("sw.dex.region.1.seen") == 2); // uses real Galar-home resolution
    assert(fake.ints.find("sw.dex.region.2.total") == fake.ints.end());
    assert(reader.DispatchAction("dex_region", 1));
    reader.Sample(host);
    assert(fake.ints.at("sw.dex.sel_region") == 1);

    // A later discovery re-renders immediately, including a real adjacent method; an absent
    // later species contributes no slot/count, and an identical next tick replays no leak.
    for (auto& r : fake.regions)
        if (r.base == fake.heap_begin + sw_addresses::Find("dex.galar")->sysbot_offset) {
            r.data[sw_zukan::kEntrySize] = 1;
            r.data[2 * sw_zukan::kEntrySize] = 0;
        }
    assert(reader.DispatchAction("dex_region", 0));
    fake.ints.clear(); fake.texts.clear();
    reader.Sample(host);
    assert(fake.ints.at("sw.dex.detail.evo_count") == 2);
    assert(fake.ints.at("sw.dex.detail.evo.1.link_ready") == 1);
    assert(fake.ints.at("sw.dex.detail.evo.1.method_ready") == 1);
    assert(fake.texts.at("sw.dex.detail.evo.1.method_text") == "Lv. 33");
    assert(fake.ints.at("sw.dex.detail.evo.2.ready") == 0);
    const auto ints = fake.ints; const auto texts = fake.texts;
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints == ints && fake.texts == texts);

    reader.ResetSession(); // no DLC access carried into a different save/boot
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.dex.sel_region") == 0);
    assert(fake.ints.at("sw.dex.region.1.available") == 0);
    assert(fake.ints.find("sw.dex.region.1.total") == fake.ints.end());
    // Branch filtering preserves a seen selected member at the end of Eevee's eight
    // branches, and never draws an evolution arrow between sibling branches.
    sw_evolution::Table table;
    const int branches[] = {134, 135, 136, 196, 197, 470, 471, 700};
    std::vector<sw_evolution::Edge> edges;
    for (int species_id : branches)
        edges.push_back({6, 0, static_cast<std::uint16_t>(species_id), 0, 0});
    sw_evolution::AddToTable(table, 133, edges);
    auto known = sw_evolution::KnownChain(table, 700, [](int species_id) { return species_id == 133; });
    assert(known.size() == 2 && known[0].species == 133 && known[1].species == 700);
    assert(sw_evolution::ConsecutiveEvolution(table, known, 1));
    known = sw_evolution::KnownChain(table, 700, [](int) { return true; });
    assert(known.size() == 9 && known[0].species == 133 && known[1].species == 700);
    assert(sw_evolution::ConsecutiveEvolution(table, known, 1));
    assert(!sw_evolution::ConsecutiveEvolution(table, known, 2));
    assert(sw_evolution::KnownParent(table, known, 2)); // sibling method still refers to known Eevee
    for (int selected : {470, 471, 700}) {
        const auto row = sw_evolution::KnownChain(table, selected, [](int) { return true; });
        assert(row.size() == 9 && row[0].species == 133 && row[1].species == selected);
        for (int sibling : branches)
            assert(std::any_of(row.begin(), row.end(), [&](const auto& member) { return member.species == sibling; }));
        assert(sw_evolution::ConsecutiveEvolution(table, row, 1));
        assert(sw_evolution::KnownParent(table, row, 1));
        assert(!sw_evolution::ConsecutiveEvolution(table, row, 2));
    }
    std::puts("sw_reader: unknown evolution stages/counts and undiscovered DLC dex totals stay hidden");
}

void TestNativeRaidRosterAndPublishPipeline() {
    FakeHost fake = BuildFixture(false);
    AddLiveParty(fake); SetZoneName(fake, "Wyndon");
    AddBattleFixture(fake, 0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    constexpr std::uint64_t client = 0x40000000, main = 0x40002000, setup = 0x40004000;
    constexpr std::uint64_t pass = 0x40006000, roster = 0x40008000, first_mon = 0x40010000;
    constexpr std::uint64_t boss_extension = first_mon + 0x5000, shield_address = first_mon + 0x6000;
    std::vector<unsigned char> cl(0x350), root(0x200), settings(0x400), container(0x80);
    std::vector<unsigned char> table(sw_battle_raid::kRosterBytes);
    PutU64(cl, 0x58, main); PutU64(cl, 0x70, pass); cl[0x2A6] = 0;
    PutU64(root, 0x88, setup); PutU32(root, 0x150, 2);
    settings[0x3B9] = 5;
    PutU64(container, 8, roster); PutU64(table, 0, main);
    // These public species/HP/pokeID values reproduce the first real Den42 raid's roster.
    // The synthetic addresses are deliberately unrelated to the old heap-relative bpp anchor.
    const unsigned species[] = {25, 631, 135, 324, 211};
    const unsigned hp[] = {211, 199, 171, 178, 357};
    const unsigned ids[] = {0, 12, 6, 18, 24}; // participant order is NOT pokeID order
    for (int i = 0; i < 5; ++i) {
        const auto pointer = first_mon + i * 0x1000;
        PutU64(table, 8 + i * 0x38, pointer); table[8 + i * 0x38 + 0x30] = 1;
        PutU64(table, 0x120 + ids[i] * 8, pointer);
        std::vector<unsigned char> mon(sw_battle::kStructBytes);
        PutU16(mon, 0x70, species[i]); PutU16(mon, 0x72, hp[i]); PutU16(mon, 0x74, hp[i]);
        mon[0x7C] = i == 0 ? 100 : i == 4 ? 39 : 70; mon[0x7D] = ids[i];
        mon[0x273] = i == 4 ? 1 : 0; mon[0x298] = i == 4 ? 1 : 0;
        if (i == 4) PutU64(mon, 0x278, boss_extension);
        PutU16(mon, 0x2D4, 85); mon[0x2D8] = 15; mon[0x2D9] = 15;
        fake.regions.push_back({pointer, std::move(mon)});
    }
    fake.regions.push_back({client, std::move(cl)}); fake.regions.push_back({main, std::move(root)});
    fake.regions.push_back({setup, std::move(settings)}); fake.regions.push_back({pass, std::move(container)});
    fake.regions.push_back({roster, std::move(table)});
    std::vector<unsigned char> extension(0x10), shield(6);
    PutU64(extension, 0, shield_address); shield[1] = 2; shield[2] = 2; shield[3] = 2;
    fake.regions.push_back({boss_extension, std::move(extension)});
    fake.regions.push_back({shield_address, std::move(shield)});
    fake.personal[25] = {12, 12}; fake.personal[211] = {10, 3};
    const auto read = [&](std::uint64_t address, void* data, std::size_t bytes) {
        return FakeReadMemory(&fake, address, data, bytes) != 0;
    };
    auto raid = sw_battle_raid::Read(client, read);
    assert(raid.kind_known && raid.active && raid.ready);
    assert(raid.participants[1].mon.species == 631 && raid.participants[1].poke_id == 12);
    assert(raid.participants[2].mon.species == 135 && raid.participants[2].poke_id == 6);
    assert(raid.participants[4].boss && raid.participants[4].dmax);
    assert(raid.shield.valid && !raid.shield.active && raid.shield.total == 2 && raid.shield.remaining == 2);
    auto* boss = const_cast<FakeRegion*>(fake.Find(first_mon + 4 * 0x1000, 1));
    auto* ally = const_cast<FakeRegion*>(fake.Find(first_mon + 0x1000, 1));
    auto* own = const_cast<FakeRegion*>(fake.Find(first_mon, 1));
    auto* cfg = const_cast<FakeRegion*>(fake.Find(setup, 1));
    auto* roster_region = const_cast<FakeRegion*>(fake.Find(roster, 1));
    auto* barrier = const_cast<FakeRegion*>(fake.Find(shield_address, 1));
    assert(boss && ally && own && cfg && roster_region && barrier);
    PutU16(ally->data, 0x70, 211); PutU16(ally->data, 0x72, 171); PutU16(ally->data, 0x74, 171);
    raid = sw_battle_raid::Read(client, read);
    assert(raid.ready && raid.participants[1].mon.species == raid.participants[4].mon.species);
    assert(!raid.participants[1].boss && raid.participants[4].boss);
    assert(raid.participants[1].address != raid.participants[4].address); // second boot duplicate species
    PutU16(ally->data, 0x70, 631); PutU16(ally->data, 0x72, 199);
    PutU16(boss->data, 0x74, 100); PutU16(ally->data, 0x74, 0);
    own->data[0x298] = 1; own->data[0x299] = 2;
    barrier->data[0] = 1; barrier->data[2] = 1;
    raid = sw_battle_raid::Read(client, read);
    assert(raid.ready && raid.participants[4].mon.cur_hp == 100 && raid.participants[1].mon.cur_hp == 0);
    assert(raid.participants[0].dmax && raid.participants[0].dmax_elapsed == 2);
    assert(raid.shield.active && raid.shield.remaining == 1);
    const auto host = MakeHostApi(fake);
    sw_reader::Reader reader; PrimeWorldEntered(fake, host, reader);
    SetCurrentScreen(fake, 0xFFFFFFFFu);
    reader.SetBattleHint(true, true, client); reader.Sample(host);
    if (sw_battle_raid::kRuntimeVerified) {
        assert(fake.ints.at("sw.battle.raid.ready") == 1 && fake.ints.at("sw.battle.raid.ally_count") == 3);
        assert(fake.ints.at("sw.battle.raid.boss.current_hp") == 100);
        assert(fake.ints.at("sw.battle.raid.ally.0.fainted") == 1);
        assert(fake.texts.at("sw.battle.raid.ally.0.hp_pct_text") == "0%");
        assert(fake.ints.at("sw.battle.raid.boss.types_ready") == 1);
        assert(fake.ints.at("sw.battle.own.species") == 25 && fake.ints.at("sw.battle.foe.species") == 211);
        assert(fake.ints.at("sw.battle.raid.dmax.ready") == (sw_battle_raid::kDmaxVerified ? 1 : 0));
        if (sw_battle_raid::kDmaxVerified) assert(fake.ints.at("sw.battle.raid.dmax.turns") == 1);
        assert(fake.ints.at("sw.battle.raid.shield.ready") == (sw_battle_raid::kShieldVerified ? 1 : 0));
        if (sw_battle_raid::kShieldVerified) assert(fake.texts.at("sw.battle.raid.shield.text") == "1/2");
    } else {
        assert(fake.ints.at("sw.battle.raid.ready") == 0 && fake.ints.at("sw.battle.raid.active") == 0);
        assert(fake.ints.find("sw.battle.raid.boss.species") == fake.ints.end());
    }
    barrier->data[2] = 3;
    raid = sw_battle_raid::Read(client, read);
    assert(raid.ready && !raid.shield.valid); // a failed shield does not hide verified roster HP
    barrier->data[2] = 0;
    raid = sw_battle_raid::Read(client, read);
    assert(raid.ready && raid.shield.valid && !raid.shield.active); // native83DB60/83DB90
    barrier->data[2] = 1; barrier->data[0] = 0;
    reader.Sample(host);
    if (sw_battle_raid::kRuntimeVerified && sw_battle_raid::kShieldVerified) {
        fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
        assert(fake.ints.at("sw.battle.raid.shield.active") == 0);
        assert(fake.ints.find("sw.battle.raid.shield.total") == fake.ints.end());
        assert(fake.ints.find("sw.battle.raid.shield.remaining") == fake.ints.end());
    }
    own->data[0x299] = 4;
    assert(!sw_battle_raid::Read(client, read).ready); own->data[0x299] = 2;
    PutU64(roster_region->data, 0x120 + 12 * 8, first_mon + 2 * 0x1000);
    assert(!sw_battle_raid::Read(client, read).ready);
    PutU64(roster_region->data, 0x120 + 12 * 8, first_mon + 0x1000);
    cfg->data[0x3B9] = 0; raid = sw_battle_raid::Read(client, read);
    assert(raid.active && !raid.ready); cfg->data[0x3B9] = 5;
    cfg->data[0x3B8] = 1; raid = sw_battle_raid::Read(client, read);
    assert(raid.active && !raid.ready); cfg->data[0x3B8] = 0;
    int root_reads = 0;
    const auto changed_root = sw_battle_raid::Read(client, [&](std::uint64_t address, void* data, std::size_t bytes) {
        if (!read(address, data, bytes)) return false;
        if (address == client + 0x58 && ++root_reads == 2) {
            const std::uint64_t different = main + 0x1000;
            std::memcpy(data, &different, sizeof different);
        }
        return true;
    });
    assert(changed_root.active && !changed_root.ready);
    auto* client_region = const_cast<FakeRegion*>(fake.Find(client, 1));
    assert(client_region);
    PutU64(client_region->data, 0x58, 0);
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    if (sw_battle_raid::kRuntimeVerified) {
        assert(fake.ints.at("sw.battle.raid.ready") == 0);
        assert(fake.ints.at("sw.battle.own.ready") == 0 && fake.ints.at("sw.battle.foe.ready") == 0);
        assert(fake.ints.find("sw.battle.own.species") == fake.ints.end()); // no stale single fallback
    }
    PutU64(client_region->data, 0x58, main);
    reader.SetBattleHint(false, true, 0); reader.ResetSession();
    SetCurrentScreen(fake, 0); SetZoneName(fake, "");
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.battle.raid.ready") == 0 && fake.ints.at("sw.battle.raid.ally_count") == 0);
    std::puts("sw_reader: native raid roster preserves participant identity, fresh HP, and fail-closed gates");
}

// Battle Tower ("institution") battles allocate the bpp arrays away from the fixed heap anchor
// (live 2026-10-30, LIVE-READERS-132.md). The root comes from the battle client's own battle
// party (client+0x340) instead: PassA mirror when present (root 2), else PassB itself (root 3).
void TestBattleRootFromClientPartyInstitution() {
    for (const bool with_mirror : {true, false}) {
        FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
        AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
        const auto* a_own = sw_addresses::Find("battle.own_mon");
        const std::uint64_t fixed = fake.heap_begin + a_own->sysbot_offset;
        for (auto& r : fake.regions)
            if (r.base == fixed) std::fill(r.data.begin(), r.data.end(), 0); // stale anchor
        constexpr std::uint64_t kStride = 0x7A0, kPassB = 0x18910;
        const std::uint64_t pass_b = fake.heap_begin + 0x9A2EBFC0ull; // live tower offset
        std::vector<unsigned char> bb(12 * kStride + 0x350, 0);
        const auto put = [&](std::vector<unsigned char>& b, std::size_t at, std::uint16_t sp,
                             std::uint16_t mx, std::uint16_t cur) {
            PutU16(b, at + 0x70, sp); PutU16(b, at + 0x72, mx); PutU16(b, at + 0x74, cur);
            b[at + 0x7C] = 50;
            PutU16(b, at + 0x2D4, 85); b[at + 0x2D8] = 14; b[at + 0x2D9] = 15;
        };
        put(bb, 0, 25, 110, 110);
        put(bb, kStride, 3, 155, 155);
        put(bb, 2 * kStride, 6, 153, 77);
        put(bb, 12 * kStride, 344, 128, 128); // Claydol
        fake.regions.push_back({pass_b, bb});
        if (with_mirror) fake.regions.push_back({pass_b - kPassB, bb});
        const std::uint64_t client = fake.heap_begin + 0xA000'0000ull, party = client + 0x1000;
        const auto main = client + 0x2000;
        std::vector<unsigned char> cl(0x400, 0), pt(0x40, 0), ma(0x200);
        PutU64(cl, 0x58, main); PutU32(ma, 0x150, 0);
        PutU64(cl, 0x340, party);
        PutU64(pt, 0x00, pass_b + 2 * kStride); // Charizard active
        PutU64(pt, 0x08, pass_b);
        PutU64(pt, 0x10, pass_b + kStride);
        pt[0x30] = 3;
        fake.regions.push_back({client, cl});
        fake.regions.push_back({party, pt});
        fake.regions.push_back({main, ma});

        const EdenDsmodHostApi host = MakeHostApi(fake);
        sw_reader::Reader reader;
        AddLiveParty(fake);
        PrimeWorldEntered(fake, host, reader);
        SetCurrentScreen(fake, 0xFFFFFFFFu);
        reader.SetBattleHint(true, true, client);
        reader.Sample(host);
        assert(fake.ints.at("sw.battle.root_source") == (with_mirror ? 2 : 3));
        assert(fake.ints.at("sw.battle.own.ready") == 1 && fake.ints.at("sw.battle.foe.ready") == 1);
        assert(fake.ints.at("sw.battle.own.species") == 6);
        assert(fake.texts.at("sw.battle.own.hp_text") == "77/153");
        assert(fake.ints.at("sw.battle.foe.species") == 344);
        assert(reader.battle_view().active_pid == 2);

        // A party entry off the stride (torn/foreign pointer): no client root this tick. The
        // root read before stands for the hold (the same battle's own array, never another
        // battle's), then the stale fixed anchor fails Decode -> both sides fail closed.
        for (auto& r : fake.regions)
            if (r.base == party) PutU64(r.data, 0x08, pass_b + 8);
        for (int t = 0; t < sw_reader::Reader::kMenuHoldTicks; ++t) {
            reader.Sample(host);
            assert(fake.ints.at("sw.battle.root_source") == (with_mirror ? 2 : 3));
            assert(fake.ints.at("sw.battle.own.species") == 6);
        }
        reader.Sample(host);
        assert(fake.ints.at("sw.battle.root_source") == 1);
        assert(fake.ints.at("sw.battle.own.ready") == 0 && fake.ints.at("sw.battle.foe.ready") == 0);
    }
}

// Owner Thor 2026-10-31 (first Hop battle): the trainer's lead faints and the next Pokemon comes
// out at a different pokeID. The active foe must follow the foe participant's battle party in the
// native roster (sw_battle_foe.h), not the fixed lead slot (pokeID 12); a broken roster falls back.
void TestBattleForeignActiveFoeFollowsRosterParty() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    constexpr std::uint64_t kStride = 0x7A0, kPassB = 0x18910;
    const std::uint64_t pass_b = fake.heap_begin + 0x9A2EBFC0ull;
    std::vector<unsigned char> bb(15 * kStride + 0x350, 0);
    const auto put = [&](std::size_t id, std::uint16_t sp, std::uint16_t mx, std::uint16_t cur) {
        const std::size_t at = id * kStride;
        PutU16(bb, at + 0x70, sp); PutU16(bb, at + 0x72, mx); PutU16(bb, at + 0x74, cur);
        bb[at + 0x7C] = 6; bb[at + 0x7D] = static_cast<unsigned char>(id);
        PutU16(bb, at + 0x2D4, 33); bb[at + 0x2D8] = 30; bb[at + 0x2D9] = 35;
    };
    put(0, 813, 22, 20);    // own Scorbunny
    put(12, 819, 21, 0);    // Hop's Wooloo, fainted
    put(13, 816, 20, 20);   // Hop's Sobble, sent out next
    put(14, 810, 19, 19);   // a third member on the bench
    fake.regions.push_back({pass_b, bb});
    fake.regions.push_back({pass_b - kPassB, bb});
    const std::uint64_t client = fake.heap_begin + 0xA000'0000ull, main = client + 0x2000;
    const std::uint64_t pass = client + 0x3000, roster = client + 0x4000;
    std::vector<unsigned char> cl(0x400, 0), ma(0x200, 0), container(0x20, 0), table(0x210, 0);
    PutU64(cl, 0x58, main); PutU64(cl, 0x70, pass); PutU64(cl, 0x340, roster + 8);
    PutU32(ma, 0x150, 0);
    PutU64(container, 8, roster);
    PutU64(table, 0, main);
    PutU64(table, 8, pass_b); table[8 + 0x30] = 1;                         // own party
    PutU64(table, 8 + 0x38, pass_b + 13 * kStride);                        // foe: active first
    PutU64(table, 8 + 0x38 + 8, pass_b + 12 * kStride);
    PutU64(table, 8 + 0x38 + 16, pass_b + 14 * kStride);
    table[8 + 0x38 + 0x30] = 3;
    PutU64(table, 0x120, pass_b);
    for (int id = 12; id <= 14; ++id) PutU64(table, 0x120 + id * 8, pass_b + id * kStride);
    fake.regions.push_back({client, cl}); fake.regions.push_back({main, ma});
    fake.regions.push_back({pass, container}); fake.regions.push_back({roster, table});

    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    AddLiveParty(fake);
    PrimeWorldEntered(fake, host, reader);
    SetCurrentScreen(fake, 0xFFFFFFFFu);
    reader.SetBattleHint(true, true, client);
    reader.Sample(host);
    assert(fake.ints.at("sw.battle.root_source") == 2);
    assert(fake.ints.at("sw.battle.foe.source") == 1);
    assert(fake.ints.at("sw.battle.foe.ready") == 1 && fake.ints.at("sw.battle.foe.species") == 816);
    assert(fake.texts.at("sw.battle.foe.hp_text") == "20/20");
    assert(fake.ints.at("sw.battle.foe.party_count") == 3);
    // Sprite art offset (sw_species_icon_metrics.h): Sobble's box is x48..92, feet at row 108.
    assert(fake.ints.at("sw.battle.foe.icon_dx") == 68 - 70 && fake.ints.at("sw.battle.foe.icon_dy") == 104 - 108);
    assert(fake.ints.at("sw.battle.foe.bench.0.shown") == 1 && fake.ints.at("sw.battle.foe.bench.1.shown") == 1);
    assert(fake.ints.at("sw.battle.foe.bench.2.shown") == 0);
    // one bench member (Sobble's teammate at id 14) alive, Wooloo fainted -> exactly one dark ball
    assert(fake.ints.at("sw.battle.foe.bench.0.fainted") == 0 && fake.ints.at("sw.battle.foe.bench.1.fainted") == 1);

    // A roster pokeID table that disagrees with the party pointer: the roster read before stands
    // for the hold (a table mid-rewrite), then fail closed to the fixed lead.
    for (auto& r : fake.regions)
        if (r.base == roster) PutU64(r.data, 0x120 + 13 * 8, pass_b + 15 * kStride);
    for (int t = 0; t < sw_reader::Reader::kMenuHoldTicks; ++t) {
        reader.Sample(host);
        assert(fake.ints.at("sw.battle.foe.source") == 1 && fake.ints.at("sw.battle.foe.species") == 816);
    }
    reader.Sample(host);
    assert(fake.ints.at("sw.battle.foe.source") == 0 && fake.ints.at("sw.battle.foe.party_count") == 0);
    assert(fake.ints.at("sw.battle.foe.bench.0.shown") == 0);
    assert(fake.ints.at("sw.battle.foe.species") == 819);
}

// Battle-kinds bug 4 (2026-11-01, Tower doubles): rule 1 -> both party[0..1] of each side are
// active. The header follows the own Pokemon choosing now (client+0x78), the partner is a chip,
// not a bench icon; the second active foe is not a bench ball; the Foe selection switches foes.
void TestBattleDoublesActives() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    constexpr std::uint64_t kStride = 0x7A0, kPassB = 0x18910;
    const std::uint64_t pass_b = fake.heap_begin + 0x9A2EBFC0ull;
    std::vector<unsigned char> bb(15 * kStride + 0x350, 0);
    const auto put = [&](std::size_t id, std::uint16_t sp, std::uint16_t mx, std::uint16_t cur) {
        const std::size_t at = id * kStride;
        PutU16(bb, at + 0x70, sp); PutU16(bb, at + 0x72, mx); PutU16(bb, at + 0x74, cur);
        bb[at + 0x7C] = 50; bb[at + 0x7D] = static_cast<unsigned char>(id);
        PutU16(bb, at + 0x2D4, 33); bb[at + 0x2D8] = 30; bb[at + 0x2D9] = 35;
    };
    put(0, 3, 155, 155);    // own left: Venusaur
    put(1, 6, 153, 120);    // own right: Charizard (choosing)
    put(12, 819, 21, 21);   // foe party[0]
    put(13, 816, 20, 20);   // foe party[1]: also active
    put(14, 810, 19, 19);   // the foe's one bench member
    fake.regions.push_back({pass_b, bb});
    fake.regions.push_back({pass_b - kPassB, bb});
    const std::uint64_t client = fake.heap_begin + 0xA000'0000ull, main = client + 0x2000;
    const std::uint64_t pass = client + 0x3000, roster = client + 0x4000;
    std::vector<unsigned char> cl(0x400, 0), ma(0x200, 0), container(0x20, 0), table(0x210, 0);
    PutU64(cl, 0x58, main); PutU64(cl, 0x70, pass); PutU64(cl, 0x340, roster + 8);
    PutU64(cl, 0x78, pass_b + kStride);   // the current mon: own party[1]
    PutU32(ma, 0x150, 1);                 // rule 1: double battle
    PutU64(container, 8, roster);
    PutU64(table, 0, main);
    PutU64(table, 8, pass_b); PutU64(table, 16, pass_b + kStride); table[8 + 0x30] = 2;
    PutU64(table, 8 + 0x38, pass_b + 12 * kStride);
    PutU64(table, 8 + 0x38 + 8, pass_b + 13 * kStride);
    PutU64(table, 8 + 0x38 + 16, pass_b + 14 * kStride);
    table[8 + 0x38 + 0x30] = 3;
    PutU64(table, 0x120, pass_b); PutU64(table, 0x128, pass_b + kStride);
    for (int id = 12; id <= 14; ++id) PutU64(table, 0x120 + id * 8, pass_b + id * kStride);
    fake.regions.push_back({client, cl}); fake.regions.push_back({main, ma});
    fake.regions.push_back({pass, container}); fake.regions.push_back({roster, table});

    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    AddLiveParty(fake);
    PrimeWorldEntered(fake, host, reader);
    SetCurrentScreen(fake, 0xFFFFFFFFu);
    reader.SetBattleHint(true, true, client);
    reader.Sample(host);
    assert(fake.ints.at("sw.battle.doubles") == 1 && reader.battle_rule() == 1);
    // own: Charizard chooses -> header/own tab; Venusaur is the partner chip; no bench icons
    assert(fake.ints.at("sw.battle.own.choosing") == 1 && fake.ints.at("sw.battle.own.species") == 6);
    assert(fake.ints.at("sw.battle.own.duo2") == 1 && fake.ints.at("sw.battle.partner.ready") == 1);
    assert(fake.ints.at("sw.battle.partner.hp_pct") == 100 && fake.ints.at("sw.battle.bench.count") == 0);
    // foe: party[0] in the header, party[1] the other-foe chip, one bench ball (id 14)
    assert(fake.ints.at("sw.battle.foe.duo") == 1 && fake.ints.at("sw.battle.foe.species") == 819);
    assert(fake.ints.at("sw.battle.foe.other.ready") == 1 && fake.ints.at("sw.battle.foe.sel0") == 1);
    assert(fake.ints.at("sw.battle.foe.bench.0.shown") == 1 && fake.ints.at("sw.battle.foe.bench.1.shown") == 0);
    // the Foe selection (other-foe chip): the header / Foe tab / effectiveness follow
    assert(reader.SelectFoe(1));
    reader.Sample(host);
    assert(fake.ints.at("sw.battle.foe.species") == 816 && fake.ints.at("sw.battle.foe.sel1") == 1);
    // Back to a single battle (rule 0): party[1] is a bench member again, no selection.
    for (auto& r : fake.regions)
        if (r.base == main) PutU32(r.data, 0x150, 0);
    reader.Sample(host);
    assert(fake.ints.at("sw.battle.doubles") == 0 && fake.ints.at("sw.battle.foe.duo") == 0);
    assert(fake.ints.at("sw.battle.foe.species") == 819 && fake.ints.at("sw.battle.own.species") == 3);
    assert(fake.ints.at("sw.battle.foe.bench.1.shown") == 1 && fake.ints.at("sw.battle.bench.count") == 1);
    assert(!reader.SelectFoe(1));
}

// A sides fixture (sw_battle_foe.h): a roster with up to five client parties at the live PassB
// stride, a rule / multiMode, and (optionally) the native target select open on a grid index.
struct SidesFixture {
    static constexpr std::uint64_t kStride = 0x7A0, kPassB = 0x18910;
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    std::uint64_t pass_b = 0, client = 0, main = 0, pass = 0, roster = 0, setup = 0;
    std::vector<unsigned char> bb, cl, ma, container, table, su;
    SidesFixture() {
        AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
        fake.main_base = 0x7100000000ull;
        pass_b = fake.heap_begin + 0x9A2EBFC0ull;
        bb.assign(24 * kStride + 0x350, 0);
        client = fake.heap_begin + 0xA000'0000ull;
        main = client + 0x2000; pass = client + 0x3000; roster = client + 0x4000; setup = client + 0x5000;
        cl.assign(0x800, 0); ma.assign(0x200, 0); container.assign(0x20, 0); table.assign(0x210, 0);
        su.assign(0x400, 0);
        PutU64(cl, 0x58, main); PutU64(cl, 0x70, pass); PutU64(cl, 0x340, roster + 8);
        PutU64(container, 8, roster);
        PutU64(table, 0, main);
        PutU64(ma, 0x88, setup);
    }
    void Mon(unsigned id, std::uint16_t species, std::uint16_t max_hp, std::uint16_t cur_hp) {
        const std::size_t at = id * kStride;
        PutU16(bb, at + 0x70, species); PutU16(bb, at + 0x72, max_hp); PutU16(bb, at + 0x74, cur_hp);
        bb[at + 0x7C] = 41; bb[at + 0x7D] = static_cast<unsigned char>(id);
        PutU16(bb, at + 0x2D4, 33); bb[at + 0x2D8] = 30; bb[at + 0x2D9] = 35;
        PutU64(table, 0x120 + id * 8, pass_b + id * kStride);
    }
    void Party(int c, std::initializer_list<unsigned> ids) {
        int i = 0;
        for (const unsigned id : ids) PutU64(table, 8 + c * 0x38 + 8 * i++, pass_b + id * kStride);
        table[8 + c * 0x38 + 0x30] = static_cast<unsigned char>(ids.size());
    }
    void Rule(std::uint32_t rule, int mode) { // mode < 0: setup unreadable
        PutU32(ma, 0x150, rule);
        if (mode < 0) PutU64(ma, 0x88, 0);
        else su[0x3B9] = static_cast<unsigned char>(mode);
    }
    // The native target select (FIGHT step 6) with the cursor on `grid` (12 x 2), the chain
    // sw_battle_cursor.h walks (client+0x358 ... views[3] list +0x5D8 cursor).
    void TargetSelect(std::uint32_t grid) {
        const std::uint64_t base = client + 0x10000, view = base, sub = base + 0x1000, slot = base + 0x2000,
                            s1 = base + 0x3000, ui = base + 0x4000, arr = base + 0x5000, tv = base + 0x6000,
                            list = base + 0x7000, cursor = base + 0x8000;
        PutU64(cl, 0x560, fake.main_base + sw_battle_cursor::kFightFn);
        PutU32(cl, 0x570, sw_battle_cursor::kFightTargetStep);
        PutU64(cl, 0x358, view);
        std::vector<unsigned char> v(0x700, 0), su_(0x40, 0), sl(0x20, 0), s(0x700, 0), u(0x100, 0),
            a(21 * 8, 0), t(0x600, 0), l(0x410, 0), c(0x20, 0);
        PutU64(v, 0x5C8, sub); PutU64(su_, 0x38, slot); PutU64(sl, 0x10, s1); PutU64(s, 0x6D8, ui);
        PutU64(u, 0, fake.main_base + sw_battle_cursor::kUiRootVtable);
        PutU64(u, 0x78, arr); PutU64(u, 0x80, arr + 21 * 8);
        for (int i = 0; i < 21; ++i) PutU64(a, i * 8, base + 0x20000 + i * 0x1000);
        PutU64(a, 3 * 8, tv);
        PutU64(t, 0, fake.main_base + sw_battle_cursor::kTargetViewVtable);
        PutU64(t, sw_battle_cursor::kTargetListOffset, list);
        PutU64(l, 0, fake.main_base + sw_battle_cursor::kListVtable); PutU64(l, 0x408, cursor);
        PutU64(c, 0, fake.main_base + sw_battle_cursor::kCursorVtable);
        PutU32(c, 8, grid); PutU32(c, 0xC, 12); PutU32(c, 0x10, 2);
        for (auto& [at, data] : std::initializer_list<std::pair<std::uint64_t, std::vector<unsigned char>*>>{
                 {view, &v}, {sub, &su_}, {slot, &sl}, {s1, &s}, {ui, &u}, {arr, &a}, {tv, &t}, {list, &l}, {cursor, &c}})
            fake.regions.push_back({at, *data});
    }
    void SetGrid(std::uint32_t grid) {
        for (auto& r : fake.regions)
            if (r.base == client + 0x18000) PutU32(r.data, 8, grid);
    }
    EdenDsmodHostApi Start(sw_reader::Reader& reader) {
        fake.regions.push_back({pass_b, bb});
        fake.regions.push_back({pass_b - kPassB, bb});
        fake.regions.push_back({client, cl}); fake.regions.push_back({main, ma});
        fake.regions.push_back({pass, container}); fake.regions.push_back({roster, table});
        fake.regions.push_back({setup, su});
        const EdenDsmodHostApi host = MakeHostApi(fake);
        AddLiveParty(fake);
        PrimeWorldEntered(fake, host, reader);
        SetCurrentScreen(fake, 0xFFFFFFFFu);
        reader.SetBattleHint(true, true, client);
        reader.Sample(host);
        return host;
    }
};

// Owner Thor 2026-11-01 (dev.30): a double battle against TWO trainers, one Pokemon each
// (Maractus far-left, Toxtricity far-right). multiMode 4 = you hold positions 0 and 2, trainer A
// position 1, trainer B position 3: two foe parties. Both are active foes (header + other-foe
// chip), each trainer's bench is its own ball group, and target select has all four cards.
void TestBattleTwoFoeParties() {
    SidesFixture f;
    f.Rule(1, 4);
    f.Mon(0, 3, 155, 155);     // own Venusaur (position 0)
    f.Mon(1, 6, 297, 168);     // own Charizard (position 2, choosing)
    f.Mon(2, 25, 90, 90);      // own bench Pikachu
    f.Mon(12, 849, 112, 112);  // trainer A: Toxtricity (position 1, far right)
    f.Mon(13, 810, 80, 80);    //            its bench, alive
    f.Mon(18, 556, 100, 50);   // trainer B: Maractus (position 3, far left)
    f.Mon(19, 819, 70, 0);     //            its bench, fainted
    f.Party(0, {0, 1, 2});
    f.Party(1, {12, 13});
    f.Party(3, {18, 19});
    PutU64(f.cl, 0x78, f.pass_b + 1 * SidesFixture::kStride); // the current mon: Charizard
    f.TargetSelect(6); // cursor on the far-right card
    sw_reader::Reader reader;
    const auto host = f.Start(reader);
    auto& ints = f.fake.ints;
    assert(ints.at("sw.battle.doubles") == 1 && ints.at("sw.battle.sides.mode") == 4);
    assert(ints.at("sw.battle.foe.source") == 1 && ints.at("sw.battle.foe.duo") == 1);
    // the cursor rests on the far-right card -> the header shows that foe (Toxtricity)
    assert(ints.at("sw.battle.native.target") == 1 && ints.at("sw.battle.foe.sel0") == 1);
    assert(ints.at("sw.battle.foe.species") == 849 && ints.at("sw.battle.foe.other.ready") == 1);
    assert(ints.at("sw.battle.foe.other.hp_pct") == 50); // Maractus as the other-foe chip
    assert(ints.at("sw.battle.foe.party_count") == 4);
    // own: Charizard chooses, Venusaur the partner chip, Pikachu the one bench member
    assert(ints.at("sw.battle.own.species") == 6 && ints.at("sw.battle.own.choosing") == 1);
    assert(ints.at("sw.battle.own.duo2") == 1 && ints.at("sw.battle.partner.hp_pct") == 100);
    assert(ints.at("sw.battle.bench.count") == 1);
    // bench balls: trainer B's group (far left) first, then trainer A's after a gap
    assert(ints.at("sw.battle.foe.bench.0.shown") == 1 && ints.at("sw.battle.foe.bench.0.fainted") == 1 &&
           ints.at("sw.battle.foe.bench.0.dx") == 0);
    assert(ints.at("sw.battle.foe.bench.1.shown") == 1 && ints.at("sw.battle.foe.bench.1.fainted") == 0 &&
           ints.at("sw.battle.foe.bench.1.dx") == 24);
    assert(ints.at("sw.battle.foe.bench.2.shown") == 0);
    // target select: four live cards by position, the user (Charizard, near right) greyed
    assert(ints.at("sw.battle.target.live") == 1 && reader.target_cursor_ok());
    assert(ints.at("sw.battle.target.0.ready") == 1 && ints.at("sw.battle.target.0.hp_pct") == 50);
    assert(ints.at("sw.battle.target.1.ready") == 1 && ints.at("sw.battle.target.1.hp_pct") == 100);
    assert(ints.at("sw.battle.target.2.ready") == 1 && ints.at("sw.battle.target.2.user") == 0);
    assert(ints.at("sw.battle.target.3.ready") == 1 && ints.at("sw.battle.target.3.user") == 1);
    assert(ints.at("sw.battle.target.3.hp_pct") == 168 * 100 / 297);
    // the cursor moves to the far-left card: header, Foe tab and effectiveness follow Maractus
    f.SetGrid(0);
    reader.Sample(host);
    assert(ints.at("sw.battle.native.target") == 0 && ints.at("sw.battle.foe.sel1") == 1);
    assert(ints.at("sw.battle.foe.species") == 556 && ints.at("sw.battle.foe.other.hp_pct") == 100);
    // the Foe tab switch back (other-foe chip tap) works while the cursor stays
    assert(reader.SelectFoe(0));
    reader.Sample(host);
    assert(ints.at("sw.battle.foe.species") == 849);
    // Trainer B's Maractus faints and it has nothing left: still its position's card, fainted.
    for (auto& r : f.fake.regions)
        if (r.base == f.pass_b || r.base == f.pass_b - SidesFixture::kPassB) PutU16(r.data, 18 * SidesFixture::kStride + 0x74, 0);
    reader.Sample(host);
    assert(ints.at("sw.battle.target.0.ready") == 1 && ints.at("sw.battle.target.0.fainted") == 1);
    std::puts("sw_reader: two foe parties (1 vs 2): both active foes, per-trainer balls, four target cards");
}

// Multi battle (multiMode 1..3, four clients, one position each): your party[1] is a BENCH
// member, the partner is the NPC client 2's lead (species name), both foes from clients 1 and 3.
void TestBattleMultiPartner() {
    SidesFixture f;
    f.Rule(1, 1);
    f.Mon(0, 3, 155, 155);     // own Venusaur (position 0)
    f.Mon(1, 6, 297, 297);     // own Charizard: bench in a multi battle
    f.Mon(6, 832, 160, 40);    // partner (client 2): Dubwool (position 2)
    f.Mon(12, 849, 112, 112);  // foe client 1 (position 1)
    f.Mon(18, 556, 100, 100);  // foe client 3 (position 3)
    f.Party(0, {0, 1});
    f.Party(1, {12});
    f.Party(2, {6});
    f.Party(3, {18});
    f.TargetSelect(12); // near-left = you
    sw_reader::Reader reader;
    f.Start(reader);
    auto& ints = f.fake.ints;
    assert(ints.at("sw.battle.sides.mode") == 1);
    assert(ints.at("sw.battle.own.species") == 3 && ints.at("sw.battle.own.choosing") == 0);
    assert(ints.at("sw.battle.bench.count") == 1); // Charizard waits on the bench
    assert(ints.at("sw.battle.own.duo2") == 1 && ints.at("sw.battle.partner.ready") == 1);
    assert(ints.at("sw.battle.partner.hp_pct") == 25 && ints.at("sw.battle.partner.shiny") == 0);
    assert(ints.at("sw.battle.foe.duo") == 1 && ints.at("sw.battle.foe.species") == 849);
    assert(ints.at("sw.battle.foe.bench.0.shown") == 0);
    assert(ints.at("sw.battle.target.live") == 1);
    assert(ints.at("sw.battle.target.2.user") == 1 && ints.at("sw.battle.target.3.hp_pct") == 25);
    assert(ints.at("sw.battle.target.3.user") == 0);
    std::puts("sw_reader: multi battle: party[1] benched, the NPC partner's lead is the partner chip");
}

// Two foe parties but the battle setup (multiMode) unreadable: never guessed -> fixed-lead
// fallback, no target cards, and the top target select stays visible.
void TestBattleTwoFoePartiesUnknownModeFailsClosed() {
    SidesFixture f;
    f.Rule(1, -1);
    f.Mon(0, 3, 155, 155);
    f.Mon(1, 6, 297, 168);
    f.Mon(12, 849, 112, 112);
    f.Mon(18, 556, 100, 50);
    f.Party(0, {0, 1});
    f.Party(1, {12});
    f.Party(3, {18});
    f.TargetSelect(6);
    sw_reader::Reader reader;
    f.Start(reader);
    auto& ints = f.fake.ints;
    assert(ints.at("sw.battle.foe.source") == 0 && ints.at("sw.battle.foe.duo") == 0);
    assert(ints.at("sw.battle.sides.mode") == -1);
    assert(ints.at("sw.battle.target.live") == 0 && !reader.target_cursor_ok());
    std::puts("sw_reader: two foe parties with an unknown multiMode fail closed (top target select kept)");
}

// The target cards' names: the game's species names (companion language) for both foes, your
// partner and you, in each doubles kind -- one trainer, two trainers (1 vs 2), multi.
void AddTargetNames(SidesFixture& f) {
    std::vector<std::string> names(900, "x");
    names[3] = "Venusaur"; names[6] = "Charizard"; names[25] = "Pikachu"; names[470] = "Leafeon";
    names[471] = "Glaceon"; names[556] = "Maractus"; names[832] = "Dubwool"; names[849] = "Toxtricity";
    f.fake.dat_files[sw_species_names::MonsNameDatPath()] = GameMessageFixture(names);
}
void AssertCards(const FakeHost& fake, const std::array<const char*, 4>& want) {
    for (int t = 0; t < 4; ++t) {
        const std::string prefix = "sw.battle.target." + std::to_string(t);
        assert(fake.ints.at(prefix + ".ready") == 1 && fake.ints.at(prefix + ".name_ready") == 1);
        assert(fake.texts.at(prefix + ".name") == want[static_cast<std::size_t>(t)]);
    }
}
void TestTargetCardNamesEveryDoublesKind() {
    {   // one trainer (mode 0): far-left = its party[1], far-right = party[0]
        SidesFixture f;
        f.Rule(1, 0);
        f.Mon(0, 3, 155, 155); f.Mon(1, 6, 297, 297);
        f.Mon(12, 471, 120, 120); f.Mon(13, 470, 120, 60);
        f.Party(0, {0, 1}); f.Party(1, {12, 13});
        f.TargetSelect(0);
        AddTargetNames(f);
        sw_reader::Reader reader;
        f.Start(reader);
        assert(f.fake.ints.at("sw.battle.target.live") == 1);
        AssertCards(f.fake, {"Leafeon", "Glaceon", "Venusaur", "Charizard"});
    }
    {   // two trainers (mode 4): one foe per trainer
        SidesFixture f;
        f.Rule(1, 4);
        f.Mon(0, 3, 155, 155); f.Mon(1, 6, 297, 168);
        f.Mon(12, 849, 112, 112); f.Mon(18, 556, 100, 50);
        f.Party(0, {0, 1}); f.Party(1, {12}); f.Party(3, {18});
        f.TargetSelect(6);
        AddTargetNames(f);
        sw_reader::Reader reader;
        f.Start(reader);
        assert(f.fake.ints.at("sw.battle.target.live") == 1);
        AssertCards(f.fake, {"Maractus", "Toxtricity", "Venusaur", "Charizard"});
    }
    {   // multi (mode 1): the NPC partner's lead by its species name
        SidesFixture f;
        f.Rule(1, 1);
        f.Mon(0, 3, 155, 155); f.Mon(1, 6, 297, 297); f.Mon(6, 832, 160, 40);
        f.Mon(12, 849, 112, 112); f.Mon(18, 556, 100, 100);
        f.Party(0, {0, 1}); f.Party(1, {12}); f.Party(2, {6}); f.Party(3, {18});
        f.TargetSelect(12);
        AddTargetNames(f);
        sw_reader::Reader reader;
        f.Start(reader);
        assert(f.fake.ints.at("sw.battle.target.live") == 1);
        AssertCards(f.fake, {"Maractus", "Toxtricity", "Venusaur", "Dubwool"});
    }
    std::puts("sw_reader: target cards name both foes, the partner and you in every doubles kind");
}

// Owner Thor 2026-11-01 (dev.33, doubles): "some time top screen shown, sometime hidden ... some
// time tell choose pokemon on top screen, some time have 4 selectable pokemon". Intermittent
// failed reads -- the client state, the target cursor chain, the roster, the bpp root, one card's
// battler -- on most ticks for 10 s: the bottom keeps its four named cards and the hovered card,
// the top hide request stands; the state READ as over ends it at once; reads failing for good
// end it within the hold (the top comes back, the bottom shows its notice -- never neither).
void TestTargetSelectHoldsThroughReadFailures() {
    SidesFixture f;
    f.Rule(1, 0);
    f.Mon(0, 3, 155, 155); f.Mon(1, 6, 297, 297);
    f.Mon(12, 471, 120, 120); f.Mon(13, 470, 120, 60);
    f.Party(0, {0, 1}); f.Party(1, {12, 13});
    f.TargetSelect(6);
    AddTargetNames(f);
    const std::uint64_t stride = SidesFixture::kStride, pass_a = f.pass_b - SidesFixture::kPassB;
    const auto in = [](std::uint64_t a, std::uint64_t lo, std::uint64_t n) { return a >= lo && a < lo + n; };
    int tick = 0;
    bool forever = false;
    f.fake.fail_read = [&](std::uint64_t a, std::size_t) {
        if (forever) return in(a, f.client, 0x800);
        switch (tick % 7) {
        case 1: return in(a, f.roster, 0x210);                                   // the roster
        case 2: return in(a, f.client + 0x560, 0x18);                           // the client state
        case 3: return in(a, f.client + 0x18000, 0x20);                         // the target cursor
        case 4: return in(a, f.pass_b + 12 * stride, 0x350) || in(a, pass_a + 12 * stride, 0x350); // a foe
        case 5: return in(a, f.client + 0x340, 8);                              // the bpp root
        case 6: return tick % 2 == 0 && in(a, f.client + 0x10000, 0x8000);      // the view chain
        default: return false;
        }
    };
    sw_reader::Reader reader;
    const auto host = f.Start(reader);
    auto& ints = f.fake.ints;
    for (tick = 0; tick < 600; ++tick) { // 10 s at 60 Hz
        reader.Sample(host);
        assert(ints.at("sw.battle.native.menu") == static_cast<int>(sw_battle_cursor::Menu::Target));
        assert(ints.at("sw.battle.native.target_known") == 1 && ints.at("sw.battle.native.target1") == 1);
        assert(ints.at("sw.battle.target.live") == 1 && reader.target_cursor_ok());
        AssertCards(f.fake, {"Leafeon", "Glaceon", "Venusaur", "Charizard"});
        assert(ints.at("sw.battle.foe.duo") == 1 && ints.at("sw.battle.root_source") != 1);
    }
    assert(ints.at("sw.battle.hold.menu") > 0 && ints.at("sw.battle.hold.cards") > 0 &&
           ints.at("sw.battle.hold.sides_fail") > 0 && ints.at("sw.battle.hold.window_fail") > 0);
    // The cursor moves: followed at once.
    tick = 0;
    f.SetGrid(18);
    reader.Sample(host);
    assert(ints.at("sw.battle.native.target") == 3 && ints.at("sw.battle.target.live") == 1);
    // The state READ as the command menu's: target select is over this very tick.
    for (auto& r : f.fake.regions)
        if (r.base == f.client) PutU64(r.data, 0x560, f.fake.main_base + sw_battle_cursor::kCommandWaitFn);
    reader.Sample(host);
    assert(ints.at("sw.battle.native.menu") != static_cast<int>(sw_battle_cursor::Menu::Target));
    assert(ints.at("sw.battle.target.live") == 0);
    // Back in target select, then every client read fails for good: held for the hold only.
    for (auto& r : f.fake.regions)
        if (r.base == f.client) PutU64(r.data, 0x560, f.fake.main_base + sw_battle_cursor::kFightFn);
    reader.Sample(host);
    assert(ints.at("sw.battle.target.live") == 1 && reader.target_cursor_ok());
    forever = true;
    int held = 0;
    while (ints.at("sw.battle.native.menu") == static_cast<int>(sw_battle_cursor::Menu::Target) && held < 100) {
        reader.Sample(host);
        ++held;
    }
    assert(held > 1 && held <= sw_reader::Reader::kMenuHoldTicks + 1);
    assert(ints.at("sw.battle.target.live") == 0 && !reader.target_cursor_ok()); // top shown
    std::puts("sw_reader: target select holds through intermittent failed reads, ends when read as over");
}

// Review 3 C1: a wild foe's shiny mark is cached per battle. The next wild battle can reuse the
// battle client's address and meet the same species / level / max HP; it re-reads its own PK8.
void TestWildShinyCachedPerBattle() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, 200, 356, 150, 70, 120, 200);
    const std::uint64_t client = fake.heap_begin + 0xA000'0000ull, main = client + 0x2000, setup = client + 0x3000;
    std::vector<unsigned char> cl(0x400, 0), ma(0x200, 0), su(0x400, 0);
    PutU64(cl, 0x58, main); PutU32(ma, 0x150, 0); PutU64(ma, 0x88, setup); // wild (competitor 0), offline
    fake.regions.push_back({client, cl}); fake.regions.push_back({main, ma}); fake.regions.push_back({setup, su});
    const auto shiny = ShinyVariant(g_mewtwo, false);
    std::vector<unsigned char> pk(sw_pk8::kSizeParty, 0);
    std::memcpy(pk.data(), shiny.data(), shiny.size());
    const std::uint64_t pk_at = fake.heap_begin + 0x8FEA3648ull;
    fake.regions.push_back({pk_at, pk});
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    AddLiveParty(fake);
    PrimeWorldEntered(fake, host, reader);
    SetCurrentScreen(fake, 0xFFFFFFFFu);
    reader.SetBattleHint(true, true, client);
    reader.Sample(host);
    assert(fake.ints.at("sw.battle.foe.species") == 150 && fake.ints.at("sw.battle.foe.shiny") == 1);
    // the battle ends; the next one (same client address, same Mewtwo Lv.70 200 HP) is not shiny
    FieldRan(reader); // the guest runs the field again (battle over, not a stall)
    reader.SetBattleHint(false, true, 0);
    SetCurrentScreen(fake, 0xFFFF5127u);
    reader.Sample(host);
    reader.Sample(host);
    for (auto& r : fake.regions)
        if (r.base == pk_at) std::memcpy(r.data.data(), g_mewtwo, sizeof g_mewtwo);
    SetCurrentScreen(fake, 0xFFFFFFFFu);
    reader.SetBattleHint(true, true, client);
    reader.Sample(host);
    assert(fake.ints.at("sw.battle.foe.species") == 150 && fake.ints.at("sw.battle.foe.shiny") == 0);
    std::puts("sw_reader: the wild shiny mark never carries into the next battle (battle epoch)");
}


void TestBattleContinuityActiveAndBench() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    fake.personal[25] = {12, 12};
    AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    const auto* a_own = sw_addresses::Find("battle.own_mon");
    const std::uint64_t own_base = fake.heap_begin + a_own->sysbot_offset;
    constexpr std::uint64_t kStride = 0x7A0, kPassB = 0x18910;
    // pokeID 1 (Venusaur 250/301) and 2 (Charizard, fainted) in the bpp array.
    for (auto& r : fake.regions) {
        if (r.base != own_base) continue;
        PutU16(r.data, kStride + 0x70, 3);
        PutU16(r.data, kStride + 0x72, 301);
        PutU16(r.data, kStride + 0x74, 250);
        r.data[kStride + 0x7C] = 100;
        PutU16(r.data, 2 * kStride + 0x70, 6);
        PutU16(r.data, 2 * kStride + 0x72, 297);
        PutU16(r.data, 2 * kStride + 0x74, 0);
        r.data[2 * kStride + 0x7C] = 100;
    }
    // Battle client: +0x340 -> its battle party {ptrs[6], count at +0x30}, active first.
    const std::uint64_t client = fake.heap_begin + 0xA000'0000ull, party = client + 0x1000;
    const auto main = client + 0x2000;
    std::vector<unsigned char> cl(0x400, 0), pt(0x40, 0), ma(0x200);
    PutU64(cl, 0x58, main); PutU32(ma, 0x150, 0); // native single rule, independent of raid
    PutU64(cl, 0x340, party);
    const std::uint64_t pass_b = own_base + kPassB;
    PutU64(pt, 0x00, pass_b + 1 * kStride); // Venusaur active (switched in)
    PutU64(pt, 0x08, pass_b + 0 * kStride);
    PutU64(pt, 0x10, pass_b + 2 * kStride);
    pt[0x30] = 3;
    fake.regions.push_back({client, cl});
    fake.regions.push_back({party, pt});
    fake.regions.push_back({main, ma});

    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    AddLiveParty(fake);
    PrimeWorldEntered(fake, host, reader);
    SetCurrentScreen(fake, 0xFFFFFFFFu);
    reader.SetBattleHint(true, true, client);
    reader.Sample(host);
    assert(fake.ints.at("sw.state.battle") == 1);
    assert(fake.ints.at("sw.battle.own.species") == 3);
    assert(fake.texts.at("sw.battle.own.hp_text") == "250/301");
    const auto& v = reader.battle_view();
    assert(v.active_pid == 1 && v.members[0].ok && v.members[2].fainted && !v.members[1].fainted);
    assert(fake.ints.at("sw.battle.bench.count") == 2);
    assert(fake.ints.at("sw.battle.bench.0.pid") == 0 && fake.ints.at("sw.battle.bench.1.pid") == 2);
    assert(fake.ints.at("sw.battle.bench.1.fainted") == 1 && fake.ints.at("sw.battle.bench.0.fainted") == 0);
    sw_helper::Status st;
    st.helper_present = st.menu_waiting = st.offline_single = true;
    assert(sw_helper::CheckSwitch(st, v, 0) == sw_helper::Refusal::None);
    assert(sw_helper::CheckSwitch(st, v, 1) == sw_helper::Refusal::ActiveMember);
    assert(sw_helper::CheckSwitch(st, v, 2) == sw_helper::Refusal::FaintedMember);

    // dev.19 stats / stages / gender from the bpp (+0x2A2 stats, +0x2AF gender, +0x2B4 stages).
    for (auto& r : fake.regions)
        if (r.base == own_base) {
            const std::size_t o = kStride; // active pid 1
            for (int k = 0; k < 5; ++k) PutU16(r.data, o + 0x2A2 + k * 2, static_cast<std::uint16_t>(100 + k));
            r.data[o + 0x2AF] = 1;
            for (int k = 0; k < 7; ++k) r.data[o + 0x2B4 + k] = 6;
            r.data[o + 0x2B4] = 8;  // Attack +2
            r.data[o + 0x2B5] = 5;  // Defense -1
            r.data[o + 0x2BA] = 7;  // evasion +1
        }
    reader.Sample(host);
    assert(fake.ints.at("sw.battle.own.gender_ready") == 1 && fake.ints.at("sw.battle.own.gender") == 1);
    assert(fake.texts.at("sw.battle.own.st0.text") == "250/301");
    assert(fake.texts.at("sw.battle.own.st1.text") == "100 » 200" && fake.ints.at("sw.battle.own.st1.up") == 1);
    assert(fake.texts.at("sw.battle.own.st1.stage") == "+2");
    assert(fake.texts.at("sw.battle.own.st2.text") == "101 » 67" && fake.ints.at("sw.battle.own.st2.down") == 1);
    assert(fake.texts.at("sw.battle.own.st5.text") == "104" && fake.ints.at("sw.battle.own.st5.up") == 0);
    assert(fake.texts.at("sw.battle.own.accev") == "Evasion +1" && fake.ints.at("sw.battle.own.accev_on") == 1);

    // Matchup-card tabs: Foe / Own / Field only.
    assert(reader.DispatchAction("battle_tab", 2) && !reader.DispatchAction("battle_tab", 3));
    reader.Sample(host);
    assert(fake.ints.at("sw.battle.tab.2") == 1 && fake.ints.at("sw.battle.tab.0") == 0);

    // Turn animation (CurrentScreen 0) and the native party screen (overlay): still battle, live.
    SetCurrentScreen(fake, 0x00000000u);
    for (auto& r : fake.regions)
        if (r.base == own_base) PutU16(r.data, kStride + 0x74, 120); // took a hit mid-turn
    reader.Sample(host);
    assert(fake.ints.at("sw.state.battle") == 1 && fake.ints.at("sw.battle.own.ready") == 1);
    // dev.19: the shown HP drains toward the new HP (no plate object in the fake heap) over
    // kDrainTicks: 130 HP / 120 ticks -> 2 per tick: 250 -> 248 on the first tick, 120 after 65.
    assert(fake.texts.at("sw.battle.own.hp_text") == "248/301");
    assert(fake.ints.at("sw.battle.own.gauge_source") == 0);
    for (int i = 0; i < 64; ++i) reader.Sample(host);
    assert(fake.texts.at("sw.battle.own.hp_text") == "120/301");
    assert(fake.ints.at("sw.battle.own.cur_hp") == 120);
    SetCurrentScreen(fake, 0xFF000000u);
    for (int i = 0; i < 200; ++i) reader.Sample(host); // longer than every legacy timeout
    assert(fake.ints.at("sw.state.battle") == 1 && fake.ints.at("sw.state.menu") == 0);
    assert(fake.ints.at("sw.battle.own.ready") == 1);

    // Battle over: the beat stops. No field sentinel (a town): party+zone valid for 30 ticks.
    FieldRan(reader); // the guest runs the field again (battle over, not a stall)
    reader.SetBattleHint(false, false, 0); // helper stays "seen" for this boot
    SetCurrentScreen(fake, 0x00000000u);
    for (int i = 0; i < 29; ++i) reader.Sample(host);
    assert(fake.ints.at("sw.state.battle") == 1);
    reader.Sample(host);
    assert(fake.ints.at("sw.state.battle") == 0 && fake.ints.at("sw.state.field") == 1);
    assert(fake.ints.at("sw.battle.own.ready") == 0 && fake.ints.at("sw.battle.bench.count") == 0);
    std::puts("sw_reader: battle page continuity through animations/overlays, active battler from the "
              "client's battle party, bench pokeIDs/fainted flags, battle end on the stopped beat");
}

// ---------------------------------------------------------------------------------------------
// Title/loading/field/battle/menu phase classifier (2026-10-25/26 state-detection task, REVISED
// after an owner game-session review of the first CurrentScreen-only design: see
// sw_reader.h's SampleState header comment, "DESIGN REVISION 2", for the full story). These
// tests drive the classifier using the SAME two independent "save actually loaded" signals the
// shipped function uses -- `SetZoneName` (sw_addresses.h's "location.zone_name" anchor,
// `zone_cache_ready_`) and `AddLiveParty` (sw_addresses.h's "party.array_root" anchor,
// `AnyLivePartySlotValid()`) -- not CurrentScreen alone, which the owner's review correctly
// identified as too noisy at the title screen by itself.
constexpr int kFieldStableTicks = 120;  // mirrors sw_reader.h's own kFieldStableTicks (2s @ 60Hz)
constexpr int kReloadTriggerTicks = 18; // mirrors sw_reader.h's own kReloadTriggerTicks (300ms)

void AssertPhase(FakeHost& fake, bool title, bool loading, bool field, bool battle, bool menu) {
    assert(fake.ints.at("sw.state.ready") == 1);
    assert(fake.ints.at("sw.state.title") == (title ? 1 : 0));
    assert(fake.ints.at("sw.state.loading") == (loading ? 1 : 0));
    assert(fake.ints.at("sw.state.field") == (field ? 1 : 0));
    const bool boot = fake.ints.at("sw.state.boot_loading") == 1;
    assert(!boot || loading);
    assert(fake.ints.at("sw.state.field_page") == (field || menu || (loading && !boot) ? 1 : 0));
    assert(fake.ints.at("sw.state.battle") == (battle ? 1 : 0));
    assert(fake.ints.at("sw.state.menu") == (menu ? 1 : 0));
    assert(fake.ints.at("sw.state.in_battle") == (battle ? 1 : 0)); // back-compat key, same value
}

// TITLE never reacts to CurrentScreen at all any more -- only the zone tag's live validity
// decides it. Party is deliberately NOT part of this decision: live-verified this task (2
// independent boots), the live party array checksum-validates even AT the title screen, since
// the title screen's own cosmetic stadium render decrypts the real save's party to pose it --
// exactly the "falls back to zone tag" case the owner's review itself anticipated. A coincidental
// CurrentScreen sentinel match while the zone is still invalid -- the exact live-caught
// regression the FIRST (CurrentScreen-only) design had -- must never move phase_.
void TestPhaseTitleIgnoresCurrentScreenUntilZoneValid() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    AddLiveParty(fake); // valid even at title -- must not matter for this phase
    AddBattleFixture(fake, /*current_screen=*/0xFFFF5127u, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;

    reader.Sample(host); // zone not set up at all -- DecodeZoneName fails closed, zone invalid
    AssertPhase(fake, /*title=*/true, false, false, false, false);

    // Even the EXACT field sentinel does not move it -- title does not consult CurrentScreen.
    SetCurrentScreen(fake, 0xFFFFFFFFu);
    reader.Sample(host);
    AssertPhase(fake, /*title=*/true, false, false, false, false);

    // An EMPTY zone name (the tag's region present, no real name decided -- DecodeZoneName's own
    // "empty decode" fail-closed path) is ALSO still title -- this is the live-confirmed title
    // screen reading itself (`sw.location.zone_ready` read ABSENT, 2 independent boots).
    SetZoneName(fake, "");
    reader.Sample(host);
    AssertPhase(fake, /*title=*/true, false, false, false, false);

    std::puts("sw_reader: title never consults CurrentScreen (only the zone tag's live validity) "
              "-- a coincidental field/battle sentinel match, and an empty-but-present zone tag, "
              "both stay title; this is the fix for the owner-caught live regression");
}


// Owner Thor 2026-10-31: a new game (no starter yet) stuck on loading while the player could
// walk, then showed a dimmed battle page outdoors. A clean party count of 0 is a valid empty
// party, and the CurrentScreen battle sentinel alone is no battle without a Pokemon.
void TestPhaseNewGameEmptyPartyReachesFieldNeverBattle() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    {
        FakeHost live = BuildLivePartyFixture({}); // root present, count 0
        for (auto& r : live.regions) fake.regions.push_back(std::move(r));
    }
    AddBattleFixture(fake, /*current_screen=*/0xFF000000u, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);
    SetZoneName(fake, "Postwick");
    reader.Sample(host);
    AssertPhase(fake, false, /*loading=*/true, false, false, false);

    SetCurrentScreen(fake, 0xFFFF5127u); // walking around the house
    reader.Sample(host);
    AssertPhase(fake, false, false, /*field=*/true, false, false);

    SetCurrentScreen(fake, 0xFFFFFFFFu); // outdoors: the battle sentinel value, no party
    for (int i = 0; i < kFieldStableTicks + 5; ++i) {
        reader.Sample(host);
        AssertPhase(fake, false, false, /*field=*/true, false, false);
    }

    // A fresh boot straight onto that outdoor reading also reaches field (2 s stable timeout).
    sw_reader::Reader second;
    SetZoneName(fake, "");
    second.Sample(host);
    SetZoneName(fake, "Postwick");
    second.Sample(host);
    for (int i = 0; i < kFieldStableTicks; ++i) second.Sample(host);
    AssertPhase(fake, false, false, /*field=*/true, false, false);

    std::puts("sw_reader: new game with an empty party reaches field and never classifies the "
              "battle sentinel as a battle (owner Thor 2026-10-31)");
}
// title -> loading -> field, the Wild-Area-style instant path: zone becomes valid (Continue
// pressed, load progressing), then the field sentinel confirms arrival immediately once read.
void TestPhaseTitleToLoadToFieldViaFieldSentinel() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    AddLiveParty(fake);
    AddBattleFixture(fake, /*current_screen=*/0xFF000000u, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);
    AssertPhase(fake, true, false, false, false, false);

    SetZoneName(fake, "Wyndon"); // Continue pressed -- the live zone buffer now decodes
    reader.Sample(host);
    AssertPhase(fake, false, /*loading=*/true, false, false, false);
    // The boot load after the title is the one that shows the loading page.
    assert(fake.ints.at("sw.state.boot_loading") == 1 && fake.ints.at("sw.state.field_page") == 0);

    // Still loading (screen still shows the boot-to-world overlay) -- zone alone is not enough,
    // matching the live finding that zone_ready flips ~2-3s into an ~8-9s load.
    reader.Sample(host);
    AssertPhase(fake, false, true, false, false, false);

    SetCurrentScreen(fake, 0xFFFF5127u); // the load visually finishes
    reader.Sample(host);
    AssertPhase(fake, false, false, /*field=*/true, false, false);

    std::puts("sw_reader: title -> loading (zone becomes valid) -> field (field sentinel "
              "confirms instantly), the Wild-Area-style path");
}

// The owner's own flagged bug: a town spawn with NO named CurrentScreen sentinel must still
// reach field, via the 2s party+zone stability timeout -- loading can never stick indefinitely.
void TestPhaseTownSpawnReachesFieldViaStabilityTimeoutNeverSticks() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    AddLiveParty(fake);
    AddBattleFixture(fake, /*current_screen=*/0xFF000000u, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);
    SetZoneName(fake, "Wyndon");
    reader.Sample(host);
    AssertPhase(fake, false, true, false, false, false);

    // The load visually finishes into an ordinary TOWN camera -- no named sentinel at all, ever.
    SetCurrentScreen(fake, 0x0u);
    for (int i = 0; i < kFieldStableTicks - 1; ++i) {
        reader.Sample(host);
        AssertPhase(fake, false, /*loading=*/true, false, false, false); // waiting, not stuck
    }
    reader.Sample(host); // the kFieldStableTicks-th consecutive stable tick commits
    AssertPhase(fake, false, false, /*field=*/true, false, false);

    std::puts("sw_reader: a town spawn with no named CurrentScreen sentinel still reaches field "
              "after 2s of stable party+zone validity -- the owner's 'loading can never stick' "
              "requirement, fixing the previous design's indefinite-stuck-on-loading bug");
}

// Fly/area reload, trigger 1: the live zone-name buffer changing in place (a real Fly/warp
// overwrites the SAME mutable buffer, per sw_zone.h's own header comment) is itself evidence of
// a reload from kField, even while CurrentScreen still reads the field sentinel throughout.
void TestPhaseFlyReloadViaZoneNameChange() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    AddLiveParty(fake);
    AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    PrimeWorldEntered(fake, host, reader); // confirmed field at "Wyndon"
    AssertPhase(fake, false, false, true, false, false);

    SetZoneName(fake, "Hammerlocke"); // the Fly destination's name overwrites the same buffer
    reader.Sample(host);
    AssertPhase(fake, false, /*loading=*/true, false, false, false);
    // Owner Thor 2026-10-31: a location change keeps the field page (no loading page, no
    // field_page edge that would reset the selected page).
    assert(fake.ints.at("sw.state.boot_loading") == 0 && fake.ints.at("sw.state.field_page") == 1);

    // Confirms the new destination the same way any load does.
    for (int i = 0; i < kFieldStableTicks; ++i) {
        reader.Sample(host);
        assert(fake.ints.at("sw.state.field_page") == 1);
    }
    AssertPhase(fake, false, false, /*field=*/true, false, false);

    std::puts("sw_reader: a Fly/warp reload is detected from the live zone name changing in "
              "place, even with no CurrentScreen change at all, and the new destination is "
              "confirmed through the normal loading path");
}

// Fly/area reload, trigger 2: CurrentScreen drifting off the field sentinel for >300ms (the
// owner's other named trigger). REVISED after a live-caught regression (see
// TestPhaseFieldReloadTimerGoesStraightToMenuWhenZoneUnchanged's own comment for the full story):
// when party and zone still confirm the same place, an unnamed camera now stays in field.
// Only an explicit menu signal may change it to menu; neither can reset the selected field page. This trigger still produces a genuine reload
// when the zone is NOT the confirmed place (invalid, or already showing something different) --
// e.g. a loading screen whose zone buffer momentarily reads invalid rather than stale.
void TestPhaseCurrentScreenNonFieldTimeoutGoesToLoadingOnlyWhenZoneIsNotConfirmed() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    AddLiveParty(fake);
    AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    PrimeWorldEntered(fake, host, reader);
    AssertPhase(fake, false, false, true, false, false);

    SetZoneName(fake, ""); // the zone reading becomes invalid (not just "unchanged")
    SetCurrentScreen(fake, 0x12345678u); // unrecognized, NOT the overlay bucket, NOT battle
    for (int i = 0; i < kReloadTriggerTicks - 1; ++i) {
        reader.Sample(host);
        AssertPhase(fake, false, false, /*field=*/true, false, false); // <300ms -> still field
    }
    reader.Sample(host); // the kReloadTriggerTicks-th consecutive non-field tick triggers
    AssertPhase(fake, false, /*loading=*/true, false, false, false);

    std::puts("sw_reader: CurrentScreen drifting off the field sentinel for >300ms triggers a "
              "genuine reload back to loading when the zone is NOT reading the already-confirmed "
              "place (invalid here); a confirmed town camera remains in field instead");
}

// Opening the X-menu/Town Map from the field must be read as kMenu, not swallowed by the
// non-field reload timer above -- sw_reader.h's kField case checks the overlay bucket BEFORE the
// generic reload-timeout branch specifically so this does not happen.
void TestPhaseMenuOverlayFromFieldIsNotMisreadAsReload() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    AddLiveParty(fake);
    AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    PrimeWorldEntered(fake, host, reader);
    AssertPhase(fake, false, false, true, false, false);

    SetCurrentScreen(fake, 0xFF000000u); // X-menu/Town Map
    for (int i = 0; i < kReloadTriggerTicks + 5; ++i) { // well past the reload-timer threshold
        reader.Sample(host);
        AssertPhase(fake, false, false, false, false, /*menu=*/true); // stays menu, never "loading"
    }

    // Closing it straight back to the field, instant (party+zone are already confirmed).
    SetCurrentScreen(fake, 0xFFFF5127u);
    reader.Sample(host);
    AssertPhase(fake, false, false, /*field=*/true, false, false);

    std::puts("sw_reader: opening the X-menu/Town Map from the field reads as menu, not a "
              "reload, even held open well past the reload-timer's own 300ms threshold -- the "
              "overlay-bucket check runs before the generic non-field timer");
}

// A town's unnamed CurrentScreen=0 previously oscillated kField -> kMenu after18ticks,
// then kMenu -> kField after120ticks (2.3s), reopening Town Map through its field bind.
// Same valid party+zone now latch kField until an explicit overlay, battle or zone change.
// Field-page availability also stays1 through a real menu, preserving the user's chosen tab.
void TestPhaseTownCameraDoesNotRepeatFieldPageEdges() {
    FakeHost fake = BuildFixture(false);
    AddLiveParty(fake);
    AddBattleFixture(fake, 0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    PrimeWorldEntered(fake, host, reader);
    SetCurrentScreen(fake, 0);
    for (int i = 0; i < 600; ++i) {
        reader.Sample(host);
        AssertPhase(fake, false, false, true, false, false);
    }
    for (std::uint32_t value : {0x55667788u, 0x99aabbccu, 0xddeeff00u, 0x13579bdfu}) {
        SetCurrentScreen(fake, value);
        for (int i = 0; i < kReloadTriggerTicks * 2; ++i) {
            reader.Sample(host);
            AssertPhase(fake, false, false, true, false, false);
        }
    }
    SetCurrentScreen(fake, 0xFF000000u); // genuinely opens a menu
    reader.Sample(host);
    AssertPhase(fake, false, false, false, false, true);
    SetCurrentScreen(fake, 0); // closes back to town; field_page must remain continuously1
    for (int i = 0; i < kFieldStableTicks + 10; ++i) {
        reader.Sample(host);
        assert(fake.ints.at("sw.state.field_page") == 1);
        assert(fake.ints.at("sw.state.loading") == 0);
    }
    AssertPhase(fake, false, false, true, false, false);
    SetZoneName(fake, "Hammerlocke"); // a genuine Fly/warp still produces a loading edge
    reader.Sample(host);
    AssertPhase(fake, false, true, false, false, false);
    std::puts("sw_reader: confirmed town cameras and native menu open/close preserve field-page selection");
}

// Shield owner 2026-11-07 (Eden Duo 1.3.4, Thor; early game after Hop's battle near Magnolia's
// house): the bottom blinked map <-> empty battle page every ~0.52 s in the field. The eden log
// shows 127 pairs `field_page -> map` / `d.state.command -> battle`, map -> battle within 2 ticks
// and battle -> map after 0.5 s: CurrentScreen read the 0xFFFFFFFF battle sentinel in the field
// (with a party), kField entered kBattle on it, kBattle left after kBattleEndTicks (30) without
// the helper's battle beat, and kField re-entered on the next tick. With the guest helper present
// (field hook beating, no battle client), the sentinel alone must never be a battle: no edge at
// all, field page every tick, through a fresh boot's load too; a real battle (the beat) still
// enters at once and leaves 30 ticks after the beat stops, and stays out.
void TestPhaseHelperFieldBattleSentinelNeverOscillates() {
    FakeHost fake = BuildFixture(false);
    AddLiveParty(fake);
    AddBattleFixture(fake, 0xFFFF5127u, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.SetBattleHint(false, true, 0); // the helper has spoken (field hook), no battle client
    PrimeWorldEntered(fake, host, reader);
    AssertPhase(fake, false, false, true, false, false);
    SetCurrentScreen(fake, 0xFFFFFFFFu); // the early-game field reading
    for (int i = 0; i < 600; ++i) { // 10 s: the old classifier flipped ~19 times here
        reader.SetBattleHint(false, true, 0);
        reader.Sample(host);
        AssertPhase(fake, false, false, true, false, false);
        assert(fake.ints.at("sw.state.screen_battle") == 1);
        assert(fake.ints.at("sw.state.battle_edges") == 0);
    }
    // A real battle: the battle client beats. Entry at once, exit 30 ticks after the beat, while
    // CurrentScreen keeps reading the sentinel throughout.
    const std::uint64_t client = 0x21'9532'EE60ull;
    reader.SetBattleHint(true, true, client);
    reader.Sample(host);
    AssertPhase(fake, false, false, false, true, false);
    for (int i = 0; i < 90; ++i) { reader.SetBattleHint(true, true, client); reader.Sample(host); }
    AssertPhase(fake, false, false, false, true, false);
    FieldRan(reader); // the guest runs the field again (battle over, not a stall)
    reader.SetBattleHint(false, true, 0);
    for (int i = 0; i < 29; ++i) reader.Sample(host);
    AssertPhase(fake, false, false, false, true, false);
    reader.Sample(host);
    AssertPhase(fake, false, false, true, false, false);
    for (int i = 0; i < 600; ++i) {
        reader.SetBattleHint(false, true, 0);
        reader.Sample(host);
        AssertPhase(fake, false, false, true, false, false);
    }
    assert(fake.ints.at("sw.state.battle_edges") == 2); // one battle: in, out -- nothing else
    // A fresh boot straight into that field reading (the log's loading -> battle at 192.4 s):
    // loading resolves to field by the stability timeout, never to battle.
    sw_reader::Reader boot;
    boot.SetBattleHint(false, true, 0);
    SetZoneName(fake, "");
    boot.Sample(host); // title
    SetZoneName(fake, "Route 2");
    for (int i = 0; i < kFieldStableTicks + 60; ++i) {
        boot.SetBattleHint(false, true, 0);
        boot.Sample(host);
        assert(fake.ints.at("sw.state.battle") == 0);
    }
    AssertPhase(fake, false, false, true, false, false);
    assert(fake.ints.at("sw.state.battle_edges") == 0);
    // Without a helper (not installed): the sentinel stays the legacy battle signal.
    sw_reader::Reader legacy;
    SetZoneName(fake, "Wyndon");
    PrimeWorldEntered(fake, host, legacy);
    SetCurrentScreen(fake, 0xFFFFFFFFu);
    legacy.Sample(host);
    AssertPhase(fake, false, false, false, true, false);
    std::puts("sw_reader: with the guest helper the CurrentScreen battle sentinel in the field never "
              "enters battle (no map/battle oscillation, Shield owner 2026-11-07); the helper's beat "
              "enters/leaves once");
}

// Desktop 2026-11-07 (slow GPU, Battle Tower): the battle page fell to the map for ~1 s every
// 30-60 s mid-battle -- the game stalled, the battle beat went silent past its window, and the
// 30-tick exit fired. A silent battle beat with no field frames is a stall, not the end of the
// battle; the field frames (or 5 s of silence) end it.
void TestPhaseBattleStallKeepsBattle() {
    FakeHost fake = BuildFixture(false);
    AddLiveParty(fake);
    AddBattleFixture(fake, 0xFFFF5127u, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.SetBattleHint(false, true, 0);
    PrimeWorldEntered(fake, host, reader);
    const std::uint64_t client = 0x21'9532'EE60ull;
    SetCurrentScreen(fake, 0);
    reader.SetBattleHint(true, true, client);
    reader.Sample(host);
    AssertPhase(fake, false, false, false, true, false);
    reader.SetBattleScreen(1); // the move page is up
    reader.SetBattleHint(false, true, 0); // stall: no battle beat, no field frame either
    for (int i = 0; i < 299; ++i) {
        reader.SetBattleHint(false, true, 0);
        reader.Sample(host);
        AssertPhase(fake, false, false, false, true, false);
        assert(reader.battle_client() == client && reader.battle_screen() == 1); // page state kept
    }
    reader.SetBattleHint(true, true, client); // the game resumes the battle
    reader.Sample(host);
    AssertPhase(fake, false, false, false, true, false);
    assert(fake.ints.at("sw.state.battle_edges") == 1);
    reader.SetBattleHint(false, true, 0); // a stall that never ends: out after 5 s, never stuck
    for (int i = 0; i < 299; ++i) reader.Sample(host);
    AssertPhase(fake, false, false, false, true, false);
    reader.Sample(host);
    AssertPhase(fake, false, false, true, false, false);
    // A real end: the field runs -> out after the 30 ticks as before.
    reader.SetBattleHint(true, true, client);
    reader.Sample(host);
    FieldRan(reader);
    reader.SetBattleHint(false, true, 0);
    for (int i = 0; i < 29; ++i) reader.Sample(host);
    AssertPhase(fake, false, false, false, true, false);
    reader.Sample(host);
    AssertPhase(fake, false, false, true, false, false);
    std::puts("sw_reader: an emulation stall (silent battle beat, no field frames) keeps the battle "
              "page; field frames or 5 s of silence end the battle");
}

// Defensive backstop: kLoading itself must ALSO be able to recover to menu from zone+party
// validity alone (independent of kField's own reload-timer fix above), for the case where the
// zone reading reverts back to the place confirmed before this exact kLoading episode started
// (e.g. a torn/transient read, or any other path reaching kLoading this test file does not
// specifically exercise) -- kLoading itself must never be a dead end on its own.
void TestPhaseLoadingItselfRecoversFromZonePartyValidityAlone() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    AddLiveParty(fake);
    AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    PrimeWorldEntered(fake, host, reader); // confirmed field at "Wyndon"
    AssertPhase(fake, false, false, true, false, false);

    // A genuine zone change (kField's own instant, unconditional check) correctly enters
    // kLoading to confirm the new destination -- confirmed_zone_name_ is still the OLD place
    // ("Wyndon") until a fresh kField commit updates it.
    SetZoneName(fake, "Hammerlocke");
    reader.Sample(host);
    AssertPhase(fake, false, /*loading=*/true, false, false, false);

    // The zone reading reverts back to the place confirmed before this episode started (a torn/
    // transient read reverting, or any other real-world cause) -- kLoading recognizes this is no
    // longer evidence of a real, in-progress reload and recovers to menu directly from zone+party
    // validity, with CurrentScreen still an unrecognized value throughout.
    SetCurrentScreen(fake, 0x42424242u);
    SetZoneName(fake, "Wyndon");
    reader.Sample(host);
    AssertPhase(fake, false, false, false, false, /*menu=*/true);

    std::puts("sw_reader: kLoading itself still recovers to menu from zone+party validity alone "
              "(a defensive backstop, independent of CurrentScreen) if the zone reading reverts "
              "back to the place already confirmed before this loading episode began");
}

// Battle: the original proven instant toggle (both directions), now reached through the new
// title/loading path instead of the retired `world_entered_` flag, plus NEW debounce protection
// against a single torn/unclassified tick on the way OUT (see sw_reader.h's kBattle case
// comment) -- an instant exit on ANY non-0xFFFFFFFF reading would have silently broken the
// ORIGINAL battle<->field fix's own sticky-through-an-unclassified-tick guarantee.
void TestPhaseBattleInstantToggleAndStickyThroughOneUnclassifiedTick() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    fake.personal[25] = {12, 12};
    AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    AddLiveParty(fake);
    PrimeWorldEntered(fake, host, reader); // confirmed field first (battle always follows field)
    AssertPhase(fake, false, false, true, false, false);

    SetCurrentScreen(fake, 0xFFFFFFFFu); // the real edge into battle -- instant
    reader.Sample(host);
    AssertPhase(fake, false, false, false, /*battle=*/true, false);
    assert(fake.ints.at("sw.battle.own.ready") == 1);

    SetCurrentScreen(fake, 0x7u); // a single unclassified/torn tick mid-battle
    reader.Sample(host);
    AssertPhase(fake, false, false, false, /*battle=*/true, false); // still battle, not field

    SetCurrentScreen(fake, 0xFFFF5127u); // the real edge back out -- instant
    reader.Sample(host);
    AssertPhase(fake, false, false, /*field=*/true, false, false);
    assert(fake.ints.at("sw.battle.own.ready") == 0);

    std::puts("sw_reader: battle<->field stays an instant toggle on the named sentinels, and a "
              "single unclassified mid-battle tick does not flip it (the original fix's own "
              "guarantee, now re-verified reached via the title/loading path instead of the "
              "retired world_entered_ flag)");
}

void TestBattleReaderOutsideBattleFailsClosed() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    // CurrentScreen reads the FIELD sentinel this time; the battle struct bytes are still present
    // (as they would be on real hardware -- stale from a previous battle, or never written this
    // boot) but must never be shown, since the game is not actually in a battle right now.
    AddBattleFixture(fake, /*current_screen=*/0xFFFF5127u, /*own_cur_hp=*/200, /*own_max_hp=*/356,
                      /*foe_species=*/25, /*foe_level=*/55, /*foe_cur_hp=*/40, /*foe_max_hp=*/120);
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);

    assert(fake.ints.at("sw.state.ready") == 1);
    assert(fake.ints.at("sw.state.in_battle") == 0);
    assert(fake.ints.at("sw.battle.own.ready") == 0);
    assert(fake.ints.at("sw.battle.foe.ready") == 0);
    assert(fake.ints.find("sw.battle.own.species") == fake.ints.end());
    assert(fake.ints.find("sw.battle.foe.species") == fake.ints.end());

    std::puts("sw_reader: the battle struct is never published while CurrentScreen reads field, "
              "even though stale bytes are present at the address");
}

void TestBattleReaderInvalidStructFailsClosed() {
    FakeHost fake = BuildFixture(/*corrupt_checksum=*/false);
    // CurHP > MaxHP is impossible for a real battle state -- a torn read or a stale/garbage
    // struct, not a real one. Level 255 (the all-zero-then-overwritten default) is also out of
    // the valid 1..100 range on its own, doubly invalid.
    AddBattleFixture(fake, /*current_screen=*/0xFFFFFFFFu, /*own_cur_hp=*/999, /*own_max_hp=*/356,
                      /*foe_species=*/25, /*foe_level=*/55, /*foe_cur_hp=*/40, /*foe_max_hp=*/120);
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    AddLiveParty(fake);
    PrimeWorldEntered(fake, host, reader); // the title gate requires leaving title for real first
    SetCurrentScreen(fake, 0xFFFFFFFFu);   // PrimeWorldEntered left current_screen at the field sentinel
    reader.Sample(host);

    assert(fake.ints.at("sw.state.in_battle") == 1); // classification itself is unaffected
    assert(fake.ints.at("sw.battle.own.ready") == 0); // but the impossible own struct is refused
    assert(fake.ints.at("sw.battle.foe.ready") == 1); // the foe struct is independently valid
    assert(fake.ints.find("sw.battle.own.species") == fake.ints.end());

    std::puts("sw_reader: an impossible battle struct (CurHP > MaxHP) is refused, not published, "
              "independently of the other side");
}

// --------------------------------------------------------------------------------------- Dens +
// current zone. Builds a synthetic heap for sw_addresses.h's "location.zone_name" + the 3
// "dens.*" anchors: the real 8-byte tag + a UTF-16LE zone name at the zone-name anchor, plus all
// 3 den region blocks at their own real (111-slot) capacity. `galar_slot_overrides` places den
// slots (g_den_galar*: synthetic, or with --private-data the gb-635571 fixture save's own) at
// specific slot indices within the Galar block; every other slot/region stays
// all-zero, matching a real empty den slot's own DenType=0 encoding.
FakeHost BuildZoneAndDensFixture(const std::string& zone_name,
                                  const std::map<int, const unsigned char*>& galar_slot_overrides) {
    const auto* a_zone = sw_addresses::Find("location.zone_name");
    const auto* a_galar = sw_addresses::Find("dens.vanilla");
    const auto* a_ioa = sw_addresses::Find("dens.isle_of_armor");
    const auto* a_ct = sw_addresses::Find("dens.crown_tundra");
    assert(a_zone && a_galar && a_ioa && a_ct);
    assert(a_zone->verified && a_galar->verified && a_ioa->verified && a_ct->verified);

    FakeHost fake;
    fake.heap_begin = 0x2105400000ULL; // same real HeapLow() value BuildFixture uses
    // The zone names are the player's romfs place_name lines (sw_zone kZonePlaceLine); this
    // fixture's English table names two zones.
    std::vector<std::string> places(249, "Elsewhere");
    places[static_cast<std::size_t>(sw_zone::kZonePlaceLine[0])] = "Rolling Fields";
    places[static_cast<std::size_t>(sw_zone::kZonePlaceLine[2])] = "Watchtower Ruins";
    fake.dat_files[sw_lang::MessagePath(sw_lang::Lang::English, "place_name.dat", "script")] = GameMessageFixture(places);

    std::vector<unsigned char> zone_buf(a_zone->size, 0);
    std::memcpy(zone_buf.data(), sw_zone::kZoneTag, sizeof sw_zone::kZoneTag);
    for (std::size_t i = 0; i < zone_name.size(); ++i)
        PutU16(zone_buf, 8 + i * 2, static_cast<std::uint16_t>(static_cast<unsigned char>(zone_name[i])));
    fake.regions.push_back({fake.heap_begin + a_zone->sysbot_offset, std::move(zone_buf)});

    std::vector<unsigned char> galar_buf(a_galar->size, 0);
    for (const auto& [slot, bytes] : galar_slot_overrides)
        std::memcpy(galar_buf.data() + slot * 0x18, bytes, 0x18);
    fake.regions.push_back({fake.heap_begin + a_galar->sysbot_offset, std::move(galar_buf)});

    fake.regions.push_back({fake.heap_begin + a_ioa->sysbot_offset, std::vector<unsigned char>(a_ioa->size, 0)});
    fake.regions.push_back({fake.heap_begin + a_ct->sysbot_offset, std::vector<unsigned char>(a_ct->size, 0)});
    return fake;
}

void TestDensMatchCurrentZoneFromRealSaveBytes() {
    // "Watchtower Ruins" is Galar den slot 12's real zone (sw_zone::kGalarDenZone[12] == 2, the
    // romfs name at kZonePlaceLine[2]) -- slot 12's bytes are the real save's own Rare/4-star den
    // (LIVE-READERS-132.md's 2026-10-25 "Raid dens" session; tools/decode_dens.py reproduces the
    // identical hash/stars/type for this exact fixture). Slot 20 (West Lake Axewell, a DIFFERENT
    // zone, also real and active in the same save) must NOT show up here -- proving the zone
    // filter, not just "any active den in the block", drives the strip.
    FakeHost fake = BuildZoneAndDensFixture(
        "Watchtower Ruins", {{12, g_den_galar12}, {20, g_den_galar20}});
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);

    assert(fake.ints.at("sw.location.zone_ready") == 1);
    assert(fake.texts.at("sw.location.zone_name") == "Watchtower Ruins");
    assert(fake.texts.at("sw.location.region") == "Galar");

    assert(fake.ints.at("sw.dens.has_any") == 1);
    assert(fake.ints.at("sw.dens.0.ready") == 1);
    assert(fake.ints.at("sw.dens.0.stars") == 4);
    assert(fake.ints.at("sw.dens.0.den_type") == 2); // Rare
    assert(fake.texts.at("sw.dens.0.stars_text") == "5"); // the raw 0-based rank 4 = five stars
    assert(fake.texts.at("sw.dens.0.icon_src").find("townmap_icon_dungeon_00^q") != std::string::npos);
    // West Lake Axewell's den (slot 20) is real and active in the same block, but belongs to a
    // DIFFERENT zone -- must not appear as a second strip slot.
    assert(fake.ints.find("sw.dens.1.ready") == fake.ints.end());
    // RaidSpawnDetail has no species field at all -- structurally spoiler-safe, not something
    // this function has to enforce, but worth asserting nothing calling itself a den ever
    // mentions one.
    for (const auto& [k, v] : fake.texts) {
        (void)v;
        assert(k.find("species") == std::string::npos || k.rfind("sw.dens", 0) != 0);
    }

    std::puts("sw_reader: den strip shows only the current zone's own real save-decoded den "
              "(Watchtower Ruins, Rare 4-star), not an active den belonging to a different zone");
}

void TestDensHiddenForZoneWithNoActiveDens() {
    // Rolling Fields (Galar Location 0) is a real zone, but this fixture's own save byte at slot
    // 0 (the real, confirmed-inactive DenType==0 save bytes) has no active den there.
    FakeHost fake = BuildZoneAndDensFixture("Rolling Fields", {{0, g_den_galar0}});
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);

    assert(fake.texts.at("sw.location.zone_name") == "Rolling Fields");
    assert(fake.ints.at("sw.dens.has_any") == 0);
    assert(fake.ints.find("sw.dens.0.ready") == fake.ints.end());

    std::puts("sw_reader: a real zone with no active den in this save publishes has_any=0 (strip "
              "hidden), not an empty placeholder slot");
}

void TestDensAndRegionOmittedForUnmappedZone() {
    // "Wyndon" is a real, live-verified place name (LIVE-READERS-132.md) but not a Wild Area/
    // Isle of Armor/Crown Tundra sub-zone -- no region subtitle, no den strip, even though an
    // active, real den (slot 12) exists elsewhere in the same block and the zone name itself
    // reads fine.
    FakeHost fake = BuildZoneAndDensFixture("Wyndon", {{12, g_den_galar12}});
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);

    assert(fake.texts.at("sw.location.zone_name") == "Wyndon");
    assert(fake.ints.find("sw.location.region_ready") == fake.ints.end());
    assert(fake.texts.find("sw.location.region") == fake.texts.end());
    assert(fake.ints.at("sw.dens.has_any") == 0);

    std::puts("sw_reader: a town (Wyndon) omits the region subtitle and hides the den strip "
              "entirely, never a guessed region or an unrelated zone's den");
}

void TestZoneTagMismatchFailsClosedAndHidesDens() {
    FakeHost fake = BuildZoneAndDensFixture("Watchtower Ruins", {{12, g_den_galar12}});
    // Corrupt the 8-byte tag in place -- a real buffer with a different record layout, or a
    // torn/garbage read, must never be decoded as if it were a trusted zone name.
    const auto* a_zone = sw_addresses::Find("location.zone_name");
    for (auto& r : fake.regions)
        if (r.base == fake.heap_begin + a_zone->sysbot_offset) r.data[0] = 0x00;
    const EdenDsmodHostApi host = MakeHostApi(fake);

    sw_reader::Reader reader;
    reader.Sample(host);

    assert(fake.ints.find("sw.location.zone_ready") == fake.ints.end());
    assert(fake.texts.find("sw.location.zone_name") == fake.texts.end());
    // Dens must fail closed too: no current zone this tick -> nothing to match -> strip hidden,
    // never "fall back to showing every active den regardless of zone".
    assert(fake.ints.at("sw.dens.has_any") == 0);
    assert(fake.ints.find("sw.dens.0.ready") == fake.ints.end());

    std::puts("sw_reader: a corrupted zone-name tag fails closed (no zone published), and the "
              "den strip correctly has nothing to match against rather than showing an "
              "unfiltered den");
}

} // namespace

// Pokédex detail depth (sw_dex_info.h): a synthetic personal record (made-up stats and ability ids,
// sw_dex_info.h's field layout) decodes every field the detail page shows.
void TestDexInfoRecord() {
    std::uint8_t r[0xB0]{};
    const std::uint8_t head[0x18] = {50, 60, 70, 80, 90, 100, 11, 3, 120, 1, 0, 1, 0, 0, 0, 0, 0, 0, 31, 20, 50, 3, 1, 7};
    std::memcpy(r, head, sizeof head);
    r[0x18] = 20; r[0x1A] = 20; r[0x1C] = 30; r[0x22] = 100;
    sw_dex_info::Record d;
    assert(sw_dex_info::Decode(r, d));
    assert(d.base[0] == 50 && d.base[4] == 90 && d.catch_rate == 120 && d.base_exp == 100);
    assert(sw_dex_info::EvYield(d) == std::string("1 ") + sw_str::S(sw_str::Id::kStatSpAtk));
    assert(sw_dex_info::EggGroups(d) == "Monster / Grass");
    assert(sw_dex_info::Gender(d.gender) == "87.5% male / 12.5% female");
    assert(sw_dex_info::Gender(255) == "Genderless" && sw_dex_info::Gender(127) == "50% male / 50% female");
    assert(std::string(sw_dex_info::GrowthName(d.growth)) == "Medium Slow");
    std::vector<std::string> names(70);
    names[20] = "Ability A"; names[30] = "Ability B";
    std::string reg, hid;
    sw_dex_info::Abilities(d, names, reg, hid);
    assert(reg == "Ability A" && hid == "Ability B");
    std::uint8_t empty[0xB0]{};
    assert(!sw_dex_info::Decode(empty, d)); // absent species: fail closed
    std::puts("dex info record decode ok");
}

// dev.17 live catch: after the native exchange moved the party's +0x89 index byte (2 after a
// 0<->2 reorder), the count must still read as the BYTE at +0x88 -- never u64 0x203.
void TestLivePartyCountIsByte() {
    FakeHost fake = BuildLivePartyFixture({g_mewtwo, g_eternatus, g_zacian}, 2);
    fake.personal[150] = {13, 13, 106, 5};
    fake.personal[890] = {9, 15, 140, 5};
    fake.personal[888] = {17, 17, 92, 5};
    const EdenDsmodHostApi host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.Sample(host);
    assert(fake.ints.at("sw.party.count") == 3);
    assert(fake.ints.count("sw.party.0.current_hp") == 1);
    std::puts("live party count read as a byte (+0x89 index ignored) ok");
}

namespace {
void TestBattleGauge() {
    using namespace sw_battle_gauge;
    std::uint8_t obj[0x40]{};
    const std::uint32_t flags = 0x101;
    std::memcpy(obj + 0x08, &flags, 4);
    const std::uint32_t max = 211, target = 187, w = 488, h = 248;
    std::memcpy(obj + 0x20, &max, 4);
    std::memcpy(obj + 0x24, &target, 4);
    std::memcpy(obj + 0x30, &w, 4);
    std::memcpy(obj + 0x38, &h, 4);
    Plate p;
    assert(ParsePlate(obj, p) && p.max == 211 && p.target == 187);
    obj[0x30] = 0;
    assert(!ParsePlate(obj, p));
    assert(Step(211, 187, -24) == 210 && Step(188, 187, -24) == 187 && Step(-1, 50, 0) == 50);
    assert(Step(211, 0, -211) == 209 && Step(10, 20, 10) == 11);
}
} // namespace

// Route list (owner 2026-11-01): the Town Map "Wild Pokemon" row follows the current zone, the
// grid opens on the live weather ("Now"), a weather chip switches the table (live Sunshine =
// encounter table 4: Vulpix), names come from the companion language's monsname table, levels
// floor at Lv. 60 once the Hall of Fame block is set, and a place without a table hides the row.
struct RouteWorld {
    std::uint64_t registry_row = 0, parameters = 0;
};
RouteWorld AddRouteWorld(FakeHost& fake, std::uint64_t zone, std::uint32_t weather_id) {
    fake.main_base = 0x80000000;
    const auto slot = fake.heap_begin + 0xB0000000ull;
    const auto global = slot + 0x1000, manager = slot + 0x2000;
    const auto weather = slot + 0x3000, parameters = slot + 0x4000;
    const auto world_slot = slot + 0x10000, world = world_slot + 0x1000;
    const auto player = slot + 0x100000, vtable = player + 0x1000;
    const auto wrapper = player + 0x2000, component = player + 0x3000, vector = player + 0x4000;
    auto add_pointer = [&](std::uint64_t address, std::uint64_t value) {
        std::vector<unsigned char> bytes(8); PutU64(bytes, 0, value);
        fake.regions.push_back({address, std::move(bytes)});
    };
    add_pointer(fake.main_base + 0x2610848, slot); add_pointer(slot, global);
    std::vector<unsigned char> gl(0x98), man(0x410), we(0x400), pa(0x64);
    PutU64(gl, 0x90, manager); PutU64(man, 0x408, weather); PutU64(we, 0x3F8, parameters);
    PutU32(pa, 0x60, weather_id);
    fake.regions.push_back({global, gl}); fake.regions.push_back({manager, man});
    fake.regions.push_back({weather, we}); fake.regions.push_back({parameters, pa});
    add_pointer(fake.main_base + 0x26194D8, world_slot); add_pointer(world_slot, world);
    std::vector<unsigned char> wo(0xC0), ob(0x600), wrap(0x68), comp(0x499), objects(16);
    PutU64(wo, 0xB0, vector); PutU64(wo, 0xB8, vector + 16);
    PutU64(ob, 0x50, vtable); PutU64(ob, 0x588, wrapper);
    std::array<float, 3> xyz{18945.25f, 1724.5f, 53708.75f};
    std::memcpy(ob.data() + 0xB0, xyz.data(), sizeof xyz);
    PutU64(wrap, 0x60, component); PutU64(objects, 0, player);
    fake.regions.push_back({world, wo}); fake.regions.push_back({player, ob});
    fake.regions.push_back({wrapper, wrap}); fake.regions.push_back({component, comp});
    fake.regions.push_back({vector, objects}); add_pointer(vtable, fake.main_base + 0xDA2490);
    const auto registry_slot = slot + 0x20000, registry = registry_slot + 0x1000;
    const auto registry_vector = registry_slot + 0x2000, registry_row = registry_slot + 0x3000;
    add_pointer(fake.main_base + 0x2617C48, registry_slot); add_pointer(registry_slot, registry);
    std::vector<unsigned char> reg(0x158), row(0x3A8);
    PutU64(reg, 0x148, registry_vector); PutU64(reg, 0x150, registry_vector + 8);
    PutU64(row, 0x3A0, zone);
    fake.regions.push_back({registry, reg}); fake.regions.push_back({registry_row, row});
    add_pointer(registry_vector, registry_row);
    fake.dat_files[sw_town_map::kLayoutPath] = NativeMapLayoutFixture();
    return {registry_row, parameters};
}

void TestRouteListPipeline() {
    sw_test_encounters::Fixtures(false,"z_wr0102",true);
    FakeHost fake = BuildFixture(false);
    AddLiveParty(fake);
    AddBattleFixture(fake, 0xFFFFFFFFu, 200, 356, 25, 55, 40, 120);
    const auto rolling = sw_town_map::ZoneHash("z_wr0102");
    for (const auto& [path,bytes] : sw_test_encounters::files) fake.dat_files[path]=bytes;
    const auto world = AddRouteWorld(fake, rolling, 1); // standing in Rolling Fields, Cloudy
    std::vector<std::string> en(900, "Mon"), fr(900, "Pkmn");
    en[37] = "Vulpix"; fr[37] = "Goupix";
    en[819] = "Skwovet"; fr[819] = "Rongourmand";
    fake.dat_files[sw_lang::MessagePath(sw_lang::Lang::English, "monsname.dat")] = GameMessageFixture(en);
    fake.dat_files[sw_lang::MessagePath(sw_lang::Lang::French, "monsname.dat")] = GameMessageFixture(fr);
    const auto host = MakeHostApi(fake);
    sw_reader::Reader reader;
    reader.SetLanguage(sw_lang::Lang::English);
    PrimeWorldEntered(fake, host, reader);
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    sw_route_encounters::Data encounters;
    assert(sw_route_encounters::Load(host,encounters));
    const auto* zone = sw_route::Find(encounters,rolling);
    assert(zone && fake.ints.at("sw.map.wild.ready") == 1);
    assert(fake.ints.at("sw.map.wild.place_ready") == 0); // the card's title already names it
    const int count = static_cast<int>(sw_route::Rows(encounters,*zone, 1).size());
    assert(fake.ints.at("sw.map.wild.count") == count && count > 5);
    assert(fake.ints.at("sw.map.wild.more_ready") == 1 && fake.texts.at("sw.map.wild.more") == "+" + std::to_string(count - 5));
    assert(fake.texts.at("sw.map.wild.strip.0.icon_src").find("module:sw:ui/form/") != std::string::npos);
    assert(fake.ints.at("sw.route.list_open") == 0 && fake.ints.at("sw.route.ready") == 0);
    assert(fake.ints.at("sw.progress.champion_known") == 0); // no Hall of Fame block in this heap

    assert(reader.DispatchAction("map_wild", 0));
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.route.list_open") == 1 && fake.ints.at("sw.route.ready") == 1);
    assert(fake.ints.at("sw.route.weather.shown") == 1 && fake.ints.at("sw.route.weather.count") == 9);
    assert(fake.ints.at("sw.route.weather.1.selected") == 1 && fake.ints.at("sw.route.weather.1.now") == 1);
    assert(fake.ints.at("sw.route.weather.0.selected") == 0 && fake.ints.at("sw.route.weather.0.now") == 0);
    assert(fake.texts.at("sw.route.weather.name") == "Overcast");
    assert(fake.texts.at("sw.route.weather.4.icon_src").find("weather_icon_s_07^t") != std::string::npos);
    assert(fake.ints.at("sw.route.count") == count);
    auto find_row = [&](const std::string& name) {
        for (int i = 0; i < fake.ints.at("sw.route.count"); ++i)
            if (fake.texts.at("sw.route." + std::to_string(i) + ".name") == name) return i;
        return -1;
    };
    assert(find_row("Vulpix") < 0);
    // Chip 4 = Intense Sun (table 4) = live 6: Vulpix wanders there, table levels before the HoF.
    assert(reader.DispatchAction("route_weather", 4));
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.route.weather.4.selected") == 1 && fake.ints.at("sw.route.weather.1.now") == 1);
    assert(fake.texts.at("sw.route.weather.name") == "Intense Sun");
    const int vulpix = find_row("Vulpix");
    assert(vulpix >= 0);
    const auto p = "sw.route." + std::to_string(vulpix) + ".";
    assert(fake.texts.at(p + "method").starts_with("Wanders ") && fake.texts.at(p + "method").ends_with("%"));
    assert(fake.texts.at(p + "level").starts_with("Lv. ") && fake.texts.at(p + "level") != "Lv. 60");
    assert(fake.texts.at(p + "icon_src").find("module:sw:ui/form/37/0") != std::string::npos);
    assert(fake.ints.at(p + "caught") == 0); // no Pokedex blocks in this heap: never a guessed ball
    assert(!reader.DispatchAction("route_weather", 9));
    // Hall of Fame entered: Wild Area wild Pokemon are Lv. 60 or more.
    const auto* hof = sw_addresses::Find("progress.hall_of_fame");
    assert(hof && hof->verified);
    std::vector<unsigned char> when(8); PutU64(when, 0, 0x5DD06865ull);
    fake.regions.push_back({fake.heap_begin + hof->sysbot_offset, when});
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.progress.champion_known") == 1 && fake.ints.at("sw.progress.champion") == 1);
    assert(fake.texts.at(p + "level") == "Lv. 60");
    // The companion language names the species and the method.
    reader.SetLanguage(sw_lang::Lang::French);
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.texts.at(p + "name") == "Goupix");
    assert(fake.texts.at(p + "method").starts_with("Errant ") && fake.texts.at(p + "level") == "N. 60");
    assert(fake.texts.at("sw.route.weather.name") == "Soleil intense");
    reader.SetLanguage(sw_lang::Lang::English);
    // Back closes the grid; a town without a table hides the Town Map row.
    assert(reader.DispatchAction("route_back", 0));
    for (auto& region : fake.regions)
        if (region.base == world.registry_row) PutU64(region.data, 0x3A0, sw_town_map::ZoneHash("z_t0101"));
    fake.ints.clear(); fake.texts.clear(); reader.Sample(host);
    assert(fake.ints.at("sw.route.list_open") == 0 && fake.ints.at("sw.route.ready") == 0);
    assert(fake.ints.at("sw.map.wild.ready") == 0);
    assert(!reader.DispatchAction("map_wild", 0));
    std::puts("sw_reader: route list follows the zone, opens on the live weather, switches tables, "
              "floors post-game levels and names species in the companion language");
}

// The Shield twin (dsmod-pokemon-shield-reader-live, SW_VARIANT_SHIELD=1) runs every test on the
// Shield record except the few whose fake memory is laid out at Sword 1.3.2's literal main offsets.
#if defined(SW_VARIANT_SHIELD)
#define SWORD_ONLY(call) ((void)0)
#else
#define SWORD_ONLY(call) call
#endif
void RunAll() {
    SWORD_ONLY(TestBattleGauge());
    TestLivePartyCountIsByte();
    TestDexInfoRecord();
    TestFullPipelineAgainstLiveVerifiedValues();
    TestLiveStoredPartyChainReadsThreeRealMembers();
    TestPartyShinyFromPk8();
    TestLivePartySixMembers();
    TestPartyDetailSelectionFreshnessAndPrivacy();
    TestUnchangedTickReplaysIdenticalKeys();
    TestLanguageChangeReloadsAndRepublishes();
    TestLivePartyFailedSlotNeverFallsBackToMirror();
    TestLivePartyReflectsAChangeOnTheVeryNextSample();
    TestLivePartySlotWithBadChecksumSkipped();
    TestEggSlotNeverRevealsContents();
    TestPartyNicknameDecoded();
    TestCorruptChecksumFailsClosed();
    TestMisc8OutOfRangeFailsClosed();
    TestSpeciesTypesMissingRomfsFailsClosed();
    TestBattleReaderInWildBattle();
    SWORD_ONLY(TestBattleBallIdentityPagingAndViewReset());
    SWORD_ONLY(TestNativeBattleMenuFsmClassification());
    TestInBattleStickyAcrossUnclassifiedTick();
    TestBattleHintFromHelperAndBattleView();
    TestBattleContinuityActiveAndBench();
    TestBattleRootFromClientPartyInstitution();
    TestBattleForeignActiveFoeFollowsRosterParty();
    TestBattleDoublesActives();
    TestBattleTwoFoeParties();
    TestBattleMultiPartner();
    TestTargetCardNamesEveryDoublesKind();
    TestTargetSelectHoldsThroughReadFailures();
    TestBattleTwoFoePartiesUnknownModeFailsClosed();
    TestWildShinyCachedPerBattle();
    SWORD_ONLY(TestNativeRaidRosterAndPublishPipeline());
    TestDmaxPreviewPreservesNativeActionIdentity();
    TestDexDiscoveryDoesNotRevealUnknownStagesOrDlcTotals();
    TestBattleFieldDecodeAndNativeEnumMap();
    TestBattleFieldPointerAndPublishPipeline();
    TestLivePartyMovesForPpMedicinePicker();
    SWORD_ONLY(TestOverworldWeatherPositionAndBikePipeline());
    TestNativeMapGeometryAndHash();
    TestPhaseTitleIgnoresCurrentScreenUntilZoneValid();
    TestPhaseTitleToLoadToFieldViaFieldSentinel();
    TestPhaseTownSpawnReachesFieldViaStabilityTimeoutNeverSticks();
    TestPhaseNewGameEmptyPartyReachesFieldNeverBattle();
    TestPhaseFlyReloadViaZoneNameChange();
    TestPhaseCurrentScreenNonFieldTimeoutGoesToLoadingOnlyWhenZoneIsNotConfirmed();
    TestPhaseMenuOverlayFromFieldIsNotMisreadAsReload();
    TestPhaseTownCameraDoesNotRepeatFieldPageEdges();
    TestPhaseHelperFieldBattleSentinelNeverOscillates();
    TestPhaseBattleStallKeepsBattle();
    TestPhaseLoadingItselfRecoversFromZonePartyValidityAlone();
    TestPhaseBattleInstantToggleAndStickyThroughOneUnclassifiedTick();
    TestBattleReaderOutsideBattleFailsClosed();
    TestBattleReaderInvalidStructFailsClosed();
    TestDensMatchCurrentZoneFromRealSaveBytes();
    TestDensHiddenForZoneWithNoActiveDens();
    TestDensAndRegionOmittedForUnmappedZone();
    TestZoneTagMismatchFailsClosedAndHidesDens();
    SWORD_ONLY(TestRouteListPipeline()); // Shield: dsmod-pokemon-shield-route
    std::puts("0100ABF008968000_reader_test: all checks passed");
}

int main(int argc, char** argv) {
    // --private-data: the whole suite again on the real fixture save's bytes (SW_FIXTURES_DIR,
    // sw_test_fixtures.h); skipped (exit 77) when they are not set up.
    if (argc > 1 && std::strcmp(argv[1], "--private-data") == 0) {
        if (!sw_test::PrivateDataAvailable()) return sw_test::kSkip;
        LoadPrivateFixtures();
        RunAll();
        std::puts("0100ABF008968000_reader_test --private-data: all checks passed on the real fixture bytes");
        return 0;
    }
    FillSyntheticFixtures();
    RunAll();
    return 0;
}
