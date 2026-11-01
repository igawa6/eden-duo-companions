// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem Engage (0100A6301214E000) build-pinned data, game Ver. 2.0.0 (update v327680,
// merged base+update exefs), build id 8C08B9719E085F91847B5E0F935D9488 (+ 16 zero bytes).
//
// Every value below is verified live on that build:
//   - TypeInfo slots and the scene singletons (Title / Hub / Gmap / Map)
//   - UnitPool force walk and the Unit person/job/Emblem/bond chains
//   - MapImage grid, MapImageTerrain.m_Result (the terrain id) and MapImage.m_Danger.m_AttackAll
//     (the game's own danger bitmap; it is not recomputed here)
// TypeInfo slots are main-relative RVAs from the IL2CPP dump's script.json ScriptMetadata
// (calibrated against the known MapSituation row, exact match).

#pragma once

#include <cstdint>

namespace fee_pins {
using u64 = std::uint64_t;

// The only supported build: Ver. 2.0.0, main NSO build id + zero padding (32 bytes total).
inline constexpr char BuildIdHex[] =
    "8C08B9719E085F91847B5E0F935D948800000000000000000000000000000000";

// ---- IL2CPP structural constants (this build's il2cpp.h / verified live) ----------------------

// Il2CppClass.static_fields offset.
inline constexpr u64 KlassStatics = 0xB8;
// SZArray: max_length (u32) at +0x18, element data from +0x20.
inline constexpr u64 ArrayLength = 0x18;
inline constexpr u64 ArrayData = 0x20;
// Managed string: char count (i32) at +0x10, UTF-16LE chars from +0x14.
inline constexpr u64 StringLength = 0x10;
inline constexpr u64 StringData = 0x14;

// ---- TypeInfo slot RVAs (script.json) ----------------------------------------------------------

inline constexpr u64 TypeInfoUnitPool = 0x615C150;         // App.UnitPool_TypeInfo
inline constexpr u64 TypeInfoMapSituation = 0x6153020;     // SingletonClass<MapSituation>
inline constexpr u64 TypeInfoMapSequence = 0x61534E8;      // SingletonProcInst<MapSequence>
inline constexpr u64 TypeInfoMapCursor = 0x6152FA0;        // SingletonClass<MapCursor>
// MapCursor.m_Pos (Vector3 world position, READ ONLY: writes drive the camera/SFX); cell =
// round((world - 2.5) / 5) per axis (Map.CELL_SIZE 5), x = +0x1C, z = +0x24 (LIVE-READERS).
inline constexpr u64 MapCursorPos = 0x1C;
inline constexpr u64 TypeInfoMapImage = 0x6152FD0;         // SingletonClass<MapImage>
inline constexpr u64 TypeInfoHubSequence = 0x6153490;      // SingletonProcInst<HubSequence>
inline constexpr u64 TypeInfoGmapSequence = 0x6153468;     // SingletonProcInst<GmapSequence>
inline constexpr u64 TypeInfoTitleLoop = 0x6153658;        // SingletonProcInst<TitleLoopSequence>
inline constexpr u64 TypeInfoMapMiniMap = 0x61534E0;       // SingletonProcInst<MapMiniMap>
inline constexpr u64 TypeInfoMapPanelDanger = 0x61532D0;   // SingletonMonoBehaviour<...>
inline constexpr u64 TypeInfoMapPanelDangerAll = 0x61532D8;
inline constexpr u64 TypeInfoMapDeploy = 0x6152FA8;        // SingletonClass<MapDeploy>
inline constexpr u64 TypeInfoMapBattleInfoWindow = 0x6152F88;
// App.Language (dump.cs class Language): statics +0x0 s_Lang (Language.Langs 0..11), +0x4 s_Voice.
inline constexpr u64 TypeInfoLanguage = 0x6158B00;
inline constexpr u64 LanguageLang = 0x0;

// ---- UnitPool (static fields; NOT a singleton) --------------------------------------------------
// statics+0x0: s_Units  (Unit[250], SZArray of Unit*, elements from +0x20, pool pre-allocated)
// statics+0x8: s_Forces (Force[8], SZArray of Force*; array index == Force.Type enum value)
inline constexpr u64 UnitPoolUnits = 0x0;
inline constexpr u64 UnitPoolForces = 0x8;
inline constexpr u64 UnitPoolUnitMax = 250;
// Force: m_Head +0x10, m_Tail +0x18 (Unit*; both 0 = empty roster). Walk m_Head via Unit.m_Next.
inline constexpr u64 ForceHead = 0x10;
inline constexpr u64 ForceTail = 0x18;

// ---- Unit (0x240-byte objects in the pool) -------------------------------------------------------
inline constexpr u64 UnitPrev = 0x18;         // Unit*
inline constexpr u64 UnitNext = 0x20;         // Unit*
inline constexpr u64 UnitPerson = 0x40;       // PersonData*
inline constexpr u64 UnitJob = 0x48;          // JobData*
inline constexpr u64 UnitForce = 0x50;        // Force* (back-reference == the Force's address)
inline constexpr u64 UnitLevel = 0x89;        // u8
inline constexpr u64 UnitHp = 0x8B;           // u8 -- display/current HP; max HP is a calculated
                                              // getter (STATIC-READERS 2026-10-28), still open
inline constexpr u64 UnitEngageCount = 0x91;  // u8
inline constexpr u64 UnitEngageTurn = 0x92;   // u8
inline constexpr u64 UnitEngageCountView = 0x93; // u8; 255 = Unit.InvalidEngageCount (suppressed)
inline constexpr u64 UnitX = 0xA0;            // s8 grid x (cell)
inline constexpr u64 UnitZ = 0xA1;            // s8 grid z (cell)
inline constexpr u64 UnitItemList = 0xB8;     // UnitItemList* -> +0x10 UnitItem[8] (SZArray)
inline constexpr u64 UnitItemSelected = 0xC0; // unreliable cold (lazily populated), do not poll
inline constexpr u64 UnitGodUnit = 0xD0;      // GodUnit* (0 = no Emblem equipped)

// PersonData: +0x20 Pid (string "PID_..."), +0x28 Name key (string "MPID_..."),
// +0x38 Fid (face sprite name in the facethumb atlas, e.g. "Lueur").
inline constexpr u64 PersonPid = 0x20;
inline constexpr u64 PersonName = 0x28;
inline constexpr u64 PersonFid = 0x38;
inline constexpr u64 PersonIcon = 0x60;
inline constexpr u64 PersonGender = 0x6C;  // Gender enum (i32): 1 male, 2 female
// JobData: +0x28 Name key (string "MJID_..."), +0x68 MoveType (i32)
inline constexpr u64 JobName = 0x28;
inline constexpr u64 JobMoveType = 0x68;
// JobData +0x40 UnitIconID_M, +0x48 UnitIconID_F, +0x50 UnitIconWeaponID (map dot sprite name parts).
inline constexpr u64 JobIconM = 0x40, JobIconF = 0x48, JobIconWeapon = 0x50;
// GodUnit: +0x20 m_God (GodData*), +0x38 m_Bonds (GodBondHolder*).
// GodData: +0x28 Mid (name key "MGID_..."), +0x50 FaceIconName (Emblem face sprite name).
inline constexpr u64 GodUnitGod = 0x20;
inline constexpr u64 GodDataName = 0x28;
inline constexpr u64 GodDataFace = 0x50;
inline constexpr u64 GodDataIcon = 0x70; // UnitIconID ("530Marth"), Emblem dot sprite prefix
inline constexpr u64 GodUnitBonds = 0x38;
// GodBondHolder +0x30: Dictionary<string, GodBond>. _entries at dict+0x18; entries data from the
// array's own +0x20; Entry stride 0x18 {hashCode i32 +0x0 (negative = free), next i32 +0x4,
// key string* +0x8, value GodBond* +0x10}. GodBond: m_Level +0x28 (u8), m_Exp +0x2A (u16).
inline constexpr u64 BondHolderDict = 0x30;
inline constexpr u64 DictEntries = 0x18;
inline constexpr u64 DictEntrySize = 0x18;
inline constexpr u64 DictEntryKey = 0x8;
inline constexpr u64 DictEntryValue = 0x10;
inline constexpr u64 GodBondLevel = 0x28;
inline constexpr u64 GodBondExp = 0x2A;

// ---- MapSituation (SingletonClass; s_Instance at statics+0x0) -----------------------------------
inline constexpr u64 MapSituationForce = 0x38; // i32: 0 player, 1 enemy, 2 ally
inline constexpr u64 MapSituationTurn = 0x48;  // i32
inline constexpr u64 MapSituationSubPhase = 0x4C; // i32

// ---- MapImage (SingletonClass; s_Instance at statics+0x0; instance 0 outside a map battle) -----
inline constexpr u64 MapImageUnit = 0x28;    // MapImageCoreByte* (unit occupancy)
inline constexpr u64 MapImageTerrain = 0x30; // MapImageTerrain*
inline constexpr u64 MapImageDanger = 0x40;  // MapImageDanger*
inline constexpr u64 MapImageW = 0x68;       // i32 (1..32)
inline constexpr u64 MapImageH = 0x6C;       // i32
inline constexpr u64 MapImagePlayAreaX = 0x70; // i32 x4: X, Z, W, H
// MapImageTerrain: m_Result at +0x20 (resolved terrain -- the current getter's source;
// m_Base +0x18 was the 2026-10-27 probe's read, corrected by STATIC-READERS).
inline constexpr u64 MapImageTerrainResult = 0x20;
// MapImageCoreByte: m_Images at +0x10 (byte[1024]; index = z*32 + x).
inline constexpr u64 MapImageCoreByteImages = 0x10;
// MapImageDanger: m_AttackAll +0x20, m_RodAll +0x28, m_GunnerAll +0x30 (MapImageCoreBit*); the
// selected unit's own range is the inherited m_Images at +0x10 (not read yet).
inline constexpr u64 MapImageDangerAttackAll = 0x20;
// MapImageCoreBit: m_Images at +0x10 (u32[32]; bit (z*32+x) = (words[idx/32] >> (idx%32)) & 1).
inline constexpr u64 MapImageCoreBitImages = 0x10;

// ---- Unit details (2026-10-30 reader pass; offsets from dump.cs + the original NSO getters) ----
// Displayed stat i (CapabilityDefinition.Type: 0 Hp 1 Str 2 Tech/Dex 3 Quick/Spd 4 Luck 5 Def
// 6 Magic 7 Mdef/Res 8 Phys/Bld 9 Sight 10 Move) per Unit.Get<Stat>Impl(calcEnhance=true)
// (GetStrImpl 0x1A5A7F0, GetMaxHpImpl 0x1A5A660, GetPhysImpl 0x1A5B2A0, GetMovePowerImpl 0x1A5B690):
//   limit = clamp(JobData.Limit[i] + PersonData.Limit[i] (sbyte), 0, 255)
//   value = clamp(Unit.m_BaseCapability[i] (sbyte) + JobData.Base[i], 0, limit)
//   shown = clamp(value + Unit.m_EnhanceCalculator.m_Values[i] (int), 0, 255)  (HP: min 1)
// Capability objects keep their T[] at +0x10 (array data from +0x20).
inline constexpr u64 CapabilityData = 0x10;
inline constexpr u64 UnitBaseCapability = 0x58;   // UnitBaseCapability (sbyte[])
inline constexpr u64 UnitEnhanceCalc = 0x150;     // UnitEnhanceCalculator*
inline constexpr u64 EnhanceCalcValues = 0x10;    // UnitEnhanceValues* (m_Values)
inline constexpr u64 EnhanceValuesData = 0x10;    // int[]
inline constexpr u64 JobBase = 0xC8;              // Capability (byte[])
inline constexpr u64 JobLimit = 0xD0;             // Capability (byte[])
inline constexpr u64 PersonLimit = 0xD8;          // CapabilitySbyte
inline constexpr int StatCount = 11;
// Unit.m_CalcInfo (+0x1E8, Unit.CalcInfo): the game's own combat-stat cache that every
// Unit.GetSimple* getter refreshes (Update 0x1B42D50, once per GameTime frame; Count = frame).
inline constexpr u64 UnitCalcInfo = 0x1E8;
inline constexpr u64 CalcCount = 0x10, CalcAttack = 0x14, CalcHit = 0x18, CalcAvoid = 0x1C,
                     CalcCritical = 0x20, CalcSecure = 0x24, CalcPhysAttack = 0x2C,
                     CalcMagicAttack = 0x30;
// UnitItemList +0x10 UnitItem[8]. UnitItem: m_Item +0x18 (ItemData*), m_Endurance +0x20 (u8),
// m_RefineLevel +0x21 (u8), m_Flags +0x24 (i32, Equipped = 1).
inline constexpr u64 ItemListItems = 0x10;
inline constexpr u64 UnitItemData = 0x18, UnitItemEndurance = 0x20, UnitItemRefine = 0x21,
                     UnitItemFlags = 0x24;
inline constexpr int ItemSlots = 8;
// ItemData: Iid +0x20, Name +0x28 (MIID_), Kind +0x48 (i32), Icon +0x58 (string),
// Endurance +0x60 (u8; 0 = unbreakable weapon).
inline constexpr u64 ItemRangeI = 0x63, ItemRangeO = 0x64; // u8 min / max range
inline constexpr u64 ItemIid = 0x20, ItemName = 0x28, ItemKind = 0x48, ItemIcon = 0x58,
                     ItemMaxUses = 0x60;
// SkillArray: m_List +0x18 (List<Entity>; Entity = packed u32, Index = v & 0xFFF,
// Category = v >> 28). List<T>: _items +0x10, _size +0x18.
inline constexpr u64 SkillArrayList = 0x18;
inline constexpr u64 ListItems = 0x10, ListSize = 0x18;
inline constexpr u64 UnitEquipSkill = 0xF8, UnitPrivateSkill = 0x100, UnitMaskSkill = 0xF0,
                     UnitReceiveSkill = 0x108;
inline constexpr u64 GodUnitSyncroSkills = 0x68; // SkillArray
// SkillData (StructData<SkillData>.s_list, List<SkillData>; StructBase.Index +0x10):
// Sid +0x28, Name +0x30 (MSID_), IconLabel +0x68.
inline constexpr u64 TypeInfoSkillData = 0x6153CF0;   // App.StructData<SkillData>
inline constexpr u64 TypeInfoTerrainData = 0x6153D18; // App.StructData<TerrainData>
inline constexpr u64 StructIndex = 0x10;
inline constexpr u64 SkillSid = 0x28, SkillName = 0x30, SkillIcon = 0x68;
inline constexpr u64 SkillFlag = 0x1B8;  // SkillData.Flags (u64); Invisible = 1
inline constexpr u64 SkillInvisible = 1;
// Unit Details skill sources: PersonData.CommonSkills +0x178 (personal), JobData.MaskSkill
// +0x128 (class), Unit.m_EquipSkill (inherited, 2 slots), the Emblem bond's SynchroSkills, then
// the "Type Bonus" rows: bond EngageSkills + GodData.EngageAttack.
inline constexpr u64 PersonCommonSkills = 0x178, JobMaskSkill = 0x128;
inline constexpr u64 UnitLearnedJobSkill = 0x120; // SkillData* (the class skill the unit learned)
// GodBond +0x40 LevelData: SynchroSkills +0x10, EngagedSkills +0x18, EngageSkills +0x20.
inline constexpr u64 GodBondLevelData = 0x40;
inline constexpr u64 LevelSynchro = 0x10, LevelEngaged = 0x18, LevelEngage = 0x20;
// GodData +0xA8 EngageAttack (SID string).
inline constexpr u64 GodDataEngageAttack = 0xA8;
// TerrainData (s_list indexed by the MapImageTerrain.m_Result byte, MapImageTerrain.GetData
// 0x2064ED0): Tid +0x20, Name +0x28 (MTID_), Defense +0x5C, Avoid +0x5D, Heal +0x62 (sbyte).
inline constexpr u64 TerrainTid = 0x20, TerrainName = 0x28, TerrainDefense = 0x5C,
                     TerrainAvoid = 0x5D, TerrainHeal = 0x62;

// UnitInfoParamManager (SingletonClass): the values the native unit window / Unit Details show for
// the unit under the native cursor. m_CalcUnit +0x20 (a calc copy: same PersonData and cell as the
// real unit), m_param +0x70 (UnitParamDetail[17] indexed by ValueType: 0 MaxHp 1 Rating 2 Attack
// 3 Hit 4 Critical 5 Avoid 6 Secure 7 Continuous 8 Str 9 Magic 10 Tech 11 Quick 12 Def 13 Mdef
// 14 Luck 15 Phys 16 Move). UnitParamDetail.m_value +0x10 (int[7] by ValueDetail Base, God,
// Terrain, Support, Effect, Hub, Weight); the number on screen is the sum of the seven.
inline constexpr u64 TypeInfoUnitInfoParamManager = 0x61530E0;
inline constexpr u64 ParamCalcUnit = 0x20, ParamDetails = 0x70, ParamDetailValues = 0x10;
inline constexpr int ParamTypes = 17, ParamDetailCount = 7;
// JobData.Style +0x90 (BattleStyle.Types). LevelData +0x28 StyleItems -> +0x10 List<ItemData>[]
// indexed by style: the Emblem's Engage weapons for this class style.
inline constexpr u64 JobStyle = 0x90;
inline constexpr u64 LevelStyleItems = 0x28, StyleItemsLists = 0x10;

// ---- Fields round 2 (2026-10-30; original-NSO getters, verified live -- LIVE-READERS "Live
// verification 4") ----------------------------------------------------------------------------
// Unit.m_Status (+0x10, Unit.StatusField) -> BitField64 value u64 at +0x10. "Units Ready" =
// player-force units with (status & (Fixed | Vision)) == 0 (SortieTopMenuContent.UpdateInfo
// 0x24EC450 counts exactly this over UnitPool.GetForce(Player)).
inline constexpr u64 UnitStatus = 0x10, StatusValue = 0x10;
inline constexpr u64 UnitDoneMask = 0x08000001;
// MapSituation: m_WinRuleMID +0x60, m_WinRuleMIDArg +0x68, m_LoseRuleMID +0x70, Arg +0x78
// (GetWinRuleString 0x1F4D8A0 / GetLoseRuleString 0x1F4DB30 -> Mess.Get(MID) with arg 0).
inline constexpr u64 MapSituationWinMid = 0x60, MapSituationWinArg = 0x68,
                     MapSituationLoseMid = 0x70, MapSituationLoseArg = 0x78;
// SingletonProcInst<SortieSequence>: instance non-null while Battle Prep runs.
inline constexpr u64 TypeInfoSortieSequence = 0x6153600;
// SingletonClass<GodPool> (SingletonPool<GodPool, GodUnit>): every created GodUnit (the player's
// obtained Emblems, plus enemy copies in some chapters). m_List +0x10 (List<GodUnit>).
inline constexpr u64 TypeInfoGodPool = 0x6152F70;
inline constexpr u64 GodPoolList = 0x28; // live: +0x28 List<GodUnit> (8 on Ch.10)
inline constexpr u64 GodUnitParent = 0x28;     // Unit* wearing it (0 = in the box)
inline constexpr u64 GodDataForce = 0x94;      // Force.Type (0 = player Emblem)
inline constexpr u64 GodDataSort = 0xA0;       // i16 Ring Reference order
inline constexpr u64 GodDataSynchroEnhance = 0xF8; // CapabilitySbyte (all zero live; unused)
// SkillData.EnhanceValue +0x210 (CapabilitySbyte): the stat bonus a skill grants; the bond's
// Synchro list carries the Emblem's hidden stat skills ("Strength +3" ...).
inline constexpr u64 SkillEnhance = 0x210;
// Unit.m_Person etc as above; JobData.Style +0x90 -> BattleStyle.Types; display text
// "MBSID_<name>" (battlestyle message table).
// ---- Live verification 5 (2026-10-30) -------------------------------------------------------
// Unit.GetSimpleHit (0x1A5DBD0): Unit.CalcInfo.Update(m_CalcInfo, this) -> the game's own
// BattleUtil.CalcScope pass for this unit alone; fills the CalcInfo fields above. Read-only for
// the battle state (the cache is the unit's own); called through the HUD call stub.
inline constexpr u64 UnitGetSimpleHit = 0x1A5DBD0;
// SingletonClass<GameUserData>: m_Chapter +0x38 (ChapterData*), ChapterData.Name +0x28
// ("MCID_M010"); the title line is Mess(Name + "_PREFIX") + Mess(Name)
// (GameSaveDataHeader.GetChapterPrefix 0x2284770 / ChapterData.GetName 0x2AF9A40).
inline constexpr u64 TypeInfoGameUserData = 0x6152F38;
// TerrainData.Prohibition +0x40 (TerrainData.Prohibitions: 0 None, 1 All, 2 Ground, 3 Near).
// Live Ch.26 table read: Wall/Pillar/NoEntry/Nothing/Chest/Door = 1, Sky/Chair = 2, floors 0.
// (TerrainData.MoveCost +0x64 reads 0 on every row -- not a passability signal.)
inline constexpr u64 TerrainProhibition = 0x40;
inline constexpr int ProhibitAll = 1, ProhibitGround = 2;
// SortieUtil.GetSortieUnitMax() (static, 0x24F2D80): the deploy limit Unit Selection shows as
// "14/14". (MapDispos.GetSortieLimit 0x29C9480 is NOT it: it counts every army unit -- 41 live.)
inline constexpr u64 SortieUtilGetSortieUnitMax = 0x24F2D80;
inline constexpr u64 GameUserDataChapter = 0x38, ChapterName = 0x28;
inline constexpr int MapMaxSide = 32; // MapImage.MaxWidth/MaxHeight
inline constexpr int MapCells = 1024; // MapImage.MaxSize
// ---- Live verification 6 (2026-10-30) -------------------------------------------------------
// Load plan `forecast` helper (tools/fire-emblem-engage/gen_load_plan.py, appended after every
// earlier block): (offense Unit, defense Unit, out) -> the native forecast panel's numbers for
// that pair from the game's own BattleInfo.CalcSimple + BattleCalculator.CalcSimulation.
inline constexpr u64 ForecastHelper = 0x3F47204;
// GameUserData: m_Gold +0x58 (Money), m_PieceOfBond +0x6C (Bond Fragments).
inline constexpr u64 GameUserDataGold = 0x58, GameUserDataPieceOfBond = 0x6C;
// SingletonClass<RelayUserGlobalData>; RelayUserGlobalData.GetTicketCount(this) 0x200DA70 (the
// count the Somniel menu header shows: per-save ticket dict + uncommitted delta) -> call stub.
inline constexpr u64 TypeInfoRelayUserGlobalData = 0x61530A0;
inline constexpr u64 RelayGetTicketCount = 0x200DA70;
// SingletonClass<GmapSpotManager>: m_SpotArray +0x20 (GmapSpot[]); GmapSpot m_Chapters +0x18
// (List<ChapterData>), m_MobUnit +0x40 (GmapMobUnit*: a skirmish placed on that spot).
inline constexpr u64 TypeInfoGmapSpotManager = 0x6152F60;
// HubUtil.GetTimezoneType() (static) 0x2A5A030: Somniel time of day, HubUtil.TimezoneType
// 0 Morning, 1 Day ("Afternoon", MID_Hub_Time_Noon), 2 Evening, 3 Night.
inline constexpr u64 HubGetTimezoneType = 0x2A5A030;
// GmapSpotManager.GetNewestSpot(this) 0x2B3F640: the spot "To Next Chapter" points at.
inline constexpr u64 GmapGetNewestSpot = 0x2B3F640;
inline constexpr u64 GmapSpotArray = 0x20, GmapSpotChapters = 0x18, GmapSpotMobUnit = 0x40;
// ---- Live verification 7 (2026-10-31) -------------------------------------------------------
// Bond Effects ladder: GodData.GrowTable +0xD0 (Ggid string) keys GodGrowthData.s_LevelLists
// (App.GodGrowthData_TypeInfo statics +0x0, Dictionary<string, List<GodGrowthData.LevelData>>),
// one cumulative LevelData per bond level (GodBond.m_LevelData points at the current one).
inline constexpr u64 TypeInfoGodGrowthData = 0x6157B70;
inline constexpr u64 GodDataGrowTable = 0xD0;
inline constexpr u64 GrowthLevelLists = 0x0;

// ---- Iteration 2 (2026-10-31), static from the IL2CPP dump; live checks in LIVE-READERS ----
// Attack phase: SingletonProcInst<MapSequenceTargetSelect>.s_Instance is non-null while the
// native attack-target selection / combat forecast is open; MapSequenceTargetSelect
// m_TargetData +0x78 (MapTarget.Data: m_Unit +0x18 = the targeted unit), m_BattleInfo +0x88
// (BattleInfo: m_Sides +0x18 -> BattleSide.ContainerArray m_Array +0x10 -> BattleInfoSide[]
// Offense 0 / Defense 1; BattleInfoSide.m_Unit +0x20). SingletonProcInst<MapSequenceBattle>
// .s_Instance is non-null while a map combat runs (m_Defender +0xC8).
inline constexpr u64 TypeInfoTargetSelect = 0x6153548;
inline constexpr u64 TypeInfoMapSequenceBattle = 0x6153500;
inline constexpr u64 TargetSelectTargetData = 0x78, TargetSelectBattleInfo = 0x88;
inline constexpr u64 MapTargetDataUnit = 0x18;
inline constexpr u64 BattleInfoSides = 0x18, SideContainerArray = 0x10, BattleInfoSideUnit = 0x20;
// BattleInfoSide.m_UnitItem +0x28: the weapon the native forecast uses (its L/R pick).
inline constexpr u64 BattleInfoSideUnitItem = 0x28;
inline constexpr u64 MapSequenceBattleDefender = 0xC8;
// Unit.m_Edit +0x30 (UnitEdit, the avatar's player-given name): UnitEdit.m_Name +0x10 (string;
// null / empty = not named). The game's text tag g6 t3 ("[tag g6t3:]", MPID_Lueur_M000 in the
// prologue) is that name.
inline constexpr u64 UnitEdit = 0x30, UnitEditName = 0x10;
// Live combat HP (owner 2026-11-01: the bottom HP must drop per hit like the top screen).
// Unit.m_Hp is ChangeValue {m_Value +0x8B, m_Display +0x8C}. Combat.CombatWorld.s_this (static
// +0x0, set by CombatWorld.get_Instance 0x29255D0) -> <Record> +0x38 (CombatRecord)
// -> <GameStatus> +0x28 (CharacterGameStatus[]): <Unit> +0x28, <MaxHP> +0x50, <HP> +0x54.
inline constexpr u64 UnitHpDisplay = 0x8C;
inline constexpr u64 TypeInfoCombatWorld = 0x6156588; // script.json Combat.CombatWorld_TypeInfo
inline constexpr u64 CombatWorldRecord = 0x38, CombatRecordGameStatus = 0x28;
inline constexpr u64 GameStatusUnit = 0x28, GameStatusHp = 0x54;
// The record's statuses are the pre-combat import (live 2026-11-01: constant through the blows,
// GameStatusSide +0x20 = 0 offense / 1 defense). The HP the combat HUD is drawing: CombatWorld.chrs
// +0x20 (Character[]) -> Character <Side> +0x18, <ChainId> +0x1C (-1 for the two fighters),
// _HUD +0x168 (CharacterHUD, cached on first use; null until the gauges come up) -> CombatGaugeController +0x20 -> m_Hp +0x48 (TextMeshProUGUI) -> m_text +0xD0
// (CharacterHUD.TryUpdateValues writes it per blow). Character._gs +0x50 is null live.
inline constexpr u64 GameStatusSide = 0x20;
inline constexpr u64 CombatWorldChars = 0x20, CharacterSide = 0x18, CharacterChainId = 0x1C,
                     CharacterHud = 0x168, HudGauge = 0x20, GaugeHpText = 0x48, TmpText = 0xD0;
// UnitEdit.m_Gender +0x20 (Gender i32): the avatar's chosen gender. The hero's PersonData stays
// male (PID_リュール Gender 1); the game's own IsPlayerFemale reads m_Gender when it is non-zero
// (UnitEdit.IsEnable) and PersonData.Gender otherwise.
inline constexpr u64 UnitEditGender = 0x20;

} // namespace fee_pins
