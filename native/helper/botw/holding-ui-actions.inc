// Exact 1.6.0 native reservation/carry route. No stock/save writes.
// Add invokes 10C3BE0 and the holding-only 121C900..121C958 spawn flow;
// Put away invokes 10C5F50, including native carry cancellation.
// World creation can leave CarryBox in its waiting child after Link already
// owns it. On the exact new carrier only, enter the same native Normal/Grab
// children observed after Inventory Hold -> Return to game. Their enter methods
// initialize the native physics attachment; never write transforms/body flags.
// Private mailbox1104..1151 is retired before native AI calls; an epoch, UI context,
// actor-generation or ownership change cancels the finish without replay.
holding_grab_finish:
    stp x29, x30, [sp, #-32]!
    stp x20, x21, [sp, #16]
    ldr w9, [x19, #1104]
    cbz w9, holding_grab_done
    ldr w10, [x19, #4092]
    cmp w9, w10
    b.ne holding_grab_clear
    ldr w9, [x19, #4]
    ldr w10, [x19, #1108]
    sub w9, w9, w10
    cmp w9, #60
    b.hi holding_grab_clear
    cmp w9, #3
    b.lo holding_grab_done
    adrp x9, ui_root
    ldr x9, [x9, :lo12:ui_root]
    cbz x9, holding_grab_clear
    add x9, x9, #0x64, lsl #12
    ldr x9, [x9, #0xc30]
    mov x10, #0x9000000
    movk x10, #0x1e, lsl #32
    cmp x9, x10
    b.ne holding_grab_clear
    adrp x9, player_info_root
    ldr x9, [x9, :lo12:player_info_root]
    cbz x9, holding_grab_clear
    ldr x20, [x9, #0x60]
    ldr x9, [x19, #1120]
    cmp x20, x9
    b.ne holding_grab_clear
    cbz x20, holding_grab_clear
    ldr x9, [x20]
    adrp x10, player_vtable
    add x10, x10, :lo12:player_vtable
    cmp x9, x10
    b.ne holding_grab_clear
    adrp x9, carry_root
    ldr x9, [x9, :lo12:carry_root]
    cbz x9, holding_grab_clear
    ldr x10, [x19, #1144]
    cmp x9, x10
    b.ne holding_grab_clear
    ldr w10, [x9, #0xa78]
    cmp w10, #0x40
    b.ne holding_grab_done
    ldr w10, [x9, #0x308]
    cmn w10, #1
    b.eq holding_grab_clear
    ldr w11, [x19, #1136]
    cmp w10, w11
    b.ne holding_grab_clear
    ldr x9, [x9, #0x300]
    cbz x9, holding_grab_clear
    ldr w11, [x9, #0x40]
    cmp w10, w11
    b.ne holding_grab_clear
    ldr x21, [x9, #0x48]
    cbz x21, holding_grab_clear
    ldr x11, [x19, #1128]
    cmp x21, x11
    b.ne holding_grab_clear
    ldr w11, [x21, #0x60]
    cmp w10, w11
    b.ne holding_grab_clear
    ldr x9, [x20, #0x88]
    cmp x9, x21
    b.ne holding_grab_done
    ldr x9, [x21]
    adrp x10, carry_actor_vtable
    add x10, x10, :lo12:carry_actor_vtable
    cmp x9, x10
    b.ne holding_grab_clear
    ldr x9, [x21, #0x870]
    cmp x9, x21
    b.ne holding_grab_clear
    ldr x0, [x21, #0x558]
    cbz x0, holding_grab_clear
    mov w11, #3
holding_grab_ai_loop:
    ldr x9, [x0, #8]
    cmp x9, x21
    b.ne holding_grab_clear
    ldr x9, [x0]
    adrp x10, carry_ai_vtable
    add x10, x10, :lo12:carry_ai_vtable
    cmp x9, x10
    b.eq holding_grab_ai_found
    cmp w11, #3
    b.ne holding_grab_first_ai
    adrp x10, carry_root_ai_vtable
    add x10, x10, :lo12:carry_root_ai_vtable
    b holding_grab_ai_parent
holding_grab_first_ai:
    adrp x10, carry_first_ai_vtable
    add x10, x10, :lo12:carry_first_ai_vtable
holding_grab_ai_parent:
    cmp x9, x10
    b.ne holding_grab_clear
    ldrh w9, [x0, #0x1c]
    ldr w10, [x0, #0x28]
    cmp w9, w10
    b.hs holding_grab_clear
    ldr x10, [x0, #0x30]
    cbz x10, holding_grab_clear
    ldr x0, [x10, x9, lsl #3]
    cbz x0, holding_grab_clear
    subs w11, w11, #1
    b.ne holding_grab_ai_loop
    b holding_grab_clear
holding_grab_ai_found:
    ldrh w9, [x0, #0x1c]
    cmp w9, #2
    b.ne holding_grab_clear
    ldr x9, [x21, #0x190]
    cbz x9, holding_grab_done
    ldr w9, [x0, #0x28]
    cmp w9, #3
    b.ne holding_grab_clear
    ldr x9, [x0, #0x30]
    cbz x9, holding_grab_clear
    ldr x10, [x9, #16]
    cbz x10, holding_grab_clear
    ldr x10, [x10]
    adrp x11, carry_wait_vtable
    add x11, x11, :lo12:carry_wait_vtable
    cmp x10, x11
    b.ne holding_grab_clear
    ldr x9, [x9]
    cbz x9, holding_grab_clear
    ldr x10, [x9, #8]
    cmp x10, x21
    b.ne holding_grab_clear
    ldr x10, [x9]
    adrp x11, carry_grab_vtable
    add x11, x11, :lo12:carry_grab_vtable
    cmp x10, x11
    b.ne holding_grab_clear
    ldr w10, [x9, #0x28]
    cmp w10, #4
    b.ne holding_grab_clear
    mov x21, x9
    str wzr, [x19, #1104]
    mov w1, #0
    mov x2, xzr
    bl native_carry_ai_change
    mov x0, x21
    mov w1, #1
    mov x2, xzr
    bl native_carry_ai_change
    b holding_grab_done
holding_grab_clear:
    str wzr, [x19, #1104]
holding_grab_done:
    ldp x20, x21, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
holding_request:
    ldr w9, [x19, #160]
    cbnz w9, refused
    sub x11, x11, #4
    cmp x10, x11
    b.ne refused
    stp x29, x30, [sp, #-192]!
    stp x25, x26, [sp, #16]
    stp x27, x28, [sp, #32]
    str wzr, [sp, #80] // outer recursive pouch lock held
    ldr w24, [x19, #20]
    adrp x9, pouch_root
    ldr x21, [x9, :lo12:pouch_root]
    ldr x9, [x19, #32]
    cmp x21, x9
    b.ne holding_refused
    cbz x21, holding_refused
    ldr x9, [x21]
    adrp x10, pouch_vtable
    add x10, x10, :lo12:pouch_vtable
    cmp x9, x10
    b.ne holding_refused
    adrp x9, info_data_root
    ldr x9, [x9, :lo12:info_data_root]
    ldr x10, [x19, #968]
    cmp x9, x10
    b.ne holding_refused
    cbz x9, holding_refused
    ldr x9, [x9]
    adrp x10, info_data_vtable
    add x10, x10, :lo12:info_data_vtable
    cmp x9, x10
    b.ne holding_refused
    ldrb w9, [x21, #0x49]
    cmp w9, #1
    b.ne holding_refused
    adrp x9, player_info_root
    ldr x9, [x9, :lo12:player_info_root]
    ldr x10, [x19, #96]
    cmp x9, x10
    b.ne holding_refused
    cbz x9, holding_refused
    ldr x10, [x9]
    adrp x11, player_info_vtable
    add x11, x11, :lo12:player_info_vtable
    cmp x10, x11
    b.ne holding_refused
    ldr x9, [x9, #0x60]
    ldr x10, [x19, #104]
    cmp x9, x10
    b.ne holding_refused
    cbz x9, holding_refused
    ldr x10, [x9]
    adrp x11, player_vtable
    add x11, x11, :lo12:player_vtable
    cmp x10, x11
    b.ne holding_refused
    cmp w24, #11
    b.ne holding_player_ready
    // Same Player predicates as the native Inventory X/Hold entry,
    // 11B6CDC..11B6DA4; true/Player+CED bit1 refuse holding.
    mov x0, x9
    bl native_hold_blocked
    tbnz w0, #0, holding_refused
    ldr x9, [x19, #104]
    ldrb w9, [x9, #0xced]
    tbnz w9, #1, holding_refused
holding_player_ready:
    adrp x9, creator_root
    ldr x9, [x9, :lo12:creator_root]
    ldr x10, [x19, #112]
    cmp x9, x10
    b.ne holding_refused
    cbz x9, holding_refused
    ldr x10, [x9]
    adrp x11, creator_vtable
    add x11, x11, :lo12:creator_vtable
    cmp x10, x11
    b.ne holding_refused
    add x9, x9, #0xe8
    mov w10, #6
holding_creator_loop:
    ldr w11, [x9]
    cbnz w11, holding_refused
    add x9, x9, #0x68
    subs w10, w10, #1
    b.ne holding_creator_loop
    add x0, x21, #0x48
    bl native_pouch_lock
    mov w9, #1
    str w9, [sp, #80]
    bl holding_list_validate
    cbz w0, holding_refused
    mov w9, #0x44a0
    movk w9, #4, lsl #16
    add x25, x21, x9
    mov x27, #0
    mov x28, #0 // first native free slot
    str wzr, [sp, #84]
holding_slots_loop:
    add x9, x25, x27, lsl #4
    mov x10, #24
    madd x10, x27, x10, x19
    add x10, x10, #840
    ldr x0, [x9]
    ldr x11, [x10]
    cmp x0, x11
    b.ne holding_refused
    ldrh w11, [x9, #8]
    ldr w12, [x10, #16]
    cmp w11, w12
    b.ne holding_refused
    mov w12, #0xfefe
    tst w11, w12
    b.ne holding_refused
    cbnz x0, holding_occupied_slot
    cmp x28, #0
    csel x28, x9, x28, eq
    ldr x11, [x10, #8]
    cbnz x11, holding_refused
    ldr w11, [x10, #20]
    cbnz w11, holding_refused
    b holding_next_slot
holding_occupied_slot:
    cmp w24, #11
    b.ne holding_slot_ready
    ldrb w11, [x9, #8]
    cmp w11, #1
    b.ne holding_refused
holding_slot_ready:
    ldr w11, [sp, #84]
    add w11, w11, #1
    str w11, [sp, #84]
    // x0 item; x1 expected quantity/name hash; x2 expected stock flag.
    add x1, x10, #8
    ldr w2, [x10, #20]
    bl holding_item_validate
    cbz w0, holding_refused
holding_next_slot:
    add x27, x27, #1
    cmp x27, #5
    b.ne holding_slots_loop
    ldr w9, [sp, #84]
    ldr w10, [x19, #832]
    cmp w9, w10
    b.ne holding_refused
    bl holding_carry_validate
    cbz w0, holding_refused
    cmp w24, #12
    b.eq holding_put_ready
    cbz x28, holding_refused
    // Add only to a settled carried tray or an empty native carry state.
    ldr w9, [x26, #0xa78]
    tst w9, #0xffffffbf
    b.ne holding_refused
    ldr w9, [sp, #84]
    ldr x10, [x19, #976]
    cbz w9, holding_empty_carry
    cbz x10, holding_refused
    ldr x9, [x26, #0x310]
    cbz x9, holding_refused
    b holding_target
holding_empty_carry:
    cbnz x10, holding_refused
    // Native B7C83C sends INVALID generation directly to fresh CarryBox
    // creation (B7C878/B7C940), ignoring a retained +310 record. Keep its
    // raw pointer frozen by holding_carry_validate, but require idle flags.
    // Its previously freed storage is not dereferenced on this native path.
    ldr w9, [x26, #0x30c]
    cbnz w9, holding_refused
    ldr w9, [x26, #0xa78]
    cbnz w9, holding_refused
    ldr w9, [x26, #0x308]
    cmn w9, #1
    b.ne holding_refused
holding_target:
    ldr x23, [x19, #40]
    ldr w9, [x19, #76]
    cmp w9, #1
    b.lt holding_refused
    str w9, [sp, #88]
    str w9, [sp, #48]
    ldr w9, [x19, #132]
    str w9, [sp, #52]
    mov x0, x23
    add x1, sp, #48
    mov w2, #1
    bl holding_item_validate
    cbz w0, holding_refused
    ldr x9, [x23, #0x30]
    ldr x10, [x19, #80]
    cmp x9, x10
    b.ne holding_refused
    ldr w9, [x23, #0x88]
    ldr w10, [x19, #124]
    cmp w9, w10
    b.ne holding_refused
    ldr w9, [x23, #0x80]
    ldr w10, [x19, #128]
    cmp w9, w10
    b.ne holding_refused
    // Native material cache resolves by reverse list traversal.
    ldr w9, [x19, #960]
    cmp w9, #49
    b.hi holding_refused
    ldr w10, [x19, #964]
    cmp w10, #19
    b.hi holding_refused
    mov w11, #0x4230
    movk w11, #4, lsl #16
    add x11, x21, x11
    ldr x0, [x11, x9, lsl #3]
holding_cache_loop:
    bl holding_member
    cbz w9, holding_refused
    ldr w9, [x0, #0x18]
    cmp w9, #7
    b.ne holding_refused
    cbz w10, holding_cache_found
    ldr x0, [x0, #16]
    sub x0, x0, #8
    sub w10, w10, #1
    b holding_cache_loop
holding_cache_found:
    cmp x0, x23
    b.ne holding_refused
    // The generic native Hold routine has a Drop fallback: enforce CanStack.
    adrp x9, info_data_root
    ldr x27, [x9, :lo12:info_data_root]
    ldr x9, [x19, #968]
    cmp x27, x9
    b.ne holding_refused
    cbz x27, holding_refused
    ldr x9, [x27]
    adrp x10, info_data_vtable
    add x10, x10, :lo12:info_data_vtable
    cmp x9, x10
    b.ne holding_refused
    ldr x0, [x23, #0x30]
    bl native_actor_crc
    ldr w9, [x27, #0x78]
    sub w10, w9, #1
    mov w11, #32767
    cmp w10, w11
    b.hi holding_refused
    ldr x10, [x27, #0x48]
    cbz x10, holding_refused
    mov w11, #0
holding_tag_search:
    cmp w11, w9
    b.hs holding_refused
    add w12, w11, w9
    lsr w12, w12, #1
    ldr w13, [x10, x12, lsl #2]
    cmp w13, w0
    b.eq holding_tag_found
    b.hi holding_tag_lower
    add w11, w12, #1
    b holding_tag_search
holding_tag_lower:
    mov w9, w12
    b holding_tag_search
holding_tag_found:
    ldp x9, x10, [x27, #0x50]
    cbz x9, holding_refused
    cbz x10, holding_refused
    ldr w10, [x10, x12, lsl #2]
    add x10, x9, x10
    stp x9, x10, [sp, #64]
    .ifdef BOTW_190
    mov x0, x27
    .else
    ldr w0, [x27, #0x60]
    .endif
    add x1, sp, #64
    mov w2, #0x906f
    movk w2, #0x9b44, lsl #16
    bl native_has_tag
    tbz w0, #0, holding_refused
    adrp x9, actor_heap_root
    ldr x9, [x9, :lo12:actor_heap_root]
    ldr x10, [x19, #992]
    cmp x9, x10
    b.ne holding_refused
    cbz x9, holding_refused
    ldr x27, [x9, #0x28]
    ldr x10, [x19, #1000]
    cmp x27, x10
    b.ne holding_refused
    cbz x27, holding_refused
    b holding_commit
holding_put_ready:
    ldr w9, [sp, #84]
    cbz w9, holding_refused
    ldr x9, [x19, #40]
    cbnz x9, holding_refused
    ldr x9, [x19, #80]
    cbnz x9, holding_refused
holding_commit:
    // Last cancellation/epoch checks occur after all potentially slow predicates.
    add x9, x19, #72
    ldar w9, [x9]
    cmp w9, w20
    b.ne holding_refused
    add x9, x19, #4092
    ldar w9, [x9]
    ldr w10, [x19, #88]
    cmp w9, w10
    b.ne holding_refused
    ldr w9, [x19, #8]
    cmp w9, w20
    b.ne holding_refused
    ldr w9, [x19, #4]
    ldr w10, [x19, #64]
    sub w9, w9, w10
    cmp w9, #3
    b.hi holding_refused
    adrp x9, ui_root
    ldr x9, [x9, :lo12:ui_root]
    cmp x9, x22
    b.ne holding_refused
    add x9, x22, #0x64, lsl #12
    ldr x9, [x9, #0xc30]
    mov x10, #0x9000000
    movk x10, #0x1e, lsl #32
    cmp x9, x10
    b.ne holding_refused
    adrp x9, player_info_root
    ldr x9, [x9, :lo12:player_info_root]
    ldr x10, [x19, #96]
    cmp x9, x10
    b.ne holding_refused
    cbz x9, holding_refused
    ldr x9, [x9, #0x60]
    ldr x10, [x19, #104]
    cmp x9, x10
    b.ne holding_refused
    cbz x9, holding_refused
    ldr x9, [x9]
    adrp x10, player_vtable
    add x10, x10, :lo12:player_vtable
    cmp x9, x10
    b.ne holding_refused
    bl holding_carry_validate
    cbz w0, holding_refused
    cmp w24, #12
    b.eq holding_return_native
    mov x0, x21
    ldr w1, [x19, #960]
    ldr w2, [x19, #964]
    bl native_hold
    ldr w9, [x23, #0x20]
    ldr w10, [sp, #88]
    sub w10, w10, #1
    cmp w9, w10
    b.ne holding_unknown
    ldr x9, [x28]
    cmp x9, x23
    b.ne holding_unknown
    ldrb w9, [x28, #8]
    cbnz w9, holding_unknown
    // Native close spawns outside the pouch lock. Copy the validated name to
    // keep its lifetime independent of unlocked native pouch cleanup.
    ldr x9, [x23, #0x30]
    add x10, sp, #112
    mov w11, #0
holding_copy_name:
    ldr x12, [x9, x11]
    str x12, [x10, x11]
    add w11, w11, #8
    cmp w11, #64
    b.ne holding_copy_name
    add x0, x21, #0x48
    bl native_pouch_unlock
    str wzr, [sp, #80]
    add x0, sp, #112
    mov x1, x27
    .ifdef BOTW_190
    // Exact native close call: name, heap, -1, false, null, holding=true,
    // and its two forward/up offsets. No register is inherited from unlock.
    mov w2, #-1
    mov w3, wzr
    mov x4, xzr
    mov w5, #1
    mov w9, #0xcccd
    movk w9, #0x3f4c, lsl #16
    fmov s0, w9
    movk w9, #0xbf4c, lsl #16
    fmov s1, w9
    .endif
    bl native_hold_spawn
    adrp x9, pouch_root
    ldr x9, [x9, :lo12:pouch_root]
    cmp x9, x21
    b.ne holding_unknown
    add x9, x19, #4092
    ldar w9, [x9]
    ldr w10, [x19, #88]
    cmp w9, w10
    b.ne holding_unknown
    adrp x9, ui_root
    ldr x9, [x9, :lo12:ui_root]
    cmp x9, x22
    b.ne holding_unknown
    add x9, x22, #0x64, lsl #12
    ldr x9, [x9, #0xc30]
    mov x10, #0x9000000
    movk x10, #0x1e, lsl #32
    cmp x9, x10
    b.ne holding_unknown
    add x0, x21, #0x48
    bl native_pouch_lock
    mov w9, #1
    str w9, [sp, #80]
    ldr x9, [x28]
    cmp x9, x23
    b.ne holding_unknown
    ldrb w9, [x28, #8]
    cbnz w9, holding_unknown
    bl holding_list_validate
    cbz w0, holding_unknown
    ldr w9, [sp, #88]
    sub w9, w9, #1
    str w9, [sp, #48]
    mov x0, x23
    add x1, sp, #48
    bl holding_member
    cbz w9, holding_unknown
    ldrb w2, [x23, #0x25]
    bl holding_item_validate
    cbz w0, holding_unknown
    add x9, x23, #0x3c
    add x10, sp, #112
    mov w11, #0
holding_recheck_name:
    ldr x12, [x9, x11]
    ldr x13, [x10, x11]
    cmp x12, x13
    b.ne holding_unknown
    add w11, w11, #8
    cmp w11, #64
    b.ne holding_recheck_name
    // Matches the original native holding-only close branch after spawn returns.
    mov w9, #1
    strb w9, [x28, #8]
    str x26, [x19, #1144]
    ldr w9, [x26, #0x308]
    cmn w9, #1
    b.eq holding_success
    str w9, [x19, #1136]
    ldr x9, [x26, #0x300]
    cbz x9, holding_success
    ldr x9, [x9, #0x48]
    cbz x9, holding_success
    str x9, [x19, #1128]
    ldr w9, [x19, #88]
    str w9, [x19, #1104]
    ldr w9, [x19, #4]
    str w9, [x19, #1108]
    ldr x9, [x19, #104]
    str x9, [x19, #1120]
    b holding_success
holding_return_native:
    mov x0, x21
    bl native_put_away
holding_success:
    mov w27, #1
    b holding_finish
holding_unknown:
    mov w27, #3 // native call issued; never retry automatically
    b holding_finish
holding_refused:
    mov w27, #2
holding_finish:
    ldr w9, [sp, #80]
    cbz w9, holding_ack
    add x0, x21, #0x48
    bl native_pouch_unlock
holding_ack:
    str w27, [x19, #16]
    add x9, x19, #12
    stlr w20, [x9]
    ldp x25, x26, [sp, #16]
    ldp x27, x28, [sp, #32]
    ldp x29, x30, [sp], #192
    b done

// Bounded member lookup; preserves x0 and x10 (cache row).
holding_member:
    mov w9, #0
    cbz x0, holding_member_return
    tst x0, #7
    b.ne holding_member_return
    ldr w11, [x21, #0x78]
    ldr x12, [x21, #0x68]
holding_member_loop:
    sub x13, x12, #8
    cmp x13, x0
    b.eq holding_member_found
    ldr x12, [x12]
    subs w11, w11, #1
    b.ne holding_member_loop
    ret
holding_member_found:
    mov w9, #1
holding_member_return:
    ret

holding_item_validate:
    stp x29, x30, [sp, #-16]!
    bl holding_member
    cbz w9, holding_item_bad
    ldr w9, [x0, #0x18]
    cmp w9, #7
    b.ne holding_item_bad
    ldrb w9, [x0, #0x24]
    cbnz w9, holding_item_bad
    ldrb w9, [x0, #0x25]
    cmp w2, #1
    b.hi holding_item_bad
    cmp w9, w2
    b.ne holding_item_bad
    ldr w9, [x0, #0x20]
    ldr w10, [x1]
    cmp w9, w10
    b.ne holding_item_bad
    cmp w9, #0
    b.lt holding_item_bad
    ldr x9, [x0, #0x28]
    adrp x10, pouch_string_vtable
    add x10, x10, :lo12:pouch_string_vtable
    cmp x9, x10
    b.ne holding_item_bad
    ldr w9, [x0, #0x38]
    cmp w9, #64
    b.ne holding_item_bad
    ldr x9, [x0, #0x30]
    add x10, x0, #0x3c
    cmp x9, x10
    b.ne holding_item_bad
    mov w10, #0x9dc5
    movk w10, #0x811c, lsl #16
    mov w11, #0x193
    movk w11, #0x100, lsl #16
    mov w12, #0
holding_name_loop:
    ldrb w13, [x9, x12]
    cbz w13, holding_name_end
    cmp w13, #0x20
    b.lo holding_item_bad
    cmp w13, #0x7e
    b.hi holding_item_bad
    eor w10, w10, w13
    mul w10, w10, w11
    add w12, w12, #1
    cmp w12, #63
    b.ls holding_name_loop
    b holding_item_bad
holding_name_end:
    cbz w12, holding_item_bad
    ldr w9, [x1, #4]
    cmp w10, w9
    b.ne holding_item_bad
    mov w0, #1
    b holding_item_end
holding_item_bad:
    mov w0, #0
holding_item_end:
    ldp x29, x30, [sp], #16
    ret

holding_carry_validate:
    adrp x9, carry_root
    ldr x26, [x9, :lo12:carry_root]
    ldr x9, [x19, #800]
    cmp x26, x9
    b.ne holding_carry_bad
    cbz x26, holding_carry_bad
    ldr x9, [x26]
    adrp x10, carry_vtable
    add x10, x10, :lo12:carry_vtable
    cmp x9, x10
    b.ne holding_carry_bad
    ldr x9, [x26, #0x300]
    ldr x10, [x19, #808]
    cmp x9, x10
    b.ne holding_carry_bad
    ldr w11, [x26, #0x308]
    ldr w12, [x19, #824]
    cmp w11, w12
    b.ne holding_carry_bad
    ldr w12, [x26, #0x30c]
    ldr w13, [x19, #836]
    cmp w12, w13
    b.ne holding_carry_bad
    cmp w12, #1
    b.hi holding_carry_bad
    ldr w12, [x26, #0xa78]
    ldr w13, [x19, #828]
    cmp w12, w13
    b.ne holding_carry_bad
    ldr x12, [x26, #0x310]
    ldr x13, [x19, #816]
    cmp x12, x13
    b.ne holding_carry_bad
    cbz x12, holding_record_ready
    // Fresh-create branch never dereferences the retained record. Bypass its
    // type check only for fully settled idle state; active/busy guards remain.
    cmn w11, #1
    b.ne holding_record_validate
    ldr w14, [x26, #0x30c]
    cbnz w14, holding_record_validate
    ldr w14, [x26, #0xa78]
    cbz w14, holding_record_ready
holding_record_validate:
    ldr x12, [x12]
    adrp x13, carry_record_vtable
    add x13, x13, :lo12:carry_record_vtable
    cmp x12, x13
    b.ne holding_carry_bad
holding_record_ready:
    cmn w11, #1
    b.eq holding_carry_empty
    cbz x9, holding_carry_bad
    ldr w12, [x9, #0x40]
    cmp w12, w11
    b.ne holding_carry_bad
    ldr x12, [x9, #0x48]
    ldr x13, [x19, #976]
    cmp x12, x13
    b.ne holding_carry_bad
    cbz x12, holding_carry_bad
    ldr x13, [x12]
    adrp x14, carry_actor_vtable
    add x14, x14, :lo12:carry_actor_vtable
    cmp x13, x14
    b.ne holding_carry_bad
    ldr x14, [x19, #984]
    cmp x13, x14
    b.ne holding_carry_bad
    ldr w13, [x12, #0x60]
    cmp w13, w11
    b.ne holding_carry_bad
    ldr x13, [x12, #0x68]
    cmp x13, x9
    b.ne holding_carry_bad
    // Native carry link resolution B7C86C refuses BaseProc state byte 3.
    ldrb w13, [x12, #0x64]
    cmp w13, #3
    b.eq holding_carry_bad
    ldr x13, [x12, #0x1c]
    mov x14, #0x6143
    movk x14, #0x7272, lsl #16
    movk x14, #0x4279, lsl #32
    movk x14, #0x786f, lsl #48
    cmp x13, x14 // CarryBox
    b.ne holding_carry_bad
    ldrb w13, [x12, #0x24]
    cbnz w13, holding_carry_bad
    b holding_carry_ok
holding_carry_empty:
    ldr x12, [x19, #976]
    cbnz x12, holding_carry_bad
    ldr x12, [x19, #984]
    cbnz x12, holding_carry_bad
holding_carry_ok:
    mov w0, #1
    ret
holding_carry_bad:
    mov w0, #0
    ret

holding_list_validate:
    // Validate the complete canonical list before using cached/tray pointers.
    ldr w9, [x21, #0x78]
    ldr w10, [x19, #56]
    cmp w9, w10
    b.ne holding_list_bad
    sub w10, w9, #1
    cmp w10, #419
    b.hi holding_list_bad
    ldr w10, [x21, #0x7c]
    cmp w10, #8
    b.ne holding_list_bad
    add x10, x21, #0x68
    ldr x11, [x10]
    mov x12, x10
holding_list_validate_loop:
    cmp x11, x10
    b.eq holding_list_bad
    cbz x11, holding_list_bad
    tst x11, #7
    b.ne holding_list_bad
    ldr x13, [x11, #8]
    cmp x13, x12
    b.ne holding_list_bad
    mov x12, x11
    ldr x11, [x11]
    subs w9, w9, #1
    b.ne holding_list_validate_loop
    cmp x11, x10
    b.ne holding_list_bad
    ldr x11, [x10, #8]
    cmp x11, x12
    b.ne holding_list_bad
    mov w0, #1
    ret
holding_list_bad:
    mov w0, #0
    ret
