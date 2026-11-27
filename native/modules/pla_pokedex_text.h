// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only native Pokedex message namespaces and narrowly verified word slots.
#pragma once
#include "pla_pokedex.h"
#include <cstdio>
#include <string_view>

namespace pla {
struct PokedexTextTable {
  std::span<const uint8_t> dat, tbl;
};
struct PokedexEntryMessages {
  PokedexTextTable full, ongoing, generic;
};
inline uint64_t PokedexLabelHash(std::string_view label) {
  uint64_t hash = 0xcbf29ce484222645;
  for (const unsigned char c : label)
    hash = (hash ^ c) * 0x100000001b3;
  return hash;
}
// AHTB has one final *_max label with NO DAT row. Validate all label hashes,
// lengths and the expected namespace sentinel; never expose that sentinel as
// text.
inline std::optional<unsigned> PokedexLabelRow(const PokedexTextTable &table,
                                               std::string_view wanted,
                                               std::string_view sentinel) {
  if (table.dat.size() < 16 || table.dat.size() > 1024 * 1024 ||
      table.tbl.size() < 8 || table.tbl.size() > 1024 * 1024 ||
      wanted.empty() || wanted.size() > 1023 || wanted == sentinel ||
      sentinel.empty() || DexU32(table.tbl, 0) != 0x42544841 ||
      DexU16(table.dat, 0) != 1)
    return std::nullopt;
  const unsigned rows = DexU16(table.dat, 2), count = DexU32(table.tbl, 4);
  if (!rows || count != rows + 1 || count > 8192 ||
      count > (table.tbl.size() - 8) / 10)
    return std::nullopt;
  const auto hash = PokedexLabelHash(wanted);
  std::optional<unsigned> found;
  size_t at = 8;
  for (unsigned row = 0; row < count; ++row) {
    if (at > table.tbl.size() || table.tbl.size() - at < 10)
      return std::nullopt;
    const uint64_t stored = uint64_t(DexU32(table.tbl, at)) |
                            uint64_t(DexU32(table.tbl, at + 4)) << 32;
    const size_t length = DexU16(table.tbl, at + 8);
    at += 10;
    if (length < 2 || length > 1024 || length > table.tbl.size() - at ||
        table.tbl[at + length - 1])
      return std::nullopt;
    const std::string_view label(
        reinterpret_cast<const char *>(table.tbl.data() + at), length - 1);
    for (const unsigned char c : label)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_'))
        return std::nullopt;
    if (PokedexLabelHash(label) != stored ||
        (label == sentinel) != (row == rows))
      return std::nullopt;
    if (stored == hash && label == wanted) {
      if (found.has_value())
        return std::nullopt;
      found = row;
    }
    at += length;
  }
  return at == table.tbl.size() ? found : std::nullopt;
}
inline std::string PokedexMessageLabel(const PokedexTextTable &table,
                                       std::string_view label,
                                       std::string_view sentinel,
                                       bool multiline = false) {
  const auto row = PokedexLabelRow(table, label, sentinel);
  return row.has_value() ? Message(table.dat, *row, multiline) : std::string{};
}
inline std::string PokedexEntryComment(const PokedexEntry &entry,
                                       const PokedexFormState &form,
                                       const PokedexEntryMessages &tables) {
  const auto kind = PokedexEntryTextPermission(entry, form);
  if (kind == PokedexEntryTextKind::Unavailable || entry.species > 905)
    return {};
  char label[64];
  if (kind == PokedexEntryTextKind::Generic) {
    const auto variant = PokedexGenericCommentVariant(entry, form);
    if (!variant.has_value())
      return {};
    std::snprintf(label, sizeof(label), "msg_ui_pokedex_16_%02u", *variant);
    return PokedexMessageLabel(tables.generic, label, "msg_pokedex_max", true);
  }
  const char which = kind == PokedexEntryTextKind::Full ? 'A' : 'B';
  std::snprintf(label, sizeof(label), "ZKN_COMMENT_%c_%03u_%03u", which,
                entry.species, form.form);
  return PokedexMessageLabel(which == 'A' ? tables.full : tables.ongoing, label,
                             which == 'A' ? "msg_zukan_comment_A_max"
                                          : "msg_zukan_comment_B_max",
                             true);
}
enum class PokedexUiLabel { Seen, Caught, ResearchLevel, Complete, Perfect };
inline std::string PokedexUiLabelText(PokedexUiLabel which,
                                      const PokedexTextTable &table) {
  const char *label = nullptr;
  switch (which) {
  case PokedexUiLabel::Seen:
    label = "msg_ui_pokedex_02_01";
    break;
  case PokedexUiLabel::Caught:
    label = "msg_ui_pokedex_02_04";
    break;
  case PokedexUiLabel::ResearchLevel:
    label = "msg_ui_pokedex_15_04";
    break;
  case PokedexUiLabel::Complete:
    label = "msg_ui_pokedex_15_02";
    break;
  case PokedexUiLabel::Perfect:
    label = "msg_ui_pokedex_15_03";
    break;
  default:
    return {};
  }
  return PokedexMessageLabel(table, label, "msg_pokedex_max");
}
inline std::string PokedexAreaName(unsigned area,
                                   const PokedexTextTable &pokedex,
                                   const PokedexTextTable &script_place_name) {
  if (!area)
    return PokedexMessageLabel(pokedex, "msg_ui_pokedex_04_02",
                               "msg_pokedex_max");
  if (area > 5)
    return {};
  char label[32];
  std::snprintf(label, sizeof(label), "PLACENAME_AREA%02u", area);
  // SCRIPT/place_name has the bare names. COMMON/place_name_indirect has
  // the SAME hashes but different phrases; namespace sentinel rejects it.
  return PokedexMessageLabel(script_place_name, label, "msg_place_name_max");
}
inline std::string PokedexCategoryName(const PokedexEntry &entry,
                                       const PokedexFormState &form,
                                       const PokedexTextTable &category) {
  if (PokedexEntryTextPermission(entry, form) ==
          PokedexEntryTextKind::Unavailable ||
      entry.species > 905)
    return {};
  char label[48];
  if (form.form)
    std::snprintf(label, sizeof(label), "ZKN_TYPE_%03u_%03u", entry.species,
                  form.form);
  else
    std::snprintf(label, sizeof(label), "ZKN_TYPE_%03u", entry.species);
  // An absent regional label does not authorize a guessed base-form category.
  return PokedexMessageLabel(category, label, "msg_zkn_type_max");
}
struct PokedexTaskTextTables {
  PokedexTextTable ui, moves, types;
};
// Only the two native task word slots proven at130DB20/130E100 are admitted:
// control10,count2,0107(move) or0103(type),arg0000. Every other control
// refuses.
inline std::string PokedexTaskWordMessage(const PokedexTextTable &table,
                                          std::string_view label,
                                          unsigned variable,
                                          std::string_view word) {
  if ((variable != 0x0107 && variable != 0x0103) || word.empty() ||
      word.size() > 256)
    return {};
  for (const unsigned char c : word)
    if (c < 32 || c == 127)
      return {};
  const auto row = PokedexLabelRow(table, label, "msg_pokedex_max");
  if (!row.has_value())
    return {};
  const auto dat = table.dat;
  const size_t section = DexU32(dat, 12);
  if (section > dat.size() || dat.size() - section < 4 ||
      *row >= (dat.size() - section - 4) / 8)
    return {};
  const size_t record = section + 4 + *row * 8;
  const int32_t offset = int32_t(DexU32(dat, record));
  if (offset < 0 || size_t(offset) > dat.size() - section)
    return {};
  const size_t start = section + size_t(offset),
               count = DexU16(dat, record + 4);
  if (!count || count > 1024 || count > (dat.size() - start) / 2)
    return {};
  std::vector<unsigned> units;
  uint16_t key = uint16_t(0x7c89 + *row * 0x2983);
  for (size_t j = 0; j < count; ++j) {
    units.push_back(DexU16(dat, start + j * 2) ^ key);
    key = uint16_t((key << 3) | (key >> 13));
  }
  std::string text;
  unsigned insertions = 0;
  for (size_t at = 0; at < units.size(); ++at) {
    const auto cp = units[at];
    if (!cp)
      return insertions == 1 ? text : std::string{};
    if (cp == 0x10) {
      if (units.size() - at < 4 || units[at + 1] != 2 ||
          units[at + 2] != variable || units[at + 3] != 0 || ++insertions > 1)
        return {};
      text += word;
      at += 3;
    } else {
      if ((cp < 32 && cp != 10) || (cp >= 0xd800 && cp <= 0xdfff))
        return {};
      Utf8(text, cp);
    }
  }
  return {};
}
inline std::string PokedexTaskLabel(const PokedexEntry &entry,
                                    const PokedexNativeTask &task,
                                    const PokedexTaskTextTables &tables) {
  if (!entry.valid || !entry.revealed || !entry.ever_updated ||
      !entry.species || entry.species > 905 || task.species != entry.species ||
      task.native_type > 24)
    return {};
  // Exact native hash array397B958, helper130C280. Raw10 uses time array397BA38
  // via caller130E53C; no native row arithmetic or translated text is embedded.
  constexpr const char *labels[25] = {"msg_ui_pokedex_21_01",
                                      "msg_ui_pokedex_21_04",
                                      "msg_ui_pokedex_21_11",
                                      "msg_ui_pokedex_21_02",
                                      "msg_ui_pokedex_21_13",
                                      "msg_ui_pokedex_21_12",
                                      "msg_ui_pokedex_21_07",
                                      "msg_ui_pokedex_21_08",
                                      "msg_ui_pokedex_21_09",
                                      "msg_ui_pokedex_21_10",
                                      nullptr,
                                      "msg_ui_pokedex_21_28",
                                      "msg_ui_pokedex_21_31",
                                      "msg_ui_pokedex_21_15",
                                      "msg_ui_pokedex_21_03",
                                      "msg_ui_pokedex_21_14",
                                      "msg_ui_pokedex_21_16",
                                      "msg_ui_pokedex_21_17",
                                      "msg_ui_pokedex_21_18",
                                      "msg_ui_pokedex_21_19",
                                      "msg_ui_pokedex_21_25",
                                      "msg_ui_pokedex_21_26",
                                      "msg_ui_pokedex_21_27",
                                      "msg_ui_pokedex_21_30",
                                      "msg_ui_pokedex_21_29"};
  char label[32];
  const char *message = labels[task.native_type];
  if (task.native_type == 10) {
    if (task.time_of_day > 4)
      return {};
    constexpr const char *time_labels[5]{
        "msg_ui_pokedex_21_20", "msg_ui_pokedex_21_21", "msg_ui_pokedex_21_22",
        "msg_ui_pokedex_21_23", "msg_ui_pokedex_21_24"};
    message = time_labels[task.time_of_day];
  }
  if (task.native_type == 1) {
    if (!task.move || task.move > 850)
      return {};
    std::snprintf(label, sizeof(label), "WAZANAME_%03u", task.move);
    const auto word =
        PokedexMessageLabel(tables.moves, label, "msg_wazaname_max");
    return PokedexTaskWordMessage(tables.ui, message, 0x0107, word);
  }
  if (task.native_type == 2) {
    if (task.move_type >= 18)
      return {};
    std::snprintf(label, sizeof(label), "TYPENAME_%03u", task.move_type);
    const auto word =
        PokedexMessageLabel(tables.types, label, "msg_typename_max");
    return PokedexTaskWordMessage(tables.ui, message, 0x0103, word);
  }
  return PokedexMessageLabel(tables.ui, message, "msg_pokedex_max", true);
}
} // namespace pla
