// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "re4_map_legend.h"
#include <string>
#include <vector>

namespace re4 {
inline std::string RoomArtKey(std::string_view key){std::string s(key);std::replace(s.begin(),s.end(),'-','/');return s;}
// Keys and asset paths have process lifetime. The rt23 ABI accepts string keys only.
struct MapRowKeys {
    std::string state,show,art;
    std::array<std::string,10> images;
};
struct MapLegendKeys {
    std::array<MapRowKeys,std::size(MapStateRooms)> rooms;
    std::array<MapRowKeys,std::size(MapDoorDefs)> doors;
    std::array<std::string,std::size(MapMarkDefs)> marks;
    size_t built=0;
    MapLegendKeys() {
        for(size_t n=0;n<rooms.size();++n) {
            auto& k=rooms[n];const auto& row=MapStateRooms[n];
            const auto prefix="re4.map.room."+std::string(row.key);
            k.state=prefix+".state";k.show=prefix+".show";k.art=prefix+".art";built+=3;
            for(int tint=1;tint<=4;++tint)
                k.images[tint-1]="module:re4/room/"+RoomArtKey(row.key)+"/"+std::to_string(tint);
            if(row.multi)for(int floor=0;floor<6;++floor)
                k.images[4+floor]="module:re4/room/"+RoomArtKey(row.key)+"/4/f"+std::to_string(floor);
        }
        for(size_t n=0;n<doors.size();++n) {
            auto& k=doors[n];const auto& row=MapDoorDefs[n];
            const auto prefix="re4.map.door."+std::to_string(row.area)+"."+std::to_string(row.index);
            k.state=prefix+".state";k.show=prefix+".show";k.art=prefix+".art";built+=3;
            for(int tint=1;tint<=3;++tint)
                k.images[tint-1]="module:re4/door/"+std::to_string(row.angle)+"/"+std::to_string(tint);
        }
        for(size_t n=0;n<marks.size();++n) {
            const auto& row=MapMarkDefs[n];
            marks[n]=std::string("re4.map.")+(row.save?"save.":"goal.")+std::to_string(row.area)+"."+std::to_string(row.index)+".show";
            ++built;
        }
    }
};
inline const MapLegendKeys& LegendKeys(){static const MapLegendKeys keys;return keys;}

class MapLegendPublisher {
    struct Inputs {
        unsigned area;
        std::optional<int> floor;
        std::optional<uint16_t> room;
        std::optional<std::array<uint32_t,13>> flags;
        std::optional<std::array<uint8_t,MapSaveCount>> passed;
        std::optional<uint8_t> mode;
        std::optional<std::array<bool,256>> owned;
        bool operator==(const Inputs&) const = default;
    };
    struct Row {const MapRowKeys* keys;unsigned tint;const char* image;};
    std::optional<Inputs> inputs;
    std::vector<Row> rows;
    std::vector<const char*> marks;
    std::vector<std::pair<std::string,const char*>> warm;
public:
    size_t rebuilds=0;
    void Publish(const EdenDsmodHostApi& h,const Snapshot& s,
                 std::optional<unsigned> area,std::optional<int> floor) {
        // The host clears all module outputs before every sample. Absence hides rows.
        h.publish_i64(h.userdata,"re4.map.native_area",area?static_cast<int>(*area):-1);
        h.publish_text(h.userdata,"re4.map.current.art","");
        h.publish_i64(h.userdata,"re4.map.warm.count",0);
        if(!area){inputs.reset();rows.clear();marks.clear();warm.clear();return;}
        Inputs next{*area,floor,s.room,s.map_flags,s.map_passed,s.map_mode,{}};
        if(s.owned_items) {
            next.owned.emplace();
            for(const auto& item:*s.owned_items)if(item.id<256 && item.count)(*next.owned)[item.id]=true;
        }
        if(!inputs || *inputs!=next) {
            ++rebuilds;inputs=next;rows.clear();marks.clear();
            const auto& keys=LegendKeys();
            const auto group=[](unsigned a){return a<=1?0:a<=4?1:a<=10?2:a<=17?3:4;};
            for(size_t n=0;n<std::size(MapStateRooms);++n) {
                const auto& room=MapStateRooms[n];
                if(group(room.area)!=group(*area))continue;
                const auto tint=MapRoomTint(s,room,*area,floor);
                if(tint==RoomTint::Hidden)continue;
                const bool multi=tint==RoomTint::Current && room.multi;
                if(multi && (!floor || *floor<0 || *floor>5))continue;
                const auto value=static_cast<unsigned>(tint);
                const auto& k=keys.rooms[n];
                rows.push_back({&k,value,k.images[multi?4+*floor:value-1].c_str()});
            }
            for(size_t n=0;n<std::size(MapDoorDefs);++n) {
                const auto& door=MapDoorDefs[n];if(door.area!=*area)continue;
                const auto value=static_cast<unsigned>(MapDoorTint(s,door,*area));
                if(value){const auto& k=keys.doors[n];rows.push_back({&k,value,k.images[value-1].c_str()});}
            }
            const auto goal=MapGoalPart(s,*area);
            for(size_t n=0;n<std::size(MapMarkDefs);++n) {
                const auto& mark=MapMarkDefs[n];if(mark.area!=*area)continue;
                if(mark.save?(s.map_mode && !(*s.map_mode&1)):(goal && *goal==mark.index))
                    marks.push_back(keys.marks[n].c_str());
            }
            // Native-visible states only, current room first. Shared door images
            // occupy one row. Build bind names only when the native inputs change.
            warm.clear();
            const auto add=[&](const Row& row) {
                if(std::none_of(warm.begin(),warm.end(),[&](const auto& v){return std::string_view(v.second)==row.image;}))
                    warm.emplace_back("re4.map.warm."+std::to_string(warm.size())+".art",row.image);
            };
            for(const auto& row:rows)if(row.tint==static_cast<unsigned>(RoomTint::Current))add(row);
            for(const auto& row:rows)add(row);
        }
        for(const auto& [key,image]:warm)h.publish_text(h.userdata,key.c_str(),image);
        h.publish_i64(h.userdata,"re4.map.warm.count",warm.size());
        for(const auto& row:rows) {
            h.publish_i64(h.userdata,row.keys->state.c_str(),row.tint);
            h.publish_i64(h.userdata,row.keys->show.c_str(),1);
            h.publish_text(h.userdata,row.keys->art.c_str(),row.image);
            if(row.tint==static_cast<unsigned>(RoomTint::Current))
                h.publish_text(h.userdata,"re4.map.current.art",row.image);
        }
        for(const auto* key:marks)h.publish_i64(h.userdata,key,1);
    }
};
}
