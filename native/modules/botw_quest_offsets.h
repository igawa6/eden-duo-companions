// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_ready_offsets.h"
namespace botw {
inline bool quests_supported(const EdenDsmodHostApi& h) {
  return ready_supported(h);
}
struct QuestOffsets {
  uint64_t root,manager_vtable,quest_vtable,step_vtable,destination_handle;
  uint64_t dlc_root,dlc_vtable;
  uint32_t dlc_version;
  uint64_t shrine_vtable,tower_vtable;
};
inline constexpr QuestOffsets Quests160{0x2CD1700,0x2BEBA70,0x2BEBA90,0x2BEBAF0,0x2B7E8D0,0x2CC4A40,0x2BCD5C0,0xD0,0x2B86010,0x2B86A00};
inline constexpr QuestOffsets Quests190{0x1E46720,0x1D3BBD8,0x1D3BBF8,0x1D3BC58,0x1DA6E60,0x1E39A00,0x1D1E300,0xD8,0x1CDC1E0,0x1CDC410};
inline const QuestOffsets& quest_offsets(const EdenDsmodHostApi& h) {
  return matches_build(h,InventoryBuild190)?Quests190:Quests160;
}
}
