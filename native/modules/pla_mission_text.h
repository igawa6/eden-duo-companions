// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only received-row text. Revalidate against the owned source snapshot;
// category-specific AHTB hashes select native DAT rows, never quest ID math.
#pragma once
#include "pla_mission_page.h"
#include "pla_text.h"
#include <string_view>
#include <unordered_set>

namespace pla {
struct MissionTextTable {
  std::span<const uint8_t> dat, tbl;
};
struct MissionTextTables {
  MissionTextTable main, sub;
};
struct MissionRowStrings {
  std::string title, summary, purpose;
};
namespace mission_text_detail {
inline uint64_t Hash(std::string_view label) {
  uint64_t hash = 0xCBF29CE484222645ULL;
  for (const unsigned char c : label)
    hash = (hash ^ c) * 0x100000001B3ULL;
  return hash;
}
inline std::string Lookup(const MissionTextTable &table, uint64_t wanted,
                          std::string_view sentinel) {
  if (!wanted || wanted == 0xCBF29CE484222645ULL ||
      wanted == Hash(sentinel) || table.dat.size() < 16 ||
      table.dat.size() > 1024 * 1024 || table.tbl.size() < 8 ||
      table.tbl.size() > 1024 * 1024)
    return {};
  const auto u16 = [](std::span<const uint8_t> b, size_t at) {
    return unsigned(b[at]) | unsigned(b[at + 1]) << 8;
  };
  const auto u32 = [&](std::span<const uint8_t> b, size_t at) {
    return uint32_t(u16(b, at)) | uint32_t(u16(b, at + 2)) << 16;
  };
  if (u16(table.dat, 0) != 1 || u32(table.tbl, 0) != 0x42544841)
    return {};
  const unsigned rows = u16(table.dat, 2), count = u32(table.tbl, 4);
  if (!rows || count != rows + 1 || count > 8192 ||
      count > (table.tbl.size() - 8) / 10)
    return {};
  size_t at = 8;
  std::optional<unsigned> found;
  std::unordered_set<uint64_t> hashes;
  hashes.reserve(count);
  for (unsigned row = 0; row < count; ++row) {
    if (at > table.tbl.size() || table.tbl.size() - at < 10)
      return {};
    const uint64_t hash = uint64_t(u32(table.tbl, at)) |
                          uint64_t(u32(table.tbl, at + 4)) << 32;
    const size_t length = u16(table.tbl, at + 8);
    at += 10;
    if (length < 2 || length > 1024 || length > table.tbl.size() - at ||
        table.tbl[at + length - 1])
      return {};
    const std::string_view label(
        reinterpret_cast<const char *>(table.tbl.data() + at), length - 1);
    // Dotted purpose suffixes are present in the native main table.
    for (const unsigned char c : label)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.'))
        return {};
    if (Hash(label) != hash ||
        (label == sentinel) != (row == rows) ||
        !hashes.insert(hash).second)
      return {};
    if (hash == wanted)
      found = row;
    at += length;
  }
  if (at != table.tbl.size() || !found.has_value())
    return {};
  // Shared decoder rejects control words, aliases and surrogate units.
  return Message(table.dat, *found, true);
}
inline bool SameRow(const MissionPageRow &a, const MissionPageRow &b) {
  return a.request == b.request && a.state_hash == b.state_hash &&
      a.generation == b.generation && a.raw == b.raw &&
      a.ordinal == b.ordinal && a.status == b.status &&
      a.title_hash == b.title_hash && a.summary_hash == b.summary_hash &&
      a.purpose_hash == b.purpose_hash;
}
} // namespace mission_text_detail
enum class MissionUiLabel {
  Missions, Requests, Sort, NumericalOrder, Summary, CurrentObjective, NoneReceived
};
inline std::string MissionUiLabelText(MissionUiLabel label,
                                      const MissionTextTable &table) {
  const char *name = nullptr;
  switch (label) {
  case MissionUiLabel::Missions: name = "msg_ui_mssn_01_01"; break;
  case MissionUiLabel::Requests: name = "msg_ui_mssn_01_02"; break;
  case MissionUiLabel::Sort: name = "msg_ui_mssn_02_02"; break;
  case MissionUiLabel::NumericalOrder: name = "msg_ui_mssn_03_01"; break;
  case MissionUiLabel::Summary: name = "msg_ui_mssn_05_01"; break;
  case MissionUiLabel::CurrentObjective: name = "msg_ui_mssn_06_01"; break;
  case MissionUiLabel::NoneReceived: name = "msg_ui_mssn_25_01"; break;
  default: return {};
  }
  return mission_text_detail::Lookup(table, mission_text_detail::Hash(name),
                                     "msg_mssn_max");
}
inline MissionRowStrings MissionRowText(const MissionPageRow &row,
                                         const MissionDefinitions &main,
                                         const MissionDefinitions &sub,
                                         const MissionPageSnapshot &snapshot,
                                         const MissionTextTables &tables) {
  const auto model = BuildMissionPage(main, sub, snapshot);
  if (!model.available ||
      std::none_of(model.rows.begin(), model.rows.end(), [&](const auto &allowed) {
        return mission_text_detail::SameRow(row, allowed);
      }))
    return {};
  const auto &table = row.request ? tables.sub : tables.main;
  const auto sentinel = row.request ? "msg_questlist_sub_max"
                                    : "msg_questlist_main_max";
  return {mission_text_detail::Lookup(table, row.title_hash, sentinel),
          mission_text_detail::Lookup(table, row.summary_hash, sentinel),
          mission_text_detail::Lookup(table, row.purpose_hash, sentinel)};
}
} // namespace pla
