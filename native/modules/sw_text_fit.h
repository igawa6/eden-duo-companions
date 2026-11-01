// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Layout help for published text (2026-11-01 text-length stress test). The runtime wraps labels
// at spaces only and splits a word wider than the line by character ("Galarnuss-Kra|nz"); a
// hyphenated word that is too wide gets a break opportunity after its hyphen instead.

#include <string>
#include <string_view>

#include "sw_font_metrics.h"

namespace sw_text_fit {

/// `text` with "- " for "-" inside every space-separated word wider than `width` px at `scale`.
inline std::string BreakLongHyphenated(std::string_view text, int scale, int width) {
    std::string out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(' ', start), text.size());
        const std::string_view word = text.substr(start, end - start);
        if (sw_font::MeasureText(word, scale) > width && word.find('-') != std::string_view::npos) {
            for (std::size_t i = 0; i < word.size(); ++i) {
                out += word[i];
                if (word[i] == '-' && i + 1 < word.size()) out += ' ';
            }
        } else {
            out.append(word);
        }
        if (end >= text.size()) break;
        out += ' ';
        start = end + 1;
    }
    return out;
}

/// Japanese (2026-11-01): `text` with every U+3000 (the game's phrase separator) as U+0020, the
/// only character the runtime breaks a line at; a line never starts with the separator.
inline std::string IdeographicSpacesToBreaks(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 3, "\xE3\x80\x80") == 0) {
            out += ' ';
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

/// Wide characters Chinese runs together without spaces: Han, kana, CJK punctuation, full-width forms.
inline bool IsWideCjk(char32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
           (cp >= 0xFF00 && cp <= 0xFFEF);
}

/// Chinese (2026-11-01): text has no spaces between words and wraps between any two characters.
/// The reader flattens the game's authored line breaks (sized for the native text box) to U+0020;
/// between two wide characters that space is dropped, so the runtime -- which splits a run wider
/// than the line by character -- wraps the sentence anywhere instead of at a visible gap. A space
/// next to Latin letters or digits ("HP 50") stays. Korean keeps its spaces (it breaks at words).
inline std::string JoinCjkSpaces(std::string_view text) {
    const auto decode = [&](std::size_t i, std::size_t& n) -> char32_t {
        const auto c = static_cast<unsigned char>(text[i]);
        n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (n == 1 || i + n > text.size()) {
            n = 1;
            return c;
        }
        char32_t cp = c & (0x7F >> n);
        for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        return cp;
    };
    std::string out;
    out.reserve(text.size());
    char32_t prev = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == ' ') {
            std::size_t j = i;
            while (j < text.size() && text[j] == ' ') ++j;
            std::size_t n = 0;
            const char32_t next = j < text.size() ? decode(j, n) : 0;
            if (!(IsWideCjk(prev) && IsWideCjk(next))) out.append(text.substr(i, j - i));
            i = j;
            continue;
        }
        std::size_t n = 0;
        prev = decode(i, n);
        out.append(text.substr(i, n));
        i += n;
    }
    return out;
}

} // namespace sw_text_fit
