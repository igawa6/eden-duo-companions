// SPDX-License-Identifier: GPL-3.0-or-later
//
// Monster Sanctuary guest helper -- format-2 load-plan code.
// Title 0100A0F00DA68000, update v524288 (Ver 2.1.0.32), build 8EA928B6AA10C84F9E69129C45BCAAAF.
//
// Hooked site: GameStateManager.Update(), RVA 0x4FABC0, entry instruction
// `stp x20, x19, [sp, #-0x20]!` (bytes f44fbea9). The load plan replaces that one instruction
// with `b ms_guest_helper_entry`; the helper re-executes it first ("reproduce and resume", the
// Belmont pattern, native/helper/README.md), works in its own disposable frame below it and
// branches back to 0x4FABC4. Only x0 (`this`) is live-in; everything the helper touches is
// saved and restored. GameStateManager.Update runs once per frame on Unity's main thread, so
// every game call below runs where the game's own UI code runs.
//
// What the helper does (see native/helper/MS-HELPER.md for the mailbox table):
//   * every frame: heartbeat, the current GameStates / CombatStates ids (diagnostic words),
//     and the battle-menu hide (CombatUIController.Menu's transform scaled to 0 while the
//     companion asks for it and keeps renewing its lease; restored otherwise);
//   * at most one request per frame, each gated on the game state and answered with a result
//     code: the native combat menus (MenuList.SelectMenuItem + TrySelectCurrent, i.e. the A
//     press: SelectAction / item -> UseConsumable / switch -> SwitchMonster all run through the
//     game's own menu code), the native target selector (TargetSelector.SelectTarget +
//     TrySelectTarget -> CombatController.SelectTarget -> StartAction), cancel targeting,
//     fast travel (MapMenu.ConfirmTeleporting, at a teleporter or directly while exploring),
//     team swap (MonsterManager.SwitchMonsters), equip (EquipmentMenu.EquipNewItem's own
//     three calls after the menu's own enable checks), the follower
//     (PlayerFollower.SwitchMonster, what the native SelectFollower menu calls), the native B of
//     the item / switch windows (MenuList.SelectionCancelled) and the Monster Info cursor
//     (TargetSelector.SelectTarget, what the D-pad calls in that mode);
//   * at the title: the game's own "Press [A] to start" prompt object (GameObject.Find of the
//     name MainMenu.Start looks up, once Unity's splash is finished), published so the
//     companion shows its prompt only with it.
// Pointers the companion passes are only compared, never dereferenced, until they were found
// in the game's own lists.

.text
.global ms_guest_helper_entry

// ---- mailbox offsets (keep in sync with native/modules/ms_helper_mailbox.h) ----
.equ MB_MAGIC,      0x000
.equ MB_HEARTBEAT,  0x004
.equ MB_REQ_SEQ,    0x008
.equ MB_REQ_OP,     0x00C
.equ MB_REQ_EPOCH,  0x010
.equ MB_PTR_A,      0x018
.equ MB_PTR_B,      0x020
.equ MB_ARG_A,      0x030
.equ MB_ARG_B,      0x034
.equ MB_ARG_C,      0x038
.equ MB_RES_SEQ,    0x060
.equ MB_RES_CODE,   0x064
.equ MB_RES_VALUE,  0x068
.equ MB_HIDE_ON,    0x080   // bit 0: menu, bit 1: combo
.equ MB_HIDE_LEASE, 0x084
.equ MB_HIDE_STATE, 0x088
.equ MB_HIDE_TR,    0x090
.equ MB_HIDE_SCALE, 0x098   // 3 x f32
.equ MB_DIAG_GS,    0x0B0
.equ MB_DIAG_CS,    0x0B4
.equ MB_DIAG_REQS,  0x0B8   // requests answered (incl. refusals)
.equ MB_DIAG_PRECULL, 0x0BC  // render-time passes (tk2dCamera.OnPreCull hook), diagnostic
.equ MB_DIAG_SAVES,   0x0A8  // render-time passes that had to hide a window again (the game had
                             // re-scaled it after Update: a frame dev.13 would have drawn)
.equ MB_DIAG_REMARKS, 0x0AC  // held windows found re-scaled by the game, both passes
.equ MB_COMBO_STATE, 0x0C0  // state, padding, transform, original scale (same layout as menu)
// the rest of the battle-menu group (hide bit 1, one 0x20 block each, same layout as menu):
// SkillTooltip, MonsterInfo, CombatItemMenu, ItemTooltip, StartCombatMenu (switch list only)
.equ MB_GROUP_STATE, 0x100
// the title's engagement prompt GameObject ("UnifyEngagementPrompt"), 0 when not found / not
// at the title (written by the guest; GameObject.Find only returns active objects)
.equ MB_TITLE_PROMPT, 0x0E0
.equ MB_GROUP_STRIDE, 0x20
.equ MB_EPOCH,      0xFFC

// ---- result codes (ms_helper_mailbox::Result) ----
.equ R_OK, 0
.equ R_NOT_CONFIGURED, 1
.equ R_BAD_OP, 2
.equ R_STALE_EPOCH, 3
.equ R_NOT_READY, 4
.equ R_WRONG_STATE, 5
.equ R_NOT_PLAYER_TURN, 6
.equ R_MENU_CLOSED, 7
.equ R_BAD_INDEX, 8
.equ R_STALE, 9
.equ R_DISABLED, 10
.equ R_NATIVE_DECLINED, 11
.equ R_NOT_MEMBER, 12
.equ R_SAME, 13
.equ R_SLOT_INVALID, 14
.equ R_BUSY, 15

.equ OP_MAX, 10

// The hide marks the transforms it holds with this scale: (0, 0, HIDE_MARK_Z). Any other value
// there is the game's own (UIOpeningAnim / ScaleTween write (1, 0) .. (1, 1) with z 0), so the
// helper always knows the scale the game last asked for and gives exactly that back.
.equ HIDE_MARK_Z_BITS, 0x3E000000   // 0.125f
// Frames a group window stays held after its gate closes (the player's turn ended, the switch
// list closed): the game's own close animations (0.1 s scale / position tweens) run hidden.
.equ HIDE_GRACE, 30

// ---- game layout (Il2CppDumper dump of this build; byte-checked in RE-REPORT-2) ----
.equ GC_COMBAT, 0xC0
.equ GC_GAMESTATE, 0xC8
.equ GSM_STATE, 0x18
.equ CC_STATE, 0xB8
.equ SM_CURRENT, 0x18
.equ STATE_ID, 0x40
.equ CC_TARGETSEL, 0x60
.equ CC_PLAYERTURN, 0x154
.equ UI_COMBAT, 0x20
.equ UI_INGAME, 0x28
.equ CUI_MENU, 0x18
.equ CUI_ITEMMENU, 0x70
.equ CUI_STARTMENU, 0x90
.equ CUI_TOOLTIP, 0x40
.equ CUI_MONSTERINFO, 0x50
.equ CUI_ITEMTOOLTIP, 0x80
.equ CUI_LEVELSELECT, 0x78   // CombatUIController.LevelSelect (LevelSelectMenu: Y's level popup)
.equ CUI_BUFFINFOMENU, 0xC0  // CombatUIController.BuffInfoMenu
.equ BIM_ISOPEN, 0x28        // BuffInfoMenu.IsOpen
.equ LSM_MENULIST, 0x28      // LevelSelectMenu.MenuList
.equ LSM_ISOPEN, 0x38        // LevelSelectMenu.IsOpen
.equ SCM_ISSWITCH, 0x71
.equ SCM_ISOPEN, 0x72
.equ CM_MENULIST, 0x40
.equ CIM_MENULIST, 0x28
.equ SCM_MONSTERMENU, 0x20
.equ ML_LISTS, 0x58
.equ ML_INPUT, 0x68
.equ ML_SELECTING, 0x69
.equ MLI_DISABLED, 0xD1
.equ TS_TARGETS, 0x40
.equ TS_CURRENT, 0x18
.equ TS_TYPE, 0x28
.equ TS_SELECTING, 0x50
.equ TS_ENEMYINFO, 0x51
.equ TS_LOCKED, 0x52
.equ IGM_MAP, 0x48
.equ MAP_ISOPEN, 0xD8
.equ MAP_TELEPORTS, 0xE0
.equ MAP_START, 0xF0
.equ MAP_OPENTYPE, 0xF8
.equ PC_INVENTORY, 0x20
.equ PC_MINIMAP, 0x28
.equ PC_MENUSTACK, 0x1C0
.equ MM_CURRENTENTRY, 0x18
.equ TS_MINIMAPENTRY, 0x20
.equ UI_POPUP, 0x30
.equ UI_DIALOGUE, 0x50
.equ DV_ISOPEN, 0x88
.equ PC_MONSTERS, 0x98
.equ PC_FOLLOWER, 0x108
.equ MM_ACTIVE, 0x20
.equ MM_INACTIVE, 0x28
.equ INV_WEAPONS, 0x38
.equ INV_ACCESSORIES, 0x40
.equ EQ_TYPE, 0x30
.equ PF_MONSTER, 0x40
.equ PF_STATE, 0x50
.equ LIST_ITEMS, 0x10
.equ LIST_SIZE, 0x18
.equ ARR_DATA, 0x20
.equ UI_BUFFINFO, 0x70      // UIController.BuffInfoOverlay
.equ BIO_ISOPEN, 0x5C       // BuffInfoOverlay.IsOpen (MenuList.Update ignores input while open)
.equ ML_VT_CANCEL, 0x1C8    // MenuList vtable slot 10 SelectionCancelled (method ptr, MethodInfo)

// Save everything the helper touches in a disposable 0xB0-byte frame (x29 = it).
.macro HELPER_FRAME_SAVE
    sub  sp, sp, #0xB0
    stp  x0,  x30, [sp, #0x00]
    stp  x19, x20, [sp, #0x10]
    stp  x21, x22, [sp, #0x20]
    stp  x23, x24, [sp, #0x30]
    stp  x25, x26, [sp, #0x40]
    stp  x27, x28, [sp, #0x50]
    stp  d8,  d9,  [sp, #0x60]
    stp  d10, d11, [sp, #0x70]
    stp  d12, d13, [sp, #0x80]
    stp  d14, d15, [sp, #0x90]
    stp  x29, xzr, [sp, #0xA0]
    mov  x29, sp
.endm
.macro HELPER_FRAME_RESTORE
    ldp  x29, xzr, [sp, #0xA0]
    ldp  x0,  x30, [sp, #0x00]
    ldp  x19, x20, [sp, #0x10]
    ldp  x21, x22, [sp, #0x20]
    ldp  x23, x24, [sp, #0x30]
    ldp  x25, x26, [sp, #0x40]
    ldp  x27, x28, [sp, #0x50]
    ldp  d8,  d9,  [sp, #0x60]
    ldp  d10, d11, [sp, #0x70]
    ldp  d12, d13, [sp, #0x80]
    ldp  d14, d15, [sp, #0x90]
    add  sp, sp, #0xB0
.endm

// x20 GameController, x21 UIController, x22 PlayerController (0 when not there yet);
// w23 GameStates id, w24 CombatStates id (-1 = chain not there).
.macro READ_ROOTS
    adrp x0, ms_slot_game_controller
    add  x0, x0, :lo12:ms_slot_game_controller
    bl   load_instance
    mov  x20, x0
    adrp x0, ms_slot_ui_controller
    add  x0, x0, :lo12:ms_slot_ui_controller
    bl   load_instance
    mov  x21, x0
    adrp x0, ms_slot_player_controller
    add  x0, x0, :lo12:ms_slot_player_controller
    bl   load_instance
    mov  x22, x0
    mov  w23, #-1
    mov  w24, #-1
    cbz  x20, 8f
    ldr  x0, [x20, #GC_GAMESTATE]
    cbz  x0, 7f
    ldr  x0, [x0, #GSM_STATE]
    bl   machine_state
    mov  w23, w0
7:  ldr  x0, [x20, #GC_COMBAT]
    cbz  x0, 8f
    ldr  x0, [x0, #CC_STATE]
    bl   machine_state
    mov  w24, w0
8:
.endm

.macro MAILBOX_READY fail
    adrp x19, ms_mailbox_base
    add  x19, x19, :lo12:ms_mailbox_base
    ldr  w9, [x19, #MB_MAGIC]
    movz w10, #0x534D
    movk w10, #0x3248, lsl #16      // 'MSH2'
    cmp  w9, w10
    b.ne \fail
.endm

ms_guest_helper_entry:
    // --- Reproduce the overwritten entry instruction exactly. ---
    stp  x20, x19, [sp, #-0x20]!
    HELPER_FRAME_SAVE

    adrp x19, ms_mailbox_base
    add  x19, x19, :lo12:ms_mailbox_base
    ldr  w9, [x19, #MB_HEARTBEAT]
    add  w9, w9, #1
    str  w9, [x19, #MB_HEARTBEAT]
    MAILBOX_READY helper_done

    READ_ROOTS
    str  w23, [x19, #MB_DIAG_GS]
    str  w24, [x19, #MB_DIAG_CS]

    bl   battle_menu_hide
    bl   title_prompt

    // --- one request per frame ---
    add  x9, x19, #MB_REQ_SEQ
    ldar w9, [x9]
    ldr  w10, [x19, #MB_RES_SEQ]
    cmp  w9, w10
    b.eq helper_done

    ldr  w11, [x19, #MB_REQ_OP]
    cmp  w11, #1
    b.lt refuse_bad_op
    cmp  w11, #OP_MAX
    b.gt refuse_bad_op

    ldr  w12, [x19, #MB_REQ_EPOCH]
    add  x9, x19, #MB_EPOCH
    ldar w13, [x9]
    cbz  w13, refuse_stale_epoch
    cmp  w12, w13
    b.ne refuse_stale_epoch

    cbz  x20, refuse_not_ready
    cbz  x21, refuse_not_ready
    cbz  x22, refuse_not_ready

    cmp  w11, #1
    b.eq op_menu_pick
    cmp  w11, #2
    b.eq op_target_pick
    cmp  w11, #3
    b.eq op_cancel_target
    cmp  w11, #4
    b.eq op_teleport
    cmp  w11, #5
    b.eq op_team_swap
    cmp  w11, #6
    b.eq op_equip
    cmp  w11, #7
    b.eq op_follower
    cmp  w11, #8
    b.eq op_menu_cancel
    cmp  w11, #9
    b.eq op_info_pick
    cmp  w11, #10
    b.eq op_level_open
    b    refuse_bad_op

// ---------------------------------------------------------------------------
// Battle gate: GameStates.Combat (2), CombatStates.MonsterTurn (3), player's turn.
// Falls through on success with x25 = CombatController; branches to a refusal otherwise.
// ---------------------------------------------------------------------------
.macro BATTLE_GATE
    cmp  w23, #2
    b.ne refuse_wrong_state
    cmp  w24, #3
    b.ne refuse_wrong_state
    ldr  x25, [x20, #GC_COMBAT]
    cbz  x25, refuse_not_ready
    ldrb w9, [x25, #CC_PLAYERTURN]
    cbz  w9, refuse_not_player_turn
.endm

// ---------------------------------------------------------------------------
// MENU_PICK: arg_a = menu (0 command menu, 1 combat item menu, 2 switch monster list, 3 the
// level popup: LevelSelectMenu.OnItemSelected -> CombatController.SelectAction of that level),
// arg_b = column (MenuList.Lists index), arg_c = row,
// ptr_a = the MenuListItem the companion expects there (0 = do not check).
// The native A press: SelectMenuItem moves the cursor (the top screen shows it),
// TrySelectCurrent runs the menu's own checks and its OnItemSelected.
// result_value = MenuList.IsSelecting | TargetSelector.IsSelecting << 1 after the call.
// ---------------------------------------------------------------------------
op_menu_pick:
    BATTLE_GATE
    ldr  x9, [x21, #UI_COMBAT]
    cbz  x9, refuse_not_ready
    ldr  w10, [x19, #MB_ARG_A]
    cmp  w10, #0
    b.eq 1f
    cmp  w10, #1
    b.eq 2f
    cmp  w10, #2
    b.eq 3f
    cmp  w10, #3
    b.eq 4f
    b    refuse_bad_index
4:  ldr  x9, [x9, #CUI_LEVELSELECT]
    cbz  x9, refuse_menu_closed
    ldrb w10, [x9, #LSM_ISOPEN]
    cbz  w10, refuse_menu_closed
    ldr  x26, [x9, #LSM_MENULIST]
    b    5f
1:  ldr  x9, [x9, #CUI_MENU]
    cbz  x9, refuse_menu_closed
    ldr  x26, [x9, #CM_MENULIST]
    b    5f
2:  ldr  x9, [x9, #CUI_ITEMMENU]
    cbz  x9, refuse_menu_closed
    ldr  x26, [x9, #CIM_MENULIST]
    b    5f
3:  ldr  x9, [x9, #CUI_STARTMENU]
    cbz  x9, refuse_menu_closed
    ldr  x26, [x9, #SCM_MONSTERMENU]
5:  cbz  x26, refuse_menu_closed
    ldrb w9, [x26, #ML_SELECTING]
    cbz  w9, refuse_menu_closed
    ldrb w9, [x26, #ML_INPUT]
    cbz  w9, refuse_menu_closed
    // column
    ldr  x0, [x26, #ML_LISTS]
    ldr  w1, [x19, #MB_ARG_B]
    bl   list_item
    cbz  x0, refuse_bad_index
    // row
    ldr  w1, [x19, #MB_ARG_C]
    bl   list_item
    cbz  x0, refuse_bad_index
    mov  x27, x0
    ldr  x9, [x19, #MB_PTR_A]
    cbz  x9, 6f
    cmp  x9, x27
    b.ne refuse_stale
6:  ldrb w9, [x27, #MLI_DISABLED]
    cbnz w9, refuse_disabled
    bl   combat_ui_sig
    mov  w28, w0
    mov  x0, x26
    mov  x1, x27
    mov  w2, #0
    mov  x3, #0
    bl   ms_menulist_select_item
    mov  x0, x26
    mov  x1, #0
    bl   ms_menulist_try_select_current
    // accepted when the native combat UI moved on: another menu or the target selector
    // opened, the menu closed or locked. Unchanged = the menu's own checks said no.
    bl   combat_ui_sig
    str  x0, [x19, #MB_RES_VALUE]
    cmp  w0, w28
    b.eq refuse_native_declined_keep
    b    op_success

// w0 = bits of the combat UI state (uses x21 UIController, x25 CombatController):
// 0 command menu selecting, 1 command menu locked, 2 target selector selecting,
// 3 item menu selecting, 4 switch list selecting, 5 level popup open, 8..15 CombatStates id.
combat_ui_sig:
    mov  w0, #0
    ldr  x9, [x21, #UI_COMBAT]
    cbz  x9, 9f
    ldr  x10, [x9, #CUI_MENU]
    cbz  x10, 1f
    ldr  x10, [x10, #CM_MENULIST]
    cbz  x10, 1f
    ldrb w11, [x10, #ML_SELECTING]
    orr  w0, w0, w11
    ldrb w11, [x10, #0x6A]
    orr  w0, w0, w11, lsl #1
1:  ldr  x10, [x9, #CUI_ITEMMENU]
    cbz  x10, 2f
    ldr  x10, [x10, #CIM_MENULIST]
    cbz  x10, 2f
    ldrb w11, [x10, #ML_SELECTING]
    orr  w0, w0, w11, lsl #3
2:  ldr  x10, [x9, #CUI_LEVELSELECT]
    cbz  x10, 5f
    ldrb w11, [x10, #LSM_ISOPEN]
    orr  w0, w0, w11, lsl #5
5:  ldr  x10, [x9, #CUI_STARTMENU]
    cbz  x10, 9f
    ldr  x10, [x10, #SCM_MONSTERMENU]
    cbz  x10, 9f
    ldrb w11, [x10, #ML_SELECTING]
    orr  w0, w0, w11, lsl #4
9:  ldr  x10, [x25, #CC_TARGETSEL]
    cbz  x10, 3f
    ldrb w11, [x10, #TS_SELECTING]
    orr  w0, w0, w11, lsl #2
3:  ldr  x10, [x25, #CC_STATE]
    cbz  x10, 4f
    ldr  x10, [x10, #SM_CURRENT]
    cbz  x10, 4f
    ldr  w11, [x10, #STATE_ID]
    and  w11, w11, #0xFF
    orr  w0, w0, w11, lsl #8
4:  ret

// ---------------------------------------------------------------------------
// TARGET_PICK: ptr_a = the Monster to target; it must be in TargetSelector.targets.
// SelectTarget moves the native cursor, TrySelectTarget is the native confirm (its own
// ActionTypeRestriction / counterfeit checks, then CombatController.SelectTarget).
// ---------------------------------------------------------------------------
op_target_pick:
    BATTLE_GATE
    ldr  x26, [x25, #CC_TARGETSEL]
    cbz  x26, refuse_menu_closed
    ldrb w9, [x26, #TS_SELECTING]
    cbz  w9, refuse_menu_closed
    ldrb w9, [x26, #TS_ENEMYINFO]
    cbnz w9, refuse_menu_closed
    ldrb w9, [x26, #TS_LOCKED]
    cbnz w9, refuse_menu_closed
    ldr  x0, [x26, #TS_TARGETS]
    ldr  x1, [x19, #MB_PTR_A]
    cbz  x1, refuse_not_member
    bl   list_contains
    cbz  w0, target_group
    mov  x0, x26
    ldr  x1, [x19, #MB_PTR_A]
    mov  w2, #0
    mov  x3, #0
    bl   ms_targetselector_select_target
target_confirm:
    mov  x0, x26
    ldr  x1, [x26, #TS_CURRENT]
    mov  x2, #0
    bl   ms_targetselector_try_select_target
    ldrb w9, [x26, #TS_SELECTING]
    str  x9, [x19, #MB_RES_VALUE]
    cbnz w9, refuse_native_declined_keep
    b    op_success

// A group action (all allies / all enemies) selects the whole side: the tapped monster is not
// in TargetSelector.targets one by one. If it is on the field, confirm what the native
// selector shows (its CurrentTarget) -- the game's own confirm still runs its checks.
target_group:
    // only for AllEnemies (1) / AllAllies (4) actions (TargetSelector.targetType): the native A
    // press confirms the whole side; the tapped monster must be on that side
    ldr  w9, [x26, #TS_TYPE]
    cmp  w9, #1
    b.eq 1f
    cmp  w9, #4
    b.ne refuse_not_member
    ldr  x0, [x25, #0x70]                 // CombatController.PlayerMonsters
    b    2f
1:  ldr  x0, [x25, #0x48]                 // CombatController.Enemies
2:  ldr  x1, [x19, #MB_PTR_A]
    bl   list_contains
    cbz  w0, refuse_not_member
    b    target_confirm

// ---------------------------------------------------------------------------
// CANCEL_TARGET: CombatController.CancelTargetSelection while the selector is open.
// ---------------------------------------------------------------------------
op_cancel_target:
    BATTLE_GATE
    ldr  x26, [x25, #CC_TARGETSEL]
    cbz  x26, refuse_menu_closed
    ldrb w9, [x26, #TS_SELECTING]
    cbz  w9, refuse_menu_closed
    ldrb w9, [x26, #TS_LOCKED]
    cbnz w9, refuse_menu_closed
    mov  x0, x25
    mov  x1, #0
    bl   ms_combat_cancel_target_selection
    str  xzr, [x19, #MB_RES_VALUE]
    b    op_success

// ---------------------------------------------------------------------------
// TELEPORT: ptr_a = a TeleportSelectable in MapMenu.teleports. Only while the native map is
// open at a teleporter (isOpen, openType == Teleporting) -- the only way the game itself
// reaches ConfirmTeleporting. The start teleporter is refused (natively a silent cancel).
// ---------------------------------------------------------------------------
op_teleport:
    ldr  x9, [x21, #UI_INGAME]
    cbz  x9, refuse_not_ready
    ldr  x26, [x9, #IGM_MAP]
    cbz  x26, refuse_not_ready
    // allowed: the native teleporter map is open (the game's own route), or plain exploring
    // with no menu, dialogue, popup, combat or loading (direct fast travel, owner decision)
    ldrb w9, [x26, #MAP_ISOPEN]
    cbz  w9, 1f
    ldr  w9, [x26, #MAP_OPENTYPE]
    cmp  w9, #2
    b.ne refuse_wrong_state
    b    2f
1:  cmp  w23, #1                         // GameStates.Exploring
    b.ne refuse_wrong_state
    ldr  x9, [x22, #PC_MENUSTACK]
    cbz  x9, 3f
    ldr  w9, [x9, #LIST_SIZE]
    cbnz w9, refuse_wrong_state
3:  ldr  x9, [x21, #UI_DIALOGUE]
    cbz  x9, 4f
    ldrb w9, [x9, #DV_ISOPEN]
    cbnz w9, refuse_wrong_state
4:  ldr  x0, [x21, #UI_POPUP]
    cbz  x0, 2f
    mov  x1, #0
    bl   ms_popup_is_open
    tbnz w0, #0, refuse_wrong_state
2:  // only teleporters the game lists: explored teleporter rooms (MapMenu.InitMapTiles,
    // rebuilt on every scene load)
    ldr  x1, [x19, #MB_PTR_A]
    cbz  x1, refuse_not_member
    ldr  x0, [x26, #MAP_TELEPORTS]
    bl   list_contains
    cbz  w0, refuse_not_member
    ldr  x1, [x19, #MB_PTR_A]
    // the teleporter where the player is: its room is MinimapManager.CurrentEntry
    // (StartTeleport is only set while the menu is open; equal there = a silent cancel)
    ldr  x9, [x1, #TS_MINIMAPENTRY]
    ldr  x10, [x22, #PC_MINIMAP]
    cbz  x10, 5f
    ldr  x10, [x10, #MM_CURRENTENTRY]
    cmp  x9, x10
    b.eq refuse_same
5:  ldrb w9, [x26, #MAP_ISOPEN]
    cbz  w9, 6f
    ldr  x9, [x26, #MAP_START]
    cmp  x9, x1
    b.eq refuse_same
6:  mov  x0, x26
    mov  x2, #0
    bl   ms_map_confirm_teleporting
    str  xzr, [x19, #MB_RES_VALUE]
    b    op_success

// ---------------------------------------------------------------------------
// Field gate: GameStates.Exploring (1) only.
// ---------------------------------------------------------------------------
.macro FIELD_GATE
    cmp  w23, #1
    b.ne refuse_wrong_state
.endm

// x0 = Monster* candidate -> w0 = 1 if in MonsterManager.Active or .Inactive. Uses x22.
team_contains:
    str  x30, [sp, #-0x10]!
    mov  x1, x0                       // list_contains keeps x1
    ldr  x9, [x22, #PC_MONSTERS]
    cbz  x9, 2f
    ldr  x0, [x9, #MM_ACTIVE]
    bl   list_contains
    cbnz w0, 1f
    ldr  x9, [x22, #PC_MONSTERS]
    ldr  x0, [x9, #MM_INACTIVE]
    bl   list_contains
1:  ldr  x30, [sp], #0x10
    ret
2:  mov  w0, #0
    ldr  x30, [sp], #0x10
    ret

// ---------------------------------------------------------------------------
// TEAM_SWAP: ptr_a, ptr_b = two different monsters of the team (active or bench).
// ---------------------------------------------------------------------------
op_team_swap:
    FIELD_GATE
    ldr  x26, [x19, #MB_PTR_A]
    ldr  x27, [x19, #MB_PTR_B]
    cbz  x26, refuse_not_member
    cbz  x27, refuse_not_member
    cmp  x26, x27
    b.eq refuse_same
    mov  x0, x26
    bl   team_contains
    cbz  w0, refuse_not_member
    mov  x0, x27
    bl   team_contains
    cbz  w0, refuse_not_member
    ldr  x0, [x22, #PC_MONSTERS]
    cbz  x0, refuse_not_ready
    mov  x1, x26
    mov  x2, x27
    mov  x3, #0
    bl   ms_monstermanager_switch_monsters
    str  xzr, [x19, #MB_RES_VALUE]
    b    op_success

// ---------------------------------------------------------------------------
// EQUIP: ptr_a = Monster (team), ptr_b = InventoryItem in InventoryManager.Weapons (slots 0,1)
// or .Accessories (slots 2..5), arg_a = EquipmentManager.EquipmentSlot.
// The native equipment menu's enable rule (OnShowInventoryItem): not
// Has{Weapon,Accessory}TypeEquipped(slot, equipment) and the item's own virtual
// can-equip(monster) (vtable +0x228). Then EquipNewItem's three calls:
// prev = EquipmentManager.Equip(equipment, slot, false); Inventory.RemoveItem(equipment, 1, 0);
// if prev: Inventory.AddItem(prev, 1, 0). result_value = prev.
// ---------------------------------------------------------------------------
op_equip:
    FIELD_GATE
    ldr  x26, [x19, #MB_PTR_A]               // monster
    cbz  x26, refuse_not_member
    mov  x0, x26
    bl   team_contains
    cbz  w0, refuse_not_member
    ldr  x28, [x22, #PC_INVENTORY]
    cbz  x28, refuse_not_ready
    ldr  w9, [x19, #MB_ARG_A]                 // slot
    cmp  w9, #5
    b.hi refuse_slot_invalid
    ldr  x0, [x28, #INV_WEAPONS]
    cmp  w9, #2
    b.lo 1f
    ldr  x0, [x28, #INV_ACCESSORIES]
1:  ldr  x1, [x19, #MB_PTR_B]
    cbz  x1, refuse_not_member
    bl   list_contains
    cbz  w0, refuse_not_member
    // equipment = InventoryItem.get_Equipment()
    ldr  x0, [x19, #MB_PTR_B]
    mov  x1, #0
    bl   ms_inventoryitem_get_equipment
    cbz  x0, refuse_not_member
    mov  x27, x0
    // EquipmentManager of the monster
    mov  x0, x26
    mov  x1, #0
    bl   ms_monster_get_equipment
    cbz  x0, refuse_not_ready
    mov  x25, x0
    // slot count and slot kind vs equipment type (0 = weapon)
    ldr  w24, [x19, #MB_ARG_A]
    ldr  w9, [x27, #EQ_TYPE]
    cmp  w24, #2
    b.hs 2f
    cbnz w9, refuse_slot_invalid             // accessory into a weapon slot
    mov  x0, x25
    mov  x1, #0
    bl   ms_equipment_weapon_slot_count
    cmp  w24, w0
    b.ge refuse_slot_invalid
    mov  x0, x25
    mov  w1, w24
    mov  x2, x27
    mov  x3, #0
    bl   ms_equipment_has_weapon_type
    cbnz w0, refuse_disabled
    b    3f
2:  cbz  w9, refuse_slot_invalid             // weapon into an accessory slot
    mov  x0, x25
    mov  x1, #0
    bl   ms_equipment_accessory_slot_count
    sub  w9, w24, #2
    cmp  w9, w0
    b.ge refuse_slot_invalid
    mov  x0, x25
    mov  w1, w24
    mov  x2, x27
    mov  x3, #0
    bl   ms_equipment_has_accessory_type
    cbnz w0, refuse_disabled
3:  // the item's own can-equip(monster): vtable slot at klass+0x228 / MethodInfo +0x230
    ldr  x8, [x27]
    ldr  x9, [x8, #0x228]
    ldr  x2, [x8, #0x230]
    mov  x0, x27
    mov  x1, x26
    blr  x9
    tbz  w0, #0, refuse_disabled
    // EquipNewItem's calls
    mov  x0, x25
    mov  x1, x27
    mov  w2, w24
    mov  w3, #0
    mov  x4, #0
    bl   ms_equipment_equip
    mov  x26, x0                              // previous equipment (may be 0)
    mov  x0, x28
    mov  x1, x27
    mov  w2, #1
    mov  w3, #0
    mov  x4, #0
    bl   ms_inventory_remove_item
    cbz  x26, 4f
    mov  x0, x28
    mov  x1, x26
    mov  w2, #1
    mov  w3, #0
    mov  x4, #0
    bl   ms_inventory_add_item
4:  str  x26, [x19, #MB_RES_VALUE]
    b    op_success

// ---------------------------------------------------------------------------
// FOLLOWER: ptr_a = a team monster; PlayerFollower.SwitchMonster(monster), the call the
// native SelectFollower menu makes. Refused while the follower is dashing / using its
// explore action (FollowerState 2, 3) or when it already is that monster.
// ---------------------------------------------------------------------------
op_follower:
    FIELD_GATE
    ldr  x26, [x19, #MB_PTR_A]
    cbz  x26, refuse_not_member
    mov  x0, x26
    bl   team_contains
    cbz  w0, refuse_not_member
    ldr  x27, [x22, #PC_FOLLOWER]
    cbz  x27, refuse_not_ready
    ldr  x9, [x27, #PF_MONSTER]
    cmp  x9, x26
    b.eq refuse_same
    ldr  w9, [x27, #PF_STATE]
    cmp  w9, #2
    b.hs refuse_busy
    mov  x0, x27
    mov  x1, x26
    mov  x2, #0
    bl   ms_follower_switch_monster
    ldr  x9, [x27, #PF_MONSTER]
    str  x9, [x19, #MB_RES_VALUE]
    b    op_success

// ---------------------------------------------------------------------------
// MENU_CANCEL: arg_a = menu (1 combat item menu, 2 the in-battle Switch Monster list). The
// native B: MenuList.Update calls its virtual SelectionCancelled (vtable slot 10) while the list
// is selecting, takes input and the buff info overlay is closed; that runs the menu's own
// OnSelectionCancelled (CombatItemMenu / StartCombatMenu.MonsterSelectionCancelled: close it,
// back to the command menu) and the cancel sound. Same gates here.
// result_value = combat_ui_sig after the call; unchanged = NativeDeclined.
// ---------------------------------------------------------------------------
op_menu_cancel:
    BATTLE_GATE
    ldr  x9, [x21, #UI_COMBAT]
    cbz  x9, refuse_not_ready
    ldr  w10, [x19, #MB_ARG_A]
    cmp  w10, #1
    b.eq 1f
    cmp  w10, #3
    b.eq 4f
    cmp  w10, #2
    b.ne refuse_bad_index
    ldr  x9, [x9, #CUI_STARTMENU]
    cbz  x9, refuse_menu_closed
    ldrb w10, [x9, #SCM_ISSWITCH]             // only the in-battle Switch Monster list
    cbz  w10, refuse_menu_closed
    ldr  x26, [x9, #SCM_MONSTERMENU]
    b    2f
1:  ldr  x9, [x9, #CUI_ITEMMENU]
    cbz  x9, refuse_menu_closed
    ldr  x26, [x9, #CIM_MENULIST]
    b    2f
4:  ldr  x9, [x9, #CUI_LEVELSELECT]              // the level popup: LevelSelectMenu.OnSelectionCancelled
    cbz  x9, refuse_menu_closed
    ldrb w10, [x9, #LSM_ISOPEN]
    cbz  w10, refuse_menu_closed
    ldr  x26, [x9, #LSM_MENULIST]
2:  cbz  x26, refuse_menu_closed
    ldrb w9, [x26, #ML_SELECTING]
    cbz  w9, refuse_menu_closed
    ldrb w9, [x26, #ML_INPUT]
    cbz  w9, refuse_menu_closed
    ldr  x9, [x21, #UI_BUFFINFO]
    cbz  x9, 3f
    ldrb w9, [x9, #BIO_ISOPEN]
    cbnz w9, refuse_wrong_state
3:  bl   combat_ui_sig
    mov  w28, w0
    ldr  x8, [x26]
    ldr  x9, [x8, #ML_VT_CANCEL]
    ldr  x1, [x8, #ML_VT_CANCEL + 8]
    mov  x0, x26
    blr  x9
    bl   combat_ui_sig
    str  x0, [x19, #MB_RES_VALUE]
    cmp  w0, w28
    b.eq refuse_native_declined_keep
    b    op_success

// ---------------------------------------------------------------------------
// INFO_PICK: ptr_a = a Monster in TargetSelector.targets while the selector browses in
// IsEnemyInfo mode (the Monster Info command: targets = enemies + allies). What the D-pad does
// there: SelectTarget(monster, animate) -- the cursor moves and the native MonsterInfo card shows
// that monster (UpdateMonsterInfo). No confirm. result_value = CurrentTarget after the call.
// ---------------------------------------------------------------------------
op_info_pick:
    BATTLE_GATE
    ldr  x26, [x25, #CC_TARGETSEL]
    cbz  x26, refuse_menu_closed
    ldrb w9, [x26, #TS_SELECTING]
    cbz  w9, refuse_menu_closed
    ldrb w9, [x26, #TS_ENEMYINFO]
    cbz  w9, refuse_wrong_state
    ldrb w9, [x26, #TS_LOCKED]
    cbnz w9, refuse_menu_closed
    ldr  x1, [x19, #MB_PTR_A]
    cbz  x1, refuse_not_member
    ldr  x9, [x26, #TS_CURRENT]
    cmp  x9, x1
    b.eq refuse_same
    ldr  x0, [x26, #TS_TARGETS]
    bl   list_contains
    cbz  w0, refuse_not_member
    mov  x0, x26
    ldr  x1, [x19, #MB_PTR_A]
    mov  w2, #1
    mov  x3, #0
    bl   ms_targetselector_select_target
    ldr  x9, [x26, #TS_CURRENT]
    str  x9, [x19, #MB_RES_VALUE]
    ldr  x10, [x19, #MB_PTR_A]
    cmp  x9, x10
    b.ne refuse_native_declined_keep
    b    op_success

// ---------------------------------------------------------------------------
// LEVEL_OPEN: arg_b = column, arg_c = row of a skill in the command window, ptr_a = the
// MenuListItem the companion expects there (0 = do not check). The native Y: CombatController
// .Update calls ProcessMonsterLevelSelection on Y while the battle is interactive and neither the
// BuffInfoMenu nor a popup is open; it opens CombatUIController.LevelSelect for the hovered row's
// action when CombatMenu.ActionHasMultipleLevels. Here: the same gates, SelectMenuItem moves the
// native cursor to the row (OnItemHovered), then TrySelectMonsterLevel (IsInteractive ->
// ProcessMonsterLevelSelection). OK when the popup is open afterwards, else NativeDeclined.
// ---------------------------------------------------------------------------
op_level_open:
    BATTLE_GATE
    ldr  x9, [x21, #UI_COMBAT]
    cbz  x9, refuse_not_ready
    ldr  x10, [x9, #CUI_LEVELSELECT]
    cbz  x10, refuse_not_ready
    ldrb w11, [x10, #LSM_ISOPEN]
    cbnz w11, refuse_same
    ldr  x10, [x9, #CUI_BUFFINFOMENU]
    cbz  x10, 1f
    ldrb w11, [x10, #BIM_ISOPEN]
    cbnz w11, refuse_wrong_state
1:  ldr  x9, [x9, #CUI_MENU]
    cbz  x9, refuse_menu_closed
    ldr  x26, [x9, #CM_MENULIST]
    cbz  x26, refuse_menu_closed
    ldrb w9, [x26, #ML_SELECTING]
    cbz  w9, refuse_menu_closed
    ldrb w9, [x26, #ML_INPUT]
    cbz  w9, refuse_menu_closed
    ldr  x0, [x21, #UI_POPUP]
    cbz  x0, 2f
    mov  x1, #0
    bl   ms_popup_is_open
    tbnz w0, #0, refuse_wrong_state
2:  ldr  x0, [x26, #ML_LISTS]
    ldr  w1, [x19, #MB_ARG_B]
    cbz  w1, refuse_bad_index                 // column 0 is the command rail, no levels there
    bl   list_item
    cbz  x0, refuse_bad_index
    ldr  w1, [x19, #MB_ARG_C]
    bl   list_item
    cbz  x0, refuse_bad_index
    mov  x27, x0
    ldr  x9, [x19, #MB_PTR_A]
    cbz  x9, 3f
    cmp  x9, x27
    b.ne refuse_stale
3:  mov  x0, x26
    mov  x1, x27
    mov  w2, #0
    mov  x3, #0
    bl   ms_menulist_select_item
    mov  x0, x25
    mov  x1, #0
    bl   ms_combat_try_select_monster_level
    // re-read after the native calls; a vanished controller or popup reads as not open
    ldr  x9, [x21, #UI_COMBAT]
    cbz  x9, 4f
    ldr  x9, [x9, #CUI_LEVELSELECT]
    cbz  x9, 4f
    ldrb w9, [x9, #LSM_ISOPEN]
4:  str  x9, [x19, #MB_RES_VALUE]
    cbz  w9, refuse_native_declined_keep
    b    op_success

// ---------------------------------------------------------------------------
// The title's own "Press [A] to start" prompt: MainMenu.Start finds the GameObject named
// "UnifyEngagementPrompt" and shows it while the game waits for the press. It is already active
// while Unity's start-up splash (the publisher logos) still covers the title, so it counts only
// once UnityEngine.Rendering.SplashScreen.isFinished. At GameStates 0 the helper checks every 8th
// frame: splash finished, then the same GameObject.Find (it only returns an active object), and
// publishes the object at MB_TITLE_PROMPT; anywhere else the word is 0. The literal's metadata
// slot is used only once the game has initialised it (MainMenu.Start ran).
// ---------------------------------------------------------------------------
title_prompt:
    cbnz w23, 2f
    ldr  w9, [x19, #MB_HEARTBEAT]
    tst  w9, #7
    b.ne 3f
    adrp x9, ms_slot_lit_engagement_prompt
    add  x9, x9, :lo12:ms_slot_lit_engagement_prompt
    ldr  x9, [x9]
    cbz  x9, 2f
    tst  x9, #7
    b.ne 2f
    str  x30, [sp, #-0x10]!
    mov  x0, #0
    bl   ms_splash_is_finished
    tbz  w0, #0, 1f
    adrp x9, ms_slot_lit_engagement_prompt
    add  x9, x9, :lo12:ms_slot_lit_engagement_prompt
    ldr  x0, [x9]
    mov  x1, #0
    bl   ms_gameobject_find
    str  x0, [x19, #MB_TITLE_PROMPT]
    ldr  x30, [sp], #0x10
    ret
1:  ldr  x30, [sp], #0x10
2:  str  xzr, [x19, #MB_TITLE_PROMPT]
3:  ret

// ---------------------------------------------------------------------------
// Independent menu/combo hide. While its MB_HIDE_ON bit is set and the companion's lease (a
// heartbeat value it renews every sample) has not run out, the component's transform is held at
// the marked scale (0, 0, HIDE_MARK_Z) -- the menu still runs, takes input and moves its cursor;
// it is just not drawn. Runs from GameStateManager.Update and again from tk2dCamera.OnPreCull,
// so nothing the game does during the frame reaches a rendered frame. Whenever the game writes
// its own scale (the opening / closing animations do, every frame they run) the helper keeps
// that value and marks the transform again; released, the transform gets the game's latest
// value back, and nothing at all if the game has written one since the last mark.
// ---------------------------------------------------------------------------
battle_menu_hide:
    stp  x30, x28, [sp, #-0x20]!
    str  x27, [sp, #0x10]
    // x27 = CombatUIController (0 when not there)
    mov  x27, #0
    cbz  x21, 1f
    ldr  x27, [x21, #UI_COMBAT]
1:  // the command menu: whenever the companion asks (verified desktop + the Thor's hide=1/1)
    mov  x0, #0
    cbz  x27, 2f
    ldr  x0, [x27, #CUI_MENU]
2:  add  x1, x19, #MB_HIDE_STATE
    mov  w2, #1
    mov  w3, #1
    bl   hide_component
    // the combo counter (its own option, bit 2)
    mov  x0, #0
    cbz  x27, 3f
    ldr  x0, [x27, #0x48]                       // CombatUIController.ComboView
3:  add  x1, x19, #MB_COMBO_STATE
    mov  w2, #2
    mov  w3, #1
    bl   hide_component
    // the windows the bottom mirrors on the player's turn (CombatStates.MonsterTurn = 3); at the
    // battle's start screen (Intro), the Keeper lead pick and the result they stay drawn. A
    // closed item window (after an item use) is given back only as the game last scaled it
    // (its close animation), so it does not show over the Combat Result.
    cmp  w24, #3
    cset w28, eq
    mov  x0, #CUI_TOOLTIP
    mov  x1, #0
    bl   hide_group_member
    mov  x0, #CUI_MONSTERINFO
    mov  x1, #1
    bl   hide_group_member
    mov  x0, #CUI_ITEMMENU
    mov  x1, #2
    bl   hide_group_member
    mov  x0, #CUI_ITEMTOOLTIP
    mov  x1, #3
    bl   hide_group_member
    // the level popup (Y on a skill with levels, owner round 3)
    mov  x0, #CUI_LEVELSELECT
    mov  x1, #5
    bl   hide_group_member
    // StartCombatMenu only while it is the in-battle Switch Monster list (IsSwitchSelect + IsOpen)
    mov  x0, #0
    mov  w3, #0
    cbz  x27, 4f
    ldr  x0, [x27, #CUI_STARTMENU]
    cbz  x0, 4f
    cbz  w28, 4f
    ldrb w9, [x0, #SCM_ISSWITCH]
    ldrb w10, [x0, #SCM_ISOPEN]
    and  w3, w9, w10
4:  add  x1, x19, #MB_GROUP_STATE + 4 * MB_GROUP_STRIDE
    mov  w2, #1
    bl   hide_component
    ldr  x27, [sp, #0x10]
    ldp  x30, x28, [sp], #0x20
    ret

// x0 = CombatUIController field offset, x1 = group slot; gate w28 (MonsterTurn), bit 1.
hide_group_member:
    stp  x30, xzr, [sp, #-0x10]!
    add  x9, x19, #MB_GROUP_STATE
    add  x1, x9, x1, lsl #5
    cbz  x27, 1f
    ldr  x0, [x27, x0]
    b    2f
1:  mov  x0, #0
2:  mov  w2, #1
    mov  w3, w28
    bl   hide_component
    ldp  x30, xzr, [sp], #0x10
    ret

// x0 = component, x1 = its saved-state block (+0 held, +4 heartbeat its gate was last open,
// +8 transform, +0x10 the game's latest scale, 3 x f32), w2 = hide flag mask, w3 = gate (0 =
// shown in this state: given back HIDE_GRACE frames after the gate closed). Shared lease.
hide_component:
    stp  x30, x25, [sp, #-0x30]!
    stp  x26, x27, [sp, #0x10]
    stp  x28, x24, [sp, #0x20]
    mov  x25, x0
    mov  x26, x1
    mov  w27, w2
    mov  w28, w3
    // hold = hide_on && lease && component && (gate || (held && within the grace))
    ldr  w9, [x19, #MB_HIDE_ON]
    tst  w9, w27
    b.eq hide_release
    ldr  w9, [x19, #MB_HIDE_LEASE]
    ldr  w10, [x19, #MB_HEARTBEAT]
    sub  w9, w9, w10
    tbnz w9, #31, hide_release
    cbz  x25, hide_release
    cbz  w28, 1f
    str  w10, [x26, #4]                         // the gate is open this frame
    b    2f
1:  ldr  w9, [x26, #0]
    cbz  w9, hide_out                           // not held: nothing to keep hidden
    ldr  w9, [x26, #4]
    sub  w9, w10, w9
    cmp  w9, #HIDE_GRACE
    b.hs hide_release
2:  mov  x0, x25
    mov  x1, #0
    bl   ms_component_get_transform
    cbz  x0, hide_out
    mov  x25, x0                                // transform
    ldr  w9, [x26, #0]
    cbz  w9, 3f
    ldr  x10, [x26, #8]
    cmp  x10, x25
    b.eq 3f
    str  wzr, [x26, #0]                         // another object than the one held: forget it
3:  mov  x0, x25
    mov  x1, #0
    bl   ms_transform_get_local_scale
    bl   scale_is_mark
    cbz  w0, 4f
    // still our mark: nothing changed since; unless the held state was lost (adopt it, the
    // resting scale of the game's windows is 1)
    ldr  w9, [x26, #0]
    cbnz w9, hide_out
    fmov s0, #1.0
    fmov s1, #1.0
    fmov s2, #1.0
4:  // the game's own latest scale (first hold, or written since the last mark): keep it, mark
    ldr  w9, [x26, #0]
    cbz  w9, 5f
    ldr  w9, [x19, #MB_DIAG_REMARKS]
    add  w9, w9, #1
    str  w9, [x19, #MB_DIAG_REMARKS]
5:  str  s0, [x26, #16]
    str  s1, [x26, #20]
    str  s2, [x26, #24]
    str  x25, [x26, #8]
    mov  w9, #1
    str  w9, [x26, #0]
    movi d0, #0
    movi d1, #0
    movz w9, #(HIDE_MARK_Z_BITS >> 16), lsl #16
    fmov s2, w9
    mov  x0, x25
    mov  x1, #0
    bl   ms_transform_set_local_scale
    b    hide_out
hide_release:
    ldr  w9, [x26, #0]
    cbz  w9, hide_out
    str  wzr, [x26, #0]
    cbz  x25, hide_out                          // the menu is gone: nothing to give back
    mov  x0, x25
    mov  x1, #0
    bl   ms_component_get_transform
    ldr  x10, [x26, #8]
    cmp  x0, x10
    b.ne hide_out
    mov  x25, x0
    mov  x1, #0
    bl   ms_transform_get_local_scale
    bl   scale_is_mark
    cbz  w0, hide_out                           // the game set its own scale since: keep it
    ldr  s0, [x26, #16]
    ldr  s1, [x26, #20]
    ldr  s2, [x26, #24]
    mov  x0, x25
    mov  x1, #0
    bl   ms_transform_set_local_scale
hide_out:
    ldp  x28, x24, [sp, #0x20]
    ldp  x26, x27, [sp, #0x10]
    ldp  x30, x25, [sp], #0x30
    ret

// s0..s2 = a localScale -> w0 = 1 when it is the hide's own mark (0, 0, HIDE_MARK_Z), bit exact.
// Leaf; keeps s0..s2.
scale_is_mark:
    fmov w9, s0
    fmov w10, s1
    orr  w9, w9, w10
    fmov w10, s2
    movz w11, #(HIDE_MARK_Z_BITS >> 16), lsl #16
    cmp  w10, w11
    ccmp w9, #0, #0, eq
    cset w0, eq
    ret

// ---------------------------------------------------------------------------
// Small helpers (local bl/ret: x30 here is our own chain, already saved).
// ---------------------------------------------------------------------------
// x0 = address of a TypeInfo slot -> x0 = static field 0 (the Instance) or 0.
load_instance:
    ldr  x9, [x0]
    cbz  x9, 1f
    tst  x9, #7                       // an uninitialised slot holds an odd metadata token
    b.ne 1f
    ldr  x9, [x9, #0xB8]
    cbz  x9, 1f
    ldr  x0, [x9]
    ret
1:  mov  x0, #0
    ret

// x0 = StateMachine<T>* -> w0 = CurrentState.ID or -1.
machine_state:
    cbz  x0, 1f
    ldr  x0, [x0, #SM_CURRENT]
    cbz  x0, 1f
    ldr  w0, [x0, #STATE_ID]
    ret
1:  mov  w0, #-1
    ret

// x0 = List<T>*, w1 = index -> x0 = element or 0 when out of range.
list_item:
    cbz  x0, 1f
    ldr  w9, [x0, #LIST_SIZE]
    cmp  w1, w9
    b.hs 1f
    ldr  x9, [x0, #LIST_ITEMS]
    cbz  x9, 1f
    add  x9, x9, #ARR_DATA
    ldr  x0, [x9, w1, uxtw #3]
    ret
1:  mov  x0, #0
    ret

// x0 = List<T>*, x1 = candidate -> w0 = 1 if found (bounded to 512 entries).
list_contains:
    cbz  x0, 2f
    ldr  w9, [x0, #LIST_SIZE]
    cmp  w9, #512
    b.hi 2f
    ldr  x10, [x0, #LIST_ITEMS]
    cbz  x10, 2f
    add  x10, x10, #ARR_DATA
    mov  w11, #0
1:  cmp  w11, w9
    b.hs 2f
    ldr  x12, [x10, w11, uxtw #3]
    cmp  x12, x1
    b.eq 3f
    add  w11, w11, #1
    b    1b
2:  mov  w0, #0
    ret
3:  mov  w0, #1
    ret

// ---------------------------------------------------------------------------
// Refusals and the common tail: result_code, result_seq = req_seq (release), resume.
// ---------------------------------------------------------------------------
refuse_not_configured:   mov w0, #R_NOT_CONFIGURED;  b acknowledge_refuse
refuse_bad_op:           mov w0, #R_BAD_OP;          b acknowledge_refuse
refuse_stale_epoch:      mov w0, #R_STALE_EPOCH;     b acknowledge_refuse
refuse_not_ready:        mov w0, #R_NOT_READY;       b acknowledge_refuse
refuse_wrong_state:      mov w0, #R_WRONG_STATE;     b acknowledge_refuse
refuse_not_player_turn:  mov w0, #R_NOT_PLAYER_TURN; b acknowledge_refuse
refuse_menu_closed:      mov w0, #R_MENU_CLOSED;     b acknowledge_refuse
refuse_bad_index:        mov w0, #R_BAD_INDEX;       b acknowledge_refuse
refuse_stale:            mov w0, #R_STALE;           b acknowledge_refuse
refuse_disabled:         mov w0, #R_DISABLED;        b acknowledge_refuse
refuse_not_member:       mov w0, #R_NOT_MEMBER;      b acknowledge_refuse
refuse_same:             mov w0, #R_SAME;            b acknowledge_refuse
refuse_slot_invalid:     mov w0, #R_SLOT_INVALID;    b acknowledge_refuse
refuse_busy:             mov w0, #R_BUSY;            b acknowledge_refuse
// the native call ran but declined: keep the result_value written by the op
refuse_native_declined_keep:
    mov  w0, #R_NATIVE_DECLINED
    b    acknowledge_common

acknowledge_refuse:
    str  xzr, [x19, #MB_RES_VALUE]
    b    acknowledge_common
op_success:
    mov  w0, #R_OK
acknowledge_common:
    str  w0, [x19, #MB_RES_CODE]
    ldr  w9, [x19, #MB_DIAG_REQS]
    add  w9, w9, #1
    str  w9, [x19, #MB_DIAG_REQS]
    ldr  w9, [x19, #MB_REQ_SEQ]
    add  x10, x19, #MB_RES_SEQ
    stlr w9, [x10]

helper_done:
    HELPER_FRAME_RESTORE
    b    ms_resume

// ---------------------------------------------------------------------------
// Second hook: tk2dCamera.OnPreCull(), RVA 0x13396A0, entry `str x19, [sp, #-0x20]!`
// (f30f1ef8), resumes at 0x13396A4. Unity calls it on the main thread right before a camera
// culls and renders, i.e. after every Update, coroutine, LateUpdate and tween of the frame. The
// battle-menu hide runs here once more, so whatever the game's own code did to a held window
// this frame (UIOpeningAnim.AnimateElement sets its scale to (1, 0) and a ScaleTween grows it
// to (1, 1) over 0.1 s; a bottom tap or the A press opens a window from inside Update) is undone
// before the frame is drawn. No heartbeat, no requests here (several cameras may call it).
// ---------------------------------------------------------------------------
.global ms_guest_helper_precull
ms_guest_helper_precull:
    str  x19, [sp, #-0x20]!
    HELPER_FRAME_SAVE
    MAILBOX_READY precull_done
    ldr  w9, [x19, #MB_DIAG_PRECULL]
    add  w9, w9, #1
    str  w9, [x19, #MB_DIAG_PRECULL]
    READ_ROOTS
    ldr  w25, [x19, #MB_DIAG_REMARKS]
    bl   battle_menu_hide
    ldr  w9, [x19, #MB_DIAG_REMARKS]
    cmp  w9, w25
    b.eq precull_done
    ldr  w9, [x19, #MB_DIAG_SAVES]
    add  w9, w9, #1
    str  w9, [x19, #MB_DIAG_SAVES]
precull_done:
    HELPER_FRAME_RESTORE
    b    ms_resume_precull
