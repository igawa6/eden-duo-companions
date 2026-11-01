// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_map_assets.h"
#include <algorithm>
namespace ot0 {
namespace {
struct MapTexturePin {
  PakMember member;
  TexturePin texture;
};
#include "ot0_map_pins.inc"
} // namespace
std::size_t MapTextureCount() { return std::size(MapTexturePins); }
std::optional<Image> LoadMapTexture(const RangeReader &read,
                                    std::string_view path, std::string *why) {
  constexpr std::string_view prefix = "/Game/";
  if (!path.starts_with(prefix) || path.size() > 1024 ||
      path.find("..") != path.npos) {
    if (why)
      *why = "Invalid native map texture path";
    return std::nullopt;
  }
  path.remove_prefix(prefix.size());
  const auto dot = path.find('.');
  if (dot == path.npos ||
      path.substr(dot + 1) !=
          path.substr(path.rfind('/') + 1, dot - path.rfind('/') - 1)) {
    if (why)
      *why = "Invalid map texture object identity";
    return std::nullopt;
  }
  const auto member = std::string(path.substr(0, dot)) + ".uexp";
  const auto found = std::lower_bound(
      std::begin(MapTexturePins), std::end(MapTexturePins), member,
      [](const MapTexturePin &p, const std::string &key) {
        return p.member.path < key;
      });
  if (found == std::end(MapTexturePins) || found->member.path != member) {
    if (why)
      *why = "Current map texture is not supported by this archive";
    return std::nullopt;
  }
  const auto bytes = ReadMember(read, found->member, why);
  return bytes ? DecodeTexture(*bytes, found->texture, why) : std::nullopt;
}
} // namespace ot0
