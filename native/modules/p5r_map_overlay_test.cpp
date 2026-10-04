// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// p5r_map_overlay public-reader tests over synthetic scenes (p5r_map_overlay_fixtures.inc): a town
// field, a Palace floor whose covers hang on one flag, and a Palace floor with three radar
// entries. Expected values come from an independent model of the native rules (fixture generator).
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "p5r_map_overlay.h"
#include "p5r_map_overlay_fixtures.inc"

namespace {
void check(bool ok, const char* name, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s: %s\n", name, what);
        std::exit(1);
    }
}
struct Memory {
    std::map<std::uint64_t, std::vector<std::uint8_t>> ranges;
    explicit Memory(const Fixture& f) {
        for (const auto& r : f.ranges)
            ranges[r.at] = r.bytes;
    }
    bool read(std::uint64_t at, void* out, size_t n) const {
        auto it = ranges.upper_bound(at);
        if (it == ranges.begin())
            return false;
        --it;
        if (at - it->first + n > it->second.size())
            return false;
        std::memcpy(out, it->second.data() + (at - it->first), n);
        return true;
    }
    template <class T>
    void poke(std::uint64_t at, T value) {
        auto it = --ranges.upper_bound(at);
        std::memcpy(it->second.data() + (at - it->first), &value, sizeof(T));
    }
    template <class T>
    T peek(std::uint64_t at) const {
        T v{};
        read(at, &v, sizeof(T));
        return v;
    }
};
bool Run(const Memory& m, const Fixture& f, p5r_map_overlay::Frame& out,
         std::uint64_t main_size = 0x3200000) {
    return p5r_map_overlay::Sample(
        [&](std::uint64_t at, void* dst, size_t n) { return m.read(at, dst, n); }, f.main,
        main_size, f.field, f.ctx, f.expect.field_major, out);
}
} // namespace

int main() {
    for (const auto& f : Fixtures()) {
        const Memory m{f};
        p5r_map_overlay::Frame o;
        check(Run(m, f, o), f.name, "sample");
        check(o.tex_major == f.expect.tex_major && o.tex_minor == f.expect.tex_minor, f.name,
              "texpack");
        check(o.part_count == f.expect.parts.size(), f.name, "part count");
        for (size_t i = 0; i < f.expect.parts.size(); ++i)
            check(o.part_visible[i] == f.expect.parts[i], f.name, "part visibility");
        check(o.poi_count == f.expect.pois.size(), f.name, "poi count");
        for (size_t i = 0; i < f.expect.pois.size(); ++i)
            check(o.pois[i].type == static_cast<std::uint16_t>(f.expect.pois[i][0]) &&
                      o.pois[i].x == f.expect.pois[i][1] && o.pois[i].y == f.expect.pois[i][2],
                  f.name, "poi");
        check(o.radar_ready == f.expect.radar, f.name, "radar gate");
        check(o.fog_red == f.expect.fog_red, f.name, "red fog layer (C32D40 gate)");
        check(o.enemy_count == f.expect.radar_xy.size(), f.name, "radar count");
        for (size_t i = 0; i < f.expect.radar_xy.size(); ++i)
            check(std::abs(o.enemies[i].x - f.expect.radar_xy[i][0]) < 1e-3f &&
                      std::abs(o.enemies[i].y - f.expect.radar_xy[i][1]) < 1e-3f,
                  f.name, "radar position");

        // Bounded reads: an array without its -2 terminator inside the bound is refused.
        {
            Memory bad = m;
            const auto parts = m.peek<std::uint64_t>(f.ctx + 0xb0);
            for (unsigned i = 0; i < f.expect.parts.size() + 1; ++i)
                if (bad.peek<std::int32_t>(parts + 72 * i + 0x1c) == -2)
                    bad.poke<std::int32_t>(parts + 72 * i + 0x1c, -1);
            check(!Run(bad, f, o), f.name, "missing terminator");
        }
        // A derived flag routed through 899240 is refused, never guessed.
        if (f.expect.parts.size() > 2) {
            Memory bad = m;
            const auto parts = m.peek<std::uint64_t>(f.ctx + 0xb0);
            bad.poke<std::int32_t>(parts + 72 * 2 + 0x1c, 0x40000010);
            check(!Run(bad, f, o), f.name, "special flag");
        }
        // Flag table outside the main image, and a non-finite rectangle.
        {
            Memory bad = m;
            bad.poke<std::uint64_t>(f.main + 0x1d834c8, 0x10);
            check(!Run(bad, f, o), f.name, "flag table bounds");
            check(!Run(m, f, o, 0x1000), f.name, "main size bound");
        }
        {
            Memory bad = m;
            const auto parts = m.peek<std::uint64_t>(f.ctx + 0xb0);
            bad.poke<float>(parts + 0x14, std::nanf(""));
            check(!Run(bad, f, o), f.name, "nan rect");
        }
        // Radar is suppressed while the native minimap is not being drawn (list not rebuilt).
        {
            Memory bad = m;
            bad.poke<float>(f.ctx + 0x11c, 0.5f);
            check(Run(bad, f, o) && !o.radar_ready && o.enemy_count == 0, f.name,
                  "radar fade gate");
        }
        std::printf("ok %s: %u parts, %u pois, radar %s %u\n", f.name, o.part_count, o.poi_count,
                    f.expect.radar ? "ready" : "off",
                    static_cast<unsigned>(f.expect.radar_xy.size()));
    }
    // Throne Room: the flag that clears covers 2..7 decides both fog and the radar filter.
    for (const auto& f : Fixtures()) {
        if (std::strcmp(f.name, "throne") != 0)
            continue;
        Memory m{f};
        p5r_map_overlay::Frame o;
        check(Run(m, f, o) && o.part_visible[2] && o.part_visible[7], "throne", "covers visible");
        const auto secs = m.peek<std::uint64_t>(f.main + 0x1d834c8 + 16 * 2);
        const std::uint32_t bit = 0x1bd;
        auto word = m.peek<std::uint32_t>(secs + 4 * (bit >> 5));
        m.poke<std::uint32_t>(secs + 4 * (bit >> 5), word | (1u << (bit & 31)));
        check(Run(m, f, o) && !o.part_visible[2] && !o.part_visible[7], "throne",
              "flag clears covers");
    }
    // Player icon rotation (C36EE0): unit quaternions about the vertical axis; the expected angles
    // are the native minimap triangle's direction for those orientations (down-left 220, up
    // tilted right ~20, right 90, and the identity 180).
    {
        struct Q {
            std::array<float, 4> q;
            float deg;
        } cases[] = {
            {{0.0f, -0.342020183801651f, 0.0f, 0.9396926164627075f}, 220.0f},
            {{0.0f, 0.9850388765335083f, 0.0f, 0.17233237624168396f}, 19.847f},
            {{0.0f, 0.7071067690849304f, 0.0f, 0.7071067690849304f}, 90.0f},
            {{0.0f, 0.0f, 0.0f, 1.0f}, 180.0f},
        };
        for (const auto& c : cases) {
            std::uint8_t raw[16];
            std::memcpy(raw, c.q.data(), 16);
            float deg = -1;
            const bool ok = p5r_map_overlay::PlayerRotation(
                [&](std::uint64_t at, void* dst, size_t n) {
                    if (at != 0x1000 + 0x2a20 || n != 16)
                        return false;
                    std::memcpy(dst, raw, 16);
                    return true;
                },
                0x1000, deg);
            check(ok && std::abs(deg - c.deg) < 0.01f, "rotation", "quaternion angle");
        }
        float deg{};
        const std::array<float, 4> bad{0, 0, 0, 0};
        check(!p5r_map_overlay::PlayerRotation(
                  [&](std::uint64_t, void* dst, size_t) {
                      std::memcpy(dst, bad.data(), 16);
                      return true;
                  },
                  0x1000, deg),
              "rotation", "degenerate quaternion refused");
    }
    std::puts("p5r_map_overlay tests passed");
}
