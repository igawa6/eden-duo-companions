// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Game font for the companion: TextMesh Pro font "LaptureRegular" from the player's game files
// (nothing is shipped). The host's built-in font only has A-Z without umlauts
// and no apostrophe; see tools/cotl/README.md (game font).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "chained_echoes_unity.h"
#include "core/mods/dsmod_module_extensions.h"

namespace cotl {

// Bundle and name of the font (Cult of the Lamb 1.5.27)
inline constexpr const char* kFontBundle = "6685d1be5dc371c74341da1c3dc48c86.bundle";
inline constexpr const char* kFontName = "LaptureRegular";
// Cap height of the atlas in px and the cap height reported to the host. The host scales glyphs
// nearest-neighbour to text_scale x 5 px x (atlas / reported): 30 / 25 draws the game font
// 20 % larger than the built-in block font (lowercase looks smaller otherwise); at
// text_scale 5 that is exactly 1:1, scales 3-6 stay close to it.
inline constexpr int kFontCapPx = 30;
inline constexpr int kFontLineHeight = 25;

struct HostFont {
    uint32_t line_height = 0; // host convention: cap height
    uint32_t first_codepoint = 0;
    std::vector<EdenDsmodFontGlyph> glyphs; // dense from first_codepoint, missing characters empty
    ce_unity::Image atlas;                  // RGB white, alpha = coverage
};

// Converts the TMP font's SDF atlas into a coverage atlas with cap height cap_px
// (sampled bilinearly, edge at d = 0.5, 1 px margin per glyph). Characters up to U+2026.
// line_height is reported to the host as the cap height.
std::optional<HostFont> BuildHostFont(const ce_unity::TmpFont& font, int cap_px, int line_height);

struct IconGlyph {
    uint32_t codepoint = 0;
    ce_unity::Image image; // only alpha counts, the host tints glyphs
};

// Appends icons as glyphs below the atlas: transparent margin trimmed, scaled to height_px,
// standing on the baseline. Empty images are skipped.
void AddIconGlyphs(HostFont& font, const std::vector<IconGlyph>& icons, int height_px);

} // namespace cotl
