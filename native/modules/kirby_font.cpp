// SPDX-License-Identifier: GPL-3.0-or-later
// Rasteriser adapted from eden-duo-companions native/modules/wonder_font.cpp (Eden DSMod fork).

#include "kirby_font.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

// stb_truetype, private to this translation unit (static functions, no exported symbols that
// could clash with another module or the host). Contraction off so the rasteriser's float math
// is the same on x86-64 and AArch64 (clang would otherwise fuse multiply-adds on ARM).
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

namespace KirbyFont {
namespace {

constexpr std::uint32_t BfotfMagic = 0x7F9A0218;
constexpr std::uint32_t FirstCodepoint = 0x20;
constexpr std::uint32_t LastCodepoint = 0x2026; // U+2026 ellipsis; U+2022 bullet sits inside
constexpr std::uint32_t DefaultCap = 30;
constexpr std::uint32_t MinCap = 6, MaxCap = 160;
constexpr std::uint32_t MaxAtlas = 2048;
constexpr std::uint32_t Pad = 1; // transparent border around every glyph box
// the layouts' outlined fonts (the -OL-M- .bfcpx): a 6-unit border at their 64-unit size, in the material's black
constexpr float EdgeOfEm = 6.0f / 64;

std::uint32_t Be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}

bool Rendered(std::uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E)
        return true;
    if (cp >= 0xA0 && cp <= 0xFF)
        return true;
    switch (cp) {
    case 0x2013: // en dash
    case 0x2014: // em dash
    case 0x2018:
    case 0x2019:
    case 0x201C:
    case 0x201D:
    case 0x2022: // bullet
    case 0x2026: // ellipsis
        return true;
    default:
        return false;
    }
}

/// Same substitutions as the host's FallbackCodepoint (mod_ui_text.cpp), used for run slots the
/// font lacks: a slot in the run shadows the host's own fallback, so it has to do the same job.
std::uint32_t Fallback(std::uint32_t c) {
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

struct Spec {
    std::string member; // scft base name
    std::uint32_t cap{DefaultCap};
    std::optional<std::uint32_t> edge; // RRGGBB: the glyphs white, edged in it
    std::string Canonical() const {
        char hex[8]{};
        if (edge)
            std::snprintf(hex, sizeof hex, "/%06X", *edge);
        return member + ":" + std::to_string(cap) + hex;
    }
};

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::optional<Spec> ParseSpec(std::string_view text) {
    text = Trim(text);
    Spec spec;
    if (const auto slash = text.rfind('/'); slash != std::string_view::npos) {
        const std::string_view hex = Trim(text.substr(slash + 1));
        std::uint32_t rgb{};
        const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), rgb, 16);
        if (hex.size() != 6 || ec != std::errc{} || end != hex.data() + hex.size())
            return std::nullopt;
        spec.edge = rgb;
        text = Trim(text.substr(0, slash));
    }
    std::string_view name = text;
    // "user:" is a source prefix, not the cap separator
    if (const auto colon = text.rfind(':');
        colon != std::string_view::npos && !(text.starts_with("user:") && colon == 4)) {
        name = Trim(text.substr(0, colon));
        const std::string_view num = Trim(text.substr(colon + 1));
        std::uint32_t cap{};
        const auto [end, ec] = std::from_chars(num.data(), num.data() + num.size(), cap);
        if (ec != std::errc{} || end != num.data() + num.size() || cap < MinCap || cap > MaxCap)
            return std::nullopt;
        spec.cap = cap;
    }
    const bool user = name.starts_with("user:");
    std::string_view base = user ? name.substr(5) : name;
    if (base.empty() || base.size() > 64)
        return std::nullopt;
    for (const char c : base) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.')
            return std::nullopt;
    }
    if (user && !base.ends_with(".ttf") && !base.ends_with(".otf"))
        return std::nullopt;
    if (name.find("..") != std::string_view::npos)
        return std::nullopt;
    spec.member = std::string{name};
    return spec;
}

struct Raster {
    std::uint32_t cp{};
    int w{}, h{}, x0{}, y0{}, advance{};
    std::vector<std::uint8_t> coverage;
    std::vector<std::uint8_t> rgba; // an edged glyph: white over its border
    std::uint32_t ax{}, ay{}; // atlas position of the box (inside the padding)
};

// The glyph white over a ring `width` px wide in `rgb` (every inked texel stamps a disc, soft on its last
// pixel), its box grown to hold the ring
void Edge(Raster& r, float width, std::uint32_t rgb) {
    const int m = static_cast<int>(std::ceil(width)) + 1, w = r.w + 2 * m, h = r.h + 2 * m;
    std::vector<std::pair<int, float>> disc;
    for (int dy = -m; dy <= m; ++dy)
        for (int dx = -m; dx <= m; ++dx)
            if (const float k = std::clamp(width + 0.5f - std::hypot(static_cast<float>(dx), static_cast<float>(dy)), 0.0f, 1.0f); k > 0)
                disc.emplace_back(dy * w + dx, k);
    std::vector<float> ring(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x)
            if (const float c = r.coverage[static_cast<std::size_t>(y) * r.w + x] / 255.0f; c > 0)
                for (const auto& [off, k] : disc) {
                    float& e = ring[static_cast<std::size_t>((y + m) * w + x + m + off)];
                    e = std::max(e, c * k);
                }
    const float edge[3]{static_cast<float>(rgb >> 16 & 0xFF), static_cast<float>(rgb >> 8 & 0xFF), static_cast<float>(rgb & 0xFF)};
    r.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x - m, sy = y - m;
            const float f = sx >= 0 && sy >= 0 && sx < r.w && sy < r.h ? r.coverage[static_cast<std::size_t>(sy) * r.w + sx] / 255.0f : 0;
            const float e = ring[static_cast<std::size_t>(y) * w + x] * (1 - f), a = f + e;
            if (a <= 0)
                continue;
            auto* px = &r.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c)
                px[c] = static_cast<std::uint8_t>(std::lround((255 * f + edge[c] * e) / a));
            px[3] = static_cast<std::uint8_t>(std::lround(255 * a));
        }
    r.x0 -= m, r.y0 -= m, r.w = w, r.h = h;
}

// One decoded font file serves every size asked of it (callers hold BuildFontAtlas' lock).
std::shared_ptr<const std::vector<std::uint8_t>> FontFile(const KirbyAssets::RomfsReader& read, const std::string& member) {
    static std::map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> files;
    auto& otf = files[member];
    if (!otf) {
        std::optional<std::vector<std::uint8_t>> bytes;
        if (member.starts_with("user:")) {
            bytes = read(member);
        } else {
            for (const char* ext : {".bfotf.cmp", ".bfttf.cmp"}) {
                const auto raw = read("/font/ScalableFontBin/" + member + ext);
                const auto plain = raw ? KirbyAssets::DecompressCmp(*raw) : std::nullopt;
                if (plain && (bytes = DecodeBfotf(*plain)))
                    break;
            }
        }
        if (!bytes || bytes->size() < 12) {
            files.erase(member);
            return nullptr;
        }
        otf = std::make_shared<const std::vector<std::uint8_t>>(std::move(*bytes));
    }
    return otf;
}

// The game's combination for `member` (font/Region/<region>/<member>-Scalable.bin, else STD's): its files in order, each
// with the codepoints it takes (none: any left), the 64 px variation's baseline offset (up) and bold width, and the
// vertical scale.
// The standard Kurokane takes D, J, V, f, j and v from FOT-RodinWanpakuPro-UB; Korean and Chinese draw almost
// everything from their own fonts.
using Ranges = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
struct Combination {
    std::vector<std::pair<std::string, Ranges>> files;
    float baseline{}, bold{}, scale_y{1};
};

std::optional<Combination> LoadCombination(const KirbyAssets::RomfsReader& read, const std::string& region,
                                           const std::string& member) {
    std::optional<std::vector<std::uint8_t>> bin;
    for (const std::string& r : {region, std::string{"STD"}})
        if ((bin = read("/font/Region/" + r + "/" + member + "-Scalable.bin")))
            break;
    const auto doc = bin ? KirbyAssets::ReadXbinYaml(*bin) : std::nullopt;
    const auto* setting = doc ? doc->Find("scalable_setting") : nullptr;
    const auto* defines = setting ? setting->Find("font_defines") : nullptr;
    if (!defines)
        return std::nullopt;
    Combination c;
    for (const auto& d : defines->list) {
        const auto* file = d.Find("file");
        if (!file || file->text.empty())
            continue;
        auto& ranges = c.files.emplace_back(file->text.substr(0, file->text.rfind('.')), Ranges{}).second;
        if (const auto* rs = d.Find("ranges"))
            for (const auto& r : rs->list)
                if (r.list.size() == 2)
                    ranges.emplace_back(static_cast<std::uint32_t>(r.list[0].number), static_cast<std::uint32_t>(r.list[1].number));
    }
    if (const auto* y = setting->Find("base_scale_y"); y && y->real > 0)
        c.scale_y = static_cast<float>(y->real);
    if (const auto* vs = setting->Find("variations"))
        for (const auto& v : vs->list)
            if (const auto* size = v.Find("size"); size && size->number == 64) {
                if (const auto* b = v.Find("baseline_offset"))
                    c.baseline = static_cast<float>(b->number);
                if (const auto* b = v.Find("bold_width"))
                    c.bold = static_cast<float>(b->number);
            }
    if (c.files.empty())
        return std::nullopt;
    return c;
}

std::mutex region_lock;
std::string region_folder = "STD";

} // namespace

struct GlyphSource {
    struct Face {
        std::shared_ptr<const std::vector<std::uint8_t>> file;
        stbtt_fontinfo info{};
        float scale{}; // px per font unit, every face at the same em
        Ranges ranges; // none: any codepoint
    };
    std::vector<Face> faces;
    float scale_y{1}, shift{}, bold{}, edge{};
    std::optional<std::uint32_t> edge_rgb;
};

namespace {

// strokes grown by `b` px to the right and down, as the game's bold width
void Embolden(Raster& r, float b) {
    const int m = static_cast<int>(std::ceil(b)), w = r.w + m, h = r.h + m;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float v = 0;
            for (int dy = 0; dy <= m; ++dy)
                for (int dx = 0; dx <= m; ++dx)
                    if (const int sx = x - dx, sy = y - dy; sx >= 0 && sy >= 0 && sx < r.w && sy < r.h)
                        v = std::max(v, r.coverage[static_cast<std::size_t>(sy) * r.w + sx] *
                                            std::clamp(b + 1 - static_cast<float>(std::max(dx, dy)), 0.0f, 1.0f));
            out[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(std::lround(v));
        }
    r.coverage = std::move(out), r.w = w, r.h = h;
}

// `cp` from the first face that takes it and has it
std::optional<Raster> Draw(const GlyphSource& src, std::uint32_t cp) {
    for (const auto& face : src.faces) {
        if (!face.ranges.empty() &&
            std::none_of(face.ranges.begin(), face.ranges.end(), [&](const auto& r) { return cp >= r.first && cp <= r.second; }))
            continue;
        const stbtt_fontinfo& font = face.info;
        const int gi = stbtt_FindGlyphIndex(&font, static_cast<int>(cp));
        if (gi == 0)
            continue;
        const float sc = face.scale, scy = face.scale * src.scale_y;
        Raster r;
        r.cp = cp;
        int adv, lsb, x1, y1;
        stbtt_GetGlyphHMetrics(&font, gi, &adv, &lsb);
        r.advance = static_cast<int>(std::lround(static_cast<float>(adv) * sc));
        // The box from the outline's own points: stb's box of a CFF glyph can miss parts of it (Kurokane's
        // J loses its hook)
        stbtt_vertex* verts = nullptr;
        const int count = stbtt_GetGlyphShape(&font, gi, &verts);
        float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
        for (int v = 0; v < count; ++v) {
            const auto take = [&](float x, float y) {
                lx = std::min(lx, x), hx = std::max(hx, x), ly = std::min(ly, y), hy = std::max(hy, y);
            };
            take(verts[v].x, verts[v].y);
            if (verts[v].type == STBTT_vcurve || verts[v].type == STBTT_vcubic)
                take(verts[v].cx, verts[v].cy);
            if (verts[v].type == STBTT_vcubic)
                take(verts[v].cx1, verts[v].cy1);
        }
        r.x0 = count > 0 ? static_cast<int>(std::floor(lx * sc)) : 0;
        r.y0 = count > 0 ? static_cast<int>(std::floor(-hy * scy)) : 0;
        x1 = count > 0 ? static_cast<int>(std::ceil(hx * sc)) : 0;
        y1 = count > 0 ? static_cast<int>(std::ceil(-ly * scy)) : 0;
        r.w = std::max(0, x1 - r.x0);
        r.h = std::max(0, y1 - r.y0);
        if (r.w > 1024 || r.h > 1024) {
            stbtt_FreeShape(&font, verts);
            return std::nullopt;
        }
        if (r.w > 0 && r.h > 0) {
            r.coverage.assign(static_cast<std::size_t>(r.w) * r.h, 0);
            stbtt__bitmap bitmap{r.w, r.h, r.w, r.coverage.data()};
            stbtt_Rasterize(&bitmap, 0.35f, verts, count, sc, scy, 0, 0, r.x0, r.y0, 1, font.userdata);
            // Nothing inked (a blank glyph with a box): keep advance only.
            if (std::all_of(r.coverage.begin(), r.coverage.end(), [](auto v) { return v == 0; })) {
                r.w = r.h = 0;
                r.coverage.clear();
            } else {
                if (src.bold > 0)
                    Embolden(r, src.bold);
                if (src.edge_rgb)
                    Edge(r, src.edge, *src.edge_rgb);
            }
        } else {
            r.w = r.h = 0;
        }
        r.y0 -= static_cast<int>(std::lround(src.shift));
        stbtt_FreeShape(&font, verts);
        return r;
    }
    return std::nullopt;
}

std::shared_ptr<const FontAtlas> Build(const KirbyAssets::RomfsReader& read, const Spec& spec, const std::string& region) {
    const auto otf = FontFile(read, spec.member);
    if (!otf)
        return nullptr;

    stbtt_fontinfo info{};
    const int offset = stbtt_GetFontOffsetForIndex(otf->data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info, otf->data(), offset))
        return nullptr;

    // Cap height in font units: the top of 'H' (fallbacks: 'E', 'I', then 70% of the ascent).
    float cap_units = 0.0f;
    for (const int probe : {'H', 'E', 'I'}) {
        int x0, y0, x1, y1;
        const int gi = stbtt_FindGlyphIndex(&info, probe);
        if (gi != 0 && stbtt_GetGlyphBox(&info, gi, &x0, &y0, &x1, &y1) && y1 > 0) {
            cap_units = static_cast<float>(y1);
            break;
        }
    }
    if (cap_units <= 0.0f) {
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
        cap_units = static_cast<float>(ascent) * 0.7f;
    }
    if (cap_units <= 0.0f)
        return nullptr;
    const float scale = static_cast<float>(spec.cap) / cap_units;
    const float em = scale / stbtt_ScaleForMappingEmToPixels(&info, 1);
    auto source = std::make_shared<GlyphSource>();
    source->edge = spec.edge ? em * EdgeOfEm : 0;
    source->edge_rgb = spec.edge;
    if (const auto combination = spec.member.starts_with("user:") ? std::nullopt : LoadCombination(read, region, spec.member)) {
        source->scale_y = combination->scale_y;
        source->shift = combination->baseline * em / 64;
        // (Korean and Chinese only: the other languages keep the strokes their pages were made with)
        if (region == "KOR" || region == "CHI" || region == "TWN")
            source->bold = combination->bold * em / 64;
        for (const auto& [name, ranges] : combination->files) {
            GlyphSource::Face face{FontFile(read, name), {}, 0, ranges};
            if (!face.file || !stbtt_InitFont(&face.info, face.file->data(), stbtt_GetFontOffsetForIndex(face.file->data(), 0)))
                continue;
            face.scale = face.file == otf ? scale : stbtt_ScaleForMappingEmToPixels(&face.info, em);
            source->faces.push_back(std::move(face));
        }
    }
    if (source->faces.empty())
        source->faces.push_back({otf, info, scale, {}});

    // Rasterise every rendered codepoint the fonts have, in codepoint order.
    std::vector<Raster> rasters;
    std::map<std::uint32_t, std::size_t> by_cp;
    for (std::uint32_t cp = FirstCodepoint; cp <= LastCodepoint; ++cp) {
        if (!Rendered(cp))
            continue;
        auto r = Draw(*source, cp);
        if (!r)
            continue;
        by_cp.emplace(cp, rasters.size());
        rasters.push_back(std::move(*r));
    }
    if (by_cp.find('A') == by_cp.end() && by_cp.find('0') == by_cp.end())
        return nullptr; // not a Latin font

    // Shelf pack in codepoint order into the smallest power-of-two width that stays square-ish.
    std::uint32_t width = 0, height = 0;
    for (std::uint32_t w = 128; w <= MaxAtlas; w *= 2) {
        std::uint32_t x = 0, y = 0, shelf = 0;
        bool fits = true;
        for (auto& r : rasters) {
            if (r.w == 0)
                continue;
            const std::uint32_t cw = static_cast<std::uint32_t>(r.w) + 2 * Pad;
            const std::uint32_t ch = static_cast<std::uint32_t>(r.h) + 2 * Pad;
            if (cw > w) {
                fits = false;
                break;
            }
            if (x + cw > w) {
                x = 0;
                y += shelf;
                shelf = 0;
            }
            r.ax = x + Pad;
            r.ay = y + Pad;
            x += cw;
            shelf = std::max(shelf, ch);
        }
        const std::uint32_t h = y + shelf;
        if (fits && h <= w) {
            width = w;
            height = std::max<std::uint32_t>(h, 1);
            break;
        }
    }
    if (width == 0)
        return nullptr;

    auto out = std::make_shared<FontAtlas>();
    out->atlas.width = width;
    out->atlas.height = height;
    out->atlas.rgba.assign(std::size_t{width} * height * 4, 0);
    for (std::size_t i = 0; i < out->atlas.rgba.size(); i += 4)
        out->atlas.rgba[i] = out->atlas.rgba[i + 1] = out->atlas.rgba[i + 2] = 0xFF;
    for (const auto& r : rasters) {
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) {
                auto* px = &out->atlas.rgba[((std::size_t{r.ay} + y) * width + r.ax + x) * 4];
                if (r.rgba.empty())
                    px[3] = r.coverage[static_cast<std::size_t>(y) * r.w + x];
                else
                    std::copy_n(&r.rgba[(static_cast<std::size_t>(y) * r.w + x) * 4], 4, px);
            }
    }

    const auto glyph_of = [](const Raster& r) {
        EdenDsmodFontGlyph g{};
        if (r.w > 0) {
            g.x = static_cast<std::uint16_t>(r.ax);
            g.y = static_cast<std::uint16_t>(r.ay);
            g.w = static_cast<std::uint16_t>(r.w);
            g.h = static_cast<std::uint16_t>(r.h);
            g.bearing_x = static_cast<std::int16_t>(r.x0);
            g.bearing_y = static_cast<std::int16_t>(-r.y0); // box top above the baseline
        }
        g.advance = static_cast<std::uint16_t>(std::max(0, r.advance));
        return g;
    };
    EdenDsmodFontGlyph blank{};
    if (const auto it = by_cp.find(' '); it != by_cp.end())
        blank.advance = glyph_of(rasters[it->second]).advance;
    else
        blank.advance = static_cast<std::uint16_t>((spec.cap + 1) / 2);

    out->line_height = spec.cap;
    {
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
        out->em = scale / stbtt_ScaleForMappingEmToPixels(&info, 1);
        out->ascent = static_cast<float>(ascent) * scale, out->descent = static_cast<float>(descent) * scale;
    }
    out->first_codepoint = FirstCodepoint;
    out->source = std::move(source);
    out->glyphs.resize(LastCodepoint - FirstCodepoint + 1, blank);
    for (std::uint32_t cp = FirstCodepoint; cp <= LastCodepoint; ++cp) {
        auto& slot = out->glyphs[cp - FirstCodepoint];
        if (const auto it = by_cp.find(cp); it != by_cp.end()) {
            slot = glyph_of(rasters[it->second]);
        } else if (const std::uint32_t alt = Fallback(cp); alt != 0) {
            if (const auto a = by_cp.find(alt); a != by_cp.end())
                slot = glyph_of(rasters[a->second]);
        }
    }
    return out;
}

} // namespace

std::optional<std::vector<std::uint8_t>> DecodeBfotf(std::span<const std::uint8_t> file) {
    if (file.size() < 8)
        return std::nullopt;
    const std::uint32_t key = Be32(file.data()) ^ BfotfMagic;
    const std::uint32_t size = Be32(file.data() + 4) ^ key;
    if (size == 0 || size > file.size() - 8)
        return std::nullopt;
    std::vector<std::uint8_t> out(size);
    const std::array<std::uint8_t, 4> k{static_cast<std::uint8_t>(key >> 24),
                                        static_cast<std::uint8_t>(key >> 16),
                                        static_cast<std::uint8_t>(key >> 8),
                                        static_cast<std::uint8_t>(key)};
    for (std::size_t i = 0; i < size; ++i)
        out[i] = file[8 + i] ^ k[i & 3];
    return out;
}

void SetRegion(std::string_view message_dir) {
    static constexpr std::pair<std::string_view, std::string_view> Folders[]{
        {"JP_Japanese", "STD"},   {"KR_Korean", "KOR"},     {"CN_Chinese", "CHI"},       {"TW_Chinese", "TWN"},
        {"US_English", "US_ENG"}, {"EU_English", "EU_ENG"}, {"US_French", "US_FRA"},     {"EU_French", "EU_FRA"},
        {"EU_German", "EU_GER"},  {"EU_Italian", "EU_ITA"}, {"US_Spanish", "US_SPA"},    {"EU_Spanish", "EU_SPA"},
        {"EU_Dutch", "EU_NLD"},   {"US_Portuguese", "US_POR"},
    };
    std::string_view folder = "STD";
    for (const auto& [dir, f] : Folders)
        if (dir == message_dir)
            folder = f;
    std::lock_guard guard{region_lock};
    region_folder = folder;
}

FontAtlas::Ref FontAtlas::Find(char32_t c) const {
    const bool dense = c >= first_codepoint && c - first_codepoint < glyphs.size();
    if (dense && (Rendered(c) || !source))
        return {&glyphs[c - first_codepoint], &atlas};
    if (!source)
        return {};
    std::lock_guard guard{lock};
    auto it = drawn.find(c);
    if (it == drawn.end()) {
        std::optional<std::pair<EdenDsmodFontGlyph, KirbyAssets::Image>> made;
        if (const auto r = Draw(*source, c)) {
            const std::size_t texels = static_cast<std::size_t>(r->w) * static_cast<std::size_t>(r->h);
            KirbyAssets::Image image{static_cast<std::uint32_t>(r->w), static_cast<std::uint32_t>(r->h),
                                     std::vector<std::uint8_t>(texels * 4, 0xFF)};
            for (std::size_t i = 0; i < texels; ++i)
                if (r->rgba.empty())
                    image.rgba[i * 4 + 3] = r->coverage[i];
                else
                    std::copy_n(&r->rgba[i * 4], 4, &image.rgba[i * 4]);
            EdenDsmodFontGlyph g{};
            g.w = static_cast<std::uint16_t>(r->w);
            g.h = static_cast<std::uint16_t>(r->h);
            g.bearing_x = static_cast<std::int16_t>(r->x0);
            g.bearing_y = static_cast<std::int16_t>(-r->y0);
            g.advance = static_cast<std::uint16_t>(std::max(0, r->advance));
            made.emplace(g, std::move(image));
        }
        it = drawn.emplace(c, std::move(made)).first;
    }
    if (it->second)
        return {&it->second->first, &it->second->second};
    return dense ? Ref{&glyphs[c - first_codepoint], &atlas} : Ref{};
}

std::shared_ptr<const FontAtlas> BuildFontAtlas(const KirbyAssets::RomfsReader& read,
                                                std::string_view spec_text, bool standard) {
    const auto spec = ParseSpec(spec_text);
    if (!spec || !read)
        return nullptr;
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const FontAtlas>> cache;
    std::string region = "STD";
    if (!standard) {
        std::lock_guard guard{region_lock};
        region = region_folder;
    }
    const std::string key = region + "|" + spec->Canonical();
    // One build at a time under the lock: the asset worker and decode_font may ask for the same
    // spec at once, and both must end up with the very same object.
    std::lock_guard lock{mutex};
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;
    std::shared_ptr<const FontAtlas> built;
    try {
        built = Build(read, *spec, region);
    } catch (...) {
        built = nullptr;
    }
    // Only successes are cached: a failure can be the romfs not being readable yet.
    if (built)
        cache.emplace(key, built);
    return built;
}

int IconFont::Glyph(char32_t c) const {
    const auto it = std::lower_bound(codes.begin(), codes.end(), std::pair<char32_t, int>{c, -1});
    return it != codes.end() && it->first == c ? it->second : -1;
}

std::shared_ptr<const IconFont> LoadIconFont(const KirbyAssets::RomfsReader& read, const std::string& path) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const IconFont>> cache;
    std::lock_guard lock{mutex};
    if (const auto it = cache.find(path); it != cache.end())
        return it->second;
    const auto packed = read(path);
    const auto bytes = packed ? KirbyAssets::DecompressCmp(*packed) : std::nullopt;
    if (!bytes)
        return nullptr;
    const std::span<const std::uint8_t> f{*bytes};
    const auto u8at = [&](std::size_t o) { return o < f.size() ? f[o] : std::uint8_t{}; };
    const auto u16at = [&](std::size_t o) { return static_cast<std::uint32_t>(u8at(o) | u8at(o + 1) << 8); };
    const auto u32at = [&](std::size_t o) { return u16at(o) | u16at(o + 2) << 16; };
    // FFNT, then FINF: its section offsets point past each section's magic and size
    if (f.size() < 0x40 || std::memcmp(f.data(), "FFNT", 4) != 0 || std::memcmp(f.data() + 0x14, "FINF", 4) != 0)
        return nullptr;
    auto font = std::make_shared<IconFont>();
    font->line = static_cast<int>(u16at(0x20));
    const std::size_t tglp = u32at(0x28), cwdh = u32at(0x2C), cmap = u32at(0x30);
    font->cell_w = u8at(tglp), font->cell_h = u8at(tglp + 1);
    font->baseline = static_cast<int>(u16at(tglp + 8));
    font->per_row = static_cast<int>(u16at(tglp + 0xC));
    font->per_sheet = font->per_row * static_cast<int>(u16at(tglp + 0xE));
    const std::size_t data = u32at(tglp + 0x14);
    if (data >= f.size() || font->per_sheet <= 0)
        return nullptr;
    // the sheets are stored bottom up
    font->sheets = KirbyAssets::DecodeBntxAll(f.subspan(data));
    for (auto& sheet : font->sheets) {
        const std::size_t row = std::size_t{sheet.width} * 4;
        for (std::uint32_t y = 0; y < sheet.height / 2; ++y)
            std::swap_ranges(sheet.rgba.begin() + y * row, sheet.rgba.begin() + (y + 1) * row,
                             sheet.rgba.begin() + (sheet.height - 1 - y) * row);
    }
    for (std::size_t at = cwdh; at && at + 8 <= f.size(); at = u32at(at + 4)) {
        const std::uint32_t first = u16at(at), last = u16at(at + 2);
        if (font->widths.size() < last + 1)
            font->widths.resize(last + 1);
        for (std::uint32_t i = first; i <= last && at + 8 + (i - first) * 3 + 3 <= f.size(); ++i) {
            const std::size_t w = at + 8 + (i - first) * 3;
            font->widths[i] = {static_cast<std::int8_t>(u8at(w)), u8at(w + 1), u8at(w + 2)};
        }
        if (u32at(at + 4) <= at)
            break;
    }
    // code maps: direct (0), table (1) or scan (2) of u32 code / u32 index pairs
    for (std::size_t at = cmap; at && at + 16 <= f.size();) {
        const std::uint32_t begin = u32at(at), end = u32at(at + 4), method = u16at(at + 8);
        if (method == 0) {
            for (std::uint32_t c = begin; c <= end && c - begin < 0x10000; ++c)
                font->codes.push_back({c, static_cast<int>(u16at(at + 16) + c - begin)});
        } else if (method == 1) {
            for (std::uint32_t c = begin; c <= end && c - begin < 0x10000; ++c)
                if (const auto i = u16at(at + 16 + (c - begin) * 2); i != 0xFFFF)
                    font->codes.push_back({c, static_cast<int>(i)});
        } else if (method == 2) {
            for (std::uint32_t i = 0, n = u32at(at + 16); i < n && i < 0x10000; ++i)
                font->codes.push_back({u32at(at + 20 + i * 8), static_cast<int>(u32at(at + 24 + i * 8))});
        }
        const std::size_t next = u32at(at + 12);
        if (next <= at)
            break;
        at = next;
    }
    std::sort(font->codes.begin(), font->codes.end());
    if (font->sheets.empty())
        return nullptr;
    cache.emplace(path, font);
    return font;
}

std::optional<std::string> ParseFontSpec(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > 4096)
        return std::nullopt;
    std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (const auto hash = line.find('#'); hash != std::string_view::npos)
            line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty())
            continue;
        const auto spec = ParseSpec(line);
        if (!spec)
            return std::nullopt;
        return spec->Canonical();
    }
    return std::nullopt;
}

} // namespace KirbyFont
