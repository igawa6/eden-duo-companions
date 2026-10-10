// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The companion's words in the game's language (header only, no game data). Every string the
// pages show is a row of lp_strings_data.h (generated from tools/luminescent-platinum/lp_strings.tsv
// and the manifest's t() bindings): a game label (B), our own translation (D), or both (C: the
// label where it reads like our row, else our row). The module publishes each row as lp.t.<key>
// and composes its sentences from the same rows:
//   - Resolve: the game label (normalised) where the row lists the variant, else our text for the
//     variant, else English;
//   - Fill: templates with named slots ({item}, {who}, {n} ...) and the Korean particle choice
//     ({을/를} after a slot, decided by the slot's last letter);
//   - PluralOne: which of the _one / _other rows a count takes.

#pragma once

#include "lp_lang.h"

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lp_strings {

inline constexpr unsigned UpperFirst = 1; // upper-case the label's first letter (es Pokétch apps)

struct Row {
    std::string_view key, table, label; // the game label (table without the language prefix)
    unsigned flags = 0;
    unsigned label_mask = 0;            // bit lp_lang::Variant: that variant shows the label
    std::array<std::string_view, 11> text; // our text by lp_lang::Variant ("" = English, [0])
};
// lp.t.<key>.w<scale>: the width a label of `scale` (fit_text down to min_scale within `wrap` px;
// wrap 0 = no fitting) draws the row's text at.
struct WidthSpec {
    std::uint16_t key;
    int scale, min_scale, wrap;
};
// lp.t.sum.<name>: the widths of up to six rows at `scale` added up (a row of labels); with a
// limit > 0 also lp.t.sum.<name>.fits (the sum is at most the limit).
struct SumSpec {
    std::string_view name;
    int scale, limit;
    std::array<std::uint16_t, 6> parts; // a part past the table's end is unused
};
// Where the manifest draws a row (the dev fit report): scale, smallest scale, width available
// (0 = unbounded), lines allowed.
struct FitSpec {
    std::uint16_t key;
    int scale, min_scale, width, lines;
    std::string_view where;
};
// The same for a label that shows the game's names of one kind ("move", "item", "ability", "species").
struct CatalogFitSpec {
    std::string_view kind;
    int scale, min_scale, width, lines;
    std::string_view where;
};

// ---- labels ------------------------------------------------------------------------------------

namespace detail {
inline bool IsTrimSpace(char32_t cp) {
    return cp == ' ' || cp == 0xA0 || cp == 0x202F || cp == 0x3000;
}
inline void AppendUtf8(std::string& out, char32_t cp) {
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
inline std::vector<char32_t> Decode(std::string_view t) {
    std::vector<char32_t> cps;
    for (std::size_t i = 0; i < t.size();)
        cps.push_back(lp_lang::NextCodepoint(t, i));
    return cps;
}
inline std::string Encode(const std::vector<char32_t>& cps) {
    std::string out;
    for (const char32_t cp : cps)
        AppendUtf8(out, cp);
    return out;
}
} // namespace detail

// A label as a UI word: its line breaks joined (nothing between Chinese / Japanese text, else a
// space), runs of spaces collapsed, spaces, no-break spaces and a trailing colon trimmed at both
// ends ("Vus :" -> "Vus", " Sort" -> "Sort"). Mirrors lp_strings.py normalize().
inline std::string NormalizeLabel(std::string_view text, unsigned flags = 0) {
    using namespace detail;
    const auto in = Decode(text);
    std::vector<char32_t> out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char32_t cp = in[i];
        if (cp == '\n') {
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            const char32_t next = i + 1 < in.size() ? in[i + 1] : 0;
            if (!out.empty() && next && !lp_lang::IsSpaceless(out.back()) && !lp_lang::IsSpaceless(next))
                out.push_back(' ');
            continue;
        }
        if (cp == ' ' && !out.empty() && out.back() == ' ')
            continue;
        out.push_back(cp);
    }
    const auto trim = [&out] {
        while (!out.empty() && IsTrimSpace(out.back()))
            out.pop_back();
        std::size_t lead = 0;
        while (lead < out.size() && IsTrimSpace(out[lead]))
            ++lead;
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(lead));
    };
    trim();
    while (!out.empty() && (out.back() == ':' || out.back() == 0xFF1A)) {
        out.pop_back();
        trim();
    }
    if ((flags & UpperFirst) && !out.empty()) {
        char32_t& c = out.front();
        if (c >= 'a' && c <= 'z')
            c -= 0x20;
        else if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
            c -= 0x20;
    }
    return Encode(out);
}

// The text of a label in the language in effect (English where the language leaves it empty), ""
// when the catalog has none.
using LabelFn = std::function<std::string(std::string_view table, std::string_view label)>;

inline std::string Resolve(const Row& row, lp_lang::Variant v, const LabelFn& label) {
    const auto i = static_cast<std::size_t>(v);
    if (!row.label.empty() && (row.label_mask >> i & 1) && label)
        if (auto t = NormalizeLabel(label(row.table, row.label), row.flags); !t.empty())
            return t;
    if (i < row.text.size() && !row.text[i].empty())
        return std::string{row.text[i]};
    return std::string{row.text[0]};
}

// ---- templates -----------------------------------------------------------------------------------

// The count n takes the _one row: n == 1 (en, de, es, it), n <= 1 (fr, pt-BR: CLDR), and always
// for ko, zh and ja, whose two rows are the same text.
inline bool PluralOne(lp_lang::Variant v, long long n) {
    using V = lp_lang::Variant;
    switch (v) {
    case V::Fr:
    case V::PtBR: return n == 0 || n == 1;
    case V::Ko:
    case V::ZhHans:
    case V::ZhHant:
    case V::Ja: return true;
    default: return n == 1;
    }
}

// The Korean particle for `choice` ("을/를", "이/가", "은/는", "과/와", "으로/로") after `word`:
// the first form after a final consonant, the second after a vowel (and ㄹ before 으로/로). A
// Hangul syllable gives its final consonant, a digit its Sino-Korean reading; anything else (a
// Latin item name, an empty slot) takes the combined form 을(를), (으)로.
inline std::string KoParticle(std::string_view choice, std::string_view word) {
    const auto slash = choice.find('/');
    if (slash == std::string_view::npos)
        return std::string{choice};
    const std::string_view first = choice.substr(0, slash), second = choice.substr(slash + 1);
    const bool ro = second == "로";
    char32_t last = 0;
    for (std::size_t i = 0; i < word.size();) {
        const char32_t cp = lp_lang::NextCodepoint(word, i);
        if (cp != ' ')
            last = cp;
    }
    int pick = 0; // 1 first, 2 second, 0 combined
    if (last >= 0xAC00 && last <= 0xD7A3) {
        const int jong = static_cast<int>((last - 0xAC00) % 28);
        pick = jong == 0 || (ro && jong == 8) ? 2 : 1;
    } else if (last >= '0' && last <= '9') {
        // 영 일 이 삼 사 오 육 칠 팔 구: 2 4 5 9 end in a vowel; 1 7 8 end in ㄹ
        const int d = static_cast<int>(last - '0');
        const bool vowel = d == 2 || d == 4 || d == 5 || d == 9;
        const bool rieul = d == 1 || d == 7 || d == 8;
        pick = vowel || (ro && rieul) ? 2 : 1;
    }
    if (pick == 1)
        return std::string{first};
    if (pick == 2)
        return std::string{second};
    return ro ? "(으)로" : std::string{first} + "(" + std::string{second} + ")";
}

struct Slot {
    std::string_view name;
    std::string value;
};

// Fills {name} slots by plain substitution; a {X/Y} token is a Korean particle decided by the slot
// right before it (no slot right before: the combined form). Unknown slots are left empty.
inline std::string Fill(std::string_view tmpl, std::initializer_list<Slot> slots) {
    std::string out;
    std::string_view last_value;
    std::size_t last_end = std::string::npos; // out.size() right after the last filled slot
    for (std::size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            const auto close = tmpl.find('}', i);
            if (close != std::string_view::npos) {
                const auto token = tmpl.substr(i + 1, close - i - 1);
                if (token.find('/') != std::string_view::npos) {
                    out += KoParticle(token, last_end == out.size() ? last_value : std::string_view{});
                } else {
                    last_value = {};
                    for (const auto& s : slots)
                        if (s.name == token) {
                            out += s.value;
                            last_value = s.value;
                            break;
                        }
                    last_end = out.size();
                }
                i = close + 1;
                continue;
            }
        }
        out += tmpl[i++];
    }
    return out;
}

// ---- the table in effect -------------------------------------------------------------------------

class Table {
public:
    // Every row resolved for variant v (labels through `label`; none: our text and English).
    void Build(std::span<const Row> rows, lp_lang::Variant v, const LabelFn& label) {
        variant = v;
        texts.clear();
        texts.reserve(rows.size());
        for (const auto& r : rows)
            texts.push_back(Resolve(r, v, label));
    }
    const std::string& Get(std::size_t index) const {
        static const std::string empty;
        return index < texts.size() ? texts[index] : empty;
    }
    lp_lang::Variant Variant() const {
        return variant;
    }
    std::size_t Size() const {
        return texts.size();
    }

private:
    lp_lang::Variant variant = lp_lang::Variant::En;
    std::vector<std::string> texts;
};

// ---- Pokédex order -------------------------------------------------------------------------------

// A sort key for a Latin name: letters folded to their base lower-case letter (é -> e, Œ -> oe),
// so accents sort with their letter; other code points stay as they are (Hangul code-point order
// is already 가나다 order). Ties are broken by the caller.
inline std::string FoldKey(std::string_view name) {
    std::string out;
    for (std::size_t i = 0; i < name.size();) {
        char32_t cp = lp_lang::NextCodepoint(name, i);
        if (cp >= 'A' && cp <= 'Z') {
            cp += 0x20;
        } else if (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7) {
            static constexpr char Base[] = "aaaaaaeceeeeiiiidnooooo_ouuuuyts" // U+00C0..U+00DF
                                           "aaaaaaeceeeeiiiidnooooo_ouuuuyty"; // U+00E0..U+00FF
            const char b = Base[cp - 0xC0];
            if (cp == 0xC6 || cp == 0xE6) {
                out += "ae";
                continue;
            }
            if (cp == 0xDF) {
                out += "ss";
                continue;
            }
            cp = static_cast<unsigned char>(b);
        } else if (cp == 0x152 || cp == 0x153) {
            out += "oe";
            continue;
        }
        detail::AppendUtf8(out, cp);
    }
    return out;
}

// Native Town Map WordSet slots1/2 are the saved rival and localized supporter names.
// An unreadable required name hides the sentence until ready, avoiding broken punctuation.
inline std::string MapGuide(std::string_view text, std::string_view rival, std::string_view supporter) {
    if ((text.find("{rival}") != std::string_view::npos && rival.empty()) ||
        (text.find("{supporter}") != std::string_view::npos && supporter.empty())) return {};
    return Fill(text, {{"rival", std::string{rival}}, {"supporter", std::string{supporter}}});
}

} // namespace lp_strings
