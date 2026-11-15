// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD 1.0.1 guest helper (format-2 load plan): right-stick mirror, Adventure Pouch ops
// and the top-HUD hide list.
//
// Hook 1 (stick): main+0xed6408 `orr w8, w25, w5` inside the per-frame Controller::Update (main+0xed5708),
// where w5 / w25 hold the sampled right stick x / y as raw ints (-32767..32767) right before the
// game converts them (scvtf, / 32767.0) and stores them at this+0x90 (stick analysis).
// When the companion has a live stick vector in the mailbox, the helper substitutes it for the
// sampled one; otherwise the native values pass through untouched. It also copies the sampled npad
// button word (x24) into the mailbox, and adds the owner-approved virtual buttons to this+0x28: L3 for
// a staged shield bash, R3 for a sword stab, B for a put-away and L for the L lock. dev.48: none of
// them while the loop hook's last menu sample (+0x6C) is not play (a pause / map state or no
// controller), and the L lock's L not during an event either (EventManager state, +0x248). Nothing
// else is read or written. The stick hook leaves the condition flags alone (no cmp, only cbz / cbnz
// and flag-free arithmetic), so the code after the hook sees exactly the state the original
// instruction would have left.
//
// Hook 2 (pouch): main+0xe3a818 `adrp x8, <dSystem page>` in the game's main-loop function, after its
// per-frame calls and right before its epilogue (the same point the SSHD randomizer injects its own
// per-frame code). Only x8 is live from here on, and the epilogue reloads x19-x30 and d8-d11 from the
// stack, so the helper may use any general register and call game functions; it keeps sp balanced and
// never touches FP registers. One request per frame, on the game thread, between frames:
//   op 1 SWAP_POUCH(a, b): what the native Item Check's swapAdventurePouchItems (inlined into the
//        Item Check layout at main+0xc028d8..0xc02ce8, the game's only pouch-to-pouch move) does to
//        the current file: swap the two u32 pouch entries, move the shield slot along
//        (file+0x53B1), clear the last-used pouch slot (file+0x53B0) if it was one of the two.
//   op 2 POUCH_STOCK(pouch a, stock b): calls the game's own swapStockAndPouchItems (main+0xde59e0),
//        which the Item Check uses for store / retrieve / exchange.
//   op 3 EQUIP_B(item a): what the player's own ZR-wheel commit does once the wheel closes on an
//        item (player update main+0xa64230..0xa64990, equip analysis): the
//        item's ownership flag (table main+0x139b6dc) must be set (main+0x4ddcc0) and the item not
//        withheld by the story (main+0xddd2cc); then player+0x6408 (the selected B-item the next
//        ZR press draws) = item and the game's set-B-item routine main+0xa17c9c(player, item),
//        which writes the file's equipped_b_item (+0x53AE) and the HUD ZR icon. dev.40: like the
//        native wheel, which opens and commits with the sword drawn, it runs with Link's hands
//        empty or holding the sword; refused (6) with any other item in hand (the module has the
//        stick hook press B to put it away first, +0x1C) or no player, and (9) during the two
//        sword attack states the native wheel refuses (player+0x468 0x68 / 0x9A with the sword).
//        dev.41: refused (10) while the native B / pouch wheel is open (player+0x63B5 non-zero, the
//        module's own gate re-checked on the game thread): the wheel's own commit decides.
//   op 4 SET_POUCH_R(slot a): the file's lastUsedPouchItemSlot (+0x53B0) = a non-empty pouch slot,
//        the same plain store the player's pouch-wheel commit makes (main+0xa657ac); the native
//        pouch wheel (hold R) then opens on that slot.
// The loop hook also counts the top-HUD hide list's ttl (+0x200) down once per game frame and folds
// the draw probe's per-frame bits into its frame counters.
//
// Hook 3 (top-HUD hide, owner 2026-11-11): main+0x1007440 `str x21, [sp, #-0x30]!`, the entry of
// the layout library's Pane::Draw (vtable slot +0x20 of every pane class, x0 = pane, x1 = draw
// info): it tests the pane's visible bit (+0xEB bit 0), draws the pane itself (slot +0x28) and
// then calls Draw on each child. When the pane is in the module's hide list, the helper returns
// at once, so neither the pane nor its subtree is drawn this frame. Nothing in the pane is
// written: visibility, alpha (+0xE8 / +0xE9), animation and the game's own updates stay native,
// so the bottom screen's D-pad mirror (which reads the final visible bits and effective alpha)
// keeps seeing exactly what the game would draw. A list entry matches on the pane pointer AND the
// first 8 bytes of its name (+0xEC), and the list lapses by itself when the module stops
// refreshing the ttl. Only x9-x13 and the flags are used, which are free at a call boundary.
//
// Refused (nothing written) when the file manager is missing or an event (cutscene, dialogue, the
// native Item Check itself) runs. The pause-menu gate is the module's (it stages a request only
// with no pause / Collection / map menu open).
// dev.44: the loop hook also samples that menu state for the module once per game frame: the game's
// layout controller dLytControlGame_c (instance pointer main+0x183AC40) keeps its current state ID
// at +0x120, one of its static state objects (StateID_Normal main+0x164F988 in play; Pause*,
// Map* while the pause menu, Collection or map runs; names from the state objects, live 2026-11-14).
// The old "menu word" main+0x192D0E4 is a slot of the game thread dMain's own stack (created at
// main+0x103DDC8 on main+0x183E000, 0xF0000 bytes): whatever the deepest call left there, never a flag.
//
// Mailbox (4096 bytes; layout mirrored in native/modules/01002DA013484000.cpp):
//   +0x00 u32 magic   0x44485353 ("SSHD"), written by the module
//   +0x04 u32 heartbeat, incremented by the stick hook on every pass
//   +0x08 u32 ttl     passes the vector stays valid; the module refreshes it every tick, the
//                     helper counts it down, so a stopped companion falls back to native input
//   +0x0C s32 x, +0x10 s32 y   raw stick, already clamped by the module
//   +0x14 u32 npad buttons the live controller sampled this pass (stick hook; bit 6 = L): read by
//             the module's L-hold toggle to see what the game sees (virtual and physical alike)
//   +0x18 u32 shield-bash frames: while non-zero the stick hook adds the L3 bit (0x10000) to the
//             controller's button word; the loop hook counts it down once per game frame
//   +0x1C u32 put-away frames (dev.40): likewise the B bit (npad bit 1 -> game bit 0x400 at
//             main+0xed6378): the game's own put-away of the item in Link's hand, sent before a
//             ZR equip the way a ZR press puts the item away before the native wheel opens
//   +0x20 u32 L-hold frames (dev.45, the L lock): while non-zero the stick hook adds the L bit
//             (npad bit 6 -> game bit 0x4000) to the controller's button word and to the +0x14 copy;
//             the module refreshes it every tick while the lock holds L and zeroes it to let go, the
//             loop hook counts it down once per game frame (a stopped module releases L by itself).
//             dev.37..44 held L with the runtime's enforce rule instead (a virtual press every 50 ms,
//             one host log line each).
//   +0x24 u32 sword-stab frames (dev.47): R3, game bit 0x20000, counted down like L3.
//   +0x40 u32 req_seq (module, written last)   +0x44 u32 op   +0x48 u32 a   +0x4C u32 b
//   +0x50 u32 req_epoch (= the epoch word when staged)
//   +0x60 u32 res_seq (helper, written last = req_seq)   +0x64 u32 res_code
//   +0x68 u32 loop heartbeat (pouch hook) = one per game frame
//   +0x6C u32 menu sample (dev.44, loop hook, every frame): 0 the controller in StateID_Normal,
//             1 in any other state (pause, Collection, map and their resource states), 2 no controller
//   +0x70 u32 its stamp: the loop heartbeat of the frame it was taken in, written last (release)
//   +0x74 u32 the state ID's distance from StateID_Normal (0 play; logged by the module)
//   Stick sequence (spin attacks; the loop hook plays it, one step vector per game frame, through
//   the stick hook's x/y/ttl words above):
//   +0x80 u32 seq_req (module, written last)   +0x84 u32 steps (1..8)   +0x88 u32 seq_epoch
//   +0x90 u32 state (helper: 0 idle, 1 playing; the stick hook sets 2 when the physical stick moves)
//   +0x94 u32 step   +0x98 u32 frames left in it   +0x9C u32 id playing   +0xA8 u32 frames played
//   +0xA0 u32 seq_res (helper, written last = the id)   +0xA4 u32 seq_code
//   +0xC0 steps[8]: s16 x, s16 y, u32 frames (1..120)
//   seq_code: 0 played to the end, 1 the physical stick took over, 2 bad table, 3 stale epoch,
//             5 event running, 6 no player or Link holds an item other than the sword. Each frame
//             of play re-checks the event and the hands (5 / 6 stop it); stopping zeroes the ttl,
//             so the native stick passes straight through again.
//   Top-HUD hide list (draw hook), double-buffered (dev.37): the module writes a changed list into
//   the bank the draw hook is NOT reading, then publishes bank + count with one release store of
//   the select word, then the ttl. The hook reads the select word once per pane, so it always sees
//   a complete list and the count never drops to 0 during a change (dev.29 wrote count 0 first,
//   which let the panes draw for a frame on every change).
//   +0x200 u32 ttl     game frames the list stays valid (module refreshes it, loop hook counts down)
//   +0x204 u32 select  bits 0..7 entries in use (0..16), bit 8 the bank (0: +0x210, 1: +0x310)
//   +0x208 u32 hits    draws skipped (helper, diagnostics)
//   +0x210 bank 0 entries[16], +0x310 bank 1 entries[16]: u64 pane, u32 name bytes 0..3, 4..7
//   Draw probe (diagnostics, dev.37): per game frame, whether one pane (the module writes the Cross
//   base N_bace_00) was drawn (visible bit and effective alpha) or skipped by the list:
//   +0x410 u64 probe pane (0 = off)   +0x418 u32 this frame: bit 0 drawn, bit 1 skipped (draw hook)
//   +0x41C u32 frames drawn   +0x420 u32 frames skipped   +0x424 u32 frames counted (loop hook)
//   +0xFFC   load-plan epoch word (host)
// res_code: 0 ok, 1 bad op, 2 bad slot / item, 3 stale epoch, 4 no file manager, 5 event running,
//           6 no player or Link holds an item, 7 item not owned or withheld, 9 a sword attack plays,
//           10 a native item / pouch wheel is open (op 3).
//
// Undefined symbols are resolved by tools/skyward-sword-hd/build_helper_plan.py: the mailbox through
// a load-plan relocation, every main+offset target (sshd_* below) encoded directly, since the helper
// itself lives inside main's .text.

// player+0x6404 when Link's hands hold no item (live, hero file, 2026-11-11: 0xB idle, 0x4 with
// the slingshot drawn)
#define SSHD_HELD_NONE 0xb
// ... and 0x11 with the sword drawn (live, g0 file, 2026-11-11: after a right-stick flick); a spin
// sequence and (dev.40) a ZR equip accept either
#define SSHD_HELD_SWORD 0x11

    .text
    .globl sshd_stick_hook
    .p2align 2
sshd_stick_hook:
    stp     x9, x10, [sp, #-32]!
    str     x11, [sp, #16]
    adrp    x9, sshd_mailbox_base
    add     x9, x9, :lo12:sshd_mailbox_base
    ldr     w10, [x9, #0]
    movz    w11, #0x5353
    movk    w11, #0x4448, lsl #16
    eor     w10, w10, w11
    cbnz    w10, 1f                 // not configured: pass through
    ldr     w10, [x9, #4]
    add     w10, w10, #1
    str     w10, [x9, #4]           // heartbeat
    // only the controller instance that sampled a real style this frame (+0x84 = 6/8/9)
    ldrb    w10, [x19, #0x84]
    sub     w11, w10, #6
    cbz     w11, 2f
    sub     w11, w10, #8
    cbz     w11, 2f
    sub     w11, w10, #9
    cbnz    w11, 1f
2:
    // the npad buttons this pass sampled (x24, the game's own copy it maps into this+0x28 at
    // main+0xed633c..0xed63dc; bit 6 = L) for the module's L-hold toggle; w11 holds the copy until
    // it is stored below, the virtual buttons use w10 only
    mov     w11, w24
    // dev.48: no virtual button while the game is not in play by the loop hook's last menu sample
    // (+0x6C: 1 a pause / map state, 2 no controller); the counters still run down in the loop hook
    ldr     w10, [x9, #0x6C]
    cbnz    w10, 6f
    // dev.45 L lock (the owner-approved virtual L hold): while +0x20 counts frames, add the game's L
    // bit (npad bit 6, mapped to this+0x28 bit 14 = 0x4000 at main+0xed6368..0xed6380) to the word the
    // sampler just stored, and report it in the copy as well, so the module sees the L the game sees.
    // dev.48: not during an event (EventManager state +0x248 non-zero, or no manager), like the
    // module's own LBlocked 3, without its few ticks of off-thread debounce
    ldr     w10, [x9, #0x20]
    cbz     w10, 7f
    adrp    x10, sshd_event_manager
    ldr     x10, [x10, :lo12:sshd_event_manager]
    cbz     x10, 7f
    ldr     w10, [x10, #0x248]
    cbnz    w10, 7f
    orr     w11, w11, #0x40
    ldr     w10, [x20]
    orr     w10, w10, #0x4000
    str     w10, [x20]
7:
    // shield bash (owner-approved virtual L3): while +0x18 counts frames, add the game's own
    // left-stick-click bit (npad bit 4, mapped to this+0x28 bit 16 at main+0xed63b4) to the word the
    // sampler just stored (x20 = this+0x28, written at main+0xed63dc)
    ldr     w10, [x9, #0x18]
    cbz     w10, 5f
    ldr     w10, [x20]
    orr     w10, w10, #0x10000
    str     w10, [x20]
5:
    // dev.47 owner-approved R3 (the sword stab): npad bit 5 maps to game bit 17 at main+0xed63bc
    ldr     w10, [x9, #0x24]
    cbz     w10, 8f
    ldr     w10, [x20]
    orr     w10, w10, #0x20000
    str     w10, [x20]
8:
    // put-away (dev.40): while +0x1C counts frames, add the game's B bit (0x400) the same way
    ldr     w10, [x9, #0x1C]
    cbz     w10, 6f
    ldr     w10, [x20]
    orr     w10, w10, #0x400
    str     w10, [x20]
6:
    str     w11, [x9, #0x14]
    // a stick sequence playing (+0x90 == 1) yields to the physical stick: native |x| or |y| of
    // 8192 (a quarter deflection) or more asks the loop hook to stop it (state 2) and the native
    // values pass through. Flag-free arithmetic (eor/sub/lsr), like the rest of this hook.
    ldr     w10, [x9, #0x90]
    sub     w10, w10, #1
    cbnz    w10, 4f
    eor     w10, w5, w5, asr #31
    sub     w10, w10, w5, asr #31   // |x|
    lsr     w10, w10, #13
    eor     w11, w25, w25, asr #31
    sub     w11, w11, w25, asr #31  // |y|
    orr     w10, w10, w11, lsr #13
    cbz     w10, 4f
    mov     w10, #2
    str     w10, [x9, #0x90]
    b       1f
4:
    ldr     w10, [x9, #8]
    cbz     w10, 1f                 // no live vector
    sub     w10, w10, #1
    str     w10, [x9, #8]
    ldr     w5, [x9, #12]
    ldr     w25, [x9, #16]
1:
    ldr     x11, [sp, #16]
    ldp     x9, x10, [sp], #32
    orr     w8, w25, w5             // the replaced instruction
    b       sshd_resume

    .globl sshd_loop_hook
    .p2align 2
sshd_loop_hook:
    adrp    x19, sshd_mailbox_base
    add     x19, x19, :lo12:sshd_mailbox_base
    ldr     w9, [x19, #0]
    movz    w10, #0x5353
    movk    w10, #0x4448, lsl #16
    cmp     w9, w10
    b.ne    9f                      // not configured
    ldr     w9, [x19, #0x68]
    add     w9, w9, #1
    str     w9, [x19, #0x68]        // loop heartbeat
    // dev.44 menu sample: the layout controller's state as this frame left it, then the stamp
    mov     w11, #2                 // 2: no controller (title, file select, a load)
    mov     w12, #0
    adrp    x10, sshd_lyt_control_game
    ldr     x10, [x10, :lo12:sshd_lyt_control_game]
    cbz     x10, 46f
    ldr     x10, [x10, #0x120]      // its current state ID (a static sFStateID_c)
    cbz     x10, 46f
    adrp    x12, sshd_state_normal
    add     x12, x12, :lo12:sshd_state_normal
    sub     x12, x10, x12           // 0 in StateID_Normal, else the state's distance from it
    cmp     x12, #0
    cset    w11, ne                 // 0 play, 1 a pause / map state
46:
    str     w11, [x19, #0x6C]
    str     w12, [x19, #0x74]
    add     x10, x19, #0x70
    stlr    w9, [x10]               // stamp = this frame's loop heartbeat, written last
    ldr     w9, [x19, #0x18]        // shield-bash L3 frames left: one per game frame
    cbz     w9, 41f
    sub     w9, w9, #1
    str     w9, [x19, #0x18]
41:
    ldr     w9, [x19, #0x1C]        // put-away B frames left (dev.40): one per game frame
    cbz     w9, 44f
    sub     w9, w9, #1
    str     w9, [x19, #0x1C]
44:
    ldr     w9, [x19, #0x20]        // L-hold frames left (dev.45): one per game frame, so a module
    cbz     w9, 45f                 // that stops refreshing it lets L go by itself
    sub     w9, w9, #1
    str     w9, [x19, #0x20]
45:
    ldr     w9, [x19, #0x24]        // sword-stab R3 frames left: expires even if the module stops
    cbz     w9, 8f
    sub     w9, w9, #1
    str     w9, [x19, #0x24]
8:
    ldr     w9, [x19, #0x200]       // top-HUD hide list ttl: one per game frame
    cbz     w9, 40f
    sub     w9, w9, #1
    str     w9, [x19, #0x200]
40:
    // draw probe: fold this frame's drawn / skipped bits into the frame counters
    ldr     x9, [x19, #0x410]
    cbz     x9, 42f
    ldr     w9, [x19, #0x418]
    str     wzr, [x19, #0x418]
    ldr     w10, [x19, #0x424]
    add     w10, w10, #1
    str     w10, [x19, #0x424]
    tbz     w9, #0, 43f
    ldr     w10, [x19, #0x41C]
    add     w10, w10, #1
    str     w10, [x19, #0x41C]
43:
    tbz     w9, #1, 42f
    ldr     w10, [x19, #0x420]
    add     w10, w10, #1
    str     w10, [x19, #0x420]
42:
    // --- stick sequence (spin attacks): one step vector per game frame through the stick hook
    ldr     w9, [x19, #0x90]        // state
    cbz     w9, 30f
    mov     w27, #1
    cmp     w9, #2
    b.eq    38f                     // the physical stick took over
    mov     w27, #5
    adrp    x10, sshd_event_manager
    ldr     x10, [x10, :lo12:sshd_event_manager]
    cbz     x10, 31f
    ldr     w11, [x10, #0x248]
    cbnz    w11, 38f                // an event started
31:
    mov     w27, #6
    adrp    x10, sshd_player
    ldr     x10, [x10, :lo12:sshd_player]
    cbz     x10, 38f
    mov     w11, #0x6404
    ldrh    w11, [x10, x11]
    cmp     w11, #SSHD_HELD_NONE
    ccmp    w11, #SSHD_HELD_SWORD, #4, ne // Z: empty hands or the sword
    b.ne    38f                     // Link took an item out
    ldr     w10, [x19, #0x98]       // frames left in this step
    cbnz    w10, 36f
    ldr     w11, [x19, #0x94]
    add     w11, w11, #1
    str     w11, [x19, #0x94]
    mov     w27, #0
    ldr     w12, [x19, #0x84]
    cmp     w11, w12
    b.hs    38f                     // played to the end
    add     x12, x19, #0xC4
    add     x12, x12, w11, uxtw #3
    ldr     w10, [x12]
    b       36f
30: // idle: a new request?
    add     x9, x19, #0x80
    ldar    w26, [x9]               // seq_req (pairs with the module's release store)
    ldr     w9, [x19, #0xA0]
    cmp     w26, w9
    b.eq    39f
    mov     w27, #3
    ldr     w9, [x19, #0x88]
    add     x10, x19, #0xFFC
    ldar    w10, [x10]
    cbz     w10, 37f
    cmp     w9, w10
    b.ne    37f                     // staged before a host invalidate
    mov     w27, #2
    ldr     w12, [x19, #0x84]       // steps
    sub     w9, w12, #1
    cmp     w9, #7
    b.hi    37f                     // 1..8 steps
    add     x13, x19, #0xC4
    mov     w11, #0
32:
    add     x10, x13, w11, uxtw #3
    ldr     w10, [x10]
    sub     w10, w10, #1
    cmp     w10, #119
    b.hi    37f                     // 1..120 frames a step
    add     w11, w11, #1
    cmp     w11, w12
    b.lo    32b
    mov     w27, #5
    adrp    x10, sshd_event_manager
    ldr     x10, [x10, :lo12:sshd_event_manager]
    cbz     x10, 33f
    ldr     w11, [x10, #0x248]
    cbnz    w11, 37f
33:
    mov     w27, #6
    adrp    x10, sshd_player
    ldr     x10, [x10, :lo12:sshd_player]
    cbz     x10, 37f
    mov     w11, #0x6404
    ldrh    w11, [x10, x11]
    cmp     w11, #SSHD_HELD_NONE
    ccmp    w11, #SSHD_HELD_SWORD, #4, ne
    b.ne    37f
    str     w26, [x19, #0x9C]       // playing this id
    str     wzr, [x19, #0x94]       // step 0
    str     wzr, [x19, #0xA8]
    ldr     w10, [x19, #0xC4]       // its frames
    mov     w9, #1
    str     w9, [x19, #0x90]
36: // emit the current step for the next frame's stick samples
    sub     w10, w10, #1
    str     w10, [x19, #0x98]
    ldr     w11, [x19, #0x94]
    add     x12, x19, #0xC0
    add     x12, x12, w11, uxtw #3
    ldrsh   w13, [x12]
    ldrsh   w14, [x12, #2]
    str     w13, [x19, #0x0C]
    str     w14, [x19, #0x10]
    mov     w13, #8
    str     w13, [x19, #8]          // ttl (stick passes), refreshed every frame
    ldr     w13, [x19, #0xA8]
    add     w13, w13, #1
    str     w13, [x19, #0xA8]       // frames played
    b       39f
37: // refused before it started
    str     w27, [x19, #0xA4]
    add     x9, x19, #0xA0
    stlr    w26, [x9]
    b       39f
38: // finished or stopped: native stick again
    str     wzr, [x19, #8]
    str     wzr, [x19, #0x90]
    str     w27, [x19, #0xA4]
    ldr     w9, [x19, #0x9C]
    add     x10, x19, #0xA0
    stlr    w9, [x10]
39:
    add     x9, x19, #0x40
    ldar    w20, [x9]               // req_seq (pairs with the module's release store)
    ldr     w9, [x19, #0x60]
    cmp     w20, w9
    b.eq    9f                      // nothing new
    mov     w21, #3
    ldr     w9, [x19, #0x50]
    add     x10, x19, #0xFFC
    ldar    w10, [x10]
    cbz     w10, 8f                 // epoch never seeded
    cmp     w9, w10
    b.ne    8f                      // staged before a host invalidate
    mov     w21, #4
    adrp    x22, sshd_file_manager
    ldr     x22, [x22, :lo12:sshd_file_manager]
    cbz     x22, 8f
    mov     w21, #5
    adrp    x9, sshd_event_manager
    ldr     x9, [x9, :lo12:sshd_event_manager]
    cbz     x9, 3f
    ldr     w10, [x9, #0x248]       // EventManager state, 0 = no event
    cbnz    w10, 8f
3:
    mov     w21, #2
    ldr     w9, [x19, #0x44]        // op
    ldr     w23, [x19, #0x48]       // a
    ldr     w24, [x19, #0x4C]       // b
    cmp     w9, #1
    b.eq    4f
    cmp     w9, #2
    b.eq    5f
    cmp     w9, #3
    b.eq    11f
    cmp     w9, #4
    b.eq    13f
    mov     w21, #1
    b       8f
4:  // SWAP_POUCH: a, b in 0..7, a != b
    cmp     w23, #7
    b.hi    8f
    cmp     w24, #7
    b.hi    8f
    cmp     w23, w24
    b.eq    8f
    add     x9, x22, #0x10          // current file (file A outside the title screen)
    add     x10, x9, #0x7C0         // pouch_items[8]
    ldr     w11, [x10, w23, uxtw #2]
    ldr     w12, [x10, w24, uxtw #2]
    str     w12, [x10, w23, uxtw #2]
    str     w11, [x10, w24, uxtw #2]
    add     x10, x9, #5, lsl #12
    add     x10, x10, #0x3B0        // +0x53B0 last used slot, +0x53B1 shield slot
    ldrb    w11, [x10, #1]
    cmp     w11, w23
    b.ne    6f
    strb    w24, [x10, #1]
    b       7f
6:
    cmp     w11, w24
    b.ne    7f
    strb    w23, [x10, #1]
7:
    ldrb    w11, [x10]
    cmp     w11, w23
    ccmp    w11, w24, #4, ne        // Z set if last == a or last == b
    b.ne    10f
    mov     w11, #8
    strb    w11, [x10]
10:
    mov     w21, #0
    b       8f
5:  // POUCH_STOCK: pouch a 0..7, stock b 0..59 -> native swapStockAndPouchItems(a, b)
    cmp     w23, #7
    b.hi    8f
    cmp     w24, #59
    b.hi    8f
    mov     w0, w23
    mov     w1, w24
    bl      sshd_swap_stock_and_pouch
    mov     w21, #0
    b       8f
11: // EQUIP_B: item a in 0..8 except 7 (not on the wheel)
    cmp     w23, #8
    b.hi    8f
    cmp     w23, #7
    b.eq    8f
    mov     w21, #6
    adrp    x25, sshd_player
    ldr     x25, [x25, :lo12:sshd_player]
    cbz     x25, 8f
    mov     w21, #10
    mov     w9, #0x63B5
    ldrb    w9, [x25, x9]           // dev.41: the native B / pouch wheel bits: open = its own commit first (10)
    cbnz    w9, 8f
    mov     w21, #6
    mov     w9, #0x6404
    ldrh    w9, [x25, x9]           // item in Link's hand
    cmp     w9, #SSHD_HELD_NONE
    b.eq    12f
    cmp     w9, #SSHD_HELD_SWORD
    b.ne    8f                      // another item in hand (6)
    mov     w21, #9
    ldr     w9, [x25, #0x468]       // action: the native wheel refuses the two sword attack states
    cmp     w9, #0x68
    b.eq    8f
    cmp     w9, #0x9a
    b.eq    8f
12:
    mov     w21, #7
    adrp    x9, sshd_bitem_flags
    add     x9, x9, :lo12:sshd_bitem_flags
    ldr     w26, [x9, w23, uxtw #2] // the item's ownership flag id
    mov     w0, w26
    bl      sshd_has_item
    cbz     w0, 8f
    mov     w0, w26
    bl      sshd_item_withheld
    tbnz    w0, #0, 8f
    mov     w9, #0x6408
    strh    w23, [x25, x9]          // selected B-item, as the wheel commit stores it
    mov     x0, x25
    mov     w1, w23
    bl      sshd_set_b_item
    mov     w21, #0
    b       8f
13: // SET_POUCH_R: a non-empty pouch slot a in 0..7
    cmp     w23, #7
    b.hi    8f
    add     x9, x22, #0x10          // current file
    add     x10, x9, #0x7C0
    add     x10, x10, w23, uxtw #2
    ldrh    w11, [x10]              // pouch item id
    cbz     w11, 8f
    add     x10, x9, #5, lsl #12
    strb    w23, [x10, #0x3B0]      // lastUsedPouchItemSlot
    mov     w21, #0
8:
    str     w21, [x19, #0x64]
    add     x9, x19, #0x60
    stlr    w20, [x9]               // res_seq last
9:
    adrp    x8, sshd_dsystem_page   // the replaced instruction
    b       sshd_loop_resume

    .globl sshd_draw_hook
    .p2align 2
sshd_draw_hook:
    // Pane::Draw entry (x0 = pane): skip the pane and its subtree when the live hide list holds it
    adrp    x9, sshd_mailbox_base
    add     x9, x9, :lo12:sshd_mailbox_base
    ldr     w10, [x9, #0x200]       // ttl: 0 = the list lapsed (or was never written)
    cbz     w10, 1f
    add     x11, x9, #0x204
    ldar    w10, [x11]              // select word (pairs with the module's release store)
    and     w11, w10, #0x100
    add     x11, x9, w11, uxtw      // bank 1 is 0x100 bytes after bank 0
    add     x11, x11, #0x210
    and     w10, w10, #0xFF         // count
    cbz     w10, 1f
    cmp     w10, #16
    b.hi    1f
2:
    ldr     x12, [x11]
    cmp     x12, x0
    b.eq    3f
4:
    add     x11, x11, #16
    subs    w10, w10, #1
    b.ne    2b
    b       1f
3:  // same pointer: the name must agree too (a freed pane's address reused by another pane)
    ldr     w12, [x11, #8]
    ldr     w13, [x0, #0xEC]
    cmp     w12, w13
    b.ne    4b
    ldr     w12, [x11, #12]
    ldr     w13, [x0, #0xF0]
    cmp     w12, w13
    b.ne    4b
    ldr     w10, [x9, #0x208]
    add     w10, w10, #1
    str     w10, [x9, #0x208]       // hits
    ldr     x12, [x9, #0x410]       // draw probe: skipped this frame
    cmp     x12, x0
    b.ne    5f
    ldr     w10, [x9, #0x418]
    orr     w10, w10, #2
    str     w10, [x9, #0x418]
5:
    ret                             // not drawn this frame
1:
    ldr     x12, [x9, #0x410]       // draw probe: drawn this frame (visible bit, effective alpha)
    cmp     x12, x0
    b.ne    6f
    ldrb    w10, [x0, #0xEB]
    tbz     w10, #0, 6f
    ldrb    w10, [x0, #0xE9]
    cbz     w10, 6f
    ldr     w10, [x9, #0x418]
    orr     w10, w10, #1
    str     w10, [x9, #0x418]
6:
    str     x21, [sp, #-0x30]!      // the replaced instruction
    b       sshd_draw_resume
