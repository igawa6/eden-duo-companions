// SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
// SPDX-License-Identifier: GPL-3.0-or-later
//
// FINAL FANTASY RESONANCE guest helper -- format-2 load-plan code.
// Title 0100606026F12000, update 1.0.1 (v65536), main build 84E182F72EE1DA4F477804BD05B54C15.
//
// The game's own code for what the companion's touch pages ask, on the game thread:
//   * fast travel: what runs when the player answers Yes on the world map's "travel there?" dialog;
//   * a recovery item used on a member: what the main menu's item and heal pages run (CPP_VM_SkillActionBase);
//   * equipment: CPP_BFL_Unit's equip and unequip (the equipment menu's own calls), and its equippable check;
//   * a menu sound: CPP_BFL_Sound::PlaySound2DByTag (main+0x42c8f90), the game's UI sounds by gameplay tag (Sound.UI.Cursor,
//     Sound.UI.Item.Equip, ...); it finds the game's sound subsystem itself and does nothing without one. Its own channel in
//     the mailbox (sound_seq, sound_ack, the tag's FName), served every tick before a request, so it never waits on one;
//   * quest tracking: CPP_QuestSubsystem::SetPreferredQuestId, the journal's Track (the game's own check; the tracked quest again
//     untracks it), which saves it and tells the minimap and the quest notices.
//
// Hooked site: CPP_TaskManager's FTickableGameObject::Tick (main+0x42d630c, the only vtable slot that names it), entry
// instruction `ldr x8, [x0, #0x8]` (f9400408). The task manager drives the game's root task (m_pTask) and ticks every frame
// on the game thread; the travel the helper starts is itself a task given to it (CPP_MapTaskTransition). Live in: x0 (the
// tickable base, this+0x30) and s0 (DeltaTime). The helper keeps both, works in its own frame and leaves by running the
// overwritten instruction and branching to main+0x42d6310 ("reproduce and resume", native/helper/README.md).
//
// The game's travel lives on the world map's view model (CPP_VM_WldMap). Its three pieces read only a few fields of it:
//   UpdateFastTravelBehavior (main+0x43806b0): WldMapDA +0x168 (DA_WldMapParameter), FocusObjectData.RelatedMapId +0x3b8;
//       writes FastTravelBehavior +0x408 {bCanTravel, DialogTag (FName) +0x40c}. The game's own rules: the current map's
//       type (no travel out of a dungeon), a story event, a place not visited, a locked map.
//   the travel (main+0x4381770, what HandleTransitionDialogClose calls on Yes): +0x3b8, LandingPointId +0x3e8 and one
//       virtual call (+0x188) through the view model to close the map's widget, which returns at once on null.
// So the helper hands them a view model of its own in the mailbox: those fields, and a vtable whose +0x188 returns null.
//
// The item pages' view model (CPP_VM_SkillActionBase, 0x280 bytes) is handed over the same way: SetUseAction (main+0x438e9b0)
// fills UseActionData +0x78, UseItemData +0x88 and UseSkillData +0x118 from the game's own tables, then calls three MVVM field
// notifications through its vtable (+0x2e8); it reads SkillActionDA +0x68 (required) and SkillTargetingDA +0x70 (optional).
// The game's check (main+0x438ef60: the item is owned, its usable scene is the map's) wants UserUnitParam +0x260. Then
// ApplyActionToUnit (main+0x438ed40, reserve on, as the heal menu's ApplyHealSkill calls it) uses the item up and applies its
// skill to the target (or the whole party, the skill's TargetType), and sends the game's skill-applied event (its sound).
// That view model stays in the mailbox for the whole session: each request overwrites the last one's texts and arrays as a
// real one would (zeroed FText, TArray and FString are the engine's empty values; FText's assignment checks for null).
//
// Mailbox (keep in sync with native/modules/ffr_helper_mailbox.h):
.text
.global ffr_guest_helper_entry

.equ MB_MAGIC,     0x000
.equ MB_HEARTBEAT, 0x004
.equ MB_REQ_SEQ,   0x008
.equ MB_REQ_OP,    0x00C
.equ MB_REQ_EPOCH, 0x010
.equ MB_PTR_A,     0x018   // the op's object (travel: DA_WldMapParameter; item: DA_UI_SkillAction; track: CPP_QuestSubsystem)
.equ MB_PTR_B,     0x020   // its class, compared with the object's own
.equ MB_PTR_C,     0x028   // item: DA_UI_SkillTargeting (0: none)
.equ MB_PTR_D,     0x040   // its class
.equ MB_ARG_A,     0x030   // travel: map id; item: item id; equip, unequip, equippable: unit id
.equ MB_ARG_B,     0x034   // travel: landing point; item: target unit; equip, unequip: item id; equippable: count
.equ MB_ARG_C,     0x038   // equip: slot (accessories: the index in the list)
.equ MB_RES_SEQ,   0x060
.equ MB_RES_CODE,  0x064
.equ MB_RES_VALUE, 0x068   // travel: the game's dialog tag (FName); unequip: its return
.equ MB_DIAG_REQS, 0x070   // requests answered
.equ MB_SND_SEQ,   0x078   // a sound: the module writes the tag, then bumps sound_seq (release)
.equ MB_SND_ACK,   0x07C   // the last sound_seq played
.equ MB_SND_TAG,   0x080   // the gameplay tag (an FName: comparison index, number)
.equ MB_FAKE_VM,   0x400   // the world map's view model handed to the game (0x420 bytes)
.equ MB_FAKE_VT,   0x900   // its vtable (0x190 bytes)
.equ MB_EPOCH,     0xFFC
.equ MB_ITEM_VM,   0x1000  // the item pages' view model (0x280 bytes)
.equ MB_ACTION,    0x1300  // UISkillActionData {UserUnitId, TmpNum, ActionKey {category, id}}, then the target id at +0x10
.equ MB_ITEM_VT,   0x1400  // its vtable (0x2f0 bytes)
.equ MB_IDS,       0x1800  // equippable: the item ids (at most 256)
.equ MB_BITS,      0x1C00  // equippable: one bit per id, set when the unit can wear it
.equ MAX_IDS, 256

.equ VM_WLDMAP_DA, 0x168
.equ VM_FOCUS_ID,  0x39c
.equ VM_MAP_ID,    0x3b8
.equ VM_LANDING,   0x3e8
.equ VM_BEHAVIOR,  0x408
.equ VM_TAG,       0x40c
.equ VM_TAG_NUM,   0x410
.equ VT_WIDGET,    0x188

.equ SA_ACTION_DA,  0x068
.equ SA_TARGET_DA,  0x070
.equ SA_CATEGORY,   0x080
.equ SA_ID,         0x084
.equ SA_USER_PARAM, 0x260
.equ SA_TARGET,     0x268
.equ VT_NOTIFY,     0x2e8
.equ CAT_ITEM, 2

.equ UOBJ_FLAGS, 0x8
.equ UOBJ_CLASS, 0x10
.equ RF_GARBAGE_BIT, 30

.equ OP_CHECK, 1       // the game's travel verdict only
.equ OP_TRAVEL, 2      // the verdict, then the travel when it says yes
.equ OP_ITEM, 3        // the game's check, then the item used on the target
.equ OP_EQUIP, 4       // the game's equippable check, then the equip
.equ OP_UNEQUIP, 5
.equ OP_EQUIPPABLE, 6  // the equippable check for a list of items
.equ OP_TRACK, 7       // track a quest (arg A), or untrack it when it is the tracked one
.equ OP_MAX, 7

.equ R_OK, 0
.equ R_NOT_CONFIGURED, 1
.equ R_BAD_OP, 2
.equ R_STALE_EPOCH, 3
.equ R_NOT_READY, 4
.equ R_REFUSED, 5

// An object the module named (pointer at \ptr, class at \cls): alive and of that class, or \fail
.macro CHECK_OBJECT ptr, cls, fail
    ldr  x9, [x19, #\ptr]
    cbz  x9, \fail
    ldr  x10, [x19, #\cls]
    cbz  x10, \fail
    ldr  x11, [x9, #UOBJ_CLASS]
    cmp  x11, x10
    b.ne \fail
    ldr  w11, [x9, #UOBJ_FLAGS]
    tbnz w11, #RF_GARBAGE_BIT, \fail
.endm

ffr_guest_helper_entry:
    stp  x29, x30, [sp, #-0x50]!
    mov  x29, sp
    stp  x19, x20, [sp, #0x10]
    stp  x21, x22, [sp, #0x20]
    stp  x23, x24, [sp, #0x30]
    str  x0, [sp, #0x40]
    str  d0, [sp, #0x48]

    adrp x19, ffr_mailbox_base
    add  x19, x19, :lo12:ffr_mailbox_base
    ldr  w9, [x19, #MB_MAGIC]
    movz w10, #0x4646
    movk w10, #0x3252, lsl #16      // 'FFR2'
    cmp  w9, w10
    b.ne done
    ldr  w9, [x19, #MB_HEARTBEAT]
    add  w9, w9, #1
    str  w9, [x19, #MB_HEARTBEAT]

    // A sound on its own channel (the latest one asked: sounds asked between two frames play once)
    add  x9, x19, #MB_SND_SEQ
    ldar w10, [x9]
    ldr  w11, [x19, #MB_SND_ACK]
    cmp  w10, w11
    b.eq 1f
    str  w10, [x19, #MB_SND_ACK]
    ldr  x0, [x19, #MB_SND_TAG]
    cbz  x0, 1f
    bl   ffr_play_sound
1:

    add  x9, x19, #MB_REQ_SEQ
    ldar w20, [x9]
    ldr  w10, [x19, #MB_RES_SEQ]
    cmp  w20, w10
    b.eq done
    str  xzr, [x19, #MB_RES_VALUE]

    ldr  w12, [x19, #MB_REQ_EPOCH]
    add  x9, x19, #MB_EPOCH
    ldar w13, [x9]
    cbz  w13, stale
    cmp  w12, w13
    b.ne stale

    ldr  w21, [x19, #MB_REQ_OP]
    cmp  w21, #OP_CHECK
    b.lt bad_op
    cmp  w21, #OP_MAX
    b.gt bad_op
    // A world with its map manager (none at the title or while a map loads)
    bl   ffr_get_map_manager
    cbz  x0, not_ready
    cmp  w21, #OP_ITEM
    b.eq op_item
    cmp  w21, #OP_EQUIP
    b.eq op_equip
    cmp  w21, #OP_UNEQUIP
    b.eq op_unequip
    cmp  w21, #OP_EQUIPPABLE
    b.eq op_equippable
    cmp  w21, #OP_TRACK
    b.eq op_track

// ---- fast travel (OP_CHECK, OP_TRAVEL) ----
    CHECK_OBJECT MB_PTR_A, MB_PTR_B, not_ready
    add  x22, x19, #MB_FAKE_VM
    add  x9, x19, #MB_FAKE_VT
    adr  x10, ffr_ret_null
    str  x10, [x9, #VT_WIDGET]
    str  x9, [x22]
    ldr  x9, [x19, #MB_PTR_A]
    str  x9, [x22, #VM_WLDMAP_DA]
    str  wzr, [x22, #VM_FOCUS_ID]
    ldr  w9, [x19, #MB_ARG_A]
    str  w9, [x22, #VM_MAP_ID]
    ldr  w9, [x19, #MB_ARG_B]
    str  w9, [x22, #VM_LANDING]
    str  xzr, [x22, #VM_BEHAVIOR]
    str  wzr, [x22, #VM_TAG_NUM]

    mov  x0, x22
    bl   ffr_update_travel_behavior
    ldr  w9, [x22, #VM_TAG]
    str  w9, [x19, #MB_RES_VALUE]
    ldr  w9, [x22, #VM_TAG_NUM]
    str  w9, [x19, #MB_RES_VALUE + 4]
    ldrb w9, [x22, #VM_BEHAVIOR]
    cbz  w9, refused
    cmp  w21, #OP_TRAVEL
    b.ne ok
    mov  x0, x22
    bl   ffr_travel
    b    ok

// ---- a recovery item on a member (OP_ITEM: arg A item, arg B target) ----
op_item:
    CHECK_OBJECT MB_PTR_A, MB_PTR_B, not_ready
    ldr  x9, [x19, #MB_PTR_C]
    cbz  x9, 1f
    CHECK_OBJECT MB_PTR_C, MB_PTR_D, not_ready
1:
    mov  x9, #MB_ITEM_VM
    add  x22, x19, x9
    mov  x9, #MB_ITEM_VT
    add  x9, x19, x9
    adr  x10, ffr_ret_null
    str  x10, [x9, #VT_WIDGET]
    str  x10, [x9, #VT_NOTIFY]
    str  x9, [x22]
    ldr  x9, [x19, #MB_PTR_A]
    str  x9, [x22, #SA_ACTION_DA]
    ldr  x9, [x19, #MB_PTR_C]
    str  x9, [x22, #SA_TARGET_DA]
    mov  x9, #MB_ACTION
    add  x23, x19, x9
    ldr  w9, [x19, #MB_ARG_B]
    str  w9, [x23]                  // UserUnitId: an item's user is its target
    str  wzr, [x23, #0x4]
    mov  w9, #CAT_ITEM
    str  w9, [x23, #0x8]            // ActionKey.category (and its padding)
    ldr  w9, [x19, #MB_ARG_A]
    str  w9, [x23, #0xc]            // ActionKey.id
    mov  x0, x22
    mov  x1, x23
    bl   ffr_set_use_action
    ldrb w9, [x22, #SA_CATEGORY]    // the game took it: an item, this one
    cmp  w9, #CAT_ITEM
    b.ne not_ready
    ldr  w9, [x22, #SA_ID]
    ldr  w10, [x19, #MB_ARG_A]
    cmp  w9, w10
    b.ne not_ready
    ldr  w0, [x19, #MB_ARG_B]
    bl   ffr_get_unit_param
    cbz  x0, not_ready
    str  x0, [x22, #SA_USER_PARAM]
    ldr  w9, [x19, #MB_ARG_B]
    str  w9, [x22, #SA_TARGET]
    mov  x0, x22
    bl   ffr_item_valid             // the game's check: owned, usable here
    tbz  w0, #0, refused
    ldr  w9, [x19, #MB_ARG_B]
    str  w9, [x23, #0x10]
    mov  x0, x22
    add  x1, x23, #0x10
    mov  w2, #1
    bl   ffr_apply_action
    b    ok

// ---- equipment (arg A unit, arg B item, arg C slot) ----
op_equip:
    ldr  w0, [x19, #MB_ARG_A]
    ldr  w1, [x19, #MB_ARG_B]
    bl   ffr_is_equippable
    tbz  w0, #0, refused
    ldr  w0, [x19, #MB_ARG_A]
    ldr  w1, [x19, #MB_ARG_B]
    ldr  w2, [x19, #MB_ARG_C]
    bl   ffr_set_equipment
    b    ok

op_unequip:
    ldr  w0, [x19, #MB_ARG_A]
    ldr  w1, [x19, #MB_ARG_B]
    bl   ffr_unequip
    and  w9, w0, #1
    str  w9, [x19, #MB_RES_VALUE]
    tbz  w0, #0, refused
    b    ok

// ---- which of a list of items a unit can wear (arg A unit, arg B count; ids at MB_IDS, bits to MB_BITS) ----
op_equippable:
    ldr  w23, [x19, #MB_ARG_B]
    cmp  w23, #MAX_IDS
    b.hi bad_op
    mov  x9, #MB_BITS
    add  x24, x19, x9
    stp  xzr, xzr, [x24]
    stp  xzr, xzr, [x24, #0x10]
    mov  x22, #0
1:
    cmp  w22, w23
    b.ge ok
    mov  x9, #MB_IDS
    add  x9, x19, x9
    ldr  w1, [x9, x22, lsl #2]
    ldr  w0, [x19, #MB_ARG_A]
    bl   ffr_is_equippable
    tbz  w0, #0, 2f
    lsr  x9, x22, #5
    ldr  w10, [x24, x9, lsl #2]
    and  x11, x22, #31
    mov  w12, #1
    lsl  w12, w12, w11
    orr  w10, w10, w12
    str  w10, [x24, x9, lsl #2]
2:
    add  x22, x22, #1
    b    1b

// ---- quest tracking (object A the quest subsystem, arg A the quest) ----
op_track:
    CHECK_OBJECT MB_PTR_A, MB_PTR_B, not_ready
    ldr  x0, [x19, #MB_PTR_A]
    ldr  w1, [x19, #MB_ARG_A]
    bl   ffr_set_preferred_quest
    tbz  w0, #0, refused
    b    ok

ok:
    mov  w9, #R_OK
    b    answer
refused:
    mov  w9, #R_REFUSED
    b    answer
not_ready:
    mov  w9, #R_NOT_READY
    b    answer
bad_op:
    mov  w9, #R_BAD_OP
    b    answer
stale:
    mov  w9, #R_STALE_EPOCH
answer:
    str  w9, [x19, #MB_RES_CODE]
    ldr  w9, [x19, #MB_DIAG_REQS]
    add  w9, w9, #1
    str  w9, [x19, #MB_DIAG_REQS]
    add  x10, x19, #MB_RES_SEQ
    stlr w20, [x10]

done:
    ldr  x0, [sp, #0x40]
    ldr  d0, [sp, #0x48]
    ldp  x23, x24, [sp, #0x30]
    ldp  x21, x22, [sp, #0x20]
    ldp  x19, x20, [sp, #0x10]
    ldp  x29, x30, [sp], #0x50
    // The overwritten instruction, then the rest of Tick
    ldr  x8, [x0, #0x8]
    b    ffr_resume

// The view models' virtuals the game calls (the widget getter +0x188, the field notifications +0x2e8): nothing to do
ffr_ret_null:
    mov  x0, xzr
    ret
