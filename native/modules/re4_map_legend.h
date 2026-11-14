// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include "re4_reader.h"

namespace re4 {
// Native FlagChkVar numbers bits from the most significant bit of each word.
inline std::optional<bool> MapFlag(std::span<const uint32_t> words, unsigned flag) {
    if(flag/32>=words.size())return {};
    return (words[flag/32]&(0x80000000u>>(flag%32)))!=0;
}
enum class RoomTint : uint8_t { Hidden, Entered, NotEntered, Sealed, Current };
inline RoomTint NativeRoomTint(bool current, std::optional<bool> hide,
        std::optional<bool> open, std::optional<bool> sealed, std::optional<bool> passed) {
    if(!hide || *hide)return RoomTint::Hidden;
    if(current)return RoomTint::Current;
    if(!open || !passed || !sealed)return RoomTint::Hidden;
    if(!*open && !*passed)return RoomTint::Hidden;
    if(*sealed)return RoomTint::Sealed;
    return *passed?RoomTint::Entered:RoomTint::NotEntered;
}
enum class DoorTint : uint8_t { Hidden, Open, Locked, HaveKey };
inline DoorTint NativeDoorTint(unsigned flag_type, std::optional<bool> flag,
        std::optional<bool> key_owned) {
    if(flag_type==0)return DoorTint::Open;
    if(flag_type>2)return DoorTint::Hidden;
    if(!flag)return DoorTint::Hidden;
    if(*flag)return DoorTint::Open;
    if(!key_owned)return DoorTint::Hidden;
    return *key_owned?DoorTint::HaveKey:DoorTint::Locked;
}
inline std::optional<unsigned> NativeGoalPart(std::span<const uint32_t> scenario,
        std::span<const uint16_t> flags) {
    if(flags.empty())return {};
    unsigned part=0;
    for(unsigned n=1;n<flags.size();++n) {
        const auto flag=MapFlag(scenario,flags[n]);
        if(!flag)return {};
        if(*flag)part=n;
    }
    return part;
}
inline std::optional<unsigned> NativeMapArea(const Snapshot& s,std::string_view key) {
    if(!s.room || !s.map_flags)return {};
    // Native stage 4/5 selectors precede scenario progress; the remaining selector
    // chooses island, castle, village-day, or prologue, in that order.
    if((*s.room>>8)!=4 && (*s.room>>8)!=5) {
        const auto flags=std::span(*s.map_flags).first<8>();
        const bool island=*MapFlag(flags,47),castle=*MapFlag(flags,40);
        if(!island && !castle)return *MapFlag(flags,29)?1:0;
        if((island && !key.starts_with("island-")) ||
           (!island && castle && !key.starts_with("castle-")))return {};
    }
    for(const auto& room:MapStateRooms)if(room.key==key) {
        if(room.room==0x306) {
            if(!s.position || !std::isfinite((*s.position)[1]))return {};
            return (*s.position)[1]<2000?6:7;
        }
        if(room.room==0x304) {
            if(!s.map_open)return {};
            return (*s.map_open)[0]==2 && (*s.map_open)[1]!=0?7:6;
        }
        return room.area;
    }
    return {};
}
inline std::optional<bool> NativePassed(const Snapshot& s,uint16_t room) {
    for(const auto& row:MapSaveRooms)if(row.room==room) {
        if(!s.map_passed || (*s.map_passed)[row.index]>1)return {};
        return (*s.map_passed)[row.index]!=0;
    }
    return false; // native getRoomSavePtr returns null for rooms without save records
}
inline RoomTint MapRoomTint(const Snapshot& s,const MapStateRoom& room,unsigned area,
                            std::optional<int> floor={}) {
    if(!s.room || !s.map_flags)return RoomTint::Hidden;
    const std::string_view group=area<=1?"village-":area<=4?"castle-":
        area<=10?"island-":area<=17?"stage4-":"stage5-";
    if(!room.key.starts_with(group))return RoomTint::Hidden;
    const auto flags=std::span(*s.map_flags).first<8>();
    if(room.room==0x217)return *s.room==0x217?RoomTint::Current:RoomTint::Hidden;
    if((room.room==0x227 && *s.room==0x228) || (room.room==0x228 && *s.room==0x227))return RoomTint::Hidden;
    if(room.room==0x509 && *s.room==0x507) {
        if(!floor)return RoomTint::Hidden;
        if(*floor==1)return RoomTint::Current;
        const auto passed=NativePassed(s,0x509);
        return !passed?RoomTint::Hidden:*passed?RoomTint::Entered:RoomTint::NotEntered;
    }
    for(const auto& rule:MapRoomRules)if(rule.area==area && rule.room==room.room) {
        const bool alias=room.room==0x814 && *MapFlag(flags,33);
        return NativeRoomTint(room.room==*s.room || (alias && *s.room==0x214),
            MapFlag(flags,rule.hide),MapFlag(flags,rule.open),MapFlag(flags,rule.sealed),
            NativePassed(s,alias?0x214:room.room));
    }
    return RoomTint::Hidden;
}
inline DoorTint MapDoorTint(const Snapshot& s,const MapDoorDef& door,unsigned area) {
    if(door.area!=area || !s.map_flags)return DoorTint::Hidden;
    const auto words=std::span(*s.map_flags);
    const auto flag=MapFlag(door.type==2?words.subspan(8):words.first(8),door.flag);
    std::optional<bool> owned;
    if(door.item==255)owned=false;
    else if(s.owned_items) {
        owned=false;
        for(const auto& item:*s.owned_items)if(item.id==door.item && item.count){owned=true;break;}
    }
    return NativeDoorTint(door.type,flag,owned);
}
inline std::optional<unsigned> MapGoalPart(const Snapshot& s,unsigned area) {
    if(!area || !s.map_flags)return {};
    unsigned part=0;
    bool found=false;
    for(const auto& row:MapGoalFlags)if(row.area==area) {
        found=true;
        if(row.index && *MapFlag(std::span(*s.map_flags).first<8>(),row.flag))part=row.index;
    }
    return found?std::optional<unsigned>(part):std::nullopt;
}
}
