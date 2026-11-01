// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem Engage: managed (IL2CPP System.String) readers shared by fee_reader and fee_hub.
// One bounded bulk read per string (length, then all UTF-16 units), never one read per char.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "fee_pins.inc"

namespace fee_guest {

inline void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

/// UTF-16 -> UTF-8; an unpaired surrogate becomes U+FFFD.
inline std::string Utf16ToUtf8(std::u16string_view w) {
    std::string out;
    out.reserve(w.size());
    for (std::size_t i = 0; i < w.size(); ++i) {
        char32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00) {
            if (i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (char32_t(w[i + 1]) - 0xDC00);
                ++i;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xDC00 && c < 0xE000) {
            c = 0xFFFD;
        }
        AppendUtf8(out, c);
    }
    return out;
}

/// Name keys are ASCII ("MPID_Lueur"); anything else is marked '?'.
inline std::string AsciiKey(std::u16string_view w) {
    std::string out(w.size(), '?');
    for (std::size_t i = 0; i < w.size(); ++i)
        if (w[i] < 0x80)
            out[i] = char(w[i]);
    return out;
}

/// The UTF-16 units of the System.String at `at`; nullopt for null, unreadable, a negative
/// length or more than `max_chars`. `read(addr, out, n)` is the caller's bounded guest read.
template <class ReadFn>
std::optional<std::u16string> ManagedChars(const ReadFn& read, std::uint64_t at,
                                           std::size_t max_chars) {
    if (!at)
        return std::nullopt;
    std::int32_t len = -1;
    if (!read(at + fee_pins::StringLength, &len, sizeof len) || len < 0 ||
        std::size_t(len) > max_chars)
        return std::nullopt;
    std::u16string w(std::size_t(len), u'\0');
    if (!w.empty() && !read(at + fee_pins::StringData, w.data(), w.size() * 2))
        return std::nullopt;
    return w;
}

} // namespace fee_guest
