// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Tests only: authored FlatBuffers + GFLXPACK fixtures, no game bytes.
#include <cassert>
#include <map>
#include <string>
#include "sw_encounters.h"
namespace sw_test_encounters {
using sw_archive::Bytes;
using sw_archive::Hash;
inline void Put(Bytes& b,std::size_t p,std::uint64_t v,unsigned n=4) {
    assert(p+n<=b.size()); for(unsigned i=0;i<n;++i) b[p+i]=v>>(i*8);
}
struct Node {
    std::map<unsigned,std::uint64_t> scalars;
    std::map<unsigned,Node> children;
    std::map<unsigned,std::vector<Node>> vectors;
};
struct Builder {
    Bytes b=Bytes(4);
    std::size_t Allocate(std::size_t n) { auto p=b.size(); b.resize(p+n);return p; }
    std::size_t Table(const Node& node) {
        unsigned fields=0;
        for(auto& [i,v]:node.scalars) fields=std::max(fields,i+1);
        for(auto& [i,v]:node.children) fields=std::max(fields,i+1);
        for(auto& [i,v]:node.vectors) fields=std::max(fields,i+1);
        auto vt=Allocate(4+fields*2),p=Allocate(4+fields*8);
        Put(b,vt,4+fields*2,2);Put(b,vt+2,4+fields*8,2);Put(b,p,p-vt);
        auto field=[&](unsigned i){Put(b,vt+4+i*2,4+i*8,2);return p+4+i*8;};
        for(auto& [i,v]:node.scalars) Put(b,field(i),v,8);
        for(auto& [i,v]:node.children) { auto f=field(i),target=Table(v);Put(b,f,target-f); }
        for(auto& [i,vs]:node.vectors) {
            auto f=field(i),v=Allocate(4+vs.size()*4);Put(b,f,v-f);Put(b,v,vs.size());
            for(std::size_t j=0;j<vs.size();++j) { auto at=v+4+j*4,target=Table(vs[j]);Put(b,at,target-at); }
        }
        return p;
    }
    Bytes Build(const Node& n) { auto root=Table(n);Put(b,0,root);return b; }
};
inline Node Slot(int sp,int rate,int form=0) { return {{{0,rate},{1,sp},{2,form}},{},{}}; }
inline Node Sub(std::vector<Node> slots,int lo=12,int hi=18) { return {{{0,lo},{1,hi}},{},{{2,std::move(slots)}}}; }
inline Node Encounter(std::uint64_t zone,std::vector<Node> subs) { return {{{0,zone}},{},{{1,std::move(subs)}}}; }
inline Bytes Pak(const std::vector<std::pair<std::string,Bytes>>& files,bool compressed=false) {
    const std::size_t count=files.size(),folder=48,ft=folder+16+count*16;
    Bytes b(ft+count*24);std::memcpy(b.data(),"GFLXPACK",8);
    Put(b,16,count);Put(b,20,1);Put(b,24,ft,8);Put(b,40,folder,8);Put(b,folder+8,count);
    for(std::size_t i=0;i<count;++i) {
        Put(b,folder+16+i*16,Hash(files[i].first),8);Put(b,folder+24+i*16,i);
        auto raw=files[i].second;
        Bytes encoded=raw;
        if(compressed) {
            encoded.resize(LZ4_compressBound(raw.size()));
            int n=LZ4_compress_default(reinterpret_cast<const char*>(raw.data()),reinterpret_cast<char*>(encoded.data()),raw.size(),encoded.size());
            assert(n>0);encoded.resize(n);
        }
        auto p=ft+i*24;Put(b,p+2,compressed?2:0,2);Put(b,p+4,raw.size());Put(b,p+8,encoded.size());Put(b,p+16,b.size(),8);
        b.insert(b.end(),encoded.begin(),encoded.end());
    }
    return b;
}
inline std::map<std::string,Bytes> files;
inline int reads=0;
inline std::size_t Read(void*,const char* path,std::uint64_t off,void* out,std::size_t size) {
    ++reads;auto it=files.find(path);if(it==files.end()) return 0;
    auto& b=it->second;if(!out) return b.size();if(off>=b.size()) return 0;
    auto n=std::min(size,b.size()-std::size_t(off));std::memcpy(out,b.data()+off,n);return n;
}
inline EdenDsmodHostApi Host() { EdenDsmodHostApi host{};host.read_romfs=Read;return host; }
inline void Fixtures(bool compressed, std::string zone_name="z_r7777", bool pipeline=false) {
    const auto zh=Hash(zone_name),spawner=Hash("synthetic-spawner");
    std::vector<Node> hidden(11,Sub({}));hidden[0]=Sub({Slot(7,20),Slot(7,10),Slot(8,30,2)});
    hidden[9]=Sub({Slot(9,100)});hidden[10]=Sub({Slot(10,100)});
    std::vector<Node> symbol(9,Sub({}));symbol[0]=Sub({Slot(7,40),Slot(11,60)});symbol[4]=symbol[0];
    if(pipeline) {
        hidden.assign(11,Sub({}));
        for(auto& sub:symbol) {
            std::vector<Node> rows;
            for(int i=819;i<826;++i) rows.push_back(Slot(i,10));
            sub=Sub(rows);
        }
        symbol[4].vectors[2].push_back(Slot(37,30));
    }
    auto hidden_b=Builder{}.Build({{}, {}, {{1,{Encounter(zh,hidden)}}}});
    auto symbol_b=Builder{}.Build({{}, {}, {{1,{Encounter(spawner,symbol)}}}});
    auto object=Node{{{1,0},{7,spawner}},{},{}};
    Node zone;
    zone.children[0].scalars[1]=zh;
    Node sym; sym.children[0]=object;
    zone.vectors[20]={sym,sym};
    auto placement=Builder{}.Build({{}, {}, {{0,{zone}}}});
    files.clear();
    files["romfs:/bin/archive/field/resident/data_table.gfpak"]=Pak({{"encount_" SW_VARIANT_ENCOUNTER_SUFFIX ".bin",hidden_b},{"encount_symbol_" SW_VARIANT_ENCOUNTER_SUFFIX ".bin",symbol_b}},compressed);
    files["romfs:/bin/archive/field/resident/placement.gfpak"]=Pak({{"synthetic.bin",placement}},compressed);
    std::string label="PLACENAME_"+zone_name+"_01";
    for(auto& c:label) if(c>='a' && c<='z') c+='A'-'a';
    Bytes tbl(18+label.size());std::memcpy(tbl.data(),"AHTB",4);Put(tbl,4,1);Put(tbl,16,label.size(),2);std::memcpy(tbl.data()+18,label.data(),label.size());
    files["romfs:/bin/message/English/script/place_name.tbl"]=tbl;
}
}
