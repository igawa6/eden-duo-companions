// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "acnh_msbt.h"

#include <algorithm>
#include <cstring>

#include "acnh_bytes.h"

namespace acnh {
namespace {
struct Reader {
    const std::vector<uint8_t>& d;
    bool be;
    uint16_t U16(size_t o) const {
        return bytes::U16(d, o, be);
    }
    uint32_t U32(size_t o) const {
        return bytes::U32(d, o, be);
    }
};
} // namespace

namespace {
/// Appends the code point at s[i] (a surrogate pair or one unit); returns the units used.
size_t AppendUtf16Unit(std::string& out, std::u16string_view s, size_t i) {
    const char32_t u = s[i];
    if (u >= 0xD800 && u < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
        AppendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (char32_t(s[i + 1]) - 0xDC00));
        return 2;
    }
    AppendUtf8(out, u >= 0xD800 && u < 0xE000 ? 0xFFFD : u); // a lone surrogate
    return 1;
}
} // namespace

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool Msbt::Parse(const std::vector<uint8_t>& d) {
    raw.clear();
    if (d.size() < 0x20 || std::memcmp(d.data(), "MsgStdBn", 8) != 0)
        return false;
    const Reader r{d, !(d[8] == 0xFF && d[9] == 0xFE)};
    if (d[12] != 1)
        return false; // ACNH: UTF-16 only
    const uint16_t sections = r.U16(14);
    size_t off = 0x20;
    std::unordered_map<uint32_t, std::string> labels;
    std::vector<std::u16string> texts;
    for (uint16_t s = 0; s < sections && off + 0x10 <= d.size(); ++s) {
        const uint32_t size = r.U32(off + 4);
        const size_t body = off + 0x10;
        if (body + size > d.size())
            return false;
        if (std::memcmp(d.data() + off, "LBL1", 4) == 0) {
            const uint32_t buckets = r.U32(body);
            for (uint32_t i = 0; i < buckets && body + 4 + i * 8 + 8 <= body + size; ++i) {
                const uint32_t count = r.U32(body + 4 + i * 8);
                size_t p = body + r.U32(body + 4 + i * 8 + 4);
                for (uint32_t k = 0; k < count && p < body + size; ++k) {
                    const uint8_t len = d[p];
                    if (p + 1 + len + 4 > body + size)
                        break;
                    std::string name(reinterpret_cast<const char*>(d.data() + p + 1), len);
                    labels[r.U32(p + 1 + len)] = std::move(name);
                    p += 1 + len + 4;
                }
            }
        } else if (std::memcmp(d.data() + off, "TXT2", 4) == 0) {
            const uint32_t n = r.U32(body);
            if (4 + size_t{n} * 4 > size)
                return false;
            for (uint32_t i = 0; i < n; ++i) {
                const size_t start = body + r.U32(body + 4 + i * 4);
                const size_t end = i + 1 < n ? body + r.U32(body + 4 + (i + 1) * 4) : body + size;
                std::u16string t;
                for (size_t p = start; p + 2 <= end && p + 2 <= d.size(); p += 2)
                    t += static_cast<char16_t>(r.U16(p));
                // Cut at the terminator, stepping over tags (a tag's own fields can be 0).
                size_t k = 0;
                while (k < t.size() && t[k] != 0) {
                    if (t[k] == 0x0E && k + 3 < t.size())
                        k += 4 + (size_t{t[k + 3]} + 1) / 2;
                    else if (t[k] == 0x0F)
                        k += 3;
                    else
                        ++k;
                }
                t.resize(std::min(k, t.size()));
                texts.push_back(std::move(t));
            }
        }
        off = body + ((size + 15) & ~size_t{15});
    }
    for (auto& [index, name] : labels)
        if (index < texts.size())
            raw.emplace(std::move(name), texts[index]);
    return !raw.empty();
}

const std::u16string* Msbt::Raw(std::string_view label) const {
    const auto it = raw.find(std::string{label});
    return it == raw.end() ? nullptr : &it->second;
}

std::string Msbt::Text(std::string_view label) const {
    const auto* s = Raw(label);
    return s ? ToUtf8(*s) : std::string{};
}

std::string Msbt::ToUtf8(std::u16string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const char16_t u = s[i];
        if (u == 0)
            break;
        if (u == 0x0E && i + 3 < s.size()) {
            const size_t units = (s[i + 3] + 1u) / 2; // argument bytes, rounded up to whole units
            i += 4 + units;
            continue;
        }
        if (u == 0x0F && i + 2 < s.size()) {
            i += 3;
            continue;
        }
        i += AppendUtf16Unit(out, s, i);
    }
    return out;
}

std::string Utf16ToUtf8(std::u16string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();)
        i += AppendUtf16Unit(out, s, i);
    return out;
}

char32_t NextCodepoint(std::string_view s, size_t& i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    const int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (n < 0 || i + n >= s.size()) {
        ++i;
        return 0xFFFD;
    }
    char32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    i += n + 1;
    return cp;
}

} // namespace acnh
