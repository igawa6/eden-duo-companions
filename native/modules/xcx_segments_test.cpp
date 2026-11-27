// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// FrontierNav segments: BDAT hashes, flag layouts, visited continents, hex placement, darkness
// (research: contracts/MAP-REVEAL.md, MIRA-MAP.md). Optional real-data checks run when
// XCX_COMMON_BDAT names the player's common.bdat; XCX_FLAGS_FRESH / XCX_FLAGS_PROGRESS name 0x2800-byte
// flag areas cut from a new and a progressed save (the progressed one has activity on every continent).
#include "xcx_bdat.h"
#include "xcx_map.h"
#include "xcx_reader.h"
#include "xcx_segments.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
using namespace xcx;
static bool Near(double a, double b, double t) { return std::fabs(a - b) <= t; }
// Set category-2 flag `id` to `value` (0..3) in a flag area.
static void SetCat2(std::vector<std::uint8_t> &f, unsigned id, unsigned value) {
    std::uint32_t word;
    const std::size_t at = FlagCat2Offset + std::size_t(id >> 4) * 4;
    std::memcpy(&word, f.data() + at, 4);
    word &= ~(3u << ((id & 15) * 2));
    word |= value << ((id & 15) * 2);
    std::memcpy(f.data() + at, &word, 4);
}
static void SetCat1(std::vector<std::uint8_t> &f, unsigned id, bool on) {
    if (on)
        f[id >> 3] |= std::uint8_t(1u << (id & 7));
    else
        f[id >> 3] &= std::uint8_t(~(1u << (id & 7)));
}
static std::vector<std::uint8_t> File(const char *path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
int main() {
    // The table names the loader looks up.
    assert(bdat::Murmur3("SEG_Terminal1501") == 0x7e4fe065);
    assert(bdat::Murmur3("SEG_Terminal1601") == 0x4f99d9f0);
    assert(bdat::Murmur3("SEG_Terminal1701") == 0x2a8a4c6b);
    assert(bdat::Murmur3("SEG_Terminal2001") == 0xc8e3ecd9);
    assert(bdat::Murmur3("SEG_Terminal2201") == 0x231fe1dc);
    assert(bdat::Murmur3("SEG_NormalList") == 0x3e72e869);

    assert(ContinentOfZone(1501) == 0 && ContinentOfZone(1601) == 1 && ContinentOfZone(1701) == 2 &&
           ContinentOfZone(2001) == 3 && ContinentOfZone(2201) == 4);
    assert(ContinentOfZone(1401) == -1 && ContinentOfZone(5000) == -1 && ContinentOfZone(0) == -1);

    // Flag layouts (MAP-REVEAL.md): category 1 one bit per id, category 2 two bits per id.
    std::vector<std::uint8_t> flags(FlagBytes, 0);
    assert(!Cat1(flags, 32006) && Cat2(flags, 4000) == 0);
    flags[32006 >> 3] = std::uint8_t(1u << (32006 & 7)); // 32006 >> 3 = 4000, past nothing: inside 0x2000?
    // 32006 >> 3 = 4000 < 0x2000, so it is a real category-1 id.
    assert(Cat1(flags, 32006) && !Cat1(flags, 32007) && !Cat1(flags, 32005));
    const unsigned id = 3711; // 3711 >> 4 = 231, shift (3711 & 15) * 2 = 30
    flags[FlagCat2Offset + 231 * 4 + 3] = 0xc0; // bits 30..31 = 3
    assert(Cat2(flags, 3711) == 3 && Cat2(flags, 3710) == 0 && Cat2(flags, 3712) == 0);
    assert(Cat2(flags, id) == 3);
    flags[FlagCat2Offset + 231 * 4 + 3] = 0x40; // 1
    assert(Cat2(flags, 3711) == 1);
    // Out of range ids read as 0 / false, never out of the buffer.
    assert(!Cat1(flags, 0xffff) && Cat2(flags, 0xffff) == 0);
    std::vector<std::uint8_t> shortflags(0x100);
    assert(!Cat1(shortflags, 5000) && Cat2(shortflags, 5000) == 0);

    // Visited: any open bit or nonzero state on a continent; nothing for unreadable flags.
    SegmentMap map;
    map.ok = true;
    for (int c = 0; c < Continents; ++c) {
        map.continent[c].ax = -5225.;
        map.continent[c].az = 3078.;
        SegHex plain{1, 1, 2, std::uint16_t(31000 + c), std::uint16_t(3700 + c)};
        SegHex probe{2, 2, 1, std::uint16_t(31100 + c), 0};
        map.continent[c].hexes = {plain, probe};
    }
    std::vector<std::uint8_t> blank(FlagBytes, 0);
    auto none = Visited(map, blank);
    for (bool v : none)
        assert(!v);
    for (bool v : Visited(map, {}))
        assert(!v);
    for (bool v : Visited(map, shortflags))
        assert(!v);
    auto open2 = blank; // continent 2: the probe's open bit
    SetCat1(open2, 31102, true);
    auto v = Visited(map, open2);
    assert(!v[0] && !v[1] && v[2] && !v[3] && !v[4]);
    auto state3 = blank; // continent 4: the normal segment's state
    SetCat2(state3, 3704, 1);
    v = Visited(map, state3);
    assert(!v[0] && !v[1] && !v[2] && !v[3] && v[4]);
    // A probe's category-2 flag id is unused: its state flag never counts.
    auto probe_state = blank;
    probe_state[FlagCat2Offset] = 3; // id 0, which a probe hex carries as state_flag 0
    v = Visited(map, probe_state);
    for (bool x : v)
        assert(!x);

    // Hex placement: Primordia anchor (-5225, 3078), hex (9, 6): column odd, so half a hex lower.
    Continent primordia{-5225., 3078., {}};
    const auto c = HexCentre(primordia, SegHex{9, 6, 2, 0, 0});
    assert(Near(c.x, -5225. + 0.75 * HexSize * 9, 1e-9) && Near(c.z, 3078. + 6 * HexSize + 0.5 * HexSize, 1e-9));
    const auto even = HexCentre(primordia, SegHex{8, 6, 2, 0, 0});
    assert(Near(even.z, 3078. + 6 * HexSize, 1e-9));

    // Darkness: an unlit continent's hex is dark at its centre, a lit one is not, far away is not.
    SegmentMap one;
    one.ok = true;
    one.continent[1].ax = -6000.;
    one.continent[1].az = 1000.;
    one.continent[1].hexes = {SegHex{0, 0, 2, 1, 1}};
    const auto owner = BuildOwnership(one);
    const auto centre = map::WorldUv(-6000., 1000.);
    std::array<bool, Continents> lit{};
    assert(Darkness(owner, lit, centre.u, centre.v) > 0.99);
    assert(Darkness(owner, lit, centre.u + 400, centre.v) == 0.);
    lit[1] = true;
    assert(Darkness(owner, lit, centre.u, centre.v) == 0.);
    lit[1] = false;
    lit[0] = true; // another continent being lit changes nothing here
    assert(Darkness(owner, lit, centre.u, centre.v) > 0.99);
    // The edge is a smooth ramp, not a step.
    double previous = 1.;
    for (double d = 0; d <= 120; d += 4) {
        const double dark = Darkness(owner, std::array<bool, Continents>{}, centre.u + d, centre.v);
        assert(dark <= previous + 1e-9);
        previous = dark;
    }
    assert(previous == 0.);

    // Real data (optional): the five lattices and the fresh / progressed save.
    if (const char *path = std::getenv("XCX_COMMON_BDAT")) {
        const auto bytes = File(path);
        const auto real = LoadSegments(bytes);
        assert(real.ok);
        const std::size_t counts[Continents] = {87, 77, 95, 75, 92};
        for (int k = 0; k < Continents; ++k)
            assert(real.continent[k].hexes.size() == counts[k]);
        // Primordia anchor and first hex: table 522 (-5225, 3078), Terminal1501 row 1 is (2, 2).
        assert(Near(real.continent[0].ax, -5225., 1e-3) && Near(real.continent[0].az, 3078., 1e-3));
        assert(real.continent[0].hexes[0].i == 2 && real.continent[0].hexes[0].j == 2 &&
               real.continent[0].hexes[0].type == 1);
        if (const char *fresh = std::getenv("XCX_FLAGS_FRESH")) {
            const auto f = File(fresh);
            assert(f.size() >= FlagBytes);
            for (bool x : Visited(real, f))
                assert(!x);
        }
        if (const char *progress = std::getenv("XCX_FLAGS_PROGRESS")) {
            const auto f = File(progress);
            assert(f.size() >= FlagBytes);
            for (bool x : Visited(real, f))
                assert(x);
            // Clearing one continent's flags (its open bits and states) leaves it unvisited.
            auto cleared = f;
            for (const auto &hex : real.continent[3].hexes) {
                SetCat1(cleared, hex.open_flag, false);
                if (hex.type == 2)
                    SetCat2(cleared, hex.state_flag, 0);
            }
            const auto partial = Visited(real, cleared);
            assert(partial[0] && partial[1] && partial[2] && !partial[3] && partial[4]);
        }
        std::puts("xcx segments: real data checked");
    }
    std::puts("xcx segments: all checks passed");
    return 0;
}
