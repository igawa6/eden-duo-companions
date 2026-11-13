// SPDX-License-Identifier: GPL-3.0-or-later
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main equipment_fixture_main
#include "botw_player_equipment_test.cpp"
#undef main
#define main hold_fixture_main
#include "botw_hold_test.cpp"
#undef main
#pragma GCC diagnostic pop

#include "botw_inventory_update_fixture.h"
int main() {
  using E=PlayerEquipmentFixture;using H=HoldFixture;
  static_assert(botw::Inventory190.pause_shift==24);
  static_assert(botw::Inventory190.carry_actor_vtable==0x1CBB040);
  static_assert(botw::Inventory190.weapon_actor_vtable==0x1C3F330);
  static_assert(botw::Inventory190.armor_actor_vtable==0x1BC2210);
  {E f;update_fixture(f);assert(botw::supported(f.h));assert(botw::inventory_supported(f.h));
    const auto s=botw::player_equipment(f.h);assert(s.valid&&s.player==E::Player);
    for(unsigned i=0;i<6;++i)assert(s.slots[i].name=="Equipment_"+std::to_string(i));}
  for(unsigned i=0;i<6;++i)for(unsigned fault=0;fault<5;++fault){E f;update_fixture(f);
    auto actor=E::Actors+i*0x1000,link=E::Links+i*0x1000;
    if(fault==0)f.put(actor,E::M+(i<3?0x2AE2348:0x2A6D608));
    if(fault==1)f.put<uint32_t>(link+0x40,999);
    if(fault==2)f.put<uint32_t>(actor+0x60,999);
    if(fault==3)f.put<uint64_t>(actor+0x68,0);
    if(fault==4)f.race=actor+0x60;
    assert(!botw::player_equipment(f.h).valid);
  }
  {H f;update_fixture(f);const auto pouch=botw::pouch(f.h);assert(pouch.valid&&pouch.items.size()==2);
    assert(botw::context(f.h)==botw::UiContext::World);assert(botw::held_carry(f.h).empty());
    assert(botw::inventory_hold(f.h,H::Apple,pouch).valid);}
  for(unsigned fault=0;fault<5;++fault){H f;f.active_carry();update_fixture(f);
    assert(botw::held_carry(f.h).active());
    if(fault==0)f.put(H::CarryActor,H::M+0x1BC3558); // rejected static candidate
    if(fault==1)f.put<uint32_t>(H::Carry+0xA78,0x62); // observed transient, never ready
    if(fault==2)f.put<uint32_t>(H::CarryActor+0x60,100);
    if(fault==3)f.put<uint64_t>(H::CarryActor+0x68,0);
    if(fault==4)f.race=H::Carry+0x308;
    assert(!botw::held_carry(f.h).valid);
  }
  {H f;update_fixture(f);f.h.build_id[31]^=1;
    f.h.is_mapped=[](void*,uint64_t,uint64_t)->EdenDsmodBool{assert(false);return false;};
    f.h.read_memory=[](void*,uint64_t,void*,size_t)->EdenDsmodBool{assert(false);return false;};
    assert(!botw::inventory_supported(f.h));assert(!botw::pouch(f.h).valid);
    assert(!botw::player_equipment(f.h).valid);assert(!botw::held_carry(f.h).valid);
  }
  std::cout<<"1.9.0 inventory evidence gate, equipment/hold readers and stale identity/race refusal passed\n";
}
