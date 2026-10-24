// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_automatons.h"
#include <cstdlib>
#include <iostream>
#include <unordered_map>

namespace {
constexpr uint64_t B=0x80000000,H=B+0x34bf070,N=0x2107000000,O=N+0x1020;
constexpr uint64_t L=O+0x2e40,R=N+0x20000,Root=N+0x30000,Buf=N+0x40000,Data=N+0x60000;
void Check(bool v,const char* why) {if(!v){std::cerr<<why<<'\n';std::exit(1);}}
struct Memory {
  std::unordered_map<uint64_t,uint8_t>b;
  std::vector<std::pair<uint64_t,std::vector<uint8_t>>>writes;
  unsigned mask_reads{};bool change_mask{},fail_write{};
  void Put(uint64_t a,uint64_t v,size_t n=8){for(size_t i=0;i<n;++i)b[a+i]=uint8_t(v>>(i*8));}
  template<size_t S>void Bytes(uint64_t a,const std::array<uint8_t,S>&v){for(size_t i=0;i<S;++i)b[a+i]=v[i];}
  Memory(){
    Bytes(B+0x26da938,std::array<uint8_t,20>{0x08,0x78,0xb2,0xb9,0x08,0xe5,0x7a,0xd3,0x09,0x30,0x59,0xf9,0x20,0x69,0x68,0x78,0xc0,0x03,0x5f,0xd6});
    Bytes(B+0x26dab4c,std::array<uint8_t,40>{0xa8,0x83,0x5c,0xf8,0x08,0x01,0x40,0xf9,0x09,0x21,0x40,0x79,0x29,0x25,0xdc,0x1a,0x29,0xff,0x07,0x36,0x09,0x13,0x00,0x90,0x29,0x61,0x2f,0x91,0x35,0x79,0x7c,0x78,0x89,0xfb,0x7f,0xd3,0x08,0x01,0x09,0x8b});
    for(size_t i=0;i<6;++i)Put(B+0x293abd8+i*2,echoes::AutomatonIds[i],2);
    Put(B+0x34bc848,Root);Put(Root+0x10,Buf);Put(Buf+0x16e08,Data);Put(Data+0x10,63,2);
    Put(H,N);Put(H+8,N);Put(H+16,1);
    Put(N,H);Put(N+8,H);Put(N+16,O);Put(O,B+0x34119c8);Put(O-32,B+0x34266d0);
    Put(O+0x88,2,3);Put(O+0x7faa,0,1);Put(O+0x7f50,0x1000000000);
    Put(L,B+0x342e498);Put(L+0x3260,R);Put(L+0x3268,R+6*64);Put(L+0x3270,R+8*64);
    Put(L+0x3278,0);Put(L+0x98,0xfe0001,3);Put(L+0x32a0,1,4);
    for(size_t i=0;i<6;++i){for(size_t j=0;j<64;++j)b[R+i*64+j]=0;Put(R+i*64,echoes::AutomatonIds[i],2);}
  }
  bool Read(uint64_t a,void*out,size_t n){
    if(a==Data+0x10 && ++mask_reads==4 && change_mask)Put(a,31,2);
    auto*p=static_cast<uint8_t*>(out);for(size_t i=0;i<n;++i){auto it=b.find(a+i);if(it==b.end())return false;p[i]=it->second;}return true;
  }
  auto Reader(){return [this](auto a,auto p,auto n){return Read(a,p,n);};}
  auto Sample(){return echoes::ReadAutomatons(Reader(),B,true);}
  bool Select(const echoes::AutomatonSnapshot&s,uint16_t id){return echoes::DirectSelectAutomaton(s,id,Reader(),[this](auto a,const void*p,size_t n){
    if(fail_write)return false;const auto*q=static_cast<const uint8_t*>(p);writes.push_back({a,{q,q+n}});return true;});}
};
}
int main(){
  {Memory m;auto s=m.Sample();Check(s.ownership_valid && s.owned_native_ids.size()==6 && s.selector.held,"valid mode0");
   Check(s.source==echoes::AutomatonSource{Root,Buf,Data},"ownership source");
   Check(s.selector.highlighted_native_id==1184 && s.selector.ordered_native_ids.back()==160,"native order");
   Check(m.Select(s,160) && m.writes.size()==3,"direct far selection");
   Check(m.writes[0].first==L+0x3278 && m.writes[0].second==std::vector<uint8_t>({5,0,0,0,5,0,0,0}),"atomic actual/target");
   Check(m.writes[1].first==L+0x32a0 && m.writes[2].first==L+0x9a && m.writes[2].second[0]==2,"native mode0 refresh");}
  {Memory m;m.Put(O+0x88,0,3);auto s=m.Sample();Check(s.ownership_valid && s.selector.selector_valid && !s.selector.held && s.selector.ordered_native_ids.empty(),"closed keeps owned only");}
  {Memory m;m.Put(O+0x88,0,3);m.Put(Data+0x10,0,2);auto s=m.Sample();Check(s.ownership_valid && s.owned_native_ids.empty() && !s.selector.held,"zero machines is valid closed ownership");}
  {Memory m;m.Put(L+0x3268,R);Check(!m.Sample().selector.held,"empty held list unavailable");}
  {Memory m;m.Put(Data+0x10,0,2);auto s=m.Sample();Check(!s.selector.held,"unowned displayed entries refused");}
  {Memory m;m.Put(O+0x7faa,1,1);auto s=m.Sample();Check(s.ownership_valid && !s.selector.selector_valid,"echo mode must not masquerade as mode0");}
  {Memory m;m.Put(L,B+0x342c260);Check(!m.Sample().selector.held,"echo list rejected");}
  {Memory m;m.Put(R+64,1184,2);Check(!m.Sample().selector.held,"duplicate IDs rejected");}
  {Memory m;m.Put(R+64,40,2);Check(!m.Sample().selector.held,"nonmachine ID rejected");}
  {Memory m;m.Put(L+0x3270,R+8*64);Check(m.Sample().selector.held,"six machines retain native doubled capacity8");}
  {Memory m;m.Put(L+0x3270,R+9*64);Check(!m.Sample().selector.held,"capacity bound");}
  {Memory m;m.Put(L+0x3268,R+7*64);m.Put(L+0x3270,R+8*64);Check(!m.Sample().selector.held,"logical membership remains six");}
  {Memory m;m.Put(L+0x3278,6,4);Check(!m.Sample().selector.held,"cursor bound");}
  {Memory m;m.Put(N+8,N);Check(!m.Sample().selector.held,"registry loop bound");}
  {Memory m;m.change_mask=true;Check(!m.Sample().selector.held,"post-list ownership race");}
  {Memory m;m.Put(B+0x26da938,0,1);Check(!m.Sample().compatible,"immutable getter mismatch");}
  {Memory m;auto d=echoes::DescribeAutomatons(m.Reader(),B);Check(d.find("held=1")!=std::string::npos && d.find("ids=1184,853,1114,239,826,160,")!=std::string::npos,"bounded mode0 diagnostics");}
  {Memory m;auto s=m.Sample();Check(!m.Select(s,40) && m.writes.empty(),"no foreign selection write");}
  {Memory m;auto s=m.Sample();m.Put(L+0x98,2,1);Check(!m.Select(s,160) && m.writes.empty(),"no transitional selection write");}
  {Memory m;auto s=m.Sample();Check(m.Select(s,1184) && m.writes.empty(),"current selection noop");}
  {Memory m;auto s=m.Sample();m.fail_write=true;Check(!m.Select(s,160) && m.writes.empty(),"failed store aborts");}
  std::cout<<"Automaton ownership/held/direct selection checks passed\n";
}
