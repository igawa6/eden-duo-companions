// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Package-driven output filter. The host clears the module snapshot every tick, so every
// published key costs a map insert (string key + node) per sample, whether or not the package
// ever reads it. A module that publishes a wide contract (persona detail for every roster member,
// every confidant's portrait key, ...) pays for families the installed package never binds.
//
// The host hands the package manifest to create() as config_json. From it the filter collects
// every string the package could use as a value name (binds, gates, derived terms, map marker
// templates, dict keys; everything but the declarative "module_outputs" list and "_about"),
// normalises each one to its family (every digit run and every "{i...}" / "%d" placeholder
// becomes '#') and then drops only INDEXED keys (keys with a digit run) whose family the package
// never names. Keys without digits (states, gates, diagnostics such as item.stage) are always
// published. A config without "pages" (tests pass "{}") disables the filter: every key flows.
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace p5r_publish {

// FNV-1a over the key's family form, computed on the fly (no allocation).
struct FamilyHash {
    std::uint64_t h{dsmod_sdk::Fnv1a64Basis};
    bool indexed{};
    bool in_digits{};
    void Byte(unsigned char c) {
        h = dsmod_sdk::Fnv1a64Step(h, c);
    }
    void Char(char c) {
        if (c >= '0' && c <= '9') {
            indexed = true;
            if (!in_digits)
                Byte('#');
            in_digits = true;
            return;
        }
        in_digits = false;
        Byte(static_cast<unsigned char>(c));
    }
};

class Filter {
public:
    // Normalised family of a manifest string: digit runs, "{i}", "{i+N}", "{i-N}" and "%d" -> '#'.
    static std::uint64_t Family(std::string_view s) {
        FamilyHash f;
        for (size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '{' && i + 2 < s.size() && s[i + 1] == 'i') {
                const size_t close = s.find('}', i);
                if (close != std::string_view::npos) {
                    bool placeholder = true;
                    for (size_t j = i + 2; j < close; ++j)
                        placeholder &= s[j] == '+' || s[j] == '-' || (s[j] >= '0' && s[j] <= '9');
                    if (placeholder) {
                        f.Char('0');
                        i = close;
                        continue;
                    }
                }
            }
            if (c == '%' && i + 1 < s.size() && s[i + 1] == 'd') {
                f.Char('0');
                ++i;
                continue;
            }
            f.Char(c);
        }
        return f.h;
    }
    // Builds the family set from the manifest JSON text. Returns true when the filter is active.
    bool Configure(const char* config) {
        families.clear();
        active = false;
        if (!config)
            return false;
        const std::string_view text{config};
        size_t i = 0;
        int depth = 0;
        bool saw_pages = false;
        std::string value;
        auto read_string = [&](std::string& out) {
            // text[i] == '"'
            out.clear();
            for (++i; i < text.size(); ++i) {
                const char c = text[i];
                if (c == '"') {
                    ++i;
                    return true;
                }
                if (c == '\\' && i + 1 < text.size()) {
                    const char e = text[++i];
                    if (e == 'u') {
                        out.push_back('?'); // non-ASCII never forms a value name
                        i += 4;
                        continue;
                    }
                    out.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
                    continue;
                }
                out.push_back(c);
            }
            return false;
        };
        auto skip_value = [&]() {
            // skip one JSON value starting at text[i] (after ':' and blanks)
            int d = 0;
            std::string scratch;
            while (i < text.size()) {
                const char c = text[i];
                if (c == '"') {
                    if (!read_string(scratch))
                        return;
                    if (d == 0)
                        return;
                    continue;
                }
                if (c == '[' || c == '{')
                    ++d;
                else if (c == ']' || c == '}') {
                    if (--d <= 0) {
                        ++i;
                        return;
                    }
                } else if (d == 0 && (c == ',' || c == '}' || c == ']'))
                    return;
                ++i;
            }
        };
        while (i < text.size()) {
            const char c = text[i];
            if (c == '{' || c == '[') {
                ++depth;
                ++i;
                continue;
            }
            if (c == '}' || c == ']') {
                --depth;
                ++i;
                continue;
            }
            if (c != '"') {
                ++i;
                continue;
            }
            if (!read_string(value))
                break;
            size_t j = i;
            while (j < text.size() &&
                   (text[j] == ' ' || text[j] == '\n' || text[j] == '\r' || text[j] == '\t'))
                ++j;
            const bool is_key = j < text.size() && text[j] == ':';
            if (is_key && depth == 1) {
                if (value == "pages")
                    saw_pages = true;
                if (value == "module_outputs" || value == "_about") {
                    i = j + 1;
                    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' ||
                                               text[i] == '\r' || text[i] == '\t'))
                        ++i;
                    skip_value();
                    continue;
                }
            }
            Add(value);
        }
        active = saw_pages && !families.empty();
        if (active) {
            size_t size = 16;
            while (size < families.size() * 4)
                size <<= 1;
            table.assign(size, 0);
            mask = size - 1;
            for (std::uint64_t h : families) {
                h |= 1; // 0 marks an empty slot
                size_t at = static_cast<size_t>(h ^ (h >> 29)) & mask;
                while (table[at] && table[at] != h)
                    at = (at + 1) & mask;
                table[at] = h;
            }
        }
        return active;
    }
    // True when the key should be published.
    bool Wants(const char* key) const {
        if (!active)
            return true;
        FamilyHash f;
        for (const char* p = key; *p; ++p)
            f.Char(*p);
        if (!f.indexed)
            return true;
        const std::uint64_t h = f.h | 1;
        for (size_t at = static_cast<size_t>(h ^ (h >> 29)) & mask;; at = (at + 1) & mask) {
            if (table[at] == h)
                return true;
            if (!table[at])
                return false;
        }
    }
    bool Active() const {
        return active;
    }
    size_t Size() const {
        return families.size();
    }

private:
    void Add(std::string_view s) {
        // gates combine names with '+', '|' and ','; '!' negates
        size_t start = 0;
        for (size_t k = 0; k <= s.size(); ++k) {
            if (k == s.size() || s[k] == '+' || s[k] == '|' || s[k] == ',') {
                std::string_view part = s.substr(start, k - start);
                while (!part.empty() && (part.front() == '!' || part.front() == ' '))
                    part.remove_prefix(1);
                while (!part.empty() && part.back() == ' ')
                    part.remove_suffix(1);
                if (!part.empty())
                    families.insert(Family(part));
                start = k + 1;
            }
        }
    }
    bool active{};
    std::unordered_set<std::uint64_t> families;
    std::vector<std::uint64_t> table; // open addressing, load <= 1/4
    size_t mask{};
};

} // namespace p5r_publish
