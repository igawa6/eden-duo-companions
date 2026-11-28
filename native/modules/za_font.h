// SPDX-FileCopyrightText: Copyright 2026 Eden Duo Companions contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"
#include "za_assets.h"

namespace za {

struct PagedFont {
    static constexpr std::uint32_t kPageWidth = 1024u;
    static constexpr std::uint32_t kPageHeight = 1024u;

    int language = 2;
    std::uint32_t line_height = 50u;
    std::uint32_t first_codepoint = 0x20u;
    std::uint32_t page_count = 1u;
    std::vector<EdenDsmodFontGlyph> glyphs;

    struct PlacedGlyph {
        std::uint32_t cp = 0;
        int glyph_index = 0;
        std::uint16_t ax = 0;
        std::uint16_t ay_in_page = 0;
        std::uint16_t w = 0;
        std::uint16_t h = 0;
        float scale = 0.0f;
    };
    std::vector<std::uint8_t> font_bytes;
    std::vector<std::vector<PlacedGlyph>> pages;
};

class FontCache {
public:
    std::shared_ptr<const PagedFont> Ensure(Assets& assets, int language);
    bool RenderPage(Assets& assets, int language, std::uint32_t page, Picture& out);

private:
    std::mutex mutex_;
    std::shared_ptr<const PagedFont> cached_;
};

} // namespace za
