// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_map_assets.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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
int checks=0;
void Check(bool b,const char* message){++checks;if(!b)throw std::runtime_error(message);}
uint32_t U32(const uint8_t* p){return p[0]|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
uint16_t U16(const uint8_t* p){return uint16_t(p[0]|uint16_t(p[1])<<8);}
#ifdef ECHOES_TEST_NATIVE_ASTC
struct DecodeState {size_t calls=0;};
EdenDsmodBool Decode(void* user,uint32_t w,uint32_t h,uint32_t bw,uint32_t bh,
                     const uint8_t* data,size_t n,uint8_t* out,size_t bytes) {
    auto& state=*static_cast<DecodeState*>(user);++state.calls;
    Check(w<=1024 && h<=1024 && bytes==uint64_t(w)*h*4,"bounded native ASTC tile");
    Check(n==uint64_t((w+bw-1)/bw)*((h+bh-1)/bh)*16,"native ASTC block ROI size");
    Tegra::Texture::ASTC::Decompress({data,n},w,h,1,bw,bh,{out,bytes});return EDEN_DSMOD_TRUE;
}
void Save(const char* name,const echoes::Image& image) {
#ifdef ECHOES_TEST_PNG
    if(const char* output=std::getenv("ECHOES_ASSET_OUTPUT")) {
        std::filesystem::create_directories(output);
        const auto path=std::filesystem::path(output)/(std::string(name)+".png");
        Check(stbi_write_png(path.c_str(),image.w,image.h,4,image.rgba.data(),image.w*4)!=0,"native map PNG");
    }
#else
    (void)name;(void)image;
#endif
}
#endif
struct NativeArchive {
    std::filesystem::path path;uint64_t size{};size_t bytes_read{},calls{};
    std::map<std::string,std::pair<uint64_t,uint64_t>,std::less<>> members;
    size_t Read(uint64_t at,void* output,size_t n) {
        if(at>size || n>size-at)return 0;
        std::ifstream stream(path,std::ios::binary);stream.seekg(static_cast<std::streamoff>(at));
        stream.read(static_cast<char*>(output),static_cast<std::streamsize>(n));
        bytes_read+=static_cast<size_t>(stream.gcount());++calls;
        return static_cast<size_t>(stream.gcount());
    }
    explicit NativeArchive(const char* root) {
        path=std::filesystem::path(root)/"region_common/ui/Field.arc";size=std::filesystem::file_size(path);
        std::array<uint8_t,20> h{};Check(Read(0,h.data(),20)==20 && !std::memcmp(h.data(),"SARC",4),"native SARC");
        Check(U16(h.data()+6)==0xfeff,"SARC byte order");
        const uint64_t data=U32(h.data()+12),declared=U32(h.data()+8),off=U16(h.data()+4);
        Check(declared<=size && data<=declared,"SARC range");
        std::array<uint8_t,12> fat{};Check(Read(off,fat.data(),12)==12 && !std::memcmp(fat.data(),"SFAT",4),"SFAT");
        const size_t n=U16(fat.data()+6);Check(n<30000,"SARC count");
        std::vector<uint8_t> nodes(n*16);Check(Read(off+12,nodes.data(),nodes.size())==nodes.size(),"SARC nodes");
        const uint64_t names=off+12+n*16+8;
        for(size_t i=0;i<n;++i) {
            const auto* p=nodes.data()+i*16;const uint32_t attr=U32(p+4),first=U32(p+8),last=U32(p+12);
            Check(first<=last && data+last<=declared,"member bounds");if(!(attr>>24))continue;
            const uint64_t name=names+uint64_t(attr&0xffffff)*4;Check(name<data,"name bounds");
            std::array<char,256> text{};const size_t length=std::min<uint64_t>(255,data-name);
            Check(Read(name,text.data(),length)==length,"member name");
            members.emplace(std::string(text.data()),std::pair{data+first,uint64_t(last-first)});
        }
    }
    size_t Asset(std::string_view path,uint64_t offset,void* out,size_t n) {
        constexpr std::string_view prefix="region_common/ui/Field.arc#";
        if(!path.starts_with(prefix))return 0;
        path.remove_prefix(prefix.size());
        const auto it=members.find(path);if(it==members.end())return 0;
        if(!out && !n)return it->second.second;
        if(offset>it->second.second || n>it->second.second-offset)return 0;
        return Read(it->second.first+offset,out,n);
    }
    std::vector<uint8_t> Blob(std::string_view name) {
        std::string path="region_common/ui/Field.arc#";path+=name;
        const size_t n=Asset(path,0,nullptr,0);Check(n && n<=128u*1024,"fixture layout size");
        std::vector<uint8_t> b(n);Check(Asset(path,0,b.data(),n)==n,"fixture layout read");return b;
    }
};
}
int main(){try {
    echoes::FieldMapGeometry geometry;echoes::Image image(2,2);echoes::MapBounds bounds{1,2,3,4};
    std::array<uint8_t,64> bits{};echoes::MapComposeStatus status{};
    int reads=0;
    echoes::FieldMapAssets missing([&](std::string_view,uint64_t,void*,size_t){++reads;return size_t(0);});
    Check(!missing.Compose(bits,image,bounds,&status) && image.Empty() && image.w==0 && bounds.width==0,"missing source clears outputs");
    Check(status==echoes::MapComposeStatus::MissingOrInvalidSource,"missing source status");
    int payload=0;
    echoes::FieldMapAssets oversized([&](std::string_view,uint64_t,void* p,size_t){if(p)++payload;return size_t(128u<<10)+1;});
    Check(!oversized.Inspect(geometry) && geometry.shapes.empty() && payload==0,"oversized layout rejected before read");
    echoes::FieldMapAssets shortread([](std::string_view,uint64_t,void* p,size_t){return p?size_t(1):size_t(32);});
    Check(!shortread.Inspect(geometry),"short layout read rejected");
    std::array<uint8_t,32> junk{};
    Check(!echoes::ParseFieldMapLayouts(junk,junk,geometry) && geometry.shapes.empty(),"bad layout clears metadata");
    const char* root=std::getenv("ECHOES_ROMFS");
    if(!root){std::cout<<checks<<" checks passed; native fixture skipped (set ECHOES_ROMFS)\n";return 77;}
    NativeArchive arc(root);
    echoes::FieldMapAssets assets([&](std::string_view p,uint64_t o,void* b,size_t n){return arc.Asset(p,o,b,n);});
    const size_t before=arc.bytes_read;
    Check(assets.Inspect(geometry),"native geometry accepted");
    Check(geometry.shapes.size()==108 && geometry.terrain.size()==2,"native Field shape and terrain counts");
    Check(arc.bytes_read-before<512u*1024,"metadata reads exclude native texture payloads");
    std::array<bool,512> seen{};
    for(const auto& shape:geometry.shapes) {
        Check(shape.index<512 && !seen[shape.index],"unique sparse bit index");seen[shape.index]=true;
        Check(shape.pane.parent>=0 && geometry.traverse_panes[shape.pane.parent].name=="N_InOut_00","original traversal ancestry");
        for(const auto& picture:shape.pictures) {
            Check(picture.material.textures[1]=="FieldBreakThrough_"+std::to_string(shape.index)+"^_H","native bit to material binding");
            Check(!picture.material.native_record.empty() && !picture.uv.empty(),"original material and UV retained");
        }
    }
    Check(seen[0] && seen[1] && seen[470] && !seen[48],"native sparse indices preserved");
    auto tex=std::find_if(geometry.textures.begin(),geometry.textures.end(),[](const auto& t){return t.name=="FieldTraverse_00 ^_H";});
    Check(tex!=geometry.textures.end() && tex->width==7200 && tex->height==5184,"original atlas dimensions and space in name");
    const auto fb=arc.Blob("blyt/Field.bflyt"),tb=arc.Blob("blyt/Field_Traverse.bflyt");
    for(size_t cut=0;cut<tb.size();cut+=1099)
        Check(!echoes::ParseFieldMapLayouts(fb,std::span(tb).first(cut),geometry) && geometry.shapes.empty(),"truncated layouts fail closed");
    auto bad=tb;bad[24]=0;bad[25]=0;bad[26]=0;bad[27]=0;
    Check(!echoes::ParseFieldMapLayouts(fb,bad,geometry),"zero-sized section rejected");
    bad=tb;for(size_t i=20;i+8<bad.size();) {
        const size_t n=U32(bad.data()+i+4);
        if(!std::memcmp(bad.data()+i,"prt1",4)) {bad[i+0x54]=255;break;}i+=n;
    }
    Check(!echoes::ParseFieldMapLayouts(fb,bad,geometry),"unbounded override count rejected");
    const size_t cached=arc.bytes_read;
    for(int pattern=0;pattern<3;++pattern) {
        bits.fill(pattern==1?255:0);if(pattern==2)bits[0]=1;
        image=echoes::Image(2,2);bounds={1,2,3,4};
        Check(!assets.Compose(bits,image,bounds,&status) && image.Empty() && image.w==0 && bounds.width==0 && bounds.left==0,"unproven composition never publishes terrain");
        Check(status==echoes::MapComposeStatus::UnsupportedDecoderOrDiscovery,"explicit unsupported composition status");
    }
    Check(arc.bytes_read==cached,"native metadata cached across discovery snapshots");
#ifdef ECHOES_TEST_NATIVE_ASTC
    Check(assets.Inspect(geometry),"native geometry retained");
    Check(geometry.excluded_quads.size()==4,"three story parts and sandstorm have original quad exclusions");
    DecodeState decoder;
    echoes::FieldMapAssets map([&](std::string_view p,uint64_t o,void* b,size_t n){return arc.Asset(p,o,b,n);},&decoder,Decode);
    bits.fill(0);
    Check(map.Compose(bits,image,bounds,&status) && status==echoes::MapComposeStatus::Ready,"empty discovery composes covered atlas");
    Check(image.w==1272 && image.h==916 && bounds.width==1272 && bounds.height==916,"authored native atlas dimensions and aspect");
    Check(std::all_of(image.rgba.begin(),image.rgba.end(),[](uint8_t c){return c==0;}),"empty discovery exposes no terrain or hidden RGB");
    bits[0]=1;
    Check(map.Compose(bits,image,bounds,&status),"one discovered native region composes");
    size_t visible=0;for(size_t i=3;i<image.rgba.size();i+=4)visible+=image.rgba[i]!=0;
    Check(visible>0 && visible<size_t(image.w)*image.h/10,"one discovery reveals a bounded native region");
    Save("field-map-one-region",image);const auto one_region=image.rgba;
    const size_t previous_calls=decoder.calls,previous_reads=arc.bytes_read;
    Check(map.Compose(bits,image,bounds,&status) && image.rgba==one_region,"identical discovery atlas is deterministic");
    Check(decoder.calls==previous_calls && arc.bytes_read==previous_reads,"identical discovery avoids decoding and source reads");
    bits.fill(0);for(const auto& shape:geometry.shapes)bits[shape.index/8]|=uint8_t(1u<<(shape.index%8));
    Check(map.Compose(bits,image,bounds,&status),"all authored discovered regions compose");
    visible=0;for(size_t i=3;i<image.rgba.size();i+=4)visible+=image.rgba[i]!=0;
    Check(visible>size_t(image.w)*image.h/3 && visible<size_t(image.w)*image.h,"explored atlas retains covered story areas and native exterior");
    for(const auto& q:geometry.excluded_quads) {
        const int x0=std::max(0,int(std::floor(q.left-bounds.left)));
        const int x1=std::min(image.w,int(std::ceil(q.left+q.width-bounds.left)));
        const int y0=std::max(0,int(std::floor(bounds.top-q.top)));
        const int y1=std::min(image.h,int(std::ceil(bounds.top-q.top+q.height)));
        bool hidden=true;
        for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x) {
            const auto* p=image.At(x,y);hidden &= p[0]==0 && p[1]==0 && p[2]==0 && p[3]==0;
        }
        Check(hidden,"unknown story/sandstorm quads expose neither terrain nor hidden RGB");
    }
    Save("field-map-all-authored-regions",image);
    const auto conservative=image.rgba;
    Check(geometry.field_extension_quads.size()==3,"Field extensions are distinct from weather exclusions");
    Check(map.Compose(bits,image,bounds,&status,true),"verified hidden Field extensions permit discovered terrain");
    size_t story_visible=0;for(size_t i=3;i<image.rgba.size();i+=4)story_visible+=image.rgba[i]!=0;
    Check(story_visible>visible,"native story predicate only expands eligible explored coverage");
    const auto revealed=image.rgba;
    Check(map.Compose(bits,image,bounds,&status,false) && image.rgba==conservative,"unknown story restores all conservative covers through cache");
    Check(map.Compose(bits,image,bounds,&status,true) && image.rgba==revealed,"story state participates in image cache identity");
    Check(geometry.field_extension_quads.size()==3 && geometry.sandstorm_quads.size()==1,"story and weather covers retain separate native roles");
    Check(map.Compose(bits,image,bounds,&status,true,true),"verified hidden native sandstorm permits explored desert terrain");
    size_t weather_visible=0;for(size_t i=3;i<image.rgba.size();i+=4)weather_visible+=image.rgba[i]!=0;
    Check(weather_visible>story_visible,"native weather predicate independently expands eligible coverage");
    const auto weather_revealed=image.rgba;
    Check(map.Compose(bits,image,bounds,&status,true,false) && image.rgba==revealed,"unknown weather restores the native sandstorm cover");
    Check(map.Compose(bits,image,bounds,&status,true,true) && image.rgba==weather_revealed,"weather state participates in image cache identity");
    bits.fill(0);
    Check(map.Compose(bits,image,bounds,&status,true) && std::all_of(image.rgba.begin(),image.rgba.end(),[](uint8_t c){return c==0;}),"story progress never reveals undiscovered terrain");
    bits.fill(0);bits[0]=1;
    Check(map.Compose(bits,image,bounds,&status) && image.rgba==one_region,"tile cache eviction preserves exact map output");
    bits.fill(255);image=echoes::Image(2,2);bounds={1,2,3,4};
    Check(!map.Compose(bits,image,bounds,&status) && image.Empty() && bounds.width==0,"unmapped discovery bits refuse output");
    bits.fill(0);bits[0]=1;
    echoes::FieldMapAssets rejected_decode([&](std::string_view p,uint64_t o,void* b,size_t n){return arc.Asset(p,o,b,n);},nullptr,
        +[](void*,uint32_t,uint32_t,uint32_t,uint32_t,const uint8_t*,size_t,uint8_t*,size_t)->EdenDsmodBool{return EDEN_DSMOD_FALSE;});
    image=echoes::Image(2,2);bounds={1,2,3,4};
    Check(!rejected_decode.Compose(bits,image,bounds,&status) && image.Empty() && bounds.width==0,"decoder refusal never publishes a partial map");
    echoes::FieldMapAssets truncated_payload([&](std::string_view p,uint64_t o,void* b,size_t n){
        if(b && n>1024*1024)return size_t(0);
        return arc.Asset(p,o,b,n);
    },&decoder,Decode);
    Check(!truncated_payload.Compose(bits,image,bounds,&status) && image.Empty() && bounds.width==0,"short BNTX payload never publishes a partial map");
    auto altered=tb;
    for(size_t i=20;i+8<altered.size();) {
        const size_t n=U32(altered.data()+i+4);
        if(!std::memcmp(altered.data()+i,"prt1",4)) {
            const size_t first=U32(altered.data()+i+0x60+28);const float shifted=1;
            std::memcpy(altered.data()+i+first+0x2c,&shifted,4);break;
        }i+=n;
    }
    echoes::FieldMapAssets divergent_pictures([&](std::string_view p,uint64_t o,void* b,size_t n){
        if(p=="region_common/ui/Field.arc#blyt/Field_Traverse.bflyt") {
            if(!b && !n)return altered.size();
            if(o>altered.size() || n>altered.size()-o)return size_t(0);
            std::memcpy(b,altered.data()+o,n);return n;
        }return arc.Asset(p,o,b,n);
    },&decoder,Decode);
    Check(!divergent_pictures.Compose(bits,image,bounds,&status) && image.Empty() && bounds.width==0,"different native picture coverage refuses composition");

#endif
    std::cout<<checks<<" checks passed with native Field layouts and ranged BNTX metadata\n";
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<"\n";return 1;}}
