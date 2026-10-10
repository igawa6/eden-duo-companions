// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's text language for the Luminescent Platinum companion (header only, no game data).
//
// The language is the save's CONFIG.msg_lang_id (PlayerWork + 0xAC), chosen at New Game; the
// console language is not used. Each slot has its own message bundle and object prefix, its UI
// texture suffix and its game font (contracts/language-mods.md §3). Language mods overwrite a slot's
// files, so two variants share a slot and are told apart by their text:
//   - slot 2 holds LP's English or the PT-BR translation (english_ss_typename #1 "Lutador");
//   - slot 7 holds Castellano or Latin American Spanish (spanish_ss_wazaname #33 "Placaje" /
//     "Tacleada").
// <prefix>_ss_language_select SS_language_select_000 tells a translation mod from LP's stock text.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lp_lang {

// MessageEnumData.MsgLangId
inline constexpr int Jpn = 1, Usa = 2, Fra = 3, Ita = 4, Deu = 5, Esp = 7, Kor = 8, Sch = 9, Tch = 10;
inline constexpr int English = Usa;

enum class Variant : int { En, PtBR, Fr, De, EsES, Es419, Ko, ZhHans, ZhHant, Ja, It };

struct Info {
    int id = English;
    bool kanji = false;          // Japanese with kanji (jpn_kanji)
    std::string_view bundle;     // Message/<bundle>
    std::string_view prefix;     // object prefix: <prefix>_ss_wazaname, <prefix>_ss_monsname (common_msbt)
    std::string_view sfx;        // common_lang.<sfx>, logo_dia_<sfx>, text_dia_<sfx>_op_pushbutton
    std::string_view font_bundle; // Dpr/font/<font_bundle>
    std::string_view face;       // the Font object (m_Name) the game draws this language with
    bool cjk = false;            // a Han / kana / Hangul face: the glyph set comes from the text
};

inline bool Valid(int id) {
    return (id >= Jpn && id <= Deu) || (id >= Esp && id <= Tch);
}

// The slot's bundles and font; an unknown id is English.
inline Info Lookup(int id, bool kanji = false) {
    static constexpr std::string_view Latin = "FOT-UDKakugoC80Pro-DB";
    switch (Valid(id) ? id : English) {
    case Jpn:
        return kanji ? Info{Jpn, true, "jpn_kanji", "jpn_kanji", "jp", "jpn_font", "FOT-RodinNTLGPro-DB", true}
                     : Info{Jpn, false, "jpn", "jpn", "jp", "jpn_font", "FOT-RodinNTLGPro-DB", true};
    case Fra: return {Fra, false, "french", "french", "fr", "efigs_font", Latin, false};
    case Ita: return {Ita, false, "italian", "italian", "it", "efigs_font", Latin, false};
    case Deu: return {Deu, false, "german", "german", "ge", "efigs_font", Latin, false};
    case Esp: return {Esp, false, "spanish", "spanish", "sp", "efigs_font", Latin, false};
    case Kor: return {Kor, false, "korean", "korean", "ko", "kor_font", "nintendo_udsg-r_ko_003", true};
    case Sch: return {Sch, false, "simp_chinese", "simp_chinese", "si", "sch_font", "nintendo_udsg-r_org_zh-cn_003", true};
    case Tch: return {Tch, false, "trad_chinese", "trad_chinese", "tr", "tch_font", "nintendo_udjxh-db_zh-tw_003", true};
    default: return {Usa, false, "english", "english", "en", "efigs_font", Latin, false};
    }
}

// BCP 47 code of a variant (lp.lang.variant).
inline std::string_view Code(Variant v) {
    static constexpr std::array<std::string_view, 11> Codes{"en", "pt-BR", "fr", "de", "es-ES", "es-419",
                                                            "ko", "zh-Hans", "zh-Hant", "ja", "it"};
    const auto i = static_cast<std::size_t>(v);
    return i < Codes.size() ? Codes[i] : "en";
}

// LP's own text in every slot without a translation mod (also PT-BR's, which keeps it).
inline constexpr std::string_view StockSelectText = "Only English is available in the base mod.";

struct Detected {
    Variant variant = Variant::En;
    bool modded = false; // a translation mod fills the slot (not LP's stock text)
};

// select0: <prefix>_ss_language_select SS_language_select_000; move33: <prefix>_ss_wazaname
// labelIndex 33; type1: <prefix>_ss_typename labelIndex 1.
inline Detected Detect(int id, std::string_view select0, std::string_view move33, std::string_view type1) {
    Detected d;
    d.modded = !select0.empty() && !select0.starts_with(StockSelectText);
    switch (Valid(id) ? id : English) {
    case Jpn: d.variant = Variant::Ja; break;
    case Fra: d.variant = Variant::Fr; break;
    case Ita: d.variant = Variant::It; break;
    case Deu: d.variant = Variant::De; break;
    case Esp: d.variant = move33 == "Tacleada" ? Variant::Es419 : Variant::EsES; break;
    case Kor: d.variant = Variant::Ko; break;
    case Sch: d.variant = Variant::ZhHans; break;
    case Tch: d.variant = Variant::ZhHant; break;
    default:
        // PT-BR keeps LP's language-select text: only its translated tables tell it apart
        d.variant = type1 == "Lutador" ? Variant::PtBR : Variant::En;
        d.modded = d.variant == Variant::PtBR;
        break;
    }
    return d;
}

// ---- UTF-8 and code points ---------------------------------------------------------------------

// The code point at t[i] (advances i; a bad lead byte is read as itself).
inline char32_t NextCodepoint(std::string_view t, std::size_t& i) {
    const auto c = static_cast<unsigned char>(t[i]);
    const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    char32_t cp = n == 1 ? c : c & (0x3F >> (n - 1));
    for (int k = 1; k < n && i + k < t.size(); ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(t[i + k]) & 0x3F);
    i += n;
    return cp;
}

// Han, kana, CJK punctuation and full-width forms: scripts written without spaces between words.
// Hangul is not one of them (Korean puts spaces between words).
inline bool IsSpaceless(char32_t cp) {
    return (cp >= 0x3000 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

inline bool IsSentenceEnd(char32_t cp) {
    return cp == '.' || cp == '!' || cp == '?' || cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F;
}

// The game's text boxes break lines at fixed widths; the companion wraps by itself, so a break only
// stays after a sentence end (. ! ? 。 ！ ？). Every other break becomes a space, or nothing when
// the text on either side is written without spaces (Chinese, Japanese).
inline std::string Reflow(std::string_view t) {
    std::string out;
    out.reserve(t.size());
    for (std::size_t i = 0; i < t.size();) {
        if (t[i] != '\n') {
            const std::size_t start = i;
            NextCodepoint(t, i);
            out.append(t.substr(start, i - start));
            continue;
        }
        ++i;
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        char32_t last = 0;
        if (!out.empty()) {
            std::size_t s = out.size() - 1;
            while (s > 0 && (static_cast<unsigned char>(out[s]) & 0xC0) == 0x80)
                --s;
            std::size_t k = s;
            last = NextCodepoint(out, k);
        }
        char32_t next = 0;
        if (i < t.size()) {
            std::size_t k = i;
            next = NextCodepoint(t, k);
        }
        if (IsSentenceEnd(last))
            out += '\n';
        else if (!IsSpaceless(last) && !IsSpaceless(next))
            out += ' ';
    }
    return out;
}

inline std::vector<std::string> Reflow(std::vector<std::string> texts) {
    for (auto& t : texts)
        t = Reflow(t);
    return texts;
}

// A set of BMP code points (the font atlas is a dense run below U+10000).
class CodepointSet {
public:
    bool Has(char32_t cp) const {
        return cp < bits.size() && bits[cp];
    }
    // Adds cp; true when it is new. Controls and code points past the BMP are ignored.
    bool Add(char32_t cp) {
        if (cp < 0x20 || cp >= bits.size() || bits[cp])
            return false;
        bits[cp] = true;
        ++count;
        return true;
    }
    void AddText(std::string_view t) {
        for (std::size_t i = 0; i < t.size();)
            Add(NextCodepoint(t, i));
    }
    // The code points of t not in the set yet (each once).
    std::vector<std::uint32_t> Missing(std::string_view t) const {
        std::vector<std::uint32_t> out;
        for (std::size_t i = 0; i < t.size();) {
            const char32_t cp = NextCodepoint(t, i);
            if (cp >= 0x20 && cp < bits.size() && !bits[cp] &&
                std::find(out.begin(), out.end(), static_cast<std::uint32_t>(cp)) == out.end())
                out.push_back(static_cast<std::uint32_t>(cp));
        }
        return out;
    }
    std::vector<std::uint32_t> Sorted() const {
        std::vector<std::uint32_t> out;
        out.reserve(count);
        for (std::uint32_t cp = 0; cp < bits.size(); ++cp)
            if (bits[cp])
                out.push_back(cp);
        return out;
    }
    std::size_t Size() const {
        return count;
    }

private:
    std::vector<bool> bits = std::vector<bool>(0x10000, false);
    std::size_t count = 0;
};

// ---- labels ------------------------------------------------------------------------------------

// Message tables by label name: table (without the language prefix, e.g. "ss_btl_app") ->
// labelName -> text (the words' str concatenated; tag words carry no str and drop out).
using LabelMap = std::map<std::string, std::string, std::less<>>;
using TableMap = std::map<std::string, LabelMap, std::less<>>;

struct LabelTables {
    TableMap lang, english;

    // The text in the current language, else English (a missing or empty label), else "".
    std::string Get(std::string_view table, std::string_view label) const {
        if (auto s = Find(lang, table, label); !s.empty())
            return s;
        return Find(english, table, label);
    }

private:
    static std::string Find(const TableMap& m, std::string_view table, std::string_view label) {
        const auto t = m.find(table);
        if (t == m.end())
            return {};
        const auto l = t->second.find(label);
        return l == t->second.end() ? std::string{} : l->second;
    }
};

// The tables the catalog keeps by label (both in the current language and in English). Tables in
// Message/common_msbt are marked; the rest are in the language bundle.
struct LabelTableDef {
    std::string_view table;
    bool common = false;
};
inline constexpr std::array<LabelTableDef, 25> LabelTableList{{
    {"ss_language_select"}, {"ss_btl_app"},    {"ss_status"},     {"ss_box"},        {"ss_pokedex", true},
    {"ss_xmenu"},           {"dlp_adventure_note"}, {"dp_options"}, {"dp_poketch"},  {"ss_bag"},
    {"ss_btl_state"},       {"ss_bag_pocket"}, {"ss_btl_pokelist"}, {"ss_typename"}, {"ss_wazaname"},
    {"ss_itemname"},        {"ss_tokusei"},    {"ss_monsname", true}, {"dp_townmap"},
    // the Pokédex page: category, height and weight (already in the language's units), form names
    {"ss_zkn_type", true},  {"ss_zkn_height", true}, {"ss_zkn_weight", true}, {"ss_zkn_form", true},
    {"dp_characters"}, // localized native supporter names used by Town Map guidance
    {"ss_xmenu_timeline"}, // the native Town Map next-destination guidance
}};

} // namespace lp_lang
