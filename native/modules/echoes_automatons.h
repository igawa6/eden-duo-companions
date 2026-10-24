// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_code_guard.h"
#include "echoes_selector.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string>

namespace echoes {
// Original mode0 ActorSelect list, distinct from the mode1 Echo list. The
// original six-entry ID table at main+293abd8 is also used by the ownership
// mask loop at26dab4c. Names/art remain runtime ROMFS resources.
inline constexpr std::array<uint16_t,6> AutomatonIds{1184,853,1114,239,826,160};
inline bool IsAutomaton(uint16_t id) {
  return std::find(AutomatonIds.begin(),AutomatonIds.end(),id)!=AutomatonIds.end();
}
struct AutomatonSource {
  uint64_t root{},buffer{},data{};
  bool operator==(const AutomatonSource&)const=default;
};
struct AutomatonSnapshot {
  bool compatible{}, ownership_valid{};
  uint16_t ownership_mask{};
  AutomatonSource source;
  std::vector<uint16_t> owned_native_ids;
  SelectorSnapshot selector;
};
namespace automaton_detail {
inline uint64_t Le(const uint8_t* p,size_t n) {
  uint64_t v{};for(size_t i=0;i<n;++i)v|=uint64_t(p[i])<<(i*8);return v;
}
inline bool Bytes(const ReadFn& r,uint64_t p,void* out,size_t n) {
  return r && p && p<=std::numeric_limits<uint64_t>::max()-n && r(p,out,n);
}
inline bool Num(const ReadFn& r,uint64_t p,uint64_t& v,size_t n=8) {
  std::array<uint8_t,8>b{};if(!Bytes(r,p,b.data(),n))return false;
  v=Le(b.data(),n);return true;
}
inline bool Pointer(const ReadFn& r,uint64_t p,uint64_t& v) {
  return Num(r,p,v) && v && v%8==0 && v<std::numeric_limits<uint64_t>::max()-0x18000;
}
template<size_t N> inline bool Guard(const ReadFn& r,uint64_t base,uint64_t off,const std::array<uint8_t,N>& original) {
  std::array<uint8_t,N>b{};
  return Bytes(r,base+off,b.data(),N) && MatchSupportedGuard(b,original,off);
}
inline bool Guards(const ReadFn& r,uint64_t base) {
  return Guard(r,base,0x26da938,std::array<uint8_t,20>{0x08,0x78,0xb2,0xb9,0x08,0xe5,0x7a,0xd3,0x09,0x30,0x59,0xf9,0x20,0x69,0x68,0x78,0xc0,0x03,0x5f,0xd6}) &&
    Guard(r,base,0x26dab4c,std::array<uint8_t,40>{0xa8,0x83,0x5c,0xf8,0x08,0x01,0x40,0xf9,0x09,0x21,0x40,0x79,0x29,0x25,0xdc,0x1a,0x29,0xff,0x07,0x36,0x09,0x13,0x00,0x90,0x29,0x61,0x2f,0x91,0x35,0x79,0x7c,0x78,0x89,0xfb,0x7f,0xd3,0x08,0x01,0x09,0x8b}) &&
    Guard(r,base,0x293abd8,std::array<uint8_t,12>{0xa0,0x04,0x55,0x03,0x5a,0x04,0xef,0x00,0x3a,0x03,0xa0,0x00});
}
struct Ownership {
  uint64_t root{},buffer{},data{};uint16_t mask{};
  bool operator==(const Ownership&)const=default;
};
inline bool Owned(const ReadFn& r,uint64_t b,Ownership& o) {
  uint64_t mask{};
  if(!Pointer(r,b+0x34bc848,o.root) || !Pointer(r,o.root+0x10,o.buffer) ||
     !Pointer(r,o.buffer+0x16e08,o.data) || !Num(r,o.data+0x10,mask,2))return false;
  // The native loop tests six low bits; high bits are unrelated save data.
  o.mask=uint16_t(mask&63);return true;
}
struct Frame {
  std::array<uint8_t,24> registry{};
  std::vector<uint64_t> chain;
  uint64_t owner{},begin{},end{},capacity{},cursor{},target{};
  std::array<uint8_t,3> state{};
  std::vector<uint8_t> records;
  bool operator==(const Frame&)const=default;
};
inline bool Held(const ReadFn& r,uint64_t b,Frame& f) {
  const auto head=b+0x34bf070;
  if(!Bytes(r,head,f.registry.data(),24))return false;
  uint64_t node=Le(f.registry.data()+8,8),prev=head;
  while(node!=head) {
    std::array<uint8_t,24> link{};uint64_t vt{};
    if(!node || node%8 || f.chain.size()/4>=128 ||
       !Bytes(r,node,link.data(),24) || Le(link.data(),8)!=prev)return false;
    const auto owner=Le(link.data()+16,8);
    if(!owner || owner%8 || !Num(r,owner,vt))return false;
    f.chain.insert(f.chain.end(),{node,owner,vt,Le(link.data()+8,8)});
    if(vt==b+0x34119c8) {if(f.owner)return false;f.owner=owner;}
    prev=node;node=Le(link.data()+8,8);
  }
  uint64_t wrapper{},mask{},mode{},listvt{};
  if(Le(f.registry.data(),8)!=prev || f.owner<32 ||
     f.owner>std::numeric_limits<uint64_t>::max()-0x8000 ||
     !Num(r,f.owner-32,wrapper) || wrapper!=b+0x34266d0 ||
     !Num(r,f.owner+0x7f50,mask) || mask!=0x1000000000 ||
     !Num(r,f.owner+0x7faa,mode,1) || mode!=0 ||
     !Bytes(r,f.owner+0x88,f.state.data(),3))return false;
  if(f.state[0]==0)return true;
  if(f.state[0]!=2)return false;
  const uint64_t list=f.owner+0x2e40;
  if(!Num(r,list,listvt) || listvt!=b+0x342e498 ||
     !Pointer(r,list+0x3260,f.begin) || !Num(r,list+0x3268,f.end) ||
     !Num(r,list+0x3270,f.capacity) || !Num(r,list+0x3278,f.cursor,4) ||
     !Num(r,list+0x327c,f.target,4) || f.end<f.begin || f.capacity<f.end ||
     (f.end-f.begin)%64 || (f.capacity-f.begin)%64 ||
     // Original vector growth doubles capacity (26dabe8..26dac14): the
     // sixth owned machine occupies a vector with eight allocated records.
     // Logical membership remains at most six; capacity is not item count.
     (f.capacity-f.begin)/64>8)return false;
  const auto count=(f.end-f.begin)/64;
  if(!count || count>6 || f.cursor>=count || f.target>=count)return false;
  f.records.resize(size_t(count)*64);
  if(!Bytes(r,f.begin,f.records.data(),f.records.size()))return false;
  std::vector<uint16_t> ids;
  for(size_t i=0;i<f.records.size();i+=64) {
    const auto id=uint16_t(Le(f.records.data()+i,2));
    if(!IsAutomaton(id) || std::find(ids.begin(),ids.end(),id)!=ids.end())return false;
    ids.push_back(id);
  }
  return true;
}
} // namespace automaton_detail

// No cached state. Ownership remains available while the picker is closed;
// the held displayed list requires three identical native ownership frames.
inline AutomatonSnapshot ReadAutomatons(const ReadFn& read,uint64_t base,bool build_matches) {
  using namespace automaton_detail;
  AutomatonSnapshot out;
  if(!build_matches || !base || base%8 || base>std::numeric_limits<uint64_t>::max()-0x34c0000 || !Guards(read,base))return out;
  out.compatible=true;Ownership a,b,c;
  if(Owned(read,base,a) && Owned(read,base,b) && Owned(read,base,c) && a==b && b==c) {
    out.ownership_valid=true;out.ownership_mask=a.mask;out.source={a.root,a.buffer,a.data};
    for(size_t i=0;i<6;++i)if(a.mask&(1u<<i))out.owned_native_ids.push_back(AutomatonIds[i]);
  }
  Frame x,y,z;
  if(!out.ownership_valid || !Held(read,base,x) || !Held(read,base,y) || x!=y ||
     !Held(read,base,z) || y!=z || !Owned(read,base,c) || a!=c)return out;
  auto& s=out.selector;s.compatible=true;s.selector_valid=true;
  s.source.owner=x.owner;s.source.mode=0;s.source.native_state=x.state[0];
  if(x.state[0]==0)return out;
  for(size_t i=0;i<x.records.size();i+=64) {
    const auto id=uint16_t(Le(x.records.data()+i,2));
    if(std::find(out.owned_native_ids.begin(),out.owned_native_ids.end(),id)==out.owned_native_ids.end())return AutomatonSnapshot{.compatible=true};
    s.ordered_native_ids.push_back(id);
  }
  s.held=true;s.source.records=x.begin;s.source.end=x.end;s.source.capacity=x.capacity;
  s.highlighted_index=int32_t(x.cursor);s.target_index=int32_t(x.target);
  s.highlighted_native_id=s.ordered_native_ids[size_t(x.cursor)];
  return out;
}

// Bounded, read-only diagnostics. Does not confer compatibility or authority;
// even partial failed frames report the native fields needed to find the gate.
inline std::string DescribeAutomatons(const ReadFn& read,uint64_t base) {
  using namespace automaton_detail;
  if(!base || base%8 || base>std::numeric_limits<uint64_t>::max()-0x34c0000)return "invalid base";
  Frame f;const bool held=Held(read,base,f);
  uint64_t wrapper{},mask{},mode{},vt{},begin{},end{},cap{},cur{},target{},direction{};
  std::array<uint8_t,3> state{},lstate{};
  if(f.owner && f.owner<=std::numeric_limits<uint64_t>::max()-0x8000) {
    Num(read,f.owner-32,wrapper);Num(read,f.owner+0x7f50,mask);
    Num(read,f.owner+0x7faa,mode,1);Bytes(read,f.owner+0x88,state.data(),3);
    const auto list=f.owner+0x2e40;
    Num(read,list,vt);Num(read,list+0x3260,begin);Num(read,list+0x3268,end);
    Num(read,list+0x3270,cap);Num(read,list+0x3278,cur,4);Num(read,list+0x327c,target,4);
    Num(read,list+0x32a0,direction,4);Bytes(read,list+0x98,lstate.data(),3);
  }
  char line[640]{};
  std::snprintf(line,sizeof(line),"held=%d nodes=%zu owner=%llx wrapper=%llx mode=%llu mask=%llx state=%u/%u/%u listvt=%llx vector=%llx/%llx/%llx cursor=%llu/%llu lifecycle=%u/%u/%u direction=%llu",
    int(held),f.chain.size()/4,(unsigned long long)f.owner,(unsigned long long)(wrapper>=base?wrapper-base:wrapper),
    (unsigned long long)mode,(unsigned long long)mask,unsigned(state[0]),unsigned(state[1]),unsigned(state[2]),
    (unsigned long long)(vt>=base?vt-base:vt),(unsigned long long)begin,(unsigned long long)end,(unsigned long long)cap,
    (unsigned long long)cur,(unsigned long long)target,unsigned(lstate[0]),unsigned(lstate[1]),unsigned(lstate[2]),(unsigned long long)direction);
  std::string out=line;
  if(begin && end>=begin && end-begin<=6*64 && (end-begin)%64==0) {
    out+=" ids=";
    for(uint64_t p=begin;p<end;p+=64){uint64_t id{};if(!Num(read,p,id,2)){out+="?";break;}
      out+=std::to_string(id);out+=",";}
  }
  return out;
}

// Caller freshly revalidates the exact same hold immediately before this call.
// Only the native UI cursor and native Change-state request are written. Native
// release still calls the same2228a60 equip routine used by Echoes.
template<class Write> bool DirectSelectAutomaton(const AutomatonSnapshot& value,uint16_t id,const ReadFn& read,Write&& write) {
  using namespace automaton_detail;const auto& s=value.selector;
  if(!value.compatible || !value.ownership_valid || !s.selector_valid || !s.held ||
     s.source.mode!=0 || s.source.native_state!=2 || !s.source.owner || s.source.owner%8 ||
     s.source.owner>std::numeric_limits<uint64_t>::max()-0x8000 ||
     s.ordered_native_ids.empty() || s.ordered_native_ids.size()>6 ||
     s.highlighted_index<0 || size_t(s.highlighted_index)>=s.ordered_native_ids.size() ||
     s.target_index!=s.highlighted_index ||
     s.highlighted_native_id!=s.ordered_native_ids[size_t(s.highlighted_index)] ||
     !s.source.records || s.source.records%8 || s.source.end<s.source.records ||
     s.source.end-s.source.records!=s.ordered_native_ids.size()*64 ||
     s.source.capacity<s.source.end || !IsAutomaton(id) ||
     std::find(value.owned_native_ids.begin(),value.owned_native_ids.end(),id)==value.owned_native_ids.end())return false;
  const auto at=std::find(s.ordered_native_ids.begin(),s.ordered_native_ids.end(),id);
  if(at==s.ordered_native_ids.end())return false;
  const uint64_t list=s.source.owner+0x2e40;
  std::array<uint8_t,3> state{};uint64_t direction{};
  if(!Bytes(read,list+0x98,state.data(),3) || state[0]!=1 || state[2]!=0xfe ||
     !Num(read,list+0x32a0,direction,4) || direction>1)return false;
  const uint32_t index=uint32_t(at-s.ordered_native_ids.begin());
  if(index==uint32_t(s.highlighted_index))return true;
  std::array<uint8_t,8> pair{};
  for(size_t i=0;i<4;++i)pair[i]=pair[i+4]=uint8_t(index>>(i*8));
  if(!write(list+0x3278,pair.data(),8))return false;
  // Mode0 has its own direction field; its d0 entry refreshes all fifteen
  // slots through26db040 before animation, exactly as native Change does.
  const std::array<uint8_t,4> left{};
  if(!write(list+0x32a0,left.data(),4))return false;
  const uint8_t change=2;return write(list+0x9a,&change,1);
}
} // namespace echoes
