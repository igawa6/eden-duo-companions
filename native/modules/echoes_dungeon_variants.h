// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_dungeon_assets.h"
namespace echoes {
struct DungeonVariantsSnapshot {
 bool compatible{}, known{};
 MapEpochSource source{};
 std::vector<DungeonRoomVariant> variants;
};
// Bounded read-only optional exact GlobalFlags observations. The caller brackets this
// result with authenticated dungeon progress and normal gameplay/context. It
// grants neither room visibility nor travel permission. Unknown clears results.
DungeonVariantsSnapshot ReadDungeonVariants(const ReadFn&, uint64_t main_base,
 bool build_matches, const MapEpochSource&, const DungeonMapArt&);
} // namespace echoes
