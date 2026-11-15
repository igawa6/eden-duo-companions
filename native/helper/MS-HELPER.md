# Monster Sanctuary guest helper (format-2 load plan)

Title `0100A0F00DA68000`, update v524288 (Ver 2.1.0.32), main build
`8EA928B6AA10C84F9E69129C45BCAAAF`. Source `ms_guest_helper.S`; plan built by
`tools/monster-sanctuary/build_helper_plan.py` from the player's own exefs `main` (`MS_NSO`),
written to `packages/MonsterSanctuary/dualscreen/guest/<build>/load-plan.json` and referenced by
the manifest's `load_plan`. Module side: `native/modules/ms_helper_mailbox.h`.

## Hook

`GameStateManager.Update` RVA `0x4FABC0`, first instruction `stp x20, x19, [sp, #-0x20]!`
(`f44fbea9`), replaced by `b code`. The helper re-executes that instruction, saves everything it
touches in its own 0xB0-byte frame and branches back to `0x4FABC4` (the Belmont "reproduce and
resume" pattern, README.md). It runs once per frame on Unity's main thread. The builder checks
the hook bytes and that every called RVA still starts with a prologue / one-load getter.

Second hook (render-time hide, 2026-11-13): `tk2dCamera.OnPreCull` RVA `0x13396A0`, first
instruction `str x19, [sp, #-0x20]!` (`f30f1ef8`), entry `ms_guest_helper_precull`, resumes at
`0x13396A4`. Unity calls it on the main thread right before each camera culls (after every
Update, coroutine, LateUpdate and tween of the frame; three tk2d cameras, so ~3 calls a frame).
It runs only the battle-menu hide: no heartbeat, no requests.

Roots: TypeInfo slots GameController `0x4A9B980`, UIController `0x4A9D240`, PlayerController
`0x4A9CBE0` (an odd value = metadata token, not initialised yet) -> klass `+0xB8` -> slot 0.
(RE-REPORT-2's `0x49DE010`/`0x49DF1D0`/`0x49F82B0` are the code's GOT cells pointing at these.)

## Mailbox (4096 bytes, epoch at 0xFFC)

| Off | Field | Writer |
|---|---|---|
| 0x000 | magic `MSH2` (0x3248534D) | module, last at configure |
| 0x004 | heartbeat | guest, every frame |
| 0x008 | req_seq (release, written last) | module |
| 0x00C | op | module |
| 0x010 | req_epoch (= word at 0xFFC) | module |
| 0x018 / 0x020 | ptr_a / ptr_b | module |
| 0x030 / 0x034 / 0x038 | arg_a / arg_b / arg_c | module |
| 0x060 | result_seq (release) | guest |
| 0x064 | result code | guest |
| 0x068 | result value | guest |
| 0x080 | hide bits: 1 menu, 2 combo | module |
| 0x084 | hide lease (heartbeat value) | module, every sample |
| 0x088 / 0x090 / 0x098 | hidden flag / transform / saved scale (3 f32) | guest |
| 0x0A8 / 0x0AC | render-time passes that had to hide a window again / held windows found re-scaled by the game (diagnostic) | guest |
| 0x0B0 / 0x0B4 / 0x0B8 / 0x0BC | GameStates id / CombatStates id / answered count / render-time passes | guest |
| 0x0C0 / 0x0C8 / 0x0D0 | combo hidden flag / transform / saved scale (3 f32) | guest |
| 0x0E0 | the title's "Press [A] to start" GameObject while the game shows it, else 0 (u64) | guest |
| 0x100 + k*0x20 | battle-menu group k (0 SkillTooltip, 1 MonsterInfo, 2 CombatItemMenu, 3 ItemTooltip, 4 StartCombatMenu, 5 LevelSelectMenu): hidden flag / transform / saved scale | guest |

## Ops (one per frame, each gated, every refusal answered)

| Op | Gate | What runs |
|---|---|---|
| 1 MENU_PICK (arg_a menu 0 command / 1 items / 2 switch list / 3 level popup, arg_b column, arg_c row, ptr_a expected item) | Combat, CombatStates.MonsterTurn, player turn, menu IsSelecting+InputEnabled, row not IsDisabled; the level popup only while LevelSelectMenu.IsOpen | `MenuList.SelectMenuItem` (native cursor) + `TrySelectCurrent` (the A press: SelectAction / item -> UseConsumable / Switch Monster -> SwitchMonster / a level -> LevelSelectMenu.OnItemSelected -> SelectAction of that level) |
| 2 TARGET_PICK (ptr_a Monster) | as above + TargetSelector selecting, not locked / enemy info; monster in `targets` (group actions: on the field) | `TargetSelector.SelectTarget` + `TrySelectTarget` (CanTargetMonster, counterfeit, -> SelectTarget -> StartAction) |
| 3 CANCEL_TARGET | selector open | `CombatController.CancelTargetSelection` |
| 4 TELEPORT (ptr_a TeleportSelectable) | MapMenu open with openType Teleporting (the game's only route); in `teleports`; not `StartTeleport` (natively a silent cancel) | `MapMenu.ConfirmTeleporting` |
| 5 TEAM_SWAP (ptr_a, ptr_b) | Exploring; both in MonsterManager.Active/Inactive; different | `MonsterManager.SwitchMonsters` |
| 6 EQUIP (ptr_a Monster, ptr_b InventoryItem, arg_a slot) | Exploring; team monster; item in Inventory.Weapons (slot 0-1) / Accessories (2-5); slot kind, slot count; not Has{Weapon,Accessory}TypeEquipped; item's can-equip (vtable +0x228) | EquipNewItem's calls: `EquipmentManager.Equip`, `InventoryManager.RemoveItem`, `AddItem(previous)` |
| 7 FOLLOWER (ptr_a Monster) | Exploring; team monster; not already; follower not dashing / using ability | `PlayerFollower.SwitchMonster` (what the native SelectFollower menu calls) |
| 8 MENU_CANCEL (arg_a menu 1 items / 2 switch list / 3 level popup) | Combat, MonsterTurn, player turn; the list selecting + taking input; the switch list only with IsSwitchSelect; BuffInfoOverlay closed (MenuList.Update ignores input then) | the native B: the list's virtual `SelectionCancelled` (vtable slot 10, +0x1C8) -> CombatItemMenu.OnSelectionCancelled / StartCombatMenu.MonsterSelectionCancelled + cancel sound |
| 9 INFO_PICK (ptr_a Monster) | as above + TargetSelector selecting in IsEnemyInfo mode, not locked; monster in `targets` (enemies + allies); not the current one | `TargetSelector.SelectTarget(monster, animate)`: what the D-pad calls there; the native MonsterInfo card updates (UpdateMonsterInfo) |
| 10 LEVEL_OPEN (arg_b column 1..2, arg_c row, ptr_a expected item) | Combat, MonsterTurn, player turn; the command MenuList selecting + taking input; LevelSelectMenu not open; BuffInfoMenu (+0xC0, IsOpen +0x28) closed; no popup (PopupController.IsOpen) -- CombatController.Update's own gates for Y | `MenuList.SelectMenuItem` (the cursor moves to the skill: OnItemHovered) + `CombatController.TrySelectMonsterLevel` 0x252100 (IsInteractive -> ProcessMonsterLevelSelection: ActionHasMultipleLevels -> StartLevelSelection -> LevelSelectMenu.Open). OK when LevelSelectMenu.IsOpen afterwards |

MENU_CANCEL, like MENU_PICK, answers OK when the combat UI changed and NativeDeclined when not;
INFO_PICK answers OK when TargetSelector.CurrentTarget is the asked monster afterwards.

combat_ui_sig bit 5 is LevelSelectMenu.IsOpen (owner round 3), so a level pick or the popup's
cancel counts as a change.

MENU_PICK answers OK when the combat UI changed (another menu / target selector opened, the menu
closed or locked, the combat state moved) and NativeDeclined when nothing changed.

Result codes: 0 OK, 1 not configured, 2 bad op, 3 stale epoch, 4 not ready, 5 wrong state, 6 not
player turn, 7 menu closed, 8 bad index, 9 stale, 10 disabled, 11 native declined, 12 not member,
13 same, 14 slot invalid, 15 busy.

## Battle-menu and combo hide

While the respective hide bit is set and the lease (heartbeat + 90 frames, renewed every module
sample) holds, `CombatUIController.Menu` or `CombatUIController.ComboView` (+0x48) is held at the
marked scale (0, 0, 0.125). The menu still runs, takes input and moves its cursor, it is only not
drawn. Lease lapses (bottom screen not sampling), the option turned off, or module Destroy
(writes hide off) -> the scale is given back. Each component has independent saved state (+0
held, +4 heartbeat its gate was last open, +8 transform, +0x10 scale). Menu hide defaults On;
combo hide defaults Off.

Hide bit 1 covers the whole battle-menu group the bottom mirrors (owner round 2, 2026-11-12):
the command menu (`CombatUIController.Menu` +0x18) always, and, while the combat state is
MonsterTurn (3), the SkillTooltip (+0x40), MonsterInfo card (+0x50; target select and the Monster
Info command), CombatItemMenu (+0x70), ItemTooltip (+0x80), the StartCombatMenu (+0x90) while
it is the in-battle Switch Monster list (IsSwitchSelect +0x71 and IsOpen +0x72) and, since owner
round 3, Y's level popup LevelSelectMenu (+0x78). The battle's
start screen, the Keeper lead pick and the result screens stay drawn. The target selector's arrows
over the monsters on the field are not a window and stay drawn.

**Flicker fix (2026-11-13).** The game itself scales these windows: `MenuList.Open` with
`UseOpeningAnim` calls `UIOpeningAnim.Open` -> `AnimateElement`, which sets the element's
`localScale` to (1, 0) and starts a 0.1 s `ScaleTween` to (1, 1) (`ScaleTween.Update` writes the
scale every frame it runs). dev.13 hid only from `GameStateManager.Update`, so whatever the game
wrote later in the same frame (the tween, a menu opened by the A press or by a bottom tap inside
Update) was drawn: the command window at the start of a turn, a window opening on a command
switch. Now:
- the hide runs again from `tk2dCamera.OnPreCull`, the last thing before the camera culls, so a
  held window is at the mark in every rendered frame;
- any scale that is not the mark is the game's own: the helper keeps it as the scale to give back
  and marks the transform again (so a close animation's end value, e.g. a closed item window
  collapsed to (1, 0), is what comes back, never a stale "open" scale);
- released, a transform still at the mark gets the game's latest scale back; one the game has
  re-scaled since is left alone;
- a group window stays held for 30 frames after its gate closes (the turn ended, the switch list
  closed), so the game's own close animations run hidden too.
dev.13's special case for the closed item window (restore only while IsOpen) is subsumed by the
latest-scale rule.

## Title prompt

At GameStates 0 the helper checks every 8th frame: `UnityEngine.Rendering.SplashScreen.isFinished`
(the publisher logos no longer cover the title), then `GameObject.Find("UnifyEngagementPrompt")`
(the name MainMenu.Start looks up; Find returns active objects only; the literal's metadata slot
0x4ADA8F0 is used only once initialised) and stores the object at 0x0E0; anywhere else 0. The
module shows its "Press [A] to start" only with it (live: the prompt object is active under the
splash, so the splash check is needed).

The `h_detach` action of a diagnostics build (`EDEN_MS_DIAGNOSTICS=1`, ms_config.h; not in the
shipped manifest) runs the Destroy restore path for both, without renewing the lease.
