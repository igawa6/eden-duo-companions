// SPDX-License-Identifier: GPL-3.0-or-later
#define BOTW_CHAMPION_FIXTURE_ONLY
#include "botw_champions_test.cpp"
#include "botw_effects.h"

int main() {
  ChampionFixture f;
  std::array<float,36> timers{};std::array<uint32_t,12> levels{};
  for(unsigned i=0;i<12;++i)timers[i*3+2]=-1;
  levels[5]=2;timers[15]=7496;timers[16]=7497;
  f.put(f.Player+0x1ED8,timers);f.put(f.Player+0x1FF0,levels);
  assert(f.mem.at(f.Player+0x1EE0+3)==0xBF); // first -1 marker in the 1.6 layout
  auto e=botw::active_effects(f.h,f.Player);
  assert(e.valid&&e.count==1&&e.rows[0].kind==botw::EffectKind::Attack&&e.rows[0].seconds==250);
  f.put(f.U+0x68,std::array<uint8_t,24>{});f.put<float>(f.U+0x68,-10);
  f.put<float>(f.U+0x6C,-20);f.put<uint8_t>(f.U+0x74,1);
  f.put(f.Player+0x2028,std::array<uint32_t,3>{0,1,0});
  f.put(f.Player+0x2084,std::array<float,2>{40,-10});
  timers[21]=1796;timers[22]=1797;f.put(f.Player+0x1ED8,timers);
  auto t=botw::temperature_sample(f.h,f.Player);assert(t.valid&&t.food_heat_s==60);
  f.put<float>(f.Player+0x1EE0,0);assert(!botw::active_effects(f.h,f.Player).valid);
  std::cout<<"1.6 memory-backed effect and thermal timers passed\n";
}
