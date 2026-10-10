// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <optional>
#include <vector>
#include "dq3_map.h"

namespace dq3 {

// A drag is a native Line-Up swap. Only normal controller input performs the change.
struct PartyDragOrder {
    int source{}, target{}, stage{}; // open, select source, select target, close, finished
    std::vector<u64> before, after; // stable native field-data identities, in party order

    static std::optional<PartyDragOrder> Start(int source, int target, std::vector<u64> fields) {
        if (fields.size()<2 || fields.size()>4 || source<0 || target<0 ||
            source>=static_cast<int>(fields.size()) || target>=static_cast<int>(fields.size()) || source==target)
            return std::nullopt;
        for (std::size_t k=0;k<fields.size();++k)
            if (!fields[k] || std::find(fields.begin(),fields.begin()+k,fields[k])!=fields.begin()+k)
                return std::nullopt;
        PartyDragOrder out{source,target,0,fields,fields};
        std::swap(out.after[source],out.after[target]);
        return out;
    }
    int Goal() const { return stage==0 ? -2 : stage==1 ? source : stage==2 ? target : -3; }
    bool MatchesRoster(const std::vector<u64>& fields) const { return fields==(stage<3 ? before : after); }
    bool FinishStage(const FieldMenuState& menu, const std::vector<u64>& fields) {
        using M=FieldMenuMode;
        if (stage<0 || stage>3) return false;
        if (stage==3) {
            if (menu.mode!=M::Field || fields!=after) return false;
        } else {
            if (menu.mode!=M::LineUp || menu.count!=static_cast<int>(before.size())) return false;
            if (stage==0 && (menu.selected!=-1 || menu.cursor!=0 || fields!=before)) return false;
            if (stage==1 && (menu.selected!=source || menu.cursor!=source || fields!=before)) return false;
            if (stage==2 && (menu.selected!=-1 || menu.cursor!=target || fields!=after)) return false;
        }
        ++stage;
        return true;
    }
};

struct PartyOrderStep { int button; FieldMenuState expected; bool terminal{}; };
// Buttons: X, Right, Down, A, Left, B. Reject a menu outside the pinned native route.
inline std::optional<PartyOrderStep> PlanPartyOrder(const FieldMenuState& menu,int goal,int count) {
    using M=FieldMenuMode;
    if (count<2 || count>4) return std::nullopt;
    if (menu.mode==M::LineUp && (menu.count!=count || menu.cursor<0 || menu.cursor>=count ||
        menu.selected < -1 || menu.selected>=count)) return std::nullopt;
    FieldMenuState expected=menu;
    if (goal==-2) {
        if (menu.mode==M::Field) return PartyOrderStep{0,{M::Top,0}};
        if (menu.mode==M::Top) {
            if (menu.cursor==0) return PartyOrderStep{1,{M::Top,3}};
            if (menu.cursor==3 || menu.cursor==4) return PartyOrderStep{2,{M::Top,menu.cursor+1}};
            if (menu.cursor==5) return PartyOrderStep{3,{M::Misc,0}};
        }
        if (menu.mode==M::Misc && menu.cursor>=0 && menu.cursor<=3 && menu.command==menu.cursor+1) {
            if (menu.cursor<3) return PartyOrderStep{2,{M::Misc,menu.cursor+1}};
            return PartyOrderStep{3,{M::LineUp,0},true};
        }
    } else if (goal==-3) {
        if (menu.mode==M::LineUp) {
            if (menu.selected>=0) {expected.selected=-1;return PartyOrderStep{5,expected};}
            return PartyOrderStep{5,{M::Misc,3}};
        }
        if (menu.mode==M::Misc && menu.cursor==3 && menu.command==4) return PartyOrderStep{5,{M::Top,5}};
        if (menu.mode==M::Top && menu.cursor==5) return PartyOrderStep{5,{M::Field},true};
    } else if (goal>=0 && goal<count && menu.mode==M::LineUp) {
        if (menu.cursor<goal) {++expected.cursor;return PartyOrderStep{1,expected};}
        if (menu.cursor>goal) {--expected.cursor;return PartyOrderStep{4,expected};}
        expected.selected=menu.selected<0 ? goal : -1;
        return PartyOrderStep{3,expected,true};
    }
    return std::nullopt;
}
} // namespace dq3
