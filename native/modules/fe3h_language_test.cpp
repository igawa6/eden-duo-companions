// SPDX-FileCopyrightText: Copyright 2026 Eden_Duo_Mode contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// fe3h_language: finding the game's language from a few of its words. Synthetic message data; no
// game data is needed.

#include "fe3h_language.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <string>

using namespace fe3h_language;

namespace {

// The five probe terms of a language, by message part 1 index (257, 259, 598, 283, 284).
const std::map<int, std::map<uint32_t, std::string>> Data = {
    {0, {{257, "stats-ja"}, {259, "skill-ja"}, {598, "win-ja"}, {283, "hit-ja"}, {284, "crit-ja"}}},
    {1, {{257, "Stats"}, {259, "Skill"}, {598, "Victory"}, {283, "Hit"}, {284, "Crit"}}},
    {2, {{257, "Stats"}, {259, "Skill"}, {598, "Victory"}, {283, "Hit"}, {284, "Crit"}}}, // = 1
    {9, {{257, "stats-ko"}, {259, "skill-ko"}, {598, "win-ko"}, {283, "hit-ko"}, {284, "crit-ko"}}},
    {10, {{257, "stats-tw"}, {259, "skill-tw"}, {598, "win-tw"}, {283, "hit-tw"}, {284, "crit-tw"}}},
};

bool Get(int language, uint32_t index, std::string& out) {
    const auto lang = Data.find(language);
    if (lang == Data.end())
        return false;
    const auto text = lang->second.find(index);
    if (text == lang->second.end())
        return false;
    out = text->second;
    return true;
}

std::string ProbeOf(int language) {
    return BuildProbe([&](uint32_t index, std::string& out) { return Get(language, index, out); });
}

} // namespace

int main() {
    // the probe is the five words joined with '|'
    assert(ProbeOf(1) == "Stats|Skill|Victory|Hit|Crit|");
    // control codes (colour tags) do not matter
    assert(BuildProbe([](uint32_t, std::string& out) { out = "a\x1b" "b"; return true; }) ==
           "ab|ab|ab|ab|ab|");
    // it is empty while any word is missing, or empty (the text manager is not loaded yet)
    assert(BuildProbe([](uint32_t, std::string&) { return false; }).empty());
    assert(BuildProbe([](uint32_t index, std::string& out) { out = index == 257 ? "x" : ""; return true; })
               .empty());
    assert(BuildProbe([](uint32_t index, std::string& out) { out = "x"; return index != 598; }).empty());

    // each language finds itself, with or without a hint, and a wrong hint does not fool it
    for (const int language : {0, 9, 10}) {
        const std::string probe = ProbeOf(language);
        assert(MatchLanguage(probe, -1, Get) == language);
        assert(MatchLanguage(probe, language, Get) == language);
        for (const int hint : {0, 1, 9, 10, 11, 99})
            assert(MatchLanguage(probe, hint, Get) == language);
    }
    // the regional twins have the same words: the hint or else the first of them
    assert(MatchLanguage(ProbeOf(1), -1, Get) == 1);
    assert(MatchLanguage(ProbeOf(1), 1, Get) == 1);
    assert(MatchLanguage(ProbeOf(1), 2, Get) == 2);
    assert(MatchLanguage(ProbeOf(1), 9, Get) == 1);
    // nothing to find
    assert(MatchLanguage("", 9, Get) == -1);
    assert(MatchLanguage("no|such|words|here|at all|", 9, Get) == -1);
    // an unreadable romfs finds nothing
    assert(MatchLanguage(ProbeOf(9), 9, [](int, uint32_t, std::string&) { return false; }) == -1);

    std::puts("fe3h_language_test: ok");
    return 0;
}
