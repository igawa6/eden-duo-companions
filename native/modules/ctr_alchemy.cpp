// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ctr_alchemy.h"

#include "ctr_lzma.h"

#include <bc_decoder.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace CtrAlchemy {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 Block = 0x8000;
constexpr u32 FormatBc3 = 0x37456ecdu;
constexpr u32 FormatLa8 = 0x44e04333u;

u16 Rd16(const Bytes& b, std::size_t at) {
    return at + 2 <= b.size() ? static_cast<u16>(b[at] | b[at + 1] << 8) : 0;
}
u32 Rd32(const Bytes& b, std::size_t at) {
    return at + 4 <= b.size() ? static_cast<u32>(b[at] | b[at + 1] << 8 | b[at + 2] << 16 |
                                                 static_cast<u32>(b[at + 3]) << 24)
                              : 0;
}
u64 Rd64(const Bytes& b, std::size_t at) {
    return u64{Rd32(b, at)} | u64{Rd32(b, at + 4)} << 32;
}
float RdF(const Bytes& b, std::size_t at) {
    const u32 v = Rd32(b, at);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

/// The parts of an IGZ v10 file this module uses.
struct Igz {
    struct Section {
        u32 offset, size;
    };
    std::vector<Section> data; ///< data sections (pointer section indices)
    std::vector<std::string> types;
    std::vector<u32> exid; ///< EXID name hashes
    const Bytes* bytes{};

    bool Parse(const Bytes& b) {
        bytes = &b;
        if (b.size() < 0x24 || Rd32(b, 0) != 0x49475A01u || Rd32(b, 4) != 10)
            return false;
        const u32 nsec = Rd32(b, 0x0C);
        if (nsec == 0 || nsec > 16 || 0x14 + 16 * (nsec + 1) > b.size())
            return false;
        const u32 fix_off = Rd32(b, 0x14 + 4), fix_len = Rd32(b, 0x14 + 8);
        for (u32 i = 1; i <= nsec; ++i) {
            const u32 off = Rd32(b, 0x14 + 16 * i + 4), size = Rd32(b, 0x14 + 16 * i + 8);
            if (u64{off} + size > b.size())
                return false;
            data.push_back({off, size});
        }
        if (u64{fix_off} + fix_len > b.size())
            return false;
        for (u32 p = fix_off; p + 16 <= fix_off + fix_len;) {
            const u32 count = Rd32(b, p + 4), size = Rd32(b, p + 8), start = Rd32(b, p + 12);
            if (size < 16 || start > size || u64{p} + size > fix_off + u64{fix_len})
                return false;
            const char* tag = reinterpret_cast<const char*>(b.data() + p);
            if (std::memcmp(tag, "TMET", 4) == 0) {
                u32 q = p + start;
                for (u32 i = 0; i < count && q < p + size; ++i) {
                    const auto end = std::find(b.begin() + q, b.begin() + p + size, u8{0});
                    types.emplace_back(reinterpret_cast<const char*>(b.data() + q),
                                       static_cast<std::size_t>(end - (b.begin() + q)));
                    q = static_cast<u32>(end - b.begin()) + 1;
                    q += q & 1;
                }
            } else if (std::memcmp(tag, "EXID", 4) == 0) {
                for (u32 i = 0; i < count && p + start + 8 * i + 8 <= p + size; ++i)
                    exid.push_back(Rd32(b, p + start + 8 * i));
            }
            p += size;
        }
        return !data.empty();
    }
    int Type(std::string_view name) const {
        for (std::size_t i = 0; i < types.size(); ++i)
            if (types[i] == name)
                return static_cast<int>(i);
        return -1;
    }
    /// Offsets (in the object section) of every object whose type index equals `type`.
    std::vector<u32> Objects(int type, u32 min_size) const {
        std::vector<u32> out;
        const auto& s = data[0];
        for (u32 off = 0; off + min_size <= s.size; off += 8)
            if (Rd64(*bytes, s.offset + off) == static_cast<u64>(type))
                out.push_back(s.offset + off);
        return out;
    }
    /// A data pointer -> file offset (0 when out of range).
    u32 Resolve(u32 ptr, u32 size) const {
        const u32 sec = ptr >> 27, off = ptr & 0x7FFFFFF;
        if (sec >= data.size() || u64{off} + size > data[sec].size)
            return 0;
        return data[sec].offset + off;
    }
};

} // namespace

bool Archive::Open(const ReadAt& read) {
    entries.clear();
    u8 head[0x38];
    if (!read || read(0, head, sizeof head) != sizeof head || std::memcmp(head, "IGA\x1a", 4) != 0)
        return false;
    Bytes h(head, head + sizeof head);
    if (Rd32(h, 4) != 13)
        return false;
    const u32 count = Rd32(h, 0x0C);
    sector = Rd32(h, 0x10);
    const u64 names = Rd64(h, 0x28);
    if (count == 0 || count > 100000 || sector == 0 || (sector & (sector - 1)))
        return false;
    Bytes toc(16ull * count);
    if (read(0x38 + 4ull * count, toc.data(), toc.size()) != toc.size())
        return false;
    Bytes offs(4ull * count);
    if (read(names, offs.data(), offs.size()) != offs.size())
        return false;
    entries.resize(count);
    for (u32 i = 0; i < count; ++i) {
        Entry& e = entries[i];
        e.offset = Rd32(toc, 16 * i);
        e.length = Rd32(toc, 16 * i + 8);
        e.mode = Rd32(toc, 16 * i + 12);
        // name record: "<full path>\0<short path>\0"
        char buf[512];
        const std::size_t n = read(names + Rd32(offs, 4 * i), buf, sizeof buf);
        const char* end = static_cast<const char*>(std::memchr(buf, 0, n));
        if (!end)
            return false;
        const char* s = end + 1;
        const char* end2 = static_cast<const char*>(std::memchr(s, 0, n - (s - buf)));
        if (!end2)
            return false;
        e.name.assign(s, end2);
    }
    return true;
}

const Entry* Archive::FindExact(std::string_view name) const {
    for (const Entry& e : entries)
        if (e.name == name)
            return &e;
    return nullptr;
}

const Entry* Archive::Find(std::initializer_list<std::string_view> fragments) const {
    for (const Entry& e : entries) {
        bool all = true;
        for (const auto f : fragments)
            all = all && e.name.find(f) != std::string::npos;
        if (all)
            return &e;
    }
    return nullptr;
}

bool Archive::Read(const Entry& e, const ReadAt& read, Bytes& out) const {
    out.clear();
    if (e.length > (256u << 20))
        return false;
    const u32 kind = e.mode >> 28;
    if (e.mode == 0xFFFFFFFFu || kind == 0xF) {
        out.resize(e.length);
        return read(e.offset, out.data(), out.size()) == out.size();
    }
    if (kind != 2)
        return false;
    out.reserve(e.length);
    u64 pos = e.offset;
    Bytes packed, block;
    while (out.size() < e.length) {
        const std::size_t want = std::min<std::size_t>(Block, e.length - out.size());
        u8 head[9];
        if (read(pos, head, 9) != 9)
            return false;
        const u32 csize = head[0] | head[1] << 8 | head[2] << 16 | static_cast<u32>(head[3]) << 24;
        if (csize == 0 || csize > Block + 0x100) { // stored block
            const std::size_t at = out.size();
            out.resize(at + want);
            if (read(pos, out.data() + at, want) != want)
                return false;
            pos += want;
        } else {
            packed.resize(csize);
            if (read(pos + 9, packed.data(), csize) != csize ||
                !CtrLzma::Decode(head + 4, packed.data(), csize, want, block) ||
                block.size() != want)
                return false;
            out.insert(out.end(), block.begin(), block.end());
            pos += 9 + csize;
        }
        pos = (pos + sector - 1) / sector * sector;
    }
    return true;
}

std::optional<Image> DecodeImage(const Bytes& b) {
    Igz z;
    if (!z.Parse(b))
        return std::nullopt;
    const int type = z.Type("igImage2");
    if (type < 0)
        return std::nullopt;
    for (const u32 o : z.Objects(type, 0x48)) {
        const u32 w = Rd16(b, o + 0x18), h = Rd16(b, o + 0x1A);
        const u32 fmt_ref = Rd32(b, o + 0x2C), size = Rd32(b, o + 0x38), ptr = Rd32(b, o + 0x40);
        if (!w || !h || w > 8192 || h > 8192 || !size)
            continue;
        const u32 at = z.Resolve(ptr, size);
        if (!at || !(fmt_ref & 0x80000000u) || (fmt_ref & 0x7FFFFFFF) >= z.exid.size())
            continue;
        const u32 fmt = z.exid[fmt_ref & 0x7FFFFFFF];
        Image img;
        img.width = w;
        img.height = h;
        Bytes rgba(std::size_t{w} * h * 4);
        if (fmt == FormatBc3) {
            const u32 bx = (w + 3) / 4, by = (h + 3) / 4;
            if (std::size_t{bx} * by * 16 > size)
                continue;
            for (u32 y = 0; y < by; ++y)
                for (u32 x = 0; x < bx; ++x)
                    bcn::DecodeBc3(b.data() + at + (std::size_t{y} * bx + x) * 16,
                                   rgba.data() + (std::size_t{y} * 4 * w + x * 4) * 4, x * 4, y * 4,
                                   w, h);
        } else if (fmt == FormatLa8) {
            if (std::size_t{w} * h * 2 > size)
                continue;
            for (std::size_t i = 0; i < std::size_t{w} * h; ++i) {
                const u8 l = b[at + 2 * i], a = b[at + 2 * i + 1];
                rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = l;
                rgba[4 * i + 3] = a;
            }
        } else {
            continue;
        }
        // stored bottom-up
        img.rgba.resize(rgba.size());
        const std::size_t row = std::size_t{w} * 4;
        for (u32 y = 0; y < h; ++y)
            std::memcpy(img.rgba.data() + y * row, rgba.data() + (h - 1 - y) * row, row);
        return img;
    }
    return std::nullopt;
}

std::optional<Font> DecodeFont(const Bytes& b) {
    Igz z;
    if (!z.Parse(b))
        return std::nullopt;
    const int metrics = z.Type("igCharMetrics"), image = z.Type("igImage2");
    if (metrics < 0 || image < 0)
        return std::nullopt;
    Font f;
    for (const u32 o : z.Objects(metrics, 0x48)) {
        const u32 cp = Rd32(b, o + 0x0C);
        if (cp >= 0x110000 || f.glyphs.count(cp))
            continue;
        Glyph g;
        g.kern_left = RdF(b, o + 0x10);
        g.step = RdF(b, o + 0x18);
        g.off_x = RdF(b, o + 0x1C);
        g.off_y = RdF(b, o + 0x20);
        g.w = RdF(b, o + 0x24);
        g.h = RdF(b, o + 0x28);
        g.u0 = RdF(b, o + 0x34);
        g.v0 = RdF(b, o + 0x38);
        g.u1 = RdF(b, o + 0x3C);
        g.v1 = RdF(b, o + 0x40);
        // metrics from romfs (a LayeredFS mod may replace the font): all finite and in range
        const auto sane = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
        if (!sane(g.step, -4096, 4096) || !sane(g.kern_left, -4096, 4096) || !sane(g.off_x, -4096, 4096) ||
            !sane(g.off_y, -4096, 4096) || !sane(g.w, 0, 4096) || !sane(g.h, 0, 4096) || !sane(g.u0, 0, 1) ||
            !sane(g.v0, 0, 1) || !sane(g.u1, g.u0, 1) || !sane(g.v1, g.v0, 1))
            continue;
        f.cell_height = std::max(f.cell_height, g.h);
        f.glyphs.emplace(cp, g);
    }
    for (const u32 o : z.Objects(image, 0x48)) {
        const u32 w = Rd16(b, o + 0x18), h = Rd16(b, o + 0x1A);
        const u32 fmt_ref = Rd32(b, o + 0x2C), size = Rd32(b, o + 0x38), ptr = Rd32(b, o + 0x40);
        if (!w || !h || !(fmt_ref & 0x80000000u) || (fmt_ref & 0x7FFFFFFF) >= z.exid.size() ||
            z.exid[fmt_ref & 0x7FFFFFFF] != FormatLa8 || std::size_t{w} * h * 2 > size)
            continue;
        const u32 at = z.Resolve(ptr, size);
        if (!at)
            continue;
        f.atlas_w = w;
        f.atlas_h = h;
        f.atlas_la.assign(b.begin() + at, b.begin() + at + std::size_t{w} * h * 2);
        break;
    }
    if (f.glyphs.empty() || f.atlas_la.empty() || f.cell_height <= 0)
        return std::nullopt;
    return f;
}

std::optional<MinimapParams> ParseMinimap(const Bytes& b) {
    Igz z;
    if (!z.Parse(b))
        return std::nullopt;
    const auto& s = z.data[0];
    for (u32 i = 0x100; i + 0x30 <= s.size; i += 4) {
        const float w = RdF(b, s.offset + i), h = RdF(b, s.offset + i + 4);
        if (w >= 50 && w <= 3000 && h >= 50 && h <= 3000 && w == std::floor(w) &&
            h == std::floor(h)) {
            MinimapParams p;
            p.w = w;
            p.h = h;
            for (int k = 0; k < 4; ++k)
                p.v[k] = RdF(b, s.offset + i + 8 + 4 * k);
            p.direction = static_cast<std::int32_t>(Rd32(b, s.offset + i + 0x2C));
            if (p.direction < 0 || p.direction > 3 || p.v[0] == p.v[2] || p.v[1] == p.v[3])
                return std::nullopt;
            return p;
        }
    }
    return std::nullopt;
}

void ProjectToTexture(const MinimapParams& p, float x, float y, float tex, float& px, float& py) {
    const float v0 = p.v[0], v1 = p.v[1], v2 = p.v[2], v3 = p.v[3];
    float a, b;
    switch (p.direction) {
    case 3:
        a = (x - v2) / (v0 - v2);
        b = (v3 - y) / (v3 - v1);
        break;
    case 2:
        a = (v0 - x) / (v0 - v2);
        b = (y - v1) / (v3 - v1);
        break;
    case 1:
        a = (y - v2) / (v0 - v2);
        b = (x - v1) / (v3 - v1);
        break;
    default:
        a = (v0 - y) / (v0 - v2);
        b = (v3 - x) / (v3 - v1);
        break;
    }
    px = tex / 2 + 2.0f * (a - 0.5f) * p.w * (tex / 800.0f);
    py = tex / 2 + 2.0f * (b - 0.5f) * p.h * (tex / 800.0f);
}

} // namespace CtrAlchemy
