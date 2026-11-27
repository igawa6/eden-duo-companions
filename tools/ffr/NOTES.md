# FINAL FANTASY RESONANCE companion: research notes

Title `0100606026F12000`, update 1.0.1, build `84E182F72EE1DA4F477804BD05B54C15…`. Unreal Engine 5.6. The game's C++ module is
`/Script/mucha`.

## UE 5.6 layout (this build, Android/Peanuts)

| What | Where |
|---|---|
| FNamePool `Blocks[]` | `main+0x7c54978` (u32 CurrentBlock and u32 cursor just before it). Blocks are 0x20000 bytes. An entry id is block<<16 \| offset/2. The u16 header is wide = bit 0, len = h>>6 |
| FUObjectArray `ObjObjects` | `main+0x7c8e6a0` (Objects**, PreAllocated, Max 2162688, Num, MaxChunks, NumChunks). FUObjectItem stride 0x18 |
| UObject | class +0x10, FName +0x18, outer +0x20, InternalIndex +0xc |
| UStruct | SuperStruct +0x40, Children +0x48, ChildProperties +0x50, PropertiesSize +0x58 |
| FField / FProperty | FFieldClass* +0x8 (its FName at +0), Next +0x18, Name +0x20, ElementSize +0x30, PropertyFlags +0x38, Offset +0x44. Typed refs: Struct/Object/Byte-enum/Set-elem/Map-key at +0x70; Array inner, Map value, Enum (EnumProperty) and Class meta at +0x78. Bool: bytes +0x70..0x73 = size, byte offset, byte mask, field mask |
| UEnum | Names TArray<{FName, int64}> at +0x40 |

The globals sit in `.bss` and move with every update. Ship them per build, or resolve them from the code that accesses them (see
PORTING_A_GAME §2.2).

## Game state

`CPP_MuchaGameInstance` → subsystem `CPP_MuchaSaveDataManager` (+0x60 `m_pGameSaveData`) → `CPP_MuchaGameSaveData` (live, 0x7d0 bytes):

| Offset | Field |
|---|---|
| +0x28 | `SaveData_Player` (currentMoney +0, PlayTime float +0x10, GameDifficulty +0x20, …) |
| +0x60 | `TArray<SaveData_UnitParameter>` (0xa0 each: UnitId, Experience, HitPoint, MagicPoint, baseParameterList, equipVisionId +0x20, EquipWeaponId +0x24, EquipArmorId +0x28, EquipAccessoryIdList +0x30, equipSkillList +0x40, JoinStatus +0x80) |
| +0x90 | `SaveData_Party` (partyList TArray<int>, guestUnitId) |
| +0xa8 | `TArray<SaveData_Inventory>` ({ItemId, Num}) |
| +0x1a8 | `SaveData_Map` (mapId +0, unitLocation FVector(double) +0x8, unitDirection +0x20, lastIslandVisited +0x120) |
| +0x178 | `SaveData_Progress` |

Map: the world subsystem `CPP_MiniMapSubsystem` has MiniMapDA +0x68, AreaManager +0x80 (`CPP_MiniMapAreaManager`: AreaDataMap +0x40,
CurrentAreaId +0x90), Property +0x90, EntryDataMap +0xc0 (markers, `MiniMapEntryData` 0xd8 each) and PlayerLocation +0x170.
`CPP_MapManager` is the per-map world subsystem. Battle: `CPP_BtlManager` (TickableWorldSubsystem), `CPP_BtlUnit`,
`CPP_BtlUnitParameter`. These are not read yet.

## Assets

- Fonts: `FFRS-Switch.pak` → `FFRS/Content/UI/Font/FontFace/SE/*.ufont` (raw TTF/OTF bytes). The pak is Oodle-compressed.
- UI art: `FFRS/Content/UI/{Textures,Common,MainMenu,Battle}` in `FFRS-Switch.utoc/.ucas` (IoStore, Oodle). Names and strings are in
  `FFRS/Content/Datatable/Strings` and `L10N/en`.
- Oodle on arm64: ooz builds on arm64 with sse2neon and an `_rotl` fix. It is GPL-3.0, like this repository.

## Companion 1.0.0 (module 0100606026F12000.cpp, ffr_*.cpp)

Build it with `tools/ffr/build_android.sh <eden-duo checkout> <out> [version]`, or with the CMake target `dsmod-ffr`. Desktop checks: `tools/ffr/dev/assets_tool.cpp` (asset layer) and `render_tool.cpp` (renders every page with sample state from
your RomFS).

Found live, in addition to the reflection dump:

- `CPP_UnitParameter` (not reflected): unit id +0xf8, level +0x100, HP/MP/BP +0x10c, base max HP/MP/BP +0x120,
  Attack/Defence/Intelligence/Mind/Divine/Agility +0x12c. The field holds one per character under
  `CPP_MapManager.CPP_MapUnitParameterStorage`. Max HP/MP = base + the gear's params (eUnitParameterType 1/2).
  `CPP_BtlUnitParameter` derives from it (CPP_BtlUnit +0x838). The battle panel is not yet checked in a real battle.
- Equipment table: `CPP_AppAssetManager.m_DataTableMap` (+0xd0, key 44 ItemEquipmentData) → `UDataTable` RowMap +0x30
  (element 0x18). In each row: type +0x7d, detail +0x7e, params map +0x80 (element 0x10).
- Map: `CPP_MiniMapSubsystem.AreaManager` (+0x80) → CurrentAreaId +0x90, MapDA +0x38 → LandCaptureDA +0x30 →
  Map<mapId, MapLandCaptureData> (element 0x80) → AreaLandCaptureDataMap (+0x20, element 0x130). The value holds the
  texture soft path (an FName with an instance number: "Town_01Gra_22" + 6 = "_5"), and WorldLocation +0x50 / BoxSize +0x60 in
  land coordinates. Land x = world Y, land y = −world X. The 512² capture covers a square of side max(bw, bh) centred on
  WorldLocation. The player: `CPP_MuchaPlayerController` +0x350 pawn → RootComponent +0x1b8 → location +0x140, rotation +0x158.
- Map names: the `DT_MapData_*` DataTables, MAP_DATA.Name FText (+0x20) → TextData → +0x20 → +0x40 → FString at +8
  (English display string).

## Companion 1.0.4: battle panel

- Battle units: `CPP_BtlManager` +0x48 → `CPP_BtlUnitManager` +0x28 `m_UnitList` → `CPP_BtlUnit` +0x838 → `CPP_BtlUnitParameter`
  (HP/MP at the `CPP_UnitParameter` offsets). A unit is on the party's side when its id is in the save's unit list, or when it is a
  vision whose `m_pVisionOwner` (+0x370) is such a unit. The page shows enemies on the left and the party on the right, as the game does.
- Timing: a full read runs about 10 times a second. During a battle, HP and MP of the units already found are re-read about 30 times a
  second (`Reader::RefreshBattle`, one 0x1c-byte read per unit, checked against the unit id). The full object-array rescan runs every
  3 s out of battle and every 10 s in one, plus whenever a tracked object goes away.
- Still not checked in a real battle: whether +0x10c is the HP the game is showing during the fight, or a value it only writes at the end
  of an action.
- When a battle ends, the page goes back to the tab (and party member) that was open when it started.

## Companion 1.0.5: live map, world map, map icons, battle commands (built, not yet checked live)

- Map: the Map tab is the default. The picture is now the manifest's `map` widgets (`ffr_town_map` over a 4096 square,
  `ffr_world_map` over a 16384 square, y up). The module publishes the player every tick (`Reader::ReadPlayer`, a few reads), so the
  marker moves at 60 Hz without new pages. Pinch zooms; `view_zoom_step` +/- buttons and `view_reset` for the player. The page hash
  no longer includes the position.
- World map: `MapCapture/Wld/Wld_<area>` (1024², transparent islands) over `T_Wld_ocean`. Assumed: the field map's LandCapture
  entries hold one area per island, with the same WorldLocation / BoxSize meaning as a town's. Each island is a dynamic marker of one
  world size (`size_world` 1024); the module pads each picture to its share of that square (`module:ffr:isl:<fraction>:<texture>`).
- Map icons: `CPP_MiniMapSubsystem.EntryDataMap` (+0xc0, element 0xe8). In each value, `ObjectData.InteractTag` (+0x70, FName
  `Field.Object.Interact.Type.Shop.Weapon` …) picks the `ICON_MAP_L_*` texture, and `ObjectData.ReferenceActor` (+0x90, a weak pointer
  {index, serial}) gives the actor, whose root component holds the location. Unchecked live.
- Battle commands, from the reflection dump (unchecked live): `CPP_VM_BtlCmd` CommandStack +0x88 (BtlCmdStackData 0x68: state +0,
  ActionKey {u8 cat +4, id +8}, Index +0xc, EntryIndex +0x18), FocusCmdData +0x100 (state, EntryIndex, Value.ActionKey, TargetIdList
  +0x110), bDisplayCmd +0x199. A `CPP_BtlCmdListBase` has BaseStackData +0x30 and ActionKeyList +0x98. `CPP_UI_BtlCmdManager`
  ActionUnitIndex +0x58, ActionUnitUniqueIdList +0x60. Names: cat 1 `ST_SkillName`, cat 0 `ST_CommandSkillName`, cat 2 the item
  tables. Still unknown: the battle unit's unique id (targets name units by it); `CPP_BtlUnit` has no reflected field for it.
- Battle taps: the page is one `$tap` area; the module hit-tests it against the layout it drew (`ffr::BattleHit`), then presses
  buttons through the package's enforce rules (`ffr.drv.<button>`, one per sample, at least 140 ms apart). It presses A only when the
  game's cursor is on the tapped entry, or the tapped unit is targeted. Target search tries Down, Right, Up, Left in turn.

### Checked live (1.0.5 dev, 2026-11-22)

- World map: the field map (id 1000) has 15 island areas in its LandCapture entry, with the same WorldLocation / BoxSize meaning as a
  town's. Some islands have near-identical story variants at the same spot (7/17/18, 9/13, 6/16); one per spot is kept.
- Battle: `CPP_VM_BtlCmd` (match on ancestry, the game uses a subclass) works: bDisplayCmd follows the player's turn, FocusCmdData's
  EntryIndex is the cursor, CommandStack is the path (root: Attack 40N010, Skills = cat 1 id 1, Defend 400010, Items = cat 1 id 7,
  Escape 400020; skill ids 40Nxxx belong to unit 100N). FocusCmdData's state stays 0 (no target state there).
- Each submenu is a new `CPP_BtlCmdListBase` object (BtlCmdVM +0x28). The open one holds the focused entry (FocusCmdData.Value.ActionKey)
  at the cursor; `Reader::FindList` looks for it when the known list stops matching (at most every 100 ms).
- `CPP_BtlUnitManager` (BtlManager +0x48): m_pCmdSelectUnit +0x38 is whose command it is. Its m_TargetUnitList +0x48 is filled
  only once a target is confirmed and is kept into the next turn, so it is not the pointer.
- The target choice: the window is up and the CommandStack path ends in an action (a skill, Attack, Defend, an item; not a command
  list, cat 1 id < 1000). The pointer: each unit's `CPP_VM_BtlUnitParam` (BtlUnitParam +0x1a0 = the unit's CPP_BtlUnitParameter,
  TargetSelectState +0x1ba: 0 none, 1 Main, the pointed unit; 2 Sub, the others an area action reaches). Unchecked live.
- Minimap entries (MiniMapEntryData): BelongAreaId +0xb8 (the town's entries stay in the map indoors: filter on the current area),
  bIsMarkedQuestTarget +0xbc (the objective; drawn with `ICON_MAP_L_Check` on a gold badge). Treasure is not shown, as in the game.

### 1.0.5 follow-ups (from reflection; check with the dev log's `fin=` / `bp=` fields)
- Resonance wheel = the game's Finish command: `CPP_UI_BtlCmdManager.CmdType` (+0x30) == 2 while it is up (path empty).
  `CPP_VM_FinishCmd.EntryDataList` (+0x70): FinishCmdEntryData 0x20 = bEnable +0, UnitUniqueId +4, VisionId +8, SkillId +0xc,
  RelatedNavigation +0x10 (EUINavigation: 0 left, 1 right, 2 up, 3 down). Unit unique id: `CPP_VM_BtlUnitParam.UniqueID` +0x198.
  No focused-entry field found yet; a tap presses the direction, then A if the wheel is still up.
- Stagger: live unit parameter = UNIT_PARAMETER + 0xc8, so Breakpoint +0x114, MaxBreakPoint +0x128; staggered = max > 0, bp <= 0.
- `CommandSkillName200` = "Tronn's Skills" (a vision's command list); Limit Bursts = id 10 (only in the list when ready).
- Host: module pictures share a 64 MiB budget, evicted oldest-*loaded* first even while in use: keep the world map's pictures small.

### Checked live (1.0.5 dev, 2026-11-23)

- The target pointer (TargetSelectState Main / Sub), the resonance wheel (CmdType 2, EntryDataList and its directions), the stagger
  tag (Breakpoint / MaxBreakPoint), visions beside their owner (`m_pVisionOwner`), Limit Bursts (only when the game lists them) and
  the world-map heading all match the game's own screens.
- The equipped vision (`equipVisionId`): its face is `Icon/Vision_Face/ICON_UnitFace_summon<vision id>`, and its name is
  `ST_VisionName` `VisionName<id>`.
- Vision stat bonuses: visions are not in the equipment table. `SaveData_Vision` (GameSaveData +0x118, 0x58 each: ID, awakeningPoint,
  synchroPoints Map<unit id, int>) unlocks rows of the mastery tables (data tables 46 awakening, 47 synchro; VISION_MASTERY_DATA:
  vision ID +8, masteryPoint +0x10, detailData +0x18). A row whose masteryPoint is reached grants passive skills (type 2) and,
  in synchro, flat parameters (type 5: {eUnitParameterType, value}). A passive (table 26, bBattleOnly +0x60) changes a parameter
  through an effect (table 29) of type 3 with ParamList {value, eUnitParameterType, 2 = percent}. Checked: Lasswell with Tronn,
  base max MP 68 → 74 at level 6 and 74 → 81 at level 7 (+10%, rounded down), as the game shows.
- Guests: `SaveData_Party.guestUnitId` (+0x10; also `SaveData_UnitGuest` +0x80) is a temporary ally (Charlotte, 1016). She is not in
  the save's unit list, so the battle reader takes her side from this id.

## Companion 1.0.6: the map follows the player, side-quest objectives, edge arrows (built, not yet checked live)

- Follow: the module publishes the view rect (`ffr.map.vx0` ..) centred on the player every tick, in towns and dungeons too,
  never clamped to the picture (no `clamp_view`: the player stays in the centre), sized by its own zoom (towns 1 = as wide as
  the whole picture, the world 1 = 26000 land units across; 0.2-40). The host glides its base view after the rect, but holds it
  while its own view is moved (dragged or pinched: `view_custom`), and a map widget's zoom cannot be bound.
- +/- are two taps each: `ffr.map.<town|world>_follow` (the view at home: the module's `zoom` action, which also starts a new
  page so the host redraws at once; without it the new window showed only at the player's next step) and `_look` (the host's
  `view_zoom_step`). The module reads `get_f64("@view_zoom|@view_pan_x|@view_pan_y:<widget>")`, the host's `view_custom` test.
- Home (module action `home`): the module's zoom times the host's view zoom becomes the module's, then the module raises
  `ffr.map.<kind>_reset` (and `ffr.drv.pending`, the enforce gate) for one sample, and the enforce rule runs `view_reset` on that
  map. The host glides its view home while the base still holds, then the base glides onto the player: a short zoom-out /
  zoom-in. The user chose this over a Peanuts Native change (2026-11-23).
- Objectives, from reflection: `CPP_QuestSubsystem` (GameInstanceSubsystem) QuestDataMap +0x38 (Map<int, QuestData 0x98>,
  element 0xa8; QuestData ID +0x10, Category +0x14 EQuestCategory 1 Main / 2 Sub / 3 Mini, Progression +0x68 EQuestProgression
  1 Start / 2 Progress / 3 Complete), MainQuestId +0x98, PreferredQuestId +0x9c. A minimap entry's quests:
  `FieldObjectCoreData.RelatedQuestIdList` (ObjectData +0x20, TArray<int>). An entry is the story's objective (gold) when a
  Main quest in it is at Start or Progress (or it is marked and its quests are unknown), and a side quest's (blue) when a Sub or
  Mini quest in it is; both at once draws the gold badge with a small blue one (`goal:both`, arrow `b`).
- Checked live (2026-11-23, world map): the quest table reads right (10 quests; main 1001 and side 14 at Progress, PreferredQuestId
  14). Only one world entry had quest ids, a town landmark (map 2020) with both [1001, 14], marked. So a place the story and a
  tracked side quest share is one entry, which 1.0.6's first build drew gold only. Towns and dungeons are still to check.
- Edge arrows: for each objective out of view, a dynamic marker (`arrow` group) on the view's edge, inset 52 px (and clear of
  the zoom buttons on the right). The view is the host's last drawn one: `get_f64("@map_view_cx|cy|ppw:<area>")`. The
  picture is `module:ffr:arrow:<m|s>:<0-23>`, 15-degree steps clockwise from the right (markers cannot rotate).

## World map from a town, fast travel (research, 2026-11-23, live in Kol)
- Loaded everywhere: `/Game/Datatable/Map/DA_MapLandmark` (CPP_DA_MapLandmark.landmarkDataList +0x38, MAP_LANDMARK_DATA_LIST 0x50:
  landmarkDataList {Str Condition, int mapId} 0x18 each, Location +0x18 (world vector; land = (Y, -X)), airShipLandingPointId +0x30),
  `DA_LandmarkList` (same row type, 80 rows with story variants).
- `CPP_UI_WldMapSubsystem` (a WorldSubsystem, present in towns, WldMapId +0x40 = 1000): FldObjDataMap +0x48 Map<int,
  StaticFieldObjectData 0x50> (element 0x60): the landmarks the world map shows (known ones only; 7 in this save). Value: TypeData
  +0x08 (type 6, id), InteractTag +0x10 (Field.Object.Interact.Type.Landmark), RelatedMapId +0x18, Location +0x30, LandingPointId +0x48.
  Kol = map 2020 at land (15000, -12775).
- Visited: CPP_MuchaGameSaveData.Map (+0x1a8) .perMapSaveDataMap (+0x1c8): Map<int, SaveData_PerMap{bIsKnown, bIsVisited, bIsCleared}>
  (element 0x10).
- Rules (DA_WldMapParameter): bEnableFastTravelUnvisitedMap 0; FastTravelMapTypeEnableMap {Dungeon: 0}; default behaviour travel with
  dialog UI.Dialog.Box.Map.FastTravel; refused with ...CanNot.MapType / ...CanNot.Event (a progress event) / ...CanNot.LockMap.
  IsJumpMapEntryWhenClick 1. Menu view model: CPP_VM_WldMap (CursorPosition +0x2e0, FocusObjectId +0x39c, FastTravelBehavior +0x408).
- The menu, checked live by hand (2026-11-23): VM_WldMap_C (IsA CPP_VM_WldMap) and VM_WldMapLandmarkList_C exist only after the
  map first opens. Minus opens it (SceneTag stays UI.WldMap.Scene.Normal, focus jumps to the current place); ZL opens the list
  (SceneTag UI.WldMap.Scene.Location, DisplayCategory Town, cursor on the first entry); R / L change category (Town, Dungeon,
  Shrine) and reset the cursor to the first entry; the d-pad moves it and FocusObjectData.RelatedMapId follows; one A opens the
  dialog (CPP_DialogManager.DialogWidget +0x40 set). B closes the dialog, B the list, then the map; DisplayCategory goes back to
  None when the map closes. Presses sent with `adb shell input` do not reach the game: only the module's button actions do.
- Companion 1.0.7 (dev): world map places (group lm) with on_marker_tap, a card with Fast Travel driving the above (Travel in the
  module), and in towns a World > place scope switch with Back. MAP_DATA.MapType at +0x101 (0 town, 1 dungeon, 4 shrine),
  GroupId +0x10 (a dungeon's floors), sameMapId +0x18.
- Correction (live, 2026-11-23): FastTravelMapTypeEnableMap {Dungeon: 0} is about the map the player is in (CanNot.MapType), not
  the destination: the Earth Shrine (3001, a dungeon) is a valid destination once visited. Destinations need only bIsVisited.
  FldObjDataMap is empty after a restart until the game's map first opens; DA_LandmarkList filtered by bIsKnown gives the same
  places before that.
- Menus (live, 2026-11-23): CPP_VM_MainMenu.SceneDataStack (+0x80, GameplayTags) is empty in the field, [Root] with the main
  menu open, [Root, Party] / [Root, ActionEquip] in its screens. The system/save menu, NPC talk and shops do not show in it,
  nor in CPP_GameWidgetManagerSubsystem.InputMappingCount (always 0). The fast travel dialog: FastTravelBehavior
  (VM_WldMap +0x408 bCanTravel, +0x40c DialogTag UI.Dialog.Box.Map.FastTravel) and the DialogManager widget; it opens on No, so Up
  then A travels. Peanuts Native has no BRK guest-call bridge (pnrt_stubs.cpp), but it installs a package's format-2 load plan
  (peanuts_helper.cpp): see "Fast travel through the game's own code" below.
- Who has the controls (live, 2026-11-23): CPP_GameWidgetManagerSubsystem +0x30 (CPP_EU_GameLayoutSubsystem.PrimaryGameLayout) ->
  WBP_OverAllLayout_C (CPP_EU_WBP_GameLayout): Layers +0x3a0 Map<GameplayTag, CommonActivatableWidgetContainerBase*> (element
  0x18), CurrentInputTargetLayer +0x540; a stack's WidgetList +0x190. Field: no input layer, Game=[WBP_FieldLayout_C]. Main menu and
  world map: input UI.Layer.System (WBP_MainMenuLayout_C); system menu WBP_SystemMenuLayout_C (+ Front1 WBP_LoadLayout_C); a
  conversation: input UI.Layer.Event (WBP_TalkEventLayout_C, the field layout gone); a shop: System over the conversation. The fast
  travel runs only from the field (no input layer, Event and Save empty) and stops if Event or Save fills while it runs.

### Fast travel pacing (tried, reverted)
Pacing presses by the reader's state (90 ms floor, next press as soon as the list/focus changed) broke the sequence live: the game ignores presses that come too soon after the last one, even when its view model already shows the change. The fixed waits (230–650 ms per step) are what works.
- Dungeon rule in the companion: MapState.map_type = MAP_DATA.MapType of the player's map; type 1 (Dungeon) shows the card as
  "Leave the dungeon to fast travel" (state D, no button) and the travel action refuses. Shrines (4) and towns (0) are allowed.

### Fast travel through the game's own code (guest helper, 2026-11-24)

Static RE of this build's main (a pack's main.patched.bin is a flat image; pointers in it are already relocated to base 0). UE's
native-function tables pair each UFunction name with its exec thunk (`{"HandleTransitionDialogClose", exec}` at 0x72f5828).

- `CPP_VM_WldMap::HandleTransitionDialogClose` (0x4381f18, exec 0x4050a54): on a result with button 0 (Yes) it tail-calls the
  travel, 0x4381770(vm). That reads only vm+0x3b8 (FocusObjectData.RelatedMapId), vm+0x3e8 (LandingPointId) and one virtual call
  vm->vtable[0x188] (the map widget, to close it; null returns at once).
- The travel proper: 0x41e6070(mapId): CPP_MapManager (0x41c37b4: GEngine 0x8c29430 -> world -> subsystem) gets a transition
  request (0x41be44c), and a NewObject<CPP_MapTaskTransition> goes to CPP_TaskManager (0x42d128c). The rest of 0x4381770 is the
  map manager's units and the menu (UI layer 107); the landing point (0x4172be8) is used only while that menu is up.
- The game's rules: `UpdateFastTravelBehavior` 0x43806b0(vm) (vm+0x168 WldMapDA = DA_WldMapParameter, vm+0x3b8) writes
  FastTravelBehavior +0x408 {bCanTravel, DialogTag +0x40c}: FastTravelMapTypeEnableMap for the current map's type, event flag 13,
  the visited check (0x4296c00) unless bEnableFastTravelUnvisitedMap. DecideObject (0x4380a10) only opens the dialog that names.
- Hook: CPP_TaskManager's FTickableGameObject vtable (0x7283308, from its constructor 0x3feb970) slot 0 = Tick 0x42d630c
  (`ldr x8, [x0, #0x8]`), the only reference; IsTickable 1, no world: every frame on the game thread. Nothing of these is in
  main.changed.tsv.
- native/helper/ffr_guest_helper.S hands the game a view model of its own in the mailbox (+0x400; its vtable +0x900 with +0x188
  returning null): op 1 the verdict, op 2 the verdict and the travel. The module (ffr_helper_mailbox.h) uses it when the
  heartbeat moves, otherwise the button sequence. Plan: tools/ffr/build_helper_plan.py --nso <exefs main> --main-bin <image>.


### Items, equipment and quest tracking through the game's own code (guest helper, 2026-11-24)

Same method as fast travel (static RE of main, checked live). The helper's mailbox is 8 KiB now.

- Recovery items (the main menu's item and heal pages, CPP_VM_SkillActionBase): SetUseAction 0x438e9b0(vm, UISkillActionData
  {UserUnitId, TmpNum, ActionKey{category 2 item, id}}) fills UseItemData +0x88 (GetConsumablesDataById) and UseSkillData +0x118
  (the item's SkillId, 0x42bf718); it needs SkillActionDA +0x68 (DA_UI_SkillAction) and reads SkillTargetingDA +0x70, then three
  field notifications (vtable +0x2e8). The check 0x438ef60 (owned, usableScene MapOnly/Any) wants UserUnitParam +0x260
  (GetUnitParameter 0x43cb110). ApplyActionToUnit 0x438ed40(vm, &target, reserve): uses the item up (0x428ee90(id, -1) when
  isConsumedAfterUse), applies the skill to the target or the party (TargetType Group; reserve adds the reserve members), and
  sends CPP_ApplySkillActionDelVal through the event dispatcher (the menu's sound). ApplyHealSkill only snapshots HP/MP around it.
- Live, the consumables (eDataTableType 39) and skills (25): Potion 1 / Hi-Potion 2 / 14 / 22 HP Single, Ether 3 / 18 / 20 MP,
  23 and 31-36 Group, Phoenix Down 4 / 37 defaultTargetState Dead; bombs and status items usableScene BattleOnly.
- Equipment: CPP_BFL_Unit::SetEquipmentId 0x43ced00(unit, item, slot) is the whole equip: when every copy is worn it takes one
  off another member, writes the save (0x429ef30: the item's type picks weapon +0x24, armor +0x28, accessory list +0x30[slot],
  appended past its end), applies it to the live unit and recalculates. UnequipmentId 0x43ceec8, IsEquipableEquipment 0x43b47f8.
  GetEquipSlotNum's table (0x664a5b0): weapon 1, armor 1, accessory 2, vision 1.
- Quest tracking: CPP_QuestSubsystem::SetPreferredQuestId 0x4283aa4(sub, id): the game's CanPreferred check, then the id (the
  tracked one again: -1) into PreferredQuestId +0x9c and the save, and the PreferredChange delegates (minimap, notices).
- QuestData (QuestDataMap value, 0x98): Property {IconTag, bCanPreferred +0x8, Priolity +0xc}, ID +0x10, Category +0x14, Name
  +0x18 (a generated text: its FString inline at FTextData +0x18), Subtitle +0x28, DestinationTitle +0x38, Description +0x48,
  SummaryList +0x58, Progression +0x68, bIsPreferred +0x80.
  The texts can be missing for a while after the quest is in the map (a completed side quest read at load had an empty
  Name; seconds later it read 'Dream of an Idealist'): the reader reads a quest's texts again every 3 s while its name is empty.
  The cause: a side quest's Name is a string table text (FTextHistory_StringTableEntry) that the game resolves only when its
  journal shows it; until then TextData +0x20's reference has no display string. The reference holds the table (FName +0x20, the
  full path /Game/Datatable/Strings/Quest/ST_EventQuestTitle.ST_EventQuestTitle) and the key as an FTextKey index (+0x28). The
  loaded UStringTable (class StringTable, object name ST_EventQuestTitle): its FStringTable at +0x28, KeysToEntries at +0x20
  (TSet elements of 0x20: the key's FTextKey index +0, the entry +8), the entry's SourceString FString at +0x10 (Japanese). The
  key strings themselves are not in reachable memory, so the module maps the source text through the game's ja Game.locres
  to the key (EventQuestTitle_032 for quest 14) and en Game.locres to the English title.
- Peanuts Native installs the load plan at launch by stopping the game's threads (40 tries, ~2 s). With frame generation on,
  every try timed out ("40 times they did not all stop"); with it off, the plan installed at once. Turning frame generation on
  after launch keeps the helper working (the user, 2026-11-24).
- Companion 1.0.7 (dev): Items is a use page (an item, then a party card), Equipment a member > slot > piece manager with the
  game's equippable answer, Quests a journal (Main, Side) with Track; the battle page takes the whole screen and has no tab.

### The game's menu sounds

The game's UI sounds are gameplay tags played by `CPP_BFL_Sound::PlaySound2DByTag` (exec thunk main+0x3e5a5e4, the
implementation main+0x42c8f90, `sub sp, sp, #0x60`). It takes the FGameplayTag by value in x0 (its FName: comparison index,
number), finds `CPP_SoundSubsystem` itself from GEngine (main+0x42c8ff8) and returns when there is none. The tags are in the
name pool from startup: `Sound.UI.Cursor`, `Sound.UI.Cancel`, `Sound.UI.Error`, `Sound.UI.Common.Tab.Switch`,
`Sound.UI.Item.Equip` / `Unequip`, `Sound.UI.Status.Heal`, `Sound.UI.MainMenu.Map.Target.On` / `Off`, and more under
`Sound.UI.` (a live dump of the pool, 2026-11-24). Their indices depend on the session, so the module looks them up by name
(`Reader::FindNames`, one 128 KiB pool block per tick). The helper plays them on a channel of their own in the mailbox
(sound_seq 0x078, sound_ack 0x07C, tag 0x080) at the start of each tick, so a sound never waits on a request.

### Dialog log, turn order, scene backdrop (2026-11-26, checked live)

- The backlog: `CPP_BacklogSubsystem` (a GameInstanceSubsystem, no reflected fields) +0x30 `TArray<BacklogData>` (0x48 each:
  SpeakerName FText +0, Text FText +0x10, VoiceCueSheetName +0x30, VoiceCueIndex +0x40). It keeps the newest 30 lines (a window that
  slides) and still holds the last conversation's lines afterwards. The newest is the line in the talk window.
- The talk window: the `UI.Layer.Event` stack's widget that IsA `CPP_WBP_Talk` (WBP_TalkEventLayout_C) -> WindowManager +0x598
  -> m_pWindow +0x190 (`CPP_WBP_TalkWindow`, one per window type) -> m_pTextBlock +0x558 (text FText +0x188, the generated string)
  and m_pSpeaker +0x5a0 (`CPP_WBP_TalkSpeaker`) -> m_pTexture +0x348: the speaker's bust, `/Game/Chara/Face/<chara>_<emotion>_BU`
  (1024 square, the figure standing on the foot; e.g. 0010 Rain, 0020 Lasswell, 0030 Fina, 0220 Emma). The backlog has no
  portrait, so the module keeps the bust it saw for each line and speaker. WBP_TalkWindow_C's Text_Name (+0x688) holds the name.
- The timeline: `CPP_VM_BtlTimeLine` (BtlTimeLineDA +0x68, TimelineUnified +0x70, TurnVMMap +0xd8 Map<EUIBtlTurnTimeLine 0 Current /
  1 Next / 2 Additional, CPP_VM_BtlTurn*>). `CPP_VM_BtlTurn.EntryDataList` +0xa8 (BtlTimelineEntryData 0x14: UnitUniqueId +0,
  DuplicateCount +4, BelongTimeline +8, BelongPhaseType +9: 0 Normal, 1 Additional (an extra action), 2 Finish (the visions'
  resonance: UnitUniqueId -1)). Unique ids are `CPP_VM_BtlUnitParam.UniqueID` (+0x198), and 0 is a real one (the first party
  member). The Current list holds only those still to act: a unit leaves it as its turn begins, so the one acting is the first of
  the previous order that is gone. Visions have no timeline entries of their own (no unit view model either).
- Extra actions and the finish break that rule. A stagger's bonus phase appends an Additional entry for the unit that staggered;
  it stays at the head of the Current list while that unit acts and leaves only as the next one starts (the finish's entry, or
  the new turn's list). The finish's entry does the same until the new turn's list replaces it. `CPP_UI_BtlCmdManager.CmdType`
  (+0x30: 0 Normal, 1 Additional, 2 Finish) tells which phase is on, and `CPP_BtlUnitManager.m_pCmdSelectUnit` is not updated
  for either: it still names the last unit to act, an enemy or another ally. Checked live over three battles.
- The results: `CPP_VM_BattleResult` (in the MVVM collection, kept for the session) BtlResultData +0x68 (`BTL_RESULT_DATA` 0xc0:
  itemDataList +0 (ItemId, Num, acquireLimitOverNum), unitDataList +0x10 (`BTL_RESULT_UNIT_DATA` 0xd8: UnitId, before/after EXP,
  before/after level, ap +0x14, IsDead +0xce), enemyDataList +0x20, visionDataList +0x30 (`BTL_RESULT_VISION_DATA` 0x30: VisionId,
  before/after MR, before/after AP, ap +0x14, UnitId +0x18), base/bonus/total EXP +0x40, base/before/bonus/total money +0x4c,
  base/bonus/total AP +0x5c). It keeps the last battle's numbers after, so whether they show comes from the layout:
  `WBP_Btl_ResultLayout_C` (`CPP_WBP_Btl_ResultLayout`, a CommonActivatableWidget under WBP_OverAllLayout) bIsActive +0x3e0 is set
  while the results show and cleared when they close (IsFinishPage +0x5c8 is set just before). The battle manager's units are
  still there until then, so the battle is still on.
- Timeline faces: `UI/Textures/Icon/Unit_Face_Timeline/ICON_UnitFace_unit<id>` (party), `summon<vision id>`, `mons<%04d FaceIconId>`.
  FaceIconId is UNIT_PARAMETER +0x188, held by the live unit parameter at +0xc8 (+0x250); IsBoss +0x150 (+0x218).
- The backdrop: texture readback (runtime 24) of the newest RGBA8 render target of the game's output size (the module tries
  1920x1080, 1280x720 and other sizes until one answers), at most every 1.5 s while the dialog or battle page shows; cropped to
  the upper 80 %, scaled to 310x270, box-blurred and toned.
