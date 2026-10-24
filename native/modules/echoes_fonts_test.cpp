// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_fonts.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
namespace {
size_t checks = 0;
void Check(bool b) {
  ++checks;
  if (!b) {
    std::cerr << "Failed check " << checks << '\n';
    std::abort();
  }
}
std::vector<uint8_t> File(const char *path) {
  std::ifstream f(path, std::ios::binary);
  Check(bool(f));
  return {(std::istreambuf_iterator<char>(f)), {}};
}
echoes::NativeFonts Input(std::vector<uint8_t> bytes) {
  echoes::NativeFonts n;
  n.compatible = n.locale_valid = n.body_font_supported =
      n.standard_font_valid = true;
  n.standard_font = std::move(bytes);
  return n;
}
} // namespace
int main() {
  echoes::FontAtlas atlas;
  auto cps = echoes::EnglishBodyCodepoints();
  Check(!echoes::BodyAtlas({}, cps, atlas));
  Check(!echoes::BodyAtlas(Input({0, 1, 0, 0}), cps, atlas));
  echoes::HeadingFont heading;
  Check(!echoes::ParseHeading({}, heading));
  std::vector<uint8_t> junk(128, 0);
  Check(!echoes::ParseHeading(junk, heading));
  acnh::Image image;
  Check(!echoes::RenderHeading({}, "A", {255, 255, 255, 255}, 256, image));
  // A hand-built one-cell font exercises composition separately from decoding.
  heading.sheet = acnh::Image(4, 4);
  heading.baseline = 2;
  heading.line_feed = 4;
  heading.glyphs['A'] = {0, 0, 2, 3, -1, 2, 3};
  for (size_t p = 0; p < heading.sheet.rgba.size(); p += 4)
    heading.sheet.rgba[p + 3] = 128;
  Check(echoes::RenderHeading(heading, "AA", {10, 20, 30, 128}, 7, image));
  Check(image.w == 7 && image.h == 4 && image.rgba[0] == 10 &&
        image.rgba[3] == 64);
  Check(echoes::RenderCompactHeading(heading,"AA",{10,20,30,128},6,image));
  Check(image.w==6 && image.h==3 && image.rgba[0]==10 && image.rgba[3]<=64);
  Check(!echoes::RenderCompactHeading(heading,"AA",{10,20,30,128},5,image) && image.Empty());
  Check(!echoes::RenderHeading(heading, "AA", {255, 255, 255, 255}, 6, image) &&
        image.Empty());
  Check(!echoes::RenderHeading(heading, "B", {255, 255, 255, 255}, 256, image));
  for (auto text : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
                    std::string("\xf4\x90\x80\x80"), std::string("A\nA"),
                    std::string("\xe2\x80")})
    Check(!echoes::RenderHeading(heading, text, {255, 255, 255, 255}, 256,
                                 image));
  if (const char *path = std::getenv("ECHOES_FONT_TTF")) {
    auto bytes = File(path);
    auto native = Input(bytes);
    Check(echoes::BodyAtlas(native, cps, atlas));
    Check(atlas.image.w == 1024 && atlas.image.h % 512 == 0 &&
          atlas.image.rgba.size() <= 16 * 1024 * 1024 &&
          atlas.glyphs.size() > 8192 && atlas.line_height == 29);
    Check(echoes::RenderBody(atlas,"127 / 127",{244,240,217,255},400,image));
    Check(!image.Empty() && image.h>=29 && image.h<60);
    Check(!echoes::RenderBody(atlas,"127 / 127",{244,240,217,255},10,image) && image.Empty());
    Check(!echoes::RenderBody(atlas,"\xc0\xaf",{244,240,217,255},400,image));
    Check(!echoes::RenderBody(atlas,"\xe2\x98\x83",{244,240,217,255},400,image));
    Check(echoes::RenderBodyParagraph(atlas,"Native words wrap\non a second line",{244,240,217,255},400,154,image));
    Check(image.w==400 && image.h==154);
    Check(!echoes::RenderBodyParagraph(atlas,"Unfittable",{244,240,217,255},2,154,image) && image.Empty());
    Check(!echoes::RenderBodyParagraph(atlas,"One\nTwo\nThree\nFour\nFive",{244,240,217,255},400,154,image));
    Check(!echoes::RenderBodyParagraph(atlas,"Bad \xc0\xaf",{244,240,217,255},400,154,image));
    const auto &h = atlas.glyphs['H' - atlas.first_codepoint];
    Check(h.h >= 29 && h.h <= 30);
    for (const auto &g : atlas.glyphs)
      if (g.w && g.h)
        Check(g.y / 512 == (g.y + g.h - 1) / 512);
    auto ellipsis = atlas.glyphs[0x2026 - atlas.first_codepoint];
    Check(ellipsis.w > 0 && ellipsis.advance > 0);
    auto wrong = native;
    wrong.standard_font[0] ^= 1;
    Check(!echoes::BodyAtlas(wrong, cps, atlas) && atlas.image.Empty());
    wrong = native;
    wrong.standard_font_valid = false;
    Check(!echoes::BodyAtlas(wrong, cps, atlas));
    wrong = native;
    wrong.body_font_supported = false;
    Check(!echoes::BodyAtlas(wrong, cps, atlas));
    Check(!echoes::BodyAtlas(native, cps, atlas, 21));
    Check(!echoes::BodyAtlas(native, cps, atlas, 37));
    Check(!echoes::BodyAtlas(native, {}, atlas));
    for (uint32_t cp : {0x10ffffu, 0xd800u, 0x1f642u}) {
      std::array<uint32_t, 1> required{cp};
      Check(!echoes::BodyAtlas(native, required, atlas));
    }
    std::vector<uint32_t> huge(8193, 65);
    Check(!echoes::BodyAtlas(native, huge, atlas));
    std::array<uint32_t, 2> dense{32, 0x10000};
    Check(!echoes::BodyAtlas(native, dense, atlas));
    Check(echoes::BodyAtlas(native, cps, atlas, 22));
    auto broad = cps;
    for (uint32_t cp = 0xa0; cp <= 0x17f; ++cp)
      broad.push_back(cp);
    for (uint32_t cp = 0x410; cp <= 0x44f; ++cp)
      broad.push_back(cp);
    Check(echoes::BodyAtlas(native, broad, atlas, 36));
    Check(atlas.image.h >= 1024);
    for (const auto &g : atlas.glyphs)
      if (g.w && g.h)
        Check(g.y / 512 == (g.y + g.h - 1) / 512);
    std::cout << "Real native body atlas validated\n";
  } else
    std::cout << "SKIP real body font (ECHOES_FONT_TTF unset)\n";
  if (const char *path = std::getenv("ECHOES_HEADING")) {
    auto bytes = File(path);
    Check(echoes::ParseHeading(bytes, heading));
    Check(echoes::RenderHeading(heading, "Echoes", {49, 41, 31, 255}, 800,
                                image));
    size_t ink = 0;
    for (size_t p = 3; p < image.rgba.size(); p += 4)
      ink += image.rgba[p] != 0;
    Check(ink > 100);
    Check(!echoes::RenderHeading(heading, "Echoes", {255, 255, 255, 255}, 10,
                                 image));
    auto bad = bytes;
    bad.resize(40);
    Check(!echoes::ParseHeading(bad, heading));
    bad = bytes;
    bad[4] ^= 1;
    Check(!echoes::ParseHeading(bad, heading));
    bad = bytes;
    bad[0x3e] = 2;
    Check(!echoes::ParseHeading(bad, heading));
    bad = bytes;
    bad[0x82180 + 20] = 0x88;
    bad[0x82180 + 21] = 0x21;
    bad[0x82180 + 22] = 0x08;
    bad[0x82180 + 23] = 0;
    Check(!echoes::ParseHeading(bad, heading));
    std::cout << "Real original Heading decoded and composed\n";
  } else
    std::cout << "SKIP real Heading (ECHOES_HEADING unset)\n";
  std::cout << checks << " font rendering checks passed\n";
}
