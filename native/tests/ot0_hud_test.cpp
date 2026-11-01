// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic fixture mirroring the live 2026-10-25 Aristocrat fight: 18 BattlePStaPanel rows,
// the first 4 showing the party (research analysis/20261025-battle-route "Live batch 6").
#include "../modules/ot0_hud.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <array>
#include <map>
namespace {
struct Memory {
  std::map<uint64_t, uint8_t> bytes;
  int writes{};
  template <class T> void put(uint64_t p, const T &t) {
    const auto *b = reinterpret_cast<const uint8_t *>(&t);
    for (size_t i = 0; i < sizeof(T); ++i) bytes[p + i] = b[i];
  }
  bool read(uint64_t p, void *dest, size_t n) {
    auto *b = static_cast<uint8_t *>(dest);
    for (size_t i = 0; i < n; ++i) {
      auto it = bytes.find(p + i);
      if (it == bytes.end()) return false;
      b[i] = it->second;
    }
    return true;
  }
  float f(uint64_t p) { float v{}; assert(read(p, &v, 4)); return v; }
};
constexpr uint64_t M = 0x80000000, root = 0x1400000000, bm = 0x1400010000, layout = 0x1400020000,
                   control = 0x1400030000, data = 0x1400040000, rows = 0x1400100000,
                   widgets = 0x1400200000, ctrls = 0x1400300000;
constexpr uint64_t cmd = 0x1400400000, cmdw = 0x1400410000, cmdc = 0x1400420000;
constexpr uint64_t ctlw = 0x1400500000, ctlc = 0x1400510000;
constexpr int N = 18;
uint64_t Row(int i) { return rows + 0x1000ull * i; }
uint64_t Widget(int i) { return widgets + 0x1000ull * i; }
Memory Fixture() {
  Memory m;
  m.put(M + 0x7cc8cc0, root); m.put(root, M + 0x6207610); m.put(root + 0x2f0, uint8_t(26));
  m.put(M + 0x7cc5b78, bm); m.put(bm + 8, uint32_t(0x8)); m.put(bm + 0x1678, layout);
  m.put(layout, M + 0x6221a30); m.put(layout + 0x2f8, control);
  m.put(control, M + 0x62281d0); m.put(control + 0x328, data);
  m.put(control + 0x330, int32_t(N)); m.put(control + 0x334, int32_t(24));
  m.put(control + 0x300, cmd); m.put(cmd, M + 0x621cf90); m.put(cmd + 0x108, cmdw);
  m.put(cmd + 0x110, cmdc); m.put(cmdc + 8, uint32_t(1));
  m.put(cmdw, M + 0x5d011e8); m.put(cmdw + 8, cmdw);
  m.put(cmdw + 0x198, std::array<float, 6>{1, 0, 0, 1, -32, 0}); m.put(cmdw + 0x1b0, uint8_t(1));
  // The control panel's own Slate widget (render transform unset, as live).
  m.put(control + 0x108, ctlw); m.put(control + 0x110, ctlc); m.put(ctlc + 8, uint32_t(1));
  m.put(ctlw, M + 0x5d011e8); m.put(ctlw + 8, ctlw);
  m.put(ctlw + 0x198, std::array<uint32_t, 7>{}); // matrix, translation, set flag word: all zero
  for (int i = 0; i < N; ++i) {
    m.put(data + 8ull * i, Row(i));
    m.put(Row(i), M + 0x6232f28); m.put(Row(i) + 0x108, Widget(i));
    m.put(Row(i) + 0x110, ctrls + 0x10ull * i); m.put(ctrls + 0x10ull * i + 8, uint32_t(1));
    m.put(Widget(i), M + 0x5d011e8); m.put(Widget(i) + 8, Widget(i));
    m.put(Widget(i) + 0x1cc, 1.0f);
  }
  return m;
}
struct Bound {
  Memory &m;
  ot0::ReadMemory read() { return [this](uint64_t p, void *d, size_t n) { return m.read(p, d, n); }; }
  ot0::WriteBlock block() {
    return [this](uint64_t p, const void* expect, const void* value, size_t n) {
      std::vector<uint8_t> now(n);
      if (!m.read(p, now.data(), n) || std::memcmp(now.data(), expect, n)) return false;
      const auto* bytes = static_cast<const uint8_t*>(value);
      for (size_t i=0; i<n; ++i) m.bytes[p+i]=bytes[i];
      ++m.writes;
      return true;
    };
  }
  ot0::WriteF32 write() {
    return [this](uint64_t p, float expect, float value) {
      float now{};
      if (!m.read(p, &now, 4) || now != expect) return false;
      m.put(p, value); ++m.writes; return true;
    };
  }
};
} // namespace
int main() {
  { // all rows found; hide, hold, restore
    auto m = Fixture(); Bound b{m};
    assert(ot0::FindPartyHudRows(b.read(), M).size() == N);
    ot0::PartyHud hud;
    hud.Update(b.read(), b.write(), M, true);
    assert(hud.Hidden() == N && m.writes == N);
    for (int i = 0; i < N; ++i) assert(m.f(Widget(i) + 0x1cc) == 0.0f);
    hud.Update(b.read(), b.write(), M, true); // nothing to re-assert
    assert(m.writes == N);
    m.put(Widget(2) + 0x1cc, 1.0f); // the game shows a row again: re-hidden
    hud.Update(b.read(), b.write(), M, true);
    assert(m.f(Widget(2) + 0x1cc) == 0.0f && m.writes == N + 1);
    hud.Update(b.read(), b.write(), M, false);
    assert(hud.Hidden() == 0);
    for (int i = 0; i < N; ++i) assert(m.f(Widget(i) + 0x1cc) == 1.0f);
  }
  { // mid native fade is never taken; a recycled row is forgotten, not restored
    auto m = Fixture(); Bound b{m};
    m.put(Widget(1) + 0x1cc, 0.4f);
    ot0::PartyHud hud;
    hud.Update(b.read(), b.write(), M, true);
    assert(hud.Hidden() == N - 1 && m.f(Widget(1) + 0x1cc) == 0.4f);
    m.put(Widget(3), M + 0x1234); // row 3's Slate widget replaced
    hud.Restore(b.read(), b.write(), M);
    assert(hud.Hidden() == 0 && m.f(Widget(3) + 0x1cc) == 0.0f && m.f(Widget(0) + 0x1cc) == 1.0f);
  }
  { // game changed an owned row's opacity: not ours to restore
    auto m = Fixture(); Bound b{m};
    ot0::PartyHud hud;
    hud.Update(b.read(), b.write(), M, true);
    m.put(Widget(0) + 0x1cc, 0.5f);
    hud.Restore(b.read(), b.write(), M);
    assert(m.f(Widget(0) + 0x1cc) == 0.5f && m.f(Widget(1) + 0x1cc) == 1.0f);
  }
  { // rejections: manager flag, wrong vtables, dead weak pointer, bad count
    auto m = Fixture(); Bound b{m};
    m.put(bm + 8, uint32_t(0x40000008));
    assert(ot0::FindPartyHudRows(b.read(), M).empty());
    m = Fixture(); m.put(layout, M + 1); assert(ot0::FindPartyHudRows(b.read(), M).empty());
    m = Fixture(); m.put(control + 0x330, int32_t(40)); assert(ot0::FindPartyHudRows(b.read(), M).empty());
    m = Fixture(); m.put(ctrls + 8, uint32_t(0)); m.put(Widget(5) + 8, uint64_t(0));
    assert(ot0::FindPartyHudRows(b.read(), M).size() == N - 2);
    m = Fixture(); m.bytes.erase(M + 0x7cc5b78); assert(ot0::FindPartyHudRows(b.read(), M).empty());
    m = Fixture(); m.put(root + 0x2f0, uint8_t(23)); assert(ot0::FindPartyHudRows(b.read(), M).empty());
  }
  { // NCE relocation delta: shifted vtables accepted only with the host's delta; leaving battle
    // (state 23) restores what was hidden
    auto m = Fixture(); Bound b{m};
    constexpr int64_t D = 0x1000;
    for (uint64_t p : {root, layout, control}) { uint64_t vt{}; m.read(p, &vt, 8); m.put(p, vt + D); }
    for (int i = 0; i < N; ++i) { m.put(Row(i), M + 0x6232f28 + D); m.put(Widget(i), M + 0x5d011e8 + D); }
    assert(ot0::FindPartyHudRows(b.read(), M).empty());
    assert(ot0::FindPartyHudRows(b.read(), M, D).size() == N);
    ot0::PartyHud hud;
    hud.Update(b.read(), b.write(), M, true, D);
    assert(hud.Hidden() == N);
    m.put(root + 0x2f0, uint8_t(23));
    hud.Update(b.read(), b.write(), M, true, D); // battle over: everything put back
    assert(hud.Hidden() == 0 && m.f(Widget(0) + 0x1cc) == 1.0f && m.f(Widget(17) + 0x1cc) == 1.0f);
  }
  { // command group shift: the parent control panel's unused render transform, set once
    auto m = Fixture(); Bound b{m};
    ot0::CommandShift s(112, -368);
    s.Update(b.read(), b.block(), M, true);
    assert(s.Shifted() && m.f(ctlw + 0x198) == 1.0f && m.f(ctlw + 0x1a4) == 1.0f &&
           m.f(ctlw + 0x1a8) == 112.0f && m.f(ctlw + 0x1ac) == -368.0f);
    uint8_t set{}; m.read(ctlw + 0x1b0, &set, 1); assert(set == 1);
    assert(m.f(cmdw + 0x1a8) == -32.0f); // the command panel's own (animated) transform untouched
    const int w = m.writes;
    m.put(cmdw + 0x1a8, -10.0f); // a frame of the native slide: nothing to re-apply
    s.Update(b.read(), b.block(), M, true);
    assert(m.writes == w && s.Shifted());
    s.Update(b.read(), b.block(), M, false); // setting off: the original (unset) transform back
    m.read(ctlw + 0x1b0, &set, 1);
    assert(!s.Shifted() && set == 0 && m.f(ctlw + 0x1a8) == 0.0f && m.f(ctlw + 0x198) == 0.0f);
    s.Update(b.read(), b.block(), M, true);
    m.put(root + 0x2f0, uint8_t(23)); // battle over
    s.Update(b.read(), b.block(), M, true);
    m.read(ctlw + 0x1b0, &set, 1);
    assert(!s.Shifted() && set == 0);
  }
  { // never takes over a transform the game set itself, never writes a released widget
    auto m = Fixture(); Bound b{m};
    m.put(ctlw + 0x1b0, uint8_t(1));
    ot0::CommandShift s(112, -368);
    s.Update(b.read(), b.block(), M, true);
    assert(!s.Shifted() && m.writes == 0);
    m.put(ctlw + 0x1b0, uint8_t(0));
    s.Update(b.read(), b.block(), M, true);
    assert(s.Shifted());
    m.put(ctlc + 8, uint32_t(0)); // widget released: restore must not write
    const int w = m.writes;
    s.Restore(b.read(), b.block(), M);
    assert(!s.Shifted() && m.writes == w);
  }
  { // a refused transform transaction changes nothing; failed restore retains ownership
    auto m = Fixture(); Bound b{m};
    m.put(ctlw + 0x1b0, uint32_t(0x12340000)); // native padding must survive
    ot0::CommandShift s(112, -368);
    const ot0::WriteBlock refuse = [](auto, auto, auto, auto) { return false; };
    s.Update(b.read(), refuse, M, true);
    assert(!s.Shifted() && m.writes == 0 && m.f(ctlw + 0x198) == 0.0f);
    s.Update(b.read(), b.block(), M, true);
    assert(s.Shifted() && m.writes == 1);
    s.Restore(b.read(), refuse, M);
    assert(s.Shifted() && m.f(ctlw + 0x1a8) == 112.0f);
    s.Restore(b.read(), b.block(), M);
    uint32_t flag{}; m.read(ctlw + 0x1b0, &flag, 4);
    assert(!s.Shifted() && flag == 0x12340000 && m.f(ctlw + 0x198) == 0.0f);
  }
  std::cout << "ot0 hud ok\n";
}
