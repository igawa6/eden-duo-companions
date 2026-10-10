// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Unity asset reader for the Chained Echoes 1.41 companion (0100C510166F0000, Unity 2020.3.36f1,
// Switch). Decodes the player's own installed romfs at runtime; nothing decoded is persisted or
// shipped. Contract and measurements: research/chained-echoes/contracts/unity-assets.md.
//
//   Bundles: Data/StreamingAssets/aa/Switch/<name>.bundle (Addressables; 637 bundles, every one
//     UnityFS format 7, "5.x.x" / "2020.3.36f1", header flags 0x243 = block info LZ4HC (3),
//     blocks+directory combined (0x40), 16-byte padding before the first block (0x200, the
//     2020.3.34+ meaning)). Data blocks are 128 KiB LZ4HC chunks. Bundle file names are stable
//     for the selected 1.41 build, so the Addressables catalog (aa/catalog.json) is not needed to
//     map names to bundles; sprite families that span several bundles use fixed search lists.
//   Nodes: CAB-<hash> (SerializedFile v22, little endian, platform 38, embedded typetrees) and
//     CAB-<hash>.resS (streamed texture bytes; every needed Texture2D is streamed).
//   Texture2D formats used by the companion: RGBA32 (4), RGB24 (3), Alpha8 (1, the CE font
//     atlas) and DXT5 (12, Overdrive skill tiles); others decoded too (ARGB32, DXT1, BC4/5/7).
//     Switch block-linear swizzle when m_PlatformBlob gobsPerBlock (1 << u32 @ +8) > 1 (padded
//     to whole GOB blocks; swizzled RGB24 is stored as RGBA32), as UnityPy 1.25 decodes it.
//   Sprites: no SpriteAtlas; m_RD.textureRect crop (Python round() on the float rect) of the
//     whole texture; settingsRaw 64 (tight mesh, packingMode tight) or 0. Tight sprites carry no
//     UV0, so UnityPy masks the crop with the m_RD mesh triangles (Pillow polygon fill): pixels
//     outside become (0,0,0,0). That is reproduced bit for bit (MaskSprite).
//   Font: the TextMesh Pro font asset "CE" (MonoBehaviour, script TMP_FontAsset) in systemgfx,
//     atlas Texture2D "CE Atlas" 256x256 Alpha8 (swizzled, gobsPerBlock 16), 97 characters
//     (32-126, 160, 8203, 8230, 9633), point size 27, cap line 21, glyph rects bottom-up.
//
// Threading: every public function may be called from several threads (the module's asset
// worker and the timing thread). Nothing throws out of the public API: failures return
// std::nullopt / empty.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

namespace ce_unity {

/// Hard cap on any single allocation driven by file contents.
inline constexpr std::size_t MaxAlloc = 64u * 1024u * 1024u;
/// Hard cap on the number of Value nodes one object read may create.
inline constexpr std::size_t MaxValues = 4u * 1024u * 1024u;

/// Whole-file reader: nullopt when the file is missing.
using RomfsReader = std::function<std::optional<std::vector<std::uint8_t>>(std::string_view path)>;

/// Partial reader. `size` returns the file size (nullopt = missing); `read` fills exactly
/// `size` bytes at `offset` and returns false on any failure.
struct RangeReader {
    std::function<std::optional<std::uint64_t>(std::string_view path)> size;
    std::function<bool(std::string_view path, std::uint64_t offset, std::uint8_t* out,
                       std::size_t size)>
        read;
    explicit operator bool() const {
        return size && read;
    }
};

/// Adapts a whole-file reader (keeps the most recently read file, thread-safe).
RangeReader MakeRangeReader(RomfsReader whole);
/// host.read_romfs as a RangeReader (out == NULL returns the size; reads chunked to 32 MiB).
/// The host API pointer must outlive the reader.
RangeReader MakeHostRangeReader(const EdenDsmodHostApi* host);
/// A directory on disk standing in for the romfs (tests and the dev tool).
RangeReader MakeDirectoryRangeReader(std::string root);

/// ASTC blocks -> RGBA8. Chained Echoes ships no ASTC texture; kept for the shared core.
using AstcDecoder = std::function<bool(const std::uint8_t* blocks, std::size_t size,
                                       std::uint32_t w, std::uint32_t h, std::uint32_t bw,
                                       std::uint32_t bh, std::vector<std::uint8_t>& rgba)>;
AstcDecoder MakeHostAstcDecoder(void* userdata, EdenDsmodAstcDecoder decode);

struct Image {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba; ///< RGBA8, top-left origin, width * height * 4
};

// ---- low level: values, bundles, serialized files -------------------------------------------

/// Dynamic value produced by the typetree walker. Integers are Int (signed types) or UInt;
/// arrays of UInt8/SInt8/char become Bytes; pair is a 2-item Array; map is an Array of pairs;
/// PPtr is an Object {m_FileID, m_PathID}.
struct Value {
    enum class Kind : std::uint8_t { None, Int, UInt, Float, Bool, String, Bytes, Array, Object };
    Kind kind{Kind::None};
    std::int64_t i{};
    std::uint64_t u{};
    double f{};
    std::string s;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> fields;

    const Value* Get(std::string_view name) const;
    /// Dotted path, e.g. "m_RD.textureRect.x".
    const Value* Path(std::string_view dotted) const;
    std::int64_t AsInt(std::int64_t fallback = 0) const;
    double AsFloat(double fallback = 0.0) const;
    std::string_view AsString() const;
};

struct PPtr {
    std::int32_t file_id{};
    std::int64_t path_id{};
    bool IsNull() const {
        return file_id == 0 && path_id == 0;
    }
};

struct ObjectInfo {
    std::int64_t path_id{};
    std::uint64_t byte_start{}; ///< absolute, data offset applied
    std::uint32_t byte_size{};
    std::int32_t class_id{};
    std::int32_t type_index{};
};

struct BundleNode {
    std::string path;
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint32_t flags{};
};

/// Random-access byte source (absolute offsets).
struct ByteSource {
    std::uint64_t size{};
    std::function<bool(std::uint64_t offset, std::uint8_t* out, std::size_t size)> read;
};

/// A parsed UnityFS archive. Thread-safe (block cache under its own mutex).
class Bundle : public std::enable_shared_from_this<Bundle> {
public:
    static std::shared_ptr<Bundle> Open(ByteSource file);
    ~Bundle();
    const std::vector<BundleNode>& Nodes() const;
    const BundleNode* FindNode(std::string_view path_or_basename) const;
    bool Read(std::uint64_t offset, std::uint8_t* out, std::size_t size) const;
    ByteSource NodeSource(const BundleNode& node) const;
    /// (compression = block flags & 0x3F, block count, uncompressed bytes) per compression type.
    std::vector<std::array<std::uint64_t, 3>> BlockSummary() const;
    std::string engine_version;
    std::uint32_t format{};
    std::uint32_t header_flags{};

private:
    Bundle();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// A parsed SerializedFile (metadata only; object bytes are read on demand).
class SerializedFile {
public:
    static std::shared_ptr<SerializedFile> Open(ByteSource file);
    ~SerializedFile();
    std::int32_t Platform() const;
    std::uint32_t Version() const;
    bool HasTypeTrees() const;
    const std::vector<ObjectInfo>& Objects() const;
    const ObjectInfo* Find(std::int64_t path_id) const;
    std::optional<std::vector<std::uint8_t>> ReadRaw(const ObjectInfo& obj) const;
    std::optional<Value> ReadObject(const ObjectInfo& obj) const;
    std::optional<std::string> PeekName(const ObjectInfo& obj) const;

private:
    SerializedFile();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// ---- typed views -------------------------------------------------------------------------

struct Rectf {
    float x{}, y{}, width{}, height{};
};

struct TextureInfo {
    std::string name;
    std::int32_t width{}, height{}, format{};
    std::uint64_t stream_offset{};
    std::uint32_t stream_size{};
    std::string stream_path;
    std::vector<std::uint8_t> platform_blob;
    std::vector<std::uint8_t> image_data;
};

struct SpriteInfo {
    std::string name;
    Rectf rect;                       ///< m_Rect (full sprite rect, pixels)
    float offset_x{}, offset_y{}, pixels_to_units{};
    float pivot_x{0.5f}, pivot_y{0.5f}; ///< m_Pivot (normalised, bottom-left origin)
    std::vector<std::string> atlas_tags;
    PPtr sprite_atlas;
    std::uint8_t render_key_guid[16]{};
    std::int64_t render_key_second{};
    PPtr texture;
    PPtr alpha_texture;
    Rectf texture_rect;
    float texture_rect_offset_x{}, texture_rect_offset_y{};
    std::uint32_t settings_raw{};
    bool packed() const {
        return settings_raw & 1;
    }
    bool tight() const {
        return ((settings_raw >> 1) & 1) == 0;
    }
    std::uint32_t rotation() const {
        return (settings_raw >> 2) & 0xF;
    }
};

/// Texture bytes -> RGBA8 (top-left origin), exactly like UnityPy's Texture2D.image.
std::optional<Image> DecodeTextureData(std::span<const std::uint8_t> data, const TextureInfo& tex,
                                       const AstcDecoder& astc, std::int32_t platform = 38);

/// Tegra block-linear -> linear, UnityPy's TextureSwizzler.deswizzle (16-byte texels).
std::vector<std::uint8_t> SwitchDeswizzle(std::span<const std::uint8_t> data, std::uint32_t width,
                                          std::uint32_t height, std::uint32_t block_width,
                                          std::uint32_t block_height,
                                          std::uint32_t gobs_per_block);

// ---- high level -------------------------------------------------------------------------------

/// Romfs prefix of the Addressables bundle tree; bundle names below are relative to it.
inline constexpr std::string_view DefaultAssetRoot = "Data/StreamingAssets/aa/Switch/";

/// A sprite's location inside its bundle for one game build (serialized node index, path id).
struct SpritePin {
    std::uint32_t file{};
    std::int64_t path_id{};
};

/// Bundle-level access with a small LRU of parsed bundles (tables, sprite index, decompressed
/// blocks). Sprite/texture names resolve to the FIRST object of that name in object-table order.
class Reader {
public:
    explicit Reader(RangeReader read, AstcDecoder astc = {},
                    std::string asset_root = std::string{DefaultAssetRoot},
                    std::size_t max_bundles = 6);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    void SetAstcDecoder(AstcDecoder astc);
    void ClearCache();
    /// Bundle header facts (for the contract/tool); nullptr when missing.
    std::shared_ptr<const Bundle> OpenBundle(std::string_view bundle);
    std::shared_ptr<const SerializedFile> OpenSerialized(std::string_view bundle);

    std::vector<std::string> ListSprites(std::string_view bundle);
    std::vector<std::string> ListTextures(std::string_view bundle);
    std::optional<SpriteInfo> GetSprite(std::string_view bundle, std::string_view sprite);
    /// Sprite pixels like UnityPy's Sprite.image.
    std::optional<Image> LoadSprite(std::string_view bundle, std::string_view sprite);
    /// Whole texture by name.
    std::optional<Image> LoadTexture(std::string_view bundle, std::string_view texture);

    std::vector<std::pair<std::string, std::int64_t>> ListObjects(std::string_view bundle,
                                                                  std::int32_t class_id = -1);
    std::optional<Value> ReadObjectByName(std::string_view bundle, std::string_view name,
                                          std::int32_t class_id = 114);
    std::optional<Value> ReadObject(std::string_view bundle, std::int64_t path_id);

    /// Drops a bundle from the LRU (its tables, name index and block cache). The map page calls this
    /// after decoding from mapstextures (153k objects: ~25 MiB of tables + index when resident).
    void Evict(std::string_view bundle);
    /// Where the name index found `sprite` (for generating a pin table with the dev tool).
    std::optional<SpritePin> FindSpritePin(std::string_view bundle, std::string_view sprite);
    /// LoadSprite through a pinned (serialized file, path id): the object's class and m_Name are
    /// checked, then it decodes without building the bundle's name index (saves ~24 ms and ~10 MiB
    /// on mapstextures). A stale pin (another game build) falls back to the name index.
    std::optional<Image> LoadSpritePinned(std::string_view bundle, std::string_view sprite,
                                          SpritePin pin);

    /// LoadSprite / LoadSpriteAny consult the built-in 1.41 pin table first (default on).
    void SetUseBuiltinPins(bool use);

    /// True when the bundle's sprite index contains `sprite` (indexes the bundle on first use).
    bool HasSprite(std::string_view bundle, std::string_view sprite);
    /// The first bundle of `bundles` that has the sprite, then its pixels. `found_in` receives the
    /// bundle name. Bundles are indexed lazily in list order and stay in the LRU.
    std::optional<Image> LoadSpriteAny(std::span<const std::string_view> bundles,
                                       std::string_view sprite, std::string* found_in = nullptr);

private:
    struct Loaded;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// Built-in pins for 1.41 (chained_echoes_unity_pins.inc, generated by `ce-unity-tool pins`): every
/// sprite of the accepted design, the UX-SPEC asset map extras, all 26 area maps and every ctb_ /
/// bestiary_ / portrait_ family member (764 rows). Object locations only, verified on use.
std::optional<SpritePin> BuiltinPin(std::string_view bundle, std::string_view sprite);
std::size_t BuiltinPinCount();

// ---- Chained Echoes bundles and sprite families ---------------------------------------------

namespace bundles {
inline constexpr std::string_view SystemGfx = "systemgfx_assets_all.bundle";
inline constexpr std::string_view PackedAssets = "packedassets_assets_all.bundle";
inline constexpr std::string_view Gfx2 = "gfx2_assets_all.bundle";
inline constexpr std::string_view Gfx = "gfx_assets_all.bundle";
inline constexpr std::string_view Bestiary = "bestiary_assets_all.bundle";
inline constexpr std::string_view MapsTextures = "mapstextures_assets_all.bundle";
inline constexpr std::string_view Sprites2 = "sprites2_assets_all.bundle";
inline constexpr std::string_view StartMenu = "scenes_scenes_startmenu.bundle";
inline constexpr std::string_view Enemies2 = "enemies2_assets_all.bundle";
inline constexpr std::string_view ElrantGfx = "elrantgfx_assets_all_dff358e3c538eb46787ddbb31d0a44ad.bundle";
inline constexpr std::string_view DuplicateIsolation6 =
    "duplicateassetisolation6_assets_all_9cc672ce30231e5cf83a797758ea9f71.bundle";
inline constexpr std::string_view ElrantLevels = "elrantlevels_scenes_all_59c2b9ccb64074a7297230c516a83983.bundle";
inline constexpr std::string_view BrTunnel = "scenes_scenes_br_tunnel.bundle";
inline constexpr std::string_view FiorWoods = "areasfiorwoods_scenes_all.bundle";
} // namespace bundles

/// Sprite families whose members live in different bundles (each id in exactly one bundle in
/// 1.41; full-asset-pull counts in the contract). Lists are ordered by member count, the costly
/// scene bundles last.
enum class Family { Ctb, Bestiary, Portrait };
std::span<const std::string_view> FamilyBundles(Family family);
/// "ctb_<id>", "bestiary_<id>", "portrait_<name>".
std::string FamilySprite(Family family, std::string_view id);

// ---- loose serialized files (TextAsset tables) -----------------------------------------------

/// Data/resources.assets: the game's TextAsset tables (rewardBoard, the skill/crystal tables the
/// design reads names and base costs from). Not a bundle: a SerializedFile read in ranges.
inline constexpr std::string_view ResourcesAssets = "Data/resources.assets";

/// m_Script of the TextAsset named `name` (or with `path_id` when name is empty) in a loose
/// serialized file. Uses the embedded typetree, else the fixed TextAsset layout (m_Name, m_Script).
std::optional<std::string> ReadTextAsset(const RangeReader& read, std::string_view assets_path,
                                         std::string_view name, std::int64_t path_id = 0);

// ---- TextMesh Pro font ------------------------------------------------------------------------

struct TmpGlyph {
    std::uint32_t index{};
    float width{}, height{}, bearing_x{}, bearing_y{}, advance{};
    std::int32_t rect_x{}, rect_y{}, rect_w{}, rect_h{}; ///< atlas pixels, y from the BOTTOM
    float scale{1.0f};
    std::int32_t atlas_index{};
};

struct TmpFont {
    std::string name, family, style;
    float point_size{}, scale{}, line_height{}, ascent{}, cap_line{}, mean_line{}, baseline{},
        descent{};
    std::int32_t atlas_w{}, atlas_h{}, padding{};
    std::vector<TmpGlyph> glyphs;                 ///< m_GlyphTable order
    std::map<std::uint32_t, std::uint32_t> chars; ///< unicode -> index into glyphs
    PPtr atlas;                                   ///< m_AtlasTextures[0]
    Image atlas_image;                            ///< decoded atlas (top-left origin), may be empty

    const TmpGlyph* Find(std::uint32_t codepoint) const;
};

/// The TMP_FontAsset MonoBehaviour `name` (default: the CE font in systemgfx) and its atlas.
std::optional<TmpFont> LoadTmpFont(Reader& reader, std::string_view bundle = bundles::SystemGfx,
                                   std::string_view name = "CE", bool with_atlas = true);

/// One glyph's coverage as an RGBA tile (top-left origin, glyph rect size): colour channels 255,
/// alpha = atlas alpha, or binarised at `threshold` (alpha >= threshold -> 255, else 0) when
/// threshold >= 0 (the accepted mockups use 128: crisp pixel text without the atlas AA fringe).
std::optional<Image> GlyphTile(const TmpFont& font, std::uint32_t codepoint, int threshold = -1);

/// The whole atlas as a host font atlas (RGB 255, A = coverage, optionally binarised).
Image HostFontAtlas(const TmpFont& font, int threshold = -1);

/// Host font metrics (EdenDsmodFontSink): glyphs run densely from the lowest code point to the
/// highest one <= max_codepoint (gaps empty). The default keeps U+2026 (the native ellipsis used
/// for truncation) and drops U+25A1 (box), giving a 8199-entry run of 14-byte records. Mapping:
/// x = rect_x, y = atlas_h - rect_y - rect_h (top-left), w, h = glyph rect, bearing_x/bearing_y/
/// advance = Python-rounded metrics (bearing_y = glyph top above the baseline);
/// line_height = round(cap_line) = 21 (the host convention: line height = cap height).
bool BuildHostGlyphs(const TmpFont& font, std::uint32_t& first_codepoint,
                     std::vector<EdenDsmodFontGlyph>& glyphs, std::uint32_t& line_height,
                     std::uint32_t max_codepoint = 0x2026);

// ---- process-wide counters (development measurement; cheap relaxed atomics) -------------------

struct Stats {
    std::uint64_t romfs_reads{};          ///< RangeReader read calls made by bundles
    std::uint64_t romfs_bytes{};          ///< bytes those calls fetched
    std::uint64_t blocks_decompressed{};  ///< LZ4 data blocks inflated (cache misses)
    std::uint64_t bytes_decompressed{};
    std::uint64_t bundles_opened{};
    std::uint64_t objects_indexed{};      ///< names peeked while indexing bundles
};
Stats GetStats();
void ResetStats();
/// Per-open-bundle cache of decompressed 128 KiB blocks (default 2 MiB).
void SetBlockCacheBudget(std::size_t bytes);

} // namespace ce_unity
