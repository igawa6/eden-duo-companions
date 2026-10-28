// Discard without the menu: Dave the Diver 1.0.6.1267 (main build F74B633E480E4E16).
// Lives over DebugExtensions.DebugConeSight (main+25CE2E0, a static debug helper nothing calls).
// InGameManager.LateUpdate (main+4203910, every frame in a dive, on the game's own thread) jumps
// here instead of running its first instruction. Each frame: count a heartbeat; if the companion
// posted a request (seq at +4 differs from done at +8 and the magic word is set), call
// LootBox.Remove_Impl exactly as the menu's DumpCell does, then mark it done. Then run the
// displaced instruction and carry on with LateUpdate.
//
// Mailbox (Eden's dual-screen mailbox, written by the module through store_u32/u64):
//   +0x00 magic 'DTDQ'  +0x04 request seq  +0x08 done seq  +0x0C requests run
//   +0x10 LootBox*  +0x18 item id  +0x1C grade  +0x20 lift type  +0x24 count
//   +0x28 catch time (DateTime ticks)  +0x30 heartbeat
// Menu (round 22), picked up by SushiBarManager.Update (main+402EEF0) at the sushi bar:
//   +0x40 heartbeat  +0x44 request seq  +0x48 done seq  +0x4C requests run
//   +0x50 SushiBarMenuManager*  +0x58 slot id  +0x5C recipe id  +0x60 bonus count (out)
//   +0x64 plates  +0x68 AddMenu's result  +0x6C kind (0 add, 1 remove, 2 swap = remove then add)
//   +0x70 RemoveMenu's result
// HideHUD (0.38.39, rounds 63 / 64), every LateUpdate while the magic word is set: up to 8 entries (0.38.56; 6 before) of 0x30 bytes from
// +0x100, written by the module: +0x00 the HUD component (MonoBehaviour*), +0x08 its class pointer (the entry is used
// only while the object still has that class and a live native object, m_CachedPtr @0x10), +0x10 mode (0 skip,
// 1 read its Transform.localScale into +0x18.., 2 set localScale to +0x24..), +0x14 frames done (counted),
// +0x18 / +0x1C / +0x20 the scale read, +0x24 / +0x28 / +0x2C the scale to set. Nothing else is touched: the
// pictures, the game's own logic and its own show / hide all stay; scale 0 only makes the piece invisible.
    .text
    .globl _start
_start:
    stp     x29, x30, [sp, #-0x30]!
    mov     x29, sp
    stp     x0, x1, [sp, #0x10]
    stp     x19, x20, [sp, #0x20]
mb_adrp:
    adrp    x19, _start            // relocated by Eden to the mailbox page
mb_add:
    add     x19, x19, #0           // relocated by Eden to the mailbox offset
    ldr     w8, [x19, #0x30]
    add     w8, w8, #1
    str     w8, [x19, #0x30]
    ldr     w8, [x19]
    movz    w9, #0x5444
    movk    w9, #0x5144, lsl #16
    cmp     w8, w9
    b.ne    out
    add     x20, x19, #0x100       // ---- HideHUD entries
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
    bl      GET_TRANSFORM          // Component.get_transform
    cbz     x0, hud_skip
    ldr     w8, [x20, #0x10]
    cmp     w8, #2
    b.eq    hud_set
    mov     x1, xzr
    bl      GET_LOCALSCALE         // Transform.get_localScale -> s0, s1, s2
    stp     s0, s1, [x20, #0x18]
    str     s2, [x20, #0x20]
    b       hud_done
hud_set:
    ldr     s0, [x20, #0x24]
    ldp     s1, s2, [x20, #0x28]
    mov     x1, xzr
    bl      SET_LOCALSCALE         // Transform.set_localScale(value)
hud_done:
    ldr     w8, [x20, #0x14]
    add     w8, w8, #1
    str     w8, [x20, #0x14]
hud_skip:
    add     x20, x20, #0x30
    add     x8, x19, #0x280      // 0.38.56: 8 entries (+ the village Bei / Credit panels)
    cmp     x20, x8
    b.ne    hud_next
    add     x10, x19, #4
    ldar    w20, [x10]             // request seq (acquire: its fields were written before it)
    ldr     w10, [x19, #8]
    cmp     w20, w10
    b.eq    out
    ldr     x0, [x19, #0x10]
    cbz     x0, fin
    ldr     w1, [x19, #0x18]
    ldr     w2, [x19, #0x24]
    ldr     w3, [x19, #0x1c]
    ldr     w4, [x19, #0x20]
    mov     w5, #1
    ldr     x6, [x19, #0x28]
    mov     x7, xzr
    bl      REMOVE_IMPL
    ldr     w8, [x19, #0xc]
    add     w8, w8, #1
    str     w8, [x19, #0xc]
fin:
    add     x10, x19, #8
    stlr    w20, [x10]             // done = this request
out:
    ldp     x19, x20, [sp, #0x20]
    ldp     x0, x1, [sp, #0x10]
    ldp     x29, x30, [sp], #0x30
    stp     x29, x30, [sp, #-0x30]!   // LateUpdate's own first instruction
    b       LATEUPDATE_CONT

// ---- tonight's menu: SushiBarManager.Update's only instruction ("b main+402EF00") jumps here.
// If the companion posted a request, call SushiBarMenuManager.AddMenu(slot, recipe, &bonus,
// Place.Main, plates) the way the game's own "Add" does (RecipeScrollerSheet), then carry on.
    .globl menu_entry
menu_entry:
    stp     x29, x30, [sp, #-0x30]!
    mov     x29, sp
    stp     x0, x1, [sp, #0x10]
    stp     x19, x20, [sp, #0x20]
mb2_adrp:
    adrp    x19, _start            // relocated by Eden to the mailbox page
mb2_add:
    add     x19, x19, #0           // relocated by Eden to the mailbox offset
    ldr     w8, [x19, #0x40]
    add     w8, w8, #1
    str     w8, [x19, #0x40]
    ldr     w8, [x19]
    movz    w9, #0x5444
    movk    w9, #0x5144, lsl #16
    cmp     w8, w9
    b.ne    out2
    add     x10, x19, #0x44
    ldar    w20, [x10]             // request seq (acquire: its fields were written before it)
    ldr     w10, [x19, #0x48]
    cmp     w20, w10
    b.eq    out2
    ldr     x0, [x19, #0x50]
    cbz     x0, fin2
    ldr     w8, [x19, #0x6c]
    cbz     w8, add2               // kind 0: add only
    ldr     w1, [x19, #0x58]       // remove / swap: SushiBarMenuManager.RemoveMenu(slot), as the
    mov     x2, xzr                // game's own "Delete Menu" (RecipeScrollerSheet) does
    bl      REMOVE_MENU
    and     w0, w0, #0xff
    str     w0, [x19, #0x70]
    ldr     w8, [x19, #0x6c]
    cmp     w8, #1
    b.eq    cnt2                   // kind 1: remove only
    ldr     x0, [x19, #0x50]
add2:
    ldr     w1, [x19, #0x58]
    ldr     w2, [x19, #0x5c]
    add     x3, x19, #0x60
    mov     w4, wzr                // Place.Main
    ldr     w5, [x19, #0x64]
    mov     x6, xzr
    bl      ADD_MENU
    and     w0, w0, #0xff
    str     w0, [x19, #0x68]
cnt2:
    ldr     w8, [x19, #0x4c]
    add     w8, w8, #1
    str     w8, [x19, #0x4c]
fin2:
    add     x10, x19, #0x48
    stlr    w20, [x10]             // done = this request
out2:
    ldp     x19, x20, [sp, #0x20]
    ldp     x0, x1, [sp, #0x10]
    ldp     x29, x30, [sp], #0x30
    b       SUSHIBAR_UPDATE_BODY   // Update's own instruction: b main+402EF00
