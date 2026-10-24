// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_assets.h"
#include "acnh_tex.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#ifdef ECHOES_TEST_NATIVE_ASTC
#include "video_core/textures/astc.h"
#endif
#ifdef ECHOES_TEST_PNG
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <stb_image_write.h>
#pragma GCC diagnostic pop
#endif

namespace {
int checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct File {
    std::filesystem::path path;
    uint64_t begin=0, size=0;
    size_t Read(uint64_t at, void* out, size_t length) const {
        if (!out && !length) return size;
        if (at>size || length>size-at) return 0;
        std::ifstream f(path,std::ios::binary);f.seekg(static_cast<std::streamoff>(begin+at));
        f.read(static_cast<char*>(out),static_cast<std::streamsize>(length));
        return static_cast<size_t>(f.gcount());
    }
};
uint32_t U32(const uint8_t* p) {return p[0]|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
uint16_t U16(const uint8_t* p) {return uint16_t(p[0]|uint16_t(p[1])<<8);}
File NativeFile(std::string_view archive="GameMain.arc") {
    const char* variable=archive=="GameMain.arc"?"ECHOES_GAMEMAIN_BNTX":archive=="IconMapPin.arc"?"ECHOES_MAPPIN_BNTX":"ECHOES_ITEM_BNTX";
    if(const char* p=std::getenv(variable)) return {p,0,std::filesystem::file_size(p)};
    const char* root=std::getenv("ECHOES_ROMFS");
    if(!root) return {};
    File arc{std::filesystem::path(root)/"region_common/ui"/archive,0,0};
    arc.size=std::filesystem::file_size(arc.path);
    std::array<uint8_t,20> header{};
    Check(arc.Read(0,header.data(),20)==20 && !std::memcmp(header.data(),"SARC",4),"SARC header");
    Check(U16(header.data()+6)==0xfeff,"little-endian SARC");
    const uint32_t data=U32(header.data()+12), declared=U32(header.data()+8);
    Check(declared<=arc.size && data<=declared,"SARC range");
    std::array<uint8_t,12> fat{};
    const uint16_t offset=U16(header.data()+4);
    Check(arc.Read(offset,fat.data(),fat.size())==fat.size() && !std::memcmp(fat.data(),"SFAT",4),"SFAT header");
    const uint16_t n=U16(fat.data()+6);
    Check(n<=30000,"SARC count");
    const uint64_t names=offset+12+uint64_t(n)*16+8;
    for(unsigned i=0;i<n;++i) {
        std::array<uint8_t,16> node{};
        Check(arc.Read(offset+12+uint64_t(i)*16,node.data(),16)==16,"SARC node");
        const uint32_t attr=U32(node.data()+4), first=U32(node.data()+8), last=U32(node.data()+12);
        Check(first<=last && uint64_t(data)+last<=declared,"SARC member range");
        if(!(attr>>24)) continue;
        const uint64_t at=names+uint64_t(attr&0xffffff)*4;
        if(at>=data) continue;
        std::array<char,512> name{};
        const size_t bytes=std::min<uint64_t>(name.size()-1,data-at);
        Check(arc.Read(at,name.data(),bytes)==bytes,"SARC name");
        if(std::string_view(name.data())=="timg/__Combined.bntx") return {arc.path,data+first,last-first};
    }
    throw std::runtime_error("GameMain BNTX absent");
}
#ifdef ECHOES_TEST_NATIVE_ASTC
EdenDsmodBool Decode(void*, uint32_t w,uint32_t h,uint32_t bw,uint32_t bh,
                     const uint8_t* in,size_t ni,uint8_t* out,size_t no) {
    if(no!=uint64_t(w)*h*4) return EDEN_DSMOD_FALSE;
    Tegra::Texture::ASTC::Decompress({in,ni},w,h,1,bw,bh,{out,no});
    return EDEN_DSMOD_TRUE;
}
#endif
void Save(std::string_view key,const echoes::Image& image) {
#ifdef ECHOES_TEST_PNG
    if(const char* p=std::getenv("ECHOES_ASSET_OUTPUT")) {
        auto path=std::filesystem::path(p);std::filesystem::create_directories(path);
        std::string name(key);for(auto& c:name) if(c=='/')c='-';
        path/=name+".png";
        Check(stbi_write_png(path.c_str(),image.w,image.h,4,image.rgba.data(),image.w*4)!=0,"PNG output");
    }
#else
    (void)key;(void)image;
#endif
}
} // namespace
int main() {
    try {
        int calls=0;
        echoes::Assets empty([&](std::string_view,uint64_t,void*,size_t){++calls;return size_t(0);});
        echoes::Image image(3,3);
        for(const auto key:{"","background/","divider/2","cell/0/1","cell/95/164","cell/1241/164",
                            "cell/216/1081","cell/+216/164","cell/0216/164","cell/216/164/x",
                            "cell/999999999999999/164","cursor/61/164","cursor/216/-1","pin/0","pin/7","pin/01","pin/1/x","rift-fill/x","nav/4","nav/00","nav-dim/9","disc/196/150",
                            "badge/z/76/76","badge/y/76/70","button/79/144","bottom-button/79/144","bottom-button/286/0","favorite/2","sort-dot/3","line/500/41","actor/47/224","actor-focus/268/0","actor/268/224/x"}) {
            Check(!empty.Load(key,image) && image.Empty() && image.w==0 && image.h==0,"invalid key clears image");
        }
        Check(calls==0,"malformed dimensions must not read RomFS");
        Check(empty.Load("favorite-remove",image),"requested remove-favorite action glyph loads");
        Check(image.w==96 && image.h==96,"remove-favorite glyph canvas");
        int trash_left=96,trash_top=96,trash_right=-1,trash_bottom=-1;
        for(int y=0;y<image.h;++y)for(int x=0;x<image.w;++x)if(image.At(x,y)[3]) {
            trash_left=std::min(trash_left,x);trash_right=std::max(trash_right,x);trash_top=std::min(trash_top,y);trash_bottom=std::max(trash_bottom,y);
        }
        Check(trash_right-trash_left+1>=40 && trash_bottom-trash_top+1>=40,"remove-favorite symbol has legible ink bounds");
        Check(trash_left>=8 && trash_top>=8 && trash_right<88 && trash_bottom<88,"remove-favorite glyph retains transparent touch padding");
        Check(image.At(48,53)[3]==0,"remove-favorite bin remains hollow");
        Check(calls==0,"functional remove glyph does not require or substitute game artwork");
        Check(!empty.Load("background",image) && image.Empty(),"missing RomFS fails closed");
        for(const auto key:{"rift-fill","pin/1","pin/6","held-background","footer","nav/0","nav/1","nav/2","nav/3","nav-dim/0",
                            "button/286/144","bottom-button/286/144","favorite-on","favorite-off","sort-dot-on","sort-dot-off","disc/196/196","badge/y/76/76","panel/670/832",
                            "line/560/40","selectionglow","dictionary","name-line","actor/268/224",
                            "actor-focus/268/224","sort-banner","name-banner"})
            Check(!empty.Load(key,image) && image.Empty(),"new native components require original resources");
        int body_reads=0;
        echoes::Assets oversize([&](std::string_view,uint64_t,void* p,size_t){if(p)++body_reads;return size_t(32u<<20)+1;});
        Check(!oversize.Load("background",image) && body_reads==0,"oversize source rejected before read");
        echoes::Assets shortread([](std::string_view,uint64_t,void* p,size_t){return p?size_t(12):size_t(16);});
        Check(!shortread.Load("divider",image) && image.Empty(),"short source read rejected");
        echoes::Assets malformed([](std::string_view,uint64_t,void* p,size_t n){if(p)std::memset(p,0,n);return p?n:size_t(64);});
        Check(!malformed.Load("divider",image) && image.Empty(),"malformed BNTX rejected");
        const File file=NativeFile();
        if(file.path.empty()) {std::cout<<checks<<" checks passed; native fixtures skipped (set ECHOES_ROMFS)\n";return 0;}
#ifdef ECHOES_TEST_NATIVE_ASTC
        acnh::SetAstcDecoder(nullptr,Decode);
#endif
        int queries=0,reads=0;
        echoes::Assets assets([&](std::string_view path,uint64_t at,void* p,size_t n){
            Check(path=="region_common/ui/GameMain.arc#timg/__Combined.bntx","native asset source");
            if(p)++reads;else ++queries;
            return file.Read(at,p,n);
        });
        for(const auto key:{"background","divider","player","cell/216/164","cell/260/120","cursor/216/164","cursor/260/120"}) {
            Check(assets.Load(key,image),"native asset load");Save(key,image);
            if(std::string_view(key)=="background") {
                Check(image.w==1240 && image.h==1080,"background dimensions");
                bool opaque=true;for(size_t i=3;i<image.rgba.size();i+=4)opaque &= image.rgba[i]==255;
                Check(opaque,"background opaque");
            }
            if(std::string_view(key)=="divider") Check(image.w==1064 && image.h==46,"native divider dimensions");
            if(std::string_view(key)=="player") Check(image.w==64 && image.h==64,"native player marker dimensions");
        }
        struct Component {const char* key;int w,h;};
        for(const auto& component:std::array<Component,36>{{
            {"warp",56,56},{"warp-dark",56,56},{"rift-fill",256,256},{"held-background",1240,1080},{"footer",1240,208},{"button/286/144",286,144},
            {"bottom-button/286/144",286,144},{"bottom-button/373/118",373,118},{"bottom-button/1180/118",1180,118},
            {"button/268/112",268,112},{"button/392/112",392,112},{"panel/962/1064",962,1064},{"favorite-on",48,48},{"favorite-off",48,48},
            {"sort-dot-on",32,32},{"sort-dot-off",32,32},
            {"disc/196/196",196,196},{"badge/x/76/76",76,76},{"badge/y/76/76",76,76},
            {"badge/up/76/76",76,76},{"panel/670/832",670,832},{"panel/1192/290",1192,290},
            {"line/560/40",560,40},
            {"nav/0",80,80},{"nav/1",80,80},{"nav/2",80,80},{"nav/3",80,80},
            {"selectionglow",216,110},{"dictionary",670,830},{"name-line",618,40},
            {"actor/268/224",268,224},{"actor-focus/268/224",268,224},
            {"sort-banner",650,135},{"name-banner",1240,128},{"item-name-line",1144,40},{"equipped-check",64,64}}}) {
            Check(assets.Load(component.key,image),"original component fixture load");
            Check(image.w==component.w && image.h==component.h,"component presentation dimensions");
            bool visible=false;
            for(size_t i=3;i<image.rgba.size();i+=4)visible |= image.rgba[i]!=0;
            Check(visible,"original component has visible artwork");Save(component.key,image);
        }
        Check(queries==1 && reads==1,"container cached across assets");
        Check(assets.Load("cell/216/164",image),"cell cached load");
        Check(image.w==216 && image.h==164,"cell dimensions");
        std::vector<uint8_t> bntx(file.size);
        Check(file.Read(0,bntx.data(),bntx.size())==bntx.size(),"fixture native source read");
        echoes::Image original_warp,dark_warp;
        Check(acnh::BntxDecode(bntx,"Warp_00^q",original_warp),"fixture original native warp glyph");
        Check(assets.Load("warp",image) && image.rgba==original_warp.rgba,"map warp glyph preserves exact original pixels");
        Check(assets.Load("warp-dark",dark_warp) && dark_warp.w==original_warp.w && dark_warp.h==original_warp.h,"header warp variant retains native dimensions");
        for(size_t i=0;i<original_warp.rgba.size();i+=4)
            Check(dark_warp.rgba[i]==48 && dark_warp.rgba[i+1]==46 && dark_warp.rgba[i+2]==61 && dark_warp.rgba[i+3]==original_warp.rgba[i+3],"header dark glyph preserves native alpha and text palette");
        echoes::Image original_rift;
        Check(acnh::BntxDecode(bntx,"Map_FieldCrackActor^w",original_rift),"fixture original map-rift fill");
        Check(assets.Load("rift-fill",image) && image.rgba==original_rift.rgba,"rift-fill preserves exact original256px material");
        const File pin_file=NativeFile("IconMapPin.arc");
        if(!pin_file.path.empty()) {
            std::vector<uint8_t> original_pins(pin_file.size);
            Check(pin_file.Read(0,original_pins.data(),original_pins.size())==original_pins.size(),"fixture original IconMapPin source");
            int pin_queries=0,pin_reads=0;
            echoes::Assets pin_assets([&](std::string_view path,uint64_t at,void* out,size_t length){
                Check(path=="region_common/ui/IconMapPin.arc#timg/__Combined.bntx","pin resolver uses exact native archive");
                if(out)++pin_reads;else ++pin_queries;return pin_file.Read(at,out,length);
            });
            constexpr std::array<const char*,6> pin_names={"MapFlag_00","MapFlag_01","MapFlag_02","MapPin_02","MapPin_03","MapPin_04"};
            for(size_t i=0;i<pin_names.size();++i) {
                echoes::Image original;
                Check(acnh::BntxDecode(original_pins,pin_names[i],original),"fixture requested original pin exists");
                const std::string key="pin/"+std::to_string(i+1);
                Check(pin_assets.Load(key,image) && image.w==60 && image.h==60 && image.rgba==original.rgba,"companion pin kind matches exact original sprite");
                Save(key,image);
            }
            Check(pin_queries==1 && pin_reads==1,"pin source cached across all six kinds");
        }
        const File item_file=NativeFile("IconItem.arc");
        if(!item_file.path.empty()) {
            Check(item_file.size>0 && item_file.size<=(32u<<20),"prologue icon fixture source bounded");
            std::vector<uint8_t> item_textures(item_file.size);
            Check(item_file.Read(0,item_textures.data(),item_textures.size())==item_textures.size(),"original IconItem fixture source read");
            for(const auto name:{"Sword_01","Item_Shield","Arrow_01","Bomb_01","HeartPiece_00"}) {
                echoes::Image original;
                Check(acnh::BntxDecode(item_textures,name,original),"original prologue item texture decodes through inventory BNTX path");
                Check(!original.Empty() && original.w>0 && original.h>0 && original.w<=512 && original.h<=512,"prologue icon native dimensions bounded");
                bool ink=false;
                for(size_t i=3;i<original.rgba.size();i+=4)ink |= original.rgba[i]!=0;
                Check(ink,"original prologue item artwork has visible alpha");
                Save(std::string("prologue/")+name,original);
                std::cout<<"Prologue native icon "<<name<<": "<<original.w<<'x'<<original.h<<" visible\n";
            }
        }
        // Native category masks retain the authored icon silhouettes, including transparent
        // margins. Tinting must not turn them into opaque rectangular app controls.
        constexpr std::array<const char*,4> categories={"HeaderTab_05^w","HeaderTab_02^w","HeaderTab_00^w","HeaderTab_04^w"};
        for(size_t k=0;k<categories.size();++k) {
            echoes::Image original;
            Check(acnh::BntxDecode(bntx,categories[k],original),"fixture category source exists");
            Check(assets.Load("nav/"+std::to_string(k),image),"category component load");
            bool same_alpha=original.w==image.w && original.h==image.h;
            for(size_t i=3;same_alpha && i<image.rgba.size();i+=4)
                same_alpha &= original.rgba[i]==image.rgba[i];
            Check(same_alpha,"category keeps original icon silhouette");
        }
        Check(assets.Load("background",image),"quiet ordinary material loads");
        const auto ordinary=image.rgba;
        Check(assets.Load("held-background",image) && image.rgba==ordinary,"ordinary and held material remains seamless");
        echoes::Image native_star;
        Check(acnh::BntxDecode(bntx,"ItemFavo_00^w",native_star),"fixture original favorite star");
        Check(assets.Load("favorite-on",image) && image.rgba==native_star.rgba,"favorite on preserves original sprite");
        Check(assets.Load("favorite-off",image),"derived inactive favorite loads");
        bool muted=true;
        for(size_t i=0;i<image.rgba.size();i+=4)
            muted &= image.rgba[i]==image.rgba[i+1] && image.rgba[i+1]==image.rgba[i+2] && image.rgba[i+3]<=native_star.rgba[i+3];
        Check(muted,"inactive favorite outlines original white-star silhouette");
        Check(image.At(24,24)[3]==0,"inactive favorite has an empty star center");
        for(const auto key:{"sort-dot-on","sort-dot-off"}) {
            echoes::Image native_dot;
            Check(acnh::BntxDecode(bntx,std::string_view(key)=="sort-dot-on"?"PageCount_01^t":"PageCount_00^t",native_dot),"fixture original page-counter dot");
            if(std::string_view(key)=="sort-dot-off")for(size_t i=0;i<native_dot.rgba.size();i+=4)
                native_dot.rgba[i]=native_dot.rgba[i+1]=native_dot.rgba[i+2]=104;
            Check(assets.Load(key,image) && image.rgba==native_dot.rgba,"sort dot preserves original alpha and native selected/inactive material");
        }
        echoes::Image native_sort;
        Check(acnh::BntxDecode(bntx,"ActerSortBG_00^w",native_sort),"fixture original sort-banner pattern");
        Check(assets.Load("sort-banner",image),"white-label sort-banner loads");
        Check(image.rgba==native_sort.rgba,"sort banner matches original main-screen material without darkening");
        Check(assets.Load("bottom-button/286/144",image),"flush bottom control loads");
        bool mirrored=true;
        for(int y=0;y<image.h;++y)for(int x=0;x<image.w/2;++x)
            mirrored &= !std::memcmp(image.At(x,y),image.At(image.w-1-x,y),4);
        Check(mirrored,"native capsule footer retains mirrored end caps");
        Check(image.At(143,100)[3]>240 && image.At(143,100)[0]==98,
              "filled capsule provides opaque paper behind dark labels");
        unsigned bottom_coverage=0;
        for(int x=30;x<256;++x)bottom_coverage+=image.At(x,143)[3];
        Check(bottom_coverage>0,"flush bottom control extends its native fill to canvas edge");
        echoes::Image half;
        Check(acnh::BntxDecode(bntx,"ActerBG_00^w",half),"fixture original ActorSelect half pane");
        Check(assets.Load("actor/268/224",image),"ActorSelect pane assembled");
        bool actor_corners=true;
        for(int y=0;y<24;++y)for(int x=0;x<24;++x) {
            actor_corners &= !std::memcmp(image.At(x,y),half.At(x,y),4);
            actor_corners &= !std::memcmp(image.At(267-x,y),half.At(x,y),4);
            actor_corners &= !std::memcmp(image.At(x,223-y),half.At(x,235-y),4);
        }
        Check(actor_corners,"ActorSelect preserves original24px corner art and mirrored half role");
        echoes::Image ribbon;
        Check(acnh::BntxDecode(bntx,"ActerNameBG_00^w",ribbon),"fixture original selected-name ribbon");
        Check(assets.Load("name-banner",image),"selected-name ribbon composed");
        bool tiled=true;
        for(int y=0;y<128;++y)for(int x=0;x<1240;++x)
            tiled &= !std::memcmp(image.At(x,y),ribbon.At(x%ribbon.w,y),4);
        Check(tiled,"selected-name ribbon retains original pattern wavelength");
        // Remove only one texture's native name from an otherwise valid owned fixture.
        // The optional component must fail closed without poisoning unrelated native art.
        auto missing=bntx;
        constexpr std::string_view ribbon_name="ActerNameBG_00^w";
        auto name=std::search(missing.begin(),missing.end(),ribbon_name.begin(),ribbon_name.end());
        Check(name!=missing.end(),"fixture selected-name texture metadata");*name='X';
        echoes::Assets absent([&](std::string_view,uint64_t at,void* out,size_t length){
            if(!out && !length)return missing.size();
            if(at>missing.size() || length>missing.size()-at)return size_t(0);
            std::memcpy(out,missing.data()+at,length);return length;
        });
        Check(!absent.Load("name-banner",image) && image.Empty(),"missing required component texture fails closed");
        Check(absent.Load("player",image),"missing optional component leaves other native resources usable");
        Check(assets.Load("cell/216/164",image),"restore cell for native frame fixture checks");
        echoes::Image upper, lower;
        Check(acnh::BntxDecode(bntx,"ItemBase_00_LT^w",upper) &&
              acnh::BntxDecode(bntx,"ItemBase_00_LB^w",lower),"fixture original frame corners");
        bool corners=true;
        for(int y=0;y<48;++y)for(int x=0;x<48;++x) {
            corners &= !std::memcmp(image.At(x,y),upper.At(x,y),4);
            corners &= !std::memcmp(image.At(x,116+y),lower.At(x,y),4);
        }
        Check(corners,"frame preserves original upper and lower corner pixels");
        bool symmetric=true;for(int y=0;y<image.h;++y)for(int x=0;x<image.w/2;++x)
            symmetric &= !std::memcmp(image.At(x,y),image.At(image.w-1-x,y),4);
        Check(symmetric,"cell native horizontal flips");
        Check(image.At(0,0)[3]==0 && image.At(215,163)[3]==0,"cell outer corners transparent");
        Check(image.At(108,82)[3]>0,"cell interior retains native fill");
        const auto old=image.rgba;
        Check(assets.Load("cursor/216/164",image),"cursor cached load");
        Check(image.At(108,82)[3]==0,"cursor center remains transparent");
        Check(image.At(0,0)[3]==0,"cursor outer margin transparent");
        unsigned top=0,bottom=0;bool cursor_sym=true;
        for(int y=0;y<image.h;++y)for(int x=0;x<image.w;++x) {
            const auto* p=image.At(x,y);
            if(y<62)top+=p[3];
            if(y>=102)bottom+=p[3];
            cursor_sym &= !std::memcmp(p,image.At(image.w-1-x,y),4);
        }
        Check(top>0 && bottom>0 && cursor_sym,"native upper/lower cursor corners and flips");
        echoes::Image native_lower_cursor;
        Check(acnh::BntxDecode(bntx,"ComCursor_01^q",native_lower_cursor),"fixture original lower-color cursor cap");
        bool lower_orientation=true;
        for(int y=0;y<62;++y)for(int x=0;x<62;++x) {
            const auto* source=native_lower_cursor.At(x,61-y);
            const auto* left=image.At(x,image.h-62+y);
            const auto* right=image.At(image.w-1-x,image.h-62+y);
            if(source[3]) {
                lower_orientation &= !std::memcmp(left,source,4);
                lower_orientation &= !std::memcmp(right,source,4);
            } else lower_orientation &= left[3]==0 && right[3]==0;
        }
        Check(lower_orientation,"native lower cursor caps flip vertically then mirror into inward-facing corners");
        // Exercise rendered-cache eviction with many valid, distinct frames.
        for(int w=1100;w<1120;++w) Check(assets.Load("cell/"+std::to_string(w)+"/1000",image),"bounded cache eviction");
        Check(assets.Load("cell/216/164",image) && image.rgba==old,"native cache remains stable after eviction");
        Check(queries==1 && reads==1,"eviction does not reload container");
        std::cout<<checks<<" checks passed with native GameMain BNTX\n";
    }catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<"\n";return 1;}
}
