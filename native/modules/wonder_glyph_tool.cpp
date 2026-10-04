// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-side check of the Wonder companion PictureFont glyphs, run against a romfs dump:
//   wonder-glyph-tool <romfs> <out_dir>
// Writes <out_dir>/glyph_<name>.pam per named glyph, <out_dir>/contact.pam (every PictureFont
// glyph on a grey grid, codepoint order) and <out_dir>/contact.txt (one line per cell:
// "x y w h U+XXXX name") so a labelled PNG can be drawn from it. Exercises the module's code.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "wonder_glyphs.h"

using namespace WonderAssets;

static bool WritePam(const std::string& path, const Image& img) {
    std::ofstream o(path, std::ios::binary);
    o << "P7\nWIDTH " << img.width << "\nHEIGHT " << img.height
      << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    o.write(reinterpret_cast<const char*>(img.rgba.data()),
            static_cast<std::streamsize>(img.rgba.size()));
    return static_cast<bool>(o);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <romfs> <out_dir>\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1], out = argv[2];
    const RomfsReader read = [&](const std::string& path) -> std::optional<std::vector<std::uint8_t>> {
        std::ifstream f(root + path, std::ios::binary);
        if (!f)
            return std::nullopt;
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
    };
    const AstcDecoder astc{};
    const auto codes = PictureFontCodepoints(read, astc);
    if (codes.empty()) {
        std::fprintf(stderr, "PictureFont not found or not decodable\n");
        return 1;
    }
    const auto name_of = [](char32_t c) -> std::string {
        for (const auto& g : PictureGlyphs())
            if (g.codepoint == c)
                return std::string{g.name};
        return "?";
    };
    constexpr std::uint32_t cols = 5, cell = 96, pad = 20, label = 28;
    const std::uint32_t rows = static_cast<std::uint32_t>((codes.size() + cols - 1) / cols);
    Image sheet{cols * cell, rows * (cell + label), {}};
    sheet.rgba.assign(std::size_t{sheet.width} * sheet.height * 4, 0);
    for (std::size_t i = 0; i < sheet.rgba.size(); i += 4) {
        const std::uint32_t p = static_cast<std::uint32_t>(i / 4);
        const bool checker = ((p % sheet.width) / 8 + (p / sheet.width) / 8) % 2;
        sheet.rgba[i] = sheet.rgba[i + 1] = sheet.rgba[i + 2] = checker ? 0x50 : 0x40;
        sheet.rgba[i + 3] = 0xFF;
    }
    std::ofstream txt(out + "/contact.txt");
    int failed = 0;
    for (std::size_t n = 0; n < codes.size(); ++n) {
        const auto img = LoadPictureGlyph(read, codes[n], astc);
        const std::string name = name_of(codes[n]);
        char cp[16];
        std::snprintf(cp, sizeof cp, "U+%04X", static_cast<unsigned>(codes[n]));
        if (!img) {
            std::printf("%s %s: FAILED\n", cp, name.c_str());
            ++failed;
            continue;
        }
        const std::uint32_t cx = static_cast<std::uint32_t>(n % cols) * cell;
        const std::uint32_t cy = static_cast<std::uint32_t>(n / cols) * (cell + label);
        const std::uint32_t ox = cx + (cell - img->width) / 2, oy = cy + pad / 2;
        for (std::uint32_t y = 0; y < img->height && oy + y < sheet.height; ++y)
            for (std::uint32_t x = 0; x < img->width && ox + x < sheet.width; ++x) {
                const std::uint8_t* s = img->rgba.data() + (std::size_t{y} * img->width + x) * 4;
                std::uint8_t* d =
                    sheet.rgba.data() + (std::size_t{oy + y} * sheet.width + ox + x) * 4;
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<std::uint8_t>((s[c] * s[3] + d[c] * (255 - s[3])) / 255);
            }
        txt << cx << ' ' << cy + cell - pad / 2 << ' ' << cell << ' ' << label << ' ' << cp << ' '
            << name << '\n';
        if (name != "?")
            WritePam(out + "/glyph_" + name + ".pam", *img);
        std::printf("%s %-7s %ux%u\n", cp, name.c_str(), img->width, img->height);
    }
    // Every named glyph must resolve through the name API too.
    for (const auto& g : PictureGlyphs())
        if (!LoadPictureGlyph(read, g.name, astc)) {
            std::printf("name %s: FAILED\n", std::string{g.name}.c_str());
            ++failed;
        }
    WritePam(out + "/contact.pam", sheet);
    return failed ? 1 : 0;
}
