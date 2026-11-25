// SPDX-FileCopyrightText: Copyright 2026 Sickops
// SPDX-License-Identifier: GPL-3.0-or-later
// Drives the shipped Pokemon Legends Z-A module DLL through its exported C ABI, with a host
// that only offers the save extension pointed at a copy of the player's real save.
// Validates the exact binary the package ships, without booting the game.
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "za_build.h"
#include "za_save.h"
#include "za_map.h"
#include "za_catalog.h"

#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <array>
#include <bit>
#include <algorithm>
#include <set>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

struct FakeHost {
    std::map<std::string, long long> ints;
    std::map<std::string, double> floats;
    std::map<std::string, std::string> texts;
    unsigned long long tick = 0;
    std::string save_path;
    std::string romfs;
    int logs = 0;
    int active_slot=-1, deployed_slot=-1;
    std::map<uint64_t,std::vector<uint8_t>> working_memory;
    std::vector<uint8_t> live_party;
    unsigned long long bytes_read = 0;
};

void SetWorkingParty(FakeHost& h) {
    auto party=h.live_party;
    auto step=[](uint32_t v){v^=v<<2;v^=v>>15;v^=v<<13;return v;};
    uint32_t cipher=za::KParty;
    for(int i=std::popcount(cipher);i>0;--i)cipher=step(cipher);
    for(std::size_t i=0;i<party.size();++i){
        party[i]^=static_cast<uint8_t>(cipher>>(8*(i%4)));
        if(i%4==3)cipher=step(cipher);
    }
    const auto put=[&](uint64_t at,const auto& value){
        auto& bytes=h.working_memory[at];bytes.resize(sizeof(value));
        std::memcpy(bytes.data(),&value,sizeof(value));
    };
    put(0x100000+0x610A710,uint64_t{0x90000});
    put(0x90018,uint64_t{0x91000});
    put(0x911b0,uint64_t{0x92000});
    std::array<uint64_t,6> nodes{},actors{};
    for(int i=0;i<6;++i){
        const uint64_t node=0xa0000+i*0x1000,wrapper=0xb0000+i*0x1000;
        const uint64_t raw=0xc0000+i*0x1000,actor=0xd0000+i*0x1000;
        nodes[i]=node;actors[i]=actor;
        put(node,uint64_t{0x100000+0x3e2a438});put(node+0x10,node);put(node+0x50,wrapper);
        put(wrapper+0x10,wrapper);
        put(wrapper+0x30,std::array<uint64_t,8>{raw,0,za::kStoredSize,0,raw+0x200,0,16,0});
        const auto data=party.begin()+5+i*za::kPartySlotSize;
        h.working_memory[raw]={data,data+za::kStoredSize};
        h.working_memory[raw+0x200]={data+za::kStoredSize,data+za::kPartySize};
        put(actor,uint64_t{0x100000+0x3be0d28});put(actor+0x10,actor);
        put(node+0x64,std::array<float,8>{});
    }
    put(0x920f0,nodes);put(0x92060,actors);
}

bool ReadSelection(FakeHost& h,uint64_t address,void* out,size_t size){
    if(!h.working_memory.empty()){
        if(address>=0xd03ab&&address<=0xd53ab&&(address-0xd03ab)%0x1000==0&&size==1){
            if(out)*static_cast<uint8_t*>(out)=int((address-0xd03ab)/0x1000)==h.deployed_slot;
            return true;
        }
        const auto it=h.working_memory.find(address);
        if(it!=h.working_memory.end()&&size<=it->second.size()){
            if(out)std::memcpy(out,it->second.data(),size);
            return true;
        }
    }
    if(!h.live_party.empty()){
        const uint64_t words[]={0xaabbccdd00000000ull|za::KParty,0x50000,0xB45,0};
        uint64_t pointer=0;
        if(address==0x100000+0x610A670)pointer=0x20000;
        if(address==0x20030)pointer=0x30000;
        if(address==0x30008)pointer=0x40000;
        if(address==0x30010)pointer=0x40020;
        if(pointer&&size==8){if(out)std::memcpy(out,&pointer,8);return true;}
        if(address==0x40000&&size==sizeof(words)){if(out)std::memcpy(out,words,size);return true;}
        if(address>=0x50000&&address-0x50000<=h.live_party.size()&&
           size<=h.live_party.size()-(address-0x50000)){
            if(out)std::memcpy(out,h.live_party.data()+address-0x50000,size);
            return true;
        }
    }
    if(h.active_slot<0)return false;std::array<uint8_t,16> bytes{};uint64_t value=0;
    if(address==0x100000+0x48891E0&&size==8)value=0x80000;
    else if(address==0x80000&&size==8)value=0x100000+0x3DE1A20;
    else if(address==0x800B8&&size==8)value=0x81000;
    else if(address==0x81368&&size==8)value=0x82000;
    else if(address==0x82000&&size==16){value=0x100000+0x3BD3B30;std::memcpy(bytes.data(),&value,8);uint32_t index=h.active_slot,count=6;std::memcpy(bytes.data()+8,&index,4);std::memcpy(bytes.data()+12,&count,4);if(out)std::memcpy(out,bytes.data(),size);return true;}
    else return false;if(out)std::memcpy(out,&value,size);return true;
}

EdenDsmodHostApi MakeHost(FakeHost& h) {
    EdenDsmodHostApi api{};
    api.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    api.struct_size = sizeof(EdenDsmodHostApi);
    api.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    api.userdata = &h;
    api.title_id = 0x0100F43008C44000ull;
    api.main_base=0x100000;std::memcpy(api.build_id,za::kBuildId,sizeof(api.build_id));
    api.read_memory=[](void* p,uint64_t at,void* out,size_t size)->EdenDsmodBool{return ReadSelection(*static_cast<FakeHost*>(p),at,out,size);};
    api.is_mapped=[](void* p,uint64_t at,size_t size)->EdenDsmodBool{return ReadSelection(*static_cast<FakeHost*>(p),at,nullptr,size);};
    api.get_tick = [](void* p) { return static_cast<FakeHost*>(p)->tick; };
    api.get_i64=[](void* p,const char* name,int64_t fallback)->int64_t{const auto& values=static_cast<FakeHost*>(p)->ints;const auto found=values.find(name);return found==values.end()?fallback:found->second;};
    api.publish_i64 = [](void* p, const char* n, long long v) { static_cast<FakeHost*>(p)->ints[n] = v; };
    api.get_f64=[](void* p,const char* name,double fallback)->double{
        const auto& values=static_cast<FakeHost*>(p)->floats;
        const auto it=values.find(name);
        return it==values.end()?fallback:it->second;
    };
    api.publish_f64 = [](void* p, const char* n, double v) { static_cast<FakeHost*>(p)->floats[n] = v; };
    api.publish_text = [](void* p, const char* n, const char* v) { static_cast<FakeHost*>(p)->texts[n] = v; };
    api.get_text = [](void* p, const char* name) -> const char* {
        if(std::strcmp(name,"__page")!=0) return nullptr;
        const auto& texts=static_cast<FakeHost*>(p)->texts;
        const auto page=texts.find(name);
        return page==texts.end()||page->second.empty()?nullptr:page->second.c_str();
    };
    api.log = [](void* p, uint32_t, const char* m) {
        ++static_cast<FakeHost*>(p)->logs;
        std::printf("  module log: %s\n", m);
    };
    api.read_romfs = [](void* p, const char* path, uint64_t offset, void* out, size_t size) -> size_t {
        auto& host = *static_cast<FakeHost*>(p);
        const auto full = host.romfs + "/" + path;
        FILE* f = std::fopen(full.c_str(), "rb");
        if (!f) return 0;
        _fseeki64(f, 0, SEEK_END);
        const auto length = _ftelli64(f);
        if (length < 0) { std::fclose(f); return 0; }
        if (!out) { std::fclose(f); return static_cast<size_t>(length); }
        if (offset >= static_cast<uint64_t>(length)) { std::fclose(f); return 0; }
        _fseeki64(f, static_cast<int64_t>(offset), SEEK_SET);
        size = std::min(size, static_cast<size_t>(length - offset));
        const auto got = std::fread(out, 1, size, f);
        std::fclose(f);
        return got;
    };
    return api;
}

EdenDsmodHostSaveApi MakeSaveApi(FakeHost& h) {
    EdenDsmodHostSaveApi api{};
    api.version = EDEN_DSMOD_SAVE_EXT_VERSION;
    api.struct_size = sizeof(EdenDsmodHostSaveApi);
    api.abi_hash = EDEN_DSMOD_SAVE_EXT_HASH;
    api.userdata = &h;
    api.read_save_file = [](void* p, const char* name, unsigned long long offset, void* out,
                            size_t size) -> size_t {
        auto& host = *static_cast<FakeHost*>(p);
        if (std::strcmp(name, "main") != 0)
            return 0;
        FILE* f = std::fopen(host.save_path.c_str(), "rb");
        if (!f)
            return 0;
        std::fseek(f, 0, SEEK_END);
        const long long total = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (out == nullptr) {
            std::fclose(f);
            return static_cast<size_t>(total);
        }
        if (offset >= static_cast<unsigned long long>(total)) {
            std::fclose(f);
            return 0;
        }
        std::fseek(f, static_cast<long>(offset), SEEK_SET);
        const size_t read = std::fread(out, 1, size, f);
        std::fclose(f);
        host.bytes_read += read;
        return read;
    };
    return api;
}

int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

template <typename T>
void Show(const char* key, const T& value) {
    std::printf("  %-18s %s\n", key, std::to_string(value).c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: za_module_host <module.dll> <save-copy.bin> <romfs-dir>\n");
        return 77;
    }
    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 2;
    }
    using GetModuleFn = const EdenDsmodModuleApi* (*)(uint32_t, uint64_t);
    using GetSaveFn = const EdenDsmodModuleSaveExtensions* (*)(uint32_t, uint64_t);
    using GetExtFn = const EdenDsmodModuleExtensions* (*)(uint32_t, uint64_t);
    auto get_module = reinterpret_cast<GetModuleFn>(
        reinterpret_cast<void*>(GetProcAddress(lib, "eden_dsmod_get_module")));
    auto get_save = reinterpret_cast<GetSaveFn>(
        reinterpret_cast<void*>(GetProcAddress(lib, "eden_dsmod_get_save_extensions")));
    auto get_ext = reinterpret_cast<GetExtFn>(
        reinterpret_cast<void*>(GetProcAddress(lib, "eden_dsmod_get_extensions")));
    Check(get_module && get_save && get_ext, "exports present");
    if (!get_module || !get_save || !get_ext)
        return 1;

    const EdenDsmodModuleApi* api =
        get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const EdenDsmodModuleSaveExtensions* save_ext =
        get_save(EDEN_DSMOD_SAVE_EXT_VERSION, EDEN_DSMOD_SAVE_EXT_HASH);
    const EdenDsmodModuleExtensions* ext =
        get_ext(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    Check(api != nullptr, "module api negotiates");
    Check(save_ext != nullptr, "save extensions negotiate");
    Check(ext != nullptr, "base extensions negotiate");
    if (!api || !save_ext || !ext)
        return 1;
    std::printf("module: %s, title %016llX, caps %llX\n", api->name,
                static_cast<unsigned long long>(api->title_id),
                static_cast<unsigned long long>(api->capabilities));

    FakeHost h;
    h.save_path = argv[2];
    h.romfs = argv[3];
    const EdenDsmodHostApi host = MakeHost(h);
    const EdenDsmodHostSaveApi save_api = MakeSaveApi(h);

    void* m = api->create(&host, nullptr);
    Check(m != nullptr, "create");
    if (!m)
        return 1;
    save_ext->configure(m, &save_api);

    h.tick += 10;
    api->tick(m, &host);
    api->sample(m, &host);

    Check(h.ints["za.ready"] == 1, "za.ready");
    Check(h.ints["za.hash"] == 1, "za.hash");
    Check(h.texts["za.status"] == "sauvegarde a jour", "za.status text");
    std::printf("status: %s | ot: %s | money %lld | play %lld s\n", h.texts["za.status"].c_str(),
                h.texts["za.ot"].c_str(), h.ints["za.money"], h.ints["za.play"]);
    Show("za.party.count", h.ints["za.party.count"]);
    for (int i = 0; i < 6; ++i) {
        const std::string p = "za.p" + std::to_string(i);
        if (h.ints[p + ".v"])
            std::printf("  slot %d: sp %lld lvl %lld hp %lld/%lld nature %lld name %s\n", i,
                        h.ints[p + ".sp"], h.ints[p + ".lvl"], h.ints[p + ".hp"],
                        h.ints[p + ".hpmax"], h.ints[p + ".nature"], h.texts[p + ".name"].c_str());
    }
    Show("za.dex.caught", h.ints["za.dex.caught"]);
    Show("za.dex.known", h.ints["za.dex.known"]);
    Show("za.dex.g0", h.ints["za.dex.g0"]);
    Show("za.dex.g8.max", h.ints["za.dex.g8.max"]);
    Show("za.box.used", h.ints["za.box.used"]);
    Show("za.box.view1", h.ints["za.box.view1"]);
    Show("za.box.count", h.ints["za.box.count"]);
    Show("za.box.unlocked", h.ints["za.box.unlocked"]);
    Show("za.box.view.used", h.ints["za.box.view.used"]);
    std::printf("  pmx %.1f pmy %.1f | xi %lld yi %lld zi %lld | street '%s' map '%s'\n",
                h.floats["za.pmx"], h.floats["za.pmy"], h.ints["za.xi"], h.ints["za.yi"],
                h.ints["za.zi"], h.texts["za.street"].c_str(), h.texts["za.map"].c_str());

    struct ImageResult { unsigned width = 0, height = 0; size_t size = 0; std::vector<uint8_t> pixels; } image;
    auto sink = +[](void* p, uint32_t w, uint32_t h, const uint8_t* data, size_t size) {
        *static_cast<ImageResult*>(p) = {w, h, size};
        static_cast<ImageResult*>(p)->pixels.assign(data,data+size);
    };
    Check(ext->load_image != nullptr, "image extension offered");
    if (ext->load_image) {
        Check(ext->load_image(m, &host, "module:icon/6400", &image, sink) == EDEN_DSMOD_TRUE,
              "Pikachu icon through exported ABI");
        Check(image.width == 160 && image.height == 160 && image.size == 160*160*4,
              "icon RGBA size");
        api->sample(m, &host);
        Check(!h.texts["za.p0.sname"].empty(), "species names published after asset load");
        Check(ext->load_image(m, &host, "module:map", &image, sink) == EDEN_DSMOD_TRUE,
              "map through exported ABI");
        Check(image.width == 640 && image.height == 640 && image.size == 640*640*4,
              "map RGBA size");
        Check(ext->load_image(m, &host, "module:icon/nonsense", &image, sink) == EDEN_DSMOD_FALSE,
              "invalid image key rejected");
    }

    Check(ext->load_image(m,&host,"module:icon/114688",&image,sink),"normal Lucario icon loads");
    const auto normal_lucario=image.pixels;
    Check(ext->load_image(m,&host,"module:icon/638976",&image,sink),"shiny Lucario icon loads");
    Check(image.pixels!=normal_lucario,"normal and shiny Lucario use different textures");
    Check(h.ints["za.zone0.v"]==1&&h.ints["za.zone19.v"]==1,"twenty game-authored wild zones");
    Check(h.ints["za.center8.v"]==1&&h.ints["za.center9.v"]==0,"nine Pokemon centers resolved from field meshes");
    Check(ext->load_image(m,&host,"module:zone/0",&image,sink),"zone material loads");
    Check(image.pixels.size()>4&&image.pixels[1]==0,"zone material coverage channel is empty outside its contour");
    Check(ext->load_image(m,&host,"module:center",&image,sink),"Pokemon center icon loads");
    Check(image.pixels.size()>4&&image.pixels[3]==0,"Pokemon center icon has a transparent background");
    for (const char* marker : {"module:map-player", "module:map-portal-wild", "module:map-portal-battle"}) {
        Check(ext->load_image(m, &host, marker, &image, sink), "game map marker decodes");
        Check(image.pixels.size() > 4 && image.pixels[3] == 0,
              "game map marker uses transparent material coverage");
    }
    Check(ext->on_action(m,"map_control",0),"wild zone filter toggles");
    api->sample(m,&host);
    Check(h.ints["za.zone0.v"]==0&&h.ints["za.map.image"]==2,"wild filter hides markers and selects red layer");
    Check(ext->on_action(m,"map_control",3),"zoom-in button accepted");
    api->sample(m,&host);
    Check(h.ints["za.map.percent"]==125,"zoom-in uses an intermediate magnification");
    Check(h.ints["za.map.image"]==2,"zoom preserves the stable filtered texture");
    h.floats["@view_zoom:lumiose"]=1.6;
    h.floats["@view_pan_x:lumiose"]=120;
    h.floats["@view_pan_y:lumiose"]=80;
    api->sample(m,&host);
    Check(h.ints["za.map.percent"]==160,"reads the fractional pinch state");
    ext->on_action(m,"map_control",0);
    api->sample(m,&host);
    bool found_zone=false;
    for(int i=0;i<20&&!found_zone;++i){
        const auto point="za.zone"+std::to_string(i)+".";
        if(!h.ints[point+"v"])continue;
        const auto x=std::uint32_t(h.floats[point+"x"]-za::kMapViewX);
        const auto y=std::uint32_t(h.floats[point+"y"]-za::kMapViewY);
        ext->on_action(m,"map_tap",std::int64_t((std::uint64_t(x)<<32)|y));
        api->sample(m,&host);
        found_zone=h.ints["za.zone.selected"]>0&&h.ints["za.zone.rows"]>0;
    }
    Check(found_zone,"zoomed tap still finds wild encounters");
    h.floats["@view_zoom:lumiose"]=8;
    api->sample(m,&host);
    Check(h.ints["za.map.percent"]==800,"compact map accepts a pinch beyond the old limit");
    h.floats["@view_zoom:lumiose"]=1.6;
    api->sample(m,&host);
    h.floats.erase("@view_zoom:lumiose");
    h.floats.erase("@view_pan_x:lumiose");
    h.floats.erase("@view_pan_y:lumiose");
    ext->on_action(m,"map_control",10);
    ext->on_action(m,"map_control",0);
    ext->on_action(m,"map_control",0);
    api->sample(m,&host);
    int cafes=0;
    for(int i=0;i<24;++i)cafes+=int(h.ints["za.cafe"+std::to_string(i)+".v"]);
    Check(cafes>=8,"game-authored cafés available");
    Check(ext->on_action(m,"cafe_pick",0),"café marker opens");
    api->sample(m,&host);
    Check(h.ints["za.center.cafe"]==1&&!h.texts["za.center.name"].empty(),"café uses its game name");
    ext->on_action(m,"center_close",0);
    Check(ext->on_action(m,"map_control",8),"café filter toggles");
    api->sample(m,&host);
    Check(h.ints["za.cafe0.v"]==0&&h.ints["za.map.cafes"]==0,"café filter hides markers");
    ext->on_action(m,"map_control",8);
    ext->on_action(m,"dex_gen",1);ext->on_action(m,"dex_filter",0);api->sample(m,&host);
    Check(h.ints["za.dex.rows"]==232,"Illumis catalogue has exactly 232 game-native species");
    Check(h.ints["za.d0.sp"]==152&&h.ints["za.d0.number"]==1,"regional order starts with Chikorita, not Bulbasaur");
    std::set<long long> regional;
    for(int start=0;start<232;start+=12){h.ints["@scroll_first:za_dex"]=start;api->sample(m,&host);
        for(int i=start;i<std::min(start+12,232);++i){const auto k="za.d"+std::to_string(i);Check(h.ints[k+".v"]&&h.ints[k+".number"]==i+1,"all scrolled dex rows retain regional numbering");Check(regional.insert(h.ints[k+".sp"]).second,"no duplicate regional species");}}
    Check(regional.size()==232,"entire regional list reachable by scrolling");
    Check(ext->on_action(m,"dex_pick",155),"absolute scrolled dex row accepted");api->sample(m,&host);Check(h.ints["za.detail.number"]==156,"far row updates persistent detail pane");
    ext->on_action(m,"dex_gen",2);api->sample(m,&host);Check(h.ints["za.dex.gen"]==2,"Mega Dimension catalogue selector");
    ext->on_action(m,"dex_gen",3);ext->on_action(m,"dex_filter",0);api->sample(m,&host);
    Check(h.ints["za.dex.gen"]==3&&h.ints["za.dex.rows"]>=40,"Mega forms have their own catalogue");
    std::set<long long> mega_icons;
    for(int i=0;i<h.ints["za.dex.rows"];++i){auto k="za.d"+std::to_string(i)+".";Check(mega_icons.insert(h.ints[k+"icon"]).second,"distinct Mega forms");}
    ext->on_action(m,"dex_pick",1);api->sample(m,&host);Check(h.ints["za.detail.icon"]==h.ints["za.d1.icon"],"Mega detail selects exact form");
    Check(ext->on_action(m,"center_travel",0)==EDEN_DSMOD_FALSE&&ext->on_action(m,"box_pick",0)==EDEN_DSMOD_FALSE,"menu-driven travel and swap unavailable");
    h.ints["@scroll_first:za_dex"]=0;
    Check(h.ints["za.center0.destination"]>0,"center has a real game travel destination");
    Check(ext->on_action(m,"center_pick",0)==EDEN_DSMOD_TRUE,"center opens its confirmation");api->sample(m,&host);
    Check(!h.texts["za.center.name"].empty()&&h.texts["za.center.name"]!="Centre Pokémon 1","center localized name from romfs");
    ext->on_action(m,"center_close",0);
    Check(ext->on_action(m,"team_select",5)==EDEN_DSMOD_TRUE,"team selection accepted");
    api->sample(m,&host);
    Check(h.ints["za.team.selected"]==5 && h.ints["za.sel.sp"]==h.ints["za.p5.sp"],"selected panel follows selected party slot");
    Check(!h.texts["za.sel.m0.name"].empty(),"selected move names from romfs");
    ext->on_action(m,"team_select",0);
    ext->on_action(m,"dex_filter",0);ext->on_action(m,"dex_gen",1);ext->on_action(m,"dex_pick",1);api->sample(m,&host);
    Check(h.ints["za.detail.sp"]==153 && h.ints["za.dex.open"]==0,"dex row updates persistent detail pane without page toggle");
    Check(!h.texts["za.detail.description"].empty(),"dex description from romfs");
    ext->on_action(m,"dex_pick",2);api->sample(m,&host);
    Check(h.ints["za.detail.sp"]==154 && h.ints["za.dex.open"]==0,"next dex tap changes detail and navigation edge");
    const auto before_guide=h.texts["__page"];
    Check(ext->on_action(m,"guide_combat",0),"selected dex species opens combat guide");
    h.texts["__page"]="combat";api->sample(m,&host);
    Check(h.ints["za.guide.type1"]==11&&h.ints["za.guide.weaknesses"]==5,
          "the guide uses the selected Grass opponent");
    Check(h.texts["za.gc0.name"]==h.texts["za.sel.m0.name"]&&!h.texts["za.gc0.factor"].empty(),
          "combat guide compares a real party move");
    Check(ext->on_action(m,"guide_evolutions",0),"selected dex species opens evolution guide");
    h.texts["__page"]="evolutions";api->sample(m,&host);
    Check(h.ints["za.guide.evolution_rows"]>=1&&h.texts["za.ge0.from"]=="Macronium"&&
          h.texts["za.ge0.to"]=="Méganium"&&h.texts["za.ge0.condition"]=="Niveau 32",
          "real evolution table describes Macronium evolving at level 32");
    h.texts["__page"]="dex";
    ext->on_action(m,"dex_pick",0);
    Check(ext->on_action(m,"guide_dex_pin",0),"a dex objective can be pinned");
    Check(ext->on_action(m,"guide_locations",0),"selected dex species opens encounter list");
    h.texts["__page"]="locations";api->sample(m,&host);
    Check(h.ints["za.guide.location_rows"]>0&&h.texts["za.gl0.label"].find("20")!=std::string::npos,
          "Germignon resolves to a catalog encounter in Wild Zone 20");
    Check(ext->on_action(m,"guide_map",0),"encounter opens its location on the map");
    h.texts["__page"]="map";api->sample(m,&host);
    Check(h.ints["za.zone.selected"]==20,"result selects Wild Zone 20 on the map");
    const auto zone_rows=h.ints["za.zone.rows"];
    const auto missing=h.ints["za.zone.missing"];
    Check(ext->on_action(m,"zone_missing_toggle",0),"wild zone filter toggles");
    api->sample(m,&host);
    Check(h.ints["za.zone.missing_only"]&&h.ints["za.zone.rows"]<=zone_rows&&
          h.ints["za.zone.missing"]==missing,"wild zone count stays stable while filtering");
    for(int i=0;i<h.ints["za.zone.rows"];++i)
        Check(h.ints["za.zp"+std::to_string(i)+".state"]<2,"filtered encounter is uncaught");
    ext->on_action(m,"zone_missing_toggle",0);
    h.texts["__page"]="notebook";api->sample(m,&host);
    Check(h.ints["za.guide.objective_rows"]==1&&!h.ints["za.go0.mission"],"notebook preserves the pinned species");
    Check(h.ints["za.note.pending0"]&&h.ints["za.note.value0"]>0,"species pin requests a persistent flag");
    h.ints["@flag:za_notebook_0"]=h.ints["za.note.value0"];
    api->sample(m,&host);
    Check(!h.ints["za.note.pending0"],"acknowledged species pin stops writing");
    Check(ext->on_action(m,"guide_open",0)&&ext->on_action(m,"guide_remove",0),"species objective can reopen its locations and be removed");
    h.texts["__page"]="locations";api->sample(m,&host);
    Check(h.ints["za.note.pending0"]&&!h.ints["za.note.value0"],"removing a pin clears its persisted slot");
    h.ints["@flag:za_notebook_0"]=0;
    h.texts["__page"]=before_guide;
    for(int category=0;category<3;++category){ext->on_action(m,"quest_category",category);api->sample(m,&host);Check(h.ints["za.quest.category"]==category,"mission category binds");Check(h.ints["za.quest.rows"]>0,"mission catalogue available");
        for(int i=0;i<h.ints["za.quest.rows"];++i){const auto state=h.ints["za.q"+std::to_string(i)+".state"];Check(state>=1&&state<=3,"undiscovered and unknown missions are excluded");}}
    ext->on_action(m,"quest_category",0);api->sample(m,&host);
    auto first_mission=h.texts["za.q0.title"];
    Check(ext->on_action(m,"quest_pick",0),"mission tap accepted");api->sample(m,&host);
    Check(h.ints["za.quest.detail.open"]==1 && h.texts["za.quest.detail.title"]==first_mission,"mission detail shows the selected title");
    Check(!h.texts["za.quest.body.0.text"].empty(),"mission detail has description");
    Check(h.ints["za.quest.detail.state"]==2,"active main quest highlighted");
    Check(ext->on_action(m,"quest_pin",0),"mission can be pinned");
    h.texts["__page"]="notebook";api->sample(m,&host);
    Check(h.ints["za.guide.objective_rows"]==1 && h.ints["za.go0.mission"] &&
          h.texts["za.go0.label"]==first_mission,"notebook keeps the selected mission identity");
    Check(h.ints["za.note.pending0"]&&h.ints["za.note.value0"]>0,"mission pin requests a persistent flag");
    h.ints["@flag:za_notebook_0"]=h.ints["za.note.value0"];
    FakeHost restored;
    restored.save_path=h.save_path;
    restored.romfs=h.romfs;
    restored.texts["__page"]="notebook";
    restored.ints["@flag:za_notebook_0"]=h.ints["@flag:za_notebook_0"];
    const auto restored_host=MakeHost(restored);
    const auto restored_save_api=MakeSaveApi(restored);
    if(void* fresh=api->create(&restored_host,nullptr)) {
        save_ext->configure(fresh,&restored_save_api);
        restored.tick+=10;
        api->tick(fresh,&restored_host);
        Check(ext->load_image(fresh,&restored_host,"module:ready",&image,sink),"fresh module loads the mission catalogue");
        api->sample(fresh,&restored_host);
        Check(restored.ints["za.guide.objective_rows"]==1 && restored.ints["za.go0.mission"] &&
              restored.texts["za.go0.label"]==first_mission&&!restored.ints["za.note.pending0"],
              "a fresh module restores the same mission identity from the host flag");
        api->destroy(fresh);
    } else {
        Check(false,"new module session");
    }
    Check(ext->on_action(m,"guide_open",0)&&ext->on_action(m,"guide_remove",0),"mission entry can reopen its detail and be removed");
    h.texts["__page"]="missions";api->sample(m,&host);
    h.ints["@flag:za_notebook_0"]=0;
    h.texts["__page"]=before_guide;
    ext->on_action(m,"quest_pick",0);api->sample(m,&host);
    Check(h.ints["za.quest.detail.open"]==1 && h.ints["za.q0.selected"]==1,"tapping the current mission preserves its detail");
    Check(!ext->on_action(m,"quest_map",0),"mission without a known pin cannot open the map");
    const auto point_id=static_cast<uint32_t>(h.ints["za.quest.detail.number"]);
    constexpr uint64_t system=0x210000, heaps=0x220000, ui_heap=0x230000, map=0x240000;
    constexpr uint64_t groups=0x250000, points=0x260000;
    const auto put=[&](uint64_t address,const auto& data){
        auto& bytes=h.working_memory[address];
        bytes.resize(sizeof(data));
        std::memcpy(bytes.data(),&data,sizeof(data));
    };
    put(host.main_base+0x610A000,system);
    put(system+0x5F8,heaps);
    put(heaps+0x6A8,ui_heap);
    put(ui_heap,host.main_base+0x3BCE178);
    put(ui_heap+0x38,map);
    std::array<uint8_t,4096> presenter{};
    const auto stamp=[](auto& bytes,size_t offset,auto value){
        std::memcpy(bytes.data()+offset,&value,sizeof(value));
    };
    stamp(presenter,0,host.main_base+0x3DF1D48);
    stamp(presenter,0x10,map);
    stamp(presenter,0x50,groups);
    stamp(presenter,0x58,groups+32);
    stamp(presenter,0x584,uint32_t{0x100});
    put(map,presenter);
    put(map+0x10,map);
    put(map+0x50,groups);
    put(map+0x58,groups+32);
    put(groups,std::array<uint64_t,4>{points,points+2*0x90,0,0});
    std::array<uint8_t,2*0x90> pin{};
    stamp(pin,0,uint8_t{1});
    stamp(pin,4,uint32_t{2});
    stamp(pin,8,512.f);
    stamp(pin,12,-256.f);
    stamp(pin,0x28,point_id);
    stamp(pin,0x90,uint8_t{3});
    stamp(pin,0x94,uint32_t{15});
    stamp(pin,0x98,-621.2474f);
    stamp(pin,0x9c,1455.9521f);
    stamp(pin,0xb8,uint32_t{1});
    put(points,pin);
    h.floats["@view_zoom:lumiose"]=1;
    h.floats["@view_pan_x:lumiose"]=0;
    h.floats["@view_pan_y:lumiose"]=0;
    api->tick(m,&host);api->sample(m,&host);
    Check(h.ints["za.quest.detail.pin"]==1,"selected mission resolves to its native map pin");
    int landmark=-1;
    for (int i=0;i<32;++i) {
        if(h.ints["za.landmark"+std::to_string(i)+".v"]){landmark=i;break;}
    }
    Check(landmark>=0 && ext->on_action(m,"landmark_pick",landmark),
          "visible native travel landmark can be selected");
    api->sample(m,&host);
    Check(h.texts["za.center.name"].find("Hôtel Z")!=std::string::npos,"landmark resolves to Hôtel Z");
    Check(!ext->on_action(m,"map_mark_start",0) && !ext->on_action(m,"map_mark_clear",0),
          "marker commands require the native game helper instead of a local substitute");
    h.floats.erase("@view_zoom:lumiose");
    h.floats.erase("@view_pan_x:lumiose");
    h.floats.erase("@view_pan_y:lumiose");
    Check(ext->on_action(m,"quest_map",0),"mission map shortcut accepted for a known pin");
    api->sample(m,&host);
    Check(h.ints["za.quest.show_map"]==1 && h.ints["za.map.quest.open"]==1 &&
          h.floats["za.map.home.zoom"]>2 && h.floats["za.map.home.x"]==0.375 &&
          h.floats["za.map.home.y"]==0.4375,"mission shortcut opens the map centered on the pin");
    stamp(pin,0x98,512.f);stamp(pin,0x9c,-256.f);put(points,pin);
    h.texts["__page"]="map";
    h.tick+=60;api->tick(m,&host);api->sample(m,&host);
    const auto landmark_key="za.landmark"+std::to_string(landmark);
    Check(h.ints["za.map.quest.pin_count"]>0&&h.ints["za.mq0.selected"]&&h.ints["za.mq0.shown"]&&
          h.floats["za.mq0.x"]==h.floats[landmark_key+".x"]&&
          h.floats["za.mq0.y"]==h.floats[landmark_key+".y"],
          "the selected quest remains visible when a landmark has exactly the same coordinates");
    Check(!h.ints["za.map.recenter"]&&!ext->on_action(m,"map_recenter",0),
          "recenter requires live coordinates");
    constexpr uint64_t position=0x280000;
    std::vector<uint8_t> registry(0x20000);
    stamp(registry,0,position);
    h.working_memory[host.main_base+0x40F0000]=std::move(registry);
    std::array<uint8_t,80> position_record{};
    stamp(position_record,0,host.main_base+0x3DA4348);
    std::memcpy(position_record.data()+0x18,"t1",3);
    stamp(position_record,0x38,-512.f);
    stamp(position_record,0x40,-256.f);
    put(position,position_record);
    h.texts["__page"]="map";
    h.floats["@view_zoom:lumiose"]=4;
    h.tick+=360;api->tick(m,&host);api->sample(m,&host);
    const auto map_reset=h.ints["za.map.home.reset"];
    Check(h.ints["za.map.recenter"]&&ext->on_action(m,"map_recenter",0),
          "recenter accepts a live position in Lumiose");
    api->sample(m,&host);
    float player_x{},player_y{};
    Check(za::MapPoint("t1",-512.f,-256.f,player_x,player_y)&&
          h.ints["za.map.home.reset"]>map_reset&&h.floats["za.map.home.zoom"]==4&&
          h.floats["za.map.home.x"]==std::clamp(player_x/640.f,.125f,.875f)&&
          h.floats["za.map.home.y"]==std::clamp(player_y/640.f,.125f,.875f),
          "recenter resets the viewport around the live marker at its current zoom");
    std::array<uint8_t,4096> mailbox{};
    EdenDsmodHostExtensions guest{};
    guest.userdata=mailbox.data();guest.mailbox_address=0x290000;guest.mailbox_size=mailbox.size();
    guest.load_u32=[](void* data,uint32_t offset,uint32_t* value)->EdenDsmodBool{
        std::memcpy(value,static_cast<uint8_t*>(data)+offset,sizeof(*value));return true;
    };
    guest.load_u64=[](void* data,uint32_t offset,uint64_t* value)->EdenDsmodBool{
        std::memcpy(value,static_cast<uint8_t*>(data)+offset,sizeof(*value));return true;
    };
    guest.store_u32=[](void* data,uint32_t offset,uint32_t value)->EdenDsmodBool{
        std::memcpy(static_cast<uint8_t*>(data)+offset,&value,sizeof(value));return true;
    };
    guest.store_u64=[](void* data,uint32_t offset,uint64_t value)->EdenDsmodBool{
        std::memcpy(static_cast<uint8_t*>(data)+offset,&value,sizeof(value));return true;
    };
    ext->configure(m,&guest);
    stamp(mailbox,8,uint32_t{1});
    put(host.main_base+0x612D920,uint32_t{0});
    const auto map_tick=[&]{++h.tick;api->tick(m,&host);api->sample(m,&host);};
    const auto marker_result=[&](uint32_t result){
        stamp(mailbox,0x10c,uint32_t{0});stamp(mailbox,0x120,result);map_tick();
    };
    const auto place_marker=[&]{
        return ext->on_action(m,"map_mark_start",0)&&
               ext->on_action(m,"map_mark_at",(int64_t{360}<<32)|360);
    };
    const auto map_focus=[&](bool focused){
        stamp(presenter,0x584,focused?uint32_t{0x100}:uint32_t{0});
        put(map,presenter);map_tick();
    };
    put(host.main_base+0x612D920,uint32_t{1});
    map_tick();
    Check(h.ints["za.map.mark.can_start"]&&place_marker(),"one tap starts placement even while the scene is busy");
    map_tick();
    uint32_t queued{};
    guest.load_u32(guest.userdata,0x10c,&queued);
    Check(!queued&&!h.ints["za.cmd.map_close"],"selected coordinates wait for the scene to become ready");
    put(host.main_base+0x612D920,uint32_t{0});
    map_tick();
    guest.load_u32(guest.userdata,0x10c,&queued);
    Check(queued==2&&!h.ints["za.cmd.marker_open"]&&!h.ints["za.cmd.map_close"],
          "the queued marker is sent once the open map is ready");
    marker_result(4);
    Check(h.ints["za.cmd.map_close"]==1&&h.ints["za.enforce.active"],"successful placement closes the game map");
    map_tick();
    Check(!h.ints["za.cmd.map_close"],"a marker confirmation sends one close command");
    for (int frame=1;frame<32;++frame) map_tick();
    Check(h.ints["za.cmd.map_close"],"an ignored close is retried only if the map remains focused");
    stamp(mailbox,8,uint32_t{2});
    map_focus(false);
    map_focus(true);
    Check(place_marker(),"another marker can be requested");
    map_tick();
    marker_result(3);
    Check(!h.ints["za.cmd.map_close"],"failed placement leaves the map open");
    map_focus(false);
    Check(ext->on_action(m,"map_mark_clear",0),"existing marker can be cleared from the street");
    map_tick();
    guest.load_u32(guest.userdata,0x10c,&queued);
    Check(queued==3&&h.ints["za.cmd.marker_open"],"clearing from the street opens the map without waiting for a native request");
    map_focus(true);
    marker_result(4);
    Check(h.ints["za.cmd.map_close"],"successful removal closes the game map");
    map_focus(false);
    Check(place_marker(),"marker placement can begin from the street");
    map_tick();
    guest.load_u32(guest.userdata,0x10c,&queued);
    Check(queued==2&&h.ints["za.cmd.marker_open"]&&h.ints["za.enforce.active"],
          "placing a marker from the street opens the map and queues the coordinates on the same tick");
    marker_result(4);
    Check(!h.ints["za.cmd.marker_open"]&&!h.ints["za.cmd.map_close"],
          "opening is sent once and closure waits for the focused map");
    constexpr uint64_t fresh_map=0x2a0000;
    auto fresh_presenter=presenter;
    stamp(fresh_presenter,0x10,fresh_map);
    stamp(fresh_presenter,0x584,uint32_t{0x100});
    put(fresh_map,fresh_presenter);
    put(fresh_map+0x10,fresh_map);
    put(fresh_map+0x50,groups);
    put(fresh_map+0x58,groups+32);
    stamp(mailbox,0x140,fresh_map);
    map_tick();
    Check(h.ints["za.cmd.map_close"],"the acknowledged model closes the fresh map without a UI heap scan");
    stamp(fresh_presenter,0x584,uint32_t{0});put(fresh_map,fresh_presenter);map_tick();
    stamp(fresh_presenter,0x584,uint32_t{0x100});put(fresh_map,fresh_presenter);map_tick();
    Check(place_marker(),"marker placement can begin while the map is focused");
    map_tick();
    stamp(fresh_presenter,0x208,uint32_t{2});put(fresh_map,fresh_presenter);
    marker_result(4);
    Check(!h.ints["za.cmd.map_close"],"a pending close never cancels a destination dialog");
    stamp(fresh_presenter,0x208,uint32_t{0});put(fresh_map,fresh_presenter);
    ext->configure(m,nullptr);
    h.floats.erase("@view_zoom:lumiose");
    h.texts["__page"]="missions";
    api->sample(m,&host);
    int active_count=0;
    for (int category=0;category<3;++category) {
        ext->on_action(m,"quest_category",category);api->sample(m,&host);
        for (int i=0;i<h.ints["za.quest.rows"];++i) {
            if (h.ints["za.q"+std::to_string(i)+".state"]==2) ++active_count;
        }
    }
    Check(ext->on_action(m,"quest_category",3),"the active filter can be selected");
    api->sample(m,&host);
    Check(active_count>0&&h.ints["za.quest.rows"]==active_count,"the active filter combines all three quest categories");
    for (int i=0;i<h.ints["za.quest.rows"];++i) {
        const auto key="za.q"+std::to_string(i)+".";
        Check(h.ints[key+"state"]==2&&!h.texts[key+"category"].empty(),
              "the active filter keeps only in-progress quests with their source category");
    }
    ext->on_action(m,"quest_category",2);api->sample(m,&host);
    const auto secondary_count=h.ints["za.quest.rows"];
    for(int i=0;i<secondary_count;++i){
        if(h.ints["za.q"+std::to_string(i)+".state"]!=2||!h.ints["za.q"+std::to_string(i)+".number"])continue;
        const auto number=uint32_t(h.ints["za.q"+std::to_string(i)+".number"]);
        stamp(pin,4,uint32_t{4});stamp(pin,0x28,number);put(points,pin);
        h.tick+=60;api->tick(m,&host);
        ext->on_action(m,"quest_pick",i);api->sample(m,&host);
        Check(h.ints["za.quest.detail.pin"]==1,"secondary quest resolves its display number, not its internal script ID");
        break;
    }
    h.working_memory.clear();
    h.texts["__page"]=before_guide;
    for (int i=0;i<secondary_count;++i) {
        if (h.ints["za.q"+std::to_string(i)+".number"]!=53) continue;
        ext->on_action(m,"quest_pick",i);api->sample(m,&host);
        break;
    }
    bool money=false,calcium=false;
    for (int i=0;i<h.ints["za.quest.body.rows"];++i) {
        const auto row="za.quest.body."+std::to_string(i)+".";
        money|=h.ints[row+"style"]==3 && h.texts[row+"text"].find("1800")!=std::string::npos;
        calcium|=h.ints[row+"icon"]==49 && h.texts[row+"text"].find("Calcium")!=std::string::npos;
    }
    Check(h.ints["za.quest.detail.number"]==53 && money && calcium,
          "mission 053 rewards have individual amount and item icon bindings");
    for (const auto* asset : {"module:map-portal-wild","module:map-portal-battle",
                             "module:map-portal-unknown","module:map-pin","module:map-landmark",
                             "module:item/49","module:progress/2","module:reward-money"}) {
        image={};
        Check(ext->load_image(m,&host,asset,&image,sink)==EDEN_DSMOD_TRUE && image.width>0,asset);
    }

    ext->on_action(m,"team_select",0);api->sample(m,&host);
    Check(h.ints["za.sel.type1"]==17 && h.ints["za.sel.type2"]==8,"Forgelina Fairy/Steel from personal table");
    Check(h.ints["za.sel.m0.type"]==8,"Metal Claw Steel from move table");
    Check(h.ints["za.p0.xpmax"]>1 && h.ints["za.p0.xp"]>0,"XP progress uses species growth table");
    image={};Check(ext->load_image(m,&host,"module:type/17",&image,sink)==EDEN_DSMOD_TRUE&&image.width==60,"type glyph decoded from game");
    ext->on_action(m,"quest_category",1);api->sample(m,&host);
    Check(h.ints["za.q0.state"]==2 && !h.texts["za.quest.body.0.text"].empty(),"active Extras mapped to Mable work and description");
    h.active_slot=0;api->tick(m,&host);ext->on_action(m,"team_select",5);
    Check(ext->on_action(m,"send_out",0),"send request accepted with valid HUD selection");
    api->tick(m,&host);api->sample(m,&host);Check(h.ints["za.cmd.right"]==1&&h.ints["za.cmd.send"]==0,"send selects before dispatching Up");
    for(int frame=0;frame<18;++frame)api->tick(m,&host);api->sample(m,&host);
    Check(h.ints["za.cmd.send"]==0,"never send while selected slot differs");
    h.active_slot=5;for(int frame=0;frame<18;++frame)api->tick(m,&host);api->sample(m,&host);
    Check(h.ints["za.cmd.send"]==1,"dispatch Up only after target selection confirmed");
    api->tick(m,&host);api->sample(m,&host);Check(h.ints["za.cmd.send"]==0,"send pulse not repeated");
    std::ifstream party_file(std::filesystem::path(argv[2]).parent_path()/"live-party.enc",std::ios::binary);
    h.live_party.assign(std::istreambuf_iterator<char>(party_file),{});
    SetWorkingParty(h);h.working_memory.erase(0xd5000);h.deployed_slot=0;h.active_slot=0;api->tick(m,&host);
    ext->on_action(m,"team_select",0);
    Check(ext->on_action(m,"attack",0),"attack request accepted when HUD slot matches");
    api->tick(m,&host);api->sample(m,&host);
    Check(h.ints["za.cmd.aim"]==1&&h.ints["za.cmd.attack0"]==0,"aim starts before attack");
    for(int frame=0;frame<11;++frame)api->tick(m,&host);
    api->sample(m,&host);
    Check(h.ints["za.cmd.aim"]==0&&h.ints["za.cmd.attack0"]==1,"move emitted once after aim");
    api->tick(m,&host);api->sample(m,&host);
    Check(h.ints["za.cmd.attack0"]==0,"move pulse clears");
    Check(h.ints["za.sel.m0.charging"]==0,"no predicted timer when combat node is absent");
    for(int frame=0;frame<120;++frame)api->tick(m,&host);
    api->sample(m,&host);
    Check(ext->on_action(m,"attack",0),"next attack can be prepared");
    api->tick(m,&host);
    h.active_slot=5;
    for(int frame=0;frame<11;++frame)api->tick(m,&host);
    api->sample(m,&host);
    Check(h.ints["za.cmd.attack0"]==0,"attack cancelled if the game selection changes");
    // Ask for a different Pokémon while the previously selected one is on the field.
    for(int frame=0;frame<26;++frame)api->tick(m,&host);
    h.active_slot=0;api->tick(m,&host);
    ext->on_action(m,"team_select",1);
    Check(ext->on_action(m,"attack",0),"attack on a benched party member queues a switch");
    api->tick(m,&host);api->sample(m,&host);
    Check(h.ints["za.cmd.right"]==1&&h.ints["za.cmd.send"]==0,"selects the party slot first");
    h.active_slot=1;
    for(int frame=0;frame<18;++frame)api->tick(m,&host);
    api->sample(m,&host);
    Check(h.ints["za.cmd.send"]==1&&h.ints["za.cmd.aim"]==0,"sends without aiming before deployment");
    for(int frame=0;frame<16;++frame)api->tick(m,&host);
    api->sample(m,&host);
    Check(h.ints["za.cmd.aim"]==0&&h.ints["za.cmd.attack0"]==0,"old field actor blocks attack");
    h.deployed_slot=1;
    api->tick(m,&host);api->sample(m,&host);
    Check(h.ints["za.cmd.aim"]==0,"deployment flag precedes the end of the animation");
    for(int frame=0;frame<44;++frame)api->tick(m,&host);
    api->sample(m,&host);
    Check(h.ints["za.cmd.aim"]==0,"does not aim before settling");
    api->tick(m,&host);api->sample(m,&host);
    Check(h.ints["za.cmd.aim"]==1,"confirmed field actor enables aim after settling");
    for(int frame=0;frame<11;++frame)api->tick(m,&host);
    api->sample(m,&host);
    Check(h.ints["za.cmd.attack0"]==1,"attack fires once after confirmed deployment and aim");
    const std::array<float,8> timing{4,0,0,0,8,0,0,0};
    std::memcpy(h.working_memory[0xa1064].data(),timing.data(),sizeof(timing));
    api->tick(m,&host);api->sample(m,&host);
    Check(h.ints["za.sel.m0.recharge"]==500&&h.texts["za.sel.m0.recharge_text"]=="4,0 s","shows observed cooldown");
    ext->on_action(m,"team_select",0);api->sample(m,&host);
    Check(h.ints["za.p1.c0"]==1&&h.ints["za.p1.r0"]==500,"portrait still shows the other Pokémon’s cooldown");
    Check(h.ints["za.p1.deployed"]==1&&h.ints["za.p0.selected"]==1&&!h.ints["za.p0.deployed"],
          "field partner and companion selection are independent");
    h.texts["__page"]="dex";
    ext->on_action(m,"dex_gen",1);
    ext->on_action(m,"dex_filter",0);
    ext->on_action(m,"dex_pick",1);
    Check(ext->on_action(m,"guide_combat",0),"live combat guide opens");
    h.texts["__page"]="combat";api->sample(m,&host);
    int ready_move=-1;
    for(int i=0;i<24;++i) if(h.ints["za.gc"+std::to_string(i)+".ready"]){ready_move=i;break;}
    Check(ready_move>=0&&h.ints["za.guide.ready_moves"]>0,"combat guide highlights a confirmed ready counter");
    if(ready_move>=0){
        const auto at=0xa0064+0x1000*(ready_move/4);
        std::array<float,8> charging{};
        std::memcpy(charging.data(),h.working_memory[at].data(),sizeof(charging));
        const auto original=charging;
        charging[ready_move%4]=2;
        charging[4+ready_move%4]=8;
        std::memcpy(h.working_memory[at].data(),charging.data(),sizeof(charging));
        api->tick(m,&host);api->sample(m,&host);
        Check(!h.ints["za.gc"+std::to_string(ready_move)+".ready"],"combat guide removes highlight during real cooldown");
        std::memcpy(h.working_memory[at].data(),original.data(),sizeof(original));
    }
    h.texts["__page"]=before_guide;
    for(int frame=0;frame<26;++frame)api->tick(m,&host);
    h.active_slot=0;api->tick(m,&host);
    Check(ext->on_action(m,"attack",0),"HUD selection alone is not a deployment");
    api->tick(m,&host);api->sample(m,&host);
    Check(h.ints["za.cmd.send"]==1&&h.ints["za.cmd.aim"]==0,"sends when HUD already selects the benched Pokémon");
    h.working_memory.clear();
    h.live_party.clear();
    h.active_slot=-1;api->tick(m,&host);Check(!ext->on_action(m,"send_out",0),"refuse send with unknown HUD selection");
    image={};Check(ext->load_image(m,&host,"module:item/1",&image,sink)==EDEN_DSMOD_TRUE && image.width==160,"item icon through exported ABI");
    ext->on_action(m,"dex_gen",1);
    for(int filter=1;filter<=4;++filter) {
        ext->on_action(m,"dex_filter",filter); api->sample(m,&host);
        for(int i=0;i<std::min<long long>(12,h.ints["za.dex.rows"]);++i) {
            const auto k="za.d"+std::to_string(i); if(!h.ints[k+".v"])continue;
            const auto state=h.ints[k+".state"];
            Check(filter==1 ? state>=2 : filter==2 ? state<2 : filter==3 ? state==1 : state==3,
                  "dex filter matches displayed state");
        }
    }
    ext->on_action(m,"bag_category",-1);api->sample(m,&host);
    const auto item_total=h.ints["za.bag.total"];
    std::set<long long> item_ids;
    Check(!h.texts["za.item.description"].empty()&&!h.texts["za.item.description"].starts_with("Description indisponible"),"item description decoded despite reversed text/labels pack order");
    for(int start=0;start<item_total;start+=12){h.ints["@scroll_first:za_bag"]=start;api->sample(m,&host);
        for(int i=start;i<std::min<long long>(start+12,item_total);++i){const auto k="za.i"+std::to_string(i);Check(h.ints[k+".v"],"scrolled bag row present");Check(item_ids.insert(h.ints[k+".id"]).second,"no duplicate bag item");Check(h.ints[k+".count"]>0||h.ints[k+".held"],"only owned/equipped bag rows");}}
    Check(item_ids.size()==item_total,"entire bag reachable by touch scrolling");
    ext->on_action(m,"bag_pick",100);api->sample(m,&host);Check(h.ints["za.item.id"]==h.ints["za.i100.id"],"absolute bag row selects correct object after scrolling");
    h.ints["@scroll_first:za_bag"]=0;
    long long category_total=0;
    for(int category=0;category<=8;++category) {
        ext->on_action(m,"bag_category",category);api->sample(m,&host);
        category_total+=h.ints["za.bag.rows"];
        for(int i=0;i<std::min<long long>(16,h.ints["za.bag.rows"]);++i) {
            const auto k="za.i"+std::to_string(i);
            if(h.ints[k+".v"])Check(h.ints[k+".pouch"]==category,"bag category filter");
        }
    }
    Check(category_total==item_total,"bag categories account for all items");
    ext->on_action(m,"bag_category",6);api->sample(m,&host);ext->on_action(m,"bag_pick",0);api->sample(m,&host);
    Check(h.texts["za.item.name"].find(" - ")!=std::string::npos&&h.ints["za.item.type"]>=0,"TM includes move name and type");
    Check(!h.texts["za.item.description"].empty(),"TM move detail available");
    ext->on_action(m,"zone_pick",0);api->sample(m,&host);Check(h.ints["za.zone.rows"]>0&&!h.texts["za.zp0.name"].empty(),"zone tap publishes a real encounter list");
    std::printf("regional dex + scroll windows + item/TM detail + zone encounters: OK; bag %lld items\n",item_total);

    // Selection is an exclusive, nonanimated flag, including after repeated taps.
    for(int tap=0;tap<18;++tap){
        const int selected=tap%6;Check(ext->on_action(m,"team_select",selected),"team selection accepted");api->sample(m,&host);
        int count=0;for(int i=0;i<6;++i){const bool on=h.ints["za.p"+std::to_string(i)+".selected"]!=0;count+=on;Check(on==(i==selected),"only selected party card marked");}
        Check(count==1,"exactly one selection border");
        Check(h.ints["za.sel.xp"]>=0&&h.ints["za.sel.xp"]<=h.ints["za.sel.xpmax"],"selected XP stays inside bar bounds");
    }
    const auto nav_origin=h.texts["__page"];
    h.texts["__page"]="team";api->sample(m,&host);
    Check(h.ints["za.nav.slot0.active"]&&h.ints["za.nav.slot1.id"]==4&&
          h.ints["za.nav.slot2.id"]==5&&!h.ints["za.nav.more.active"],"default fixed shortcuts");
    Check(ext->on_action(m,"nav_more",0)&&ext->on_action(m,"nav_pick",6),"More opens Missions");
    api->sample(m,&host);
    Check(h.ints["za.nav.destination"]==6&&!h.ints["za.nav.menu"],"choosing a tab closes the panel");
    h.texts["__page"]="missions";api->sample(m,&host);
    Check(h.ints["za.nav.more.active"]&&h.ints["za.nav.more.id"]==6&&!h.ints["za.nav.destination"],
          "More reflects the active secondary tab");
    Check(ext->on_action(m,"nav_more",0)&&ext->on_action(m,"nav_edit",0)&&
          ext->on_action(m,"nav_slot",2)&&ext->on_action(m,"nav_pick",3),"pin Boxes in slot three");
    api->sample(m,&host);
    Check(h.ints["za.nav.slot2.id"]==3&&h.ints["za.nav.pending"]&&h.ints["za.nav.encoded"]==143,
          "shortcut edit is published for persistence");
    h.ints["@flag:za_nav_shortcuts"]=143;api->sample(m,&host);
    Check(!h.ints["za.nav.pending"],"stored configuration acknowledges the pending write");
    Check(ext->on_action(m,"nav_slot",0)&&ext->on_action(m,"nav_pick",3),"already pinned tab can be moved");
    api->sample(m,&host);
    Check(h.ints["za.nav.slot0.id"]==3&&h.ints["za.nav.slot2.id"]==1,"moving a pinned tab swaps positions");
    Check(ext->on_action(m,"nav_shortcut",0),"updated shortcut opens Boxes");
    api->sample(m,&host);
    Check(h.ints["za.nav.destination"]==3&&!h.ints["za.nav.menu"],"shortcut remains usable during editing");
    h.texts["__page"]="box";h.ints["@flag:za_nav_shortcuts"]=341;api->sample(m,&host);
    h.texts["__page"]=nav_origin;h.ints["@flag:za_nav_shortcuts"]=145;api->sample(m,&host);
    Check(!ext->on_action(m,"box_details",-1)&&!ext->on_action(m,"box_details",30),"invalid box detail slots refused");
    Check(!ext->on_action(m,"native_box_prepare",0)&&!ext->on_action(m,"native_box_party",0),"direct swap rejects a save snapshot without live box/party data");
    int box_slot=-1;for(int i=0;i<30;++i)if(h.ints["za.b"+std::to_string(i)+".v"]){box_slot=i;break;}
    Check(box_slot>=0,"save fixture has a box Pokemon");
    if(box_slot>=0){
        const auto p="za.b"+std::to_string(box_slot);
        Check(ext->on_action(m,"box_details",box_slot),"box detail opens without exchange");api->sample(m,&host);
        Check(h.ints["za.boxdetail.icon"]==h.ints[p+".icon"]&&h.ints["za.boxdetail.sp"]==h.ints[p+".sp"],"box detail matches exact form and shiny flag");
        Check(!ext->on_action(m,"native_box_prepare",0)&&!h.ints["za.nativebox.ready"],"viewing a saved box does not arm a native swap");
        Check(!h.texts["za.boxdetail.name"].empty()&&h.ints["za.boxdetail.atk"]>0&&h.ints["za.boxdetail.hpmax"]>0,"box detail publishes name and computed stats");
        image={};Check(ext->load_image(m,&host,"module:radar/1",&image,sink)&&image.width==280&&image.height==280&&image.size==280*280*4,"stats radar RGBA through ABI");
    }
    Check(h.ints["za.progress.mable_level"]==41&&h.ints["za.progress.mable_points"]==70300,"Mable progression matches fixture");
    Check(h.ints["za.progress.hyperspace"]==10260,"hyperspace research matches fixture");
    Check(!h.ints["za.progress.is_infinite"]&&h.texts["za.progress.rank"]=="—","a save without a verified rank never displays infinity");
    Check(h.ints["za.research.rows"]==50,"all fifty research levels available");
    const auto previous_page=h.texts["__page"];h.texts["__page"]="progress";
    Check(!ext->on_action(m,"research_pick",-1)&&!ext->on_action(m,"research_pick",50),"research indexes bounded");
    Check(ext->on_action(m,"research_pick",40),"research level selected");api->sample(m,&host);
    Check(h.ints["za.research.open"]&&h.ints["za.research.selected"]==41&&h.ints["za.research.points"]==68400,"reward belongs to selected level");
    Check(h.ints["za.research.open.scroll"],"research scrollbar is enabled while its pane is open");
    Check(ext->on_action(m,"nav_more",0),"More can cover a research pane");api->sample(m,&host);
    Check(h.ints["za.research.open"]&&!h.ints["za.research.open.scroll"],
          "More suppresses the research scrollbar without closing the research pane");
    Check(ext->on_action(m,"nav_more",0),"More closes over the research pane");api->sample(m,&host);
    Check(h.ints["za.research.open.scroll"],"closing More restores the research scrollbar");
    Check(h.texts["za.research.reward"].find("Déflagration")!=std::string::npos&&!h.texts["za.research.description"].empty()&&h.ints["za.research.type"]==9,"TM reward has actual move name, effect and type");
    std::set<long long> levels;
    for(int start=0;start<50;start+=12){h.ints["@scroll_first:za_research"]=start;api->sample(m,&host);
        for(int i=start;i<std::min(start+12,50);++i){const auto p="za.rl"+std::to_string(i)+".";Check(!h.texts[p+"label"].empty(),"scrolled research level populated");Check(!h.texts[p+"reward"].empty()&&(i==0||h.ints[p+"icon"]>0),"research list shows reward and its artwork");levels.insert(i);}}
    Check(levels.size()==50,"whole research list reachable");
    h.texts["__page"]=previous_page;
    Check(ext->on_action(m,"research_close",0),"research closes");api->sample(m,&host);Check(!h.ints["za.research.open"],"overlay closed");
    Check(!ext->on_action(m,"dimension_pick",0)&&!h.ints["za.map.active_battles"],"unavailable live map records never become active markers");
    image={};Check(ext->load_image(m,&host,"module:cafe",&image,sink)&&image.size>0,"game-native cafe glyph decoded");
    for(int art=0;art<4;++art){std::printf("loading progress art %d\n",art);std::fflush(stdout);image={};Check(ext->load_image(m,&host,("module:progress/"+std::to_string(art)).c_str(),&image,sink)&&image.size>0,"game-native research artwork decoded");}
    long long previous_toggle=h.ints["za.boxdetail.toggle"];
    for(int click=0;click<5;++click){ext->on_action(m,"box_details",box_slot);api->sample(m,&host);Check(h.ints["za.boxdetail.toggle"]!=previous_toggle,"every box detail request produces a page-bind edge");previous_toggle=h.ints["za.boxdetail.toggle"];}
    Check(h.ints["za.map.battle_zones"]==12,"twelve game-authored battle zone contours");

    h.texts["__page"]="map";
    ext->on_action(m,"center_close",0);
    for(int i=0;i<8;++i){ext->on_action(m,"map_control",10);api->sample(m,&host);}
    Check(h.ints["za.map.wide"]&&h.ints["za.map.groups"]>0,"wide map groups overlapping markers");
    const double center_x=h.floats["za.center0.x"],center_y=h.floats["za.center0.y"];
    h.floats["@view_zoom:lumiose_large"]=13;
    h.floats["@view_pan_x:lumiose_large"]=center_x-200-420./12;
    h.floats["@view_pan_y:lumiose_large"]=center_y-94-420./12;
    api->sample(m,&host);
    Check(h.ints["za.map.percent"]==1200,"wide map clamps pinch to the new limit");
    Check(h.ints["za.center0.v"]&&std::abs(h.floats["za.center0.x"]-620)<.01&&
          std::abs(h.floats["za.center0.y"]-514)<.01,"a marker stays centered at maximum zoom");
    h.floats.erase("@view_zoom:lumiose_large");
    h.floats.erase("@view_pan_x:lumiose_large");
    h.floats.erase("@view_pan_y:lumiose_large");
    api->sample(m,&host);
    Check(ext->on_action(m,"map_group",0),"group opens its candidate list");
    api->sample(m,&host);
    Check(!h.ints["za.map.wide"]&&h.ints["za.map.choices"]>1&&!h.texts["za.mc0.name"].empty(),
          "map group expands a list of named choices");
    Check(ext->on_action(m,"map_choice",0),"an individual marker opens from the group");
    api->sample(m,&host);
    Check(!h.ints["za.map.wide"]&&!h.ints["za.map.choices"],"choosing a marker shows only its details");
    ext->on_action(m,"map_panel",0);api->sample(m,&host);
    Check(h.ints["za.map.wide"],"closing the details expands the map");

    h.texts["__page"]="dex";
    ext->on_action(m,"dex_gen",1);
    ext->on_action(m,"dex_filter",0);
    api->sample(m,&host);
    int wild_target=-1,pikachu=-1;
    for(int i=0;i<h.ints["za.dex.rows"];++i){
        const auto prefix="za.d"+std::to_string(i)+".";
        const auto species=h.ints[prefix+"sp"];
        if(species==25) pikachu=i;
        if(h.ints[prefix+"state"]<2&&std::ranges::any_of(za::kZoneEncounters,[&](const auto& e){return e.species==species;}))
            wild_target=i;
    }
    if(wild_target>=0){
        ext->on_action(m,"dex_pick",wild_target);
        Check(ext->on_action(m,"guide_dex_pin",0),"capture target can be pinned");
        Check(ext->on_action(m,"guide_capture",0),"capture plan opens");
        h.texts["__page"]="capture";api->sample(m,&host);
        Check(h.ints["za.capture.rows"]>0&&h.ints["za.cp0.count"]==1&&!h.texts["za.cp0.names"].empty(),
              "capture plan links a real uncaught species to its wild zones");
        Check(ext->on_action(m,"guide_capture_map",0),"capture row links to the map");
        h.texts["__page"]="map";api->sample(m,&host);
        Check(h.ints["za.zone.selected"]==h.ints["za.cp0.zone"],"capture route focuses the selected wild zone");
    }else{
        h.texts["__page"]="capture";api->sample(m,&host);
        Check(!h.ints["za.capture.targets"],"completed collection has no outstanding targets");
    }
    Check(pikachu>=0,"main dex has Pikachu");
    if(pikachu>=0){
        ext->on_action(m,"dex_pick",pikachu);
        Check(ext->on_action(m,"guide_evolutions",0),"Pikachu evolution guide opens");
        h.texts["__page"]="evolutions";api->sample(m,&host);
        int stone=-1;
        for(int i=0;i<h.ints["za.guide.evolution_rows"];++i)
            if(h.ints["za.ge"+std::to_string(i)+".item"]==83&&
               h.texts["za.ge"+std::to_string(i)+".condition"].find("Pierre Foudre")!=std::string::npos) stone=i;
        Check(stone>=0,"Pikachu requires a Thunder Stone");
        if(stone>=0){
            const auto stock=h.ints["za.ge"+std::to_string(stone)+".stock"];
            Check(ext->on_action(m,"guide_item",stone),"guide opens its own linked item");
            h.texts["__page"]="bag";api->sample(m,&host);
            Check(h.ints["za.bag.guide"]&&h.ints["za.item.id"]==83&&h.ints["za.item.count"]==stock&&
                  !h.texts["za.item.description"].empty(),"bag shows the correct evolution item and stock");
            Check(ext->on_action(m,"bag_help",4),"evolution item opens its reverse compatibility list");
            h.texts["__page"]="bag_tools";api->sample(m,&host);
            Check(h.ints["za.bagtool.evolve"]&&h.ints["za.bagtool.item"]==83,"evolution tool keeps the stone identity");
            for(int i=0;i<h.ints["za.bagtool.rows"];++i){
                const auto prefix="za.bt"+std::to_string(i)+".";
                Check(h.ints[prefix+"eligible"]&&!h.texts[prefix+"context"].empty(),"evolution matches identify the team or box location");
            }
            Check(ext->on_action(m,"guide_item_back",0),"item can return to the evolution guide");
            h.texts["__page"]="evolutions";api->sample(m,&host);
        }
    }
    Check(ext->on_action(m,"search_open",0),"dex search opens");
    h.texts["__page"]="search";api->sample(m,&host);
    const auto search_letter=[&](const std::string& letter){
        for(int i=0;i<h.ints["za.search.keys"];++i) if(h.texts["za.sk"+std::to_string(i)+".key"]==letter)
            return bool(ext->on_action(m,"search_key",i));
        return false;
    };
    for(const auto letter:{"M","E","G","A","N"}){
        Check(search_letter(letter),"search accepts a keyboard letter");
        api->sample(m,&host);
    }
    int meganium=-1;
    for(int i=0;i<h.ints["za.search.rows"];++i) if(h.texts["za.sr"+std::to_string(i)+".name"]=="Méganium"){meganium=i;break;}
    if(meganium<0){
        std::printf("Search '%s' has %lld results\n",h.texts["za.search.query"].c_str(),h.ints["za.search.rows"]);
        for(int i=0;i<h.ints["za.search.rows"];++i) std::printf("  %s\n",h.texts["za.sr"+std::to_string(i)+".name"].c_str());
    }
    Check(meganium>=0&&ext->on_action(m,"search_pick",meganium),"accent-insensitive search opens matching Pokémon");
    h.texts["__page"]="dex";api->sample(m,&host);
    Check(h.ints["za.detail.sp"]==154,"search opens and selects the correct entry");
    Check(ext->on_action(m,"search_open",1)&&ext->on_action(m,"search_clear",0),"box search starts a new query");
    h.texts["__page"]="search";api->sample(m,&host);
    if(box_slot>=0){
        const auto current_box=h.ints["za.box.view1"];
        const auto species=h.ints["za.b"+std::to_string(box_slot)+".sp"];
        for(char digit:std::to_string(species)) Check(ext->on_action(m,"search_digit",digit-'0'),"numeric search key");
        api->sample(m,&host);
        int result=-1;
        const auto expected="BOÎTE "+std::to_string(current_box)+" · Case "+std::to_string(box_slot+1);
        for(int i=0;i<h.ints["za.search.rows"];++i)
            if(h.texts["za.sr"+std::to_string(i)+".context"]==expected){result=i;break;}
        Check(result>=0&&ext->on_action(m,"search_pick",result),"box search selects the exact saved cell");
        h.texts["__page"]="box_detail";api->sample(m,&host);
        Check(h.texts["za.boxdetail.position"]==expected&&h.ints["za.boxdetail.sp"]==species,
              "box result opens the matching creature and displays its position");
    }
    h.texts["__page"]=before_guide;
    h.texts["__page"]="bag";
    ext->on_action(m,"bag_category",-1);
    api->sample(m,&host);
    const auto first_item=h.ints["za.item.id"];
    Check(first_item>0&&ext->on_action(m,"bag_favor",0),"stocked item can be added to favorites");
    api->sample(m,&host);
    Check(h.ints["za.bf0.id"]==first_item&&h.ints["za.bf0.pending"]&&
          h.ints["za.bf0.value"]==first_item&&h.ints["za.bf0.count"]>0,"favorite publishes its exact persistent item ID and stock");
    h.ints["@flag:za_bag_favorite_0"]=first_item;
    api->sample(m,&host);
    Check(!h.ints["za.bf0.pending"]&&ext->on_action(m,"bag_favorite_pick",0),"persistent favorite opens its exact item");
    api->sample(m,&host);
    Check(h.ints["za.item.id"]==first_item&&h.ints["za.bag.fav_remove"],"favorite detail offers removal");
    Check(ext->on_action(m,"bag_favor",0),"favorite can be removed");
    api->sample(m,&host);
    Check(!h.ints["za.bf0.id"]&&h.ints["za.bf0.pending"]&&!h.ints["za.bf0.value"],"removal clears the persisted slot");
    h.ints["@flag:za_bag_favorite_0"]=0;
    const auto pick_item=[&](int id){
        h.texts["__page"]="bag";
        ext->on_action(m,"bag_category",-1);
        api->sample(m,&host);
        for(int i=0;i<h.ints["za.bag.rows"];++i){
            if(h.ints["za.i"+std::to_string(i)+".id"]==id){
                const auto picked=ext->on_action(m,"bag_pick",i);
                api->sample(m,&host);
                return bool(picked);
            }
        }
        return false;
    };
    if(pick_item(17)||pick_item(26)||pick_item(25)){
        Check(ext->on_action(m,"bag_help",1),"HP medicine opens the healing target list");
        h.texts["__page"]="bag_tools";api->sample(m,&host);
        bool damaged=false;
        for(int i=0;i<h.ints["za.bagtool.rows"];++i){
            const auto prefix="za.bt"+std::to_string(i)+".";
            const auto hp=h.ints[prefix+"hp"], maximum=h.ints[prefix+"hpmax"];
            Check(bool(h.ints[prefix+"eligible"])==(hp>0&&hp<maximum),"potions highlight living injured Pokémon");
            damaged|=hp>0&&hp<maximum;
        }
        Check(damaged&&!h.ints["za.bagtool.hp_live"],"saved HP is identified when live HP is unavailable");
    }
    if(pick_item(28)){
        Check(ext->on_action(m,"bag_help",1),"Revive opens the healing target list");
        h.texts["__page"]="bag_tools";api->sample(m,&host);
        for(int i=0;i<h.ints["za.bagtool.rows"];++i){
            const auto prefix="za.bt"+std::to_string(i)+".";
            Check(bool(h.ints[prefix+"eligible"])==(h.ints[prefix+"hp"]==0),"Revive only highlights fainted Pokémon");
        }
    }
    ext->on_action(m,"team_select",4);
    h.texts["__page"]="team";
    Check(ext->on_action(m,"team_heal_open",0),"team can open compatible medicines for the selected Pokémon");
    api->sample(m,&host);
    Check(h.ints["za.guide.page"]==11,"team healing opens its own page");
    h.texts["__page"]="team_heal";api->sample(m,&host);
    Check(h.ints["za.heal.target"]&&h.ints["za.heal.rows"]>0&&h.ints["za.hm0.stock"]&&
          h.texts["za.heal.name"]==h.texts["za.sel.name"],"in-stock medicine is shown with the chosen Pokémon");
    Check(!ext->on_action(m,"team_heal_pick",0),"an invalid item does not open the healing action");
    Check(ext->on_action(m,"team_heal_pick",h.ints["za.hm0.id"]),"a compatible stocked medicine opens confirmation");
    api->sample(m,&host);
    h.texts["__page"]="bag_tools";api->sample(m,&host);
    Check(h.ints["za.bagtool.from_team"]&&h.ints["za.bagtool.rows"]==1&&
          h.texts["za.bagtool.target_name"]==h.texts["za.heal.name"],"confirmation keeps the same Pokémon");
    Check(ext->on_action(m,"team_heal_back",0),"confirmation can return to the team medicine list");
    ext->on_action(m,"team_select",5);
    Check(ext->on_action(m,"bag_hold_team",0),"team detail opens its held item comparison");
    h.texts["__page"]="bag_tools";api->sample(m,&host);
    Check(h.ints["za.bagtool.compare"]&&h.ints["za.bagtool.rows"]==1&&
          h.texts["za.bagtool.target_name"]==h.texts["za.sel.name"],"held item screen keeps the chosen party member");
    if(box_slot>=0){
        Check(ext->on_action(m,"bag_hold_box",0),"boxed detail opens its held item comparison");
        api->sample(m,&host);
        Check(h.ints["za.bagtool.compare"]&&h.ints["za.bagtool.rows"]==1&&
              h.texts["za.bt0.context"]==h.texts["za.boxdetail.position"],"held item screen keeps the exact box cell");
    }
    ext->on_action(m,"bag_hold_end",0);
    h.texts["__page"]="bag";
    ext->on_action(m,"bag_category",6);
    api->sample(m,&host);
    const int machine_rows=h.ints["za.bag.rows"];
    bool machine_opened=false;
    for(int i=0;i<machine_rows&&!machine_opened;++i){
        ext->on_action(m,"bag_pick",i);
        if(!ext->on_action(m,"bag_help",3)) continue;
        h.texts["__page"]="bag_tools";api->sample(m,&host);
        for(int j=0;j<h.ints["za.bagtool.rows"];++j){
            if(!h.ints["za.bt"+std::to_string(j)+".eligible"]) continue;
            const auto move_name=h.texts["za.bagtool.move"];
            Check(ext->on_action(m,"bag_target",j)&&ext->on_action(m,"moves_bag",0),"compatible TM opens the moves screen");
            h.texts["__page"]="moves";api->sample(m,&host);
            Check(h.ints["za.moves.frombag"]&&h.texts["za.moves.name"]==move_name,"CT remains preselected for the exact Pokémon");
            Check(ext->on_action(m,"moves_slot",2),"CT flow allows choosing a replacement slot");
            api->sample(m,&host);
            Check(h.texts["za.moves.name"]==move_name&&!h.texts["za.moves.compare"].empty(),"choosing a slot preserves the CT and exposes a comparison");
            machine_opened=true;
            break;
        }
        h.texts["__page"]="bag";
    }
    Check(machine_opened,"a compatible TM is present in the fixture");
    h.texts["__page"]=before_guide;
    Check(!ext->on_action(m,"center_travel",0)&&!ext->on_action(m,"box_pick",0),"menu-driven travel and exchange stay disabled");
    Check(ext->on_action(m,"moves_team",0),"party move menu opens from a valid saved entity");api->sample(m,&host);
    Check(h.ints["za.moves.rows"]>0&&!h.texts["za.moves.name"].empty()&&!h.texts["za.moves.description"].empty(),"move names and details decoded from game assets");
    int damaging_attacks=0;
    auto detail_reset=h.ints["za.moves.detail_reset"];
    for(int slot=0;slot<4;++slot){
        const auto prefix="za.sel.m"+std::to_string(slot)+".";
        if(!h.ints[prefix+"id"]) continue;
        Check(ext->on_action(m,"moves_slot",slot),"an equipped attack can be inspected in the move menu");
        api->sample(m,&host);
        for(const std::string stat:{"category","power","accuracy"}){
            Check(!h.texts[prefix+stat].empty()&&h.texts[prefix+stat]==h.texts["za.moves."+stat],
                  "the team attack stats match the move details");
        }
        if(h.texts[prefix+"power"]!="—"&&h.texts[prefix+"accuracy"].ends_with("%")) ++damaging_attacks;
    }
    Check(damaging_attacks>0&&h.ints["za.moves.detail_reset"]!=detail_reset,
          "switching equipped attacks updates the stats and resets the scroll");
    Check(!h.ints["za.moves.can_apply"]&&!h.ints["za.moves.target"],"save-only data never authorizes a native move edit");
    Check(!ext->on_action(m,"moves_slot",4)&&!ext->on_action(m,"moves_pick",192),"move selection rejects invalid indexes");
    Check(ext->on_action(m,"moves_box",0),"boxed move details open");api->sample(m,&host);
    Check(h.ints["za.moves.frombox"]&&!h.ints["za.moves.can_apply"],"boxed move menu is explicitly read-only until native commit is verified");
    std::printf("exclusive selection + XP + four-tab navigation + box detail/radar + progression: OK\n");

    // An unchanged save must cost a size query plus the 0x20-byte trailer, never a 3 MiB read.
    const auto before = h.bytes_read;
    h.tick += 300;
    api->tick(m, &host);
    const auto unchanged = h.bytes_read - before;
    std::printf("unchanged save: %llu byte(s) re-read\n", unchanged);
    Check(unchanged <= 0x40, "unchanged save costs a tail read only");
    api->sample(m, &host);
    Check(h.ints["za.ready"] == 1, "ready survives a skipped poll");

    // Browsing: next, then back, then follow the game's box again.
    const long long first = h.ints["za.box.view1"];
    Check(ext->on_action(m, "box_next", 0) == EDEN_DSMOD_TRUE, "box_next accepted");
    api->sample(m, &host);
    Check(h.ints["za.box.view1"] == first + 1, "box_next advances");
    Check(h.ints["za.box.follow"] == 0, "browsing stops following");
    Check(ext->on_action(m, "box_prev", 0) == EDEN_DSMOD_TRUE, "box_prev accepted");
    api->sample(m, &host);
    Check(h.ints["za.box.view1"] == first, "box_prev returns");
    h.texts["__page"]="box";api->sample(m,&host);
    Check(ext->on_action(m,"box_picker",0),"number opens box picker");
    api->sample(m,&host);
    Check(h.ints["za.box.picker"]&&h.ints["za.box.tile"+std::to_string(first-1)+".selected"],
          "picker highlights the displayed box");
    Check(!ext->on_action(m,"box_set",32)&&ext->on_action(m,"box_set",4),"picker bounds and direct selection");
    api->sample(m,&host);
    Check(!h.ints["za.box.picker"]&&h.ints["za.box.view1"]==5,"direct selection closes the picker");
    Check(ext->on_action(m, "box_follow", 0) == EDEN_DSMOD_TRUE, "box_follow accepted");
    api->sample(m, &host);
    Check(h.ints["za.box.follow"] == 1, "follow restored");
    Check(ext->on_action(m, "refresh", 0) == EDEN_DSMOD_TRUE, "refresh accepted");
    Check(ext->on_action(m, "nonsense", 0) == EDEN_DSMOD_FALSE, "unknown action refused");

    // A rewritten save (same size, different trailer) must trigger a full re-read.
    {
        const std::string changed = h.save_path + ".changed";
        std::FILE* in = std::fopen(h.save_path.c_str(), "rb");
        std::vector<unsigned char> all;
        if (in) {
            std::fseek(in, 0, SEEK_END);
            all.resize(static_cast<size_t>(std::ftell(in)));
            std::fseek(in, 0, SEEK_SET);
            const size_t got = std::fread(all.data(), 1, all.size(), in);
            all.resize(got);
            std::fclose(in);
        }
        Check(!all.empty(), "save copy readable");
        all.back() ^= 0xFF;
        std::FILE* out = std::fopen(changed.c_str(), "wb");
        if (out) {
            std::fwrite(all.data(), 1, all.size(), out);
            std::fclose(out);
        }
        h.save_path = changed;
        const auto before_change = h.bytes_read;
        h.tick += 300;
        api->tick(m, &host);
        api->sample(m, &host);
        Check(h.bytes_read - before_change >= all.size(), "rewritten save is read in full");
        Check(h.texts["za.status"] == "somme de controle invalide", "rewritten save flagged");
        std::printf("rewritten save status: %s\n", h.texts["za.status"].c_str());
    }

    // A save that is not there must degrade to "introuvable", never crash.
    h.save_path = "Z:/does/not/exist";
    ext->on_action(m, "refresh", 0);
    api->sample(m, &host);
    Check(h.ints["za.ready"] == 0, "missing save clears ready");
    std::printf("missing save status: %s\n", h.texts["za.status"].c_str());

    api->destroy(m);
    std::printf(failures == 0 ? "za-module-host: ok\n" : "za-module-host: %d failure(s)\n",
                failures);
    return failures == 0 ? 0 : 1;
}
