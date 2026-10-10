// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Self-contained Unity asset reader for the Pokemon Brilliant Diamond 1.3.0 + Luminescent
// Platinum companion (Unity 2019.4.27f1, Switch). Reads the player's own romfs; nothing decoded
// is persisted or shipped.
//
//   UnityFS bundle (format 6/7, "5.x.x")
//     header (big endian): signature, u32 format, cstr player version, cstr engine version,
//     i64 file size, u32 compressed / uncompressed block-info size, u32 flags
//       flags & 0x3F  block-info compression (0 none, 2 LZ4, 3 LZ4HC; 1 LZMA is rejected)
//       flags & 0x40  blocks and directory combined     flags & 0x80  block info at file end
//       flags & 0x200 padding before the first block (2020.3.34+ only; this game never sets it)
//     format >= 7 (and every 2019.4.15+ bundle): the block info starts 16-aligned.
//     block info: 16-byte hash, i32 block count {u32 usize, u32 csize, u16 flags}, i32 node count
//     {i64 offset, i64 size, u32 flags, cstr path}. Nodes are CAB-<hash> (SerializedFile) and
//     CAB-<hash>.resS (texture bytes), addressed in the concatenated uncompressed block stream.
//   SerializedFile (version 21/22 here, little endian on Switch): metadata (unity version,
//     platform 38, embedded typetrees in bundles; resources.assets has none), object table
//     (path id, byte start, size, type index), script types, externals.
//   Objects are read by a generic typetree walker into lp_unity::Value. Font objects without a
//   typetree use the 2019.4 Font layout built in (same field order UnityPy's TPK yields).
//
// Texture decode matches UnityPy 1.25 (export/Texture2DConverter.py):
//   * Switch block-linear deswizzle only when m_PlatformBlob is >= 12 bytes and its
//     gobsPerBlock (1 << u32 at +8) is > 1 (2019.4 textures have no PlatformBlob: linear data);
//   * RGBA32 / ARGB32 / BGRA32 / RGB24 / Alpha8 (RGB 0, A = value, as Pillow) / R8 / DXT1 /
//     DXT5 / BC4 / BC5 / BC7 on the CPU (bc_decoder), ASTC LDR (any block size) through the
//     injected AstcDecoder; other formats (unused by the UI bundles) return nullopt;
//   * output RGBA8 with a top-left origin (Unity rows are bottom-up; UnityPy flips).
// Sprites match UnityPy's Sprite.image: atlas render data when the sprite is packed in a
// SpriteAtlas (m_SpriteAtlas, else the atlas named by m_AtlasTags[0]), textureRect crop with
// Python round() on the float rect, packingRotation applied when settingsRaw.packed is set.
// Tight packing (packingMode 0, e.g. the pokemon_l party icons) applies UnityPy's mask_sprite:
// the m_RD mesh triangles (float positions - min) * pixelsToUnits, truncated to int, filled with
// a port of Pillow 12.3's polygon scanline; pixels outside become (0,0,0,0). UnityPy's other
// tight path (render_sprite_mesh, used only when the mesh has non-zero UV0) is not implemented:
// such sprites get the mask instead (none of the checked UI sprites have UV0).
//
// Threading: every public function is safe to call from several threads. Reader keeps a small
// mutex-guarded LRU of parsed bundles (tables, sprite index, atlas map, decompressed blocks).
// Nothing throws out of the public API: failures return std::nullopt / empty.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"

namespace lp_unity {

/// Hard cap on any single allocation driven by file contents.
inline constexpr std::size_t MaxAlloc = 64u * 1024u * 1024u;
/// Hard cap on the number of Value nodes one object read may create.
inline constexpr std::size_t MaxValues = 4u * 1024u * 1024u;

/// Whole-file reader: nullopt when the file is missing.
using RomfsReader = std::function<std::optional<std::vector<std::uint8_t>>(std::string_view path)>;

/// Partial reader. `size` returns the file size (nullopt = missing); `read` fills exactly
/// `size` bytes at `offset` and returns false on any failure. The module binds both to
/// host->read_romfs (out == NULL returns the size); resources.assets is 56 MB, so prefer this.
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

/// ASTC blocks (linear, row-major block order) -> tightly packed RGBA8 of w x h.
using AstcDecoder = std::function<bool(const std::uint8_t* blocks, std::size_t size,
                                       std::uint32_t w, std::uint32_t h, std::uint32_t bw,
                                       std::uint32_t bh, std::vector<std::uint8_t>& rgba)>;
/// Binds the host's EdenDsmodHostExtensions::decode_astc (userdata is the host's).
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
    std::string s; ///< String, and Bytes (TypelessData, vector<UInt8>/<char>/<SInt8>)
    std::vector<Value> items;                          ///< Array (pair = 2 items)
    std::vector<std::pair<std::string, Value>> fields; ///< Object, in serialization order

    /// Field (Object) or nullptr. Also accepts "first"/"second" on a pair array.
    const Value* Get(std::string_view name) const;
    /// Dotted path, e.g. "m_RD.textureRect.x".
    const Value* Path(std::string_view dotted) const;
    std::int64_t AsInt(std::int64_t fallback = 0) const;
    double AsFloat(double fallback = 0.0) const;
    std::string_view AsString() const; ///< String/Bytes contents, else empty
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

/// A parsed UnityFS archive. Thread-safe (block cache under its own mutex). NodeSource keeps the
/// bundle alive.
class Bundle : public std::enable_shared_from_this<Bundle> {
public:
    static std::shared_ptr<Bundle> Open(ByteSource file);
    ~Bundle();
    const std::vector<BundleNode>& Nodes() const;
    const BundleNode* FindNode(std::string_view path_or_basename) const;
    /// Uncompressed stream range (node offset + offset).
    bool Read(std::uint64_t offset, std::uint8_t* out, std::size_t size) const;
    ByteSource NodeSource(const BundleNode& node) const;
    /// Decompressed blocks this bundle keeps (its block LRU, at most BlockCacheBytes).
    std::size_t CachedBytes() const;
    /// NodeSource keeps a node of up to this size in memory (the serialized-file tables).
    static constexpr std::uint64_t PreloadBytes = 8u * 1024u * 1024u;
    static constexpr std::size_t BlockCacheBytes = 24u * 1024u * 1024u;
    std::string engine_version;
    std::uint32_t format{};

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
    const std::vector<ObjectInfo>& Objects() const;
    const ObjectInfo* Find(std::int64_t path_id) const;
    std::optional<std::vector<std::uint8_t>> ReadRaw(const ObjectInfo& obj) const;
    /// Typetree-driven read (embedded tree, or the built-in Font tree for class 128).
    std::optional<Value> ReadObject(const ObjectInfo& obj) const;
    /// m_Name without parsing the whole object (walks the typetree up to m_Name, so it also
    /// works for MonoBehaviour, whose m_Name follows m_GameObject/m_Enabled/m_Script).
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
    std::string stream_path; ///< "archive:/CAB-x/CAB-x.resS" or empty
    std::vector<std::uint8_t> platform_blob;
    std::vector<std::uint8_t> image_data; ///< inline "image data" (empty when streamed)
};

struct SpriteInfo {
    std::string name;
    Rectf rect;
    float offset_x{}, offset_y{}, pixels_to_units{};
    std::vector<std::string> atlas_tags;
    PPtr sprite_atlas;
    std::uint8_t render_key_guid[16]{};
    std::int64_t render_key_second{};
    // Effective render data (the atlas entry when packed, else m_RD).
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

/// Romfs prefix of the asset-bundle tree; bundle paths below are relative to it.
inline constexpr std::string_view DefaultAssetRoot = "/Data/StreamingAssets/AssetAssistant/";

/// Bytes of preloaded serialized-file tables and decompressed blocks a Reader keeps across all
/// of its open bundles (the newest bundle always stays).
inline constexpr std::uint64_t CacheBudget = 96u * 1024u * 1024u;

/// Bundle-level access with a small LRU of parsed bundles (at most `max_bundles`, CacheBudget).
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

    /// Sprite names in the bundle (object order).
    std::vector<std::string> ListSprites(std::string_view bundle);
    /// Texture2D names in the bundle (object order).
    std::vector<std::string> ListTextures(std::string_view bundle);
    std::optional<SpriteInfo> GetSprite(std::string_view bundle, std::string_view sprite);

    /// Sprite pixels like UnityPy's Sprite.image (only the needed ASTC blocks are decoded).
    std::optional<Image> LoadSprite(std::string_view bundle, std::string_view sprite);
    /// Whole texture by name.
    std::optional<Image> LoadTexture(std::string_view bundle, std::string_view texture);

    /// Party icon: UIs/textures_mass/pokemon_l/pm{species:04}_{form:02}_{gender:02}_{shiny:02}_l,
    /// sprite pm..._L; gender falls back across the requested one, then 00, 01, 02; shiny falls
    /// back to non-shiny (no shiny pokemon_l bundles exist), then form to 00.
    std::optional<Image> LoadPokemonIcon(int species, int form, int gender, bool shiny);
    /// Item icon: sprite item_{id:04} in UIs/textures_mass/texturemass.
    std::optional<Image> LoadItemIcon(int item_id);

    // Generic typetree objects (MonoBehaviour data tables, messages, ...).
    /// (m_Name, path id) of every object of `class_id` (-1 = every class) in object order.
    /// MonoBehaviour is class 114.
    std::vector<std::pair<std::string, std::int64_t>> ListObjects(std::string_view bundle,
                                                                  std::int32_t class_id = -1);
    /// The first object named `name` of `class_id` (default MonoBehaviour; -1 = any class: note
    /// that a bundle's MonoScript objects share their MonoBehaviours' names), via its typetree.
    std::optional<Value> ReadObjectByName(std::string_view bundle, std::string_view name,
                                          std::int32_t class_id = 114);
    std::optional<Value> ReadObject(std::string_view bundle, std::int64_t path_id);

private:
    struct Loaded;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// Message table (MonoBehaviour with labelDataArray, e.g. bundle "Message/english" object
/// "english_ss_wazaname"; the resident tables such as english_ss_monsname are in
/// "Message/common_msbt"):
/// result[labelIndex] = the concatenated wordDataArray[].str of that
/// label (empty for gaps). Empty vector when the object is missing. With event_newlines, a word
/// whose eventID is 1 (the end of a native text line) is followed by '\n', and labels that share
/// an index are appended rather than replaced. With array_index, numeric item lookups use
/// result[arrayIndex] instead; e.g. LP ITEMNAME_1633 has arrayIndex 1833 (Incense Burner).
std::vector<std::string> ReadMessageTable(Reader& reader, std::string_view lang_bundle,
                                          std::string_view object_name,
                                          bool event_newlines = false, bool array_index = false);
// Decode an already-read table; numeric item lookups use arrayIndex, named labels use labelIndex.
std::vector<std::string> DecodeMessageTable(const Value& table, bool event_newlines = false,
                                            bool array_index = false);

std::string PokemonIconBundle(int species, int form, int gender, bool shiny);
std::string PokemonIconSprite(int species, int form, int gender, bool shiny);
std::string ItemIconSprite(int item_id);

/// Common UI bundles (relative to the asset root).
namespace bundles {
inline constexpr std::string_view SharedUi = "UIs/shareduiassets/sharedui";
inline constexpr std::string_view SharedUiLangEn = "UIs/shareduiassets/sharedui_lang.en";
inline constexpr std::string_view BattleLangEn = "UIs/shareduiassets/battle_lang.en";
inline constexpr std::string_view Common = "UIs/textures/common";
inline constexpr std::string_view CommonLangEn = "UIs/textures/common_lang.en";
inline constexpr std::string_view Pokemon = "UIs/textures/pokemon";
inline constexpr std::string_view MenuTop = "UIs/textures/menutop";
inline constexpr std::string_view TextureMass = "UIs/textures_mass/texturemass";
inline constexpr std::string_view MessageEnglish = "Message/english";
inline constexpr std::string_view PersonalMasterdatas = "Pml/personal_masterdatas";
} // namespace bundles

// ---- fonts ---------------------------------------------------------------------------------

// The game's Western text font (FOT-UDKakugoC80Pro-DB) lives in the BASE Data/resources.assets
// (the Luminescent overlay's resources.assets only holds LiberationSans), as a Font object
// without a typetree.

/// Font m_FontData (OTF/TTF bytes) by Font m_Name.
std::optional<std::vector<std::uint8_t>> ExtractFontData(const RangeReader& read,
                                                         std::string_view assets_path,
                                                         std::string_view font_name);
/// Font m_FontData from a font bundle (the 1.3.0 layout: Dpr/font/efigs_font holds
/// FOT-UDKakugoC80Pro-DB, kor_font / sch_font / tch_font the Korean and Chinese faces).
std::optional<std::vector<std::uint8_t>> ExtractBundleFontData(Reader& reader, std::string_view bundle,
                                                               std::string_view font_name);
/// Font names in a serialized file.
std::vector<std::string> ListFonts(const RangeReader& read, std::string_view assets_path);

/// CJK faces: Han, kana and Hangul fill a box about 1.15-1.2 times the face's Latin cap height,
/// centred on the cap band, so at the companion's cap-based sizes a line of them stood taller than
/// Latin text and reached over the cap line into the edges of tight rows (the move bars). Latin text
/// looks centred a little under the cap band's middle (its lower case). BuildFontAtlas draws these
/// glyphs at their own scale and height: the reference ideograph ('国', '한' or 'あ'; its ink box
/// y0..y1 in font units, y up) becomes CjkInk times the cap height tall, centred CjkCentre cap
/// heights above the baseline. Their advances follow the smaller scale; the line height does not
/// change, and the runtime and lp_text measure from the same metrics, so every width and height
/// stays exact.
inline constexpr float CjkInk = 1.05f, CjkCentre = 0.45f;
struct CjkMetrics {
    float scale = 0; ///< atlas px per font unit for these glyphs
    int shift = 0;   ///< px they move down from where that scale puts them
};
inline CjkMetrics CjkMetricsFor(int y0, int y1, std::uint32_t cap_height_px, float base_scale) {
    CjkMetrics m;
    m.scale = base_scale;
    if (y1 <= y0)
        return m;
    const float cap = static_cast<float>(cap_height_px);
    m.scale = std::min(base_scale, CjkInk * cap / static_cast<float>(y1 - y0));
    const float shift = static_cast<float>(y0 + y1) * 0.5f * m.scale - CjkCentre * cap;
    m.shift = static_cast<int>(shift >= 0 ? shift + 0.5f : shift - 0.5f);
    return m;
}
/// The scripts written in that em box: CJK symbols and punctuation, kana, Han, Hangul (syllables
/// and jamo), CJK compatibility ideographs and the full-width forms.
inline bool CjkCentred(std::uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3000 && cp <= 0x30FF) || (cp >= 0x3130 && cp <= 0x318F) ||
           (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

struct FontAtlas {
    Image atlas; ///< RGB 0xFF, A = coverage
    std::uint32_t line_height{};
    std::uint32_t first_codepoint{};
    std::vector<EdenDsmodFontGlyph> glyphs; ///< dense run from first_codepoint
    float scale{};                          ///< font units -> atlas pixels
    std::int32_t ascent_px{}, descent_px{}; ///< hhea/OS2 metrics in atlas pixels
    std::vector<std::uint32_t> missing;     ///< requested codepoints the font lacks
    std::uint32_t page_h{};                 ///< rows per page (0 = one atlas)
    CjkMetrics cjk{};                       ///< how CJK glyphs were drawn (CjkMetricsFor)
};

/// ASCII, Latin-1, en/em dash, curly quotes, bullet, ellipsis, multiplication sign, male/female
/// signs, and what the language mods' tables add: Œ œ ― „ U+202F ℃ → − ★ ■ ♥ U+3000 ー.
/// (U+20BD is not in FOT-UDKakugoC80Pro-DB; the game draws the Poke Dollar otherwise.)
std::vector<std::uint32_t> DefaultCodepoints();

/// Rasterises `codepoints` with stb_truetype (TrueType and CFF outlines). cap_height_px is the
/// height of 'H' in atlas pixels and becomes line_height (the host's convention: line_height =
/// the cap height, see wonder_font.h). Glyphs absent from the font get an empty box advancing
/// like a space and are listed in `missing`. nullopt on a bad font or an atlas over 4096 px.
/// page_h > 0 builds a paged atlas (runtime 17 font_page_h): no glyph crosses a multiple of page_h,
/// and when one page cannot hold the set the atlas is 2048 px wide and as many pages tall as needed.
/// `fallback` (optional, another face) draws the code points `font` lacks, and the accented Latin
/// letters U+00A0-U+024F, at the same cap height; only what neither face has is listed in `missing`.
std::optional<FontAtlas> BuildFontAtlas(std::span<const std::uint8_t> font,
                                        std::uint32_t cap_height_px,
                                        std::span<const std::uint32_t> codepoints,
                                        std::uint32_t page_h = 0,
                                        std::span<const std::uint8_t> fallback = {});

} // namespace lp_unity
