// Diagnostic only: preserve the original UI call and record its frame cadence.
// No action dispatcher and no production support gate.
.text
.global ui_frame_probe
ui_frame_probe:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    adrp x19, bridge_mailbox
    add x19, x19, :lo12:bridge_mailbox
    mov w20, #0x3352
    movk w20, #0x4955, lsl #16
    str w20, [x19]
    ldr w20, [x19, #4]
    add w20, w20, #1
    str w20, [x19, #4]
    stp x0, x1, [x19, #8]
    str x30, [x19, #24]
    add x20, sp, #32
    str x20, [x19, #32]
    bl native_ui_update
    stp x0, x1, [x19, #40]
    ldr w20, [x19, #56]
    add w20, w20, #1
    str w20, [x19, #56]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
