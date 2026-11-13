// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_quest_offsets.h"
namespace botw {
inline bool shortcut_supported(const EdenDsmodHostApi& h) {
#ifdef BOTW_190_RUNES_EVIDENCE
  return quests_supported(h);
#else
  return supported(h);
#endif
}
struct ShortcutOffsets {
  uint64_t screen_vtable,world_state,menu_state,end_state,controller_vtable;
  uint32_t screen_shift;
};
inline constexpr ShortcutOffsets Shortcut160{0x2BA1D40,0x2BA1B50,0x2BA1C10,0x2BA1C70,0x2B858B0,0};
inline constexpr ShortcutOffsets Shortcut190{0x1CF51D8,0x1E2D538,0x1E2D598,0x1E2D5F8,0x1CDB958,24};
inline const ShortcutOffsets& shortcut_offsets(const EdenDsmodHostApi& h) {
#ifdef BOTW_190_RUNES_EVIDENCE
  return matches_build(h,InventoryBuild190)?Shortcut190:Shortcut160;
#else
  (void)h;return Shortcut160;
#endif
}
}
