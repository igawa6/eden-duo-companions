// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Build-pinned, key-free reads from the owner's merged Octopath Traveler 0
// Switch 1.0.5 pak. Every entry header and stored hash is checked before Oodle;
// decoded hashes are pinned too.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ot0 {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

using Sha1Digest = std::array<u8, 20>;
Sha1Digest Sha1(std::span<const u8> bytes);

/// Little-endian loads (archive and cooked package fields).
inline u32 Le32(const u8 *p) {
  return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24;
}
inline u64 Le64(const u8 *p) { return u64{Le32(p)} | u64{Le32(p + 4)} << 32; }

/// romfs path of the archive and its pinned size (merged base + update,
/// Switch 1.0.5).
inline constexpr std::string_view PakPath =
    "romfs:Octopath_Traveler0/Content/Paks/pakchunk0-Switch.pak";
inline constexpr u64 PakSize = 1594117919ULL;

struct Block {
  u64 start; ///< relative to the member offset
  u64 end;
};

struct PakMember {
  std::string_view path; ///< informational (the index is not read)
  u64 offset;            ///< FPakEntry offset in the archive
  u64 compressed_size;   ///< stored payload bytes (== sum of blocks for Oodle
                         ///< members)
  u64 size;              ///< decoded bytes
  u32 compression;       ///< 0 none, 1 Oodle (this archive's method table)
  u32 header_bytes;      ///< in-data FPakEntry length
  u32 block_size;        ///< FPakEntry CompressionBlockSize
  std::span<const Block> blocks;
  Sha1Digest stored_sha1;  ///< = the FPakEntry hash
  Sha1Digest decoded_sha1; ///< whole decoded member
};

enum class MemberId : u32 {
#define X(name) name,
#include "ot0_member_ids.inc"
#undef X
  Count,
};
const PakMember &Member(MemberId id);

/// Reads exactly `size` bytes at `offset` of the archive; false on a short
/// read.
using RangeReader =
    std::function<bool(u64 offset, void *out, std::size_t size)>;

/// Parses an in-data FPakEntry and checks it against the pin. Empty string =
/// match.
std::string CheckEntryHeader(std::span<const u8> header, const PakMember &pin);

/// Checks the pinned stored hash again, then decodes concatenated blocks.
/// Each Oodle block decodes to min(block_size, remaining). nullopt on any ooz
/// failure.
std::optional<std::vector<u8>> DecodeStored(std::span<const u8> stored,
                                            const PakMember &pin);

/// Header check, stored-hash check, decode, decoded-hash check. `why` gets the
/// failure reason.
std::optional<std::vector<u8>> ReadMember(const RangeReader &read,
                                          const PakMember &pin,
                                          std::string *why = nullptr);
/// ReadMember of each pin in order; stops at the first failure with `why` =
/// "<path>: <reason>".
std::optional<std::vector<std::vector<u8>>>
ReadMembers(const RangeReader &read, std::span<const PakMember *const> pins,
            std::string *why = nullptr);

struct Image {
  u32 width{};
  u32 height{};
  std::vector<u8> rgba; ///< straight RGBA8, row-major
};

enum class TextureFormat : u32 { Bc1, Bc3, Bgra8 };
struct TexturePin {
  TextureFormat format;
  u32 width;
  u32 height;
  u32 format_at; ///< Pixel format FString bytes; dimensions precede this by 16
                 ///< bytes.
  u32 data_at; ///< First inline mip bytes after the format and three mip-header
               ///< fields.
  u32 data_bytes;
};
std::optional<TexturePin> TextureLayout(MemberId id);

// The native BC decoder requires aligned input. These wrappers copy one block
// first.
void DecodeBc1Block(const u8 *block, u8 *dst, std::size_t x, std::size_t y,
                    std::size_t width, std::size_t height);
void DecodeBc3Block(const u8 *block, u8 *dst, std::size_t x, std::size_t y,
                    std::size_t width, std::size_t height);
// Validate native dimensions, format, mip header and trailer before decoding
// RGBA8.
std::optional<Image> DecodeTexture(std::span<const u8> uexp,
                                   const TexturePin &pin,
                                   std::string *why = nullptr);
// Decode only the requested rectangle (all three formats); bounds checked
// without overflow.
std::optional<Image> DecodeTextureRect(std::span<const u8> uexp,
                                       const TexturePin &pin,
                                       const std::array<u32, 4> &rect,
                                       std::string *why = nullptr);
} // namespace ot0
