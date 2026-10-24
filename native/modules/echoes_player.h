// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_map.h"
#include "echoes_map_assets.h"
#include <memory>

namespace echoes {
struct PlayerWorldSource {
  MapEpochSource map_epoch{};
  MapLevelSource level{};
  uint64_t holder{}, payload{}, lock_word{};
  bool operator==(const PlayerWorldSource &) const = default;
};
struct PlayerWorldSnapshot {
  bool compatible{}, valid{};
  std::array<float, 3> xyz{};
  std::string level_key;
  PlayerWorldSource source{};
};
// No gameplay/readiness permission is implied. The caller must independently
// bracket gameplay context. Lock words are not sequence numbers: equal copies
// cannot exclude ABA. No native locks, reference counts, guest writes or calls;
// no retained position fallback.
PlayerWorldSnapshot ReadPlayerWorld(const ReadFn &, uint64_t main_base,
                                    bool build_matches,
                                    const MapSnapshot &expected);
class FieldProjection {
public:
  FieldProjection() = default;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  friend bool LoadFieldProjection(const ReadAssetFn &, const FieldMapGeometry &,
                                  FieldProjection &);
  friend struct FieldProjectionAccess;
};
// Asset-worker only. Reads original Common.eclo, UiMapPlayerPos and
// UiMapLevelDefine GSHT resources, with strict bounds; no decoded art, fixture
// coordinates, or extracted alias tables are bundled.
bool LoadFieldProjection(const ReadAssetFn &, const FieldMapGeometry &,
                         FieldProjection &);
// Convert an authenticated native Field map point (not a world position)
// through the original decoded terrain anchor/bounds. Caller owns scope/reveal.
struct FieldNativeProjection { bool valid{}; std::array<float,2> uv{}; };
FieldNativeProjection ProjectFieldNativePoint(const FieldProjection &,
                                               std::array<float,2>);
// Pure authored Field-world position projection. Uses original sole-room bounds,
// centre, scale and terrain registration; no level aliases or live player state.
// Requires original Land/CheckPoint attachments to share the qualified root chain.
// This grants no icon discovery, gameplay, or fog-reveal permission.
FieldNativeProjection ProjectFieldAuthoredWorld(const FieldProjection &,
                                                std::array<float,3>,
                                                std::array<float,2> native_offset = {});
struct FieldPlayerProjection {
  bool valid{}, authored_alias{};
  std::array<float, 2> native_layout{}, uv{};
  PlayerWorldSnapshot world{};
};
// Field raw world, or an exact unambiguous authored type0 alias. Unsupported
// room, special-key or malformed/ambiguous precedence returns empty. Caller
// must also require discovered/visible raster coverage before drawing; this
// function grants no gameplay access.
FieldPlayerProjection ProjectFieldPlayer(const FieldProjection &,
                                         const PlayerWorldSnapshot &);
} // namespace echoes
