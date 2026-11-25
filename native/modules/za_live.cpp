// SPDX-FileCopyrightText: Copyright 2026 Sickops
// SPDX-License-Identifier: GPL-3.0-or-later
// Z-A 2.0.2: read-only access to the game's encrypted live SCBlock table.
// Root independently documented by SysBot LZA; entry layout and actual party/coordinate
// records verified in the running Windows Eden process. Other builds fail closed.
#include "za_live.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>
#include <unordered_set>
namespace za { namespace {
template<class T> T At(const std::uint8_t* bytes){T out{};std::memcpy(&out,bytes,sizeof(out));return out;}
bool Read(const EdenDsmodHostApi& host,std::uint64_t at,void* out,std::size_t count){
    if(at<0x10000||at>=(1ull<<39)||count>(1u<<20)||at+count<at||!out||!host.is_mapped)return false;
    // ReadBlock synchronizes rasterizer pages, including unrelated GPU allocations
    // encountered by a scan. CPU-owned script data needs no GPU download. The host
    // promises a contiguous pointer only within one page: validate/copy each page.
    if(host.get_read_pointer){
        for(std::size_t offset=0;offset<count;){
            const auto n=std::min<std::size_t>(count-offset,4096-((at+offset)&4095));
            if(!host.is_mapped(host.userdata,at+offset,n))return false;
            const auto* source=host.get_read_pointer(host.userdata,at+offset,n);
            if(!source)return false;
            std::memcpy(static_cast<std::uint8_t*>(out)+offset,source,n);offset+=n;
        }
        return true;
    }
    return host.read_memory&&host.is_mapped(host.userdata,at,count)&&host.read_memory(host.userdata,at,out,count);
}
// A scan window may straddle unmapped guest pages even when its useful objects
// are valid. Preserve readable pages and leave holes zeroed; never read through a hole.
bool ScanRead(const EdenDsmodHostApi& host,std::uint64_t at,std::vector<std::uint8_t>& out){
    std::fill(out.begin(),out.end(),0);
    if(Read(host,at,out.data(),out.size()))return true;
    bool any=false;
    for(std::size_t offset=0;offset<out.size();){const auto count=std::min<std::size_t>(out.size()-offset,4096-((at+offset)&4095));
        any=Read(host,at+offset,out.data()+offset,count)||any;offset+=count;}
    return any;
}
std::uint64_t Pointer(const EdenDsmodHostApi& host,std::uint64_t at){
    std::uint64_t out{};return Read(host,at,&out,sizeof(out)) ? out : 0;
}
bool ReadMapPins(const EdenDsmodHostApi& host, u64 model, std::vector<QuestMapPin>& pins,
                 std::vector<LandmarkPin>& landmarks, MapMarker& marker,
                 std::vector<UnlockedTravelPoint>& unlocked_travel, bool focused) {
    constexpr std::size_t group_size = 32, point_size = 0x90, point_limit = 1024;
    const auto begin = Pointer(host, model + 0x50), end = Pointer(host, model + 0x58);
    if (!begin || end <= begin || (end - begin) % group_size ||
        end - begin > 256 * group_size) {
        return false;
    }
    std::vector<std::uint8_t> groups(end - begin);
    if (!Read(host, begin, groups.data(), groups.size())) return false;
    std::size_t total = 0;
    std::vector<QuestMapPin> result;
    std::vector<LandmarkPin> destinations;
    std::vector<UnlockedTravelPoint> unlocked;
    const auto catalog = Pointer(host, model + 0x4a8);
    const auto entries = catalog ? Pointer(host, catalog + 0x120) : 0;
    u32 entry_count = 0;
    if (entries) Read(host, catalog + 0x160, &entry_count, sizeof(entry_count));
    MapMarker current_marker;
    std::vector<std::uint8_t> points;
    for (std::size_t group = 0; group < groups.size(); group += group_size) {
        const auto first = At<u64>(groups.data() + group);
        const auto last = At<u64>(groups.data() + group + 8);
        if (first == last) continue;
        if (!first || last < first || (last - first) % point_size ||
            (last - first) / point_size > point_limit - total) return false;
        points.resize(last - first);
        if (!Read(host, first, points.data(), points.size())) return false;
        total += points.size() / point_size;
        for (std::size_t offset = 0; offset < points.size(); offset += point_size) {
            const auto* point = points.data() + offset;
            const auto kind = At<u32>(point + 4);
            const auto id = At<u32>(point + 0x28);
            // Match the game's travel dispatch check, including its catalog entry.
            if ((kind==4 || kind==9 || kind==13 || kind==15) && (point[0]&2) &&
                entries && entry_count<=4096 && id<entry_count) {
                const auto entry = entries + u64{id}*32;
                const auto key = Pointer(host, entry);
                if (key && Pointer(host, entry + 0x10)) unlocked.push_back({kind,id,key});
            }
            if(kind==1 && group==group_size){
                const float x=At<float>(point+8),y=At<float>(point+12);
                if(std::isfinite(x)&&std::isfinite(y)&&std::hypot(x,y)<=2048.f)
                    current_marker={true,(point[0]&0x82)==2,x,y};
            }
            if (id == 0 || id > 512) continue;
            const bool quest=(kind>=2 && kind<=5) && (point[0]&1);
            const bool landmark=kind==15 && (point[0]&1);
            if (!quest && !landmark) continue;
            const float x = At<float>(point + 8), y = At<float>(point + 12);
            if (!std::isfinite(x) || !std::isfinite(y) || std::hypot(x, y) > 2048.f) continue;
            if (quest) {
                const int category=kind<=3?0:2;
                if(!std::ranges::any_of(result,[&](const auto& pin){return pin.category==category&&pin.id==id&&std::hypot(pin.x-x,pin.y-y)<.01f;}))
                    result.push_back({category,int(id),x,y});
            }
            else destinations.push_back({int(id),x,y});
        }
    }
    if (Pointer(host, model + 0x50) != begin || Pointer(host, model + 0x58) != end ||
        Pointer(host, model) != host.main_base + 0x3DF1D48 || Pointer(host, model + 0x10) != model) {
        return false;
    }
    // Closing the native map can leave its model alive with empty quest groups.
    // Keep the last loaded points. The current mission state still filters their display.
    if (focused || !result.empty()) pins = std::move(result);
    landmarks = std::move(destinations);
    if (catalog && (Pointer(host, model + 0x4a8)!=catalog ||
                    Pointer(host, catalog + 0x120)!=entries)) unlocked.clear();
    unlocked_travel = std::move(unlocked);
    marker=current_marker;
    return true;
}
struct Cipher {
    std::uint32_t state;unsigned byte=0;
    static std::uint32_t Step(std::uint32_t v){v^=v<<2;v^=v>>15;v^=v<<13;return v;}
    explicit Cipher(std::uint32_t key):state(key){for(int i=std::popcount(key);i;--i)state=Step(state);}
    std::uint8_t Next(){auto result=static_cast<std::uint8_t>(state>>(byte*8));if(++byte==4){state=Step(state);byte=0;}return result;}
};
// Native context getters (439B0/105DD0 in 2.0.2) use generation-checked
// handles. Follow the same ownership path read-only; never take the first
// checksum-valid party-shaped buffer from the many old copies in the heap.
u64 NativeObject(const EdenDsmodHostApi& host,u64 handle_at,u64 type){
    const auto key=Pointer(host,handle_at),base=Pointer(host,host.main_base+0x3EC21C8);
    if(!key||!base)return 0;
    const auto entry=base+(key&0xFFFFF)*32+0x10;
    std::array<std::uint8_t,32> state{};
    if(!Read(host,entry,state.data(),state.size())||At<u64>(state.data())!=key||
       !At<std::uint16_t>(state.data()+0x18)||!At<std::uint16_t>(state.data()+0x1A)||
       At<std::uint32_t>(state.data()+0x1C)==0xFFFFFFFF)return 0;
    const auto face=At<u64>(state.data()+0x10);if(face<0x10028)return 0;
    const auto object=face-0x28;
    // Components with multiple native interfaces have extra vtables at +30/+38;
    // the registry owns the handle, not those fields in the resolved object.
    return Pointer(host,object)==host.main_base+type&&Pointer(host,object+0x10)==object&&
           Pointer(host,handle_at)==key&&Pointer(host,entry)==key?object:0;
}
u64 NativePartyStorage(const EdenDsmodHostApi& host){
    auto typed=[&](u64 object,u64 type){return object&&Pointer(host,object)==host.main_base+type&&Pointer(host,object+0x10)==object;};
    const auto manager=Pointer(host,host.main_base+0x610B810);
    if(!typed(manager,0x3D2B8C8))return 0;
    const auto context=Pointer(host,manager+0x48);
    if(!typed(context,0x3D2C490))return 0;
    const auto actor=NativeObject(host,context+0x40,0x3D2A8E8);if(!actor)return 0;
    const auto components=Pointer(host,actor+0x2038);
    if(!typed(components,0x3D2A770))return 0;
    const auto party=NativeObject(host,components+0x100,0x3D81450);if(!party)return 0;
    const auto storage=Pointer(host,party+0x190);
    if(!typed(storage,0x3E2D1D0))return 0;
    const auto actors=Pointer(host,storage+0x70),count=Pointer(host,storage+0x78);
    const auto nodes=Pointer(host,storage+0x98),node_count=Pointer(host,storage+0xA0);
    if(actors<0x10000||nodes<0x10000||count<1||count>6||node_count!=count)return 0;
    return Pointer(host,manager+0x48)==context&&
        NativeObject(host,context+0x40,0x3D2A8E8)==actor&&
        NativeObject(host,components+0x100,0x3D81450)==party?storage:0;
}
bool Block(const EdenDsmodHostApi& host,std::uint64_t entry,std::uint32_t key,std::size_t expected,std::vector<std::uint8_t>& out){
    std::array<std::uint8_t,32> descriptor{};
    if(!Read(host,entry,descriptor.data(),descriptor.size()) || At<std::uint32_t>(descriptor.data())!=key)return false;
    const auto data=At<std::uint64_t>(descriptor.data()+8), size=At<std::uint64_t>(descriptor.data()+16);
    if(size!=expected+5)return false;
    out.resize(size);
    if(!Read(host,data,out.data(),out.size()))return false;
    Cipher cipher(key);for(auto& b:out)b^=cipher.Next();
    if(out[0]!=4 || At<std::uint32_t>(out.data()+1)!=expected)return false;
    out.erase(out.begin(),out.begin()+5);return true;
}
// Native party storage (Z-A 2.0.2), verified against an unsaved team change.
// Each slot owns separate encrypted stored data (0x148) and party stats (0x10).
// Read all six atomically by identity; never replace a changed team with its save snapshot.
bool WorkingPartyArrays(const EdenDsmodHostApi& host,u64 nodes_at,u64 actors_at,int slots,
                  std::array<Entity,6>& rows,int& count,decltype(LiveSnapshot::recharge)& recharge,int& deployed,
                  bool handles=false,std::array<u64,6>* verified_nodes=nullptr){
    std::array<std::uint64_t,6> nodes{},after{};
    if(!nodes_at||slots<1||slots>6||!Read(host,nodes_at,nodes.data(),slots*sizeof(u64)))return false;
    const auto keys=nodes;
    if(handles)for(int i=0;i<slots;++i){
        nodes[i]=NativeObject(host,nodes_at+i*sizeof(u64),0x3E2A438);
        if(!nodes[i])return false;
    }
    count=0;std::unordered_set<std::uint64_t> identities;
    for(int i=0;i<6;++i){
        if(!nodes[i])continue;
        if(Pointer(host,nodes[i])!=host.main_base+0x3e2a438)return false;
        const auto wrapper=Pointer(host,nodes[i]+0x50);
        if(!wrapper||Pointer(host,nodes[i]+0x10)!=nodes[i]||Pointer(host,wrapper+0x10)!=wrapper)return false;
        std::array<std::uint64_t,8> descriptors{};
        if(!Read(host,wrapper+0x30,descriptors.data(),sizeof(descriptors))||descriptors[2]!=kStoredSize||descriptors[6]!=16)return false;
        std::array<std::uint8_t,kPartySize> bytes{};
        if(!Read(host,descriptors[0],bytes.data(),kStoredSize)||!Read(host,descriptors[4],bytes.data()+kStoredSize,16))return false;
        if(std::all_of(bytes.begin(),bytes.begin()+kStoredSize,[](auto x){return x==0;}))continue;
        auto& e=rows[i];
        if(!DecryptEntity(bytes.data(),bytes.size(),e)||!e.checksum_ok||e.species>1025||e.level<1||e.level>100||!e.hp_max||e.hp>e.hp_max)return false;
        if(!identities.insert((std::uint64_t(e.pid)<<32)|e.encryption_constant).second)return false;
        // The battle node stores remaining and initial cooldowns as four floats each.
        // Use only the node type verified in 2.0.2 and the same slot/identity as the live party.
        std::array<float,8> timers{};
        if(Pointer(host,nodes[i])==host.main_base+0x3e2a438&&
           Read(host,nodes[i]+0x64,timers.data(),sizeof(timers))){
            for(int move=0;move<4;++move){
                const float remaining=timers[move],initial=timers[move+4];
                if(e.moves[move]&&initial>=0&&initial<=60&&remaining>=0&&remaining<=initial)
                    recharge[i][move]={remaining,initial,true};
            }
        }
        ++count;
    }
    if(count<=0||!Read(host,nodes_at,after.data(),slots*sizeof(u64))||keys!=after)return false;
    if(handles)for(int i=0;i<slots;++i)
        if(NativeObject(host,nodes_at+i*sizeof(u64),0x3E2A438)!=nodes[i])return false;
    if(verified_nodes)*verified_nodes=nodes;

    // Each slot has a field actor in addition to its battle data. A recalled actor stays
    // allocated, but its deployed flag clears; the HUD selection is a separate object.
    std::array<u64,6> actors{},verified{};
    if(!actors_at||!Read(host,actors_at,actors.data(),slots*sizeof(u64)))return true;
    int active=-1;
    bool complete=true;
    for(int slot=0;slot<6;++slot){
        if(!actors[slot])continue;
        if(Pointer(host,actors[slot])!=host.main_base+0x3be0d28||
           Pointer(host,actors[slot]+0x10)!=actors[slot]){
            complete=false;
            continue;
        }
        std::uint8_t flag=0;
        if(!Read(host,actors[slot]+0x3ab,&flag,1)||flag>1){
            complete=false;
            continue;
        }
        if(flag){
            if(active>=0||!rows[slot].present)return true;
            active=slot;
        }
    }
    if((active>=0||complete)&&Read(host,actors_at,verified.data(),slots*sizeof(u64))&&actors==verified)
        deployed=active;
    return true;
}
// Compatibility fallback for older hosts/known captures. Native authority comes
// from the two separately allocated vectors above, never an inferred distance.
bool WorkingPartyAt(const EdenDsmodHostApi& host,u64 root,std::array<Entity,6>& rows,int& count,
                    decltype(LiveSnapshot::recharge)& recharge,int& deployed){
    return root&&WorkingPartyArrays(host,root+0xF0,root+0x60,6,rows,count,recharge,deployed);
}

}
// The SCBlock box is a save snapshot. The native box service owns current PA9s
// and an occupancy mask; empty cells can retain old serialized bytes.
namespace {
bool WorkingBoxAt(const EdenDsmodHostApi& host,u64 service,int box,std::array<Entity,30>& rows){
    if(box<0||box>=32||Pointer(host,service)!=host.main_base+0x3D59CF0||
       Pointer(host,service+0x10)!=service||Pointer(host,service+0x980)!=32)return false;
    const auto data=Pointer(host,service+0x978);
    std::array<u64,15> occupied{},after{};
    std::array<std::uint8_t,30*kBoxSlotSize> bytes{},verified{};
    const auto address=data+u64(box)*30*kBoxSlotSize;
    if(!data||!Read(host,service+0x40,occupied.data(),sizeof(occupied))||
       !Read(host,address,bytes.data(),bytes.size()))return false;
    rows={};std::unordered_set<u64> identities;
    for(int slot=0;slot<30;++slot){
        const int index=box*30+slot;if(!(occupied[index/64]&(1ull<<(index%64))))continue;
        auto& e=rows[slot];
        const auto* raw=bytes.data()+slot*kBoxSlotSize;
        if(!DecryptEntity(raw,kPartySize,e)||!e.present)return false;
        if(!e.checksum_ok){
            // The native service can retain an explicitly flagged Bad Egg. It
            // must not invalidate every other cell, nor authorize an exchange
            // of that record. Verify the body checksum on a LOCAL copy with
            // only the known flag cleared; unknown flags or damaged bodies
            // still reject the snapshot. Never clear the game's own flag.
            if(At<std::uint16_t>(raw+4)!=4)return false;
            std::array<std::uint8_t,kPartySize> check{};
            std::copy_n(raw,kPartySize,check.data());check[4]=0;
            Entity verified;
            if(!DecryptEntity(check.data(),check.size(),verified)||!verified.checksum_ok)return false;
            e.egg=true;
        }
        if(e.species>1025||e.level<1||e.level>100||!e.hp_max||e.hp>e.hp_max||
           !identities.insert((u64(e.pid)<<32)|e.encryption_constant).second)return false;
    }
    return Read(host,service+0x40,after.data(),sizeof(after))&&occupied==after&&
           Read(host,address,verified.data(),verified.size())&&bytes==verified&&
           Pointer(host,service+0x978)==data&&Pointer(host,service)==host.main_base+0x3D59CF0;
}
u64 BoxService(const EdenDsmodHostApi& host){
    const auto begin=Pointer(host,host.main_base+0x610A710),end=Pointer(host,host.main_base+0x610A718);
    if(!begin||end<begin||(end-begin)%8||end-begin>256*8)return 0;
    for(auto p=begin;p<end;p+=8){const auto service=Pointer(host,p);
        if(Pointer(host,service)==host.main_base+0x3D59CF0&&Pointer(host,service+0x10)==service)return service;}
    return 0;
}
}
bool LiveReader::LastHP(const Entity& entity,std::uint16_t& hp) const {
    const auto found=confirmed_hp_.find((std::uint64_t(entity.pid)<<32)|entity.encryption_constant);
    if(found==confirmed_hp_.end()||found->second>entity.hp_max)return false;
    hp=found->second;return true;
}
LiveSnapshot LiveReader::Poll(const EdenDsmodHostApi& host,bool inspect_boxes,int requested_box,u64 preferred_map){
    LiveSnapshot result;
    if(!host.main_base){result.diagnostic="Memory: game base unavailable";result.problem=true;return result;}
    if(!BuildMatches(host.build_id)){
        // Some hosts may expose stale metadata. Accept that only if all nine relative
        // function pointers in three independent 2.0.2 vtables match the verified image.
        constexpr std::uint64_t signatures[][4]={{0x3da4348,0x2add89c,0x269fe70,0x269fe7c},{0x3de1a20,0x2c05980,0x269fe70,0x269fe7c},{0x3bd3b30,0xfe6214,0xfe6290,0xfe56f4}};
        bool verified=true;
        for(const auto& sig:signatures){std::array<std::uint64_t,3> words{};
            if(!Read(host,host.main_base+sig[0],words.data(),sizeof(words))){verified=false;break;}
            for(int i=0;i<3;++i)if(words[i]!=host.main_base+sig[i+1])verified=false;
        }
        if(!verified){char id[41];for(int i=0;i<20;++i)std::snprintf(id+i*2,3,"%02X",host.build_id[i]);
            result.diagnostic=std::string("Unsupported game build ")+id+". Enable the Z-A 2.0.2 update and restart Eden.";result.problem=true;return result;}
    }
    result.diagnostic="Memory: game manager unavailable";result.problem=true;
    const auto tick=host.get_tick?host.get_tick(host.userdata):0;
    // The game's za_rank property is copied from this initialized native owner
    // (33D288). Points can reach a threshold before promotion: never infer rank.
    const auto rank=Pointer(host,host.main_base+0x3EC3760);
    std::array<u8,0x44> rank_data{},rank_again{};
    if(Read(host,rank,rank_data.data(),rank_data.size())&&
       At<u64>(rank_data.data())==host.main_base+0x3DA5E80&&At<u32>(rank_data.data()+0x40)==1&&
       Read(host,rank,rank_again.data(),rank_again.size())&&rank_data==rank_again&&
       Pointer(host,host.main_base+0x3EC3760)==rank){
        const auto r=At<u32>(rank_data.data()+0x18),p=At<u32>(rank_data.data()+0x1C);
        if(r>=1&&r<=27&&p<=10000000){result.rank_live=true;result.trainer_rank=int(r);result.ticket_points=int(p);}
    }
    // The same active-name list used by za_battlezone_activated (DBC6E0),
    // owned by the current world manager. Empty names mean inactive zones.
    const auto world_slot=Pointer(host,host.main_base+0x3EC71C8);
    const auto world=NativeObject(host,world_slot,0x3D71CC0);
    const auto zones=Pointer(host,world+0xF8);
    if(world&&Pointer(host,zones)==host.main_base+0x3D83680&&Pointer(host,zones+0x10)==zones){
        const auto count=Pointer(host,zones+0x110),head=Pointer(host,zones+0x100);
        auto node=head;bool valid=count<=32;std::unordered_set<u64> visited;
        for(u64 i=0;valid&&node&&i<count;++i){
            if(!visited.insert(node).second){valid=false;break;}
            if(Pointer(host,node+8)==0x128D7F07BDB7E0B6ull){ // area t1
                const auto begin=Pointer(host,node+0x28),end=Pointer(host,node+0x30);
                if(end<begin||(end-begin)%16||end-begin>32*16){valid=false;break;}
                std::vector<u8> entries(end-begin),after(entries.size());
                if(!entries.empty()&&!Read(host,begin,entries.data(),entries.size())){valid=false;break;}
                for(std::size_t n=0;n<entries.size();n+=16){
                    const auto hash=Pointer(host,At<u64>(entries.data()+n));
                    if(hash&&hash!=0xCBF29CE484222645ull)result.active_battle_zones.push_back(hash);
                }
                valid=(entries.empty()||Read(host,begin,after.data(),after.size()))&&entries==after&&
                      Pointer(host,node+0x28)==begin&&Pointer(host,node+0x30)==end;
            }
            node=Pointer(host,node);
        }
        result.battle_zones_live=valid&&!node&&Pointer(host,zones+0x100)==head&&Pointer(host,zones+0x110)==count&&
                                   NativeObject(host,world_slot,0x3D71CC0)==world&&Pointer(host,world+0xF8)==zones;
        if(!result.battle_zones_live)result.active_battle_zones.clear();
    }
    // Active services own the research and Royale counters; the encrypted save blocks
    // for these structures are only snapshots. Never scan for a matching naked number.
    const auto services=Pointer(host,host.main_base+0x610A710),services_end=Pointer(host,host.main_base+0x610A718);
    if(services&&services_end>=services&&(services_end-services)%8==0&&services_end-services<=256*8){
        for(auto at=services;at<services_end;at+=8){
            const auto object=Pointer(host,at),type=Pointer(host,object);
            if(!object||Pointer(host,object+0x10)!=object)continue;
            if(type==host.main_base+0x3DA3308){
                // Hyperspace service: fifty bounded 0xC8 records, followed by
                // their count. Read both snapshots; discard torn/regenerated lists.
                std::array<u8,0x28B8> data{},again{};
                if(Read(host,object+0x40,data.data(),data.size())&&Read(host,object+0x40,again.data(),again.size())&&data==again&&
                   At<u32>(data.data()+10000)<=50&&Pointer(host,at)==object&&Pointer(host,object)==type){
                    const auto ascii=[](const u8* p,std::size_t size){std::size_t n=0;while(n<size&&p[n]>=32&&p[n]<127)++n;
                        return n&&n<size&&p[n]==0?std::string(reinterpret_cast<const char*>(p),n):std::string{};};
                    bool valid=true;
                    for(u32 i=0;i<At<u32>(data.data()+10000);++i){const auto* p=data.data()+i*200;
                        if(p[0x60]>3)continue;
                        const bool special=p[0x60]>=2;
                        if(!special&&(p[0x62]<1||p[0x62]>3))continue;
                        Dimension d;d.kind=p[0x60];d.type=p[0x63];d.content=ascii(p+0x40,32);d.gate=ascii(p+0xA8,32);
                        // Native deserializer 0x98795C: kind, zero-based rank, type count,
                        // then three types and their independently drawn encounter patterns.
                        d.rank=special ? -1 : p[0x61];
                        if(special)d.type=-1;
                        else for(int j=0;j<p[0x62];++j){d.types[j]=p[0x63+j];d.patterns[j]=p[0x66+j];}
                        d.x=At<float>(p+0x8C);d.y=At<float>(p+0x90);d.z=At<float>(p+0x94);
                        if(d.rank>4||std::any_of(d.types.begin(),d.types.end(),[](int t){return t>17;})||(!special&&d.content.empty())||d.gate.empty()||!std::isfinite(d.x)||!std::isfinite(d.y)||!std::isfinite(d.z)||
                           std::abs(d.x)>10000||std::abs(d.y)>10000||std::abs(d.z)>10000){if(special)continue;valid=false;break;}
                        result.dimensions.push_back(std::move(d));
                    }
                    result.dimensions_live=valid;if(!valid)result.dimensions.clear();
                }
            }else if(type==host.main_base+0x3DA4D10){
                std::array<std::uint32_t,3> data{},again{};
                if(Read(host,object+0x40,data.data(),sizeof(data))&&Read(host,object+0x40,again.data(),sizeof(again))&&data==again&&
                   data[0]>=1&&data[0]<=50&&data[1]<=data[0]&&data[2]<=10000000&&Pointer(host,at)==object&&Pointer(host,object)==type){
                    result.research_live=true;result.research_level=int(data[0]);result.research_claimed=int(data[1]);result.research_points=int(data[2]);
                }
            }else if(type==host.main_base+0x3DA5730){
                std::array<std::uint32_t,6> data{},again{};
                if(Read(host,object+0x40,data.data(),sizeof(data))&&Read(host,object+0x40,again.data(),sizeof(again))&&data==again&&
                    data[2]<=10000000&&data[3]<=10000000&&Pointer(host,at)==object&&Pointer(host,object)==type){
                    result.royale_live=true;result.royale_wins=int(data[2]);result.royale_medals=int(data[3]);
                }
            }
        }
    }
    auto selected_object=[&](std::uint64_t hud){
        if(!hud||Pointer(host,hud)!=host.main_base+0x3DE1A20)return false;
        const auto view=Pointer(host,hud+0xB8);
        const auto selector=view?Pointer(host,view+0x368):0;
        std::array<std::uint8_t,16> selection{};
        if(selector&&Read(host,selector,selection.data(),selection.size())&&
           At<std::uint64_t>(selection.data())==host.main_base+0x3BD3B30){
            const auto index=At<std::uint32_t>(selection.data()+8),count=At<std::uint32_t>(selection.data()+12);
            if(count>=1&&count<=6&&index<count){result.selected_index=int(index);return true;}
        }return false;
    };
    auto position=[&](std::uint64_t object){
        std::array<std::uint8_t,80> data{};
        if(!Read(host,object,data.data(),data.size())||At<std::uint64_t>(data.data())!=host.main_base+0x3DA4348)return false;
        const auto* area=reinterpret_cast<const char*>(data.data()+0x18);
        auto length=std::find(area,area+24,0)-area;
        const auto x=At<float>(data.data()+0x38),y=At<float>(data.data()+0x3C),z=At<float>(data.data()+0x40);
        if(!length||length>=24||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||std::abs(x)>100000||std::abs(y)>100000||std::abs(z)>100000)return false;
        result.area.assign(area,length);result.x=x;result.y=y;result.z=z;
        result.coordinates_valid=result.position_live=true;return true;
    };
    // These are entries in runtime pointer registries, whose slots move between sessions.
    // Validate cached objects by their build-specific vtable, rediscover only on failure.
    auto selected=[&](std::uint64_t slot){const auto object=Pointer(host,slot);if(selected_object(object)){selector_object_=object;return true;}return false;};
    const bool have_selection=selected_object(selector_object_)||selected(selector_slot_?selector_slot_:host.main_base+0x48891E0);
    const bool have_position=position(position_object_);
    if((!have_selection||!have_position)&&tick>=discovery_retry_){
        discovery_retry_=tick+300;
        const auto heap_begin=host.get_heap_begin?host.get_heap_begin(host.userdata):0;
        const auto heap_end=host.get_heap_end?host.get_heap_end(host.userdata):0;
        auto scan=[&](std::uint64_t offset,std::size_t size,auto&& accept){
            std::vector<std::uint8_t> pointers(size);
            if(!ScanRead(host,host.main_base+offset,pointers))return;
            for(std::size_t at=0;at+8<=size;at+=8){const auto object=At<std::uint64_t>(pointers.data()+at);
                // NCE and Dynarmic place the guest heap at different addresses.
                const bool in_heap=heap_begin&&heap_end>heap_begin ? object>=heap_begin&&object<heap_end : object>=0x10000&&object<(1ull<<39);
                const bool mapped_alias=object>=0x10000&&object<(1ull<<39)&&host.is_mapped&&host.is_mapped(host.userdata,object,8);
                if((in_heap||mapped_alias)&&accept(host.main_base+offset+at,object))break;
            }
        };
        // BSS holds allocator/owner roots; an object's offset within its allocation varies.
        // Registry references in the data segment can move by megabytes across launches.
        // Inspect only the first 512 bytes of distinct validated roots, not the guest heap.
        bool found_position=have_position,found_selection=have_selection;
        std::unordered_set<std::uint64_t> visited;
        scan(0x610B000,0x70000,[&](auto,auto root){
            if(!visited.insert(root).second)return false;
            for(std::uint64_t offset=0;offset<512;offset+=8){
                const auto object=root+offset,kind=Pointer(host,object);
                if(!found_position&&kind==host.main_base+0x3DA4348&&position(object)){position_object_=object;found_position=true;}
                if(!found_selection&&kind==host.main_base+0x3DE1A20&&selected_object(object)){selector_object_=object;found_selection=true;}
                if(found_position&&found_selection)return true;
            }return false;
        });
        if(!found_position)scan(0x40F0000,0x20000,[&](auto,auto object){if(position(object)){position_object_=object;return true;}return false;});
        if(!found_selection)scan(0x4800000,0x100000,[&](auto slot,auto){if(selected(slot)){selector_slot_=slot;return true;}return false;});
    }
    bool map_focused = false;
    auto map_object=[&](std::uint64_t object){
        std::array<std::uint8_t,0x588> data{};
        if(!Read(host,object,data.data(),data.size())||At<std::uint64_t>(data.data())!=host.main_base+0x3DF1D48||At<std::uint64_t>(data.data()+0x10)!=object)return false;
        const auto mode=At<std::uint32_t>(data.data()+0x208),destination=At<std::uint32_t>(data.data()+0x2A8);
        if(mode>3||destination>10000)return false;
        // The view remains allocated after closing/warping. Focus is relinquished
        // both when closed and to the travel dialog. Mode 4 is an inactive cached
        // destination list; the controller primes it with Plus before navigation.
        const bool focused=(At<std::uint32_t>(data.data()+0x584)&0x100)!=0;
        map_focused = focused;
        result.map_mode=focused?int(mode):(mode==2?4:3);result.map_destination=int(destination);return true;
    };
    if(preferred_map&&preferred_map!=map_object_&&map_object(preferred_map)){
        map_object_=preferred_map;
        next_quest_pin_poll_=0;
    }else if(!map_object(map_object_)){
        if(map_object_){
            unlocked_travel_.clear();
            next_quest_pin_poll_=0;
        }
        map_object_=0;
        const auto system=Pointer(host,host.main_base+0x610A000),heaps=system?Pointer(host,system+0x5F8):0;
        const auto ui_heap=heaps?Pointer(host,heaps+0x6A8):0;
        const auto begin=Pointer(host,ui_heap)==host.main_base+0x3BCE178?Pointer(host,ui_heap+0x38):0;
        if(begin>=0x10000&&begin<(1ull<<39)){
            if(begin!=ui_scan_begin_){ui_scan_begin_=begin;ui_scan_cursor_=0;ui_scan_retry_=0;}
            constexpr std::uint64_t Window=128ull<<20,Chunk=1ull<<20;
            if(ui_scan_cursor_>=Window&&tick>=ui_scan_retry_)ui_scan_cursor_=0;
            std::vector<std::uint8_t> bytes(Chunk);
            bool scanned=false;
            for(int budget=0;budget<4&&ui_scan_cursor_<Window&&!map_object_;++budget){
                scanned=true;
                const auto address=begin+ui_scan_cursor_;ui_scan_cursor_+=Chunk;
                if(!ScanRead(host,address,bytes))continue;
                for(std::size_t at=0;at+8<=bytes.size();at+=8)if(At<std::uint64_t>(bytes.data()+at)==host.main_base+0x3DF1D48&&map_object(address+at)){map_object_=address+at;break;}
            }
            if(scanned&&ui_scan_cursor_>=Window)ui_scan_retry_=tick+300;
        }
    }
    if (map_object_ && tick >= next_quest_pin_poll_) {
        if (ReadMapPins(host,map_object_,quest_pins_,landmark_pins_,map_marker_,unlocked_travel_,map_focused)) {
            next_quest_pin_poll_ = tick + 30;
            quest_map_loaded_ |= map_focused || !quest_pins_.empty();
        } else unlocked_travel_.clear();
    }
    result.quest_pins = quest_pins_;
    result.quest_map_loaded=quest_map_loaded_;
    result.landmark_pins = landmark_pins_;
    if (map_object_) result.unlocked_travel = unlocked_travel_;
    result.map_marker=map_marker_;
    const auto* page=host.get_text?host.get_text(host.userdata,"__page"):nullptr;
    const bool want_boxes=inspect_boxes||(page&&(std::strcmp(page,"box")==0||std::strcmp(page,"box_detail")==0||std::strcmp(page,"box_exchange")==0||std::strcmp(page,"moves")==0));
    std::uint32_t phase=~0u;
    result.scene_idle=Read(host,host.main_base+0x612D920,&phase,sizeof(phase))&&phase==0;
    if(want_boxes){
        const int wanted=std::clamp(requested_box>=0?requested_box:(host.get_i64?int(host.get_i64(host.userdata,"za.box.view",0)):0),0,31);
        if(!box_service_)box_service_=BoxService(host);
        if(box_service_&&WorkingBoxAt(host,box_service_,wanted,result.box_view)){
            result.box_valid=result.box_live=true;result.box_view_index=wanted;
        }else box_service_=0;
    }
    auto box_object=[&](std::uint64_t object){
        std::array<std::uint8_t,0x100> b{};
        if(!Read(host,object,b.data(),b.size())||At<std::uint64_t>(b.data())!=host.main_base+0x3DB4880||At<std::uint64_t>(b.data()+0x10)!=object)return false;
        const auto box=At<std::uint64_t>(b.data()+0xE0),party=At<std::uint64_t>(b.data()+0xE8);
        auto index=[&](std::uint64_t view,int max){
            if(Pointer(host,view)!=host.main_base+0x3BD0E60)return -1;
            const auto selector=Pointer(host,view+0x368);std::array<std::uint8_t,16> data{};
            if(!Read(host,selector,data.data(),data.size())||At<std::uint64_t>(data.data())!=host.main_base+0x3BD3B30||At<std::uint32_t>(data.data()+12)!=6)return -1;
            const int value=int(At<std::uint32_t>(data.data()+8));return value>=0&&value<max?value:-1;
        };
        const int cursor=index(box,30),team=index(party,6);if(cursor<0||team<0)return false;
        result.boxes.cursor=cursor;result.boxes.party_cursor=team;return true;
    };
    auto drag_object=[&](std::uint64_t drag){
        std::array<std::uint8_t,0xD8> state{};
        if(!Read(host,drag,state.data(),state.size())||At<std::uint64_t>(state.data())!=host.main_base+0x3DB4D18||At<std::uint64_t>(state.data()+0x10)!=drag)return false;
        const int from=int(At<std::uint32_t>(state.data()+0xC8)),slot=int(At<std::uint32_t>(state.data()+0xCC));
        const int to=int(At<std::uint32_t>(state.data()+0xD0)),target=int(At<std::uint32_t>(state.data()+0xD4));
        auto kind=[](int v){return (v>=0&&v<32)||v==998;};
        if(!kind(from)||!kind(to)||slot<0||slot>=30||target<0||target>=(to==998?6:30))return false;
        result.boxes.source_box=from;result.boxes.source_slot=slot;result.boxes.target_box=to;result.boxes.target_slot=target;return true;
    };
    if(inspect_boxes){
        bool pane_ok=box_object(box_object_),drag_ok=drag_object(box_drag_);
        if(!pane_ok)box_object_=0;if(!drag_ok)box_drag_=0;
        if(!pane_ok||!drag_ok){
        const auto system=Pointer(host,host.main_base+0x610A000),heaps=system?Pointer(host,system+0x5F8):0;
        const auto ui_heap=heaps?Pointer(host,heaps+0x6A8):0;
        const auto begin=Pointer(host,ui_heap)==host.main_base+0x3BCE178?Pointer(host,ui_heap+0x38):0;
        if(begin>=0x10000&&begin<(1ull<<39)){
            if(begin!=box_scan_begin_){box_scan_begin_=begin;box_scan_cursor_=0;box_scan_retry_=0;}
            constexpr std::uint64_t Window=4ull<<30,Chunk=1ull<<20;
            if(box_scan_cursor_>=Window&&tick>=box_scan_retry_)box_scan_cursor_=0;
            std::vector<std::uint8_t> data(Chunk);bool scanned=false;
            for(int budget=0;budget<4&&box_scan_cursor_<Window&&(!pane_ok||!drag_ok);++budget){
                scanned=true;const auto address=begin+box_scan_cursor_;box_scan_cursor_+=Chunk;
                if(!ScanRead(host,address,data))continue;
                for(std::size_t at=0;at+8<=data.size();at+=8){
                    const auto type=At<std::uint64_t>(data.data()+at);
                    if(!pane_ok&&type==host.main_base+0x3DB4880&&box_object(address+at)){box_object_=address+at;pane_ok=true;}
                    if(!drag_ok&&type==host.main_base+0x3DB4D18&&drag_object(address+at)){box_drag_=address+at;drag_ok=true;}
                    if(pane_ok&&drag_ok)break;
                }
            }
            if(scanned&&box_scan_cursor_>=Window)box_scan_retry_=tick+300;
        }
        }
        result.boxes.valid=pane_ok&&drag_ok;
    }
    const auto root=Pointer(host,host.main_base+0x610A670);
    const auto manager=root?Pointer(host,root+0x30):0;
    const auto begin=manager?Pointer(host,manager+8):0;
    const auto end=manager?Pointer(host,manager+16):0;
    if(!begin || end<begin || (end-begin)%32 || end-begin>2048*32)return result;
    result.diagnostic="Memory: data table still loading";result.problem=true;
    if(begin!=table_ || tick>=next_index_ || !party_entry_){
        table_=begin;next_index_=tick+60;party_entry_=coordinates_entry_=quest_entry_=mable_entry_=box_entry_=0;progress_entries_.clear();
        std::vector<std::uint8_t> entries(end-begin);
        if(!Read(host,begin,entries.data(),entries.size()))return result;
        std::uint64_t previous=0;
        for(std::size_t i=0;i<entries.size();i+=32){
            const auto key=At<std::uint32_t>(entries.data()+i);
            if((i&&key<=previous)){table_=party_entry_=coordinates_entry_=quest_entry_=mable_entry_=box_entry_=0;return result;}
            previous=key;
            if(key==KParty)party_entry_=begin+i;
            if(key==KBox)box_entry_=begin+i;
            if(key==KCoordinates)coordinates_entry_=begin+i;
            if(key==0xB9B223B9)quest_entry_=begin+i;
            if(key==0x03913534)mable_entry_=begin+i;
            if(key==0x1D7EE369||key==0x4F35D0DD||key==0x0235471C)progress_entries_[key]=begin+i;
        }
    }
    std::vector<std::uint8_t> bytes;
    for(const auto& [key,entry]:progress_entries_){
        std::array<u64,4> descriptor{},after{};
        std::array<std::uint8_t,5> value{};
        if(!Read(host,entry,descriptor.data(),sizeof(descriptor))||std::uint32_t(descriptor[0])!=key||descriptor[2]!=5||
           !Read(host,descriptor[1],value.data(),value.size())||!Read(host,entry,after.data(),sizeof(after))||descriptor!=after)continue;
        Cipher cipher(key);for(auto& b:value)b^=cipher.Next();
        if(value[0]!=10)continue; // SCType.UInt32; primitive values stay in their active SCBlock.
        const auto v=At<std::uint32_t>(value.data()+1);if(v>2147483647)continue;
        if(key==0x1D7EE369)result.infinite_points=int(v);
        if(key==0x4F35D0DD)result.money=int(v);
        if(key==0x0235471C)result.hyperspace_points=int(v);
    }
    if(party_entry_ && Block(host,party_entry_,KParty,0xB40,bytes)){
        bool consistent=true;
        for(std::size_t i=0;i<6;++i){
            const auto* record=bytes.data()+i*kPartySlotSize;
            if(!At<std::uint32_t>(record))continue;
            Entity entity;
            if(!DecryptEntity(record,kPartySize,entity) || !entity.checksum_ok || entity.species>1025 ||
                entity.level<1 || entity.level>100 || entity.hp_max==0 || entity.hp>entity.hp_max){consistent=false;break;}
            result.party[i]=std::move(entity);++result.party_count;
        }
        result.party_valid=consistent&&result.party_count>0;
    }
    std::array<Entity,6> active_party{};int active_count=0;
    decltype(result.recharge) recharge{};
    std::array<u64,6> native_party_nodes{};
    const auto read_party=[&](u64 address,u64 actors=0,int slots=6){
        active_party={};active_count=0;recharge={};int deployed=-2;
        if(!(actors?WorkingPartyArrays(host,address,actors,slots,active_party,active_count,recharge,deployed,true,&native_party_nodes):
                    WorkingPartyAt(host,address,active_party,active_count,recharge,deployed)))return false;
        if(!actors)working_root_=address;
        result.deployed_index=deployed;
        result.recharge=recharge;
        working_party_=active_party;working_count_=active_count;working_tick_=tick;
        return true;
    };
    const auto native_storage=NativePartyStorage(host);
    const auto native_nodes=native_storage?Pointer(host,native_storage+0x98):0;
    const auto native_actors=native_storage?Pointer(host,native_storage+0x70):0;
    const auto native_count=native_storage?Pointer(host,native_storage+0x78):0;
    const bool native_party=native_storage&&read_party(native_nodes,native_actors,int(native_count))&&
        Pointer(host,native_storage+0x98)==native_nodes&&Pointer(host,native_storage+0x70)==native_actors&&
        Pointer(host,native_storage+0x78)==native_count&&Pointer(host,native_storage+0xA0)==native_count&&
        NativePartyStorage(host)==native_storage;
    bool party_found=native_party||(working_root_&&read_party(working_root_));
    if(!party_found){
        working_root_=0;
        // Some old hosts exposed a conveniently adjacent allocation through this path.
        // Validate it, but never depend on that allocation order across restarts.
        auto candidate=Pointer(host,host.main_base+0x610A710);
        for(const auto off:{0x18,0x1B0})candidate=candidate?Pointer(host,candidate+off):0;
        party_found=candidate&&read_party(candidate);
    }
    if(!party_found){
        const auto context=Pointer(host,host.main_base+0x610A710);
        const u64 anchor=context>=0x100000?(context&~0xfffffull):0;
        if(anchor!=working_scan_anchor_){
            working_scan_anchor_=anchor;working_scan_cursor_=0;working_scan_retry_=0;
        }
        constexpr u64 Window=512ull<<20,Chunk=1ull<<20;
        if(anchor&&(working_scan_cursor_<Window||tick>=working_scan_retry_)){
            if(working_scan_cursor_>=Window)working_scan_cursor_=0;
            const u64 index=working_scan_cursor_/Chunk,distance=((index+1)/2)*Chunk;
            const u64 address=(index&1)?(distance<anchor?anchor-distance:0):anchor+distance;
            working_scan_cursor_+=Chunk;
            std::vector<std::uint8_t> bytes(Chunk+0x120);
            if(address&&ScanRead(host,address,bytes)){
                for(std::size_t at=0;at<Chunk;at+=8){
                    // Legacy allocation signature; this fallback never authorizes an exchange.
                    if(At<u64>(bytes.data()+at)!=host.main_base+0x3eb9a30)continue;
                    if(read_party(address+at)){party_found=true;break;}
                }
            }
            if(working_scan_cursor_>=Window)working_scan_retry_=tick+300;
        }
    }
    if(working_count_>0&&tick>=working_tick_&&tick-working_tick_<=120){
        result.party=working_party_;result.party_count=working_count_;result.party_valid=result.party_live=true;
        for(int i=0;i<6;++i)result.hp_valid[i]=result.party[i].present;
    }
    result.party_current=native_party;
    if(native_party)result.party_nodes=native_party_nodes;
    if(want_boxes&&!result.box_live&&box_entry_){
        const int wanted=std::clamp(requested_box>=0?requested_box:(host.get_i64?int(host.get_i64(host.userdata,"za.box.view",0)):0),0,31);
        if(wanted!=cached_box_index_||tick>=next_box_poll_){
            next_box_poll_=tick+15;std::vector<std::uint8_t> data;
            if(Block(host,box_entry_,KBox,kBoxCount*kBoxSlotCount*kBoxSlotSize,data)){
                std::array<Entity,30> rows{};bool valid=true;
                for(int i=0;i<30;++i){const auto* record=data.data()+(wanted*30+i)*kBoxSlotSize;
                    if(!At<std::uint32_t>(record))continue;
                    if(!DecryptEntity(record,kPartySize,rows[i])||!rows[i].checksum_ok||rows[i].species>1025){valid=false;break;}
                }
                if(valid){cached_box_=std::move(rows);cached_box_index_=wanted;cached_box_valid_=true;}
            }
        }
        if(cached_box_valid_&&cached_box_index_==wanted){result.box_valid=true;result.box_view_index=wanted;result.box_view=cached_box_;}
    }
    if(quest_entry_ && Block(host,quest_entry_,0xB9B223B9,16384,bytes)){
        for(std::size_t at=0;at<bytes.size();at+=16){auto key=At<std::uint64_t>(bytes.data()+at);if(key!=14695981039346656837ull)result.quest_work[key]=At<std::uint64_t>(bytes.data()+at+8);}
    }
    if(mable_entry_ && Block(host,mable_entry_,0x03913534,16384,bytes)){
        for(std::size_t at=0;at<bytes.size();at+=16){auto key=At<std::uint64_t>(bytes.data()+at);if(key!=14695981039346656837ull)result.quest_work[key]=At<std::uint64_t>(bytes.data()+at+8);}
    }
    // The encrypted SCBlocks are save snapshots. Combat uses separate 0x148-byte PA9
    // records in the script-object allocator. Match EC + PID + species + checksum, never
    // a naked HP value. Search a bounded allocator window once, then read six cached records.
    if(result.party_valid&&!result.party_live){
        const auto access=Pointer(host,host.main_base+0x610A710);
        const auto boxes=access?Pointer(host,access+0xB0):0;
        const auto allocator=boxes?Pointer(host,boxes+8):0;
        const auto pool=allocator>=0x100000?(allocator&~0xfffffull):0;
        bool changed=pool!=pool_begin_;
        for(int i=0;i<6;++i)if(hp_identity_[i]!=result.party[i].encryption_constant)changed=true;
        if(changed){pool_begin_=pool;pool_cursor_=0;hp_records_.fill(0);for(int i=0;i<6;++i)hp_identity_[i]=result.party[i].encryption_constant;}
        auto decode=[&](const std::uint8_t* data,int slot,Entity& entity){return DecryptEntity(data,kStoredSize,entity)&&entity.checksum_ok&&entity.encryption_constant==result.party[slot].encryption_constant&&entity.pid==result.party[slot].pid&&entity.species==result.party[slot].species&&entity.hp<=result.party[slot].hp_max;};
        bool missing=false;
        for(int i=0;i<6;++i)if(result.party[i].present){
            std::array<std::uint8_t,kStoredSize> record{};Entity entity;
            if(hp_records_[i]&&Read(host,hp_records_[i],record.data(),record.size())&&decode(record.data(),i,entity)){
                result.party[i].hp=entity.hp;
                result.party[i].exp=entity.exp;
                result.party[i].status_condition=entity.status_condition;
                result.hp_valid[i]=true;
            }
            else{hp_records_[i]=0;missing=true;}
        }
        constexpr std::uint64_t Window=32u<<20,Chunk=256u<<10;
        if(missing&&pool_begin_&&(pool_cursor_<Window||tick>=pool_retry_)){
            if(pool_cursor_>=Window)pool_cursor_=0;
            std::vector<std::uint8_t> chunk(Chunk+kStoredSize);
            // Search outward on both sides of the allocator page: allocation order differs
            // between NCE and Dynarmic, and the sixth PA9 can precede the anchor.
            const auto index=pool_cursor_/Chunk,distance=((index+1)/2)*Chunk;
            const auto address=(index&1)?(distance<pool_begin_?pool_begin_-distance:0):pool_begin_+distance;pool_cursor_+=Chunk;
            if(address&&ScanRead(host,address,chunk)){
                for(std::size_t at=0;at<Chunk;at+=4){auto ec=At<std::uint32_t>(chunk.data()+at);if(!ec)continue;
                    for(int i=0;i<6;++i)if(!hp_records_[i]&&ec==hp_identity_[i]){
                        Entity entity;
                        if(decode(chunk.data()+at,i,entity)){
                            hp_records_[i]=address+at;
                            result.party[i].hp=entity.hp;
                            result.party[i].exp=entity.exp;
                            result.party[i].status_condition=entity.status_condition;
                            result.hp_valid[i]=true;
                        }
                    }
                }
            }
            if(pool_cursor_>=Window)pool_retry_=tick+300;
        }
    }
    if(result.party_valid)for(int i=0;i<6;++i)if(result.party[i].present){
        auto& entity=result.party[i];
        const auto key=(std::uint64_t(entity.pid)<<32)|entity.encryption_constant;
        if(result.hp_valid[i]){confirmed_hp_[key]=entity.hp;confirmed_exp_[key]=entity.exp;}
        else {LastHP(entity,entity.hp);if(auto found=confirmed_exp_.find(key);found!=confirmed_exp_.end())entity.exp=found->second;}
    }
    if(!result.position_live && coordinates_entry_ && Block(host,coordinates_entry_,KCoordinates,0x70,bytes)){
        const auto length=std::find(bytes.begin(),bytes.begin()+0x20,0)-bytes.begin();
        result.area.assign(reinterpret_cast<const char*>(bytes.data()),length);
        result.x=At<float>(bytes.data()+0x20);result.y=At<float>(bytes.data()+0x24);
        result.z=At<float>(bytes.data()+0x28);result.rotation=At<float>(bytes.data()+0x2c);
        result.coordinates_valid=!result.area.empty()&&result.area.size()<24&&
            std::isfinite(result.x)&&std::isfinite(result.y)&&std::isfinite(result.z)&&
            std::abs(result.x)<100000&&std::abs(result.y)<100000&&std::abs(result.z)<100000;
    }
    int hp=0;for(bool valid:result.hp_valid)hp+=valid;
    result.problem=false;
    result.diagnostic="Live: HP "+std::to_string(hp)+"/"+std::to_string(result.party_count)+" · position "+(result.position_live?"OK":"not found")+" · selection "+(result.selected_index>=0?"OK":"not found");
    return result;
}
}
