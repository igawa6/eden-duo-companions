// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_assets.h"
#include "acnh_tex.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

namespace echoes {
namespace {
constexpr size_t MaxSource = 32u << 20;
constexpr size_t MaxImages = 64u << 20;
constexpr std::string_view Source = "region_common/ui/GameMain.arc#timg/__Combined.bntx";
constexpr std::string_view PinSource = "region_common/ui/IconMapPin.arc#timg/__Combined.bntx";

bool Dimensions(std::string_view key, std::string_view prefix, int minimum, int& w, int& h) {
    if (!key.starts_with(prefix)) return false;
    key.remove_prefix(prefix.size());
    const auto slash = key.find('/');
    if (slash == std::string_view::npos) return false;
    auto parse = [](std::string_view s, int& value) {
        if (s.empty() || s.front() < '1' || s.front() > '9') return false;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), value);
        return r.ec == std::errc{} && r.ptr == s.data() + s.size();
    };
    return parse(key.substr(0, slash), w) && parse(key.substr(slash + 1), h) &&
           w >= minimum && h >= minimum && w <= 1240 && h <= 1080 &&
           uint64_t(w) * h <= 1240u * 1080u;
}

void Copy(Image& to, const Image& from, int dx, int dy, int sx, int sy, int w, int h,
          bool mirror = false) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            std::memcpy(to.At(dx+x, dy+y), from.At(sx+(mirror ? w-1-x : x), sy+y), 4);
}
void Over(uint8_t* dst, const uint8_t* src) {
    const unsigned sa = src[3], da = dst[3], a = sa*255 + da*(255-sa);
    if (!a) { std::memset(dst, 0, 4); return; }
    for (int c = 0; c < 3; ++c)
        dst[c] = uint8_t((unsigned(src[c])*sa*255 + unsigned(dst[c])*da*(255-sa) + a/2)/a);
    dst[3] = uint8_t((a+127)/255);
}

// Separable Lanczos resampling retains the texture's native fibers at the companion scale.
Image Resize(const Image& in, int w, int h) {
    auto kernel = [](double x) {
        x = std::abs(x);
        if (x < 1e-10) return 1.0;
        if (x >= 3) return 0.0;
        constexpr double pi = 3.14159265358979323846;
        return std::sin(pi*x)*std::sin(pi*x/3)/(pi*pi*x*x/3);
    };
    auto pass = [&](const Image& src, int dw, int dh, bool horizontal) {
        Image dst(dw, dh);
        const int n = horizontal ? src.w : src.h, m = horizontal ? dw : dh;
        const double ratio = double(n)/m, filter = std::max(1.0, ratio);
        for (int pos = 0; pos < m; ++pos) {
            const double center = (pos+.5)*ratio;
            const int first = std::max(0, int(std::floor(center-3*filter)));
            const int last = std::min(n, int(std::ceil(center+3*filter)));
            for (int cross = 0; cross < (horizontal ? dh : dw); ++cross) {
                std::array<double,4> sum{}; double total = 0;
                for (int k = first; k < last; ++k) {
                    const double f = kernel((k+.5-center)/filter); total += f;
                    const auto* p = horizontal ? src.At(k,cross) : src.At(cross,k);
                    for (int c=0;c<3;++c) sum[c] += f*p[c]*p[3]/255.;
                    sum[3] += f*p[3];
                }
                auto* p = horizontal ? dst.At(pos,cross) : dst.At(cross,pos);
                const double alpha = std::clamp(sum[3]/total,0.,255.);
                for (int c=0;c<3;++c)
                    p[c] = uint8_t(std::clamp(std::round(alpha > 0 ? sum[c]/total*255/alpha : 0),0.,255.));
                p[3] = uint8_t(std::round(alpha));
            }
        }
        return dst;
    };
    return pass(pass(in,w,in.h,true),w,h,false);
}
void Composite(Image& to, const Image& from, int dx, int dy) {
    for (int y=0;y<from.h && y+dy<to.h;++y)
        for (int x=0;x<from.w && x+dx<to.w;++x)
            if(x+dx>=0 && y+dy>=0) Over(to.At(x+dx,y+dy),from.At(x,y));
}
Image Region(const Image& source, int x, int y, int w, int h) {
    Image out(w,h);Copy(out,source,0,0,x,y,w,h);return out;
}
void Color(Image& image, const std::array<uint8_t,3>& rgb) {
    for(size_t i=0;i<image.rgba.size();i+=4)
        for(int c=0;c<3;++c) image.rgba[i+c]=rgb[c];
}
// Inner black keyline preserves the native cap alpha/shadow and lower bleed.
void TabBorder(Image& image,bool bleed) {
    const auto source=image;
    for(int y=0;y<image.h;++y)for(int x=0;x<image.w;++x) {
        bool edge=source.At(x,y)[3]<180;
        for(int dy=-3;dy<=3 && !edge;++dy)for(int dx=-3;dx<=3;++dx) {
            const int xx=x+dx,yy=y+dy;
            if(bleed && yy>=image.h)continue;
            if(xx<0 || xx>=image.w || yy<0 || yy>=image.h || source.At(xx,yy)[3]<180){edge=true;break;}
        }
        if(edge){auto* pixel=image.At(x,y);pixel[0]=18;pixel[1]=17;pixel[2]=23;}
    }
}
Image ActorPanel(const Image& half, int w, int h) {
    Image full(176,236),out(w,h);
    Copy(full,half,0,0,0,0,88,236);Copy(full,half,88,0,0,0,88,236,true);
    constexpr int border=24;
    const int sx[4]={0,border,176-border,176}, sy[4]={0,border,236-border,236};
    const int dx[4]={0,border,w-border,w}, dy[4]={0,border,h-border,h};
    for(int y=0;y<3;++y)for(int x=0;x<3;++x) {
        auto part=Region(full,sx[x],sy[y],sx[x+1]-sx[x],sy[y+1]-sy[y]);
        if(part.w!=dx[x+1]-dx[x] || part.h!=dy[y+1]-dy[y])
            part=Resize(part,dx[x+1]-dx[x],dy[y+1]-dy[y]);
        Copy(out,part,dx[x],dy[y],0,0,part.w,part.h);
    }
    return out;
}
} // namespace

struct Assets::Impl {
    explicit Impl(ReadAssetFn fn) : read(std::move(fn)) {}
    ReadAssetFn read;
    std::vector<uint8_t> source, pin_source;
    std::map<std::string, Image, std::less<>> textures, rendered;
    size_t image_bytes = 0;

    const Image* Texture(std::string_view name, int w, int h, std::string_view path=Source) {
        if(path!=Source && path!=PinSource)return nullptr;
        auto& bytes=path==Source?source:pin_source;
        if (const auto found = textures.find(name); found != textures.end()) return &found->second;
        if (bytes.empty()) {
            if (!read) return nullptr;
            const size_t size = read(path,0,nullptr,0);
            if (!size || size > MaxSource) return nullptr;
            std::vector<uint8_t> loaded(size);
            if (read(path,0,loaded.data(),size) != size) return nullptr;
            bytes = std::move(loaded);
        }
        Image decoded;
        if (!acnh::BntxDecode(bytes,name,decoded) || decoded.w != w || decoded.h != h ||
            decoded.Empty() || decoded.rgba.size() > MaxImages-image_bytes) return nullptr;
        image_bytes += decoded.rgba.size();
        return &textures.emplace(std::string(name),std::move(decoded)).first->second;
    }
    bool Make(std::string_view key, Image& out) {
        int w=0,h=0;
        if(key=="warp" || key=="warp-dark") {
            // Original map waypoint glyph. Header variant preserves its alpha
            // while using the accepted native dark button-text palette.
            const auto* warp=Texture("Warp_00^q",56,56);
            if(!warp)return false;
            out=*warp;
            if(key=="warp-dark")Color(out,{48,46,61});
            return true;
        }
        if(key=="rift-fill") {
            // Authored map-rift material only. Polygon projection, reveal clipping and
            // native activation checks belong to the caller; this is not a generic icon.
            const auto* fill=Texture("Map_FieldCrackActor^w",256,256);
            if(!fill)return false;
            out=*fill;return true;
        }
        if(key.starts_with("pin/")) {
            if(key.size()!=5 || key[4]<'1' || key[4]>'6')return false;
            // Companion kind numbers are local. Native UiMapIconPin IDs differ.
            constexpr std::array<std::string_view,6> names={"MapFlag_00","MapFlag_01","MapFlag_02",
                                                           "MapPin_02","MapPin_03","MapPin_04"};
            const auto* pin=Texture(names[size_t(key[4]-'1')],60,60,PinSource);
            if(!pin)return false;
            out=*pin;return true;
        }
        if (key == "dropdown-chevron") {
            out=Image(24,16);
            for(int y=0;y<12;++y)for(int x=y;x<24-y;++x) {
                auto* pixel=out.At(x,y);pixel[0]=48;pixel[1]=46;pixel[2]=61;pixel[3]=255;
            }
            return true;
        }
        if (key == "background" || key == "held-background") {
            // Owner correction: one quiet native cloth material across ordinary and held
            // contexts. No normal-map facets, simulated world fields or synthetic noise.
            const auto* cloth=Texture("MatCloth_00^w",1024,1024);
            if(!cloth)return false;
            const auto texture=Resize(*cloth,1240,1240);
            out=Image(1240,1080);
            constexpr int dark[3]={32,32,44},light[3]={53,52,66};
            for(int y=0;y<1080;++y)for(int x=0;x<1240;++x) {
                const auto* p=texture.At(x,y);auto* d=out.At(x,y);
                const unsigned gray=(19595u*p[0]+38470u*p[1]+7471u*p[2]+32768u)>>16;
                for(int c=0;c<3;++c)d[c]=uint8_t(dark[c]+gray*(light[c]-dark[c])/255);
                d[3]=255;
            }
            return true;
        }
        if(key=="notebook-tabs/0" || key=="notebook-tabs/1") {
            if(!Make("bottom-button/440/112",out))return false;
            for(int y=56;y<112;++y)for(int x=0;x<440;++x)std::memcpy(out.At(x,y),out.At(x,111-y),4);
            const int selected=key.back()-'0';
            for(int y=0;y<112;++y)for(int x=selected*220;x<(selected+1)*220;++x){auto* p=out.At(x,y);p[0]=210;p[1]=183;p[2]=125;}
            TabBorder(out,false);
            for(int y=0;y<112;++y)for(int x=219;x<=221;++x){auto* p=out.At(x,y);p[0]=18;p[1]=17;p[2]=23;}
            return true;
        }
        if(key.starts_with("sort-tabs/")) {
            if(key.size()!=11 || key[10]<'0' || key[10]>'4')return false;
            if(!Make("bottom-button/960/112",out))return false;
            for(int y=56;y<112;++y)for(int x=0;x<960;++x)
                std::memcpy(out.At(x,y),out.At(x,111-y),4);
            const int selected=key[10]-'0';
            for(int y=0;y<112;++y)for(int x=selected*192;x<(selected+1)*192;++x) {
                auto* pixel=out.At(x,y);pixel[0]=210;pixel[1]=183;pixel[2]=125;
            }
            TabBorder(out,false);
            for(int edge=192;edge<960;edge+=192)for(int y=0;y<112;++y)
                for(int x=edge-1;x<=edge+1;++x){auto* pixel=out.At(x,y);pixel[0]=18;pixel[1]=17;pixel[2]=23;}
            return true;
        }
        if(Dimensions(key,"bottom-button/",80,w,h) || Dimensions(key,"button/",80,w,h)) {
            // Owner-selected B: native filled capsule, with footer lower cap
            // continuing beyond the canvas. Keep cap shape and alpha/shadow.
            const auto* half=Texture("Com_Btn_Base_00^w",64,108);
            if(!half)return false;
            Image full(128,108);Copy(full,*half,0,0,0,0,64,108);Copy(full,*half,64,0,0,0,64,108,true);
            const bool footer=key.starts_with("bottom-button/");
            for(size_t i=0;i<full.rgba.size();i+=4) {
                full.rgba[i]=footer?98:230;full.rgba[i+1]=footer?97:223;full.rgba[i+2]=footer?112:199;
            }
            // Scale the original 128x108 assembly uniformly. A square target
            // must not squeeze the native button cap (and its shadow) to 1:1.
            const double scale=footer?56./128.:std::min(double(w)/128.,double(h)/108.);
            const int nw=std::max(2,int(std::lround(128*scale))),
                      nh=std::max(2,int(std::lround(108*scale))),cap=nw/2;
            const auto native=Resize(full,nw,nh);
            const int offset_y=footer?0:(h-nh)/2;
            out=Image(w,h);
            for(int y=offset_y;y<(footer?h:offset_y+nh);++y)for(int x=0;x<w;++x) {
                const int sx=x<cap?x:(x>=w-cap?nw-(w-x):cap);
                const int sy=footer?std::min(y,nh/2):y-offset_y;
                std::memcpy(out.At(x,y),native.At(sx,sy),4);
            }
            if(footer)TabBorder(out,true);
            return true;
        }
        if(key=="favorite-remove") {
            // Companion action glyph requested by the owner: remove a favorite.
            // This is an original functional symbol in the native cream/gold palette,
            // not an extracted game icon or an unrelated native icon repurposed.
            out=Image(96,96);
            const auto line=[&](double x1,double y1,double x2,double y2,double width,
                                const std::array<uint8_t,4>& color) {
                const double dx=x2-x1,dy=y2-y1,length=dx*dx+dy*dy;
                for(int y=0;y<96;++y)for(int x=0;x<96;++x) {
                    const double t=length?std::clamp(((x+.5-x1)*dx+(y+.5-y1)*dy)/length,0.,1.):0;
                    const double distance=std::hypot(x+.5-(x1+t*dx),y+.5-(y1+t*dy));
                    const double coverage=std::clamp(width/2+.5-distance,0.,1.);
                    if(coverage>0) {
                        auto pixel=color;pixel[3]=uint8_t(std::lround(pixel[3]*coverage));
                        Over(out.At(x,y),pixel.data());
                    }
                }
            };
            struct Stroke {double x1,y1,x2,y2;bool cream;};
            constexpr Stroke strokes[]={
                {17,27,79,27,true},{37,17,59,17,true},{37,17,35,25,true},{59,17,61,25,true},
                {26,37,30,79,false},{30,79,66,79,false},{66,79,70,37,false},
                {40,40,41,68,true},{56,40,55,68,true}};
            for(const auto& s:strokes)line(s.x1+2,s.y1+3,s.x2+2,s.y2+3,8,{0,0,0,100});
            for(const auto& s:strokes)line(s.x1,s.y1,s.x2,s.y2,7,
                s.cream?std::array<uint8_t,4>{255,243,215,255}:std::array<uint8_t,4>{243,217,157,255});
            return true;
        }
        if(key=="favorite-on" || key=="favorite-off") {
            // One authored favorite sprite supplies the exact star silhouette. The off
            // state outlines its white star ink; it is a derived control state, not a
            // claim that the game ships an independent empty-star asset.
            const auto* star=Texture("ItemFavo_00^w",48,48);
            if(!star)return false;
            out=*star;
            if(key=="favorite-off") {
                std::array<uint8_t,48*48> mask{};
                for(int y=0;y<48;++y)for(int x=0;x<48;++x) {
                    const auto* p=star->At(x,y);
                    const int white=std::min({p[0],p[1],p[2]});
                    mask[size_t(y)*48+x]=uint8_t(std::clamp((white-140)*255/80,0,255)*unsigned(p[3])/255);
                }
                for(int y=0;y<48;++y)for(int x=0;x<48;++x) {
                    unsigned inner=255;
                    for(int yy=std::max(0,y-1);yy<=std::min(47,y+1);++yy)
                        for(int xx=std::max(0,x-1);xx<=std::min(47,x+1);++xx)
                            inner=std::min(inner,unsigned(mask[size_t(yy)*48+xx]));
                    auto* p=out.At(x,y);p[0]=p[1]=p[2]=235;
                    p[3]=uint8_t(mask[size_t(y)*48+x]-inner);
                }
            }
            return true;
        }
        if(key=="sort-dot-on" || key=="sort-dot-off") {
            // Authored PageCounter sprites: filled current mode, hollow other modes.
            const auto* dot=Texture(key=="sort-dot-on"?"PageCount_01^t":"PageCount_00^t",32,32);
            if(!dot)return false;
            out=*dot;
            if(key=="sort-dot-off")Color(out,{104,104,104});
            return true;
        }
        if(key.starts_with("cost/")) {
            // L_ActorCost: cost triangles on a 16-degree arc of radius 84 above the
            // icon, tips inward, the arc centred for the count. Bright slots are
            // ActorCost_00 over the ActorCost_02 glow; reduced slots are the grey
            // ActorCost_01 at the arc's end, as in the native ActorSelect.
            int count=0,dim=0,percent=0;
            const auto rest=key.substr(5);const auto a=rest.find('/');
            const auto b=a==std::string_view::npos?a:rest.find('/',a+1);
            const auto number=[](std::string_view s,int& v){
                const auto r=std::from_chars(s.data(),s.data()+s.size(),v);
                return !s.empty() && r.ec==std::errc{} && r.ptr==s.data()+s.size();};
            if(b==std::string_view::npos || !number(rest.substr(0,a),count) ||
               !number(rest.substr(a+1,b-a-1),dim) || !number(rest.substr(b+1),percent) ||
               count<1 || count>6 || dim<0 || dim>=count || percent<50 || percent>300)return false;
            const auto* bright=Texture("ActorCost_00^w",24,24);
            const auto* grey=Texture("ActorCost_01^w",24,24);
            const auto* glow=Texture("ActorCost_02^w",24,24);
            if(!bright || !grey || !glow)return false;
            const double s=1.1*percent/100.,radius=84*s;
            const int tri=int(std::ceil(34*s)),width=int(std::ceil(2*radius*std::sin(48*3.14159265/180)+tri*2));
            const int height=int(std::ceil(radius*(1-std::cos(48*3.14159265/180))+tri*1.6));
            out=Image(width,height);
            const double cx=width/2.,cy=tri*.8+radius;
            // Bilinear sample of a rotated, scaled source centred at (px,py).
            const auto stamp=[&](const Image& src,double px,double py,double angle,double scale,double alpha) {
                const double c=std::cos(angle),sn=std::sin(angle),half=src.w*scale/2+2;
                for(int y=int(py-half*1.5);y<=int(py+half*1.5);++y)for(int x=int(px-half*1.5);x<=int(px+half*1.5);++x) {
                    if(x<0||y<0||x>=width||y>=height)continue;
                    const double dx=x+.5-px,dy=y+.5-py;
                    const double u=(c*dx+sn*dy)/scale+src.w/2.-.5,v=(-sn*dx+c*dy)/scale+src.h/2.-.5;
                    if(u<-1||v<-1||u>src.w||v>src.h)continue;
                    double acc[4]{};
                    const int u0=int(std::floor(u)),v0=int(std::floor(v));
                    for(int j=0;j<2;++j)for(int i=0;i<2;++i) {
                        const int su=u0+i,sv=v0+j;
                        if(su<0||sv<0||su>=src.w||sv>=src.h)continue;
                        const double w=(i?u-u0:1-(u-u0))*(j?v-v0:1-(v-v0));
                        const auto* p=src.At(su,sv);
                        for(int k=0;k<3;++k)acc[k]+=w*p[k]*p[3]/255.;acc[3]+=w*p[3];
                    }
                    if(acc[3]<=0)continue;
                    uint8_t pixel[4];
                    for(int k=0;k<3;++k)pixel[k]=uint8_t(std::clamp(acc[k]*255/acc[3],0.,255.));
                    pixel[3]=uint8_t(std::clamp(acc[3]*alpha,0.,255.));
                    Over(out.At(x,y),pixel);
                }
            };
            for(int i=0;i<count;++i) {
                const double angle=(16.*i-8.*(count-1))*3.14159265358979/180;
                const double px=cx+radius*std::sin(angle),py=cy-radius*std::cos(angle);
                const bool reduced=i>=count-dim;
                if(!reduced)stamp(*glow,px,py,angle,s*34/24.,.55);
                stamp(reduced?*grey:*bright,px,py,angle,s,1);
            }
            return true;
        }
        if(key=="footer") {
            // Native menu footer: dim plum band under the gold MainMenuLine border.
            const auto* line=Texture("MainMenuLine_00^w",667,25);
            if(!line)return false;
            out=Image(1240,208);
            for(int y=12;y<208;++y)for(int x=0;x<1240;++x) {
                const uint8_t layer[4]={40,22,34,175};std::memcpy(out.At(x,y),layer,4);
            }
            for(int x=0;x<1240;x+=line->w)Composite(out,*line,x,0);
            return true;
        }
        if(key=="quick-ring") {
            out=Image(208,208);
            for(int y=0;y<208;++y)for(int x=0;x<208;++x) {
                const double r=std::hypot(x-103.5,y-103.5);
                const double a=std::clamp(std::min(104-r,r-97.),0.,1.);
                auto* d=out.At(x,y);d[0]=114;d[1]=220;d[2]=111;d[3]=uint8_t(a*255);
            }
            return true;
        }
        if(Dimensions(key,"disc/",64,w,h) && w==h) {
            // HUD SetItem slot: grey MatPaper disc, soft rim, darker edge.
            const auto* paper=Texture("MatPaper_00^_A",256,256);
            if(!paper)return false;
            const auto p=Resize(*paper,w,w);
            double mean=0;
            for(int y=0;y<w;++y)for(int x=0;x<w;++x){const auto* s=p.At(x,y);mean+=(s[0]+s[1]+s[2])/3.;}
            mean/=double(w)*w;
            out=Image(w,w);
            for(int y=0;y<w;++y)for(int x=0;x<w;++x) {
                const auto* s=p.At(x,y);
                const double r=std::hypot(x-w/2.+.5,y-w/2.+.5)/(w/2.);
                const double edge=1-.18*std::clamp((r-.75)/.25,0.,1.);
                const double g=(118+((s[0]+s[1]+s[2])/3.-mean)*.35)*edge;
                auto* d=out.At(x,y);
                d[0]=uint8_t(std::clamp(g,0.,255.));d[1]=uint8_t(std::clamp(g*.985,0.,255.));
                d[2]=uint8_t(std::clamp(g*.955,0.,255.));
                d[3]=uint8_t(std::clamp((1-r)*w/3.,0.,1.)*235);
            }
            return true;
        }
        if(key.starts_with("badge/")) {
            // Native button glyph: cream disc, dark letter (footer/HUD prompt colours).
            const auto rest=key.substr(6);const auto slash=rest.find('/');
            if(slash==std::string_view::npos)return false;
            const auto name=rest.substr(0,slash);
            int size=0;
            if(!Dimensions(key.substr(6),std::string(name)+"/",32,size,h) || size!=h || size>256)return false;
            const bool dpad=name=="up";
            const auto* glyph=name=="x"?Texture("ButtonX_00^w",48,48):name=="y"?Texture("ButtonY_00^w",48,48):
                              name=="b"?Texture("ButtonB_00^w",48,48):name=="a"?Texture("ButtonA_00^w",48,48):dpad?Texture("ButtonUp_00^w",80,80):nullptr;
            if(!glyph)return false;
            constexpr uint8_t cream[3]={236,229,190},ink[3]={70,66,56};
            if(!dpad) {
                out=Resize(*glyph,size,size);
                for(size_t i=0;i<out.rgba.size();i+=4) {
                    const double l=out.rgba[i]/255.;
                    for(int c=0;c<3;++c)out.rgba[i+c]=uint8_t(std::lround(cream[c]*(1-l)+ink[c]*l));
                }
                return true;
            }
            out=Image(size,size);
            for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
                const double r=std::hypot(x-size/2.+.5,y-size/2.+.5)/(size/2.);
                auto* d=out.At(x,y);std::memcpy(d,cream,3);
                d[3]=uint8_t(std::clamp((1-r)*size/2.,0.,1.)*255);
            }
            auto mark=Resize(*glyph,size*3/4,size*3/4);
            for(size_t i=0;i<mark.rgba.size();i+=4)std::memcpy(&mark.rgba[i],ink,3);
            Composite(out,mark,size/8,size/8);
            return true;
        }
        if(Dimensions(key,"panel/",160,w,h)) {
            // Notebook detail window: dimmed DictionaryBG with the rope-pattern
            // corners; corners shrink uniformly for short panels.
            const auto* bg=Texture("DictionaryBG_00^w",101,149);
            const auto* lt=Texture("PatternWindowBG_00_LT^w",315,315);
            const auto* lb=Texture("PatternWindowBG_00_LB^w",315,315);
            if(!bg || !lt || !lb)return false;
            const int c=std::min({315,w/2,h/2});
            out=Resize(*bg,w,h);Color(out,{18,10,14});
            for(size_t i=3;i<out.rgba.size();i+=4)out.rgba[i]=uint8_t(out.rgba[i]*.55);
            for(int lower=0;lower<2;++lower) {
                Image corner=c==315?(lower?*lb:*lt):Resize(lower?*lb:*lt,c,c);
                for(size_t i=3;i<corner.rgba.size();i+=4)corner.rgba[i]=uint8_t(corner.rgba[i]*.45);
                Composite(out,corner,0,lower?h-c:0);
                Image mirror(c,c);Copy(mirror,corner,0,0,0,0,c,c,true);
                Composite(out,mirror,w-c,lower?h-c:0);
            }
            return true;
        }
        if(key.size()>=5 && (key.starts_with("nav/") || key.starts_with("nav-dim/"))) {
            const bool dim=key.starts_with("nav-dim/");
            const auto index=key.substr(dim?8:4);
            if(index.size()!=1 || index[0]<'0' || index[0]>'3')return false;
            constexpr std::array<std::string_view,4> names={"HeaderTab_05^w","HeaderTab_02^w","HeaderTab_00^w","HeaderTab_04^w"};
            const auto* icon=Texture(names[size_t(index[0]-'0')],80,80);
            if(!icon)return false;
            out=*icon;Color(out,dim?std::array<uint8_t,3>{205,180,128}:std::array<uint8_t,3>{243,217,157});return true;
        }
        if(key=="selectionglow") {
            const auto* light=Texture("MenuHeader_TextLight_00^w",350,118);
            if(!light)return false;
            out=Resize(*light,216,110);
            for(size_t i=0;i<out.rgba.size();i+=4) {
                const auto mask=uint8_t(double(out.rgba[i])*.2);
                out.rgba[i]=255;out.rgba[i+1]=200;out.rgba[i+2]=123;out.rgba[i+3]=mask;
            }
            return true;
        }
        if(key=="dictionary") {
            const auto* bg=Texture("DictionaryBG_00^w",101,149);
            const auto* lt=Texture("PatternWindowBG_00_LT^w",315,315);
            const auto* lb=Texture("PatternWindowBG_00_LB^w",315,315);
            if(!bg || !lt || !lb)return false;
            out=Resize(*bg,670,830);Color(out,{21,10,10});
            for(int lower=0;lower<2;++lower) {
                Image corner=lower?*lb:*lt;
                for(size_t i=3;i<corner.rgba.size();i+=4)corner.rgba[i]/=4;
                Composite(out,corner,0,lower?515:0);
                Image mirror(315,315);Copy(mirror,corner,0,0,0,0,315,315,true);
                Composite(out,mirror,355,lower?515:0);
            }
            return true;
        }
        if(key=="name-line" || key=="item-name-line" || key=="map-line" ||
           (Dimensions(key,"line/",40,w,h) && h==40 && w>=560)) {
            const auto* left=Texture("HeaderLine_00_L^w",232,40);
            const auto* middle=Texture("HeaderLine_00_C^w",248,40);
            const auto* right=Texture("HeaderLine_00_R^w",300,40);
            if(!left || !middle || !right)return false;
            if(!key.starts_with("line/"))w=key=="name-line"?618:key=="map-line"?900:1144;
            out=Image(w,40);Copy(out,*left,0,0,0,0,232,40);
            const auto center=Resize(*middle,w-532,40);Copy(out,center,232,0,0,0,w-532,40);
            Copy(out,*right,w-300,0,0,0,300,40);return true;
        }
        if(Dimensions(key,"actor/",48,w,h) || Dimensions(key,"actor-focus/",48,w,h)) {
            const auto* half=Texture(key.starts_with("actor-focus/")?"ActerBG_00Foxus^q":"ActerBG_00^w",88,236);
            if(!half)return false;
            out=ActorPanel(*half,w,h);return true;
        }
        if(key=="sort-banner") {
            const auto* banner=Texture("ActerSortBG_00^w",650,135);
            if(!banner)return false;
            // Preserve the main-screen selector's original ochre material.
            out=*banner;
            return true;
        }
        if(key=="name-banner") {
            const auto* banner=Texture("ActerNameBG_00^w",648,128);
            if(!banner)return false;
            out=Image(1240,128);
            Copy(out,*banner,0,0,0,0,648,128);Copy(out,*banner,648,0,0,0,592,128);
            return true;
        }
        if(key=="equipped-check") {
            const auto* check=Texture("ItemOn_00^w",48,48);
            if(!check)return false;
            out=Resize(*check,64,64);return true;
        }
        if (key == "player") {
            const auto* player=Texture("Icon_Player_00^q",64,64);
            if(!player)return false;
            out=*player;return true;
        }
        if (key == "divider") {
            const auto* line=Texture("DialogLinePattern_00^t",1064,46);
            if (!line) return false;
            out=*line;
            for(size_t i=0;i<out.rgba.size();i+=4) {
                out.rgba[i]=232;out.rgba[i+1]=200;out.rgba[i+2]=115;
            }
            return true;
        }
        if (Dimensions(key,"cell/",96,w,h)) {
            const auto* a=Texture("ItemBase_00_LT^w",48,48);
            const auto* b=Texture("ItemBase_00_LB^w",48,48);
            if (!a || !b) return false;
            out=Image(w,h);
            Copy(out,*a,0,0,0,0,48,48);Copy(out,*a,w-48,0,0,0,48,48,true);
            Copy(out,*b,0,h-48,0,0,48,48);Copy(out,*b,w-48,h-48,0,0,48,48,true);
            for(int x=48;x<w-48;++x) {
                Copy(out,*a,x,0,47,0,1,48);Copy(out,*b,x,h-48,47,0,1,48);
            }
            for(int y=48;y<h-48;++y) {
                Copy(out,*a,0,y,0,47,48,1);Copy(out,*a,w-48,y,0,47,48,1,true);
                for(int x=48;x<w-48;++x) std::memcpy(out.At(x,y),a->At(47,47),4);
            }
            return true;
        }
        if (Dimensions(key,"cursor/",62,w,h)) {
            const auto* a=Texture("ComCursor_00^q",62,62);
            const auto* b=Texture("ComCursor_01^q",62,62);
            if(!a || !b) return false;
            out=Image(w,h);
            auto corner = [&](const Image& image, int dx, int dy, bool flip_x, bool flip_y) {
                for(int y=0;y<62;++y) for(int x=0;x<62;++x)
                    Over(out.At(dx+x,dy+y),image.At(flip_x?61-x:x,flip_y?61-y:y));
            };
            corner(*a,0,0,false,false);corner(*a,w-62,0,true,false);
            // Both source textures are authored upper-left caps. L_ComCursor's
            // lower frames use vertical flip (LB) and180-degree rotation (RB).
            corner(*b,0,h-62,false,true);corner(*b,w-62,h-62,true,true);
            return true;
        }
        return false;
    }
};
Assets::Assets(ReadAssetFn read) : impl_(std::make_unique<Impl>(std::move(read))) {}
Assets::~Assets() = default;
bool Assets::Load(std::string_view key, Image& out) {
    out={};
    try {
        if (const auto found=impl_->rendered.find(key);found!=impl_->rendered.end()) {
            out=found->second;return true;
        }
        Image image;
        if (!impl_->Make(key,image) || image.Empty()) return false;
        if (image.rgba.size() > MaxImages-impl_->image_bytes) {
            for(const auto& entry:impl_->rendered) impl_->image_bytes-=entry.second.rgba.size();
            impl_->rendered.clear();
        }
        if (image.rgba.size() > MaxImages-impl_->image_bytes) return false;
        impl_->image_bytes+=image.rgba.size();
        out=image;
        impl_->rendered.emplace(std::string(key),std::move(image));
        return true;
    } catch (...) { out={};return false; }
}
} // namespace echoes
