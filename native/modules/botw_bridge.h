// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_gear.h"
#include "botw_runes.h"
#include "botw_world_runes.h"
#include "botw_quests.h"
#include "botw_inventory_actions.h"
#include "botw_consume_actions.h"
#include "botw_hold_actions.h"
#include "botw_map_actions.h"
#include "botw_warp.h"
#include "botw_tracker_flags.h"
#include "core/mods/dsmod_module_extensions.h"
#include <chrono>
namespace botw {
struct SelectionBridge {
  using Clock=std::chrono::steady_clock;
  EdenDsmodHostExtensions ext{};
  EdenDsmodHostApi host{};
  Clock::time_point sampled{},heartbeat_at{},requested{};
  // Fix round A (2026-10-18): measured spacing of the guest helper heartbeat (one beat per game
  // frame). At ~8 game fps a beat arrives every ~125 ms, so a fixed 100 ms heartbeat freshness
  // window refused about a fifth of all taps/drops. The window is now one observed frame plus
  // half (never below the old 100 ms, never above 400 ms, always below the health window): the
  // guard still demands that the helper beat within the last frame, and the guest re-validates
  // every native pointer/count at commit (and refuses a request older than 3 beats).
  Clock::duration frame_interval{};
  uint32_t hold_refusal{};
  uint32_t heartbeat{},pending{},epoch{},request_epoch{};
  QuickKind pending_kind{};
  uint64_t target{},cursor_screen{};
  int32_t cursor_index{-1};
  bool cursor_supported{};
  bool healthy{},epoch_changed{},quest_supported{},inventory_supported{},lower_armor_supported{},armor_supported{},arrows_supported{},remove_supported{},remove_armor_supported{},consume_supported{},pending_consume{},pending_ready_consume{},pending_quest{},pending_inventory{};
  int32_t consume_value{};
  bool hold_supported{},pending_hold{};InventoryHold hold_before;
  bool world_rune_supported{},pending_world_rune{};uint32_t rune_refusal{};int32_t world_rune_id{-1};uint64_t world_rune_state{},world_rune_screen{};
  bool sort_supported{},pending_sort{};int sort_category{-1},sort_before{-1};TrackerFlagReader sort_flags;
  bool warp_supported{},pending_warp{};uint32_t warp_refusal{},warp_result{},warp_acked{};
  bool map_supported{},pending_map{},map_delete{};uint32_t map_refusal{},equip_refusal{},quest_refusal{};
  int map_target_index{-1},map_kind{-1};unsigned map_saved_matches{};std::array<float,3> map_xyz{};
  uint32_t target_slot{},previous_actor_id{};
  PlayerEquipmentSnapshot inventory_before;
  std::string target_actor,target_pouch_actor,target_head_actor;
  bool refresh_head{},pending_remove{},pending_ready_equip{};uint32_t previous_head_id{};
  int32_t target_quest_index{-1};
  uint64_t target_map{};
  const char* status="Selection unavailable";
  // Guest helper mailbox magics "DUO3".."DUOG" in release order ("DUOB" was never
  // published). Returns 1..13, or 0 for an unknown helper.
  static unsigned helper_level(uint32_t magic) {
    constexpr uint32_t magics[]={0x334F5544,0x344F5544,0x354F5544,0x364F5544,0x374F5544,0x384F5544,0x394F5544,
                                 0x414F5544,0x434F5544,0x444F5544,0x454F5544,0x464F5544,0x474F5544,0x484F5544,0x494F5544};
    for(unsigned i=0;i<sizeof(magics)/sizeof(magics[0]);++i)if(magic==magics[i])return i+1;
    return 0;
  }
  Clock::duration beat_window() const {
    using std::chrono::milliseconds;
    const Clock::duration lo=milliseconds(100),hi=milliseconds(400);
    const auto w=frame_interval*3/2;
    return w<lo?lo:w>hi?hi:w;
  }
  bool beat_fresh(Clock::time_point now) const {return now-heartbeat_at<=beat_window();}
  // The helper is "alive" while it beat within 2.5 observed frames (never below the original
  // 250 ms, never above 750 ms). At ~4 game fps a single slow frame exceeded the fixed 250 ms and
  // flickered the whole bridge unhealthy, which retired every hold token.
  Clock::duration health_window() const {
    using std::chrono::milliseconds;
    const auto w=frame_interval*5/2;
    return w<milliseconds(250)?Clock::duration(milliseconds(250)):w>milliseconds(750)?Clock::duration(milliseconds(750)):w;
  }
  // Pending requests are acknowledged by the helper on its next frame: allow two observed frames
  // (plus slack) before cancelling, never less than the original 250 ms nor more than 600 ms.
  Clock::duration ack_window() const {
    using std::chrono::milliseconds;
    const auto w=frame_interval*2+milliseconds(50);
    return w<milliseconds(250)?Clock::duration(milliseconds(250)):w>milliseconds(800)?Clock::duration(milliseconds(800)):w;
  }
  void note_beat(uint32_t beat,Clock::time_point now) {
    if(beat==heartbeat)return;
    if(heartbeat_at.time_since_epoch().count()) {
      const auto d=now-heartbeat_at;
      if(d>Clock::duration::zero()&&d<=std::chrono::seconds(1))
        frame_interval=frame_interval.count()?(frame_interval*3+d)/4:d;
    }
    heartbeat=beat;heartbeat_at=now;
  }
  void cancel() {
    if (ext.store_u32) ext.store_u32(ext.userdata,72,0);
  }
  void configure(const EdenDsmodHostExtensions* h) {
    cancel();ext={};healthy=false;
    if (h && h->version==EDEN_DSMOD_EXT_VERSION && h->struct_size==sizeof(*h) &&
        h->abi_hash==EDEN_DSMOD_EXT_HASH && h->mailbox_size>=4096 &&
        h->load_u32 && h->store_u32 && h->store_u64) {ext=*h;cancel();}
  }
  bool scope() const {
    const char* page=host.get_text ? host.get_text(host.userdata,"__page") : nullptr;
    return page && std::strcmp(page,"ready")==0 && host.get_i64 &&
      host.get_i64(host.userdata,"__aux_present",0)==1;
  }
  bool quest_scope() const {
    const char* page=host.get_text ? host.get_text(host.userdata,"__page") : nullptr;
    return page && std::strcmp(page,"quest")==0 && host.get_i64 &&
      host.get_i64(host.userdata,"__aux_present",0)==1;
  }
  bool inventory_scope() const {
    const char* page=host.get_text ? host.get_text(host.userdata,"__page") : nullptr;
    return page && std::strcmp(page,"inventory")==0 && host.get_i64 &&
      host.get_i64(host.userdata,"__aux_present",0)==1;
  }
  bool equip_scope(bool ready) const {return ready ? scope() : inventory_scope();}
  bool map_scope() const {
    const char* page=host.get_text ? host.get_text(host.userdata,"__page") : nullptr;
    return page && std::strcmp(page,"map")==0 && host.get_i64 &&
      host.get_i64(host.userdata,"__aux_present",0)==1;
  }
  // Travel is offered on the Map page (marker inspect) and the Trackers page (selected row).
  // Owner item 11 (iteration 10, 2026-10-20): the Ready page's shrine-panel Travel button
  // (item 9, iteration 9) is REMOVED -- standing inside the shrine already means the player is
  // there, Travel to it is redundant. "ready" dropped from this scope accordingly.
  bool warp_scope() const {
    const char* page=host.get_text ? host.get_text(host.userdata,"__page") : nullptr;
    return page && (std::strcmp(page,"map")==0 || std::strcmp(page,"trackers")==0) &&
      host.get_i64 && host.get_i64(host.userdata,"__aux_present",0)==1;
  }
  bool load(uint32_t at,uint32_t& value) const {
    return ext.load_u32 && ext.load_u32(ext.userdata,at,&value);
  }
  // Iteration 11 item E (owner: "hidden companion = native HUD back"). The guest helper
  // (companion-hud.inc) reads mailbox+2048 every UI frame: 0 = companion shown, apply our HUD
  // hide rules (minimap draw tree off, Dungeon pose on the five side gauges); 1 = companion
  // hidden, leave the minimap drawn and re-apply the native pose once (guest latch +2052).
  // Written only when the mailbox disagrees (an edge, a fresh process, or a helper that
  // re-zeroed it), so steady ticks cost one mailbox load and no store.
  static constexpr uint32_t hud_native_at=2048;
  bool sync_native_hud(bool aux_present) const {
    if(!ext.load_u32 || !ext.store_u32)return false;
    const uint32_t want=aux_present ? 0u : 1u;
    uint32_t have{};
    if(load(hud_native_at,have) && have==want)return false;
    return ext.store_u32(ext.userdata,hud_native_at,want)!=0;
  }
  uint32_t session_epoch() const {
    const int64_t value=host.get_i64 ? host.get_i64(host.userdata,"__mailbox_epoch",0) : 0;
    return value>0 && uint64_t(value)<=UINT32_MAX ? uint32_t(value) : 0;
  }
  void poll(const EdenDsmodHostApi& h,NativeQuick native) {
    const auto now=Clock::now();
    if (sampled.time_since_epoch().count() && now-sampled>std::chrono::milliseconds(250)) cancel();
    host=h;sampled=now;
    const uint32_t current=session_epoch();
    epoch_changed=current!=epoch;
    if (epoch_changed) {cancel();epoch=current;}
    uint32_t magic{},beat{},guest_epoch{};
    unsigned level{};
    healthy=epoch && load(4092,guest_epoch) && guest_epoch==epoch &&
      load(0,magic) && (level=helper_level(magic))!=0 && load(4,beat);
    if(!healthy)level=0;
    // Each helper keeps every earlier route; a feature needs the helper that introduced it.
    quest_supported=level>=2;inventory_supported=level>=3;lower_armor_supported=level>=4;
    armor_supported=level>=5;arrows_supported=level>=6;remove_supported=level>=7;
    remove_armor_supported=level>=8;consume_supported=level>=9;map_supported=level>=10;
    world_rune_supported=level>=11;cursor_supported=level>=12;hold_supported=level>=13;warp_supported=level>=14;sort_supported=level>=15;
#ifdef BOTW_190_INVENTORY_EVIDENCE
    if(matches_build(h,InventoryBuild190)) {
      quest_supported=map_supported=world_rune_supported=cursor_supported=warp_supported=false;
#ifdef BOTW_190_QUEST_EVIDENCE
      quest_supported=level>=2;
#endif
    }
#endif
    if (healthy) note_beat(beat,now);
    healthy=healthy && heartbeat_at.time_since_epoch().count() &&
      now-heartbeat_at<health_window();
    if (!pending) {
      // Fix round 3, Inventory finding 7: native BOTW shows no idle "ready" status line in the
      // pause inventory -- clear it instead of replacing with "Ready to select" once the guest
      // helper is healthy again (still clears the earlier "Selection unavailable" message).
      if (healthy && std::strcmp(status,"Selection unavailable")==0) status="";
      return;
    }
    uint32_t ack{},result{};
    if (load(12,ack) && ack==pending && load(16,result)) {
      if(pending_sort) {
        cancel();
        const bool scoped=healthy && !epoch_changed && request_epoch==epoch && inventory_scope() && context(h)==UiContext::World;
        uint64_t manager{};int32_t category{-1};
        const bool confirmed=result==1 && scoped && pouch(h).valid &&
          read(h,h.main_base+inventory_offsets(h).pouch_root,manager) && read(h,manager+0x44800,category) && category==sort_category &&
          (sort_category>=4 || sort_mode(sort_category)==1-sort_before);
        if(result==1 && scoped && !confirmed && now-requested<std::chrono::seconds(3)){
          status="Sorting";return;
        }
        status=result==2?"Sort refused":confirmed?"Sorted":"Sort not confirmed";
        pending=0;pending_sort=false;return;
      }
      if(pending_warp) {
        // The Travel transaction (botw_warp_logic.h) owns what happens next; the bridge only
        // reports the guest's one-shot answer. Never re-sent.
        cancel();warp_result=result;++warp_acked;
        status=result==2?"Travel refused":result==1?"Traveling...":"Travel not confirmed";
        pending=0;pending_warp=false;return;
      }
      if(pending_world_rune) {
        cancel();
        const bool scoped=healthy && !epoch_changed && request_epoch==epoch && scope() && context(h)==UiContext::World;
        const auto fresh=scoped ? world_runes(h) : WorldRunes{};
        const bool confirmed=result==1 && fresh.valid && fresh.state==world_rune_state && fresh.screen==world_rune_screen && fresh.current==world_rune_id && fresh.screen_current==world_rune_id;
        if(result==1 && scoped && !confirmed && now-requested<std::chrono::seconds(3)){status="Changing rune";return;}
        status=result==2 ? "Rune change refused" : confirmed ? "Rune selected" : "Rune change not confirmed";
        pending=0;pending_world_rune=false;return;
      }
      if (pending_map) {
        cancel();
        const bool scoped=healthy && !epoch_changed && request_epoch==epoch && map_scope() && context(h)==UiContext::World;
        const auto fresh=scoped ? map_edit_snapshot(h) : MapEditSnapshot{};
        const auto saved=scoped ? saved_map_icons(h) : SavedMapIcons{};
        bool native_ok=false,saved_ok=false;
        if(fresh.valid && map_target_index>=0 && map_target_index<105) {
          const auto& v=fresh.markers[map_target_index];
          native_ok=v.address==target && (map_delete ? !v.active : v.active && v.kind==map_kind &&
            std::abs(v.xyz[0]-map_xyz[0])<.05f && std::abs(v.xyz[2]-map_xyz[2])<.05f);
        }
        if(saved.valid) {
          unsigned found=0;
          for(const auto& v:saved.icons)if(v.kind==map_kind && std::abs(v.xyz[0]-map_xyz[0])<.05f && std::abs(v.xyz[2]-map_xyz[2])<.05f)++found;
          saved_ok=map_delete ? found+1==map_saved_matches : found>0;
        }
        const bool confirmed=result==1 && native_ok && saved_ok;
        if(result==1 && scoped && !confirmed && now-requested<std::chrono::seconds(3)) {status="Updating map";return;}
        status=result==2 ? "Map edit refused" : confirmed ? (map_delete ? "Marker deleted" : "Marker placed") : "Map edit not confirmed";
        pending=0;pending_map=false;return;
      }
      if(pending_hold){
        cancel();
        const bool scoped=healthy&&!epoch_changed&&request_epoch==epoch&&inventory_scope()&&context(h)==UiContext::World;
        const auto tray=scoped?held_tray(h):HeldTray{};
        const auto carry=scoped?held_carry(h):HeldCarry{};
        const bool unchanged=scoped&&player_equipment(h)==hold_before.equipment;
        const bool confirmed=result==1&&unchanged&&(hold_before.put_away?
          carry.empty()&&held_return_confirmed(hold_before.tray,tray,pouch(h)):
          carry.active()&&carry.flags==0x40&&held_add_confirmed(hold_before.tray,tray,hold_before.item));
        if(result==1&&scoped&&!confirmed&&now-requested<std::chrono::seconds(3)){
          status=hold_before.put_away?"Putting away":"Preparing to hold";return;
        }
        status=result==2?(hold_before.put_away?"Put away refused":"Hold refused"):
          confirmed?(hold_before.put_away?"Items returned":"Holding"):
          (hold_before.put_away?"Put away not confirmed":"Hold not confirmed");
        pending=0;pending_hold=false;return;
      }
      if (pending_consume) {
        cancel();
        // Minimap helper cards (temperature/food, 2026-10-18): a Ready-scope Eat confirms in
        // the Ready scope it was sent from, same as a Ready-scope Equip already does.
        const bool valid_scope=healthy && !epoch_changed && request_epoch==epoch && equip_scope(pending_ready_consume) && context(h)==UiContext::World;
        const auto fresh=valid_scope ? pouch(h) : Snapshot{};
        const auto attached=valid_scope ? player_equipment(h) : PlayerEquipmentSnapshot{};
        const Item* chosen=nullptr;
        if(fresh.valid)for(const auto& i:fresh.items)if(i.address==target)chosen=&i;
        uint32_t food_job{},food_result{};
        const bool applied=load(416,food_job) && !food_job && load(420,food_result) && food_result==1;
        const bool confirmed=result==1 && applied && fresh.valid && attached==inventory_before &&
          (consume_value==1 ? chosen==nullptr : chosen && !chosen->equipped && chosen->actor==target_pouch_actor && chosen->value==consume_value-1);
        if(result==1 && valid_scope && !confirmed && now-requested<std::chrono::milliseconds(3000)) {status="Eating";return;}
        status=result==2 ? "Eat refused" : confirmed ? "Consumed" : "Eat not confirmed";
        pending=0;pending_consume=false;pending_ready_consume=false;return;
      }
      if (pending_inventory) {
        cancel();
        const bool valid_scope=healthy && !epoch_changed && request_epoch==epoch &&
          equip_scope(pending_ready_equip) && context(h)==UiContext::World;
        const auto fresh=valid_scope ? pouch(h) : Snapshot{};
        const auto attached=valid_scope ? player_equipment(h) : PlayerEquipmentSnapshot{};
        const Item* chosen=nullptr;
        unsigned arrow_equipped=0;
        if(fresh.valid)for(const auto& i:fresh.items)if(i.type==2 && i.equipped)++arrow_equipped;
        if(fresh.valid)for(const auto& i:fresh.items)if(i.address==target)chosen=&i;
        bool actor_confirmed=false;
        if(attached.valid) {
          if(pending_remove && target_slot<3) actor_confirmed=attached.slots[target_slot].id==UINT32_MAX;
          else if(target_slot==6) actor_confirmed=arrow_equipped==1 && attached==inventory_before;
          else if(target_slot<6) actor_confirmed=attached.slots[target_slot].name==target_actor &&
            attached.slots[target_slot].id!=previous_actor_id;
        }
        const bool confirmed=result==1 && chosen && (pending_remove ? !chosen->equipped : chosen->equipped) &&
          chosen->actor==target_pouch_actor && actor_confirmed &&
          (!refresh_head || (attached.slots[3].name==target_head_actor && attached.slots[3].id!=previous_head_id));
        if(result==1 && valid_scope && !confirmed && now-requested<std::chrono::seconds(3)) {
          status=pending_remove ? "Removing" : "Equipping";return;
        }
        status=result==2 ? (pending_remove ? "Removal refused" : "Equip refused") : confirmed ? (pending_remove ? "Removed" : "Equipped") : (pending_remove ? "Removal not confirmed" : "Equip not confirmed");
        pending=0;pending_inventory=false;return;
      }
      if (pending_quest) {
        cancel();
        const bool valid_scope=healthy && !epoch_changed && request_epoch==epoch && quest_scope() && context(h)==UiContext::World;
        const auto destination=valid_scope ? quest_destination(h) : CurrencySnapshot{};
        uint64_t map{},selected{};
        const bool confirmed=result==1 && destination.valid && destination.value==target_quest_index &&
          read(h,h.main_base+ready_offsets(h).map_root,map) && map==target_map &&
          read(h,map+0x3A0,selected) && selected==target;
        if (result==1 && valid_scope && !confirmed && now-requested<std::chrono::milliseconds(500)) {
          status="Awaiting native quest update";return;
        }
        status=result==2 ? "Quest selection refused" : confirmed ? "Quest selected" : "Quest selection not confirmed";
        pending=0;pending_quest=false;return;
      }
      // A held tap changes only the native cursor. Equipment and the active
      // world Rune remain under the game's ordinary hold-release handler.
      UiSample ui{};uint64_t screen{};
      const bool scoped=healthy && cursor_supported && !epoch_changed && request_epoch==epoch && scope() &&
        native.valid && native.kind==pending_kind && ui_sample(h,ui) &&
        read(h,ui.array+45*8,screen) && screen==cursor_screen;
      bool confirmed=false;
      if (result==1 && scoped) {
        if (pending_kind==QuickKind::Rune) {
          const auto fresh=runes(h);
          confirmed=fresh.valid && fresh.highlighted==cursor_index && cursor_index>=0 &&
            cursor_index<fresh.count && fresh.ids[size_t(cursor_index)]==int32_t(target);
        } else {
          const auto fresh=gear(h,pouch(h));
          confirmed=fresh.valid && fresh.screen==cursor_screen && fresh.highlighted==cursor_index &&
            ((cursor_index==-1 && target==0) || (cursor_index>=0 && size_t(cursor_index)<fresh.items.size() && fresh.items[size_t(cursor_index)].address==target));
        }
      }
      status=result==2 ? "Selection refused" : confirmed ? "Highlighted" : "Selection not confirmed";
      pending=0;cancel();return;
    }
    if (!healthy || (pending_warp ? (!warp_scope() || context(h)!=UiContext::World) : pending_world_rune ? (!scope() || context(h)!=UiContext::World) : pending_map ? (!map_scope() || context(h)!=UiContext::World) : pending_inventory ? (!equip_scope(pending_ready_equip) || context(h)!=UiContext::World) : pending_sort ? (!inventory_scope() || context(h)!=UiContext::World) : pending_hold ? (!inventory_scope() || context(h)!=UiContext::World) : pending_consume ? (!equip_scope(pending_ready_consume) || context(h)!=UiContext::World) : pending_quest ? (!quest_scope() || context(h)!=UiContext::World) : (!scope() || !native.valid || native.kind!=pending_kind)) ||
        now-requested>ack_window()) {
      cancel();status="Selection cancelled";
    }
  }
  // Native flags select the NEXT Y-sort comparator; sortItems flips them afterward.
  // sortKey is actor metadata (type/set), not pickup chronology.
  int sort_mode(int category) {
    if(category<0||category>6)return -1;
    if(category>=4)return 0;
    constexpr const char* keys[]={"SortTypeWeaponPouch","SortTypeBowPouch","SortTypeShieldPouch","SortTypeArmorPouch"};
    if(!sort_flags.begin(host,true))return -1;
    const auto value=sort_flags.observe_boolean(warp::crc(keys[category]));
    return sort_flags.finish()&&value ? int(*value) : -1;
  }
  static const char* sort_label(int category,int mode) {
    if(mode<0)return "Sort";
    switch(category){
    case 0:case 1:return mode?"Sort: Type":"Sort: Power";
    case 2:return mode?"Sort: Type":"Sort: Guard";
    case 3:return mode?"Sort: Set":"Sort: Part";
    case 4:return "Sort: Type";
    case 5:return "Sort: Effect";
    default:return "Sort: Type";
    }
  }
  bool request_sort(int32_t category) {
    const auto now=Clock::now();uint32_t guest_epoch{};
    if (!healthy || !sort_supported || pending || !epoch || session_epoch()!=epoch ||
        !load(4092,guest_epoch) || guest_epoch!=epoch || !inventory_scope() ||
        context(host)!=UiContext::World || category<0 || category>6 ||
        now-sampled>std::chrono::milliseconds(100) || !beat_fresh(now)) return false;
    const int mode=sort_mode(category);const auto held=held_tray(host);
    if(mode<0 || !held.valid || held.count)return false;
    // Mailbox offset 48 is the universal "ui root" identity every request kind's dispatcher
    // checks BEFORE the per-kind switch (bug found live this round,
    // "Sort refused"): same UiSample.manager every other op (quest/hold/map/...)
    // already populates there, not the pouch-specific identity (offset 32, this op's own).
    UiSample ui{};
    if (!ui_sample(host,ui) || ui.flags!=0x1E09000000ULL) return false;
    uint64_t manager{};
    if (!read(host,host.main_base+inventory_offsets(host).pouch_root,manager) || manager%8 || !manager) return false;
    uint32_t sequence{},ack{},beat{};
    if (!load(8,sequence) || !load(12,ack) || sequence!=ack || !load(4,beat)) return false;
    if (++sequence==0) ++sequence;
    auto u32=[&](uint32_t at,uint32_t v){return ext.store_u32(ext.userdata,at,v)!=0;};
    auto u64=[&](uint32_t at,uint64_t v){return ext.store_u64(ext.userdata,at,v)!=0;};
    cancel();
    if (!u32(16,0) || !u32(20,14) || !u32(24,uint32_t(category)) || !u64(32,manager) ||
        !u64(48,ui.manager) || !u32(64,beat) || !u32(88,epoch) || session_epoch()!=epoch ||
        !inventory_scope() || context(host)!=UiContext::World || !u32(72,sequence) ||
        session_epoch()!=epoch || !u32(8,sequence)) {cancel();return false;}
    pending=sequence;pending_sort=true;pending_hold=false;pending_consume=false;pending_inventory=false;
    pending_quest=false;pending_map=false;pending_world_rune=false;pending_warp=false;
    sort_category=category;sort_before=mode;
    request_epoch=epoch;requested=now;status="Sort queued";return true;
  }
  // One Travel request (op 13) for the native record `point` (landmarks index `n`). Payload first,
  // then the permit and sequence, exactly like the other routes; the guest re-validates all of it.
  bool request_warp(unsigned n,const warp::Point& display) {
    const auto now=Clock::now();uint32_t guest_epoch{};
    warp_refusal=(!healthy?1u:0u)|(!warp_supported?2u:0u)|(pending?4u:0u)|(!epoch||session_epoch()!=epoch?8u:0u)|
      ((!load(4092,guest_epoch)||guest_epoch!=epoch)?16u:0u)|(!warp_scope()?32u:0u)|
      (now-sampled>std::chrono::milliseconds(100)?128u:0u)|(!beat_fresh(now)?256u:0u);
    if(warp_refusal)return false;
    const auto fresh=warp::point(host,n);
    if(!fresh.valid||fresh!=display||!fresh.enabled()){warp_refusal=512;return false;}
    UiSample ui{};uint64_t evt{};
    if(!ui_sample(host,ui)||ui.flags!=0x1E09000000ULL||!read(host,host.main_base+warp::EventRoot,evt)||!evt){warp_refusal=1024;return false;}
    uint32_t sequence{},ack{},beat{};
    if(!load(8,sequence)||!load(12,ack)||sequence!=ack||!load(4,beat)){warp_refusal=2048;return false;}
    if(++sequence==0)++sequence;
    auto u32=[&](uint32_t at,uint32_t v){return ext.store_u32(ext.userdata,at,v)!=0;};
    auto u64=[&](uint32_t at,uint64_t v){return ext.store_u64(ext.userdata,at,v)!=0;};
    cancel();
    if(!u32(16,0)||!u32(20,13)||!u64(48,ui.manager)||!u32(64,beat)||!u32(88,epoch)||
       !u64(1024,fresh.data)||!u64(1032,fresh.record)||!u32(1040,fresh.index)||!u32(1044,uint32_t(fresh.kind))||
       !u64(1048,fresh.entry)||!u64(1056,fresh.map_ptr)||!u64(1064,fresh.pos_ptr)||
       !u32(1072,warp::fnv(fresh.map))||!u32(1076,warp::fnv(fresh.pos))||!u64(1080,evt)||
       session_epoch()!=epoch||!warp_scope()||context(host)!=UiContext::World||
       !u32(72,sequence)||session_epoch()!=epoch||!u32(8,sequence)){cancel();warp_refusal=4096;return false;}
    pending=sequence;pending_warp=true;pending_map=false;pending_inventory=false;pending_consume=false;pending_quest=false;pending_hold=false;pending_world_rune=false;
    request_epoch=epoch;requested=now;status="Travel queued";return true;
  }
  bool request_world_rune(int32_t id,const WorldRunes& display) {
    const auto now=Clock::now();uint32_t guest_epoch{};
    const bool epoch_ok=load(4092,guest_epoch) && guest_epoch==epoch;
    rune_refusal=(!healthy || !world_rune_supported || pending || !epoch || session_epoch()!=epoch || !epoch_ok || !scope()) ? 1u : 0u;
    if(now-sampled>std::chrono::milliseconds(100))rune_refusal|=128;
    if(!beat_fresh(now))rune_refusal|=256;
    if(rune_refusal || id<0 || id>7 || !display.valid)return false;
    const auto fresh=world_runes(host);if(!fresh.valid || fresh!=display)return false;
    bool available=false;for(int i=0;i<fresh.list.count;++i)if(fresh.list.ids[size_t(i)]==id)available=true;
    if(!available)return false;
    const auto& flag=fresh.unlock[size_t(id)];
    uint32_t sequence{},ack{},beat{};
    if(!load(8,sequence) || !load(12,ack) || sequence!=ack || !load(4,beat))return false;
    if(++sequence==0)++sequence;
    UiSample ui{};if(!ui_sample(host,ui) || ui.flags!=0x1E09000000ULL)return false;
    auto u32=[&](uint32_t at,uint32_t v){return ext.store_u32(ext.userdata,at,v)!=0;};
    auto u64=[&](uint32_t at,uint64_t v){return ext.store_u64(ext.userdata,at,v)!=0;};
    cancel();
    if(!u32(16,0) || !u32(20,10) || !u64(48,ui.manager) || !u32(64,beat) || !u32(88,epoch) ||
       !u64(640,fresh.screen) || !u64(648,fresh.state) || !u64(656,fresh.game_data) || !u64(664,fresh.dlc) ||
       !u64(672,fresh.holder) || !u64(680,fresh.bank) || !u64(688,fresh.flags) || !u64(696,flag.address) ||
       !u32(704,uint32_t(flag.index)) || !u32(708,uint32_t(fresh.flag_count)) || !u32(712,uint32_t(id)) ||
       !u32(716,uint32_t(fresh.current)) || !u32(720,uint32_t(fresh.screen_current)) || !u32(724,fresh.dlc_version) ||
       !u32(728,fresh.permission) || !u32(732,flag.properties) ||
       session_epoch()!=epoch || !scope() || context(host)!=UiContext::World ||
       !u32(72,sequence) || session_epoch()!=epoch || !u32(8,sequence)){cancel();return false;}
    pending=sequence;pending_world_rune=true;pending_map=false;pending_inventory=false;pending_consume=false;pending_quest=false;
    request_epoch=epoch;requested=now;world_rune_id=id;world_rune_state=fresh.state;world_rune_screen=fresh.screen;status="Rune change queued";return true;
  }
  bool request_map(unsigned palette,float x,float z,int deletion=-1) {
    const auto now=Clock::now();uint32_t guest_epoch{};status="Map request expired";
    const bool have_epoch=load(4092,guest_epoch);
    map_refusal=(!healthy?1u:0u) | (!map_supported?2u:0u) | (pending?4u:0u) | (!epoch?8u:0u) |
      (session_epoch()!=epoch?16u:0u) | ((!have_epoch || guest_epoch!=epoch)?32u:0u) | (!map_scope()?64u:0u) |
      (now-sampled>std::chrono::milliseconds(100)?128u:0u) | (!beat_fresh(now)?256u:0u) |
      ((!std::isfinite(x) || !std::isfinite(z) || std::abs(x)>6000 || std::abs(z)>5000)?512u:0u);
    if(map_refusal)return false;
    status="Map data unavailable";
    const auto a=map_edit_snapshot(host);const auto save=saved_map_icons(host);
    const int index=deletion>=0 ? deletion : map_edit_target(a,palette);
    if(!a.valid || !save.valid || index<0 || index>=105 || (deletion>=0 && !a.markers[index].active))return false;
    const auto& v=a.markers[index];
    status="Map bridge busy";
    uint32_t sequence{},ack{},beat{};
    if(!load(8,sequence) || !load(12,ack) || sequence!=ack || !load(4,beat))return false;
    if(++sequence==0)++sequence;
    auto u32=[&](uint32_t at,uint32_t value){return ext.store_u32(ext.userdata,at,value)!=0;};
    auto u64=[&](uint32_t at,uint64_t value){return ext.store_u64(ext.userdata,at,value)!=0;};
    auto f32=[&](uint32_t at,float f){uint32_t bits;std::memcpy(&bits,&f,4);return u32(at,bits);};
    cancel();status="Map request interrupted";
    if(!u32(16,0) || !u32(20,9) || !u64(48,a.ui.manager) || !u32(64,beat) || !u32(88,epoch) ||
       !u64(640,a.data) || !u64(648,a.controller) || !u64(656,a.area) || !u64(664,a.stamps) ||
       !u64(672,a.pins) || !u64(680,v.address) || !f32(688,v.xyz[0]) || !f32(692,v.xyz[1]) || !f32(696,v.xyz[2]) ||
       !u32(700,uint32_t(v.kind)) || !u32(704,v.active) || !u32(708,deletion>=0?14:palette) ||
       !f32(712,x) || !f32(716,z) || !u64(720,a.free_stamp) || !u32(728,index<5?index:index-5) || !u32(732,index<5) ||
       !f32(736,a.projection[0]) || !f32(740,a.projection[1]) || !f32(744,a.projection[2]) || !f32(748,a.projection[3]) ||
       !f32(752,a.scale[0]) || !f32(756,a.scale[1]) || !u64(760,a.gamedata) || session_epoch()!=epoch || !map_scope() ||
       context(host)!=UiContext::World || !u32(72,sequence) || session_epoch()!=epoch || !u32(8,sequence)) {cancel();return false;}
    pending=sequence;pending_map=true;pending_consume=false;pending_inventory=false;pending_quest=false;
    map_delete=deletion>=0;map_target_index=index;target=v.address;map_kind=map_delete?v.kind:map_edit_kind(palette);
    map_xyz=map_delete?v.xyz:std::array<float,3>{x,0,z};map_saved_matches=0;
    for(const auto& icon:save.icons)if(icon.kind==map_kind && std::abs(icon.xyz[0]-map_xyz[0])<.05f && std::abs(icon.xyz[2]-map_xyz[2])<.05f)++map_saved_matches;
    request_epoch=epoch;requested=now;status="Map edit queued";return true;
  }
  bool hold_return_can_supersede() const {
    uint32_t ack{},result{};
    return pending&&pending_hold&&!hold_before.put_away&&load(12,ack)&&ack==pending&&load(16,result)&&result==1;
  }
  bool request_hold(uint64_t address,const Snapshot& display,bool put_away=false){
    const auto now=Clock::now();uint32_t guest_epoch{};
    // Fix round A: low bits = bridge gates, bits 16+ = inventory_hold() stage (diagnostic only).
    hold_refusal=(!healthy||!hold_supported?1u:0u)|(pending&&!(put_away&&hold_return_can_supersede())?2u:0u)|
      (!epoch||session_epoch()!=epoch||!load(4092,guest_epoch)||guest_epoch!=epoch?4u:0u)|(!inventory_scope()?8u:0u)|
      (now-sampled>std::chrono::milliseconds(100)?128u:0u)|(!beat_fresh(now)?256u:0u);
    if(hold_refusal)return false;
    uint32_t stage{};
    const auto action=inventory_hold(host,address,display,put_away,&stage);if(!action.valid){hold_refusal=stage<<16;return false;}
    uint32_t sequence{},ack{},beat{};
    if(!load(8,sequence)||!load(12,ack)||sequence!=ack||!load(4,beat)){hold_refusal=16;return false;}
    if(++sequence==0)++sequence;
    auto u32=[&](uint32_t at,uint32_t value){return ext.store_u32(ext.userdata,at,value)!=0;};
    auto u64=[&](uint32_t at,uint64_t value){return ext.store_u64(ext.userdata,at,value)!=0;};
    cancel();
    if(!u32(16,0)||!u32(20,put_away?12:11)||!u32(24,7)||!u32(28,7)||!u64(32,action.manager)||
       !u64(40,put_away?0:address)||!u64(48,action.ui.manager)||!u32(56,action.count)||!u32(64,beat)||
       !u32(76,put_away?0:uint32_t(action.item.value))||!u64(80,action.name)||!u32(88,epoch)||
       !u64(96,action.equipment.info)||!u64(104,action.equipment.player)||!u64(112,action.equipment.creator)||
       !u32(124,action.metadata88)||!u32(128,action.metadata80)||!u32(132,put_away?0:uint32_t(actor_key(action.item.actor)))||
       !u64(800,action.carry.root)||!u64(808,action.carry.link)||!u64(816,action.carry.request)||
       !u32(824,action.carry.generation)||!u32(828,action.carry.flags)||!u32(832,action.tray.count)||!u32(836,action.carry.acquired))return false;
    for(unsigned i=0;i<5;++i){const auto& slot=action.tray.slots[i];const uint32_t at=840+i*24;
      if(!u64(at,slot.address)||!u32(at+8,uint32_t(slot.remaining))||!u32(at+12,slot.address?uint32_t(actor_key(slot.actor)):0)||
         !u32(at+16,uint32_t(slot.flag8)|(uint32_t(slot.flag9)<<8))||!u32(at+20,slot.in_inventory))return false;
    }
    if(!u32(960,action.column)||!u32(964,action.index)||!u64(968,action.info_data)||
       !u64(976,action.carry.actor)||!u64(984,action.carry.actor_vtable)||!u64(992,action.heap_root)||!u64(1000,action.heap)||
       session_epoch()!=epoch||!inventory_scope()||context(host)!=UiContext::World||
       !u32(72,sequence)||session_epoch()!=epoch||!u32(8,sequence)){cancel();return false;}
    pending=sequence;pending_hold=true;pending_consume=false;pending_inventory=false;pending_quest=false;pending_map=false;pending_world_rune=false;
    hold_before=action;request_epoch=epoch;requested=now;status=put_away?"Put away queued":"Hold queued";return true;
  }
  // from_ready: a minimap helper card Eat (temperature/food, 2026-10-18) -- same native
  // request; only the companion page scope the request is allowed from differs.
  bool request_consume(uint64_t address,const Snapshot& display,bool from_ready=false) {
    const auto now=Clock::now();uint32_t guest_epoch{};
    if(!healthy || !consume_supported || pending || !epoch || session_epoch()!=epoch ||
       !load(4092,guest_epoch) || guest_epoch!=epoch || !equip_scope(from_ready) ||
       now-sampled>std::chrono::milliseconds(100) || !beat_fresh(now))return false;
    if(hold_supported){const auto tray=held_tray(host);if(!tray.valid||tray.count)return false;}
    const auto action=inventory_consume(host,address,display);if(!action.valid)return false;
    uint32_t sequence{},ack{},beat{};
    if(!load(8,sequence) || !load(12,ack) || sequence!=ack || !load(4,beat))return false;
    if(++sequence==0)++sequence;
    auto u32=[&](uint32_t at,uint32_t v){return ext.store_u32(ext.userdata,at,v)!=0;};
    auto u64=[&](uint32_t at,uint64_t v){return ext.store_u64(ext.userdata,at,v)!=0;};
    cancel();
    if(!u32(16,0) || !u32(20,8) || !u32(24,7) || !u32(28,uint32_t(action.item.type)) ||
       !u64(32,action.manager) || !u64(40,address) || !u64(48,action.ui.manager) || !u32(56,action.count) ||
       !u32(64,beat) || !u32(76,uint32_t(action.item.value)) || !u64(80,address+0x3C) || !u32(88,epoch) ||
       !u64(96,action.equipment.info) || !u64(104,action.equipment.player) || !u64(112,action.equipment.creator) ||
       !u32(124,action.food.cook[2]) || !u32(128,action.food.cook[0]) || !u32(132,uint32_t(actor_key(action.item.actor))) ||
       !u32(304,action.cache_column) || !u32(308,action.row) || !u32(336,action.food.cook[1]) ||
       !u32(340,action.food.cook[3]) || !u32(344,action.food.cook[4]) || !u32(348,uint32_t(action.food.use)) ||
       !u64(368,action.screen) || !u64(376,action.column) || !u64(384,action.eat_root) || !u64(392,action.preview) ||
       session_epoch()!=epoch || !equip_scope(from_ready) || context(host)!=UiContext::World ||
       !u32(72,sequence) || session_epoch()!=epoch || !u32(8,sequence)) {cancel();return false;}
    pending=sequence;pending_consume=true;pending_ready_consume=from_ready;pending_inventory=false;pending_quest=false;target=address;
    inventory_before=action.equipment;target_pouch_actor=action.item.actor;consume_value=action.item.value;
    request_epoch=epoch;requested=now;status="Eat queued";return true;
  }
  // from_card: an armor equip from the temperature card (Ready scope, armor types 4-6, no remove).
  bool request_inventory(uint64_t address,const Snapshot& display,bool remove=false,bool from_ready=false,bool from_card=false) {
    if(from_card && (remove || from_ready))return false;
    const bool ready_scope=from_ready || from_card;
    const auto now=Clock::now();uint32_t guest_epoch{};
    const bool epoch_valid=load(4092,guest_epoch) && guest_epoch==epoch;
    equip_refusal=(!healthy || !inventory_supported || pending || !epoch || session_epoch()!=epoch || !epoch_valid || !equip_scope(ready_scope)) ? 1u : 0u;
    if(now-sampled>std::chrono::milliseconds(100))equip_refusal|=128;
    if(!beat_fresh(now))equip_refusal|=256;
    if(equip_refusal)return false;
    if(remove && !remove_supported)return false;
    if(hold_supported){const auto tray=held_tray(host);if(!tray.valid||tray.count)return false;}
    const auto action=inventory_equip(host,address,display,lower_armor_supported,armor_supported,arrows_supported,remove,remove_armor_supported);
    // Iteration 8 item 1: Ready-origin requests were previously equip-only (never remove), a
    // companion-side routing restriction -- not a native/runtime one -- left over from before
    // the touch picker's None cell existed. Extend it symmetrically: Ready may now ALSO remove
    // Weapon/Bow/Shield (the same 3 types the Inventory page's own, already-proven, Remove
    // button allows -- INVENTORY-REMOVE.md's DUO9 kind7), never arrows (type 2, no native
    // unequip concept) and never armor (unchanged, armor removal stays Inventory-page-only).
    const bool from_ready_type_ok=remove ? (action.item.type==0 || action.item.type==1 || action.item.type==3)
                                          : (action.item.type==0 || action.item.type==1 || action.item.type==2 || action.item.type==3);
    if(!action.valid || (from_ready && !from_ready_type_ok) ||
       (from_card && (action.item.type<4 || action.item.type>6)))return false;
    uint32_t sequence{},ack{},beat{};
    if(!load(8,sequence) || !load(12,ack) || sequence!=ack || !load(4,beat))return false;
    if(++sequence==0)++sequence;
    const auto& old=action.equipment.slots[action.slot==6 ? 2 : action.slot];
    auto u32=[&](uint32_t at,uint32_t v){return ext.store_u32(ext.userdata,at,v)!=0;};
    auto u64=[&](uint32_t at,uint64_t v){return ext.store_u64(ext.userdata,at,v)!=0;};
    cancel();
    if(!u32(16,0) || !u32(20,remove ? 7 : 6) || !u32(24,action.slot) || !u32(28,uint32_t(action.item.type)) ||
       !u64(32,action.manager) || !u64(40,address) || !u64(48,action.ui.manager) ||
       !u32(56,action.count) || !u32(64,beat) || !u32(76,uint32_t(action.item.value)) ||
       !u64(80,action.name) || !u32(88,epoch) || !u64(96,action.equipment.info) ||
       !u64(104,action.equipment.player) || !u64(112,action.equipment.creator) ||
       !u32(120,old.id) || !u32(124,action.modifier_flags) || !u32(128,action.modifier_value) ||
       !u32(132,action.name_key) || !u64(136,old.actor) || !u64(144,old.link_data) ||
       (remove && (!u32(304,action.column) || !u32(308,action.index) || !u32(320,uint32_t(actor_key(action.fit.valid ? action.fit.target_actor : action.slot>=3 ? "Armor_Default_Lower" : ""))))) ||
       (action.fit.valid && (!u64(224,action.fit.head.address) || !u64(232,action.fit.upper.address) ||
        !u32(240,action.equipment.slots[3].id) || !u32(244,uint32_t(actor_key(action.fit.head_actor))) ||
        !u32(248,action.fit.needs_b) || !u32(252,action.fit.refresh_head) || !u64(256,action.fit.info_data) ||
        !u32(264,uint32_t(action.fit.head.value)) || !u32(268,uint32_t(actor_key(action.fit.head.actor))) ||
        !u64(272,action.equipment.slots[3].actor) || !u64(280,action.equipment.slots[3].link_data) ||
        !u32(288,action.equipment.slots[4].id) || !u32(292,uint32_t(actor_key(action.fit.upper.actor))) ||
        !u32(296,uint32_t(action.fit.upper.value)))) ||
       session_epoch()!=epoch || !equip_scope(ready_scope) || context(host)!=UiContext::World ||
       !u32(72,sequence) || !u32(8,sequence)) {cancel();return false;}
    pending=sequence;pending_remove=remove;pending_inventory=true;pending_ready_equip=ready_scope;pending_quest=false;target=address;
    target_slot=action.slot;inventory_before=action.equipment;target_pouch_actor=action.item.actor;
    target_actor=action.fit.valid ? action.fit.target_actor : remove && action.slot>=3 ? "Armor_Default_Lower" : action.item.actor;
    refresh_head=action.fit.valid && action.fit.refresh_head;
    target_head_actor=action.fit.head_actor;previous_head_id=action.equipment.slots[3].id;
    previous_actor_id=old.id;
    request_epoch=epoch;requested=now;status=remove ? "Removal queued" : "Equip queued";return true;
  }
  bool request_quest(uint64_t address,const QuestSnapshot& display) {
    const auto now=Clock::now();uint32_t guest_epoch{};
    quest_refusal=0;
    if(!healthy)quest_refusal|=1;
    if(!quest_supported)quest_refusal|=2;
    if(pending)quest_refusal|=4;
    if(!epoch || session_epoch()!=epoch)quest_refusal|=8;
    if(!load(4092,guest_epoch) || guest_epoch!=epoch)quest_refusal|=16;
    if(!quest_scope())quest_refusal|=32;
    if(!quests_supported(host))quest_refusal|=64;
    if(now-sampled>std::chrono::milliseconds(100))quest_refusal|=128;
    if(!beat_fresh(now))quest_refusal|=256;
    if(context(host)!=UiContext::World || !display.valid)quest_refusal|=512;
    if(quest_refusal)return false;
    const auto fresh=quests(host);if(!fresh.valid)return false;
    const Quest* shown=nullptr;const Quest* chosen=nullptr;
    for(const auto& q:display.entries)if(q.address==address)shown=&q;
    for(const auto& q:fresh.entries)if(q.address==address)chosen=&q;
    if(!shown || !chosen || chosen->state!=1 || chosen->active ||
       shown->index!=chosen->index || shown->state!=chosen->state || shown->name!=chosen->name ||
       shown->message!=chosen->message) return false;
    uint64_t manager{},map{},gamedata{},vt{},array{},entry{};int32_t count{},handle{};uint32_t dlc{};
    UiSample ui{},after{};
    if(!ui_sample(host,ui) || ui.flags!=0x1E09000000ULL ||
       !read(host,host.main_base+quest_offsets(host).root,manager) || manager%8 || manager>UINT64_MAX-0xC8 ||
       !read(host,manager,vt) || vt!=host.main_base+quest_offsets(host).manager_vtable ||
       !read(host,manager+0xB8,count) || count<1 || count>512 || chosen->index<0 || chosen->index>=count ||
       !read(host,manager+0xC0,array) || array%8 || array>UINT64_MAX-uint64_t(count)*8 ||
       !read(host,array+uint64_t(chosen->index)*8,entry) || entry!=address ||
       !read(host,address+0xD8,dlc) || (dlc&~1u) ||
       !read(host,host.main_base+ready_offsets(host).map_root,map) || map%8 || map>UINT64_MAX-0x3A8 ||
       !read(host,map,vt) || vt!=host.main_base+ready_offsets(host).map_vtable ||
       !read(host,host.main_base+inventory_offsets(host).gamedata_root,gamedata) || gamedata%8 ||
       !read(host,gamedata,vt) || vt!=host.main_base+inventory_offsets(host).gamedata_vtable ||
       !read(host,host.main_base+quest_offsets(host).destination_handle,handle) || handle<0 ||
       !ui_sample(host,after) || after!=ui) return false;
    uint32_t sequence{},ack{},beat{};
    if(!load(8,sequence) || !load(12,ack) || sequence!=ack || !load(4,beat))return false;
    if(++sequence==0)++sequence;
    auto u32=[&](uint32_t at,uint32_t v){return ext.store_u32(ext.userdata,at,v)!=0;};
    auto u64=[&](uint32_t at,uint64_t v){return ext.store_u64(ext.userdata,at,v)!=0;};
    cancel();
    if(!u32(16,0) || !u32(20,5) || !u32(24,uint32_t(chosen->index)) || !u64(40,address) ||
       !u64(48,ui.manager) || !u32(56,uint32_t(count)) || !u32(64,beat) || !u32(88,epoch) ||
       !u64(96,manager) || !u64(104,map) || !u64(112,gamedata) || !u32(120,uint32_t(handle)) ||
       session_epoch()!=epoch || !quest_scope() || context(host)!=UiContext::World ||
       !u32(72,sequence) || !u32(8,sequence)) {cancel();return false;}
    pending=sequence;pending_inventory=false;pending_quest=true;target=address;target_map=map;target_quest_index=chosen->index;
    request_epoch=epoch;requested=now;status="Quest selection queued";return true;
  }
  bool request(int64_t index,QuickKind shown,const GearSnapshot& display,const RuneSnapshot& rune_display) {
    const auto now=Clock::now();
    uint32_t guest_epoch{};
    if (!healthy || !cursor_supported || pending || !epoch || session_epoch()!=epoch ||
        !load(4092,guest_epoch) || guest_epoch!=epoch || !scope() || !supported(host) ||
        now-sampled>std::chrono::milliseconds(100) ||
        !beat_fresh(now) || index< -1 || index>=20) return false;
    const auto native=native_quick(host);
    if (!native.valid || native.kind!=shown) return false;
    int32_t selector=-1,initial=-1,highlight=-1,count=0,value=0;
    uint64_t screen{},identity{},actor{};
    UiSample ui{};
    if (!ui_sample(host,ui)) return false;
    if (shown==QuickKind::Rune) {
      if(index<0)return false;
      const auto fresh=runes(host);
      if (!fresh.valid || !rune_display.valid || fresh.ids!=rune_display.ids ||
          fresh.count!=rune_display.count || index>=fresh.count ||
          !read(host,ui.array+45*8,screen) || screen>UINT64_MAX-0x3868 ||
          !read(host,screen+0x3864,highlight) || highlight!=fresh.highlighted ||
          fresh.selected!=rune_display.selected || fresh.highlighted!=rune_display.highlighted) return false;
      selector=4;identity=uint64_t(fresh.ids[index]);initial=fresh.selected;count=fresh.count;
    } else {
      const auto pouch_snapshot=pouch(host);
      const auto fresh=gear(host,pouch_snapshot);
      if (!fresh.valid || !display.valid || fresh.kind!=display.kind ||
          fresh.screen!=display.screen || fresh.items.size()!=display.items.size() ||
          fresh.initial!=display.initial || fresh.highlighted!=display.highlighted ||
          (index>=0 && size_t(index)>=fresh.items.size())) return false;
      for (size_t i=0;i<fresh.items.size();++i)
        if (fresh.items[i].address!=display.items[i].address ||
            fresh.items[i].actor!=display.items[i].actor ||
            fresh.items[i].value!=display.items[i].value) return false;
      selector=shown==QuickKind::Weapon ? 0 : shown==QuickKind::Shield ? 1 : shown==QuickKind::Bow ? 2 : shown==QuickKind::Arrow ? 3 : -1;
      if (selector<0) return false;
      screen=fresh.screen;
      if(index<0) {
        if(shown==QuickKind::Arrow)return false;
        uint64_t ctl{},vt{};uint32_t cells{},flat{};
        std::array<int32_t,21> mapping{};
        if(!read(host,screen+0x3790,ctl)||!ctl||ctl%8||ctl>UINT64_MAX-0x400||
           !read(host,ctl,vt)||vt!=host.main_base+0x2B858B0||
           !read(host,ctl+0x2AC,cells)||cells<1||cells>21||
           !read(host,ctl+0x2DC,flat)||flat>=cells||
           !host.read_memory(host.userdata,screen+0x386C,mapping.data(),cells*4)||
           mapping[flat]!=fresh.highlighted||std::count(mapping.begin(),mapping.begin()+cells,-1)!=1)return false;
      } else {identity=fresh.items[index].address;value=fresh.items[index].value;}
      initial=fresh.initial;highlight=fresh.highlighted;count=int32_t(fresh.items.size());
      if (index>=0 && (identity>UINT64_MAX-0x38 || !read(host,identity+0x30,actor))) return false;
    }
    uint32_t sequence{},ack{},beat{};
    if (!load(8,sequence) || !load(12,ack) || sequence!=ack || !load(4,beat)) return false;
    if (++sequence==0) ++sequence;
    auto u32=[&](uint32_t at,uint32_t v){return ext.store_u32(ext.userdata,at,v)!=0;};
    auto u64=[&](uint32_t at,uint64_t v){return ext.store_u64(ext.userdata,at,v)!=0;};
    cancel();
    if (!u32(16,0) || !u32(20,uint32_t(selector)) || !u32(24,uint32_t(index)) ||
        !u32(28,uint32_t(initial)) || !u64(32,screen) || !u64(40,identity) ||
        !u64(48,ui.manager) || !u32(56,uint32_t(count)) || !u32(60,uint32_t(highlight)) ||
        !u32(64,beat) || !u32(76,uint32_t(value)) || !u64(80,actor) ||
        !u32(88,epoch) || session_epoch()!=epoch ||
        !scope() || !u32(72,sequence) || !u32(8,sequence)) {cancel();return false;}
    pending=sequence;pending_inventory=false;pending_quest=false;pending_kind=shown;target=identity;cursor_screen=screen;cursor_index=int32_t(index);request_epoch=epoch;requested=now;status="Highlight queued";
    return true;
  }
};
} // namespace botw
