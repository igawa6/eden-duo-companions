.set hud_map_root_field, 0xe30
.set hud_main_root_field, 0xf70
.set hud_dungeon_field, 0x628
.ifdef BOTW_190
.set hud_map_root_field, 0xdb0
.set hud_main_root_field, 0xf80
.set hud_dungeon_field, 0x640
.endif
// Exact BOTW1.6.0 native HUD layout. Keep AppMap ticking, hide only its draw tree.
// World/Menu roots are reacquired on every call; no cached guest pointer survives a scene.
companion_map_restore:
    mov w17, #1
    b companion_map_visibility
companion_map_hide:
    // Suppression requires explicit host opt-in. Zero keeps the native minimap drawn.
    adrp x9, bridge_mailbox
    add x9, x9, :lo12:bridge_mailbox
    ldr w10, [x9, #2048]
    cbz w10, hud_map_return
    ldr w11, [x9, #4]
    add w11, w11, #1
    sub w11, w11, w10
    cmp w11, #60
    b.hi hud_map_return
    mov w17, #0
companion_map_visibility:
    adrp x9, ui_root
    ldr x9, [x9, :lo12:ui_root]
    cbz x9, hud_map_return
    add x9, x9, #0x64, lsl #12
    ldr x10, [x9, #0xc30]
    cbnz w17, hud_map_scope
    tbnz x10, #2, hud_map_return // Native menu maps remain visible.
hud_map_scope:
    // World or Menu, differing only at bit2. Other states never touch a pane.
    bic x10, x10, #4
    mov x11, #0
    movk x11, #0x900, lsl #16
    movk x11, #0x1e, lsl #32
    cmp x10, x11
    b.ne hud_map_return
    adrp x9, screen_root
    ldr x9, [x9, :lo12:screen_root]
    cbz x9, hud_map_return
    ldr w10, [x9, #0x28]
    cmp w10, #99
    b.ne hud_map_return
    ldr x9, [x9, #0x30]
    cbz x9, hud_map_return
    ldr x9, [x9, #336] // slot42, AppMap
    cbz x9, hud_map_return
    ldr x10, [x9]
    adrp x11, hud_map_vtable
    add x11, x11, :lo12:hud_map_vtable
    cmp x10, x11
    b.ne hud_map_return
    // Verify the owning screen's layout identity as well as its class and slot.
    ldr x10, [x9, #0xd0]
    cbz x10, hud_map_return
    ldrh w11, [x10, #8]
    cmp w11, #0x30 // final zero digit followed by NUL
    b.ne hud_map_return
    ldr x10, [x10]
    adr x11, hud_app_map_name
    ldr x11, [x11]
    cmp x10, x11
    b.ne hud_map_return
    add x9, x9, #3, lsl #12
    ldr x9, [x9, #hud_map_root_field]
    cbz x9, hud_map_return
    ldr x10, [x9]
    adrp x11, hud_root_vtable
    add x11, x11, :lo12:hud_root_vtable
    cmp x10, x11
    b.ne hud_map_return
    ldr x10, [x9, #0x18]
    cbnz x10, hud_map_return // Root must have no parent.
    ldr x10, [x9, #0xb8]
    adr x11, hud_root_name
    ldr x11, [x11]
    cmp x10, x11
    b.ne hud_map_return
    ldrb w10, [x9, #0xc0]
    cbnz w10, hud_map_return
    ldrb w10, [x9, #0x58]
    bic w10, w10, #1
    orr w10, w10, w17
    strb w10, [x9, #0x58]
hud_map_return:
    ret

companion_hud_pose:
    // Frame 160: x19-x24 at +16..+63, Weather/Time native pane state at +64..+159 (item 7).
    stp x29, x30, [sp, #-160]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    adrp x9, ui_root
    ldr x9, [x9, :lo12:ui_root]
    cbz x9, hud_pose_return
    add x9, x9, #0x64, lsl #12
    ldr x10, [x9, #0xc30]
    mov x11, #0
    movk x11, #0x900, lsl #16
    movk x11, #0x1e, lsl #32
    cmp x10, x11
    b.ne hud_pose_return
    adrp x9, screen_root
    ldr x9, [x9, :lo12:screen_root]
    cbz x9, hud_pose_return
    ldr w10, [x9, #0x28]
    cmp w10, #99
    b.ne hud_pose_return
    ldr x9, [x9, #0x30]
    cbz x9, hud_pose_return
    ldr x19, [x9, #296] // slot37, MainScreen
    cbz x19, hud_pose_return
    ldr x10, [x19]
    adrp x11, hud_screen_vtable
    add x11, x11, :lo12:hud_screen_vtable
    cmp x10, x11
    b.ne hud_pose_return
    add x9, x19, #3, lsl #12
    ldr x20, [x9, #hud_main_root_field] // MainScreen RootPane
    ldr x21, [x9, #hud_dungeon_field] // Native MainScreen_00_Dungeon animator
    cbz x20, hud_pose_return
    cbz x21, hud_pose_return
    ldr x10, [x21]
    adrp x11, hud_anim_vtable
    add x11, x11, :lo12:hud_anim_vtable
    cmp x10, x11
    b.ne hud_pose_return
    ldr x9, [x21, #0x18]
    cbz x9, hud_pose_return
    ldr w10, [x9]
    mov w11, #0x6170
    movk w11, #0x3169, lsl #16 // pai1
    cmp w10, w11
    b.ne hud_pose_return
    ldrh w10, [x9, #8]
    cmp w10, #2
    b.ne hud_pose_return
    ldrh w10, [x21, #0x38]
    cmp w10, #5
    b.ne hud_pose_return
    ldr x19, [x21, #0x30]
    cbz x19, hud_pose_return
    adr x24, hud_binding_names
    mov w23, #5
hud_validate_binding:
    ldr x0, [x19]
    cbz x0, hud_pose_return
    ldr x9, [x0]
    adrp x10, hud_parts_vtable
    add x10, x10, :lo12:hud_parts_vtable
    cmp x9, x10
    b.ne hud_pose_return
    ldr x9, [x0, #0x18]
    cbz x9, hud_pose_return
    ldr x9, [x9, #0x18]
    cmp x9, x20
    b.ne hud_pose_return
    add x0, x0, #0xb8
    mov x1, x24
    mov w2, #24
hud_compare_name:
    ldrb w9, [x0], #1
    ldrb w10, [x1], #1
    cmp w9, w10
    b.ne hud_pose_return
    cbz w9, hud_binding_ok
    subs w2, w2, #1
    b.ne hud_compare_name
    b hud_pose_return
hud_binding_ok:
    add x24, x24, #24
    add x19, x19, #16
    subs w23, w23, #1
    b.ne hud_validate_binding
    // Nonzero = companion shown. Zero (including a missing module) restores native pose.
    adrp x9, bridge_mailbox
    add x9, x9, :lo12:bridge_mailbox
    ldr w10, [x9, #2048]
    cbz w10, hud_pose_native
    ldr w11, [x9, #4]
    add w11, w11, #1
    sub w11, w11, w10
    cmp w11, #60
    b.ls hud_pose_companion
hud_pose_native:
    ldr w10, [x9, #2052]
    cbnz w10, hud_pose_return
    mov w10, #1
    str w10, [x9, #2052]
    ldr s0, [x21, #0x20]
    bl hud_pose_animate
    // Item 7 (iteration 13): Pa_Time_00 TY is not animated (only the companion pose moves it):
    // put it back on its MainScreen_00 layout value for the native HUD.
    ldr x9, [x21, #0x30]
    ldr x20, [x9, #64] // binding 4, Pa_Time_00
    bl hud_time_base_y
    str s2, [x20, #0x34]
    b hud_pose_return
hud_pose_companion:
    str wzr, [x9, #2052]
    // Item 7 (iteration 12): the Dungeon pose (frame 1.0) also hides Pa_Weather_00 and
    // Pa_Time_00 (shrine layout has no weather/clock). Keep the native weather + time widget:
    // apply the native animator's own frame first, keep those two panes' SRT/size/flags/alpha
    // (pane +0x30..+0x5F), pose the gauges at 1.0, then put the two panes back.
    ldr x9, [x21, #0x30]
    ldr x19, [x9, #48] // binding 3, Pa_Weather_00 (validated above)
    ldr x20, [x9, #64] // binding 4, Pa_Time_00
    ldr x22, [x9, #16] // binding 1, Pa_TempMeter_00
    ldr s0, [x21, #0x20]
    bl hud_pose_animate
    ldr x23, [x22, #0x30] // native-frame TempMeter TX|TY
    ldp x9, x10, [x19, #0x30]
    stp x9, x10, [sp, #64]
    ldp x9, x10, [x19, #0x40]
    stp x9, x10, [sp, #80]
    ldp x9, x10, [x19, #0x50]
    stp x9, x10, [sp, #96]
    ldp x9, x10, [x20, #0x30]
    stp x9, x10, [sp, #112]
    ldp x9, x10, [x20, #0x40]
    stp x9, x10, [sp, #128]
    ldp x9, x10, [x20, #0x50]
    stp x9, x10, [sp, #144]
    fmov s0, #1.0
    bl hud_pose_animate
    ldp x9, x10, [sp, #64]
    stp x9, x10, [x19, #0x30]
    ldp x9, x10, [sp, #80]
    stp x9, x10, [x19, #0x40]
    ldp x9, x10, [sp, #96]
    stp x9, x10, [x19, #0x50]
    ldp x9, x10, [sp, #112]
    stp x9, x10, [x20, #0x30]
    ldp x9, x10, [sp, #128]
    stp x9, x10, [x20, #0x40]
    ldp x9, x10, [sp, #144]
    stp x9, x10, [x20, #0x50]
    // Item 7 (iteration 13): move Weather + Time by the gauges' pose offset (TempMeter posed
    // minus native TX/TY), so the right-side cluster keeps its native arrangement. Weather TX/TY
    // and Time TX are rewritten by every Animate (native frame values here); Time TY is not
    // animated, so it is set from its layout value (absolute, never accumulates).
    ldp s0, s1, [x22, #0x30]
    fmov s2, w23
    lsr x9, x23, #32
    fmov s3, w9
    fsub s0, s0, s2
    fsub s1, s1, s3
    ldp s2, s3, [x19, #0x30]
    fadd s2, s2, s0
    fadd s3, s3, s1
    stp s2, s3, [x19, #0x30]
    ldr s3, [x20, #0x30]
    fadd s3, s3, s0
    str s3, [x20, #0x30]
    bl hud_time_base_y
    fadd s2, s2, s1
    str s2, [x20, #0x34]
hud_pose_return:
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #160
    ret
// s2 = Pa_Time_00 layout TY (MainScreen_00.bflyt, 1.6.0: -279.0); clobbers w9 only.
hud_time_base_y:
    mov w9, #0x8000
    movk w9, #0xc38b, lsl #16
    fmov s2, w9
    ret
// Animate() at frame s0 with the animator enabled; frame/enabled restored. x21 = animator.
hud_pose_animate:
    // Animate() only applies the five native bindings. It does not enqueue playback.
    stp x29, x30, [sp, #-32]!
    stp x22, x23, [sp, #16]
    ldr w22, [x21, #0x20]
    ldrb w23, [x21, #0x24]
    mov w9, #1
    strb w9, [x21, #0x24]
    str s0, [x21, #0x20]
    mov x0, x21
    bl hud_animate
    str w22, [x21, #0x20]
    strb w23, [x21, #0x24]
    ldp x22, x23, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
.p2align 3
hud_root_name: .asciz "RootPane"
.p2align 3
hud_app_map_name: .asciz "AppMap_00"
.p2align 3
hud_binding_names:
    .asciz "Pa_SoundGauge_00"
    .zero 7
    .asciz "Pa_TempMeter_00"
    .zero 8
    .asciz "Pa_Sensor_00"
    .zero 11
    .asciz "Pa_Weather_00"
    .zero 10
    .asciz "Pa_Time_00"
    .zero 13
.p2align 2
