// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Companion language (2026-11-01): MessagePath per language, the settings value mapping (Korean /
// Chinese only while offered, the row's next offered value), the string table's completeness
// (every key in all ten languages, matching format codes, every Latin code point drawable by the
// package font, Chinese without spaces between Han), the module's text in French / Japanese /
// Korean / Chinese, French / Japanese / Korean / Chinese name decodes through the real loaders
// with a fake romfs, the zone-name columns, and line breaking (hyphens, U+3000, Chinese joins).
// F13 (game words read at run time): the game-line cleanup and fallback with a fake romfs; with
// SW_ROMFS_DIR (an extracted romfs, never in the repository) the module's runtime game text and
// Wild Area zone names equal the baked tables they replaced (sw_game_text_fixture.inc hashes).
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "sw_font_metrics.h"
#include "sw_helper_mailbox.h"
#include "sw_lang.h"
#include "sw_game_text_fixture.inc"
#include "sw_move_names.h"
#include "sw_party_details.h"
#include "sw_species_names.h"
#include "sw_strings.h"
#include "sw_test_romfs.h"
#include "sw_text.h"
#include "sw_text_fit.h"
#include "sw_zone.h"

namespace {
using sw_lang::Lang;

std::map<std::string, std::vector<unsigned char>> g_files;
std::vector<std::string> g_reads;

std::size_t FakeReadRomfs(void*, const char* path, std::uint64_t offset, void* out, std::size_t size) {
    g_reads.emplace_back(path);
    const auto it = g_files.find(path);
    if (it == g_files.end()) return 0;
    if (!out) return it->second.size();
    if (offset > it->second.size()) return 0;
    const std::size_t n = std::min<std::size_t>(size, it->second.size() - offset);
    std::memcpy(out, it->second.data() + offset, n);
    return n;
}

void Put16(std::vector<unsigned char>& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<unsigned char>(v);
    b[at + 1] = static_cast<unsigned char>(v >> 8);
}
void Put32(std::vector<unsigned char>& b, std::size_t at, std::uint32_t v) {
    Put16(b, at, v & 0xFFFF);
    Put16(b, at + 2, v >> 16);
}

// UTF-8 -> UTF-16 units (BMP only, enough for the fixtures).
std::vector<std::uint16_t> Units(const std::string& s) {
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { out.push_back(c); i += 1; }
        else if ((c & 0xE0) == 0xC0) { out.push_back(static_cast<std::uint16_t>(((c & 0x1F) << 6) | (s[i + 1] & 0x3F))); i += 2; }
        else { out.push_back(static_cast<std::uint16_t>(((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F))); i += 3; }
    }
    return out;
}

// The game's encrypted message table format (sw_text.h ReadDat), as the reader test builds it.
std::vector<unsigned char> Dat(const std::vector<std::string>& lines) {
    std::vector<std::vector<std::uint16_t>> units;
    std::size_t bytes = 16 + 4 + 8 * lines.size();
    for (const auto& line : lines) {
        units.push_back(Units(line));
        bytes += (units.back().size() + 1) * 2;
    }
    std::vector<unsigned char> out(bytes);
    Put16(out, 0, 1); Put16(out, 2, static_cast<std::uint32_t>(lines.size()));
    Put32(out, 4, static_cast<std::uint32_t>(bytes - 16)); Put32(out, 12, 16); Put32(out, 16, static_cast<std::uint32_t>(bytes - 16));
    std::size_t offset = 4 + 8 * lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& text = units[i];
        Put32(out, 20 + i * 8, static_cast<std::uint32_t>(offset));
        Put16(out, 20 + i * 8 + 4, static_cast<std::uint32_t>(text.size() + 1));
        auto key = static_cast<std::uint16_t>(sw_text::kBaseKey + i * sw_text::kAdvanceKey);
        for (std::size_t j = 0; j <= text.size(); ++j) {
            const std::uint16_t value = j == text.size() ? 0 : text[j];
            Put16(out, 16 + offset + j * 2, value ^ key);
            key = sw_text::RotateLeft3(key);
        }
        offset += (text.size() + 1) * 2;
    }
    return out;
}

EdenDsmodHostApi Host() {
    EdenDsmodHostApi host{};
    host.read_romfs = &FakeReadRomfs;
    return host;
}

void TestMessagePathPerLanguage() {
    assert(sw_lang::MessagePath(Lang::English, "monsname.dat") == "romfs:/bin/message/English/common/monsname.dat");
    assert(sw_lang::MessagePath(Lang::French, "wazaname.dat") == "romfs:/bin/message/French/common/wazaname.dat");
    assert(sw_lang::MessagePath(Lang::German, "itemname.dat") == "romfs:/bin/message/German/common/itemname.dat");
    assert(sw_lang::MessagePath(Lang::Italian, "tokusei.dat") == "romfs:/bin/message/Italian/common/tokusei.dat");
    assert(sw_lang::MessagePath(Lang::Spanish, "place_name.dat", "script") ==
           "romfs:/bin/message/Spanish/script/place_name.dat");
    // Japanese (tier c): kanji and kana are the game's two Japanese folders
    assert(sw_lang::MessagePath(Lang::JapaneseKanji, "monsname.dat") == "romfs:/bin/message/JPN_KANJI/common/monsname.dat");
    assert(sw_lang::MessagePath(Lang::Japanese, "monsname.dat") == "romfs:/bin/message/JPN/common/monsname.dat");
    assert(sw_lang::MessagePath(Lang::JapaneseKanji, "place_name.dat", "script") ==
           "romfs:/bin/message/JPN_KANJI/script/place_name.dat");
    // Korean / Chinese (offered with their shared system font)
    assert(sw_lang::MessagePath(Lang::Korean, "monsname.dat") == "romfs:/bin/message/Korean/common/monsname.dat");
    assert(sw_lang::MessagePath(Lang::ChineseSimplified, "x.dat") == "romfs:/bin/message/Simp_Chinese/common/x.dat");
    assert(sw_lang::MessagePath(Lang::ChineseTraditional, "x.dat") == "romfs:/bin/message/Trad_Chinese/common/x.dat");
    assert(sw_lang::ArtSuffix(Lang::English) == "_eng" && sw_lang::ArtSuffix(Lang::Spanish) == "_spa");
    assert(sw_lang::ArtSuffix(Lang::Japanese).empty() && sw_lang::ArtSuffix(Lang::JapaneseKanji).empty() &&
           sw_lang::ArtSuffix(Lang::Korean) == "_kor");
    assert(sw_lang::ArtSuffix(Lang::ChineseSimplified) == "_sch" && sw_lang::ArtSuffix(Lang::ChineseTraditional) == "_tch");
    assert(sw_lang::Code(Lang::Korean) == "ko" && sw_lang::Code(Lang::ChineseSimplified) == "zh-Hans" &&
           sw_lang::Code(Lang::ChineseTraditional) == "zh-Hant");
    assert(sw_lang::Code(Lang::JapaneseKanji) == "ja" && sw_lang::Code(Lang::Japanese) == "ja-kana");
    // The current-language overload follows SetCurrent.
    sw_lang::SetCurrent(Lang::German);
    assert(sw_lang::MessagePath("btl_app.dat") == "romfs:/bin/message/German/common/btl_app.dat");
    sw_lang::SetCurrent(Lang::English);
}

void TestSettingValue() {
    sw_lang::SysfontOfferRef() = 0;
    assert(sw_lang::FromSetting(0) == Lang::English);
    assert(sw_lang::FromSetting(1) == Lang::French);
    assert(sw_lang::FromSetting(4) == Lang::Spanish);
    assert(sw_lang::FromSetting(5) == Lang::JapaneseKanji);
    assert(sw_lang::FromSetting(6) == Lang::Japanese);
    // not offered (Korean/Chinese without their shared system font, junk, an old host's -1): English
    assert(sw_lang::FromSetting(7) == Lang::English);
    assert(sw_lang::FromSetting(8) == Lang::English);
    assert(sw_lang::FromSetting(9) == Lang::English);
    assert(sw_lang::FromSetting(10) == Lang::English); // a stress build's pseudo-locale
    assert(sw_lang::FromSetting(-1) == Lang::English);
    assert(sw_lang::FromSetting(1LL << 40) == Lang::English);
    // the settings row's next value skips what is not offered: 0..6 wrap without fonts
    assert(sw_lang::NextOffered(0) == 1 && sw_lang::NextOffered(5) == 6);
    assert(sw_lang::NextOffered(6) == 0 && sw_lang::NextOffered(7) == 0 && sw_lang::NextOffered(9) == 0);
    assert(sw_lang::NextOffered(-1) == 0 && sw_lang::NextOffered(99) == 0);
    // only Korean's font: 6 -> 7 -> 0
    sw_lang::SetSysfontOffered(Lang::Korean, true);
    assert(sw_lang::Offered(7) && !sw_lang::Offered(8) && !sw_lang::Offered(9));
    assert(sw_lang::FromSetting(7) == Lang::Korean && sw_lang::FromSetting(8) == Lang::English);
    assert(sw_lang::NextOffered(6) == 7 && sw_lang::NextOffered(7) == 0 && sw_lang::NextOffered(8) == 0);
    // all three: 6 -> 7 -> 8 -> 9 -> 0
    sw_lang::SetSysfontOffered(Lang::ChineseSimplified, true);
    sw_lang::SetSysfontOffered(Lang::ChineseTraditional, true);
    assert(sw_lang::NextOffered(7) == 8 && sw_lang::NextOffered(8) == 9 && sw_lang::NextOffered(9) == 0);
    assert(sw_lang::FromSetting(9) == Lang::ChineseTraditional);
    // Simplified withdrawn: 7 -> 9
    sw_lang::SetSysfontOffered(Lang::ChineseSimplified, false);
    assert(sw_lang::NextOffered(7) == 9 && sw_lang::FromSetting(8) == Lang::English);
    assert(!sw_lang::Offered(10) && !sw_lang::Offered(-1));
    sw_lang::SysfontOfferRef() = 0;
}

std::string Conversions(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p) {
        if (*p != '%') continue;
        ++p;
        while (*p && std::strchr("0123456789.-", *p)) ++p;
        if (!*p) break; // a trailing '%' (num.percent) is text, not a conversion
        out += *p;
    }
    return out;
}

// Every language's game lines from the real romfs (true), or none loaded (false: no SW_ROMFS_DIR).
bool LoadRealGameText() {
    sw_str::UnloadGameForTest();
    if (!sw_test::RomfsDir()) return false;
    const auto host = sw_test::RomfsHost();
    for (int l = 0; l < sw_lang::kCount; ++l) assert(sw_str::LoadGame(host, static_cast<Lang>(l)) == 0);
    return true;
}

void TestStringTableComplete() {
    const int count = static_cast<int>(sw_str::Id::kCount);
    assert(count > 150);
    const bool real = LoadRealGameText();
    if (!real) std::cout << "SW_ROMFS_DIR not set: game lines checked as their fallbacks\n";
    int game_rows = 0;
    for (int i = 0; i < count; ++i) {
        const auto id = static_cast<sw_str::Id>(i);
        assert(sw_str::Find(sw_str::kKeys[i]) == id);
        if (sw_str::IsGame(id)) {
            ++game_rows;
            // the companion's own English placeholder, never empty
            assert(sw_str::kGame[i].fallback && *sw_str::kGame[i].fallback);
            if (!real) {
                for (int l = 0; l < sw_lang::kCount; ++l)
                    assert(sw_str::kText[i][l] || std::string(sw_str::S(id, static_cast<Lang>(l))) == sw_str::kGame[i].fallback);
                continue;
            }
        }
        const std::string english = sw_str::S(id, Lang::English);
        for (int l = 0; l < sw_lang::kCount; ++l) {
            const char* text = sw_str::S(id, static_cast<Lang>(l));
            if (!text || !*text) {
                std::cerr << "missing " << sw_str::kKeys[i] << " in " << sw_lang::kCodes[l] << "\n";
                assert(false);
            }
            // the same printf conversions in every language
            if (Conversions(text) != Conversions(english.c_str())) {
                std::cerr << "format mismatch " << sw_str::kKeys[i] << " " << sw_lang::kCodes[l] << "\n";
                assert(false);
            }
            // Latin languages: every code point has a glyph in the package font (no U+202F, no
            // tofu). Japanese / Korean / Chinese are drawn from the module's face chains:
            // sw_modfont_test checks them in the real fonts, gen_manifest.py against the
            // <code>_font_metrics.json tables.
            const std::string s = text;
            if (sw_lang::IsCjk(static_cast<Lang>(l))) {
                assert(s.find("  ") == std::string::npos || english.find("  ") != std::string::npos); // no doubled ASCII spaces
                // phrases are separated by U+0020 (the runtime's only break), never U+3000
                assert(s.find("\xE3\x80\x80") == std::string::npos);
                // Chinese: no space between two Han characters (the module would drop it anyway)
                if (sw_lang::IsChinese(static_cast<Lang>(l))) assert(sw_text_fit::JoinCjkSpaces(s) == s);
                continue;
            }
            for (std::size_t k = 0; k < s.size();) {
                const unsigned char c = static_cast<unsigned char>(s[k]);
                char32_t cp = c;
                std::size_t n = 1;
                if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (s[k + 1] & 0x3F); n = 2; }
                else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | ((s[k + 1] & 0x3F) << 6) | (s[k + 2] & 0x3F); n = 3; }
                if (cp != '\n' && !sw_font::Covers(cp)) {
                    std::cerr << "no glyph U+" << std::hex << static_cast<unsigned>(cp) << std::dec << " in "
                              << sw_str::kKeys[i] << " " << sw_lang::kCodes[l] << "\n";
                    assert(false);
                }
                k += n;
            }
        }
        // Japanese, Korean and Chinese have their own text for every word
        if (english.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos &&
            english.find('%') == std::string::npos && english.size() > 3) {
            for (const Lang cjk : {Lang::JapaneseKanji, Lang::Japanese, Lang::Korean, Lang::ChineseSimplified,
                                   Lang::ChineseTraditional}) {
                if (english == sw_str::S(id, cjk)) {
                    std::cerr << "untranslated " << sw_str::kKeys[i] << " in " << sw_lang::Code(cjk) << "\n";
                    assert(false);
                }
            }
        }
    }
    assert(game_rows == 85);
}

// F13: a game line is read and cleaned at run time; what cannot be read shows the placeholder.
void TestGameLineCleanup() {
    using namespace sw_str;
    std::string out;
    assert(CleanGameLine("You can\xE2\x80\x99t fly there yet.\\c", 0, out) && out == "You can\xE2\x80\x99t fly there yet.");
    assert(CleanGameLine("A[WAIT 30]B\\r", 0, out) && out == "AB");
    assert(CleanGameLine("line one\\nline two\\c", 0, out) && out == "line one\nline two");
    assert(CleanGameLine("line one\\nline two", kFixFlat, out) && out == "line one line two");
    assert(CleanGameLine(" Back", kFixTrim, out) && out == "Back");
    assert(CleanGameLine("Accuracy", kFixSpace, out) && out == "Accuracy ");
    assert(CleanGameLine("Nr. [VAR 0202]", kFixNum, out) && out == "Nr. %03d");
    assert(CleanGameLine("Card della Lega Pok\xC3\xA9mon", kFixWords3, out) && out == "Card della Lega");
    assert(CleanGameLine("Card", kFixWords3, out) && out == "Card");
    // U+3000 -> the runtime's break (ASCII space)
    assert(CleanGameLine("\xE4\xBB\x8A\xE3\x81\xAF\xE3\x80\x80\xE4\xBD\xBF", 0, out) && out == "\xE4\xBB\x8A\xE3\x81\xAF \xE4\xBD\xBF");
    // not plain text: nothing (the caller shows the placeholder); `out` untouched
    out = "keep";
    assert(!CleanGameLine("No. [VAR 0202]", 0, out) && out == "keep");      // number code without num
    assert(!CleanGameLine("[VAR 0100] used it", 0, out) && out == "keep");
    assert(!CleanGameLine("[~ 12]", 0, out) && !CleanGameLine("a\\u0001", 0, out) && !CleanGameLine("50%", 0, out));
    assert(!CleanGameLine(" ", kFixTrim, out) && !CleanGameLine("", 0, out) && out == "keep");
}

void TestGameTextFromFakeRomfs() {
    using sw_str::Id;
    g_files.clear();
    sw_str::UnloadGameForTest();
    const auto host = Host();
    // only French btl_app.dat exists: its lines come from it (cleaned), everything else is the
    // English placeholder, and LoadGame counts the shortfall once
    std::vector<std::string> btl(70, "x");
    btl[2] = "Attaque";
    btl[7] = "N.";
    btl[33] = " Retour";
    g_files["romfs:/bin/message/French/common/btl_app.dat"] = Dat(btl);
    int game_rows = 0, from_btl_app = 0; // French lines read from the game (no companion column)
    const int fr = sw_lang::Index(Lang::French);
    for (int i = 0; i < static_cast<int>(Id::kCount); ++i) {
        if (!sw_str::IsGame(static_cast<Id>(i)) || sw_str::kText[i][fr]) continue;
        ++game_rows;
        if (std::string(sw_str::kGame[i].file) == "btl_app.dat") ++from_btl_app;
    }
    const int missing = sw_str::LoadGame(host, Lang::French);
    assert(from_btl_app == 12 && missing == game_rows - from_btl_app);
    assert(sw_str::LoadGame(host, Lang::French) == 0); // loaded once: no second report
    assert(std::string(sw_str::S(Id::kCmdFight, Lang::French)) == "Attaque");
    assert(std::string(sw_str::S(Id::kCmdBack, Lang::French)) == "Retour");
    assert(std::string(sw_str::S(Id::kLevelPrefix, Lang::French)) == "N.");
    assert(std::string(sw_str::S(Id::kType9, Lang::French)) == sw_str::kGame[static_cast<int>(Id::kType9)].fallback);
    assert(std::string(sw_str::S(Id::kStatHp, Lang::French)) == sw_str::kGame[static_cast<int>(Id::kStatHp)].fallback);
    assert(std::string(sw_str::S(Id::kCommonUse, Lang::French)) == "x");
    // a language never loaded shows the placeholders; companion text is unaffected
    assert(std::string(sw_str::S(Id::kCmdBack, Lang::German)) == sw_str::kGame[static_cast<int>(Id::kCmdBack)].fallback);
    assert(std::string(sw_str::S(Id::kCmdFight, Lang::English)) == "Fight"); // the companion's own English
    assert(std::string(sw_str::S(Id::kBagEmpty, Lang::German)) == "Diese Tasche ist leer");
    // the placeholders are the companion's, not the game's words
    for (int i = 0; i < static_cast<int>(Id::kCount); ++i)
        if (sw_str::IsGame(static_cast<Id>(i))) assert(std::strlen(sw_str::kGame[i].fallback) > 0);
    sw_str::UnloadGameForTest();
    g_files.clear();
}

// With the real romfs: every game line of every language equals what the module showed when the
// text was baked in (b92fe1b), except the six English near copies now in the game's own wording
// (sw_game_text_fixture.inc lists them).
void TestGameTextMatchesBakedTable() {
    if (!LoadRealGameText()) {
        std::cout << "SW_ROMFS_DIR not set: runtime game text vs baked table skipped\n";
        return;
    }
    int checked = 0;
    for (const auto& row : sw_fixture::kGameTextHash) {
        const auto id = sw_str::Find(row.key);
        assert(id != sw_str::Id::kCount && sw_str::IsGame(id));
        for (int l = 0; l < sw_lang::kCount; ++l) {
            if (sw_test::Fnv(sw_str::S(id, static_cast<Lang>(l))) != row.hash[l]) {
                std::cerr << "game text differs from the baked table: " << row.key << " " << sw_lang::kCodes[l] << "\n";
                assert(false);
            }
            ++checked;
        }
    }
    assert(checked == 850);
    sw_str::UnloadGameForTest();
}

void TestModuleTextFollowsLanguage() {
    using sw_helper::Result;
    using sw_str::Region;
    if (!LoadRealGameText()) {
        std::cout << "SW_ROMFS_DIR not set: module game text per language skipped\n";
        return;
    }
    sw_lang::SetCurrent(Lang::English);
    // the game's own line (townmap.dat 15), read from the romfs
    assert(std::string(sw_helper::ResultText(Result::FlyLocked)) == "You can\xE2\x80\x99t fly there yet.");
    assert(sw_str::Level(50) == "Lv.50");
    assert(std::string(sw_str::TypeName(9)) == "Fire");
    sw_lang::SetCurrent(Lang::French);
    // game wording (townmap.dat line 15), not a companion translation
    assert(std::string(sw_helper::ResultText(Result::FlyLocked)) ==
           "Vous ne pouvez pas appeler de taxi volant\nmaintenant.");
    assert(sw_str::Level(50) == "N.50");
    assert(std::string(sw_str::TypeName(9)) == "Feu");
    assert(std::string(sw_str::RegionName(Region::kArmor)) == "Isolarmure");
    // Japanese: the game's own words (townmap.dat 15, btl_app.dat 7, typename 9, place_name 165)
    sw_lang::SetCurrent(Lang::JapaneseKanji);
    assert(std::string(sw_helper::ResultText(Result::FlyLocked)) == "\xE4\xBB\x8A\xE3\x81\xAF \xE4\xBD\xBF\xE3\x81\x88\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93"); // 今は　使えません
    assert(sw_str::Level(50) == "Lv.50");
    assert(std::string(sw_str::TypeName(9)) == "\xE3\x81\xBB\xE3\x81\xAE\xE3\x81\x8A"); // ほのお
    assert(std::string(sw_str::RegionName(Region::kArmor)) == "\xE3\x83\xA8\xE3\x83\xAD\xE3\x82\xA4\xE5\xB3\xB6"); // ヨロイ島
    sw_lang::SetCurrent(Lang::Japanese);
    assert(std::string(sw_str::RegionName(Region::kArmor)) == "\xE3\x83\xA8\xE3\x83\xAD\xE3\x82\xA4\xE3\x81\x98\xE3\x81\xBE"); // ヨロイじま
    assert(std::string(sw_helper::ResultText(Result::FlyLocked)) == "\xE3\x81\x84\xE3\x81\xBE\xE3\x81\xAF \xE3\x81\xA4\xE3\x81\x8B\xE3\x81\x88\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93"); // いまは　つかえません
    // Korean / Chinese: the game's own words too (townmap.dat 15, typename 9, place_name 165)
    sw_lang::SetCurrent(Lang::Korean);
    assert(std::string(sw_helper::ResultText(Result::FlyLocked)) == "\xEC\xA7\x80\xEA\xB8\x88\xEC\x9D\x80 \xEC\x93\xB8 \xEC\x88\x98 \xEC\x97\x86\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4"); // 지금은 쓸 수 없습니다
    assert(std::string(sw_str::TypeName(9)) == "\xEB\xB6\x88\xEA\xBD\x83"); // 불꽃
    assert(std::string(sw_str::RegionName(Region::kArmor)) == "\xEA\xB0\x91\xEC\x98\xB7\xEC\x84\xAC"); // 갑옷섬
    assert(sw_str::Level(50) == "Lv.50");
    sw_lang::SetCurrent(Lang::ChineseSimplified);
    assert(std::string(sw_helper::ResultText(Result::FlyLocked)) == "\xE7\x8E\xB0\xE5\x9C\xA8\xE6\x97\xA0\xE6\xB3\x95\xE4\xBD\xBF\xE7\x94\xA8\xE3\x80\x82"); // 现在无法使用。
    assert(std::string(sw_str::TypeName(9)) == "\xE7\x81\xAB"); // 火
    assert(std::string(sw_str::RegionName(Region::kArmor)) == "\xE9\x93\xA0\xE5\xB2\x9B"); // 铠岛
    sw_lang::SetCurrent(Lang::ChineseTraditional);
    assert(std::string(sw_str::RegionName(Region::kArmor)) == "\xE9\x8E\xA7\xE5\xB3\xB6"); // 鎧島
    assert(std::string(sw_str::TypeName(15)) == "\xE9\xBE\x8D"); // 龍
    sw_lang::SetCurrent(Lang::German);
    assert(sw_str::Level(7) == "Lv. 7");
    assert(sw_str::Format(sw_str::Id::kDexNumber, 25) == "Nr. 025");
    sw_lang::SetCurrent(Lang::English);
    sw_str::UnloadGameForTest();
}

void TestFrenchSpeciesAndMoveNames() {
    g_files.clear();
    g_reads.clear();
    // monsname line == species id; wazaname line == move id. The French table's own text,
    // including U+202F (narrow no-break space, which the font lacks) -> U+00A0 in the decode.
    g_files["romfs:/bin/message/French/common/monsname.dat"] =
        Dat({"\xC5\x92uf", "Bulbizarre", "Herbizarre", "Florizarre", "Salam\xC3\xA8""che"});
    g_files["romfs:/bin/message/French/common/wazaname.dat"] =
        Dat({"\xE2\x80\x94", "\xC3\x89""cras\xE2\x80\x99""Face", "Poing-Karat\xC3\xA9", "Torgnoles\xE2\x80\xAF!"});
    g_files["romfs:/bin/message/English/common/monsname.dat"] = Dat({"Egg", "Bulbasaur"});
    const auto host = Host();
    sw_lang::SetCurrent(Lang::French);
    std::vector<std::string> species, moves;
    assert(sw_species_names::LoadNames(host, species));
    assert(sw_move_names::LoadNames(host, moves));
    std::string name;
    assert(sw_species_names::SpeciesName(species, 1, name) && name == "Bulbizarre");
    assert(sw_species_names::SpeciesName(species, 4, name) && name == "Salam\xC3\xA8""che");
    assert(sw_move_names::MoveName(moves, 1, name) && name == "\xC3\x89""cras\xE2\x80\x99""Face");
    assert(sw_move_names::MoveName(moves, 3, name) && name == "Torgnoles\xC2\xA0!");
    for (const auto& path : g_reads) assert(path.find("/English/") == std::string::npos);
    // back to English: the same loaders read the English folder
    sw_lang::SetCurrent(Lang::English);
    species.clear();
    assert(sw_species_names::LoadNames(host, species));
    assert(sw_species_names::SpeciesName(species, 1, name) && name == "Bulbasaur");
}

// Japanese names decode through the same loaders from the JPN_KANJI / JPN folders (the tables'
// own text: katakana species, kanji/kana move names with U+3000 inside).
void TestJapaneseSpeciesAndMoveNames() {
    g_files.clear();
    g_reads.clear();
    g_files["romfs:/bin/message/JPN_KANJI/common/monsname.dat"] =
        Dat({"\xE3\x82\xBF\xE3\x83\x9E\xE3\x82\xB4", "\xE3\x83\x95\xE3\x82\xB7\xE3\x82\xAE\xE3\x83\x80\xE3\x83\x8D"}); // タマゴ, フシギダネ
    g_files["romfs:/bin/message/JPN_KANJI/common/wazaname.dat"] =
        Dat({"\xE2\x80\x94", "\xE3\x81\xAF\xE3\x81\x9F\xE3\x81\x8F", "\xE7\x81\xAB\xE3\x81\xAE\xE7\xB2\x89"}); // —, はたく, 火の粉
    g_files["romfs:/bin/message/JPN/common/wazaname.dat"] =
        Dat({"\xE2\x80\x94", "\xE3\x81\xAF\xE3\x81\x9F\xE3\x81\x8F", "\xE3\x81\xB2\xE3\x81\xAE\xE3\x81\x93"}); // —, はたく, ひのこ
    const auto host = Host();
    sw_lang::SetCurrent(Lang::JapaneseKanji);
    std::vector<std::string> species, moves;
    assert(sw_species_names::LoadNames(host, species));
    assert(sw_move_names::LoadNames(host, moves));
    std::string name;
    assert(sw_species_names::SpeciesName(species, 1, name) && name == "\xE3\x83\x95\xE3\x82\xB7\xE3\x82\xAE\xE3\x83\x80\xE3\x83\x8D");
    assert(sw_move_names::MoveName(moves, 2, name) && name == "\xE7\x81\xAB\xE3\x81\xAE\xE7\xB2\x89");
    sw_lang::SetCurrent(Lang::Japanese);
    moves.clear();
    assert(sw_move_names::LoadNames(host, moves));
    assert(sw_move_names::MoveName(moves, 2, name) && name == "\xE3\x81\xB2\xE3\x81\xAE\xE3\x81\x93");
    for (const auto& path : g_reads) assert(path.find("/English/") == std::string::npos);
    sw_lang::SetCurrent(Lang::English);
}

// Korean / Chinese names decode through the same loaders from the Korean / Simp_Chinese /
// Trad_Chinese folders (the tables' own text: Hangul, simplified and traditional Han).
void TestKoreanAndChineseNames() {
    g_files.clear();
    g_reads.clear();
    g_files["romfs:/bin/message/Korean/common/monsname.dat"] =
        Dat({"\xEC\x95\x8C", "\xEC\x9D\xB4\xEC\x83\x81\xED\x95\xB4\xEC\x94\xA8"}); // 알, 이상해씨
    g_files["romfs:/bin/message/Simp_Chinese/common/monsname.dat"] =
        Dat({"\xE8\x9B\x8B", "\xE5\xA6\x99\xE8\x9B\x99\xE7\xA7\x8D\xE5\xAD\x90"}); // 蛋, 妙蛙种子
    g_files["romfs:/bin/message/Trad_Chinese/common/monsname.dat"] =
        Dat({"\xE8\x9B\x8B", "\xE5\xA6\x99\xE8\x9B\x99\xE7\xA8\xAE\xE5\xAD\x90"}); // 蛋, 妙蛙種子
    g_files["romfs:/bin/message/Korean/common/wazaname.dat"] =
        Dat({"\xE2\x80\x94", "\xEB\xA7\x89\xEC\xB9\x98\xEA\xB8\xB0"}); // —, 막치기
    const auto host = Host();
    const auto name_of = [&](Lang lang) {
        sw_lang::SetCurrent(lang);
        std::vector<std::string> species;
        assert(sw_species_names::LoadNames(host, species));
        std::string name;
        assert(sw_species_names::SpeciesName(species, 1, name));
        return name;
    };
    assert(name_of(Lang::Korean) == "\xEC\x9D\xB4\xEC\x83\x81\xED\x95\xB4\xEC\x94\xA8");
    assert(name_of(Lang::ChineseSimplified) == "\xE5\xA6\x99\xE8\x9B\x99\xE7\xA7\x8D\xE5\xAD\x90");
    assert(name_of(Lang::ChineseTraditional) == "\xE5\xA6\x99\xE8\x9B\x99\xE7\xA8\xAE\xE5\xAD\x90");
    sw_lang::SetCurrent(Lang::Korean);
    std::vector<std::string> moves;
    assert(sw_move_names::LoadNames(host, moves));
    std::string move;
    assert(sw_move_names::MoveName(moves, 1, move) && move == "\xEB\xA7\x89\xEC\xB9\x98\xEA\xB8\xB0");
    for (const auto& path : g_reads) assert(path.find("/English/") == std::string::npos);
    sw_lang::SetCurrent(Lang::English);
}

// Wild Area zone names come from the player's romfs (place_name, every language): the popup in
// the game's language matches in any of them, ’ and ' alike.
void TestZoneNamesInEveryLanguage() {
    // a fake table: line kZonePlaceLine[0] of each language
    sw_zone::PlaceLines lines;
    for (auto& table : lines) table.assign(249, "Somewhere else");
    lines[0][sw_zone::kZonePlaceLine[0]] = "Rolling Fields";
    lines[1][sw_zone::kZonePlaceLine[0]] = "Plaine Verdoyante";
    lines[0][sw_zone::kZonePlaceLine[5]] = "Axew\xE2\x80\x99s Eye";
    assert(sw_zone::FindZoneId("Rolling Fields", lines) == 0);
    assert(sw_zone::FindZoneId("Plaine Verdoyante", lines) == 0); // the French game's popup
    assert(sw_zone::FindZoneId("Axew's Eye", lines) == 5);
    assert(sw_zone::FindZoneId("Axew\xE2\x80\x99s Eye", lines) == 5); // the game's own apostrophe
    assert(sw_zone::FindZoneId("Wyndon", lines) == -1 && sw_zone::FindZoneId("", lines) == -1);
    assert(sw_zone::ZoneName(lines, 0, Lang::French) == "Plaine Verdoyante" && sw_zone::ZoneName(lines, 0, Lang::German) == "Somewhere else");
    assert(sw_zone::ZoneName(lines, 44, Lang::English).empty() && sw_zone::ZoneName(lines, -1, Lang::English).empty());
    // a language whose table could not be read names nothing (the popup's own text is shown)
    lines[2].clear();
    assert(sw_zone::ZoneName(lines, 0, Lang::German).empty());
    // U+202F (the game's French narrow space) and U+00A0 (the module decoder's) are the same
    assert(sw_zone::SameZoneName("Lac\xE2\x80\xAF!", "Lac\xC2\xA0!") && !sw_zone::SameZoneName("Lac !", "Lac\xC2\xA0!"));
    assert(sw_zone::SameZoneName("Giant's Cap", "Giant\xE2\x80\x99s Cap") && !sw_zone::SameZoneName("Giant", "Giant's"));

    if (!sw_test::RomfsDir()) {
        std::cout << "SW_ROMFS_DIR not set: real zone names skipped\n";
        return;
    }
    // The real tables, as the Reader loads them: the 44 x 10 names equal the baked table they
    // replaced (b92fe1b, sw_game_text_fixture.inc), and the lookup gives the same zone for each.
    const auto host = sw_test::RomfsHost();
    sw_zone::PlaceLines real;
    for (int l = 0; l < sw_lang::kCount; ++l)
        assert(sw_party_details::LoadText(host, sw_lang::MessagePath(static_cast<Lang>(l), "place_name.dat", "script").c_str(), real[l]));
    for (int i = 0; i < 44; ++i) {
        for (int l = 0; l < sw_lang::kCount; ++l) {
            const std::string& name = sw_zone::ZoneName(real, i, static_cast<Lang>(l));
            assert(sw_test::Fnv(name) == sw_fixture::kZoneNameHash[i][l]);
            assert(sw_zone::FindZoneId(name, real) == i);
            std::string ascii = name; // the old table's English spelling
            for (std::size_t at; (at = ascii.find("\xE2\x80\x99")) != std::string::npos;) ascii.replace(at, 3, "'");
            assert(sw_zone::FindZoneId(ascii, real) == i);
        }
    }
    // every other place name of every language: no zone, unless it is a zone's own text
    int others = 0;
    for (int l = 0; l < sw_lang::kCount; ++l)
        for (const auto& text : real[l]) {
            const int id = sw_zone::FindZoneId(text, real);
            if (id < 0) { ++others; continue; }
            bool named = false;
            for (int k = 0; k < sw_lang::kCount; ++k) named = named || sw_zone::SameZoneName(text, sw_zone::ZoneName(real, id, static_cast<Lang>(k)));
            assert(named);
        }
    assert(others > 1500);
    const auto col = [&](Lang l) { return sw_zone::ZoneName(real, 0, l); };
    assert(col(Lang::English) == "Rolling Fields");
    assert(col(Lang::JapaneseKanji) == "\xE3\x81\x86\xE3\x82\x89\xE3\x82\x89\xE3\x81\x8B\xE8\x8D\x89\xE5\x8E\x9F"); // うららか草原
}
void TestHyphenBreak() {
    // German "Galarnuss-Kranz" (192 px at scale 4) in the 168 px evolution column
    assert(sw_text_fit::BreakLongHyphenated("Galarnuss-Kranz", 4, 168) == "Galarnuss- Kranz");
    assert(sw_text_fit::BreakLongHyphenated("Lv. 30, Rotom-Rad", 4, 168) == "Lv. 30, Rotom-Rad"); // fits
    assert(sw_text_fit::BreakLongHyphenated("Use Galarnuss-Kranz now", 4, 168) == "Use Galarnuss- Kranz now");
    assert(sw_text_fit::BreakLongHyphenated("", 4, 168).empty());
    // Japanese: the game's U+3000 phrase separators become the runtime's break opportunities
    assert(sw_text_fit::IdeographicSpacesToBreaks("\xE4\xBB\x8A\xE3\x81\xAF\xE3\x80\x80\xE4\xBD\xBF\xE3\x81\x88\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93") ==
           "\xE4\xBB\x8A\xE3\x81\xAF \xE4\xBD\xBF\xE3\x81\x88\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93"); // 今は　使えません
    assert(sw_text_fit::IdeographicSpacesToBreaks("Lv.5\xE3\x80") == "Lv.5\xE3\x80"); // a cut sequence stays
    // Chinese: a flattened game line break between two Han characters goes (the runtime then wraps
    // the run by character); spaces next to Latin / digits stay
    assert(sw_text_fit::JoinCjkSpaces("\xE7\x9A\x84 \xE7\x9A\xAE\xE5\x8D\xA1\xE4\xB8\x98\xEF\xBC\x8C  \xE8\x84\xB8") ==
           "\xE7\x9A\x84\xE7\x9A\xAE\xE5\x8D\xA1\xE4\xB8\x98\xEF\xBC\x8C\xE8\x84\xB8"); // 的 皮卡丘，  脸 -> 的皮卡丘，脸
    assert(sw_text_fit::JoinCjkSpaces("\xE5\x89\xA9 HP 50 \xE7\x82\xB9") == "\xE5\x89\xA9 HP 50 \xE7\x82\xB9"); // 剩 HP 50 点
    assert(sw_text_fit::JoinCjkSpaces(" \xE7\x9A\x84 ") == " \xE7\x9A\x84 "); // edges stay
    assert(sw_text_fit::JoinCjkSpaces("Lv.5 \xE3\x80") == "Lv.5 \xE3\x80"); // a cut sequence stays
    // Korean keeps its word spaces (Hangul is not joined: it breaks at words like Latin)
    assert(sw_text_fit::JoinCjkSpaces("\xEB\xA7\x8C\xEB\x93\xA4\xEC\x96\xB4 \xEB\x82\xB4\xEB\x8A\x94") ==
           "\xEB\xA7\x8C\xEB\x93\xA4\xEC\x96\xB4 \xEB\x82\xB4\xEB\x8A\x94"); // 만들어 내는
}
} // namespace

int main() {
    TestMessagePathPerLanguage();
    TestSettingValue();
    TestStringTableComplete();
    TestGameLineCleanup();
    TestGameTextFromFakeRomfs();
    TestGameTextMatchesBakedTable();
    TestModuleTextFollowsLanguage();
    TestFrenchSpeciesAndMoveNames();
    TestJapaneseSpeciesAndMoveNames();
    TestKoreanAndChineseNames();
    TestZoneNamesInEveryLanguage();
    TestHyphenBreak();
    std::cout << "sw_lang_test: all passed\n";
    return 0;
}
