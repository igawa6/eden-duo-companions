// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "acnh_lang_grammar.h"

#include <algorithm>
#include <cstdio>

#include "acnh_catalog.h"
#include "acnh_msbt.h"

namespace acnh {
namespace {

struct Token {
    bool tag = false;
    uint16_t group = 0, type = 0;
    std::vector<uint8_t> args; ///< tag argument bytes (incl. a pad byte when odd)
    std::u16string text;       ///< plain run
};

std::vector<Token> Tokenize(std::u16string_view s) {
    std::vector<Token> out;
    for (size_t i = 0; i < s.size();) {
        const char16_t u = s[i];
        if (u == 0)
            break;
        if (u == 0x0E && i + 3 < s.size()) {
            Token t;
            t.tag = true;
            t.group = s[i + 1];
            t.type = s[i + 2];
            const size_t len = s[i + 3];
            for (size_t b = 0; b < len; ++b) {
                const size_t k = i + 4 + b / 2;
                const char16_t w = k < s.size() ? s[k] : 0;
                t.args.push_back(static_cast<uint8_t>((b & 1 ? w >> 8 : w) & 0xFF));
            }
            out.push_back(std::move(t));
            i += 4 + (len + 1) / 2;
            continue;
        }
        if (u == 0x0F && i + 2 < s.size()) { // closing tag: no text
            i += 3;
            continue;
        }
        if (out.empty() || out.back().tag)
            out.emplace_back();
        out.back().text.push_back(u);
        ++i;
    }
    return out;
}

/// u16 byte-length prefixed UTF-16 strings in `a` from byte `from`.
std::vector<std::u16string> StringList(const std::vector<uint8_t>& a, size_t from) {
    std::vector<std::u16string> out;
    size_t p = from;
    while (p + 2 <= a.size()) {
        const size_t len = a[p] | size_t{a[p + 1]} << 8;
        p += 2;
        if (p + len > a.size())
            break;
        std::u16string s;
        for (size_t b = 0; b + 2 <= len; b += 2)
            s.push_back(static_cast<char16_t>(a[p + b] | a[p + b + 1] << 8));
        out.push_back(std::move(s));
        p += len;
    }
    return out;
}

struct Attr {
    bool ok = false;
    int gender = 0, indef = 0, def = 0;
};

Attr AttrOf(const std::vector<Token>& name) {
    Attr a;
    for (const auto& t : name)
        if (t.tag && t.group == 50 && (t.type == 0 || t.type == 26) && t.args.size() >= 3) {
            a.ok = true;
            a.gender = t.args[0], a.indef = t.args[1], a.def = t.args[2];
            break;
        }
    return a;
}

char16_t Upper(char16_t c, const std::vector<uint16_t>* table) {
    if (table && table->size() >= 2) {
        // the game's {lower, upper} pairs, sorted by lower (writer 0x1aa26d0 binary search)
        size_t lo = 0, hi = table->size() / 2;
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            const uint16_t l = (*table)[mid * 2];
            if (l == c)
                return static_cast<char16_t>((*table)[mid * 2 + 1]);
            if (l < c)
                lo = mid + 1;
            else
                hi = mid;
        }
        return c;
    }
    if ((c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return static_cast<char16_t>(c - 0x20);
    if (c >= 0x430 && c <= 0x44F)
        return static_cast<char16_t>(c - 0x20);
    return c;
}

class Writer {
public:
    Writer(Lang l, const std::vector<uint16_t>* t) : lang{l}, table{t} {}
    bool cap = false;   ///< capitalise the next character (+0x4ba)
    bool title = false; ///< capitalise every word of the next insertion (+0x234)
    std::u16string out;
    void Put(std::u16string_view s, bool words) {
        for (size_t i = 0; i < s.size(); ++i) {
            char16_t c = s[i];
            const bool word_start =
                words && (out.empty() || out.back() == u' ' || out.back() == 0xA0);
            if (cap || word_start) {
                const char16_t up = Upper(c, table);
                // Dutch: a capital "I" followed by "j" makes "IJ" (0x1aa2758, EUnl only)
                if (lang == Lang::EUnl && up == u'I' && i + 1 < s.size() && s[i + 1] == u'j') {
                    out.push_back(u'I');
                    out.push_back(u'J');
                    ++i;
                    cap = false;
                    continue;
                }
                c = up;
                cap = false;
            }
            out.push_back(c);
        }
    }

private:
    Lang lang;
    const std::vector<uint16_t>* table;
};

std::u16string ArticleText(const Msbt* articles, int index) {
    if (!articles || index <= 0)
        return {};
    char lab[8];
    std::snprintf(lab, sizeof lab, "%03d", index);
    const std::u16string* raw = articles->Raw(lab);
    if (!raw)
        return {};
    std::u16string plain; // an article entry's own tags (EUnl 002 "{10:16:}") write nothing
    for (const auto& t : Tokenize(*raw))
        if (!t.tag)
            plain += t.text;
    return plain;
}

} // namespace

std::string ComposeItemMessage(Lang lang, std::u16string_view message, const ItemNameRaw& item,
                               int count, const Msbt* articles,
                               const std::vector<uint16_t>* case_table) {
    const bool plural = count != 1 && !item.plural.empty();
    const std::vector<Token> name = Tokenize(plural ? item.plural : item.singular);
    const Attr attr = AttrOf(name);
    Writer w{lang, case_table};
    int case_index = 0;
    int article = 0; ///< 1 indefinite, 2 definite written before the insertion
    for (const auto& t : Tokenize(message)) {
        if (!t.tag) {
            w.Put(t.text, false);
            continue;
        }
        if (t.group == 50) {
            switch (t.type) {
            case 1: // definite article of the inserted item
            case 2: // indefinite article
                if (attr.ok) {
                    w.Put(ArticleText(articles, t.type == 2 ? attr.indef : attr.def), false);
                    article = t.type == 2 ? 1 : 2;
                }
                break;
            case 3:
                w.cap = true;
                break;
            case 4:
                w.cap = true;
                w.title = true;
                break;
            case 17: { // word by the inserted item's gender (4-byte header, then the strings)
                const auto list = StringList(t.args, 4);
                if (!list.empty() && attr.ok) {
                    const int k = std::clamp(attr.gender - 1, 0, static_cast<int>(list.size()) - 1);
                    w.Put(list[k], false);
                }
                break;
            }
            case 23:
                case_index = t.args.empty() ? 0 : t.args[0];
                break;
            case 29:
                w.cap = false;
                w.title = false;
                break;
            default:
                break;
            }
            continue;
        }
        if (t.group == 125) { // the item name
            const bool words = w.title;
            for (const auto& n : name) {
                if (!n.tag) {
                    w.Put(n.text, words);
                } else if (n.group == 50 && n.type == 22) {
                    const auto list = StringList(n.args, 0);
                    int k = case_index;
                    if (lang == Lang::EUnl) // adjective form by the article written (LEAD)
                        k = article == 1 ? 0 : article == 2 ? 2 : 1;
                    if (!list.empty())
                        w.Put(list[std::clamp(k, 0, static_cast<int>(list.size()) - 1)], words);
                } else if (n.group == 50 && n.type == 29) {
                    w.cap = false;
                }
            }
            w.title = false;
            w.cap = false;
            continue;
        }
        // other groups (0 colour / ruby / page, 10, 40, 90, 110, ...) write nothing here
    }
    return Utf16ToUtf8(w.out);
}

ItemNameRaw FindItemNameRaw(Catalog& catalog, Lang lang, uint16_t id) {
    return {catalog.ItemNameRaw(lang, id, false), catalog.ItemNameRaw(lang, id, true)};
}

std::string ComposeItemMessage(Catalog& catalog, Lang lang, std::string_view path,
                               std::string_view label, uint16_t id, int count,
                               const std::vector<uint16_t>* case_table) {
    const auto m = catalog.Message(lang, path);
    const std::u16string* raw = m ? m->Raw(label) : nullptr;
    if (!raw)
        return {};
    const auto articles = catalog.Message(lang, "String/STR_Article");
    return ComposeItemMessage(lang, *raw, FindItemNameRaw(catalog, lang, id), count, articles.get(),
                              case_table);
}

} // namespace acnh
