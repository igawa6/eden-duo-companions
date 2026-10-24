// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_assets.h"
#include "echoes_quick_slots.h"
#include "echoes_helper_mailbox.h"
#include "echoes_quick_slot_bridge.h"
#include "echoes_drive.h"
#include "echoes_direct_select.h"
#include "echoes_direct_sort.h"
#include "echoes_context.h"
#include "echoes_inventory.h"
#include "echoes_menu_drive.h"
#include "echoes_warp_drive.h"
#include "echoes_warp_direct.h"
#include "echoes_requalification.h"
#include "echoes_map.h"
#include "echoes_asset_cache.h"
#include "echoes_map_icon_assets.h"
#include "echoes_dungeon_icons.h"
#include "echoes_map_assets.h"
#include "echoes_dungeon_assets.h"
#include "echoes_dungeon_variants.h"
#include "echoes_dungeon_view.h"
#include "echoes_resources.h"
#include "echoes_stamps.h"
#include "echoes_stamp_bridge.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include <algorithm>
#include <charconv>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace echoes {
namespace {
bool HostBuild(const EdenDsmodHostApi& host) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string build;
  for (const auto byte : host.build_id) {
    build.push_back(digits[byte >> 4]); build.push_back(digits[byte & 15]);
  }
  return host.title_id == TitleId && SupportsBuild(build);
}
ReadFn Reader(const EdenDsmodHostApi& host) {
  return [&host](uint64_t at, void* out, size_t size) {
    return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, out, size);
  };
}
bool SameHold(const LiveSnapshot& a, const LiveSnapshot& b) {
  return a.compatible && b.compatible && a.collection_valid && b.collection_valid &&
      a.selector.selector_valid && b.selector.selector_valid &&
      a.selector.held && b.selector.held && a.collection_source == b.collection_source &&
      a.learned_native_ids == b.learned_native_ids && a.learned_crc_keys == b.learned_crc_keys &&
      a.automatons.ownership_valid == b.automatons.ownership_valid && a.automatons.source == b.automatons.source &&
      a.automatons.owned_native_ids == b.automatons.owned_native_ids &&
      a.selector.source == b.selector.source &&
      a.selector.ordered_native_ids == b.selector.ordered_native_ids;
}
std::string_view AutomatonActor(uint16_t id) {
  constexpr std::array<std::string_view,6> names{"ZolMachine","OctorokMachine","TektightMachine","DekubabaMachine","MoriblinMachine","CrowlyMachine"};
  const auto at=std::find(AutomatonIds.begin(),AutomatonIds.end(),id);
  return at==AutomatonIds.end()?std::string_view{}:names[size_t(at-AutomatonIds.begin())];
}
// Publish key names for a fixed-size per-index widget array (cells, map/dungeon
// slots, map icons, warp targets, ...) are a pure function of the index: the
// index set is bounded once at compile time and never renumbered at runtime, so
// "<prefix><i><suffix>" (or a whole per-index key struct) is identical every
// time it is built for the same i. Sample() runs at 60Hz and republishes every
// key every tick regardless (the host clears the snapshot each tick), so only
// the KEY STRINGS are cached here -- every value below is still recomputed and
// published unconditionally on every call, exactly as before.
const std::string& IndexedKey(std::vector<std::string>& cache,size_t i,std::string_view prefix,std::string_view suffix) {
  if (i>=cache.size()) {
    cache.reserve(std::max(cache.size(),i+1));
    while (cache.size()<=i) cache.push_back(std::string(prefix)+std::to_string(cache.size())+std::string(suffix));
  }
  return cache[i];
}
template<typename Keys,typename Build>
const Keys& IndexedKeys(std::vector<Keys>& cache,size_t i,Build&& build) {
  if (i>=cache.size()) {
    cache.reserve(std::max(cache.size(),i+1));
    while (cache.size()<=i) cache.push_back(build(cache.size()));
  }
  return cache[i];
}
// Shared key shape for the field-map icon arrays (quest/landmark/entrance),
// the dungeon treasure-chest icons and the map place-name labels: all four
// publish exactly {visible,x,y,image} per index under their own prefix.
struct MapIconKeys { std::string visible,x,y,image; };
MapIconKeys BuildMapIconKeys(std::string_view prefix,size_t i) {
  const auto p=std::string(prefix)+std::to_string(i)+".";
  return {p+"visible",p+"x",p+"y",p+"image"};
}
// Per-ordinal warp-target widget keys (echo.warp.<n>.*): reset unconditionally
// every sample regardless of whether the warp panel is even open.
struct WarpKeys { std::string visible,x,y,hit_x,hit_y,token,selected; };
WarpKeys BuildWarpKeys(size_t i) {
  const auto p="echo.warp."+std::to_string(i)+".";
  return {p+"visible",p+"x",p+"y",p+"hit_x",p+"hit_y",p+"token",p+"selected"};
}
// The held-selector grid (echo.cell.<i>.*, up to ~127 cells) republished every
// sample while a hold is in progress.
struct CellKeys { std::string icon,cost,favorite,selected,echo_selected,machine_selected,token; };
CellKeys BuildCellKeys(size_t i) {
  const auto p="echo.cell."+std::to_string(i)+".";
  return {p+"icon",p+"cost",p+"favorite",p+"selected",p+"echo_selected",p+"machine_selected",p+"token"};
}
// The Notebook/Items inspection grid (echo.inspect.cell.<i>.*, up to ~127
// cells) republished every sample while one of those pages is shown.
struct InspectCellKeys {
  std::string token,selected,equipped,echo_selected,machine_selected,echo_equipped,machine_equipped,
              favorite,quantity_src,equipped_icon,cost,icon;
};
InspectCellKeys BuildInspectCellKeys(size_t i) {
  const auto p="echo.inspect.cell."+std::to_string(i)+".";
  return {p+"token",p+"selected",p+"equipped",p+"echo_selected",p+"machine_selected",p+"echo_equipped",
          p+"machine_equipped",p+"favorite",p+"quantity_src",p+"equipped_icon",p+"cost",p+"icon"};
}
// Map-pin stamp keys (echo.stamp.<i>.*, StampCapacity slots), always reset
// every sample whether or not the stamp panel is open.
struct StampKeys { std::string packed,persist_pending,visible,icon,x,y; };
StampKeys BuildStampKeys(size_t i) {
  const auto p="echo.stamp."+std::to_string(i)+".";
  return {p+"packed","echo.stamp.persist."+std::to_string(i)+".pending",p+"visible",p+"icon",p+"x",p+"y"};
}
// Dungeon floor-list rows (echo.dungeon.floor.<i>.*), always reset every
// sample regardless of whether a dungeon is even active.
struct DungeonFloorKeys { std::string visible,selected,image,token; };
DungeonFloorKeys BuildDungeonFloorKeys(size_t i) {
  const auto p="echo.dungeon.floor."+std::to_string(i)+".";
  return {p+"visible",p+"selected",p+"image",p+"token"};
}
// Quick-slot rail (echo.quick.<i>.*, QuickSlots::Count slots), published
// every sample regardless of page.
struct QuickKeys { std::string valid,active,token,icon,cost; };
QuickKeys BuildQuickKeys(size_t i) {
  const auto p="echo.quick."+std::to_string(i)+".";
  return {p+"valid",p+"active",p+"token",p+"icon",p+"cost"};
}
// Map viewport inside the framed panel; keeps the composed map's 1272:916 aspect.
// Top of the inspected name; the detail column ends above the footer band.
constexpr int InspectTop=382;
constexpr int MapX=24,MapY=106,MapW=930,MapH=842;
constexpr int MapRasterW=1272,MapRasterH=1152,MapRasterPadY=118;
// No native Swordfighter-ownership reader exists yet. Its D-pad slot is shown only
// after this many learned Echoes, well past the early Suthorn rift that grants it.
constexpr size_t SwordfighterLearnedProxy=10;
struct Module {
  EdenDsmodHostApi host;
  Resources resources;
  Assets assets;
  QuickSlots quick_slots;
  // Guest helper (load plan): sets Y through the native setter in-game.
  helper::HelperMailbox helper;
  QuickSlotFlagBridge quick_bridge;
  QuickSlotFlagBridge::Restored quick_restored;
  bool quick_restored_applied{};
  uint64_t quick_owner{},quick_save{};
  MapIconAssets map_icons;
  SelectorDrive drive;
  MenuDrive menu_drive;
  MenuPulse menu_pulse{MenuPulse::None};
  uint64_t menu_until{};
  WarpTravelDrive warp_drive;
  WarpPulse warp_pulse{WarpPulse::None};
  uint64_t warp_until{},warp_started{},warp_last_tick{},warp_epoch{1};
  bool warp_open{},warp_opening{},warp_closing{},warp_launcher_ready{};
  int32_t warp_selected{-1};
  int warp_action_reason{};
  bool warp_failure_recorded{},warp_failure_context_valid{},warp_failure_menu_valid{};
  int warp_failure_context_kind{},warp_failure_menu_state{};
  struct WarpSelection { uint64_t token{},tick{};MapEpochSource source{};NativeWarpIcon icon{}; };
  std::optional<WarpSelection> warp_selection;
  MapSnapshot current_map,warp_map;
  MapEpochSource warp_source{};
  CurrentLocationSnapshot warp_location;
  std::string warp_initial_level;
  WarpMenuSnapshot warp_menu;
  WarpUnlockSnapshot warp_unlocks;
  WarpChoiceSnapshot warp_choice;
  FieldPlayerProjection warp_player;
  std::array<bool,64> warp_visible{};

  int intent_result{}, intent_abort{};
  LiveSnapshot direct_source;
  uint16_t direct_target{0xffff};
  uint64_t direct_started{};
  int direct_status{};
  void ObserveDirect(const LiveSnapshot& value,uint64_t tick) {
    if(direct_status!=1)return;
    const auto& s=value.selector;
    std::array<uint8_t,3> state{};
    const bool refreshed=SameHold(direct_source,value) &&
      Reader(host)(s.source.owner+(s.source.mode==0?0x2ed8:0x1e8),state.data(),state.size()) && state[0]==1 && state[2]==0xfe;
    if(refreshed && s.highlighted_native_id==direct_target &&
       s.highlighted_index==s.target_index)direct_status=2;
    else if(!value.collection_valid || value.collection_source!=direct_source.collection_source ||
            (s.selector_valid && !s.held) || tick<direct_started || tick-direct_started>90)direct_status=3;
    if(direct_status!=1 && host.log) {
      char message[192];std::snprintf(message,sizeof(message),"Echoes direct selection: result=%d requested=%u actual=%u index=%d target=%d",direct_status,unsigned(direct_target),unsigned(s.highlighted_native_id),s.highlighted_index,s.target_index);host.log(host.userdata,1,message);
    }
  }
  enum class IntentKind { None, Equip, Favorite, Sort };
  struct Intent {
    IntentKind kind{IntentKind::None};
    LiveSnapshot source;
    uint16_t id{0xffff};
    uint8_t sort{};
    bool favorite{}, owns_hold{};
    int stage{};
    uint64_t started{}, phase{};
  } intent;
  uint64_t x_until{}, y_until{};
  uint64_t diagnostic_tick{}; unsigned diagnostic_count{};
  bool input_hold{}, input_left{}, input_x{}, input_y{};
  bool notebook_automatons{};
  LiveSnapshot observed;
  ContextSnapshot context;
  RupeeSnapshot rupees;
  CheckpointVisibilitySnapshot checkpoints;
  LandmarkVisibilitySnapshot landmarks;
  EntranceVisibilitySnapshot entrances;
  // Save-flag visibility sets (entrances, checkpoints, landmarks, rifts) change
  // only on native progress; they refresh every MapFlagRefreshTicks or at once
  // on a new map source, while the per-sample validation below still runs.
  static constexpr uint64_t MapFlagRefreshTicks=12;
  uint64_t entrance_tick{};bool entrance_attempted{};MapEpochSource entrance_sample_source{};
  uint64_t rift_tick{};bool rift_attempted{};MapEpochSource rift_sample_source{};
  // Widget IDs retain host fallback images while another URI decodes. Never
  // reuse a slot across source changes, discovery rollback or observation gaps.
  // Exhaustion requires a full module reload, which also clears host image caches.
  struct MapSlot {
    MapEpochSource source{};
    std::array<uint8_t,64> discovered{};
    bool used{};
    bool field_extensions_hidden{}, sandstorm_hidden{};
    uint32_t rifts{};
  };
  std::mutex map_mutex, map_compose_mutex;
  std::shared_ptr<FieldMapAssets> map_assets;
  std::array<MapSlot,64> map_slots{};
  // Lazily-grown caches of the (index-only-dependent) publish key strings for
  // the fixed-size per-sample widget arrays below; see IndexedKey/IndexedKeys.
  std::vector<std::string> map_slot_visible_keys_, dungeon_slot_visible_keys_;
  std::vector<MapIconKeys> quest_keys_, landmark_keys_, entrance_keys_, dungeon_icon_keys_, map_label_keys_;
  std::vector<WarpKeys> warp_keys_;
  std::vector<CellKeys> cell_keys_;
  std::vector<InspectCellKeys> inspect_cell_keys_;
  std::vector<StampKeys> stamp_keys_;
  std::vector<DungeonFloorKeys> dungeon_floor_keys_;
  std::vector<QuickKeys> quick_slot_keys_;
  std::shared_ptr<DungeonMapAssets> dungeon_assets;
  using DungeonSlot = DungeonViewSlot;
  DungeonViewCache dungeon_views;
  size_t dungeon_slot{};
  bool dungeon_supported{}, dungeon_valid{}, dungeon_ready{};
  std::string dungeon_key, dungeon_uri;
  int32_t dungeon_floor{}, dungeon_selected{};
  bool dungeon_floor_open{};
  size_t dungeon_floor_menu_slot{};
  DungeonProgressSnapshot dungeon_progress;
  DungeonPlayerRoom dungeon_player_room;
  DungeonIconsSnapshot dungeon_icons;
  std::array<std::optional<MapBounds>,64> dungeon_bounds;
  DungeonVariantsSnapshot dungeon_variants;
  size_t map_slots_used{};
  size_t map_slot{};
  bool map_continuity{}, map_ready{};
  std::string map_uri;
  FieldStorySnapshot field_story;
  FieldPlayerProjection player;
  CurrentLocationSnapshot location;
  RiftVisibilitySnapshot rifts;
  PrologueScopeSnapshot prologue;
  SwordfighterSnapshot swordfighter;
  bool swordfighter_display_active{};
  unsigned swordfighter_missing_samples{};
  bool prologue_ready{},prologue_map_valid{},prologue_level_valid{};
  // Display-only hold: on Thor one sample in a few seconds misses the opening
  // scope, which flashed the waiting logo. Taps still need prologue_ready.
  static constexpr int PrologueHoldSamples=30;
  bool prologue_display{};int prologue_hold{};PrologueScopeSnapshot prologue_shown{};
  size_t prologue_selected{1};
  StampStore stamps;
  StampFlagBridge stamp_bridge;
  bool stamps_loaded{},stamp_persist_any{};
  uint8_t stamp_kind{};
  struct StampTap {int64_t point{};uint64_t tick{};MapEpochSource source{};};
  std::optional<StampTap> stamp_tap;
  struct MapCoverage {
    std::string uri;
    int width{},height{};
    std::vector<uint8_t> alpha;
  };
  std::shared_ptr<const MapCoverage> map_coverage;

  InventorySnapshot inventory, previous_inventory;
  bool previous_inventory_valid{}, home_ready{}, home_page_initialized{};
  // home_ready is the fresh gate every action checks; home_shown keeps the page
  // on screen through short context/inventory read misses (no waiting flash).
  static constexpr int HomeHoldSamples=45;
  bool home_shown{};int home_hold{};
  int home_page{1};
  uint64_t home_epoch{1};
  std::array<uint16_t,5> inspected{{0xffff,0xffff,0xffff,0xffff,0xffff}};
  // Dimmed triangles per PasteCostReduction group; -1 = not verified yet.
  std::array<int,7> cost_reduced{{0,-1,-1,-1,-1,-1,-1}};
  std::shared_ptr<const NativeFonts> fonts;
  // Asset-worker owned; no decoded game art is shipped in the package.
  std::vector<uint8_t> echo_textures, item_textures, ingredient_textures;
  uint64_t epoch{1}, font_epoch{}, font_attempt{}, cached_base{};
  bool font_attempted{}, ui_held{}, pulse_active{}, resource_ready_seen{};
  uint64_t pulse_at{};
  explicit Module(const EdenDsmodHostApi& api)
      : host(api), resources(AssetReader(),Reader(host),host.main_base), assets(AssetReader()), map_icons(AssetReader()) {}
  FieldArchiveCache field_archive_cache;
  ReadAssetFn AssetReader() {
    return [this](std::string_view path, uint64_t offset, void* out, size_t size) {
      const ReadAssetFn raw=[this](std::string_view source,uint64_t at,void* data,size_t length){const std::string name(source);return host.read_romfs(host.userdata,name.c_str(),at,data,length);};
      return field_archive_cache.Read(raw,path,offset,out,size);
    };
  }
  // Every sample republishes every key (the host clears the snapshot each
  // tick), so I()/T() run hundreds of times per Sample. Most call sites pass
  // either a literal key or an already-built std::string (e.g. a cached
  // per-index prefix, or "prefix+suffix"); both are already null-terminated,
  // so these overloads forward the existing buffer instead of copying it into
  // a fresh std::string just to get a c_str(). The std::string_view overloads
  // remain only as a fallback for a genuine non-owning view.
  // The host rebuilds its point snapshot every sample. The inspection pages
  // record what they publish so a held read miss can replay the last list.
  // Entries are overwritten in place (sizes counted separately) so a steady
  // page reuses the same string buffers instead of reallocating every sample.
  bool inspect_recording{};
  std::vector<std::pair<std::string,int64_t>> inspect_ints;
  std::vector<std::pair<std::string,std::string>> inspect_texts;
  size_t inspect_int_count{}, inspect_text_count{};
  template<class V,class T> static void Record(std::vector<std::pair<std::string,V>>& out,size_t& count,const char* key,T&& value) {
    if(count<out.size()){out[count].first.assign(key);out[count].second=std::forward<T>(value);}
    else out.emplace_back(key,std::forward<T>(value));
    ++count;
  }
  void I(const char* key, int64_t value) {
    host.publish_i64(host.userdata, key, value);
    if(inspect_recording)Record(inspect_ints,inspect_int_count,key,value);
  }
  void I(const std::string& key, int64_t value) { I(key.c_str(), value); }
  void I(std::string_view key, int64_t value) { I(std::string(key).c_str(), value); }
  void T(const char* key, const char* value) {
    host.publish_text(host.userdata, key, value);
    if(inspect_recording)Record(inspect_texts,inspect_text_count,key,value);
  }
  void T(const char* key, const std::string& value) { T(key, value.c_str()); }
  void T(const std::string& key, const char* value) { T(key.c_str(), value); }
  void T(const std::string& key, const std::string& value) { T(key.c_str(), value.c_str()); }
  void T(std::string_view key, std::string_view value) { T(std::string(key).c_str(), std::string(value).c_str()); }
  void Bump() { epoch = epoch == ((UINT64_C(1)<<46)-1) ? 1 : epoch+1; }
  LiveSnapshot Read() {
    auto value=ReadLive(Reader(host),host.main_base,HostBuild(host));
    if(!value.collection_valid)return value;
    auto machines=ReadAutomatons(Reader(host),host.main_base,HostBuild(host));
    if(machines.ownership_valid && machines.source.root==value.collection_source.access_object &&
       machines.source.buffer==value.collection_source.save_data) {
      if(machines.selector.held) {
        const auto equipped=value.selector;
        value.selector=machines.selector;
        value.selector.equipped_valid=equipped.equipped_valid;
        value.selector.equipped_native_id=equipped.equipped_native_id;
      }
      value.automatons=std::move(machines);
    }
    return value;
  }
  // Tri-power cost reductions: PasteCostReduction group g (1..6) removes one
  // triangle once bit g+3 of the progress word is set (main+0x25a54d0, table
  // main+0x29483c0). Read-only chain *(*(main+0x34bc840)+8)+0x1974, the path the
  // ActorSelect code uses (main+0x24d81cc); the cost function's sibling slot at
  // main+0x34bc850 holds no object on the live game. Unknown on any failed
  // validation, so no reduction is ever guessed.
  void Diagnose(uint64_t tick,const LiveSnapshot& live) {
    if(!host.log || diagnostic_count>=30 || (diagnostic_count && tick>=diagnostic_tick && tick-diagnostic_tick<600))return;
    diagnostic_tick=tick;++diagnostic_count;
    const auto read=Reader(host);uint32_t code{},context_code{};uint64_t locale{},vt{},save{};
    const bool code_ok=read(host.main_base+0x21cdae0,&code,4);
    read(host.main_base+0x24db2d0,&context_code,4);
    read(host.main_base+0x34aa570,&locale,8);
    if(locale && !(locale&7))read(locale+8,&vt,8);
    read(host.main_base+0x34bc848,&save,8);
    char message[512];
    std::snprintf(message,sizeof(message),"Echoes readiness: base=%llx main_size=%llx relocation=%lld code_read=%u code=%08x context_code=%08x locale=%llx locale_vt=%llx save=%llx fonts=%u ready=%u live=%u/%u context=%u/%u prologue=%u presentation=%u",(unsigned long long)host.main_base,(unsigned long long)host.main_size,(long long)(host.get_i64?host.get_i64(host.userdata,"__relocation_delta",0):0),code_ok,code,context_code,(unsigned long long)locale,(unsigned long long)vt,(unsigned long long)save,unsigned(bool(fonts)),unsigned(bool(resources.Snapshot())),live.compatible,live.collection_valid,context.valid,unsigned(context.context),context.prologue_hud,context.field_presentation);
    host.log(host.userdata,1,message);
    const auto machines=std::string("Echoes automaton state: ")+DescribeAutomatons(read,host.main_base);host.log(host.userdata,1,machines.c_str());
    const auto states=std::string("Echoes context states: ")+DescribeContext(read,host.main_base);host.log(host.userdata,1,states.c_str());
    const auto phase=std::string("Echoes resource phase: ")+resources.LastBuildPhase()+" attempts="+std::to_string(resources.BuildAttempts());host.log(host.userdata,1,phase.c_str());
  }
  void ReadCostReductions() {
    cost_reduced={{0,-1,-1,-1,-1,-1,-1}};
    if (!HostBuild(host)) { I("echoes.cost.flags",-1); return; }
    const auto read=Reader(host);
    uint64_t holder{},object{};uint8_t state{};uint32_t flags{};
    if (!read(host.main_base+0x34bc840,&holder,8) || !holder || (holder&7) ||
        !read(holder+8,&object,8) || !object || (object&7) ||
        !read(object+0x17,&state,1) || (state&0x40) ||
        !read(object+0x1974,&flags,4)) { I("echoes.cost.flags",-1); return; }
    for (int group=1;group<=6;++group) cost_reduced[group]=(flags>>(group+3))&1;
    I("echoes.cost.flags",flags);
  }
  void Fonts(uint64_t tick) {
    const auto read = Reader(host);
    // Check the live locale even after copying the immutable font. No forced
    // English fallback and no borrowed guest font buffer survives this call.
    bool locale_same = false;
    if (fonts && cached_base == host.main_base && HostBuild(host)) {
      uint64_t object{}, vtable{}; uint32_t flag{};
      locale_same = read(host.main_base+0x34aa570,&object,sizeof(object)) &&
          object == fonts->locale_object && read(object+8,&vtable,sizeof(vtable)) &&
          vtable == host.main_base+0x32ae560 && read(object+0x20,&flag,sizeof(flag)) &&
          flag == fonts->locale_flag;
    }
    if (fonts && !locale_same) {
      fonts.reset(); resources.SetNativeFonts({}); ++font_epoch;
      font_attempted = false;
    }
    if (!fonts && (!font_attempted || tick < font_attempt || tick-font_attempt >= 60)) {
      font_attempted = true; font_attempt = tick;
      auto found = std::make_shared<NativeFonts>(ReadNativeFonts(read, host.main_base, HostBuild(host)));
      if (found->standard_font_valid && found->locale_valid && found->body_font_supported) {
        fonts = std::move(found); cached_base = host.main_base;
        resources.SetNativeFonts(fonts); ++font_epoch;
      }
    }
    const bool ready=bool(resources.Snapshot());
    if (ready && !resource_ready_seen) ++font_epoch;
    resource_ready_seen=ready;
    I("__font_epoch", static_cast<int64_t>(font_epoch));
    T("echo.bootstrap","module:echoes/bootstrap/"+std::to_string(font_epoch));
  }
  const EchoMetadata* Metadata(const ReadyResources& ready, const LiveSnapshot& value,
                               uint16_t id) const {
    if(IsAutomaton(id)) {
      if(!value.automatons.ownership_valid || std::find(value.automatons.owned_native_ids.begin(),value.automatons.owned_native_ids.end(),id)==value.automatons.owned_native_ids.end())return nullptr;
      const auto at=std::find_if(ready.automata.begin(),ready.automata.end(),[&](const auto& e){return e.crc_key==ActorKey(AutomatonActor(id));});
      return at==ready.automata.end()?nullptr:&*at;
    }
    const auto at = std::find(value.learned_native_ids.begin(), value.learned_native_ids.end(), id);
    if (at == value.learned_native_ids.end()) return nullptr;
    const auto index = static_cast<size_t>(at-value.learned_native_ids.begin());
    if (index >= value.learned_crc_keys.size()) return nullptr;
    const auto key = value.learned_crc_keys[index];
    const auto entry = std::find_if(ready.echoes.begin(), ready.echoes.end(),
        [key](const auto& item) { return item.crc_key == key; });
    return entry == ready.echoes.end() || entry->name.empty() ? nullptr : &*entry;
  }
  // Native cost triangles for an Echo, or "" while its current reduction is
  // unknown. Group 0 never reduces; other groups need the verified reduction state.
  std::string CostImage(const EchoMetadata& entry,int percent) const {
    if (entry.base_slots<1 || entry.base_slots>6) return "";
    int dim=0;
    // Native rule (main+0x25a54d0): one-slot Echoes never reduce.
    if (entry.reduction_group!=0 && entry.base_slots>=2) {
      if (entry.reduction_group>=cost_reduced.size() || cost_reduced[entry.reduction_group]<0) return "";
      dim=cost_reduced[entry.reduction_group];
    }
    if (dim>=static_cast<int>(entry.base_slots)) return "";
    return "module:echoes/assets/cost/"+std::to_string(entry.base_slots)+"/"+std::to_string(dim)+"/"+
        std::to_string(percent);
  }
  // Direct Y change through the guest helper in normal play; otherwise the
  // native picker route. The id is already a learned, owned Echo/Automaton.
  bool EquipY(uint16_t id,const LiveSnapshot& fresh) {
    if(helper.RequestInFlight())return false;
    if(helper.Healthy() && context.valid && context.context==NativeContext::FieldHud)
      return helper.SubmitEquipY(id);
    return StartEchoIntent(IntentKind::Equip,id,false,fresh,host.get_tick(host.userdata));
  }
  static bool SameInventory(const InventorySnapshot& a,const InventorySnapshot& b) {
    return a.source==b.source && a.acquired_accessories==b.acquired_accessories &&
        a.acquired_outfits==b.acquired_outfits && a.equipped_outfit==b.equipped_outfit &&
        a.accessory_capacity==b.accessory_capacity && a.equipped_accessories==b.equipped_accessories &&
        a.ingredients==b.ingredients && a.coupons==b.coupons;
  }
  uint32_t Quantity(const InventoryMetadata& entry,const InventorySnapshot& items) const {
    if (!items.inventory_valid) return 0;
    if (entry.kind==InventoryKind::Accessory) return bool(items.acquired_accessories&entry.mask);
    if (entry.kind==InventoryKind::Outfit) return bool(items.acquired_outfits&entry.mask);
    if (entry.kind==InventoryKind::Coupon) return items.coupons;
    return entry.quantity_index>=0 && static_cast<size_t>(entry.quantity_index)<items.ingredients.size()
        ?items.ingredients[static_cast<size_t>(entry.quantity_index)]:0;
  }
  bool Equipped(const InventoryMetadata& entry) const {
    if (entry.kind==InventoryKind::Outfit) return inventory.equipped_outfit==entry.mask;
    return entry.kind==InventoryKind::Accessory &&
        std::find(inventory.equipped_accessories.begin(),inventory.equipped_accessories.end(),
                  entry.mask)!=inventory.equipped_accessories.end();
  }
  LiveSnapshot ShortcutSnapshot(const LiveSnapshot& value) const {
    auto joined=value;
    if(value.automatons.ownership_valid) {
      joined.learned_native_ids.insert(joined.learned_native_ids.end(),value.automatons.owned_native_ids.begin(),value.automatons.owned_native_ids.end());
    } else {
      // Unknown machine ownership must not erase a persisted machine shortcut.
      const bool machines=std::any_of(quick_slots.slots().begin(),quick_slots.slots().end(),IsAutomaton) ||
        std::any_of(quick_restored.begin(),quick_restored.end(),[](const auto& id){return id && IsAutomaton(*id);});
      if(machines)joined.collection_valid=false;
    }
    return joined;
  }
  std::vector<uint16_t> InspectionList(const ReadyResources& ready,const LiveSnapshot& value,
                                       const InventorySnapshot& items,int page) const {
    std::vector<uint16_t> ids;
    if (page==3 && value.collection_valid) {
      const auto& owned=notebook_automatons?value.automatons.owned_native_ids:value.learned_native_ids;
      for (const auto id:owned) if (Metadata(ready,value,id)) ids.push_back(id);
    } else if ((page==1 || page==2) && ready.inventory_ready && items.inventory_valid) {
      for (size_t i=0;i<ready.inventory.entries.size();++i) {
        const auto& entry=ready.inventory.entries[i];
        const bool gear=entry.kind==InventoryKind::Accessory || entry.kind==InventoryKind::Outfit;
        if (gear==(page==1) && Quantity(entry,items)) ids.push_back(static_cast<uint16_t>(i));
      }
    }
    return ids;
  }
  static bool Normal(const ContextSnapshot& c) {
    return c.valid && (c.context==NativeContext::NativeMenu || c.context==NativeContext::FieldHud);
  }
  static bool Display(const ContextSnapshot& c) {
    return c.valid && (Normal(c) || c.field_presentation);
  }
  static const MapDiscovery* FieldDiscovery(const MapSnapshot& map,const LiveSnapshot& value) {
    if(!map.discovery_valid || !value.collection_valid || map.source.save_epoch!=value.collection_source ||
       !map.map_count || map.map_count>map.maps.size()) return nullptr;
    const MapDiscovery* field=nullptr;
    for(size_t i=0;i<map.map_count;++i) if(map.maps[i].key=="Field") {
      if(field)return nullptr;
      field=&map.maps[i];
    }
    return field;
  }
  void ObserveMap(const LiveSnapshot& value) {
    MapSnapshot map;
    if((Display(context)&&value.collection_valid) || context.prologue_hud)
      map=ReadMapSnapshot(Reader(host),host.main_base,HostBuild(host));
    prologue_map_valid=map.discovery_valid;prologue_level_valid=map.current_level_valid;
    location={};prologue={};
    if(const auto ready=resources.Snapshot();ready && context.prologue_hud && map.discovery_valid)
      prologue=ReadPrologueScope(Reader(host),host.main_base,HostBuild(host),map,ready->locations);
    if(const auto ready=resources.Snapshot();ready && map.discovery_valid && Display(context)) {
      location=ReadCurrentLocation(Reader(host),host.main_base,HostBuild(host),map,ready->locations);
      if(FieldDiscovery(map,value)) {
        const auto now=host.get_tick(host.userdata);
        if(!rift_attempted || rift_sample_source!=map.source || now<rift_tick || now-rift_tick>=MapFlagRefreshTicks) {
          rift_attempted=true;rift_tick=now;rift_sample_source=map.source;
          rifts=ReadRiftVisibility(Reader(host),host.main_base,HostBuild(host),map.source,ready->rift_regions);
        }
      } else {rifts={};rift_attempted=false;}
    } else {rifts={};rift_attempted=false;}
    field_story={};
    // FieldDiscovery(map,value) below is the same scan the entry condition just
    // ran (map is not reassigned until the `map={}` a few lines down): reuse
    // its result as `first` instead of re-scanning map.maps[] a second time.
    const auto* field_for_story=FieldDiscovery(map,value);
    if(const auto ready=resources.Snapshot();ready && ready->field_story.valid && field_for_story) {
      field_story=ReadFieldStory(Reader(host),host.main_base,HostBuild(host),map.source,ready->field_story);
      const auto after=ReadMapSnapshot(Reader(host),host.main_base,HostBuild(host));
      const auto* first=field_for_story;const auto* final=FieldDiscovery(after,value);
      const auto fresh_context=ReadContext(Reader(host),host.main_base,HostBuild(host));
      if(!first || !final || after.source!=map.source || first->discovered!=final->discovered || !Display(fresh_context)) {
        field_story={};map={};
      }
    }
    player={};
    if(const auto ready=resources.Snapshot();ready && ready->player_ready &&
       context.valid && context.field_presentation && home_page==4 &&
       !value.selector.held && FieldDiscovery(map,value)) {
      const auto world=ReadPlayerWorld(Reader(host),host.main_base,HostBuild(host),map);
      const auto after_context=ReadContext(Reader(host),host.main_base,HostBuild(host));
      if(after_context.valid && after_context.field_presentation)
        player=ProjectFieldPlayer(ready->player,world);
    }
    // Same reuse as above: map is unchanged between the entry condition and
    // `initial_field` a few lines later, so one scan serves both.
    const auto* field_for_checkpoints=FieldDiscovery(map,value);
    if(const auto ready=resources.Snapshot();ready && Display(context) && home_page==4 && !value.selector.held &&
       map.current_level_valid && map.current_level_key=="Field" && field_for_checkpoints) {
      const auto now=host.get_tick(host.userdata);
      if(!entrance_attempted || entrance_sample_source!=map.source || now<entrance_tick || now-entrance_tick>=MapFlagRefreshTicks) {
        entrance_attempted=true;entrance_tick=now;entrance_sample_source=map.source;
        checkpoints=ReadCheckpointVisibility(Reader(host),host.main_base,HostBuild(host),map.source,ready->quest_catalog,ready->checkpoints);
        landmarks=ReadLandmarkVisibility(Reader(host),host.main_base,HostBuild(host),map.source,ready->landmarks);
        entrances=ReadEntranceVisibility(Reader(host),host.main_base,HostBuild(host),map.source,ready->entrances);
      }
      const auto final_map=ReadMapSnapshot(Reader(host),host.main_base,HostBuild(host));
      const auto final_context=ReadContext(Reader(host),host.main_base,HostBuild(host));
      const auto* initial_field=field_for_checkpoints;const auto* final_field=FieldDiscovery(final_map,value);
      if(!Display(final_context) || !initial_field || !final_field || final_map.source!=map.source ||
         final_map.current_level_source!=map.current_level_source || final_map.current_level_key!="Field" ||
         initial_field->discovered!=final_field->discovered){checkpoints={};landmarks={};entrances={};entrance_attempted=false;}
    } else {checkpoints={};landmarks={};entrances={};entrance_attempted=false;}
    current_map=map;
    ObserveDungeon(map,value);
    if(dungeon_supported) {
      // A major dungeon with unavailable progress never falls back to Field.
      AcceptMap({},value);
      stamp_kind=0;stamp_tap.reset();player={};rifts={};rift_attempted=false;
    } else {
      AcceptMap(map,value,field_story.known && !field_story.native_field_extension_visible,
                field_story.sandstorm_known && !field_story.sandstorm_visible);
    }
  }
  static bool SameDungeonVariants(std::span<const DungeonRoomVariant> a,
                                  std::span<const DungeonRoomVariant> b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),
      [](const auto& x,const auto& y){return x.room==y.room && x.known==y.known && x.texture==y.texture;});
  }
  void ObserveDungeon(const MapSnapshot& map,const LiveSnapshot& value) {
    const auto ready=resources.Snapshot();
    const bool previous=dungeon_valid;
    const auto previous_key=dungeon_key;
    const auto previous_source=dungeon_progress.source;
    const auto previous_floor=dungeon_floor;
    dungeon_supported=dungeon_valid=false;dungeon_icons={};dungeon_player_room={};dungeon_progress={};dungeon_variants={};dungeon_key.clear();
    if(ready && map.current_level_valid) {
      const auto def=std::find_if(ready->dungeons.begin(),ready->dungeons.end(),
                                [&](const auto& row){return row.key==map.current_level_key;});
      if(def!=ready->dungeons.end()) {
        dungeon_supported=true;dungeon_key=def->key;
        bool labels=true;
        for(int32_t floor=def->floor_min;floor<def->floor_min+def->floor_count;++floor)
          labels=labels && ready->dungeon_floor_labels.contains(std::string(DungeonFloorMessageKey(*def,floor)));
        if(labels && ready->dungeon_art.contains(def->key) && Display(context) && value.collection_valid && map.discovery_valid &&
           map.source.save_epoch==value.collection_source) {
          const auto world=ReadPlayerWorld(Reader(host),host.main_base,HostBuild(host),map);
          const auto room=ResolveDungeonPlayerRoom(*def,world);
          const auto progress=ReadDungeonProgress(Reader(host),host.main_base,HostBuild(host),map,*def);
          const auto variants=progress.valid?ReadDungeonVariants(Reader(host),host.main_base,HostBuild(host),
            progress.source,ready->dungeon_art.at(def->key)):DungeonVariantsSnapshot{};
          const auto after=ReadMapSnapshot(Reader(host),host.main_base,HostBuild(host));
          const auto final_world=ReadPlayerWorld(Reader(host),host.main_base,HostBuild(host),after);
          const auto final_room=ResolveDungeonPlayerRoom(*def,final_world);
          const auto final_progress=ReadDungeonProgress(Reader(host),host.main_base,HostBuild(host),after,*def);
          const auto final_variants=final_progress.valid?ReadDungeonVariants(Reader(host),host.main_base,HostBuild(host),
            final_progress.source,ready->dungeon_art.at(def->key)):DungeonVariantsSnapshot{};
          const auto final_context=ReadContext(Reader(host),host.main_base,HostBuild(host));
          if(world.valid && room.valid && progress.valid && final_world.valid && final_room.valid &&
             after.discovery_valid && after.current_level_valid && after.source==map.source &&
             after.current_level_source==map.current_level_source && after.current_level_key==def->key &&
             world.source==final_world.source && room.room==final_room.room && room.floor==final_room.floor &&
             progress.source==map.source && final_progress.valid && final_progress.source==progress.source &&
             final_progress.record==progress.record && final_progress.visited_rooms==progress.visited_rooms &&
             final_progress.map_item==progress.map_item && Display(final_context)) {
            dungeon_valid=true;dungeon_progress=progress;dungeon_floor=room.floor;dungeon_player_room=final_room;
            dungeon_icons=ReadDungeonIcons(Reader(host),host.main_base,HostBuild(host),after,*def);
            if(!Display(ReadContext(Reader(host),host.main_base,HostBuild(host))))dungeon_icons={};
            if(variants.known && final_variants.known && variants.source==progress.source &&
               final_variants.source==progress.source && SameDungeonVariants(variants.variants,final_variants.variants))
              dungeon_variants=final_variants;
            if(!previous || previous_key!=def->key || previous_source!=progress.source || previous_floor!=room.floor)
              dungeon_selected=room.floor;
          }
        }
      }
    }
    AcceptDungeon();
  }
  void AcceptDungeon() {
    std::lock_guard lock(map_mutex);
    dungeon_ready=false;dungeon_uri.clear();
    if(!dungeon_valid || !dungeon_assets) {dungeon_views.Withdraw();return;}
    const auto selected=dungeon_views.Accept({dungeon_key,dungeon_selected,dungeon_progress.source,
      dungeon_progress.visited_rooms,dungeon_progress.map_item,dungeon_variants.variants});
    if(!selected)return;
    dungeon_slot=*selected;
    const auto& slot=dungeon_views.slots()[dungeon_slot];
    dungeon_uri="module:echoes/dungeon/"+std::to_string(dungeon_slot)+"/"+
      std::to_string(slot.visited)+"/"+(slot.map_item?"1":"0");
    dungeon_ready=true;
  }

  struct MapView {
    StampImageView image;
    MapEpochSource source;
    std::shared_ptr<const MapCoverage> coverage;
  };
  // The 3 host pan/zoom bind names for a field-map slot ("@view_zoom:echo_field_map_<slot>"
  // etc.) are a pure function of the slot index, which only changes when map
  // continuity breaks (AcceptMap), not every sample. CurrentMapView() and
  // PublishMarker() both need them on every map-page sample, so build each
  // slot's 3 strings once and reuse rather than re-concatenating repeatedly.
  std::vector<std::array<std::string,3>> field_map_view_binds_;
  const std::array<std::string,3>& FieldMapViewBinds(size_t slot) {
    if (slot>=field_map_view_binds_.size()) {
      field_map_view_binds_.reserve(std::max(field_map_view_binds_.size(),slot+1));
      while (field_map_view_binds_.size()<=slot) {
        const auto suffix="echo_field_map_"+std::to_string(field_map_view_binds_.size());
        field_map_view_binds_.push_back({"@view_zoom:"+suffix,"@view_pan_x:"+suffix,"@view_pan_y:"+suffix});
      }
    }
    return field_map_view_binds_[slot];
  }
  std::optional<MapView> CurrentMapView() {
    if(!host.get_f64 || !Normal(context) || !home_ready || home_page!=4 || ui_held)return {};
    std::shared_ptr<const MapCoverage> coverage;MapEpochSource source;size_t slot{};
    {std::lock_guard lock(map_mutex);
      if(!map_ready || !map_coverage || map_coverage->uri!=map_uri)return {};
      coverage=map_coverage;source=map_slots[map_slot].source;slot=map_slot;
    }
    const auto& binds=FieldMapViewBinds(slot);
    const auto read=[&](const std::string& key){return host.get_f64(host.userdata,key.c_str(),std::numeric_limits<double>::quiet_NaN());};
    StampImageView view{MapW,MapH,read(binds[0]),read(binds[1]),read(binds[2])};
    StampPoint point;
    if(!StampViewPoint({.5,.5},view,point) && (!std::isfinite(view.zoom) || !std::isfinite(view.pan_x) || !std::isfinite(view.pan_y) || view.zoom<1 || view.zoom>4))return {};
    return MapView{view,source,coverage};
  }
  static bool MapCovered(const MapView& view,StampUv uv) {
    if(!std::isfinite(uv.u)||!std::isfinite(uv.v)||uv.u<0||uv.u>=1||uv.v<0||uv.v>=1)return false;
    const size_t x=static_cast<size_t>(uv.u*view.coverage->width),y=static_cast<size_t>(uv.v*view.coverage->height);
    return view.coverage->alpha[y*static_cast<size_t>(view.coverage->width)+x]==255;
  }
  void PublishNativeMapIcons(const std::shared_ptr<const ReadyResources>& ready,uint64_t tick) {
    for(size_t i=0;i<32;++i)I(IndexedKeys(quest_keys_,i,[](size_t n){return BuildMapIconKeys("echo.quest.",n);}).visible,0);
    for(size_t i=0;i<6;++i)I(IndexedKeys(landmark_keys_,i,[](size_t n){return BuildMapIconKeys("echo.landmark.",n);}).visible,0);
    for(size_t i=0;i<96;++i)I(IndexedKeys(entrance_keys_,i,[](size_t n){return BuildMapIconKeys("echo.entrance.",n);}).visible,0);
    I("echo.entrance.known",entrances.known);I("echo.entrance.compatible",entrances.compatible);
    I("echo.quest.known",checkpoints.known);I("echo.quest.compatible",checkpoints.compatible);
    I("echo.quest.visible.count",checkpoints.visible.size());I("echo.quest.unsupported",checkpoints.unsupported_markers);
    for(size_t i=0;i<2;++i){I("echo.quest.tracked."+std::to_string(i),checkpoints.tracked_quests[i]);I("echo.quest.display."+std::to_string(i),checkpoints.display[i]);}
    I("echo.landmark.known",landmarks.known);I("echo.landmark.compatible",landmarks.compatible);
    const auto view=CurrentMapView();
    if(!ready || !view || dungeon_supported || warp_open || stamp_kind || ui_held ||
       !current_map.current_level_valid || current_map.current_level_key!="Field")return;
    const auto point=[&](std::array<float,3> world,int pixels,int& x,int& y,std::array<float,2> offset=std::array<float,2>{}) {
      const auto projected=ProjectFieldAuthoredWorld(ready->player,world,offset);if(!projected.valid)return false;
      const StampUv uv{projected.uv[0],(projected.uv[1]*916+MapRasterPadY)/MapRasterH};StampPoint at;
      if(!MapCovered(*view,uv)||!StampViewPoint(uv,view->image,at))return false;
      x=MapX+int(std::lround(at.x))-pixels/2;y=MapY+int(std::lround(at.y))-pixels/2;
      return x>=MapX && y>=MapY && x+pixels<=MapX+MapW && y+pixels<=MapY+MapH;
    };
    if(checkpoints.known && checkpoints.source==view->source) {
      size_t slot=0;
      for(const auto& ref:checkpoints.visible) {
        if(slot>=32 || ref.group>=ready->checkpoints.size())break;
        const auto& group=ready->checkpoints[ref.group];if(ref.marker>=group.markers.size())continue;
        const auto& marker=group.markers[ref.marker];
        if(marker.side_view || marker.auxiliary_byte || marker.auxiliary[0]!=0 || marker.auxiliary[1]!=0)continue;
        int x{},y{};if(!point(marker.world,112,x,y))continue;
        const auto& key=IndexedKeys(quest_keys_,slot++,[](size_t n){return BuildMapIconKeys("echo.quest.",n);});
        I(key.x,x);I(key.y,y);T(key.image,"module:echoes/map-icon/checkpoint/"+std::to_string((tick%700)/5*5));I(key.visible,1);
      }
    }
    if(entrances.known && entrances.source==view->source && entrances.visible.size()==ready->entrances.field.size())
      for(size_t i=0;i<ready->entrances.field.size() && i<96;++i) {
        const auto& field=ready->entrances.field[i];if(!entrances.visible[i] || field.icon_index>=ready->entrances.icons.size())continue;
        const auto& icon=ready->entrances.icons[field.icon_index];
        if(icon.texture_name.empty())continue;
        int x{},y{};if(!point(field.world,28,x,y,icon.offset))continue;
        const auto& key=IndexedKeys(entrance_keys_,i,[](size_t n){return BuildMapIconKeys("echo.entrance.",n);});
        I(key.x,x);I(key.y,y);T(key.image,"module:echoes/map-icon/entrance/"+icon.texture_name);I(key.visible,1);
      }
    if(landmarks.known && landmarks.source==view->source && landmarks.visible.size()==ready->landmarks.size())
      for(size_t i=0;i<ready->landmarks.size() && i<6;++i) {
        const auto& item=ready->landmarks[i];if(!landmarks.visible[i] || item.level_key!="Field")continue;
        int x{},y{};if(!point({item.position[0],0,item.position[1]},28,x,y))continue;
        const auto& key=IndexedKeys(landmark_keys_,i,[](size_t n){return BuildMapIconKeys("echo.landmark.",n);});
        I(key.x,x);I(key.y,y);T(key.image,"module:echoes/map-icon/landmark/"+item.texture_name);I(key.visible,1);
      }
  }
  void PublishDungeonIcons(const std::shared_ptr<const ReadyResources>& ready) {
    for(size_t i=0;i<64;++i)I(IndexedKeys(dungeon_icon_keys_,i,[](size_t n){return BuildMapIconKeys("echo.dungeon.icon.",n);}).visible,0);
    I("echo.dungeon.icons.known",dungeon_icons.valid);
    if(!ready || !home_shown || home_page!=4 || ui_held || !dungeon_ready || !dungeon_icons.valid || !host.get_f64)return;
    std::optional<MapBounds> bounds;size_t slot{};
    {std::lock_guard lock(map_mutex);slot=dungeon_slot;bounds=dungeon_bounds[slot];}
    if(!bounds)return;
    const auto def=std::find_if(ready->dungeons.begin(),ready->dungeons.end(),[&](const auto& row){return row.key==dungeon_key;});
    const auto art=ready->dungeon_art.find(dungeon_key);
    if(def==ready->dungeons.end() || art==ready->dungeon_art.end())return;
    const auto read=[&](const char* field){const auto key=std::string(field)+"echo_dungeon_map_"+std::to_string(slot);return host.get_f64(host.userdata,key.c_str(),std::numeric_limits<double>::quiet_NaN());};
    const StampImageView view{MapW,MapH,read("@view_zoom:"),read("@view_pan_x:"),read("@view_pan_y:")};
    size_t output=0;
    for(const auto& icon:dungeon_icons.icons) {
      if(output>=64)break;
      if(!DungeonIconVisible(icon,dungeon_icons,dungeon_progress,*def,art->second,dungeon_selected))continue;
      const auto uv=ProjectDungeonIcon(icon,*bounds,MapW,MapH);StampPoint point;
      if(!uv || !StampViewPoint({(*uv)[0],(*uv)[1]},view,point))continue;
      const int x=MapX+int(std::lround(point.x))-32,y=MapY+int(std::lround(point.y))-32;
      if(x<MapX || y<MapY || x+64>MapX+MapW || y+64>MapY+MapH)continue;
      const auto& key=IndexedKeys(dungeon_icon_keys_,output++,[](size_t n){return BuildMapIconKeys("echo.dungeon.icon.",n);});
      I(key.x,x);I(key.y,y);T(key.image,icon.sprite==DungeonIconSprite::TreasureBoxOpen?"module:echoes/map-icon/chest/open":"module:echoes/map-icon/chest/closed");I(key.visible,1);
    }
  }
  void StopWarp() {
    const bool owned=warp_open || warp_pulse!=WarpPulse::None || warp_drive.status()==WarpTravelStatus::Pending;
    warp_drive.Cancel();warp_open=warp_opening=warp_closing=warp_launcher_ready=false;
    warp_location={};warp_initial_level.clear();
    warp_selection.reset();warp_selected=-1;warp_menu={};warp_unlocks={};warp_choice={};warp_player={};
    warp_visible.fill(false);warp_pulse=owned?WarpPulse::Neutral:WarpPulse::None;warp_until=owned?warp_last_tick+2:0;
  }
  void WarpPulseFor(WarpPulse pulse,uint64_t tick) {
    warp_pulse=pulse;warp_until=pulse==WarpPulse::None?0:tick+2;
  }
  bool WarpPoint(const ReadyResources& ready,const NativeWarpIcon& icon,const MapView& view,int& x,int& y) {
    const auto projected=ProjectFieldNativePoint(ready.player,{icon.x,icon.y});
    if(!projected.valid)return false;
    const StampUv uv{projected.uv[0],(projected.uv[1]*916+MapRasterPadY)/MapRasterH};
    StampPoint point;
    if(!MapCovered(view,uv)||!StampViewPoint(uv,view.image,point))return false;
    x=MapX+static_cast<int>(std::lround(point.x))-28;y=MapY+static_cast<int>(std::lround(point.y))-28;
    return x-28>=MapX && y-28>=MapY && x+84<=MapX+MapW && y+84<=MapY+MapH;
  }
  void ReadWarpObservation(const ReadyResources& ready) {
    warp_map=ReadMapSnapshot(Reader(host),host.main_base,HostBuild(host));
    if(warp_open && (warp_map.source!=warp_source || warp_map.current_level_key!=warp_initial_level || (warp_location.known && warp_map.current_level_source!=warp_location.level_source))) {warp_location={};warp_initial_level.clear();}
    warp_unlocks={};warp_menu={};warp_choice={};warp_player={};
    const auto* set=NativeWarpSetForCurrentLevel(ready.warps,warp_map.current_level_key);
    if(set && set->key=="Field")warp_unlocks=ReadWarpUnlocks(Reader(host),host.main_base,HostBuild(host),warp_map.source,*set);
    if(context.valid && context.context==NativeContext::NativeMenu) {
      const auto observe=[&]{return ReadWarpMenu(Reader(host),host.main_base,HostBuild(host),ready.warps,true);};
      warp_menu=warp_open?RequalifyFresh(observe,[](const auto& value){return value.compatible && value.valid;}):observe();
      if(warp_menu.valid)warp_choice=ReadWarpChoice(Reader(host),host.main_base,HostBuild(host),warp_menu);
    }
    if(ready.player_ready) {
      const auto world=ReadPlayerWorld(Reader(host),host.main_base,HostBuild(host),warp_map);
      const auto after=ReadContext(Reader(host),host.main_base,HostBuild(host));
      if(after.valid && context.valid && after.context==context.context)warp_player=ProjectFieldPlayer(ready.player,world);
    }
  }
  void ObserveWarp(uint64_t tick) {
    if(tick<warp_last_tick){StopWarp();return;}warp_last_tick=tick;
    if(warp_pulse!=WarpPulse::None && tick>=warp_until)warp_pulse=WarpPulse::None;
    const auto ready=resources.Snapshot();
    const auto* set=ready?NativeWarpSetForCurrentLevel(ready->warps,current_map.current_level_key):nullptr;
    warp_launcher_ready=ready && ready->player_ready && set && set->key=="Field" &&
      context.valid && context.context==NativeContext::FieldHud && home_ready && home_page==4 &&
      !ui_held && !dungeon_supported && bool(CurrentMapView()) && !warp_open &&
      intent.kind==IntentKind::None && menu_drive.status()!=MenuDriveStatus::Pending;
    if(!warp_open)return;
    if(!ready){StopWarp();return;}
    ReadWarpObservation(*ready);
    if(warp_drive.status()==WarpTravelStatus::Pending) {
      const auto pulse=warp_drive.Advance(context,warp_map,warp_unlocks,warp_menu,warp_choice,warp_player,tick,false,[&](const WarpMenuSnapshot& expected,uint32_t ordinal){
        if(!host.write_memory)return false;
        const bool result=DirectWarpTarget(Reader(host),host.main_base,HostBuild(host),ready->warps,expected,ordinal,
          [&](uint64_t at,const void* bytes,size_t n){return host.write_memory(host.userdata,at,bytes,n);});
        if(host.log) {
          const auto at=std::find_if(expected.targets.begin(),expected.targets.end(),[&](const auto& icon){return icon.ordinal==ordinal;});
          const auto f=[&](uint64_t address){float v{};Reader(host)(address,&v,4);return double(v);};
          const auto q=[&](uint64_t address){uint64_t v{};Reader(host)(address,&v,8);return v;};
          const uint64_t icon=at==expected.targets.end()?0:at->component;
          const uint64_t wrapper=icon?q(icon+0x18):0,pane=wrapper && wrapper<UINT64_MAX-0x20?q(wrapper+0x18):0;
          uint8_t absolute{};if(icon)Reader(host)(icon+0xe9,&absolute,1);
          char message[512];std::snprintf(message,sizeof(message),"Echoes warp jump: result=%d ordinal=%u cursor=%.3f,%.3f target=%.3f,%.3f view=%llx pane=%.3f,%.3f absolute=%u scale=%.3f camera=%.3f,%.3f/%.3f,%.3f origin=%.3f,%.3f",result,ordinal,double(expected.cursor_x),double(expected.cursor_y),at==expected.targets.end()?0.:double(at->cursor_x),at==expected.targets.end()?0.:double(at->cursor_y),(unsigned long long)expected.native_view,pane?f(pane+0x30):0.,pane?f(pane+0x34):0.,unsigned(absolute),f(expected.component+0x3d8),f(expected.component+0x428),f(expected.component+0x42c),f(expected.component+0x430),f(expected.component+0x434),f(expected.component+0x420),f(expected.component+0x424));host.log(host.userdata,1,message);
        }
        return result;
      },[&](const WarpMenuSnapshot& expected){
        if(!host.write_memory)return false;
        const bool result=RequestNativeFullZoom(Reader(host),host.main_base,HostBuild(host),ready->warps,expected,
          [&](uint64_t at,const void* bytes,size_t n){return host.write_memory(host.userdata,at,bytes,n);});
        if(host.log){char message[160];std::snprintf(message,sizeof(message),"Echoes warp native zoom: result=%d index=%u previous=%u scale=%.3f",result,unsigned(expected.zoom_index),unsigned(expected.zoom_previous),double(expected.zoom_scale));host.log(host.userdata,1,message);}
        return result;
      });
      if(pulse!=WarpPulse::None)WarpPulseFor(pulse,tick);
      if(warp_drive.status()==WarpTravelStatus::Aborted) {
        warp_failure_recorded=true;warp_failure_context_valid=context.valid;warp_failure_context_kind=static_cast<int>(context.context);
        warp_failure_menu_valid=warp_menu.valid;warp_failure_menu_state=static_cast<int>(warp_menu.state);
      }
      if(warp_drive.status()!=WarpTravelStatus::Pending) {
        if(host.log){char message[240];std::snprintf(message,sizeof(message),"Echoes warp result: status=%d abort=%d preparation=%d menu=%d/%d focus=%d/%u cursor=%.3f,%.3f",int(warp_drive.status()),int(warp_drive.abort_reason()),int(warp_drive.preparation_abort()),warp_menu.valid,int(warp_menu.state),warp_menu.focus_warp,warp_menu.destination_index,double(warp_menu.cursor_x),double(warp_menu.cursor_y));host.log(host.userdata,1,message);}
        warp_open=warp_opening=warp_closing=false;warp_location={};warp_initial_level.clear();warp_selected=-1;WarpPulseFor(WarpPulse::Neutral,tick);
      }
      return;
    }
    if(warp_map.source!=warp_source && !warp_closing){StopWarp();WarpPulseFor(WarpPulse::Neutral,tick);return;}
    if(warp_opening) {
      if(warp_menu.valid && warp_menu.state==WarpMapState::Normal && context.valid && context.context==NativeContext::NativeMenu)warp_opening=false;
      else if(tick-warp_started>180){StopWarp();WarpPulseFor(WarpPulse::Neutral,tick);}
      return;
    }
    if(warp_closing) {
      if(context.valid && context.context==NativeContext::FieldHud){StopWarp();WarpPulseFor(WarpPulse::Neutral,tick);return;}
      if(tick-warp_started>180){StopWarp();WarpPulseFor(WarpPulse::Neutral,tick);return;}
      if(warp_menu.valid && (warp_menu.state==WarpMapState::Normal || warp_menu.state==WarpMapState::ConfirmWarp) && tick>=warp_until+10)WarpPulseFor(WarpPulse::B,tick);
      return;
    }
    if(!context.valid || context.context!=NativeContext::NativeMenu || !warp_menu.valid || warp_menu.map_level!="Field" ||
       (warp_menu.state!=WarpMapState::Normal && warp_menu.state!=WarpMapState::SelectedIcon)) {
      StopWarp();WarpPulseFor(WarpPulse::Neutral,tick);
    }
  }
  void PublishWarp(const std::shared_ptr<const ReadyResources>& ready) {
    I("echo.warp.ready",warp_launcher_ready);I("echo.warp.panel",warp_open);I("echo.warp.closed",warp_launcher_ready && !warp_open);
    const bool busy=warp_drive.status()==WarpTravelStatus::Pending;
    I("echo.warp.failure.recorded",warp_failure_recorded);
    I("echo.warp.failure.context.valid",warp_failure_context_valid);I("echo.warp.failure.context.kind",warp_failure_context_kind);
    I("echo.warp.failure.menu.valid",warp_failure_menu_valid);I("echo.warp.failure.menu.state",warp_failure_menu_state);
    I("echo.warp.action.reason",warp_action_reason);
    I("echo.warp.status",static_cast<int>(warp_drive.status()));I("echo.warp.abort",static_cast<int>(warp_drive.abort_reason()));
    I("echo.warp.prepare.abort",static_cast<int>(warp_drive.preparation_abort()));
    warp_visible.fill(false);
    for(size_t i=0;i<64;++i){const auto& k=IndexedKeys(warp_keys_,i,BuildWarpKeys);I(k.visible,0);I(k.selected,0);I(k.token,0);}
    const auto view=CurrentMapView();
    const auto* set=ready?NativeWarpSetForCurrentLevel(ready->warps,warp_map.current_level_key):nullptr;
    std::vector<std::array<int,2>> used;
    if(ready && view && warp_open && !warp_opening && !warp_closing && !busy && warp_menu.valid && warp_menu.targets_known &&
       warp_unlocks.known && warp_unlocks.source==warp_map.source && view->source==warp_map.source && set && set->key=="Field") {
      for(const auto& icon:warp_menu.targets) {
        const auto n=icon.ordinal;int x{},y{};
        if(n>=64 || n>=set->destinations.size() || n>=warp_unlocks.unlocked.size() || !warp_unlocks.unlocked[n] ||
           set->destinations[n].level!="Field" || !icon.selectable || !icon.native_allowed || !icon.cursor_known || !WarpPoint(*ready,icon,*view,x,y))continue;
        if(std::any_of(used.begin(),used.end(),[&](const auto& p){return std::abs(p[0]-x)<112 && std::abs(p[1]-y)<112;}))continue;
        used.push_back({x,y});warp_visible[n]=true;const auto& k=IndexedKeys(warp_keys_,n,BuildWarpKeys);
        I(k.visible,1);I(k.x,x);I(k.y,y);I(k.hit_x,x-28);I(k.hit_y,y-28);
        I(k.token,static_cast<int64_t>((warp_epoch<<8)|n));I(k.selected,warp_selected==static_cast<int32_t>(n));
      }
    }
    if(warp_selection) {
      const auto request=*warp_selection;warp_selection.reset();const auto tick=host.get_tick(host.userdata);
      const auto n=request.token&255;
      const bool same_icon=std::find(warp_menu.targets.begin(),warp_menu.targets.end(),request.icon)!=warp_menu.targets.end();
      if(tick>=request.tick && tick-request.tick<=2 && (request.token>>8)==warp_epoch && n<64 &&
         warp_visible[n] && view && view->source==request.source && warp_map.source==request.source &&
         !busy && !warp_opening && !warp_closing && warp_menu.valid && warp_menu.state==WarpMapState::Normal && same_icon) {
        warp_selected=static_cast<int32_t>(n);warp_action_reason=0;
      } else warp_action_reason=16;
      I("echo.warp.action.reason",warp_action_reason);
    }
    const bool selected=warp_selected>=0 && warp_selected<64 && warp_visible[warp_selected];
    I("echo.warp.selected",selected);I("echo.warp.confirm.ready",selected && !busy && warp_menu.valid && warp_menu.state==WarpMapState::Normal);
    if(ready){T("echo.warp.confirm.image","module:echoes/control236/"+std::to_string(ready->locale_flag)+"/MapWarp_Dialog_00");T("echo.warp.back.image","module:echoes/control236/"+std::to_string(ready->locale_flag)+"/Operation_Return");}
    constexpr std::array names{"minus","a","b","neutral","west","east","north","south","northwest","northeast","southwest","southeast"};
    constexpr std::array pulses{WarpPulse::Minus,WarpPulse::A,WarpPulse::B,WarpPulse::Neutral,WarpPulse::West,WarpPulse::East,WarpPulse::North,WarpPulse::South,WarpPulse::NorthWest,WarpPulse::NorthEast,WarpPulse::SouthWest,WarpPulse::SouthEast};
    for(size_t i=0;i<names.size();++i)I(std::string("echo.warp.input.")+names[i],warp_pulse==pulses[i]);
    I("echo.warp.input.any",warp_pulse!=WarpPulse::None);
  }
  bool WarpAction(std::string_view name,int64_t argument) {
    const auto refuse=[&](int reason){warp_action_reason=reason;I("echo.warp.action.reason",reason);return false;};
    warp_action_reason=0;
    if(!home_ready || ui_held || home_page!=4)return refuse(1);
    const auto ready=resources.Snapshot();if(!ready)return refuse(2);
    const auto tick=host.get_tick(host.userdata);
    const auto fresh=ReadContext(Reader(host),host.main_base,HostBuild(host));
    if(!fresh.valid || !context.valid || fresh.context!=context.context)return refuse(3);
    if(name=="warp_open") {
      if(!warp_launcher_ready || warp_open || fresh.context!=NativeContext::FieldHud)return refuse(99);
      ReadWarpObservation(*ready);const auto* set=NativeWarpSetForCurrentLevel(ready->warps,warp_map.current_level_key);
      if(!set || set->key!="Field" || !warp_unlocks.known || warp_unlocks.source!=warp_map.source)return refuse(99);
      warp_location={};warp_initial_level.clear();
      if(location.known && location.source==warp_map.source && location.level_source==warp_map.current_level_source) {
        warp_location=location;warp_initial_level=warp_map.current_level_key;
      }
      warp_failure_recorded=false;warp_failure_context_valid=warp_failure_menu_valid=false;warp_failure_context_kind=warp_failure_menu_state=0;
      warp_source=warp_map.source;warp_open=warp_opening=true;warp_closing=false;warp_started=tick;warp_selected=-1;stamp_kind=0;
      warp_epoch=warp_epoch>0x7fffffffffffULL?1:warp_epoch+1;WarpPulseFor(WarpPulse::Minus,tick);return true;
    }
    if(name=="warp_close") {
      if(!warp_open)return refuse(99);
      warp_drive.Cancel();warp_closing=true;warp_opening=false;warp_selected=-1;warp_started=tick;WarpPulseFor(WarpPulse::Neutral,tick);return true;
    }
    if(!warp_open || warp_opening || warp_closing || warp_drive.status()==WarpTravelStatus::Pending || fresh.context!=NativeContext::NativeMenu)return refuse(4);
    ReadWarpObservation(*ready);
    if(!warp_menu.valid)return refuse(5);
    if(warp_menu.state!=WarpMapState::Normal)return refuse(6);
    if(!warp_unlocks.known)return refuse(7);
    if(warp_unlocks.source!=warp_map.source)return refuse(8);
    if(name=="warp_select") {
      const auto token=static_cast<uint64_t>(argument);const auto n=token&255;
      if((token>>8)!=warp_epoch)return refuse(9);
      if(n>=64 || !warp_visible[n])return refuse(10);
      // Host pan/zoom values exist only during sample publication. Queue the
      // exact native identity; PublishWarp validates its fresh revealed hit
      // region on the next sample before making it selected.
      const auto* set=NativeWarpSetForCurrentLevel(ready->warps,warp_map.current_level_key);
      if(!set || set->key!="Field" || n>=set->destinations.size() || n>=warp_unlocks.unlocked.size() || !warp_unlocks.unlocked[n] || set->destinations[n].level!="Field")return refuse(13);
      for(const auto& icon:warp_menu.targets)if(icon.ordinal==n && icon.selectable && icon.native_allowed && icon.cursor_known){warp_selection=WarpSelection{token,tick,warp_map.source,icon};return true;}
      return refuse(15);
    }
    if(name=="warp_travel" && warp_selected>=0 && warp_selected<64 && warp_visible[warp_selected])
      return warp_drive.Request(ready->warps,"Field",static_cast<uint32_t>(warp_selected),fresh,warp_map,warp_unlocks,warp_menu,tick);
    return refuse(99);
  }
  // view: the caller's single CurrentMapView() for this sample (see Publish());
  // avoids re-locking map_mutex and re-reading the host pan/zoom binds here.
  void PublishMapLabels(const std::shared_ptr<const ReadyResources>& ready,const std::optional<MapView>& view) {
    for(size_t i=0;i<26;++i)I(IndexedKeys(map_label_keys_,i,[](size_t n){return BuildMapIconKeys("echo.map.label.",n);}).visible,0);
    if(!ready || !view || ready->map_labels.size()!=ready->map_label_sizes.size())return;
    std::array<uint8_t,64> bits{};
    {std::lock_guard lock(map_mutex);if(map_slots[map_slot].source!=view->source)return;bits=map_slots[map_slot].discovered;}
    std::vector<std::array<int,4>> boxes;
    for(size_t i=0;i<ready->map_labels.size();++i) {
      const auto& label=ready->map_labels[i];const auto native_size=ready->map_label_sizes[i];const std::array size{int(std::ceil(native_size[0]*.75)),int(std::ceil(native_size[1]*.75))};
      if(!size[0] || !size[1] || !NativeMapLabelDiscovered(label,bits))continue;
      StampPoint point;const StampUv uv{label.u,(label.v*916+MapRasterPadY)/MapRasterH};
      if(!StampViewPoint(uv,view->image,point))continue;
      const int cx=static_cast<int>(std::lround(point.x)),cy=static_cast<int>(std::lround(point.y));
      const std::array box{cx-size[0]/2,cy-size[1]/2,cx+(size[0]+1)/2,cy+(size[1]+1)/2};
      if(box[0]<0 || box[1]<0 || box[2]>=MapW || box[3]>=MapH)continue;
      if(std::any_of(boxes.begin(),boxes.end(),[&](const auto& b){return box[0]-4<b[2] && box[2]+4>b[0] && box[1]-4<b[3] && box[3]+4>b[1];}))continue;
      // Every texel beneath the name's bounds must already be revealed; text
      // cannot bridge story/weather fog or reveal a name over covered terrain.
      const double z=view->image.zoom;
      const double px=std::clamp(view->image.pan_x,0.,MapW*(1.-1./z));
      const double py=std::clamp(view->image.pan_y,0.,MapH*(1.-1./z));
      const int x0=static_cast<int>((px+box[0]/z)*view->coverage->width/MapW);
      const int x1=static_cast<int>(std::ceil((px+box[2]/z)*view->coverage->width/MapW));
      const int y0=static_cast<int>((py+box[1]/z)*view->coverage->height/MapH);
      const int y1=static_cast<int>(std::ceil((py+box[3]/z)*view->coverage->height/MapH));
      bool revealed=x0>=0 && y0>=0 && x1<=view->coverage->width && y1<=view->coverage->height;
      for(int y=y0;revealed && y<y1;++y)for(int x=x0;x<x1;++x)
        if(view->coverage->alpha[size_t(y)*view->coverage->width+x]!=255){revealed=false;break;}
      if(!revealed)continue;
      boxes.push_back(box);
      const auto& key=IndexedKeys(map_label_keys_,i,[](size_t n){return BuildMapIconKeys("echo.map.label.",n);});
      I(key.visible,1);I(key.x,MapX+cx-172);I(key.y,MapY+cy-24);
      T(key.image,"module:echoes/map-label/"+std::to_string(ready->locale_flag)+"/"+std::to_string(i));
    }
  }
  // view: the caller's single CurrentMapView() for this sample (see Publish()).
  void PublishStamps(const std::optional<MapView>& view) {
    stamps_loaded=stamp_bridge.AdvanceRestore(stamps);
    I("echo.stamp.restore.pending",stamp_bridge.restore_pending());
    I("echo.stamp.restore.slot",static_cast<int64_t>(stamp_bridge.restore_slot()));
    if(stamp_tap) {
      const auto request=*stamp_tap;stamp_tap.reset();
      const auto now=host.get_tick(host.userdata);StampUv uv;
      if(view && stamps_loaded && stamp_kind && now>=request.tick && now-request.tick<=2 && view->source==request.source && StampTapUv(request.point,view->image,uv) && MapCovered(*view,uv)) {
        uv.v=(uv.v*MapRasterH-MapRasterPadY)/916;
        if(std::isfinite(uv.v) && uv.v>=0 && uv.v<1) {
          const double radius=38.*MapRasterW/(MapW*view->image.zoom);
          if(!stamps.RemoveNear(uv,radius))stamps.Add(static_cast<StampKind>(stamp_kind),uv);
        }
      }
    }
    if(stamps_loaded)stamp_bridge.QueueWrites(stamps);
    stamp_persist_any=stamp_bridge.writes_pending();
    for(size_t i=0;i<StampCapacity;++i) {
      const auto& key=IndexedKeys(stamp_keys_,i,BuildStampKeys);
      I(key.packed,stamp_bridge.write_value(i));
      I(key.persist_pending,stamp_bridge.write_pending(i));
      I(key.visible,0);T(key.icon,"");
    }
    I("echo.stamp.enforce.any",stamp_bridge.restore_pending() || stamp_persist_any);
    I("echo.stamp.ready",stamps_loaded && bool(view) && !warp_open);I("echo.stamp.panel",stamps_loaded && bool(view) && stamp_kind!=0 && !warp_open);
    I("echo.stamp.count",static_cast<int64_t>(stamps.count()));I("echo.stamp.kind",stamp_kind);I("echo.stamp.persist.any",stamp_persist_any);
    if(!view || !stamps_loaded || warp_open)return;
    for(size_t i=0;i<StampCapacity;++i)if(stamps.records()[i].present()) {
      auto uv=stamps.records()[i].uv();uv.v=(uv.v*916+MapRasterPadY)/MapRasterH;
      StampPoint at;if(!MapCovered(*view,uv) || !StampViewPoint(uv,view->image,at))continue;
      const auto x=std::lround(at.x)+MapX-30,y=std::lround(at.y)+MapY-30;
      if(x<MapX||y<MapY||x+60>MapX+MapW||y+60>MapY+MapH)continue;
      const auto& key=IndexedKeys(stamp_keys_,i,BuildStampKeys);
      I(key.x,x);I(key.y,y);I(key.visible,1);
      T(key.icon,"module:echoes/assets/pin/"+std::to_string(static_cast<int>(stamps.records()[i].kind)));
    }
  }
  void PublishMarker(const LiveSnapshot& value) {
    I("echo.map.marker.visible",0);I("echo.map.marker.x",0);I("echo.map.marker.y",0);
    I("echo.map.player.valid",player.valid);
    T("echo.map.player.u",player.valid?std::to_string(player.uv[0]):"");
    T("echo.map.player.v",player.valid?std::to_string(player.uv[1]):"");
    if(!player.valid || !home_shown || home_page!=4 || ui_held || value.selector.held || !host.get_f64)return;
    std::shared_ptr<const MapCoverage> coverage;size_t slot{};
    {
      std::lock_guard lock(map_mutex);
      if(!map_ready || !map_coverage || map_coverage->uri!=map_uri ||
         player.world.source.map_epoch!=map_slots[map_slot].source)return;
      coverage=map_coverage;slot=map_slot;
    }
    const double u=player.uv[0],v=(player.uv[1]*916+MapRasterPadY)/MapRasterH;
    if(!std::isfinite(u)||!std::isfinite(v)||u<0||u>=1||v<0||v>=1)return;
    const auto px=static_cast<size_t>(u*coverage->width),py=static_cast<size_t>(v*coverage->height);
    if(coverage->alpha[py*static_cast<size_t>(coverage->width)+px]!=255)return;
    const auto& binds=FieldMapViewBinds(slot);
    const double missing=std::numeric_limits<double>::quiet_NaN();
    const double zoom=host.get_f64(host.userdata,binds[0].c_str(),missing),
                 pan_x=host.get_f64(host.userdata,binds[1].c_str(),missing),
                 pan_y=host.get_f64(host.userdata,binds[2].c_str(),missing);
    if(!std::isfinite(zoom)||!std::isfinite(pan_x)||!std::isfinite(pan_y)||zoom<1||zoom>4)return;
    // Match the host's float UV window and integer source-texel crop. Keeping
    // continuous coordinates here would drift a few pixels at high zoom.
    const auto coordinate=[](double uv,double pan,float extent,int pixels,double scale) {
      const float z=static_cast<float>(scale);
      const float origin=static_cast<float>(std::clamp(pan,0.0,double(extent)*(1-1/scale)))/extent;
      const float end=origin+1.0f/z;
      const int start=static_cast<int>(origin*static_cast<float>(pixels));
      const int span=std::max(1,static_cast<int>((end-origin)*static_cast<float>(pixels)));
      return (uv*pixels-start)*extent/span;
    };
    const double x=MapX+coordinate(u,pan_x,MapW,coverage->width,zoom)-32;
    const double y=MapY+coordinate(v,pan_y,MapH,coverage->height,zoom)-32;
    if(x<MapX||y<MapY||x+64>MapX+MapW||y+64>MapY+MapH)return;
    I("echo.map.marker.x",std::lround(x));I("echo.map.marker.y",std::lround(y));I("echo.map.marker.visible",1);
  }
  // Bookkeeping only: source reads happen in ObserveMap, raster/IO in LoadImage.
  void AcceptMap(const MapSnapshot& map,const LiveSnapshot& value,bool extensions_hidden=false,bool sandstorm_hidden=false) {
    // A held page keeps its last map through top-screen events: clearing it
    // would blank the image and spend a fresh slot when the HUD returns.
    if(!Display(context) && home_shown)return;
    const auto* field=Display(context)?FieldDiscovery(map,value):nullptr;
    std::lock_guard lock(map_mutex);
    map_ready=false;map_uri.clear();
    if(!field || !map_assets) {map_continuity=false;return;}
    uint32_t next_rifts=0;
    if(rifts.known && rifts.source==map.source)for(size_t i=0;i<rifts.regions.size() && i<29;++i)
      if(rifts.regions[i].active)next_rifts|=uint32_t(1)<<i;
    bool same=map_continuity && map_slots_used && map_slots[map_slot].source==map.source;
    if(same) for(size_t i=0;i<64;++i)
      if((map_slots[map_slot].discovered[i]&field->discovered[i])!=map_slots[map_slot].discovered[i])same=false;
    // A previous image with fewer covers must never be the fallback when a
    // native story change (or unknown observation) adds a cover again.
    if(same && ((map_slots[map_slot].field_extensions_hidden && !extensions_hidden) ||
                (map_slots[map_slot].sandstorm_hidden && !sandstorm_hidden)))same=false;
    if(same && map_slots[map_slot].rifts!=next_rifts)same=false;
    if(!same) {
      if(map_slots_used==map_slots.size()){map_continuity=false;return;}
      map_slot=map_slots_used++;map_slots[map_slot].used=true;
      map_slots[map_slot].source=map.source;
    }
    auto& slot=map_slots[map_slot];slot.rifts=next_rifts;
    slot.discovered=field->discovered;slot.field_extensions_hidden=extensions_hidden;slot.sandstorm_hidden=sandstorm_hidden;
    map_continuity=true;map_ready=true;
    constexpr char hex[]="0123456789abcdef";
    map_uri="module:echoes/map/"+std::to_string(map_slot)+"/";
    for(auto b:slot.discovered){map_uri+=hex[b>>4];map_uri+=hex[b&15];}
    map_uri+="/";map_uri+=static_cast<char>('0'+(extensions_hidden?1:0)+(sandstorm_hidden?2:0));
    map_uri+="/"+std::to_string(slot.rifts);
  }
  // Intents use the game's controller path only. A native observation must
  // confirm each transition; neither favorites nor equipment are written here.
  static bool SameOwnership(const LiveSnapshot& a,const LiveSnapshot& b) {
    return a.compatible && b.compatible && a.collection_valid && b.collection_valid &&
        a.collection_source==b.collection_source && a.learned_native_ids==b.learned_native_ids &&
        a.learned_crc_keys==b.learned_crc_keys && a.automatons.ownership_valid==b.automatons.ownership_valid &&
        a.automatons.source==b.automatons.source && a.automatons.owned_native_ids==b.automatons.owned_native_ids;
  }
  static std::optional<bool> Favorite(const LiveSnapshot& value,uint16_t id) {
    if(IsAutomaton(id))return {};
    if (value.selector.held && value.selector.favorite_valid)
      return std::find(value.selector.favorite_native_ids.begin(),value.selector.favorite_native_ids.end(),id)!=value.selector.favorite_native_ids.end();
    const auto at=std::find(value.learned_native_ids.begin(),value.learned_native_ids.end(),id);
    if (!value.favorite_valid || at==value.learned_native_ids.end()) return {};
    const auto crc=value.learned_crc_keys[static_cast<size_t>(at-value.learned_native_ids.begin())];
    return std::find(value.favorite_crc_keys.begin(),value.favorite_crc_keys.end(),crc)!=value.favorite_crc_keys.end();
  }
  bool SettledEcho(const LiveSnapshot& value,uint16_t id) const {
    const auto& s=value.selector;std::array<uint8_t,3> state{};
    return s.selector_valid && s.held && s.highlighted_native_id==id &&
      s.highlighted_index==s.target_index &&
      Reader(host)(s.source.owner+(s.source.mode==0?0x2ed8:0x1e8),state.data(),state.size()) && state[0]==1 && state[2]==0xfe;
  }
  bool JumpEcho(const LiveSnapshot& value,uint16_t id) {
    if(!host.write_memory)return false;
    const auto write=[&](uint64_t at,const void* bytes,size_t n){return host.write_memory(host.userdata,at,bytes,n);};
    return IsAutomaton(id)?DirectSelectAutomaton(value.automatons,id,Reader(host),write):DirectSelect(value,id,Reader(host),write);
  }
  void FinishIntent() {
    if(host.log){char message[160];std::snprintf(message,sizeof(message),"Echoes native action complete: kind=%d id=%u sort=%u",int(intent.kind),unsigned(intent.id),unsigned(intent.sort));host.log(host.userdata,1,message);}
    intent={};input_hold=input_left=input_x=input_y=false;x_until=y_until=0;intent_result=2;
  }
  void CancelIntent(int reason=0) {
    if(intent.kind!=IntentKind::None){intent_result=3;intent_abort=reason;}
    intent={}; input_hold=input_left=input_x=input_y=false; x_until=y_until=0;
    drive.Cancel();
  }
  bool StartEchoIntent(IntentKind kind,uint16_t id,bool favorite,const LiveSnapshot& value,uint64_t tick) {
    if (intent.kind!=IntentKind::None || menu_drive.status()==MenuDriveStatus::Pending || drive.status()==DriveStatus::Pending ||
        !value.collection_valid)return false;
    const auto& owned=IsAutomaton(id)?value.automatons.owned_native_ids:value.learned_native_ids;
    if(std::find(owned.begin(),owned.end(),id)==owned.end())return false;
    if(kind==IntentKind::Favorite) {
      const auto current=Favorite(value,id);if(!current)return false;
      if(*current==favorite)return true;
    }
    const bool owns=!value.selector.held;
    if(owns && (!context.valid || context.context!=NativeContext::FieldHud))return false;
    if(!owns && !ui_held)return false;
    intent_abort=0;intent_result=1;intent={kind,value,id,0,favorite,owns,0,tick,tick};
    return true;
  }
  void AdvanceIntent(const LiveSnapshot& value,uint64_t tick) {
    input_hold=input_left=input_x=input_y=false;
    if(intent.kind==IntentKind::None)return;
    if(tick<intent.started || tick-intent.started>2400) {CancelIntent(1);return;}
    if(!SameOwnership(intent.source,value)) {CancelIntent(2);return;}
    if(!Normal(context)) {CancelIntent(3);return;}
    if(intent.kind==IntentKind::Sort) {
      // A Y pulse rebuilds the native list. During that animation the strict
      // reader may publish no selector; wait without emitting another pulse.
      if(!value.selector.selector_valid || !value.selector.sort_valid) {
        if(intent.stage==0 || tick-intent.phase>120)CancelIntent(5);
        return;
      }
      if(!value.selector.held || value.selector.source.owner!=intent.source.selector.source.owner ||
         !value.selector.equipped_valid || value.selector.equipped_native_id!=intent.source.selector.equipped_native_id) {CancelIntent(5);return;}
      if(value.selector.sort_enum==intent.sort && tick-intent.phase>=30 && SettledEcho(value,value.selector.highlighted_native_id)) {
        // Only our acknowledged sort transition may start a fresh stable
        // bootstrap in the same hold. Arbitrary source changes stay disarmed.
        FinishIntent();drive=SelectorDrive{};return;
      }
      if(intent.stage==0) {
        if(!host.write_memory){CancelIntent(6);return;}
        const auto prepared=PrepareDirectSort(value,intent.sort,Reader(host),[&](uint64_t at,const void* bytes,size_t n){return host.write_memory(host.userdata,at,bytes,n);});
        if(prepared==DirectSortResult::Refused){CancelIntent(6);return;}
        if(prepared==DirectSortResult::Current){FinishIntent();return;}
        intent.phase=tick;intent.stage=1;y_until=tick+SelectorDrive::PulseTicks;
      }
      // Exactly one native Y operation. The index was positioned once so the
      // original handler sorts straight to the target; never cycle or retry.
      input_y=tick<y_until && value.selector.sort_enum!=intent.sort;
      if(tick-intent.phase>120)CancelIntent(6);
      return;
    }
    if(intent.stage==6) {
      // A favorite edit must restore a machine that previously occupied Y.
      // Close the Echo picker before opening the native machine picker.
      if(value.selector.selector_valid && !value.selector.held && tick-intent.phase>=8) {
        intent.id=intent.source.selector.equipped_native_id;intent.kind=IntentKind::Equip;
        intent.source=value;intent.stage=0;intent.phase=tick;
      } else if(tick-intent.phase>120)CancelIntent(7);
      return;
    }
    if(intent.stage==7) {
      if(value.selector.equipped_valid && value.selector.equipped_native_id==intent.id && !value.selector.held){FinishIntent();return;}
      if(tick-intent.phase>120)CancelIntent(7);
      return;
    }
    input_hold=intent.owns_hold && !IsAutomaton(intent.id);
    input_left=intent.owns_hold && IsAutomaton(intent.id);
    if(!value.selector.selector_valid || !value.selector.held) {
      if(intent.stage>0 || tick-intent.started>180)CancelIntent(4);
      return;
    }
    if(value.selector.source.owner!=intent.source.selector.source.owner && intent.source.selector.source.owner) {CancelIntent(5);return;}
    if(intent.stage==0 && intent.owns_hold) {
      // A readable picker can precede its input-ready opening animation.
      // Let the native opening animation settle before one direct cursor jump.
      intent.stage=5;intent.phase=tick;return;
    }
    if(intent.stage==5 && tick-intent.phase<30)return;
    if(intent.stage==0 || intent.stage==5) {
      if(JumpEcho(value,intent.id)){intent.stage=1;intent.phase=tick;}
      else if(tick-intent.phase>120)CancelIntent(6);
      return;
    }
    if(intent.stage==1) {
      if(!SettledEcho(value,intent.id)){if(tick-intent.phase>90)CancelIntent(6);return;}
      if(intent.kind==IntentKind::Equip){intent.stage=7;intent.phase=tick;input_hold=input_left=false;return;}
      const auto favorite=Favorite(value,intent.id);
      if(!favorite){CancelIntent();return;}
      if(*favorite==intent.favorite){FinishIntent();return;}
      x_until=tick+2;intent.stage=2;intent.phase=tick;
    }
    if(intent.stage==2) {
      input_x=tick<x_until;
      if(value.selector.highlighted_native_id!=intent.id){CancelIntent();return;}
      const auto favorite=Favorite(value,intent.id);
      if(favorite && *favorite==intent.favorite && tick>=x_until+2) {
        if(intent.owns_hold && intent.source.selector.equipped_valid &&
            value.selector.highlighted_native_id!=intent.source.selector.equipped_native_id) {
          intent.stage=IsAutomaton(intent.source.selector.equipped_native_id)?6:3;
          intent.phase=tick;input_x=false;x_until=0;
          if(intent.stage==6)input_hold=input_left=false;
          return;
        }
        FinishIntent();return;
      }
      if(tick-intent.phase>120)CancelIntent();
    }
    if(intent.stage==3) {
      if(JumpEcho(value,intent.source.selector.equipped_native_id)){intent.stage=4;intent.phase=tick;}
      else if(tick-intent.phase>120)CancelIntent(6);
    } else if(intent.stage==4) {
      if(SettledEcho(value,intent.source.selector.equipped_native_id))FinishIntent();
      else if(tick-intent.phase>90)CancelIntent(6);
    }
  }
  void PublishDungeonFloors(const std::shared_ptr<const ReadyResources>& ready) {
    for(size_t i=0;i<10;++i) {
      const auto& widget=IndexedKeys(dungeon_floor_keys_,i,BuildDungeonFloorKeys);
      I(widget.visible,0);I(widget.selected,0);T(widget.image,"");I(widget.token,0);
    }
    const bool floor_ready=ready && home_shown && home_page==4 && !ui_held && dungeon_valid && dungeon_ready;
    if(!floor_ready || dungeon_floor_menu_slot!=dungeon_slot)dungeon_floor_open=false;
    I("echo.dungeon.floor.menu",floor_ready && dungeon_floor_open);
    T("echo.dungeon.floor.current","");
    I("echo.dungeon.floor.rows",0);I("echo.dungeon.floor.index",0);
    if(!ready || !dungeon_valid)return;
    const auto def=std::find_if(ready->dungeons.begin(),ready->dungeons.end(),
                              [&](const auto& row){return row.key==dungeon_key;});
    if(def==ready->dungeons.end())return;
    T("echo.dungeon.floor.current","module:echoes/dungeon-floor/"+std::to_string(ready->locale_flag)+"/"+std::string(DungeonFloorMessageKey(*def,dungeon_selected)));
    I("echo.dungeon.floor.rows",def->floor_count);
    I("echo.dungeon.floor.index",def->floor_min+def->floor_count-1-dungeon_selected);
    I("echo.dungeon.floor.epoch",static_cast<int64_t>(dungeon_slot+1));
    for(int32_t i=0;i<def->floor_count;++i) {
      const auto floor=def->floor_min+def->floor_count-1-i;
      const auto key=DungeonFloorMessageKey(*def,floor);
      const auto& widget=IndexedKeys(dungeon_floor_keys_,static_cast<size_t>(i),BuildDungeonFloorKeys);
      const bool shown=home_shown && home_page==4 && !ui_held && dungeon_ready;
      I(widget.visible,shown);I(widget.selected,shown && dungeon_floor_open && floor==dungeon_selected);
      I(widget.token,static_cast<int64_t>(((dungeon_slot+1)<<8)|static_cast<size_t>(i)));
      T(widget.image,"module:echoes/dungeon-floor/"+std::to_string(ready->locale_flag)+"/"+std::string(key));
    }
  }
  // The HUD's own slots, shown on the bottom rail: Y is the set Echo, X is Bind,
  // D-pad Up is Swordfighter Form. Only verified state picks the Y artwork.
  void PublishRail(const std::shared_ptr<const ReadyResources>& ready,const LiveSnapshot& value) {
    const bool rail=home_shown && !ui_held && stamp_kind==0 && !dungeon_supported && !warp_open;
    const EchoMetadata* current=rail && value.selector.equipped_valid?
        Metadata(*ready,value,value.selector.equipped_native_id):nullptr;
    I("echo.rail.ready",rail);
    I("echo.rail.up",rail && value.learned_native_ids.size()>=SwordfighterLearnedProxy);
    I("echo.rail.y.valid",current!=nullptr);
    T("echo.rail.y.icon",current?"module:echoes/icon/"+current->actor:"");
    T("echo.rail.y.cost",current?CostImage(*current,110):"");
  }
  void PublishInspection(const std::shared_ptr<const ReadyResources>& ready,const LiveSnapshot& value) {
    const bool normal=Display(context);
    // View defaults require float-only points: frozen host SnapshotNumber reads
    // an integer shadow before floats, and derived values create such a shadow.
    if(host.publish_f64) {
      host.publish_f64(host.userdata,"echo.map.view.zoom",1.26);
      DungeonRoomFocus focus;
      if(ready && dungeon_ready && dungeon_player_room.valid && dungeon_selected==dungeon_player_room.floor &&
         dungeon_player_room.room<64 && (dungeon_progress.visited_rooms & (UINT64_C(1)<<dungeon_player_room.room))) {
        const auto def=std::find_if(ready->dungeons.begin(),ready->dungeons.end(),[&](const auto& row){return row.key==dungeon_key;});
        const auto art=ready->dungeon_art.find(dungeon_key);
        if(def!=ready->dungeons.end() && art!=ready->dungeon_art.end())
          focus=ProjectDungeonRoomFocus(*def,art->second,dungeon_player_room.room,dungeon_selected,MapW,MapH);
      }
      const double focus_zoom=focus.valid?std::clamp(std::min(1./(focus.extent_uv[0]*1.8),1./(focus.extent_uv[1]*1.8)),1.6,3.0):1.0;
      host.publish_f64(host.userdata,"echo.dungeon.view.zoom",focus_zoom);
      host.publish_f64(host.userdata,"echo.dungeon.view.cx",focus.valid?focus.center_uv[0]:.5);
      host.publish_f64(host.userdata,"echo.dungeon.view.cy",focus.valid?focus.center_uv[1]:.5);
      host.publish_f64(host.userdata,"echo.map.view.center",.5);
    }
    prologue_ready=ready && context.prologue_hud && prologue.known && prologue.location.key=="OP_Dungeon" && ready->prologue.size()==5 && ready->prologue_text.size()==5;
    {
      const bool left=(context.valid && context.context==NativeContext::Title) ||
          (prologue.known && prologue.location.key!="OP_Dungeon") ||
          (location.known && location.key!="OP_Dungeon") || !ready;
      // Hold the opening page through any top-screen event (hints, dialogue,
      // cutscenes, the pause menu) until the opening is really left: the
      // title, or a known location outside the opening dungeon.
      if(prologue_ready){prologue_display=true;prologue_hold=0;prologue_shown=prologue;}
      else if(prologue_display && !left && (context.valid || prologue_hold<PrologueHoldSamples))++prologue_hold;
      else {prologue_display=false;prologue_hold=0;}
    }
    const auto& shown=prologue_ready?prologue:prologue_shown;
    home_ready=ready && normal && value.collection_valid && !(location.known && location.key=="OP_Dungeon") && !prologue.opening && !prologue_display;
    const bool gear_ready=home_ready && ready->inventory_ready && inventory.inventory_valid;
    if (home_ready && !home_page_initialized) {
      std::lock_guard lock(map_mutex);
      home_page=(map_ready || dungeon_ready)?4:(gear_ready?1:3); home_page_initialized=true;
    }
    if ((home_page==1 || home_page==2) && !gear_ready) home_ready=false;
    {
      const bool left=!ready || prologue_display || prologue.opening || (location.known && location.key=="OP_Dungeon") ||
          (context.valid && context.context==NativeContext::Title);
      if(home_ready){home_shown=true;home_hold=0;}
      // A readable non-title context keeps the page however long a top-screen
      // event lasts; only unreadable samples are bounded.
      else if(home_shown && !left && (context.valid || home_hold<HomeHoldSamples))++home_hold;
      else {home_shown=false;home_hold=0;}
    }
    if (gear_ready) {
      if (previous_inventory_valid && !SameInventory(previous_inventory,inventory)) ++home_epoch;
      previous_inventory=inventory; previous_inventory_valid=true;
    }
    if (home_ready && observed.collection_valid && (value.collection_source!=observed.collection_source ||
        value.learned_native_ids!=observed.learned_native_ids || value.learned_crc_keys!=observed.learned_crc_keys))
      ++home_epoch;
    if(home_ready && (value.automatons.source!=observed.automatons.source || value.automatons.owned_native_ids!=observed.automatons.owned_native_ids))++home_epoch;
    if (home_epoch>=(UINT64_C(1)<<38)) home_epoch=1;
    I("echo.home.ready",home_shown);
    I("echo.rupees.valid",home_shown && rupees.rupees_valid);
    I("echo.rupees.count",rupees.rupees_valid?rupees.rupees:0);
    T("echo.rupees.image",ready && rupees.rupees_valid?"module:echoes/rupees/"+std::to_string(ready->locale_flag)+"/"+std::to_string(rupees.rupees):"");
    T("echo.map.reset.image",ready?"module:echoes/map-reset/"+std::to_string(ready->locale_flag):"");
    PublishRail(ready,value);
    I("echo.map.sandstorm.known",field_story.sandstorm_known);
    I("echo.map.sandstorm.visible",field_story.sandstorm_visible);
    I("echo.map.story.known",field_story.known);
    I("echo.map.story.visible",field_story.native_field_extension_visible);
    I("echo.map.story.query_ready",ready && ready->field_story.valid);
    {
      std::lock_guard lock(map_mutex);
      I("echo.map.ready",(normal || home_shown) && value.collection_valid && (map_ready || dungeon_ready));
      I("echo.dungeon.supported",dungeon_supported);
      I("echo.map.field_scope",!dungeon_supported);
      I("echo.dungeon.valid",dungeon_valid);
      I("echo.dungeon.ready",home_shown && home_page==4 && !ui_held && !value.selector.held && dungeon_ready);
      I("echo.dungeon.floor",dungeon_floor);I("echo.dungeon.selected",dungeon_selected);
      I("echo.dungeon.map_item",dungeon_progress.map_item);
      I("echo.dungeon.variants.known",dungeon_variants.known);
      I("echo.dungeon.visited",std::bit_cast<int64_t>(dungeon_progress.visited_rooms));
      I("echo.dungeon.slots_exhausted",dungeon_views.used()==dungeon_views.slots().size() && !dungeon_ready);
      T("echo.dungeon.key",dungeon_supported?dungeon_key:"");
      T("echo.dungeon.image",(normal || home_shown) && dungeon_ready?dungeon_uri:"");
      for(size_t i=0;i<dungeon_views.slots().size();++i)
        I(IndexedKey(dungeon_slot_visible_keys_,i,"echo.dungeon.slot.",".visible"),home_shown && home_page==4 && !ui_held &&
          !value.selector.held && (normal || home_shown) && dungeon_ready && i==dungeon_slot);
      I("echo.map.slots_exhausted",map_slots_used==map_slots.size() && !map_ready);
      T("echo.map.image",(normal || home_shown) && map_ready?map_uri:"");
      for(size_t i=0;i<map_slots.size();++i)
        I(IndexedKey(map_slot_visible_keys_,i,"echo.map.slot.",".visible"),home_shown && home_page==4 && !ui_held && !value.selector.held &&
          (normal || home_shown) && map_ready && i==map_slot);
    }
    PublishDungeonFloors(ready);
    const auto* display_location=&location;
    // Native MapMenu withdraws the area's live reader. Retain only the
    // pre-open authenticated name while this companion owns the same map,
    // save and initial resource; this is presentation, never travel permission.
    if(!location.known && warp_open && context.valid && context.context==NativeContext::NativeMenu &&
       warp_location.known && current_map.source==warp_source && warp_map.source==warp_source &&
       current_map.current_level_source==warp_location.level_source &&
       current_map.current_level_key==warp_initial_level && warp_map.current_level_key==warp_initial_level)
      display_location=&warp_location;
    T("echo.map.heading",ready && display_location->known?std::string("module:echoes/map-heading-field/")+std::to_string(ready->locale_flag)+"/"+display_location->key:"");
    I("echo.map.area.known",display_location->known);T("echo.map.area.key",display_location->known?display_location->key:"");I("echo.map.rifts.known",rifts.known);I("echo.map.rifts.count",rifts.known?static_cast<int64_t>(std::count_if(rifts.regions.begin(),rifts.regions.end(),[](const auto& r){return r.active;})):0);
    PublishMarker(value);
    PublishNativeMapIcons(ready,host.get_tick(host.userdata));
    PublishDungeonIcons(ready);
    I("echo.context.field_presentation",context.field_presentation);
    I("echo.context.valid",context.valid); I("echo.context.kind",static_cast<int64_t>(context.context));
    I("echo.inventory.ready",inventory.inventory_valid); I("echo.inventory.catalog_ready",ready && ready->inventory_ready);
    I("echo.page",prologue_display?6:(ui_held?5:(home_shown?home_page:0)));
    const bool combat=home_shown && home_page==4 && !ui_held && !stamp_kind && !warp_open &&
      swordfighter_display_active && ready && ready->prologue.size()==5;
    I("echo.combat.visible",combat);
    I("echo.combat.input_ready",combat && swordfighter.known && swordfighter.active && context.valid && context.context==NativeContext::FieldHud &&
      intent.kind==IntentKind::None && menu_drive.status()!=MenuDriveStatus::Pending);
    I("echo.combat.known",swordfighter.known);I("echo.combat.active",swordfighter.active);I("echo.combat.mode",swordfighter.mode);
    if(ready)for(const int index:{1,3,4})T("echo.combat."+std::to_string(index)+".icon",std::string("module:echoes/prologue-icon/")+std::to_string(ready->locale_flag)+"/"+std::to_string(index));
    I("echo.prologue.link",prologue_display && shown.opening);
    I("echo.prologue.actor.known",shown.actor.known);I("echo.prologue.actor.kind",shown.actor.kind);
    I("echo.prologue.ready",prologue_display);I("echo.prologue.hud",context.prologue_hud);I("echo.prologue.map.valid",prologue_map_valid);I("echo.prologue.level.valid",prologue_level_valid);I("echo.prologue.scope.known",prologue.known);I("echo.prologue.catalog.count",ready?static_cast<int64_t>(ready->prologue.size()):0);I("echo.prologue.text.count",ready?static_cast<int64_t>(ready->prologue_text.size()):0);
    T("echo.prologue.heading",ready && prologue_display && shown.known?"module:echoes/map-heading/"+std::to_string(ready->locale_flag)+"/"+shown.location.key:"");
    if(prologue_display) {
      const auto locale=std::to_string(ready->locale_flag);
      for(size_t i=0;i<ready->prologue.size();++i) {
        const auto prefix="echo.prologue."+std::to_string(i)+".";
        T(prefix+"icon","module:echoes/prologue-icon/"+locale+"/"+std::to_string(i));
        I(prefix+"selected",i==prologue_selected);
      }
      const auto& layout=ready->prologue_text.at(static_cast<uint32_t>(prologue_selected));
      T("echo.prologue.icon","module:echoes/prologue-icon/"+locale+"/"+std::to_string(prologue_selected));
      T("echo.prologue.name","module:echoes/inspection-title/"+locale+"/prologue/"+std::to_string(prologue_selected));
      T("echo.prologue.description","module:echoes/inspection-description/"+locale+"/prologue/"+std::to_string(prologue_selected));
      I("echo.prologue.name.h",layout.title_h);I("echo.prologue.line.y",InspectTop+layout.title_h);
      I("echo.prologue.description.h",layout.description_h);
    }
    // While a short read miss is held, replay the last published list: an
    // empty count for one sample snaps the grid's scroll back to the top.
    if(ready && home_shown && !home_ready && home_page!=4) {
      for(size_t i=0;i<inspect_int_count;++i)host.publish_i64(host.userdata,inspect_ints[i].first.c_str(),inspect_ints[i].second);
      for(size_t i=0;i<inspect_text_count;++i)host.publish_text(host.userdata,inspect_texts[i].first.c_str(),inspect_texts[i].second.c_str());
      return;
    }
    inspect_int_count=inspect_text_count=0;inspect_recording=true;
    struct StopRecording{bool& on;~StopRecording(){on=false;}} stop_recording{inspect_recording};
    I("echo.inspect.epoch",static_cast<int64_t>(home_epoch));
    I("echo.inspect.count",0); I("echo.inspect.index",-1);
    I("echo.inspect.action.ready",0);I("echo.inspect.action.show",0);
    I("echo.inspect.favorite.known",0);I("echo.inspect.favorite",0);I("echo.inspect.equip.ready",0);
    T("echo.inspect.favorite.image","");T("echo.inspect.action.image","");
    I("echo.inspect.selected.token",0);
    T("echo.inspect.heading",""); T("echo.inspect.description",""); T("echo.inspect.category","");
    T("echo.inspect.icon","");T("echo.inspect.count.image","");T("echo.items.heading","");T("echo.items.description","");
    I("echo.inspect.name.h",64);I("echo.inspect.line.y",InspectTop+64);I("echo.inspect.description.h",1);
    if (!ready) return;
    const auto locale=std::to_string(ready->locale_flag);
    T("echo.back.image","module:echoes/control/"+locale+"/Operation_Return");
    I("echo.notebook.automatons",notebook_automatons);
    T("echo.notebook.tabs","module:echoes/assets/notebook-tabs/"+std::to_string(notebook_automatons));
    for(int i=0;i<2;++i)T("echo.notebook.tab."+std::to_string(i),"module:echoes/notebook-tab/"+locale+"/"+std::to_string(i)+"/"+std::to_string(int(notebook_automatons)==i));
    T("echo.stamp.back.image","module:echoes/control236/"+locale+"/Operation_Return");
    for (int n=0;n<4;++n) {
      const int page=n==0?4:n;
      T("echo.nav."+std::to_string(n)+".image","module:echoes/nav/"+locale+"/"+std::to_string(n));
      I("echo.nav."+std::to_string(n)+".selected",home_page==page);
    }
    if (!home_ready || home_page==4) return;
    const auto ids=InspectionList(*ready,value,inventory,home_page);
    auto& selected=inspected[static_cast<size_t>(home_page)];
    if (std::find(ids.begin(),ids.end(),selected)==ids.end()) selected=ids.empty()?0xffff:ids.front();
    I("echo.inspect.count",static_cast<int64_t>(ids.size()));
    const auto detail_layout=[&](std::string_view kind,uint32_t key,const InspectionTextLayout* layout) {
      if(!layout)return;
      const auto root="module:echoes/inspection-";
      T("echo.inspect.heading",std::string(root)+"title/"+locale+"/"+std::string(kind)+"/"+std::to_string(key));
      T("echo.inspect.description",std::string(root)+"description/"+locale+"/"+std::string(kind)+"/"+std::to_string(key));
      I("echo.inspect.name.h",layout->title_h);I("echo.inspect.line.y",InspectTop+layout->title_h);
      I("echo.inspect.description.h",layout->description_h);
      I("echo.inspect.favorite.x",24+layout->title_right+16);
      I("echo.inspect.favorite.y",680+std::max(0,int(layout->title_bottom)-64));
    };
    I("echo.inspect.detail.epoch",static_cast<int64_t>((home_epoch<<16)|selected));
    bool selected_equipped=false;
    for (size_t i=0;i<ids.size();++i) {
      const auto id=ids[i]; const auto& key=IndexedKeys(inspect_cell_keys_,i,BuildInspectCellKeys);
      const bool chosen=id==selected;
      I(key.token,static_cast<int64_t>((home_epoch<<24)|(static_cast<uint64_t>(home_page)<<16)|id));
      I(key.selected,chosen); I(key.equipped,0);
      I(key.echo_selected,chosen && !notebook_automatons);I(key.machine_selected,chosen && notebook_automatons);
      I(key.echo_equipped,0);I(key.machine_equipped,0);
      T(key.favorite,home_page==3 && Favorite(value,id).value_or(false)?"module:echoes/assets/favorite-on":"");
      T(key.quantity_src,""); T(key.equipped_icon,""); T(key.cost,"");
      if (home_page==3) {
        const auto* entry=Metadata(*ready,value,id);
        T(key.icon,"module:echoes/icon/"+entry->actor);
        T(key.cost,CostImage(*entry,80));
        // The native Notebook checks the Echo currently set on Y.
        if (value.selector.equipped_valid && id==value.selector.equipped_native_id) {
          I(key.equipped,1);I(notebook_automatons?key.machine_equipped:key.echo_equipped,1);T(key.equipped_icon,"module:echoes/assets/equipped-check");
        }
        if (chosen) {
          T("echo.inspect.icon","module:echoes/icon/"+entry->actor);
          const auto& layouts=IsAutomaton(id)?ready->inspection_automaton_text:ready->inspection_echo_text;
          const auto layout=layouts.find(entry->crc_key);
          detail_layout(IsAutomaton(id)?"automaton":"echo",entry->crc_key,layout==layouts.end()?nullptr:&layout->second);
        }
      } else {
        const auto& entry=ready->inventory.entries[id];
        if(home_page==1 && Equipped(entry)) {
          I(key.equipped,1);T(key.equipped_icon,"module:echoes/assets/equipped-check");
        }
        T(key.icon,"module:echoes/item-icon/"+locale+"/"+std::to_string(id));
        if (home_page==2)
          T(key.quantity_src,"module:echoes/quantity/"+locale+"/"+std::to_string(Quantity(entry,inventory)));
        if (chosen) {
          selected_equipped=Equipped(entry);
          const bool gear=entry.kind==InventoryKind::Accessory || entry.kind==InventoryKind::Outfit;
          const bool actionable=gear || (entry.kind==InventoryKind::Ingredient && entry.edible);
          I("echo.inspect.action.show",actionable);
          const bool capacity=entry.kind!=InventoryKind::Accessory || selected_equipped || inventory.equipped_accessory_count<inventory.accessory_capacity;
          // Outfit state commits when the native pause closes. Only start it
          // from gameplay, so we never close a pause opened by the player.
          const bool not_current_outfit=entry.kind!=InventoryKind::Outfit ||
              (!selected_equipped && context.context==NativeContext::FieldHud);
          I("echo.inspect.action.ready",actionable && capacity && not_current_outfit && Normal(context) && intent.kind==IntentKind::None && menu_drive.status()!=MenuDriveStatus::Pending);
          if(actionable)T("echo.inspect.action.image","module:echoes/control/"+locale+"/"+
            (gear?(entry.kind==InventoryKind::Accessory && selected_equipped?"DressUpMenu_Dialog_Remove":"DressUpMenu_Dialog_Equipment"):(entry.drink?"ItemMenu_Dialog_Drink":"ItemMenu_Dialog_Eat")));
          T("echo.inspect.icon","module:echoes/item-icon/"+locale+"/"+std::to_string(id));
          if(home_page==2) {
            T("echo.items.heading","module:echoes/item-heading/"+locale+"/"+std::to_string(id));
            T("echo.items.description","module:echoes/detail/"+locale+"/item/"+std::to_string(id));
          }
          const auto layout=ready->inventory_text.find(id);
          detail_layout("item",id,layout==ready->inventory_text.end()?nullptr:&layout->second);
        }
      }
      if (chosen) {
        I("echo.inspect.selected.token",static_cast<int64_t>((home_epoch<<24)|(static_cast<uint64_t>(home_page)<<16)|id));
        if(home_page==3) {
          const auto favorite=Favorite(value,id);
          I("echo.inspect.favorite.known",favorite.has_value());
          I("echo.inspect.favorite",favorite.value_or(false));
          T("echo.inspect.favorite.image",favorite?std::string("module:echoes/assets/favorite-")+(*favorite?"on":"off"):"");
          I("echo.inspect.equip.ready",context.context==NativeContext::FieldHud && intent.kind==IntentKind::None &&
            value.selector.equipped_valid && value.selector.equipped_native_id!=id && menu_drive.status()!=MenuDriveStatus::Pending);
          T("echo.inspect.action.image","module:echoes/control202/"+locale+"/DressUpMenu_Dialog_Equipment");
        }
        I("echo.inspect.index",static_cast<int64_t>(i));
        T("echo.inspect.count.image","module:echoes/inspection-count/"+locale+"/"+std::to_string(i+1)+"/"+std::to_string(ids.size()));
      }
    }
    T("echo.inspect.category","module:echoes/category/"+locale+"/"+std::to_string(home_page)+"/"+
        (selected_equipped?"1":"0"));
  }
  void Publish(const LiveSnapshot& value, Direction pulse, bool neutral) {
    I("echoes.build_ok", value.compatible);
    I("echoes.collection.ready", value.collection_valid);
    I("echoes.collection.count", static_cast<int64_t>(value.learned_native_ids.size()));
    const auto& selector=value.selector;
    I("echoes.selector.ready",selector.selector_valid);
    I("echoes.selector.held",selector.selector_valid && selector.held);
    I("echoes.selector.count",static_cast<int64_t>(selector.ordered_native_ids.size()));
    I("echoes.selector.index",selector.highlighted_index);
    I("echoes.selector.target_index",selector.target_index);
    I("echoes.selector.id",selector.highlighted_native_id);
    I("echoes.equipped.ready",selector.equipped_valid);
    I("echoes.equipped.id",selector.equipped_native_id);
    const auto ready=resources.Snapshot();
    bool held=ready && !prologue.opening && value.compatible && value.collection_valid && selector.selector_valid &&
        selector.held && selector.highlighted_index>=0 &&
        static_cast<size_t>(selector.highlighted_index)<selector.ordered_native_ids.size();
    if (held) for (const auto id:selector.ordered_native_ids)
      if (!Metadata(*ready,value,id)) { held=false; break; }
    const bool show_held=held && !intent.owns_hold;
    if (ui_held != show_held || (show_held && !SameHold(observed,value))) Bump();
    ui_held=show_held;
    I("echo.ui.ready",bool(ready)); I("echo.ui.held",ui_held);
    I("echo.direct.status",direct_status);
    I("echo.helper.healthy",helper.Healthy());I("echo.helper.status",static_cast<int64_t>(helper.LastEquipYStatus()));
    I("echo.helper.code",static_cast<int64_t>(helper.LastEquipYRefuseCode()));
    I("echo.auto.busy",helper.RequestInFlight() || intent.kind!=IntentKind::None || menu_drive.status()==MenuDriveStatus::Pending || warp_drive.status()==WarpTravelStatus::Pending);
    I("echo.auto.result",intent_result);I("echo.auto.abort",intent_abort);I("echo.auto.kind",static_cast<int>(intent.kind));I("echo.auto.stage",intent.stage);
    I("echo.menu.status",static_cast<int>(menu_drive.status()));I("echo.menu.abort",static_cast<int>(menu_drive.abort_reason()));
    constexpr std::array<const char*,8> menu_names{"plus","r","up","down","left","right","a","b"};
    for(size_t i=0;i<menu_names.size();++i)I(std::string("echo.menu.input.")+menu_names[i],menu_pulse==static_cast<MenuPulse>(i+1));
    I("echo.input.hold",input_hold);I("echo.input.hold_left",input_left);I("echo.input.x",input_x);I("echo.input.y",input_y);
    I("echo.ui.epoch",static_cast<int64_t>(epoch));
    I("echo.favorite.star.token",static_cast<int64_t>((epoch<<16)|0xffff));
    I("echo.ui.count",held?static_cast<int64_t>(selector.ordered_native_ids.size()):0);
    I("echo.ui.index",held?selector.highlighted_index:-1);
    I("echo.drive.pending",drive.status()==DriveStatus::Pending);
    I("echo.drive.status",static_cast<int64_t>(drive.status()));
    I("echo.drive.abort",static_cast<int64_t>(drive.abort_reason()));
    I("echo.drive.left",pulse==Direction::Left && held);
    I("echo.drive.right",pulse==Direction::Right && held);
    I("echo.drive.neutral",neutral);
    I("echo.drive.any",neutral || (held && pulse!=Direction::None) || input_hold || input_left || input_x || input_y || menu_pulse!=MenuPulse::None || warp_pulse!=WarpPulse::None);
    T("echo.selected.name",""); T("echo.selected.heading","");
    T("echo.sort.image","");
    I("echo.sort.ready",held && selector.sort_valid && selector.source.mode==1);
    I("echo.ui.automaton",held && selector.source.mode==0);
    T("echo.held.category",ready?"module:echoes/notebook-tab/"+std::to_string(ready->locale_flag)+"/1/0":"");
    T("echo.sort.tabs.image",held && selector.sort_valid?"module:echoes/assets/sort-tabs/"+std::to_string(std::countr_zero(selector.sort_enum)):"");
    T("echo.sort.count.image",held?"module:echoes/sort-count/"+std::to_string(ready->locale_flag)+"/"+std::to_string(selector.highlighted_index+1)+"/"+std::to_string(selector.ordered_native_ids.size()):"");
    for(int i=0;i<5;++i) {
      const auto prefix="echo.sort.mode."+std::to_string(i)+".";
      I(prefix+"selected",held && selector.sort_valid && selector.sort_enum==(1<<i));
      T(prefix+"image",held && ready?"module:echoes/sort-tab/"+std::to_string(ready->locale_flag)+"/"+std::to_string(i)+"/"+std::to_string(selector.sort_valid && selector.sort_enum==(1<<i)):"");
    }
    for(int i=0;i<5;++i)T("echo.sort.dot."+std::to_string(i),held && selector.sort_valid?std::string("module:echoes/assets/sort-dot-")+(selector.sort_enum==(1<<i)?"on":"off"):"");
    T("echo.footer.count",""); T("echo.footer.instruction","");
    I("echo.selected.heading_valid",0);
    if (ready) {
      for (const auto& [key,text]:ready->ui) T("echo.ui."+key,text);
    }
    if (held) {
      if (selector.sort_valid)
        T("echo.sort.image","module:echoes/sort/"+std::to_string(ready->locale_flag)+"/"+
          std::to_string(selector.sort_enum));
      T("echo.footer.count","module:echoes/count/"+std::to_string(ready->locale_flag)+"/"+
        std::to_string(selector.highlighted_index+1)+"/"+
        std::to_string(selector.ordered_native_ids.size()));
      if (ready->locale_flag==2 || ready->locale_flag==32)
        T("echo.footer.instruction",std::string("module:echoes/")+
          (drive.status()==DriveStatus::Pending?"choosing/":"release/")+std::to_string(ready->locale_flag));
      for (size_t i=0;i<selector.ordered_native_ids.size();++i) {
        const auto id=selector.ordered_native_ids[i];
        const auto* entry=Metadata(*ready,value,id);
        const auto& key=IndexedKeys(cell_keys_,i,BuildCellKeys);
        T(key.icon,"module:echoes/icon/"+entry->actor);
        T(key.cost,CostImage(*entry,130));
        const auto favorite=Favorite(value,id);
        T(key.favorite,favorite && *favorite?"module:echoes/assets/favorite-on":"");
        I(key.selected,static_cast<int32_t>(i)==selector.highlighted_index);
        I(key.echo_selected,selector.source.mode==1 && static_cast<int32_t>(i)==selector.highlighted_index);
        I(key.machine_selected,selector.source.mode==0 && static_cast<int32_t>(i)==selector.highlighted_index);
        I(key.token,static_cast<int64_t>((epoch<<16)|id));
        if (static_cast<int32_t>(i)==selector.highlighted_index) {
          T("echo.selected.name",entry->name);
          if (!ready->hero_body.image.Empty()) {
            I("echo.selected.heading_valid",1);
            T("echo.selected.heading",std::string(IsAutomaton(id)?"module:echoes/held-automaton/":"module:echoes/held-name/")+std::to_string(ready->locale_flag)+"/"+
              std::to_string(entry->crc_key));
          }
        }
      }
    }
    quick_bridge.AdvanceRestore(quick_restored);
    if(value.compatible && value.collection_valid &&
       (quick_owner!=value.collection_source.access_object || quick_save!=value.collection_source.save_data)) {
      quick_owner=value.collection_source.access_object;quick_save=value.collection_source.save_data;
      quick_restored_applied=false;
    }
    quick_slots.Sync(ShortcutSnapshot(value));
    if(quick_slots.ready() && quick_bridge.status()==QuickSlotRestoreStatus::Ready) {
      if(!quick_restored_applied) {
        for(size_t i=0;i<3;++i)if(quick_restored[i])quick_slots.Assign(i,*quick_restored[i]);
        quick_restored_applied=true;
      }
      quick_bridge.QueueWrites(quick_slots.slots());
      for(size_t i=0;i<3;++i)quick_restored[i]=quick_slots.slots()[i]==QuickSlots::Empty?std::nullopt:std::optional<uint16_t>(quick_slots.slots()[i]);
    }
    I("echo.quickslot.restore.pending",quick_bridge.restore_pending());
    I("echo.quickslot.restore.slot",quick_bridge.restore_slot());
    I("echo.quickslot.enforce.any",quick_bridge.restore_pending() || quick_bridge.writes_pending());
    for(size_t i=0;i<3;++i) {
      I("echo.quickslot.persist."+std::to_string(i)+".pending",quick_bridge.write_pending(i));
      I("echo.quickslot."+std::to_string(i)+".packed",quick_bridge.write_value(i));
    }
    const bool quick_visible=home_shown && !ui_held && (home_page==3 || home_page==4) &&
      !stamp_kind && !warp_open && quick_slots.ready() &&
      !(home_page==4 && swordfighter_display_active && ready && ready->prologue.size()==5);
    I("echo.quick.visible",quick_visible);
    I("echo.quick.assign.ready",quick_visible && quick_restored_applied && home_page==3 && intent.kind==IntentKind::None);
    I("echo.quick.equip.ready",quick_visible && home_page==4 && intent.kind==IntentKind::None && context.valid && context.context==NativeContext::FieldHud);
    for(size_t i=0;i<QuickSlots::Count;++i) {
      const auto& key=IndexedKeys(quick_slot_keys_,i,BuildQuickKeys);
      const auto id=quick_slots.Target(i);const auto* entry=id && ready?Metadata(*ready,value,*id):nullptr;
      I(key.valid,quick_visible && entry);I(key.active,quick_visible && entry && quick_slots.active()==i);
      I(key.token,entry?static_cast<int64_t>((home_epoch<<24)|(i<<16)|*id):0);
      T(key.icon,entry?"module:echoes/icon/"+entry->actor:"");
      T(key.cost,entry?CostImage(*entry,110):"");
    }
    PublishInspection(ready,value);
    // PublishStamps/PublishMapLabels both independently queried the same
    // live map view (mutex lock + host pan/zoom reads); nothing between
    // them and here mutates home_ready/home_page/ui_held/map state, so one
    // CurrentMapView() this sample serves both instead of two.
    const auto field_map_view=CurrentMapView();
    PublishStamps(field_map_view);
    PublishMapLabels(ready,field_map_view);
    PublishWarp(ready);
    observed=value;
  }
};
EdenDsmodBool Supports(const char* build) { return build && SupportsBuild(build); }
void* Create(const EdenDsmodHostApi* host, const char*) {
  if (!dsmod_sdk::HostAbiMatches(host) || !HostBuild(*host) || !host->is_mapped ||
      !host->read_memory || !host->publish_i64 || !host->publish_text || !host->get_tick ||
      !host->read_romfs) return nullptr;
  try { return new Module(*host); } catch (...) { return nullptr; }
}
void Destroy(void* instance) { delete static_cast<Module*>(instance); }
void Sample(void* instance, const EdenDsmodHostApi* host) {
  if (!instance || !dsmod_sdk::HostAbiMatches(host)) return;
  auto& self=*static_cast<Module*>(instance);
  // Asset callbacks may overlap sampling. The host copy is immutable for this
  // module lifetime; a changed session must create a new instance.
  if (!HostBuild(*host) || host->main_base!=self.host.main_base ||
      host->userdata!=self.host.userdata) {
    self.CancelIntent();self.menu_drive.Cancel();self.menu_pulse=MenuPulse::None;self.StopWarp(); self.resources.SetNativeFonts({}); self.fonts.reset();
    self.context={}; self.inventory={}; self.ObserveMap({});
    self.Publish({},Direction::None,self.pulse_active); self.pulse_active=false;
    return;
  }
  try {
    const auto tick=host->get_tick(host->userdata);
    self.helper.Poll(*host);
    self.Fonts(tick);
    const auto value=self.Read();
    const auto observe_context=[&]{return ReadContext(Reader(self.host),self.host.main_base,HostBuild(self.host));};
    self.context=(self.warp_open || self.warp_drive.status()==WarpTravelStatus::Pending)
        ?RequalifyFresh(observe_context,[](const auto& value){return value.compatible && value.valid && value.context!=NativeContext::Unknown;})
        :observe_context();
    self.rupees=Module::Display(self.context)?ReadRupees(Reader(self.host),self.host.main_base,HostBuild(self.host)):RupeeSnapshot{};
    self.Diagnose(tick,value);
    self.ObserveDirect(value,tick);
    if (Module::Normal(self.context)) self.ReadCostReductions(); else self.cost_reduced={{0,-1,-1,-1,-1,-1,-1}};
    self.inventory=Module::Display(self.context) || self.menu_drive.status()==MenuDriveStatus::Pending
        ?ReadInventory(Reader(self.host),self.host.main_base,HostBuild(self.host)):InventorySnapshot{};
    const bool old_form=self.swordfighter_display_active;
    self.swordfighter=Module::Display(self.context)?ReadSwordfighter(Reader(self.host),self.host.main_base,HostBuild(self.host)):SwordfighterSnapshot{};
    // Reference acquisition can make one sample unavailable. Retain only the
    // visual rail for at most six samples; input always needs a fresh read.
    if(self.swordfighter.known) {
      self.swordfighter_display_active=self.swordfighter.active;
      self.swordfighter_missing_samples=0;
    } else if(!Module::Display(self.context) || ++self.swordfighter_missing_samples>6) {
      self.swordfighter_display_active=false;
      self.swordfighter_missing_samples=6;
    }
    if(self.host.log && old_form!=self.swordfighter_display_active) {
      char message[144];std::snprintf(message,sizeof(message),"Echoes Swordfighter: known=%u active=%u mode=%u kind=%u",self.swordfighter.known,self.swordfighter.active,self.swordfighter.mode,unsigned(self.swordfighter.actor.kind));self.host.log(self.host.userdata,1,message);
    }
    self.ObserveMap(value);
    self.ObserveWarp(tick);
    if(self.menu_drive.status()==MenuDriveStatus::Pending) {
      const auto menu=ReadMenu(Reader(self.host),self.host.main_base,HostBuild(self.host));
      const auto button=self.menu_drive.Advance(menu,self.inventory,self.context,tick);
      if(button!=MenuPulse::None){self.menu_pulse=button;self.menu_until=tick+2;}
    }
    if(tick>=self.menu_until || self.menu_drive.status()!=MenuDriveStatus::Pending)self.menu_pulse=MenuPulse::None;
    const auto before=self.drive.status();
    if (!self.resources.Snapshot() && before==DriveStatus::Pending) self.drive.Cancel();
    auto pulse=self.drive.Advance(value,tick);
    self.AdvanceIntent(value,tick);
    if(self.drive.status()==DriveStatus::Aborted)pulse=Direction::None;
    const bool aborted=before==DriveStatus::Pending && self.drive.status()!=DriveStatus::Pending &&
        self.drive.status()!=DriveStatus::Complete;
    const bool neutral=aborted && self.pulse_active &&
        (tick<self.pulse_at || tick-self.pulse_at<SelectorDrive::PulseTicks);
    if (self.pulse_active && (tick<self.pulse_at || tick-self.pulse_at>=SelectorDrive::PulseTicks))
      self.pulse_active=false;
    if (pulse!=Direction::None) { self.pulse_active=true; self.pulse_at=tick; }
    if (neutral) self.pulse_active=false;
    self.Publish(value,pulse,neutral);
  } catch (...) {
    self.CancelIntent();self.menu_drive.Cancel();self.menu_pulse=MenuPulse::None;self.StopWarp(); self.context={}; self.inventory={}; self.ObserveMap({});
    self.Publish({},Direction::None,self.pulse_active); self.pulse_active=false;
  }
}
void Tick(void*,const EdenDsmodHostApi*) {}
void Configure(void* instance,const EdenDsmodHostExtensions* host) {
  if (!host || host->version!=EDEN_DSMOD_EXT_VERSION || host->abi_hash!=EDEN_DSMOD_EXT_HASH ||
      host->struct_size<sizeof(*host)) return;
  try {
    acnh::SetAstcDecoder(host->userdata,host->decode_astc);
    if(instance) {
      auto& self=*static_cast<Module*>(instance);
      std::lock_guard lock(self.map_mutex);
      if(!self.map_assets && host->decode_astc)
        self.map_assets=std::make_shared<FieldMapAssets>(self.AssetReader(),host->userdata,host->decode_astc);
      if(!self.dungeon_assets && host->decode_astc)
        self.dungeon_assets=std::make_shared<DungeonMapAssets>(self.AssetReader());
      self.helper.Configure(host);
    }
  } catch (...) { /* An unavailable decoder never enables the map page. */ }
}
EdenDsmodBool Action(void* instance,const char* action,int64_t argument) {
  if (!instance || !action) return false;
  auto& self=*static_cast<Module*>(instance);
  try {
    const auto name=std::string_view(action);
    if(name=="stamp_restore_packet")return self.stamp_bridge.RestorePacket(argument);
    if(name=="stamp_persist_ack")return argument>=0 && argument<static_cast<int64_t>(StampCapacity) && self.stamp_bridge.AcknowledgeWrite(static_cast<size_t>(argument));
    if(argument<0)return false;
    if(name.starts_with("warp_"))return self.WarpAction(name,argument);
    if(self.warp_open || self.warp_drive.status()==WarpTravelStatus::Pending)return false;
    if(name=="prologue_inspect") {
      if(!self.prologue_ready || !self.prologue.opening || argument>=5)return false;
      const auto c=ReadContext(Reader(self.host),self.host.main_base,HostBuild(self.host));
      if(!c.prologue_hud || !ReadPrologueActor(Reader(self.host),self.host.main_base,HostBuild(self.host)).link)return false;
      self.prologue_selected=static_cast<size_t>(argument);return true;
    }
    if(name=="dungeon_floor_menu") {
      if(!self.home_ready || self.home_page!=4 || self.ui_held || !self.dungeon_valid || !self.dungeon_ready)return false;
      self.ObserveMap(self.Read());
      if(!self.dungeon_valid || !self.dungeon_ready)return false;
      self.dungeon_floor_menu_slot=self.dungeon_slot;
      self.dungeon_floor_open=!self.dungeon_floor_open;return true;
    }
    if(name=="dungeon_floor") {
      if(!self.home_ready || self.home_page!=4 || self.ui_held || !self.dungeon_valid || !self.dungeon_ready)return false;
      const auto token=static_cast<uint64_t>(argument);
      if((token>>8)!=self.dungeon_slot+1)return false;
      self.ObserveMap(self.Read());
      if(!self.dungeon_valid || !self.dungeon_ready || (token>>8)!=self.dungeon_slot+1)return false;
      const auto ready=self.resources.Snapshot();if(!ready)return false;
      const auto def=std::find_if(ready->dungeons.begin(),ready->dungeons.end(),
                                [&](const auto& row){return row.key==self.dungeon_key;});
      const auto index=token&255;
      if(def==ready->dungeons.end() || index>=static_cast<uint64_t>(def->floor_count))return false;
      self.dungeon_selected=def->floor_min+def->floor_count-1-static_cast<int32_t>(index);
      self.dungeon_floor_open=false;
      self.AcceptDungeon();return self.dungeon_ready;
    }
    if(name=="stamp_kind") {
      if(!self.home_ready || self.home_page!=4 || self.ui_held || self.dungeon_supported || argument>6)return false;
      const auto context=ReadContext(Reader(self.host),self.host.main_base,HostBuild(self.host));
      if(!Module::Normal(context))return false;
      self.stamp_kind=static_cast<uint8_t>(argument);return true;
    }
    if(name=="stamp_tap") {
      if(!self.home_ready || self.home_page!=4 || self.ui_held || self.dungeon_supported || !self.stamps_loaded || !self.stamp_kind)return false;
      std::lock_guard lock(self.map_mutex);if(!self.map_ready)return false;
      self.stamp_tap=Module::StampTap{argument,self.host.get_tick(self.host.userdata),self.map_slots[self.map_slot].source};return true;
    }
    if(name=="inventory_action") {
      if(!self.home_ready || self.ui_held || self.home_page<1 || self.home_page>2 ||
          self.intent.kind!=Module::IntentKind::None || self.menu_drive.status()==MenuDriveStatus::Pending)return false;
      const auto token=static_cast<uint64_t>(argument);
      if((token>>24)!=self.home_epoch || ((token>>16)&255)!=static_cast<uint64_t>(self.home_page) || static_cast<uint16_t>(token)!=self.inspected[self.home_page])return false;
      const auto ready=self.resources.Snapshot();if(!ready || !ready->inventory_ready || static_cast<uint16_t>(token)>=ready->inventory.entries.size())return false;
      const auto& entry=ready->inventory.entries[static_cast<uint16_t>(token)];
      const auto context=ReadContext(Reader(self.host),self.host.main_base,HostBuild(self.host));
      const auto inventory=ReadInventory(Reader(self.host),self.host.main_base,HostBuild(self.host));
      if(!Module::Normal(context) || !inventory.inventory_valid || !Module::SameInventory(inventory,self.inventory))return false;
      if(self.helper.RequestInFlight())return false;
      // In the field the guest helper applies the change natively (no menu);
      // anywhere else the companion still drives the pause menu.
      const bool direct=self.helper.Healthy() && context.valid && context.context==NativeContext::FieldHud;
      const auto menu=ReadMenu(Reader(self.host),self.host.main_base,HostBuild(self.host));
      const auto tick=self.host.get_tick(self.host.userdata);
      if(entry.kind==InventoryKind::Accessory || entry.kind==InventoryKind::Outfit) {
        if(direct)return entry.kind==InventoryKind::Outfit?self.helper.SubmitOutfit(static_cast<uint16_t>(entry.mask))
                                                           :self.helper.SubmitAccessory(entry.mask,!self.Equipped(entry));
        return self.menu_drive.RequestEquipment(entry.mask,entry.kind==InventoryKind::Outfit,!self.Equipped(entry),menu,inventory,context,tick);
      }
      if(entry.kind!=InventoryKind::Ingredient || !entry.edible || entry.quantity_index<0 || entry.quantity_index>15)return false;
      if(direct)return self.helper.SubmitConsume(static_cast<uint8_t>(entry.quantity_index),entry.heal);
      return self.menu_drive.RequestConsume(entry.item_id,static_cast<size_t>(entry.quantity_index),menu,inventory,context,tick);
    }
    if(name=="sort" || name=="sort_to") {
      if(!self.ui_held || self.intent.kind!=Module::IntentKind::None || self.menu_drive.status()==MenuDriveStatus::Pending || self.drive.status()==DriveStatus::Pending || argument<0 || argument>(name=="sort_to"?4:1))return false;
      const auto value=self.Read();
      if(!SameHold(self.observed,value) || !value.selector.sort_valid)return false;
      const auto current=std::countr_zero(value.selector.sort_enum);
      const auto target=name=="sort_to"?uint8_t(1<<argument):uint8_t(1<<((current+(argument?4:1))%5));
      // A tab reflects the observed native mode. Retapping it must not cycle Y.
      if(target==value.selector.sort_enum)return true;
      self.drive.Cancel();const auto tick=self.host.get_tick(self.host.userdata);
      self.intent={Module::IntentKind::Sort,value,0xffff,target,false,false,0,tick,tick};return true;
    }
    if(name=="quickslot_restore_packet")return self.quick_bridge.RestorePacket(argument);
    if(name=="quickslot_persist_ack")return argument>=0 && argument<3 && self.quick_bridge.AcknowledgeWrite(static_cast<size_t>(argument));
    if(name=="quick_equip" || name.starts_with("quick_assign_")) {
      if(!self.home_ready || self.ui_held || self.intent.kind!=Module::IntentKind::None || self.warp_open || self.stamp_kind)return false;
      const auto token=static_cast<uint64_t>(argument);if((token>>24)!=self.home_epoch)return false;
      const auto fresh=self.Read();if(!Module::SameOwnership(self.observed,fresh))return false;
      self.quick_slots.Sync(self.ShortcutSnapshot(fresh));
      const auto id=static_cast<uint16_t>(token);
      if(name=="quick_equip") {
        const auto slot=(token>>16)&255;
        if(self.home_page!=4 || slot>=QuickSlots::Count || self.quick_slots.Target(slot)!=id)return false;
        self.context=ReadContext(Reader(self.host),self.host.main_base,HostBuild(self.host));
        if(fresh.selector.equipped_valid && fresh.selector.equipped_native_id==id)return true;
        return self.EquipY(id,fresh);
      }
      if(!self.quick_restored_applied || self.home_page!=3 || ((token>>16)&255)!=3 || name.size()!=14 || name.back()<'0' || name.back()>'2')return false;
      // Notebooks only changes the slot's option. Y changes only when the slot
      // being replaced is the one currently on Y (owner, 2026-10-23).
      const auto slot=static_cast<size_t>(name.back()-'0');
      const bool was_y=self.quick_slots.active()==slot;
      if(!self.quick_slots.Assign(slot,id))return false;
      if(!was_y)return true;
      self.context=ReadContext(Reader(self.host),self.host.main_base,HostBuild(self.host));
      if(fresh.selector.equipped_valid && fresh.selector.equipped_native_id==id)return true;
      return self.EquipY(id,fresh);
    }
    if(name=="favorite_selected" || name=="equip_echo") {
      if(!self.home_ready || self.ui_held || self.home_page!=3)return false;
      const auto token=static_cast<uint64_t>(argument);
      if((token>>24)!=self.home_epoch || ((token>>16)&255)!=3 || static_cast<uint16_t>(token)!=self.inspected[3])return false;
      const auto value=self.Read();
      if(!Module::SameOwnership(self.observed,value))return false;
      self.context=ReadContext(Reader(self.host),self.host.main_base,HostBuild(self.host));
      if(name=="favorite_selected") {
        const auto favorite=Module::Favorite(value,static_cast<uint16_t>(token));if(!favorite)return false;
        return self.StartEchoIntent(Module::IntentKind::Favorite,static_cast<uint16_t>(token),!*favorite,value,self.host.get_tick(self.host.userdata));
      }
      return self.StartEchoIntent(Module::IntentKind::Equip,static_cast<uint16_t>(token),false,value,self.host.get_tick(self.host.userdata));
    }
    if (name=="current_echo") {
      // Y slot tap: open the Notebook on the currently set Echo. Inspection only.
      if (!self.home_ready || self.ui_held) return false;
      const auto ready=self.resources.Snapshot(); if (!ready) return false;
      const auto context=ReadContext(Reader(self.host),self.host.main_base,HostBuild(self.host));
      if (!Module::Normal(context)) return false;
      const auto value=self.Read();
      if (value.selector.held || !value.collection_valid || !value.selector.equipped_valid ||
          !self.Metadata(*ready,value,value.selector.equipped_native_id)) return false;
      self.notebook_automatons=IsAutomaton(value.selector.equipped_native_id);
      self.inspected[3]=value.selector.equipped_native_id; ++self.home_epoch;
      self.home_page=3; self.home_page_initialized=true; return true;
    }
    if(name=="notebook_category") {
      if(!self.home_ready || self.ui_held || self.home_page!=3 || self.intent.kind!=Module::IntentKind::None || argument<0 || argument>1)return false;
      const auto fresh=self.Read();if(!Module::SameOwnership(self.observed,fresh))return false;
      self.notebook_automatons=argument!=0;++self.home_epoch;self.inspected[3]=0xffff;return true;
    }
    if (name=="page" || name=="inspect") {
      if (!self.home_ready || self.ui_held) return false;
      const auto ready=self.resources.Snapshot(); if (!ready) return false;
      const auto context=ReadContext(Reader(self.host),self.host.main_base,HostBuild(self.host));
      if (!context.valid || (context.context!=NativeContext::NativeMenu &&
                            context.context!=NativeContext::FieldHud)) return false;
      const auto value=self.Read(); if (value.selector.held || !value.collection_valid) return false;
      const auto items=ReadInventory(Reader(self.host),self.host.main_base,HostBuild(self.host));
      if (name=="page") {
        if(argument==4) {
          // Home navigation is independent of map data availability. Terrain
          // widgets retain their separate discovery/source/coverage gates.
          self.home_page=4;self.home_page_initialized=true;return true;
        }
        if (argument<1 || argument>3 || ((argument==1 || argument==2) &&
            (!items.inventory_valid || !ready->inventory_ready))) return false;
        self.home_page=static_cast<int>(argument); self.home_page_initialized=true; return true;
      }
      if(self.home_page<1 || self.home_page>3)return false;
      const auto token=static_cast<uint64_t>(argument);
      if ((token>>24)!=self.home_epoch || ((token>>16)&255)!=static_cast<uint64_t>(self.home_page)) return false;
      if (self.home_page==3) {
        if (value.collection_source!=self.observed.collection_source ||
            value.learned_native_ids!=self.observed.learned_native_ids ||
            value.learned_crc_keys!=self.observed.learned_crc_keys) return false;
      } else if (!items.inventory_valid || !self.inventory.inventory_valid ||
                 !Module::SameInventory(items,self.inventory)) return false;
      const auto ids=self.InspectionList(*ready,value,items,self.home_page);
      const auto id=static_cast<uint16_t>(token&0xffff);
      if (std::find(ids.begin(),ids.end(),id)==ids.end()) return false;
      self.inspected[static_cast<size_t>(self.home_page)]=id; return true;
    }
    if (name!="select") return false;
    if (!self.ui_held || (static_cast<uint64_t>(argument)>>16)!=self.epoch) return false;
    const auto value=self.Read();
    if (!SameHold(self.observed,value) || !self.resources.Snapshot()) return false;
    if(!self.host.write_memory || self.intent.kind!=Module::IntentKind::None)return false;
    const auto fresh=self.Read();
    if(!SameHold(value,fresh))return false;
    const auto id=static_cast<uint16_t>(argument&0xffff);
    const bool written=self.JumpEcho(fresh,id);
    if(written) {
      self.direct_source=fresh;self.direct_target=id;
      self.direct_started=self.host.get_tick(self.host.userdata);self.direct_status=1;
    }
    self.I("echo.direct.requested",written?id:0xffff);
    self.I("echo.direct.written",written);
    return written;
  } catch (...) { self.drive.Cancel(); return false; }
}
bool Emit(const Image& image,void* receiver,EdenDsmodImageSink sink) {
  if (!sink || image.Empty()) return false;
  sink(receiver,static_cast<uint32_t>(image.w),static_cast<uint32_t>(image.h),
       image.rgba.data(),image.rgba.size()); return true;
}
EdenDsmodBool LoadImage(void* instance,const EdenDsmodHostApi*,const char* key,
                        void* receiver,EdenDsmodImageSink sink) {
  if (!instance || !key || !sink) return false;
  auto& self=*static_cast<Module*>(instance);
  try {
    std::string_view path(key); Image image;
    constexpr std::string_view quest_icon="module:echoes/map-icon/checkpoint/";
    if(path.starts_with(quest_icon)) {
      uint32_t frame{};const auto digits=path.substr(quest_icon.size());const auto result=std::from_chars(digits.data(),digits.data()+digits.size(),frame);
      return result.ec==std::errc{} && result.ptr==digits.data()+digits.size() && frame<700 &&
        self.map_icons.Checkpoint(frame,112,image) && Emit(image,receiver,sink);
    }
    if(path=="module:echoes/map-icon/chest/open" || path=="module:echoes/map-icon/chest/closed")
      return self.map_icons.Chest(path.ends_with("/open"),64,image) && Emit(image,receiver,sink);
    constexpr std::string_view entrance_icon="module:echoes/map-icon/entrance/";
    if(path.starts_with(entrance_icon)) {
      const auto ready=self.resources.Snapshot();if(!ready)return false;
      const auto name=path.substr(entrance_icon.size());
      if(std::none_of(ready->entrances.icons.begin(),ready->entrances.icons.end(),[&](const auto& row){return row.texture_name==name;}))return false;
      return self.map_icons.Landmark(name,40,image) && Emit(image,receiver,sink);
    }
    constexpr std::string_view landmark_icon="module:echoes/map-icon/landmark/";
    if(path.starts_with(landmark_icon)) {
      const auto ready=self.resources.Snapshot();if(!ready)return false;
      const auto name=path.substr(landmark_icon.size());
      if(std::none_of(ready->landmarks.begin(),ready->landmarks.end(),[&](const auto& row){return row.texture_name==name;}))return false;
      return self.map_icons.Smoothie(40,image) && Emit(image,receiver,sink);
    }
    if (path.starts_with("module:echoes/assets/"))
      return self.assets.Load(path.substr(21),image) && Emit(image,receiver,sink);
    constexpr std::string_view dungeon_prefix="module:echoes/dungeon/";
    if(path.starts_with(dungeon_prefix)) {
      const auto rest=path.substr(dungeon_prefix.size());
      const auto a=rest.find('/'),b=a==rest.npos?rest.npos:rest.find('/',a+1);
      if(a==rest.npos || b==rest.npos || b+2!=rest.size() || (rest[b+1]!='0' && rest[b+1]!='1'))return false;
      size_t slot{};uint64_t visited{};
      const auto index=std::from_chars(rest.data(),rest.data()+a,slot);
      const auto bits=std::from_chars(rest.data()+a+1,rest.data()+b,visited);
      if(!a || b==a+1 || index.ec!=std::errc{} || index.ptr!=rest.data()+a ||
         bits.ec!=std::errc{} || bits.ptr!=rest.data()+b || slot>=64)return false;
      Module::DungeonSlot source;
      std::shared_ptr<DungeonMapAssets> assets;
      {std::lock_guard lock(self.map_mutex);
        source=self.dungeon_views.slots()[slot];assets=self.dungeon_assets;
      }
      if(!assets || !source.used || source.visited!=visited || source.map_item!=(rest[b+1]=='1'))return false;
      const auto ready=self.resources.Snapshot();if(!ready)return false;
      const auto def=std::find_if(ready->dungeons.begin(),ready->dungeons.end(),
                                [&](const auto& row){return row.key==source.key;});
      if(def==ready->dungeons.end())return false;
      MapBounds bounds;DungeonComposeStatus status;
      std::lock_guard worker_lock(self.map_compose_mutex);
      if(!assets->Compose(*def,source.floor,visited,source.variants,image,bounds,&status,source.map_item) || image.Empty())return false;
      {std::lock_guard lock(self.map_mutex);self.dungeon_bounds[slot]=bounds;}
      // Aspect-preserving native room raster in the unchanged Map viewport.
      // Transparent margins participate in drag/pinch; no stretched room art.
      const double fit=std::min(double(MapW)/image.w,double(MapH)/image.h);
      const int w=std::clamp(int(std::lround(image.w*fit)),1,MapW),
                h=std::clamp(int(std::lround(image.h*fit)),1,MapH);
      Image padded(MapW,MapH);
      const int dx=(MapW-w)/2,dy=(MapH-h)/2;
      for(int y=0;y<h;++y)for(int x=0;x<w;++x)
        std::copy_n(image.At(std::min(image.w-1,x*image.w/w),std::min(image.h-1,y*image.h/h)),4,padded.At(dx+x,dy+y));
      return Emit(padded,receiver,sink);
    }
    constexpr std::string_view map_prefix="module:echoes/map/";
    if(path.starts_with(map_prefix)) {
      const auto rest=path.substr(map_prefix.size());const auto slash=rest.find('/');
      if(slash==std::string_view::npos || rest.size()<slash+133 || rest[slash+129]!='/' || rest[slash+131]!='/' ||
         rest[slash+130]<'0' || rest[slash+130]>'3')return false;
      const bool extensions_hidden=((rest[slash+130]-'0')&1)!=0;
      const bool sandstorm_hidden=((rest[slash+130]-'0')&2)!=0;
      uint32_t rift_mask{};const auto mask_text=rest.substr(slash+132);
      const auto mask_parse=std::from_chars(mask_text.data(),mask_text.data()+mask_text.size(),rift_mask);
      if(mask_text.empty() || mask_parse.ec!=std::errc{} || mask_parse.ptr!=mask_text.data()+mask_text.size() || rift_mask>=(uint32_t(1)<<29))return false;
      size_t slot{};const auto parsed=std::from_chars(rest.data(),rest.data()+slash,slot);
      if(!slash || parsed.ec!=std::errc{} || parsed.ptr!=rest.data()+slash || slot>=64)return false;
      std::array<uint8_t,64> bits{};
      const auto nibble=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
      for(size_t i=0;i<64;++i) {
        int a=nibble(rest[slash+1+i*2]),b=nibble(rest[slash+2+i*2]);
        if(a<0||b<0)return false;bits[i]=static_cast<uint8_t>((a<<4)|b);
      }
      std::shared_ptr<FieldMapAssets> assets;
      {
        std::lock_guard lock(self.map_mutex);
        if(!self.map_slots[slot].used || !self.map_assets || self.map_slots[slot].rifts!=rift_mask)return false;
        if(extensions_hidden && !self.map_slots[slot].field_extensions_hidden)return false;
        if(sandstorm_hidden && !self.map_slots[slot].sandstorm_hidden)return false;
        for(size_t i=0;i<64;++i)
          if((bits[i]&self.map_slots[slot].discovered[i])!=bits[i])return false;
        assets=self.map_assets;
      }
      MapBounds bounds;MapComposeStatus status;
      std::lock_guard worker_lock(self.map_compose_mutex);
      if(!assets->Compose(bits,image,bounds,&status,extensions_hidden,sandstorm_hidden) || image.w!=1272 || image.h!=916)return false;
      if(rift_mask) {
        const auto ready=self.resources.Snapshot();Image fill;
        if(!ready || !ready->player_ready || ready->rift_regions.size()>29 || !self.assets.Load("rift-fill",fill))return false;
        for(size_t i=0;i<ready->rift_regions.size();++i)if(rift_mask&(uint32_t(1)<<i)) {
          for(const auto& ring:ready->rift_regions[i].rings) {
            std::vector<std::array<double,2>> points;double lo_x=1272,hi_x=0,lo_y=916,hi_y=0;
            bool valid=true;
            for(const auto& point:ring) {
              PlayerWorldSnapshot world;world.compatible=world.valid=true;world.level_key="Field";world.xyz={point[0],0.f,point[1]};
              const auto at=ProjectFieldPlayer(ready->player,world);if(!at.valid){valid=false;break;}
              const double x=at.uv[0]*1272,y=at.uv[1]*916;points.push_back({x,y});
              lo_x=std::min(lo_x,x);hi_x=std::max(hi_x,x);lo_y=std::min(lo_y,y);hi_y=std::max(hi_y,y);
            }
            if(!valid || points.size()<3)continue;
            for(int y=std::max(0,static_cast<int>(std::floor(lo_y)));y<std::min(916,static_cast<int>(std::ceil(hi_y)));++y)
              for(int x=std::max(0,static_cast<int>(std::floor(lo_x)));x<std::min(1272,static_cast<int>(std::ceil(hi_x)));++x) {
                if(image.At(x,y)[3]!=255)continue;
                bool inside=false;const double px=x+.5,py=y+.5;
                for(size_t j=0,k=points.size()-1;j<points.size();k=j++) {
                  const auto& a=points[j];const auto& b=points[k];
                  if((a[1]>py)!=(b[1]>py) && px<(b[0]-a[0])*(py-a[1])/(b[1]-a[1])+a[0])inside=!inside;
                }
                if(!inside)continue;
                const auto* src=fill.At(x%fill.w,y%fill.h);auto* dst=image.At(x,y);
                const unsigned alpha=src[3];for(int c=0;c<3;++c)dst[c]=static_cast<uint8_t>((unsigned(src[c])*alpha+unsigned(dst[c])*(255-alpha)+127)/255);
              }
          }
        }
      }
      Image padded(MapRasterW,MapRasterH);
      for(int y=0;y<image.h;++y)std::copy_n(image.At(0,y),static_cast<size_t>(image.w)*4,padded.At(0,y+MapRasterPadY));
      image=std::move(padded);
      auto coverage=std::make_shared<Module::MapCoverage>();
      coverage->uri=path;coverage->width=image.w;coverage->height=image.h;
      coverage->alpha.resize(static_cast<size_t>(image.w)*static_cast<size_t>(image.h));
      for(size_t i=0;i<coverage->alpha.size();++i)coverage->alpha[i]=image.rgba[i*4+3];
      {
        std::lock_guard lock(self.map_mutex);
        if(self.map_ready && self.map_uri==path)self.map_coverage=std::move(coverage);
      }
      return Emit(image,receiver,sink);
    }
    const bool bootstrap_request=path.starts_with("module:echoes/bootstrap/");
    if(bootstrap_request && self.host.log)self.host.log(self.host.userdata,1,"Echoes bootstrap: building resources");
    const bool resources_ok=self.resources.EnsureReady();
    if(bootstrap_request && self.host.log){const auto phase=std::string("Echoes bootstrap: ")+self.resources.LastBuildPhase()+" attempts="+std::to_string(self.resources.BuildAttempts());self.host.log(self.host.userdata,1,phase.c_str());}
    if (!resources_ok) return false;
    const auto ready=self.resources.Snapshot(); if (!ready) return false;
    const auto locale=std::to_string(ready->locale_flag);
    const auto number=[](std::string_view digits,uint32_t& value) {
      if (digits.empty()) return false;
      const auto parsed=std::from_chars(digits.data(),digits.data()+digits.size(),value);
      return parsed.ec==std::errc{} && parsed.ptr==digits.data()+digits.size();
    };
    const auto text_line=[&](std::string_view text,int w,int h,bool center,bool right=false,
                            std::array<uint8_t,4> color=std::array<uint8_t,4>{244,240,217,255},
                            bool middle=true) {
      if (!RenderBody(ready->body,text,color,static_cast<uint32_t>(w),image) || image.h>h)
        return false;
      Image padded(w,h);
      const int x=center?(w-image.w)/2:(right?w-image.w:0);
      int first=image.h,last=-1;
      for(int row=0;row<image.h;++row)for(int col=0;col<image.w;++col)
        if(image.At(col,row)[3]){first=std::min(first,row);last=std::max(last,row);}
      const int y=center && middle && last>=first?(h-(last-first+1))/2-first:0;
      for (int row=0;row<image.h;++row)if(row+y>=0 && row+y<h)
        std::copy_n(image.At(0,row),static_cast<size_t>(image.w)*4,padded.At(x,y+row));
      return Emit(padded,receiver,sink);
    };
    // Uniformly scale native font masks, then apply one-pixel bold and outline.
    // Never stretch to a panel width; labels that cannot fit remain unavailable.
    const auto outlined_label=[&](std::string_view text,int w,int h,int cap,std::array<uint8_t,3> color=std::array<uint8_t,3>{243,229,198},Image* output=nullptr) {
      const auto& font=ready->body;
      const auto hi=uint32_t('H')-font.first_codepoint;
      if(uint32_t('H')<font.first_codepoint || hi>=font.glyphs.size() || !font.glyphs[hi].h ||
         !RenderBody(font,text,{255,255,255,255},4096,image))return false;
      const double scale=double(cap)/font.glyphs[hi].h;
      int top=image.h,bottom=-1;
      for(int y=0;y<image.h;++y)for(int x=0;x<image.w;++x)if(image.At(x,y)[3]) {
        top=std::min(top,y);bottom=std::max(bottom,y);
      }
      if(bottom<top)return false;
      const int sw=std::max(1,int(std::lround(image.w*scale))),
                sh=std::max(1,int(std::lround((bottom-top+1)*scale)));
      if(sw+4>w || sh+4>h)return false;
      Image mask(sw+1,sh),padded(w,h);
      for(int y=0;y<sh;++y)for(int x=0;x<sw;++x) {
        const double px=std::clamp((x+.5)/scale-.5,0.,double(image.w-1)),
                     py=std::clamp((y+.5)/scale-.5,0.,double(bottom-top));
        const int x0=int(px),y0=int(py)+top,x1=std::min(x0+1,image.w-1),y1=std::min(y0+1,bottom);
        const double fx=px-x0,fy=py-(y0-top);
        const auto alpha=uint8_t(std::lround((1-fy)*((1-fx)*image.At(x0,y0)[3]+fx*image.At(x1,y0)[3])+
                                             fy*((1-fx)*image.At(x0,y1)[3]+fx*image.At(x1,y1)[3])));
        mask.At(x,y)[3]=std::max(mask.At(x,y)[3],alpha);
        mask.At(x+1,y)[3]=std::max(mask.At(x+1,y)[3],alpha);
      }
      const int dx=(w-mask.w)/2,dy=(h-mask.h)/2;
      for(int y=-1;y<=mask.h;++y)for(int x=-1;x<=mask.w;++x) {
        uint8_t stroke=0;
        for(int oy=-1;oy<=1;++oy)for(int ox=-1;ox<=1;++ox)
          if(x+ox>=0 && x+ox<mask.w && y+oy>=0 && y+oy<mask.h)
            stroke=std::max(stroke,mask.At(x+ox,y+oy)[3]);
        const unsigned fill=x>=0&&x<mask.w&&y>=0&&y<mask.h?mask.At(x,y)[3]:0;
        auto* d=padded.At(x+dx,y+dy);const unsigned alpha=fill+stroke*(255-fill)/255;
        if(alpha) {
          for(int c=0;c<3;++c)d[c]=uint8_t((unsigned(color[c])*fill+alpha/2)/alpha);
          d[3]=uint8_t(alpha);
        }
      }
      if(output) {*output=std::move(padded);return true;}
      return Emit(padded,receiver,sink);
    };
    const auto title=[&](std::string_view text,std::array<uint8_t,4> color=std::array<uint8_t,4>{244,240,217,255}) {
      if (!RenderHeading(ready->heading,text,color,1144,image)) return false;
      for (int y=86;y<image.h;++y)
        for (int x=0;x<image.w;++x) if (image.At(x,y)[3]) return false;
      Image padded(1144,86);
      for (int y=0;y<std::min(image.h,86);++y)
        std::copy_n(image.At(0,y),static_cast<size_t>(image.w)*4,padded.At(0,y));
      return Emit(padded,receiver,sink);
    };
    const bool field_area=path.starts_with("module:echoes/map-heading-field/"+locale+"/");
    const auto area_prefix=std::string("module:echoes/")+(field_area?"map-heading-field/":"map-heading/")+locale+"/";
    if(path.starts_with(area_prefix)) {
      const auto area=std::find_if(ready->locations.begin(),ready->locations.end(),[&](const auto& row){return row.key==path.substr(area_prefix.size());});
      if(area==ready->locations.end() || area->name.empty())return false;
      Image glyphs;
      // Left-align current area within the map board, clear of header launchers.
      // Long area names use the native body face instead of disappearing.
      if(!RenderCompactHeading(ready->heading,area->name,{237,184,185,255},field_area?650:850,glyphs) &&
         !RenderBody(ready->body,area->name,{237,184,185,255},field_area?650:850,glyphs))return false;
      Image padded(930,64);int top=glyphs.h,bottom=-1;
      for(int y=0;y<glyphs.h;++y)for(int x=0;x<glyphs.w;++x)if(glyphs.At(x,y)[3]){top=std::min(top,y);bottom=std::max(bottom,y);}
      if(bottom<top || bottom-top+1>64)return false;
      const int dx=12,dy=(64-(bottom-top+1))/2-top;
      for(int y=0;y<glyphs.h;++y)if(y+dy>=0 && y+dy<64)std::copy_n(glyphs.At(0,y),static_cast<size_t>(glyphs.w)*4,padded.At(dx,y+dy));
      return Emit(padded,receiver,sink);
    }
    const auto reset_label="module:echoes/map-reset/"+locale;
    if(path==reset_label)return outlined_label("Reset",144,80,22,{243,229,198});
    const auto rupee_label="module:echoes/rupees/"+locale+"/";
    if(path.starts_with(rupee_label)) {
      uint32_t count{};if(!number(path.substr(rupee_label.size()),count) || count>MaxRupees)return false;
      Image label;if(!outlined_label(std::to_string(count),160,60,36,{243,229,198},&label))return false;
      int right=-1;for(int y=0;y<label.h;++y)for(int x=0;x<label.w;++x)if(label.At(x,y)[3])right=std::max(right,x);
      if(right<0)return false;Image aligned(160,60);const int shift=158-right;
      for(int y=0;y<label.h;++y)for(int x=0;x<label.w;++x)if(x+shift>=0 && x+shift<160)std::copy_n(label.At(x,y),4,aligned.At(x+shift,y));
      return Emit(aligned,receiver,sink);
    }
    constexpr std::string_view bootstrap="module:echoes/bootstrap/";
    if (path.starts_with(bootstrap)) {
      uint64_t generation{}; const auto digits=path.substr(bootstrap.size());
      const auto parsed=std::from_chars(digits.data(),digits.data()+digits.size(),generation);
      if (digits.empty() || parsed.ec!=std::errc{} || parsed.ptr!=digits.data()+digits.size()) return false;
      return Emit(Image(1,1),receiver,sink);
    }
    constexpr std::string_view icon_prefix="module:echoes/icon/";
    if (path.starts_with(icon_prefix)) {
      const auto actor=path.substr(icon_prefix.size());
      if (std::none_of(ready->echoes.begin(),ready->echoes.end(),
          [&](const auto& entry){return entry.actor==actor;}) && std::none_of(ready->automata.begin(),ready->automata.end(),[&](const auto& entry){return entry.actor==actor;})) return false;
      if (self.echo_textures.empty()) {
        constexpr char source[]="romfs:/region_common/ui/IconCopyActor.arc#timg/__Combined.bntx";
        const auto size=self.host.read_romfs(self.host.userdata,source,0,nullptr,0);
        if (!size || size>32*1024*1024) return false;
        std::vector<uint8_t> bytes(size);
        if (self.host.read_romfs(self.host.userdata,source,0,bytes.data(),size)!=size) return false;
        self.echo_textures=std::move(bytes);
      }
      return acnh::BntxDecode(self.echo_textures,actor,image) && Emit(image,receiver,sink);
    }
    const auto inspection_title="module:echoes/inspection-title/"+locale+"/";
    const auto inspection_description="module:echoes/inspection-description/"+locale+"/";
    if(path.starts_with(inspection_title) || path.starts_with(inspection_description)) {
      const bool title_image=path.starts_with(inspection_title);
      const auto rest=path.substr(title_image?inspection_title.size():inspection_description.size());
      const auto slash=rest.find('/');uint32_t key{};
      if(slash==std::string_view::npos || !number(rest.substr(slash+1),key))return false;
      const InspectionTextLayout* layout=nullptr;std::string_view name,description;const FontAtlas* description_font=&ready->body;
      if(rest.substr(0,slash)=="echo") {
        const auto entry=std::find_if(ready->echoes.begin(),ready->echoes.end(),[&](const auto& e){return e.crc_key==key;});
        const auto found=ready->inspection_echo_text.find(key);
        if(entry==ready->echoes.end() || found==ready->inspection_echo_text.end())return false;
        layout=&found->second;name=entry->name;description=entry->description;
      } else if(rest.substr(0,slash)=="automaton") {
        const auto entry=std::find_if(ready->automata.begin(),ready->automata.end(),[&](const auto& e){return e.crc_key==key;});
        const auto found=ready->inspection_automaton_text.find(key);
        if(entry==ready->automata.end() || found==ready->inspection_automaton_text.end())return false;
        layout=&found->second;name=entry->name;description=entry->description;
      } else if(rest.substr(0,slash)=="item" && ready->inventory_ready && key<ready->inventory.entries.size()) {
        const auto found=ready->inventory_text.find(key);if(found==ready->inventory_text.end())return false;
        layout=&found->second;name=ready->inventory.entries[key].name;description=ready->inventory.entries[key].description;
      } else if(rest.substr(0,slash)=="prologue" && key<ready->prologue.size()) {
        const auto found=ready->prologue_text.find(key);if(found==ready->prologue_text.end())return false;
        if(key>=ready->prologue_description_display.size())return false;
        layout=&found->second;name=ready->prologue[key].name;description=ready->prologue_description_display[key];description_font=&ready->prologue_body;
      } else return false;
      const uint32_t width=(rest.substr(0,slash)=="echo" || rest.substr(0,slash)=="automaton")?(title_image?580:930):InspectionWidth;
      if(title_image) {
        if(layout->use_body)
          return RenderBodyParagraph(width==580?ready->echo_name_body:ready->hero_body,name,{237,184,185,255},width,layout->title_h,image) && Emit(image,receiver,sink);
        image=Image(width,static_cast<int>(layout->title_h));int y=0;
        for(const auto& line:layout->heading_lines) {
          Image glyphs;if(!RenderCompactHeading(ready->heading,line,{237,184,185,255},width,glyphs))return false;
          for(int row=0;row<glyphs.h;++row)for(int x=0;x<glyphs.w;++x) {
            const auto* pixel=glyphs.At(x,row);if(!pixel[3])continue;
            if(y+row>=image.h)return false;
            std::copy_n(pixel,4,image.At(x,y+row));
          }
          y+=72;
        }
        return Emit(image,receiver,sink);
      }
      image=Image(width,1024);
      if(!description.empty()) {
        Image lines;
        if(!RenderBodyParagraph(*description_font,InspectionDescription(description),{255,244,219,255},width,layout->description_h,lines))return false;
        for(int y=0;y<lines.h;++y)std::copy_n(lines.At(0,y),static_cast<size_t>(lines.w)*4,image.At(0,y));
      }
      return Emit(image,receiver,sink);
    }
    const auto held_name=std::string(path.starts_with("module:echoes/held-automaton/")?"module:echoes/held-automaton/":"module:echoes/held-name/")+locale+"/";
    if(path.starts_with(held_name)) {
      uint32_t crc{};if(!number(path.substr(held_name.size()),crc))return false;
      const auto& entries=path.starts_with("module:echoes/held-automaton/")?ready->automata:ready->echoes;
      const auto entry=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.crc_key==crc;});
      if(entry==entries.end() || ready->hero_body.image.Empty() ||
         !RenderBody(ready->hero_body,entry->name,{255,244,219,255},1144,image) || image.h>64)return false;
      Image padded(1144,64);const int x=(1144-image.w)/2,y=(64-image.h)/2;
      for(int row=0;row<image.h;++row)std::copy_n(image.At(0,row),static_cast<size_t>(image.w)*4,padded.At(x,y+row));
      return Emit(padded,receiver,sink);
    }
    const auto inspection_count="module:echoes/inspection-count/"+locale+"/";
    if(path.starts_with(inspection_count)) {
      const auto rest=path.substr(inspection_count.size());const auto slash=rest.find('/');uint32_t index{},count{};
      if(slash==std::string_view::npos || !number(rest.substr(0,slash),index) || !number(rest.substr(slash+1),count) ||
         !index || index>count || count>512)return false;
      return text_line(std::to_string(index)+" / "+std::to_string(count),460,40,false,true,{243,217,157,255});
    }
    const bool quick_control=path.starts_with("module:echoes/control202/"+locale+"/");
    const bool popup_control=path.starts_with("module:echoes/control236/"+locale+"/");
    const bool compact_control=path.starts_with("module:echoes/control268/"+locale+"/");
    const auto control_prefix=std::string("module:echoes/")+(quick_control?"control202/":popup_control?"control236/":compact_control?"control268/":"control/")+locale+"/";
    if(path.starts_with(control_prefix)) {
      const auto key=std::string(path.substr(control_prefix.size()));
      if(key!="MapWarp_Dialog_00" && key!="Operation_Return" && key!="DressUpMenu_Dialog_Equipment" && key!="DressUpMenu_Dialog_Remove" && key!="ItemMenu_Dialog_Eat" && key!="ItemMenu_Dialog_Drink")return false;
      const auto found=ready->ui.find(key);
      if(!popup_control && !compact_control && key=="Operation_Return")
        return found!=ready->ui.end() && outlined_label(found->second,381,100,34);
      return found!=ready->ui.end() && text_line(found->second,quick_control?202:popup_control?236:compact_control?268:320,48,true,false,(!popup_control && !compact_control && key=="Operation_Return")?std::array<uint8_t,4>{243,229,198,255}:std::array<uint8_t,4>{48,46,61,255});
    }
    const auto floor_prefix="module:echoes/dungeon-floor/"+locale+"/";
    if(path.starts_with(floor_prefix)) {
      const auto found=ready->dungeon_floor_labels.find(std::string(path.substr(floor_prefix.size())));
      return found!=ready->dungeon_floor_labels.end() &&
        text_line(found->second,236,48,true,false,{48,46,61,255},true);
    }
    const auto nav_prefix="module:echoes/nav/"+locale+"/";
    if (path.starts_with(nav_prefix)) {
      uint32_t index{};
      constexpr std::array<std::string_view,4> keys{"MapMenu","DressUpMenu","ItemMenu","Dictionary"};
      if (!number(path.substr(nav_prefix.size()),index) || index>=keys.size()) return false;
      const auto found=ready->ui.find(std::string(keys[index]));
      // Cream native glyphs centered inside the owner-selected slate footer.
      return found!=ready->ui.end() && outlined_label(index==3 && (ready->locale_flag==2 || ready->locale_flag==32)?"Notebooks":found->second,381,100,34);
    }
    const auto notebook_tab="module:echoes/notebook-tab/"+locale+"/";
    if(path.starts_with(notebook_tab)) {
      const auto rest=path.substr(notebook_tab.size());const auto slash=rest.find('/');uint32_t category{},selected{};
      if(slash==rest.npos || !number(rest.substr(0,slash),category) || category>1 || !number(rest.substr(slash+1),selected) || selected>1)return false;
      const auto key=category?"Dictionary_Machine":"Dictionary_Karimono";
      const auto at=ready->ui.find(key);if(at==ready->ui.end())return false;
      std::string text=at->second;
      if((ready->locale_flag==2 || ready->locale_flag==32) && !text.empty() && text[0]>='a' && text[0]<='z')text[0]-='a'-'A';
      return outlined_label(text,220,112,24,selected?std::array<uint8_t,3>{48,46,61}:std::array<uint8_t,3>{243,229,198});
    }
    const auto sort_tab_prefix="module:echoes/sort-tab/"+locale+"/";
    if(path.starts_with(sort_tab_prefix)) {
      const auto rest=path.substr(sort_tab_prefix.size());const auto slash=rest.find('/');
      uint32_t index{},selected{};
      if(slash==rest.npos || !number(rest.substr(0,slash),index) || index>=5 ||
         !number(rest.substr(slash+1),selected) || selected>1)return false;
      const auto found=ready->ui.find("ActorSelect_Sort_0"+std::to_string(index));
      if(found==ready->ui.end())return false;
      const auto color=selected?std::array<uint8_t,3>{48,46,61}:std::array<uint8_t,3>{243,229,198};
      const auto split=found->second.find(' ');
      if(split!=std::string::npos) {
        Image first,last;
        if(!outlined_label(std::string_view(found->second).substr(0,split),192,36,26,color,&first) ||
           !outlined_label(std::string_view(found->second).substr(split+1),192,36,26,color,&last))return false;
        Image two(192,112);
        for(int y=0;y<36;++y) {
          std::copy_n(first.At(0,y),192*4,two.At(0,y+20));
          std::copy_n(last.At(0,y),192*4,two.At(0,y+56));
        }
        return Emit(two,receiver,sink);
      }
      return outlined_label(found->second,192,112,26,color);
    }
    const auto sort_prefix="module:echoes/sort/"+locale+"/";
    if (path.starts_with(sort_prefix)) {
      uint32_t value{};
      if (!number(path.substr(sort_prefix.size()),value) || !std::has_single_bit(value) || value>16) return false;
      const auto key="ActorSelect_Sort_0"+std::to_string(std::countr_zero(value));
      const auto found=ready->ui.find(key);
      if(found==ready->ui.end() || !RenderBody(ready->body,found->second,{255,255,255,255},519,image))return false;
      Image padded(519,70);int first=image.h,last=-1;
      for(int y=0;y<image.h;++y)for(int x=0;x<image.w;++x)if(image.At(x,y)[3]){first=std::min(first,y);last=std::max(last,y);}
      if(last<first || last-first+1>70)return false;
      const int dx=519-image.w,dy=(70-(last-first+1))/2-first;
      for(int y=0;y<image.h;++y)if(y+dy>=0 && y+dy<70)std::copy_n(image.At(0,y),size_t(image.w)*4,padded.At(dx,y+dy));
      return Emit(padded,receiver,sink);
    }
    const auto category_prefix="module:echoes/category/"+locale+"/";
    if (path.starts_with(category_prefix)) {
      const auto rest=path.substr(category_prefix.size()); const auto slash=rest.find('/');
      uint32_t page{},equipped{};
      if (slash==std::string_view::npos || !number(rest.substr(0,slash),page) ||
          !number(rest.substr(slash+1),equipped) || page<1 || page>3 || equipped>1) return false;
      constexpr std::array<std::string_view,3> keys{"DressUpMenu","ItemMenu","Dictionary_Karimono"};
      const auto found=ready->ui.find(std::string(keys[page-1]));
      if (found==ready->ui.end()) return false;
      std::string text=found->second;
      if (page==3 && (ready->locale_flag==2 || ready->locale_flag==32) && !text.empty() && text[0]>='a' && text[0]<='z')text[0]-='a'-'A';
      if (page==1 && equipped && (ready->locale_flag==2 || ready->locale_flag==32)) text+=" / Equipped";
      return text_line(text,460,48,false,false,{237,184,185,255});
    }
    const auto quantity_prefix="module:echoes/quantity/"+locale+"/";
    if (path.starts_with(quantity_prefix)) {
      uint32_t quantity{};
      if (!number(path.substr(quantity_prefix.size()),quantity) || !quantity || quantity>99) return false;
      if (!RenderBody(ready->body,std::to_string(quantity),{244,240,217,255},96,image) || image.h>44) return false;
      Image padded(100,48);
      const int x=98-image.w,y=2;
      // The game's quantity role uses a dark stroke over colorful item art.
      // Preserve the native glyph pixels; add only the surrounding contrast edge.
      for (int row=0;row<image.h;++row) for (int col=0;col<image.w;++col) {
        const auto alpha=image.At(col,row)[3];
        if (!alpha) continue;
        for (int dy=-2;dy<=2;++dy) for (int dx=-2;dx<=2;++dx) {
          auto* dst=padded.At(x+col+dx,y+row+dy);
          dst[0]=26;dst[1]=22;dst[2]=18;dst[3]=std::max(dst[3],alpha);
        }
      }
      for (int row=0;row<image.h;++row) for (int col=0;col<image.w;++col)
        if (image.At(col,row)[3]) std::copy_n(image.At(col,row),4,padded.At(x+col,y+row));
      return Emit(padded,receiver,sink);
    }
    const auto map_label="module:echoes/map-label/"+locale+"/";
    if(path.starts_with(map_label)) {
      uint32_t index{};
      if(!number(path.substr(map_label.size()),index) || index>=ready->map_labels.size())return false;
      Image text;
      if(!RenderBody(ready->map_body,ready->map_labels[index].name,{255,244,219,255},456,text) || text.h>60)return false;
      image=Image(460,64);const int ox=(460-text.w)/2,oy=(64-text.h)/2;
      for(int pass=0;pass<2;++pass)for(int y=0;y<text.h;++y)for(int x=0;x<text.w;++x) {
        const auto* src=text.At(x,y);if(!src[3])continue;const int radius=pass==0?1:0;
        for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx) {
          auto* dst=image.At(ox+x+dx,oy+y+dy);const unsigned a=src[3];
          for(int c=0;c<3;++c) {
            const unsigned color=pass==0?(c==0?35:c==1?29:22):src[c];
            dst[c]=static_cast<uint8_t>((color*a+unsigned(dst[c])*(255-a)+127)/255);
          }
          dst[3]=static_cast<uint8_t>(a+(unsigned(dst[3])*(255-a)+127)/255);
        }
      }
      return Emit(image,receiver,sink);
    }
    const auto prologue_icon="module:echoes/prologue-icon/"+locale+"/";
    if(path.starts_with(prologue_icon)) {
      uint32_t index{};
      if(!number(path.substr(prologue_icon.size()),index) || index>=ready->prologue.size())return false;
      if(self.item_textures.empty()) {
        constexpr char source[]="romfs:/region_common/ui/IconItem.arc#timg/__Combined.bntx";
        const auto size=self.host.read_romfs(self.host.userdata,source,0,nullptr,0);
        if(!size || size>32*1024*1024)return false;
        std::vector<uint8_t> bytes(size);
        if(self.host.read_romfs(self.host.userdata,source,0,bytes.data(),size)!=size)return false;
        self.item_textures=std::move(bytes);
      }
      return acnh::BntxDecode(self.item_textures,ready->prologue[index].texture,image) && Emit(image,receiver,sink);
    }
    const auto item_icon_prefix="module:echoes/item-icon/"+locale+"/";
    const auto item_heading_prefix="module:echoes/item-heading/"+locale+"/";
    if (path.starts_with(item_icon_prefix) || path.starts_with(item_heading_prefix)) {
      const bool heading=path.starts_with(item_heading_prefix);
      const auto& prefix=heading?item_heading_prefix:item_icon_prefix;
      uint32_t index{};
      if (!ready->inventory_ready || !number(path.substr(prefix.size()),index) ||
          index>=ready->inventory.entries.size()) return false;
      const auto& entry=ready->inventory.entries[index];
      if (heading) return ready->heading_inventory_indices.contains(index) && title(entry.name,{237,184,185,255});
      const std::string_view source=entry.kind==InventoryKind::Ingredient
          ?"romfs:/region_common/ui/IconSmoothie.arc#timg/__Combined.bntx"
          :"romfs:/region_common/ui/IconItem.arc#timg/__Combined.bntx";
      if (!entry.texture.starts_with(std::string(source)+"#")) return false;
      const auto texture=entry.texture.substr(source.size()+1);
      auto& textures=entry.kind==InventoryKind::Ingredient?self.ingredient_textures:self.item_textures;
      if (textures.empty()) {
        const auto size=self.host.read_romfs(self.host.userdata,source.data(),0,nullptr,0);
        if (!size || size>32*1024*1024) return false;
        std::vector<uint8_t> bytes(size);
        if (self.host.read_romfs(self.host.userdata,source.data(),0,bytes.data(),size)!=size) return false;
        textures=std::move(bytes);
      }
      return acnh::BntxDecode(textures,texture,image) && Emit(image,receiver,sink);
    }
    const auto detail_prefix="module:echoes/detail/"+locale+"/";
    if (path.starts_with(detail_prefix)) {
      const auto rest=path.substr(detail_prefix.size()); const auto slash=rest.find('/');
      uint32_t id{};
      if (slash==std::string_view::npos || !number(rest.substr(slash+1),id)) return false;
      std::string_view text;
      if (rest.substr(0,slash)=="echo") {
        const auto entry=std::find_if(ready->echoes.begin(),ready->echoes.end(),[id](const auto& e){return e.crc_key==id;});
        if (entry==ready->echoes.end()) return false;
        text=entry->description;
      } else if (rest.substr(0,slash)=="item" && ready->inventory_ready && id<ready->inventory.entries.size())
        text=ready->inventory.entries[id].description;
      else return false;
      if (text.empty()) return Emit(Image(1144,154),receiver,sink);
      return RenderBodyParagraph(ready->body,InspectionDescription(text),{244,240,217,255},1144,154,image) && Emit(image,receiver,sink);
    }
    const auto count_prefix="module:echoes/count/"+std::to_string(ready->locale_flag)+"/";
    const auto sort_count="module:echoes/sort-count/"+locale+"/";
    if(path.starts_with(sort_count)) {
      const auto numbers=path.substr(sort_count.size());const auto slash=numbers.find('/');
      uint32_t index{},count{};
      if(slash==numbers.npos || !number(numbers.substr(0,slash),index) ||
         !number(numbers.substr(slash+1),count) || !index || index>count || count>MaxLearnedKeys)return false;
      return text_line(std::to_string(index)+" / "+std::to_string(count),238,64,false,true,{243,229,198,255});
    }
    const auto instruction="module:echoes/release/"+std::to_string(ready->locale_flag);
    const auto choosing="module:echoes/choosing/"+std::to_string(ready->locale_flag);
    std::string body_text; bool right_align=false;
    if (path.starts_with(count_prefix)) {
      const auto numbers=path.substr(count_prefix.size());
      const auto slash=numbers.find('/');
      if (slash==std::string_view::npos) return false;
      uint32_t index{},count{};
      const auto first=std::from_chars(numbers.data(),numbers.data()+slash,index);
      const auto last=std::from_chars(numbers.data()+slash+1,numbers.data()+numbers.size(),count);
      if (first.ec!=std::errc{} || first.ptr!=numbers.data()+slash || last.ec!=std::errc{} ||
          last.ptr!=numbers.data()+numbers.size() || !index || index>count || count>MaxLearnedKeys)
        return false;
      body_text=std::to_string(index)+" / "+std::to_string(count);right_align=true;
    } else if ((path==instruction || path==choosing) && (ready->locale_flag==2 || ready->locale_flag==32)) {
      body_text=path==choosing?"Choosing...":"Release to select"; right_align=false;
    }
    if (!body_text.empty()) {
      if (!RenderBody(ready->body,body_text,{244,240,217,255},400,image) || image.h>48) return false;
      Image padded(400,48); const int x=right_align?400-image.w:(400-image.w)/2;
      for (int y=0;y<image.h;++y)
        std::copy_n(image.At(0,y),static_cast<size_t>(image.w)*4,padded.At(x,y));
      return Emit(padded,receiver,sink);
    }
    constexpr std::string_view atlas="module:echoes/body-atlas?p=";
    if (path.starts_with(atlas)) {
      uint32_t page{}; const auto digits=path.substr(atlas.size());
      const auto parsed=std::from_chars(digits.data(),digits.data()+digits.size(),page);
      if (parsed.ec!=std::errc{} || parsed.ptr!=digits.data()+digits.size() ||
          page>=static_cast<uint32_t>(ready->body.image.h/512)) return false;
      image=Image(ready->body.image.w,512);
      std::copy_n(ready->body.image.At(0,static_cast<int>(page)*512),image.rgba.size(),image.rgba.data());
      return Emit(image,receiver,sink);
    }
    const auto prefix="module:echoes/heading/"+std::to_string(ready->locale_flag)+"/";
    if (!path.starts_with(prefix)) return false;
    uint32_t crc{}; const auto digits=path.substr(prefix.size());
    const auto parsed=std::from_chars(digits.data(),digits.data()+digits.size(),crc);
    if (parsed.ec!=std::errc{} || parsed.ptr!=digits.data()+digits.size() ||
        !ready->heading_echo_keys.contains(crc)) return false;
    const auto entry=std::find_if(ready->echoes.begin(),ready->echoes.end(),
        [crc](const auto& e){return e.crc_key==crc;});
    if (entry==ready->echoes.end() ||
        !RenderHeading(ready->heading,entry->name,{244,240,217,255},1144,image))
      return false;
    // FINF line feed is 94, while the original glyph cells are 85 high.
    // Remove only transparent line-spacing rows; never crop ink or resize it.
    for (int y=86;y<image.h;++y)
      for (int x=0;x<image.w;++x) if (image.At(x,y)[3]) return false;
    Image padded(1144,86);
    for (int y=0;y<std::min(image.h,86);++y)
      std::copy_n(image.At(0,y),static_cast<size_t>(image.w)*4,padded.At(0,y));
    return Emit(padded,receiver,sink);
  } catch (...) { return false; }
}
EdenDsmodBool LoadData(void* instance,const EdenDsmodHostApi*,const char* key,
                       void* receiver,EdenDsmodDataSink sink) {
  if (!instance || !key || !sink || std::string_view(key)!="module:echoes/body-font") return false;
  try {
    auto& self=*static_cast<Module*>(instance);
    if (!self.resources.Snapshot()) return false;
    constexpr uint8_t marker[]{'E','C','H','O','B','O','D','Y','1'};
    sink(receiver,marker,sizeof(marker)); return true;
  } catch (...) { return false; }
}
EdenDsmodBool DecodeFont(void* instance,const uint8_t* bytes,size_t size,
                         void* receiver,EdenDsmodFontSink sink) {
  constexpr char marker[]="ECHOBODY1";
  if (!instance || !sink || !bytes || size!=9 || std::memcmp(bytes,marker,9)) return false;
  try {
    auto& self=*static_cast<Module*>(instance);
    const auto ready=self.resources.Snapshot(); if (!ready) return false;
    const auto& body=ready->body;
    sink(receiver,body.line_height,body.first_codepoint,body.glyphs.data(),
         static_cast<uint32_t>(body.glyphs.size())); return true;
  } catch (...) { return false; }
}
const EdenDsmodModuleApi Api{
    EDEN_DSMOD_MODULE_ABI_VERSION,sizeof(EdenDsmodModuleApi),0,EDEN_DSMOD_MODULE_ABI_HASH,
    TitleId,"Echoes of Wisdom",EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN|EDEN_DSMOD_CAP_ROMFS_READ|
    EDEN_DSMOD_CAP_EXTENSIONS|EDEN_DSMOD_CAP_FONT_EPOCH|EDEN_DSMOD_CAP_WRITE_MEMORY,Supports,Create,Destroy,Sample,Tick};
const EdenDsmodModuleExtensions Extensions{EDEN_DSMOD_EXT_VERSION,sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH,Configure,Action,LoadImage};
const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,sizeof(EdenDsmodFontExtensions),
    EDEN_DSMOD_FONT_EXT_HASH,DecodeFont};
const EdenDsmodModuleDataExtensions DataExtensions{EDEN_DSMOD_DATA_EXT_VERSION,
    sizeof(EdenDsmodModuleDataExtensions),EDEN_DSMOD_DATA_EXT_HASH,LoadData};
} // namespace
} // namespace echoes
DSMOD_SDK_EXPORT_MODULE(echoes::Api)
DSMOD_SDK_EXPORT_EXTENSIONS(echoes::Extensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(echoes::FontExtensions)
DSMOD_SDK_EXPORT_DATA_EXTENSIONS(echoes::DataExtensions)
