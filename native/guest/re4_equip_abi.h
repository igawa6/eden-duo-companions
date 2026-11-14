// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include "re4_hud_abi.h" // reused unmodified: heartbeat/epoch/seq/deadline contract + re4_hud_lease_live
// Reserved format-2 mailbox for tap-to-equip. Its own lease/epoch, never piggybacked on the
// HUD hide/show request (Re4HudMailbox) so the two can never stomp each other's refusal
// state (the tap-to-equip research REPORT). Guest owns heartbeat/ack/error;
// host owns the request fields (slot, deadline, room_stable).
// Fixed offset of this mailbox inside the package's shared 4096-byte guest mailbox region
// (packages/ResidentEvil4/dualscreen's load-plan.json, mailbox_size=4096), well clear of
// Re4HudMailbox (72 bytes at offset 0). The combined trampoline (tools/resident-evil-4/
// gen_hud_helper.py) calls re4_hud_tick with the mailbox base, then re4_equip_tick with
// base+RE4_EQUIP_MAILBOX_OFFSET, every frame -- "appended to that same tick" below.
#define RE4_EQUIP_MAILBOX_OFFSET 256u
typedef struct Re4EquipMailbox {
    uint32_t heartbeat, epoch;
    uint32_t seq, request_epoch, deadline;
    uint32_t slot;        // host-provided case-slot index (PlacedItem.slot), 0-based
    // Host-computed conditions the guest cannot itself observe across multiple frames or
    // without an Ashley-HP reader: bit 0 = room id unchanged for the last N host samples
    // (Q3 loading fallback, belt-and-suspenders alongside the two guest-side candidate
    // loading bits, not a replacement for them); bit 1 = host state != GameOver (stand-in
    // for SubScreenCall's unreadable "Ashley carried + dead" sub-check, see re4_equip.h).
    uint32_t host_flags;
    uint32_t ack, ok, error;
    // error: 0 none/ok, 1 reserved -- the guest never stores this itself; it is EquipLease's
    // own stall-timeout Result when no ack lands within 3x the lease window (re4_equip.h
    // Poll()). 2 bad pG/pPL/manager pointer, or the host-provided slot index outside the
    // item array's live count (TOCTOU: the array can shrink between when the host captured
    // this slot and when this tick actually runs). 3 guard failed (case would not open right
    // now -- see Re4EquipGuardBits, or a defined RE4_EQUIP_ROUTINE_OFFSET's own stricter
    // check failed), 4 arm()
    // itself refused (manager/item-record check). 5 is reserved/unused by this build's
    // FLAGS-ONLY guard (owner decision 2026-11-01): it no longer fires an unconditional
    // refusal when RE4_EQUIP_ROUTINE_OFFSET is undefined -- kept numbered for any future
    // build that wants a refusal reason distinct from 3.
    uint32_t guard_bits; // Re4EquipGuardBits snapshot at the moment of this decision (diagnostic)
} Re4EquipMailbox;
// Bit layout of Re4EquipMailbox.guard_bits, one bit per condition this helper actually
// checked this call (diagnostic only; `error` is the authoritative refusal reason).
enum {
    RE4_EQUIP_GUARD_LEON_ALIVE      = 1u << 0,
    RE4_EQUIP_GUARD_NOT_GAMEOVER    = 1u << 1, // proxy for SubScreenCall's unreadable
                                                // "Ashley carried + dead" sub-check
    RE4_EQUIP_GUARD_SCREEN_ENABLED  = 1u << 2, // STA_SSCRN_ENABLE, pG+0x5090 & 0x02000000
    RE4_EQUIP_GUARD_ROUTINE_CALM    = 1u << 3, // subScrCheck() -- ALWAYS 0 today, Q1 gap
    RE4_EQUIP_GUARD_BUSY_CLEAR      = 1u << 4, // pG+0x503c == 0
    RE4_EQUIP_GUARD_SUBSCREEN_LIVE  = 1u << 5, // *(main+0xB26C98) sane/non-null
    RE4_EQUIP_GUARD_NO_EVENT        = 1u << 6, // pG+0x5098 & 0x00090000 == 0
    RE4_EQUIP_GUARD_NO_SUSPEND      = 1u << 7, // pG+0x5094 & 0x10000000 == 0
    RE4_EQUIP_GUARD_NO_SCRIPT_FREEZE= 1u << 8, // pG+0x1a8 & 0x80000000 == 0
    RE4_EQUIP_GUARD_NOT_LOADING_UI  = 1u << 9, // *(main+0x1242E10) == 0
    RE4_EQUIP_GUARD_NOT_LOADING_STA = 1u << 10,// pG+0x509C & 0x00002000 == 0
    RE4_EQUIP_GUARD_ROOM_STABLE     = 1u << 11,// host-tracked, mailbox.host_flags bit0 != 0
    RE4_EQUIP_GUARD_NOT_REQUESTED   = 1u << 12,// pG+0x5098 & 0x04000000 (STA_SSCRN_REQUEST,
                                                // the case/menu's own "already opening" latch) clear
    RE4_EQUIP_GUARD_GAME_MAINLOOP   = 1u << 13,// pG+0x28 == 3; native GameTask, not loading
};
#define RE4_EQUIP_GUARD_ALL_KNOWN ( \
    RE4_EQUIP_GUARD_LEON_ALIVE | RE4_EQUIP_GUARD_NOT_GAMEOVER | RE4_EQUIP_GUARD_SCREEN_ENABLED | \
    RE4_EQUIP_GUARD_BUSY_CLEAR | RE4_EQUIP_GUARD_SUBSCREEN_LIVE | RE4_EQUIP_GUARD_NO_EVENT | \
    RE4_EQUIP_GUARD_NO_SUSPEND | RE4_EQUIP_GUARD_NO_SCRIPT_FREEZE | \
    RE4_EQUIP_GUARD_NOT_LOADING_STA | RE4_EQUIP_GUARD_ROOM_STABLE | RE4_EQUIP_GUARD_NOT_REQUESTED | \
    RE4_EQUIP_GUARD_GAME_MAINLOOP)
// FLAGS-ONLY guard (owner decision 2026-11-01): RE4_EQUIP_GUARD_ALL_KNOWN alone is now the
// shipped "safe" bar -- RE4_EQUIP_GUARD_ROUTINE_CALM stays deliberately excluded from it
// because Leon's r_no_0/1/2 Switch byte offset (ANSWERS.md Q1) is still not located after 11+
// dedicated sessions. This is an accepted, owner-approved risk, not an oversight:
// 20261101-reload-guard/REPORT.md's live data shows every ALL_KNOWN bit staying in its "pass"
// state throughout a real reload and a real knife swing, so this guard cannot by itself
// detect either state -- a tap during one of those two windows can race the native call.
// Defining RE4_EQUIP_ROUTINE_OFFSET once the offset is live-confirmed re-adds ROUTINE_CALM as
// an *additional* required bit in re4_equip_tick.c with no other code change -- the "macro to
// switch it off" the owner asked to keep.
//
// RE4_EQUIP_GUARD_GAME_MAINLOOP stays IN this ALL_KNOWN macro (it still gates the HOST's own
// copy of g.safe in re4_equip.h, which reliably reads pG+0x28==3 once per Sample() tick and
// correctly dims the UI during a real loading transition -- unaffected by this note). The
// GUEST's own re-check in re4_equip_tick.c, however, no longer requires this same bit: see
// that file's matching comment and analysis/20261109-equip-regression.md for why a
// hook-timing mismatch (not a real loading signal) made every tap-to-equip request fail
// there after 839be36 added it, even in ordinary gameplay.
//
// RE4_EQUIP_GUARD_NOT_LOADING_UI is ALSO deliberately excluded from ALL_KNOWN as of the
// 20261102 fix-C round (audit item 3, room/door-dim live verify): live-read this round at
// *(main+0x1242E10) both at the genuine title screen (256) and mid-gameplay, idle, in no way
// near a loading screen (65792) -- never observed 0 in any session's live data. This bit was
// therefore permanently clear in practice, making equip.safe unreachable via ALL_KNOWN
// regardless of every other condition (a real, previously-undiscovered regression, not the
// intended behaviour) -- confirms analysis/20261101-equip-guards/ANSWERS.md's own Q3 caution
// that a nonzero read here "cannot yet be interpreted with full confidence" the hard way. The
// bit is still computed and still reported in guard_bits/Re4EquipMailbox for a future session
// that finds the real write-0 site or a corroborating second signal; it just never gates
// g.safe until then. The legacy Status_flg[3] check remains conservative; GAME_MAINLOOP
// now provides the required, live-verified gate for the entire native room load.
