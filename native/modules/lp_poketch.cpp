// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lp_poketch.h"
#include "lp_poketch_display.h"
#include "lp_raster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace lp_poketch {

lp_unity::Image Enlarge(const lp_unity::Image& in, int scale1000);

struct Sprite {
    lp_unity::Image img;
    int rect_w = 0, rect_h = 0, off_x = 0, off_y = 0; // rect and tight-texture offset (y up)
};

struct SpriteCache::Impl {
    std::mutex mutex;
    std::map<std::string, std::optional<Sprite>> sprites;
    struct Icon {
        std::shared_ptr<const lp_unity::Image> image;
        std::uint64_t used = 0;
    };
    std::map<std::array<int, 3>, Icon> icons;
    std::uint64_t serial = 0;
};

SpriteCache::SpriteCache() : impl{std::make_unique<Impl>()} {}
SpriteCache::~SpriteCache() = default;
SpriteCache::Impl& SpriteCache::Data() { return *impl; }

namespace {

constexpr std::string_view Bundle = "UIs/ui/uiresidentwindow";
std::mutex surface_mutex;
std::map<int, std::vector<std::uint8_t>> surfaces;

std::vector<std::uint8_t> SurfaceCells(int id) {
    std::scoped_lock lock{surface_mutex};
    const auto it = surfaces.find(id);
    return it == surfaces.end() ? std::vector<std::uint8_t>{} : it->second;
}

// Decoded pkc_ sprites; the bundle reader caches blocks, this keeps the decoded pixels. The
// timer's button labels and the Hidden Moves names live in the language's common textures.
const Sprite* Get(lp_unity::Reader& u, SpriteCache::Impl& state, std::string_view language,
                  const std::string& name) {
    auto& cache = state.sprites;
    const bool lang_art = (name.starts_with("pkc_btn_timer_") && name.find("triangle") == std::string::npos) ||
                          name.starts_with("pkc_txt_hidden_");
    const std::string bundle = lang_art ? "UIs/textures/common_lang." + std::string{language} : std::string{Bundle};
    const std::string key = lang_art ? std::string{language} + "/" + name : name;
    std::scoped_lock lock{state.mutex};
    if (auto it = cache.find(key); it != cache.end())
        return it->second ? &*it->second : nullptr;
    std::optional<Sprite> s;
    const auto info = u.GetSprite(bundle, name);
    auto img = u.LoadSprite(bundle, name);
    // The rect and offsets are floats from the bundle: NaN, infinite or sub-pixel rects never become
    // a sprite (Put divides by the rect, Nine slices the pixels by it).
    if (info && img && lp_raster::Fits(img->width, img->height, img->rgba.size())) {
        const auto w = lp_raster::RoundPx(info->rect.width), h = lp_raster::RoundPx(info->rect.height);
        const auto ox = lp_raster::RoundPx(info->texture_rect_offset_x);
        const auto oy = lp_raster::RoundPx(info->texture_rect_offset_y);
        if (w && h && ox && oy && *w > 0 && *h > 0)
            s = Sprite{std::move(*img), *w, *h, *ox, *oy};
    }
    return (cache[key] = std::move(s)) ? &*cache[key] : nullptr;
}

std::shared_ptr<const lp_unity::Image> GetIcon(lp_unity::Reader& u, SpriteCache::Impl& state,
                                               int species, int form, int gender) {
    std::scoped_lock lock{state.mutex};
    const std::array key{species, form, gender};
    if (const auto it = state.icons.find(key); it != state.icons.end()) {
        it->second.used = ++state.serial;
        return it->second.image;
    }
    auto image = u.LoadPokemonIcon(species, form, gender, false);
    auto saved = image && image->width && image->height ?
        std::make_shared<const lp_unity::Image>(std::move(*image)) : nullptr;
    // Only the current handful of party/history/nursery icons need decoded pixels.
    // Shared ownership keeps an in-flight render valid while an older key is evicted.
    if (state.icons.size() >= 32) {
        const auto oldest = std::min_element(state.icons.begin(), state.icons.end(),
            [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        state.icons.erase(oldest);
    }
    state.icons.emplace(key, SpriteCache::Impl::Icon{saved, ++state.serial});
    return saved;
}

std::string Num(std::string_view prefix, int n, int digits = 2) {
    char b[8];
    std::snprintf(b, sizeof b, "%0*d", digits, n);
    return std::string{prefix} + b;
}

// The display before the colour pass: one grey level per pixel, white = lit background.
struct Lcd {
    lp_unity::Reader& u;
    SpriteCache::Impl& cache;
    SpriteCache* owner;
    std::string_view language;
    std::vector<std::uint8_t> g = std::vector<std::uint8_t>(Width * Height, 255);

    void Mix(int x, int y, int grey, int a) {
        if (x < 0 || y < 0 || x >= Width || y >= Height || a <= 0)
            return;
        auto& d = g[static_cast<size_t>(y) * Width + x];
        d = static_cast<std::uint8_t>((d * (255 - a) + grey * a) / 255);
    }
    void Fill(int x0, int y0, int x1, int y1, int grey) {
        // Clip once rather than iterating invisible pixels and checking each one.
        // Fill is opaque, so the visible row can be written directly.
        x0 = std::clamp(x0, 0, Width);
        x1 = std::clamp(x1, 0, Width);
        y0 = std::clamp(y0, 0, Height);
        y1 = std::clamp(y1, 0, Height);
        if (x0 >= x1 || y0 >= y1) return;
        for (int y = y0; y < y1; ++y)
            std::fill(g.begin() + y * Width + x0, g.begin() + y * Width + x1,
                      static_cast<std::uint8_t>(grey));
    }
    // Unity Image colour multiply: sprite luminance x tint grey (the art is white/grey masks).
    void Draw(const lp_unity::Image& im, int x0, int y0, int w, int h, int tint, int alpha = 255) {
        if (!lp_raster::Fits(im.width, im.height, im.rgba.size()) || w <= 0 || h <= 0)
            return;
        for (int y = 0; y < h; ++y) {
            const int sy = static_cast<int>(static_cast<long>(y) * im.height / h);
            for (int x = 0; x < w; ++x) {
                const int sx = static_cast<int>(static_cast<long>(x) * im.width / w);
                const auto* p = im.rgba.data() + (static_cast<size_t>(sy) * im.width + sx) * 4;
                const int luma = (p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8;
                Mix(x0 + x, y0 + y, luma * tint / 255, p[3] * alpha / 255);
            }
        }
    }
    // Place sprite `name` with its rect centred at (cx, cy), y down; sw/sh stretch the rect.
    void Put(const std::string& name, int cx, int cy, int tint, int sw = 0, int sh = 0, int alpha = 255) {
        const auto* s = Get(u, cache, language, name);
        if (!s || s->rect_w <= 0 || s->rect_h <= 0)
            return;
        const double fx = sw ? double(sw) / s->rect_w : 1.0, fy = sh ? double(sh) / s->rect_h : 1.0;
        const int rw = sw ? sw : s->rect_w, rh = sh ? sh : s->rect_h;
        const int tw = static_cast<int>(std::lround(s->img.width * fx)), th = static_cast<int>(std::lround(s->img.height * fy));
        const int x = cx - rw / 2 + static_cast<int>(std::lround(s->off_x * fx));
        const int y = cy + rh / 2 - static_cast<int>(std::lround(s->off_y * fy)) - th;
        Draw(s->img, x, y, tw, th, tint, alpha);
    }
    // Nine-slice (border b px kept 1:1), the prefab's sliced Image type.
    void Nine(const std::string& name, int x, int y, int w, int h, int b, int tint) {
        const auto* s = Get(u, cache, language, name);
        if (!s || w < 2 * b || h < 2 * b)
            return;
        const int W0 = static_cast<int>(s->img.width), H0 = static_cast<int>(s->img.height);
        if (!lp_raster::NineFits(W0, H0, b))
            return; // a source narrower than its two borders would slice outside its pixels
        const int xs[4] = {0, b, W0 - b, W0}, ys[4] = {0, b, H0 - b, H0};
        const int xd[4] = {x, x + b, x + w - b, x + w}, yd[4] = {y, y + b, y + h - b, y + h};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                for (int yy = yd[r]; yy < yd[r + 1]; ++yy)
                    for (int xx = xd[c]; xx < xd[c + 1]; ++xx) {
                        const int sx = xs[c] + (xx - xd[c]) * (xs[c + 1] - xs[c]) / std::max(1, xd[c + 1] - xd[c]);
                        const int sy = ys[r] + (yy - yd[r]) * (ys[r + 1] - ys[r]) / std::max(1, yd[r + 1] - yd[r]);
                        const auto* p = s->img.rgba.data() + (static_cast<size_t>(sy) * W0 + sx) * 4;
                        Mix(xx, yy, ((p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8) * tint / 255, p[3]);
                    }
    }
    // A clock hand: a w-wide bar of length len from the centre, snapped to the 4 px half-dot.
    void Hand(int cx, int cy, double deg, int len, int w, int grey) {
        const double a = deg * 3.14159265358979 / 180.0, ux = std::sin(a), uy = -std::cos(a);
        for (int y = cy - len - w; y <= cy + len + w; y += 4)
            for (int x = cx - len - w; x <= cx + len + w; x += 4) {
                const double dx = x + 2 - cx, dy = y + 2 - cy;
                const double along = dx * ux + dy * uy, across = std::abs(dx * uy - dy * ux);
                if (along >= -w / 2.0 && along <= len && across <= w / 2.0)
                    Fill(x, y, x + 4, y + 4, grey);
            }
    }
    // A party icon on the display: four grey levels of the icon's own shading.
    void Icon(int species, int form, int gender, int cx, int cy, int size, bool faint, bool mirror = false) {
        auto im = GetIcon(u, cache, species, form, gender);
        if (!im)
            return;
        const int side = static_cast<int>(std::max(im->width, im->height));
        const int w = static_cast<int>(im->width) * size / side, h = static_cast<int>(im->height) * size / side;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t sx = static_cast<size_t>(mirror ? w - 1 - x : x) * im->width / w;
                const auto* p = im->rgba.data() + ((static_cast<size_t>(y) * im->height / h) * im->width + sx) * 4;
                if (p[3] < 128)
                    continue;
                const int l = (p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8;
                int grey = l < 60 ? 0 : l < 120 ? 76 : l < 200 ? 171 : 255;
                if (faint)
                    grey = grey < 200 ? 171 : 255;
                Mix(cx - w / 2 + x, cy - h / 2 + y, grey, 255);
            }
    }
    // A flat app button (Memo / Roulette / Timer): light body, darker strip below, 6 px frame.
    void FlatButton(int x, int y, int w, int h, int strip, int strip_grey) {
        Fill(x, y, x + w, y + h, 0xde);
        Fill(x, y + h - strip, x + w, y + h, strip_grey);
        Nine("pkc_frm_6px_01", x, y, w, h, 6, 0x4c);
    }
    // A box the size of the sprite's rect, flipped vertically (the timer's down arrows).
    void PutFlippedV(const std::string& name, int cx, int cy, int tint) {
        const auto* s = Get(u, cache, language, name);
        if (!s) return;
        lp_unity::Image f = s->img;
        const size_t row = static_cast<size_t>(f.width) * 4;
        for (std::uint32_t y = 0; y < f.height / 2; ++y)
            std::swap_ranges(f.rgba.begin() + y * row, f.rgba.begin() + (y + 1) * row,
                             f.rgba.begin() + (f.height - 1 - y) * row);
        Draw(f, cx - static_cast<int>(f.width) / 2, cy - static_cast<int>(f.height) / 2,
             static_cast<int>(f.width), static_cast<int>(f.height), tint);
    }
    // A boxed touch button as the prefab builds it: a grey shadow strip under a white body with
    // the sliced black frame (Image_Shadow is the parent height minus the body, at the bottom).
    void Button(int x, int y, int w, int h, const std::string& base, int border, int shadow, int depth = 10) {
        Fill(x + border, y + h - border, x + w - border, y + h + depth, shadow);
        Fill(x + border, y + border, x + w - border, y + h - border, 255);
        Nine(base, x, y, w, h, border, 255);
    }
};

void DrawDigitalWatch(Lcd& l, int h, int m) {
    // Pikachu strip is bottom-anchored; its ground lines continue across the 640 px BG.
    if (const auto* s = Get(l.u, l.cache, l.language, "pkc_img_watch_under_01")) {
        const int sw = static_cast<int>(s->img.width), left = (Width - sw) / 2;
        for (int x = 0; x < Width; ++x) {
            const int sx = std::clamp(x - left, 0, sw - 1);
            for (int y = 0; y < static_cast<int>(s->img.height); ++y) {
                const auto* p = s->img.rgba.data() + (static_cast<size_t>(y) * sw + sx) * 4;
                l.Mix(x, Height - static_cast<int>(s->img.height) + y, 0, p[3]);
            }
        }
    }
    const int cy = 240 - 28;
    const int d[5] = {h / 10, h % 10, 10, m / 10, m % 10}, x[5] = {-214, -90, 0, 90, 214};
    for (int k = 0; k < 5; ++k)
        l.Put(Num("pkc_txt_num_01_110x220_", d[k]), 320 + x[k], cy, 0);
}

void DrawAnalogWatch(Lcd& l, int h, int m) {
    l.Put("pkc_img_analog_clock_01_01", 320, 234, 0x4c);
    l.Put("pkc_img_analog_clock_01_02", 320, 240, 0x7f);
    l.Hand(320, 240, m * 6.0, 140, 16, 0x4c);
    l.Hand(320, 240, (h % 12) * 30.0 + m / 2.0, 100, 24, 0x7f);
}

void DrawPedometer(Lcd& l, int steps) {
    steps = std::clamp(steps, 0, 99999);
    const int cy = 240 - 98;
    l.Nine("pkc_frm_pedometer_01", 100, cy - 92, 440, 184, 16, 0);
    const int x[5] = {-156, -78, 0, 78, 156};
    for (int k = 0, p = 10000; k < 5; ++k, p /= 10)
        l.Put(Num("pkc_txt_num_03_26x128_", (steps / p) % 10), 320 + x[k], cy - 1, 0);
    l.Button(320 - 98, 343 - 83, 196, 166, "pkc_btn_pedometer_base_01", 8, 0xaa);
    l.Put("pkc_btn_pedometer_clear_01", 322, 343, 0);
}

void CounterApp(Lcd& l, int n) {
    const int cy = 240 - 106;
    l.Fill(140, cy - 90, 500, cy - 84, 0xab);
    l.Fill(140, cy + 84, 500, cy + 90, 0xab);
    const int x[4] = {-116, -38, 40, 118};
    for (int k = 0, p = 1000; k < 4; ++k, p /= 10)
        l.Put(Num("pkc_txt_num_03_26x128_", (n / p) % 10), 320 + x[k], cy - 1, 0);
    l.Button(320 - 98, 334 - 83, 196, 166, "pkc_btn_pedometer_base_01", 8, 0xaa);
    l.Put("pkc_btn_plus_01_line", 322, 334, 0xab);
}

void PokemonListApp(Lcd& l, const std::vector<int>& a) {
    l.Put("pkc_deco_pokemonlist_01", 352, 256, 0);
    l.Put("pkc_deco_pokemonlist_01", 288, 256, 0);
    for (int i = 0; i < 6 && i * 6 + 5 < static_cast<int>(a.size()); ++i) {
        const int* m = a.data() + i * 6;
        if (!m[5])
            continue;
        const int cx = 92 + (i % 2 ? 356 : 100), cy = 77 + (i / 2) * 154;
        const int hop = a.size() >= 38 && a[36] == i ? std::clamp(a[37], -24, 0) : 0;
        l.Icon(m[0], m[1], m[2], cx - 4, std::max(56, cy - 14 + hop), 112, m[3] == 0);
        l.Nine("pkc_frm_hpguage_01", cx - 100, cy + 55, 200, 28, 8, 0x4d);
        const int fill = std::clamp(m[3], 0, 1000) * 180 / 1000;
        if (fill > 0)
            l.Fill(cx - 90, cy + 63, cx - 90 + fill, cy + 75, 0);
        if (m[4])
            l.Put("pkc_icon_pokemonitem_01", cx + 84, cy + 30, 0);
    }
}

void FriendshipApp(Lcd& l, const std::vector<int>& a) {
    for (int i = 0; i < 6 && i * 5 + 4 < static_cast<int>(a.size()); ++i) {
        const int* m = a.data() + i * 5;
        if (!m[4])
            continue;
        // Keep the full 140px icon and hearts inside the LCD in all three rows.
        const int cx = 320 + (i % 2 ? 104 : -104), cy = 100 + (i / 2) * 146;
        l.Put("pkc_pl_pokeshadow_01", cx, cy + 52, 0, 0, 0, 171);
        const int hop = a.size() >= 32 && a[30] == i ? std::clamp(a[31], -24, 0) : 0;
        l.Icon(m[0], m[1], m[2], cx, std::max(70, cy - 20 + hop), 140, false);
        for (int h = 0; h < std::clamp(m[3], 0, 2); ++h)
            l.Put("pkc_icon_heart_01", cx + 50 + h * 30, cy - 58 + h * 18, 255, 48, 48);
    }
}

void CoinApp(Lcd& l, int face, int frame) {
    l.Fill(0, 248 - 12, Width, 248 + 12, 0xab);
    l.Fill(0, 272 - 6, Width, 272 + 6, 0xab);
    l.Fill(0, 287 - 3, Width, 287 + 3, 0xab);
    // The five native edge/face sprites rotate while the coin rises and lands.
    static constexpr int Rise[] = {0, 64, 128, 176, 200, 208, 200, 176, 128, 64, 24, 0};
    static constexpr int Spin[] = {0, 1, 2, 3, 4, 1, 0, 1, 2, 3, 4, 1};
    const bool flipping = frame >= 0 && frame < 12;
    const int sprite = flipping && frame < 11 ? Spin[frame] : face ? 3 : 0;
    l.Put("pkc_img_coin_anim_01_" + std::to_string(sprite), 316, 376 - (flipping ? Rise[frame] : 0), 255);
}

void CalendarApp(Lcd& l, int month, int today, int first, int days, int marks) {
    l.Put("pkc_deco_calender_top_01", 320, 36, 0xab);
    if (month >= 10)
        l.Put(Num("pkc_txt_num_03_month_", month / 10), 300, 35, 255);
    l.Put(Num("pkc_txt_num_03_month_", month % 10), month >= 10 ? 340 : 320, 35, 255);
    const bool six_weeks = first + days > 35;
    for (int d = 1; d <= days; ++d) {
        const int cell = first + d - 1, col = cell % 7, row = cell / 7;
        if (row > 5)
            break;
        // Six weeks need room for the full 52px current-day frame, not just
        // the 36px numeral. Never overlap week five or clip the bottom frame.
        const int cx = 40 + 40 + col * 80;
        const int cy = (six_weeks ? 128 : 142) + row * (six_weeks ? 60 : 64);
        if (marks & (1 << (d - 1)))
            l.Put("pkc_pl_common_01", cx, cy, 0xab, 68, 52);
        if (d == today)
            l.Nine("pkc_frm_4px_01", cx - 34, cy - 26, 68, 52, 4, 0);
        const std::string set = col == 0 ? "pkc_txt_num_03_sun_" : "pkc_txt_num_03_day_";
        if (d >= 10) {
            l.Put(Num(set, d / 10), cx - 12, cy, 255);
            l.Put(Num(set, d % 10), cx + 12, cy, 255);
        } else {
            l.Put(Num(set, d), cx, cy, 255);
        }
    }
}

void ColorChangerApp(Lcd& l, int colour) {
    l.Put("pkc_img_Kecleon_01", 295, 174, 0);
    l.Put("pkc_pl_scale_01", 320, 404, 0);
    l.Put("pkc_btn_pedometer_base_02", 320 - 182 + colour * 52, 404, 255);
}

void CalculatorApp(Lcd& l, int op, const std::vector<int>& codes) {
    l.Nine("pkc_frm_4px_01", 53, 12, 534, 68, 4, 0);
    l.Put(Num("pkc_txt_num_02_32x36_", op >= 12 && op <= 15 ? op : 17), 92, 46, 0, 0, 0, op >= 12 ? 255 : 64);
    int x = 587 - 32;
    for (auto it = codes.rbegin(); it != codes.rend(); ++it, x -= 40)
        l.Put(Num("pkc_txt_num_02_32x36_", std::clamp(*it, 0, 18)), x, 46, 0);
    for (const auto& k : CalcKeys) {
        const int cx = 320 + k.x, cy = 284 - k.y;
        l.Button(cx - k.w / 2, cy - 44, k.w, 80, "pkc_btn_calculator_base_01", 4, 0xa8, 8);
        l.Put(Num("pkc_txt_num_02_32x36_", k.code), cx - k.w / 2 + 48, cy - 2, 0);
    }
}

void MemoApp(Lcd& l, int tool) {
    // canvas (white), ruler stripes along its foot, the tool column on the right
    const auto cells = SurfaceCells(MemoSurface);
    for (int y = 0; y < MemoRows; ++y)
        for (int x = 0; x < MemoCols; ++x)
            if (static_cast<size_t>(y * MemoCols + x) < cells.size() && cells[y * MemoCols + x])
                l.Fill(x * MemoCell, y * MemoCell, (x + 1) * MemoCell, (y + 1) * MemoCell, 0);
    for (int k = 0; k < 5; ++k)
        l.Fill(0, 450 + k * 6, 510, 456 + k * 6, k % 2 ? 0xde : 0x7f);
    l.Fill(510, 0, Width, Height, 0xab);
    l.Fill(510, 0, 516, Height, 0x4c);
    const struct { const char* icon; int y; } tools[2] = {{"pkc_bt_icon_pencil_01", 248}, {"pkc_bt_icon_eraser_01", 12}};
    for (int t = 0; t < 2; ++t) {
        l.FlatButton(527, tools[t].y, 96, 220, 18, 0xab);
        l.Put(tools[t].icon, 575, tools[t].y + 106, 255);
        if (tool != t)
            for (int y = tools[t].y; y < tools[t].y + 220; ++y)
                for (int x = 527; x < 623; ++x) l.Mix(x, y, 0, 77);
    }
}

void RouletteApp(Lcd& l, int angle) {
    l.Fill(0, 0, 510, Height, 0x7f);
    l.Fill(516, 0, Width, Height, 0xab);
    l.Fill(510, 0, 516, Height, 0x4c);
    l.Put("pkc_img_roulette_01_base", 256, 240, 0xde, 460, 460);
    const auto cells = SurfaceCells(RouletteSurface);
    for (int y = 0; y < WheelRows; ++y)
        for (int x = 0; x < WheelCols; ++x)
            if (static_cast<size_t>(y * WheelCols + x) < cells.size() && cells[y * WheelCols + x])
                l.Fill(26 + x * WheelCell, 10 + y * WheelCell, 26 + (x + 1) * WheelCell, 10 + (y + 1) * WheelCell, 0x4c);
    l.Put("pkc_img_roulette_01_line", 256, 240, 0x4c, 460, 460);
    // Rotate native arrow pixels before applying the fixed monitor texture.
    // Rotating a textured widget would incorrectly rotate its LCD decoration.
    if (const auto arrow = RenderArrow(l.u, 7, 1000, l.owner)) {
        const double radians = angle * 3.141592653589793 / 180.0;
        const double cs = std::cos(radians), sn = std::sin(radians);
        for (int y = 0; y < 88; ++y) for (int x = 0; x < 88; ++x) {
            const int sx = static_cast<int>(std::lround((x - 43.5) * cs + (y - 43.5) * sn + 43.5));
            const int sy = static_cast<int>(std::lround(-(x - 43.5) * sn + (y - 43.5) * cs + 43.5));
            if (sx < 0 || sy < 0 || sx >= 88 || sy >= 88) continue;
            const auto* pixel = arrow->rgba.data() + (sy * 88 + sx) * 4;
            // Colour 7 is neutral grey (173): undo its tint before the LCD pass.
            l.Mix(212 + x, 196 + y, std::min(255, pixel[0] * 255 / 173), pixel[3]);
        }
    }
    const struct { const char* icon; int cy; } buttons[3] = {
        {"pkc_btn_roulette_01_start", 158}, {"pkc_btn_roulette_01_stop", 294}, {"pkc_btn_roulette_01_clear", 430}};
    for (const auto& b : buttons) {
        l.FlatButton(530, b.cy - 54, 96, 108, 18, 0xab);
        l.Put(b.icon, 578, b.cy - 6, 0x7f);
    }
}

void KitchenTimerApp(Lcd& l, int mm, int ss, int running, int alarm) {
    const int f = alarm >= 0 ? alarm % 4 : 0;
    const std::string n = std::to_string(f);
    l.Put("pkc_img_Snorlax_anim_01_" + n, 320, 195, 0x4c);
    l.Put("pkc_img_Snorlax_anim_03_" + n, 320, 195, 0xde);
    l.Put("pkc_img_Snorlax_anim_02_" + n, 320, 195, 0xab);
    const int cy = 288;
    const int digits[5] = {mm / 10, mm % 10, 10, ss / 10, ss % 10}, xs[5] = {-96, -46, 0, 46, 96};
    for (int k = 0; k < 5; ++k)
        l.Put(Num("pkc_txt_num_05_36x88_", digits[k]), 320 + xs[k], cy, 0x4c);
    if (!running && alarm < 0)
        for (int k : {0, 1, 3, 4}) {
            l.Put("pkc_btn_timer_triangle_01_base", 320 + xs[k], cy - 66, 0xab);
            l.Put("pkc_btn_timer_triangle_01_line", 320 + xs[k], cy - 66, 0x4c);
            l.PutFlippedV("pkc_btn_timer_triangle_01_base", 320 + xs[k], cy + 66, 0xab);
            l.PutFlippedV("pkc_btn_timer_triangle_01_line", 320 + xs[k], cy + 66, 0x4c);
        }
    l.Fill(0, 390, Width, Height, 0xab);
    const struct { const char* text; int cx; } buttons[3] = {
        {"pkc_btn_timer_start_01", 106}, {"pkc_btn_timer_stop_01", 320}, {"pkc_btn_timer_reset_01", 534}};
    for (const auto& b : buttons) {
        l.FlatButton(b.cx - 106, 392, 212, 84, 12, 0x7f);
        l.Put(b.text, b.cx - 2, 430, 255);
    }
}

void DowsingApp(Lcd& l) {
    l.Put("pkc_deco_target_01", 320, 240, 0, 640, 360);
    // the 640x121 band art: its top half caps the screen, its bottom half the foot (sliced 640x480)
    if (const auto* s = Get(l.u, l.cache, l.language, "pkc_deco_top_under_01")) {
        const int h = static_cast<int>(s->img.height), half = h / 2;
        lp_unity::Image top{s->img.width, static_cast<std::uint32_t>(half),
                            std::vector<std::uint8_t>(s->img.rgba.begin(), s->img.rgba.begin() + s->img.width * 4 * half)};
        lp_unity::Image bottom{s->img.width, static_cast<std::uint32_t>(h - half),
                               std::vector<std::uint8_t>(s->img.rgba.begin() + s->img.width * 4 * half, s->img.rgba.end())};
        l.Draw(top, 0, 0, Width, half, 0);
        l.Draw(bottom, 0, Height - (h - half), Width, h - half, 0);
    }
}

void DotArtApp(Lcd& l) {
    static constexpr int Grey[4] = {255, 170, 128, 77};
    const auto dots = SurfaceCells(DotArtSurface);
    for (int i = 0; i < 768 && i < static_cast<int>(dots.size()); ++i)
        if (dots[i]) l.Fill(i % 32 * 20, i / 32 * 20, i % 32 * 20 + 20, i / 32 * 20 + 20, Grey[dots[i] & 3]);
}

void DowsingOverlay(Lcd& l, const std::vector<int>& a) {
    if (a.size() < 4) return;
    const int radius = std::clamp(a[0], 0, 200), cx = a[1], cy = a[2];
    if (radius > 0)
        for (int y = std::max(60, cy-radius); y < std::min(420, cy+radius); ++y)
            for (int x = std::max(0, cx-radius); x < std::min(Width, cx+radius); ++x) {
                const double d = std::hypot(x+0.5-cx, y+0.5-cy);
                if (d <= radius && d >= std::max(0, radius-8)) l.Mix(x,y,0x4d,255);
            }
    for (int i = 0; i < std::min(a[3], 8) && 5+2*i < static_cast<int>(a.size()); ++i) {
        const int mx=a[4+2*i], my=a[5+2*i];
        for (int y = std::max(60,my-20); y < std::min(420,my+20); ++y)
            for (int x = std::max(0,mx-20); x < std::min(Width,mx+20); ++x)
                if (std::hypot(x+0.5-mx,y+0.5-my) < 19) l.Mix(x,y,0,255);
    }
}

void HistoryApp(Lcd& l, const std::vector<int>& a) {
    for (size_t i = 0; i + 1 < a.size() && i / 2 < 12; i += 2)
        if (a[i] > 0)
            l.Icon(a[i], a[i + 1], 0, 80 + 160 * static_cast<int>(i / 2 % 4),
                   108 + 148 * static_cast<int>(i / 2 / 4) +
                   (a.size() >= 26 && a[24] == static_cast<int>(i / 2) ? std::clamp(a[25], -24, 0) : 0), 128, false);
}

void MarkingMapApp(Lcd& l, const std::vector<int>& a) {
    auto arg = [&](size_t i) { return i < a.size() ? a[i] : -1; };
    l.Put("pkc_img_map_01", 314, 234, 0);
    // hidden parts: Fullmoon Island, Flower Paradise, Newmoon Island, Spring Path (works 278-281)
    const int bits = arg(7);
    if (bits & 1) l.Put("pkc_img_map_04", 320 - 235, 240 - 150, 0);
    if (bits & 8) l.Put("pkc_img_map_03", 320 + 258, 240 - 110, 0);
    if (bits & 2) l.Put("pkc_img_map_04", 320 - 184, 240 - 150, 0);
    if (bits & 4) l.Put("pkc_img_map_05", 320 + 183, 240 + 79, 0);
    for (int k = 0; k < 6; ++k) {
        const int x = arg(8 + 2 * k), y = arg(9 + 2 * k);
        const bool placed = x > 0 && y > 0;
        l.Put(Num("pkc_icon_map_mark_01_", k + 1), placed ? x : 365 + 51 * k, placed ? y : 455, 255);
        if (arg(20) == k) {
            const int mx = placed ? x : 365 + 51*k, my = placed ? y : 455;
            for (int yy=my-20; yy<my+20; ++yy) for (int xx=mx-20; xx<mx+20; ++xx)
                l.Mix(xx,yy,255,128);
        }
    }
    const int phase = arg(6);
    if (phase == 0 && arg(0) >= 0) l.Put("pkc_icon_map_cursor_01", arg(0), arg(1), 0, 36, 36); // dark: the light sprite vanishes on the display
    if (phase == 1 && arg(2) >= 0) l.Put("pkc_icon_map_pokemon_01", arg(2), arg(3), 255);
    if (phase == 2 && arg(4) >= 0) l.Put("pkc_icon_map_pokemon_01", arg(4), arg(5), 255);
}

void ChainCounterApp(Lcd& l, const std::vector<int>& a) {
    auto arg = [&](size_t i) { return i < a.size() ? a[i] : 0; };
    l.Fill(0, 0, Width, 132, 0xab);
    l.Put("pkc_img_podium_01_front", 320, 368, 0xab);
    l.Put("pkc_img_podium_01_top", 320, 368, 0x7f);
    l.Put("pkc_img_podium_01_line", 320, 368, 0x4c);
    auto number = [&](int n, int x, int y) {  // 4 digits, leading zeros hidden, 2x the 12x18 glyphs
        n = std::clamp(n, 0, 9999);
        const std::string s = std::to_string(n);
        for (size_t i = 0; i < s.size(); ++i)
            l.Put(Num("pkc_txt_num_04_12x18_", s[i] - '0'), x + static_cast<int>(i) * 26, y, 0, 24, 36);
    };
    if (arg(0) > 0) {
        l.Icon(arg(0), 0, 0, 230, 66, 112, false);
        number(arg(1), 320, 66);
    }
    const int px[3] = {320, 536, 104}, py[3] = {228, 278, 298};
    for (int k = 0; k < 3; ++k) {
        const int mons = arg(2 + 2 * k), count = arg(3 + 2 * k);
        if (mons <= 0) continue;
        l.Icon(mons, 0, 0, px[k], py[k], 112, false);
        number(count, px[k] - 13 * static_cast<int>(std::to_string(std::clamp(count, 0, 9999)).size()) + 13, py[k] + 80);
    }
}

void EggMonitorApp(Lcd& l, const std::vector<int>& a, const lp_unity::Image* title) {
    auto arg = [&](size_t i) { return i < a.size() ? a[i] : 0; };
    l.Put("pkc_deco_cloud_01", 314, 105, 255, 630, 144);
    for (int x = 0; x < Width; x += 66) l.Put("pkc_deco_fence_01", x + 33, 330, 255);  // a tiled fence row
    const int tw = title ? std::min(520, static_cast<int>(title->width)) : 260;
    l.Put("pkc_deco_doorplate_01", 320, 50, 255, tw + 40, 80);
    if (title) l.Draw(*title, 320-tw/2, 26, tw, 48, 255);
    for (int i = 0; i < 2; ++i)
        if (arg(i * 4 + 3)) l.Icon(arg(i * 4), arg(i * 4 + 1), arg(i * 4 + 2), i ? 480 : 160,
                                  300 + (a.size() >= 11 && arg(9) == i ? std::clamp(arg(10), -24, 0) : 0), 150, false, i == 0);
    if (arg(8)) l.Icon(0, 0, 0, 320, 380 + (a.size() >= 11 && arg(9) == 2 ? std::clamp(arg(10), -24, 0) : 0), 110, false);
}

void HiddenMovesApp(Lcd& l, const std::vector<int>& a) {
    l.Put("pkc_txt_hidden_01_00", 320, 66, 0, 388, 64);
    l.Fill(126, 96, 514, 102, 0); // span the full 388px title, centred on the LCD
    static constexpr int Name[8] = {1, 5, 2, 6, 3, 7, 4, 8};
    for (int b = 0; b < 8 && b < static_cast<int>(a.size()); ++b) {
        if (a[b] <= 0) continue;
        const auto* s = Get(l.u, l.cache, l.language, Num("pkc_txt_hidden_01_", Name[b]));
        const int w = s ? s->rect_w * 2 : 288, h = s ? s->rect_h * 2 : 64;
        l.Put(Num("pkc_txt_hidden_01_", Name[b]), (b % 2 ? 501 : 177) + 16, 171 + 78 * (b / 2), a[b] == 2 ? 0 : 0xab, w, h);
    }
}

} // namespace

void Calc::Press(int code) {
    if (code < 0 || code > 16) return;
    auto value = [&] { return std::strtod(entry.c_str(), nullptr); };
    auto show = [&](double v) {
        if (!std::isfinite(v) || std::abs(v) >= 1e10) {
            error = true;
            entry = "0";
            return;
        }
        char b[32];
        std::snprintf(b, sizeof b, "%.10g", v);
        std::string s = b;
        if (s.find('e') != std::string::npos) {
            std::snprintf(b, sizeof b, "%.9f", v);
            s = b;
        }
        if (s.find('.') != std::string::npos) {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        const size_t digits = std::count_if(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (digits > 10 && s.find('.') != std::string::npos)
            s.resize(s.size() - (digits - 10));
        if (s == "-0") s = "0";
        entry = s;
    };
    if (code == 11) { *this = Calc{}; return; }
    if (error) return;
    if (code <= 9 || code == 10) {
        if (fresh) { entry = code == 10 ? "0." : std::string(1, char('0' + code)); fresh = false; return; }
        const size_t digits = std::count_if(entry.begin(), entry.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (digits >= 10) return;
        if (code == 10) { if (entry.find('.') == std::string::npos) entry += '.'; return; }
        if (entry == "0") entry.clear();
        entry += char('0' + code);
        return;
    }
    auto apply = [&] {
        const double v = value();
        double r = v;
        if (op == 12) r = acc + v;
        else if (op == 13) r = acc - v;
        else if (op == 14) r = acc * v;
        else if (op == 15) { if (v == 0) { error = true; entry = "0"; return; } r = acc / v; }
        show(r);
        acc = value();
    };
    if (code >= 12 && code <= 15) {
        if (op >= 0 && !fresh) apply(); else acc = value();
        op = code; fresh = true;
        return;
    }
    if (code == 16 && op >= 0) {
        apply();
        op = -1; fresh = true;
    }
}

std::vector<int> Calc::Codes() const {
    if (error) return {18};
    std::vector<int> out;
    for (char c : entry) {
        if (c >= '0' && c <= '9') out.push_back(c - '0');
        else if (c == '.') out.push_back(10);
        else if (c == '-') out.push_back(13);
    }
    // 11 glyphs fit before the operator glyph: drop trailing fraction digits, never the sign.
    if (out.size() > 11) {
        out.resize(11);
        if (out.back() == 10) out.pop_back();
    }
    return out;
}

std::vector<std::uint8_t> UnpackDotArt(const std::uint8_t* data) {
    std::vector<std::uint8_t> dots(768);
    for (int k = 0; k < 192; ++k)
        for (int j = 0; j < 4; ++j) dots[4 * k + j] = (data[k] >> (2 * j)) & 3;
    return dots;
}

std::vector<std::uint8_t> DefaultDotArt() {
    // "Touch!", rows 8-15, value 3 (contracts/poketch-apps.md, InitializeColorIndexBuffer)
    static constexpr const char* Rows[8] = {
        ".33333........................3.", "...3....................3.....3.", "...3....................3.....3.",
        "...3..33333.3...3.33333.33333.3.", "...3..3...3.3...3.3.....3...3.3.", "...3..3...3.3...3.3.....3...3.3.",
        "...3..3...3.3...3.3.....3...3...", "...3..33333.33333.33333.3...3.3."};
    std::vector<std::uint8_t> dots(768);
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 32; ++c) dots[(8 + r) * 32 + c] = Rows[r][c] == '3' ? 3 : 0;
    return dots;
}

void SetSurface(int id, std::vector<std::uint8_t> cells) {
    std::scoped_lock lock{surface_mutex};
    surfaces[id] = std::move(cells);
}

void ResetSurfaces() {
    std::scoped_lock lock{surface_mutex};
    surfaces.clear();
}

std::optional<lp_unity::Image> RenderArrow(lp_unity::Reader& unity, int colour, int scale1000,
                                           SpriteCache* cache) {
    if (scale1000 < 1000 || scale1000 > 2000 ||
        colour < 0 || colour >= static_cast<int>(Colours.size())) return std::nullopt;
    std::optional<SpriteCache> local;
    if (!cache) { local.emplace(); cache = &*local; }
    // Composite on a transparent 88x88: grey levels and coverage, then the display colour.
    constexpr int S = 88;
    std::vector<int> grey(S * S, 255), alpha(S * S, 0);
    const struct { const char* name; int tint; } layers[3] = {
        {"pkc_img_roulette_02_02", 0x7f}, {"pkc_img_roulette_02_03", 0xde}, {"pkc_img_roulette_02_01", 0x4c}};
    for (const auto& layer : layers) {
        const auto* s = Get(unity, cache->Data(), "en", layer.name);
        if (!s) continue;
        const int ox = (S - s->rect_w) / 2 + s->off_x, oy = (S - s->rect_h) / 2 + (s->rect_h - s->off_y - static_cast<int>(s->img.height));
        for (std::uint32_t y = 0; y < s->img.height; ++y)
            for (std::uint32_t x = 0; x < s->img.width; ++x) {
                const auto* p = s->img.rgba.data() + (static_cast<size_t>(y) * s->img.width + x) * 4;
                const int dx = ox + static_cast<int>(x), dy = oy + static_cast<int>(y);
                if (dx < 0 || dy < 0 || dx >= S || dy >= S || p[3] == 0) continue;
                const int g = ((p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8) * layer.tint / 255;
                auto& G = grey[dy * S + dx];
                G = (G * (255 - p[3]) + g * p[3]) / 255;
                alpha[dy * S + dx] = std::max<int>(alpha[dy * S + dx], p[3]);
            }
    }
    const auto& c = Colours[colour];
    lp_unity::Image out{S, S, std::vector<std::uint8_t>(S * S * 4)};
    for (int i = 0; i < S * S; ++i) {
        out.rgba[i * 4 + 0] = static_cast<std::uint8_t>(grey[i] * c[0] / 255);
        out.rgba[i * 4 + 1] = static_cast<std::uint8_t>(grey[i] * c[1] / 255);
        out.rgba[i * 4 + 2] = static_cast<std::uint8_t>(grey[i] * c[2] / 255);
        out.rgba[i * 4 + 3] = static_cast<std::uint8_t>(alpha[i]);
    }
    return Enlarge(out, scale1000);
}

std::optional<lp_unity::Image> RenderRing(int colour, bool dot, int scale1000) {
    if (scale1000 < 1000 || scale1000 > 2000 ||
        colour < 0 || colour >= static_cast<int>(Colours.size())) return std::nullopt;
    const int S = dot ? 40 : 400;
    const double r = S / 2.0;
    const int grey = dot ? 0 : 0x4d;
    const auto& c = Colours[colour];
    lp_unity::Image out{static_cast<std::uint32_t>(S), static_cast<std::uint32_t>(S), std::vector<std::uint8_t>(S * S * 4)};
    for (int y = 0; y < S; y += 8)
        for (int x = 0; x < S; x += 8) {
            const double d = std::hypot(x + 4 - r, y + 4 - r);
            if (d > r - 1 || (!dot && d < r - 11)) continue;
            for (int yy = y; yy < y + 8; ++yy)
                for (int xx = x; xx < x + 8; ++xx) {
                    auto* p = out.rgba.data() + (static_cast<size_t>(yy) * S + xx) * 4;
                    p[0] = static_cast<std::uint8_t>(grey * c[0] / 255);
                    p[1] = static_cast<std::uint8_t>(grey * c[1] / 255);
                    p[2] = static_cast<std::uint8_t>(grey * c[2] / 255);
                    p[3] = 255;
                }
        }
    return Enlarge(out, scale1000);
}

std::string Key(const std::vector<int>& v) {
    std::string k = "module:lp:poketch";
    for (int x : v) k += "/" + std::to_string(x);
    return k;
}

// Nearest-neighbour enlargement of an RGBA picture (the overlays).
lp_unity::Image Enlarge(const lp_unity::Image& in, int scale1000) {
    if (scale1000 == 1000) return in;
    const std::uint32_t w = in.width * scale1000 / 1000, h = in.height * scale1000 / 1000;
    lp_unity::Image out{w, h, std::vector<std::uint8_t>(static_cast<size_t>(w) * h * 4)};
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            std::memcpy(out.rgba.data() + (static_cast<size_t>(y) * w + x) * 4,
                        in.rgba.data() + (static_cast<size_t>(y * 1000 / scale1000) * in.width + x * 1000 / scale1000) * 4, 4);
    return out;
}

std::optional<lp_unity::Image> Render(lp_unity::Reader& unity, const std::vector<int>& v, int scale1000,
                                      std::string_view lang, SpriteCache* cache, const lp_unity::Image* nursery_title) {
    if (scale1000 < 1000 || scale1000 > 2000) return std::nullopt;
    if (lang.size() != 2) lang = "en";
    if (v.size() < 2 || v[0] < 0 || v[0] >= AppCount || v[1] < 0 || v[1] >= static_cast<int>(Colours.size()))
        return std::nullopt;
    std::optional<SpriteCache> local;
    if (!cache) { local.emplace(); cache = &*local; }
    Lcd l{unity, cache->Data(), cache, lang};
    const std::vector<int> a(v.begin() + 2, v.end());
    auto arg = [&](size_t i) { return i < a.size() ? a[i] : 0; };
    // A month has at most 31 mark bits. Validate before adding the weekday
    // offset or shifting, including keys received before calendar data is ready.
    if (v[0] == Calendar && (a.size() < 5 || arg(0) < 1 || arg(0) > 12 ||
        arg(1) < 0 || arg(1) > 31 || arg(2) < 0 || arg(2) > 6 ||
        arg(3) < 1 || arg(3) > 31)) return std::nullopt;
    switch (v[0]) {
    case DigitalWatch: DrawDigitalWatch(l, arg(0), arg(1)); break;
    case AnalogWatch: DrawAnalogWatch(l, arg(0), arg(1)); break;
    case Calculator: CalculatorApp(l, arg(0), std::vector<int>(a.begin() + std::min<size_t>(1, a.size()), a.end())); break;
    case Pedometer: DrawPedometer(l, arg(0)); break;
    case Counter: CounterApp(l, arg(0)); break;
    case PokemonList: PokemonListApp(l, a); break;
    case Friendship: FriendshipApp(l, a); break;
    case CoinToss: CoinApp(l, arg(0), a.size() > 1 ? arg(1) : -1); break;
    case Calendar: CalendarApp(l, arg(0), arg(1), arg(2), arg(3), arg(4)); break;
    case ColorChanger: ColorChangerApp(l, v[1] == 8 ? 0 : v[1]); break;
    case MemoPad: MemoApp(l, arg(0)); break;
    case Dowsing: DowsingApp(l); DowsingOverlay(l,a); break;
    case EggMonitor: EggMonitorApp(l, a, nursery_title); break;
    case History: HistoryApp(l, a); break;
    case MarkingMap: MarkingMapApp(l, a); break;
    case DotArtist: DotArtApp(l); break;
    case ChainCounter: ChainCounterApp(l, a); break;
    case HiddenMoves: HiddenMovesApp(l, a); break;
    case Roulette: RouletteApp(l, arg(1)); break;
    case KitchenTimer: KitchenTimerApp(l, arg(0), arg(1), arg(2), a.size() > 3 ? a[3] : -1); break;
    default: break;
    }
    return Display(l.g, Colours[v[1]], scale1000);
}

} // namespace lp_poketch
