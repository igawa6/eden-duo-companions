// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sw_modfont.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>

#include "acnh_tex.h"
#include "sw_font_metrics.h"
#include "sw_strings.h"
#include "sw_text.h"
#include "sw_variant.h"
#include "sw_variant.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace sw_modfont {
namespace {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
constexpr std::size_t kMaxFontBytes = 16u << 20;
constexpr std::size_t kMaxDatBytes = 4u << 20;
constexpr std::size_t kMaxAtlasBytes = 64u << 20;

u32 Be32(const u8* p) { return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3]; }
u16 Le16(std::span<const u8> d, std::size_t o) { return o + 2 <= d.size() ? static_cast<u16>(d[o] | (d[o + 1] << 8)) : 0; }
u32 Le32(std::span<const u8> d, std::size_t o) {
    return o + 4 <= d.size() ? u32{d[o]} | (u32{d[o + 1]} << 8) | (u32{d[o + 2]} << 16) | (u32{d[o + 3]} << 24) : 0;
}

// lp_unity's CJK rule (CjkMetricsFor / CjkCentred): Han, kana and full-width forms drawn at their
// own scale, 1.05 cap heights of ink centred 0.45 cap heights above the baseline.
constexpr float kCjkInk = 1.05f, kCjkCentre = 0.45f;
bool CjkCentred(u32 cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3000 && cp <= 0x30FF) || (cp >= 0x3130 && cp <= 0x318F) ||
           (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}
struct Scales {
    float scale = 0, cjk = 0;
    int shift = 0;
};
bool IsSfnt(const u8* p) {
    return (p[0] == 0 && p[1] == 1 && p[2] == 0 && p[3] == 0) || std::memcmp(p, "OTTO", 4) == 0 ||
           std::memcmp(p, "true", 4) == 0;
}
bool OpenFace(std::span<const u8> bytes, stbtt_fontinfo& f) {
    if (bytes.size() < 12 || bytes.size() > kMaxFontBytes) return false;
    const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
    return offset >= 0 && stbtt_InitFont(&f, bytes.data(), offset) != 0;
}
std::optional<Scales> ScalesOf(const stbtt_fontinfo& f, u32 cap_px) {
    float cap_units = 0;
    for (const int probe : {'H', 'E', 'I'}) {
        int x0, y0, x1, y1;
        const int gi = stbtt_FindGlyphIndex(&f, probe);
        if (gi != 0 && stbtt_GetGlyphBox(&f, gi, &x0, &y0, &x1, &y1) && y1 > 0) {
            cap_units = static_cast<float>(y1);
            break;
        }
    }
    if (cap_units <= 0) return std::nullopt;
    Scales s;
    s.scale = s.cjk = static_cast<float>(cap_px) / cap_units;
    for (const int probe : {0x56FD, 0x3042}) { // 国, あ
        int x0, y0, x1, y1;
        const int gi = stbtt_FindGlyphIndex(&f, probe);
        if (gi != 0 && stbtt_GetGlyphBox(&f, gi, &x0, &y0, &x1, &y1) && y1 > y0) {
            const float cap = static_cast<float>(cap_px);
            s.cjk = std::min(s.scale, kCjkInk * cap / static_cast<float>(y1 - y0));
            const float shift = static_cast<float>(y0 + y1) * 0.5f * s.cjk - kCjkCentre * cap;
            s.shift = static_cast<int>(shift >= 0 ? shift + 0.5f : shift - 0.5f);
            break;
        }
    }
    return s;
}

// The scales of every face of a chain: its own when it has Latin capitals, else the first face's
// by units-per-em (nintendo_udsg-r_ext_zh-cn: accented letters and symbols, no 'H').
std::optional<std::vector<Scales>> ChainScales(std::span<const stbtt_fontinfo> faces, u32 cap_px) {
    std::vector<Scales> out;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        auto sc = ScalesOf(faces[i], cap_px);
        if (!sc) {
            if (i == 0) return std::nullopt;
            const float ratio = stbtt_ScaleForMappingEmToPixels(&faces[i], 1.0f) /
                                stbtt_ScaleForMappingEmToPixels(&faces[0], 1.0f);
            sc = Scales{out[0].scale * ratio, out[0].cjk * ratio, out[0].shift};
        }
        out.push_back(*sc);
    }
    return out;
}

std::uint64_t NowMsSteady() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool ReadAll(const EdenDsmodHostApi& host, std::string_view path, std::size_t limit, std::vector<u8>& out) {
    if (!host.read_romfs) return false;
    const std::string p{path};
    const std::size_t size = host.read_romfs(host.userdata, p.c_str(), 0, nullptr, 0);
    if (size == 0 || size > limit) return false;
    out.resize(size);
    return host.read_romfs(host.userdata, p.c_str(), 0, out.data(), size) == size;
}

std::atomic<const Face*> g_measure{nullptr};
const sw_font::Override kOverride{
    [](char32_t cp) {
        const Face* f = g_measure.load();
        return f && f->Covers(cp);
    },
    [](char32_t cp) {
        const Face* f = g_measure.load();
        return f ? f->Advance(cp) : -1;
    },
    []() {
        const Face* f = g_measure.load();
        return f ? static_cast<int>(f->cap()) : sw_font::kCap;
    },
};
} // namespace

void SetMeasuringFace(const Face* face) {
    g_measure.store(face);
    sw_font::ActiveOverride().store(face ? &kOverride : nullptr);
}

std::optional<std::vector<u8>> DecodeBfotf(std::span<const u8> file) {
    constexpr u32 kKey = 0x49621806, kMagic = 0x7F9A0218;
    if (file.size() < 8 || (Be32(file.data()) ^ kKey) != kMagic) return std::nullopt;
    const u32 size = Be32(file.data() + 4) ^ kKey;
    if (size == 0 || size > file.size() - 8 || size > kMaxFontBytes) return std::nullopt;
    std::vector<u8> out((size + 3) & ~u32{3}, 0);
    for (u32 i = 0; i < out.size(); i += 4) {
        u8 word[4] = {0, 0, 0, 0};
        std::memcpy(word, file.data() + 8 + i, std::min<std::size_t>(4, file.size() - 8 - i));
        const u32 w = Be32(word) ^ kKey;
        out[i] = static_cast<u8>(w >> 24);
        out[i + 1] = static_cast<u8>(w >> 16);
        out[i + 2] = static_cast<u8>(w >> 8);
        out[i + 3] = static_cast<u8>(w);
    }
    out.resize(size);
    return out;
}

std::optional<Atlas> BuildOtfAtlas(std::span<const u8> otf, u32 cap_px, std::span<const u32> codepoints, u32 page_h) {
    const std::span<const u8> one[] = {otf};
    return BuildOtfAtlas(one, cap_px, codepoints, page_h);
}

std::optional<Atlas> BuildOtfAtlas(std::span<const std::span<const u8>> faces, u32 cap_px,
                                   std::span<const u32> codepoints, u32 page_h) {
    try {
        constexpr u32 kPad = 1, kMaxAtlas = 4096, kPagedWidth = 2048;
        if (cap_px < 4 || cap_px > 256 || codepoints.empty() || faces.empty()) return std::nullopt;
        std::vector<u32> cps(codepoints.begin(), codepoints.end());
        std::sort(cps.begin(), cps.end());
        cps.erase(std::unique(cps.begin(), cps.end()), cps.end());
        if (cps.back() - cps.front() > 0x10000) return std::nullopt;
        std::vector<stbtt_fontinfo> infos(faces.size());
        for (std::size_t i = 0; i < faces.size(); ++i)
            if (!OpenFace(faces[i], infos[i])) return std::nullopt;
        const auto scales = ChainScales(infos, cap_px);
        if (!scales) return std::nullopt;
        struct Raster {
            u32 cp{};
            int w{}, h{}, x0{}, y0{}, advance{}, shift{};
            std::vector<u8> coverage;
            u32 ax{}, ay{};
        };
        std::vector<Raster> rasters;
        Atlas out;
        for (const u32 cp : cps) {
            std::size_t fi = 0;
            int gi = 0;
            for (; fi < infos.size(); ++fi)
                if ((gi = stbtt_FindGlyphIndex(&infos[fi], static_cast<int>(cp))) != 0) break;
            if (gi == 0) {
                out.missing.push_back(cp);
                continue;
            }
            const stbtt_fontinfo& info = infos[fi];
            const Scales& sc = (*scales)[fi];
            Raster r;
            r.cp = cp;
            float s = sc.scale;
            if (CjkCentred(cp)) {
                s = sc.cjk;
                r.shift = sc.shift;
            }
            int adv, lsb, x1, y1;
            stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
            r.advance = static_cast<int>(std::lround(static_cast<float>(adv) * s));
            stbtt_GetGlyphBitmapBox(&info, gi, s, s, &r.x0, &r.y0, &x1, &y1);
            r.w = std::max(0, x1 - r.x0);
            r.h = std::max(0, y1 - r.y0);
            if (r.w > 0 && r.h > 0) {
                if (r.w > 1024 || r.h > 1024) return std::nullopt;
                r.coverage.assign(static_cast<std::size_t>(r.w) * r.h, 0);
                stbtt_MakeGlyphBitmap(&info, r.coverage.data(), r.w, r.h, r.w, s, s, gi);
                if (std::all_of(r.coverage.begin(), r.coverage.end(), [](u8 v) { return v == 0; })) {
                    r.w = r.h = 0;
                    r.coverage.clear();
                }
            } else {
                r.w = r.h = 0;
            }
            rasters.push_back(std::move(r));
        }
        // Shelf packing (lp_unity): a shelf never straddles a page; past one page the atlas stays
        // kPagedWidth wide and grows by whole pages.
        u32 width = 0, height = 0;
        for (u32 w = 128; w <= kMaxAtlas; w *= 2) {
            u32 x = 0, y = 0, shelf = 0;
            bool fits = true;
            for (auto& r : rasters) {
                if (r.w == 0) continue;
                const u32 cw = static_cast<u32>(r.w) + 2 * kPad, ch = static_cast<u32>(r.h) + 2 * kPad;
                if (cw > w || (page_h > 0 && ch > page_h)) {
                    fits = false;
                    break;
                }
                if (x + cw > w) {
                    x = 0;
                    y += shelf;
                    shelf = 0;
                }
                if (page_h > 0 && y / page_h != (y + ch - 1) / page_h) {
                    x = 0;
                    y = (y / page_h + 1) * page_h;
                    shelf = 0;
                }
                r.ax = x + kPad;
                r.ay = y + kPad;
                x += cw;
                shelf = std::max(shelf, ch);
            }
            const bool square = y + shelf <= w && (page_h == 0 || y + shelf <= page_h);
            const bool paged = page_h > 0 && w == kPagedWidth && y + shelf <= 0xFFFF;
            if (fits && (square || paged)) {
                width = w;
                height = std::max<u32>(y + shelf, 1);
                break;
            }
        }
        if (width == 0 || static_cast<std::size_t>(width) * height * 4 > kMaxAtlasBytes) return std::nullopt;
        out.page_h = page_h;
        out.width = width;
        out.height = height;
        out.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
        for (std::size_t i = 0; i < out.rgba.size(); i += 4) out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = 0xFF;
        for (const auto& r : rasters)
            for (int y = 0; y < r.h; ++y)
                for (int x = 0; x < r.w; ++x)
                    out.rgba[((static_cast<std::size_t>(r.ay) + y) * width + r.ax + x) * 4 + 3] =
                        r.coverage[static_cast<std::size_t>(y) * r.w + x];
        EdenDsmodFontGlyph blank{};
        blank.advance = static_cast<u16>((cap_px + 1) / 2);
        for (const auto& r : rasters)
            if (r.cp == ' ') blank.advance = static_cast<u16>(std::max(0, r.advance));
        out.first = cps.front();
        out.line_height = cap_px;
        out.glyphs.assign(cps.back() - cps.front() + 1, blank);
        for (const auto& r : rasters) {
            EdenDsmodFontGlyph g{};
            if (r.w > 0) {
                g.x = static_cast<u16>(r.ax);
                g.y = static_cast<u16>(r.ay);
                g.w = static_cast<u16>(r.w);
                g.h = static_cast<u16>(r.h);
                g.bearing_x = static_cast<std::int16_t>(r.x0);
                g.bearing_y = static_cast<std::int16_t>(-r.y0 - r.shift);
            }
            g.advance = static_cast<u16>(std::max(0, r.advance));
            out.glyphs[r.cp - out.first] = g;
        }
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Atlas> BuildBffntAtlas(std::span<const u8> d) {
    try {
        if (d.size() < 0x40 || std::memcmp(d.data(), "FFNT", 4) != 0) return std::nullopt;
        const std::size_t finf = Le16(d, 6);
        if (finf + 0x20 > d.size() || std::memcmp(d.data() + finf, "FINF", 4) != 0) return std::nullopt;
        const u32 alter = Le16(d, finf + 14);
        struct Width {
            std::int8_t left = 0;
            u8 glyph = 0, advance = 0;
        };
        const Width default_width{static_cast<std::int8_t>(d[finf + 16]), d[finf + 17], d[finf + 18]};
        const std::size_t tglp = Le32(d, finf + 20) >= 8 ? Le32(d, finf + 20) - 8 : 0;
        if (tglp + 0x20 > d.size() || std::memcmp(d.data() + tglp, "TGLP", 4) != 0) return std::nullopt;
        const u32 cell_w = d[tglp + 8], cell_h = d[tglp + 9], declared_sheets = d[tglp + 10];
        const u32 baseline = Le16(d, tglp + 16), cols = Le16(d, tglp + 20), rows = Le16(d, tglp + 22);
        const std::size_t sheet_off = Le32(d, tglp + 28);
        if (!cell_w || !cell_h || !cols || !rows || sheet_off >= d.size()) return std::nullopt;
        std::map<u32, Width> widths;
        for (u32 block = Le32(d, finf + 24), guard = 0; block >= 8 && guard < 256; ++guard) {
            const std::size_t at = block - 8;
            if (at + 16 > d.size() || std::memcmp(d.data() + at, "CWDH", 4) != 0) break;
            const u32 first = Le16(d, at + 8), last = Le16(d, at + 10);
            for (u32 g = first; g <= last && at + 16 + (g - first) * 3 + 3 <= d.size(); ++g) {
                const std::size_t e = at + 16 + (g - first) * 3;
                widths[g] = Width{static_cast<std::int8_t>(d[e]), d[e + 1], d[e + 2]};
            }
            const u32 next = Le32(d, at + 12);
            block = next > block ? next : 0;
        }
        std::map<u32, u32> cmap;
        for (u32 block = Le32(d, finf + 28), guard = 0; block >= 8 && guard < 256; ++guard) {
            const std::size_t at = block - 8;
            if (at + 24 > d.size() || std::memcmp(d.data() + at, "CMAP", 4) != 0) break;
            const u32 begin = Le32(d, at + 8), end = Le32(d, at + 12), method = Le16(d, at + 16);
            const std::size_t q = at + 24;
            if (method == 0) {
                const u32 base = Le16(d, q);
                for (u32 c = begin; c <= end && c - begin < 0x10000; ++c) cmap[c] = base + c - begin;
            } else if (method == 1) {
                for (u32 c = begin; c <= end && c - begin < 0x10000; ++c) {
                    const u32 i = Le16(d, q + 2 * (c - begin));
                    if (i != 0xFFFF) cmap[c] = i;
                }
            } else if (method == 2) {
                const u32 n = Le16(d, q);
                for (u32 k = 0; k < n && q + 4 + 8 * k + 6 <= d.size(); ++k) {
                    const u32 i = Le16(d, q + 4 + 8 * k + 4);
                    if (i != 0xFFFF) cmap[Le32(d, q + 4 + 8 * k)] = i;
                }
            }
            const u32 next = Le32(d, at + 20);
            block = next > block ? next : 0;
        }
        if (cmap.empty()) return std::nullopt;
        const u8* sheets = d.data() + sheet_off;
        const std::size_t sheets_size = d.size() - sheet_off;
        u32 sheet_count = std::max<u32>(1, acnh::BntxLayers(sheets, sheets_size));
        if (declared_sheets != 0) sheet_count = std::min(sheet_count, declared_sheets);
        Atlas out;
        u32 sw = 0, sh = 0;
        for (u32 s = 0; s < sheet_count; ++s) {
            acnh::Image img;
            if (!acnh::BntxDecodeLayer(sheets, sheets_size, s, img) || img.w <= 0 || img.h <= 0) return std::nullopt;
            if (s == 0) {
                sw = static_cast<u32>(img.w);
                sh = static_cast<u32>(img.h);
                out.rgba.assign(static_cast<std::size_t>(sw) * sh * 4 * sheet_count, 0);
            } else if (static_cast<u32>(img.w) != sw || static_cast<u32>(img.h) != sh) {
                return std::nullopt;
            }
            // sheets are stored bottom-up
            for (u32 y = 0; y < sh; ++y)
                std::memcpy(out.rgba.data() + (static_cast<std::size_t>(s) * sh + y) * sw * 4,
                            img.rgba.data() + static_cast<std::size_t>(sh - 1 - y) * sw * 4, static_cast<std::size_t>(sw) * 4);
        }
        out.width = sw;
        out.height = sh * sheet_count;
        out.page_h = sh;
        const u32 per_sheet = cols * rows;
        const auto glyph_for = [&](u32 index) {
            EdenDsmodFontGlyph g{};
            const u32 s = index / per_sheet;
            if (s >= sheet_count) return g;
            const u32 cell = index % per_sheet;
            const auto it = widths.find(index);
            const Width w = it == widths.end() ? default_width : it->second;
            g.x = static_cast<u16>((cell % cols) * (cell_w + 1) + 1);
            g.y = static_cast<u16>((cell / cols) * (cell_h + 1) + 1 + s * sh);
            g.w = w.glyph;
            g.h = static_cast<u16>(cell_h);
            g.bearing_x = w.left;
            g.bearing_y = static_cast<std::int16_t>(baseline);
            g.advance = w.advance;
            return g;
        };
        const bool has_alter = alter < per_sheet * sheet_count;
        u32 last = 0xFF;
        for (const auto& [cp, index] : cmap)
            if (cp <= 0xFFFF && index / per_sheet < sheet_count) last = std::max(last, cp);
        u32 cap = 0;
        for (const char32_t probe : {U'H', U'E', U'A'}) {
            const auto it = cmap.find(probe);
            if (it == cmap.end()) continue;
            const auto g = glyph_for(it->second);
            for (u32 row = 0; row < g.h && cap == 0; ++row)
                for (u32 col = 0; col < g.w; ++col) {
                    const std::size_t at = (static_cast<std::size_t>(g.y + row) * out.width + g.x + col) * 4 + 3;
                    if (at < out.rgba.size() && out.rgba[at] >= 128) {
                        cap = row < baseline ? baseline - row : 0;
                        break;
                    }
                }
            if (cap) break;
        }
        out.line_height = cap ? cap : std::max<u32>(1, baseline);
        out.first = 0x20;
        out.glyphs.reserve(last - 0x20 + 1);
        for (u32 c = 0x20; c <= last; ++c) {
            const auto it = cmap.find(c);
            if (it != cmap.end() && it->second / per_sheet < sheet_count) {
                out.glyphs.push_back(glyph_for(it->second));
            } else if (has_alter && c >= 0x21 && c <= 0xFF) {
                out.glyphs.push_back(glyph_for(alter)); // the host's dense run draws the alternate glyph
            } else {
                // the host has no glyph there at all (Canvas::MeasureText: half the size)
                EdenDsmodFontGlyph blank{};
                blank.advance = c <= 0xFF ? default_width.advance : static_cast<u16>((out.line_height + 1) / 2);
                out.glyphs.push_back(blank);
            }
        }
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

Face::~Face() { Close(); }

void Face::Close() {
    for (auto& m : members_) delete static_cast<stbtt_fontinfo*>(m.info);
    members_.clear();
    ok_ = false;
}

bool Face::Open(std::vector<u8> otf, u32 cap_px) {
    return Open(std::vector<Bytes>{std::make_shared<const std::vector<u8>>(std::move(otf))}, cap_px);
}

bool Face::Open(std::vector<Bytes> fonts, u32 cap_px) {
    Close();
    std::vector<stbtt_fontinfo> infos;
    for (auto& bytes : fonts) {
        if (!bytes) return false;
        Member m;
        m.bytes = std::move(bytes);
        auto* info = new stbtt_fontinfo{};
        m.info = info;
        members_.push_back(std::move(m));
        if (!OpenFace(*members_.back().bytes, *info)) {
            Close();
            return false;
        }
        infos.push_back(*info);
    }
    const auto scales = infos.empty() ? std::nullopt : ChainScales(infos, cap_px);
    if (!scales) {
        Close();
        return false;
    }
    for (std::size_t i = 0; i < members_.size(); ++i) {
        members_[i].scale = (*scales)[i].scale;
        members_[i].cjk_scale = (*scales)[i].cjk;
    }
    cap_ = cap_px;
    ok_ = true;
    return true;
}

std::vector<std::span<const u8>> Face::spans() const {
    std::vector<std::span<const u8>> out;
    for (const auto& m : members_) out.emplace_back(*m.bytes);
    return out;
}

bool Face::Covers(char32_t cp) const {
    if (cp == '\n' || cp == ' ') return true;
    if (!ok_) return false;
    for (const auto& m : members_)
        if (stbtt_FindGlyphIndex(static_cast<const stbtt_fontinfo*>(m.info), static_cast<int>(cp)) != 0) return true;
    return false;
}

int Face::Advance(char32_t cp) const {
    if (!ok_) return -1;
    for (const auto& m : members_) {
        const auto* info = static_cast<const stbtt_fontinfo*>(m.info);
        const int gi = stbtt_FindGlyphIndex(info, static_cast<int>(cp));
        if (gi == 0) continue;
        int adv, lsb;
        stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
        return static_cast<int>(std::lround(static_cast<float>(adv) * (CjkCentred(cp) ? m.cjk_scale : m.scale)));
    }
    return -1;
}

std::vector<std::string_view> SysfontsOf(sw_lang::Lang lang) {
    switch (lang) {
    case sw_lang::Lang::Korean: return {kSysfontKo};
    case sw_lang::Lang::ChineseSimplified: return {kSysfontZhCn, kSysfontZhCnExt};
    case sw_lang::Lang::ChineseTraditional: return {kSysfontZhTw};
    default: return {};
    }
}

std::uint32_t ProbeSysfonts(const EdenDsmodHostApi& host) {
    std::uint32_t mask = 0;
    for (const auto lang : {sw_lang::Lang::Korean, sw_lang::Lang::ChineseSimplified, sw_lang::Lang::ChineseTraditional}) {
        bool offered = false;
        // An older host has no such source (and may not even know prefixes): never asked.
        if ((host.capabilities & kCapSourceSysfont) != 0 && host.read_romfs) {
            const std::string path{SysfontsOf(lang).front()};
            const std::size_t size = host.read_romfs(host.userdata, path.c_str(), 0, nullptr, 0);
            u8 head[4] = {};
            offered = size >= 12 && size <= kMaxFontBytes &&
                      host.read_romfs(host.userdata, path.c_str(), 0, head, sizeof head) == sizeof head && IsSfnt(head);
        }
        sw_lang::SetSysfontOffered(lang, offered);
        if (offered) mask |= 1u << static_cast<int>(lang);
    }
    return mask;
}

void AddCodepoints(std::string_view s, std::set<u32>& out) {
    for (std::size_t i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp = c;
        std::size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0 || i + n > s.size()) {
            ++i;
            continue;
        }
        if (n > 1) {
            cp = c & (0x7F >> n);
            for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<u8>(s[i + k]) & 0x3F);
        }
        i += n;
        if (cp >= 0x20 && cp <= 0xFFFF) out.insert(cp);
    }
}

Font::~Font() {
    stopping_ = true;
    if (worker_.joinable()) worker_.join();
    for (const auto& f : faces_)
        if (f && g_measure.load() == f.get()) SetMeasuringFace(nullptr);
}

Face* Font::EnsureFace(const EdenDsmodHostApi& host, Kind kind) {
    // Kanji and kana share one chain (the Rodin face alone).
    const int slot = static_cast<int>(kind == Kind::Kana ? Kind::Kanji : kind);
    if (faces_[slot]) return faces_[slot].get();
    if (face_failed_[slot]) return nullptr;
    if (!rodin_ && !rodin_failed_) {
        std::vector<u8> file;
        std::optional<std::vector<u8>> otf;
        if (ReadAll(host, kBfotfPath, kMaxFontBytes, file)) otf = DecodeBfotf(file);
        if (otf) rodin_ = std::make_shared<const std::vector<u8>>(std::move(*otf));
        else rodin_failed_ = true;
    }
    std::vector<Face::Bytes> chain;
    const sw_lang::Lang lang = kind == Kind::Korean ? sw_lang::Lang::Korean
                               : kind == Kind::Hans ? sw_lang::Lang::ChineseSimplified
                               : kind == Kind::Hant ? sw_lang::Lang::ChineseTraditional
                                                    : sw_lang::Lang::JapaneseKanji;
    for (const auto name : SysfontsOf(lang)) {
        std::vector<u8> ttf;
        // Only real firmware fonts come back (runtime 23); an empty read is a missing face.
        if (ReadAll(host, name, kMaxFontBytes, ttf) && ttf.size() >= 12 && IsSfnt(ttf.data()))
            chain.push_back(std::make_shared<const std::vector<u8>>(std::move(ttf)));
        else if (chain.empty())
            break; // no primary face: the language cannot be drawn
    }
    const bool sysfont = !SysfontsOf(lang).empty();
    if (rodin_) chain.push_back(rodin_); // the romfs face: Japanese, or what a shared font lacks
    auto face = std::make_unique<Face>();
    if ((sysfont && (chain.empty() || chain.front() == rodin_)) || !face->Open(std::move(chain), kCapPx)) {
        face_failed_[slot] = true; // the companion stays in the font it has (alternate glyphs)
        if (host.log) {
            const std::string line = std::string(SW_VARIANT_NAME " DSMod: ") + KindCode(kind) + " font unavailable";
            host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, line.c_str());
        }
        return nullptr;
    }
    faces_[slot] = std::move(face);
    return faces_[slot].get();
}

std::vector<u32> Font::BaseCodepoints(const EdenDsmodHostApi& host, sw_lang::Lang lang) {
    std::set<u32> cps;
    for (u32 c = 0x20; c <= 0x7E; ++c) cps.insert(c);
    for (u32 c = 0xA0; c <= 0xFF; ++c) cps.insert(c);
    for (u32 c : {0x2013u, 0x2014u, 0x2018u, 0x2019u, 0x201Cu, 0x201Du, 0x2026u, 0x2640u, 0x2642u, 0x3000u, 0x3001u,
                  0x3002u, 0x300Cu, 0x300Du, 0x30FBu, 0x30FCu, 0xFF01u, 0xFF0Cu, 0xFF1Au, 0xFF1Bu, 0xFF1Fu})
        cps.insert(c);
    // the companion's own text in this language, with the game lines it shows (read from the
    // player's romfs, sw_str::LoadGame; already loaded when this is the language in effect)
    sw_str::LoadGame(host, lang);
    for (int i = 0; i < static_cast<int>(sw_str::Id::kCount); ++i) AddCodepoints(sw_str::S(static_cast<sw_str::Id>(i), lang), cps);
    // every game text table the module reads (names, descriptions, Pokédex text: the variant's own
    // entry file, places)
    static constexpr const char* kCommon[] = {
        "monsname.dat", "wazaname.dat", "itemname.dat", "tokusei.dat", "seikaku.dat", "wazainfo.dat",
        "iteminfo.dat", "tokuseiinfo.dat", sw_variant::kActive.dex_comment_file, "btl_app.dat", "btl_state.dat",
        "gwazaname.dat", "gwazainfo.dat", "townmap_facility.dat", "zkn_type.dat", "zkn_height.dat", "zkn_weight.dat",
    };
    std::vector<u8> buf;
    std::vector<std::string> lines;
    const auto add_file = [&](const std::string& path) {
        lines.clear();
        if (ReadAll(host, path, kMaxDatBytes, buf) && sw_text::ReadDat(buf.data(), buf.size(), lines))
            for (const auto& line : lines) AddCodepoints(line, cps);
    };
    for (const char* f : kCommon) add_file(sw_lang::MessagePath(lang, f));
    add_file(sw_lang::MessagePath(lang, "place_name.dat", "script"));
    for (const u32 cp : extra_) cps.insert(cp);
    return {cps.begin(), cps.end()};
}

void Font::Start(Kind kind, std::vector<u8> source, std::vector<u32> cps) {
    if (worker_.joinable()) worker_.join();
    running_ = true;
    Face* const face = kind == Kind::Latin ? nullptr : faces_[static_cast<int>(kind == Kind::Kana ? Kind::Kanji : kind)].get();
    worker_ = std::thread([this, kind, face, source = std::move(source), cps = std::move(cps)]() mutable {
        const auto start = NowMsSteady();
        std::optional<Atlas> atlas;
        if (kind == Kind::Latin) {
            atlas = BuildBffntAtlas(source);
        } else if (face) {
            const auto faces = face->spans();
            for (u32 cap = kCapPx;; cap -= 4) {
                if (stopping_) break;
                atlas = BuildOtfAtlas(faces, cap, cps, kPageH);
                if (!atlas || atlas->Pages() <= kMaxPages || cap < kMinCapPx + 4) break;
            }
            const bool fits = atlas && atlas->Pages() <= kMaxPages;
            if (!stopping_) result_ = fits ? 1 : -1;
            if (!fits) { // a grown set past the caps keeps the last fitting atlas of this kind
                std::scoped_lock lock{mutex_};
                const auto& last = prepared_ ? prepared_ : served_;
                if (last && last->kind == kind) atlas.reset();
            }
        }
        if (atlas && !stopping_) {
            auto built = std::make_shared<Built>();
            built->kind = kind;
            built->cps = std::move(cps);
            built->atlas = std::move(*atlas);
            built->build_ms = NowMsSteady() - start;
            {
                std::scoped_lock lock{mutex_};
                prepared_ = std::move(built);
            }
            SetMeasuringFace(face);
            changed_ = true;
            ++epoch_; // the host decodes again (__font_epoch) and Decode serves this build
        }
        running_ = false;
    });
}

void Font::Note(std::string_view text) {
    if (!target_ || *target_ == Kind::Latin) return;
    const Face* face = faces_[static_cast<int>(*target_ == Kind::Kana ? Kind::Kanji : *target_)].get();
    if (!face) return;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (static_cast<u8>(text[i]) < 0x80) continue;
        std::set<u32> cps;
        AddCodepoints(text.substr(i), cps);
        for (const u32 cp : cps)
            if (!known_.count(cp) && face->Covers(cp) && growth_.Accept()) {
                known_.insert(cp);
                extra_.push_back(cp);
            }
        return;
    }
}

void Font::Update(const EdenDsmodHostApi& host, sw_lang::Lang lang, std::uint64_t now_ms) {
    if (running_ || stopping_) return;
    if (awaiting_) { // the finished module-font build's verdict
        awaiting_ = false;
        const bool fits = result_.exchange(0) > 0;
        if (fits) {
            fitting_ = {building_.begin(), building_.end()};
            fitting_extra_ = building_extra_;
        } else if (!fitting_.empty()) {
            // Too many glyphs for the caps: back to the last set that fit (its atlas keeps being
            // served when this build produced none) and stop growing until the language changes.
            known_ = fitting_;
            extra_.resize(std::min(extra_.size(), fitting_extra_));
        }
        growth_.Result(fits);
        building_.clear();
    }
    const Kind want = KindOf(lang);
    const bool lang_changed = !lang_seen_ || lang != lang_;
    lang_seen_ = true;
    lang_ = lang;
    if (want == Kind::Latin) {
        // Latin from boot: the host's own BFFNT (nothing to do). After a CJK font: the module's copy.
        if (!target_ || *target_ == Kind::Latin) return;
        std::vector<u8> bffnt;
        if (!ReadAll(host, kBffntPath, kMaxFontBytes, bffnt)) return;
        target_ = Kind::Latin;
        known_.clear();
        fitting_.clear();
        Start(Kind::Latin, std::move(bffnt), {});
        return;
    }
    if (!EnsureFace(host, want)) return;
    if (lang_changed || !target_ || *target_ != want) {
        auto cps = BaseCodepoints(host, lang);
        known_ = {cps.begin(), cps.end()};
        fitting_.clear();
        growth_.Reset(now_ms);
        target_ = want;
        StartModule(want, std::move(cps));
        return;
    }
    if (growth_.Due(now_ms)) // a nickname / OT name with new glyphs (GlyphGrowth: paced and capped)
        StartModule(want, {known_.begin(), known_.end()});
}

void Font::StartModule(Kind kind, std::vector<u32> cps) {
    building_ = cps;
    building_extra_ = extra_.size();
    result_ = 0;
    awaiting_ = true;
    Start(kind, {}, std::move(cps));
}

bool Font::Decode(void* receiver, EdenDsmodFontSink sink) {
    std::shared_ptr<const Built> f;
    {
        std::scoped_lock lock{mutex_};
        if (prepared_) served_ = std::move(prepared_);
        f = served_;
    }
    if (!f || !sink || f->atlas.glyphs.empty()) return false;
    sink(receiver, f->atlas.line_height, f->atlas.first, f->atlas.glyphs.data(), static_cast<u32>(f->atlas.glyphs.size()));
    return true;
}

bool Font::LoadImage(const EdenDsmodHostApi& host, std::string_view key, void* receiver, EdenDsmodImageSink sink,
                     bool& handled) {
    handled = false;
    if (!key.starts_with(kKeyPrefix)) return false;
    handled = true;
    const auto rest = key.substr(kKeyPrefix.size());
    std::shared_ptr<const Atlas> atlas;
    u32 page = 0;
    if (rest == "{p}") {
        // The literal manifest key: the host stores its own BFFNT atlas under it at boot; this
        // answers only if it asks before that lands (the same pixels).
        {
            std::scoped_lock lock{mutex_};
            atlas = host_latin_;
        }
        if (!atlas) {
            std::vector<u8> bffnt;
            auto built = ReadAll(host, kBffntPath, kMaxFontBytes, bffnt) ? BuildBffntAtlas(bffnt) : std::nullopt;
            if (!built) return false;
            atlas = std::make_shared<const Atlas>(std::move(*built));
            std::scoped_lock lock{mutex_};
            host_latin_ = atlas;
        }
        sink(receiver, atlas->width, atlas->height, atlas->rgba.data(), atlas->rgba.size());
        return true;
    }
    if (rest.empty() || rest.size() > 3 || rest.find_first_not_of("0123456789") != std::string_view::npos) return false;
    page = static_cast<u32>(std::stoul(std::string{rest}));
    std::shared_ptr<const Built> f;
    {
        std::scoped_lock lock{mutex_};
        f = served_;
    }
    if (!f) return false;
    const auto& a = f->atlas;
    const std::uint64_t ph = a.page_h ? a.page_h : a.height;
    const std::uint64_t top = page * ph;
    if (top >= a.height) return false;
    const auto rows = static_cast<u32>(std::min<std::uint64_t>(ph, a.height - top));
    sink(receiver, a.width, rows, a.rgba.data() + top * a.width * 4, static_cast<std::size_t>(rows) * a.width * 4);
    return true;
}

Font::Stats Font::stats() const {
    std::scoped_lock lock{mutex_};
    Stats s;
    // the newest build (the one the current epoch stands for), served or not yet decoded
    const auto& f = prepared_ ? prepared_ : served_;
    if (!f) return s;
    s.kind = f->kind;
    s.module_font = true;
    s.cap_px = f->atlas.line_height;
    s.pages = f->atlas.Pages();
    s.width = f->atlas.width;
    s.height = f->atlas.height;
    s.glyphs = static_cast<u32>(f->cps.size());
    s.missing = static_cast<u32>(f->atlas.missing.size());
    s.build_ms = f->build_ms;
    s.atlas_bytes = f->atlas.rgba.size();
    return s;
}

} // namespace sw_modfont
