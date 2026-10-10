// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Plantin MT Pro atlas for the Dragon Quest III companion. Conventions: dq3_font.h.

#include "dq3_font.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

// stb_truetype, private to this translation unit (static functions). Contraction off so the
// rasteriser's float math is the same on x86-64 and AArch64.
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

namespace dq3 {
namespace {

constexpr u32 FirstCodepoint = 0x20;
constexpr u32 StyleBase = 0xE000;
constexpr u32 LastCodepoint = StyleBase + 0x100 * (static_cast<u32>(FontStyle::Count) - 2) + 0xFF; // end of the last range
constexpr u32 MaxAtlas = 2048;
constexpr u32 Pad = 1;

struct StyleSpec {
    FontStyle style;
    int face; ///< 0 Plantin Semibold, 1 Plantin Bold, 2 Avenir Next Demi
    float em_px;
};
constexpr StyleSpec Styles[] = {
    {FontStyle::Value, 0, 26.0f},   {FontStyle::Bold26, 1, 26.0f},  {FontStyle::Semi24, 0, 24.0f},
    {FontStyle::Semi28, 0, 28.0f},  {FontStyle::Semi30, 0, 30.0f},  {FontStyle::Semi32, 0, 32.0f},
    {FontStyle::Bold24, 1, 24.0f},  {FontStyle::Bold28, 1, 28.0f},  {FontStyle::Bold30, 1, 30.0f},
    {FontStyle::Bold32, 1, 32.0f},  {FontStyle::Bold34, 1, 34.0f},  {FontStyle::Bold36, 1, 36.0f},
    {FontStyle::Bold40, 1, 40.0f},  {FontStyle::Bold44, 1, 44.0f},  {FontStyle::Avenir24, 2, 24.0f},
    {FontStyle::Avenir26, 2, 26.0f}, {FontStyle::Semi22, 0, 22.0f}, {FontStyle::Semi20, 0, 20.0f},
    {FontStyle::Bold22, 1, 22.0f},  {FontStyle::Bold54, 1, 54.0f},
};
static_assert(std::size(Styles) == static_cast<std::size_t>(FontStyle::Count));

bool RenderedPlain(u32 cp) {
    if ((cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0xFF))
        return true;
    switch (cp) {
    case 0x2013:
    case 0x2014:
    case 0x2018:
    case 0x2019:
    case 0x201C:
    case 0x201D:
    case 0x2022:
    case 0x2026:
        return true;
    default:
        return false;
    }
}

struct Raster {
    u32 cp{};
    int w{}, h{}, x0{}, y0{}, advance{};
    std::vector<u8> coverage;
    u32 ax{}, ay{};
};

std::shared_ptr<const FontAtlas> Build(std::span<const u8> semibold, std::span<const u8> bold,
                                       std::span<const u8> avenir) {
    if (avenir.empty())
        avenir = bold;
    if (semibold.size() < 12 || bold.size() < 12 || avenir.size() < 12)
        return nullptr;
    stbtt_fontinfo fonts[3]{};
    const std::span<const u8> files[3] = {semibold, bold, avenir};
    for (int i = 0; i < 3; ++i) {
        if (std::memcmp(files[i].data(), "OTTO", 4) != 0)
            return nullptr;
        const int off = stbtt_GetFontOffsetForIndex(files[i].data(), 0);
        if (off < 0 || !stbtt_InitFont(&fonts[i], files[i].data(), off))
            return nullptr;
    }
    std::vector<Raster> rasters;
    std::map<u32, std::size_t> by_cp;
    for (const StyleSpec& st : Styles) {
        stbtt_fontinfo& info = fonts[st.face];
        const float scale = stbtt_ScaleForMappingEmToPixels(&info, st.em_px);
        const auto add = [&](u32 source_cp, u32 slot_cp) {
            const int gi = stbtt_FindGlyphIndex(&info, static_cast<int>(source_cp));
            if (gi == 0)
                return true;
            Raster r;
            r.cp = slot_cp;
            int adv, lsb, x1, y1;
            stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
            r.advance = static_cast<int>(std::lround(static_cast<float>(adv) * scale));
            stbtt_GetGlyphBitmapBox(&info, gi, scale, scale, &r.x0, &r.y0, &x1, &y1);
            r.w = std::max(0, x1 - r.x0);
            r.h = std::max(0, y1 - r.y0);
            if (r.w > 0 && r.h > 0) {
                if (r.w > 256 || r.h > 256)
                    return false;
                r.coverage.assign(static_cast<std::size_t>(r.w) * r.h, 0);
                stbtt_MakeGlyphBitmap(&info, r.coverage.data(), r.w, r.h, r.w, scale, scale, gi);
                if (std::all_of(r.coverage.begin(), r.coverage.end(), [](u8 v) { return v == 0; })) {
                    r.w = r.h = 0;
                    r.coverage.clear();
                }
            } else {
                r.w = r.h = 0;
            }
            by_cp.emplace(slot_cp, rasters.size());
            rasters.push_back(std::move(r));
            return true;
        };
        if (st.style == FontStyle::Value) {
            for (u32 cp = FirstCodepoint; cp <= 0x2026; ++cp)
                if (RenderedPlain(cp) && !add(cp, cp))
                    return nullptr;
        } else {
            const u32 base = StyleBase + 0x100 * (static_cast<u32>(st.style) - 1);
            for (u32 cp = 0x20; cp <= 0xFF; ++cp)
                if ((cp <= 0x7E || cp >= 0xA0) && !add(cp, base + cp))
                    return nullptr;
        }
    }
    if (!by_cp.contains('0') || !by_cp.contains('A') || !by_cp.contains(StyleBase + 'A'))
        return nullptr;

    u32 width = 0, height = 0;
    for (u32 w = 256; w <= MaxAtlas; w *= 2) {
        u32 x = 0, y = 0, shelf = 0;
        bool fits = true;
        for (auto& r : rasters) {
            if (r.w == 0)
                continue;
            const u32 cw = static_cast<u32>(r.w) + 2 * Pad, ch = static_cast<u32>(r.h) + 2 * Pad;
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
        const u32 h = y + shelf;
        if (fits && h <= w) {
            width = w;
            height = std::max<u32>(h, 1);
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
    for (const auto& r : rasters)
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x)
                out->atlas.rgba[((std::size_t{r.ay} + y) * width + r.ax + x) * 4 + 3] =
                    r.coverage[static_cast<std::size_t>(y) * r.w + x];
    const auto glyph_of = [](const Raster& r) {
        EdenDsmodFontGlyph g{};
        if (r.w > 0) {
            g.x = static_cast<std::uint16_t>(r.ax);
            g.y = static_cast<std::uint16_t>(r.ay);
            g.w = static_cast<std::uint16_t>(r.w);
            g.h = static_cast<std::uint16_t>(r.h);
            g.bearing_x = static_cast<std::int16_t>(r.x0);
            g.bearing_y = static_cast<std::int16_t>(-r.y0);
        }
        g.advance = static_cast<std::uint16_t>(std::max(0, r.advance));
        return g;
    };
    EdenDsmodFontGlyph blank{};
    blank.advance = glyph_of(rasters[by_cp.at(' ')]).advance;
    out->line_height = FontLineHeight;
    out->first_codepoint = FirstCodepoint;
    out->glyphs.assign(LastCodepoint - FirstCodepoint + 1, blank);
    for (const auto& [cp, index] : by_cp)
        out->glyphs[cp - FirstCodepoint] = glyph_of(rasters[index]);
    return out;
}

// Next codepoint of UTF-8 (invalid bytes read as U+FFFD).
u32 NextUtf8(std::string_view s, std::size_t& i) {
    const u8 c = static_cast<u8>(s[i++]);
    if (c < 0x80)
        return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n == 0)
        return 0xFFFD;
    u32 cp = c & (0x3F >> n);
    while (n-- > 0) {
        if (i >= s.size() || (static_cast<u8>(s[i]) & 0xC0) != 0x80)
            return 0xFFFD;
        cp = cp << 6 | (static_cast<u8>(s[i++]) & 0x3F);
    }
    return cp;
}

void AppendUtf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6);
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | cp >> 12);
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | cp >> 18);
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace

int FontAtlas::Measure(std::string_view utf8) const {
    float pen = 0.0f;
    for (std::size_t i = 0; i < utf8.size();) {
        const u32 cp = NextUtf8(utf8, i);
        if (cp >= first_codepoint && cp - first_codepoint < glyphs.size())
            pen += static_cast<float>(glyphs[cp - first_codepoint].advance);
        else
            pen += 15.0f * 0.5f; // the host's missing-glyph advance (wanted * 0.5)
    }
    return static_cast<int>(pen);
}

bool IsFontSpec(std::span<const u8> bytes) {
    std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (const auto hash = line.find('#'); hash != std::string_view::npos)
            line = line.substr(0, hash);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.remove_suffix(1);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
            line.remove_prefix(1);
        if (!line.empty())
            return line == "dq3-font 1";
    }
    return false;
}

std::shared_ptr<const FontAtlas> BuildFontAtlasFrom(std::span<const u8> semibold,
                                                    std::span<const u8> bold,
                                                    std::span<const u8> avenir) {
    try {
        return Build(semibold, bold, avenir);
    } catch (...) {
        return nullptr;
    }
}

namespace {
std::mutex font_mutex;  ///< guards font_cache only (taken by the tick thread)
std::mutex build_mutex; ///< serialises builders; never taken by CachedFontAtlas
std::shared_ptr<const FontAtlas> font_cache;
} // namespace

std::shared_ptr<const FontAtlas> CachedFontAtlas() {
    std::scoped_lock lock{font_mutex};
    return font_cache;
}

std::shared_ptr<const FontAtlas> BuildFontAtlas(const RangeReader& read, std::string* why) {
    std::scoped_lock build{build_mutex};
    if (auto cached = CachedFontAtlas())
        return cached;
    // pak reads, hash checks, Oodle and rasterisation without font_mutex
    std::string reason;
    const auto semi = ReadMember(read, Member(MemberId::FontSemibold), &reason);
    const auto bold = semi ? ReadMember(read, Member(MemberId::FontBold), &reason) : std::nullopt;
    const auto avenir = bold ? ReadMember(read, Member(MemberId::FontAvenir), &reason) : std::nullopt;
    if (!semi || !bold || !avenir) {
        if (why)
            *why = "font member: " + reason;
        return nullptr;
    }
    auto built = BuildFontAtlasFrom(*semi, *bold, *avenir);
    if (!built) {
        if (why)
            *why = "font rasterisation failed";
        return nullptr;
    }
    std::scoped_lock lock{font_mutex};
    if (!font_cache)
        font_cache = std::move(built);
    return font_cache;
}

std::string Styled(std::u32string_view text, FontStyle style) {
    std::string out;
    const u32 base = style == FontStyle::Value ? 0 : StyleBase + 0x100 * (static_cast<u32>(style) - 1);
    for (const char32_t c : text) {
        u32 cp = static_cast<u32>(c);
        if (base && cp >= 0x20 && cp <= 0xFF && !(cp > 0x7E && cp < 0xA0))
            cp += base;
        AppendUtf8(out, cp);
    }
    return out;
}

std::string StyledWrap(std::u32string_view text, FontStyle style) {
    // the host wraps at ASCII spaces: keep them plain (drawn with the Value style's space)
    std::string out;
    std::u32string run;
    for (const char32_t c : text) {
        if (c == U' ') {
            out += Styled(run, style);
            out += ' ';
            run.clear();
        } else {
            run += c;
        }
    }
    return out + Styled(run, style);
}

std::string Styled(std::string_view ascii, FontStyle style) {
    return Styled(AsciiToU32(ascii), style);
}

namespace {
std::atomic<NonAsciiHook> non_ascii_hook{nullptr};
std::atomic<std::uint64_t> non_ascii_count{0};
std::mutex non_ascii_mutex;
std::string non_ascii_first; // guarded by non_ascii_mutex

std::string EscapeNonAscii(std::string_view s) {
    std::string out;
    for (const char c : s) {
        const auto b = static_cast<u8>(c);
        if (b < 0x80 && b >= 0x20) {
            out += c;
        } else {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\x%02X", b);
            out += hex;
        }
    }
    return out;
}

void ReportNonAscii(std::string_view s) {
    if (non_ascii_count.fetch_add(1, std::memory_order_relaxed) == 0) {
        std::lock_guard lock{non_ascii_mutex};
        non_ascii_first = EscapeNonAscii(s.substr(0, 96));
    }
    if (const NonAsciiHook hook = non_ascii_hook.load(std::memory_order_acquire)) {
        hook(s);
        return;
    }
    // Debug and test builds: a narrow non-ASCII string reached the byte-wise text path (pass UTF-32).
    assert(!"dq3: non-ASCII byte in the byte-wise text path (use a U\"...\" string)");
}
} // namespace

std::u32string AsciiToU32(std::string_view s) {
    std::u32string u;
    u.reserve(s.size());
    bool bad = false;
    for (const char c : s) {
        const auto b = static_cast<u8>(c);
        bad |= b >= 0x80;
        u += b >= 0x80 ? U'?' : static_cast<char32_t>(b);
    }
    if (bad)
        ReportNonAscii(s);
    return u;
}

#ifdef DQ3_TEST_API
NonAsciiHook SetNonAsciiHook(NonAsciiHook hook) {
    return non_ascii_hook.exchange(hook, std::memory_order_acq_rel);
}
#endif

std::uint64_t NonAsciiTextCount() {
    return non_ascii_count.load(std::memory_order_relaxed);
}

std::string FirstNonAsciiText() {
    std::lock_guard lock{non_ascii_mutex};
    return non_ascii_first;
}

std::string Colour(u32 argb, const std::string& styled) {
    char tag[16];
    std::snprintf(tag, sizeof(tag), "{c:#%08X}", argb);
    return tag + styled + "{/c}";
}

std::string Grouped(std::int64_t v) {
    const std::uint64_t mag = v < 0 ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
    const std::string s = std::to_string(mag);
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0)
            out += ',';
        out += s[i];
    }
    return v < 0 ? "-" + out : out;
}

std::optional<std::u32string> FromUtf16(std::span<const std::uint16_t> units) {
    std::u32string out;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const u32 u = units[i];
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 >= units.size() || units[i + 1] < 0xDC00 || units[i + 1] > 0xDFFF)
                return std::nullopt;
            out += static_cast<char32_t>(0x10000 + ((u - 0xD800) << 10) + (units[++i] - 0xDC00));
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return std::nullopt;
        } else {
            out += static_cast<char32_t>(u);
        }
    }
    return out;
}

// ------------------------------------------------------------------ text into composed images
namespace {
struct TextFonts {
    std::vector<u8> files[2]; ///< Semibold, Bold
    stbtt_fontinfo info[2]{};
};
std::mutex text_fonts_mutex;
std::shared_ptr<const TextFonts> text_fonts;

std::shared_ptr<const TextFonts> LoadTextFonts(const RangeReader& read) {
    {
        std::scoped_lock lock{text_fonts_mutex};
        if (text_fonts)
            return text_fonts;
    }
    auto f = std::make_shared<TextFonts>();
    const auto semi = ReadMember(read, Member(MemberId::FontSemibold));
    const auto bold = semi ? ReadMember(read, Member(MemberId::FontBold)) : std::nullopt;
    if (!semi || !bold)
        return nullptr;
    f->files[0] = *semi;
    f->files[1] = *bold;
    for (int i = 0; i < 2; ++i) {
        if (f->files[i].size() < 12 || std::memcmp(f->files[i].data(), "OTTO", 4) != 0)
            return nullptr;
        const int off = stbtt_GetFontOffsetForIndex(f->files[i].data(), 0);
        if (off < 0 || !stbtt_InitFont(&f->info[i], f->files[i].data(), off))
            return nullptr;
    }
    std::scoped_lock lock{text_fonts_mutex};
    if (!text_fonts)
        text_fonts = std::move(f);
    return text_fonts;
}

std::pair<int, int> Metrics(const stbtt_fontinfo& info, float scale) {
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    // FreeType size metrics (what Pillow's getmetrics returns): ascender rounded up, descender down
    return {static_cast<int>(std::ceil(static_cast<float>(asc) * scale - 1e-4f)),
            static_cast<int>(std::ceil(static_cast<float>(-desc) * scale - 1e-4f))};
}

int Advance(const stbtt_fontinfo& info, float scale, std::u32string_view text) {
    int pen = 0;
    for (const char32_t c : text) {
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&info, stbtt_FindGlyphIndex(&info, static_cast<int>(c)), &adv, &lsb);
        pen += static_cast<int>(std::lround(static_cast<float>(adv) * scale));
    }
    return pen;
}
} // namespace

#ifdef DQ3_TEST_API
std::optional<std::pair<int, int>> GameTextMetrics(const RangeReader& read, float em_px, bool bold) {
    const auto f = LoadTextFonts(read);
    if (!f)
        return std::nullopt;
    const auto& info = f->info[bold ? 1 : 0];
    return Metrics(info, stbtt_ScaleForMappingEmToPixels(&info, em_px));
}
#endif

bool DrawGameText(Image& dst, const RangeReader& read, std::u32string_view text, const GameTextStyle& st, int x,
                  int y, char ah, char av) {
    const auto f = LoadTextFonts(read);
    if (!f)
        return false;
    const auto& info = f->info[st.bold ? 1 : 0];
    const float scale = stbtt_ScaleForMappingEmToPixels(&info, st.em_px);
    const auto [asc, desc] = Metrics(info, scale);
    const int width = Advance(info, scale, text);
    int pen = ah == 'm' ? x - width / 2 : ah == 'r' ? x - width : x;
    const int base = av == 'a'   ? y + asc
                     : av == 'm' ? y + static_cast<int>(std::lround((asc - desc) / 2.0))
                                 : y;
    // one coverage layer for the whole run (glyph overlaps add up, capped)
    const int pad = st.stroke + 2;
    const int lw = width + 2 * pad + static_cast<int>(st.em_px), lh = asc + desc + 2 * pad + 4;
    const int ox = pen - pad - static_cast<int>(st.em_px) / 2, oy = base - asc - pad - 2;
    std::vector<u8> cov(static_cast<std::size_t>(lw) * lh, 0);
    for (const char32_t c : text) {
        const int gi = stbtt_FindGlyphIndex(&info, static_cast<int>(c));
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
        stbtt_GetGlyphBitmapBox(&info, gi, scale, scale, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw <= 512 && gh <= 512) {
            std::vector<u8> g(static_cast<std::size_t>(gw) * gh, 0);
            stbtt_MakeGlyphBitmap(&info, g.data(), gw, gh, gw, scale, scale, gi);
            for (int yy = 0; yy < gh; ++yy)
                for (int xx = 0; xx < gw; ++xx) {
                    const int cx = pen + x0 + xx - ox, cy = base + y0 + yy - oy;
                    if (cx < 0 || cy < 0 || cx >= lw || cy >= lh)
                        continue;
                    u8& d = cov[static_cast<std::size_t>(cy) * lw + cx];
                    d = static_cast<u8>(std::min(255, d + g[static_cast<std::size_t>(yy) * gw + xx]));
                }
        }
        pen += static_cast<int>(std::lround(static_cast<float>(adv) * scale));
    }
    const auto paint = [&](const std::vector<u8>& layer, const u8 rgb[3]) {
        for (int yy = 0; yy < lh; ++yy) {
            const int dy = oy + yy;
            if (dy < 0 || dy >= static_cast<int>(dst.height))
                continue;
            for (int xx = 0; xx < lw; ++xx) {
                const int dx = ox + xx;
                const u8 a = layer[static_cast<std::size_t>(yy) * lw + xx];
                if (!a || dx < 0 || dx >= static_cast<int>(dst.width))
                    continue;
                u8* p = &dst.rgba[(static_cast<std::size_t>(dy) * dst.width + dx) * 4];
                const double sa = a / 255.0, da = p[3] / 255.0, oa = sa + da * (1.0 - sa);
                for (int ch = 0; ch < 3; ++ch)
                    p[ch] = static_cast<u8>(std::lround((rgb[ch] * sa + p[ch] * da * (1.0 - sa)) / oa));
                p[3] = static_cast<u8>(std::lround(oa * 255.0));
            }
        }
    };
    if (st.stroke > 0) {
        std::vector<u8> ring(cov.size(), 0);
        const int r = st.stroke;
        for (int yy = 0; yy < lh; ++yy)
            for (int xx = 0; xx < lw; ++xx) {
                u8 m = 0;
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        if (dx * dx + dy * dy > r * r)
                            continue;
                        const int sx = xx + dx, sy = yy + dy;
                        if (sx >= 0 && sy >= 0 && sx < lw && sy < lh)
                            m = std::max(m, cov[static_cast<std::size_t>(sy) * lw + sx]);
                    }
                ring[static_cast<std::size_t>(yy) * lw + xx] = m;
            }
        paint(ring, st.stroke_rgb);
    }
    paint(cov, st.rgb);
    return true;
}

} // namespace dq3
