// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH's own fonts from the player's romfs (research/acnh/catalog/UI_FONTS.md §1):
//
// Scalable fonts: Font/ScalableFont.sarc.zs (zstd SARC, member names without folder):
//   nintendoP_Seurat-B.bfttf             main UI face (SystemB_00 Latin face, TrueType)
//   nintendoP_RodinNTLG-EB_003_Park.bfotf dialogue face (Normal_00, CFF)
//   ParkExt.bfttf                        picture glyphs U+E0A0.. (buttons, UI symbols, game icons),
//                                        the first face of every font pack (fcpx)
// Container (all u32 big endian, whole file XOR'd per word with one key):
//   +0 magic ^ key  (key = be32(file[0]) ^ 0x7F9A0218; 3.0.3: 0x49621806 for all 12 files)
//   +4 size ^ key   (plain length)          +8 font words ^ key (plain = first `size` bytes)
// The key is derived from each file's header, never hard-coded.
//
// Bitmap digit fonts (BFFNT v4.1, Font/BmpFont_US.sarc.zs): SystemBOutline_00 (pocket stack
// counts: fill + outline levels in one channel) and BookNumber_00 (Critterpedia numbers). TGLP
// gives the cell size; each sheet is a whole embedded BNTX stored bottom-up (flipped here).
//
// Atlas spec (the package's font metrics and atlas): "<face>[:<cap px>[:<set>]]"
//   face  ui (Seurat-B + ParkExt)  talk (RodinNTLG-EB + ParkExt)
//   cap   cap height in atlas px, 8..96 (default 30)
//   set   latin (default: Latin, Latin Ext, Greek, Cyrillic, punctuation, symbols + ParkExt
//         U+E000..E2FF)  or  all (every code point of the face: kana and CJK too; large)  or a
//         game language folder ("JPja", "KRko", "CNzh", "TWzh", ...): latin + every code point
//         of that language's String and LayoutMsg texts (SetLanguageCodepoints). In CNzh / TWzh /
//         KRko the code points from U+2E80 up come from that language's SystemB face first
//         (DFP_GB_Y9_0 / DFPT_Y8 / AsiaKSDNR-B, the faces after Seurat-B in
//         SystemB_00_<lang>.bfcpx; the fcpx per-face ranges are not decoded: LEAD), else Seurat-B
//         (it has kana + 6722 kanji)
// The ABI wants one dense run, so glyphs are laid out from U+0020 to the last rendered code point;
// unrendered slots in between are empty glyphs advancing like a space (except the same
// substitutions as the host's fallback, e.g. e-acute -> e, when the face lacks one).
// Metrics follow Core::Mods::FontMetrics like wonder_font.h: line_height = cap height,
// bearing_y = box top above the baseline, advance rounded; RGB 0xFF, A = coverage.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"
#include "acnh_types.h"

namespace acnh {

class Romfs;

/// .bfttf / .bfotf -> plain TTF/OTF bytes (empty on a malformed header). `key` gets the XOR key.
std::vector<uint8_t> DecodeBfttf(std::span<const uint8_t> file, uint32_t* key = nullptr);

/// Rows per atlas page (runtime 17 paged font: the package's font_page_h, module:acnh/font/<spec>
/// ?p=<n> = rows [n * AtlasPageRows, (n + 1) * AtlasPageRows) of the atlas). The shelf packer
/// never lets a glyph cross a page boundary, so a glyph is always inside one page. Pages keep the
/// font out of the host's 64 MiB module image cache (FontPages has its own budget, r9 pop).
inline constexpr uint32_t AtlasPageRows = 512;

struct FontAtlas {
    Image atlas;
    uint32_t line_height = 0;
    uint32_t first_codepoint = 0x20;
    std::vector<EdenDsmodFontGlyph> glyphs;
    std::string spec; ///< canonical spec
    size_t rendered = 0;
};

/// A BFFNT bitmap font (digits).
struct BmpFont {
    int cell_w = 0, cell_h = 0, cells_per_row = 0;
    Image sheet; ///< sheet 0, flipped upright
    std::map<uint32_t, int> cmap;
    /// The glyph's coverage (alpha of the decoded sheet), cell-sized.
    bool Glyph(uint32_t cp, std::vector<uint8_t>& alpha) const;
};

class Fonts {
public:
    explicit Fonts(Romfs& romfs);
    ~Fonts();

    /// Canonical spec or nullopt when malformed.
    static std::optional<std::string> CanonicalSpec(std::string_view spec);
    /// Cached atlas (null when the romfs is not readable yet or the spec is bad).
    std::shared_ptr<const FontAtlas> Atlas(std::string_view spec);
    /// Provider of a language set's extra code points (the module: its catalog's texts).
    void SetLanguageCodepoints(
        std::function<std::vector<uint32_t>(std::string_view lang)> provider);
    /// A string rendered in one face at `em_px` pixels per em, tight to its ink (alpha > 2).
    /// face: "ui", "talk", "parkext" or a CJK face ("ui_cn" / "ui_tw" / "ui_kr": what it lacks,
    /// e.g. Latin, comes from "ui"). UTF-8 input; code points the face lacks are skipped.
    bool Text(std::string_view face, std::string_view utf8, double em_px, Rgba colour, Image& out);
    /// Advance width in pixels of `utf8` at em_px (layout helper).
    double TextWidth(std::string_view face, std::string_view utf8, double em_px);
    /// Advance of `cp` in a "ui:<cap>" atlas, in atlas px, by the atlas rule (ParkExt from U+2000
    /// when it has the glyph, else Seurat-B; lround(advance * scale)); a code point from U+2E80
    /// that neither face has advances one em (a CJK face's glyph: layout helper); -1 = no glyph.
    /// nullopt = Seurat-B is not readable. TextWidth (acnh_publish.h) measures with this.
    std::optional<int> UiAdvance(uint32_t cap, uint32_t cp);
    /// BFFNT from Font/BmpFont_US.sarc.zs ("SystemBOutline_00", "BookNumber_00").
    std::shared_ptr<const BmpFont> Bitmap(std::string_view name);
    /// acnh_mock.BmpFont.render: digits from a BFFNT, ink-cropped glyphs overlapping 2 px, height
    /// h. With an outline colour, SystemBOutline's levels map outline (grey) -> fill (bright)
    /// (that level mapping is UNVERIFIED: the game's FontInfo border handling is not RE'd).
    bool Number(std::string_view font, std::string_view text, double h, Rgba fill,
                std::optional<Rgba> outline, Image& out);

private:
    struct Face;
    std::shared_ptr<const Face> LoadFace(std::string_view face);

    Romfs& romfs;
    std::function<std::vector<uint32_t>(std::string_view lang)> lang_cps;
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const Face>, std::less<>> faces;
    std::map<std::string, std::shared_ptr<const FontAtlas>, std::less<>> atlases;
    std::map<std::string, std::vector<uint32_t>, std::less<>> lang_sets; ///< lang_cps results
    std::map<std::string, std::shared_ptr<const BmpFont>, std::less<>> bitmaps;
};

} // namespace acnh
