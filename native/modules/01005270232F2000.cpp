// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Normal Formation, exploration and battle pages, Switch 1.0.5. Native
// game-thread helper provides fresh computed stats and leased minimap visibility.
// Optional moved battle-HUD writes use Slate opacity:
// the verified minimap container and battle party rows (ot0_hud.h),
// plus moving the battle command group up into the freed top-right space (Slate
// render translation), expect-checked, restored when the setting is turned off,
// the battle ends or the companion screen is hidden.
#include "core/mods/modules/dsmod_module_sdk.h"
#include "ot0_appearance.h"
#include "ot0_battle.h"
#include "ot0_battle_selection.h"
#include "ot0_battle_stats.h"
#include "ot0_battle_status.h"
#include "ot0_companion_helper.h"
#include "ot0_heal.h"
#include "ot0_exploration.h"
#include "ot0_equipment.h"
#include "ot0_hud.h"
#include "ot0_results.h"
#include "ot0_field_stats.h"
#include "ot0_gear.h"
#include "ot0_minimap.h"
#include "ot0_pak.h"
#include "ot0_reader.h"
#include "ot0_resources.h"
#include "ot0_enemy_stats.h"
#include "ot0_screen.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <deque>
#include <mutex>
#include <string>

namespace {
constexpr std::uint64_t Title = 0x01005270232F2000ULL;
constexpr std::string_view Build =
    "9FE60A212CFF9B9A3656AB63D4F882E500000000000000000000000000000000";
constexpr std::string_view Prefix = "module:ot0:screen/";
constexpr std::string_view SettingsPrefix = "module:ot0:settings/";
std::string BuildHex(const EdenDsmodHostApi &h) {
  const char *digits = "0123456789ABCDEF";
  std::string out;
  for (auto b : h.build_id) {
    out += digits[b >> 4];
    out += digits[b & 15];
  }
  return out;
}
struct View {
  std::uint64_t generation{};
  ot0::Snapshot party;
  ot0::LastBattle last_battle;
  ot0::MemberStats field_stats;
  int party_tab{};   // Party page: 0 Stats, 1 Equipment
  ot0::Gear gear;    // the selected member's equipped items (Equipment tab)
  bool items_page{}; // footer Items page (native start-menu Items)
  int items_tab{}, items_first{}, items_last_first{4096};
  ot0::Inventory inventory;
  bool quests_page{}; // footer Quests page (native start-menu Quests)
  ot0::QuestStates quests;
  int selected{-1};
  ot0::ExplorationSnapshot map;
  ot0::MinimapIcons icons;
  std::optional<ot0::ProtagonistAppearance> appearance;
  bool map_page{};
  ot0::BattleSnapshot battle;
  int focus{};
  ot0::BattleSelection selection;
  ot0::BattleStatsSnapshot stats;
  ot0::BattleStatusSnapshot statuses;
  bool automatic_details{true};
  bool modal_active{};
  bool battle_layout_known{};
  ot0::ShopComparisonSnapshot shop;
  ot0::HealState heal;
  bool heal_open{}, heal_available{};
};
// The page picture depends on the map's revealed area and cell, never on the exact position:
// markers are runtime overlays positioned every tick (no page re-render while walking).
// The page frames the map; it changes with the map's identity only (not position or reveal).
ot0::ExplorationSnapshot PageIdentity(const ot0::ExplorationSnapshot &m) {
  ot0::ExplorationSnapshot k;
  k.valid = m.valid; k.state = m.state; k.map_id = m.map_id; k.map_name_id = m.map_name_id;
  k.map_key = m.map_key; k.section_key = m.section_key; k.texture_path = m.texture_path;
  return k;
}
ot0::ExplorationSnapshot PageMap(ot0::ExplorationSnapshot m) {
  if (m.valid && m.block_width) {
    const auto cell = ot0::WorldCell(m, m.world_xyz[0], m.world_xyz[1]);
    m.texture_position = {double(cell.x), double(cell.y)};
  }
  m.world_xyz = {};
  m.heading_valid = false;
  m.native_direction = 0;
  return m;
}
// The map picture only changes with the revealed area / section / art, not with the player's
// cell: a rebuild per cell step meant a reload (and a chance of a blank frame) every few steps.
ot0::ExplorationSnapshot PictureMap(ot0::ExplorationSnapshot m) {
  m = PageMap(std::move(m));
  m.texture_position = {};
  m.state = 0;
  return m;
}
bool SameIconSet(const ot0::MinimapIcons &a, const ot0::MinimapIcons &b) {
  if (a.valid != b.valid || a.complete != b.complete || a.map_id != b.map_id ||
      a.player_color != b.player_color || a.icons.size() != b.icons.size())
    return false;
  for (std::size_t i = 0; i < a.icons.size(); ++i)
    if (a.icons[i].frame_path != b.icons[i].frame_path ||
        a.icons[i].background_frame_path != b.icons[i].background_frame_path ||
        a.icons[i].color != b.icons[i].color || a.icons[i].quest != b.icons[i].quest)
      return false;
  return true;
}
std::string IconKey(const ot0::MinimapIcon &icon) {
  std::uint64_t hash = 14695981039346656037ULL;
  const auto mix = [&](const void *p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) { hash ^= static_cast<const unsigned char *>(p)[i]; hash *= 1099511628211ULL; }
  };
  mix(icon.frame_path.data(), icon.frame_path.size());
  mix(icon.background_frame_path.data(), icon.background_frame_path.size());
  mix(icon.color.data(), sizeof icon.color);
  mix(icon.background_color.data(), sizeof icon.background_color);
  mix(&icon.opacity, sizeof icon.opacity);
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(hash));
  return std::string("module:ot0:icon/") + buf;
}
// Map pane picture rectangle (TownMapLayout inner area) and the default ("home") view: about the
// native top minimap's span around the player; the user can pinch/drag, the view glides back.
// 570 texels = the native minimap circle diameter measured in the Wishvale house (2026-10-31).
constexpr double MapW = 704, MapH = 772; // map widget world = the pane picture, y up
constexpr double MapHomeTexels = 570.0, MapMaxZoom = 8.0;
constexpr int MapMarkers = 32, MapRings = 8;
// Publish keys for the fixed-count rows, built once: Sample() runs every frame.
struct PublishKeys {
  template <std::size_t N, std::size_t S>
  static std::array<std::array<std::string, S>, N> Make(const char *stem,
                                                       const std::array<const char *, S> &suffix) {
    std::array<std::array<std::string, S>, N> out;
    for (std::size_t i = 0; i < N; ++i)
      for (std::size_t j = 0; j < S; ++j)
        out[i][j] = stem + std::to_string(i) + suffix[j];
    return out;
  }
  std::array<std::array<std::string, 3>, 5> foe = Make<5, 3>("ot0.foe", {".on", ".y", ".enemy"});
  std::array<std::array<std::string, 6>, 45> fx =
      Make<45, 6>("ot0.fx", {".on", ".src", ".player", ".x", ".y", ".scale"});
  std::array<std::array<std::string, 3>, 8> p = Make<8, 3>("ot0.p", {".on", ".x", ".y"});
  std::array<std::array<std::string, 2>, 8> mp = Make<8, 2>("ot0.mp", {".on", ".y"});
  std::array<std::array<std::string, 5>, MapMarkers> mk =
      Make<MapMarkers, 5>("ot0.mk", {".on", ".src", ".x", ".y", ".scale"});
  std::array<std::array<std::string, 4>, MapRings> rg =
      Make<MapRings, 4>("ot0.rg", {".on", ".tint", ".x", ".y"});
};
class Module {
public:
  explicit Module(const EdenDsmodHostApi &h) : cleanup_host(h) {}
  ~Module() {
    // Reload/disable can destroy the instance while its native widgets still
    // exist. Restore owned values before discarding their identities.
    try {
      native_helper.Stop();
      native_heal.Stop();
      std::lock_guard lock(hud_mutex);
      if(!cleanup_host.read_memory || !write_api.write_batch)return;
      const ot0::ReadMemory read=[&](auto address,void* out,std::size_t n) {
        return cleanup_host.read_memory(cleanup_host.userdata,address,out,n)==EDEN_DSMOD_TRUE;
      };
      const ot0::WriteF32 write=[&](std::uint64_t address,float expect,float value) {
        const EdenDsmodWriteOp op{address,4,0,&expect,&value};
        return write_api.write_batch(write_api.userdata,&op,1)==EDEN_DSMOD_TRUE;
      };
      const auto delta=cleanup_host.get_i64?
        cleanup_host.get_i64(cleanup_host.userdata,"__relocation_delta",0):0;
      const ot0::WriteBlock block = [&](auto address, const void* expect, const void* value, std::size_t n) {
        const EdenDsmodWriteOp op{address, static_cast<uint32_t>(n), 0, expect, value};
        return write_api.write_batch(write_api.userdata, &op, 1) == EDEN_DSMOD_TRUE;
      };
      command.Restore(read,block,cleanup_host.main_base,delta);
      hud.Restore(read,write,cleanup_host.main_base,delta);
    } catch(...) {
      // A failing host callback must never make module teardown terminate.
    }
  }
  // Between full samples: only the player's world position (map manager +0x348 block, the same
  // fields ReadExploration validates), so the map marker moves every frame instead of every 6th.
  void SamplePosition(const EdenDsmodHostApi &h) {
    if (!h.read_memory)
      return;
    const auto rd = [&](std::uint64_t a, void *out, std::size_t n) {
      return h.read_memory(h.userdata, a, out, n) == EDEN_DSMOD_TRUE;
    };
    std::uint64_t mm{}, vt{};
    std::array<std::uint8_t, 0x28> m{};
    const auto delta = h.get_i64 ? h.get_i64(h.userdata, "__relocation_delta", 0) : 0;
    if (!rd(h.main_base + 0x7cc7588, &mm, 8) || !mm || !rd(mm, &vt, 8) ||
        !ot0::MatchesVtable(vt, h.main_base + 0x63be0f0, delta) || !rd(mm + 0x348, m.data(), m.size()))
      return;
    std::uint32_t map_id{};
    std::array<double, 3> xyz{};
    std::memcpy(&map_id, m.data(), 4);
    std::memcpy(xyz.data(), m.data() + 0x10, 24);
    if (!std::all_of(xyz.begin(), xyz.end(), [](double v) { return std::isfinite(v) && std::abs(v) < 1e9; }))
      return;
    std::lock_guard lock(view_mutex);
    if (current.map.valid && current.map.map_id == map_id && current.map_page && current.party.state == 23)
      current.map.world_xyz = xyz;
  }
  void Sample(const EdenDsmodHostApi &h) {
    if (samples % 6 != 0)
      SamplePosition(h);
    if (samples++ % 6 == 0) {
      const ot0::ReadMemory read = [&](auto address, void *out, std::size_t n) {
        return h.read_memory &&
               h.read_memory(h.userdata, address, out, n) == EDEN_DSMOD_TRUE;
      };
      const auto delta =
          h.get_i64 ? h.get_i64(h.userdata, "__relocation_delta", 0) : 0;
      auto next = ot0::ReadSnapshot(read, h.main_base, delta);
      results.Observe(next);
      const auto modal = ot0::ReadModalState(read, h.main_base, delta);
      const bool modal_active = modal.valid && modal.active;
      ot0::BattleSnapshot battle;
      if (next.valid && next.state == 26)
        battle = ot0::ReadBattle(read, h.main_base, delta);
      if (battle.valid) {
        battle.command_actor_id=ot0::ReadCommandActor(read,h.main_base,battle,delta);
        const auto enemy_stats=ot0::ReadEnemyStats(read,h.main_base,battle,delta);
        for(int i=0;i<battle.enemy_count;++i) {
          if(enemy_stats[static_cast<std::size_t>(i)].max_hp_valid)
            battle.enemies[static_cast<std::size_t>(i)].max_hp=enemy_stats[static_cast<std::size_t>(i)].max_hp;
          if(enemy_stats[static_cast<std::size_t>(i)].speed_valid)
            battle.enemies[static_cast<std::size_t>(i)].speed=enemy_stats[static_cast<std::size_t>(i)].speed;
        }
        const auto limits = ot0::ReadBattleResourceLimits(read, h.main_base, battle, delta);
        for (std::size_t i = 0; i < battle.members.size(); ++i)
          if (limits[i].valid) {
            battle.members[i].max_hp = static_cast<int32_t>(limits[i].max_hp);
            battle.members[i].max_sp = static_cast<int32_t>(limits[i].max_sp);
          }
      }
      ot0::ExplorationSnapshot map;
      std::optional<ot0::ProtagonistAppearance> appearance;
      if (next.valid) {
        map = ot0::ReadExploration(read, h.main_base, delta);
        appearance = ot0::ReadProtagonistAppearance(read, h.main_base, delta);
        // Reject a party/load transition between the independently bounded
        // readers.
        const auto checked = ot0::ReadSnapshot(read, h.main_base, delta);
        if (checked != next) {
          next = {};
          next.state = checked.state;
          map = {};
          appearance.reset();
        } else if (map.state != next.state)
          map = {};
      }
      if (next.valid && next.state == 25) {
        const auto limits = ot0::ReadMenuResourceLimits(read, h.main_base, next, delta);
        for (std::size_t i=0;i<next.members.size();++i)
          if(limits[i].valid) {
            next.members[i].max_hp=limits[i].max_hp;
            next.members[i].max_sp=limits[i].max_sp;
          }
      }
      auto icons=map.valid && !modal_active?ot0::ReadMinimapIcons(read,h.main_base,delta,&map):ot0::MinimapIcons{};
      if(icons.map_id!=map.map_id)icons={};
      // Hold-off: one failed exploration/icon read in the open field must not drop the map page or
      // its markers for a frame (map blink); keep the last good read for up to ~20 samples.
      if(next.valid && next.state==23 && !modal_active) {
        std::lock_guard lock(view_mutex);
        if(!map.valid && current.map.valid && explore_grace<40) {
          ++explore_grace; map=current.map; icons=current.icons;
        } else if(map.valid) explore_grace=0;
        if(map.valid && !icons.valid && current.icons.valid && current.icons.map_id==map.map_id && icons_grace<40) {
          ++icons_grace; icons=current.icons;
        } else if(icons.valid) icons_grace=0;
      } else explore_grace=icons_grace=0;
      bool battle_layout_known=false;
      if(next.valid && next.state==26 && battle.valid) {
        const auto formation=ot0::ReadBattleFormation(read,h.main_base,battle,delta);
        battle_layout_known=formation.valid;
        if(formation.valid) {
          const auto saved=next.members;
          next.members={};
          const auto identity=[](uint32_t id){return id>=900 && id<=909?900u:id;};
          for(std::size_t slot=0;slot<formation.ids.size();++slot) {
            const auto id=formation.ids[slot];
            if(!id)continue;
            const auto found=std::find_if(saved.begin(),saved.end(),[&](const auto& member) {
              return member.available && identity(member.id)==identity(id);
            });
            if(found==saved.end()) {battle_layout_known=false;break;}
            next.members[slot]=*found;
          }
          if(!battle_layout_known)next.members=saved;
        }
      }
      const auto selection=battle.valid?ot0::ReadBattleSelection(read,h.main_base,battle,delta):ot0::BattleSelection{};
      auto stats=battle.valid?ot0::ReadBattleStats(read,h.main_base,battle,delta):ot0::BattleStatsSnapshot{};
      // Boost forecast (R/L on the command menu): the BP the pending command will spend.
      if(stats.valid)
        for(int i=0;i<battle.member_count && i<8;++i) {
          const auto actor=stats.party[static_cast<std::size_t>(i)].native_actor;
          std::uint8_t boost{},selector{};
          if(actor && read(actor+0x510,&boost,1) && boost<=3)
            battle.members[static_cast<std::size_t>(i)].boost=std::min<int32_t>(boost,battle.members[static_cast<std::size_t>(i)].bp);
          if(actor && read(actor+0x2a8,&selector,1) && selector<=1)
            battle.members[static_cast<std::size_t>(i)].weapon_slot=selector?0:1;
        }
      const auto metadata=std::atomic_load(&battle_metadata);
      const auto statuses=battle.valid && metadata?ot0::ReadBattleStatuses(read,h.main_base,battle,*metadata,delta):ot0::BattleStatusSnapshot{};
      const auto detail_party=selection.phase==ot0::BattleSelectionPhase::AllyTarget?selection.ally_id:selection.actor_id;
      stats=native_helper.Query(read,h.main_base,delta,stats,detail_party,selection.enemy_index);
      const auto shop=next.valid && next.state!=26 && !modal_active?
          ot0::ReadShopComparison(read,h.main_base,next,delta):ot0::ShopComparisonSnapshot{};
      std::lock_guard lock(view_mutex);
      if(!map.valid || !icons.valid || !icons.complete ||
         map.section_key!=current.map.section_key || map.texture_path!=current.map.texture_path ||
         icons.icons.size()!=current.icons.icons.size())map_render_ready=false;
      else for(std::size_t i=0;i<icons.icons.size();++i)
        if(icons.icons[i].frame_path!=current.icons.icons[i].frame_path ||
           icons.icons[i].background_frame_path!=current.icons.icons[i].background_frame_path)
          map_render_ready=false;
      // Party page: the selected member's native Status values (helper, field only).
      ot0::MemberStats field_stats;
      if (next.valid && next.state == 23 && !delta && native_helper.Poll().live &&
          !current.map_page && current.selected >= 0 &&
          current.selected < 8 && current.party.members[static_cast<std::size_t>(current.selected)].available)
        field_stats = native_stats.Poll(current.party.members[static_cast<std::size_t>(current.selected)].id);
      ot0::Inventory inventory;
      if (next.valid && current.items_page && next.state != 26)
        inventory = ot0::ReadInventory(read, h.main_base, delta);
      ot0::QuestStates quest_states;
      if (next.valid && current.quests_page && next.state != 26)
        quest_states = ot0::ReadQuestStates(read, h.main_base, delta);
      ot0::Gear gear;
      if (next.valid && !current.map_page && current.party_tab == 1 && current.selected >= 0 &&
          current.selected < 8 && current.party.members[static_cast<std::size_t>(current.selected)].available)
        gear = ot0::ReadGear(read, h.main_base, delta,
                             current.party.members[static_cast<std::size_t>(current.selected)].id);
      const bool details_changed=selection!=current.selection || stats!=current.stats || statuses!=current.statuses ||
                                 field_stats!=current.field_stats || gear!=current.gear ||
                                 inventory!=current.inventory || quest_states!=current.quests;
      const bool roles_changed=next.state==26 && !std::equal(
          next.members.begin(),next.members.end(),current.party.members.begin(),
          [](const auto& a,const auto& b) {
            return a.available==b.available && (!a.available || a.id==b.id);
          });
      if (!next.valid || !battle_layout_known || ot0::GetBattleFlow(battle) != ot0::BattleFlow::Combat)
        battle_render_ready = false;
      if(roles_changed)battle_render_ready=false;
      if (battle != current.battle &&
          !(next != current.party || PageIdentity(map) != PageIdentity(current.map) ||
            appearance != current.appearance || modal_active != current.modal_active || shop != current.shop || !SameIconSet(icons, current.icons) ||
            battle_layout_known != current.battle_layout_known) &&
          !requests.empty()) {
        current.battle = battle;
        current.selection=selection;
        current.stats=stats;
        current.statuses=statuses;
        current.focus = ot0::ComputeBattleLayout(battle, current.focus).focus;
        if(current.automatic_details && selection.valid && selection.phase==ot0::BattleSelectionPhase::EnemyTarget)
          current.focus=selection.enemy_index;
        ChangeLocked();
      }
      else if (requests.empty() || details_changed || next != current.party || PageIdentity(map) != PageIdentity(current.map) ||
          appearance != current.appearance || modal_active != current.modal_active || shop != current.shop || !SameIconSet(icons, current.icons) ||
          battle_layout_known != current.battle_layout_known) {
        current.battle = battle;
        current.selection=selection;
        current.stats=stats;
        current.statuses=statuses;
        current.focus = ot0::ComputeBattleLayout(battle, current.focus).focus;
        if(current.automatic_details && selection.valid && selection.phase==ot0::BattleSelectionPhase::EnemyTarget)
          current.focus=selection.enemy_index;
        std::uint32_t selected_id = 0;
        if (current.selected >= 0)
          selected_id = current.party.members[current.selected].id;
        current.party = next;
        current.last_battle = results.Last();
        current.field_stats = field_stats;
        current.gear = gear;
        current.inventory = std::move(inventory);
        if (quest_states != current.quests) ++quests_generation;
        current.quests = std::move(quest_states);
        current.map = std::move(map);
        current.appearance = appearance;
        current.modal_active = modal_active;
        current.battle_layout_known=battle_layout_known;
        current.shop = shop;
        current.icons=std::move(icons);
        current.map_page = !current.items_page && !current.quests_page && prefer_map && current.map.valid && !current.shop.valid;
        current.selected = -1;
        if (next.valid) {
          for (int i = 0; i < 8; ++i)
            if (next.members[i].available) {
              if (current.selected < 0)
                current.selected = i;
              if (selected_id && next.members[i].id == selected_id) {
                current.selected = i;
                break;
              }
            }
        }
        ChangeLocked();
      } else {
        // Same page: only positions moved. Markers follow these every tick.
        current.map = std::move(map);
        current.icons = std::move(icons);
      }
      // The map picture has its own generation: a new cell or newly revealed area.
      std::vector<ot0::MapPoint> frame;
      if (current.icons.valid && current.icons.map_id == current.map.map_id)
        for (const auto &icon : current.icons.icons)
          if (!icon.quest) // fixed POIs only: quest markers follow walking NPCs
            frame.push_back({icon.world_x, icon.world_y});
      std::sort(frame.begin(), frame.end(), [](const auto &a, const auto &b) {
        return a.x != b.x ? a.x < b.x : a.y < b.y;
      });
      if (current.map.valid &&
          (PictureMap(current.map) != PictureMap(picture_map) || frame != picture_frame)) {
        picture_map = current.map;
        picture_frame = std::move(frame);
        ++picture_generation;
      }
    }
    View view;
    ot0::ScreenVisuals visuals;
    ot0::MapGeometry geometry;
    std::uint64_t view_picture_generation{}, view_quests_generation{};
    std::shared_ptr<const std::vector<std::uint8_t>> view_alpha;
    {
      std::lock_guard lock(view_mutex);
      view_alpha = picture_alpha;
      view_quests_generation = quests_generation;
      view = current;
      geometry = map_geometry;
      view_picture_generation = picture_generation;
      if(visuals_generation==current.generation)visuals=render_visuals;
    }
    if (h.publish_text) {
      const auto key = std::string(Prefix) + std::to_string(view.generation);
      h.publish_text(h.userdata, "ot0.screen", key.c_str());
    }
    if (h.publish_i64) {
      h.publish_i64(h.userdata, "ot0.ready", view.party.valid && !view.modal_active &&
                     (view.party.state != 26 || (view.battle_layout_known && ot0::GetBattleFlow(view.battle) == ot0::BattleFlow::Combat)));
      h.publish_i64(h.userdata, "ot0.state", view.party.state);
      h.publish_i64(h.userdata, "ot0.asset_error", asset_error.load());
      h.publish_i64(h.userdata, "ot0.selected", view.selected);
      h.publish_i64(h.userdata, "ot0.map_available", view.map.valid && !view.modal_active && !view.shop.valid);
      h.publish_i64(h.userdata, "ot0.map_on", view.map_page);
      h.publish_i64(h.userdata,"ot0.heal_available",view.heal_available);
      h.publish_i64(h.userdata,"ot0.map_page",view.map_page && view.map.valid);
      h.publish_i64(h.userdata,"ot0.party_page",!view.map_page && !view.items_page && !view.quests_page && view.party.valid && view.party.state!=26);
      h.publish_i64(h.userdata,"ot0.items_page",view.items_page && view.party.valid && view.party.state!=26);
      const bool quests_shown = view.quests_page && view.party.valid && view.party.state!=26;
      h.publish_i64(h.userdata,"ot0.quests_page",quests_shown);
      const int quest_rows = quests_shown ? std::clamp(quests_count.load(), 0, 64) : 0;
      if (h.publish_text)
        for (int i = 0; i < quest_rows; ++i)
          h.publish_text(h.userdata, ("ot0.quests.row" + std::to_string(i)).c_str(),
                         ("module:ot0:questrow/" + std::to_string(view_quests_generation) + "/" + std::to_string(i)).c_str());
      h.publish_i64(h.userdata,"ot0.quests.count",quest_rows);
      h.publish_i64(h.userdata,"ot0.quests.h",std::max(1, quest_rows * 128));
      h.publish_i64(h.userdata,"ot0.quests.gen",static_cast<std::int64_t>(view_quests_generation));
      h.publish_i64(h.userdata,"ot0.standard_tabs",!view.heal_available);
      h.publish_i64(h.userdata,"ot0.heal_open",view.heal_open);
      h.publish_i64(h.userdata,"ot0.heal_confirm",view.heal_open && view.heal.ready);
      h.publish_i64(h.userdata,"ot0.heal_phase",static_cast<int>(view.heal.phase));
      h.publish_i64(h.userdata,"ot0.heal_result",view.heal.result);
      h.publish_i64(h.userdata,"ot0.heal_target",view.heal.target);
      h.publish_i64(h.userdata,"ot0.heal_caster",view.heal.caster);
      h.publish_i64(h.userdata,"ot0.heal_skill",view.heal.skill);
      h.publish_i64(h.userdata,"ot0.heal_cost",view.heal.cost);
      h.publish_i64(h.userdata, "ot0.shop_available", view.shop.valid && !view.modal_active);
      h.publish_i64(h.userdata, "ot0.shop_native_selected", view.shop.selected_member_id);
      h.publish_i64(h.userdata, "ot0.battle_layout_known", view.battle_layout_known);
      const bool battle_on = view.party.state == 26;
      const bool combat = view.party.valid && !view.modal_active &&
                          view.battle_layout_known &&
                          ot0::GetBattleFlow(view.battle) == ot0::BattleFlow::Combat;
      const ot0::TurnEntry command_actor{false,view.battle.command_actor_id};
      const auto* acting=combat && view.battle.command_actor_id?&command_actor:
          combat && view.battle.now_index>=0 && view.battle.now_index<view.battle.now_count &&
          view.battle.now[static_cast<std::size_t>(view.battle.now_index)].enemy?
          &view.battle.now[static_cast<std::size_t>(view.battle.now_index)]:nullptr;
      h.publish_i64(h.userdata,"ot0.acting_party",acting && !acting->enemy?acting->ref:0);
      h.publish_i64(h.userdata,"ot0.acting_enemy",acting && acting->enemy?static_cast<std::int64_t>(acting->ref):-1);
      h.publish_i64(h.userdata, "ot0.battle", battle_on);
      h.publish_i64(h.userdata,"ot0.detail_auto",view.automatic_details);
      h.publish_i64(h.userdata,"ot0.selection_phase",static_cast<int>(view.selection.phase));
      h.publish_i64(h.userdata,"ot0.selection_actor",view.selection.actor_id);
      h.publish_i64(h.userdata,"ot0.selection_ally",view.selection.ally_id);
      h.publish_i64(h.userdata,"ot0.selection_enemy",view.selection.enemy_index);
      h.publish_i64(h.userdata, "ot0.combat", combat);
      h.publish_i64(h.userdata, "ot0.map_icons",view.icons.icons.size());
      h.publish_i64(h.userdata, "ot0.map_icons_complete",view.icons.complete);
      h.publish_i64(h.userdata, "ot0.minimap_hide_supported",helper_supported.load());
      h.publish_i64(h.userdata, "ot0.minimap_hidden",minimap_hidden.load());
      h.publish_i64(h.userdata, "ot0.minimap_failures",minimap_failures.load());
      h.publish_i64(h.userdata, "ot0.hud_hidden", hud_hidden.load());
      h.publish_i64(h.userdata, "ot0.hud_failures", hud_failures.load());
      h.publish_i64(h.userdata, "ot0.hud_request", hud_request.load());
      const auto live_flag = h.get_i64 ? h.get_i64(h.userdata, "@flag:ot0.hide_party_hud", -1) : -1;
      h.publish_i64(h.userdata, "ot0.live_flags", live_flag == 0 || live_flag == 1);
      const auto bl = ot0::ComputeBattleLayout(view.battle, view.focus);
      for (int i = 0; i < 5; ++i) {
        const auto& prefix = keys.foe[static_cast<std::size_t>(i)];
        const bool party_detail=view.automatic_details && view.selection.valid &&
            (view.selection.phase==ot0::BattleSelectionPhase::Command || view.selection.phase==ot0::BattleSelectionPhase::AllyTarget);
        const bool on = combat && !view.automatic_details && !party_detail && i < bl.other_count;
        h.publish_i64(h.userdata, prefix[0].c_str(), on);
        h.publish_i64(h.userdata, prefix[1].c_str(),
                      ot0::BattleLayout::RowTop(bl.other_count, i));
        h.publish_i64(h.userdata, prefix[2].c_str(),
                      on ? bl.others[i] : -1);
      }
      PublishMap(h, view, geometry, view_picture_generation, view_alpha.get());
      // Native BP pulse: live pips breathe in size (and alpha via ot0.pulse); spent (boost
      // forecast) pips blink grey at double speed. Reduced Motion keeps them still.
      const bool still=h.get_i64 && h.get_i64(h.userdata,"@flag:ot0.reduced_motion",0)==1;
      const auto pulse_ms=std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      const double breathe=still?0:(1-std::cos((pulse_ms%1800)*6.283185307179586/1800.0))/2;
      const double blink=still?0:(1-std::cos((pulse_ms%900)*6.283185307179586/900.0))/2;
      for(int i=0;i<45;++i) {
        const auto& key=keys.fx[static_cast<std::size_t>(i)];
        const bool on=i<static_cast<int>(visuals.pulses.size()) && !view.modal_active;
        h.publish_i64(h.userdata,key[0].c_str(),on);
        if(!on)continue;
        const auto& glyph=visuals.pulses[static_cast<std::size_t>(i)];
        if(h.publish_text) {
          std::string source=glyph.spent?"module:ot0:glyph/bpspent":"module:ot0:glyph/bp";
          if(glyph.player) {
            source="module:ot0:glyph/player/";
            constexpr char hex[]="0123456789abcdef";
            for(const auto c:glyph.color) {source+=hex[c>>4];source+=hex[c&15];}
          }
          h.publish_text(h.userdata,key[1].c_str(),source.c_str());
        }
        h.publish_i64(h.userdata,key[2].c_str(),glyph.player);
        h.publish_i64(h.userdata,key[3].c_str(),glyph.x-32);
        h.publish_i64(h.userdata,key[4].c_str(),glyph.y-32);
        const double grow=glyph.player?1.0:glyph.spent?1.0-0.25*blink:1.0+0.18*breathe;
        h.publish_i64(h.userdata,key[5].c_str(),std::lround(glyph.size*1000/64.0*grow));
      }
      const auto layout = ot0::ComputeFormationLayout(view.party);
      for (int i = 0; i < 8; ++i) {
        const auto& prefix = keys.p[static_cast<std::size_t>(i)];
        h.publish_i64(h.userdata, prefix[0].c_str(),
                      false); // old Formation card taps; the Party page uses the pair rows
        h.publish_i64(h.userdata, prefix[1].c_str(), layout.slots[i].x);
        h.publish_i64(h.userdata, prefix[2].c_str(), layout.slots[i].y);
      }
      int row=0;
      for(int pair=0;pair<4;++pair) {
        const bool occupied=view.party.members[pair].available || view.party.members[pair+4].available;
        for(int side=0;side<2;++side) {
          const int slot=pair+4*side;
          const auto& prefix=keys.mp[static_cast<std::size_t>(slot)];
          h.publish_i64(h.userdata,prefix[0].c_str(),occupied && view.party.members[slot].available &&
              (view.map_page ? view.heal_available : view.party.state != 26) && !view.modal_active &&
              view.heal.phase!=ot0::HealPhase::Applying);
          h.publish_i64(h.userdata,prefix[1].c_str(),ot0::TownMapLayout::PairTop+row*ot0::TownMapLayout::PairPitch);
        }
        if(occupied)++row;
      }
    }
  }
  // Map pane: a runtime map widget whose world is the picture (MapW x MapH, y up). The runtime
  // pans/zooms the picture and keeps every marker on its world point; the module publishes
  // world positions and the home view (a fixed native scale around the player).
  void PublishMap(const EdenDsmodHostApi &h, const View &view, const ot0::MapGeometry &g,
                  std::uint64_t view_picture_generation, const std::vector<std::uint8_t> *alpha) {
    const bool page = view.map_page && view.map.valid && !view.modal_active && !view.heal_open;
    // Double-buffered picture: the map widget keeps showing the last picture the module has
    // delivered while a hidden 1-px widget fetches the next one (a changed map image key
    // draws nothing until decoded, which blinked the map while walking).
    if (page && h.publish_text) {
      const auto next = "module:ot0:mappic/" + std::to_string(view_picture_generation);
      std::string served;
      {
        std::lock_guard lock(view_mutex);
        served = served_picture_key;
      }
      h.publish_text(h.userdata, "ot0.map.next", next.c_str());
      h.publish_text(h.userdata, "ot0.map.pic", (served.empty() ? next : served).c_str());
    }
    const bool shown = page && g.valid && g.picture_w && g.picture_h && g.texture_w && g.texture_h;
    h.publish_i64(h.userdata, "ot0.map.on", page);
    int used = 0, rings = 0;
    bool player = false;
    const bool reduced = h.get_i64 && h.get_i64(h.userdata, "@flag:ot0.reduced_motion", 0) == 1;
    const auto put = [&](const std::string &key, double value) {
      if (h.publish_f64) h.publish_f64(h.userdata, key.c_str(), value);
      else h.publish_i64(h.userdata, key.c_str(), std::lround(value));
    };
    if (shown) {
      const auto picture_point = [&](double wx, double wy, double &x, double &y) {
        const auto t = ot0::NativeTexturePoint(view.map, wx, wy, g.texture_w, g.texture_h);
        const double u = (t.x - g.crop_x) / g.picture_w, v = (t.y - g.crop_y) / g.picture_h;
        x = u * MapW;
        y = (1 - v) * MapH;
        return std::isfinite(u) && std::isfinite(v) && u >= 0 && v >= 0 && u <= 1 && v <= 1;
      };
      double px = MapW / 2.0, py = MapH / 2.0;
      player = picture_point(view.map.world_xyz[0], view.map.world_xyz[1], px, py);
      // Home view: MapHomeTexels native texels across, centred on the player, kept inside.
      const double home = std::clamp(g.picture_w / MapHomeTexels, 1.0, MapMaxZoom);
      const double w = MapW / home, hgt = MapH / home;
      const double x0 = std::clamp(px - w / 2, 0.0, MapW - w), y0 = std::clamp(py - hgt / 2, 0.0, MapH - hgt);
      put("ot0.map.v.x0", x0); put("ot0.map.v.y0", y0);
      put("ot0.map.v.x1", x0 + w); put("ot0.map.v.y1", y0 + hgt);
      if (view.icons.valid && view.icons.map_id == view.map.map_id) {
        std::vector<std::pair<std::string, ot0::MinimapIcon>> fresh;
        for (const auto &icon : view.icons.icons) {
          double x, y;
          if (used >= MapMarkers || !picture_point(icon.world_x, icon.world_y, x, y))
            continue;
          // Only discovered places: an icon the native reveal policy has not already passed is
          // shown when the revealed map is drawn within 24 texels of it (quest markers excepted).
          if (!icon.quest && !icon.reveal_checked) {
            if (!alpha || alpha->size() != std::size_t(g.picture_w) * g.picture_h)
              continue;
            const int cx = int(x / MapW * g.picture_w), cy = int((1 - y / MapH) * g.picture_h);
            bool seen = false;
            for (int dy = -24; dy <= 24 && !seen; dy += 4)
              for (int dx = -24; dx <= 24 && !seen; dx += 4) {
                const int px = cx + dx, py = cy + dy;
                seen = px >= 0 && py >= 0 && px < int(g.picture_w) && py < int(g.picture_h) &&
                       (*alpha)[std::size_t(py) * g.picture_w + px] > 0;
              }
            if (!seen)
              continue;
          }
          auto key = IconKey(icon);
          // Native minimap: a quest marker stays still while rings ripple out from it.
          if (icon.quest && !reduced && rings < MapRings) {
            const auto &r = keys.rg[static_cast<std::size_t>(rings++)];
            const auto c = [&](int i) { return std::int64_t(std::clamp(icon.color[i], 0.f, 1.f) * 255 + .5f); };
            h.publish_i64(h.userdata, r[0].c_str(), 1);
            h.publish_i64(h.userdata, r[1].c_str(), (std::int64_t(255) << 24) | (c(0) << 16) | (c(1) << 8) | c(2));
            put(r[2], x); put(r[3], y);
          }
          const auto &k = keys.mk[static_cast<std::size_t>(used++)];
          h.publish_i64(h.userdata, k[0].c_str(), 1);
          if (h.publish_text)
            h.publish_text(h.userdata, k[1].c_str(), key.c_str());
          put(k[2], x); put(k[3], y);
          fresh.emplace_back(std::move(key), icon);
        }
        std::lock_guard lock(view_mutex);
        for (auto &[key, icon] : fresh)
          icon_glyphs.try_emplace(std::move(key), std::move(icon));
      }
      if (player) { // native player frame, static, drawn above the icons
        const auto c = view.icons.valid ? view.icons.player_color : std::array<float, 4>{1, 1, 1, 1};
        char hex[9];
        std::snprintf(hex, sizeof hex, "%02x%02x%02x%02x",
                      int(std::clamp(c[0], 0.f, 1.f) * 255 + .5f), int(std::clamp(c[1], 0.f, 1.f) * 255 + .5f),
                      int(std::clamp(c[2], 0.f, 1.f) * 255 + .5f), int(std::clamp(c[3], 0.f, 1.f) * 255 + .5f));
        if (h.publish_text)
          h.publish_text(h.userdata, "ot0.pl.src", (std::string("module:ot0:glyph/player/") + hex).c_str());
        put("ot0.pl.x", px); put("ot0.pl.y", py);
      }
    }
    h.publish_i64(h.userdata, "ot0.pl.on", player);
    for (int i = used; i < MapMarkers; ++i)
      h.publish_i64(h.userdata, keys.mk[static_cast<std::size_t>(i)][0].c_str(), 0);
    for (int i = rings; i < MapRings; ++i)
      h.publish_i64(h.userdata, keys.rg[static_cast<std::size_t>(i)][0].c_str(), 0);
  }
  bool Action(std::string_view action, std::int64_t slot) {
    if (action == "hud_hide") {
      // Diagnostic preference; hiding still requires live settings reads.
      // No game write here.
      if (slot != 0 && slot != 1)
        return false;
      hide_setting.store(slot != 0);
      return true;
    }
    std::lock_guard lock(view_mutex);
    if (current.modal_active)
      return false;
    if(current.party.state==26 && !current.battle_layout_known)
      return false;
    if(action=="detail_mode") {
      if(current.party.state!=26)return false;
      current.automatic_details=!current.automatic_details;
      ChangeLocked();return true;
    }
    if (action == "foe") {
      if(current.automatic_details)return false;
      // Companion-side focus only (no game write): slot = tapped compact foe
      // row (0..2).
      const auto layout =
          ot0::ComputeBattleLayout(current.battle, current.focus);
      if (ot0::GetBattleFlow(current.battle) != ot0::BattleFlow::Combat ||
          slot < 0 || slot >= layout.other_count)
        return false;
      slot = layout.others[static_cast<std::size_t>(slot)];
      if (current.focus != slot) {
        current.focus = static_cast<int>(slot);
        ChangeLocked();
      }
      return true;
    }
    if (current.party.state == 26)
      return false;
    if(action=="heal_cancel") {
      native_heal.Cancel();current.heal_open=false;current.heal={};ChangeLocked();return true;
    }
    if(action=="heal" || action=="heal_apply") {
      if(!current.heal_available || current.selected<0 || current.selected>=8)return false;
      if(action=="heal_apply") {
        if(!current.heal_open || !current.heal.ready)return false;
        const bool ok=native_heal.Apply();
        current.heal=native_heal.Poll();ChangeLocked();return ok;
      }
      if(current.heal.phase==ot0::HealPhase::Applying)return false;
      if(current.heal_open) {
        native_heal.Cancel();current.heal_open=false;current.heal={};ChangeLocked();return true;
      }
      current.heal_open=true;
      const bool ok=native_heal.Find(current.party.members[current.selected].id);
      current.heal=native_heal.Poll();ChangeLocked();return ok;
    }
    if (action == "items_tab") {
      if (!current.items_page || slot < 0 || slot >= ot0::ItemTabs) return false;
      if (current.items_tab != slot) { current.items_tab = int(slot); current.items_first = 0; ChangeLocked(); }
      return true;
    }
    if (action == "items_page_step") { // slot 0 previous, 1 next (the screen clamps to the list)
      if (!current.items_page || (slot != 0 && slot != 1)) return false;
      const int first = std::clamp(current.items_first + (slot ? ot0::ItemRows : -ot0::ItemRows), 0,
                                   current.items_last_first);
      if (first != current.items_first) { current.items_first = first; ChangeLocked(); }
      return true;
    }
    if (action == "party_view") {
      if (slot < 0 || slot > 1 || current.map_page) return false;
      if (current.party_tab != slot) { current.party_tab = int(slot); ChangeLocked(); }
      return true;
    }
    if (action == "page") {
      if (!current.party.valid || slot < 0 || slot > 3 ||
          (slot == 0 && (!current.map.valid || current.shop.valid)))
        return false;
      if (slot >= 2) { // Items / Quests
        native_heal.Cancel();current.heal_open=false;current.heal={};
        const bool items = slot == 2;
        if (current.items_page != items || current.quests_page == items || current.map_page) {
          current.items_page = items; current.quests_page = !items; current.map_page = false;
          map_render_ready = false; ChangeLocked();
        }
        return true;
      }
      if (current.items_page || current.quests_page) {
        current.items_page = current.quests_page = false; ChangeLocked();
      }
      prefer_map = slot == 0;
      native_heal.Cancel();current.heal_open=false;current.heal={};
      const bool page = prefer_map && current.map.valid;
      if (page != current.map_page) {
        current.map_page = page;
        map_render_ready=false;
        ChangeLocked();
      }
      return true;
    }
    if (action != "select" || slot < 0 || slot >= 8 ||
        (current.map_page && !current.heal_available) || current.party.state == 26 ||
        !current.party.valid || !current.party.members[slot].available)
      return false;
    if (current.selected != slot) {
      if(current.heal_open && current.heal.phase==ot0::HealPhase::Applying)return false;
      current.selected = static_cast<int>(slot);
      if(current.heal_open) {
        native_heal.Find(current.party.members[slot].id);
        current.heal=native_heal.Poll();
      }
      ChangeLocked();
    }
    return true;
  }
  bool Image(const EdenDsmodHostApi &h, std::string_view key, void *receiver,
             EdenDsmodImageSink sink) {
    if (!sink || !h.read_romfs)
      return false;
    if (key == "module:ot0:glyph/bp" || key == "module:ot0:glyph/bpspent" ||
        key.starts_with("module:ot0:glyph/player/")) {
      std::array<ot0::u8,4> color{255,255,255,255};
      const bool spent=key=="module:ot0:glyph/bpspent";
      if(spent)color={150,150,150,200};
      const bool player=key!="module:ot0:glyph/bp" && !spent;
      if(player) {
        const auto hex=key.substr(std::string_view("module:ot0:glyph/player/").size());
        if(hex.size()!=8)return false;
        for(int i=0;i<4;++i) {
          unsigned value{};const auto [end,ec]=std::from_chars(hex.data()+i*2,hex.data()+i*2+2,value,16);
          if(ec!=std::errc{} || end!=hex.data()+i*2+2)return false;
          color[i]=static_cast<ot0::u8>(value);
        }
      }
      std::lock_guard lock(art_mutex);
      const auto image=composer.Glyph(ReadAssets(h),player,color);
      if(!image)return false;
      sink(receiver,image->width,image->height,image->rgba.data(),image->rgba.size());
      return true;
    }
    if (key.starts_with("module:ot0:mappic/")) {
      ot0::ExplorationSnapshot map;
      std::vector<ot0::MapPoint> frame;
      {
        std::lock_guard lock(view_mutex);
        map = picture_map; // latest; the host keeps showing the last good picture meanwhile
        frame = picture_frame;
      }
      ot0::MapGeometry geometry;
      std::optional<ot0::Image> image;
      {
        std::lock_guard lock(art_mutex);
        image = composer.MapPicture(ReadAssets(h), map, &geometry, frame);
      }
      if (!image)
        return false;
      auto alpha = std::make_shared<std::vector<std::uint8_t>>(std::size_t(image->width) * image->height);
      for (std::size_t i = 0; i < alpha->size(); ++i)
        (*alpha)[i] = image->rgba[i * 4 + 3];
      {
        std::lock_guard lock(view_mutex);
        map_geometry = geometry;
        picture_alpha = std::move(alpha);
        served_picture_key = std::string(key);
      }
      sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
      return true;
    }
    if (key.starts_with("module:ot0:questrow/")) {
      const auto slash = key.rfind('/');
      int index{};
      const auto [end, ec] = std::from_chars(key.data() + slash + 1, key.data() + key.size(), index);
      if (ec != std::errc{} || end != key.data() + key.size())
        return false;
      ot0::QuestStates states;
      {
        std::lock_guard lock(view_mutex);
        states = current.quests;
      }
      std::optional<ot0::Image> image;
      {
        std::lock_guard lock(art_mutex);
        image = composer.QuestList(ReadAssets(h), states, index);
      }
      if (!image)
        return false;
      sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
      return true;
    }
    if (key.starts_with("module:ot0:ringf/")) {
      // White two-ring ripple frame (tinted per marker by the host).
      unsigned frame{};
      const auto digits = key.substr(std::string_view("module:ot0:ringf/").size());
      const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), frame);
      if (ec != std::errc{} || end != digits.data() + digits.size() || frame >= ot0::RingFrames)
        return false;
      std::lock_guard lock(art_mutex);
      const auto image = composer.RingFrame(ReadAssets(h), frame);
      if (!image)
        return false;
      sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
      return true;
    }
    if (key.starts_with("module:ot0:icon/")) {
      ot0::MinimapIcon icon;
      {
        std::lock_guard lock(view_mutex);
        const auto found = icon_glyphs.find(std::string(key));
        if (found == icon_glyphs.end())
          return false;
        icon = found->second;
      }
      std::lock_guard lock(art_mutex);
      const auto image = composer.IconGlyph(ReadAssets(h), icon);
      if (!image)
        return false;
      sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
      return true;
    }
    if (key.starts_with(SettingsPrefix)) {
      const auto flags = key.substr(SettingsPrefix.size());
      if (flags.size() != 3 || flags.find_first_not_of("01") != flags.npos)
        return false;
      std::lock_guard lock(art_mutex);
      const auto image = composer.ComposeSettings(ReadAssets(h), flags[0] == '1',
                                                   flags[1] == '1', flags[2] == '1');
      if (!image)
        return false;
      // Settings is not a battle frame and cannot authorize hiding the HUD.
      sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
      return true;
    }
    if (!key.starts_with(Prefix))
      return false;
    const auto id = key.substr(Prefix.size());
    std::uint64_t generation{};
    const auto [end, ec] =
        std::from_chars(id.data(), id.data() + id.size(), generation);
    if (ec != std::errc{} || end != id.data() + id.size())
      return false;
    View view;
    {
      std::lock_guard lock(view_mutex);
      auto found =
          std::find_if(requests.begin(), requests.end(), [&](const View &v) {
            return v.generation == generation;
          });
      if (found == requests.end())
        return false;
      view = *found;
    }
    const auto read = ReadAssets(h);
    std::lock_guard lock(art_mutex);
    std::string why;
    if(!std::atomic_load(&battle_metadata)) {
      constexpr ot0::MemberId ids[]{ot0::MemberId::CharaStatusDb,ot0::MemberId::AilmentTypeDb,ot0::MemberId::TextUI,ot0::MemberId::TextSkill};
      std::array<std::optional<std::vector<ot0::u8>>,4> tables;
      bool complete=true;
      for(std::size_t i=0;i<4;++i) {tables[i]=ot0::ReadMember(read,ot0::Member(ids[i]),nullptr);complete=complete && tables[i].has_value();}
      if(complete) {
        auto decoded=ot0::DecodeBattleMetadata(*tables[0],*tables[1],*tables[2],*tables[3]);
        if(decoded.valid)std::atomic_store(&battle_metadata,std::make_shared<const ot0::BattleMetadata>(std::move(decoded)));
      }
    }
    const auto metadata=std::atomic_load(&battle_metadata);
    const ot0::BattleDetailView detail{&view.selection,&view.stats,metadata.get(),&view.statuses,view.automatic_details};
    ot0::HealScreenView healing;
    healing.available=view.heal_available;healing.open=view.heal_open;
    healing.checking=view.heal.phase==ot0::HealPhase::Checking || view.heal.phase==ot0::HealPhase::Idle;
    healing.ready=view.heal.ready;healing.applying=view.heal.phase==ot0::HealPhase::Applying;
    healing.done=view.heal.phase==ot0::HealPhase::Done;
    healing.target=healing.checking && view.selected>=0?view.party.members[view.selected].id:
        view.heal.target?view.heal.target:view.selected>=0?view.party.members[view.selected].id:0;
    healing.hp=healing.done?view.heal.after_hp:view.heal.hp;
    healing.max_hp=view.heal.max_hp;healing.cost=view.heal.cost;
    if(metadata)healing.skill=metadata->skill_texts.LookupName(view.heal.text_id);
    if(healing.skill.empty())healing.skill="Healing skill";
    healing.message=healing.done?(view.heal.hp==0?"Revived":"HP restored"):view.heal.max_hp>0 && view.heal.hp>=view.heal.max_hp?
        "HP is already full":view.heal.result==OT0_HEAL_NO_TARGET?
        "No usable healing skill for this target":view.heal.result==OT0_HEAL_UNAFFORDABLE?
        "Not enough SP":"Healing unavailable right now";
    // Names use the same owned native character table as the roster.
    healing.caster_id=view.heal.caster;
    ot0::ScreenVisuals visuals;
    int items_last_first = 4096;
    const ot0::PartyView party_view{view.party_tab, &view.gear, view.items_page && view.party.state != 26,
                                    view.items_tab, view.items_first, &view.inventory, &items_last_first,
                                    view.quests_page && view.party.state != 26, &view.quests};
    auto image = composer.Compose(read, view.party, view.selected, &why,
                                  &view.map, view.map_page,
                                  view.appearance ? &*view.appearance : nullptr,
                                  &view.battle, view.focus,
                                  view.modal_active || (view.party.state==26 && !view.battle_layout_known),
                                  &view.shop,&visuals,&view.icons,&detail,&healing,&view.last_battle,&view.field_stats,&party_view);
    if (view.quests_page)
      quests_count.store(composer.QuestCount(read, view.quests));
    if (view.items_page) {
      std::lock_guard lock(view_mutex);
      if (current.generation == view.generation) { // never from a superseded render
        current.items_last_first = items_last_first;
        if (current.items_first > items_last_first) { current.items_first = items_last_first; ChangeLocked(); }
      }
    }
    if (!image) {
      bool report{};
      {
        std::lock_guard lock(view_mutex);
        if (current.generation == view.generation) {
          battle_render_ready = false;
          map_render_ready=false;
          report = !asset_error.exchange(true);
        }
      }
      if (report && h.log)
        h.log(h.userdata, EDEN_DSMOD_LOG_WARNING,
              ("Octopath companion art unavailable: " + why).c_str());
      return false;
    }
    {
      std::lock_guard lock(view_mutex);
      if (current.generation == view.generation) {
        asset_error.store(false);
        map_render_ready=current.map_page && visuals.map_complete;
        // map_geometry comes only from the delivered map picture (double-buffered with
        // served_picture_key/picture_alpha); this page's MapBase may already hold a newer crop.
        render_visuals=std::move(visuals);
        visuals_generation=view.generation;
        battle_render_ready = current.party.valid && !current.modal_active &&
                              current.battle_layout_known &&
                              ot0::GetBattleFlow(current.battle) == ot0::BattleFlow::Combat;
      }
    }
    sink(receiver, image->width, image->height, image->rgba.data(),
         image->rgba.size());
    return true;
  }

  void SetWriteApi(const EdenDsmodHostWriteApi &api) {
    std::lock_guard lock(hud_mutex);
    write_api = api;
  }
  void SetExtensions(const EdenDsmodHostExtensions& ext) { native_helper.Configure(ext);native_heal.Configure(ext);native_stats.Configure(ext); }
  void PollGuest(const EdenDsmodHostApi& h) {
    const auto state=native_helper.Poll();
    helper_supported.store(state.live && state.version==OT0_COMPANION_ABI);
    minimap_hidden.store(state.hidden);
    if(h.publish_i64) {
      h.publish_i64(h.userdata,"ot0.native_mailbox",state.mailbox);
      h.publish_i64(h.userdata,"ot0.native_mailbox_size",state.mailbox_size);
      h.publish_i64(h.userdata,"ot0.native_heartbeat",state.heartbeat);
      h.publish_i64(h.userdata,"ot0.native_epoch",state.epoch);
      h.publish_i64(h.userdata,"ot0.native_helper_version",state.version);
      h.publish_i64(h.userdata,"ot0.native_hook_live",state.live);
      h.publish_i64(h.userdata,"ot0.native_query_result",state.query_result);
    }
  }
  // Every frame, shown or hidden: the command group follows the game's slide
  // each frame; the HUD rows are maintained about twice a second.
  void Tick(const EdenDsmodHostApi &h) {
    PollGuest(h);
    // Queried once per tick; both the animation phase and the HUD logic use them.
    const bool aux = h.get_i64 && h.get_i64(h.userdata, "__aux_present", 0) == 1;
    const char *page = h.get_text ? h.get_text(h.userdata, "__page") : nullptr;
    if(h.publish_i64) {
      const bool reduced=h.get_i64 && h.get_i64(h.userdata,"@flag:ot0.reduced_motion",0)==1;
      const bool animate=aux && page && std::string_view(page)=="party";
      const auto now=std::chrono::steady_clock::now().time_since_epoch();
      const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
      const int phase=reduced || !animate?0:static_cast<int>((1.0-std::cos((ms%1800)*6.283185307179586/1800.0))*7.5);
      h.publish_i64(h.userdata,"ot0.pulse",phase);
    }
    const bool rows_due = ticks++ % 30 == 0;
    const auto get = [&](const char *name, std::int64_t fallback) {
      return h.get_i64 ? h.get_i64(h.userdata, name, fallback) : fallback;
    };
    // A host unable to identify its current page cannot prove that the moved
    // HUD is visible. Keep the native HUD in that case.
    const bool battle_page = page && std::string_view(page) == "party";
    const ot0::ReadMemory read = [&](auto address, void *out, std::size_t n) {
      return h.read_memory &&
             h.read_memory(h.userdata, address, out, n) == EDEN_DSMOD_TRUE;
    };
    const auto modal = ot0::ReadModalState(read, h.main_base, get("__relocation_delta", 0));
    auto healing=native_heal.Poll();
    bool ready{},map_ready{};
    std::optional<ot0::MinimapIcons> lease_icons;
    {
      std::lock_guard lock(view_mutex);
      const bool visible = aux && battle_page && modal.valid && !modal.active;
      const bool heal_allowed=visible && healing.live && !get("__relocation_delta",0) &&
          current.party.valid && current.party.state==23 && current.map_page && current.map.valid && !current.shop.valid;
      if(!heal_allowed && current.heal_open) {
        native_heal.Cancel();current.heal_open=false;healing=native_heal.Poll();
      }
      const auto same_identity=[](uint32_t a,uint32_t b) {
        return a==b || (a>=900 && a<=909 && b>=900 && b<=909);
      };
      if(heal_allowed && current.heal_open && current.selected>=0 &&
         healing.phase!=ot0::HealPhase::Applying && healing.phase!=ot0::HealPhase::Checking &&
         (healing.phase==ot0::HealPhase::Idle ||
          !same_identity(healing.target,current.party.members[current.selected].id) ||
          (healing.phase==ot0::HealPhase::Unavailable && healing.result==OT0_HEAL_EXPIRED))) {
        native_heal.Find(current.party.members[current.selected].id);
        healing=native_heal.Poll();
      }
      if(current.heal_available!=heal_allowed || current.heal!=healing) {
        current.heal_available=heal_allowed;current.heal=healing;ChangeLocked();
      }
      if (last_visible && *last_visible != visible) {
        battle_render_ready = false;
        map_render_ready=false;
        // Do not repaint a cached formation on return before the next normal
        // six-tick poll. A native swap may have happened while aux was hidden.
        if(visible)samples=0;
        // Returning to a cached battle still needs a fresh current view.
        ChangeLocked();
      }
      last_visible = visible;
      map_ready=visible && current.map_page && current.map.valid && current.icons.complete &&
          map_render_ready && !asset_error.load();
      // Hold-off: a single incomplete icon read or a redraw in progress must not re-show the native
      // minimap for a frame (show/hide blink). Leaving the map page or the field still re-shows at once.
      const bool map_context=visible && current.map_page && current.map.valid && !asset_error.load();
      if(map_ready) { map_grace=120; last_lease=current.icons; }
      else if(map_context && map_grace>0) { --map_grace; map_ready=last_lease.has_value(); }
      else map_grace=0;
      if(map_ready)lease_icons=last_lease;
      ready = battle_render_ready && current.party.valid && !current.modal_active &&
              current.battle_layout_known &&
              modal.valid && !modal.active &&
              ot0::GetBattleFlow(current.battle) == ot0::BattleFlow::Combat && !asset_error.load();
      // A previous page or hold-off cannot authorize hiding current combat information.
      // A fresh, valid battle image must exist after visibility, role or lifecycle changes.

    }
    // Hiding requires the live settings protocol (Eden Duo 1.3.2+). Older
    // rt22 hosts keep native information visible rather than losing a working
    // re-show control. The legacy action remains a local diagnostic preference.
    const auto flag = get("@flag:ot0.hide_party_hud", -1);
    const bool flag_supported = flag == 0 || flag == 1;
    const bool hide = flag_supported ? flag != 0 : hide_setting.load();
    hud_request.store((hide ? 1 : 0) + (aux ? 2 : 0));
    const bool hide_map=hide && flag_supported && aux && battle_page && map_ready;
    if(rows_due || !hide_map || !minimap_hidden.load())
      native_helper.SetMapLease(read,h.main_base,get("__relocation_delta",0),lease_icons?&*lease_icons:nullptr,hide_map);
    std::lock_guard lock(hud_mutex);
    if (!write_api.write_batch)
      return;
    const ot0::WriteF32 write = [&](std::uint64_t address, float expect,
                                    float value) {
      const EdenDsmodWriteOp op{address, 4, 0, &expect, &value};
      return write_api.write_batch(write_api.userdata, &op, 1) ==
             EDEN_DSMOD_TRUE;
    };
    const ot0::WriteBlock block = [&](auto address, const void* expect, const void* value, std::size_t n) {
      const EdenDsmodWriteOp op{address, static_cast<uint32_t>(n), 0, expect, value};
      return write_api.write_batch(write_api.userdata, &op, 1) == EDEN_DSMOD_TRUE;
    };
    const auto delta = get("__relocation_delta", 0);
    // With the party HUD hidden, the command group moves up into its top-right
    // space.
    const bool hide_now = hide && flag_supported && aux && ready && battle_page;
    // Restore immediately if the bottom screen, its reader, or its assets stop
    // being usable. First hide also runs immediately: raising commands before
    // the next maintenance tick would put them over a still-visible party HUD.
    // Showing: hide the rows first, then raise. Restoring: lower the command group first, so the
    // rows never reappear inside a still-shifted parent panel.
    if (!hide_now)
      command.Update(read, block, h.main_base, false, delta);
    if (rows_due || !hide_now || !hud.Hidden())
      hud.Update(read, write, h.main_base, hide_now, delta);
    if (hide_now)
      command.Update(read, block, h.main_base, hud.Hidden() > 0, delta);
    hud_hidden.store(hud.Hidden() + (command.Shifted() ? 100 : 0));
    hud_failures.store(hud.Failures() + command.Failures());
  }

private:
  const PublishKeys keys;
  ot0::BattleResults results; // sampler thread only
  ot0::NativeStats native_stats;
  std::shared_ptr<const ot0::BattleMetadata> battle_metadata;
  ot0::NativeHeal native_heal;
  ot0::NativeCompanionHelper native_helper;
  ot0::RangeReader ReadAssets(const EdenDsmodHostApi &h) {
    return [this, &h](auto offset, void *out, std::size_t n) {
      if (!n || n > 64 * 1024 * 1024 || offset > ot0::PakSize ||
          n > ot0::PakSize - offset)
        return false;
      if (!pak_size_ok.load()) {
        if (h.read_romfs(h.userdata, ot0::PakPath.data(), 0, nullptr, 0) != ot0::PakSize)
          return false;
        pak_size_ok.store(true);
      }
      return h.read_romfs(h.userdata, ot0::PakPath.data(), offset, out, n) == n;
    };
  }
  void ChangeLocked() {
    ++current.generation;
    requests.push_back(current);
    if (requests.size() > 32)
      requests.pop_front();
  }
  std::atomic<unsigned> samples{};
  EdenDsmodHostApi cleanup_host{}; // callbacks remain valid through destroy()
  bool prefer_map{true};
  // Lock order: art_mutex may be held while taking view_mutex (screen render), never the reverse.
  std::mutex view_mutex, art_mutex;
  View current;
  ot0::ScreenVisuals render_visuals;
  ot0::MapGeometry map_geometry;                        // view_mutex; latest composed map picture
  ot0::ExplorationSnapshot picture_map;                 // view_mutex; snapshot for the map picture
  std::vector<ot0::MapPoint> picture_frame;             // view_mutex; icon points it frames
  std::uint64_t quests_generation{};                    // view_mutex; quest list image key
  std::shared_ptr<const std::vector<std::uint8_t>> picture_alpha; // view_mutex; revealed mask
  std::string served_picture_key;                       // view_mutex; last delivered map picture
  std::atomic<int> quests_count{0};                     // listed quest rows (render thread)
  std::uint64_t picture_generation{};                   // view_mutex
  std::map<std::string, ot0::MinimapIcon> icon_glyphs; // view_mutex; key -> icon look
  std::uint64_t visuals_generation{};
  bool map_render_ready{};
  bool battle_render_ready{}; // view_mutex; first successful current-battle
                              // image
  std::optional<bool> last_visible; // view_mutex; includes settings and hidden screen
  std::deque<View> requests;
  std::atomic<bool> pak_size_ok{}, asset_error{};
  std::mutex hud_mutex;
  EdenDsmodHostWriteApi write_api{};
  std::atomic<bool> helper_supported{};
  ot0::PartyHud hud;
  std::atomic<int> minimap_hidden{},minimap_failures{};
  // Top-right target, in the game's 1280x720 UI units, added to the game's own
  // translation.
  // Raised into the hidden party HUD's top-right space; 112 keeps the wide "Ultra" label on screen.
  ot0::CommandShift command{112.0f, -368.0f};
  int map_grace{};                   // Tick thread hold-offs (frames)
  int explore_grace{}, icons_grace{};                // sampler thread hold-offs (samples)
  std::optional<ot0::MinimapIcons> last_lease;       // view_mutex
  unsigned ticks{};
  std::atomic<int> hud_hidden{}, hud_failures{}, hud_request{};
  std::atomic<bool> hide_setting{true};
  ot0::ScreenComposer composer;
};
EdenDsmodBool Supports(const char *id) {
  return id && std::string_view(id) == Build ? EDEN_DSMOD_TRUE
                                             : EDEN_DSMOD_FALSE;
}
void *Create(const EdenDsmodHostApi *h, const char *) {
  try {
    if (!dsmod_sdk::HostAbiMatches(h) || h->title_id != Title ||
        BuildHex(*h) != Build || !h->read_memory || !h->read_romfs)
      return nullptr;
    return new Module(*h);
  } catch (...) {
    return nullptr;
  }
}
void Destroy(void *p) { delete static_cast<Module *>(p); }
void Sample(void *p, const EdenDsmodHostApi *h) {
  try {
    if (p && h)
      static_cast<Module *>(p)->Sample(*h);
  } catch (...) {
    if (h && h->publish_i64)
      h->publish_i64(h->userdata, "ot0.ready", 0);
  }
}
void Tick(void *p, const EdenDsmodHostApi *h) {
  try {
    if (p && h)
      static_cast<Module *>(p)->Tick(*h);
  } catch (...) {
  }
}
void ConfigureWrite(void *p, const EdenDsmodHostWriteApi *api) {
  try {
    if (p && api && api->version == EDEN_DSMOD_WRITE_EXT_VERSION &&
        api->struct_size == sizeof(*api) &&
        api->abi_hash == EDEN_DSMOD_WRITE_EXT_HASH && api->write_batch)
      static_cast<Module *>(p)->SetWriteApi(*api);
  } catch (...) {
  }
}
void Configure(void *p, const EdenDsmodHostExtensions *api) {
  try {
    if(p && api && api->version==EDEN_DSMOD_EXT_VERSION &&
       api->struct_size==sizeof(*api) && api->abi_hash==EDEN_DSMOD_EXT_HASH)
      static_cast<Module*>(p)->SetExtensions(*api);
  } catch (...) {
  }
}
EdenDsmodBool Action(void *p, const char *a, std::int64_t arg) {
  try {
    return p && a && static_cast<Module *>(p)->Action(a, arg)
               ? EDEN_DSMOD_TRUE
               : EDEN_DSMOD_FALSE;
  } catch (...) {
    return EDEN_DSMOD_FALSE;
  }
}
EdenDsmodBool Image(void *p, const EdenDsmodHostApi *h, const char *key,
                    void *receiver, EdenDsmodImageSink sink) {
  try {
    return p && h && key &&
                   static_cast<Module *>(p)->Image(*h, key, receiver, sink)
               ? EDEN_DSMOD_TRUE
               : EDEN_DSMOD_FALSE;
  } catch (...) {
    return EDEN_DSMOD_FALSE;
  }
}
const EdenDsmodModuleApi Api{EDEN_DSMOD_MODULE_ABI_VERSION,
                             sizeof(EdenDsmodModuleApi),
                             0,
                             EDEN_DSMOD_MODULE_ABI_HASH,
                             Title,
                             "Octopath Traveler 0 companion",
                             EDEN_DSMOD_CAP_EXTENSIONS |
                                 EDEN_DSMOD_CAP_ROMFS_READ |
                                 EDEN_DSMOD_CAP_TICK_WHEN_HIDDEN,
                             Supports,
                             Create,
                             Destroy,
                             Sample,
                             Tick};
const EdenDsmodModuleExtensions Extensions{EDEN_DSMOD_EXT_VERSION,
                                           sizeof(EdenDsmodModuleExtensions),
                                           EDEN_DSMOD_EXT_HASH,
                                           Configure,
                                           Action,
                                           Image};
const EdenDsmodModuleWriteExtensions WriteExtensions{
    EDEN_DSMOD_WRITE_EXT_VERSION, sizeof(EdenDsmodModuleWriteExtensions),
    EDEN_DSMOD_WRITE_EXT_HASH, ConfigureWrite};
} // namespace
DSMOD_SDK_EXPORT_MODULE(Api)
DSMOD_SDK_EXPORT_EXTENSIONS(Extensions)
DSMOD_SDK_EXPORT_WRITE_EXTENSIONS(WriteExtensions)
