// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Move the fixture's main-relative identities, removing every old root/type.
// Object addresses, fields, generations and links stay independent of the table.
template<class F> void update_fixture(F& f) {
  std::vector<std::pair<uint64_t,uint64_t>> ids;
  const auto& a=botw::Inventory160;const auto& b=botw::Inventory190;
  const uint64_t botw::InventoryOffsets::* fields[]={
    &botw::InventoryOffsets::pouch_root,&botw::InventoryOffsets::pouch_vtable,
    &botw::InventoryOffsets::pouch_string_vtable,&botw::InventoryOffsets::ui_root,
    &botw::InventoryOffsets::screen_root,&botw::InventoryOffsets::ui_vtable,
    &botw::InventoryOffsets::screens_vtable,&botw::InventoryOffsets::player_info_root,
    &botw::InventoryOffsets::player_info_vtable,&botw::InventoryOffsets::player_vtable,
    &botw::InventoryOffsets::creator_root,&botw::InventoryOffsets::creator_vtable,
    &botw::InventoryOffsets::player_gear_vtable,&botw::InventoryOffsets::player_armor_vtable,
    &botw::InventoryOffsets::weapon_actor_vtable,&botw::InventoryOffsets::armor_actor_vtable,
    &botw::InventoryOffsets::info_data_root,&botw::InventoryOffsets::info_data_vtable,
    &botw::InventoryOffsets::carry_root,&botw::InventoryOffsets::carry_vtable,
    &botw::InventoryOffsets::carry_record_vtable,&botw::InventoryOffsets::carry_actor_vtable,
    &botw::InventoryOffsets::actor_heap_root,&botw::InventoryOffsets::eat_player_root,
    &botw::InventoryOffsets::eat_root_vtable,&botw::InventoryOffsets::pause_player_vtable,
    &botw::InventoryOffsets::inventory_screen_vtable,&botw::InventoryOffsets::inventory_column_vtable,
    &botw::InventoryOffsets::gamedata_root,&botw::InventoryOffsets::gamedata_vtable,
    &botw::InventoryOffsets::bool_flag_vtable,&botw::InventoryOffsets::int_flag_vtable};
  for(auto field:fields)ids.emplace_back(F::M+a.*field,F::M+b.*field);
  const auto original=f.mem;
  for(const auto& [at,byte]:original) {
    (void)byte;
    if(at%8)continue;
    uint64_t value{};bool complete=true;
    for(unsigned i=0;i<8;++i){auto it=original.find(at+i);if(it==original.end()){complete=false;break;}value|=uint64_t(it->second)<<(i*8);}
    if(!complete)continue;
    for(auto [old,next]:ids)if(value==old){f.put(at,next);break;}
  }
  for(auto [old,next]:ids) {
    bool complete=true;for(unsigned i=0;i<8;++i)if(!f.mem.contains(old+i))complete=false;
    if(!complete)continue;
    uint64_t value{};for(unsigned i=0;i<8;++i)value|=uint64_t(f.mem.at(old+i))<<(i*8);
    f.put(next,value);for(unsigned i=0;i<8;++i)f.mem.erase(old+i);
  }
  for(unsigned i=0;i<32;++i){auto hex=[](char c){return c<='9'?c-'0':c-'A'+10;};f.h.build_id[i]=uint8_t((hex(botw::InventoryBuild190[2*i])<<4)|hex(botw::InventoryBuild190[2*i+1]));}
}
