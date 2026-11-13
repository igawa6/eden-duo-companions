// SPDX-License-Identifier: GPL-3.0-or-later
// Owner item 3 (2026-10-18, as corrected by the owner): while health is low a "Heal" page joins
// the Ready right-pane cycle (auto-shown once per low episode; shows the low-health state and the
// heart icon). Tapping that page opens a helper card INSIDE the Ready circle minimap (shared card
// frame, card 3) listing at most 4 pouch foods that best refill the missing hearts; a second tap
// of the page or recovering health closes it.
// Display + ranking only: eating goes through the existing guarded native consume request
// (SelectionBridge::request_consume, the Inventory Eat path), never a direct write.
#pragma once
#include "botw_consumables.h"
#include "botw_player_equipment.h"
#include "botw_ready_panes.h"
#include "botw_key.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>
namespace botw {
// All health values are native quarter hearts ("life" units; 4 = one heart).
//  life      Player+0x848 int32      native getLife (vtable+0x2B8, main+D50), contracts/CONSUME-ROUTE-RESEARCH.
//  max_life  Player+0x1868 int32     native getMaxLife (vtable+0xF0, main+F33680). The food update
//                                    at main+F7E0D8..F7E0E0 stores int(PlayerInfo+0xA0) + the
//                                    LifeMaxUp level here, i.e. it INCLUDES temporary (yellow) hearts.
//  base_max  PlayerInfo+0xA0 float   red-heart containers (main+F7E124..F7E12C stores exactly
//                                    int(PlayerInfo+0xA0) there when the bonus ends).
// Normal food never refills yellow hearts, so "missing" is measured against the red maximum.
struct PlayerHealth {
  bool valid{};
  int32_t life{},max_life{},base_max{};
  int32_t missing() const {return valid && life<base_max ? base_max-life : 0;}
  bool operator==(const PlayerHealth&) const = default;
};
inline PlayerHealth player_health(const EdenDsmodHostApi& h,uint64_t info,uint64_t player) {
  if(!ready_supported(h) || info<0x10000 || info%8 || info>UINT64_MAX-0xA8 ||
     player<0x10000 || player%8 || player>UINT64_MAX-0x2450)return {};
  auto sample=[&](PlayerHealth& s) {
    float base{};
    if(!read(h,player+0x848,s.life) || !read(h,player+0x1868,s.max_life) || !read(h,info+0xA0,base) ||
       !std::isfinite(base) || base<4.f || base>120.f || std::trunc(base)!=base)return false;
    s.base_max=int32_t(base);
    // Containers come in whole hearts; temporary hearts add at most 30 hearts on top.
    return s.base_max%4==0 && s.max_life>=s.base_max && s.max_life<=s.base_max+120 &&
      s.life>=0 && s.life<=s.max_life;
  };
  PlayerHealth a{},b{};
  if(!sample(a) || !sample(b) || a!=b)return {};
  a.valid=true;return a;
}
// Low health, in quarter hearts against the red maximum `base`:
//  on  = max(25% of max (rounded up), min(3 hearts, half of max))
//  off = min(max, on + 2 hearts)
// 25% is the proportional rule for big heart pools (13+ containers); the 3-heart floor matters
// in the early/mid game, where one ordinary hit takes 1-3 hearts; "half of max" keeps a 3-6
// heart Link from being "low" after a single small hit (3 hearts -> low at 1.5, 4 -> at 2).
// The 2-heart hysteresis band (capped at full) means a half-heart snack or a quarter-heart
// regen tick cannot toggle the page.
inline int32_t low_health_on(int32_t base) {return std::max((base+3)/4,std::min(12,base/2));}
inline int32_t low_health_off(int32_t base) {return std::min(base,low_health_on(base)+8);}
class LowHealth {
 public:
  using Clock=std::chrono::steady_clock;
  // Feed every 100 ms reader pass (valid or not). Invalid reads (menus, loading, a torn read)
  // keep the latched state; only a valid recovered reading or a session change ends an episode.
  void observe(const PlayerHealth& p,uint32_t epoch,Clock::time_point now) {
    if(epoch!=epoch_){*this={};epoch_=epoch;}
    if(!p.valid)return;
    health_=p;seen_=now;
    const bool dead=p.life<=0,high=p.life>=low_health_off(p.base_max),
      under=!dead && p.life<=low_health_on(p.base_max);
    if(!low_) {
      if(under){if(!pending_on_)pending_on_=now;if(now-*pending_on_>=OnDelay){low_=true;rising_=true;pending_on_.reset();}}
      else pending_on_.reset();
    } else {
      if(high || dead){if(!pending_off_)pending_off_=now;if(now-*pending_off_>=OffDelay){low_=false;rising_=false;pending_off_.reset();}}
      else pending_off_.reset();
    }
  }
  // The page is offered while the episode is latched and a valid reading is recent.
  bool shown(Clock::time_point now) const {
    return low_ && seen_ && now-*seen_<=Stale && health_.life>0;
  }
  bool low() const {return low_;}
  // True once per episode, at the moment the page first becomes available.
  bool take_rising() {const bool r=rising_;rising_=false;return r;}
  const PlayerHealth& health() const {return health_;}
  static constexpr auto OnDelay=std::chrono::milliseconds(300);
  static constexpr auto OffDelay=std::chrono::milliseconds(1000);
  static constexpr auto Stale=std::chrono::milliseconds(2000);
 private:
  PlayerHealth health_{};
  bool low_{},rising_{};
  uint32_t epoch_{};
  std::optional<Clock::time_point> seen_,pending_on_,pending_off_;
};
// One overlay row: a pouch food (stack) and how well it covers the missing hearts.
struct HealOption {
  uint64_t address{};            // the pouch entry Eat targets (first entry of a merged group)
  std::string actor;
  std::array<uint32_t,5> cook{}; // cooked-meal identity (zero for raw)
  int32_t heal{};                // quarter hearts restored (0 for full recovery)
  bool full{};                   // LifeMaxUp (hearty): full recovery + `bonus` temporary quarters
  int32_t bonus{};
  int32_t effect{-1};            // cook::CureEffectId of the dish's OTHER effect (-1/1/2 = none to
                                  // show separately; H-2: previously dropped on the floor so a
                                  // dish's real bonus effect (e.g. Attack Up) never reached the row)
  uint32_t count{};
  int32_t rank{},waste{};        // filled by rank_heal_options
};
// cook::CureEffectId (fixtures/botw-decomp/src/Game/Cooking/cookManager.h, dsmod-re-analyst
// RE-confirmed 2026-10-18 against the public decomp's PouchItem::CookData struct and
// uiPauseMenuDataMgr.cpp's setCookDataOnLastAddedItem, which stores the SAME effect id onto both
// cooked meals and elixirs). Plain health food (-1/1) and Hearty (2, shown via `full`/`bonus`
// instead) return nullptr (no effect text). The RE pass found no universal per-effect badge icon
// in the native Pouch UI for ordinary cooked dishes (only the Monster Extract elixir item,
// Item_Cook_C_17, carries per-effect icon variants) -- so the card shows this text label rather
// than inventing an icon the native game doesn't have for this item class.
// i8 cardfix round 2 (critic C8-3, LIVE-confirmed): the first pass used the dish-NAME adjective
// ("Mighty", "Chilly", ...), which 1) duplicates the row name's own first word (dishes are named
// "Mighty <X>") and 2) does not match what the native game actually shows the player -- eating a
// real Mighty dish this round made the native top-screen HUD show "Attack Up", not "Mighty". This
// now uses that same buff-HUD vocabulary (the cook::CureEffectId enum's own name, humanized),
// confirmed against the live capture for id 10/Attack Up; the rest are the same naming pattern,
// not yet independently live-confirmed one by one -- flag if any reads oddly in a future capture.
inline const char* heal_effect_label(int32_t effect) {
  switch(effect) {
    case 4:return "Heat Resistance";case 5:return "Cold Resistance";case 6:return "Shock Resistance";
    case 10:return "Attack Up";case 11:return "Defense Up";case 12:return "Stealth Up";
    case 13:return "Speed Up";case 14:return "Stamina Recovery";case 15:return "Stamina Vessel Up";
    case 16:return "Fireproof";default:return nullptr;
  }
}
// Owner item 3 (iteration 10, 2026-10-20 redesign): the food card's effect row gets a small
// icon alongside its text. No per-effect badge exists natively (see the comment above), so this
// reuses the SAME 4-icon family the Ready page's own active-buffs panel already shows for this
// exact concept (botw_ready_panes.h's effect.N.kind -> Nt_PauseIconGoGo_00/Attack_00/Hot_00/
// L_04, tools/*.py's existing src_names/tint_colors list) -- not a new/invented icon, the
// companion's own established "temporary status effect" iconography, bucketed by family:
// 0=GoGo (speed/stamina-ish), 1=Attack, 2=Hot (heat/fire), 3=Generic (everything else).
inline int32_t heal_effect_icon_kind(int32_t effect) {
  switch(effect) {
    case 13:case 14:case 15:return 0;        // Speed Up / Stamina Recovery / Stamina Vessel Up
    case 10:return 1;                        // Attack Up
    case 4:case 16:return 2;                 // Heat Resistance / Fireproof
    case 5:case 6:case 11:case 12:return 3;  // Cold/Shock Resistance, Defense Up, Stealth Up
    default:return 3;
  }
}
// Heart-distance band of `heal` against `missing`: exact first, then +1 heart, -1, +2, -2, ...
// A band is one heart wide: 1-4 quarters over is +1, 1-4 quarters under is -1, and so on.
// Returns the sort rank (0 exact, 1 = +1, 2 = -1, 3 = +2, 4 = -2, ...).
inline int32_t heal_band_rank(int32_t heal,int32_t missing) {
  const int32_t d=heal-missing;if(d==0)return 0;
  const int32_t k=(std::abs(d)+3)/4;
  return d>0 ? 2*k-1 : 2*k;
}
// Owner ordering: band rank; then fewest quarter hearts wasted (over-heal for + bands,
// shortfall for - bands, i.e. |heal-missing|); then the item the player has most of.
// Full-recovery (hearty) items come after every regular item, least bonus first: they always
// refill exactly, but spending one on ordinary healing throws away its extra hearts, so they
// only fill rows the regular food leaves free (e.g. when nothing else heals).
inline std::vector<HealOption> rank_heal_options(std::vector<HealOption> options,int32_t missing,size_t limit=5) {
  for(auto& o:options) {
    if(o.full){o.rank=INT32_MAX;o.waste=o.bonus;}
    else {o.rank=heal_band_rank(o.heal,missing);o.waste=std::abs(o.heal-missing);}
  }
  std::stable_sort(options.begin(),options.end(),[](const HealOption& a,const HealOption& b){
    if(a.rank!=b.rank)return a.rank<b.rank;
    if(a.waste!=b.waste)return a.waste<b.waste;
    if(a.count!=b.count)return a.count>b.count;
    if(a.actor!=b.actor)return a.actor<b.actor;
    return a.address<b.address;
  });
  if(options.size()>limit)options.resize(limit);
  return options;
}
// Heal amounts come from the same guarded consumable() reader the Inventory Eat slot uses:
// raw = ActorInfo cureItemHitPointRecover (botw_consume_metadata.h, generated from 1.6.0
// ActorInfo); cooked = pouch cook data +0x80 health / +0x8C effect / +0x90 level.
inline HealOption heal_option(const Item& item,const Consumable& food) {
  HealOption o;o.address=item.address;o.actor=item.actor;o.count=uint32_t(item.value);
  if(food.recipe)o.cook=food.cook;
  if(food.effect==2){o.full=true;o.bonus=food.level;}
  else {o.heal=food.health;if(heal_effect_label(food.effect))o.effect=food.effect;}
  return o;
}
inline bool heal_eligible(const Consumable& food) {return food.valid && (food.effect==2 || food.health>0);}
// Identical stacks (same actor + cooked data) merge into one row; Eat targets the first entry.
inline void merge_heal_option(std::vector<HealOption>& out,HealOption o) {
  for(auto& e:out)if(e.actor==o.actor && e.cook==o.cook && e.full==o.full && e.heal==o.heal && e.bonus==o.bonus && e.effect==o.effect) {
    e.count+=o.count;return;
  }
  out.push_back(std::move(o));
}
inline std::vector<HealOption> heal_candidates(const EdenDsmodHostApi& h,const Snapshot& pouch) {
  std::vector<HealOption> out;if(!pouch.valid)return out;
  for(const auto& item:pouch.items) {
    if((item.type!=7 && item.type!=8) || item.equipped || item.value<1)continue;
    const auto food=consumable(h,item);
    if(heal_eligible(food))merge_heal_option(out,heal_option(item,food));
  }
  return out;
}
// "4", "4½", "¾" (UTF-8; the game font carries ¼ ½ ¾).
inline std::string hearts_text(int32_t quarters) {
  if(quarters<0)quarters=0;
  static constexpr const char* fraction[]={"","\xC2\xBC","\xC2\xBD","\xC2\xBE"};
  std::string s=quarters>=4 || quarters==0 ? std::to_string(quarters/4) : std::string();
  return s+fraction[quarters%4];
}
// The Ready right-pane page cycle with the conditional Heal page (index 2) after Weather (0) and
// Buffs (1). The wrapped ReadyPanes keeps its own normal/drawn pages; this only overlays "Heal is
// the shown page", so hiding Heal returns to whatever page was underneath.
struct HealPage {
  bool available{},showing{};
  int right(const ReadyPanes& panes) const {return available && showing ? 2 : panes.right();}
  int count() const {return available ? 3 : 2;}
  // `episode`: the low-health latch is still set. A stale reading (pause menu, loading) hides the
  // page but keeps "Heal was the shown page", so it comes back as it was; only the end of the
  // episode forgets it.
  void set_available(bool now_available,bool rising,bool episode=false) {
    available=now_available;
    if(!available && !episode)showing=false;
    else if(available && rising)showing=true;  // once per low-health episode; never re-steals after a swipe
  }
  bool step(ReadyPanes& panes,int direction) {
    if(direction!=1 && direction!=-1)return false;
    if(!available)return panes.step(true,direction);
    auto& page=panes.drawn ? panes.drawn_right : panes.normal_right;
    if(showing){showing=false;page=direction>0 ? 0 : 1;return true;}
    if((direction>0 && page==1) || (direction<0 && page==0)){showing=true;return true;}
    return panes.step(true,direction);
  }
};
// Page + food-card content. The card itself (open/close, dim, one card at a time) belongs to the
// shared card frame (botw_cards.h: botw.card.active, card_toggle 3). This
// publishes the page, the card's 4 rows and botw.card.food.close_request (health recovered).
// Rows are kept current while the page is offered, so the card shows fresh data the moment it
// opens; a row's Eat resolves only against this published list, re-checked against the fresh pouch.
struct HealAdvisor {
  // i9 item 5: 4 rows, one per circular-card quadrant.
  static constexpr size_t Rows=4;
  static constexpr int64_t CardId=3;
  LowHealth low;
  HealPage page;
  std::vector<HealOption> rows;
  // Once per sample: the page follows the latch (auto-shown once on the rising edge).
  void update(LowHealth::Clock::time_point now) {
    page.set_available(low.shown(now),low.take_rising(),low.low());
    if(!low.low())rows.clear();
  }
  bool close_request() const {return !low.low();}
  // Recompute rows from the current pouch (100 ms reader pass, while low).
  void refresh(const EdenDsmodHostApi& h,const Snapshot& pouch) {
    const int32_t missing=low.health().missing();
    rows=missing>0 ? rank_heal_options(heal_candidates(h,pouch),missing,Rows) : std::vector<HealOption>{};
  }
  const HealOption* row(int64_t index) const {
    return page.available && index>=0 && uint64_t(index)<rows.size() ? &rows[size_t(index)] : nullptr;
  }
  // Fresh identity check before a native request: same pouch entry, actor, cooked data, edible.
  static bool still_matches(const EdenDsmodHostApi& h,const Snapshot& pouch,const HealOption& o) {
    if(!pouch.valid)return false;
    for(const auto& item:pouch.items)if(item.address==o.address) {
      if(item.actor!=o.actor || item.equipped || item.value<1)return false;
      const auto food=consumable(h,item);
      if(!heal_eligible(food))return false;
      const auto again=heal_option(item,food);
      return again.cook==o.cook && again.full==o.full && again.heal==o.heal && again.bonus==o.bonus && again.effect==o.effect;
    }
    return false;
  }
  void publish(const EdenDsmodHostApi& h,bool busy) const {
    const auto& p=low.health();const int32_t missing=p.missing();
    h.publish_i64(h.userdata,"botw.ready.heal.available",page.available);
    h.publish_i64(h.userdata,"botw.ready.right.heal",page.available && page.showing);
    h.publish_i64(h.userdata,"botw.ready.right.dots",page.count());
    h.publish_i64(h.userdata,"botw.card.food.close_request",close_request());
    h.publish_i64(h.userdata,"botw.ready.heal.valid",p.valid);
    h.publish_i64(h.userdata,"botw.ready.heal.life",p.valid ? p.life : -1);
    h.publish_i64(h.userdata,"botw.ready.heal.max",p.valid ? p.base_max : -1);
    h.publish_i64(h.userdata,"botw.ready.heal.missing",missing);
    const std::string life=p.valid ? hearts_text(p.life) : std::string("-");
    // i9 item 10: never show the "/<max>" max-hearts text -- only the current/needed values.
    h.publish_text(h.userdata,"botw.ready.heal.hearts_text",life.c_str());
    h.publish_text(h.userdata,"botw.ready.heal.need_text",(missing>0 ? "Need "+hearts_text(missing) : std::string("Full")).c_str());
    h.publish_text(h.userdata,"botw.ready.heal.need_amount_text",(missing>0 ? hearts_text(missing) : std::string("Full")).c_str());
    h.publish_text(h.userdata,"botw.ready.heal.summary",(p.valid ? life+" hearts"+
      (missing>0 ? std::string(" \xC2\xB7 need ")+hearts_text(missing) : std::string()) : std::string()).c_str());
    h.publish_i64(h.userdata,"botw.ready.heal.busy",busy);
    const bool shown=page.available;
    h.publish_i64(h.userdata,"botw.ready.heal.count",shown ? int64_t(rows.size()) : 0);
    h.publish_i64(h.userdata,"botw.ready.heal.empty",shown && rows.empty() && missing>0);
    for(unsigned i=0;i<Rows;++i) {
      Key key("botw.ready.heal.row.",i);
      const HealOption* o=shown && i<rows.size() ? &rows[i] : nullptr;
      h.publish_i64(h.userdata,key(".valid"),o!=nullptr);
      h.publish_text(h.userdata,key(".actor"),o ? o->actor.c_str() : "");
      h.publish_i64(h.userdata,key(".name"),o ? actor_key(o->actor) : -1);
      h.publish_i64(h.userdata,key(".full"),o && o->full);
      h.publish_i64(h.userdata,key(".exact"),o && !o->full && o->rank==0);
      h.publish_text(h.userdata,key(".heal_text"),o ? (o->full ? std::string("Full") : "+"+hearts_text(o->heal)).c_str() : "");
      h.publish_text(h.userdata,key(".bonus_text"),o && o->full && o->bonus>0 ? ("+"+hearts_text(o->bonus)+" extra").c_str() : "");
      const char* elabel=o ? heal_effect_label(o->effect) : nullptr;
      h.publish_i64(h.userdata,key(".has_effect"),elabel!=nullptr);
      h.publish_text(h.userdata,key(".effect_text"),elabel?elabel:"");
      h.publish_i64(h.userdata,key(".effect_icon_kind"),o?heal_effect_icon_kind(o->effect):0);
      h.publish_i64(h.userdata,key(".count"),o ? int64_t(o->count) : 0);
      h.publish_text(h.userdata,key(".count_text"),o ? ("\xC3\x97"+std::to_string(o->count)).c_str() : "");
    }
  }
};
} // namespace botw
