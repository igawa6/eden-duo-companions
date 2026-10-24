// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_assets.h"
#include "echoes_fonts.h"
namespace echoes {
// Display-only aliases U+2400..2403 for original Pictogram E0E0/E0E2/E0E3/E0E6.
// Raw message metadata must remain untouched. Strict UTF-8; reserved aliases in
// input and unsupported private-use glyphs fail closed. Output clears on failure.
bool PrologueDisplayText(std::string_view raw, std::string& display);
// Separate optional atlas: original native Standard body plus exact CMAP-selected
// Pictogram glyphs. The caller supplies codepoints from display-normalized text.
// No effect on the core body/heading fonts. No guest calls or bundled art.
bool PrologueBodyAtlas(const NativeFonts& native, const ReadAssetFn& read,
                       std::span<const uint32_t> required_display_codepoints,
                       FontAtlas& out, uint32_t cap_height=29);
} // namespace echoes
