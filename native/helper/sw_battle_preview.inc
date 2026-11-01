// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only native move preview, published only from a genuine command-menu update.
// 8040B0 is the native Fight screen's Dynamax opportunity query. 845420 maps a BASE move
// to its ordinary Max move; 8454F0 reads that base move's native Max power. 865A90 supplies
// a separate Gigantamax message index (not a normal move id), using the exact species/form/
// factor inputs of native UI adapter8C7390. Actual actions continue encoding the BASE id.
// B4 is released last, matching the menu beat at08. A reader rejects partial/stale samples.
// x19 client, x20 mailbox. Preserves all callee-saved registers.
sw_battle_preview:
    stp x29, x30, [sp, #-0x60]!
    stp x21, x22, [sp, #0x10]
    stp x23, x24, [sp, #0x20]
    stp x25, x26, [sp, #0x30]
    mov x29, sp
    str wzr, [x20, #0xB4]
    str wzr, [x20, #0xB0]
    str wzr, [x20, #0xF8]
    ldr w9, [x20, #0x38]
    ubfx w10, w9, #16, #8             // communication: offline only
    cbnz w10, bpreview_done
    ubfx w10, w9, #8, #8              // wild or trainer
    cmp w10, #1
    b.hi bpreview_done
    and w9, w9, #0xff
    cbz w9, bpreview_supported
    cmp w9, #2
    b.ne bpreview_done
    cbnz w10, bpreview_done            // verified offline wild raid only
    ldr x9, [x19, #0x58]
    ldr x9, [x9, #0x88]
    ldrb w9, [x9, #0x3B9]
    cmp w9, #5
    b.ne bpreview_done
bpreview_supported:
    ldr x21, [x19, #0x78]
    cbz x21, bpreview_done
    ldrb w9, [x21, #0x33C]
    cmp w9, #4
    b.hi bpreview_done
    str x21, [x20, #0xE8]
    str x19, [x20, #0xF0]
    str wzr, [sp, #0x40]
    mov x0, x19
    add x1, sp, #0x40
    mov x2, x21
    bl sw_dmax_opportunity
    ldr w9, [sp, #0x40]
    and w9, w9, #0x00ffffff
    ldrb w10, [x21, #0x298]
    orr w9, w9, w10, lsl #24
    str w9, [x20, #0xB0]
    mov x0, x21
    bl sw_battle_species
    mov w23, w0
    mov x0, x21
    bl sw_battle_gmax_factor
    mov w24, w0
    ldrb w25, [x21, #0x33D]
    mov w22, wzr
bpreview_move:
    mov w9, #-1
    add x10, x20, #0xD8
    str w9, [x10, x22, lsl #2]
    add x10, x20, #0xB8
    str wzr, [x10, x22, lsl #2]
    add x10, x20, #0xC8
    str wzr, [x10, x22, lsl #2]
    add x10, x20, #0x188
    str wzr, [x10, x22, lsl #2]
    ldrb w9, [x21, #0x33C]
    cmp w22, w9
    b.hs bpreview_next
    mov x0, x21
    mov w1, w22
    bl sw_get_waza
    cbz w0, bpreview_next
    mov w26, w0
    add x10, x20, #0x188
    str w26, [x10, x22, lsl #2]
    mov x0, x19
    mov x1, x21
    mov w2, w26
    bl sw_raid_target
    cmp w0, #5
    b.hs 1f
    mov w9, #1
    lsl w9, w9, w22
    ldr w10, [x20, #0xF8]
    orr w10, w10, w9
    str w10, [x20, #0xF8]
1:  mov w0, w26
    bl sw_max_move
    add x10, x20, #0xB8
    str w0, [x10, x22, lsl #2]
    mov w2, w0
    mov x0, x19
    mov x1, x21
    bl sw_raid_target
    cmp w0, #5
    b.hs 2f
    mov w9, #0x10
    lsl w9, w9, w22
    ldr w10, [x20, #0xF8]
    orr w10, w10, w9
    str w10, [x20, #0xF8]
2:
    mov w0, w26
    bl sw_max_power
    add x10, x20, #0xC8
    str w0, [x10, x22, lsl #2]
    // Exact native UI adapter also handles Morpeko's Hangry/Aura Wheel exception.
    // A G-Max result uses its separate message index; retain the generic Max id for metadata.
    mov x0, xzr                        // unused by native8C7390
    add x1, sp, #0x44
    add x2, sp, #0x48
    mov w3, w23
    mov w4, w25
    mov w5, #1
    mov w6, w24
    mov w7, w26
    bl sw_max_preview_adapter
    ldr w0, [sp, #0x48]
    add x10, x20, #0xD8
    str w0, [x10, x22, lsl #2]
    ldr w9, [sp, #0x44]
    cbz w9, bpreview_next
    add x10, x20, #0xB8
    str w9, [x10, x22, lsl #2]
bpreview_next:
    add w22, w22, #1
    cmp w22, #4
    b.lo bpreview_move
    ldr w9, [x20, #0x08]
    add x10, x20, #0xB4
    stlr w9, [x10]
bpreview_done:
    ldp x25, x26, [sp, #0x30]
    ldp x23, x24, [sp, #0x20]
    ldp x21, x22, [sp, #0x10]
    ldp x29, x30, [sp], #0x60
    ret
