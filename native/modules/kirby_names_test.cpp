// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// The colour presets' names in the game's languages: the mask characters' names read from Name/Omen.msbt of the
// player's romfs. Synthetic MSBT files built here; no game data.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kirby_assets.h"
#include "kirby_colour.h"

namespace {

int failures = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                 \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

using Bytes = KirbyAssets::Bytes;

void Put16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void Put32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void Set32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void Section(Bytes& out, const char* magic, const Bytes& body) {
    out.insert(out.end(), magic, magic + 4);
    Put32(out, static_cast<std::uint32_t>(body.size()));
    out.resize(out.size() + 8);
    out.insert(out.end(), body.begin(), body.end());
    while (out.size() % 16)
        out.push_back(0xAB);
}

// An MSBT with a LBL1 (all labels in two hash groups) and a TXT2 (UTF-16LE text, one colour tag in the first)
Bytes Msbt(const std::vector<std::pair<std::string, std::u32string>>& entries) {
    Bytes lbl;
    Put32(lbl, 2);
    const std::size_t table = lbl.size();
    lbl.resize(lbl.size() + 16);
    for (int g = 0; g < 2; ++g) {
        std::uint32_t count = 0;
        const auto start = static_cast<std::uint32_t>(lbl.size());
        for (std::size_t i = g; i < entries.size(); i += 2) {
            lbl.push_back(static_cast<std::uint8_t>(entries[i].first.size()));
            lbl.insert(lbl.end(), entries[i].first.begin(), entries[i].first.end());
            Put32(lbl, static_cast<std::uint32_t>(i));
            ++count;
        }
        Set32(lbl, table + g * 8, count);
        Set32(lbl, table + g * 8 + 4, start);
    }
    Bytes txt;
    Put32(txt, static_cast<std::uint32_t>(entries.size()));
    const std::size_t offsets = txt.size();
    txt.resize(txt.size() + 4 * entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Set32(txt, offsets + 4 * i, static_cast<std::uint32_t>(txt.size()));
        if (i == 0) { // a colour tag (group 0, type 3, four bytes) before the text, dropped by MsbtText
            Put16(txt, 0x0E), Put16(txt, 0), Put16(txt, 3), Put16(txt, 4);
            Put32(txt, 0xFF00FFFF);
        }
        for (const char32_t c : entries[i].second)
            Put16(txt, static_cast<std::uint16_t>(c));
        Put16(txt, 0);
    }
    Bytes out{'M', 's', 'g', 'S', 't', 'd', 'B', 'n', 0xFF, 0xFE, 0, 0, 1, 3, 3, 0};
    out.resize(0x20);
    Section(out, "LBL1", lbl);
    Section(out, "ATR1", Bytes{0, 0, 0, 0, 0, 0, 0, 0});
    Section(out, "TXT2", txt);
    return out;
}

const KirbyColour::Preset& Find(std::string_view key) {
    for (const auto& p : KirbyColour::Presets)
        if (p.key == key)
            return p;
    std::fprintf(stderr, "no preset %.*s\n", static_cast<int>(key.size()), key.data());
    std::exit(1);
}

} // namespace

int main() {
    // invented labels and names: the lookup must go by the English text, not by a known label
    const auto english = Msbt({{"Mask_A", U"Chef Kawasaki"}, {"Mask_B", U"King Dedede"}, {"Mask_C", U"Kine"}});
    const auto french = Msbt({{"Mask_A", U"Cuisinier Test"}, {"Mask_B", U"Roi Test"}, {"Mask_C", U""}});

    // the reader primitives
    const auto labels = KirbyAssets::MsbtLabels(english);
    CHECK(labels.size() == 3);
    CHECK(KirbyAssets::MsbtText(english, "Mask_A") == std::u32string{U"Chef Kawasaki"});
    CHECK(KirbyAssets::MsbtText(french, "Mask_B") == std::u32string{U"Roi Test"});
    CHECK(KirbyAssets::MsbtLabels(Bytes{1, 2, 3}).empty());
    CHECK(KirbyAssets::MsbtLabels(Bytes(english.begin(), english.begin() + 0x30)).empty());

    std::map<std::string, Bytes> files{{"/msg/Ark/US_English/Name/Omen.msbt", english},
                                       {"/msg/Ark/EU_French/Name/Omen.msbt", french}};
    int reads = 0;
    const KirbyAssets::RomfsReader read = [&](const std::string& path) -> std::optional<Bytes> {
        ++reads;
        const auto it = files.find(path);
        return it == files.end() ? std::nullopt : std::optional{it->second};
    };
    const auto& dedede = Find("dedede");
    const auto& kawasaki = Find("kawasaki");
    const auto& kine = Find("kine");

    // a mask character in a language the table lists: the game's own name, through the English label
    CHECK(KirbyColour::NameIn(dedede, "EU_French", read) == U"Roi Test");
    // an empty text in the game's file: the English name
    CHECK(KirbyColour::NameIn(kine, "EU_French", read) == U"Kine");
    // Italian: the preset's own Italian name, nothing read
    reads = 0;
    CHECK(KirbyColour::NameIn(kawasaki, "EU_Italian", read) == kawasaki.italian);
    CHECK(reads == 0);
    // a language the table does not list: the English name, nothing read
    CHECK(KirbyColour::NameIn(dedede, "JP_Japanese", read) == dedede.english);
    CHECK(reads == 0);
    // the other games' characters: the companion's own names
    CHECK(KirbyColour::NameIn(Find("sylveon"), "EU_German", read) == U"Feelinara");
    // no game file for the language (EU_German listed for Dedede): the English name
    CHECK(KirbyColour::NameIn(dedede, "EU_German", read) == dedede.english);
    // the English file only in EU_English: still found
    files.erase("/msg/Ark/US_English/Name/Omen.msbt");
    files["/msg/Ark/EU_English/Name/Omen.msbt"] = english;
    CHECK(KirbyColour::NameIn(dedede, "EU_French", read) == U"Roi Test");
    // a German file too: Chef Kawasaki is listed for German
    files["/msg/Ark/EU_German/Name/Omen.msbt"] = Msbt({{"Mask_A", U"Koch Test"}});
    CHECK(KirbyColour::NameIn(kawasaki, "EU_German", read) == U"Koch Test");
    // an English text that matches no label (Dedede renamed): the English name
    files["/msg/Ark/EU_English/Name/Omen.msbt"] = Msbt({{"Mask_B", U"King Dedede III"}});
    CHECK(KirbyColour::NameIn(dedede, "EU_French", read) == dedede.english);
    // broken files and a reader that has nothing: the English name, no crash
    files["/msg/Ark/EU_English/Name/Omen.msbt"] = Bytes(english.begin(), english.begin() + 0x47);
    files["/msg/Ark/EU_French/Name/Omen.msbt"] = Bytes{'M', 's', 'g'};
    CHECK(KirbyColour::NameIn(dedede, "EU_French", read) == dedede.english);
    CHECK(KirbyColour::NameIn(dedede, "EU_French", KirbyAssets::RomfsReader{}) == dedede.english);
    CHECK(KirbyColour::NameIn(dedede, "EU_French", [](const std::string&) { return std::optional<Bytes>{}; }) ==
          dedede.english);

    if (failures)
        std::fprintf(stderr, "%d check(s) failed\n", failures);
    else
        std::puts("kirby names: ok");
    return failures ? 1 : 0;
}
