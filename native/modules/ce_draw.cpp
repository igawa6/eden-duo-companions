// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ce_draw.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ce_draw {
namespace {

std::string_view BundleFor(std::string_view alias) {
    using namespace ce_unity::bundles;
    if (alias == "systemgfx")
        return SystemGfx;
    if (alias == "packedassets")
        return PackedAssets;
    if (alias == "gfx2")
        return Gfx2;
    if (alias == "bestiary")
        return Bestiary;
    if (alias == "startmenu")
        return StartMenu;
    if (alias == "sprites2")
        return Sprites2;
    return alias;
}

/// UTF-8 -> code points (invalid bytes become '?').
std::vector<std::uint32_t> Decode(std::string_view s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(c);
            ++i;
            continue;
        }
        const int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
        if (n < 0) {
            out.push_back('?');
            ++i;
            continue;
        }
        std::uint32_t cp = c & (0x3F >> n);
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        if (!ok) {
            out.push_back('?');
            ++i;
            continue;
        }
        out.push_back(cp);
        i += static_cast<std::size_t>(n) + 1;
    }
    return out;
}

std::string Encode(const std::vector<std::uint32_t>& cps) {
    std::string out;
    for (const auto cp : cps) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

constexpr std::uint32_t Ellipsis = 0x2026;

/// PIL alpha_composite of one pixel (straight alpha).
inline void Over(std::uint8_t* d, const std::uint8_t* s) {
    const unsigned sa = s[3];
    if (sa == 0)
        return;
    const unsigned da = d[3];
    if (sa == 255 || da == 0) {
        std::memcpy(d, s, 4);
        return;
    }
    const double a = sa / 255.0, b = da / 255.0 * (1.0 - a);
    const double out = a + b;
    for (int k = 0; k < 3; ++k)
        d[k] = static_cast<std::uint8_t>(std::lround((s[k] * a + d[k] * b) / out));
    d[3] = static_cast<std::uint8_t>(std::lround(out * 255.0));
}

void CompositeInto(Image& dst, const Image& src, int x, int y) {
    const int dw = static_cast<int>(dst.width), dh = static_cast<int>(dst.height);
    const int sw = static_cast<int>(src.width), sh = static_cast<int>(src.height);
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(dw, x + sw), y1 = std::min(dh, y + sh);
    for (int yy = y0; yy < y1; ++yy) {
        std::uint8_t* d = dst.rgba.data() + (static_cast<std::size_t>(yy) * dw + x0) * 4;
        const std::uint8_t* s =
            src.rgba.data() + (static_cast<std::size_t>(yy - y) * sw + (x0 - x)) * 4;
        for (int xx = x0; xx < x1; ++xx, d += 4, s += 4)
            Over(d, s);
    }
}

void Split(const std::string& text, std::vector<std::vector<std::string>>& rows) {
    std::size_t start = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        start = 3;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::vector<std::string> cells;
        std::size_t c = 0;
        while (true) {
            const std::size_t e = line.find(';', c);
            cells.push_back(line.substr(c, e == std::string::npos ? std::string::npos : e - c));
            if (e == std::string::npos)
                break;
            c = e + 1;
        }
        rows.push_back(std::move(cells));
        start = end + 1;
    }
}

// ---- BGDatabase (Data/StreamingAssets/bansheegz_database.bytes) string columns -------------------
// Layout seen in 1.41 (research contracts/field-readers.md "Text"): a meta name (int length +
// bytes), its fields (int length + name), and per string field: int count, count x (row, end
// offset), then the concatenated UTF-8 values. The "en" field is what the English game shows.
constexpr const char* BgColumns[] = {"tr_QUESTTASKS",     "tr_QUESTS_NAME",   "tr_REWARDBOARD",
                                     "tr_EQUIPS_NAME",    "tr_PASSIVES_NAME", "tr_PASSIVES_DESCR",
                                     "tr_CLASSEMBLEMS_NAME"};

std::size_t FindName(const std::vector<std::uint8_t>& d, std::string_view name, std::size_t from,
                     std::size_t to) {
    std::string pat(4, '\0');
    const auto n = static_cast<std::uint32_t>(name.size());
    std::memcpy(pat.data(), &n, 4);
    pat.append(name);
    to = std::min(to, d.size());
    if (from >= to)
        return std::string::npos;
    const auto it = std::search(d.begin() + static_cast<std::ptrdiff_t>(from),
                                d.begin() + static_cast<std::ptrdiff_t>(to), pat.begin(), pat.end());
    return it == d.begin() + static_cast<std::ptrdiff_t>(to)
               ? std::string::npos
               : static_cast<std::size_t>(it - d.begin()) + pat.size();
}

std::vector<std::string> ColumnAfter(const std::vector<std::uint8_t>& d, std::size_t p) {
    const auto i32 = [&](std::size_t at) {
        std::int32_t v{};
        if (at + 4 <= d.size())
            std::memcpy(&v, d.data() + at, 4);
        return v;
    };
    for (std::size_t q = p; q < p + 2000 && q + 12 < d.size(); ++q) {
        const std::int32_t n = i32(q);
        if (n < 2 || n > 100000 || q + 4 + 8ull * static_cast<std::uint64_t>(n) > d.size())
            continue;
        bool ok = true;
        std::int32_t prev = 0;
        for (std::int32_t k = 0; k < n && ok; ++k) {
            const std::int32_t r = i32(q + 4 + 8 * static_cast<std::size_t>(k));
            const std::int32_t e = i32(q + 8 + 8 * static_cast<std::size_t>(k));
            ok = r == k && e >= prev && e < 4000000;
            prev = e;
        }
        if (!ok)
            continue;
        const std::size_t blob = q + 4 + 8 * static_cast<std::size_t>(n);
        if (blob + static_cast<std::size_t>(prev) > d.size())
            return {};
        std::vector<std::string> out;
        out.reserve(static_cast<std::size_t>(n));
        std::int32_t start = 0;
        for (std::int32_t k = 0; k < n; ++k) {
            const std::int32_t e = i32(q + 8 + 8 * static_cast<std::size_t>(k));
            out.emplace_back(reinterpret_cast<const char*>(d.data() + blob + start),
                             static_cast<std::size_t>(e - start));
            start = e;
        }
        return out;
    }
    return {};
}

} // namespace

// ---- Art -------------------------------------------------------------------------------------

Art::Art(ce_unity::RangeReader r) : read{r}, reader{std::move(r)} {}

std::optional<Ptr> Art::CacheFind(const std::string& key) {
    const auto it = sprites.find(key);
    if (it == sprites.end())
        return std::nullopt;
    sprite_lru.splice(sprite_lru.begin(), sprite_lru, it->second.at);
    return it->second.img;
}

void Art::CachePut(std::string key, Ptr img) {
    sprite_lru.push_front(key);
    sprite_bytes += img ? img->rgba.size() : 0;
    sprites.insert_or_assign(std::move(key), Cached{std::move(img), sprite_lru.begin()});
    // the newest entry always stays (a single picture over the budget is still served)
    while (sprite_lru.size() > 1 && (sprite_bytes > CacheBudget || sprite_lru.size() > CacheEntries)) {
        const auto old = sprites.find(sprite_lru.back());
        if (old->second.img)
            sprite_bytes -= old->second.img->rgba.size();
        sprites.erase(old);
        sprite_lru.pop_back();
    }
}

Ptr Art::Sprite(std::string_view alias, std::string_view name, bool crop) {
    std::string key;
    key.append(alias).append("|").append(name).append(crop ? "|c" : "|");
    std::scoped_lock lock{mutex};
    if (auto hit = CacheFind(key))
        return *hit;
    Ptr out;
    if (auto img = reader.LoadSprite(BundleFor(alias), name)) {
        out = std::make_shared<const Image>(crop ? AlphaCropped(*img) : std::move(*img));
    }
    CachePut(std::move(key), out);
    return out;
}

Ptr Art::Texture(std::string_view alias, std::string_view name) {
    std::string key;
    key.append("tex|").append(alias).append("|").append(name);
    std::scoped_lock lock{mutex};
    if (auto hit = CacheFind(key))
        return *hit;
    Ptr out;
    if (auto img = reader.LoadTexture(BundleFor(alias), name))
        out = std::make_shared<const Image>(std::move(*img));
    CachePut(std::move(key), out);
    return out;
}

Ptr Art::Family(ce_unity::Family family, std::string_view id, bool crop) {
    const std::string name = ce_unity::FamilySprite(family, id);
    std::string key = "fam|" + name + (crop ? "|c" : "|");
    std::scoped_lock lock{mutex};
    if (auto hit = CacheFind(key))
        return *hit;
    Ptr out;
    if (auto img = reader.LoadSpriteAny(ce_unity::FamilyBundles(family), name))
        out = std::make_shared<const Image>(crop ? AlphaCropped(*img) : std::move(*img));
    CachePut(std::move(key), out);
    return out;
}

void Art::EvictBundle(std::string_view alias) {
    std::scoped_lock lock{mutex};
    reader.Evict(BundleFor(alias));
}

std::size_t Art::CachedBytes() {
    std::scoped_lock lock{mutex};
    return sprite_bytes;
}

std::size_t Art::CachedCount() {
    std::scoped_lock lock{mutex};
    return sprites.size();
}

bool Art::LoadFontLocked() {
    if (font)
        return true;
    if (font_failed)
        return false;
    font = ce_unity::LoadTmpFont(reader);
    if (!font || font->atlas_image.rgba.empty() || font->glyphs.empty()) {
        font.reset();
        font_failed = true;
        return false;
    }
    return true;
}

bool Art::FontReady() {
    std::scoped_lock lock{mutex};
    return LoadFontLocked();
}

const Glyph* Art::GlyphFor(std::uint32_t cp) {
    std::scoped_lock lock{mutex};
    if (!LoadFontLocked())
        return nullptr;
    if (const auto it = glyphs.find(cp); it != glyphs.end())
        return &it->second;
    const ce_unity::TmpGlyph* g = font->Find(cp);
    if (!g) {
        if (cp == '?')
            return nullptr;
        // kit: chars.get(ch) or chars['?']
        if (const auto q = glyphs.find('?'); q != glyphs.end())
            return &q->second;
        g = font->Find('?');
        if (!g)
            return nullptr;
        cp = '?';
    }
    Glyph out;
    out.bearing_x = g->bearing_x;
    out.bearing_y = g->bearing_y;
    out.advance = g->advance;
    if (auto t = ce_unity::GlyphTile(*font, cp, 128))
        out.tile = std::move(*t);
    return &glyphs.emplace(cp, std::move(out)).first->second;
}

bool Art::HostFont(std::uint32_t& first, std::vector<EdenDsmodFontGlyph>& out,
                   std::uint32_t& line_height) {
    std::scoped_lock lock{mutex};
    return LoadFontLocked() && ce_unity::BuildHostGlyphs(*font, first, out, line_height);
}

std::optional<Image> Art::HostFontAtlas() {
    std::scoped_lock lock{mutex};
    if (!LoadFontLocked())
        return std::nullopt;
    return ce_unity::HostFontAtlas(*font, 128);
}

const std::vector<std::vector<std::string>>* Art::Table(std::int64_t path_id) {
    std::scoped_lock lock{text_mutex};
    if (const auto it = tables.find(path_id); it != tables.end())
        return it->second.empty() ? nullptr : &it->second;
    std::vector<std::vector<std::string>> rows;
    if (const auto text = ce_unity::ReadTextAsset(read, ce_unity::ResourcesAssets, "", path_id))
        Split(*text, rows);
    auto& slot = tables[path_id];
    slot = std::move(rows);
    return slot.empty() ? nullptr : &slot;
}

const std::vector<std::string>* Art::BgText(std::string_view meta) {
    std::scoped_lock lock{text_mutex};
    if (!bg_tried) {
        bg_tried = true;
        if (const auto bytes = RomfsFile("Data/StreamingAssets/bansheegz_database.bytes")) {
            const auto& d = *bytes;
            for (const char* m : BgColumns) {
                const std::size_t at = FindName(d, m, 0, d.size());
                if (at == std::string::npos)
                    continue;
                const std::size_t en = FindName(d, "en", at, at + 400000);
                if (en == std::string::npos)
                    continue;
                auto column = ColumnAfter(d, en);
                if (!column.empty())
                    bg_text.emplace(m, std::move(column));
            }
        }
    }
    const auto it = bg_text.find(meta);
    return it == bg_text.end() ? nullptr : &it->second;
}

std::optional<std::vector<std::uint8_t>> Art::RomfsFile(std::string_view path) {
    if (!read)
        return std::nullopt;
    const auto size = read.size(path);
    if (!size || *size == 0 || *size > (16u << 20))
        return std::nullopt;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(*size));
    if (!read.read(path, 0, out.data(), out.size()))
        return std::nullopt;
    return out;
}

// ---- image helpers ---------------------------------------------------------------------------

Image Blank(int w, int h, std::uint32_t argb) {
    Image im;
    im.width = static_cast<std::uint32_t>(std::max(0, w));
    im.height = static_cast<std::uint32_t>(std::max(0, h));
    im.rgba.resize(static_cast<std::size_t>(im.width) * im.height * 4);
    if (argb) {
        const std::uint8_t px[4]{static_cast<std::uint8_t>(argb >> 16),
                                 static_cast<std::uint8_t>(argb >> 8),
                                 static_cast<std::uint8_t>(argb), static_cast<std::uint8_t>(argb >> 24)};
        for (std::size_t i = 0; i < im.rgba.size(); i += 4)
            std::memcpy(im.rgba.data() + i, px, 4);
    }
    return im;
}

Image Scale(const Image& im, int s) {
    if (s <= 1)
        return im;
    Image out = Blank(static_cast<int>(im.width) * s, static_cast<int>(im.height) * s);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        const std::uint8_t* srow = im.rgba.data() + static_cast<std::size_t>(y / s) * im.width * 4;
        std::uint8_t* d = out.rgba.data() + static_cast<std::size_t>(y) * out.width * 4;
        for (std::uint32_t x = 0; x < out.width; ++x, d += 4)
            std::memcpy(d, srow + (x / s) * 4, 4);
    }
    return out;
}

Image Crop(const Image& im, int x, int y, int w, int h) {
    Image out = Blank(w, h);
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            const int sx = x + xx, sy = y + yy;
            if (sx < 0 || sy < 0 || sx >= static_cast<int>(im.width) ||
                sy >= static_cast<int>(im.height))
                continue;
            std::memcpy(out.rgba.data() + (static_cast<std::size_t>(yy) * w + xx) * 4,
                        im.rgba.data() + (static_cast<std::size_t>(sy) * im.width + sx) * 4, 4);
        }
    return out;
}

Image AlphaCropped(const Image& im) {
    int x0 = static_cast<int>(im.width), y0 = static_cast<int>(im.height), x1 = -1, y1 = -1;
    for (std::uint32_t y = 0; y < im.height; ++y)
        for (std::uint32_t x = 0; x < im.width; ++x)
            if (im.rgba[(static_cast<std::size_t>(y) * im.width + x) * 4 + 3]) {
                x0 = std::min(x0, static_cast<int>(x));
                y0 = std::min(y0, static_cast<int>(y));
                x1 = std::max(x1, static_cast<int>(x));
                y1 = std::max(y1, static_cast<int>(y));
            }
    if (x1 < 0)
        return im;
    return Crop(im, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

Image ResizeNearest(const Image& im, int sx, int sy, int sw, int sh, int dw, int dh) {
    Image out = Blank(dw, dh);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return out;
    for (int y = 0; y < dh; ++y) {
        const int py = sy + std::min(sh - 1, static_cast<int>((y + 0.5) * sh / dh));
        for (int x = 0; x < dw; ++x) {
            const int px = sx + std::min(sw - 1, static_cast<int>((x + 0.5) * sw / dw));
            if (px < 0 || py < 0 || px >= static_cast<int>(im.width) ||
                py >= static_cast<int>(im.height))
                continue;
            std::memcpy(out.rgba.data() + (static_cast<std::size_t>(y) * dw + x) * 4,
                        im.rgba.data() + (static_cast<std::size_t>(py) * im.width + px) * 4, 4);
        }
    }
    return out;
}

Image Greyed(const Image& im, float amount) {
    if (amount <= 0)
        return im;
    Image out = im;
    for (std::size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
        const std::uint8_t* p = im.rgba.data() + i;
        const unsigned l = (p[0] * 19595u + p[1] * 38470u + p[2] * 7471u + 0x8000u) >> 16;
        for (int k = 0; k < 3; ++k)
            out.rgba[i + k] =
                static_cast<std::uint8_t>(std::lround(p[k] * (1.0f - amount) + l * amount));
    }
    return out;
}

Image Faded(const Image& im, float a) {
    if (a >= 1.0f)
        return im;
    Image out = im;
    for (std::size_t i = 3; i < out.rgba.size(); i += 4)
        out.rgba[i] = static_cast<std::uint8_t>(out.rgba[i] * a);
    return out;
}

Image FlipX(const Image& im) {
    Image out = im;
    for (std::uint32_t y = 0; y < im.height; ++y)
        for (std::uint32_t x = 0; x < im.width; ++x)
            std::memcpy(out.rgba.data() + (static_cast<std::size_t>(y) * im.width + x) * 4,
                        im.rgba.data() +
                            (static_cast<std::size_t>(y) * im.width + (im.width - 1 - x)) * 4,
                        4);
    return out;
}

// ---- Canvas ----------------------------------------------------------------------------------

Canvas::Canvas(Art& a, int w, int h, std::uint32_t argb) : art{a}, im{Blank(w, h, argb)} {}

void Canvas::Composite(const Image& src, int x, int y) {
    CompositeInto(im, src, x, y);
}

void Canvas::Fill(int x0, int y0, int x1, int y1, std::uint32_t rgb) {
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(static_cast<int>(im.width), x1);
    y1 = std::min(static_cast<int>(im.height), y1);
    const std::uint8_t px[4]{static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8),
                             static_cast<std::uint8_t>(rgb), 0xFF};
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            std::memcpy(im.rgba.data() + (static_cast<std::size_t>(y) * im.width + x) * 4, px, 4);
}

void Canvas::Rect(int x, int y, int w, int h, std::uint32_t rgb) {
    x = Snap(x), y = Snap(y), w = Snap(w), h = Snap(h);
    Fill(x, y, x + w, y + h, rgb);
}

void Canvas::Darken(float a, int x, int y, int w, int h) {
    if (w < 0)
        w = static_cast<int>(im.width);
    if (h < 0)
        h = static_cast<int>(im.height);
    Composite(Blank(w, h, (static_cast<std::uint32_t>(255 * a) << 24) | col::shadow), x, y);
}

Canvas::Box Canvas::Put(const Image* src, int x, int y, int s, std::string_view anchor, float alpha,
                        float grey, bool flip) {
    if (!src || src->rgba.empty())
        return {x, y, 0, 0};
    Image img = flip ? FlipX(*src) : *src;
    img = Scale(img, s);
    img = Faded(Greyed(img, grey), alpha);
    const int w = static_cast<int>(img.width), h = static_cast<int>(img.height);
    if (anchor == "c")
        anchor = "cc";
    const char v = anchor.size() > 0 ? anchor[0] : 't';
    const char hz = anchor.size() > 1 ? anchor[1] : 'l';
    const int ax = hz == 'l' ? 0 : hz == 'c' ? w / 2 : w;
    const int ay = v == 't' ? 0 : v == 'c' ? h / 2 : h;
    const int px = x - ax, py = y - ay;
    Composite(img, px, py);
    return {px, py, w, h};
}

void Canvas::Backdrop() {
    const auto t = art.Sprite("systemgfx", "mainMenuBG");
    if (!t)
        return;
    const Image s = Scale(*t, 3);
    for (int y = 0; y < static_cast<int>(im.height); y += static_cast<int>(s.height))
        for (int x = 0; x < static_cast<int>(im.width); x += static_cast<int>(s.width))
            Composite(s, x, y);
}

int Canvas::TextWidth(std::string_view s, int size) {
    int w = 0;
    for (const auto cp : Decode(s))
        if (const Glyph* g = art.GlyphFor(cp))
            w += static_cast<int>(g->advance) * size;
    return w;
}

std::string Canvas::Fit(std::string_view s, int maxw, int size) {
    if (maxw < 0 || TextWidth(s, size) <= maxw)
        return std::string{s};
    auto cps = Decode(s);
    auto with = [&](const std::vector<std::uint32_t>& v) {
        auto t = v;
        t.push_back(Ellipsis);
        return TextWidth(Encode(t), size);
    };
    while (!cps.empty() && with(cps) > maxw)
        cps.pop_back();
    while (!cps.empty() && cps.back() == ' ')
        cps.pop_back();
    cps.push_back(Ellipsis);
    return Encode(cps);
}

int Canvas::Text(std::string_view str, int x, int y, int size, std::uint32_t rgb, bool shadow,
                 char align, int maxw, float alpha) {
    const std::string s = Fit(str, maxw, size);
    const int w = TextWidth(s, size);
    if (align == 'c')
        x -= w / 2;
    else if (align == 'r')
        x -= w;
    x = Snap(x);
    struct Placed {
        const Glyph* g;
        int x, y;
    };
    std::vector<Placed> placed;
    int cx = x;
    for (const auto cp : Decode(s)) {
        const Glyph* g = art.GlyphFor(cp);
        if (!g)
            continue;
        if (!g->tile.rgba.empty()) {
            const int gx = static_cast<int>(cx + g->bearing_x * size);
            const int gy = static_cast<int>(y + (21 - g->bearing_y) * size);
            placed.push_back({g, gx, gy});
        }
        cx += static_cast<int>(g->advance) * size;
    }
    if (placed.empty())
        return cx;
    // Union masks per layer (kit draws a shadow layer and a text layer, each faded as a whole).
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -(1 << 30), by1 = -(1 << 30);
    const int off = shadow ? 3 * size : 0;
    for (const auto& p : placed) {
        bx0 = std::min(bx0, p.x);
        by0 = std::min(by0, p.y);
        bx1 = std::max(bx1, p.x + static_cast<int>(p.g->tile.width) * size + off);
        by1 = std::max(by1, p.y + static_cast<int>(p.g->tile.height) * size + off);
    }
    const int mw = bx1 - bx0, mh = by1 - by0;
    std::vector<std::uint8_t> text_mask(static_cast<std::size_t>(mw) * mh),
        shadow_mask(shadow ? text_mask.size() : 0);
    for (const auto& p : placed) {
        const Image& t = p.g->tile;
        for (std::uint32_t ty = 0; ty < t.height * size; ++ty)
            for (std::uint32_t tx = 0; tx < t.width * size; ++tx) {
                if (!t.rgba[((ty / size) * t.width + tx / size) * 4 + 3])
                    continue;
                const int mx = p.x - bx0 + static_cast<int>(tx), my = p.y - by0 + static_cast<int>(ty);
                text_mask[static_cast<std::size_t>(my) * mw + mx] = 1;
                if (shadow)
                    shadow_mask[static_cast<std::size_t>(my + off) * mw + mx + off] = 1;
            }
    }
    const auto layer = [&](const std::vector<std::uint8_t>& mask, std::uint32_t c) {
        const std::uint8_t px[4]{static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8),
                                 static_cast<std::uint8_t>(c),
                                 static_cast<std::uint8_t>(255 * std::clamp(alpha, 0.0f, 1.0f))};
        for (int my = 0; my < mh; ++my) {
            const int yy = by0 + my;
            if (yy < 0 || yy >= static_cast<int>(im.height))
                continue;
            for (int mx = 0; mx < mw; ++mx) {
                const int xx = bx0 + mx;
                if (!mask[static_cast<std::size_t>(my) * mw + mx] || xx < 0 ||
                    xx >= static_cast<int>(im.width))
                    continue;
                Over(im.rgba.data() + (static_cast<std::size_t>(yy) * im.width + xx) * 4, px);
            }
        }
    };
    if (shadow)
        layer(shadow_mask, col::shadow);
    layer(text_mask, rgb);
    return cx;
}

std::vector<std::string> Canvas::Wrap(std::string_view s, int maxw, int size) {
    std::vector<std::string> out;
    std::string cur;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ')
            ++i;
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ')
            ++j;
        if (j == i)
            break;
        const std::string word{s.substr(i, j - i)};
        const std::string t = cur.empty() ? word : cur + " " + word;
        if (TextWidth(t, size) <= maxw || cur.empty()) {
            cur = t;
        } else {
            out.push_back(cur);
            cur = word;
        }
        i = j;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

int Canvas::Paragraph(std::string_view s, int x, int y, int w, int size, std::uint32_t rgb,
                      int max_lines, int leading) {
    auto lines = Wrap(s, w, size);
    if (max_lines > 0 && static_cast<int>(lines.size()) > max_lines) {
        lines.resize(static_cast<std::size_t>(max_lines));
        auto& last = lines.back();
        last = Fit(last, w, size);
        if (last.size() < 3 || last.compare(last.size() - 3, 3, "\xE2\x80\xA6") != 0)
            last = Fit(last + "\xE2\x80\xA6", w, size);
    }
    for (std::size_t i = 0; i < lines.size(); ++i)
        Text(lines[i], x, y + static_cast<int>(i) * leading, size, rgb);
    return y + static_cast<int>(lines.size()) * leading;
}

int Canvas::Digits(long long value, int n, int x, int y, int size, std::uint32_t rgb,
                   float zero_alpha, char align) {
    std::string s = std::to_string(std::max(0LL, value));
    if (static_cast<int>(s.size()) < n)
        s.insert(0, static_cast<std::size_t>(n) - s.size(), '0');
    std::size_t nz = 0;
    while (nz < s.size() && s[nz] == '0')
        ++nz;
    if (nz == s.size())
        --nz;
    const int total = TextWidth(s, size);
    if (align == 'r')
        x -= total;
    if (nz)
        x = Text(s.substr(0, nz), x, y, size, col::muted, true, 'l', -1, zero_alpha);
    return Text(s.substr(nz), x, y, size, rgb);
}

Canvas::Box Canvas::Nine(const Ptr& a, Box box, int l, int t, int r, int b, int s, int edge,
                         float alpha, int hs, int vs) {
    if (!a || a->rgba.empty())
        return box;
    const int aw = static_cast<int>(a->width), ah = static_cast<int>(a->height);
    const int w = Snap(box.w), h = Snap(box.h);
    Image out = Blank(w, h);
    const auto piece = [&](int sx0, int sy0, int sx1, int sy1, int dx, int dy, int dw, int dh) {
        if (dw <= 0 || dh <= 0 || sx1 <= sx0 || sy1 <= sy0)
            return;
        CompositeInto(out, ResizeNearest(*a, sx0, sy0, sx1 - sx0, sy1 - sy0, dw, dh), dx, dy);
    };
    const int cw = w - s * l - s * r, ch = h - s * t - s * b;
    piece(0, 0, l, t, 0, 0, s * l, s * t);
    piece(aw - r, 0, aw, t, w - s * r, 0, s * r, s * t);
    piece(0, ah - b, l, ah, 0, h - s * b, s * l, s * b);
    piece(aw - r, ah - b, aw, ah, w - s * r, h - s * b, s * r, s * b);
    const int hx = hs < 0 ? aw / 2 - edge / 2 : hs;
    const int vy = vs < 0 ? t : vs;
    piece(hx, 0, hx + edge, t, s * l, 0, cw, s * t);
    piece(hx, ah - b, hx + edge, ah, s * l, h - s * b, cw, s * b);
    piece(0, vy, l, vy + edge, 0, s * t, s * l, ch);
    piece(aw - r, vy, aw, vy + edge, w - s * r, s * t, s * r, ch);
    const int mx = aw / 2, my = ah / 2;
    piece(mx - 2, my - 2, mx + 2, my + 2, s * l, s * t, cw, ch);
    Composite(Faded(out, alpha), Snap(box.x), Snap(box.y));
    return {Snap(box.x), Snap(box.y), w, h};
}

Canvas::Box Canvas::Nine(std::string_view alias, std::string_view name, Box box, int k, int edge,
                         int vs, bool crop, float alpha) {
    return Nine(art.Sprite(alias, name, crop), box, k, k, k, k, 3, edge, alpha, -1, vs);
}

void Canvas::SelectedFill(Box box, float alpha) {
    const auto a = art.Sprite("packedassets", "choiceSelector2", true);
    if (!a)
        return;
    const int x = Snap(box.x), y = Snap(box.y), w = Snap(box.w), h = Snap(box.h);
    constexpr int c = 4, k = 4;
    Image out = Blank(w, h);
    const int bw = std::max(U, w - c * U);
    const int mid_h = std::max(U, h - 2 * k * U);
    const int ah = static_cast<int>(a->height), aw = static_cast<int>(a->width);
    const int rows[3][4]{{0, k, 0, k * U}, {k, ah - k, k * U, mid_h}, {ah - k, ah, h - k * U, k * U}};
    for (const auto& r : rows) {
        CompositeInto(out, ResizeNearest(*a, 0, r[0], c, r[1] - r[0], c * U, r[3]), 0, r[2]);
        CompositeInto(out, ResizeNearest(*a, c, r[0], aw - c, r[1] - r[0], bw, r[3]), c * U, r[2]);
    }
    Composite(Faded(out, alpha), x, y);
}

void Canvas::InfoField(Box b, char tone, float alpha) {
    const char* name = tone == 'r' ? "partyInfoFieldBGR" : tone == 'y' ? "partyInfoFieldBGY"
                                                                        : "partyInfoFieldBGG";
    Nine(tone == 'y' ? "packedassets" : "systemgfx", name, b, 2, 4, -1, false, alpha);
}

Canvas::Box Canvas::Cursor(int x, int y, int frame, std::string_view anchor) {
    return Put(art.Sprite("systemgfx", "handCursor_" + std::to_string(frame)), x, y, 3, anchor);
}

Canvas::Box Canvas::NameRibbon(int cx, int y, std::string_view label, std::uint32_t rgb) {
    const auto a = art.Sprite("systemgfx", "commandbox_name", true);
    if (!a)
        return {cx, y, 0, 0};
    constexpr int tail = 16;
    const int aw = static_cast<int>(a->width), ah = static_cast<int>(a->height);
    const int need = TextWidth(label) / 3 + 2 * tail + 12;
    const int nw = std::max(2 * tail + 24, need);
    const int body_w = nw - 2 * tail;
    Image rib = Blank(nw, ah);
    CompositeInto(rib, Crop(*a, 0, 0, tail, ah), 0, 0);
    CompositeInto(rib, Crop(*a, aw - tail, 0, tail, ah), nw - tail, 0);
    CompositeInto(rib, ResizeNearest(*a, tail, 0, 6, ah, body_w, ah), tail, 0);
    const Box b = Put(&rib, cx, y, 3, "tc");
    Text(label, cx, b.y + (b.h - 21) / 2 - 3, 1, rgb, true, 'c');
    return b;
}

int Canvas::Bar(int x, int y, int w, double frac, char kind, int h) {
    x = Snap(x), y = Snap(y), w = Snap(w);
    const int hp = h * U;
    std::uint32_t hi = col::hp_hi, mid = col::hp_mid, lo = col::hp_lo;
    if (kind == 't')
        hi = col::tp_hi, mid = col::tp_mid, lo = col::tp_lo;
    else if (kind == 'e')
        hi = col::en_hi, mid = col::en_mid, lo = col::en_lo;
    const std::uint32_t e = col::bar_edge;
    Fill(x + U, y, x + w - U, y + U, e);
    Fill(x + U, y + hp - U, x + w - U, y + hp, e);
    Fill(x, y + U, x + U, y + hp - U, e);
    Fill(x + w - U, y + U, x + w, y + hp - U, e);
    const int ix0 = x + U, ix1 = x + w - U;
    Fill(ix0, y + U, ix1, y + hp - U, col::navy_empty);
    // kit: snap((ix1 - ix0) * frac) on a float (Python round: half to even)
    const double raw = (ix1 - ix0) * std::clamp(frac, 0.0, 1.0);
    const int fw2 = static_cast<int>(std::nearbyint(raw / 3.0)) * 3;
    if (fw2 > 0) {
        Fill(ix0, y + U, ix0 + fw2, y + 2 * U, hi);
        Fill(ix0, y + 2 * U, ix0 + fw2, y + hp - 2 * U, mid);
        Fill(ix0, y + hp - 2 * U, ix0 + fw2, y + hp - U, lo);
    }
    return y + hp;
}

void Canvas::PartnerField(Box b, bool ko) {
    const auto a = art.Sprite("systemgfx", ko ? "partyInfoFieldBGR" : "partyInfoFieldBGG");
    if (!a)
        return;
    Composite(ResizeNearest(*a, 0, 0, static_cast<int>(a->width), static_cast<int>(a->height),
                            Snap(b.w), Snap(b.h)),
              Snap(b.x), Snap(b.y));
}

int Canvas::OdGauge(int x, int y, int w, double pos, const std::vector<Zone>& zones, int h,
                    bool tick, bool marker, std::optional<std::pair<double, char>> projection) {
    x = Snap(x), y = Snap(y), w = Snap(w);
    const int hp = h * U;
    Fill(x, y, x + w, y + hp, col::gauge_edge);
    const int ix0 = x + U, ix1 = x + w - U, iw = ix1 - ix0;
    const auto snapf = [](double v) { return static_cast<int>(std::nearbyint(v / 3.0)) * 3; };
    const auto fill = [&](double a, double b, char z) {
        std::uint32_t hi = col::ne_hi, mid = col::ne_mid, lo = col::ne_lo;
        if (z == 'o')
            hi = col::hp_hi, mid = 0x0d624b, lo = col::hp_lo;
        else if (z == 'h')
            hi = col::en_hi, mid = col::en_mid, lo = col::en_lo;
        const int xa = ix0 + snapf(iw * a), xb = ix0 + snapf(iw * b);
        Fill(xa, y + U, xb, y + 2 * U, hi);
        Fill(xa, y + 2 * U, xb, y + hp - 2 * U, mid);
        Fill(xa, y + hp - 2 * U, xb, y + hp - U, lo);
    };
    fill(0, 1, 'n');
    for (const auto& z : zones)
        fill(z.a, z.b, z.kind);
    const int px = ix0 + snapf(iw * std::clamp(pos, 0.0, 1.0));
    if (tick)
        Fill(px, y, px + U, y + hp, 0xffffff);
    int bottom = y + hp;
    if (projection) {
        const char* name = projection->second == 'o'   ? "markerProjectionGreen"
                           : projection->second == 'h' ? "markerProjectionRed"
                                                       : "markerProjection";
        const int ppx = ix0 + snapf(iw * std::clamp(projection->first, 0.0, 1.0));
        const Box b = Put(art.Sprite("systemgfx", name), ppx + 1, y + hp - U * 3, 3, "tc");
        bottom = std::max(bottom, b.y + b.h);
    }
    if (marker) {
        const Box b = Put(art.Sprite("systemgfx", "marker"), px + 1, y + hp - U * 3, 3, "tc");
        bottom = std::max(bottom, b.y + b.h);
    }
    return bottom;
}

int Canvas::StatusTag(int pic, int x, int y, std::optional<int> turns) {
    const auto icon = art.Sprite("systemgfx", "icon_states_" + std::to_string(pic));
    if (!icon)
        return x;
    const Box b = Put(icon, x, y, 3);
    int end = b.x + b.w;
    if (turns)
        end = Text(std::to_string(*turns), end + 6, b.y + (b.h - 21) / 2, 1, col::ink);
    return end;
}

} // namespace ce_draw
