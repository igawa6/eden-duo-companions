# FINAL FANTASY RESONANCE companion: research notes

Title `0100606026F12000`, update 1.0.1, build `84E182F72EE1DA4F477804BD05B54C15…`. Unreal Engine 5.6. The game's C++ module is
`/Script/mucha`.

## Development probe (not shipped)

`probe/ffr_probe.cpp` is a native module that serves guest memory reads on `127.0.0.1:47123` inside the game process. To use it:

1. Build it (NDK clang, `-static-libstdc++`, `llvm-strip --strip-all`), then package it with `tools/build_dualscreen_package.py`.
2. Install it on Peanuts Native by pushing the unzipped package to `<folder of the game's .nup>/0100606026F12000-work/peanuts/<Name>/`.
   adb cannot create folders in the app's own `packs/peanuts/` on Android 13.
3. Start the game with the screens already on. The runtime starts on the first frame that reaches the screen. A game started while
   the display was waking discarded every frame, and the companion stayed "pending".
4. Run `adb forward tcp:47123 tcp:47123`, then `probe_client.py`. Reads keep working with the screen asleep (the game is suspended),
   so the long dumps run with the panel off.

The scripts:

- `probe/ue_discover.py <run dir>` finds the name pool and the object array and dumps `objects.tsv` (about 67k objects in 1 minute).
- `probe/ue_sdk.py <run dir> <out.txt>` dumps the reflection of `/Script/mucha` (classes, structs, enums) in about 13 s.

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
- Oodle on arm64: ooz (`dm/third_party/ooz` in peanuts-native) builds with sse2neon and an `_rotl` fix. It is GPL-3.0, like this repository.

## Companion 1.0.0 (module 0100606026F12000.cpp, ffr_*.cpp)

Build it with `tools/ffr/build_android.sh <eden-duo checkout> <out> <version> [--dev]`. `--dev` adds the memory server
(port 47123, with TCP_NODELAY). Never publish a `--dev` build. The release build, and the CMake target `dsmod-ffr`, leave it out. Desktop checks: `tools/ffr/dev/assets_tool.cpp` (asset layer), `render_tool.cpp` (renders every page with sample state from
your RomFS) and `live_tool.cpp` (runs `ffr::Reader` against the device's dev server).

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
