// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Offsets for Cult of the Lamb 1.5.27 and 1.5.28 (same build 316A4D4881A9F1E077A07262E917917C).
// Source: Il2CppDumper output (dump.cs / script.json), see
// tools/cotl/README.md. Addresses relative to main.
#pragma once

#include <cstdint>

namespace cotl::off {

inline constexpr uint8_t kBuildId[16] = {0x31, 0x6A, 0x4D, 0x48, 0x81, 0xA9, 0xF1, 0xE0,
                                         0x77, 0xA0, 0x72, 0x62, 0xE9, 0x17, 0x91, 0x7C};
inline constexpr const char* kBuildIdHex = "316A4D4881A9F1E077A07262E917917C"
                                           "00000000000000000000000000000000";
inline constexpr uint64_t kTitleId = 0x01002E7016C46000ull;

// TypeInfo-Slots (script.json, ScriptMetadata)
inline constexpr uint64_t kDataManagerTypeInfo = 0x713AC50;
inline constexpr uint64_t kPlayerFarmingTypeInfo = 0x713FE88;
inline constexpr uint64_t kBiomeGeneratorTypeInfo = 0x7139CB8; // MMBiomeGeneration.BiomeGenerator
inline constexpr uint64_t kSaveAndLoadTypeInfo = 0x7140C48;
inline constexpr uint64_t kSaveAndLoadLoaded = 0x4; // static: bool Loaded
inline constexpr uint64_t kMainMenuTypeInfo = 0x71428F8; // UIMainMenuController
inline constexpr uint64_t kMainMenuInstance = 0x0;       // static: only set at the title screen
inline constexpr uint64_t kEquipmentManagerTypeInfo = 0x713B9A0;
inline constexpr uint64_t kDifficultyManagerTypeInfo = 0x713B278;
inline constexpr uint64_t kPlayerFleeceManagerTypeInfo = 0x713FE90;

// EquipmentManager (static), WeaponData, CurseData, RelicData
namespace gear {
inline constexpr uint64_t kWeapons = 0x8;           // WeaponData[]
inline constexpr uint64_t kCurses = 0x10;           // CurseData[]
inline constexpr uint64_t kRelics = 0x18;           // RelicData[]
inline constexpr uint64_t kType = 0x18;             // EquipmentType bzw. RelicType
inline constexpr uint64_t kWeaponSpeed = 0x4C;
inline constexpr uint64_t kWeaponCombos = 0x50;     // List<WeaponCombos>
inline constexpr uint64_t kComboDamage = 0x14;
inline constexpr uint64_t kCurseDamage = 0x6C;
inline constexpr uint64_t kRelicCharge = 0x28;      // DamageRequiredToCharge
inline constexpr uint64_t kPlayerRelic = 0x398;     // PlayerFarming._playerRelic
inline constexpr uint64_t kRelicDouble = 0x101;     // PlayerRelic.DoubleDamage
inline constexpr uint64_t kRelicMultiplier = 0x104; // PlayerRelic.DamageMultiplier
inline constexpr uint64_t kRelicScale = 0x1D0;      // PlayerRelic.PlayerScaleModifier
inline constexpr uint64_t kCurrentDifficulty = 0x8; // DifficultyManager.currentDifficultyData
inline constexpr uint64_t kDifficultyDamage = 0x2C; // DifficultyData: player damage
inline constexpr uint64_t kFleeceDamage = 0x38;     // PlayerFleeceManager.damageMultiplier
inline constexpr uint64_t kDmDamageBonus = 0x1024;  // DataManager: added in both damage formulas
inline constexpr uint64_t kDmFleece = 0x11C;        // DataManager.PlayerFleece
inline constexpr uint64_t kDmPhase = 0x614;         // DataManager.CurrentPhase (Night = 4)
} // namespace gear

inline constexpr uint64_t kFollowerBrainTypeInfo = 0x713C068;
// WorshipperData (ScriptableObject): static _Instance @0; colours per kind like SetFollowerCostume
// (0x29B7FF0) via GetColourData (0x214A5D0) and SkinAndData.Contains (0x214A450)
inline constexpr uint64_t kWorshipperDataTypeInfo = 0x71434D8;
namespace wd {
inline constexpr uint64_t kInstance = 0x0;        // static
inline constexpr uint64_t kCharacters = 0x30;     // List<SkinAndData>
inline constexpr uint64_t kSkin = 0x20;           // SkinAndData: List<CharacterSkin>
inline constexpr uint64_t kSlotAndColours = 0x28; // SkinAndData: List<SlotsAndColours>
inline constexpr uint64_t kSkinName = 0x10;       // CharacterSkin.Skin (string)
inline constexpr uint64_t kSetList = 0x10;        // SlotsAndColours.SlotAndColours: List<SlotAndColor>
inline constexpr uint64_t kSlot = 0x10;           // SlotAndColor.Slot (string)
inline constexpr uint64_t kColor = 0x18;          // SlotAndColor.color: 4 float RGBA
} // namespace wd
inline constexpr uint64_t kFollowerTypeInfo = 0x713C060;     // Follower (MonoBehaviour of the scene)
inline constexpr uint64_t kFollowerInfoTypeInfo = 0x713C098; // FollowerInfo
inline constexpr uint64_t kFloatingTypeInfo = 0x713C390;     // FollowerTask_Floating
inline constexpr uint64_t kManualControlTypeInfo = 0x713C478; // FollowerTask_ManualControl
inline constexpr uint64_t kHardManualControlTypeInfo = 0x713C3D8;
inline constexpr uint64_t kLightManualControlTypeInfo = 0x713C468;
inline constexpr uint64_t kEnforcingManualControlTypeInfo = 0x713C348;
inline constexpr uint64_t kSleepTaskTypeInfo = 0x713C5B0;    // FollowerTask_Sleep
inline constexpr uint64_t kBedRestTaskTypeInfo = 0x713C5B8;  // FollowerTask_SleepBedRest
// Lamb busy (static fields): menus, scene changes, conversations
inline constexpr uint64_t kUiMenuBaseTypeInfo = 0x7142908;     // Lamb.UI.UIMenuBase
inline constexpr uint64_t kUiMenuActiveMenus = 0x0;            // List<UIMenuBase>
inline constexpr uint64_t kGameManagerTypeInfo = 0x713C8A0;
inline constexpr uint64_t kGameManagerInMenu = 0x24;           // bool
inline constexpr uint64_t kMmTransitionTypeInfo = 0x713EBF0;   // MMTools.MMTransition
inline constexpr uint64_t kMmTransitionIsPlaying = 0x9;        // bool
inline constexpr uint64_t kMmConversationTypeInfo = 0x713EBD8; // MMTools.MMConversation
inline constexpr uint64_t kMmConversationIsPlaying = 0x38;     // bool
inline constexpr uint64_t kGetAttentionTypeInfo = 0x713C3C0; // FollowerTask_GetAttention
inline constexpr uint64_t kStructuresPubTypeInfo = 0x7141930; // Structures_Pub
inline constexpr uint64_t kPubIsDrinking = 0x0;               // static, bool

// Static fields
inline constexpr uint64_t kDataManagerInstance = 0xE0;  // DataManager.instance
inline constexpr uint64_t kDataManagerAllNecklaces = 0x50; // DataManager.AllNecklaces (static, List<ITEM_TYPE>)
inline constexpr uint64_t kPlayerFarmingInstance = 0x10; // PlayerFarming.Instance
inline constexpr uint64_t kAllBrains = 0x40;             // FollowerBrain.AllBrains

// PlayerFarming (player 1 = PlayerFarming.Instance)
namespace player {
inline constexpr uint64_t kLocation = 0x20;     // static: FollowerLocation
inline constexpr int32_t kLocationBase = 1;     // FollowerLocation.Base (base; Church = 0)
inline constexpr uint64_t kState = 0x150;       // _state (StateMachine)
inline constexpr uint64_t kPlayers = 0x0;       // static: List<PlayerFarming>
inline constexpr uint64_t kWasInConversation = 0x378; // read by AnyPlayerInConversation
inline constexpr uint64_t kPickedUpFollower = 0x2C0;  // <PickedUpFollower>k__BackingField
inline constexpr uint64_t kPlayerController = 0xF0;   // _playerController
inline constexpr uint64_t kUnitObject = 0xF8;         // _unitObject
inline constexpr uint64_t kGoToAndStopping = 0x2E8;   // GoToAndStopping (bool)
inline constexpr uint64_t kGoToCallback = 0x2F8;      // GoToCallback
inline constexpr uint64_t kAbortGoToCallback = 0x300; // AbortGoToCallback
inline constexpr uint64_t kGoToTarget = 0x310;        // forcePositionOnTimeoutTarget (Vector3)
inline constexpr uint64_t kLastPosition = 0x320;      // lastPosition (Vector3)
inline constexpr uint64_t kStateCurrent = 0x30; // StateMachine.currentstate (Idle 0, Moving 1)
inline constexpr uint64_t kStateLock = 0x44;    // StateMachine.<LockStateChanges> (bool)
inline constexpr int32_t kStateInActive = 13;   // StateMachine.State.InActive
inline constexpr uint64_t kRunTrinkets = 0x68;  // List<TarotCards.TarotCard>
inline constexpr uint64_t kHealth = 0x1C0;      // <health>k__BackingField (HealthPlayer)
inline constexpr uint64_t kWeapon = 0x350;      // _currentWeapon (EquipmentType)
inline constexpr uint64_t kCurse = 0x354;       // _currentCurse (EquipmentType, None = 9999)
inline constexpr uint64_t kWeaponLevel = 0x358; // currentWeaponLevel
inline constexpr uint64_t kCurseLevel = 0x360;  // currentCurseLevel
inline constexpr uint64_t kRelic = 0x368;       // currentRelicType (RelicType, None = 0)
} // namespace player

// Follower (scene object); Follower.Followers is static @0x0 (List<Follower>)
namespace follower {
inline constexpr uint64_t kFollowers = 0x0;
inline constexpr uint64_t kState = 0x98;             // Follower.State (StateMachine)
inline constexpr int32_t kStateTimedAction = 16;     // StateMachine.State.TimedAction, Follower.TimedAnimation
inline constexpr uint64_t kInteraction = 0xC8;       // Interaction_FollowerInteraction
inline constexpr uint64_t kBrain = 0x130;            // <Brain>k__BackingField
inline constexpr uint64_t kDying = 0x150;            // _dying (bool)
inline constexpr uint64_t kLightningIncoming = 0x1DC; // in the interaction (bool)
inline constexpr uint64_t kInteractState = 0x48;      // Interaction.state
inline constexpr uint64_t kInteractInteractable = 0x79; // Interaction.Interactable
inline constexpr uint64_t kInteractPlayer = 0x98;     // Interaction._playerFarming
inline constexpr uint64_t kInteractActivated = 0x129; // interaction_FollowerInteraction.Activated
inline constexpr uint64_t kInteractTask = 0x138;      // interaction_FollowerInteraction.Task
inline constexpr uint64_t kInteractAnimator = 0x140;  // ...: SimpleSpineAnimator of the coroutine
} // namespace follower

// UnityEngine.Object: m_CachedPtr is 0 once the object has been destroyed
inline constexpr uint64_t kUnityCachedPtr = 0x10;
// FollowerTask: subtask <ChangeLocationTask>
inline constexpr uint64_t kTaskChangeLocation = 0x28;
// FollowerTask.State and its value Doing
inline constexpr uint64_t kTaskState = 0x20;
inline constexpr int32_t kTaskStateDoing = 4;

// Health (base class of HealthPlayer): live values of the hearts in half hearts
// HealthPlayer: the getters read the own PLAYER_* fields, not the base fields of Health;
// only get_TotalSpiritHearts reads the base field _TotalSpiritHearts
namespace health {
inline constexpr uint64_t kTotalHp = 0x400;
inline constexpr uint64_t kHp = 0x3FC;
inline constexpr uint64_t kBlue = 0x3EC;
inline constexpr uint64_t kBlack = 0x3E8;
inline constexpr uint64_t kFire = 0x3F0;
inline constexpr uint64_t kIce = 0x3F4;
inline constexpr uint64_t kSpiritTotal = 0xC4;
inline constexpr uint64_t kSpirit = 0x3F8;
} // namespace health

// MMBiomeGeneration.BiomeGenerator (Instance static @0x18)
namespace biome {
inline constexpr uint64_t kInstance = 0x18;
inline constexpr uint64_t kRooms = 0x88;    // List<BiomeRoom>
inline constexpr uint64_t kCurrentX = 0x208;
inline constexpr uint64_t kCurrentY = 0x20C;
} // namespace biome

// BiomeRoom
namespace room {
inline constexpr uint64_t kX = 0x20;
inline constexpr uint64_t kY = 0x24;
inline constexpr uint64_t kConnections = 0x38; // N_Room, E_Room, S_Room, W_Room (8 bytes each)
inline constexpr uint64_t kDiscovered = 0x5A;
inline constexpr uint64_t kHidden = 0x5B;
inline constexpr uint64_t kGameObjectPath = 0x68; // string, room prefab
inline constexpr uint64_t kVisited = 0x5C;
inline constexpr uint64_t kIsRespawnRoom = 0x70;
inline constexpr uint64_t kIsDeathCatRoom = 0x71;
inline constexpr uint64_t kIsYngyaRoom = 0x72;
inline constexpr uint64_t kIsMysticShopRoom = 0x73;
inline constexpr uint64_t kIsBoss = 0x74;
} // namespace room

// RoomConnection
namespace conn {
inline constexpr uint64_t kRoom = 0x10; // BiomeRoom
inline constexpr uint64_t kType = 0x18; // GenerateRoom.ConnectionTypes (False = 0)
inline constexpr int32_t kTypeTrue = 1, kTypeBoss = 4, kTypeNextLayer = 6;
} // namespace conn

// StructuresData
namespace structure {
inline constexpr uint64_t kType = 0x10;      // StructureBrain.TYPES
inline constexpr uint64_t kInventory = 0x98; // List<InventoryItem>
} // namespace structure

// TarotCards.TarotCard
namespace tarot {
inline constexpr uint64_t kCard = 0x10;    // TarotCards.Card
inline constexpr uint64_t kUpgrade = 0x14; // UpgradeIndex
} // namespace tarot

// FollowerBrain
namespace brain {
inline constexpr uint64_t kInfo = 0x20;        // _directInfoAccess (FollowerInfo)
inline constexpr uint64_t kCurrentTask = 0xC8; // _currentTask
} // namespace brain

// ThoughtData
namespace thought {
inline constexpr uint64_t kType = 0x10;     // ThoughtType (Thought)
inline constexpr uint64_t kModifier = 0x18; // faith modifier
inline constexpr uint64_t kQuantity = 0x24; // stacked
} // namespace thought

// InventoryItem
namespace item {
inline constexpr uint64_t kType = 0x10;     // ITEM_TYPE
inline constexpr uint64_t kQuantity = 0x14;
inline constexpr uint64_t kQuantityReserved = 0x18;
} // namespace item

// FollowerTask_GetAttention
inline constexpr uint64_t kComplaintType = 0x68;
inline constexpr int32_t kComplaintGiveQuest = 8; // Follower.ComplaintType.GiveQuest

// ObjectivesData
namespace obj {
inline constexpr uint64_t kType = 0x10; // Objectives.TYPES
inline constexpr uint64_t kGroupId = 0x18; // string, I2 term of the title
inline constexpr uint64_t kUniqueGroupId = 0x40; // string, groups the sub-items of a quest
inline constexpr uint64_t kIsComplete = 0x20;
inline constexpr uint64_t kIsFailed = 0x21;
inline constexpr uint64_t kExpireDuration = 0x2C; // QuestExpireDuration
inline constexpr uint64_t kExpireTimestamp = 0x30;
inline constexpr uint64_t kFollower = 0x48;       // <Follower>k__BackingField
inline constexpr uint64_t kCollectItemType = 0x78; // Objectives_CollectItem.ItemType
inline constexpr int32_t kTypeCollectItem = 1;     // Objectives.TYPES.COLLECT_ITEM
inline constexpr int32_t kTypeGiveItem = 33;       // GIVE_ITEM (NPC quests, e.g. "Seeking Sozo")
inline constexpr uint64_t kGiveItemType = 0x88;    // Objectives_GiveItem.TargetType
inline constexpr int32_t kTypeKnucklebones = 24;   // DEFEAT_KNUCKLEBONES: string @0x78
inline constexpr int32_t kTypeKillEnemies = 8;
inline constexpr int32_t kTypeRecruitCursed = 22;  // FollowerName @0x88
inline constexpr int32_t kTypeFindFollower = 23;   // TargetFollowerName @0x90
inline constexpr int32_t kTypeBlizzard = 31;
inline constexpr int32_t kTypeLegendaryRun = 36;
inline constexpr int32_t kTypeFlockade = 38;       // opponent term @0x78
inline constexpr int32_t kTypeSwordReturn = 40;
inline constexpr int32_t kTypeFeedAnimal = 41;
inline constexpr int32_t kTypeWalkAnimal = 43;
inline constexpr int32_t kTypeBedRest = 15;        // SEND_FOLLOWER_BED_REST: string FollowerName @0x78
inline constexpr uint64_t kSubclassFields = 0x78;  // first field of the subclasses
} // namespace obj

// DataManager (instance fields)
namespace dm {
inline constexpr uint64_t kCurrentDayIndex = 0x610;
inline constexpr uint64_t kCurrentGameTime = 0x618; // TimeManager.get_CurrentGameTime
inline constexpr uint64_t kObjectives = 0xE38;      // List<ObjectivesData>
inline constexpr uint64_t kCompletedObjectives = 0xE40; // List<ObjectivesData>, completed ones
inline constexpr uint64_t kItems = 0xE80;               // List<InventoryItem> (base inventory)
inline constexpr uint64_t kFollowers = 0xCE0;           // List<FollowerInfo>
inline constexpr uint64_t kImprisonedIds = 0xDC8;       // List<int>
inline constexpr uint64_t kSwordReturnDay = 0x254;
inline constexpr uint64_t kAnimalLists[] = {0xB68, 0xB78, 0xB80}; // StructureManager.GetAnimalByID
inline constexpr uint64_t kBlizzardOfferings = 0xB90;
inline constexpr uint64_t kLegendaryKnown = 0xEE0; // List<EquipmentType>
inline constexpr uint64_t kEnemiesKilled = 0x11B0;
inline constexpr uint64_t kCultLevel = 0x540;      // CurrentCultLevel (CultLevel, One = 0)
inline constexpr uint64_t kCultName = 0x5D0;       // string
inline constexpr uint64_t kBaseStructures = 0xD18; // List<StructuresData>
inline constexpr uint64_t kElderlyIds = 0xDD0;          // List<int>
inline constexpr uint64_t kOnMissionaryIds = 0xDD8;     // List<int>
inline constexpr uint64_t kCultTraits = 0x5C0;          // List<TraitType>, cult traits
inline constexpr uint64_t kRecruit = 0xCE8;             // Followers_Recruit (List<FollowerInfo>)
inline constexpr uint64_t kTraitManipulatingIds = 0xDC0; // List<int>
inline constexpr uint64_t kLeftInDungeonIds = 0xDE0;    // List<int>
inline constexpr uint64_t kTransitioningIds = 0xDE8;    // List<int>
inline constexpr uint64_t kDemonIds = 0xDF0;            // List<int>
inline constexpr uint64_t kCurrentSeason = 0x16C;       // SeasonsManager.Season
inline constexpr uint64_t kCurrentWeatherEvent = 0x174; // SeasonsManager.WeatherEvent
inline constexpr uint64_t kCurrentPhase = 0x614;        // TimeManager.get_CurrentPhase
inline constexpr uint64_t kShowLoyaltyBars = 0x75A;     // bool
inline constexpr uint64_t kInTutorial = 0x850;          // bool, enables Make Demand in Normal/Poet
inline constexpr uint64_t kDoctrineUnlockedUpgrades = 0x5B0; // List<DoctrineType>
inline constexpr int32_t kDoctrineInspire = 6, kDoctrineIntimidate = 7; // WorkWorship_*
inline constexpr int32_t kItemBlackGold = 0x14;         // InventoryItem.ITEM_TYPE.BLACK_GOLD
inline constexpr int32_t kDoctrineBribe = 0xF;          // Possessions_Bribe
inline constexpr int32_t kDoctrineExtort = 0xE;         // Possessions_ExtortTithes
inline constexpr uint64_t kOnboardingPhase = 0x110;     // int, GiveWorkerCommands
inline constexpr uint64_t kHasBuiltShrine1 = 0x14C;     // bool
inline constexpr uint64_t kSozoNoLongerBrainwashed = 0x6C3; // bool
inline constexpr uint64_t kOnboardedWool = 0x95C;       // bool, Give Pet only after that (MakeDemandCommands)
// Declared at TotalElapsedGameTime (day * 1200 + time); -float.MaxValue: never (DataManager..ctor)
inline constexpr uint64_t kLastHolidayDeclared = 0x35C; // float, holiday 1200
inline constexpr uint64_t kLastNudismDeclared = 0x364;  // float, nudism 600
inline constexpr uint64_t kLastFastDeclared = 0x374;    // float, fasting 3600
} // namespace dm

// FollowerInfo
namespace fi {
inline constexpr uint64_t kId = 0x10;
inline constexpr uint64_t kName = 0x18; // string
inline constexpr uint64_t kCursedState = 0x20; // Thought: Starving 0x83, Freezing 0x1E0, Child 0x188
inline constexpr uint64_t kLeavingCult = 0xC0;
inline constexpr uint64_t kAge = 0xB4;
inline constexpr uint64_t kLifeExpectancy = 0xB8;
inline constexpr uint64_t kRole = 0x104;
inline constexpr uint64_t kThoughts = 0x120;  // List<ThoughtData>
inline constexpr uint64_t kAdoration = 0x128; // loyalty; FollowerBrainStats.get_Adoration
inline constexpr uint64_t kXpLevel = 0x12C;
inline constexpr uint64_t kDwellingId = 0x158;
inline constexpr uint64_t kTraits = 0x1E8; // List<TraitType>
inline constexpr uint64_t kOldAge = 0x1F4;
inline constexpr uint64_t kNecklace = 0x8C;          // ITEM_TYPE, FollowerBrainInfo.get_Necklace
inline constexpr uint64_t kShowingNecklace = 0x60;   // bool
inline constexpr uint64_t kDlcPets = 0xA0;           // List<FollowerPet.DLCPet>
inline constexpr uint64_t kIsSnowman = 0x210;        // bool
inline constexpr uint64_t kDrunk = 0x260;            // float, IsDrunk: > 0
inline constexpr uint64_t kReceivedBlessing = 0x2D1; // bool
inline constexpr uint64_t kKissed = 0x2D3;           // bool, KissedAction
inline constexpr uint64_t kInspired = 0x2D4;         // bool
inline constexpr uint64_t kPetDog = 0x2D5;           // bool
inline constexpr uint64_t kIntimidated = 0x2D7;      // bool
inline constexpr uint64_t kBribed = 0x2D8;           // bool
inline constexpr uint64_t kPaidTithes = 0x2D0;       // bool
inline constexpr uint64_t kScaredTraitInteracted = 0x2D9; // bool
inline constexpr uint64_t kMarriedToLeader = 0x1F5;  // bool
inline constexpr uint64_t kSkinName = 0x58;          // string
inline constexpr uint64_t kSkinColour = 0x50;        // int, colour set in WorshipperData.SkinAndData
inline constexpr uint64_t kWorkThroughNightDay = 0x2E4; // int; equals DM.CurrentDayIndex: WorkThroughNight
inline constexpr uint64_t kHappiness = 0x224;
inline constexpr uint64_t kFaith = 0x228;
inline constexpr uint64_t kSatiation = 0x230;
inline constexpr uint64_t kStarvation = 0x234;
inline constexpr uint64_t kIllness = 0x254;
inline constexpr uint64_t kInjured = 0x258;
inline constexpr uint64_t kExhaustion = 0x25C;
inline constexpr uint64_t kDissent = 0x280;
inline constexpr int32_t kThoughtStarving = 0x83, kThoughtFreezing = 0x1E0, kThoughtChild = 0x188;
inline constexpr int32_t kTraitExistentialDread = 0x4E, kTraitMissionaryTerrified = 0x43;
} // namespace fi

} // namespace cotl::off
