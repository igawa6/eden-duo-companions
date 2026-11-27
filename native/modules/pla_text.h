// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace pla {
inline void Utf8(std::string &out, unsigned cp) {
    if (cp < 0x80)
        out += char(cp);
    else if (cp < 0x800) {
        out += char(0xc0 | (cp >> 6));
        out += char(0x80 | (cp & 63));
    } else if (cp >= 0x10000) {
        out += char(0xf0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 63));
        out += char(0x80 | ((cp >> 6) & 63));
        out += char(0x80 | (cp & 63));
    } else {
        out += char(0xe0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 63));
        out += char(0x80 | (cp & 63));
    }
}
// PLA message section 0, same rotating XOR as the private research reader.
// Only plain place names are accepted; control tokens and surrogate units
// refuse the row.
inline std::string Message(std::span<const uint8_t> bytes, unsigned row, bool multiline = false) {
    auto u16 = [&](size_t at) { return unsigned(bytes[at]) | unsigned(bytes[at + 1]) << 8; };
    auto u32 = [&](size_t at) { return uint32_t(u16(at)) | uint32_t(u16(at + 2)) << 16; };
    if (bytes.size() < 16 || row >= u16(2))
        return {};
    const size_t section = u32(12);
    if (section > bytes.size() || bytes.size() - section < 4 ||
        row >= (bytes.size() - section - 4) / 8)
        return {};
    const size_t entry = section + 4 + row * 8;
    const int32_t offset = int32_t(u32(entry));
    if (offset < 0 || size_t(offset) > bytes.size() - section)
        return {};
    const size_t start = section + offset, count = u16(entry + 4);
    if (count > (multiline ? 1024u : 256u) || count > (bytes.size() - start) / 2)
        return {};
    uint16_t key = uint16_t(0x7c89 + row * 0x2983);
    std::string text;
    for (size_t j = 0; j < count; ++j) {
        const unsigned cp = u16(start + j * 2) ^ key;
        key = uint16_t((key << 3) | (key >> 13));
        if (!cp)
            return text;
        if ((cp < 32 && !(multiline && cp == 10)) || (cp >= 0xd800 && cp <= 0xdfff))
            return {};
        Utf8(text, cp);
    }
    return {}; // no terminator
}
// AHTB label hashes select DAT rows. Labels themselves are never shown.
inline std::string MessageByHash(std::span<const uint8_t> dat, std::span<const uint8_t> tbl,
                                 uint64_t wanted, bool multiline = false) {
    const auto u16=[&](size_t at){return unsigned(tbl[at]) | unsigned(tbl[at+1])<<8;};
    const auto u32=[&](size_t at){return uint32_t(u16(at)) | uint32_t(u16(at+2))<<16;};
    if (!wanted || tbl.size()<8 || tbl.size()>1024*1024 || u32(0)!=0x42544841) return {};
    const auto count=u32(4);
    if (count>8192 || count>(tbl.size()-8)/10) return {};
    size_t at=8;
    unsigned found=count;
    for (unsigned row=0; row<count; ++row) {
        if (at>tbl.size() || tbl.size()-at<10) return {};
        const uint64_t hash=uint64_t(u32(at)) | uint64_t(u32(at+4))<<32;
        const size_t length=u16(at+8);
        at+=10;
        if (!length || length>1024 || length>tbl.size()-at) return {};
        if (hash==wanted) {
            if (found!=count) return {};
            found=row;
        }
        at+=length;
    }
    return found==count ? std::string{} : Message(dat,found,multiline);
}
} // namespace pla
