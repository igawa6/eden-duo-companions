// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/mods/dsmod_module_abi.h"
#include <array>
#include <cstdint>
#include <cstring>
namespace botw {
inline constexpr char Build[] =
    "8E9978D50BDD20B4C8395A106C27FFDE00000000000000000000000000000000";
inline constexpr char InventoryBuild190[] =
    "CD57B23FA4BBAD65803D9788C01821EE00000000000000000000000000000000";
inline bool matches_build(const EdenDsmodHostApi& h, const char* build) {
  if (h.title_id != 0x01007EF00011E000ULL) return false;
  for (unsigned i=0;i<32;++i) {
    auto hex=[](char c){return c<='9'?c-'0':c-'A'+10;};
    if (h.build_id[i] != uint8_t((hex(build[2*i])<<4)|hex(build[2*i+1]))) return false;
  }
  return true;
}
// Full support remains limited to 1.6.0 until every reader/action is verified.
inline bool supported(const EdenDsmodHostApi& h) {return matches_build(h,Build);}
inline bool inventory_supported(const EdenDsmodHostApi& h) {
  if (supported(h)) return true;
#ifdef BOTW_190_INVENTORY_EVIDENCE
  // Compile only for an isolated, disposable inventory verification package.
  return matches_build(h,InventoryBuild190);
#else
  return false;
#endif
}
struct InventoryOffsets {
  uint64_t pouch_root;
  uint64_t pouch_vtable;
  uint64_t pouch_string_vtable;
  uint64_t ui_root;
  uint64_t screen_root;
  uint64_t ui_vtable;
  uint64_t screens_vtable;
  uint64_t player_info_root;
  uint64_t player_info_vtable;
  uint64_t player_vtable;
  uint64_t creator_root;
  uint64_t creator_vtable;
  uint64_t player_gear_vtable;
  uint64_t player_armor_vtable;
  uint64_t weapon_actor_vtable;
  uint64_t armor_actor_vtable;
  uint64_t info_data_root;
  uint64_t info_data_vtable;
  uint64_t eat_player_root;
  uint64_t eat_root_vtable;
  uint64_t pause_player_vtable;
  uint64_t inventory_screen_vtable;
  uint64_t inventory_column_vtable;
  uint64_t carry_root;
  uint64_t carry_vtable;
  uint64_t carry_record_vtable;
  uint64_t carry_actor_vtable;
  uint64_t actor_heap_root;
  uint64_t native_string_vtable;
  std::array<uint64_t,6> defaults, default_names;
  uint32_t pause_shift;
  uint64_t gamedata_root, gamedata_vtable, bool_flag_vtable, int_flag_vtable;
};
inline constexpr InventoryOffsets Inventory160{
  0x2CA6D50, // pouch_root
  0x2B87780, // pouch_vtable
  0x2A6FAF0, // pouch_string_vtable
  0x2CBDB80, // ui_root
  0x2CC2490, // screen_root
  0x2BB1AB8, // ui_vtable
  0x2BBEA98, // screens_vtable
  0x2CA1140, // player_info_root
  0x2B7BE80, // player_info_vtable
  0x2B7BEA8, // player_vtable
  0x2C9A458, // creator_root
  0x2B4D9C8, // creator_vtable
  0x2BE40B0, // player_gear_vtable
  0x2BD75A8, // player_armor_vtable
  0x2AE2348, // weapon_actor_vtable
  0x2A6D608, // armor_actor_vtable
  0x2CC4490, // info_data_root
  0x2BCAB60, // info_data_vtable
  0x2CA4A38, // eat_player_root
  0x2B81128, // eat_root_vtable
  0x2B60C20, // pause_player_vtable
  0x2BA7240, // inventory_screen_vtable
  0x2B8AEE8, // inventory_column_vtable
  0x2C9A2E8, // carry_root
  0x2B4D938, // carry_vtable
  0x2B4D870, // carry_record_vtable
  0x2B60188, // carry_actor_vtable
  0x2CA1B10, // actor_heap_root
  0x2A6E6C0, // native_string_vtable
  {0x2CA68F0, 0x2CA6920, 0x2CA6900, 0x2CA6930, 0x2CA6940, 0x2CA6950},
  {0x265BC1E, 0x2640BC8, 0x2635EDB, 0x262092C, 0x2667123, 0x2661AFE},
  0x0,
  0x2CC5FE8, 0x2BD07E0, 0x2BD1078, 0x2BD1140,
};
inline constexpr InventoryOffsets Inventory190{
  0x1E18570, // pouch_root
  0x1CDD7D8, // pouch_vtable
  0x1BC46E0, // pouch_string_vtable
  0x1E328D8, // ui_root
  0x1E374B8, // screen_root
  0x1D01CB8, // ui_vtable
  0x1D0F658, // screens_vtable
  0x1E13ED0, // player_info_root
  0x1CD7B90, // player_info_vtable
  0x1CD7BB8, // player_vtable
  0x1E0C998, // creator_root
  0x1CA8F78, // creator_vtable
  0x1D349D8, // player_gear_vtable
  0x1D282A8, // player_armor_vtable
  0x1C3F330, // weapon_actor_vtable
  0x1BC2210, // armor_actor_vtable
  0x1E39450, // info_data_root
  0x1D1B3F8, // info_data_vtable
  0x1E15FB8, // eat_player_root
  0x1CDA3C0, // eat_root_vtable
  0x1CBBC98, // pause_player_vtable
  0x1CF9AD8, // inventory_screen_vtable
  0x1CE0B48, // inventory_column_vtable
  0x1E0C828, // carry_root
  0x1CA8EE8, // carry_vtable
  0x1CA8E20, // carry_record_vtable
  0x1CBB040, // carry_actor_vtable
  0x1E14CD8, // actor_heap_root
  0x1BC32C8, // native_string_vtable
  {0x1E18110, 0x1E18140, 0x1E18120, 0x1E18150, 0x1E18160, 0x1E18170},
  {0x15F3B72, 0x15D71D1, 0x165BB15, 0x15BFB92, 0x16107FB, 0x163E7D9},
  0x18,
  0x1E3B188, 0x1D210E0, 0x1D21A20, 0x1D21AE8,
};
inline const InventoryOffsets& inventory_offsets(const EdenDsmodHostApi& h) {
#ifdef BOTW_190_INVENTORY_EVIDENCE
  return matches_build(h,InventoryBuild190) ? Inventory190 : Inventory160;
#else
  (void)h;
  return Inventory160;
#endif
}
} // namespace botw
