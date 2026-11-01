// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_pak.h"
namespace ot0 {
// Battle sprite sheet of an enemy, by exact native texture object path
// (/Game/Character/Resource/V1_0_0/Enm/Textures/<Name>_cl.<Name>_cl, from DecodeEnemies).
// Every texture uses header/stored/decoded pins (ot0_enemy_pins.inc).
std::optional<Image> LoadEnemyTexture(const RangeReader &read, std::string_view object_path,
                                      std::string *why = nullptr);
std::size_t EnemyTextureCount();
} // namespace ot0
