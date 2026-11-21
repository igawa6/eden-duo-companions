// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "core/mods/dsmod_module_abi.h"
#include "za_save.h"
namespace za {
struct BoxInput {
    bool valid=false;
    int cursor=0,party_cursor=0,source_box=0,source_slot=0,target_box=0,target_slot=0;
    bool EmptyDrag()const{return source_box==0&&source_slot==0&&target_box==0&&target_slot==0;}
};
struct MoveRecharge {
    float remaining=0,initial=0;
    bool valid=false;
};
struct Dimension {
    std::string gate,content;
    int kind=0,type=-1,rank=-1;
    std::array<int,3> types{{-1,-1,-1}},patterns{};
    float x=0,y=0,z=0;
};
struct QuestMapPin {
    int category = 0;
    int id = 0;
    // Native map space, centered on Illumis, with Y pointing down.
    float x = 0;
    float y = 0;
};
struct LandmarkPin {
    int row;
    float x,y;
};
struct UnlockedTravelPoint {
    unsigned kind, row;
    u64 key;
};
struct MapMarker {
    bool known=false, visible=false;
    float x=0,y=0;
};
struct LiveSnapshot {
    bool dimensions_live=false,battle_zones_live=false,rank_live=false;
    std::vector<Dimension> dimensions;
    std::vector<u64> active_battle_zones;
    int trainer_rank=-1,ticket_points=-1;
    bool research_live=false,royale_live=false;
    int research_level=-1,research_claimed=-1,research_points=-1;
    int infinite_points=-1,royale_wins=-1,royale_medals=-1;
    int money=-1,hyperspace_points=-1;
    std::array<std::array<MoveRecharge,4>,6> recharge{};
    bool party_live=false;
    bool party_current=false, scene_idle=false;
    bool party_valid=false, coordinates_valid=false, position_live=false;
    std::array<Entity,6> party{};
    std::array<u64,6> party_nodes{}; // fresh generation-checked native objects only
    int party_count=0;
    int deployed_index=-2; // -2 unknown, -1 no field partner
    int selected_index=-1,map_mode=-1,map_destination=0;
    BoxInput boxes;
    bool box_valid=false, box_live=false;
    int box_view_index=0;
    std::array<Entity,30> box_view{};
    std::array<bool,6> hp_valid{};
    std::unordered_map<u64,u64> quest_work;
    std::vector<QuestMapPin> quest_pins;
    bool quest_map_loaded=false;
    std::vector<LandmarkPin> landmark_pins;
    std::vector<UnlockedTravelPoint> unlocked_travel;
    MapMarker map_marker;
    std::string area,diagnostic;
    float x=0,y=0,z=0,rotation=0;
};
class LiveReader {
public:
    LiveSnapshot Poll(const EdenDsmodHostApi& host,bool inspect_boxes=false,int requested_box=-1,
                      u64 preferred_map=0);
    // Last checksum-verified combat HP, keyed by PID + EC, survives transient torn reads.
    bool LastHP(const Entity& entity, std::uint16_t& hp) const;
private:
    std::unordered_map<std::uint64_t,std::uint16_t> confirmed_hp_;
    std::unordered_map<std::uint64_t,std::uint32_t> confirmed_exp_;
    std::array<std::uint64_t,6> hp_records_{};
    std::array<std::uint32_t,6> hp_identity_{};
    std::uint64_t position_object_=0, selector_slot_=0, selector_object_=0, discovery_retry_=0;
    std::uint64_t map_object_=0,ui_scan_begin_=0,ui_scan_cursor_=0,ui_scan_retry_=0;
    std::uint64_t next_quest_pin_poll_=0;
    std::vector<QuestMapPin> quest_pins_;
    bool quest_map_loaded_=false;
    std::vector<LandmarkPin> landmark_pins_;
    std::vector<UnlockedTravelPoint> unlocked_travel_;
    MapMarker map_marker_;
    u64 working_root_=0,working_scan_anchor_=0,working_scan_cursor_=0,working_scan_retry_=0;
    std::array<Entity,6> working_party_{};int working_count_=0;std::uint64_t working_tick_=0;
    std::uint64_t box_object_=0,box_drag_=0,box_scan_begin_=0,box_scan_cursor_=0,box_scan_retry_=0;
    std::uint64_t box_entry_=0,next_box_poll_=0;
    u64 box_service_=0;
    int cached_box_index_=-1;
    bool cached_box_valid_=false;
    std::array<Entity,30> cached_box_{};
    std::uint64_t pool_begin_=0, pool_cursor_=0, pool_retry_=0;
    std::uint64_t party_entry_=0, coordinates_entry_=0, quest_entry_=0, mable_entry_=0, table_=0, next_index_=0;
    std::unordered_map<std::uint32_t,u64> progress_entries_;
};
}
