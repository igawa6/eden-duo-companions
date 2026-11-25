// SPDX-FileCopyrightText: Copyright 2026 Sickops
// SPDX-License-Identifier: GPL-3.0-or-later
// Z-A 2.0.2. Separate hooks preserve the field and map update contexts.
.text
.p2align 2
.global za_travel_open
za_travel_open:
    // Preserve the normal button path, including its eligibility checks.
    stp x9, x10, [sp, #-32]!
    stp x11, x12, [sp, #16]
    adrp x9, za_mailbox
    add x9, x9, :lo12:za_mailbox
    add x10, x9, #0x100
    tbnz w0, #0, .Lmanual_open
    ldar w11, [x10]
    cbz w11, .Lopen_idle
    stlr wzr, [x10]
    ldr w11, [x9, #0x104]
    ldr w12, [x9, #4092]
    eor w11, w11, w12
    cbnz w11, .Lopen_idle
    ldr w11, [x9, #0x108]
    ldr w12, [x9, #8]
    sub w11, w11, w12
    tbnz w11, #31, .Lopen_idle
.Lrestore_open:
    ldp x11, x12, [sp, #16]
    ldp x9, x10, [sp], #32
    b za_native_map_requested
.Lmanual_open:
    stlr wzr, [x10]
    b .Lrestore_open
.Lopen_idle:
    ldp x11, x12, [sp, #16]
    ldp x9, x10, [sp], #32
    b za_native_map_idle

.p2align 2
.global za_travel_select
za_travel_select:
    // This entry is the beginning of the map owner's ordinary update.
    // Preserve the native arguments and the caller's return address.
    stp x0, x1, [sp, #-96]!
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x30, [sp, #64]
    str x19, [sp, #80]
    adrp x19, za_mailbox
    add x19, x19, :lo12:za_mailbox
    add x9, x19, #0x10c
    ldar w10, [x9]
    cbz w10, resume
    ldr w10, [x19, #0x104]
    ldr w11, [x19, #4092]
    cmp w10, w11
    b.ne expired
    ldr w10, [x19, #0x108]
    ldr w11, [x19, #8]
    sub w10, w10, w11
    tbnz w10, #31, expired

    adrp x10, native_map_owner
    add x10, x10, :lo12:native_map_owner
    ldr x11, [x0]
    cmp x10, x11
    b.ne resume
    ldr w10, [x0, #0x114]
    cmp w10, #8
    b.ne resume
    ldr x0, [x0, #0x1e0]
    cbz x0, resume
    adrp x10, native_map_model
    add x10, x10, :lo12:native_map_model
    ldr x11, [x0]
    cmp x10, x11
    b.ne resume
    ldr x10, [x0, #0x10]
    cmp x10, x0
    b.ne resume
    ldrb w10, [x0, #0x585]
    tbz w10, #0, resume
    ldr w10, [x0, #0x13c]
    cmp w10, #32
    b.ne resume

    ldr w10, [x19, #0x10c]
    cmp w10, #2
    b.eq apply_marker
    cmp w10, #3
    b.eq apply_marker

    ldr w16, [x19, #0x124]
    cmp w16, #9
    b.eq scan_points
    cmp w16, #13
    b.eq scan_points
    cmp w16, #15
    b.eq scan_points
    cmp w16, #4
    b.ne invalid_destination
scan_points:
    ldp x11, x12, [x0, #0x50]
    cbz x11, invalid_destination
    sub x13, x12, x11
    cmp x13, #0x2000
    b.hi invalid_destination
    tst x13, #31
    b.ne invalid_destination
    mov w15, #1024
next_group:
    cmp x11, x12
    b.eq invalid_destination
    ldp x1, x13, [x11], #32
    cmp x1, x13
    b.eq next_group
    b.hi invalid_destination
    cbz x1, invalid_destination
next_point:
    sub x14, x13, x1
    cmp x14, #0x90
    b.lo invalid_destination
    subs w15, w15, #1
    b.mi invalid_destination
    ldr w14, [x1, #4]
    cmp w14, w16
    b.ne advance_point
    ldrb w14, [x1]
    tbz w14, #1, advance_point
    ldr w10, [x1, #0x28]
    ldr w14, [x19, #0x110]
    cmp w10, w14
    b.eq primary_point
    ldr w14, [x19, #0x128]
    cmp w10, w14
    b.ne advance_point
    ldr x17, [x19, #0x130]
    b check_catalog
primary_point:
    ldr x17, [x19, #0x118]
check_catalog:
    cbz x17, advance_point
    ldr x14, [x0, #0x4a8]
    cbz x14, invalid_destination
    ldr w8, [x14, #0x160]
    cmp w8, #4096
    b.hi invalid_destination
    cmp w10, w8
    b.hs advance_point
    ldr x14, [x14, #0x120]
    cbz x14, invalid_destination
    add x14, x14, x10, lsl #5
    ldr x8, [x14]
    cmp x8, x17
    b.ne advance_point
    ldr x8, [x14, #0x10]
    cbz x8, advance_point
    add x10, x19, #0x100
    stlr wzr, [x10]
    stlr wzr, [x9]
    add x0, x0, #0x138
    sub sp, sp, #128
    stp q0, q1, [sp]
    stp q2, q3, [sp, #32]
    stp q4, q5, [sp, #64]
    stp q6, q7, [sp, #96]
    bl native_travel_dialog
    ldp q0, q1, [sp]
    ldp q2, q3, [sp, #32]
    ldp q4, q5, [sp, #64]
    ldp q6, q7, [sp, #96]
    add sp, sp, #128
    mov w10, #1
    b report
apply_marker:
    // The native pin routines own both the map record and the minimap target.
    // They must run on this map owner's update, after its normal ready checks.
    ldp x11, x12, [x0, #0x50]
    cbz x11, invalid_destination
    sub x12, x12, x11
    cmp x12, #64
    b.lo invalid_destination
    cmp x12, #0x2000
    b.hi invalid_destination
    ldp x11, x12, [x11, #32]
    cbz x11, invalid_destination
    sub x12, x12, x11
    cmp x12, #0x90
    b.ne invalid_destination
    ldr w12, [x11, #4]
    cmp w12, #1
    b.ne invalid_destination
    sub sp, sp, #160
    stp q0, q1, [sp]
    stp q2, q3, [sp, #32]
    stp q4, q5, [sp, #64]
    stp q6, q7, [sp, #96]
    str x0, [sp, #136]
    cmp w10, #3
    b.eq clear_marker
    add x11, x19, #0x138
    ldp s0, s1, [x11]
    fabs s2, s0
    fabs s3, s1
    fmul s2, s2, s2
    fmadd s2, s3, s3, s2
    mov w11, #0x4a800000
    fmov s3, w11
    fcmp s2, s3
    b.hi marker_rejected
    b.vs marker_rejected
    ldr x11, [x0, #0x388]
    str x11, [sp, #128]
    ldr w11, [x0, #0x264]
    str w11, [sp, #144]
    mov w11, #2
    str w11, [x0, #0x264]
    ldr x11, [x19, #0x138]
    str x11, [x0, #0x388]
    bl native_marker_set
    ldr x0, [sp, #136]
    ldr x11, [sp, #128]
    str x11, [x0, #0x388]
    ldr w11, [sp, #144]
    str w11, [x0, #0x264]
    b marker_applied
clear_marker:
    bl native_marker_clear
marker_applied:
    ldr x0, [sp, #136]
    str x0, [x19, #0x140]
    mov w10, #4
    b restore_marker
marker_rejected:
    mov w10, #3
restore_marker:
    ldp q0, q1, [sp]
    ldp q2, q3, [sp, #32]
    ldp q4, q5, [sp, #64]
    ldp q6, q7, [sp, #96]
    add sp, sp, #160
    add x11, x19, #0x100
    stlr wzr, [x11]
    add x9, x19, #0x10c
    stlr wzr, [x9]
    b report
advance_point:
    add x1, x1, #0x90
    cmp x1, x13
    b.ne next_point
    b next_group
expired:
    mov w10, #2
    b reject
invalid_destination:
    mov w10, #3
reject:
    add x11, x19, #0x100
    stlr wzr, [x11]
    stlr wzr, [x9]
report:
    add x11, x19, #0x120
    stlr w10, [x11]
resume:
    ldr x19, [sp, #80]
    ldp x8, x30, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp], #96
    sub sp, sp, #0x130
    b native_map_tick_resume
