// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Binding of Isaac: a tiny, header-only XML reader for the game's data files (items.xml,
// stringtable.sta, *.anm2, ...). Enough of XML 1.0 for these files: elements, attributes (single or
// double quotes), character data, comments, processing instructions, CDATA and the predefined +
// numeric entities. No DTDs, no namespaces. Fails closed (returns false) on malformed input;
// bounded depth. Owner: ASSETS lane.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace isaac_xml {

struct Node {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<Node>> children;
    std::string text; // concatenated character data directly inside this element

    const std::string* Attr(std::string_view key) const {
        for (const auto& [k, v] : attrs)
            if (k == key)
                return &v;
        return nullptr;
    }
    std::string_view AttrOr(std::string_view key, std::string_view fallback = {}) const {
        const std::string* v = Attr(key);
        return v ? std::string_view{*v} : fallback;
    }
    /// First direct child element with this name, or null.
    const Node* Child(std::string_view child_name) const {
        for (const auto& c : children)
            if (c->name == child_name)
                return c.get();
        return nullptr;
    }
};

namespace detail {

inline void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// Decodes entities in [s, s+n) and appends to out. False on a malformed reference.
inline bool Unescape(std::string_view s, std::string& out) {
    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c != '&') {
            out.push_back(c);
            ++i;
            continue;
        }
        const std::size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 12)
            return false;
        const std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp")
            out.push_back('&');
        else if (ent == "lt")
            out.push_back('<');
        else if (ent == "gt")
            out.push_back('>');
        else if (ent == "quot")
            out.push_back('"');
        else if (ent == "apos")
            out.push_back('\'');
        else if (ent.size() >= 2 && ent[0] == '#') {
            std::uint32_t cp = 0;
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const std::string_view digits = ent.substr(hex ? 2 : 1);
            if (digits.empty())
                return false;
            for (const char d : digits) {
                std::uint32_t v;
                if (d >= '0' && d <= '9')
                    v = static_cast<std::uint32_t>(d - '0');
                else if (hex && d >= 'a' && d <= 'f')
                    v = static_cast<std::uint32_t>(d - 'a' + 10);
                else if (hex && d >= 'A' && d <= 'F')
                    v = static_cast<std::uint32_t>(d - 'A' + 10);
                else
                    return false;
                cp = cp * (hex ? 16 : 10) + v;
                if (cp > 0x10FFFF)
                    return false;
            }
            if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF))
                return false;
            AppendUtf8(out, cp);
        } else {
            return false;
        }
        i = semi + 1;
    }
    return true;
}

inline bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
inline bool IsNameChar(char c) {
    return !IsSpace(c) && c != '=' && c != '>' && c != '/' && c != '<' && c != '"' && c != '\'' &&
           c != '\0';
}

} // namespace detail

/// Parses a whole document; `root` receives the single top-level element.
inline bool Parse(std::string_view doc, Node& root) {
    using namespace detail;
    constexpr std::size_t MaxDepth = 64;
    constexpr std::size_t MaxNodes = 250000;
    root = Node{};
    std::size_t i = 0;
    const std::size_t n = doc.size();
    if (n > (std::size_t(32) << 20))
        return false;
    std::size_t nodes = 0;
    // UTF-8 BOM
    if (n >= 3 && static_cast<unsigned char>(doc[0]) == 0xEF &&
        static_cast<unsigned char>(doc[1]) == 0xBB && static_cast<unsigned char>(doc[2]) == 0xBF)
        i = 3;
    std::vector<Node*> stack;
    bool have_root = false;
    while (i < n) {
        if (doc[i] != '<') {
            const std::size_t lt = doc.find('<', i);
            const std::string_view chunk = doc.substr(i, (lt == std::string_view::npos ? n : lt) - i);
            if (!stack.empty()) {
                if (!Unescape(chunk, stack.back()->text))
                    return false;
            } else {
                for (const char c : chunk)
                    if (!IsSpace(c))
                        return false;
            }
            if (lt == std::string_view::npos)
                break;
            i = lt;
            continue;
        }
        if (doc.compare(i, 4, "<!--") == 0) {
            const std::size_t e = doc.find("-->", i + 4);
            if (e == std::string_view::npos)
                return false;
            i = e + 3;
            continue;
        }
        if (doc.compare(i, 9, "<![CDATA[") == 0) {
            const std::size_t e = doc.find("]]>", i + 9);
            if (e == std::string_view::npos || stack.empty())
                return false;
            stack.back()->text.append(doc.substr(i + 9, e - i - 9));
            i = e + 3;
            continue;
        }
        if (doc.compare(i, 2, "<?") == 0) {
            const std::size_t e = doc.find("?>", i + 2);
            if (e == std::string_view::npos)
                return false;
            i = e + 2;
            continue;
        }
        if (doc.compare(i, 2, "<!") == 0) { // DOCTYPE etc.: skip to '>'
            const std::size_t e = doc.find('>', i + 2);
            if (e == std::string_view::npos)
                return false;
            i = e + 1;
            continue;
        }
        if (doc.compare(i, 2, "</") == 0) {
            std::size_t j = i + 2;
            while (j < n && IsNameChar(doc[j]))
                ++j;
            const std::string_view nm = doc.substr(i + 2, j - i - 2);
            while (j < n && IsSpace(doc[j]))
                ++j;
            if (j >= n || doc[j] != '>' || stack.empty() || stack.back()->name != nm)
                return false;
            stack.pop_back();
            i = j + 1;
            continue;
        }
        // start tag
        std::size_t j = i + 1;
        while (j < n && IsNameChar(doc[j]))
            ++j;
        if (j == i + 1)
            return false;
        if (++nodes > MaxNodes)
            return false;
        Node* node;
        if (stack.empty()) {
            if (have_root)
                return false;
            have_root = true;
            node = &root;
        } else {
            if (stack.size() >= MaxDepth)
                return false;
            stack.back()->children.push_back(std::make_unique<Node>());
            node = stack.back()->children.back().get();
        }
        node->name.assign(doc.substr(i + 1, j - i - 1));
        bool self_close = false;
        for (;;) {
            while (j < n && IsSpace(doc[j]))
                ++j;
            if (j >= n)
                return false;
            if (doc[j] == '>') {
                ++j;
                break;
            }
            if (doc[j] == '/') {
                if (j + 1 >= n || doc[j + 1] != '>')
                    return false;
                self_close = true;
                j += 2;
                break;
            }
            const std::size_t ks = j;
            while (j < n && IsNameChar(doc[j]))
                ++j;
            if (j == ks)
                return false;
            std::string key{doc.substr(ks, j - ks)};
            while (j < n && IsSpace(doc[j]))
                ++j;
            if (j >= n || doc[j] != '=')
                return false;
            ++j;
            while (j < n && IsSpace(doc[j]))
                ++j;
            if (j >= n || (doc[j] != '"' && doc[j] != '\''))
                return false;
            const char q = doc[j++];
            const std::size_t ve = doc.find(q, j);
            if (ve == std::string_view::npos)
                return false;
            std::string value;
            if (!Unescape(doc.substr(j, ve - j), value))
                return false;
            if (node->attrs.size() >= 256 || node->Attr(key))
                return false;
            node->attrs.emplace_back(std::move(key), std::move(value));
            j = ve + 1;
        }
        if (!self_close)
            stack.push_back(node);
        i = j;
    }
    return have_root && stack.empty();
}

inline bool Parse(const std::vector<std::uint8_t>& bytes, Node& root) {
    return Parse(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()}, root);
}

/// Integer attribute (decimal, optional sign; a trailing fraction like "100.0" is truncated).
inline bool AttrInt(const Node& n, std::string_view key, long long& out) {
    const std::string* v = n.Attr(key);
    if (!v || v->empty())
        return false;
    std::size_t i = 0;
    bool neg = false;
    if ((*v)[0] == '-' || (*v)[0] == '+') {
        neg = (*v)[0] == '-';
        i = 1;
    }
    if (i >= v->size() || (*v)[i] < '0' || (*v)[i] > '9')
        return false;
    long long r = 0;
    for (; i < v->size() && (*v)[i] >= '0' && (*v)[i] <= '9'; ++i) {
        r = r * 10 + ((*v)[i] - '0');
        if (r > (1LL << 40))
            return false;
    }
    if (i < v->size() && (*v)[i] != '.')
        return false;
    out = neg ? -r : r;
    return true;
}
inline long long AttrIntOr(const Node& n, std::string_view key, long long fallback) {
    long long v;
    return AttrInt(n, key, v) ? v : fallback;
}

/// Floating attribute ("100", "-4", "2.5").
inline bool AttrFloat(const Node& n, std::string_view key, double& out) {
    const std::string* v = n.Attr(key);
    if (!v || v->empty())
        return false;
    std::size_t i = 0;
    bool neg = false;
    if ((*v)[0] == '-' || (*v)[0] == '+') {
        neg = (*v)[0] == '-';
        i = 1;
    }
    double r = 0;
    bool digits = false;
    for (; i < v->size() && (*v)[i] >= '0' && (*v)[i] <= '9'; ++i) {
        r = r * 10 + ((*v)[i] - '0');
        digits = true;
    }
    if (i < v->size() && (*v)[i] == '.') {
        double f = 0.1;
        for (++i; i < v->size() && (*v)[i] >= '0' && (*v)[i] <= '9'; ++i, f *= 0.1) {
            r += ((*v)[i] - '0') * f;
            digits = true;
        }
    }
    if (!digits || i != v->size() || !std::isfinite(r))
        return false;
    out = neg ? -r : r;
    return true;
}

} // namespace isaac_xml
