// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "acnh_types.h"
#include "core/mods/dsmod_module_extensions.h"
#include <span>
#include <string_view>
#include <vector>
namespace pla {
struct Font {
    std::vector<uint8_t> rgba;
    std::vector<EdenDsmodFontGlyph> glyphs;
};
bool BuildFont(std::span<const uint8_t> file, Font &out, unsigned cap = 23);
acnh::Image TextImage(const Font &font, std::string_view text, unsigned cap, int w, int h,
                      bool left = false);
} // namespace pla
