// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Synthetic road-map overlay scenes for p5r_map_overlay_test.cpp. Generated: every address, flag
// number, texpack id, PARTS / ICON row, coordinate and radar entry is made up; only the layout is
// the one p5r_map_overlay.h reads. No bytes come from the game executable or from live memory.
// Expected values come from an independent model of the native rules in the generator.
//   town   non-dungeon field: two texture pieces, POIs gated by flags of sections 0 and 2 plus
//          skipped icon types, radar drawn with an empty list, no red fog.
//   throne Palace floor: rectangle and quad covers on one flag (0x200001BD, section 2, clear =
//          covers shown), other covers hidden, red fog via the texpack flag, empty radar.
//   east2f Palace floor with texture pieces, covers, three radar entries on the player's height
//          layer, red fog (so covered radar entries still show).
// Scene::Build lays out main (flag section table at main+1D834C8 and its bit words), the field
// context (+1F0 ids, +2A10 player, +F750 radar list, +FB10 count), the map context (+18 flags,
// +1C, +78 row, +80 texpack, +B0 PARTS, +C0 ICON, +100 / +11C radar gates, +1F0 scale, +1F4
// layer, +DFD special view) and the row arrays (72-byte rows, cond -2 terminator).
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Range {
    std::uint64_t at;
    std::vector<std::uint8_t> bytes;
};
struct Expect {
    std::uint16_t tex_major, tex_minor, field_major;
    std::vector<int> parts;
    std::vector<std::array<float, 3>> pois;
    bool radar;
    std::vector<std::array<float, 2>> radar_xy;
    bool fog_red;
};
struct Fixture {
    const char* name;
    std::uint64_t main, field, ctx;
    std::vector<Range> ranges;
    Expect expect;
};

namespace overlay_fixture {
struct Row {
    std::uint16_t type;
    std::int32_t cond;
    std::uint16_t mode;
    float w, h, x, y;
    std::array<float, 8> quad;
};
struct Scene {
    const char* name{};
    std::uint64_t main{}, field{}, ctx{};
    std::uint16_t field_major{}, field_minor{}, tex_major{}, tex_minor{};
    std::uint32_t layers{};
    std::array<float, 8> thresholds{};
    std::int32_t map_cond{};
    std::int16_t origin_x{}, origin_y{};
    float scale{};
    std::uint64_t flags18{};
    std::int32_t layer{};
    std::uint8_t ctx1c{};
    float f100{}, f11c{};
    std::array<float, 3> player{};
    std::vector<std::uint32_t> flags_set;
    std::vector<Row> parts, icons;
    std::vector<std::array<float, 3>> radar;
};
struct Image {
    std::vector<Range> ranges;
    void Map(std::uint64_t at, std::size_t size) {
        ranges.push_back({at, std::vector<std::uint8_t>(size)});
    }
    template <class T>
    void Put(std::uint64_t at, const T& value) {
        for (auto& r : ranges)
            if (at >= r.at && at - r.at + sizeof(T) <= r.bytes.size()) {
                std::memcpy(r.bytes.data() + (at - r.at), &value, sizeof(T));
                return;
            }
        std::abort();
    }
};
// Flag sections: {bit words, bit count} x 6 at main+1D834C8; the words live in main.
constexpr std::array<std::uint64_t, 6> SectionBits{0xc00, 0xc00, 0x1400, 0x200, 0x200, 0x200};
constexpr std::uint64_t SectionWords = 0x2380000;
inline void Rows(Image& img, std::uint64_t at, const std::vector<Row>& rows) {
    img.Map(at, 72 * (rows.size() + 1));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        const std::uint64_t p = at + 72 * i;
        img.Put(p, r.type);
        img.Put(p + 0xc, r.w);
        img.Put(p + 0x10, r.h);
        img.Put(p + 0x14, r.x);
        img.Put(p + 0x18, r.y);
        img.Put(p + 0x1c, r.cond);
        img.Put(p + 0x20, r.mode);
        img.Put(p + 0x24, r.quad);
    }
    img.Put(at + 72 * rows.size() + 0x1c, std::int32_t(-2));
}
inline Fixture Build(const Scene& s, Expect e) {
    Image img;
    const std::uint64_t row = s.ctx + 0x1000, tex = s.ctx + 0x1100, parts = s.ctx + 0x2000,
                        icons = s.ctx + 0x8000;
    img.Map(s.main + 0x1d834c8, 16 * SectionBits.size());
    std::uint64_t words = s.main + SectionWords;
    for (std::size_t k = 0; k < SectionBits.size(); ++k) {
        img.Put(s.main + 0x1d834c8 + 16 * k, words);
        img.Put(s.main + 0x1d834c8 + 16 * k + 8, SectionBits[k]);
        img.Map(words, SectionBits[k] / 8);
        words += 0x400;
    }
    for (std::uint32_t f : s.flags_set) {
        const std::uint64_t at =
            s.main + SectionWords + 0x400 * (f >> 28) + 4 * ((f & 0x0fffffff) >> 5);
        std::uint32_t w{};
        for (const auto& r : img.ranges)
            if (at >= r.at && at - r.at + 4 <= r.bytes.size())
                std::memcpy(&w, r.bytes.data() + (at - r.at), 4);
        img.Put(at, w | (1u << (f & 31)));
    }
    img.Map(s.field, 0xfb14);
    img.Put(s.field + 0x1f0, s.field_major);
    img.Put(s.field + 0x1f2, s.field_minor);
    img.Put(s.field + 0x2a10, s.player);
    for (std::size_t i = 0; i < s.radar.size(); ++i)
        img.Put(s.field + 0xf750 + 0x30 * i, s.radar[i]);
    img.Put(s.field + 0xfb10, static_cast<std::uint32_t>(s.radar.size()));
    img.Map(s.ctx, 0xe00);
    img.Put(s.ctx + 0x18, s.flags18);
    img.Put(s.ctx + 0x1c, s.ctx1c);
    img.Put(s.ctx + 0x78, row);
    img.Put(s.ctx + 0x80, tex);
    img.Put(s.ctx + 0xb0, parts);
    img.Put(s.ctx + 0xc0, icons);
    img.Put(s.ctx + 0x100, s.f100);
    img.Put(s.ctx + 0x11c, s.f11c);
    img.Put(s.ctx + 0x1f0, s.scale);
    img.Put(s.ctx + 0x1f4, s.layer);
    img.Map(row, 8);
    img.Put(row + 4, s.origin_x);
    img.Put(row + 6, s.origin_y);
    img.Map(tex, 72);
    img.Put(tex, s.tex_major);
    img.Put(tex + 2, s.tex_minor);
    img.Put(tex + 4, s.layers);
    img.Put(tex + 0x10, s.thresholds);
    img.Put(tex + 0x30, s.map_cond);
    Rows(img, parts, s.parts);
    Rows(img, icons, s.icons);
    return {s.name, s.main, s.field, s.ctx, std::move(img.ranges), std::move(e)};
}
} // namespace overlay_fixture

inline std::vector<Fixture> Fixtures() {
    using namespace overlay_fixture;
    std::vector<Fixture> out;

    {
        Scene s;
        s.name = "town";
        s.main = 0x80000000ull;
        s.field = 0x2100010000ull;
        s.ctx = 0x2100100000ull;
        s.field_major = 4;
        s.field_minor = 7;
        s.tex_major = 21;
        s.tex_minor = 5;
        s.layers = 1;
        s.thresholds = {400.0f, 800.0f, 1200.0f, 1600.0f, 2000.0f, 2400.0f, 2800.0f, 3200.0f};
        s.map_cond = -1;
        s.origin_x = 500;
        s.origin_y = 100;
        s.scale = 16.0f;
        s.flags18 = 0x11ull;
        s.layer = 0;
        s.ctx1c = 0x80;
        s.f100 = 1.0f;
        s.f11c = 1.0f;
        s.player = {96.0f, 0.0f, -1024.0f};
        s.flags_set = {0x100, 0x20000050};
        s.parts = {
            {0,
             -1,
             1,
             800.0f,
             600.0f,
             0.0f,
             0.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {1,
             -1,
             1,
             800.0f,
             560.0f,
             0.0f,
             0.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        };
        s.icons = {
            {60,
             -1,
             1,
             0.0f,
             0.0f,
             320.0f,
             410.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {61,
             -1,
             1,
             0.0f,
             0.0f,
             380.0f,
             360.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {62,
             -1,
             1,
             0.0f,
             0.0f,
             400.0f,
             250.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {5,
             256,
             1,
             0.0f,
             0.0f,
             450.0f,
             260.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {5,
             257,
             1,
             0.0f,
             0.0f,
             570.0f,
             60.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {5,
             536870992,
             1,
             0.0f,
             0.0f,
             220.0f,
             70.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {28,
             536870993,
             1,
             0.0f,
             0.0f,
             440.0f,
             250.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {13,
             -1,
             1,
             0.0f,
             0.0f,
             560.0f,
             30.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {19,
             536870994,
             1,
             0.0f,
             0.0f,
             480.0f,
             370.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {1, -1, 1, 0.0f, 0.0f, 10.0f, 10.0f, {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {115,
             -1,
             1,
             0.0f,
             0.0f,
             20.0f,
             20.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {46,
             -1,
             1,
             0.0f,
             0.0f,
             530.0f,
             400.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        };
        s.radar = {};
        Expect e{21,
                 5,
                 4,
                 {1, 1},
                 {{60.f, 320.0f, 410.0f},
                  {61.f, 380.0f, 360.0f},
                  {62.f, 400.0f, 250.0f},
                  {5.f, 450.0f, 260.0f},
                  {5.f, 220.0f, 70.0f},
                  {13.f, 560.0f, 30.0f},
                  {46.f, 530.0f, 400.0f}},
                 true,
                 {},
                 false};
        out.push_back(Build(s, std::move(e)));
    }
    {
        Scene s;
        s.name = "throne";
        s.main = 0x80000000ull;
        s.field = 0x2100210000ull;
        s.ctx = 0x2100300000ull;
        s.field_major = 151;
        s.field_minor = 40;
        s.tex_major = 160;
        s.tex_minor = 4;
        s.layers = 1;
        s.thresholds = {400.0f, 800.0f, 1200.0f, 1600.0f, 2000.0f, 2400.0f, 2800.0f, 3200.0f};
        s.map_cond = 536871416;
        s.origin_x = 4;
        s.origin_y = 4;
        s.scale = 16.0f;
        s.flags18 = 0x11ull;
        s.layer = 0;
        s.ctx1c = 0x00;
        s.f100 = 1.0f;
        s.f11c = 1.0f;
        s.player = {240.0f, 0.0f, 48.0f};
        s.flags_set = {0x20000164, 0x200001f8, 0x20000202, 0x20000a10, 0x20000a11};
        s.parts = {
            {0,
             -1,
             1,
             1500.0f,
             760.0f,
             0.0f,
             0.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {1,
             -1,
             1,
             1500.0f,
             760.0f,
             0.0f,
             0.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871357,
             0,
             170.0f,
             320.0f,
             670.0f,
             250.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871357,
             0,
             10.0f,
             330.0f,
             660.0f,
             250.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871357,
             0,
             6.0f,
             250.0f,
             650.0f,
             320.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871357,
             0,
             130.0f,
             4.0f,
             670.0f,
             580.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871357,
             0,
             100.0f,
             6.0f,
             700.0f,
             580.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {27,
             536871357,
             0,
             8.0f,
             16.0f,
             840.0f,
             250.0f,
             {840.0f, 250.0f, 840.0f, 590.0f, 880.0f, 250.0f, 860.0f, 580.0f}},
            {27,
             536871268,
             0,
             24.0f,
             70.0f,
             880.0f,
             460.0f,
             {880.0f, 460.0f, 880.0f, 530.0f, 990.0f, 460.0f, 970.0f, 560.0f}},
            {27,
             536871268,
             0,
             6.0f,
             4.0f,
             870.0f,
             530.0f,
             {860.0f, 510.0f, 860.0f, 570.0f, 890.0f, 530.0f, 870.0f, 570.0f}},
            {18,
             536871426,
             0,
             120.0f,
             250.0f,
             540.0f,
             440.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871426,
             0,
             330.0f,
             110.0f,
             650.0f,
             580.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871426,
             0,
             140.0f,
             330.0f,
             840.0f,
             250.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        };
        s.icons = {
            {4,
             536873488,
             1,
             0.0f,
             0.0f,
             930.0f,
             510.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {28,
             536873489,
             1,
             0.0f,
             0.0f,
             930.0f,
             510.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {13,
             536871426,
             1,
             0.0f,
             0.0f,
             580.0f,
             420.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {17,
             536871392,
             1,
             0.0f,
             0.0f,
             900.0f,
             600.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {19,
             536873490,
             1,
             0.0f,
             0.0f,
             930.0f,
             510.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        };
        s.radar = {};
        Expect e{160,
                 4,
                 151,
                 {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
                 {{4.f, 930.0f, 510.0f}, {28.f, 930.0f, 510.0f}, {13.f, 580.0f, 420.0f}},
                 true,
                 {},
                 true};
        out.push_back(Build(s, std::move(e)));
    }
    {
        Scene s;
        s.name = "east2f";
        s.main = 0x80000000ull;
        s.field = 0x2100410000ull;
        s.ctx = 0x2100500000ull;
        s.field_major = 151;
        s.field_minor = 16;
        s.tex_major = 160;
        s.tex_minor = 2;
        s.layers = 1;
        s.thresholds = {450.0f, 1200.0f, 1600.0f, 1800.0f, 2000.0f, 2400.0f, 2800.0f, 3200.0f};
        s.map_cond = 536871415;
        s.origin_x = 1000;
        s.origin_y = 600;
        s.scale = 16.0f;
        s.flags18 = 0x11ull;
        s.layer = 0;
        s.ctx1c = 0x80;
        s.f100 = 1.0f;
        s.f11c = 1.0f;
        s.player = {-2800.0f, 0.0f, 1520.0f};
        s.flags_set = {0x20000112, 0x20000113, 0x2000016f, 0x20000170, 0x20000180, 0x20000181,
                       0x20000182, 0x20000183, 0x200001f7, 0x20000a20, 0x20000a21, 0x20000a22};
        s.parts = {
            {0,
             -1,
             1,
             1280.0f,
             820.0f,
             0.0f,
             0.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {1,
             -1,
             1,
             1280.0f,
             820.0f,
             0.0f,
             0.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {2,
             536871184,
             0,
             40.0f,
             6.0f,
             640.0f,
             590.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {2,
             536871185,
             0,
             40.0f,
             6.0f,
             410.0f,
             590.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {1,
             536871186,
             0,
             90.0f,
             70.0f,
             270.0f,
             350.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {1,
             536871187,
             0,
             40.0f,
             6.0f,
             840.0f,
             590.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {1,
             536871188,
             1,
             40.0f,
             6.0f,
             920.0f,
             450.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {1,
             536871188,
             1,
             40.0f,
             6.0f,
             970.0f,
             450.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871296,
             0,
             20.0f,
             30.0f,
             1000.0f,
             200.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871297,
             0,
             21.0f,
             32.0f,
             1010.0f,
             208.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871298,
             0,
             22.0f,
             34.0f,
             1020.0f,
             216.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871299,
             0,
             23.0f,
             36.0f,
             1030.0f,
             224.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871296,
             0,
             24.0f,
             38.0f,
             1040.0f,
             232.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871297,
             0,
             25.0f,
             40.0f,
             1050.0f,
             240.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871298,
             0,
             26.0f,
             42.0f,
             1060.0f,
             248.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871299,
             0,
             27.0f,
             44.0f,
             1070.0f,
             256.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871296,
             0,
             28.0f,
             46.0f,
             1080.0f,
             264.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871297,
             0,
             29.0f,
             48.0f,
             1090.0f,
             272.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {27,
             536871299,
             0,
             12.0f,
             10.0f,
             1070.0f,
             270.0f,
             {1070.0f, 270.0f, 1070.0f, 280.0f, 1080.0f, 270.0f, 1080.0f, 280.0f}},
            {27,
             536871297,
             0,
             9.0f,
             13.0f,
             1180.0f,
             310.0f,
             {1180.0f, 310.0f, 1180.0f, 390.0f, 1200.0f, 310.0f, 1190.0f, 390.0f}},
            {18,
             536871280,
             0,
             430.0f,
             200.0f,
             40.0f,
             130.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {18,
             536871279,
             0,
             140.0f,
             128.0f,
             14.0f,
             2.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        };
        s.icons = {
            {4,
             536873504,
             1,
             0.0f,
             0.0f,
             750.0f,
             600.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {28,
             536873520,
             1,
             0.0f,
             0.0f,
             750.0f,
             600.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {10,
             536873521,
             1,
             0.0f,
             0.0f,
             1150.0f,
             270.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {26,
             536873522,
             1,
             0.0f,
             0.0f,
             720.0f,
             690.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {12,
             536873523,
             1,
             0.0f,
             0.0f,
             110.0f,
             210.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {12,
             536873524,
             1,
             0.0f,
             0.0f,
             660.0f,
             460.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {13,
             536873505,
             1,
             0.0f,
             0.0f,
             550.0f,
             420.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {15,
             536873506,
             1,
             0.0f,
             0.0f,
             600.0f,
             770.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {17,
             536873525,
             1,
             0.0f,
             0.0f,
             1070.0f,
             580.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {17,
             536873526,
             1,
             0.0f,
             0.0f,
             1020.0f,
             240.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {17,
             536873527,
             1,
             0.0f,
             0.0f,
             920.0f,
             300.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
            {19,
             536873528,
             1,
             0.0f,
             0.0f,
             750.0f,
             600.0f,
             {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        };
        s.radar = {
            {1600.0f, -400.0f, 2400.0f}, {-480.0f, 0.0f, 560.0f}, {-960.0f, -380.0f, 1840.0f}};
        Expect e{160,
                 2,
                 151,
                 {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                 {{4.f, 750.0f, 600.0f}, {13.f, 550.0f, 420.0f}, {15.f, 600.0f, 770.0f}},
                 true,
                 {{1100.0f, 750.0f}, {970.0f, 635.0f}, {940.0f, 715.0f}},
                 true};
        out.push_back(Build(s, std::move(e)));
    }
    return out;
}
