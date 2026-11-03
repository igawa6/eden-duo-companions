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
inline constexpr uint64_t kGetAttentionTypeInfo = 0x713C3C0; // FollowerTask_GetAttention

// Statische Felder
inline constexpr uint64_t kDataManagerInstance = 0xE0;  // DataManager.instance
inline constexpr uint64_t kPlayerFarmingInstance = 0x10; // PlayerFarming.Instance
inline constexpr uint64_t kAllBrains = 0x40;             // FollowerBrain.AllBrains

// PlayerFarming (player 1 = PlayerFarming.Instance)
namespace player {
inline constexpr uint64_t kLocation = 0x20;     // static: FollowerLocation
inline constexpr uint64_t kRunTrinkets = 0x68;  // List<TarotCards.TarotCard>
inline constexpr uint64_t kHealth = 0x1C0;      // <health>k__BackingField (HealthPlayer)
inline constexpr uint64_t kWeapon = 0x350;      // _currentWeapon (EquipmentType)
inline constexpr uint64_t kCurse = 0x354;       // _currentCurse (EquipmentType, None = 9999)
inline constexpr uint64_t kWeaponLevel = 0x358; // currentWeaponLevel
inline constexpr uint64_t kCurseLevel = 0x360;  // currentCurseLevel
inline constexpr uint64_t kRelic = 0x368;       // currentRelicType (RelicType, None = 0)
} // namespace player

// Health (base class of HealthPlayer): live heart values in half hearts
// HealthPlayer: the getters read their own PLAYER_* fields, not Health's base fields;
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
inline constexpr uint64_t kQuantity = 0x24; // gestapelt
} // namespace thought

// InventoryItem
namespace item {
inline constexpr uint64_t kType = 0x10;     // ITEM_TYPE
inline constexpr uint64_t kQuantity = 0x14;
} // namespace item

// FollowerTask_GetAttention
inline constexpr uint64_t kComplaintType = 0x68;
inline constexpr int32_t kComplaintGiveQuest = 8; // Follower.ComplaintType.GiveQuest

// ObjectivesData
namespace obj {
inline constexpr uint64_t kType = 0x10; // Objectives.TYPES
inline constexpr uint64_t kGroupId = 0x18; // string, I2 term of the title
inline constexpr uint64_t kUniqueGroupId = 0x40; // string, fasst Unterpunkte eines Quests zusammen
inline constexpr uint64_t kIsComplete = 0x20;
inline constexpr uint64_t kIsFailed = 0x21;
inline constexpr uint64_t kExpireDuration = 0x2C; // QuestExpireDuration
inline constexpr uint64_t kExpireTimestamp = 0x30;
inline constexpr uint64_t kFollower = 0x48;       // <Follower>k__BackingField
inline constexpr uint64_t kCollectItemType = 0x78; // Objectives_CollectItem.ItemType
inline constexpr int32_t kTypeCollectItem = 1;     // Objectives.TYPES.COLLECT_ITEM
inline constexpr int32_t kTypeGiveItem = 33;       // GIVE_ITEM (NPC-Quests, z. B. "Seeking Sozo")
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

// DataManager (Instanzfelder)
namespace dm {
inline constexpr uint64_t kCurrentDayIndex = 0x610;
inline constexpr uint64_t kCurrentGameTime = 0x618; // TimeManager.get_CurrentGameTime
inline constexpr uint64_t kObjectives = 0xE38;      // List<ObjectivesData>
inline constexpr uint64_t kCompletedObjectives = 0xE40; // List<ObjectivesData>, erledigte
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
} // namespace dm

// FollowerInfo
namespace fi {
inline constexpr uint64_t kId = 0x10;
inline constexpr uint64_t kName = 0x18; // string
inline constexpr uint64_t kCursedState = 0x20;
inline constexpr uint64_t kAge = 0xB4;
inline constexpr uint64_t kLifeExpectancy = 0xB8;
inline constexpr uint64_t kRole = 0x104;
inline constexpr uint64_t kThoughts = 0x120;  // List<ThoughtData>
inline constexpr uint64_t kAdoration = 0x128; // loyalty; FollowerBrainStats.get_Adoration
inline constexpr uint64_t kXpLevel = 0x12C;
inline constexpr uint64_t kDwellingId = 0x158;
inline constexpr uint64_t kTraits = 0x1E8; // List<TraitType>
inline constexpr uint64_t kOldAge = 0x1F4;
inline constexpr uint64_t kHappiness = 0x224;
inline constexpr uint64_t kFaith = 0x228;
inline constexpr uint64_t kSatiation = 0x230;
inline constexpr uint64_t kStarvation = 0x234;
inline constexpr uint64_t kIllness = 0x254;
inline constexpr uint64_t kInjured = 0x258;
inline constexpr uint64_t kExhaustion = 0x25C;
inline constexpr uint64_t kDissent = 0x280;
} // namespace fi

} // namespace cotl::off
