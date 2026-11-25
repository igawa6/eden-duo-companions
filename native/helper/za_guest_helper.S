// SPDX-FileCopyrightText: Copyright 2026 Sickops
// SPDX-License-Identifier: GPL-3.0-or-later
// Z-A 2.0.2. The load plan replaces the first instruction of the scene update.
// See za_guest_mailbox.h for the request layout. The helper uses the game's
// own accessors and never writes a Pokémon record itself.

.text
.p2align 2
.global za_guest_entry
za_guest_entry:
    adrp x9, za_mailbox
    add x9, x9, :lo12:za_mailbox
    ldr w10, [x9, #8]
    add w10, w10, #1
    str w10, [x9, #8]
    ldar w10, [x9]
    add x11, x9, #4
    ldar w11, [x11]
    cmp w10, w11
    b.eq .Lresume

    // A scene update can be re-entered by the native operation.
    add x11, x9, #12
    ldaxr w12, [x11]
    cbnz w12, .Lresume
    mov w12, #1
    stxr w13, w12, [x11]
    cbnz w13, .Lresume

    sub sp, sp, #0xe0
    stp x0, x1, [sp]
    stp x2, x3, [sp, #0x10]
    stp x4, x5, [sp, #0x20]
    stp x6, x7, [sp, #0x30]
    stp x8, x19, [sp, #0x40]
    stp x20, x30, [sp, #0x50]
    stp q0, q1, [sp, #0x60]
    stp q2, q3, [sp, #0x80]
    stp q4, q5, [sp, #0xa0]
    stp q6, q7, [sp, #0xc0]
    mov x19, x9
    ldar w20, [x19]
    ldr w10, [x19, #4]
    cmp w20, w10
    b.eq .Lunlock

    ldr w10, [x19, #16]
    add x9, x19, #0xffc
    ldar w11, [x9]
    cmp w10, w11
    b.ne .Lreject
    adrp x9, za_scene_phase
    ldr w9, [x9, :lo12:za_scene_phase]
    cbnz w9, .Lreject
    ldr w9, [x19, #20]
    sub w9, w9, #1
    cmp w9, #3
    b.hi .Lreject

    // Resolve the current party with the same ownership and generation
    // checks as the module's read-only team reader.
    adrp x0, za_manager
    ldr x0, [x0, :lo12:za_manager]
    cbz x0, .Lreject
    ldr x9, [x0]
    adrp x10, za_manager_type
    add x10, x10, :lo12:za_manager_type
    cmp x9, x10
    b.ne .Lreject
    ldr x0, [x0, #0x48]
    cbz x0, .Lreject
    ldr x9, [x0]
    adrp x10, za_context_type
    add x10, x10, :lo12:za_context_type
    cmp x9, x10
    b.ne .Lreject
    ldr x0, [x0, #0x40]
    adrp x1, za_actor_type
    add x1, x1, :lo12:za_actor_type
    bl .Lresolve
    cbz x0, .Lreject
    ldr x0, [x0, #0x2038]
    cbz x0, .Lreject
    ldr x9, [x0]
    adrp x10, za_components_type
    add x10, x10, :lo12:za_components_type
    cmp x9, x10
    b.ne .Lreject
    ldr x0, [x0, #0x100]
    adrp x1, za_party_type
    add x1, x1, :lo12:za_party_type
    bl .Lresolve
    cbz x0, .Lreject
    ldr x0, [x0, #0x190]
    cbz x0, .Lreject
    ldr x9, [x0]
    adrp x10, za_storage_type
    add x10, x10, :lo12:za_storage_type
    cmp x9, x10
    b.ne .Lreject
    ldp x12, x13, [x0, #0x70]
    ldp x14, x15, [x0, #0x98]
    cmp x13, x15
    b.ne .Lreject
    cmp x13, #6
    b.hi .Lreject
    ldr w2, [x19, #24]
    cmp x2, x13
    b.hs .Lreject
    cbz x12, .Lreject
    cbz x14, .Lreject

    adrp x10, za_partner_type
    add x10, x10, :lo12:za_partner_type
    mov x9, #0
.Lpartners:
    ldr x15, [x12, x9, lsl #3]
    cbz x15, .Lnext_partner
    ldr x16, [x15]
    cmp x16, x10
    b.ne .Lreject
    ldrb w16, [x15, #0x3ab]
    cbnz w16, .Lreject
.Lnext_partner:
    add x9, x9, #1
    cmp x9, x13
    b.lo .Lpartners

    ldr x0, [x14, x2, lsl #3]
    adrp x1, za_node_type
    add x1, x1, :lo12:za_node_type
    bl .Lresolve
    cbz x0, .Lreject
    ldr x9, [x0, #0x50]
    cbz x9, .Lreject
    ldr x9, [x9, #0x30]
    cbz x9, .Lreject
    ldr x9, [x9]
    ldr x10, [x19, #40]
    cmp x9, x10
    b.ne .Lreject
    ldr w9, [x19, #20]
    cmp w9, #4
    b.eq .Lmedicine
    cmp w9, #3
    b.eq .Lheld_item
    cmp w9, #1
    b.ne .Lexchange

    ldp w1, w2, [x19, #28]
    cmp w1, #3
    b.hi .Lreject
    cbz w2, .Lreject
    lsr w9, w2, #16
    cbnz w9, .Lreject
    bl za_replace_move
    b .Laccepted

.Lexchange:
    ldp w1, w2, [x19, #28]
    cmp w1, #32
    b.hs .Lreject
    cmp w2, #30
    b.hs .Lreject
    adrp x9, za_services
    add x9, x9, :lo12:za_services
    ldp x10, x11, [x9]
    cbz x10, .Lreject
    subs x12, x11, x10
    b.ls .Lreject
    cmp x12, #0x800
    b.hi .Lreject
    tst x12, #7
    b.ne .Lreject
    adrp x12, za_box_service_type
    add x12, x12, :lo12:za_box_service_type
.Lfind_box:
    ldr x13, [x10], #8
    cbz x13, .Lnext_service
    ldr x14, [x13]
    cmp x14, x12
    b.eq .Lcheck_box
.Lnext_service:
    cmp x10, x11
    b.lo .Lfind_box
    b .Lreject
.Lcheck_box:
    ldr x9, [x13, #0x10]
    cmp x9, x13
    b.ne .Lreject
    ldr x9, [x13, #0x978]
    ldr x10, [x13, #0x980]
    cbz x9, .Lreject
    cmp x10, #32
    b.ne .Lreject
    mov w11, #30
    madd w11, w1, w11, w2
    lsr w12, w11, #6
    add x13, x13, #0x40
    ldr x14, [x13, x12, lsl #3]
    lsr x14, x14, x11
    tbz x14, #0, .Lreject
    mov x12, #0x198
    madd x9, x11, x12, x9
    ldr x9, [x9]
    ldr x10, [x19, #48]
    cmp x9, x10
    b.ne .Lreject
    mov w0, wzr
    ldr w3, [x19, #24]
    bl za_exchange_box
    cbz w0, .Lreject
    b .Laccepted

// The bag uses the Pokémon node's equip function and the bag manager's
// decrement / add functions in this order. Validate both quantities before
// calling any of them, and verify the slot before updating stock.
.Lheld_item:
    stp x21, x22, [sp, #-0x20]!
    stp x23, x24, [sp, #0x10]
    mov x21, x0
    ldp w22, w23, [x19, #28]
    orr w9, w22, w23
    lsr w9, w9, #16
    cbnz w9, .Lheld_failed
    cmp w22, w23
    b.eq .Lheld_failed
    ldr x9, [x21]
    ldr x9, [x9, #0x178]
    adrp x10, za_equip_item
    add x10, x10, :lo12:za_equip_item
    cmp x9, x10
    b.ne .Lheld_failed
    adrp x9, za_bag_runtime
    add x9, x9, :lo12:za_bag_runtime
    ldr w9, [x9, #0x80]
    cbz w9, .Lheld_failed
    adrp x9, za_bag_manager
    ldr x24, [x9, :lo12:za_bag_manager]
    cbz x24, .Lheld_failed
    ldr x10, [x21, #0x50]
    ldr x0, [x10, #0x70]
    cbz x0, .Lheld_failed
    bl za_current_item
    cmp w0, w23
    b.ne .Lheld_failed
    cbz w22, .Lcheck_return_space
    mov w0, w22
    bl za_bag_record
    lsr x0, x0, #32
    cbz w0, .Lheld_failed
    str w0, [x19, #64]
.Lcheck_return_space:
    cbz w23, .Lapply_held
    mov w0, w23
    bl za_bag_record
    lsr x0, x0, #32
    str w0, [x19, #68]
    adrp x9, za_item_catalog
    ldr x0, [x9, :lo12:za_item_catalog]
    cbz x0, .Lheld_failed
    mov w1, w23
    bl za_bag_maximum
    ldr w9, [x19, #68]
    cmp w9, w0
    b.hs .Lheld_failed
.Lapply_held:
    mov x0, x21
    mov w1, w22
    cbz w22, .Lremove_held
    bl za_equip_item
    b .Lverify_held
.Lremove_held:
    bl za_unequip_item
.Lverify_held:
    ldr x10, [x21, #0x50]
    ldr x0, [x10, #0x70]
    bl za_current_item
    cmp w0, w22
    b.ne .Lheld_failed
    cbz w22, .Lreturn_held
    mov x0, x24
    mov w1, w22
    mov w2, #1
    bl za_bag_decrement
.Lreturn_held:
    cbz w23, .Lconfirm_held
    mov x0, x24
    mov w1, w23
    mov w2, #1
    mov w3, wzr
    mov w4, wzr
    bl za_bag_add
.Lconfirm_held:
    cbz w22, .Lconfirm_returned
    mov w0, w22
    bl za_bag_record
    lsr x0, x0, #32
    ldr w9, [x19, #64]
    sub w9, w9, #1
    cmp w0, w9
    b.ne .Lheld_failed
.Lconfirm_returned:
    cbz w23, .Lheld_succeeded
    mov w0, w23
    bl za_bag_record
    lsr x0, x0, #32
    ldr w9, [x19, #68]
    add w9, w9, #1
    cmp w0, w9
    b.ne .Lheld_failed
.Lheld_succeeded:
    ldp x23, x24, [sp, #0x10]
    ldp x21, x22, [sp], #0x20
    b .Laccepted
.Lheld_failed:
    ldp x23, x24, [sp, #0x10]
    ldp x21, x22, [sp], #0x20
    b .Lreject

// Same target validation as held items. The two packed state values are
// {u16 HP, u16 status}. Use the game's setters, then spend one medicine.
.Lmedicine:
    stp x21, x22, [sp, #-0x30]!
    stp x23, x24, [sp, #0x10]
    stp x25, x26, [sp, #0x20]
    ldp w22, w23, [x19, #28]
    ldr w24, [x19, #48]
    ldr x21, [x0, #0x50]
    ldr x25, [x21, #0x70]
    cbz x25, .Lmedicine_reject
    cbz w22, .Lmedicine_reject
    lsr w9, w22, #16
    cbnz w9, .Lmedicine_reject
    cmp w23, w24
    b.eq .Lmedicine_reject
    lsr w9, w23, #16
    lsr w10, w24, #16
    cmp w9, #5
    b.hi .Lmedicine_reject
    cmp w10, w9
    b.eq .Lmedicine_health
    cbnz w10, .Lmedicine_reject
.Lmedicine_health:
    uxth w9, w23
    uxth w10, w24
    cmp w10, w9
    b.lo .Lmedicine_reject
    mov x0, x25
    bl za_current_hp
    uxth w9, w23
    cmp w0, w9
    b.ne .Lmedicine_reject
    mov x0, x25
    bl za_current_status
    lsr w9, w23, #16
    cmp w0, w9
    b.ne .Lmedicine_reject
    adrp x9, za_bag_runtime
    add x9, x9, :lo12:za_bag_runtime
    ldr w9, [x9, #0x80]
    cbz w9, .Lmedicine_reject
    adrp x9, za_bag_manager
    ldr x26, [x9, :lo12:za_bag_manager]
    cbz x26, .Lmedicine_reject
    mov w0, w22
    bl za_bag_record
    lsr x0, x0, #32
    cbz w0, .Lmedicine_reject
    str w0, [x19, #64]
    uxth w1, w24
    uxth w9, w23
    cmp w1, w9
    b.eq .Lmedicine_cure
    mov x0, x21
    bl za_set_hp
    mov x0, x25
    bl za_current_hp
    uxth w9, w24
    cmp w0, w9
    b.ne .Lmedicine_reject
.Lmedicine_cure:
    eor w9, w23, w24
    lsr w9, w9, #16
    cbz w9, .Lmedicine_spend
    mov x0, x21
    mov w1, wzr
    bl za_cure_status
    mov x0, x25
    bl za_current_status
    cbnz w0, .Lmedicine_reject
.Lmedicine_spend:
    mov x0, x26
    mov w1, w22
    mov w2, #1
    bl za_bag_decrement
    mov w0, w22
    bl za_bag_record
    lsr x0, x0, #32
    ldr w9, [x19, #64]
    sub w9, w9, #1
    cmp w0, w9
    b.ne .Lmedicine_reject
    ldp x25, x26, [sp, #0x20]
    ldp x23, x24, [sp, #0x10]
    ldp x21, x22, [sp], #0x30
    b .Laccepted
.Lmedicine_reject:
    ldp x25, x26, [sp, #0x20]
    ldp x23, x24, [sp, #0x10]
    ldp x21, x22, [sp], #0x30
    b .Lreject
.Laccepted:
    mov w9, #1
    b .Lack
.Lreject:
    mov w9, wzr
.Lack:
    ldr w10, [x19, #20]
    cmp w10, #3
    b.lo .Lack_command
    str w9, [x19, #76]
    add x10, x19, #72
    stlr w20, [x10]
.Lack_command:
    str w9, [x19, #32]
    add x9, x19, #4
    stlr w20, [x9]
.Lunlock:
    add x9, x19, #12
    stlr wzr, [x9]
    ldp q6, q7, [sp, #0xc0]
    ldp q4, q5, [sp, #0xa0]
    ldp q2, q3, [sp, #0x80]
    ldp q0, q1, [sp, #0x60]
    ldp x20, x30, [sp, #0x50]
    ldp x8, x19, [sp, #0x40]
    ldp x6, x7, [sp, #0x30]
    ldp x4, x5, [sp, #0x20]
    ldp x2, x3, [sp, #0x10]
    ldp x0, x1, [sp]
    add sp, sp, #0xe0
.Lresume:
    sub sp, sp, #0x90
    b za_scene_resume

// x0 = generation key, x1 = expected type. Return current object or zero.
.Lresolve:
    cbz x0, .Lmissing
    adrp x9, za_registry
    ldr x9, [x9, :lo12:za_registry]
    cbz x9, .Lmissing
    and x10, x0, #0xfffff
    add x9, x9, x10, lsl #5
    ldr x10, [x9, #0x10]
    cmp x10, x0
    b.ne .Lmissing
    ldrh w10, [x9, #0x28]
    cbz w10, .Lmissing
    ldrh w10, [x9, #0x2a]
    cbz w10, .Lmissing
    ldr w10, [x9, #0x2c]
    cmn w10, #1
    b.eq .Lmissing
    ldr x0, [x9, #0x20]
    cmp x0, #0x28
    b.ls .Lmissing
    sub x0, x0, #0x28
    ldr x9, [x0]
    cmp x9, x1
    b.ne .Lmissing
    ldr x9, [x0, #0x10]
    cmp x9, x0
    b.ne .Lmissing
    ret
.Lmissing:
    mov x0, xzr
    ret
