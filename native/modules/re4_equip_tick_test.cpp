// SPDX-License-Identifier: GPL-3.0-or-later
// Compile and execute the actual freestanding equip helper with native functions replaced by
// fixtures, mirroring re4_hud_tick_test.cpp's pattern. Included twice: once as shipped
// (FLAGS-ONLY guard, no RE4_EQUIP_ROUTINE_OFFSET -- owner decision 2026-11-01: arms+refreshes
// once every RE4_EQUIP_GUARD_ALL_KNOWN bit is set, an accepted risk since Leon's r_no_0/1/2
// Switch byte offset is still not located, ANSWERS.md Q1) and once with a placeholder offset
// defined (this test's own choice, NOT a claim about the real offset) to prove the additional,
// stricter ROUTINE_CALM gate works once that offset is ever found.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
// Included once at global scope: re4_equip_tick.c's own #include of this header (inside
// each namespace below) is then a no-op under #pragma once, so both namespaces see the
// SAME Re4EquipMailbox type and RE4_EQUIP_GUARD_* enum/macro via ordinary enclosing-scope
// lookup, rather than each silently getting its own (or, for the second one, none at all).
#include "../guest/re4_equip_abi.h"

namespace shipped {
unsigned arm_calls=0, refresh_calls=0; unsigned long last_manager=0, last_item=0; int arm_result=1;
int NativeArm(unsigned long manager,unsigned long item){++arm_calls;last_manager=manager;last_item=item;return arm_result;}
void NativeRefresh(){++refresh_calls;}
#define FN(off,R,...) reinterpret_cast<R(*)(__VA_ARGS__)>( \
    off==0x294600 ? reinterpret_cast<uintptr_t>(&NativeArm) : reinterpret_cast<uintptr_t>(&NativeRefresh))
#include "../guest/re4_equip_tick.c"
#undef FN
}

namespace routine_known {
// RE4_EQUIP_ROUTINE_OFFSET here is a test-only placeholder inside the fixture's own pPL
// region; it is NOT a claim about the real Switch offset (still unlocated, ANSWERS.md Q1).
#define RE4_EQUIP_ROUTINE_OFFSET 0x40
unsigned arm_calls=0, refresh_calls=0; unsigned long last_manager=0, last_item=0; int arm_result=1;
int NativeArm(unsigned long manager,unsigned long item){++arm_calls;last_manager=manager;last_item=item;return arm_result;}
void NativeRefresh(){++refresh_calls;}
#define FN(off,R,...) reinterpret_cast<R(*)(__VA_ARGS__)>( \
    off==0x294600 ? reinterpret_cast<uintptr_t>(&NativeArm) : reinterpret_cast<uintptr_t>(&NativeRefresh))
#include "../guest/re4_equip_tick.c"
#undef FN
#undef RE4_EQUIP_ROUTINE_OFFSET
}

namespace {
// Shared fixture layout for both namespaces above (same offsets, same struct shape).
struct Fixture {
    uint8_t* memory; size_t size; uintptr_t main_addr;
    uintptr_t pG_addr, pPL_addr, manager_addr, array_addr, subscreen_addr;
    explicit Fixture() {
        size=0x1250000; // covers main+0x1242E10 (NowLoading UI flag)
        memory=static_cast<uint8_t*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,
            MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0));
        assert(memory!=MAP_FAILED);
        main_addr=reinterpret_cast<uintptr_t>(memory);
        pG_addr=main_addr+0x100000; pPL_addr=main_addr+0x101000;
        manager_addr=main_addr+0x102000; array_addr=main_addr+0x103000;
        subscreen_addr=main_addr+0x104000;
        // Double indirection, matching the guest helper's own exact chain:
        // Addr pG = *(const Addr*)(*(const Addr*)(main+0xB25D78)); i.e. main+0xB25D78
        // holds a SLOT address, and that slot holds pG itself.
        *reinterpret_cast<uint64_t*>(memory+0xB25D78)=reinterpret_cast<uintptr_t>(memory+0x200000);
        *reinterpret_cast<uint64_t*>(memory+0x200000)=pG_addr;
        *reinterpret_cast<uint64_t*>(memory+0xB25D88)=reinterpret_cast<uintptr_t>(memory+0x200008);
        *reinterpret_cast<uint64_t*>(memory+0x200008)=pPL_addr;
        *reinterpret_cast<uint64_t*>(memory+0xB25F60)=manager_addr;
        *reinterpret_cast<uint64_t*>(memory+0xB26C98)=subscreen_addr;
        *reinterpret_cast<uint64_t*>(memory+0x102020)=array_addr; // manager+0x20 == array base
        *reinterpret_cast<int32_t*>(memory+0x102030)=32; // manager+0x30 == live item count
        // Default: every known guard condition satisfied (the "would open the case" state).
        SetGuardsSatisfied();
    }
    ~Fixture(){munmap(memory,size);}
    uint32_t& At32(uintptr_t addr){return *reinterpret_cast<uint32_t*>(memory+(addr-main_addr));}
    uint8_t& At8(uintptr_t addr){return *reinterpret_cast<uint8_t*>(memory+(addr-main_addr));}
    int16_t& Life(){return *reinterpret_cast<int16_t*>(memory+(pG_addr-main_addr)+0x5028);}
    void SetGuardsSatisfied() {
        Life()=2400;
        At8(pG_addr+0x28)=3;             // GameTask main loop, not a room load
        At32(pG_addr+0x5090)=0x02000000u; // STA_SSCRN_ENABLE set
        At8(pG_addr+0x503c)=0;            // busy lock clear
        At32(pG_addr+0x5098)=0;           // no event/suspend bits
        At32(pG_addr+0x5094)=0;
        At32(pG_addr+0x1a8)=0;            // no scripted freeze
        At32(pG_addr+0x509c)=0;           // not loading (STA)
        At32(main_addr+0x1242E10)=0;      // not loading (UI flag)
    }
};
}

int main(){
    {
        using namespace shipped;
        Fixture f;
        Re4EquipMailbox m{};
        m.epoch=m.request_epoch=1;m.seq=2;m.deadline=90;m.heartbeat=10;m.slot=3;
        m.host_flags=1|2; // room stable + not game-over
        re4_equip_tick(f.main_addr,&m);
        // FLAGS-ONLY guard: every known bit is set, the shipped build never sets
        // ROUTINE_CALM (never even computes it), but that no longer blocks arm()/refresh --
        // this is the owner's accepted-risk default, not an error.
        assert(m.ack==2 && m.error==0 && m.ok==1);
        assert((m.guard_bits & RE4_EQUIP_GUARD_ALL_KNOWN)==RE4_EQUIP_GUARD_ALL_KNOWN);
        assert(!(m.guard_bits & RE4_EQUIP_GUARD_ROUTINE_CALM));
        assert(arm_calls==1 && refresh_calls==1);
        assert(last_manager==f.manager_addr && last_item==f.array_addr+3u*14u);

        // GAME_MAINLOOP (pG+0x28!=3) is still computed and still published in guard_bits for
        // any non-3 routine value, but the guest no longer requires it to arm/refresh (see
        // this file's own comment at the gate below and analysis/20261109-equip-regression.md
        // -- a live round trip found this guest-side read unreliable at the hook's exact call
        // site, unlike the host's own copy of the same check, which stays required there).
        for(uint8_t routine : {0,1,2,4,5,6,255}) {
            Fixture g; g.At8(g.pG_addr+0x28)=routine;
            Re4EquipMailbox mm{};mm.epoch=mm.request_epoch=1;mm.seq=2;mm.deadline=90;mm.heartbeat=10;
            mm.host_flags=1|2;
            const auto arms_before=arm_calls, refreshes_before=refresh_calls;
            re4_equip_tick(g.main_addr,&mm);
            assert(mm.ack==2 && mm.error==0 && mm.ok==1);
            assert(!(mm.guard_bits & RE4_EQUIP_GUARD_GAME_MAINLOOP));
            assert(arm_calls==arms_before+1 && refresh_calls==refreshes_before+1);
        }

        // Torn/stale/expired request: ignored entirely, no ack.
        Re4EquipMailbox stale=m; stale.ack=0; stale.seq=3; // odd seq: torn write in progress
        re4_equip_tick(f.main_addr,&stale); assert(stale.ack==0);
        Re4EquipMailbox expired{}; expired.epoch=expired.request_epoch=1;expired.seq=2;
        expired.deadline=5; expired.heartbeat=200; // deadline far in the past
        re4_equip_tick(f.main_addr,&expired); assert(expired.ack==0);

        // Each known guard bit, individually cleared, blocks with error=3 and is absent
        // from guard_bits (never silently ignored).
        struct Case{void(*break_it)(Fixture&);uint32_t missing_bit;};
        Case cases[]={
            {[](Fixture&f){f.Life()=0;},RE4_EQUIP_GUARD_LEON_ALIVE},
            {[](Fixture&f){f.At32(f.pG_addr+0x5090)=0;},RE4_EQUIP_GUARD_SCREEN_ENABLED},
            {[](Fixture&f){f.At8(f.pG_addr+0x503c)=1;},RE4_EQUIP_GUARD_BUSY_CLEAR},
            {[](Fixture&f){f.At32(f.pG_addr+0x5098)=0x00080000;},RE4_EQUIP_GUARD_NO_EVENT},
            {[](Fixture&f){f.At32(f.pG_addr+0x5098)=0x04000000;},RE4_EQUIP_GUARD_NOT_REQUESTED},
            {[](Fixture&f){f.At32(f.pG_addr+0x5094)=0x10000000;},RE4_EQUIP_GUARD_NO_SUSPEND},
            {[](Fixture&f){f.At32(f.pG_addr+0x1a8)=0x80000000;},RE4_EQUIP_GUARD_NO_SCRIPT_FREEZE},
            {[](Fixture&f){f.At32(f.pG_addr+0x509c)=0x2000;},RE4_EQUIP_GUARD_NOT_LOADING_STA},
        };
        for(auto& c:cases) {
            Fixture g; c.break_it(g);
            Re4EquipMailbox mm{};mm.epoch=mm.request_epoch=1;mm.seq=2;mm.deadline=90;mm.heartbeat=10;
            mm.host_flags=1|2;
            re4_equip_tick(g.main_addr,&mm);
            assert(mm.error==3);
            assert(!(mm.guard_bits & c.missing_bit));
        }
        // RE4_EQUIP_GUARD_NOT_LOADING_UI (main+0x1242E10) is deliberately NOT in the cases
        // list above and must NOT gate arm()/refresh (20261102 fix-C: live-read nonzero both
        // at the title screen and in ordinary mid-gameplay in every sample taken this round,
        // never 0 -- see re4_equip_abi.h's comment). Still computed/reported in guard_bits
        // when it happens to pass, just never required.
        {
            Fixture g; g.At32(g.main_addr+0x1242E10)=65792; // live-observed non-loading value
            Re4EquipMailbox mm{};mm.epoch=mm.request_epoch=1;mm.seq=2;mm.deadline=90;mm.heartbeat=10;
            mm.host_flags=1|2;
            re4_equip_tick(g.main_addr,&mm);
            assert(mm.error==0 && mm.ok==1);
            assert(!(mm.guard_bits & RE4_EQUIP_GUARD_NOT_LOADING_UI));
            assert((mm.guard_bits & RE4_EQUIP_GUARD_ALL_KNOWN)==RE4_EQUIP_GUARD_ALL_KNOWN);
        }
        // host_flags bits (room-stable, not-game-over) individually withheld.
        for(uint32_t flags : {0u,1u,2u}) {
            Fixture g;
            Re4EquipMailbox mm{};mm.epoch=mm.request_epoch=1;mm.seq=2;mm.deadline=90;mm.heartbeat=10;
            mm.host_flags=flags;
            re4_equip_tick(g.main_addr,&mm);
            assert(mm.error==3);
        }
        // Bad pG/pPL/manager pointer -> error=2, never dereferenced further.
        Fixture bad; *reinterpret_cast<uint64_t*>(bad.memory+0x200000)=1; // pG resolves to 1: fails sane()
        Re4EquipMailbox mb{};mb.epoch=mb.request_epoch=1;mb.seq=2;mb.deadline=90;mb.heartbeat=10;
        re4_equip_tick(bad.main_addr,&mb); assert(mb.error==2);

        // The FIRST-level cell (main+0xB25D78/+0xB25D88 itself, read before any check
        // existed) holding garbage must also fail closed with error=2 rather than being
        // dereferenced a second time unchecked -- this used to crash the process outright
        // (SIGSEGV on the unchecked second dereference) instead of returning a clean refusal.
        Fixture garbage_cell; *reinterpret_cast<uint64_t*>(garbage_cell.memory+0xB25D78)=1;
        Re4EquipMailbox mgc{};mgc.epoch=mgc.request_epoch=1;mgc.seq=2;mgc.deadline=90;mgc.heartbeat=10;
        re4_equip_tick(garbage_cell.main_addr,&mgc); assert(mgc.error==2 && mgc.ok==0);
        Fixture garbage_cell2; *reinterpret_cast<uint64_t*>(garbage_cell2.memory+0xB25D88)=1;
        Re4EquipMailbox mgc2{};mgc2.epoch=mgc2.request_epoch=1;mgc2.seq=2;mgc2.deadline=90;mgc2.heartbeat=10;
        re4_equip_tick(garbage_cell2.main_addr,&mgc2); assert(mgc2.error==2 && mgc2.ok==0);

        // The slot (PlacedItem.slot) is host-provided and can be stale by the time this tick
        // actually runs: the manager's own live item count (manager+0x30) can have shrunk
        // since the host captured it. Must refuse with error=2 and never reach arm(),
        // exactly like a bad pointer -- not merely dim the UI and hope.
        Fixture shrunk; shrunk.At32(shrunk.manager_addr+0x30)=2; // count==2, slot 5 is now OOB
        Re4EquipMailbox ms{};ms.epoch=ms.request_epoch=1;ms.seq=2;ms.deadline=90;ms.heartbeat=10;ms.slot=5;
        ms.host_flags=1|2;
        const auto arm_before_oob=arm_calls, refresh_before_oob=refresh_calls;
        re4_equip_tick(shrunk.main_addr,&ms);
        assert(ms.error==2 && ms.ok==0 && arm_calls==arm_before_oob && refresh_calls==refresh_before_oob);
        // slot exactly equal to count is also out of bounds (0-based, [0,count)).
        Fixture at_bound; at_bound.At32(at_bound.manager_addr+0x30)=4;
        Re4EquipMailbox mab{};mab.epoch=mab.request_epoch=1;mab.seq=2;mab.deadline=90;mab.heartbeat=10;mab.slot=4;
        mab.host_flags=1|2;
        re4_equip_tick(at_bound.main_addr,&mab); assert(mab.error==2 && mab.ok==0);

        // DOUBLE-APPLY: arm()/refresh() is a one-shot action, unlike re4_hud_tick's
        // idempotent mask. A second tick against the SAME already-acked (non-torn) request
        // -- exactly what happens every frame for up to 90 heartbeats while the host has not
        // yet sent a new request -- must NOT re-fire arm()/refresh() a second time.
        Fixture once;
        Re4EquipMailbox mo{};mo.epoch=mo.request_epoch=1;mo.seq=2;mo.deadline=90;mo.heartbeat=10;mo.slot=1;
        mo.host_flags=1|2;
        const auto arm_before_once=arm_calls;
        re4_equip_tick(once.main_addr,&mo);
        assert(mo.error==0 && mo.ok==1 && arm_calls==arm_before_once+1);
        const auto arm_after_first=arm_calls, refresh_after_first=refresh_calls;
        const auto ok_after_first=mo.ok, ack_after_first=mo.ack;
        re4_equip_tick(once.main_addr,&mo); // same mailbox, untouched: heartbeat just advances
        assert(arm_calls==arm_after_first && refresh_calls==refresh_after_first);
        assert(mo.ok==ok_after_first && mo.ack==ack_after_first);

        // FLAGS-ONLY path still reaches arm(): its own refusal (e.g. manager+0x1b==1
        // internally) must propagate as error=4, and refresh must never run.
        Fixture refuse; arm_result=0;
        unsigned refresh_before=refresh_calls;
        Re4EquipMailbox mr{};mr.epoch=mr.request_epoch=1;mr.seq=2;mr.deadline=90;mr.heartbeat=10;mr.slot=1;
        mr.host_flags=1|2;
        re4_equip_tick(refuse.main_addr,&mr);
        assert(mr.error==4 && mr.ok==0 && refresh_calls==refresh_before);
        arm_result=1;
    }
    std::cout<<"shipped (flags-only) equip helper: all-known arms+refreshes without routine-calm, each guard bit individually gates, bad pointer and arm() refusal both rejected\n";

    {
        using namespace routine_known;
        Fixture f;
        Re4EquipMailbox m{};m.epoch=m.request_epoch=1;m.seq=2;m.deadline=90;m.heartbeat=10;m.slot=7;
        m.host_flags=1|2;
        // RE4_EQUIP_ROUTINE_OFFSET==0x40 here: r_no_0/1/2 at pPL+0x40 must read "calm"
        // (0,0,0) for this placeholder test build.
        *reinterpret_cast<uint8_t*>(f.memory+(f.pPL_addr-f.main_addr)+0x40)=0;
        *reinterpret_cast<uint8_t*>(f.memory+(f.pPL_addr-f.main_addr)+0x41)=0;
        *reinterpret_cast<uint8_t*>(f.memory+(f.pPL_addr-f.main_addr)+0x42)=0;
        re4_equip_tick(f.main_addr,&m);
        assert(m.error==0 && m.ok==1 && arm_calls==1 && refresh_calls==1);
        assert(last_manager==f.manager_addr && last_item==f.array_addr+7u*14u);
        assert(m.guard_bits & RE4_EQUIP_GUARD_ROUTINE_CALM);

        // Aiming (r_no_1==6, r_no_2==4): subScrCheck would block -> must refuse, never call.
        Fixture g;
        *reinterpret_cast<uint8_t*>(g.memory+(g.pPL_addr-g.main_addr)+0x40)=0;
        *reinterpret_cast<uint8_t*>(g.memory+(g.pPL_addr-g.main_addr)+0x41)=6;
        *reinterpret_cast<uint8_t*>(g.memory+(g.pPL_addr-g.main_addr)+0x42)=4;
        Re4EquipMailbox mg{};mg.epoch=mg.request_epoch=1;mg.seq=2;mg.deadline=90;mg.heartbeat=10;
        mg.host_flags=1|2;
        re4_equip_tick(g.main_addr,&mg);
        assert(mg.error==3 && mg.ok==0 && !(mg.guard_bits & RE4_EQUIP_GUARD_ROUTINE_CALM));

        // arm() itself refuses (e.g. manager+0x1b==1 internally) -> error=4, refresh never runs.
        Fixture h; arm_result=0;
        *reinterpret_cast<uint8_t*>(h.memory+(h.pPL_addr-h.main_addr)+0x40)=0;
        *reinterpret_cast<uint8_t*>(h.memory+(h.pPL_addr-h.main_addr)+0x41)=0;
        *reinterpret_cast<uint8_t*>(h.memory+(h.pPL_addr-h.main_addr)+0x42)=0;
        unsigned refresh_before=refresh_calls;
        Re4EquipMailbox mh{};mh.epoch=mh.request_epoch=1;mh.seq=2;mh.deadline=90;mh.heartbeat=10;
        mh.host_flags=1|2;
        re4_equip_tick(h.main_addr,&mh);
        assert(mh.error==4 && mh.ok==0 && refresh_calls==refresh_before);
        arm_result=1;
    }
    std::cout<<"routine-offset-defined build: calm state arms+refreshes, aiming state refuses, arm() refusal propagates\n";
}
#else
int main(){std::cout<<"equip guest helper fixture requires Linux x86_64 MAP_32BIT\n";}
#endif
