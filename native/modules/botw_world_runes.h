// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_runes.h"
namespace botw {
inline constexpr std::array<uint32_t,8> RuneUnlockHashes={0x191BCCF9,0x191BCCF9,0x795E7BBC,0x7504085D,0x5992B256,0xF7DD3E03,0xC9328299,0x89EBCAE4};
struct WorldRunes {
 bool valid{};RuneSnapshot list;
 uint64_t state{},screen{},game_data{},dlc{},holder{},bank{},flags{};int32_t flag_count{};int32_t current{},screen_current{};uint32_t dlc_version{};uint8_t permission{};
 std::array<RuneFlag,8> unlock;RuneFlag option,bombs,stasis;
 bool operator==(const WorldRunes&) const = default;
};
// Mirror the exact native selector's availability rules, including DLC version and
// the amiibo option, instead of treating an unlock flag alone as availability.
inline WorldRunes world_runes(const EdenDsmodHostApi& h) {
 UiSample ui{},after{};const auto q=native_quick(h);
 if(!ui_sample(h,ui) || ui.flags!=0x1E09000000ULL || !q.valid || q.kind!=QuickKind::None)return {};
 struct Roots {
  uint64_t state{},screen{},game_data{},dlc{},holder{},bank{},flags{},state_vt{},screen_vt{},game_vt{},dlc_vt{};
  int32_t current{},screen_current{},count{},capacity{};uint32_t dlc_version{};uint8_t permission{};
  bool operator==(const Roots&) const = default;
 } a{},z{};
 auto roots=[&](Roots& r) {
  return read(h,h.main_base+ready_offsets(h).rune_root,r.state) && r.state && ! (r.state%8) && r.state<=UINT64_MAX-0x244 &&
   read(h,r.state,r.state_vt) && r.state_vt==h.main_base+ready_offsets(h).rune_vtable && read(h,r.state+0x240,r.current) && r.current>=-1 && r.current<=7 &&
   read(h,ui.array+45*8,r.screen) && r.screen && !(r.screen%8) && r.screen<=UINT64_MAX-(0x36EC+shortcut_offsets(h).screen_shift) &&
   read(h,r.screen,r.screen_vt) && r.screen_vt==h.main_base+shortcut_offsets(h).screen_vtable && read(h,r.screen+(0x36E8+shortcut_offsets(h).screen_shift),r.screen_current) && r.screen_current>=-1 && r.screen_current<=7 &&
   read(h,h.main_base+inventory_offsets(h).gamedata_root,r.game_data) && r.game_data && !(r.game_data%8) && r.game_data<=UINT64_MAX-0xBE9 &&
   read(h,r.game_data,r.game_vt) && r.game_vt==h.main_base+inventory_offsets(h).gamedata_vtable && read(h,r.game_data+0xBE8,r.permission) && r.permission<=1 &&
   read(h,r.game_data+0xBE0,r.holder) && r.holder && !(r.holder%8) && read(h,r.holder,r.bank) && r.bank && !(r.bank%8) && r.bank<=UINT64_MAX-24 &&
   read(h,r.bank+8,r.count) && read(h,r.bank+12,r.capacity) && r.count>0 && r.count<=r.capacity && r.capacity<=65536 &&
   read(h,r.bank+16,r.flags) && r.flags && !(r.flags%8) && r.flags<=UINT64_MAX-uint64_t(r.count)*8 &&
   read(h,h.main_base+quest_offsets(h).dlc_root,r.dlc) && r.dlc && !(r.dlc%8) && r.dlc<=UINT64_MAX-(quest_offsets(h).dlc_version+4) &&
   read(h,r.dlc,r.dlc_vt) && r.dlc_vt==h.main_base+quest_offsets(h).dlc_vtable && read(h,r.dlc+quest_offsets(h).dlc_version,r.dlc_version) && r.dlc_version<=0xFFFF;
 };
 if(!roots(a))return {};
 WorldRunes out;
 for(unsigned i=0;i<8;++i){out.unlock[i]=rune_flag(h,RuneUnlockHashes[i]);if(!out.unlock[i].valid)return {};}
 out.option=rune_flag(h,0x511872C2);out.bombs=rune_flag(h,0x604C3BAE);out.stasis=rune_flag(h,0x9CA3D1C1);
 if(!out.option.valid || !out.bombs.valid || !out.stasis.valid)return {};
 const auto enabled=[&](const RuneFlag& f){return f.value && (!a.permission || (f.properties&1));};
 out.list.selected=-1;
 for(unsigned i=0;i<8;++i)if(enabled(out.unlock[i]) && (i!=6 || a.dlc_version>=0x300) && (i!=7 || enabled(out.option))) {
  if(int(i)==a.current)out.list.selected=out.list.count;
  out.list.ids[size_t(out.list.count++)]=int(i);
 }
 for(unsigned i=0;i<8;++i)if(rune_flag(h,RuneUnlockHashes[i])!=out.unlock[i])return {};
 if(rune_flag(h,0x511872C2)!=out.option || rune_flag(h,0x604C3BAE)!=out.bombs || rune_flag(h,0x9CA3D1C1)!=out.stasis ||
    !roots(z) || a!=z || !ui_sample(h,after) || after!=ui)return {};
 const auto last=native_quick(h);if(!last.valid || last.kind!=QuickKind::None)return {};
 out.list.valid=true;out.list.bombs_plus=enabled(out.bombs);out.list.stasis_plus=enabled(out.stasis);
 out.permission=a.permission;out.holder=a.holder;out.bank=a.bank;out.flags=a.flags;out.flag_count=a.count;
 out.state=a.state;out.screen=a.screen;out.game_data=a.game_data;out.dlc=a.dlc;out.current=a.current;out.screen_current=a.screen_current;out.dlc_version=a.dlc_version;out.valid=true;return out;
}
} // namespace botw
