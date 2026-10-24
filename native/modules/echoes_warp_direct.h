// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_warp_menu.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
namespace echoes {
// Caller joins fresh same-save unlock/context before this request, supplies the
// fully authenticated native snapshot, then observes exact native icon focus.
// Only the original UI cursor pair is written: no world/save/shared_ptr edits.
// Native update 25b7428/25b74fc writes/clamps this same aligned XY pair. A short
// nonzero native refresh input must already be active before it to bypass old-focus easing;
// one write is not proof of native focus or completed travel.
template<class Write> bool DirectWarpTarget(const ReadFn &read,uint64_t base,bool matches,
 const NativeWarpCatalog &catalog,const WarpMenuSnapshot &expected,uint32_t ordinal,Write &&write) {
 if(!expected.compatible||!expected.valid||expected.state!=WarpMapState::Normal||
    expected.map_level!="Field"||expected.warp_set!="Field"||!expected.component||
    expected.component%8||expected.component>UINT64_MAX-0x3a0||
    !expected.targets_known||!expected.cursor_known||!expected.native_view)return false;
 const auto at=std::find_if(expected.targets.begin(),expected.targets.end(),
                            [ordinal](const auto &v){return v.ordinal==ordinal;});
 if(at==expected.targets.end()||!at->component||!at->selectable||!at->native_allowed||
    !at->cursor_known||!std::isfinite(at->cursor_x)||!std::isfinite(at->cursor_y)||
    std::abs(at->cursor_x)>1e7f||std::abs(at->cursor_y)>1e7f)return false;
 // Re-read the complete authenticated source and target set. The warmed-up
 // native stick may legitimately change cursor/focus between observations;
 // those transient values are not the target authority. Map/icon/permission
 // identity must still be identical. No retry after a source difference.
 const auto fresh=ReadWarpMenu(read,base,matches,catalog,true);
 if(!fresh.compatible||!fresh.valid||fresh.state!=WarpMapState::Normal||
    fresh.component!=expected.component||fresh.native_view!=expected.native_view||
    fresh.map_index!=expected.map_index||fresh.map_level!=expected.map_level||
    fresh.warp_set!=expected.warp_set||!fresh.cursor_known||!fresh.targets_known||
    fresh.targets!=expected.targets)return false;
 const std::array<uint32_t,2> pair{std::bit_cast<uint32_t>(at->cursor_x),std::bit_cast<uint32_t>(at->cursor_y)};
 std::array<uint8_t,8> bytes{};
 for(size_t p=0;p<2;++p)for(size_t b=0;b<4;++b)bytes[p*4+b]=uint8_t(pair[p]>>(8*b));
 return write(expected.component+0x398,bytes.data(),bytes.size());
}
// Native zoom-out transitions preserve the previous index and interpolate
// through Zoom state1. Queue that same lifecycle once with target index0;
// the original entry owns animation, layout callbacks, sound and camera.
template<class Write> bool RequestNativeFullZoom(const ReadFn &read,uint64_t base,bool matches,
 const NativeWarpCatalog &catalog,const WarpMenuSnapshot &expected,Write &&write) {
 if(!expected.compatible||!expected.valid||expected.state!=WarpMapState::Normal||
    expected.map_level!="Field"||expected.warp_set!="Field"||!expected.component||expected.component%8||
    expected.component>UINT64_MAX-0x4a8||!expected.zoom_valid||expected.zoom_scales.empty()||
    expected.zoom_index>=expected.zoom_scales.size()||expected.zoom_previous!=expected.zoom_index||
    expected.zoom_state!=0||expected.zoom_pending!=0xfe||
    std::abs(expected.zoom_scale-expected.zoom_scales[expected.zoom_index])>0.0001f)return false;
 const auto fresh=ReadWarpMenu(read,base,matches,catalog,true);
 if(!fresh.compatible||!fresh.valid||fresh.state!=WarpMapState::Normal||
    fresh.component!=expected.component||fresh.native_view!=expected.native_view||
    fresh.map_index!=expected.map_index||fresh.map_level!=expected.map_level||fresh.warp_set!=expected.warp_set||
    !fresh.zoom_valid||fresh.zoom_index!=expected.zoom_index||fresh.zoom_previous!=expected.zoom_previous||
    fresh.zoom_state!=expected.zoom_state||fresh.zoom_pending!=expected.zoom_pending||
    fresh.zoom_scales!=expected.zoom_scales||fresh.zoom_scale!=expected.zoom_scale)return false;
 if(expected.zoom_index==0)return true;
 const uint8_t index=0,pending=1;
 if(!write(expected.component+0x418,&index,1))return false;
 return write(expected.component+0x26a,&pending,1);
}
} // namespace echoes
