// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_warp_choice.cpp"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <unordered_map>
namespace {
constexpr uint64_t Base=0x80000000,Menu=0x90000020,Focus=0x90010000,FocusOwner=0x90020000,Popup=0x90030000,PopupOwner=0x90040000,Values=0x90050000;
size_t checks{};
void Check(bool b){++checks;if(!b)throw std::runtime_error("choice check "+std::to_string(checks));}
struct Memory {
 std::unordered_map<uint64_t,uint8_t> bytes;
 std::unordered_map<uint64_t,size_t> visits;
 std::function<void(uint64_t,size_t)> before;
 bool fail{},throw_read{};
 void Put(uint64_t p,uint64_t v,size_t n=8){for(size_t i=0;i<n;++i)bytes[p+i]=uint8_t(v>>(8*i));}
 void State(uint64_t p,uint8_t v){Put(p,v,1);Put(p+1,1,1);Put(p+2,0xfe,1);}
 Memory(){
  for(const auto&g:echoes::Guards)for(size_t i=0;i<g.bytes.size();++i)bytes[Base+g.at+i]=g.bytes[i];
  Put(Menu,Base+0x341ebf0);Put(Menu-0x20,Base+0x34268d0);Put(Base+0x341ebf0+0x1e8,Base+0x25c89a0);
  State(Menu+0x88,2);State(Menu+0x208,4);Put(Menu+0x3b0,Focus);Put(Menu+0x3b8,FocusOwner);
  Put(Focus,Base+0x34223d8);Put(Focus+0x1c8,51,4);Put(Focus+0x1cc,0,1);Put(Focus+0x1cd,1,1);
  Put(Menu+0x388,Popup);Put(Menu+0x390,PopupOwner);Put(Popup,Base+0x3420bc0);Put(Base+0x3420bc0+0x70,Base+0x25ed464);
  Put(Popup+0x80,Base+0x3420c90);Put(Popup+0x100,Base+0x3420c48);State(Popup+0x88,2);State(Popup+0x108,2);
  Put(Popup+0x220,Base+0x341f0c0);Put(Popup+0x228,Menu);Put(Popup+0x240,Popup+0x220);Put(Base+0x341f0c0+0x30,Base+0x25d10a0);
  Put(Popup+0xe0,Values);Put(Popup+0xe8,Values+2);Put(Popup+0xf0,Values+2);Put(Values,0,1);Put(Values+1,1,1);Put(Popup+0x1b0,0); }
 echoes::ReadFn Reader(){return [&](uint64_t p,void*out,size_t n){
  if(throw_read)throw std::runtime_error("fault");
  if(fail)return false;
  auto count=++visits[p];if(before)before(p,count);
  for(size_t i=0;i<n;++i){auto a=bytes.find(p+i);if(a==bytes.end())return false;static_cast<uint8_t*>(out)[i]=a->second;}return true;
 };}
};
}
int main(){try{
 Memory m;echoes::WarpMenuSnapshot menu;menu.compatible=menu.valid=menu.focus_warp=menu.native_allowed=menu.selectable=true;menu.component=Menu;menu.focused=Focus;menu.focused_owner=FocusOwner;menu.destination_index=51;menu.state=echoes::WarpMapState::ConfirmWarp;
 auto run=[&](){return echoes::ReadWarpChoice(m.Reader(),Base,true,menu);};
 auto bad=[&](){auto s=run();Check(!s.valid&&s.popup==0&&s.menu==0&&s.choice==echoes::NativeWarpChoice::Cancel);};
 auto s=run();Check(s.compatible&&s.valid&&s.popup==Popup&&s.menu==Menu&&s.focused==Focus&&s.destination_index==51&&s.choice==echoes::NativeWarpChoice::Warp);
 m.Put(Popup+0x1b4,1,4);s=run();Check(s.valid&&s.choice==echoes::NativeWarpChoice::Cancel);m.Put(Popup+0x1b4,0,4);
 for(auto [p,n,v]:std::vector<std::tuple<uint64_t,size_t,uint64_t>>{
  {Menu,8,0},{Menu-0x20,8,0},{Base+0x341ebf0+0x1e8,8,0},{Menu+0x88,1,3},{Menu+0x20a,1,4},
  {Menu+0x208,1,3},{Menu+0x3b0,8,0},{Menu+0x3b8,8,0},{Focus,8,0},{Focus+0x1c8,4,50},{Focus+0x1cd,1,0},
  {Menu+0x388,8,0},{Menu+0x390,8,0},{Popup,8,0},{Base+0x3420bc0+0x70,8,0},{Popup+0x80,8,0},{Popup+0x100,8,0},
  {Popup+0x88,1,1},{Popup+0x108,1,1},{Popup+0x220,8,0},{Popup+0x228,8,Focus},{Popup+0x240,8,0},
  {Base+0x341f0c0+0x30,8,0},{Popup+0xe8,8,Values+3},{Popup+0xf0,8,Values+65},{Values,1,1},{Values+1,1,0},
  {Popup+0x1b0,4,1},{Popup+0x1b4,4,2}}){auto saved=m.bytes;m.Put(p,v,n);bad();m.bytes=std::move(saved);}
 for(const auto&g:echoes::Guards){m.bytes[Base+g.at]^=1;bad();m.bytes[Base+g.at]^=1;}
 m.visits.clear();m.before=[&](uint64_t p,size_t n){if(p==Popup+0x1b0&&n==2)m.Put(Popup+0x1b4,1,4);};bad();m.before={};m.Put(Popup+0x1b4,0,4);
 m.visits.clear();m.before=[&](uint64_t p,size_t n){if(p==Popup+0x1b0&&n==3)m.Put(Popup+0x1b4,1,4);};bad();m.before={};m.Put(Popup+0x1b4,0,4);
 m.fail=true;bad();m.fail=false;m.throw_read=true;bad();m.throw_read=false;
 auto saved_menu=menu;menu.valid=false;bad();menu=saved_menu;menu.native_allowed=false;bad();menu=saved_menu;menu.selectable=false;bad();menu=saved_menu;menu.state=echoes::WarpMapState::Normal;bad();menu=saved_menu;
 Check(!echoes::ReadWarpChoice({},Base,true,menu).valid);Check(!echoes::ReadWarpChoice(m.Reader(),Base,false,menu).valid);Check(!echoes::ReadWarpChoice(m.Reader(),Base+1,true,menu).valid);
 if(const char* dir=std::getenv("ECHOES_SEGMENTS")){std::ifstream f(std::filesystem::path(dir)/"text.bin",std::ios::binary);std::vector<uint8_t> text{std::istreambuf_iterator<char>(f),{}};for(const auto&g:echoes::Guards)Check(g.at+g.bytes.size()<=text.size()&&std::equal(g.bytes.begin(),g.bytes.end(),text.begin()+g.at));}
 std::cout<<checks<<" native warp popup choice checks passed\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
