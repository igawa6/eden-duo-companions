// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_map_offsets.h"
namespace botw {
struct WarpOffsets {
  uint64_t event_root,event_vtable,event_list_vtable,event_current,latch_root;
  uint64_t medallion_map,medallion_pos;
};
inline constexpr WarpOffsets Warp160{0x2CC56D8,0x2BCF2F8,0x2BCF898,0x1D2B8,0x2CD1590,0x2651911,0x269652D};
inline constexpr WarpOffsets Warp190{0x1E3A638,0x1D1FCE8,0x1D201A0,0x1D2B8,0x1E465B0,0x15EEDEF,0x161794D};
inline bool warp_build_supported(const EdenDsmodHostApi& h) {
  return map_build_supported(h);
}
inline const WarpOffsets* warp_offsets(const EdenDsmodHostApi& h) {
  return matches_build(h,InventoryBuild190) ? &Warp190 : matches_build(h,Build) ? &Warp160 : nullptr;
}
}
