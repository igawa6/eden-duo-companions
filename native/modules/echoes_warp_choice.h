// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_warp_menu.h"
namespace echoes {
enum class NativeWarpChoice : uint8_t { Warp=0, Cancel=1 };
struct WarpChoiceSnapshot {
 bool compatible{},valid{};
 uint64_t menu{},focused{},popup{},popup_owner{};
 uint32_t destination_index{};
 NativeWarpChoice choice{NativeWarpChoice::Cancel};
 bool operator==(const WarpChoiceSnapshot&) const=default;
};
// Exact native MapMenu state4 popup only. The expected menu must come from the
// authenticated current ReadWarpMenu observation; caller also brackets same-save
// unlock, context and interference before any host input. This reads no event
// result and grants no travel permission by itself.
WarpChoiceSnapshot ReadWarpChoice(const ReadFn&,uint64_t main_base,bool build_matches,
 const WarpMenuSnapshot& expected);
} // namespace echoes
