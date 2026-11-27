// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstring>
#include <span>
#include <string_view>
#include <vector>
namespace pla {
// Plain little-endian SARC only. No packed-UI/Oodle path is admitted.
inline std::vector<uint8_t> SarcTexture(std::span<const uint8_t> b) {
    const auto u16 = [&](size_t at) { return unsigned(b[at]) | unsigned(b[at + 1]) << 8; };
    const auto u32 = [&](size_t at) { return uint32_t(u16(at)) | uint32_t(u16(at + 2)) << 16; };
    if (b.size() < 32 || std::memcmp(b.data(), "SARC", 4) || u16(6) != 0xfeff)
        return {};
    const size_t header = u16(4), data = u32(12);
    if (header < 20 || header > b.size() - 12 || data > b.size() ||
        std::memcmp(b.data() + header, "SFAT", 4) || u16(header + 4) != 12)
        return {};
    const size_t count = u16(header + 6), nodes = header + 12;
    if (count > (b.size() - nodes) / 16)
        return {};
    const size_t sfnt = nodes + count * 16;
    if (sfnt > b.size() - 8 || std::memcmp(b.data() + sfnt, "SFNT", 4) || u16(sfnt + 4) != 8)
        return {};
    const size_t names = sfnt + 8;
    if (names > data)
        return {};
    for (size_t n = nodes; n < sfnt; n += 16) {
        const auto attr = u32(n + 4);
        if (!(attr & 0x01000000))
            continue;
        const size_t off = size_t(attr & 0xffffff) * 4;
        if (off >= data - names)
            return {};
        const char *name = reinterpret_cast<const char *>(b.data() + names + off);
        const auto *end = static_cast<const char *>(std::memchr(name, 0, data - names - off));
        if (!end)
            return {};
        const size_t begin = u32(n + 8), finish = u32(n + 12);
        if (begin > finish || finish > b.size() - data)
            return {};
        if (std::string_view(name, end - name) == "timg/__Combined.bntx")
            return {b.begin() + data + begin, b.begin() + data + finish};
    }
    return {};
}
} // namespace pla
