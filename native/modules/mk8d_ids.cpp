// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The id -> name rules (mk8d_ids.h). The name lists themselves are read from the running game
// by mk8d_reader.cpp; every rule below cites the 4.0.0 instructions it reproduces (3.0.3 applies
// the same rules, verified live against its own screens).

#include "mk8d_ids.h"

#include <cmath>
#include <cstring>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace Mk8dIds {
namespace {
using dsmod_sdk::Le16;
using dsmod_sdk::Le32;

bool IsSafeName(std::string_view s) {
    if (s.empty() || s.size() > 96)
        return false;
    for (const char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_'))
            return false;
    return true;
}

} // namespace

std::string DriverIconName(const std::vector<std::string>& names, int driver_id, int variant) {
    // main+0x414930: the icon preload skips 0x1d and 0x34 (Mii, drawn from the Mii face).
    if (driver_id < 0 || driver_id == 0x1d || driver_id == 0x34)
        return {};
    const std::size_t index = static_cast<std::size_t>(driver_id) + 1; // main+0x414d64 add #1
    if (index >= names.size() || !IsSafeName(names[index]))
        return {};
    std::string out = names[index];
    // main+0x414e74..0x414e98: `cmp w8,#0x2c ; b.hi ; lsl ; and #0x160000010010`.
    constexpr std::uint64_t VariantMask = 0x160000010010ULL;
    if (driver_id <= 0x2c && ((std::uint64_t{1} << driver_id) & VariantMask)) {
        if (variant < 0 || variant > 9)
            return {};
        out.push_back('0');                              // rodata 0xee7af1 "0"
        out.push_back(static_cast<char>('0' + variant)); // main+0x414fe0 add #0x30
    }
    return out;
}

int DriverLabel(const std::vector<std::int32_t>& labels, int driver_id) {
    if (driver_id < 0)
        return -1;
    const std::size_t index = static_cast<std::size_t>(driver_id) + 1; // main+0x412a34 add #1
    return index < labels.size() && labels[index] > 0 ? labels[index] : -1;
}

std::string_view CourseFolder(const std::vector<std::string>& tokens, int course_raw) {
    if (course_raw < 0 || course_raw > MaxCourseRaw)
        return {};
    const std::size_t index = static_cast<std::size_t>(course_raw) + 1; // main+0x8af1e8
    if (index >= tokens.size() || !IsSafeName(tokens[index]))
        return {};
    return tokens[index];
}

std::string CoursePictName(const std::vector<std::string>& names, int course_raw) {
    if (course_raw < 0 || course_raw > MaxCourseRaw)
        return {};
    const std::size_t index = static_cast<std::size_t>(course_raw) + 1; // main+0x418de4 add #1
    if (index >= names.size() || !IsSafeName(names[index]))
        return {};
    std::string_view name = names[index];
    if (name.size() > 3 && name.substr(name.size() - 3) == "_00")
        name.remove_suffix(3);
    else
        return {}; // every 4.0.0 entry ends in _00; anything else is not a picture name
    return std::string{name};
}

int CoinKind(int raw) {
    if (raw == 0x33)
        return 1;
    // main+0x87ed68: w = raw - 0x34; w <= 0xe && (0x7001 >> w) & 1
    const int w = raw - 0x34;
    if (w >= 0 && w <= 0xe && ((0x7001 >> w) & 1))
        return 2;
    if (raw == 0x46)
        return 3;
    return raw == 0x66 ? 4 : 0;
}

int ItemFrame(const std::array<std::uint8_t, ItemCount>& table, int item, int raw, int uses_left) {
    if (item < 0 || item >= ItemCount)
        return -1;
    if (uses_left == 1 || uses_left == 2) {
        switch (item) {
        case 7: // Mush3: fmov #0.0 / #1.0 (main+0x5121b8/0x5121bc)
            return uses_left == 2 ? 1 : 0;
        case 17: // Banana3: #3.0 / #17.0 (main+0x5121e8/0x5121ec)
            return uses_left == 2 ? 17 : 3;
        case 18: // Koura3: #5.0 / #18.0 (main+0x512218/0x51221c)
            return uses_left == 2 ? 18 : 5;
        case 19: // RedKoura3: #7.0 / #19.0 (main+0x512248/0x51224c)
            return uses_left == 2 ? 19 : 7;
        default:
            break;
        }
    }
    int frame = table[static_cast<std::size_t>(item)];
    if (item == 0xf) { // main+0x4234b0 cmp w0,#0xf
        switch (CoinKind(raw)) {
        case 1:
            frame = 0x19;
            break;
        case 2:
            frame = 0x1a;
            break;
        case 4:
            frame = 0x1d;
            break;
        default:
            break;
        }
    }
    return frame;
}

float MapWorldYScale(int course_raw, float scale_0x44) {
    return course_raw == 0x44 && std::isfinite(scale_0x44) && scale_0x44 > 0.0f ? scale_0x44 : 1.0f;
}

std::optional<ItemPattern> ParseItemPattern(const std::uint8_t* b, std::size_t n) {
    if (!b || n < 0x14 || std::memcmp(b, "FLAN", 4) != 0)
        return std::nullopt;
    const auto in = [n](std::size_t at, std::size_t len) { return at <= n && len <= n - at; };
    const auto cstr = [&](std::size_t at) -> std::optional<std::string> {
        std::string s;
        for (std::size_t i = at; i < n && i < at + 128; ++i) {
            if (!b[i])
                return s;
            s.push_back(static_cast<char>(b[i]));
        }
        return std::nullopt;
    };
    std::size_t o = Le16(b + 6);
    const unsigned sections = Le16(b + 0x10);
    for (unsigned s = 0; s < sections; ++s) {
        if (!in(o, 8))
            return std::nullopt;
        const std::uint32_t size = Le32(b + o + 4);
        if (size < 8 || !in(o, size))
            return std::nullopt;
        if (std::memcmp(b + o, "pai1", 4) != 0) {
            o += size;
            continue;
        }
        if (size < 0x14)
            return std::nullopt;
        ItemPattern out;
        const unsigned textures = Le16(b + o + 0xc);
        const unsigned entries = Le16(b + o + 0xe);
        const std::uint32_t entry_table = Le32(b + o + 0x10);
        const std::size_t tex_table = o + 0x14;
        if (!in(tex_table, textures * 4u) || !in(o + entry_table, entries * 4u))
            return std::nullopt;
        for (unsigned t = 0; t < textures; ++t) {
            auto name = cstr(tex_table + Le32(b + tex_table + 4 * t));
            if (!name)
                return std::nullopt;
            out.textures.push_back(std::move(*name));
        }
        for (unsigned e = 0; e < entries; ++e) {
            const std::size_t eo = o + Le32(b + o + entry_table + 4 * e);
            if (!in(eo, 0x20))
                return std::nullopt;
            const auto name = cstr(eo);
            if (!name || *name != "P_Item_00")
                continue;
            const unsigned tags = b[eo + 0x1c];
            if (!in(eo + 0x20, tags * 4u))
                return std::nullopt;
            for (unsigned t = 0; t < tags; ++t) {
                const std::size_t to = eo + Le32(b + eo + 0x20 + 4 * t);
                if (!in(to, 8))
                    return std::nullopt;
                if (std::memcmp(b + to, "FLTP", 4) != 0)
                    continue;
                const unsigned groups = b[to + 4];
                if (!in(to + 8, groups * 4u))
                    return std::nullopt;
                for (unsigned g = 0; g < groups; ++g) {
                    const std::size_t go = to + Le32(b + to + 8 + 4 * g);
                    if (!in(go, 12) || b[go + 2] != 1) // curve 1 = step keys (frame, u16 value)
                        return std::nullopt;
                    const unsigned keys = Le16(b + go + 4);
                    const std::size_t ko = go + Le32(b + go + 8);
                    if (!in(ko, keys * 8u))
                        return std::nullopt;
                    for (unsigned k = 0; k < keys; ++k) {
                        float frame{};
                        std::memcpy(&frame, b + ko + 8 * k, 4);
                        const unsigned value = Le16(b + ko + 8 * k + 4);
                        if (!(frame >= 0.0f && frame < 256.0f) || frame != std::floor(frame) ||
                            value >= textures)
                            return std::nullopt;
                        const auto f = static_cast<std::size_t>(frame);
                        if (out.frame_texture.size() <= f)
                            out.frame_texture.resize(f + 1, -1);
                        out.frame_texture[f] = static_cast<std::int32_t>(value);
                    }
                }
                if (!out.frame_texture.empty())
                    return out;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::string ItemIconName(const ItemPattern& pattern, int frame) {
    if (frame < 0 || static_cast<std::size_t>(frame) >= pattern.frame_texture.size())
        return {};
    const int tex = pattern.frame_texture[static_cast<std::size_t>(frame)];
    if (tex < 0 || static_cast<std::size_t>(tex) >= pattern.textures.size())
        return {};
    std::string_view name = pattern.textures[static_cast<std::size_t>(tex)];
    constexpr std::string_view Prefix = "tc_item_";
    if (name.size() <= Prefix.size())
        return {};
    for (std::size_t i = 0; i < Prefix.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != Prefix[i])
            return {};
    }
    name.remove_prefix(Prefix.size());
    if (const auto caret = name.find('^'); caret != std::string_view::npos)
        name = name.substr(0, caret);
    return IsSafeName(name) ? std::string{name} : std::string{};
}

} // namespace Mk8dIds
