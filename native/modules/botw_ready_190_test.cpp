// SPDX-License-Identifier: GPL-3.0-or-later
#include "01007EF00011E000.cpp"
#include "botw_test_fixture.h"
#include "botw_inventory_update_fixture.h"
#include <cassert>
#include <iostream>
struct ReadyFixture:Fixture {
  static constexpr uint64_t G=0x8000000,H=G+0x1000,B=G+0x2000,Ints=G+0x3000,Int=G+0x4000,
    Floats=G+0x5000,Float=G+0x6000,Bools=G+0x7000,Bomb=G+0x8000,Stasis=G+0x9000,
    Info=0x9000000,Player=Info+0x10000,Rune=Info+0x20000,World=0xA000000,Parts=World+0x1000,
    Weather=World+0x2000,Temp=World+0x3000,Hud=0xB000000,TempPane=Hud+0x10000,
    SensorPane=Hud+0x20000,TempAnim=Hud+0x30000,SensorAnim=Hud+0x40000,
    Map=0xC000000,Sensor=Map+0x10000;
  ReadyFixture() {
    update_fixture(*this);const auto& i=botw::Inventory190;const auto& r=botw::Ready190;
    put(M+i.gamedata_root,G);put(G,M+i.gamedata_vtable);put(G+0xBE0,H);put(H,B);
    put<int32_t>(B+0x18,1);put<int32_t>(B+0x1C,1);put(B+0x20,Ints);put(Ints,Int);
    put(Int,std::array<uint8_t,40>{});put(Int,M+i.int_flag_vtable);put<uint8_t>(Int+8,1);
    put<int32_t>(Int+20,999999);put<int32_t>(Int+24,347549);put<uint32_t>(Int+28,0x23149BF8);
    put<int32_t>(B+0x28,1);put<int32_t>(B+0x2C,1);put(B+0x30,Floats);put(Floats,Float);
    put(Float,std::array<uint8_t,40>{});put(Float,M+r.float_flag_vtable);put<uint8_t>(Float+8,2);
    put<float>(Float+20,360);put<float>(Float+24,302.5f);put<uint32_t>(Float+28,0x9C79A188);
    put<int32_t>(B+8,2);put<int32_t>(B+12,2);put(B+16,Bools);put(Bools,Bomb);put(Bools+8,Stasis);
    for(auto p:{Bomb,Stasis}){put(p,M+i.bool_flag_vtable);put<uint32_t>(p+8,0x01000700);}
    put<uint32_t>(Bomb+12,0x604C3BAE);put<uint32_t>(Stasis+12,0x9CA3D1C1);
    put(M+r.rune_root,Rune);put(Rune,M+r.rune_vtable);put<int32_t>(Rune+0x240,3);
    put(M+i.player_info_root,Info);put(Info,M+i.player_info_vtable);put(Info+0x60,Player);
    put(Player,M+i.player_vtable);put<float>(Info+0xA4,1500);put<float>(Info+0xA8,3000);put<float>(Player+0x186C,3000);
    put<float>(U+0x70,0.5f);
    put(M+r.world_root,World);put(World,M+r.world_vtable);put<int32_t>(World+0x5C0,9);put<int32_t>(World+0x5C4,9);
    put(World+0x5C8,Parts);put<int32_t>(World+0x780,0);put(Parts+24,Weather);put(Parts+32,Temp);
    put(Weather,M+r.weather_vtable);put(Temp,M+r.temperature_vtable);put<float>(Temp+0x20,23);put<float>(Temp+0x40,23);put<float>(Temp+0x60,0);
    put(Weather+0x284,std::array<uint8_t,5>{0,1,2,6,4});
    for(auto off:{r.stage_current,r.stage_requested,r.map_type})stage(off,"MainField");
    put(A+37*8,Hud);put(Hud,M+r.hud_vtable);put(Hud+0x36F0,TempPane);put(TempPane,M+r.temperature_pane_vtable);
    put(TempPane+0x130,TempAnim);put(TempAnim,M+r.animator_vtable);put<float>(TempAnim+0x20,74);
    put(Hud+0x3D80,SensorPane);put(SensorPane,M+r.sensor_pane_vtable);put<float>(SensorPane+0x144,75);
    put(SensorPane+0x158,SensorAnim);put(SensorAnim,M+r.animator_vtable);put<float>(SensorAnim+0x20,75);
    put(M+r.map_root,Map);put(Map,M+r.map_vtable);put<int32_t>(Map+0x38A8,0);put<uint8_t>(Map+0x38AC,1);
    put(M+r.sensor_root,Sensor);put(Sensor,M+r.sensor_vtable);
  }
  void stage(uint64_t offset,const char* name) {
    std::array<uint8_t,56> b{};const auto vt=M+botw::Ready190.stage_vtable,p=M+offset+20;const int32_t capacity=32;
    std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+8,&p,8);std::memcpy(b.data()+16,&capacity,4);
    std::memcpy(b.data()+20,name,std::strlen(name)+1);put(M+offset,b);
  }
};
int main() {
  using F=ReadyFixture;
  {F f;assert(botw::ready_supported(f.h)&&botw::supported(f.h));
    auto c=botw::currency(f.h);assert(c.valid&&c.value==347549);
    auto t=botw::world_clock(f.h);assert(t.valid&&t.minutes==1210);
    auto r=botw::current_rune(f.h);assert(r.valid&&r.id==3&&r.upgraded);
    auto s=botw::stamina(f.h);assert(s.valid&&s.depleted&&s.percent==50);
    auto n=botw::noise(f.h);assert(n.valid&&n.percent==25);
    auto e=botw::environment(f.h);assert(e.valid&&e.temperature==23);
    auto d=botw::temperature_dial(f.h);assert(d.valid&&botw::temperature_rotation(d)==-42);
    auto sensor=botw::sensor(f.h);assert(sensor.valid&&sensor.frame==75&&sensor.enabled);
    f.stage(botw::Ready190.map_type,"CDungeon");assert(!botw::main_field_stage(f.h));
    assert(!botw::sensor(f.h).valid&&!botw::temperature_dial(f.h).valid);
  }
  for(auto at:{F::Int,F::Float,F::World,F::Temp,F::Rune,F::Player,F::SensorPane,F::TempAnim}) {
    F f;f.race=at;
    if(at==F::Int)assert(!botw::currency(f.h).valid);
    if(at==F::Float)assert(!botw::world_clock(f.h).valid);
    if(at==F::World||at==F::Temp)assert(!botw::environment(f.h).valid);
    if(at==F::Rune)assert(!botw::current_rune(f.h).valid);
    if(at==F::Player)assert(!botw::stamina(f.h).valid);
    if(at==F::SensorPane)assert(!botw::sensor(f.h).valid);
    if(at==F::TempAnim)assert(!botw::temperature_dial(f.h).valid);
  }
  {F f;f.h.build_id[31]^=1;f.h.is_mapped=[](void*,uint64_t,uint64_t)->EdenDsmodBool{assert(false);return false;};
    assert(!botw::currency(f.h).valid&&!botw::world_clock(f.h).valid&&!botw::stamina(f.h).valid);
    assert(!botw::environment(f.h).valid&&!botw::sensor(f.h).valid&&!botw::temperature_dial(f.h).valid);
  }
  std::cout<<"1.9.0 Ready values, pane shifts, CDungeon scope and torn/unknown refusal passed\n";
}
