// SPDX-License-Identifier: GPL-3.0-or-later
// BOTW 1.6.0 temperature danger + clothing recommendation card (RE 2026-10-18, NOTES.md in
// internal notes). Every threshold below is the game's own logic:
//  * Player calc (main+0xF3FC88..0xF3FCD8) writes UIManager+0x6C = felt temperature T (physics
//    component +0x170/+0x174) and UIManager+0x68 = T - limit, limit = Player+0x2084 (heat, T>0)
//    or Player+0x2088 (cold, T<=0); UIManager+0x74 = 1 (valid).
//  * HUD (main+0x11895A0..0x118A254): heat warning (Pa_Hot_00) when T>0 && T-limit>0, cold warning
//    (Pa_Cold_00) when T<=0 && T-limit<0, none while max(UIManager+0x78,+0x7C) > 0 (fire zone).
//  * Limits: Player+0x2084 = StatusEffectList ResistHot[min(armor+food,2)] = 40/50/9999,
//    Player+0x2088 = ResistCold[min(armor+food,2)] = 0/-10/-9999 (main+0xF4ED54..0xF4EDB0; the
//    lookup main+0x16A6E40 clamps the index to the last value).
//  * Armor levels Player+0x2028 heat / +0x202C cold / +0x2030 flame guard = sum of the worn
//    pieces' armorEffect.EffectLevel (main+0x1693A00/0x1693E30/0x1693C20, combos count through
//    main+0x168F270); food levels are effect slots 7/8/9 (Player+0x200C/+0x2010/+0x2014), timers
//    Player+0x1ED8+12*slot (frames at 30 fps).
//  * Fire: UIManager+0x78 = area fire level (physics +0x90 byte 0x17), +0x7C = contact fire level
//    (byte 0x16) (main+0xF3B730); main+0x15C96BC burns when the level > physics+0x180 =
//    min(armor+food flame guard,2) (3 with the flame-guard set bonus).
#pragma once
#include "botw_context.h"
#include "botw_ready_offsets.h"
#include "botw_temperature_metadata.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>
namespace botw {
inline constexpr std::array<float,3> native_heat_limits{40.f,50.f,9999.f};
inline constexpr std::array<float,3> native_cold_limits{0.f,-10.f,-9999.f};
enum class TempDanger:int {None=0,Heat=1,Cold=2,Burn=3};
// Cook effect ids (ActorInfo cureItemEffectType) and armor kinds per danger.
inline int temp_food_effect(TempDanger k){return k==TempDanger::Heat?4:k==TempDanger::Cold?5:k==TempDanger::Burn?16:-1;}
inline int temp_armor_kind(TempDanger k){return k==TempDanger::Heat?1:k==TempDanger::Cold?2:k==TempDanger::Burn?3:0;}

struct TemperatureSample {
  bool valid{};
  float felt{},margin{};            // UIManager+0x6C / +0x68
  int area_fire{},contact_fire{};   // UIManager+0x78 / +0x7C
  int armor_heat{},armor_cold{},armor_burn{};
  int food_heat{},food_cold{},food_burn{};
  int food_heat_s{},food_cold_s{},food_burn_s{};
  float heat_limit{},cold_limit{};  // Player+0x2084 / +0x2088
  int burn_resist{-1};              // physics component +0x180 (-1 unreadable)
  int hud_state{2};                 // HUD meter +0x1D8 applied warning: 0 heat, 1 cold, 2 none
  bool operator==(const TemperatureSample&) const = default;
};

inline int level_index(int level){return std::clamp(level,0,2);}
// Smallest total resistance level whose native limit keeps the felt temperature safe.
inline int heat_level_needed(float felt){for(int l=0;l<3;++l)if(felt<=native_heat_limits[size_t(l)])return l;return 2;}
inline int cold_level_needed(float felt){for(int l=0;l<3;++l)if(felt>=native_cold_limits[size_t(l)])return l;return 2;}

// The game's own decision, plus a margin bias used only by the latch (bias>0 keeps a latched
// danger until the temperature is `bias` degrees back inside the limit).
inline TempDanger raw_danger(const TemperatureSample& s,TempDanger latched=TempDanger::None,float bias=0.f) {
  if(!s.valid)return TempDanger::None;
  if(s.area_fire>0 || s.contact_fire>0) {
    // Fire zone: the HUD hides heat/cold warnings. Burning when the area level beats the
    // effective flame guard (native compare at main+0x15C96D0).
    const int resist=s.burn_resist>=0 ? s.burn_resist : std::min(s.armor_burn+s.food_burn,2);
    return s.area_fire>resist ? TempDanger::Burn : TempDanger::None;
  }
  const float heat_bias=latched==TempDanger::Heat?bias:0.f,cold_bias=latched==TempDanger::Cold?bias:0.f;
  if(s.felt>0.f && s.margin>-heat_bias)return TempDanger::Heat;
  if(s.felt<=0.f && s.margin<cold_bias)return TempDanger::Cold;
  return TempDanger::None;
}
inline int needed_level(const TemperatureSample& s,TempDanger k) {
  switch(k){
    case TempDanger::Heat:return std::max(1,heat_level_needed(s.felt));
    case TempDanger::Cold:return std::max(1,cold_level_needed(s.felt));
    case TempDanger::Burn:return std::clamp(s.area_fire,1,2);
    default:return 0;
  }
}
inline int current_level(const TemperatureSample& s,TempDanger k) {
  switch(k){
    case TempDanger::Heat:return s.armor_heat+s.food_heat;
    case TempDanger::Cold:return s.armor_cold+s.food_cold;
    case TempDanger::Burn:return s.burn_resist>=0 ? s.burn_resist : std::min(s.armor_burn+s.food_burn,2);
    default:return 0;
  }
}
inline int active_food_level(const TemperatureSample& s,TempDanger k){return k==TempDanger::Heat?s.food_heat:k==TempDanger::Cold?s.food_cold:k==TempDanger::Burn?s.food_burn:0;}

// No flicker: a new danger is shown after it held for enter_ms, cleared after leave_ms, and a
// shown heat/cold danger stays until the temperature is 0.5 C back inside the native limit.
class DangerLatch {
  TempDanger shown_{},pending_{};int64_t since_{-1};
 public:
  static constexpr int64_t enter_ms=400,leave_ms=900;static constexpr float bias=0.5f;
  TempDanger update(const TemperatureSample& s,int64_t now_ms) {
    const auto raw=s.valid ? raw_danger(s,shown_,bias) : TempDanger::None;
    if(raw==shown_){pending_=raw;since_=-1;return shown_;}
    if(raw!=pending_ || since_<0){pending_=raw;since_=now_ms;}
    const int64_t wait=raw==TempDanger::None ? leave_ms : enter_ms;
    if(now_ms-since_>=wait){shown_=raw;since_=-1;}
    return shown_;
  }
  TempDanger shown() const {return shown_;}
  void clear(){*this={};}
};

// ---------------- recommendation logic (pure; host-tested) ----------------
struct ArmorOption {uint64_t address{};std::string actor;int type{};bool equipped{};int level{};int star{};std::string series;bool set_bonus{};};
struct FoodOption {uint64_t address{};std::string actor;int level{};int seconds{};};
struct CardRow {
  bool valid{},reaches{},worn{},set_bonus{};
  std::vector<uint64_t> equip;      // pieces to put on, head/upper/lower order
  std::vector<std::string> icons;   // actors shown (the pieces of the row, or the food)
  int level{},pieces{},seconds{};uint64_t food{};std::string food_actor;
};
struct ClothingPlan {bool valid{};TempDanger kind{};int need{},current{};CardRow quick,best,food;bool empty{};};

// Armor in the pouch with its level for this danger kind (0 = no help), one row per instance.
template<class Items> std::vector<ArmorOption> armor_options(const Items& items,TempDanger k) {
  std::vector<ArmorOption> out;const int kind=temp_armor_kind(k);
  for(const auto& i:items) {
    if(i.type<4 || i.type>6)continue;
    const auto* info=armor_temperature_info(i.actor);
    ArmorOption o{i.address,i.actor,i.type,i.equipped,0,0,"",false};
    if(info && info->type==i.type){o.star=info->star;o.series=std::string(info->series);o.set_bonus=info->set_bonus;if(info->kind==kind)o.level=info->level;}
    out.push_back(std::move(o));
  }
  return out;
}
namespace detail {
// Deterministic preference between two pieces: higher level, already worn, upgraded, then name/address.
inline bool better_piece(const ArmorOption& a,const ArmorOption& b) {
  if(a.level!=b.level)return a.level>b.level;
  if(a.equipped!=b.equipped)return a.equipped;
  if(a.star!=b.star)return a.star>b.star;
  if(a.actor!=b.actor)return a.actor<b.actor;
  return a.address<b.address;
}
inline const ArmorOption* worn(const std::vector<ArmorOption>& o,int type){for(const auto& a:o)if(a.type==type&&a.equipped)return &a;return nullptr;}
}
inline ClothingPlan plan_clothing(TempDanger kind,int need,int current,int food_level,
                                  const std::vector<ArmorOption>& armor,const std::vector<FoodOption>& food) {
  ClothingPlan p;p.kind=kind;p.need=need;p.current=current;
  if(kind==TempDanger::None || need<1)return p;
  p.valid=true;
  const auto total=[&](int armor_sum){return kind==TempDanger::Burn ? std::min(armor_sum+food_level,2) : armor_sum+food_level;};
  int worn_sum=0;std::array<const ArmorOption*,3> worn_piece{};
  for(int t=4;t<=6;++t){worn_piece[size_t(t-4)]=detail::worn(armor,t);if(worn_piece[size_t(t-4)])worn_sum+=worn_piece[size_t(t-4)]->level;}
  std::array<const ArmorOption*,3> best{};
  for(const auto& a:armor)if(a.level>0){auto& b=best[size_t(a.type-4)];if(!b || detail::better_piece(a,*b))b=&a;}
  // Row 1, quickest fix: fewest swaps. Slots are independent, so taking the largest gains first
  // is optimal; equal gains go head, upper, lower. Stops as soon as the need is met.
  {
    std::array<int,3> gain{};
    for(size_t s=0;s<3;++s)gain[s]=best[s] ? best[s]->level-(worn_piece[s]?worn_piece[s]->level:0) : 0;
    std::array<int,3> order{0,1,2};
    std::stable_sort(order.begin(),order.end(),[&](int a,int b){return gain[size_t(a)]>gain[size_t(b)];});
    int sum=worn_sum;std::vector<int> picked;
    for(int s:order){if(total(sum)>=need || gain[size_t(s)]<=0)break;sum+=gain[size_t(s)];picked.push_back(s);}
    std::sort(picked.begin(),picked.end());
    if(!picked.empty()) {
      auto& r=p.quick;r.valid=true;r.level=total(sum);r.reaches=r.level>=need;r.pieces=int(picked.size());
      for(int s:picked){r.equip.push_back(best[size_t(s)]->address);r.icons.push_back(best[size_t(s)]->actor);}
    }
  }
  // Row 2, best owned outfit: highest level, then a complete set-bonus outfit, then upgrades,
  // then pieces already worn. Candidates per slot: every piece with the slot's best level.
  {
    std::array<std::vector<const ArmorOption*>,3> cand;
    for(size_t s=0;s<3;++s) {
      const int top=best[s] ? best[s]->level : 0;
      for(const auto& a:armor)if(size_t(a.type-4)==s && top>0 && a.level==top)cand[s].push_back(&a);
      std::sort(cand[s].begin(),cand[s].end(),[](const ArmorOption* a,const ArmorOption* b){return detail::better_piece(*a,*b);});
      if(cand[s].size()>16)cand[s].resize(16);
      if(cand[s].empty())cand[s].push_back(nullptr); // nothing helps here: keep the slot as it is
    }
    struct Score {int level,set,star,keep;bool operator>(const Score& o)const{return std::tie(level,set,star,keep)>std::tie(o.level,o.set,o.star,o.keep);}};
    Score top{-1,0,0,0};std::array<const ArmorOption*,3> pick{};
    for(auto* h:cand[0])for(auto* u:cand[1])for(auto* l:cand[2]) {
      std::array<const ArmorOption*,3> o{h,u,l};int sum=0,star=0,keep=0;
      for(size_t s=0;s<3;++s){if(o[s]){sum+=o[s]->level;star+=o[s]->star;keep+=o[s]->equipped;}else if(worn_piece[s])sum+=worn_piece[s]->level;}
      const bool set=h&&u&&l&&h->set_bonus&&u->set_bonus&&l->set_bonus&&h->series==u->series&&u->series==l->series;
      const Score sc{total(sum),set,star,keep};
      if(sc>top){top=sc;pick=o;}
    }
    bool any=false;for(auto* a:pick)if(a)any=true;
    if(any) {
      auto& r=p.best;r.valid=true;r.level=top.level;r.reaches=r.level>=need;r.set_bonus=top.set!=0;
      for(auto* a:pick)if(a){r.icons.push_back(a->actor);if(!a->equipped)r.equip.push_back(a->address);}
      r.pieces=int(r.equip.size());r.worn=r.equip.empty();
    }
  }
  // Row 3, best consumable: reaches the need together with the armor worn now (native food levels
  // add to armor; a new meal replaces the active one), then longest duration, then lowest level.
  {
    const FoodOption* pick=nullptr;
    auto reaches=[&](const FoodOption& f){return (kind==TempDanger::Burn ? std::min(worn_sum+f.level,2) : worn_sum+f.level)>=need;};
    for(const auto& f:food) {
      if(f.level<1)continue;
      if(!pick){pick=&f;continue;}
      const bool fr=reaches(f),pr=reaches(*pick);
      if(fr!=pr){if(fr)pick=&f;continue;}
      if(!fr && f.level!=pick->level){if(f.level>pick->level)pick=&f;continue;}
      if(f.seconds!=pick->seconds){if(f.seconds>pick->seconds)pick=&f;continue;}
      if(f.level!=pick->level){if(f.level<pick->level)pick=&f;continue;}
      if(f.actor!=pick->actor){if(f.actor<pick->actor)pick=&f;continue;}
      if(f.address<pick->address)pick=&f;
    }
    if(pick) {
      auto& r=p.food;r.valid=true;r.food=pick->address;r.food_actor=pick->actor;r.icons={pick->actor};
      r.level=pick->level;r.seconds=pick->seconds;r.reaches=reaches(*pick);
    }
  }
  p.empty=!p.quick.valid && !p.best.valid;
  return p;
}

// ---------------- live reader ----------------
// `player` must be the validated Player (player_equipment()). World only.
inline TemperatureSample temperature_sample(const EdenDsmodHostApi& h,uint64_t player) {
  UiSample ui{},after{};
  if(!ready_supported(h) || !ui_sample(h,ui) || ui.flags!=0x1E09000000ULL || !player || player%8 || player>UINT64_MAX-0x2700)return {};
  struct Raw {
    std::array<uint8_t,0x18> uim{};std::array<uint32_t,3> food{},armor{};std::array<float,2> limits{};
    std::array<float,9> timers{};uint64_t hud{},hud_vt{},meter{},meter_vt{};uint32_t hud_state{2};
    uint64_t physics{},bodies{},comp_vt{};int32_t count{};uint8_t resist{};bool comp_ok{};
    bool operator==(const Raw&) const = default;
  } a{},z{};
  auto get=[&](Raw& r) {
    if(ui.manager>UINT64_MAX-0x80 || !read(h,ui.manager+0x68,r.uim) || r.uim[0xC]!=1 ||
       !read(h,player+0x200C,r.food) || !read(h,player+0x2028,r.armor) ||
       !read(h,player+0x2084,r.limits) || !read(h,player+ready_offsets(h)->effect_timers+7*12,r.timers))return false;
    r.hud_state=2;
    if(!(ui.array<=UINT64_MAX-37*8 && read(h,ui.array+37*8,r.hud) && r.hud && r.hud%8==0 && r.hud<=UINT64_MAX-(0x36E0+ready_offsets(h)->pane_shift) &&
         read(h,r.hud,r.hud_vt) && r.hud_vt==h.main_base+ready_offsets(h)->hud_vtable && read(h,r.hud+(0x36D8+ready_offsets(h)->pane_shift),r.meter) && r.meter && r.meter%8==0 &&
         r.meter<=UINT64_MAX-0x1E0 && read(h,r.meter,r.meter_vt) && r.meter_vt==h.main_base+ready_offsets(h)->temperature_pane_vtable && read(h,r.meter+0x1D8,r.hud_state)))
      r.hud_state=2;
    // Player physics component (Player vtable+0x308 = main+0x1C14300): *(Player+0x6A8)+0x60 element 0, +0x40.
    r.comp_ok=read(h,player+0x6A8,r.physics) && r.physics && r.physics%8==0 && r.physics<=UINT64_MAX-0x88 &&
      read(h,r.physics+0x58,r.count) && r.count>0 && r.count<64 && read(h,r.physics+0x60,r.bodies) && r.bodies%8==0 &&
      r.bodies>=0x10000 && r.bodies<=UINT64_MAX-0x200 && read(h,r.bodies+0x40,r.comp_vt) && r.comp_vt==h.main_base+ready_offsets(h)->physics_body_vtable &&
      read(h,r.bodies+0x40+0x180,r.resist) && r.resist<=3;
    return true;
  };
  if(!get(a) || !get(z) || !(a==z) || !ui_sample(h,after) || !(after==ui))return {};
  TemperatureSample s;
  auto f32=[](const uint8_t* p){float v;std::memcpy(&v,p,4);return v;};
  auto u32=[](const uint8_t* p){uint32_t v;std::memcpy(&v,p,4);return v;};
  s.margin=f32(a.uim.data());s.felt=f32(a.uim.data()+4);
  const uint32_t fa=u32(a.uim.data()+0x10),fc=u32(a.uim.data()+0x14);
  s.heat_limit=a.limits[0];s.cold_limit=a.limits[1];
  if(!std::isfinite(s.felt) || !std::isfinite(s.margin) || std::fabs(s.felt)>500.f || fa>3 || fc>3)return {};
  for(auto v:a.armor)if(v>9)return {};
  for(auto v:a.food)if(v>16)return {};
  // Native consistency guard: the stored limits equal the StatusEffectList value at the native
  // index (armor + food level, clamped), and the margin is T minus the limit on T's side of zero.
  if(s.heat_limit!=native_heat_limits[size_t(level_index(int(a.armor[0]+a.food[0])))] ||
     s.cold_limit!=native_cold_limits[size_t(level_index(int(a.armor[1]+a.food[1])))])return {};
  const float expect=s.felt>0.f ? s.felt-s.heat_limit : s.felt-s.cold_limit;
  if(std::fabs(expect-s.margin)>0.05f)return {};
  s.armor_heat=int(a.armor[0]);s.armor_cold=int(a.armor[1]);s.armor_burn=int(a.armor[2]);
  auto secs=[&](unsigned i){const float f=a.timers[i*3];return std::isfinite(f)&&f>0&&f<30.f*86400 ? int(f/30.f+0.999f) : 0;};
  s.food_heat=int(a.food[0]);s.food_cold=int(a.food[1]);s.food_burn=int(a.food[2]);
  s.food_heat_s=secs(0);s.food_cold_s=secs(1);s.food_burn_s=secs(2);
  s.area_fire=int(fa);s.contact_fire=int(fc);s.burn_resist=a.comp_ok ? int(a.resist) : -1;
  s.hud_state=a.hud_state<=2 ? int(a.hud_state) : 2;
  s.valid=true;return s;
}
} // namespace botw
