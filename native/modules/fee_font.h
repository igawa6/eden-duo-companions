// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem Engage 2.0.0: the companion's text is drawn in the game's own menu face,
// FOT-Humming Std, decoded at runtime from the player's own romfs. Nothing is shipped: the
// package carries only a tiny request file (the package's font_metrics_src) naming which weight
// to decode, and the host asks for it again through the normal image callback for the atlas.
//
//   bundle  Data/StreamingAssets/aa/Switch/fe_assets_fonts/systemjpsdf.bundle /
//           systemboldjpsdf.bundle (relative to fee::FeeAssetRoot)
//   asset   one TMP_FontAsset (MonoBehaviour, class 114) per bundle, named "SystemJP SDF" /
//           "SystemBoldJP SDF" -- FOT-Humming Std style M (Regular) / B (Bold), baked static SDF
//           atlases (4096x4096, padding 5, point size 40/39) covering ASCII + accented Latin +
//           arrows, missing only U+2014 (em dash) and U+2019 (right single quote), which this
//           module substitutes with '-' and '\'' so every page's text renders without a gap.
//           (The English-locale "systemsdf.bundle"/"systemboldsdf.bundle" pair is a *different*,
//           empty dynamic placeholder -- not used here.)
//
// Host mapping: EdenDsmodFontGlyph.{x,y,w,h} index the atlas image this module serves back at
// "fee:font/<regular|bold>" (module:fee:font/<regular|bold>); bearing/advance come straight from
// the TMP glyph metrics (point-size units == atlas px here, m_Scale 1.0). line_height = the face's
// cap line (wonder_font.h's convention: line_height = cap height), so a host text size of N scales
// uniformly from a cap height of N. The atlas alpha is the baked SDF channel, re-centred around its
// 0.5 isoline with the same linear ramp the design-preview renderer uses (fe_lib.py SDFFont.mask):
// crisp coverage, no raw SDF grey fringe, no AA bleed into neighbouring glyphs.
//
// decode_font is called once (EdenDsmodFontSink has no pixel payload); the atlas pixels are asked
// for separately, through the ordinary load_image path, once the host knows the glyph rects.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

#include "lp_unity.h"

namespace fee_font {

enum class Face { Regular, Bold };

/// Bundle path (relative to fee::FeeAssetRoot) and TMP_FontAsset name for a face.
std::string_view Bundle(Face face);
std::string_view AssetName(Face face);

struct TmpGlyph {
    std::uint32_t index{};
    float width{}, height{}, bearing_x{}, bearing_y{}, advance{};
    std::int32_t rect_x{}, rect_y{}, rect_w{}, rect_h{};
};

struct TmpFont {
    float point_size{}, cap_line{}, ascent_line{}, descent_line{};
    std::int32_t atlas_w{}, atlas_h{}, padding{};
    std::vector<TmpGlyph> glyphs;                 // m_GlyphTable order
    std::unordered_map<std::uint32_t, std::uint32_t> chars; // unicode -> index into glyphs
    lp_unity::Image atlas_image;                  // decoded Texture2D, top-left origin

    const TmpGlyph* Find(std::uint32_t codepoint) const;
};

/// Reads the TMP_FontAsset MonoBehaviour by name plus its atlas Texture2D. nullopt on anything
/// that does not look like this game's SDF font asset (missing tables, no atlas texture).
std::optional<TmpFont> LoadTmpFont(lp_unity::Reader& reader, std::string_view bundle,
                                   std::string_view name);

/// Codepoints this module substitutes when the baked atlas lacks a glyph (em dash -> '-',
/// right single quote -> apostrophe): requested -> substitute.
std::uint32_t SubstituteFor(std::uint32_t codepoint);

/// Host atlas image: RGB 0xFF, A = coverage reconstructed from the SDF around its 0.5 isoline
/// (fe_lib.py's ramp, k = 2 * m_AtlasPadding) -- crisp glyph edges, no grey SDF fringe.
lp_unity::Image HostFontAtlas(const TmpFont& font);

/// Repacks the glyphs the host uses (codepoints <= max_codepoint plus substitutes) from the
/// 4096x4096 baked atlas into a small shelf-packed atlas (1024 wide), rewriting the glyph rects
/// and atlas size to match; the full JP atlas would be a 64 MiB image per face.
void PackAtlas(TmpFont& font, std::uint32_t max_codepoint = 0x2700);

/// The host glyph run, from the lowest mapped codepoint to `max_codepoint` (default stops before
/// the CJK blocks this JP-locale atlas also carries -- the companion's Latin/UI text only).
/// Missing-but-substituted codepoints (SubstituteFor) are filled in from their substitute glyph.
bool BuildHostGlyphs(const TmpFont& font, std::uint32_t& first_codepoint,
                     std::vector<EdenDsmodFontGlyph>& glyphs, std::uint32_t& line_height,
                     std::uint32_t max_codepoint = 0x2700);

/// The manifest's font_metrics_src asset: a package text file whose first line is
/// "FEEFONT regular" or "FEEFONT bold" (any trailing text ignored). False otherwise.
bool ParseRequest(std::span<const std::uint8_t> bytes, Face& face);

/// Owns the font-only lp_unity reader (separate from fee::Assets' -- this module's romfs access
/// is read-only and tiny: one bundle, decoded once per face, kept for the LoadImage round trip).
/// Thread-safe: decode_font and the image callback both land on the host's own threads.
class FontCache {
public:
    explicit FontCache(lp_unity::RangeReader read, std::string asset_root);

    void SetAstcDecoder(lp_unity::AstcDecoder astc);

    /// decode_font: parses `bytes` as a font request, decodes that face (cached after the first
    /// call) and reports its glyph run through `sink`. False on a bad request or decode failure.
    bool DecodeFont(const std::uint8_t* bytes, std::size_t size, void* receiver,
                    EdenDsmodFontSink sink);

    /// The atlas for "fee:font/regular" / "fee:font/bold" (image callback); nullptr if that face
    /// has not been decoded yet (decode_font always runs first) or decoding failed.
    std::shared_ptr<const lp_unity::Image> Atlas(Face face);

private:
    struct Entry {
        std::shared_ptr<const lp_unity::Image> atlas; // packed host atlas
        std::shared_ptr<const TmpFont> font;          // glyph rects point into `atlas`
    };

    Entry& Decode(Face face); // builds+caches; Entry.atlas stays null on failure

    lp_unity::Reader reader_;
    std::mutex mutex_;
    std::unordered_map<int, Entry> cache_; // key = static_cast<int>(Face)
};

} // namespace fee_font
