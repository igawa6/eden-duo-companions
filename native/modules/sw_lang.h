// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Companion language (2026-11-01, multi-language tiers a+b). The player picks it on the @settings
// page ("lang" choice flag, persisted by the runtime); the module reads "@flag:lang" every tick and
// the manifest switches its own labels with text_bind "@flag:lang" + text_map (gen_manifest.py).
//
// Sword ships every language in one romfs (bin/message/<Folder>/, 10 folders, identical file sets
// and line indices; analysis/20261101-multilanguage §1), so game text is just the folder; the
// baked-text art has per-language arc siblings (_eng/_fre/... suffix; the bare file is Japanese).
//
// The setting value IS the Lang value. Values 0..4 are the Latin-script languages, 5..6 Japanese
// (kanji, kana; tier c), 7..9 Korean / Simplified / Traditional Chinese (2026-11-01). Hangul and
// Chinese are not in the game's romfs fonts: Sword draws them from the console's shared system
// fonts (pl:u; analysis/20261101-cjk-font/PLU-EVIDENCE.md), which a module reads through the
// runtime 23 "sysfont:" source -- real firmware fonts only, empty otherwise. So 7..9 are offered
// only on a host that returns that language's font bytes (sw_modfont ProbeSysfonts, once per
// boot, SetSysfontOffered); an older host or one without real firmware fonts never offers them,
// and a stale persisted 7..9 reads as English.

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace sw_lang {

enum class Lang : int {
    English = 0,
    French = 1,
    German = 2,
    Italian = 3,
    Spanish = 4,
    // tier c (2026-11-01): Japanese in the game's own font (sw_modfont.h), both of the game's
    // scripts: kanji (JPN_KANJI, the default Japanese choice) and kana (JPN).
    JapaneseKanji = 5,
    Japanese = 6,
    // offered only when the console's shared system font for it is readable (runtime 23 sysfont:)
    Korean = 7,
    ChineseSimplified = 8,
    ChineseTraditional = 9,
};
inline constexpr int kCount = 10;
inline constexpr int kAlwaysOffered = 7; // settings values 0..6 (the package and romfs fonts)

inline constexpr std::string_view kFolders[kCount] = {
    "English", "French", "German", "Italian", "Spanish",
    "JPN_KANJI", "JPN", "Korean", "Simp_Chinese", "Trad_Chinese",
};
// appli arc suffix of the baked-text art; Japanese (both scripts) is the unsuffixed base file.
inline constexpr std::string_view kArtSuffixes[kCount] = {
    "_eng", "_fre", "_ger", "_ita", "_spa", "", "", "_kor", "_sch", "_tch",
};
// strings.json column codes (tools/pokemon-sword/strings.json).
inline constexpr std::string_view kCodes[kCount] = {
    "en", "fr", "de", "it", "es", "ja", "ja-kana", "ko", "zh-Hans", "zh-Hant",
};

inline bool IsCjk(Lang lang) { return static_cast<int>(lang) >= 5; }
inline bool IsJapanese(Lang lang) { return lang == Lang::JapaneseKanji || lang == Lang::Japanese; }
inline bool IsChinese(Lang lang) { return lang == Lang::ChineseSimplified || lang == Lang::ChineseTraditional; }
/// Drawn from a shared system font (Korean, Chinese): offered only while that font is readable.
inline bool NeedsSysfont(Lang lang) { return static_cast<int>(lang) >= kAlwaysOffered && static_cast<int>(lang) < kCount; }

/// Bit (1 << value) per sysfont language whose font bytes this host returned (sw_modfont
/// ProbeSysfonts sets it once per boot; one module per game process, atomic like CurrentRef).
inline std::atomic<std::uint32_t>& SysfontOfferRef() {
    static std::atomic<std::uint32_t> mask{0};
    return mask;
}
inline void SetSysfontOffered(Lang lang, bool offered) {
    const std::uint32_t bit = std::uint32_t{1} << static_cast<int>(lang);
    if (offered) SysfontOfferRef().fetch_or(bit, std::memory_order_relaxed);
    else SysfontOfferRef().fetch_and(~bit, std::memory_order_relaxed);
}
inline bool Offered(int value) {
    if (value >= 0 && value < kAlwaysOffered) return true;
    return value >= kAlwaysOffered && value < kCount &&
           (SysfontOfferRef().load(std::memory_order_relaxed) >> value & 1u) != 0;
}
/// The settings row's next value after `value` (wrapping), skipping every language not offered:
/// the manifest's language row sets its flag to this (module-published "sw.lang.next"), so an
/// unoffered Korean / Chinese is never shown as a choice.
inline int NextOffered(std::int64_t value) {
    int v = value >= 0 && value < kCount ? static_cast<int>(value) : -1;
    for (int step = 0; step < kCount; ++step) {
        v = (v + 1) % kCount;
        if (Offered(v)) return v;
    }
    return 0;
}

/// The settings value -> a language; anything not offered (a stale persisted value, Korean /
/// Chinese without their system font, an old host's fallback, -1) is English, the game-default
/// text of every earlier build.
inline Lang FromSetting(std::int64_t value) {
    return Offered(static_cast<int>(value)) && value == static_cast<int>(value) ? static_cast<Lang>(value)
                                                                               : Lang::English;
}
inline int Index(Lang lang) {
    const int i = static_cast<int>(lang);
    return i >= 0 && i < kCount ? i : 0;
}
inline std::string_view Folder(Lang lang) { return kFolders[Index(lang)]; }
inline std::string_view ArtSuffix(Lang lang) { return kArtSuffixes[Index(lang)]; }
inline std::string_view Code(Lang lang) { return kCodes[Index(lang)]; }

/// "romfs:/bin/message/<Folder>/<dir>/<file>": every game-text load goes through this (the 19
/// former "romfs:/bin/message/English/..." literals).
inline std::string MessagePath(Lang lang, std::string_view file, std::string_view dir = "common") {
    std::string path = "romfs:/bin/message/";
    path += Folder(lang);
    path += '/';
    path += dir;
    path += '/';
    path += file;
    return path;
}

// The language in effect for this module instance. The Reader sets it from "@flag:lang" (one
// module per game process; atomic because the tick/action callbacks may run on another thread), and
// every loader / string lookup reads it, so callers keep their old signatures.
inline std::atomic<int>& CurrentRef() {
    static std::atomic<int> current{0};
    return current;
}
inline Lang Current() { return static_cast<Lang>(CurrentRef().load(std::memory_order_relaxed)); }
inline void SetCurrent(Lang lang) { CurrentRef().store(Index(lang), std::memory_order_relaxed); }
inline std::string MessagePath(std::string_view file, std::string_view dir = "common") {
    return MessagePath(Current(), file, dir);
}

} // namespace sw_lang
