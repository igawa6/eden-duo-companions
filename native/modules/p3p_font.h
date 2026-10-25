// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The game's own lettering for the P3P companion, read from the user's romfs (asset-free package).
//
// data_EN/umd0.cpk -> init.bin (Atlus PAK v1) -> font/font0.fnt: Atlus FNT, 3650 64x64 4bpp
// glyphs, decoded by p5r_font.h. Glyph i is code point 0x20 + i for printable ASCII; past '~' the
// font continues with kana, so only ASCII is mapped (the host's fallback handles the rest).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/mods/modules/dsmod_module_sdk.h"
#include "p5r_font.h"
#include "p5r_romfs_assets.h"

namespace p3p_font {

inline int GlyphIndex(uint32_t cp) {
    return cp >= 0x20 && cp <= 0x7E ? static_cast<int>(cp - 0x20) : -1;
}

/// font/font0.fnt out of the English startup archive; false if any layer is missing or malformed.
inline bool ReadFont(const EdenDsmodHostApi& host, std::vector<uint8_t>& out) {
    out.clear();
    p5r_assets::Cpk cpk;
    p5r_assets::Cpk::Entry entry;
    std::vector<uint8_t> init;
    std::vector<p5r_assets::PakEntry> members;
    if (!cpk.Open(p5r_assets::HostReader(host), "data_EN/umd0.cpk") || !cpk.Find("init.bin", entry) ||
        !cpk.ReadEntry(entry, init) || !p5r_assets::ParsePak(init, members))
        return false;
    const auto font = p5r_assets::PakFind(members, "font/font0.fnt");
    out.assign(font.begin(), font.end());
    return !out.empty();
}

/// The atlas and metrics the host draws text with (both pure functions of the font file).
inline bool BuildAtlas(const EdenDsmodHostApi& host, p5r_font::Atlas& out) {
    std::vector<uint8_t> bytes;
    return ReadFont(host, bytes) && p5r_font::DecodeAtlas(bytes.data(), bytes.size(), out, GlyphIndex);
}

} // namespace p3p_font
