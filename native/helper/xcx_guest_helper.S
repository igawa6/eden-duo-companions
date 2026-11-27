// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Xenoblade Chronicles X: Definitive Edition 1.0.2 guest helper -- format-2 load-plan code
// (Board / Exit Skell as direct calls to the game's own request functions).
// Title 0100453019AA8000, main build 2A720C7CE5C84905C1C6847211F670FBB31842B6.
//
// Hook site: main+0x409ed4, the entry of the field HUD per-frame update (its only direct caller is
// the virtual vf_40ce08 at main+0x40ce08, which passes x0 = *(main+0x1df5978), the HUD object).
// The entry instruction `stp x29, x30, [sp, #-0x20]!` (fd7bbea9) is not PC-relative and not a
// branch; the helper re-executes it first ("reproduce and resume": the frame the original
// function expects is opened exactly as the unpatched game would open it), runs, restores every
// register it touched and branches to main+0x409ed8. Only x0 is live-in at the entry; every other
// caller-saved register is dead there, and the callee-saved ones the helper uses (x19-x28) are saved
// in a disposable scratch frame below the reproduced one. The game's functions it calls follow the
// AAPCS64, so d8-d15 survive and the helper itself uses no FP/SIMD register.
//
// OP_SWITCH(k) (op 3) is the controlled-character switch: the native Active Members close path
// without the menu (R = HUD+0x34130; see R/contracts/SWITCH-PATH.md): gather the four slots'
// {kind, id}, swap entries 0 and k, stage four rows with 0x41a704 (aux values from 0x56bed0) and
// queue them with 0x41a7a0; the consumer at 0x409f98 runs in the same hook call. It needs an on-foot
// valid state, an idle request block, 0xf0388() == 0 and slot k still holding the member token the
// module sent. The 0x400-byte scratch for 0xd75c/0x56bed0 is the mailbox's 0x400..0x7ff, not the stack.
//
// What it does per hook call (all on the game thread, the one that runs the HUD update):
//   * beats: guest magic 'XCG1' and heartbeat (every call, even without a module request).
//   * only when the module magic 'XCM1' is set:
//       - a pending request (req_seq != ack_seq): resolve the controlled party record, then
//           OP_BOARD  call tryRideOn   main+0x2052fc(character actor)   on foot, actor type 2
//           OP_EXIT   call tryGetOff   main+0x20538c(Skell actor)        piloting, actor type 4
//         and answer with a result code (xcx_helper_mailbox.h) and ack_seq (stlr).
//       - every 10th call (poll): publish the state word {valid, piloting, on_foot, gate (diagnostic
//         only), can_board, can_exit, transitioning, record byte +10, slot, hud_ok}; can_board =
//         canRideOn main+0x1b9c14, can_exit = exit singleton gate + canGetOff main+0x1b9fbc.
//         The action-prompt gate (*(main+0x1feffc0)+0x158) never blocks anything.
//
// "Controlled party record": party manager main+0x1df58d0, 4 records of 0x14 from +0xc, record
// +0 u32 character handle, +4 u32 Skell handle, +9 byte "is controlled". Handles resolve through
// the game's resolver main+0x1a2dc (x0 = pointer to the u32 handle; returns the object or null) and
// are resolved fresh on every request and poll. Actor +0xb4 is the actor type (2 character, 4
// Skell). The driven actor is the one whose +0xd0 bit5 is set (live 2026-11-21): the character on
// foot, the Skell while piloting, neither during a transition (then nothing is called).
//
// No system instruction is used (no SVC/MRS/MSR/barrier); the mailbox sequence words use
// ldar/stlr. Mailbox layout: native/modules/xcx_helper_mailbox.h (single source of truth; every
// offset below is a literal immediate and must match it).

.text
.global xcx_guest_helper_entry
xcx_guest_helper_entry:
    stp  x29, x30, [sp, #-0x20]!        // reproduced entry instruction (fd7bbea9)

    sub  sp, sp, #0xa0
    stp  x0,  x1,  [sp, #0x00]
    stp  x19, x20, [sp, #0x10]
    stp  x21, x22, [sp, #0x20]
    stp  x23, x24, [sp, #0x30]
    stp  x25, x26, [sp, #0x40]
    stp  x27, x28, [sp, #0x50]
    stp  x29, x30, [sp, #0x60]

    mov  x20, x0                        // HUD object
    mov  x21, xzr
    mov  x22, xzr
    mov  x23, xzr
    mov  x24, xzr
    mov  w25, wzr
    adrp x19, xcx_mailbox_base
    add  x19, x19, :lo12:xcx_mailbox_base

    // --- per-frame beat ---
    movz w9, #0x4358                    // 'XCG1'
    movk w9, #0x3147, lsl #16
    str  w9, [x19, #0x04]
    ldr  w9, [x19, #0x08]
    add  w9, w9, #1
    str  w9, [x19, #0x08]               // heartbeat

    ldr  w9, [x19, #0x00]
    movz w10, #0x4358                   // 'XCM1'
    movk w10, #0x314D, lsl #16
    cmp  w9, w10
    b.ne finish                         // no module: touch nothing else

    // --- request? ---
    add  x9, x19, #0x20
    ldar w26, [x9]                      // req_seq (pairs with the module's release store)
    ldr  w9, [x19, #0x30]               // ack_seq (only this helper writes it)
    cmp  w26, w9
    b.eq poll_stage
    add  x9, x19, #0x2C
    ldar w9, [x9]                       // cancel_seq
    cmp  w9, w26
    b.eq res_cancelled
    ldr  w9, [x19, #0x28]               // req_epoch
    add  x10, x19, #0xFFC
    ldar w10, [x10]                     // host epoch word
    cbz  w10, res_stale
    cmp  w9, w10
    b.ne res_stale
    ldr  w27, [x19, #0x24]              // req_op
    sub  w9, w27, #1
    cmp  w9, #3
    b.hi res_badop                      // 1 board, 2 exit, 3 switch, 4 return

    bl   xcx_ctx                        // w0 = valid, w25 = flags, x21..x24 = record/actors
    tst  w25, #0x10000                  // HUD object is the game's current one
    b.eq res_notready
    tst  w25, #0x2000                   // the field must accept player input (not a menu/event/pause)
    b.eq res_notready
    mov  w28, w0                        // keep xcx_ctx's verdict across the call
    bl   xcx_field_ok                   // 0xf0b44: the game's field-action-allowed predicate (false while
    tbz  w0, #0, res_notready           // loading, live-verified); asked for every op
    mov  w0, w28
    tst  w25, #0x40                     // neither actor is the driven one: a transition
    b.ne res_transition
    cbz  w0, res_badhandle
    cmp  w27, #4
    b.eq req_return
    cmp  w27, #3
    b.eq req_switch
    cmp  w27, #1
    b.ne req_exit
    tst  w25, #0x2                      // piloting: boarding does not apply
    b.ne res_notapplicable
    mov  x0, x22                        // character actor (type 2, checked by xcx_ctx)
    bl   xcx_ride_request
    b    req_called
req_exit:
    tst  w25, #0x4                      // on foot: exiting does not apply
    b.ne res_notapplicable
    mov  x0, x23                        // Skell actor (type 4, checked by xcx_ctx)
    bl   xcx_exit_request
req_called:
    and  w1, w0, #1                     // the game's own return: 1 = request submitted
    cbz  w1, res_refused
    mov  w0, #0
    b    answer
res_refused:
    mov  w0, #7
    b    answer
// ---- OP_RETURN: the native "Return to Skell" menu entry's own action (option A). The menu handler
// (vf_837aa8 cmd 6 -> 0x41e8d4) does `strb 1, [H+0x262b8+0xed]` = byte H+0x263a5; the consumer 0x41dd80
// (called at 0x40a024 inside this same HUD update, x19 = H+0x262b8) then runs 0xf16b0() and, if true,
// 0x130864() (the warp request), and clears the byte. We store that one byte and fall through.
// Fire and forget: the game's own predicate may still refuse (it clears the byte, nothing is queued).
req_return:
    tst  w25, #0x2                      // piloting: nothing to return to
    b.ne res_notapplicable
    tst  w25, #0x80                     // record byte +10 clear: not on foot with a Skell
    b.eq res_notapplicable
    cbz  x23, res_badhandle             // the controlled record's Skell handle must resolve (type 4)
    mov  w9, #0x63a5
    movk w9, #0x2, lsl #16
    ldrb w10, [x20, x9]                 // H+0x263a5 must be 0: no other Return request pending
    cbnz w10, res_notready
    adrp x9, xcx_ctrl_object
    add  x9, x9, :lo12:xcx_ctrl_object
    ldr  x9, [x9]
    cbz  x9, res_notready
    ldr  w10, [x9, #0x10]
    tst  w10, #0x4                      // world change already in progress
    b.ne res_notready
    bl   xcx_can_return                 // the consumer's own predicate, asked first so we never store a doomed request
    tbz  w0, #0, res_cannotreturn
    mov  w9, #0x63a5
    movk w9, #0x2, lsl #16
    mov  w10, #1
    strb w10, [x20, x9]                 // the byte the menu sets
    mov  w0, #0
    mov  w1, #1
    b    answer
res_cannotreturn: mov w0, #12; b answer_zero

// ---- OP_SWITCH(k): the native Active Members close path, minus the menu. In the same hook call
// the consumer at 0x409f98 (after this code) turns the staged block into the game's own request.
// Locals: sp+0x70 u32 out-handle; sp+0x78+8*s {u32 kind, u32 id} for slots 0..3.
// x21 = request block R = HUD+0x34130, x28 = k, x27 = target token (kind<<8|id), x25 = loop index.
req_switch:
    tst  w25, #0x2                      // piloting: not in this version
    b.ne res_notapplicable
    ldr  w28, [x19, #0x5C]              // k: the slot whose member becomes controlled
    sub  w9, w28, #1
    cmp  w9, #2
    b.hi res_badop                      // 1..3 only
    ldr  w27, [x19, #0x60]              // the member token the module showed in slot k
    mov  w9, #0x4130
    movk w9, #0x3, lsl #16
    add  x21, x20, x9                   // R
    ldr  x9, [x21, #0xa8]               // the request block must be idle (SWITCH-PATH Q3)
    ldr  x10, [x21, #0x38]
    orr  x9, x9, x10
    ldr  x10, [x21, #0x70]
    orr  x9, x9, x10
    ldrh w10, [x21, #0xb0]
    orr  x9, x9, x10
    ldrh w10, [x21, #0xb2]
    orr  x9, x9, x10
    ldrb w10, [x21, #0xb6]
    orr  x9, x9, x10
    cbnz x9, res_notready
    adrp x9, xcx_ctrl_object
    add  x9, x9, :lo12:xcx_ctrl_object
    ldr  x9, [x9]
    cbz  x9, res_notready               // 0xf0388 dereferences it
    bl   xcx_cooldown                   // bool: 1 = the game does not allow a switch yet
    tbnz w0, #0, res_cooldown
    mov  w25, wzr
sw_gather:                              // current {kind, id} of the four slots
    add  x8, sp, #0x70
    mov  w0, w25
    bl   xcx_slot_handle
    mov  w11, wzr                       // kind, id default 0/0 (an empty or stale slot)
    mov  w12, wzr
    ldr  w9, [sp, #0x70]
    tst  w9, #0x8007ffff                // the game's own handle test (0x41a7d0)
    b.eq 1f
    mov  w0, w25
    bl   xcx_slot_record
    cbz  x0, 1f
    ldrb w11, [x0, #0x78]               // kind: 1 avatar, 2 character
    ldrb w12, [x0, #0x79]               // id
1:
    add  x9, sp, #0x78
    add  x9, x9, x25, lsl #3
    str  w11, [x9]
    str  w12, [x9, #4]
    add  w25, w25, #1
    cmp  w25, #4
    b.lo sw_gather
    ldr  w9, [sp, #0x78]                // slot 0 and slot k need kind 1 or 2 (the native handler
    sub  w9, w9, #1                     // refuses kind 0 and 3)
    cmp  w9, #1
    b.hi res_badhandle
    add  x9, sp, #0x78
    add  x9, x9, x28, lsl #3
    ldr  w10, [x9]
    sub  w11, w10, #1
    cmp  w11, #1
    b.hi res_badhandle
    ldr  w11, [x9, #4]
    orr  w11, w11, w10, lsl #8          // slot k's token now
    cmp  w11, w27
    b.ne res_staletarget                // the party moved since the module looked
    mov  w27, w11                       // result value: the member that becomes controlled
    ldr  x10, [x9]                      // swap entries 0 and k
    ldr  x11, [sp, #0x78]
    str  x10, [sp, #0x78]
    str  x11, [x9]
    mov  w25, wzr
sw_stage:                               // 0x89ebb4's per-row sequence, row s
    add  x0, x19, #0x400                // helper-owned 0x400-byte scratch in the mailbox
    bl   xcx_init_tmp
    add  x9, sp, #0x78
    add  x9, x9, x25, lsl #3
    ldr  w0, [x9, #4]                   // id
    add  x1, x19, #0x400
    bl   xcx_lookup
    ldrb w4, [x19, #0x7f4]              // scratch +0x3f4
    ldrh w5, [x19, #0x7f6]              // scratch +0x3f6
    tst  w0, #1
    csel w4, w4, wzr, ne
    csel w5, w5, wzr, ne
    add  x9, sp, #0x78
    add  x9, x9, x25, lsl #3
    ldr  w2, [x9]
    ldr  w3, [x9, #4]
    mov  w1, w25
    mov  x0, x21
    mov  w6, #1
    bl   xcx_stage
    add  w25, w25, #1
    cmp  w25, #4
    b.lo sw_stage
    mov  x0, x21
    mov  w1, #1
    bl   xcx_commit                     // phase 1, sub 3, busy: the consumer runs right after us
    str  w27, [x19, #0x68]              // target token, for the module's completion check
    ldr  w9, [x19, #0x6C]
    add  w9, w9, #1
    str  w9, [x19, #0x6C]
    mov  w1, w27
    mov  w0, #0
    b    answer
res_cooldown:     mov w0, #10; b answer_zero
res_staletarget:  mov w0, #11; b answer_zero
res_cancelled:    mov w0, #4;  b answer_zero
res_stale:        mov w0, #3;  b answer_zero
res_badop:        mov w0, #2;  b answer_zero
res_notready:     mov w0, #8;  b answer_zero
res_transition:   mov w0, #9;  b answer_zero
res_badhandle:    mov w0, #6;  b answer_zero
res_notapplicable: mov w0, #5; b answer_zero
answer_zero:
    mov  w1, wzr
answer:                                 // w0 = result code, w1 = result value
    str  w0, [x19, #0x34]
    str  w1, [x19, #0x38]
    ldr  w9, [x19, #0x08]
    str  w9, [x19, #0x3C]               // ack_beat
    str  x22, [x19, #0x40]              // diagnostics: character / Skell / controlled actor
    str  x23, [x19, #0x48]
    str  x24, [x19, #0x50]
    ldr  w9, [x19, #0x58]
    add  w9, w9, #1
    str  w9, [x19, #0x58]               // requests answered
    str  wzr, [x19, #0x14]              // the poll runs again on the next call
    add  x9, x19, #0x30
    stlr w26, [x9]                      // ack_seq, released after the result words
    b    finish

    // --- eligibility poll every 10th call ---
poll_stage:
    ldr  w9, [x19, #0x14]
    cbz  w9, do_poll
    sub  w9, w9, #1
    str  w9, [x19, #0x14]
    b    finish
do_poll:
    mov  w9, #9
    str  w9, [x19, #0x14]
    bl   xcx_ctx
    tbz  w25, #0, poll_store            // not the controlled character/Skell
    tbz  w25, #13, poll_store           // input not accepted: no can_* bits
    bl   xcx_field_ok
    tbz  w0, #0, poll_store             // 0xf0b44 false (loading): no can_* bits
    orr  w25, w25, #0x8000
    tbnz w25, #2, poll_foot
    tbnz w25, #1, poll_pilot
    b    poll_store
poll_foot:
    mov  x0, x22
    bl   xcx_can_ride_on                // x0 = character actor; returns bool in bit 0
    tbz  w0, #0, poll_store
    orr  w25, w25, #0x10
    b    poll_store
poll_pilot:
    adrp x9, xcx_exit_singleton
    add  x9, x9, :lo12:xcx_exit_singleton
    ldr  w9, [x9, #0x90]                // tryGetOff requires this to be 0 first
    cbnz w9, poll_store
    mov  x0, x23
    bl   xcx_can_get_off                // x0 = Skell actor; returns bool in bit 0
    tbz  w0, #0, poll_store
    orr  w25, w25, #0x20
poll_store:
    tbz  w25, #16, poll_publish         // HUD object not verified: nothing more is read
    bl   xcx_switch_probe
    bl   xcx_return_probe
poll_publish:
    str  w25, [x19, #0x0C]              // state word
    str  x22, [x19, #0x40]
    str  x23, [x19, #0x48]
    str  x24, [x19, #0x50]
    ldr  w9, [x19, #0x10]
    add  w9, w9, #1
    str  w9, [x19, #0x10]               // poll_beat

finish:
    ldp  x0,  x1,  [sp, #0x00]
    ldp  x19, x20, [sp, #0x10]
    ldp  x21, x22, [sp, #0x20]
    ldp  x23, x24, [sp, #0x30]
    ldp  x25, x26, [sp, #0x40]
    ldp  x27, x28, [sp, #0x50]
    ldp  x29, x30, [sp, #0x60]
    add  sp, sp, #0xa0
    b    xcx_resume                     // main+0x409ed8

// ---------------------------------------------------------------------------
// xcx_ctx: resolve the controlled party record and the actors. In: x19 mailbox, x20 HUD object.
// Out: w0 = 1 when the driven actor (+0xd0 bit5) is the record's character (type 2) or Skell
// (type 4); w25 = state flags: bit0 valid, bit1 piloting, bit2 on foot, bit3 prompt gate (diagnostic), bit6 transitioning, bit7 record byte +10,
// bits 8-9 party slot, bit16 HUD object verified; x21 record, x22 character, x23 Skell,
// x24 controlled actor (each 0 when unresolved). Clobbers x0-x18 (plus the callee-saved set it
// owns: x21-x25); x30 is saved here.
xcx_ctx:
    stp  x29, x30, [sp, #-0x20]!
    mov  w25, wzr
    mov  x21, xzr
    mov  x22, xzr
    mov  x23, xzr
    mov  x24, xzr
    cbz  x20, ctx_none
    adrp x9, xcx_hud_global
    add  x9, x9, :lo12:xcx_hud_global
    ldr  x9, [x9]                       // *(main+0x1df5978): the HUD object the game passes
    cmp  x9, x20
    b.ne ctx_none                       // not the HUD update this helper was written for
    orr  w25, w25, #0x10000
    // Input readiness: the byte +0xb0 of the object at main+0x1fefb70, bit2. This is 0x1e1bdc(obj, 4),
    // the first test canRideOn and the game's own board prompt (0x410144) make; set by setPausePlayer
    // style requests (events, menus). A pure byte read; null object or the bit set = not ready.
    adrp x9, xcx_input_object
    add  x9, x9, :lo12:xcx_input_object
    ldr  x9, [x9]
    mvn  w10, wzr
    cbz  x9, 6f
    ldrb w10, [x9, #0xb0]
    tst  w10, #0x4
    b.ne 6f
    orr  w25, w25, #0x2000
6:
    str  w10, [x19, #0x70]              // diagnostics: the raw mask byte (0xffffffff: no object)
    adrp x9, xcx_prompt_gate
    add  x9, x9, :lo12:xcx_prompt_gate
    ldr  x9, [x9]
    cbz  x9, 1f
    ldrb w10, [x9, #0x158]              // 0 hides the action prompts (the game re-reads it each frame)
    cbz  w10, 1f
    orr  w25, w25, #0x8
1:
    adrp x12, xcx_party_manager
    add  x12, x12, :lo12:xcx_party_manager
    add  x12, x12, #0xc                 // record 0
    mov  w10, wzr
    mov  w11, #0x14
2:
    umaddl x13, w10, w11, x12           // record i
    ldrb w9, [x13, #9]                  // is controlled
    cbnz w9, 3f
    add  w10, w10, #1
    cmp  w10, #4
    b.lo 2b
    b    ctx_none                       // no controlled record (title / loading / not initialised)
3:
    mov  x21, x13
    orr  w25, w25, w10, lsl #8          // party slot
    ldr  w9, [x21, #0]                  // character handle
    str  w9, [sp, #0x10]
    add  x0, sp, #0x10
    bl   xcx_resolve
    mov  x22, x0
    ldr  w9, [x21, #4]                  // Skell handle
    str  w9, [sp, #0x10]
    add  x0, sp, #0x10
    bl   xcx_resolve
    mov  x23, x0
    ldrb w9, [x21, #10]                 // diagnostics: record byte +10 (1 on foot, 0 mounted)
    cbz  w9, 5f
    orr  w25, w25, #0x80
5:
    cbz  x22, ctx_none                  // handles are resolved fresh on every call, never cached
    ldrb w9, [x22, #0xb4]
    cmp  w9, #2
    b.ne ctx_none                       // the record's character must be a type 2 actor
    cbz  x23, 6f
    ldrb w9, [x23, #0xb4]
    cmp  w9, #4
    b.ne ctx_none                       // a Skell handle must resolve to a type 4 actor
6:
    // The driven-actor mark: actor +0xd0 bit5 is set on exactly one actor of the controlled slot
    // (live 2026-11-21): the character on foot, the Skell while piloting, neither in transition.
    ldrb w9, [x22, #0xd0]
    tbz  w9, #5, 8f
    cbz  x23, 7f
    ldrb w9, [x23, #0xd0]
    tbnz w9, #5, ctx_none               // both marked: contradictory, refuse
7:
    mov  x24, x22
    orr  w25, w25, #0x1                 // valid
    orr  w25, w25, #0x4                 // on foot
    b    ctx_valid
8:
    cbz  x23, 9f
    ldrb w9, [x23, #0xd0]
    tbz  w9, #5, 9f
    mov  x24, x23
    orr  w25, w25, #0x1                 // valid
    orr  w25, w25, #0x2                 // piloting
    b    ctx_valid
9:
    orr  w25, w25, #0x40                // transitioning: neither actor carries the mark
    b    ctx_none
ctx_valid:
    mov  w0, #1
    ldp  x29, x30, [sp], #0x20
    ret
ctx_none:
    mov  w0, wzr
    ldp  x29, x30, [sp], #0x20
    ret

// ---------------------------------------------------------------------------
// xcx_switch_probe: the switch gates, read-only. Sets state bits 10 (switch ready: valid, on foot,
// request block idle, cooldown clear), 11 (request block idle), 12 (0xf0388 == 0), and stores slot
// 0's member token (kind<<8|id, or 0xffffffff) at mailbox +0x64. In: x19, x20, w25.
xcx_switch_probe:
    stp  x29, x30, [sp, #-0x20]!
    mvn  w9, wzr
    add  x8, sp, #0x10
    str  w9, [sp, #0x10]
    mov  w0, wzr
    bl   xcx_slot_handle
    mvn  w11, wzr
    ldr  w9, [sp, #0x10]
    tst  w9, #0x8007ffff
    b.eq 1f
    mov  w0, wzr
    bl   xcx_slot_record
    cbz  x0, 1f
    ldrb w9, [x0, #0x78]
    ldrb w10, [x0, #0x79]
    orr  w11, w10, w9, lsl #8
1:
    str  w11, [x19, #0x64]
    mov  w9, #0x4130
    movk w9, #0x3, lsl #16
    add  x12, x20, x9
    ldr  x9, [x12, #0xa8]
    ldr  x10, [x12, #0x38]
    orr  x9, x9, x10
    ldr  x10, [x12, #0x70]
    orr  x9, x9, x10
    ldrh w10, [x12, #0xb0]
    orr  x9, x9, x10
    ldrh w10, [x12, #0xb2]
    orr  x9, x9, x10
    ldrb w10, [x12, #0xb6]
    orr  x9, x9, x10
    cbnz x9, 2f
    orr  w25, w25, #0x800               // idle
2:
    adrp x9, xcx_ctrl_object
    add  x9, x9, :lo12:xcx_ctrl_object
    ldr  x9, [x9]
    cbz  x9, 3f
    bl   xcx_cooldown
    tbnz w0, #0, 3f
    orr  w25, w25, #0x1000              // 0xf0388() == 0
3:
    mov  w10, #0xB805                   // valid, on foot, idle, cooldown clear, input ready, field ok
    and  w9, w25, w10
    cmp  w9, w10
    b.ne 4f
    orr  w25, w25, #0x400
4:
    ldp  x29, x30, [sp], #0x20
    ret

// ---------------------------------------------------------------------------
// xcx_return_probe: state bit14 can_return (0xf16b0, the consumer's predicate), bit15 field_ok
// (0xf0b44, the loading/fade gate: required for every op and bit since HELPER-LIVE-5), and the plain-memory components of
// 0xf0b44 (LOADING-AND-RETURN Q1) into mailbox words 0x74..0xb0 for live diffing. Both calls are the
// ones the game makes itself every frame from the HUD update (0xf16b0 from 0x4162ec; 0xf0b44 from
// 0x562cb4 and inside 0xf16b0); 0xf0b44 takes a lock and may lazily create the fade object, as the
// game does each frame, so it is called here on the game thread. Every pointer is null-checked.
xcx_return_probe:
    stp  x29, x30, [sp, #-0x20]!
    tbnz w25, #15, 1f
    bl   xcx_field_ok                   // diagnostic in states that never reached the gate
    tbz  w0, #0, 2f
    orr  w25, w25, #0x8000
1:
    tbz  w25, #13, 2f
    bl   xcx_can_return                 // only under the field_ok and input gates
    tbz  w0, #0, 2f
    orr  w25, w25, #0x4000
2:
    adrp x9, xcx_action_block           // 0x74: u32 [*(main+0x1ff3698)+0x44]
    add  x9, x9, :lo12:xcx_action_block
    ldr  x9, [x9]
    mvn  w10, wzr
    cbz  x9, 3f
    ldr  w10, [x9, #0x44]
3:  str  w10, [x19, #0x74]
    adrp x9, xcx_btl_lock               // 0x78: u32 main+0x26e1950
    add  x9, x9, :lo12:xcx_btl_lock
    ldr  w10, [x9]
    str  w10, [x19, #0x78]
    adrp x9, xcx_flag_2308              // 0x7c: u8 main+0x26e2308
    add  x9, x9, :lo12:xcx_flag_2308
    ldrb w10, [x9]
    str  w10, [x19, #0x7c]
    adrp x9, xcx_mode_1618              // 0x80: u32 main+0x1dc1618
    add  x9, x9, :lo12:xcx_mode_1618
    ldr  w10, [x9]
    str  w10, [x19, #0x80]
    adrp x9, xcx_ctrl_object            // 0x84: u32 [obj+0x10], 0x88: u8 [obj+0x12]
    add  x9, x9, :lo12:xcx_ctrl_object
    ldr  x9, [x9]
    mvn  w10, wzr
    mvn  w11, wzr
    cbz  x9, 4f
    ldr  w10, [x9, #0x10]
    ldrb w11, [x9, #0x12]
4:  str  w10, [x19, #0x84]
    str  w11, [x19, #0x88]
    adrp x9, xcx_ui_singleton           // 0x8c fade object low32 (0 none), 0x90 layers, 0x94 lock byte
    add  x9, x9, :lo12:xcx_ui_singleton
    ldr  x9, [x9]
    mov  w10, wzr
    mov  w11, wzr
    mov  w13, wzr
    cbz  x9, 5f
    mov  w12, #0x4b98
    movk w12, #0x9, lsl #16
    ldr  x12, [x9, x12]                 // F = *(U+0x94b98)
    cbz  x12, 5f
    mov  w10, w12
    ldr  x14, [x12, #0x8]
    ldr  x15, [x12, #0x10]
    sub  x15, x15, x14
    lsr  x11, x15, #3
    ldrb w13, [x12, #0x70]
5:  str  w10, [x19, #0x8c]
    str  w11, [x19, #0x90]
    str  w13, [x19, #0x94]
    mov  w9, #0x10d0                    // 0x98: H+0x210d0 | H+0x210d1 << 8
    movk w9, #0x2, lsl #16
    add  x9, x20, x9
    ldrb w10, [x9]
    ldrb w11, [x9, #1]
    orr  w10, w10, w11, lsl #8
    str  w10, [x19, #0x98]
    adrp x9, xcx_zone_ptr               // 0x9c world id (+0x140), 0xa0 zone (+0xa8)
    add  x9, x9, :lo12:xcx_zone_ptr
    ldr  x9, [x9]
    mvn  w10, wzr
    mvn  w11, wzr
    cbz  x9, 6f
    ldr  w10, [x9, #0x140]
    ldr  w11, [x9, #0xa8]
6:  str  w10, [x19, #0x9c]
    str  w11, [x19, #0xa0]
    adrp x9, xcx_event_byte             // 0xa4: u8 main+0x1fefb38
    add  x9, x9, :lo12:xcx_event_byte
    ldrb w10, [x9]
    str  w10, [x19, #0xa4]
    adrp x9, xcx_obj_1f91e30            // 0xa8: u8 [*(main+0x1f91e30)+0x14]
    add  x9, x9, :lo12:xcx_obj_1f91e30
    ldr  x9, [x9]
    mvn  w10, wzr
    cbz  x9, 7f
    ldrb w10, [x9, #0x14]
7:  str  w10, [x19, #0xa8]
    adrp x9, xcx_obj_1fefa20            // 0xac: low32 of [*(main+0x1fefa20)+0x30]
    add  x9, x9, :lo12:xcx_obj_1fefa20
    ldr  x9, [x9]
    mvn  w10, wzr
    cbz  x9, 8f
    ldr  x9, [x9, #0x30]
    mov  w10, w9
8:  str  w10, [x19, #0xac]
    mov  w9, #0x63a5                    // 0xb0: u8 H+0x263a5 (the Return request byte)
    movk w9, #0x2, lsl #16
    ldrb w10, [x20, x9]
    str  w10, [x19, #0xb0]
    ldp  x29, x30, [sp], #0x20
    ret
