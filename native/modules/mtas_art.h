// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// My Time at Sandrock art from the player's own romfs (nothing of the game's art is shipped).
//
// The game's UnityFS bundles ship with NO embedded typetrees, even for built-in classes
// (confirmed empirically this session: every one of a sampled bundle's 4 types reports 0
// typetree nodes) -- so ce_unity::Reader's generic typetree walker (which, like Chained Echoes'
// own bundles, expects one) cannot read a Sprite/Texture2D/Font object here. Same conclusion
// Cult of the Lamb's reader reached for this game's Unity version (cotl_icons.h: "Sprite object
// raw (Unity 2022.3, no TypeTree)"); same fix: a fixed-offset PIN per asset (bundle, node, local
// byte offset, size, known width/height/format), built once offline with UnityPy against a
// per-family bundle/path-id index and verified byte-exact against UnityPy's own
// (typetree-independent) decode. At runtime this module only does the generic, format-stable
// part: open the bundle with ce_unity::Bundle (UnityFS block decompression, no typetree needed),
// read the pinned byte
// range, Tegra-deswizzle (ce_unity::SwitchDeswizzle) and ASTC-decode (host decode_astc) -- the
// same two steps Cult of the Lamb's DecodeSwitchSprite documents, generalised here to the 6x6
// block size this game's atlases use (confirmed via gobsPerBlock = 16 at m_PlatformBlob+8, same
// field cotl_icons.cpp reads) instead of its own 12x12.
//
// Font: SourceHanSans-Medium.otf (OTTO/CFF, 16,554,192 bytes) lives inline inside the `font/dummy`
// bundle's main serialized-file node (not streamed) -- pinned the same way. Rasterised at runtime
// with stb_truetype (CFF-capable since v1.18), same cap-height convention as
// lp_unity::BuildFontAtlas (Luminescent Platinum). All supported BMP glyphs have stable metrics;
// pixels are rasterized one page at a time into the runtime's bounded font-page cache.
#pragma once

#include "chained_echoes_unity.h"

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"

namespace mtas {

/// Byte location + decode recipe for one sprite, pinned offline (see header comment). All
/// offsets/sizes are absolute to the bundle's *node* (ce_unity::BundleNode::offset + this
/// value), matching ms_art.cpp's `node->offset + t->second.offset` convention.
struct SpritePin {
    const char* bundle;      ///< romfs-relative, under Data/StreamingAssets/AssetBundles/
    const char* data_node;   ///< node holding the raw (possibly streamed) texture bytes
    std::uint64_t node_offset; ///< local offset of the texture's own bytes within that node
    std::uint32_t atlas_w, atlas_h; ///< the backing atlas texture's full pixel size
    std::uint32_t gobs_per_block;   ///< Tegra swizzle: 1 << m_PlatformBlob[8..11]
    std::uint32_t block;            ///< ASTC block size: 6 (format 50) or 8 (format 51)
    // Unity sprite rect, bottom-left origin, pixels (float-rounded offline already).
    float rect_x, rect_y, rect_w, rect_h;
    // 9-slice border (left, bottom, right, top); all zero = no slicing (plain icon).
    float border_l, border_b, border_r, border_t;
};

/// One named entry of UiPins()/SpriteIndex()'s backing table. A plain aggregate (not
/// std::pair<std::string_view, SpritePin>) so the generated .inc files' existing
/// `{"name", {fields...}}` entries need no reformatting -- see SpritePinTable's own comment.
/// `name` is a fixed, null-padded byte buffer (longest of the 3,192 generated names is 44 bytes),
/// not a `std::string_view`/`const char*`: every one of these names is unique (unlike
/// `bundle`/`data_node` below, which many entries legitimately share), so a pointer field here
/// would need its OWN per-entry dynamic relocation in a position-independent .so -- measured
/// live (review-L3 finding 4's own follow-up): switching the whole table from
/// std::unordered_map to an array of pointer-keyed entries shrank .text a lot but grew the
/// shipped .so by ~150 KiB of added R_X86_64_RELATIVE relocations, net LARGER, backwards from the
/// fix's own point. Inlining the one field that is always unique (so can never be deduplicated
/// the way `bundle`/`data_node` already are across many entries) removes that many relocations.
struct NamedSpritePin {
    char name[48];
    SpritePin pin;
};

/// Read-only, sorted-by-name, binary-search-backed view over a NamedSpritePin table -- same
/// lookup surface as the old std::unordered_map<string_view, SpritePin> (at/count/size/
/// range-for) UiPins()/SpriteIndex() used to return, but the table itself is a plain array with
/// a compile-time-constant (unsorted) initial layout, sorted in place exactly once at runtime
/// (see mtas_art.cpp's UiPins()/SpriteIndex()/SortPinsByNameInPlace) instead of ~3,200 combined
/// runtime hash-insert call sites (review-L3 finding 4: this alone was measured at ~37% of the
/// whole linked module's .text).
class SpritePinTable {
public:
    constexpr explicit SpritePinTable(std::span<const NamedSpritePin> sorted_by_name)
        : entries_(sorted_by_name) {}
    const SpritePin* find(std::string_view name) const;
    const SpritePin& at(std::string_view name) const;
    std::size_t count(std::string_view name) const { return find(name) ? 1 : 0; }
    std::size_t size() const { return entries_.size(); }
    auto begin() const { return entries_.begin(); }
    auto end() const { return entries_.end(); }

private:
    std::span<const NamedSpritePin> entries_;
};

/// Every native UI sprite this companion's real Direction B pages use, keyed by a short stable
/// name (`module:mtas:ui:<name>:<w>:<h>`). Generated by tools/gen_pins_header.py from an offline
/// UnityPy resolve (tools/resolve_pins2.py) against this build's own romfs bundles -- see that
/// script for the exact resolution (SpriteAtlas renderDataMap lookup, m_PlatformBlob gobs field),
/// proven byte-exact against UnityPy's own decode before being pinned here. "Paper"/"Coin" are
/// the Stage-8 slice's original two sprites (day-cell parchment, gold coin), now folded into the
/// same table. Tab<Page>Nor/Sel + TabBtnBGNor/Sel + TabStar: the native pause-tab atlas
/// (I_Label_*). WorkshopIcon: the Workshop tab's own icon (no native "Workshop" tab exists,
/// matching the mockup's own substitution). Scroll{Track,Thumb,Cap}: the native list scrollbar
/// (I_OrderItem_img_scrollbar_*). MiniInfoBG: the dark HUD well weather/season icons sit in on
/// parchment. Season{Spring,Summer,Autumn,Winter}, ClockIcon: the HUD's own icons. KuangNote01:
/// the gold sticky note (hero mission card). NoteMain: the lighter note (clock/date/gold block).
/// PageMain: the Calendar's own corner-pinned cream page (B's signature background). Mission*,
/// OrderBG*, OrderBGSel, OrderNeedItemBG, OrderItemLimit: the Mission page's icons and order
/// cards. Winter{Round,Snowman}: the Calendar's winter decoration strip (this fixture's
/// season; other seasons' decoration is a follow-up, see STATUS.md).
const SpritePinTable& UiPins();
const SpritePinTable& MapPins();

/// Round-2 data lane: the dynamic counterpart of UiPins() -- item icon (ItemPrototype male/
/// femaleIconPath) and NPC Social-head portrait (bare localized NPC name, spaces stripped when
/// that's the only match -- the task's own "Fennec Fox"/"FennecFox" example) sprite names,
/// resolved offline the same byte-exact way (research/.../tools/resolve_sprite_index.py, same
/// method as resolve_pins2.py) from the real romfs's sprite-shard
/// system (ASSET-PATHS.md §8: 1,611 uisystem_sprite_split shards, no shipped name->shard index in
/// the game's own data). 3,102 names resolved this pass (3,035 item icons + 67 NPC portraits out
/// of 3,117/126 attempted; the rest were not found in the scanned sprite-name set at all -- no
/// dedicated bust art shipped for those NPCs, or no icon sprite for those items -- see
/// mtas_sprite_index_report.json, not shipped). Same guarantee as UiPins(): byte ranges only, no
/// pixel data or game text in the generated table itself.
const SpritePinTable& SpriteIndex();

/// Font object's raw OTF/CFF bytes, pinned the same way (see header comment).
struct FontPin {
    const char* bundle;
    const char* data_node;
    std::uint64_t node_offset;
    std::uint64_t length;
};

inline constexpr FontPin kPinFont{"font/dummy", "CAB-45ccedc16468a1b14b17c0d03955afca", 7700,
                                  16554192};

/// A whole texture's ASTC blocks, Tegra-deswizzled into linear block rows (padded to whole GOBs).
struct LinearAstc {
    std::vector<std::uint8_t> blocks;
    std::uint32_t row_blocks{}; ///< padded blocks per row
    std::uint32_t block{};      ///< ASTC block size (square)
    std::uint32_t atlas_w{}, atlas_h{};
};
std::optional<LinearAstc> ReadLinearAstc(ce_unity::Bundle& bundle, const SpritePin& pin);
/// Decodes only the blocks a rect (Unity bottom-left origin, pixels) overlaps; top-left-origin
/// RGBA out. Refuses rects above 16 MiB of RGBA (the host's own per-image cap).
std::optional<ce_unity::Image> CropLinearAstc(const LinearAstc& linear, float rect_x, float rect_y,
                                              float rect_w, float rect_h,
                                              const ce_unity::AstcDecoder& astc);
/// Square box-filter downscale by an integer factor (src side a multiple of `side`); a no-op
/// for any other shape.
ce_unity::Image BoxDownscale(const ce_unity::Image& src, std::uint32_t side);

/// ASTC blocks (already Tegra-deswizzled, linear) -> RGBA8, decoded through the whole atlas's
/// known gob/block layout, then cropped to the sprite's own rect (Unity bottom-left origin) and
/// optionally nine-sliced to a requested output size. Mirrors Cult of the Lamb's
/// DecodeSwitchSprite (cotl_icons.cpp), generalised to an arbitrary ASTC block size.
std::optional<ce_unity::Image> DecodeSwizzledAstcSprite(ce_unity::Bundle& bundle,
                                                        const SpritePin& pin,
                                                        const ce_unity::AstcDecoder& astc);

/// Nine-slice stretch of `src` (whose own border is `pin`'s) to `out_w` x `out_h`. `out_w`/
/// `out_h` must each be at least `border_l+border_r`/`border_b+border_t` + 1; returns nullopt
/// otherwise. A zero border on every side is a plain nearest-neighbour-free bilinear-free integer
/// scale (the coin icon case).
std::optional<ce_unity::Image> NineSlice(const ce_unity::Image& src, const SpritePin& pin,
                                         std::uint32_t out_w, std::uint32_t out_h);

/// One rasterised glyph in atlas pixels, source font units and the same bearing/advance
/// convention as lp_unity::BuildFontAtlas / EdenDsmodFontGlyph.
struct FontAtlas {
    ce_unity::Image atlas; ///< RGB 0xFF, A = coverage (top-left origin)
    std::uint32_t line_height{};
    std::uint32_t first_codepoint{};
    std::vector<EdenDsmodFontGlyph> glyphs; ///< dense run from first_codepoint
};

/// Latin-1 atlas, or BMP metrics with no pixel allocation when paged. cap_height_px is the pixel
/// height of 'H'; advance/bearing follow lp_unity::BuildFontAtlas's documented convention
/// (bearing_y = glyph top above the baseline, line_height = cap height).
std::optional<FontAtlas> BuildLatinFontAtlas(const std::vector<std::uint8_t>& otf_bytes,
                                             std::uint32_t cap_height_px, bool paged = false,
                                             const std::vector<std::uint32_t>& priority = {});
std::vector<std::uint32_t> DefaultCodepoints();
std::vector<std::uint32_t> FontCodepoints(std::string_view text);
inline constexpr std::uint32_t FontPageHeight = 128;

/// Bounded LRU of decoded (pre-nine-slice, native sprite-rect resolution) sprite pixels, keyed by
/// sprite name. Round-2: SpriteIndex() turns ResolveSprite/LoadSprite into an arbitrary-name
/// lookup (any of ~3,000 item icons or NPC portraits, not just the 90 fixed UI pins), so a long
/// play session could otherwise decode a great many distinct ASTC crops over time -- this bounds
/// both the number of distinct sprites held and their total RGBA byte footprint, evicting the
/// least-recently-used entry first. Nine-slicing/scaling to a caller's requested (w,h) still runs
/// per request (cheap, ScaleRect); only the Tegra-deswizzle + ASTC-decode step is cached. Pure
/// bookkeeping, no decode of its own -- unit-tested directly with synthetic images
/// (mtas_publish_test.cpp).
class SpriteLru {
public:
    explicit SpriteLru(std::size_t max_entries, std::size_t max_bytes)
        : max_entries_(max_entries), max_bytes_(max_bytes) {}

    /// nullptr on a miss; touches recency on a hit.
    const ce_unity::Image* Get(const std::string& name);
    void Put(std::string name, ce_unity::Image image);
    void Clear();

    std::size_t EntryCount() const {
        return order_.size();
    }
    std::size_t ByteCount() const {
        return bytes_;
    }

private:
    void EvictLocked();

    std::size_t max_entries_;
    std::size_t max_bytes_;
    std::size_t bytes_{0};
    std::list<std::string> order_; ///< front = most recently used
    std::unordered_map<std::string, std::pair<ce_unity::Image, std::list<std::string>::iterator>>
        map_;
};

/// Opens bundles through a host read_romfs-backed ce_unity::Bundle, applies the pins above and
/// caches results. Thread-safe (guards its own state; ce_unity::Bundle itself is already
/// thread-safe per its own header).
class Art {
public:
    explicit Art(ce_unity::RangeReader romfs, ce_unity::AstcDecoder astc = {});

    /// The base extensions' decode_astc is only available after configure(), which runs after
    /// create() -- set it once it is (mirrors Luminescent Platinum's own
    /// reader.Assets().SetAstcDecoder(...) in its Configure callback).
    void SetAstcDecoder(ce_unity::AstcDecoder astc);

    /// Decoded, nine-sliced to (w,h); nullopt on any decode failure or an unknown name (never
    /// fabricated pixels). `name` is looked up in UiPins() first (the 90 fixed UI chrome
    /// sprites), then SpriteIndex() (round-2: item icons / NPC portraits) -- a caller does not
    /// need to know which table a name came from. Same "module:mtas:ui:<name>:<w>:<h>" key route
    /// 0100B63016916000.cpp already forwards generically to this function, so DATA-CONTRACT.md's
    /// "Icons for UI" publishing recipe needs no module-side code change.
    std::optional<ce_unity::Image> LoadSprite(std::string_view name, std::uint32_t w,
                                              std::uint32_t h);
    /// Round-2: the sprite at its own native resolution (the pin's rect size, before any
    /// nine-slice/scale), from the bounded LRU cache above on a repeat request. The lower-level
    /// primitive LoadSprite builds on; exposed directly for a caller that wants the native size or
    /// wants to decode once and request several output sizes without re-running ASTC decode each
    /// time.
    std::optional<ce_unity::Image> ResolveSprite(std::string_view name);
    /// One tile of a big square map texture pin (`grid` x `grid` tiles, row 0 = the visual top),
    /// box-downscaled to `px` square. The texture's deswizzled ASTC blocks (4 MiB for the
    /// 4096 px world map) are kept for the session so each tile decodes only its own blocks.
    std::optional<ce_unity::Image> LoadMapTile(std::string_view pin_name, std::uint32_t col,
                                               std::uint32_t row, std::uint32_t grid,
                                               std::uint32_t px);
    /// The whole map texture pin as one `grid * px` square picture, stitched from LoadMapTile
    /// tiles (row 0 = the visual top). A single LoadMapTile(.., 1, ..) of the 4096 px world map
    /// is refused by CropLinearAstc's 16 MiB per-crop cap, so the picture is built tile by tile.
    std::optional<ce_unity::Image> LoadMapWhole(std::string_view pin_name, std::uint32_t grid,
                                                std::uint32_t px);
    void SetFontText(const std::string& text);
    /// Builds (once, lazily) and returns the font glyph table for decode_font.
    std::optional<ce_unity::Image> LoadMapBadge(std::string_view name);
    std::optional<ce_unity::Image> VersionLabel();
    std::optional<ce_unity::Image> LabelImage(std::string_view text, int width, int height, int cap);
    std::string FitLabel(std::string_view text, int width, int cap_height);
    bool FontGlyphs(std::uint32_t& first_codepoint, std::vector<EdenDsmodFontGlyph>& glyphs,
                    std::uint32_t& line_height);
    /// The same build's atlas image, for the font_atlas `module:` image request.
    std::optional<ce_unity::Image> FontAtlasImage(std::uint32_t page = 0);

    /// Cache occupancy (entries, bytes) -- for a test/diagnostic, not production logic.
    std::pair<std::size_t, std::size_t> SpriteCacheStats();
    std::pair<std::size_t, std::size_t> BundleCacheStats();
    // 8 MiB decoded bundles + 24 MiB native sprites; host images have their own 32 MiB budget.
    static constexpr std::size_t BundleCacheBudget = 8u * 1024u * 1024u;

private:
    std::shared_ptr<ce_unity::Bundle> OpenBundleLocked(const std::string& bundle);
    void TrimBundlesLocked();
    bool BuildFontLocked();
    /// UiPins() then SpriteIndex(); nullptr if `name` is in neither.
    static const SpritePin* FindPin(std::string_view name);
    /// Caller already holds mutex_.
    std::optional<ce_unity::Image> ResolveSpriteLocked(std::string_view name,
                                                       const SpritePin& pin);

    ce_unity::RangeReader romfs_;
    ce_unity::AstcDecoder astc_;
    std::mutex mutex_;
    std::list<std::pair<std::string, std::shared_ptr<ce_unity::Bundle>>> bundles_;
    std::optional<FontAtlas> font_;
    std::vector<std::uint8_t> font_bytes_, thai_font_bytes_;
    std::uint32_t thai_first_page_{};
    bool font_build_attempted_{false};
    std::vector<std::uint32_t> font_priority_;
    // Task's suggested bound: <= 64 sprites / <= 24 MiB of decoded RGBA (native sprite
    // resolution, pre-nine-slice -- an item icon or NPC portrait is a few KiB to ~100 KiB each at
    // the rect sizes seen this pass, so 24 MiB comfortably covers 64 of the largest ones with
    // headroom).
    SpriteLru sprite_cache_{64, 24u * 1024u * 1024u};
    std::string map_pin_;
    std::optional<LinearAstc> map_linear_;
};

} // namespace mtas
