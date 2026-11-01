// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pokemon Sword 1.3.2 guest helper -- format-2 load-plan code (battle commands: phase 1 FIGHT/RUN,
// phase 2 SWITCH/ITEM + the battle-alive beat, see op_switch/op_item/sw_alive_entry; the battle
// HP-plate HUD hide, see sw_hud_entry).
// Title 0100ABF008968000, main build A3B75BCD3311385AEED67FBEEB79CBB7BF02F471.
//
// Hook site: the battle client's "select action: wait for the command menu" step,
// main+0x802DF0 (state main+0x8021D0, substep 5; the state machine is the client's own
// member-function pointer at client+0x560, substep int at client+0x570). The game's own
// thread calls it exactly once per frame while the FIGHT/POKEMON/BAG/RUN menu is up and
// waiting for input, and never outside that window -- so the idle cost of this helper is
// zero outside battle and a handful of loads/stores per frame while the menu waits.
// Entry instruction `stp x22, x21, [sp, #-0x30]!` (f657bda9) is replaced with
// `b sw_guest_helper_entry`; the helper re-executes it first ("reproduce and resume",
// the Belmont helper's Variant A).
//
// What a request does (all addresses verified live 2026-10-26 with breakpoints and
// write-watchpoints on the desktop harness):
//   FIGHT slot k -- the same native steps the game takes after the player picks a move:
//     legality through the game's own checks (move count client-mon+0x33C, move id
//     main+0x7E2240, PP main+0x7E2480, "move blocked" main+0x8041B0 -- the check the
//     native FIGHT step uses for Struggle/Disable/Taunt/Choice), then the command UI's own
//     force-quit (view vtable +0xC8, the path the battle timer uses), the game's own
//     SetFightParam (main+0x818500, target 5 = "resolve later", as the move UI writes it)
//     on the client's action slot, and the client's FIGHT state (main+0x803590) at
//     substep 4 -- the step that follows the move UI's decision: native target
//     resolution, then the native finish/send to the battle server.
//   RUN -- force-quit the command UI, then continue inside main+0x802DF0 exactly as the
//     game does when the menu reports command 4 (RUN): its own escape checks, messages
//     and state change (main+0x802ED8 onward). Only offered for the first position.
//
// Mailbox layout: native/modules/sw_helper_mailbox.h (single source of truth; every
// offset below is a literal immediate and must match it).

.text
.global sw_guest_helper_entry
sw_guest_helper_entry:
    stp  x22, x21, [sp, #-0x30]!        // reproduced entry instruction

    sub  sp, sp, #0x70
    stp  x0,  x1,  [sp, #0x00]
    stp  x19, x20, [sp, #0x10]
    stp  x23, x24, [sp, #0x20]
    stp  x25, x26, [sp, #0x30]
    stp  x29, x30, [sp, #0x40]
    stp  x27, x28, [sp, #0x50]          // +0x60..0x6F: scratch (trap check out-params)

    mov  x19, x0                        // client
    adrp x20, sw_mailbox_base
    add  x20, x20, :lo12:sw_mailbox_base

    // --- per-frame "menu waiting" publication ---
    movz w9, #0x5753                    // 'SWG1'
    movk w9, #0x3147, lsl #16
    str  w9, [x20, #0x04]
    ldr  w9, [x20, #0x08]
    add  w9, w9, #1
    str  w9, [x20, #0x08]               // wait_beat
    ldrb w9, [x19, #0x348]
    ldrb w10, [x19, #0x34b]
    orr  w9, w9, w10, lsl #8
    str  w9, [x20, #0x0C]               // pos_info
    str  x19, [x20, #0x10]              // client
    ldr  x9, [x19, #0x78]
    str  x9, [x20, #0x18]               // current mon
    bl   battle_kind
    str  w0, [x20, #0x38]               // battle_kind (review H2/M4)
    ldr  w9, [x20, #0xA4]
    str  w9, [x20, #0xFC]
    bl   sw_battle_preview

    // --- request? ---
    ldr  w9, [x20, #0x00]
    movz w10, #0x5753                   // 'SWM1'
    movk w10, #0x314D, lsl #16
    cmp  w9, w10
    b.ne passthrough
    add  x9, x20, #0x20
    ldar w23, [x9]                      // req_seq (pairs with the module's release store)
    ldr  w9, [x20, #0x40]               // ack_seq (only this helper writes it)
    cmp  w23, w9
    b.eq passthrough
    add  x9, x20, #0x34
    ldar w9, [x9]                       // cancel_seq
    cmp  w9, w23
    b.eq refuse_cancelled
    ldr  w9, [x20, #0x30]               // req_epoch
    add  x10, x20, #0xFFC
    ldar w10, [x10]                     // host/module epoch word
    cbz  w10, refuse_stale_epoch
    cmp  w9, w10
    b.ne refuse_stale_epoch

    // The hook only runs from substep 5 of this state; check anyway (fail closed).
    ldr  x9, [x19, #0x560]
    adrp x10, sw_state_select_action
    add  x10, x10, :lo12:sw_state_select_action
    cmp  x9, x10
    b.ne refuse_not_waiting
    ldr  x9, [x19, #0x88]               // action slot
    cbz  x9, refuse_not_waiting
    ldr  x24, [x19, #0x78]              // mon
    cbz  x24, refuse_not_waiting
    ldr  x9, [x19, #0x358]              // view
    cbz  x9, refuse_not_waiting

    ldr  w9, [x20, #0x24]               // req_op
    cmp  w9, #1
    b.eq op_fight_kind
    cmp  w9, #5
    b.eq op_fight_kind
    ldr  w10, [x20, #0x38]
    tbz  w10, #31, refuse_battle_type   // Run/Switch/Item remain offline singles only
    cmp  w9, #2
    b.eq op_run
    cmp  w9, #3
    b.eq op_switch
    cmp  w9, #4
    b.eq op_item
    cmp  w9, #6
    b.eq op_open_menu
    cmp  w9, #7
    b.eq op_open_menu
    cmp  w9, #8
    b.eq op_decide
    b    refuse_bad_op

// ---------------------------------------------------------------------------
op_fight_kind:
    ldr  w9, [x20, #0x38]
    tbz  w9, #31, fight_raid_kind
    ldr  w9, [x20, #0x24]
    cmp  w9, #5
    b.eq refuse_battle_type             // explicit Max action is verified only in offline Raid
    b    op_fight
fight_raid_kind:
    cmp  w9, #2                        // exact offline wild raid; no institution/link modes
    b.ne refuse_battle_type
    ldrb w9, [x19, #0x2A6]
    cbnz w9, refuse_battle_type
    ldr  x9, [x19, #0x58]
    ldr  x9, [x9, #0x88]
    ldrb w9, [x9, #0x3B9]
    cmp  w9, #5
    b.ne refuse_battle_type
op_fight:
    mov  x0, x24
    bl   sw_is_dead
    tbnz w0, #0, refuse_fainted
    ldr  w25, [x20, #0x28]              // slot 0..3
    cmp  w25, #3
    b.hi refuse_bad_arg
    ldrb w9, [x24, #0x33c]              // move count
    cmp  w25, w9
    b.hs refuse_bad_arg
    mov  x0, x24
    mov  w1, w25
    bl   sw_get_waza                    // u32 move id at mon+0x2D4+k*0x1C
    mov  w26, w0
    cbz  w26, refuse_no_move
    ldr  w9, [x20, #0x2C]               // expected move id (module's own reader)
    cmp  w9, w26
    b.ne refuse_mismatch
    mov  x0, x24
    mov  w1, w25
    bl   sw_get_pp
    tst  w0, #0xffff
    b.eq refuse_no_pp
    mov  w27, wzr
    ldrb w9, [x24, #0x298]
    cbnz w9, fight_max_enabled
    ldr  w9, [x20, #0x24]
    cmp  w9, #5
    b.ne fight_effective
    str  wzr, [sp, #0x60]
    mov  x0, x19
    add  x1, sp, #0x60
    mov  x2, x24
    bl   sw_dmax_opportunity
    ldrb w9, [sp, #0x60]
    cmp  w9, #1
    b.ne refuse_dmax
fight_max_enabled:
    mov  w27, #1
fight_effective:
    mov  w28, w26
    cbz  w27, fight_block_check
    mov  w0, w26
    bl   sw_max_move
    mov  w28, w0
fight_block_check:
    mov  x0, x19
    mov  x1, x24
    mov  w2, w28                        // native Fight checks the effective Max move
    mov  x3, xzr
    bl   sw_is_waza_blocked
    tbnz w0, #0, refuse_blocked
    ldr  w9, [x20, #0x38]
    and  w9, w9, #0xff
    cmp  w9, #2
    b.ne fight_single_target
    mov  x0, x19
    mov  x1, x24
    mov  w2, w28
    bl   sw_raid_target
    cmp  w0, #5
    b.hs refuse_target
    mov  w28, w0
    // Same final special-case refusal as native Fight after target selection (Pollen Puff).
    mov  x0, x19
    mov  x1, x24
    mov  w2, w26
    mov  w3, w28
    mov  x4, xzr
    bl   sw_selected_target_blocked
    tbnz w0, #0, refuse_blocked
    b    fight_commit
fight_single_target:
    mov  w28, #5
fight_commit:

    ldr  x0, [x19, #0x358]
    ldr  x8, [x0]
    ldr  x8, [x8, #0xc8]
    blr  x8                             // command UI force-quit

    ldr  x0, [x19, #0x88]
    ldrb w1, [x24, #0x7d]               // pokeID
    mov  w2, w26
    mov  w3, w28
    mov  w4, wzr
    mov  w5, wzr
    bl   sw_set_fight_param
    cbz  w27, 1f
    ldr  x9, [x19, #0x88]
    ldr  x10, [x9]
    orr  x10, x10, #0x80000000
    str  x10, [x9]                      // exact native94A668 bit31, including already-active turns
1:

    adrp x9, sw_state_fight
    add  x9, x9, :lo12:sw_state_fight
    str  x9, [x19, #0x560]
    str  xzr, [x19, #0x568]
    mov  w9, #4
    cmp  w28, #5
    mov  w10, #9
    csel w9, w9, w10, eq                // validated raid target skips native target-picker UI
    str  w9, [x19, #0x570]

    str  w26, [x20, #0x48]              // result_value = move id
commit_return:
    mov  w0, #0
    bl   acknowledge
    // Return from main+0x802DF0 the way it returns on every path: w0 = 0.
    ldp  x0,  x1,  [sp, #0x00]
    ldp  x19, x20, [sp, #0x10]
    ldp  x23, x24, [sp, #0x20]
    ldp  x25, x26, [sp, #0x30]
    ldp  x29, x30, [sp, #0x40]
    ldp  x27, x28, [sp, #0x50]
    add  sp, sp, #0x70
    ldp  x22, x21, [sp], #0x30
    mov  w0, wzr
    ret

// ---------------------------------------------------------------------------
// Native main802EA0/802EBC tests condition0x22 before choosing the Bag/Pokemon
// state, then main803098 clears both native state counters. The picker itself
// retains the game's legality, item/party selection and B/cancel behavior.
op_open_menu:
    ldrb w9, [x19, #0x348]
    ldrb w10, [x19, #0x34b]
    cmp  w9, w10
    b.ne refuse_not_first
    mov  x0, x24
    mov  w1, #0x22
    bl   sw_mon_condition
    tbnz w0, #0, refuse_blocked
    ldr  w25, [x20, #0x24]
    ldr  x0, [x19, #0x358]
    ldr  x8, [x0]
    ldr  x8, [x8, #0xc8]
    blr  x8
    cmp  w25, #6
    b.ne 1f
    adrp x9, sw_state_bag
    add  x9, x9, :lo12:sw_state_bag
    b    2f
1:  adrp x9, sw_state_pokemon
    add  x9, x9, :lo12:sw_state_pokemon
2:  str  x9, [x19, #0x560]
    str  xzr, [x19, #0x568]
    str  wzr, [x19, #0x570]
    str  w25, [x20, #0x48]
    b    commit_return

// ---------------------------------------------------------------------------
// DECIDE (2026-10-31, owner dev.23 "bottom taps drive the top like a real press"): store
// req_arg (1 FIGHT, 2 BAG; live dev24: 3 is the Pokemon list) as the command UI's decided command -- UI_obj+0x50, the value the
// command view's own A press writes and that main+0x802DF0 reads every frame through view
// vtable +0xC0 (Native99E6E0: *(*(view+0x5C8)+0x38) -> main+0x9A4F60 -> +8 -> +0x50) -- then
// fall through into the native function, which dispatches it this same frame exactly as after
// the A press: FIGHT opens the native move list, BAG the native bag (its own checks/messages).
op_decide:
    ldr  w25, [x20, #0x28]
    cmp  w25, #1
    b.eq 1f
    cmp  w25, #2
    b.ne refuse_bad_arg
1:  ldr  x0, [x19, #0x358]
    ldr  x9, [x0]
    ldr  x9, [x9, #0xc0]                // the decided-command getter must be Native99E6E0
    adrp x10, sw_decided_getter
    add  x10, x10, :lo12:sw_decided_getter
    cmp  x9, x10
    b.ne refuse_not_waiting
    ldr  x8, [x0, #0x5c8]
    cbz  x8, refuse_not_waiting
    ldr  x0, [x8, #0x38]
    cbz  x0, refuse_not_waiting
    bl   sw_ui_slot_lookup
    cbz  x0, refuse_not_waiting
    ldr  w9, [x0, #0x58]                // UI_obj(+8)+0x50: 0 = nothing decided yet
    cbnz w9, refuse_not_waiting
    str  w25, [x0, #0x58]
    // Close the command menu the way the native decide does before the next screen opens
    // (view vtable +0xC8, the same close op_open_menu uses); live dev24: without it the FIGHT
    // state waited behind a still-open command menu.
    ldr  x0, [x19, #0x358]
    ldr  x8, [x0]
    ldr  x8, [x8, #0xc8]
    blr  x8
    str  w25, [x20, #0x48]
    mov  w0, #0
    bl   acknowledge
    b    passthrough

op_run:
    ldrb w9, [x19, #0x348]
    ldrb w10, [x19, #0x34b]
    cmp  w9, w10
    b.ne refuse_not_first
    ldr  x0, [x19, #0x358]
    ldr  x8, [x0]
    ldr  x8, [x8, #0xc8]
    blr  x8                             // command UI force-quit
    str  wzr, [x20, #0x48]
    mov  w0, #0
    bl   acknowledge
    ldp  x0,  x1,  [sp, #0x00]
    ldp  x19, x20, [sp, #0x10]
    ldp  x23, x24, [sp, #0x20]
    ldp  x25, x26, [sp, #0x30]
    ldp  x29, x30, [sp, #0x40]
    ldp  x27, x28, [sp, #0x50]
    add  sp, sp, #0x70
    // Finish main+0x802DF0's own prologue (0x802DF4..0x802E08), then take its
    // "menu reported command 4" branch with the same registers it would have.
    stp  x20, x19, [sp, #0x10]
    stp  x29, x30, [sp, #0x20]
    add  x29, sp, #0x20
    mov  x20, x1
    mov  x19, x0
    b    sw_native_run_branch


// ---------------------------------------------------------------------------
// SWITCH to the party member whose pokeID is req_arg (phase 2). Mirrors the Pokemon state
// (main+0x8032C0): the party screen's "cannot switch" flag computed exactly as its substep 0 does
// (held-item context main+0x7E2D30 == Shed Shell skips it; trap check main+0x805070 returns 5 when
// free; the rule/alive-count check main+0x7F8110 / main+0x8141D0), the party screen's own refusals
// for a fainted member (IsDead main+0x7E1600) and the battler already out (battle-party slot 0 /
// the client's current mon), then SetChangeParam (main+0x818630, position, battle-party index)
// and the client's finish state (main+0x8030C0) -- the state the native party screen hands over to.
op_switch:
    ldrb w9, [x19, #0x348]
    ldrb w10, [x19, #0x34b]
    cmp  w9, w10
    b.ne refuse_not_first
    ldr  w25, [x20, #0x28]              // target pokeID
    cmp  w25, #5
    b.hi refuse_bad_arg
    bl   find_member                    // x21 = bpp, w22 = battle-party index
    cbz  x21, refuse_not_in_party
    cbz  w22, refuse_active
    cmp  x21, x24
    b.eq refuse_active
    mov  x0, x21
    bl   sw_is_dead
    tbnz w0, #0, refuse_fainted
    bl   trap_check
    cbnz w0, refuse_trapped

    ldr  x0, [x19, #0x358]
    ldr  x8, [x0]
    ldr  x8, [x8, #0xc8]
    blr  x8                             // command UI force-quit
    ldr  x0, [x19, #0x88]
    ldrb w1, [x19, #0x348]
    mov  w2, w22
    bl   sw_set_change_param
    bl   enter_finish
    str  w25, [x20, #0x48]              // result_value = pokeID
    b    commit_return

// ---------------------------------------------------------------------------
// ITEM req_arg on pokeID req_expect (medicine) -- mirrors the Bag state (main+0x8039C0): its
// substep-0 "items usable in this battle" checks (flag 0x800 via main+0x7F35C0, rule/alive count),
// the ball check main+0x8048A0 for balls (main+0x7EC410), the medicine check for the target, then
// SetItemParam (main+0x818600: user pokeID, item, target pokeID), the ball-thrown flag
// (client+0x2B5 |= 4 when the item's param 0x12 is 5) and the per-client reserved-item record
// (client+0x292/+0x294) exactly as main+0x803CC4..0x803D20 do, and the finish state.
op_item:
    ldrb w9, [x19, #0x348]
    ldrb w10, [x19, #0x34b]
    cmp  w9, w10
    b.ne refuse_not_first
    ldr  w25, [x20, #0x28]              // item id
    cbz  w25, refuse_bad_arg
    cmp  w25, #0x7ff
    b.hi refuse_bad_arg
    ldr  w26, [x20, #0x2C]              // target pokeID (medicine)
    ldr  x0, [x19, #0x58]
    mov  w1, #0x800
    bl   sw_battle_flag
    tbnz w0, #0, refuse_items_blocked
    ldr  x0, [x19, #0x58]
    bl   sw_battle_rule
    cmp  w0, #2
    b.ne 1f
    ldr  x0, [x19, #0x340]
    bl   sw_party_alive_count
    tst  w0, #0xff
    b.eq refuse_items_blocked
1:  mov  w0, w25
    bl   sw_item_is_ball
    tbz  w0, #0, 2f
    mov  x0, x19
    add  x1, x19, #0x574
    mov  x2, xzr
    ldrb w3, [x19, #0x348]
    bl   sw_ball_check
    tbz  w0, #0, refuse_ball
    ldrb w26, [x24, #0x7d]              // balls: target = the user (as the bag screen's slot 0)
    b    3f
2:  lsr  w9, w26, #8                   // bit 8: the module's "restores a fainted member" flag
    str  w9, [sp, #0x68]
    and  w26, w26, #0xff
    cmp  w26, #5
    b.hi refuse_bad_arg
    mov  w27, w25                       // find_member uses w25 as the pokeID
    mov  w25, w26
    bl   find_member
    mov  w25, w27
    cbz  x21, refuse_not_in_party
    // The bag screen's fainted split: medicine only on a member that can battle, a revive only
    // on a fainted one (the game's IsDead, main+0x7E1600). HP-full/status cases are refused by
    // the module before staging (the game's per-target check has no standalone entry).
    mov  x0, x21
    bl   sw_is_dead
    and  w0, w0, #1
    ldr  w9, [sp, #0x68]
    and  w9, w9, #1
    cmp  w0, w9
    b.ne refuse_no_effect
3:
    ldr  x0, [x19, #0x358]
    ldr  x8, [x0]
    ldr  x8, [x8, #0xc8]
    blr  x8                             // command UI force-quit
    ldr  x0, [x19, #0x88]
    ldrb w1, [x24, #0x7d]
    mov  w2, w25
    mov  w3, w26
    mov  w4, wzr
    bl   sw_set_item_param
    mov  w0, w25
    mov  w1, #0x12
    bl   sw_item_param
    cmp  w0, #5
    b.ne 4f
    ldrb w9, [x19, #0x2b5]
    orr  w9, w9, #4
    strb w9, [x19, #0x2b5]
4:  ldrb w9, [x19, #0x2aa]
    cbnz w9, 5f
    ldr  x0, [x19, #0x58]
    bl   sw_battle_item_client
    ldrb w9, [x19, #0x2a6]
    cmp  w9, w0, uxtb
    b.ne 5f
    ldr  x0, [x19, #0x58]
    ldrb w1, [x19, #0x2a6]
    mov  w2, w25
    bl   sw_battle_item_reserve
    tbz  w0, #0, 5f
    ldrb w9, [x19, #0x348]
    add  x9, x19, x9, lsl #2
    strh w25, [x9, #0x292]
    strb wzr, [x9, #0x294]
5:  bl   enter_finish
    str  w25, [x20, #0x48]              // result_value = item id
    b    commit_return

// find_member: w25 = pokeID -> x21 = bpp in the client's battle party (client+0x340, entries via
// main+0x814330, count at +0x30), w22 = its index; x21 = 0 when absent. Clobbers x0/x1/x9.
find_member:
    stp  x29, x30, [sp, #-0x10]!
    mov  x21, xzr
    mov  w22, wzr
    ldr  x9, [x19, #0x340]
    cbz  x9, 9f
    mov  x28, x9
6:  ldrb w9, [x28, #0x30]
    cmp  w22, w9
    b.hs 8f
    mov  x0, x28
    mov  w1, w22
    bl   sw_party_get
    cbz  x0, 7f
    ldrb w9, [x0, #0x7d]
    cmp  w9, w25
    b.ne 7f
    mov  x21, x0
    b    9f
7:  add  w22, w22, #1
    b    6b
8:  mov  x21, xzr
9:  ldp  x29, x30, [sp], #0x10
    ret

// trap_check: the Pokemon state's substep-0 "cannot switch" flag for the current mon (x24).
// Returns w0 = 1 when trapped. Uses the frame scratch at sp+0x60 (caller's frame: +0x10 here).
trap_check:
    stp  x29, x30, [sp, #-0x10]!
    mov  w27, wzr
    mov  x0, x24
    ldr  x1, [x19, #0xe0]
    bl   sw_item_context
    and  w9, w0, #0xffff
    cmp  w9, #0x127                     // Shed Shell
    b.eq 2f
    mov  w9, #0x1f
    strb w9, [sp, #0x70]
    strh wzr, [sp, #0x74]
    mov  x0, x19
    mov  x1, x24
    add  x2, sp, #0x70
    add  x3, sp, #0x74
    bl   sw_trap_check
    cmp  w0, #5
    b.eq 2f
    and  w9, w0, #0xff
    cmp  w9, #5
    b.ne 1f
    ldrb w9, [sp, #0x70]
    ldrb w10, [x24, #0x7d]
    cmp  w9, w10
    b.ne 2f
1:  mov  w27, #1
2:  ldr  x0, [x19, #0x58]
    bl   sw_battle_rule
    cmp  w0, #2
    b.ne 3f
    ldr  x0, [x19, #0x340]
    bl   sw_party_alive_count
    tst  w0, #0xff
    b.ne 3f
    mov  w27, #1
3:  mov  w0, w27
    ldp  x29, x30, [sp], #0x10
    ret

// enter_finish: client state = finish (main+0x8030C0), adj 0, substep 0. Leaf.
enter_finish:
    adrp x9, sw_state_finish
    add  x9, x9, :lo12:sw_state_finish
    str  x9, [x19, #0x560]
    str  xzr, [x19, #0x568]
    str  wzr, [x19, #0x570]
    ret

// battle_kind: w0 = the battle's kind from its setup param (static RE 2026-10-26,
// analysis/20261026-review-fixes-re): main module = [client+0x58]; rule = [main+0x150] (the value
// main+0x7F8110 returns: 0 single, 1 double, 2 raid); setup = [main+0x88]; competitor = [setup+0]
// (main+0x7F3790: 0 wild, 1 trainer, 2 institution, 3 link/online); comm mode = byte
// [setup+0x3B8] (main+0x7F2B20: 0 offline). Bits 0-7 rule, 8-15 competitor, 16-23 comm mode,
// bit 31 set only for rule 0 + competitor 0/1 + comm mode 0. Leaf: x0, x9-x12.
battle_kind:
    mov  w0, wzr
    ldr  x9, [x19, #0x58]
    cbz  x9, 1f
    ldr  w10, [x9, #0x150]              // rule
    ldr  x9, [x9, #0x88]                // setup param
    cbz  x9, 1f
    ldr  w11, [x9]                      // competitor
    ldrb w12, [x9, #0x3b8]              // comm mode
    and  w0, w10, #0xff
    and  w9, w11, #0xff
    orr  w0, w0, w9, lsl #8
    orr  w0, w0, w12, lsl #16
    cbnz w10, 1f
    cmp  w11, #1
    b.hi 1f
    cbnz w12, 1f
    orr  w0, w0, #0x80000000
1:  ret

// ---------------------------------------------------------------------------
// acknowledge: w0 = result code. result_value must already be stored.
// Leaf: touches x9/x10 only.
acknowledge:
    str  w0, [x20, #0x44]
    ldr  w9, [x20, #0x08]
    str  w9, [x20, #0x4C]               // ack_beat
    add  x10, x20, #0x40
    stlr w23, [x10]                     // ack_seq = req_seq, release
    ret

refuse_cancelled:    mov w0, #4;  b refuse
refuse_stale_epoch:  mov w0, #3;  b refuse
refuse_not_waiting:  mov w0, #5;  b refuse
refuse_bad_op:       mov w0, #2;  b refuse
refuse_bad_arg:      mov w0, #6;  b refuse
refuse_no_move:      mov w0, #7;  b refuse
refuse_mismatch:     mov w0, #8;  b refuse
refuse_no_pp:        mov w0, #9;  b refuse
refuse_blocked:      mov w0, #10; b refuse
refuse_not_first:    mov w0, #11; b refuse
refuse_trapped:      mov w0, #12; b refuse
refuse_fainted:      mov w0, #13; b refuse
refuse_active:       mov w0, #14; b refuse
refuse_not_in_party: mov w0, #15; b refuse
refuse_ball:         mov w0, #16; b refuse
refuse_no_effect:    mov w0, #17; b refuse
refuse_items_blocked: mov w0, #18; b refuse
refuse_battle_type:  mov w0, #19; b refuse
refuse_dmax:         mov w0, #30; b refuse
refuse_target:       mov w0, #31; b refuse

refuse:
    str  wzr, [x20, #0x48]
    bl   acknowledge

passthrough:
    ldp  x0,  x1,  [sp, #0x00]
    ldp  x19, x20, [sp, #0x10]
    ldp  x23, x24, [sp, #0x20]
    ldp  x25, x26, [sp, #0x30]
    ldp  x29, x30, [sp, #0x40]
    ldp  x27, x28, [sp, #0x50]
    add  sp, sp, #0x70
    b    sw_resume                      // main+0x802DF4

// ---------------------------------------------------------------------------
// Second hook (phase 2): the battle client's per-frame update, main+0x7FCE10 (x0 = client; called
// every frame for the whole battle from the battle main loop -- command menu, turn animation,
// messages, the native party/bag screens). Entry `str x28, [sp, #-0x50]!` (fc0f1bf8) is
// reproduced. Publishes guest magic, alive_beat++ and the client object -- only for the client
// that owns a view (client+0x358, the player's; AI clients have none). Uses x16/x17 only (dead at
// a function entry), so nothing else is saved -- except a pending cursor request (alive_cursor),
// which saves x0-x8/x19-x22/x29/x30 around its native call.
.global sw_alive_entry
sw_alive_entry:
    str  x28, [sp, #-0x50]!             // reproduced entry instruction
    ldr  x16, [x0, #0x358]
    cbz  x16, 1f
    adrp x16, sw_mailbox_base
    add  x16, x16, :lo12:sw_mailbox_base
    movz w17, #0x5753                   // 'SWG1'
    movk w17, #0x3147, lsl #16
    str  w17, [x16, #0x04]
    ldr  w17, [x16, #0x50]
    add  w17, w17, #1
    str  w17, [x16, #0x50]              // alive_beat
    str  x0, [x16, #0x58]               // alive_client
    // Fresh kind throughout turn animation/intro. HUD hiding must never inherit a previous
    // single battle's verdict during an unsupported double/link battle. Only x16/x17 are used;
    // NZCV is dead at this entry before the native first BL.
    ldr  x17, [x0, #0x58]
    cbz  x17, alive_kind_unknown
    ldr  w17, [x17, #0x150]
    cmp  w17, #0xff
    b.hi alive_kind_unknown
    str  w17, [x16, #0x38]
    ldr  x17, [x0, #0x58]
    ldr  x17, [x17, #0x88]
    cbz  x17, alive_kind_unknown
    ldr  w17, [x17]
    cmp  w17, #0xff
    b.hi alive_kind_unknown
    and  w17, w17, #0xff
    ldr  w16, [x16, #0x38]
    and  w16, w16, #0xff
    orr  w17, w16, w17, lsl #8
    adrp x16, sw_mailbox_base
    add  x16, x16, :lo12:sw_mailbox_base
    str  w17, [x16, #0x38]
    ldr  x17, [x0, #0x58]
    ldr  x17, [x17, #0x88]
    ldrb w17, [x17, #0x3B8]
    ldr  w16, [x16, #0x38]
    orr  w17, w16, w17, lsl #16
    and  w16, w17, #0xff
    cbnz w16, alive_kind_store
    cmp  w17, #0x100
    b.hi alive_kind_store
    orr  w17, w17, #0x80000000
    b    alive_kind_store
alive_kind_unknown:
    mov  w17, wzr
alive_kind_store:
    adrp x16, sw_mailbox_base
    add  x16, x16, :lo12:sw_mailbox_base
    str  w17, [x16, #0x38]
    ldr  w17, [x16, #0xA4]
    str  w17, [x16, #0xFC]
    // Cursor channel (2026-10-31, bottom -> top cursor sync): a pending request moves the
    // NATIVE move-list cursor while the FIGHT state waits for input. Nothing is committed: the
    // list's own "cursor moved" step runs, exactly what the game's touch hit-test does.
    ldr  w17, [x16, #0x800]
    ldr  w16, [x16, #0x828]
    cmp  w17, w16
    b.ne alive_cursor
1:  b    sw_alive_resume                // main+0x7FCE14

// Cursor request (mailbox 0x800..0x82F, sw_helper_mailbox.h "Cursor channel"; kind at 0x80C:
// 0 move list = FIGHT step 2 + move view +0x640/+0x648, 1 command menu = select-action step 5 +
// command view (main+0x252DDB8) +0x630, 2 target select = FIGHT step 6 + target view
// (main+0x252F138) +0x5D8), run from the battle client's per-frame update (this hook's function entry: x0..x8 and x30 are live and
// restored; x9..x17 are free at a call boundary). Checked in order, every failure answered with
// a code and nothing written:
//   1 module magic / host epoch mismatch      2 not this client, or not FIGHT (main+0x803590) step 2
//   3 move view vtable (main+0x252DC10)       4 active list (+0x640, Max +0x648) != requested or vtable
//   5 grid cursor (list+0x408) vtable         6 index outside the grid (or a paged grid)
// Then: cursor+8 = index, Native15B7A20(list, index, old, 0) -- the list's own "cursor moved"
// step (the move view's callback updates its hovered slot +0x678, the cursor art moves).
alive_cursor:
    stp  x29, x30, [sp, #-0x80]!
    mov  x29, sp
    stp  x0,  x1,  [sp, #0x10]
    stp  x2,  x3,  [sp, #0x20]
    stp  x4,  x5,  [sp, #0x30]
    stp  x6,  x7,  [sp, #0x40]
    stp  x8,  x19, [sp, #0x50]
    stp  x20, x21, [sp, #0x60]
    str  x22, [sp, #0x70]
    adrp x19, sw_mailbox_base
    add  x19, x19, :lo12:sw_mailbox_base
    mov  x20, x0                        // client
    add  x9, x19, #0x800
    ldar w21, [x9]                      // request seq (acquire)
    mov  w22, #1
    ldr  w9, [x19, #0x00]
    movz w10, #0x5753                   // 'SWM1'
    movk w10, #0x314D, lsl #16
    cmp  w9, w10
    b.ne cursor_done
    ldr  w9, [x19, #0x808]
    ldr  w10, [x19, #0xFFC]
    cmp  w9, w10
    b.ne cursor_done
    mov  w22, #2
    ldr  x9, [x19, #0x820]
    cmp  x9, x20
    b.ne cursor_done
    ldr  w13, [x19, #0x80C]             // list kind: 0 move list, 1 command menu, 2 target select
    cmp  w13, #2
    b.hi cursor_done
    ldr  x9, [x20, #0x560]
    ldr  w10, [x20, #0x570]
    cbz  w13, cursor_state_move
    cmp  w13, #1
    b.eq cursor_state_command
    adrp x11, sw_state_fight            // target select: FIGHT state step 6
    add  x11, x11, :lo12:sw_state_fight
    cmp  x9, x11
    b.ne cursor_done
    cmp  w10, #6
    b.ne cursor_done
    mov  w22, #3
    ldr  x9, [x19, #0x818]              // target view
    cbz  x9, cursor_done
    ldr  x10, [x9]
    adrp x11, sw_vt_target_view
    add  x11, x11, :lo12:sw_vt_target_view
    cmp  x10, x11
    b.ne cursor_done
    mov  w22, #4
    ldr  x10, [x9, #0x5D8]              // its grid list
    b    cursor_have_list
cursor_state_command:                   // command menu: select-action state step 5
    adrp x11, sw_state_select_action
    add  x11, x11, :lo12:sw_state_select_action
    cmp  x9, x11
    b.ne cursor_done
    cmp  w10, #5
    b.ne cursor_done
    mov  w22, #3
    ldr  x9, [x19, #0x818]              // command view
    cbz  x9, cursor_done
    ldr  x10, [x9]
    adrp x11, sw_vt_command_view
    add  x11, x11, :lo12:sw_vt_command_view
    cmp  x10, x11
    b.ne cursor_done
    mov  w22, #4
    ldr  x10, [x9, #0x630]              // its grid list
    b    cursor_have_list
cursor_state_move:
    adrp x11, sw_state_fight
    add  x11, x11, :lo12:sw_state_fight
    cmp  x9, x11
    b.ne cursor_done
    cmp  w10, #2
    b.ne cursor_done
    mov  w22, #3
    ldr  x9, [x19, #0x818]              // move view
    cbz  x9, cursor_done
    ldr  x10, [x9]
    adrp x11, sw_vt_move_view
    add  x11, x11, :lo12:sw_vt_move_view
    cmp  x10, x11
    b.ne cursor_done
    mov  w22, #4
    ldrb w10, [x9, #0x61D]
    mov  x11, #0x640
    mov  x12, #0x648
    cmp  w10, #0
    csel x11, x11, x12, eq
    ldr  x10, [x9, x11]                 // active list
cursor_have_list:
    ldr  x11, [x19, #0x810]
    cmp  x10, x11
    b.ne cursor_done
    cbz  x10, cursor_done
    ldr  x11, [x10]
    adrp x12, sw_vt_grid_list
    add  x12, x12, :lo12:sw_vt_grid_list
    cmp  x11, x12
    b.ne cursor_done
    mov  w22, #5
    ldr  x11, [x10, #0x408]             // grid cursor
    cbz  x11, cursor_done
    ldr  x12, [x11]
    adrp x13, sw_vt_grid_cursor
    add  x13, x13, :lo12:sw_vt_grid_cursor
    cmp  x12, x13
    b.ne cursor_done
    mov  w22, #6
    ldr  w12, [x11, #0x1C]
    cbnz w12, cursor_done
    ldr  w12, [x11, #0x0C]
    ldr  w13, [x11, #0x10]
    mul  w12, w12, w13
    ldr  w1, [x19, #0x804]              // wanted grid index
    cmp  w1, w12
    b.hs cursor_done
    mov  w22, #0
    ldr  w2, [x11, #0x08]               // old index
    cmp  w2, w1
    b.eq cursor_done
    str  w1, [x11, #0x08]
    mov  x0, x10
    mov  w3, wzr
    bl   sw_grid_cursor_moved
cursor_done:
    str  w22, [x19, #0x82C]             // result
    add  x9, x19, #0x828
    stlr w21, [x9]                      // ack (release)
    ldr  x22, [sp, #0x70]
    ldp  x20, x21, [sp, #0x60]
    ldp  x8,  x19, [sp, #0x50]
    ldp  x6,  x7,  [sp, #0x40]
    ldp  x4,  x5,  [sp, #0x30]
    ldp  x2,  x3,  [sp, #0x20]
    ldp  x0,  x1,  [sp, #0x10]
    ldp  x29, x30, [sp], #0x80
    b    sw_alive_resume

// ---------------------------------------------------------------------------
// Third hook (HUD hide, 2026-10-26): the layout work's per-frame draw step, main+0x685A00 (the GF
// layout-work class's vtable +0x38; x0 = layout work, its nn::ui2d::Layout at +0xC0; called once
// per frame for every on-screen layout, on the render workers). Entry `sub sp, sp, #0xa0`
// (ff8302d1) is reproduced. Every layout but one leaves after a name check on x9-x12 (scratch at
// a function entry): the Layout's name (*(layout+0x30)) must be "battle_top_00" -- the battle
// HUD that draws the two HP plates (Parts panes L_near1 = own, L_far1 = foe; L_near2/L_far2 in
// doubles). Live 2026-10-26: clearing bit 0 (visible) of each plate's N_inout pane hides the whole
// plate (background, name, level, HP bar, HP number); nothing in the game sets that bit again
// (its in/out animations only drive N_inout's alpha), and the layout is rebuilt for every battle.
//
// For battle_top_00 only, every frame:
//   hud_frame++ (0x68) -- "the battle HUD layout is drawing";
//   module heartbeat: hud_req_beat (0x64) unchanged for more than 240 frames = the module is gone
//     -> treated as "show" (a stopped module never leaves the plates hidden);
//   want = module magic && hud_req (0x60) == 1 && heartbeat fresh;
//   want: the four plates' N_inout are found with the game's own Pane::FindPaneByName
//     (vtable +0x60, recursive -- root -> "L_near1" etc., then that Parts pane -> "N_inout"; Parts
//     panes do not descend while searching the root, so this stays ~40 panes a frame), bit 0 is
//     cleared where set, and the pane and its slot bit are recorded (0x80 + 8*i, hud_state bit i)
//     with the layout (0x78) -- a changed layout or battle lifetime (native initialization serial/client snapshot at 0xA0/0xA8) forgets
//     the old record first, even if the next battle reuses the same layout and pane addresses;
//   !want and something recorded: if it is still the same layout, the same lookups run again and
//     bit 0 is set back only on a pane that is still the recorded one; then the record is cleared.
//   hud_state (0x6C): bits 0-3 hidden by this helper, bits 8-11 plates found this frame.
// This runs before the layout's own matrix/draw step in the same call, so a change shows on the
// same frame. FindPaneByName only reads the pane tree.
.global sw_hud_entry
sw_hud_entry:
    sub  sp, sp, #0xa0                  // reproduced entry instruction
    ldr  x9, [x0, #0xc0]                // nn::ui2d::Layout*
    cbz  x9, hud_resume
    ldr  x10, [x9, #0x30]               // layout name
    cbz  x10, hud_resume
    and  x11, x10, #0xfff               // the reads below stay inside the name's page:
    cmp  x11, #0xff0                    // a name in the last 16 bytes of a page is skipped
    b.hi hud_resume
    ldr  x11, [x10]
    movz x12, #0x6162                   // "battle_t"
    movk x12, #0x7474, lsl #16
    movk x12, #0x656c, lsl #32
    movk x12, #0x745f, lsl #48
    cmp  x11, x12
    b.ne menu_entry                     // every other layout: the battle-menu hide below
    ldr  w11, [x10, #8]
    movz w12, #0x706f                   // "op_0"
    movk w12, #0x305f, lsl #16
    cmp  w11, w12
    b.ne menu_entry
    ldrh w11, [x10, #12]
    cmp  w11, #0x30                     // "0\0"
    b.ne menu_entry

    sub  sp, sp, #0x90
    stp  x0,  x1,  [sp, #0x00]
    stp  x2,  x3,  [sp, #0x10]
    stp  x4,  x5,  [sp, #0x20]
    stp  x6,  x7,  [sp, #0x30]
    stp  x8,  x19, [sp, #0x40]
    stp  x20, x21, [sp, #0x50]
    stp  x22, x23, [sp, #0x60]
    stp  x24, x25, [sp, #0x70]
    stp  x29, x30, [sp, #0x80]
    add  x29, sp, #0x80

    mov  x19, x9                        // layout
    adrp x20, sw_mailbox_base
    add  x20, x20, :lo12:sw_mailbox_base
    // Review L4: the client's native initialization advances the battle lifetime. This does
    // not rely on timers or field updates (the latter can keep running during a wild battle).
    ldr  w9, [x20, #0xA4]
    ldr  w10, [x20, #0xA0]
    cmp  w9, w10
    b.ne hud_lifetime_new
    ldr  x9, [x20, #0x58]
    ldr  x10, [x20, #0xA8]
    cmp  x9, x10
    b.eq hud_lifetime_current
hud_lifetime_new:
    ldr  w9, [x20, #0xA4]
    str  w9, [x20, #0xA0]
    ldr  x9, [x20, #0x58]
    str  x9, [x20, #0xA8]
    // Never dereference a previous lifetime's pane pointers: the addresses may already be reused.
    str  wzr, [x20, #0x6C]
    str  xzr, [x20, #0x78]
    stp  xzr, xzr, [x20, #0x80]
    stp  xzr, xzr, [x20, #0x90]
    str  wzr, [x20, #0x198]
hud_lifetime_current:
    movz w9, #0x5753                    // 'SWG1'
    movk w9, #0x3147, lsl #16
    str  w9, [x20, #0x04]
    ldr  w9, [x20, #0x68]
    add  w9, w9, #1
    str  w9, [x20, #0x68]               // hud_frame
    ldr  w9, [x20, #0x64]               // hud_req_beat (module)
    ldr  w10, [x20, #0x70]              // last seen
    cmp  w9, w10
    b.eq 1f
    str  w9, [x20, #0x70]
    str  wzr, [x20, #0x74]
    b    2f
1:  ldr  w10, [x20, #0x74]
    add  w10, w10, #1
    str  w10, [x20, #0x74]              // frames since the module's heartbeat moved
2:  mov  w21, wzr                       // want
    mov  w25, wzr                       // requested pane family1standard/2raid
    ldr  w9, [x20, #0x00]
    movz w10, #0x5753                   // 'SWM1'
    movk w10, #0x314D, lsl #16
    cmp  w9, w10
    b.ne 3f
    add  x9, x20, #0x60
    ldar w9, [x9]                       // hud_req
    cbz  w9, 3f
    cmp  w9, #2
    b.hi 3f
    mov  w25, w9
    ldr  w9, [x20, #0x74]
    cmp  w9, #240
    b.hi 3f
    ldr  w9, [x20, #0xA4]
    ldr  w10, [x20, #0xFC]
    cbz  w9, 3f
    cmp  w9, w10
    b.ne 3f
    ldr  w9, [x20, #0x38]
    cmp  w25, #2
    b.eq hud_want_raid
    tbz  w9, #31, 3f                   // no hiding unsupported doubles/link/institution HP
    b    hud_want_supported
hud_want_raid:
    cmp  w9, #2
    b.ne 3f
    ldr  x9, [x20, #0x58]
    cbz  x9, 3f
    ldrb w10, [x9, #0x2A6]
    cbnz w10, 3f
    ldr  x9, [x9, #0x58]
    cbz  x9, 3f
    ldr  x9, [x9, #0x88]
    cbz  x9, 3f
    ldrb w10, [x9, #0x3B9]
    cmp  w10, #5
    b.ne 3f
hud_want_supported:
    mov  w21, #1
3:  ldr  x22, [x19, #0x18]              // root pane
    cbz  x22, hud_done
    ldr  w23, [x20, #0x6C]              // hud_state
    cbnz w21, hud_hide
    ldr  w25, [x20, #0x198]             // restore the family actually owned, not the current page
    b    hud_restore

    // --- hide ---
hud_hide:
    ldr  x9, [x20, #0x78]
    cmp  x9, x19
    b.eq 4f
    mov  w23, wzr                       // another layout: the record belongs to a gone battle
    str  x19, [x20, #0x78]
    stp  xzr, xzr, [x20, #0x80]
    stp  xzr, xzr, [x20, #0x90]
    str  wzr, [x20, #0x198]
4:  ldr  w9, [x20, #0x198]
    cmp  w9, w25
    b.eq hud_mode_current
    tst  w23, #0xf
    b.eq hud_mode_new
    mov  w25, w9                       // restore previous family first; new hide follows next frame
    b    hud_restore
hud_mode_new:
    str  w25, [x20, #0x198]
hud_mode_current:
    and  w23, w23, #0xf                 // found bits are per frame
    mov  w24, wzr                       // plate index 0..3
5:  bl   hud_find_plate                 // x0 = N_inout of plate w24, or 0
    cbz  x0, 6f
    mov  w9, #0x100
    lsl  w9, w9, w24
    orr  w23, w23, w9                   // found
    ldrb w9, [x0, #0x58]
    tbz  w9, #0, 6f                     // already hidden
    and  w9, w9, #0xfe
    strb w9, [x0, #0x58]                // visible = 0
    add  x10, x20, #0x80
    str  x0, [x10, w24, uxtw #3]
    mov  w9, #1
    lsl  w9, w9, w24
    orr  w23, w23, w9                   // hidden by this helper
6:  add  w24, w24, #1
    cmp  w24, #4
    b.lo 5b
    str  w23, [x20, #0x6C]
    b    hud_done

    // --- show again (only what this helper hid, only in the same layout) ---
hud_restore:
    tst  w23, #0xf
    b.eq hud_forget
    ldr  x9, [x20, #0x78]
    cmp  x9, x19
    b.ne hud_forget                     // that battle's layout is gone with its panes
    mov  w24, wzr
7:  mov  w9, #1
    lsl  w9, w9, w24
    tst  w23, w9
    b.eq 8f
    bl   hud_find_plate
    cbz  x0, 8f
    add  x10, x20, #0x80
    ldr  x10, [x10, w24, uxtw #3]
    cmp  x0, x10
    b.ne 8f
    ldrb w9, [x0, #0x58]
    orr  w9, w9, #1
    strb w9, [x0, #0x58]                // visible = 1
8:  add  w24, w24, #1
    cmp  w24, #4
    b.lo 7b
hud_forget:
    str  wzr, [x20, #0x6C]
    str  xzr, [x20, #0x78]
    stp  xzr, xzr, [x20, #0x80]
    stp  xzr, xzr, [x20, #0x90]
    str  wzr, [x20, #0x198]

hud_done:
    ldp  x0,  x1,  [sp, #0x00]
    ldp  x2,  x3,  [sp, #0x10]
    ldp  x4,  x5,  [sp, #0x20]
    ldp  x6,  x7,  [sp, #0x30]
    ldp  x8,  x19, [sp, #0x40]
    ldp  x20, x21, [sp, #0x50]
    ldp  x22, x23, [sp, #0x60]
    ldp  x24, x25, [sp, #0x70]
    ldp  x29, x30, [sp, #0x80]
    add  sp, sp, #0x90
hud_resume:
    b    sw_hud_resume                  // main+0x685A04

// ---------------------------------------------------------------------------
// Battle-menu hide (2026-11-01, owner: "hide top screen battle hud: fight, pokemon, bag, run pill,
// y info and x pokeball stripe, moveset option, pokeball selection group"), same draw hook, same
// method as the HP plates: clear bit 0 (visible) of one container pane per menu layout, every
// frame, keyed by the layout's name; restore only what this helper cleared, in the same layout
// and battle lifetime. Mailbox 0x840.. (sw_helper_mailbox.h "Menu hide"):
//   menu_req (0x840, module): bit 0 = the menus, bit 1 = target select too;
//   menu_req_beat (0x844, module heartbeat) -- unchanged for 1200 hook calls = module gone = show;
//   cursor layouts (0x868, module, 4 x u64): each menu's "cursor" Layout, resolved by OWNERSHIP
//     on the module side (sw_battle_cursor.h SampleLayouts: command, move list, ball picker,
//     target); ~85 Layouts are named "cursor", so a cursor is touched only on an exact address
//     match (and its vtable + name are checked here again);
//   records (0x888, guest, 10 x {pane, layout, u32 lifetime serial, u32 hidden}): slot 0
//     battle_commandSelect_00 "all", 1/2 the two battle_commandSelect_01 "all_00" (X ball / Y info
//     strip and the Move Info strip; claimed with an exclusive store, the hook runs on several
//     render workers), 3 battle_skillSelect_00 "all", 4 battle_ballselect_00 RootPane,
//     5 battle_target_select_00 RootPane (bit 1 only), 6..9 the four cursor RootPanes.
// Each slot is written only by the call drawing its own layout, so concurrent workers never
// share a record. Want = module magic && req bit && heartbeat fresh && the battle lifetime current
// (0xA4 == 0xFC, as the HP plates).
menu_entry:
    sub  sp, sp, #0x90
    stp  x0,  x1,  [sp, #0x00]
    stp  x2,  x3,  [sp, #0x10]
    stp  x4,  x5,  [sp, #0x20]
    stp  x6,  x7,  [sp, #0x30]
    stp  x8,  x19, [sp, #0x40]
    stp  x20, x21, [sp, #0x50]
    stp  x22, x23, [sp, #0x60]
    stp  x24, x25, [sp, #0x70]
    stp  x29, x30, [sp, #0x80]
    add  x29, sp, #0x80
    mov  x19, x9                        // layout
    mov  x21, x10                       // its name
    adrp x20, sw_mailbox_base
    add  x20, x20, :lo12:sw_mailbox_base
    // --- which menu? w24 = slot, w25 = pane mode (0 "all", 1 "all_00", 2 RootPane), w23 = req bit
    adr  x22, menu_names
    mov  w24, wzr                       // table index
1:  mov  x0, x21
    add  x1, x22, x24, lsl #5
    bl   menu_streq
    cbnz w0, 2f
    add  w24, w24, #1
    cmp  w24, #6
    b.lo 1b
    b    menu_done
2:  add  x9, x22, x24, lsl #5
    ldrb w23, [x9, #0x1E]               // req bit
    ldrb w25, [x9, #0x1D]               // pane mode
    ldrb w24, [x9, #0x1C]               // slot
    cmp  w24, #6
    b.ne 4f
    // cursor: exact match with one of the module's ownership-resolved cursor Layouts
    ldr  x9, [x19]
    adrp x10, sw_vt_layout
    add  x10, x10, :lo12:sw_vt_layout
    cmp  x9, x10
    b.ne menu_done
    mov  w9, wzr
3:  add  x10, x20, #0x868
    ldr  x10, [x10, w9, uxtw #3]
    cmp  x10, x19
    b.eq 5f
    add  w9, w9, #1
    cmp  w9, #4
    b.lo 3b
    b    menu_done
5:  add  w24, w24, w9               // slot 6..9
    cmp  w9, #3
    b.ne 4f
    mov  w23, #2                        // the target-select cursor follows the target bit
4:  // --- want?
    mov  w22, wzr
    ldr  w9, [x20, #0x00]
    movz w10, #0x5753                   // 'SWM1'
    movk w10, #0x314D, lsl #16
    cmp  w9, w10
    b.ne 6f
    add  x9, x20, #0x840
    ldar w9, [x9]                       // menu_req
    tst  w9, w23
    b.eq 6f
    ldr  w9, [x20, #0x844]              // module heartbeat
    ldr  w10, [x20, #0x848]             // last seen
    cmp  w9, w10
    b.eq 7f
    str  w9, [x20, #0x848]
    str  wzr, [x20, #0x84C]
    b    8f
7:  ldr  w10, [x20, #0x84C]
    add  w10, w10, #1
    str  w10, [x20, #0x84C]             // hook calls since the heartbeat moved (approximate)
    cmp  w10, #1200
    b.hi 6f
8:  ldr  w9, [x20, #0xA4]
    ldr  w10, [x20, #0xFC]
    cbz  w9, 6f
    cmp  w9, w10
    b.ne 6f
    mov  w22, #1
6:  // --- the pane: x23 <- pane (root, or root -> FindPaneByName)
    ldr  x0, [x19, #0x18]               // RootPane
    cbz  x0, menu_done
    cmp  w25, #2
    b.eq 9f
    adr  x1, menu_all_name
    cbz  w25, 10f
    adr  x1, menu_all00_name
10: ldr  x8, [x0]
    ldr  x8, [x8, #0x60]
    mov  w2, #1
    blr  x8
    cbz  x0, menu_done
9:  mov  x23, x0                        // pane
    ldr  w25, [x20, #0xA4]              // current lifetime serial
    // --- the record: x21 = &record[slot] (24 bytes)
    cmp  w24, #1
    b.ne 11f
    // battle_commandSelect_01: two instances -> record 1 or 2
    add  x9, x20, #0x888 + 24 * 1 + 8   // record[1].layout
    ldr  x10, [x9]
    cmp  x10, x19
    b.eq 11f
    add  x11, x20, #0x888 + 24 * 2 + 8  // record[2].layout
    ldr  x12, [x11]
    cmp  x12, x19
    b.eq 12f
    cbnz w22, 13f
    b    menu_done                      // not ours and nothing to hide
13: ldr  w12, [x20, #0x888 + 24 * 1 + 16]
    cmp  w12, w25
    b.ne 14f                            // record 1 is a gone battle's: reclaim it
    cbnz x10, 15f
14: ldaxr x10, [x9]                     // claim record 1 exclusively
    cmp  x10, x19
    b.eq 11f
    cbz  x10, 19f
    ldr  w12, [x20, #0x888 + 24 * 1 + 16]
    cmp  w12, w25
    b.eq 16f                            // the other instance holds it in this battle
19: stlxr w12, x19, [x9]
    cbnz w12, 14b
    str  xzr, [x9, #-8]                 // its pane, still unrecorded
    str  wzr, [x9, #12]                 // hidden
    str  w25, [x9, #8]                  // serial
    b    11f
16:                                     // (no clrex: system instructions are refused; a dangling monitor is harmless)
15: mov  w24, #2
    b    11f
12: mov  w24, #2
11: mov  w9, #24
    add  x21, x20, #0x888
    umaddl x21, w24, w9, x21            // &record[slot]
    ldr  x9, [x21, #8]                  // recorded layout
    ldr  w10, [x21, #16]                // its lifetime serial
    cbz  w22, menu_show
    // --- hide
    cmp  x9, x19
    b.ne 17f
    cmp  w10, w25
    b.eq 18f
17: str  xzr, [x21, #0]                 // another layout / battle: forget, never touch it
    str  x19, [x21, #8]
    str  w25, [x21, #16]
    str  wzr, [x21, #20]
18: ldrb w9, [x23, #0x58]
    tbz  w9, #0, menu_done              // already hidden (by us, or by the game: then not ours)
    and  w9, w9, #0xfe
    strb w9, [x23, #0x58]               // visible = 0
    str  x23, [x21, #0]
    mov  w9, #1
    str  w9, [x21, #20]
    b    menu_done
menu_show:
    ldr  w11, [x21, #20]
    cbz  w11, menu_done
    cmp  x9, x19
    b.ne menu_forget
    cmp  w10, w25
    b.ne menu_forget
    ldr  x11, [x21, #0]
    cmp  x11, x23
    b.ne menu_forget
    ldrb w9, [x23, #0x58]
    orr  w9, w9, #1
    orr  w9, w9, #0x10
    strb w9, [x23, #0x58]               // visible = 1, matrix dirty: a cursor that moved while
                                        // hidden is redrawn where it is now (live 2026-11-01)
menu_forget:
    str  wzr, [x21, #20]
    str  xzr, [x21, #0]
menu_done:
    ldp  x0,  x1,  [sp, #0x00]
    ldp  x2,  x3,  [sp, #0x10]
    ldp  x4,  x5,  [sp, #0x20]
    ldp  x6,  x7,  [sp, #0x30]
    ldp  x8,  x19, [sp, #0x40]
    ldp  x20, x21, [sp, #0x50]
    ldp  x22, x23, [sp, #0x60]
    ldp  x24, x25, [sp, #0x70]
    ldp  x29, x30, [sp, #0x80]
    add  sp, sp, #0x90
    b    sw_hud_resume

// menu_streq: x0 = NUL-terminated name, x1 = table name (NUL-terminated, < 28 bytes) -> w0 = 1
// when equal. Byte compare, stops at the first difference or NUL (never reads past the name).
menu_streq:
    mov  x9, xzr
1:  ldrb w10, [x0, x9]
    ldrb w11, [x1, x9]
    cmp  w10, w11
    b.ne 2f
    cbz  w10, 3f
    add  x9, x9, #1
    cmp  x9, #28
    b.lo 1b
2:  mov  w0, wzr
    ret
3:  mov  w0, #1
    ret

.balign 32
menu_names:                             // 32 bytes: name[28], slot, pane mode, req bit, 0
    .ascii "battle_commandSelect_00\0\0\0\0\0"
    .byte 0, 0, 1, 0
    .ascii "battle_commandSelect_01\0\0\0\0\0"
    .byte 1, 1, 1, 0
    .ascii "battle_skillSelect_00\0\0\0\0\0\0\0"
    .byte 3, 0, 1, 0
    .ascii "battle_ballselect_00\0\0\0\0\0\0\0\0"
    .byte 4, 2, 1, 0
    .ascii "battle_target_select_00\0\0\0\0\0"
    .byte 5, 2, 2, 0
    .ascii "cursor\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
    .byte 6, 2, 1, 0
menu_all_name:
    .ascii "all\0"
menu_all00_name:
    .ascii "all_00\0"
.balign 4

// hud_find_plate: x22 = root pane, w24 = plate index -> x0 = that plate's N_inout pane or 0.
// Pane::FindPaneByName(name, recursive) is the pane's vtable +0x60 (main+0x18BFEB0 for both
// nn::ui2d::Pane and Parts). Clobbers the caller-saved registers.
hud_find_plate:
    stp  x29, x30, [sp, #-0x10]!
    mov  x29, sp
    adr  x1, hud_plate_names
    cmp  w25, #2
    b.eq hud_find_raid
    add  x1, x1, w24, uxtw #3
    b    hud_find_outer
hud_find_raid:
    adr  x1, hud_raid_plate_names
    add  x1, x1, w24, uxtw #4
hud_find_outer:
    mov  x0, x22
    ldr  x8, [x0]
    ldr  x8, [x8, #0x60]
    mov  w2, #1
    blr  x8
    cbz  x0, 9f
    adr  x1, hud_inout_name
    ldr  x8, [x0]
    ldr  x8, [x8, #0x60]
    mov  w2, #1
    blr  x8
9:  ldp  x29, x30, [sp], #0x10
    ret

.balign 8
hud_plate_names:                        // 8 bytes each, index = hud_state bit
    .ascii "L_near1\0"
    .ascii "L_far1\0\0"
    .ascii "L_near2\0"
    .ascii "L_far2\0\0"
hud_inout_name:
    .ascii "N_inout\0"
.balign 16
hud_raid_plate_names:                   // four near raid plates; native boss L_far1 is untouched
    .ascii "L_near1_raid\0\0\0\0"
    .ascii "L_near2_raid\0\0\0\0"
    .ascii "L_near3_raid\0\0\0\0"
    .ascii "L_near4_raid\0\0\0\0"

// ---------------------------------------------------------------------------
// Fourth hook (field actions, 2026-10-26): the field process's per-frame update, main+0xDBD850
// (vtable +0x40 of the field process class, called once per game frame from the process runner
// main+0x674198). The process can update behind a wild battle too (verified 2026-10-28);
// a beat never grants control by itself. field_free and the module's phase gate are required.
// Entry `sub sp, sp, #0xf0` (ffc303d1) is reproduced; resume at main+0xDBD854. It runs before
// the field update of the same frame takes its own locks.
//
// Every frame: guest magic + field_beat++ (0x100) + field_free (0x104, see field_free). On a request in the field channel
// (0x110.., independent of the battle channel) it runs one of:
//   1 MEDICINE (arg item in the verified set, expect party slot, 0x138 the slot's encryption
//     constant) -- the Bag's own "use on a member" steps (main+0x7B9FC0): party = field manager
//     +0x58 (vtable checked), both locks as the Bag takes them, the same member still in that slot,
//     the game's own usability check main+0x13B6720 ("It won't have any effect."), SubItem
//     main+0x1420B70 (must remove one) and the game's own apply main+0x13B7300.
//   2 REPEL (arg 76/77/79) -- the Bag's repel branch (main+0x7B93F4..): the game's own "a Repel
//     is still in effect" check main+0xDCED50, SubItem (must remove one), UseRepel main+0xDCECC0.
//   3 FLY (zone hash 0x128, landing hash 0x130) -- the Town Map's own chain: StateCheckFly's
//     preconditions (event flag 0xEB5178B9A0F65ED4, player object +0x4AA, main+0xD7E430), the
//     landing lookup main+0xF09C00 that fills the warp target (as main+0x1516670 does), the
//     destination's own unlock flag (EventWork check main+0x1410F00, as the Town Map's list
//     builder does), then the warp request exactly as the field's Town Map caller builds it
//     (main+0x15197A0..0x1519840: taxi flag main+0x1519990, request main+0xDF2CA0).
//   4 FLY_QUERY (count 0x150, pairs at 0x200) -- the same lookup + unlock flag per destination
//     (bitmask at 0x158) and the precondition result (0x160): what the Town Map shows as
//     available, without opening it.
// Frame: 0x00-0x6F saves, 0x70 flag hash out, 0x78 request handle, 0x80 scratch, 0xC0 warp
// params P (0x100), 0x1C0 scratch target (0x90), 0x250/0x258 hash pair.
.global sw_field_entry
sw_field_entry:
    sub  sp, sp, #0xf0                  // reproduced entry instruction
    sub  sp, sp, #0x280
    stp  x0,  x1,  [sp, #0x00]
    stp  x19, x20, [sp, #0x10]
    stp  x21, x22, [sp, #0x20]
    stp  x23, x24, [sp, #0x30]
    stp  x25, x26, [sp, #0x40]
    stp  x29, x30, [sp, #0x50]
    stp  x27, x28, [sp, #0x60]
    add  x29, sp, #0x50

    adrp x20, sw_mailbox_base
    add  x20, x20, :lo12:sw_mailbox_base
    movz w9, #0x5753                    // 'SWG1'
    movk w9, #0x3147, lsl #16
    str  w9, [x20, #0x04]
    ldr  w9, [x20, #0x100]
    add  w9, w9, #1
    str  w9, [x20, #0x100]              // field_beat
    bl   field_free
    str  w0, [x20, #0x104]              // field_free: the player has control (review H1)
    bl   fprogress_poll
    cbnz w0, field_done

    ldr  w9, [x20, #0x00]
    movz w10, #0x5753                   // 'SWM1'
    movk w10, #0x314D, lsl #16
    cmp  w9, w10
    b.ne field_done
    add  x9, x20, #0x110
    ldar w23, [x9]                      // field req_seq
    ldr  w9, [x20, #0x140]              // field ack_seq
    cmp  w23, w9
    b.eq field_done
    add  x9, x20, #0x124
    ldar w9, [x9]                       // field cancel_seq
    cmp  w9, w23
    b.eq frefuse_cancelled
    ldr  w9, [x20, #0x120]
    add  x10, x20, #0xFFC
    ldar w10, [x10]
    cbz  w10, frefuse_stale
    cmp  w9, w10
    b.ne frefuse_stale

    ldr  w9, [x20, #0x114]
    cmp  w9, #4
    b.eq fop_fly_query                  // read-only: allowed whatever runs
    ldr  w10, [x20, #0x104]
    cbz  w10, frefuse_not_free          // review H1: a script/event/dialogue/warp task runs
    cmp  w9, #1
    b.eq fop_medicine
    cmp  w9, #2
    b.eq fop_repel
    cmp  w9, #3
    b.eq fop_fly
    cmp  w9, #5
    b.eq fop_bike
    cmp  w9, #6
    b.eq fop_progression
    cmp  w9, #7
    b.eq fop_party_swap
    b    frefuse_bad_op

// --- PARTY_SWAP (owner 2026-10-31, Pokemon page drag reorder) -----------------
// The field party's own position exchange, PokeParty vtable +0x40 = main+0x7844F0 (the same
// object the medicine path resolves: field manager +0x58, vtable checked): it refuses a==b or
// an index > 5 itself, swaps the two slot pointers (+0x58 + 8*i), packs empty slots
// (main+0x784400) and keeps the party's +0x89 index pointing at the same member. No battle
// object is involved (the 0x83C230/0x83C870 wrappers are the in-battle roster's). Arguments:
// 0x118 from, 0x11C to (both < count, distinct), 0x138 / 0x13C the PK8 encryption constants the
// module showed for those slots -- both must still match under the party lock, else nothing.
fop_party_swap:
    ldr  w25, [x20, #0x118]             // from
    ldr  w26, [x20, #0x11C]             // to
    cmp  w25, #5
    b.hi frefuse_bad_arg
    cmp  w26, #5
    b.hi frefuse_bad_arg
    cmp  w25, w26
    b.eq frefuse_bad_arg
    adrp x9, sw_fieldmgr_slot
    add  x9, x9, :lo12:sw_fieldmgr_slot
    ldr  x9, [x9]
    ldr  x9, [x9]
    cbz  x9, frefuse_not_in_party
    ldr  x9, [x9, #0x90]                // field manager
    cbz  x9, frefuse_not_in_party
    ldr  x21, [x9, #0x58]               // party
    cbz  x21, frefuse_not_in_party
    ldr  x9, [x21]
    adrp x10, sw_party_vtable
    add  x10, x10, :lo12:sw_party_vtable
    cmp  x9, x10
    b.ne frefuse_not_in_party
    mov  x0, x21
    bl   obj_lock
    mov  w24, #27                       // TargetChanged
    ldrb w9, [x21, #0x88]               // member count (byte, as the exchange's packer reads it)
    cmp  w25, w9
    b.hs 1f
    cmp  w26, w9
    b.hs 1f
    add  x9, x21, x25, lsl #3
    ldr  x9, [x9, #0x58]
    cbz  x9, 1f
    ldr  w9, [x9, #0xd0]                // PK8 encryption constant (unencrypted)
    ldr  w10, [x20, #0x138]
    cbz  w10, 1f
    cmp  w9, w10
    b.ne 1f
    add  x9, x21, x26, lsl #3
    ldr  x9, [x9, #0x58]
    cbz  x9, 1f
    ldr  w9, [x9, #0xd0]
    ldr  w10, [x20, #0x13C]
    cbz  w10, 1f
    cmp  w9, w10
    b.ne 1f
    mov  x0, x21
    mov  w1, w25
    mov  w2, w26
    bl   sw_party_exchange
    mov  w24, wzr
1:  mov  x0, x21
    bl   obj_unlock
    mov  w0, w24
    cbnz w0, frefuse
    str  wzr, [x20, #0x148]
    b    fcommit

// --- MEDICINE ---------------------------------------------------------------
// Only the module's allowed medicine set (kFieldMedicineMask), with an explicit move slot for PP
// items, only on the member the tile showed (M1: the live slot's PK8 encryption
// constant at slot+0xD0 must equal the module's 0x138 word), and the bag must really give one up
// (M2: SubItem main+0x1420B70 returns 1 only when it removed an item -- count >= 1 -- and changes
// nothing otherwise), so the order is CanUse -> SubItem -> Apply, all under both locks.
fop_medicine:
    ldr  w25, [x20, #0x118]             // item
    ldr  w26, [x20, #0x11C]             // party slot
    cmp  w25, #63
    b.hi frefuse_not_usable
    movz x9, #0x0000                    // kFieldMedicineMask:17..33,38..41,51,53
    movk x9, #0xFFFE, lsl #16
    movk x9, #0x03C3, lsl #32
    movk x9, #0x0028, lsl #48
    lsr  x9, x9, x25
    tbz  x9, #0, frefuse_not_usable
    cmp  w26, #5
    b.hi frefuse_bad_arg
    ldr  w27, [x20, #0x13C]             // PP items: selected move, never silently the first move
    cmp  w27, #3
    b.hi frefuse_bad_arg
    adrp x9, sw_fieldmgr_slot
    add  x9, x9, :lo12:sw_fieldmgr_slot
    ldr  x9, [x9]
    ldr  x9, [x9]
    cbz  x9, frefuse_not_in_party
    ldr  x9, [x9, #0x90]                // field manager
    cbz  x9, frefuse_not_in_party
    ldr  x21, [x9, #0x58]               // party
    cbz  x21, frefuse_not_in_party
    ldr  x9, [x21]
    adrp x10, sw_party_vtable
    add  x10, x10, :lo12:sw_party_vtable
    cmp  x9, x10
    b.ne frefuse_not_in_party
    ldrb w9, [x21, #0x88]               // member count (byte; +0x89 is the party index)
    cmp  x26, x9
    b.hs frefuse_not_in_party
    bl   gamedata
    cbz  x0, frefuse_not_in_party
    ldr  x22, [x0, #0x1d8]              // MyItem
    cbz  x22, frefuse_not_in_party
    mov  x0, x21
    bl   obj_lock
    mov  x0, x22
    bl   obj_lock
    mov  w24, #27                       // TargetChanged
    add  x9, x21, x26, lsl #3
    ldr  x9, [x9, #0x58]                // that slot's object
    cbz  x9, 1f
    ldr  w9, [x9, #0xd0]                // its PK8 encryption constant (unencrypted)
    ldr  w10, [x20, #0x138]
    cmp  w9, w10
    b.ne 1f
    mov  w24, #17                       // NoEffect
    mov  x0, x21
    mov  w1, w25
    mov  w2, w26
    mov  w3, w27
    bl   sw_item_can_use
    tbz  w0, #0, 1f
    mov  w24, #26                       // NoItem
    mov  x0, x22
    mov  w1, w25
    mov  w2, #1
    bl   sw_sub_item
    tbz  w0, #0, 1f
    mov  x0, x21
    mov  w1, w25
    mov  w2, w26
    mov  w3, w27
    mov  w4, wzr
    bl   sw_item_apply
    mov  w24, wzr
1:  mov  x0, x22
    bl   obj_unlock
    mov  x0, x21
    bl   obj_unlock
    mov  w0, w24
    cbnz w0, frefuse
    str  w25, [x20, #0x148]
    b    fcommit

// --- REPEL ------------------------------------------------------------------
fop_repel:
    ldr  w25, [x20, #0x118]
    cmp  w25, #76                       // Super Repel
    b.eq 1f
    cmp  w25, #77                       // Max Repel
    b.eq 1f
    cmp  w25, #79                       // Repel
    b.ne frefuse_not_usable
1:  bl   sw_repel_active
    tbnz w0, #0, frefuse_repel_active
    bl   gamedata
    cbz  x0, frefuse_not_usable
    ldr  x22, [x0, #0x1d8]              // MyItem
    cbz  x22, frefuse_not_usable
    mov  x0, x22
    bl   obj_lock
    mov  x0, x22
    mov  w1, w25
    mov  w2, #1
    bl   sw_sub_item
    and  w24, w0, #1                    // review M2: 1 only when one was really removed
    mov  x0, x22
    bl   obj_unlock
    cbz  w24, frefuse_no_item
    mov  w0, w25
    bl   sw_use_repel
    str  w25, [x20, #0x148]
    b    fcommit

// --- FLY --------------------------------------------------------------------
fop_fly:
    ldr  x9, [x20, #0x128]
    str  x9, [sp, #0x250]
    ldr  x9, [x20, #0x130]
    str  x9, [sp, #0x258]
    bl   fly_precheck
    cbnz w0, frefuse_not_here
    add  x22, sp, #0xc0                 // P
    mov  x9, xzr
2:  add  x10, x22, x9
    stp  xzr, xzr, [x10]
    add  x9, x9, #16
    cmp  x9, #0x100
    b.lo 2b
    add  x0, x22, #0x10
    bl   sw_target_init
    add  x1, x22, #0x10
    bl   resolve_landing                // w0 = 0 none, 1 found & unlocked, 2 found, locked
    cbz  w0, frefuse_bad_dest
    cmp  w0, #2
    b.eq frefuse_fly_locked
    // the field's own request record (main+0x15197A0..0x15197FC)
    movz w9, #0x0100
    movk w9, #0x0001, lsl #16
    str  w9, [x22, #0xa0]
    mov  w9, #0x100
    str  w9, [x22, #0xa4]
    mov  w9, #-1
    str  w9, [x22, #0xa8]
    bl   fnv_basis
    str  x0, [x22, #0xb0]
    str  x0, [x22, #0xd0]
    adrp x9, sw_empty_string
    add  x9, x9, :lo12:sw_empty_string
    str  x9, [x22, #0xd8]
    mov  x0, xzr
    mov  w1, #1
    bl   sw_taxi_flag
    str  xzr, [sp, #0x78]
    mov  x0, x22
    add  x8, sp, #0x78
    bl   sw_warp_request
    ldr  x0, [sp, #0x78]
    cbz  x0, 3f
    bl   ref_release
    str  xzr, [sp, #0x78]
3:  ldr  x0, [x22, #0xe8]
    cbz  x0, 4f
    bl   ref_release
4:  str  wzr, [x20, #0x148]
    b    fcommit

// --- BIKE -------------------------------------------------------------------
// Exact native Plus-button path (main+0xDC3980): raw player -> CanBike -> native task,
// with the returned task reference released. This keeps mount/dismount animation and refusal
// rules in the game; no button or top-screen menu is involved and no bag item is consumed.
fop_bike:
    bl   sw_bike_player
    cbz  x0, frefuse_bike
    bl   sw_bike_can_use
    tbz  w0, #0, frefuse_bike
    str  xzr, [sp, #0x78]
    add  x8, sp, #0x78
    bl   sw_bike_task
    ldr  x0, [sp, #0x78]
    cbz  x0, frefuse_bike
    bl   ref_release
    str  wzr, [x20, #0x148]
    b    fcommit

// --- FLY_QUERY --------------------------------------------------------------
fop_fly_query:
    // The map's Bike capsule uses the game's own Plus-button legality predicate. During a
    // script/menu/battle it is deliberately false, independently of the last successful query.
    str  wzr, [x20, #0x168]
    ldr  w9, [x20, #0x104]
    cbz  w9, bike_query_done
    bl   sw_bike_player
    cbz  x0, bike_query_done
    bl   sw_bike_can_use
    and  w0, w0, #1
    str  w0, [x20, #0x168]
bike_query_done:
    // Native TownMap15140A0 display-parent lookup; retain the raw zone alongside it so
    // a later host sample can discard a stale query after a zone transition.
    str xzr, [x20, #0x178]
    str xzr, [x20, #0x180]
    bl sw_current_zone
    cbz x0, map_query_done
    str x0, [x20, #0x178]
    adrp x9, sw_fieldmgr_slot
    add x9, x9, :lo12:sw_fieldmgr_slot
    ldr x9, [x9]
    ldr x9, [x9]
    cbz x9, map_query_done
    ldr x0, [x9, #0x90]
    cbz x0, map_query_done
    add x1, x20, #0x178
    bl sw_map_parent_zone
    str x0, [x20, #0x180]
map_query_done:
    bl   fly_precheck
    str  w0, [x20, #0x160]
    ldr  w26, [x20, #0x150]             // count
    cmp  w26, #48
    b.hi frefuse_bad_arg
    mov  x27, xzr                       // bitmask
    mov  w25, wzr                       // index
5:  cmp  w25, w26
    b.hs 6f
    add  x9, x20, #0x200
    add  x9, x9, x25, lsl #4
    ldp  x10, x11, [x9]
    str  x10, [sp, #0x250]
    str  x11, [sp, #0x258]
    add  x0, sp, #0x1c0
    bl   sw_target_init
    add  x1, sp, #0x1c0
    bl   resolve_landing
    cmp  w0, #1
    b.ne 7f
    mov  x9, #1
    lsl  x9, x9, x25
    orr  x27, x27, x9
7:  add  w25, w25, #1
    b    5b
6:  str  x27, [x20, #0x158]
    str  w26, [x20, #0x148]
    b    fcommit

// resolve_landing: hashes at sp(caller)+0x250/0x258, x1 = target record (initialised) -> w0 =
// 0 not found, 1 found and unlocked, 2 found but locked. Uses the caller's frame (x29-based).
resolve_landing:
    stp  x29, x30, [sp, #-0x20]!
    str  x19, [sp, #0x10]
    mov  x19, x1
    bl   fnv_basis
    str  x0, [x29, #0x20]               // caller sp+0x70: the destination's unlock-flag hash
    adrp x9, sw_fieldmgr_slot
    add  x9, x9, :lo12:sw_fieldmgr_slot
    ldr  x9, [x9]
    ldr  x9, [x9]
    cbz  x9, 8f
    ldr  x28, [x9, #0x90]               // field manager
    cbz  x28, 8f
    mov  x0, x28
    bl   obj_lock
    mov  x0, x28
    add  x1, x29, #0x200                // caller sp+0x250: zone hash
    add  x2, x29, #0x208                // caller sp+0x258: landing hash
    mov  x3, x19
    add  x4, x29, #0x20
    bl   sw_resolve_landing
    and  w19, w0, #1
    mov  x0, x28
    bl   obj_unlock
    cbz  w19, 8f
    bl   gamedata
    cbz  x0, 9f
    ldr  x28, [x0, #0x1b8]              // EventWork
    cbz  x28, 9f
    mov  x0, x28
    bl   obj_lock
    mov  x0, x28
    ldr  x1, [x29, #0x20]
    bl   sw_event_flag
    and  w19, w0, #1
    mov  x0, x28
    bl   obj_unlock
    mov  w0, #2
    sub  w0, w0, w19                    // 1 unlocked, 2 locked
    b    10f
8:  mov  w0, wzr
    b    10f
9:  mov  w0, #2
10: ldr  x19, [sp, #0x10]
    ldp  x29, x30, [sp], #0x20
    ret

// fly_precheck: StateCheckFly's own three checks (main+0x151BF78..0x151C104) -> w0 = 0 allowed,
// 1 / 2 refused (the game's two "can't call a Flying Taxi" outcomes).
fly_precheck:
    stp  x29, x30, [sp, #-0x20]!
    str  x19, [sp, #0x10]
    bl   gamedata
    cbz  x0, 2f
    ldr  x19, [x0, #0x1b8]              // EventWork
    cbz  x19, 2f
    mov  x0, x19
    bl   obj_lock
    movz x1, #0x5ed4
    movk x1, #0xa0f6, lsl #16
    movk x1, #0x78b9, lsl #32
    movk x1, #0xeb51, lsl #48
    mov  x0, x19
    bl   sw_event_flag
    and  w9, w0, #1
    str  w9, [sp, #0x18]
    mov  x0, x19
    bl   obj_unlock
    ldr  w9, [sp, #0x18]
    cbnz w9, 2f
    adrp x9, sw_world_slot
    add  x9, x9, :lo12:sw_world_slot
    ldr  x9, [x9]
    ldr  x0, [x9]
    cbz  x0, 3f
    add  x1, x29, #0x20                 // ignored scratch (the native passes a stack slot)
    bl   sw_find_player
    cbz  x0, 3f
    ldrb w9, [x0, #0x4aa]
    cbz  w9, 1f
3:  bl   sw_fly_blocked
    and  w0, w0, #1
    b    4f
1:  mov  w0, #1
    b    4f
2:  mov  w0, #2
4:  ldr  x19, [sp, #0x10]
    ldp  x29, x30, [sp], #0x20
    ret

// field_free: w0 = 1 while the player has control of the field (review H1), the same condition
// under which the game dispatches field input at all (main+0xF1BD00: input runs only when
// main+0xF198E0([G+0x68]) is false). G = the field system *(*(main+0x2610848)); [G+0x68] is the
// field task runner, +0x78 its task-stack head: non-zero while any field task runs -- the event
// script task (NPC dialogue, scripted walks, cutscenes: main+0x14C9A70), a warp/Flying Taxi
// request (main+0xDF2CA0) and the X-menu launcher are all pushed there (main+0xF19670). Also
// refused: an event about to start ([G+0xB8] event manager +0x1F8 != the empty FNV hash, the
// check main+0xDC3510 makes before it starts one). Leaf: x0, x9-x11.
field_free:
    mov  w0, wzr
    adrp x9, sw_fieldmgr_slot
    add  x9, x9, :lo12:sw_fieldmgr_slot
    ldr  x9, [x9]
    cbz  x9, 1f
    ldr  x9, [x9]                       // G
    cbz  x9, 1f
    ldr  x10, [x9, #0x68]               // field task runner
    cbz  x10, 1f
    ldr  x10, [x10, #0x78]              // task-stack head
    cbnz x10, 1f
    ldr  x10, [x9, #0xb8]               // event manager
    cbz  x10, 1f
    ldr  x10, [x10, #0x1f8]             // pending event hash
    movz x11, #0x2645
    movk x11, #0x8422, lsl #16
    movk x11, #0x9ce4, lsl #32
    movk x11, #0xcbf2, lsl #48
    cmp  x10, x11
    b.ne 1f
    mov  w0, #1
1:  ret

// gamedata: x0 = the game-data object (*(*(main+0x2610798))) or 0. Leaf.
gamedata:
    adrp x0, sw_gamedata_slot
    add  x0, x0, :lo12:sw_gamedata_slot
    ldr  x0, [x0]
    cbz  x0, 1f
    ldr  x0, [x0]
1:  ret

// fnv_basis: x0 = the empty FNV-1a 64 hash (the game's "no hash" value). Leaf.
fnv_basis:
    movz x0, #0x2645
    movk x0, #0x8422, lsl #16
    movk x0, #0x9ce4, lsl #32
    movk x0, #0xcbf2, lsl #48
    ret

// obj_lock / obj_unlock / ref_release: the game's lock idiom on obj+0x48 (vtable +0x10 / +0x18).
obj_lock:
    add  x0, x0, #0x48
    ldr  x8, [x0]
    ldr  x8, [x8, #0x10]
    br   x8
obj_unlock:
ref_release:
    add  x0, x0, #0x48
    ldr  x8, [x0]
    ldr  x8, [x8, #0x18]
    br   x8

// fcommit / refusals: field channel acknowledge (result value already stored).
fcommit:
    mov  w0, #0
    b    facknowledge
frefuse_cancelled:   mov w0, #4;  b frefuse
frefuse_stale:       mov w0, #3;  b frefuse
frefuse_bad_op:      mov w0, #2;  b frefuse
frefuse_bad_arg:     mov w0, #6;  b frefuse
frefuse_not_in_party: mov w0, #15; b frefuse
frefuse_no_effect:   mov w0, #17; b frefuse
frefuse_not_here:    add w0, w0, #19; b frefuse   // 20 / 21: the two precondition outcomes
frefuse_fly_locked:  mov w0, #22; b frefuse
frefuse_bad_dest:    mov w0, #23; b frefuse
frefuse_repel_active: mov w0, #24; b frefuse
frefuse_not_usable:  mov w0, #25; b frefuse
frefuse_no_item:     mov w0, #26; b frefuse
frefuse_not_free:    mov w0, #28; b frefuse
frefuse_bike:        mov w0, #29; b frefuse
frefuse:
    str  wzr, [x20, #0x148]
facknowledge:
    str  w0, [x20, #0x144]
    ldr  w9, [x20, #0x100]
    str  w9, [x20, #0x14C]              // ack_beat
    add  x10, x20, #0x140
    stlr w23, [x10]                     // field ack_seq = req_seq, release

field_done:
    ldp  x0,  x1,  [sp, #0x00]
    ldp  x19, x20, [sp, #0x10]
    ldp  x21, x22, [sp, #0x20]
    ldp  x23, x24, [sp, #0x30]
    ldp  x25, x26, [sp, #0x40]
    ldp  x29, x30, [sp, #0x50]
    ldp  x27, x28, [sp, #0x60]
    add  sp, sp, #0x280
    b    sw_field_resume                // main+0xDBD854

// ---------------------------------------------------------------------------
// Battle lifetime hook: client initialization main+0x7FC398 stores the field-manager pointer
// at client+0xE0. The native client initializer runs once per client per battle, before its
// per-frame update starts. A fresh initialization invalidates an earlier HUD ownership record
// even when allocator addresses are reused. No timer heuristic; pauses leave the serial alone.
// Reproduce the overwritten store, preserve both scratch registers and NZCV (no flag-changing
// instruction), and resume at the next native instruction.
.global sw_battle_begin_entry
sw_battle_begin_entry:
    str  x0, [x19, #0xE0]
    stp  x16, x17, [sp, #-0x10]!
    adrp x16, sw_mailbox_base
    add  x16, x16, :lo12:sw_mailbox_base
    ldr  w17, [x16, #0xA4]
    add  w17, w17, #1
    str  w17, [x16, #0xA4]
    ldp  x16, x17, [sp], #0x10
    b    sw_battle_begin_resume

#include "sw_field_progression.inc"
#include "sw_raid_target.inc"
#include "sw_battle_preview.inc"

#include "sw_field_portrait.inc"
