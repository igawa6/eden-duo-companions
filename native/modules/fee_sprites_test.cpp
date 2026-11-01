// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Synthetic test for fee::Sprites::Render's key parsing/dispatch (no romfs/game data needed):
// every bundle lookup fails against an always-missing RangeReader, so this only exercises key
// grammar, arity checks and "unknown name" fallthrough -- never a crash, never a wrong-sized
// output. A live decode against real sprite bytes is covered by the orchestrator's device check.

#include "fee_sprites.h"

#include <cassert>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            ++failures;                                                                         \
        }                                                                                        \
    } while (0)

lp_unity::RangeReader MissingReader() {
    lp_unity::RangeReader r;
    r.size = [](std::string_view) -> std::optional<std::uint64_t> { return std::nullopt; };
    r.read = [](std::string_view, std::uint64_t, std::uint8_t*, std::size_t) { return false; };
    return r;
}

void CheckRejects(fee::Sprites& s, std::string_view key) {
    std::vector<std::uint8_t> rgba;
    std::uint32_t w = 0, h = 0;
    const bool ok = s.Render(key, rgba, w, h);
    CHECK(!ok);
    CHECK(rgba.empty());
    CHECK(w == 0);
    CHECK(h == 0);
}

} // namespace

int main() {
    lp_unity::Reader unity{MissingReader(), {}, "/Data/StreamingAssets/aa/Switch/fe_assets_ui/"};
    fee::Sprites sprites{unity};

    // Tab capsule fill clipped to its outline (owner 2026-11-01: dark corner patch): a 20x10
    // ring outline with an opaque full-rect fill -> the fill survives only inside the ring.
    {
        lp_unity::Image o, f;
        o.width = f.width = 20;
        o.height = f.height = 10;
        o.rgba.assign(20 * 10 * 4, 0);
        f.rgba.assign(20 * 10 * 4, 255);
        for (std::uint32_t y = 2; y <= 7; ++y)
            for (std::uint32_t x = 4; x <= 15; ++x)
                if (y == 2 || y == 7 || x == 4 || x == 15)
                    o.rgba[(y * 20 + x) * 4 + 3] = 255;
        fee::ClipToOutline(f, o);
        CHECK(f.rgba[(0 * 20 + 0) * 4 + 3] == 0);  // corner outside
        CHECK(f.rgba[(3 * 20 + 2) * 4 + 3] == 0);  // left of the outline
        CHECK(f.rgba[(5 * 20 + 10) * 4 + 3] == 255); // inside
        CHECK(f.rgba[(2 * 20 + 8) * 4 + 3] == 255);  // under the line
        // A leaking outline (gap) floods the centre: the fill is left alone.
        lp_unity::Image g;
        g.width = 20, g.height = 10;
        g.rgba.assign(20 * 10 * 4, 255);
        o.rgba[(2 * 20 + 8) * 4 + 3] = 0;
        fee::ClipToOutline(g, o);
        CHECK(g.rgba[3] == 255);
    }

    // Portrait crop-to-fill (Thor recheck 2026-11-01: face strips were stretched into boxes).
    {
        // A 180x74 strip whose columns encode x and rows encode y.
        lp_unity::Image src;
        src.width = 180;
        src.height = 74;
        src.rgba.resize(180 * 74 * 4);
        for (std::uint32_t y = 0; y < 74; ++y)
            for (std::uint32_t x = 0; x < 180; ++x) {
                auto* px = &src.rgba[(y * 180 + x) * 4];
                px[0] = std::uint8_t(x), px[1] = std::uint8_t(y), px[2] = 0, px[3] = 255;
            }
        // 120x86 row box: crop 64x74 of width... aspect 120/86 -> 103x74 centred (x 38..141).
        auto row = fee::CropFill(src, 120, 86, 0.5f, 0.5f);
        CHECK(row.width == 120 && row.height == 86);
        CHECK(row.rgba[0] >= 37 && row.rgba[0] <= 39);                       // left edge x ~38
        CHECK(row.rgba[(119) * 4] >= 138 && row.rgba[(119) * 4] <= 141);     // right edge x ~140
        CHECK(row.rgba[(85 * 120) * 4 + 1] >= 72);                           // full height kept
        // Uniform scale: x and y advance by the same source step (no stretch).
        const float sx = float(row.rgba[119 * 4] - row.rgba[0]) / 119.0f;
        const float sy = float(row.rgba[(85 * 120) * 4 + 1] - row.rgba[1]) / 85.0f;
        CHECK(sx > 0.75f * sy && sx < 1.25f * sy);
        // Square (Matchup) and tall (Overview) boxes keep the aspect too.
        auto sq = fee::CropFill(src, 72, 72, 0.5f, 0.5f);
        CHECK(sq.width == 72 && sq.height == 72 && sq.rgba[0] >= 52 && sq.rgba[0] <= 54);
        auto tall = fee::CropFill(src, 230, 260, 0.5f, 0.0f);
        CHECK(tall.width == 230 && tall.height == 260 && tall.rgba[1] == 0);
        // face/portrait keys parse but fail cleanly without romfs art.
        CheckRejects(sprites, "face/Alfred/120x86");
        CheckRejects(sprites, "portrait/Alfred/230x260");
        CheckRejects(sprites, "face/Alfred/0x86");
        CheckRejects(sprites, "face/Alfred");
        CheckRejects(sprites, "unitart/SoldierMG/230x260"); // no romfs: clean failure
        CheckRejects(sprites, "unitart/SoldierMG/x");
        CheckRejects(sprites, "dotring/SoldierMG/96");      // no romfs: clean failure
        CheckRejects(sprites, "dotring/SoldierMG/8");       // ring diameter out of range
        CheckRejects(sprites, "dotring/SoldierMG/x");
        CheckRejects(sprites, "dotring/SoldierMG/96/p");    // no romfs: clean failure
        CheckRejects(sprites, "dotring/SoldierMG/96/z");    // unknown faction
        CheckRejects(sprites, "dotplate/SoldierMG/120x86"); // plate needs a faction
        CheckRejects(sprites, "dotplate/SoldierMG/8x8/p");  // too small
        CheckRejects(sprites, "facering/Alfred/96/e");      // no romfs: clean failure
        CheckRejects(sprites, "facering/Alfred/9999/e");
        CheckRejects(sprites, "faceplate/Alfred/120x86");   // plate needs a faction
        CheckRejects(sprites, "faceplate/Alfred/120x86/q");
        CheckRejects(sprites, "standby/1240x1080");          // no romfs: clean failure
        CheckRejects(sprites, "standby/10x10");
        CheckRejects(sprites, "ghostring/96");               // no romfs: clean failure
        CheckRejects(sprites, "ghostring/4");
    }

    // Malformed grammar: wrong arity, bad size tokens, empty/unknown kind.
    CheckRejects(sprites, "");
    CheckRejects(sprites, "nope/Foo/48");
    CheckRejects(sprites, "panel/textures/WdwContent");          // missing size
    CheckRejects(sprites, "panel/textures/WdwContent/0x0");      // zero size rejected
    CheckRejects(sprites, "panel/textures/WdwContent/48xbad");   // non-numeric height
    CheckRejects(sprites, "panel/textures/WdwContent/9999x9999"); // over the 4096 cap
    CheckRejects(sprites, "icon/godring/Lueur/0");                // zero size rejected
    CheckRejects(sprites, "icon/godring/Lueur/9999");             // over the 1024 cap
    CheckRejects(sprites, "icon/notagroup/Lueur/48");              // unknown icon group
    CheckRejects(sprites, "item/Antidote/0");
    CheckRejects(sprites, "hpgauge/base/Blue/120x16");            // retired key family
    CheckRejects(sprites, "dot/001Lueur_x/xbad");
    CheckRejects(sprites, "bg/notmenu/1240x1080");
    CheckRejects(sprites, "bg/menu/0x0");

    // gear/<d>/<RRGGBB> : a procedural glyph (no romfs lookup), used for the War Settings button
    // (owner 2026-11-01: no gear/option icon exists in the romfs UI sprite survey).
    {
        std::vector<std::uint8_t> rgba;
        std::uint32_t w = 0, h = 0;
        CHECK(sprites.Render("gear/64/FFFFFF", rgba, w, h));
        CHECK(w == 64 && h == 64);
        CHECK(rgba.size() == std::size_t(64) * 64 * 4);
        // Centre pixel is inside the hole: fully transparent.
        const std::size_t c = (std::size_t(32) * 64 + 32) * 4;
        CHECK(rgba[c + 3] == 0);
        // Some pixel along the horizontal spoke (well inside the outer radius) is opaque white.
        const std::size_t edge = (std::size_t(32) * 64 + 50) * 4;
        CHECK(rgba[edge + 3] == 255);
    }
    CheckRejects(sprites, "gear/4/FFFFFF");     // below the 8 px floor
    CheckRejects(sprites, "gear/300/FFFFFF");   // over the 256 px cap
    CheckRejects(sprites, "gear/64/bad");       // bad hex colour
    CheckRejects(sprites, "gear/64");           // missing colour

    // Well-formed grammar that still fails cleanly because the backing romfs has nothing (the
    // synthetic reader reports every file missing): never a crash, never a non-empty result.
    CheckRejects(sprites, "panel/textures/WdwContent/360x220");
    CheckRejects(sprites, "panel/menucursor/CursorMenuBg/112x90");
    CheckRejects(sprites, "icon/item/Antidote/48");
    CheckRejects(sprites, "icon/skill/Empty/44");
    CheckRejects(sprites, "icon/godface/Lueur/560x560");
    CheckRejects(sprites, "item/Antidote/48/equip");
    CheckRejects(sprites, "dot/001Lueur_000/x2");
    CheckRejects(sprites, "bg/menu/1240x1080/d40");

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("fee_sprites_test: all checks passed\n");
    return 0;
}
