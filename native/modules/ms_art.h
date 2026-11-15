// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Monster Sanctuary art from the player's own romfs (nothing of the game's art is shipped).
// The package carries pictures.idx (tools/monster-sanctuary/build_picture_index.py): positions
// only -- where each texture's pixels start inside Data/data.unity3d (a UnityFS bundle of LZ4HC
// blocks, opened with the Chained Echoes reader), each 2D Toolkit sprite's rectangle and the
// game font's glyph rectangles and native localization string positions. Textures in this build are RGBA32, bottom-up rows, unswizzled.
//
// Keys served (module:ms:...):
//   spr<k>:<sprite>   the sprite scaled k times (nearest), k = 1..8; "turned" sprites (tk2d
//                     flipped) are turned back
//   font              the regular UI font as a host atlas (decode_font gives its glyphs)
//   txt:<font>:<k>:<w>:<a>:<text>  one line in a game font at k times, w mockup px wide
//   wrap:<font>:<k>:<w>:<lines>:<text>  the text word-wrapped into at most <lines> lines; "|"
//                     starts a new line (a description's own points, one per line)
//   box<k>:<sprite>[~<set>][@<step>] / row<k>:...  a monster in a 64x64 box / 64x54 Team row;
//                     a frame of <set>'s idle clip is placed by its anchor offset (A / O index
//                     records); @<n> adds the set's particle effects (P records) as they are
//                     n idle-clip frames (n / clip fps seconds) into their FxPeriod loop
//   Text may carry inline tokens: {i:<sprite>} draws that sprite right after the text before it
//   (placed from the rendered glyph advances), {s:<name>} the native string <name>.
#pragma once

#include "ms_fx.h"
#include "ms_mapdraw.h"

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ms {

struct PictureIndex {
    struct Texture {
        int node{};
        std::uint64_t offset{}, bytes{};
        int width{}, height{}, format{};
    };
    struct Sprite {
        int tex{};
        int x{}, y{}, w{}, h{};
        bool turned{};
    };
    struct Glyph {
        int x{}, y{}, w{}, h{};   ///< stored rect in the atlas (turned glyphs: transposed)
        int bearing_x{}, bearing_y{}, bottom{}, advance{}; ///< font units, y up
        bool turned{};
    };
    struct Font {
        float line_height{};
        int tex{-1};
        std::map<std::uint32_t, Glyph> glyphs;
    };
    struct Text {
        int node{};
        std::uint64_t offset{}; ///< length-prefixed LocaDataEntry.String in the serialized node
        std::uint32_t bytes{};
    };
    std::map<std::string, Text, std::less<>> strings;
    std::vector<std::string> nodes;
    std::map<int, Texture> textures;
    std::map<std::string, Sprite, std::less<>> sprites;
    std::map<std::string, Font, std::less<>> fonts;
    /// A battle sprite set's idle clip (tk2dSpriteAnimationClip as the game plays it).
    struct Clip {
        float fps{};
        int wrap{};       ///< tk2dSpriteAnimationClip.WrapMode (0 Loop, 1 LoopSection, 2 Once, 3 PingPong, ...)
        int loop_start{};
        std::vector<std::string> frames; ///< sprite names, in clip order (holds repeated)
    };
    std::map<std::string, Clip, std::less<>> clips; ///< by lower-case set key ("thanatos_light")
    /// A clip frame's top-left corner relative to the sprite's anchor (texture px, y down).
    std::map<std::string, std::pair<int, int>, std::less<>> offsets;
    /// Particle effects by lower-case set key ("spectralwolf_dark", "maptp").
    std::map<std::string, FxSet, std::less<>> fx;

    /// Parses the text index; false on a malformed line or a wrong version.
    bool Parse(std::string_view text);
};

/// Crops a sprite out of a decoded RGBA texture (top-left origin), turning it back when the
/// collection stored it turned (tk2d "flipped": transposed), then scales it `scale` times.
Picture CropSprite(const Picture& texture, const PictureIndex::Sprite& s, int scale);

/// The frame of `clip` shown `ms` milliseconds into its loop (tk2d wrap modes; unknown modes and
/// an empty clip give frame 0).
std::size_t ClipFrame(const PictureIndex::Clip& clip, double ms);

/// RGBA32 texture bytes (bottom-up rows) -> top-left-origin picture.
std::optional<Picture> DecodeRgba32(const std::uint8_t* data, std::size_t size, int width, int height);

class Art {
public:
    /// `read_romfs`: the host's read_romfs (path, offset, out, size; out == null -> size).
    using Romfs = std::function<std::size_t(const char*, std::uint64_t, void*, std::size_t)>;
    explicit Art(Romfs romfs);
    ~Art();

    /// "module:ms:spr4:ui/layout" / "module:ms:font"; nullopt for other keys or a failure.
    std::optional<Picture> Load(std::string_view key);
    /// The font glyphs for decode_font (regular font, laid out on the "font" atlas).
    bool Font(std::uint32_t& first, std::vector<EdenDsmodFontGlyph>& glyphs, std::uint32_t& line_height);
    /// English native localization, read from the player's bundle; empty on missing/bad data.
    std::string String(std::string_view name);
    /// `text` word-wrapped into lines at most `w` mockup px wide in `font` (the same measure as
    /// the "W:" wrap pictures; a word wider than the box gets a line of its own). nullopt while a
    /// decode holds the lock (never waits) or before pictures.idx loaded.
    std::optional<std::vector<std::string>> WrapLines(std::string_view font, int w, std::string_view text);
    /// Sprite names that failed lookup since the last call (each name reported once). Never
    /// waits for a running decode (empty then).
    std::vector<std::string> TakeMissing();
    /// A copy of the idle clips by lower-case set key (empty until pictures.idx loaded), so the
    /// sample thread can pick frames without taking the decode lock.
    std::map<std::string, PictureIndex::Clip, std::less<>> Clips();
    /// The set keys that carry particle effects (empty until pictures.idx loaded).
    std::set<std::string, std::less<>> FxSets();
    /// The particle layer alone: `set`'s effect at `seconds`, its emitter anchor at (ax, ay) of a
    /// transparent width x height picture, `scale` picture px per game pixel.
    std::optional<Picture> FxPicture(std::string_view set, double seconds, int frame, std::uint32_t width,
                                     std::uint32_t height, double ax, double ay, int scale);

private:
    bool LoadLocked();
    /// The cached decoded texture (shared_ptr: a crop can hold it across a cache eviction without
    /// copying the whole decode, which can be several MB).
    std::shared_ptr<const Picture> TextureLocked(int id);
    bool BuildFontLocked();
    std::optional<Picture> TextLocked(std::string_view font, std::string_view text, int scale);
    std::optional<Picture> WrapLocked(std::string_view key);
    std::string StringLocked(std::string_view name);
    const PictureIndex::Clip* ClipLocked(std::string_view sprite);
    void DrawFxLocked(std::string_view name, const FxSet& set, double seconds, int frame, Picture& out,
                      double ax, double ay, int scale);
    void MissLocked(std::string_view sprite);

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ms
