// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_pak.h"
#include "ot0_reader.h"

namespace ot0 {
struct ProtagonistAppearance {
  u32 created{}, job{}, gender{}, hair{}, face{}, eye{}, skin{}, hair_color{},
      personality{};
  u32 childhood_body{}, childhood_hair{}, childhood_face{}, childhood_skin{},
      childhood_hair_color{}, actor_chara_id{};
  bool child{};
  bool operator==(const ProtagonistAppearance &) const = default;
};
// SaveDataMainCharacter at the verified save root+0xa10, with repeated
// roots/state/record checks. Native save data is read only; unsupported
// rendering selectors remain visible here.
std::optional<ProtagonistAppearance>
ReadProtagonistAppearance(const ReadMemory &read, u64 main,
                          int64_t relocation_delta = 0);
// Normal (N) atlas reconstructed from the owner's original part textures using
// native full layer order and red skip threshold. Palette zero only: other
// palettes fail closed until their native selector mapping is verified.
// Original layer alpha bytes are preserved.
std::optional<Image>
LoadProtagonistAtlas(const RangeReader &read,
                     const ProtagonistAppearance &appearance,
                     std::string *why = nullptr);
} // namespace ot0
