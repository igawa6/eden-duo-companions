// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_map_labels.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
using namespace echoes;
namespace {
size_t checks{};
void Check(bool ok,const char*why){++checks;if(!ok){std::cerr<<"FAIL: "<<why<<'\n';std::exit(1);}}
uint16_t H(const std::vector<uint8_t>&b,size_t p){return uint16_t(b.at(p))|uint16_t(b.at(p+1))<<8;}
uint32_t D(const std::vector<uint8_t>&b,size_t p){return H(b,p)|uint32_t(H(b,p+2))<<16;}
uint64_t Q(const std::vector<uint8_t>&b,size_t p){return D(b,p)|uint64_t(D(b,p+4))<<32;}
float F(const std::vector<uint8_t>&b,size_t p){return std::bit_cast<float>(D(b,p));}
std::vector<uint8_t> File(const std::filesystem::path&p){std::ifstream f(p,std::ios::binary);return{std::istreambuf_iterator<char>(f),{}};}
void Put(std::vector<uint8_t>&b,size_t p,uint64_t v,size_t n){for(size_t i=0;i<n;++i)b.at(p+i)=uint8_t(v>>(8*i));}
struct Fixture {
 std::filesystem::path root;
 std::map<std::string,std::vector<uint8_t>,std::less<>> files;
 explicit Fixture(const char*r):root(r){
  auto b=File(root/"region_common/ui/Field.arc");Check(b.size()>20&&std::memcmp(b.data(),"SARC",4)==0,"native SARC");const size_t fat=H(b,4),data=D(b,12),n=H(b,fat+6),names=fat+12+n*16+8;
  for(size_t i=0;i<n;++i){const size_t p=fat+12+i*16;const auto attr=D(b,p+4);if(!(attr>>24))continue;const size_t name=names+size_t(attr&0xffffff)*4;std::string s(reinterpret_cast<const char*>(b.data()+name));if(s=="blyt/Field.bflyt"||s=="blyt/L_MapLocationName.bflyt"){const size_t first=data+D(b,p+8),last=data+D(b,p+12);files["region_common/ui/Field.arc#"+s]={b.begin()+static_cast<std::ptrdiff_t>(first),b.begin()+static_cast<std::ptrdiff_t>(last)};}}
  files["region_common/datasheets/ui/UiMapLocationInfo.gsheet"]=File(root/"region_common/datasheets/ui/UiMapLocationInfo.gsheet");
 }
 size_t Read(std::string_view path,uint64_t off,void*out,size_t n){
  auto it=files.find(path);if(it==files.end())it=files.emplace(std::string(path),File(root/std::string(path))).first;const auto&b=it->second;
  if(!out&&!n)return b.size();
  if(off>b.size()||n>b.size()-off)return 0;
  std::copy_n(b.data()+off,n,static_cast<uint8_t*>(out));return n;
 }
 FieldMapGeometry Geometry(){
  const auto&b=files.at("region_common/ui/Field.arc#blyt/Field.bflyt");FieldMapGeometry g;std::vector<int>stack{-1};int last=-1;
  for(size_t o=H(b,6);o<b.size();){const std::string tag(reinterpret_cast<const char*>(b.data()+o),4);const size_t z=D(b,o+4);
   if(tag=="pas1")stack.push_back(last);else if(tag=="pae1")stack.pop_back();
   else if(tag=="pan1"||tag=="pic1"||tag=="prt1"||tag=="bnd1"||tag=="wnd1"||tag=="txt1"){
    MapPane p;p.name=std::string(reinterpret_cast<const char*>(b.data()+o+12));p.parent=stack.back();p.flags=b[o+8];p.origin=b[o+9];p.alpha=b[o+10];p.x=F(b,o+44);p.y=F(b,o+48);p.z=F(b,o+52);p.rotation_x=F(b,o+56);p.rotation_y=F(b,o+60);p.rotation_z=F(b,o+64);p.scale_x=F(b,o+68);p.scale_y=F(b,o+72);p.width=F(b,o+76);p.height=F(b,o+80);last=static_cast<int>(g.field_panes.size());g.field_panes.push_back(p);
   }o+=z;
  }return g;
 }
};
}
int main(){
 NativeMapLabel sample;sample.name="revealed";sample.traversal_indices={0,511};std::array<uint8_t,64>bits{};
 Check(!NativeMapLabelDiscovered(sample,bits),"empty discovery hides label");bits[0]=1;Check(!NativeMapLabelDiscovered(sample,bits),"partial region remains hidden");bits[63]=128;Check(NativeMapLabelDiscovered(sample,bits),"all authored indices authorize conservative label");sample.traversal_indices.push_back(512);Check(!NativeMapLabelDiscovered(sample,bits),"out of bounds discovery index refused");sample.traversal_indices={0,0};Check(!NativeMapLabelDiscovered(sample,bits),"duplicate indices refused");
 std::vector<NativeMapLabel>labels{sample};Check(!LoadNativeMapLabels({}, {},"regionUS/USen",labels)&&labels.empty(),"missing worker source clears labels");
 const char*env=std::getenv("ECHOES_ROMFS");if(!env){std::cout<<checks<<" unit checks passed; native fixture unavailable\n";return 77;}
 Fixture fixture(env);const ReadAssetFn read=[&](std::string_view p,uint64_t o,void*b,size_t n){return fixture.Read(p,o,b,n);};const auto geometry=fixture.Geometry();
 Check(LoadNativeMapLabels(read,geometry,"regionUS/USen",labels)&&labels.size()==26,"all original Field label attachments localize");
 Check(labels[0].part_name=="L_LocationName_00_00"&&labels[0].label_ref=="glossary/Location:Field_011"&&labels[0].traversal_indices.size()==7,"first label retains native override and discovery array");
 Check(std::abs(labels[0].u-((-455.0175476074219-12+644)/1272))<1e-12&&std::abs(labels[0].v-((419-252.7874755859375)/916))<1e-12,"authored parent chain projects into native terrain UV");
 bits.fill(255);for(const auto&l:labels){Check(!l.name.empty()&&l.u>=0&&l.u<=1&&l.v>=0&&l.v<=1,"every native label localized and bounded");Check(NativeMapLabelDiscovered(l,bits),"complete discovery admits native label");auto partial=bits;const auto first=l.traversal_indices.front();partial[first/8]&=uint8_t(~(1u<<(first%8)));Check(!NativeMapLabelDiscovered(l,partial),"any missing authored index suppresses each label");}
 const auto original=labels;
 auto changed_geometry=geometry;for(auto&p:changed_geometry.field_panes)if(p.name==labels[0].part_name)p.x+=1;
 Check(!LoadNativeMapLabels(read,changed_geometry,"regionUS/USen",labels)&&labels.empty(),"mismatched parsed geometry refuses labels");
 const std::string sheet="region_common/datasheets/ui/UiMapLocationInfo.gsheet";const auto saved=fixture.files[sheet];const auto first_array=static_cast<size_t>(Q(saved,static_cast<size_t>(Q(saved,32))+16));
 Put(fixture.files[sheet],first_array,512,4);Check(!LoadNativeMapLabels(read,geometry,"regionUS/USen",labels)&&labels.empty(),"bad native traversal index clears all output");fixture.files[sheet]=saved;
 fixture.files[sheet].resize(100);Check(!LoadNativeMapLabels(read,geometry,"regionUS/USen",labels),"truncated metadata refused");fixture.files[sheet]=saved;
 const std::string path="region_common/ui/Field.arc#blyt/Field.bflyt";const auto field=fixture.files[path];fixture.files[path][8]=8;Check(!LoadNativeMapLabels(read,geometry,"regionUS/USen",labels),"unsupported FLYT revision refused");fixture.files[path]=field;
 Check(!LoadNativeMapLabels(read,geometry,"../bad",labels),"unsafe message root refused");
 Check(LoadNativeMapLabels(read,geometry,"regionUS/USen",labels)&&labels==original,"valid restoration after failed loads deterministic");
 std::cout<<checks<<" map label checks passed; "<<labels.size()<<" native labels\n";
}
