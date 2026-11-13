map_edit_request:
    sub x11, x11, #4
    cmp x10, x11
    b.ne refused
    adrp x0, stage_current
    add x0, x0, :lo12:stage_current
    bl map_stage_guard
    cbz w0, refused
    adrp x0, stage_requested
    add x0, x0, :lo12:stage_requested
    bl map_stage_guard
    cbz w0, refused
    adrp x9, map_root
    ldr x21, [x9, :lo12:map_root]
    ldr x9, [x19, #640]
    cmp x21, x9
    b.ne refused
    cbz x21, refused
    ldr x9, [x21]
    adrp x10, map_vtable
    add x10, x10, :lo12:map_vtable
    cmp x9, x10
    b.ne refused
    adrp x9, gamedata_root
    ldr x9, [x9, :lo12:gamedata_root]
    ldr x10, [x19, #760]
    cmp x9, x10
    b.ne refused
    cbz x9, refused
    ldr x9, [x9]
    adrp x10, gamedata_manager_vtable
    add x10, x10, :lo12:gamedata_manager_vtable
    cmp x9, x10
    b.ne refused
    adrp x9, map_controller_root
    ldr x22, [x9, :lo12:map_controller_root]
    ldr x9, [x19, #648]
    cmp x22, x9
    b.ne refused
    cbz x22, refused
    ldr x9, [x22]
    adrp x10, map_controller_vtable
    add x10, x10, :lo12:map_controller_vtable
    cmp x9, x10
    b.ne refused
    ldr x9, [x22, #0x8d8]
    cmp x9, x21
    b.ne refused
    ldr x9, [x22, #0x8e0]
    add x10, x21, #0x850
    cmp x9, x10
    b.ne refused
    ldr x10, [x19, #656]
    cmp x9, x10
    b.ne refused
    ldr x9, [x9]
    adrp x10, map_area_vtable
    add x10, x10, :lo12:map_area_vtable
    cmp x9, x10
    b.ne refused
    ldr w9, [x21, #0x610]
    cmp w9, #100
    b.ne refused
    ldr w9, [x21, #0x614]
    cmp w9, #100
    b.ne refused
    ldr w9, [x21, #0x658]
    cmp w9, #5
    b.ne refused
    ldr w9, [x21, #0x65c]
    cmp w9, #5
    b.ne refused
    ldr x22, [x21, #0x618]
    ldr x9, [x19, #664]
    cmp x22, x9
    b.ne refused
    cbz x22, refused
    ldr x23, [x21, #0x660]
    ldr x9, [x19, #672]
    cmp x23, x9
    b.ne refused
    cbz x23, refused
    // All stamp objects are validated before any native list walk.
    mov x12, #0
    mov x13, #0
    adrp x14, map_stamp_vtable
    add x14, x14, :lo12:map_stamp_vtable
map_stamp_members:
    ldr x9, [x22, x12, lsl #3]
    cbz x9, refused
    tst x9, #7
    b.ne refused
    ldr x10, [x9]
    cmp x10, x14
    b.ne refused
    ldrb w10, [x9, #0x3c]
    tbnz w10, #4, map_stamp_next
    cmp x13, #0
    csel x13, x9, x13, eq
map_stamp_next:
    add x12, x12, #1
    cmp x12, #100
    b.lo map_stamp_members
    ldr x9, [x21, #0x650]
    cmp x9, x13
    b.ne refused
    ldr x9, [x19, #720]
    cmp x9, x13
    b.ne refused
    mov x12, #0
    adrp x14, map_pin_vtable
    add x14, x14, :lo12:map_pin_vtable
map_pin_members:
    ldr x9, [x23, x12, lsl #3]
    cbz x9, refused
    tst x9, #7
    b.ne refused
    ldr x10, [x9]
    cmp x10, x14
    b.ne refused
    ldr w10, [x9, #0x40]
    cmp w10, w12
    b.ne refused
    add x12, x12, #1
    cmp x12, #5
    b.lo map_pin_members
    ldr w24, [x19, #708]
    cmp w24, #14
    b.hi refused
    ldr x0, [x19, #680]
    cbz x0, refused
    // Target must belong to its expected array and preserve active/kind/XYZ.
    ldr w12, [x19, #728]
    cmp w12, #100
    b.hs refused
    ldr w10, [x19, #732]
    cbz w10, map_target_stamp
    cmp w10, #1
    b.ne refused
    cmp w12, #5
    b.hs refused
    ldr x9, [x23, x12, lsl #3]
    b map_target_compare
map_target_stamp:
    ldr x9, [x22, x12, lsl #3]
map_target_compare:
    cmp x9, x0
    b.ne refused
    ldr w9, [x0, #0x1c]
    ldr w10, [x19, #700]
    cmp w9, w10
    b.ne refused
    ldr w9, [x0, #0x3c]
    and w9, w9, #16
    ldr w10, [x19, #704]
    cmp w9, w10
    b.ne refused
    ldr x9, [x0, #0x28]
    ldr x10, [x19, #688]
    cmp x9, x10
    b.ne refused
    ldr w9, [x0, #0x30]
    ldr w10, [x19, #696]
    cmp w9, w10
    b.ne refused
    cmp w24, #14
    b.eq map_delete_validate
    cmp w24, #6
    b.lo map_place_stamp_validate
    cmp w24, #10
    b.hi map_place_stamp_validate
    sub w9, w24, #6
    cmp w9, w12
    b.ne refused
    ldr w9, [x19, #732]
    cmp w9, #1
    b.ne refused
    b map_place_projection
map_place_stamp_validate:
    ldr w9, [x19, #732]
    cbnz w9, refused
    cmp x0, x13
    b.ne refused
    ldr w9, [x19, #704]
    cbnz w9, refused
map_place_projection:
    // Bound finite inputs in native code too; reject NaN through unordered compare.
    ldr s0, [x19, #712]
    fabs s0, s0
    mov w9, #0x8000
    movk w9, #0x45bb, lsl #16 // 6000
    fmov s1, w9
    fcmp s0, s1
    b.hi refused
    ldr s0, [x19, #716]
    fabs s0, s0
    mov w9, #0x4000
    movk w9, #0x459c, lsl #16 // 5000
    fmov s1, w9
    fcmp s0, s1
    b.hi refused
    // Exact projection snapshot is rechecked; native trig uses that angle.
    ldr x9, [x21, #0x878]
    ldr x10, [x21, #0x880]
    ldr x11, [x19, #736]
    ldr x12, [x19, #744]
    cmp x9, x11
    b.ne refused
    cmp x10, x12
    b.ne refused
    ldr x9, [x21, #0x8b0]
    ldr x10, [x19, #752]
    cmp x9, x10
    b.ne refused
    ldr s0, [x21, #0x878]
    fcmp s0, #0.0
    b.le refused
    b.vs refused
    fmov s1, #16.0
    fcmp s0, s1
    b.hi refused
    ldr s0, [x21, #0x8b0]
    fmov s1, #2.0
    fcmp s0, s1
    b.ne refused
    ldr s0, [x21, #0x8b4]
    fcmp s0, s1
    b.ne refused
    b map_edit_commit
map_delete_validate:
    ldr w9, [x19, #704]
    cmp w9, #16
    b.ne refused
map_edit_commit:
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
    cmp w24, #14
    b.eq map_delete_commit
    sub sp, sp, #64
    stp d8, d9, [sp]
    stp d10, d11, [sp, #16]
    stp d12, d13, [sp, #32]
    ldr s0, [x21, #0x87c]
    adrp x9, map_angle_constant
    ldr w9, [x9, :lo12:map_angle_constant]
    fmov s1, w9
    fmul s8, s0, s1
    fmov s0, s8
    bl native_cosf
    fmov s9, s0
    fmov s0, s8
    bl native_sinf
    fmov s10, s0
    ldr s11, [x21, #0x878]
    ldr s0, [x19, #712]
    ldr s1, [x21, #0x8b0]
    ldr s2, [x21, #0x880]
    fmul s0, s0, s1
    fadd s0, s0, s2
    fmul s12, s0, s11
    ldr s0, [x19, #716]
    fneg s0, s0
    ldr s1, [x21, #0x8b4]
    ldr s2, [x21, #0x884]
    fmul s0, s0, s1
    fadd s0, s0, s2
    fmul s13, s0, s11
    fmul s0, s9, s12
    fmul s1, s10, s13
    fadd s0, s0, s1
    fmul s1, s10, s12
    fneg s1, s1
    fmul s2, s9, s13
    fadd s1, s1, s2
    stp s0, s1, [sp, #48]
    mov x0, x21
    mov w1, w24
    add x2, sp, #48
    mov w3, #0
    bl native_map_place
    // Existing pins keep their active state, so C8(true) does not serialize
    // the changed coordinates. Use the same native save routine as C8.
    ldr w9, [x19, #704]
    cbz w9, map_place_saved
    cmp w24, #6
    b.lo map_place_saved
    cmp w24, #10
    b.hi map_place_saved
    mov x0, x21
    bl native_map_save_update
map_place_saved:
    ldp d8, d9, [sp]
    ldp d10, d11, [sp, #16]
    ldp d12, d13, [sp, #32]
    add sp, sp, #64
    b successful
map_delete_commit:
    ldr x0, [x19, #680]
    ldr w9, [x0, #0x3c]
    orr w9, w9, #8
    str w9, [x0, #0x3c]
    mov w1, #0
    ldr w9, [x19, #732]
    cbz w9, map_delete_stamp
    bl native_map_pin_disable
    b map_delete_recache
map_delete_stamp:
    bl native_map_stamp_disable
map_delete_recache:
    // This is the native UI's post-delete first-free cursor maintenance.
    mov x12, #0
    mov x13, #0
map_delete_scan:
    ldr x9, [x22, x12, lsl #3]
    ldrb w10, [x9, #0x3c]
    tbz w10, #4, map_delete_found
    add x12, x12, #1
    cmp x12, #100
    b.lo map_delete_scan
    b map_delete_cached
map_delete_found:
    mov x13, x9
map_delete_cached:
    str x13, [x21, #0x650]
    b successful
map_stage_guard:
    ldr x9, [x0]
    adrp x10, map_stage_vtable
    add x10, x10, :lo12:map_stage_vtable
    cmp x9, x10
    b.ne map_stage_failed
    ldr x9, [x0, #8]
    add x10, x0, #20
    cmp x9, x10
    b.ne map_stage_failed
    ldr w9, [x0, #16]
    cmp w9, #32
    b.ne map_stage_failed
    // Exact ten bytes including terminator.
    ldr x9, [x0, #20]
    mov x10, #0x614d
    movk x10, #0x6e69, lsl #16
    movk x10, #0x6946, lsl #32
    movk x10, #0x6c65, lsl #48
    cmp x9, x10
    b.ne map_stage_failed
    ldrh w9, [x0, #28]
    cmp w9, #0x64
    b.ne map_stage_failed
    mov w0, #1
    ret
map_stage_failed:
    mov w0, #0
    ret

