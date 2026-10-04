// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Game-font atlases for the Wonder companion. Format notes and metric conventions: wonder_font.h.

#include "wonder_font.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
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

namespace WonderFont {
namespace {

constexpr std::uint32_t BfotfMagic = 0x7F9A0218;
constexpr std::uint32_t FirstCodepoint = 0x20;
constexpr std::uint32_t LastCodepoint = 0x2026; // U+2026 ellipsis; U+2022 bullet sits inside
constexpr std::uint32_t DefaultCap = 30;
constexpr std::uint32_t MinCap = 6, MaxCap = 160;
constexpr std::uint32_t MaxAtlas = 2048;
constexpr std::uint32_t Pad = 1; // transparent border around every glyph box

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
    std::string Canonical() const {
        return member + ":" + std::to_string(cap);
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
    std::string_view name = text;
    if (const auto colon = text.rfind(':'); colon != std::string_view::npos) {
        name = Trim(text.substr(0, colon));
        const std::string_view num = Trim(text.substr(colon + 1));
        std::uint32_t cap{};
        const auto [end, ec] = std::from_chars(num.data(), num.data() + num.size(), cap);
        if (ec != std::errc{} || end != num.data() + num.size() || cap < MinCap || cap > MaxCap)
            return std::nullopt;
        spec.cap = cap;
    }
    if (name.ends_with(".bfotf"))
        name.remove_suffix(6);
    if (name.empty() || name.size() > 64)
        return std::nullopt;
    for (const char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.')
            return std::nullopt;
    }
    if (name.find("..") != std::string_view::npos)
        return std::nullopt;
    spec.member = std::string{name};
    return spec;
}

struct Raster {
    std::uint32_t cp{};
    int w{}, h{}, x0{}, y0{}, advance{};
    std::vector<std::uint8_t> coverage;
    std::uint32_t ax{}, ay{}; // atlas position of the box (inside the padding)
};

std::shared_ptr<const FontAtlas> Build(const WonderAssets::RomfsReader& read, const Spec& spec) {
    const auto archive_z = read(std::string{FontArchive});
    if (!archive_z)
        return nullptr;
    const auto archive = WonderAssets::Zstd(*archive_z);
    if (!archive)
        return nullptr;
    auto member = WonderAssets::SarcMember(*archive, "scft/" + spec.member + ".bfotf");
    // Game 1.0.0 / 1.0.1 ship the same Mario lettering under its older name (renamed in 1.2.x).
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> OlderNames{{
        {"Nin-SuperMarioBros.V2.0", "NinNewSuperMarioBrosT53Reg"},
        {"Nin-SuperMarioBros.V2.0-o", "NinNewSuperMarioBrosT53ou"},
    }};
    for (const auto& [name, older] : OlderNames)
        if (!member && spec.member == name)
            member = WonderAssets::SarcMember(*archive, "scft/" + std::string{older} + ".bfotf");
    if (!member)
        return nullptr;
    const auto otf = DecodeBfotf(*member);
    if (!otf || otf->size() < 12)
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

    // Rasterise every rendered codepoint the font has, in codepoint order.
    std::vector<Raster> rasters;
    std::map<std::uint32_t, std::size_t> by_cp;
    for (std::uint32_t cp = FirstCodepoint; cp <= LastCodepoint; ++cp) {
        if (!Rendered(cp))
            continue;
        const int gi = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
        if (gi == 0)
            continue;
        Raster r;
        r.cp = cp;
        int adv, lsb, x1, y1;
        stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
        r.advance = static_cast<int>(std::lround(static_cast<float>(adv) * scale));
        stbtt_GetGlyphBitmapBox(&info, gi, scale, scale, &r.x0, &r.y0, &x1, &y1);
        r.w = std::max(0, x1 - r.x0);
        r.h = std::max(0, y1 - r.y0);
        if (r.w > 0 && r.h > 0) {
            if (r.w > 1024 || r.h > 1024)
                return nullptr;
            r.coverage.assign(static_cast<std::size_t>(r.w) * r.h, 0);
            stbtt_MakeGlyphBitmap(&info, r.coverage.data(), r.w, r.h, r.w, scale, scale, gi);
            // Nothing inked (a blank glyph with a box): keep advance only.
            if (std::all_of(r.coverage.begin(), r.coverage.end(), [](auto v) { return v == 0; })) {
                r.w = r.h = 0;
                r.coverage.clear();
            }
        } else {
            r.w = r.h = 0;
        }
        by_cp.emplace(cp, rasters.size());
        rasters.push_back(std::move(r));
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
            for (int x = 0; x < r.w; ++x)
                out->atlas.rgba[((std::size_t{r.ay} + y) * width + r.ax + x) * 4 + 3] =
                    r.coverage[static_cast<std::size_t>(y) * r.w + x];
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
    out->first_codepoint = FirstCodepoint;
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

std::shared_ptr<const FontAtlas> BuildFontAtlas(const WonderAssets::RomfsReader& read,
                                                std::string_view spec_text) {
    const auto spec = ParseSpec(spec_text);
    if (!spec || !read)
        return nullptr;
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const FontAtlas>> cache;
    const std::string key = spec->Canonical();
    // One build at a time under the lock: the asset worker and decode_font may ask for the same
    // spec at once, and both must end up with the very same object.
    std::lock_guard lock{mutex};
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;
    std::shared_ptr<const FontAtlas> built;
    try {
        built = Build(read, *spec);
    } catch (...) {
        built = nullptr;
    }
    // Only successes are cached: a failure can be the romfs not being readable yet.
    if (built)
        cache.emplace(key, built);
    return built;
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

} // namespace WonderFont
