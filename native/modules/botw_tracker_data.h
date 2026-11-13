// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_tracker_flags.h"
#include "botw_tracker_world_metadata.h"
#include "botw_tracker_dlc_locations.h"
#include "botw_tracker_metadata.h"
#include "botw_quests.h"
#include "botw_map_landmarks.h"
#include <cstdio>
#include <cstdlib>
namespace botw {
struct TrackerRecord {
  std::string id,name,actor,status,detail;
  int64_t name_key{-1};int group{},state{-1};
  bool location{};float x{},y{};uint64_t address{};
};
struct TrackerData {
  bool valid{};
  std::array<std::vector<TrackerRecord>,7> categories;
  std::array<std::string,7> summary;
};
inline uint32_t tracker_crc(std::string_view s) {
  uint32_t c=~0u;for(unsigned char ch:s){c^=ch;for(unsigned b=0;b<8;++b)c=(c>>1)^(0xEDB88320u&uint32_t(-int32_t(c&1)));}return ~c;
}
inline std::string tracker_number(int n,int width=3){char s[24]{};std::snprintf(s,sizeof(s),"%0*d",width,n);return s;}
inline int tracker_binary_state(std::optional<bool> b){return b?(*b?2:0):-1;}
inline std::string tracker_yes(std::optional<bool> b){return b?(*b?"yes":"no"):"unknown";}
inline void tracker_unknown(TrackerData& d) {
  d.valid=false;
  for(auto& rows:d.categories)for(auto& r:rows){r.state=-1;r.status="Unavailable";r.detail="Game progress is unavailable";}
  for(auto& s:d.summary)s="Progress unavailable";
}
inline std::string tracker_summary(const std::vector<TrackerRecord>& rows) {
  unsigned complete{},unknown{};for(const auto& r:rows){complete+=r.state==2;unknown+=r.state<0;}
  return std::to_string(complete)+" / "+std::to_string(rows.size())+" complete"+(unknown?" · "+std::to_string(unknown)+" unknown":"");
}
// Pure assembly also serves as a bounded fixture seam. Reader methods return optional,
// so missing flags never become false; caller must finish its stable batch afterward.
template<class Flags> inline TrackerData tracker_data_records(Flags& f,const MapLandmarks& landmarks,
    const QuestSnapshot& quests,std::optional<bool> dlc3,bool sample_valid=true) {
  TrackerData out;out.valid=sample_valid;
  const auto master=f.boolean(0xE51256E2u);
  for(const auto& e:tracker_koroks){TrackerRecord r;r.id=e.id;r.name="Korok "+tracker_number(int(out.categories[0].size()+1));r.actor="Obj_KorokNuts";r.group=0;
    const auto collected=f.boolean(e.flag_hash);r.state=tracker_binary_state(collected);r.status=collected?(*collected?"Collected":"Not collected"):"Unknown";
    r.detail=*e.puzzle?"Puzzle: "+std::string(e.puzzle):"Puzzle type unknown";r.location=true;r.x=e.x;r.y=-e.z;out.categories[0].push_back(std::move(r));}
  auto shrine=[&](int id,const TrackerWorldEntry* e,bool dlc){TrackerRecord r;const auto number=tracker_number(id);r.id="Dungeon"+number;r.name=(dlc?"DLC shrine ":"Shrine ")+number;r.name_key=actor_key(r.id);r.group=dlc?3:1;
    const bool enabled=!dlc||(dlc3&&*dlc3);
    const auto seen=enabled?f.integer(tracker_crc("Location_"+r.id)):std::optional<int32_t>{};
    const auto cleared=enabled?f.boolean(tracker_crc("Clear_"+r.id)):std::optional<bool>{};
    const auto opened=enabled?f.boolean(tracker_crc("Open_"+r.id)):std::optional<bool>{};
    const auto chest=enabled?f.boolean(tracker_crc("CompleteTreasure_"+r.id)):std::optional<bool>{};
    if(cleared&&*cleared){r.state=2;r.status="Completed";}else if(cleared&&seen){r.state=*seen>0?1:0;r.status=*seen>0?"Discovered":"Not discovered";}else r.status="Unknown";
    // Iteration 5 (owner item 1): the detail shares one caption line with the shrine name on the
    // tracker map, so it is kept to what the status line does not already say (seen = the
    // Discovered status; base/DLC = the family). Worst case "Active unknown · All chests unknown".
    r.detail="Active "+tracker_yes(opened)+" · All chests "+tracker_yes(chest);
    if(dlc&&!enabled){r.status=dlc3?"DLC unavailable":"DLC availability unknown";r.detail="Progress unavailable";}
    if(e){r.location=true;r.x=e->x;r.y=-e->z;}
    else if(dlc){const TrackerDlcShrineLocation* match{};unsigned matches{};
      for(const auto& l:tracker_dlc_shrine_locations)if(r.id==l.id){match=&l;++matches;}
      if(matches==1&&std::isfinite(match->x)&&std::isfinite(match->z)){r.location=true;r.x=match->x;r.y=-match->z;if(match->name&&*match->name)r.name=match->name;}}
    out.categories[0].push_back(std::move(r));};
  for(const auto& e:tracker_shrines){const int id=std::atoi(e.id+7);shrine(id,&e,false);}
  // DLC positions use the attributed ID-matched supplement; progress stays edition-gated.
  for(int id=120;id<136;++id)shrine(id,nullptr,true);
  for(const auto& e:tracker_towers){TrackerRecord r;r.id=e.id;r.name="Tower "+std::string(e.id+5);r.name_key=actor_key(r.id);r.group=2;r.location=true;r.x=e.x;r.y=-e.z;
    const MapLandmarks::Entry* match{};unsigned matches{};
    if(sample_valid&&landmarks.valid)for(unsigned i=0;i<landmarks.count&&i<landmarks.entries.size();++i){const auto& l=landmarks.entries[i];
      if(l.kind==3&&std::isfinite(l.xyz[0])&&std::isfinite(l.xyz[1])&&std::isfinite(l.xyz[2])&&std::abs(l.xyz[0]-e.x)<=1&&std::abs(l.xyz[1]-e.y)<=1&&std::abs(l.xyz[2]-e.z)<=1){match=&l;++matches;}}
    if(matches==1){r.state=match->opened?2:0;r.status=match->opened?"Activated":"Not activated";r.detail="Native tower activation";}else {r.status="Unknown";r.detail="Tower activation unavailable";}
    out.categories[0].push_back(std::move(r));}
  for(const auto& e:tracker_bosses){TrackerRecord r;r.id=e.id;r.actor=e.actor;r.name_key=actor_key(r.actor);r.group=r.actor.starts_with("Enemy_Golem")?0:r.actor.starts_with("Enemy_Giant")?1:2;
    r.name=r.group==0?"Stone Talus":r.group==1?(r.actor.find("Bone")!=std::string::npos?"Stalnox":"Hinox"):"Molduga";
    const auto defeated=f.boolean(e.flag_hash);r.state=tracker_binary_state(defeated);r.status=defeated?(*defeated?"Defeated at least once":"Not yet defeated"):"Unknown";r.detail="Can respawn after a Blood Moon";
    r.location=true;r.x=e.x;r.y=-e.z;out.categories[1].push_back(std::move(r));}
  constexpr const char* booknames[]={"Creature","Monster","Material","Equipment","Treasure"};
  for(const auto& e:trackers::compendium_entries){const auto edition=e.edition==1?master:e.edition==2?dlc3:std::optional<bool>(true);
    if(edition&&!*edition)continue;
    TrackerRecord r;r.id=std::string(e.actor);r.actor=r.id;r.name_key=actor_key(r.actor);r.group=e.category;r.name=std::string(booknames[e.category])+" · "+std::to_string(e.sort_key);
    const auto registered=edition?f.boolean(e.flag_hash):std::optional<bool>{};r.state=tracker_binary_state(registered);r.status=registered?(*registered?"Registered":"Not registered"):"Unknown";
    r.detail=std::string(e.edition==1?"Master Mode":e.edition==2?"Champions’ Ballad":"Base game")+" · Photo or purchased entry";
    if(!edition)r.detail+=" · Edition unavailable";
    out.categories[2].push_back(std::move(r));}
  for(const auto& e:tracker_key_items){TrackerRecord r;r.id=e.actor;r.actor=e.actor;r.name_key=actor_key(r.actor);r.group=r.actor.starts_with("GameRomHorse")?1:0;
    r.name=r.actor=="Obj_ProofKorok"?"Hestu’s Gift":r.actor=="Obj_ProofBook"?"Confidential Envelope":r.actor=="Obj_ProofGolemKiller"?"Medal of Honor: Talus":r.actor=="Obj_ProofGiantKiller"?"Medal of Honor: Hinox":r.actor=="Obj_ProofSandwormKiller"?"Medal of Honor: Molduga":r.actor.starts_with("GameRomHorseReins")?"Horse bridle":"Horse saddle";
    const bool enabled=!e.dlc||(dlc3&&*dlc3);const auto acquired=enabled?f.boolean(e.flag_hash):std::optional<bool>{};r.state=tracker_binary_state(acquired);r.status=acquired?(*acquired?"Acquired":"Not acquired"):"Unknown";
    r.detail=std::string(e.dlc?"DLC · ":"")+"Ever acquired · Current pouch presence is separate";if(e.dlc&&!enabled)r.detail="Ancient horse gear · DLC progress unavailable";out.categories[3].push_back(std::move(r));}
  constexpr const char* upgradecats[]={"weapon","bow","shield"};constexpr const char* upgradetitles[]={"Weapon slots","Bow slots","Shield slots"};constexpr int limits[]={11,8,16};
  for(int category=0;category<3;++category){TrackerRecord r;r.id=upgradecats[category];r.name=upgradetitles[category];r.group=category;bool valid=true,gap=false;int bought{},spent{},next{};
    for(const auto& e:tracker_hestu_upgrades)if(std::string_view(e.category)==upgradecats[category]){const auto got=f.observe_boolean(e.flag_hash);if(!got){valid=false;continue;}if(*got){if(gap)valid=false;++bought;spent+=e.cost;}else {gap=true;if(!next)next=e.cost;}}
    if(valid){r.state=bought==limits[category]?2:bought?1:0;r.status=std::to_string(bought)+" / "+std::to_string(limits[category])+" upgrades";r.detail="Spent: "+std::to_string(spent)+" seeds · "+(next?"Next: "+std::to_string(next)+(next==1?" seed":" seeds"):"Fully expanded");}
    else {r.status="Unknown";r.detail="Purchase history is incomplete or inconsistent";}out.categories[4].push_back(std::move(r));}
  if(quests.valid)for(const auto& q:quests.entries){TrackerRecord r;r.id=q.name;r.name=q.name;r.name_key=actor_key(q.name);r.group=q.category;r.address=q.address;
    r.state=q.state<0||q.state>2?-1:!q.revealed?0:q.state==2?2:q.state==1?1:0;r.status=r.state==0?"Not started":r.state==1?"Active":r.state==2?"Completed":"Unknown";
    r.detail=std::string(q.dlc&6u?"DLC · ":"Base game · ")+(q.active?"Selected native quest":r.state==0?"Undiscovered quest · Read-only tracker":"Quest journal progress");out.categories[6].push_back(std::move(r));}
  for(unsigned i=0;i<out.categories.size();++i)out.summary[i]=tracker_summary(out.categories[i]);
  out.summary[0]="900 Koroks · 120 base + 16 DLC shrines · 15 towers";
  out.summary[2]="385 base entries"+(master?(*master?std::string(" + 5 Master Mode"):std::string()):std::string(" · Master Mode unknown"))+(dlc3?(*dlc3?std::string(" + 4 DLC"):std::string()):std::string(" · DLC unknown"));
  out.summary[4]="35 upgrades · 441 seeds total · Seed balance is separate";
  out.summary[5]="Armor planner";if(!quests.valid)out.summary[6]="Quest progress unavailable";
  if(!sample_valid)tracker_unknown(out);
  return out;
}
struct TrackerDlcSample {uint64_t root{},vtable{};uint32_t version{};bool valid{};bool operator==(const TrackerDlcSample&)const=default;};
inline TrackerDlcSample tracker_dlc_sample(const EdenDsmodHostApi& h){TrackerDlcSample s;if(!quests_supported(h)||h.main_size<quest_offsets(h)->dlc_root+8||h.main_base>UINT64_MAX-(quest_offsets(h)->dlc_root+8))return s;
 s.valid=read(h,h.main_base+quest_offsets(h)->dlc_root,s.root)&&s.root>=0x10000&&s.root%8==0&&s.root<=UINT64_MAX-(quest_offsets(h)->dlc_version+4)&&read(h,s.root,s.vtable)&&s.vtable==h.main_base+quest_offsets(h)->dlc_vtable&&read(h,s.root+quest_offsets(h)->dlc_version,s.version)&&s.version<=0xFFFF;return s;}
inline TrackerData tracker_data(const EdenDsmodHostApi& h,const MapLandmarks& landmarks,TrackerFlagReader* cache=nullptr){
 TrackerFlagReader local;auto& flags=cache?*cache:local;const bool ready=flags.begin(h);const auto dlc=ready?tracker_dlc_sample(h):TrackerDlcSample{};
 const auto qs=ready?quests(h,true):QuestSnapshot{};
 auto out=tracker_data_records(flags,landmarks,qs,dlc.valid?std::optional<bool>(dlc.version>=0x300):std::optional<bool>{},ready);
 if(!ready||!flags.finish()||tracker_dlc_sample(h)!=dlc)tracker_unknown(out);
 return out;
}
} // namespace botw
