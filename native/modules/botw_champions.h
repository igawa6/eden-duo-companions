// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_hud.h"
#include <cstdio>

namespace botw {
// Exact Switch 1.6.0 Player state, canonical order Zora/Rito/Goron/Gerudo.
// GameData SupportReuseTime* are persistence snapshots, not the live timers.
// Native getters +F4CD90..F4CE34; native ticking +F3C990..F3CB0C;
// Wind exhaustion +EC8304..EC8388 initializes seconds*30 and resets use count.
struct ChampionAbility {
  bool valid{},recharging{};
  int32_t charges{-1},seconds{-1};
};
using ChampionAbilities=std::array<ChampionAbility,4>;
inline ChampionAbilities champion_abilities(const EdenDsmodHostApi& h) {
  ChampionAbilities out{};
  UiSample before{},after{};
  if (!ready_supported(h)||h.main_size<inventory_offsets(h)->player_info_root+8||h.main_base>UINT64_MAX-(inventory_offsets(h)->player_info_root+8)||
      !ui_sample(h,before)||!hud_context(before)) return out;
  struct Player {uint64_t info{},info_vt{},actor{},actor_vt{};bool operator==(const Player&)const=default;};
  auto ptr=[](uint64_t p,uint64_t span){return p>=0x10000&&p%8==0&&p<=UINT64_MAX-span;};
  auto player=[&](Player& p){
    return read(h,h.main_base+inventory_offsets(h)->player_info_root,p.info)&&ptr(p.info,0x68)&&
      read(h,p.info,p.info_vt)&&p.info_vt==h.main_base+inventory_offsets(h)->player_info_vtable&&
      read(h,p.info+0x60,p.actor)&&ptr(p.actor,ready_offsets(h)->champion_timers[0]+12)&&
      read(h,p.actor,p.actor_vt)&&p.actor_vt==h.main_base+inventory_offsets(h)->player_vtable;
  };
  Player a{},z{};
  if(!player(a))return out;
  const auto& timer_offsets=ready_offsets(h)->champion_timers;
  const auto& count_offsets=ready_offsets(h)->champion_counts;
  struct Raw {float frames{},rate{};int32_t count{};bool operator==(const Raw&)const=default;};
  for(unsigned i=0;i<4;++i){
    auto sample=[&](Raw& v){
      v.count=1;
      return read(h,a.actor+timer_offsets[i],v.frames)&&read(h,a.actor+timer_offsets[i]+8,v.rate)&&
        (!count_offsets[i]||read(h,a.actor+count_offsets[i],v.count));
    };
    Raw first{},last{};
    if(!sample(first)||!sample(last)||first!=last||!std::isfinite(first.frames)||
       !std::isfinite(first.rate)||first.frames<-30.f||first.frames>1000000.f||
       first.count<0||first.count>3)continue;
    auto& ability=out[i];
    if(first.frames>0.f){
      // Native rate incorporates the current upgraded/castle recharge multiplier.
      // Refuse zero/positive/implausible rates rather than inventing a countdown.
      if(first.rate>=0.f||first.rate<-100.f)continue;
      const double seconds=std::ceil(double(first.frames)/(30.0*-double(first.rate)));
      if(!std::isfinite(seconds)||seconds<1||seconds>86400)continue;
      ability={true,true,0,int32_t(seconds)};
    }else{
      ability={true,false,first.count,0};
    }
  }
  if(!player(z)||z!=a||!ui_sample(h,after)||after!=before)return {};
  return out;
}
inline std::string champion_countdown(int32_t seconds){
  if(seconds<0)return {};
  char text[24]{};
  if(seconds<6000)std::snprintf(text,sizeof(text),"%d:%02d",seconds/60,seconds%60);
  else std::snprintf(text,sizeof(text),"%dh%02d",seconds/3600,(seconds/60)%60);
  return text;
}
} // namespace botw
