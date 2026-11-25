// SPDX-FileCopyrightText: Copyright 2026 jadesigns.
// SPDX-License-Identifier: GPL-3.0-or-later
// SO2R HideHUD guest helper (0.9.142; the Dave the Diver HideHUD loop, tools/dtd/discard_patch/stub.S).
// Star Ocean The Second Story R 1.0.1 (main build D214BA64D902AEDA). Eden places this code after main (load plan
// format 2, "guest_code"); CriMonoBehaviourManager.Update (main+1A843E0, every frame in every scene, on the game's
// own thread) jumps here instead of running its first instruction (sub sp, sp, #0x50). Each frame: count a heartbeat
// at +4; only while the magic word 'SO2H' is at +0, walk the HideHUD entries; then run the displaced instruction and
// carry on with Update.
//
// Mailbox (Eden's dual-screen mailbox, written by the module through store_u32 / store_u64):
//   +0x00 magic 'SO2H'   +0x04 heartbeat (counted every frame)
//   +0x100.. 15 entries of 0x30 bytes: +0x00 the HUD piece (MonoBehaviour*), +0x08 its class pointer (the entry is
//   used only while the object still has that class and a live native object, m_CachedPtr @0x10), +0x10 mode
//   (0 skip, 1 read its Transform.localScale into +0x18.., 2 set localScale to +0x24..), +0x14 frames done,
//   +0x18 / +0x1C / +0x20 the scale read, +0x24 / +0x28 / +0x2C the scale to set.
// Nothing else is touched: the pictures, the game's own logic and its own show / hide stay; scale 0 only makes the
// piece invisible.
    .text
    .globl _start
_start:
    stp     x29, x30, [sp, #-0x30]!
    mov     x29, sp
    stp     x0, x1, [sp, #0x10]
    stp     x19, x20, [sp, #0x20]
    .globl mb_adrp
mb_adrp:
    adrp    x19, _start            // relocated by Eden to the mailbox page
    .globl mb_add
mb_add:
    add     x19, x19, #0           // relocated by Eden to the mailbox offset
    ldr     w8, [x19, #4]
    add     w8, w8, #1
    str     w8, [x19, #4]
    ldr     w8, [x19]
    movz    w9, #0x4F53
    movk    w9, #0x4832, lsl #16
    cmp     w8, w9
    b.ne    out
    add     x20, x19, #0x100
hud_next:
    ldr     w8, [x20, #0x10]
    cbz     w8, hud_skip
    ldr     x0, [x20]
    cbz     x0, hud_skip
    ldr     x9, [x0, #0x10]        // m_CachedPtr: the native object is still alive
    cbz     x9, hud_skip
    ldr     x9, [x0]
    ldr     x10, [x20, #8]
    cmp     x9, x10                // still that class (not a freed / reused object)
    b.ne    hud_skip
    mov     x1, xzr
    .globl call_transform
call_transform:
    bl      .                      // relocated: Component.get_transform
    cbz     x0, hud_skip
    ldr     w8, [x20, #0x10]
    cmp     w8, #2
    b.eq    hud_set
    mov     x1, xzr
    .globl call_getscale
call_getscale:
    bl      .                      // relocated: Transform.get_localScale -> s0, s1, s2
    stp     s0, s1, [x20, #0x18]
    str     s2, [x20, #0x20]
    b       hud_done
hud_set:
    ldr     s0, [x20, #0x24]
    ldp     s1, s2, [x20, #0x28]
    mov     x1, xzr
    .globl call_setscale
call_setscale:
    bl      .                      // relocated: Transform.set_localScale(value)
hud_done:
    ldr     w8, [x20, #0x14]
    add     w8, w8, #1
    str     w8, [x20, #0x14]
hud_skip:
    add     x20, x20, #0x30
    add     x8, x19, #0x3D0        // 15 entries (0.9.143: + the footer's vertical list)
    cmp     x20, x8
    b.ne    hud_next
out:
    ldp     x19, x20, [sp, #0x20]
    ldp     x0, x1, [sp, #0x10]
    ldp     x29, x30, [sp], #0x30
    sub     sp, sp, #0x50          // Update's own first instruction
    .globl back
back:
    b       .                      // relocated: main+1A843E4 (the rest of Update)
