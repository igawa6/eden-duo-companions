// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Field page geometry of the Dragon Quest III HD-2D Remake companion (revision 2 "current-compact", 1240 x 1080 canvas)
// that the module's page values depend on. tools/dq3/gen_manifest.py reads these constants (and the
// Map page ones in dq3_map.h, the FontStyle enumerators in dq3_font.h) and fails when its own layout
// differs. Internal header.
#pragma once

namespace dq3 {

inline constexpr int FTabX0 = 36, FTabW = 1168, FTabH = 88; ///< command tabs (gen_manifest X0, CW, TAB_H)
inline constexpr int FieldTabs = 4;                       ///< Map, Party, Bag, Journal (gen_manifest FTABS)
inline constexpr int FieldCardW = 286, FieldCardH = 188;  ///< top party cards
inline constexpr int FieldTextW = 174;                    ///< the card's text column (gen_manifest TW)
inline constexpr int StripGap = 8;                        ///< gap between the top cards (gen_manifest G)
/// Top party strip (owner 2026-10-10): N = 1..4 active members share the content box equally,
/// card width (FTabW - StripGap * (N - 1)) / N = 1168 / 580 / 384 / 286 (gen_manifest strip_w); the battle
/// party row uses the same rects. Card i of N starts at StripCardX(N, i).
constexpr int StripCardW(int n) { return n < 1 || n > 4 ? 0 : (FTabW - StripGap * (n - 1)) / n; }
constexpr int StripCardX(int n, int i) { return FTabX0 + i * (StripCardW(n) + StripGap); }
static_assert(StripCardW(4) == FieldCardW && StripCardW(3) == 384 && StripCardW(2) == 580 && StripCardW(1) == FTabW);
static_assert(StripCardX(4, 3) + StripCardW(4) == FTabX0 + FTabW && StripCardX(3, 2) + StripCardW(3) == FTabX0 + FTabW &&
              StripCardX(2, 1) + StripCardW(2) == FTabX0 + FTabW);
inline constexpr int PartyCellW = 132;                    ///< Party grid cells (gen_manifest grid_geom)
inline constexpr int CellTextPad = 14;                    ///< a cell name wraps at cell width - this
inline constexpr int SlotTextW = 195;                     ///< equipped-slot label / name width (gen_manifest SLOT_TW)
inline constexpr int JournalWrapW = 418;                  ///< objective width (gen_manifest JLIW)
inline constexpr int BagNameW = 436;                      ///< Bag list name room (gen_manifest B_NAME_W)
inline constexpr int BagPaneNameW = 420;                  ///< Bag pane name room (gen_manifest B_PANE_NAME_W)
inline constexpr int SpellNameW = 386;                    ///< Party Spells list name room (gen_manifest SPELL_NAME_W)
inline constexpr int BagSubGap = 30;                      ///< Bag sub-tabs start 30 px after the bag's title
inline constexpr int MaxRows = 320;                       ///< list rows published (gen_manifest ROWS)

} // namespace dq3
