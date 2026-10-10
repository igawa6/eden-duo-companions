// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Text layout as the runtime draws it (mod_ui_text.cpp Canvas::MeasureText / LayoutLines), from the
// same game font metrics the module decodes for the runtime: a glyph's advance scaled by
// 5 * scale / line_height, a missing glyph half a line wide, greedy word wrap (a word wider than
// the line breaks by character), colour tags ignored. Used to size scroll regions to their text.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lp_assets.h"

namespace lp_text {

// Town Map guidance: three lines at body size, falling back to size4 only when needed.
// Widget height binds are unavailable in runtime18, so the five exact sizes have gated panels.
inline constexpr int MapGoalWidth = 1048;
struct MapGoalLayout { int scale = 5, text_height = 0, height = 64; };
inline MapGoalLayout FitMapGoal(int height5, int height4) {
    const int scale = height5 <= 105 ? 5 : 4;
    const int text_height = std::min(scale == 5 ? 105 : 84, std::max(0, scale == 5 ? height5 : height4));
    return {scale, text_height, std::max(40, text_height) + 24};
}

class Measure {
public:
    explicit Measure(const lp_assets::Font& f) : font{f} {}

    // MeasureText: the pen advance, truncated like the runtime's static_cast<s32>.
    int Width(std::string_view text, int scale) const {
        if (scale <= 0) scale = 1;
        const float wanted = static_cast<float>(scale) * 5.0f;
        const float ratio = font.line_height ? wanted / static_cast<float>(font.line_height) : 0.0f;
        float pen = 0.0f;
        for (size_t i = 0; i < text.size();) {
            if (const size_t tag = TagLen(text, i)) { i += tag; continue; }
            const char32_t cp = Next(text, i);
            if (cp == '\n') continue;
            const auto* g = Glyph(cp);
            pen += g == nullptr ? wanted * 0.5f : static_cast<float>(g->advance) * ratio;
        }
        return static_cast<int>(pen);
    }

    // LayoutLines: the number of lines `text` takes at `scale` wrapped to `wrap`.
    int Lines(std::string_view text, int scale, int wrap) const {
        int lines = 0;
        size_t start = 0;
        while (true) {
            const size_t nl = text.find('\n', start);
            const std::string_view para = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
            lines += ParaLines(para, scale, wrap);
            if (nl == std::string_view::npos) break;
            start = nl + 1;
        }
        return lines;
    }

    // The painted height: lines at a pitch of 5 * scale + 3 * scale (the runtime's default gap).
    int Height(std::string_view text, int scale, int wrap) const {
        if (text.empty()) return 0;
        const int n = Lines(text, scale, wrap);
        return (n - 1) * (scale * 8) + scale * 5;
    }

private:
    int ParaLines(std::string_view para, int scale, int wrap) const {
        if (wrap <= 0) return 1;
        int lines = 0;
        std::string cur;
        bool has_cur = false;
        size_t p = 0;
        while (true) {
            const size_t sp = para.find(' ', p);
            const std::string_view word = para.substr(p, sp == std::string_view::npos ? std::string_view::npos : sp - p);
            if (has_cur) {
                std::string cand = cur + ' ' + std::string{word};
                if (Width(cand, scale) > wrap) { ++lines; cur.clear(); has_cur = false; continue; }
                cur = std::move(cand);
            } else if (Width(word, scale) <= wrap) {
                cur = std::string{word};
                has_cur = true;
            } else {
                std::string piece;
                for (size_t i = 0; i < word.size();) {
                    size_t j = i;
                    if (const size_t tag = TagLen(word, i)) j = i + tag; else Next(word, j);
                    std::string next = piece + std::string{word.substr(i, j - i)};
                    if (Width(piece, scale) > 0 && Width(next, scale) > wrap) { ++lines; piece = std::string{word.substr(i, j - i)}; }
                    else piece = std::move(next);
                    i = j;
                }
                cur = std::move(piece);
                has_cur = true;
            }
            if (sp == std::string_view::npos) break;
            p = sp + 1;
        }
        return lines + 1;
    }

    const EdenDsmodFontGlyph* Glyph(char32_t cp) const {
        if (cp >= font.first_codepoint && cp - font.first_codepoint < font.glyphs.size())
            return &font.glyphs[cp - font.first_codepoint];
        return nullptr;
    }
    static size_t TagLen(std::string_view t, size_t i) {
        if (t.substr(i).starts_with("{/c}")) return 4;
        if (t.size() - i >= 13 && t.substr(i).starts_with("{c:#") && t[i + 12] == '}') return 13;
        return 0;
    }
    static char32_t Next(std::string_view t, size_t& i) {
        const auto c = static_cast<unsigned char>(t[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t cp = n == 1 ? c : c & (0x3F >> (n - 1));
        for (int k = 1; k < n && i + k < t.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(t[i + k]) & 0x3F);
        i += n;
        return cp;
    }

    const lp_assets::Font& font;
};

} // namespace lp_text
