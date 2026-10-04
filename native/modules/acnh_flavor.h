// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The companion's own flavour lines (owner, round 7: LANGUAGE OPTION C) for the screens that show
// before / instead of the game's text: the waiting page, the wrong-version page and the Pockets
// blocking card. Our own text (not game data), translated into the 14 game languages with each
// language's official names (Tom Nook, Isabelle, Dodo Airlines, Resident Services, NookPhone,
// Pockets, Dodo Code... looked up in that language's own String / LayoutMsg texts).
//
// One table (acnh_flavor.cpp, a TSV raw string) is the single source:
//  - the package generator parses it (packaging tools/gen_manifest.py, ACNH_FLAVOR_SRC) into one
//    UTF-8 MSBT per language (dualscreen/flavor/<REGION><LANG>.msbt) that the runtime picks by the
//    language the game is given (manifest "msbt" + "msbt_lang"; no game memory is read for it);
//  - the module adds the lines' code points to the font atlas glyph sets (Codepoints) and, on a
//    game build it cannot read, serves a font of every language's lines ("ui:<cap>:flavor") so the
//    version page can be drawn in the game font without knowing the language.
// The table also lists the ACNH builds the module knows (main NSO build id, first 16 hex digits ->
// the update's display version): the module loads on all of them, but reads game memory only on
// 3.0.3 (text-only mode elsewhere: font + version index, no reads).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "acnh_lang.h"

namespace acnh::flavor {

struct Line {
    Lang lang;
    std::string key;  ///< "wait.0.t", "ver.s", "card.2.t", ...
    std::string text; ///< UTF-8, '\n' = line break
};

struct Build {
    std::string id16;    ///< first 16 hex digits of the main NSO build id, upper case
    std::string version; ///< display version "3.0.3"
};

/// Every line of the table (the "have" template expanded per known build: have.<index>).
const std::vector<Line>& Lines();
/// One line, empty when missing.
std::string_view Text(Lang lang, std::string_view key);
/// The known builds in table order (index = the module's acnh.ver.idx).
const std::vector<Build>& Builds();
/// Index of a running build (hex, any case, 16..64 digits; the first 16 compared, digits from 32 on
/// must be zero) or -1.
int BuildIndex(std::string_view hex);
/// Code points of every line of one language (sorted, unique, >= 0x20).
std::vector<uint32_t> Codepoints(Lang lang);
/// Code points of every line of every language (the text-only font).
std::vector<uint32_t> CodepointsAll();
/// For the text-only font: which language's CJK face a code point should come from
/// (KRko for Hangul, CNzh / TWzh for Han code points only those lines use, else Count = main face).
Lang CjkFaceFor(uint32_t cp);
/// Number of waiting lines (wait.0 .. wait.<n-1>).
int WaitCount();

} // namespace acnh::flavor
