// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_ui.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
namespace SmoUi {
namespace {
using Span = std::span<const uint8_t>;
bool Fits(Span b, uint64_t p, uint64_t n) { return p <= b.size() && n <= b.size() - p; }
uint16_t U16(Span b, size_t p) { return uint16_t(b[p]) | uint16_t(b[p + 1]) << 8; }
uint32_t U32(Span b, size_t p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= uint32_t(b[p + i]) << (8 * i);
    return v;
}
uint64_t U64(Span b, size_t p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= uint64_t(b[p + i]) << (8 * i);
    return v;
}
void Over(uint8_t* d, unsigned r, unsigned g, unsigned b, unsigned a) {
    const unsigned old = d[3], total = a + old * (255-a)/255;
    if (!total) return;
    const unsigned color[]{r,g,b};
    for (unsigned k=0;k<3;++k) d[k] = uint8_t((color[k]*a + d[k]*old*(255-a)/255)/total);
    d[3] = uint8_t(total);
}
}
std::optional<fe3h_ui2d::Texture> Life(std::span<const uint8_t> bytes, unsigned hp, unsigned maximum) {
    if (!maximum || maximum>9 || hp>maximum) return {};
    const auto gauge = fe3h_ui2d::BntxDecode(bytes,"NyLifeGauge00^s");
    const auto shadow = fe3h_ui2d::BntxDecode(bytes,"NyLifeShadow00^s");
    const auto heart = fe3h_ui2d::BntxDecode(bytes,"NyIconLife^s");
    if (!gauge || !shadow || !heart || gauge->width!=128 || gauge->height!=128 ||
        shadow->width!=128 || shadow->height!=128 || heart->width!=64 || heart->height!=128) return {};
    fe3h_ui2d::Texture out{"life",128,128,std::vector<uint8_t>(128*128*4)};
    for (unsigned y=0;y<128;++y) for(unsigned x=0;x<128;++x) {
        const size_t p=(y*128+x)*4;
        auto* d=out.rgba.data()+p;
        Over(d,0,0,0,shadow->rgba[p]/2);
        double angle=std::atan2(double(x)-63.5,63.5-double(y));
        if(angle<0) angle+=2*std::numbers::pi;
        const bool filled=angle < (double(hp)/maximum)*2*std::numbers::pi;
        const bool low=hp*3<=maximum;
        Over(d,filled?(low?255:0):80,filled?(low?36:255):80,filled?0:80,gauge->rgba[p]);
        const unsigned hx=x<64?x:127-x;
        Over(d,255,255,255,heart->rgba[(y*64+hx)*4]);
    }
    return out;
}
std::optional<fe3h_ui2d::Texture> DecodeAstcBrti(Span b, std::string_view name, void* userdata,
                                                 EdenDsmodAstcDecoder decode) {
    if (!decode || b.size() < 0x30 || std::memcmp(b.data(), "BNTX", 4)) return std::nullopt;
    const auto count = U32(b, 0x24);
    const auto table = U64(b, 0x28);
    if (!count || count > 4096 || !Fits(b, table, uint64_t(count) * 8)) return std::nullopt;
    for (uint32_t i = 0; i < count; ++i) {
        const auto p = U64(b, size_t(table) + i * 8);
        if (!Fits(b, p, 0x78) || std::memcmp(b.data() + p, "BRTI", 4)) continue;
        const auto t = b.subspan(size_t(p));
        // Pointer FIELDS (name/mips) inside a BRTI block are absolute offsets into the whole
        // BNTX buffer `b`, not relative to the BRTI block itself -- matching smo_capture.cpp's
        // proven Portraits() parser, which this mirrors. Only the small fixed-layout header
        // fields below (format/width/height/tile/layout/selectors/size) are read at a constant
        // offset from the BRTI block's own start (via `t`).
        const auto name_off = U64(t, 0x60);
        if (!Fits(b, name_off, 2)) continue;
        const auto len = U16(b, size_t(name_off));
        if (!len || len > 128 || !Fits(b, name_off + 2, len)) continue;
        const std::string_view found_name{reinterpret_cast<const char*>(b.data() + name_off + 2), len};
        if (found_name != name) continue;
        // 0x2d06 = ASTC4x4 sRGB (same format check as smo_capture.cpp's portrait registry).
        const auto width = U32(t, 0x24), height = U32(t, 0x28);
        if (U32(t, 0x1c) != 0x2d06 || !width || !height || width > 4096 || height > 4096 ||
            U32(t, 0x2c) != 1 || U32(t, 0x30) != 1 || U16(t, 0x16) < 1 || U16(t, 0x12) > 1)
            return std::nullopt;
        const auto layout = U32(t, 0x34) & 7;
        const auto selectors = U32(t, 0x58);
        const auto mips = U64(t, 0x70);
        if (!Fits(b, mips, 8)) return std::nullopt;
        const auto image = U64(b, size_t(mips));
        const auto size = U32(t, 0x50);
        if (!Fits(b, image, size)) return std::nullopt;
        const auto we = (width + 3) / 4, he = (height + 3) / 4;
        const auto raw = b.subspan(size_t(image), size);
        std::vector<uint8_t> blocks;
        if (U16(t, 0x12) == 0) {
            uint32_t bh = 1u << layout;
            while (bh > 1 && bh * 8 >= 2 * he) bh /= 2;
            blocks = fe3h_ui2d::Deswizzle(raw, we, he, 16, bh);
        } else if (raw.size() >= size_t(we) * he * 16) {
            blocks.assign(raw.begin(), raw.begin() + size_t(we) * he * 16);
        }
        if (blocks.size() != size_t(we) * he * 16) return std::nullopt;
        fe3h_ui2d::Texture out{std::string(name), width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
        if (!decode(userdata, width, height, 4, 4, blocks.data(), blocks.size(),
                    out.rgba.data(), out.rgba.size())) return std::nullopt;
        for (size_t px = 0; px < out.rgba.size(); px += 4) {
            const std::array<uint8_t, 4> rgba{out.rgba[px], out.rgba[px + 1], out.rgba[px + 2], out.rgba[px + 3]};
            for (unsigned c = 0; c < 4; ++c) {
                const auto selector = (selectors >> (8 * c)) & 255;
                out.rgba[px + c] = selector == 0 ? 0 : selector == 1 ? 255 : rgba[selector - 2];
            }
        }
        return out;
    }
    return std::nullopt;
}
std::optional<fe3h_ui2d::Texture> WorldBackground(std::span<const uint8_t> bytes) {
    const auto mask=fe3h_ui2d::BntxDecode(bytes,"CmImageWorld03^r");
    if(!mask || mask->width!=2048 || mask->height!=2048) return {};
    fe3h_ui2d::Texture out{"world_bg",1240,1080,std::vector<uint8_t>(1240*1080*4)};
    for(unsigned y=0;y<1080;++y)for(unsigned x=0;x<1240;++x){
        const unsigned sx=560+x*1200/1240,sy=950+y*1045/1080;
        const unsigned v=mask->rgba[(sy*2048+sx)*4];
        auto* d=out.rgba.data()+(y*1240+x)*4;
        d[0]=v>=236?255:254;
        d[1]=v>=236?uint8_t(43-(v-236)*23/19):v>=205?43:uint8_t(43+(205-v)*95/205);
        d[2]=v>=236?uint8_t(42+(v-236)*8/19):v>=205?42:uint8_t(42+(205-v)*96/205);
        d[3]=255;
    }
    return out;
}
std::optional<fe3h_ui2d::Texture> WorldBackgroundNeutral(std::span<const uint8_t> bytes) {
    const auto mask=fe3h_ui2d::BntxDecode(bytes,"CmImageWorld03^r");
    if(!mask || mask->width!=2048 || mask->height!=2048) return {};
    fe3h_ui2d::Texture out{"world_bg_neutral",1240,1080,std::vector<uint8_t>(1240*1080*4)};
    for(unsigned y=0;y<1080;++y)for(unsigned x=0;x<1240;++x){
        const unsigned sx=560+x*1200/1240,sy=950+y*1045/1080;
        const unsigned v=mask->rgba[(sy*2048+sx)*4];
        auto* d=out.rgba.data()+(y*1240+x)*4;
        d[0]=d[1]=d[2]=uint8_t(v);d[3]=255;
    }
    return out;
}
std::optional<fe3h_ui2d::Texture> FlagRing(std::span<const uint8_t> bytes){
    const auto q=fe3h_ui2d::BntxDecode(bytes,"NyEffectCircle32^s");
    if(!q || q->width!=32 || q->height!=32)return {};
    fe3h_ui2d::Texture out{"flag_ring",64,64,std::vector<uint8_t>(64*64*4)};
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
        auto* p=out.rgba.data()+(y*64+x)*4;
        p[0]=p[1]=p[2]=255;p[3]=q->rgba[((y<32?y:63-y)*32+(x<32?x:63-x))*4];
    }
    return out;
}
std::optional<fe3h_ui2d::Texture> ObjectivePointer(std::span<const uint8_t> bytes, unsigned degrees) {
    const auto q=fe3h_ui2d::BntxDecode(bytes,"CmCircle24^s");
    const auto arrow=fe3h_ui2d::BntxDecode(bytes,"CmArrow^s");
    if(!q || q->width!=24 || q->height!=24 || !arrow || arrow->width!=32 || arrow->height!=32)return {};
    fe3h_ui2d::Texture out{"objective_pointer",48,48,std::vector<uint8_t>(48*48*4)};
    const double a=(degrees%360)*std::numbers::pi/180,co=std::cos(a),si=std::sin(a);
    for(unsigned y=0;y<48;++y)for(unsigned x=0;x<48;++x){
        auto* d=out.rgba.data()+(y*48+x)*4;
        const unsigned qx=x<24?x:47-x,qy=y<24?y:47-y;
        Over(d,255,255,255,q->rgba[(qy*24+qx)*4]);
        const double dx=(double(x)-23.5)*32/34,dy=(double(y)-23.5)*32/34;
        const int ax=int(std::lround(co*dx+si*dy+15.5)),ay=int(std::lround(-si*dx+co*dy+15.5));
        if(ax>=0 && ax<32 && ay>=0 && ay<32)Over(d,235,37,40,arrow->rgba[(ay*32+ax)*4]);
    }
    return out;
}
std::optional<fe3h_ui2d::Texture> ObjectiveDot(std::span<const uint8_t> bytes) {
    const auto dots=fe3h_ui2d::BntxDecode(bytes,"CmLineDot^s");
    if(!dots || dots->width!=8 || dots->height!=8)return {};
    // The native tile repeats two dots horizontally. Isolate one dot before positioning
    // along an arbitrary connector, so its spacing is independent of the tile's UV repeat.
    fe3h_ui2d::Texture out{"objective_dot",8,8,std::vector<uint8_t>(8*8*4)};
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
        auto* d=out.rgba.data()+(y*8+x)*4;
        d[0]=235;d[1]=37;d[2]=40;d[3]=dots->rgba[((y/2+2)*8+(x/2+1))*4];
    }
    return out;
}
std::optional<fe3h_ui2d::Texture> TitleLogoSmall(std::span<const uint8_t> bytes, void* userdata,
                                                 EdenDsmodAstcDecoder decode) {
    auto logo=DecodeAstcBrti(bytes,"CmTitleLogoSmall^w",userdata,decode);
    if(!logo || logo->width!=240 || logo->height!=150) return {};
    return logo;
}
std::optional<fe3h_ui2d::Texture> CappyCursor(std::span<const uint8_t> bytes, void* userdata,
                                              EdenDsmodAstcDecoder decode) {
    const auto cur=DecodeAstcBrti(bytes,"CmCursor00^w",userdata,decode);
    const auto eyes=DecodeAstcBrti(bytes,"CmCursorEyes00^w",userdata,decode);
    if(!cur || cur->width!=80 || cur->height!=52 || !eyes || eyes->width!=34 || eyes->height!=34) return {};
    fe3h_ui2d::Texture out{"cappy_cursor",80,60,std::vector<uint8_t>(80*60*4)};
    for(unsigned y=0;y<52;++y)for(unsigned x=0;x<80;++x){
        auto* d=out.rgba.data()+((y+4)*80+x)*4;
        const auto* s=cur->rgba.data()+(y*80+x)*4;
        Over(d,s[0],s[1],s[2],s[3]);
    }
    for(unsigned y=0;y<34;++y)for(unsigned x=0;x<34;++x){
        auto* d=out.rgba.data()+((y+2)*80+(x+40))*4;
        const auto* s=eyes->rgba.data()+(y*34+x)*4;
        Over(d,s[0],s[1],s[2],s[3]);
    }
    return out;
}
fe3h_ui2d::Texture BarFill() {
    fe3h_ui2d::Texture out{"bar_fill",4,4,std::vector<uint8_t>(4*4*4,0xFF)};
    return out;
}
fe3h_ui2d::Texture Slab(bool tall) {
    fe3h_ui2d::Texture out{"counter_slab",316,tall?138u:84u,std::vector<uint8_t>(316*(tall?138:84)*4)};
    for(unsigned y=0;y<out.height;++y)for(unsigned x=0;x<out.width;++x){
        // Native collection-counter slab, tilted1.6degrees as in the accepted mockup.
        const double py=double(y)+double(x)*0.02793;
        auto* d=out.rgba.data()+(y*out.width+x)*4;
        if(py>=15 && py<out.height-5)Over(d,0,0,0,70);
        if(py>=8 && py<out.height-12)Over(d,61,61,61,255);
    }
    return out;
}
}
