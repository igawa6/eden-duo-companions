// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH companion: the game's grammar tags for a message that inserts an item name (Nook Miles+
// "Catch an Olive Flounder", "Fange eine Flunder", "Поймай ложного палтуса!"). Rules and their
// evidence: research/acnh/impl/fix/data.md ("item grammar"); summary:
//   item name attributes  {50:0:g,i,d} (singular) / {50:26:g,i,d} (the _pl form): g = gender /
//                         number class, i / d = String/STR_Article label (%03d) of the indefinite /
//                         definite article (data: every language's table + names agree; the
//                         article lookup "%03d" in STR_Article is code 0x1ab3760, the table loader
//                         0x1aad8d4)
//   {50:2:} / {50:1:}     the indefinite / definite article of the inserted item (data + live USen)
//   {50:3:}               capitalise the next character (code 0x1ab5ae8: +0x4ba; the game's case
//                         table, Dutch "IJ" 0x1aa2758)
//   {50:4:}               capitalise + the "title" flag of the next insertion (code 0x1ab5bb8:
//                         +0x4ba and +0x234): every word capitalised (LIVE USen "Olive Flounder";
//                         word boundary = space, LEAD for other separators)
//   {50:23:c}             grammatical case of the next insertion (code 0x1ab5bc8: +0x230 = c,
//                         copied into the insertion state 0x1ab65fc); {50:22:list} inside a name =
//                         the ending per case, u16 byte-length prefixed UTF-16 strings in the order
//                         nominative, accusative, dative, genitive (data: EUru / EUde endings)
//   {50:17:r,..,list}     a word chosen by the gender of the referenced insertion (EUde "einen /
//                         eine / ein"; index = g - 1: data-consistent, LEAD)
//   {50:29:}              cancel the pending capitalisation (code 0x1ab5be0)
//   {125:n:..}            the item name (singular, or the _pl form when count != 1 and the item
//                         has one: stackable rule 0x1b0c050)
//   {0:0:..}              ruby (furigana) -> base text only; other tags (colour, voice, ...) drop.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "acnh_lang.h"

namespace acnh {

class Catalog;
class Msbt;

struct ItemNameRaw {
    std::u16string singular; ///< String/Item STR_ItemName_* "<kind>_<id:05d>" raw (tags kept)
    std::u16string plural;   ///< its "_pl" label raw, empty when none
};

/// `message`: the MSBT raw string (tags kept, Msbt::Raw); `item`: the name inserted by its
/// {125:n} tag; `count` selects the plural (count != 1 and a plural exists); `articles` = the
/// language's String/STR_Article (may be null: no articles); `case_table` = the game's {lower,
/// upper} pairs (LiveSnapshot::case_table, may be null: ASCII / Latin-1 fallback).
std::string ComposeItemMessage(Lang lang, std::u16string_view message, const ItemNameRaw& item,
                               int count, const Msbt* articles,
                               const std::vector<uint16_t>* case_table);

/// The raw singular / plural item name of item `id` in `lang` (empty when unknown).
ItemNameRaw FindItemNameRaw(Catalog& catalog, Lang lang, uint16_t id);

/// Convenience: message `path`/`label` of `lang` with item `id` inserted ("" when missing).
std::string ComposeItemMessage(Catalog& catalog, Lang lang, std::string_view path,
                               std::string_view label, uint16_t id, int count,
                               const std::vector<uint16_t>* case_table);

} // namespace acnh
