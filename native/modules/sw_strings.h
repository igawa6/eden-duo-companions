// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The companion's text in the companion language (sw_lang.h). The table is generated from
// tools/pokemon-sword/strings.json (sw_strings_data.h), the same source the manifest's labels use,
// so a key exists once for both.
//
// Two kinds of key (F13, 2026-11-01: the package ships no text copied from the game):
//  - companion text: kText has the companion's own wording in every language;
//  - a game line: kText is nullptr where the game's text applies, and kGame names the line
//    (romfs bin/message/<Folder>/<dir>/<file>, the same line index in every folder). LoadGame reads
//    those lines from the player's own romfs, once per language, when the language is loaded
//    (Reader::SetLanguage, the font's code point set); the Reader publishes them every tick as
//    sw.str.<key>, which the manifest labels show. A line that cannot be read shows the
//    companion's English placeholder (kGame fallback, not game text); the loader reports how many.
// tools/pokemon-sword/sw_strings.py mirrors CleanGameLine for the generator, the stress test and
// the no-game-text check.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_lang.h"
#include "sw_text.h"

namespace sw_str {

// strings.json "fix" steps (sw_strings.py FIXES, same order).
inline constexpr std::uint8_t kFixTrim = 1;   // outer spaces removed
inline constexpr std::uint8_t kFixFlat = 2;   // line breaks -> spaces
inline constexpr std::uint8_t kFixSpace = 4;  // one trailing space added
inline constexpr std::uint8_t kFixNum = 8;    // the number code -> %03d
inline constexpr std::uint8_t kFixWords3 = 16; // the first three words

struct GameRef {
    const char* dir = nullptr;  // "common" / "script"; nullptr = companion text
    const char* file = nullptr; // "btl_app.dat"
    int line = -1;
    const char* fallback = nullptr; // the companion's English placeholder (not game text)
    std::array<std::uint8_t, sw_lang::kCount> fix{};
};

} // namespace sw_str

#include "sw_strings_data.h"

namespace sw_str {

/// A decoded game line (sw_text ParseLineString) -> the text the companion shows. Control codes
/// (\c, \r, [WAIT n]) are dropped, the game's line breaks become '\n', U+3000 an ASCII space (the
/// runtime breaks lines at U+0020 only), then the row's fix steps. False (nothing shown) for a
/// line that is not plain text: an unresolved variable or reference, an unknown escape, a '%'
/// that is not the number code -- the caller shows the fallback instead.
inline bool CleanGameLine(std::string_view raw, std::uint8_t fix, std::string& out) {
    std::string t;
    t.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        const std::string_view rest = raw.substr(i);
        if (rest.starts_with("\\c") || rest.starts_with("\\r")) { i += 2; continue; }
        if (rest.starts_with("\\n")) { t += '\n'; i += 2; continue; }
        if (rest.starts_with("[WAIT ")) {
            const auto end = rest.find(']');
            if (end == std::string_view::npos) return false;
            i += end + 1;
            continue;
        }
        if ((fix & kFixNum) && rest.starts_with("[VAR 0202]")) { t += "%03d"; i += 10; continue; }
        if (rest.starts_with("[VAR ") || rest.starts_with("[~ ") || rest[0] == '\\') return false;
        if (rest[0] == '%') return false;
        if (rest.starts_with("\xE3\x80\x80")) { t += ' '; i += 3; continue; } // U+3000
        t += rest[0];
        ++i;
    }
    if (fix & kFixTrim) {
        const auto b = t.find_first_not_of(' ');
        t = b == std::string::npos ? std::string{} : t.substr(b, t.find_last_not_of(' ') - b + 1);
    }
    if (fix & kFixFlat)
        for (char& c : t) if (c == '\n') c = ' ';
    if (fix & kFixWords3) {
        int spaces = 0;
        for (std::size_t k = 0; k < t.size(); ++k)
            if (t[k] == ' ' && ++spaces == 3) { t.resize(k); break; }
    }
    if (fix & kFixSpace) t += ' ';
    if (t.empty()) return false;
    out = std::move(t);
    return true;
}

/// The game lines of every language loaded so far. A language's slot is written once, by
/// LoadGame, before `ready` is set; readers (tick, action and font threads) never see it change.
struct GameStore {
    std::mutex mutex;
    std::array<std::atomic<bool>, sw_lang::kCount> ready{}; // loaded (some lines may be missing)
    std::array<std::array<std::string, static_cast<int>(Id::kCount)>, sw_lang::kCount> text;
    std::array<int, sw_lang::kCount> missing{};
};
inline GameStore& Store() {
    static GameStore store;
    return store;
}
inline bool IsGame(Id id) {
    const int i = static_cast<int>(id);
    return i >= 0 && i < static_cast<int>(Id::kCount) && kGame[i].file != nullptr;
}

/// Reads every game line of `lang` from the player's romfs (once per language per process; the
/// files each row names, decoded once each). Returns the number of lines that could not be read
/// on the call that loaded them (0 otherwise), so the caller logs a shortfall once.
inline int LoadGame(const EdenDsmodHostApi& host, sw_lang::Lang lang) {
    auto& st = Store();
    const int col = sw_lang::Index(lang);
    if (st.ready[col].load(std::memory_order_acquire)) return 0;
    std::scoped_lock lock{st.mutex};
    if (st.ready[col].load(std::memory_order_relaxed)) return 0;
    auto& slot = st.text[col];
    int missing = 0;
    // each named file is read and decoded once (path, lines; empty lines = unreadable)
    std::vector<std::pair<std::string, std::vector<std::string>>> files;
    const auto lines_of = [&](const std::string& path) -> const std::vector<std::string>& {
        for (const auto& f : files) if (f.first == path) return f.second;
        std::vector<std::string> lines;
        const std::size_t size = host.read_romfs ? host.read_romfs(host.userdata, path.c_str(), 0, nullptr, 0) : 0;
        if (size > 0 && size <= 1024 * 1024) {
            std::vector<std::uint8_t> bytes(size);
            if (host.read_romfs(host.userdata, path.c_str(), 0, bytes.data(), size) != size ||
                !sw_text::ReadDat(bytes.data(), bytes.size(), lines))
                lines.clear();
        }
        files.emplace_back(path, std::move(lines));
        return files.back().second;
    };
    for (int i = 0; i < static_cast<int>(Id::kCount); ++i) {
        const GameRef& g = kGame[i];
        if (!g.file || kText[i][col]) continue;
        const auto& lines = lines_of(sw_lang::MessagePath(lang, g.file, g.dir));
        std::string clean;
        if (g.line >= 0 && static_cast<std::size_t>(g.line) < lines.size() &&
            CleanGameLine(lines[static_cast<std::size_t>(g.line)], g.fix[static_cast<std::size_t>(col)], clean))
            slot[static_cast<std::size_t>(i)] = std::move(clean);
        else
            ++missing;
    }
    st.missing[col] = missing;
    st.ready[col].store(true, std::memory_order_release);
    return missing;
}
/// Tests only: forget every loaded language (no reader may hold a pointer from S()).
inline void UnloadGameForTest() {
    auto& st = Store();
    std::scoped_lock lock{st.mutex};
    for (int l = 0; l < sw_lang::kCount; ++l) {
        st.ready[l].store(false);
        for (auto& t : st.text[l]) t.clear();
        st.missing[l] = 0;
    }
}

inline const char* S(Id id, sw_lang::Lang lang) {
    const int i = static_cast<int>(id);
    if (i < 0 || i >= static_cast<int>(Id::kCount)) return "";
    const int column = sw_lang::Index(lang);
    if (const char* own = kText[i][column]) return own;
    const GameRef& g = kGame[i];
    if (!g.file) return kText[i][0] ? kText[i][0] : "";
    auto& st = Store();
    if (st.ready[column].load(std::memory_order_acquire)) {
        const std::string& loaded = st.text[column][static_cast<std::size_t>(i)];
        if (!loaded.empty()) return loaded.c_str();
    }
    return g.fallback;
}
inline const char* S(Id id) { return S(id, sw_lang::Current()); }

/// A key by its strings.json name ("res.no_pp"), for tests and tools; Id::kCount if unknown.
inline Id Find(std::string_view key) {
    for (int i = 0; i < static_cast<int>(Id::kCount); ++i)
        if (key == kKeys[i]) return static_cast<Id>(i);
    return Id::kCount;
}

/// printf with one table format ("Lv. %d", "%s male / %s female", ...).
template <class... Args>
inline std::string Format(Id id, Args... args) {
    char buf[160];
    const int n = std::snprintf(buf, sizeof buf, S(id), args...);
    return n < 0 ? std::string{} : std::string(buf, static_cast<std::size_t>(n) < sizeof buf ? n : sizeof buf - 1);
}

/// Personal-table type id (Normal, Fighting, Flying, ...) -> the game's own type name
/// (typename.dat, same order), "" out of range.
inline const char* TypeName(int personal_type) {
    static constexpr Id kTypes[18] = {
        Id::kType0, Id::kType1, Id::kType2, Id::kType3, Id::kType4, Id::kType5,
        Id::kType6, Id::kType7, Id::kType8, Id::kType9, Id::kType10, Id::kType11,
        Id::kType12, Id::kType13, Id::kType14, Id::kType15, Id::kType16, Id::kType17,
    };
    return personal_type >= 0 && personal_type < 18 ? S(kTypes[personal_type]) : "";
}

/// The regions of the zone / Fly tables (sw_zone.h, sw_fly.h).
enum class Region : std::uint8_t { kGalar, kWildArea, kArmor, kCrown };
inline const char* RegionName(Region region) {
    switch (region) {
    case Region::kGalar: return S(Id::kRegionGalar);
    case Region::kWildArea: return S(Id::kRegionWildArea);
    case Region::kArmor: return S(Id::kRegionArmor);
    case Region::kCrown: return S(Id::kRegionCrown);
    }
    return "";
}

/// "Lv.50" (English) / "N.50" / "Lv. 50" (German) ...: the battle info's own level prefix.
inline std::string Level(int level) { return std::string(S(Id::kLevelPrefix)) + std::to_string(level); }

} // namespace sw_str
