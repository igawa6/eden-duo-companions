// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Mario Kart 8 Deluxe: the asset-free art, text and map-projection side of the module.
// Everything is decoded at runtime from the player's own romfs through host.read_romfs; no
// pixel, glyph or string of the game is shipped. The game-memory reader is a separate part of
// the module and talks to this file only through Library (images, map cameras, message text)
// and MapCamera::Project. Every file layout below was checked against the whole 4.0.0 romfs and
// the CTGP-DX 1.1.1 course files.
//
// Image keys (the part after "module:mk8d:", also accepted with that prefix):
//   map/<course folder>             Course/<c>/course_maptexture.bntx, texture ym_Map_<c>^*
//   chara/<name>                    UI/cmn/common.sarc#cm_L_CharaIcon_00.szs, tc_MapChara_<name>^*
//   item/<name>                     UI/cmn/race.sarc#rc_L_ItemBox_00.szs, tc_Item_<name>^*
//   cup/<name>[/<variant>]          UI/cmn/race/ym_CupIcon<name>_<variant><sfx>.bntx
//                                   (variant Line (default: white emblem + alpha), 130x130
//                                   (colour badge), Line128x128, Nml (normal map))
//   coursepict/<name>               UI/cmn/menu/ym_CoursePict_<name>_00^u.bntx
//   lyt/<dir>/<sarc>/<layout>/<tex> UI/<dir>/<sarc>.sarc#<layout>.szs, texture <tex> (exact)
//   lytmat/<dir>/<sarc>/<layout>/<material>
//                                   the layout material's first texture, each channel
//                                   interpolated between the material's black and white colours
//   mapfit/<course>                 the course map cut to its MapFitBox (see MapFit)
//   emblem/<code>                   Kart/Emblem/Emblem_<code>.szs (Yaz0 BFRES, embedded
//                                   textures.bntx), texture Emblem_<code> (the kart emblem,
//                                   e.g. "Mro", "Dkg"; the driver -> code table is the reader's)
// <name> is the game's own texture-name component ("Mario", "Banana", "Mushroom", "DLC01");
// the reader's id -> name mapping lives in mk8d_ids.h.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"

namespace Mk8dAssets {

struct AstcDecoder {
    void* userdata{};
    EdenDsmodAstcDecoder decode{};
};

/// Straight RGBA8, row-major, top-left origin.
struct Image {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
};

/// One texture descriptor of a BNTX (BRTI), as stored in the file.
struct TextureInfo {
    std::string name;
    std::uint32_t width{}, height{}, format{}, image_size{}, arrays{}, block_height_log2{};
    std::uint32_t channel_map{};
    std::uint16_t tile_mode{};
    std::uint64_t image_offset{}; ///< mip 0, from the start of the BNTX
};

/// course_mapcamera.bin (45 bytes: 11 little-endian floats + a text newline) and the game's own
/// minimap projection, from the 4.0.0 code (loader and sead LookAtCamera, cited in Parse):
///   floats 0-2 LookAtCamera position, 3-5 look-at point, 6-8 up (normalised on load),
///   float 9 ortho width (left/right = -/+ w/2), float 10 ortho height (top/bottom = +/- h/2).
/// view = LookAt matrix * world; ndc = (2 vx / w, 2 vy / h); the icon lands on the 480x480
/// P_MapDummy_00 pane at ndc * 240, so u = 0.5 + ndc.x / 2 and v = 0.5 - ndc.y / 2 (top-left).
/// Verified live: equal to the layout positions the game itself writes for its minimap icon
/// panes (4.0.0 and 3.0.3, every racer, normal and mirror mode).
struct MapCamera {
    float position[3]{};
    float look_at[3]{};
    float up[3]{};     ///< normalised, as the game stores it
    float width{};     ///< ortho extent along the camera's right axis (world units)
    float height{};    ///< ortho extent along the camera's up axis
    float row[3][4]{}; ///< sead LookAtCamera view matrix rows: right, up, direction (+ translation)

    static std::optional<MapCamera> Parse(std::span<const std::uint8_t> file);
    /// Project a world point to map-texture coordinates (0..1, top-left origin; values outside
    /// 0..1 are off the map). world_y_scale reproduces the game's per-course Y scale (4.0.0 code
    /// multiplies world Y by 6.2 when the race course id is 0x44; the reader passes the scale,
    /// Mk8dIds::MapWorldYScale). Mirror mode needs no change: the game flips the texture and the
    /// icons together, so texture coordinates are the same.
    bool Project(float x, float y, float z, float& u, float& v, float world_y_scale = 1.0f) const;
};

/// Map "fit" box (pages v2.1): the part of a course's map texture the companion zooms to.
/// Deterministic, from the texture alone: the bounding box of every texel with alpha > 0 (the
/// road and the game's own soft halo), grown by MapFitMargin of its larger side on each edge,
/// then grown symmetrically about its centre to the aspect MapFitAspectW:MapFitAspectH (the
/// page's map area). It may extend past the texture; the image key pads with transparency.
/// Normalised texture coordinates (0..1, top-left origin, the same space as r{i}.map_x/y):
///   fit_x = (map_x - u0) / (u1 - u0), fit_y = (map_y - v0) / (v1 - v0).
inline constexpr int MapFitAspectW = 1;
inline constexpr int MapFitAspectH = 1;
inline constexpr float MapFitMargin = 0.02f;
struct MapFit {
    float u0{}, v0{}, u1{1}, v1{1};
};

/// Advance widths of a BFFNT font (CWDH "char width") by code point (CMAP), in font units: what
/// the dual-screen runtime lays text out with (its width = sum(advance) * 5 * scale / cap height).
/// Used to fit a name into a column by choosing the text scale (pages v3, r{i}.name_scale).
struct FontAdvances {
    std::unordered_map<std::uint32_t, std::uint16_t> advance;
    std::uint16_t fallback{};
    /// Sum of the advances of a UTF-8 string (unknown code points: `fallback`).
    std::uint32_t Measure(std::string_view utf8) const;
};

/// Message text of one MSBT file: label -> UTF-8 text with the control tags removed.
struct MessageTable {
    std::unordered_map<std::string, std::string> texts;
    const std::string* Find(std::string_view label) const;
};

// ---- stateless decoders (exposed for tests and for the reader's own use) --------------------

std::optional<std::vector<std::uint8_t>> Yaz0Decompress(std::span<const std::uint8_t> in);
/// SARC member by name (name table) or by the name's SFAT hash when the archive has no names.
std::optional<std::span<const std::uint8_t>> SarcMember(std::span<const std::uint8_t> sarc,
                                                        std::string_view name);
std::uint32_t SarcHash(std::string_view name, std::uint32_t multiplier = 0x65);
std::optional<std::vector<TextureInfo>> ParseBntx(std::span<const std::uint8_t> bntx);
std::optional<Image> DecodeTexture(std::span<const std::uint8_t> bntx, const TextureInfo& texture,
                                   const AstcDecoder& astc);
std::optional<MessageTable> ParseMsbt(std::span<const std::uint8_t> msbt);
/// BFFNT (v4, either byte order): CMAP (direct / table / scan) + CWDH -> advance per code point.
std::optional<FontAdvances> ParseBffntAdvances(std::span<const std::uint8_t> bffnt);
/// BFLYT: the first texture of a material and its black/white interpolation colours (RGBA8).
std::optional<std::string> LayoutMaterialTexture(std::span<const std::uint8_t> flyt,
                                                 std::string_view material,
                                                 std::array<std::uint8_t, 4>* black,
                                                 std::array<std::uint8_t, 4>* white);

/// Per-module-instance, thread-safe, lazily filled cache over the player's romfs. load_image
/// (the runtime's asset worker), the module's own worker jobs and the tick thread may call it
/// concurrently. Images and archives are LRU-bounded; map cameras, message files and fonts are
/// kept per course / file (a few hundred small entries at most).
class Library {
public:
    explicit Library(std::size_t image_budget = 48 * 1024 * 1024,
                     std::size_t archive_budget = 24 * 1024 * 1024);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    void SetAstcDecoder(AstcDecoder decoder);
    void Clear();

    /// A key from the table above; null for an unknown key or any decode failure.
    std::shared_ptr<const Image> LoadImage(const EdenDsmodHostApi& host, std::string_view key);
    /// The map camera of a course folder ("Gu_FirstCircuit"); cached per course.
    std::shared_ptr<const MapCamera> LoadMapCamera(const EdenDsmodHostApi& host,
                                                   std::string_view course);
    /// UI/<lang>/message.sarc#<file>.msbt, cached. lang "USen", file "Common".
    std::shared_ptr<const MessageTable> LoadMessages(const EdenDsmodHostApi& host,
                                                     std::string_view lang, std::string_view file);
    /// UI/<lang>/font.sarc#<member> advance widths, cached (e.g. "USen",
    /// "turbo_MARIOFont.bffnt" -- the page font). Null on failure.
    std::shared_ptr<const FontAdvances> LoadFontAdvances(const EdenDsmodHostApi& host,
                                                         std::string_view lang,
                                                         std::string_view member);
    /// Convenience: one label's text, "" when missing.
    std::string Text(const EdenDsmodHostApi& host, std::string_view lang, std::string_view file,
                     std::string_view label);
    /// Texture names inside a key's container (map/<c>, chara/_, item/_, lyt/<d>/<s>/<l>/_):
    /// for tests and for id -> name checks.
    std::vector<std::string> ListTextures(const EdenDsmodHostApi& host, std::string_view key);

    // Key builders ("module:mk8d:<kind>/<name>"); empty when the name is not a safe component.
    static std::string MapKey(std::string_view course);
    static std::string CharaKey(std::string_view name);
    static std::string ItemKey(std::string_view name);
    static std::string CupKey(std::string_view name);
    static std::string CoursePictKey(std::string_view name);
    static std::string EmblemKey(std::string_view code);
    /// module:mk8d:mapfit/<course>: the map texture cut to MapFitBox (nearest, texel grid,
    /// transparent outside the texture).
    static std::string MapFitKey(std::string_view course);
    /// The fit box of a course's map (cached with the image); nullopt if the map fails.
    std::optional<MapFit> MapFitBox(const EdenDsmodHostApi& host, std::string_view course);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Mk8dAssets
