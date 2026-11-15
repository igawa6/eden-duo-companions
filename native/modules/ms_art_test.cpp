// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Monster Sanctuary art: index parsing and sprite / glyph geometry always; the decode from a
// real romfs only with MS_ROMFS=<merged romfs folder> (exit 77 = skipped when unset).
#include "ms_art.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

void TestParseAndCrop() {
    ms::PictureIndex idx;
    assert(!idx.Parse("M 2\nN 0 a\n"));
    ms::PictureIndex ok;
    assert(ok.Parse("M 1\nN 0 sharedassets0.assets\nT 0 0 100 64 4 4 4\nS ui/x 0 1 0 2 3 1\n"
                    "S icons/a b 0 0 0 1 1 0\nL regular 12 0\nG regular 72 0 0 2 1 0 9 2 5 1\n"));
    ms::PictureIndex text_index;
    assert(text_index.Parse("M 1\nN 0 a\nQ options 0 100 7\n"));
    assert(text_index.strings.at("options").offset == 100);
    ms::PictureIndex bad_text;
    assert(!bad_text.Parse("M 1\nN 0 a\nQ options 0 100 201\n"));
    assert(ok.sprites.count("icons/a b") == 1);
    assert(ok.sprites.at("ui/x").turned && ok.textures.at(0).width == 4);
    assert(ok.fonts.at("regular").glyphs.at('H').bottom == 2);
    // a 4x4 texture whose red channel = x + 10 y (top-left origin)
    ms::Picture t;
    t.width = t.height = 4;
    t.rgba.resize(64);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            t.rgba[(y * 4 + x) * 4] = static_cast<std::uint8_t>(x + 10 * y);
    // stored 2x3 at (1,0), turned: output is 3 wide, 2 tall, out (x,y) = stored (1 - y, 2 - x)
    const auto c = ms::CropSprite(t, ok.sprites.at("ui/x"), 2);
    assert(c.width == 6 && c.height == 4);
    assert(c.rgba[0] == 22);                       // out (0,0) <- stored local (1,2) = tex (2,2)
    assert(c.rgba[((0 * 6) + 4) * 4] == 2);        // out (2,0) <- stored local (1,0) = tex (2,0)
    assert(c.rgba[((2 * 6) + 0) * 4] == 21);       // out (0,1) <- stored local (0,2) = tex (1,2)
    // RGBA32 rows are stored bottom-up
    std::uint8_t raw[2 * 2 * 4]{};
    raw[0] = 7;           // stored row 0 = bottom
    raw[8] = 9;           // stored row 1 = top
    const auto d = ms::DecodeRgba32(raw, sizeof raw, 2, 2);
    assert(d && d->rgba[0] == 9 && d->rgba[8] == 7);
    assert(!ms::DecodeRgba32(raw, 4, 2, 2));
}

std::size_t ReadFile(const std::string& path, std::uint64_t off, void* out, std::size_t size) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return 0;
    const auto n = static_cast<std::uint64_t>(f.tellg());
    if (!out)
        return static_cast<std::size_t>(n);
    if (off + size > n)
        return 0;
    f.seekg(static_cast<std::streamoff>(off));
    f.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
    return f ? size : 0;
}


void WritePam(const std::string& path, const ms::Picture& p) {
    std::ofstream o(path, std::ios::binary);
    o << "P7\nWIDTH " << p.width << "\nHEIGHT " << p.height << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    o.write(reinterpret_cast<const char*>(p.rgba.data()), static_cast<std::streamsize>(p.rgba.size()));
}

// Particle effects (ms_fx): records, Unity curve / gradient evaluation, the deterministic,
// periodic simulation and the emitter offset by frame number.
void TestParticles() {
    // AnimationCurve: Hermite with the keys' own slopes; flat keys ease in and out
    ms::FxCurve c;
    c.keys = {{0, 0, 0, 0}, {1, 1, 0, 0}};
    assert(c.Eval(-1) == 0 && c.Eval(2) == 1 && std::abs(c.Eval(0.5f) - 0.5f) < 1e-6f);
    assert(std::abs(c.Eval(0.25f) - 0.15625f) < 1e-6f); // 3u^2 - 2u^3
    ms::FxCurve linear;
    linear.keys = {{0, 0, 1, 1}, {1, 1, 1, 1}};
    assert(std::abs(linear.Eval(0.3f) - 0.3f) < 1e-6f);
    ms::FxCurve step;
    step.keys = {{0, 2, 0, INFINITY}, {1, 5, INFINITY, 0}};
    assert(step.Eval(0.9f) == 2);
    // Gradient: blend lerps, fixed takes the first key at or after t
    ms::FxGradient g;
    g.colors = {{0.5f, {1, 0, 0}}, {1, {0, 0, 1}}};
    g.alphas = {{0, 0}, {0.5f, 1}};
    auto v = g.Eval(0.75f);
    assert(std::abs(v[0] - 0.5f) < 1e-6f && std::abs(v[2] - 0.5f) < 1e-6f && v[3] == 1);
    assert(g.Eval(0.25f)[0] == 1 && std::abs(g.Eval(0.25f)[3] - 0.5f) < 1e-6f);
    g.fixed = true;
    assert(g.Eval(0.6f)[2] == 1 && g.Eval(0.4f)[0] == 1);
    assert(ms::FrameNumber("SpectralWolf/7") == 7 && ms::FrameNumber("a/b") == -1 && ms::FrameNumber("x") == -1);

    // a root that emits 6 / s from a box, each birth firing a burst of 5..7 rising particles
    const std::string idx =
        "M 1\nN 0 a\n"
        "P test 0 -1 - x=-2 y=-10 off=0:0,0;1:0,-3 loop=1 dur=2.5 rate=6 life=0.1,0.1 speed=0,0 size=0.5,0.5 "
        "grav=0 c0=1,1,1,1 c1=1,1,1,1 shape=box,12,3 ren=0\n"
        "P test 1 0 - loop=0 dur=0.2 burst=5,7 life=1.2,1.4 speed=1,3 size=2,2 grav=0 c0=1,1,1,1 c1=1,1,1,1 "
        "shape=circle,1,1 sz=0,0,0,0;0.15,1,0,0;0.66,1,0,0;1,0,0,0 vx=k:0,-10,0,0;0.2,0.1,0,0;1,-10,0,0 vy=k:0,5,0,0 "
        "lim=1,100,1000,k:0,300,0,0;0.23,3.6,0,0,1 grad=0/0,0.08,0.15,0.38;1,0.08,0.15,0.38/0,0;0.21,1;0.88,1;1,0 ren=1\n";
    ms::PictureIndex fx;
    assert(fx.Parse(idx));
    const auto& set = fx.fx.at("test");
    assert(set.systems.size() == 2 && set.systems[0].subs == std::vector<int>{1} && !set.systems[0].rendered);
    assert(set.systems[0].offsets.at(1).second == -3 && std::abs(set.Reach(set.systems[0]) - 1.5) < 1e-6);
    ms::PictureIndex bad;
    assert(!bad.Parse("M 1\nN 0 a\nP test 1 -1 - life=1,1\n"));       // ids start at 0
    assert(!bad.Parse("M 1\nN 0 a\nP test 0 -1 - rate=6\n"));         // no lifetime
    assert(!bad.Parse("M 1\nN 0 a\nP test 0 -1 - life=1,1 shape=cone,1,1\n"));
    const auto draw = [&](double t, int frame) {
        ms::Picture p;
        p.width = p.height = 64;
        p.rgba.assign(64 * 64 * 4, 0);
        ms::DrawFx(set, "test", t, frame, p, 32, 48, 2, [](std::string_view) { return nullptr; });
        return p;
    };
    const auto lit = [](const ms::Picture& p) {
        int n = 0, top = 1 << 30;
        for (std::uint32_t i = 0; i < p.width * p.height; ++i)
            if (p.rgba[i * 4 + 3]) {
                ++n;
                top = std::min<int>(top, static_cast<int>(i / p.width));
                assert(p.rgba[i * 4] == 20 && p.rgba[i * 4 + 1] == 38 && p.rgba[i * 4 + 2] == 97);
            }
        return std::pair{n, top};
    };
    const auto a = draw(3.0, 0), b = draw(3.0, 0), later = draw(3.5, 0);
    assert(a.rgba == b.rgba);                              // a pure function of time
    assert(draw(3.0 + ms::FxPeriod, 0).rgba == a.rgba);    // repeats every FxPeriod
    assert(draw(0.0, 0).rgba == draw(ms::FxPeriod, 0).rgba);
    assert(lit(a).first > 20 && a.rgba != later.rgba);     // already burning at any moment, moving
    // the emitter follows the frame: frame 1 lifts it 3 game px (6 picture px at scale 2)
    const auto lifted = draw(3.0, 1);
    assert(lit(lifted).first == lit(a).first && lit(lifted).second == lit(a).second - 6);
}

int TestRomfs() {
    const char* romfs = std::getenv("MS_ROMFS");
    const char* package = std::getenv("MS_PACKAGE");
    if (!romfs || !*romfs)
        return 77;
    if (!package || !*package)
        package = MS_TEST_PACKAGE;
    ms::Art art([&](const char* path, std::uint64_t off, void* out, std::size_t size) {
        std::string p{path};
        if (p.starts_with("file:"))
            return ReadFile(std::string{package} + "/" + p.substr(5), off, out, size);
        return ReadFile(std::string{romfs} + "/" + p, off, out, size);
    });
    const auto particle = art.Load("module:ms:spr1:map/teleporter");
    if (!particle) { // pictures.idx or data.unity3d did not load
        std::fprintf(stderr, "ms art: could not load romfs %s with package %s\n", romfs, package);
        return 1;
    }
    assert(particle->width == 2 && particle->height == 2);
    for (const auto value : particle->rgba)
        assert(value == 255); // the native RGB24 Rect2x2 material texture (opaque white)
    for (const auto name : {"Tree", "DuelCircle", "InfinityArena", "ChampionChallenge", "House",
                            "Item", "Jail", "Smith", "Sword", "Sun", "OnlineArena", "DressMaker"}) {
        const auto pic = art.Load(std::string{"module:ms:spr1:ui/MapIcon"} + name);
        assert(pic && pic->width && pic->height);
    }
    for (int i = 1; i <= 16; ++i)
        assert(art.Load("module:ms:spr1:ui/MapMarker" + std::to_string(i)));
    assert(art.String("options") == "Options");
    assert(art.String("on") == "On" && art.String("off") == "Off");
    assert(art.String("unavailable") == "Not Available");
    assert(art.String("absent").empty());
    // W5's two-line columns preserve the full native labels and fractional-turn unit.
    for (const auto& [text, width] : std::vector<std::pair<std::string, int>>{
             {art.String("execution"), 84}, {art.String("duration"), 128},
             {art.String("healthiness"), 88}, {"99999%", 84},
             {"999.66 " + art.String("turns"), 128}, {"100%", 88}}) {
        const auto full = art.Load("module:ms:txt:small:4:400:l:" + text);
        const auto fitted = art.Load("module:ms:txt:small:4:" + std::to_string(width) + ":l:" + text);
        assert(full && fitted);
        for (std::uint32_t y = 0; y < full->height; ++y)
            for (std::uint32_t x = 0; x < full->width; ++x)
                for (int c = 0; c < 4; ++c)
                    assert(full->rgba[(y * full->width + x) * 4 + c] ==
                           (x < fitted->width ? fitted->rgba[(y * fitted->width + x) * 4 + c] : 0));
    }
    const auto rule = art.String("turn1");
    assert(rule == "The Keeper who has first turn may only use two of their three chosen Monsters during their first turn as a drawback.");
    const auto split = rule.rfind(' ', rule.size() / 2);
    assert(split != std::string::npos);
    for (const auto& line : {rule.substr(0, split), rule.substr(split + 1)}) {
        const auto full = art.Load("module:ms:txt:small:4:400:l:" + line);
        const auto fitted = art.Load("module:ms:txt:small:4:294:l:" + line);
        assert(full && fitted && fitted->width == 294 * 4);
        // Every pixel of both native lines survives the banner's fit (no ellipsis needed).
        for (std::uint32_t y = 0; y < full->height; ++y)
            for (std::uint32_t x = 0; x < full->width; ++x)
                for (int c = 0; c < 4; ++c)
                    assert(full->rgba[(y * full->width + x) * 4 + c] ==
                           (x < fitted->width ? fitted->rgba[(y * fitted->width + x) * 4 + c] : 0));
    }
    const auto layout = art.Load("module:ms:spr1:ui/layout");
    assert(layout && layout->width == 64 && layout->height == 64);
    assert(layout->rgba[3] == 0 || layout->rgba[3] == 255); // pixel art: no partial alpha at the corner
    const auto wolf = art.Load("module:ms:spr2:Thanatos/0");
    assert(wolf && wolf->width == 88 && wolf->height == 76);
    const auto turned = art.Load("module:ms:spr1:Thanatos_light/0"); // stored 38x44, turned
    assert(turned && turned->width == 44 && turned->height == 38);
    // box lookup: any letter case, then the monster's still shrunk into the box
    const auto cat = art.Load("module:ms:box2:Catzerker/0");
    assert(cat && cat->width == 128 && cat->height == 128);
    const auto vaero = art.Load("module:ms:box2:Vaero_light/0");
    assert(vaero && vaero->width == 128);
    const auto row = art.Load("module:ms:row2:Vaero_light/0");
    assert(row && row->width == 128 && row->height == 108);
    // Team crop preserves every bottom pixel at the original scale, with no sprite overflow.
    assert(std::equal(row->rgba.begin(), row->rgba.end(), vaero->rgba.begin() + 20 * 128 * 4));
    std::uint32_t first{}, line{};
    std::vector<EdenDsmodFontGlyph> glyphs;
    assert(art.Font(first, glyphs, line) && line == 12);
    const auto& h = glyphs.at('H' - first);
    assert(h.w == 5 && h.h == 7 && h.bearing_y == 7 && h.advance == 5);
    const auto& g = glyphs.at('g' - first);
    assert(g.bearing_y == 6 && g.h == 7); // one row below the baseline
    const auto title = art.Load("module:ms:txt:bold:4:146:c:Explore Ability");
    assert(title && title->width == 146 * 4 && title->height == 14 * 4);
    // too wide for bold: falls back to a smaller font, still exactly the box width
    const auto fit = art.Load("module:ms:txt:bold:4:30:l:Spectral Flame");
    assert(fit && fit->width == 30 * 4 && fit->height <= 14 * 4);
    assert(!art.Load("module:ms:txt:bold:9:40:l:x") && !art.Load("module:ms:txt:bold:4:40:l"));
    // the Teleporter row's place name (T:small/3:65): 3x, exactly its 195 x 36 rect, not cut
    const auto place = art.Load("module:ms:txt:small:3:65:l:Monster Farm");
    const auto place_full = art.Load("module:ms:txt:small:1:400:l:Monster Farm");
    assert(place && place->width == 65 * 3 && place->height == 12 * 3 && place_full);
    std::uint32_t used = 0;
    for (std::uint32_t x = 0; x < place_full->width; ++x)
        for (std::uint32_t y = 0; y < place_full->height; ++y)
            if (place_full->rgba[(y * place_full->width + x) * 4 + 3] != 0)
                used = x + 1;
    assert(used > 0 && used <= 65);
    // the target card's Weak / Resist line (T:small:60, ms_publish WeakResistLine): the sign, the
    // native word and two element icons fit the card; the third does not (61 / 62 px), so from
    // three icons on the word gives way and the sign and up to five icons fit
    const auto ink = [&art](const std::string& line) {
        const auto pic = art.Load("module:ms:txt:small:1:400:l:" + line);
        assert(pic);
        std::uint32_t right = 0;
        for (std::uint32_t x = 0; x < pic->width; ++x)
            for (std::uint32_t y = 0; y < pic->height; ++y)
                if (pic->rgba[(y * pic->width + x) * 4 + 3] != 0)
                    right = x + 1;
        return right;
    };
    const std::string els2 = "{i:ui/ElementFire}{i:ui/ElementWater}";
    const std::string els3 = els2 + "{i:ui/ElementWind}";
    const std::string els5 = els3 + "{i:ui/ElementEarth}{i:ui/ElementQuad}";
    assert(ink("{i:ui/ResistanceSign}{s:resist}" + els2) <= 60 && ink("{i:ui/WeaknessSign}{s:weak}" + els2) <= 60);
    assert(ink("{i:ui/ResistanceSign}{s:resist}" + els3) > 60 && ink("{i:ui/WeaknessSign}{s:weak}" + els3) > 60);
    assert(ink("{i:ui/ResistanceSign}" + els5) <= 60);
    // the battle description strip's pages (owner ruling R3): a native SkillTooltip text that is
    // longer than the 298 px line, wrapped at 276 px in the regular font; every word kept, in
    // order, and every page fits its line
    const std::string desc = "3 x 155% (465%) physical damage (neutral) Critical Hits apply Bleed stacks equal to "
                             "20% of the damage dealt";
    const auto one = art.WrapLines("regular", 298, desc);
    const auto pages = art.WrapLines("regular", 276, desc);
    assert(one && one->size() == 2 && pages && pages->size() == 2);
    std::string joined;
    for (const auto& line : *pages) {
        joined += (joined.empty() ? "" : " ") + line;
        assert(ink(line) <= 276 + 2);
    }
    assert(joined == desc);
    assert(art.WrapLines("regular", 298, "Short text")->size() == 1);
    // idle clips: every frame of a set's clip, placed by its anchor offset inside the clip's
    // union; sets whose sprites have another name (Vaero plays birdy/*)
    const auto clips = art.Clips();
    assert(clips.count("thanatos") && clips.count("thanatos_light") && clips.count("vaero"));
    assert(clips.at("vaero").frames.front().starts_with("birdy/") && clips.at("thanatos").fps > 14);
    int lowest = 0;
    for (const auto& frame : clips.at("vaero").frames) {
        const auto pic = art.Load("module:ms:box2:" + frame + "~Vaero");
        assert(pic && pic->width == 128 && pic->height == 128);
        for (std::uint32_t y = 0; y < pic->height; ++y)
            for (std::uint32_t x = 0; x < pic->width; ++x)
                if (pic->rgba[(y * pic->width + x) * 4 + 3])
                    lowest = std::max(lowest, static_cast<int>(y));
    }
    assert(lowest >= 124); // the clip's union sits on the box bottom
    assert(art.Load("module:ms:row2:" + clips.at("thanatos_light").frames[3] + "~Thanatos_light"));
    // inline icon: drawn right after the rendered text (its own glyph advances), not at a column
    const auto opaque_cols = [](const ms::Picture& p, std::uint32_t x0, std::uint32_t x1) {
        int n = 0;
        for (std::uint32_t x = x0; x < x1 && x < p.width; ++x)
            for (std::uint32_t y = 0; y < p.height; ++y)
                if (p.rgba[(y * p.width + x) * 4 + 3]) {
                    ++n;
                    break;
                }
        return n;
    };
    const auto plain = art.Load("module:ms:txt:bold:1:110:l:Claws");
    const auto iconic = art.Load("module:ms:txt:bold:1:110:l:Claws{i:ui/ElementPhysical}");
    assert(plain && iconic && iconic->width == 110);
    std::uint32_t text_end = 0;
    for (std::uint32_t x = 0; x < plain->width; ++x)
        if (opaque_cols(*plain, x, x + 1))
            text_end = x + 1;
    assert(text_end > 10 && text_end < 40);
    assert(opaque_cols(*iconic, text_end, text_end + 10) >= 6);     // the 8 px icon follows the text
    assert(opaque_cols(*iconic, text_end + 12, iconic->width) == 0); // nothing at a fixed column
    // {s:<name>}: the native label, then its icons
    const auto weak = art.Load("module:ms:txt:regular:1:60:l:{s:weak}{i:ui/ElementFire}{i:ui/ElementEarth}");
    const auto weak_plain = art.Load("module:ms:txt:regular:1:60:l:Weak");
    assert(weak && weak_plain && opaque_cols(*weak, 0, 60) > opaque_cols(*weak_plain, 0, 60) + 12);
    // inline icons that alone overflow the box: the line is squeezed into it, not clipped (each
    // box column is the natural line's column x * natural / box, so the last icon is still there;
    // a line that fits no font ends on the last font tried, small)
    const std::string icons = "{i:ui/ElementFire}{i:ui/ElementEarth}{i:ui/ElementWater}{i:ui/ElementWind}";
    const auto left = art.Load("module:ms:txt:small:1:200:l:" + icons);
    const auto right = art.Load("module:ms:txt:small:1:200:r:" + icons);
    const auto squeezed = art.Load("module:ms:txt:regular:1:20:l:" + icons);
    assert(left && right && squeezed && squeezed->width == 20 && squeezed->height == left->height);
    const auto col = [](const ms::Picture& p, std::uint32_t x, std::uint32_t y) {
        return &p.rgba[(y * p.width + x) * 4];
    };
    // the natural line width: the shift that turns the right-aligned picture into the left one
    std::uint32_t lw = 0;
    for (std::uint32_t w = 21; w <= 200 && !lw; ++w) {
        bool same = true;
        for (std::uint32_t x = 0; x < w && same; ++x)
            for (std::uint32_t y = 0; y < left->height && same; ++y)
                same = std::equal(col(*left, x, y), col(*left, x, y) + 4, col(*right, x + 200 - w, y));
        if (same)
            lw = w;
    }
    assert(lw > 30);
    for (std::uint32_t x = 0; x < 20; ++x)
        for (std::uint32_t y = 0; y < squeezed->height; ++y)
            assert(std::equal(col(*squeezed, x, y), col(*squeezed, x, y) + 4, col(*left, x * lw / 20, y)));
    // the battle Back's native B glyph (12 px, like the title prompt's A) before the native "Back"
    const auto bglyph = art.Load("module:ms:spr1:ui/ButtonBSwitch");
    assert(bglyph && bglyph->width == 12 && bglyph->height == 12);
    const auto back = art.Load("module:ms:txt:regular:1:52:c:{i:ui/ButtonBSwitch} {s:back}");
    const auto back_plain = art.Load("module:ms:txt:regular:1:52:c:Back");
    assert(back && back_plain && back->width == 52 && back->height == back_plain->height);
    assert(art.TakeMissing().empty());
    // a missing inline sprite is reported once, and the text still renders
    assert(art.Load("module:ms:txt:regular:1:60:l:Ab{i:ui/NoSuchIcon}"));
    const auto missing = art.TakeMissing();
    assert(missing.size() == 1 && missing[0] == "ui/NoSuchIcon" && art.TakeMissing().empty());
    // wrapped description: one native point per line, continuation lines hang, no ellipsis
    const auto wrapped = art.Load("module:ms:wrap:regular:1:146:6:- Player can ride the Monster|"
                                  "- Monster can glide over long gaps while the player rides it");
    assert(wrapped && wrapped->width == 146 && wrapped->height == 6 * 12);
    const auto band = [&](int line) {
        int n = 0;
        for (std::uint32_t y = line * 12; y < static_cast<std::uint32_t>(line + 1) * 12; ++y)
            for (std::uint32_t x = 0; x < wrapped->width; ++x)
                n += wrapped->rgba[(y * wrapped->width + x) * 4 + 3] != 0;
        return n;
    };
    assert(band(0) && band(1) && band(2) && !band(3) && !band(4) && !band(5));
    // the continuation line starts under the text, not under the "-"
    std::uint32_t first0 = 999, first2 = 999;
    for (std::uint32_t x = 0; x < wrapped->width; ++x)
        for (std::uint32_t y = 0; y < 12; ++y) {
            if (wrapped->rgba[(y * wrapped->width + x) * 4 + 3] && first0 == 999)
                first0 = x;
            if (wrapped->rgba[((y + 24) * wrapped->width + x) * 4 + 3] && first2 == 999)
                first2 = x;
        }
    assert(first2 > first0 + 3);
    // particle effects: the Spectral Wolf's blue fire, native colours, moving, deterministic
    const auto wolf_plain = art.Load("module:ms:box6:SpectralWolf/0~SpectralWolf");
    const auto fire = art.Load("module:ms:box6:SpectralWolf/0~SpectralWolf@21");
    const auto fire_again = art.Load("module:ms:box6:SpectralWolf/0~SpectralWolf@21");
    const auto fire_next = art.Load("module:ms:box6:SpectralWolf/0~SpectralWolf@22");
    assert(wolf_plain && fire && fire_again && fire_next);
    assert(fire->rgba == fire_again->rgba && fire->rgba != fire_next->rgba && fire->rgba != wolf_plain->rgba);
    // the fire adds the native flame colours over and around the wolf (its own sprite uses them too)
    int changed = 0, flame_changed = 0;
    for (std::uint32_t i = 0; i < fire->width * fire->height; ++i)
        if (std::memcmp(&fire->rgba[i * 4], &wolf_plain->rgba[i * 4], 4) != 0) {
            ++changed;
            flame_changed += fire->rgba[i * 4] == 20 && fire->rgba[i * 4 + 1] == 37 && fire->rgba[i * 4 + 2] == 96;
        }
    assert(changed > 36 * 10 && flame_changed > 36 * 4); // whole 6x6 game pixels
    // the shifted wolves burn in their own colours; a monster without effects ignores "@"
    const auto dark = art.Load("module:ms:box6:SpectralWolf_dark/0~SpectralWolf_dark@45");
    const auto dark_plain = art.Load("module:ms:box6:SpectralWolf_dark/0~SpectralWolf_dark");
    assert(dark && dark_plain && dark->rgba != dark_plain->rgba);
    const auto cat_fx = art.Load("module:ms:box2:Catzerker/0~Catzerker@45");
    const auto cat_plain = art.Load("module:ms:box2:Catzerker/0~Catzerker");
    assert(cat_fx && cat_plain && cat_fx->rgba == cat_plain->rgba);
    const auto sets = art.FxSets();
    for (const auto* name : {"spectralwolf", "spectraleagle", "spectraltoad", "spectrallion", "specter",
                             "sycophantom", "aazerach", "frosty", "iceblob", "ascendant", "maptp"})
        assert(sets.count(name));
    assert(!sets.count("steamgolem")); // ShowParticlesInMenus is off: not in the game's menus
    // the map teleporter sparkle on its own: cyan / white, rising above its 8 px tile
    const auto sparkle = art.FxPicture("maptp", 0.5, -1, 16, 32, 8, 24, 1);
    assert(sparkle);
    int above = 0, any = 0; // rows 0..19: more than 4 px over the emitter, past the tile's own half
    for (std::uint32_t y = 0; y < 32; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            const bool on = sparkle->rgba[(y * 16 + x) * 4 + 3] != 0;
            any += on;
            above += on && y < 20;
        }
    std::fprintf(stderr, "maptp: %d px, %d above\n", any, above);
    assert(any > 0 && above > 0);
    if (std::getenv("MS_FX_TIME")) { // dev aid: what one effect picture costs
        const auto t0 = std::chrono::steady_clock::now();
        for (int step = 100; step < 160; ++step)
            assert(art.Load("module:ms:box6:SpectralWolf/" + std::to_string(step % 10) + "~SpectralWolf@" + std::to_string(step)));
        const auto t1 = std::chrono::steady_clock::now();
        for (int step = 0; step < 60; ++step)
            assert(art.Load("module:ms:box6:SpectralWolf/" + std::to_string(step % 10) + "~SpectralWolf"));
        const auto t2 = std::chrono::steady_clock::now();
        std::fprintf(stderr, "box6 with fire %.2f ms, without %.2f ms\n",
                     std::chrono::duration<double, std::milli>(t1 - t0).count() / 60,
                     std::chrono::duration<double, std::milli>(t2 - t1).count() / 60);
    }
    if (const char* dir = std::getenv("MS_FX_DUMP")) { // dev aid: one second of each effect as PAMs
        for (const std::string set : {"SpectralWolf", "SpectralEagle", "SpectralToad", "SpectralLion", "Specter",
                                      "Frosty", "IceBlob", "Aazerach", "Sycophantom", "Ascendant", "SpectralWolf_light"})
            for (int step = 0; step < 30; step += 3) {
                const auto clip = art.Clips().at([&] {
                    std::string k = set;
                    for (auto& ch : k)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    return k;
                }());
                const auto frame = clip.frames[static_cast<std::size_t>(step / 2) % clip.frames.size()];
                if (const auto pic = art.Load("module:ms:box4:" + frame + "~" + set + "@" + std::to_string(step)))
                    WritePam(std::string{dir} + "/" + set + "-" + std::to_string(step) + ".pam", *pic);
            }
        for (int step = 0; step < 30; step += 3)
            if (const auto pic = art.FxPicture("maptp", step / 30.0, -1, 16, 32, 8, 24, 4 /*scale*/ / 4))
                WritePam(std::string{dir} + "/maptp-" + std::to_string(step) + ".pam", *pic);
    }
    const auto atlas = art.Load("module:ms:font");
    assert(atlas && atlas->width == 256);
    if (const char* dump = std::getenv("MS_DUMP_FONT")) { // dev aid: the host atlas as PAM
        std::ofstream o(dump, std::ios::binary);
        o << "P7\nWIDTH " << atlas->width << "\nHEIGHT " << atlas->height
          << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
        o.write(reinterpret_cast<const char*>(atlas->rgba.data()), static_cast<std::streamsize>(atlas->rgba.size()));
    }
    return 0;
}

} // namespace

int main() {
    TestParseAndCrop();
    TestParticles();
    const int r = TestRomfs();
    std::puts(r == 0 ? "ms art: ok (romfs)" : r == 77 ? "ms art: ok (romfs skipped)" : "ms art: FAILED (romfs)");
    return r;
}
