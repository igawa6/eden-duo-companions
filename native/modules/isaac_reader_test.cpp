// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Unit tests for the Isaac reader's pure rules. No game data: the seed alphabet
// is a synthetic 32-letter one; the expected strings are the live pause-screen
// seeds (verify/REPORT.md, H1LZ V9ZP / CHXE EQJE) mapped letter by letter into
// it.
//
// Integration fixtures use the owner's own files (nothing copyrighted in git):
//   ISAAC_ABP_IMAGE  AfterbirthPlus.nro      ISAAC_REP_IMAGE  Repentance.nro
//   ISAAC_ROMFS      merged program romfs    ISAAC_FILE       package dir (eid_*.dat)
// A missing fixture is reported as SKIPPED and fails the run, unless
// ISAAC_ALLOW_MISSING_FIXTURES=1, which exits 77 (ctest: Skipped) instead.

#include "core/mods/dsmod_module_abi.h"
#include "isaac_reader.h"
#include "isaac_text.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace isaac_reader::detail;

// Serves "romfs:" from ISAAC_ROMFS and "file:" from ISAAC_FILE.
std::size_t FixtureRomfs(void*, const char* path, std::uint64_t offset, void* out,
                         std::size_t size) {
    const std::string p{path};
    const char* root = nullptr;
    std::size_t skip = 0;
    if (p.starts_with("romfs:")) {
        root = std::getenv("ISAAC_ROMFS");
        skip = 6;
    } else if (p.starts_with("file:")) {
        root = std::getenv("ISAAC_FILE");
        skip = 5;
    }
    if (!root)
        return 0;
    std::ifstream in(std::string{root} + "/" + p.substr(skip), std::ios::binary | std::ios::ate);
    if (!in)
        return 0;
    const auto total = static_cast<std::size_t>(in.tellg());
    if (!out)
        return total;
    if (offset > total)
        return 0;
    const auto n = std::min(size, total - static_cast<std::size_t>(offset));
    in.seekg(offset);
    in.read(static_cast<char*>(out), n);
    return in.gcount();
}

bool TextFixture() {
    return std::getenv("ISAAC_ROMFS") && std::getenv("ISAAC_FILE");
}

// Optional integration fixture using the owner's AB+ NRO image. No copyrighted
// data in git. Returns false when the image is not supplied.
bool AfterbirthFixture() {
    const char* path = std::getenv("ISAAC_ABP_IMAGE");
    if (!path)
        return false;
    struct Fixture {
        std::map<std::uint64_t, std::vector<std::uint8_t>> blocks;
        std::map<std::string, std::int64_t> ints;
        std::map<std::string, std::string> texts;
        bool torn{}, torn_history{};
        int frames{};
        std::size_t read_bytes{};
        int history_reads{};
    } f;
    constexpr std::uint64_t main = 0x80000000, nro = 0x2000000000, game = 0x2100000000,
                            manager = 0x2200000000, player = 0x2300000000, players = 0x2400000000,
                            counts = 0x2500000000;
    std::ifstream in(path, std::ios::binary);
    assert(in);
    f.blocks[nro] = {std::istreambuf_iterator<char>(in), {}};
    f.blocks[nro].resize(0x830000);
    f.blocks[main].resize(0x200000);
    f.blocks[game].resize(0x318400);
    f.blocks[manager].resize(0x3700);
    f.blocks[player].resize(0x3B00);
    f.blocks[players].resize(8);
    f.blocks[counts].resize(600 * 4);
    const auto put = [&](std::uint64_t base, std::size_t offset, auto value) {
        std::memcpy(f.blocks.at(base).data() + offset, &value, sizeof(value));
    };
    put(main, 0x10CC80, nro + 0x2A37A0);
    put(nro, 0x81D3F0, nro + 0x826AF0);
    put(nro, 0x81D388, nro + 0x827850);
    put(nro, 0x826AF0, game);
    put(nro, 0x827850, manager);
    put(manager, 0, 2u);
    put(game, 0, 1u);
    put(game, 0x8D00, 84u);
    put(game, 0xC360, players);
    put(game, 0xC368, players + 8);
    put(game, 0xC370, players + 8);
    put(game, 0x277934, 100u);
    put(game, 0x277938, 4197u);
    put(players, 0, player);
    put(player, 0x29B8, counts);
    put(player, 0x29C0, counts + 600 * 4);
    put(player, 0x2684, 6u);
    put(player, 0x2688, 6u);
    put(player, 0x27C0, 10u);
    put(player, 0x28C4, 1.f);
    put(player, 0x27D0, 3.5f);
    put(player, 0x27D4, -23.75f);
    put(player, 0x27C4, 1.f);
    EdenDsmodHostApi host{};
    host.userdata = &f;
    host.main_base = main;
    host.main_size = 0x200000;
    host.get_i64 = [](void*, const char*, std::int64_t) -> std::int64_t { return 0; };
    host.is_mapped = [](void* ptr, std::uint64_t addr, std::uint64_t size) -> EdenDsmodBool {
        const auto& blocks = static_cast<Fixture*>(ptr)->blocks;
        auto it = blocks.upper_bound(addr);
        if (it == blocks.begin())
            return EDEN_DSMOD_FALSE;
        --it;
        return addr - it->first <= it->second.size() &&
               size <= it->second.size() - (addr - it->first);
    };
    host.read_memory = [](void* ptr, std::uint64_t addr, void* out,
                          std::size_t size) -> EdenDsmodBool {
        auto& fixture = *static_cast<Fixture*>(ptr);
        fixture.read_bytes += size;
        auto it = fixture.blocks.upper_bound(addr);
        if (it == fixture.blocks.begin())
            return EDEN_DSMOD_FALSE;
        --it;
        if (addr - it->first > it->second.size() || size > it->second.size() - (addr - it->first))
            return EDEN_DSMOD_FALSE;
        std::memcpy(out, it->second.data() + addr - it->first, size);
        if (fixture.torn && addr == game + 0x277934 && size == 4) {
            auto n = 100u + (++fixture.frames);
            std::memcpy(out, &n, 4);
        }
        if (fixture.torn_history && addr == game + 0x9A78 && size == 16 &&
            ++fixture.history_reads % 2 == 0) {
            std::uint64_t changed{};
            std::memcpy(&changed, static_cast<std::uint8_t*>(out) + 8, 8);
            changed += 0x18;
            std::memcpy(static_cast<std::uint8_t*>(out) + 8, &changed, 8);
        }
        return EDEN_DSMOD_TRUE;
    };
    host.publish_i64 = [](void* ptr, const char* key, std::int64_t value) {
        static_cast<Fixture*>(ptr)->ints[key] = value;
    };
    host.publish_text = [](void* ptr, const char* key, const char* value) {
        static_cast<Fixture*>(ptr)->texts[key] = value;
    };
    // Optional real-resource text path verifies that Room selection reaches
    // trinket EID.
    host.read_romfs = FixtureRomfs;
    auto text = isaac_text::CreateGameText();
    const bool check_text = TextFixture();
    if (check_text)
        assert(text->Load(&host, isaac_romfs::Lang::En));
    isaac_reader::Reader reader{host, nullptr};
    if (check_text)
        reader.SetText(text.get());
    reader.Sample(host);
    assert(f.ints.at("nro.ok") == 1 && f.ints.at("run.ready") == 1);
    assert(f.ints.at("edition.abp") == 1 && f.ints.at("pk.max") == 1);
    assert(f.texts.at("st.tears") == "10.00" && f.texts.at("st.range") == "23.75");
    assert(f.ints.at("run.paused") == 0);
    put(game, 0xC440, 1u);
    reader.Sample(host);
    assert(f.ints.at("run.paused") == 0);
    put(game, 0xC440, 2u);
    reader.Sample(host);
    assert(f.ints.at("run.paused") == 1);
    put(game, 0xC440, 0u);
    for (std::size_t off : {0xD628u, 0x4EC64u, 0xC2E0u, 0xA398u, 0x193538u}) {
        put(game, off, 1u);
        reader.Sample(host);
        assert(f.ints.at("run.paused") == 1);
        put(game, off, 0u);
    }
    put(game, 0x297CC0 + 0x244, std::uint8_t{1});
    reader.Sample(host);
    assert(f.ints.at("run.paused") == 0);
    put(game, 0x297CC0 + 0x38, player);
    reader.Sample(host);
    assert(f.ints.at("run.paused") == 1);
    put(game, 0x297CC0 + 0x244, std::uint8_t{0});
    put(counts, 251 * 4, 1u);
    reader.Sample(host);
    assert(f.ints.at("pk.max") == 2);
    put(player, 0x2A98, 1u);
    put(player, 0x2A9C, std::uint8_t{0});
    reader.Sample(host);
    assert(f.texts.at("pk.0.name") == "???" && f.ints.at("pk.0.effect") == -1);
    put(game, 0xA354 + 1, std::uint8_t{1});
    put(game, 0xA31C + 4, 1u);
    reader.Sample(host);
    assert(f.ints.at("pk.0.ident") == 1 && f.ints.at("pk.0.effect") == 1);
    put(player, 0x2688, 2u);
    reader.Sample(host);
    assert(f.ints.at("pk.0.effect") == 5);
    put(counts, 75 * 4, 1u);
    reader.Sample(host);
    assert(f.ints.at("pk.0.effect") == 2);
    put(counts, 75 * 4, 0u);
    // A saved Bag page must fall back to held slots in the edition without
    // crafting.
    assert(f.ints.at("bag.held") == 0);
    constexpr std::uint64_t history = 0x2600000000, room = 0x2700000000, entities = 0x2800000000,
                            pedestal = 0x2900000000;
    f.blocks[history].resize(0x18);
    put(history, 0, 88u);
    put(history, 4, 1u);
    put(game, 0x9A78, history);
    put(game, 0x9A80, history + 0x18);
    put(player, 0x29C0, counts + 600 * 4);
    put(counts, 4, 1u);
    assert(reader.OnAction("ui_open", 1));
    reader.Sample(host);
    assert(f.ints.at("inv.count") == 1 && f.ints.at("inv.0.id") == 1 && f.ints.at("inv.0.n") == 1);
    assert(reader.OnAction("info_inv", 0));
    reader.Sample(host);
    assert(f.ints.at("isel.id") == 1);
    f.blocks[room].resize(0x1800);
    f.blocks[entities].resize(8);
    f.blocks[pedestal].resize(0xD00);
    put(game, 0x8CF8, room);
    put(room, 0x1640, entities);
    put(room, 0x164C, 1u);
    put(entities, 0, pedestal);
    put(pedestal, 0x30, 5u);
    put(pedestal, 0x34, 100u);
    put(pedestal, 0x38, 1u);
    put(pedestal, 0x664, 1u);
    put(pedestal, 0x680, 10.f);
    put(pedestal, 0x684, 10.f);
    put(pedestal, 0x6B4, 10.f);
    assert(reader.OnAction("ui_open", 2));
    reader.Sample(host);
    assert(f.ints.at("ped.count") == 1 && f.ints.at("info.id") == 1 && f.ints.at("room.near") == 0);
    put(game, 0xC, 64u);
    reader.Sample(host);
    assert(f.ints.at("info.blind") == 1 && f.ints.at("info.id") == 0);
    // Ground trinkets remain describable under Curse of the Blind; collectible
    // masking must not leak into this different pickup kind. Both kinds use the
    // same nearest/tap selection.
    put(pedestal, 0x34, 350u);
    put(pedestal, 0x38, 53u);
    reader.Sample(host);
    assert(f.ints.at("ped.count") == 1 && f.ints.at("ped.0.kind") == 1);
    assert(f.ints.at("info.kind") == 1 && f.ints.at("info.id") == 53);
    assert(f.ints.at("info.blind") == 0 && f.ints.at("room.near") == 0);
    assert(f.texts.at("info.key") == "module:isaac:trink/53");
    if (check_text) {
        assert(!f.texts.at("info.name").empty() && !f.texts.at("info.eid").empty());
        assert(f.texts.at("info.eid") == text->Eid(isaac_text::Kind::Trinket, 53));
        assert(f.ints.at("info.eid_on") == 1);
    }
    assert(reader.OnAction("info_ped", 0));
    reader.Sample(host);
    assert(f.ints.at("info.kind") == 1 && f.ints.at("info.id") == 53);
    // Golden trinkets keep the id in the low 15 bits (subtype | 0x8000).
    put(pedestal, 0x38, 53 | 0x8000);
    reader.Sample(host);
    assert(f.ints.at("ped.count") == 1 && f.ints.at("ped.0.kind") == 1 &&
           f.ints.at("ped.0.id") == 53);
    assert(f.texts.at("ped.0.key") == "module:isaac:trink/53/g");
    assert(f.ints.at("info.kind") == 1 && f.ints.at("info.id") == 53);
    assert(f.texts.at("info.key") == "module:isaac:trink/53/g");
    if (check_text)
        assert(f.texts.at("info.eid") == text->Eid(isaac_text::Kind::Trinket, 53));
    put(pedestal, 0x38, 0x8000); // golden flag without an id: not a pickup to describe
    reader.Sample(host);
    assert(f.ints.at("ped.count") == 0);
    put(pedestal, 0x38, 53u);
    reader.Sample(host);
    assert(f.ints.at("ped.count") == 1);
    // Unknown pickup variants and taken trinkets must leave the room selection.
    put(pedestal, 0x34, 20u);
    reader.Sample(host);
    assert(f.ints.at("ped.count") == 0);
    put(pedestal, 0x34, 350u);
    put(pedestal, 0x38, 0u);
    reader.Sample(host);
    assert(f.ints.at("ped.count") == 0 && f.ints.at("info.kind") == -1);
    f.read_bytes = 0;
    reader.Sample(host);
    std::printf("AB+ ROOM guest bytes per sample: %zu\n", f.read_bytes);
    assert(f.read_bytes < 16384); // ROOM must not traverse the complete floor descriptor array.
    f.torn_history = true;
    reader.Sample(host);
    assert(f.ints.at("run.torn") > 0);
    f.torn_history = false;
    // A new run reusing the same allocations must not keep its old inventory
    // selection.
    assert(reader.OnAction("ui_open", 1));
    assert(reader.OnAction("info_inv", 0));
    put(manager, 0, 1u);
    reader.Sample(host);
    assert(f.ints.at("run.ready") == 0);
    assert(!reader.OnAction("info_inv", 0));
    put(manager, 0, 2u);
    reader.Sample(host);
    assert(f.ints.at("run.ready") == 1 && f.ints.at("isel.on") == 0);
    // Reject an incomplete history record instead of publishing a truncated
    // inventory.
    put(game, 0x9A80, history + 0x19);
    reader.Sample(host);
    assert(f.ints.at("run.ready") == 0);
    put(game, 0x9A80, history + 0x18);
    reader.Sample(host);
    assert(f.ints.at("run.ready") == 1);
    f.torn = true;
    reader.Sample(host);
    assert(f.ints.at("run.ready") == 1 && f.ints.at("run.torn") > 0);
    f.torn = false;
    // A wrong native fingerprint must stop the reader rather than guessing
    // offsets.
    f.blocks[nro][0x1C1470] ^= 1;
    isaac_reader::Reader wrong{host, nullptr};
    wrong.Sample(host);
    assert(f.ints.at("nro.ok") == 0 && f.ints.at("run.ready") == 0);
    std::puts("AB+ reader fixture passed: sample, capacity, pills, history/selection, "
              "pedestals/trinkets/golden/blind curse, torn reads, fingerprint rejection");
    return true;
}

// Repentance integration fixture: the owner's Repentance.nro (module id 91C73FDD...B2AB)
// mapped at a synthetic base with synthetic game, player and room state, so that the real
// fingerprints gate the Repentance ROOM page. Returns false when the image is not supplied.
bool RepentanceFixture() {
    const char* path = std::getenv("ISAAC_REP_IMAGE");
    if (!path)
        return false;
    struct Fixture {
        std::map<std::uint64_t, std::vector<std::uint8_t>> blocks;
        std::map<std::string, std::int64_t> ints;
        std::map<std::string, std::string> texts;
    } f;
    constexpr std::uint64_t main = 0x80000000, nro = 0x2000000000, game = 0x2100000000,
                            manager = 0x2200000000, player = 0x2300000000, players = 0x2400000000,
                            room = 0x2700000000, entities = 0x2800000000,
                            pedestal = 0x2900000000;
    std::ifstream in(path, std::ios::binary);
    assert(in);
    f.blocks[nro] = {std::istreambuf_iterator<char>(in), {}};
    assert(f.blocks[nro].size() == 0xAB1000); // Repentance.nro header file size
    f.blocks[nro].resize(0x1710970);          // text..bss end
    f.blocks[main].resize(0x200000);
    f.blocks[game].resize(0x2F0100);
    f.blocks[manager].resize(0x40000);
    f.blocks[player].resize(0x3000);
    f.blocks[players].resize(8);
    f.blocks[room].resize(0x2000);
    f.blocks[entities].resize(8);
    f.blocks[pedestal].resize(0x600);
    const auto put = [&](std::uint64_t base, std::size_t offset, auto value) {
        std::memcpy(f.blocks.at(base).data() + offset, &value, sizeof(value));
    };
    put(main, 0x10CC78, nro + 0x4C0840); // run_repentance GOT slot
    put(nro, 0xAAC698, nro + 0xABB448);  // g_Game GOT
    put(nro, 0xAAC648, nro + 0xABCCE0);  // g_Manager GOT
    put(nro, 0xABB448, game);
    put(nro, 0xABCCE0, manager);
    put(manager, 0x8, 2u); // in a run
    put(game, 0x0, 1);     // stage
    put(game, 0x24F99C, 100); // frame counter
    put(game, 0x25C50, players);
    put(game, 0x25C58, players + 8);
    put(players, 0, player);
    put(player, 0x310, 200.f); // position (EID nearest rule)
    put(player, 0x314, 200.f);
    put(game, 0x21550, room);
    put(room, 0x1950 + 0x78, entities);
    put(room, 0x1950 + 0x84, 1u);
    put(entities, 0, pedestal);
    put(pedestal, 0x38, 5);    // pickup
    put(pedestal, 0x3C, 350);  // trinket
    put(pedestal, 0x40, 53);   // subtype
    put(pedestal, 0x1B8, std::uint64_t{0}); // entity flags
    put(pedestal, 0x2F4, 50);  // spawn frame -> FrameCount 50
    put(pedestal, 0x310, 210.f);
    put(pedestal, 0x314, 200.f);
    put(pedestal, 0x344, 10.f);
    EdenDsmodHostApi host{};
    host.userdata = &f;
    host.main_base = main;
    host.main_size = 0x200000;
    host.get_i64 = [](void*, const char* name, std::int64_t) -> std::int64_t {
        assert(std::strcmp(name, "__source:aoc") == 0);
        return 1; // Repentance add-on enabled
    };
    host.is_mapped = [](void* ptr, std::uint64_t addr, std::uint64_t size) -> EdenDsmodBool {
        const auto& blocks = static_cast<Fixture*>(ptr)->blocks;
        auto it = blocks.upper_bound(addr);
        if (it == blocks.begin())
            return EDEN_DSMOD_FALSE;
        --it;
        return addr - it->first <= it->second.size() &&
               size <= it->second.size() - (addr - it->first);
    };
    host.read_memory = [](void* ptr, std::uint64_t addr, void* out,
                          std::size_t size) -> EdenDsmodBool {
        const auto& blocks = static_cast<Fixture*>(ptr)->blocks;
        auto it = blocks.upper_bound(addr);
        if (it == blocks.begin())
            return EDEN_DSMOD_FALSE;
        --it;
        if (addr - it->first > it->second.size() || size > it->second.size() - (addr - it->first))
            return EDEN_DSMOD_FALSE;
        std::memcpy(out, it->second.data() + addr - it->first, size);
        return EDEN_DSMOD_TRUE;
    };
    host.publish_i64 = [](void* ptr, const char* key, std::int64_t value) {
        static_cast<Fixture*>(ptr)->ints[key] = value;
    };
    host.publish_text = [](void* ptr, const char* key, const char* value) {
        static_cast<Fixture*>(ptr)->texts[key] = value;
    };
    host.read_romfs = FixtureRomfs;
    auto text = isaac_text::CreateGameText();
    const bool check_text = TextFixture();
    if (check_text)
        assert(text->Load(&host, isaac_romfs::Lang::En));
    isaac_reader::Reader reader{host, nullptr};
    if (check_text)
        reader.SetText(text.get());
    assert(reader.OnAction("ui_open", 2));
    reader.Sample(host);
    assert(f.ints.at("nro.ok") == 1 && f.ints.at("edition.abp") == 0);
    assert(f.ints.at("run.ready") == 1);
    // Room and Near domains must pass their real fingerprints.
    const std::string bad = f.texts.at("nro.bad");
    assert(bad.find("room") == std::string::npos && bad.find("near") == std::string::npos);
    const auto expect = [&](std::int64_t kind, std::int64_t id, const std::string& key) {
        assert(f.ints.at("ped.count") == 1 && f.ints.at("ped.0.kind") == kind);
        assert(f.ints.at("ped.0.id") == id && f.texts.at("ped.0.key") == key);
        assert(f.ints.at("room.near") == 0 && f.ints.at("room.sel") == 0);
        assert(f.ints.at("info.kind") == kind && f.ints.at("info.id") == id);
        assert(f.texts.at("info.key") == key);
        if (check_text) {
            const auto k = kind ? isaac_text::Kind::Trinket : isaac_text::Kind::Collectible;
            assert(!f.texts.at("ped.0.name").empty() && !f.texts.at("info.name").empty());
            assert(f.texts.at("info.name") == text->Item(k, static_cast<int>(id)).name);
            assert(!f.texts.at("info.eid").empty());
            assert(f.texts.at("info.eid") == text->Eid(k, static_cast<int>(id)));
        }
    };
    // Plain trinket, then a golden one (subtype | 0x8000), under Curse of the Blind:
    // trinkets stay describable.
    expect(1, 53, "module:isaac:trink/53");
    put(game, 0xC, 64u);
    put(pedestal, 0x40, 53 | 0x8000);
    reader.Sample(host);
    expect(1, 53, "module:isaac:trink/53/g");
    assert(f.ints.at("ped.0.blind") == 0 && f.ints.at("info.blind") == 0);
    // A collectible under the same curse is masked.
    put(pedestal, 0x3C, 100);
    put(pedestal, 0x40, 1);
    reader.Sample(host);
    assert(f.ints.at("ped.count") == 1 && f.ints.at("ped.0.kind") == 0);
    assert(f.ints.at("ped.0.blind") == 1 && f.ints.at("ped.0.id") == 0);
    assert(f.ints.at("info.blind") == 1 && f.ints.at("info.id") == 0);
    put(game, 0xC, 0u);
    reader.Sample(host);
    expect(0, 1, "module:isaac:coll/1");
    // Golden flag without an id, a taken trinket and an unknown variant leave the list.
    put(pedestal, 0x3C, 350);
    for (const std::int32_t sub : {0x8000, 0, -1}) {
        put(pedestal, 0x40, sub);
        reader.Sample(host);
        assert(f.ints.at("ped.count") == 0 && f.ints.at("info.kind") == -1);
    }
    put(pedestal, 0x3C, 20);
    put(pedestal, 0x40, 1);
    reader.Sample(host);
    assert(f.ints.at("ped.count") == 0);
    std::printf("Repentance reader fixture passed: trinket, golden trinket, blind curse, "
                "collectible, rejected subtypes%s\n",
                check_text ? ", names/EID" : "");
    return true;
}

int main() {
    const bool abp_fixture = AfterbirthFixture();
    const bool rep_fixture = RepentanceFixture();
    // The launcher is shared. Edition selection must probe its matching NRO entry
    // and fail closed when that entry cannot be read.
    struct Probe {
        std::int64_t source{};
        int source_queries{}, guest_accesses{};
        std::vector<std::uint64_t> reads;
        std::map<std::string, std::int64_t> ints;
        std::map<std::string, std::string> texts;
    };
    for (const std::int64_t source : {0, 1, -1}) {
        Probe probe;
        probe.source = source;
        EdenDsmodHostApi host{};
        host.userdata = &probe;
        host.main_base = 0x80000000;
        host.main_size = 0x200000;
        host.get_i64 = [](void* p, const char* name, std::int64_t) {
            assert(std::strcmp(name, "__source:aoc") == 0);
            auto& t = *static_cast<Probe*>(p);
            ++t.source_queries;
            return t.source;
        };
        host.is_mapped = [](void* p, std::uint64_t, std::uint64_t) -> EdenDsmodBool {
            ++static_cast<Probe*>(p)->guest_accesses;
            return EDEN_DSMOD_TRUE;
        };
        host.read_memory = [](void* p, std::uint64_t address, void*, size_t) -> EdenDsmodBool {
            ++static_cast<Probe*>(p)->guest_accesses;
            static_cast<Probe*>(p)->reads.push_back(address);
            return EDEN_DSMOD_FALSE;
        };
        host.publish_i64 = [](void* p, const char* name, std::int64_t value) {
            static_cast<Probe*>(p)->ints[name] = value;
        };
        host.publish_text = [](void* p, const char* name, const char* value) {
            static_cast<Probe*>(p)->texts[name] = value;
        };
        isaac_reader::Reader reader{host, nullptr};
        reader.Sample(host);
        reader.Sample(host);
        assert(probe.ints.at("dlc.available") == source);
        assert(probe.ints.at("run.ready") == 0 && probe.ints.at("nro.ok") == 0);
        assert(probe.source_queries == (source < 0 ? 2 : 1) && probe.guest_accesses > 0);
        if (source == 0) {
            assert(probe.texts.at("run.why") == "AfterbirthPlus.nro not loaded");
            for (auto address : probe.reads)
                assert(address == host.main_base + 0x10CC80);
        } else {
            assert(probe.texts.at("run.why") != "AfterbirthPlus.nro not loaded");
            for (auto address : probe.reads)
                assert(address != host.main_base + 0x10CC80);
        }
    }
    const char* abc = "abcdefghijklmnopqrstuvwxyz012345";
    assert(Seed2String(0x323A2203u, abc) == "hxkv 15vn");
    assert(Seed2String(0x1E093DC4u, abc) == "chte eoie");

    // Minimap::Render timer (live: 4197 -> 00:02:19, 27000 -> 00:15:00, 1926 ->
    // 00:01:04)
    assert(TimeText(4197, 0) == "00:02:19");
    assert(TimeText(27000, 0) == "00:15:00");
    assert(TimeText(1926, 0) == "00:01:04");
    assert(TimeText(108000 + 1800 + 30, 0) == "01:01:01");
    assert(TimeText(0, 22) == "00:16:00"); // challenge 22 counts down from 28800
    assert(TimeText(30000, 22) == "00:00:00");

    // StatHUD::Render "%.2f" (live: speed 1.00, tears 2.73, damage 3.50,
    // range 6.50)
    assert(StatText(1.0f) == "1.00");
    assert(StatText(30.0f / (10.0f + 1.0f)) == "2.73");
    assert(StatText(260.0f / 40.0f) == "6.50");
    assert(StatText(-0.5f) == "-0.50");
    // the game's 12-byte buffer: at most 11 characters
    assert(StatText(10000000.0f) == "10000000.00");
    assert(StatText(123456792.0f) == "123456792.0"); // "%.2f" truncated
    assert(StatText(-1.0e30f).size() == 11);

    // PauseScreen::Show tallies: default Isaac = 2 marks each (live,
    // verify/REPORT.md)
    assert(StatTally(0, 1.0f) == 2 && StatTally(1, 10.0f) == 2 && StatTally(2, 3.5f) == 2);
    assert(StatTally(3, 260.0f) == 2 && StatTally(4, 1.0f) == 2 && StatTally(5, 0.0f) == 2);
    assert(StatTally(0, 1.5f) == 4);  // live sentinel: speed 1.5 -> 4 marks
    assert(StatTally(2, 20.0f) == 7); // damage 20 -> full (clamped 7)
    assert(StatTally(5, -10.0f) == 0 && StatTally(5, 100.0f) == StatTallyMax);
    assert(StatTally(1, -3.0f) == 0);  // pow of a negative base = NaN -> 0
    assert(StatTally(3, 290.0f) == 3); // (290-230)/60+2 = 3
    { // Bag of Crafting preview (live recipe: [8,8,12,12,15,15,1,1] -> 647 4.5
        // Volt)
        unsigned char b[12] = {8, 8, 12, 12, 15, 15, 1, 1, 0x87, 0x02, 0, 0};
        assert(BagPreview(b, true, 0, true, true) == 1);
        assert(BagPreview(b, true, 64, true, true) == 2); // Curse of the Blind -> "?"
        assert(BagPreview(b, true, 64 | 1, true, true) == 2);
        assert(BagPreview(b, false, 0, true, true) == 0); // torn
        assert(BagPreview(b, true, 0, false, true) == 0); // special-seed curses: closed
        assert(BagPreview(b, true, 0, true, false) == 0); // no items.xml entry
        b[7] = 0;
        assert(BagPreview(b, true, 0, true, true) == 0); // 7 of 8
        b[7] = 1;
        b[8] = b[9] = 0;
        assert(BagPreview(b, true, 0, true, true) == 0); // no result
    }
    // round 8: bag held = 710 in any active slot (collectible, Schoolbag, Tainted
    // Cain's pocket slot 2)
    assert(BagHeld({710, 0, 0, 0}) && BagHeld({105, 710, 0, 0}) && BagHeld({0, 0, 710, 0}));
    assert(!BagHeld({0, 0, 0, 0}) && !BagHeld({105, 45, 0, 0}) && !BagHeld({709, 711, -1, 0}));

    // Level::GetName suffix
    assert(FloorSuffix(1, 0, 0, 0) == " I");
    assert(FloorSuffix(2, 0, 0, 0) == " II");
    assert(FloorSuffix(7, 0, 1, 0) == " I");
    assert(FloorSuffix(8, 0, 0, 0) == " II");
    assert(FloorSuffix(9, 0, 0, 0).empty());
    assert(FloorSuffix(1, 2, 0, 0) == " XL");
    assert(FloorSuffix(1, 0, 2, 0).empty()); // greed
    assert(FloorSuffix(1, 0, 3, 0).empty()); // greedier
    assert(FloorSuffix(1, 0, 0, 0x2C).empty());

    // map key: header byte + 6 bytes per room
    std::vector<MapRoom> rooms{{84, 1, 1, 0x85, 1, 1}, {71, 4, 2, 5, 2, 0}};
    assert(MapHex(rooms, false) == "00"
                                   "540101850101"
                                   "470402050200");
    assert(MapHex(rooms, true) == "01");
    assert(MapHex({}, false) == "00");

    // map flags byte (Minimap::Config::render icon rules)
    assert(MapFlags(0x1, 1, 0) == 1);
    assert(MapFlags(0x400, 1, 0) == 2);
    assert(MapFlags(0x801, 4, 0) == 5); // red treasure icon
    assert(MapFlags(0, 11, 1) == 8);    // boss challenge room
    assert(MapFlags(0, 11, 0) == 0);    // normal challenge room
    assert(MapFlags(0, 5, 1) == 0);     // subtype only matters for type 11

    // nearest pedestal (EID: FindInRadius(player, 200) + closest with FrameCount
    // > 0)
    {
        std::vector<NearCand> c{
            {100.0f, 100.0f, 10.0f, 0, 5},          // 0: d 141.4
            {300.0f, 200.0f, 10.0f, 0, 5},          // 1: d 100
            {200.0f, 400.0f, 10.0f, 0, 5},          // 2: d 200 < 200 + size 10 -> in range
            {200.0f, 190.0f, 10.0f, 1ull << 46, 5}, // 3: flag 46 -> QueryRadius skips it
            {200.0f, 195.0f, 10.0f, 0, 50},         // 4: spawned this frame -> FrameCount 0
        };
        float d = 0;
        assert(NearestPedestal(200.0f, 200.0f, c, 50, 200.0f, &d) == 1 && d == 100.0f);
        c[1].x = 500.0f; // d 300 > 210: out of range
        assert(NearestPedestal(200.0f, 200.0f, c, 50, 200.0f, &d) == 0);
        c[4].spawn = 49; // FrameCount 1 -> closest
        assert(NearestPedestal(200.0f, 200.0f, c, 50, 200.0f, &d) == 4 && d == 5.0f);
        std::vector<NearCand> far{{200.0f, 420.0f, 10.0f, 0, 0}}; // d 220 >= 210
        assert(NearestPedestal(200.0f, 200.0f, far, 50, 200.0f, &d) == -1 && d == -1.0f);
        std::vector<NearCand> tie{{250.0f, 200.0f, 10.0f, 0, 0}, {150.0f, 200.0f, 10.0f, 0, 0}};
        assert(NearestPedestal(200.0f, 200.0f, tie, 50, 200.0f, &d) == 0); // strict <: first wins
    }

    // The pocket slot count (GetMaxPocketItems).
    {
        using isaac_reader::detail::MaxPocketItems;
        const std::int32_t k0[4] = {1, 1, 1, 1}, i0[4] = {18, 0, 0, 0};
        const std::int32_t k1[4] = {0, 2, 1, 1}, i1[4] = {3, 2, 0, 0};
        assert(MaxPocketItems(false, k0, i0) == 1 && MaxPocketItems(true, k0, i0) == 2);
        assert(MaxPocketItems(false, k1, i1) == 2 && MaxPocketItems(true, k1, i1) == 3);
    }

    // round 9: PILLCARD button glyph (GetButtonFrame, RenderButtonIcon anim /
    // type-1 remap, control_pocket_item action)
    {
        using namespace isaac_reader::detail;
        assert(ButtonFrame(11) == 11 && ButtonFrame(9) == 12 && ButtonFrame(12) == 13); // R ZL ZR
        assert(ButtonFrame(8) == 10 && ButtonFrame(4) == 2 && ButtonFrame(7) == 3);
        assert(ButtonFrame(0) == 7 && ButtonFrame(3) == 4 && ButtonFrame(14) == 14);
        assert(ButtonFrame(16) == 25 && ButtonFrame(17) == 26 && ButtonFrame(18) == -1);
        assert(ButtonFrame(0xABAB0000u) == 19 && ButtonFrame(0xABAB0007u) == 20);
        assert(ButtonFrame(0xABAB0008u) == -1 && ButtonFrame(0xCDCD0001u) == -1);
        assert(ButtonFrame(0xFFFFFFFFu) == -1);
        assert(std::string{ButtonAnim(3)} == "Switch_Pro" &&
               std::string{ButtonAnim(2)} == "Switch");
        assert(std::string{ButtonAnim(0)} == "Switch_JoyCon" &&
               std::string{ButtonAnim(1)} == "Switch_JoyCon");
        assert(std::string{ButtonAnim(-1)} == "Switch" && std::string{ButtonAnim(6)} == "Switch");
        assert(ButtonFrameForType(8, 1) == 9 && ButtonFrameForType(16, 1) == 20 &&
               ButtonFrameForType(19, 1) == 23 && ButtonFrameForType(11, 1) == 11);
        assert(ButtonFrameForType(8, 3) == 8 && ButtonFrameForType(12, 1) == 12);
        assert(PocketAction(0, false, 0, 0) == 10 && PocketAction(21, true, 0, 0) == 10);
        assert(PocketAction(19, false, 1, 0) == 10); // J&E without a twin: PILLCARD
        assert(PocketAction(19, true, 1, 0) == 10);  // twin lower -> PILLCARD
        assert(PocketAction(20, true, 0, 1) == 9);   // self lower -> ITEM
        assert(PocketAction(20, true, 1, 1) == 9);   // tie -> self -> ITEM
    }

    // instruction decoder (GetPillEffect jump targets)
    auto i = Decode(0x528000c8u, 0x3C8674); // mov w8, #6
    assert(i.kind == Insn::MovzW && i.rd == 8 && i.imm == 6);
    i = Decode(0x14000027u, 0x3C8678); // b +0x9c
    assert(i.kind == Insn::B && i.target == 0x3C8714);
    i = Decode(0x17ffffe0u, 0x3C8100); // b -0x80
    assert(i.kind == Insn::B && i.target == 0x3C8080);
    i = Decode(0xb9400000u, 0);
    assert(i.kind == Insn::Other);

    assert(isaac_reader::SupportsBuildHex(
        "B6E5BDB9DC12E1D1A25CBFDA17F4BE24B4754EF5000000000000000000000000"));
    assert(!isaac_reader::SupportsBuildHex("B6E5BDB9"));
    std::puts("isaac reader tests passed (synthetic)");

    std::vector<std::string> skipped;
    if (!abp_fixture)
        skipped.push_back("AB+ reader fixture (ISAAC_ABP_IMAGE unset)");
    if (!rep_fixture)
        skipped.push_back("Repentance reader fixture (ISAAC_REP_IMAGE unset)");
    if (!TextFixture() && (abp_fixture || rep_fixture))
        skipped.push_back("fixture name/EID checks (ISAAC_ROMFS and ISAAC_FILE not both set)");
    if (skipped.empty())
        return 0;
    for (const auto& s : skipped)
        std::printf("SKIPPED: %s\n", s.c_str());
    const char* allow = std::getenv("ISAAC_ALLOW_MISSING_FIXTURES");
    if (allow && std::strcmp(allow, "1") == 0) {
        std::puts("isaac reader fixtures SKIPPED (ISAAC_ALLOW_MISSING_FIXTURES=1)");
        return 77;
    }
    std::puts("FAILED: isaac reader fixtures missing; supply them (see the top of this file) "
              "or set ISAAC_ALLOW_MISSING_FIXTURES=1");
    return 1;
}
