// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Top-screen battle party HUD hide, Switch 1.0.5 (live-confirmed 2026-10-25, research
// analysis/20261025-battle-route "Live batch 5/6"). Memory-only, no guest calls:
// battle manager [main+0x7cc5b78] +0x1678 BattleLayoutWidget_C, +0x2f8
// BattlePlyControlPanel_C, +0x328 TArray<BattlePStaPanel_C*> (rows 0..3 = party);
// each row's cached Slate widget (UWidget+0x108, SObjectWidget) RenderOpacity f32 at
// +0x1cc. Slate re-reads the opacity when it paints; visibility writes are not seen
// (invalidation cache). Rows are taken only at the native 1.0 and only ever put back
// to 1.0 while every link still points to the same objects.
#pragma once
#include "ot0_reader.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
namespace ot0 {
// Expect-checked 4-byte write: stores `value` only if the guest still holds `expect`.
using WriteF32 = std::function<bool(uint64_t address, float expect, float value)>;
// One atomic expect-checked block, including the transform flag and padding.
using WriteBlock = std::function<bool(uint64_t, const void*, const void*, std::size_t)>;
struct HudRow {
  uint64_t row{}, widget{}, controller{};
  bool operator==(const HudRow &) const = default;
};
// Rows of the live battle party HUD: empty unless EAPP_STATE is 26 (battle) and every link
// validates. `delta` is the host's __relocation_delta: under NCE a vtable may read as
// main+offset+delta (same rule as the Odyssey companion).
std::vector<HudRow> FindPartyHudRows(const ReadMemory &read, uint64_t main, int64_t delta = 0);
// The battle command panel (same validation as the rows); nullopt outside battle.
std::optional<HudRow> FindCommandPanel(const ReadMemory &read, uint64_t main, int64_t delta = 0);
// The parent control panel uses an otherwise unset render transform. Apply and restore
// its complete matrix/translation/flag as one atomic host write.
class CommandShift {
public:
  CommandShift(float dx, float dy) : dx_(dx), dy_(dy) {}
  void Update(const ReadMemory &read, const WriteBlock &write, uint64_t main, bool shift,
              int64_t delta = 0);
  void Restore(const ReadMemory &read, const WriteBlock &write, uint64_t main, int64_t delta = 0);
  bool Shifted() const { return active_; }
  int Failures() const { return failures_; }

private:
  // SWidget render transform: 2x2 matrix +0x198, translation +0x1a8, TOptional set word +0x1b0.
  struct Fields {
    float m[4]{}, t[2]{};
    uint32_t set{};
    bool operator==(const Fields &o) const {
      return std::equal(m, m + 4, o.m) && t[0] == o.t[0] && t[1] == o.t[1] && (set & 0xff) == (o.set & 0xff);
    }
  };
  static_assert(sizeof(Fields) == 0x1c);
  Fields Ours() const;
  static bool WriteFields(const WriteBlock &write, uint64_t widget, const Fields &from, const Fields &to);
  float dx_, dy_;
  HudRow panel_{};      // control panel, its Slate widget and weak-pointer controller
  Fields base_{};
  bool active_{};
  int failures_{};
};
class PartyHud {
public:
  // hide: setting on and the companion screen present. Outside battle state 26 this restores.
  void Update(const ReadMemory &read, const WriteF32 &write, uint64_t main, bool hide,
              int64_t delta = 0);
  // Puts back every row this instance hid that still validates.
  void Restore(const ReadMemory &read, const WriteF32 &write, uint64_t main, int64_t delta = 0);
  int Hidden() const { return static_cast<int>(owned_.size()); }
  int Failures() const { return failures_; }

private:
  std::vector<HudRow> owned_;
  int failures_{};
};
} // namespace ot0
