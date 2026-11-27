// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// PKHeX.Core PokedexSaveData/ResearchEntry/StatisticsEntry/ResearchTask (MIT),
// pinned6b21d83543db3e7fb7149000e8b13a2753c2f014. Pure decoded-data foundation;
// no guest pointer, saved-block fallback, or current-state freshness authority.
#pragma once
#include "pla_metadata.h"
#include "pla_text.h"
#include <algorithm>
#include <array>
#include <optional>

namespace pla {
inline constexpr size_t PokedexDataSize = 0x1e460;
inline uint16_t DexU16(std::span<const uint8_t> b, size_t at) {
  return uint16_t(b[at]) | uint16_t(b[at + 1]) << 8;
}
inline uint32_t DexU32(std::span<const uint8_t> b, size_t at) {
  return uint32_t(DexU16(b, at)) | uint32_t(DexU16(b, at + 2)) << 16;
}
struct PokedexForm {
  unsigned form = 0, storage_index = 0;
};
struct PokedexSpeciesIndex {
  unsigned hisui = 0;
  std::array<unsigned, 5> local{};
  std::vector<PokedexForm> forms;
};
struct PokedexIndex {
  bool valid = false;
  std::array<PokedexSpeciesIndex, 906> species;
};
inline PokedexIndex ReadPokedexIndex(std::span<const uint8_t> personal,
                                     std::span<const uint8_t> form_storage) {
  // Native form storage: u16count followed by318(species,form,index)u16
  // triples.
  if (form_storage.size() != 1910 || DexU16(form_storage, 0) != 318)
    return {};
  FlatMetadata f(personal);
  const auto tables = f.Tables(f.Read(0, 4), 0, 2048);
  if (!f.Valid() || tables.size() != 1276)
    return {};
  PokedexIndex out;
  for (const auto t : tables) {
    const auto species = unsigned(f.Scalar(t, 0, 2)),
               form = unsigned(f.Scalar(t, 1, 2));
    const auto dex = unsigned(f.Scalar(t, 49, 2));
    if (species > 905 || form > 27 || dex > 242 || (!species && dex))
      return {};
    if (!dex)
      continue;
    auto &entry = out.species[species];
    if (entry.hisui && entry.hisui != dex)
      return {};
    entry.hisui = dex;
    for (unsigned j = 0; j < 5; ++j) {
      const auto local = unsigned(f.Scalar(t, 51 + j, 4));
      if (local > 242)
        return {};
      // Primary GetDexIndex scans forms in table order for a nonzero local
      // index.
      if (!entry.local[j])
        entry.local[j] = local;
    }
  }
  if (!f.Valid())
    return {};
  std::array<bool, 243> used_dex{};
  unsigned count = 0;
  for (unsigned species = 1; species <= 905; ++species) {
    const auto dex = out.species[species].hisui;
    if (dex && used_dex[dex])
      return {};
    if (dex) {
      used_dex[dex] = true;
      ++count;
    }
  }
  if (count != 242)
    return {};
  std::array<bool, 1480> used_storage{};
  std::vector<unsigned> keys;
  for (size_t at = 2; at < form_storage.size(); at += 6) {
    const unsigned species = DexU16(form_storage, at),
                   form = DexU16(form_storage, at + 2);
    const unsigned storage = DexU16(form_storage, at + 4),
                   key = species | form << 11;
    if (species > 905 || form > 27 || storage >= 1480 ||
        used_storage[storage] ||
        std::find(keys.begin(), keys.end(), key) != keys.end())
      return {};
    keys.push_back(key);
    used_storage[storage] = true;
    if (!species) {
      if (form || storage)
        return {};
      continue;
    }
    if (!out.species[species].hisui)
      return {};
    out.species[species].forms.push_back({form, storage});
  }
  for (const auto &entry : out.species)
    if (entry.hisui && entry.forms.empty())
      return {};
  out.valid = true;
  return out;
}
struct PokedexSnapshot {
  bool valid = false;
  uint32_t global_flags = 0;
  unsigned total_research_points = 0;
  std::vector<uint8_t> bytes;
};
inline PokedexSnapshot ParsePokedexSnapshot(std::span<const uint8_t> bytes) {
  if (bytes.size() != PokedexDataSize)
    return {};
  const auto points = DexU32(bytes, 0xc), flags = DexU32(bytes, 0);
  // Primary PokedexSave8a.UpdateSpecificReportPoke caps total points99999;
  // per-species ResearchRate and task counters cap60000 in the same source.
  if (points > 99999 || (flags & ~0xfffu))
    return {};
  PokedexSnapshot out;
  out.global_flags = flags;
  out.total_research_points = points;
  out.bytes.assign(bytes.begin(), bytes.end());
  out.valid = true;
  return out;
}
struct PokedexFormState {
  bool valid = false, revealed = false;
  unsigned species = 0, form = 0;
  uint8_t seen_flags = 0, obtained_flags = 0, caught_flags = 0;
};
inline PokedexFormState ReadPokedexForm(const PokedexSnapshot &snapshot,
                                        const PokedexIndex &index,
                                        unsigned species, unsigned form) {
  if (!snapshot.valid || snapshot.bytes.size() != PokedexDataSize ||
      !index.valid || !species || species > 905 ||
      !index.species[species].hisui || form > 27)
    return {};
  for (const auto &mapping : index.species[species].forms) {
    if (mapping.form != form)
      continue;
    if (mapping.storage_index >= 1480)
      return {};
    const size_t at = 0x151a8 + mapping.storage_index * 0x18;
    PokedexFormState out;
    out.valid = true;
    out.seen_flags = snapshot.bytes[at + 4];
    out.obtained_flags = snapshot.bytes[at + 5];
    out.caught_flags = snapshot.bytes[at + 6];
    out.revealed = out.seen_flags || out.obtained_flags;
    if (out.revealed) {
      out.species = species;
      out.form = form;
    }
    return out;
  }
  return {};
}
struct PokedexEntry {
  bool valid = false, revealed = false, seen_in_wild = false, obtained = false;
  bool ever_updated = false, has_report = false, perfect = false,
       selected_form_revealed = false;
  unsigned species = 0, selected_form = 0, obtained_count = 0,
           raw_research_rate = 0;
  // Raw progress for task projection; zeroed when species is unrevealed.
  uint32_t reported_progress = 0;
  std::array<unsigned, 29> counters{}; // payloadresearch+10..48, u16
};
inline PokedexEntry ReadPokedexEntry(const PokedexSnapshot &snapshot,
                                     const PokedexIndex &index,
                                     unsigned species) {
  if (!snapshot.valid || snapshot.bytes.size() != PokedexDataSize ||
      !index.valid || !species || species > 905 ||
      !index.species[species].hisui)
    return {};
  const std::span<const uint8_t> bytes = snapshot.bytes;
  const size_t at = 0x70 + species * 0x58;
  const auto flags = DexU32(bytes, at), progress = DexU32(bytes, at + 4);
  const auto rate = DexU16(bytes, at + 8), obtained = DexU16(bytes, at + 0xa);
  if ((flags & ~0x7fu) || rate > 60000 || obtained > 60000 ||
      bytes[at + 0x50] > 27)
    return {};
  bool seen = false, stats_obtained = false;
  for (const auto &form : index.species[species].forms) {
    if (form.storage_index >= 1480)
      return {};
    const size_t stat = 0x151a8 + form.storage_index * 0x18;
    seen |= bytes[stat + 4] != 0;
    stats_obtained |= bytes[stat + 5] != 0;
  }
  PokedexEntry out;
  out.valid = true;
  // HasEverBeenUpdated is broader than sightings. No name/icon/task reveal
  // from that flag alone, nor from a report flag on inconsistent edited data.
  out.revealed = seen || stats_obtained || obtained;
  if (!out.revealed)
    return out;
  out.seen_in_wild = seen;
  out.obtained = obtained != 0;
  out.ever_updated = flags & 1;
  out.has_report = flags & 2;
  out.perfect = flags & 4;
  out.species = species;
  const auto form = ReadPokedexForm(snapshot, index, species, bytes[at + 0x50]);
  out.selected_form_revealed = form.valid && form.revealed;
  if (out.selected_form_revealed)
    out.selected_form = bytes[at + 0x50];
  out.obtained_count = obtained;
  out.raw_research_rate = rate;
  out.reported_progress = progress;
  for (unsigned j = 0; j < out.counters.size(); ++j) {
    out.counters[j] = DexU16(bytes, at + 0x10 + j * 2);
    if (out.counters[j] > 60000)
      return {};
  }
  return out;
}
inline std::string PokedexSpeciesName(const PokedexSnapshot &snapshot,
                                      const PokedexIndex &index,
                                      unsigned species,
                                      std::span<const uint8_t> names) {
  const auto entry = ReadPokedexEntry(snapshot, index, species);
  return entry.valid && entry.revealed ? Message(names, species)
                                       : std::string{};
}
enum class PokedexEntryTextKind { Unavailable, Generic, Ongoing, Full };
// Native UI1302C30..1302C48 chooses A for rawResearchRate>=100, otherwise
// B when HasAnyReport, otherwise generic1303268. Form/text-table lookup remains
// separate; this enum never authorizes unseen species/forms or invents a row.
inline PokedexEntryTextKind
PokedexEntryTextPermission(const PokedexEntry &entry,
                           const PokedexFormState &form) {
  if (!entry.valid || !entry.revealed || !entry.species || !form.valid ||
      !form.revealed || form.species != entry.species || form.form > 27)
    return PokedexEntryTextKind::Unavailable;
  if (entry.raw_research_rate >= 100)
    return PokedexEntryTextKind::Full;
  return entry.has_report ? PokedexEntryTextKind::Ongoing
                          : PokedexEntryTextKind::Generic;
}
inline PokedexEntryTextKind
PokedexEntryTextPermission(const PokedexEntry &entry) {
  PokedexFormState selected;
  selected.valid = selected.revealed = entry.selected_form_revealed;
  selected.species = entry.species;
  selected.form = entry.selected_form;
  return PokedexEntryTextPermission(entry, selected);
}
inline std::optional<unsigned>
PokedexGenericCommentVariant(const PokedexEntry &entry,
                             const PokedexFormState &form) {
  if (PokedexEntryTextPermission(entry, form) != PokedexEntryTextKind::Generic)
    return std::nullopt;
  // Native fallback common/pokedex msg_ui_pokedex_16_02 /16_01 /16_00.
  return entry.obtained_count ? 2u : entry.ever_updated ? 1u : 0u;
}

struct PokedexSummary {
  bool valid = false;
  unsigned total = 0, revealed = 0, seen_in_wild = 0, obtained = 0,
           reported = 0, perfect = 0;
  // Raw recorded rate buckets only. Required-task correction is a separate
  // operation; this array must not be labelled native completed-species count.
  std::array<unsigned, 11> raw_level_counts{};
};
inline PokedexSummary ReadPokedexSummary(const PokedexSnapshot &snapshot,
                                         const PokedexIndex &index,
                                         unsigned area = 0) {
  if (!snapshot.valid || !index.valid || area > 5)
    return {};
  PokedexSummary out;
  for (unsigned species = 1; species <= 905; ++species) {
    const auto &record = index.species[species];
    if (!record.hisui || (area && !record.local[area - 1]))
      continue;
    const auto entry = ReadPokedexEntry(snapshot, index, species);
    if (!entry.valid)
      return {};
    ++out.total;
    out.revealed += entry.revealed;
    out.seen_in_wild += entry.seen_in_wild;
    out.obtained += entry.obtained;
    out.reported += entry.has_report;
    out.perfect += entry.perfect;
    ++out.raw_level_counts[std::min(entry.raw_research_rate / 10, 10u)];
  }
  out.valid = true;
  return out;
}
// Exact pkNX3bc649ee PokedexResearchTable.fbs. Native TaskType is retained
// numerically. Native1030984 dispatch independently proves0..24counter offsets;
// 12F77E0/12F77F0 assign occurrence indices for types1/2. External25..27 remain
// unavailable here.
struct PokedexNativeTask {
  unsigned species = 0, native_type = 0, qualifier = 0;
  int counter_index = -1;
  unsigned move = 0, move_type = 0, time_of_day = 0, points_single = 0,
           points_bonus = 0;
  bool required_for_completion = false;
  std::array<uint64_t, 3> hashes{};
  std::vector<unsigned> thresholds;
};
struct PokedexResearchTable {
  bool valid = false;
  std::array<std::vector<PokedexNativeTask>, 906> species;
};
inline PokedexResearchTable
ReadPokedexResearchTable(std::span<const uint8_t> bytes) {
  FlatMetadata f(bytes);
  const auto rows = f.Tables(f.Read(0, 4), 0, 2048);
  if (!f.Valid() || rows.size() != 1611)
    return {};
  PokedexResearchTable out;
  std::array<unsigned, 906> move_indices{}, type_indices{};
  for (const auto row : rows) {
    PokedexNativeTask task;
    task.species = unsigned(f.Scalar(row, 0, 4));
    task.native_type = unsigned(f.Scalar(row, 1, 4));
    task.qualifier = unsigned(f.Scalar(row, 2, 4));
    task.move = unsigned(f.Scalar(row, 3, 4));
    task.move_type = unsigned(f.Scalar(row, 4, 4));
    task.time_of_day = unsigned(f.Scalar(row, 5, 4));
    for (unsigned j = 0; j < 3; ++j)
      task.hashes[j] = f.Scalar(row, 6 + j, 8);
    task.points_single = unsigned(f.Scalar(row, 14, 4));
    task.points_bonus = unsigned(f.Scalar(row, 15, 4));
    const auto required = f.Scalar(row, 16, 1);
    task.required_for_completion = bool(required);
    if (!task.species || task.species > 905 || task.native_type > 27 ||
        task.qualifier > 0x7fffffffu || task.move > 850 ||
        task.move_type >= 18 || task.time_of_day > 5 ||
        task.points_single > 255 || task.points_bonus > 255 || required > 1)
      return {};
    if (task.native_type == 1) {
      if (move_indices[task.species] >= 4)
        return {};
      task.counter_index = int(move_indices[task.species]++);
    } else if (task.native_type == 2) {
      if (type_indices[task.species] >= 3)
        return {};
      task.counter_index = int(type_indices[task.species]++);
    }
    unsigned previous = 0;
    bool ended = false;
    for (unsigned j = 0; j < 5; ++j) {
      const auto threshold = unsigned(f.Scalar(row, 9 + j, 4));
      if (!threshold) {
        ended = true;
        continue;
      }
      if (ended || threshold <= previous || threshold > 60000)
        return {};
      previous = threshold;
      task.thresholds.push_back(threshold);
    }
    auto &tasks = out.species[task.species];
    if (!f.Valid() || task.thresholds.empty() || tasks.size() >= 10)
      return {};
    tasks.push_back(std::move(task));
  }
  out.valid = true;
  return out;
}
inline std::vector<PokedexNativeTask>
PokedexRevealedTasks(const PokedexSnapshot &snapshot, const PokedexIndex &index,
                     const PokedexResearchTable &table, unsigned species) {
  const auto entry = ReadPokedexEntry(snapshot, index, species);
  return table.valid && entry.valid && entry.revealed
             ? table.species[species]
             : std::vector<PokedexNativeTask>{};
}
struct PokedexTaskDefinition {
  bool valid = false, required_for_completion = false;
  unsigned type = 0, move = 0, move_type = 0, time_of_day = 0;
  int index = -1;
  std::vector<unsigned> thresholds;
};
// This accepts PKHeX's DERIVED40-byte task record. The native ROMFS
// FlatBuffer is handled separately by ReadPokedexResearchTable, never cast
// here.
inline PokedexTaskDefinition
ReadPokedexTaskDefinition(std::span<const uint8_t> bytes) {
  if (bytes.size() != 0x28 || bytes[0] > 27 || bytes[0x20] < 1 ||
      bytes[0x20] > 5 || bytes[0x26] > 1)
    return {};
  PokedexTaskDefinition out;
  out.type = bytes[0];
  out.move = DexU16(bytes, 4);
  out.move_type = bytes[6];
  out.time_of_day = bytes[7];
  out.required_for_completion = bytes[0x26];
  if (out.move > 850 || out.move_type >= 18 || out.time_of_day > 5)
    return {};
  if (out.type == 1 || out.type == 2) {
    out.index = bytes[0x27];
    if (out.index >= (out.type == 1 ? 4 : 3))
      return {};
  }
  unsigned previous = 0;
  for (unsigned j = 0; j < bytes[0x20]; ++j) {
    const unsigned threshold = bytes[0x21 + j];
    if (threshold <= previous)
      return {};
    previous = threshold;
    out.thresholds.push_back(threshold);
  }
  out.valid = true;
  return out;
}
struct PokedexTaskProgress {
  bool valid = false, current_available = false;
  unsigned current = 0, reported_thresholds = 0, achieved_thresholds = 0,
           unreported = 0;
};
inline PokedexTaskProgress
ReadPokedexTaskProgress(const PokedexEntry &entry,
                        const PokedexTaskDefinition &task, unsigned slot) {
  if (!entry.valid || !entry.revealed || !task.valid || slot >= 10 ||
      task.thresholds.empty() || task.thresholds.size() > 5)
    return {};
  const unsigned raw_reported = (entry.reported_progress >> (slot * 3)) & 7;
  const unsigned reported = std::max(raw_reported, 1u) - 1;

  PokedexTaskProgress out;
  out.valid = true;
  out.reported_thresholds = reported;
  if (task.type == 0)
    out.current = entry.obtained_count;
  else if (task.type == 1 && task.index >= 0 && task.index < 4)
    out.current = entry.counters[task.index];
  else if (task.type == 2 && task.index >= 0 && task.index < 3)
    out.current = entry.counters[4 + task.index];
  else if (task.type >= 3 && task.type <= 24)
    out.current = entry.counters[task.type + 4];
  else
    return out; // Forms/gender, Arceus and quest tasks need additional sources.
  unsigned achieved = 0;
  for (const auto threshold : task.thresholds) {
    if (out.current < threshold)
      break;
    ++achieved;
  }
  // PKHeX compatibility projection keeps reported progress monotonic even
  // for edited saves with a smaller current counter. It is not a live event.
  out.current_available = true;
  out.achieved_thresholds = std::max(achieved, reported);
  out.unreported = out.achieved_thresholds - reported;
  return out;
}
inline PokedexTaskDefinition
PokedexDefinitionFromNative(const PokedexResearchTable &table, unsigned species,
                            unsigned slot) {
  if (!table.valid || !species || species > 905 ||
      slot >= table.species[species].size())
    return {};
  const auto &task = table.species[species][slot];
  PokedexTaskDefinition out;
  out.type = task.native_type;
  out.move = task.move;
  out.move_type = task.move_type;
  out.time_of_day = task.time_of_day;
  out.index = task.counter_index;
  out.required_for_completion = task.required_for_completion;
  out.thresholds = task.thresholds;
  out.valid = true;
  return out;
}
inline PokedexTaskProgress ReadPokedexNativeTaskProgress(
    const PokedexSnapshot &snapshot, const PokedexIndex &index,
    const PokedexResearchTable &table, unsigned species, unsigned slot) {
  return ReadPokedexTaskProgress(
      ReadPokedexEntry(snapshot, index, species),
      PokedexDefinitionFromNative(table, species, slot), slot);
}
inline std::optional<unsigned>
PokedexReportedResearchRate(const PokedexEntry &entry,
                            std::span<const PokedexTaskDefinition> tasks) {
  if (!entry.valid || !entry.revealed || tasks.empty() || tasks.size() > 10)
    return std::nullopt;
  bool required_done = true;
  for (unsigned slot = 0; slot < tasks.size(); ++slot) {
    if (!tasks[slot].valid)
      return std::nullopt;
    const auto progress = ReadPokedexTaskProgress(entry, tasks[slot], slot);
    if (!progress.valid)
      return std::nullopt;
    if (tasks[slot].required_for_completion && !progress.reported_thresholds)
      required_done = false;
  }
  return entry.raw_research_rate >= 100 && !required_done
             ? 99
             : entry.raw_research_rate;
}
inline std::optional<unsigned>
PokedexResearchRate(const PokedexSnapshot &snapshot, const PokedexIndex &index,
                    const PokedexResearchTable &table, unsigned species) {
  if (!table.valid || !species || species > 905)
    return std::nullopt;
  std::vector<PokedexTaskDefinition> definitions;
  for (unsigned i = 0; i < table.species[species].size(); ++i)
    definitions.push_back(PokedexDefinitionFromNative(table, species, i));
  return PokedexReportedResearchRate(ReadPokedexEntry(snapshot, index, species),
                                     definitions);
}
struct PokedexResearchSummary {
  bool valid = false;
  unsigned total = 0, completed = 0;
  // Rate buckets0..9 are floor(rate/10); bucket10 means reported rate>=100.
  // This is a count grouping, not a claim that the raw rate/display is capped.
  std::array<unsigned, 11> level_counts{};
};
inline PokedexResearchSummary ReadPokedexResearchSummary(
    const PokedexSnapshot &snapshot, const PokedexIndex &index,
    const PokedexResearchTable &table, unsigned area = 0) {
  if (!snapshot.valid || !index.valid || !table.valid || area > 5)
    return {};
  PokedexResearchSummary out;
  for (unsigned species = 1; species <= 905; ++species) {
    const auto &catalog = index.species[species];
    if (!catalog.hisui || (area && !catalog.local[area - 1]))
      continue;
    const auto entry = ReadPokedexEntry(snapshot, index, species);
    if (!entry.valid)
      return {};
    ++out.total;
    if (!entry.revealed) {
      ++out.level_counts[0];
      continue;
    }
    const auto rate = PokedexResearchRate(snapshot, index, table, species);
    if (!rate.has_value())
      return {};
    out.completed += *rate >= 100;
    ++out.level_counts[std::min(*rate / 10, 10u)];
  }
  out.valid = true;
  return out;
}
} // namespace pla
