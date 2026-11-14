// SPDX-License-Identifier: GPL-3.0-or-later
// Direct tap-to-equip (owner decision 2026-11-01: no native case-open round trip). Guard is
// "the game's own permission to open the case right now" (GC SubScreenCall/subScrCheck,
// re-implemented here as pure reads -- NEVER calling the native opener, which has side
// effects), plus the extra preconditions main+0x4AF630's own refresh wrapper needs beyond
// that (reentrancy lock, non-null subscreen pointer) and an event/suspend/scripted-freeze/
// loading sweep. Full derivation + confidence per field:
// this build's equip-guards research notes (ANSWERS.md).
//
// RE4_EQUIP_ROUTINE_OFFSET (Leon's r_no_0/1/2 byte offset from pPL, ANSWERS.md Q1) is NOT
// located as of this file's writing -- no live bracket has found it (same gap flagged
// across 11+ prior dedicated sessions in work/equip/REPORT.md, this file's own ANSWERS.md,
// and 20261101-reload-guard/REPORT.md). FLAGS-ONLY guard (owner decision 2026-11-01, accepted
// risk): with the macro undefined (the shipped default), RE4_EQUIP_GUARD_ALL_KNOWN alone
// (every bit below EXCEPT ROUTINE_CALM) is enough to call arm()/4AF630 -- reload-guard's own
// live data shows every one of those bits staying in its "pass" state throughout a real
// reload and a real knife swing, so a tap during either window can race the native call; the
// owner accepted this rather than keep the feature permanently display-only. Defining the
// macro (build-time only, once the offset is live-confirmed) adds RE4_EQUIP_GUARD_ROUTINE_CALM
// as an *additional* required bit below, closing the gap with no other code change -- this is
// the "macro to switch it off" the owner asked to keep.
//
// Written without `goto`: this source is compiled twice over -- once as real freestanding C
// (gen_hud_helper.py's clang -c), once pulled directly into a C++ unit test
// (re4_equip_tick_test.cpp) to execute the exact same logic on-host. A `goto` jumping over an
// initialized declaration is valid C but ill-formed C++, so every early exit below is a
// `return` with its own inline ack-store instead of a shared `goto ack;` label.
#include "re4_equip_abi.h"
typedef unsigned long Addr;
#ifndef FN
#define FN(off,R,...) ((R (*)(__VA_ARGS__))(main+(off)))
#endif
static int sane(Addr p) { return p>=0x10000 && p<((Addr)1<<39) && !(p&7); }

__attribute__((section(".text.entry")))
void re4_equip_tick(Addr main, volatile Re4EquipMailbox *m) {
    // Self-advanced, exactly like re4_hud_tick.c's own heartbeat: this helper owns its own
    // mailbox region (RE4_EQUIP_MAILBOX_OFFSET inside the shared 4096-byte blob) and is called
    // from the combined trampoline (gen_hud_helper.py) right after re4_hud_tick every frame --
    // "appended to that same tick" -- but never depends on re4_hud_tick's own mailbox state.
    uint32_t now = m->heartbeat + 1;
    uint32_t seq = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
    // arm()/refresh() is a one-shot action, unlike re4_hud_tick's idempotent mask
    // reassertion: the host leaves a request's fields live in the mailbox for up to 90
    // heartbeats after a result lands (it polls on its own schedule and never clears
    // anything on read), so without this check every tick in that window would re-fire
    // arm()/refresh() on the same item. ack already equalling the current (non-torn) seq
    // means this exact request already produced a terminal result -- never redo it.
    if (!(seq & 1) && __atomic_load_n(&m->ack, __ATOMIC_ACQUIRE) == seq) {
        __atomic_store_n(&m->heartbeat, now, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&m->heartbeat, now, __ATOMIC_RELEASE);
    m->ok = 0; m->error = 0; m->guard_bits = 0;
    uint32_t epoch = m->request_epoch, deadline = __atomic_load_n(&m->deadline, __ATOMIC_ACQUIRE);
    if ((seq & 1) || __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE) != seq ||
        epoch != m->epoch || !re4_hud_lease_live(now, deadline)) {
        return; // torn/stale/expired request: do nothing, no ack (same contract as re4_hud_tick)
    }

    // main+0xB25D78/+0xB25D88 each hold the address of a SLOT that in turn holds pG/pPL
    // itself (double indirection; re4_equip_tick_test.cpp's Fixture comment has the exact
    // chain). The slot address itself is a compile-time-constant offset from `main` and can
    // never fail sane() -- validate the value READ FROM it (the real second-level pointer)
    // before dereferencing that a second time, instead of re-checking the always-sane slot.
    Addr pG_cell  = *(const Addr *)(main + 0xB25D78);
    Addr pPL_cell = *(const Addr *)(main + 0xB25D88);
    if (!sane(pG_cell) || !sane(pPL_cell)) {
        m->error = 2; __atomic_store_n(&m->ack, seq, __ATOMIC_RELEASE); return;
    }
    Addr pG  = *(const Addr *)pG_cell;
    Addr pPL = *(const Addr *)pPL_cell;
    Addr manager  = *(const Addr *)(main + 0xB25F60); // == "items" object, re4_reader.h Poll()
    Addr subscr   = *(const Addr *)(main + 0xB26C98);
    if (!sane(pG) || !sane(pPL) || !sane(manager)) {
        m->error = 2; __atomic_store_n(&m->ack, seq, __ATOMIC_RELEASE); return;
    }

    // ---- Q0: SubScreenCall's own permission predicate, pure reads only ----
    uint32_t bits = 0;
    {
        // Switch GameTask's real dispatch byte: door-demo 4 -> stage-init 1 -> room-init 2
        // -> main loop 3. Re-check here even if a host request predates the room load.
        volatile const unsigned char *game_routine = (volatile const unsigned char *)(pG + 0x28);
        if (*game_routine == 3) bits |= RE4_EQUIP_GUARD_GAME_MAINLOOP;
    }
    {
        volatile const int16_t *life = (volatile const int16_t *)(pG + 0x5028);
        if (*life > 0) bits |= RE4_EQUIP_GUARD_LEON_ALIVE;
    }
    // SubScreenCall also blocks when Ashley is carried and dead (pSUB->id==3 &&
    // ashley_life<=0); no live Ashley-HP reader exists, so this helper uses the game-over
    // state that Leon's own HP-hitting-zero already drives on the host as a conservative
    // stand-in (ANSWERS.md Q0) -- NOT a substitute for the real pSUB->id/ashley_life check,
    // which is why this bit is named NOT_GAMEOVER rather than NOT_ASHLEY_CARRIED_DEAD. The
    // host passes this in via the busy/deadline-protected mailbox fields it already owns;
    // see re4_equip.h PollEquipGuard for the matching host-side computation.
    if (m->host_flags & 2u) bits |= RE4_EQUIP_GUARD_NOT_GAMEOVER; // host_flags bit 1
    {
        volatile const uint32_t *sta0 = (volatile const uint32_t *)(pG + 0x5090);
        if (*sta0 & 0x02000000u) bits |= RE4_EQUIP_GUARD_SCREEN_ENABLED;
    }
    // RE4_EQUIP_GUARD_ROUTINE_CALM (RE4_EQUIP_ROUTINE_OFFSET) is deliberately computed later,
    // after the RE4_EQUIP_GUARD_ALL_KNOWN gate below, not here -- see that block.
    // ---- Equip-specific extras beyond Q0 (ANSWERS.md "Conclusion for the owner's heuristic") ----
    {
        // main+0x4AF630's own early-exit tests this byte against exactly 1 (reentrancy/busy
        // lock), not merely "nonzero" -- ANSWERS.md's raw-byte observation of the refresh
        // wrapper. Matching that exact native check, not a stricter "==0", is the owner's
        // 2026-11-01 guard list.
        volatile const unsigned char *busy = (volatile const unsigned char *)(pG + 0x503c);
        if (*busy != 1) bits |= RE4_EQUIP_GUARD_BUSY_CLEAR;
    }
    if (sane(subscr)) bits |= RE4_EQUIP_GUARD_SUBSCREEN_LIVE;
    // ---- Q2: event/cutscene/suspend/scripted-freeze ----
    {
        volatile const uint32_t *sta2 = (volatile const uint32_t *)(pG + 0x5098);
        if ((*sta2 & 0x00090000u) == 0) bits |= RE4_EQUIP_GUARD_NO_EVENT;
        // STA_SSCRN_REQUEST: the case/menu's own idempotency latch, set by SubScreenOpen()
        // itself while it is already mid-open -- refuse a second tap-to-equip while it is set.
        if ((*sta2 & 0x04000000u) == 0) bits |= RE4_EQUIP_GUARD_NOT_REQUESTED;
        volatile const uint32_t *sta1 = (volatile const uint32_t *)(pG + 0x5094);
        if ((*sta1 & 0x10000000u) == 0) bits |= RE4_EQUIP_GUARD_NO_SUSPEND;
        volatile const uint32_t *evctl = (volatile const uint32_t *)(pG + 0x1a8);
        if ((*evctl & 0x80000000u) == 0) bits |= RE4_EQUIP_GUARD_NO_SCRIPT_FREEZE;
        volatile const uint32_t *sta3 = (volatile const uint32_t *)(pG + 0x509c);
        if ((*sta3 & 0x00002000u) == 0) bits |= RE4_EQUIP_GUARD_NOT_LOADING_STA;
    }
    {
        volatile const uint32_t *loading_ui = (volatile const uint32_t *)(main + 0x1242E10);
        if (*loading_ui == 0) bits |= RE4_EQUIP_GUARD_NOT_LOADING_UI;
    }
    if (m->host_flags & 1u) bits |= RE4_EQUIP_GUARD_ROOM_STABLE;
    m->guard_bits = bits;

    // GAME_MAINLOOP is computed above (still reported in guard_bits for diagnostics, still
    // required on the HOST side's own copy of this same ALL_KNOWN gate -- re4_equip.h's
    // g.safe, which already reliably reads pG+0x28==3 every Sample() tick and correctly
    // dims the UI during a real loading transition, per 010099A00BC1E000_test.cpp's
    // existing routine=1/2/4 cases) but deliberately excluded from the GUEST's own re-check
    // here. Live round-trip testing after 839be36 found this guest-side read of pG+0x28,
    // taken at this hook's exact point in the native frame (main+0x1EA7C, the reticle-update
    // tail branch), reads something other than 3 on every single observed tick during solid,
    // ordinary gameplay -- confirmed 3/3 real taps in Leon's own campaign, guard_bits always
    // missing exactly this one bit, while an out-of-band host read of the identical pG+0x28
    // byte at the same moment reliably reads 3. That makes this specific re-check a hook-
    // timing mismatch, not a genuine loading signal, and as gated it silently failed every
    // tap-to-equip request (error=3) rather than ever protecting one -- a regression, not the
    // intended behaviour. See analysis/20261109-equip-regression.md.
    if ((bits & (RE4_EQUIP_GUARD_ALL_KNOWN & ~(uint32_t)RE4_EQUIP_GUARD_GAME_MAINLOOP)) !=
        (RE4_EQUIP_GUARD_ALL_KNOWN & ~(uint32_t)RE4_EQUIP_GUARD_GAME_MAINLOOP)) {
        m->error = 3; __atomic_store_n(&m->ack, seq, __ATOMIC_RELEASE); return;
    }
#ifdef RE4_EQUIP_ROUTINE_OFFSET
    // Stricter mode, active only once RE4_EQUIP_ROUTINE_OFFSET is live-confirmed and defined:
    // ALL_KNOWN alone is necessary but not sufficient (it never fires during a reload/knife
    // swing, reload-guard/REPORT.md); additionally require Leon's own routine-state read.
    {
        volatile const unsigned char *r_no = (volatile const unsigned char *)(pPL + RE4_EQUIP_ROUTINE_OFFSET);
        unsigned r0 = r_no[0], r1 = r_no[1], r2 = r_no[2];
        int calm = 1;
        if (r0 != 0) calm = 0;
        else if (r1 == 6) { if (r2 == 2 || r2 == 4 || r2 == 5) calm = 0; }
        else if (r1 == 0xF) { calm = (r2 == 2 || r2 == 0xA); }
        else if (r1 > 4 && r1 != 0x11) calm = 0;
        if (calm) bits |= RE4_EQUIP_GUARD_ROUTINE_CALM;
    }
    m->guard_bits = bits; // re-publish with ROUTINE_CALM now folded in (diagnostic)
    if (!(bits & RE4_EQUIP_GUARD_ROUTINE_CALM)) {
        m->error = 3; __atomic_store_n(&m->ack, seq, __ATOMIC_RELEASE); return;
    }
#endif
    // FLAGS-ONLY path (shipped default, RE4_EQUIP_ROUTINE_OFFSET undefined): ALL_KNOWN is the
    // whole guard. Owner-accepted risk: see this file's top comment and ANSWERS.md/
    // reload-guard/REPORT.md -- a tap mid-reload or mid-knife-swing is not detected here.
    {
        Addr array = *(const Addr *)(manager + 0x20);
        // Re-read the live item count too, not just the array base: the host-provided slot
        // (PlacedItem.slot) was captured whenever this request was created, potentially
        // several frames before this tick actually runs -- the case's own item count can
        // shrink in between (TOCTOU, same class of staleness as the array base itself).
        // 384 matches re4_reader.h's own fixed decode-buffer cap for this same array.
        int32_t count = *(const int32_t *)(manager + 0x30);
        if (!sane(array) || count <= 0 || count > 384 || m->slot >= (uint32_t)count) {
            m->error = 2; __atomic_store_n(&m->ack, seq, __ATOMIC_RELEASE); return;
        }
        Addr item = array + (Addr)m->slot * 14;
        int armed = FN(0x294600, int, Addr, Addr)(manager, item);
        if (!armed) {
            m->error = 4; __atomic_store_n(&m->ack, seq, __ATOMIC_RELEASE); return;
        }
        FN(0x4AF630, void, void)();
        m->ok = 1;
    }
    __atomic_store_n(&m->ack, seq, __ATOMIC_RELEASE);
}
