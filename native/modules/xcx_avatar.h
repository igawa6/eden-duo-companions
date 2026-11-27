// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The player's avatar portrait. The game draws it from the icon_face_avatar atlas: the cell is the
// avatar's hair style, the hair is tinted with one of the avatar's palette colours (contracts/
// AVATAR.md). Everything here is decoded at run time from the player's own memory and RomFS; no
// game pixels are packaged.
#pragma once
#include "xcx_assets.h"
#include "xcx_reader.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
namespace xcx {
struct AvatarPortraitSpec {
    int cell{};                    // index into icon_face_avatar (16 columns of 112 px)
    std::array<float, 4> tint{};   // the hair colour, palette entry Color5 (r, g, b, a/128)
};
// BDAT lookups on the player's common.bdat: hair row -> cell, palette id -> colour.
std::optional<AvatarPortraitSpec> ResolveAvatar(Archive &, const EdenDsmodHostApi &,
                                                const AvatarLook &);
// 112 x 112 RGBA: atlas cell `spec.cell` of the two icon_face_avatar textures, hair tinted.
Image ComposeAvatar(const Image &base, const Image &mask, const AvatarPortraitSpec &spec);
// Convenience: ResolveAvatar + ComposeAvatar from the player's RomFS. Empty on any failure.
Image AvatarPortrait(Assets &, const EdenDsmodHostApi &, const AvatarLook &);
} // namespace xcx
