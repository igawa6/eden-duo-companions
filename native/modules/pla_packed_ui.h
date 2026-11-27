// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pla {
// Exact 1.1.1 native UI members needed by the read-only companion pages. The
// caller reads only this path/size from the player's ROMFS. No game art is
// embedded here, and unknown archives are never passed to the unsafe codec.
enum class PackedUiMember {
    BattleMoves, PokemonCard, PokemonTask, MapTabs, HudArcPhone, PokedexAreas,
    BattleTimeline, BattleHeader
};
const char *PackedUiPath(PackedUiMember member);
size_t PackedUiSize(PackedUiMember member);
std::vector<uint8_t> DecodePackedUi(PackedUiMember member,
                                    std::span<const uint8_t> archive);
} // namespace pla
