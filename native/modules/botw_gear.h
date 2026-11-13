// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_quick.h"
namespace botw {
struct GearSnapshot {
  bool valid{};
  QuickKind kind{};
  uint64_t screen{};
  int32_t initial{-1}, highlighted{-1};
  std::vector<Item> items;
  // Iteration 8 item 1: true only for the companion-built touch picker (never the native-mirrored
  // held wheel) on Weapon/Bow/Shield, when a synthetic "None" entry (empty actor, address 0) has
  // been inserted at items[0] by the caller. Items[] stays otherwise a normal native pouch slice;
  // this flag only tells the UI/action layer that slot 0 is special, not a real pouch instance.
  bool has_none{};
};
// Read native shortcut order, not a filtered pouch index. The cached pouch provides instance
// membership; every displayed instance and the menu fields are reread across the snapshot.
// This is a reader only. A valid snapshot does not authorize guest actions.
inline GearSnapshot gear(const EdenDsmodHostApi& h, const Snapshot& pouch) {
  GearSnapshot out;
  auto q=native_quick(h);
  const int type=q.kind==QuickKind::Weapon ? 0 : q.kind==QuickKind::Bow ? 1 :
                 q.kind==QuickKind::Shield ? 3 : q.kind==QuickKind::Arrow ? 2 : -1;
  UiSample ui{},after{};
  if (!q.valid || type<0 || !pouch.valid || pouch.items.size()>420 || !ui_sample(h,ui)) return out;
  struct Fields {
    uint64_t screen{};
    std::array<uint64_t,20> slots{};
    int32_t initial{},highlighted{},count{};
    uint8_t close_mode{};
    bool operator==(const Fields&) const = default;
  } a{},z{};
  auto fields=[&](Fields& f) {
    return ui.array<=UINT64_MAX-45*8 && read(h,ui.array+45*8,f.screen) &&
      f.screen%8==0 && f.screen<=UINT64_MAX-(0x38CD+shortcut_offsets(h)->screen_shift) &&
      read(h,f.screen+(0x37B0+shortcut_offsets(h)->screen_shift),f.slots) && read(h,f.screen+(0x3860+shortcut_offsets(h)->screen_shift),f.initial) &&
      read(h,f.screen+(0x3864+shortcut_offsets(h)->screen_shift),f.highlighted) && read(h,f.screen+(0x3868+shortcut_offsets(h)->screen_shift),f.count) &&
      read(h,f.screen+(0x38CC+shortcut_offsets(h)->screen_shift),f.close_mode) && f.close_mode==3 &&
      f.count>=0 && f.count<=(type==2 ? 6 : 20) && f.initial>=-1 && f.initial<f.count &&
      f.highlighted>=-1 && f.highlighted<f.count;
  };
  if (!fields(a)) return out;
  std::array<std::array<uint8_t,0x38>,20> bytes{};
  auto identity=[&](uint64_t address, const Item& item, std::array<uint8_t,0x38>& b) {
    if (!address || address%8 || !read(h,address,b)) return false;
    int32_t native_type{},value{};uint64_t name{};
    std::memcpy(&native_type,b.data()+0x18,4);
    std::memcpy(&value,b.data()+0x20,4);
    std::memcpy(&name,b.data()+0x30,8);
    if (native_type!=type || native_type!=item.type || value!=item.value ||
        b[0x24]!=uint8_t(item.equipped) || b[0x25]!=1 || item.actor.empty() ||
        item.actor.size()>=64 || name<0x10000 ||
        name>UINT64_MAX-(item.actor.size()+1)) return false;
    // Read exactly the expected string including its terminator; no adjacent string assumption.
    std::array<char,64> actor{};
    return h.is_mapped(h.userdata,name,item.actor.size()+1) &&
      h.read_memory(h.userdata,name,actor.data(),item.actor.size()+1) &&
      std::memcmp(actor.data(),item.actor.c_str(),item.actor.size()+1)==0;
  };
  for (int32_t i=0;i<a.count;++i) {
    const Item* item=nullptr;
    for (const auto& candidate:pouch.items)
      if (candidate.address==a.slots[i]) {
        if (item) return {}; // Ambiguous membership fails closed.
        item=&candidate;
      }
    for (int32_t j=0;j<i;++j) if (a.slots[j]==a.slots[i]) return {};
    if (!item || !identity(a.slots[i],*item,bytes[i])) return {};
    out.items.push_back(*item);
  }
  for (int32_t i=0;i<a.count;++i) {
    std::array<uint8_t,0x38> again{};
    if (!identity(a.slots[i],out.items[i],again) || again!=bytes[i]) return {};
  }
  if (!fields(z) || a!=z || !ui_sample(h,after) || after!=ui) return {};
  const auto last=native_quick(h);
  if (!last.valid || last.kind!=q.kind) return {};
  out.valid=true;out.kind=q.kind;out.screen=a.screen;
  out.initial=a.initial;out.highlighted=a.highlighted;
  return out;
}
} // namespace botw
