// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe3h_ui2d.h"

#include <cstring>

#include "bc_decoder.h"

namespace fe3h_ui2d {
namespace {

uint16_t U16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t U64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
bool Fits(std::span<const uint8_t> d, uint64_t off, uint64_t n) {
    return off <= d.size() && n <= d.size() - off;
}

struct Brti {
    std::string name;
    uint32_t tile{}, format{}, width{}, height{}, layout{}, image_size{};
    uint64_t mip0{};
};

bool ParseBrti(std::span<const uint8_t> d, uint64_t o, Brti& t) {
    if (!Fits(d, o, 0x78) || std::memcmp(d.data() + o, "BRTI", 4) != 0)
        return false;
    const uint8_t* b = d.data() + o;
    t.tile = U16(b + 0x12);
    t.format = U32(b + 0x1C);
    t.width = U32(b + 0x24);
    t.height = U32(b + 0x28);
    t.layout = U32(b + 0x34);
    t.image_size = U32(b + 0x50);
    const uint64_t name = U64(b + 0x60), mips = U64(b + 0x70);
    if (!Fits(d, name, 2) || !Fits(d, name + 2, U16(d.data() + name)) || !Fits(d, mips, 8))
        return false;
    t.name.assign(reinterpret_cast<const char*>(d.data() + name + 2), U16(d.data() + name));
    t.mip0 = U64(d.data() + mips);
    return Fits(d, t.mip0, t.image_size);
}

std::vector<Brti> Textures(std::span<const uint8_t> d) {
    std::vector<Brti> out;
    if (d.size() < 0x30 || std::memcmp(d.data(), "BNTX", 4) != 0)
        return out;
    const uint32_t n = U32(d.data() + 0x24);
    const uint64_t arr = U64(d.data() + 0x28);
    if (n > 4096 || !Fits(d, arr, uint64_t(n) * 8))
        return out;
    for (uint32_t k = 0; k < n; ++k) {
        Brti t;
        if (ParseBrti(d, U64(d.data() + arr + size_t(k) * 8), t))
            out.push_back(std::move(t));
    }
    return out;
}

} // namespace

std::vector<SarcFile> Sarc(std::span<const uint8_t> d) {
    std::vector<SarcFile> out;
    if (d.size() < 0x20 || std::memcmp(d.data(), "SARC", 4) != 0 ||
        std::memcmp(d.data() + 0x14, "SFAT", 4) != 0)
        return out;
    const uint32_t data = U32(d.data() + 0x0C);
    const uint16_t n = U16(d.data() + 0x1A);
    const uint64_t names = 0x20 + uint64_t(n) * 16;
    if (!Fits(d, names, 8) || std::memcmp(d.data() + names, "SFNT", 4) != 0 || data > d.size())
        return out;
    for (uint16_t k = 0; k < n; ++k) {
        const uint8_t* r = d.data() + 0x20 + size_t(k) * 16;
        const uint32_t attr = U32(r + 4), begin = U32(r + 8), end = U32(r + 12);
        if (!(attr & 0x01000000) || end < begin || !Fits(d, uint64_t(data) + begin, end - begin))
            continue;
        const uint64_t no = names + 8 + uint64_t(attr & 0xFFFF) * 4;
        if (no >= d.size())
            continue;
        const size_t len = strnlen(reinterpret_cast<const char*>(d.data() + no), d.size() - no);
        if (no + len >= d.size())
            continue;
        out.push_back({std::string(reinterpret_cast<const char*>(d.data() + no), len),
                       size_t(data) + begin, size_t(end - begin)});
    }
    return out;
}

std::vector<uint8_t> Deswizzle(std::span<const uint8_t> src, uint32_t w_el, uint32_t h_el,
                               uint32_t bpe, uint32_t bh) {
    const size_t row = size_t(w_el) * bpe;
    const size_t gx = (row + 63) / 64;
    const size_t gb = 512 * size_t(bh);
    std::vector<uint8_t> out(row * h_el);
    for (size_t y = 0; y < h_el; ++y) {
        const size_t yb = (y / (8 * bh)) * gb * gx + ((y % (8 * bh)) / 8) * 512 + ((y % 8) / 2) * 64 +
                          (y % 2) * 16;
        for (size_t xb = 0; xb < row; xb += 16) {
            const size_t a = yb + (xb / 64) * gb + ((xb % 64) / 32) * 256 + ((xb % 32) / 16) * 32;
            const size_t n = std::min<size_t>(16, row - xb);
            if (a + n > src.size())
                return {};
            std::memcpy(out.data() + y * row + xb, src.data() + a, n);
        }
    }
    return out;
}

std::optional<Texture> BntxDecode(std::span<const uint8_t> d, std::string_view name) {
    for (const Brti& t : Textures(d)) {
        const std::string_view base = std::string_view{t.name}.substr(0, t.name.find('^'));
        if (t.name != name && base != name)
            continue;
        const uint32_t type = t.format >> 8;
        uint32_t bpe;
        switch (type) {
        case 0x1A: // BC1
        case 0x1D: // BC4
            bpe = 8;
            break;
        case 0x1C: // BC3
        case 0x1E: // BC5
            bpe = 16;
            break;
        default:
            return std::nullopt;
        }
        if (t.width == 0 || t.height == 0 || t.width > MaxDim || t.height > MaxDim)
            return std::nullopt;
        const uint32_t we = (t.width + 3) / 4, he = (t.height + 3) / 4;
        uint32_t bh = 1u << (t.layout & 7);
        while (bh > 1 && bh * 8 >= 2 * he)
            bh /= 2;
        const std::span<const uint8_t> raw = d.subspan(t.mip0, t.image_size);
        std::vector<uint8_t> lin;
        if (t.tile == 0) {
            lin = Deswizzle(raw, we, he, bpe, bh);
        } else if (raw.size() >= size_t(we) * he * bpe) {
            lin.assign(raw.begin(), raw.begin() + size_t(we) * he * bpe);
        }
        if (lin.size() != size_t(we) * he * bpe)
            return std::nullopt;
        // Decode the whole block grid, then crop to the texture size (as the reference does).
        const uint32_t fw = we * 4, fh = he * 4;
        const uint32_t ch = type == 0x1D ? 1 : type == 0x1E ? 2 : 4;
        std::vector<uint8_t> plane(size_t(fw) * fh * ch);
        for (uint32_t by = 0; by < he; ++by)
            for (uint32_t bx = 0; bx < we; ++bx) {
                const uint8_t* s = lin.data() + (size_t(by) * we + bx) * bpe;
                uint8_t* o = plane.data() + (size_t(by) * 4 * fw + size_t(bx) * 4) * ch;
                switch (type) {
                case 0x1A:
                    bcn::DecodeBc1(s, o, bx * 4, by * 4, fw, fh);
                    break;
                case 0x1C:
                    bcn::DecodeBc3(s, o, bx * 4, by * 4, fw, fh);
                    break;
                case 0x1D:
                    bcn::DecodeBc4(s, o, bx * 4, by * 4, fw, fh, false);
                    break;
                default:
                    bcn::DecodeBc5(s, o, bx * 4, by * 4, fw, fh, false);
                    break;
                }
            }
        Texture out{t.name, t.width, t.height, std::vector<uint8_t>(size_t(t.width) * t.height * 4)};
        for (uint32_t y = 0; y < t.height; ++y)
            for (uint32_t x = 0; x < t.width; ++x) {
                const uint8_t* p = plane.data() + (size_t(y) * fw + x) * ch;
                uint8_t* q = out.rgba.data() + (size_t(y) * t.width + x) * 4;
                if (ch == 4) {
                    std::memcpy(q, p, 4);
                } else if (ch == 2) {
                    q[0] = q[1] = q[2] = p[0];
                    q[3] = p[1];
                } else {
                    q[0] = q[1] = q[2] = p[0];
                    q[3] = 0xFF;
                }
            }
        return out;
    }
    return std::nullopt;
}

} // namespace fe3h_ui2d
