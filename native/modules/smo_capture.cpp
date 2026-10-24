// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_capture.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace SmoCapture {
namespace {
using Span = std::span<const uint8_t>;
bool Fits(Span b, uint64_t p, uint64_t n) { return p <= b.size() && n <= b.size() - p; }
uint16_t U16(Span b, size_t p) { return uint16_t(b[p]) | uint16_t(b[p + 1]) << 8; }
uint32_t U32(Span b, size_t p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= uint32_t(b[p + i]) << (8 * i);
    return v;
}
uint64_t U64(Span b, size_t p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= uint64_t(b[p + i]) << (8 * i);
    return v;
}
// Bounded port of mk8d_assets.cpp's Yaz0 recipe; overlapping backreferences are valid.
constexpr size_t MaxArchive = 64 * 1024 * 1024;
std::optional<std::vector<uint8_t>> ExpandYaz0(Span in) {
    if (in.size() < 16) return std::nullopt;
    const uint32_t size = uint32_t(in[4]) << 24 | uint32_t(in[5]) << 16 |
                          uint32_t(in[6]) << 8 | uint32_t(in[7]);
    if (!size || size > MaxArchive) return std::nullopt;
    std::vector<uint8_t> out(size);
    size_t si = 16, di = 0;
    uint8_t code = 0, mask = 0;
    while (di < out.size()) {
        if (!mask) {
            if (si >= in.size()) return std::nullopt;
            code = in[si++]; mask = 0x80;
        }
        if (code & mask) {
            if (si >= in.size()) return std::nullopt;
            out[di++] = in[si++];
        } else {
            if (!Fits(in, si, 2)) return std::nullopt;
            const uint8_t a = in[si++], b = in[si++];
            const size_t distance = ((a & 15) << 8) | b;
            if (distance + 1 > di) return std::nullopt;
            size_t count = a >> 4;
            if (!count) {
                if (si >= in.size()) return std::nullopt;
                count = size_t(in[si++]) + 0x12;
            } else {
                count += 2;
            }
            if (count > out.size() - di) return std::nullopt;
            size_t source = di - distance - 1;
            while (count--) out[di++] = out[source++];
        }
        mask >>= 1;
    }
    return out;
}
// BFRES 8.0 external-file records, with file-relative offsets. Never search for magic.
std::optional<Span> EmbeddedBntx(Span b) {
    if (b.size() < 0xd0 || std::memcmp(b.data(), "FRES", 4) ||
        U32(b, 8) != 0x00080000 || U16(b, 0xc) != 0xfeff || U32(b, 0x1c) != b.size())
        return std::nullopt;
    const auto table = U64(b, 0x98);
    const auto count = U16(b, 0xc8);
    if (!count || count > 64 || !Fits(b, table, uint64_t(count) * 16))
        return std::nullopt;
    std::optional<Span> found;
    for (unsigned i = 0; i < count; ++i) {
        const auto p = U64(b, size_t(table) + i * 16);
        const auto n = U64(b, size_t(table) + i * 16 + 8);
        if (n < 4 || !Fits(b, p, n)) return std::nullopt;
        if (!std::memcmp(b.data() + p, "BNTX", 4)) {
            if (found) return std::nullopt;
            found = b.subspan(size_t(p), size_t(n));
        }
    }
    return found;
}

struct Container {
    std::vector<uint8_t> expanded;
    Span bntx;
};
std::optional<Container> Open(Span archive) {
    Container out;
    if (archive.size() >= 4 && !std::memcmp(archive.data(), "Yaz0", 4)) {
        auto decoded = ExpandYaz0(archive);
        if (!decoded) return std::nullopt;
        out.expanded = std::move(*decoded);
        archive = out.expanded;
    }
    if (archive.size() > MaxArchive) return std::nullopt;
    Span bfres;
    for (const auto& f : fe3h_ui2d::Sarc(archive)) {
        if (f.name != "HackListTexture.bfres") continue;
        if (!bfres.empty() || !Fits(archive, f.offset, f.size)) return std::nullopt;
        bfres = archive.subspan(f.offset, f.size);
    }
    const auto bntx = EmbeddedBntx(bfres);
    if (!bntx) return std::nullopt;
    out.bntx = *bntx;
    return out;
}
struct Portrait {
    std::string name;
    uint32_t tile{}, layout{}, selectors{}, size{};
    uint64_t image{};
};
std::optional<std::vector<Portrait>> Portraits(Span b) {
    if (b.size() < 0x30 || std::memcmp(b.data(), "BNTX", 4)) return std::nullopt;
    const auto count = U32(b, 0x24);
    const auto table = U64(b, 0x28);
    if (!count || count > 256 || !Fits(b, table, uint64_t(count) * 8)) return std::nullopt;
    std::vector<Portrait> rows;
    for (uint32_t i = 0; i < count; ++i) {
        const auto p = U64(b, size_t(table) + i * 8);
        if (!Fits(b, p, 0x78) || std::memcmp(b.data() + p, "BRTI", 4)) return std::nullopt;
        const auto t = b.subspan(size_t(p));
        // The actual registry is one layer of 256² ASTC4x4 sRGB portraits.
        if (U32(t, 0x1c) != 0x2d06 || U32(t, 0x24) != 256 || U32(t, 0x28) != 256 ||
            U32(t, 0x2c) != 1 || U32(t, 0x30) != 1 || U16(t, 0x16) < 1 ||
            U16(t, 0x12) > 1 || (U32(t, 0x34) & 7) > 5) return std::nullopt;
        const auto name = U64(t, 0x60), mips = U64(t, 0x70);
        if (!Fits(b, name, 2) || !Fits(b, mips, 8)) return std::nullopt;
        const auto len = U16(b, size_t(name));
        if (!len || len > 128 || !Fits(b, name + 2, len)) return std::nullopt;
        Portrait row;
        row.name.assign(reinterpret_cast<const char*>(b.data() + name + 2), len);
        if (row.name.find_first_of("/\\#") != row.name.npos ||
            row.name.find('\0') != row.name.npos ||
            std::any_of(rows.begin(), rows.end(), [&](const Portrait& q) { return q.name == row.name; }))
            return std::nullopt;
        row.tile = U16(t, 0x12); row.layout = U32(t, 0x34) & 7;
        row.selectors = U32(t, 0x58); row.size = U32(t, 0x50);
        row.image = U64(b, size_t(mips));
        if (row.size < 65536 || !Fits(b, row.image, row.size)) return std::nullopt;
        for (unsigned c = 0; c < 4; ++c)
            if (((row.selectors >> (8 * c)) & 255) > 5) return std::nullopt;
        rows.push_back(std::move(row));
    }
    return rows;
}
} // namespace

std::vector<std::string> Names(Span archive) {
    const auto container = Open(archive);
    if (!container) return {};
    const auto portraits = Portraits(container->bntx);
    if (!portraits) return {};
    std::vector<std::string> names;
    for (const auto& p : *portraits) names.push_back(p.name);
    return names;
}

std::optional<fe3h_ui2d::Texture> DecodeArchive(Span archive, std::string_view capture,
                                              void* userdata, EdenDsmodAstcDecoder decode) {
    if (!decode || capture.empty()) return std::nullopt;
    const auto container = Open(archive);
    if (!container) return std::nullopt;
    const auto portraits = Portraits(container->bntx);
    if (!portraits) return std::nullopt;
    const auto found = std::find_if(portraits->begin(), portraits->end(),
                                  [&](const Portrait& p) { return p.name == capture; });
    if (found == portraits->end()) return std::nullopt;
    const auto raw = container->bntx.subspan(size_t(found->image), found->size);
    std::vector<uint8_t> blocks;
    if (found->tile == 0) {
        uint32_t bh = 1u << found->layout;
        while (bh > 1 && bh * 8 >= 128) bh /= 2;
        blocks = fe3h_ui2d::Deswizzle(raw, 64, 64, 16, bh);
    } else {
        blocks.assign(raw.begin(), raw.begin() + 65536);
    }
    if (blocks.size() != 65536) return std::nullopt;
    fe3h_ui2d::Texture out{std::string(capture), 256, 256, std::vector<uint8_t>(256 * 256 * 4)};
    if (!decode(userdata, 256, 256, 4, 4, blocks.data(), blocks.size(),
                out.rgba.data(), out.rgba.size())) return std::nullopt;
    for (size_t p = 0; p < out.rgba.size(); p += 4) {
        const std::array<uint8_t, 4> rgba{out.rgba[p], out.rgba[p + 1], out.rgba[p + 2], out.rgba[p + 3]};
        for (unsigned c = 0; c < 4; ++c) {
            const auto selector = (found->selectors >> (8 * c)) & 255;
            out.rgba[p + c] = selector == 0 ? 0 : selector == 1 ? 255 : rgba[selector - 2];
        }
    }
    return out;
}
} // namespace SmoCapture
