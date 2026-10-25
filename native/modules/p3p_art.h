// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The game's own camp-menu art for the P3P companion, read from the user's romfs (asset-free
// package) and served through load_image as "module:p3p:<key>":
//   fill               8x8 white, tinted by the manifest for themed panels
//   bg/<route>         1240x1080 page background in the camp menu's style (0 male/blue, 1 female/pink)
//   spr/<n>            sprite n of the camp menu sheet (data_EN/umd0.cpk -> init_free.bin ->
//                      init/camp.bin -> c_main_01.spr); white with alpha, tinted at runtime
//   persona/<id>       Persona artwork, 1024x1024 (data/umd0.cpk camp/persona/pst_<ID>.tmx)
//   bust/<n>           camp portrait (camp/bustup/c_cha<NN>a.tmx; 01 male lead, 99 female lead)
// Any key may end in "@<h>": the picture is resampled to h pixels high (aspect kept), because the
// host's image blit samples nearest-neighbour and the HD art is drawn far below its own size.
//
// Formats:
//   TMX   "TMX0" at +8; +0x10 u8 palette count, u8 palette format, u16 w, u16 h, u8 format;
//         data at +0x40. 0x00 RGBA32, 0x13 8-bit indexed (CSM1 palette: swap index bits 3/4),
//         0x14 4-bit indexed (low nibble first). PS2 alpha 0..0x80. No swizzle.
//   SPR0  (P3P variant, "SPR0" at +8) +0x14 u16 texture count, u16 sprite count, u32 texture
//         table, u32 sprite table; both tables hold (u32 0, u32 offset). Sprite record 0x80 bytes:
//         +0x14 texture index, +0x54 u32 x1, y1, x2, y2 in PSP texels (the HD textures are 4x).
#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/modules/dsmod_module_sdk.h"
#include "p5r_romfs_assets.h"

namespace p3p_art {

struct Image {
    uint32_t w{}, h{};
    std::vector<uint8_t> rgba;
};

inline uint32_t Le16(std::span<const uint8_t> d, size_t at) { return d[at] | d[at + 1] << 8; }
inline uint32_t Le32(std::span<const uint8_t> d, size_t at) {
    return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | uint32_t(d[at + 3]) << 24;
}
inline uint8_t Alpha(uint8_t a) { return uint8_t(std::min(255, a * 2)); }

/// One TMX texture starting at `at`; false for an unknown format or a short buffer.
inline bool DecodeTmx(std::span<const uint8_t> d, size_t at, Image& out) {
    if (at > d.size() || d.size() - at < 0x40 || std::memcmp(&d[at + 8], "TMX0", 4)) return false;
    const uint32_t palettes = d[at + 0x10], palette_format = d[at + 0x11];
    const uint32_t w = Le16(d, at + 0x12), h = Le16(d, at + 0x14), format = d[at + 0x16];
    if (!w || !h || w > 4096 || h > 4096) return false;
    const size_t pixels = size_t(w) * h, data = at + 0x40, avail = d.size() - data;
    out.w = w; out.h = h; out.rgba.resize(pixels * 4);
    if (format == 0x00) {
        if (avail < pixels * 4) return false;
        std::memcpy(out.rgba.data(), &d[data], pixels * 4);
        for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = Alpha(out.rgba[i]);
        return true;
    }
    if ((format != 0x13 && format != 0x14) || palette_format != 0) return false;
    const size_t colors = format == 0x13 ? 256 : 16, table = colors * 4 * std::max<uint32_t>(1, palettes);
    const size_t index_bytes = format == 0x13 ? pixels : (pixels + 1) / 2;
    if (avail < table + index_bytes) return false;
    uint8_t palette[256][4];
    for (size_t i = 0; i < colors; ++i) {
        // CSM1 stores 256-colour palettes in 8-entry stripes: entry i lives at i with bits 3/4 swapped.
        const size_t src = colors == 256 ? (i & ~size_t(0x18)) | ((i & 8) << 1) | ((i & 16) >> 1) : i;
        std::memcpy(palette[i], &d[data + src * 4], 4);
        palette[i][3] = Alpha(palette[i][3]);
    }
    const size_t indices = data + table;
    for (size_t p = 0; p < pixels; ++p) {
        const uint8_t v = format == 0x13 ? d[indices + p] : (d[indices + p / 2] >> (p & 1 ? 4 : 0)) & 15;
        std::memcpy(&out.rgba[p * 4], palette[v], 4);
    }
    return true;
}

struct Sprite {
    uint32_t texture{}, x1{}, y1{}, x2{}, y2{};
};
constexpr uint32_t SpriteScale = 4; // HD textures are 4x the PSP layout the records use

/// Texture offsets and sprite records of an SPR0 sheet.
inline bool ParseSpr(std::span<const uint8_t> d, std::vector<size_t>& textures, std::vector<Sprite>& sprites) {
    if (d.size() < 0x20 || std::memcmp(&d[8], "SPR0", 4)) return false;
    const uint32_t tex_count = Le16(d, 0x14), spr_count = Le16(d, 0x16);
    const uint32_t tex_table = Le32(d, 0x18), spr_table = Le32(d, 0x1c);
    if (tex_table > d.size() || (d.size() - tex_table) / 8 < tex_count ||
        spr_table > d.size() || (d.size() - spr_table) / 8 < spr_count)
        return false;
    textures.clear(); sprites.clear();
    for (uint32_t i = 0; i < tex_count; ++i) {
        const uint32_t at = Le32(d, tex_table + 8 * i + 4);
        if (at >= d.size()) return false;
        textures.push_back(at);
    }
    for (uint32_t i = 0; i < spr_count; ++i) {
        const uint32_t at = Le32(d, spr_table + 8 * i + 4);
        if (at > d.size() || d.size() - at < 0x80) return false;
        sprites.push_back({Le32(d, at + 0x14), Le32(d, at + 0x54), Le32(d, at + 0x58), Le32(d, at + 0x5c), Le32(d, at + 0x60)});
    }
    return true;
}

/// Sprite n of a sheet, cropped out of its texture.
inline bool SpriteImage(std::span<const uint8_t> sheet, uint32_t n, Image& out) {
    std::vector<size_t> textures;
    std::vector<Sprite> sprites;
    if (!ParseSpr(sheet, textures, sprites) || n >= sprites.size()) return false;
    const Sprite s = sprites[n];
    Image tex;
    if (s.texture >= textures.size() || s.x2 <= s.x1 || s.y2 <= s.y1 || !DecodeTmx(sheet, textures[s.texture], tex))
        return false;
    const uint32_t x = s.x1 * SpriteScale, y = s.y1 * SpriteScale;
    const uint32_t w = std::min(s.x2 * SpriteScale, tex.w) - std::min(x, tex.w);
    const uint32_t h = std::min(s.y2 * SpriteScale, tex.h) - std::min(y, tex.h);
    if (!w || !h) return false;
    out.w = w; out.h = h; out.rgba.resize(size_t(w) * h * 4);
    for (uint32_t r = 0; r < h; ++r)
        std::memcpy(&out.rgba[size_t(r) * w * 4], &tex.rgba[(size_t(y + r) * tex.w + x) * 4], size_t(w) * 4);
    return true;
}

/// Area-average resample to h pixels high, aspect kept (premultiplied, so transparent edges do not
/// darken). Each output pixel averages the source box it covers, with fractional edge weights.
inline void Resize(Image& img, uint32_t h) {
    if (!img.w || !img.h || !h || h == img.h) return;
    const uint32_t w = std::max<uint32_t>(1, uint32_t((uint64_t(img.w) * h + img.h / 2) / img.h));
    // Separable: output pixel o covers source [o*scale, (o+1)*scale), weighted by overlap.
    struct Tap { uint32_t first; std::vector<float> weight; };
    auto taps = [](uint32_t src, uint32_t dst) {
        std::vector<Tap> out(dst);
        const double scale = double(src) / dst;
        for (uint32_t o = 0; o < dst; ++o) {
            const double a = o * scale, b = std::min<double>(src, (o + 1) * scale);
            const uint32_t i0 = std::min<uint32_t>(src - 1, uint32_t(a));
            const uint32_t i1 = std::clamp<uint32_t>(uint32_t(std::ceil(b)), i0 + 1, src);
            double total = 0;
            out[o].first = i0;
            for (uint32_t i = i0; i < i1; ++i) {
                const double wgt = std::min<double>(b, i + 1) - std::max<double>(a, i);
                out[o].weight.push_back(float(std::max(0.0, wgt)));
                total += std::max(0.0, wgt);
            }
            for (auto& v : out[o].weight) v = total > 0 ? float(v / total) : 1.0f / float(out[o].weight.size());
        }
        return out;
    };
    const auto tx = taps(img.w, w), ty = taps(img.h, h);
    std::vector<float> rows(size_t(w) * img.h * 4); // horizontal pass, premultiplied
    for (uint32_t y = 0; y < img.h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float acc[4]{};
            for (size_t k = 0; k < tx[x].weight.size(); ++k) {
                const uint8_t* p = &img.rgba[(size_t(y) * img.w + tx[x].first + k) * 4];
                const float a = p[3] * tx[x].weight[k];
                for (int c = 0; c < 3; ++c) acc[c] += p[c] * a;
                acc[3] += a;
            }
            std::memcpy(&rows[(size_t(y) * w + x) * 4], acc, sizeof acc);
        }
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float acc[4]{};
            for (size_t k = 0; k < ty[y].weight.size(); ++k) {
                const float* p = &rows[(size_t(ty[y].first + k) * w + x) * 4];
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * ty[y].weight[k];
            }
            uint8_t* o = &out[(size_t(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c) o[c] = uint8_t(std::clamp(acc[3] > 0 ? acc[c] / acc[3] + 0.5f : 0.0f, 0.0f, 255.0f));
            o[3] = uint8_t(std::clamp(acc[3] + 0.5f, 0.0f, 255.0f));
        }
    img = {w, h, std::move(out)};
}

/// The camp menu's backdrop: a light wash, a mid-tone diagonal across the lower left, a dark corner
/// at the lower right, a dark title band and the yellow strip down the left edge (anti-aliased).
inline void Background(bool female, Image& out) {
    struct Rgb { float r, g, b; };
    const Rgb wash = female ? Rgb{255, 184, 206} : Rgb{186, 214, 255};
    const Rgb diag = female ? Rgb{255, 122, 165} : Rgb{98, 150, 235};
    const Rgb dark = female ? Rgb{106, 10, 30} : Rgb{14, 36, 96};
    const Rgb strip{255, 196, 60};
    constexpr uint32_t W = 1240, H = 1080, Band = 86, Strip = 24;
    out = {W, H, std::vector<uint8_t>(size_t(W) * H * 4)};
    auto mix = [](Rgb a, Rgb b, float t) { t = std::clamp(t, 0.0f, 1.0f); return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; };
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            Rgb c = mix(wash, diag, (fy - fx - 300) / 1.4142f + 0.5f);   // below y = x + 300
            c = mix(c, dark, (fy + fx - 1940) / 1.4142f + 0.5f);        // below y = 1940 - x
            if (y < Band) c = dark;
            else if (y < Band + 4 || x < Strip) c = strip;
            uint8_t* o = &out.rgba[(size_t(y) * W + x) * 4];
            o[0] = uint8_t(c.r + 0.5f); o[1] = uint8_t(c.g + 0.5f); o[2] = uint8_t(c.b + 0.5f); o[3] = 255;
        }
}

inline bool Number(std::string_view s, int base, uint32_t limit, uint32_t& out) {
    if (s.empty() || s.size() > 4) return false;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out, base);
    return ec == std::errc{} && end == s.data() + s.size() && out <= limit;
}

/// Opened archives and the camp sheet, shared by load_image calls (any thread).
class Library {
public:
    bool Load(const EdenDsmodHostApi& host, std::string_view key, Image& out) {
        if (!key.starts_with("module:p3p:")) return false;
        key.remove_prefix(11);
        uint32_t n{}, height{};
        if (const auto at = key.find('@'); at != key.npos) {
            if (!Number(key.substr(at + 1), 10, 2048, height) || !height) return false;
            key = key.substr(0, at);
        }
        if (!Decode(host, key, out)) return false;
        Resize(out, height);
        return true;
    }

private:
    bool Decode(const EdenDsmodHostApi& host, std::string_view key, Image& out) {
        uint32_t n{};
        if (key == "fill") {
            out = {8, 8, std::vector<uint8_t>(8 * 8 * 4, 0xff)};
            return true;
        }
        if (key == "bg/0" || key == "bg/1") {
            Background(key == "bg/1", out);
            return true;
        }
        std::scoped_lock guard{lock_};
        if (key.starts_with("spr/") && Number(key.substr(4), 10, 0xffff, n))
            return Camp(host) && SpriteImage(camp_, n, out);
        if (key.starts_with("persona/") && Number(key.substr(8), 10, 0xfff, n) && n) {
            char path[40];
            std::snprintf(path, sizeof path, "camp/persona/pst_%03X.tmx", n);
            return Shared(host, path, out);
        }
        if (key.starts_with("bust/") && Number(key.substr(5), 10, 99, n) && n) {
            char path[40];
            std::snprintf(path, sizeof path, "camp/bustup/c_cha%02ua.tmx", n);
            return Shared(host, path, out);
        }
        return false;
    }
    bool Open(const EdenDsmodHostApi& host, p5r_assets::Cpk& cpk, bool& tried, const char* path) {
        if (cpk.IsOpen()) return true;
        if (tried || !host.read_romfs) return false;
        tried = true;
        return cpk.Open(p5r_assets::HostReader(host), path);
    }
    bool Shared(const EdenDsmodHostApi& host, const char* path, Image& out) {
        p5r_assets::Cpk::Entry entry;
        std::vector<uint8_t> bytes;
        return Open(host, shared_, shared_tried_, "data/umd0.cpk") && shared_.Find(path, entry) &&
               entry.extract <= (size_t(16) << 20) && shared_.ReadEntry(entry, bytes) && DecodeTmx(bytes, 0, out);
    }
    // c_main_01.spr (29 MB) is kept once read; the 58 MB startup archive around it is not.
    bool Camp(const EdenDsmodHostApi& host) {
        if (!camp_.empty()) return true;
        if (camp_tried_) return false;
        p5r_assets::Cpk::Entry entry;
        std::vector<uint8_t> init;
        std::vector<p5r_assets::PakEntry> members, camp;
        if (!Open(host, english_, english_tried_, "data_EN/umd0.cpk") || !english_.Find("init_free.bin", entry) ||
            entry.extract > (size_t(128) << 20) || !english_.ReadEntry(entry, init) ||
            !p5r_assets::ParsePak(init, members)) {
            camp_tried_ = true;
            return false;
        }
        camp_tried_ = true;
        const auto bin = p5r_assets::PakFind(members, "init/camp.bin");
        if (bin.empty() || !p5r_assets::ParsePak(bin, camp)) return false;
        const auto sheet = p5r_assets::PakFind(camp, "c_main_01.spr");
        camp_.assign(sheet.begin(), sheet.end());
        return !camp_.empty();
    }
    std::mutex lock_;
    p5r_assets::Cpk english_, shared_;
    bool english_tried_{}, shared_tried_{}, camp_tried_{};
    std::vector<uint8_t> camp_;
};

} // namespace p3p_art
