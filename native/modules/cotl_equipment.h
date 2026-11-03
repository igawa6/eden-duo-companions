// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Values of the equipment cards as in the game: WeaponInfoCard, CurseInfoCard, RelicInfoCard.
// Formulas: PlayerWeapon.GetDamage and PlayerSpells.GetCurseDamageMultiplier (tools/cotl/README.md).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "cotl_snapshot.h"

namespace cotl {

inline constexpr int32_t kNoEquipment = 9999; // EquipmentType.None

// Average weapon damage, curse damage; nullopt without read data
std::optional<float> WeaponDamage(const Run& r);
std::optional<float> CurseDamage(const Run& r);
// RelicData.GetChargeCategory (DamageRequiredToCharge)
std::string ChargeCategory(float damage_required);
// IntExtensions.ToNumeral: Roman 1-10, otherwise empty
std::string ToNumeral(int32_t n);
// float.ToString with at most two decimals, no trailing zeros
std::string FormatStat(float v);

} // namespace cotl
