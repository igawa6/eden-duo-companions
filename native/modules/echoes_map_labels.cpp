// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_map_labels.h"
#include "echoes_catalog.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
namespace echoes {
namespace {
struct Blob {
  std::span<const uint8_t> b;
  void Fit(size_t p,size_t n)const{if(p>b.size()||n>b.size()-p)throw std::runtime_error("range");}
  uint16_t H(size_t p)const{Fit(p,2);return uint16_t(b[p])|uint16_t(b[p+1])<<8;}
  uint32_t D(size_t p)const{return H(p)|uint32_t(H(p+2))<<16;}
  uint64_t Q(size_t p)const{return D(p)|uint64_t(D(p+4))<<32;}
  float F(size_t p)const{const float f=std::bit_cast<float>(D(p));if(!std::isfinite(f)||std::abs(f)>100000)throw std::runtime_error("float");return f;}
  Blob Sub(size_t p,size_t n)const{Fit(p,n);return{b.subspan(p,n)};}
  std::string C(size_t p,size_t limit=256)const{
    std::string s;
    for(size_t i=0;i<limit;++i){Fit(p+i,1);const auto c=b[p+i];if(!c)return s;if(c<32||c>126)throw std::runtime_error("string");s+=char(c);}
    throw std::runtime_error("termination");
  }
};
std::vector<uint8_t> Read(const ReadAssetFn& read,std::string_view path,size_t limit) {
  const auto n=read?read(path,0,nullptr,0):0;
  if(!n||n>limit)throw std::runtime_error("source budget");
  std::vector<uint8_t>b(n);
  if(read(path,0,b.data(),n)!=n||read(path,0,nullptr,0)!=n)throw std::runtime_error("source changed");
  return b;
}
bool Component(std::string_view s){return !s.empty()&&s.size()<128&&s.front()!='/'&&s.back()!='/'&&std::all_of(s.begin(),s.end(),[](char c){return(c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='/';});}
struct NativePart{std::string ref;MapPane text;};
MapPane Pane(Blob b,int parent){
  b.Fit(0,84);MapPane p;p.name=b.C(12,24);p.parent=parent;p.flags=b.b[8];p.origin=b.b[9];p.alpha=b.b[10];
  p.x=b.F(44);p.y=b.F(48);p.z=b.F(52);p.rotation_x=b.F(56);p.rotation_y=b.F(60);p.rotation_z=b.F(64);p.scale_x=b.F(68);p.scale_y=b.F(72);p.width=b.F(76);p.height=b.F(80);return p;
}
struct Layout{std::vector<MapPane>panes;std::map<std::string,NativePart> labels;};
Layout ParseLayout(Blob b,bool field){
  if(b.b.size()>128*1024||b.b.size()<20||!std::equal(b.b.begin(),b.b.begin()+4,"FLYT")||b.H(4)!=0xfeff||b.D(8)!=0x09000000||b.D(12)!=b.b.size())throw std::runtime_error("layout");
  Layout out;std::vector<int> stack{-1};int last=-1;size_t sections=0;std::set<std::string>names;
  for(size_t o=b.H(6);o<b.b.size();){
    const size_t size=b.D(o+4);if(size<8)throw std::runtime_error("section");auto s=b.Sub(o,size);++sections;const std::string tag(reinterpret_cast<const char*>(s.b.data()),4);
    if(tag=="pas1"){if(last<0||stack.size()>32)throw std::runtime_error("depth");stack.push_back(last);}
    else if(tag=="pae1"){if(stack.size()<2)throw std::runtime_error("stack");stack.pop_back();}
    else if(tag=="pan1"||tag=="pic1"||tag=="prt1"||tag=="bnd1"||tag=="wnd1"||tag=="txt1"){
      auto p=Pane(s,stack.back());if(!names.insert(p.name).second||out.panes.size()>=1024)throw std::runtime_error("pane");
      last=static_cast<int>(out.panes.size());out.panes.push_back(p);
      if(field&&tag=="prt1"&&p.name.starts_with("L_LocationName_")){
        if(s.D(84)!=1||s.C(136)!="L_MapLocationName"||s.C(96,24)!="T_text_00"||s.D(120)!=1)throw std::runtime_error("label part");
        const size_t offset=s.D(124);auto t=s.Sub(offset,s.D(offset+4));
        if(!std::equal(t.b.begin(),t.b.begin()+4,"txt1")||t.C(12,24)!="T_text_00")throw std::runtime_error("text override");
        NativePart part;part.text=Pane(t,-1);part.ref=t.C(t.D(128));
        if(!out.labels.emplace(p.name,std::move(part)).second)throw std::runtime_error("duplicate label");
      }
    }
    o+=size;
  }
  if(sections!=b.H(16)||stack.size()!=1||out.panes.empty())throw std::runtime_error("structure");
  return out;
}
struct Transform{double x{},y{},sx{1},sy{1};};
Transform World(const std::vector<MapPane>& panes,int id){
  std::vector<int>chain;
  for(int i=id;i>=0;){if(static_cast<size_t>(i)>=panes.size()||chain.size()>=32)throw std::runtime_error("ancestry");chain.push_back(i);i=panes[static_cast<size_t>(i)].parent;}
  Transform t;
  for(auto it=chain.rbegin();it!=chain.rend();++it){const auto&p=panes[static_cast<size_t>(*it)];
    if(p.origin||p.z||p.rotation_x||p.rotation_y||p.rotation_z||p.scale_x<=0||p.scale_y<=0||!(p.flags&1)||!p.alpha)throw std::runtime_error("unsupported transform");
    t={t.x+p.x*t.sx,t.y+p.y*t.sy,t.sx*p.scale_x,t.sy*p.scale_y};
  }
  return t;
}
int Find(const std::vector<MapPane>& p,std::string_view name){int found=-1;for(size_t i=0;i<p.size();++i)if(p[i].name==name){if(found>=0)throw std::runtime_error("duplicate");found=static_cast<int>(i);}if(found<0)throw std::runtime_error("missing");return found;}
std::vector<NativeMapLabel> Sheet(Blob b){
  b.Fit(0,192);if(!std::equal(b.b.begin(),b.b.begin()+8,"gsht\2\0\0\0")||b.D(24)!=2||b.D(28)!=3||b.D(40)!=26||b.D(44)!=32)throw std::runtime_error("schema");
  const size_t rows=static_cast<size_t>(b.Q(32)),pool=static_cast<size_t>(b.Q(16));b.Fit(rows,26*32);if(rows<192||pool<rows+26*32)throw std::runtime_error("sheet bounds");
  const std::array<std::string_view,3> names{"partsName","mTraverseInfos","mIndex"};
  const std::array<uint16_t,3> types{4,0,258},flags{4,2,0},offsets{0,16,0},sizes{16,16,4},strides{16,4,4},children{0,1,0};
  for(size_t i=0;i<3;++i){const size_t p=48+i*48;if(b.Q(p)<pool||b.C(static_cast<size_t>(b.Q(p)))!=names[i]||b.H(p+16)!=types[i]||b.H(p+18)!=flags[i]||b.H(p+20)!=offsets[i]||b.H(p+22)!=sizes[i]||b.H(p+24)!=strides[i]||b.H(p+26)!=children[i])throw std::runtime_error("field");}
  if(b.Q(128)!=144)throw std::runtime_error("child descriptor");
  std::vector<NativeMapLabel>out;std::set<std::string>unique;
  for(size_t i=0;i<26;++i){const size_t p=rows+i*32,at=static_cast<size_t>(b.Q(p)),n=static_cast<size_t>(b.Q(p+8));
    if(at<pool||n>128||b.C(at)!=b.C(at,n+1))throw std::runtime_error("part name");
    NativeMapLabel l;l.part_name=b.C(at,n+1);if(l.part_name.size()!=n||!l.part_name.starts_with("L_LocationName_")||!unique.insert(l.part_name).second)throw std::runtime_error("part identity");
    const size_t arr=static_cast<size_t>(b.Q(p+16)),count=static_cast<size_t>(b.Q(p+24));
    if(arr<pool||!count||count>512)throw std::runtime_error("traverse array");
    b.Fit(arr,count*4);std::set<uint16_t>indices;
    for(size_t j=0;j<count;++j){const auto index=b.D(arr+j*4);if(index>=512||!indices.insert(static_cast<uint16_t>(index)).second)throw std::runtime_error("index");l.traversal_indices.push_back(static_cast<uint16_t>(index));}
    out.push_back(std::move(l));
  }
  return out;
}
} // namespace
bool LoadNativeMapLabels(const ReadAssetFn&read,const FieldMapGeometry& geometry,std::string_view root,std::vector<NativeMapLabel>&out){
  out.clear();try{
    if(!Component(root)||geometry.field_panes.empty())return false;
    const auto metadata=Read(read,"region_common/datasheets/ui/UiMapLocationInfo.gsheet",1u<<20);
    auto labels=Sheet({metadata});
    const auto field=Read(read,"region_common/ui/Field.arc#blyt/Field.bflyt",128u<<10),child=Read(read,"region_common/ui/Field.arc#blyt/L_MapLocationName.bflyt",128u<<10);
    const auto parsed=ParseLayout({field},true),layout=ParseLayout({child},false);
    if(parsed.labels.size()!=26||geometry.field_panes.size()!=parsed.panes.size())return false;
    const auto terrain=Find(geometry.field_panes,"P_Terrain_00");const auto tr=World(geometry.field_panes,terrain);const auto &tp=geometry.field_panes[static_cast<size_t>(terrain)];
    const double width=tp.width*tr.sx,height=tp.height*tr.sy,left=tr.x-width/2,top=tr.y+height/2;
    if(width!=1272||height!=916)return false;
    const auto child_at=Find(layout.panes,"T_text_00");auto child_panes=layout.panes;
    std::unordered_map<std::string,std::unordered_map<std::string,std::string>>banks;
    for(auto&l:labels){const auto found=parsed.labels.find(l.part_name);if(found==parsed.labels.end())return false;
      const auto id=Find(geometry.field_panes,l.part_name),raw_id=Find(parsed.panes,l.part_name);const auto a=World(geometry.field_panes,id),raw=World(parsed.panes,raw_id);
      if(a.x!=raw.x||a.y!=raw.y||a.sx!=raw.sx||a.sy!=raw.sy)return false;
      const int parent=child_panes[static_cast<size_t>(child_at)].parent;child_panes[static_cast<size_t>(child_at)]=found->second.text;child_panes[static_cast<size_t>(child_at)].parent=parent;const auto local=World(child_panes,child_at);
      l.u=(a.x+local.x*a.sx-left)/width;l.v=(top-a.y-local.y*a.sy)/height;
      if(!std::isfinite(l.u)||!std::isfinite(l.v)||l.u<0||l.u>1||l.v<0||l.v>1)return false;
      l.label_ref=found->second.ref;const auto colon=l.label_ref.find(':');if(colon==l.label_ref.npos||l.label_ref.find(':',colon+1)!=l.label_ref.npos)return false;
      const auto file=l.label_ref.substr(0,colon),key=l.label_ref.substr(colon+1);if(!Component(file)||!Component(key))return false;
      if(!banks.contains(file)){if(banks.size()>=4)return false;const auto msbt=Read(read,std::string(root)+"/message/"+file+".msbt",1u<<20);if(!DecodeNativeMessages(msbt,banks[file]))return false;}
      const auto text=banks[file].find(key);if(text==banks[file].end()||text->second.empty())return false;l.name=text->second;
    }
    out=std::move(labels);return true;
  }catch(...){out.clear();return false;}
}
bool NativeMapLabelDiscovered(const NativeMapLabel&l,std::span<const uint8_t,64>bits){
  if(l.name.empty()||l.traversal_indices.empty()||l.traversal_indices.size()>512)return false;
  std::array<uint8_t,64> seen{};
  for(const auto i:l.traversal_indices){
    if(i>=512)return false;
    const auto mask=static_cast<uint8_t>(1u<<(i%8));
    if((seen[i/8]&mask)||!(bits[i/8]&mask))return false;
    seen[i/8]|=mask;
  }
  return true;
}
} // namespace echoes
