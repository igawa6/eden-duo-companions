// SPDX-License-Identifier: GPL-3.0-or-later
#include "01007EF00011E000.cpp"
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>

// An unsupported compile must not interpret even a plausible old-layout fixture.
// Keep the real update ID here so widening either gate requires revisiting this test.
int main() {
  constexpr char Update[] =
      "CD57B23FA4BBAD65803D9788C01821EE00000000000000000000000000000000";
  Fixture f;
  assert(supports(botw::Build));
  assert(botw::supported(f.h));
  assert(botw::context(f.h) == botw::UiContext::World);
  assert(!supports(nullptr));
  assert(!supports(""));
  assert(!supports(Update));

  for (unsigned i = 0; i < 32; ++i) {
    auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
    f.h.build_id[i] = (hex(Update[2*i]) << 4) | hex(Update[2*i+1]);
  }
  assert(!botw::supported(f.h));
  // Fail immediately on any attempt to inspect guest memory after refusal.
  f.h.is_mapped = [](void*, uint64_t, uint64_t) -> EdenDsmodBool {
    assert(false && "unsupported build attempted guest mapping access");
    return false;
  };
  f.h.read_memory = [](void*, uint64_t, void*, size_t) -> EdenDsmodBool {
    assert(false && "unsupported build attempted guest read");
    return false;
  };
  assert(botw::context(f.h) == botw::UiContext::Unknown);
  assert(!botw::pouch(f.h).valid);
  assert(!botw::currency(f.h).valid);
  assert(!botw::world_clock(f.h).valid);
  assert(!botw::current_rune(f.h).valid);
  assert(!botw::stamina(f.h).valid);
  assert(!botw::noise(f.h).valid);
  assert(!botw::map_snapshot(f.h).valid);
  assert(!botw::map_position(f.h).valid);
  assert(!botw::map_pins(f.h).valid);
  assert(!botw::map_koroks(f.h).valid);
  assert(!botw::map_landmarks(f.h).valid);
  assert(!botw::quests(f.h).valid);
  assert(!botw::quest_destination(f.h).valid);
  // A one-byte change to the supported ID and the wrong title also fail closed.
  Fixture unknown;
  unknown.h.build_id[31] ^= 1;
  assert(!botw::supported(unknown.h));
  Fixture wrong_title;
  wrong_title.h.title_id ^= 1;
  assert(!botw::supported(wrong_title.h));
  std::cout << "1.6.0 gate accepted; 1.9.0/unknown/title gates refused; "
               "14 reader entry points refused without guest access\n";
}
