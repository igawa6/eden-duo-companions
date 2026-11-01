// SPDX-License-Identifier: GPL-3.0-or-later
// Sword1.3.2 actual EXP/move-learning and evolution effects, without opening Bag.
// Native evidence:796800/798730/797E30 (EXP),77DA90/77DB00/C8DA80 (evolution).
// State and externally borrowed EvoParams remain in guest mailbox memory until the native
// effect finishes. Once committed, pause/cancellation cannot split the item's EXP/evolution
// flow. Keep the original live target identity and finish native cleanup before acknowledging.
// A guest reset destroys both tasks and mailbox.
// Mailbox500..6AF is private guest state. 170 publishes active request sequence to the host.

fop_progression:
    ldr w25, [x20, #0x118]             // item
    ldr w26, [x20, #0x11c]             // party slot
    cmp w26, #5
    b.hi frefuse_bad_arg
    cmp w25, #50
    b.eq 1f
    sub w9, w25, #1124
    cmp w9, #4
    b.ls 1f
    cmp w25, #80
    b.lo frefuse_not_usable
    cmp w25, #85
    b.ls 1f
    sub w9, w25, #107
    cmp w9, #2
    b.ls 1f
    cmp w25, #849
    b.ne frefuse_not_usable
1:  adrp x9, sw_fieldmgr_slot
    add x9, x9, :lo12:sw_fieldmgr_slot
    ldr x9, [x9]
    ldr x28, [x9]                     // G
    cbz x28, frefuse_not_in_party
    ldr x9, [x28, #0x90]
    cbz x9, frefuse_not_in_party
    ldr x21, [x9, #0x58]
    cbz x21, frefuse_not_in_party
    ldr x9, [x21]
    adrp x10, sw_party_vtable
    add x10, x10, :lo12:sw_party_vtable
    cmp x9, x10
    b.ne frefuse_not_in_party
    ldrb w9, [x21, #0x88]
    cmp x26, x9
    b.hs frefuse_not_in_party
    bl gamedata
    cbz x0, frefuse_not_in_party
    ldr x22, [x0, #0x1d8]
    cbz x22, frefuse_not_in_party
    add x9, x21, x26, lsl #3
    ldr x9, [x9, #0x58]
    cbz x9, frefuse_not_in_party
    ldr w9, [x9, #0xd0]
    ldr w10, [x20, #0x138]
    cmp w9, w10
    b.ne fprogress_changed
    mov x0, x22
    mov w1, w25
    bl sw_item_count
    cbz w0, frefuse_no_item
    mov x0, x21
    mov w1, w26
    ldr x8, [x0]
    ldr x8, [x8, #0x50]
    blr x8
    cbz x0, frefuse_not_in_party
    mov x27, x0                      // exact native mon
    // Initialize guest state and POD buffers before eligibility checks.
    add x9, x20, #0x500
    mov w10, #0x1b0 / 16
2:  stp xzr, xzr, [x9], #16
    subs w10, w10, #1
    b.ne 2b
    str x21, [x20, #0x520]
    str x22, [x20, #0x528]
    str x27, [x20, #0x530]
    str x28, [x20, #0x540]
    str w25, [x20, #0x50c]
    str w26, [x20, #0x510]
    ldr w9, [x20, #0x120]
    str w9, [x20, #0x508]
    ldr w9, [x20, #0x138]
    str w9, [x20, #0x514]
    str w23, [x20, #0x504]
    cmp w25, #50
    b.eq fprogress_candy
    sub w9, w25, #1124
    cmp w9, #4
    b.ls fprogress_candy
    // Evolution stone: native BagStateEvolution's own predicate.
    add x0, x20, #0x640
    mov x1, x28
    mov w2, wzr
    mov w3, wzr
    bl sw_evolution_context
    mov x0, x27
    add x1, x20, #0x640
    mov w2, w25
    add x3, x20, #0x608
    add x4, x20, #0x60c
    bl sw_evolution_item_check
    tbz w0, #0, frefuse_no_effect
    mov w9, #1
    strb w9, [x20, #0x614]            // native stone path flag
    bl fprogress_retain
    bl fprogress_consume
    cbz w0, fprogress_start_no_item
    bl fprogress_evolve
    b field_done

fprogress_candy:
    // Native Bag7BB190 explicitly permits RareCandy at100 only when evolution is eligible.
    mov x0, x27
    bl sw_mon_level
    cmp w0, #100
    b.lo fprogress_candy_under100
    cmp w25, #50
    b.ne frefuse_no_effect
    bl sw_overworld_weather
    mov w3, w0
    add x0, x20, #0x640
    mov x1, x28
    mov w2, wzr
    bl sw_evolution_context
    mov x0, x27
    add x1, x20, #0x640
    mov x2, x21
    add x3, x20, #0x608
    add x4, x20, #0x60c
    bl sw_evolution_level_check
    tbz w0, #0, frefuse_no_effect
    bl fprogress_retain
    bl fprogress_consume
    cbz w0, fprogress_start_no_item
    bl fprogress_evolve
    b field_done
fprogress_candy_under100:
    // Native shared CanUse checks, not shared Apply.
    mov x0, x21
    mov w1, w25
    mov w2, w26
    mov w3, wzr
    bl sw_item_can_use
    tbz w0, #0, frefuse_no_effect
    mov x0, x27
    bl sw_mon_level
    cmp w0, #100
    b.hs frefuse_no_effect
    str w0, [x20, #0x560]
    cmp w25, #50
    b.ne fprogress_exp_candy
    mov x0, x27
    bl sw_mon_species
    mov w24, w0
    mov x0, x27
    bl sw_mon_form
    mov w1, w0
    mov w0, w24
    bl sw_growth_select
    ldr w0, [x20, #0x560]
    add w0, w0, #1
    bl sw_growth_exp
    mov w24, w0
    mov x0, x27
    bl sw_mon_exp
    sub w24, w24, w0
    b fprogress_exp_ready
fprogress_exp_candy:
    // Native table20450F0, values100/800/3000/10000/30000, one item.
    sub w9, w25, #1124
    adrp x10, sw_exp_candy_values
    add x10, x10, :lo12:sw_exp_candy_values
    ldr w24, [x10, x9, lsl #2]
fprogress_exp_ready:
    cbz w24, frefuse_no_effect
    add x9, x20, #0x580
    str w24, [x9, x26, lsl #2]
    str x21, [x20, #0x5c0]           // EXPparams+40 party
    mov x9, #1
    str x9, [x20, #0x5c8]            // +48 mode
    strh w9, [x20, #0x5d0]           // +50 flags
    add x9, x20, #0x688
    str x9, [x20, #0x680]            // pointer to persistent output byte
    ldr x9, [x28, #0x60]             // appli manager
    cbz x9, frefuse_not_usable
    str x9, [x20, #0x548]
    adrp x9, sw_resource_slot
    add x9, x9, :lo12:sw_resource_slot
    ldr x9, [x9]
    ldr x9, [x9]
    cbz x9, frefuse_not_usable
    ldr x9, [x9, #0x288]             // application heap
    cbz x9, frefuse_not_usable
    str x9, [x20, #0x550]
    bl fprogress_retain
    bl fprogress_consume
    cbz w0, fprogress_start_no_item
    ldr x0, [x20, #0x548]
    ldr x1, [x20, #0x550]
    add x2, x20, #0x550
    add x3, x20, #0x548
    add x4, x20, #0x580
    add x5, x20, #0x680
    add x8, x20, #0x538
    bl sw_exp_effect
    mov w9, #1
    str w9, [x20, #0x500]            // EXP phase
    str w23, [x20, #0x170]
    str wzr, [x20, #0x104]
    b field_done

fprogress_changed:
    mov w0, #27
    b frefuse
fprogress_start_no_item:
    bl fprogress_release
    b frefuse_no_item

// Retain the exact target and party for the complete asynchronous operation.
fprogress_retain:
    stp x29, x30, [sp, #-16]!
    mov x0, x21
    bl obj_lock
    mov x0, x27
    bl obj_lock
    ldp x29, x30, [sp], #16
    ret
fprogress_consume:
    stp x29, x30, [sp, #-16]!
    mov x0, x22
    mov w1, w25
    mov w2, #1
    bl sw_sub_item
    ldp x29, x30, [sp], #16
    ret

fprogress_evolve:
    stp x29, x30, [sp, #-16]!
    ldr x9, [x20, #0x530]
    str x9, [x20, #0x600]
    mov w9, #6
    str w9, [x20, #0x610]            // native Bag/level-up evolution mode
    mov w9, #1
    strb w9, [x20, #0x615]
    strb w9, [x20, #0x618]
    mov w10, #90
    strh w10, [x20, #0x616]
    str w9, [x20, #0x620]
    ldr x0, [x20, #0x540]
    add x1, x20, #0x600
    bl sw_evolution_effect
    ldr x9, [x20, #0x540]
    ldr x9, [x9, #0x68]
    cbz x9, 1f
    ldr x9, [x9, #0x78]
1:  str x9, [x20, #0x558]            // latch the native queued field task, never dereference after pop
    mov w9, #2
    str w9, [x20, #0x500]
    ldr w9, [x20, #0x504]
    str w9, [x20, #0x170]
    str wzr, [x20, #0x104]
    ldp x29, x30, [sp], #16
    ret

// Returns1 while an operation owned by an earlier frame is active. Never restarts a request.
fprogress_poll:
    stp x29, x30, [sp, #-16]!
    ldr w9, [x20, #0x500]
    cbz w9, fprogress_idle
    str wzr, [x20, #0x104]
    ldr w10, [x20, #0x508]
    ldr w11, [x20, #0xffc]
    cmp w10, w11
    b.ne fprogress_cancel_pending
    ldr w10, [x20, #0x504]
    ldr w11, [x20, #0x124]
    cmp w10, w11
    b.ne fprogress_poll_phase
fprogress_cancel_pending:
    mov w10, #1
    str w10, [x20, #0x518]
 fprogress_poll_phase:
    cmp w9, #2
    b.eq fprogress_poll_evo
    cmp w9, #3
    b.eq fprogress_poll_restore
    ldr x9, [x20, #0x538]
    cbz x9, fprogress_finish
    add x9, x9, #0x54
    ldar w9, [x9]
    cbnz w9, fprogress_wait
    ldr x0, [x20, #0x538]
    bl ref_release
    str xzr, [x20, #0x538]
    // Recheck identity at the asynchronous boundary before another native effect.
    // The retained party must still be the current one; matching an old retained slot's EC
    // alone would not protect a title/save transition that replaces the whole party object.
    adrp x9, sw_fieldmgr_slot
    add x9, x9, :lo12:sw_fieldmgr_slot
    ldr x9, [x9]
    ldr x9, [x9]
    ldr x10, [x20, #0x540]
    cmp x9, x10
    b.ne fprogress_target_changed
    ldr x9, [x9, #0x90]
    cbz x9, fprogress_target_changed
    ldr x9, [x9, #0x58]
    ldr x10, [x20, #0x520]
    cmp x9, x10
    b.ne fprogress_target_changed
    ldr x9, [x20, #0x520]
    ldr w10, [x20, #0x510]
    ldrb w11, [x9, #0x88]
    cmp x10, x11
    b.hs fprogress_target_changed
    add x9, x9, x10, lsl #3
    ldr x9, [x9, #0x58]
    cbz x9, fprogress_target_changed
    ldr w9, [x9, #0xd0]
    ldr w10, [x20, #0x514]
    cmp w9, w10
    b.ne fprogress_target_changed
    // The native post-EXP eligibility check handles level/time/weather/party rules.
    bl sw_overworld_weather
    mov w3, w0
    add x0, x20, #0x640
    ldr x1, [x20, #0x540]
    ldrb w2, [x20, #0x688]
    bl sw_evolution_context
    ldr x0, [x20, #0x530]
    add x1, x20, #0x640
    ldr x2, [x20, #0x520]
    add x3, x20, #0x608
    add x4, x20, #0x60c
    bl sw_evolution_level_check
    tbz w0, #0, fprogress_finish
    strb wzr, [x20, #0x614]
    bl fprogress_evolve
    b fprogress_wait
fprogress_target_changed:
    mov w9, #32
    str w9, [x20, #0x51c]            // EXP committed, but never evolve a replaced target/context
    b fprogress_finish
fprogress_poll_evo:
    // Native status5 can precede move-learning completion; with no new move the task
    // finishes leaving status1 (C8E674..C8E69C). Native Bag resumes on task pop, not status.
    // We start from an empty task stack, latch the pushed task, and retain params until empty.
    ldr x9, [x20, #0x540]
    ldr x9, [x9, #0x68]
    cbz x9, fprogress_wait
    ldr x9, [x9, #0x78]
    cbz x9, 1f
    str x9, [x20, #0x558]
    b fprogress_wait
1:  ldr x9, [x20, #0x558]
    cbz x9, fprogress_wait
    // The native overworld evolution owner E08920 restores the global screen fade after
    // C8DA80 finishes (E08AF8..E08B70). The effect itself leaves opaque black on channel0.
    // Use the same native eight-frame transition and completion query before returning.
    bl fprogress_fade_manager
    cbz x0, fprogress_wait
    mov w1, wzr
    mov w2, wzr
    mov w3, #8
    bl sw_field_fade_in
    mov w9, #3
    str w9, [x20, #0x500]
    b fprogress_wait
fprogress_poll_restore:
    bl fprogress_fade_manager
    cbz x0, fprogress_wait
    mov w1, wzr
    bl sw_field_fade_done
    tbz w0, #0, fprogress_wait
fprogress_finish:
    ldr w23, [x20, #0x504]
    ldr w25, [x20, #0x50c]
    bl fprogress_release
    str wzr, [x20, #0x500]
    str wzr, [x20, #0x170]
    ldr w9, [x20, #0x110]
    cmp w9, w23
    b.ne fprogress_wait              // never acknowledge a new request with an old result
    ldr w0, [x20, #0x51c]
    cbnz w0, fprogress_ack_error
    str w25, [x20, #0x148]
    b fprogress_ack
fprogress_ack_error:
    str wzr, [x20, #0x148]
fprogress_ack:
    str w0, [x20, #0x144]
    ldr w9, [x20, #0x100]
    str w9, [x20, #0x14c]
    add x9, x20, #0x140
    stlr w23, [x9]
fprogress_wait:
    mov w0, #1
    ldp x29, x30, [sp], #16
    ret
fprogress_idle:
    mov w0, wzr
    ldp x29, x30, [sp], #16
    ret
fprogress_fade_manager:
    adrp x9, sw_fade_slot
    add x9, x9, :lo12:sw_fade_slot
    ldr x9, [x9]
    cbz x9, 1f
    ldr x0, [x9]
    ret
1:  mov x0, xzr
    ret
fprogress_release:
    stp x29, x30, [sp, #-16]!
    ldr x0, [x20, #0x530]
    cbz x0, 1f
    bl ref_release
    str xzr, [x20, #0x530]
1:  ldr x0, [x20, #0x520]
    cbz x0, 2f
    bl ref_release
    str xzr, [x20, #0x520]
2:  ldp x29, x30, [sp], #16
    ret
