// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sw_ui_images.h"
#include "sw_ui_geometry.h"
#include "sw_encounter_archive.h"
#include "sw_species_icons.h"
#include <charconv>
#include <array>
#include <cmath>
#include <functional>
#include <string_view>

namespace sw_ui_images {
namespace {
using acnh::Image;
using Color = std::array<double,4>;
constexpr Color White{255,255,255,255}, Ink{31,29,28,255};
double Segment(double x,double y,double ax,double ay,double bx,double by) {
    double dx=bx-ax,dy=by-ay,t=std::clamp(((x-ax)*dx+(y-ay)*dy)/(dx*dx+dy*dy),0.,1.);
    return std::hypot(x-ax-t*dx,y-ay-t*dy);
}
Image Paint(int w,int h,const std::function<Color(double,double)>& pixel) {
    Image out(w,h);
    constexpr int ss=4;
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        Color sum{};
        for(int j=0;j<ss;++j) for(int i=0;i<ss;++i) {
            auto c=pixel(x+(i+.5)/ss,y+(j+.5)/ss);
            for(int k=0;k<3;++k) sum[k]+=c[k]*c[3]/255.;
            sum[3]+=c[3];
        }
        auto* p=out.At(x,y);
        for(int k=0;k<3;++k) p[k]=sum[3]?std::clamp(int(std::lround(sum[k]*255/sum[3])),0,255):255;
        p[3]=std::clamp(int(std::lround(sum[3]/(ss*ss))),0,255);
    }
    return out;
}
Image Shape(std::string_view key) {
    if(key=="disc_white" || key=="disc_black") {
        const auto body=key=="disc_white"?White:Ink;
        return Paint(124,124,[&](double x,double y){
            if(std::hypot(x-62,y-62)<=48) return body;
            // Soft circular drop shadow, behind the button at a five-pixel vertical offset.
            double d=std::hypot(x-62,y-67)-48;
            return Color{0,0,0,70*.5*std::erfc(d/(6*std::sqrt(2.)))};
        });
    }
    if(key=="chevron2") return Paint(124,124,[](double x,double y){
        double d=std::min(Segment(x,y,43,35,81,62),Segment(x,y,81,62,43,89));
        return d<=8?Ink:d<=13?White:Color{};
    });
    if(key=="key_wedge") return Paint(24,44,[](double x,double y){return x<21.5-y*.5?White:Color{};});
    int d=key=="lock88"?88:key=="lock56"?56:0;
    if(d) return Paint(d+1,d+1,[&](double x,double y){
        const double cx=(d+1)/2.,cy=(d+1)/2.,s=d/88.;
        x=(x-cx)/s; y=(y-cy)/s;
        Color c=std::hypot(x,y)<=44?White:Color{};
        // Rounded padlock body, curved shackle, keyhole. All coordinates are authored geometry.
        bool shackle=std::abs(x)<=11 && y>=-24 && y<=-3 &&
            ((y>=-14 && std::abs(x)>=7) || (y< -14 && std::abs(std::hypot(x,y+14)-9)<=2));
        bool body=std::abs(x)<=19 && y>=-7 && y<=23 &&
            std::hypot(std::max(0.,std::abs(x)-16),std::max(0.,std::abs(y-8)-12))<=3;
        if(shackle||body) c=Ink;
        if(std::hypot(x,y-4)<=6 || (std::abs(x)<=2.5 && y>=4 && y<=17)) c=White;
        return c;
    });
    return {};
}
Image Mask(Image src) {
    for(std::size_t i=0;i<src.rgba.size();i+=4) {
        // PIL convert('L'): integer ITU-R 601 luma; multiply uses truncating division.
        const unsigned l=(19595*src.rgba[i]+38470*src.rgba[i+1]+7471*src.rgba[i+2]+32768)>>16;
        src.rgba[i+3]=(255-l)*src.rgba[i+3]/255;
        src.rgba[i]=src.rgba[i+1]=src.rgba[i+2]=255;
    }
    return src;
}
Image MaxBody(const Image& src) {
    // PIL-compatible separable bilinear fit; round at each pass before applying the
    // authored capsule's exact 4x/Lanczos coverage (generated without any game input).
    const auto sample=[](int a,int b,double f) {
        constexpr int precision=1<<22;
        const int wa=std::lround((1-f)*precision),wb=std::lround(f*precision);
        return std::clamp((a*wa+b*wb+(1<<21))>>22,0,255);
    };
    Image horizontal(640,src.h),out(640,94);
    for(int y=0;y<src.h;++y) for(int x=0;x<640;++x) {
        const double fx=std::clamp((x+.5)*src.w/640.-.5,0.,double(src.w-1));
        int ix=fx;
        for(int k=0;k<4;++k)
            horizontal.At(x,y)[k]=sample(src.At(ix,y)[k],src.At(std::min(ix+1,src.w-1),y)[k],fx-ix);
    }
    for(int y=0;y<94;++y) for(int x=0;x<640;++x) {
        const double fy=std::clamp((y+.5)*src.h/94.-.5,0.,double(src.h-1));
        int iy=fy;
        auto* p=out.At(x,y);
        for(int k=0;k<4;++k)
            p[k]=sample(horizontal.At(x,iy)[k],horizontal.At(x,std::min(iy+1,src.h-1))[k],fy-iy);
        const int edge=std::min(x,639-x);
        const unsigned alpha=edge<50?kCapsuleEdge[std::min(y,93-y)][edge]:255;
        p[3]=p[3]*alpha/255;
    }
    return out;
}
Image GameImage(const EdenDsmodHostApi& host,std::string_view key) {
    if(key.starts_with("form/")) {
        key.remove_prefix(5);
        auto split=key.find('/');
        if(split==std::string_view::npos) return {};
        unsigned species=0,form=0;
        auto a=std::from_chars(key.data(),key.data()+split,species);
        auto b=std::from_chars(key.data()+split+1,key.data()+key.size(),form);
        if(a.ec!=std::errc{} || a.ptr!=key.data()+split || b.ec!=std::errc{} || b.ptr!=key.data()+key.size() || species>898 || form>255) return {};
        char path[160];
        if(!sw_species_icons::IconSourceForm(species,form,path,sizeof path)) return {};
        std::string source=path;auto hash=source.find('#');
        auto bytes=sw_archive::Load(host,source.substr(0,hash).c_str());
        Image out;
        if(!acnh::BntxDecode(bytes,source.substr(hash+1),out)) return {};
        return out;
    }
    std::string archive,texture;
    bool mask=true;
    constexpr std::string_view commands[]{"fight","pokemon","bag","run","cheer"};
    for(int i=0;i<5;++i) if(key==std::string("cmd_")+std::string(commands[i])) {
        archive="battle_commandSelect_00_eng"; texture="battle_command_0"+std::to_string(i)+"^r";
    }
    if(key=="key_x" || key=="key_y") {
        archive="battle_commandSelect_01_eng"; texture=key=="key_x"?"button_icon_X_00^t":"button_icon_Y_00^t";
    }
    if(key=="move_max_body") { archive="battle_skillSelect_00_eng";texture="battle_gskill_bg_00^o";mask=false; }
    if(archive.empty()) return {};
    auto bytes=sw_archive::Load(host,("romfs:/bin/appli/battle/bin/"+archive+".arc").c_str());
    Image src;
    if(!acnh::BntxDecodeAny(bytes.data(),bytes.size(),texture,src)||src.Empty()) return {};
    return mask?Mask(std::move(src)):MaxBody(src);
}
} // namespace
bool Images::Load(const EdenDsmodHostApi& host,const char* key,void* receiver,EdenDsmodImageSink sink,bool& handled) {
    std::string_view name=key;
    constexpr std::string_view prefix="module:sw:ui/";
    handled=name.starts_with(prefix);
    if(!handled) return false;
    name.remove_prefix(prefix.size());
    std::lock_guard lock(mutex_);
    auto [it,added]=cache_.try_emplace(std::string(name));
    if(added) {
        // Cache failures too: unreadable game files never trigger a per-frame retry.
        try { it->second=Shape(name); if(it->second.Empty()) it->second=GameImage(host,name); } catch(...) {}
    }
    const auto& img=it->second;
    if(img.Empty()) return false;
    sink(receiver,img.w,img.h,img.rgba.data(),img.rgba.size()); return true;
}
void Images::Reset() { std::lock_guard lock(mutex_);cache_.clear(); }
} // namespace sw_ui_images
