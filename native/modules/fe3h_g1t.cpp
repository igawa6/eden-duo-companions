// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe3h_g1t.h"

#include <cstring>

#include <bc_decoder.h>

namespace fe3h_g1t {
namespace {

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

struct Header {
    uint32_t count{}, table{};
    uint64_t size{};
};

bool ReadHeader(fe3h_romfs::Source& g, Header& h) {
    uint8_t b[0x1C];
    h.size = g.Size();
    if (h.size < sizeof(b) || !g.Read(0, sizeof(b), b) || std::memcmp(b, "GT1G", 4) != 0)
        return false;
    h.table = U32(b + 0x0C);
    h.count = U32(b + 0x10);
    return h.count > 0 && h.count < 4096 && h.table >= 0x1C &&
           uint64_t(h.table) + uint64_t(h.count) * 4 <= h.size;
}

} // namespace

uint64_t Mip0Size(uint8_t format, uint32_t w, uint32_t h) {
    const uint64_t bx = (w + 3) / 4, by = (h + 3) / 4;
    switch (format) {
    case 0x59:
        return bx * by * 8;
    case 0x5A:
    case 0x5B:
    case 0x5F:
        return bx * by * 16;
    case 0x00:
    case 0x01:
        return uint64_t(w) * h * 4;
    default:
        return 0;
    }
}

bool Count(fe3h_romfs::Source& g, uint32_t& count) {
    Header h;
    if (!ReadHeader(g, h))
        return false;
    count = h.count;
    return true;
}

std::optional<TextureInfo> Texture(fe3h_romfs::Source& g, uint32_t index) {
    Header h;
    if (!ReadHeader(g, h) || index >= h.count)
        return std::nullopt;
    uint8_t o[8];
    const bool has_next = index + 1 < h.count;
    if (!g.Read(h.table + uint64_t(index) * 4, has_next ? 8 : 4, o))
        return std::nullopt;
    const uint64_t start = uint64_t(h.table) + U32(o);
    const uint64_t end = has_next ? uint64_t(h.table) + U32(o + 4) : h.size;
    if (start + 8 > end || end > h.size)
        return std::nullopt;
    uint8_t t[8 + 0x14];
    const size_t want = static_cast<size_t>(std::min<uint64_t>(sizeof(t), end - start));
    if (!g.Read(start, want, t))
        return std::nullopt;
    TextureInfo info;
    info.index = index;
    info.mips = t[0] >> 4;
    info.format = t[1];
    if ((t[0] & 0x0F) != 0 || info.mips == 0) // array / volume layouts are not decoded
        return std::nullopt;
    info.width = 1u << (t[2] & 0x0F);
    info.height = 1u << (t[2] >> 4);
    uint64_t p = start + 8;
    if (t[7] & 0x10) {
        if (want < 12)
            return std::nullopt;
        const uint32_t ext = U32(t + 8);
        if (ext < 4 || ext > 0x100 || p + ext > end)
            return std::nullopt;
        if (ext >= 0x14) {
            if (want < 8 + 0x14)
                return std::nullopt;
            info.width = U32(t + 8 + 12);
            info.height = U32(t + 8 + 16);
        }
        p += ext;
    }
    if (info.width == 0 || info.height == 0 || info.width > MaxDim || info.height > MaxDim)
        return std::nullopt;
    info.data_offset = p;
    info.data_size = Mip0Size(info.format, info.width, info.height);
    if (info.data_size == 0 || info.data_size > end - p)
        return std::nullopt;
    return info;
}

std::optional<Image> DecodePixels(uint8_t format, uint32_t w, uint32_t h,
                                  const std::vector<uint8_t>& data) {
    const uint64_t need = Mip0Size(format, w, h);
    if (need == 0 || data.size() < need || w == 0 || h == 0 || w > MaxDim || h > MaxDim)
        return std::nullopt;
    Image img{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
    uint8_t* d = img.rgba.data();
    if (format == 0x00 || format == 0x01) {
        std::memcpy(d, data.data(), img.rgba.size());
        if (format == 0x01)
            for (size_t p = 0; p < img.rgba.size(); p += 4)
                std::swap(d[p], d[p + 2]);
        return img;
    }
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    const size_t bpb = format == 0x59 ? 8 : 16;
    for (uint32_t y = 0; y < by; ++y)
        for (uint32_t x = 0; x < bx; ++x) {
            const uint8_t* s = data.data() + (size_t(y) * bx + x) * bpb;
            uint8_t* o = d + (size_t(y) * 4 * w + size_t(x) * 4) * 4;
            switch (format) {
            case 0x59:
                bcn::DecodeBc1(s, o, x * 4, y * 4, w, h);
                break;
            case 0x5A:
                bcn::DecodeBc2(s, o, x * 4, y * 4, w, h);
                break;
            case 0x5B:
                bcn::DecodeBc3(s, o, x * 4, y * 4, w, h);
                break;
            case 0x5F:
                bcn::DecodeBc7(s, o, x * 4, y * 4, w, h);
                break;
            default:
                return std::nullopt;
            }
        }
    return img;
}

std::optional<Image> Decode(fe3h_romfs::Source& g, uint32_t index) {
    const auto t = Texture(g, index);
    if (!t)
        return std::nullopt;
    std::vector<uint8_t> px;
    if (!g.ReadVec(t->data_offset, static_cast<size_t>(t->data_size), px))
        return std::nullopt;
    return DecodePixels(t->format, t->width, t->height, px);
}

} // namespace fe3h_g1t
