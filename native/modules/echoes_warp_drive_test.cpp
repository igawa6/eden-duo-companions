// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_warp_drive.h"
#include <cstdlib>
#include <iostream>
using namespace echoes;
namespace {
size_t checks{};
void Check(bool v,const char*s){++checks;if(!v){std::cerr<<s<<'\n';std::abort();}}
struct Fixture {
 NativeWarpCatalog catalog{{{"Field",{{"Field","Point","Warp_Test",0}}}},{{"Field","Field"}}, {}};
 ContextSnapshot context;
 MapSnapshot map;
 WarpUnlockSnapshot unlock;
 WarpMenuSnapshot menu;
 WarpPrepareDrive drive;
 Fixture(){
  context.compatible=context.valid=true;context.context=NativeContext::FieldHud;
  map.compatible=map.discovery_valid=map.current_level_valid=true;
  map.source.save_epoch.access_object=1;map.source.save_epoch.save_data=2;map.source.traversal=3;
  map.current_level_key="Field";map.current_level_source.controller=4;
  unlock.compatible=unlock.known=true;unlock.source=map.source;unlock.unlocked={1};
 }
 bool Request(uint64_t t=0){return drive.Request(catalog,"Field",0,context,map,unlock,t);}
 WarpPulse Step(uint64_t t,bool physical=false){return drive.Advance(context,map,unlock,menu,t,physical);}
 void Open(){context.context=NativeContext::NativeMenu;menu.compatible=menu.valid=true;menu.component=5;menu.warp_set="Field";menu.map_level="Field";}
 void Focus(){menu.focus_warp=menu.selectable=menu.native_allowed=true;menu.focused=6;menu.unlock_flag="Warp_Test";}
};
}
int main(){
 Fixture f;Check(f.Request(),"valid request");Check(f.Step(0)==WarpPulse::Minus,"one native Minus pulse");
 Check(f.Step(1)==WarpPulse::None,"no repeated input");f.Open();f.Focus();f.Step(6);f.Step(12);
 Check(f.drive.status()==WarpPrepareStatus::Ready,"stable exact native focus prepares only");
 Check(f.Step(13)==WarpPulse::None,"ready emits no A or travel");
 for(int mode=0;mode<9;++mode){Fixture t;
  if(mode==0)t.unlock.unlocked[0]=0;
  if(mode==1)t.context.context=NativeContext::Unknown;
  if(mode==2)t.context.prologue_hud=true;
  if(mode==3)t.map.current_level_valid=false;
  if(mode==4)t.map.source.save_epoch.save_data=0;
  if(mode==5)t.unlock.source.traversal++;
  if(mode==6)t.catalog.level_sets.clear();
  if(mode==7)t.unlock.unlocked.clear();
  if(mode==8){t.context.context=NativeContext::Unknown;t.context.field_presentation=true;}
  Check(!t.Request(),"invalid request refuses before input");Check(t.Step(0)==WarpPulse::None,"rejected emits none");
 }
 for(int mode=0;mode<7;++mode){Fixture t;Check(t.Request(),"abort case starts");t.Step(0);
  if(mode==0)t.context.context=NativeContext::Unknown;
  if(mode==1)t.unlock.known=false;
  if(mode==2)t.unlock.unlocked[0]=0;
  if(mode==3)t.map.current_level_key="Other";
  if(mode==4)t.map.current_level_source.controller++;
  t.Step(mode==5?61:6,mode==6);
  Check(t.drive.status()==WarpPrepareStatus::Aborted,"changes abort");Check(t.Step(7)==WarpPulse::None,"abort never replay");
 }
 Fixture t;t.Request();t.Step(0);t.Open();t.Focus();t.Step(6);t.menu.unlock_flag="Warp_Other";t.Step(12);
 Check(t.drive.abort_reason()==WarpPrepareAbort::UnexpectedFocus,"changed flag aborts");
 t=Fixture{};t.Request();t.Step(0);t.Open();t.Focus();t.menu.native_allowed=false;t.Step(6);
 Check(t.drive.abort_reason()==WarpPrepareAbort::NativeDenied,"native denial aborts");
 t=Fixture{};t.Request();t.Step(0);t.Open();t.Step(6);for(uint64_t i=60;i<=900;i+=60)t.Step(i);
 Check(t.drive.abort_reason()==WarpPrepareAbort::Timeout,"no focus times out without guessing navigation");
 t=Fixture{};t.Request();t.drive.Cancel();Check(t.Step(0)==WarpPulse::None,"cancel neutral");
 t=Fixture{};t.Request();t.Step(10);t.Step(9);Check(t.drive.abort_reason()==WarpPrepareAbort::Clock,"clock rollback");
 t=Fixture{};t.Request();t.Step(0);t.Open();t.menu.targets_known=t.menu.cursor_known=true;t.menu.native_view=7;
 t.menu.targets={{8,0,30,-30,true,true,30,-30,true}};
 Check(t.Step(6)==WarpPulse::SouthEast,"exact authored target selects analog direction");
 Check(t.Step(10)==WarpPulse::SouthEast,"bounded continuous hold escapes native snap");
 t.Focus();t.menu.destination_index=1;t.menu.focused=9;
 Check(t.Step(15)==WarpPulse::SouthEast,"another native waypoint never stops requested navigation");
 Check(t.Step(18)==WarpPulse::SouthEast,"far hold does not release inside native snap threshold");
 t.menu.cursor_x=20;t.menu.cursor_y=-20;t.Focus();t.menu.destination_index=0;t.menu.focused=8;
 Check(t.Step(20)==WarpPulse::Neutral,"exact requested native focus stops long hold early");t.Step(26);t.Step(32);
 Check(t.drive.status()==WarpPrepareStatus::Ready,"observed movement then exact native destination focus");
 t=Fixture{};t.Request();t.Step(0);t.Open();t.menu.targets_known=t.menu.cursor_known=true;t.menu.native_view=7;
 t.menu.targets={{8,0,30,-30,true,true,30,-30,true}};t.Step(6);
 Check(t.Step(65)==WarpPulse::SouthEast,"far hold remains bounded at60ticks");
 Check(t.Step(66)==WarpPulse::Neutral,"far hold deadline explicitly neutralizes");t.Step(72);
 Check(t.drive.abort_reason()==WarpPrepareAbort::Navigation,"no native movement refuses replay");
 t=Fixture{};t.Request();t.Step(0);t.Open();t.menu.targets_known=t.menu.cursor_known=true;t.menu.native_view=7;
 t.menu.targets={{8,0,30,-30,true,true,30,-30,true}};t.Step(6);t.menu.targets[0].component=9;t.Step(7);
 Check(t.drive.abort_reason()==WarpPrepareAbort::SourceChanged,"target source changes abort even while moving");
 t=Fixture{};t.Request();t.Step(0);t.Open();t.menu.targets_known=t.menu.cursor_known=true;t.menu.native_view=7;
 t.menu.targets={{8,0,20,-20,true,true,20,-20,true}};
 Check(t.Step(6)==WarpPulse::SouthEast,"near target begins bounded correction");
 Check(t.Step(17)==WarpPulse::SouthEast,"near correction holds12ticks");
 Check(t.Step(18)==WarpPulse::Neutral,"near correction releases at12tick deadline");
 t.Step(24);Check(t.drive.abort_reason()==WarpPrepareAbort::Navigation,"unchanged near cursor still aborts without replay");
 // Direct route writes once, refreshes once and requires native identity.
 Fixture jump;jump.Request();jump.Step(0);jump.Open();
 jump.menu.targets_known=jump.menu.cursor_known=true;jump.menu.native_view=7;
 jump.menu.targets={{8,0,300,-300,true,true,300,-300,true}};
 size_t jumps{};WarpTargetWrite set=[&](const WarpMenuSnapshot &observed,uint32_t ordinal){
  ++jumps;return observed==jump.menu&&ordinal==0;};
 auto direct=[&](uint64_t tick){return jump.drive.Advance(jump.context,jump.map,jump.unlock,jump.menu,tick,false,set);};
 Check(direct(6)==WarpPulse::East&&jumps==0,"native refresh begins before any cursor write");
 Check(direct(13)==WarpPulse::East&&jumps==0,"bounded warmup still has no cursor write");
 Check(direct(14)==WarpPulse::East&&jumps==1,"one cursor write occurs with native refresh already active");
 jump.Focus();jump.menu.focused=8;jump.menu.cursor_x=300;jump.menu.cursor_y=-300;
 Check(direct(15)==WarpPulse::Neutral&&jumps==1,"exact native target stops post-write refresh early");
 Check(direct(16)==WarpPulse::Neutral&&jumps==1,"refresh neutralizes after bounded post-write hold");
 Check(direct(21)==WarpPulse::None,"exact native target begins stability window after neutral");direct(27);
 Check(jump.drive.status()==WarpPrepareStatus::Ready&&jumps==1,"exact native target acknowledges single write");
 Fixture denied;denied.Request();denied.Step(0);denied.Open();denied.menu.targets_known=denied.menu.cursor_known=true;denied.menu.native_view=7;
 denied.menu.targets={{8,0,300,-300,true,true,300,-300,true}};
 WarpTargetWrite refuse=[](const WarpMenuSnapshot &,uint32_t){return false;};
 denied.drive.Advance(denied.context,denied.map,denied.unlock,denied.menu,6,false,refuse);
 Check(denied.drive.Advance(denied.context,denied.map,denied.unlock,denied.menu,14,false,refuse)==WarpPulse::Neutral&&denied.drive.abort_reason()==WarpPrepareAbort::Navigation,"failed direct write aborts without fallback");
 Fixture missed;missed.Request();missed.Step(0);missed.Open();missed.menu=denied.menu;
 size_t attempts{};WarpTargetWrite ack=[&](const WarpMenuSnapshot &,uint32_t){++attempts;return true;};
 missed.drive.Advance(missed.context,missed.map,missed.unlock,missed.menu,6,false,ack);
 missed.drive.Advance(missed.context,missed.map,missed.unlock,missed.menu,14,false,ack);
 Check(missed.drive.Advance(missed.context,missed.map,missed.unlock,missed.menu,47,false,ack)==WarpPulse::Neutral&&missed.drive.abort_reason()==WarpPrepareAbort::Navigation&&attempts==1,"missing native focus aborts without replay");
 Fixture warmed;warmed.Request();warmed.Step(0);warmed.Open();warmed.menu=denied.menu;
 size_t warmed_writes{};WarpTargetWrite warm_write=[&](const WarpMenuSnapshot &,uint32_t){++warmed_writes;return true;};
 warmed.drive.Advance(warmed.context,warmed.map,warmed.unlock,warmed.menu,6,false,warm_write);
 warmed.menu.targets[0].component++;
 Check(warmed.drive.Advance(warmed.context,warmed.map,warmed.unlock,warmed.menu,14,false,warm_write)==WarpPulse::Neutral&&warmed.drive.abort_reason()==WarpPrepareAbort::SourceChanged&&warmed_writes==0,"changed native target during warmup refuses cursor write");
 Fixture physical;physical.Request();physical.Step(0);physical.Open();physical.menu=denied.menu;
 physical.drive.Advance(physical.context,physical.map,physical.unlock,physical.menu,6,false,warm_write);
 Check(physical.drive.Advance(physical.context,physical.map,physical.unlock,physical.menu,14,true,warm_write)==WarpPulse::None&&physical.drive.abort_reason()==WarpPrepareAbort::Interference&&warmed_writes==0,"interference during warmup cancels before cursor write");
 Fixture zoomed;zoomed.Request();zoomed.Step(0);zoomed.Open();zoomed.menu=denied.menu;
 zoomed.menu.zoom_valid=true;zoomed.menu.zoom_scales={1.f,2.f,3.f};zoomed.menu.zoom_index=zoomed.menu.zoom_previous=2;
 zoomed.menu.zoom_pending=0xfe;zoomed.menu.zoom_scale=3.f;
 size_t zoom_requests{},target_writes{};WarpZoomWrite zoom_request=[&](const WarpMenuSnapshot&){++zoom_requests;return true;};
 WarpTargetWrite target_request=[&](const WarpMenuSnapshot& observed,uint32_t){++target_writes;return observed.zoom_index==0&&observed.zoom_scale==1.f;};
 auto zoom_tick=[&](uint64_t tick){return zoomed.drive.Advance(zoomed.context,zoomed.map,zoomed.unlock,zoomed.menu,tick,false,target_request,zoom_request);};
 Check(zoom_tick(6)==WarpPulse::Neutral&&zoom_requests==1&&target_writes==0,"full zoom queues once before any cursor write");
 zoomed.menu.zoom_index=0;zoomed.menu.zoom_pending=1;zoom_tick(7);
 zoomed.menu.zoom_state=1;zoomed.menu.zoom_pending=0xfe;zoomed.menu.zoom_scale=2.f;zoom_tick(14);
 Check(zoom_requests==1&&target_writes==0,"native zoom transition completes without replay or cursor capture");
 zoomed.menu.targets[0].cursor_y=-200;zoomed.menu.zoom_state=0;zoomed.menu.zoom_previous=0;zoomed.menu.zoom_scale=1.f;
 Check(zoom_tick(20)==WarpPulse::Neutral&&target_writes==0,"native full zoom requires neutral settled window");
 Check(zoom_tick(26)==WarpPulse::East&&target_writes==0,"only settled full zoom starts native input warmup");
 Check(zoom_tick(34)==WarpPulse::East&&target_writes==1&&zoom_requests==1,"one cursor jump follows full zoom and input warmup");
 // Full native route: already-open map, two distinct guarded A pulses,
 // original Warp choice readback, loading, changed world at exact target.
 struct TravelFixture : Fixture {
  WarpTravelDrive travel;WarpChoiceSnapshot choice;FieldPlayerProjection player;
  uint64_t now{};
  TravelFixture(){Open();Focus();menu.targets_known=menu.cursor_known=true;menu.native_view=7;menu.targets={{6,0,30,-30,true,true,30,-30,true}};
   catalog.level_aliases.emplace_back("DekuTreeArea","Field");map.current_level_key="DekuTreeArea";
   World("DekuTreeArea",0,0,0,0,0);
  }
  void World(std::string level,float x,float y,float z,float nx,float ny){
   map.current_level_key=level;player.valid=player.world.valid=true;player.world.level_key=level;player.world.xyz={x,y,z};
   player.world.source.map_epoch=map.source;player.world.source.level=map.current_level_source;player.native_layout={nx,ny};
  }
  bool Start(){return travel.Request(catalog,"Field",0,context,map,unlock,menu,0);}
  WarpPulse Tick(uint64_t tick,bool physical=false){now=tick;return travel.Advance(context,map,unlock,menu,choice,player,tick,physical);}
  void Popup(){menu.state=WarpMapState::ConfirmWarp;choice.compatible=choice.valid=true;choice.menu=menu.component;choice.focused=menu.focused;choice.popup=9;choice.destination_index=0;choice.choice=NativeWarpChoice::Warp;}
  void Prepared(){Check(Start(),"travel accepts exact already-open native map");Tick(6);Check(Tick(12)==WarpPulse::A,"one A opens exact native target popup");Popup();Tick(18);Check(Tick(24)==WarpPulse::None,"popup cannot confirm during short stability window");Check(Tick(29)==WarpPulse::None,"button release gap is not rounded down");Check(Tick(30)==WarpPulse::A,"one A confirms authenticated stable Warp choice after button release");}
 };
 TravelFixture full;full.Prepared();full.menu.valid=false;full.context.context=NativeContext::Unknown;full.Tick(36);
 Check(full.travel.status()==WarpTravelStatus::Pending,"loading unknown sends no replay");
 full.context.context=NativeContext::FieldHud;full.World("Field",210,10,89,30,-32);full.Tick(42);Check(full.Tick(54)==WarpPulse::Neutral&&full.travel.status()==WarpTravelStatus::Complete,"changed authenticated exact-target world completes");
 TravelFixture cancelled;cancelled.Start();cancelled.Tick(6);cancelled.Tick(12);cancelled.Popup();cancelled.choice.choice=NativeWarpChoice::Cancel;cancelled.Tick(18);
 Check(cancelled.travel.abort_reason()==WarpTravelAbort::UnexpectedChoice,"Cancel selection never confirmed");
 TravelFixture wrong;wrong.Prepared();wrong.menu.valid=false;wrong.context.context=NativeContext::FieldHud;wrong.World("Field",210,10,89,300,300);wrong.Tick(36);
 Check(wrong.travel.status()==WarpTravelStatus::Pending,"wrong destination coordinates never complete");
 TravelFixture unknown;unknown.Prepared();unknown.menu.valid=false;unknown.context.context=NativeContext::Unknown;unknown.World("Field",210,10,89,30,-30);unknown.Tick(36);
 Check(unknown.travel.status()==WarpTravelStatus::Pending,"position alone does not defeat unknown gameplay context");
 // A start inside the arrival range could land with no detectable change, so the
 // confirm is never pressed: refused with neutral input instead of a late timeout.
 TravelFixture same;same.map.current_level_key="Field";same.World("Field",0,0,0,30,-30);
 Check(same.Start(),"same-point fixture starts");same.Tick(6);same.Tick(12);same.Popup();same.Tick(18);same.Tick(24);same.Tick(29);
 Check(same.Tick(30)==WarpPulse::Neutral&&same.travel.abort_reason()==WarpTravelAbort::AlreadyThere,"warp to the current spot is refused before confirm");
 TravelFixture margin;margin.map.current_level_key="Field";margin.World("Field",0,0,0,37,-30);
 Check(margin.Start(),"margin fixture starts");margin.Tick(6);margin.Tick(12);margin.Popup();margin.Tick(18);margin.Tick(24);margin.Tick(29);
 Check(margin.Tick(30)==WarpPulse::Neutral&&margin.travel.abort_reason()==WarpTravelAbort::AlreadyThere,"start within the same-spot margin is refused too");
 TravelFixture executed;executed.map.current_level_key="Field";executed.World("Field",0,0,0,40,-30);executed.Prepared();executed.menu.state=WarpMapState::WarpExecuting;executed.Tick(36);executed.menu.valid=false;executed.context.context=NativeContext::FieldHud;executed.World("Field",0,0,0,30,-30);executed.Tick(42);executed.Tick(54);
 Check(executed.travel.status()==WarpTravelStatus::Complete,"observed native execution permits a nearby short warp");
 TravelFixture changed;changed.Prepared();changed.menu.valid=false;changed.context.context=NativeContext::FieldHud;changed.map.source.save_epoch.save_data++;changed.Tick(36);
 Check(changed.travel.abort_reason()==WarpTravelAbort::SourceChanged,"different save aborts after confirmation");
 TravelFixture interference;interference.Start();Check(interference.Tick(6,true)==WarpPulse::Neutral&&interference.travel.abort_reason()==WarpTravelAbort::Interference,"physical input aborts and neutralizes");
 TravelFixture stopped;stopped.menu.focus_warp=false;Check(stopped.Start(),"navigation failure fixture starts");
 stopped.Tick(6);stopped.Tick(66);stopped.Tick(72);
 Check(stopped.travel.abort_reason()==WarpTravelAbort::Preparation,"native navigation abort propagates through travel");
 Check(stopped.travel.preparation_abort()==WarpPrepareAbort::Navigation,"travel abort preserves original navigation reason");
 stopped.travel.Cancel();
 Check(stopped.travel.preparation_abort()==WarpPrepareAbort::Navigation,"later cancellation does not erase native preparation failure");
 TravelFixture source_failure;source_failure.menu.focus_warp=false;source_failure.Start();source_failure.Tick(6);source_failure.menu.native_view++;
 source_failure.Tick(7);
 Check(source_failure.travel.abort_reason()==WarpTravelAbort::Preparation && source_failure.travel.preparation_abort()==WarpPrepareAbort::SourceChanged,"travel retains native target source failure instead of Cancelled");
 TravelFixture delayed;delayed.Prepared();delayed.menu.valid=false;delayed.context.context=NativeContext::Unknown;
 delayed.World("Field",210,10,89,30,-30);
 bool no_replay=true;for(uint64_t tick=60;tick<=1620;tick+=60)no_replay&=delayed.Tick(tick)==WarpPulse::None;
 Check(no_replay && delayed.travel.status()==WarpTravelStatus::Pending,"post-confirm unknown remains pending beyond old global deadline without input replay");
 delayed.context.context=NativeContext::FieldHud;delayed.Tick(1632);
 Check(delayed.Tick(1644)==WarpPulse::Neutral && delayed.travel.status()==WarpTravelStatus::Complete,"late authenticated stable arrival completes within independent arrival budget");
 TravelFixture expired;expired.Prepared();expired.menu.valid=false;expired.context.context=NativeContext::Unknown;
 bool stayed_pending=true;for(uint64_t tick=60;tick<=3600;tick+=60)stayed_pending&=expired.Tick(tick)==WarpPulse::None && expired.travel.status()==WarpTravelStatus::Pending;
 Check(stayed_pending && expired.Tick(3630)==WarpPulse::None && expired.travel.status()==WarpTravelStatus::Pending,"arrival deadline is measured from final confirm at30 rather than request at0");
 Check(expired.Tick(3631)==WarpPulse::Neutral && expired.travel.abort_reason()==WarpTravelAbort::Timeout,"unqualified HUD eventually times out at bounded post-confirm deadline");
 TravelFixture other;other.catalog.sets[0].destinations[0].level="S_Dungeon018";Check(!other.Start(),"unqualified interior travel destination refuses");
 std::cout<<checks<<" native warp preparation/travel state-machine checks passed\n";
}
