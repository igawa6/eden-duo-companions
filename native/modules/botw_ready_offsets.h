// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_inventory_offsets.h"
namespace botw {
inline bool ready_supported(const EdenDsmodHostApi& h) {
#ifdef BOTW_190_READY_EVIDENCE
  return inventory_supported(h);
#else
  return supported(h);
#endif
}
struct ReadyOffsets {
  uint64_t float_flag_vtable;
  uint64_t rune_root;
  uint64_t rune_vtable;
  uint64_t world_root;
  uint64_t world_vtable;
  uint64_t weather_vtable;
  uint64_t temperature_vtable;
  uint64_t map_root;
  uint64_t map_vtable;
  uint64_t sensor_root;
  uint64_t sensor_vtable;
  uint64_t hud_vtable;
  uint64_t sensor_pane_vtable;
  uint64_t temperature_pane_vtable;
  uint64_t animator_vtable;
  uint64_t stage_current;
  uint64_t stage_requested;
  uint64_t map_type;
  uint64_t stage_vtable;
  uint32_t pane_shift;
  std::array<uint32_t,4> champion_timers,champion_counts;
  uint64_t physics_body_vtable;
};
inline constexpr ReadyOffsets Ready160{
  0x2BD1208, // float_flag_vtable
  0x2C9A668, // rune_root
  0x2B4DD40, // rune_vtable
  0x2CE1E30, // world_root
  0x2C02270, // world_vtable
  0x2C01B60, // weather_vtable
  0x2C018B8, // temperature_vtable
  0x2CA63B0, // map_root
  0x2B86848, // map_vtable
  0x2CA66D0, // sensor_root
  0x2B87700, // sensor_vtable
  0x2BA14E8, // hud_vtable
  0x2B8D980, // sensor_pane_vtable
  0x2B8F8D0, // temperature_pane_vtable
  0x2BBE308, // animator_vtable
  0x2C9E868, // stage_current
  0x2CA4DA0, // stage_requested
  0x2BE1708, // map_type
  0x2A701C8, // stage_vtable
  0,
  {0x1E18,0x1DF4,0x1E00,0x1E0C}, {0,0x1CC8,0x1CD0,0x1CCC},
  0x2BCE588, // physics_body_vtable
};
inline constexpr ReadyOffsets Ready190{
  0x1D21BB0, // float_flag_vtable
  0x1E0CD40, // rune_root
  0x1CA92D8, // rune_vtable
  0x1E57580, // world_root
  0x1D518E0, // world_vtable
  0x1D51020, // weather_vtable
  0x1D50D78, // temperature_vtable
  0x1E17EF8, // map_root
  0x1CDD6C8, // map_vtable
  0x1E17EE0, // sensor_root
  0x1CDD708, // sensor_vtable
  0x1CF4C20, // hud_vtable
  0x1CE2EA8, // sensor_pane_vtable
  0x1CE4C08, // temperature_pane_vtable
  0x1D0EEA0, // animator_vtable
  0x1E11360, // stage_current
  0x1E162B8, // stage_requested
  0x1DAE3A0, // map_type
  0x1BC4E70, // stage_vtable
  24,
  {0x1E1C,0x1DF8,0x1E04,0x1E10}, {0,0x1CCC,0x1CD4,0x1CD0},
  0x1D1F000, // physics_body_vtable
};
inline const ReadyOffsets& ready_offsets(const EdenDsmodHostApi& h) {
#ifdef BOTW_190_READY_EVIDENCE
  return matches_build(h,InventoryBuild190) ? Ready190 : Ready160;
#else
  (void)h;return Ready160;
#endif
}
} // namespace botw
