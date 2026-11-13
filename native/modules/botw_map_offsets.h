// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_shortcut_offsets.h"
namespace botw {
inline bool map_build_supported(const EdenDsmodHostApi& h) {
  return shortcut_supported(h);
}
struct MapOffsets {
  uint64_t root;
  uint64_t vtable;
  uint64_t controller_root;
  uint64_t controller_vtable;
  uint64_t area_vtable;
  uint64_t pin_vtable;
  uint64_t stamp_vtable;
  uint64_t place_vtable;
  uint64_t shrine_vtable;
  uint64_t tower_vtable;
  uint64_t start_vtable;
  uint64_t beast_vtable;
  uint64_t lab_vtable;
  uint64_t ex_vtable;
  uint64_t korok_vtable;
  uint64_t horse_vtable;
  uint64_t yunobo_vtable;
  uint64_t wolf_vtable;
  uint64_t bike_vtable;
  uint64_t medallion_vtable;
  uint64_t guardian_vtable;
  uint64_t name_vtable;
  uint64_t label_source_vtable;
  uint64_t discovered_source_vtable;
  uint64_t death_vtable;
  uint64_t death_root;
  uint64_t death_manager_vtable;
  uint64_t player_marker_vtable;
  uint64_t save_handles;
  uint64_t position_flag_vtable;
  uint64_t screen_vtable;
  uint64_t null_pane_vtable;
  uint64_t picture_pane_vtable;
  uint64_t root_pane_vtable;
  uint32_t minimap_sr, minimap_root;
};
inline constexpr MapOffsets Map160{
  0x2CA63B0, // root
  0x2B86848, // vtable
  0x2CA63B8, // controller_root
  0x2B8AAD8, // controller_vtable
  0x2B86868, // area_vtable
  0x2B866B0, // pin_vtable
  0x2B874F0, // stamp_vtable
  0x2B868E8, // place_vtable
  0x2B86010, // shrine_vtable
  0x2B86A00, // tower_vtable
  0x2B86270, // start_vtable
  0x2B86388, // beast_vtable
  0x2B864A0, // lab_vtable
  0x2B86158, // ex_vtable
  0x2B86C10, // korok_vtable
  0x2B86B18, // horse_vtable
  0x2B86D18, // yunobo_vtable
  0x2B86E10, // wolf_vtable
  0x2B86F08, // bike_vtable
  0x2B875E8, // medallion_vtable
  0x2B865B8, // guardian_vtable
  0x2B87200, // name_vtable
  0x2BB1D70, // label_source_vtable
  0x2BB1E00, // discovered_source_vtable
  0x2B873F8, // death_vtable
  0x2CA3E18, // death_root
  0x2B7D758, // death_manager_vtable
  0x2B87300, // player_marker_vtable
  0x2B7E7D8, // save_handles
  0x2BD15F0, // position_flag_vtable
  0x2B93A50, // screen_vtable
  0x2C22488, // null_pane_vtable
  0x2BBD870, // picture_pane_vtable
  0x2BBD968, // root_pane_vtable
  0x5410, 0x3E30,
};
inline constexpr MapOffsets Map190{
  0x1E17EF8, // root
  0x1CDD6C8, // vtable
  0x1E17BB8, // controller_root
  0x1CE0748, // controller_vtable
  0x1CDBFB0, // area_vtable
  0x1CDD438, // pin_vtable
  0x1CDD340, // stamp_vtable
  0x1CDC0C8, // place_vtable
  0x1CDC1E0, // shrine_vtable
  0x1CDC410, // tower_vtable
  0x1CDC528, // start_vtable
  0x1CDC640, // beast_vtable
  0x1CDC758, // lab_vtable
  0x1CDC2F8, // ex_vtable
  0x1CDC870, // korok_vtable
  0x1CDC978, // horse_vtable
  0x1CDCA70, // yunobo_vtable
  0x1CDCB68, // wolf_vtable
  0x1CDCC60, // bike_vtable
  0x1CDD530, // medallion_vtable
  0x1CDD248, // guardian_vtable
  0x1CDCF58, // name_vtable
  0x1D01DC0, // label_source_vtable
  0x1D01D90, // discovered_source_vtable
  0x1CDD058, // death_vtable
  0x1E152F8, // death_root
  0x1CD9590, // death_manager_vtable
  0x1CDD150, // player_marker_vtable
  0x1DA6D60, // save_handles
  0x1D21F98, // position_flag_vtable
  0x1CE8298, // screen_vtable
  0x1B7CAC0, // null_pane_vtable
  0x1D0E408, // picture_pane_vtable
  0x1D0E500, // root_pane_vtable
  0x5270, 0x3DB0,
};
inline const MapOffsets& map_offsets(const EdenDsmodHostApi& h) {
  return matches_build(h,InventoryBuild190)?Map190:Map160;
}
}
