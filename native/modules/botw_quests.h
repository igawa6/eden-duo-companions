// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include "botw_hud.h"
#include "botw_quest_offsets.h"
#include "botw_inventory.h"
namespace botw {
inline CurrencySnapshot quest_destination(const EdenDsmodHostApi& h) {
  UiSample ui{},after{};
  if (!quests_supported(h) || !ui_sample(h,ui) || !hud_context(ui)) return {};
  struct Roots {
    uint64_t manager{},vtable{},holder{},bank{},array{};
    int32_t count{},capacity{};
    bool operator==(const Roots&) const = default;
  } a{},z{};
  auto roots=[&](Roots& r) {
    return read(h,h.main_base+inventory_offsets(h)->gamedata_root,r.manager) && r.manager%8==0 &&
      r.manager<=UINT64_MAX-0xBE8 && read(h,r.manager,r.vtable) &&
      r.vtable==h.main_base+inventory_offsets(h)->gamedata_vtable && read(h,r.manager+0xBE0,r.holder) && r.holder%8==0 &&
      read(h,r.holder,r.bank) && r.bank%8==0 && r.bank<=UINT64_MAX-0x28 &&
      read(h,r.bank+0x18,r.count) && read(h,r.bank+0x1C,r.capacity) &&
      r.count>0 && r.count<=65536 && r.capacity>=r.count && r.capacity<=65536 &&
      read(h,r.bank+0x20,r.array) && r.array%8==0 && r.array<=UINT64_MAX-uint64_t(r.count)*8;
  };
  if (h.main_size<inventory_offsets(h)->gamedata_root+8 || h.main_base>UINT64_MAX-(inventory_offsets(h)->gamedata_root+8) || !roots(a)) return {};
  struct Flag {
    uint64_t vtable{};
    uint8_t type{},properties{};
    std::array<uint8_t,2> pad{};
    int32_t initial{},minimum{},maximum{},value{};
    uint32_t hash{};
    uint64_t debug{};
    bool operator==(const Flag&) const = default;
  } f{},again{};
  static_assert(sizeof(Flag)==40);
  auto flag=[&](int32_t i,uint64_t& p,Flag& v) {
    return i>=0 && i<a.count && read(h,a.array+uint64_t(i)*8,p) && p%8==0 &&
      read(h,p,v) && v.vtable==h.main_base+inventory_offsets(h)->int_flag_vtable && v.type==1;
  };
  int32_t lo=0,hi=a.count;uint64_t p{},p2{};
  for (unsigned step=0;lo<hi && step<17;++step) {
    const auto mid=lo+(hi-lo)/2;
    if (!flag(mid,p,f)) return {};
    if (f.hash<0x5ED42D75u) lo=mid+1;else hi=mid;
  }
  if (lo!=hi || !flag(lo,p,f) || f.hash!=0x5ED42D75u ||
      f.minimum!=-1 || f.maximum!=65535 || f.value<-1 || f.value>f.maximum ||
      !flag(lo,p2,again) || p2!=p || again!=f || !roots(z) || z!=a ||
      !ui_sample(h,after) || after!=ui) return {};
  return {true,f.value};
}
struct Quest {
  uint64_t address{};
  int32_t index{},category{},state{};
  uint32_t dlc{};
  std::string name,message;
  // Native Quest::mLocation (+0x38): the StaticMsg/LocationMarker label the Adventure Log
  // prints under the title ("Great Plateau"). Empty when the quest has none.
  std::string place;
  bool active{},revealed{};
};
struct QuestSnapshot {
  bool valid{};
  std::vector<Quest> entries;
};
inline bool quest_string(const EdenDsmodHostApi& h,uint64_t p,std::string& out,bool empty=false) {
  out.clear();
  if(p>UINT64_MAX-64)return false;
  for(unsigned i=0;i<64;++i) {
    char c{};if(!read(h,p+i,c))return false;
    if(!c)return empty || !out.empty();
    if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return false;
    out.push_back(c);
  }
  return false;
}
inline QuestSnapshot quests(const EdenDsmodHostApi& h,bool include_unstarted=false) {
  UiSample ui{},after{};
  if(!quests_supported(h)||!ui_sample(h,ui)||!hud_context(ui)||h.main_size<quest_offsets(h)->root+8||h.main_base>UINT64_MAX-(quest_offsets(h)->root+8))return {};
  bool dlc2{},dlc3{};
  if(!rune_upgrade(h,0x7609DE2C,dlc2)||!rune_upgrade(h,0x010EEEBA,dlc3))return {};
  const auto destination=quest_destination(h);if(!destination.valid)return {};
  struct Roots {
    uint64_t manager{},vtable{},array{};int32_t count{},capacity{};
    bool operator==(const Roots&) const = default;
  } a{},z{};
  auto roots=[&](Roots& s) {
    return read(h,h.main_base+quest_offsets(h)->root,s.manager)&&s.manager%8==0&&s.manager<=UINT64_MAX-0xF0&&
      read(h,s.manager,s.vtable)&&s.vtable==h.main_base+quest_offsets(h)->manager_vtable&&
      read(h,s.manager+0xB8,s.count)&&s.count>0&&s.count<=512&&
      read(h,s.manager+0xBC,s.capacity)&&s.capacity>=s.count&&s.capacity<=512&&
      read(h,s.manager+0xC0,s.array)&&s.array%8==0&&s.array<=UINT64_MAX-uint64_t(s.count)*8;
  };
  if(!roots(a))return {};
  QuestSnapshot out;
  std::vector<uint64_t> seen;
  for(int32_t i=0;i<a.count;++i) {
    uint64_t p{},p2{};std::array<uint8_t,0x158> raw{},again{};
    if(!read(h,a.array+uint64_t(i)*8,p)||p%8||p>UINT64_MAX-raw.size()||!read(h,p,raw))return {};
    if(std::find(seen.begin(),seen.end(),p)!=seen.end())return {};
    seen.push_back(p);
    auto field=[&]<class T>(unsigned offset){T v{};std::memcpy(&v,raw.data()+offset,sizeof(v));return v;};
    if(field.operator()<uint64_t>(0)!=h.main_base+quest_offsets(h)->quest_vtable)return {};
    const auto state=field.operator()<int32_t>(0xC);
    const auto journal=raw[0x30];const auto aoc=field.operator()<uint32_t>(0xD8);
    if(state<0||state>2||journal>1||(aoc&~7u))return {};
    const auto current_step=field.operator()<int32_t>(0x140);
    if(state>0&&journal&&current_step<0)return {};
    // Native registry includes repeatable minigames, undiscovered quests and DLC entries.
    // Only journal entries accepted by the running game's DLC availability are exposed.
    // A quest is revealed only once it has advanced past step 0, for EVERY type: iteration 14
    // item 7 dumped an early save (Great Plateau start) where all
    // 15 base+DLC Main quests sit at state1/step0 with the journal byte set, and the native
    // Adventure Log listed none of them (Main/Shrine/Side tabs greyed). The earlier step-0 Main
    // exception showed all 15 there.
    const bool revealed=state>0&&current_step>0;
    const bool visible=journal&&(revealed||include_unstarted)&&(!(aoc&2u)||dlc2)&&(!(aoc&4u)||dlc3);
    if(visible) {
      Quest q;q.address=p;q.index=i;q.state=state;q.dlc=aoc;q.active=revealed&&i==destination.value;q.revealed=revealed;
      std::string type;
      if(!quest_string(h,p+0x54,q.name)||!quest_string(h,field.operator()<uint64_t>(0xC0),type))return {};
      q.category=type=="Main"?0:type=="Sub"?1:type=="Mini"?2:-1;
      if(q.category<0)return {};
      // Location is presentation only: an unreadable or unusual label leaves the row without a
      // place line instead of invalidating the whole journal.
      if(const auto location=field.operator()<uint64_t>(0x38);location&&!quest_string(h,location,q.place,true))q.place.clear();
      std::string type_again;
      if(!quest_string(h,field.operator()<uint64_t>(0xC0),type_again)||type_again!=type)return {};
      // Unstarted tracker rows expose identity and state only; unrevealed descriptions
      // may not have an attached current-step message. Existing journal behavior is unchanged.
      if(revealed) {
      const auto count=field.operator()<int32_t>(0xA0),cap=field.operator()<int32_t>(0xA4),step=field.operator()<int32_t>(0x140);
      const auto array=field.operator()<uint64_t>(0xA8);
      if(count<2||count>64||cap<count||cap>64||step<0||step>=count||array%8||array>UINT64_MAX-uint64_t(count)*8)return {};
      const auto selected=std::max(0,step-1);uint64_t s{},s2{},message{},message2{},vt{};
      std::string message_again;
      if(!read(h,array+uint64_t(selected)*8,s)||s%8||s>UINT64_MAX-0x88||!read(h,s,vt)||
        vt!=h.main_base+quest_offsets(h)->step_vtable||!read(h,s+0x48,message)||!quest_string(h,message,q.message)||
        !quest_string(h,message,message_again)||message_again!=q.message||
        !read(h,s+0x48,message2)||message2!=message||!read(h,array+uint64_t(selected)*8,s2)||s2!=s)return {};
      }
      out.entries.push_back(std::move(q));
    }
    if(!read(h,p,again)||raw!=again||!read(h,a.array+uint64_t(i)*8,p2)||p2!=p)return {};
  }
  bool d2{},d3{};const auto last=quest_destination(h);
  if(!roots(z)||a!=z||!rune_upgrade(h,0x7609DE2C,d2)||!rune_upgrade(h,0x010EEEBA,d3)||dlc2!=d2||dlc3!=d3||
    !last.valid||last.value!=destination.value||!ui_sample(h,after)||ui!=after)return {};
  out.valid=true;return out;
}
} // namespace botw
