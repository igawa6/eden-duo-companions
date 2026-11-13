quest_request:
    // World only. The ordinary journal handler's MapData rebuild and GameData
    // setter are used below; the hidden journal rehydrates on native reopen.
    sub x11, x11, #4
    cmp x10, x11
    b.ne refused
    adrp x9, quest_root
    ldr x21, [x9, :lo12:quest_root]
    ldr x9, [x19, #96]
    cmp x21, x9
    b.ne refused
    cbz x21, refused
    ldr x9, [x21]
    adrp x12, quest_manager_vtable
    add x12, x12, :lo12:quest_manager_vtable
    cmp x9, x12
    b.ne refused
    ldr w9, [x21, #0xb8]
    ldr w10, [x19, #56]
    cmp w9, w10
    b.ne refused
    cmp w9, #512
    b.hi refused
    ldr w24, [x19, #24]
    cmp w24, w9
    b.hs refused
    ldr x9, [x21, #0xc0]
    cbz x9, refused
    ldr x23, [x9, x24, lsl #3]
    ldr x9, [x19, #40]
    cmp x23, x9
    b.ne refused
    cbz x23, refused
    tst x23, #7
    b.ne refused
    ldr x9, [x23]
    add x10, x12, #0x20
    cmp x9, x10
    b.ne refused
    ldr w9, [x23, #12]
    cmp w9, #1
    b.ne refused
    ldr w9, [x23, #0x140]
    cmp w9, #1
    b.lt refused
    ldrb w9, [x23, #0x30]
    cmp w9, #1
    b.ne refused
    ldr w9, [x23, #0xd8]
    tst w9, #0xfffffffe
    b.ne refused
    adrp x9, map_root
    ldr x21, [x9, :lo12:map_root]
    ldr x9, [x19, #104]
    cmp x21, x9
    b.ne refused
    cbz x21, refused
    ldr x9, [x21]
    adrp x10, map_vtable
    add x10, x10, :lo12:map_vtable
    cmp x9, x10
    b.ne refused
    adrp x9, gamedata_root
    ldr x22, [x9, :lo12:gamedata_root]
    ldr x9, [x19, #112]
    cmp x22, x9
    b.ne refused
    cbz x22, refused
    ldr x9, [x22]
    adrp x10, gamedata_manager_vtable
    add x10, x10, :lo12:gamedata_manager_vtable
    cmp x9, x10
    b.ne refused
    adrp x9, destination_handle
    ldr w24, [x9, :lo12:destination_handle]
    ldr w10, [x19, #120]
    cmp w24, w10
    b.ne refused
    cmn w24, #1
    b.eq refused
    .ifdef BOTW_INVENTORY_ONLY
    // The isolated quest route retains the shared commit's cancel/epoch checks.
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
    b quest_commit
    .else
    b commit
    .endif
quest_commit:
    str x23, [x21, #0x3a0]
    mov x0, x21
    bl native_map_rebuild
    mov x0, x22
    ldr w1, [x19, #24]
    mov w2, w24
    bl native_destination_set
