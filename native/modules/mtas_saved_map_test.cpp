// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mtas_map.h"
#include "mtas_saved_map.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <zlib.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#pragma GCC diagnostic pop

namespace {
using Bytes = std::vector<std::uint8_t>;
void U32(Bytes& b, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void Set32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b.at(at + i) = static_cast<std::uint8_t>(v >> (8 * i));
}
void Append(void* out, void* data, int n) {
    auto& b = *static_cast<Bytes*>(out);
    auto* p = static_cast<std::uint8_t*>(data); b.insert(b.end(), p, p + n);
}
Bytes Encoded(bool jpeg, int side = 1024) {
    Bytes pixels(std::size_t(side) * side * 4, 0), encoded;
    const unsigned s = static_cast<unsigned>(side);
    for (unsigned y = 0; y < s; ++y)
        for (unsigned x = 0; x < s; ++x) {
            auto at = (y * s + x) * 4;
            pixels[at] = x * 1024 / s > 400 && x * 1024 / s < 600 && y * 1024 / s > 200 &&
                         y * 1024 / s < 800 ? 255 : 0;
            pixels[at + 3] = 255;
        }
    if (jpeg) assert(stbi_write_jpg_to_func(Append, &encoded, side, side, 4, pixels.data(), 95));
    else assert(stbi_write_png_to_func(Append, &encoded, side, side, 4, pixels.data(), side * 4));
    return encoded;
}
Bytes Save(const Bytes& image) {
    Bytes body; U32(body, 1); U32(body, 0); body.push_back(0); U32(body, 9);
    body.insert(body.end(), 9, 1); U32(body, image.size());
    body.insert(body.end(), image.begin(), image.end()); U32(body, 1); U32(body, 3); body.push_back(1);
    Set32(body, 4, body.size() - 8);
    Bytes save; U32(save, 0); U32(save, 10); U32(save, 123); U32(save, 456); U32(save, 0);
    U32(save, 1); std::string name = "Pathea.MapNs.MapModule";
    save.push_back(name.size()); save.insert(save.end(), name.begin(), name.end());
    U32(save, body.size()); save.insert(save.end(), body.begin(), body.end());
    Set32(save, 0, save.size() - 4); return save;
}
Bytes Gzip(const Bytes& save) {
    z_stream z{}; assert(deflateInit2(&z, 6, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    Bytes out(20 + compressBound(save.size()) + 32);
    std::memcpy(out.data(), save.data(), 20); Set32(out, 0, save.size() - 24); Set32(out, 16, 1);
    z.next_in = const_cast<Bytef*>(save.data() + 20); z.avail_in = save.size() - 20;
    z.next_out = out.data() + 20; z.avail_out = out.size() - 20;
    assert(deflate(&z, Z_FINISH) == Z_STREAM_END); out.resize(z.total_out + 20);
    deflateEnd(&z); return out;
}
Bytes Padded(Bytes save) {
    Set32(save, 20, 2); // unrelated module padding makes two different images the same file size
    save.push_back(4); save.insert(save.end(), {'T','e','s','t'});
    constexpr std::size_t total = 128 * 1024;
    assert(save.size() + 4 < total); U32(save, total - save.size() - 4);
    save.resize(total); Set32(save, 0, save.size() - 4); return save;
}
void DecodeTests() {
    for (bool jpeg : {false, true}) {
        const auto save = Save(Encoded(jpeg)); const auto fog = mtas::DecodeSavedFog(save);
        assert(fog && fog->width == 1024 && fog->height == 1024);
        assert(fog->rgba[0] == 0 && fog->rgba[(500 * 1024 + 500) * 4] > 250);
        const auto compressed = Gzip(save); const auto roundtrip = mtas::DecodeSavedFog(compressed);
        assert(roundtrip && roundtrip->rgba == fog->rgba);
        for (std::size_t n = 24; n < save.size(); n += 31) {
            Bytes truncated(save.begin(), save.begin() + n);
            Set32(truncated, 0, n - 4); assert(!mtas::DecodeSavedFog(truncated));
        }
        for (std::size_t n : {std::size_t{0}, std::size_t{20}, save.size() - 1})
            assert(!mtas::DecodeSavedFog(std::span{save}.first(n)));
        for (const auto [at, value] : {std::pair{0u, 0u}, {4u, 11u}, {16u, 2u}, {20u, 513u},
                                      {50u, 2u}, {54u, 0u}, {59u, 10u}, {72u, 0xffffffffu}}) {
            auto bad = save; Set32(bad, at, value); assert(!mtas::DecodeSavedFog(bad));
        }
        auto bad = compressed; bad.back() ^= 1; assert(!mtas::DecodeSavedFog(bad));
        auto encoded = Encoded(jpeg); encoded.back() ^= 1;
        assert(!mtas::DecodeSavedFog(Save(encoded)));
        auto trailing = save; trailing.push_back(0); Set32(trailing, 0, trailing.size() - 4);
        assert(!mtas::DecodeSavedFog(trailing));
        auto duplicate = save; Set32(duplicate, 20, 2);
        duplicate.insert(duplicate.end(), save.begin() + 24, save.end());
        Set32(duplicate, 0, duplicate.size() - 4); assert(!mtas::DecodeSavedFog(duplicate));
    }
    assert(!mtas::DecodeSavedFog(Save(Bytes{0,1,2,3,4,5,6,7})));
    // Switch V1.4.2 saves store the 256 px mask render texture directly (fixture-pc1).
    assert(mtas::DecodeSavedFog(Save(Encoded(false, 256))));
    const auto small = mtas::DecodeSavedFog(Gzip(Save(Encoded(false, 256))));
    assert(small && small->width == 256 && small->height == 256);
    for (int side : {128, 300, 2048})
        assert(!mtas::DecodeSavedFog(Save(Encoded(false, side))));
    std::puts("PASS: JPEG/PNG, gzip round trips and malformed framing/image rejection");
}
void CacheTests() {
    auto save = Padded(Save(Encoded(false)));
    bool missing = false, torn = false; unsigned reads{};
    const mtas::SaveMapRead read = [&](const char*, std::uint64_t, void* out, std::size_t n) {
        if (missing) return std::size_t{0};
        if (!out) return save.size();
        if (n != save.size()) return std::size_t{0};
        std::memcpy(out, save.data(), n);
        if (torn && (++reads % 2 == 0)) static_cast<std::uint8_t*>(out)[8] ^= 1;
        return n;
    };
    mtas::SavedMapCache cache;
    assert(cache.Poll("MTAS/test.save", true, read) && cache.Fog());
    const auto rev = cache.Revision(); assert(!cache.Poll("MTAS/test.save", false, read));
    assert(cache.Revision() == rev);
    save[8] ^= 1; assert(!cache.Poll("MTAS/test.save", false, read)); // changed file, same fog
    assert(cache.Revision() == rev);
    const auto original_size = save.size(); save = Padded(Save(Encoded(true)));
    assert(save.size() == original_size && cache.Poll("MTAS/test.save", false, read));
    torn = true; assert(cache.Poll("MTAS/test.save", false, read) && !cache.Fog());
    torn = false; assert(cache.Poll("MTAS/test.save", false, read) && cache.Fog());
    missing = true; assert(cache.Poll("MTAS/new.save", false, read) && !cache.Fog());
    missing = false; assert(cache.Poll("MTAS/test.save", false, read));
    assert(cache.Poll("../test.save", false, read) && !cache.Fog());
    assert(cache.Poll("MTAS/test.save", false, read));
    save[4] = 0; assert(cache.Poll("MTAS/test.save", false, read) && !cache.Fog());
    save[4] = 10; assert(cache.Poll("MTAS/test.save", false, read));
    const mtas::SaveMapRead throwing = [](const char*, std::uint64_t, void*, std::size_t) -> std::size_t {
        throw std::runtime_error("read failed");
    };
    assert(cache.Poll("MTAS/test.save", false, throwing) && !cache.Fog());
    std::puts("PASS: unchanged/same-size change, slot change, missing and torn reads fail closed");
}
void FixtureTests() {
    for (const char* key : {"MTAS_TOWN_SAVE", "MTAS_LATE_SAVE", "MTAS_PC1_SAVE", "MTAS_POISONED_SAVE"}) {
        const auto* path = std::getenv(key); if (!path) continue;
        std::ifstream in(path, std::ios::binary); assert(in);
        Bytes bytes{std::istreambuf_iterator<char>{in}, {}};
        const auto fog = mtas::DecodeSavedFog(bytes);
        assert(fog); std::printf("PASS: optional %s framed decode\n", key);
        // Genuine saves keep every tile; the Thor New Game save (fog = uninitialised memory,
        // written without the RT flushed back) loses every tile that held any "reveal".
        int rejected = -1;
        const auto trusted = mtas::TrustedFogBaseline(*fog, &rejected);
        std::size_t kept = 0;
        for (std::size_t i = 0; i < trusted.rgba.size(); i += 4) kept += trusted.rgba[i] >= mtas::FogBaselineMinRed;
        const bool poisoned = std::strcmp(key, "MTAS_POISONED_SAVE") == 0;
        std::printf("      %s: %d noise tiles rejected, %zu explored cells kept\n", key, rejected, kept);
        assert(poisoned ? rejected > 200 && kept == 0 : rejected == 0 && kept > 0);
    }
}
// A genuine fog: soft discs (the native brush) on a cleared mask, top-down.
ce_unity::Image SoftDiscs(std::uint32_t side, std::initializer_list<std::array<float, 3>> discs) {
    ce_unity::Image fog{side, side, std::vector<std::uint8_t>(std::size_t(side) * side * 4, 0)};
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            float best = 0;
            for (const auto& [cx, cy, r] : discs) {
                const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                best = std::max(best, std::clamp((r - d) / 4.0f, 0.0f, 1.0f)); // 4-cell soft edge
            }
            auto* p = &fog.rgba[(std::size_t(y) * side + x) * 4];
            p[0] = static_cast<std::uint8_t>(best * 255); p[3] = 255;
        }
    return fog;
}
// Uninitialised GPU memory as seen in the Thor save: vertical stripes and blocks of random bytes.
void Garbage(ce_unity::Image& fog, std::uint32_t x0, std::uint32_t x1) {
    std::uint32_t seed = 12345;
    const auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 24; };
    std::vector<std::uint8_t> column(fog.width);
    for (auto& c : column) c = next() & 1 ? 255 : static_cast<std::uint8_t>(next());
    for (std::uint32_t y = 0; y < fog.height; ++y) {
        if (y % 24 == 0) for (auto& c : column) if (next() < 64) c = static_cast<std::uint8_t>(next());
        for (std::uint32_t x = x0; x < x1; ++x) fog.rgba[(std::size_t(y) * fog.width + x) * 4] = column[x];
    }
}
void ExploredRecordTests() {
    // Genuine soft fog keeps every tile and its exact values.
    const auto clean = SoftDiscs(256, {{{100, 90, 30}}, {{160, 100, 12}}});
    int rejected = -1;
    const auto trusted = mtas::TrustedFogBaseline(clean, &rejected);
    assert(rejected == 0 && trusted.width == 256 && trusted.rgba == [&] {
        auto copy = clean; for (std::size_t i = 0; i < copy.rgba.size(); i += 4) copy.rgba[i + 1] = copy.rgba[i + 2] = copy.rgba[i];
        return copy.rgba; }());
    // A hard-edged (thresholded) genuine mask also passes: one contour per tile is not noise.
    auto hard = clean;
    for (std::size_t i = 0; i < hard.rgba.size(); i += 4) hard.rgba[i] = hard.rgba[i] >= 128 ? 255 : 0;
    mtas::TrustedFogBaseline(hard, &rejected); assert(rejected == 0);
    // Stripes over the whole left half: every tile there that held reveal is dropped; the disc on
    // the right, two tiles clear of the garbage, survives untouched.
    auto mixed = SoftDiscs(256, {{{200, 128, 20}}});
    Garbage(mixed, 0, 128);
    const auto kept = mtas::TrustedFogBaseline(mixed, &rejected);
    assert(rejected >= 8 * 16 - 2);
    for (std::uint32_t y = 0; y < 256; ++y)
        for (std::uint32_t x = 0; x < 256; ++x) {
            const auto at = (std::size_t(y) * 256 + x) * 4;
            if (x < 128) assert(kept.rgba[at] == 0);
            if (x >= 160) assert(kept.rgba[at] == mixed.rgba[at]);
        }
    // A 1024 save is box-resampled to the 256 RT grid; malformed input yields nothing.
    const auto big = mtas::TrustedFogBaseline(SoftDiscs(1024, {{{400, 360, 120}}}), &rejected);
    assert(rejected == 0 && big.width == 256 && big.rgba[(90 * 256 + 100) * 4] == 255 && big.rgba[0] == 0);
    ce_unity::Image odd{100, 50, Bytes(100 * 50 * 4, 255)};
    assert(mtas::TrustedFogBaseline(odd).rgba.empty());

    // ExploredCells: the baseline grown by FogBaselineDilate, a FogStampReach disc per stamp.
    ce_unity::Image dot{256, 256, Bytes(256 * 256 * 4, 0)};
    dot.rgba[(40 * 256 + 50) * 4] = 255;
    auto cells = mtas::ExploredCells(&dot, nullptr, 0);
    assert(cells.size() == 256 * 256 && cells[40 * 256 + 50] == 255);
    assert(cells[40 * 256 + 50 + mtas::FogBaselineDilate] == 255 && cells[40 * 256 + 50 + mtas::FogBaselineDilate + 1] == 0);
    dot.rgba[(40 * 256 + 50) * 4] = mtas::FogBaselineMinRed - 1;
    cells = mtas::ExploredCells(&dot, nullptr, 0);
    assert(std::count(cells.begin(), cells.end(), 255) == 0);
    std::vector<std::uint8_t> bits(256 * 32, 0);
    bits[130 * 32 + 137 / 8] |= 1u << (137 % 8); // stamp at x 130, v-row 137 -> image row 118
    cells = mtas::ExploredCells(nullptr, bits.data(), bits.size());
    const int R = mtas::FogStampReach;
    assert(cells[118 * 256 + 130] == 255 && cells[118 * 256 + 130 + R] == 255 && cells[118 * 256 + 130 + R + 1] == 0);
    assert(cells[(118 - R) * 256 + 130] == 255 && cells[(118 - R - 1) * 256 + 130] == 0);
    assert(cells[(118 - 12) * 256 + 130 + 12] == 0); // a disc, not a square
    assert(cells[137 * 256 + 130] == 0);             // not the unflipped row
    cells = mtas::ExploredCells(nullptr, bits.data(), 100);
    assert(std::count(cells.begin(), cells.end(), 255) == 0); // wrong size = absent
    std::puts("PASS: trusted baseline (genuine kept, stripes dropped, resample, malformed) and explored cells");
}
}
void MapTileTests() {
    // Majority rule on the synthetic mask: revealed x 400..600, y 200..800 of 1024 (top-down).
    const auto fog = mtas::DecodeSavedFog(Save(Encoded(false)));
    const auto tiles = mtas::TilesFromFog(*fog);
    // Tile column 7 spans x 448..511 (fully inside), column 6 spans 384..447 (48 of 64 inside:
    // majority), column 9 spans 576..639 (23 of 64: no). Rows 4..11 are inside y 200..800 at
    // least half; row 3 (192..255, 55 of 64 inside) yes; row 12 (768..831, 31 of 64) no.
    assert(tiles.test(4 * 16 + 7) && tiles.test(4 * 16 + 6) && !tiles.test(4 * 16 + 9));
    assert(tiles.test(3 * 16 + 7) && !tiles.test(12 * 16 + 7) && !tiles.test(0));
    const auto small = mtas::TilesFromFog(*mtas::DecodeSavedFog(Save(Encoded(false, 256))));
    assert(small == tiles);
    ce_unity::Image odd; odd.width = 100; odd.height = 100; odd.rgba.assign(100 * 100 * 4, 255);
    assert(mtas::TilesFromFog(odd).none()); // fail closed
    // Coordinates: v up, row 0 = top.
    assert(mtas::TileAtUv(0.0f, 0.999f) == 0);
    assert(mtas::TileAtUv(0.999f, 0.0f) == 15 * 16 + 15);
    assert(mtas::TileAtUv(0.507f, 0.535f) == 7 * 16 + 8);
    assert(!mtas::TileAtUv(1.0f, 0.5f) && !mtas::TileAtUv(-0.1f, 0.5f) &&
           !mtas::TileAtUv(0.5f, std::nanf("")));
    std::vector<std::uint8_t> bits(256 * 32, 0);
    bits[130 * 32 + 137 / 8] |= 1u << (137 % 8); // x=130 (u .508), y=137 (v .535)
    const auto history = mtas::TilesFromHistory(bits.data(), bits.size());
    assert(history.count() == 1 && history.test(7 * 16 + 8));
    assert(mtas::TilesFromHistory(bits.data(), 100).none());
    int col{}, row{}; std::uint32_t px{};
    assert(mtas::MapTileKey(7 * 16 + 8, 128) == "module:mtas:map:8:7:128");
    assert(mtas::ParseMapTileKey("8:7:128", col, row, px) && col == 8 && row == 7 && px == 128);
    for (const char* bad : {"16:0:128", "0:0:16", "0:0:512", "a:0:128", "0:0", "0:0:128:1", ""})
        assert(!mtas::ParseMapTileKey(bad, col, row, px));
    mtas::MapFog state;
    state.Observe({1, 2, "a.save"});
    assert(state.Explored().none() && !state.HasBaseline()); // nothing known = all fogged
    state.SetBaseline(tiles); state.Reveal(0);
    assert(state.Explored() == (tiles | mtas::MapTileMask{}.set(0)));
    state.Observe({1, 2, "a.save"}); assert(state.Explored().test(0)); // same session kept
    state.Observe({1, 3, "a.save"}); // reload re-creates the history array: drop everything
    assert(state.Explored().none() && !state.HasBaseline());
    state.Reveal(5); state.Observe({1, 3, "b.save"}); assert(state.Explored().none());
    std::puts("PASS: tile majority, orientation, history, keys and session reset");
}
void MarkerVisibilityTests() {
    ce_unity::Image fog; fog.width = fog.height = 256; fog.rgba.assign(256 * 256 * 4, 0);
    const float u = 130.5f / 256, v = 137.5f / 256;
    assert(!mtas::MarkerExplored(nullptr, nullptr, 0, u, v));
    assert(!mtas::MarkerExplored(&fog, nullptr, 0, u, v));
    fog.rgba[(118 * 256 + 130) * 4] = 249;
    assert(!mtas::MarkerExplored(&fog, nullptr, 0, u, v));
    fog.rgba[(118 * 256 + 130) * 4] = 250;
    assert(mtas::MarkerExplored(&fog, nullptr, 0, u, v));
    assert(!mtas::MarkerExplored(&fog, nullptr, 0, 1, v));
    std::vector<std::uint8_t> history(8192, 0);
    history[130 * 32 + 137 / 8] |= 1 << (137 % 8);
    assert(mtas::MarkerExplored(nullptr, history.data(), history.size(), u, v));
    assert(!mtas::MarkerExplored(nullptr, history.data(), 8191, u, v));
    assert(!mtas::MarkerExplored(nullptr, history.data(), history.size(), u, std::nanf("")));
    std::puts("PASS: marker reveal is point-based, correctly oriented and fail closed");
}
int main() { DecodeTests(); CacheTests(); MapTileTests(); MarkerVisibilityTests(); ExploredRecordTests(); FixtureTests(); }
