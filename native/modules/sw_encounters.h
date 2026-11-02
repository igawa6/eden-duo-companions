// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <map>
#include <tuple>
#include "sw_encounter_archive.h"
#include "sw_text.h"
#include "sw_variant.h"

namespace sw_route_encounters {
enum Method : std::uint8_t { Wander=0, Flying=1, Surf=2, Special=3, Grass=4, Tree=5, Fishing=6 };
struct Slot { std::uint16_t species; std::uint8_t form, method, rate, min_level, max_level; std::uint16_t weathers; };
struct Zone { std::uint64_t zone_hash; std::uint16_t first, count, weathers_possible; std::uint8_t boost60; };
inline constexpr int kBoostLevel = 60;
inline constexpr std::array<int,9> kTableToLive{0,1,2,3,6,4,5,7,8};
struct Data { std::vector<Slot> slots; std::vector<Zone> zones; };
inline std::uint16_t Weather(std::string_view zone) {
    // pkNX EncounterTableUtil.WeatherbyArea; converted to the live weather enum.
    if (zone == "z_r0601") return 0x040;
    if (zone == "z_r0901") return 0x010;
    if (zone == "z_r0902") return 0x010;
    if (zone == "z_r0903") return 0x010;
    if (zone == "z_r0904") return 0x002;
    if (zone == "z_r1001") return 0x020;
    if (zone == "z_r1002") return 0x020;
    if (zone == "z_wr0102") return 0x1FF;
    if (zone == "z_wr0103") return 0x1FF;
    if (zone == "z_wr0104") return 0x1FF;
    if (zone == "z_wr0105") return 0x1FF;
    if (zone == "z_wr0106") return 0x1FF;
    if (zone == "z_wr0107") return 0x1FF;
    if (zone == "z_wr0108") return 0x1FF;
    if (zone == "z_wr0109") return 0x1FF;
    if (zone == "z_wr0110") return 0x1FF;
    if (zone == "z_wr0121") return 0x1FF;
    if (zone == "z_wr0122") return 0x1FF;
    if (zone == "z_wr0131") return 0x1FF;
    if (zone == "z_wr0132") return 0x1FF;
    if (zone == "z_wr0133") return 0x1FF;
    if (zone == "z_wr0134") return 0x1FF;
    if (zone == "z_wr0135") return 0x1FF;
    if (zone == "z_wr0136") return 0x1FF;
    if (zone == "z_wr0201") return 0x14F;
    if (zone == "z_wr0202") return 0x14F;
    if (zone == "z_wr0203") return 0x1CF;
    if (zone == "z_wr0204") return 0x14F;
    if (zone == "z_wr0206") return 0x1CF;
    if (zone == "z_wr0208") return 0x14F;
    if (zone == "z_wr0209") return 0x1CF;
    if (zone == "z_wr0211") return 0x1C7;
    if (zone == "z_wr0221") return 0x14F;
    if (zone == "z_wr0222") return 0x14F;
    if (zone == "z_wr0223") return 0x14F;
    if (zone == "z_wr0224") return 0x14F;
    if (zone == "z_wr0231") return 0x14F;
    if (zone == "z_wr0301") return 0x173;
    if (zone == "z_wr0303") return 0x173;
    if (zone == "z_wr0304") return 0x17F;
    if (zone == "z_wr0305") return 0x17F;
    if (zone == "z_wr0306") return 0x173;
    if (zone == "z_wr0307") return 0x002;
    if (zone == "z_wr0308") return 0x173;
    if (zone == "z_wr0310") return 0x17F;
    if (zone == "z_wr0311") return 0x002;
    if (zone == "z_wr0312") return 0x13F;
    if (zone == "z_wr0313") return 0x17F;
    if (zone == "z_wr0321") return 0x15F;
    if (zone == "z_wr0322") return 0x002;
    return 1;
}

using sw_archive::Table;
using sw_archive::Require;
using Key = std::tuple<int,int,int,int,int,int>;
struct Accumulator {
    std::vector<std::pair<Key,std::uint16_t>> rows;
    void Add(const Table& sub, int method, unsigned mask) {
        auto lo = sub.Scalar(0,1), hi = sub.Scalar(1,1);
        std::vector<std::tuple<int,int,int>> merged;
        for (const auto& sl : sub.Tables(2)) {
            auto sp = sl.Scalar(1,4), form = sl.Scalar(2,1), rate = sl.Scalar(0,1);
            if (!sp || !rate) continue;
            Require(sp <= 898 && rate <= 100);
            auto it = std::find_if(merged.begin(),merged.end(),[&](auto& x){return std::get<0>(x)==int(sp) && std::get<1>(x)==int(form);});
            if (it == merged.end()) merged.emplace_back(sp,form,rate);
            else { std::get<2>(*it) += rate; Require(std::get<2>(*it)<=100); }
        }
        if (!lo || merged.empty()) return;
        Require(lo<=hi && hi<=100);
        for (auto [sp,form,rate] : merged) {
            Key key{sp,form,method,rate,lo,hi};
            auto it = std::find_if(rows.begin(),rows.end(),[&](auto& x){return x.first==key;});
            if (it==rows.end()) rows.emplace_back(key,mask); else it->second |= mask;
        }
    }
    void Weathers(const std::vector<Table>& subs, int method) {
        // Group identical weather tables before adding them: the first table's slot order wins.
        // Add() merges equal rows and stable_sort below retains that original order on rate ties.
        for (std::size_t w=0; w<std::min<std::size_t>(9,subs.size()); ++w)
            Add(subs[w],method,1u<<kTableToLive[w]);
    }
};
inline bool Load(const EdenDsmodHostApi& host, Data& out) {
    out = {};
    try {
        using namespace sw_archive;
        auto labels = sw_archive::Load(host,"romfs:/bin/message/English/script/place_name.tbl");
        std::vector<sw_text::TblEntry> entries;
        Require(labels.size() >= 8 && sw_archive::Read(labels,4,4) <= 8192);
        Require(sw_text::ReadTbl(labels.data(),labels.size(),entries));
        std::map<std::uint64_t,std::string> ids;
        for (auto& e : entries) {
            std::string_view name=e.name;
            if (!name.starts_with("PLACENAME_Z_") || !name.ends_with("_01")) continue;
            std::string z(name.substr(10,name.size()-13));
            for (auto& c:z) if(c>='A' && c<='Z') c += 'a'-'A';
            ids[Hash(z)]=z;
        }
        for (auto z : {"z_r0202","z_d0103","z_r1002"}) ids[Hash(z)]=z;
        Pak data(sw_archive::Load(host,"romfs:/bin/archive/field/resident/data_table.gfpak"));
        auto hidden=data.Named("encount_" SW_VARIANT_ENCOUNTER_SUFFIX ".bin");
        auto symbol=data.Named("encount_symbol_" SW_VARIANT_ENCOUNTER_SUFFIX ".bin");
        std::map<std::uint64_t,Table> hidden_tables, symbol_tables;
        for (auto t:Table(hidden).Tables(1)) Require(hidden_tables.emplace(t.Scalar(0,8),t).second);
        for (auto t:Table(symbol).Tables(1)) Require(symbol_tables.emplace(t.Scalar(0,8),t).second);
        Pak placement(sw_archive::Load(host,"romfs:/bin/archive/field/resident/placement.gfpak"));
        std::map<std::uint64_t,std::map<std::pair<std::uint64_t,int>,int>> spawners;
        for (std::size_t i=0;i<placement.files;++i) {
            auto member=placement.Member(i);
            if (member.size() >= 4 && std::memcmp(member.data(),"AHTB",4)==0) continue;
            for (auto z:Table(member).Tables(0)) {
                auto zh=z.Child(0).Scalar(1,8);
                for (auto s:z.Tables(20)) {
                    auto obj=s.Child(0); auto h=obj.Scalar(7,8); int kind=static_cast<std::int32_t>(obj.Scalar(1,4));
                    if(h && h!=0xCBF29CE484222645ull) ++spawners[zh][{h,kind}];
                }
            }
        }
        std::map<std::uint64_t,Accumulator> zones;
        // Symbol groups precede grass/tree/fishing, matching the grid's original tie order.
        for(auto& [zh,counts]:spawners) {
            std::vector<std::pair<std::pair<std::uint64_t,int>,int>> ordered(counts.begin(),counts.end());
            std::sort(ordered.begin(),ordered.end(),[](auto& a,auto& b){
                return std::tuple{a.first.second,-a.second,a.first.first}<std::tuple{b.first.second,-b.second,b.first.first};
            });
            for(auto& [key,count]:ordered) {
                auto it=symbol_tables.find(key.first);
                if(it!=symbol_tables.end()) zones[zh].Weathers(it->second.Tables(1),key.second>=0 && key.second<=3?key.second:Special);
            }
        }
        for(auto& [zh,t]:hidden_tables) {
            auto subs=t.Tables(1); auto& a=zones[zh]; a.Weathers(subs,Grass);
            if(subs.size()>9) a.Add(subs[9],Tree,511);
            if(subs.size()>10) a.Add(subs[10],Fishing,511);
        }
        Data result;
        for(auto& [zh,a]:zones) {
            if(a.rows.empty()) continue;
            Require(ids.contains(zh));
            std::stable_sort(a.rows.begin(),a.rows.end(),[](auto& a,auto& b){return std::tuple{std::get<2>(a.first),-std::get<3>(a.first)}<std::tuple{std::get<2>(b.first),-std::get<3>(b.first)};});
            Require(result.slots.size()+a.rows.size()<=65535);
            result.zones.push_back({zh,static_cast<std::uint16_t>(result.slots.size()),static_cast<std::uint16_t>(a.rows.size()),Weather(ids.at(zh)),static_cast<std::uint8_t>(ids.at(zh).starts_with("z_wr"))});
            for(auto& [key,mask]:a.rows) {
                auto [sp,form,method,rate,lo,hi]=key;
                result.slots.push_back({static_cast<std::uint16_t>(sp),static_cast<std::uint8_t>(form),static_cast<std::uint8_t>(method),static_cast<std::uint8_t>(rate),static_cast<std::uint8_t>(lo),static_cast<std::uint8_t>(hi),mask});
            }
        }
        Require(!result.zones.empty()); out=std::move(result); return true;
    } catch (...) { return false; }
}
struct Cache {
    Data data;
    bool attempted=false;
    void Ensure(const EdenDsmodHostApi& host) { if(!attempted) { attempted=true; Load(host,data); } }
    void Reset() { data={}; attempted=false; }
};
} // namespace sw_route_encounters
