// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD art decoders and compositors. See sshd_art.h. The compositions follow the
// owner-accepted "alt1b" Ready mockup (the Ready design mockups), whose
// Python builders define the same pieces from the same romfs members.

#include "sshd_art.h"
#include "sshd_dungeon.h"
#include "sshd_dpad.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sshd {
namespace {

uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}
float BeF32(const uint8_t* p) {
    const uint32_t v = Be32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float SmoothStep(float e0, float e1, float x) {
    const float t = Clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

void Set(float* px, float r, float g, float b, float a) {
    px[0] = r;
    px[1] = g;
    px[2] = b;
    px[3] = a;
}
void Set8(float* px, int r, int g, int b, int a) {
    Set(px, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

/// One straight-alpha "over" at a pixel (PIL alpha_composite).
void Blend(float* d, const float* s, float extra_alpha = 1.0f) {
    const float sa = s[3] * extra_alpha;
    if (sa <= 0.0f) {
        return;
    }
    const float da = d[3];
    const float oa = sa + da * (1.0f - sa);
    if (oa <= 0.0f) {
        Set(d, 0, 0, 0, 0);
        return;
    }
    for (int c = 0; c < 3; ++c) {
        d[c] = (s[c] * sa + d[c] * da * (1.0f - sa)) / oa;
    }
    d[3] = oa;
}

/// Separable resample with a tent filter widened on downscale (PIL BILINEAR with antialias),
/// done in premultiplied alpha so transparent texels do not bleed colour.
Img ResampleAxis(const Img& s, int nw, bool horizontal) {
    const int sw = horizontal ? s.w : s.h;
    const int other = horizontal ? s.h : s.w;
    Img o = horizontal ? Img(nw, s.h) : Img(s.w, nw);
    const float scale = static_cast<float>(sw) / static_cast<float>(nw);
    const float support = std::max(1.0f, scale);
    std::vector<float> wts;
    for (int i = 0; i < nw; ++i) {
        const float center = (static_cast<float>(i) + 0.5f) * scale;
        const int lo = std::max(0, static_cast<int>(std::floor(center - support)));
        const int hi = std::min(sw - 1, static_cast<int>(std::ceil(center + support)));
        wts.assign(static_cast<size_t>(hi - lo + 1), 0.0f);
        float total = 0.0f;
        for (int k = lo; k <= hi; ++k) {
            const float d = std::fabs((static_cast<float>(k) + 0.5f - center) / support);
            const float w = d < 1.0f ? 1.0f - d : 0.0f;
            wts[static_cast<size_t>(k - lo)] = w;
            total += w;
        }
        if (total <= 0.0f) {
            continue;
        }
        for (int j = 0; j < other; ++j) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = lo; k <= hi; ++k) {
                const float w = wts[static_cast<size_t>(k - lo)];
                if (w == 0.0f) {
                    continue;
                }
                const float* p = horizontal ? s.at(k, j) : s.at(j, k);
                acc[0] += p[0] * p[3] * w;
                acc[1] += p[1] * p[3] * w;
                acc[2] += p[2] * p[3] * w;
                acc[3] += p[3] * w;
            }
            float* q = horizontal ? o.at(i, j) : o.at(j, i);
            const float a = acc[3] / total;
            if (acc[3] > 0.0f) {
                Set(q, acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], a);
            }
        }
    }
    return o;
}

/// Small separable [1 2 1] blur of a single-channel mask, `passes` times.
void BlurMask(std::vector<float>& m, int w, int h, int passes) {
    std::vector<float> t(m.size());
    for (int p = 0; p < passes; ++p) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float l = m[static_cast<size_t>(y) * w + std::max(0, x - 1)];
                const float r = m[static_cast<size_t>(y) * w + std::min(w - 1, x + 1)];
                t[static_cast<size_t>(y) * w + x] = (l + 2 * m[static_cast<size_t>(y) * w + x] + r) / 4;
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float u = t[static_cast<size_t>(std::max(0, y - 1)) * w + x];
                const float d = t[static_cast<size_t>(std::min(h - 1, y + 1)) * w + x];
                m[static_cast<size_t>(y) * w + x] = (u + 2 * t[static_cast<size_t>(y) * w + x] + d) / 4;
            }
        }
    }
}

/// Separable box blur (radius r, clamped edges) of a single-channel mask, O(w * h).
void BoxBlur(std::vector<float>& m, int w, int h, int r) {
    std::vector<float> t(m.size());
    const float k = 1.0f / static_cast<float>(2 * r + 1);
    for (int y = 0; y < h; ++y) {
        const float* row = &m[static_cast<size_t>(y) * w];
        float acc = 0.0f;
        for (int x = -r; x <= r; ++x)
            acc += row[std::clamp(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            t[static_cast<size_t>(y) * w + x] = acc * k;
            acc += row[std::min(w - 1, x + r + 1)] - row[std::max(0, x - r)];
        }
    }
    for (int x = 0; x < w; ++x) {
        float acc = 0.0f;
        for (int y = -r; y <= r; ++y)
            acc += t[static_cast<size_t>(std::clamp(y, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) {
            m[static_cast<size_t>(y) * w + x] = acc * k;
            acc += t[static_cast<size_t>(std::min(h - 1, y + r + 1)) * w + x] - t[static_cast<size_t>(std::max(0, y - r)) * w + x];
        }
    }
}

/// dev.36: near-round (octagonal) min or max filter of radius r: r passes alternating a
/// 4-neighbour cross and a 3 x 3 square, so the native map's edge lines keep one width on
/// diagonals and curves (a square filter widens them by up to 1.4 there). O(w * h * r).
void Morph(std::vector<float>& m, int w, int h, int r, bool take_max) {
    std::vector<float> t(m.size()), u(m.size());
    const auto pick = [take_max](float a, float b) { return take_max ? std::max(a, b) : std::min(a, b); };
    for (int p = 0; p < r; ++p) {
        for (int y = 0; y < h; ++y) {
            const size_t row = static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x)
                t[row + x] = pick(m[row + x], pick(m[row + std::max(0, x - 1)], m[row + std::min(w - 1, x + 1)]));
        }
        // even passes: the cross (vertical neighbours of the unfiltered row); odd: the square
        const std::vector<float>& src = p % 2 == 0 ? m : t;
        for (int y = 0; y < h; ++y) {
            const size_t up = static_cast<size_t>(std::max(0, y - 1)) * w, dn = static_cast<size_t>(std::min(h - 1, y + 1)) * w;
            const size_t row = static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x)
                u[row + x] = pick(t[row + x], pick(src[up + x], src[dn + x]));
        }
        m.swap(u);
    }
}

/// Deterministic value noise in [-0.5, 0.5], cell = 8 px (PIL effect_noise at 1/8, bicubic up).
float HashNoise(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFF) / 65535.0f - 0.5f;
}
float SmoothNoise(float x, float y) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    const float a = HashNoise(x0, y0), b = HashNoise(x0 + 1, y0);
    const float c = HashNoise(x0, y0 + 1), d = HashNoise(x0 + 1, y0 + 1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

constexpr int kCanvasW = 1240, kCanvasH = 1080;
constexpr float kMX = 620.0f, kMY = 430.0f, kMR = 350.0f; // minimap centre and radius
constexpr int kFrameX = 220, kFrameY = 30, kFrameS = 800;  // minimap frame picture box

/// The native pause backdrop tone (#2F3225) with a soft radial light (mockup dark_bg()).
void BackdropPixel(int cx, int cy, float* out) {
    const float rx = (static_cast<float>(cx) - kCanvasW / 2.0f) / kCanvasW;
    const float ry = (static_cast<float>(cy) - kCanvasH / 2.0f) / kCanvasH;
    const float r = std::sqrt(rx * rx + ry * ry);
    const float k = 1.18f - 0.55f * r;
    const float n = SmoothNoise(cx / 8.0f, cy / 8.0f) * (18.0f / 64.0f) * 14.0f;
    const float base[3] = {0x2F, 0x32, 0x25};
    for (int c = 0; c < 3; ++c) {
        out[c] = Clamp01((base[c] * k + n) / 255.0f);
    }
    out[3] = 1.0f;
}

void FillEllipse(Img& im, float cx, float cy, float rx, float ry, Col c) {
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            const float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            const float d = (std::sqrt(dx * dx + dy * dy) - 1.0f) * std::min(rx, ry);
            const float a = Clamp01(0.5f - d) * c.a;
            if (a > 0.0f) {
                const float s[4] = {c.r, c.g, c.b, a};
                Blend(im.at(x, y), s);
            }
        }
    }
}

} // namespace

std::vector<uint8_t> Img::Rgba8() const {
    std::vector<uint8_t> o(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        o[i] = static_cast<uint8_t>(std::lround(Clamp01(p[i]) * 255.0f));
    }
    return o;
}

Col Hex(std::string_view s) {
    if (!s.empty() && s[0] == '#') {
        s.remove_prefix(1);
    }
    auto byte = [&](size_t i) {
        return i + 2 <= s.size() ? std::stoi(std::string(s.substr(i, 2)), nullptr, 16) / 255.0f : 1.0f;
    };
    return Col{byte(0), byte(2), byte(4), s.size() >= 8 ? byte(6) : 1.0f};
}

// ---------------------------------------------------------------- containers

std::optional<std::vector<uint8_t>> Lz11(const std::vector<uint8_t>& d, size_t limit) {
    if (d.size() < 4 || (d[0] != 0x10 && d[0] != 0x11)) {
        return std::nullopt;
    }
    const bool lz11 = d[0] == 0x11;
    size_t size = d[1] | d[2] << 8 | d[3] << 16;
    size_t pos = 4;
    if (size == 0) {
        if (d.size() < 8) {
            return std::nullopt;
        }
        size = d[4] | d[5] << 8 | d[6] << 16 | static_cast<size_t>(d[7]) << 24;
        pos = 8;
    }
    if (size > (256u << 20)) {
        return std::nullopt;
    }
    // a prefix (limit): decode only the first `limit` bytes (dev.32: ObjectPack's Common.arc)
    if (limit < size)
        size = limit;
    std::vector<uint8_t> out;
    out.reserve(size);
    const size_t n = d.size();
    while (out.size() < size && pos < n) {
        const uint8_t flags = d[pos++];
        for (int bit = 0; bit < 8 && out.size() < size; ++bit) {
            if ((flags & (0x80 >> bit)) == 0) {
                if (pos >= n) {
                    return std::nullopt;
                }
                out.push_back(d[pos++]);
                continue;
            }
            if (pos + 1 >= n) {
                return std::nullopt;
            }
            const uint8_t b0 = d[pos];
            size_t length, disp;
            if (!lz11) {
                length = (b0 >> 4) + 3;
                disp = ((b0 & 0xF) << 8 | d[pos + 1]) + 1;
                pos += 2;
            } else {
                const int hi = b0 >> 4;
                if (hi == 0) {
                    if (pos + 2 >= n) {
                        return std::nullopt;
                    }
                    length = ((b0 & 0xF) << 4 | (d[pos + 1] >> 4)) + 0x11;
                    disp = ((d[pos + 1] & 0xF) << 8 | d[pos + 2]) + 1;
                    pos += 3;
                } else if (hi == 1) {
                    if (pos + 3 >= n) {
                        return std::nullopt;
                    }
                    length = ((b0 & 0xF) << 12 | d[pos + 1] << 4 | (d[pos + 2] >> 4)) + 0x111;
                    disp = ((d[pos + 2] & 0xF) << 8 | d[pos + 3]) + 1;
                    pos += 4;
                } else {
                    length = static_cast<size_t>(hi) + 1;
                    disp = ((b0 & 0xF) << 8 | d[pos + 1]) + 1;
                    pos += 2;
                }
            }
            if (disp > out.size()) {
                return std::nullopt;
            }
            const size_t start = out.size() - disp;
            for (size_t i = 0; i < length && out.size() < size; ++i) {
                out.push_back(out[start + i]);
            }
        }
    }
    if (out.size() != size) {
        return std::nullopt;
    }
    return out;
}

std::vector<U8Entry> U8List(const std::vector<uint8_t>& d, bool need_data) {
    std::vector<U8Entry> out;
    if (d.size() < 0x20 || Be32(d.data()) != 0x55AA382D) {
        return out;
    }
    const uint32_t root = Be32(&d[4]);
    if (static_cast<size_t>(root) + 12 > d.size()) {
        return out;
    }
    const uint32_t n = Be32(&d[root + 8]);
    const size_t strt = root + static_cast<size_t>(n) * 12;
    if (n == 0 || strt > d.size()) {
        return out;
    }
    auto name = [&](uint32_t off) {
        std::string s;
        for (size_t i = strt + off; i < d.size() && d[i] != 0; ++i) {
            s.push_back(static_cast<char>(d[i]));
        }
        return s;
    };
    std::vector<std::pair<std::string, uint32_t>> dirs; // (name, end index)
    for (uint32_t i = 1; i < n; ++i) {
        const uint8_t* e = &d[root + static_cast<size_t>(i) * 12];
        const uint8_t type = e[0];
        const uint32_t name_off = static_cast<uint32_t>(e[1]) << 16 | Be16(e + 2);
        const uint32_t a = Be32(e + 4), b = Be32(e + 8);
        while (!dirs.empty() && i >= dirs.back().second) {
            dirs.pop_back();
        }
        std::string path;
        for (const auto& [dn, end] : dirs) {
            if (!dn.empty()) {
                path += dn + "/";
            }
        }
        path += name(name_off);
        if (type == 1) {
            dirs.emplace_back(name(name_off), b);
        } else if (!need_data || static_cast<size_t>(a) + b <= d.size()) {
            out.push_back({path, a, b});
        }
    }
    return out;
}

std::optional<std::vector<uint8_t>> U8Get(const std::vector<uint8_t>& d, std::string_view path) {
    for (const auto& e : U8List(d)) {
        if (e.path == path) {
            return std::vector<uint8_t>(d.begin() + e.off, d.begin() + e.off + e.size);
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- GX textures

namespace {
void C565(uint16_t v, float* o) {
    const int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    Set8(o, r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255);
}
void C5A3(uint16_t v, float* o) {
    if (v & 0x8000) {
        const int r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
        Set8(o, r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255);
    } else {
        const int a3 = v >> 12 & 7;
        Set8(o, (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, a3 * 0x24 + (a3 >> 1));
    }
}
struct Block {
    int bw, bh, bpp;
};
std::optional<Block> BlockOf(int fmt) {
    switch (fmt) {
    case 0: case 8: case 14: return Block{8, 8, 4};
    case 1: case 2: case 9: return Block{8, 4, 8};
    case 3: case 4: case 5: return Block{4, 4, 16};
    case 6: return Block{4, 4, 32};
    default: return std::nullopt;
    }
}
} // namespace

static std::optional<Img> GxDecodePal(const uint8_t* d, size_t avail, int w, int h, int fmt,
                               const std::vector<std::array<float, 4>>* pal) {
    const auto blk = BlockOf(fmt);
    if (!blk || w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        return std::nullopt;
    }
    const int W = (w + blk->bw - 1) / blk->bw * blk->bw, H = (h + blk->bh - 1) / blk->bh * blk->bh;
    const size_t need = static_cast<size_t>(W) * H * blk->bpp / 8;
    if (need > avail) {
        return std::nullopt;
    }
    Img im(w, h);
    size_t p = 0;
    float px[4];
    auto put = [&](int x, int y, const float* c) {
        if (x < w && y < h) {
            std::memcpy(im.at(x, y), c, sizeof(float) * 4);
        }
    };
    for (int by = 0; by < H; by += blk->bh) {
        for (int bx = 0; bx < W; bx += blk->bw) {
            if (fmt == 14) {
                for (int sy = 0; sy < 8; sy += 4) {
                    for (int sx = 0; sx < 8; sx += 4) {
                        const uint16_t c0 = Be16(d + p), c1 = Be16(d + p + 2);
                        const uint32_t bits = Be32(d + p + 4);
                        p += 8;
                        float cs[4][4];
                        C565(c0, cs[0]);
                        C565(c1, cs[1]);
                        for (int c = 0; c < 3; ++c) {
                            if (c0 > c1) {
                                cs[2][c] = (2 * cs[0][c] + cs[1][c]) / 3;
                                cs[3][c] = (cs[0][c] + 2 * cs[1][c]) / 3;
                            } else {
                                cs[2][c] = (cs[0][c] + cs[1][c]) / 2;
                                cs[3][c] = 0;
                            }
                        }
                        cs[2][3] = 1;
                        cs[3][3] = c0 > c1 ? 1.0f : 0.0f;
                        for (int k = 0; k < 16; ++k) {
                            put(bx + sx + k % 4, by + sy + k / 4, cs[bits >> (30 - 2 * k) & 3]);
                        }
                    }
                }
                continue;
            }
            if (fmt == 6) {
                for (int k = 0; k < 16; ++k) {
                    const uint8_t a = d[p + 2 * k], r = d[p + 2 * k + 1];
                    const uint8_t g = d[p + 32 + 2 * k], b = d[p + 32 + 2 * k + 1];
                    Set8(px, r, g, b, a);
                    put(bx + k % 4, by + k / 4, px);
                }
                p += 64;
                continue;
            }
            for (int yy = 0; yy < blk->bh; ++yy) {
                for (int xx = 0; xx < blk->bw; ++xx) {
                    uint32_t v;
                    if (blk->bpp == 4) {
                        const size_t idx = static_cast<size_t>(yy) * blk->bw + xx;
                        const uint8_t byte = d[p + idx / 2];
                        v = (idx & 1) ? (byte & 15) : (byte >> 4);
                    } else if (blk->bpp == 8) {
                        v = d[p + static_cast<size_t>(yy) * blk->bw + xx];
                    } else {
                        v = Be16(d + p + (static_cast<size_t>(yy) * blk->bw + xx) * 2);
                    }
                    switch (fmt) {
                    case 0: Set8(px, v * 17, v * 17, v * 17, v * 17); break;
                    case 1: Set8(px, v, v, v, v); break;
                    case 2: Set8(px, (v & 15) * 17, (v & 15) * 17, (v & 15) * 17, (v >> 4) * 17); break;
                    case 3: Set8(px, v & 255, v & 255, v & 255, v >> 8); break;
                    case 4: C565(static_cast<uint16_t>(v), px); break;
                    case 5: C5A3(static_cast<uint16_t>(v), px); break;
                    default:
                        if (pal == nullptr || v >= pal->size()) {
                            Set(px, 0, 0, 0, 0);
                        } else {
                            std::memcpy(px, (*pal)[v].data(), sizeof(px));
                        }
                    }
                    put(bx + xx, by + yy, px);
                }
            }
            p += static_cast<size_t>(blk->bw) * blk->bh * blk->bpp / 8;
        }
    }
    return im;
}

std::optional<Img> GxDecode(const uint8_t* data, size_t avail, int w, int h, int fmt) {
    return GxDecodePal(data, avail, w, h, fmt, nullptr);
}

std::optional<Img> TplDecode(const std::vector<uint8_t>& d) {
    if (d.size() < 12 || Be32(d.data()) != 0x0020AF30) {
        return std::nullopt;
    }
    const uint32_t n = Be32(&d[4]), table = Be32(&d[8]);
    if (n == 0 || table > d.size() || d.size() - table < 8) {
        return std::nullopt;
    }
    const uint32_t io = Be32(&d[table]), po = Be32(&d[table + 4]);
    if (static_cast<size_t>(io) + 12 > d.size()) {
        return std::nullopt;
    }
    const int h = Be16(&d[io]), w = Be16(&d[io + 2]);
    const int fmt = static_cast<int>(Be32(&d[io + 4]));
    const uint32_t data = Be32(&d[io + 8]);
    if (data >= d.size()) {
        return std::nullopt;
    }
    std::vector<std::array<float, 4>> pal;
    if (po != 0 && static_cast<size_t>(po) + 12 > d.size())
        return std::nullopt;
    if (po != 0) {
        const uint16_t pn = Be16(&d[po]);
        const uint32_t pf = Be32(&d[po + 4]), pd = Be32(&d[po + 8]);
        if (pd + static_cast<size_t>(pn) * 2 <= d.size()) {
            for (uint16_t i = 0; i < pn; ++i) {
                const uint16_t v = Be16(&d[pd + i * 2]);
                std::array<float, 4> c{};
                if (pf == 0) {
                    Set8(c.data(), v & 255, v & 255, v & 255, v >> 8);
                } else if (pf == 1) {
                    C565(v, c.data());
                } else {
                    C5A3(v, c.data());
                }
                pal.push_back(c);
            }
        }
    }
    return GxDecodePal(&d[data], d.size() - data, w, h, fmt, pal.empty() ? nullptr : &pal);
}

// ---------------------------------------------------------------- image ops

Img Resize(const Img& s, int w, int h) {
    if (s.empty() || w <= 0 || h <= 0) {
        return Img(std::max(w, 1), std::max(h, 1));
    }
    if (w == s.w && h == s.h) {
        return s;
    }
    return ResampleAxis(ResampleAxis(s, w, true), h, false);
}
Img SizeW(const Img& s, int w) {
    return Resize(s, w, std::max(1, static_cast<int>(std::lround(static_cast<float>(s.h) * w / s.w))));
}
Img Fit(const Img& s, int size) {
    if (s.w >= s.h) {
        return SizeW(s, size);
    }
    return Resize(s, std::max(1, static_cast<int>(std::lround(static_cast<float>(size) * s.w / s.h))), size);
}
Img Tint(const Img& s, Col b, Col w, float a) {
    Img o = s;
    for (size_t i = 0; i < o.p.size(); i += 4) {
        const float I = o.p[i];
        o.p[i] = b.r + (w.r - b.r) * I;
        o.p[i + 1] = b.g + (w.g - b.g) * I;
        o.p[i + 2] = b.b + (w.b - b.b) * I;
        o.p[i + 3] *= a;
    }
    return o;
}
Img AMask(const Img& s, Col c, float a) {
    Img o = s;
    for (size_t i = 0; i < o.p.size(); i += 4) {
        const float al = o.p[i + 3] * o.p[i] * a;
        Set(&o.p[i], c.r, c.g, c.b, al);
    }
    return o;
}
Img Fade(const Img& s, float a) {
    Img o = s;
    for (size_t i = 3; i < o.p.size(); i += 4) {
        o.p[i] *= a;
    }
    return o;
}
Img FlipH(const Img& s) {
    Img o(s.w, s.h);
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x)
            std::memcpy(o.at(s.w - 1 - x, y), s.at(x, y), 16);
    return o;
}
Img FlipV(const Img& s) {
    Img o(s.w, s.h);
    for (int y = 0; y < s.h; ++y)
        std::memcpy(o.at(0, s.h - 1 - y), s.at(0, y), 16 * static_cast<size_t>(s.w));
    return o;
}
Img Rot90(const Img& s) {
    Img o(s.h, s.w);
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x)
            std::memcpy(o.at(y, s.w - 1 - x), s.at(x, y), 16);
    return o;
}
Img Crop(const Img& s, int x0, int y0, int w, int h) {
    Img o(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x0 + x, sy = y0 + y;
            if (sx >= 0 && sy >= 0 && sx < s.w && sy < s.h)
                std::memcpy(o.at(x, y), s.at(sx, sy), 16);
        }
    return o;
}
void Over(Img& dst, const Img& src, int x0, int y0) {
    for (int y = 0; y < src.h; ++y) {
        const int dy = y0 + y;
        if (dy < 0 || dy >= dst.h)
            continue;
        for (int x = 0; x < src.w; ++x) {
            const int dx = x0 + x;
            if (dx >= 0 && dx < dst.w)
                Blend(dst.at(dx, dy), src.at(x, y));
        }
    }
}
void OverCentered(Img& dst, const Img& src, float cx, float cy) {
    Over(dst, src, static_cast<int>(cx) - src.w / 2, static_cast<int>(cy) - src.h / 2);
}
Img Quad(const Img& s) {
    Img o(2 * s.w, 2 * s.h);
    Over(o, s, 0, 0);
    Over(o, FlipH(s), s.w, 0);
    Over(o, FlipV(s), 0, s.h);
    Over(o, FlipV(FlipH(s)), s.w, s.h);
    return o;
}

// ---------------------------------------------------------------- RFNT font

struct Font {
    int alt = 0, dl = 0, dg = 0, dc = 0;
    int cw = 0, ch = 0, maxw = 0, ssize = 0, scount = 0, sfmt = 0, srow = 0, sline = 0,
        sw = 0, sh = 0, simg = 0;
    std::vector<std::array<int, 3>> widths; // index = glyph
    std::vector<int> cmap = std::vector<int>(0x10000, -1);
    std::vector<Img> sheets;
};

namespace {
std::shared_ptr<Font> ParseFont(const std::vector<uint8_t>& d) {
    if (d.size() < 16 || std::memcmp(d.data(), "RFNT", 4) != 0) {
        return nullptr;
    }
    auto f = std::make_shared<Font>();
    size_t p = Be16(&d[12]);
    while (p + 8 <= d.size()) {
        const uint8_t* mg = &d[p];
        const uint32_t sz = Be32(&d[p + 4]);
        const size_t b = p + 8;
        if (std::memcmp(mg, "FINF", 4) == 0 && b + 23 <= d.size()) {
            f->alt = Be16(&d[b + 2]);
            f->dl = static_cast<int8_t>(d[b + 4]);
            f->dg = d[b + 5];
            f->dc = static_cast<int8_t>(d[b + 6]);
        } else if (std::memcmp(mg, "TGLP", 4) == 0 && b + 24 <= d.size()) {
            f->cw = d[b];
            f->ch = d[b + 1];
            f->maxw = d[b + 3];
            f->ssize = static_cast<int>(Be32(&d[b + 4]));
            f->scount = Be16(&d[b + 8]);
            f->sfmt = Be16(&d[b + 10]) & 0x7FFF;
            f->srow = Be16(&d[b + 12]);
            f->sline = Be16(&d[b + 14]);
            f->sw = Be16(&d[b + 16]);
            f->sh = Be16(&d[b + 18]);
            f->simg = static_cast<int>(Be32(&d[b + 20]));
        } else if (std::memcmp(mg, "CWDH", 4) == 0 && b + 8 <= d.size()) {
            const int a = Be16(&d[b]), e = Be16(&d[b + 2]);
            for (int i = a; i <= e; ++i) {
                const size_t q = b + 8 + static_cast<size_t>(i - a) * 3;
                if (q + 3 > d.size())
                    break;
                if (static_cast<int>(f->widths.size()) <= i)
                    f->widths.resize(static_cast<size_t>(i) + 1, {f->dl, f->dg, f->dc});
                f->widths[static_cast<size_t>(i)] = {static_cast<int8_t>(d[q]), d[q + 1],
                                                     static_cast<int8_t>(d[q + 2])};
            }
        } else if (std::memcmp(mg, "CMAP", 4) == 0 && b + 12 <= d.size()) {
            const int c0 = Be16(&d[b]), c1 = Be16(&d[b + 2]), m = Be16(&d[b + 4]);
            const size_t q = b + 12;
            if (m == 0 && q + 2 <= d.size()) {
                const int o = Be16(&d[q]);
                for (int c = c0; c <= c1; ++c)
                    f->cmap[static_cast<size_t>(c)] = o + c - c0;
            } else if (m == 1) {
                for (int c = c0; c <= c1; ++c) {
                    const size_t r = q + 2 * static_cast<size_t>(c - c0);
                    if (r + 2 > d.size())
                        break;
                    const int g = Be16(&d[r]);
                    if (g != 0xFFFF)
                        f->cmap[static_cast<size_t>(c)] = g;
                }
            } else if (q + 2 <= d.size()) {
                const int n = Be16(&d[q]);
                for (int k = 0; k < n; ++k) {
                    const size_t r = q + 2 + 4 * static_cast<size_t>(k);
                    if (r + 4 > d.size())
                        break;
                    f->cmap[Be16(&d[r])] = Be16(&d[r + 2]);
                }
            }
        }
        if (sz == 0)
            break;
        p += sz;
    }
    if (f->cw == 0 || f->srow == 0 || f->sline == 0 || f->scount == 0) {
        return nullptr;
    }
    for (int i = 0; i < f->scount; ++i) {
        const size_t off = static_cast<size_t>(f->simg) + static_cast<size_t>(i) * f->ssize;
        if (off >= d.size())
            return nullptr;
        auto sheet = GxDecode(&d[off], d.size() - off, f->sw, f->sh, f->sfmt);
        if (!sheet)
            return nullptr;
        f->sheets.push_back(std::move(*sheet));
    }
    return f;
}

/// Glyph intensity strip of `text` (white, alpha = I), like brfnt.Font.render.
Img RenderRun(const Font& f, std::string_view text) {
    struct G {
        int g, l, gw, cw;
    };
    std::vector<G> gl;
    int W = 0;
    for (unsigned char ch : text) {
        int g = f.cmap[ch];
        if (g < 0)
            g = f.alt;
        std::array<int, 3> wd{f.dl, f.dg, f.dc};
        if (g >= 0 && g < static_cast<int>(f.widths.size()))
            wd = f.widths[static_cast<size_t>(g)];
        gl.push_back({g, wd[0], wd[1], wd[2]});
        W += wd[2];
    }
    const int PAD = f.maxw;
    Img img(std::max(W, 1) + 2 * PAD, f.ch);
    int x = PAD;
    const int per = f.srow * f.sline;
    int minx = img.w, maxx = -1;
    for (const auto& g : gl) {
        const int si = g.g / per, k = g.g % per;
        if (si < static_cast<int>(f.sheets.size())) {
            const Img& sh = f.sheets[static_cast<size_t>(si)];
            const int gx = (k % f.srow) * (f.cw + 1), gy = (k / f.srow) * (f.ch + 1);
            for (int yy = 0; yy < f.ch; ++yy) {
                for (int xx = 0; xx < std::min(g.gw, f.cw); ++xx) {
                    const int sx = gx + xx, sy = gy + yy, dx = x + g.l + xx;
                    if (sx >= sh.w || sy >= sh.h || dx < 0 || dx >= img.w)
                        continue;
                    const float* s = sh.at(sx, sy);
                    float* d = img.at(dx, yy);
                    if (s[3] > 0.0f) {
                        d[0] = d[1] = d[2] = s[0];
                        d[3] = std::max(d[3], s[3]);
                        minx = std::min(minx, dx);
                        maxx = std::max(maxx, dx);
                    }
                }
            }
        }
        x += g.cw;
    }
    const int x0 = std::max(0, std::min(minx, PAD));
    const int x1 = std::max(maxx + 1, PAD + W);
    return Crop(img, x0, 0, x1 - x0, img.h);
}

struct TextStyle {
    const char* name;
    const char *mb, *mw, *top, *bot, *sb, *sw;
    float shadow_a = 1.0f; // the shadow pane's alpha
    bool special = false;  // special_00 (titles) instead of normal_00
};
constexpr TextStyle kStyles[] = {
    {"plank", "#FFFFFF", "#FFFFFF", "#EDE2D0", "#CAAF81", "#614821", "#553F1D"},
    {"title", "#FFFFBE", "#BDB085", "#FFFFFF", "#E0D6B4", "#000000", "#000000", 1.0f, true},
    // the HUD's own button captions (DoButton T_helpBtn_00 / T_dawsing_00 materials + shadow)
    {"hud", "#868686", "#FFFFFF", "#FFE988", "#BAA566", "#000000", "#000000"},
    // the map's area-name plate (Map2D map_00 T_title_00 over T_titleS_00 at alpha 64)
    {"plate", "#FFFFFF", "#FFFFFF", "#FFECC6", "#DBC584", "#000000", "#000000", 64.0f / 255.0f, true},
    // the neighbouring-area plate (MapField mapPlace_00 T_area_00 over T_areaS_00 at alpha 64)
    {"place", "#E7D789", "#81622D", "#FFFFFF", "#FFFFFF", "#000000", "#000000", 64.0f / 255.0f, true},
    // the Sky map's island names (native capture 07: brown letters (111, 76, 29) on a cream halo)
    {"isle", "#6F4C1D", "#6F4C1D", "#FFFFFF", "#FFFFFF", "#FFF6D2", "#FFF6D2", 0.9f, true},
    // the map's floor tabs (Map2D mapFloorBtn_00 T_text_00 white over T_textS_00 at alpha 64)
    {"floor", "#FFFFFF", "#FFFFFF", "#FFFFFF", "#FFFFFF", "#614821", "#553F1D", 64.0f / 255.0f, true},
    // dev.46: the choice plank's label (MenuPause N_options_00 T_options_00 over T_optionsS_00 at 64)
    {"choice", "#FFFFFF", "#FFFFFF", "#F6F2EA", "#E1C79B", "#614821", "#553F1D", 64.0f / 255.0f},
    // dev.46: the Options row label (MenuOptions options_00 T_name_01 white over T_nameS_01 at 50)
    {"optrow", "#FFFFFF", "#FFFFFF", "#FFFFFF", "#FFFFFF", "#000000", "#000000", 50.0f / 255.0f},
    // dev.46: the do-prompt word (DoButton basicInfo_00 T_infoText_00 over T_infoTextS_00 at 64)
    {"doinfo", "#FFFBE3", "#FFFBE3", "#FFFFFF", "#C8C8C8", "#000000", "#000000", 64.0f / 255.0f},
};
} // namespace

// ---------------------------------------------------------------- Art

// dev.33 (memory budget): the romfs files (whole layout archives, up to 17 MB each) and the
// decoded layout textures (float RGBA) are kept least recently used first within a byte budget,
// instead of for the whole session; a dropped one is read / decoded again when asked for (same bytes).
const std::vector<uint8_t>* Art::File(const std::string& path) {
    for (size_t i = 0; i < files_.size(); ++i) {
        if (files_[i].first == path) {
            std::rotate(files_.begin() + static_cast<std::ptrdiff_t>(i),
                        files_.begin() + static_cast<std::ptrdiff_t>(i) + 1, files_.end());
            return &files_.back().second;
        }
    }
    auto bytes = read_(path);
    if (!bytes) {
        error_ = "romfs read failed: " + path;
        return nullptr;
    }
    size_t total = bytes->size();
    for (const auto& [p, b] : files_)
        total += b.size();
    while (!files_.empty() && total > kFileCacheBytes) {
        total -= files_.front().second.size();
        files_.erase(files_.begin());
    }
    files_.emplace_back(path, std::move(*bytes));
    return &files_.back().second;
}

void Art::CacheTexture(const std::string& key, const Img& im) {
    size_t total = im.p.size() * sizeof(float);
    for (const auto& [k, t] : textures_)
        total += t.p.size() * sizeof(float);
    while (!textures_.empty() && total > kTextureCacheBytes) {
        total -= textures_.front().second.p.size() * sizeof(float);
        textures_.erase(textures_.begin());
    }
    textures_.emplace_back(key, im);
}

std::optional<Img> Art::Layout(std::string_view arc, std::string_view member) {
    const std::string key = std::string(arc) + "/" + std::string(member);
    for (size_t i = 0; i < textures_.size(); ++i)
        if (textures_[i].first == key) {
            std::rotate(textures_.begin() + static_cast<std::ptrdiff_t>(i),
                        textures_.begin() + static_cast<std::ptrdiff_t>(i) + 1, textures_.end());
            return textures_.back().second;
        }
    const auto* d = File("Layout/" + std::string(arc) + ".arc");
    if (d == nullptr)
        return std::nullopt;
    const std::vector<U8Entry>* entries = nullptr;
    for (const auto& [name, list] : layouts_)
        if (name == arc) entries = &list;
    if (entries == nullptr) {
        layouts_.emplace_back(arc, U8List(*d));
        entries = &layouts_.back().second;
    }
    const std::string path = "timg/" + std::string(member) + ".tpl";
    for (const auto& e : *entries) {
        if (e.path != path) continue;
        const std::vector<uint8_t> tpl(d->begin() + e.off, d->begin() + e.off + e.size);
        auto im = TplDecode(tpl);
        if (im) CacheTexture(key, *im);
        else error_ = "tpl decode failed: " + std::string(member);
        return im;
    }
    error_ = "no member " + std::string(member) + " in " + std::string(arc);
    return std::nullopt;
}

const Font* Art::GetFont(bool special) {
    for (const auto& [s, f] : fonts_)
        if (s == special)
            return f.get();
    const auto* d = File(special ? "US/Font/en_US/special_00.brfnt" : "US/Font/en_US/normal_00.brfnt");
    if (d == nullptr)
        return nullptr;
    auto f = ParseFont(*d);
    if (!f) {
        error_ = "font parse failed";
        return nullptr;
    }
    fonts_.emplace_back(special, f);
    return f.get();
}

std::optional<Img> Art::Text(std::string_view style, float cap, std::string_view text) {
    const TextStyle* st = nullptr;
    for (const auto& s : kStyles)
        if (style == s.name)
            st = &s;
    if (st == nullptr || cap <= 0)
        return std::nullopt;
    const bool special = st->special;
    const Font* f = GetFont(special);
    if (f == nullptr)
        return std::nullopt;
    const Img m = RenderRun(*f, text);
    auto layer = [&](Col blk, Col wht, const Col* top, const Col* bot) {
        Img o(m.w, m.h);
        for (int y = 0; y < m.h; ++y) {
            const float t = m.h > 1 ? static_cast<float>(y) / (m.h - 1) : 0.0f;
            for (int x = 0; x < m.w; ++x) {
                const float I = m.at(x, y)[3];
                float c[3] = {blk.r + (wht.r - blk.r) * I, blk.g + (wht.g - blk.g) * I,
                              blk.b + (wht.b - blk.b) * I};
                if (top != nullptr) {
                    c[0] *= top->r * (1 - t) + bot->r * t;
                    c[1] *= top->g * (1 - t) + bot->g * t;
                    c[2] *= top->b * (1 - t) + bot->b * t;
                }
                Set(o.at(x, y), c[0], c[1], c[2], I);
            }
        }
        return o;
    };
    const Col top = Hex(st->top), bot = Hex(st->bot);
    const Img a = layer(Hex(st->mb), Hex(st->mw), &top, &bot);
    const Img s = layer(Hex(st->sb), Hex(st->sw), nullptr, nullptr);
    constexpr int pad = 4;
    Img o(m.w + pad * 2, m.h + pad * 2);
    Over(o, st->shadow_a < 1.0f ? Fade(s, st->shadow_a) : s, pad + 2, pad + 2);
    Over(o, a, pad, pad);
    const float scale = cap / (special ? 38.0f : 31.0f);
    return Resize(o, std::max(1, static_cast<int>(std::lround(o.w * scale))),
                  std::max(1, static_cast<int>(std::lround(o.h * scale))));
}

std::optional<Img> Art::Background(int x0, int y0, int w, int h) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return std::nullopt;
    Img o(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            BackdropPixel(x0 + x, y0 + y, o.at(x, y));
    return o;
}

std::optional<Img> Art::MinimapFrame(bool pressed) {
    // Backdrop outside the circle, the soft drop shadow and the three-tone plank rim
    // (#5E4524 / #CEBE8D / #937544), transparent inside the map circle.
    Img o(kFrameS, kFrameS);
    const float cx = kMX - kFrameX, cy = kMY - kFrameY;
    for (int y = 0; y < o.h; ++y) {
        for (int x = 0; x < o.w; ++x) {
            float* px = o.at(x, y);
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const float r = std::sqrt(dx * dx + dy * dy);
            if (r < kMR - 1.0f) {
                continue;
            }
            BackdropPixel(kFrameX + x, kFrameY + y, px);
            // shadow: ellipse 350 x 355 centred 21 px below, blurred ~14 px, alpha 140
            const float ex = dx / kMR, ey = (dy - 21.0f) / (kMR + 5.0f);
            const float ed = (std::sqrt(ex * ex + ey * ey) - 1.0f) * kMR;
            const float sa = (140.0f / 255.0f) * (1.0f - SmoothStep(-28.0f, 28.0f, ed));
            const float sh[4] = {0, 0, 0, sa};
            Blend(px, sh);
            const struct {
                float rad;
                const char* col;
            } rings[] = {{kMR + 18.0f, "#5E4524"},
                         {kMR + 13.0f, pressed ? "#FFF5A5" : "#CEBE8D"},
                         {kMR + 6.0f, pressed ? "#CFAA6A" : "#937544"}};
            for (const auto& rg : rings) {
                const float a = Clamp01(rg.rad - r + 0.5f);
                if (a > 0.0f) {
                    const Col c = Hex(rg.col);
                    const float s[4] = {c.r, c.g, c.b, a};
                    Blend(px, s);
                }
            }
            px[3] *= Clamp01(r - (kMR - 1.0f)); // anti-aliased inner edge
        }
    }
    return o;
}

namespace {
// A pane's four vertex colours (top-left, top-right, bottom-left, bottom-right), bilinear across
// the picture, multiplied into it (alpha only: the panes here keep white RGB).
Img VtxAlpha(const Img& s, float tl, float tr, float bl, float br) {
    Img o = s;
    for (int y = 0; y < o.h; ++y) {
        const float v = o.h > 1 ? static_cast<float>(y) / (o.h - 1) : 0.0f;
        for (int x = 0; x < o.w; ++x) {
            const float u = o.w > 1 ? static_cast<float>(x) / (o.w - 1) : 0.0f;
            o.at(x, y)[3] *= (tl * (1 - u) + tr * u) * (1 - v) + (bl * (1 - u) + br * u) * v;
        }
    }
    return o;
}
Img Rot180(const Img& s) { return FlipV(FlipH(s)); }
// Native TEV of a one-texture pane: colour = black + (white - black) * texel, alpha * a.
Img TevA(const Img& s, std::string_view b, std::string_view w, float a = 1.0f) {
    Img o = s;
    const Col cb = Hex(b), cw = Hex(w);
    for (size_t i = 0; i < o.p.size(); i += 4) {
        o.p[i] = cb.r + (cw.r - cb.r) * o.p[i];
        o.p[i + 1] = cb.g + (cw.g - cb.g) * o.p[i + 1];
        o.p[i + 2] = cb.b + (cw.b - cb.b) * o.p[i + 2];
        o.p[i + 3] *= a;
    }
    return o;
}
// A texture repeated over w x h, `rx` x `ry` times (wrap mode repeat), or mirrored on odd tiles.
Img Tile(const Img& t, int w, int h, float rx, float ry, bool mirror = false) {
    Img o(w, h);
    if (t.empty())
        return o;
    for (int y = 0; y < h; ++y) {
        const float fv = (y + 0.5f) / h * ry;
        int ty = static_cast<int>(std::floor(fv * t.h));
        const int tiley = static_cast<int>(std::floor(fv));
        ty = ((ty % t.h) + t.h) % t.h;
        if (mirror && (tiley & 1)) ty = t.h - 1 - ty;
        for (int x = 0; x < w; ++x) {
            const float fu = (x + 0.5f) / w * rx;
            int tx = static_cast<int>(std::floor(fu * t.w));
            const int tilex = static_cast<int>(std::floor(fu));
            tx = ((tx % t.w) + t.w) % t.w;
            if (mirror && (tilex & 1)) tx = t.w - 1 - tx;
            std::memcpy(o.at(x, y), t.at(tx, ty), 16);
        }
    }
    return o;
}
// Horizontal three-slice: the left and right `cap` fraction of the source keep their aspect at
// the output height, the middle stretches (a native pane widened past its texture's width).
Img HStretch3(const Img& s, int w, int h, float cap) {
    const int sc = std::max(1, static_cast<int>(s.w * cap));
    const int oc = std::min(w / 2, std::max(1, static_cast<int>(std::lround(static_cast<float>(sc) * h / s.h))));
    Img o(w, h);
    Over(o, Resize(Crop(s, 0, 0, sc, s.h), oc, h), 0, 0);
    Over(o, Resize(Crop(s, s.w - sc, 0, sc, s.h), oc, h), w - oc, 0);
    if (w - 2 * oc > 0)
        Over(o, Resize(Crop(s, sc, 0, s.w - 2 * sc, s.h), w - 2 * oc, h), oc, 0);
    return o;
}
// Multiplies one picture's alpha by another's (same size): a pane material's second texture
// used as the shape mask.
Img MaskBy(const Img& s, const Img& m) {
    Img o = s;
    for (int y = 0; y < o.h && y < m.h; ++y)
        for (int x = 0; x < o.w && x < m.w; ++x)
            o.at(x, y)[3] *= m.at(x, y)[3];
    return o;
}
// The map's region colour table (Map2D map_00_type P_bace_14 / MapField mapPlace_00_color P_bg_00
// material keys): 0 sky brown, 1 forest green, 2 volcano red, 3 desert yellow, 4 light green,
// 5 water teal, 6 purple. Pairs are the material's black / white colours.
struct RegionCol {
    const char *plate_b, *plate_w, *place_b, *place_w;
};
constexpr RegionCol kRegionCols[] = {
    {"#594330", "#594330", "#3C2C13", "#594930"}, {"#468230", "#468230", "#296513", "#468230"},
    {"#823630", "#954943", "#651913", "#823630"}, {"#6D6D30", "#948526", "#505013", "#948526"},
    {"#73962D", "#73962D", "#567910", "#73962D"}, {"#2D9196", "#2D9196", "#107479", "#2D9196"},
    {"#645087", "#645087", "#645087", "#645087"}, // purple: plate only (mapPlace_00 has no purple key)
};
} // namespace

// The map's compass (Layout Map2D map_00 N_houkoul_00), every pane with its BRLYT material and
// vertex colours: a soft purple-brown shadow disc, the bronze disc (tr_rotateIconBg_00), the rose
// (tr_rotateIcon_01 mirrored into four quadrants, a white highlight over a black shadow one unit
// off), the needle's two shadows (tr_rotateIconS_00 at 32 / 64) and the two needle halves
// (tr_rotateIcon_04: north (246,137,118) -> (230,93,93), south (255,255,230) -> (195,194,180), each
// with its tr_rotateIcon_05 rim). Drawn on a 128-unit square at 1.25 px per unit, so the needle
// (58 units above and below the centre) keeps the layout's proportions.
std::optional<Img> Art::Compass() {
    auto bgs = Layout("Map2D", "tr_circle64S_00");
    auto bg = Layout("Map2D", "tr_rotateIconBg_00");
    auto rose = Layout("Map2D", "tr_rotateIcon_01");
    auto ns = Layout("Map2D", "tr_rotateIconS_00");
    auto n4 = Layout("Map2D", "tr_rotateIcon_04");
    auto n5 = Layout("Map2D", "tr_rotateIcon_05");
    if (!bgs || !bg || !rose || !ns || !n4 || !n5)
        return std::nullopt;
    constexpr float k = 1.25f;
    const int S = static_cast<int>(128 * k);
    const float c = S / 2.0f;
    auto px = [&](float units) { return std::max(1, static_cast<int>(std::lround(units * k))); };
    // pane centre in screen space from the layout's y-up position
    auto put = [&](Img& o, const Img& im, float ux, float uy) { OverCentered(o, im, c + ux * k, c - uy * k); };
    Img o(S, S);
    put(o, TevA(Resize(*bgs, px(96.8f), px(96.8f)), "#28143C", "#28143C", 64.0f / 255.0f), -6, -7);
    put(o, Resize(*bg, px(88), px(88)), 0, 0);
    const Img q = Resize(Quad(*rose), px(72), px(72));
    put(o, VtxAlpha(TevA(q, "#000000", "#000000", 128.0f / 255.0f), 0.5f, 0.0f, 1.0f, 0.5f), 1, 1);
    put(o, VtxAlpha(TevA(q, "#FFFFFF", "#FFFFFF"), 0.5f, 1.0f, 0.5f, 0.5f), 0, 0);
    const Img needle_s = Resize(*ns, px(32), px(100));
    put(o, TevA(needle_s, "#000000", "#000000", 32.0f / 255.0f), -3, -3);
    put(o, TevA(needle_s, "#000000", "#000000", 64.0f / 255.0f), 0, 0);
    const Img half = Resize(*n4, px(32), px(64)), rim = Resize(*n5, px(32), px(64));
    put(o, Rot180(TevA(half, "#FFFFE6", "#C3C2B4")), 0, -26);
    put(o, TevA(half, "#F68976", "#E65D5D"), 0, 26);
    put(o, Rot180(VtxAlpha(TevA(rim, "#000000", "#000000", 50.0f / 255.0f), 1, 0, 1, 0)), 0, -26);
    put(o, TevA(rim, "#E65D5D", "#E65D5D", 64.0f / 255.0f), 0, 26);
    return o;
}

// The map's area-name plate (Layout Map2D map_00 N_inOut_00), w x h px: the wooden board
// (tr_areaCaptionBg_01 mirrored twice across, (125,96,50) -> (162,132,81)), the two faint dark
// scrolls (tr_kazari_00 at 100), a pale paper halo (tr_kami_00 through the torn sheet tr_pattern_08,
// (208,187,106) at 80) and the region-coloured torn paper strip (map_00_type colour, vertex alpha
// 128 at the top-left and bottom-right), closed by the gold scroll ends (tr_kazari_05,
// (212,191,138) -> (229,201,132), with their black shadows at 64). Proportions from the layout
// (board 417.6 x 102.4 units, strip 369.6 x 56, ends 123 x 84); wider plates stretch the middle.
std::optional<Img> Art::AreaPlate(int w, int h, int region) {
    auto board = Layout("Map2D", "tr_areaCaptionBg_01");
    auto kazari = Layout("Map2D", "tr_kazari_00");
    auto kami = Layout("Map2D", "tr_kami_00");
    auto torn = Layout("Map2D", "tr_pattern_08");
    auto ends = Layout("Map2D", "tr_kazari_05");
    if (!board || !kazari || !kami || !torn || !ends || w < 64 || h < 32 || w > 2048 || h > 512 || region < 0 ||
        region >= static_cast<int>(std::size(kRegionCols)))
        return std::nullopt;
    const float k = h / 112.0f; // px per unit; the plate box is 112 units tall (board 102.4 + shadow)
    const int bw = w - static_cast<int>(16 * k), bh = static_cast<int>(102.4f * k);
    const float cx = bw / 2.0f, cy = 51.2f * k;
    Img o(w, h);
    // the board, with a soft shadow below-right (W_shadow_01, black at 32)
    const Img wood = HStretch3(Tile(*board, 128, 64, 2, 1, true), bw, bh, 0.12f);
    const Img b = TevA(wood, "#7D6032", "#A28451");
    Over(o, TevA(b, "#000000", "#000000", 0.18f), static_cast<int>(6 * k), static_cast<int>(6 * k));
    Over(o, b, 0, 0);
    // inner widths scale with the board (native 369.6 / 417.6 and 386.4 / 417.6)
    const int iw = static_cast<int>(bw * 0.885f), ih = static_cast<int>(56 * k);
    const int hw = static_cast<int>(bw * 0.925f), hh = static_cast<int>(74.8f * k);
    const int kz = static_cast<int>(38.5f * k);
    {
        const Img kzi = TevA(Resize(*kazari, kz, kz), "#28190A", "#28190A", 100.0f / 255.0f);
        OverCentered(o, FlipV(kzi), cx - iw * 0.39f, cy + 5 * k);
        OverCentered(o, FlipH(kzi), cx + iw * 0.35f, cy - 5 * k);
    }
    const Img tornh = HStretch3(*torn, hw, hh, 0.1f);
    OverCentered(o, TevA(MaskBy(Tile(*kami, hw, hh, 6, 2), tornh), "#D0BB6A", "#D0B970", 80.0f / 255.0f), cx, cy);
    const RegionCol& rc = kRegionCols[region];
    const Img torni = HStretch3(*torn, iw, ih, 0.1f);
    Img strip = TevA(MaskBy(Tile(*kami, iw, ih, 6, 2), torni), rc.plate_b, rc.plate_w);
    // the paper stage: the kami grain adds a warm light over the colour (fitted to the native
    // Sealed Grounds plate, 2026-11-11 capture 05)
    {
        const Img grain = Tile(*kami, iw, ih, 6, 2);
        for (int y = 0; y < ih; ++y)
            for (int x = 0; x < iw; ++x) {
                float* p = strip.at(x, y);
                const float g = grain.at(x, y)[0];
                const float warm[3] = {0.30f, 0.22f, 0.06f};
                for (int ch = 0; ch < 3; ++ch)
                    p[ch] = Clamp01(p[ch] * (0.9f + 0.1f * g) + warm[ch] * (0.6f + 0.4f * g));
            }
    }
    OverCentered(o, VtxAlpha(strip, 0.5f, 1.0f, 1.0f, 0.5f), cx, cy);
    // gold scroll ends (P_waku_00 vertically flipped on the left, P_waku_01 mirrored on the right)
    const int ew = static_cast<int>(123 * k), eh = static_cast<int>(84 * k);
    const Img end = Resize(*ends, ew, eh);
    const float ex = bw / 2.0f - 75.3f * k; // native end centres sit 75.3 units inside the board ends
    const Img left = FlipV(end), right = FlipH(end);
    OverCentered(o, TevA(left, "#000000", "#000000", 64.0f / 255.0f), cx - ex - 1.5f * k, cy + 2 * k);
    OverCentered(o, TevA(left, "#D4BF8A", "#E5C984"), cx - ex, cy);
    OverCentered(o, TevA(right, "#000000", "#000000", 64.0f / 255.0f), cx + ex - 2 * k, cy + 2 * k);
    OverCentered(o, TevA(right, "#D4BF8A", "#E5C984"), cx + ex, cy);
    return o;
}

// The neighbouring-area plate (Layout MapField mapPlace_00): a band of horizontal stripes
// (tr_stripe_01 repeated 4 times down) in the region colour (mapPlace_00_color) at alpha 100,
// fading out over its two end panes (vertex alpha 0 at the outer edges), w x h px.
std::optional<Img> Art::PlacePlate(int w, int h, int region) {
    auto stripe = Layout("Map2D", "tr_stripe_01");
    if (!stripe || w < 32 || h < 8 || w > 2048 || h > 512 || region < 0 ||
        region >= static_cast<int>(std::size(kRegionCols)))
        return std::nullopt;
    const RegionCol& rc = kRegionCols[region];
    const int fw = std::max(1, static_cast<int>(w * 32.0f / 204.0f)); // 32-unit fades on a 140-unit band
    Img band = TevA(Tile(*stripe, w, h, static_cast<float>(w) / h * 45.0f / 140.0f * 1.0f, 4), rc.place_b, rc.place_w,
                    100.0f / 255.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float a = 1.0f;
            if (x < fw) a = static_cast<float>(x) / fw;
            else if (x >= w - fw) a = static_cast<float>(w - 1 - x) / fw;
            band.at(x, y)[3] *= a;
        }
    return band;
}

// Collection panels (Layout MenuPause pause_01): kind 0 = the bug panel (green), 1 = the quest
// item row (purple), 2 = the treasure panel (tan). The window (tr_commonWinLT_08 corners: outline
// in the frame material's black, body in its white, the content in the window's white), the
// panel paper (tr_kami_08 shape with tr_kami_00 grain between the P_bace material's black and
// white) and the drop shadow (tr_commonWinLTS_00, black at 100, 8 units down-left). w x h px with
// `k` px per layout unit implied by the corner size (16 units).
std::optional<Img> Art::CollPanel(int w, int h, int kind) {
    auto corner = Layout("MenuPause", "tr_commonWinLT_08");
    auto cs = Layout("MenuPause", "tr_commonWinLTS_00");
    auto shape = Layout("MenuPause", "tr_kami_08");
    auto grain = Layout("MenuPause", "tr_kami_00");
    if (!corner || !cs || !shape || !grain || w < 64 || h < 64 || w > 2048 || h > 2048 || kind < 0 || kind > 2)
        return std::nullopt;
    struct K {
        const char *frame_b, *frame_w, *content, *paper_b, *paper_w;
        float rep_y;
    };
    static constexpr K kK[] = {
        {"#4F9A83", "#B7FFB5", "#B7FFB5", "#6592A0", "#9DDE90", 18},
        {"#794E99", "#F1B5FF", "#E8B5FF", "#9072AA", "#DE90DE", 7},
        {"#9A784F", "#FFE6B4", "#FFE6B4", "#997F6C", "#DEBF90", 18},
    };
    const K& m = kK[kind];
    const int pad = 24;
    Img o(w + pad, h + pad);
    // every panel is 340 units wide; the frame pieces are 32 units
    const int c = std::max(8, std::min(h / 2 - 1, w * 32 / 340));
    auto window = [&](const Img& cor, Col body) {
        Img win(w, h);
        const Img a = Resize(cor, c, c);
        for (int y = c; y < h - c; ++y)
            for (int x = c; x < w - c; ++x)
                Set(win.at(x, y), body.r, body.g, body.b, 1);
        Over(win, a, 0, 0);
        Over(win, FlipH(a), w - c, 0);
        Over(win, FlipV(a), 0, h - c);
        Over(win, Rot180(a), w - c, h - c);
        // edges: the corner's inner row / column stretched along each side (window frame pieces)
        const Img top = Resize(Crop(a, c - 1, 0, 1, c), w - 2 * c, c);
        const Img left = Resize(Crop(a, 0, c - 1, c, 1), c, h - 2 * c);
        Over(win, top, c, 0);
        Over(win, FlipV(top), c, h - c);
        Over(win, left, 0, c);
        Over(win, FlipH(left), w - c, c);
        return win;
    };
    // shadow (W_shadow: the corner shadow texture, black, alpha 100, offset 8 units down-left)
    const Img sh = window(*cs, Hex("#000000"));
    Over(o, TevA(sh, "#000000", "#000000", 100.0f / 255.0f * 0.8f), 0, pad);
    // the window: outline = frame black, body = frame white, content = window white
    Img win = window(TevA(*corner, m.frame_b, m.frame_w), Hex(m.content));
    // the panel paper over the window, inset like the native (the kami_08 shape has its own margin)
    // paper colour = lerp(black, white) by the grain, held to the middle of the range, the shape's
    // facets lightening it (fitted to the native Collection, 2026-11-11 capture 06)
    const int mi = std::max(4, std::min(w, h) / 28);
    const Img sp = Resize(*shape, w - 2 * mi, h - 2 * mi);
    const Img gr = Tile(*grain, w - 2 * mi, h - 2 * mi, 4, m.rep_y);
    Img paper(sp.w, sp.h);
    const Col pb = Hex(m.paper_b), pw = Hex(m.paper_w);
    for (int y = 0; y < sp.h; ++y)
        for (int x = 0; x < sp.w; ++x) {
            const float t = Clamp01(0.7f + 0.3f * gr.at(x, y)[0] + 0.25f * (sp.at(x, y)[0] - 0.8f));
            Set(paper.at(x, y), Clamp01(pb.r + (pw.r - pb.r) * t + 0.05f), Clamp01(pb.g + (pw.g - pb.g) * t + 0.05f),
                Clamp01(pb.b + (pw.b - pb.b) * t + 0.05f), Clamp01(sp.at(x, y)[3] * 1.2f));
        }
    Over(win, paper, mi, mi);
    Over(o, win, pad, 0);
    return o;
}

// Link's view cone on the maps (Layout MapField mapIcon_00 N_player_00 / P_sight_01: tr_sight_00,
// black (147,131,55,0) -> white (255,247,146,255), pane alpha 180), turned `deg` degrees
// clockwise from map-up, on a kConeCanvas square whose centre is Link's head. The trapezoid's
// narrow end sits under the head and its wide end points along the facing; its size is fitted to
// the native map against the head icon's visible width (native captures 01 / 03 of 2026-11-11: far
// edge 2.33 and far width 3.2 head widths out), for the 84 px tr_linkPIcon_00 the pages draw.
std::optional<Img> Art::Cone(int deg) {
    auto sight = Layout("MapField", "tr_sight_00");
    if (!sight || deg < 0 || deg >= 360)
        return std::nullopt;
    constexpr int C = kConeCanvas;
    constexpr float head = 84 * 62.0f / 80.0f;    // visible head width (icon alpha bbox 62 / 80)
    const int q = static_cast<int>(head * 3.2f / (119.0f / 128.0f)); // quad: texture rows 4..123 are the wide end
    const float far = head * 2.33f;
    Img up(C, C);
    const Img cone = Rot180(TevA(Resize(*sight, q, q), "#938337", "#FFF792", 180.0f / 255.0f));
    // after the turn the wide end is texture row 128 - 101 = 27
    Over(up, cone, C / 2 - q / 2, static_cast<int>(C / 2.0f - far - q * 27.0f / 128.0f));
    if (deg == 0)
        return up;
    // turn about the centre (bilinear), clockwise on screen
    Img o(C, C);
    const float a = deg * 3.14159265f / 180.0f, ca = std::cos(a), sa = std::sin(a), c = (C - 1) / 2.0f;
    for (int y = 0; y < C; ++y)
        for (int x = 0; x < C; ++x) {
            const float dx = x - c, dy = y - c;
            const float sx = ca * dx + sa * dy + c, sy = -sa * dx + ca * dy + c;
            const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            const float fx = sx - x0, fy = sy - y0;
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const int xx = x0 + (k & 1), yy = y0 + (k >> 1);
                if (xx < 0 || yy < 0 || xx >= C || yy >= C)
                    continue;
                const float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                const float* p = up.at(xx, yy);
                for (int ch = 0; ch < 3; ++ch)
                    acc[ch] += p[ch] * p[3] * wgt; // premultiplied
                acc[3] += p[3] * wgt;
            }
            float* d = o.at(x, y);
            if (acc[3] > 0)
                Set(d, acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], acc[3]);
        }
    return o;
}

// One Collection slot (tr_subBugBase_00 at alpha 160 in the panel's slot colour).
std::optional<Img> Art::CollSlot(int s, int kind) {
    auto base = Layout("MenuPause", "tr_subBugBase_00");
    if (!base || s < 8 || s > 512 || kind < 0 || kind > 2)
        return std::nullopt;
    static constexpr const char* kB[] = {"#084441", "#2B134F", "#7A4017"};
    static constexpr const char* kW[] = {"#1D4545", "#3A114F", "#5F3E11"};
    return TevA(Resize(*base, s, s), kB[kind], kW[kind], 160.0f / 255.0f);
}

std::optional<Img> Art::Socket(int s) {
    auto base = Layout("DoButton", "tr_itemBace_03");
    if (!base || s <= 0 || s > 1024)
        return std::nullopt;
    Img o(s, s);
    Over(o, Tint(Resize(Quad(*base), s, s), Hex("#4A3518"), Hex("#CFAA6A")), 0, 0);
    const int in = static_cast<int>(s * 0.74f);
    FillEllipse(o, s / 2.0f, s / 2.0f, in / 2.0f, in / 2.0f, Hex("#82673C"));
    return o;
}

std::optional<Img> Art::Beacon(int s) {
    auto glow = Layout("Map2D", "tr_circleM_00");
    auto c64 = Layout("Map2D", "tr_circle64_00");
    auto pin = Layout("Map2D", "tr_iconPinF_00");
    if (!glow || !c64 || !pin || s <= 0 || s > 512)
        return std::nullopt;
    Img big(2 * s, 2 * s);
    const float c = static_cast<float>(s);
    const int g = static_cast<int>(s * 1.7f), i84 = static_cast<int>(s * 0.84f), i30 = static_cast<int>(s * 0.3f);
    OverCentered(big, AMask(Resize(*glow, g, g), Hex("#9FF3FF"), 0.55f), c, c);
    OverCentered(big, AMask(Resize(*c64, s, s), Hex("#F4FFFF")), c, c);
    OverCentered(big, AMask(Resize(*c64, i84, i84), Hex("#18B4E8")), c, c);
    OverCentered(big, Tint(Resize(*pin, i84, i84), Hex("#18B4E8"), Hex("#AEF4FF")), c, c);
    OverCentered(big, AMask(Resize(*c64, i30, i30), Hex("#FFFFFF")), c, c);
    return Crop(big, s / 2 - 10, s / 2 - 10, s + 20, s + 20);
}

std::optional<Img> Art::Digit(int d, int h) {
    if (d < 0 || d > 9 || h <= 0 || h > 512)
        return std::nullopt;
    char name[32];
    std::snprintf(name, sizeof(name), "th_Number_%02d", d);
    auto im = Layout("DoButton", name);
    if (!im)
        return std::nullopt;
    return Resize(Tint(*im, Hex("#000000"), Hex("#FFFFFF")), static_cast<int>(std::lround(48.0 * h / 64.0)), h);
}

std::optional<Img> Art::Plank(int w, int h, std::string_view variant) {
    if (w < 8 || h < 8 || w > 2048 || h > 2048)
        return std::nullopt;
    Col body, blk, wht;
    float alpha = 1.0f;
    if (variant == "corner") {
        body = Hex("#3A3020"), blk = Hex("#1E160A"), wht = Hex("#CEBE8D"), alpha = 0.95f;
    } else if (variant == "foot") {
        body = Hex("#6E5434"), blk = Hex("#3A2810"), wht = Hex("#E6CF92");
    } else {
        return std::nullopt;
    }
    auto lt = Layout("DoButton", "tr_frameLT_00");
    auto tt = Layout("DoButton", "tr_frameT_00");
    auto kami = Layout("MenuPause", "tr_kami_05");
    auto rlt = Layout("MenuPause", "tr_commonWinLT_01");
    auto rld = Layout("MenuPause", "tr_commonWinLD_00");
    if (!lt || !tt || !kami || !rlt || !rld)
        return std::nullopt;
    // window8: solid material colour through the feathered frame masks
    const int cs = std::min(48, h / 2 - 2);
    Img im(w, h);
    for (int y = cs; y < h - cs; ++y)
        for (int x = cs; x < w - cs; ++x)
            Set(im.at(x, y), body.r, body.g, body.b, 1);
    const Img co = Resize(Tint(*lt, body, body), cs, cs);
    Over(im, co, 0, 0);
    Over(im, FlipH(co), w - cs, 0);
    Over(im, FlipV(co), 0, h - cs);
    Over(im, FlipV(FlipH(co)), w - cs, h - cs);
    const Img ttc = Tint(*tt, body, body);
    const Img e = Resize(ttc, w - 2 * cs, cs);
    Over(im, e, cs, 0);
    Over(im, FlipV(e), cs, h - cs);
    const Img ev = Resize(Rot90(ttc), cs, h - 2 * cs);
    Over(im, ev, 0, cs);
    Over(im, FlipH(ev), w - cs, cs);
    // paper grain, masked by the plank, then the top-light shading ramp
    const Img grain = Tint(Resize(*kami, w, h), Hex("#5A3C1E"), Hex("#FFFFFF"), 0.16f);
    for (int y = 0; y < h; ++y) {
        const float t = 1.08f + (0.86f - 1.08f) * (h > 1 ? static_cast<float>(y) / (h - 1) : 0.0f);
        for (int x = 0; x < w; ++x) {
            float* px = im.at(x, y);
            const float ma = px[3];
            float g[4];
            std::memcpy(g, grain.at(x, y), sizeof(g));
            g[3] *= ma;
            Blend(px, g);
            for (int c = 0; c < 3; ++c)
                px[c] = Clamp01(px[c] * t);
        }
    }
    // gold rim (W_waku): corners + stretched edge columns of tr_commonWinLT_01 / LD_00
    const int rc = std::min(32, h / 2 - 2);
    const Img a = Resize(Tint(*rlt, blk, wht), rc, rc), b = Resize(Tint(*rld, blk, wht), rc, rc);
    Over(im, a, 0, 0);
    Over(im, FlipH(a), w - rc, 0);
    Over(im, b, 0, h - rc);
    Over(im, FlipH(b), w - rc, h - rc);
    Over(im, Resize(Crop(a, rc - 1, 0, 1, rc), w - 2 * rc, rc), rc, 0);
    Over(im, Resize(Crop(b, rc - 1, 0, 1, rc), w - 2 * rc, rc), rc, h - rc);
    const Img lef = Resize(Crop(a, 0, rc - 1, rc, 1), rc, h - 2 * rc);
    Over(im, lef, 0, rc);
    Over(im, FlipH(lef), w - rc, rc);
    if (alpha < 1.0f)
        im = Fade(im, alpha);
    // soft drop shadow, 8 px down-right
    Img out(w + 16, h + 16);
    std::vector<float> m(static_cast<size_t>(out.w) * out.h, 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            m[static_cast<size_t>(y + 8) * out.w + x + 8] = im.at(x, y)[3] * 120.0f / 255.0f;
    BlurMask(m, out.w, out.h, 4);
    const Col sc = Hex("#1E1206");
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            Set(out.at(x, y), sc.r, sc.g, sc.b, m[static_cast<size_t>(y) * out.w + x]);
    Over(out, im, 0, 0);
    return out;
}

// ---------------------------------------------------------------- area maps

namespace {
constexpr AreaMap kAreas[] = {
    {"F401", -20480.32f, 7684.18f, -7573.23f, 15558.50f, 0.07f, false}, // Sealed Grounds
    // dev.32: the Sealed Grounds by the temple (F402) in its own geometry (the F401 frame; the
    // module binds it while Link is in F402)
    {"F402", -20480.32f, 7684.18f, -7573.23f, 15558.50f, 0.07f, false},
    {"F000", -12000.00f, 16000.00f, -16500.00f, 10500.00f, 0.065f, true, true, true}, // Skyloft island (cloud plane cropped)
    {"F100", -20205.19f, 15220.30f, -13870.62f, 17657.00f, 0.055f, false, true, true}, // Faron Woods
    {"D100", -12360.00f, 9789.19f, -20613.68f, 9144.76f, 0.07f, false, true, true}, // Skyview Temple: native +x right (dev.30, live D100 overview)
    {"D200", -12300.81f, 12374.02f, -19320.00f, 17557.12f, 0.06f, false, true, true}, // Earth Temple: native +x right (dev.30)
    {"D101", -18220.10f, 12400.00f, -13649.60f, 7030.71f, 0.075f, false, true, true}, // Ancient Cistern: native +x right
    {"D201", -14268.70f, 11420.00f, -23078.00f, 9030.00f, 0.068f, false, true, true}, // Fire Sanctuary: native +x right
    {"D300", -9100.00f, 11100.00f, -26100.00f, 5000.00f, 0.075f, false, true, true}, // Lanayru Mining Facility: native +x right (dev.30)
    {"D301", -15000.00f, 8300.00f, -10000.00f, 10000.00f, 0.08f, false, true, true}, // Sandship: native bow on the left
    // Sky Keep's board (dev.32): 3 x 3 cells kSkyKeepPitch apart, +x right / +z down
    {"D003", -10500.0f, 10500.0f, -10500.0f, 10500.0f, kSkyKeepPpu, false, true, true},
    {"D003_0", -3457.91f, 4300.00f, -3027.39f, 4300.00f, 0.1f, false, true, true}, // Sky Keep: +x right like the native tile (live D003_0, dev.31)
    {"D003_1", -3030.67f, 3500.00f, -3500.00f, 2882.81f, 0.1f, false, true, true}, // Sky Keep: +x right like the native tile (live D003_0, dev.31)
    {"D003_2", -3785.00f, 3785.00f, -3200.01f, 3075.54f, 0.1f, false, true, true}, // Sky Keep: +x right like the native tile (live D003_0, dev.31)
    {"D003_3", -4300.00f, 2900.00f, -2800.00f, 3000.00f, 0.1f, false, true, true}, // Sky Keep: +x right like the native tile (live D003_0, dev.31)
    {"D003_4", -3000.00f, 3000.00f, -4300.00f, 4300.00f, 0.1f, false, true, true}, // Sky Keep: +x right like the native tile (live D003_0, dev.31)
    {"D003_5", -4300.00f, 3105.33f, -4300.00f, 3047.94f, 0.1f, false, true, true}, // Sky Keep: +x right like the native tile (live D003_0, dev.31)
    {"D003_6", -4300.00f, 2020.45f, -3000.00f, 4300.00f, 0.1f, false, true, true}, // Sky Keep: +x right like the native tile (live D003_0, dev.31)
    {"D003_7", -3354.85f, 4300.00f, -3354.85f, 4300.00f, 0.1f, false, true, true}, // Sky Keep: +x right like the native tile (live D003_0, dev.31)
    {"D003_8", -1642.20f, 1644.84f, -1642.20f, 1642.20f, 0.1f, false, true, true}, // Sky Keep: +x right like the native tile (live D003_0, dev.31)
    {"F200", -27285.70f, 32744.30f, -51405.40f, 16608.50f, 0.029f, false, true, true}, // Eldin Volcano
    {"F300", -42247.30f, 18011.90f, -17744.30f, 24101.20f, 0.033f, false, true, true}, // Lanayru Desert
    {"F301", -5000.00f, 17500.00f, -13000.00f, 11000.00f, 0.0843f, false, true, true}, // Sand Sea: crop the empty sea and outer barrier, retain all port floors
    {"F302", -32653.30f, 25319.10f, -14750.40f, 22875.10f, 0.04f, false, true, true}, // Lanayru Gorge
    {"F303", -6500.00f, 5945.44f, -6213.61f, 6700.00f, 0.15f, false, true, true},    // Lanayru Caves
    // dev.33 (static map analysis C5): the Shipyard (one room, +x right /
    // +z down, fitted to the hero's native capture) and the sailing Sand Sea (its sea-chart state =
    // the past background room_obj0k, kMapPast)
    {"F301_4", -38500.0f, 19800.0f, -17800.0f, 21200.0f, 0.04f, false, true, true},
    {"F301_1", -141400.0f, 8800.0f, -46200.0f, 55800.0f, 0.0155f, false, true, true},
    // Isle of Songs interior (dev.30): the native map shows the room (owner capture 16, 2026-11-11)
    {"F010r", -3731.92f, 3731.92f, -3744.08f, 5489.41f, 0.2f, false, true, true},
    // dev.34 (item 1, live S100 in a trial, stage survey): the native trial map
    // draws the whole realm only as a small blob under Link (about 325 world units per top-screen
    // pixel, no zoom in or out); the pages draw it at that size against Link, so 0.02 px per unit is plenty
    {"S000", -20000.00f, 25000.00f, -26314.60f, 25000.00f, 0.04f, false, true, true}, // The Goddess's Silent Realm: the whole realm (live)
    {"S100", -18243.00f, 15217.40f, -13852.70f, 17545.30f, 0.02f, false, true, true}, // Farore's Silent Realm
    {"S200", -27285.70f, 32744.30f, -51405.40f, 16608.50f, 0.02f, false, true, true}, // Din's Silent Realm
    {"S300", -42247.30f, 18011.90f, -17744.30f, 24101.20f, 0.02f, false, true, true}, // Nayru's Silent Realm
    {"F020", -180000.0f, 180000.0f, -140000.0f, 110000.0f, 0.0055f, false}, // the Sky (SkyMap)
};

// A map triangle (dev.32): an upward, map-tagged collision face with its native map class.
struct Tri {
    float x[3], y[3], z[3];
    uint8_t cls;     // native map class 1..30 (dBgS::GetMapCode)
    uint8_t bg;      // registered-background state: 0 both, 1 present only, 2 past only
    int16_t room;    // room index, -1 = stage-level actor collision (always drawn in full)
};

// One SPLC record's words as the module uses them (code0, code1, code3).
struct Plc {
    uint32_t c0, c1, c3;
};

std::vector<Plc> PlcRows(const std::vector<uint8_t>& d) {
    std::vector<Plc> out;
    if (d.size() < 8 || Be32(d.data()) != 0x53504C43) // "SPLC"
        return out;
    const uint16_t size = Be16(&d[4]), count = Be16(&d[6]);
    if (size < 0x14 || 8 + static_cast<size_t>(size) * count > d.size())
        return out;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* r = &d[8 + static_cast<size_t>(i) * size];
        out.push_back({Be32(r), Be32(r + 4), Be32(r + 12)});
    }
    return out;
}

bool Degenerate(const Tri& t) {
    for (int a = 0; a < 3; ++a) {
        const int b = (a + 1) % 3;
        const float dx = t.x[b] - t.x[a], dy = t.y[b] - t.y[a], dz = t.z[b] - t.z[a];
        if (dx * dx + dy * dy + dz * dz < 1e-6f)
            return true;
    }
    return false;
}

float LeF32(const uint8_t* p) {
    const uint32_t v = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                       static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// A KCL background (room.kcl / room_n.kcl / room_obj<n>k.kcl) with its PLC: every prism whose
// face normal points up, whose PLC record is in range and map-tagged (att0 >= 32), in its native
// map class. Positions come from the HD predecoded room.kcl2 (float32 LE [N][3][3] in prism
// order) when its size matches, else from the prism reconstruction (dBgWKCol::GetTriPnt).
void KclTris(const std::vector<uint8_t>& d, const std::vector<uint8_t>* kcl2, const std::vector<Plc>& plc,
             int room, uint8_t bg, bool trial, std::vector<Tri>& out) {
    if (d.size() < 0x14 || plc.empty())
        return;
    const size_t pos_off = Be32(&d[0]), nrm_off = Be32(&d[4]), tri_off = static_cast<size_t>(Be32(&d[8])) + 0x10,
                 oct_off = Be32(&d[12]);
    if (!(pos_off < nrm_off && nrm_off < tri_off && tri_off <= oct_off && oct_off <= d.size()))
        return;
    const size_t nv = (nrm_off - pos_off) / 12, nn = (tri_off - nrm_off) / 12, nt = (oct_off - tri_off) / 16;
    const bool direct = kcl2 != nullptr && kcl2->size() == nt * 36;
    auto vec = [&](size_t base, size_t i, double* o) {
        for (int k = 0; k < 3; ++k)
            o[k] = BeF32(&d[base + i * 12 + k * 4]);
    };
    for (size_t i = 0; i < nt; ++i) {
        const uint8_t* t = &d[tri_off + i * 16];
        const double height = BeF32(t);
        const uint16_t pi = Be16(t + 4), fi = Be16(t + 6), e1 = Be16(t + 8), e2 = Be16(t + 10),
                       e3 = Be16(t + 12), attr = Be16(t + 14);
        if (pi >= nv || fi >= nn || e1 >= nn || e2 >= nn || e3 >= nn || attr >= plc.size())
            continue;
        double v1[3], fn[3];
        vec(pos_off, pi, v1);
        vec(nrm_off, fi, fn);
        if (!(fn[1] > 0.0))
            continue; // downward / vertical faces are not on the map
        const int cls = Art::MapClass(plc[attr].c0, plc[attr].c1, plc[attr].c3, trial);
        if (cls <= 0)
            continue;
        Tri tr{};
        if (direct) {
            const uint8_t* p = &(*kcl2)[i * 36];
            for (int v = 0; v < 3; ++v) {
                tr.x[v] = LeF32(p + v * 12);
                tr.y[v] = LeF32(p + v * 12 + 4);
                tr.z[v] = LeF32(p + v * 12 + 8);
            }
        } else {
            double n1[3], n2[3], n3[3];
            vec(nrm_off, e1, n1);
            vec(nrm_off, e2, n2);
            vec(nrm_off, e3, n3);
            auto cross = [](const double* a, const double* b, double* o) {
                o[0] = a[1] * b[2] - a[2] * b[1];
                o[1] = a[2] * b[0] - a[0] * b[2];
                o[2] = a[0] * b[1] - a[1] * b[0];
            };
            double ca[3], cb[3];
            cross(n1, fn, ca);
            cross(n2, fn, cb);
            const double db = cb[0] * n3[0] + cb[1] * n3[1] + cb[2] * n3[2];
            const double da = ca[0] * n3[0] + ca[1] * n3[1] + ca[2] * n3[2];
            if (da == 0 || db == 0)
                continue;
            for (int k = 0; k < 3; ++k) {
                const double v2 = v1[k] + cb[k] * (height / db), v3 = v1[k] + ca[k] * (height / da);
                float* dst = k == 0 ? tr.x : (k == 1 ? tr.y : tr.z);
                dst[0] = static_cast<float>(v1[k]);
                dst[1] = static_cast<float>(v2);
                dst[2] = static_cast<float>(v3);
            }
        }
        bool finite = true;
        for (int v = 0; v < 3; ++v)
            finite = finite && std::isfinite(tr.x[v]) && std::isfinite(tr.y[v]) && std::isfinite(tr.z[v]) &&
                     std::fabs(tr.x[v]) < 1e7f && std::fabs(tr.z[v]) < 1e7f;
        if (!finite || Degenerate(tr))
            continue;
        tr.cls = static_cast<uint8_t>(cls);
        tr.bg = bg;
        tr.room = static_cast<int16_t>(room);
        out.push_back(tr);
    }
}

// A stage-level DZB (cBgD_t: vertex count / table, triangle count / table of u16 v0 v1 v2 id grp,
// ...) with its PLC (row = the triangle's id): the upward map-tagged faces. Only world-space
// actor collision is used (the actor sits at the origin; its Root group is the identity).
void DzbTris(const std::vector<uint8_t>& d, const std::vector<Plc>& plc, bool trial, std::vector<Tri>& out,
             int room = -1) {
    if (d.size() < 0x34 || plc.empty())
        return;
    const uint32_t nv = Be32(&d[0]), vo = Be32(&d[4]), nt = Be32(&d[8]), to = Be32(&d[12]);
    if (static_cast<size_t>(vo) + static_cast<size_t>(nv) * 12 > d.size() ||
        static_cast<size_t>(to) + static_cast<size_t>(nt) * 10 > d.size())
        return;
    for (uint32_t i = 0; i < nt; ++i) {
        const uint8_t* t = &d[to + static_cast<size_t>(i) * 10];
        const uint16_t ix[3] = {Be16(t), Be16(t + 2), Be16(t + 4)}, id = Be16(t + 6);
        if (ix[0] >= nv || ix[1] >= nv || ix[2] >= nv || id >= plc.size())
            continue;
        const int cls = Art::MapClass(plc[id].c0, plc[id].c1, plc[id].c3, trial);
        if (cls <= 0)
            continue;
        Tri tr{};
        for (int v = 0; v < 3; ++v) {
            const uint8_t* p = &d[vo + static_cast<size_t>(ix[v]) * 12];
            tr.x[v] = BeF32(p);
            tr.y[v] = BeF32(p + 4);
            tr.z[v] = BeF32(p + 8);
        }
        // the face normal's y (counter-clockwise front, as the KCL faces): upward only
        const float ax = tr.x[1] - tr.x[0], az = tr.z[1] - tr.z[0], bx = tr.x[2] - tr.x[0], bz = tr.z[2] - tr.z[0];
        if (!(az * bx - ax * bz > 0.0f) || Degenerate(tr))
            continue;
        tr.cls = static_cast<uint8_t>(cls);
        tr.bg = 0;
        tr.room = static_cast<int16_t>(room);
        out.push_back(tr);
    }
}

// World-space actor collision a stage registers on its map (map colour
// analysis; dev.32 survey of every stage's dzb/ and oarc/*/dzb/: only these are authored in world
// space with map-tagged faces and a static state; the others are actor-local and need their
// actor's transform, or move with the story: Eldin's rising lava):
// Skyloft's lake, Faron Woods' two ponds and Skyview
// Temple's room water (class 7), the Mining Facility's quicksand, the Fire Sanctuary's lava sheets.
// arc "" = the stage archive
// itself; room -1 = no room.
struct StageDzb {
    const char* stage;
    const char* arc;
    const char* name;
    int room;
};
constexpr StageDzb kStageDzbs[] = {
    // (dev.35: not S000's copy: the live native S000 map, trial or not, draws the lake bed there,
    // sand yellow / its trial cyan, and no water)
    {"F000", "", "StageF000Water", -1},
    {"F100", "WaterF100", "WaterF100", -1},      {"F100", "WaterF100_b", "WaterF100_b", -1},
    {"D100", "WaterD100_r02", "r02_w", 2},       {"D100", "WaterD100_r03", "r03_w", 3},
    {"D100", "WaterD100_r04", "r04_w", 4},       {"D100", "WaterD100_r06", "r06_w", 6},
    // the Lanayru Mining Facility's quicksand floors over rooms 2 and 3 (class 29, live dev.32:
    // the native present-day map fills both rooms with quicksand)
    {"D300", "", "StageSand02", 2},              {"D300", "", "StageSand03", 3},
    // the Fire Sanctuary's lava sheets (class 6; live dev.32 hero 1F: the native map shows them),
    // each with the room it lies over most (rooms 5, 6, 10)
    {"D201", "DwnLavaD2", "DwnLavaD2_B", 5},      {"D201", "FlowLavaD2", "FlowLavaD2_A", 6},
    {"D201", "FlowLavaD2", "FlowLavaD2_B", 10},
};

// dev.35 (dev.35 analysis, item 2): the water surface actor WaterSf (OBJS at the
// origin, no turn, so its dzb is world space) of a layer: its params nibble (p1 >> 16 & 0xF) 6 / 7 / 9
// selects the layer archive's oarc/WaterF104A / B / D, a class-7 sheet. Inside the Great Tree F100_1
// layers 1 / 2 the pool at y -400, layer 3 at y -100 (the native 1F map draws it, live dev.34 hero
// layer 3); F103_1 layers 1 / 2 the upper pool at y 3400. Layer 0 places none.
struct LayerDzb {
    const char* stage;
    uint32_t layers; // bit n = layer n
    const char* arc; // oarc/<arc>.arc with dzb/<arc>.dzb + dat/<arc>.plc
};
constexpr LayerDzb kLayerDzbs[] = {
    {"F100_1", (1u << 1) | (1u << 2), "WaterF104A"},
    {"F100_1", 1u << 3, "WaterF104D"},
    {"F103_1", (1u << 1) | (1u << 2), "WaterF104B"},
};

bool SilentRealm(std::string_view st) {
    return st.size() == 4 && st[0] == 'S' && st[1] >= '0' && st[1] <= '3' && st.substr(2) == "00";
}
} // namespace

int Art::MapClass(uint32_t code0, uint32_t code1, uint32_t code3, bool trial) {
    const int att0 = static_cast<int>(code3 >> 5 & 0x3F), att1 = static_cast<int>(code1 >> 17 & 7);
    if (att0 < 0x20)
        return -1;
    const int base = att0 - 0x20;
    if (trial && (code0 & 0x00200000u) != 0)
        return 30; // a Silent Realm trial draws target-through faces as the realm's own class
    switch (base) {
    case 4: if (att1 == 1) return 19; break;
    case 5: if (att1 == 1) return 20; if (att1 == 2) return 21; break;
    case 8: if (att1 == 1) return 22; break;
    case 10: if (att1 == 1) return 23; if (att1 == 2) return 24; break;
    case 12: if (att1 == 1) return 25; break;
    case 13: if (att1 == 1) return 26; if (att1 == 2) return 27; if (att1 == 3) return 28; break;
    case 15: if (att1 == 1) return 29; break;
    default: break;
    }
    return base < 31 ? base : -1;
}

namespace {
// Common map materials (Object/NX/ObjectPack.arc.LZ -> oarc/Common.arc): the solid texture of
// each map class (main+0x1695C48 table, null where main+0xFA8C4C's mask 0x3E0ABAC1 has the bit)
// and the scroll classes of dat/mapScrollTex_00.dat.
constexpr const char* kSolidTex[31] = {nullptr, "m_tuti",  "m_rock",  "m_sand",  "m_grass", "m_tree",  nullptr,
                                       nullptr, "m_stone", nullptr,   "m_metal", nullptr,   nullptr,   nullptr,
                                       "m_carpet", nullptr, "m_wood", nullptr,   "m_naraku", nullptr,  "m_tree",
                                       "m_tree", "m_stone", "m_metal", "m_metal", nullptr,  nullptr,   nullptr,
                                       nullptr, nullptr,   "m_stone"};
struct ScrollTex {
    int cls;
    const char* tex;
};
constexpr ScrollTex kScrollTex[] = {{6, "m_lava"}, {7, "m_water"}, {11, "m_numa"}, {15, "m_quicksand"}, {17, "m_death"}};
// Map texture coordinates: one 32-texel tile per 1200 world units (main+0xFAA7B4's 1/1200
// texture matrix scale); the 128-row solid textures hold four 32-row height bands.
constexpr float kSolidTile = 1200.0f;

std::optional<Img> BtiDecode(const std::vector<uint8_t>& d) {
    if (d.size() < 0x20)
        return std::nullopt;
    const int fmt = d[0], w = Be16(&d[2]), h = Be16(&d[4]);
    const uint32_t off = Be32(&d[0x1C]);
    if (off >= d.size())
        return std::nullopt;
    return GxDecode(&d[off], d.size() - off, w, h, fmt);
}

// Bilinear, wrapping in x; y wraps inside rows [y0, y0 + rows).
void Sample(const Img& t, float u, float v, int y0, int rows, float* o) {
    u -= 0.5f;
    v -= 0.5f;
    const float fu = std::floor(u), fv = std::floor(v);
    const float ax = u - fu, ay = v - fv;
    auto wrap = [](int a, int n) { return ((a % n) + n) % n; };
    const int x0 = wrap(static_cast<int>(fu), t.w), x1 = (x0 + 1) % t.w;
    const int ya = y0 + wrap(static_cast<int>(fv), rows), yb = y0 + (ya - y0 + 1) % rows;
    const float* a = t.at(x0, ya);
    const float* b = t.at(x1, ya);
    const float* c = t.at(x0, yb);
    const float* e = t.at(x1, yb);
    for (int k = 0; k < 3; ++k)
        o[k] = (a[k] * (1 - ax) + b[k] * ax) * (1 - ay) + (c[k] * (1 - ax) + e[k] * ax) * ay;
}
} // namespace

struct Art::MapRes {
    Img solid[31];
    Img scroll[31];
    float scale[31] = {};
    float c0[31][3] = {}, c1[31][3] = {};
    struct Grad {
        std::string stage;
        float high, low;
        float rgb[3];
    };
    std::vector<Grad> grads;
    float edge[3] = {100 / 255.0f, 135 / 255.0f, 80 / 255.0f}; // mapEdge_near_00.bedge +0x2C
};

// Loads the Common map materials once: ObjectPack's node table, then only the prefix up to the
// end of oarc/Common.arc (about 8.5 MB of the 46 MB archive).
const Art::MapRes* Art::MapResources() {
    if (map_res_)
        return map_res_.get();
    if (map_res_failed_)
        return nullptr;
    map_res_failed_ = true;
    auto lz = read_("Object/NX/ObjectPack.arc.LZ");
    if (!lz) {
        error_ = "romfs read failed: ObjectPack";
        return nullptr;
    }
    auto head = Lz11(*lz, 0x20);
    if (!head || head->size() < 0x20 || Be32(head->data()) != 0x55AA382D) {
        error_ = "ObjectPack header";
        return nullptr;
    }
    const uint32_t data_off = Be32(&(*head)[12]);
    head = Lz11(*lz, data_off);
    if (!head)
        return nullptr;
    U8Entry common{};
    for (const auto& e : U8List(*head, false))
        if (e.path == "oarc/Common.arc")
            common = e;
    if (common.size == 0) {
        error_ = "ObjectPack: no Common.arc";
        return nullptr;
    }
    head.reset();
    auto pack = Lz11(*lz, static_cast<size_t>(common.off) + common.size);
    lz.reset();
    if (!pack || pack->size() < static_cast<size_t>(common.off) + common.size) {
        error_ = "ObjectPack: Common.arc truncated";
        return nullptr;
    }
    const std::vector<uint8_t> arc(pack->begin() + common.off, pack->begin() + common.off + common.size);
    pack.reset();
    auto res = std::make_shared<MapRes>();
    auto tex = [&](const char* name) -> std::optional<Img> {
        auto b = U8Get(arc, std::string("tex/") + name + ".bti");
        return b ? BtiDecode(*b) : std::nullopt;
    };
    for (int c = 0; c < 31; ++c)
        if (kSolidTex[c] != nullptr) {
            auto t = tex(kSolidTex[c]);
            if (!t || t->w < 1 || t->h < 4) {
                error_ = std::string("map texture ") + kSolidTex[c];
                return nullptr;
            }
            res->solid[c] = std::move(*t);
        }
    // dat/mapScrollTex_00.dat: 28-byte BE records {class u32, scale f32, f32, s16, u16, u16,
    // RGBA endpoint 0, RGBA endpoint 1, pad}
    auto scroll = U8Get(arc, "dat/mapScrollTex_00.dat");
    if (!scroll) {
        error_ = "mapScrollTex_00.dat";
        return nullptr;
    }
    for (size_t o = 0; o + 28 <= scroll->size(); o += 28) {
        const uint32_t cls = Be32(&(*scroll)[o]);
        const char* name = nullptr;
        for (const auto& s : kScrollTex)
            if (static_cast<uint32_t>(s.cls) == cls)
                name = s.tex;
        auto t = name ? tex(name) : std::nullopt;
        if (!t)
            continue;
        for (int c : {static_cast<int>(cls), cls == 15 ? 29 : -1}) { // class 29 takes the quicksand pass
            if (c < 0)
                continue;
            res->scroll[c] = *t;
            res->scale[c] = BeF32(&(*scroll)[o + 4]);
            for (int k = 0; k < 3; ++k) {
                res->c0[c][k] = (*scroll)[o + 18 + k] / 255.0f;
                res->c1[c][k] = (*scroll)[o + 22 + k] / 255.0f;
            }
        }
    }
    // dat/MapGradation.dat: "CSVD", u32 count, 44-byte records {name[32], high f32, low f32, RGBA}
    if (auto g = U8Get(arc, "dat/MapGradation.dat"); g && g->size() >= 8 && Be32(g->data()) == 0x43535644) {
        const uint32_t n = Be32(&(*g)[4]);
        for (uint32_t i = 0; i < n && 8 + (i + 1) * 44ull <= g->size(); ++i) {
            const uint8_t* r = &(*g)[8 + i * 44];
            MapRes::Grad gr;
            for (int k = 0; k < 32 && r[k] != 0; ++k)
                gr.stage.push_back(static_cast<char>(r[k]));
            gr.high = BeF32(r + 32);
            gr.low = BeF32(r + 36);
            for (int k = 0; k < 3; ++k)
                gr.rgb[k] = r[40 + k] / 255.0f;
            res->grads.push_back(gr);
        }
    }
    if (auto e = U8Get(arc, "dat/mapEdge_near_00.bedge"); e && e->size() >= 0x30)
        for (int k = 0; k < 3; ++k)
            res->edge[k] = (*e)[0x2C + k] / 255.0f;
    map_res_ = std::move(res);
    map_res_failed_ = false;
    return map_res_.get();
}

std::span<const AreaMap> AreaMaps() {
    return kAreas;
}

namespace {
constexpr GenStage kGenStages[] = {
#include "sshd_stages.inc"
};
} // namespace

std::span<const GenStage> GenStages() {
    return kGenStages;
}

const GenStage* FindGen(std::string_view stage) {
    for (const auto& g : kGenStages)
        if (stage == g.stage)
            return &g;
    return nullptr;
}

bool LayerActors(std::string_view stage) {
    for (const auto& l : kLayerDzbs)
        if (stage == l.stage)
            return true;
    return false;
}

GenFrame GenFrameOf(const GenStage& g) {
    GenFrame f;
    if (g.geo == 0)
        return f; // paper only: the frame is world units about the origin
    f.cx = (g.x0 + g.x1) / 2.0, f.cz = (g.z0 + g.z1) / 2.0;
    // the larger half-span plus 6 % margin; at least 1200 units so a closet is not blown up
    f.half = std::max(1200.0, std::max(g.x1 - g.x0, g.z1 - g.z0) / 2.0 * 1.06);
    f.s = kGenHalf / f.half;
    return f;
}

const AreaMap* FindArea(std::string_view stage) {
    for (const auto& a : kAreas)
        if (stage == a.stage)
            return &a;
    // dev.34: the generic frames, built once (map coordinates, +x right / +z down after the turn)
    static const std::vector<AreaMap> gen = [] {
        std::vector<AreaMap> v;
        for (const auto& g : kGenStages) {
            const GenFrame f = GenFrameOf(g);
            const float ppu = static_cast<float>((kGenPx - 2 * kMapPad) / (2.0 * f.half));
            v.push_back({g.stage, static_cast<float>(f.cx - f.half), static_cast<float>(f.cx + f.half),
                         static_cast<float>(f.cz - f.half), static_cast<float>(f.cz + f.half), ppu, false, true, true,
                         g.rot, true});
        }
        return v;
    }();
    for (const auto& a : gen)
        if (stage == a.stage)
            return &a;
    return nullptr;
}

// Keep parsed geometry, not the decompressed archives. Room order is preserved so equal-height
// triangles retain exactly the same rasterization order for every visited mask.
struct Art::StageTriangles {
    std::vector<std::pair<int, std::vector<Tri>>> rooms;
};

std::optional<Img> Art::PaperBase(int w, int h) {
    // paper: tr_mapBgAll_00 tiled (128 px) + the 16x16 map grid (64 px)
    auto paper = Layout("Map2D", "tr_mapBgAll_00");
    auto grid = Layout("Map2D", "tt_glid16x16_00");
    if (!paper || !grid)
        return std::nullopt;
    const Img pt = Resize(Tint(*paper, Hex("#CDB77E"), Hex("#EAD9A2")), 128, 128);
    const Img gt = Resize(AMask(*grid, Hex("#FFF6D8"), 0.4f), 64, 64);
    Img out(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* px = out.at(x, y);
            std::memcpy(px, pt.at(x % 128, y % 128), 16);
            Blend(px, gt.at(x % 64, y % 64));
        }
    return out;
}

void Art::CacheStage(const std::string& st, std::shared_ptr<StageTriangles> rooms) {
    triangles_.emplace_back(st, std::move(rooms));
    while (triangles_.size() > kStageCacheMax)
        triangles_.erase(triangles_.begin()); // least recently used first
}

size_t Art::CacheBytes() const {
    size_t n = 0;
    for (const auto& [p, b] : files_)
        n += b.size();
    for (const auto& [k, im] : textures_)
        n += im.p.size() * sizeof(float);
    for (const auto& [st, rooms] : triangles_)
        for (const auto& [nn, tris] : rooms->rooms)
            n += tris.size() * sizeof(Tri);
    if (map_res_) {
        for (int c = 0; c < 31; ++c)
            n += (map_res_->solid[c].p.size() + map_res_->scroll[c].p.size()) * sizeof(float);
    }
    return n;
}

// The stage's parsed map faces (cached, LRU): every room's map-tagged KCL / KCL2 faces plus the
// stage-level actor collision of kStageDzbs; Sky Keep (D003) = the map dummy's eight tiles.
std::shared_ptr<Art::StageTriangles> Art::StageTris(const std::string& st) {
    const bool trial = SilentRealm(st);
    std::shared_ptr<StageTriangles> cached;
    for (size_t i = 0; i < triangles_.size(); ++i)
        if (triangles_[i].first == st) {
            // most recently used last (dev.33: the cache keeps kStageCacheMax stages, LRU)
            cached = triangles_[i].second;
            std::rotate(triangles_.begin() + static_cast<std::ptrdiff_t>(i),
                        triangles_.begin() + static_cast<std::ptrdiff_t>(i) + 1, triangles_.end());
            break;
        }
    if (!cached && st == "D003") {
        // Sky Keep's board (dev.32): the native map draws each tile from the stage's map dummy
        // oarc/D3MapDummy.arc (dzb/<nn>.dzb + dat/<nn>.plc, tile room nn, in that room's own
        // coordinates: a 6000-unit rock square with the room's map faces), not from the rooms'
        // KCL. Every D003_<n> stage carries the same archive; D003_0's is read.
        auto lz = read_("Stage/D003_0/NX/D003_0_stg_l0.arc.LZ");
        auto arc = lz ? Lz11(*lz) : std::nullopt;
        lz.reset();
        auto dummy = arc ? U8Get(*arc, "oarc/D3MapDummy.arc") : std::nullopt;
        arc.reset();
        if (!dummy) {
            error_ = "Sky Keep map dummy (D003_0 oarc/D3MapDummy.arc)";
            return nullptr;
        }
        cached = std::make_shared<StageTriangles>();
        for (int n = 0; n < 8; ++n) {
            char name[16];
            std::snprintf(name, sizeof(name), "%02d", n);
            auto dzb = U8Get(*dummy, std::string("dzb/") + name + ".dzb");
            auto plc = U8Get(*dummy, std::string("dat/") + name + ".plc");
            std::vector<Tri> tile;
            if (dzb && plc)
                DzbTris(*dzb, PlcRows(*plc), false, tile);
            cached->rooms.emplace_back(n, std::move(tile));
        }
        CacheStage(st, cached);
    }
    if (!cached) {
        auto lz = read_("Stage/" + st + "/NX/" + st + "_stg_l0.arc.LZ");
        if (!lz) {
            error_ = "romfs read failed: stage " + st;
            return nullptr;
        }
        auto arc = Lz11(*lz);
        lz.reset();
        if (!arc) {
            error_ = "lz11 failed: " + st;
            return nullptr;
        }
        cached = std::make_shared<StageTriangles>();
        const auto entries = U8List(*arc);
        for (const auto& e : entries) {
            if (e.path.rfind("rarc/", 0) != 0 || !e.path.ends_with(".arc"))
                continue;
            const size_t r = e.path.rfind("_r");
            const int nn = r == std::string::npos ? -1 : std::atoi(e.path.c_str() + r + 2);
            const std::vector<uint8_t> room(arc->begin() + e.off, arc->begin() + e.off + e.size);
            const auto files = U8List(room);
            auto get = [&](const std::string& p) -> std::optional<std::vector<uint8_t>> {
                for (const auto& f : files)
                    if (f.path == p)
                        return std::vector<uint8_t>(room.begin() + f.off, room.begin() + f.off + f.size);
                return std::nullopt;
            };
            // a room with a past background (kcl/room_obj<n>k.kcl, registered by the time-shift
            // actor) registers its own backgrounds as present-only (dRoom_c::setupBg SetUnkBase(1));
            // the native draw keeps bg state {0, 1} (present) or {0, 2} (past variant)
            std::vector<std::string> past;
            for (const auto& f : files)
                if (f.path.rfind("kcl/room_obj", 0) == 0 && f.path.ends_with("k.kcl"))
                    past.push_back(f.path.substr(4, f.path.size() - 8)); // "room_obj0k"
            std::vector<Tri> tris;
            auto add = [&](const std::string& name, uint8_t bg) {
                auto kcl = get("kcl/" + name + ".kcl");
                auto plc = get("dat/" + name + ".plc");
                if (!kcl || !plc)
                    return;
                auto kcl2 = get("kcl/" + name + ".kcl2");
                KclTris(*kcl, kcl2 ? &*kcl2 : nullptr, PlcRows(*plc), nn, bg, trial, tris);
            };
            add("room", past.empty() ? 0 : 1);
            add("room_n", past.empty() ? 0 : 1);
            for (const auto& p : past)
                add(p, 2);
            cached->rooms.emplace_back(nn, std::move(tris));
        }
        for (const auto& s : kStageDzbs) {
            if (st != s.stage)
                continue;
            std::optional<std::vector<uint8_t>> sub;
            if (*s.arc != '\0')
                sub = U8Get(*arc, std::string("oarc/") + s.arc + ".arc");
            const std::vector<uint8_t>& src = sub ? *sub : *arc;
            if (*s.arc != '\0' && !sub)
                continue;
            auto dzb = U8Get(src, std::string("dzb/") + s.name + ".dzb");
            auto plc = U8Get(src, std::string("dat/") + s.name + ".plc");
            std::vector<Tri> actors;
            if (dzb && plc)
                DzbTris(*dzb, PlcRows(*plc), trial, actors, s.room);
            if (!actors.empty())
                cached->rooms.emplace_back(s.room, std::move(actors));
        }
        CacheStage(st, cached);
    }
    return cached;
}

// dev.34: the drawn faces' extent (x0, x1, z0, z1) of a stage: the faces whose class has a native
// map material (what Map draws), for the generic sub-stage frames (sshd_stages.inc).
std::optional<std::array<float, 4>> Art::MapBounds(std::string_view stage) {
    const auto rooms = StageTris(std::string(stage));
    const MapRes* res = rooms ? MapResources() : nullptr;
    if (res == nullptr)
        return std::nullopt;
    std::array<float, 4> b{INFINITY, -INFINITY, INFINITY, -INFINITY};
    for (const auto& [nn, tris] : rooms->rooms)
        for (const Tri& t : tris) {
            if ((res->solid[t.cls].empty() && res->scroll[t.cls].empty()) || t.bg == 2)
                continue; // not drawn, or a past-only background (the present frame)
            for (int k = 0; k < 3; ++k) {
                b[0] = std::min(b[0], t.x[k]), b[1] = std::max(b[1], t.x[k]);
                b[2] = std::min(b[2], t.z[k]), b[3] = std::max(b[3], t.z[k]);
            }
        }
    if (!(b[1] > b[0]) || !(b[3] > b[2]))
        return std::nullopt;
    return b;
}

// dev.34: a stage's bezier rail paths (rarc/<st>_r00.arc dat/room.bzs: PATH records, 12 bytes: s8,
// s8, u16 first point, u16 count, u8[6] with [4] == 2 = a bezier path over BPNT records, 40 bytes,
// the first vec3 the point), as (x, z) polylines.
std::vector<std::vector<std::array<float, 2>>> Art::RailPaths(const std::string& st) {
    std::vector<std::vector<std::array<float, 2>>> out;
    auto lz = read_("Stage/" + st + "/NX/" + st + "_stg_l0.arc.LZ");
    auto arc = lz ? Lz11(*lz) : std::nullopt;
    lz.reset();
    auto room = arc ? U8Get(*arc, "rarc/" + st + "_r00.arc") : std::nullopt;
    arc.reset();
    auto bzs = room ? U8Get(*room, "dat/room.bzs") : std::nullopt;
    if (!bzs || bzs->size() < 12)
        return out;
    const std::vector<uint8_t>& d = *bzs;
    const auto chunk = [&](const char* name, int rec) -> std::pair<size_t, int> {
        const int n = static_cast<int16_t>(Be16(d.data() + 4));
        const size_t base = Be32(d.data() + 8);
        for (int i = 0; i < n; ++i) {
            const size_t o = base + static_cast<size_t>(i) * 12;
            if (o + 12 > d.size())
                break;
            if (std::memcmp(d.data() + o, name, 4) != 0)
                continue;
            const int cnt = static_cast<int16_t>(Be16(d.data() + o + 4));
            const size_t at = o + Be32(d.data() + o + 8);
            if (cnt < 0 || at + static_cast<size_t>(cnt) * rec > d.size())
                return {0, 0};
            return {at, cnt};
        }
        return {0, 0};
    };
    const auto [pa, pn] = chunk("PATH", 12);
    const auto [ba, bn] = chunk("BPNT", 40);
    for (int i = 0; i < pn; ++i) {
        const uint8_t* r = d.data() + pa + static_cast<size_t>(i) * 12;
        if (r[10] != 2)
            continue; // not a bezier (BPNT) path
        const int first = Be16(r + 2), count = Be16(r + 4);
        if (first + count > bn)
            continue;
        std::vector<std::array<float, 2>> pl;
        for (int k = 0; k < count; ++k) {
            const uint8_t* b = d.data() + ba + static_cast<size_t>(first + k) * 40;
            pl.push_back({BeF32(b), BeF32(b + 8)});
        }
        out.push_back(std::move(pl));
    }
    return out;
}

std::optional<Img> Art::Map(std::string_view stage, uint32_t room_mask, int floor, bool paper_bg, uint32_t full_mask,
                            int state) {
    const AreaMap* area = FindArea(stage);
    if (area == nullptr) {
        error_ = "no map for stage " + std::string(stage);
        return std::nullopt;
    }
    const std::string st(stage);
    std::shared_ptr<StageTriangles> cached = StageTris(st);
    if (!cached)
        return std::nullopt;
    // dev.34: a Silent Realm blob sits on the page's own parchment (no paper rectangle around it)
    if (SilentRealm(st) && st != "S000") // S000's native trial map is the whole realm on paper
        paper_bg = false;
    // Sandship (dev.31): r00's 1F collision includes the roofs of the bow cabin (room 1) and the
    // stern cabins (rooms 13, 14), which the native map draws only once those rooms are visited
    // (native no-Map room-0 capture: hull and centre deck only). While such a room is not in the
    // mask, r00 pixels above the 1F / B1 split are not drawn over its cabin. The cabin boxes are
    // the rooms' own KCL extents (r01 x -5195..-3015 z +-1452, r14 x 1450..3585, r13 x
    // 3585..5031) widened to the roof r00 lays over them: the deck's width (|z| <= 2300) between
    // the centre deck (x -3000..1375) and the hull ring (the bow roof to x -5950, stepped inside
    // the narrowing hull). Measured on the stage's l0 KCL (dev.31).
    struct Claim {
        int room;
        float x0, z0, x1, z1;
    };
    static constexpr Claim kSandshipCabins[] = {
        {1, -4500, -2300, -3015, 2300}, {1, -5600, -2150, -4500, 2150}, {1, -5950, -1200, -5600, 1200}, {14, 1450, -2300, 3585, 2300},
        {13, 3585, -2300, 5800, 2300}};
    std::vector<Claim> claims;
    if (st == "D301" && room_mask != 0xFFFFFFFFu)
        for (const auto& c : kSandshipCabins)
            if ((room_mask >> c.room & 1u) == 0)
                claims.push_back(c);
    const bool past = (state & kMapPast) != 0 || st == "S300"; // MapRelated: S300 is always past
    // dev.35 (item 4, dev.35 analysis RE Q2): in a trial the per-class colour decode
    // main+0xFAAE44..0xFAAEE8 gives class 30 (target-through, main+0xFC3314) the unvisited-room
    // treatment (three channels zeroed, the fourth the 135/255 shade): those faces draw in the
    // native unvisited shade over the parchment, not in the stone texture
    const bool trial_dim = SilentRealm(st);
    // dev.33 (static map analysis C3): in F402's layer 7 the Siren2D actor
    // registers oarc/F406Col (only in F402_stg_l7.arc; identity transform, so the dzb is world
    // space) and the native map draws it over the pit; the height buffer keeps its higher faces.
    std::vector<Tri> extra;
    if (st == "F402" && (state & kMapF406) != 0) {
        std::shared_ptr<StageTriangles> col;
        for (const auto& [name, rooms] : triangles_)
            if (name == "F402#F406Col") col = rooms;
        if (!col) {
            auto lz = read_("Stage/F402/NX/F402_stg_l7.arc.LZ");
            auto arc = lz ? Lz11(*lz) : std::nullopt;
            lz.reset();
            auto oarc = arc ? U8Get(*arc, "oarc/F406Col.arc") : std::nullopt;
            arc.reset();
            auto dzb = oarc ? U8Get(*oarc, "dzb/F406Col.dzb") : std::nullopt;
            auto plc = oarc ? U8Get(*oarc, "dat/F406Col.plc") : std::nullopt;
            if (!dzb || !plc) {
                error_ = "F402 layer 7: oarc/F406Col";
                return std::nullopt;
            }
            col = std::make_shared<StageTriangles>();
            std::vector<Tri> faces;
            DzbTris(*dzb, PlcRows(*plc), false, faces);
            col->rooms.emplace_back(-1, std::move(faces));
            CacheStage("F402#F406Col", col);
        }
        extra = col->rooms[0].second;
    }
    // dev.35: the loaded layer's world-space water (kLayerDzbs), cached per stage and layer
    if (const int lv = state >> kMapLayerShift & 0x1F; lv > 0 && LayerActors(st)) {
        const int layer = lv - 1;
        const std::string ck = st + "#l" + std::to_string(layer);
        std::shared_ptr<StageTriangles> col;
        for (const auto& [name, rooms] : triangles_)
            if (name == ck) col = rooms;
        if (!col) {
            col = std::make_shared<StageTriangles>();
            std::vector<Tri> faces;
            std::optional<std::vector<uint8_t>> arc;
            for (const auto& l : kLayerDzbs) {
                if (st != l.stage || (l.layers >> layer & 1u) == 0)
                    continue;
                if (!arc) {
                    auto lz = read_("Stage/" + st + "/NX/" + st + "_stg_l" + std::to_string(layer) + ".arc.LZ");
                    arc = lz ? Lz11(*lz) : std::nullopt;
                    if (!arc)
                        break; // unreadable layer archive: the rooms alone
                }
                auto oarc = U8Get(*arc, std::string("oarc/") + l.arc + ".arc");
                auto dzb = oarc ? U8Get(*oarc, std::string("dzb/") + l.arc + ".dzb") : std::nullopt;
                auto plc = oarc ? U8Get(*oarc, std::string("dat/") + l.arc + ".plc") : std::nullopt;
                if (dzb && plc)
                    DzbTris(*dzb, PlcRows(*plc), SilentRealm(st), faces);
            }
            col->rooms.emplace_back(-1, std::move(faces));
            CacheStage(ck, col);
        }
        extra.insert(extra.end(), col->rooms[0].second.begin(), col->rooms[0].second.end());
    }
    std::vector<const Tri*> tris;
    std::vector<Tri> board; // Sky Keep: the tiles moved into their cells
    if (st == "D003") {
        for (int i = 0; i < 9; ++i) {
            const int n = board_[i];
            if (n < 0 || n > 7 || (board_cur_only_ && n != board_cur_))
                continue;
            const float dx = (i % 3 - 1) * kSkyKeepPitch, dz = (i / 3 - 1) * kSkyKeepPitch;
            for (const Tri& t0 : cached->rooms[static_cast<size_t>(n)].second) {
                Tri t = t0;
                for (int k = 0; k < 3; ++k)
                    t.x[k] += dx, t.z[k] += dz;
                board.push_back(t);
            }
        }
        for (const Tri& t : board)
            tris.push_back(&t);
        if (tris.empty())
            room_mask = 0; // an empty board is paper, not an error
    }
    for (const auto& [nn, room] : cached->rooms) {
        if (st == "D003")
            break;
        if (nn >= 0 && room_mask != 0xFFFFFFFFu && (nn > 31 || (room_mask >> nn & 1u) == 0))
            continue;
        for (const Tri& t : room)
            if (t.bg == 0 || t.bg == (past ? 2 : 1))
                tris.push_back(&t);
    }
    for (const Tri& t : extra)
        tris.push_back(&t);
    if (tris.empty() && room_mask == 0xFFFFFFFFu && !area->gen) { // a generic frame may be paper only
        error_ = "no collision triangles: " + st;
        return std::nullopt;
    }
    const MapRes* res = MapResources();
    if (res == nullptr)
        return std::nullopt;
    const float ppu = area->ppu;
    const int pad = kMapPad;
    // dev.34: a generic frame is always the kGenPx square the manifest's GEN area expects
    const int W = area->gen ? kGenPx : static_cast<int>((area->xmax - area->xmin) * ppu) + 2 * pad;
    const int H = area->gen ? kGenPx : static_cast<int>((area->zmax - area->zmin) * ppu) + 2 * pad;
    // the generic frame's MapArea yaw turn (quarter turns) of the world (x, z) into map coordinates
    const float rc = area->rot == 1 ? 0.0f : area->rot == 2 ? -1.0f : area->rot == 3 ? 0.0f : 1.0f;
    const float rs = area->rot == 1 ? 1.0f : area->rot == 3 ? -1.0f : 0.0f;
    // Bound the RGBA8 picture to the runtime callback cap (float storage is four times larger).
    if (static_cast<size_t>(W) * H * 4 > (16u << 20) || W > 4096 || H > 4096) {
        error_ = "map too large";
        return std::nullopt;
    }
    // the picture <-> world transform (sshd_art.h AreaMap)
    const auto wx_of = [&](float qx) { return area->flip_h ? (qx - pad) / ppu + area->xmin : area->xmax - (qx - pad) / ppu; };
    const auto wz_of = [&](float qy) { return area->flip_v ? (qy - pad) / ppu + area->zmin : area->zmax - (qy - pad) / ppu; };
    // Rasterize with a height buffer: at each pixel the highest map face wins (an orthographic top
    // view), keeping its class and whether its room is drawn in full colour.
    const size_t N = static_cast<size_t>(W) * H;
    std::vector<float> depth(N, -INFINITY), grad(N, 0.0f);
    std::vector<uint8_t> lab(N, 0), dim(N, 0);
    for (const Tri* tp : tris) {
        const Tri& t = *tp;
        if (res->solid[t.cls].empty() && res->scroll[t.cls].empty())
            continue; // no native material for the class (lotus, vine, life, grass 19): not drawn
        const float cy = (t.y[0] + t.y[1] + t.y[2]) / 3.0f;
        if (cy < yband_[0] || cy > yband_[1])
            continue; // dev.34: another native floor (gmapf)
        // Fire Sanctuary (dev.33, static map analysis C4, HD main+0xFC1E00..
        // 0xFC22C0): the native section draw keeps every room whole on its floor except room 6,
        // split at -5500 (the lava basin on B1), and room 9, split at -550; a face not entirely
        // below the split draws on 1F, one not entirely above it on B1. dev.32's centroid split at
        // -3000 dropped room 10's 1F abyss (the black pit at y -3230).
        if (st == "D201" && floor != 0) {
            const float lo = std::min({t.y[0], t.y[1], t.y[2]}), hi = std::max({t.y[0], t.y[1], t.y[2]});
            const float split = t.room == 6 ? -5500.0f : t.room == 9 ? -550.0f : NAN;
            if (floor == 1 && !std::isnan(split) && hi < split)
                continue;
            if (floor == 2 && (std::isnan(split) || lo > split))
                continue;
        }
        if (st == "D301" && floor >= 1 && floor <= 3) {
            // r00's deck/corridors and the connecting stairs span native floor tabs.
            // B3 keeps all of r12, including its rising exit stairs, like the native tab.
            constexpr float bounds[] = {INFINITY, -350.0f, -1150.0f, -2600.0f, -INFINITY};
            if (cy >= bounds[floor - 1] || cy < bounds[floor])
                continue;
        }
        float px[3], py[3];
        for (int k = 0; k < 3; ++k) {
            const float mx = t.x[k] * rc - t.z[k] * rs, mz = t.x[k] * rs + t.z[k] * rc;
            px[k] = (area->flip_h ? mx - area->xmin : area->xmax - mx) * ppu + pad;
            py[k] = (area->flip_v ? mz - area->zmin : area->zmax - mz) * ppu + pad;
        }
        const float area2 = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
        if (std::fabs(area2) < 1e-6f)
            continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({px[0], px[1], px[2]}))));
        const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({px[0], px[1], px[2]}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({py[0], py[1], py[2]}))));
        const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({py[0], py[1], py[2]}))));
        const bool clip = !claims.empty() && t.room == 0 && cy >= -350.0f;
        const bool full = t.room < 0 || t.room > 31 || (full_mask >> t.room & 1u) != 0;
        // the face's height change per picture pixel (its plane's gradient), for the edge pass
        const float gx = ((t.y[1] - t.y[0]) * (py[2] - py[0]) - (t.y[2] - t.y[0]) * (py[1] - py[0])) / area2;
        const float gy = ((t.y[2] - t.y[0]) * (px[1] - px[0]) - (t.y[1] - t.y[0]) * (px[2] - px[0])) / area2;
        const float g = std::sqrt(gx * gx + gy * gy);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float qx = x + 0.5f, qy = y + 0.5f;
                const float w0 = (px[1] - qx) * (py[2] - qy) - (px[2] - qx) * (py[1] - qy);
                const float w1 = (px[2] - qx) * (py[0] - qy) - (px[0] - qx) * (py[2] - qy);
                const float w2 = (px[0] - qx) * (py[1] - qy) - (px[1] - qx) * (py[0] - qy);
                const bool in = area2 > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
                if (!in)
                    continue;
                const float h = (w0 * t.y[0] + w1 * t.y[1] + w2 * t.y[2]) / area2;
                const size_t i = static_cast<size_t>(y) * W + x;
                if (h <= depth[i])
                    continue;
                if (clip) {
                    const float wx = wx_of(qx), wz = wz_of(qy);
                    bool hidden = false;
                    for (const auto& c : claims)
                        hidden = hidden || (wx >= c.x0 && wx <= c.x1 && wz >= c.z0 && wz <= c.z1);
                    if (hidden)
                        continue;
                }
                depth[i] = h;
                grad[i] = g;
                lab[i] = t.cls;
                dim[i] = full && !(trial_dim && t.cls == 30) ? 0 : 1;
            }
        }
    }
    {
        // Seal one-pixel cracks between faces (the native expands each map face by about 10
        // world units for the same reason): an empty pixel with 5+ drawn neighbours takes the
        // highest of them.
        // (dev.33) the filled pixels are collected first and written afterwards, so every test reads
        // the unsealed picture like the former full copies did, without four extra buffers
        std::vector<std::pair<uint32_t, uint32_t>> fill; // (pixel, the neighbour it copies)
        for (int y = 1; y + 1 < H; ++y)
            for (int x = 1; x + 1 < W; ++x) {
                const size_t i = static_cast<size_t>(y) * W + x;
                if (lab[i] != 0)
                    continue;
                int n = 0;
                size_t best = i;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const size_t j = i + static_cast<size_t>(dy * W + dx);
                        if (lab[j] != 0) {
                            ++n;
                            if (best == i || depth[j] > depth[best])
                                best = j;
                        }
                    }
                if (n >= 5)
                    fill.emplace_back(static_cast<uint32_t>(i), static_cast<uint32_t>(best));
            }
        // a filled pixel was empty, so it is never another fill's source: the order is free
        for (const auto& [i, b] : fill) {
            lab[i] = lab[b];
            dim[i] = dim[b];
            depth[i] = depth[b];
            grad[i] = grad[b];
        }
    }
    // one picture allocation (dev.33: the blank picture is no longer allocated under the paper)
    Img out;
    if (paper_bg) {
        auto paper = PaperBase(W, H);
        if (!paper) return std::nullopt;
        out = std::move(*paper);
    } else {
        out = Img(W, H);
    }
    // the stage's gradation record: height range of the four texture bands and the edge tint
    float high = 0.0f, low = -10000.0f; // native fallback (main+0xFAA8A4)
    float tint[3] = {res->edge[0], res->edge[1], res->edge[2]};
    const std::string grad_stage = st == "D003" ? "D003_0" : st;
    for (const auto& g : res->grads)
        if (g.stage == grad_stage) {
            high = g.high, low = g.low;
            std::memcpy(tint, g.rgb, sizeof(tint));
        }
    const float span = high - low;
    // Native-shade pass. Full colour: the class's solid texture (height band, world-xz texels) or
    // its scroll texture's intensity between the class's two endpoint colours. Unvisited rooms
    // of a dungeon with its Map: the native black at about 135/255 over the map parchment, which
    // composites to a dark olive (native D100 capture: paper x (0.53, 0.53, 0.45)).
    static constexpr float kDim[3] = {0.53f, 0.53f, 0.45f};
    static constexpr float kPaper[3] = {237 / 255.0f, 221 / 255.0f, 164 / 255.0f};
    // full-colour pixels are shaded straight into the picture; unvisited ones are shaded from
    // the parchment below them in the final pass
    // dev.35 (item 4): a Silent Realm trial recolours every solid-material face, not only the
    // target-through ones: the live S000 native map with and without the trial byte (same view)
    // maps every face colour (r, g, b) to (b, r, r) exactly (grass (113, 180, 103) -> (103, 113,
    // 113), sand (226, 210, 67) -> (67, 226, 226), wood (128, 80, 52) -> (52, 128, 128), stone
    // greys unchanged); the scroll materials keep theirs (live S200 trial: the lava stays orange,
    // which no (b, r, r) can give); the parchment and its edge halo keep their colours
    const bool realm = SilentRealm(st);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = static_cast<size_t>(y) * W + x;
            const int c = lab[i];
            if (c == 0 || dim[i])
                continue;
            float o[3];
            const float wx = wx_of(x + 0.5f), wz = wz_of(y + 0.5f);
            if (!res->solid[c].empty()) {
                const Img& t = res->solid[c];
                const int rows = t.h / 4;
                const float f = span != 0.0f ? (depth[i] - low) / span : 0.0f;
                const int band = std::clamp(static_cast<int>(std::floor(f * 4.0f)), 0, 3);
                Sample(t, wx / kSolidTile * t.w, wz / kSolidTile * rows, band * rows, rows, o);
                if (realm) {
                    const float r = o[0];
                    o[0] = o[2];
                    o[1] = r;
                    o[2] = r;
                }
            } else {
                const Img& t = res->scroll[c];
                const float s = res->scale[c] > 0.0f ? res->scale[c] : 2000.0f;
                float v[3];
                Sample(t, wx / s * t.w, wz / s * t.h, 0, t.h, v);
                for (int k = 0; k < 3; ++k)
                    o[k] = res->c0[c][k] + (res->c1[c][k] - res->c0[c][k]) * v[0];
            }
            Set(out.at(x, y), o[0], o[1], o[2], 1.0f);
        }
    if (keep_classes_)
        last_classes_ = lab;
    // dev.36 (dev.36 analysis): the native near edge (BEDG mapEdge_near, colour =
    // the stage's MapGradation RGBA, HD main+0xE26EC8) is an opaque line in that colour along every
    // height step of the map, not a darkening: live F001r (Knight Academy, edge RGBA 140 / 140 / 120)
    // draws (140, 140, 120) exactly around the building's silhouette, around each raised object
    // (beds, desks, the wood pieces along the round rooms' walls keep only a brown core) and along
    // every 30-unit step of the spiral stair (the treads stay white between grey step lines). The
    // native interior view fits the area to the screen and the line keeps its screen width: about
    // 1.1 % of the area's larger span in both live interiors (F001r: 4.5 px of the higher face + 1.5
    // px of the lower one at 0.102 px per unit in a 1280 px capture, about 45 + 15 of 5675 units;
    // F010r: 8 px at 0.079 px per unit in 1920 px, about 100 of 9233 units). The picture spans the
    // area too (a GEN frame its larger span x 1.06 over 1520 px), so the widths are fractions of the
    // picture's drawn span: about 11 + 4 px on a GEN picture. A face counts as lower
    // when it lies more than kStep below (more than its own slope explains: ramps draw no lines);
    // the empty paper is lower than every face. The native draws this line on interior maps (STIF
    // map context 3: F001r, F100_1 Inside the Great Tree, boss rooms ...) and on a dungeon's zoomed
    // map (live D100 near zoom: (110, 110, 100) = D100's edge RGBA, 5 px at about 0.087 px per unit,
    // about 57 world units); a dungeon's whole-area default view and the fields draw none (live
    // D200 / F200 / D100). The companion's dungeon picture serves its zoomed views (Ready minimap,
    // Map page near), so a dungeon draws the line at the native zoomed width in world units (42 +
    // 15) and keeps dev.32's wide halo for its whole-area view; fields keep dev.32's darkened rim.
    const GenStage* gs = area->gen ? FindGen(st) : nullptr;
    const bool dungeon = !area->gen && st.size() >= 4 && st[0] == 'D' && st[1] >= '0' && st[1] <= '9';
    const bool interior = (gs != nullptr && gs->ctx == 3) || st == "F010r"; // F010r: the Isle of Songs' interior
    const bool lines = interior || dungeon;
    constexpr float kStep = 20.0f; // world units: the 30-unit stair steps draw, floor seams do not
    const int halo_px = std::clamp(static_cast<int>(std::lround(400.0f * ppu)), 8, 48); // fields / dungeons
    const float span_px = static_cast<float>(std::max(W, H) - 2 * pad); // the picture's drawn span
    std::vector<float> edge(N, 0.0f);
    if (lines) {
        const int in_px = interior ? std::clamp(static_cast<int>(std::lround(0.0072f * span_px)), 2, 24)
                                   : std::clamp(static_cast<int>(std::lround(42.0f * ppu)), 1, 24);
        const int out_px = interior ? std::max(1, static_cast<int>(std::lround(in_px / 3.0f)))
                                    : std::max(1, static_cast<int>(std::lround(15.0f * ppu)));
        // inner part: the lowest drawn height within in_px (empty = -inf, unvisited never lower)
        std::vector<float> m(N);
        for (size_t i = 0; i < N; ++i)
            m[i] = lab[i] == 0 ? -INFINITY : (dim[i] ? INFINITY : depth[i]);
        Morph(m, W, H, in_px, false);
        for (size_t i = 0; i < N; ++i)
            if (lab[i] != 0 && !dim[i] && m[i] < depth[i] - kStep - 2.2f * grad[i] * static_cast<float>(in_px))
                edge[i] = 1.0f;
        // outer part: the highest full-colour height within out_px over a lower face or the paper
        for (size_t i = 0; i < N; ++i)
            m[i] = lab[i] != 0 && !dim[i] ? depth[i] : -INFINITY;
        Morph(m, W, H, out_px, true);
        for (size_t i = 0; i < N; ++i) {
            if (edge[i] != 0.0f || (lab[i] != 0 && dim[i]) || m[i] == -INFINITY)
                continue;
            if (lab[i] == 0 || m[i] > depth[i] + kStep + 2.2f * grad[i] * static_cast<float>(out_px))
                edge[i] = 1.0f;
        }
        // creases: the native line also runs where two faces meet at an angle with no step (live
        // F011r, the Lumpy Pumpkin: the entrance ramp and the faceted door landing are outlined
        // against the flat floor). Across in_px either way along four directions, the pixel's
        // neighbours leave a straight line by more than kCrease (a second difference; a step is the
        // rule above). A ramp of slope 0.4 meeting the floor draws a line about 60 world units wide
        // on F001r's scale; facets of natural terrain that bend less stay clean.
        constexpr float kCrease = 8.0f; // world units
        const int dirs[4][2] = {{in_px, 0}, {0, in_px}, {(in_px * 7 + 5) / 10, (in_px * 7 + 5) / 10},
                                {(in_px * 7 + 5) / 10, -(in_px * 7 + 5) / 10}};
        std::vector<uint8_t> crease(N, 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = static_cast<size_t>(y) * W + x;
                if (lab[i] == 0 || dim[i] || edge[i] != 0.0f)
                    continue;
                for (const auto& dv : dirs) {
                    const int ax = x + dv[0], ay = y + dv[1], bx = x - dv[0], by = y - dv[1];
                    if (ax < 0 || ay < 0 || bx < 0 || by < 0 || ax >= W || bx >= W || ay >= H || by >= H)
                        continue;
                    const size_t a = static_cast<size_t>(ay) * W + ax, b = static_cast<size_t>(by) * W + bx;
                    if (lab[a] == 0 || lab[b] == 0 || dim[a] || dim[b])
                        continue;
                    const float lim = kStep + 2.2f * grad[i] * static_cast<float>(in_px);
                    if (std::fabs(depth[a] - depth[i]) > lim || std::fabs(depth[b] - depth[i]) > lim)
                        continue; // a step within reach: the step rule draws it
                    if (std::fabs(depth[a] + depth[b] - 2.0f * depth[i]) > kCrease) {
                        crease[i] = 1;
                        break;
                    }
                }
            }
        // the crease line as wide as a step line (the native draws both alike): grown by out_px
        std::vector<float> cm(N);
        for (size_t i = 0; i < N; ++i)
            cm[i] = crease[i] ? 1.0f : 0.0f;
        crease = {};
        Morph(cm, W, H, out_px, true);
        for (size_t i = 0; i < N; ++i)
            if (cm[i] > 0.0f && lab[i] != 0 && !dim[i])
                edge[i] = 1.0f;
        // the line's own one-pixel antialiasing (the native line fades over about a pixel)
        BlurMask(edge, W, H, 1);
    } else {
        // dev.32 near edge (fields, dungeons): where a full-colour face stands above a face within
        // rim_px (about 100 world units) by more than 40 units plus its own slope, its rim darkens
        // (native D200: stone 195 -> 143); empty and unvisited pixels never count as lower
        const int r = std::clamp(static_cast<int>(std::lround(100.0f * ppu)), 2, 12);
        constexpr float kRimStep = 40.0f;
        std::vector<float> lo(N), t(N);
        for (size_t i = 0; i < N; ++i)
            lo[i] = lab[i] != 0 && !dim[i] ? depth[i] : INFINITY;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float m = INFINITY;
                for (int k = std::max(0, x - r); k <= std::min(W - 1, x + r); ++k)
                    m = std::min(m, lo[static_cast<size_t>(y) * W + k]);
                t[static_cast<size_t>(y) * W + x] = m;
            }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = static_cast<size_t>(y) * W + x;
                if (lab[i] == 0 || dim[i])
                    continue;
                float m = INFINITY;
                for (int k = std::max(0, y - r); k <= std::min(H - 1, y + r); ++k)
                    m = std::min(m, t[static_cast<size_t>(k) * W + x]);
                if (m < depth[i] - kRimStep - 2.2f * grad[i] * static_cast<float>(r))
                    for (int k = 0; k < 3; ++k)
                        out.p[i * 4 + k] *= 0.73f;
            }
    }
    // Unvisited rooms' soft silhouette: their shade fades in over a few pixels (native D100).
    std::vector<float> dimsoft;
    if (std::find(dim.begin(), dim.end(), uint8_t{1}) != dim.end()) {
        dimsoft.assign(N, 0.0f);
        for (size_t i = 0; i < N; ++i)
            dimsoft[i] = lab[i] != 0 && dim[i] ? 1.0f : 0.0f;
        BlurMask(dimsoft, W, H, 2);
    }
    for (size_t i = 0; i < N; ++i) {
        if (lab[i] == 0 || !dim[i])
            continue;
        float* px = &out.p[i * 4];
        const float a = std::max(dimsoft[i], 0.55f);
        const float* base = paper_bg ? px : kPaper;
        for (int k = 0; k < 3; ++k)
            px[k] = base[k] * (1.0f - a * (1.0f - kDim[k]));
        px[3] = 1.0f;
        edge[i] = 0.0f;
    }
    dimsoft = {};
    grad = {};
    depth = {};
    // dev.34 (item 8): the Shipyard's brown rail lines. They are not collision: the native map's
    // lines match the room's bezier rail paths (r00 room.bzs PATH records with unk3[4] == 2 over
    // their BPNT points, static map analysis C5, line for line with the gaps),
    // drawn under the platforms (only where no map face is) about 450 units wide in the rails'
    // brown, with the map's own far-edge halo around them.
    std::vector<uint8_t> rail;
    if (st == "F301_4") {
        rail.assign(N, 0);
        const auto paths = RailPaths(st);
        const float r_line = 225.0f * ppu;
        const Col brown = Hex("#8C4A2B");
        for (const auto& pl : paths)
            for (size_t k = 1; k < pl.size(); ++k) {
                const float ax = (pl[k - 1][0] - area->xmin) * ppu + pad, ay = (pl[k - 1][1] - area->zmin) * ppu + pad;
                const float bx = (pl[k][0] - area->xmin) * ppu + pad, by = (pl[k][1] - area->zmin) * ppu + pad;
                const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
                const int x0 = std::max(0, static_cast<int>(std::floor(std::min(ax, bx) - r_line - 1)));
                const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max(ax, bx) + r_line + 1)));
                const int y0 = std::max(0, static_cast<int>(std::floor(std::min(ay, by) - r_line - 1)));
                const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max(ay, by) + r_line + 1)));
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const size_t i = static_cast<size_t>(y) * W + x;
                        if (lab[i] != 0 || rail[i])
                            continue;
                        const float qx = x + 0.5f - ax, qy = y + 0.5f - ay;
                        const float t = l2 > 0 ? std::clamp((qx * dx + qy * dy) / l2, 0.0f, 1.0f) : 0.0f;
                        const float ex = qx - t * dx, ey = qy - t * dy;
                        if (ex * ex + ey * ey > r_line * r_line)
                            continue;
                        rail[i] = 1;
                        Set(&out.p[i * 4], brown.r, brown.g, brown.b, 1.0f);
                    }
            }
    }
    // Far edge (native mapEdge_far + the stage gradation tint): a soft halo outside the map's
    // silhouette, the parchment multiplied towards the stage's edge colour (native D200 / D100:
    // about paper x 0.65 at the boundary, fading out over halo_px). Two box blurs of the land
    // mask (half the halo each) give the falloff.
    {
        std::vector<float> halo(N, 0.0f);
        // (dev.36: the edge line's outer part counts as land: the halo starts beyond it)
        for (size_t i = 0; i < N; ++i)
            halo[i] = lab[i] != 0 || (!rail.empty() && rail[i]) || edge[i] >= 0.5f ? 1.0f : 0.0f;
        // dev.36: an interior's halo (live F001r) holds paper x about 0.7 for some 8 native px (75
        // of 5675 world units, 1.3 % of the span) past the edge line and fades over the next 4: a
        // plateau (the land grown by the plateau width) under a short blur; fields and dungeons keep
        // dev.32's falloff
        if (interior) {
            Morph(halo, W, H, std::max(1, static_cast<int>(std::lround(0.0129f * span_px))), true);
            const int fade = std::max(1, static_cast<int>(std::lround(0.0034f * span_px)));
            BoxBlur(halo, W, H, fade);
            BoxBlur(halo, W, H, fade);
        } else {
            BoxBlur(halo, W, H, std::max(1, halo_px / 2));
            BoxBlur(halo, W, H, std::max(1, halo_px / 2));
        }
        const float kHalo = interior ? 0.72f : 0.85f;
        for (size_t i = 0; i < N; ++i) {
            if (lab[i] != 0 || halo[i] <= 0.0f || (!rail.empty() && rail[i]))
                continue;
            float* px = &out.p[i * 4];
            const float f = (interior ? halo[i] : std::min(1.0f, halo[i] * 2.0f)) * kHalo;
            if (paper_bg) {
                for (int k = 0; k < 3; ++k)
                    px[k] *= 1.0f - f + f * tint[k];
            } else {
                const float s[4] = {tint[0] * kPaper[0], tint[1] * kPaper[1], tint[2] * kPaper[2], f * 0.6f};
                Blend(px, s);
            }
        }
    }
    if (keep_classes_) {
        last_edges_.assign(N, 0);
        for (size_t i = 0; i < N; ++i)
            last_edges_[i] = static_cast<uint8_t>(std::lround(std::clamp(edge[i], 0.0f, 1.0f) * 255.0f));
    }
    // dev.36: the near edge line over everything drawn so far (faces, paper, halo), opaque in the
    // stage's edge colour; the Shipyard's rails stay under no line (they are not collision)
    for (size_t i = 0; i < N; ++i) {
        const float a = edge[i];
        if (a <= 0.0f || (!rail.empty() && rail[i]))
            continue;
        float* px = &out.p[i * 4];
        for (int k = 0; k < 3; ++k)
            px[k] = px[k] * (1.0f - a) + tint[k] * a;
        px[3] = std::max(px[3], a);
    }
    return out;
}


// ---------------------------------------------------------------- Sky Keep (dev.31)
// The native Sky Keep map is a 3 x 3 grid of room tiles the player can slide; the cells come from
// the file (module: slot+0x539C). dev.32: each tile is the native map dummy of its room
// (Map "D003": oarc/D3MapDummy.arc), in the room's own coordinates moved into its cell, drawn in
// the native colours with the map's edge halo between the squares.
std::optional<Img> Art::SkyKeep(const int (&cells)[9], int cur, bool cur_only) {
    for (int i = 0; i < 9; ++i)
        board_[i] = cells[i];
    board_cur_ = cur;
    board_cur_only_ = cur_only;
    return Map("D003");
}

// ---------------------------------------------------------------- sky (flying page)

namespace {
// Sky island actors of stage F020 (dat/stage.bzs, layer 0 OBJ/SOBJ): actor name -> map art.
struct SkyIsland {
    const char* art;
    float x, z;
    int size; // picture px on the sky map
};
constexpr SkyIsland kSkyIslands[] = {
    {"th_skyloftMap_00", 0.0f, 0.0f, 240},            // City (Skyloft)
    {"th_pumpkinMap_00", 102667.0f, 47640.0f, 110},   // PumpBar (Lumpy Pumpkin)
    {"th_terryMap_00", 130349.0f, -108755.0f, 100},   // IslTery (Beedle's island)
    {"th_chikurinMap_00", 39922.0f, -110282.0f, 90},  // IslBamb (Bamboo Island)
    {"th_ruletouMap_00", -114296.0f, 49995.0f, 100},  // RouletC (Fun Fun Island)
    // MapMark actors without an island actor, matched to the native sky map by shape/position
    // (2026-11-07, MEDIUM): Bug Island and Isle of Songs.
    {"th_mushiMap_00", -170937.0f, -50851.0f, 100},
    {"th_utaMap_00", -140495.0f, -109061.0f, 110},
};
} // namespace

std::optional<Img> Art::SkyMap() {
    const AreaMap* area = FindArea("F020");
    auto cloud = Layout("Map2D", "tt_cloudC_00");
    if (area == nullptr || !cloud)
        return std::nullopt;
    const float ppu = area->ppu;
    const int W = static_cast<int>((area->xmax - area->xmin) * ppu) + 2 * kMapPad;
    const int H = static_cast<int>((area->zmax - area->zmin) * ppu) + 2 * kMapPad;
    auto paper = PaperBase(W, H);
    if (!paper) return std::nullopt;
    Img out = std::move(*paper);
    // soft cloud banks, a fixed scatter (decoration only, like the mockup's sky sample)
    const Img cl = AMask(*cloud, Hex("#FFFFFF"), 0.6f);
    for (int i = 0; i < 26; ++i) {
        const float fx = (HashNoise(i, 7) + 0.5f) * W, fy = (HashNoise(i, 13) + 0.5f) * H;
        const int sz = 260 + static_cast<int>((HashNoise(i, 29) + 0.5f) * 260);
        OverCentered(out, SizeW(cl, sz), fx, fy);
    }
    // The native sky map draws +x to the right and +z downward (checked 2026-11-07 against the
    // game's own map: Fun Fun Island lower left, Lumpy Pumpkin lower right of Skyloft).
    auto to_px = [&](float x, float z) {
        return std::pair<float, float>{(x - area->xmin) * ppu + kMapPad, (z - area->zmin) * ppu + kMapPad};
    };
    // The native Sky map's small islands (owner capture 07, all 35 match these layer-0 F020 actors):
    // RockSky rocks = a brown core in a soft brown-grey halo, IslTrea treasure islands = a green
    // core with a light-green rim, IslTreB = a tiny brown dot. The native draws them from their
    // geometry (no layout sprite); sizes and colours are measured on capture 07 (core about
    // 2000 units, halo 4500 units in radius).
    struct SmallIsland {
        int kind; // 0 RockSky, 1 IslTrea, 2 IslTreB
        float x, z;
    };
    static constexpr SmallIsland kSmall[] = {
        {1, 28601.0f, 83382.0f}, {1, -26036.0f, 68742.0f}, {1, -138290.0f, 31413.0f}, {1, 158283.0f, -80069.0f},
        {1, -110653.0f, -42674.0f}, {1, -72044.0f, 43573.0f}, {0, 67217.0f, -39877.0f}, {0, 75362.0f, -46346.0f},
        {0, 81926.0f, -39325.0f}, {0, 75509.0f, -28703.0f}, {1, 102975.0f, -121240.0f}, {0, 86931.0f, -29312.0f},
        {0, 93628.0f, -46508.0f}, {0, 64882.0f, 91484.0f}, {0, 152716.0f, -12306.0f}, {0, 88776.0f, -114022.0f},
        {0, 133827.0f, -89781.0f}, {0, -165992.0f, 108444.0f}, {0, -133156.0f, 74883.0f}, {0, -166711.0f, 57791.0f},
        {0, 7970.0f, 97012.0f}, {0, -118503.0f, 100497.0f}, {0, 134031.0f, 13247.0f}, {0, -23502.0f, 44887.0f},
        {0, -2187.0f, -70742.0f}, {2, 135829.0f, 76112.0f}, {1, -114386.0f, 49984.0f}, {2, -145711.0f, 44303.0f},
        {0, 60039.0f, 21646.0f}, {0, 59502.0f, 69750.0f}, {0, -19101.0f, -27738.0f}, {0, -24471.0f, 19239.0f},
        {0, 19017.0f, -36870.0f}, {0, -42635.0f, 110331.0f}, {0, -75973.0f, 29465.0f},
    };
    const float rc = 2000.0f * ppu, rh = 4500.0f * ppu;
    for (const auto& s : kSmall) {
        const auto [cx, cy] = to_px(s.x, s.z);
        const float outer = s.kind == 2 ? rc * 0.6f : rh;
        for (int y = static_cast<int>(cy - outer) - 1; y <= static_cast<int>(cy + outer) + 1; ++y)
            for (int x = static_cast<int>(cx - outer) - 1; x <= static_cast<int>(cx + outer) + 1; ++x) {
                if (x < 0 || y < 0 || x >= W || y >= H)
                    continue;
                const float r = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                float px[4];
                if (s.kind == 2) {
                    if (r > rc * 0.6f)
                        continue;
                    Set(px, 101 / 255.f, 64 / 255.f, 47 / 255.f, Clamp01(rc * 0.6f - r));
                } else if (r <= rc) {
                    const float inner = SmoothStep(rc * 0.25f, rc * 0.7f, r);
                    if (s.kind == 0)
                        Set(px, (95 + 25 * inner) / 255.f, (64 + 18 * inner) / 255.f, (50 + 8 * inner) / 255.f, 1.0f);
                    else
                        Set(px, (100 + 75 * inner) / 255.f, (160 + 38 * inner) / 255.f, (95 + 33 * inner) / 255.f, 1.0f);
                } else {
                    const float a = 0.75f * (1.0f - SmoothStep(rc, s.kind == 0 ? rh : rh * 0.7f, r));
                    if (s.kind == 0)
                        Set(px, 150 / 255.f, 128 / 255.f, 92 / 255.f, a);
                    else
                        Set(px, 172 / 255.f, 190 / 255.f, 125 / 255.f, a);
                }
                Blend(out.at(x, y), px);
            }
    }
    for (const auto& isl : kSkyIslands) {
        auto art = Layout("Map2D", isl.art);
        if (!art)
            continue;
        const auto [px, py] = to_px(isl.x, isl.z);
        OverCentered(out, Fit(*art, isl.size), px, py);
    }
    return out;
}

std::optional<Img> Art::LoftMarker() {
    auto bird = Layout("DoButton", "th_skyBird_02");
    auto link = Layout("Map2D", "tr_linkPIcon_00");
    if (!bird || !link)
        return std::nullopt;
    Img o(120, 120);
    OverCentered(o, Fit(*bird, 116), 60, 70);
    OverCentered(o, Resize(*link, 64, 64), 60, 44);
    return o;
}

std::optional<Img> Art::AltGauge(int h) {
    auto sun = Layout("DoButton", "th_skySun_08");
    auto rays = Layout("DoButton", "th_skySunS_09");
    auto cloud = Layout("DoButton", "th_skyCloud_02");
    if (!sun || !rays || !cloud || h < 200 || h > 1024)
        return std::nullopt;
    Img o(120, h);
    // light column (the native gauge's sky beam), brighter toward the sun
    for (int y = 50; y < h - 30; ++y) {
        const float t = static_cast<float>(y - 50) / static_cast<float>(h - 80);
        for (int x = 46; x < 74; ++x) {
            const float edge = 1.0f - std::fabs((x + 0.5f) - 60.0f) / 14.0f;
            const float s[4] = {0.62f + 0.3f * (1 - t), 0.86f + 0.1f * (1 - t), 1.0f, Clamp01(edge * 1.6f) * 0.85f};
            Blend(o.at(x, y), s);
        }
    }
    // the sun textures are left halves, mirrored by the native layout
    auto whole = [](const Img& half) {
        Img w(half.w * 2, half.h);
        Over(w, half, 0, 0);
        Over(w, FlipH(half), half.w, 0);
        return w;
    };
    OverCentered(o, Tint(Resize(whole(*rays), 100, 70), Hex("#FFB000"), Hex("#FFF4A0")), 60, 46);
    OverCentered(o, Tint(Resize(whole(*sun), 64, 44), Hex("#F0A000"), Hex("#FFF8C0")), 60, 46);
    OverCentered(o, SizeW(*cloud, 100), 60, h - 26);
    return o;
}

std::optional<Img> Art::Ribbon(int w, int h) {
    auto bg = Layout("Map2D", "tr_areaCaptionBg_00");
    if (!bg || w < 16 || h < 16 || w > 1024 || h > 512)
        return std::nullopt;
    return Tint(Resize(*bg, w, h), Hex("#2E5E1E"), Hex("#A8D878"));
}

// Stamina wheel (DoButton tr_ganaba*: rim, backing, green circle), the green part kept for the
// last `pct` percent of the turn: the native gauge empties clockwise from 12 o'clock.
std::optional<Img> Art::StaminaWheel(int pct, int s) {
    auto bg = Layout("DoButton", "tr_ganabaFuchiBg_00");
    auto circ = Layout("DoButton", "tr_ganabaCircle_00");
    auto rim = Layout("DoButton", "tr_ganabaFuchi_00");
    if (!bg || !circ || !rim || pct < 0 || pct > 100 || s < 32 || s > 512)
        return std::nullopt;
    Img o(s, s);
    Over(o, Tint(Resize(*bg, s, s), Hex("#101808"), Hex("#3A4A2A")), 0, 0);
    const int cs = s * 86 / 100;
    Img c = Tint(Resize(*circ, cs, cs), Hex("#2E6E1E"), Hex("#B8F07A"));
    constexpr float kPi = 3.14159265f;
    for (int y = 0; y < cs; ++y)
        for (int x = 0; x < cs; ++x) {
            float a = std::atan2(x + 0.5f - cs / 2.0f, -(y + 0.5f - cs / 2.0f)); // 0 at top, clockwise
            if (a < 0)
                a += 2 * kPi;
            if (a / (2 * kPi) * 100.0f < 100.0f - static_cast<float>(pct))
                c.at(x, y)[3] = 0;
        }
    Over(o, c, (s - cs) / 2, (s - cs) / 2);
    Over(o, Tint(Resize(*rim, s, s), Hex("#3A2A10"), Hex("#F0E6C0")), 0, 0);
    return o;
}

namespace {
// TEV colour mapping of a pane material (per channel: black + (white - black) * texel), alpha scaled.
Img Tev(const Img& s, Col b, Col w, float a = 1.0f) {
    Img o = s;
    for (size_t i = 0; i < o.p.size(); i += 4) {
        o.p[i] = b.r + (w.r - b.r) * o.p[i];
        o.p[i + 1] = b.g + (w.g - b.g) * o.p[i + 1];
        o.p[i + 2] = b.b + (w.b - b.b) * o.p[i + 2];
        o.p[i + 3] *= a;
    }
    return o;
}
// A drop shadow: the picture's alpha in black at `a`, under the picture, offset (dx, dy).
Img WithShadow(const Img& s, int dx, int dy, float a, int pad) {
    Img o(s.w + 2 * pad, s.h + 2 * pad);
    Over(o, Tev(s, Hex("#000000"), Hex("#000000"), a), pad + dx, pad + dy);
    Over(o, s, pad, pad);
    return o;
}
} // namespace

// Native D-pad cluster (Layout DoButton remoConBtn_03 / nunBtn_03 / remoConBtn_05 / commonIcon_04).
// Material colours from the BRLYTs: cross pieces and the eye black 0 -> white (255,251,227); Fi's
// shadow black, its outline white with the cyan / magenta corner gradient, its attention halos
// (200,255,255) -> (111,226,255) / (147,233,255); the Loftwing call icon's shadow black at 64; the
// selected target's socket tr_itemBace_03 (135,89,161) -> (216,145,240) at 180 over a
// tr_circle64_00 disc in black at 64, the target's shadow black at 64 two units left-down.
std::optional<Img> Art::Dpad(std::string_view piece, int arg) {
    const Col cream_b = Hex("#000000"), cream_w = Hex("#FFFBE3");
    auto tex = [&](const char* m) { return Layout("DoButton", m); };
    if (piece == "base" || piece == "up" || piece == "right" || piece == "down" || piece == "left") {
        const char* m = piece == "base" ? "tr_crossBtnBasic_00" : piece == "up" ? "tr_crossBtnBasicT_03"
                        : piece == "right" ? "tr_crossBtnBasicR_01" : piece == "down" ? "tr_crossBtnBasicD_00"
                                                                          : "tr_crossBtnBasicL_00";
        auto t = tex(m);
        if (!t)
            return std::nullopt;
        return Tev(*t, cream_b, cream_w);
    }
    if (piece == "eye") {
        auto t = tex("tr_cameraMark_00");
        if (!t)
            return std::nullopt;
        return Tev(*t, cream_b, cream_w);
    }
    if (piece == "fi")
        return tex("tr_phiIcon_00");
    if (piece == "fis0" || piece == "fis1" || piece == "fih0" || piece == "fih1") {
        auto t = tex("tr_phiIconS_00");
        if (!t)
            return std::nullopt;
        Img s = Resize(*t, 88, 88);
        if (piece == "fis0")
            return Tev(s, Hex("#000000"), Hex("#000000"));
        if (piece == "fih0")
            return Tev(s, Hex("#C8FFFF"), Hex("#6FE2FF"));
        if (piece == "fih1")
            return Tev(s, Hex("#C8FFFF"), Hex("#93E9FF"));
        // P_plusIconS_01: white material, vertex colours TL cyan, TR magenta, BL magenta, BR cyan
        for (int y = 0; y < s.h; ++y)
            for (int x = 0; x < s.w; ++x) {
                const float u = (x + 0.5f) / s.w, v = (y + 0.5f) / s.h;
                const float m = u * (1 - v) + (1 - u) * v; // weight of the magenta corners
                float* px = s.at(x, y);
                px[0] *= m;
                px[1] *= 1 - m;
            }
        return s;
    }
    if (piece == "bird") {
        // th_skyBird_03 is the left half; the 64 x 40 pane mirrors it (texture wrap)
        auto t = tex("th_skyBird_03");
        if (!t)
            return std::nullopt;
        Img full(t->w * 2, t->h);
        Over(full, *t, 0, 0);
        Over(full, FlipH(*t), t->w, 0);
        return WithShadow(Resize(full, 128, 80), -4, 4, 64.0f / 255.0f, 6);
    }
    if (piece == "tsocket") {
        auto base = tex("tr_itemBace_03");
        auto disc = tex("tr_circle64_00");
        if (!base || !disc)
            return std::nullopt;
        Img o(144, 144);
        Over(o, Tev(Resize(Quad(*base), 144, 144), Hex("#8759A1"), Hex("#D891F0"), 180.0f / 255.0f), 0, 0);
        OverCentered(o, Tev(Resize(*disc, 116, 116), Hex("#000000"), Hex("#000000"), 64.0f / 255.0f), 72, 72);
        return o;
    }
    if (piece == "target") {
        const char* art = DowsingTargetArt(arg);
        if (art == nullptr) {
            error_ = "no dowsing target art";
            return std::nullopt;
        }
        auto t = tex(art);
        if (!t)
            return std::nullopt;
        Img sq(160, 160);
        OverCentered(sq, Fit(*t, 160), 80, 80);
        return WithShadow(sq, -4, 4, 64.0f / 255.0f, 6);
    }
    error_ = "unknown dpad piece";
    return std::nullopt;
}

// Stamina ring segment, centred on 12 o'clock: the glass profile of tr_ganabaCircle_00 across the
// stroke, in the same colours as the Ready stamina wheel (green) or towards the gauge's exhausted
// red (170,10,40). The picture is (2 ceil(rout sin h) + 4) x (ceil(rout - rin cos h) + 4) with the
// ring centre (w / 2, rout + 2) above it; the manifest uses the same box and pivot.
std::optional<Img> Art::StaminaSegment(int rin, int rout, int half_mdeg, int variant) {
    auto circ = Layout("DoButton", "tr_ganabaCircle_00");
    if (!circ || rin < 8 || rout <= rin || rout > 1024 || half_mdeg <= 0 || half_mdeg > 30000 || variant < 0 ||
        variant > 1)
        return std::nullopt;
    constexpr double kPi = 3.14159265358979323846;
    const double h = half_mdeg / 1000.0 * kPi / 180.0;
    const int w = 2 * static_cast<int>(std::ceil(rout * std::sin(h))) + 4;
    const int ht = static_cast<int>(std::ceil(rout - rin * std::cos(h))) + 4;
    Img o(w, ht);
    const Col b = variant == 0 ? Hex("#2E6E1E") : Hex("#4A0814");
    const Col wh = variant == 0 ? Hex("#B8F07A") : Hex("#F0607A");
    const float cx = w / 2.0f, cy = rout + 2.0f;
    for (int y = 0; y < ht; ++y)
        for (int x = 0; x < w; ++x) {
            const float dx = x + 0.5f - cx, dy = cy - (y + 0.5f);
            const float r = std::sqrt(dx * dx + dy * dy);
            const float a = std::atan2(dx, dy);
            const float cov = Clamp01(r - rin + 0.5f) * Clamp01(rout - r + 0.5f) *
                              Clamp01((static_cast<float>(h) - std::fabs(a)) * r + 0.5f);
            if (cov <= 0.0f)
                continue;
            const float t = Clamp01((r - rin) / static_cast<float>(rout - rin));
            const int sx = std::clamp(static_cast<int>(circ->w * (0.5f + 0.5f * (0.30f + 0.65f * t))), 0, circ->w - 1);
            const float lum = circ->at(sx, circ->h / 2)[0];
            Set(o.at(x, y), b.r + (wh.r - b.r) * lum, b.g + (wh.g - b.g) * lum, b.b + (wh.b - b.b) * lum, cov);
        }
    return o;
}

// The ring's backing: the gauge's dark green (P_ganbariBg_05, (0,57,0) at 220) between rims in the
// gauge rim's gold (tr_ganabaFuchi_00 material (153,132,84) -> (255,231,135)). (2 rout + 8) square.
std::optional<Img> Art::StaminaTrack(int rin, int rout, int variant) {
    if (rin < 8 || rout <= rin || rout > 1024 || variant < 0 || variant > 1)
        return std::nullopt;
    const int s = 2 * rout + 8;
    Img o(s, s);
    // dev.34 (item 5): variant 1 = the exhausted red (live: the native land wheel turns red at 0
    // stamina, when the ring has no segment left to colour)
    const Col dark = Hex(variant == 0 ? "#003900" : "#5A0A14"), gold = Hex("#D9C27A");
    const float c = s / 2.0f;
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            const float dx = x + 0.5f - c, dy = y + 0.5f - c;
            const float r = std::sqrt(dx * dx + dy * dy);
            if (r < rin - 4.0f || r > rout + 4.0f)
                continue;
            const float band = Clamp01(r - (rin - 3.0f) + 0.5f) * Clamp01((rout + 3.0f) - r + 0.5f);
            const float inner = Clamp01(r - (rin - 1.0f) + 0.5f) * Clamp01((rout + 1.0f) - r + 0.5f);
            // gold rims, the dark centre at the gauge's 220 alpha
            Set(o.at(x, y), gold.r + (dark.r - gold.r) * inner, gold.g + (dark.g - gold.g) * inner,
                gold.b + (dark.b - gold.b) * inner, band * (1.0f - inner) + inner * (220.0f / 255.0f));
        }
    return o;
}

namespace {
// One MSBT (MsgStdBn) file's label -> text pairs: LBL1 hash buckets of (length, name, index),
// TXT2 UTF-16 strings; control tags (0x0E group type size payload) are dropped, ASCII kept
// (line breaks included), the typographic apostrophe folded to '.
std::optional<std::vector<std::pair<std::string, std::string>>> ParseMsbt(const std::vector<uint8_t>& d) {
    if (d.size() < 0x20 || std::memcmp(d.data(), "MsgStdBn", 8) != 0)
        return std::nullopt;
    const bool be = d[8] == 0xFE;
    auto r16 = [&](size_t o) { return be ? Be16(&d[o]) : static_cast<uint16_t>(d[o] | d[o + 1] << 8); };
    auto r32 = [&](size_t o) {
        return be ? Be32(&d[o]) : static_cast<uint32_t>(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | static_cast<uint32_t>(d[o + 3]) << 24);
    };
    const int nsec = r16(0xE);
    size_t pos = 0x20;
    std::vector<std::pair<std::string, uint32_t>> labels;
    std::vector<std::string> texts;
    for (int i = 0; i < nsec && pos + 16 <= d.size(); ++i) {
        const uint32_t sz = r32(pos + 4);
        const size_t s0 = pos + 16;
        if (sz > d.size() - s0)
            return std::nullopt;
        const size_t end = s0 + sz;
        if (std::memcmp(&d[pos], "LBL1", 4) == 0) {
            if (sz < 4)
                return std::nullopt;
            const uint32_t n = r32(s0);
            if (n > (sz - 4) / 8)
                return std::nullopt;
            for (size_t k = 0; k < n; ++k) {
                const uint32_t cnt = r32(s0 + 4 + 8 * k), off = r32(s0 + 8 + 8 * k);
                if (off > sz)
                    return std::nullopt;
                size_t q = s0 + off;
                for (uint32_t c = 0; c < cnt; ++c) {
                    if (q >= end)
                        return std::nullopt;
                    const uint8_t l = d[q];
                    if (end - q < static_cast<size_t>(l) + 5)
                        return std::nullopt;
                    labels.emplace_back(std::string(reinterpret_cast<const char*>(&d[q + 1]), l), r32(q + 1 + l));
                    q += 5 + l;
                }
            }
        } else if (std::memcmp(&d[pos], "TXT2", 4) == 0) {
            if (sz < 4)
                return std::nullopt;
            const uint32_t n = r32(s0);
            if (n > (sz - 4) / 4)
                return std::nullopt;
            for (size_t k = 0; k < n; ++k) {
                const uint32_t o = r32(s0 + 4 + 4 * k), e = k + 1 < n ? r32(s0 + 8 + 4 * k) : sz;
                if (o < 4 + static_cast<size_t>(n) * 4 || o > e || e > sz)
                    return std::nullopt;
                std::string t;
                for (size_t q = s0 + o; q + 1 < s0 + e; q += 2) {
                    const uint16_t ch = r16(q);
                    if (ch == 0)
                        break;
                    if (ch == 0x0E) { // control tag: skip group, type, size and payload
                        if (q + 7 >= s0 + e)
                            break;
                        const size_t payload = r16(q + 6);
                        if (payload > s0 + e - q - 8)
                            return std::nullopt;
                        q += 6 + payload;
                        continue;
                    }
                    if (ch < 0x80)
                        t.push_back(static_cast<char>(ch));
                    else if (ch == 0x2019)
                        t.push_back('\'');
                }
                texts.push_back(t);
            }
        }
        pos = (s0 + sz + 15) & ~static_cast<size_t>(15);
    }
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [name, idx] : labels)
        if (idx < texts.size())
            out.emplace_back(name, texts[idx]);
    return out;
}
} // namespace

// Item names: NAME_ITEM_%03d labels of 003-ItemGet.msbt in US/Object/en_US/0-Common.arc (the
// game builds the same keys from the item id; item analysis).
std::optional<std::string> Art::ItemName(int id) {
    if (id < 0 || id > 999)
        return std::nullopt;
    if (item_names_.empty()) {
        const auto* arc = File("US/Object/en_US/0-Common.arc");
        if (arc == nullptr)
            return std::nullopt;
        auto b = U8Get(*arc, "0-Common/003-ItemGet.msbt");
        const auto pairs = b ? ParseMsbt(*b) : std::nullopt;
        if (!pairs) {
            error_ = "item msbt missing";
            return std::nullopt;
        }
        for (const auto& [name, text] : *pairs)
            if (name.rfind("NAME_ITEM_", 0) == 0)
                item_names_.emplace_back(std::atoi(name.c_str() + 10), text);
        if (item_names_.empty()) {
            error_ = "no item names";
            return std::nullopt;
        }
    }
    for (const auto& [k, v] : item_names_)
        if (k == id && !v.empty())
            return v;
    return std::nullopt;
}

// Map names: 0-Common/007-MapText.msbt (MAP_00 Skyloft .. MAP_40, MAP_UNKNOWN "?"), with the
// game's own line breaks (MAP_27 "Pumpkin \nLanding").
std::optional<std::string> Art::MapText(std::string_view label) {
    if (map_text_.empty()) {
        const auto* arc = File("US/Object/en_US/0-Common.arc");
        if (arc == nullptr)
            return std::nullopt;
        auto b = U8Get(*arc, "0-Common/007-MapText.msbt");
        const auto pairs = b ? ParseMsbt(*b) : std::nullopt;
        if (!pairs || pairs->empty()) {
            error_ = "map msbt missing";
            return std::nullopt;
        }
        map_text_ = *pairs;
    }
    for (const auto& [k, v] : map_text_)
        if (k == label && !v.empty())
            return v;
    error_ = "no map text " + std::string(label);
    return std::nullopt;
}

// ---------------------------------------------------------------- map markers (dev.27)

namespace {
constexpr float kMarkUnit = 4.0f; // marker art px per layout unit (the manifest sizes the canvas)
// `up` turned `deg` degrees clockwise about its centre (bilinear, premultiplied).
Img Turn(const Img& up, int deg) {
    if (deg % 360 == 0)
        return up;
    Img o(up.w, up.h);
    const float a = deg * 3.14159265f / 180.0f, ca = std::cos(a), sa = std::sin(a);
    const float cx = (up.w - 1) / 2.0f, cy = (up.h - 1) / 2.0f;
    for (int y = 0; y < up.h; ++y)
        for (int x = 0; x < up.w; ++x) {
            const float dx = x - cx, dy = y - cy;
            const float sx = ca * dx + sa * dy + cx, sy = -sa * dx + ca * dy + cy;
            const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            const float fx = sx - x0, fy = sy - y0;
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const int xx = x0 + (k & 1), yy = y0 + (k >> 1);
                if (xx < 0 || yy < 0 || xx >= up.w || yy >= up.h)
                    continue;
                const float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                const float* p = up.at(xx, yy);
                for (int ch = 0; ch < 3; ++ch)
                    acc[ch] += p[ch] * p[3] * wgt;
                acc[3] += p[3] * wgt;
            }
            if (acc[3] > 0)
                Set(o.at(x, y), acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], acc[3]);
        }
    return o;
}
// A pane: texture resized to (w, h) layout units, centred at layout offset (lx, ly) (y up) from
// the canvas centre.
void PaneOn(Img& c, const Img& tex, float w, float h, float lx = 0, float ly = 0, float unit = kMarkUnit) {
    const int pw = std::max(1, static_cast<int>(std::lround(w * unit)));
    const int ph = std::max(1, static_cast<int>(std::lround(h * unit)));
    OverCentered(c, Resize(tex, pw, ph), c.w / 2.0f + lx * unit, c.h / 2.0f - ly * unit);
}
} // namespace

// MapSky N_goddessC_00: the shadow P_goddessCS_00 (black, (-2, -2)) under P_goddessC_00 (black
// (90, 0, 138) -> white): the purple tag with its light wing emblem; 28-unit canvas.
std::optional<Img> Art::GoddessTag() {
    auto t = Layout("MapSky", "nh_goddessTBox_00");
    if (!t)
        return std::nullopt;
    Img c(static_cast<int>(28 * kMarkUnit), static_cast<int>(28 * kMarkUnit));
    PaneOn(c, TevA(*t, "#000000", "#000000", 0.35f), 23, 23, -2, -2);
    PaneOn(c, TevA(*t, "#5A008A", "#FFFFFF"), 23, 23);
    return c;
}

// N_saveIcon_00's small statue: nh_save_02 (its black rim is the purple ring) in the
// saveObjectColor frame's material, over the tr_circle32S_00 shadow; 12 units on the field
// (MapField), 10 on the Sky (MapSky); 16-unit canvas.
std::optional<Img> Art::Statue(int type, bool sky) {
    auto dot = Layout("MapField", "nh_save_02");
    auto sh = Layout("MapField", "tr_circle32S_00");
    if (!dot || !sh || type < 0 || type > 2)
        return std::nullopt;
    static constexpr const char* kWhite[3] = {"#96FF96", "#FFA100", "#F5FF5F"}; // A green, B orange, C yellow
    const float d = sky ? 10.0f : 12.0f;
    Img c(static_cast<int>(16 * kMarkUnit), static_cast<int>(16 * kMarkUnit));
    PaneOn(c, TevA(*sh, "#000000", "#000000", 0.35f), d + 2, d + 2, -1, -1);
    PaneOn(c, TevA(*dot, "#734082", kWhite[type]), d, d);
    return c;
}

// N_door_000: P_door_00 (nh_mapDoor_00, black (80, 80, 80) -> white (255, 255, 0): the yellow bar
// with its dark border), 40 x 20 at the group's 0.65 scale, turned `deg` clockwise; 30-unit canvas.
std::optional<Img> Art::Door(int deg) {
    auto t = Layout("MapField", "nh_mapDoor_00");
    if (!t || deg < 0 || deg >= 360)
        return std::nullopt;
    Img c(static_cast<int>(30 * kMarkUnit), static_cast<int>(30 * kMarkUnit));
    PaneOn(c, TevA(*t, "#505050", "#FFFF00"), 40 * 0.65f, 20 * 0.65f);
    return Turn(c, deg);
}

// ---------------------------------------------------------------- dungeon markers (dev.30)
// MapField mapIcon_00 panes with their BRLYT materials, group scales and offsets (the
// dungeon marker analysis), upright, centred on the object.

// N_doorKey_00: P_doorKey_00 (nh_mapDoorKey_00, black (96, 96, 28) -> white (255, 249, 213)),
// 28 units at the N_alpha_13 scale 0.85, drawn over the door bar; 30-unit canvas.
std::optional<Img> Art::DoorKey() {
    auto t = Layout("MapField", "nh_mapDoorKey_00");
    if (!t)
        return std::nullopt;
    Img c(static_cast<int>(30 * kMarkUnit), static_cast<int>(30 * kMarkUnit));
    PaneOn(c, TevA(*t, "#60601C", "#FFF9D5"), 28 * 0.85f, 28 * 0.85f);
    return c;
}

// N_mark_00 (the red X): P_mark_00 (tr_mark_00, black (221, 72, 9) -> white (255, 171, 127)),
// 17 units at the N_mark_000 scale 0.9; 20-unit canvas.
// dev.33 (Skyloft analysis C2a): the Sky class's red common target, MapSky
// N_commonIcon_000 at its far-view group scale 0.55: the shadow P_commonIconS_00 (nh_tamanori_00,
// black at alpha 64, (-1, -1)) under P_commonIcon_00 (black (255, 0, 0) -> white (255, 200, 100)),
// 14 units (15 with the shadow at (-1.5, -1.5) for the Academy / Bazaar marks, big = 1); 20-unit canvas.
std::optional<Img> Art::Target(int big) {
    auto t = Layout("MapSky", "nh_tamanori_00");
    if (!t || big < 0 || big > 1)
        return std::nullopt;
    constexpr float g = 0.55f;
    const float d = (big ? 15.0f : 14.0f) * g, o = (big ? -1.5f : -1.0f) * g;
    Img c(static_cast<int>(20 * kMarkUnit), static_cast<int>(20 * kMarkUnit));
    PaneOn(c, TevA(*t, "#000000", "#000000", 64.0f / 255.0f), d, d, o, o);
    PaneOn(c, TevA(*t, "#FF0000", "#FFC864"), d, d);
    return c;
}

// dev.33 (Skyloft analysis C2c): a Bug Medal bug location,
// N_mushi_01 (MapSky and MapField alike) at its far-view group scale 0.55: the glow P_mushi_00
// (tr_circleM_00, black (237, 250, 69) -> white (255, 249, 191), 22 units) under the core P_mushiS_00
// (nh_save_02, (255, 248, 164) -> (36, 212, 159), 14 units); 20-unit canvas.
std::optional<Img> Art::Bug() {
    auto glow = Layout("MapField", "tr_circleM_00");
    auto core = Layout("MapField", "nh_save_02");
    if (!glow || !core)
        return std::nullopt;
    constexpr float g = 0.55f;
    Img c(static_cast<int>(20 * kMarkUnit), static_cast<int>(20 * kMarkUnit));
    PaneOn(c, TevA(*glow, "#EDFA45", "#FFF9BF"), 22 * g, 22 * g);
    PaneOn(c, TevA(*core, "#FFF8A4", "#24D49F"), 14 * g, 14 * g);
    return c;
}

// dev.34 (item 9): a Triforce piece (MapSky mapIcon_01 N_triforce_00: P_triforceS_00 nh_tryForceMini_00
// in gold (166,143,0) -> (255,246,151) over P_triforce_00, the same texture in black, 2 units down-left;
// both 22 units), 26-unit canvas.
std::optional<Img> Art::Triforce() {
    auto t = Layout("MapSky", "nh_tryForceMini_00");
    if (!t)
        return std::nullopt;
    Img c(static_cast<int>(26 * kMarkUnit), static_cast<int>(26 * kMarkUnit));
    PaneOn(c, TevA(*t, "#000000", "#000000"), 22, 22, -2, -2);
    PaneOn(c, TevA(*t, "#A68F00", "#FFF697"), 22, 22);
    return c;
}

std::optional<Img> Art::Mark() {
    auto t = Layout("MapField", "tr_mark_00");
    if (!t)
        return std::nullopt;
    Img c(static_cast<int>(20 * kMarkUnit), static_cast<int>(20 * kMarkUnit));
    PaneOn(c, TevA(*t, "#DD4809", "#FFAB7F"), 17 * 0.9f, 17 * 0.9f);
    return c;
}

// N_treasure_000 (a closed dungeon chest): P_treasure_00 (nh_reasure_00 mirrored into its 23-unit
// quad, untinted) over its black shadow P_treasureS_00 at alpha 64, (-1.5, -2) units, drawn like
// the opened-chest key "openchest": 23 units = 80 px on a 100 px square.
std::optional<Img> Art::ClosedChest() {
    auto t = Layout("MapField", "nh_reasure_00");
    if (!t)
        return std::nullopt;
    Img full(t->w * 2, t->h);
    Over(full, *t, 0, 0);
    Over(full, FlipH(*t), t->w, 0);
    const Img q = Resize(full, 80, 80);
    Img o(100, 100);
    Over(o, TevA(q, "#000000", "#000000", 64.0f / 255.0f), 10 - 5, 10 + 7);
    Over(o, q, 10, 10);
    return o;
}

// The boss door's objective. kind 0: N_zelda_00 (P_loop_01 tr_env_06 swirl (255, 255, 255) ->
// (255, 238, 0) at alpha 128, 34 units; P_zelda_01 tr_circle64S_00 (211, 211, 61) -> white at
// alpha 128, 30 units; P_zelda_00 tr_zeldaIcon_01, 28 units, 2 units down). kind 1 / 2 / 3: the
// sacred flame N_swordFire_01 (Farore green) / _00 (Din red) / _02 (Nayru blue): five glow panes
// (tr_circle32S_00 shaped, tr_env_17 grain) in their material colours and alphas at the N_alpha
// scale 0.6. 64-unit canvas (the flame rises 28 units above the door).
std::optional<Img> Art::Objective(int kind) {
    if (kind < 0 || kind > 3)
        return std::nullopt;
    Img c(static_cast<int>(64 * kMarkUnit), static_cast<int>(64 * kMarkUnit));
    if (kind == 0) {
        auto loop = Layout("MapField", "tr_env_06");
        auto circ = Layout("MapField", "tr_circle64S_00");
        auto face = Layout("MapField", "tr_zeldaIcon_01");
        if (!loop || !circ || !face)
            return std::nullopt;
        PaneOn(c, TevA(*loop, "#FFFFFF", "#FFEE00", 128.0f / 255.0f), 34, 34);
        PaneOn(c, TevA(*circ, "#D3D33D", "#FFFFFF", 128.0f / 255.0f), 30, 30);
        PaneOn(c, *face, 28, 28, 0, -2);
        return c;
    }
    auto glow = Layout("MapField", "tr_circle32S_00");
    auto grain = Layout("MapField", "tr_env_17");
    if (!glow || !grain)
        return std::nullopt;
    struct P {
        float x, y, sx, sy, a;
    };
    static constexpr P kP[5] = {{-2, 14, 0.90f, 1.20f, 1.0f}, {-2, 14, 0.90f, 1.20f, 200 / 255.0f},
                                {-2, 7, 0.75f, 0.85f, 200 / 255.0f}, {-2, -0.1f, 0.55f, 0.55f, 200 / 255.0f},
                                {-2, -0.1f, 0.45f, 0.45f, 200 / 255.0f}};
    // material black / white per pane: _01 green, _00 red, _02 blue (pane order as kP)
    static constexpr const char* kCol[3][5][2] = {
        {{"#00DD33", "#00E086"}, {"#3EE820", "#3CEA1D"}, {"#9CF660", "#A0FB63"}, {"#FFFD9D", "#FFFAB8"}, {"#FFFFFF", "#FFFFFF"}},
        {{"#FF0000", "#FF0000"}, {"#FF3333", "#FF2F2F"}, {"#FF7373", "#FF788A"}, {"#FFABE1", "#FFABE7"}, {"#FFFFFF", "#FFFFFF"}},
        {{"#1C9CFF", "#2784FF"}, {"#1C9CFF", "#2784FF"}, {"#74C2FF", "#77BFFF"}, {"#AAFFFF", "#AAFFFF"}, {"#FFFFFF", "#FFFFFF"}}};
    constexpr float g = 0.6f;
    for (int i = 0; i < 5; ++i) {
        const P& p = kP[i];
        const int w = std::max(1, static_cast<int>(std::lround(56 * p.sx * g * kMarkUnit)));
        const int h = std::max(1, static_cast<int>(std::lround(56 * p.sy * g * kMarkUnit)));
        // the glow, its alpha thinned by the grain texture (the flame's flicker, one frame) and
        // narrowed towards the top into a flame (the loops' scrolling UVs, frozen)
        Img q = Resize(*glow, w, h);
        const Img n = Resize(*grain, w, h);
        for (int y = 0; y < h; ++y) {
            const float t = static_cast<float>(y) / std::max(1, h - 1);
            const float half = 0.5f * w * (0.3f + 0.7f * std::sqrt(t));
            for (int x = 0; x < w; ++x) {
                const float edge = std::clamp((half - std::fabs(x + 0.5f - w / 2.0f)) / (0.18f * w), 0.0f, 1.0f);
                q.at(x, y)[3] *= (0.75f + 0.25f * n.at(x, y)[0]) * edge;
            }
        }
        OverCentered(c, TevA(q, kCol[kind - 1][i][0], kCol[kind - 1][i][1], p.a), c.w / 2.0f + p.x * g * kMarkUnit,
                     c.h / 2.0f - p.y * g * kMarkUnit);
    }
    return c;
}

// ---------------------------------------------------------------- NPC balloons and landmarks (dev.31)
// Pane data: the NPC map mark analysis of the portrait layouts and art (BRLYT positions in layout units, y up, from the marker root; rotations
// counter-clockwise; alphas; materials) and MapField / MapSky mapIcon BRLYT dumps.
namespace {
// `tex` resized to w x h layout units, turned `rot` degrees counter-clockwise (BRLYT z rotation),
// centred at (lx, ly) layout units from the canvas centre (y up).
void PaneRot(Img& c, const Img& tex, float w, float h, float lx, float ly, float rot, float unit = kMarkUnit) {
    const int pw = std::max(1, static_cast<int>(std::lround(w * unit)));
    const int ph = std::max(1, static_cast<int>(std::lround(h * unit)));
    const int s = static_cast<int>(std::ceil(std::hypot(pw, ph))) + 2;
    Img sq(s, s);
    OverCentered(sq, Resize(tex, pw, ph), s / 2.0f, s / 2.0f);
    OverCentered(c, Turn(sq, static_cast<int>(std::lround(-rot))), c.w / 2.0f + lx * unit, c.h / 2.0f - ly * unit);
}
// A half texture drawn with u 0..2 and a mirror wrap: the half and its mirror side by side.
Img Mirrored(const Img& half) {
    Img full(half.w * 2, half.h);
    Over(full, half, 0, 0);
    Over(full, FlipH(half), half.w, 0);
    return full;
}
struct BalloonPanes {
    int icon;
    bool sky;          // MapSky (else MapField)
    const char* tex;   // the portrait
    bool mirror;       // portrait u 0..2 (half texture)
    float rot;         // the frame's rotation (+45 left of the mark, -45 right)
    float sx, sy, fx, fy, fs; // frame shadow / frame centres, frame size
    float ix, iy, jx, jy, iw, ih; // portrait shadow / portrait centres, portrait size
};
constexpr BalloonPanes kBalloons[] = {
    {17, true, "nh_roulette_00", false, -45, 11.58f, 12.63f, 12.63f, 15.79f, 40, 13.68f, 16.84f, 15.0f, 18.0f, 22, 22},
    {19, true, "nh_takegiri_00", false, 45, -13.68f, 11.93f, -13.68f, 16.14f, 40, -16.84f, 18.25f, -16.0f, 19.0f, 20, 22},
    {20, true, "nh_kobunB_00", false, 45, -10.53f, 8.42f, -11.58f, 11.58f, 39, -14.74f, 12.63f, -13.68f, 13.68f, 22, 22},
    {23, true, "nh_sebasun_00", false, -45, 11.40f, 11.93f, 12.46f, 16.14f, 40, 14.56f, 17.19f, 15.0f, 18.0f, 20, 22},
    {18, false, "th_konsai_00", false, -45, 11.17f, 12.34f, 13.17f, 14.34f, 40, 15.17f, 15.34f, 16.17f, 16.34f, 23, 20},
    {21, false, "nh_rairyu_00", true, -45, 10.50f, 12.34f, 12.50f, 14.34f, 40, 13.50f, 16.34f, 14.50f, 17.34f, 23, 28},
    {22, false, "nh_goron_00", true, -45, 10.75f, 11.47f, 12.90f, 13.62f, 40, 15.05f, 15.77f, 15.05f, 16.85f, 23, 23},
};
} // namespace

std::optional<Img> Art::Balloon(int icon) {
    const BalloonPanes* b = nullptr;
    for (const auto& k : kBalloons)
        if (k.icon == icon)
            b = &k;
    if (b == nullptr)
        return std::nullopt;
    const std::string_view arc = b->sky ? "MapSky" : "MapField";
    auto frame = Layout(arc, "nh_mapBaloon_01");
    auto shadow = Layout(arc, "nh_mapBaloonSdw_01");
    auto face = Layout(arc, b->tex);
    if (!frame || !shadow || !face)
        return std::nullopt;
    Img c(static_cast<int>(80 * kMarkUnit), static_cast<int>(80 * kMarkUnit));
    // P_bgS (black at alpha 128), P_bg (black (140, 140, 140) -> white), P_iconS (black at 64), P_icon
    PaneRot(c, TevA(Mirrored(*shadow), "#000000", "#000000", 128.0f / 255.0f), b->fs, b->fs, b->sx, b->sy, b->rot);
    PaneRot(c, TevA(Mirrored(*frame), "#8C8C8C", "#FFFFFF"), b->fs, b->fs, b->fx, b->fy, b->rot);
    const Img f = b->mirror ? Mirrored(*face) : *face;
    PaneOn(c, TevA(f, "#000000", "#000000", 64.0f / 255.0f), b->iw, b->ih, b->ix, b->iy);
    PaneOn(c, f, b->iw, b->ih, b->jx, b->jy);
    return c;
}

std::optional<Img> Art::Cave() {
    auto t = Layout("MapField", "nh_cave_00");
    auto s = Layout("MapField", "nh_caveSdw_00");
    if (!t || !s)
        return std::nullopt;
    Img c(static_cast<int>(20 * kMarkUnit), static_cast<int>(20 * kMarkUnit));
    constexpr float g = 0.66f, a = 238.0f / 255.0f;
    PaneOn(c, TevA(Mirrored(*s), "#1F1F1F", "#000000", a * 100.0f / 255.0f), 25 * g, 25 * g, -3 * g, -1.5f * g);
    PaneOn(c, TevA(Mirrored(*t), "#1F1F1F", "#FFFFFF", a), 25 * g, 25 * g);
    return c;
}

std::optional<Img> Art::BeedleShop(int face) {
    auto house = Layout("MapSky", "nh_terry_01");
    auto hs = Layout("MapSky", "nh_terry_sdw_01");
    auto terry = Layout("MapSky", "nh_terry_00");
    if (!house || !hs || !terry || face < 0 || face > 1)
        return std::nullopt;
    Img c(static_cast<int>(48 * kMarkUnit), static_cast<int>(48 * kMarkUnit));
    constexpr float g = 0.9f;
    PaneOn(c, TevA(*hs, "#000000", "#000000", 128.0f / 255.0f), 30 * g, 30 * g, -2.2f * g, 1.5f * g);
    PaneOn(c, *house, 30 * g, 30 * g, 0, 4 * g);
    if (face) {
        PaneOn(c, TevA(*terry, "#000000", "#000000", 64.0f / 255.0f), 22 * g, 22 * g, -1 * g, -1 * g);
        PaneOn(c, *terry, 22 * g, 22 * g);
    }
    return c;
}

std::optional<Img> Art::Kikwi(int elder) {
    if (elder < 0 || elder > 1)
        return std::nullopt;
    auto t = Layout("MapField", elder ? "nh_moriJin_01" : "nh_moriJin_00");
    if (!t)
        return std::nullopt;
    Img c(static_cast<int>(40 * kMarkUnit), static_cast<int>(40 * kMarkUnit));
    constexpr float g = 0.9f;
    const float w = (elder ? 25 * 0.8f : 20) * g, h = (elder ? 34 * 0.8f : 20) * g;
    PaneOn(c, TevA(*t, "#000000", "#000000", 64.0f / 255.0f), w, h, -1 * g, -1 * g);
    PaneOn(c, *t, w, h);
    return c;
}

std::optional<Img> Art::StartArrow(int deg) {
    auto t = Layout("MapField", "tr_start_01");
    if (!t || deg < 0 || deg >= 360)
        return std::nullopt;
    Img c(static_cast<int>(40 * kMarkUnit), static_cast<int>(40 * kMarkUnit));
    PaneOn(c, TevA(*t, "#008000", "#008000", 160.0f / 255.0f), 11 * 1.3f, 14 * 1.2f, 0, 8);
    PaneOn(c, TevA(*t, "#5FD500", "#B6FF00", 180.0f / 255.0f), 11, 14, 0, 8);
    return Turn(c, deg);
}

namespace {
// dev.46: the native clamped two-pane button (System2D messageBtn_00 P_choicesBg_00 / _01, Map2D
// mapFloorBtn_00): the left pane maps the texture over `cap` px (UV 0..k on a clamped texture, so
// its last column runs across the middle), the right pane is the mirrored texture over `cap` px.
// The ends never scale with the width.
Img Clamp3(const Img& tex, int cap, int h, int w) {
    cap = std::max(1, std::min(cap, w));
    Img o(w, h);
    const Img c = Resize(tex, cap, h);
    for (int y = 0; y < h; ++y)
        for (int x = cap; x < w - cap; ++x)
            std::memcpy(o.at(x, y), c.at(cap - 1, y), 16);
    Over(o, c, 0, 0);
    Over(o, FlipH(c), w - cap, 0);
    return o;
}
// dev.46: P_lighLoop: tr_env_04 (the soft band, stretched like the pane) times tr_env_20 (the grain)
// tiled at its own texel size, never stretched; colour black -> white by the grain.
Img LightLoop(const Img& env04, const Img& env20, int w, int h, Col b, Col wc, float a) {
    const Img band = Resize(env04, w, h);
    Img o(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float g = env20.at(x % env20.w, y % env20.h)[0];
            Set(o.at(x, y), b.r + (wc.r - b.r) * g, b.g + (wc.g - b.g) * g, b.b + (wc.b - b.b) * g,
                band.at(x, y)[0] * g * a);
        }
    return o;
}
} // namespace

// A floor tab (Map2D mapFloorBtn_00 N_btn_00) for native floor `floor` (0 = 1F, -1 = B1, 1 = 2F):
// the button body P_choicesBg_00 / _01 (tr_buttonLBg_00, left 80 units and its mirror 40 units
// right of +20, (56, 43, 20) -> (58, 47, 30) at alpha 180), its rim P_choicesBg_02 / _03
// (tr_button76L_00, (102, 71, 45) -> (222, 198, 134)), the kazari scrolls P_kaariL_00 / R_00
// (tr_kazari_00, (40, 25, 10) at alpha 80, 48 units at (-24, 3) and turned at (22, -3)), the
// label T_text_00 (white over T_textS_00). sel = N_on_00 shown: body (171, 135, 90) at 200, rim
// (255, 245, 165), the light P_lighLoop_00 (tr_env_04, (255, 255, 210) at 32). 130 x 76 units
// at 2 px per unit.
std::optional<Img> Art::FloorTab(int floor, int sel) {
    auto body = Layout("Map2D", "tr_buttonLBg_00");
    auto rim = Layout("Map2D", "tr_button76L_00");
    auto scroll = Layout("Map2D", "tr_kazari_00");
    auto light = Layout("Map2D", "tr_env_04");
    if (!body || !rim || !scroll || !light || floor < -3 || floor > 4 || sel < 0 || sel > 1)
        return std::nullopt;
    constexpr float U = 2.0f;
    Img c(static_cast<int>(130 * U), static_cast<int>(76 * U));
    const float cx = c.w / 2.0f, cy = c.h / 2.0f;
    // a 120-unit button from -60 to +60 units: the 80-unit left pane maps the texture over its
    // first 40 units (UV 0..2, clamped: the texture's last column runs on to +20) and its 40-unit
    // mirror. dev.46: the texture is no longer resized over the whole 80 units (that stretched the
    // end 2x); the same clamped 3-slice as the choice planks (Art::Choice).
    const int bw = static_cast<int>(120 * U), bh = static_cast<int>(68 * U), cap = static_cast<int>(40 * U);
    const auto button = [&](const Img& tex, std::string_view b, std::string_view w, float a, float dx = 0, float dy = 0) {
        Over(c, Clamp3(TevA(tex, b, w, a), cap, bh, bw), static_cast<int>(cx - 60 * U + dx), static_cast<int>(cy - 34 * U + dy));
    };
    // the drop shadow (W_shadow_00, 64 alpha, (-4, -5) units)
    button(*body, "#000000", "#000000", 0.25f, -4 * U, 5 * U);
    button(*body, "#382B14", "#3A2F1E", 180.0f / 255.0f);
    button(*rim, "#66472D", "#DEC686", 1.0f);
    PaneOn(c, TevA(*scroll, "#28190A", "#28190A", 80.0f / 255.0f), 48, 48, -24, 3, U);
    PaneOn(c, Rot180(TevA(*scroll, "#28190A", "#28190A", 80.0f / 255.0f)), 48, 48, 22, -3, U);
    if (sel) {
        button(*body, "#AB875A", "#AB875A", 200.0f / 255.0f);
        button(*rim, "#FFF5A5", "#FFF5A5", 1.0f);
        PaneOn(c, TevA(*light, "#FFFFD2", "#FFFFD2", 32.0f / 255.0f), 99, 48, 0, 0, U);
    }
    const auto lab = FloorLabel(floor);
    auto txt = Text("floor", 46, std::string_view(lab.data()));
    if (!txt)
        return std::nullopt;
    OverCentered(c, *txt, cx, cy);
    return c;
}

// dev.46 (owner pick, alt A): the choice plank, the button the game uses whenever it asks the
// player to choose (bird-statue Cancel / Save, Fi's choices, the pause "Options" button, the map
// floor buttons): body P_choicesBg (tr_buttonLBg_00, pause N_options_00 P_choicesBg_18
// (56, 43, 20) -> (58, 47, 30) at alpha 180), the light loop P_lighLoop (tr_env_04 x tr_env_20,
// (255, 255, 199) -> (255, 251, 210) at 32), the rim (tr_button76L_00, (102, 71, 45) ->
// (222, 198, 134)), the scrolls P_kaariL / R (tr_kazari_00, (40, 25, 10) at 80, 48 units, 40 units
// in from each end, the right one turned). Clamped 3-slice with 40-unit ends at h / 85 px per unit
// (messageBtn_00: 68 units x 1.25). Selected = N_on (body (171, 135, 90) at 40 % of 180, rim
// (255, 245, 165) at 128, light loop at 60); pressed = the lit rim + N_flash (tr_btnlightL_00
// (255, 250, 200)); disabled = body at 110, a grey rim (the game has no disabled choice plank).
// The textures are the MenuOptions copies (byte-identical to System2D's; the Options row is in the
// same archive). The picture has no drop shadow; its visible body is rows 7..113 of a 120 px plank.
std::optional<Img> Art::Choice(int w, int h, int state, bool opaque) {
    if (w < 16 || h < 16 || w > 2048 || h > 512 || state < 0 || state > 3)
        return std::nullopt;
    auto body = Layout("MenuOptions", "tr_buttonLBg_00");
    auto rim = Layout("MenuOptions", "tr_button76L_00");
    auto scroll = Layout("MenuOptions", "tr_kazari_00");
    auto env04 = Layout("MenuOptions", "tr_env_04");
    auto env20 = Layout("MenuOptions", "tr_env_20");
    auto flash = Layout("MenuOptions", "tr_btnlightL_00");
    if (!body || !rim || !scroll || !env04 || !env20 || !flash)
        return std::nullopt;
    const float u = h / 85.0f;
    const int cap = std::min(w / 2, static_cast<int>(std::lround(40 * u)));
    const bool dis = state == 3;
    Img o(w, h);
    Over(o, Clamp3(TevA(*body, "#382B14", "#3A2F1E", dis ? 110.0f / 255.0f : opaque ? 1.0f : 180.0f / 255.0f), cap, h, w), 0, 0);
    if (!dis) {
        const int lw = w - cap, lh = static_cast<int>(h * 0.70f);
        Over(o, LightLoop(*env04, *env20, lw, lh, Hex("#FFFFC7"), Hex("#FFFBD2"), (state == 1 ? 60.0f : 32.0f) / 255.0f),
             cap / 2, (h - lh) / 2);
    }
    Over(o, Clamp3(dis ? TevA(*rim, "#4A4232", "#8E8670") : TevA(*rim, "#66472D", "#DEC686"), cap, h, w), 0, 0);
    const int ks = std::max(1, static_cast<int>(std::lround(48 * u)));
    const Img k = Resize(TevA(*scroll, "#28190A", "#28190A", 80.0f / 255.0f), ks, ks);
    Over(o, k, static_cast<int>(std::lround(40 * u - ks / 2.0f)), static_cast<int>(std::lround(h / 2.0f - 3 * u - ks / 2.0f)));
    Over(o, Rot180(k), static_cast<int>(std::lround(w - 38 * u - ks / 2.0f)),
         static_cast<int>(std::lround(h / 2.0f + 3 * u - ks / 2.0f)));
    if (state == 1) {
        Over(o, Clamp3(TevA(*body, "#AB875A", "#AB875A", 180.0f / 255.0f * 0.40f), cap, h, w), 0, 0);
        Over(o, Clamp3(TevA(*rim, "#FFF5A5", "#FFF5A5", 128.0f / 255.0f), cap, h, w), 0, 0);
    } else if (state == 2) {
        Over(o, Clamp3(TevA(*rim, "#FFF5A5", "#FFF5A5", 128.0f / 255.0f), cap, h, w), 0, 0);
        Over(o, Clamp3(TevA(*flash, "#FFFAC8", "#FFFAC8", 40.0f / 255.0f), cap, h, w), 0, 0);
    }
    return o;
}

// dev.46 (owner pick, alt B for the top bars): the Options screen row (MenuOptions options_00
// P_bg_00: tr_optionsBtn_00 1240x120 RGBA8 drawn 1:1 at 1080p): the 64 px rounded left end, its
// mirror, and the dark name part's column (x 300) repeated, at 120 px, then scaled to h (no
// stretched ends). Selected = the native focus: the flash window W_flash_00 (615x54 units on the
// 620x60 row, tr_optionsBtnFlash_00 tinted (247, 226, 192)), its tapered ends kept and its middle
// column repeated (the live Options screen, dev.46 capture: unfocused rows at full strength, the
// focused row brightened by this flash, about 0.24 in its middle at that moment). Under the white
// label that measured 3.99:1, so the flash is eased out of the label's band (the alt-B mockup's
// rule). Pressed = the same, stronger. State 3 = an unselected tab: the plain row at 62 % (the
// approved alt-B mockup; the Options screen has no unfocused look, it marks focus with its gold
// bracket cursor, which has no room between the 12 px tab gaps). Visible body: rows 2..110 of the
// 120 px row (a 10 px shadow).
std::optional<Img> Art::OptRow(int w, int h, int state) {
    if (w < 64 || h < 16 || w > 2048 || h > 240 || state < 0 || state > 3)
        return std::nullopt;
    auto t = Layout("MenuOptions", "tr_optionsBtn_00");
    auto fl = Layout("MenuOptions", "tr_optionsBtnFlash_00");
    if (!t || !fl || t->w < 320 || t->h < 16 || fl->w < 4)
        return std::nullopt;
    const int H = t->h, W = std::max(130, static_cast<int>(std::lround(w * static_cast<float>(H) / h)));
    constexpr int kEnd = 64, kMid = 300;
    Img o(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = kEnd; x < W - kEnd; ++x)
            std::memcpy(o.at(x, y), t->at(kMid, y), 16);
    const Img end = Crop(*t, 0, 0, kEnd, H);
    Over(o, end, 0, 0);
    Over(o, FlipH(end), W - kEnd, 0);
    if (state > 0) {
        const int fw = W - 10, fh = H - 12;
        Img f;
        if (fw >= fl->w) {
            const int half = fl->w / 2;
            f = Img(fw, fl->h);
            for (int y = 0; y < fl->h; ++y)
                for (int x = half; x < fw - (fl->w - half); ++x)
                    std::memcpy(f.at(x, y), fl->at(half, y), 16);
            Over(f, Crop(*fl, 0, 0, half, fl->h), 0, 0);
            Over(f, Crop(*fl, half, 0, fl->w - half, fl->h), fw - (fl->w - half), 0);
            f = Resize(f, fw, fh);
        } else {
            f = Resize(*fl, fw, fh);
        }
        // eased out of the label's band (the alt-B mockup's rule; the live capture's full-strength flash
        // under the white label measured 3.99:1 on the selected tab): the flash brightens the row's
        // upper and lower thirds and its ends, x 1.6 (selected) / x 2.4 (pressed) the texture alpha
        Img g = TevA(f, "#000000", "#F7E2C0", state == 2 ? 2.4f : 1.6f);
        for (int y = 0; y < g.h; ++y) {
            const float d = std::fabs(y - g.h / 2.0f) / (0.32f * H);
            const float m = 1.0f - std::exp(-std::pow(d, 6.0f));
            for (int x = 0; x < g.w; ++x)
                g.at(x, y)[3] = std::min(1.0f, g.at(x, y)[3] * m);
        }
        Over(o, g, 5, 6);
    }
    if (state == 3)
        o = Fade(o, 0.62f); // an unselected tab sinks into the board (the alt-B mockup; no native look)
    return W == w && H == h ? o : Resize(o, w, h);
}

// dev.46: the refusal pill: the HUD do-prompt window (DoButton basicInfo_00 W_bgP_00: the body
// silhouette tr_doWinLTS_00 in (20, 5, 45), here opaque so the word reads on any ground; W_bgP_01:
// the gold frame tr_doWinLT_00 / tr_doWinLD_00, (148, 116, 23) -> (255, 223, 127) at 128). The
// window is two 41-texel rows: corners at h / 82 px per texel, LT = doWinLT, RT = doWinLD turned,
// LB = doWinLD, RB = doWinLT turned; the straight runs are the corners' last column.
// dev.48: a pill taller than kPillBase keeps the kPillBase pill's corners and rim (9 px bands top and
// bottom) and grows its straight middle: the rows between the corners repeat the top corners' last row,
// so the navy body takes the extra height (64 px outer: 46 px of navy; scaled whole it would be 38).
// Up to kPillBase px the window is drawn exactly as before.
constexpr int kPillBase = 44;
std::optional<Img> Art::DoPill(int w, int h) {
    if (w < 32 || h < 16 || w > 1024 || h > 256)
        return std::nullopt;
    auto lts = Layout("DoButton", "tr_doWinLTS_00");
    auto ltt = Layout("DoButton", "tr_doWinLT_00");
    auto ldt = Layout("DoButton", "tr_doWinLD_00");
    if (!lts || !ltt || !ldt)
        return std::nullopt;
    const int base = std::min(h, kPillBase);
    const int hh = base / 2, bh = base - hh, low = h - bh;
    const int cw = std::min(w / 2, static_cast<int>(std::lround(64.0f * base / 82.0f)));
    // one window: top-left corner tl (cw x hh), bottom-left bl (cw x bh), the right side mirrored /
    // turned as `right` says, the straight runs from the corners' last column, the rows between the
    // corners (a pill taller than kPillBase) copies of the top corners' last row
    const auto window = [&](const Img& tl, const Img& bl, const Img& tr, const Img& br) {
        Img o(w, h);
        for (int y = 0; y < hh; ++y)
            for (int x = cw; x < w - cw; ++x)
                std::memcpy(o.at(x, y), tl.at(cw - 1, y), 16);
        for (int y = 0; y < bh; ++y)
            for (int x = cw; x < w - cw; ++x)
                std::memcpy(o.at(x, low + y), bl.at(cw - 1, y), 16);
        Over(o, tl, 0, 0);
        Over(o, bl, 0, low);
        Over(o, tr, w - cw, 0);
        Over(o, br, w - cw, low);
        for (int y = hh; y < low; ++y)
            std::memcpy(o.at(0, y), o.at(0, hh - 1), static_cast<size_t>(w) * 16);
        return o;
    };
    const Img bt = Resize(TevA(*lts, "#14052D", "#14052D"), cw, hh);
    const Img bb = Resize(FlipV(TevA(*lts, "#14052D", "#14052D")), cw, bh);
    Img o = window(bt, bb, FlipH(bt), FlipH(bb));
    const Img lt = Resize(TevA(*ltt, "#947417", "#FFDF7F", 128.0f / 255.0f), cw, hh);
    const Img ld = Resize(TevA(*ldt, "#947417", "#FFDF7F", 128.0f / 255.0f), cw, bh);
    Over(o, window(lt, ld, Resize(Rot180(TevA(*ldt, "#947417", "#FFDF7F", 128.0f / 255.0f)), cw, hh),
                   Resize(Rot180(TevA(*ltt, "#947417", "#FFDF7F", 128.0f / 255.0f)), cw, bh)), 0, 0);
    return o;
}

// The neighbouring-area plate (MapField mapPlace_00): the MAP_xx name (label 0..10 = MAP_00..10,
// 11 = MAP_40, 12 = MAP_UNKNOWN "?") in the T_area_00 style on the striped band in the region
// colour. The native draw (main+0xC830xx) puts the plate outside its mark: the box (text x 1.2,
// band height) moves along the mark's screen direction until its edge meets the mark. Fixed
// 900 px canvas whose centre is the mark.
bool Art::PlaceLines(int label, std::vector<Img>& lines, int& tw, int& th) {
    lines.clear();
    tw = th = 0;
    if (label < 0 || label > 12)
        return false;
    char key[16];
    if (label == 12)
        std::snprintf(key, sizeof(key), "MAP_UNKNOWN");
    else
        std::snprintf(key, sizeof(key), "MAP_%02d", label == 11 ? 40 : label);
    const auto name = MapText(key);
    if (!name)
        return false;
    size_t s0 = 0;
    while (s0 <= name->size()) {
        size_t e = name->find('\n', s0);
        if (e == std::string::npos)
            e = name->size();
        std::string ln = name->substr(s0, e - s0);
        while (!ln.empty() && ln.back() == ' ')
            ln.pop_back();
        if (!ln.empty()) {
            auto t = Text("place", 30, ln);
            if (!t)
                return false;
            tw = std::max(tw, t->w);
            th += t->h;
            lines.push_back(std::move(*t));
        }
        s0 = e + 1;
    }
    return !lines.empty();
}

std::optional<PlaceSize> Art::PlaceMetrics(int label) {
    std::vector<Img> lines;
    int tw = 0, th = 0;
    if (!PlaceLines(label, lines, tw, th))
        return std::nullopt;
    return PlaceSize{tw, th, std::max(160, static_cast<int>(tw * 1.8f)), th + 12};
}

std::optional<Img> Art::Place(int label, int deg, int region) {
    if (label < 0 || label > 12 || deg < 0 || deg >= 360 || region < 0 || region > 5)
        return std::nullopt;
    std::vector<Img> lines;
    int tw = 0, th = 0;
    if (!PlaceLines(label, lines, tw, th))
        return std::nullopt;
    const int bw = std::max(160, static_cast<int>(tw * 1.8f)), bh = th + 12;
    auto band = PlacePlate(bw, bh, region);
    if (!band)
        return std::nullopt;
    constexpr int S = 900;
    const auto off = PlaceOffset(deg, static_cast<float>(tw), static_cast<float>(bw), static_cast<float>(bh), S);
    const float cx = S / 2.0f + off[0], cy = S / 2.0f + off[1];
    Img c(S, S);
    OverCentered(c, *band, cx, cy);
    float y = cy - th / 2.0f;
    for (const auto& ln : lines) {
        OverCentered(c, ln, cx, y + ln.h / 2.0f);
        y += ln.h;
    }
    return c;
}

// MapSky N_cloudBig_00 at its group scale 2.2: P_edge_00 (tr_kumoNuki_02 quartered, olive
// (64, 64, 0)) round the cloud, P_bg_09 (tr_kumoNuki_01, white) and the three swirl panes
// P_kumo_06 / 07 / 08 (tr_areaLight_02 in their materials, shaped by tr_kumoNuki_01). cloudAlpha
// frame 0 (not entered): the cover at 220 and the swirls at 255; frame 1 (entered): cover 0,
// swirls 64. 300-unit canvas at 2 px per unit.
std::optional<Img> Art::Thunderhead(int entered) {
    auto edge = Layout("MapSky", "tr_kumoNuki_02");
    auto bg = Layout("MapSky", "tr_kumoNuki_01");
    auto sw = Layout("MapSky", "tr_areaLight_02");
    if (!edge || !bg || !sw || entered < 0 || entered > 1)
        return std::nullopt;
    constexpr float U = 2.0f;
    Img c(static_cast<int>(300 * U), static_cast<int>(300 * U));
    const Img shape = Quad(*bg);
    const float g = 128 * 2.2f;
    const float cover = entered ? 0.0f : 220.0f / 255.0f, swirl = entered ? 64.0f / 255.0f : 1.0f;
    if (cover > 0)
        PaneOn(c, TevA(shape, "#FFFFFF", "#FFFFFF", cover), g, g, 0, 0, U);
    struct K {
        const char *b, *w;
        float s;
    };
    for (const K& k : {K{"#C3DDDF", "#B2CED2", 1.05f}, K{"#C3DDDF", "#BAD6DB", 0.95f}, K{"#FFFFFF", "#FFFFFF", 1.0f}}) {
        const int px = static_cast<int>(g * k.s * U);
        Img swirl_img = MaskBy(Resize(TevA(*sw, k.b, k.w, swirl * 0.6f), px, px), Resize(shape, px, px));
        OverCentered(c, swirl_img, c.w / 2.0f, c.h / 2.0f);
    }
    // the native ring reads as a faint olive line (capture 07), not the material's full alpha
    PaneOn(c, TevA(Quad(*edge), "#404000", "#404000", 0.3f), g, g, 0, 0, U);
    return c;
}

// MapSky N_areaLight_00: P_light_00_00 (tr_areaLight_00, 36 x 0.7 units) in the areaColor frame
// of the region (0 Faron teal, 1 Eldin red, 2 Lanayru yellow) under the cloud petals P_kumo_02 /
// _00 / _01 (tr_areaLight_01, 48 units x 1.1 / 0.8 / 1.0); 64-unit canvas.
std::optional<Img> Art::Column(int region) {
    auto light = Layout("MapSky", "tr_areaLight_00");
    auto petal = Layout("MapSky", "tr_areaLight_01");
    if (!light || !petal || region < 0 || region > 2)
        return std::nullopt;
    static constexpr const char* kB[3] = {"#19D37F", "#EC1919", "#ECBA19"};
    static constexpr const char* kW[3] = {"#8CEDD9", "#FFB09F", "#FFFF98"};
    Img c(static_cast<int>(64 * kMarkUnit), static_cast<int>(64 * kMarkUnit));
    PaneOn(c, TevA(*petal, "#CFE0E1", "#D3E4E6"), 48 * 1.1f, 48 * 1.1f);
    PaneOn(c, TevA(*petal, "#CFE0E1", "#D3E4E6"), 48 * 0.8f, 48 * 0.8f);
    PaneOn(c, TevA(*petal, "#FFFFFF", "#FFFFFF"), 48, 48);
    PaneOn(c, TevA(*light, kB[region], kW[region]), 36 * 0.7f, 36 * 0.7f);
    return c;
}

// A Sky island's name (MAP_00 Skyloft, MAP_26..31) as the native name layer draws it: brown
// letters with a cream halo, the game's line breaks, left-aligned `gap` px right of the island's
// centre and vertically centred on it. Fixed 640 px canvas whose centre is the island.
bool Art::IsleLines(int idx, std::vector<Img>& lines, int& th) {
    static constexpr int kMap[7] = {0, 26, 27, 28, 29, 30, 31};
    lines.clear();
    th = 0;
    if (idx < 0 || idx > 6)
        return false;
    char key[16];
    std::snprintf(key, sizeof(key), "MAP_%02d", kMap[idx]);
    const auto name = MapText(key);
    if (!name)
        return false;
    size_t s0 = 0;
    while (s0 <= name->size()) {
        size_t e = name->find('\n', s0);
        if (e == std::string::npos)
            e = name->size();
        std::string ln = name->substr(s0, e - s0);
        while (!ln.empty() && ln.back() == ' ')
            ln.pop_back();
        if (!ln.empty()) {
            auto t = Text("isle", 26, ln);
            if (!t)
                return false;
            th += t->h - 10;
            lines.push_back(std::move(*t));
        }
        s0 = e + 1;
    }
    return true;
}

std::optional<Img> Art::IsleName(int idx, int gap) {
    if (idx < 0 || idx > 6 || gap < 0 || gap > 200)
        return std::nullopt;
    std::vector<Img> lines;
    int th = 0;
    if (!IsleLines(idx, lines, th))
        return std::nullopt;
    constexpr int S = 640;
    Img c(S, S);
    int y = S / 2 - th / 2 - 5;
    for (const auto& ln : lines) {
        Over(c, ln.w > S / 2 - gap ? Resize(ln, S / 2 - gap, ln.h) : ln, S / 2 + gap, y);
        y += ln.h - 10;
    }
    return c;
}

std::optional<Box> Art::IsleNameBox(int idx, int gap) {
    if (idx < 0 || idx > 6 || gap < 0 || gap > 200)
        return std::nullopt;
    std::vector<Img> lines;
    int th = 0;
    if (!IsleLines(idx, lines, th) || lines.empty())
        return std::nullopt;
    constexpr int S = 640;
    // the same layout as IsleName: line images left-aligned `gap` px right of the island
    int y = S / 2 - th / 2 - 5, w = 0;
    const int y0 = y;
    int y1 = y;
    for (const auto& ln : lines) {
        w = std::max(w, std::min(ln.w, S / 2 - gap));
        y1 = std::max(y1, y + ln.h);
        y += ln.h - 10;
    }
    return Box{static_cast<float>(gap), static_cast<float>(y0 - S / 2), static_cast<float>(gap + w),
               static_cast<float>(y1 - S / 2)};
}

// ---------------------------------------------------------------- key dispatch

std::optional<Img> Art::Build(std::string_view key) {
    auto split = [](std::string_view s, size_t max_parts) {
        std::vector<std::string> parts;
        while (parts.size() + 1 < max_parts) {
            const size_t slash = s.find('/');
            if (slash == std::string_view::npos)
                break;
            parts.emplace_back(s.substr(0, slash));
            s.remove_prefix(slash + 1);
        }
        parts.emplace_back(s);
        return parts;
    };
    auto num = [](const std::string& s) { return std::atoi(s.c_str()); };
    // sil/<key>: the Collection's "not collected" look of an icon (DoButton commonIcon_00_shadow:
    // the icon in black at alpha 80, its bottom-left vertex transparent)
    if (key.rfind("sil/", 0) == 0) {
        auto im = Build(key.substr(4));
        if (!im)
            return im;
        Img o = *im;
        for (size_t i = 0; i < o.p.size(); i += 4)
            Set(&o.p[i], 0, 0, 0, o.p[i + 3] * 80.0f / 255.0f);
        return VtxAlpha(o, 1, 1, 0, 1);
    }
    // dev.34: ctitle/<style>/<cap>/<w>/<h>/<MapText label>: the native map name with its own line
    // breaks (the native plate draws MAP_06 "Volcano \nSummit" on two lines), each line at cap (two
    // or more lines at 0.8 cap), centred in a w x h box and shrunk to fit, never below kMinPlateCap;
    // an empty name = nothing.
    if (key.rfind("ctitle/", 0) == 0) {
        last_fit_cap_ = INFINITY; // no name drawn: nothing to fit
        const auto t = split(key, 6);
        if (t.size() != 6)
            return std::nullopt;
        const int bw = num(t[3]), bh = num(t[4]);
        if (bw < 8 || bh < 8 || bw > 2048 || bh > 512)
            return std::nullopt;
        Img o(bw, bh);
        const auto name = MapText(t[5]);
        if (!name)
            return t[5] == "-" ? std::optional<Img>(o) : std::nullopt;
        std::vector<std::string> lines;
        for (size_t s0 = 0; s0 <= name->size();) {
            size_t e = name->find('\n', s0);
            if (e == std::string::npos)
                e = name->size();
            std::string ln = name->substr(s0, e - s0);
            while (!ln.empty() && ln.back() == ' ')
                ln.pop_back();
            if (!ln.empty())
                lines.push_back(ln);
            s0 = e + 1;
        }
        const float cap = static_cast<float>(std::atof(t[2].c_str())) * (lines.size() > 1 ? 0.8f : 1.0f);
        std::vector<Img> ims;
        int tw = 0, th = 0;
        for (const auto& ln : lines) {
            auto im = Text(t[1], cap, ln);
            if (!im)
                return std::nullopt;
            tw = std::max(tw, im->w);
            th += im->h - (ims.empty() ? 0 : static_cast<int>(cap * 0.3f)); // the glyph boxes' slack overlaps
            ims.push_back(std::move(*im));
        }
        if (ims.empty())
            return o;
        // dev.42: the shrink stops at the 18 px secondary-text cap (legibility minimums); a name
        // that would need less overflows its box instead (the manifest's boxes fit every map name
        // at 18 or more: LastFitCap, checked by sshd_art_test over the real names)
        const float fit = std::min({1.0f, static_cast<float>(bw) / tw, static_cast<float>(bh) / th});
        const float k = std::max(fit, std::min(1.0f, kMinPlateCap / cap));
        last_fit_cap_ = cap * fit;
        float y = (bh - th * k) / 2.0f;
        for (const auto& im : ims) {
            const Img r = k < 1.0f ? Resize(im, std::max(1, static_cast<int>(im.w * k)), std::max(1, static_cast<int>(im.h * k))) : im;
            Over(o, r, (bw - r.w) / 2, static_cast<int>(std::lround(y)));
            y += r.h - cap * 0.3f * k;
        }
        return o;
    }
    const auto p = split(key, key.rfind("map/", 0) == 0 ? 6 : 5); // dev.32 map keys have six parts
    if (p.empty())
        return std::nullopt;
    const std::string& kind = p[0];
    if (kind == "lyt" && p.size() == 3)
        return Layout(p[1], p[2]);
    // sq/<arc>/<member>: the layout texture centred on a transparent square canvas, so square
    // icon rects keep the native aspect (DoButton icons are e.g. 128x176, 200x160).
    if (kind == "sq" && p.size() == 3) {
        auto im = Layout(p[1], p[2]);
        if (!im)
            return im;
        const int n = std::max(im->w, im->h);
        return Crop(*im, (im->w - n) / 2, (im->h - n) / 2, n, n);
    }
    if (kind == "txt") {
        const auto t = split(key, 4);
        if (t.size() == 4)
            return Text(t[1], static_cast<float>(std::atof(t[2].c_str())), t[3]);
    }
    if (kind == "bg" && p.size() == 5)
        return Background(num(p[1]), num(p[2]), num(p[3]), num(p[4]));
    if (kind == "frame")
        return MinimapFrame(p.size() == 2 && p[1] == "pressed");
    if (kind == "compass")
        return Compass();
    if (kind == "socket" && p.size() == 2)
        return Socket(num(p[1]));
    if (kind == "beacon" && p.size() == 2)
        return Beacon(num(p[1]));
    if (kind == "digit" && p.size() == 3)
        return Digit(num(p[1]), num(p[2]));
    if (kind == "plank" && p.size() == 4)
        return Plank(num(p[2]), num(p[3]), p[1]);
    if (kind == "map" && p.size() == 2)
        return p[1] == "F020" ? SkyMap() : Map(p[1]);
    // dev.34: gmapf/<stage>/<y lo>/<y hi>/<state>: a generic stage's picture of one native floor (the
    // faces whose centroid lies in the floor's live band)
    if (kind == "gmapf" && p.size() == 5) {
        yband_[0] = static_cast<float>(num(p[2])), yband_[1] = static_cast<float>(num(p[3]));
        auto im = Map(p[1], 0xFFFFFFFFu, 0, true, 0xFFFFFFFFu, num(p[4]));
        yband_[0] = -INFINITY, yband_[1] = INFINITY;
        return im;
    }
    if (kind == "map" && p.size() == 3) // map/<stage>/<room mask>
        return Map(p[1], static_cast<uint32_t>(std::strtoul(p[2].c_str(), nullptr, 10)));
    if (kind == "map" && p.size() == 4) // map/<stage>/<room mask>/<floor>
        return Map(p[1], static_cast<uint32_t>(std::strtoul(p[2].c_str(), nullptr, 10)), num(p[3]));
    // dev.32: map/<stage>/<room mask>/<floor>/<full-colour rooms>[/<state>] (unvisited rooms in the
    // native Map shade; state bit 0 = the past variant)
    if (kind == "map" && (p.size() == 5 || p.size() == 6))
        return Map(p[1], static_cast<uint32_t>(std::strtoul(p[2].c_str(), nullptr, 10)), num(p[3]), true,
                   static_cast<uint32_t>(std::strtoul(p[4].c_str(), nullptr, 10)), p.size() == 6 ? num(p[5]) : 0);
    if (kind == "stamina" && p.size() == 3)
        return StaminaWheel(num(p[1]), num(p[2]));
    if (kind == "dpad" && (p.size() == 2 || p.size() == 3))
        return Dpad(p[1], p.size() == 3 ? num(p[2]) : 0);
    if (kind == "stamseg" && p.size() == 5)
        return StaminaSegment(num(p[1]), num(p[2]), num(p[3]), num(p[4]));
    if (kind == "stamtrack" && (p.size() == 3 || p.size() == 4))
        return StaminaTrack(num(p[1]), num(p[2]), p.size() == 4 ? num(p[3]) : 0);
    if (kind == "softglow") {
        // highlight behind the current shortcut (mockup softglow: circleM in warm gold)
        auto g = Layout("Map2D", "tr_circleM_00");
        if (!g)
            return std::nullopt;
        return AMask(Resize(*g, 232, 232), Hex("#FFE9A0"), 0.55f);
    }
    if (kind == "wingoff") {
        // a spent dash feather: the native outline piece over a dimmed feather
        auto w = Layout("DoButton", "th_skyWing_01");
        auto o = Layout("DoButton", "th_skyWingS_02");
        if (!w || !o)
            return std::nullopt;
        Img im = Fade(Tint(*w, Hex("#3A1010"), Hex("#8A5A5A")), 0.55f);
        Over(im, Fade(Resize(*o, im.w, im.h), 0.8f), 0, 0);
        return im;
    }
    if (kind == "loft")
        return LoftMarker();
    if (kind == "altgauge" && p.size() == 2)
        return AltGauge(num(p[1]));
    if (kind == "ribbon" && p.size() == 3)
        return Ribbon(num(p[1]), num(p[2]));
    if (kind == "areaplate" && p.size() == 4)
        return AreaPlate(num(p[1]), num(p[2]), num(p[3]));
    if (kind == "placeplate" && p.size() == 4)
        return PlacePlate(num(p[1]), num(p[2]), num(p[3]));
    if (kind == "cpanel" && p.size() == 4)
        return CollPanel(num(p[1]), num(p[2]), num(p[3]));
    if (kind == "cone" && p.size() == 2)
        return Cone(num(p[1]));
    if (kind == "goddesstag")
        return GoddessTag();
    if (kind == "statue" && p.size() == 3)
        return Statue(num(p[1]), num(p[2]) != 0);
    if (kind == "door" && p.size() == 2)
        return Door(num(p[1]));
    if (kind == "doorkey")
        return DoorKey();
    if (kind == "mark")
        return Mark();
    if (kind == "triforce")
        return Triforce();
    if (kind == "target" && p.size() == 2)
        return Target(num(p[1]));
    if (kind == "bug")
        return Bug();
    if (kind == "closedchest")
        return ClosedChest();
    if (kind == "objective" && p.size() == 2)
        return Objective(num(p[1]));
    if (kind == "balloon" && p.size() == 2)
        return Balloon(num(p[1]));
    if (kind == "cave")
        return Cave();
    if (kind == "kikwi" && p.size() == 2)
        return Kikwi(num(p[1]));
    if (kind == "beedle" && p.size() == 2)
        return BeedleShop(num(p[1]));
    if (kind == "start" && p.size() == 2)
        return StartArrow(num(p[1]));
    if (kind == "skykeep" && p.size() == 4 && p[1].size() == 9) {
        // skykeep/<9 cells, one char each: '0'..'8' a room, '-' empty>/<Link's room>/<1 = all rooms>
        int cells[9];
        for (int i = 0; i < 9; ++i)
            cells[i] = p[1][i] >= '0' && p[1][i] <= '8' ? p[1][i] - '0' : -1;
        return SkyKeep(cells, num(p[2]), num(p[3]) == 0);
    }
    if (kind == "floortab" && p.size() == 3)
        return FloorTab(num(p[1]), num(p[2]));
    // dev.46: choice/<w>/<h>/<state>[/<opaque>], optrow/<w>/<h>/<state>, dopill/<w>/<h>
    if (kind == "choice" && (p.size() == 4 || p.size() == 5))
        return Choice(num(p[1]), num(p[2]), num(p[3]), p.size() == 5 && num(p[4]) != 0);
    if (kind == "optrow" && p.size() == 4)
        return OptRow(num(p[1]), num(p[2]), num(p[3]));
    if (kind == "dopill" && p.size() == 3)
        return DoPill(num(p[1]), num(p[2]));
    if (kind == "blank")
        return Img(4, 4); // a transparent picture (the Map page's Link marker off his floor)
    if (kind == "place" && p.size() == 4)
        return Place(num(p[1]), num(p[2]), num(p[3]));
    if (kind == "thunderhead" && p.size() == 2)
        return Thunderhead(num(p[1]));
    if (kind == "column" && p.size() == 2)
        return Column(num(p[1]));
    if (kind == "islename" && p.size() == 3)
        return IsleName(num(p[1]), num(p[2]));
    if (kind == "openchest") {
        // the opened-chest map icon (MapField mapIcon_00 N_treasure_01: nh_reasure_01 mirrored into
        // its 23-unit quad (u 0..2, mirror wrap), black (8,37,39) -> white (158,158,31), over its
        // black shadow at 64 offset (-1.5, -2) units), 23 units = 80 px on a 100 px square
        auto t = Layout("MapField", "nh_reasure_01");
        if (!t)
            return std::nullopt;
        Img full(t->w * 2, t->h);
        Over(full, *t, 0, 0);
        Over(full, FlipH(*t), t->w, 0);
        const Img q = Resize(full, 80, 80);
        Img o(100, 100);
        Over(o, TevA(q, "#000000", "#000000", 64.0f / 255.0f), 10 - 5, 10 + 7);
        Over(o, TevA(q, "#082527", "#9E9E1F"), 10, 10);
        return o;
    }
    if (kind == "cslot" && p.size() == 3)
        return CollSlot(num(p[1]), num(p[2]));
    if (kind == "ccheck" && p.size() == 3) {
        // the quest-item check mark (pause_01 P_check_*: tr_checkMark_00, (255,223,206) -> (255,63,0))
        auto m = Layout("MenuPause", "tr_checkMark_00");
        if (!m || num(p[1]) < 4 || num(p[2]) < 4 || num(p[1]) > 512 || num(p[2]) > 512)
            return std::nullopt;
        return TevA(Resize(*m, num(p[1]), num(p[2])), "#FFDFCE", "#FF3F00");
    }
    if (kind == "cnum" && p.size() == 3) {
        // a Collection count (commonIcon T_comNumber_00: the HUD caption colours), right-aligned in
        // a box two digits wide so one fixed widget rect never stretches it
        const int v = num(p[2]);
        auto t = Text("hud", static_cast<float>(num(p[1])), std::to_string(v));
        auto box = Text("hud", static_cast<float>(num(p[1])), "88");
        if (!t || !box || v < 0 || v > 999)
            return std::nullopt;
        Img o(std::max(box->w + 6, t->w), t->h); // 6 px slack: digit widths differ a little
        Over(o, *t, o.w - t->w, 0);
        return o;
    }
    error_ = "unknown key " + std::string(key);
    return std::nullopt;
}

} // namespace sshd
