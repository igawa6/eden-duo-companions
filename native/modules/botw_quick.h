// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_context.h"
#include "botw_shortcut_offsets.h"
namespace botw {
enum class QuickKind { None, Weapon, Pending, Bow, Shield, Rune, Arrow };
struct NativeQuick {
  bool valid{};
  QuickKind kind{};
};
inline NativeQuick native_quick(const EdenDsmodHostApi &h) {
  if(!shortcut_supported(h))return {};
  UiSample a{}, b{};
  if (!ui_sample(h,a)) return {};
  struct Fields {
    uint64_t screen{}, vtable{}, state{};
    uint32_t category{}, held{};
    int32_t selector{}, selector_copy{};
    bool operator==(const Fields&) const = default;
  } x{}, y{};
  auto fields = [&](Fields& f) {
    return a.array <= UINT64_MAX - 45*8 &&
      read(h,a.array+45*8,f.screen) && f.screen%8==0 &&
      f.screen <= UINT64_MAX-(0x3728+shortcut_offsets(h)->screen_shift) && read(h,f.screen,f.vtable) &&
      f.vtable==h.main_base+shortcut_offsets(h)->screen_vtable && read(h,f.screen+(0x150+shortcut_offsets(h)->screen_shift),f.state) &&
      read(h,f.screen+(0x3720+shortcut_offsets(h)->screen_shift),f.selector) && read(h,f.screen+(0x3724+shortcut_offsets(h)->screen_shift),f.selector_copy) &&
      read(h,a.manager+0x64C24,f.category) && read(h,a.manager+0x64C58,f.held);
  };
  if (!fields(x) || !fields(y) || x!=y || !ui_sample(h,b) || a!=b) return {};
  const uint32_t held_direction=x.held & 0xD0000;
  // A category transition may clear its selector pair, but does not authorize
  // an ambiguous combination of directions. Match the guest callback's guard.
  if (held_direction && held_direction!=0x10000 && held_direction!=0x40000 &&
      held_direction!=0x80000) return {};
  if (a.flags==0x1E09000000ULL && x.category==0 &&
      x.state==h.main_base+shortcut_offsets(h)->world_state)
    return {true,held_direction ? QuickKind::Pending : QuickKind::None};
  // ScreenMainShortCut's own type distinguishes contextual bow/arrow modes.
  // ZR is not authoritative: the bow can remain drawn after ZR is released.
  QuickKind kind=QuickKind::None;
  uint32_t direction=0;
  if (x.selector==x.selector_copy) switch (x.selector) {
    case 0: kind=QuickKind::Weapon; direction=0x80000; break;
    case 1: kind=QuickKind::Shield; direction=0x40000; break;
    case 2: kind=QuickKind::Bow; direction=0x80000; break;
    case 3: kind=QuickKind::Arrow; direction=0x40000; break;
    case 4: kind=QuickKind::Rune; direction=0x10000; break;
  }
  if (direction && held_direction==direction && x.category==3) {
    if (a.flags==0x1E09000004ULL && x.state==h.main_base+shortcut_offsets(h)->menu_state)
      return {true,kind};
    if ((a.flags==0x1E09000000ULL || a.flags==0x1E09000004ULL) &&
        (x.state==h.main_base+shortcut_offsets(h)->world_state || x.state==h.main_base+shortcut_offsets(h)->menu_state ||
         x.state==h.main_base+shortcut_offsets(h)->end_state))
      return {true,QuickKind::Pending};
  }
  // The native selector clears/switches its type pair while entering another
  // category. Keep a previously armed hold through this bounded transition,
  // without displaying stale items; unknown values still fail closed.
  if (x.category==3 && held_direction &&
      x.selector>=-1 && x.selector<=4 && x.selector_copy>=-1 && x.selector_copy<=4 &&
      (x.selector!=x.selector_copy || x.selector==-1) &&
      (a.flags==0x1E09000000ULL || a.flags==0x1E09000004ULL) &&
      (x.state==h.main_base+shortcut_offsets(h)->world_state || x.state==h.main_base+shortcut_offsets(h)->menu_state ||
       x.state==h.main_base+shortcut_offsets(h)->end_state)) return {true,QuickKind::Pending};
  return {};
}
struct QuickOverlay {
  bool armed{};
  QuickKind shown{};
  void update(bool ready, NativeQuick q) {
    shown=QuickKind::None;
    if (!ready || !q.valid) { armed=false; return; }
    if (q.kind==QuickKind::Pending) return;
    if (q.kind==QuickKind::None) { armed=true; return; }
    if (armed) shown=q.kind;
  }
};
} // namespace botw
