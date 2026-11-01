// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_hud.h"
#include <algorithm>
#include <cstring>
namespace ot0 {
namespace {
constexpr uint64_t AppRoot = 0x7cc8cc0, AppVtable = 0x6207610, BattleManager = 0x7cc5b78,
                   LayoutVtable = 0x6221a30, ControlVtable = 0x62281d0, RowVtable = 0x6232f28,
                   CommandVtable = 0x621cf90, ObjectWidgetVtable = 0x5d011e8;
constexpr uint64_t Opacity = 0x1cc, Matrix = 0x198, Translation = 0x1a8, TransformSet = 0x1b0;
constexpr int MaxRows = 32;
template <class T> bool Get(const ReadMemory &read, uint64_t at, T &out) {
  return at && read(at, &out, sizeof out);
}
bool Pointer(uint64_t p) { return p >= 0x1000000 && p < (1ull << 40) && p % 8 == 0; }
bool Object(const ReadMemory &read, uint64_t p, uint64_t vtable, int64_t delta) {
  uint64_t vt{};
  return Pointer(p) && Get(read, p, vt) &&
         (vt == vtable || (delta && vt == vtable + static_cast<uint64_t>(delta)));
}
// A UMG widget's cached Slate widget: TWeakPtr{object, controller} at UWidget+0x108 with a live
// shared reference, the SObjectWidget vtable and TSharedFromThis pointing back at itself.
bool OwnedWidget(const ReadMemory &read, uint64_t main, int64_t delta, uint64_t object,
                 uint64_t vtable, HudRow &out) {
  uint64_t widget{}, controller{}, self{};
  uint32_t shared{};
  if (!Object(read, object, main + vtable, delta) || !Get(read, object + 0x108, widget) ||
      !Get(read, object + 0x110, controller) || !Pointer(controller) ||
      !Get(read, controller + 8, shared) || shared == 0 ||
      !Object(read, widget, main + ObjectWidgetVtable, delta) || !Get(read, widget + 8, self) ||
      self != widget)
    return false;
  out = {object, widget, controller};
  return true;
}
bool InBattle(const ReadMemory &read, uint64_t main, int64_t delta) {
  uint64_t root{};
  uint8_t state{};
  return Get(read, main + AppRoot, root) && Object(read, root, main + AppVtable, delta) &&
         Get(read, root + 0x2f0, state) && state == 26;
}
bool StillValid(const ReadMemory &read, uint64_t main, int64_t delta, const HudRow &row,
                uint64_t vtable) {
  HudRow now;
  return OwnedWidget(read, main, delta, row.row, vtable, now) && now == row;
}
// Battle manager -> BattleLayoutWidget_C (+0x1678) -> BattlePlyControlPanel_C (+0x2f8).
uint64_t ControlPanel(const ReadMemory &read, uint64_t main, int64_t delta) {
  uint64_t bm{}, layout{}, control{};
  uint32_t flags{};
  if (!InBattle(read, main, delta) || !Get(read, main + BattleManager, bm) || !Pointer(bm) ||
      !Get(read, bm + 8, flags) || (flags & 0x40000000) != 0 ||
      !Get(read, bm + 0x1678, layout) || !Object(read, layout, main + LayoutVtable, delta) ||
      !Get(read, layout + 0x2f8, control) || !Object(read, control, main + ControlVtable, delta))
    return 0;
  return control;
}
} // namespace

std::vector<HudRow> FindPartyHudRows(const ReadMemory &read, uint64_t main, int64_t delta) {
  std::vector<HudRow> rows;
  uint64_t data{};
  int32_t count{}, max{};
  const auto control = ControlPanel(read, main, delta);
  if (!control || !Get(read, control + 0x328, data) || !Get(read, control + 0x330, count) ||
      !Get(read, control + 0x334, max) || count <= 0 || count > MaxRows || max < count ||
      !Pointer(data))
    return rows;
  for (int i = 0; i < count; ++i) {
    uint64_t row{};
    HudRow found;
    if (Get(read, data + 8ull * i, row) &&
        OwnedWidget(read, main, delta, row, RowVtable, found))
      rows.push_back(found);
  }
  return rows;
}

std::optional<HudRow> FindCommandPanel(const ReadMemory &read, uint64_t main, int64_t delta) {
  uint64_t panel{};
  HudRow found;
  const auto control = ControlPanel(read, main, delta);
  if (!control || !Get(read, control + 0x300, panel) ||
      !OwnedWidget(read, main, delta, panel, CommandVtable, found))
    return std::nullopt;
  return found;
}

void PartyHud::Update(const ReadMemory &read, const WriteF32 &write, uint64_t main, bool hide,
                      int64_t delta) {
  // Leaving battle (or title/load) puts the rows back so a later battle never starts with rows
  // this instance hid but no longer owns.
  if (!hide || !InBattle(read, main, delta)) {
    Restore(read, write, main, delta);
    return;
  }
  // A recycled or rebuilt row is forgotten, never written.
  std::erase_if(owned_,
                [&](const HudRow &r) { return !StillValid(read, main, delta, r, RowVtable); });
  for (const auto &row : FindPartyHudRows(read, main, delta)) {
    float opacity{};
    if (!Get(read, row.widget + Opacity, opacity))
      continue;
    const bool owned = std::find(owned_.begin(), owned_.end(), row) != owned_.end();
    if (opacity == 0.0f || (!owned && opacity != 1.0f))
      continue; // already hidden, or mid native fade: take it at the settled 1.0 only
    if (!write(row.widget + Opacity, opacity, 0.0f)) {
      ++failures_;
      continue;
    }
    if (!owned)
      owned_.push_back(row);
  }
}

void PartyHud::Restore(const ReadMemory &read, const WriteF32 &write, uint64_t main,
                       int64_t delta) {
  std::erase_if(owned_, [&](const HudRow &r) {
    float opacity{};
    if (!StillValid(read, main, delta, r, RowVtable) || !Get(read, r.widget + Opacity, opacity))
      return true; // gone: nothing of ours to put back
    if (opacity != 0.0f)
      return true; // the game changed it since: not ours any more
    if (write(r.widget + Opacity, 0.0f, 1.0f))
      return true;
    ++failures_;
    return false; // retry on the next call
  });
}

void CommandShift::Update(const ReadMemory &read, const WriteBlock &write, uint64_t main,
                          bool shift, int64_t delta) {
  if (!shift || !InBattle(read, main, delta)) {
    Restore(read, write, main, delta);
    return;
  }
  // The parent BattlePlyControlPanel's Slate widget: the game never sets or animates its render
  // transform (live 2026-10-31), so one write keeps the whole command group raised. Shifting the
  // command panel itself lost to the game's per-turn slide animation for a frame each time.
  uint64_t control = ControlPanel(read, main, delta);
  HudRow panel;
  if (!control || !OwnedWidget(read, main, delta, control, ControlVtable, panel)) {
    active_ = false;
    return;
  }
  if (!(panel == panel_)) { // new battle / rebuilt widget: nothing of ours in it
    panel_ = panel;
    active_ = false;
  }
  Fields now{};
  if (!Get(read, panel_.widget + Matrix, now))
    return;
  Fields ours = Ours();
  ours.set = (now.set & ~0xffu) | 1u; // keep the padding bytes of the flag word
  if (active_ && now == ours)
    return; // still in place
  if (active_) { // something else rewrote it: never fight the game, just stop owning it
    active_ = false;
    ++failures_;
    return;
  }
  if (now.set & 0xff)
    return; // the game uses this transform itself: leave it alone
  if (!WriteFields(write, panel_.widget, now, ours)) {
    ++failures_;
    return;
  }
  base_ = now;
  active_ = true;
}

void CommandShift::Restore(const ReadMemory &read, const WriteBlock &write, uint64_t main,
                           int64_t delta) {
  if (!active_)
    return;
  Fields now{};
  if (StillValid(read, main, delta, panel_, ControlVtable) &&
      Get(read, panel_.widget + Matrix, now) && now == Ours() &&
      !WriteFields(write, panel_.widget, now, base_)) {
    ++failures_;
    return; // retry
  }
  active_ = false;
}

CommandShift::Fields CommandShift::Ours() const {
  Fields f{};
  f.m[0] = 1, f.m[3] = 1, f.t[0] = dx_, f.t[1] = dy_, f.set = 1;
  return f;
}

bool CommandShift::WriteFields(const WriteBlock &write, uint64_t widget, const Fields &from,
                               const Fields &to) {
  return write(widget + Matrix, &from, &to, sizeof(Fields));
}
} // namespace ot0
