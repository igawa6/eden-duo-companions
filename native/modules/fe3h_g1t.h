// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Koei Tecmo G1T texture containers as FE3H 1.2.0 stores them (Switch, platform 0x0A), decoded
// to straight RGBA8 (mip 0, first layer). Layout (research/fe3h/data/REPORT.md §4, the Python
// reference is research/fe3h/data/tools/fe3h_g1t.py):
//
//   0x00 'GT1G', 0x04 version (ascii, '0600'), 0x08 u32 file size, 0x0C u32 table offset,
//   0x10 u32 texture count N, 0x14 u32 platform, 0x18 u32 extra-header size, 0x1C u32[N] flags;
//   table: u32[N] offsets relative to the table offset -> texture headers.
//   texture header: b0 = mip count << 4 (low nibble 0 for 2D), b1 = format,
//     b2 = log2 h << 4 | log2 w, b3, b4..b7 flags; b7 & 0x10 -> extension {u32 size (incl.
//     itself), u32 depth, u32 ?, u32 width, u32 height when size >= 0x14} (NPOT sizes);
//     pixels follow, LINEAR (not Tegra block-linear), mip 0 first.
//   formats: 0x59 BC1, 0x5A BC2, 0x5B BC3, 0x5F BC7, 0x00 RGBA8, 0x01 BGRA8 (the Python
//     reference decodes 0x01 as BGRA; no 0x01 texture is behind a module key yet).
//
// Pure functions over a Source (ranged), any thread.

#include <cstdint>
#include <optional>
#include <vector>

#include "fe3h_romfs.h"

namespace fe3h_g1t {

inline constexpr uint32_t MaxDim = 4096;

struct TextureInfo {
    uint32_t index{};
    uint8_t format{};
    uint32_t width{}, height{};
    uint32_t mips{};
    uint64_t data_offset{}; // mip 0, from the start of the G1T
    uint64_t data_size{};   // bytes of mip 0
};

struct Image {
    uint32_t width{}, height{};
    std::vector<uint8_t> rgba; // straight RGBA8, row-major, top-left origin
};

// Texture count of a G1T (validated header). False on a bad header.
bool Count(fe3h_romfs::Source& g1t, uint32_t& count);
// Header of texture `index`, bounds-checked against the container and its neighbour.
std::optional<TextureInfo> Texture(fe3h_romfs::Source& g1t, uint32_t index);
// Bytes of mip 0 for a supported format; 0 = unsupported.
uint64_t Mip0Size(uint8_t format, uint32_t w, uint32_t h);
// Decodes mip 0 of texture `index`.
std::optional<Image> Decode(fe3h_romfs::Source& g1t, uint32_t index);
// Decodes raw mip-0 bytes of a format (exposed for tests).
std::optional<Image> DecodePixels(uint8_t format, uint32_t w, uint32_t h,
                                  const std::vector<uint8_t>& data);

} // namespace fe3h_g1t
