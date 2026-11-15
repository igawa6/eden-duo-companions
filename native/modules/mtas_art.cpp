// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mtas_art.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace mtas {

namespace {

std::string BundlePath(const char* bundle) {
    return std::string{"Data/StreamingAssets/AssetBundles/"} + bundle;
}

/// Sorts `table` BY NAME, IN PLACE. Called exactly once (guarded local-static below) -- tried a
/// `constexpr` compile-time sort first (the smaller UiPins() table manages it), but
/// SpriteIndex()'s 3,102 entries blow past the compiler's default constexpr-evaluation step
/// budget doing string comparisons at compile time (`-fconstexpr-ops-limit`). In-place (not
/// returning a sorted copy) matters here: `table` itself is still a compile-time constant in its
/// UNSORTED order (a plain, non-`constexpr` `static` array with a literal initializer still gets
/// static, not dynamic, initialization -- no runtime construction code either way), so this is
/// one array's worth of storage, not two -- a sorted COPY would otherwise double the sprite
/// table's footprint in the shipped .so for no benefit (measured live during this fix: a sorted-
/// copy version of this function made the stripped .so ~12% LARGER on disk than before this
/// whole change, exactly backwards from the point of it; see NamedSpritePin's own comment for the
/// other half of that regression, a per-entry pointer needing its own relocation).
template <std::size_t N>
void SortPinsByNameInPlace(std::array<NamedSpritePin, N>& table) {
    std::sort(table.begin(), table.end(), [](const NamedSpritePin& a, const NamedSpritePin& b) {
        return std::string_view{a.name} < std::string_view{b.name};
    });
}

} // namespace

const SpritePin* SpritePinTable::find(std::string_view name) const {
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), name,
        [](const NamedSpritePin& e, std::string_view n) { return std::string_view{e.name} < n; });
    return (it != entries_.end() && std::string_view{it->name} == name) ? &it->pin : nullptr;
}

const SpritePin& SpritePinTable::at(std::string_view name) const {
    const SpritePin* p = find(name);
    if (!p)
        throw std::out_of_range("SpritePinTable::at: unknown pin name");
    return *p;
}

const SpritePinTable& UiPins() {
    static auto table = std::to_array<NamedSpritePin>(
#include "mtas_pins_ui.inc"
    );
    static const bool sorted = [] { SortPinsByNameInPlace(table); return true; }();
    (void)sorted;
    static const SpritePinTable view{table};
    return view;
}

const SpritePinTable& MenuPins() {
    static auto table = std::to_array<NamedSpritePin>(
#include "mtas_menu_pins.inc"
    );
    static const bool sorted = [] { SortPinsByNameInPlace(table); return true; }();
    (void)sorted;
    static const SpritePinTable view{table};
    return view;
}

const SpritePinTable& MapPins() {
    static auto table = std::to_array<NamedSpritePin>(
#include "mtas_map_pins.inc"
    );
    static const bool sorted = [] { SortPinsByNameInPlace(table); return true; }();
    (void)sorted;
    static const SpritePinTable view{table};
    return view;
}

const SpritePinTable& SpriteIndex() {
    static auto table = std::to_array<NamedSpritePin>(
#include "mtas_sprite_index.inc"
    );
    static const bool sorted = [] { SortPinsByNameInPlace(table); return true; }();
    (void)sorted;
    static const SpritePinTable view{table};
    return view;
}

std::optional<LinearAstc> ReadLinearAstc(ce_unity::Bundle& bundle, const SpritePin& pin) {
    const std::uint32_t block = pin.block;
    if (block == 0 || pin.gobs_per_block == 0)
        return std::nullopt;
    const auto* node = bundle.FindNode(pin.data_node);
    if (!node)
        return std::nullopt;

    // Tegra block-linear layout: GOBs are 4 blocks wide; a GOB column is `gobs_per_block` blocks
    // tall. Pad the atlas's own block grid up to whole GOBs on each axis (same formula as
    // Cult of the Lamb's DecodeSwitchSprite, cotl_icons.cpp, generalised to `block`).
    const std::uint32_t row_blocks =
        ((pin.atlas_w + block * 4 - 1) / (block * 4)) * 4;
    const std::uint32_t col_blocks =
        ((pin.atlas_h + block * 8 * pin.gobs_per_block - 1) / (block * 8 * pin.gobs_per_block)) *
        8 * pin.gobs_per_block;
    const std::uint64_t bytes = std::uint64_t{row_blocks} * col_blocks * 16;
    if (bytes > ce_unity::MaxAlloc)
        return std::nullopt;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(bytes));
    if (!bundle.Read(node->offset + pin.node_offset, raw.data(), raw.size()))
        return std::nullopt;

    LinearAstc out;
    out.blocks = ce_unity::SwitchDeswizzle(raw, row_blocks * block, col_blocks * block,
                                           block, block, pin.gobs_per_block);
    if (out.blocks.size() != bytes)
        return std::nullopt;
    out.row_blocks = row_blocks;
    out.block = block;
    out.atlas_w = pin.atlas_w;
    out.atlas_h = pin.atlas_h;
    return out;
}

std::optional<ce_unity::Image> DecodeSwizzledAstcSprite(ce_unity::Bundle& bundle,
                                                        const SpritePin& pin,
                                                        const ce_unity::AstcDecoder& astc) {
    if (!astc)
        return std::nullopt;
    const auto linear = ReadLinearAstc(bundle, pin);
    if (!linear)
        return std::nullopt;
    return CropLinearAstc(*linear, pin.rect_x, pin.rect_y, pin.rect_w, pin.rect_h, astc);
}

std::optional<ce_unity::Image> CropLinearAstc(const LinearAstc& linear_astc, float rect_x,
                                              float rect_y, float rect_w, float rect_h,
                                              const ce_unity::AstcDecoder& astc) {
    if (!astc || linear_astc.block == 0)
        return std::nullopt;
    const std::uint32_t block = linear_astc.block;
    const std::uint32_t row_blocks = linear_astc.row_blocks;
    const auto& linear = linear_astc.blocks;
    // Decode only the blocks the sprite's own rect overlaps -- a full 2048x2048 atlas decoded
    // whole would exceed the host's 16 MiB-per-call image cap once padded (cotl_icons.cpp's own
    // comment on why it tiles rather than decoding whole).
    const auto ix = static_cast<std::int64_t>(std::lround(rect_x));
    const auto iy = static_cast<std::int64_t>(std::lround(rect_y));
    const auto iw = static_cast<std::int64_t>(std::lround(rect_w));
    const auto ih = static_cast<std::int64_t>(std::lround(rect_h));
    if (ix < 0 || iy < 0 || iw <= 0 || ih <= 0 || ix + iw > linear_astc.atlas_w ||
        iy + ih > linear_astc.atlas_h || iw * ih * 4 > std::int64_t{16} * 1024 * 1024)
        return std::nullopt;
    const auto bx0 = static_cast<std::uint32_t>(ix / block);
    const auto by0 = static_cast<std::uint32_t>(iy / block);
    const auto bx1 = static_cast<std::uint32_t>((ix + iw + block - 1) / block);
    const auto by1 = static_cast<std::uint32_t>((iy + ih + block - 1) / block);
    const std::uint32_t nx = bx1 - bx0, ny = by1 - by0;
    std::vector<std::uint8_t> blocks(std::size_t{nx} * ny * 16);
    for (std::uint32_t r = 0; r < ny; ++r)
        std::memcpy(blocks.data() + std::size_t{r} * nx * 16,
                    linear.data() + (std::size_t{by0 + r} * row_blocks + bx0) * 16,
                    std::size_t{nx} * 16);
    std::vector<std::uint8_t> rgba;
    if (!astc(blocks.data(), blocks.size(), nx * block, ny * block, block, block, rgba) ||
        rgba.size() < std::size_t{nx} * ny * block * block * 4)
        return std::nullopt;

    // rgba's row 0 is the bottom Unity row of this block range (the GOB/block copy loop above
    // walks atlas block-rows by0, by0+1, ... in INCREASING atlas-Y order into increasing
    // blocks[]/rgba row order, and atlas Y increases upward in Unity's bottom-left convention --
    // same reasoning DecodeTextureData's own FlipVertical call documents for the generic
    // whole-texture path). `out` is top-left-origin (row 0 = the sprite's visual TOP = the
    // HIGHEST atlas Y within the sprite's own rect, tile-row `bottom+ih-1`), so row 0 must read
    // rgba's row `bottom+ih-1`, and row `ih-1` (visual bottom, tile-row `bottom`) reads rgba's row
    // `bottom`.
    //
    // Round-2 fix: the previous formula (`tile_h - 1 - (bottom + ih - 1 - row)`) read the wrong
    // direction -- it read row 0 from rgba's LOW rows (near atlas-bottom) instead of its HIGH rows
    // (near atlas-top), producing a vertically flipped sprite. The ~44 Stage-8/9 UI pins this
    // round found live (Paper, Coin, every mission/tab icon) happened not to show it: a coin
    // disc+star, a parchment panel and most of the UI chrome read the same flipped or not. Found
    // this round only because the dynamic sprite index (SpriteIndex(), item icons / NPC portraits)
    // includes sprites with an obvious up/down (a face, a mission item) -- Heidi/Mason/Fennec Fox
    // and the Bronze Stick/Bronze Bars item icons all round-trip to the WRONG orientation with the
    // old formula and the RIGHT one with this fix, cross-checked byte-for-byte against UnityPy's
    // own independent decode of the same Sprite objects (small residual per-pixel diffs only --
    // ASTC-decoder rounding noise between two different software ASTC implementations, same
    // magnitude whether the sprite is symmetric like Coin or not -- never the ~150-250 avg-diff a
    // genuine flip produces). See DATA-CONTRACT.md "Icons for UI" / "sprite orientation fix" for
    // the measurements. mtas_publish_test.cpp's TestNineSlice/TestUiPins do not exercise this
    // function at all (they use synthetic Solid() images, bypassing ASTC decode entirely) --
    // dsmod-mtas-sprite-resolve-test (round-2, real romfs + real CPU ASTC decoder) is the first
    // test that actually calls this code path end-to-end.
    ce_unity::Image out;
    out.width = static_cast<std::uint32_t>(iw);
    out.height = static_cast<std::uint32_t>(ih);
    out.rgba.resize(static_cast<std::size_t>(iw * ih * 4));
    const std::int64_t left = ix - std::int64_t{bx0} * block;
    const std::int64_t bottom = iy - std::int64_t{by0} * block;
    for (std::int64_t row = 0; row < ih; ++row) {
        const std::int64_t src_row = bottom + ih - 1 - row;
        std::memcpy(out.rgba.data() + row * iw * 4,
                    rgba.data() + (src_row * nx * block + left) * 4,
                    static_cast<std::size_t>(iw * 4));
    }
    return out;
}

ce_unity::Image BoxDownscale(const ce_unity::Image& src, std::uint32_t side) {
    if (side == 0 || src.width != src.height || src.width % side || side == src.width)
        return src;
    const std::uint32_t f = src.width / side;
    ce_unity::Image out;
    out.width = out.height = side;
    out.rgba.resize(std::size_t{side} * side * 4);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x)
            for (std::uint32_t c = 0; c < 4; ++c) {
                std::uint32_t sum = 0;
                for (std::uint32_t dy = 0; dy < f; ++dy)
                    for (std::uint32_t dx = 0; dx < f; ++dx)
                        sum += src.rgba[((std::size_t{y} * f + dy) * src.width + x * f + dx) * 4 + c];
                out.rgba[(std::size_t{y} * side + x) * 4 + c] =
                    static_cast<std::uint8_t>((sum + f * f / 2) / (f * f));
            }
    return out;
}

namespace {

/// Nearest-neighbour sample of one axis-aligned rectangle of `src` into `out_w` x `out_h`.
void ScaleRect(const ce_unity::Image& src, int sx0, int sy0, int sw, int sh,
              ce_unity::Image& out, int dx0, int dy0, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    for (int y = 0; y < dh; ++y) {
        const int sy = sy0 + std::min(sh - 1, (y * sh) / dh);
        for (int x = 0; x < dw; ++x) {
            const int sx = sx0 + std::min(sw - 1, (x * sw) / dw);
            const std::uint8_t* s = &src.rgba[(static_cast<std::size_t>(sy) * src.width + sx) * 4];
            std::uint8_t* d =
                &out.rgba[(static_cast<std::size_t>(dy0 + y) * out.width + dx0 + x) * 4];
            std::memcpy(d, s, 4);
        }
    }
}

} // namespace

std::optional<ce_unity::Image> NineSlice(const ce_unity::Image& src, const SpritePin& pin,
                                         std::uint32_t out_w, std::uint32_t out_h) {
    if (src.width == 0 || src.height == 0 || src.rgba.size() < std::size_t{src.width} * src.height * 4)
        return std::nullopt;
    const int l = static_cast<int>(std::lround(pin.border_l));
    int b = static_cast<int>(std::lround(pin.border_b));
    int r = static_cast<int>(std::lround(pin.border_r));
    const int t = static_cast<int>(std::lround(pin.border_t));
    // Native half-pixel capsule borders can round away a one-pixel centre strip.
    if (l + r >= static_cast<int>(src.width) && pin.border_l + pin.border_r < src.width)
        r = static_cast<int>(src.width) - l - 1;
    if (b + t >= static_cast<int>(src.height) && pin.border_b + pin.border_t < src.height)
        b = static_cast<int>(src.height) - t - 1;
    if (l < 0 || b < 0 || r < 0 || t < 0 || l + r >= static_cast<int>(src.width) ||
        b + t >= static_cast<int>(src.height))
        return std::nullopt;
    if (out_w == 0 || out_h == 0 || static_cast<int>(out_w) <= l + r ||
        static_cast<int>(out_h) <= b + t)
        return std::nullopt;
    ce_unity::Image out;
    out.width = out_w;
    out.height = out_h;
    out.rgba.assign(static_cast<std::size_t>(out_w) * out_h * 4, 0);
    if (l == 0 && r == 0 && b == 0 && t == 0) {
        ScaleRect(src, 0, 0, static_cast<int>(src.width), static_cast<int>(src.height), out, 0, 0,
                 static_cast<int>(out_w), static_cast<int>(out_h));
        return out;
    }
    const int sw = static_cast<int>(src.width), sh = static_cast<int>(src.height);
    const int cw = sw - l - r, ch = sh - t - b; // centre piece (top-left-origin image: top border = t)
    const int dcw = static_cast<int>(out_w) - l - r, dch = static_cast<int>(out_h) - t - b;
    // 9 pieces, top-left origin (t = top border, b = bottom border).
    ScaleRect(src, 0, 0, l, t, out, 0, 0, l, t);                       // top-left
    ScaleRect(src, l, 0, cw, t, out, l, 0, dcw, t);                    // top
    ScaleRect(src, sw - r, 0, r, t, out, out_w - r, 0, r, t);          // top-right
    ScaleRect(src, 0, t, l, ch, out, 0, t, l, dch);                    // left
    ScaleRect(src, l, t, cw, ch, out, l, t, dcw, dch);                 // centre
    ScaleRect(src, sw - r, t, r, ch, out, out_w - r, t, r, dch);       // right
    ScaleRect(src, 0, sh - b, l, b, out, 0, out_h - b, l, b);          // bottom-left
    ScaleRect(src, l, sh - b, cw, b, out, l, out_h - b, dcw, b);       // bottom
    ScaleRect(src, sw - r, sh - b, r, b, out, out_w - r, out_h - b, r, b); // bottom-right
    return out;
}

const ce_unity::Image* SpriteLru::Get(const std::string& name) {
    const auto it = map_.find(name);
    if (it == map_.end())
        return nullptr;
    order_.splice(order_.begin(), order_, it->second.second); // move to front (most recent)
    return &it->second.first;
}

void SpriteLru::Put(std::string name, ce_unity::Image image) {
    const std::size_t new_bytes = image.rgba.size();
    if (const auto it = map_.find(name); it != map_.end()) {
        bytes_ -= it->second.first.rgba.size();
        order_.erase(it->second.second);
        map_.erase(it);
    }
    order_.push_front(name);
    bytes_ += new_bytes;
    map_.emplace(std::move(name), std::make_pair(std::move(image), order_.begin()));
    EvictLocked();
}

void SpriteLru::Clear() {
    order_.clear();
    map_.clear();
    bytes_ = 0;
}

void SpriteLru::EvictLocked() {
    while (!order_.empty() && (order_.size() > max_entries_ || bytes_ > max_bytes_)) {
        const std::string& victim = order_.back();
        const auto it = map_.find(victim);
        if (it != map_.end()) {
            bytes_ -= it->second.first.rgba.size();
            map_.erase(it);
        }
        order_.pop_back();
    }
}

std::vector<std::uint32_t> DefaultCodepoints() {
    std::vector<std::uint32_t> cps;
    for (std::uint32_t c = 0x20; c <= 0x7E; ++c)
        cps.push_back(c);
    for (std::uint32_t c = 0xA0; c <= 0xFF; ++c)
        cps.push_back(c);
    return cps;
}

std::optional<FontAtlas> BuildLatinFontAtlas(const std::vector<std::uint8_t>& otf_bytes,
                                             std::uint32_t cap_height_px, bool paged,
                                             const std::vector<std::uint32_t>& priority) {
    try {
        constexpr std::uint32_t MaxAtlas = 2048;
        constexpr std::uint32_t Pad = 1;
        if (otf_bytes.size() < 12 || cap_height_px < 4 || cap_height_px > 256)
            return std::nullopt;
        stbtt_fontinfo info{};
        const int offset = stbtt_GetFontOffsetForIndex(otf_bytes.data(), 0);
        if (offset < 0 || stbtt_InitFont(&info, otf_bytes.data(), offset) == 0)
            return std::nullopt;
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
            int a, d, g;
            stbtt_GetFontVMetrics(&info, &a, &d, &g);
            cap_units = static_cast<float>(a) * 0.7f;
        }
        if (cap_units <= 0.0f)
            return std::nullopt;
        const float scale = static_cast<float>(cap_height_px) / cap_units;
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);

        auto cps = DefaultCodepoints();
        if (paged) {
            // Build exactly what's needed -- `priority` (the caller's own SetFontText() text) is
            // already a superset of every character the companion can ever display: it comes
            // straight from GameText().FontText(), which walks the SAME `loc_` row table every
            // item name/NPC name/mission/UI string resolves against (Text::ItemName/NpcName/Get
            // all look up a row in `loc_`; FontText() concatenates every row in it). Laying out
            // the WHOLE 0x20-0xFFFF BMP range regardless of `priority` (the old behaviour) built
            // and shelf-packed tens of thousands of codepoints the font has a glyph for but this
            // companion will never ask to render -- confirmed live (review-L3 finding 3):
            // FontGlyphs() called with an empty `priority` printed "65504 metrics, 482 pages" at
            // the coarsest (36px) cap height. `DefaultCodepoints()` (ASCII + Latin-1) is still
            // unioned in as a small floor, both for the state before the very first SetFontText()
            // and as cheap insurance for any formatted punctuation a text scan might miss. A
            // later SetFontText() with different/larger text naturally "extends" this: it resets
            // font_build_attempted_/font_ (see Art::SetFontText), forcing a fresh
            // BuildLatinFontAtlas call with the new priority as its own `cps`.
            std::vector<std::uint32_t> wanted = cps;
            for (const auto cp : priority)
                if (cp <= 0xFFFF)
                    wanted.push_back(cp);
            std::sort(wanted.begin(), wanted.end());
            wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
            cps = std::move(wanted);
            for (const auto cp : priority)
                if (cp > 0xFFFF && cp <= 0x10FFFF && stbtt_FindGlyphIndex(&info, cp))
                    cps.push_back(cp);
        }
        const auto first = cps.front(), last = cps.back();
        std::stable_partition(cps.begin(), cps.end(), [&](auto cp) {
            return std::binary_search(priority.begin(), priority.end(), cp);
        });
        struct Raster {
            std::uint32_t cp{};
            int w{}, h{}, x0{}, y0{}, advance{};
            std::vector<std::uint8_t> coverage;
            std::uint32_t ax{}, ay{};
        };
        std::vector<Raster> rasters;
        FontAtlas out;
        for (const std::uint32_t cp : cps) {
            const int gi = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
            if (gi == 0)
                continue; // left as the shared blank glyph below
            Raster r;
            r.cp = cp;
            int adv, lsb, x1, y1;
            stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
            r.advance = static_cast<int>(std::lround(static_cast<float>(adv) * scale));
            stbtt_GetGlyphBitmapBox(&info, gi, scale, scale, &r.x0, &r.y0, &x1, &y1);
            r.w = std::max(0, x1 - r.x0);
            r.h = std::max(0, y1 - r.y0);
            if (r.w > 0 && r.h > 0 && r.w <= 512 && r.h <= 512) {
                if (!paged) {
                    r.coverage.assign(static_cast<std::size_t>(r.w) * r.h, 0);
                    stbtt_MakeGlyphBitmap(&info, r.coverage.data(), r.w, r.h, r.w, scale, scale, gi);
                }
            } else {
                r.w = r.h = 0;
            }
            rasters.push_back(std::move(r));
        }
        if (rasters.empty())
            return std::nullopt;

        // Shelf packing: fixed-width virtual pages, or the smallest square for Latin-only callers.
        std::uint32_t width = 0, height = 0;
        for (std::uint32_t w = paged ? MaxAtlas : 128; w <= MaxAtlas; w *= 2) {
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
                if (paged && y / FontPageHeight != (y + ch - 1) / FontPageHeight) {
                    x = 0;
                    y = (y / FontPageHeight + 1) * FontPageHeight;
                    shelf = 0;
                }
                r.ax = x + Pad;
                r.ay = y + Pad;
                x += cw;
                shelf = std::max(shelf, ch);
            }
            if (fits && y + shelf <= (paged ? 65535u : w)) {
                width = w;
                height = std::max<std::uint32_t>(y + shelf, 1);
                break;
            }
        }
        if (width == 0)
            return std::nullopt;
        out.atlas.width = width;
        out.atlas.height = height;
        if (!paged) {
            out.atlas.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
            for (std::size_t i = 0; i < out.atlas.rgba.size(); i += 4)
                out.atlas.rgba[i] = out.atlas.rgba[i + 1] = out.atlas.rgba[i + 2] = 0xFF;
            for (const auto& r : rasters)
                for (int y = 0; y < r.h; ++y)
                    for (int x = 0; x < r.w; ++x)
                        out.atlas.rgba[((std::size_t{r.ay} + y) * width + r.ax + x) * 4 + 3] =
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
                g.bearing_y = static_cast<std::int16_t>(-r.y0);
            }
            g.advance = static_cast<std::uint16_t>(std::max(0, r.advance));
            return g;
        };
        EdenDsmodFontGlyph blank{};
        blank.advance = static_cast<std::uint16_t>((cap_height_px + 1) / 2);
        for (const auto& r : rasters)
            if (r.cp == ' ')
                blank.advance = glyph_of(r).advance;
        out.first_codepoint = first;
        out.line_height = cap_height_px;
        out.glyphs.assign(last - first + 1, blank);
        for (const auto& r : rasters)
            if (r.w > 0 || r.cp != ' ')
                out.glyphs[r.cp - out.first_codepoint] = glyph_of(r);
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

Art::Art(ce_unity::RangeReader romfs, ce_unity::AstcDecoder astc)
    : romfs_(std::move(romfs)), astc_(std::move(astc)) {}

void Art::SetAstcDecoder(ce_unity::AstcDecoder astc) {
    std::lock_guard lock{mutex_};
    astc_ = std::move(astc);
}

std::shared_ptr<ce_unity::Bundle> Art::OpenBundleLocked(const std::string& bundle) {
    for (auto it = bundles_.begin(); it != bundles_.end(); ++it) {
        if (it->first == bundle) {
            bundles_.splice(bundles_.begin(), bundles_, it);
            return bundles_.front().second;
        }
    }
    if (!romfs_)
        return nullptr;
    const auto path = BundlePath(bundle.c_str());
    const auto size = romfs_.size(path);
    if (!size || *size == 0)
        return nullptr;
    ce_unity::ByteSource src;
    src.size = *size;
    src.read = [r = romfs_, path](std::uint64_t off, std::uint8_t* out, std::size_t n) {
        return r.read(path, off, out, n);
    };
    auto opened = ce_unity::Bundle::Open(std::move(src));
    if (opened) {
        bundles_.emplace_front(bundle, opened);
        TrimBundlesLocked();
    }
    return opened;
}

void Art::TrimBundlesLocked() {
    std::size_t bytes = 0;
    for (const auto& entry : bundles_)
        bytes += entry.second->ResidentBytes();
    while (!bundles_.empty() && (bytes > BundleCacheBudget || bundles_.size() > 32)) {
        bytes -= bundles_.back().second->ResidentBytes();
        bundles_.pop_back();
    }
}

std::pair<std::size_t, std::size_t> Art::BundleCacheStats() {
    std::lock_guard lock{mutex_};
    std::size_t bytes = 0;
    for (const auto& entry : bundles_)
        bytes += entry.second->ResidentBytes();
    return {bundles_.size(), bytes};
}

const SpritePin* Art::FindPin(std::string_view name) {
    if (const SpritePin* p = UiPins().find(name))
        return p;
    if (const SpritePin* p = MenuPins().find(name))
        return p;
    if (const SpritePin* p = MapPins().find(name))
        return p;
    return SpriteIndex().find(name);
}

std::optional<ce_unity::Image> Art::ResolveSpriteLocked(std::string_view name,
                                                        const SpritePin& pin) {
    const std::string key{name};
    if (const ce_unity::Image* cached = sprite_cache_.Get(key))
        return *cached;
    auto bundle = OpenBundleLocked(pin.bundle);
    if (!bundle)
        return std::nullopt;
    auto decoded = DecodeSwizzledAstcSprite(*bundle, pin, astc_);
    TrimBundlesLocked();
    if (!decoded)
        return std::nullopt;
    sprite_cache_.Put(key, *decoded);
    return decoded;
}

std::optional<ce_unity::Image> Art::ResolveSprite(std::string_view name) {
    const SpritePin* pin = FindPin(name);
    if (!pin)
        return std::nullopt;
    std::lock_guard lock{mutex_};
    return ResolveSpriteLocked(name, *pin);
}

std::optional<ce_unity::Image> Art::LoadMapTile(std::string_view pin_name, std::uint32_t col,
                                               std::uint32_t row, std::uint32_t grid,
                                               std::uint32_t px) {
    const SpritePin* pin = FindPin(pin_name);
    if (!pin || grid == 0 || col >= grid || row >= grid || px == 0 || pin->atlas_w % grid ||
        pin->atlas_h % grid)
        return std::nullopt;
    const std::uint32_t tile_w = pin->atlas_w / grid, tile_h = pin->atlas_h / grid;
    if (px > tile_w || tile_w % px || tile_h != tile_w)
        return std::nullopt;
    std::lock_guard lock{mutex_};
    if (map_pin_ != pin_name || !map_linear_) {
        map_linear_.reset();
        auto bundle = OpenBundleLocked(pin->bundle);
        if (!bundle)
            return std::nullopt;
        map_linear_ = ReadLinearAstc(*bundle, *pin);
        // The deswizzled block array is kept instead; the bundle's own copy need not stay.
        bundles_.remove_if([&](const auto& entry) { return entry.first == pin->bundle; });
        if (!map_linear_)
            return std::nullopt;
        map_pin_ = std::string{pin_name};
    }
    // Tile row 0 is the map's visual top = the highest Unity (bottom-left origin) rows.
    const auto tile = CropLinearAstc(*map_linear_, static_cast<float>(col * tile_w),
                                     static_cast<float>((grid - 1 - row) * tile_h),
                                     static_cast<float>(tile_w), static_cast<float>(tile_h), astc_);
    if (!tile)
        return std::nullopt;
    return BoxDownscale(*tile, px);
}

std::optional<ce_unity::Image> Art::LoadMapWhole(std::string_view pin_name, std::uint32_t grid,
                                                std::uint32_t px) {
    if (grid == 0 || px == 0 || std::uint64_t{grid} * px * grid * px * 4 > 16u * 1024 * 1024)
        return std::nullopt;
    const std::uint32_t side = grid * px;
    ce_unity::Image out{side, side, std::vector<std::uint8_t>(std::size_t{side} * side * 4)};
    for (std::uint32_t row = 0; row < grid; ++row)
        for (std::uint32_t col = 0; col < grid; ++col) {
            const auto tile = LoadMapTile(pin_name, col, row, grid, px);
            if (!tile || tile->width != px || tile->height != px)
                return std::nullopt;
            for (std::uint32_t y = 0; y < px; ++y)
                std::copy_n(tile->rgba.data() + std::size_t{y} * px * 4, std::size_t{px} * 4,
                            out.rgba.data() + ((std::size_t{row} * px + y) * side + col * px) * 4);
        }
    return out;
}

std::optional<ce_unity::Image> Art::LoadSprite(std::string_view name, std::uint32_t w,
                                               std::uint32_t h) {
    const bool paper = name == "PagePaper";
    const SpritePin* pin = FindPin(paper ? "PageMain" : name);
    if (!pin)
        return std::nullopt;
    std::lock_guard lock{mutex_};
    auto decoded = ResolveSpriteLocked(paper ? "PageMain" : name, *pin);
    if (!decoded)
        return std::nullopt;
    if (paper) {
        // The native page interior, without its painted frame or corner studs.
        ce_unity::Image out{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
        ScaleRect(*decoded, 66, 66, int(decoded->width) - 132, int(decoded->height) - 132,
                  out, 0, 0, int(w), int(h));
        return out;
    }
    if (name == "FilterPillOn" || name == "FilterPillOff") {
        const double scale = std::min(double(w) / decoded->width, double(h) / decoded->height);
        if (scale > 1) {
            ce_unity::Image enlarged{static_cast<std::uint32_t>(decoded->width * scale),
                                    static_cast<std::uint32_t>(decoded->height * scale), {}};
            enlarged.rgba.resize(static_cast<std::size_t>(enlarged.width) * enlarged.height * 4);
            ScaleRect(*decoded, 0, 0, decoded->width, decoded->height, enlarged,
                      0, 0, enlarged.width, enlarged.height);
            auto borders = *pin;
            borders.border_l *= scale; borders.border_r *= scale;
            borders.border_b *= scale; borders.border_t *= scale;
            return NineSlice(enlarged, borders, w, h);
        }
    }
    return NineSlice(*decoded, *pin, w, h);
}

std::pair<std::size_t, std::size_t> Art::SpriteCacheStats() {
    std::lock_guard lock{mutex_};
    return {sprite_cache_.EntryCount(), sprite_cache_.ByteCount()};
}

std::vector<std::uint32_t> FontCodepoints(std::string_view text) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        std::uint32_t cp = lead;
        int remaining = 0;
        if (lead >= 0xF0) { cp &= 7; remaining = 3; }
        else if (lead >= 0xE0) { cp &= 15; remaining = 2; }
        else if (lead >= 0xC0) { cp &= 31; remaining = 1; }
        while (remaining-- && i < text.size())
            cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 63);
        if (cp >= 0x20)
            out.push_back(cp);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Art::SetFontText(const std::string& text) {
    std::lock_guard lock{mutex_};
    font_priority_ = FontCodepoints(text);
    font_priority_.shrink_to_fit();
    font_.reset();
    thai_first_page_ = 0;
    font_build_attempted_ = false;
}

bool Art::BuildFontLocked() {
    if (font_build_attempted_)
        return font_.has_value();
    font_build_attempted_ = true;
    auto bundle = OpenBundleLocked(kPinFont.bundle);
    if (!bundle)
        return false;
    const auto* node = bundle->FindNode(kPinFont.data_node);
    if (!node)
        return false;
    std::vector<std::uint8_t> otf(static_cast<std::size_t>(kPinFont.length));
    const bool read = bundle->Read(node->offset + kPinFont.node_offset, otf.data(), otf.size());
    TrimBundlesLocked();
    if (!read)
        return false;
    if (otf.size() < 4 || std::memcmp(otf.data(), "OTTO", 4) != 0)
        return false; // not the font bytes we expect at this pin: refuse rather than guess
    // Start at 36px cap height (the largest token, hero). The host's own text renderer
    // scales a baked "game font" glyph by wanted/line_height, wanted = widget text_scale * 5.0
    // (mod_ui_text.cpp DrawText/MeasureText) -- a float ratio, not limited to integer multiples
    // of the baked size -- so one atlas serves every token (manifest text_scale 7/5/4 ->
    // ~35/25/20px; larger alphabets may need a smaller bake to fit the page budget: see
    // tools/gen_dualscreen_manifest.py's own docstring for the exact mapping).
    // Keep the entire current bundle together: scattered BMP pages can exhaust the host's
    // protected 32 MiB cache even when only a few dozen characters are visible. Leave one
    // page for the shipped Thai fallback. All other supported BMP characters remain available.
    for (std::uint32_t cap = 36; cap >= 20; --cap) {
        font_ = BuildLatinFontAtlas(otf, cap, true, font_priority_);
        if (!font_)
            return false;
        std::uint32_t last_page = 0;
        for (const auto cp : font_priority_) {
            if (cp < font_->first_codepoint || cp - font_->first_codepoint >= font_->glyphs.size())
                continue;
            const auto& g = font_->glyphs[cp - font_->first_codepoint];
            if (g.w && g.h)
                last_page = std::max(last_page, g.y / FontPageHeight);
        }
        if (last_page < 31)
            break;
    }
    if (font_) {
        font_bytes_ = std::move(otf);
        // Use the shipped fallback for Thai and characters absent from Source Han (e.g. Latin combining marks).
        ce_unity::Reader unity(romfs_, {}, "Data/StreamingAssets/AssetBundles/");
        const auto sf = unity.OpenSerialized("referenced_font");
        if (sf) {
            for (const auto& obj : sf->Objects()) {
                if (obj.class_id != 128 || sf->PeekName(obj) != "Sarabun-Medium")
                    continue;
                const auto raw = sf->ReadRaw(obj);
                if (!raw)
                    continue;
                // Unity 2021 has no embedded Font typetree. Locate the length-prefixed TTF
                // payload within this named Font by scanning for a byte run that (a) carries a
                // real TrueType/OpenType sfnt signature right after its own 4-byte length prefix
                // and (b) actually PARSES as a font via stb_truetype's own table-directory
                // validation -- not an exact byte-length match (`length == 83080`), which would
                // silently stop finding this payload the moment a future game update re-exports
                // this embedded font by even one byte, with no diagnostic (review-L3 finding 5:
                // this was the single point of failure for the whole Thai-fallback step, and any
                // other non-Source-Han character, going silent). `off += 4` matches this blob's
                // own 4-byte field alignment (Unity's binary serializer), same stepping as before.
                for (std::size_t off = 0; off + 4 <= raw->size(); off += 4) {
                    std::uint32_t length{};
                    std::memcpy(&length, raw->data() + off, 4);
                    if (length < 4 || off + 4 + length > raw->size())
                        continue;
                    const std::uint8_t* candidate = raw->data() + off + 4;
                    // sfnt version tag: 0x00010000 (TrueType) or "OTTO" (CFF-flavoured
                    // OpenType, same family as the primary Source Han font) -- a cheap
                    // pre-filter before the heavier parse below, broader than the old check
                    // (which only recognised the TrueType tag).
                    const bool looks_like_sfnt =
                        (candidate[0] == 0 && candidate[1] == 1 && candidate[2] == 0 &&
                         candidate[3] == 0) ||
                        std::memcmp(candidate, "OTTO", 4) == 0;
                    if (!looks_like_sfnt)
                        continue;
                    stbtt_fontinfo probe{};
                    const int font_offset = stbtt_GetFontOffsetForIndex(candidate, 0);
                    if (font_offset < 0 || stbtt_InitFont(&probe, candidate, font_offset) == 0)
                        continue; // signature matched by coincidence; not a real, parseable font
                    thai_font_bytes_.assign(candidate, candidate + length);
                    break;
                }
            }
        }
        if (!thai_font_bytes_.empty()) {
            const auto thai = BuildLatinFontAtlas(thai_font_bytes_, font_->line_height, true, font_priority_);
            if (thai) {
                thai_first_page_ = (font_->atlas.height + FontPageHeight - 1) / FontPageHeight;
                stbtt_fontinfo primary{};
                stbtt_InitFont(&primary, font_bytes_.data(), stbtt_GetFontOffsetForIndex(font_bytes_.data(), 0));
                // Bounded by BOTH atlases' own glyph tables, not a hardcoded 0xFFFF: `font_`/
                // `thai` are both built from the same (now priority-sized, not always-full-BMP)
                // `cps`, so their `glyphs` vectors can be far smaller than the whole BMP range --
                // see BuildLatinFontAtlas's own `paged` comment.
                const auto last_cp = font_->first_codepoint +
                    static_cast<std::uint32_t>(std::min(font_->glyphs.size(), thai->glyphs.size())) - 1;
                for (std::uint32_t cp = font_->first_codepoint; cp <= last_cp; ++cp) {
                    auto g = thai->glyphs.at(cp - thai->first_codepoint);
                    if (g.w && g.h && ((cp >= 0x0E00 && cp <= 0x0E7F) ||
                                      stbtt_FindGlyphIndex(&primary, cp) == 0)) {
                        g.y += thai_first_page_ * FontPageHeight;
                        font_->glyphs.at(cp - font_->first_codepoint) = g;
                    }
                }
                font_->atlas.height = thai_first_page_ * FontPageHeight + thai->atlas.height;
            }
        }
    }
    return font_.has_value();
}

bool Art::FontGlyphs(std::uint32_t& first_codepoint, std::vector<EdenDsmodFontGlyph>& glyphs,
                     std::uint32_t& line_height) {
    std::lock_guard lock{mutex_};
    if (!BuildFontLocked())
        return false;
    first_codepoint = font_->first_codepoint;
    glyphs = font_->glyphs;
    line_height = font_->line_height;
    return true;
}

// Fit at a fixed cap height using the same unkerned advances as the host. A literal ASCII
// ellipsis also works before a language atlas containing U+2026 is resident.
std::string Art::FitLabel(std::string_view text, int width, int cap_height) {
    std::lock_guard lock{mutex_};
    if (!BuildFontLocked())
        return {};
    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info, font_bytes_.data(), stbtt_GetFontOffsetForIndex(font_bytes_.data(), 0)))
        return {};
    int x0{}, y0{}, x1{}, y1{};
    stbtt_GetGlyphBox(&info, stbtt_FindGlyphIndex(&info, 'H'), &x0, &y0, &x1, &y1);
    const float scale = static_cast<float>(cap_height) / y1;
    std::vector<std::pair<std::size_t, float>> ends;
    float total = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        std::uint32_t cp = lead;
        const int extra = lead < 0x80 ? 0 : lead < 0xE0 ? 1 : lead < 0xF0 ? 2 : 3;
        if (extra) {
            cp &= (1u << (6 - extra)) - 1;
            for (int j = 0; j < extra && i < text.size(); ++j)
                cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 0x3F);
        }
        int adv{}, bearing{};
        stbtt_GetCodepointHMetrics(&info, static_cast<int>(cp), &adv, &bearing);
        total += adv * scale;
        ends.emplace_back(i, total);
    }
    if (total <= width)
        return std::string{text};
    int adv{}, bearing{};
    stbtt_GetCodepointHMetrics(&info, '.', &adv, &bearing);
    const float dots = 3 * adv * scale;
    while (!ends.empty() && ends.back().second + dots > width)
        ends.pop_back();
    std::string out{text.substr(0, ends.empty() ? 0 : ends.back().first)};
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + "...";
}

// The title is visible before the host's asynchronous font atlas is ready. Decode its small
// version label directly from the same game font so that it never flashes the host fallback.
std::optional<ce_unity::Image> Art::VersionLabel() {
    auto image = LabelImage("V1.4.2", 100, 32, 20);
    if (!image) return std::nullopt;
    int left = 100, right = -1;
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 100; ++x)
        if (image->rgba[(y * 100 + x) * 4 + 3]) {
            left = std::min(left, x); right = std::max(right, x);
        }
    if (right < left) return std::nullopt;
    auto out = *image;
    std::fill(out.rgba.begin(), out.rgba.end(), 0);
    const int shift = (99 - left - right) / 2;
    // The native title uses a light fill and a one-pixel dark outline.
    for (int pass = 0; pass < 2; ++pass)
        for (int y = 1; y < 31; ++y) for (int x = left; x <= right; ++x) {
            const auto alpha = image->rgba[(y * 100 + x) * 4 + 3];
            if (!alpha) continue;
            for (int dy = pass ? 0 : -1; dy <= (pass ? 0 : 1); ++dy)
                for (int dx = pass ? 0 : -1; dx <= (pass ? 0 : 1); ++dx) {
                    const int tx = x + shift + dx, ty = y + dy;
                    if (tx < 0 || tx >= 100) continue;
                    auto* p = &out.rgba[(ty * 100 + tx) * 4];
                    p[0] = pass ? 0xFF : 0x1B;
                    p[1] = pass ? 0xF3 : 0x22;
                    p[2] = pass ? 0xDA : 0x33;
                    p[3] = std::max(p[3], alpha);
                }
        }
    return out;
}

std::optional<ce_unity::Image> Art::LabelImage(std::string_view text, int width, int height, int cap) {
    std::lock_guard lock(mutex_);
    if (!BuildFontLocked())
        return std::nullopt;
    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info, font_bytes_.data(), stbtt_GetFontOffsetForIndex(font_bytes_.data(), 0)))
        return std::nullopt;
    int x0{}, y0{}, x1{}, y1{};
    stbtt_GetGlyphBox(&info, stbtt_FindGlyphIndex(&info, 'H'), &x0, &y0, &x1, &y1);
    const float scale = static_cast<float>(cap) / y1;
    ce_unity::Image out;
    out.width = width;
    out.height = height;
    out.rgba.assign(out.width * out.height * 4, 0);
    int pen = 0;
    for (std::size_t at = 0; at < text.size();) {
        const auto lead = static_cast<unsigned char>(text[at++]);
        std::uint32_t cp = lead;
        const int extra = lead < 0x80 ? 0 : lead < 0xE0 ? 1 : lead < 0xF0 ? 2 : 3;
        if (extra) {
            cp &= (1u << (6 - extra)) - 1;
            for (int j = 0; j < extra && at < text.size(); ++j)
                cp = (cp << 6) | (static_cast<unsigned char>(text[at++]) & 0x3F);
        }
        const int gi = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
        int adv{}, lsb{};
        stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
        stbtt_GetGlyphBitmapBox(&info, gi, scale, scale, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        std::vector<std::uint8_t> ink(static_cast<std::size_t>(w) * h);
        stbtt_MakeGlyphBitmap(&info, ink.data(), w, h, w, scale, scale, gi);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int dx = pen + x0 + x, dy = cap + 4 + y0 + y;
                if (dx < 0 || dy < 0 || dx >= static_cast<int>(out.width) || dy >= static_cast<int>(out.height))
                    continue;
                auto* pixel = &out.rgba[(dy * out.width + dx) * 4];
                pixel[0] = 0x55; pixel[1] = 0x50; pixel[2] = 0x4A;
                pixel[3] = ink[y * w + x];
            }
        pen += static_cast<int>(std::lround(adv * scale));
    }
    return out;
}

std::optional<ce_unity::Image> Art::LoadMapBadge(std::string_view name) {
    const bool room = name.starts_with("room_");
    const bool monster = name.starts_with("monster_");
    if (!room && !monster && !name.starts_with("plain_")) return std::nullopt;
    name.remove_prefix(monster ? 8 : room ? 5 : 6);
    static constexpr std::pair<std::string_view, std::string_view> badges[] =
#include "mtas_map_badges.inc"
    ;
    ce_unity::Image out;
    out.width = out.height = 100;
    out.rgba.assign(100 * 100 * 4, 0);
    const auto stamp = [&](const char* sprite, int x, int y, int w, int h) {
        const auto image = LoadSprite(sprite, w, h);
        if (!image) return false;
        for (int sy = 0; sy < h; ++sy) for (int sx = 0; sx < w; ++sx) {
            auto* dst = &out.rgba[((y + sy) * 100 + x + sx) * 4];
            const auto* src = &image->rgba[(sy * w + sx) * 4];
            const unsigned alpha = src[3] * 255 + dst[3] * (255 - src[3]);
            if (!alpha) continue;
            for (int c = 0; c < 3; ++c)
                dst[c] = static_cast<std::uint8_t>((src[c] * src[3] * 255 + dst[c] * dst[3] * (255 - src[3])) / alpha);
            dst[3] = static_cast<std::uint8_t>(alpha / 255);
        }
        return true;
    };
    if (monster) {
        const auto split = name.find('~');
        if (split == std::string_view::npos) return std::nullopt;
        const auto sprite = std::string{name.substr(0, split)}, bg = std::string{name.substr(split + 1)};
        if (!bg.starts_with("I_Map_img_MonsterBg_") || !stamp(bg.c_str(), 0, 0, 100, 100) ||
            !stamp(sprite.c_str(), 26, 26, 48, 48)) return std::nullopt;
        return out;
    }
    if (name.starts_with("I_Map_") || name.ends_with("_Mini")) {
        const auto sprite = std::string{name};
        if (room && !stamp("I_Map_icon_Inroom_00", 0, 0, 100, 100)) return std::nullopt;
        if (!stamp(sprite.c_str(), 26, 26, 48, 48)) return std::nullopt;
        return out;
    }
    if (name.starts_with("MapIcon_MissionArea")) {
        const bool main = name.starts_with("MapIcon_MissionAreaMain");
        if (!stamp(main ? "I_Map_img_MissionArea_00" : "I_Map_img_MissionArea_01", 0, 0, 100, 100) ||
            !stamp(main ? "I_Map_img_MissionAreaInfo_00" : "I_Map_img_MissionAreaInfo_01", 26, 31, 48, 48)) return std::nullopt;
        return out;
    }
    const auto it = std::find_if(std::begin(badges), std::end(badges), [&](const auto& b) { return b.first == name; });
    if (it == std::end(badges)) return std::nullopt;
    if (room && !stamp("I_Map_icon_Inroom_00", 0, 0, 100, 100)) return std::nullopt;
    if (!stamp("I_Map_img_MissionBg_00", 26, 26, 48, 48) || !stamp(it->second.data(), 26, 26, 48, 48)) return std::nullopt;
    return out;
}


std::optional<ce_unity::Image> Art::FontAtlasImage(std::uint32_t page) {
    std::lock_guard lock{mutex_};
    if (!BuildFontLocked() || page >= (font_->atlas.height + FontPageHeight - 1) / FontPageHeight)
        return std::nullopt;
    // Only requested pages are rasterized. The runtime owns the 32 MiB page cache.
    ce_unity::Image image;
    image.width = font_->atlas.width;
    image.height = std::min(FontPageHeight, font_->atlas.height - page * FontPageHeight);
    image.rgba.assign(std::size_t{image.width} * image.height * 4, 255);
    for (std::size_t i = 3; i < image.rgba.size(); i += 4)
        image.rgba[i] = 0;
    stbtt_fontinfo info{};
    const auto& bytes = thai_first_page_ && page >= thai_first_page_ ? thai_font_bytes_ : font_bytes_;
    if (!stbtt_InitFont(&info, bytes.data(), stbtt_GetFontOffsetForIndex(bytes.data(), 0)))
        return std::nullopt;
    int x0, y0, x1, y1;
    stbtt_GetGlyphBox(&info, stbtt_FindGlyphIndex(&info, 'H'), &x0, &y0, &x1, &y1);
    const float scale = static_cast<float>(font_->line_height) / y1;
    for (std::size_t i = 0; i < font_->glyphs.size(); ++i) {
        const auto& g = font_->glyphs[i];
        if (!g.w || !g.h || g.y / FontPageHeight != page)
            continue;
        std::vector<std::uint8_t> coverage(std::size_t{g.w} * g.h);
        stbtt_MakeGlyphBitmap(&info, coverage.data(), g.w, g.h, g.w, scale, scale,
                             stbtt_FindGlyphIndex(&info, static_cast<int>(i + font_->first_codepoint)));
        for (std::uint32_t y = 0; y < g.h; ++y)
            for (std::uint32_t x = 0; x < g.w; ++x)
                image.rgba[((g.y % FontPageHeight + y) * image.width + g.x + x) * 4 + 3] =
                    coverage[y * g.w + x];
    }
    return image;
}

} // namespace mtas
