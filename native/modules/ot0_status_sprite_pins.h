// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_pak.h"
namespace ot0 {
struct StatusSpritePin {
  std::string_view object_path, atlas_path;
  // Native PaperSprite SourceUV / SourceDimension, pixel units, never a grid guess.
  std::array<u32, 4> rectangle;
  PakMember frame, package_header;
};
#include "ot0_status_sprite_pins.inc"
} // namespace ot0
