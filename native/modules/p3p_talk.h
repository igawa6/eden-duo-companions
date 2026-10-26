// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The open message window and its choice list (itfMesManager.c), decoded from the game's own
// loaded message file (Atlus MSG1 BMD). Read-only: the companion shows the choices, the game's
// cursor picks.
//
// Windows: a pool of 128 slots of 0x40 bytes at main+0x83a898 (init 0x1bc620); slot +0x18 is the
// window (getters 0x1b9a90, 0x1bac60: main+0x83a868 + i * 0x40 + 0x48). Window fields, live on the
// lobby clock (SAVE_OR_HEAL) and battle tips (SYS_TUTORIAL_SEL, five-page SYS_TUTORIAL_01):
//   +0x00 flags (0 when closed)   +0x08 BMD   +0x30 current message dialog   +0x260 BMD + 0x20
//   +0x48 page   +0x4a pages (page == pages once the message has finished, e.g. under a choice)
//   +0x58 selection dialog index, +0x68 hidden-option mask (bit i hides option i: the caller's
//   mask, plus options whose text evaluates hidden), +0x6c selection state (1 opening, 2 open),
//   +0x6e cursor among the shown options (getter 0x1bac60), +0x72 shown count (all set by the
//   selection opener ending 0x1ba94c).
//
// Text: 0 ends, '\n', 0x80 0xXX for XX in 0x80..0xDE is ASCII XX - 0x60 (the encoding of the
// player's name too), other bytes >= 0x80 start two-byte glyphs (not decoded: fail closed), and
// 0xFn starts a function of 2 + (n - 1) * 2 bytes whose second byte is table << 5 | function
// (tables at main+0x622cc8). Only functions whose handlers append no text are dropped, plus the
// protagonist's names (0x81 surname 0x1af210, 0x82 first name 0x1af390, 0x83 full name 0x1af510),
// 0x44 (0x1ae950 / 0x1b0870: the script's text variable (lo - 1) & 0xff, a string pointed to from
// main+0x83a318 + index * 8, e.g. "10" in "Resume from 10F") and 0x25 (0x1ae590: the rest of the
// line up to '\n' is the speaker's name). Any other text-producing function (items 0x45, 0x84,
// 0xa3..0xa7) fails the text closed.
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace p3p_talk {

struct Names {
    std::string surname, first, full;
    std::vector<std::string> numbers; // the script's text variables (0x44), empty if unset
};

/// Functions whose handlers append nothing visible: colour 0x01 (an escape for the renderer),
/// layout/wait/voice/bust-up/flag functions. 0x47 in options applies the choice's effects.
inline bool Silent(uint8_t f) {
    switch (f) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x21: case 0x22: case 0x23: case 0x24: case 0x26: case 0x27:
    case 0x41: case 0x42: case 0x43: case 0x46: case 0x47: case 0x61:
    case 0x85: case 0x86: case 0x87: case 0x88: case 0x89:
    case 0xa1: case 0xa2: case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad:
    case 0xc1: return true;
    default: return false;
    }
}

/// One page or option, flattened to lines; false if anything in it cannot be reproduced.
/// `speaker` receives a 0x25 inline name, if any.
inline bool Decode(std::span<const uint8_t> b, const Names& names, std::string& out, std::string* speaker = nullptr) {
    out.clear();
    std::string* to = &out;
    std::string inline_name;
    bool in_name = false;
    for (size_t j = 0; j < b.size() && b[j];) {
        const uint8_t c = b[j];
        if ((c & 0xF0) == 0xF0) {
            const size_t n = 2 + size_t((c & 0x0F) ? (c & 0x0F) - 1 : 0) * 2;
            if (j + n > b.size()) return false;
            const uint8_t f = b[j + 1];
            j += n;
            if (f == 0x81 || f == 0x82 || f == 0x83) {
                const std::string& name = f == 0x81 ? names.surname : f == 0x82 ? names.first : names.full;
                if (name.empty()) return false;
                *to += name;
            } else if (f == 0x44) {
                const size_t index = n >= 4 ? size_t((b[j - n + 2] - 1) & 0xff) : names.numbers.size();
                if (index >= names.numbers.size() || names.numbers[index].empty()) return false;
                *to += names.numbers[index];
            } else if (f == 0x25) {
                if (!speaker) return false;
                in_name = true; to = &inline_name;
            } else if (!Silent(f)) return false;
            continue;
        }
        if (c == '\n') {
            if (in_name) { in_name = false; to = &out; }
            else *to += '\n';
            ++j; continue;
        }
        if (c >= 0x80) {
            if (j + 1 >= b.size() || c != 0x80 || b[j + 1] < 0x80 || b[j + 1] > 0xDE) return false;
            *to += char(b[j + 1] - 0x60); j += 2; continue;
        }
        if (c < 0x20) return false;
        *to += char(c); ++j;
    }
    if (in_name) return false;
    // Blank lines (the tutorials' spacer rows) and trailing space carry nothing.
    std::string tidy;
    size_t start = 0;
    while (start <= out.size()) {
        size_t end = out.find('\n', start);
        if (end == out.npos) end = out.size();
        std::string line = out.substr(start, end - start);
        while (!line.empty() && line.back() == ' ') line.pop_back();
        size_t lead = line.find_first_not_of(' ');
        if (lead != line.npos) tidy += (tidy.empty() ? "" : "\n") + line;
        start = end + 1;
    }
    out = std::move(tidy);
    if (speaker) {
        while (!inline_name.empty() && inline_name.back() == ' ') inline_name.pop_back();
        size_t lead = inline_name.find_first_not_of(' ');
        *speaker = lead == inline_name.npos ? std::string{} : inline_name.substr(lead);
    }
    return true;
}

/// The window fields Parse() needs, as read from game memory.
struct Window {
    uint64_t bmd{};      // +0x08, BMD start
    uint64_t message{};  // +0x30, the current message dialog (absolute)
    int page{}, pages{}; // +0x48 / +0x4a
    int state{}, cursor{-1}, shown{}, selection{}; // +0x6c, +0x6e, +0x72, +0x58
    uint32_t mask{};     // +0x68
};

struct Talk {
    bool text_ok{}, choosing{}, choices_ok{};
    std::string speaker, text;
    std::vector<std::string> choices; // the shown options, in order
    int selected{-1};                 // index into choices
};

/// The open window's current page and (while a selection is open) its shown options. Throws
/// when the window does not agree with the message file.
inline Talk Parse(std::span<const uint8_t> bmd, const Window& w, const Names& names) {
    auto fail = [](const char* why) { throw std::runtime_error(why); };
    auto u32 = [&](size_t at) {
        if (at + 4 > bmd.size()) fail("message file truncated");
        return uint32_t(bmd[at] | bmd[at + 1] << 8 | bmd[at + 2] << 16 | uint32_t(bmd[at + 3]) << 24);
    };
    auto u16 = [&](size_t at) {
        if (at + 2 > bmd.size()) fail("message file truncated");
        return uint32_t(bmd[at] | bmd[at + 1] << 8);
    };
    if (bmd.size() < 0x28 || bmd[8] != 'M' || bmd[9] != 'S' || bmd[10] != 'G' || bmd[11] != '1') fail("no message file");
    const uint32_t count = u32(0x18);
    if (count == 0 || count > 4096) fail("bad message file");
    auto text = [&](uint32_t rel) { // a string at BMD + 0x20 + rel
        const size_t at = size_t(rel) + 0x20;
        if (at >= bmd.size()) fail("message file truncated");
        return bmd.subspan(at);
    };
    Talk t;
    // The current message: one of the file's message dialogs (kind 0).
    if (w.message < w.bmd + 0x20 || w.message - w.bmd >= bmd.size()) fail("message outside file");
    const uint32_t rel = uint32_t(w.message - w.bmd - 0x20);
    int index = -1;
    for (uint32_t i = 0; i < count; ++i)
        if (u32(0x20 + 8 * i + 4) == rel) { index = int(i); break; }
    if (index < 0 || u32(0x20 + 8 * index) != 0) fail("message not in file");
    const size_t m = size_t(rel) + 0x20;
    const int pages = int(u16(m + 24)), speaker = int(u16(m + 26));
    if (pages < 1 || pages != w.pages || w.page < 0 || w.page > pages) fail("page out of range");
    const int page = w.page == pages ? pages - 1 : w.page; // finished: its last page stays shown
    std::string inline_name;
    t.text_ok = Decode(text(u32(m + 28 + 4 * page)), names, t.text, &inline_name);
    if (t.text_ok && !inline_name.empty()) t.speaker = inline_name;
    else if (speaker != 0xffff) {
        // Speaker table after the dialog table: {offset, count}, then name offsets.
        const size_t table = 0x20 + 8 * size_t(count);
        const uint32_t names_at = u32(table), n = u32(table + 4);
        if (uint32_t(speaker) < n && !Decode(text(u32(size_t(names_at) + 0x20 + 4 * speaker)), names, t.speaker)) t.speaker.clear();
    }
    if (w.state < 1) return t;
    // The selection dialog (kind 1): name[24], u16, u16 options, u32, option offsets.
    if (w.selection < 0 || uint32_t(w.selection) >= count || u32(0x20 + 8 * w.selection) != 1) fail("selection not in file");
    const size_t s = size_t(u32(0x20 + 8 * w.selection + 4)) + 0x20;
    const int options = int(u16(s + 26));
    if (options < 1 || options > 32) fail("bad selection");
    t.choosing = true; t.choices_ok = true;
    for (int i = 0; i < options; ++i) {
        if (w.mask >> i & 1) continue;
        std::string option;
        if (!Decode(text(u32(s + 32 + 4 * i)), names, option)) t.choices_ok = false;
        for (char& ch : option) if (ch == '\n') ch = ' ';
        t.choices.push_back(std::move(option));
    }
    if (int(t.choices.size()) != w.shown) fail("selection count mismatch");
    if (!t.choices_ok) t.choices.clear();
    else if (w.cursor >= 0 && w.cursor < w.shown) t.selected = w.cursor;
    return t;
}

} // namespace p3p_talk
