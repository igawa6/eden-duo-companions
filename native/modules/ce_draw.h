// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes companion: drawing kit. A C++ port of the accepted mockups' kit
// (research tools/redesign2/kit.py, redesign 2026-10-09): a 1240x1080 canvas that is a native
// 413x360 screen drawn at 3x, native sprites nearest-scaled by integers, the CE TMP bitmap font
// (S1/S2/S3 = 1x/2x/3x, alpha binarised at 128, 1-px dark shadow) and the native component roles
// (nine-sliced windows, capsule bars, the Overdrive gauge). Every picture is decoded at runtime
// from the player's own romfs through ce_unity::Reader; nothing extracted ships.
//
// Threading: Art is shared by the module's asset worker, the font callback and the readers on the
// timing thread (all public calls lock). Pictures and the font decode under `mutex`; the text
// tables the readers use (Table, BgText) have their own lock, so a reader never waits for a
// bundle decode. Canvas is a plain value type used by one thread at a time.
#pragma once

#include "chained_echoes_unity.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ce_draw {

using Image = ce_unity::Image;
using Ptr = std::shared_ptr<const Image>;

inline constexpr int W = 1240, H = 1080, U = 3;

/// Appended to every model signature before it is hashed into a picture key, so a module whose
/// drawing changed never reuses a picture cached under the previous build's key. A fixed release
/// stamp (not __DATE__ / __TIME__) keeps the build reproducible: bump it with the package version.
inline constexpr std::string_view PictureStamp = " ce-1.0.0";

/// 0xRRGGBB colour roles of the kit (COL in kit.py).
namespace col {
inline constexpr std::uint32_t bg = 0x211c20, panel = 0x13182a, gold = 0xffad1b, ink = 0xfff8e3,
                               muted = 0xc4b795, shadow = 0x050609, navy_empty = 0x171c2c,
                               low = 0xeb4905, hp_hi = 0x1ea65a, hp_mid = 0x0d614b,
                               hp_lo = 0x163f4a, tp_hi = 0x1ea7a4, tp_mid = 0x0d4b61,
                               tp_lo = 0x16234a, en_hi = 0xeb4905, en_mid = 0xaf1919,
                               en_lo = 0x51141f, bar_edge = 0xfbaa1b, gauge_edge = 0x111122,
                               ne_hi = 0xec9833, ne_mid = 0xba7622, ne_lo = 0x984311,
                               zone_od = 0x3fcf72, zone_oh = 0xff5a2a, zone_ne = 0xec9833,
                               discount = 0x3fcf72;
}

/// kit.snap: nearest multiple of 3 (Python round(v / 3) * 3; v / 3 is never a tie for ints).
constexpr int Snap(int v) {
    const int q = v >= 0 ? (v + 1) / 3 : -((-v + 1) / 3);
    return q * U;
}

struct Glyph {
    float bearing_x{}, bearing_y{}, advance{};
    Image tile; ///< binarised coverage (alpha 0/255), may be empty (space)
};

/// Decoded game art and the CE font, cached. Bundle aliases are kit.py's substrings:
/// "systemgfx", "packedassets", "gfx2", "bestiary", "startmenu".
class Art {
public:
    explicit Art(ce_unity::RangeReader read);
    Art(const Art&) = delete;
    Art& operator=(const Art&) = delete;

    /// A sprite (UnityPy pixels); crop = trim fully transparent borders (kit asset(crop=True)).
    Ptr Sprite(std::string_view alias, std::string_view name, bool crop = false);
    /// A whole Texture2D.
    Ptr Texture(std::string_view alias, std::string_view name);
    /// ctb_<id> / bestiary_<id> / portrait_<name> across their bundles.
    Ptr Family(ce_unity::Family family, std::string_view id, bool crop = false);
    /// The CE font's glyph for a code point ('?' for a missing one); null when the font failed.
    const Glyph* GlyphFor(std::uint32_t codepoint);
    bool FontReady();
    /// Host font hand-off (decode_font / font atlas image).
    bool HostFont(std::uint32_t& first, std::vector<EdenDsmodFontGlyph>& glyphs,
                  std::uint32_t& line_height);
    std::optional<Image> HostFontAtlas();
    /// A TextAsset table of Data/resources.assets by path id (111..131), split into rows of ';'
    /// separated cells (header row kept); cached.
    const std::vector<std::vector<std::string>>* Table(std::int64_t path_id);
    /// The English text of a BGDatabase string column (Data/StreamingAssets/
    /// bansheegz_database.bytes: tr_QUESTS_NAME, tr_PASSIVES_NAME, ...) in row order; null when
    /// missing. The 16 MiB file is read once and only the columns the readers use are kept.
    const std::vector<std::string>* BgText(std::string_view meta);
    /// A whole romfs file; nullopt when missing / over 16 MiB.
    std::optional<std::vector<std::uint8_t>> RomfsFile(std::string_view path);
    /// Drops a bundle's parsed tables and blocks from the reader (decoded pictures stay cached):
    /// the field page after drawing from mapstextures (~25 MiB resident while open).
    void EvictBundle(std::string_view alias);
    /// Decoded picture cache: bytes held / entries (for the dev tools).
    std::size_t CachedBytes();
    std::size_t CachedCount();

    /// Decoded picture cache budget: least recently used pictures beyond it are dropped (a
    /// picture still drawn stays alive through its shared pointer).
    static constexpr std::size_t CacheBudget = 32u << 20;
    static constexpr std::size_t CacheEntries = 4096;

private:
    bool LoadFontLocked();
    ce_unity::RangeReader read;
    ce_unity::Reader reader;
    std::mutex mutex;       ///< sprites, textures, font (held across a bundle decode)
    std::mutex text_mutex;  ///< tables and BGDatabase text (never held across a bundle decode)
    bool bg_tried{};
    std::map<std::string, std::vector<std::string>, std::less<>> bg_text;
    struct Cached {
        Ptr img; ///< null = the decode failed (not retried)
        std::list<std::string>::iterator at;
    };
    /// mutex held: the cached picture (touched) or nullopt when not cached.
    std::optional<Ptr> CacheFind(const std::string& key);
    void CachePut(std::string key, Ptr img);
    std::unordered_map<std::string, Cached> sprites;
    std::list<std::string> sprite_lru; ///< most recently used first
    std::size_t sprite_bytes{};
    std::optional<ce_unity::TmpFont> font;
    bool font_failed{};
    std::map<std::uint32_t, Glyph> glyphs;
    std::map<std::int64_t, std::vector<std::vector<std::string>>> tables;
};

// ---- image helpers -------------------------------------------------------------------------
Image Blank(int w, int h, std::uint32_t argb = 0);
Image Scale(const Image& im, int s);
Image Crop(const Image& im, int x, int y, int w, int h);
Image AlphaCropped(const Image& im);
/// PIL NEAREST resize of a source sub-rectangle.
Image ResizeNearest(const Image& im, int sx, int sy, int sw, int sh, int dw, int dh);
Image Greyed(const Image& im, float amount);
Image Faded(const Image& im, float alpha);
Image FlipX(const Image& im);

/// One 1240x1080 (or any size) RGBA canvas with kit.py's drawing operations.
class Canvas {
public:
    Canvas(Art& art, int w = W, int h = H, std::uint32_t argb = 0xFF000000u | col::bg);

    Image& Pixels() {
        return im;
    }
    Art& art;

    /// PIL Image.alpha_composite of `src` at (x, y).
    void Composite(const Image& src, int x, int y);
    /// kit.rect / _rect: opaque fill of [x0, x1) x [y0, y1) (no blending).
    void Fill(int x0, int y0, int x1, int y1, std::uint32_t rgb);
    /// kit.rect: snapped (x, y, w, h).
    void Rect(int x, int y, int w, int h, std::uint32_t rgb);
    /// Translucent black-ish overlay (kit states darken()).
    void Darken(float a, int x = 0, int y = 0, int w = -1, int h = -1);

    struct Box {
        int x{}, y{}, w{}, h{};
    };
    /// kit.put: anchor "tl","tc","tr","cl","c","cr","bl","bc","br".
    Box Put(const Image* img, int x, int y, int s = 3, std::string_view anchor = "tl",
            float alpha = 1.0f, float grey = 0.0f, bool flip = false);
    Box Put(const Ptr& img, int x, int y, int s = 3, std::string_view anchor = "tl",
            float alpha = 1.0f, float grey = 0.0f, bool flip = false) {
        return Put(img.get(), x, y, s, anchor, alpha, grey, flip);
    }
    /// kit.backdrop: mainMenuBG tiled at 3x.
    void Backdrop();

    // text (y = cap top); returns the x after the last glyph
    int Text(std::string_view s, int x, int y, int size = 1, std::uint32_t rgb = col::ink,
             bool shadow = true, char align = 'l', int maxw = -1, float alpha = 1.0f);
    int TextWidth(std::string_view s, int size = 1);
    std::string Fit(std::string_view s, int maxw, int size);
    std::vector<std::string> Wrap(std::string_view s, int maxw, int size = 1);
    int Paragraph(std::string_view s, int x, int y, int w, int size, std::uint32_t rgb,
                  int max_lines, int leading);
    int Digits(long long value, int n, int x, int y, int size = 2, std::uint32_t rgb = col::ink,
               float zero_alpha = 0.45f, char align = 'l');

    // frames
    Box Nine(const Ptr& a, Box box, int l, int t, int r, int b, int s = 3, int edge = 4,
             float alpha = 1.0f, int hs = -1, int vs = -1);
    Box Nine(std::string_view alias, std::string_view name, Box box, int k, int edge = 4,
             int vs = -1, bool crop = true, float alpha = 1.0f);
    void PartyWindow(Box b) {
        Nine("systemgfx", "dBox_Battle", b, 12, 2, 15);
    }
    void MenuWindow(Box b) {
        Nine("systemgfx", "commandbox_bg", b, 12, 2, 15);
    }
    void EnemyCard(Box b) {
        Nine("systemgfx", "enemy_descrip", b, 12, 2, 15);
    }
    void Button(Box b) {
        Nine("systemgfx", "dBox_choice", b, 4);
    }
    void SelectedFill(Box b, float alpha = 1.0f);
    void InfoField(Box b, char tone = 'g', float alpha = 1.0f);
    Box Cursor(int x, int y, int frame = 0, std::string_view anchor = "cr");
    Box NameRibbon(int cx, int y, std::string_view label, std::uint32_t rgb = col::gold);

    // gauges and tags
    int Bar(int x, int y, int w, double frac, char kind = 'h', int h = 5);
    struct Zone {
        double a{}, b{};
        char kind{'o'}; ///< 'o' Overdrive (green) / 'h' Overheat (red)
    };
    /// projection: (gauge fraction, 'g' grey / 'o' green / 'h' red), kit od_gauge(projection=)
    int OdGauge(int x, int y, int w, double pos, const std::vector<Zone>& zones, int h = 8,
                bool tick = true, bool marker = true,
                std::optional<std::pair<double, char>> projection = std::nullopt);
    /// kit partner_field(): partyInfoFieldBGG (BGR when KO) stretched to the box
    void PartnerField(Box b, bool ko = false);
    int StatusTag(int pic, int x, int y, std::optional<int> turns);

private:
    Image im;
};

} // namespace ce_draw
