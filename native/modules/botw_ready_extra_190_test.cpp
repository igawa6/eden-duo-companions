// SPDX-License-Identifier: GPL-3.0-or-later
#define BOTW_CHAMPION_FIXTURE_ONLY
#include "botw_champions_test.cpp"
#include "botw_inventory_update_fixture.h"
namespace {
struct ReadyExtra190Fixture:ChampionFixture {
  static constexpr uint64_t Hud=0xB100000,Meter=0xB200000,Physics=0xB300000,Bodies=0xB400000,Item=0xB500000;
  ReadyExtra190Fixture() {
    update_fixture(*this);const auto original=mem;
    for(auto [off,n]:{std::pair{0x1DF4,48},std::pair{0x1CC8,12}}){
      for(int j=0;j<n;++j)mem.erase(Player+off+j);
      for(int j=0;j<n;++j)if(auto at=original.find(Player+off+j);at!=original.end())put<uint8_t>(Player+off+4+j,at->second);
    }
    put<int32_t>(Player+0x848,20);put<int32_t>(Player+0x1868,120);put<float>(Info+0xA0,120);
    put(U+0x68,std::array<uint8_t,24>{});put<float>(U+0x68,-10);put<float>(U+0x6C,-20);put<uint8_t>(U+0x74,1);
    put(Player+0x200C,std::array<uint32_t,3>{});put(Player+0x2028,std::array<uint32_t,3>{0,1,0});put(Player+0x2084,std::array<float,2>{40,-10});
    put(Player+0x1ED8+7*12,std::array<float,9>{});put(A+37*8,Hud);put(Hud,M+botw::Ready190.hud_vtable);put(Hud+0x36F0,Meter);
    put(Meter,M+botw::Ready190.temperature_pane_vtable);put<uint32_t>(Meter+0x1D8,0);
    put(Player+0x6A8,Physics);put<int32_t>(Physics+0x58,1);put(Physics+0x60,Bodies);put(Bodies+0x40,M+botw::Ready190.physics_body_vtable);put<uint8_t>(Bodies+0x1C0,2);
    put(Item+0x28,M+botw::Inventory190.pouch_string_vtable);put(Item+0x30,Item+0x3C);put<uint32_t>(Item+0x80,25);put<uint32_t>(Item+0x88,botw::ModifierDurabilityUp);
  }
};
}
int main() {
  using F=ReadyExtra190Fixture;
  {F f;auto a=botw::champion_abilities(f.h);for(unsigned i=0;i<4;++i)assert(a[i].valid&&a[i].charges==(i?3:1));
    for(unsigned i=0;i<4;++i){auto off=botw::Ready190.champion_timers[i];f.put<float>(f.Player+off,1800);f.put<float>(f.Player+off+8,-3);assert(botw::champion_abilities(f.h)[i].seconds==20);}}
  {F f;f.race=f.Player+0x1DF8;assert(!botw::champion_abilities(f.h)[1].valid);}
  {F f;auto h=botw::player_health(f.h,f.Info,f.Player);assert(h.valid&&h.life==20&&h.missing()==100);f.put<float>(f.Info+0xA0,NAN);assert(!botw::player_health(f.h,f.Info,f.Player).valid);}
  {F f;auto m=botw::gear_modifier(f.h,f.Item);assert(m.valid&&m.value==25&&m.flags==botw::ModifierDurabilityUp);f.put(f.Item+0x28,f.M+botw::Inventory160.pouch_string_vtable);assert(!botw::gear_modifier(f.h,f.Item).valid);}
  {F f;auto t=botw::temperature_sample(f.h,f.Player);assert(t.valid&&t.hud_state==0&&t.burn_resist==2&&t.armor_cold==1&&t.cold_limit==-10);f.put(f.Bodies+0x40,f.M+botw::Ready160.physics_body_vtable);t=botw::temperature_sample(f.h,f.Player);assert(t.valid&&t.burn_resist==-1);}
  {F f;f.h.build_id[0]^=1;assert(!botw::player_health(f.h,f.Info,f.Player).valid&&!botw::gear_modifier(f.h,f.Item).valid&&!botw::temperature_sample(f.h,f.Player).valid);}
  std::cout<<"1.9 Champion fields, health, modifier and temperature detail readers passed\n";
}
