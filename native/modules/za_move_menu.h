// SPDX-FileCopyrightText: Copyright 2026 Sickops
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include "za_assets.h"
#include "za_catalog.h"
#include "za_native_box.h"
namespace za {
struct MoveChoice {int id=0,level=0,item=0;};
inline std::vector<MoveChoice> MoveChoices(Assets& assets,const Entity& e,const std::vector<Save::Item>& items){
    std::vector<MoveChoice> rows;
    const auto add=[&](int id,int level,int item){
        if(id<=0||assets.MoveName(id).empty())return;
        if(std::none_of(rows.begin(),rows.end(),[&](const auto& row){return row.id==id;}))rows.push_back({id,level,item});
    };
    auto levels=assets.Learnset(e.internal_species,e.form);
    std::stable_sort(levels.begin(),levels.end(),[](const auto& a,const auto& b){return (a.level==254?1:a.level)>(b.level==254?1:b.level);});
    for(const auto& move:levels)if(move.level<=e.level||move.level==254)add(move.id,move.level==254?1:move.level,0);
    for(int id:e.moves)add(id,0,0);
    const auto machines=assets.MachineMoves(e.internal_species,e.form);
    for(const auto& item:items)if(item.count>0)for(const auto& tm:kTMs)
        if(tm.item==item.id&&std::find(machines.begin(),machines.end(),tm.move)!=machines.end())add(tm.move,0,item.id);
    if(rows.size()>192)rows.resize(192);
    return rows;
}
struct MoveMenu {
    Entity entity{};
    std::vector<MoveChoice> choices;
    int party=-1,box=-1,cell=-1,slot=0,desired=0,opened=0;
    bool from_box=false,from_bag=false;
    bool Refresh(const LiveSnapshot& live){
        if(from_box){
            if(!live.box_live||live.box_view_index!=box||cell<0||cell>=30||!NativeBoxExchange::Same(entity,live.box_view[cell]))return false;
            entity=live.box_view[cell];return true;
        }
        if(!live.party_current)return false;
        for(int i=0;i<6;++i)if(NativeBoxExchange::Same(entity,live.party[i])){party=i;entity=live.party[i];return true;}
        return false;
    }
    bool Ready(const LiveSnapshot& live)const{
        return !from_box&&!entity.egg&&opened>0&&party>=0&&party<6&&slot>=0&&slot<4&&desired>0&&
            live.party_current&&live.party_nodes[party]&&live.scene_idle&&live.position_live&&live.area=="t1"&&live.deployed_index==-1&&
            NativeBoxExchange::Same(entity,live.party[party])&&live.party[party].moves[slot]!=desired&&
            std::find(live.party[party].moves.begin(),live.party[party].moves.end(),desired)==live.party[party].moves.end()&&
            std::any_of(choices.begin(),choices.end(),[&](const auto& row){return row.id==desired;});
    }
};
}
