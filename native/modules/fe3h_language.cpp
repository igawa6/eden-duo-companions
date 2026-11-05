// SPDX-FileCopyrightText: Copyright 2026 Eden_Duo_Mode contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe3h_language.h"

namespace fe3h_language {

std::string CleanText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
        if (static_cast<unsigned char>(c) >= 0x20)
            out.push_back(c);
    return out;
}

std::string BuildProbe(const TextGetter& get) {
    std::string probe;
    for (const uint32_t index : ProbeIndices) {
        std::string text;
        if (!get(index, text))
            return {};
        text = CleanText(text);
        if (text.empty())
            return {};
        probe += text;
        probe += '|';
    }
    return probe;
}

int MatchLanguage(const std::string& probe, int preferred,
                  const std::function<bool(int, uint32_t, std::string&)>& get) {
    if (probe.empty())
        return -1;
    const auto matches = [&](int language) {
        return BuildProbe([&](uint32_t index, std::string& out) { return get(language, index, out); }) ==
               probe;
    };
    if (preferred >= 0 && preferred < Count && matches(preferred))
        return preferred;
    for (int language = 0; language < Count; ++language)
        if (language != preferred && matches(language))
            return language;
    return -1;
}

} // namespace fe3h_language
