// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_map_annotations.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
namespace {
size_t checks{};
void Check(bool value, const char *reason) {
  ++checks;
  if (!value) {
    std::cerr << "FAIL " << checks << ": " << reason << '\n';
    std::abort();
  }
}
std::vector<uint8_t> File(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
uint64_t U64(const std::vector<uint8_t> &b, size_t p) {
  uint64_t v{};
  for (size_t i = 0; i < 8; ++i)
    v |= uint64_t(b[p + i]) << (i * 8);
  return v;
}
void Put(std::vector<uint8_t> &b, size_t p, uint64_t v, size_t n) {
  for (size_t i = 0; i < n; ++i)
    b[p + i] = uint8_t(v >> (i * 8));
}
} // namespace
int main() {
  const char *env = std::getenv("ECHOES_ROMFS");
  if (!env) {
    std::cout << "Original RomFS unavailable\n";
    return 77;
  }
  const std::filesystem::path root(env);
  auto locations =
      File(root / "region_common/datasheets/ui/UiLocationInfo.gsheet");
  auto rifts = File(root / "region_common/datasheets/CrackRegion.gsheet");
  std::vector<echoes::NativeLocation> names;
  std::vector<echoes::NativeRiftRegion> polygons;
  Check(echoes::ParseNativeLocations(locations, names) && names.size() == 117,
        "all original location rows parse");
  Check(names[0].key == "OP_Dungeon" &&
            names[0].name_label == "glossary/Location:L_Dungeon001",
        "opening dungeon retains exact native label alias");
  Check(echoes::ParseNativeRiftRegions(rifts, polygons) &&
            polygons.size() == 29 && polygons[8].rings.size() == 2 &&
            polygons[8].rings[0].size() == 28 &&
            polygons[8].rings[1].size() == 281,
        "original multipart rift geometry preserves every authored ring");
  const echoes::ReadAssetFn read = [&](std::string_view path, uint64_t off,
                                       void *out, size_t n) {
    const auto b = File(root / std::string(path));
    if (!out && !n)
      return b.size();
    if (off > b.size() || n > b.size() - off)
      return size_t(0);
    std::copy_n(b.data() + off, n, static_cast<uint8_t *>(out));
    return n;
  };
  Check(echoes::LocalizeNativeLocations(read, "regionUS/USen", names),
        "original native location references resolve through owned English "
        "banks");
  Check(!names[0].name.empty() && names[0].name == names[1].name,
        "opening dungeon localization follows native shared label rather than "
        "internal key");
  Check(!echoes::LocalizeNativeLocations(read, "../../other", names) &&
            names[0].name.empty(),
        "invalid locale path withdraws localized names");
  for (int fault = 0; fault < 7; ++fault) {
    auto b = locations;
    const auto row = size_t(U64(b, 32));
    if (fault == 0)
      b[4] = 3;
    if (fault == 1)
      Put(b, 44, 103, 4);
    if (fault == 2)
      Put(b, row + 32, 2, 1);
    if (fault == 3)
      Put(b, row + 8, UINT64_MAX, 8);
    if (fault == 4)
      Put(b, row + 96, 65, 8);
    if (fault == 5)
      Put(b, 48 + 20, 8, 2);
    if (fault == 6)
      b.resize(row + 20);
    Check(!echoes::ParseNativeLocations(b, names) && names.empty(),
          "malformed schema/boolean/string/condition/truncation clears "
          "location output");
  }
  for (int fault = 0; fault < 8; ++fault) {
    auto b = rifts;
    const auto row = size_t(U64(b, 32)), points = size_t(U64(b, row + 8));
    if (fault == 0)
      Put(b, points, 33, 4);
    if (fault == 1)
      Put(b, points + 4, 4097, 4);
    if (fault == 2)
      Put(b, points + 8, 0x7fc00000, 4);
    if (fault == 3)
      Put(b, row + 16, U64(b, row + 16) - 1, 8);
    if (fault == 4)
      Put(b, row + 56, U64(b, row) & 0xffffffff, 4);
    if (fault == 5)
      Put(b, row + 8, UINT64_MAX, 8);
    if (fault == 6)
      Put(b, points + 4, 2, 4);
    if (fault == 7)
      Put(b, points, 0, 4);
    Check(!echoes::ParseNativeRiftRegions(b, polygons) && polygons.empty(),
          "malformed rings/nonfinite coordinates/length/IDs never partially "
          "publish");
  }
  std::cout << checks << " native map annotation checks passed\n";
}
