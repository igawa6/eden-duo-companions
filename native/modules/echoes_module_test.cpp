// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_reader.h"
#include "echoes_requalification.h"
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <string>

namespace {
struct Host {
  EdenDsmodHostApi api{};
  std::map<std::string, int64_t> points;
  std::map<std::string, std::string> texts;
  std::map<std::string, double> floats;
  size_t reads{};
  Host() {
    api.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    api.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    api.struct_size = sizeof(api);
    api.title_id = echoes::TitleId;
    api.main_base = 0x80000000;
    api.main_size = 0x3500000;
    api.userdata = this;
    for (size_t i = 0; i < 32; ++i) {
      const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
      api.build_id[i] = static_cast<uint8_t>((nibble(echoes::BuildId[2*i]) << 4) |
                                            nibble(echoes::BuildId[2*i+1]));
    }
    api.is_mapped = [](void*, uint64_t, uint64_t) -> EdenDsmodBool { return true; };
    api.read_memory = [](void* self, uint64_t, void*, size_t) -> EdenDsmodBool {
      ++static_cast<Host*>(self)->reads;
      return false;
    };
    api.publish_i64 = [](void* self, const char* key, int64_t value) {
      static_cast<Host*>(self)->points[key] = value;
    };
    api.publish_f64 = [](void* self,const char* key,double value) { static_cast<Host*>(self)->floats[key]=value; };
    api.publish_text = [](void* self,const char* key,const char* value) {
      static_cast<Host*>(self)->texts[key] = value;
    };
    api.get_tick = [](void*) -> uint64_t { return 0; };
    api.read_romfs = [](void*,const char*,uint64_t,void*,size_t) -> size_t { return 0; };
  }
};
}
int main() {
  assert(!eden_dsmod_get_module(0, EDEN_DSMOD_MODULE_ABI_HASH));
  assert(!eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, 0));
  const auto* module = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION,
                                            EDEN_DSMOD_MODULE_ABI_HASH);
  assert(module && module->title_id == echoes::TitleId);
  assert(module->capabilities & EDEN_DSMOD_CAP_WRITE_MEMORY);
  assert(!(module->capabilities & EDEN_DSMOD_CAP_GUEST_CALL));
  assert(!module->supports_build(nullptr));
  assert(module->supports_build(echoes::BuildId.data()));
  assert(!module->create(nullptr, nullptr));
  Host host;
  host.api.title_id ^= 1;
  assert(!module->create(&host.api, nullptr));
  host.api.title_id = echoes::TitleId;
  host.api.build_id[0] ^= 1;
  assert(!module->create(&host.api, nullptr));
  host.api.build_id[0] ^= 1;
  const auto mapped = host.api.is_mapped;
  host.api.is_mapped = nullptr;
  assert(!module->create(&host.api, nullptr));
  host.api.is_mapped = mapped;
  // Missing mandatory host callbacks must fail before invoking guest reads.
  const auto valid_api = host.api;
  for (int missing=0;missing<6;++missing) {
    auto bad=valid_api;
    if(missing==0)bad.read_memory=nullptr;
    if(missing==1)bad.publish_i64=nullptr;
    if(missing==2)bad.publish_text=nullptr;
    if(missing==3)bad.get_tick=nullptr;
    if(missing==4)bad.read_romfs=nullptr;
    if(missing==5)bad.is_mapped=nullptr;
    const auto before=host.reads;
    assert(!module->create(&bad,nullptr));assert(host.reads==before);
  }
  for(int malformed=0;malformed<3;++malformed) {
    auto bad=valid_api;
    if(malformed==0)--bad.struct_size;
    if(malformed==1)++bad.abi_version;
    if(malformed==2)bad.abi_hash^=1;
    assert(!module->create(&bad,nullptr));
  }
  void* instance = module->create(&host.api, nullptr);
  assert(instance);
  // Previously actionable publication must be overwritten on an unavailable sample.
  host.points["echoes.selector.held"] = 1;
  host.points["echoes.selector.count"] = 127;
  host.points["echoes.selector.index"] = 3;
  host.points["echo.home.ready"]=1;
  host.points["echo.page"]=3;
  host.points["echo.inspect.count"]=127;
  host.points["echo.inventory.ready"]=1;
  for(const char* key:{"echo.input.hold","echo.input.x","echo.input.y",
      "echo.menu.input.plus","echo.menu.input.r","echo.menu.input.up",
      "echo.menu.input.down","echo.menu.input.left","echo.menu.input.right",
      "echo.menu.input.a","echo.menu.input.b","echo.auto.busy",
      "echo.inspect.action.ready","echo.inspect.favorite.known","echo.inspect.equip.ready",
      "echo.warp.ready","echo.warp.panel","echo.warp.selected","echo.warp.confirm.ready",
      "echo.warp.input.any","echo.warp.input.minus","echo.warp.input.a","echo.warp.input.b",
      "echo.warp.input.neutral","echo.warp.input.west","echo.warp.input.east",
      "echo.warp.input.north","echo.warp.input.south","echo.warp.input.northwest",
      "echo.warp.input.northeast","echo.warp.input.southwest","echo.warp.input.southeast"})
    host.points[key]=1;
  for(const char* key:{"echo.sort.image","echo.inspect.heading","echo.inspect.description",
                       "echo.inspect.category","echo.inspect.icon","echo.inspect.count.image","echo.selected.name","echo.footer.count"})
    host.texts[key]="stale";
  module->sample(instance, &host.api);
  assert(host.reads > 0);
  // Frozen host default-view lookup prefers ints: these must remain float-only.
  assert(host.floats.at("echo.map.view.zoom")==1.26);
  assert(host.floats.at("echo.map.view.center")==0.5);
  assert(!host.points.contains("echo.map.view.zoom"));
  assert(!host.points.contains("echo.map.view.center"));
  assert(host.points.at("echoes.build_ok") == 0);
  assert(host.points.at("echoes.collection.count") == 0);
  assert(host.points.at("echoes.selector.held") == 0);
  assert(host.points.at("echoes.selector.count") == 0);
  assert(host.points.at("echoes.selector.index") == -1);
  assert(host.points.at("echoes.selector.id") == 0xffff);
  assert(host.points.at("echo.ui.held") == 0);
  assert(host.points.at("echo.ui.count") == 0);
  assert(host.points.at("echo.drive.any") == 0);
  assert(host.points.at("echo.home.ready")==0);
  assert(host.points.at("echo.page")==0);
  assert(host.points.at("echo.inspect.count")==0);
  assert(host.points.at("echo.inspect.index")==-1);
  assert(host.points.at("echo.inventory.ready")==0);
  for(const char* key:{"echo.input.hold","echo.input.x","echo.input.y",
      "echo.menu.input.plus","echo.menu.input.r","echo.menu.input.up",
      "echo.menu.input.down","echo.menu.input.left","echo.menu.input.right",
      "echo.menu.input.a","echo.menu.input.b","echo.auto.busy",
      "echo.inspect.action.ready","echo.inspect.favorite.known","echo.inspect.equip.ready",
      "echo.warp.ready","echo.warp.panel","echo.warp.selected","echo.warp.confirm.ready",
      "echo.warp.input.any","echo.warp.input.minus","echo.warp.input.a","echo.warp.input.b",
      "echo.warp.input.neutral","echo.warp.input.west","echo.warp.input.east",
      "echo.warp.input.north","echo.warp.input.south","echo.warp.input.northwest",
      "echo.warp.input.northeast","echo.warp.input.southwest","echo.warp.input.southeast"})
    assert(host.points.at(key)==0);
  for(const char* key:{"echo.sort.image","echo.inspect.heading","echo.inspect.description",
                       "echo.inspect.category","echo.inspect.icon","echo.inspect.count.image","echo.selected.name","echo.footer.count"})
    assert(host.texts.at(key).empty());
  assert(!eden_dsmod_get_extensions(0,EDEN_DSMOD_EXT_HASH));
  assert(!eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION,0));
  const auto* ext=eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION,EDEN_DSMOD_EXT_HASH);
  assert(ext && !ext->on_action(instance,"select",0));
  assert(!ext->on_action(instance,"select",-1));
  assert(!ext->on_action(instance,"invalid",0));
  assert(!ext->load_image(instance,&host.api,"module:echoes/assets/unknown",nullptr,
      [](void*,uint32_t,uint32_t,const uint8_t*,size_t){assert(false);}));
  assert(host.points.at("echo.map.ready")==0);
  for(const char* key:{"echo.dungeon.valid","echo.dungeon.ready","echo.dungeon.supported",
                       "echo.dungeon.map_item","echo.dungeon.floor.rows"})
    assert(host.points.at(key)==0);
  assert(host.texts.at("echo.dungeon.image").empty());
  for(size_t slot=0;slot<64;++slot)
    assert(host.points.at("echo.dungeon.slot."+std::to_string(slot)+".visible")==0);
  for(const char* key:{"module:echoes/dungeon/0/0/0","module:echoes/dungeon/64/0/0",
                       "module:echoes/dungeon//0/0","module:echoes/dungeon/0/-1/0",
                       "module:echoes/dungeon/0/0/2","module:echoes/dungeon/0/0/0/extra"})
    assert(!ext->load_image(instance,&host.api,key,nullptr,
      [](void*,uint32_t,uint32_t,const uint8_t*,size_t){assert(false);}));
  assert(host.texts.at("echo.map.image").empty());
  for(size_t slot=0;slot<64;++slot)
    assert(host.points.at("echo.map.slot."+std::to_string(slot)+".visible")==0);
  for(const auto& key:std::array<std::string,5>{
      "module:echoes/map/0/"+std::string(128,'0'),
      "module:echoes/map/64/"+std::string(128,'0'),
      "module:echoes/map/0/"+std::string(128,'g'),
      "module:echoes/map/0/0", "module:echoes/map//"+std::string(128,'0')})
    assert(!ext->load_image(instance,&host.api,key.c_str(),nullptr,
      [](void*,uint32_t,uint32_t,const uint8_t*,size_t){assert(false);}));
  for(int i=0;i<5;++i) {
    assert(host.points.at("echo.sort.mode."+std::to_string(i)+".selected")==0);
    assert(host.texts.at("echo.sort.mode."+std::to_string(i)+".image").empty());
  }
  const auto unavailable_reads=host.reads;
  for(const char* action:{"dungeon_floor_menu","dungeon_floor","page","inspect","select","inventory_action",
      "warp_open","warp_select","warp_travel","warp_close",
      "favorite_selected","equip_echo","favorite_add/0","favorite_remove","sort","sort_to"}) {
    for(int64_t value:{INT64_C(-1),INT64_C(0),INT64_C(1),INT64_C(3),INT64_C(4),INT64_C(65535),INT64_MAX})
      assert(!ext->on_action(instance,action,value));
  }
  assert(!ext->on_action(nullptr,"select",1));
  assert(!ext->on_action(instance,nullptr,1));
  assert(host.reads==unavailable_reads); // Published unavailable state refuses before rereading.
  {
    auto changed=host.api;changed.main_base+=0x1000;
    module->sample(instance,&changed);assert(host.reads==unavailable_reads);
    changed=host.api;changed.userdata=nullptr;
    module->sample(instance,&changed);assert(host.reads==unavailable_reads);
    assert(!ext->on_action(instance,"page",3));
    assert(!ext->on_action(instance,"select",INT64_MAX));
  }
  const auto reads = host.reads;
  host.api.build_id[0] ^= 1;
  module->sample(instance, &host.api);
  assert(host.reads == reads); // Refuse before touching unsupported guest memory.
  module->destroy(instance);
  // A mapped-range refusal must suppress read_memory itself, including the
  // font/context/inventory startup paths, and leave every action unavailable.
  {
    Host unmapped;
    unmapped.api.is_mapped=[](void*,uint64_t,uint64_t)->EdenDsmodBool{return false;};
    void* rejected=module->create(&unmapped.api,nullptr);assert(rejected);
    module->sample(rejected,&unmapped.api);
    assert(unmapped.reads==0);
    assert(unmapped.points.at("echo.ui.held")==0);
    assert(unmapped.points.at("echo.home.ready")==0);
    assert(!ext->on_action(rejected,"inspect",1));
    assert(!ext->on_action(rejected,"page",3));
    module->destroy(rejected);
  }
  {
    int observations{};
    const auto recovered=echoes::RequalifyFresh([&]{return ++observations;},[](int value){return value==2;});
    assert(recovered==2 && observations==2); // A full fresh second read qualifies.
    observations=0;
    const auto unavailable=echoes::RequalifyFresh([&]{return -(++observations);},[](int value){return value>0;});
    assert(unavailable==-3 && observations==3); // Bounded; final unknown stays unknown.
    observations=0;
    const auto first=echoes::RequalifyFresh([&]{return ++observations;},[](int value){return value==1;});
    assert(first==1 && observations==1); // Never repeat an already qualified read.
  }
  std::cout << "Echoes module ABI/refusal/publication checks passed\n";
}
