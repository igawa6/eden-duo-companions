// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Native PLA ROMFS FlatBuffers: pkNX (MIT), commit
// 3bc649eea311ab1f249af43a562f8d70fe90ed9b, PersonalInfoLAfb.fbs / Waza.fbs.
#pragma once
#include <cstdio>
#include <string>
#include "pla_metadata.h"
#include "pla_text.h"
#include "re4_sha256.h"
#include <array>
#include <optional>

namespace pla {
struct PokemonPersonal {
  bool valid = false, present = false;
  unsigned type1 = 0, type2 = 0, hisui_dex = 0;
  // Schema order: HP, Attack, Defense, Sp. Atk, Sp. Def, Speed.
  std::array<unsigned, 6> base_stats{};
  // Personal fields 20/21: height in centimetres, weight in hectograms (Onix 880 / 2100).
  unsigned height_cm = 0, weight_hg = 0;
};
inline PokemonPersonal ReadPokemonPersonal(std::span<const uint8_t> bytes,
                                           unsigned species, unsigned form) {
  if (!species || species > 905 || form > 255)
    return {};
  FlatMetadata f(bytes);
  const auto tables = f.Tables(f.Read(0, 4), 0, 2048);
  // Exact 1.1.1 table; no guessed fallback from an absent form to form0.
  if (!f.Valid() || tables.size() != 1276)
    return {};
  PokemonPersonal result;
  unsigned matches = 0;
  for (const auto table : tables) {
    const auto id = f.Scalar(table, 0, 2), variant = f.Scalar(table, 1, 2);
    if (id > 905 || variant > 255)
      return {};
    if (id != species || variant != form)
      continue;
    ++matches;
    const auto present = f.Scalar(table, 2, 1);
    result.present = bool(present);
    result.type1 = unsigned(f.Scalar(table, 3, 1));
    result.type2 = unsigned(f.Scalar(table, 4, 1));
    for (unsigned j = 0; j < 6; ++j)
      result.base_stats[j] = unsigned(f.Scalar(table, 8 + j, 1));
    result.hisui_dex = unsigned(f.Scalar(table, 49, 2));
    result.height_cm = unsigned(f.Scalar(table, 20, 2));
    result.weight_hg = unsigned(f.Scalar(table, 21, 2));
    if (present > 1 || result.type1 >= 18 || result.type2 >= 18 ||
        result.hisui_dex > 242)
      return {};
  }
  if (!f.Valid() || matches != 1)
    return {};
  result.valid = true;
  return result;
}
// English game text uses feet/inches and pounds (Paras: 1'00", 11.9 lbs).
inline std::string PokemonHeightText(unsigned cm) {
  if (!cm) return {};
  const unsigned inches = unsigned(cm / 2.54 + 0.5);
  char text[16];
  std::snprintf(text, sizeof text, "%u'%02u\"", inches / 12, inches % 12);
  return text;
}
inline std::string PokemonWeightText(unsigned hg) {
  if (!hg) return {};
  char text[24];
  std::snprintf(text, sizeof text, "%.1f lbs.", hg * 0.220462);
  return text;
}
struct PokemonMove {
  bool valid = false, usable = false, can_style = false;
  // Native type IDs follow typename.dat; category0=status,1=physical,2=special.
  unsigned type = 0, category = 0, power = 0, accuracy = 0, pp = 0;
  unsigned agile_power = 0, strong_power = 0, strong_accuracy = 0;
  // Raw native modifiers, not an inferred turn-order/damage forecast.
  std::array<int32_t, 3> speed_user{};   // normal, Agile, Strong
  std::array<int32_t, 2> speed_target{}; // normal, Agile
};
inline PokemonMove ReadPokemonMove(std::span<const uint8_t> bytes,
                                   unsigned id) {
  if (!id || id > 850 || bytes.size() > 4096)
    return {};
  FlatMetadata f(bytes);
  const auto table = f.Read(0, 4);
  if (f.Scalar(table, 0, 4) != id)
    return {};
  PokemonMove result;
  const auto usable = f.Scalar(table, 1, 1), style = f.Scalar(table, 68, 1);
  result.usable = bool(usable);
  result.can_style = bool(style);
  result.type = unsigned(f.Scalar(table, 2, 1));
  result.category = unsigned(f.Scalar(table, 4, 1));
  result.power = unsigned(f.Scalar(table, 5, 1));
  result.accuracy = unsigned(f.Scalar(table, 6, 1));
  result.pp = unsigned(f.Scalar(table, 7, 1));
  result.agile_power = unsigned(f.Scalar(table, 74, 1));
  result.strong_power = unsigned(f.Scalar(table, 79, 1));
  result.strong_accuracy = unsigned(f.Scalar(table, 80, 1));
  for (unsigned j = 0; j < 3; ++j)
    result.speed_user[j] = int32_t(uint32_t(f.Scalar(table, 69 + j, 4)));
  for (unsigned j = 0; j < 2; ++j)
    result.speed_target[j] = int32_t(uint32_t(f.Scalar(table, 72 + j, 4)));
  if (!f.Valid() || usable > 1 || style > 1 || result.type >= 18 ||
      result.category > 2 || result.accuracy > 101 ||
      result.strong_accuracy > 101 || !result.pp || result.pp > 40)
    return {};
  result.valid = true;
  return result;
}
// Exact 1.1.1 main+0x3984908, attacker-major18x18. Type-only nominal
// matchups: no battle inversion, move exceptions, statuses, or damage forecast.
inline bool ValidPokemonTypeChart(std::span<const uint8_t> chart) {
  return chart.size() == 324 &&
         re4::Sha256(chart) ==
             "7ce3941bb754bac872d6e4864201805ae739d5f3e35e169d3482dbb4418eedc8";
}
struct PokemonTypeMultiplier {
  unsigned numerator = 0, denominator = 1;
  bool operator==(const PokemonTypeMultiplier &) const = default;
};
struct PokemonMatchups {
  bool valid = false;
  std::array<PokemonTypeMultiplier, 18> types{};
};
inline PokemonMatchups PokemonTypeMatchups(std::span<const uint8_t> chart,
                                           unsigned type1, unsigned type2) {
  if (type1 >= 18 || type2 >= 18 || !ValidPokemonTypeChart(chart))
    return {};
  PokemonMatchups result;
  for (unsigned attack = 0; attack < 18; ++attack) {
    const unsigned first = chart[attack * 18 + type1];
    const unsigned second = type1 == type2 ? 4 : chart[attack * 18 + type2];
    // Chart values have neutral4. Native283E2A0 combines to an exponent
    // enum; actual damage283EB50 remaps two weaknesses/resistances to
    // 2.5 / 0.4 rather than4 / 0.25. Keep exact rational display values.
    switch (first * second) {
    case 0:
      result.types[attack] = {0, 1};
      break;
    case 4:
      result.types[attack] = {2, 5};
      break;
    case 8:
      result.types[attack] = {1, 2};
      break;
    case 16:
      result.types[attack] = {1, 1};
      break;
    case 32:
      result.types[attack] = {2, 1};
      break;
    case 64:
      result.types[attack] = {5, 2};
      break;
    default:
      return {};
    }
  }
  result.valid = true;
  return result;
}
// PKHeX.Core (MIT) PersonalInfo8LA.RecordPermitIndexes / PA8.MasteredRecord,
// pinned source commit6b21d83543db3e7fb7149000e8b13a2753c2f014. The64-bit
// record uses only61 ordered move-shop indices. Other moves remain unknown;
// their level/seed mastery is not inferred from this record or Pokemon level.
inline std::optional<bool> PokemonMoveMastered(std::span<const uint8_t> record,
                                               unsigned move_id) {
  if (record.size() != 8 || !move_id || move_id > 850)
    return std::nullopt;
  constexpr std::array<unsigned, 61> moves{
      206, 424, 422, 423, 301, 249, 191, 523, 332, 446, 129, 161, 345,
      466, 829, 116, 339, 347, 156, 608, 7,   9,   8,   409, 398, 427,
      428, 141, 404, 157, 421, 442, 231, 595, 352, 451, 412, 196, 188,
      414, 247, 555, 430, 605, 416, 401, 528, 667, 224, 444, 200, 583,
      63,  53,  85,  58,  94,  399, 434, 796, 344};
  for (unsigned i = 0; i < moves.size(); ++i)
    if (moves[i] == move_id)
      return bool(record[i >> 3] & (1u << (i & 7)));
  return std::nullopt;
}
// pkNX Learnset.fbs: learnsetMeta field3 is the Arceus vector. Preserve
// repeated moves: native CheckWazaMastered accepts ANY matching threshold.
struct PokemonLearnedMove {
  unsigned move = 0, level = 0, mastery_level = 0;
};
struct PokemonLearnset {
  bool valid = false;
  std::vector<PokemonLearnedMove> moves;
};
inline PokemonLearnset ReadPokemonLearnset(std::span<const uint8_t> bytes,
                                           unsigned species, unsigned form) {
  if (!species || species > 905 || form > 27)
    return {};
  FlatMetadata f(bytes);
  const auto tables = f.Tables(f.Read(0, 4), 0, 2048);
  if (!f.Valid() || tables.size() != 1276)
    return {};
  PokemonLearnset result;
  unsigned matches = 0;
  for (const auto table : tables) {
    const auto id = f.Scalar(table, 0, 2), variant = f.Scalar(table, 1, 2);
    if (id > 905 || variant > 27)
      return {};
    if (id != species || variant != form)
      continue;
    ++matches;
    for (const auto entry : f.Tables(table, 3, 64)) {
      PokemonLearnedMove move;
      move.level = unsigned(f.Scalar(entry, 0, 2));
      move.mastery_level = unsigned(f.Scalar(entry, 1, 2));
      move.move = unsigned(f.Scalar(entry, 2, 2));
      if (move.level > 100 || move.mastery_level > 100 || !move.move ||
          move.move > 850)
        return {};
      result.moves.push_back(move);
    }
  }
  if (!f.Valid() || matches != 1)
    return {};
  result.valid = true;
  return result;
}
// Native CheckWazaMastered2B618D4: a reached LevelMaster threshold OR a
// mastered shop-record bit OR PA8.AlphaMove(+3E) equality. Current level comes
// from the validated native calculated tail, not a guessed mastery surrogate.
inline std::optional<bool> PokemonMoveMastery(
    const PokemonLearnset &learnset, std::span<const uint8_t> mastered_record,
    unsigned current_level, unsigned move_id, unsigned alpha_move) {
  if (!learnset.valid || mastered_record.size() != 8 || !current_level ||
      current_level > 100 || !move_id || move_id > 850 || alpha_move > 850)
    return std::nullopt;
  for (const auto &entry : learnset.moves)
    if (entry.move == move_id && current_level >= entry.mastery_level)
      return true;
  const auto shop = PokemonMoveMastered(mastered_record, move_id);
  if (shop.has_value() && *shop)
    return true;
  return alpha_move == move_id;
}
inline std::string PokemonTypeName(std::span<const uint8_t> dat, unsigned id) {
  return id < 18 ? Message(dat, id) : std::string{};
}
inline std::string PokemonNatureName(std::span<const uint8_t> dat,
                                     unsigned id) {
  return id < 25 ? Message(dat, id) : std::string{};
}
inline std::string PokemonMoveName(std::span<const uint8_t> dat, unsigned id) {
  return id && id <= 850 ? Message(dat, id) : std::string{};
}
inline std::string PokemonMoveDescription(std::span<const uint8_t> dat,
                                          unsigned id) {
  return id && id <= 850 ? Message(dat, id, true) : std::string{};
}
} // namespace pla
