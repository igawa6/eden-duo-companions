// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-side check of the Wonder companion's game-font atlas, run against a romfs dump:
//   wonder-font-tool <romfs> <spec> <out_dir> [text_scale...]
// Writes <out_dir>/<member>_<cap>_atlas.pam (the atlas as served, RGBA) and, per text scale
// (default: the 1:1 scale cap/5, plus 3), <member>_<cap>_sample_s<scale>.pam: a sample line laid
// out with the host Canvas::DrawText arithmetic (pen/baseline floats, nearest-neighbour glyph
// scaling) on dark grey, with the cap line (blue) and baseline (red) marked. Also prints the
// metrics and an FNV-1a hash of the atlas + glyph table (to check determinism).
// It exercises exactly the code the module ships.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "wonder_font.h"

using namespace WonderFont;

static std::optional<std::vector<std::uint8_t>> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static bool WritePam(const std::string& path, const WonderAssets::Image& img) {
    std::ofstream o(path, std::ios::binary);
    o << "P7\nWIDTH " << img.width << "\nHEIGHT " << img.height
      << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    o.write(reinterpret_cast<const char*>(img.rgba.data()),
            static_cast<std::streamsize>(img.rgba.size()));
    return static_cast<bool>(o);
}

static std::uint32_t NextCodepoint(std::string_view s, std::size_t& i) {
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80)
        return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    std::uint32_t cp = c & (0x3F >> extra);
    while (extra-- > 0 && i < s.size())
        cp = cp << 6 | (static_cast<unsigned char>(s[i++]) & 0x3F);
    return cp;
}

// Canvas::DrawText (mod_ui_text.cpp) + DrawImageRegion's nearest sampling, alpha-tinted white.
static WonderAssets::Image RenderSample(const FontAtlas& f, std::string_view text, int scale) {
    const float wanted = static_cast<float>(scale) * 5.0f;
    const float ratio = wanted / static_cast<float>(f.line_height);
    const auto find = [&](std::uint32_t cp) -> const EdenDsmodFontGlyph* {
        if (cp >= f.first_codepoint && cp - f.first_codepoint < f.glyphs.size())
            return &f.glyphs[cp - f.first_codepoint];
        return nullptr;
    };
    float width = 0.0f;
    for (std::size_t i = 0; i < text.size();) {
        const auto* g = find(NextCodepoint(text, i));
        width += g ? static_cast<float>(g->advance) * ratio : wanted * 0.5f;
    }
    const int margin = static_cast<int>(wanted);
    WonderAssets::Image img;
    img.width = static_cast<std::uint32_t>(width) + 2 * margin;
    img.height = static_cast<std::uint32_t>(wanted * 2.2f) + 2 * margin / 2;
    img.rgba.resize(std::size_t{img.width} * img.height * 4);
    for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i] = img.rgba[i + 1] = img.rgba[i + 2] = 0x30;
        img.rgba[i + 3] = 0xFF;
    }
    const int x = margin, y = margin / 2 + static_cast<int>(wanted * 0.4f);
    const float baseline = static_cast<float>(y) + wanted;
    const auto hline = [&](int row, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        if (row < 0 || row >= static_cast<int>(img.height))
            return;
        for (std::uint32_t c = 0; c < img.width; ++c) {
            auto* p = &img.rgba[(std::size_t(row) * img.width + c) * 4];
            p[0] = r, p[1] = g, p[2] = b;
        }
    };
    hline(y, 0x30, 0x60, 0xE0);                           // cap line (widget top)
    hline(static_cast<int>(baseline), 0xE0, 0x30, 0x30); // baseline
    float pen = static_cast<float>(x);
    for (std::size_t i = 0; i < text.size();) {
        const auto* g = find(NextCodepoint(text, i));
        if (!g) {
            pen += wanted * 0.5f;
            continue;
        }
        if (g->w > 0 && g->h > 0) {
            const int gx = static_cast<int>(pen + static_cast<float>(g->bearing_x) * ratio);
            const int gy = static_cast<int>(baseline - static_cast<float>(g->bearing_y) * ratio);
            const int gw = std::max(1, static_cast<int>(static_cast<float>(g->w) * ratio));
            const int gh = std::max(1, static_cast<int>(static_cast<float>(g->h) * ratio));
            for (int row = 0; row < gh; ++row) {
                const int sy = g->y + static_cast<int>(static_cast<long long>(row) * g->h / gh);
                for (int col = 0; col < gw; ++col) {
                    const int sx = g->x + static_cast<int>(static_cast<long long>(col) * g->w / gw);
                    const int px = gx + col, py = gy + row;
                    if (px < 0 || py < 0 || px >= static_cast<int>(img.width) ||
                        py >= static_cast<int>(img.height))
                        continue;
                    const unsigned a = f.atlas.rgba[(std::size_t(sy) * f.atlas.width + sx) * 4 + 3];
                    auto* p = &img.rgba[(std::size_t(py) * img.width + px) * 4];
                    for (int k = 0; k < 3; ++k)
                        p[k] = static_cast<std::uint8_t>((p[k] * (255 - a) + 255 * a + 127) / 255);
                }
            }
        }
        pen += static_cast<float>(g->advance) * ratio;
    }
    return img;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <romfs> <spec> <out_dir> [text_scale...]\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1], spec = argv[2], out = argv[3];
    const WonderAssets::RomfsReader read = [&](const std::string& p) { return ReadFile(root + p); };
    const auto font = BuildFontAtlas(read, spec);
    if (!font) {
        std::fprintf(stderr, "could not build '%s'\n", spec.c_str());
        return 1;
    }
    const auto again = BuildFontAtlas(read, spec);
    const auto canon = ParseFontSpec(std::span{reinterpret_cast<const std::uint8_t*>(spec.data()),
                                               spec.size()});
    std::string base = canon ? *canon : spec;
    std::replace(base.begin(), base.end(), ':', '_');

    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            h = (h ^ static_cast<const std::uint8_t*>(p)[i]) * 1099511628211ull;
    };
    mix(font->atlas.rgba.data(), font->atlas.rgba.size());
    mix(font->glyphs.data(), font->glyphs.size() * sizeof(EdenDsmodFontGlyph));
    const auto& A = font->glyphs['A' - font->first_codepoint];
    std::printf("%s: atlas %ux%u, line_height %u, first 0x%X, %zu glyphs, cached %s, hash %016llx\n"
                "  'A' box %ux%u bearing (%d,%d) advance %u\n",
                base.c_str(), font->atlas.width, font->atlas.height, font->line_height,
                font->first_codepoint, font->glyphs.size(), again == font ? "yes" : "NO",
                static_cast<unsigned long long>(h), A.w, A.h, A.bearing_x, A.bearing_y, A.advance);

    if (!WritePam(out + "/" + base + "_atlas.pam", font->atlas))
        return 1;
    std::vector<int> scales;
    for (int i = 4; i < argc; ++i)
        scales.push_back(std::atoi(argv[i]));
    if (scales.empty()) {
        scales.push_back(std::max<int>(1, static_cast<int>(font->line_height / 5)));
        if (scales.front() != 3)
            scales.push_back(3);
    }
    constexpr std::string_view sample =
        "SUPER MARIO BROS. WONDER 1.2.1 • WORLD 9 COINS 026 × café “Aq”";
    for (const int s : scales) {
        if (s <= 0)
            continue;
        if (!WritePam(out + "/" + base + "_sample_s" + std::to_string(s) + ".pam",
                      RenderSample(*font, sample, s)))
            return 1;
    }
    return 0;
}
