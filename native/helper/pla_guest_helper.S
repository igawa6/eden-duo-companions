// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// 1.1.1 only. Hook main+0x19e5ff0: x0 is the just-resolved live view,
// x19 is the player. Resume reproduces ldrb w8,[x19,#0x7fc].
// NZCV is dead here: native successors use CBZ/CBNZ, then define flags.
// Save all volatile GPRs and every SIMD register before any native call.
#include "pla_mailbox.h"
.text
.p2align 2
.global pla_entry
pla_entry:
    stp x16, x17, [sp, #-16]!
    adrp x16, pla_mailbox
    add x16, x16, :lo12:pla_mailbox
    ldar x17, [x16]                    // req (low32) + ack (high32), one acquire read
    lsr x16, x17, #32
    sub w16, w17, w16                  // no NZCV change on idle path
    cbz w16, pla_idle
    sub sp, sp, #0x2c0
    stp x0, x1, [sp, #0x0]
    stp x2, x3, [sp, #0x10]
    stp x4, x5, [sp, #0x20]
    stp x6, x7, [sp, #0x30]
    stp x8, x9, [sp, #0x40]
    stp x10, x11, [sp, #0x50]
    stp x12, x13, [sp, #0x60]
    stp x14, x15, [sp, #0x70]
    stp x18, x30, [sp, #0x80]
    stp x20, x21, [sp, #0xa0]
    stp x22, x23, [sp, #0xb0]
    stp q0, q1, [sp, #0xc0]
    stp q2, q3, [sp, #0xe0]
    stp q4, q5, [sp, #0x100]
    stp q6, q7, [sp, #0x120]
    stp q8, q9, [sp, #0x140]
    stp q10, q11, [sp, #0x160]
    stp q12, q13, [sp, #0x180]
    stp q14, q15, [sp, #0x1a0]
    stp q16, q17, [sp, #0x1c0]
    stp q18, q19, [sp, #0x1e0]
    stp q20, q21, [sp, #0x200]
    stp q22, q23, [sp, #0x220]
    stp q24, q25, [sp, #0x240]
    stp q26, q27, [sp, #0x260]
    stp q28, q29, [sp, #0x280]
    stp q30, q31, [sp, #0x2a0]
    adrp x20, pla_mailbox
    add x20, x20, :lo12:pla_mailbox
    mov w21, w17
    mov x22, x0                        // only dereference the native live object
    ldr x9, [x20, #PLA_VIEW]
    cbz x9, pla_invalid
    tst x9, #7
    b.ne pla_invalid
    cmp x9, x22
    b.ne pla_invalid
    ldr x9, [x22]
    adrp x10, pla_type
    add x10, x10, :lo12:pla_type
    cmp x9, x10
    b.ne pla_invalid
    // Refuse pad input before calling; original handling runs after we resume.
    ldrb w9, [x19, #0x7fc]
    cbnz w9, pla_invalid
    ldrb w9, [x19, #0x800]
    cbnz w9, pla_invalid
    ldrb w9, [x19, #0x801]
    cbnz w9, pla_invalid
    ldrb w9, [x19, #0x802]
    cbnz w9, pla_invalid
    ldr w9, [x22, #0x750]
    cmp w9, #2
    b.eq pla_busy
    ldr w9, [x22, #0x704]
    cbnz w9, pla_busy
    ldrb w9, [x22, #0x5b4]              // native toggle's enabled guard
    cbz w9, pla_busy
    ldr w9, [x22, #0x5ac]
    ldr w10, [x20, #PLA_MODE]
    cmp w9, w10
    b.ne pla_invalid
    cmp w9, #1
    b.hi pla_invalid
    ldr w9, [x22, #0x6f8]
    ldr w10, [x20, #PLA_ITEM]
    cmp w9, w10
    b.ne pla_invalid
    ldr w9, [x22, #0x6fc]
    ldr w10, [x20, #PLA_PARTY]
    cmp w9, w10
    b.ne pla_invalid
    // List lengths must still match the sample (six-byte items, u64 party).
    ldr x9, [x22, #0x5c0]
    ldr x10, [x22, #0x5c8]
    sub x10, x10, x9
    ldr w11, [x20, #PLA_ITEMS]
    cmp w11, #64
    b.hi pla_invalid
    mov x12, #6
    mul x11, x11, x12
    cmp x10, x11
    b.ne pla_invalid
    ldr x9, [x22, #0x630]
    ldr x10, [x22, #0x638]
    sub x10, x10, x9
    ldr w11, [x20, #PLA_PARTIES]
    cmp w11, #6
    b.hi pla_invalid
    lsl x11, x11, #3
    cmp x10, x11
    b.ne pla_invalid
    add x9, x20, #PLA_CANCEL
    ldar w9, [x9]
    cmp w9, w21
    b.eq pla_invalid
    add x9, x20, #PLA_EPOCH
    ldar w9, [x9]
    ldr w10, [x20, #PLA_REQ_EPOCH]
    cbz w9, pla_invalid
    cmp w9, w10
    b.ne pla_invalid
    ldr w23, [x20, #PLA_OP]
    mov x0, x22
    cmp w23, #PLA_TOGGLE
    b.eq pla_do_toggle
    ldr w9, [x22, #0x5ac]
    add x10, x22, #0x742
    ldrb w9, [x10, x9]
    cbz w9, pla_busy
    cmp w23, #PLA_NEXT
    b.eq pla_do_next
    cmp w23, #PLA_PREVIOUS
    b.ne pla_invalid
    bl pla_previous
    b pla_called
pla_do_next:
    bl pla_next
    b pla_called
pla_do_toggle:
    bl pla_toggle
pla_called:
    // A call that returned without changing mode/index is not success.
    ldr w9, [x22, #0x5ac]
    ldr w10, [x20, #PLA_MODE]
    cmp w9, w10
    b.ne pla_done
    ldr w9, [x22, #0x6f8]
    ldr w10, [x20, #PLA_ITEM]
    cmp w9, w10
    b.ne pla_done
    ldr w9, [x22, #0x6fc]
    ldr w10, [x20, #PLA_PARTY]
    cmp w9, w10
    b.eq pla_invalid
pla_done:
    mov w0, #PLA_DONE
    b pla_ack
pla_busy:
    mov w0, #PLA_BUSY
    b pla_ack
pla_invalid:
    mov w0, #PLA_INVALID
    // Refusals need no object reads (including a failed type/view check).
    str wzr, [x20, #PLA_POST_MODE]
    str wzr, [x20, #PLA_POST_ITEM]
    str wzr, [x20, #PLA_POST_PARTY]
    b pla_publish
pla_ack:
    // Only DONE/BUSY reach here, after validating the live object type.
    ldr w9, [x22, #0x5ac]
    str w9, [x20, #PLA_POST_MODE]
    ldr w9, [x22, #0x6f8]
    str w9, [x20, #PLA_POST_ITEM]
    ldr w9, [x22, #0x6fc]
    str w9, [x20, #PLA_POST_PARTY]
pla_publish:
    str w0, [x20, #PLA_RESULT]
    add x9, x20, #PLA_ACK
    stlr w21, [x9]                      // publish all results last
    ldp q0, q1, [sp, #0xc0]
    ldp q2, q3, [sp, #0xe0]
    ldp q4, q5, [sp, #0x100]
    ldp q6, q7, [sp, #0x120]
    ldp q8, q9, [sp, #0x140]
    ldp q10, q11, [sp, #0x160]
    ldp q12, q13, [sp, #0x180]
    ldp q14, q15, [sp, #0x1a0]
    ldp q16, q17, [sp, #0x1c0]
    ldp q18, q19, [sp, #0x1e0]
    ldp q20, q21, [sp, #0x200]
    ldp q22, q23, [sp, #0x220]
    ldp q24, q25, [sp, #0x240]
    ldp q26, q27, [sp, #0x260]
    ldp q28, q29, [sp, #0x280]
    ldp q30, q31, [sp, #0x2a0]
    ldp x0, x1, [sp, #0x0]
    ldp x2, x3, [sp, #0x10]
    ldp x4, x5, [sp, #0x20]
    ldp x6, x7, [sp, #0x30]
    ldp x8, x9, [sp, #0x40]
    ldp x10, x11, [sp, #0x50]
    ldp x12, x13, [sp, #0x60]
    ldp x14, x15, [sp, #0x70]
    ldp x18, x30, [sp, #0x80]
    ldp x20, x21, [sp, #0xa0]
    ldp x22, x23, [sp, #0xb0]
    add sp, sp, #0x2c0
pla_idle:
    ldp x16, x17, [sp], #16
    ldrb w8, [x19, #0x7fc]              // overwritten instruction
    b pla_resume
