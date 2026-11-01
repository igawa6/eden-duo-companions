// SPDX-License-Identifier: GPL-3.0-or-later
// Pure native target enumeration. No RNG, target UI, code injection or input simulation.
// x0 client, x1 current BattleMon, w2 EFFECTIVE move -> w0 field target0(self)/4(boss), or5.
// Scope is the twice-boot-verified offline wild raid, rule2/submode5/local participant0.
// Native7ECA00 maps target enums0/3 to descriptor2(opponents); enum7 returns the
// source field position directly. Enum2 is deliberately refused: descriptor5 is not self.
// 7EE2F0 enumerates live target pokeIDs. Require the exact boss/own PID in that list, then
// resolve it through native7F9470; roster order and species never stand in for identity.
sw_raid_target:
    stp x29, x30, [sp, #-0x90]!
    stp x19, x20, [sp, #0x10]
    stp x21, x22, [sp, #0x20]
    stp x23, x24, [sp, #0x30]
    stp x25, x26, [sp, #0x40]
    stp x27, x28, [sp, #0x50]
    mov x29, sp
    mov x19, x0
    mov x28, x1
    mov w21, w2
    ldrb w9, [x19, #0x2A6]
    cbnz w9, rtarget_bad
    ldr x27, [x19, #0x58]
    cbz x27, rtarget_bad
    ldr w9, [x27, #0x150]
    cmp w9, #2
    b.ne rtarget_bad
    ldr x9, [x27, #0x88]
    cbz x9, rtarget_bad
    ldr w10, [x9]
    cbnz w10, rtarget_bad
    ldrb w10, [x9, #0x3B8]
    cbnz w10, rtarget_bad
    ldrb w10, [x9, #0x3B9]
    cmp w10, #5
    b.ne rtarget_bad
    ldr x9, [x19, #0x70]
    cbz x9, rtarget_bad
    ldr x22, [x9, #8]
    cbz x22, rtarget_bad
    mov x0, x28
    bl sw_is_dead
    tbnz w0, #0, rtarget_bad             // a fainted raid actor has Cheer, not FIGHT
    mov w0, w21
    bl sw_move_target
    mov w24, #2
    cbz w0, rtarget_kind
    cmp w0, #3
    b.eq rtarget_kind
    cmp w0, #7
    b.ne rtarget_bad
    mov w24, wzr
rtarget_kind:
    ldrb w25, [x28, #0x7D]
    cmp w25, #29
    b.hi rtarget_bad
    mov x0, x27
    mov x1, x22
    mov w2, w25
    bl sw_field_position
    cbnz w0, rtarget_bad               // local field position0 is the verified fixture family
    cbz w24, rtarget_done              // native enum7: exact living source position, no RNG
    mov w23, w0
    mov x0, x22
    mov w1, #4
    bl sw_position_mon
    cbz x0, rtarget_bad
    ldrb w9, [x0, #0x273]
    cmp w9, #1
    b.ne rtarget_bad
    ldrb w25, [x0, #0x7D]
    cmp w25, #29
    b.hi rtarget_bad
    bl sw_is_dead
    tbnz w0, #0, rtarget_bad
rtarget_enumerate:
    str xzr, [sp, #0x60]
    str xzr, [sp, #0x68]
    add x0, sp, #0x60
    mov w1, w24
    mov w2, w23
    bl sw_target_descriptor
    add x0, sp, #0x60
    mov w1, #2
    mov w2, #5
    mov x3, x22
    add x4, sp, #0x68
    bl sw_target_enumerate
    cbz w0, rtarget_bad
    cmp w0, #5
    b.hi rtarget_bad
    mov w26, w0
    mov w9, wzr
    add x10, sp, #0x68
rtarget_scan:
    ldrb w11, [x10, w9, uxtw]
    cmp w11, w25
    b.eq rtarget_resolve
    add w9, w9, #1
    cmp w9, w26
    b.lo rtarget_scan
    b rtarget_bad
rtarget_resolve:
    mov x0, x27
    mov x1, x22
    mov w2, w25
    bl sw_field_position
    cmp w0, #4
    b.eq rtarget_done
    b rtarget_bad
rtarget_bad:
    mov w0, #5
rtarget_done:
    ldp x27, x28, [sp, #0x50]
    ldp x25, x26, [sp, #0x40]
    ldp x23, x24, [sp, #0x30]
    ldp x21, x22, [sp, #0x20]
    ldp x19, x20, [sp, #0x10]
    ldp x29, x30, [sp], #0x90
    ret
