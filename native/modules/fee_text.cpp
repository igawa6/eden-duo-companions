// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fee_text.h"

#include <algorithm>
#include <cstring>

namespace fee_text {
namespace {

std::uint16_t U16(const std::uint8_t* d, std::size_t o, bool little) {
    return little ? static_cast<std::uint16_t>(d[o] | (d[o + 1] << 8))
                  : static_cast<std::uint16_t>((d[o] << 8) | d[o + 1]);
}

std::uint32_t U32(const std::uint8_t* d, std::size_t o, bool little) {
    return little ? (static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) |
                     (static_cast<std::uint32_t>(d[o + 2]) << 16) |
                     (static_cast<std::uint32_t>(d[o + 3]) << 24))
                  : ((static_cast<std::uint32_t>(d[o]) << 24) |
                     (static_cast<std::uint32_t>(d[o + 1]) << 16) |
                     (static_cast<std::uint32_t>(d[o + 2]) << 8) |
                     static_cast<std::uint32_t>(d[o + 3]));
}

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

/// TXT2 payload -> UTF-8: text as is, '\n' kept, control tags dropped (their arguments can hold
/// UTF-16 text of their own, e.g. an Emblem's ruby spelling, so the byte count must be honoured).
std::string DecodeUtf16(const std::uint8_t* d, std::size_t begin, std::size_t end, bool little,
                        bool keep_tags = false) {
    std::string out;
    std::size_t i = begin;
    while (i + 2 <= end) {
        const std::uint16_t u = U16(d, i, little);
        if (u == 0)
            break;
        if (u == 0x000E) {
            if (i + 8 > end)
                break;
            const std::size_t arg_bytes = U16(d, i + 6, little);
            if (!keep_tags && U16(d, i + 2, little) == 6 && U16(d, i + 4, little) == 3)
                out += PlayerNameMark; // the avatar's name (Assets::Text fills it in)
            if (keep_tags) {
                // Rule texts keep their argument tags as "\x01<group><type>" so the reader can
                // substitute the leader name / rule argument the way Mess.Get would.
                out += '\x01';
                out += char('0' + std::min<int>(U16(d, i + 2, little), 9));
                out += char('0' + std::min<int>(U16(d, i + 4, little), 9));
            }
            i += 8 + ((arg_bytes + 1) & ~std::size_t{1});
            continue;
        }
        if (u == 0x000F) {
            i += 2;
            continue;
        }
        i += 2;
        if (u >= 0xD800 && u < 0xDC00 && i + 2 <= end) {
            const std::uint16_t lo = U16(d, i, little);
            if (lo >= 0xDC00 && lo < 0xE000) {
                AppendUtf8(out, 0x10000 + ((char32_t(u) - 0xD800) << 10) + (char32_t(lo) - 0xDC00));
                i += 2;
                continue;
            }
        }
        AppendUtf8(out, u >= 0xD800 && u < 0xE000 ? 0xFFFD : u);
    }
    return out;
}

} // namespace

bool ParseMsbt(const std::uint8_t* d, std::size_t size, Table& out) {
    out.text.clear();
    if (!d || size < 0x20 || std::memcmp(d, "MsgStdBn", 8) != 0)
        return false;
    const bool little = d[8] == 0xFF && d[9] == 0xFE;
    if (!little && !(d[8] == 0xFE && d[9] == 0xFF))
        return false;
    const std::uint8_t encoding = d[12];
    if (encoding != 1)
        return false; // Engage's tables are UTF-16 (LE); anything else is not this format
    const std::uint16_t sections = U16(d, 14, little);

    std::unordered_map<std::uint32_t, std::string> labels;
    std::vector<std::pair<std::size_t, std::size_t>> texts;
    std::size_t off = 0x20;
    for (std::uint16_t s = 0; s < sections && off + 0x10 <= size; ++s) {
        const std::uint32_t body_size = U32(d, off + 4, little);
        const std::size_t body = off + 0x10;
        if (body_size > size - body)
            return false;
        if (std::memcmp(d + off, "LBL1", 4) == 0) {
            if (body_size < 4)
                return false;
            const std::uint32_t buckets = U32(d, body, little);
            if (4 + std::size_t{buckets} * 8 > body_size)
                return false;
            for (std::uint32_t b = 0; b < buckets; ++b) {
                const std::uint32_t count = U32(d, body + 4 + b * 8, little);
                std::size_t p = body + U32(d, body + 8 + b * 8, little);
                for (std::uint32_t k = 0; k < count; ++k) {
                    if (p >= body + body_size)
                        break;
                    const std::size_t len = d[p];
                    if (p + 1 + len + 4 > body + body_size)
                        break;
                    const std::string name(reinterpret_cast<const char*>(d + p + 1), len);
                    labels[U32(d, p + 1 + len, little)] = name;
                    p += 1 + len + 4;
                }
            }
        } else if (std::memcmp(d + off, "TXT2", 4) == 0) {
            if (body_size < 4)
                return false;
            const std::uint32_t count = U32(d, body, little);
            if (4 + std::size_t{count} * 4 > body_size)
                return false;
            texts.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::size_t begin = body + U32(d, body + 4 + i * 4, little);
                const std::size_t end =
                    i + 1 < count ? body + U32(d, body + 4 + (i + 1) * 4, little) : body + body_size;
                if (begin > body + body_size || end > body + body_size || begin > end)
                    return false;
                texts.emplace_back(begin, end);
            }
        }
        off = body + ((std::size_t{body_size} + 15) & ~std::size_t{15});
    }
    for (const auto& [index, name] : labels) {
        if (index >= texts.size())
            continue;
        const bool rule = name.starts_with("MID_RULE_");
        out.text.emplace(name, DecodeUtf16(d, texts[index].first, texts[index].second, little, rule));
    }
    return !out.text.empty();
}

bool IsRawKey(std::string_view t) {
    const auto upper_digit = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!t.empty() && t[0] == 'M') { // ^M[A-Z0-9]*_
        std::size_t i = 1;
        while (i < t.size() && upper_digit(t[i]))
            ++i;
        if (i < t.size() && t[i] == '_')
            return true;
    }
    std::size_t n = 0; // ^[A-Z]{2,5}ID_
    while (n < t.size() && t[n] >= 'A' && t[n] <= 'Z')
        ++n;
    for (std::size_t k = 2; k <= 5; ++k)
        if (n >= k + 2 && t.substr(k, 3) == "ID_")
            return true;
    return false;
}

std::string SingleLine(std::string_view text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        const bool ws = c == ' ' || c == '\t' || c == '\r' || c == '\n';
        if (ws) {
            space = !out.empty();
            continue;
        }
        if (space) {
            out += ' ';
            space = false;
        }
        out += c;
    }
    return out;
}

std::string FaceSprite(std::string_view icon) {
    std::size_t at = 0;
    while (at < icon.size() && icon[at] >= '0' && icon[at] <= '9')
        ++at;
    const std::string_view tail = icon.substr(at);
    if (tail.empty() || tail.size() > 48)
        return {};
    const std::string_view num = icon.substr(0, at);
    if ((num == "051" || num == "052") && tail.starts_with("Lueur"))
        return "LueurW";
    return std::string{tail};
}

std::string GenderedIcon(std::string_view icon, int gender) {
    if (gender == 2 && icon.size() >= 8 && (icon.starts_with("001Lueur") || icon.starts_with("002Lueur")))
        return std::string{icon[2] == '1' ? "051" : "052"} + std::string{icon.substr(3)};
    return std::string{icon};
}

std::string HelpKey(std::string_view key) {
    const std::size_t cut = key.find('_');
    if (cut == std::string_view::npos || cut == 0 || key.size() > 64)
        return {};
    std::string out;
    out.reserve(key.size() + 2);
    out.append(key.substr(0, cut));
    out += "_H";
    out.append(key.substr(cut));
    return out;
}

const char* LocaleDir(int lang) {
    // App.Language.Langs (dump.cs enum Language.Langs).
    static constexpr const char* kDirs[] = {
        "jp/jpja", "us/usen", "us/usfr", "us/uses", "eu/euen", "eu/eufr",
        "eu/eues", "eu/eude", "eu/euit", "tw/twch", "cn/cnch", "kr/krko",
    };
    return lang >= 0 && lang < int(sizeof kDirs / sizeof kDirs[0]) ? kDirs[lang] : "";
}

} // namespace fee_text
