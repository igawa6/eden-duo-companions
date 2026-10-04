// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// p5r_font.h tests. Part 1 builds a tiny synthetic 8bpp .FNT (2x2 cells, hand-made Huffman tree)
// so the bit order, leaf rule and palette lookup are pinned without shipping game data. Part 2,
// when P5R_FNT is set, decodes the real file and checks it against the Python reference decoder's
// metrics JSON (P5R_FNT_REF) glyph by glyph, plus the atlas contract the host relies on.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include "p5r_font.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL " << what << '\n';
        ++failures;
    }
}
template <class T>
void put(std::vector<uint8_t>& v, T value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    v.insert(v.end(), p, p + sizeof(T));
}

// Two glyphs of 2x2 = 4 symbols each, palette index 0 or 1. Huffman: root(0) -> bit0 node1
// (leaf 0), bit1 node2 (leaf 1). Stream bits (LSB first): glyph0 = 0,1,1,0  glyph1 = 1,1,1,1.
std::vector<uint8_t> Synthetic() {
    std::vector<uint8_t> f;
    put<uint32_t>(f, 0x20);
    put<uint32_t>(f, 0); // file size (informational)
    f.resize(14, 0);
    put<uint16_t>(f, 2); // glyph count
    put<uint16_t>(f, 2); // cell w
    put<uint16_t>(f, 2); // cell h
    put<uint16_t>(f, 4); // bytes per glyph -> 8bpp
    f.resize(0x20, 0);
    for (int i = 0; i < 256; ++i) { // palette: entry 1 = coverage 200
        const uint8_t g = i == 1 ? 200 : 0;
        f.insert(f.end(), {g, g, g, 0xFF});
    }
    put<uint32_t>(f, 4); // cuts
    f.insert(f.end(), {0, 2, 1, 2});
    put<uint32_t>(f, 3); // unknown block
    f.insert(f.end(), {9, 9, 9});
    put<uint32_t>(f, 0); // reserved x2
    put<uint32_t>(f, 0);
    const uint8_t stream = 0b11110110; // bits LSB first: 0,1,1,0,1,1,1,1
    put<uint32_t>(f, 0x20);            // compressed header size
    put<uint32_t>(f, 18);              // dict: 3 nodes
    put<uint32_t>(f, 1);               // comp size
    put<uint32_t>(f, 0);
    put<uint16_t>(f, 4);
    put<uint16_t>(f, 0);
    put<uint32_t>(f, 3); // positions
    put<uint32_t>(f, 0);
    put<uint32_t>(f, 8);
    // nodes {?, child0, child1}
    for (uint16_t v : {uint16_t(0), uint16_t(1), uint16_t(2), uint16_t(0), uint16_t(0), uint16_t(0),
                       uint16_t(0), uint16_t(0), uint16_t(1)})
        put<uint16_t>(f, v);
    put<uint32_t>(f, 0);
    put<uint32_t>(f, 4);
    put<uint32_t>(f, 8);
    f.push_back(stream);
    return f;
}

std::vector<uint8_t> ReadFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
} // namespace

int main() {
    {
        const auto f = Synthetic();
        p5r_font::Decoded d;
        check(p5r_font::Decode(f.data(), f.size(), d), "synthetic decodes");
        check(d.count == 2 && d.cell_w == 2 && d.cell_h == 2, "synthetic shape");
        const std::vector<uint8_t> want{0, 200, 200, 0, 200, 200, 200, 200};
        check(d.coverage == want, "synthetic bit order + palette");
        check(d.cuts.size() == 2 && d.cuts[1][0] == 1 && d.cuts[1][1] == 2, "synthetic cuts");
        // Every truncation must be rejected, never read out of bounds.
        for (size_t n = 0; n < f.size(); ++n) {
            p5r_font::Decoded t;
            check(!p5r_font::Decode(f.data(), n, t), "truncated input rejected");
        }
        auto bad = f;
        bad[20] = 3; // bytes per glyph 3 -> 6 bpp: unsupported
        check(!p5r_font::Decode(bad.data(), bad.size(), d), "odd bpp rejected");
        check(p5r_font::AtlasKeyPath("module:p5r_font:file:ui/font/FONT0.FNT") ==
                  "file:ui/font/FONT0.FNT",
              "key path");
        check(p5r_font::AtlasKeyPath("module:p5r_font:file:../x").empty(), "key escape rejected");
        check(p5r_font::AtlasKeyPath("module:p5r_font:romfs:x").empty(), "key romfs rejected");
    }
    if (const char* path = std::getenv("P5R_FNT")) {
        const auto bytes = ReadFile(path);
        p5r_font::Decoded d;
        check(p5r_font::Decode(bytes.data(), bytes.size(), d), "real font decodes");
        check(d.cell_w == 48 && d.cell_h == 48 && d.count == p5r_font::MaxGlyphs, "real shape");
        uint32_t baseline{}, cap{};
        check(p5r_font::CapMetrics(d, baseline, cap) && baseline == 37 && cap == 27,
              "real cap metrics (H rows 10..36)");
        if (const char* ref = std::getenv("P5R_FNT_REF")) {
            // Reference: raw coverage of glyphs 0..MaxGlyphs-1 from an independent decoder (alpha
            // plane).
            const auto want = ReadFile(ref);
            check(want.size() == d.coverage.size(), "reference size");
            size_t diff = 0;
            for (size_t i = 0; i < want.size() && i < d.coverage.size(); ++i)
                diff += want[i] != d.coverage[i];
            check(diff == 0, "coverage identical to Python reference");
            std::cout << "reference compare: " << want.size() << " bytes, " << diff << " differ\n";
        }
        p5r_font::Atlas a;
        check(p5r_font::BuildAtlas(d, a), "atlas builds");
        check(a.glyphs.size() == p5r_font::LastCodepoint - p5r_font::FirstCodepoint + 1, "run");
        check(a.line_height == 27, "line height is cap height");
        check(a.glyphs['h' - 0x20].bearing_y == 33,
              "glyphs lowered by the quote/ascender overshoot (cap top 10, ink top 6)");
        const auto& q = a.glyphs['?' - 0x20];
        const auto& eq = a.glyphs['=' - 0x20];
        check(q.w > 0 && q.advance > 0 && eq.w > 0, "'?' and '=' present");
        check(a.glyphs[0].w == 0 && a.glyphs[0].advance == 13, "space advance only");
        check(a.glyphs[0x80 - 0x20].advance == 0, "unmapped slot empty");
        check(a.glyphs[0xA5 - 0x20].w > 0, "yen mapped");
        if (const char* dump = std::getenv("P5R_FNT_DUMP")) { // RGBA raw, for eyeballing
            std::ofstream(dump, std::ios::binary)
                .write(reinterpret_cast<const char*>(a.rgba.data()),
                       static_cast<std::streamsize>(a.rgba.size()));
            std::cout << "atlas " << a.width << "x" << a.height << " -> " << dump << '\n';
        }
    } else {
        std::cout << "P5R_FNT not set: real-font checks skipped\n";
    }
    if (failures == 0)
        std::cout << "p5r_font: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
