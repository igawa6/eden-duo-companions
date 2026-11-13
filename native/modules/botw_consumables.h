// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_inventory.h"
#include "botw_consume_metadata.h"
#include <cmath>
namespace botw {
struct Consumable {
  bool valid{},recipe{},health_only{};
  int32_t use{},health{},frames{},effect{-1},level{};
  std::array<uint32_t,5> cook{};
};
inline bool consume_effect(int effect) {
  switch(effect) {case -1:case 1:case 2:case 4:case 5:case 6:case 10:case 11:case 12:case 13:case 14:case 15:case 16:return true;default:return false;}
}
// This reads metadata only. Native Eat remains responsible for effects and save.
inline Consumable consumable(const EdenDsmodHostApi& h,const Item& item) {
  if(!inventory_supported(h) || (item.type!=7 && item.type!=8) || item.equipped || item.value<1 || item.value>999999)return {};
  const auto* info=consume_info(item.actor);if(!info)return {};
  std::array<uint8_t,0x98> a{},b{};
  if(item.address%8 || !read(h,item.address,a) || !read(h,item.address,b) || a!=b)return {};
  auto get=[&]<class T>(size_t at){T v{};std::memcpy(&v,a.data()+at,sizeof v);return v;};
  if(get.template operator()<int32_t>(0x18)!=item.type || get.template operator()<int32_t>(0x20)!=item.value ||
     a[0x24]!=0 || a[0x25]!=1 || get.template operator()<uint64_t>(0x28)!=h.main_base+inventory_offsets(h).pouch_string_vtable ||
     get.template operator()<uint64_t>(0x30)!=item.address+0x3C || get.template operator()<int32_t>(0x38)!=64 ||
     item.actor.size()>=64 || std::memcmp(a.data()+0x3C,item.actor.c_str(),item.actor.size()+1))return {};
  Consumable out;out.recipe=info->recipe;out.use=get.template operator()<int32_t>(0x1C);
  if(out.recipe ? (item.type!=8 || out.use!=8) : out.use!=10)return {};
  for(unsigned i=0;i<5;++i)out.cook[i]=get.template operator()<uint32_t>(0x80+i*4);
  out.health=info->health;out.frames=item.type==8 ? info->frames : 0;
  out.effect=item.type==8 ? info->effect : -1;out.level=item.type==8 ? info->level : 0;
  if(out.recipe) {
    const auto health=get.template operator()<int32_t>(0x80),seconds=get.template operator()<int32_t>(0x84),price=get.template operator()<int32_t>(0x88);
    const float effect=get.template operator()<float>(0x8C),level=get.template operator()<float>(0x90);
    if(health<0 || health>120 || seconds<0 || seconds>86400 || price<0 || price>999999 ||
       !std::isfinite(effect) || effect < -1 || effect>16 || std::trunc(effect)!=effect || !consume_effect(int(effect)) ||
       !std::isfinite(level) || level<0 || level>3000 || std::trunc(level)!=level)return {};
    if(health>0)out.health=health;
    out.effect=int(effect);out.level=int(level);out.frames=seconds*30;
    if((out.effect==2 && out.level>120) || (out.effect==15 && out.level>15) ||
       (out.effect!=2 && out.effect!=14 && out.effect!=15 && out.level>3))return {};
  }
  out.health_only=out.effect==-1 || out.effect==1;
  out.valid=true;return out;
}
} // namespace botw
