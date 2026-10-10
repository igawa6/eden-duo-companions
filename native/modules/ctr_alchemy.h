// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Crash Team Racing Nitro-Fueled: Alchemy engine containers read from the player's romfs.
//   igArchive v13 (.pak)   header, TOC, name table; members are stored or LZMA blocks of 0x8000
//                          bytes ("u32 packed size, 5 property bytes, raw stream"), each block
//                          starting on a sector boundary.
//   IGZ v10                section table at 0x14, fixup tables (TMET type names, ...), objects
//                          with a type index at +0. Pointers are (section << 27) | offset.
//   igImage2               +0x18 width/height/depth/mips u16, +0x2C format (an EXID reference),
//                          +0x38 byte size, +0x40 pixel pointer. Pixels are linear and stored
//                          bottom-up. EXID 0x37456ecd = BC3, 0x44e04333 = 8-bit luminance+alpha.
//   igBitmapFont           igCharMetrics objects: +0x0C code point, +0x10 kern left/right, step,
//                          offset xy, size wh, bounding min/max, uv min/max (all f32).
//   <track>_Minimap.igz    common_Octane_MinimapData: widget w,h (f32), bounds v0..v3 (f32), and
//                          the ERacingMinimapDirection (i32) 0x2C after w.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace CtrAlchemy {

using Bytes = std::vector<std::uint8_t>;
/// Reads `size` bytes at `offset` of one romfs file; returns the count read.
using ReadAt = std::function<std::size_t(std::uint64_t offset, void* out, std::size_t size)>;

struct Entry {
    std::uint32_t offset{};
    std::uint32_t length{};
    std::uint32_t mode{};
    std::string name; ///< the short path ("textures/...igz")
};

/// One opened igArchive: the TOC only; members are read on demand.
class Archive {
public:
    bool Open(const ReadAt& read);
    const std::vector<Entry>& Entries() const {
        return entries;
    }
    /// First member whose short name contains every fragment.
    const Entry* Find(std::initializer_list<std::string_view> fragments) const;
    /// The member with exactly this name.
    const Entry* FindExact(std::string_view name) const;
    /// `read` reads the same archive file Open saw (the TOC holds no reader: hosts lend theirs
    /// per call, and one archive may be read from the asset worker and the timing thread).
    bool Read(const Entry& e, const ReadAt& read, Bytes& out) const;

private:
    std::uint32_t sector{0x800};
    std::vector<Entry> entries;
};

struct Image {
    std::uint32_t width{};
    std::uint32_t height{};
    Bytes rgba; ///< tightly packed RGBA8, top-down
};

/// The first igImage2 of an IGZ, decoded to top-down RGBA (BC3, or luminance+alpha as L,L,L,A).
std::optional<Image> DecodeImage(const Bytes& igz);

struct Glyph {
    float step{}, kern_left{}, off_x{}, off_y{}, w{}, h{};
    float u0{}, v0{}, u1{}, v1{};
};
struct Font {
    float cell_height{}; ///< tallest glyph cell, the reference size
    std::unordered_map<std::uint32_t, Glyph> glyphs;
    std::uint32_t atlas_w{}, atlas_h{};
    Bytes atlas_la; ///< 2 bytes per texel: luminance (1 = fill, 0 = outline), alpha; as stored
};
std::optional<Font> DecodeFont(const Bytes& igz);

struct MinimapParams {
    float w{}, h{};
    float v[4]{};
    std::int32_t direction{-1};
};
std::optional<MinimapParams> ParseMinimap(const Bytes& igz);

/// World (x, y) -> texture pixel of an `tex` x `tex` minimap texture (top-down image), the
/// game's projection with the texture drawn at twice the widget scale (verified on 5 tracks).
void ProjectToTexture(const MinimapParams& p, float x, float y, float tex, float& px, float& py);

} // namespace CtrAlchemy
