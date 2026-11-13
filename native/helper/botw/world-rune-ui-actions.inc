// World-only native Rune switch. Scratch mailbox640..748 is kind-specific.
world_rune_request:
    sub x11, x11, #4
    cmp x10, x11
    b.ne refused
    add x9, x22, #0x64, lsl #12
    ldr w10, [x9, #0xc24]
    cbnz w10, refused
    ldr w9, [x9, #0xc58]
    mov w10, #0xd0000
    tst w9, w10
    b.ne refused
    ldr w24, [x19, #712]
    cmp w24, #7
    b.hi refused
    adrp x9, screen_root
    ldr x9, [x9, :lo12:screen_root]
    cbz x9, refused
    ldr w10, [x9, #0x28]
    cmp w10, #99
    b.ne refused
    ldr x9, [x9, #0x30]
    cbz x9, refused
    ldr x21, [x9, #360]
    cbz x21, refused
    ldr x9, [x19, #640]
    cmp x21, x9
    b.ne refused
    ldr x9, [x21]
    adrp x10, screen_vtable
    add x10, x10, :lo12:screen_vtable
    cmp x9, x10
    b.ne refused
    ldr x9, [x21, #(0x150+pause_shift)]
    adrp x10, shortcut_world_state
    add x10, x10, :lo12:shortcut_world_state
    cmp x9, x10
    b.ne refused
    ldr w9, [x21, #(0x36e8+pause_shift)]
    add w10, w9, #1
    cmp w10, #8
    b.hi refused
    ldr w10, [x19, #720]
    cmp w9, w10
    b.ne refused
    adrp x9, rune_state_root
    ldr x23, [x9, :lo12:rune_state_root]
    cbz x23, refused
    ldr x9, [x19, #648]
    cmp x23, x9
    b.ne refused
    ldr x9, [x23]
    adrp x10, rune_state_vtable
    add x10, x10, :lo12:rune_state_vtable
    cmp x9, x10
    b.ne refused
    ldr w9, [x23, #0x240]
    add w10, w9, #1
    cmp w10, #8
    b.hi refused
    ldr w10, [x19, #716]
    cmp w9, w10
    b.ne refused
    adrp x9, gamedata_root
    ldr x22, [x9, :lo12:gamedata_root]
    cbz x22, refused
    ldr x9, [x19, #656]
    cmp x22, x9
    b.ne refused
    ldr x9, [x22]
    adrp x10, gamedata_manager_vtable
    add x10, x10, :lo12:gamedata_manager_vtable
    cmp x9, x10
    b.ne refused
    ldrb w9, [x22, #0xbe8]
    cmp w9, #1
    b.hi refused
    ldr w10, [x19, #728]
    cmp w9, w10
    b.ne refused
    ldr x9, [x22, #0xbe0]
    cbz x9, refused
    ldr x10, [x19, #672]
    cmp x9, x10
    b.ne refused
    ldr x9, [x9]
    cbz x9, refused
    ldr x10, [x19, #680]
    cmp x9, x10
    b.ne refused
    ldr w10, [x9, #8]
    cbz w10, refused
    mov w11, #1
    lsl w11, w11, #16
    cmp w10, w11
    b.hi refused
    ldr w11, [x19, #708]
    cmp w10, w11
    b.ne refused
    ldr w11, [x9, #12]
    cmp w10, w11
    b.hi refused
    mov w12, #1
    lsl w12, w12, #16
    cmp w11, w12
    b.hi refused
    ldr x9, [x9, #16]
    cbz x9, refused
    ldr x11, [x19, #688]
    cmp x9, x11
    b.ne refused
    ldr w11, [x19, #704]
    cmp w11, w10
    b.hs refused
    ldr x9, [x9, x11, lsl #3]
    cbz x9, refused
    ldr x10, [x19, #696]
    cmp x9, x10
    b.ne refused
    ldr x10, [x9]
    adrp x11, bool_flag_vtable
    add x11, x11, :lo12:bool_flag_vtable
    cmp x10, x11
    b.ne refused
    ldrb w10, [x9, #8]
    cbnz w10, refused
    adr x10, world_rune_hashes
    ldr w10, [x10, x24, lsl #2]
    ldr w11, [x9, #12]
    cmp w10, w11
    b.ne refused
    ldrb w10, [x9, #11]
    tbz w10, #0, refused
    ldrb w10, [x9, #9]
    ldr w11, [x19, #732]
    cmp w10, w11
    b.ne refused
    ldrb w11, [x22, #0xbe8]
    cbz w11, world_rune_permission_ok
    tbz w10, #0, refused
world_rune_permission_ok:
    adrp x9, dlc_root
    ldr x9, [x9, :lo12:dlc_root]
    cbz x9, refused
    ldr x10, [x19, #664]
    cmp x9, x10
    b.ne refused
    ldr x10, [x9]
    adrp x11, dlc_vtable
    add x11, x11, :lo12:dlc_vtable
    cmp x10, x11
    b.ne refused
    ldr w9, [x9, #dlc_version_field]
    lsr w10, w9, #16
    cbnz w10, refused
    ldr w10, [x19, #724]
    cmp w9, w10
    b.ne refused
    cmp w24, #6
    b.ne world_rune_option
    cmp w9, #0x300
    b.lo refused
world_rune_option:
    cmp w24, #7
    b.ne world_rune_commit
    adrp x9, amiibo_option_handle
    ldr w0, [x9, :lo12:amiibo_option_handle]
    .ifdef BOTW_190
    mov w1, wzr
    .endif
    bl native_option_bool
    tbz w0, #0, refused
world_rune_commit:
    // Final permit/epoch/World checks immediately precede the ordinary route.
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
    adrp x9, ui_root
    ldr x9, [x9, :lo12:ui_root]
    ldr x10, [x19, #48]
    cmp x9, x10
    b.ne refused
    add x9, x9, #0x64, lsl #12
    ldr x9, [x9, #0xc30]
    mov x10, #0
    movk x10, #0x900, lsl #16
    movk x10, #0x1e, lsl #32
    cmp x9, x10
    b.ne refused
    // Recheck changing identities/availability after the native option query.
    adrp x9, rune_state_root
    ldr x9, [x9, :lo12:rune_state_root]
    cmp x9, x23
    b.ne refused
    ldr w9, [x23, #0x240]
    ldr w10, [x19, #716]
    cmp w9, w10
    b.ne refused
    ldr w9, [x21, #(0x36e8+pause_shift)]
    ldr w10, [x19, #720]
    cmp w9, w10
    b.ne refused
    adrp x9, gamedata_root
    ldr x9, [x9, :lo12:gamedata_root]
    cmp x9, x22
    b.ne refused
    ldrb w9, [x22, #0xbe8]
    ldr w10, [x19, #728]
    cmp w9, w10
    b.ne refused
    ldr x9, [x22, #0xbe0]
    ldr x10, [x19, #672]
    cmp x9, x10
    b.ne refused
    ldr x9, [x9]
    ldr x10, [x19, #680]
    cmp x9, x10
    b.ne refused
    ldr x9, [x9, #16]
    ldr x10, [x19, #688]
    cmp x9, x10
    b.ne refused
    ldr w10, [x19, #704]
    ldr x9, [x9, x10, lsl #3]
    ldr x10, [x19, #696]
    cmp x9, x10
    b.ne refused
    adr x10, world_rune_hashes
    ldr w10, [x10, x24, lsl #2]
    ldr w11, [x9, #12]
    cmp w10, w11
    b.ne refused
    ldrb w10, [x9, #11]
    tbz w10, #0, refused
    ldrb w9, [x9, #9]
    ldr w10, [x19, #732]
    cmp w9, w10
    b.ne refused
    adrp x9, ui_root
    ldr x9, [x9, :lo12:ui_root]
    add x9, x9, #0x64, lsl #12
    ldr w10, [x9, #0xc24]
    cbnz w10, refused
    ldr w9, [x9, #0xc58]
    mov w10, #0xd0000
    tst w9, w10
    b.ne refused
    mov x0, x21
    mov w1, w24
    bl native_rune_resolve
    ldr w24, [x21, #(0x36e8+pause_shift)]
    add x0, x23, #0x220
    bl native_pouch_lock
    str w24, [x23, #0x240]
    add x0, x23, #0x220
    bl native_pouch_unlock
    b successful
world_rune_hashes:
    .word 0x191bccf9,0x191bccf9,0x795e7bbc,0x7504085d
    .word 0x5992b256,0xf7dd3e03,0xc9328299,0x89ebcae4

