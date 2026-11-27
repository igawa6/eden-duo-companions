// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Pure received-list projection; no guest reads, actions, or text assets.
// PLA 1.1.1 native 1270B18..70 excludes status0/1. 15ACE7C..D8 maps
// raw0/1/255 to status0/1/255; other bytes map to2. Require a known step
// too. Field9/11 use the original field8 ordinal (15A5C14/15A5DC0).
#pragma once
#include "pla_missions.h"
#include <array>
#include <optional>

namespace pla {
inline constexpr uint64_t MissionTrackedNone = 0xC0EA47549AB5F3D9ULL;
struct MissionPageObservation {
  bool request = false;
  uint64_t state_hash = 0;
  bool readable = false;
  uint8_t raw = 0;
};
struct MissionPageSnapshot {
  // Acquisition/provenance is the caller's responsibility. Owned bytes and a
  // generation bind consumers; they do not prove coherence or freshness.
  bool available = false;
  uint64_t generation = 0, tracked_hash = MissionTrackedNone;
  std::vector<MissionPageObservation> observations;
};
enum class MissionReceivedStatus { Active, Completed };
struct MissionPageRow {
  bool request = false;
  uint64_t state_hash = 0, generation = 0;
  uint8_t raw = 0;
  unsigned ordinal = 0; // Original zero-based vector position, never a UI pip.
  MissionReceivedStatus status = MissionReceivedStatus::Active;
  uint64_t title_hash = 0, summary_hash = 0, purpose_hash = 0;
};
struct MissionPageModel {
  bool available = false;
  uint64_t generation = 0;
  std::vector<MissionPageRow> rows;
  // Main/sub observations absent, unreadable or unrecognized. Counts derived
  // from rows are partial when the respective count is nonzero.
  std::array<unsigned, 2> unavailable{};
  std::optional<size_t> tracked_row;
};
inline MissionPageModel BuildMissionPage(const MissionDefinitions &main,
                                         const MissionDefinitions &sub,
                                         const MissionPageSnapshot &snapshot) {
  if (!snapshot.available || !main.valid || !sub.valid ||
      main.quests.size() != 27 || sub.quests.size() != 122 ||
      snapshot.observations.size() > 149)
    return {};
  constexpr uint64_t empty = 0xCBF29CE484222645ULL;
  std::vector<const MissionDefinition *> definitions;
  for (const auto *set : {&main, &sub}) {
    for (const auto &q : set->quests) {
      if (q.request != (set == &sub) || !q.id ||
          q.id > (q.request ? 122u : 27u) || !q.state_hash ||
          q.state_hash == empty || q.state_hash == MissionTrackedNone ||
          !q.title_hash || q.title_hash == empty || q.steps.empty() ||
          q.steps.size() > 64)
        return {};
      for (const auto *prior : definitions)
        if (q.state_hash == prior->state_hash ||
            (q.request == prior->request && q.id == prior->id))
          return {};
      for (size_t j = 0; j < q.steps.size(); ++j)
        for (size_t k = 0; k < j; ++k)
          if (q.steps[j].id && q.steps[j].id == q.steps[k].id)
            return {};
      definitions.push_back(&q);
    }
  }
  // Resolve observations before producing any identifying output. A supplied
  // category/hash mismatch, foreign identity or duplicate makes it malformed.
  for (size_t i = 0; i < snapshot.observations.size(); ++i) {
    const auto &o = snapshot.observations[i];
    const auto q = std::find_if(definitions.begin(), definitions.end(),
        [&](const auto *d) { return d->state_hash == o.state_hash &&
                                    d->request == o.request; });
    if (q == definitions.end())
      return {};
    for (size_t j = 0; j < i; ++j)
      if (snapshot.observations[j].state_hash == o.state_hash)
        return {};
  }
  MissionPageModel result;
  result.available = true;
  result.generation = snapshot.generation;
  for (const auto *q : definitions) {
    const auto o = std::find_if(snapshot.observations.begin(),
        snapshot.observations.end(), [&](const auto &v) {
          return v.state_hash == q->state_hash && v.request == q->request;
        });
    if (o == snapshot.observations.end() || !o->readable) {
      ++result.unavailable[q->request];
      continue;
    }
    if (o->raw <= 1)
      continue; // Native received-list refusal: no identifying row or hashes.
    const auto step = std::find_if(q->steps.begin(), q->steps.end(),
        [&](const auto &s) { return s.id == o->raw; });
    if (step == q->steps.end()) {
      ++result.unavailable[q->request];
      continue;
    }
    const bool completed = o->raw == 255;
    const auto usable = [&](uint64_t h) { return h == empty ? uint64_t(0) : h; };
    result.rows.push_back({q->request, q->state_hash, snapshot.generation,
        o->raw, unsigned(step - q->steps.begin()),
        completed ? MissionReceivedStatus::Completed : MissionReceivedStatus::Active,
        q->title_hash, usable(step->outline),
        completed ? 0 : usable(step->purpose)});
    if (snapshot.tracked_hash != MissionTrackedNone &&
        snapshot.tracked_hash == q->state_hash)
      result.tracked_row = result.rows.size() - 1;
  }
  return result;
}
} // namespace pla
