// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Binding of Isaac (Switch): the game's PCX sheets and a small straight-RGBA image type.
// PCX = standard ZSoft v5, RLE (encoding 1), 8 bits/plane; the game's files are 4 planes stored
// R,G,B,A planar per scanline (research/isaac/data/REPORT.md §2.1). 1 plane + 256-colour palette
// and 3 planes (RGB) are decoded too. Fails closed. Owner: ASSETS lane.
// The compositing helpers reproduce Pillow's integer maths (Image.alpha_composite), so the
// module's output matches the offline Python prototypes (research/isaac/mock) pixel for pixel.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace isaac_pcx {

struct Image {
    std::uint32_t w{}, h{};
    std::vector<std::uint8_t> px; // straight RGBA8, w*h*4

    bool Valid() const {
        return w && h && px.size() == std::size_t(w) * h * 4;
    }
    std::uint8_t* At(std::uint32_t x, std::uint32_t y) {
        return px.data() + (std::size_t(y) * w + x) * 4;
    }
    const std::uint8_t* At(std::uint32_t x, std::uint32_t y) const {
        return px.data() + (std::size_t(y) * w + x) * 4;
    }
};

inline constexpr std::uint32_t MaxSide = 4096;

/// Decodes a PCX file. False for anything that is not a well-formed 8-bit 1/3/4-plane PCX.
bool Decode(const std::uint8_t* data, std::size_t size, Image& out);

/// Transparent w x h image.
Image Blank(std::uint32_t w, std::uint32_t h);
/// Crop (x, y, w, h); parts outside the source are transparent (Pillow's crop semantics).
Image Crop(const Image& src, int x, int y, std::uint32_t w, std::uint32_t h);
/// Pillow ImagingAlphaComposite of src over dst at (x, y) (clipped).
void Over(Image& dst, const Image& src, int x, int y);
/// Nearest-neighbour integer upscale.
Image Upscale(const Image& src, std::uint32_t k);
/// Bounding box of pixels with alpha != 0; false when fully transparent.
bool AlphaBox(const Image& img, std::uint32_t& x0, std::uint32_t& y0, std::uint32_t& x1,
              std::uint32_t& y1);
/// Crop to AlphaBox (unchanged when empty).
Image Trim(const Image& img);
/// Horizontal / vertical mirror.
Image FlipX(const Image& img);
Image FlipY(const Image& img);

} // namespace isaac_pcx
