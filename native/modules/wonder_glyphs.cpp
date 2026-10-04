// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// PictureFont.bffnt: FFNT header, FINF -> TGLP (cell grid + an embedded BNTX sheet at the
// TGLP's data offset), CWDH (per-glyph widths), CMAP (codepoint -> glyph index). Same layout as
// the runtime's ParseBffnt (mod_nx_assets.cpp); the sheet is stored bottom-up.

#include "wonder_glyphs.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace WonderAssets {
namespace {
using namespace dsmod_sdk::int_types;

constexpr std::string_view FontArchive = "/Font/Font.Nin_NX_NVN.bfarc.zs";
constexpr std::string_view FontMember = "font/PictureFont.bffnt";

// Named by rendering the v1.2.1 sheet (wonder-glyph-tool). U+E006/E008 are unmapped.
constexpr std::array<PictureGlyph, 25> Glyphs{{
    {"OneUp", U'\uE004'},         // green 1-Up Mushroom
    {"SeedBlue", U'\uE005'},      // Wonder Seed, player colours
    {"SeedOrange", U'\uE007'},
    {"SeedCyan", U'\uE009'},
    {"SeedPink", U'\uE00A'},
    {"SeedYellow", U'\uE00B'},
    {"SeedGreen", U'\uE00C'},
    {"SeedPurple", U'\uE00D'},
    {"SeedRed", U'\uE00E'},
    {"SeedWhite", U'\uE00F'},
    {"PurpleCoin", U'\uE010'},    // purple flower coin
    {"GoalFlag", U'\uE011'},
    {"Medal", U'\uE012'},
    {"SeedPotBlue", U'\uE013'},   // Wonder Seed in a pot, player colours
    {"SeedPotCyan", U'\uE014'},
    {"SeedPotPink", U'\uE015'},
    {"SeedPotYellow", U'\uE016'},
    {"SeedPotGreen", U'\uE017'},
    {"SeedPotPurple", U'\uE018'},
    {"Bowser", U'\uE019'},
    {"BowserJr", U'\uE01A'},
    {"Star", U'\uE01B'},
    {"FlowerCoin", U'\uE01C'},
    {"Crown", U'\uE01D'},
    {"BlueFlower", U'\uE01E'},
}};

u16 R16(std::span<const u8> d, std::size_t o) {
    return o + 2 <= d.size() ? dsmod_sdk::Le16(d.data() + o) : 0;
}
u32 R32(std::span<const u8> d, std::size_t o) {
    return o + 4 <= d.size() ? dsmod_sdk::Le32(d.data() + o) : 0;
}
bool Magic(std::span<const u8> d, std::size_t o, std::string_view m) {
    return o + m.size() <= d.size() && std::memcmp(d.data() + o, m.data(), m.size()) == 0;
}

struct Width {
    s8 left;
    u8 glyph;
    u8 advance;
};

struct Sheet {
    u32 cell_w{}, cell_h{}, cols{}, rows{};
    Width default_width{};
    std::map<u16, Width> widths;
    std::map<char32_t, u16> cmap;
    Image image; // upright
};

std::optional<Sheet> Parse(std::span<const u8> d, AstcDecoder astc) {
    if (!Magic(d, 0, "FFNT"))
        return std::nullopt;
    const std::size_t finf = R16(d, 6);
    if (!Magic(d, finf, "FINF") || finf + 0x20 > d.size())
        return std::nullopt;
    Sheet s;
    s.default_width = {static_cast<s8>(d[finf + 16]), d[finf + 17], d[finf + 18]};
    const u32 tglp = R32(d, finf + 20), cwdh = R32(d, finf + 24), cmap = R32(d, finf + 28);
    if (tglp < 8 || !Magic(d, tglp - 8, "TGLP") || tglp - 8 + 0x20 > d.size())
        return std::nullopt;
    const std::size_t t = tglp - 8;
    s.cell_w = d[t + 8];
    s.cell_h = d[t + 9];
    s.cols = R16(d, t + 20);
    s.rows = R16(d, t + 22);
    const u32 sheet_offset = R32(d, t + 28);
    if (!s.cell_w || !s.cell_h || !s.cols || !s.rows || sheet_offset >= d.size())
        return std::nullopt;

    for (u32 block = cwdh, guard = 0; block >= 8 && guard < 1024; ++guard) {
        const std::size_t at = block - 8;
        if (!Magic(d, at, "CWDH") || at + 16 > d.size())
            break;
        const u16 first = R16(d, at + 8), last = R16(d, at + 10);
        for (u32 g = first, q = 0; g <= last; ++g, ++q) {
            const std::size_t e = at + 16 + std::size_t{q} * 3;
            if (e + 3 > d.size())
                break;
            s.widths[static_cast<u16>(g)] = {static_cast<s8>(d[e]), d[e + 1], d[e + 2]};
        }
        const u32 next = R32(d, at + 12);
        block = next > block ? next : 0;
    }
    for (u32 block = cmap, guard = 0; block >= 8 && guard < 1024; ++guard) {
        const std::size_t at = block - 8;
        if (!Magic(d, at, "CMAP") || at + 24 > d.size())
            break;
        const u32 begin = R32(d, at + 8), end = R32(d, at + 12);
        const u16 method = R16(d, at + 16);
        std::size_t q = at + 24;
        if (end >= begin && end - begin < 0x110000) {
            if (method == 0) {
                const u16 base = R16(d, q);
                for (u64 c = begin; c <= end; ++c) // u64: end may be 0xFFFFFFFF
                    s.cmap[static_cast<u32>(c)] = static_cast<u16>(base + (c - begin));
            } else if (method == 1) {
                for (u64 c = begin; c <= end && q + 2 <= d.size(); ++c, q += 2)
                    if (const u16 i = R16(d, q); i != 0xFFFF)
                        s.cmap[static_cast<u32>(c)] = i;
            } else if (method == 2) {
                const u16 n = R16(d, q);
                q += 4;
                for (u32 k = 0; k < n && q + 8 <= d.size(); ++k, q += 8)
                    if (const u16 i = R16(d, q + 4); i != 0xFFFF)
                        s.cmap[R32(d, q)] = i;
            }
        }
        const u32 next = R32(d, at + 20);
        block = next > block ? next : 0;
    }
    if (s.cmap.empty())
        return std::nullopt;

    // The sheet is a BNTX (one texture; only array layer 0 is decoded -- the PictureFont has one
    // sheet). Stored bottom-up: flip to upright.
    auto image = DecodeBntx(d.subspan(sheet_offset), {}, astc);
    if (!image || image->rgba.empty())
        return std::nullopt;
    const std::size_t row = std::size_t{image->width} * 4;
    for (u32 y = 0; y < image->height / 2; ++y)
        std::swap_ranges(image->rgba.begin() + y * row, image->rgba.begin() + (y + 1) * row,
                         image->rgba.begin() + (image->height - 1 - y) * row);
    s.image = std::move(*image);
    return s;
}

std::mutex cache_mutex;
std::shared_ptr<const Sheet> cache;

std::shared_ptr<const Sheet> GetSheet(const RomfsReader& read, AstcDecoder astc) {
    {
        std::scoped_lock lock{cache_mutex};
        if (cache)
            return cache;
    }
    const auto raw = read(std::string{FontArchive});
    if (!raw)
        return nullptr;
    const auto sarc = Zstd(*raw);
    if (!sarc)
        return nullptr;
    const auto font = SarcMember(*sarc, FontMember);
    if (!font)
        return nullptr;
    auto sheet = Parse(*font, astc);
    if (!sheet)
        return nullptr; // not cached: a later call (e.g. with an ASTC decoder) may succeed
    std::scoped_lock lock{cache_mutex};
    if (!cache)
        cache = std::make_shared<const Sheet>(std::move(*sheet));
    return cache;
}

std::optional<Image> Crop(const Sheet& s, u16 index) {
    const u32 per_sheet = s.cols * s.rows;
    if (index >= per_sheet)
        return std::nullopt; // later sheets (array layers) are not decoded
    const auto wi = s.widths.find(index);
    const Width w = wi == s.widths.end() ? s.default_width : wi->second;
    const u32 col = index % s.cols, row = index / s.cols;
    const u32 x0 = col * (s.cell_w + 1) + 1, y0 = row * (s.cell_h + 1) + 1;
    const u32 cw = std::min<u32>(w.glyph ? w.glyph : s.cell_w, s.cell_w);
    if (x0 + cw > s.image.width || y0 + s.cell_h > s.image.height)
        return std::nullopt;
    Image out{cw, s.cell_h, std::vector<u8>(std::size_t{cw} * s.cell_h * 4)};
    for (u32 y = 0; y < s.cell_h; ++y)
        std::memcpy(out.rgba.data() + std::size_t{y} * cw * 4,
                    s.image.rgba.data() + (std::size_t{y0 + y} * s.image.width + x0) * 4,
                    std::size_t{cw} * 4);
    return out;
}
} // namespace

std::span<const PictureGlyph> PictureGlyphs() {
    return Glyphs;
}

std::optional<Image> LoadPictureGlyph(const RomfsReader& read, char32_t codepoint,
                                      AstcDecoder astc) {
    const auto sheet = GetSheet(read, astc);
    if (!sheet)
        return std::nullopt;
    const auto it = sheet->cmap.find(codepoint);
    if (it == sheet->cmap.end())
        return std::nullopt;
    return Crop(*sheet, it->second);
}

std::optional<Image> LoadPictureGlyph(const RomfsReader& read, std::string_view name,
                                      AstcDecoder astc) {
    for (const auto& g : Glyphs)
        if (g.name == name)
            return LoadPictureGlyph(read, g.codepoint, astc);
    return std::nullopt;
}

std::vector<char32_t> PictureFontCodepoints(const RomfsReader& read, AstcDecoder astc) {
    std::vector<char32_t> out;
    if (const auto sheet = GetSheet(read, astc))
        for (const auto& [c, i] : sheet->cmap)
            out.push_back(c);
    return out;
}

} // namespace WonderAssets
