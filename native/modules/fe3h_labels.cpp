// SPDX-FileCopyrightText: Copyright 2026 Eden_Duo_Mode contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe3h_labels.h"

#include "fe3h_language.h"

namespace fe3h_labels {
namespace {

constexpr Entry Table[] = {
#include "fe3h_labels_data.inc"
};

} // namespace

const Entry* Entries() {
    return Table;
}

size_t EntryCount() {
    return sizeof(Table) / sizeof(Table[0]);
}

Column ColumnOf(int language) {
    switch (language) {
    case 0:
        return Ja;
    case 3:
        return De;
    case 4:
    case 5:
        return Fr;
    case 6:
    case 7:
        return Es;
    case 8:
        return It;
    case 9:
        return Ko;
    case 10:
        return Twn;
    case 11:
        return Chn;
    default: // 1 / 2 English, and an unknown language
        return En;
    }
}

const char* Own(const Entry& entry, int language) {
    const char* text = entry.own[ColumnOf(language)];
    return text ? text : entry.own[En];
}

std::string CleanGameText(const std::string& text) {
    return fe3h_language::CleanText(text);
}

} // namespace fe3h_labels
