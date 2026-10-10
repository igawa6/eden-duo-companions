// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Dragon Quest III HD-2D Remake (01003E601E324000, Switch 1.1.0.0): build-pinned access to a few
// members of the player's own Nicola/Content/Paks/Nicola-Switch.pak (UE4 pak v11, the merged
// base + v131072 update archive).
//
// Only the pak INDEX is AES-encrypted; every member used here is stored unencrypted, so no key is
// needed: the module carries a pinned table {offset, sizes, block list, hashes} for exactly the
// members it reads and fetches byte ranges through host read_romfs.
//
// Each member is read as:
//   [offset, offset + header_bytes)   the in-data FPakEntry (v11): u64 0, u64 compressed size,
//                                     u64 size, u32 compression index, u8 sha1[20],
//                                     (compressed: u32 n, n x {u64 start, u64 end}), u8 flags,
//                                     u32 block size. Must equal the pinned values field by field.
//   blocks [start, end) relative to offset, contiguous; stored bytes SHA-1 = pinned = entry hash.
//   Oodle (index 1) blocks through ooz, each to min(block size, remaining) bytes, then the whole
//   decoded member's SHA-1 = pinned. ooz is not fuzz safe: nothing reaches it before the stored
//   hash matched.
//
// Textures (.uexp, cooked UTexture2D): the inline first mip. Pinned per texture: width/height/slices
// at +266, the pixel format FString at +278 (length incl. NUL) / +282 ("PF_BC7" or "PF_B8G8R8A8"),
// right after it the mip header {first 0, mips 1, cooked 1, flags 72, count = size, size} and an
// 8-byte offset, then the data (BC7 at +321, B8G8R8A8 at +326), then {w, h, 1}.
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

namespace dq3 {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

using Sha1Digest = std::array<u8, 20>;
Sha1Digest Sha1(std::span<const u8> bytes);

/// Little-endian loads (archive and cooked package fields).
inline u32 Le32(const u8* p) {
    return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24;
}
inline u64 Le64(const u8* p) {
    return u64{Le32(p)} | u64{Le32(p + 4)} << 32;
}

/// romfs path of the archive and its pinned size (merged base + update, Switch 1.1.0.0).
inline constexpr std::string_view PakPath = "romfs:Nicola/Content/Paks/Nicola-Switch.pak";
inline constexpr u64 PakSize = 7392078816ULL;

struct Block {
    u64 start; ///< relative to the member offset
    u64 end;
};

struct PakMember {
    std::string_view path;  ///< informational (the index is not read)
    u64 offset;             ///< FPakEntry offset in the archive
    u64 compressed_size;    ///< stored payload bytes (== sum of blocks for Oodle members)
    u64 size;               ///< decoded bytes
    u32 compression;        ///< 0 none, 1 Oodle (this archive's method table)
    u32 header_bytes;       ///< in-data FPakEntry length
    u32 block_size;         ///< FPakEntry CompressionBlockSize
    std::span<const Block> blocks;
    Sha1Digest stored_sha1;  ///< = the FPakEntry hash
    Sha1Digest decoded_sha1; ///< whole decoded member
};

enum class MemberId : u32 {
    MapWindowBase,   ///< UI/Map2/Texture/T_UI_Map_Window_Base_00.uexp (parchment scrap)
    WindowBase,      ///< UI/Common/Texture/T_UI_Common_Window_Base_00.uexp (card window)
    StatusHp,        ///< UI/Common/Texture/T_UI_Common_Status_HP_00.uexp
    StatusMp,        ///< UI/Common/Texture/T_UI_Common_Status_MP_00.uexp
    StatusGaugeBg,   ///< UI/Common/Texture/T_UI_Common_Status_HP_01.uexp (gauge background, HP and MP)
    StatusIcon,      ///< UI/Common/Texture/T_UI_Common_Status_StatusIcon_00.uexp (4 x 6 cells)
    FontSemibold,    ///< UI/Font/PlantinMTPro-Semibold.ufont (OpenType CFF, "OTTO")
    FontBold,        ///< UI/Font/PlantinMTPro-Bold.ufont
    FontAvenir,      ///< UI/Font/AvenirNextW1G-Demi.ufont (the native HUD's HP / MP labels)
    Count,
};
const PakMember& Member(MemberId id);

/// Reads exactly `size` bytes at `offset` of the archive; false on a short read.
using RangeReader = std::function<bool(u64 offset, void* out, std::size_t size)>;

/// Parses an in-data FPakEntry and checks it against the pin. Empty string = match.
std::string CheckEntryHeader(std::span<const u8> header, const PakMember& pin);

/// Decodes stored member bytes (the concatenated blocks) whose SHA-1 has already been checked.
/// Each Oodle block decodes to min(block_size, remaining). nullopt on any ooz failure.
std::optional<std::vector<u8>> DecodeStored(std::span<const u8> stored, const PakMember& pin);

/// Header check, stored-hash check, decode, decoded-hash check. `why` gets the failure reason.
std::optional<std::vector<u8>> ReadMember(const RangeReader& read, const PakMember& pin,
                                          std::string* why = nullptr);
/// ReadMember of each pin in order; stops at the first failure with `why` = "<path>: <reason>".
std::optional<std::vector<std::vector<u8>>> ReadMembers(const RangeReader& read,
                                                        std::span<const PakMember* const> pins,
                                                        std::string* why = nullptr);

struct Image {
    u32 width{};
    u32 height{};
    std::vector<u8> rgba; ///< straight RGBA8, row-major
};

enum class TextureFormat : u32 { Bc7, Bgra8 };

struct TexturePin {
    u32 width;
    u32 height;
    u32 data_bytes;
    TextureFormat format{TextureFormat::Bc7};
};
/// Pinned first-mip layout of a texture member (nullopt for a non-texture member).
std::optional<TexturePin> TextureLayout(MemberId id);

/// One 4x4 block through bcn::DecodeBc7 / DecodeBc4 (externals/bc_decoder), which read the 16 / 8 block bytes
/// through a struct that needs 8-byte alignment: cooked texture data sits at any offset, so the block is
/// copied to an aligned buffer first (an unaligned one is undefined behaviour). Same arguments otherwise.
void DecodeBc7Block(const u8* block, u8* dst, std::size_t x, std::size_t y, std::size_t width, std::size_t height);
void DecodeBc4Block(const u8* block, u8* dst, std::size_t x, std::size_t y, std::size_t width, std::size_t height,
                    bool is_signed);

/// Inline first mip of a cooked PF_BC7 / PF_B8G8R8A8 .uexp checked against `pin` -> RGBA8.
std::optional<Image> DecodeTexture(std::span<const u8> uexp, const TexturePin& pin,
                                   std::string* why = nullptr);
/// PF_B8G8R8A8 only: the same checks, but only the {x, y, w, h} rectangle is converted (tall
/// monster atlases reach 8212 rows; the 4096 limit applies to the rectangle).
std::optional<Image> DecodeTextureRect(std::span<const u8> uexp, const TexturePin& pin,
                                       const std::array<u32, 4>& rect, std::string* why = nullptr);

} // namespace dq3
