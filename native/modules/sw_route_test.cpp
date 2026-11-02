// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic archive fixtures only. --romfs uses the env-gated test loader, never in-repo data.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include "sw_route.h"
#include "sw_encounter_fixture.h"
#include "sw_test_romfs.h"

namespace {
using namespace sw_test_encounters;
void Synthetic() {
    using namespace sw_route_encounters;
    for(bool compressed:{false,true}) {
        Fixtures(compressed);Cache c;reads=0;c.Ensure(Host());assert(c.data.zones.size()==1 && c.data.slots.size()==6);
        auto* zone=sw_route::Find(c.data,Hash("z_r7777"));assert(zone && zone->count==6 && zone->weathers_possible==1 && !zone->boost60);
        assert(c.data.slots[0].species==11 && c.data.slots[0].rate==60);
        assert(c.data.slots[1].species==7 && c.data.slots[1].weathers==((1<<0)|(1<<6)));
        // Equal rates retain slot order, duplicate species add rates, form is preserved.
        assert(c.data.slots[2].species==7 && c.data.slots[2].rate==30);
        assert(c.data.slots[3].species==8 && c.data.slots[3].form==2);
        auto rows=sw_route::Rows(c.data,*zone,0);assert(rows.size()==5);
        assert(rows[1].species==7 && rows[1].methods==2 && rows[1].rate[0]==40 && rows[1].rate[3]==30);
        auto before=reads;c.Ensure(Host());assert(reads==before);
        c.Reset();files.clear();c.Ensure(Host());assert(c.data.zones.empty());before=reads;c.Ensure(Host());assert(reads==before);
    }
    // Missing archive/member, corrupt header/offset, short compressed data, malformed FB and
    // malformed labels all fail closed: no partially parsed result can escape.
    for(auto path:{"romfs:/bin/archive/field/resident/data_table.gfpak","romfs:/bin/archive/field/resident/placement.gfpak","romfs:/bin/message/English/script/place_name.tbl"}) {
        Fixtures(true);auto saved=files[path];
        for(std::size_t n=0;n<saved.size();++n) {
            files[path]=Bytes(saved.begin(),saved.begin()+n);Data d;assert(!Load(Host(),d));assert(d.zones.empty() && d.slots.empty());
        }
    }
    Fixtures(false);
    {
        auto& bytes=files["romfs:/bin/archive/field/resident/data_table.gfpak"];
        auto ft=sw_archive::Read(bytes,24,8),member=sw_archive::Read(bytes,ft+16,8);
        Put(bytes,member,0xFFFFFFF0u); // internal FlatBuffers root, archive bounds remain valid
        Data data;assert(!Load(Host(),data) && data.zones.empty());
    }
    {
        // A deduplicated vtable can follow the object (signed negative displacement).
        Bytes bytes(20);Put(bytes,0,4);Put(bytes,4,0xFFFFFFF8u);Put(bytes,8,17);
        Put(bytes,12,6,2);Put(bytes,14,8,2);Put(bytes,16,4,2);
        assert(sw_archive::Table(bytes).Scalar(0,4)==17);
        assert(sw_archive::Table(bytes).Scalar(9,4)==0); // schema default for absent fields
        Put(bytes,16,7,2);
        bool refused=false;
        try { sw_archive::Table(bytes).Scalar(0,4); } catch(...) { refused=true; }
        assert(refused);
    }
    Fixtures(false);
    {
        auto& bytes=files["romfs:/bin/archive/field/resident/data_table.gfpak"];
        Put(bytes,40,~std::uint64_t{}-3,8); // folder offset addition must never wrap
        Data data;assert(!Load(Host(),data) && data.zones.empty());
    }
    Fixtures(false);auto& pak=files["romfs:/bin/archive/field/resident/data_table.gfpak"];Put(pak,24,~std::uint64_t{},8);Data d;assert(!Load(Host(),d) && d.zones.empty());
    constexpr std::array<int,9> weather{0,1,2,3,6,4,5,7,8};assert(kTableToLive==weather);
    Zone z{1,0,0,511,1};sw_route::Row row{};row.min_level=12;row.max_level=70;
    assert(sw_route::Levels(z,row,true)==std::make_pair(60,70));assert(sw_route::DefaultWeather(z,6)==6);
    std::puts("route: synthetic raw/LZ4 archives, merge/order/form/weather, cache/reset/failure, every truncated prefix PASS");
}
}
int main(int argc,char** argv) {
    if(argc>1 && std::string_view(argv[1])=="--romfs") {
        if(!sw_test::RomfsDir()) {std::puts("SW_ROMFS_DIR absent");return 77;}
        sw_route_encounters::Data d;assert(sw_route_encounters::Load(sw_test::RomfsHost(),d));
        assert(d.zones.size()==69);
        if(argc>2) {
            std::ofstream f(argv[2]);
            for(auto& s:d.slots) f<<"S "<<s.species<<' '<<int(s.form)<<' '<<int(s.method)<<' '<<int(s.rate)<<' '<<int(s.min_level)<<' '<<int(s.max_level)<<' '<<s.weathers<<'\n';
            for(auto& z:d.zones) f<<"Z "<<z.zone_hash<<' '<<z.first<<' '<<z.count<<' '<<z.weathers_possible<<' '<<int(z.boost60)<<'\n';
        }
        std::printf("route romfs: %zu zones, %zu slots\n",d.zones.size(),d.slots.size());return 0;
    }
    Synthetic();
}
