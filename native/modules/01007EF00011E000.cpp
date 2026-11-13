// SPDX-License-Identifier: GPL-3.0-or-later
// BOTW 1.6.0 private live slice with a guarded native UI-thread selection bridge.
#include <ctime>
#include "botw_runes.h"
#include "botw_gear.h"
#include "botw_bridge.h"
#include "botw_ready.h"
#include "botw_ready_panes.h"
#include "botw_quick_detail.h"
#include "botw_detail_line.h"
#include "botw_hud.h"
#include "botw_inventory.h"
#include "botw_item_status.h"
#include "botw_pause_details.h"
#include "botw_recent_gear.h"
#include "botw_environment.h"
#include "botw_quests.h"
#include "botw_map.h"
#include "botw_map_markers.h"
#include "botw_map_page_overlay.h"
#include "botw_map_pins.h"
#include "botw_saved_map_icons.h"
#include "botw_map_area.h"
#include "botw_map_landmarks.h"
#include "botw_map_places.h"
#include "botw_map_regions.h"
#include "botw_map_names.h"
#include "botw_map_moving.h"
#include "botw_map_koroks.h"
#include "botw_map_death.h"
#include "botw_map_special.h"
#include "botw_map_ready_marks.h"
#include "botw_map_label_catalog.h"
#include "botw_map_labels.h"
#include "botw_sensor.h"
#include "botw_temperature_dial.h"
#include "botw_effects.h"
#include "botw_tracker_view.h"
#include "botw_ready_counters.h"
#include "botw_ready_pins.h"
#include "botw_map_view.h"
#include "botw_event_hold.h"
#include "botw_press.h"
#include "botw_states.h"
#include "botw_drawn.h"
#include "botw_map_ui.h"
#include "botw_warp_ui.h"
#include "botw_cards.h"
#include "botw_ready_radial.h"
#include "botw_shrine_compass.h"
#include "botw_heal_advisor.h"
#include "botw_temperature_controller.h"
#include "botw_recipes.h"
#include "botw_msbt_reflow.h" // iteration 8 item 11 / F6: description reflow
#include "botw_key.h"
#include <cmath>
#include <cstdio>
#include <chrono>
#include <new>
#include <algorithm>
#ifdef BOTW_PERF_INSTRUMENT
namespace botw { namespace perf {
enum Cat {
  MainSnapshot, MapView, Equipment, Quests, MapBlock, Compass, NativeQuick,
  BridgeTrackersTravelCounters, Recipes, HoldSequences, HoldScope, ReadyRecentGearMod,
  MapEditState, InventoryAvailability, QuickOverlay, InventoryGrid, ItemSlots, PublishMisc,
  TileNames, MapPublishChain, Count
};
inline const char* Name(Cat c) {
  switch (c) {
    case MainSnapshot: return "MainSnapshot(pouch/currency/clock/stamina/noise/sensor/tempdial/env/rune/states)";
    case MapView: return "MapView(position+map_view 100ms+30Hz)";
    case Equipment: return "Equipment(player_equipment/effects/temp_card/heal/recent-seed gear_modifier)";
    case Quests: return "Quests(quests+quest_map_markers)";
    case MapBlock: return "MapBlock(map_snapshot/pins/saved_icons/area/landmarks/places/regions/names/moving/koroks/death/special/labels/caption)";
    case Compass: return "Compass(shrine_compass 100ms)";
    case NativeQuick: return "NativeQuick(native_quick+drawn_state, unconditional every tick)";
    case BridgeTrackersTravelCounters: return "BridgeTrackersTravelCounters(bridge.poll+trackers.sample+travel.sample+counters.publish)";
    case Recipes: return "Recipes(recipes.publish)";
    case HoldSequences: return "HoldSequences(recipe_hold+hold_unpick held_tray/held_carry)";
    case HoldScope: return "HoldScope(held_tray/held_carry 100ms on inventory/recipes)";
    case ReadyRecentGearMod: return "ReadyRecentGearMod(recent weapon/bow gear_modifier, every tick)";
    case MapEditState: return "MapEditState(map tap/marker/delete logic, map_edit_snapshot)";
    case InventoryAvailability: return "InventoryAvailability(consumable() scan)";
    case QuickOverlay: return "QuickOverlay(D-pad/touch quick-select picker block)";
    case InventoryGrid: return "InventoryGrid(selected-item detail + grid loop)";
    case ItemSlots: return "ItemSlots(botw.item.N publish loop)";
    case PublishMisc: return "PublishMisc(quest journal/champions/map+sensor+temp publish/hold-tray status, between QuickOverlay and InventoryGrid -- residual bucket, sub-timed below)";
    case TileNames: return "  sub: TileNames(botw.map.tile.{1,3}.0..119, 240 publish_text/tick, unconditional)";
    case MapPublishChain: return "  sub: MapPublishChain(publish_map_landmarks/places/regions/names/moving/koroks/death/special/labels, unconditional)";
    default: return "?";
  }
}
struct Accum { uint64_t ns = 0; uint64_t calls = 0; };
inline Accum g_accum[Count];
inline std::chrono::steady_clock::time_point g_last_report{};
struct Timer {
  Cat cat; std::chrono::steady_clock::time_point t0;
  explicit Timer(Cat c) : cat(c), t0(std::chrono::steady_clock::now()) {}
  ~Timer() {
    const auto dt = std::chrono::steady_clock::now() - t0;
    g_accum[cat].ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    g_accum[cat].calls += 1;
  }
};
inline void MaybeReport(const EdenDsmodHostApi* h, std::chrono::steady_clock::time_point now) {
  if (g_last_report.time_since_epoch().count() == 0) { g_last_report = now; return; }
  if (now - g_last_report < std::chrono::seconds(5)) return;
  const double window_s = std::chrono::duration<double>(now - g_last_report).count();
  char line[256];
  if (h && h->log) {
    std::snprintf(line, sizeof(line), "[botw-perf] ---- %.1fs window ----", window_s);
    h->log(h->userdata, 1u, line);
  }
  // Rank by total ns this window (descending) so the log's order IS the ranking.
  int order[Count]; for (int i = 0; i < Count; ++i) order[i] = i;
  std::sort(order, order + Count, [](int a, int b){ return g_accum[a].ns > g_accum[b].ns; });
  for (int idx = 0; idx < Count; ++idx) {
    const Cat c = static_cast<Cat>(order[idx]);
    const auto& a = g_accum[c];
    if (a.calls == 0) continue;
    const double avg_us = double(a.ns) / 1000.0 / double(a.calls);
    const double total_ms_per_s = double(a.ns) / 1e6 / window_s;
    if (h && h->log) {
      std::snprintf(line, sizeof(line), "[botw-perf] %-70s calls=%6llu avg=%7.2fus total=%6.3fms/s",
        Name(c), (unsigned long long)a.calls, avg_us, total_ms_per_s);
      h->log(h->userdata, 1u, line);
    }
  }
  if (h && h->publish_text) {
    char summary[512];
    std::snprintf(summary, sizeof(summary), "see run.log [botw-perf], %d categories, window=%.1fs", Count, window_s);
    h->publish_text(h->userdata, "botw.perf.report", summary);
  }
  for (auto& a : g_accum) a = Accum{};
  g_last_report = now;
}
}} // namespace botw::perf
#define BOTW_PERF_TIMER(cat) botw::perf::Timer _botw_perf_timer_##__LINE__(botw::perf::cat)
// Non-scoping variant for spans whose variables must outlive the measured region (e.g. the
// Inventory grid's `items`/`selected`/`art_milliseconds` locals, read by the publish loops
// right after): no new {} scope, just a manual start/stop pair around the same statements.
#define BOTW_PERF_BEGIN(cat) const auto _botw_perf_t0_##cat = std::chrono::steady_clock::now()
#define BOTW_PERF_END(cat) do { \
    auto& _a = botw::perf::g_accum[botw::perf::cat]; \
    _a.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _botw_perf_t0_##cat).count()); \
    _a.calls += 1; \
  } while (0)
#else
#define BOTW_PERF_TIMER(cat) do {} while (0)
#define BOTW_PERF_BEGIN(cat) do {} while (0)
#define BOTW_PERF_END(cat) do {} while (0)
#endif


namespace {
constexpr const char* map_tools[]={"Sword stamp","Bow stamp","Shield stamp","Pot stamp","Star stamp","Chest stamp","Red pin","Blue pin","Yellow pin","Green pin","Purple pin","Skull stamp","Leaf stamp","Crystal stamp"};
const char* map_marker_name(int kind) {
  for(unsigned i=0;i<14;++i)if(botw::map_edit_kind(i)==kind)return map_tools[i];
  return "Marker";
}
// Perf, CORRECTED after a coordinator catch: the first version of this gate
// used page=="map" only, which silently blanked the Ready circle's own koroks/landmarks/places/
// stamps/death/special/editable-marker overlays (ReadyHyrule has its own "dynamic_markers" entry
// for every one of those groups -- checked prototype/dualscreen/manifest.json top-level "map.areas.
// ReadyHyrule.dynamic_markers", not page-local bind text, which is where the group->key-prefix
// wiring actually lives; a literal text search of the "pages" array for these keys finds nothing on
// ANY page because the runtime resolves "{i}"-templated dynamic_marker specs against an area, not
// a page, and the area is what areas Ready and Map genuinely share). FullHyrule (Map) needs
// everything ReadyHyrule needs plus a few more groups (native_pins/places_shop/places_backing/
// native_horse/full_map_hits/quest_destinations, all published elsewhere, untouched here); only
// TrackerHyrule (tracker_map page) uses a wholly different key family (botw.tracker.map.*, not
// published by any function this flag touches) and pages with no "map" widget at all (inventory,
// quest, recipes, trackers, mainmenu, loading) need none of it. So the only safe skip is on that
// specific "no map widget at all" page set -- Ready and Map both need the full per-entry publish.
bool needs_full_map_markers(const char* page) {
  if(!page)return true; // unknown/transition tick: default to correct, cost is a few ticks at most
  for(const char* skip:{"inventory","quest","recipes","trackers","tracker_map","mainmenu","loading"})
    if(std::strcmp(page,skip)==0)return false;
  return true; // ready, map, or any other/future page: stay correct by default
}
struct State {
  botw::PublishReplay replay;bool event_holding{}; // iteration 12 item 5: event hold (botw_event_hold.h)
  botw::PressFlash press; // iteration 13: pressed-state flash for command-bar buttons (botw_press.h)
  botw::GameStates states;botw::StateLatch state_latch;bool held_picker_open{};bool held_focus_none{};botw::TrackerFlagReader state_flags; // game-state readers (RE-FINDINGS 2026-10-18)
  bool held_release_deferred{};uint64_t held_release_target{};std::chrono::steady_clock::time_point held_release_at{}; // bug-B fix: retry the release-on-None unequip across ticks (first attempt can race the native wheel's own close animation, see HANDOFF)
  botw::DrawnState drawn;botw::TrackerView trackers;botw::ReadyCounters counters;botw::MapUi map_ui;botw::RecipesView recipes;
  std::vector<std::string> recipe_hold;size_t recipe_hold_index{};uint32_t recipe_hold_epoch{};std::chrono::steady_clock::time_point recipe_hold_at{};
  // Item 8d, iteration 8 round 2 (2026-10-18): "tap a box unit -> remove just that one unit".
  // Same shape as recipe_hold above: a vector of native item addresses to replay (the KEPT
  // slots, in original tray order, from botw::hold_unpick_keep) + an index of how many of
  // them the bridge has already re-held, driven forward one confirmed native call at a time.
  // Phase 0 (put_away_done==false): wait for the bulk put-away (op 12) to confirm an empty
  // tray. Phase 1 (put_away_done==true): re-issue hold (op 11) for hold_unpick[hold_unpick_index]
  // once tray.count==hold_unpick_index, same confirm-before-advance discipline recipe_hold uses.
  std::vector<uint64_t> hold_unpick;size_t hold_unpick_index{};bool hold_unpick_put_away_done{};
  uint32_t hold_unpick_epoch{};std::chrono::steady_clock::time_point hold_unpick_at{};
  botw::SelectionBridge bridge;
  botw::TravelUi travel; // Travel to a discovered shrine/tower
  std::string ready_hint;
  botw::Snapshot data;
  std::array<botw::RecentGear,2> recent;std::array<bool,2> recent_actor_valid{};
  botw::ActiveEffects effects;
  bool recent_deferred{};uint64_t recent_target{};botw::Snapshot recent_display;uint32_t recent_epoch{};std::chrono::steady_clock::time_point recent_action_at{};
  // Iteration 12 item 3 (owner 2026-10-22): Ready weapon swap with an empty side. recent_remove
  // makes the deferred request the native Remove (DUO9 kind7) instead of Equip. swap_promote is
  // the secondary being equipped into an EMPTY primary: once that instance is seen equipped the
  // secondary becomes None (no auto-picked replacement). swap_demote is the primary being removed
  // because the secondary was empty: once seen unequipped with nothing equipped it is pinned as
  // the secondary. Both are companion-only and expire (swap_at) if the native result never shows.
  bool recent_remove{};botw::RecentGear::Identity swap_promote,swap_demote;std::chrono::steady_clock::time_point swap_at{};
  botw::CurrencySnapshot currency;
  botw::WorldClock clock;
  botw::Stamina stamina;
  botw::Noise noise;
  botw::Sensor sensor;
  botw::TemperatureDial temperature_dial;
  // Minimap helper cards: 1 shrine compass, 2 temperature, 3 food.
  botw::CardFrame cards;botw::TemperatureCardController temp_card;
  botw::ShrineCompassController shrine_compass;std::chrono::steady_clock::time_point compass_next{};
  // Iteration 14 item 9: the last 10 Hz compass result, republished EVERY tick (the host clears
  // tick_snapshot each tick, so publishing only on update ticks made the card text/arrow blink).
  botw::ShrineCompass compass_out{};bool compass_shown{};
  botw::Environment environment;
  botw::QuestSnapshot quests;
  bool quest_deferred{};uint64_t quest_target{};botw::QuestSnapshot quest_display;uint32_t quest_epoch{};std::chrono::steady_clock::time_point quest_action_at{};
  botw::MapSnapshot map;
  botw::QuestMapMarkers quest_markers;
  botw::MapPins pins;
  botw::SavedMapIcons saved_icons;
  botw::MapArea area;
  botw::MapLandmarks landmarks;
  botw::MapPlaces places;
  botw::MapRegions regions;
  botw::MapNames names;
  botw::MapMoving moving;
  botw::MapKoroks koroks;
  botw::MapDeath death;
  botw::MapSpecial specials;
  botw::MapLabels labels;
  botw::ReadyCaption caption;
  botw::MapPosition position;
  botw::MapView view;
  int32_t quest_category{},quest_pane{};
  // Iteration 12 item 14: the Inventory footer status line is on screen this frame.
  bool inventory_status_shown{};
  botw::ReadyPanes ready_panes;
  botw::HealAdvisor heal; // card 3 = food
  uint64_t quest_selected{};
  std::chrono::steady_clock::time_point quest_next{};
  std::chrono::steady_clock::time_point map_next{},map_retry_next{},view_next{};
  // Perf round 3b (coordinator follow-up): the published Map-page
  // viewport cull rect is now STICKY across ticks (hysteresis) instead of being recomputed from
  // the raw, continuously-jittering @map_view_* floats every single tick. During a drag/pinch
  // those floats change by a sub-pixel amount almost every tick; recomputing the margined cull
  // rect from them every tick made borderline entries flip in/out of the published set on ticks
  // where nothing the player can see actually changed, which looked like extra redraw-thread work
  // during panning (coordinator-reported DSModRedraw regression, measured +23-39% at the round-3a
  // candidate). Recomputing only when the actual (unmargined) drawn view is no longer fully
  // covered by the last computed cull rect is both cheaper AND still exactly correct: the stored
  // rect always contains the current drawn view (with margin) by construction, so nothing can go
  // missing at an edge -- see the recompute condition where this is used.
  botw::MapViewCull map_cull{};
  int map_palette{-1},map_selected{-1},map_deferred_palette{-1};botw::MapEditMarker map_selected_marker{};
  bool map_tap_deferred{},map_marker_deferred{},map_delete_deferred{};int64_t map_tap_before{},map_last_tap_seq{};botw::SavedMapIcons::Icon map_clicked_icon{};
  uint32_t map_action_epoch{};std::chrono::steady_clock::time_point map_action_at{};
  botw::InventoryView inventory;
  botw::HoldTargets hold_targets;botw::HeldTray held;botw::HeldCarry carry;std::chrono::steady_clock::time_point hold_next{};
  std::chrono::steady_clock::time_point hold_refused_until{};
  // Fix round A: last native tray/carry pair that read valid, kept for 300 ms across a torn read
  // (double-capture mismatch while the game updates the CarryBox) so the published hold state and
  // the tile hold tokens do not flicker; every request re-reads both and re-validates in full.
  std::chrono::steady_clock::time_point hold_valid_at{};
  uint32_t hold_decline{};
  std::string status_seen;std::chrono::steady_clock::time_point status_at{};
  botw::CurrentRune current_rune;
  botw::RuneSnapshot runes;
  botw::GearSnapshot gear;
  botw::QuickOverlay quick;
  botw::QuickKind touch_kind{};uint32_t touch_epoch{};botw::GearSnapshot touch_gear;
  bool secondary_picker{};botw::RecentGear::Identity secondary_focus,touch_focus;int touch_rune_focus{-1};
  // "None" secondary weapon (owner 2026-10-18): a companion-only flag, never reset by the
  // sample-gap/epoch_changed/context resets elsewhere -- it survives loads, menus and sample
  // gaps by construction (it is simply not in any of those reset lists). Cleared only by a
  // fresh secondary_confirm pin; set only by the secondary_none action. Iteration 12 item 3 adds
  // the Ready flick: set when the secondary moves into an empty primary, cleared when the primary
  // moves into an empty secondary (swap_promote/swap_demote below).
  bool secondary_none{};
  botw::WorldRunes touch_runes,touch_rune_display;int32_t touch_rune_id{-1};
  // Iteration 14 item 8 (diagnostic): life before a Ready-radial Eat, checked after "Consumed".
  int32_t eat_life_before{-1};bool eat_expect_heal{};std::chrono::steady_clock::time_point eat_at{};
  bool touch_select_deferred{};uint64_t touch_target{};botw::Snapshot touch_display;std::chrono::steady_clock::time_point touch_action_at{};
  // Iteration 8 item 1: which native action the deferred touch_target request performs.
  bool touch_remove{};
  std::chrono::steady_clock::time_point last_sample{};
  botw::UiContext context{botw::UiContext::Unknown};
  std::chrono::steady_clock::time_point next{};
};
enum class InventorySlot{None,Equip,Consume,Remove};
InventorySlot inventory_slot(const State& s,const EdenDsmodHostApi& h,const botw::Item& i);
// Quest page order = native Adventure Log (iteration 12 item 10): one list per category with
// every journal quest, active entries first, then completed ones, each in native quest-data order
// (live check 2026-10-22: Main lists Destroy Ganon, then Follow the Sheikah Slate, The Isolated
// Plateau, ... exactly as the native tab). No active/completed filter exists natively.
std::vector<const botw::Quest*> quest_journal(const State& s) {
  std::vector<const botw::Quest*> journal;
  for(const auto& q:s.quests.entries)if(q.category==s.quest_category)journal.push_back(&q);
  // Native Adventure Log order (iteration 14 item 7, live save B + DLC): open quests first, then
  // completed DLC quests, then completed base-game quests -- each group in quest-data order.
  auto rank=[](const botw::Quest* q){return q->state==1?0:(q->dlc&6u)?1:2;};
  std::stable_sort(journal.begin(),journal.end(),[&](const auto* a,const auto* b){return rank(a)<rank(b);});
  return journal;
}
// Ready arrow/recent swaps are queued and committed by the next fresh sample.
void defer_recent(State& s,const botw::Item& item,bool remove=false) {
  s.recent_target=item.address;s.recent_remove=remove;s.recent_display=s.data;s.recent_epoch=s.bridge.epoch;
  s.recent_action_at=std::chrono::steady_clock::now();s.recent_deferred=true;
}
EdenDsmodBool supports(const char *b) {
#ifdef BOTW_190_INVENTORY_EVIDENCE
  if (b && std::strcmp(b, botw::InventoryBuild190) == 0) return true;
#endif
  return b && std::strcmp(b, botw::Build) == 0;
}
void *create(const EdenDsmodHostApi *, const char *) {
  return new (std::nothrow) State;
}
void destroy(void *p) {
  static_cast<State*>(p)->bridge.cancel();delete static_cast<State *>(p);
}
// Perf instrumentation (coordinator-requested per-reader ranking).
// Compile-time only: off unless BOTW_PERF_INSTRUMENT is defined, so a normal build is
// byte-for-byte unaffected except for this dead code. Never shipped (debug-only build flag).
// Iteration 12 item 5: one held sample = the last World/Menu record, then the live game-state keys
// (event/cutscene/dialogue, the dim latch, the page latch) and no "unknown" screen on top.
void publish_event_hold(const State& s,const EdenDsmodHostApi& h) {
  s.replay.replay(h);
  botw::publish_states(h,s.states);
  botw::publish_state_latch(h,s.state_latch);
  h.publish_i64(h.userdata,"botw.state.unknown",0);
}
void sample(void *p, const EdenDsmodHostApi *h) {
  try {
    auto &s = *static_cast<State *>(p);
    // Iteration 8 item 11 / F6: one process-wide cache (game text is static; see NOTES.md
    // "cache lifetime"). publish_description_clean() fails soft to "" (unknown hash, msbt
    // path shape outside the two the table ever uses, or the file/label could not be read) --
    // the manifest keeps each widget's existing text_map/text_bind as the fallback render in
    // that case (see NOTES.md "fallback").
    static botw::DescriptionCache description_cache;
    const auto publish_description_clean=[&](const char* output,int64_t hash) {
      const std::string& cleaned=hash>=0
        ? description_cache.Lookup(h->read_romfs,h->userdata,uint64_t(hash))
        : std::string();
      h->publish_text(h->userdata,output,cleaned.c_str());
    };
    auto now = std::chrono::steady_clock::now();
    bool inventory_sampled=false;
    const auto previous_context=s.context;
    // A sample gap (>250 ms: a frame stall, e.g. the break effect on the Thor, or a debugger halt)
    // drops transient interaction state only. Iteration 14 item 6: the secondary-weapon/bow history
    // (s.recent) is kept -- every RecentGear lookup re-checks address+actor+type+value>0 against the
    // live pouch, and epoch/title/loading still clear it; wiping it here lost the secondary.
    if (s.last_sample.time_since_epoch().count() && now-s.last_sample>std::chrono::milliseconds(250))
      {s.quick={};s.touch_kind=botw::QuickKind::None;s.touch_gear={};s.touch_select_deferred=false;s.recent_deferred=false;s.quest_deferred=false;s.swap_promote={};s.swap_demote={};s.secondary_picker=false;s.secondary_focus={};}
    s.last_sample=now;
    // Iteration 13: raw host (before the replay wrap and every early return), so a stale pressed
    // id is never recorded/replayed and always decays to 0 within PressFlash::hold.
    h->publish_i64(h->userdata,"botw.ui.pressed",s.press.value(now));
    // Native UI loops (botw_fx_curves.h): exact curve values from the player's layouts on 15 Hz
    // steps, every state (the loading page's spinner too); fallbacks until the layouts are loaded.
    botw::fx::Publish(*h,botw::fx::Sample(botw::fx::Curves(),
        std::floor(std::chrono::duration<double>(now.time_since_epoch()).count()*15.)/15.));
    // Iteration 12 item 5 (botw_event_hold.h): cutscene/dialogue/warp event right after World/Menu
    // -> keep showing the current page (last record) under the dim, no "Waiting" screen.
    if(s.event_holding && now<s.next){publish_event_hold(s,*h);return;}
    if(now>=s.next && s.replay.valid()) {
      const auto ctx=botw::context(*h);
      if(ctx==botw::UiContext::Unknown) {
        auto g=botw::game_states(*h,&s.state_flags);
        if(botw::event_hold(s.context,ctx,g,s.replay.valid(),s.state_latch.dim)) {
          s.event_holding=true;s.states=std::move(g);
          s.state_latch.update(s.states,std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
          s.next=now+std::chrono::milliseconds(100);
          publish_event_hold(s,*h);return;
        }
      }
    }
    s.event_holding=false;
    // Record this reader-cadence sample's publishes (committed below only for World/Menu).
    const bool record_replay=now>=s.next;
    if(record_replay){h=s.replay.wrap(h);s.replay.begin();}
    if (now >= s.next) {
      inventory_sampled=true;
      { BOTW_PERF_TIMER(MainSnapshot);
      s.context = botw::context(*h);
      if (botw::inventory_context(s.context)) {
        s.data = botw::pouch(*h);
        // CRITIQUE-3 fix round B: the cached pouch order is the raw intrusive-list order, which
        // is not the game's own per-category grid order (it was the exact reverse in every
        // category tested). Reorder to the native grid order when it reads back consistently;
        // keep the cached pouch order (previous behaviour) on any mismatch.
        if (s.data.valid) {
          auto native = botw::native_order(*h, s.data);
          if (native.valid) s.data.items = std::move(native.items);
        }
      } else s.data = botw::Snapshot{};
      s.currency=botw::currency(*h);
      s.clock=botw::world_clock(*h);
      s.stamina=botw::stamina(*h);
      s.noise=botw::noise(*h);
      s.sensor=botw::sensor(*h);
      s.temperature_dial=botw::temperature_dial(*h);
      s.environment=botw::environment(*h);
      }
      { BOTW_PERF_TIMER(MapView);
      s.position=botw::map_position(*h);
      s.view=botw::map_view(*h,s.view.cone_pane);
      }
      s.current_rune=botw::current_rune(*h);
      s.states=botw::game_states(*h,&s.state_flags);
      s.state_latch.update(s.states,std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
      if (botw::context(*h) != s.context) {
        s.context = botw::UiContext::Unknown;
        s.data = {};s.currency={};s.current_rune={};s.clock={};s.stamina={};s.noise={};s.sensor={};s.temperature_dial={};s.environment={};
        s.position={};s.map={};s.view={};
        s.shrine_compass=botw::ShrineCompassController{};s.compass_out={};s.compass_shown=false;
      }
      s.recent_actor_valid={};
      { BOTW_PERF_TIMER(Equipment);
      if(s.context==botw::UiContext::World && s.data.valid) {
        const auto attached=botw::player_equipment(*h);
        // 2026-10-19 refactor/optimize: this whole block runs only inside the enclosing
        // "s.context==World && s.data.valid" branch (just above), which nothing in between re-reads
        // from the host -- the three botw::context(*h)==World re-checks below were each a second,
        // fully redundant double-sampled host read (botw::context does two full UiSample reads --
        // ~7-8 guest memory reads each -- to detect torn samples) of a value already known true.
        s.effects=attached.valid ? botw::active_effects(*h,attached.player) : botw::ActiveEffects{};
        s.temp_card.update(*h,s.data,attached.valid ? attached.player : 0,
          std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        s.heal.low.observe(attached.valid ? botw::player_health(*h,attached.info,attached.player) : botw::PlayerHealth{},s.bridge.epoch,now);
        if(s.heal.low.low())s.heal.refresh(*h,s.data);
        // Iteration 14 item 8: a heart food eaten from the Ready radial must raise life. Native Eat
        // applies health itself; if a confirmed Eat left life unchanged, say so (and log it) instead
        // of a plain "Consumed", so a device-only failure is visible to the player and in the log.
        if(s.eat_life_before>=0 && !s.bridge.pending) {
          const auto& hp=s.heal.low.health();
          if(s.bridge.status && std::strcmp(s.bridge.status,"Consumed")==0 && hp.valid) {
            if(s.eat_expect_heal && hp.life<=s.eat_life_before) {
              s.bridge.status="No hearts restored";
              if(h->log){char m[96];std::snprintf(m,sizeof m,"BOTW eat: confirmed but life %d -> %d",int(s.eat_life_before),int(hp.life));h->log(h->userdata,2,m);}
            }
            s.eat_life_before=-1;
          } else if(now-s.eat_at>std::chrono::seconds(5))s.eat_life_before=-1;
        }
        if(attached.valid)for(unsigned type=0;type<2;++type) {
          const botw::Item* equipped{};unsigned count{};
          for(const auto& item:s.data.items)if(item.type==int(type) && item.equipped){equipped=&item;++count;}
          const auto& live_slot=attached.slots[type?2:0];
          if(count==0 && live_slot.name.empty()) {
            s.recent_actor_valid[type]=true;s.recent[type].update(s.data,int(type),"");
            // Item 3 (b): the removed primary becomes the secondary (pin() rejects a gone/broken instance).
            if(type==0 && s.swap_demote.address){if(s.recent[0].pin(s.data,s.swap_demote,0))s.secondary_none=false;s.swap_demote={};}
          }
          if(count==1 && equipped->actor==live_slot.name && equipped->value>0) {
            s.recent_actor_valid[type]=true;s.recent[type].update(s.data,int(type),equipped->actor);
            s.recent[type].seed(s.data,int(type),[&](const botw::Item& i){const auto m=botw::gear_modifier(*h,i.address);return botw::durability_percent(i,m.flags,m.value);});
            if(type==0) {
              const botw::RecentGear::Identity now_equipped{equipped->address,equipped->actor};
              // Item 3 (a): the secondary moved into an empty primary, so nothing is left to be
              // the secondary -- None, instead of update()/seed() offering another weapon.
              if(s.swap_promote.address && s.swap_promote==now_equipped){s.secondary_none=true;s.recent[0].pinned={};}
              s.swap_promote={};
              if(s.swap_demote!=now_equipped)s.swap_demote={};
            }
          }
        }
      } else {
        s.temp_card.lost();
        if(s.context!=botw::UiContext::Menu)s.effects={};
        // Menu/drop transitions can fail the double-sampled context read for a
        // frame. Hide unavailable targets, but retain their instance identities
        // until a confirmed load/title, epoch change, or fresh pouch invalidates them.
        if(s.context==botw::UiContext::Title || s.context==botw::UiContext::Transition ||
           (s.states.valid && (s.states.title || s.states.loading))){s.recent={};s.swap_promote={};s.swap_demote={};}
      }
      }
      s.next = now + std::chrono::milliseconds(100);
    }
    // The native minimap turn and camera cone on every sample (the Ready map's turn follows the
    // camera at the redraw rate instead of the 100 ms readers); a failed read keeps the last one
    // and the 100 ms pass above decides validity.
    else if(s.context==botw::UiContext::World && s.position.valid && now>=s.view_next) {
      BOTW_PERF_TIMER(MapView);
      // At most ~30 Hz: the companion republishes at 30 Hz and the game turns its minimap at
      // its own frame rate (30 fps), so reading every 60 Hz tick would only double the cost.
      s.view_next=now+std::chrono::milliseconds(30);
      const auto turned=botw::map_view(*h,s.view.cone_pane);
      if(turned.valid)s.view=turned;
    }
    // Quests are only shown in World/menu; elsewhere they were read and discarded.
    // Clearing also clears the cadence so re-entry reads at once, like the Map reader.
    if(botw::inventory_context(s.context) && now>=s.quest_next) {
      BOTW_PERF_TIMER(Quests);
      s.quests=botw::quests(*h);
      s.quest_markers=botw::quest_map_markers(*h,s.quests);
      s.quest_next=now+std::chrono::milliseconds(500);
    }
    if(!botw::inventory_context(s.context)){s.quests={};s.quest_markers={};s.quest_next={};}
    // A native popup release may cross UI signatures between bounded reads.
    // Recover immediately on re-entry, and retry failed reads on the fast cadence.
    // Successful map data keeps its ordinary 500 ms refresh; no stale snapshot is retained.
    if(s.context==botw::UiContext::World && previous_context!=botw::UiContext::World)s.map_next={};
    if(botw::inventory_context(s.context) && now>=s.map_next) {
      BOTW_PERF_TIMER(MapBlock);
      s.map=botw::map_snapshot(*h);
      s.pins=botw::map_pins(*h);
      s.saved_icons=botw::saved_map_icons(*h);
      s.area=botw::map_area(*h);
      {auto v=botw::map_landmarks(*h);if(v.valid||botw::main_field_stage(*h))s.landmarks=v;}
      {auto v=botw::map_places(*h);if(v.valid||botw::main_field_stage(*h))s.places=v;}
      s.regions=botw::map_regions(*h);
      {auto v=botw::map_names(*h);if(v.valid||botw::main_field_stage(*h))s.names=v;}
      s.moving=botw::map_moving(*h);s.koroks=botw::map_koroks(*h);s.death=botw::map_death(*h);s.specials=botw::map_special(*h);
      s.labels=botw::map_labels(s.places,s.landmarks,s.moving);
      s.caption=botw::ready_caption(s.labels,s.position,s.map_ui,&s.specials);
      s.map_next=now+std::chrono::milliseconds(500);
      s.map_retry_next=now+std::chrono::milliseconds(100);
    } else if(botw::inventory_context(s.context) && !s.map.valid && now>=s.map_retry_next) {
      BOTW_PERF_TIMER(MapBlock);
      // Retrying just the base map avoids re-reading every overlay at the fast rate.
      s.map=botw::map_snapshot(*h);
      s.map_retry_next=now+std::chrono::milliseconds(100);
    }
    // Item 15 (owner 2026-10-20): landmarks/places/names are deliberately NOT cleared here.
    // This blanket reset also fires during the black-screen Transition between the field and a
    // shrine interior (context briefly leaves World/Menu), which wiped the item-15 cache above
    // (lines 272-275) before the shrine's own gameplay frames ever ran -- the cache-preserving
    // read at 500 ms cadence never got a chance to apply because this reset ran first and threw
    // the last-good snapshot away regardless. They keep whatever the 500 ms cache logic above
    // last decided (fresh valid data, or the previous valid snapshot while off the main field);
    // map/position/view/pins/etc. are unaffected -- those have no owner-requested hold-last
    // behavior and keep resetting on every non-inventory context exactly as before.
    if(!botw::inventory_context(s.context)){s.map_next={};s.map_retry_next={};s.map={};s.position={};s.view={};s.pins={};s.saved_icons={};s.area={};s.regions={};s.moving={};s.koroks={};s.death={};s.specials={};s.labels=botw::map_labels(s.places,s.landmarks,s.moving);s.caption={};}
    const char *page = h->get_text ? h->get_text(h->userdata, "__page") : nullptr;
    // Opt-in shrine compass (card 1): computed/published at ~10 Hz and ONLY while open (cheap
    // no-op otherwise). The shared card frame below decides whether card 1 may be open at all.
    if(now>=s.compass_next) {
      BOTW_PERF_TIMER(Compass);
      const bool open=s.cards.active==1;
      const auto out=s.shrine_compass.update(*h,open,s.position,botw::view_rotation(s.view));
      if(open && s.shrine_compass.close_requested)s.cards.close(1);
      s.compass_out=out;s.compass_shown=open&&!s.shrine_compass.close_requested;
      s.compass_next=now+std::chrono::milliseconds(100);
    }
    // Iteration 14 item 9: publish the cached result every tick; the card closing (cards.active
    // leaving 1) hides it on the same tick instead of waiting for the next 100 ms update.
    botw::publish_shrine_compass(*h,s.compass_shown&&s.cards.active==1,s.compass_out);
    botw::NativeQuick observed;
    { BOTW_PERF_TIMER(NativeQuick);
      observed = botw::native_quick(*h);
      if(inventory_sampled)s.drawn=s.context==botw::UiContext::World?botw::drawn_state(*h):botw::DrawnState{};
      if(s.context!=botw::UiContext::World)s.drawn={};
    }
    const auto& drawn=s.drawn;
    s.ready_panes.observe_drawn(drawn.valid,drawn.weapon||drawn.bow);
    const bool was_holding_pending=s.bridge.pending_hold;
    { BOTW_PERF_TIMER(BridgeTrackersTravelCounters);
      s.bridge.poll(*h,observed);
      s.trackers.sample(*h,s.data,s.landmarks,s.bridge.epoch,page);
      s.travel.sample(*h,page,s.map_ui,s.trackers,s.landmarks,s.bridge,s.context,s.position,now,s.states.shrine,s.states.shrine_number);
      s.counters.publish(*h,s.bridge.epoch,s.context==botw::UiContext::World,s.context==botw::UiContext::Menu);
    }
    { BOTW_PERF_TIMER(Recipes);
      s.recipes.publish(*h,s.data,page);
    }
    if(!s.recipe_hold.empty()) {
      BOTW_PERF_TIMER(HoldSequences);
      const bool scope=s.recipe_hold_epoch==s.bridge.epoch&&s.bridge.healthy&&s.bridge.hold_supported&&s.context==botw::UiContext::World&&s.data.valid&&
        now-s.recipe_hold_at<std::chrono::seconds(12)&&page&&(std::strcmp(page,"inventory")==0||std::strcmp(page,"recipes")==0);
      if(!scope){s.recipe_hold.clear();s.bridge.status="Recipe hold cancelled";}
      else if(std::strcmp(page,"inventory")==0&&!s.bridge.pending) {
        const auto tray=botw::held_tray(*h);const auto carry=botw::held_carry(*h);
        if(tray.valid&&carry.valid&&tray.count==s.recipe_hold_index) {
          bool processed=true;for(const auto& slot:tray.slots)if(slot.address&&!slot.flag8)processed=false;
          if(s.recipe_hold_index>=s.recipe_hold.size())s.recipe_hold.clear();
          else if(processed&&(tray.count==0||carry.active())) {
            const botw::Item* target{};for(const auto& item:s.data.items)if(item.type==7&&item.actor==s.recipe_hold[s.recipe_hold_index]&&item.value>0){if(target){target=nullptr;break;}target=&item;}
            if(!target){s.recipe_hold.clear();s.bridge.status="Recipe ingredient changed";}
            else if(s.bridge.request_hold(target->address,s.data)){++s.recipe_hold_index;s.hold_next={};s.next={};}
          }
        } else if(tray.valid&&tray.count>s.recipe_hold_index){s.recipe_hold.clear();s.bridge.status="Recipe hold changed";}
      }
    }
    h->publish_i64(h->userdata,"botw.recipes.open_inventory",!s.recipe_hold.empty());
    // Item 8d, iteration 8 round 2 (2026-10-18): the put-away-then-re-hold compound sequence,
    // structurally identical to recipe_hold above (fresh held_tray() read every tick, advance
    // exactly one confirmed native call per tick, the bridge's own hold_refusal/epoch checks
    // still gate every individual request_hold() call same as any other caller).
    if(!s.hold_unpick.empty()) {
      BOTW_PERF_TIMER(HoldSequences);
      const bool scope=s.hold_unpick_epoch==s.bridge.epoch&&s.bridge.healthy&&s.bridge.hold_supported&&s.context==botw::UiContext::World&&s.data.valid&&
        now-s.hold_unpick_at<std::chrono::seconds(12)&&page&&std::strcmp(page,"inventory")==0;
      if(!scope){s.hold_unpick.clear();s.hold_unpick_put_away_done=false;s.bridge.status="Hold adjustment cancelled";}
      else if(!s.bridge.pending) {
        const auto tray=botw::held_tray(*h);const auto carry=botw::held_carry(*h);
        if(!s.hold_unpick_put_away_done) {
          // Phase 0: wait for the bulk put-away (issued the instant the tap was accepted, see
          // the dispatcher below) to confirm every slot cleared before replaying any of them.
          if(tray.valid&&tray.count==0)s.hold_unpick_put_away_done=true;
          else if(tray.valid&&tray.count!=0){s.hold_unpick.clear();s.bridge.status="Hold adjustment interrupted";}
        } else if(tray.valid&&carry.valid&&tray.count==s.hold_unpick_index) {
          bool processed=true;for(const auto& slot:tray.slots)if(slot.address&&!slot.flag8)processed=false;
          if(s.hold_unpick_index>=s.hold_unpick.size()){s.hold_unpick.clear();s.hold_unpick_put_away_done=false;}
          else if(processed&&(tray.count==0||carry.active())) {
            if(s.bridge.request_hold(s.hold_unpick[s.hold_unpick_index],s.data)){++s.hold_unpick_index;s.hold_next={};s.next={};}
          }
        } else if(tray.valid&&tray.count>s.hold_unpick_index){s.hold_unpick.clear();s.hold_unpick_put_away_done=false;s.bridge.status="Hold adjustment changed";}
      }
    }
    // Steady status text through every step of the sequence instead of letting each individual
    // native round trip's own transient status ("Putting away" / "Preparing to hold" / "Items
    // returned" / "Holding") show through and flicker; the manifest binds the counter to this
    // flag first, falling back to the raw per-request status only when no sequence is running.
    if(was_holding_pending&&!s.bridge.pending_hold){s.next={};s.hold_next={};}
    // 2026-10-18: Recipes mirrors the same native tray in its footer preview, so it reads it too.
    const bool hold_scope=page&&(std::strcmp(page,"inventory")==0||std::strcmp(page,"recipes")==0)&&s.bridge.healthy&&s.bridge.hold_supported&&
      !s.bridge.epoch_changed&&s.context==botw::UiContext::World&&s.data.valid;
    if(hold_scope){
      BOTW_PERF_TIMER(HoldScope);
      if(now>=s.hold_next){
        auto tray=botw::held_tray(*h);auto carry=botw::held_carry(*h);
        if(tray.valid&&carry.valid){s.held=std::move(tray);s.carry=carry;s.hold_valid_at=now;}
        else if(now-s.hold_valid_at>std::chrono::milliseconds(300)){s.held=std::move(tray);s.carry=carry;}
        s.hold_next=now+std::chrono::milliseconds(100);
      }
      // Tokens identify grid items only (row, quantity, category, epoch); tray/carry readiness is
      // checked again by every request, so a torn tray read must not retire the tokens.
      if(s.held.valid&&s.carry.valid&&!s.bridge.pending)s.hold_targets.observe(s.data,s.inventory.category,s.bridge.epoch,now);
      else if(s.bridge.pending||now-s.hold_valid_at>std::chrono::milliseconds(300))s.hold_targets.clear();
    }else{s.held={};s.carry={};s.hold_targets.clear();s.hold_next={};}
    if(page&&std::strcmp(page,"recipes")==0) {
      // 2026-10-18: "Hold ingredients" only starts from empty hands (recipes_hold refuses
      // otherwise); while something is held the bar shows a blocked state instead.
      const bool held_any=s.held.valid&&s.held.count>0;
      const bool plan=!s.recipes.selected_hold_actors(s.data).empty();
      h->publish_i64(h->userdata,"botw.recipes.hold.available",plan&&!held_any);
      h->publish_i64(h->userdata,"botw.recipes.hold.blocked",plan&&held_any);
    } else h->publish_i64(h->userdata,"botw.recipes.hold.blocked",0);

    if (s.bridge.epoch_changed) {s.quick={};s.gear={};s.runes={};s.recent={};s.swap_promote={};s.swap_demote={};s.secondary_picker=false;s.secondary_focus={};
      // Lane C finding 1 [HIGH]: Item 15's cache-through-Transition
      // behavior (field<->shrine) is deliberately NOT cleared by the plain context-exit reset
      // elsewhere (search "Item 15") so a shrine interior keeps showing the overworld markers it
      // had just before entering. That reset cannot tell "brief Transition, same save" apart from
      // "a different save/session was loaded while off the main field" (quit to title, load
      // another save, then re-enter World while this cache is still holding the PREVIOUS save's
      // landmarks/places/names/stamps). epoch_changed is exactly that signal -- every other
      // per-session cache above already uses it. Force a fresh read immediately instead of
      // carrying over another save's markers.
      s.landmarks={};s.places={};s.names={};s.saved_icons={};s.area={};s.regions={};s.moving={};
      s.koroks={};s.death={};s.specials={};s.labels=botw::map_labels(s.places,s.landmarks,s.moving);
      s.caption={};s.map={};s.pins={};s.map_next={};s.map_retry_next={};
    }
    // Owner 2026-10-18: secondary-weapon auto-equip on break is CANCELLED (ITER5-BRIEF.md).
    // 2026-10-19 refactor/dead-code pass: the disabled watcher (botw::SecondaryBreak /
    // botw_secondary_break.h) and its header+test are removed -- observe() was never called from
    // this dispatch, only SecondaryBreak::clear() (a no-op *this={} reset) was reachable, and the
    // attached-equipment copy that only existed to feed observe() is gone too. Manual secondary
    // swap (teardrop flick / picker) and the "None" option are unaffected and unchanged.
    (void)inventory_sampled;
    if(s.quest_deferred) {
      const bool active=s.bridge.quest_scope() && s.bridge.healthy && !s.bridge.epoch_changed && s.quest_epoch==s.bridge.epoch && botw::context(*h)==botw::UiContext::World && now-s.quest_action_at<std::chrono::milliseconds(250);
      if(!active){s.quest_deferred=false;s.bridge.status="Quest selection cancelled";}
      else if(s.bridge.request_quest(s.quest_target,s.quest_display)){s.quest_deferred=false;s.quest_next={};}
      else if(!s.bridge.quest_refusal || (s.bridge.quest_refusal&~384u)){s.quest_deferred=false;s.bridge.status="Quest selection unavailable";}
    }
    if(s.recent_deferred) {
      const bool active=s.touch_kind==botw::QuickKind::None && s.bridge.scope() && s.bridge.healthy && !s.bridge.epoch_changed && s.recent_epoch==s.bridge.epoch && botw::context(*h)==botw::UiContext::World && observed.valid && observed.kind==botw::QuickKind::None && now-s.recent_action_at<std::chrono::milliseconds(250);
      if(!active){s.recent_deferred=false;s.swap_promote={};s.swap_demote={};s.bridge.status="Quick swap cancelled";}
      else if(s.bridge.request_inventory(s.recent_target,s.recent_display,s.recent_remove,true))s.recent_deferred=false;
      else if(!s.bridge.equip_refusal || (s.bridge.equip_refusal&~384u)){s.recent_deferred=false;s.swap_promote={};s.swap_demote={};s.bridge.status="Quick swap unavailable";}
    }
    if((s.swap_promote.address || s.swap_demote.address) && now-s.swap_at>std::chrono::seconds(6)){s.swap_promote={};s.swap_demote={};}
    { BOTW_PERF_TIMER(ReadyRecentGearMod);
    for(unsigned i=0;i<2;++i) {
      // Weapon (i==0) teardrop: "None" always wins over whatever target() would offer, so the
      // native empty-slot icon shows and stays showing until the player picks a secondary again.
      const auto* item=(i==0 && s.secondary_none) ? nullptr :
        (s.context==botw::UiContext::World && s.recent_actor_valid[i]?s.recent[i].target(s.data,int(i)):nullptr);
      botw::Key key(i?"botw.ready.recent.bow":"botw.ready.recent.weapon");
      h->publish_i64(h->userdata,key(".valid"),item!=nullptr);
      if(i==0) {
        // Iteration 12 item 3: the flick also works with an empty secondary (primary -> secondary
        // through the native Remove), so the swap hint follows what the flick would do, not None.
        const auto* primary=s.context==botw::UiContext::World && s.recent_actor_valid[0] ? s.recent[0].find(s.data,s.recent[0].current,0) : nullptr;
        h->publish_i64(h->userdata,key(".swap"),item!=nullptr || (primary && primary->equipped && s.bridge.remove_supported));
      }
      h->publish_text(h->userdata,key.c_str(),item?item->actor.c_str():"");
      const auto modifier=item?botw::gear_modifier(*h,item->address):botw::GearModifier{};
      h->publish_i64(h->userdata,key(".durability_pct"),item?botw::durability_percent(*item,modifier.flags,modifier.value):-1);
      h->publish_i64(h->userdata,key(".stat"),item?botw::quick_detail(*item,modifier,&s.counters.master_sword()).stat:-1);
    }
    }
    const bool map_scope=s.bridge.map_scope() && botw::context(*h)==botw::UiContext::World && s.bridge.map_supported && s.bridge.healthy;
    if(!map_scope || s.bridge.epoch_changed) {s.map_palette=-1;s.map_selected=-1;s.map_tap_deferred=false;s.map_marker_deferred=false;s.map_delete_deferred=false;}
    if(s.map_tap_deferred || s.map_marker_deferred || s.map_delete_deferred) {
      BOTW_PERF_TIMER(MapEditState);
      const bool fresh=map_scope && s.map_action_epoch==s.bridge.epoch && now-s.map_action_at<std::chrono::milliseconds(250);
      if(s.map_tap_deferred) {
        s.map_tap_deferred=false;
        if(fresh && h->get_f64 && h->get_i64(h->userdata,"@map_tap_seq",-1)==s.map_tap_before+1) {
          const double x=h->get_f64(h->userdata,"@map_tap_x",NAN),y=h->get_f64(h->userdata,"@map_tap_y",NAN);
          const bool queued=s.bridge.request_map(unsigned(s.map_deferred_palette),float(x),float(-y));
          if(!queued && s.bridge.map_refusal && !(s.bridge.map_refusal&~384u))s.map_tap_deferred=true;
          s.map_next={};s.map_selected=-1;
        }
      }
      if(s.map_delete_deferred) {
        s.map_delete_deferred=false;
        const auto native=fresh ? botw::map_edit_snapshot(*h) : botw::MapEditSnapshot{};
        if(native.valid && s.map_selected>=0 && native.markers[size_t(s.map_selected)]==s.map_selected_marker) {
          const bool queued=s.bridge.request_map(0,0,0,s.map_selected);
          if(!queued && s.bridge.map_refusal && !(s.bridge.map_refusal&~384u))s.map_delete_deferred=true;
          else s.map_selected=-1;
          s.map_next={};
        } else s.map_selected=-1;
      }
      if(s.map_marker_deferred) {
        s.map_marker_deferred=false;s.map_selected=-1;
        if(fresh) {
          const auto native=botw::map_edit_snapshot(*h);
          if(native.valid)for(unsigned i=0;i<105;++i)if(native.markers[i].active && native.markers[i].kind==s.map_clicked_icon.kind && native.markers[i].xyz==s.map_clicked_icon.xyz) {
            if(s.map_selected>=0){s.map_selected=-2;break;}s.map_selected=int(i);
          }
          if(s.map_selected>=0)s.map_selected_marker=native.markers[size_t(s.map_selected)];
        }
      }
    }
    s.map_last_tap_seq=h->get_i64 ? h->get_i64(h->userdata,"@map_tap_seq",0) : 0;
    h->publish_text(h->userdata,"botw.map.edit.hit","file:assets/map-transparent.png");
    // Perf, CORRECTED (coordinator catch): botw.map.edit.record.{i}.* is the
    // "editable_markers" dynamic_marker group, and ReadyHyrule has its own editable_markers entry
    // (manifest-checked, "map.areas.ReadyHyrule.dynamic_markers") -- the Ready circle shows
    // player-placed markers too, so gating this loop to page=="map" (the first version of this fix)
    // silently blanked them on Ready. Gated to needs_full_map_markers() instead -- true on Ready
    // and Map (and anywhere unexpected), false only on the pages confirmed to have no "map" widget
    // at all. botw.map.edit.armed.{i} is different: a direct page-widget text search (not an area/
    // dynamic_marker binding) finds it ONLY on the "map" page's own toolbar buttons, so that one
    // correctly stays page=="map"-only.
    const bool map_markers_full=needs_full_map_markers(page);
    const bool map_page=page && std::strcmp(page,"map")==0;
    // Perf round 3 (PERF2-BRIEF.md item 6): Map-page viewport culling.
    // Build (or reuse) a world-space AABB (botw::MapViewCull) from the map widget's own last-
    // drawn view for area "FullHyrule" (same host floats botw::map_page_overlay already reads for
    // the player/quest overlay). Perf round 3b hysteresis: only recompute s.map_cull when the
    // CURRENT (unmargined) drawn view is not fully contained inside the previously-stored cull
    // rect -- i.e. only when panning/zooming has moved far enough that the old margin would no
    // longer cover what's on screen. Between those recomputes the exact same rect (and therefore
    // the exact same published visible/hidden set) is reused tick after tick, so a steady drag no
    // longer toggles borderline entries' keys every tick. When it does recompute, the new rect is
    // centred on the current view with the usual 50% margin, so it already covers the view that
    // triggered the recompute -- correct and gapless on both pan and zoom (in or out) without a
    // separate zoomed-out special case. If the view is unknown/invalid, the stored cull is
    // invalidated too, so the next valid tick starts fresh and (like any invalid view) every
    // WithinMapView() call fails open in the meantime.
    if(map_page) {
      const auto view_rect=botw::map_view_rect(*h,"FullHyrule");
      if(!view_rect.valid) {
        s.map_cull=botw::MapViewCull{};
      } else {
        const float half_w=float(view_rect.w/2.0/view_rect.ppw);
        const float half_h=float(view_rect.h/2.0/view_rect.ppw);
        const float minx=float(view_rect.cx)-half_w,maxx=float(view_rect.cx)+half_w;
        const float miny=float(view_rect.cy)-half_h,maxy=float(view_rect.cy)+half_h;
        const bool contained=s.map_cull.valid&&s.map_cull.minx<=minx&&s.map_cull.maxx>=maxx&&
          s.map_cull.miny<=miny&&s.map_cull.maxy>=maxy;
        if(!contained) {
          const float margin_w=half_w*1.5f,margin_h=half_h*1.5f;
          s.map_cull.minx=float(view_rect.cx)-margin_w;s.map_cull.maxx=float(view_rect.cx)+margin_w;
          s.map_cull.miny=float(view_rect.cy)-margin_h;s.map_cull.maxy=float(view_rect.cy)+margin_h;
          s.map_cull.valid=true;
        }
      }
    } else {
      s.map_cull=botw::MapViewCull{};
    }
    const botw::MapViewCull* map_vcull=(map_page&&s.map_cull.valid)?&s.map_cull:nullptr;
    // Perf round 2: the full 200-slot editable_markers loop is now
    // Map-only -- Ready gets the same data via botw::publish_ready_edit's bounded, nearest-first
    // botw.map.ready.edit.{i}.* list (published below, in the main MapPublishChain block), which
    // the manifest's ReadyHyrule editable_markers group now binds instead of the full list.
    if(map_page) {
      // Perf round 4: skip the whole slot (no kind/x/y writes) when
      // inactive/culled, same discipline as round 3's koroks/places/landmarks fix -- editable_markers
      // is a plain dynamic_markers group (hide_when_kind=-1), so a missing kind/x/y already
      // `continue`s the runtime's own marker loop.
      for(unsigned i=0;i<200;++i) {
        const auto& icon=s.saved_icons.icons[i];bool active=s.saved_icons.valid && ((icon.kind>=27 && icon.kind<=35) || (icon.kind>=37 && icon.kind<=41));
        // Perf round 3: same slot-stable viewport cull as the other
        // per-entry Map lists -- index i keeps meaning "this saved-icon slot" either way.
        if(active && map_vcull && !botw::WithinMapView(icon.xyz[0],icon.xyz[2],*map_vcull))active=false;
        if(!active)continue;
        botw::Key key("botw.map.edit.record.",i);
        h->publish_i64(h->userdata,key(".kind"),icon.kind);
        if(h->publish_f64){h->publish_f64(h->userdata,key(".x"),icon.xyz[0]);h->publish_f64(h->userdata,key(".y"),-icon.xyz[2]);}
      }
    }
    h->publish_i64(h->userdata,"botw.map.edit.available",map_scope && !s.bridge.pending);
    h->publish_i64(h->userdata,"botw.map.edit.selected",map_scope && s.map_selected>=0 && !s.bridge.pending);
    h->publish_i64(h->userdata,"botw.map.edit.refusal",s.bridge.map_refusal);
    h->publish_i64(h->userdata,"botw.map.edit.tool",s.map_palette);
    if(map_page)for(int i=0;i<14;++i)h->publish_i64(h->userdata,botw::Key("botw.map.edit.armed.",unsigned(i)).c_str(),map_scope && s.map_palette==i);
    std::string map_hint="Pinch to zoom · Drag to pan";
    if(s.map_selected>=0)map_hint=std::string("Selected ")+map_marker_name(s.map_selected_marker.kind);
    else if(s.map_palette>=0) {
      const bool occupied=s.map_palette>=6 && s.map_palette<=10 && s.pins.valid && s.pins.active[size_t(s.map_palette-6)];
      map_hint=std::string(occupied ? "Move " : "Place ")+map_tools[s.map_palette]+" · Tap map";
    }
    h->publish_text(h->userdata,"botw.map.edit.hint",map_hint.c_str());

    h->publish_i64(h->userdata,"botw.bridge.available",s.bridge.healthy);
    h->publish_i64(h->userdata,"botw.bridge.heartbeat",s.bridge.heartbeat);
    h->publish_i64(h->userdata,"botw.bridge.epoch",s.bridge.epoch);
    h->publish_text(h->userdata,"botw.selection.status",s.bridge.status);
    // The shared bridge also handles quests; do not carry their feedback into the pouch.
    const bool quest_status=std::strncmp(s.bridge.status,"Quest ",6)==0 || std::strcmp(s.bridge.status,"Awaiting native quest update")==0;
    // 2026-10-18: the Inventory status line shows a result for 3 s (or while a request is pending)
    // and clears on a category change, so an old "Equipped" / "Removed" never sits under another tab.
    if(s.status_seen!=s.bridge.status){s.status_seen=s.bridge.status;s.status_at=now;}
    const bool status_fresh=s.bridge.pending||now-s.status_at<std::chrono::seconds(3);
    s.inventory_status_shown=!quest_status&&status_fresh&&s.bridge.status&&*s.bridge.status;
    h->publish_text(h->userdata,"botw.inventory.status",s.inventory_status_shown ? s.bridge.status : "");
    h->publish_i64(h->userdata,"botw.selection.pending",s.bridge.pending!=0 || s.quest_deferred);
    bool equip_available=false,remove_available=false,consume_available=false;
    { BOTW_PERF_TIMER(InventoryAvailability);
    if(s.bridge.healthy && s.bridge.inventory_supported && !s.bridge.pending &&
       s.bridge.inventory_scope() && s.context==botw::UiContext::World && s.data.valid &&
       (!s.bridge.hold_supported || (s.held.valid&&!s.held.count)))
      for(const auto& i:s.data.items)if(i.address==s.inventory.selected) {
        consume_available=s.bridge.consume_supported && botw::consumable(*h,i).valid;
        remove_available=s.bridge.remove_supported && i.equipped && (i.type==0 || i.type==1 || i.type==3 || (s.bridge.remove_armor_supported && i.type>=4 && i.type<=6));
        equip_available=!i.equipped && ((i.value>0 && (i.type==0 || i.type==1 || i.type==3 || (s.bridge.arrows_supported && i.type==2))) ||
          (((s.bridge.lower_armor_supported && i.type==6) || (s.bridge.armor_supported && (i.type==4 || i.type==5))) && i.value>=-1 && i.value<=15));}
    }
    h->publish_i64(h->userdata,"botw.inventory.consume.available",consume_available);
    h->publish_i64(h->userdata,"botw.inventory.remove.available",remove_available);
    h->publish_i64(h->userdata,"botw.inventory.equip.available",equip_available);
    { BOTW_PERF_TIMER(QuickOverlay);
    const bool touch_active=s.touch_kind!=botw::QuickKind::None;
    if(touch_active && (!s.bridge.scope() || !s.bridge.healthy || s.bridge.epoch_changed || s.touch_epoch!=s.bridge.epoch || botw::context(*h)!=botw::UiContext::World || !observed.valid || observed.kind!=botw::QuickKind::None || (!s.data.valid && s.touch_kind!=botw::QuickKind::Rune))) {
      s.touch_kind=botw::QuickKind::None;s.touch_gear={};s.touch_select_deferred=false;s.quick={};
    }
    s.quick.update(!touch_active && page && std::strcmp(page,"ready")==0, observed);
    if(s.touch_kind==botw::QuickKind::Rune) {
      s.touch_runes=botw::world_runes(*h);
      if(s.touch_runes.valid){s.runes=s.touch_runes.list;s.quick.shown=botw::QuickKind::Rune;
        int focus=-1;for(int i=0;i<s.runes.count;++i)if(s.runes.ids[size_t(i)]==s.touch_rune_focus)focus=i;
        if(focus<0){focus=s.runes.selected>=0?s.runes.selected:0;s.touch_rune_focus=s.runes.count?s.runes.ids[size_t(focus)]:-1;}
        s.runes.highlighted=focus;}
      else {s.touch_kind=botw::QuickKind::None;s.touch_select_deferred=false;s.quick={};s.bridge.status="Runes unavailable";}
    } else if(s.touch_kind!=botw::QuickKind::None) {
      s.touch_gear={};s.touch_gear.valid=true;s.touch_gear.kind=s.touch_kind;
      const int type=s.touch_kind==botw::QuickKind::Weapon ? 0 : s.touch_kind==botw::QuickKind::Bow ? 1 : s.touch_kind==botw::QuickKind::Shield ? 3 : s.touch_kind==botw::QuickKind::Arrow ? 2 : -1;
      for(const auto& item:s.data.items)if(item.type==type && item.value>0)s.touch_gear.items.push_back(item);
      // Arrow stacks retain native pouch order; equipment ranks by condition.
      if(type!=2) {
        // Read each condition once, then sort on the cached values (stable, deterministic).
        std::vector<std::pair<int,botw::Item>> ranked;ranked.reserve(s.touch_gear.items.size());
        for(auto& item:s.touch_gear.items){const auto m=botw::gear_modifier(*h,item.address);ranked.emplace_back(botw::durability_percent(item,m.flags,m.value),std::move(item));}
        std::stable_sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.first!=b.first?a.first>b.first:a.second.address<b.second.address;});
        s.touch_gear.items.clear();for(auto& r:ranked)s.touch_gear.items.push_back(std::move(r.second));
      }
      if(type<0 || s.touch_gear.items.size()>(type==2 ? 6u : 20u))s.touch_gear={};
      // Iteration 8 item 1: Weapon/Bow/Shield touch pickers (plain and secondary) get a synthetic
      // "None" entry at items[0] -- empty actor/address so every existing actor[0]!=0 "valid" gate
      // (cell art, stat, durability, tap target) already hides it correctly; the UI adds one
      // dedicated empty-slot icon at the fixed cell-0 position instead (tools/i8-pickers.py).
      // Inserted AFTER the size-cap check above (the cap is native's own 6/20 item cap, not ours)
      // and AFTER ranking (None has no durability to rank by; it must stay pinned at index 0).
      if(s.touch_gear.valid && type!=2) {
        botw::Item none{};none.type=type;
        s.touch_gear.items.insert(s.touch_gear.items.begin(),none);
        s.touch_gear.has_none=true;
      }
      if(s.secondary_picker && s.touch_gear.valid)for(size_t i=0;i<s.touch_gear.items.size();++i) {
        const auto& item=s.touch_gear.items[i];
        if(item.address==s.secondary_focus.address && item.actor==s.secondary_focus.actor)s.touch_gear.highlighted=int(i);
      }
      if(!s.secondary_picker && s.touch_gear.valid) {
        int focus=-1;for(size_t i=0;i<s.touch_gear.items.size();++i) {
          const auto& item=s.touch_gear.items[i];if(item.address==s.touch_focus.address && item.actor==s.touch_focus.actor)focus=int(i);
        }
        if(focus<0 && !s.touch_gear.items.empty()) {
          focus=0;for(size_t i=0;i<s.touch_gear.items.size();++i)if(s.touch_gear.items[i].equipped){focus=int(i);break;}
          const auto& item=s.touch_gear.items[size_t(focus)];s.touch_focus={item.address,item.actor};
        }
        s.touch_gear.highlighted=focus;
      }
      s.quick.shown=s.touch_gear.valid ? s.touch_kind : botw::QuickKind::None;
    }
    if(s.touch_select_deferred) {
      if(s.touch_kind==botw::QuickKind::None || now-s.touch_action_at>=std::chrono::milliseconds(250)) {
        s.touch_select_deferred=false;s.bridge.status=s.touch_kind==botw::QuickKind::Rune?"Rune request expired":"Equip request expired";
      } else {
        const bool rune=s.touch_kind==botw::QuickKind::Rune;
        const bool queued=rune ? s.bridge.request_world_rune(s.touch_rune_id,s.touch_rune_display) : s.bridge.request_inventory(s.touch_target,s.touch_display,s.touch_remove,true);
        const uint32_t refusal=rune ? s.bridge.rune_refusal : s.bridge.equip_refusal;
        if(queued) {s.touch_select_deferred=false;s.touch_kind=botw::QuickKind::None;s.touch_gear={};s.quick={};}
        else if(!refusal || (refusal&~384u)) {s.touch_select_deferred=false;s.bridge.status=rune ? "Rune unavailable" : (s.touch_remove ? "Remove unavailable" : "Equip unavailable");}
      }
    }
    if(s.touch_kind==botw::QuickKind::None){s.secondary_picker=false;s.secondary_focus={};}
    h->publish_i64(h->userdata,"botw.quick.secondary",s.secondary_picker);
    h->publish_i64(h->userdata,"botw.quick.secondary.none",s.secondary_none);
    // Picker-scoped: true only while the secondary picker is open AND None is current, so a
    // UI widget can bind to it without the persisted none-flag leaking a stray visual once the
    // picker is closed (the companion-only flag above deliberately survives closing it).
    h->publish_i64(h->userdata,"botw.quick.secondary.none_active",s.secondary_picker && s.secondary_none);
    const auto* secondary_candidate=s.secondary_picker?s.recent[0].find(s.data,s.secondary_focus,0):nullptr;
    // FIX ROUND 1 (2026-10-19, item C): s.recent[0].find() looks up a real pouch item by
    // address/actor -- the synthetic None entry (address 0, empty actor, items[0] whenever
    // touch_gear.has_none) can never match any real item, so secondary_candidate was always
    // nullptr while None was focused and the "Set secondary"/confirm button (secondary_confirm_hit,
    // gated on this flag, update-ready-inputs.py) was permanently gated out of existence for it --
    // the None cell was selectable (the grid tap worked, secondary_focus updated) but had no
    // reachable confirm control at all. secondary_confirm already redirects a None focus to the
    // existing secondary_none action (no native write, see the "secondary_confirm" handler above),
    // so this only needs to ALSO arm the button for that case; avoid arming it when secondary is
    // already cleared (nothing to confirm, matches quick_recent's own no-op guard for argument==0
    // when s.secondary_none).
    const bool secondary_none_focus=s.secondary_picker && s.secondary_focus.address==0 && s.secondary_focus.actor.empty();
    h->publish_i64(h->userdata,"botw.quick.secondary.confirm_available",
      !s.bridge.pending && ((secondary_candidate && !secondary_candidate->equipped) || (secondary_none_focus && !s.secondary_none)));
    // Limited to witnessed positive-to-zero exhaustion; a fast destruction can be missed.
    h->publish_i64(h->userdata,"botw.quick.touch",s.touch_kind!=botw::QuickKind::None);
    h->publish_i64(h->userdata,"botw.quick.held",s.touch_kind==botw::QuickKind::None);
    h->publish_i64(h->userdata,"botw.quick.native_kind",int(observed.kind));
    h->publish_i64(h->userdata,"botw.quick.armed",s.quick.armed);
    h->publish_text(h->userdata,"botw.quick.page",page ? page : "unknown");
    const bool gear=s.quick.shown==botw::QuickKind::Weapon || s.quick.shown==botw::QuickKind::Bow || s.quick.shown==botw::QuickKind::Shield || s.quick.shown==botw::QuickKind::Arrow;
    // Iteration 8 owner decision 2026-10-19: the D-pad hold wheel (held_gear) also gets a None
    // cell as its first entry on Weapon/Bow/Shield, same as the touch picker. Captured BEFORE
    // this tick overwrites s.gear/s.held_picker_open, so the release-detection block below (after
    // held_picker_open is recomputed) can see what was on screen the instant the native wheel
    // closed -- the physical D-pad release itself is not a separate observable event in this
    // codebase, only the native wheel's own open->closed transition is (see that block).
    if (gear) {
      if(s.touch_kind!=botw::QuickKind::None)s.gear=s.touch_gear;
      else {
        s.gear=botw::gear(*h,s.data);
        // A native category change between samples must not relabel another grid.
        if(s.gear.valid && s.gear.kind!=s.quick.shown)s.gear={};
        // Native cells include -1 (empty hand) before the real pouch indices.
        // Insert a display tile; actions reconstruct the native snapshot before
        // requesting the native cursor setter and scroll callback.
        if(s.gear.valid && s.gear.kind!=botw::QuickKind::Arrow) {
          botw::Item none{};none.type=s.gear.kind==botw::QuickKind::Weapon?0:s.gear.kind==botw::QuickKind::Bow?1:3;
          s.gear.items.insert(s.gear.items.begin(),none);
          s.gear.has_none=true;
          ++s.gear.highlighted;
          ++s.gear.initial;
        }
      }
    } else s.gear={};
    h->publish_i64(h->userdata, "botw.quick.weapon", s.gear.valid && s.quick.shown==botw::QuickKind::Weapon);
    h->publish_i64(h->userdata,"botw.quick.gear",s.gear.valid && gear);
    h->publish_text(h->userdata,"botw.quick.title",s.secondary_picker ? "Secondary Weapons" : s.quick.shown==botw::QuickKind::Rune ? "Runes" : s.quick.shown==botw::QuickKind::Arrow ? "Arrows" : s.quick.shown==botw::QuickKind::Bow ? "Bows" : s.quick.shown==botw::QuickKind::Shield ? "Shields" : "Weapons");
    if (s.quick.shown==botw::QuickKind::Rune) {
      if (s.touch_kind!=botw::QuickKind::Rune) {
        s.runes=botw::runes(*h);
      }
    } else s.runes={};
    h->publish_i64(h->userdata,"botw.quick.rune",s.runes.valid && s.quick.shown==botw::QuickKind::Rune);
    const bool held=s.touch_kind==botw::QuickKind::None;
    const bool valid_gear=s.gear.valid && gear;
    const bool valid_rune=s.runes.valid && s.quick.shown==botw::QuickKind::Rune;
    h->publish_i64(h->userdata,"botw.quick.touch_gear",!held && valid_gear);
    h->publish_i64(h->userdata,"botw.quick.touch_rune",!held && valid_rune);
    h->publish_i64(h->userdata,"botw.quick.held_gear",held && valid_gear);
    h->publish_i64(h->userdata,"botw.quick.held_rune",held && valid_rune);
    s.held_picker_open=held&&(valid_gear||valid_rune);
    // Empty hand is native highlighted=-1, shifted to display cell zero above.
    s.held_focus_none=false;
    const int highlighted=valid_gear?s.gear.highlighted:valid_rune?s.runes.highlighted:-1;
    h->publish_i64(h->userdata,"botw.quick.highlighted",highlighted);
    const int quick_count=valid_gear?int(s.gear.items.size()):valid_rune?s.runes.count:0;
    const bool has_detail=highlighted>=0&&highlighted<quick_count;
    h->publish_i64(h->userdata,"botw.quick.description_gear",has_detail&&valid_gear&&s.quick.shown!=botw::QuickKind::Arrow);
    h->publish_i64(h->userdata,"botw.quick.description_arrow",has_detail&&valid_gear&&s.quick.shown==botw::QuickKind::Arrow);
    h->publish_i64(h->userdata,"botw.quick.description_rune",has_detail&&valid_rune);
    h->publish_i64(h->userdata,"botw.quick.confirm_visible",!held&&!s.secondary_picker&&has_detail);
    h->publish_i64(h->userdata,"botw.quick.open",valid_gear||valid_rune);
    h->publish_i64(h->userdata,"botw.quick.count",quick_count);
    // 2026-10-18 drag shortcuts: every picker tile drags while the 3 x 3 grid cannot scroll.
    for(const auto& [key,open]:{std::pair<const char*,bool>{"gear",!held&&valid_gear},std::pair<const char*,bool>{"rune",!held&&valid_rune}}) {
      const std::string base=std::string("botw.quick.drag.")+key;
      h->publish_i64(h->userdata,(base+"_all").c_str(),open&&quick_count<=9);
      h->publish_i64(h->userdata,(base+"_sel").c_str(),open&&quick_count>9);
    }
    const bool focus_available=!held&&!s.secondary_picker&&!s.bridge.pending&&!s.touch_select_deferred&&s.touch_epoch==s.bridge.epoch&&highlighted>=0&&highlighted<quick_count;
    h->publish_i64(h->userdata,"botw.quick.confirm_available",focus_available);

    h->publish_i64(h->userdata,"botw.quick.empty",(gear && s.gear.valid && s.gear.items.empty()) || (s.quick.shown==botw::QuickKind::Rune && s.runes.valid && !s.runes.count));
    // Iteration 8 item 1: has_none items[0] shifts the native 6/20-item cap by one slot.
    h->publish_i64(h->userdata,"botw.quick.has_none",s.gear.valid && gear && s.gear.has_none);
    h->publish_text(h->userdata,"botw.quick.has_none_icon",s.gear.valid && gear && s.gear.has_none ?
      (s.quick.shown==botw::QuickKind::Weapon ? "empty_weapon" : s.quick.shown==botw::QuickKind::Bow ? "empty_bow" : "empty_shield") : "");
    // Iteration 8 item 1 (coordinator follow-up): the None cell needs its own focused-ring gate
    // (the existing "selected" gold-ring overlay is per-item and gated on a non-empty actor name,
    // which None never has by construction) and the Equip/Back button row needs a 3-way state
    // (hidden / "Equip" / "Unequip") instead of the single confirm_visible boolean, since
    // confirming None while something is equipped performs an unequip, not an equip.
    const bool none_selected=s.gear.valid && gear && s.gear.has_none && highlighted==0;
    h->publish_i64(h->userdata,"botw.quick.none_selected",none_selected);
    h->publish_i64(h->userdata,"botw.quick.confirm_label",
      (!held&&!s.secondary_picker&&has_detail) ? (none_selected?2:1) : 0);
    // The Equip/Unequip/Set-secondary BOX (fill/hl/strip chrome) must show for all three label
    // states, including "Set secondary" (its own, pre-existing gate, secondary.confirm_available
    // -- not reproduced here) -- confirm_label alone always reads 0 during the secondary picker.
    h->publish_i64(h->userdata,"botw.quick.confirm_box",
      (!held&&has_detail&&!none_selected) || (!held&&has_detail&&!s.secondary_picker&&none_selected));
    // Iteration 8 item 3: the 7-column gear grid's window height is dynamic by row count (1 row
    // -> no empty rows; max 3, then scroll) and a single row of fewer than 7 items is centred
    // horizontally. botw.quick.rows gates 3 complete manifest-side layout variants (panel/edge/
    // detail/description/buttons -- a scroll clip rect is a fixed box, per render-pages.py, so a
    // y_bind shift alone would move the drawn content but not its clip and was rejected); the
    // x_bind center offset is safe because it only moves cells within the grid's existing,
    // unchanged scroll clip span. See tools/i8-pickers.py. No runtime/widget schema change needed.
    // Iteration 10 item 10 (owner feedback 2026-10-20): the gear grid's column cap drops 7 -> 6
    // (then scroll past 4 rows -- 6x4=24 slots, still >= the 21-slot has_none+20-item hard cap,
    // so no real inventory ever actually needs to scroll; the manifest's grid scroll clip is
    // simply widened generously, see tools/i10-pickers.py). Rune keeps its own, unaffected 7-col/
    // 3-row geometry (item 10 is "Equipment pop-ups" only) -- hence two separate column constants
    // and two separate center_x arms below (previously one shared formula assumed both were 7).
    // Owner feedback 2026-10-20 (full-width grid): gear's own column pitch also grows, 152 -> 174
    // (138px cell + 14px gutter -> 156px cell + 18px gutter), so 6 columns span the grid clip's
    // full 1062px usable width edge to edge instead of leaving ~150px dead on the right. Rune's
    // pitch is untouched (stays 152, its own 7-col grid was never "packed" the same way -- 7*152
    // already filled the old 1064-wide clip almost exactly). kGearPitch mirrors
    // tools/i10-pickers.py's GEAR_PITCH exactly (kept in sync there, same hand-sync precedent as
    // kHeaderH/kRowH below).
    constexpr int kGearCols=6, kRuneCols=7, kGearPitch=174, kRunePitch=152;
    const int gear_rows=!gear ? 0 : std::clamp((quick_count+kGearCols-1)/kGearCols,1,3);
    h->publish_i64(h->userdata,"botw.quick.rows",gear_rows);
    int center_x=0;
    if(gear && quick_count>0 && quick_count<kGearCols) center_x=(kGearCols-quick_count)*kGearPitch/2;
    else if(s.quick.shown==botw::QuickKind::Rune && quick_count>0 && quick_count<kRuneCols) center_x=(kRuneCols-quick_count)*kRunePitch/2;
    h->publish_i64(h->userdata,"botw.quick.grid.center_x",center_x);
    // Iteration 10 item 9 (owner feedback 2026-10-20): the gear pop-up window is now vertically
    // centred (equal top/bottom canvas margin) for every row-count variant instead of a fixed
    // PANEL_TOP=72 -- the description block this used to sit above was removed (item 9's other
    // half), so the window's content band shrank and a fixed top no longer centres it. Rather
    // than re-deriving 8 (family x rows) positioned clones for every single widget in the panel
    // (panel fill/edge, header, grid, button row), this publishes ONE signed vertical delta
    // (new_panel_top - the old fixed 72) that every one of those widgets' y_bind already applies
    // uniformly -- touch_gear and held_gear share the one key because they are mutually
    // exclusive on screen (same precedent as grid.center_x above). kHeaderH/kRowH mirror
    // tools/i10-pickers.py's own constants exactly (kept in sync there, see its module docstring
    // -- this is the one piece of this fix that is NOT purely derived from the manifest and must
    // be kept matching by hand); kBelowBand is the fixed chrome below the grid (Equip/Back row +
    // margin for touch, bottom margin only for held, both post item-9 description removal).
    constexpr int kHeaderH=202, kRowH=kGearPitch, kPanelTopOld=72, kCanvasH=1080;
    // Iteration 13 item 9 (owner): gear pop-ups no longer carry the item description, so the
    // band below the grid is back to the pre-description chrome (tools/i13-boxes.py mirrors it).
    constexpr int kBelowBandTouch=160, kBelowBandHeld=40;
    int panel_dy=0;
    if(gear) {
      const int below_band=held?kBelowBandHeld:kBelowBandTouch;
      const int panel_h=kHeaderH+below_band+gear_rows*kRowH;
      panel_dy=(kCanvasH-panel_h)/2-kPanelTopOld;
    }
    h->publish_i64(h->userdata,"botw.quick.gear.panel_dy",panel_dy);
    // Iteration 8 item 3: the manifest schema only has two gates per widget (a truthy need_bind
    // and a ranged hide_bind). The detail/description block already needs hide_bind for its own
    // semantic condition (selected.valid / stat_kind / modifier / quantity / durability_pct), so
    // the row-bucket selector that picks ONE of its 3 positioned copies has to live in need_bind
    // instead of a 3rd gate that doesn't exist -- hence one explicit boolean per (family, rows)
    // combination rather than reusing the plain botw.quick.touch_gear/held_gear family flags.
    // Iteration 10 item 10: extended 3 -> 4 buckets (max rows the 6-col grid can show before
    // scroll).
    for(int r=1;r<=4;++r) {
      botw::Key tk("botw.quick.touch_gear.rows",uint64_t(r));
      botw::Key hk("botw.quick.held_gear.rows",uint64_t(r));
      h->publish_i64(h->userdata,tk.c_str(),s.gear.valid && !held && gear && gear_rows==r);
      h->publish_i64(h->userdata,hk.c_str(),s.gear.valid && held && gear && gear_rows==r);
    }
    // Layout batch 1 item (c): the rune pickers (touch_rune/held_rune) never got their own
    // row-count buckets -- only gear's were ever published -- so the manifest's dynamic-height
    // machinery (tools/i8-pickers.py) always fell through to need_bind-false-everywhere for
    // rune, which left its OLD single fixed-3-row-tall layout fully intact and visible
    // regardless of actual row count (critic P-Cons, item 3 not met: the 1-row rune picker kept
    // the full-height window). Same shape as gear's own rows publish above. Rune is unaffected by
    // iteration 10 item 10 (gear-only), so it keeps its own 7-col/max-3-row divisor unchanged.
    const bool rune_shown=s.quick.shown==botw::QuickKind::Rune;
    const int rune_rows=!rune_shown ? 0 : std::clamp((quick_count+6)/7,1,3);
    h->publish_i64(h->userdata,"botw.quick.rune_rows",rune_rows);  // hide_bind for the rune panel/edge bucket variants (botw.quick.rows is gear-only)
    for(int r=1;r<=3;++r) {
      botw::Key trk("botw.quick.touch_rune.rows",uint64_t(r));
      botw::Key hrk("botw.quick.held_rune.rows",uint64_t(r));
      h->publish_i64(h->userdata,trk.c_str(),s.runes.valid && !held && rune_shown && rune_rows==r);
      h->publish_i64(h->userdata,hrk.c_str(),s.runes.valid && held && rune_shown && rune_rows==r);
    }
    for (unsigned i=0;i<21;++i) {
      const botw::Item* item=s.gear.valid && i<s.gear.items.size() ? &s.gear.items[i] : nullptr;
      const char* actor=item ? item->actor.c_str() : "";
      bool equipped=item && item->equipped;
      if (s.quick.shown==botw::QuickKind::Rune) {
        actor=s.runes.valid && i<unsigned(s.runes.count) ? botw::rune_actor(s.runes.ids[i],s.runes.bombs_plus,s.runes.stasis_plus) : "";
        equipped=s.runes.valid && i==unsigned(s.runes.selected);
      }
      // One modifier read per cell; the scrolling grid reads this directly (every slot
      // is always published -- there is no paging window any more).
      const auto modifier=item?botw::gear_modifier(*h,item->address):botw::GearModifier{};
      const auto cell_detail=item?botw::quick_detail(*item,modifier,&s.counters.master_sword()):botw::QuickDetail{};
      const int64_t name=botw::actor_key(actor);
      const int quantity=item&&item->type==2?botw::item_quantity(*item):-1;
      const int durability=item && s.quick.shown!=botw::QuickKind::Rune && modifier.valid ? botw::durability_percent(*item,modifier.flags,modifier.value) : -1;
      botw::Key prefix("botw.quick.item.",i);
      h->publish_i64(h->userdata,prefix(".valid"),actor[0]!=0);
      h->publish_text(h->userdata,prefix.c_str(),actor);
      h->publish_i64(h->userdata,prefix(".name"),name);
      h->publish_i64(h->userdata,prefix(".equipped"),equipped);
      h->publish_i64(h->userdata,prefix(".stat"),cell_detail.stat);
      h->publish_i64(h->userdata,prefix(".quantity"),quantity);
      h->publish_i64(h->userdata,prefix(".selected"),actor[0] && int(i)==highlighted);
      h->publish_i64(h->userdata,prefix(".durability_pct"),durability);
      // Iteration 8 item 14: per-cell modifier corner badge, same semantics/icon set as the
      // detail pane below (botw::modifier_icon_for -- single switch, no duplicated cases).
      // Layout batch 2 fix round 1 (2026-10-19, critic finding): modifier_icon_for() returns ""
      // for a handful of modifier bits with no known native icon in the extracted asset catalog
      // (e.g. Durability Up, bit 2 -- deliberately never guessed a texture, see that function's
      // own comment). Showing the bare VALUE with no icon reads as an unexplained second stat --
      // suppress the badge entirely (icon AND value) rather than half-show it; this is a "show
      // less" fix, not new scope, since the icon-less case was already a known gap.
      const char* cell_modifier_icon=botw::modifier_icon_for(cell_detail.modifier,cell_detail.shots);
      h->publish_text(h->userdata,prefix(".modifier_icon"),cell_modifier_icon);
      h->publish_i64(h->userdata,prefix(".modifier_value"),cell_modifier_icon[0]?int64_t(cell_detail.modifier_value):0);
      h->publish_i64(h->userdata,prefix(".modifier_yellow"),cell_detail.yellow);
    }
    const botw::Item* detail_item=valid_gear && highlighted>=0 && size_t(highlighted)<s.gear.items.size() ? &s.gear.items[size_t(highlighted)] : nullptr;
    const char* detail_actor=detail_item ? detail_item->actor.c_str() : valid_rune && highlighted>=0 && highlighted<s.runes.count ? botw::rune_actor(s.runes.ids[size_t(highlighted)],s.runes.bombs_plus,s.runes.stasis_plus) : "";
    const auto detail=detail_item ? botw::quick_detail(*detail_item,botw::gear_modifier(*h,detail_item->address),&s.counters.master_sword()) : botw::QuickDetail{};
    const char* modifier_icon=botw::modifier_icon_for(detail.modifier,detail.shots);
    // Layout batch 2 fix round 1 (2026-10-19, critic finding): modifier_icon_for() returns ""
    // for a handful of modifier bits with no known native icon (e.g. Durability Up, bit 2 --
    // deliberately never guessed a texture). Showing the bare value/name with no icon reads as
    // an unexplained extra stat -- suppress the WHOLE modifier row (icon+name+value), not just
    // the value, by zeroing the modifier bit itself when there is no icon for it; every
    // downstream hide_bind (icon/name on "modifier", value on "modifier_message") is keyed off
    // this same value, so this is a single, consistent "show less" cut, not three separate ones.
    const uint32_t detail_modifier_shown=detail.modifier;
    uint32_t modifier_message=detail_modifier_shown;
    if(modifier_message==16 && detail.shots!=5)modifier_message=0;
    if(detail.yellow && (modifier_message==1 || modifier_message==2 || modifier_message==256))modifier_message|=0x80000000u;
    h->publish_text(h->userdata,"botw.quick.selected",detail_actor);
    h->publish_i64(h->userdata,"botw.quick.selected.valid",detail_actor[0]!=0);
    h->publish_i64(h->userdata,"botw.quick.selected.name",botw::actor_key(detail_actor));
    publish_description_clean("botw.quick.selected.description_clean",botw::actor_key(detail_actor));
    h->publish_i64(h->userdata,"botw.quick.selected.quantity",detail_item && detail_item->type==2 ? botw::item_quantity(*detail_item) : -1);
    h->publish_i64(h->userdata,"botw.quick.selected.stat",detail.stat);
    h->publish_i64(h->userdata,"botw.quick.selected.stat_kind",detail.stat_kind);
    h->publish_i64(h->userdata,"botw.quick.selected.durability_pct",detail.condition);
    h->publish_i64(h->userdata,"botw.quick.selected.modifier",detail_modifier_shown);
    h->publish_i64(h->userdata,"botw.quick.selected.modifier_value",detail_modifier_shown?detail.modifier_value:0);
    h->publish_i64(h->userdata,"botw.quick.selected.modifier_yellow",detail.yellow);
    h->publish_i64(h->userdata,"botw.quick.selected.modifier_message",modifier_message);
    h->publish_text(h->userdata,"botw.quick.selected.modifier_icon",modifier_icon);
    botw::publish_detail_line(*h,"botw.quick.selected",detail.stat,detail_item&&detail_item->type==2?botw::item_quantity(*detail_item):-1,modifier_message,detail.modifier_value,false);
    botw::publish_pause_gear(*h,"botw.quick.selected",detail.stat,detail.shots,modifier_message,detail.modifier_value,botw::modifier_icon_for(detail.modifier,detail.shots),false);
    h->publish_i64(h->userdata,"botw.quick.available_count",quick_count-(valid_gear&&s.gear.has_none?1:0));
    h->publish_i64(h->userdata,"botw.quick.selected.shots",detail.shots);
    }
    h->publish_text(h->userdata, "botw.context", botw::context_text(s.context));
    BOTW_PERF_BEGIN(PublishMisc);
    botw::publish_states(*h,s.states);
    // Integration 2026-10-18 (live): holding a D-pad direction opens the native quick-select
    // wheel, which sets the UI "menu" flags too; the companion's own held picker mirrors that
    // wheel, so it must not be covered by the native-menu lock.
    if(s.held_picker_open&&s.states.valid)h->publish_i64(h->userdata,"botw.state.menu_open",0);
    botw::publish_state_latch(*h,s.state_latch);
    h->publish_i64(h->userdata,"botw.map.valid",s.map.valid);
    h->publish_i64(h->userdata,"botw.map.position.valid",s.position.valid);
    h->publish_i64(h->userdata,"botw.map.heading.valid",s.map.valid&&s.map.main_field&&s.position.valid&&s.position.heading_valid);
    if(s.position.heading_valid)h->publish_f64(h->userdata,"botw.map.heading",s.position.heading);
    h->publish_i64(h->userdata,"botw.map.minimap.valid",s.map.valid&&s.map.main_field&&s.position.valid);
    botw::publish_map_view(*h,s.view,s.map.valid&&s.map.main_field&&s.position.valid,s.position);
    h->publish_i64(h->userdata,"botw.map.revealed",s.map.valid?s.map.revealed:-1);
    if(s.position.valid) {
      // Perf round 2: quantized to a visible-resolution grid
      // (botw::quantize_display, botw_map.h) -- see that comment for why.
      h->publish_f64(h->userdata,"botw.map.x",botw::quantize_display(s.position.x));
      h->publish_f64(h->userdata,"botw.map.y",botw::quantize_display(s.position.y));
    }
    std::array<char,80> tile{};
    // Perf: level 1 (coarse) feeds both Ready's ReadyHyrule minimap and Map's
    // FullHyrule src_detail_bind, so it stays unconditional; level 3 (the Map widget's full-detail
    // "src", manifest-checked) is Map-page-only, same reasoning as the MapPublishChain gate above.
    BOTW_PERF_BEGIN(TileNames);
    const bool tile_detail_page = page && std::strcmp(page,"map")==0;
    for(unsigned i=0;i<120;++i) {
      h->publish_text(h->userdata,botw::Key("botw.map.tile.",1u)(".",i,""),botw::map_tile_name(s.map,i,1u,tile));
      if (tile_detail_page)
        h->publish_text(h->userdata,botw::Key("botw.map.tile.",3u)(".",i,""),botw::map_tile_name(s.map,i,3u,tile));
    }
    BOTW_PERF_END(TileNames);
    h->publish_i64(h->userdata,"botw.state.title",s.context==botw::UiContext::Title);
    h->publish_i64(h->userdata,"botw.state.loading",s.context==botw::UiContext::Transition);
    h->publish_i64(h->userdata,"botw.state.unknown",s.context==botw::UiContext::Unknown);
    h->publish_i64(h->userdata,"botw.ready.clock.valid",s.clock.valid);
    h->publish_i64(h->userdata,"botw.ready.clock.minutes",s.clock.valid ? s.clock.minutes : -1);
    h->publish_i64(h->userdata,"botw.ready.noise.valid",s.noise.valid);
    h->publish_i64(h->userdata,"botw.ready.noise.percent",s.noise.valid?s.noise.percent:-1);
    h->publish_i64(h->userdata,"botw.ready.stamina.valid",s.stamina.valid);
    h->publish_i64(h->userdata,"botw.ready.stamina.depleted",s.stamina.valid && s.stamina.depleted);
    h->publish_i64(h->userdata,"botw.ready.stamina.percent",s.stamina.valid ? s.stamina.percent : -1);
    char clock[16]{};
    if(s.clock.valid){
      // Critic finding 10 (iteration 3 fix round): native BOTW HUD clock reads 12-hour
      // "03:05 PM", not 24-hour "15:05". h24 0 is native 12 AM; h24 12 is native 12 PM.
      const int h24=s.clock.minutes/60,mins=s.clock.minutes%60;
      const int h12=h24%12==0?12:h24%12;
      std::snprintf(clock,sizeof(clock),"%02d:%02d %s",h12,mins,h24<12?"AM":"PM");
    }
    h->publish_text(h->userdata,"botw.ready.clock.text",clock);
    const auto journal=quest_journal(s);
    const botw::Quest* ready_quest{};
    for(const auto& q:s.quests.entries)
      if(s.ready_panes.left()<2 && q.category==(s.ready_panes.left()==0?0:2) && q.state==1 &&
         (!ready_quest || q.active))ready_quest=&q;
    const botw::Quest* quest_selected{};
    for(const auto* q:journal)if(q->address==s.quest_selected)quest_selected=q;
    if(!quest_selected&&!journal.empty())quest_selected=journal[0];
    s.quest_selected=quest_selected?quest_selected->address:0;
    h->publish_i64(h->userdata,"botw.quest.action.available",s.bridge.quest_supported && s.bridge.healthy &&
      s.bridge.quest_scope() && !s.bridge.pending && !s.quest_deferred && s.context==botw::UiContext::World &&
      quest_selected && quest_selected->state==1 && !quest_selected->active && !(quest_selected->dlc&6u));
    h->publish_text(h->userdata,"botw.quest.action.status",
      (std::strncmp(s.bridge.status,"Quest ",6)==0 || std::strncmp(s.bridge.status,"Awaiting native quest",21)==0) ? s.bridge.status : "");
    auto publish_quest=[&](botw::Key key,const botw::Quest* q) {
      h->publish_i64(h->userdata,key(".valid"),q!=nullptr);
      h->publish_i64(h->userdata,key(".name"),q?botw::actor_key(q->name):-1);
      const int64_t description_hash=q?botw::actor_key2(q->name,'_',q->message):-1;
      h->publish_i64(h->userdata,key(".description"),description_hash);
      publish_description_clean(key(".description_clean"),description_hash);
      h->publish_i64(h->userdata,key(".complete"),q&&q->state==2);
      h->publish_i64(h->userdata,key(".active"),q&&q->active);
    };
    h->publish_i64(h->userdata,"botw.quest.valid",s.quests.valid);
    h->publish_i64(h->userdata,"botw.quest.count",journal.size());
    botw::publish_map_pins(*h,s.pins,map_vcull);
    botw::publish_ready_marks(*h,s.pins,s.quest_markers,s.position,
                              s.context==botw::UiContext::World&&s.map.valid&&s.map.main_field,
                              botw::view_rotation(s.map.valid&&s.map.main_field&&s.position.valid?s.view:botw::MapView{}),now);
    // Perf round 2: full=false on Ready (it now reads stamps
    // via the compact botw.map.ready.marks.* list built below) and anywhere else the native_stamps
    // map widget cannot be bound; Map keeps the full per-entry loop.
    botw::publish_map_stamps(*h,s.saved_icons,page&&std::strcmp(page,"map")==0,map_vcull);
    // Critique item E (fix round 2): Map-page player/quest overlay, above the native stamps,
    // using the map widget's own last-drawn view (host @map_view_* floats for "FullHyrule").
    botw::publish_map_page_overlay(*h,"botw.map.overlay",
      botw::map_page_overlay(*h,"FullHyrule",s.position,s.quest_markers));
    botw::publish_map_label_tier(*h,botw::map_view_rect(*h,"FullHyrule"));
    botw::publish_map_area(*h,s.area);
    // Ready-circle caption strip: shows the marker caption (botw.ready.marker.*) in
    // place of the area name while one is active (tools/i5-labels.py repoints the
    // existing area-name widget's need_bind at this combined key, no new widget rect).
    h->publish_i64(h->userdata,"botw.ready.area.show",s.area.valid&&!s.caption.valid);
    botw::publish_sensor(*h,s.sensor);
    botw::publish_temperature_dial(*h,s.temperature_dial);
    {
      // Minimap helper cards: one at a time inside the Ready circle.
      // Card 1 = shrine compass, 2 = temperature, 3 = food; only on Ready, never while the
      // event dim or a native menu owns the screen. Iteration 14 item 11: inside a shrine the
      // food (3) and temperature (2) cards still open over the shrine panel (they are about Link,
      // not the overworld); only the overworld shrine compass (1) stays closed there.
      const bool ready_page=page&&std::strcmp(page,"ready")==0;
      const bool menu_lock=s.states.valid&&!s.states.menu_screen.empty()&&!s.held_picker_open;
      s.cards.gate(ready_page&&s.states.valid&&!s.state_latch.dim&&!menu_lock&&(!s.states.shrine||s.cards.active!=1)&&s.context==botw::UiContext::World);
      if(s.cards.active==2&&s.temp_card.shown==botw::TempDanger::None&&!s.temp_card.busy())s.cards.close(2);
      if(s.cards.active==1&&!botw::sensor_available(s.sensor))s.cards.close(1);
      s.temp_card.pump(s.bridge,s.data,s.cards.active==2,s.context==botw::UiContext::World);
      const bool cards_can_act=s.bridge.healthy&&s.bridge.inventory_supported&&s.bridge.armor_supported&&!s.bridge.pending&&s.bridge.scope()&&
        s.context==botw::UiContext::World&&s.data.valid;
      s.temp_card.publish(*h,s.cards.active==2,cards_can_act);
      s.heal.update(now);
      if(s.cards.active==3&&s.heal.close_request())s.cards.close(3);
      s.heal.publish(*h,s.bridge.pending!=0);
      s.cards.publish(*h);
    }
    s.map_ui.publish(*h,page&&std::strcmp(page,"map")==0,s.context==botw::UiContext::World&&s.bridge.healthy,s.bridge.epoch,s.position,s.landmarks,s.saved_icons,800.f,900.f,&s.specials);
    s.travel.publish(*h,page&&std::strcmp(page,"map")==0,page&&std::strcmp(page,"trackers")==0,page&&std::strcmp(page,"ready")==0,s.bridge.healthy&&s.bridge.warp_supported&&s.context==botw::UiContext::World,now);
    // The Map page layer popover owns the Korok filter.
    const auto korok_filter=s.map_ui.korok_filter();const bool korok_layer=s.map_ui.layers[botw::MapUi::Found]||s.map_ui.layers[botw::MapUi::Unfound];
    // Perf, CORRECTED after a coordinator catch on the first version of this
    // gate (which used page=="map" and silently blanked the Ready circle's own koroks/landmarks/
    // places/moving/special markers -- ReadyHyrule has its own "dynamic_markers" entries for every
    // one of those groups, found in the manifest's TOP-LEVEL "map.areas.ReadyHyrule.dynamic_markers"
    // -- not page-local bind text, which is why a plain text search of the "pages" array for these
    // keys finds nothing on any page: the runtime resolves "{i}"-templated dynamic_marker specs
    // against the active page's AREA, not its widget tree). Checked precisely, per function, what
    // each one's data actually feeds:
    //  - landmarks/places/regions/names/moving/koroks/special: native_landmarks / native_places(+
    //    _shop/_backing on Map only, same underlying call) / region_names / place_names_* /
    //    native_moving(+native_horse) / native_koroks / native_special dynamic_marker groups, each
    //    present in BOTH ReadyHyrule and FullHyrule's dynamic_markers list (FullHyrule only adds a
    //    few Map-only groups fed by other, untouched functions: native_pins/quest_destinations/
    //    native_horse/full_map_hits). These seven stay full on Ready AND Map (needs_full_map_markers
    //    below), skipped only on the page set confirmed to have no "map" widget at all.
    //  - labels (botw.map.labels.*): manifest-checked directly -- ReadyHyrule's own 524 static
    //    labels reference it 0 times; FullHyrule's 717 reference it 193 times. Map-page-only,
    //    correctly gated to page=="map" specifically (NOT needs_full_map_markers).
    //  - death (no loop) and ready_caption (Ready needs it every tick) stay fully unconditional,
    //    as before.
    // All data was already cached in `s` at no extra read cost; the saving is purely in skipping
    // the per-entry PUBLISH loop (string-keyed host map inserts) on pages that provably never draw
    // it -- confirmed the host's `tick_snapshot` is fully cleared every tick (mod_runtime.cpp,
    // `snapshot.ints.clear()` before every SampleState/RunGameModule) and `StateSnapshot::GetInt`
    // returns the caller's fallback for a missing key (mod_types.h) -- i.e. there is NO cross-tick
    // persistence to lean on; skipping a publish is only safe when NOTHING reads that key this
    // tick, never as a "value unchanged, keep the old one" optimization.
    BOTW_PERF_BEGIN(MapPublishChain);
    // map_markers_full already computed above (same page, same helper) for the editable-markers loop.
    const bool map_labels_full = page && std::strcmp(page,"map")==0;
    // Perf, coordinator-requested spatial cull: ReadyHyrule's map widget has
    // follow_window=900 (a 900x900 world-unit window centred on and rotating with the player);
    // FullHyrule (Map) has none -- it's the static full-world view, never culled. Pass the player
    // position only on Ready so the three biggest per-entry loops (koroks 900, places 256,
    // landmarks/poi 151) skip publishing a slot's expensive x/y pair (and, for koroks, its kind/
    // tint too) once WithinReadyWindow() rules it out -- same "kind=-1, x/y omitted" shape the
    // runtime already treats as hidden for a filtered/invalid entry, so nothing the circle actually
    // draws changes; slot indices are untouched (the loop still visits every index, so i stays the
    // same entry's slot whether culled or not). moving (9 entries) and special (4 entries) are too
    // small to be worth culling -- left unconditional, as they already were.
    const bool ready_page = page && std::strcmp(page,"ready")==0;
    // Perf round 2, shared contract with the manifest/redraw lane
    // (internal notes "Ready compact marker lists", item 1 -- SUPERSEDES the single-
    // merged-list version of this block, see botw_map_ready_marks.h header comment for why):
    // Ready no longer needs the full per-entry loops for koroks/places/landmarks/stamps at all --
    // it gets one bounded, nearest-first compact list PER SOURCE instead (botw::publish_ready_*,
    // below), each published under its own botw.map.ready.<source>.{i}.* prefix so every
    // ReadyHyrule manifest group keeps its own on-screen `size` (tools/perf2-ready-compact.py
    // repoints each group's binds + count to match). map_entries_full is map_markers_full (true
    // on Map and anywhere unexpected) with Ready carved out; Map's behaviour is completely
    // unchanged (still the full catalogue, no cull, no compact publish).
    const bool map_entries_full = map_markers_full && !ready_page;
    botw::publish_map_landmarks(*h,s.landmarks,map_entries_full,nullptr,map_vcull);
    botw::publish_map_places(*h,s.places,map_entries_full,nullptr,map_vcull);botw::publish_map_regions(*h,s.regions,map_markers_full);botw::publish_map_names(*h,s.names,map_markers_full,map_vcull);botw::publish_map_moving(*h,s.moving,map_entries_full||(map_markers_full&&ready_page),ready_page?&s.position:nullptr);botw::publish_map_koroks(*h,s.koroks,korok_filter,map_entries_full,nullptr,map_vcull);botw::publish_map_death(*h,s.death);botw::publish_map_special(*h,s.specials,map_entries_full);botw::publish_map_labels(*h,s.labels,map_labels_full,map_vcull);botw::publish_ready_caption(*h,s.caption);
    if(ready_page) {
      botw::publish_ready_koroks(*h,s.koroks,korok_filter,s.position);
      botw::publish_ready_places(*h,s.places,s.position);
      botw::publish_ready_poi(*h,s.landmarks,s.position);
      botw::publish_ready_stamps(*h,s.saved_icons,s.position);
      botw::publish_ready_edit(*h,s.saved_icons,s.position);
    } else {
      botw::publish_ready_koroks(*h,{},korok_filter,{});
      botw::publish_ready_places(*h,{},{});
      botw::publish_ready_poi(*h,{},{});
      botw::publish_ready_stamps(*h,{},{});
      botw::publish_ready_edit(*h,{},{});
    }
    BOTW_PERF_END(MapPublishChain);
    h->publish_i64(h->userdata,"botw.map.quest.tint",0xFFF5F506);
    h->publish_i64(h->userdata,"botw.map.quest.valid",s.quest_markers.valid);
    h->publish_i64(h->userdata,"botw.map.quest.count",s.quest_markers.valid?s.quest_markers.count:0);
    // Perf: ring_frame/halo do not depend on the loop index `i` --
    // this was recomputed and reallocated (std::to_string + string concat) up to 5x every tick,
    // unconditionally, for a value that only actually changes once every 33 ms. Hoisted out.
    const auto ring_frame=std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()/33%48;
    const std::string halo_src="composite:qr-"+std::to_string(ring_frame);  // map markers load image keys directly (no sprite_map)
    // Perf round 4: skip the whole slot (no kind/text/x/y writes) when
    // not a valid quest-destination slot, same discipline as this round's other dynamic_markers
    // fixes (quest_destinations is a plain dynamic_markers group, hide_when_kind=-1).
    for(unsigned i=0;i<5;++i) {
      const bool valid=s.quest_markers.valid && i<s.quest_markers.count;
      if(!valid)continue;
      botw::Key key("botw.map.quest.",i);
      h->publish_i64(h->userdata,key(".kind"),0);
      if(h->publish_f64) {
        h->publish_f64(h->userdata,key(".x"),s.quest_markers.positions[i][0]);
        h->publish_f64(h->userdata,key(".y"),-s.quest_markers.positions[i][2]);
      }
      h->publish_text(h->userdata,key(".core"),
        "romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#Nt_MapChallengeIcon_00^s");
      h->publish_text(h->userdata,key(".halo"),halo_src.c_str());
    }
    h->publish_i64(h->userdata,"botw.quest.category",s.quest_category);
    h->publish_i64(h->userdata,"botw.quest.selected.address",int64_t(s.quest_selected));
    publish_quest(botw::Key("botw.quest.selected"),quest_selected);
    h->publish_i64(h->userdata,"botw.ready.quest.mode",s.quest_pane);
    // Champion ownership must be known before the pane page is published: it clamps
    // the Champions page out of the normal-left cycle when nothing is owned yet
    // (owner feedback item 11), so compute/publish it first.
    const int champions_owned=botw::publish_champion_ownership(*h,s.data,botw::champion_abilities(*h));
    s.ready_panes.set_champions_available(champions_owned>0);
    h->publish_i64(h->userdata,"botw.ready.left.page",s.ready_panes.left());
    h->publish_i64(h->userdata,"botw.ready.right.page",s.heal.page.right(s.ready_panes));
    h->publish_i64(h->userdata,"botw.ready.left.quest",s.ready_panes.left()<2);
    h->publish_i64(h->userdata,"botw.ready.left.champions",s.ready_panes.left()==2 && champions_owned>0);
    h->publish_i64(h->userdata,"botw.ready.right.weather",s.heal.page.right(s.ready_panes)==0);
    h->publish_i64(h->userdata,"botw.ready.right.buffs",s.heal.page.right(s.ready_panes)==1);
    h->publish_i64(h->userdata,"botw.ready.weapon_drawn",drawn.valid&&drawn.weapon);
    h->publish_i64(h->userdata,"botw.map.koroks.filter",int(korok_filter));
    h->publish_i64(h->userdata,"botw.map.koroks.layer",korok_layer);
    h->publish_i64(h->userdata,"botw.map.koroks.menu",0);
    h->publish_text(h->userdata,"botw.map.koroks.filter_name",!korok_layer?"Off":korok_filter==botw::KorokFilter::Found?"Found":korok_filter==botw::KorokFilter::Unfound?"Not yet found":"All");
    publish_quest(botw::Key("botw.ready.quest"),ready_quest);
    {
      // Distance to the nearest native destination marker when this pane shows the active quest.
      char distance[24]{};
      if(ready_quest&&ready_quest->active&&s.position.valid&&s.quest_markers.valid&&s.quest_markers.count>0) {
        double best=-1;
        for(unsigned i=0;i<s.quest_markers.count&&i<5;++i) {
          const double dx=double(s.quest_markers.positions[i][0])-s.position.x,dy=double(-s.quest_markers.positions[i][2])-s.position.y;
          const double d=std::sqrt(dx*dx+dy*dy);
          if(std::isfinite(d)&&(best<0||d<best))best=d;
        }
        if(best>=1000)std::snprintf(distance,sizeof(distance),"%.1f km",best/1000.0);
        else if(best>=0)std::snprintf(distance,sizeof(distance),"%d m",int(best+.5));
      }
      h->publish_text(h->userdata,"botw.ready.quest.distance",distance);
    }
    if(page&&std::strcmp(page,"quest")==0)for(unsigned i=0;i<512;++i) {
      botw::Key key("botw.quest.item.",i);const auto* q=i<journal.size()?journal[i]:nullptr;
      publish_quest(key,q);h->publish_i64(h->userdata,key(".selected"),q&&q==quest_selected);
      // Native row extras: place line (LocationMarker label) and the right-hand point tab, which
      // shows a hole on an open quest and the destination mark on the active one.
      h->publish_i64(h->userdata,key(".place"),q&&!q->place.empty()?botw::actor_key(q->place):-1);
      h->publish_i64(h->userdata,key(".open"),q&&q->state==1&&!q->active);
      // Native rows draw completed quests dimmed: ".lit" gates the full-strength art and text.
      h->publish_i64(h->userdata,key(".lit"),q&&q->state!=2);
    }
    h->publish_i64(h->userdata,"botw.ready.environment.valid",s.environment.valid);
    char temperature[24]{};
    // Native HUD's own Nt_TempMeterNew dial bakes a "°F" unit glyph (confirmed in every captured
    // live frame, runtime21/22 alike: the Ready-screen captures before and after changing the
    // game options, both with the dial in view on the top
    // screen). The native float read is Celsius; convert to match what the player's own HUD
    // actually shows instead of a silent, unlabeled unit mismatch.
    if(s.environment.valid)std::snprintf(temperature,sizeof(temperature),"%.0f°F",double(s.environment.temperature)*9.0/5.0+32.0);
    h->publish_text(h->userdata,"botw.ready.temperature.text",temperature);
    for(unsigned i=0;i<5;++i)h->publish_i64(h->userdata,botw::Key("botw.ready.weather.",i).c_str(),s.environment.valid ? s.environment.forecast[i] : -1);
    const auto arrows=botw::equipped_arrows(s.data);
    h->publish_i64(h->userdata,"botw.ready.arrow.count",arrows);
    h->publish_i64(h->userdata,"botw.ready.arrow.count.valid",arrows>=0);
    {unsigned kinds{};const auto* next=botw::arrow_step(s.data,1,&kinds);
     h->publish_i64(h->userdata,"botw.ready.arrow.kinds",s.context==botw::UiContext::World?kinds:0);
     h->publish_text(h->userdata,"botw.ready.arrow.next",next&&s.context==botw::UiContext::World?next->actor.c_str():"");}
    const bool hold_valid=hold_scope&&s.held.valid&&s.carry.valid;
    const bool holding=hold_valid&&s.held.count>0;
    const bool hold_pending=s.bridge.pending&&s.bridge.pending_hold;
    bool processed=holding;for(const auto& slot:s.held.slots)if(slot.address&&!slot.flag8)processed=false;
    h->publish_i64(h->userdata,"botw.inventory.hold.supported",s.bridge.hold_supported);
    h->publish_i64(h->userdata,"botw.inventory.hold.refusal",s.hold_decline);
    h->publish_i64(h->userdata,"botw.bridge.frame_ms",std::chrono::duration_cast<std::chrono::milliseconds>(s.bridge.frame_interval).count());
    h->publish_i64(h->userdata,"botw.inventory.hold.valid",hold_valid);
    h->publish_i64(h->userdata,"botw.inventory.hold.active",holding&&processed&&s.carry.active());
    h->publish_i64(h->userdata,"botw.inventory.hold.reserved",hold_valid?int(s.held.count):-1);
    h->publish_i64(h->userdata,"botw.inventory.hold.pending",hold_pending);
    h->publish_i64(h->userdata,"botw.inventory.hold.visible",holding||hold_pending);
    h->publish_i64(h->userdata,"botw.inventory.hold.add_mode",holding&&s.inventory.category==4);
    h->publish_i64(h->userdata,"botw.inventory.hold.put_away.available",holding&&(!s.bridge.pending||s.bridge.hold_return_can_supersede()));
    // Iteration 8 item 7 (owner, 2026-10-18): the title line now reads "Holding N/5" (no spaces
    // around the slash) at rest too, so the full-width over-grid banner can show immediately once
    // a hold is active/pending; the banner itself still only draws while tray_shown (below).
    const std::string held_title="Holding "+std::to_string(hold_valid?s.held.count:0)+"/5";
    h->publish_text(h->userdata,"botw.inventory.hold.title",held_title.c_str());
    h->publish_text(h->userdata,"botw.inventory.hold.status",hold_pending?s.bridge.status:holding?(processed&&s.carry.active()?(s.held.count==5?"Hands full · 5 / 5":"Tap materials to add"):"Preparing to hold"):"");
    {
      // Iteration 5 item 12 (owner, 2026-10-18): the tray (and the detail-pane space it
      // reclaims) shows ONLY while something is actually held or a hold is in flight -- never
      // just because the player is browsing Materials with empty hands. Previously this also
      // went true on category==4 at rest (an always-on empty-tray hint); that regressed the
      // description's space for no reason, so the category term is dropped here.
      // Item 8d sequence: the tray must not hide for the one tick between the bulk put-away
      // confirming (real tray count 0) and the first re-hold confirming -- without this the
      // banner would flicker closed-then-reopen mid-sequence (exactly the flicker item 8d asked
      // to avoid). hold_pending alone does not cover that gap because the put-away request has
      // already completed by then; add the sequence's own still-running flag explicitly.
      const bool tray_shown=botw::hold_tray_shown(hold_valid,s.bridge.hold_supported,holding,hold_pending)||!s.hold_unpick.empty();
      h->publish_i64(h->userdata,"botw.inventory.tray.shown",tray_shown);
      // Item 10 (owner, 2026-10-18): the full detail pane (icon/name/divider/Attack/durability
      // AND the description text, all need_bind this key) must not draw stale selection content
      // when the category is empty -- same inventory_items() emptiness test botw.inventory.empty
      // publishes below, computed here because pane.full is published before `items` exists yet.
      // Iteration 8 item 7: the tray no longer lives in the right pane (it is a full-width banner
      // over the LEFT grid instead), so the detail pane no longer needs to shrink/hide for it.
      const bool category_empty=s.data.valid&&botw::inventory_items(s.data,s.inventory.category).empty();
      h->publish_i64(h->userdata,"botw.inventory.pane.full",!category_empty);
      // Iteration 10 item 4 (owner, 2026-10-20): the right-pane description text widget
      // (inventory_description, tools/i10-inventory-tray.py) already uses both its need_bind
      // (pane.full) and hide_bind (hold.visible, keep[0,0]) slot, so its own two geometry
      // variants (full-height clip when the tray is hidden, tray-shortened clip when it is
      // shown -- same full/short clip split as the grid) cannot add a third, independent
      // tray-state gate directly. These two merged booleans fold the tray-state test into the
      // existing hold.visible exclusion (need_bind stays plain pane.full in both variants):
      //   desc_hide_full -- FULL variant's hide_bind (keep[0,0]): hides if holding/pending OR
      //     the tray is shown (shows only with neither, i.e. the untrayed full-height state).
      //   desc_hide_short -- SHORT variant's hide_bind (keep[0,0]): hides if holding/pending OR
      //     the tray is NOT shown (shows only while actually tray-shortened).
      h->publish_i64(h->userdata,"botw.inventory.desc_hide_full",(holding||hold_pending)||tray_shown);
      h->publish_i64(h->userdata,"botw.inventory.desc_hide_short",(holding||hold_pending)||!tray_shown);
      h->publish_i64(h->userdata,"botw.inventory.hold.idle",tray_shown&&!holding&&!hold_pending);
      const unsigned units=holding?unsigned(s.held.count):0u;
      h->publish_i64(h->userdata,"botw.inventory.hold.units",units);
      h->publish_i64(h->userdata,"botw.inventory.hold.full",holding&&units>=5);
      h->publish_i64(h->userdata,"botw.inventory.hold.refused",std::chrono::steady_clock::now()<s.hold_refused_until);
      // Item 7 (owner): "Hand Full / 2 more to fill" is retired for one pattern, "{open} Slot
      // Open", open = 5 - units (reads "0 Slot Open" at cap instead of a separate "Hands full"
      // string -- same number the old "N more to fill" used, just the one surviving phrasing).
      // Item 8d: one steady "Removing item" message for the whole compound sequence instead of
      // letting each individual step's own transient status (Putting away/Preparing to hold/
      // Items returned/Holding) show through and flicker.
      const std::string counter=!s.hold_unpick.empty()?std::string("Removing item"):hold_pending?std::string(s.bridge.status):
        std::to_string(5-units)+" Slot Open";
      h->publish_text(h->userdata,"botw.inventory.hold.counter",counter.c_str());
      // Item 7's thin hint ("Long press to hold item", between the Recipes/Eat buttons) replaces
      // the idle counter string's old job; shown only for the Materials category while nothing is
      // held/pending, so it is never on screen at the same time as the over-grid banner.
      // Iteration 12 item 14: the footer band carries one message at a time, like the native guide
      // bar -- a status result ("Sorted", "Eaten", ...) replaces the hint instead of drawing over it.
      h->publish_i64(h->userdata,"botw.inventory.hold.hint_visible",
        botw::hold_hint_visible(s.bridge.hold_supported,s.inventory.category,tray_shown,s.inventory_status_shown));
    }
    // Iteration 8 item 7/8 (owner, 2026-10-18): the 5 tray boxes are now the RAW native reservation
    // slots (botw_hold.h HeldTray::slots, manager+0x444A0, in native order) one box per unit, not
    // one box per distinct actor with a count badge -- two units of the same material now draw as
    // two filled boxes with the same icon (item 8b), matching what a live memory read of the tray
    // shows and what "tap a box unit removes that one unit" (item 8d) needs to target unambiguously.
    BOTW_PERF_END(PublishMisc);
    BOTW_PERF_BEGIN(InventoryGrid);
    for(unsigned i=0;i<5;++i){botw::Key key("botw.inventory.hold.slot.",i);
      const auto& slot=s.held.slots[i];const bool filled=hold_valid&&slot.address!=0;
      h->publish_i64(h->userdata,key(".filled"),filled);
      h->publish_i64(h->userdata,key(".empty"),hold_valid&&!filled);
      h->publish_text(h->userdata,key(".actor"),filled?slot.actor.c_str():"");
    }
    // Grouped-by-actor keys kept (no manifest page still reads them after this round's inventory-
    // tray redesign to raw slots; left rather than deleted in case a future page wants the old
    // "fewer boxes, counted" look and can reuse this aggregation instead of rebuilding it).
    std::vector<std::pair<std::string,unsigned>> held_groups;
    if(holding)for(const auto& slot:s.held.slots)if(slot.address){
      auto found=std::find_if(held_groups.begin(),held_groups.end(),[&](const auto& group){return group.first==slot.actor;});
      if(found==held_groups.end())held_groups.emplace_back(slot.actor,1);else ++found->second;
    }
    h->publish_i64(h->userdata,"botw.inventory.hold.groups.count",held_groups.size());
    for(unsigned i=0;i<5;++i){botw::Key key("botw.inventory.hold.group.",i);const bool present=i<held_groups.size();
      h->publish_text(h->userdata,key(".actor"),present?held_groups[i].first.c_str():"");
      h->publish_text(h->userdata,key(".icon"),present?held_groups[i].first.c_str():"");
      h->publish_i64(h->userdata,key(".count"),present?held_groups[i].second:0);
    }
    s.inventory.update(s.data);
    const auto items=botw::inventory_items(s.data,s.inventory.category);
    h->publish_i64(h->userdata,"botw.inventory.category",s.inventory.category);
    h->publish_i64(h->userdata,"botw.inventory.count",items.size());
    // 2026-10-18 drag shortcuts: every tile drags while the grid cannot scroll (<= 4 rows of 5);
    // in a scrolling category only the selected tile drags, so a drag elsewhere still scrolls.
    // Iteration 5 item 10 (owner, 2026-10-18): an empty category vacuously satisfies
    // "<=20 items, so every tile drags" and used to leave drag.all true, which painted a
    // full grid of ghost drop-target outlines over a category the owner wants fully
    // blank. drag.all now also requires at least one real item.
    h->publish_i64(h->userdata,"botw.inventory.drag.all",!items.empty()&&items.size()<=20);
    // Iteration 10 item 4 (owner, 2026-10-20): the grid's drag-ghost/selection-highlight tile
    // widgets (rl_inv_ghost_cells, dg_inv_tile_sel) already use both their need_bind AND
    // hide_bind slot for their own per-item/drag.all logic, so the new full-height-vs-
    // tray-shortened grid widget variants (tools/i10-inventory-tray.py) cannot add a third,
    // independent tray-state gate to either widget directly -- there is no third slot. These
    // four merged booleans give each variant's otherwise-full slot pair exactly the single
    // extra AND/OR term it needs (recomputed here, not reusing the tray_shown local above,
    // which is scoped to its own block; same inputs, still in scope at this point):
    //   drag_all.not_tray / drag_all.tray -- ghost_cells' need_bind (currently plain drag.all)
    //     becomes one of these per variant, hide_bind (count/{i} range) unchanged.
    //   drag_all_or_tray / drag_all_or_not_tray -- tile_sel's hide_bind (currently drag.all,
    //     keep[0,0]) becomes one of these per variant with the same keep[0,0], folding the
    //     tray-state exclusion into the existing drag.all exclusion; need_bind (per-item
    //     selected.{i}) unchanged.
    {
      const bool i10_tray_shown=botw::hold_tray_shown(hold_valid,s.bridge.hold_supported,holding,hold_pending)||!s.hold_unpick.empty();
      const bool i10_drag_all=!items.empty()&&items.size()<=20;
      h->publish_i64(h->userdata,"botw.inventory.drag_all.not_tray",i10_drag_all&&!i10_tray_shown);
      h->publish_i64(h->userdata,"botw.inventory.drag_all.tray",i10_drag_all&&i10_tray_shown);
      h->publish_i64(h->userdata,"botw.inventory.drag_all_or_tray",i10_drag_all||i10_tray_shown);
      h->publish_i64(h->userdata,"botw.inventory.drag_all_or_not_tray",i10_drag_all||!i10_tray_shown);
    }
    // Fix round A (CRITIQUE-2 Inventory M): the drag ghost is drawn last and covers the action
    // slot's own label at drop time, so the page shows the slot's verb for the DRAGGED tile in a
    // caption above the footer. The runtime publishes "@drag*" after the module runs, so the
    // module publishes each draggable tile's verb (Equip 1 / Eat 2 / Remove 3, as the slot would
    // do on a drop) and a manifest derived value picks the dragged one (fix-round-a.py).
    {
      auto verb_of=[&](const botw::Item& item)->int64_t{
        switch(inventory_slot(s,*h,item)){case InventorySlot::Equip:return 1;case InventorySlot::Consume:return 2;
          case InventorySlot::Remove:return 3;default:return 0;}};
      const bool all=items.size()<=20;
      for(size_t i=0;i<20;++i)h->publish_i64(h->userdata,botw::Key("botw.inventory.item.",unsigned(i))(".drag_verb"),all&&i<items.size()?verb_of(*items[i]):0);
      int64_t selected_index=-1;for(size_t i=0;i<items.size();++i)if(items[i]->address==s.inventory.selected)selected_index=int64_t(i);
      h->publish_i64(h->userdata,"botw.inventory.selected.index",selected_index);
      h->publish_i64(h->userdata,"botw.inventory.selected.drag_verb",selected_index>=0?verb_of(*items[size_t(selected_index)]):0);
    }
    constexpr const char* category_names[]={"Weapons","Bows and arrows","Shields","Armor","Materials","Food","Key items"};
    h->publish_text(h->userdata,"botw.inventory.title",category_names[s.inventory.category]);
    const int sort_mode=s.bridge.sort_mode(s.inventory.category);
    h->publish_text(h->userdata,"botw.inventory.sort.label",botw::SelectionBridge::sort_label(s.inventory.category,sort_mode));
    h->publish_i64(h->userdata,"botw.inventory.sort.mode",sort_mode);
    h->publish_i64(h->userdata,"botw.inventory.sort.available",s.context==botw::UiContext::World && s.bridge.healthy && s.bridge.sort_supported && !s.bridge.pending && s.bridge.inventory_scope() && s.data.valid && !items.empty() && s.held.valid && s.held.count==0 && sort_mode>=0);

    const botw::Item* selected{};
    for(const auto* item:items)if(item->address==s.inventory.selected)selected=item;
    h->publish_i64(h->userdata,"botw.inventory.selected.valid",selected!=nullptr);
    const auto art_milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    h->publish_text(h->userdata,"botw.inventory.selected",botw::inventory_art(selected,art_milliseconds).c_str());
    if(const int detail_scale=botw::detail_art_scale(selected))h->publish_i64(h->userdata,"botw.inventory.selected.art_scale",detail_scale);
    h->publish_i64(h->userdata,"botw.inventory.selected.name",selected ? botw::actor_key(selected->actor) : -1);
    publish_description_clean("botw.inventory.selected.description_clean",selected ? botw::actor_key(selected->actor) : -1);
    botw::publish_recovery(*h,"botw.inventory.selected.recovery",botw::recovery(*h,selected));
    botw::publish_pause_item(*h,selected,s.data);
    h->publish_i64(h->userdata,"botw.inventory.selected.equipped",selected && selected->equipped);
    h->publish_i64(h->userdata,"botw.inventory.selected.quantity",selected ? botw::item_quantity(*selected) : -1);
    const auto inventory_detail=selected&&(selected->type==0||selected->type==1||selected->type==3)?botw::quick_detail(*selected,botw::gear_modifier(*h,selected->address),&s.counters.master_sword()):botw::QuickDetail{};
    h->publish_i64(h->userdata,"botw.inventory.selected.stat",inventory_detail.stat);
    h->publish_i64(h->userdata,"botw.inventory.selected.stat_kind",inventory_detail.stat_kind);
    h->publish_i64(h->userdata,"botw.inventory.selected.durability_pct",inventory_detail.condition);
    // Iteration 9 item 2: the Inventory right-pane detail row needs the same modifier
    // icon/name/value triple the quick-picker detail pane already publishes (botw.quick.
    // selected.modifier*, 01007EF00011E000.cpp:787-811) -- same botw::modifier_icon_for()
    // single source of truth, same inventory_detail (quick_detail()) already computed two
    // lines above for .stat/.stat_kind/.durability_pct, just never carried the modifier
    // fields through to this selected-item publish block before this round.
    {
      const char* inv_sel_modifier_icon=botw::modifier_icon_for(inventory_detail.modifier,inventory_detail.shots);
      const uint32_t inv_sel_modifier_shown=inventory_detail.modifier;
      uint32_t inv_sel_modifier_message=inv_sel_modifier_shown;
      if(inv_sel_modifier_message==16 && inventory_detail.shots!=5)inv_sel_modifier_message=0;
      if(inventory_detail.yellow && (inv_sel_modifier_message==1 || inv_sel_modifier_message==2 || inv_sel_modifier_message==256))inv_sel_modifier_message|=0x80000000u;
      h->publish_i64(h->userdata,"botw.inventory.selected.modifier",inv_sel_modifier_shown);
      h->publish_i64(h->userdata,"botw.inventory.selected.modifier_value",inv_sel_modifier_shown?inventory_detail.modifier_value:0);
      h->publish_i64(h->userdata,"botw.inventory.selected.modifier_message",inv_sel_modifier_message);
      h->publish_text(h->userdata,"botw.inventory.selected.modifier_icon",inv_sel_modifier_icon);
      botw::publish_detail_line(*h,"botw.inventory.selected",inventory_detail.stat,selected?botw::item_quantity(*selected):-1,inv_sel_modifier_message,inventory_detail.modifier_value,true);
      botw::publish_pause_gear(*h,"botw.inventory.selected",inventory_detail.stat,inventory_detail.shots,inv_sel_modifier_message,inventory_detail.modifier_value,inv_sel_modifier_icon,true);
    }
    BOTW_PERF_END(InventoryGrid);

    h->publish_i64(h->userdata,"botw.inventory.empty",s.data.valid && items.empty());
    { BOTW_PERF_TIMER(ItemSlots);
    for(unsigned i=0;i<(page && std::strcmp(page,"inventory")==0 ? 420u : 16u);++i) {
      botw::Key key("botw.inventory.item.",i);
      const auto* item=i<items.size() ? items[i] : nullptr;
      const auto token=item&&i<s.hold_targets.cells.size()&&botw::HoldTargets::same(*item,s.hold_targets.cells[i].item)?s.hold_targets.cells[i].token:0;
      unsigned held_units{};if(item&&holding)for(const auto& slot:s.held.slots)held_units+=slot.address==item->address;
      h->publish_i64(h->userdata,key(".hold_token"),token);
      h->publish_i64(h->userdata,key(".held"),held_units);
      h->publish_text(h->userdata,key(".held_text"),held_units?("Held x"+std::to_string(held_units)).c_str():"");
      h->publish_text(h->userdata,key.c_str(),botw::inventory_art(item,art_milliseconds).c_str());
      // Iteration 13 item K: key-item art at native size; other cells stay unscaled (no key).
      if(const int art_scale=botw::key_item_art_scale(item))h->publish_i64(h->userdata,key(".art_scale"),art_scale);
      if(const int art_dy=botw::key_item_art_dy(item))h->publish_i64(h->userdata,key(".art_dy"),art_dy);
      const auto inventory_cell=item&&(item->type==0||item->type==1||item->type==3)?botw::quick_detail(*item,botw::gear_modifier(*h,item->address),&s.counters.master_sword()):botw::QuickDetail{};
      const auto* armor=item&&item->type>=4&&item->type<=6?botw::armor_status(item->actor):nullptr;
      h->publish_i64(h->userdata,key(".stat"),armor?armor->defense:inventory_cell.stat);
      const auto cell_recovery=botw::recovery(*h,item);
      h->publish_i64(h->userdata,key(".heals"),cell_recovery.valid&&(cell_recovery.full||cell_recovery.quarters>0));
      h->publish_i64(h->userdata,key(".name"),item ? botw::actor_key(item->actor) : -1);
      h->publish_i64(h->userdata,key(".quantity"),item ? botw::item_quantity(*item) : -1);
      h->publish_i64(h->userdata,key(".selected"),item && item==selected);
      h->publish_i64(h->userdata,key(".equipped"),item && item->type<=6 && item->equipped);
      h->publish_text(h->userdata,key(".effect_badge"),botw::item_badge(*h,item).c_str());
      // Layout batch 1 item (d): give the Inventory weapon/bow/shield grid the same per-cell
      // modifier corner badge the pickers already have (botw.quick.item.{i}.modifier_icon/
      // value) -- reuses the quick_detail() read already computed above for inventory_cell.
      // Layout batch 2 fix round 1: same icon-less suppression as the picker grid badge --
      // don't show a bare value with no icon (e.g. Durability Up, bit 2, has none).
      const char* inv_modifier_icon=botw::modifier_icon_for(inventory_cell.modifier,inventory_cell.shots);
      h->publish_text(h->userdata,key(".modifier_icon"),inv_modifier_icon);
      h->publish_i64(h->userdata,key(".modifier_value"),inv_modifier_icon[0]?int64_t(inventory_cell.modifier_value):0);
    }
    }
    h->publish_i64(h->userdata, "botw.actions_ready", s.bridge.healthy && s.bridge.scope() &&
      !s.bridge.pending && (s.gear.valid || s.runes.valid));
    h->publish_i64(h->userdata, "botw.pouch_valid", s.data.valid);
    h->publish_i64(h->userdata, "botw.pouch_count",
                   s.data.valid ? s.data.items.size() : 0);
    h->publish_text(h->userdata, "botw.status",
                    !s.data.valid          ? "Inventory unavailable"
                    : s.data.items.empty() ? "No loaded inventory"
                                           : "Inventory data available");
    h->publish_i64(h->userdata,"botw.ready.rupees.valid",s.currency.valid);
    h->publish_i64(h->userdata,"botw.ready.rupees",s.currency.valid ? s.currency.value : -1);
    std::string rupees;
    if(s.currency.valid) {
      // Native-style grouping: 347549 -> 347,549.
      const auto digits=std::to_string(s.currency.value<0?-int64_t(s.currency.value):int64_t(s.currency.value));
      for(size_t i=0;i<digits.size();++i){if(i&&(digits.size()-i)%3==0)rupees.push_back(',');rupees.push_back(digits[i]);}
      if(s.currency.value<0)rupees.insert(rupees.begin(),'-');
    }
    h->publish_text(h->userdata,"botw.ready.rupees.text",rupees.c_str());
    h->publish_i64(h->userdata,"botw.ready.rune.valid",s.current_rune.valid);
    h->publish_i64(h->userdata,"botw.ready.rune.id",s.current_rune.valid ? s.current_rune.id : -1);
    const auto* rune=s.current_rune.valid ? botw::rune_actor(s.current_rune.id,s.current_rune.upgraded,s.current_rune.upgraded) : "";
    h->publish_text(h->userdata,"botw.ready.rune",rune);
    h->publish_i64(h->userdata,"botw.ready.rune.name",botw::actor_key(rune));
    const auto ready=botw::ready_equipment(s.data);
    constexpr const char* ready_keys[]={"botw.ready.weapon","botw.ready.bow","botw.ready.arrow","botw.ready.shield"};
    for (unsigned i=0;i<ready.size();++i) {
      h->publish_text(h->userdata,ready_keys[i],ready[i].c_str());
      h->publish_i64(h->userdata,botw::Key(ready_keys[i])(".valid"),!ready[i].empty());
    }
    { BOTW_PERF_TIMER(ReadyRecentGearMod);
      const auto items=botw::ready_items(s.data);
      for(unsigned i:{0u,1u,3u}) {
        const auto mod=items[i] && items[i]->actor==ready[i] ? botw::gear_modifier(*h,items[i]->address) : botw::GearModifier{};
        const int pct=items[i] && items[i]->actor==ready[i] ? botw::durability_percent(*items[i],mod.flags,mod.value) : -1;
        botw::Key key(ready_keys[i]);
        h->publish_i64(h->userdata,key(".durability_pct"),pct);
        h->publish_i64(h->userdata,key(".stat"),items[i]&&mod.valid?botw::quick_detail(*items[i],mod,&s.counters.master_sword()).stat:-1);
        h->publish_i64(h->userdata,key(".low"),pct>=0 && pct<=20);
        h->publish_i64(h->userdata,key(".durability_raw"),items[i] && items[i]->actor==ready[i] ? items[i]->value : -1);
        h->publish_i64(h->userdata,key(".modifier_flags"),mod.valid?int64_t(mod.flags):-1);
        h->publish_i64(h->userdata,key(".modifier_value"),mod.valid?int64_t(mod.value):-1);
      }
    }
    h->publish_text(h->userdata,"botw.ready.hint",s.ready_hint.c_str());
    h->publish_i64(h->userdata,"botw.ready.effects.valid",s.effects.valid);
    h->publish_i64(h->userdata,"botw.ready.effects.count",s.effects.valid?s.effects.count:0);
    for(unsigned r=0;r<3;++r) {
      const bool on=s.effects.valid && r<s.effects.count;char time[16]{};
      if(on)botw::format_effect_time(s.effects.rows[r].seconds,time);
      botw::Key key("botw.ready.effect.",r);
      h->publish_i64(h->userdata,key(".kind"),on?int(s.effects.rows[r].kind):-1);
      h->publish_text(h->userdata,key(".time"),time);
    }
    std::string equipped;
    if (s.data.valid)
      for (auto &item : s.data.items)
        if (item.equipped && item.type == 0)
          equipped = item.actor;
    h->publish_text(h->userdata, "botw.equipped_weapon", equipped.c_str());
    { BOTW_PERF_TIMER(ItemSlots);
    for (unsigned i = 0; i < (page && std::strcmp(page,"inventory")==0 ? 420u : 16u); i++) {
      botw::Key key("botw.item.",i);
      const bool have = s.data.valid && i < s.data.items.size();
      h->publish_text(h->userdata, key.c_str(), have ? s.data.items[i].actor.c_str() : "");
      h->publish_i64(h->userdata,key(".name"),have ? botw::actor_key(s.data.items[i].actor) : -1);
    }
    }
    if(record_replay){
      if(s.context==botw::UiContext::World||s.context==botw::UiContext::Menu)s.replay.commit();
      else s.replay.discard();
    }
#ifdef BOTW_PERF_INSTRUMENT
    botw::perf::MaybeReport(h, now);
#endif
  } catch (...) {
    auto& s=*static_cast<State*>(p);
    // A reader that throws mid-transition (e.g. while the game tears down a broken weapon) must not
    // cost the player their secondary: log the exception (rate-limited) so a device run shows which
    // reader failed, and keep s.recent below -- every RecentGear lookup re-validates address+actor+
    // type+value>0 against the live pouch, and epoch/title/loading still clear it.
    {static auto last=std::chrono::steady_clock::time_point{};const auto t=std::chrono::steady_clock::now();
     if(h && h->log && t-last>std::chrono::seconds(5)){last=t;const char* what="unknown";
       try{throw;}catch(const std::exception& e){what=e.what();}catch(...){}
       char m[192];std::snprintf(m,sizeof m,"BOTW sample: exception '%s' (state reset, secondary kept)",what);h->log(h->userdata,2,m);}}
    s.replay.discard();s.event_holding=false;
    s.hold_targets.clear();s.held={};s.carry={};
    for(const auto* key:{"botw.inventory.hold.valid","botw.inventory.hold.active","botw.inventory.hold.visible","botw.inventory.hold.add_mode","botw.inventory.hold.put_away.available","botw.inventory.consume.available","botw.inventory.tray.shown","botw.inventory.hold.idle","botw.inventory.drag.all","botw.inventory.drag_all.not_tray","botw.inventory.drag_all.tray","botw.inventory.drag_all_or_tray","botw.inventory.drag_all_or_not_tray","botw.inventory.desc_hide_full","botw.inventory.desc_hide_short"})h->publish_i64(h->userdata,key,0);
    h->publish_i64(h->userdata,"botw.inventory.pane.full",1);
    h->publish_i64(h->userdata,"botw.map.koroks.menu",0);
    botw::publish_champion_ownership(*h,{});
    h->publish_i64(h->userdata,"botw.map.edit.available",0);h->publish_i64(h->userdata,"botw.map.edit.selected",0);
    s.touch_kind=botw::QuickKind::None;s.touch_gear={};s.touch_select_deferred=false;
    s.map_tap_deferred=false;s.map_marker_deferred=false;s.map_delete_deferred=false;s.map_selected=-1;
    h->publish_i64(h->userdata,"botw.inventory.equip.available",0);
    h->publish_i64(h->userdata,"botw.inventory.remove.available",0);
    s.bridge.cancel();
    s.recent_actor_valid={};s.recent_deferred=false; // s.recent kept (see above)
    s.quest_deferred=false;
    for(const auto* key:{"botw.ready.recent.weapon","botw.ready.recent.bow"}) {h->publish_text(h->userdata,key,"");h->publish_i64(h->userdata,botw::Key(key)(".valid"),0);}
    s.data = {};
    s.currency = {};
    s.clock = {};
    s.stamina = {};
    s.noise = {};
    s.sensor = {};
    botw::publish_sensor(*h,{});
    s.temperature_dial={};botw::publish_temperature_dial(*h,{});
    h->publish_i64(h->userdata,"botw.ready.noise.valid",0);
    h->publish_i64(h->userdata,"botw.ready.noise.percent",-1);
    s.environment = {};
    s.quests = {};
    s.map = {};
    s.quest_markers = {};
    s.pins = {};
    botw::publish_map_pins(*h,{});
    s.saved_icons = {};
    botw::publish_map_stamps(*h,{});
    botw::publish_ready_stamps(*h,{},{});botw::publish_ready_edit(*h,{},{});
    s.area = {};
    botw::publish_map_area(*h,{});
    h->publish_i64(h->userdata,"botw.ready.area.show",0);
    s.landmarks = {};
    botw::publish_map_landmarks(*h,{});
    s.shrine_compass=botw::ShrineCompassController{};s.compass_out={};s.compass_shown=false;botw::publish_shrine_compass(*h,false,{});
    s.temp_card.clear();s.temp_card.publish(*h,false,false);
    s.heal={};s.heal.publish(*h,false);
    s.cards.active=0;s.cards.publish(*h);
    s.travel.reset();
    for(const auto* key:{"botw.warp.supported","botw.warp.map.available","botw.warp.tracker.available","botw.warp.confirm","botw.warp.status.visible"})h->publish_i64(h->userdata,key,0);
    h->publish_i64(h->userdata,"botw.warp.confirm.name",-1);h->publish_text(h->userdata,"botw.warp.status","");
    botw::publish_map_places(*h,{});botw::publish_map_regions(*h,{});botw::publish_map_names(*h,{});botw::publish_map_moving(*h,{});botw::publish_map_koroks(*h,{});botw::publish_map_death(*h,{});botw::publish_map_special(*h,{});botw::publish_map_labels(*h,{});botw::publish_ready_caption(*h,{});
    h->publish_i64(h->userdata,"botw.map.quest.valid",0);
    h->publish_i64(h->userdata,"botw.map.quest.count",0);
    s.position = {};s.view = {};
    s.states = {};s.state_flags.clear();botw::publish_states(*h,{});
    s.state_latch.dim=false;s.state_latch.kind=0;s.state_latch.on_since=-1;s.state_latch.off_since=-1;botw::publish_state_latch(*h,s.state_latch);
    for(const auto* key:{"botw.map.valid","botw.map.minimap.valid","botw.map.position.valid","botw.map.heading.valid",
                         "botw.map.view.valid","botw.map.view.rotation","botw.map.view.heading.valid","botw.map.view.heading",
                         "botw.map.view.cone.valid","botw.map.view.cone"})h->publish_i64(h->userdata,key,0);
    s.quest_selected=0;
    for(const auto* key:{"botw.quest.action.available","botw.quest.valid","botw.quest.count","botw.quest.selected.valid","botw.ready.quest.valid"})h->publish_i64(h->userdata,key,0);
    for(const auto* key:{"botw.quest.selected.name","botw.quest.selected.description","botw.ready.quest.name","botw.ready.quest.description"})h->publish_i64(h->userdata,key,-1);
    h->publish_i64(h->userdata,"botw.ready.environment.valid",0);
    h->publish_text(h->userdata,"botw.ready.temperature.text","");
    for(unsigned i=0;i<5;++i)h->publish_i64(h->userdata,botw::Key("botw.ready.weather.",i).c_str(),-1);
    h->publish_i64(h->userdata,"botw.ready.stamina.valid",0);
    h->publish_i64(h->userdata,"botw.ready.stamina.depleted",0);
    h->publish_i64(h->userdata,"botw.ready.stamina.percent",-1);
    s.inventory.selected=0;
    for(const auto* key:{"botw.state.title","botw.state.loading","botw.ready.clock.valid","botw.ready.arrow.count.valid","botw.inventory.count","botw.inventory.selected.valid","botw.inventory.empty"})h->publish_i64(h->userdata,key,0);
    h->publish_i64(h->userdata,"botw.state.unknown",1);
    h->publish_i64(h->userdata,"botw.ready.clock.minutes",-1);
    h->publish_text(h->userdata,"botw.ready.clock.text","");
    h->publish_i64(h->userdata,"botw.ready.arrow.count",-1);
    h->publish_i64(h->userdata,"botw.ready.arrow.kinds",0);h->publish_text(h->userdata,"botw.ready.arrow.next","");
    h->publish_text(h->userdata,"botw.inventory.selected","");
    h->publish_i64(h->userdata,"botw.inventory.selected.name",-1);
    h->publish_i64(h->userdata,"botw.inventory.selected.quantity",-1);
    h->publish_i64(h->userdata,"botw.inventory.selected.equipped",0);
    s.current_rune = {};
    h->publish_i64(h->userdata,"botw.ready.rupees.valid",0);
    h->publish_i64(h->userdata,"botw.ready.rupees",-1);
    h->publish_text(h->userdata,"botw.ready.rupees.text","");
    h->publish_text(h->userdata,"botw.ready.quest.distance","");
    h->publish_i64(h->userdata,"botw.ready.rune.valid",0);
    h->publish_i64(h->userdata,"botw.ready.rune.id",-1);
    h->publish_i64(h->userdata,"botw.ready.rune.name",-1);
    h->publish_text(h->userdata,"botw.ready.rune","");
    s.quick = {};
    s.gear = {};
    h->publish_i64(h->userdata,"botw.quick.highlighted",-1);
    for(const char* key:{"botw.quick.touch_gear","botw.quick.touch_rune","botw.quick.held_gear","botw.quick.held_rune","botw.quick.selected.valid"})h->publish_i64(h->userdata,key,0);
    s.held_picker_open=false;
    h->publish_i64(h->userdata,"botw.quick.weapon",0);
    s.runes={};
    h->publish_i64(h->userdata,"botw.quick.rune",0);
    h->publish_i64(h->userdata,"botw.quick.gear",0);
    h->publish_text(h->userdata,"botw.quick.title","");
    for (unsigned i=0;i<21;++i) {
      botw::Key key("botw.quick.item.",i);
      h->publish_i64(h->userdata,key(".valid"),0);
      h->publish_text(h->userdata,key.c_str(),"");
      h->publish_i64(h->userdata,key(".name"),-1);
      h->publish_i64(h->userdata,key(".equipped"),0);
      h->publish_i64(h->userdata,key(".durability_pct"),-1);
      h->publish_text(h->userdata,key(".modifier_icon"),"");
      h->publish_i64(h->userdata,key(".modifier_value"),0);
      h->publish_i64(h->userdata,key(".modifier_yellow"),0);
    }
    h->publish_i64(h->userdata,"botw.quick.has_none",0);
    h->publish_text(h->userdata,"botw.quick.has_none_icon","");
    h->publish_i64(h->userdata,"botw.quick.none_selected",0);
    h->publish_i64(h->userdata,"botw.quick.confirm_label",0);
    h->publish_i64(h->userdata,"botw.quick.confirm_box",0);
    h->publish_i64(h->userdata,"botw.quick.rows",0);
    h->publish_i64(h->userdata,"botw.quick.grid.center_x",0);
    h->publish_i64(h->userdata,"botw.quick.gear.panel_dy",0);  // iteration 10 item 9 reset
    for(int r=1;r<=4;++r) {  // iteration 10 item 10: extended 3 -> 4 buckets
      botw::Key tk("botw.quick.touch_gear.rows",uint64_t(r));
      botw::Key hk("botw.quick.held_gear.rows",uint64_t(r));
      h->publish_i64(h->userdata,tk.c_str(),0);
      h->publish_i64(h->userdata,hk.c_str(),0);
    }
    h->publish_i64(h->userdata,"botw.quick.rune_rows",0);
    for(int r=1;r<=3;++r) {
      botw::Key trk("botw.quick.touch_rune.rows",uint64_t(r));
      botw::Key hrk("botw.quick.held_rune.rows",uint64_t(r));
      h->publish_i64(h->userdata,trk.c_str(),0);
      h->publish_i64(h->userdata,hrk.c_str(),0);
    }
    s.context = botw::UiContext::Unknown;
    h->publish_text(h->userdata, "botw.context", "Waiting for game context");
    h->publish_i64(h->userdata, "botw.actions_ready", 0);
    h->publish_i64(h->userdata, "botw.pouch_valid", 0);
    h->publish_i64(h->userdata, "botw.pouch_count", 0);
    h->publish_text(h->userdata, "botw.status", "Inventory unavailable");
    for (const auto* key:{"botw.ready.weapon","botw.ready.bow","botw.ready.arrow","botw.ready.shield"}) {
      botw::Key name(key);
      h->publish_text(h->userdata,key,"");
      h->publish_i64(h->userdata,name(".valid"),0);
      h->publish_i64(h->userdata,name(".durability_pct"),-1);
      h->publish_i64(h->userdata,name(".stat"),-1);
      h->publish_i64(h->userdata,name(".low"),0);
    }
    h->publish_text(h->userdata, "botw.equipped_weapon", "");
    h->publish_i64(h->userdata,"botw.ready.effects.valid",0);h->publish_i64(h->userdata,"botw.ready.effects.count",0);
    for(unsigned r=0;r<3;++r){botw::Key key("botw.ready.effect.",r);h->publish_i64(h->userdata,key(".kind"),-1);h->publish_text(h->userdata,key(".time"),"");}
    for (unsigned i = 0; i < 420; i++) {
      botw::Key key("botw.item.",i);
      h->publish_text(h->userdata, key.c_str(), "");
      h->publish_i64(h->userdata,key(".name"),-1);
    }
  }
}
void tick(void *p, const EdenDsmodHostApi *h) {
  auto& s=*static_cast<State*>(p);
  // Item E: runs every host tick, shown or hidden (module_tick_hidden). A host without the
  // __aux_present query keeps the companion HUD rules (fallback 1), as before this change.
  bool hud_shown=!h->get_i64 || h->get_i64(h->userdata,"__aux_present",1)!=0;
#ifdef BOTW_HUD_HIDDEN_TEST_HOOK
  // Desktop-harness-only build (never packaged): the harness cannot hide its virtual aux screen
  // mid-run, so a marker file stands in for "companion hidden" to film the shown -> hidden ->
  // re-shown edges in one session. The real path above is exercised by a no-aux boot.
  if(const char* marker=std::getenv("BOTW_HUD_HIDDEN_TEST_FILE"))
    if(FILE* f=std::fopen(marker,"rb")){std::fclose(f);hud_shown=false;}
#endif
  s.bridge.sync_native_hud(hud_shown);
  if (!h->get_i64 || h->get_i64(h->userdata,"__aux_present",0)==0) {
    s.quick={};s.recent_deferred=false;s.quest_deferred=false;s.bridge.cancel();
  }
}
const EdenDsmodModuleApi api{EDEN_DSMOD_MODULE_ABI_VERSION,
                             sizeof(EdenDsmodModuleApi),
                             0,
                             EDEN_DSMOD_MODULE_ABI_HASH,
                             0x01007EF00011E000ULL,
                             "Breath of the Wild companion prototype",
                             EDEN_DSMOD_CAP_TICK_WHEN_HIDDEN | EDEN_DSMOD_CAP_EXTENSIONS,
                             supports,
                             create,
                             destroy,
                             sample,
                             tick};
void configure(void* p,const EdenDsmodHostExtensions* h) {
  static_cast<State*>(p)->bridge.configure(h);
}
// Which native request the Inventory action slot shows for `i` (Equip / Eat / Remove), using the
// same preconditions the slot's availability flags publish. Shared by the published flags and the
// drag-to-slot shortcut so a drop does exactly what tapping the slot would do.
InventorySlot inventory_slot(const State& s,const EdenDsmodHostApi& h,const botw::Item& i) {
  if(!(s.bridge.healthy && s.bridge.inventory_supported && !s.bridge.pending &&
       s.bridge.inventory_scope() && s.context==botw::UiContext::World && s.data.valid &&
       (!s.bridge.hold_supported || (s.held.valid&&!s.held.count))))return InventorySlot::None;
  if(s.bridge.consume_supported && botw::consumable(h,i).valid)return InventorySlot::Consume;
  if(s.bridge.remove_supported && i.equipped && (i.type==0 || i.type==1 || i.type==3 || (s.bridge.remove_armor_supported && i.type>=4 && i.type<=6)))return InventorySlot::Remove;
  if(!i.equipped && ((i.value>0 && (i.type==0 || i.type==1 || i.type==3 || (s.bridge.arrows_supported && i.type==2))) ||
     (((s.bridge.lower_armor_supported && i.type==6) || (s.bridge.armor_supported && (i.type==4 || i.type==5))) && i.value>=-1 && i.value<=15)))return InventorySlot::Equip;
  return InventorySlot::None;
}
// Heal advisor actions (food card, Ready page): eat a listed row through the same guarded
// native consume request as Inventory Eat, after re-checking the row's pouch identity.
#include "01007EF00011E000_actions.h"
// Exceptions (allocation failure) must not unwind into the host: refuse the action instead,
// mirroring sample()'s fail-closed handler. No guest write is armed before a request returns.
EdenDsmodBool on_action(void* p,const char* action,int64_t argument) {
  try {auto& s=*static_cast<State*>(p);s.press.note(action,argument,std::chrono::steady_clock::now());return handle_action(s,action,argument);}
  catch(...) {return false;}
}
// Native layout animations ("module:fx/<effect>/<frame>", botw_layout_fx.h): composed on the asset
// worker from the player's own romfs, once per frame key (the runtime keeps the decoded image).
EdenDsmodBool load_image(void*,const EdenDsmodHostApi* h,const char* key,void* receiver,EdenDsmodImageSink sink) {
  try {
    std::string effect;int frame=0;
    if(!h||!key||!sink||!botw::fx::ParseFrameKey(key,effect,frame))return false;
    auto& renderer=botw::fx::Shared();
    const auto read=[h](const char* path,std::vector<uint8_t>& out)->bool{
      if(!h->read_romfs)return false;
      const size_t n=h->read_romfs(h->userdata,path,0,nullptr,0);
      if(!n||n>(64u<<20))return false;
      out.resize(n);
      return h->read_romfs(h->userdata,path,0,out.data(),n)==n;};
    // Telemetry (thread CPU time): one line when the layouts load, then every 100 frames.
    const auto cpu_ms=[]{timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return double(t.tv_sec)*1e3+double(t.tv_nsec)/1e6;};
    static bool loaded_logged=false;static double render_ms=0;static unsigned rendered=0;
    const double t0=cpu_ms();
    if(!renderer.Ensure(read))return false;
    if(!loaded_logged&&h->log){loaded_logged=true;char line[96];std::snprintf(line,sizeof line,"BOTW fx: native layouts loaded in %.1f ms CPU",cpu_ms()-t0);h->log(h->userdata,1u,line);}
    std::vector<uint8_t> rgba;int w=0,hh=0;
    const double t1=cpu_ms();
    if(!renderer.Render(effect,frame,rgba,w,hh))return false;
    render_ms+=cpu_ms()-t1;
    if(++rendered%100==0&&h->log){char line[96];std::snprintf(line,sizeof line,"BOTW fx: %u frames, %.2f ms CPU/frame",rendered,render_ms/rendered);h->log(h->userdata,1u,line);}
    sink(receiver,uint32_t(w),uint32_t(hh),rgba.data(),rgba.size());
    return true;
  } catch(...) {return false;}
}
const EdenDsmodModuleExtensions extensions{EDEN_DSMOD_EXT_VERSION,
  sizeof(EdenDsmodModuleExtensions),EDEN_DSMOD_EXT_HASH,configure,on_action,load_image};
} // namespace
extern "C" __attribute__((visibility("default"))) const EdenDsmodModuleApi *
eden_dsmod_get_module(uint32_t v, uint64_t hash) {
  return v == EDEN_DSMOD_MODULE_ABI_VERSION &&
                 hash == EDEN_DSMOD_MODULE_ABI_HASH
             ? &api
             : nullptr;
}
extern "C" __attribute__((visibility("default"))) const EdenDsmodModuleExtensions*
eden_dsmod_get_extensions(uint32_t v,uint64_t hash) {
  return v==EDEN_DSMOD_EXT_VERSION && hash==EDEN_DSMOD_EXT_HASH ? &extensions : nullptr;
}
