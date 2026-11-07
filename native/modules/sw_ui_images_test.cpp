// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>
#include <fstream>
#include "sw_ui_images.h"
#include "sw_species_icon_table.h"
#include "sw_test_romfs.h"
namespace {
struct Received { int w=0,h=0; std::vector<std::uint8_t> rgba; };
void Sink(void* p,std::uint32_t w,std::uint32_t h,const std::uint8_t* data,std::size_t n) {
    auto& r=*static_cast<Received*>(p);r={int(w),int(h),{data,data+n}};
}
int reads=0;
std::size_t Missing(void*,const char*,std::uint64_t,void*,std::size_t) {++reads;return 0;}
}
int main(int argc,char** argv) {
    sw_ui_images::Images images;
    EdenDsmodHostApi host{};host.read_romfs=Missing;
    bool real=argc>1 && std::string_view(argv[1])=="--romfs";
    if(real) { if(!sw_test::RomfsDir()) return 77; host=sw_test::RomfsHost(); }
    for(auto key:{"disc_white","disc_black","chevron2","lock88","lock56","key_wedge","cmd_fight","cmd_pokemon","cmd_bag","cmd_run","cmd_cheer","key_x","key_y","move_max_body","form/263/1"}) {
        bool game=std::string_view(key).starts_with("cmd_") || std::string_view(key).starts_with("form/") || std::string_view(key)=="key_x" || std::string_view(key)=="key_y" || std::string_view(key)=="move_max_body";
        bool handled=false;Received r;
        std::string name="module:sw:ui/"+std::string(key);
        bool ok=images.Load(host,name.c_str(),&r,Sink,handled);
        assert(handled && ok==(!game || real));
        int before=reads;images.Load(host,name.c_str(),&r,Sink,handled);assert(reads==before);
        if(!ok) continue;
        assert(r.w>0 && r.h>0 && r.rgba.size()==std::size_t(r.w)*r.h*4);
        if(std::string_view(key).starts_with("cmd_") || std::string_view(key)=="key_x" || std::string_view(key)=="key_y")
            for(std::size_t i=0;i<r.rgba.size();i+=4) assert(r.rgba[i]==255 && r.rgba[i+1]==255 && r.rgba[i+2]==255);
        if(argc>2) {
            auto safe=std::string(key);for(auto& c:safe) if(c=='/') c='_';
            std::ofstream f(std::string(argv[2])+"/"+safe+".rgba",std::ios::binary);
            f.write(reinterpret_cast<const char*>(&r.w),sizeof r.w);f.write(reinterpret_cast<const char*>(&r.h),sizeof r.h);
            f.write(reinterpret_cast<const char*>(r.rgba.data()),r.rgba.size());
        }
    }
    bool handled=false;Received r;
    if(real) {
        // Every form/gender icon file the module can select is in this romfs.
        int files=0;
        for(const auto& v:sw_species_icon_table::kVariants)
            for(char c:{'m','f','s','n'}) {
                const int bit=c=='m'?1:c=='f'?2:c=='s'?4:8;
                if(!(v.suffixes&bit)) continue;
                char path[96];
                std::snprintf(path,sizeof path,"romfs:/bin/appli/icon_pokemon/poke_icon_%04u_%02u%c_n.bntx",unsigned(v.icon_id),unsigned(v.form),c);
                assert(host.read_romfs(host.userdata,path,0,nullptr,0)>0);++files;
            }
        assert(files>800);
        // A form with its own icon draws it; a form without one draws the species' default look.
        const auto pixels=[&](const char* key){Received x;bool h=false;assert(images.Load(host,key,&x,Sink,h)&&h);return x.rgba;};
        assert(pixels("module:sw:ui/form/555/1")!=pixels("module:sw:ui/form/555/0"));
        assert(pixels("module:sw:ui/form/849/1")!=pixels("module:sw:ui/form/849/0"));
        assert(pixels("module:sw:ui/form/618/7")==pixels("module:sw:ui/form/618/0"));
        assert(pixels("module:sw:ui/form/618/1")!=pixels("module:sw:ui/form/618/0"));
        assert(pixels("module:sw:ui/form/122/5")==pixels("module:sw:ui/form/122/1"));
    }
    assert(!images.Load(host,"unrelated",&r,Sink,handled) && !handled);
    assert(!images.Load(host,"module:sw:ui/form/12/invalid",&r,Sink,handled) && handled);
    images.Reset();int before=reads;
    if(!real) { assert(!images.Load(host,"module:sw:ui/cmd_fight",&r,Sink,handled));assert(reads>before); }
    std::puts("ui images: procedural controls, mask contract, cache/failure/reset, dispatch PASS");
}
