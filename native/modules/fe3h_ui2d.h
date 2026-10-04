// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem: Three Houses (1.2.0): the Nintendo LayoutKit archives the game ships for its
// effect / forecast layouts (LINKDATA 30776..30827 and 31527: nx/ui/layout/*.arc) -- a SARC of
// blyt/*.bflyt, anim/*.bflan and one timg/__Combined.bntx. Only the textures are decoded here;
// the pane geometry the module needs (forecast name plates, portrait diamonds) is quoted from the
// layouts in fe3h_assets.h. Python reference: research/fe3h/catalog/tools/uilib.py.
//
//   SARC   'SARC' | u16 header 0x14 | u16 BOM | u32 size | u32 data offset | ...; 'SFAT' at 0x14:
//          u16 header, u16 count, u32 hash key; count x {u32 hash, u32 attr (bit 24: name at
//          (attr & 0xFFFF) * 4 in SFNT), u32 begin, u32 end} (data-relative); 'SFNT' + 8 = names.
//   BNTX   'BNTX' ...; +0x24 u32 texture count, +0x28 u64 offset of the BRTI pointer array.
//   BRTI   +0x12 u16 tile mode (0 = Tegra block-linear), +0x1C u32 format (type << 8 | variant),
//          +0x24 u32 width, height, depth, array count, layout (block height = 1 << (layout & 7)),
//          +0x50 u32 image size, +0x60 u64 name (u16 length + chars), +0x70 u64 mip pointer
//          array (first = mip 0).
//   formats 0x1A BC1, 0x1C BC3, 0x1D BC4 (-> L,L,L,255), 0x1E BC5 (LayoutKit '^t' textures:
//          luminance + alpha -> R,R,R,G).
//   block-linear: GOBs of 64 bytes x 8 rows, `block height` GOBs per block (the block height
//          halves while bh * 8 >= 2 * block rows -- the rule the catalog decode uses, checked
//          bit-exact on every texture the module serves).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fe3h_ui2d {

inline constexpr uint32_t FirstArc = 30776, LastArc = 30827, ExtraArc = 31527;
inline constexpr uint32_t MaxDim = 2048;

struct SarcFile {
    std::string name;
    size_t offset{}, size{};
};
// Files of a SARC (bounds-checked); empty on a malformed archive.
std::vector<SarcFile> Sarc(std::span<const uint8_t> d);

struct Texture {
    std::string name; // as stored, e.g. "btl_win_name_blue_l^q"
    uint32_t width{}, height{};
    std::vector<uint8_t> rgba;
};
// Decodes the texture called `name` (exact, or without its "^x" suffix).
std::optional<Texture> BntxDecode(std::span<const uint8_t> d, std::string_view name);

// Tegra block-linear -> linear (exposed for tests). `bpe` bytes per element.
std::vector<uint8_t> Deswizzle(std::span<const uint8_t> src, uint32_t w_el, uint32_t h_el,
                               uint32_t bpe, uint32_t block_height);

} // namespace fe3h_ui2d
