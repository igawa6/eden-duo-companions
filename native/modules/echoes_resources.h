// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_catalog.h"
#include "echoes_fonts.h"
#include "echoes_inventory_catalog.h"
#include "echoes_story.h"
#include "echoes_player.h"
#include "echoes_map_annotations.h"
#include "echoes_map_labels.h"
#include "echoes_landmarks.h"
#include "echoes_entrances.h"
#include "echoes_quest_markers.h"
#include "echoes_prologue.h"
#include "echoes_dungeon.h"
#include "echoes_dungeon_assets.h"
#include "echoes_warp.h"
#include <functional>
#include <memory>
#include <mutex>
#include <set>
namespace echoes {
using ResourceReadAssetFn =
    std::function<size_t(std::string_view, uint64_t, void *, size_t)>;
inline constexpr uint32_t InspectionWidth = 392;
struct InspectionTextLayout {
  std::vector<std::string> heading_lines;
  bool use_body{};
  uint32_t title_h{}, description_h{}, title_right{}, title_bottom{};
};
// Native prose is authored for a wider menu. Reflow soft line breaks in the
// inspection column while retaining explicit paragraph boundaries and words.
std::string InspectionDescription(std::string_view text);
struct ReadyResources {
  uint32_t locale_flag{};
  std::string message_root;
  std::vector<EchoMetadata> echoes;
  // Optional original UiMachineSelect metadata; ownership is sampled separately.
  std::vector<EchoMetadata> automata;
  bool inventory_ready{};
  InventoryCatalog inventory;
  StoryQuery field_story;
  bool player_ready{};
  FieldProjection player;
  std::vector<NativeLocation> locations;
  std::vector<NativeRiftRegion> rift_regions;
  std::vector<NativeMapLabel> map_labels;
  // Optional original marker metadata, prepared only on the asset worker.
  // Catalog membership grants no visibility; sampling requires epoch-bound readers.
  std::vector<NativeQuestMarkerQuest> quest_catalog;
  std::vector<NativeCheckpointGroup> checkpoints;
  std::vector<NativeLandmark> landmarks;
  NativeEntranceCatalog entrances;
  std::vector<PrologueItem> prologue;
  std::vector<DungeonMapDefinition> dungeons;
  std::unordered_map<std::string,DungeonMapArt> dungeon_art;
  NativeWarpCatalog warps;
  // Optional localized native Map.msbt floor labels, prepared on the worker.
  std::unordered_map<std::string, std::string> dungeon_floor_labels;
  std::unordered_map<std::string, std::string> ui;
  FontAtlas body;
  // Optional native cap29 atlas for original authored map-label positions.
  FontAtlas map_body;
  std::vector<std::array<int,2>> map_label_sizes;
  // Optional native body/pictogram atlas. Display strings retain native words
  // and use only internal aliases for exact original controller glyphs.
  FontAtlas prologue_body;
  std::vector<std::string> prologue_description_display;
  // Optional inspection typography, prepared on the asset worker. Missing
  // entries do not withdraw the independently required Echo picker resources.
  FontAtlas hero_body;
  FontAtlas echo_name_body;
  std::unordered_map<uint32_t, InspectionTextLayout> inspection_echo_text;
  std::unordered_map<uint32_t, InspectionTextLayout> inspection_automaton_text;
  std::unordered_map<uint32_t, InspectionTextLayout> inventory_text;
  std::unordered_map<uint32_t, InspectionTextLayout> prologue_text;
  HeadingFont heading;
  // Exact glyph coverage, without substitutions. A caller must suppress native
  // Heading rendering for a key absent here; body coverage is always complete.
  std::set<uint32_t> heading_echo_keys;
  std::set<uint32_t> heading_automaton_keys;
  std::set<std::string> heading_ui_keys;
  // Catalog indices whose complete localized name has native Heading glyphs.
  std::set<size_t> heading_inventory_indices;
};
class Resources {
public:
  // Null destination with size zero queries file size; other calls must return
  // the exact requested byte count. Assets remain private, caller-owned inputs.
  // Optional guest reader/base are fixed for this resource instance. Native
  // inventory metadata has independent code guards and is built on the worker.
  explicit Resources(ResourceReadAssetFn read, ReadFn read_memory = {},
                     uint64_t main_base = 0);
  // Sampling-thread safe and cheap: no IO, validation, or rasterization here.
  // A changed or invalid input immediately withdraws the published snapshot.
  void SetNativeFonts(std::shared_ptr<const NativeFonts> fonts);
  // Asset-worker operation. Echo/font failures publish nothing and can retry.
  // Optional inventory failure preserves Echo resources for this generation;
  // locale/session rebinding invalidates both catalogs together.
  bool EnsureReady();
  std::shared_ptr<const ReadyResources> Snapshot() const;
  // Read-only diagnostics; no assets, guest addresses or font bytes are exposed.
  std::string LastBuildPhase() const;
  uint64_t BuildAttempts() const;

private:
  ResourceReadAssetFn read_;
  const ReadFn read_memory_;
  const uint64_t main_base_;
  mutable std::mutex mutex_;
  std::mutex build_mutex_;
  uint64_t generation_{};
  uint64_t build_attempts_{};
  std::string build_phase_{"not-attempted"};
  std::shared_ptr<const NativeFonts> fonts_;
  std::shared_ptr<const ReadyResources> ready_;
};
} // namespace echoes
