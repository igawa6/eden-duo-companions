// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// BNTX textures -> RGBA8 for the ACNH companion: Tegra block-linear deswizzle (mip 0, first array
// layer), BC1-5/BC7 (externals/bc_decoder), ASTC through the host's decode_astc, R8 / R8G8 /
// R5G6B5 / RGBA8 / BGRA8, then the BRTI channel selectors (+0x58: 0 zero, 1 one, 2..5 = R,G,B,A).
// The selectors are what turn ACNH's single-channel masks into "white + alpha" (^s), luminance
// (^r), luminance + alpha (^t) or full colour (^x ^w ^_A ...), exactly like the research PNGs.
//
// BFRES (Model/Layout_* icon archives) embed a whole BNTX: BfresBntxDecode finds it.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"
#include "acnh_types.h"

namespace acnh {

/// The ASTC decoder every decode uses (the host's, from configure(); tests install their own).
/// Thread-safe. Without one, ASTC textures fail to decode.
void SetAstcDecoder(void* userdata, EdenDsmodAstcDecoder decode);

struct TexInfo {
    std::string name;
    uint32_t format = 0; ///< BRTI format word (type << 8 | variant)
    int w = 0, h = 0;
    uint8_t selectors[4] = {2, 3, 4, 5};
};

bool BntxDecode(const std::vector<uint8_t>& bntx, std::string_view tex, Image& out);
bool BfresBntxDecode(const std::vector<uint8_t>& bfres, std::string_view tex, Image& out);
/// Texture list of the BNTX at (or embedded in) `bytes`.
std::vector<TexInfo> BntxList(const std::vector<uint8_t>& bytes);
/// Decode the BNTX embedded at `bytes` (BNTX magic searched), texture by name (empty = first).
bool BntxDecodeAny(const uint8_t* bytes, size_t size, std::string_view tex, Image& out);

} // namespace acnh
