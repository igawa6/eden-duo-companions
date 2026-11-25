// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// See za_assets.h for what this decodes and why it ships nothing.
//
// The layout of every structure below was derived from the game's own files and is pinned by
// offsets only:
//   data.trpfd  FlatBuffer  field 1 = vector<string> pack paths (9,892)
//                           field 3 = vector<table{size:u64, count:u64}> parallel to it
//   data.trpfs  "ONEPACK\0" + u64 index_offset, then a FlatBuffer at index_offset:
//                           field 0 = vector<u64> pack hashes, field 1 = vector<u64> offsets
//   pack key    FNV-1a 64 of the path, *non-standard* offset basis 14695981039346656837
//   trpak       u32 header_size; at header_size+12: vector<u32> relative offsets;
//               each file: u32 unused, u32 comp_type, u32 header_size, u64 uncompressed,
//               then at +20+(header_size-12) a u32 compressed size and the payload
//   comp_type   0x07030000 = Oodle Kraken (the vendored ooz), anything else = stored
//   BNTX        "BNTX"/"NX  ", texture count at 0x24, pointer table at 0x28; each BRTI has
//               mip count at 0x16, format at 0x1C, w/h at 0x24/0x28, array count at 0x30,
//               block height log2 at 0x34, alignment at 0x50, name pointer at 0x60 and the
//               mip offset array at 0x70. Texels are block-linear (Tegra swizzle).
//   names       arc/ik_messagedatFrenchcommonmonsname.tbl.trpak, inner file 0: 1,026
//               GF text strings from 0x202C, key 0xA60C stepping by 0x2983, each u16
//               xored with a key rotated left 3 bits per code unit, 4-byte aligned ends.

#include "za_assets.h"
#include "za_save.h"
#include "za_mega.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "bc_decoder.h"
#include "third_party/ooz/ooz.h"

namespace za {
namespace {

#ifdef ZA_ASSETS_TRACE
#define ZA_TRACE(...) std::fprintf(stderr, "za_assets: " __VA_ARGS__)
#else
#define ZA_TRACE(...) ((void)0)
#endif

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 kFnvBasis = 14695981039346656837ull;
constexpr u64 kFnvPrime = 1099511628211ull;
constexpr const char* kDimensionFiles[]={
    "world/ik_data/field/dimension/dimension_rank/dimension_rank/dimension_rank_array.bin",
    "world/ik_data/field/dimension/dimension_map/dimension_map/dimension_map_array.bin",
    "world/ik_data/field/dimension/dimension_wild_pokemon/dimension_wild_pokemon/dimension_wild_pokemon_array.bin",
    "world/ik_data/field/pokemon_spawner/pokemon_spawner_data/pokemon_spawner_data_array.bin",
    "world/ik_data/field/pokemon/encount_data/encount_data/encount_data_array.bin"};

u64 Fnv1a(const char* text) {
    u64 hash = kFnvBasis;
    for (const char* p = text; *p != '\0'; ++p) {
        hash ^= static_cast<u8>(*p);
        hash *= kFnvPrime;
    }
    return hash;
}

/// The packed little-endian readers. Every caller bounds-checks first; these return 0 past the
/// end so a malformed file fails a later validation instead of reading out of bounds.
struct Buf {
    const u8* data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] bool Has(std::size_t at, std::size_t len) const {
        return data != nullptr && at <= size && len <= size - at;
    }
    [[nodiscard]] u16 U16(std::size_t at) const {
        u16 v = 0;
        if (Has(at, 2)) {
            std::memcpy(&v, data + at, 2);
        }
        return v;
    }
    [[nodiscard]] u32 U32(std::size_t at) const {
        u32 v = 0;
        if (Has(at, 4)) {
            std::memcpy(&v, data + at, 4);
        }
        return v;
    }
    [[nodiscard]] u64 U64(std::size_t at) const {
        u64 v = 0;
        if (Has(at, 8)) {
            std::memcpy(&v, data + at, 8);
        }
        return v;
    }
};

// ---- FlatBuffers (read-only, just the three shapes the archive uses) --------------------------

/// Absolute offset of field `index` of the table at `table`, or 0 when the field is absent.
std::size_t FbField(const Buf& b, std::size_t table, unsigned index) {
    if (!b.Has(table, 4)) {
        return 0;
    }
    // The soffset is signed: a vtable may sit after its table (negative offset), which is the
    // usual case for FlatBuffers' deduplicated vtables.
    const auto back = static_cast<std::int64_t>(static_cast<std::int32_t>(b.U32(table)));
    const auto vt = static_cast<std::int64_t>(table) - back;
    if (vt < 0 || static_cast<u64>(vt) + 4 > b.size) {
        return 0;
    }
    const std::size_t vt_at = static_cast<std::size_t>(vt);
    const std::size_t vt_size = b.U16(vt_at);
    const std::size_t slot = 4 + static_cast<std::size_t>(index) * 2;
    if (slot + 2 > vt_size) {
        return 0;
    }
    const std::size_t off = b.U16(vt_at + slot);
    return off == 0 ? 0 : table + off;
}

std::string FbString(const Buf& b,std::size_t table,unsigned index){
    const auto p=FbField(b,table,index);if(!p||!b.Has(p,4))return {};
    const auto at=p+b.U32(p);if(!b.Has(at,4))return {};const auto n=b.U32(at);
    return n<4096&&b.Has(at+4,n)?std::string(reinterpret_cast<const char*>(b.data+at+4),n):std::string{};
}

/// A vector's element count and the offset of its first element.
bool FbVector(const Buf& b, std::size_t table, unsigned index, std::size_t& first,
              std::size_t& count) {
    const std::size_t field = FbField(b, table, index);
    if (field == 0) {
        return false;
    }
    const std::size_t vec = field + b.U32(field);
    if (!b.Has(vec, 4)) {
        return false;
    }
    count = b.U32(vec);
    first = vec + 4;
    return true;
}

std::vector<Assets::MissionReward> MissionRewards(const Buf& b, std::size_t row) {
    std::vector<Assets::MissionReward> rewards;
    std::size_t first=0,count=0;
    if (!FbVector(b,row,10,first,count) || count>64 || !b.Has(first,count*4)) return rewards;
    for (std::size_t i=0;i<count;++i) {
        const auto slot=first+i*4, entry=slot+b.U32(slot), type_field=FbField(b,entry,0);
        const auto kind=type_field ? b.U32(type_field) : 0;
        if (kind!=1 && kind!=2 && kind!=6) continue;
        std::size_t parameters=0,size=0;
        if (!FbVector(b,entry,1,parameters,size) || size!=(kind==2 ? 2 : 1) ||
            !b.Has(parameters,size*4)) continue;
        const auto number=[&](std::size_t index) {
            const auto at=parameters+index*4, value=at+b.U32(at);
            const auto length=b.U32(value);
            if (!length || length>9 || !b.Has(value+4,length)) return 0;
            const auto* start=reinterpret_cast<const char*>(b.data+value+4);
            int result=0;
            const auto [end,error]=std::from_chars(start,start+length,result);
            return error==std::errc{} && end==start+length && result>0 ? result : 0;
        };
        using Kind=Assets::MissionReward::Kind;
        const int item=kind==2 ? number(0) : 0, amount=number(size-1);
        if (amount && (kind!=2 || item)) {
            rewards.push_back({kind==1 ? Kind::Money : kind==2 ? Kind::Item : Kind::ResearchPoints,
                               item,amount});
        }
    }
    return rewards;
}

bool FbVecU64(const Buf& b, std::size_t table, unsigned index, std::vector<u64>& out) {
    std::size_t first = 0;
    std::size_t count = 0;
    if (!FbVector(b, table, index, first, count) || !b.Has(first, count * 8)) {
        return false;
    }
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = b.U64(first + i * 8);
    }
    return true;
}

bool FbVecString(const Buf& b, std::size_t table, unsigned index, std::vector<std::string>& out) {
    std::size_t first = 0;
    std::size_t count = 0;
    if (!FbVector(b, table, index, first, count) || !b.Has(first, count * 4)) {
        return false;
    }
    out.clear();
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = first + i * 4;
        const std::size_t str = at + b.U32(at);
        const u32 len = b.U32(str);
        if (!b.Has(str + 4, len)) {
            return false;
        }
        out.emplace_back(reinterpret_cast<const char*>(b.data + str + 4), len);
    }
    return true;
}

/// field 3: vector of table{size:u64, count:u64}; only the sizes are needed.
bool FbVecTableSizes(const Buf& b, std::size_t table, unsigned index,
                     std::vector<u64>& sizes) {
    std::size_t first = 0;
    std::size_t count = 0;
    if (!FbVector(b, table, index, first, count) || !b.Has(first, count * 4)) {
        return false;
    }
    sizes.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = first + i * 4;
        const std::size_t entry = at + b.U32(at);
        const std::size_t field = FbField(b, entry, 0);
        sizes[i] = field == 0 ? 0 : b.U64(field);
    }
    return true;
}

// ---- trpak ----------------------------------------------------------------------------------

constexpr u32 kCompKraken = 0x07030000u;

/// One inner file of a trpak, decompressed.
bool UnpackPak(const Buf& pak, std::size_t index, std::vector<u8>& out) {
    const u32 header_size = pak.U32(0);
    if (header_size < 4 || header_size > pak.size) {
        return false;
    }
    const std::size_t base = header_size + 12;
    const u32 count = pak.U32(base);
    if (count == 0 || count > 65536 || !pak.Has(base + 4, u64{count} * 4)) {
        return false;
    }
    if (index >= count) {
        return false;
    }
    const std::size_t rel_at = base + 4 + index * 4;
    const std::size_t file = base + 4 + index * 4 + pak.U32(rel_at);
    if (!pak.Has(file, 20)) {
        return false;
    }
    const u32 comp_type = pak.U32(file + 4);
    const u32 file_header = pak.U32(file + 8);
    const u64 uncompressed = pak.U64(file + 12);
    if (file_header < 12) {
        return false;
    }
    const std::size_t comp_at = file + 20 + (file_header - 12);
    const u32 comp_size = pak.U32(comp_at);
    if (!pak.Has(comp_at + 4, comp_size)) {
        return false;
    }
    const u8* const payload = pak.data + comp_at + 4;
    if (uncompressed == 0 || uncompressed > (64ull << 20)) {
        return false;
    }
    if (comp_type == kCompKraken) {
        // ooz may write up to OOZ_SAFE_SPACE bytes past the destination.
        out.assign(static_cast<std::size_t>(uncompressed) + OOZ_SAFE_SPACE, 0);
        if (ooz_decompress(payload, comp_size, out.data(),
                           static_cast<std::size_t>(uncompressed)) !=
            static_cast<int>(uncompressed)) {
            out.clear();
            return false;
        }
        out.resize(static_cast<std::size_t>(uncompressed));
        return true;
    }
    // Everything else in these packs is stored as is (comp_size == uncompressed).
    out.assign(payload, payload + comp_size);
    if (out.size() != uncompressed) {
        out.resize(std::min<std::size_t>(out.size(), static_cast<std::size_t>(uncompressed)));
    }
    return !out.empty();
}

// ---- SARC (the UI resource archives inside a pack) -------------------------------------------

bool SarcMember(const Buf& sarc, const char* wanted, std::size_t& offset, std::size_t& length) {
    if (!sarc.Has(0, 0x20) || std::memcmp(sarc.data, "SARC", 4) != 0) {
        return false;
    }
    const std::size_t header = sarc.U16(4);
    const std::size_t data_offset = sarc.U32(0x0C);
    if (!sarc.Has(header, 12) || std::memcmp(sarc.data + header, "SFAT", 4) != 0) {
        return false;
    }
    const std::size_t nodes = sarc.U16(header + 6);
    const std::size_t node_at = header + 12;
    if (!sarc.Has(node_at, nodes * 16)) {
        return false;
    }
    const std::size_t name_at = node_at + nodes * 16;
    if (!sarc.Has(name_at, 8) || std::memcmp(sarc.data + name_at, "SFNT", 4) != 0) {
        return false;
    }
    const std::size_t names = name_at + 8;
    for (std::size_t i = 0; i < nodes; ++i) {
        const std::size_t node = node_at + i * 16;
        const u32 attrs = sarc.U32(node + 4);
        const u32 begin = sarc.U32(node + 8);
        const u32 end = sarc.U32(node + 12);
        const std::size_t str = names + (attrs & 0x00FFFFFFu) * 4;
        if (!sarc.Has(str, 1)) {
            continue;
        }
        const char* const name = reinterpret_cast<const char*>(sarc.data + str);
        const std::size_t room = sarc.size - str;
        std::size_t len = 0;
        while (len < room && name[len] != '\0') {
            ++len;
        }
        if (len != std::strlen(wanted) || std::memcmp(name, wanted, len) != 0) {
            continue;
        }
        if (end < begin || data_offset + end > sarc.size) {
            return false;
        }
        offset = data_offset + begin;
        length = end - begin;
        return true;
    }
    return false;
}

// ---- BNTX -----------------------------------------------------------------------------------

u64 BlockLinearOffset(u32 x, u32 y, u32 bpp, u32 block_height, u64 gobs_per_row) {
    const u64 gob = (static_cast<u64>(y / (8 * block_height)) * 512 * block_height * gobs_per_row) +
                    (static_cast<u64>(x) * bpp / 64 * 512 * block_height) +
                    (static_cast<u64>(y % (8 * block_height) / 8) * 512);
    const u64 xb = static_cast<u64>(x) * bpp;
    return gob + ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 +
           (y % 2) * 16 + (xb % 16);
}

enum class Texel { Unsupported, Bc };

struct Format {
    Texel kind{Texel::Unsupported};
    u32 block{4};
    u32 bytes{0}; ///< per block
};

Format Describe(u32 format) {
    switch (format >> 8) {
    case 0x1A:
        return {Texel::Bc, 4, 8}; // BC1
    case 0x1B:
        return {Texel::Bc, 4, 16}; // BC2
    case 0x1C:
        return {Texel::Bc, 4, 16}; // BC3
    case 0x1D:
        return {Texel::Bc, 4, 8}; // BC4
    case 0x1E:
        return {Texel::Bc, 4, 16}; // BC5
    case 0x20:
        return {Texel::Bc, 4, 16}; // BC7
    default:
        return {};
    }
}

struct Texture {
    std::string name;
    u32 format = 0;
    u32 width = 0;
    u32 height = 0;
    u32 array = 1;
    u32 block_height_log2 = 0;
    u32 alignment = 0;
    u64 data = 0; ///< offset of mip 0
};

/// Finds the texture called `wanted` (or, when `wanted` is null, the largest one).
bool FindTexture(const Buf& bntx, const char* wanted, Texture& out) {
    if (!bntx.Has(0, 0x40) || std::memcmp(bntx.data, "BNTX", 4) != 0 ||
        std::memcmp(bntx.data + 0x20, "NX  ", 4) != 0) {
        return false;
    }
    const u32 count = bntx.U32(0x24);
    const u64 table = bntx.U64(0x28);
    if (count == 0 || count > 4096 || !bntx.Has(table, u64{count} * 8)) {
        return false;
    }
    bool found = false;
    u64 best_area = 0;
    for (u32 i = 0; i < count; ++i) {
        const u64 info = bntx.U64(table + u64{i} * 8);
        if (!bntx.Has(info, 0x78) || std::memcmp(bntx.data + info, "BRTI", 4) != 0) {
            continue;
        }
        const u64 name_at = bntx.U64(info + 0x60);
        if (!bntx.Has(name_at, 2)) {
            continue;
        }
        const u32 name_len = bntx.U16(name_at);
        if (!bntx.Has(name_at + 2, name_len)) {
            continue;
        }
        const char* const name = reinterpret_cast<const char*>(bntx.data + name_at + 2);
        // Names carry a channel suffix in some archives ("map_texture_00^o"), so match on the
        // leading part.
        const bool named = wanted != nullptr && name_len >= std::strlen(wanted) &&
                           std::memcmp(name, wanted, std::strlen(wanted)) == 0;
        const u64 area = u64{bntx.U32(info + 0x24)} * bntx.U32(info + 0x28);
        if (wanted == nullptr ? (found && area <= best_area) : !named) {
            continue;
        }
        const u64 mips = bntx.U64(info + 0x70);
        if (!bntx.Has(mips, 8)) {
            continue;
        }
        Texture t;
        t.name.assign(name, name_len);
        t.format = bntx.U32(info + 0x1C);
        t.width = bntx.U32(info + 0x24);
        t.height = bntx.U32(info + 0x28);
        t.array = std::max<u32>(1, bntx.U32(info + 0x30));
        t.block_height_log2 = bntx.U32(info + 0x34) & 7;
        t.alignment = bntx.U32(info + 0x50);
        t.data = bntx.U64(mips);
        if (t.width == 0 || t.height == 0 || t.width > 8192 || t.height > 8192 ||
            t.data >= bntx.size) {
            continue;
        }
        out = t;
        found = true;
        best_area = area;
        if (wanted != nullptr) {
            return true;
        }
    }
    return found;
}

/// Unswizzles a texture's mip 0 and decodes it. With `average_blocks` the four-by-four block is
/// averaged into one pixel, which turns a 4096 map into a 1024 one without aliasing (the map page
/// then box-filters that down again).
bool DecodeTexture(const Buf& bntx, const Texture& t, bool average_blocks, Picture& out) {
    const Format f = Describe(t.format);
    if (f.kind != Texel::Bc) {
        return false;
    }
    const u32 grid_w = (t.width + f.block - 1) / f.block;
    const u32 grid_h = (t.height + f.block - 1) / f.block;
    const u32 block_height = 1u << t.block_height_log2;
    const u64 gobs_per_row = (u64{grid_w} * f.bytes + 63) / 64;
    const u64 group_rows = (grid_h + 8u * block_height - 1) / (8u * block_height);
    const u64 layer_bytes = group_rows * 512 * block_height * gobs_per_row;
    const u64 available = std::min<u64>(layer_bytes, bntx.size - t.data);
    if (available < u64{grid_w} * grid_h * f.bytes) {
        return false;
    }
    const Buf tiled{bntx.data + t.data, static_cast<std::size_t>(available)};

    const u32 out_w = average_blocks ? grid_w : t.width;
    const u32 out_h = average_blocks ? grid_h : t.height;
    out.width = out_w;
    out.height = out_h;
    out.rgba.assign(static_cast<std::size_t>(out_w) * out_h * 4, 0);

    const u32 type = t.format >> 8;
    const bool is_signed = (t.format & 0xFF) == 2;
    // BC4/BC5 decode to R8/R8G8; widen to RGBA after.
    const u32 out_bpp = type == 0x1D ? 1 : type == 0x1E ? 2 : 4;
    std::vector<u8> narrow;
    std::vector<u8> block;
    if (average_blocks) {
        narrow.assign(4 * 4 * out_bpp, 0);
    } else if (out_bpp != 4) {
        narrow.assign(static_cast<std::size_t>(out_w) * out_h * out_bpp, 0);
    }

    for (u32 by = 0; by < grid_h; ++by) {
        for (u32 bx = 0; bx < grid_w; ++bx) {
            const u64 at = BlockLinearOffset(bx, by, f.bytes, block_height, gobs_per_row);
            if (at + f.bytes > tiled.size) {
                continue;
            }
            const u8* const src = tiled.data + at;
            u8* target = nullptr;
            u32 px = 0;
            u32 py = 0;
            if (average_blocks) {
                target = narrow.data();
                px = 0;
                py = 0;
            } else {
                px = bx * 4;
                py = by * 4;
                target = out_bpp == 4 ? out.rgba.data() : narrow.data();
                target += (u64{py} * out_w + px) * out_bpp;
            }
            switch (type) {
            case 0x1A:
                bcn::DecodeBc1(src, target, px, py, average_blocks ? 4 : out_w,
                               average_blocks ? 4 : out_h);
                break;
            case 0x1B:
                bcn::DecodeBc2(src, target, px, py, average_blocks ? 4 : out_w,
                               average_blocks ? 4 : out_h);
                break;
            case 0x1C:
                bcn::DecodeBc3(src, target, px, py, average_blocks ? 4 : out_w,
                               average_blocks ? 4 : out_h);
                break;
            case 0x1D:
                bcn::DecodeBc4(src, target, px, py, average_blocks ? 4 : out_w,
                               average_blocks ? 4 : out_h, is_signed);
                break;
            case 0x1E:
                bcn::DecodeBc5(src, target, px, py, average_blocks ? 4 : out_w,
                               average_blocks ? 4 : out_h, is_signed);
                break;
            case 0x20:
                bcn::DecodeBc7(src, target, px, py, average_blocks ? 4 : out_w,
                               average_blocks ? 4 : out_h);
                break;
            default:
                break;
            }
            if (!average_blocks && out_bpp == 4) {
                continue;
            }
            if (average_blocks) {
                u32 sum[4] = {0, 0, 0, 0};
                for (u32 i = 0; i < 16; ++i) {
                    for (u32 c = 0; c < out_bpp; ++c) {
                        sum[c] += narrow[i * out_bpp + c];
                    }
                }
                u8* const d = out.rgba.data() + (u64{by} * out_w + bx) * 4;
                if (out_bpp == 4) {
                    for (u32 c = 0; c < 4; ++c) {
                        d[c] = static_cast<u8>(sum[c] / 16);
                    }
                } else {
                    d[0] = static_cast<u8>(sum[0] / 16);
                    d[1] = out_bpp == 2 ? static_cast<u8>(sum[1] / 16) : 0;
                    d[2] = 0;
                    d[3] = 255;
                }
            }
        }
    }
    if (!average_blocks && out_bpp != 4) {
        const u64 pixels = u64{out_w} * out_h;
        for (u64 i = 0; i < pixels; ++i) {
            out.rgba[i * 4] = narrow[i * out_bpp];
            out.rgba[i * 4 + 1] = out_bpp == 2 ? narrow[i * 2 + 1] : 0;
            out.rgba[i * 4 + 2] = 0;
            out.rgba[i * 4 + 3] = 255;
        }
    }
    return true;
}

/// Area-average downscale (the map page draws the result 1:1, so this is the only resampling
/// the picture ever gets).
void BoxFilter(const Picture& src, unsigned size, Picture& out) {
    if (!src.Valid() || size == 0) {
        return;
    }
    out.width = size;
    out.height = size;
    out.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
    for (unsigned y = 0; y < size; ++y) {
        const unsigned y0 = static_cast<unsigned>(u64{y} * src.height / size);
        unsigned y1 = static_cast<unsigned>((u64{y + 1} * src.height + size - 1) / size);
        y1 = std::min(src.height, std::max(y1, y0 + 1));
        for (unsigned x = 0; x < size; ++x) {
            const unsigned x0 = static_cast<unsigned>(u64{x} * src.width / size);
            unsigned x1 = static_cast<unsigned>((u64{x + 1} * src.width + size - 1) / size);
            x1 = std::min(src.width, std::max(x1, x0 + 1));
            u32 sum[4] = {0, 0, 0, 0};
            u32 n = 0;
            for (unsigned sy = y0; sy < y1; ++sy) {
                const u8* const row = src.rgba.data() + (u64{sy} * src.width + x0) * 4;
                for (unsigned sx = 0; sx < x1 - x0; ++sx) {
                    for (u32 c = 0; c < 4; ++c) {
                        sum[c] += row[sx * 4 + c];
                    }
                    ++n;
                }
            }
            u8* const d = out.rgba.data() + (u64{y} * size + x) * 4;
            for (u32 c = 0; c < 4; ++c) {
                d[c] = n == 0 ? 0 : static_cast<u8>(sum[c] / n);
            }
        }
    }
}

// ---- text -----------------------------------------------------------------------------------

void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string UiKey(const std::string& text){
    std::string result;
    for(std::size_t i=0;i<text.size();++i){
        auto c=static_cast<u8>(text[i]);
        if(c==' '||c=='\n'||c=='\r'||c=='\t'){if(!result.empty()&&result.back()!=' ')result+=' ';continue;}
        if(c>='a'&&c<='z')c-=32;
        if(c==0xC3&&i+1<text.size()){auto tail=static_cast<u8>(text[++i]);if(tail>=0xA0&&tail<=0xBE&&tail!=0xB7)tail-=32;result+=char(c);result+=char(tail);}
        else result+=char(c);
    }
    while(!result.empty()&&(result.back()==' '||result.back()==':'||result.back()=='.'||result.back()=='!'))result.pop_back();
    return result;
}

u16 Rotl16(u16 value, unsigned bits) {
    bits &= 15;
    return static_cast<u16>((value << bits) | (value >> (16 - bits)));
}

/// GF UTF-16 message table, shared by all ten languages. Formatting/substitution tags
/// are skipped as complete records; their arguments are not Unicode text.
void DecodeTextTable(const Buf& dat, std::vector<std::string>& out) {
    out.clear();
    if (!dat.Has(0, 0x14)) return;
    const auto count = dat.U16(2);
    if (count == 0 || count > 16384 || !dat.Has(0x14, std::size_t(count) * 8)) return;
    out.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        const auto entry = 0x14 + std::size_t(i) * 8;
        const auto at = std::size_t(dat.U32(entry)) + 0x10;
        const auto length = dat.U16(entry + 4);
        if (length > 4096 || !dat.Has(at, std::size_t(length) * 2)) { out.clear(); return; }
        u16 key = static_cast<u16>(0x7C89 + i * 0x2983);
        std::string text;
        for (unsigned c = 0; c < length; ++c) {
            const u16 cp = dat.U16(at + c * 2) ^ key;
            key = Rotl16(key, 3);
            // GF message tag: marker, payload-word count, then the command and arguments.
            // Zero-valued arguments are data, not string terminators. Never print them as text.
            if(cp==0x10&&c+1<length){
                const auto words=dat.U16(at+(c+1)*2)^key;
                if(words>64||c+1+words>=length)break;
                for(unsigned skip=0;skip<=words;++skip)key=Rotl16(key,3);
                c+=words+1;continue;
            }
            if (!cp) break;
            if (cp == 0x2640) text += 'F';
            else if (cp == 0x2642) text += 'M';
            else if (cp == '\n' || cp == '\r') text += ' ';
            else if(cp==0x202F||cp==0xA0)text+=' ';
            else if(cp==0x1D49)text+='e'; // French ordinal suffix, equivalent baseline glyph
            else if (cp == 0x2019) text += "'";
            else if (cp >= 0x20 && cp < 0xD800 && !(cp>=0x7F&&cp<=0x9F)) AppendUtf8(text, cp);
            else if (cp >= 0xE000 && cp < 0xF000) continue; // game substitution/control tokens
            else if (cp >= 0xF900 && cp < 0xFFFE && cp!=0xFF00) AppendUtf8(text, cp);
        }
        out.push_back(std::move(text));
    }
}

void DecodeNames(const Buf& dat, std::vector<std::string>& out) {
    DecodeTextTable(dat, out);
    if (!out.empty()) out.erase(out.begin()); // index zero is the dummy species
}

/// The icon packs all follow one fixed-length name, so the species and form are read straight out
/// of the path instead of probing four names per species:
///   arc/ik_pokemondata pm0025 pm0025 _00 _00 icon pm0025 _00 _00 _00 _0 .bntx.trpak
///                      ^20    ^24    ^31
constexpr const char* kIconPrefix = "arc/ik_pokemondata";
constexpr std::size_t kIconPrefixLen = 18;
constexpr std::size_t kIconNameLen = 68;

bool ParseIconPath(const std::string& path, int& species, int& form) {
    if (path.size() != kIconNameLen || std::memcmp(path.data(), kIconPrefix, kIconPrefixLen) != 0 ||
        std::memcmp(path.data() + kIconPrefixLen, "pm", 2) != 0 ||
        std::memcmp(path.data() + kIconPrefixLen + 6, "pm", 2) != 0 || path[30] != '_' ||
        path[33] != '_') {
        return false;
    }
    const auto digits = [&path](std::size_t at) {
        int value = 0;
        for (std::size_t i = at; i < at + 4; ++i) {
            if (path[i] < '0' || path[i] > '9') {
                return -1;
            }
            value = value * 10 + (path[i] - '0');
        }
        return value;
    };
    const int id = digits(kIconPrefixLen + 2);
    const int second = digits(kIconPrefixLen + 8);
    if (id < 0 || id != second || path[31] < '0' || path[31] > '9' || path[32] < '0' ||
        path[32] > '9') {
        return false;
    }
    species = id;
    form = (path[31] - '0') * 10 + (path[32] - '0');
    return true;
}

} // namespace

// ---- Assets ---------------------------------------------------------------------------------

void Assets::SetReader(void* userdata, ReadFn read) {
    std::scoped_lock lock{mutex_};
    userdata_ = userdata;
    read_ = read;
    archive_root_.clear();
    item_names_.clear();
    item_usage_.clear();
    move_names_.clear(); move_descriptions_.clear(); species_descriptions_.clear();
    item_index_tried_ = false; item_pack_.clear(); item_indices_.clear();
    item_icons_.clear(); item_icon_order_.clear();
    retry_at_ = {};
    tried_ = false;
    ready_ = false;
    packs_.clear();
    research_levels_.clear();trainer_ranks_.clear();research_tried_=false;research_pack_path_.clear();
    icon_packs_.clear();
    names_.clear();
    evolutions_.clear();
    evolutions_by_item_.clear();
    icons_.clear();
    icon_order_.clear();
    maps_.clear();
    money_icon_.Clear();
}

bool Assets::ReadRomfs(const char* path, std::uint64_t offset, void* out, std::size_t size) {
    const auto full = archive_root_ + path;
    return read_ != nullptr && read_(userdata_, full.c_str(), offset, out, size) == size;
}

std::uint64_t Assets::RomfsSize(const char* path) {
    const auto full = archive_root_ + path;
    return read_ == nullptr ? 0 : read_(userdata_, full.c_str(), 0, nullptr, 0);
}

bool Assets::Fetch(const char* path, std::vector<std::uint8_t>& out) {
    const u64 hash = Fnv1a(path);
    const auto found = packs_.find(hash);
    if (found == packs_.end()) {
        return false;
    }
    const PackRef& pack = found->second;
    if (pack.size == 0 || pack.size > (16u << 20)) {
        return false;
    }
    out.assign(pack.size, 0);
    // The pack's bytes live in the single data.trpfs container, at the offset the index gave.
    if (!ReadRomfs("data.trpfs", pack.offset, out.data(), out.size())) {
        ZA_TRACE("read %s @ %llu + %u failed\n", path,
                 static_cast<unsigned long long>(pack.offset), pack.size);
        out.clear();
        return false;
    }
    return true;
}

bool Assets::SetLanguage(int language){
    if(language<1||language>11||language==6)language=2;
    std::scoped_lock lock{mutex_};if(language_==language)return false;
    language_=language;ready_=false;tried_=false;retry_at_={};ui_translations_.clear();
    names_.clear();item_names_.clear();item_descriptions_.clear();move_names_.clear();move_descriptions_.clear();species_descriptions_.clear();type_names_.clear();nature_names_.clear();
    for(auto& quests:missions_)quests.clear();
    return true;
}

bool Assets::FetchInner(const char* path, std::size_t index, std::vector<std::uint8_t>& out) {
    std::string localized=path;
    if(const auto pos=localized.find("ik_messagedatFrench");pos!=std::string::npos){
        static const char* languages[]={"English","JPN_KANJI","English","French","Italian","German","English","Spanish","Korean","Simp_Chinese","Trad_Chinese","LATAM"};
        localized.replace(pos+13,6,languages[language_]);
        path=localized.c_str();
    }
    std::vector<std::uint8_t> pak;
    if (!Fetch(path, pak)) {
        if(const auto pos=localized.find(".tbl.trpak");pos!=std::string::npos){localized.replace(pos,10,".dat.trpak");if(Fetch(localized.c_str(),pak))return UnpackPak(Buf{pak.data(),pak.size()},index,out);}
        ZA_TRACE("fetch failed: %s\n", path);
        return false;
    }
    const bool ok = UnpackPak(Buf{pak.data(), pak.size()}, index, out);
    if (!ok) {
        ZA_TRACE("unpack failed: %s [%zu] (pack %zu bytes)\n", path, index, pak.size());
    }
    return ok;
}

bool Assets::FetchMessage(const char* path,std::vector<u8>& text,std::vector<u8>* labels){
    std::vector<u8> first;if(!FetchInner(path,0,first))return false;
    const bool index=first.size()>=4&&!std::memcmp(first.data(),"AHTB",4);
    if(index){if(labels)*labels=std::move(first);return FetchInner(path,1,text);}
    text=std::move(first);return !labels||FetchInner(path,1,*labels);
}

bool Assets::LoadLocked() {
    tried_ = true;
    retry_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    packs_.clear(); icon_packs_.clear(); names_.clear(); item_names_.clear(); item_descriptions_.clear();pokedex_.clear();mega_forms_.clear();
    archive_root_.clear();
    if (read_) for (const char* root : {"arc/", ""}) {
        const auto fd = std::string(root) + "data.trpfd";
        const auto fs = std::string(root) + "data.trpfs";
        if (read_(userdata_, fd.c_str(), 0, nullptr, 0) >= 0x100 &&
            read_(userdata_, fs.c_str(), 0, nullptr, 0) >= 0x1000) {
            archive_root_ = root; break;
        }
    }
    constexpr const char* kTrpfd = "data.trpfd";
    constexpr const char* kTrpfs = "data.trpfs";

    const u64 trpfd_size = RomfsSize(kTrpfd);
    const u64 trpfs_size = RomfsSize(kTrpfs);
    ZA_TRACE("trpfd %llu bytes, trpfs %llu bytes\n", static_cast<unsigned long long>(trpfd_size),
             static_cast<unsigned long long>(trpfs_size));
    if (trpfd_size < 0x100 || trpfd_size > (32u << 20) || trpfs_size < 0x1000) {
        ZA_TRACE("archive sizes rejected\n");
        return false;
    }
    std::vector<std::uint8_t> trpfd(static_cast<std::size_t>(trpfd_size));
    if (!ReadRomfs(kTrpfd, 0, trpfd.data(), trpfd.size())) {
        return false;
    }
    const Buf fd{trpfd.data(), trpfd.size()};
    const std::size_t fd_root = fd.U32(0);
    std::vector<std::string> paths;
    std::vector<u64> sizes;
    if (!FbVecString(fd, fd_root, 1, paths) || !FbVecTableSizes(fd, fd_root, 3, sizes) ||
        paths.size() != sizes.size() || paths.empty()) {
        return false;
    }

    // The container's index lives at its very end (about 155 KB in a 3.3 GB file).
    std::uint8_t header[16] = {};
    if (!ReadRomfs(kTrpfs, 0, header, sizeof(header)) ||
        std::memcmp(header, "ONEPACK\0", 8) != 0) {
        return false;
    }
    u64 index_at = 0;
    std::memcpy(&index_at, header + 8, 8);
    if (index_at >= trpfs_size || trpfs_size - index_at > (16u << 20)) {
        return false;
    }
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(trpfs_size - index_at));
    if (!ReadRomfs(kTrpfs, index_at, tail.data(), tail.size())) {
        return false;
    }
    const Buf fs{tail.data(), tail.size()};
    const std::size_t fs_root = fs.U32(0);
    std::vector<u64> hashes;
    std::vector<u64> offsets;
    if (!FbVecU64(fs, fs_root, 0, hashes) || !FbVecU64(fs, fs_root, 1, offsets) ||
        hashes.size() != offsets.size() || hashes.empty()) {
        return false;
    }
    std::unordered_map<u64, std::size_t> by_hash;
    by_hash.reserve(hashes.size() * 2);
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        by_hash.emplace(hashes[i], i);
    }
    packs_.reserve(paths.size() * 2);
    std::size_t hash_hits = 0;
    std::size_t size_rejects = 0;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const u64 hash = Fnv1a(paths[i].c_str());
        const auto it = by_hash.find(hash);
        if (it == by_hash.end()) {
            continue;
        }
        ++hash_hits;
        const u64 size = sizes[i];
        if (size == 0 || size > 0xFFFFFFFFull) {
            ++size_rejects;
            continue;
        }
        packs_.emplace(hash, PackRef{offsets[it->second], static_cast<u32>(size)});
        int icon_species = 0;
        int icon_form = 0;
        if (ParseIconPath(paths[i], icon_species, icon_form)) {
            icon_packs_[icon_species].push_back(IconRef{icon_form, paths[i]});
        }
    }
    ZA_TRACE("hash hits %zu, size rejects %zu\n", hash_hits, size_rejects);
    ZA_TRACE("sizes[0..5] = %llu %llu %llu %llu %llu %llu\n",
             static_cast<unsigned long long>(sizes.size() > 0 ? sizes[0] : 0),
             static_cast<unsigned long long>(sizes.size() > 1 ? sizes[1] : 0),
             static_cast<unsigned long long>(sizes.size() > 2 ? sizes[2] : 0),
             static_cast<unsigned long long>(sizes.size() > 3 ? sizes[3] : 0),
             static_cast<unsigned long long>(sizes.size() > 4 ? sizes[4] : 0),
             static_cast<unsigned long long>(sizes.size() > 5 ? sizes[5] : 0));
    if (packs_.empty()) {
        ZA_TRACE("no pack matched (%zu paths, %zu hashes)\n", paths.size(), hashes.size());
        return false;
    }
    // Updates may regroup logical files into a different physical pack.
    research_pack_path_.clear();std::vector<u64> logical_hashes;
    dimension_pack_paths_={};dimensions_tried_=false;dimension_levels_={};dimension_sizes_.clear();dimension_pokemon_.clear();
    std::size_t logical_rows=0,logical_count=0;
    if(FbVecU64(fd,fd_root,0,logical_hashes)&&FbVector(fd,fd_root,2,logical_rows,logical_count)&&
       logical_count==logical_hashes.size()&&fd.Has(logical_rows,logical_count*4)){
        const auto found=std::find(logical_hashes.begin(),logical_hashes.end(),Fnv1a("ik_event/bin/research_lv/research_lv.bin"));
        if(found!=logical_hashes.end()){
            const auto at=logical_rows+std::size_t(found-logical_hashes.begin())*4,row=at+fd.U32(at),owner=FbField(fd,row,0);
            const auto index=owner?fd.U32(owner):0;
            if(index<paths.size())research_pack_path_=paths[index];
        }
        for(int i=0;i<5;++i){
            const auto found=std::find(logical_hashes.begin(),logical_hashes.end(),Fnv1a(kDimensionFiles[i]));
            if(found==logical_hashes.end())continue;
            const auto at=logical_rows+std::size_t(found-logical_hashes.begin())*4,row=at+fd.U32(at),owner=FbField(fd,row,0);
            const auto index=owner?fd.U32(owner):0;
            if(index<paths.size())dimension_pack_paths_[i]=paths[index];
        }
    }
    ZA_TRACE("index: %zu paths, %zu packs\n", paths.size(), packs_.size());

    // Model numbers are not species numbers (e.g. Kalos models use a separate ordering).
    // The game's catalogue maps its species/form key to an exact model path, including Mega
    // forms and Furfrou trims. Resolve that table at runtime instead of guessing numeric offsets.
    auto model_icons = std::move(icon_packs_);
    icon_packs_.clear();
    std::vector<u8> catalogue;
    if (FetchInner("arc/ik_pokemoncatalogcatalogpoke_resource_table.trpmcatalog.trpak", 0,
                   catalogue)) {
        const Buf cat{catalogue.data(), catalogue.size()};
        std::size_t first = 0, count = 0;
        if (FbVector(cat, cat.U32(0), 1, first, count) && count <= 65536 &&
            cat.Has(first, count * 4)) {
            for (std::size_t i = 0; i < count; ++i) {
                const auto slot = first + i * 4;
                const auto row = slot + cat.U32(slot);
                const auto key_field = FbField(cat, row, 0);
                const auto path_field = FbField(cat, row, 1);
                if (!key_field || !path_field) continue;
                const auto key = key_field + cat.U32(key_field);
                const auto species_field = FbField(cat, key, 0);
                const auto form_field = FbField(cat, key, 1);
                const int species = species_field ? cat.U16(species_field) : 0;
                const int form = form_field ? cat.U16(form_field) : 0;
                const auto str = path_field + cat.U32(path_field);
                const auto length = cat.U32(str);
                if (species < 1 || species > 1025 || form > 255 ||
                    !cat.Has(str + 4, length) || length > 512) continue;
                const std::string model_path(reinterpret_cast<const char*>(cat.data + str + 4), length);
                int model = 0, repeated_model = 0, model_form = 0, gender = 0;
                if (std::sscanf(model_path.c_str(), "pm%d/pm%d_%d_%d/", &model, &repeated_model,
                                &model_form, &gender) != 4 || model != repeated_model) continue;
                if(IsMegaForm(NationalSpecies(species),form)){
                    if(std::none_of(mega_forms_.begin(),mega_forms_.end(),[&](const auto& e){return e.species==species&&e.form==form;}))mega_forms_.push_back({species,form,0});
                }
                const auto found = model_icons.find(model);
                if (found == model_icons.end()) continue;
                for (const auto& icon : found->second) {
                    if (icon.form == model_form) {
                        icon_packs_[species].push_back(IconRef{form, icon.path});
                        break;
                    }
                }
            }
        }
    }
    ZA_TRACE("catalogue: icons for %zu species\n", icon_packs_.size());

    std::vector<std::uint8_t> names;
    if (FetchInner("arc/ik_messagedatFrenchcommonmonsname.tbl.trpak", 0, names)) {
        if(names.size()>=4&&!std::memcmp(names.data(),"AHTB",4))FetchInner("arc/ik_messagedatFrenchcommonmonsname.tbl.trpak",1,names);
        DecodeNames(Buf{names.data(), names.size()}, names_);
        ZA_TRACE("names: %zu bytes -> %zu entries, first '%s'\n", names.size(), names_.size(),
                 names_.empty() ? "" : names_.front().c_str());
    } else {
        ZA_TRACE("names pack failed\n");
    }
    std::vector<u8> item_text;
    if (FetchInner("arc/ik_messagedatFrenchcommonitemname.tbl.trpak", 0, item_text)){
        if(item_text.size()>=4&&!std::memcmp(item_text.data(),"AHTB",4))FetchInner("arc/ik_messagedatFrenchcommonitemname.tbl.trpak",1,item_text);
        DecodeTextTable(Buf{item_text.data(), item_text.size()}, item_names_);
    }
    const auto text_table = [&](const char* path, std::vector<std::string>& target) {
        std::vector<u8> bytes;
        if (FetchInner(path, 0, bytes)) {
            if(bytes.size()>=4&&!std::memcmp(bytes.data(),"AHTB",4)&&!FetchInner(path,1,bytes))return;
            DecodeTextTable(Buf{bytes.data(), bytes.size()}, target);
        }
    };
    // Avalon pack: personal array, eight growth curves, move array, ability array.
    learnsets_.clear();machine_moves_.clear();move_info_.clear();evolutions_.clear();
    std::vector<u8> stats;
    if(FetchInner("arc/avalondatatokusei_array.bin.trpak",0,stats)){
        Buf b{stats.data(),stats.size()};std::size_t first=0,count=0;
        std::vector<Evolution> evolutions;
        std::unordered_map<int,bool> available_forms;
        if(FbVector(b,b.U32(0),0,first,count)&&count<4096&&b.Has(first,count*4))for(std::size_t i=0;i<count;++i){
            const auto at=first+i*4,row=at+b.U32(at),key=FbField(b,row,0);
            if(!key||!b.Has(key,4))continue;
            const auto t1=FbField(b,row,3),t2=FbField(b,row,4),growth=FbField(b,row,8);
            Personal info{t1?b.data[t1]:0,t2?b.data[t2]:0,growth?b.data[growth]:0};
            if(info.type1<18&&info.type2<18&&info.growth<8)personal_[b.U16(key)*256+b.U16(key+2)]=info;
            const int species_key=b.U16(key)*256+b.U16(key+2);
            std::size_t moves_at=0,moves_count=0;
            if(FbVector(b,row,25,moves_at,moves_count)&&moves_count<=128&&b.Has(moves_at,moves_count*4))
                for(std::size_t j=0;j<moves_count;++j){const auto at=moves_at+j*4;
                    const int move=b.U16(at),level=b.data[at+2];
                    if(move>0&&move<2048)learnsets_[species_key].push_back({move,level});}
            if(FbVector(b,row,22,moves_at,moves_count)&&moves_count<=256&&b.Has(moves_at,moves_count*2))
                for(std::size_t j=0;j<moves_count;++j){const int move=b.U16(moves_at+j*2);
                    if(move>0&&move<2048)machine_moves_[species_key].push_back(move);}
            const auto number_field=FbField(b,row,2),present=FbField(b,row,1);
            const int number=number_field?b.U16(number_field):0,form=b.U16(key+2);
            if(present&&b.data[present]) {
                available_forms.emplace(species_key,true);
                std::size_t start=0,size=0;
                if(FbVector(b,row,21,start,size)&&size<=32&&b.Has(start,size*16)) {
                    for(std::size_t j=0;j<size;++j) {
                        const auto evo=start+j*16;
                        evolutions.push_back({b.U16(key),form,b.U16(evo+12),b.U16(evo+14),
                                              b.U16(evo),b.U16(evo+2),b.U16(evo+4)});
                    }
                }
            }
            if(number>0&&number<1026&&present&&b.data[present]){
                auto found=std::find_if(pokedex_.begin(),pokedex_.end(),[&](const auto& e){return e.number==number;});
                if(found==pokedex_.end())pokedex_.push_back({b.U16(key),form,number});
                else if(form<found->form)*found={b.U16(key),form,number};
            }
        }
        for(const auto& evolution:evolutions) {
            const auto target=evolution.target*256+evolution.target_form;
            if(evolution.target>0&&evolution.target_form<256&&available_forms.contains(target)) {
                const auto source=evolution.source*256+evolution.source_form;
                evolutions_[source].push_back(evolution);
                if(source!=target) evolutions_[target].push_back(evolution);
            }
        }
    }
    if(FetchInner("arc/avalondatatokusei_array.bin.trpak",1,stats)){
        Buf b{stats.data(),stats.size()};const auto first=FbField(b,b.U32(0),0);
        if(first&&b.Has(first,8*101*4)){experience_.resize(8*101);for(std::size_t i=0;i<experience_.size();++i)experience_[i]=b.U32(first+i*4);}
    }
    if(FetchInner("arc/avalondatatokusei_array.bin.trpak",2,stats)){
        Buf b{stats.data(),stats.size()};std::size_t first=0,count=0;
        if(FbVector(b,b.U32(0),0,first,count)&&count<2048&&b.Has(first,count*4)){
            move_types_.assign(count,-1);
            move_info_.assign(count,{});
            for(std::size_t i=0;i<count;++i){const auto at=first+i*4,row=at+b.U32(at),id=FbField(b,row,0),type=FbField(b,row,2);auto index=id?b.U16(id):0;if(index<count){
                move_types_[index]=type?b.data[type]:0;
                const auto category=FbField(b,row,4),power=FbField(b,row,5),accuracy=FbField(b,row,6);
                move_info_[index]={category?b.data[category]:0,power?b.data[power]:0,accuracy?b.data[accuracy]:0};}}
        }
    }
    mega_forms_.erase(std::remove_if(mega_forms_.begin(),mega_forms_.end(),[&](const auto& e){return !personal_.contains(e.species*256+e.form)||std::none_of(pokedex_.begin(),pokedex_.end(),[&](const auto& p){return p.species==e.species;});}),mega_forms_.end());
    std::sort(mega_forms_.begin(),mega_forms_.end(),[](const auto& a,const auto& b){return a.species==b.species?a.form<b.form:NationalSpecies(a.species)<NationalSpecies(b.species);});
    for(int i=0;i<int(mega_forms_.size());++i)mega_forms_[i].number=i+1;
    std::sort(pokedex_.begin(),pokedex_.end(),[](const auto& a,const auto& b){return a.number<b.number;});
    text_table("arc/ik_messagedatFrenchcommoniteminfo.tbl.trpak", item_descriptions_);
    text_table("arc/ik_messagedatFrenchcommonwazaname.tbl.trpak", move_names_);
    text_table("arc/ik_messagedatFrenchcommontypename.tbl.trpak", type_names_);
    text_table("arc/ik_messagedatFrenchcommonseikaku.tbl.trpak", nature_names_);
    text_table("arc/ik_messagedatFrenchcommonwazainfo.tbl.trpak", move_descriptions_);
    text_table("arc/ik_messagedatFrenchcommonzukan_comment_A.tbl.trpak", species_descriptions_);
    ready_ = names_.size() >= 1025 && !names_[24].empty();
    std::unordered_map<std::string,std::string> extra_text;
    const char* quest_packs[]={"arc/ik_messagedatFrenchcommonquestlist_main.tbl.trpak","arc/ik_messagedatFrenchcommonquestlist_mj.tbl.trpak","arc/ik_messagedatFrenchcommonquestlist_sub.tbl.trpak"};
    for(int category=0;category<3;++category){
        std::vector<std::uint8_t> text,labels;
        if(!FetchMessage(quest_packs[category],text,&labels))continue;
        std::vector<std::string> strings;DecodeTextTable(Buf{text.data(),text.size()},strings);
        Buf index{labels.data(),labels.size()};if(!index.Has(0,8)||std::memcmp(labels.data(),"AHTB",4))continue;
        std::vector<std::string> keys;std::size_t at=8;
        for(unsigned i=0;i<index.U32(4)&&i<16384;++i){if(!index.Has(at,10))break;auto length=index.U16(at+8);at+=10;if(!length||!index.Has(at,length))break;keys.emplace_back(reinterpret_cast<const char*>(labels.data()+at),length-1);at+=length;}
        if(keys.size()<strings.size())continue;
        missions_[category].clear();
        if(category==1)for(std::size_t i=0;i<strings.size()&&i<keys.size();++i)extra_text[keys[i]]=strings[i];
        for(std::size_t i=0;i<strings.size();++i){const auto pos=keys[i].find("quest_title_");
            if(pos==std::string::npos||strings[i].empty()||keys[i].ends_with("01000000"))continue;
            Mission mission{keys[i],strings[i],{}};
            mission.category=category;
            auto outline=keys[i];outline.replace(pos,12,"quest_outline_");
            auto found=std::find(keys.begin(),keys.end(),outline);
            if(found!=keys.end()&&std::size_t(found-keys.begin())<strings.size())mission.description=strings[found-keys.begin()];
            auto purpose=keys[i];purpose.replace(pos,12,"quest_purpose_");
            for(std::size_t goal=0;goal<strings.size();++goal)if(keys[goal].starts_with(purpose+"_")&&!strings[goal].empty()){
                if(!mission.objectives.empty())mission.objectives+="\n";
                mission.objectives+="- "+strings[goal];
            }
            missions_[category].push_back(std::move(mission));
        }
    }
    // Resolve a logical file through trpfd's file-hash -> pack table, then its inner hash.
    const auto logical=[&](const char* path,std::vector<u8>& output){
        std::vector<u64> hashes;std::size_t first=0,count=0;
        if(!FbVecU64(fd,fd_root,0,hashes)||!FbVector(fd,fd_root,2,first,count))return false;
        const auto wanted=Fnv1a(path);const auto found=std::find(hashes.begin(),hashes.end(),wanted);
        if(found==hashes.end()||std::size_t(found-hashes.begin())>=count)return false;
        const auto at=first+std::size_t(found-hashes.begin())*4,row=at+fd.U32(at),field=FbField(fd,row,0);
        if(!field||fd.U32(field)>=paths.size())return false;
        std::vector<u8> pack;if(!Fetch(paths[fd.U32(field)].c_str(),pack))return false;
        Buf p{pack.data(),pack.size()};const auto header=p.U32(0),table=header+4+p.U32(header+4);
        if(!p.Has(table,4))return false;const auto n=p.U32(table);if(n>4096||!p.Has(table+4,n*8))return false;
        for(unsigned i=0;i<n;++i)if(p.U64(table+4+i*8)==wanted)return UnpackPak(p,i,output);
        return false;
    };
    item_usage_.clear();
    std::vector<u8> usage;
    if(logical("world/exl/item_data/item_data/item_data.bin",usage)){
        const Buf b{usage.data(),usage.size()};
        std::size_t first=0,count=0;
        if(FbVector(b,b.U32(0),0,first,count)&&count<=4096&&b.Has(first,count*4)){
            for(std::size_t i=0;i<count;++i){
                const auto at=first+i*4,row=at+b.U32(at);
                const auto number=[&](int field){const auto f=FbField(b,row,field);return f&&b.Has(f,4)?b.U32(f):0;};
                const auto flag=[&](int field){const auto f=FbField(b,row,field);return f&&b.Has(f,1)&&b.data[f]!=0;};
                const auto id=number(0);
                if(!id||id>=item_names_.size()||item_names_[id].empty()) continue;
                ItemUsage info{!flag(10),number(29)>0||number(30)>0,number(31)>0||number(32)>0};
                info.hp_points=number(29);
                info.hp_percent=number(30);
                info.revival_points=number(31);
                info.revival_percent=number(32);
                const auto cure=[&](PartyStatus status,int field){
                    if(flag(field)) info.cured_statuses|=1u<<static_cast<unsigned>(status);
                };
                cure(PartyStatus::Paralysis,17);
                cure(PartyStatus::Sleep,13);
                cure(PartyStatus::Frozen,16);
                cure(PartyStatus::Burn,15);
                cure(PartyStatus::Poison,14);
                item_usage_[id]=info;
            }
        }
    }
    std::vector<u8> conversion;
    std::unordered_map<int,int> direct_evolution_items;
    if(logical("world/ik_data/battle/plib_item_conversion/plib_item_conversion_array.bin",conversion)){
        const Buf b{conversion.data(),conversion.size()};
        std::size_t first=0,count=0;
        if(FbVector(b,b.U32(0),0,first,count)&&count<=3000&&b.Has(first,count*4)){
            for(std::size_t i=0;i<count;++i){
                const auto at=first+i*4,row=at+b.U32(at);
                const auto id=FbField(b,row,0),parameter=FbField(b,row,1);
                if(!id||!parameter||!b.Has(id,4)||!b.Has(parameter,4)) continue;
                const auto item=b.U32(id),key=b.U32(parameter);
                if(item==0||item>=item_names_.size()||key==0||key>65535) continue;
                auto [found,inserted]=direct_evolution_items.emplace(key,item);
                if(!inserted&&found->second!=item) found->second=0;
            }
        }
    }
    evolutions_by_item_.clear();
    for(auto& [key,rows]:evolutions_) for(auto& evolution:rows){
        switch(evolution.method){
        case 6:
            // Trade uses the held item ID directly. Using an item uses the conversion table.
            if(evolution.argument>0&&evolution.argument<item_names_.size()) evolution.item=evolution.argument;
            break;
        case 8:case 17:case 18:case 19:case 20:case 42:
            if(const auto found=direct_evolution_items.find(evolution.argument);found!=direct_evolution_items.end())
                evolution.item=found->second;
            break;
        }
        if(evolution.item&&key==evolution.source*256+evolution.source_form)
            evolutions_by_item_[evolution.item].push_back(evolution);
    }
    const auto load_mission_numbers = [&](const char* path, int category) {
        std::vector<u8> quests;
        if (!logical(path, quests)) return;
        const Buf b{quests.data(), quests.size()};
        std::size_t first = 0, count = 0;
        if (FbVector(b, b.U32(0), 0, first, count) && count <= 512 && b.Has(first,count*4)) {
            for (std::size_t i = 0; i < count; ++i) {
                const auto at = first + i * 4, row = at + b.U32(at), number = FbField(b,row,1);
                const auto id = number ? b.U32(number) : 0;
                const auto title = FbString(b,row,6);
                const auto mission = std::ranges::find(missions_[category], title, &Mission::id);
                if (mission != missions_[category].end()) {
                    if (id > 0 && id <= 512) {
                        mission->number = int(id);
                        mission->map_id = int(id);
                    }
                    mission->work = FbString(b,row,5);
                    mission->rewards = MissionRewards(b,row);
                }
            }
        }
    };
    load_mission_numbers("ik_event/bin/quest/main_quest.bin", 0);
    load_mission_numbers("ik_event/bin/quest/sub_quest.bin", 2);
    trainer_ranks_.clear();std::vector<u8> rank_config;
    if(logical("ik_event/bin/trainer_rank/trainer_rank.bin",rank_config)){
        Buf b{rank_config.data(),rank_config.size()};std::size_t first=0,count=0;
        if(FbVector(b,b.U32(0),0,first,count)&&count==27&&b.Has(first,count*4)){
            for(std::size_t i=0;i<count;++i){const auto at=first+i*4,row=at+b.U32(at),r=FbField(b,row,0),p=FbField(b,row,1);
                TrainerRank entry{r?int(b.U32(r)):0,p?int(b.U32(p)):0,FbString(b,row,2)};
                if(entry.rank!=int(i+1)||entry.points<0||entry.points>10000000||entry.name.empty()){trainer_ranks_.clear();break;}
                trainer_ranks_.push_back(std::move(entry));
            }
        }
    }
    std::vector<u8> extra_config;
    if(logical("ik_event/bin/quest/momiji_quest.bin",extra_config)){
        Buf b{extra_config.data(),extra_config.size()};std::size_t first=0,count=0;
        if(FbVector(b,b.U32(0),0,first,count)&&count<2048&&b.Has(first,count*4))for(std::size_t i=0;i<count;++i){
            const auto at=first+i*4,row=at+b.U32(at);const auto title=FbString(b,row,6),work=FbString(b,row,5);
            auto found=std::find_if(missions_[1].begin(),missions_[1].end(),[&](const auto& m){return m.id==title;});
            if(found==missions_[1].end())continue;found->work=work;
            found->rewards=MissionRewards(b,row);
            std::size_t texts=0,entries=0;if(FbVector(b,row,12,texts,entries)&&entries&&b.Has(texts,4)){
                const auto entry=texts+b.U32(texts);const auto outline=FbString(b,entry,1);auto text=extra_text.find(outline);
                if(text!=extra_text.end())found->description=text->second;
            }
            if(found->objectives.empty())found->objectives=found->description;
        }
    }
    // Exact zone geometry and placement are supplied by the game's own map UI.
    zones_.clear();battle_zones_.clear();centers_.clear();cafes_.clear();landmarks_.clear();
    std::vector<u8> map_ui,zone_config;
    std::unordered_map<std::string,int> zone_numbers;
    if(logical("world/exl/map_zone_pane/map_zone_pane/map_zone_pane.bin",zone_config)){
        Buf b{zone_config.data(),zone_config.size()};std::size_t first=0,count=0;
        if(FbVector(b,b.U32(0),0,first,count)&&count<256&&b.Has(first,count*4))for(std::size_t i=0;i<count;++i){
            const auto at=first+i*4,row=at+b.U32(at),number=FbField(b,row,1);
            const auto name=FbString(b,row,2);if(number&&!name.empty())zone_numbers[name]=int(b.U32(number));
        }
    }
    if(FetchInner("arc/uidatamapmap_info_00_fre.arc.trpak",0,map_ui)){
        Buf b{map_ui.data(),map_ui.size()};std::size_t at=0,len=0;
        if(SarcMember(b,"timg/__Combined.bntx",at,len))map_ui_bntx_.assign(map_ui.begin()+at,map_ui.begin()+at+len);
        if(SarcMember(b,"blyt/map_top_00.bflyt",at,len)){
            Buf layout{b.data+at,len};int texture=0;
            for(std::size_t section=layout.U16(6);layout.Has(section,8);){
                const auto size=layout.U32(section+4);if(size<8||!layout.Has(section,size))break;
                if(!std::memcmp(layout.data+section,"prt1",4)&&size>=84){
                    std::size_t n=0;while(n<24&&layout.data[section+12+n])++n;
                    std::string name(reinterpret_cast<const char*>(layout.data+section+12),n);
                    if(name.starts_with("L_w_")||name.starts_with("L_b_")){
                        auto value=[&](std::size_t offset){float f=0;auto u=layout.U32(section+offset);std::memcpy(&f,&u,4);return f;};
                        auto found=zone_numbers.find(name);if(found!=zone_numbers.end())zones_.push_back({found->second,texture,value(44),value(48),value(76),value(80)});
                        if(name.starts_with("L_b_")){
                            // Scene names use the game's event hash seed, distinct
                            // from the Trinity file-index FNV seed.
                            u64 hash=0xCBF29CE484222645ull;for(unsigned char c:name.substr(4)){hash^=c;hash*=1099511628211ull;}
                            battle_zones_.push_back({int(battle_zones_.size()+1),texture,value(44),value(48),value(76),value(80),hash});
                        }
                        ++texture;
                    }
                }section+=size;
            }
        }
    }
    // Field-area meshes are authored in world coordinates. Their scene objects rotate X/Z
    // by pi. Read the validated Havok mesh AABB rather than placing approximate map pins.
    std::vector<u8> scene;
    if(logical("world/ik_scene/field/area/t1/sub_scene/t1_field_area_/t1_field_area_0.trscn",scene)){
        Buf b{scene.data(),scene.size()};std::size_t first=0,count=0;
        if(FbVector(b,b.U32(0),4,first,count)&&count<4096&&b.Has(first,count*4))for(std::size_t i=0;i<count;++i){
            const auto at=first+i*4,row=at+b.U32(at),field=FbField(b,row,1);if(!field)continue;
            const auto vector=field+b.U32(field);if(!b.Has(vector,4)||!b.Has(vector+4,b.U32(vector)))continue;
            Buf object{b.data+vector+4,b.U32(vector)};if(!object.Has(0,4))continue;
            const auto name=FbString(object,object.U32(0),0);const bool is_cafe=name.starts_with("cafe_");
            if(!is_cafe&&!name.starts_with("pokemon_center_"))continue;
            std::string path="field/model/t1/t1_area/"+name+"/"+name+"_col.trcol";
            std::vector<u8> collision;if(!logical(path.c_str(),collision))continue;Buf c{collision.data(),collision.size()};
            if(!c.Has(0,32)||std::memcmp(c.data+4,"TAG0",4))continue;
            auto value=[&](std::size_t offset){float f=0;auto u=c.U32(offset);std::memcpy(&f,&u,4);return f;};
            for(std::size_t offset=32;c.Has(offset,96);offset+=16){
                const float xmin=value(offset),xmax=value(offset+16),ymin=value(offset+32),ymax=value(offset+48),zmin=value(offset+64),zmax=value(offset+80);
                if(value(offset+4)<3e38f||value(offset+20)>-3e38f||value(offset+68)<3e38f||value(offset+84)>-3e38f)continue;
                if(!(xmin>0&&xmax<1100&&xmax>xmin&&xmax-xmin<100&&zmin>0&&zmax<1100&&zmax>zmin&&zmax-zmin<100&&ymax>=ymin&&ymax-ymin<50))continue;
                (is_cafe?cafes_:centers_).push_back({name,-(xmin+xmax)*.5f,-(zmin+zmax)*.5f});break;
            }
        }
    }
    std::unordered_map<std::string,std::string> place_names;
    for(const auto* path:{"arc/ik_messagedatFrenchcommonplace_name.tbl.trpak","arc/ik_messagedatFrenchcommonplace_info.tbl.trpak"}){
    std::vector<u8> place_text,place_labels;
    if(FetchMessage(path,place_text,&place_labels)){
        std::vector<std::string> values;DecodeTextTable(Buf{place_text.data(),place_text.size()},values);
        Buf index{place_labels.data(),place_labels.size()};std::size_t at=8;
        for(unsigned i=0;i<index.U32(4)&&i<values.size();++i){if(!index.Has(at,10))break;auto length=index.U16(at+8);at+=10;if(!length||!index.Has(at,length))break;
            place_names[std::string(reinterpret_cast<const char*>(place_labels.data()+at),length-1)]=values[i];at+=length;}
    }
}
    // Align the game's own common UI translations. Both language tables must have the
    // same row count; story/script text is deliberately excluded from this small dictionary.
    ui_translations_.clear();
    if(language_!=3)for(const auto& path:paths){
        if(!path.starts_with("arc/ik_messagedatFrenchcommon"))continue;
        std::vector<u8> raw,fr_bytes,local_bytes;if(!Fetch(path.c_str(),raw))continue;
        std::size_t index=0;if(!UnpackPak(Buf{raw.data(),raw.size()},index,fr_bytes))continue;
        if(fr_bytes.size()>=4&&!std::memcmp(fr_bytes.data(),"AHTB",4)){index=1;if(!UnpackPak(Buf{raw.data(),raw.size()},index,fr_bytes))continue;}
        if(!FetchMessage(path.c_str(),local_bytes))continue;
        std::vector<std::string> fr,local;DecodeTextTable(Buf{fr_bytes.data(),fr_bytes.size()},fr);DecodeTextTable(Buf{local_bytes.data(),local_bytes.size()},local);
        if(fr.size()!=local.size())continue;
        for(std::size_t i=0;i<fr.size();++i)if(!fr[i].empty()&&fr[i].size()<=160&&!local[i].empty())ui_translations_.try_emplace(UiKey(fr[i]),local[i]);
    }
    std::vector<u8> buildings;
    if(logical("world/exl/map_building_data/map_building_data/map_building_data.bin",buildings)){
        Buf b{buildings.data(),buildings.size()};std::size_t first=0,count=0;
        if(FbVector(b,b.U32(0),0,first,count)&&count<4096&&b.Has(first,count*4))for(std::size_t i=0;i<count;++i){
            const auto at=first+i*4,row=at+b.U32(at);
            const auto model=FbString(b,row,7),key=FbString(b,row,2),info=FbString(b,row,3);
            const auto order=FbField(b,row,12),category=FbField(b,row,1);
            const auto warp=FbString(b,row,9);
            const auto kind=category?b.U32(category):0;
            u64 hash=0xCBF29CE484222645ull;
            for(unsigned char c:warp){hash^=c;hash*=kFnvPrime;}
            if(kind==15&&warp.starts_with("t1_")&&!place_names[key].empty()){
                Center landmark;
                landmark.name=place_names[key];
                landmark.description=place_names[info];
                landmark.travel.kind=15;
                landmark.travel.Add(int(i),hash);
                landmarks_.push_back(std::move(landmark));
            }
            for(auto& center:centers_)if(center.id==model){
                center.destination=order?int(b.U32(order)):0;
                center.name=place_names[key];center.description=place_names[info];
                if(kind==9&&warp.starts_with("t1_")&&warp.find("_pokecen")!=std::string::npos){
                    center.travel.kind=9;
                    center.travel.Add(int(i),hash);
                }
            }
            for(auto& cafe:cafes_)if(cafe.id==model){
                cafe.name=place_names[key];cafe.description=place_names[info];
                if(kind==13&&warp.starts_with("t1_cafe")){
                    cafe.travel.kind=13;
                    cafe.travel.Add(int(i),hash);
                }
            }
            if(kind==19&&warp.starts_with("t1_")){
                const auto zone=zone_numbers.find("L_w_"+FbString(b,row,8));
                if(zone!=zone_numbers.end())for(auto& entry:zones_)if(entry.number==zone->second){
                    entry.travel.kind=4;
                    entry.travel.Add(int(i),hash);
                }
            }
        }
    }
    std::erase_if(cafes_,[](const auto& cafe){return cafe.name.empty();});
    return true;
}

std::string Assets::UiText(const std::string& french){std::scoped_lock lock{mutex_};if(language_==3)return french;const auto found=ui_translations_.find(UiKey(french));return found!=ui_translations_.end()?found->second:std::string{};}

std::vector<Assets::Mission> Assets::Missions(int category){std::scoped_lock lock{mutex_};return category>=0&&category<3?missions_[category]:std::vector<Mission>{};}

bool Assets::EnsureNames() {
    std::scoped_lock lock{mutex_};
    if (!tried_ || (!ready_ && std::chrono::steady_clock::now() >= retry_at_)) {
        LoadLocked();
    }
    return ready_;
}

bool Assets::NamesReady() const {
    std::scoped_lock lock{mutex_};
    return ready_;
}

const char* Assets::SpeciesName(int species) {
    std::scoped_lock lock{mutex_};
    if (!tried_ || (!ready_ && std::chrono::steady_clock::now() >= retry_at_)) {
        LoadLocked();
    }
    if (species < 1 || static_cast<std::size_t>(species) > names_.size()) {
        return "";
    }
    return names_[static_cast<std::size_t>(species) - 1].c_str();
}

std::string Assets::ItemName(int item) {
    std::scoped_lock lock{mutex_};
    return item >= 0 && static_cast<std::size_t>(item) < item_names_.size()
        ? item_names_[item] : std::string{};
}

std::string Assets::ItemDescription(int item){std::scoped_lock lock{mutex_};return item>=0&&item<item_descriptions_.size()?item_descriptions_[item]:std::string{};}
unsigned Assets::ItemUsage::HpAfterMedicine(unsigned hp,unsigned maximum) const {
    if(!maximum||hp>=maximum) return hp;
    const unsigned points=hp?hp_points:revival_points;
    const unsigned percent=hp?hp_percent:revival_percent;
    if(percent>100||points>65535||(points&&percent)) return hp;
    const unsigned recovered=percent?std::max(1u,maximum*percent/100):points;
    return hp+std::min(recovered,maximum-hp);
}

Assets::ItemUsage Assets::ItemUsageInfo(int item){
    std::scoped_lock lock{mutex_};
    const auto found=item_usage_.find(item);
    return found==item_usage_.end()?ItemUsage{}:found->second;
}
std::vector<Assets::DexEntry> Assets::Pokedex(int category){std::scoped_lock lock{mutex_};if(category==2)return mega_forms_;std::vector<DexEntry> rows;for(const auto& e:pokedex_)if((category==0&&e.number<=232)||(category==1&&e.number>232))rows.push_back(e);return rows;}

Assets::Personal Assets::SpeciesInfo(int species,int form){
    std::scoped_lock lock{mutex_};auto found=personal_.find(species*256+form);
    if(found==personal_.end())found=personal_.find(species*256);
    return found==personal_.end()?Personal{}:found->second;
}
std::vector<Assets::Evolution> Assets::Evolutions(int species,int form){
    std::scoped_lock lock{mutex_};
    const auto found=evolutions_.find(species*256+form);
    return found==evolutions_.end()?std::vector<Evolution>{}:found->second;
}
std::vector<Assets::Evolution> Assets::EvolutionSourcesForItem(int item){
    std::scoped_lock lock{mutex_};
    const auto found=evolutions_by_item_.find(item);
    return found==evolutions_by_item_.end()?std::vector<Evolution>{}:found->second;
}
int Assets::MoveType(int move){std::scoped_lock lock{mutex_};return move>=0&&move<move_types_.size()?move_types_[move]:-1;}
std::vector<Assets::LearnMove> Assets::Learnset(int species,int form){std::scoped_lock lock{mutex_};auto it=learnsets_.find(species*256+form);if(it==learnsets_.end())it=learnsets_.find(species*256);return it==learnsets_.end()?std::vector<LearnMove>{}:it->second;}
std::vector<int> Assets::MachineMoves(int species,int form){std::scoped_lock lock{mutex_};auto it=machine_moves_.find(species*256+form);if(it==machine_moves_.end())it=machine_moves_.find(species*256);return it==machine_moves_.end()?std::vector<int>{}:it->second;}
Assets::MoveInfo Assets::MoveParameters(int move){std::scoped_lock lock{mutex_};return move>=0&&move<move_info_.size()?move_info_[move]:MoveInfo{};}
std::uint32_t Assets::ExperienceAt(int growth,int level){std::scoped_lock lock{mutex_};return growth>=0&&growth<8&&level>=0&&level<=100&&experience_.size()==808?experience_[growth*101+level]:0;}

std::vector<Assets::ResearchLevel> Assets::ResearchLevels(){
    std::scoped_lock lock{mutex_};
    if(research_tried_)return research_levels_;
    if(!ready_)return {};
    research_tried_=true;
    std::vector<u8> pack,bytes;std::vector<u64> hashes;
    // Resolve the logical member by hash: the unrelated pack name is chosen by Trinity.
    if(research_pack_path_.empty()||!Fetch(research_pack_path_.c_str(),pack))return {};
    const Buf p{pack.data(),pack.size()};
    if(!FbVecU64(p,p.U32(0),0,hashes))return {};
    const auto found=std::find(hashes.begin(),hashes.end(),Fnv1a("ik_event/bin/research_lv/research_lv.bin"));
    if(found==hashes.end()||!UnpackPak(p,std::size_t(found-hashes.begin()),bytes))return {};
    const Buf b{bytes.data(),bytes.size()};std::size_t first=0,count=0;
    if(!FbVector(b,b.U32(0),0,first,count)||count<1||count>100||!b.Has(first,count*4))return {};
    std::vector<ResearchLevel> rows;
    for(std::size_t i=0;i<count;++i){
        const auto at=first+i*4,row=at+b.U32(at);
        const auto read=[&](unsigned field){const auto f=FbField(b,row,field);return f&&b.Has(f,4)?int(b.U32(f)):0;};
        ResearchLevel level{read(0),read(1),read(2),read(3)};
        if(level.level!=int(i+1)||level.points<0||level.points>10000000||level.item<0||level.item>10000||level.count<0||level.count>999||
            (!rows.empty()&&level.points<=rows.back().points))return {};
        rows.push_back(level);
    }
    research_levels_=std::move(rows);return research_levels_;
}
std::vector<Assets::Zone> Assets::Zones(){std::scoped_lock lock{mutex_};return zones_;}
void Assets::LoadDimensionsLocked(){
    if(dimensions_tried_||!ready_)return;
    dimensions_tried_=true;
    // All five members currently share one small pack. Resolve ownership rather
    // than pinning its physical name, which changes with updates.
    std::string cached_path;std::vector<u8> cached_pack;std::vector<u64> hashes;
    const auto fetch=[&](int index,std::vector<u8>& out){
        const auto& path=dimension_pack_paths_[index];if(path.empty())return false;
        if(path!=cached_path){
            if(!Fetch(path.c_str(),cached_pack))return false;
            cached_path=path;hashes.clear();const Buf b{cached_pack.data(),cached_pack.size()};
            if(!FbVecU64(b,b.U32(0),0,hashes))return false;
        }
        const auto found=std::find(hashes.begin(),hashes.end(),Fnv1a(kDimensionFiles[index]));
        return found!=hashes.end()&&UnpackPak(Buf{cached_pack.data(),cached_pack.size()},std::size_t(found-hashes.begin()),out);
    };
    const auto rows=[](const Buf& b,std::size_t table,unsigned field,std::size_t max){
        std::vector<std::size_t> out;std::size_t first=0,count=0;
        if(!FbVector(b,table,field,first,count)||count>max||!b.Has(first,count*4))return out;
        for(std::size_t i=0;i<count;++i){const auto a=first+i*4,t=a+b.U32(a);if(!b.Has(t,4))return std::vector<std::size_t>{};out.push_back(t);}return out;
    };
    const auto dbrows=[&](const Buf& b){
        std::vector<std::size_t> out;
        for(const auto db:rows(b,b.U32(0),0,1000)){
            const auto v=rows(b,db,0,20000);if(out.size()+v.size()>20000)return std::vector<std::size_t>{};
            out.insert(out.end(),v.begin(),v.end());
        }return out;
    };
    const auto number=[](const Buf& b,std::size_t t,unsigned i){const auto f=FbField(b,t,i);return f?int(b.U32(f)):0;};
    std::vector<u8> bytes;
    if(fetch(0,bytes)){const Buf b{bytes.data(),bytes.size()};for(auto t:dbrows(b)){
        const int rank=number(b,t,0),level=number(b,t,2);if(rank>=0&&rank<5&&level>=1&&level<=250)dimension_levels_[rank]=level;
    }}
    if(fetch(1,bytes)){const Buf b{bytes.data(),bytes.size()};for(auto t:dbrows(b)){
        const auto id=FbString(b,t,0);const int size=number(b,t,1);
        if(!id.empty()&&size>=0&&size<=2)dimension_sizes_[id]=size;
    }}
    std::unordered_map<std::string,DexEntry> encounters;
    if(fetch(4,bytes)){const Buf b{bytes.data(),bytes.size()};for(auto t:dbrows(b)){
        const auto id=FbString(b,t,0);const int species=number(b,t,1),form=number(b,t,5);
        if(!id.empty()&&species>0&&species<=1025&&form>=0&&form<=255)encounters[id]={species,form,0};
    }}
    std::unordered_map<std::string,std::vector<DexEntry>> spawners;
    if(fetch(3,bytes)){const Buf b{bytes.data(),bytes.size()};for(auto t:dbrows(b)){
        const auto id=FbString(b,t,0);if(id.empty())continue;
        for(auto e:rows(b,t,5,100)){
            if(number(b,e,1)<=0)continue;
            const auto found=encounters.find(FbString(b,e,0));if(found!=encounters.end())spawners[id].push_back(found->second);
        }
    }}
    if(fetch(2,bytes)){const Buf b{bytes.data(),bytes.size()};for(auto t:dbrows(b)){
        const int type=number(b,t,0);if(type<0||type>17)continue;
        for(auto r:rows(b,t,1,5)){const int rank=number(b,r,0);if(rank<0||rank>4)continue;
            for(auto p:rows(b,r,1,255)){const int pattern=number(b,p,0);if(pattern<1||pattern>255)continue;
                auto& out=dimension_pokemon_[(type*5+rank)*256+pattern];
                for(auto s:rows(b,p,2,50)){
                    const auto found=spawners.find(FbString(b,s,0));if(found==spawners.end()||number(b,s,1)<=0)continue;
                    for(auto e:found->second)if(std::none_of(out.begin(),out.end(),[&](const auto& a){return a.species==e.species&&a.form==e.form;}))out.push_back(e);
                }
            }
        }
    }}
}
Assets::DimensionInfo Assets::DimensionDetails(const std::string& content,int rank,
        const std::array<int,3>& types,const std::array<int,3>& patterns,int kind){
    std::scoped_lock lock{mutex_};LoadDimensionsLocked();DimensionInfo result;
    if(rank<0||rank>4)return result;
    result.level=dimension_levels_[rank];
    const auto suffix=content.find("_dim_");const auto map=content.substr(0,suffix);
    if(auto found=dimension_sizes_.find(map);found!=dimension_sizes_.end())result.size=found->second;
    if(kind!=0)return result;
    for(int i=0;i<3;++i){if(types[i]<0||types[i]>17||patterns[i]<1||patterns[i]>255)continue;
        const auto found=dimension_pokemon_.find((types[i]*5+rank)*256+patterns[i]);if(found==dimension_pokemon_.end())continue;
        for(auto e:found->second)if(std::none_of(result.pokemon.begin(),result.pokemon.end(),[&](const auto& a){return a.species==e.species&&a.form==e.form;}))result.pokemon.push_back(e);
    }return result;
}
std::vector<Assets::Zone> Assets::BattleZones(){std::scoped_lock lock{mutex_};return battle_zones_;}
std::vector<Assets::TrainerRank> Assets::TrainerRanks(){std::scoped_lock lock{mutex_};return trainer_ranks_;}
int Assets::MapZoneAt(int x,int y){std::scoped_lock lock{mutex_};if(x<0||y<0||x>=640||y>=640||map_zone_hits_.size()!=640*640)return 0;return map_zone_hits_[std::size_t(y)*640+x];}
std::vector<Assets::Center> Assets::Cafes(){std::scoped_lock lock{mutex_};return cafes_;}
std::vector<Assets::Center> Assets::Centers(){std::scoped_lock lock{mutex_};return centers_;}
std::vector<Assets::Center> Assets::Landmarks(){std::scoped_lock lock{mutex_};return landmarks_;}
bool Assets::ZoneIcon(int index,Picture& out){
    std::scoped_lock lock{mutex_};char name[48];std::snprintf(name,sizeof(name),"map_zone_%02d^t",index);Buf b{map_ui_bntx_.data(),map_ui_bntx_.size()};Texture t;return index>=0&&index<32&&FindTexture(b,name,t)&&DecodeTexture(b,t,false,out);
}
bool Assets::CenterIcon(Picture& out){
    std::scoped_lock lock{mutex_};Buf b{map_ui_bntx_.data(),map_ui_bntx_.size()};Texture t;
    if(!FindTexture(b,"map_icon_center_color_00",t)||!DecodeTexture(b,t,false,out))return false;
    // Red selects the material fill; green is coverage. Raw RGB is a shader mask.
    for(std::size_t i=0;i<out.rgba.size();i+=4){
        const float fill=out.rgba[i]/255.0f;const auto alpha=out.rgba[i+1];
        out.rgba[i]=u8(255-20*fill);out.rgba[i+1]=u8(255-190*fill);out.rgba[i+2]=u8(255-185*fill);out.rgba[i+3]=alpha;
    }return out.Valid();
}
bool Assets::CafeIcon(Picture& out){
    std::scoped_lock lock{mutex_};Buf b{map_ui_bntx_.data(),map_ui_bntx_.size()};Texture t;
    if(!FindTexture(b,"map_icon_cafe_color_00",t)||!DecodeTexture(b,t,false,out))return false;
    for(std::size_t i=0;i<out.rgba.size();i+=4){const float fill=out.rgba[i]/255.0f;const auto alpha=out.rgba[i+1];
        out.rgba[i]=u8(255-102*fill);out.rgba[i+1]=u8(255-166*fill);out.rgba[i+2]=u8(255-209*fill);out.rgba[i+3]=alpha;}
    return out.Valid();
}

bool Assets::MapMarker(int index, Picture& out) {
    // The two-channel arrow points south in the atlas. Orient its pixels north,
    // so that the existing heading uses zero degrees for north.
    std::scoped_lock lock{mutex_};
    constexpr const char* names[] = {
        "map_icon_player_00^t", "map_dungeon_icon_ev_00^t",
        "map_dungeon_icon_ev_00^t", "map_icon_quest_main_pin_color_00^q", "map_icon_quest_sub_color_00^q",
        "map_dungeon_icon_lock_00^t", "map_icon_pin_00^q", "map_icon_building_color_00^t",
        "map_icon_dim_base_00^t"
    };
    if (index < 0 || index >= std::size(names)) {
        return false;
    }
    const Buf atlas{map_ui_bntx_.data(), map_ui_bntx_.size()};
    Texture texture;
    const char* dimension_name=index==1 ? "icon_cmn_dim_info_zone_tra_00^q" :
        index==2 ? "icon_cmn_dim_info_zone_tra_01^q" :
        index==5 ? "icon_cmn_dim_info_zone_tra_02^q" : nullptr;
    if(dimension_name && FindTexture(atlas,dimension_name,texture) &&
       DecodeTexture(atlas,texture,false,out)) return out.Valid();
    if (!FindTexture(atlas, names[index], texture) ||
        !DecodeTexture(atlas, texture, false, out)) {
        return false;
    }
    if (index==3 || index==4 || index==6) return out.Valid();
    if(index==8){
        for(std::size_t i=0;i<out.rgba.size();i+=4){
            const auto alpha=out.rgba[i+1];
            out.rgba[i]=17;out.rgba[i+1]=25;out.rgba[i+2]=33;out.rgba[i+3]=alpha;
        }
        return out.Valid();
    }
    constexpr u8 colors[][3] = {
        {32,179,243}, {103,53,178}, {195,71,35}, {}, {}, {69,42,101}, {}, {24,82,120}, {17,25,33}
    };
    for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        const float fill = 1.f - out.rgba[i] / 255.f;
        out.rgba[i + 3] = out.rgba[i + 1];
        for (std::size_t c = 0; c < 3; ++c) {
            out.rgba[i + c] = u8(247.f + (colors[index][c] - 247.f) * fill);
        }
    }
    if (index == 0) {
        const std::size_t count = out.rgba.size() / 4;
        for (std::size_t i = 0; i < count / 2; ++i) {
            for (std::size_t c = 0; c < 4; ++c) {
                std::swap(out.rgba[4 * i + c], out.rgba[4 * (count - 1 - i) + c]);
            }
        }
    }
    return out.Valid();
}

bool Assets::MoneyIcon(Picture& out) {
    std::scoped_lock lock{mutex_};
    if(money_icon_.Valid()){out=money_icon_;return true;}
    for(std::size_t i=0;i<2;++i){
        std::vector<u8> bytes;
        if(!FetchInner("arc/uidatamoney_windowview_money_window.truiv.trpak",i,bytes))continue;
        const Buf sarc{bytes.data(),bytes.size()};
        std::size_t offset=0,length=0;
        if(!SarcMember(sarc,"timg/__Combined.bntx",offset,length))continue;
        const Buf atlas{bytes.data()+offset,length};
        Texture texture;
        if(FindTexture(atlas,"icon_money_color^u",texture)&&
           DecodeTexture(atlas,texture,false,money_icon_)){
            out=money_icon_;
            return true;
        }
    }
    return false;
}

bool Assets::ProgressArt(int index,Picture& out){
    std::scoped_lock lock{mutex_};out.Clear();
    const char* names[]={"mresearch_top_lv_gauge_base_00","mresearch_top_lv_gauge_border_00","icon_mresarch","mresearch_top_deco_re_00","mresearch_top_i_check_01"};
    if(index<0||index>=5)return false;
    if(auto found=progress_art_.find(index);found!=progress_art_.end()){out=found->second;return true;}
    if(progress_bntx_.empty()){
        std::vector<u8> bytes;if(!FetchInner("arc/uidatamresearchmresearch_top.arc.trpak",0,bytes))return false;
        Buf sarc{bytes.data(),bytes.size()};std::size_t at=0,len=0;if(!SarcMember(sarc,"timg/__Combined.bntx",at,len))return false;
        progress_bntx_.assign(bytes.begin()+at,bytes.begin()+at+len);
    }
    Buf b{progress_bntx_.data(),progress_bntx_.size()};Texture texture;
    if(!FindTexture(b,names[index],texture)||!DecodeTexture(b,texture,false,out))return false;
    // UI glyphs are single-channel coverage masks, not opaque red pictures.
    if(index<3||index==4){
        const u8 colors[][3]={{245,252,247},{42,117,103},{36,75,73}};
        const auto color=index==4?2:index;
        // The two-channel ^t checkmark stores coverage in green (red is solid white).
        const auto coverage=(texture.format>>8)==0x1E?1:0;
        for(std::size_t p=0;p<out.rgba.size();p+=4){const auto alpha=out.rgba[p+coverage];out.rgba[p]=colors[color][0];out.rgba[p+1]=colors[color][1];out.rgba[p+2]=colors[color][2];out.rgba[p+3]=alpha;}
    }else{
        // Keep Myosotis and her decoration in the right side of the research card.
        constexpr unsigned x=900,y=0,w=1020,h=780;
        if(out.width==1920&&out.height==1080){Picture crop;crop.width=w;crop.height=h;crop.rgba.resize(w*h*4);for(unsigned row=0;row<h;++row)std::memcpy(crop.rgba.data()+row*w*4,out.rgba.data()+((row+y)*out.width+x)*4,w*4);out=std::move(crop);}
    }
    progress_art_[index]=out;return true;
}

bool Assets::MenuIcon(int index,Picture& out){
    if(index==3||index==4){if(!ProgressArt(index==3?2:4,out))return false;for(std::size_t p=0;p<out.rgba.size();p+=4)out.rgba[p]=out.rgba[p+1]=out.rgba[p+2]=255;return true;}
    std::scoped_lock lock{mutex_};out.Clear();
    const char* names[]={"status_tab_icon_00","icon_dex","icon_bspoke"};if(index<0||index>2)return false;
    if(auto found=type_icons_.find(index+100);found!=type_icons_.end()){out=found->second;return true;}
    if(type_bntx_.empty()){
        std::vector<u8> bytes;if(!FetchInner("arc/uidatastatusstatus_top_00_fre.arc.trpak",0,bytes))return false;
        Buf sarc{bytes.data(),bytes.size()};std::size_t at=0,len=0;if(!SarcMember(sarc,"timg/__Combined.bntx",at,len))return false;type_bntx_.assign(bytes.begin()+at,bytes.begin()+at+len);
    }
    Buf b{type_bntx_.data(),type_bntx_.size()};Texture texture;if(!FindTexture(b,names[index],texture)||!DecodeTexture(b,texture,false,out))return false;
    if((texture.format>>8)==0x1D)for(std::size_t p=0;p<out.rgba.size();p+=4){out.rgba[p+3]=out.rgba[p];out.rgba[p]=out.rgba[p+1]=out.rgba[p+2]=255;}
    type_icons_[index+100]=out;return true;
}

bool Assets::TypeIcon(int type,Picture& out){
    std::scoped_lock lock{mutex_};out.Clear();if(type<0||type>17)return false;
    if(auto found=type_icons_.find(type);found!=type_icons_.end()){out=found->second;return true;}
    if(type_bntx_.empty()){
        std::vector<u8> bytes;if(!FetchInner("arc/uidatastatusstatus_top_00_fre.arc.trpak",0,bytes))return false;
        Buf sarc{bytes.data(),bytes.size()};std::size_t offset=0,length=0;
        if(!SarcMember(sarc,"timg/__Combined.bntx",offset,length))return false;
        type_bntx_.assign(bytes.begin()+offset,bytes.begin()+offset+length);
    }
    char name[64];std::snprintf(name,sizeof(name),"grptxt_icon_type_%02d_",type);
    Buf b{type_bntx_.data(),type_bntx_.size()};Texture texture;
    if(!FindTexture(b,name,texture)||!DecodeTexture(b,texture,false,out))return false;
    // The game's BC4 type texture stores the glyph coverage in its red channel.
    for(std::size_t p=0;p<out.rgba.size();p+=4){out.rgba[p+3]=out.rgba[p];out.rgba[p]=out.rgba[p+1]=out.rgba[p+2]=255;}
    type_icons_[type]=out;return out.Valid();
}
std::string Assets::MoveName(int move) {
    std::scoped_lock lock{mutex_};
    return move >= 0 && static_cast<std::size_t>(move) < move_names_.size()
        ? move_names_[move] : std::string{};
}

std::string Assets::TypeName(int type){std::scoped_lock lock{mutex_};return type>=0&&type<type_names_.size()?type_names_[type]:std::string{};}
std::string Assets::NatureName(int nature){std::scoped_lock lock{mutex_};return nature>=0&&nature<nature_names_.size()?nature_names_[nature]:std::string{};}

std::string Assets::MoveDescription(int move) {
    std::scoped_lock lock{mutex_};
    return move >= 0 && static_cast<std::size_t>(move) < move_descriptions_.size()
        ? move_descriptions_[move] : std::string{};
}

std::string Assets::SpeciesDescription(int species) {
    std::scoped_lock lock{mutex_};
    if (species < 1 || static_cast<std::size_t>(species) >= species_descriptions_.size()) return {};
    const auto& text = species_descriptions_[species];
    // Some unavailable species have only a GF substitution/control token, not a description.
    return text.size() > 12 ? text : std::string{};
}

bool Assets::IndexItemsLocked() {
    if (item_index_tried_) return !item_indices_.empty();
    if (!Fetch("arc/uitexicon_itemitem_0000item_0000.bntx.trpak", item_pack_)) return false;
    item_index_tried_ = true;
    const Buf pak{item_pack_.data(), item_pack_.size()};
    const auto header = pak.U32(0);
    if (header < 4 || !pak.Has(header + 12, 4)) return false;
    const auto count = pak.U32(header + 12);
    if (!count || count > 4096) return false;
    for (u32 i = 0; i < count; ++i) {
        std::vector<u8> bytes;
        if (!UnpackPak(pak, i, bytes)) continue;
        Texture texture;
        if (!FindTexture(Buf{bytes.data(), bytes.size()}, nullptr, texture)) continue;
        int id = -1; char suffix = 0;
        if (std::sscanf(texture.name.c_str(), "item_%d%c", &id, &suffix) == 1 && id >= 0 && id < 3000)
            item_indices_.try_emplace(id, i);
    }
    ZA_TRACE("item icons: %zu named images in %u inner files\n", item_indices_.size(), count);
    return !item_indices_.empty();
}

bool Assets::ItemIcon(int item, Picture& out) {
    std::scoped_lock lock{mutex_};
    out.Clear();
    if (!tried_ || (!ready_ && std::chrono::steady_clock::now() >= retry_at_)) LoadLocked();
    if (const auto cached = item_icons_.find(item); cached != item_icons_.end()) {
        out = cached->second; return out.Valid();
    }
    if (item < 0 || item >= 3000 || !IndexItemsLocked()) return false;
    const auto entry = item_indices_.find(item);
    if (entry == item_indices_.end()) return false;
    std::vector<u8> bytes;
    if (!UnpackPak(Buf{item_pack_.data(), item_pack_.size()}, entry->second, bytes)) return false;
    const Buf bntx{bytes.data(), bytes.size()}; Texture texture; Picture image;
    if (!FindTexture(bntx, nullptr, texture) || !DecodeTexture(bntx, texture, false, image)) return false;
    if (item_icon_order_.size() >= 64) {
        item_icons_.erase(item_icon_order_.front()); item_icon_order_.erase(item_icon_order_.begin());
    }
    item_icon_order_.push_back(item); item_icons_.emplace(item, image);
    out = std::move(image); return out.Valid();
}

bool Assets::HasSpecies(int species) const {
    std::scoped_lock lock{mutex_};
    return icon_packs_.contains(species);
}

bool Assets::IconLocked(int species, int form, Picture& out, bool shiny) {
    if (!tried_ || (!ready_ && std::chrono::steady_clock::now() >= retry_at_)) {
        LoadLocked();
    }
    const int cache_key = species * 512 + form * 2 + int(shiny);
    const auto cached = icons_.find(cache_key);
    if (cached != icons_.end()) {
        out = cached->second;
        return out.Valid();
    }
    const auto packs = icon_packs_.find(species);
    if (packs == icon_packs_.end()) {
        return false;
    }
    // Prefer the form the save reports; otherwise take any form the game ships (Zygarde has no
    // ordinary form at all, only its 11..30 ones).
    const IconRef* chosen = nullptr;
    for (const IconRef& ref : packs->second) {
        if (ref.form == form) {
            chosen = &ref;
            break;
        }
        if (chosen == nullptr || (ref.form == 0 && chosen->form != 0)) {
            chosen = &ref;
        }
    }
    if (chosen == nullptr) {
        return false;
    }
    // Pack order is hash order, not palette order: Lucario's first entry is shiny.
    const auto start=chosen->path.find("iconpm");
    if(start==std::string::npos)return false;
    std::string wanted=chosen->path.substr(start+4);
    const auto extension=wanted.find(".bntx");if(extension==std::string::npos||extension==0)return false;
    wanted.resize(extension);wanted.back()=shiny?'1':'0';
    Picture picture;bool decoded=false;
    for(std::size_t inner=0;inner<4;++inner){
        std::vector<std::uint8_t> bntx;if(!FetchInner(chosen->path.c_str(),inner,bntx))break;
        const Buf bytes{bntx.data(),bntx.size()};Texture texture;
        if(FindTexture(bytes,wanted.c_str(),texture)&&DecodeTexture(bytes,texture,false,picture)&&picture.Valid()){decoded=true;break;}
    }
    if(!decoded)return false;
    // Keep the last 64 icons: a party plus two boxes, so browsing back and forth does not
    // decode the same pack again.
    if (icon_order_.size() >= 64) {
        const int oldest = icon_order_.front();
        icon_order_.erase(icon_order_.begin());
        icons_.erase(oldest);
    }
    icon_order_.push_back(cache_key);
    icons_.emplace(cache_key, picture);
    out = std::move(picture);
    return out.Valid();
}

bool Assets::Icon(int species, int form, Picture& out, bool shiny) {
    std::scoped_lock lock{mutex_};
    out.Clear();
    return IconLocked(species, form, out, shiny);
}

bool Assets::Map(unsigned size, Picture& out, unsigned layers) {
    std::scoped_lock lock{mutex_};
    if(layers>16383)return false;
    if(!maps_.contains(layers)&&maps_.size()>=4)maps_.clear();
    auto& map_=maps_[layers];
    if (map_.Valid() && map_.width == size) {
        out = map_;
        return true;
    }
    if (!tried_ || (!ready_ && std::chrono::steady_clock::now() >= retry_at_)) {
        LoadLocked();
    }
    std::vector<std::uint8_t> sarc_bytes;
    if (!FetchInner("arc/uidatamapview_map_field_00.truiv.trpak", 0, sarc_bytes)) {
        return false;
    }
    const Buf sarc{sarc_bytes.data(), sarc_bytes.size()};
    std::size_t offset = 0;
    std::size_t length = 0;
    if (!SarcMember(sarc, "timg/__Combined.bntx", offset, length)) {
        ZA_TRACE("map: timg/__Combined.bntx not in the SARC (%zu bytes)\n", sarc_bytes.size());
        return false;
    }
    const Buf bntx{sarc.data + offset, length};
    Texture texture;
    if (!FindTexture(bntx, "map_texture_00", texture)) {
        ZA_TRACE("map: map_texture_00 not in the BNTX (%zu bytes)\n", length);
        return false;
    }
    Picture blocks;
    if (!DecodeTexture(bntx, texture, true, blocks)) {
        ZA_TRACE("map: map_texture_00 %ux%u fmt 0x%X did not decode\n", texture.width,
                 texture.height, texture.format);
        return false;
    }
    Picture scaled;
    BoxFilter(blocks, size, scaled);
    if (!scaled.Valid()) {
        return false;
    }
    Buf map_ui{map_ui_bntx_.data(),map_ui_bntx_.size()};
    if(size==640)map_zone_hits_.assign(size*size,0);
    auto overlays=battle_zones_;overlays.insert(overlays.end(),zones_.begin(),zones_.end());
    for(const auto& zone:overlays){const bool battle=zone.texture>=20;
        char name[48];std::snprintf(name,sizeof(name),"map_zone_%02d^t",zone.texture);Texture t;Picture mask;
        if(!FindTexture(map_ui,name,t)||!DecodeTexture(map_ui,t,false,mask))continue;
        const float factor=float(size)/4096.0f;
        const int left=int(std::lround((2048+zone.x-zone.w*.5f)*factor)),top=int(std::lround((2048-zone.y-zone.h*.5f)*factor));
        const int width=std::max(1,int(std::lround(zone.w*factor))),height=std::max(1,int(std::lround(zone.h*factor)));
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){
            const int dx=left+x,dy=top+y;if(dx<0||dy<0||dx>=int(size)||dy>=int(size))continue;
            const auto source=(std::size_t(y*mask.height/height)*mask.width+x*mask.width/width)*4,dest=(std::size_t(dy)*size+dx)*4;
            // The map material stores coverage in green (0 outside, .4 fill, 1 border).
            const float alpha=mask.rgba[source+1]/255.0f*.85f;
            if(size==640&&!battle&&alpha>.15f)map_zone_hits_[std::size_t(dy)*size+dx]=zone.number;
            if(!(layers&(battle?2:1))||(battle&&!(layers&(1u<<(zone.texture-20+2)))))continue;
            const int green[]={battle?235:30,battle?82:207,battle?94:112};for(int channel=0;channel<3;++channel)scaled.rgba[dest+channel]=u8(std::lround(scaled.rgba[dest+channel]*(1-alpha)+green[channel]*alpha));
        }
    }
    // Present the city's circular plan without the opaque square corners around it.
    // Texture coordinates and the encounter hit mask are left unchanged.
    const float center=(float(size)-1)*.5f,radius=float(size)*.5f;
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){
        const float coverage=std::clamp(radius+.5f-std::hypot(float(x)-center,float(y)-center),0.0f,1.0f);
        scaled.rgba[(std::size_t(y)*size+x)*4+3]=u8(std::lround(255*coverage));
    }
    map_ = scaled;
    out = std::move(scaled);
    return true;
}

} // namespace za
