// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Mario Kart 8 Deluxe: the id -> name mapping between what the reader reads from game
// memory (course / driver / cup / item ids) and the names the asset keys (mk8d_assets.h) and the
// USen message labels use.
//
// No table here is typed by hand. The name lists are the game's own, read at runtime by the
// reader (mk8d_reader.cpp) from the running game:
//   course folder  course enum text in rodata (anchor main+0x1b54c ADRP+ADD), token = raw + 1
//                  (main+0x8af1e8 `add w0,w8,#1`)
//   course picture UI resource R3+0x350 name table, names[raw + 1] (main+0x418dd0..0x418df0)
//   driver icon    UI resource R3+0x298 name table, names[id + 1] (main+0x414d50..0x414d74) plus
//                  the colour suffix rule below; driver name = Common.msbt label
//                  u32 table R3+0x298+0x90 [id + 1] (main+0x412a28..0x412a44)
//   cup            [R3+0x30]+0xc80+8*raw = {cup, slot} (main+0x50d170); cup icon name pointer
//                  table in .data (anchor main+0x50d194/0x50d19c ADRP+ADD, clamp 0x17)
//   course name    [R3+0x30] + cup*0x80 + slot*0x10 = {raw, -1, msg id, flag} (init main+0x3f947c)
//   item icon      rodata byte table item -> pattern frame (main+0x4234a8 ADRP+ADD, 23 entries)
//                  + the ItemPtn animation of the player's rc_L_ItemBox_00.szs (frame -> texture)
// This file holds the containers those lists land in and the pure code-derived rules.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Mk8dIds {

/// Largest course raw id the game accepts (cup-icon code clamps `raw < 0x7b`, main+0x50d164).
inline constexpr int MaxCourseRaw = 0x7a;
/// Item enum 0..22 (main+0x423498 `cmp w0,#0x16`).
inline constexpr int ItemCount = 23;

/// Map-icon name component of a driver: names[id+1] (+ "0" + ('0' + variant) when the id is in
/// the game's colour-variant mask 0x160000010010 = ids 4, 16, 41, 42, 44, main+0x414e74..0x414e98,
/// and '0' + variant at main+0x414fe0). Ids 29 and 52 (Mii) have no map icon (the preload loop
/// skips them, main+0x414930). "" when unknown.
std::string DriverIconName(const std::vector<std::string>& names, int driver_id, int variant);
/// Common.msbt label of a driver: u32 table [id+1]; -1 when unknown or negative.
int DriverLabel(const std::vector<std::int32_t>& labels, int driver_id);
/// Course folder = course enum token[raw + 1]; "" when unknown.
std::string_view CourseFolder(const std::vector<std::string>& tokens, int course_raw);
/// Course picture component for Library::CoursePictKey: names[raw + 1] without the "_00" the
/// table carries (the key adds it back: ym_CoursePict_<name>_00^u); "" when unknown.
std::string CoursePictName(const std::vector<std::string>& names, int course_raw);
/// Coin kind of a course (main+0x87fac4): 0x33 -> 1, Animal (raw 0x34 + bit of 0x7001,
/// main+0x87ed68) -> 2, 0x46 -> 3, 0x66 -> 4, else 0.
int CoinKind(int course_raw);
/// Pattern frame of an item on this course (main+0x423488): frame_table[item], coin (15)
/// overridden by coin kind 1 -> 0x19, 2 -> 0x1a, 4 -> 0x1d. -1 when unknown.
/// uses_left: the slot's remaining uses (u32 S+0xa0). For the triple items 7/17/18/19 the race
/// HUD shows 2 left as frame 1/17/18/19 and 1 left as frame 0/3/5/7 (main+0x512150..0x512258,
/// jump table rodata 0xf2a558, `fmov` frame constants, `fcsel` on count == 2); other counts keep
/// the table frame.
int ItemFrame(const std::array<std::uint8_t, ItemCount>& frame_table, int item, int course_raw,
              int uses_left = 3);
/// World-Y scale the minimap applies before projecting (main+0x508aa0: raw course 0x44 ->
/// the rodata float at main+0xed66e4, 6.2 in 4.0.0; the reader passes the value it read).
float MapWorldYScale(int course_raw, float scale_0x44);

/// The ItemPtn pattern animation (BFLAN pai1) of rc_L_ItemBox_00: texture list and the frame ->
/// texture key list of pane P_Item_00 (identical on the slot panes).
struct ItemPattern {
    std::vector<std::string> textures;       ///< "tc_Item_Banana^l", ...
    std::vector<std::int32_t> frame_texture; ///< frame -> texture index (-1 = no key)
};
std::optional<ItemPattern> ParseItemPattern(const std::uint8_t* bflan, std::size_t size);
/// Item icon name component for Library::ItemKey ("Banana", "CoinZ"): texture name without the
/// "tc_Item_" prefix (compared case-insensitively, the game ships "tc_item_CoinZ") and the
/// "^l" suffix. "" when unknown.
std::string ItemIconName(const ItemPattern& pattern, int frame);

} // namespace Mk8dIds
