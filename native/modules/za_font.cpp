// SPDX-FileCopyrightText: Copyright 2026 Eden Duo Companions contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "za_font.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

namespace za {
namespace {

constexpr std::uint32_t kFirstCodepoint = 0x20u;
constexpr std::uint32_t kLineHeight = 50u;
constexpr float kCapRatio = 0.76f;
constexpr std::uint32_t kPad = 2u;
constexpr float kCjkInk = 1.05f;
constexpr float kCjkCentre = 0.45f;

std::vector<std::uint8_t> DecodeBfotf(const std::vector<std::uint8_t>& in) {
    if (in.size() < 12) {
        return {};
    }
    const auto be32 = [&](std::size_t off) -> std::uint32_t {
        return (std::uint32_t(in[off]) << 24) | (std::uint32_t(in[off + 1]) << 16) |
               (std::uint32_t(in[off + 2]) << 8) | std::uint32_t(in[off + 3]);
    };
    const std::uint32_t m0 = be32(0);
    if (m0 == 0x4F54544Fu || m0 == 0x00010000u) {
        return in;
    }
    constexpr std::uint32_t kBfttfMagic = 0x7F9A0218u;
    const std::uint32_t key = m0 ^ kBfttfMagic;
    const std::uint32_t size = be32(4) ^ key;
    if (size == 0 || size > in.size() - 8) {
        return {};
    }
    const std::uint8_t k[4] = {
        static_cast<std::uint8_t>(key >> 24),
        static_cast<std::uint8_t>(key >> 16),
        static_cast<std::uint8_t>(key >> 8),
        static_cast<std::uint8_t>(key),
    };
    std::vector<std::uint8_t> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = in[8 + i] ^ k[i & 3];
    }
    if (out.size() >= 4 &&
        ((out[0] == 'O' && out[1] == 'T' && out[2] == 'T' && out[3] == 'O') ||
         (out[0] == 0x00 && out[1] == 0x01 && out[2] == 0x00 && out[3] == 0x00))) {
        return out;
    }
    return {};
}

bool IsCjkCentred(std::uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3000 && cp <= 0x30FF) ||
           (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

bool IsCjkLanguage(int lang) {
    // 1=JA, 8=KO, 9=ZH-Hans, 10=ZH-Hant
    return lang == 1 || lang == 8 || lang == 9 || lang == 10;
}

std::uint32_t FallbackCp(std::uint32_t cp) {
    switch (cp) {
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2022: return 0xB7;
    default: return 0;
    }
}

void CollectCodepoints(int language, const stbtt_fontinfo& info, std::vector<std::uint32_t>& out) {
    out.clear();
    out.reserve(IsCjkLanguage(language) ? 4500 : 400);
    for (std::uint32_t cp = 0x20; cp <= 0x7E; ++cp) out.push_back(cp);
    for (std::uint32_t cp = 0xA0; cp <= 0x17F; ++cp) out.push_back(cp);
    static constexpr std::uint32_t kSymbols[] = {
        0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
        0x20AC, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212, 0x221E, 0x2605,
        0x2606, 0x2640, 0x2642, 0x3000, 0x30FC,
    };
    for (std::uint32_t cp : kSymbols) out.push_back(cp);

    if (IsCjkLanguage(language)) {
        // Full CJK punctuation, Hiragana, Katakana, and full-width ASCII
        for (std::uint32_t cp = 0x3000; cp <= 0x30FF; ++cp) {
            if (stbtt_FindGlyphIndex(&info, static_cast<int>(cp)) != 0) out.push_back(cp);
        }
        for (std::uint32_t cp = 0xFF01; cp <= 0xFF60; ++cp) {
            if (stbtt_FindGlyphIndex(&info, static_cast<int>(cp)) != 0) out.push_back(cp);
        }
        // All Kanji / Hanzi glyphs actually present in FOT-UDKakugoC80Pro-B (lazy paged 1024px!)
        for (std::uint32_t cp = 0x4E00; cp <= 0x9FFF; ++cp) {
            if (stbtt_FindGlyphIndex(&info, static_cast<int>(cp)) != 0) out.push_back(cp);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

std::shared_ptr<const PagedFont> BuildPagedFont(Assets& assets, int language) {
    static constexpr const char* kCandidates[] = {
        "arc/uifontbinFOT-UDKakugoC80Pro-B.bfotf.trpak",
        "arc/uifontbinFOT-UDKakugoC80Pro-DB.bfotf.trpak",
        "arc/uifontbinFOT-RodinNTLGPro-B.bfotf.trpak",
    };
    std::vector<std::uint8_t> font_bytes;
    for (const char* path : kCandidates) {
        std::vector<std::uint8_t> raw;
        if (assets.ReadFontPack(path, raw)) {
            font_bytes = DecodeBfotf(raw);
            if (!font_bytes.empty()) break;
        }
    }
    if (font_bytes.empty()) return nullptr;

    stbtt_fontinfo info{};
    const int offset = stbtt_GetFontOffsetForIndex(font_bytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info, font_bytes.data(), offset)) return nullptr;

    // Probe real cap height of 'H'/'E'/'I'
    float cap_units = 0.0f;
    for (const int probe : {'H', 'E', 'I'}) {
        int gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
        const int gi = stbtt_FindGlyphIndex(&info, probe);
        if (gi != 0 && stbtt_GetGlyphBox(&info, gi, &gx0, &gy0, &gx1, &gy1) && gy1 > 0) {
            cap_units = static_cast<float>(gy1);
            break;
        }
    }
    if (cap_units <= 0.0f) {
        int ascent = 0, descent = 0, line_gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
        cap_units = static_cast<float>(ascent) * 0.72f;
    }
    const float target_cap_px = static_cast<float>(kLineHeight) * kCapRatio;
    const float base_scale = target_cap_px / cap_units;
    const int vshift = static_cast<int>(std::lround((static_cast<float>(kLineHeight) - target_cap_px) * 0.5f));

    // CjkMetricsFor (LuminescentPlatinum convention): center ideographs cleanly on Latin cap band!
    float cjk_scale = base_scale;
    int cjk_shift = 0;
    for (const int probe : {0x56FD, 0x3042}) { // '国', 'あ'
        int gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
        const int gi = stbtt_FindGlyphIndex(&info, probe);
        if (gi != 0 && stbtt_GetGlyphBox(&info, gi, &gx0, &gy0, &gx1, &gy1) && gy1 > gy0) {
            cjk_scale = std::min(base_scale, kCjkInk * target_cap_px / static_cast<float>(gy1 - gy0));
            const float sh = static_cast<float>(gy0 + gy1) * 0.5f * cjk_scale - kCjkCentre * target_cap_px;
            cjk_shift = static_cast<int>(std::lround(sh));
            break;
        }
    }

    std::vector<std::uint32_t> cps;
    CollectCodepoints(language, info, cps);
    if (cps.empty()) return nullptr;
    const std::uint32_t last_cp = cps.back();

    auto out = std::make_shared<PagedFont>();
    out->language = language;
    out->line_height = kLineHeight;
    out->first_codepoint = kFirstCodepoint;

    // Pack glyphs into 1024x1024 virtual pages (NO pixel rasterization yet — done lazily per page!)
    std::uint32_t cur_page = 0;
    std::uint32_t cur_x = 0, cur_y = 0, shelf_h = 0;
    out->pages.emplace_back();

    std::map<std::uint32_t, EdenDsmodFontGlyph> metrics_map;
    for (std::uint32_t cp : cps) {
        const int gi = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
        if (gi == 0) continue;
        const bool cjk = IsCjkCentred(cp);
        const float sc = cjk ? cjk_scale : base_scale;
        const int y_adj = cjk ? (vshift - cjk_shift) : vshift;

        int adv = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
        stbtt_GetGlyphBitmapBox(&info, gi, sc, sc, &x0, &y0, &x1, &y1);
        const int gw = std::max(0, x1 - x0);
        const int gh = std::max(0, y1 - y0);

        EdenDsmodFontGlyph g{};
        g.advance = static_cast<std::uint16_t>(std::max(0, static_cast<int>(std::lround(static_cast<float>(adv) * sc))));
        if (gw > 0 && gh > 0 && gw <= 256 && gh <= 256) {
            const std::uint32_t cw = static_cast<std::uint32_t>(gw) + 2u * kPad;
            const std::uint32_t ch = static_cast<std::uint32_t>(gh) + 2u * kPad;
            if (cur_x + cw > PagedFont::kPageWidth) {
                cur_x = 0;
                cur_y += shelf_h;
                shelf_h = 0;
            }
            if (cur_y + ch > PagedFont::kPageHeight) {
                ++cur_page;
                cur_x = 0;
                cur_y = 0;
                shelf_h = 0;
                out->pages.emplace_back();
            }
            const std::uint32_t ax = cur_x + kPad;
            const std::uint32_t ay_in_page = cur_y + kPad;
            const std::uint32_t global_y = cur_page * PagedFont::kPageHeight + ay_in_page;
            if (global_y + gh <= 65535u) {
                g.x = static_cast<std::uint16_t>(ax);
                g.y = static_cast<std::uint16_t>(global_y);
                g.w = static_cast<std::uint16_t>(gw);
                g.h = static_cast<std::uint16_t>(gh);
                g.bearing_x = static_cast<std::int16_t>(x0);
                g.bearing_y = static_cast<std::int16_t>(-y0 + y_adj);

                PagedFont::PlacedGlyph pg{};
                pg.cp = cp;
                pg.glyph_index = gi;
                pg.ax = static_cast<std::uint16_t>(ax);
                pg.ay_in_page = static_cast<std::uint16_t>(ay_in_page);
                pg.w = static_cast<std::uint16_t>(gw);
                pg.h = static_cast<std::uint16_t>(gh);
                pg.scale = sc;
                out->pages[cur_page].push_back(pg);

                cur_x += cw;
                shelf_h = std::max(shelf_h, ch);
            }
        }
        metrics_map[cp] = g;
    }

    if (metrics_map.find('A') == metrics_map.end() || metrics_map.find('0') == metrics_map.end()) {
        return nullptr;
    }

    EdenDsmodFontGlyph blank{};
    if (const auto it = metrics_map.find(' '); it != metrics_map.end()) {
        blank.advance = it->second.advance;
    } else {
        blank.advance = 16;
    }

    out->page_count = static_cast<std::uint32_t>(out->pages.size());
    out->glyphs.assign(last_cp - kFirstCodepoint + 1u, blank);
    for (const auto& [cp, g] : metrics_map) {
        if (cp >= kFirstCodepoint && cp <= last_cp) {
            out->glyphs[cp - kFirstCodepoint] = g;
        }
    }
    for (std::uint32_t cp = kFirstCodepoint; cp <= last_cp; ++cp) {
        if (metrics_map.find(cp) == metrics_map.end()) {
            if (const std::uint32_t alt = FallbackCp(cp); alt != 0) {
                if (const auto it = metrics_map.find(alt); it != metrics_map.end()) {
                    out->glyphs[cp - kFirstCodepoint] = it->second;
                }
            }
        }
    }
    if (const auto it = metrics_map.find(0x27); it != metrics_map.end() && last_cp >= 0x2019u) {
        out->glyphs[0x2019u - kFirstCodepoint] = it->second;
    }
    out->font_bytes = std::move(font_bytes);
    return out;
}

} // namespace

std::shared_ptr<const PagedFont> FontCache::Ensure(Assets& assets, int language) {
    std::scoped_lock lock{mutex_};
    if (cached_ && cached_->language == language) {
        return cached_;
    }
    auto built = BuildPagedFont(assets, language);
    if (built) {
        cached_ = std::move(built);
    }
    return cached_;
}

bool FontCache::RenderPage(Assets& assets, int language, std::uint32_t page, Picture& out) {
    auto f = Ensure(assets, language);
    if (!f || page >= f->pages.size()) {
        return false;
    }
    stbtt_fontinfo info{};
    const int offset = stbtt_GetFontOffsetForIndex(f->font_bytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info, f->font_bytes.data(), offset)) {
        return false;
    }
    out.width = PagedFont::kPageWidth;
    out.height = PagedFont::kPageHeight;
    out.rgba.assign(std::size_t{out.width} * out.height * 4u, 0);
    for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        out.rgba[i] = 0xFF;
        out.rgba[i + 1] = 0xFF;
        out.rgba[i + 2] = 0xFF;
    }
    std::vector<std::uint8_t> cov;
    for (const auto& pg : f->pages[page]) {
        cov.assign(std::size_t{pg.w} * pg.h, 0);
        stbtt_MakeGlyphBitmap(&info, cov.data(), pg.w, pg.h, pg.w, pg.scale, pg.scale, pg.glyph_index);
        for (std::uint32_t y = 0; y < pg.h; ++y) {
            for (std::uint32_t x = 0; x < pg.w; ++x) {
                const std::size_t dst = (std::size_t{pg.ay_in_page + y} * out.width + pg.ax + x) * 4u + 3u;
                out.rgba[dst] = cov[std::size_t{y} * pg.w + x];
            }
        }
    }
    return true;
}

} // namespace za
