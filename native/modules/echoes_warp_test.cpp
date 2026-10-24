// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_warp.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
namespace {
size_t checks{};
void Check(bool ok, const char *message) {
  ++checks;
  if (!ok) { std::cerr << "FAIL " << checks << ": " << message << '\n'; std::abort(); }
}
std::vector<uint8_t> File(const std::filesystem::path &path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
uint64_t Q(const std::vector<uint8_t> &b, size_t p) {
  uint64_t v{};
  for (size_t i = 0; i < 8; ++i) v |= uint64_t(b[p + i]) << (8 * i);
  return v;
}
}
int main() {
  const auto *root = std::getenv("ECHOES_ROMFS");
  if (!root) { std::cout << "ECHOES_ROMFS needed for original metadata tests\n"; return 77; }
  const std::filesystem::path dir(root);
  auto sets = File(dir / "region_common/datasheets/ui/UiWarpSet.gsheet");
  auto correspondence = File(dir / "region_common/datasheets/ui/UiWarpCorrespondenceTable.gsheet");
  echoes::NativeWarpCatalog catalog;
  Check(echoes::ParseNativeWarpCatalog(sets, correspondence, catalog), "original schema and records");
  Check(catalog.sets.size() == 2 && catalog.level_sets.size() == 21, "original set and mapping counts");
  const auto *field = echoes::NativeWarpSetForLevel(catalog, "Field");
  const auto *boundary = echoes::NativeWarpSetForLevel(catalog, "L_Dungeon001");
  Check(field && boundary && field->key == "Field" && boundary->key == "Boundary", "native level membership");
  Check(field->destinations.size() == 53 && boundary->destinations.size() == 51, "authored destination counts");
  Check(!echoes::NativeWarpSetForLevel(catalog, "DekuTreeArea"), "do not infer current-location aliases");
  size_t duplicates{}, special{};
  for (const auto &d : field->destinations) {
    if (d.level == "Field" && d.location == "F_Jungle_Warp_001") ++duplicates;
    if (d.unlock_flag == "Warp_Field_Volcano_005") {
      ++special;
      Check(d.level == "S_Dungeon018" && d.location == "F_Volcano_Warp_005" && d.report_id == 49,
            "interior destination remains distinct from Field map override");
    }
  }
  Check(duplicates == 2 && special == 1, "preserve native duplicate location identities");
  auto copy = catalog;
  copy.level_sets.push_back(copy.level_sets.front());
  Check(!echoes::NativeWarpSetForLevel(copy, copy.level_sets.front().first), "reject ambiguous mapping");
  const auto fail = [&](std::vector<uint8_t> a, std::vector<uint8_t> b) {
    auto old = catalog;
    Check(!echoes::ParseNativeWarpCatalog(a, b, old), "reject malformed metadata");
    Check(old.sets.empty() && old.level_sets.empty(), "refusal clears all output");
  };
  for (size_t i : {size_t(0), size_t(4), size_t(24), size_t(28), size_t(40), size_t(44)}) {
    auto b = sets; b[i] ^= 0x80; fail(b, correspondence);
  }
  for (size_t field_index = 0; field_index < 6; ++field_index)
    for (size_t member : {size_t(16), size_t(18), size_t(20), size_t(22), size_t(24), size_t(26), size_t(32)}) {
      auto b = sets; b[48 + field_index * 48 + member] ^= 0x80; fail(b, correspondence);
    }
  auto b = sets;
  const auto row = size_t(Q(b, 32)), array = size_t(Q(b, row + 16));
  for (size_t i = 0; i < 16; ++i) b[array + 56 + 32 + i] = b[array + 32 + i];
  for (size_t i = 0; i < 16; ++i) b[array + 56 + i] = b[array + i];
  fail(b, correspondence); // Same exact level/flag identity, regardless of location.
  b = sets; b[size_t(Q(b, row))] = 0; fail(b, correspondence);
  auto truncated = sets; truncated.resize(100); fail(truncated, correspondence);
  echoes::NativeWarpCatalog loaded;
  const auto read = [&](std::string_view path, uint64_t offset, void *out, size_t size) {
    const auto bytes = File(dir / path);
    if (!out && !size) return bytes.size();
    if (offset > bytes.size() || size > bytes.size() - size_t(offset)) return size_t{};
    std::copy_n(bytes.data() + offset, size, static_cast<uint8_t *>(out)); return size;
  };
  Check(echoes::LoadNativeWarpCatalog(read, loaded), "worker path loads original tables");
  Check(loaded.sets[1].destinations == field->destinations, "worker preserves exact order");
  Check(loaded.level_aliases.size() == 31, "original alias count");
  const auto *resolved = echoes::NativeWarpSetForCurrentLevel(loaded, "DekuTreeArea");
  Check(resolved && resolved->key == "Field", "native DekuTreeArea alias resolves Field");
  Check(echoes::NativeWarpSetForCurrentLevel(loaded, "Field") ==
            echoes::NativeWarpSetForLevel(loaded, "Field"), "direct level retains exact scope");
  Check(!echoes::NativeWarpSetForCurrentLevel(loaded, "UnknownFieldArea"), "unknown alias refuses");
  auto ambiguous = loaded;
  ambiguous.level_aliases.emplace_back("DekuTreeArea", "L_Dungeon001");
  Check(!echoes::NativeWarpSetForCurrentLevel(ambiguous, "DekuTreeArea"), "duplicate alias refuses");
  ambiguous = loaded;
  ambiguous.level_aliases.emplace_back("Field", "L_Dungeon001");
  Check(!echoes::NativeWarpSetForCurrentLevel(ambiguous, "DekuTreeArea"), "alias chain refuses");
  const auto aliases = File(dir / "region_common/datasheets/ui/UiMapLevelDefine.gsheet");
  std::vector<std::pair<std::string, std::string>> parsed_aliases;
  Check(echoes::ParseNativeWarpLevelAliases(aliases, parsed_aliases), "standalone original alias parser");
  for (size_t member : {size_t(16), size_t(18), size_t(20), size_t(22), size_t(24), size_t(26), size_t(32)}) {
    auto corrupt = aliases;
    corrupt[96 + member] ^= 0x80;
    Check(!echoes::ParseNativeWarpLevelAliases(corrupt, parsed_aliases) && parsed_aliases.empty(),
          "corrupt alias schema refuses and clears");
  }
  auto corrupt = aliases;
  const auto alias_row = size_t(Q(corrupt, 32));
  std::copy_n(corrupt.data() + alias_row, 16, corrupt.data() + alias_row + 32);
  Check(!echoes::ParseNativeWarpLevelAliases(corrupt, parsed_aliases) && parsed_aliases.empty(), "duplicate original alias refuses");
  Check(!echoes::LoadNativeWarpCatalog({}, loaded) && loaded.sets.empty(), "missing asset reader clears output");
  const auto unreadable = [](uint64_t, void *, size_t) { return false; };
  const auto unlocks = echoes::ReadWarpUnlocks(unreadable, 0x80000000, true, {}, *field);
  Check(!unlocks.known && unlocks.unlocked.empty(), "unreadable flags never become unlocked");
  std::cout << checks << " warp metadata/refusal checks passed; live warp input qualification pending\n";
}
