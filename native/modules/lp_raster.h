// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bounds helpers for the pictures composed from the player's romfs (lp_assets, lp_poketch,
// lp_unity). Decoded art can be empty (a 0x0 sprite) or carry damaged float geometry; every
// pixel read and float -> int conversion on that data goes through these checks.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lp_raster {

/// Coordinates beyond this many px are never real art; casts saturate (or refuse) here.
inline constexpr double CoordLimit = 1 << 24;

/// An RGBA8 picture of w x h holds at least w * h * 4 bytes (and is not empty).
inline bool Fits(std::uint32_t w, std::uint32_t h, std::size_t bytes) {
    return w != 0 && h != 0 && bytes / 4 / w >= h; // no overflow for any w, h
}

/// C's (int) cast of a float coordinate, with NaN as 0 and infinities / huge values saturated,
/// so damaged mesh data cannot reach an undefined conversion. Real coordinates are unchanged.
inline int TruncCoord(double v) {
    if (std::isnan(v))
        return 0;
    return static_cast<int>(std::clamp(v, -CoordLimit, CoordLimit));
}

/// Python's round() of a float coordinate; nullopt for NaN, infinities and huge values.
inline std::optional<std::int64_t> RoundCoord(double v) {
    if (!std::isfinite(v) || std::abs(v) > CoordLimit)
        return std::nullopt;
    return static_cast<std::int64_t>(std::nearbyint(v)); // default rounding: half to even
}

/// lround of a sprite rect / offset value, nullopt when it is not a finite in-range number.
inline std::optional<int> RoundPx(double v) {
    if (!std::isfinite(v) || std::abs(v) > CoordLimit)
        return std::nullopt;
    return static_cast<int>(std::lround(v));
}

/// A nine-slice with border `b` can be cut from a w x h source.
inline bool NineFits(int w, int h, int b) {
    return b >= 0 && w > 0 && h > 0 && w >= 2 * b && h >= 2 * b;
}

/// The shiny party icon's mark: the native sparkle's alpha (sw x sh, `src`) scaled into the top-left
/// quarter (at least 8 px) of the square icon `dst` (dw x dh), tinted red. False (and `dst`
/// untouched) when either picture is too small or inconsistent.
inline bool StampSparkle(std::uint32_t dw, std::uint32_t dh, std::vector<std::uint8_t>& dst, std::uint32_t sw,
                         std::uint32_t sh, const std::vector<std::uint8_t>& src) {
    if (!Fits(dw, dh, dst.size()) || !Fits(sw, sh, src.size()) || dw < 8 || dh < 8)
        return false;
    const int side = std::min({std::max(8, static_cast<int>(dw) / 4), static_cast<int>(dw), static_cast<int>(dh)});
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            const auto a = src[(static_cast<std::size_t>(y) * sh / side * sw + static_cast<std::size_t>(x) * sw / side) * 4 + 3];
            if (!a)
                continue;
            auto* d = dst.data() + (static_cast<std::size_t>(y) * dw + x) * 4;
            const int rgb[3]{220, 40, 52};
            const int alpha = a + d[3] * (255 - a) / 255;
            for (int c = 0; c < 3; ++c)
                d[c] = static_cast<std::uint8_t>((rgb[c] * a + d[c] * d[3] * (255 - a) / 255) / alpha);
            d[3] = static_cast<std::uint8_t>(alpha);
        }
    return true;
}

} // namespace lp_raster
