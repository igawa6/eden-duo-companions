// SPDX-FileCopyrightText: Copyright 2026 Eden_Duo_Mode contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// fe3h_labels: the table of the companion's fixed words and the choice of a language's text. The
// table is the generated one; no game data is needed.

#include "fe3h_labels.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace fe3h_labels;

int main() {
    assert(EntryCount() > 0);
    std::set<std::string> keys;
    size_t game_entries = 0, own_entries = 0;
    for (size_t i = 0; i < EntryCount(); ++i) {
        const Entry& e = Entries()[i];
        // a key is published as "ui.l." + key: lowercase words, digits and underscores only
        assert(e.key && *e.key);
        for (const char* c = e.key; *c; ++c)
            assert((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_');
        assert(keys.insert(e.key).second);
        // English is the fallback of everything
        assert(e.own[En] && *e.own[En]);
        if (e.part >= 0) {
            // a game term: the words of the other languages come from the game
            ++game_entries;
            assert(e.index >= 0);
            for (int c = 1; c < ColumnCount; ++c)
                assert(!e.own[c]);
        } else {
            // the companion's own phrase: every column is written
            ++own_entries;
            for (int c = 0; c < ColumnCount; ++c)
                assert(e.own[c] && *e.own[c]);
        }
    }
    assert(game_entries > 0 && own_entries > 0);

    // the columns of the game's 12 languages (msgdata order), English for an unknown one
    assert(ColumnOf(0) == Ja && ColumnOf(3) == De && ColumnOf(8) == It);
    assert(ColumnOf(4) == Fr && ColumnOf(5) == Fr && ColumnOf(6) == Es && ColumnOf(7) == Es);
    assert(ColumnOf(9) == Ko && ColumnOf(10) == Twn && ColumnOf(11) == Chn);
    assert(ColumnOf(1) == En && ColumnOf(2) == En);
    assert(ColumnOf(-1) == En && ColumnOf(12) == En && ColumnOf(99) == En);

    // Own(): the language's own text, English for a game term and for an unknown language
    for (size_t i = 0; i < EntryCount(); ++i) {
        const Entry& e = Entries()[i];
        assert(!std::strcmp(Own(e, -1), e.own[En]));
        assert(!std::strcmp(Own(e, 1), e.own[En]));
        if (e.part >= 0)
            assert(!std::strcmp(Own(e, 9), e.own[En]));
        if (!std::strcmp(e.key, "academy")) {
            assert(!std::strcmp(Own(e, 9), "사관학교"));
            assert(!std::strcmp(Own(e, 0), "士官学校"));
            assert(!std::strcmp(Own(e, 10), "士官學校"));
            assert(!std::strcmp(Own(e, 11), "士官学校"));
            assert(!std::strcmp(Own(e, 1), "Academy"));
        }
    }

    // a game text loses its control codes (colour tags)
    assert(CleanGameText(std::string("a\x1b" "b\nc")) == "abc");
    assert(CleanGameText("Str") == "Str");

    std::puts("fe3h_labels_test: ok");
    return 0;
}
