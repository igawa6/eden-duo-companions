// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_data.h"
#include "ot0_map_assets.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
int main() {
  unsigned calls = 0;
  const ot0::RangeReader deny = [&](auto, void *, auto) {
    ++calls;
    return false;
  };
  for (const char *path : {"", "../Path", "/Game/UI/../Path.Path",
                           "/Game/UI/Path.Wrong", "/Game/UI/Absent.Absent"})
    assert(!ot0::LoadMapTexture(deny, path));
  assert(calls == 0);
  assert(ot0::MapTextureCount() == 780);
  assert(!ot0::DecodeMapNames({}).valid);
  const char *source = std::getenv("OT0_PAK");
  if (!source) {
    std::cout << "Map asset boundary checks passed; native assets skipped\n";
    return 0;
  }
  std::ifstream pak(source, std::ios::binary);
  assert(pak);
  const ot0::RangeReader read = [&](ot0::u64 offset, void *out, std::size_t n) {
    pak.clear();
    pak.seekg(offset);
    pak.read(static_cast<char *>(out), n);
    return std::size_t(pak.gcount()) == n;
  };
  for (const std::string suffix : {"", "_Room", "_Room2", "_Room3"}) {
    const std::string volume = suffix.empty()      ? "MinimapBoundsVolume"
                               : suffix == "_Room" ? "Room_MinimapBoundsVolume"
                               : suffix == "_Room2"
                                   ? "Room_MinimapBoundsVolume2"
                                   : "Room_MinimapBoundsVolume3";
    const auto name = "Map_DP_Plain_N0_1001_A1_" + volume;
    const auto path =
        "/Game/UI/MiniMap/Texture/MAP/Plain/Map_DP_Plain_N0_1001_A1/" + name +
        "." + name;
    std::string why;
    const auto image = ot0::LoadMapTexture(read, path, &why);
    if (!image)
      std::cerr << why << '\n';
    assert(image && image->width == 1024 && image->height == 1024);
  }
  auto bytes = ot0::ReadMember(read, ot0::Member(ot0::MemberId::TextMap));
  assert(bytes);
  const auto names = ot0::DecodeMapNames(*bytes);
  assert(names.valid);
  assert(names.LookupName(3282) == "Amphitheatre: Rehearsal Room");
  assert(names.LookupName(0).empty() && names.LookupName(0xffffffff).empty());
  (*bytes)[0] = 1;
  assert(!ot0::DecodeMapNames(*bytes).valid);
  std::cout << "Four native room textures and native area name verified\n";
}
