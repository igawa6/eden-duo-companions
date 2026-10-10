// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lp_poketch_display.h"
#include "lp_poketch.h"
#include "lp_raster.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

// Pictures from the romfs may be empty or carry damaged float geometry: no read outside a picture,
// no undefined float -> int conversion.
void TestRaster() {
    using namespace lp_raster;
    assert(Fits(1, 1, 4) && !Fits(0, 1, 4) && !Fits(1, 0, 4) && !Fits(2, 2, 15) && Fits(2, 2, 16));
    assert(!Fits(0xFFFFFFFFu, 0xFFFFFFFFu, std::numeric_limits<std::size_t>::max()));
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    assert(TruncCoord(nan) == 0 && TruncCoord(2.9) == 2 && TruncCoord(-2.9) == -2);
    assert(TruncCoord(inf) == (1 << 24) && TruncCoord(-inf) == -(1 << 24) && TruncCoord(1e300) == (1 << 24));
    assert(!RoundCoord(nan) && !RoundCoord(inf) && !RoundCoord(-1e300));
    assert(RoundCoord(2.5) == 2 && RoundCoord(3.5) == 4 && RoundCoord(-0.5) == 0); // Python round()
    assert(!RoundPx(nan) && !RoundPx(inf) && RoundPx(0.4) == 0 && RoundPx(10.5) == 11);
    assert(NineFits(8, 8, 4) && !NineFits(7, 8, 4) && !NineFits(8, 7, 4) && !NineFits(0, 0, 0) && !NineFits(8, 8, -1));
    // The shiny mark: refused for an icon under 8 px, an empty sparkle or inconsistent buffers.
    std::vector<std::uint8_t> sparkle(4 * 4 * 4, 255), icon(4 * 4 * 4, 0), empty;
    assert(!StampSparkle(4, 4, icon, 4, 4, sparkle));
    std::vector<std::uint8_t> big(16 * 16 * 4, 0);
    assert(!StampSparkle(16, 16, big, 0, 0, empty) && !StampSparkle(16, 16, big, 4, 4, empty));
    assert(!StampSparkle(16, 16, empty, 4, 4, sparkle));
    assert(std::all_of(big.begin(), big.end(), [](std::uint8_t v) { return v == 0; }));
    assert(StampSparkle(16, 16, big, 4, 4, sparkle));
    // an 8 px mark (16 / 4 < 8) in red over the transparent corner, nothing outside it
    assert(big[0] == 220 && big[1] == 40 && big[2] == 52 && big[3] == 255);
    assert(big[(7 * 16 + 7) * 4 + 3] == 255 && big[(8 * 16 + 8) * 4 + 3] == 0 && big[(0 * 16 + 8) * 4 + 3] == 0);
    std::vector<std::uint8_t> tiny(8 * 8 * 4, 0);
    assert(StampSparkle(8, 8, tiny, 1, 1, std::vector<std::uint8_t>{0, 0, 0, 255}) && tiny[(7 * 8 + 7) * 4 + 3] == 255);
}

int main() {
    TestRaster();
    using namespace lp_poketch;
    // Touch animations terminate, share a finite set of image keys, and do not
    // restart accidentally for negative time or a clock boundary.
    assert(CoinFrame(-1) == -1 && CoinFrame(0) == 0 && CoinFrame(79) == 0);
    assert(CoinFrame(80) == 1 && CoinFrame(959) == 11 && CoinFrame(960) == -1);
    assert(CoinFrame(std::numeric_limits<std::int64_t>::max()) == -1);
    assert(HopOffset(-1) == 0 && HopOffset(0) == 0 && HopOffset(80) == -12);
    assert(HopOffset(160) == -24 && HopOffset(240) == -12 && HopOffset(320) == 0);
    assert(HopOffset(400) == 0 && HopOffset(std::numeric_limits<std::int64_t>::max()) == 0);
    Calc calculator;
    calculator.Press(4);
    calculator.Press(-1);
    calculator.Press(17);
    assert(calculator.entry == "4" && !calculator.error);
    // Missing art must be local to its reader: opening a later session must
    // retry that session's resources, while animation frames avoid redecoding
    // unchanged native sprites and Pokémon icons within one session.
    int reads = 0;
    lp_unity::RangeReader missing{
        [&](std::string_view) -> std::optional<std::uint64_t> { ++reads; return std::nullopt; },
        [](std::string_view, std::uint64_t, std::uint8_t*, std::size_t) { return false; }};
    lp_unity::Reader first{missing}, second{missing};
    SpriteCache first_cache, second_cache;
    assert(Render(first, {HiddenMoves, 0, 2}, 1000, "en", &first_cache));
    const int first_reads = reads;
    assert(first_reads > 0);
    assert(Render(first, {HiddenMoves, 0, 2}, 1000, "en", &first_cache));
    assert(reads == first_reads);
    assert(Render(second, {HiddenMoves, 0, 2}, 1000, "en", &second_cache));
    assert(reads > first_reads);
    std::vector<int> party(40);
    party[0] = PokemonList;
    party[2] = 25;
    party[7] = 1;
    party[38] = 0;
    assert(Render(first, party, 1000, "en", &first_cache));
    const int icon_reads = reads;
    party[39] = -24;
    assert(Render(first, party, 1000, "en", &first_cache));
    assert(reads == icon_reads);
    // Calendar keys must never overflow the weekday/day sum or shift a mark
    // past its 31-bit month. Reject incomplete/out-of-range data without art IO.
    for (const auto& key : std::vector<std::vector<int>>{
             {Calendar, 0}, {Calendar, 0, 0, 1, 0, 31, 0},
             {Calendar, 0, 13, 1, 0, 31, 0}, {Calendar, 0, 1, -1, 0, 31, 0},
             {Calendar, 0, 1, 32, 0, 31, 0}, {Calendar, 0, 1, 1, -1, 31, 0},
             {Calendar, 0, 1, 1, 7, 31, 0}, {Calendar, 0, 1, 1, 0, 0, 0},
             {Calendar, 0, 1, 1, 0, 32, 0},
             {Calendar, 0, 1, 1, std::numeric_limits<int>::max(),
              std::numeric_limits<int>::max(), -1}}) {
        assert(!Render(first, key, 1000, "en", &first_cache));
        assert(reads == icon_reads);
    }
    // February and a six-week 31-day month are both valid, including day 31's
    // highest mark bit. No current-day highlight is also a supported state.
    assert(Render(first, {Calendar, 0, 2, 0, 0, 28, 0}, 1000, "en", &first_cache));
    assert(Render(first, {Calendar, 0, 12, 31, 6, 31, -1}, 1000, "en", &first_cache));
    // Nursery's native text is composited before the LCD grid and nearest scaling.
    // Opaque black glyph pixels must stay black in boxed and fullscreen views.
    lp_unity::Image title;
    title.width = 100; title.height = 48;
    title.rgba.resize(title.width * title.height * 4, 0);
    for (int y = 5; y < 40; ++y)
        for (int x = 20; x < 30; ++x) title.rgba[(y*title.width+x)*4+3] = 255;
    for (int scale : {1000, 1875, 2000}) {
        const auto image = Render(first, {EggMonitor, 0}, scale, "en", &first_cache, &title);
        assert(image);
        const unsigned x = 295u * scale / 1000, y = 50u * scale / 1000;
        const auto offset = (y * image->width + x) * 4;
        assert(image->rgba[offset] == 0 && image->rgba[offset+1] == 0 &&
               image->rgba[offset+2] == 0 && image->rgba[offset+3] == 255);
    }
    std::vector<std::uint8_t> grey(640 * 480, 255);
    // Four-pixel strokes and one-pixel details share an 8px monitor tile;
    // neither may be averaged into grey or leak into the adjacent background.
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 4; ++x) grey[y * 640 + x] = 0;
    grey[12 * 640 + 13] = 76;
    for (int scale : {1000, 1875, 2000}) {
        const std::array<std::uint8_t, 3> colour{111, 174, 106};
        const auto image = Display(grey, colour, scale);
        assert(image.width == 640u * scale / 1000);
        assert(image.height == 480u * scale / 1000);
        for (unsigned y = 0; y < image.height; ++y)
            for (unsigned x = 0; x < image.width; ++x) {
                const unsigned sx = x * 1000 / scale, sy = y * 1000 / scale;
                const auto source = grey[sy * 640 + sx];
                const int grid = sx % 8 == 0 || sy % 8 == 0 ? 240 : 255;
                const auto offset = (y * image.width + x) * 4;
                for (int ch = 0; ch < 3; ++ch)
                    assert(image.rgba[offset + ch] == source * grid * colour[ch] / (255 * 255));
                assert(image.rgba[offset + 3] == 255);
            }
        // Within a tile, both black and untouched background stay distinct.
        const int y = 3 * scale / 1000;
        assert(image.rgba[(y * image.width + 2 * scale / 1000) * 4] == 0);
        assert(image.rgba[(y * image.width + 6 * scale / 1000) * 4] == colour[0]);
    }
    // Overlay URI scales must obey the same bounds as the main LCD. Reject
    // malformed scales before sprite access or signed-to-unsigned allocation.
    lp_unity::Reader empty(lp_unity::RangeReader{});
    for (int scale : {std::numeric_limits<int>::min(), -1, 0, 999, 2001,
                      std::numeric_limits<int>::max()}) {
        assert(!RenderRing(0, false, scale));
        assert(!RenderRing(0, true, scale));
        assert(!RenderArrow(empty, 0, scale));
    }
    for (int scale : {1000, 1875, 2000}) {
        const auto ring = RenderRing(0, false, scale);
        const auto dot = RenderRing(0, true, scale);
        assert(ring && ring->width == 400u * scale / 1000 &&
               ring->rgba.size() == ring->width * ring->height * 4);
        assert(dot && dot->width == 40u * scale / 1000 &&
               dot->rgba.size() == dot->width * dot->height * 4);
    }
    Frames frames;
    const auto start = Frames::Clock::time_point{} + std::chrono::seconds{100};
    const auto ms = [](int n) { return std::chrono::milliseconds{n}; };
    assert(frames.Fallback(0, "clock", "", start).empty());
    frames.Ready("clock", start);
    assert(frames.Fallback(0, "clock", "", start + ms(120)).empty());
    assert(frames.Fallback(0, "calc0", "", start + ms(140)) == "clock");
    frames.Ready("calc0", start + ms(160));
    assert(frames.Fallback(0, "calc0", "", start + ms(170)) == "clock");
    assert(frames.Fallback(0, "calc0", "", start + ms(270)).empty());
    assert(frames.Fallback(0, "calc1", "", start + ms(290)) == "calc0");
    assert(frames.Fallback(2, "calc1-full", "calc0", start + ms(290)) == "calc0");
    assert(frames.Fallback(0, "calc1", "", start + ms(3300)).empty());
    // A revisited key can have Ready metadata but be evicted from the host.
    // Do not drop the last confirmed picture based on that old timestamp.
    assert(frames.Fallback(0, "clock", "", start + ms(3400)) == "calc0");
    assert(frames.Fallback(0, "clock", "", start + ms(3600)) == "calc0");
    frames.Ready("clock", start + ms(3700));
    assert(frames.Fallback(0, "clock", "", start + ms(3820)).empty());
    // Fullscreen reentry must use a currently pinned boxed backup, not the
    // previous visit's fullscreen picture, which may have been evicted.
    frames.Inactive(2);
    assert(frames.Fallback(2, "clock-full", "clock", start + ms(3840)) == "clock");
    // Themes/channels are independent; failure cannot show a different theme.
    assert(frames.Fallback(1, "teal", "", start).empty());
    for (int i = 0; i < 70; ++i) frames.Ready("later" + std::to_string(i), start + ms(4000+i));
    assert(frames.Fallback(3, "fresh", "clock", start + ms(4200)).empty());
    std::cout << "Poketch native pixel preservation, square-grid alignment and async fallback checks passed\n";
}
