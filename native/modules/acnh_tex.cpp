// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// BNTX decoding for the ACNH companion. Deswizzle + format table follow wonder_assets.cpp
// (copied so the companions can change independently); R5G6B5 added, selectors as in BRTI.

#include "acnh_tex.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>

#include <bc_decoder.h>

#include "acnh_bytes.h"

#include "core/mods/modules/dsmod_module_sdk.h"

namespace acnh {
namespace {
using namespace dsmod_sdk::int_types;
constexpr size_t MaxSource = 64u << 20;
constexpr u32 MaxDimension = 4096;
constexpr u64 MaxPixels = 8u << 20;

std::mutex astc_mutex;
void* astc_userdata = nullptr;
EdenDsmodAstcDecoder astc_decode = nullptr;

using bytes::Fits;

struct Format {
    u32 bw, bh, bpb, channels;
};
std::optional<Format> GetFormat(u32 f) {
    switch (f >> 8) {
    case 0x02:
        return Format{1, 1, 1, 1};
    case 0x07:
        return Format{1, 1, 2, 4};
    case 0x09:
        return Format{1, 1, 2, 2};
    case 0x0b:
    case 0x0c:
        return Format{1, 1, 4, 4};
    case 0x1a:
        return Format{4, 4, 8, 4};
    case 0x1b:
    case 0x1c:
        return Format{4, 4, 16, 4};
    case 0x1d:
        return Format{4, 4, 8, 1};
    case 0x1e:
        return Format{4, 4, 16, 2};
    case 0x20:
        return Format{4, 4, 16, 4};
    default:
        break;
    }
    static constexpr std::array<std::pair<u32, u32>, 14> astc{{{4, 4},
                                                               {5, 4},
                                                               {5, 5},
                                                               {6, 5},
                                                               {6, 6},
                                                               {8, 5},
                                                               {8, 6},
                                                               {8, 8},
                                                               {10, 5},
                                                               {10, 6},
                                                               {10, 8},
                                                               {10, 10},
                                                               {12, 10},
                                                               {12, 12}}};
    const u32 t = f >> 8;
    if (t >= 0x2d && t < 0x2d + astc.size())
        return Format{astc[t - 0x2d].first, astc[t - 0x2d].second, 16, 4};
    return std::nullopt;
}

u32 Pdep(u32 value, u32 mask) {
    u32 result = 0;
    for (u32 bit = 1; mask; bit += bit) {
        if (value & bit)
            result |= mask & (~mask + 1);
        mask &= mask - 1;
    }
    return result;
}

bool Unswizzle2d(std::span<const u8> source, u32 bpp, u32 width, u32 height, u32 block_height,
                 std::vector<u8>& output) {
    if (!bpp || block_height > 5)
        return false;
    const u64 row_bytes = u64{width} * bpp;
    const u64 aligned_width = (row_bytes + 63) & ~u64{63};
    const u64 aligned_height =
        (u64{height} + (u64{8} << block_height) - 1) & ~((u64{8} << block_height) - 1);
    if (aligned_width * aligned_height > source.size() || row_bytes * height > MaxSource)
        return false;
    output.assign(static_cast<size_t>(row_bytes * height), 0);
    const u32 stride = static_cast<u32>(aligned_width);
    const u32 gobs_x = stride >> 6;
    const u32 block_size = gobs_x << (9 + block_height);
    const u32 height_mask = (1u << block_height) - 1;
    for (u32 y = 0; y < height; ++y) {
        const u32 swizzled_y = Pdep(y, 0b011010000);
        const u32 block_y = y >> 3;
        const u32 offset_y =
            (block_y >> block_height) * block_size + ((block_y & height_mask) << 9);
        for (u32 x = 0; x < width; ++x) {
            const u32 byte_x = x * bpp;
            const u64 offset = u64{offset_y} + (u64{byte_x >> 6} << (9 + block_height)) +
                               (Pdep(byte_x, 0b100101111) | swizzled_y);
            if (offset + bpp > source.size())
                return false;
            std::memcpy(output.data() + size_t{y} * row_bytes + byte_x, source.data() + offset,
                        bpp);
        }
    }
    return true;
}

struct Texture {
    u32 w, h, format, size, arrays, layout, cmap;
    u16 tile;
    u64 image;
    std::string name;
};

std::span<const u8> BntxView(std::span<const u8> source) {
    const std::array<u8, 4> bm{'B', 'N', 'T', 'X'};
    auto it = std::search(source.begin(), source.end(), bm.begin(), bm.end());
    if (it == source.end())
        return {};
    auto b = source.subspan(static_cast<size_t>(it - source.begin()));
    if (b.size() < 0x30)
        return {};
    const u32 declared = bytes::Le32(b, 0x1c);
    if (declared >= 0x30 && declared <= b.size())
        b = b.first(declared);
    return b;
}

std::vector<Texture> Textures(std::span<const u8> b) {
    std::vector<Texture> out;
    if (b.empty())
        return out;
    const u32 count = bytes::Le32(b, 0x24);
    const u64 array = bytes::Le64(b, 0x28);
    if (!count || count > 8192 || !Fits(b, array, u64{count} * 8))
        return out;
    for (u32 i = 0; i < count; ++i) {
        const u64 o = bytes::Le64(b, array + u64{i} * 8);
        if (!Fits(b, o, 0xa0) || std::memcmp(b.data() + o, "BRTI", 4) != 0)
            continue;
        Texture t{bytes::Le32(b, o + 0x24),
                  bytes::Le32(b, o + 0x28),
                  bytes::Le32(b, o + 0x1c),
                  bytes::Le32(b, o + 0x50),
                  bytes::Le32(b, o + 0x30),
                  bytes::Le32(b, o + 0x34),
                  bytes::Le32(b, o + 0x58),
                  bytes::Le16(b, o + 0x12),
                  0,
                  {}};
        const u64 mip = bytes::Le64(b, o + 0x70);
        if (!Fits(b, mip, 8))
            continue;
        t.image = bytes::Le64(b, mip);
        const u64 no = bytes::Le64(b, o + 0x60);
        if (Fits(b, no, 2)) {
            const u16 nl = bytes::Le16(b, no);
            if (Fits(b, no + 2, nl))
                t.name.assign(reinterpret_cast<const char*>(b.data() + no + 2), nl);
        }
        if (!t.w || !t.h || t.w > MaxDimension || t.h > MaxDimension ||
            u64{t.w} * t.h > MaxPixels || !t.arrays || t.image >= b.size())
            continue;
        out.push_back(std::move(t));
    }
    return out;
}

bool DecodeTexture(std::span<const u8> b, const Texture& t, Image& out) {
    const auto f = GetFormat(t.format);
    if (!f)
        return false;
    const u32 bx = (t.w + f->bw - 1) / f->bw, by = (t.h + f->bh - 1) / f->bh;
    const size_t array_size = t.size / t.arrays;
    if (!Fits(b, t.image, array_size))
        return false;
    const auto src = b.subspan(t.image, array_size);
    std::vector<u8> blocks;
    if (t.tile == 0) {
        if (!Unswizzle2d(src, f->bpb, bx, by, t.layout & 7, blocks))
            return false;
    } else if (t.tile == 1) {
        const size_t row = size_t{bx} * f->bpb;
        size_t pitch = (row + 31) / 32 * 32;
        if (pitch * by > src.size())
            pitch = row;
        if (pitch * by > src.size())
            return false;
        blocks.resize(row * by);
        for (u32 y = 0; y < by; ++y)
            std::memcpy(blocks.data() + y * row, src.data() + y * pitch, row);
    } else {
        return false;
    }
    const size_t pixels = size_t{t.w} * t.h;
    std::vector<u8> channels(pixels * f->channels);
    const u32 fc = t.format >> 8;
    if (fc == 0x02 || fc == 0x09 || fc == 0x0b) {
        if (blocks.size() < channels.size())
            return false;
        std::copy_n(blocks.begin(), channels.size(), channels.begin());
    } else if (fc == 0x0c) {
        if (blocks.size() < channels.size())
            return false;
        for (size_t p = 0; p < channels.size(); p += 4) {
            channels[p] = blocks[p + 2];
            channels[p + 1] = blocks[p + 1];
            channels[p + 2] = blocks[p];
            channels[p + 3] = blocks[p + 3];
        }
    } else if (fc == 0x07) {
        if (blocks.size() < pixels * 2)
            return false;
        for (size_t p = 0; p < pixels; ++p) {
            const u32 v = dsmod_sdk::Le16(blocks.data() + p * 2);
            channels[p * 4] = static_cast<u8>(((v >> 11) & 31) * 255 / 31);
            channels[p * 4 + 1] = static_cast<u8>(((v >> 5) & 63) * 255 / 63);
            channels[p * 4 + 2] = static_cast<u8>((v & 31) * 255 / 31);
            channels[p * 4 + 3] = 255;
        }
    } else if (fc >= 0x2d) {
        void* ud;
        EdenDsmodAstcDecoder dec;
        {
            std::scoped_lock lock{astc_mutex};
            ud = astc_userdata;
            dec = astc_decode;
        }
        const size_t need = size_t{bx} * by * 16;
        if (!dec || blocks.size() < need ||
            !dec(ud, t.w, t.h, f->bw, f->bh, blocks.data(), need, channels.data(), channels.size()))
            return false;
    } else {
        for (u32 y = 0; y < by; ++y)
            for (u32 x = 0; x < bx; ++x) {
                const u8* s = blocks.data() + (size_t{y} * bx + x) * f->bpb;
                u8* d = channels.data() + (size_t{y} * 4 * t.w + x * 4) * f->channels;
                switch (fc) {
                case 0x1a:
                    bcn::DecodeBc1(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                case 0x1b:
                    bcn::DecodeBc2(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                case 0x1c:
                    bcn::DecodeBc3(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                case 0x1d:
                    bcn::DecodeBc4(s, d, x * 4, y * 4, t.w, t.h, (t.format & 255) == 2);
                    break;
                case 0x1e:
                    bcn::DecodeBc5(s, d, x * 4, y * 4, t.w, t.h, (t.format & 255) == 2);
                    break;
                case 0x20:
                    bcn::DecodeBc7(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                default:
                    return false;
                }
            }
    }
    out = Image{static_cast<int>(t.w), static_cast<int>(t.h)};
    for (size_t p = 0; p < pixels; ++p)
        for (u32 c = 0; c < 4; ++c) {
            const u8 sel = static_cast<u8>(t.cmap >> (c * 8));
            out.rgba[p * 4 + c] =
                sel == 0   ? 0
                : sel == 1 ? 255
                           : (sel - 2u < f->channels ? channels[p * f->channels + sel - 2] : 0);
        }
    return true;
}
} // namespace

void SetAstcDecoder(void* userdata, EdenDsmodAstcDecoder decode) {
    std::scoped_lock lock{astc_mutex};
    astc_userdata = userdata;
    astc_decode = decode;
}

bool BntxDecodeAny(const uint8_t* bytes, size_t size, std::string_view tex, Image& out) {
    const auto b = BntxView({bytes, size});
    for (const auto& t : Textures(b))
        if (tex.empty() || t.name == tex)
            return DecodeTexture(b, t, out);
    return false;
}

bool BntxDecode(const std::vector<uint8_t>& bntx, std::string_view tex, Image& out) {
    return BntxDecodeAny(bntx.data(), bntx.size(), tex, out);
}

bool BfresBntxDecode(const std::vector<uint8_t>& bfres, std::string_view tex, Image& out) {
    return BntxDecodeAny(bfres.data(), bfres.size(), tex, out);
}

std::vector<TexInfo> BntxList(const std::vector<uint8_t>& bytes) {
    std::vector<TexInfo> out;
    for (const auto& t : Textures(BntxView(bytes))) {
        TexInfo i;
        i.name = t.name;
        i.format = t.format;
        i.w = static_cast<int>(t.w);
        i.h = static_cast<int>(t.h);
        for (int c = 0; c < 4; ++c)
            i.selectors[c] = static_cast<uint8_t>(t.cmap >> (c * 8));
        out.push_back(std::move(i));
    }
    return out;
}

} // namespace acnh
