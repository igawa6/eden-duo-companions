// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// GameFreak message-table decoder (AHTB .tbl label table + the encrypted per-line .dat
// container), ported from this cycle's text-decoding research script, itself ported from the
// public pkNX project (kwsch/pkNX TextFile.cs) via its Python port (Digote/pokedat). Verified
// this cycle against a real romfs sample (bin/message/English/common/field_network.dat):
// header fields and the first line's ciphertext offset matched byte-for-byte before any
// decrypt was attempted, and decrypted text came out readable English end to end.
//
// This header decodes bytes already read into memory -- it does no romfs I/O of its own. A
// caller gets the bytes via the host's read_romfs (EDEN_DSMOD_CAP_ROMFS_READ), so the package
// ships no extracted text: every string is decoded from the player's own romfs at runtime
// (PORTING_A_GAME.md section 4, "Asset-free packages" -- "Text" row). Only `[~ N]`-free lines
// decode to a literal string; the Rotom-menu `xmenu*.dat` tables resolve to `[~ N]` indirection
// whose target table was not identified this cycle, and are returned as that placeholder.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace sw_text {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u16 kBaseKey = 0x7C89;
constexpr u16 kAdvanceKey = 0x2983;
constexpr u16 kVariableKey = 0x0010;
constexpr u16 kTerminatorKey = 0x0000;
constexpr u32 kTblMagic = 0x42544841; // "AHTB" read little-endian

inline u16 Rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline u32 Rd32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
struct TblEntry {
    std::string name; // cp1252, nul-trimmed
    u64 hash = 0;
};

/// Parses a whole .tbl file already in memory. Returns false on a bad magic (caller should
/// still try to show the .dat's strings positionally, since the .tbl and .dat line up
/// positionally by index).
inline bool ReadTbl(const u8* data, std::size_t size, std::vector<TblEntry>& out) {
    out.clear();
    if (size < 8) return false;
    if (Rd32(data) != kTblMagic) return false;
    const u32 count = Rd32(data + 4);
    std::size_t off = 8;
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        if (off + 10 > size) return false;
        TblEntry e;
        e.hash = static_cast<u64>(Rd32(data + off)) | (static_cast<u64>(Rd32(data + off + 4)) << 32);
        const u16 name_len = Rd16(data + off + 8);
        off += 10;
        if (off + name_len > size) return false;
        std::size_t trimmed = name_len;
        while (trimmed > 0 && data[off + trimmed - 1] == 0) --trimmed;
        e.name.assign(reinterpret_cast<const char*>(data + off), trimmed);
        off += name_len;
        out.push_back(std::move(e));
    }
    return true;
}

inline u16 RotateLeft3(u16 key) {
    return static_cast<u16>((key << 3) | (key >> 13));
}

/// One line's ciphertext (length*2 bytes, little-endian u16 units), decrypted in place with the
/// per-line starting key.
inline void DecryptU16Stream(u8* data, std::size_t size, u16 key) {
    for (std::size_t i = 0; i + 1 < size; i += 2) {
        const u16 v = static_cast<u16>(Rd16(data + i) ^ key);
        data[i] = static_cast<u8>(v);
        data[i + 1] = static_cast<u8>(v >> 8);
        key = RotateLeft3(key);
    }
}

/// Parses one decrypted line's u16 token stream into a display string. Control codes decode to
/// the same bracketed placeholders a reference text-decoding script prints (`\n`, `[WAIT n]`, `[~ n]`, …) rather than
/// being silently dropped, so a page can at least show that a line has an unresolved variable
/// instead of truncated or wrong text.
inline std::string ParseLineString(const u8* data, std::size_t size) {
    std::string s;
    std::size_t i = 0;
    while (i + 1 < size) {
        const u16 val = Rd16(data + i);
        i += 2;
        if (val == kTerminatorKey) break;
        if (val == kVariableKey) {
            if (i + 4 > size) break;
            const u16 var = Rd16(data + i + 2);
            i += 4;
            if (var == 0xBE00) { s += "\\r"; }
            else if (var == 0xBE01) { s += "\\c"; }
            else if (var == 0xBE02) {
                if (i + 2 > size) break;
                char buf[24];
                std::snprintf(buf, sizeof buf, "[WAIT %u]", Rd16(data + i));
                s += buf;
                i += 2;
            } else if (var == 0xBDFF) {
                if (i + 2 > size) break;
                char buf[24];
                std::snprintf(buf, sizeof buf, "[~ %u]", Rd16(data + i));
                s += buf;
                i += 2;
            } else {
                char buf[16];
                std::snprintf(buf, sizeof buf, "[VAR %04X]", var);
                s += buf;
            }
        } else if (val == 0x0A) {
            s += "\\n";
        } else if ((val >= 0x20 && val < 0xD800) || val >= 0xE000) {
            // BMP code point, re-encoded as UTF-8 (ASCII fast path covers the common case).
            // French text puts U+202F (narrow no-break space) before ! ? : ; the package font
            // has no glyph for it (it would draw the alternate glyph), so it becomes U+00A0.
            const u16 cp = val == 0x202F ? u16{0x00A0} : val;
            if (cp < 0x80) {
                s += static_cast<char>(cp);
            } else if (cp < 0x800) {
                s += static_cast<char>(0xC0 | (cp >> 6));
                s += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                s += static_cast<char>(0xE0 | (cp >> 12));
                s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (cp & 0x3F));
            }
        } else {
            char buf[12];
            std::snprintf(buf, sizeof buf, "\\u%04X", val);
            s += buf;
        }
    }
    return s;
}

/// Decodes every line of a whole .dat file already in memory, in file order. Returns false on a
/// malformed header (initial_key != 0, section count != 1, or a length that disagrees with the
/// header's own total_length -- the exact checks already verified this cycle against a
/// real romfs sample before any decrypt was attempted).
inline bool ReadDat(const u8* data, std::size_t size, std::vector<std::string>& out) {
    out.clear();
    if (size < 16) return false;
    const u16 sections = Rd16(data);
    const u16 line_count = Rd16(data + 2);
    const u32 total_length = Rd32(data + 4);
    const u32 initial_key = Rd32(data + 8);
    const u32 sdo = Rd32(data + 0x0C);
    if (initial_key != 0 || sections != 1) return false;
    if (static_cast<u64>(sdo) + 4 > size) return false;
    const u32 section_length = Rd32(data + sdo);
    if (section_length != total_length) return false;
    const std::size_t base = static_cast<std::size_t>(sdo) + 4;
    u16 key = kBaseKey;
    out.reserve(line_count);
    for (u16 i = 0; i < line_count; ++i) {
        const std::size_t entry = base + static_cast<std::size_t>(i) * 8;
        if (entry + 8 > size) return false;
        const std::int32_t rel_off = static_cast<std::int32_t>(Rd32(data + entry));
        const u16 length_units = Rd16(data + entry + 4);
        const std::int64_t start = static_cast<std::int64_t>(sdo) + rel_off;
        const std::size_t byte_len = static_cast<std::size_t>(length_units) * 2;
        if (start < 0 || static_cast<u64>(start) + byte_len > size) return false;
        std::vector<u8> enc(data + start, data + start + byte_len);
        DecryptU16Stream(enc.data(), enc.size(), key);
        out.push_back(ParseLineString(enc.data(), enc.size()));
        key = static_cast<u16>(key + kAdvanceKey);
    }
    return true;
}

} // namespace sw_text
