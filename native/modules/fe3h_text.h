// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FE3H message text from romfs (msgdata, LINKDATA id 0; patched by patch4/common/common/
// msgdata.bin): ptbl{12 languages} -> ptbl{4 parts} -> XL string table.
//   languages: 0 JPN, 1 ENG_U, 2 ENG_E, 3 GER, 4 FRA_E, 5 FRA_U, 6 ESP_E, 7 ESP_U, 8 ITA, 9 KOR,
//              10 TWN, 11 CHN
//   ENG_U parts: 0 system dialogs (100), 1 help (6806), 2 names / descriptions (15397),
//                3 DLC / shop (442). Part-2 index map: research/fe3h/data/REPORT.md §2
//                (1157 + name id persons, 3453 + class id, 3756 + weapon id, 10652 calendar ...).
//   XL: 'XL', u16 0x13 | u16 size, u16 cols | u16 count (at 8), u16 | u32 header size (at 0xC)
//       | u32; u32 offset[count] at the header size; string at header size + offset, NUL-
//       terminated UTF-8 (inline control codes are returned as stored).
// The game-state reader takes names from the game's RAM text manager; this is for pages that
// browse text the game has not loaded (glossary). Thread-compatible (Library serialises).

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fe3h_romfs.h"

namespace fe3h_text {

inline constexpr uint32_t MsgDataId = 0;
inline constexpr uint32_t LangEngU = 1;

class XlTable {
public:
    // Takes ownership of an XL table's bytes; false when the header / offset table is invalid.
    bool Parse(std::vector<uint8_t> bytes);
    uint32_t Count() const {
        return count_;
    }
    // String `index` (without the NUL); false when out of range or unterminated.
    bool Get(uint32_t index, std::string& out) const;

private:
    std::vector<uint8_t> b_;
    uint32_t count_{};
    uint32_t header_{};
};

class MessageText {
public:
    explicit MessageText(fe3h_romfs::LinkData& link) : link_(link) {}
    // msgdata[lang][part][index]. Loads (and keeps) one part table on first use.
    bool Get(uint32_t part, uint32_t index, std::string& out, uint32_t lang = LangEngU);
    // Number of strings in a part (0 on failure).
    uint32_t Count(uint32_t part, uint32_t lang = LangEngU);

private:
    const XlTable* Part(uint32_t lang, uint32_t part);

    fe3h_romfs::LinkData& link_;
    std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<XlTable>> parts_; // null = failed
};

} // namespace fe3h_text
