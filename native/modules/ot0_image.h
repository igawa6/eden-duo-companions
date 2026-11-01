// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include "ot0_pak.h"

namespace ot0 {
// Straight RGBA8. Invalid dimensions/crops return an empty image; operations clip to dst.
Image Resize(const Image& src, u32 x, u32 y, u32 w, u32 h, u32 out_w, u32 out_h);
void Over(Image& dst, const Image& src, int x, int y);
// Native Box margins: 0.3 of each source dimension (48 px for the 160 px InBox).
Image Nine(const Image& src, u32 out_w, u32 out_h, float margin = .3f);
Image DarkenBlur(const Image& src, double brightness = .10, u32 radius = 8);
Image RedKey(const Image& src); // Only exact opaque/masked RGB #FF0000 loses alpha.

enum class FontFace { Franco, FrancoBold, Rocio, ClearTone };
// Strict UTF-8: malformed, overlong, surrogate and out-of-range scalars are rejected.
std::optional<std::u32string> DecodeUtf8(std::string_view text);

// Instance-owned original fonts. Load and rasterisation belong on the asset worker, never
// the sampling thread. All four pinned font files are loaded once; no system font fallback.
class FontLibrary {
public:
    FontLibrary();
    ~FontLibrary();
    FontLibrary(FontLibrary&&) noexcept;
    FontLibrary& operator=(FontLibrary&&) noexcept;
    FontLibrary(const FontLibrary&) = delete;
    FontLibrary& operator=(const FontLibrary&) = delete;
    bool Load(const RangeReader& read, std::string* why = nullptr);
    bool Ready() const;
    std::optional<int> Measure(std::u32string_view text, FontFace face, float em_px) const;
    std::optional<int> Measure(std::string_view utf8, FontFace face, float em_px) const;
    // Anchors: horizontal l/m/r; vertical a (ascender), m (middle), s (baseline),
    // b (descender), t (glyph top). Missing glyphs fail atomically without drawing.
    bool DrawGameText(Image& dst, std::u32string_view text, FontFace face, float em_px,
                      int x, int y, std::array<u8, 4> color = {255,255,255,255},
                      char anchor_h = 'l', char anchor_v = 'a') const;
    bool DrawGameText(Image& dst, std::string_view utf8, FontFace face, float em_px,
                      int x, int y, std::array<u8, 4> color = {255,255,255,255},
                      char anchor_h = 'l', char anchor_v = 'a') const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ot0
