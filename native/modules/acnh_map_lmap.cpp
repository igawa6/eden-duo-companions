// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// LMap layout passes over the UI-map RT. Evidence: research/acnh/impl/fix/map.md ("LMap passes"):
// the bflyt mat1 bytes (decoded below), the layout archive shader (bgsh: vertex texgen + the
// EachIndirect fragment program) and the uniform blocks the game uploaded for P_Map_00,
// P_MapPattern_00/01/02 and P_Capture_00 while the Map app was open (live, gdb read of GPU memory).

#include "acnh_map_lmap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

#include "acnh_bytes.h"
#include "acnh_lyt.h"
#include "acnh_romfs.h"

namespace acnh {
namespace {

using bytes::F32;
using bytes::Fits;
using bytes::U8;
constexpr auto U16 = bytes::Le16;
constexpr auto U32 = bytes::Le32;

// nn::ui2d enums as stored in mat1 (Switch-Toolbox BxlytSwitch layout, a LEAD; each value below
// is confirmed by the uniform block the game uploaded for that pane).
enum AlphaFunc : uint8_t { Never, Less, LessEqual, Equal, NotEqual, GreaterEqual, Greater, Always };
enum BlendOp : uint8_t { OpDisable, OpAdd, OpSubtract, OpReverseSubtract, OpMin, OpMax };
enum BlendFactor : uint8_t {
    FZero,
    FOne,
    FDstColor,
    FInvDstColor,
    FSrcAlpha,
    FInvSrcAlpha,
    FDstAlpha,
    FInvDstAlpha,
    FSrcColor,
    FInvSrcColor
};

struct TexMap {
    std::string tex;
    std::shared_ptr<const Image> img;
    bool repeat = false;
    float srt[5] = {0, 0, 0, 1, 1}; // tx, ty, rot (deg), sx, sy
    float cs = 1, sn = 0;           // cos / sin of the SRT rotation
    bool srgb = false;              // *_SRGB texture format (sampled linear)
    bool projected = false;         // texgen source 4 = pane-based projection
    bool alpha_one = false;         // every texel alpha 255 (selector A = one)
};

struct Mat {
    std::string name;
    std::array<float, 4> black{}, white{};
    std::vector<TexMap> maps;
    uint8_t color_mode0 = 0, alpha_mode0 = 0, stages = 0;
    bool has_cmp = false;
    uint8_t cmp_func = Always;
    float cmp_ref = 0;
    bool has_blend = false, has_blend_a = false;
    uint8_t op = OpAdd, src = FSrcAlpha, dst = FInvSrcAlpha;
    uint8_t op_a = OpAdd, src_a = FSrcAlpha, dst_a = FInvSrcAlpha;
    bool has_ind = false;
    float ind[3] = {0, 0, 0};     // rot, sx, sy
    bool alpha_threshold = false; // flag bit 18: ThresholdingAlphaInterpolation
    bool capture_first = false;   // texmap0 = the runtime capture (Capture^t placeholder)
};

struct PaneDef {
    std::string name;
    float w = 0, h = 0;
    int mat = -1;
};

float Lin(float s) { // sRGB -> linear (texture sampler of an *_SRGB texture)
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

uint8_t Srgb8Slow(float x) {
    x = std::clamp(x, 0.f, 1.f);
    const float s = x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(s * 255.f));
}

struct Lut {
    std::array<float, 256> lin{};
    std::array<float, 255> step{};     // smallest linear value that encodes to k+1
    std::array<uint8_t, 4097> guess{}; // Srgb8 at x = i / 4096 (start of the threshold walk)
    Lut() {
        for (int i = 0; i < 256; ++i)
            lin[i] = Lin(i / 255.f);
        for (int k = 0; k < 255; ++k) { // bisection on the exact encoder
            float lo = 0.f, hi = 1.f;
            for (int it = 0; it < 40; ++it) {
                const float mid = (lo + hi) / 2;
                (Srgb8Slow(mid) > k ? hi : lo) = mid;
            }
            step[k] = hi;
        }
        for (int i = 0; i <= 4096; ++i)
            guess[i] = static_cast<uint8_t>(std::upper_bound(step.begin(), step.end(), i / 4096.f) -
                                            step.begin());
    }
};
const Lut& SrgbLut() {
    static const Lut l;
    return l;
}

uint8_t Srgb8(float x) { // == Srgb8Slow(x): table guess at the grid point below x, then walk up
    const Lut& l = SrgbLut();
    if (!(x > 0.f))
        return 0;
    if (x >= 1.f)
        return 255;
    int k = l.guess[static_cast<int>(x * 4096.f)];
    while (k < 255 && l.step[k] <= x)
        ++k;
    return static_cast<uint8_t>(k);
}

// Bilinear texture sample (texel centres at +0.5), channels as decoded (selectors applied),
// 0..1; `lin` linearises the colour channels (sRGB texture formats).
std::array<float, 4> Sample(const Image& im, float u, float v, bool repeat, bool lin) {
    const float x = u * im.w - 0.5f, y = v * im.h - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    auto wrap = [&](int c, int n) {
        if (repeat) {
            c %= n;
            return c < 0 ? c + n : c;
        }
        return std::clamp(c, 0, n - 1);
    };
    const int xa = wrap(x0, im.w), xb = wrap(x0 + 1, im.w);
    const int ya = wrap(y0, im.h), yb = wrap(y0 + 1, im.h);
    const uint8_t* pa = im.rgba.data() + (size_t(ya) * im.w + xa) * 4;
    const uint8_t* pb = im.rgba.data() + (size_t(ya) * im.w + xb) * 4;
    const uint8_t* pc = im.rgba.data() + (size_t(yb) * im.w + xa) * 4;
    const uint8_t* pd = im.rgba.data() + (size_t(yb) * im.w + xb) * 4;
    const auto& L = SrgbLut().lin;
    std::array<float, 4> o{};
    for (int c = 0; c < 4; ++c) {
        float a, b, cc, d;
        if (lin && c < 3)
            a = L[pa[c]], b = L[pb[c]], cc = L[pc[c]], d = L[pd[c]];
        else
            a = pa[c] / 255.f, b = pb[c] / 255.f, cc = pc[c] / 255.f, d = pd[c] / 255.f;
        const float top = a + (b - a) * tx, bot = cc + (d - cc) * tx;
        o[c] = top + (bot - top) * ty;
    }
    return o;
}

// Texture coordinate of one texmap at a pane-local point (layout units, y up, origin = pane
// centre). Projection (vertex shader c3[0xe0+0x40i] rows as uploaded): u0 = x / tex_w + 0.5,
// v0 = -y / tex_h + 0.5. Texture SRT about (0.5, 0.5) (c3[0x70+0x20i] rows as uploaded: e.g.
// grass 2.5 x rot 10 deg -> (2.46204, -0.43412, -0.51396), (-0.43412, -2.46204, 1.94808) with
// t = 1 - row1 . uv). Non-projected maps use the pane UVs (0..1 across the pane).
void TexCoord(const TexMap& m, float lx, float ly, float pw, float ph, float& u, float& v) {
    float u0, v0;
    if (m.projected) {
        u0 = lx / m.img->w + 0.5f;
        v0 = -ly / m.img->h + 0.5f;
    } else {
        u0 = lx / pw + 0.5f;
        v0 = -ly / ph + 0.5f;
    }
    const float c = m.cs, s = m.sn;
    const float du = u0 - 0.5f, dv = v0 - 0.5f;
    u = 0.5f + m.srt[3] * c * du - m.srt[4] * s * dv + m.srt[0];
    v = 0.5f + m.srt[3] * s * du + m.srt[4] * c * dv - m.srt[1];
}

float Factor(uint8_t f, const std::array<float, 4>& s, const std::array<float, 4>& d, int ch) {
    switch (f) {
    case FZero:
        return 0.f;
    case FOne:
        return 1.f;
    case FDstColor:
        return d[ch];
    case FInvDstColor:
        return 1.f - d[ch];
    case FSrcAlpha:
        return s[3];
    case FInvSrcAlpha:
        return 1.f - s[3];
    case FDstAlpha:
        return d[3];
    case FInvDstAlpha:
        return 1.f - d[3];
    case FSrcColor:
        return s[ch];
    case FInvSrcColor:
        return 1.f - s[ch];
    default:
        return 0.f;
    }
}

float BlendCh(uint8_t op, uint8_t sf, uint8_t df, const std::array<float, 4>& s,
              const std::array<float, 4>& d, int ch) {
    const float a = s[ch] * Factor(sf, s, d, ch), b = d[ch] * Factor(df, s, d, ch);
    switch (op) {
    case OpAdd:
        return a + b;
    case OpSubtract:
        return a - b;
    case OpReverseSubtract:
        return b - a;
    case OpMin:
        return std::min(s[ch], d[ch]);
    case OpMax:
        return std::max(s[ch], d[ch]);
    default:
        return s[ch];
    }
}

bool CmpPass(uint8_t f, float a, float ref) {
    switch (f) {
    case Never:
        return false;
    case Less:
        return a < ref;
    case LessEqual:
        return a <= ref;
    case Equal:
        return a == ref;
    case NotEqual:
        return a != ref;
    case GreaterEqual:
        return a >= ref;
    case Greater:
        return a > ref;
    default:
        return true;
    }
}

} // namespace

struct LMapLayers::Impl {
    std::mutex mu;
    bool tried = false, ok = false;
    std::vector<Mat> mats;
    std::vector<PaneDef> capture_children; // N_Capture_00 children in draw order
    PaneDef capture_pane;                  // P_Capture_00
    float capture_w = 640, capture_h = 640;
    std::array<float, 4> clear{38 / 255.f, 165 / 255.f, 148 / 255.f, 0.f};
    std::shared_ptr<Layout> layout;
    // composed capture of the last view (kept so a terraform recomposes only its acres)
    std::vector<uint8_t> cap;
    MapView cap_view{};

    bool Parse(const std::vector<uint8_t>& b);
};

bool LMapLayers::Impl::Parse(const std::vector<uint8_t>& b) {
    if (b.size() < 0x14 || std::memcmp(b.data(), "FLYT", 4) != 0)
        return false;
    std::vector<std::string> textures;
    std::vector<PaneDef> panes;
    std::vector<int> parent;
    std::vector<int> stack{-1};
    int last = -1, n_capture = -1;
    size_t o = U16(b, 6);
    auto cstr = [&](size_t at, size_t max) {
        std::string s;
        for (size_t i = 0; i < max && Fits(b, at + i, 1) && b[at + i]; ++i)
            s.push_back(static_cast<char>(b[at + i]));
        return s;
    };
    while (Fits(b, o, 8)) {
        const std::string tag(reinterpret_cast<const char*>(b.data() + o), 4);
        const uint32_t size = U32(b, o + 4);
        if (size < 8 || !Fits(b, o, size))
            return false;
        const size_t s = o;
        if (tag == "txl1") {
            const uint16_t n = U16(b, s + 8);
            for (uint16_t i = 0; i < n; ++i)
                textures.push_back(cstr(s + 0xC + U32(b, s + 0xC + 4 * i), 128));
        } else if (tag == "mat1") {
            const uint16_t n = U16(b, s + 8);
            for (uint16_t i = 0; i < n; ++i) {
                size_t p = s + U32(b, s + 0xC + 4 * i);
                Mat m;
                m.name = cstr(p, 0x1C);
                const uint32_t fl = U32(b, p + 0x1C);
                for (int c = 0; c < 4; ++c) {
                    m.black[c] = U8(b, p + 0x24 + c) / 255.f;
                    m.white[c] = U8(b, p + 0x28 + c) / 255.f;
                }
                p += 0x2C;
                const uint32_t nt = fl & 3, nsrt = (fl >> 2) & 3, ntg = (fl >> 4) & 3,
                               ntev = (fl >> 6) & 7, nproj = (fl >> 15) & 3;
                for (uint32_t t = 0; t < nt; ++t, p += 4) {
                    TexMap tm;
                    const uint16_t ti = U16(b, p);
                    tm.tex = ti < textures.size() ? textures[ti] : std::string{};
                    tm.repeat =
                        (U8(b, p + 2) & 3) == 1; // wrap = low 2 bits (1 repeat), filter above
                    m.maps.push_back(std::move(tm));
                }
                for (uint32_t t = 0; t < nsrt; ++t, p += 20)
                    if (t < m.maps.size())
                        for (int k = 0; k < 5; ++k)
                            m.maps[t].srt[k] = F32(b, p + 4 * k);
                for (uint32_t t = 0; t < ntg; ++t, p += 16)
                    if (t < m.maps.size())
                        m.maps[t].projected =
                            U8(b, p + 1) == 4; // texgen source: pane-based projection
                m.stages = static_cast<uint8_t>(ntev);
                for (uint32_t t = 0; t < ntev; ++t, p += 4)
                    if (t == 0)
                        m.color_mode0 = U8(b, p), m.alpha_mode0 = U8(b, p + 1);
                if (fl >> 9 & 1) {
                    m.has_cmp = true;
                    m.cmp_func = U8(b, p);
                    m.cmp_ref = F32(b, p + 4);
                    p += 8;
                }
                if (fl >> 10 & 1) {
                    m.has_blend = true;
                    m.op = U8(b, p), m.src = U8(b, p + 1), m.dst = U8(b, p + 2);
                    p += 4;
                }
                if (fl >> 12 & 1) {
                    m.has_blend_a = true;
                    m.op_a = U8(b, p), m.src_a = U8(b, p + 1), m.dst_a = U8(b, p + 2);
                    p += 4;
                }
                if (fl >> 14 & 1) {
                    m.has_ind = true;
                    for (int k = 0; k < 3; ++k)
                        m.ind[k] = F32(b, p + 4 * k);
                    p += 12;
                }
                (void)nproj; // projection params (20 bytes each): translate 0, scale 1, flags 4 in
                             // LMap
                m.alpha_threshold = (fl >> 18) & 1;
                mats.push_back(std::move(m));
            }
        } else if (tag == "pan1" || tag == "pic1" || tag == "txt1" || tag == "wnd1" ||
                   tag == "prt1" || tag == "bnd1") {
            PaneDef p;
            p.name = cstr(s + 0xC, 0x18);
            p.w = F32(b, s + 0x4C);
            p.h = F32(b, s + 0x50);
            if (tag == "pic1")
                p.mat = U16(b, s + 0x64);
            last = static_cast<int>(panes.size());
            panes.push_back(p);
            parent.push_back(stack.back());
            if (p.name == "N_Capture_00")
                n_capture = last;
        } else if (tag == "usd1" && last >= 0 && last == n_capture) {
            // N_Capture_00 user data: CaptureBGColor (int x3), CaptureBGAlpha (int)
            const uint16_t n = U16(b, s + 8);
            for (uint16_t i = 0; i < n; ++i) {
                const size_t e = s + 0xC + 0xC * i;
                const std::string name = cstr(e + U32(b, e), 64);
                const size_t data = e + U32(b, e + 4);
                const uint16_t cnt = U16(b, e + 8);
                if (U8(b, e + 10) != 1) // int
                    continue;
                if (name == "CaptureBGColor" && cnt == 3)
                    for (int c = 0; c < 3; ++c)
                        clear[c] = static_cast<float>(U32(b, data + 4 * c)) / 255.f;
                else if (name == "CaptureBGAlpha" && cnt == 1)
                    clear[3] = static_cast<float>(U32(b, data)) / 255.f;
            }
        } else if (tag == "pas1") {
            stack.push_back(last);
        } else if (tag == "pae1") {
            if (stack.size() > 1)
                stack.pop_back();
        }
        o += size;
    }
    if (n_capture < 0)
        return false;
    capture_w = panes[n_capture].w;
    capture_h = panes[n_capture].h;
    for (size_t i = 0; i < panes.size(); ++i) {
        if (parent[i] == n_capture && panes[i].mat >= 0)
            capture_children.push_back(panes[i]);
        if (panes[i].name == "P_Capture_00")
            capture_pane = panes[i];
    }
    return !capture_children.empty() && capture_pane.mat >= 0;
}

LMapLayers::LMapLayers() : impl(std::make_unique<Impl>()) {}
LMapLayers::~LMapLayers() = default;

bool LMapLayers::Loaded() const {
    return impl->ok;
}

bool LMapLayers::Load(Romfs& rf) {
    std::lock_guard lk{impl->mu};
    if (impl->tried)
        return impl->ok;
    const auto arc = rf.Layout("LMap");
    if (!arc)
        return false; // romfs not readable (yet): asked again next time
    impl->tried = true;
    const auto* flyt = arc->FindSuffix(".bflyt");
    if (!flyt || !impl->Parse(*flyt))
        return false;
    impl->layout = LoadLayout(rf, "LMap");
    if (!impl->layout)
        return false;
    for (auto& m : impl->mats)
        for (auto& t : m.maps) {
            // the RT / capture placeholders (Capture^t, Black_00^r) are replaced at runtime
            if (t.tex == "Capture^t" || t.tex == "Black_00^r")
                continue;
            t.img = impl->layout->Texture(t.tex);
            if (!t.img || t.img->w <= 0 || t.img->h <= 0)
                return false;
            const double r = t.srt[2] * 3.14159265358979323846 / 180.0;
            t.cs = static_cast<float>(std::cos(r));
            t.sn = static_cast<float>(std::sin(r));
            // "^o" = the BC1_SRGB pattern (BNTX format 0x1a06), sampled linear by the GPU
            t.srgb = t.tex.find("^o") != std::string::npos;
            t.alpha_one = true;
            for (size_t i = 3; i < t.img->rgba.size() && t.alpha_one; i += 4)
                t.alpha_one = t.img->rgba[i] == 255;
        }
    for (auto& m : impl->mats)
        m.capture_first = !m.maps.empty() && m.maps[0].tex.starts_with("Capture");
    impl->ok = true;
    return true;
}

namespace {
// capture space: N_Capture_00 frames the UI-map RT 1:1 (640 x 640 RT px at 0.5 px per world unit
// from world (80, 0); FieldUnitMapDrawer capture pos (720, 640) = its centre)
constexpr float kRtX0 = 80.f, kRtZ0 = 0.f, kRtScale = 0.5f;
} // namespace

struct LMapLayers::Composer {
    const Impl& I;
    const MapView& v;
    const MapRtSource& rt;
    std::vector<uint8_t>& cap;
    float cw, ch, view_per_cap;
    uint8_t clear8[4];

    // bilinear RT sample at view px coords (texel centres at +0.5)
    std::array<float, 4> RtLin(float vx, float vy) const {
        const float x = vx - 0.5f, y = vy - 0.5f;
        const float fx = std::floor(x), fy = std::floor(y);
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const float tx = x - fx, ty = y - fy;
        const MapRtPx q[4] = {rt.At(x0, y0), rt.At(x0 + 1, y0), rt.At(x0, y0 + 1),
                              rt.At(x0 + 1, y0 + 1)};
        auto ch_ = [&](const MapRtPx& p, int c) {
            return c == 0   ? p.r / 65535.f
                   : c == 1 ? p.g / 65535.f
                   : c == 2 ? p.b / 65535.f
                            : (p.a < 0 ? 0.f : p.a);
        };
        std::array<float, 4> o{};
        for (int c = 0; c < 4; ++c) {
            const float a = ch_(q[0], c), b = ch_(q[1], c), cc = ch_(q[2], c), d = ch_(q[3], c);
            const float top = a + (b - a) * tx, bot = cc + (d - cc) * tx;
            o[c] = top + (bot - top) * ty;
        }
        return o;
    }

    void CapPixel(int px, int py) const {
        uint8_t* outp = cap.data() + (size_t(py) * v.w + px) * 4;
        {
            // nothing drawn within the warp reach (< 1 px): every pane leaves the clear colour
            bool empty = true;
            for (int dy = -1; dy <= 1 && empty; ++dy)
                for (int dx = -1; dx <= 1 && empty; ++dx)
                    empty = !(rt.At(px + dx, py + dy).a > 0.f);
            if (empty) {
                std::memcpy(outp, clear8, 4);
                return;
            }
        }
        const float wx = v.x0 + (px + 0.5f) / v.px_per_unit,
                    wz = v.z0 + (py + 0.5f) / v.px_per_unit;
        const float cx = (wx - kRtX0) * kRtScale, cy = (wz - kRtZ0) * kRtScale; // capture px
        const float lx = cx - cw / 2, ly = ch / 2 - cy;                         // pane-local, y up
        std::array<float, 4> dst = I.clear;
        const MapRtPx self = rt.At(px, py);
        const std::array<float, 4> A0{self.r / 65535.f, self.g / 65535.f, self.b / 65535.f,
                                      self.a < 0 ? 0.f : self.a};
        const TexMap* memo_map = nullptr; // panes sharing one noise map + SRT sample it once
        std::array<float, 4> memo{};
        for (const PaneDef& pd : I.capture_children) {
            const Mat& m = I.mats[pd.mat];
            if (!m.capture_first)
                continue;
            std::array<float, 4> src{};
            if (m.color_mode0 == 11) {
                // layout archive shader (EachIndirect): A = capture at uv0 + indirect offset,
                // N = texmap1, C = texmap2 (absent -> 0); rgb = mix(A, C, C.a);
                // alpha = min(N.a, A.a) combined with C.a by min (2 TEV stages) / max (1)
                std::array<float, 4> N{1, 1, 1, 1}, C{0, 0, 0, 0}, A = A0;
                const bool warp = m.has_ind && (m.ind[1] != 0 || m.ind[2] != 0);
                if (m.maps.size() > 1 && m.maps[1].img && (warp || !m.maps[1].alpha_one)) {
                    const TexMap& t = m.maps[1];
                    if (memo_map && memo_map->img == t.img && t.projected == memo_map->projected &&
                        std::equal(t.srt, t.srt + 5, memo_map->srt)) {
                        N = memo;
                    } else {
                        float u, vv;
                        TexCoord(t, lx, ly, pd.w, pd.h, u, vv);
                        N = Sample(*t.img, u, vv, t.repeat, false);
                        memo_map = &t;
                        memo = N;
                    }
                }
                if (warp) {
                    // c3[0x20..0x3c] uploaded as (2 sx, 0, 0, -sx), (0, 2 sy, 0, -sy) (rot 0)
                    const float du = 2 * m.ind[1] * N[0] - m.ind[1];
                    const float dv = 2 * m.ind[2] * N[1] - m.ind[2];
                    A = RtLin(px + 0.5f + du * cw * view_per_cap,
                              py + 0.5f + dv * ch * view_per_cap);
                }
                const float na = std::min(N[3], A[3]);
                if (m.stages >= 2 && m.has_cmp && !m.alpha_threshold && m.black[3] == 0.f &&
                    m.white[3] == 1.f &&
                    (m.cmp_func == Equal || m.cmp_func == GreaterEqual || m.cmp_func == Greater) &&
                    na < m.cmp_ref)
                    continue; // src.a = min(na, C.a) <= na < ref: the test fails whatever C is
                if (m.maps.size() > 2 && m.maps[2].img) {
                    float u, vv;
                    TexCoord(m.maps[2], lx, ly, pd.w, pd.h, u, vv);
                    C = Sample(*m.maps[2].img, u, vv, m.maps[2].repeat, m.maps[2].srgb);
                }
                for (int c = 0; c < 3; ++c)
                    src[c] = A[c] + (C[c] - A[c]) * C[3];
                src[3] = m.stages >= 2 ? std::min(na, C[3]) : std::max(na, C[3]);
            } else if (m.color_mode0 == 0 && m.alpha_mode0 == 1 && m.maps.size() == 2 &&
                       m.maps[1].img) {
                // standard combiner, stage 0: colour Replace (texmap1), alpha Modulate
                float u, vv;
                TexCoord(m.maps[1], lx, ly, pd.w, pd.h, u, vv);
                const auto T = Sample(*m.maps[1].img, u, vv, m.maps[1].repeat, false);
                src = {T[0], T[1], T[2], A0[3] * T[3]};
            } else {
                continue;
            }
            // material colours: rgb = black + (white - black) * rgb; alpha interpolated, or
            // thresholded (sat((a - black.a) / (white.a - black.a)))
            for (int c = 0; c < 3; ++c)
                src[c] = m.black[c] + (m.white[c] - m.black[c]) * src[c];
            if (m.alpha_threshold) {
                const float d = m.white[3] - m.black[3];
                src[3] = d != 0 ? std::clamp((src[3] - m.black[3]) / d, 0.f, 1.f)
                                : (src[3] >= m.black[3] ? 1.f : 0.f);
            } else {
                src[3] = std::clamp(m.black[3] + (m.white[3] - m.black[3]) * src[3], 0.f, 1.f);
            }
            if (m.has_cmp && !CmpPass(m.cmp_func, src[3], m.cmp_ref))
                continue;
            std::array<float, 4> nd{};
            for (int c = 0; c < 3; ++c)
                nd[c] = m.has_blend ? BlendCh(m.op, m.src, m.dst, src, dst, c) : src[c];
            nd[3] = m.has_blend_a
                        ? BlendCh(m.op_a, m.src_a, m.dst_a, src, dst, 3)
                        : (m.has_blend ? BlendCh(m.op, m.src, m.dst, src, dst, 3) : src[3]);
            for (int c = 0; c < 4; ++c)
                dst[c] = std::clamp(nd[c], 0.f, 1.f);
        }
        for (int c = 0; c < 3; ++c)
            outp[c] = Srgb8(dst[c]);
        outp[3] = static_cast<uint8_t>(std::lround(dst[3] * 255.f));
    }

    // P_Capture_00: the capture texture through its own indirect warp (TEV EachIndirect, noise =
    // texmap1 projected over the 1024-unit pane); straight alpha out.
    void FinalPixel(int px, int py, uint8_t* o) const {
        const Mat* pm = I.capture_pane.mat >= 0 ? &I.mats[I.capture_pane.mat] : nullptr;
        const bool warp = pm && pm->color_mode0 == 11 && pm->has_ind && pm->maps.size() > 1 &&
                          pm->maps[1].img && (pm->ind[1] != 0 || pm->ind[2] != 0);
        const uint8_t* self = cap.data() + (size_t(py) * v.w + px) * 4;
        if (!warp) {
            std::memcpy(o, self, 4);
            return;
        }
        {
            bool clear = true; // all taps within reach transparent: stays the clear colour
            for (int dy = -1; dy <= 1 && clear; ++dy)
                for (int dx = -1; dx <= 1 && clear; ++dx) {
                    const int xx = std::clamp(px + dx, 0, v.w - 1),
                              yy = std::clamp(py + dy, 0, v.h - 1);
                    clear = cap[(size_t(yy) * v.w + xx) * 4 + 3] == 0;
                }
            if (clear) {
                std::memcpy(o, self, 4);
                return;
            }
        }
        const float k = I.capture_pane.w / cw; // P_Capture_00 units per capture px
        const float wx = v.x0 + (px + 0.5f) / v.px_per_unit,
                    wz = v.z0 + (py + 0.5f) / v.px_per_unit;
        const float cx = (wx - kRtX0) * kRtScale, cy = (wz - kRtZ0) * kRtScale;
        float u, vv;
        TexCoord(pm->maps[1], (cx - cw / 2) * k, (ch / 2 - cy) * k, I.capture_pane.w,
                 I.capture_pane.h, u, vv);
        const auto N = Sample(*pm->maps[1].img, u, vv, pm->maps[1].repeat, false);
        const float du = (2 * pm->ind[1] * N[0] - pm->ind[1]) * cw * view_per_cap;
        const float dv = (2 * pm->ind[2] * N[1] - pm->ind[2]) * ch * view_per_cap;
        const float x = px + du, y = py + dv;
        const float fx = std::floor(x), fy = std::floor(y);
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const float tx = x - fx, ty = y - fy;
        const auto& L = SrgbLut().lin;
        const int xa = std::clamp(x0, 0, v.w - 1), xb = std::clamp(x0 + 1, 0, v.w - 1);
        const int ya = std::clamp(y0, 0, v.h - 1), yb = std::clamp(y0 + 1, 0, v.h - 1);
        const uint8_t* q[4] = {
            cap.data() + (size_t(ya) * v.w + xa) * 4, cap.data() + (size_t(ya) * v.w + xb) * 4,
            cap.data() + (size_t(yb) * v.w + xa) * 4, cap.data() + (size_t(yb) * v.w + xb) * 4};
        for (int c = 0; c < 4; ++c) {
            float a, b, cc, d;
            if (c < 3)
                a = L[q[0][c]], b = L[q[1][c]], cc = L[q[2][c]], d = L[q[3][c]];
            else
                a = q[0][c] / 255.f, b = q[1][c] / 255.f, cc = q[2][c] / 255.f, d = q[3][c] / 255.f;
            const float top = a + (b - a) * tx, bot = cc + (d - cc) * tx;
            const float val = top + (bot - top) * ty;
            o[c] = c < 3 ? Srgb8(val)
                         : static_cast<uint8_t>(std::lround(std::clamp(val, 0.f, 1.f) * 255.f));
        }
    }
};

void LMapLayers::SwapCapture(std::vector<uint8_t>& cap, MapView& view) {
    std::lock_guard lk{impl->mu};
    impl->cap.swap(cap);
    std::swap(impl->cap_view, view);
}

void LMapLayers::Compose(const MapView& v, const MapRtSource& rt, Image& out,
                         const std::vector<MapRect>* dirty) const {
    std::lock_guard lk{impl->mu};
    const Impl& I = *impl;
    const bool full = !dirty || !(impl->cap_view == v) ||
                      impl->cap.size() != size_t(v.w) * v.h * 4 || out.w != v.w || out.h != v.h ||
                      out.rgba.size() != size_t(v.w) * v.h * 4;
    if (full) {
        impl->cap.assign(size_t(v.w) * v.h * 4, 0);
        impl->cap_view = v;
        out.w = v.w;
        out.h = v.h;
        out.rgba.assign(size_t(v.w) * v.h * 4, 0);
    }
    Composer C{I, v, rt, impl->cap, I.capture_w, I.capture_h, v.px_per_unit / kRtScale, {}};
    for (int c = 0; c < 3; ++c)
        C.clear8[c] = Srgb8(I.clear[c]);
    C.clear8[3] = static_cast<uint8_t>(std::lround(I.clear[3] * 255.f));
    std::vector<MapRect> rects;
    if (full)
        rects.push_back({0, 0, v.w, v.h});
    else
        rects = *dirty;
    // the capture depends on RT texels within 1 px, the final warp on capture texels within 1 px
    for (const MapRect& r : rects)
        for (int y = std::max(r.y0 - 1, 0); y < std::min(r.y1 + 1, v.h); ++y)
            for (int x = std::max(r.x0 - 1, 0); x < std::min(r.x1 + 1, v.w); ++x)
                C.CapPixel(x, y);
    for (const MapRect& r : rects)
        for (int y = std::max(r.y0 - 2, 0); y < std::min(r.y1 + 2, v.h); ++y)
            for (int x = std::max(r.x0 - 2, 0); x < std::min(r.x1 + 2, v.w); ++x)
                C.FinalPixel(x, y, out.rgba.data() + (size_t(y) * v.w + x) * 4);
}

} // namespace acnh
