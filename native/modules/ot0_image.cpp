// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_image.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
namespace ot0 {
namespace {
bool ValidSize(u32 w, u32 h) { return w && h && w <= 4096 && h <= 4096; }
bool Valid(const Image& im) {
    // Source maps may exceed 4096 in one dimension; cap total allocation instead.
    return im.width && im.height && im.width <= 16384 && im.height <= 16384 &&
        static_cast<std::size_t>(im.width) * im.height <= 32 * 1024 * 1024 &&
        im.rgba.size() == static_cast<std::size_t>(im.width) * im.height * 4;
}
constexpr int PrecisionBits = 32 - 8 - 2; // Pillow's 8-bit resampling precision
constexpr double Pi = 3.14159265358979323846;

double Sinc(double x) {
    if (x == 0.0)
        return 1.0;
    x *= Pi;
    return std::sin(x) / x;
}
double Lanczos(double x) {
    return (-3.0 <= x && x < 3.0) ? Sinc(x) * Sinc(x / 3.0) : 0.0;
}

struct Coeffs {
    int ksize{};
    std::vector<int> bounds; // xmin, count per output
    std::vector<int> k;      // fixed point
};

// Pillow precompute_coeffs + normalize_coeffs_8bpc (Resample.c) for LANCZOS.
Coeffs Precompute(int in_size, int out_size) {
    const double scale = static_cast<double>(in_size) / out_size;
    const double filterscale = scale < 1.0 ? 1.0 : scale;
    const double support = 3.0 * filterscale;
    Coeffs c;
    c.ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
    c.bounds.resize(static_cast<std::size_t>(out_size) * 2);
    c.k.assign(static_cast<std::size_t>(out_size) * c.ksize, 0);
    std::vector<double> pre(static_cast<std::size_t>(c.ksize));
    for (int xx = 0; xx < out_size; ++xx) {
        const double center = (xx + 0.5) * scale;
        const double ss = 1.0 / filterscale;
        int xmin = static_cast<int>(center - support + 0.5);
        if (xmin < 0)
            xmin = 0;
        int xmax = static_cast<int>(center + support + 0.5);
        if (xmax > in_size)
            xmax = in_size;
        xmax -= xmin;
        double ww = 0.0;
        for (int x = 0; x < xmax; ++x) {
            const double w = Lanczos((x + xmin - center + 0.5) * ss);
            pre[static_cast<std::size_t>(x)] = w;
            ww += w;
        }
        for (int x = 0; x < xmax; ++x) {
            double v = ww != 0.0 ? pre[static_cast<std::size_t>(x)] / ww : 0.0;
            c.k[static_cast<std::size_t>(xx) * c.ksize + x] =
                static_cast<int>(v * (1 << PrecisionBits) + (v < 0 ? -0.5 : 0.5));
        }
        c.bounds[static_cast<std::size_t>(xx) * 2] = xmin;
        c.bounds[static_cast<std::size_t>(xx) * 2 + 1] = xmax;
    }
    return c;
}

u8 Clip8(long long v) {
    if (v >= (1LL << PrecisionBits) * 256)
        return 255;
    if (v <= 0)
        return 0;
    return static_cast<u8>(v >> PrecisionBits);
}

// Pillow RGBA -> RGBa (MULDIV255) and back.
void Premultiply(std::vector<u8>& px) {
    for (std::size_t i = 0; i < px.size(); i += 4) {
        const unsigned a = px[i + 3];
        for (int c = 0; c < 3; ++c) {
            const unsigned t = px[i + c] * a + 128;
            px[i + c] = static_cast<u8>(((t >> 8) + t) >> 8);
        }
    }
}
void Unpremultiply(std::vector<u8>& px) {
    for (std::size_t i = 0; i < px.size(); i += 4) {
        const unsigned a = px[i + 3];
        if (a == 0 || a == 255)
            continue;
        for (int c = 0; c < 3; ++c)
            px[i + c] = static_cast<u8>(std::min(255u, (255u * px[i + c]) / a));
    }
}

} // namespace
Image Resize(const Image& src, u32 x0, u32 y0, u32 w, u32 h, u32 out_w, u32 out_h) {
    if (!Valid(src) || !w || !h || !ValidSize(out_w, out_h) ||
        x0 > src.width || y0 > src.height || w > src.width - x0 || h > src.height - y0)
        return {};
    Image crop{w, h, std::vector<u8>(std::size_t{w} * h * 4)};
    for (u32 y = 0; y < h; ++y)
        std::memcpy(crop.rgba.data() + std::size_t{y} * w * 4,
                    src.rgba.data() + ((std::size_t{y0} + y) * src.width + x0) * 4,
                    std::size_t{w} * 4);
    if (out_w == w && out_h == h)
        return crop;
    Premultiply(crop.rgba);
    Image cur = std::move(crop);
    if (out_w != cur.width) { // horizontal pass first, 8-bit intermediate (as Pillow)
        const Coeffs c = Precompute(static_cast<int>(cur.width), static_cast<int>(out_w));
        Image next{out_w, cur.height, std::vector<u8>(std::size_t{out_w} * cur.height * 4)};
        for (u32 y = 0; y < cur.height; ++y)
            for (u32 xx = 0; xx < out_w; ++xx) {
                const int xmin = c.bounds[std::size_t{xx} * 2], n = c.bounds[std::size_t{xx} * 2 + 1];
                const int* k = &c.k[std::size_t{xx} * c.ksize];
                for (int ch = 0; ch < 4; ++ch) {
                    long long ss = 1LL << (PrecisionBits - 1);
                    for (int x = 0; x < n; ++x)
                        ss += static_cast<long long>(
                                  cur.rgba[(std::size_t{y} * cur.width + xmin + x) * 4 + ch]) *
                              k[x];
                    next.rgba[(std::size_t{y} * out_w + xx) * 4 + ch] = Clip8(ss);
                }
            }
        cur = std::move(next);
    }
    if (out_h != cur.height) {
        const Coeffs c = Precompute(static_cast<int>(cur.height), static_cast<int>(out_h));
        Image next{cur.width, out_h, std::vector<u8>(std::size_t{cur.width} * out_h * 4)};
        for (u32 yy = 0; yy < out_h; ++yy) {
            const int ymin = c.bounds[std::size_t{yy} * 2], n = c.bounds[std::size_t{yy} * 2 + 1];
            const int* k = &c.k[std::size_t{yy} * c.ksize];
            for (u32 x = 0; x < cur.width; ++x)
                for (int ch = 0; ch < 4; ++ch) {
                    long long ss = 1LL << (PrecisionBits - 1);
                    for (int y = 0; y < n; ++y)
                        ss += static_cast<long long>(
                                  cur.rgba[((std::size_t{static_cast<u32>(ymin + y)}) * cur.width + x) * 4 + ch]) *
                              k[y];
                    next.rgba[(std::size_t{yy} * cur.width + x) * 4 + ch] = Clip8(ss);
                }
        }
        cur = std::move(next);
    }
    Unpremultiply(cur.rgba);
    return cur;
}

void Over(Image& dst, const Image& src, int x, int y) {
    if (!Valid(dst) || !Valid(src)) return;
    for (u32 sy = 0; sy < src.height; ++sy) {
        const int dy = y + static_cast<int>(sy);
        if (dy < 0 || dy >= static_cast<int>(dst.height))
            continue;
        for (u32 sx = 0; sx < src.width; ++sx) {
            const int dx = x + static_cast<int>(sx);
            if (dx < 0 || dx >= static_cast<int>(dst.width))
                continue;
            const u8* s = &src.rgba[(std::size_t{sy} * src.width + sx) * 4];
            u8* d = &dst.rgba[(static_cast<std::size_t>(dy) * dst.width + dx) * 4];
            const double sa = s[3] / 255.0, da = d[3] / 255.0;
            const double oa = sa + da * (1.0 - sa);
            if (oa <= 0.0) {
                d[0] = d[1] = d[2] = d[3] = 0;
                continue;
            }
            for (int c = 0; c < 3; ++c)
                d[c] = static_cast<u8>(std::lround((s[c] * sa + d[c] * da * (1.0 - sa)) / oa));
            d[3] = static_cast<u8>(std::lround(oa * 255.0));
        }
    }
}

Image Nine(const Image& src, u32 w, u32 h, float margin) {
    if (!Valid(src) || !ValidSize(w,h) || !std::isfinite(margin) || margin <= 0 || margin >= .5f)
        return {};
    const auto sxm = static_cast<u32>(std::lround(static_cast<float>(src.width) * margin));
    const auto sym = static_cast<u32>(std::lround(static_cast<float>(src.height) * margin));
    const auto dxm = std::min(sxm, w / 2), dym = std::min(sym, h / 2);
    const u32 sx[] = {0, sxm, src.width - sxm, src.width};
    const u32 sy[] = {0, sym, src.height - sym, src.height};
    const u32 dx[] = {0, dxm, w-dxm, w}, dy[] = {0, dym, h-dym, h};
    Image out{w,h,std::vector<u8>(static_cast<std::size_t>(w)*h*4)};
    for (int j=0;j<3;++j) for (int i=0;i<3;++i) {
        if (dx[i+1]==dx[i] || dy[j+1]==dy[j] || sx[i+1]==sx[i] || sy[j+1]==sy[j]) continue;
        Over(out,Resize(src,sx[i],sy[j],sx[i+1]-sx[i],sy[j+1]-sy[j],
                        dx[i+1]-dx[i],dy[j+1]-dy[j]),static_cast<int>(dx[i]),static_cast<int>(dy[j]));
    }
    return out;
}

Image RedKey(const Image& src) {
    if (!Valid(src)) return {};
    Image out=src;
    for (std::size_t i=0;i<out.rgba.size();i+=4)
        if (out.rgba[i]==255 && out.rgba[i+1]==0 && out.rgba[i+2]==0) out.rgba[i+3]=0;
    return out;
}

Image DarkenBlur(const Image& src, double brightness, u32 radius) {
    if (!Valid(src) || !std::isfinite(brightness) || brightness < 0 || brightness > 1 || radius>128)
        return {};
    Image out=src;
    Premultiply(out.rgba);
    // Sliding separable box kernel, clamped edges; O(width*height), independent of radius.
    // Three equal passes approximate a Gaussian with sigma ~= radius (sqrt(3)*r/sqrt(3)).
    std::vector<u8> temp(out.rgba.size());
    const int r=static_cast<int>(radius), divisor=2*r+1;
    if (r) for (int pass=0;pass<3;++pass) {
        for (u32 y=0;y<out.height;++y) for (int c=0;c<4;++c) {
            int sum=0;
            const auto at=[&](int x) { return out.rgba[(static_cast<std::size_t>(y)*out.width+
                static_cast<u32>(std::clamp(x,0,static_cast<int>(out.width)-1)))*4+c]; };
            for (int x=-r;x<=r;++x) sum+=at(x);
            for (u32 x=0;x<out.width;++x) {
                temp[(static_cast<std::size_t>(y)*out.width+x)*4+c]=static_cast<u8>((sum+divisor/2)/divisor);
                sum+=at(static_cast<int>(x)+r+1)-at(static_cast<int>(x)-r);
            }
        }
        for (u32 x=0;x<out.width;++x) for (int c=0;c<4;++c) {
            int sum=0;
            const auto at=[&](int y) { return temp[(static_cast<std::size_t>(
                std::clamp(y,0,static_cast<int>(out.height)-1))*out.width+x)*4+c]; };
            for (int y=-r;y<=r;++y) sum+=at(y);
            for (u32 y=0;y<out.height;++y) {
                out.rgba[(static_cast<std::size_t>(y)*out.width+x)*4+c]=static_cast<u8>((sum+divisor/2)/divisor);
                sum+=at(static_cast<int>(y)+r+1)-at(static_cast<int>(y)-r);
            }
        }
    }
    Unpremultiply(out.rgba);
    for (std::size_t i=0;i<out.rgba.size();i+=4)
        for (int c=0;c<3;++c) out.rgba[i+c]=static_cast<u8>(std::lround(out.rgba[i+c]*brightness));
    return out;
}

std::optional<std::u32string> DecodeUtf8(std::string_view text) {
    if (text.size()>16384) return std::nullopt;
    std::u32string result;
    for (std::size_t i=0;i<text.size();) {
        const u8 lead=static_cast<u8>(text[i++]);
        u32 cp; int count; u32 minimum;
        if (lead<0x80) { cp=lead; count=0; minimum=0; }
        else if (lead>=0xc2 && lead<=0xdf) { cp=lead&31; count=1; minimum=0x80; }
        else if (lead>=0xe0 && lead<=0xef) { cp=lead&15; count=2; minimum=0x800; }
        else if (lead>=0xf0 && lead<=0xf4) { cp=lead&7; count=3; minimum=0x10000; }
        else return std::nullopt;
        if (static_cast<std::size_t>(count)>text.size()-i) return std::nullopt;
        while (count--) {
            const u8 next=static_cast<u8>(text[i++]);
            if ((next&0xc0)!=0x80) return std::nullopt;
            cp=(cp<<6)|(next&63);
        }
        if (cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return std::nullopt;
        result.push_back(static_cast<char32_t>(cp));
    }
    return result;
}

struct FontLibrary::Impl {
    struct Fonts {
        std::vector<u8> files[4];
        stbtt_fontinfo faces[4]{};
    };
    mutable std::mutex mutex;
    std::shared_ptr<const Fonts> fonts;
    std::shared_ptr<const Fonts> Get() const { std::lock_guard lock(mutex); return fonts; }
};
FontLibrary::FontLibrary():impl_(std::make_unique<Impl>()) {}
FontLibrary::~FontLibrary()=default;
FontLibrary::FontLibrary(FontLibrary&&) noexcept=default;
FontLibrary& FontLibrary::operator=(FontLibrary&&) noexcept=default;
bool FontLibrary::Ready() const { return impl_ && bool(impl_->Get()); }
bool FontLibrary::Load(const RangeReader& read, std::string* why) {
    if (!impl_) impl_=std::make_unique<Impl>();
    std::lock_guard lock(impl_->mutex);
    if (impl_->fonts) return true;
    auto f=std::make_shared<Impl::Fonts>();
    const MemberId ids[]={MemberId::FontFranco, MemberId::FontFrancoBold, MemberId::FontRocio, MemberId::FontClearTone};
    for (int i=0;i<4;++i) {
        auto file=ReadMember(read,Member(ids[i]),why);
        if (!file) return false;
        f->files[i]=std::move(*file);
        const auto& bytes=f->files[i];
        const bool signature=bytes.size()>=12 && (std::memcmp(bytes.data(),"OTTO",4)==0 ||
            (bytes[0]==0 && bytes[1]==1 && bytes[2]==0 && bytes[3]==0));
        if (!signature) { if (why) *why="Native font has unsupported SFNT signature"; return false; }
        const int offset=stbtt_GetFontOffsetForIndex(bytes.data(),0);
        if (offset<0 || !stbtt_InitFont(&f->faces[i],bytes.data(),offset)) {
            if (why) *why="Native font initialization failed";
            return false;
        }
    }
    impl_->fonts=std::move(f);
    if (why) why->clear();
    return true;
}
namespace {
bool TextArgs(std::u32string_view text,FontFace face,float size) {
    if (static_cast<unsigned>(face)>3 || !std::isfinite(size) || size<1 || size>256 || text.size()>4096)
        return false;
    for (const char32_t cp:text)
        if (cp<32 || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff) || (cp>=0x7f && cp<0xa0)) return false;
    return true;
}
std::optional<int> TextWidth(const stbtt_fontinfo& f,float scale,std::u32string_view text) {
    int width=0;
    for (char32_t cp:text) {
        const int gi=stbtt_FindGlyphIndex(&f,static_cast<int>(cp));
        if (!gi) return std::nullopt;
        int advance,bearing; stbtt_GetGlyphHMetrics(&f,gi,&advance,&bearing);
        width+=static_cast<int>(std::lround(static_cast<float>(advance)*scale));
        if (width<0 || width>65536) return std::nullopt;
    }
    return width;
}
}
std::optional<int> FontLibrary::Measure(std::u32string_view text,FontFace face,float size) const {
    if (!impl_ || !TextArgs(text,face,size)) return std::nullopt;
    const auto f=impl_->Get(); if (!f) return std::nullopt;
    const auto& info=f->faces[static_cast<int>(face)];
    return TextWidth(info,stbtt_ScaleForMappingEmToPixels(&info,size),text);
}
std::optional<int> FontLibrary::Measure(std::string_view text,FontFace face,float size) const {
    const auto decoded=DecodeUtf8(text);
    return decoded ? Measure(*decoded,face,size) : std::nullopt;
}
bool FontLibrary::DrawGameText(Image& dst,std::string_view text,FontFace face,float size,
                               int x,int y,std::array<u8,4> color,char ah,char av) const {
    const auto decoded=DecodeUtf8(text);
    return decoded && DrawGameText(dst,*decoded,face,size,x,y,color,ah,av);
}
bool FontLibrary::DrawGameText(Image& dst,std::u32string_view text,FontFace face,float size,
                               int x,int y,std::array<u8,4> color,char ah,char av) const {
    if (!impl_ || !Valid(dst) || !TextArgs(text,face,size) ||
        (ah!='l' && ah!='m' && ah!='r') || (av!='a' && av!='m' && av!='s' && av!='b' && av!='t') ||
        x < -65536 || x>65536 || y < -65536 || y>65536) return false;
    const auto f=impl_->Get(); if (!f) return false;
    const auto& info=f->faces[static_cast<int>(face)];
    const float scale=stbtt_ScaleForMappingEmToPixels(&info,size);
    const auto width=TextWidth(info,scale,text); if (!width) return false;
    int asc,desc,gap; stbtt_GetFontVMetrics(&info,&asc,&desc,&gap);
    asc=static_cast<int>(std::ceil(static_cast<float>(asc)*scale-1e-4f));
    desc=static_cast<int>(std::ceil(-static_cast<float>(desc)*scale-1e-4f));
    int pen=ah=='m'?x-*width/2:ah=='r'?x-*width:x;
    int top=0;
    if (av=='t') { // Text bounding-box top, matching the mockup renderer's 'mt'.
        bool first=true;
        for (char32_t cp:text) {
            int x0,y0,x1,y1; stbtt_GetCodepointBitmapBox(&info,static_cast<int>(cp),scale,scale,&x0,&y0,&x1,&y1);
            if (x1>x0 && y1>y0) { top=first?y0:std::min(top,y0); first=false; }
        }
    }
    const int base=av=='a'?y+asc:av=='m'?y+static_cast<int>(std::lround((asc-desc)/2.0)):
                   av=='b'?y-desc:av=='t'?y-top:y;
    // Per-glyph coverage is bounded by the selected 256 px em. No whole-string bitmap allocation.
    for (char32_t cp:text) {
        const int gi=stbtt_FindGlyphIndex(&info,static_cast<int>(cp));
        int adv,bearing,x0,y0,x1,y1;
        stbtt_GetGlyphHMetrics(&info,gi,&adv,&bearing);
        stbtt_GetGlyphBitmapBox(&info,gi,scale,scale,&x0,&y0,&x1,&y1);
        const int w=x1-x0,h=y1-y0;
        if (w>0 && h>0 && w<=1024 && h<=1024) {
            std::vector<u8> coverage(static_cast<std::size_t>(w)*h);
            stbtt_MakeGlyphBitmap(&info,coverage.data(),w,h,w,scale,scale,gi);
            for (int yy=0;yy<h;++yy) for (int xx=0;xx<w;++xx) {
                const int dx=pen+x0+xx,dy=base+y0+yy;
                if (dx<0 || dy<0 || dx>=static_cast<int>(dst.width) || dy>=static_cast<int>(dst.height)) continue;
                const u8 a=coverage[static_cast<std::size_t>(yy)*w+xx]; if (!a || !color[3]) continue;
                u8* p=&dst.rgba[(static_cast<std::size_t>(dy)*dst.width+dx)*4];
                const double sa=(a/255.0)*(color[3]/255.0),da=p[3]/255.0,oa=sa+da*(1-sa);
                for (int c=0;c<3;++c) p[c]=static_cast<u8>(std::lround((color[c]*sa+p[c]*da*(1-sa))/oa));
                p[3]=static_cast<u8>(std::lround(oa*255));
            }
        }
        pen+=static_cast<int>(std::lround(static_cast<float>(adv)*scale));
    }
    return true;
}
} // namespace ot0
