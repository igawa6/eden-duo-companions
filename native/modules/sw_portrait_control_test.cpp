// SPDX-License-Identifier: GPL-3.0-or-later
#include "sw_portrait_control.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

int main() {
    using namespace sw_helper;
    std::array<std::uint32_t, MailboxSize / 4> mem{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;
    const auto load = [&](std::uint32_t o, std::uint32_t* v) { *v = mem.at(o / 4); return true; };
    const auto store = [&](std::uint32_t o, std::uint32_t v) {
        mem.at(o / 4) = v; writes.emplace_back(o,v); return true;
    };
    const auto set = [&](std::uint32_t o, std::uint32_t v) { mem.at(o/4)=v; };
    sw_portrait_control::Control control(load,store);
    set(OffsetPortraitReqSeq, 40); set(OffsetPortraitSeq,40); set(OffsetPortraitCompleteSeq,40);
    set(EpochOffset, 7); set(OffsetFieldFree,1);
    assert(control.Initialize());
    assert(mem[OffsetPortraitCancelSeq/4] == 40); // don't revive an old session
    control.Tick(true,true,0,0);
    assert(control.sequence() == 0); // need observed driver progress
    set(OffsetPortraitBeat,1); control.Tick(true,true,0,20);
    assert(control.sequence() == 41);
    assert(writes[writes.size()-2].first == OffsetPortraitReqEpoch);
    assert(writes.back() == std::make_pair(OffsetPortraitReqSeq,41u));
    set(OffsetPortraitSeq,41); control.Tick(true,true,0,30);
    assert(control.sequence() == 41); // seen seq while guest is still constructing
    set(OffsetPortraitSeq,41); set(OffsetPortraitPhase,1);
    control.Tick(true,true,40,40);
    assert(control.busy() && mem[OffsetPortraitCapturedSeq/4] == 0);
    set(OffsetPortraitPhase,2); control.Tick(true,false,41,60);
    assert(mem[OffsetPortraitCapturedSeq/4] == 41); // exact copied sequence only
    control.Tick(false,false,41,80);
    assert(mem[OffsetPortraitCancelSeq/4] == 41); // hide still drives cancellation
    set(OffsetPortraitPhase,0); set(OffsetPortraitCompleteSeq,41); control.Tick(false,false,41,100);
    assert(!control.busy());
    set(OffsetPortraitBeat,2); control.Tick(true,true,0,4000);
    assert(control.sequence() == 42);
    // An epoch change cancels instead of dropping ownership and racing cleanup.
    set(OffsetPortraitSeq,42); set(OffsetPortraitPhase,3); set(EpochOffset,8);
    control.Tick(true,true,0,4020);
    assert(control.sequence() == 42 && mem[OffsetPortraitCancelSeq/4] == 42);
    set(OffsetPortraitPhase,4); control.Tick(true,true,0,50000);
    assert(control.sequence() == 42 && control.busy()); // never free retained native state
    set(OffsetPortraitPhase,0); set(OffsetPortraitCompleteSeq,42); control.Tick(true,true,0,50020);
    set(OffsetPortraitBeat,3); control.Tick(true,true,0,54000);
    assert(control.sequence() == 43);
    set(OffsetPortraitSeq,43); set(OffsetPortraitPhase,1);
    control.Tick(true,true,0,85000);
    assert(mem[OffsetPortraitCancelSeq/4] == 43); // wall-time bound requests cleanup
    // A new controller seeds from both sides and withdraws the unacknowledged request.
    set(OffsetPortraitReqSeq,99); set(OffsetPortraitSeq,43);
    sw_portrait_control::Control reloaded(load,store);
    assert(reloaded.Initialize());
    assert(reloaded.busy()); // orphan exclusion applies even before its phase1 publication
    assert(mem[OffsetPortraitCancelSeq/4] == 99);
    reloaded.Tick(true,true,0,90000);
    set(OffsetPortraitBeat,4); reloaded.Tick(true,true,0,90020);
    assert(reloaded.sequence() == 0); // orphan must settle first
    set(OffsetPortraitSeq,99); set(OffsetPortraitPhase,0); set(OffsetPortraitCompleteSeq,99);
    set(OffsetPortraitBeat,5); reloaded.Tick(true,true,0,90040);
    assert(reloaded.sequence() == 100);
    set(OffsetPortraitSeq,100); set(OffsetPortraitPhase,1);
    reloaded.Shutdown();
    assert(mem[OffsetPortraitCancelSeq/4] == 100); // unload while rendering
    set(OffsetPortraitPhase,2); set(OffsetPortraitCancelSeq,0);
    reloaded.Shutdown();
    assert(mem[OffsetPortraitCancelSeq/4] == 100); // unload with retained output
    std::puts("Sword portrait request ownership/cancel/epoch tests passed");
}
