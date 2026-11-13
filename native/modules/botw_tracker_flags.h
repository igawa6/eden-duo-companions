// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_hud.h"
#include <optional>
#include <unordered_map>
#include <vector>
namespace botw {
// Bounded read-only exact1.6 GameData view. Missing/inaccessible is unknown,
// never false. Cached lookup identities are revalidated on every sample.
class TrackerFlagReader {
  struct Roots {
    uint64_t manager{},vt{},holder{},bank{},bools{},ints{};
    int32_t bool_count{},bool_cap{},int_count{},int_cap{};
    uint32_t mode{},generation{};uint8_t permission{};
    bool operator==(const Roots&)const=default;
  } root{},previous{};
  struct BoolFlag {
    uint64_t vt{};uint8_t type{},properties{},config{},value{};uint32_t hash{};
    bool operator==(const BoolFlag&)const=default;
  };
  struct IntFlag {
    uint64_t vt{};uint8_t type{},properties{};std::array<uint8_t,2> pad{};
    int32_t initial{},minimum{},maximum{},value{};uint32_t hash{};uint64_t debug{};
    bool operator==(const IntFlag&)const=default;
  };
  static_assert(sizeof(BoolFlag)==16&&sizeof(IntFlag)==40);
  struct Identity {int32_t index{};uint64_t pointer{};};
  struct Observation {uint32_t hash{};bool integer{};Identity id;BoolFlag b;IntFlag n;};
  const EdenDsmodHostApi* host{};UiSample ui{};bool valid_{};
  std::unordered_map<uint64_t,Identity> cache;
  std::vector<Observation> observations;
  static bool ptr(uint64_t p,uint64_t n){return p>=0x10000&&p%8==0&&p<=UINT64_MAX-n;}
  bool roots(Roots& r)const {
    const auto& h=*host;
    return read(h,h.main_base+inventory_offsets(h).gamedata_root,r.manager)&&ptr(r.manager,0xC2C)&&
      read(h,r.manager,r.vt)&&r.vt==h.main_base+inventory_offsets(h).gamedata_vtable&&
      read(h,r.manager+0xBE0,r.holder)&&ptr(r.holder,8)&&read(h,r.holder,r.bank)&&ptr(r.bank,0x28)&&
      read(h,r.bank+8,r.bool_count)&&read(h,r.bank+12,r.bool_cap)&&
      r.bool_count>0&&r.bool_count<=r.bool_cap&&r.bool_cap<=65536&&
      read(h,r.bank+16,r.bools)&&ptr(r.bools,uint64_t(r.bool_count)*8)&&
      read(h,r.bank+24,r.int_count)&&read(h,r.bank+28,r.int_cap)&&
      r.int_count>0&&r.int_count<=r.int_cap&&r.int_cap<=65536&&
      read(h,r.bank+32,r.ints)&&ptr(r.ints,uint64_t(r.int_count)*8)&&
      read(h,r.manager+0xBE8,r.permission)&&r.permission<=1&&
      read(h,r.manager+0xC18,r.mode)&&read(h,r.manager+0xC28,r.generation)&&r.generation<=255;
  }
  template<class F> bool entry(bool integer,int32_t index,Identity& id,F& f)const {
    const auto count=integer?root.int_count:root.bool_count;
    return index>=0&&index<count&&read(*host,(integer?root.ints:root.bools)+uint64_t(index)*8,id.pointer)&&
      ptr(id.pointer,sizeof(F))&&read(*host,id.pointer,f)&&
      f.vt==host->main_base+(integer?inventory_offsets(*host).int_flag_vtable:inventory_offsets(*host).bool_flag_vtable)&&f.type==unsigned(integer)&&((id.index=index),true);
  }
  template<class F> bool resolve(uint32_t hash,bool integer,Identity& id,F& f) {
    if(!valid_||observations.size()>=4096)return false;
    const uint64_t key=(uint64_t(integer)<<32)|hash;
    if(const auto found=cache.find(key);found!=cache.end()) {
      Identity current{};
      if(entry(integer,found->second.index,current,f)&&current.pointer==found->second.pointer&&f.hash==hash){id=current;return true;}
      cache.erase(found);
    }
    int32_t lo=0,hi=integer?root.int_count:root.bool_count;
    for(unsigned step=0;lo<hi&&step<17;++step){const auto mid=lo+(hi-lo)/2;if(!entry(integer,mid,id,f))return false;if(f.hash<hash)lo=mid+1;else hi=mid;}
    if(lo!=hi||!entry(integer,lo,id,f)||f.hash!=hash)return false;
    if(cache.size()>=4096) cache.clear();
    cache[key]=id;return true;
  }
public:
  void clear(){cache.clear();observations.clear();root={};previous={};host=nullptr;valid_=false;}
  bool begin(const EdenDsmodHostApi& h,bool inventory=false){
    host=&h;valid_=false;observations.clear();Roots now{};
    if(!(inventory?inventory_supported(h):supported(h))||h.main_size<inventory_offsets(h).gamedata_root+8||h.main_base>UINT64_MAX-(inventory_offsets(h).gamedata_root+8)||
       !ui_sample(h,ui)||!hud_context(ui)||!roots(now)){cache.clear();return false;}
    if(now!=previous) cache.clear();
    root=previous=now;valid_=true;return true;
  }
  std::optional<bool> boolean(uint32_t hash){
    Identity id{};BoolFlag f{};
    if(!resolve(hash,false,id,f)||(root.permission&&!(f.properties&1)))return {};
    observations.push_back({hash,false,id,f,{}});return bool(f.value&1);
  }
  // Read-only observation for catalogued event flags. Native CheckFlag uses
  // getParamBypassPerm().get().getBool; IsProgramReadable is not its gate.
  // This never invokes a guest accessor or writes a flag. Exact identity,
  // mapping, value and manager stability checks still apply at finish().
  std::optional<bool> observe_boolean(uint32_t hash){
    Identity id{};BoolFlag f{};
    if(!resolve(hash,false,id,f))return {};
    observations.push_back({hash,false,id,f,{}});return bool(f.value&1);
  }
  std::optional<int32_t> integer(uint32_t hash){
    Identity id{};IntFlag f{};
    if(!resolve(hash,true,id,f)||(root.permission&&!(f.properties&1))||f.minimum>f.maximum||f.value<f.minimum||f.value>f.maximum)return {};
    observations.push_back({hash,true,id,{},f});return f.value;
  }
  // Fix round A: read-only observation of a catalogued integer flag without the program-read
  // permission gate (the game's own getFlag_* accessors read it the same way). Bounds still apply.
  std::optional<int32_t> observe_integer(uint32_t hash){
    Identity id{};IntFlag f{};
    if(!resolve(hash,true,id,f)||f.minimum>f.maximum||f.value<f.minimum||f.value>f.maximum)return {};
    observations.push_back({hash,true,id,{},f});return f.value;
  }
  bool finish(){
    if(!valid_)return false;
    for(const auto& o:observations){Identity id{};if(o.integer){IntFlag f{};if(!entry(true,o.id.index,id,f)||id.pointer!=o.id.pointer||f!=o.n){valid_=false;break;}}
      else{BoolFlag f{};if(!entry(false,o.id.index,id,f)||id.pointer!=o.id.pointer||f!=o.b){valid_=false;break;}}}
    Roots after{};UiSample last{};
    valid_=valid_&&roots(after)&&after==root&&ui_sample(*host,last)&&last==ui;
    if(!valid_) cache.clear();
    return valid_;
  }
};
}
