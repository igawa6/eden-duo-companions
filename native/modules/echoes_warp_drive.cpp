// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_warp_drive.h"
#include <algorithm>
#include <cmath>
namespace echoes {
void WarpPrepareDrive::Abort(WarpPrepareAbort reason){status_=WarpPrepareStatus::Aborted;abort_=reason;}
void WarpPrepareDrive::Cancel(){Abort(WarpPrepareAbort::Cancelled);}
bool WarpPrepareDrive::Source(const MapSnapshot &m,const WarpUnlockSnapshot &u)const {
 return m.compatible&&m.discovery_valid&&m.current_level_valid&&
        m.source==source_&&m.current_level_source==level_source_&&m.current_level_key==raw_level_&&
        u.compatible&&u.known&&u.source==source_&&ordinal_<u.unlocked.size();
}
bool WarpPrepareDrive::Request(const NativeWarpCatalog &catalog,std::string_view set,uint32_t ordinal,
 const ContextSnapshot &c,const MapSnapshot &m,const WarpUnlockSnapshot &u,uint64_t tick,bool interference,const WarpMenuSnapshot *already_open){
 if(status_==WarpPrepareStatus::Pending)return false;
 if(interference){Abort(WarpPrepareAbort::Interference);return false;}
 const auto *native=NativeWarpSetForCurrentLevel(catalog,m.current_level_key);
 const bool native_open=c.context==NativeContext::NativeMenu&&already_open&&already_open->compatible&&already_open->valid&&already_open->state==WarpMapState::Normal&&already_open->map_level=="Field"&&already_open->warp_set==set;
 if(!c.compatible||!c.valid||(c.context!=NativeContext::FieldHud&&!native_open)||c.prologue_hud||
    !m.compatible||!m.discovery_valid||!m.current_level_valid||!m.source.save_epoch.access_object||
    !m.source.save_epoch.save_data||!m.source.traversal||!native||native->key!=set||
    ordinal>=native->destinations.size()||!u.compatible||!u.known||u.source!=m.source||
    u.unlocked.size()!=native->destinations.size()||tick>UINT64_MAX-RequestTimeout){
  Abort(WarpPrepareAbort::InvalidRequest);return false;
 }
 if(u.unlocked[ordinal]!=1){Abort(WarpPrepareAbort::Locked);return false;}
 source_=m.source;level_source_=m.current_level_source;raw_level_=m.current_level_key;
 set_=native->key;flag_=native->destinations[ordinal].unlock_flag;ordinal_=ordinal;
 start_=last_=tick;sent_=focused_seen_=target_seen_=moving_=neutralizing_=direct_sent_=direct_armed_=zoom_sent_=false;zoom_ready_at_=0;map_component_=focused_=0;
 if(native_open){sent_=true;pulse_at_=tick;map_component_=already_open->component;}
 status_=WarpPrepareStatus::Pending;abort_=WarpPrepareAbort::None;return true;
}
WarpPulse WarpPrepareDrive::Advance(const ContextSnapshot &c,const MapSnapshot &m,
 const WarpUnlockSnapshot &u,const WarpMenuSnapshot &menu,uint64_t tick,bool interference,const WarpTargetWrite &direct_target,const WarpZoomWrite &full_zoom){
 if(status_!=WarpPrepareStatus::Pending)return WarpPulse::None;
 if(interference){Abort(WarpPrepareAbort::Interference);return WarpPulse::None;}
 if(tick<last_||tick-last_>MaxClockGap){Abort(WarpPrepareAbort::Clock);return WarpPulse::None;}
 last_=tick;
 if(tick-start_>RequestTimeout){Abort(WarpPrepareAbort::Timeout);return WarpPulse::None;}
 if(!Source(m,u)){Abort(WarpPrepareAbort::SourceChanged);return WarpPulse::None;}
 if(u.unlocked[ordinal_]!=1){Abort(WarpPrepareAbort::Locked);return WarpPulse::None;}
 if(!c.compatible||!c.valid||c.prologue_hud||
    (c.context!=NativeContext::FieldHud&&c.context!=NativeContext::NativeMenu)){
  Abort(WarpPrepareAbort::ContextChanged);return WarpPulse::None;
 }
 if(!sent_){
  if(c.context!=NativeContext::FieldHud||menu.valid){Abort(WarpPrepareAbort::ContextChanged);return WarpPulse::None;}
  sent_=true;pulse_at_=tick;return WarpPulse::Minus;
 }
 if(tick-pulse_at_<SettleTicks)return WarpPulse::None;
 if(!menu.valid){
  if(map_component_){Abort(WarpPrepareAbort::ContextChanged);}
  return WarpPulse::None;
 }
 if(!menu.compatible||c.context!=NativeContext::NativeMenu||menu.state!=WarpMapState::Normal||
    !menu.component||menu.warp_set!=set_){Abort(WarpPrepareAbort::ContextChanged);return WarpPulse::None;}
 if(map_component_&&map_component_!=menu.component){Abort(WarpPrepareAbort::SourceChanged);return WarpPulse::None;}
 map_component_=menu.component;
 if(zoom_sent_&&!full_zoom){Abort(WarpPrepareAbort::Navigation);return WarpPulse::Neutral;}
 if(full_zoom&&!target_seen_) {
  if(!menu.zoom_valid||menu.zoom_scales.empty()) {Abort(WarpPrepareAbort::UnsupportedTarget);return WarpPulse::Neutral;}
  const bool settled=menu.zoom_state==0&&menu.zoom_pending==0xfe&&menu.zoom_index==0&&menu.zoom_previous==0&&
   std::abs(menu.zoom_scale-menu.zoom_scales[0])<=0.0001f;
  if(!settled) {
   zoom_ready_at_=0;
   if(tick-start_>ZoomTimeoutTicks){Abort(WarpPrepareAbort::Navigation);return WarpPulse::Neutral;}
   if(!zoom_sent_&&menu.zoom_state==0&&menu.zoom_pending==0xfe&&menu.zoom_previous==menu.zoom_index) {
    if(!full_zoom(menu)){Abort(WarpPrepareAbort::Navigation);return WarpPulse::Neutral;}
    zoom_sent_=true;
   }
   return WarpPulse::Neutral;
  }
  if(zoom_sent_) {
   if(!zoom_ready_at_)zoom_ready_at_=tick;
   if(tick-zoom_ready_at_<SettleTicks)return WarpPulse::Neutral;
  }
 }
 if(target_seen_) {
  auto current=std::find_if(menu.targets.begin(),menu.targets.end(),[&](const auto &v){return v.ordinal==ordinal_;});
  if(!menu.targets_known||!menu.cursor_known||current==menu.targets.end()||native_view_!=menu.native_view||
     target_component_!=current->component||!current->cursor_known||target_x_!=current->cursor_x||target_y_!=current->cursor_y||!current->native_allowed||!current->selectable) {
   Abort(WarpPrepareAbort::SourceChanged);return WarpPulse::Neutral;
  }
 }
 const bool exact_target_focus=target_seen_ && menu.focus_warp && menu.destination_index==ordinal_ &&
  menu.unlock_flag==flag_ && menu.focused==target_component_ && menu.native_allowed && menu.selectable;
 if(moving_) {
  if(tick<move_until_ && !exact_target_focus)return movement_;
  moving_=false;neutralizing_=true;neutral_until_=tick+SettleTicks;
  return WarpPulse::Neutral;
 }
 if(neutralizing_) {
  if(tick<neutral_until_)return WarpPulse::Neutral;
  neutralizing_=false;
  if(!menu.cursor_known||(!exact_target_focus && std::hypot(menu.cursor_x-origin_x_,menu.cursor_y-origin_y_)<0.25f)) {
   Abort(WarpPrepareAbort::Navigation);return WarpPulse::Neutral;
  }
 }
 // The native input token is installed after this host update. Warm it up
 // before the single cursor store so an earlier zero-stick game update cannot
 // ease the just-written cursor back toward the old focused icon.
 if(direct_armed_) {
  if(!direct_target){Abort(WarpPrepareAbort::Navigation);return WarpPulse::Neutral;}
  if(!direct_sent_&&tick>=move_until_) {
   if(!direct_target(menu,ordinal_)){Abort(WarpPrepareAbort::Navigation);return WarpPulse::Neutral;}
   direct_sent_=true;move_until_=tick+DirectPostTicks;
  }
  if(direct_sent_&&exact_target_focus&&tick<move_until_)move_until_=tick;
  if(tick<move_until_)return WarpPulse::East;
  // Explicit neutral/readback window before any focus may authorize A.
  if(tick<move_until_+SettleTicks)return WarpPulse::Neutral;
 }
 if(!menu.focus_warp||menu.destination_index!=ordinal_||menu.unlock_flag!=flag_){
  if(focused_seen_){Abort(WarpPrepareAbort::UnexpectedFocus);return WarpPulse::Neutral;}
  if(!menu.targets_known||!menu.cursor_known)return WarpPulse::None;
  auto found=std::find_if(menu.targets.begin(),menu.targets.end(),[&](const auto &v){return v.ordinal==ordinal_;});
  if(found==menu.targets.end()||!found->selectable||!found->native_allowed||!found->cursor_known||!menu.native_view) {
   Abort(WarpPrepareAbort::UnsupportedTarget);return WarpPulse::Neutral;
  }
  if(!target_seen_) {
   target_seen_=true;target_component_=found->component;native_view_=menu.native_view;
   target_x_=found->cursor_x;target_y_=found->cursor_y;
  } else if(target_component_!=found->component||native_view_!=menu.native_view||target_x_!=found->cursor_x||target_y_!=found->cursor_y) {
   Abort(WarpPrepareAbort::SourceChanged);return WarpPulse::Neutral;
  }
  if(direct_target) {
   if(!direct_armed_) {
    direct_armed_=true;move_until_=tick+DirectRefreshTicks;
    return WarpPulse::East;
   }
   if(tick>move_until_+SettleTicks*5){Abort(WarpPrepareAbort::Navigation);return WarpPulse::Neutral;}
   return WarpPulse::Neutral; // No write replay or directional traversal.
  }
  const auto dx=target_x_-menu.cursor_x,dy=target_y_-menu.cursor_y;
  if(!std::isfinite(dx)||!std::isfinite(dy)||std::abs(dx)>1e7f||std::abs(dy)>1e7f) {
   Abort(WarpPrepareAbort::Navigation);return WarpPulse::Neutral;
  }
  const int x=std::abs(dx)<=3?0:(dx>0?1:-1),y=std::abs(dy)<=3?0:(dy>0?1:-1);
  if(!x&&!y)return WarpPulse::Neutral; // Wait for exact native identity; never guess closest.
  movement_=y>0?(x>0?WarpPulse::NorthEast:x<0?WarpPulse::NorthWest:WarpPulse::North):
            y<0?(x>0?WarpPulse::SouthEast:x<0?WarpPulse::SouthWest:WarpPulse::South):
            x>0?WarpPulse::East:WarpPulse::West;
  // Repeat the same owned analog2-frame token each host tick to sustain this
  // bounded hold, then explicitly neutralize. Large moves escape native snap;
  // small corrections use shorter holds. Readback must show actual movement.
  const uint64_t hold=std::max(std::abs(dx),std::abs(dy))>24?FarHoldTicks:NearHoldTicks;
  moving_=true;move_until_=tick+hold;origin_x_=menu.cursor_x;origin_y_=menu.cursor_y;
  return movement_;
 }
 if(!menu.focused||!menu.native_allowed||!menu.selectable){Abort(WarpPrepareAbort::NativeDenied);return WarpPulse::None;}
 if(!focused_seen_){focused_seen_=true;focused_=menu.focused;stable_at_=tick;return WarpPulse::None;}
 if(focused_!=menu.focused){Abort(WarpPrepareAbort::UnexpectedFocus);return WarpPulse::None;}
 if(tick-stable_at_>=SettleTicks)status_=WarpPrepareStatus::Ready;
 return WarpPulse::None;
}
} // namespace echoes
namespace echoes {
void WarpTravelDrive::Abort(WarpTravelAbort reason){
 status_=WarpTravelStatus::Aborted;abort_=reason;
 // Preserve the first native preparation failure for production diagnostics.
 // Cancellation is only needed while preparation still owns a pending route.
 if(prepare_.status()!=WarpPrepareStatus::Aborted)prepare_.Cancel();
}
void WarpTravelDrive::Cancel(){Abort(WarpTravelAbort::Cancelled);}
bool WarpTravelDrive::SaveSame(const MapSnapshot &map)const {
 return map.source.save_epoch.access_object==source_.save_epoch.access_object&&
        map.source.save_epoch.save_data==source_.save_epoch.save_data;
}
bool WarpTravelDrive::BeforeCommit(const ContextSnapshot &c,const MapSnapshot &map,
 const WarpUnlockSnapshot &unlocks,const WarpMenuSnapshot &menu)const {
 const auto target=std::find_if(menu.targets.begin(),menu.targets.end(),[&](const auto &v){return v.ordinal==ordinal_;});
 return menu.targets_known&&target!=menu.targets.end()&&target->component==focused_&&
        target->x==target_x_&&target->y==target_y_&&target->selectable&&target->native_allowed&&
        c.compatible&&c.valid&&c.context==NativeContext::NativeMenu&&!c.prologue_hud&&
        map.compatible&&map.discovery_valid&&map.source==source_&&
        unlocks.compatible&&unlocks.known&&unlocks.source==source_&&
        ordinal_<unlocks.unlocked.size()&&unlocks.unlocked[ordinal_]==1&&
        menu.compatible&&menu.valid&&menu.component==menu_component_&&menu.map_level=="Field"&&
        menu.focus_warp&&menu.destination_index==ordinal_&&menu.unlock_flag==destination_.unlock_flag&&
        menu.focused==focused_&&menu.native_allowed&&menu.selectable;
}
bool WarpTravelDrive::Request(const NativeWarpCatalog &catalog,std::string_view set,uint32_t ordinal,
 const ContextSnapshot &c,const MapSnapshot &map,const WarpUnlockSnapshot &unlocks,
 const WarpMenuSnapshot &menu,uint64_t tick,bool interference){
 if(status_==WarpTravelStatus::Pending)return false;
 if(interference){Abort(WarpTravelAbort::Interference);return false;}
 const auto *native=NativeWarpSetForCurrentLevel(catalog,map.current_level_key);
 if(!native||native->key!="Field"||set!="Field"||ordinal>=native->destinations.size()||
    native->destinations[ordinal].level!="Field"||tick>UINT64_MAX-RequestTimeout-ArrivalTimeout){
  Abort(WarpTravelAbort::InvalidRequest);return false;
 }
 if(!prepare_.Request(catalog,set,ordinal,c,map,unlocks,tick,false,
                       c.context==NativeContext::NativeMenu?&menu:nullptr)){
  Abort(WarpTravelAbort::InvalidRequest);return false;
 }
 destination_=native->destinations[ordinal];source_=map.source;ordinal_=ordinal;
 requested_=last_=phase_at_=tick;phase_=Phase::Prepare;
 popup_seen_=execution_seen_=departure_seen_=world_seen_=arrival_seen_=false;
 menu_component_=focused_=popup_=0;initial_world_={};
 status_=WarpTravelStatus::Pending;abort_=WarpTravelAbort::None;return true;
}
WarpPulse WarpTravelDrive::Advance(const ContextSnapshot &c,const MapSnapshot &map,
 const WarpUnlockSnapshot &unlocks,const WarpMenuSnapshot &menu,const WarpChoiceSnapshot &choice,
 const FieldPlayerProjection &player,uint64_t tick,bool interference,const WarpTargetWrite &direct_target,const WarpZoomWrite &full_zoom){
 if(status_!=WarpTravelStatus::Pending)return WarpPulse::None;
 if(interference){Abort(WarpTravelAbort::Interference);return WarpPulse::Neutral;}
 if(tick<last_||tick-last_>MaxClockGap){Abort(WarpTravelAbort::Clock);return WarpPulse::Neutral;}
 last_=tick;
 const bool timed_out=phase_==Phase::WaitCommit?tick-phase_at_>ArrivalTimeout:tick-requested_>RequestTimeout;
 if(timed_out){Abort(WarpTravelAbort::Timeout);return WarpPulse::Neutral;}
 if(c.valid&&c.context==NativeContext::Title){Abort(WarpTravelAbort::ContextChanged);return WarpPulse::Neutral;}
 if(phase_==Phase::Prepare){
  if(player.valid&&player.world.valid&&player.world.source.map_epoch==map.source&&player.world.source.level==map.current_level_source&&player.world.level_key==map.current_level_key){initial_world_=player.world;world_seen_=true;}
  auto pulse=prepare_.Advance(c,map,unlocks,menu,tick,false,direct_target,full_zoom);
  if(prepare_.status()==WarpPrepareStatus::Aborted){Abort(WarpTravelAbort::Preparation);return WarpPulse::Neutral;}
  if(prepare_.status()!=WarpPrepareStatus::Ready)return pulse;
  const auto found=std::find_if(menu.targets.begin(),menu.targets.end(),[&](const auto &v){return v.ordinal==ordinal_;});
  if(!menu.targets_known||found==menu.targets.end()||!found->native_allowed||!found->selectable){Abort(WarpTravelAbort::NativeDenied);return WarpPulse::Neutral;}
  target_x_=found->x;target_y_=found->y;menu_component_=menu.component;focused_=menu.focused;
  if(!BeforeCommit(c,map,unlocks,menu)){Abort(WarpTravelAbort::SourceChanged);return WarpPulse::Neutral;}
  phase_=Phase::WaitPopup;phase_at_=tick;return WarpPulse::A;
 }
 if(phase_==Phase::WaitPopup){
  if(!BeforeCommit(c,map,unlocks,menu)){Abort(WarpTravelAbort::SourceChanged);return WarpPulse::Neutral;}
  if(tick-phase_at_>ReadbackTimeout){Abort(WarpTravelAbort::Timeout);return WarpPulse::Neutral;}
  if(menu.state==WarpMapState::Normal||menu.state==WarpMapState::SelectedIcon)return WarpPulse::None;
  if(menu.state!=WarpMapState::ConfirmWarp){Abort(WarpTravelAbort::ContextChanged);return WarpPulse::Neutral;}
  if(!choice.compatible||!choice.valid)return WarpPulse::None;
  if(choice.menu!=menu_component_||choice.focused!=focused_||choice.destination_index!=ordinal_||
     choice.choice!=NativeWarpChoice::Warp){Abort(WarpTravelAbort::UnexpectedChoice);return WarpPulse::Neutral;}
  if(!popup_seen_){popup_seen_=true;popup_=choice.popup;stable_at_=tick;return WarpPulse::None;}
  if(!choice.popup||choice.popup!=popup_){Abort(WarpTravelAbort::SourceChanged);return WarpPulse::Neutral;}
  if(tick-stable_at_<StableTicks || tick-phase_at_<ButtonPulseFrames+2)return WarpPulse::None;
  if(player.valid&&player.world.valid&&player.world.source.map_epoch==map.source&&player.world.source.level==map.current_level_source&&player.world.level_key==map.current_level_key){initial_world_=player.world;world_seen_=true;}
  // Arrival needs a changed world when the executing state is not sampled. A
  // start already inside the arrival range (plus margin) could land without a
  // detectable change, so that warp is refused instead of timing out later.
  if(world_seen_&&player.world.level_key==destination_.level&&
     std::hypot(player.native_layout[0]-target_x_,player.native_layout[1]-target_y_)<=SameSpotLayout){
   Abort(WarpTravelAbort::AlreadyThere);return WarpPulse::Neutral;}
  phase_=Phase::WaitCommit;phase_at_=tick;return WarpPulse::A;
 }
 // After the one guarded affirmative pulse, no further inputs are sent.
 // Native loading/context unknowns are expected here. Never replay confirm.
 if(map.compatible&&map.discovery_valid&&!SaveSame(map)){Abort(WarpTravelAbort::SourceChanged);return WarpPulse::Neutral;}
 if(menu.valid){
  if(menu.component!=menu_component_){Abort(WarpTravelAbort::SourceChanged);return WarpPulse::Neutral;}
  if(menu.state==WarpMapState::WarpExecuting){execution_seen_=true;}
  else if(menu.state==WarpMapState::Normal){Abort(WarpTravelAbort::Postcondition);return WarpPulse::Neutral;}
  else if(menu.state!=WarpMapState::ConfirmWarp&&menu.state!=WarpMapState::SelectedIcon){Abort(WarpTravelAbort::ContextChanged);return WarpPulse::Neutral;}
  return WarpPulse::None;
 }
 departure_seen_=true;
 const bool arrived=c.compatible&&c.valid&&c.context==NativeContext::FieldHud&&!c.prologue_hud&&
  map.compatible&&map.discovery_valid&&SaveSame(map)&&player.valid&&player.world.valid&&
  player.world.source.map_epoch==map.source&&player.world.source.level==map.current_level_source&&
  player.world.level_key==map.current_level_key&&player.world.level_key==destination_.level&&
  std::hypot(player.native_layout[0]-target_x_,player.native_layout[1]-target_y_)<=ArrivalLayout;
 if(!arrived){arrival_seen_=false;return WarpPulse::None;}
 bool changed=world_seen_&&(initial_world_.level_key!=player.world.level_key);
 if(world_seen_)for(size_t i=0;i<3;++i)changed|=std::abs(initial_world_.xyz[i]-player.world.xyz[i])>0.25f;
 // Some native execution states last less than a host observation. A changed
 // authenticated world at the exact original target is the alternate proof;
 // merely closing the map with unchanged coordinates cannot complete travel.
 if(!execution_seen_&&(!departure_seen_||!changed)){arrival_seen_=false;return WarpPulse::None;}
 if(!arrival_seen_){arrival_seen_=true;stable_at_=tick;arrival_x_=player.native_layout[0];arrival_y_=player.native_layout[1];return WarpPulse::None;}
 if(std::hypot(player.native_layout[0]-arrival_x_,player.native_layout[1]-arrival_y_)>1.0f){arrival_seen_=false;return WarpPulse::None;}
 if(tick-stable_at_>=StableTicks){status_=WarpTravelStatus::Complete;return WarpPulse::Neutral;}
 return WarpPulse::None;
}
} // namespace echoes
