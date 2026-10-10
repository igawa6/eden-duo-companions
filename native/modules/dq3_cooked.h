// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Minimal reader for cooked UE4.27 packages of Dragon Quest III HD-2D Remake (unversioned tagged
// properties): the package summary and name map of a .uasset, and property tags of its .uexp.
// Shared by dq3_sprites (job names) and dq3_tables (battle tables). Header-only, internal.
#pragma once

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dq3_font.h"
#include "dq3_pak.h"

namespace dq3::cooked {

using dq3::Le32;

struct Cursor {
    std::span<const u8> b;
    std::size_t o{};
    bool ok{true};
    bool Need(std::size_t n) {
        if (!ok || o > b.size() || n > b.size() - o)
            ok = false;
        return ok;
    }
    s32 I32() {
        if (!Need(4))
            return 0;
        const s32 v = static_cast<s32>(Le32(b.data() + o));
        o += 4;
        return v;
    }
    u8 U8() {
        if (!Need(1))
            return 0;
        return b[o++];
    }
    void Skip(std::size_t n) {
        if (Need(n))
            o += n;
    }
    /// FString: positive length = Latin-1 incl. NUL, negative = UTF-16 incl. NUL.
    std::optional<std::u32string> FString() {
        const s32 n = I32();
        if (!ok || n == 0)
            return ok ? std::optional<std::u32string>{std::u32string{}} : std::nullopt;
        std::u32string out;
        if (n > 0) {
            if (n > 4096 || !Need(static_cast<std::size_t>(n)) || b[o + n - 1] != 0)
                return std::nullopt;
            for (s32 i = 0; i + 1 < n; ++i)
                out += static_cast<char32_t>(b[o + i]);
            o += static_cast<std::size_t>(n);
            return out;
        }
        const s32 m = -n;
        if (m > 4096 || !Need(static_cast<std::size_t>(m) * 2))
            return std::nullopt;
        std::vector<std::uint16_t> units(static_cast<std::size_t>(m));
        std::memcpy(units.data(), b.data() + o, units.size() * 2);
        o += units.size() * 2;
        if (units.back() != 0)
            return std::nullopt;
        units.pop_back();
        return FromUtf16(units);
    }
};

inline std::string Ascii(const std::u32string& s) {
    std::string out;
    for (const char32_t c : s)
        out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}

struct Package {
    std::vector<std::string> names;
    u32 total_header{};
    u64 export_offset{}, export_size{};
};

inline std::optional<Package> ParseSummary(std::span<const u8> uasset) {
    Cursor c{uasset};
    if (static_cast<u32>(c.I32()) != 0x9E2A83C1u)
        return std::nullopt;
    const s32 legacy = c.I32();
    if (legacy != -4)
        c.I32();
    c.I32();
    c.I32();
    if (legacy <= -2) {
        const s32 n = c.I32();
        if (n < 0 || n > 64)
            return std::nullopt;
        c.Skip(static_cast<std::size_t>(n) * 20);
    }
    Package p;
    p.total_header = static_cast<u32>(c.I32());
    if (!c.FString())
        return std::nullopt;
    const u32 flags = static_cast<u32>(c.I32());
    const s32 name_count = c.I32(), name_offset = c.I32();
    if (!(flags & 0x80000000u)) // PKG_FilterEditorOnly: cooked packages have no localisation id
        return std::nullopt;
    c.Skip(8); // gatherable text data
    const s32 export_count = c.I32(), export_offset = c.I32();
    if (!c.ok || name_count <= 0 || name_count > 65536 || name_offset <= 0 || export_count != 1 ||
        export_offset <= 0)
        return std::nullopt;
    Cursor n{uasset, static_cast<std::size_t>(name_offset)};
    for (s32 i = 0; i < name_count; ++i) {
        const auto s = n.FString();
        n.Skip(4); // hashes
        if (!s || !n.ok)
            return std::nullopt;
        p.names.push_back(Ascii(*s));
    }
    Cursor e{uasset, static_cast<std::size_t>(export_offset) + 28};
    const s32 size_lo = e.I32(), size_hi = e.I32(), off_lo = e.I32(), off_hi = e.I32();
    if (!e.ok || size_hi != 0 || off_hi != 0 || static_cast<u32>(off_lo) < p.total_header)
        return std::nullopt;
    p.export_size = static_cast<u32>(size_lo);
    p.export_offset = static_cast<u32>(off_lo) - p.total_header;
    return p;
}

struct Tag {
    std::string name, type;
    s32 size{};
    std::string inner; ///< struct name / enum or byte enum / array inner type / map key type
    u8 bool_value{};   ///< BoolProperty: the value lives in the tag
    s32 number{};      ///< the name's FName number (UE "_<number - 1>" suffix when non-zero)
};

/// Reads one property tag; nullopt at "None" or on error (c.ok tells which).
inline std::optional<Tag> NextTag(Cursor& c, const Package& p) {
    s32 number = 0;
    const auto fname = [&]() -> const std::string* {
        const s32 i = c.I32();
        number = c.I32();
        if (!c.ok || i < 0 || static_cast<std::size_t>(i) >= p.names.size()) {
            c.ok = false;
            return nullptr;
        }
        return &p.names[static_cast<std::size_t>(i)];
    };
    const std::string* name = fname();
    if (!name || *name == "None")
        return std::nullopt;
    Tag t;
    t.name = *name;
    t.number = number;
    const std::string* type = fname();
    if (!type)
        return std::nullopt;
    t.type = *type;
    t.size = c.I32();
    c.I32(); // array index
    if (t.type == "StructProperty") {
        if (const auto* n = fname())
            t.inner = *n;
        c.Skip(16);
    } else if (t.type == "BoolProperty") {
        t.bool_value = c.U8();
    } else if (t.type == "EnumProperty" || t.type == "ByteProperty" ||
               t.type == "ArrayProperty" || t.type == "SetProperty") {
        if (const auto* n = fname())
            t.inner = *n;
    } else if (t.type == "MapProperty") {
        if (const auto* n = fname())
            t.inner = *n;
        fname();
    }
    if (c.U8())
        c.Skip(16);
    if (!c.ok || t.size < 0 || !c.Need(static_cast<std::size_t>(t.size)))
        return std::nullopt;
    return t;
}

} // namespace dq3::cooked
