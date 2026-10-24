// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_prologue_fonts.h"
#include "acnh_bytes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
namespace echoes {
namespace {
constexpr std::array<uint32_t,4> Originals={0xe0e0,0xe0e2,0xe0e3,0xe0e6};
constexpr uint32_t Alias=0x2400;
constexpr size_t Limit=8u<<20;
using namespace acnh::bytes;
bool Scalar(uint32_t cp) {return cp<=0x10ffff && !(cp>=0xd800 && cp<=0xdfff);}
bool Private(uint32_t cp) {return (cp>=0xe000 && cp<=0xf8ff) || (cp>=0xf0000 && cp<=0xffffd) || (cp>=0x100000 && cp<=0x10fffd);}
void Encode(uint32_t cp,std::string& out) {
    if(cp<128)out.push_back(char(cp));
    else if(cp<2048){out.push_back(char(0xc0|(cp>>6)));out.push_back(char(0x80|(cp&63)));}
    else if(cp<65536){out.push_back(char(0xe0|(cp>>12)));out.push_back(char(0x80|((cp>>6)&63)));out.push_back(char(0x80|(cp&63)));}
    else {out.push_back(char(0xf0|(cp>>18)));out.push_back(char(0x80|((cp>>12)&63)));out.push_back(char(0x80|((cp>>6)&63)));out.push_back(char(0x80|(cp&63)));}
}
bool Next(std::string_view s,size_t& at,uint32_t& cp) {
    const uint8_t c=uint8_t(s[at++]);cp=c;size_t n=0;uint32_t minimum=0;
    if(c>=0xc2 && c<=0xdf){cp=c&31;n=1;minimum=128;}
    else if(c>=0xe0 && c<=0xef){cp=c&15;n=2;minimum=2048;}
    else if(c>=0xf0 && c<=0xf4){cp=c&7;n=3;minimum=65536;}
    else if(c>=128)return false;
    if(n>s.size()-at)return false;
    while(n--){const uint8_t b=uint8_t(s[at++]);if((b&0xc0)!=0x80)return false;cp=(cp<<6)|(b&63);}
    return cp>=minimum && Scalar(cp) && (cp>=32 || cp==10 || cp==13 || cp==9);
}
void Put32(std::vector<uint8_t>& b,size_t p,uint32_t v) {for(int i=0;i<4;++i)b[p+i]=uint8_t(v>>(8*i));}
void Put64(std::vector<uint8_t>& b,size_t p,uint64_t v) {for(int i=0;i<8;++i)b[p+i]=uint8_t(v>>(8*i));}
// Bounded native FFNT metadata and BNTX array-layer view; shared texture code
// decodes layer zero, so an owned local copy points that view at the chosen layer.
// No input bytes, guest memory or external files are modified.
bool Pictograms(std::span<const uint8_t> d,std::array<acnh::Image,4>& icons) {
    const auto magic=[&](size_t p,const char* m){return Fits(d,p,8) && std::memcmp(d.data()+p,m,4)==0;};
    if(d.size()>Limit || d.size()<52 || !magic(0,"FFNT") || Le16(d,4)!=0xfeff || Le32(d,12)!=d.size())return false;
    const size_t finf=Le16(d,6);
    if(!magic(finf,"FINF") || !Fits(d,finf,32) || Le32(d,finf+4)!=32)return false;
    const uint32_t ptr=Le32(d,finf+20);if(ptr<8)return false;
    const size_t t=ptr-8;
    if(!magic(t,"TGLP") || !Fits(d,t,32))return false;
    const size_t total=Le32(d,t+4);if(total<32 || !Fits(d,t,total))return false;
    const uint32_t cw=d[t+8],ch=d[t+9],sheets=d[t+10],cols=Le16(d,t+20),rows=Le16(d,t+22),sw=Le16(d,t+24),sh=Le16(d,t+26);
    const uint32_t data=Le32(d,t+28);
    if(!cw || !ch || !sheets || sheets>8 || !cols || !rows || uint64_t(cols)*rows*sheets>65536 || !sw || !sh || sw>2048 || sh>2048 || cols*(cw+1)+1>sw || rows*(ch+1)+1>sh || data<t+32 || data>=t+total)return false;
    std::map<uint32_t,uint16_t> cmap;std::set<uint32_t> visited;
    uint32_t map_ptr=Le32(d,finf+28);
    const auto add=[&](uint32_t cp,uint32_t index){return index==0xffff || (Scalar(cp) && index<uint64_t(cols)*rows*sheets && cmap.emplace(cp,uint16_t(index)).second);};
    for(size_t count=0;map_ptr;++count) {
        if(count>=64 || map_ptr<8 || !visited.insert(map_ptr).second)return false;
        const size_t a=map_ptr-8;if(!magic(a,"CMAP") || !Fits(d,a,24))return false;
        const size_t n=Le32(d,a+4);if(n<24 || !Fits(d,a,n))return false;
        const uint32_t begin=Le32(d,a+8),last=Le32(d,a+12);const uint16_t method=Le16(d,a+16);
        if(last<begin || last>0x10ffff || last-begin>=65536)return false;
        size_t q=a+24;
        if(method==0) {
            if(n-24<2)return false;
            const uint32_t first=Le16(d,q);
            for(uint32_t cp=begin;cp<=last;++cp)if(!add(cp,first+cp-begin))return false;
        } else if(method==1) {
            if(uint64_t(last-begin+1)*2>n-24)return false;
            for(uint32_t cp=begin;cp<=last;++cp,q+=2)if(!add(cp,Le16(d,q)))return false;
        } else if(method==2) {
            if(n-24<4)return false;
            const size_t entries=Le16(d,q);q+=4;
            if(entries>(n-28)/8)return false;
            for(size_t i=0;i<entries;++i,q+=8){const uint32_t cp=Le32(d,q);if(cp<begin || cp>last || !add(cp,Le16(d,q+4)))return false;}
        } else return false;
        map_ptr=Le32(d,a+20);
    }
    if(!Fits(d,data,0x30) || std::memcmp(d.data()+data,"BNTX",4))return false;
    std::vector<uint8_t> b(d.begin()+data,d.begin()+t+total);
    const uint32_t declared=Le32(b,0x1c);if(declared<0x30 || declared>b.size())return false;b.resize(declared);
    const uint64_t array=Le64(b,0x28);
    if(Le32(b,0x24)!=1 || !Fits(b,array,8))return false;
    const uint64_t brti=Le64(b,array);
    if(!Fits(b,brti,0xa0) || std::memcmp(b.data()+brti,"BRTI",4) || Le32(b,brti+0x24)!=sw || Le32(b,brti+0x28)!=sh || Le32(b,brti+0x30)!=sheets)return false;
    const uint32_t image_size=Le32(b,brti+0x50);const uint64_t mip=Le64(b,brti+0x70);
    if(!image_size || image_size%sheets || !Fits(b,mip,8))return false;
    const uint64_t start=Le64(b,mip);const uint32_t layer_size=image_size/sheets;
    if(!Fits(b,start,image_size))return false;
    std::map<uint32_t,acnh::Image> decoded;
    for(size_t i=0;i<Originals.size();++i) {
        const auto at=cmap.find(Originals[i]);if(at==cmap.end())return false;
        const uint32_t layer=at->second/(cols*rows),index=at->second%(cols*rows);
        if(!decoded.contains(layer)) {
            auto view=b;Put64(view,size_t(mip),start+uint64_t(layer)*layer_size);Put32(view,size_t(brti)+0x30,1);Put32(view,size_t(brti)+0x50,layer_size);
            acnh::Image image;if(!acnh::BntxDecode(view,{},image) || image.w!=int(sw) || image.h!=int(sh))return false;
            for(uint32_t y=0;y<sh/2;++y)for(uint32_t x=0;x<sw*4;++x)std::swap(image.rgba[size_t(y)*sw*4+x],image.rgba[size_t(sh-1-y)*sw*4+x]);
            decoded.emplace(layer,std::move(image));
        }
        const auto& sheet=decoded.at(layer);const uint32_t gx=index%cols*(cw+1)+1,gy=index/cols*(ch+1)+1;
        if(gx+cw>sw || gy+ch>sh)return false;
        uint32_t x0=cw,y0=ch,x1=0,y1=0;
        for(uint32_t y=0;y<ch;++y)for(uint32_t x=0;x<cw;++x)if(sheet.At(int(gx+x),int(gy+y))[3]){x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x+1);y1=std::max(y1,y+1);}
        if(x1<=x0 || y1<=y0)return false;
        icons[i]=acnh::Image(int(x1-x0),int(y1-y0));
        for(uint32_t y=y0;y<y1;++y)for(uint32_t x=x0;x<x1;++x)std::memcpy(icons[i].At(int(x-x0),int(y-y0)),sheet.At(int(gx+x),int(gy+y)),4);
    }
    return true;
}
} // namespace
bool PrologueDisplayText(std::string_view raw,std::string& display) {
    display.clear();if(raw.size()>8192)return false;std::string result;
    for(size_t at=0;at<raw.size();) {
        uint32_t cp{};if(!Next(raw,at,cp) || (cp>=Alias && cp<Alias+4))return false;
        const auto p=std::find(Originals.begin(),Originals.end(),cp);
        if(p!=Originals.end())cp=Alias+uint32_t(p-Originals.begin());
        else if(Private(cp))return false;
        Encode(cp,result);
    }
    display=std::move(result);return true;
}
bool PrologueBodyAtlas(const NativeFonts& native,const ReadAssetFn& read,std::span<const uint32_t> required,FontAtlas& out,uint32_t cap) {
    out={};if(!read || required.empty() || required.size()>8192 || cap<22 || cap>36)return false;
    std::vector<uint32_t> ordinary;for(uint32_t cp:required)if(cp<Alias || cp>=Alias+4)ordinary.push_back(cp);
    if(ordinary.empty())ordinary.push_back(32);
    FontAtlas result;if(!BodyAtlas(native,ordinary,result,cap))return false;
    constexpr std::string_view path="region_common/ui/Pictogram.bffnt";
    const size_t size=read(path,0,nullptr,0);if(!size || size>Limit)return false;
    std::vector<uint8_t> bytes(size);if(read(path,0,bytes.data(),size)!=size)return false;
    std::array<acnh::Image,4> icons;if(!Pictograms(bytes,icons))return false;
    if(result.first_codepoint>Alias || Alias+4-result.first_codepoint>16384 || result.image.w!=1024 || result.image.h<=0 || result.image.h>4096-int(cap)-2)return false;
    const int row=result.image.h+1;acnh::Image image(1024,result.image.h+int(cap)+2);
    std::memcpy(image.rgba.data(),result.image.rgba.data(),result.image.rgba.size());
    result.glyphs.resize(Alias+4-result.first_codepoint);int x=1;
    for(size_t i=0;i<icons.size();++i) {
        const auto& src=icons[i];const int width=std::max(1,int(std::lround(double(src.w)*cap/src.h)));
        if(width>256 || x+width>=1024)return false;
        for(uint32_t y=0;y<cap;++y)for(int dx=0;dx<width;++dx) {
            const double sx=std::clamp((dx+.5)*src.w/width-.5,0.,double(src.w-1)),sy=std::clamp((y+.5)*src.h/cap-.5,0.,double(src.h-1));
            const int ix=int(sx),iy=int(sy),jx=std::min(ix+1,src.w-1),jy=std::min(iy+1,src.h-1);const double fx=sx-ix,fy=sy-iy;
            const double alpha=(1-fy)*((1-fx)*src.At(ix,iy)[3]+fx*src.At(jx,iy)[3])+fy*((1-fx)*src.At(ix,jy)[3]+fx*src.At(jx,jy)[3]);
            auto* p=image.At(x+dx,row+int(y));p[0]=p[1]=p[2]=255;p[3]=uint8_t(std::lround(alpha));
        }
        result.glyphs[Alias+uint32_t(i)-result.first_codepoint]={uint16_t(x),uint16_t(row),uint16_t(width),uint16_t(cap),0,int16_t(cap),uint16_t(width+3)};x+=width+2;
    }
    result.image=std::move(image);out=std::move(result);return true;
}
} // namespace echoes
