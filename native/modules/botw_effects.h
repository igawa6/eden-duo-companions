// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_player_equipment.h"
#include "botw_ready_offsets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
namespace botw {
// Active timed food/elixir effects on the validated Player (exact 1.6.0): 12 levels (u32) at
// Player+0x1FF0 and 12 timer triples (f32 frames at 30 fps) at Player+0x1ED8; the third float of
// every triple is the -1 sentinel. Slot -> native HUD effect, live-probed one slot at a time
// against the native HUD (iteration 14 item 5): 2 Speed Up (Hasty
// also fills 0 and 1, which the HUD does not list), 5 Attack Up, 6 Defense Up, 7 Heat Resistance,
// 8 Cold Resistance, 9 Flame Guard, 11 Shock Resistance; 3, 4 and 10 show nothing natively and
// get no row (they used to fall through to an "Other" row drawn with the material/apple icon).
enum class EffectKind:int {Speed=0,Attack=1,HeatResist=2,Other=3,Defense=4,ShockResist=5,ColdResist=6,FlameGuard=7};
inline bool effect_slot_kind(unsigned slot,EffectKind& kind) {
  switch(slot) {
    case 0:case 1:case 2:kind=EffectKind::Speed;return true;
    case 5:kind=EffectKind::Attack;return true;
    case 6:kind=EffectKind::Defense;return true;
    case 7:kind=EffectKind::HeatResist;return true;
    case 8:kind=EffectKind::ColdResist;return true;
    case 9:kind=EffectKind::FlameGuard;return true;
    case 11:kind=EffectKind::ShockResist;return true;
    default:return false;
  }
}
struct ActiveEffect {EffectKind kind{};int level{};int seconds{};};
struct ActiveEffects {bool valid{};unsigned count{};std::array<ActiveEffect,7> rows{};};
inline ActiveEffects active_effects_from(const std::array<uint32_t,12>& level,const std::array<float,36>& timer) {
  ActiveEffects out;
  for(unsigned i=0;i<12;++i) {
    if(level[i]>16 || timer[i*3+2]!=-1.0f)return {};
    for(unsigned j=0;j<2;++j)if(!std::isfinite(timer[i*3+j]) || timer[i*3+j]<-1.0f || timer[i*3+j]>30.0f*60*60*24)return {};
  }
  auto add=[&](EffectKind kind,int lv,float frames){
    if(lv<=0 || frames<=0)return;
    const int secs=int(frames/30.0f+0.999f);
    for(unsigned r=0;r<out.count;++r)if(out.rows[r].kind==kind){out.rows[r].seconds=std::max(out.rows[r].seconds,secs);out.rows[r].level=std::max(out.rows[r].level,lv);return;}
    if(out.count<out.rows.size())out.rows[out.count++]={kind,lv,secs};
  };
  for(unsigned i=0;i<12;++i) {
    EffectKind kind{};
    if(effect_slot_kind(i,kind))add(kind,int(level[i]),timer[i*3]);
  }
  out.valid=true;return out;
}
inline ActiveEffects active_effects(const EdenDsmodHostApi& h,uint64_t player) {
  if(!ready_supported(h) || !player || player>UINT64_MAX-0x2100)return {};
  std::array<uint32_t,12> a{},b{};std::array<float,36> t{};
  if(!read(h,player+0x1FF0,a) || !read(h,player+ready_offsets(h)->effect_timers,t) || !read(h,player+0x1FF0,b) || a!=b)return {};
  return active_effects_from(a,t);
}
inline void format_effect_time(int seconds,char (&out)[16]) {
  seconds=std::clamp(seconds,0,99*60+59);std::snprintf(out,sizeof(out),"%d:%02d",seconds/60,seconds%60);
}
} // namespace botw
