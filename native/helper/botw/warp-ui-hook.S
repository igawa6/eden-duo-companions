// Private BOTW 1.6.0 UI-thread bridge. Addresses are linked from the pinned build.
.text
.set shortcuts_enabled, 1
.set quests_enabled, 1
.set hud_enabled, 1
.ifdef BOTW_INVENTORY_ONLY
.set shortcuts_enabled, 0
.set quests_enabled, 0
.set hud_enabled, 0
.endif
.ifdef BOTW_READY
.set hud_enabled, 1
.endif
.ifdef BOTW_QUESTS
.set quests_enabled, 1
.endif
.ifdef BOTW_SHORTCUTS
.set shortcuts_enabled, 1
.endif
.set dlc_version_field, 0xd0
.set pause_shift, 0
.ifdef BOTW_190
.set dlc_version_field, 0xd8
.set pause_shift, 24
.endif
.global botw_ui_hook
botw_ui_hook:
    stp x29, x30, [sp, #-80]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    .if hud_enabled
    bl companion_map_restore
    .endif
    bl native_ui_update
    stp x0, x1, [sp, #64]
    .if hud_enabled
    bl companion_hud_pose
    bl companion_map_hide
    .endif
.global mailbox_adrp, mailbox_add
mailbox_adrp:
    adrp x19, bridge_mailbox
mailbox_add:
    add x19, x19, :lo12:bridge_mailbox
    mov w17, #0x5544
    movk w17, #0x494f, lsl #16
    str w17, [x19]
    ldr w17, [x19, #4]
    add w17, w17, #1
    str w17, [x19, #4]
    bl holding_grab_finish
    bl inventory_finish
    ldr w17, [x19, #4]
    // Acquire one immutable request, with a separate cancelable scope permit.
    add x9, x19, #8
    ldar w20, [x9]
    ldr w9, [x19, #12]
    cmp w20, w9
    b.eq done
.global pending_request
pending_request:
    add x9, x19, #4092
    ldar w9, [x9]
    cbz w9, refused
    ldr w10, [x19, #88]
    cmp w9, w10
    b.ne refused
    add x9, x19, #72
    ldar w9, [x9]
    cmp w9, w20
    b.ne refused
    ldr w9, [x19, #64]
    sub w9, w17, w9
    cmp w9, #3
    b.hi refused
    adrp x9, ui_root
    ldr x22, [x9, :lo12:ui_root]
    ldr x9, [x19, #48]
    cmp x22, x9
    b.ne refused
    cbz x22, refused
    add x9, x22, #0x64, lsl #12
    ldr x10, [x9, #0xc30]
    mov x11, #4
    movk x11, #0x900, lsl #16
    movk x11, #0x1e, lsl #32
    ldr w12, [x19, #20]
    .ifdef BOTW_INVENTORY_ONLY
    .ifdef BOTW_QUESTS
    cmp w12, #5
    b.eq quest_request
    .endif
    cmp w12, #14
    b.eq sort_request
    cmp w12, #11
    b.eq holding_request
    cmp w12, #12
    b.eq holding_request
    cmp w12, #8
    b.eq inventory_request
    cmp w12, #7
    b.eq inventory_request
    cmp w12, #6
    b.eq inventory_request
    .ifdef BOTW_WARP
    cmp w12, #13
    b.eq warp_request
    .endif
    .ifdef BOTW_MAP
    cmp w12, #9
    b.eq map_edit_request
    .endif
    .ifdef BOTW_SHORTCUTS
    cmp w12, #10
    b.eq world_rune_request
    cmp w12, #4
    b.ls shortcut_request
    .endif
    b refused
    .else
    cmp w12, #14
    b.eq sort_request
    cmp w12, #13
    b.eq warp_request
    cmp w12, #11
    b.eq holding_request
    cmp w12, #12
    b.eq holding_request
    cmp w12, #10
    b.eq world_rune_request
    cmp w12, #9
    b.eq map_edit_request
    cmp w12, #5
    b.eq quest_request
    cmp w12, #8
    b.eq inventory_request
    cmp w12, #7
    b.eq inventory_request
    cmp w12, #6
    b.eq inventory_request
    .endif
    .if shortcuts_enabled
shortcut_request:
    cmp x10, x11
    b.ne refused
    ldr w10, [x9, #0xc24]
    cmp w10, #3
    b.ne refused
    ldr w10, [x9, #0xc58]
    mov w11, #0xd0000
    and w10, w10, w11
    ldr w11, [x19, #20]
    cmp w11, #4
    b.eq rune_direction
    cmp w11, #3
    b.hi refused
    and w14, w11, #1
    cmp w14, #1
    mov w12, #0x40000
    mov w13, #0x80000
    csel w12, w12, w13, eq
    b direction_check
rune_direction:
    mov w12, #0x10000
direction_check:
    cmp w10, w12
    b.ne refused
    adrp x9, screen_root
    ldr x9, [x9, :lo12:screen_root]
    cbz x9, refused
    ldr w10, [x9, #0x28]
    cmp w10, #99
    b.ne refused
    ldr x9, [x9, #0x30]
    cbz x9, refused
    ldr x21, [x9, #360]
    ldr x9, [x19, #32]
    cmp x21, x9
    b.ne refused
    cbz x21, refused
    ldr x9, [x21]
    adrp x10, screen_vtable
    add x10, x10, :lo12:screen_vtable
    cmp x9, x10
    b.ne refused
    ldr x9, [x21, #(0x150+pause_shift)]
    .ifdef BOTW_190
    adrp x10, menu_state
    add x10, x10, :lo12:menu_state
    .else
    sub x10, x10, #0x130
    .endif
    cmp x9, x10
    b.ne refused
    ldr w9, [x21, #(0x3720+pause_shift)]
    cmp w9, w11
    b.ne refused
    ldr w9, [x21, #(0x3724+pause_shift)]
    cmp w9, w11
    b.ne refused
    mov w9, #(0x38cc+pause_shift)
    ldrb w9, [x21, x9]
    cmp w9, #3
    b.ne refused
    ldr w9, [x21, #(0x3868+pause_shift)]
    ldr w10, [x19, #56]
    cmp w9, w10
    b.ne refused
    cmp w11, #4
    mov w10, #8
    mov w12, #20
    csel w10, w10, w12, eq
    cmp w11, #3
    mov w12, #6
    csel w10, w12, w10, eq
    cmp w9, w10
    b.hi refused
    ldr w24, [x19, #24]
    cmn w24, #1
    b.eq cursor_empty_index
    cmp w24, w9
    b.hs refused
    b cursor_index_valid
cursor_empty_index:
    cmp w11, #2
    b.hi refused
    ldr x9, [x19, #40]
    cbnz x9, refused
    ldr x9, [x19, #80]
    cbnz x9, refused
cursor_index_valid:
    ldr w9, [x21, #(0x3860+pause_shift)]
    ldr w10, [x19, #28]
    cmp w9, w10
    b.ne refused
    ldr w9, [x21, #(0x3864+pause_shift)]
    ldr w10, [x19, #60]
    cmp w9, w10
    b.ne refused
    cmp w11, #4
    b.eq rune_identity
    cmn w24, #1
    b.eq commit
    mov w9, #(0x37b0+pause_shift)
    add x9, x21, x9
    ldr x23, [x9, x24, lsl #3]
    ldr x9, [x19, #40]
    cmp x23, x9
    b.ne refused
    cbz x23, refused
    tst x23, #7
    b.ne refused
    ldr w9, [x23, #0x18]
    cmp w11, #1
    mov w10, #3
    csel w10, w10, wzr, eq
    cmp w11, #2
    mov w12, #1
    csel w10, w12, w10, eq
    cmp w11, #3
    mov w12, #2
    csel w10, w12, w10, eq
    cmp w9, w10
    b.ne refused
    ldrb w9, [x23, #0x25]
    cmp w9, #1
    b.ne refused
    ldr w9, [x23, #0x20]
    ldr w10, [x19, #76]
    cmp w9, w10
    b.ne refused
    ldr x9, [x23, #0x30]
    ldr x10, [x19, #80]
    cmp x9, x10
    b.ne refused
    b commit
rune_identity:
    ldr w9, [x21, #(0x3850+pause_shift)]
    cmp w9, #8
    b.ne refused
    ldr x9, [x21, #(0x3858+pause_shift)]
    cbz x9, refused
    ldr w9, [x9, x24, lsl #2]
    ldr w10, [x19, #40]
    cmp w9, w10
    b.ne refused
commit:
    add x9, x19, #8
    ldar w9, [x9]
    cmp w9, w20
    b.ne refused
    add x9, x19, #72
    ldar w9, [x9]
    cmp w9, w20
    b.ne refused
    add x9, x19, #4092
    ldar w9, [x9]
    ldr w10, [x19, #88]
    cmp w9, w10
    b.ne refused
    ldr w9, [x19, #20]
    cmp w9, #5
    b.eq quest_commit
    // Screen+3790 is the native shortcut controller (1191F94). Resolve the
    // visible cell through the same 386C map used by native R-stick updates,
    // including the optional -1 unequip cell. Never assume cell==pouch index.
    ldr x23, [x21, #(0x3790+pause_shift)]
    cbz x23, refused
    tst x23, #7
    b.ne refused
    ldr x9, [x23]
    adrp x10, shortcut_controller_vtable
    add x10, x10, :lo12:shortcut_controller_vtable
    cmp x9, x10
    b.ne refused
    ldrb w9, [x23, #0x2c]
    tbz w9, #0, refused
    ldr w12, [x23, #0x2ac]
    sub w9, w12, #1
    cmp w9, #20
    b.hi refused
    ldr w9, [x23, #0x2dc]
    cmp w9, w12
    b.hs refused
    mov w10, #(0x386c+pause_shift)
    add x10, x21, x10
    ldr w9, [x10, x9, lsl #2]
    ldr w11, [x19, #60]
    cmp w9, w11
    b.ne refused
    mov w13, #-1
    mov w14, #0
cursor_find_cell:
    ldr w9, [x10, x14, lsl #2]
    cmp w9, w24
    b.ne cursor_next_cell
    cmn w13, #1
    b.ne refused
    mov w13, w14
cursor_next_cell:
    add w14, w14, #1
    cmp w14, w12
    b.lo cursor_find_cell
    cmn w13, #1
    b.eq refused
    // Use the game's own flat→group decomposition, then its native setter.
    ldr w14, [x23, #0x288]
    sub w9, w14, #1
    cmp w9, #20
    b.hi refused
    ldr w9, [x23, #0x2a8]
    cmp w9, w14
    b.ne refused
    ldr x10, [x23, #0x290]
    cbz x10, refused
    tst x10, #7
    b.ne refused
    mov w1, w13
    mov w2, #0
cursor_find_group:
    ldr x11, [x10, x2, lsl #3]
    cbz x11, refused
    tst x11, #7
    b.ne refused
    ldr w9, [x11, #0x2f4]
    sub w15, w9, #1
    cmp w15, #20
    b.hi refused
    cmp w1, w9
    b.lo cursor_group_found
    sub w1, w1, w9
    add w2, w2, #1
    cmp w2, w14
    b.lo cursor_find_group
    b refused
cursor_group_found:
    ldr w9, [x11, #0x2f0]
    cmp w9, w2
    b.ne refused
    // Recheck the cancelable permit immediately before the first native call.
    add x9, x19, #72
    ldar w9, [x9]
    cmp w9, w20
    b.ne refused
    mov x0, x23
    mov w3, #1
    bl native_cursor_set
    tbz w0, #0, refused
    str w24, [x21, #(0x3864+pause_shift)]
    mov x0, x21
    bl native_cursor_name
    // Native shortcut callback derives/applies horizontal pane scrolling from
    // the controller cursor. No companion writes to pane/scroll coordinates.
    mov x0, x23
    bl native_cursor_scroll
    b successful
    .endif
    .if quests_enabled
    .include "quest-ui-actions.inc"
    .endif
successful:
    mov w9, #1
    b acknowledge
refused:
    mov w9, #2
acknowledge:
    str w9, [x19, #16]
    add x9, x19, #12
    stlr w20, [x9]
done:
    ldp x0, x1, [sp, #64]
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #80
    ret

// Equipment requests avoid hidden menu state; consumption temporarily supplies
// a validated native cursor. The native pouch lock
// protects membership/identity and native autoEquip performs cache/save work.
inventory_request:
    ldr w9, [x19, #160]
    cbnz w9, refused
    sub x11, x11, #4
    cmp x10, x11
    b.ne refused
    adrp x9, pouch_root
    ldr x21, [x9, :lo12:pouch_root]
    ldr x9, [x19, #32]
    cmp x21, x9
    b.ne refused
    cbz x21, refused
    ldr x9, [x21]
    adrp x10, pouch_vtable
    add x10, x10, :lo12:pouch_vtable
    cmp x9, x10
    b.ne refused
    adrp x9, player_info_root
    ldr x9, [x9, :lo12:player_info_root]
    ldr x10, [x19, #96]
    cmp x9, x10
    b.ne refused
    cbz x9, refused
    ldr x10, [x9]
    adrp x11, player_info_vtable
    add x11, x11, :lo12:player_info_vtable
    cmp x10, x11
    b.ne refused
    ldr x9, [x9, #0x60]
    ldr x10, [x19, #104]
    cmp x9, x10
    b.ne refused
    cbz x9, refused
    ldr x10, [x9]
    adrp x11, player_vtable
    add x11, x11, :lo12:player_vtable
    cmp x10, x11
    b.ne refused
    ldr w24, [x19, #24]
    ldr w10, [x19, #20]
    cmp w10, #8
    b.ne inventory_gear_slot
    cmp w24, #7
    b.ne refused
    b inventory_empty_slot
inventory_gear_slot:
    cmp w24, #7
    b.hs refused
    cmp w24, #6
    b.eq inventory_empty_slot
    sub w10, w24, #3
    cmp w10, #2
    b.ls inventory_armor_link
    cmp w24, #2
    b.hi refused
    add x10, x9, #0xb90
    ldr x11, [x10]
    adrp x12, player_gear_vtable
    add x12, x12, :lo12:player_gear_vtable
    cmp x11, x12
    b.ne refused
    mov w11, #24
    madd x10, x24, x11, x10
    b inventory_link_ready
inventory_armor_link:
    add x10, x9, #0x2000
    ldr x11, [x10, #0x3e0]
    adrp x12, player_armor_vtable
    add x12, x12, :lo12:player_armor_vtable
    cmp x11, x12
    b.ne refused
    add x10, x10, #0x3e8
    sub w11, w24, #3
    add x10, x10, x11, lsl #4
inventory_link_ready:
    ldr w11, [x10, #16]
    ldr w12, [x19, #120]
    cmp w11, w12
    b.ne refused
    cmn w11, #1
    b.eq inventory_empty_slot
    ldr x10, [x10, #8]
    ldr x11, [x19, #144]
    cmp x10, x11
    b.ne refused
    cbz x10, refused
    ldr x11, [x10, #0x48]
    ldr x12, [x19, #136]
    cmp x11, x12
    b.ne refused
inventory_empty_slot:
    adrp x9, creator_root
    ldr x9, [x9, :lo12:creator_root]
    ldr x10, [x19, #112]
    cmp x9, x10
    b.ne refused
    cbz x9, refused
    ldr x10, [x9]
    adrp x11, creator_vtable
    add x11, x11, :lo12:creator_vtable
    cmp x10, x11
    b.ne refused
    add x9, x9, #0xe8
    mov w10, #6
inventory_idle_loop:
    ldr w11, [x9]
    cbnz w11, refused
    add x9, x9, #0x68
    subs w10, w10, #1
    b.ne inventory_idle_loop
    add x0, x21, #0x48
    bl native_pouch_lock
    // Everything below exits through inventory_unlock on refusal.
    ldr w9, [x21, #0x78]
    ldr w10, [x19, #56]
    cmp w9, w10
    b.ne inventory_unlock
    sub w10, w9, #1
    cmp w10, #419
    b.hi inventory_unlock
    ldr w10, [x21, #0x7c]
    cmp w10, #8
    b.ne inventory_unlock
    ldr x23, [x19, #40]
    cbz x23, inventory_unlock
    tst x23, #7
    b.ne inventory_unlock
    add x10, x21, #0x68
    ldr x11, [x10]
    mov x12, x10
    mov w13, wzr
inventory_list_loop:
    cmp x11, x10
    b.eq inventory_unlock
    cbz x11, inventory_unlock
    tst x11, #7
    b.ne inventory_unlock
    ldr x14, [x11, #8]
    cmp x14, x12
    b.ne inventory_unlock
    sub x14, x11, #8
    ldr w15, [x14, #0x18]
    cmp w15, #9
    b.hi inventory_unlock
    cmp x14, x23
    cinc w13, w13, eq
    mov x12, x11
    ldr x11, [x11]
    subs w9, w9, #1
    b.ne inventory_list_loop
    cmp x11, x10
    b.ne inventory_unlock
    ldr x11, [x10, #8]
    cmp x11, x12
    b.ne inventory_unlock
    cmp w13, #1
    b.ne inventory_unlock
    ldr w9, [x23, #0x18]
    ldr w10, [x19, #28]
    cmp w9, w10
    b.ne inventory_unlock
    ldr w10, [x19, #20]
    cmp w10, #8
    b.ne inventory_gear_type
    sub w10, w9, #7
    cmp w10, #1
    b.hi inventory_unlock
    b inventory_type_ready
inventory_gear_type:
    cmp w24, #0
    mov w10, #3
    csel w10, wzr, w10, eq
    cmp w24, #2
    mov w11, #1
    csel w10, w11, w10, eq
    cmp w24, #3
    add w11, w24, #1
    csel w10, w11, w10, hs
    cmp w24, #6
    mov w11, #2
    csel w10, w11, w10, eq
    cmp w9, w10
    b.ne inventory_unlock
inventory_type_ready:
    ldrb w9, [x23, #0x25]
    cmp w9, #1
    b.ne inventory_unlock
    ldrb w9, [x23, #0x24]
    ldr w10, [x19, #20]
    cmp w10, #7
    cset w10, eq
    cmp w9, w10
    b.ne inventory_unlock
    ldr w9, [x23, #0x20]
    ldr w10, [x19, #76]
    cmp w9, w10
    b.ne inventory_unlock
    cmp w24, #7
    b.eq inventory_arrow_value
    cmp w24, #6
    b.eq inventory_arrow_value
    cmp w24, #3
    b.hs inventory_dye
inventory_arrow_value:
    cmp w9, #0
    b.le inventory_unlock
    b inventory_value_valid
inventory_dye:
    add w10, w9, #1
    cmp w10, #16
    b.hi inventory_unlock
inventory_value_valid:
    ldr w9, [x23, #0x88]
    ldr w10, [x19, #124]
    cmp w9, w10
    b.ne inventory_unlock
    ldr w9, [x23, #0x80]
    ldr w10, [x19, #128]
    cmp w9, w10
    b.ne inventory_unlock
    ldr x9, [x23, #0x28]
    adrp x10, pouch_string_vtable
    add x10, x10, :lo12:pouch_string_vtable
    cmp x9, x10
    b.ne inventory_unlock
    ldr w9, [x23, #0x38]
    cmp w9, #64
    b.ne inventory_unlock
    ldr x9, [x23, #0x30]
    ldr x10, [x19, #80]
    cmp x9, x10
    b.ne inventory_unlock
    add x10, x23, #0x3c
    cmp x9, x10
    b.ne inventory_unlock
    mov w10, #0x9dc5
    movk w10, #0x811c, lsl #16
    mov w11, #0x193
    movk w11, #0x100, lsl #16
    mov w12, #64
inventory_name_loop:
    ldrb w13, [x9], #1
    cbz w13, inventory_name_done
    eor w10, w10, w13
    mul w10, w10, w11
    subs w12, w12, #1
    b.ne inventory_name_loop
    b inventory_unlock
inventory_name_done:
    cmp w12, #64
    b.eq inventory_unlock
    ldr w9, [x19, #132]
    cmp w10, w9
    b.ne inventory_unlock
    ldr w9, [x19, #20]
    cmp w9, #8
    b.ne inventory_remove_check
    bl consume_validate
    cbz w0, inventory_unlock
    b inventory_pair_ready
inventory_remove_check:
    cmp w9, #7
    b.ne inventory_equip_pair
    bl inventory_remove_location
    cbz w0, inventory_unlock
    b inventory_equip_pair
inventory_equip_pair:
    cmp w24, #3
    b.lo inventory_pair_ready
    cmp w24, #4
    b.hi inventory_pair_ready
    bl inventory_armor_fit
    cbz w0, inventory_unlock
inventory_pair_ready:
    // Recheck scope at the last point before native mutation.
.global inventory_validate_commit
inventory_validate_commit:
    add x9, x19, #8
    ldar w9, [x9]
    cmp w9, w20
    b.ne inventory_unlock
    add x9, x19, #72
    ldar w9, [x9]
    cmp w9, w20
    b.ne inventory_unlock
    add x9, x19, #4092
    ldar w9, [x9]
    ldr w10, [x19, #88]
    cmp w9, w10
    b.ne inventory_unlock
    add x9, x22, #0x64, lsl #12
    ldr x9, [x9, #0xc30]
    mov x10, #0x9000000
    movk x10, #0x1e, lsl #32
    cmp x9, x10
    b.ne inventory_unlock
.global inventory_commit
inventory_commit:
    ldr w9, [x19, #20]
    cmp w9, #8
    b.eq consume_commit
    cmp w9, #7
    b.eq inventory_remove_commit
    mov x0, x21
    mov x1, x23
    add x2, x21, #0x68
    bl native_auto_equip
    add x0, x21, #0x48
    bl native_pouch_unlock
    cmp w24, #6
    b.eq successful
    adrp x9, creator_root
    ldr x0, [x9, :lo12:creator_root]
    mov w1, w24
    add x2, x23, #0x28
    ldr w3, [x23, #0x20]
    cmp w24, #3
    b.hs inventory_create_armor
    sub sp, sp, #16
    ldr w9, [x23, #0x88]
    ldr w10, [x23, #0x80]
    stp w9, w10, [sp]
    mov x4, sp
    bl native_create_weapon
    add sp, sp, #16
    b inventory_creation_requested
inventory_create_armor:
    cmp w24, #3
    add x9, x19, #512
    csel x2, x9, x2, eq
    bl native_create_armor
    cmp w24, #4
    b.ne inventory_creation_requested
    ldr w9, [x19, #252]
    cbz w9, inventory_creation_requested
    adrp x9, creator_root
    ldr x0, [x9, :lo12:creator_root]
    mov w1, #3
    add x2, x19, #512
    ldr w3, [x19, #264]
    bl native_create_armor
inventory_creation_requested:
    ldr w9, [x19, #20]
    cmp w9, #7
    cset w10, eq
    str w10, [x19, #328]
    ldr x9, [x19, #104]
    str x9, [x19, #168]
    ldr x9, [x19, #112]
    str x9, [x19, #176]
    ldr w9, [x19, #132]
    cmp w24, #3
    ldr w10, [x19, #244]
    csel w9, w10, w9, eq
    ldr w10, [x19, #328]
    ldr w11, [x19, #320]
    cmp w10, #0
    csel w9, w11, w9, ne
    str w9, [x19, #184]
    str wzr, [x19, #216]
    cmp w24, #4
    b.ne inventory_job_pair_ready
    ldr w9, [x19, #252]
    str w9, [x19, #216]
    ldr w9, [x19, #244]
    str w9, [x19, #220]
inventory_job_pair_ready:
    ldr x9, [x19, #48]
    str x9, [x19, #200]
    str x23, [x19, #192]
    str x21, [x19, #208]
    str w24, [x19, #164]
    str w20, [x19, #160]
    b successful
inventory_unlock:
    add x0, x21, #0x48
    bl native_pouch_unlock
    b refused

// Resolve the native column/row under the same recursive pouch lock.
// Every dereferenced cached item must belong to the fully validated list.
inventory_remove_location:
    cmp w24, #7
    b.eq inventory_remove_slot_ready
    cmp w24, #5
    b.hi inventory_remove_invalid
inventory_remove_slot_ready:
    ldrb w9, [x21, #0x49]
    cmp w9, #1
    b.ne inventory_remove_invalid
    ldr w9, [x19, #304]
    cmp w9, #49
    b.hi inventory_remove_invalid
    ldr w10, [x19, #308]
    cmp w10, #19
    b.hi inventory_remove_invalid
    mov w11, #0x4230
    movk w11, #4, lsl #16
    add x11, x21, x11
    ldr x11, [x11, x9, lsl #3]
    add x12, x21, #0x68
inventory_remove_row:
    ldr x13, [x12]
    ldr w14, [x21, #0x78]
inventory_remove_member:
    sub x15, x13, #8
    cmp x15, x11
    b.eq inventory_remove_found
    ldr x13, [x13]
    subs w14, w14, #1
    b.ne inventory_remove_member
    b inventory_remove_invalid
inventory_remove_found:
    ldr w13, [x11, #0x18]
    ldr w14, [x19, #28]
    cmp w14, #7
    b.hs inventory_remove_exact_group
    cmp w14, #4
    b.hs inventory_remove_armor_group
inventory_remove_exact_group:
    cmp w13, w14
    b.ne inventory_remove_invalid
    b inventory_remove_group_ready
inventory_remove_armor_group:
    sub w13, w13, #4
    cmp w13, #2
    b.hi inventory_remove_invalid
inventory_remove_group_ready:
    cbz w10, inventory_remove_target
    ldr x11, [x11, #16]
    sub x11, x11, #8
    sub w10, w10, #1
    b inventory_remove_row
inventory_remove_target:
    cmp x11, x23
    b.ne inventory_remove_invalid
    cmp w24, #7
    b.eq consume_location_valid
    cmp w24, #3
    b.eq inventory_remove_head
    cmp w24, #4
    b.eq inventory_remove_upper
    cmp w24, #5
    b.eq inventory_remove_lower
    cmp w24, #0
    b.eq inventory_remove_sword
    cmp w24, #1
    b.eq inventory_remove_shield
    adrp x9, default_bow
    add x9, x9, :lo12:default_bow
    adrp x11, default_bow_name
    add x11, x11, :lo12:default_bow_name
    b inventory_remove_default
inventory_remove_sword:
    adrp x9, default_sword
    add x9, x9, :lo12:default_sword
    adrp x11, default_sword_name
    add x11, x11, :lo12:default_sword_name
    b inventory_remove_default
inventory_remove_shield:
    adrp x9, default_shield
    add x9, x9, :lo12:default_shield
    adrp x11, default_shield_name
    add x11, x11, :lo12:default_shield_name
    b inventory_remove_default
inventory_remove_head:
    adrp x9, default_head
    add x9, x9, :lo12:default_head
    adrp x11, default_head_name
    add x11, x11, :lo12:default_head_name
    b inventory_remove_default
inventory_remove_upper:
    adrp x9, default_upper
    add x9, x9, :lo12:default_upper
    adrp x11, default_upper_name
    add x11, x11, :lo12:default_upper_name
    b inventory_remove_default
inventory_remove_lower:
    adrp x9, default_lower
    add x9, x9, :lo12:default_lower
    adrp x11, default_lower_name
    add x11, x11, :lo12:default_lower_name
inventory_remove_default:
    ldr x10, [x9]
    adrp x12, native_string_vtable
    add x12, x12, :lo12:native_string_vtable
    cmp x10, x12
    b.ne inventory_remove_invalid
    ldr x10, [x9, #8]
    cmp x10, x11
    b.ne inventory_remove_invalid
    str x9, [x19, #312]
    mov w0, #1
    ret
inventory_remove_invalid:
    mov w0, wzr
    ret
inventory_remove_commit:
    mov x0, x21
    ldr w1, [x19, #304]
    ldr w2, [x19, #308]
    bl native_remove_item
    add x0, x21, #0x48
    bl native_pouch_unlock
    cmp w24, #3
    b.hs inventory_remove_create_armor
    ldr x0, [x19, #104]
    ldr x1, [x19, #312]
    mov w2, #30
    bl native_switch_default
    b successful
inventory_remove_create_armor:
    adrp x9, creator_root
    ldr x0, [x9, :lo12:creator_root]
    mov w1, w24
    ldr x2, [x19, #312]
    mov w3, #-1
    b inventory_create_armor

// Exact Food/Material row validation reuses the bounded native cache traversal.
consume_location_valid:
    mov w0, #1
    ret
consume_validate:
    str x30, [sp, #-16]!
    add x9, x19, #416
    ldar w9, [x9]
    cbnz w9, consume_invalid
    bl inventory_remove_location
    cbz w0, consume_invalid
    ldr w9, [x23, #0x20]
    mov w10, #0x423f
    movk w10, #0xf, lsl #16
    cmp w9, w10
    b.hi consume_invalid
    // Full native Eat consumes these exact immutable metadata bytes.
    ldr w9, [x23, #0x84]
    ldr w10, [x19, #336]
    cmp w9, w10
    b.ne consume_invalid
    ldp w9, w11, [x23, #0x8c]
    ldr w10, [x19, #340]
    ldr w12, [x19, #344]
    cmp w9, w10
    b.ne consume_invalid
    cmp w11, w12
    b.ne consume_invalid
    ldr w9, [x23, #0x1c]
    ldr w10, [x19, #348]
    cmp w9, w10
    b.ne consume_invalid
    cmp w9, #10
    b.eq consume_use_ready
    cmp w9, #8
    b.ne consume_invalid
    ldr w9, [x23, #0x18]
    cmp w9, #8
    b.ne consume_invalid
consume_use_ready:
    adrp x9, eat_player_root
    ldr x9, [x9, :lo12:eat_player_root]
    cbz x9, consume_invalid
    ldr x10, [x19, #384]
    cmp x9, x10
    b.ne consume_invalid
    ldr x10, [x9]
    adrp x11, eat_root_vtable
    add x11, x11, :lo12:eat_root_vtable
    cmp x10, x11
    b.ne consume_invalid
    ldr x9, [x9, #0x48]
    ldr x10, [x19, #392]
    cmp x9, x10
    b.ne consume_invalid
    cbz x9, consume_invalid
    ldr x10, [x9]
    adrp x11, pause_player_vtable
    add x11, x11, :lo12:pause_player_vtable
    cmp x10, x11
    b.ne consume_invalid
    ldr x10, [x19, #104]
    ldr w9, [x10, #0x848]
    cmp w9, #1
    b.lt consume_invalid
    cmp w9, #120
    b.hi consume_invalid
    adrp x9, screen_root
    ldr x9, [x9, :lo12:screen_root]
    cbz x9, consume_invalid
    ldr w10, [x9, #0x28]
    cmp w10, #99
    b.ne consume_invalid
    ldr x9, [x9, #0x30]
    cbz x9, consume_invalid
    ldr x9, [x9, #368]
    ldr x10, [x19, #368]
    cmp x9, x10
    b.ne consume_invalid
    cbz x9, consume_invalid
    ldr x10, [x9]
    adrp x11, inventory_screen_vtable
    add x11, x11, :lo12:inventory_screen_vtable
    cmp x10, x11
    b.ne consume_invalid
    ldr x10, [x9, #(0x3610+pause_shift)]
    cmp x10, x21
    b.ne consume_invalid
    ldr x10, [x9, #(0x3630+pause_shift)]
    cbnz x10, consume_invalid
    ldr w10, [x9, #(0x3638+pause_shift)]
    cbnz w10, consume_invalid
    // The next native Eat column must start strictly beyond index 29.
    ldr x10, [x9, #(0x3a10+pause_shift)]
    cbz x10, consume_invalid
    ldr x11, [x10]
    adrp x12, inventory_column_vtable
    add x12, x12, :lo12:inventory_column_vtable
    cmp x11, x12
    b.ne consume_invalid
    ldr w10, [x10, #0x134]
    cmp w10, #29
    b.ls consume_invalid
    ldr x9, [x9, #(0x3a08+pause_shift)]
    ldr x10, [x19, #376]
    cmp x9, x10
    b.ne consume_invalid
    cbz x9, consume_invalid
    ldr x10, [x9]
    adrp x11, inventory_column_vtable
    add x11, x11, :lo12:inventory_column_vtable
    cmp x10, x11
    b.ne consume_invalid
    ldr w10, [x9, #0x134]
    cmp w10, #29
    b.ne consume_invalid
    ldr w10, [x9, #0xcd0]
    cmp w10, #20
    b.ne consume_invalid
    ldr x10, [x9, #0xcd8]
    cbz x10, consume_invalid
    ldr x10, [x9, #0x140]
    cbz x10, consume_invalid
    ldr x10, [x10, #0x20]
    cbz x10, consume_invalid
    mov w0, #1
    ldr x30, [sp], #16
    ret
consume_invalid:
    mov w0, wzr
    ldr x30, [sp], #16
    ret
consume_commit:
    // This runs on the native UI thread; restore all temporary cursor fields.
    ldr x0, [x19, #368]
    ldr x9, [x19, #376]
    ldr w10, [x9, #0x130]
    str w10, [x19, #400]
    ldr w10, [x9, #0xc78]
    str w10, [x19, #404]
    ldr w10, [x19, #304]
    str w10, [x9, #0x130]
    ldr w10, [x19, #308]
    str w10, [x9, #0xc78]
    str x23, [x0, #(0x3630+pause_shift)]
    mov w10, #29
    str w10, [x0, #(0x3638+pause_shift)]
    bl native_eat
    ldr x9, [x19, #368]
    str xzr, [x9, #(0x3630+pause_shift)]
    str wzr, [x9, #(0x3638+pause_shift)]
    ldr x9, [x19, #376]
    ldr w10, [x19, #400]
    str w10, [x9, #0x130]
    ldr w10, [x19, #404]
    str w10, [x9, #0xc78]
    ldr w9, [x19, #76]
    sub w9, w9, #1
    ldr w10, [x23, #0x20]
    cmp w9, w10
    b.ne consume_not_confirmed
    cmp w9, #0
    cset w9, gt
    ldrb w10, [x23, #0x25]
    cmp w9, w10
    b.ne consume_not_confirmed
    add x0, x21, #0x48
    bl native_pouch_unlock
    adrp x9, eat_player_root
    ldr x0, [x9, :lo12:eat_player_root]
    add x1, x23, #0x3c
    bl native_eat_notify
    // Publish committed work only after native Eat and cursor restoration.
    ldr x9, [x19, #104]
    str x9, [x19, #424]
    ldr w10, [x9, #0x60]
    str w10, [x19, #456]
    str wzr, [x19, #420]
    add x9, x19, #416
    stlr w20, [x9]
    b successful
consume_not_confirmed:
    add x0, x21, #0x48
    bl native_pouch_unlock
    mov w9, #3
    str w9, [x19, #16]
    add x9, x19, #12
    stlr w20, [x9]
    b done

// Finish an already committed native creation request on the player calc
// thread. Native event actions wait for B7FB90, then OR C98 bit5 under C78;
// normal player calc consumes that request through virtual B50. Never call
// the actor's equipment/animation update directly on the UI thread.
inventory_finish:
    stp x21, x30, [sp, #-32]!
    stp x23, x24, [sp, #16]
    ldr w9, [x19, #160]
    cbz w9, inventory_finish_return
    adrp x9, ui_root
    ldr x9, [x9, :lo12:ui_root]
    ldr x10, [x19, #200]
    cmp x9, x10
    b.ne inventory_finish_clear
    cbz x9, inventory_finish_clear
    add x9, x9, #0x64, lsl #12
    ldr x9, [x9, #0xc30]
    mov x10, #0x9000000
    movk x10, #0x1e, lsl #32
    cmp x9, x10
    b.ne inventory_finish_return
    adrp x9, player_info_root
    ldr x9, [x9, :lo12:player_info_root]
    cbz x9, inventory_finish_clear
    ldr x21, [x9, #0x60]
    ldr x10, [x19, #168]
    cmp x21, x10
    b.ne inventory_finish_clear
    cbz x21, inventory_finish_clear
    ldr x9, [x21]
    adrp x10, player_vtable
    add x10, x10, :lo12:player_vtable
    cmp x9, x10
    b.ne inventory_finish_clear
    adrp x9, creator_root
    ldr x23, [x9, :lo12:creator_root]
    ldr x10, [x19, #176]
    cmp x23, x10
    b.ne inventory_finish_clear
    cbz x23, inventory_finish_clear
    ldr x9, [x23]
    adrp x10, creator_vtable
    add x10, x10, :lo12:creator_vtable
    cmp x9, x10
    b.ne inventory_finish_clear
    adrp x9, pouch_root
    ldr x9, [x9, :lo12:pouch_root]
    ldr x10, [x19, #208]
    cmp x9, x10
    b.ne inventory_finish_clear
    ldr x9, [x19, #192]
    cbz x9, inventory_finish_clear
    ldrb w10, [x9, #0x24]
    ldr w11, [x19, #328]
    cmp w11, #0
    cset w11, eq
    cmp w10, w11
    b.ne inventory_finish_clear
    ldrb w10, [x9, #0x25]
    cmp w10, #1
    b.ne inventory_finish_clear
    ldr w24, [x19, #164]
    cmp w24, #5
    b.hi inventory_finish_clear
    mov w9, #0x68
    madd x9, x24, x9, x23
    ldr w10, [x9, #0xe8]
    cbz w10, inventory_finish_clear
    cmp w10, #2
    b.ne inventory_finish_return
    ldr x9, [x9, #0x98]
    cbz x9, inventory_finish_clear
    mov w10, #0x9dc5
    movk w10, #0x811c, lsl #16
    mov w11, #0x193
    movk w11, #0x100, lsl #16
    mov w12, #64
inventory_finish_name_loop:
    ldrb w13, [x9], #1
    cbz w13, inventory_finish_name_done
    eor w10, w10, w13
    mul w10, w10, w11
    subs w12, w12, #1
    b.ne inventory_finish_name_loop
    b inventory_finish_clear
inventory_finish_name_done:
    ldr w9, [x19, #184]
    cmp w9, w10
    b.ne inventory_finish_clear
    ldr w9, [x19, #216]
    cbz w9, inventory_finish_pair_ready
    ldr w9, [x23, #0x220]
    cbz w9, inventory_finish_clear
    cmp w9, #2
    b.ne inventory_finish_return
    ldr x0, [x23, #0x1d0]
    cbz x0, inventory_finish_clear
    bl inventory_hash_name
    ldr w9, [x19, #220]
    cmp w0, w9
    b.ne inventory_finish_clear
inventory_finish_pair_ready:
    mov x0, x23
    bl native_equipment_ready
    tbz w0, #0, inventory_finish_return
    add x0, x21, #0xc78
    bl native_pouch_lock
    ldr w9, [x21, #0xc98]
    orr w9, w9, #0x20
    str w9, [x21, #0xc98]
    add x0, x21, #0xc78
    bl native_pouch_unlock
inventory_finish_clear:
    str wzr, [x19, #160]
inventory_finish_return:
    ldp x23, x24, [sp, #16]
    ldp x21, x30, [sp], #32
    ret

// Head/upper fitting: validate the old equipped pair under the pouch lock,
// query the same native ActorInfo properties as Inventory close, then prepare
// the exact head name (including _B) before any pouch/actor mutation.
inventory_armor_fit:
    stp x25, x26, [sp, #-64]!
    stp x27, x28, [sp, #16]
    str x30, [sp, #32]
    adrp x9, info_data_root
    ldr x27, [x9, :lo12:info_data_root]
    ldr x10, [x19, #256]
    cmp x27, x10
    b.ne inventory_fit_refused
    cbz x27, inventory_fit_refused
    ldr x10, [x27]
    adrp x11, info_data_vtable
    add x11, x11, :lo12:info_data_vtable
    cmp x10, x11
    b.ne inventory_fit_refused
    ldr x9, [x19, #104]
    add x9, x9, #0x2000
    ldr x10, [x9, #0x3e0]
    adrp x11, player_armor_vtable
    add x11, x11, :lo12:player_armor_vtable
    cmp x10, x11
    b.ne inventory_fit_refused
    ldr w10, [x9, #0x3f8]
    ldr w11, [x19, #240]
    cmp w10, w11
    b.ne inventory_fit_refused
    cmn w10, #1
    b.eq inventory_fit_upper_id
    ldr x10, [x9, #0x3f0]
    ldr x11, [x19, #280]
    cmp x10, x11
    b.ne inventory_fit_refused
    cbz x10, inventory_fit_refused
    ldr x10, [x10, #0x48]
    ldr x11, [x19, #272]
    cmp x10, x11
    b.ne inventory_fit_refused
inventory_fit_upper_id:
    ldr w10, [x9, #0x408]
    ldr w11, [x19, #288]
    cmp w10, w11
    b.ne inventory_fit_refused
    // The first traversal already validated the entire list's links/count.
    // This bounded second pass checks exactly the old equipped armor pair.
    ldr w9, [x21, #0x78]
    ldr x10, [x21, #0x68]
    mov w16, wzr
    mov w17, wzr
inventory_fit_list:
    sub x11, x10, #8
    ldrb w12, [x11, #0x25]
    cmp w12, #1
    b.ne inventory_fit_list_next
    ldrb w12, [x11, #0x24]
    cmp w12, #1
    b.ne inventory_fit_list_next
    ldr w12, [x11, #0x18]
    cmp w12, #4
    b.eq inventory_fit_list_head
    cmp w12, #5
    b.ne inventory_fit_list_next
    ldr x12, [x19, #232]
    cmp x12, x11
    b.ne inventory_fit_refused
    add w17, w17, #1
    b inventory_fit_list_next
inventory_fit_list_head:
    ldr x12, [x19, #224]
    cmp x12, x11
    b.ne inventory_fit_refused
    add w16, w16, #1
inventory_fit_list_next:
    ldr x10, [x10]
    subs w9, w9, #1
    b.ne inventory_fit_list
    ldr x25, [x19, #224]
    ldr x26, [x19, #232]
    cmp x25, #0
    cset w9, ne
    cmp w16, w9
    b.ne inventory_fit_refused
    cmp x26, #0
    cset w9, ne
    cmp w17, w9
    b.ne inventory_fit_refused
    cbz x25, inventory_fit_check_upper
    mov x0, x25
    mov w1, #4
    ldr w2, [x19, #268]
    ldr w3, [x19, #264]
    bl inventory_aux_armor
    cbz w0, inventory_fit_refused
inventory_fit_check_upper:
    cbz x26, inventory_fit_candidates
    mov x0, x26
    mov w1, #5
    ldr w2, [x19, #292]
    ldr w3, [x19, #296]
    bl inventory_aux_armor
    cbz w0, inventory_fit_refused
inventory_fit_candidates:
    cmp w24, #3
    csel x25, x23, x25, eq
    cmp w24, #4
    csel x26, x23, x26, eq
    cbz x25, inventory_fit_head_raw_ready
    add x25, x25, #0x3c
inventory_fit_head_raw_ready:
    cbz x26, inventory_fit_upper_raw_ready
    add x26, x26, #0x3c
inventory_fit_upper_raw_ready:
    ldr w9, [x19, #20]
    cmp w9, #7
    b.ne inventory_fit_raw_ready
    cmp w24, #3
    b.ne inventory_fit_remove_upper
    adrp x25, default_head_name
    add x25, x25, :lo12:default_head_name
inventory_fit_remove_upper:
    cmp w24, #4
    b.ne inventory_fit_raw_ready
    adrp x26, default_upper_name
    add x26, x26, :lo12:default_upper_name
inventory_fit_raw_ready:
    mov w28, wzr
    cbz x25, inventory_fit_variant
    cbz x26, inventory_fit_variant
    mov x0, x27
    mov x1, x25
    adrp x2, head_mantle_key
    add x2, x2, :lo12:head_mantle_key
    mov w3, wzr
    mov w4, #1
    bl native_info_int
    cmp w0, #2
    b.ne inventory_fit_variant
    mov x0, x27
    mov x1, x26
    adrp x2, upper_mantle_key
    add x2, x2, :lo12:upper_mantle_key
    mov w3, wzr
    mov w4, #1
    bl native_info_int
    cmp w0, #0
    cset w28, ne
inventory_fit_variant:
    ldr w9, [x19, #248]
    cmp w28, w9
    b.ne inventory_fit_refused
    cbnz x25, inventory_fit_build_head
    // Default head cannot require a mantle variant or extra creation.
    ldr w9, [x19, #252]
    cbnz w9, inventory_fit_refused
    b inventory_fit_success
inventory_fit_build_head:
    add x9, x19, #512
    adrp x10, pouch_string_vtable
    add x10, x10, :lo12:pouch_string_vtable
    str x10, [x9]
    add x10, x9, #20
    str x10, [x9, #8]
    mov w11, #64
    str w11, [x9, #16]
    mov x11, x25
    mov w12, #61
inventory_fit_copy:
    ldrb w13, [x11], #1
    cbz w13, inventory_fit_append
    strb w13, [x10], #1
    subs w12, w12, #1
    b.ne inventory_fit_copy
    b inventory_fit_refused
inventory_fit_append:
    cbz w28, inventory_fit_terminate
    mov w13, #95
    strb w13, [x10], #1
    mov w13, #66
    strb w13, [x10], #1
inventory_fit_terminate:
    strb wzr, [x10]
    add x0, x19, #532
    bl inventory_hash_name
    ldr w9, [x19, #244]
    cmp w0, w9
    b.ne inventory_fit_refused
    cmp w24, #3
    b.eq inventory_fit_no_extra_head
    ldr x0, [x19, #272]
    cbz x0, inventory_fit_refused
    add x0, x0, #0x1c
    bl inventory_hash_name
    ldr w9, [x19, #244]
    cmp w0, w9
    cset w10, ne
    ldr w9, [x19, #252]
    cmp w9, w10
    b.ne inventory_fit_refused
    b inventory_fit_success
inventory_fit_no_extra_head:
    ldr w9, [x19, #252]
    cbnz w9, inventory_fit_refused
inventory_fit_success:
    adrp x9, info_data_root
    ldr x9, [x9, :lo12:info_data_root]
    cmp x9, x27
    cset w0, eq
    b inventory_fit_return
inventory_fit_refused:
    mov w0, wzr
inventory_fit_return:
    ldr x30, [sp, #32]
    ldp x27, x28, [sp, #16]
    ldp x25, x26, [sp], #64
    ret

// x0 native equipped pouch item, w1 type, w2 name hash, w3 dye.
// Called only after bounded exact membership; all strings must be inline.
inventory_aux_armor:
    ldr w9, [x0, #0x18]
    cmp w9, w1
    b.ne inventory_aux_refused
    ldrh w9, [x0, #0x24]
    cmp w9, #0x101
    b.ne inventory_aux_refused
    ldr w9, [x0, #0x20]
    cmp w9, w3
    b.ne inventory_aux_refused
    add w9, w9, #1
    cmp w9, #16
    b.hi inventory_aux_refused
    ldr x9, [x0, #0x28]
    adrp x10, pouch_string_vtable
    add x10, x10, :lo12:pouch_string_vtable
    cmp x9, x10
    b.ne inventory_aux_refused
    ldr w9, [x0, #0x38]
    cmp w9, #64
    b.ne inventory_aux_refused
    ldr x9, [x0, #0x30]
    add x10, x0, #0x3c
    cmp x9, x10
    b.ne inventory_aux_refused
    mov w10, #0x9dc5
    movk w10, #0x811c, lsl #16
    mov w11, #0x193
    movk w11, #0x100, lsl #16
    mov w12, #64
inventory_aux_hash:
    ldrb w13, [x9], #1
    cbz w13, inventory_aux_hash_done
    eor w10, w10, w13
    mul w10, w10, w11
    subs w12, w12, #1
    b.ne inventory_aux_hash
    b inventory_aux_refused
inventory_aux_hash_done:
    cmp w12, #64
    b.eq inventory_aux_refused
    cmp w10, w2
    cset w0, eq
    ret
inventory_aux_refused:
    mov w0, wzr
    ret

// x0 bounded native actor-name string, returns FNV32; zero on missing NUL.
inventory_hash_name:
    mov x9, x0
    mov w0, #0x9dc5
    movk w0, #0x811c, lsl #16
    mov w10, #0x193
    movk w10, #0x100, lsl #16
    mov w11, #64
inventory_hash_loop:
    ldrb w12, [x9], #1
    cbz w12, inventory_hash_return
    eor w0, w0, w12
    mul w0, w0, w10
    subs w11, w11, #1
    b.ne inventory_hash_loop
    mov w0, wzr
inventory_hash_return:
    ret

// Wrap Player calc's existing predicate; apply committed pending food on this
// actor thread using the same virtual implementation as native event actions.
.global botw_player_food_hook
botw_player_food_hook:
    stp x29, x30, [sp, #-64]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    mov x20, x0
    bl native_player_food_predicate
    stp x0, x1, [sp, #32]
    adrp x19, bridge_mailbox
    add x19, x19, :lo12:bridge_mailbox
    add x9, x19, #416
    ldar w10, [x9]
    cbz w10, player_food_done
    cmn w10, #1
    b.eq player_food_done
    str w10, [sp, #48]
    adrp x9, player_info_root
    ldr x9, [x9, :lo12:player_info_root]
    cbz x9, player_food_done
    ldr x10, [x9]
    adrp x11, player_info_vtable
    add x11, x11, :lo12:player_info_vtable
    cmp x10, x11
    b.ne player_food_done
    ldr x9, [x9, #0x60]
    cmp x9, x20
    b.ne player_food_done
    ldr x10, [x20]
    adrp x11, player_vtable
    add x11, x11, :lo12:player_vtable
    cmp x10, x11
    b.ne player_food_done
    ldr x10, [x19, #424]
    cmp x10, x20
    b.ne player_food_stale
    ldr w10, [x20, #0x60]
    ldr w11, [x19, #456]
    cmp w10, w11
    b.ne player_food_stale
    adrp x9, ui_root
    ldr x9, [x9, :lo12:ui_root]
    cbz x9, player_food_done
    add x9, x9, #0x64000
    ldr x10, [x9, #0xc30]
    mov x11, #0
    movk x11, #0x900, lsl #16
    movk x11, #0x1e, lsl #32
    cmp x10, x11
    b.ne player_food_done
    mov w13, #1
    b player_food_try_claim
player_food_stale:
    mov w13, #2
player_food_try_claim:
    // Claim after player validation. -1 stays busy until result publication.
    add x9, x19, #416
    ldr w14, [sp, #48]
    mov w11, #-1
player_food_claim:
    ldaxr w10, [x9]
    cmp w10, w14
    b.ne player_food_unclaimed
    stlxr w12, w11, [x9]
    cbnz w12, player_food_claim
    mov w10, w13
    cmp w13, #2
    b.eq player_food_complete
    mov x0, x20
    bl native_player_food_apply
    mov w10, #1
    b player_food_complete
player_food_unclaimed:
    // No store on a lost claim; the next exclusive load replaces the reservation.
    b player_food_done
player_food_complete:
    str w10, [x19, #420]
    add x9, x19, #416
    stlr wzr, [x9]
player_food_done:
    ldp x0, x1, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #64
    ret

// DUOD native map adapter. Private mailbox640..799; no Pouch overlap.
// Exact World, MainField, root/array membership and marker identity are mandatory.
    .ifndef BOTW_INVENTORY_ONLY
.include "map-ui-actions.inc"

.include "world-rune-ui-actions.inc"

// Exact1.6 native Y entry: watchpoint at manager+0x44800, main+0x10C48D0.
// The native function owns its lock; never wrap it in a second pouch lock.
    .endif
    .ifdef BOTW_INVENTORY_ONLY
    .ifdef BOTW_MAP
    .include "map-ui-actions.inc"
    .endif
    .ifdef BOTW_SHORTCUTS
    .include "world-rune-ui-actions.inc"
    .endif
    .endif
sort_request:
    adrp x9, pouch_root
    ldr x21, [x9, :lo12:pouch_root]
    ldr x9, [x19, #32]
    cmp x21, x9
    b.ne refused
    cbz x21, refused
    ldr x9, [x21]
    adrp x10, pouch_vtable
    add x10, x10, :lo12:pouch_vtable
    cmp x9, x10
    b.ne refused
    // Sort belongs to World Inventory, never a native menu transition.
    mov x11, #0
    movk x11, #0x900, lsl #16
    movk x11, #0x1e, lsl #32
    // x10 was reused above: re-read the stable UI flags before invoking native code.
    add x9, x22, #0x64, lsl #12
    ldr x10, [x9, #0xc30]
    cmp x10, x11
    b.ne refused
    ldr w24, [x19, #24]
    cmp w24, #6
    b.hi refused
    mov x0, x21
    mov w1, w24
    .ifdef BOTW_190
    mov w2, wzr
    .endif
    bl native_sort_items
    b successful

.include "holding-ui-actions.inc"
.ifndef BOTW_INVENTORY_ONLY
.include "warp-ui-actions.inc"
.else
.ifdef BOTW_WARP
.include "warp-ui-actions.inc"
.endif
.endif

.if hud_enabled
.include "companion-hud.inc"
.endif
