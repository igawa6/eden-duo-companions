// SPDX-FileCopyrightText: Copyright 2026 Eden_Duo_Mode contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem: Three Houses (010055D009F78000): which language is the game running?
//
// The font the companion draws with depends on the language: the Han shapes of Japanese,
// Traditional and Simplified Chinese differ, and only the game's own language set fits
// (fe3h_font.h, CJK). The language value the game keeps (Reader::GameLanguage) can be wrong or
// unreadable, so the language is also found by the game's own words: a few terms of message part 1
// are read from the game's memory (Reader::LanguageProbe) and compared with the same terms of each
// language in the romfs message data (fe3h_text.h). The terms tell the CJK languages and German /
// French / Spanish / Italian apart. The regional variants (ENG_U / ENG_E, FRA_E / FRA_U, ESP_E /
// ESP_U) have the same words here, so the first of them (or the preferred one) is taken, which is
// all the font needs.
//
// msgdata language order: 0 JPN, 1 ENG_U, 2 ENG_E, 3 GER, 4 FRA_E, 5 FRA_U, 6 ESP_E, 7 ESP_U,
// 8 ITA, 9 KOR, 10 TWN, 11 CHN.

#include <cstdint>
#include <functional>
#include <string>

namespace fe3h_language {

inline constexpr int Count = 12;
// Message part 1 indices: Stats, Skill Level, Victory Conditions, Hit, Crit.
inline constexpr uint32_t ProbeIndices[] = {257, 259, 598, 283, 284};

// Reads one message-part-1 string by index; false when it is not available (yet).
using TextGetter = std::function<bool(uint32_t index, std::string& out)>;

// A game text made fit for comparing: the control codes (< 0x20) removed.
std::string CleanText(const std::string& text);
// The probe terms joined with '|' (control codes removed); empty when any of them is not available.
std::string BuildProbe(const TextGetter& get);
// The language whose terms equal `probe`: `preferred` first, then 0..11; -1 when none matches or
// the probe is empty. get(language, index, out) reads the message data of that language.
int MatchLanguage(const std::string& probe, int preferred,
                  const std::function<bool(int language, uint32_t index, std::string& out)>& get);

} // namespace fe3h_language
