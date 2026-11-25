// SPDX-FileCopyrightText: Copyright 2026 Sickops
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "za_live.h"
namespace za {
// Selection/readback only. The manifest invokes the game's own exchange once;
// this class never writes serialized Pokémon or generates controller inputs.
struct NativeBoxExchange {
    int box=-1,slot=-1,party=-1;
    Entity source{},victim{};
    bool completed=false;
    static bool Same(const Entity& a,const Entity& b){
        return a.present&&b.present&&a.checksum_ok&&b.checksum_ok&&
            a.encryption_constant==b.encryption_constant&&a.pid==b.pid&&
            a.species==b.species&&a.form==b.form;
    }
    bool Prepare(const LiveSnapshot& live,int b,int s,const Entity& displayed){
        if(b<0||b>=32||s<0||s>=30||!live.box_live||live.box_view_index!=b||
           !Same(live.box_view[s],displayed))return false;
        box=b;slot=s;party=-1;source=displayed;victim={};completed=false;return true;
    }
    bool Pick(const LiveSnapshot& live,int p){
        // Choosing a candidate is harmless and may use the last verified team.
        // The actual call still requires a fresh matching team in Ready().
        if(completed||slot<0||p<0||p>=6||!live.party_valid||!live.party[p].present)return false;
        party=p;victim=live.party[p];completed=false;return true;
    }
    bool Ready(const LiveSnapshot& live)const{
        return !completed&&box>=0&&box<32&&slot>=0&&slot<30&&party>=0&&party<6&&
            live.scene_idle&&live.position_live&&live.area=="t1"&&live.deployed_index==-1&&
            live.party_current&&live.box_live&&live.box_view_index==box&&
            Same(source,live.box_view[slot])&&Same(victim,live.party[party])&&
            !Same(source,victim);
    }
    void Observe(const LiveSnapshot& live){
        if(slot>=0&&party>=0&&live.party_current&&live.box_live&&live.box_view_index==box&&
           Same(source,live.party[party])&&Same(victim,live.box_view[slot]))completed=true;
    }
};
}
