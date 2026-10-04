// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH game fonts: bfttf decode, glyph atlases for the package font, text/glyph images for the UI
// art recipes, BFFNT digit fonts. Format notes: acnh_font.h.

#include "acnh_flavor.h"
#include "acnh_font.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>

#include "acnh_bytes.h"
#include "acnh_draw.h"
#include "acnh_msbt.h"
#include "acnh_romfs.h"
#include "acnh_tex.h"
#include "core/mods/modules/dsmod_module_sdk.h"

// stb_truetype, private to this translation unit (static functions). Contraction off so the
// rasteriser's float math is identical on x86-64 and AArch64.
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

namespace acnh {
namespace {
constexpr uint32_t BfttfMagic = 0x7F9A0218;
constexpr std::string_view FontArchive = "Font/ScalableFont.sarc.zs";
constexpr uint32_t MinCap = 8, MaxCap = 96, DefaultCap = 30;
constexpr uint32_t MaxAtlas = 4096;
constexpr uint64_t MaxAtlasBytes = 16ull * 1024 * 1024;
constexpr uint32_t Pad = 1;

std::string_view FaceMember(std::string_view face) {
    if (face == "ui")
        return "nintendoP_Seurat-B.bfttf";
    if (face == "ui_cn")
        return "DFP_GB_Y9_0.bfttf";
    if (face == "ui_tw")
        return "DFPT_Y8.bfttf";
    if (face == "ui_kr")
        return "AsiaKSDNR-B.bfttf";
    if (face == "talk")
        return "nintendoP_RodinNTLG-EB_003_Park.bfotf";
    if (face == "parkext")
        return "ParkExt.bfttf";
    return {};
}

bool LatinSet(uint32_t cp) {
    return (cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0x24F) ||
           (cp >= 0x370 && cp <= 0x4FF) || (cp >= 0x1E00 && cp <= 0x1EFF) ||
           (cp >= 0x2000 && cp <= 0x27BF) || (cp >= 0xE000 && cp <= 0xE2FF);
}

/// Same substitutions as the host's FallbackCodepoint (mod_ui_text.cpp), used for run slots the
/// face lacks (a slot in the run shadows the host's own fallback).
uint32_t Fallback(uint32_t c) {
    static constexpr std::string_view latin1 =
        "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTSaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (c >= 0xC0 && c <= 0xFF)
        return static_cast<unsigned char>(latin1[c - 0xC0]);
    switch (c) {
    case 0x2018:
    case 0x2019:
        return '\'';
    case 0x201C:
    case 0x201D:
        return '"';
    case 0x2013:
    case 0x2014:
        return '-';
    case 0x2026:
        return '.';
    case 0x00A0:
        return ' ';
    default:
        return 0;
    }
}

struct Raster {
    uint32_t cp{};
    int w{}, h{}, x0{}, y0{}, advance{};
    std::vector<uint8_t> coverage;
    uint32_t ax{}, ay{};
};
} // namespace

struct Fonts::Face {
    std::vector<uint8_t> plain;
    stbtt_fontinfo info{};
    int units_per_em = 1000;
    float cap_units = 0; ///< top of 'H' in font units
};

std::vector<uint8_t> DecodeBfttf(std::span<const uint8_t> file, uint32_t* key_out) {
    if (file.size() < 12)
        return {};
    const uint32_t key = dsmod_sdk::Be32(file.data()) ^ BfttfMagic;
    const uint32_t size = dsmod_sdk::Be32(file.data() + 4) ^ key;
    if (size == 0 || size > file.size() - 8)
        return {};
    std::vector<uint8_t> out(size);
    const uint8_t k[4] = {static_cast<uint8_t>(key >> 24), static_cast<uint8_t>(key >> 16),
                          static_cast<uint8_t>(key >> 8), static_cast<uint8_t>(key)};
    for (size_t i = 0; i < size; ++i)
        out[i] = file[8 + i] ^ k[i & 3];
    if (key_out)
        *key_out = key;
    return out;
}

bool BmpFont::Glyph(uint32_t cp, std::vector<uint8_t>& alpha) const {
    const auto it = cmap.find(cp);
    if (it == cmap.end() || cells_per_row <= 0)
        return false;
    const int i = it->second;
    const int x0 = (i % cells_per_row) * (cell_w + 1), y0 = (i / cells_per_row) * (cell_h + 1);
    alpha.assign(size_t(cell_w) * cell_h, 0);
    for (int y = 0; y < cell_h; ++y)
        for (int x = 0; x < cell_w; ++x)
            if (x0 + x < sheet.w && y0 + y < sheet.h)
                alpha[size_t(y) * cell_w + x] = sheet.At(x0 + x, y0 + y)[3];
    return true;
}

Fonts::Fonts(Romfs& r) : romfs{r} {}

void Fonts::SetLanguageCodepoints(
    std::function<std::vector<uint32_t>(std::string_view lang)> provider) {
    std::scoped_lock lock{mutex};
    lang_cps = std::move(provider);
}
Fonts::~Fonts() = default;

std::shared_ptr<const Fonts::Face> Fonts::LoadFace(std::string_view face) {
    {
        std::scoped_lock lock{mutex};
        if (const auto it = faces.find(face); it != faces.end())
            return it->second;
    }
    const std::string_view member = FaceMember(face);
    if (member.empty())
        return nullptr;
    std::vector<uint8_t> sarc, file;
    if (!romfs.ReadZs(FontArchive, sarc) || !Romfs::SarcFind(sarc, member, file))
        return nullptr;
    sarc = {};
    auto f = std::make_shared<Face>();
    f->plain = DecodeBfttf(file);
    if (f->plain.size() < 12)
        return nullptr;
    const int offset = stbtt_GetFontOffsetForIndex(f->plain.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&f->info, f->plain.data(), offset))
        return nullptr;
    // unitsPerEm (stb exposes it as the em-mapping scale)
    f->units_per_em =
        static_cast<int>(std::lround(1.0 / stbtt_ScaleForMappingEmToPixels(&f->info, 1.0f)));
    for (const int probe : {'H', 'E', 'I'}) {
        int x0, y0, x1, y1;
        const int gi = stbtt_FindGlyphIndex(&f->info, probe);
        if (gi != 0 && stbtt_GetGlyphBox(&f->info, gi, &x0, &y0, &x1, &y1) && y1 > 0) {
            f->cap_units = static_cast<float>(y1);
            break;
        }
    }
    if (f->cap_units <= 0) {
        int a, d, g;
        stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
        f->cap_units = static_cast<float>(a) * 0.7f;
    }
    if (f->units_per_em <= 0 || !(f->cap_units > 0)) // every scale divides by these
        return nullptr;
    std::scoped_lock lock{mutex};
    faces[std::string{face}] = f;
    return f;
}

std::optional<std::string> Fonts::CanonicalSpec(std::string_view spec) {
    std::string_view parts[3];
    int n = 0;
    while (n < 3) {
        const auto colon = spec.find(':');
        parts[n++] = spec.substr(0, colon);
        if (colon == std::string_view::npos)
            break;
        spec.remove_prefix(colon + 1);
    }
    if (FaceMember(parts[0]).empty() || parts[0] == "parkext" || parts[0].starts_with("ui_"))
        return std::nullopt;
    uint32_t cap = DefaultCap;
    if (n >= 2 && !parts[1].empty()) {
        const auto [p, ec] =
            std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), cap);
        if (ec != std::errc{} || p != parts[1].data() + parts[1].size() || cap < MinCap ||
            cap > MaxCap)
            return std::nullopt;
    }
    std::string set = "latin";
    if (n >= 3) {
        const bool lang =
            parts[2].size() == 4 && std::all_of(parts[2].begin(), parts[2].end(), [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            });
        if (parts[2] != "latin" && parts[2] != "all" && parts[2] != "flavor" && !lang)
            return std::nullopt;
        set = parts[2];
    }
    return std::string{parts[0]} + ":" + std::to_string(cap) + ":" + set;
}

std::shared_ptr<const FontAtlas> Fonts::Atlas(std::string_view spec_text) {
    const auto spec = CanonicalSpec(spec_text);
    if (!spec)
        return nullptr;
    {
        std::scoped_lock lock{mutex};
        if (const auto it = atlases.find(*spec); it != atlases.end())
            return it->second;
    }
    const std::string face_name = spec->substr(0, spec->find(':'));
    const uint32_t cap = static_cast<uint32_t>(std::stoul(spec->substr(face_name.size() + 1)));
    const bool all = spec->ends_with(":all");
    const std::string set = spec->substr(spec->rfind(':') + 1);
    const bool lang_set = set != "latin" && set != "all";
    std::vector<uint32_t> extra;
    if (lang_set) {
        if (!lang_cps)
            return nullptr;
        // memoised per set: the provider parses every text of a language, and the module steps
        // the cap down through several specs of the same set (01006F8002326000.cpp)
        {
            std::scoped_lock lock{mutex};
            if (const auto it = lang_sets.find(set); it != lang_sets.end())
                extra = it->second;
        }
        if (extra.empty()) {
            extra = lang_cps(set);
            std::sort(extra.begin(), extra.end());
            if (!extra.empty()) {
                std::scoped_lock lock{mutex};
                lang_sets.emplace(set, extra);
            }
        }
    }
    const auto main = LoadFace(face_name);
    const auto ext = LoadFace("parkext");
    // "flavor" (acnh_flavor.h, the text-only font): every language's flavour lines, each CJK code
    // point from the face of the language whose lines use it (Hangul KRko, Han only in CNzh / TWzh
    // lines from those faces, Japanese lines from Seurat-B)
    const bool flavor_set = set == "flavor";
    const auto cjk = LoadFace(set == "CNzh"   ? "ui_cn"
                              : set == "TWzh" ? "ui_tw"
                              : set == "KRko" ? "ui_kr"
                                              : "");
    std::shared_ptr<const Face> fl_cn, fl_tw, fl_kr;
    if (flavor_set) {
        fl_cn = LoadFace("ui_cn");
        fl_tw = LoadFace("ui_tw");
        fl_kr = LoadFace("ui_kr");
    }
    if (!main)
        return nullptr;
    const float scale = static_cast<float>(cap) / main->cap_units;
    const float em_px = scale * static_cast<float>(main->units_per_em);
    const float ext_scale = ext ? em_px / static_cast<float>(ext->units_per_em) : 0.0f;

    std::vector<Raster> rasters;
    std::map<uint32_t, size_t> by_cp;
    const uint32_t last = all || lang_set ? 0xFFFF : 0xE2FF;
    const float cjk_scale = cjk ? em_px / static_cast<float>(cjk->units_per_em) : 0.0f;
    for (uint32_t cp = 0x20; cp <= last; ++cp) {
        if (!all && !LatinSet(cp) &&
            !(lang_set && std::binary_search(extra.begin(), extra.end(), cp)))
            continue;
        // ParkExt is the first face of every font pack; it wins for its symbols and picture
        // glyphs (U+2000 up). Below that the main face is used (ParkExt has no Latin).
        const Face* f = nullptr;
        float s = 0;
        int gi = 0;
        if (ext && cp >= 0x2000 &&
            (gi = stbtt_FindGlyphIndex(&ext->info, static_cast<int>(cp))) != 0) {
            f = ext.get();
            s = ext_scale;
        } else if (cjk && cp >= 0x2E80 &&
                   (gi = stbtt_FindGlyphIndex(&cjk->info, static_cast<int>(cp))) != 0) {
            f = cjk.get();
            s = cjk_scale;
        } else if (const Face* fl = !flavor_set                            ? nullptr
                                    : flavor::CjkFaceFor(cp) == Lang::KRko ? fl_kr.get()
                                    : flavor::CjkFaceFor(cp) == Lang::CNzh ? fl_cn.get()
                                    : flavor::CjkFaceFor(cp) == Lang::TWzh ? fl_tw.get()
                                                                           : nullptr;
                   fl && (gi = stbtt_FindGlyphIndex(&fl->info, static_cast<int>(cp))) != 0) {
            f = fl;
            s = em_px / static_cast<float>(fl->units_per_em);
        } else if ((gi = stbtt_FindGlyphIndex(&main->info, static_cast<int>(cp))) != 0) {
            f = main.get();
            s = scale;
        } else if (cjk && (gi = stbtt_FindGlyphIndex(&cjk->info, static_cast<int>(cp))) != 0) {
            f = cjk.get();
            s = cjk_scale;
        }
        if (!f)
            continue;
        Raster r;
        r.cp = cp;
        int adv, lsb, x1, y1;
        stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
        r.advance = static_cast<int>(std::lround(static_cast<float>(adv) * s));
        stbtt_GetGlyphBitmapBox(&f->info, gi, s, s, &r.x0, &r.y0, &x1, &y1);
        r.w = std::max(0, x1 - r.x0);
        r.h = std::max(0, y1 - r.y0);
        if (r.w > 0 && r.h > 0 && r.w <= 512 && r.h <= 512) {
            r.coverage.assign(size_t(r.w) * r.h, 0);
            stbtt_MakeGlyphBitmap(&f->info, r.coverage.data(), r.w, r.h, r.w, s, s, gi);
            if (std::all_of(r.coverage.begin(), r.coverage.end(),
                            [](uint8_t v) { return v == 0; })) {
                r.w = r.h = 0;
                r.coverage.clear();
            }
        } else {
            r.w = r.h = 0;
        }
        by_cp.emplace(cp, rasters.size());
        rasters.push_back(std::move(r));
    }
    if (!by_cp.contains('A'))
        return nullptr;
    // Shelf pack in code point order into the smallest power-of-two width (height <= 2x width),
    // no glyph across a page of AtlasPageRows rows.
    uint32_t width = 0, height = 0;
    for (uint32_t w = 256; w <= MaxAtlas; w *= 2) {
        uint32_t x = 0, y = 0, shelf = 0;
        bool fits = true;
        for (auto& r : rasters) {
            if (r.w == 0)
                continue;
            const uint32_t cw = uint32_t(r.w) + 2 * Pad, ch = uint32_t(r.h) + 2 * Pad;
            if (cw > w) {
                fits = false;
                break;
            }
            if (x + cw > w) {
                x = 0;
                y += shelf;
                shelf = 0;
            }
            if (y % AtlasPageRows + ch > AtlasPageRows) { // would cross a page: next shelf/page
                if (x > 0) {
                    x = 0;
                    y += shelf;
                    shelf = 0;
                }
                if (y % AtlasPageRows + ch > AtlasPageRows)
                    y += AtlasPageRows - y % AtlasPageRows;
            }
            r.ax = x + Pad;
            r.ay = y + Pad;
            x += cw;
            shelf = std::max(shelf, ch);
        }
        const uint32_t h = y + shelf;
        // the host takes module images up to 4096 x 4096 and 16 MiB of RGBA
        if (fits && h <= std::min(MaxAtlas, 2 * w) &&
            uint64_t{w} * std::max<uint32_t>(h, 1) * 4 <= MaxAtlasBytes) {
            width = w;
            height = std::max<uint32_t>(h, 1);
            break;
        }
    }
    if (!width)
        return nullptr;
    auto out = std::make_shared<FontAtlas>();
    out->spec = *spec;
    out->atlas = Image{static_cast<int>(width), static_cast<int>(height)};
    for (size_t i = 0; i < out->atlas.rgba.size(); i += 4)
        out->atlas.rgba[i] = out->atlas.rgba[i + 1] = out->atlas.rgba[i + 2] = 0xFF;
    for (const auto& r : rasters)
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x)
                out->atlas.At(static_cast<int>(r.ax) + x, static_cast<int>(r.ay) + y)[3] =
                    r.coverage[size_t(y) * r.w + x];
    const auto glyph_of = [](const Raster& r) {
        EdenDsmodFontGlyph g{};
        if (r.w > 0) {
            g.x = static_cast<uint16_t>(r.ax);
            g.y = static_cast<uint16_t>(r.ay);
            g.w = static_cast<uint16_t>(r.w);
            g.h = static_cast<uint16_t>(r.h);
            g.bearing_x = static_cast<int16_t>(r.x0);
            g.bearing_y = static_cast<int16_t>(-r.y0);
        }
        g.advance = static_cast<uint16_t>(std::max(0, r.advance));
        return g;
    };
    EdenDsmodFontGlyph blank{};
    if (const auto it = by_cp.find(' '); it != by_cp.end())
        blank.advance = glyph_of(rasters[it->second]).advance;
    const uint32_t end = by_cp.rbegin()->first;
    out->line_height = cap;
    out->first_codepoint = 0x20;
    out->glyphs.assign(end - 0x20 + 1, blank);
    for (uint32_t cp = 0x20; cp <= end; ++cp) {
        auto& slot = out->glyphs[cp - 0x20];
        if (const auto it = by_cp.find(cp); it != by_cp.end())
            slot = glyph_of(rasters[it->second]);
        else if (const uint32_t alt = Fallback(cp); alt != 0)
            if (const auto a = by_cp.find(alt); a != by_cp.end())
                slot = glyph_of(rasters[a->second]);
    }
    out->rendered = rasters.size();
    std::scoped_lock lock{mutex};
    if (const auto it = atlases.find(*spec); it != atlases.end())
        return it->second;
    atlases[*spec] = out;
    return out;
}

std::optional<int> Fonts::UiAdvance(uint32_t cap, uint32_t cp) {
    const auto main = LoadFace("ui");
    if (!main)
        return std::nullopt;
    const auto ext = cp >= 0x2000 ? LoadFace("parkext") : nullptr;
    const float scale = static_cast<float>(cap) / main->cap_units;
    const float em_px = scale * static_cast<float>(main->units_per_em);
    int gi = 0, adv = 0, lsb = 0;
    if (ext && (gi = stbtt_FindGlyphIndex(&ext->info, static_cast<int>(cp))) != 0) {
        stbtt_GetGlyphHMetrics(&ext->info, gi, &adv, &lsb);
        return static_cast<int>(
            std::lround(static_cast<float>(adv) * em_px / static_cast<float>(ext->units_per_em)));
    }
    if ((gi = stbtt_FindGlyphIndex(&main->info, static_cast<int>(cp))) != 0) {
        stbtt_GetGlyphHMetrics(&main->info, gi, &adv, &lsb);
        return static_cast<int>(std::lround(static_cast<float>(adv) * scale));
    }
    return cp >= 0x2E80 ? static_cast<int>(std::lround(em_px)) : -1;
}

bool Fonts::Text(std::string_view face_name, std::string_view utf8, double em_px, Rgba colour,
                 Image& out) {
    const auto face = LoadFace(face_name);
    if (!face || em_px <= 0 || em_px > 1024)
        return false;
    const float s = static_cast<float>(em_px) / static_cast<float>(face->units_per_em);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&face->info, &ascent, &descent, &gap);
    struct Placed {
        int x, y, w, h;
        std::vector<uint8_t> cov;
    };
    std::vector<Placed> glyphs;
    float pen = 0;
    const int base = static_cast<int>(std::ceil(ascent * s)) + 2;
    // A CJK face (DFP_GB / DFPT / AsiaKSDNR) has no Latin: what it lacks comes from Seurat-B at the
    // same em size, as the atlas takes everything below the CJK range from the main face (a Latin
    // villager name on a CNzh / TWzh / KRko island drew no text at all)
    const auto fallback = face_name == "ui_cn" || face_name == "ui_tw" || face_name == "ui_kr"
                              ? LoadFace("ui")
                              : nullptr;
    const float fs = fallback ? static_cast<float>(em_px) / static_cast<float>(fallback->units_per_em) : 0;
    for (size_t i = 0; i < utf8.size();) {
        const char32_t cp = NextCodepoint(utf8, i);
        const stbtt_fontinfo* info = &face->info;
        float gs = s;
        int gi = stbtt_FindGlyphIndex(info, static_cast<int>(cp));
        if (!gi && fallback && (gi = stbtt_FindGlyphIndex(&fallback->info, static_cast<int>(cp))) != 0) {
            info = &fallback->info;
            gs = fs;
        }
        if (!gi)
            continue;
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
        stbtt_GetGlyphBitmapBox(info, gi, gs, gs, &x0, &y0, &x1, &y1);
        Placed p{static_cast<int>(std::floor(pen)) + x0, base + y0, x1 - x0, y1 - y0, {}};
        if (p.w > 0 && p.h > 0 && p.w <= 2048 && p.h <= 2048) {
            p.cov.assign(size_t(p.w) * p.h, 0);
            stbtt_MakeGlyphBitmap(info, p.cov.data(), p.w, p.h, p.w, gs, gs, gi);
            glyphs.push_back(std::move(p));
        }
        pen += static_cast<float>(adv) * gs;
    }
    if (glyphs.empty())
        return false;
    int minx = INT32_MAX, miny = INT32_MAX, maxx = INT32_MIN, maxy = INT32_MIN;
    for (const auto& g : glyphs)
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
                if (g.cov[size_t(y) * g.w + x] > 2) {
                    minx = std::min(minx, g.x + x);
                    miny = std::min(miny, g.y + y);
                    maxx = std::max(maxx, g.x + x);
                    maxy = std::max(maxy, g.y + y);
                }
    // within the host's image limit before allocating (a long string at a large em size)
    if (maxx < minx || maxx - minx >= 4096 || maxy - miny >= 4096)
        return false;
    out = Image{maxx - minx + 1, maxy - miny + 1};
    for (const auto& g : glyphs)
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x) {
                const int ox = g.x + x - minx, oy = g.y + y - miny;
                if (ox < 0 || oy < 0 || ox >= out.w || oy >= out.h)
                    continue;
                uint8_t* p = out.At(ox, oy);
                const uint8_t a = static_cast<uint8_t>(g.cov[size_t(y) * g.w + x] * colour.a / 255);
                if (a >= p[3]) {
                    p[0] = colour.r;
                    p[1] = colour.g;
                    p[2] = colour.b;
                    p[3] = a;
                }
            }
    return true;
}

double Fonts::TextWidth(std::string_view face_name, std::string_view utf8, double em_px) {
    const auto face = LoadFace(face_name);
    if (!face)
        return 0;
    const double s = em_px / face->units_per_em;
    double w = 0;
    for (size_t i = 0; i < utf8.size();) {
        const char32_t cp = NextCodepoint(utf8, i);
        const int gi = stbtt_FindGlyphIndex(&face->info, static_cast<int>(cp));
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&face->info, gi, &adv, &lsb);
        w += adv * s;
    }
    return w;
}

std::shared_ptr<const BmpFont> Fonts::Bitmap(std::string_view name) {
    {
        std::scoped_lock lock{mutex};
        if (const auto it = bitmaps.find(name); it != bitmaps.end())
            return it->second;
    }
    const auto arc = romfs.SarcFile("Font/BmpFont_US.sarc.zs");
    if (!arc)
        return nullptr;
    const auto* b = arc->FindBase(std::string{name} + ".bffnt");
    if (!b || b->size() < 0x40 || std::memcmp(b->data(), "FFNT", 4) != 0)
        return nullptr;
    const auto& d = *b;
    const auto u16 = [&](size_t o) { return bytes::Le16(d, o); };
    const auto u32 = [&](size_t o) { return bytes::Le32(d, o); };
    const size_t finf = u16(6);
    if (finf + 0x20 > d.size() || std::memcmp(d.data() + finf, "FINF", 4) != 0)
        return nullptr;
    const size_t tglp = u32(finf + 0x14) >= 8 ? u32(finf + 0x14) - 8 : 0;
    if (tglp + 0x20 > d.size() || std::memcmp(d.data() + tglp, "TGLP", 4) != 0)
        return nullptr;
    auto f = std::make_shared<BmpFont>();
    f->cell_w = d[tglp + 8];
    f->cell_h = d[tglp + 9];
    f->cells_per_row = static_cast<int>(u16(tglp + 20));
    const size_t sheet_size = u32(tglp + 12), data = u32(tglp + 28);
    if (f->cell_w == 0 || f->cell_h == 0 || f->cells_per_row == 0 || data >= d.size() ||
        sheet_size > d.size() - data)
        return nullptr;
    Image sheet;
    if (!BntxDecodeAny(d.data() + data, sheet_size, {}, sheet))
        return nullptr;
    f->sheet = draw::FlipV(sheet); // stored bottom-up
    // CMAP chain from FINF.ptrMap (each pointer = section start + 8)
    size_t ptr = u32(finf + 0x1C);
    for (int guard = 0; ptr >= 8 && ptr <= d.size() && d.size() - ptr >= 0x10 && guard < 64;
         ++guard) {
        const size_t c = ptr - 8;
        if (std::memcmp(d.data() + c, "CMAP", 4) != 0)
            break;
        const uint32_t cb = u32(c + 8), ce = u32(c + 12);
        const uint32_t method = u16(c + 16);
        const size_t q = c + 0x18;
        if (method == 0) {
            const uint32_t off = u16(q);
            for (uint32_t k = cb; k <= ce && k - cb < 0x10000; ++k)
                f->cmap[k] = static_cast<int>(off + k - cb);
        } else if (method == 1) {
            for (uint32_t k = cb; k <= ce && k - cb < 0x10000; ++k) {
                const uint32_t i = u16(q + 2 * (k - cb));
                if (i != 0xFFFF)
                    f->cmap[k] = static_cast<int>(i);
            }
        } else if (method == 2) {
            const uint32_t n = u16(q);
            for (uint32_t k = 0; k < n && q + 4 + 8 * k + 6 <= d.size(); ++k)
                f->cmap[u32(q + 4 + 8 * k)] = static_cast<int>(u16(q + 4 + 8 * k + 4));
        }
        ptr = u32(c + 20);
    }
    if (f->cmap.empty())
        return nullptr;
    std::scoped_lock lock{mutex};
    bitmaps[std::string{name}] = f;
    return f;
}

bool Fonts::Number(std::string_view font, std::string_view text, double h, Rgba fill,
                   std::optional<Rgba> outline, Image& out) {
    const auto f = Bitmap(font);
    if (!f || text.empty() || h <= 0)
        return false;
    // Ink-cropped glyph columns, overlapping 2 px (acnh_mock.BmpFont.render).
    std::vector<std::pair<int, std::vector<float>>> gl; // width, column-major? row-major (h x w)
    for (const char c : text) {
        std::vector<uint8_t> a;
        if (!f->Glyph(static_cast<unsigned char>(c), a))
            continue;
        int c0 = f->cell_w, c1 = -1;
        for (int x = 0; x < f->cell_w; ++x) {
            float mx = 0;
            for (int y = 0; y < f->cell_h; ++y)
                mx = std::max(mx, a[size_t(y) * f->cell_w + x] / 255.0f);
            if (mx > 0.05f) {
                c0 = std::min(c0, x);
                c1 = std::max(c1, x);
            }
        }
        if (c1 < 0) {
            c0 = 0;
            c1 = f->cell_w / 3 - 1;
        }
        const int w = c1 - c0 + 1;
        std::vector<float> g(size_t(w) * f->cell_h);
        for (int y = 0; y < f->cell_h; ++y)
            for (int x = 0; x < w; ++x)
                g[size_t(y) * w + x] = a[size_t(y) * f->cell_w + c0 + x] / 255.0f;
        gl.emplace_back(w, std::move(g));
    }
    if (gl.empty())
        return false;
    int wsum = -2 * static_cast<int>(gl.size() - 1);
    for (const auto& g : gl)
        wsum += g.first;
    std::vector<float> a(size_t(std::max(1, wsum)) * f->cell_h, 0.0f);
    int x = 0;
    for (const auto& [w, g] : gl) {
        for (int y = 0; y < f->cell_h; ++y)
            for (int k = std::max(0, -x); k < w && x + k < wsum; ++k) { // 1 px glyphs step back
                float& v = a[size_t(y) * wsum + x + k];
                v = std::max(v, g[size_t(y) * w + k]);
            }
        x += w - 2;
    }
    Image im{std::max(1, wsum), f->cell_h};
    for (int y = 0; y < f->cell_h; ++y)
        for (int xx = 0; xx < wsum; ++xx) {
            const float v = a[size_t(y) * wsum + xx];
            float r, g, b, al;
            if (outline) {
                const float t = std::clamp((v - 0.55f) / 0.35f, 0.0f, 1.0f);
                r = outline->r + (fill.r - outline->r) * t;
                g = outline->g + (fill.g - outline->g) * t;
                b = outline->b + (fill.b - outline->b) * t;
                al = std::clamp(v * 1.8f, 0.0f, 1.0f) *
                     (outline->a / 255.0f * (1 - t) + fill.a / 255.0f * t);
            } else {
                r = fill.r;
                g = fill.g;
                b = fill.b;
                al = v * fill.a / 255.0f;
            }
            uint8_t* p = im.At(xx, y);
            p[0] = static_cast<uint8_t>(r);
            p[1] = static_cast<uint8_t>(g);
            p[2] = static_cast<uint8_t>(b);
            p[3] = static_cast<uint8_t>(al * 255);
        }
    im = draw::CropAlpha(im, 2);
    out = draw::Resize(im, static_cast<int>(std::nearbyint(im.w * h / im.h)),
                       static_cast<int>(std::nearbyint(h)));
    return true;
}

} // namespace acnh
