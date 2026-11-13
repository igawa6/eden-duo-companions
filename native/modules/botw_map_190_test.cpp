// SPDX-License-Identifier: GPL-3.0-or-later
#include "01007EF00011E000.cpp"
#include "botw_inventory_update_fixture.h"
#define BOTW_MAP_FIXTURE_ONLY
#include "botw_map_test.cpp"
#include "botw_map_area_test.cpp"
#include "botw_map_pins_test.cpp"
#include "botw_map_actions_test.cpp"
#include "botw_map_places_test.cpp"
#include "botw_map_koroks_test.cpp"
#include "botw_map_moving_test.cpp"
#include "botw_map_special_test.cpp"
#include "botw_map_death_test.cpp"
#include "botw_map_names_test.cpp"
#include "botw_map_regions_test.cpp"
#include "botw_map_view_test.cpp"
#include "botw_map_markers_test.cpp"
namespace {
template<class F> void map_update(F& f) {
  update_fixture(f);std::vector<std::pair<uint64_t,uint64_t>> ids;
  ids.emplace_back(F::M+botw::Map160.root,F::M+botw::Map190.root);
  ids.emplace_back(F::M+botw::Map160.vtable,F::M+botw::Map190.vtable);
  ids.emplace_back(F::M+botw::Map160.controller_root,F::M+botw::Map190.controller_root);
  ids.emplace_back(F::M+botw::Map160.controller_vtable,F::M+botw::Map190.controller_vtable);
  ids.emplace_back(F::M+botw::Map160.area_vtable,F::M+botw::Map190.area_vtable);
  ids.emplace_back(F::M+botw::Map160.pin_vtable,F::M+botw::Map190.pin_vtable);
  ids.emplace_back(F::M+botw::Map160.stamp_vtable,F::M+botw::Map190.stamp_vtable);
  ids.emplace_back(F::M+botw::Map160.place_vtable,F::M+botw::Map190.place_vtable);
  ids.emplace_back(F::M+botw::Map160.shrine_vtable,F::M+botw::Map190.shrine_vtable);
  ids.emplace_back(F::M+botw::Map160.tower_vtable,F::M+botw::Map190.tower_vtable);
  ids.emplace_back(F::M+botw::Map160.start_vtable,F::M+botw::Map190.start_vtable);
  ids.emplace_back(F::M+botw::Map160.beast_vtable,F::M+botw::Map190.beast_vtable);
  ids.emplace_back(F::M+botw::Map160.lab_vtable,F::M+botw::Map190.lab_vtable);
  ids.emplace_back(F::M+botw::Map160.ex_vtable,F::M+botw::Map190.ex_vtable);
  ids.emplace_back(F::M+botw::Map160.korok_vtable,F::M+botw::Map190.korok_vtable);
  ids.emplace_back(F::M+botw::Map160.horse_vtable,F::M+botw::Map190.horse_vtable);
  ids.emplace_back(F::M+botw::Map160.yunobo_vtable,F::M+botw::Map190.yunobo_vtable);
  ids.emplace_back(F::M+botw::Map160.wolf_vtable,F::M+botw::Map190.wolf_vtable);
  ids.emplace_back(F::M+botw::Map160.bike_vtable,F::M+botw::Map190.bike_vtable);
  ids.emplace_back(F::M+botw::Map160.medallion_vtable,F::M+botw::Map190.medallion_vtable);
  ids.emplace_back(F::M+botw::Map160.guardian_vtable,F::M+botw::Map190.guardian_vtable);
  ids.emplace_back(F::M+botw::Map160.name_vtable,F::M+botw::Map190.name_vtable);
  ids.emplace_back(F::M+botw::Map160.label_source_vtable,F::M+botw::Map190.label_source_vtable);
  ids.emplace_back(F::M+botw::Map160.discovered_source_vtable,F::M+botw::Map190.discovered_source_vtable);
  ids.emplace_back(F::M+botw::Map160.death_vtable,F::M+botw::Map190.death_vtable);
  ids.emplace_back(F::M+botw::Map160.death_root,F::M+botw::Map190.death_root);
  ids.emplace_back(F::M+botw::Map160.death_manager_vtable,F::M+botw::Map190.death_manager_vtable);
  ids.emplace_back(F::M+botw::Map160.player_marker_vtable,F::M+botw::Map190.player_marker_vtable);
  ids.emplace_back(F::M+botw::Map160.save_handles,F::M+botw::Map190.save_handles);
  ids.emplace_back(F::M+botw::Map160.position_flag_vtable,F::M+botw::Map190.position_flag_vtable);
  ids.emplace_back(F::M+botw::Map160.screen_vtable,F::M+botw::Map190.screen_vtable);
  ids.emplace_back(F::M+botw::Map160.null_pane_vtable,F::M+botw::Map190.null_pane_vtable);
  ids.emplace_back(F::M+botw::Map160.picture_pane_vtable,F::M+botw::Map190.picture_pane_vtable);
  ids.emplace_back(F::M+botw::Map160.root_pane_vtable,F::M+botw::Map190.root_pane_vtable);
  ids.emplace_back(F::M+botw::Ready160.float_flag_vtable,F::M+botw::Ready190.float_flag_vtable);
  ids.emplace_back(F::M+botw::Ready160.rune_root,F::M+botw::Ready190.rune_root);
  ids.emplace_back(F::M+botw::Ready160.rune_vtable,F::M+botw::Ready190.rune_vtable);
  ids.emplace_back(F::M+botw::Ready160.world_root,F::M+botw::Ready190.world_root);
  ids.emplace_back(F::M+botw::Ready160.world_vtable,F::M+botw::Ready190.world_vtable);
  ids.emplace_back(F::M+botw::Ready160.weather_vtable,F::M+botw::Ready190.weather_vtable);
  ids.emplace_back(F::M+botw::Ready160.temperature_vtable,F::M+botw::Ready190.temperature_vtable);
  ids.emplace_back(F::M+botw::Ready160.map_root,F::M+botw::Ready190.map_root);
  ids.emplace_back(F::M+botw::Ready160.map_vtable,F::M+botw::Ready190.map_vtable);
  ids.emplace_back(F::M+botw::Ready160.sensor_root,F::M+botw::Ready190.sensor_root);
  ids.emplace_back(F::M+botw::Ready160.sensor_vtable,F::M+botw::Ready190.sensor_vtable);
  ids.emplace_back(F::M+botw::Ready160.hud_vtable,F::M+botw::Ready190.hud_vtable);
  ids.emplace_back(F::M+botw::Ready160.sensor_pane_vtable,F::M+botw::Ready190.sensor_pane_vtable);
  ids.emplace_back(F::M+botw::Ready160.temperature_pane_vtable,F::M+botw::Ready190.temperature_pane_vtable);
  ids.emplace_back(F::M+botw::Ready160.animator_vtable,F::M+botw::Ready190.animator_vtable);
  ids.emplace_back(F::M+botw::Ready160.stage_current,F::M+botw::Ready190.stage_current);
  ids.emplace_back(F::M+botw::Ready160.stage_requested,F::M+botw::Ready190.stage_requested);
  ids.emplace_back(F::M+botw::Ready160.map_type,F::M+botw::Ready190.map_type);
  ids.emplace_back(F::M+botw::Ready160.stage_vtable,F::M+botw::Ready190.stage_vtable);
  ids.emplace_back(F::M+botw::Quests160.root,F::M+botw::Quests190.root);
  ids.emplace_back(F::M+botw::Quests160.manager_vtable,F::M+botw::Quests190.manager_vtable);
  ids.emplace_back(F::M+botw::Quests160.quest_vtable,F::M+botw::Quests190.quest_vtable);
  ids.emplace_back(F::M+botw::Quests160.step_vtable,F::M+botw::Quests190.step_vtable);
  ids.emplace_back(F::M+botw::Quests160.destination_handle,F::M+botw::Quests190.destination_handle);
  ids.emplace_back(F::M+botw::Quests160.dlc_root,F::M+botw::Quests190.dlc_root);
  ids.emplace_back(F::M+botw::Quests160.dlc_vtable,F::M+botw::Quests190.dlc_vtable);
  ids.emplace_back(F::M+botw::Quests160.shrine_vtable,F::M+botw::Quests190.shrine_vtable);
  ids.emplace_back(F::M+botw::Quests160.tower_vtable,F::M+botw::Quests190.tower_vtable);
  const auto original=f.mem;
  for(auto [at,byte]:original) {(void)byte;if(at%4)continue;uint64_t v{};bool full=true;
    for(unsigned i=0;i<8;++i){auto it=original.find(at+i);if(it==original.end()){full=false;break;}v|=uint64_t(it->second)<<(i*8);}
    if(full)for(auto [old,next]:ids)if(v==old){f.put(at,next);break;}}
  for(auto [old,next]:ids) {bool full=true;uint64_t v{};
    for(unsigned i=0;i<8;++i){auto it=f.mem.find(old+i);if(it==f.mem.end()){full=false;break;}v|=uint64_t(it->second)<<(i*8);}
    if(full&&old!=next){f.put(next,v);for(unsigned i=0;i<8;++i)f.mem.erase(old+i);}}
  for(auto [old,next]:{std::pair{botw::Ready160.stage_current,botw::Ready190.stage_current},std::pair{botw::Ready160.stage_requested,botw::Ready190.stage_requested},std::pair{botw::Ready160.map_type,botw::Ready190.map_type}}) {
    if(!original.contains(F::M+old+20))continue;
    for(unsigned i=0;i<56;++i)if(auto at=original.find(F::M+old+i);at!=original.end())f.template put<uint8_t>(F::M+next+i,at->second);
    f.put(F::M+next,F::M+botw::Ready190.stage_vtable);f.put(F::M+next+8,F::M+next+20);
    for(unsigned i=0;i<56;++i)f.mem.erase(F::M+old+i);
  }
  for(unsigned i=0;i<2;++i){uint32_t value{};bool full=true;
    for(unsigned j=0;j<4;++j){auto it=original.find(F::M+botw::Map160.save_handles+4*i+j);if(it==original.end()){full=false;break;}value|=uint32_t(it->second)<<(8*j);}
    if(full)f.put(F::M+botw::Map190.save_handles+4*i,value);}
}
}
int main() {
  {MapFixture f;map_update(f);assert(botw::supported(f.h)&&botw::map_build_supported(f.h));assert(botw::map_snapshot(f.h).valid);auto p=botw::map_position(f.h);assert(p.valid&&p.x==-2988&&p.y==1941);f.race=MapFixture::Data+0x120;assert(!botw::map_snapshot(f.h).valid);}
  {AreaFixture f;map_update(f);assert(botw::map_area(f.h).valid);}
  {PinFixture f;map_update(f);assert(botw::map_pins(f.h).valid);}
  {PlacesFixture f;map_update(f);assert(botw::map_places(f.h).valid);}
  {KorokFixture f;map_update(f);assert(botw::map_koroks(f.h).valid);}
  {MovingFixture f;map_update(f);assert(botw::map_moving(f.h).valid);}
  {SpecialFixture f;map_update(f);assert(botw::map_special(f.h).valid);}
  {DeathFixture f;map_update(f);assert(botw::map_death(f.h).valid);}
  {NamesFixture f;map_update(f);assert(botw::map_names(f.h).valid);}
  {RegionsFixture f;map_update(f);assert(botw::map_regions(f.h).valid);}
  {ViewFixture f;map_update(f);f.put(f.Screen+0x5270,f.Sr);f.put(f.Screen+0x3DB0,f.Root);for(auto off:{0x5410,0x3E30})for(unsigned i=0;i<8;++i)f.mem.erase(f.Screen+off+i);auto v=botw::map_view(f.h);assert(v.valid&&v.cone_valid&&v.cone_pane==f.Cone);f.put(f.Cone,f.M+botw::Map160.picture_pane_vtable);assert(!botw::map_view(f.h).cone_valid);}
  {MarkerFixture f;map_update(f);assert(botw::quest_map_markers(f.h,f.journal).valid);}
  {EditFixture f;map_update(f);auto v=botw::map_edit_snapshot(f.h);assert(v.valid&&botw::saved_map_icons(f.h).valid);f.put(f.C,f.M+botw::Map160.controller_vtable);assert(!botw::map_edit_snapshot(f.h).valid);}
  {EditBridgeFixture f;map_update(f);f.mb<uint32_t>(0,0x464F5544);f.bridge.poll(f.h,{});assert(f.bridge.map_supported);assert(f.bridge.request_map(6,123,456));f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.native_place();f.bridge.poll(f.h,{});assert(f.bridge.pending);f.save_place();f.bridge.poll(f.h,{});assert(!f.bridge.pending&&std::string(f.bridge.status)=="Marker placed");}
  {EditBridgeFixture f;map_update(f);f.mb<uint32_t>(0,0x464F5544);f.native_place();f.save_place();f.bridge.poll(f.h,{});
    assert(f.bridge.request_map(0,0,0,0));f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.put<uint32_t>(f.N+0x3C,0);
    f.bridge.poll(f.h,{});assert(f.bridge.pending); // native cleared, save still contains the pin
    f.put<int32_t>(f.No+10*40+0x18,-1);f.bridge.poll(f.h,{});
    assert(!f.bridge.pending&&std::string(f.bridge.status)=="Marker deleted");}
  {MapFixture f;map_update(f);f.h.build_id[0]^=1;assert(!botw::map_build_supported(f.h)&&!botw::map_position(f.h).valid&& !botw::map_snapshot(f.h).valid);}
  std::cout<<"1.9 Map roots/classes/panes, markers, saved icons and persistent completion passed\n";
}
