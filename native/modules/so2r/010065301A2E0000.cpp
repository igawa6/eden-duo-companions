// SPDX-FileCopyrightText: Copyright 2026 jadesigns.
// SPDX-License-Identifier: GPL-3.0-or-later
// SO2R -- Star Ocean: The Second Story R (010065301A2E0000) dual-screen module.
// 0.1.0 the party (values); 0.2.0 names from the game's own text tables, the game's font and
// pictures read from the player's own game files.
//
// Freestanding C++: no C/C++ runtime, no allocation, no exceptions. It only uses the host's
// read_memory / read_romfs / publish_* services, so it builds with a plain clang + lld (no
// Android NDK): see tools/so2r/build_module.sh. Built twice: the beta (SO2R_DIAG=0) and the diag
// build (SO2R_DIAG=1, which also publishes so2r.d.* lines for the DIAG page).
//
// ---- Game memory (update 1.0.1, build D214BA64D902AEDA; Unity 2021.3.22f1, IL2CPP metadata v29).
// Every offset below is the one the game's own code uses (research rounds 3-4, 7-8):
//   SingletonBasicMonoBehaviour<ParameterManager> TypeInfo slot  main+0x417DAE0 (script.json)
//     the game itself reaches it through the pointer at main+0x49496E8
//     (PartyManager.GetPartyMemberCount main+0x21660F0); on device both land on one class
//   -> Il2CppClass -> static_fields (+0xB8) -> instance (+0x0)            = ParameterManager
//   ParameterManager.userParameter            +0x7E0  (get_UserParameter main+0x1C71870)
//                    characterMessageDictionary +0x310  Dictionary<string, ConstCharacterMessage>
//                    playerParameter          +0x490  ScriptablePlayerParameter
//   UserParameter  heroID +0x10, money +0x14  (get_Money main+0x215F4D0)
//                  characterParameters +0x28 (CharacterParameter[], indexed by PlayerID:
//                  GetCharacterParameter main+0x2160FF0), partyParameter +0x38
//   PartyParameter playerIDs +0x10, leaderID +0x18, partyMemberStates +0x80,
//                  overridePlayerIDs +0xA0, isOverrideBattleMember +0xA8
//                  GetPartyMembers (main+0x1DC28C0) = isOverride ? overridePlayerIDs : playerIDs
//   CharacterParameter CharacterID +0x10, CharacterName +0x18 (string; empty in this game),
//                  Level +0x24, Experience +0x28, HitPointMax +0x50, HitPoint +0x54
//                  (get_HitPoint main+0x247E410), MentalPointMax +0x5C, MentalPoint +0x60
//   ScriptablePlayerParameter.param +0x18 List<ConstPlayerParameter>
//     ConstPlayerParameter playerID +0x10, charaNameID +0x18 (a CHARA_* string)
//   ConstCharacterMessage messageID +0x10, firstName +0x18, lastName +0x20
//   List<T>: _items +0x10, _size +0x18.  Dictionary<K,V>: _entries +0x18, _count +0x20;
//     Entry {hashCode +0, next +4, key +8, value +0x10}, 0x18 bytes, hashCode -1 = free.
//   Il2CppArray: max_length +0x18 (the game reads 32 bits), items +0x20.
//   Il2CppString: length +0x10, UTF-16 chars +0x14.  Il2CppClass.name: +0x10.
// Every object on the path is checked by its class name, so a stale or not-yet-initialised slot
// publishes nothing instead of junk.
//
// ---- Game files. pictures.idx (in the package; tools/so2r/build_picture_index.py makes it from a
// romfs: names and positions only, no pixels):
//   SO2P 1
//   B <id> <romfs path> <file size> <stream size> <blocks>
//   T <id> <bundle> <offset in the bundle's data stream> <size> <width> <height> <format> <gobs>
//   S <name> <texture> <x> <y> <w> <h> <rotation>     (texture pixels, origin bottom left)
//   F <name> <bundle> <object offset> <glyph table +off> <glyphs> <stride> <char table +off>
//     <chars> <stride> <atlas texture> <point size> <ascent*100> <line height*100>
//     <descent*100> <padding>
// The module reads a picture's blocks out of the bundle (UnityFS: big-endian header, LZ4
// blocks), undoes the Switch block-linear layout, decodes BC3 (or Alpha8 for the font), crops
// and flips. A bundle whose size differs from the index (another game version) is not used.

#include <stddef.h>
#include <stdint.h>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

#ifndef SO2R_DIAG
#define SO2R_DIAG 0
#endif

#define EXPORT extern "C" __attribute__((visibility("default")))

extern "C" __attribute__((visibility("hidden"))) void* memcpy(void* d, const void* s, size_t n) {
    auto* o = static_cast<uint8_t*>(d);
    auto* i = static_cast<const uint8_t*>(s);
    while (n--) *o++ = *i++;
    return d;
}
extern "C" __attribute__((visibility("hidden"))) void* memset(void* d, int c, size_t n) {
    auto* o = static_cast<uint8_t*>(d);
    while (n--) *o++ = static_cast<uint8_t>(c);
    return d;
}
extern "C" __attribute__((visibility("hidden"))) void* memmove(void* d, const void* s, size_t n) {
    auto* o = static_cast<uint8_t*>(d);
    auto* i = static_cast<const uint8_t*>(s);
    if (o < i) while (n--) *o++ = *i++;
    else { o += n; i += n; while (n--) *--o = *--i; }
    return d;
}

namespace {
extern char g_hh_diag[256];   // 0.9.142 HideHUD (defined after Sample)
extern int g_hh_on;

[[maybe_unused]] constexpr const char* kVersion = "1.0.0";
constexpr uint64_t kTitle = 0x010065301A2E0000ull;
constexpr const char* kBuild = "D214BA64D902AEDA";

constexpr uint64_t kParamMgrTypeInfo = 0x417DAE0;  // SingletonBasicMonoBehaviour<ParameterManager>
[[maybe_unused]] constexpr uint64_t kParamMgrRef = 0x49496E8;       // the game's pointer to that slot
// GameSceneManager: GameManager.GetSceneType (main+0x1F0DF30) reaches it through the usage slot at
// main+0x4949418 -> MethodInfo -> klass (+0x20) -> rgctx (+0xC0) -> [0] = the singleton class ->
// static_fields (+0xB8) -> instance (+0x0); currentSceneType +0x1C (get_CurrentSceneType
// main+0x2352510). SceneType: 3 Title, 4 Fieldmap, 5 Worldmap, 6 Battle, 7 Camp.
constexpr uint64_t kSceneRef = 0x4949418;
constexpr uint64_t kMiKlass = 0x20, kKlassRgctx = 0xC0, kKlassInited = 0x132, kGsmScene = 0x1C;
constexpr uint64_t kGsmCurrentScene = 0x28;   // GameSceneManager.currentScene (GameSceneBase; TitleScene on the title)
constexpr uint64_t kClassName = 0x10;

// ---- the map (round 11-13). FieldManager: SingletonBasicMonoBehaviour<FieldManager> slot.
constexpr uint64_t kFieldMgrTypeInfo = 0x417DAC8;
constexpr uint64_t kFmPos = 0x80, kFmDir = 0x8C, kFmFieldmap = 0x184, kFmWorldmap = 0x190;
// FieldManager.playerPosition is only set when a map loads (on the Thor it stayed at the entry
// point). The live position is the player's own ring of recent positions: FieldManager.fieldPlayer
// +0x78 -> FieldPlayer.prevPositions +0x230 (Vector3[]), startPrevPosition +0x238 (the ring's
// write point; FieldPlayer.GetPrevPosition). Entries just behind it are the newest.
constexpr uint64_t kFmPlayer = 0x78, kFpPrev = 0x230, kFpPrevStart = 0x238;
// ParameterManager.minimapParameter +0x750 (ScriptableMinimapParameter, its list at +0x18):
// ConstMinimapParameter fieldmapID +0x10, minimapID +0x14, angle +0x18 -- the turn the game gives
// an area's minimap (Arlia 270: drawn a quarter turn clockwise).
constexpr uint64_t kPmMinimapParam = 0x750, kMpList = 0x18, kCmpField = 0x10, kCmpId = 0x14, kCmpAngle = 0x18;
// GameUIManager: SingletonBasicMonoBehaviour<GameUIManager> slot; its +0x28 dictionary holds the
// UIFieldController under key 1 (GameUIManager.get_UIFieldController, main+0x1F4C750).
constexpr uint64_t kGameUiTypeInfo = 0x417DAD0, kGuiControllers = 0x28;
constexpr int32_t kGuiFieldKey = 1;
// UIFieldController (TypeDef 3596)
constexpr uint64_t kUfcWorldPresenter = 0x110, kUfcFieldMinimap = 0x1C0;
// 0.9.39 (r46) UIFieldController.minimapState: the game's own minimap state (enum MinimapState: Normal 0,
// Partial 1, Hide 2), switched by its ToggleMinimap
constexpr uint64_t kUfcMinimapState = 0x200;
// MinimapManager: presenter +0x10, currentMinimapID +0x18
constexpr uint64_t kMmPresenter = 0x10, kMmCurrentId = 0x18;
// UIMinimapPresenterBase: mapEuler +0x130, minimapScale +0x14C; UIMinimapPresenter scroll task +0x178
[[maybe_unused]] constexpr uint64_t kMpEuler = 0x130;
// UIMinimapPresenterBase.baseSize +0x120 (Vector2): the minimap window's size on the game's screen
constexpr uint64_t kMpBaseSize = 0x120;
// The game's own minimap icons (r16): UIMinimapPresenterBase.minimapIconPresenterList +0x118
// (List<UIMinimapIconPresenter>); each: iconType +0xF8, followTask +0x148 (UIFollowObjectOnMinimapTask:
// cachePosition +0x50, isExistsFollowObject +0x7C), isUsed +0x15C, worldPosition +0x160.
// The icon's picture is the presenter's iconSprite list (+0x110) indexed by MapIconType; the game's
// pictures carry the same names: icon_map_<type> (see kIconPic).
constexpr uint64_t kMpIconList = 0x118, kIpType = 0xF8, kIpFollow = 0x148, kIpUsed = 0x15C, kIpWorld = 0x160;
constexpr uint64_t kFtPos = 0x50, kFtExists = 0x7C;
constexpr int kMaxMarks = 64;
// MapIconType -> the game's picture (nullptr = not drawn: invalid, the player, cannot-pass, light)
constexpr const char* kIconPic[31] = {
    nullptr, nullptr, "icon_map_friend", "icon_map_enemy", "icon_map_npc", "icon_map_treasure_box",
    "icon_map_opened_treasure_box", "icon_map_savepoint", "icon_map_recover_save_point",
    "icon_map_scenario_event", "icon_map_sub_event", "icon_map_action_point", "icon_map_city",
    "icon_map_dungeon", "icon_map_psynard", "icon_map_door", "icon_map_stairs", "icon_map_gate",
    "icon_map_elevator", "icon_map_pa_event", "icon_map_inn", "icon_map_guild", "icon_map_shop_equipment",
    "icon_map_shop_item", "icon_map_shop_food", nullptr, "icon_map_dungeon_gimmick", nullptr,
    "icon_map_sub_event_limited", "icon_map_pa_event_limited", "icon_map_facility"};
// UIMinimapPresenterBase.background +0x108 (GameImage, a UnityEngine.UI.Image): Graphic.m_Color +0x20
// (Color, 4 floats) -- the game draws the white area picture with this colour (the grey minimap)
constexpr uint64_t kMpBackground = 0x108, kGraphicColor = 0x20;
constexpr uint64_t kGiBaseSize = 0x128, kGiCurSize = 0x130;   // GameImage baseSizeDelta / currentSizeDelta (r22)
constexpr uint64_t kMpScale = 0x14C, kMpTask = 0x178;
// UIScrollMinimapBackgroundTask: mapOffset +0x40, isWorldPartial +0x59, coefficient +0x64
[[maybe_unused]] constexpr uint64_t kTaskWorldPartial = 0x59;
constexpr uint64_t kTaskOffset = 0x40, kTaskCoeff = 0x64;
// UIWorldMinimapPresenter: backgroundSize +0x150, expelOffset +0x158, nedeOffset +0x160,
// worldMinimapScale +0x170 (expelScale / nedeScale)
constexpr uint64_t kWpBgSize = 0x150, kWpExpelOffset = 0x158, kWpNedeOffset = 0x160, kWpWorldScale = 0x170;
// GameDefine: main+0x4942D00 holds the address of its TypeInfo slot (GOT -> slot -> class, as
// GetBaseWorldToMinimapCoffiecient reads it): worldToMinimapCoffiecient +0x14C (5),
// worldToWorldMinimapCoffiecient +0x150 (0.2699) in its static fields
constexpr uint64_t kGameDefineRef = 0x4942D00, kGdField = 0x14C, kGdWorld = 0x150;
// The world map: UICommon.MapIconWorldToScreenPoint (main+0x2054810) puts a world position on the
// world-map texture through the minimap camera's projection-view matrix (UICommon.pvMatrix, a
// static at +0x78; UICommon's class through the GOT at main+0x4947080 -> slot -> class):
// ndc = (PV * (pos, 1)).xy / w, point = ((ndc + 1) / 2) * texture size - texture size / 2.
// The texture size is WorldMinimapManager / WorldPartialMinimapManager.textureWidth +0x1C, +0x20.
constexpr uint64_t kUiCommonRef = 0x4947080, kUcPvMatrix = 0x78;
constexpr uint64_t kUfcWorldMinimap = 0x1C8, kUfcWorldPartial = 0x1D0, kWmTexW = 0x1C, kWmTexH = 0x20;
constexpr uint64_t kClassStatics = 0xB8;
constexpr uint64_t kPmUser = 0x7E0, kPmCharMsg = 0x310, kPmPlayerParam = 0x490;
// r43/r44 the Guide Map's objective: destinationParameter, destinationMessageDictionary; UserParameter's hero
// (heroID) and scenario progress
constexpr uint64_t kPmDestParam = 0x718, kPmDestMsg = 0x3B0, kUpScenario = 0xD0;
// systemMessageDictionary +0x300: Dictionary<string, ConstSystemMessage> (messageID +0x10, message +0x18)
constexpr uint64_t kPmSysMsg = 0x300, kCsmMessage = 0x18;
// the area's name: ConstMinimapParameter.messageID (+0x30) is a key of the game's location messages
// (ParameterManager.locationMessageDictionary +0x390, ConstLocationMessage.name +0x18) or, failing
// that, of its system messages
constexpr uint64_t kCmpMessageId = 0x30, kPmLocMsg = 0x390, kClmName = 0x18;
// 0.5.0 on the Thor: the minimap row's messageID is empty. The area's own name keys are in
// ParameterManager.fieldParameter +0x508 (ScriptableFieldParameter, list +0x18): ConstFieldParameter
// fieldmapID +0x14, fieldmapNameID +0x20, guidemapNameID +0x28
constexpr uint64_t kPmFieldParam = 0x508, kCfpField = 0x14, kCfpNameId = 0x20, kCfpGuideId = 0x28;
[[maybe_unused]] constexpr uint64_t kUpHero = 0x10;
constexpr uint64_t kUpMoney = 0x14, kUpChars = 0x28, kUpParty = 0x38;
constexpr uint64_t kPpLeader = 0x18;
[[maybe_unused]] constexpr uint64_t kPpStates = 0x80;
constexpr uint64_t kPpIds = 0x10, kPpOverIds = 0xA0, kPpOver = 0xA8;
constexpr uint64_t kArrLen = 0x18, kArrItems = 0x20;
constexpr uint64_t kStrLen = 0x10, kStrChars = 0x14;
constexpr uint64_t kListItems = 0x10, kListSize = 0x18;
constexpr uint64_t kDictEntries = 0x18, kDictCount = 0x20, kEntrySize = 0x18, kEntryKey = 8, kEntryValue = 0x10;
constexpr uint64_t kSppList = 0x18, kCppId = 0x10, kCppNameId = 0x18;
constexpr uint64_t kCcmFirst = 0x18;   // messageID +0x10, lastName +0x20
// CharacterParameter fields read in one block from +0x10 to +0x64
constexpr uint64_t kCpBlock = 0x10, kCpBlockSize = 0x144;   // through Talent +0x150 (r18 / r20)
// r18 CharacterParameter: the current stats (each after its *Base), in the order of the game's
// Status words ATK DEF HIT AVD INT LUC STM CRT GUTS (SYS_0264..0272); Equipment +0xFC: 7 item IDs in
// UIDefine.EquipType order (Weapon, Armor, Shield, Helmet, Greeve, Accessory1, Accessory2)
constexpr uint64_t kCpStat[9] = {0x68 /*Strength*/, 0x70 /*Constitution*/, 0x80 /*HitRate*/, 0x88 /*EvasionRate*/,
                                 0x78 /*Intelligence*/, 0xB0 /*Luck*/, 0xB8 /*StaminaMax*/, 0x98 /*CriticalRate*/,
                                 0x90 /*Guts*/};
constexpr uint64_t kCpEquip = 0xFC;
constexpr int kEquipSlots = 7;
// items: ParameterManager.itemParameterDictionary +0x7B0 (Dictionary<int, ConstItemParameter>,
// itemNameID +0x18) -> itemMessageDictionary +0x320 (ConstItemMessage: messageID +0x10, name +0x18)
constexpr uint64_t kPmItemParam = 0x7B0, kPmItemMsg = 0x320, kCipNameId = 0x18, kCimName = 0x18;
// 0.6.1 on the Thor: the dictionary gave nothing -- the item table itself, ScriptableItemParameter +0x640
// (param +0x18, ConstItemParameter itemID +0x10), is read when it does not
constexpr uint64_t kPmItemTable = 0x640, kCipId = 0x10;
// r23 skills: ParameterManager skillMessageDictionary +0x340 (ConstSkillMessage name +0x18, the
// dictionary GetSkillMessage reads), skillParameter +0x618, specialSkillParameter +0x630,
// superSpecialSkillParameter +0x638 (each: param list +0x18; rows: id +0x10, name ID +0x18;
// ConstSkillParameter levelupSp List<int> +0x20). CharacterParameter SkillLevels int[] +0x128,
// SkillPoint +0x130, SpecialSkillLevels int[] +0x148 (r22); UserParameter superSpecialSkillLevels
// int[] +0x50. SP to improve (CharacterParameter.GetNeedSkillPoint): levelupSp[level] - 2 x the
// level of SkillID 11 (Determination), at least 1; none once level >= the list's length.
constexpr uint64_t kPmSkillMsg = 0x340, kCsmsgName = 0x18;
constexpr uint64_t kPmSkillParam = 0x618, kPmSpecialParam = 0x630, kPmSuperParam = 0x638;
constexpr uint64_t kSkRowId = 0x10, kSkRowName = 0x18, kSkRowSp = 0x20;
constexpr uint64_t kCpSkillLv = 0x128, kCpSkillPoint = 0x130, kCpSpecialLv = 0x148;
constexpr uint64_t kUpSuperLv = 0x50;
constexpr int kSkillGuts = 11;
// r23 guest: UICommon.IsGuestPlayer = PartyParameter.GetPartyJoiningState(pid) == 2;
// partyJoiningStates PartyJoiningState[] +0x78
constexpr uint64_t kPpJoining = 0x78;
// r23 IC or Specialty: UICommon.IsItemCreation looks the SpecialSkillID up in a static list (the
// class through main+0x49497C0, static_fields +0x98; IList of SpecialSkillID)
constexpr uint64_t kIcListRef = 0x49497C0, kIcListStatic = 0x98;
constexpr int kMaxSkill = 40, kMaxSpecial = 24, kMaxSuper = 16, kMaxSpLv = 16;
// r22 / r23 combat skills: CharacterParameter CombatSkillLevels int[] +0x138, CombatSkillPoint (BP)
// +0x140; ParameterManager combatSkillParameter +0x620 (rows combatSkillID +0x10, skillNameID +0x18,
// initialLearning +0x20, levelupBp List<int> +0x30), names in battleSkillMessageDictionary +0x350
// (ConstBattleSkillMessage name +0x18); UserParameter releasedCombatSkillFlag +0x220 (BitFlag)
constexpr uint64_t kCpCombatLv = 0x138, kCpCombatPoint = 0x140;
constexpr uint64_t kPmCombatParam = 0x620, kPmBattleMsg = 0x350, kCbRowInit = 0x20, kCbRowBp = 0x30;
constexpr uint64_t kUpCombatReleased = 0x220;
constexpr int kMaxCombat = 32;
// r26 special arts / spells: CharacterParameter LearningBattleSkills (struct array +0x160: battleSkillID
// +0, battleSkillLevel +4, 12 bytes each); ParameterManager battleSkillParameter +0x4D8 (rows
// battleSkillID +0x10, battleSkillNameID +0x18; names in battleSkillMessageDictionary +0x350) and
// learningBattleSkillParameter +0x4E8 (rows playerID +0x10, battleSkillID +0x1C, levelupBp +0x20).
// BP to improve (CalcNeedCombatSkillPoint(BattleSkillID, toLevel)): levelupBp[toLevel == GameDefine
// static +0x54 ? 1 : 0].
constexpr uint64_t kCpEquipBs = 0x158;   // EquipmentBattleSkillIDs int[] (L, R)
// r33: an art's top level is its levelLimit (CharacterParameter.GetReachingBattleSkillLevelMaxCount counts
// the learned arts whose level >= it)
constexpr uint64_t kBsLevelLimit = 0x7C;
constexpr uint64_t kCpLearnBs = 0x160, kPmBsParam = 0x4D8, kPmLearnBs = 0x4E8, kLbPid = 0x10, kLbId = 0x1C, kLbBp = 0x20;
constexpr uint64_t kGdBsTop = 0x54;
// r34 the rest of the missions: GameDefine maxSkillLevel +0x50, maxCombatSkillLevel +0x58; the
// MissionUtility class (statics +0 CombatSkillLevelTargetCount); UserParameter variableFlag +0x228;
// ParameterManager musicDevilMelodyParameter +0x178
// r34 favorite food (UICampStatusSelector.UpdateLikeFood): shown when CharacterParameter.characterFlag
// (+0x118) bit 5 is set -- the item name of ConstPlayerParameter.favoriteDishItemID (+0x80) -- else
// UICommon.GetQuestionMesasage (its system message key: a string literal through main+0x4957F80)
constexpr uint64_t kCpCharaFlag = 0x118, kCppFavDish = 0x80, kQuestionKeyRef = 0x4957F80;
constexpr int kFlagFoodKnown = 5;
// r35 the tables / flags behind more missions (ParameterManager lists, UserParameter flags)
constexpr uint64_t kUpMusicFlag = 0x160, kUpFormationFlag = 0x120, kUpObtainedItem = 0xA8, kUpEnemyFlag = 0x98,
                   kUpEnemyDefeats = 0x1B8, kUpFishCaught = 0x1F8, kUpFishRecord = 0x190;
constexpr uint64_t kPmItemBook = 0x6D0, kPmEnemyBook = 0x6C8, kPmQuestList = 0x6A8, kPmFishBook = 0x6D8,
                   kPmFishingFish = 0xF0, kFishRecordSize = 0x2C;
constexpr uint64_t kGdMaxSkill = 0x50, kGdMaxCombat = 0x58, kMissionUtilityRef = 0x494BDA8, kUpVarFlag = 0x228,
                   kPmMelody = 0x178;
constexpr int kMaxArts = 16, kMaxLearn = 256;
// r24 / r25 missions: ParameterManager missionParameter +0x6B0 (rows missionID +0x10, missionCategory
// +0x14, missionMessageID +0x20, value +0x38, rewardID +0x3C), rewardParameter +0x668 (rows rewardID
// +0x10, rewardType +0x14, value +0x18, count +0x1C), names in questMessageDictionary +0x380
// (ConstQuestMessage name +0x18; UICommon.GetMissionMessage replaces a token, a string literal
// through main+0x4958190, with the mission's value). UserParameter clearedMissionFlag +0xC8,
// releasedMissionFlag +0x110; quests (Guild Missions): receivedQuestFlag +0xB8, clearedQuestFlag
// +0xC0 (BitFlag: int[] +0x10, count +0x18). Tabs: MissionToCategory = category 3..5 -> 1..3, else 0;
// their message IDs: string literals through main+0x420ED10 (4).
constexpr uint64_t kPmMissionParam = 0x6B0, kPmRewardParam = 0x668, kPmQuestMsg = 0x380, kCqmName = 0x18;
constexpr uint64_t kMsId = 0x10, kMsCat = 0x14, kMsMsg = 0x20, kMsValue = 0x38, kMsReward = 0x3C;
constexpr uint64_t kRwId = 0x10, kRwType = 0x14, kRwValue = 0x18, kRwCount = 0x1C;
constexpr uint64_t kUpMisCleared = 0xC8, kUpMisReleased = 0x110, kUpQuestReceived = 0xB8, kUpQuestCleared = 0xC0;
constexpr uint64_t kMisTokenRef = 0x4958190, kMisTabKeys = 0x420ED10;
// r29 / r30 the element row (UICommon.CreateElementalDataList): the elements in the game's order are
// the int array at the IC list's class statics +0xD8 (ElementIDs); each member's ElementResistances
// (CharacterParameter +0xC8) is indexed by ElementID; ToSystemMessageID(ElementResistanceType) takes
// the word's message ID from string literals through main+0x420DD38 (types 0..4: INVALID DOUBLE
// HALF DISABLE ABSORB).
constexpr uint64_t kElOrderStatic = 0xD8, kElWordKeys = 0x420DD38;
// r32 the Equipment screen's slot icons (UICampEquipSelector.GetItemIcon): its sprite list is indexed
// by ItemCategoryType; the weapon slot takes the character's weapon category, the others fixed ones
// (Armor 20, Shield 19, Helmet 18, Greaves 21, Accessory 22). The sprites are icon_item_<the enum's
// name in lower case>. An item's category: ConstItemParameter.itemCategoryType +0xB8.
constexpr uint64_t kCipCategory = 0xB8;
constexpr int kSlotCategory[7] = {0, 20, 19, 18, 21, 22, 22};
const char* const kItemCategoryName[23] = {
    "", "battle", "material", "cooking", "fish", "unidentified", "others", "valuable", "sword", "twin_sword",
    "wand", "knuckle", "punch", "book", "whip", "gun_and_disk", "stungun", "rod", "helmet", "shield", "armor",
    "greeve", "accessory"};
constexpr int kElTypes = 5, kElMax = 8;
// r27 mission progress. MissionUtility.GetAchievementCount switches on missionType (+0x18, 1..71)
// through a halfword table (main+0x35D55CE, case = main+0x22D3CAC + 4 x entry); most cases are
// UserParameter.GetVariableIntValue(N) (variableIntValues int[] +0x138, [N]), the rest call helpers:
// GetMaxSpecialSkillLevel(+0x30) (the most of the party members' levels; +0x30 == 0: the super
// specialty +0x34 instead), GetSpecialSkillExecutionCount (+0x128 int[]), GetSuperSpecialSkillLevel
// (+0x50), GetSuperSpecialSkillExecutionCount (+0x130), GetMaxCharacterLevel. The module reads the
// table and the case code from the game and follows the cases it knows; the others get no counter.
// Target (GetTargetAchivementCount, byte table main+0x35D565C): 0 = value (+0x38), 1 = 1, 0x0A = 15,
// others counted at run time (no counter). Achieved = count >= target (IsAchieved); the list shows
// the game's Complete when achieved (UpdateState: !achieved 1, achieved 4, achieved + cleared 5).
constexpr uint64_t kMsType = 0x18, kMsSpec = 0x30, kMsSuper = 0x34, kMsPrev = 0x28, kMsStart = 0x2C;   // previousMissionID, startScenarioProgress
// 0.9.61 (r54) the getters and fields behind mission types 29 / 33 / 39 / 43
constexpr uint64_t kFnPaGroupList = 0x1C7B540, kFnBoxList = 0x1C7A3B0, kFnBoxFlag = 0x2161D00, kUpPaConfirmed = 0x90,
                   kPmLocation = 0x578, kUpLocReleased = 0x78, kPmChallenge = 0x418, kUpChalCleared = 0x1A0;
constexpr uint64_t kAchTable = 0x35D55CE, kAchBase = 0x22D3CAC, kAchVarTail = 0x22D43AC;
constexpr uint64_t kFnMaxSpecLv = 0x2162890, kFnSpecExec = 0x21626C0, kFnMaxCharLv = 0x2162D10;
// (their super specialty branches: GetSuperSpecialSkillLevel main+0x2161C00, ...ExecutionCount main+0x2162AD0)
constexpr uint64_t kUpSpecExec = 0x128, kUpSuperExec = 0x130, kUpVarInt = 0x138;
constexpr uint8_t kTargetTable[70] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0A, 0, 0x0F, 0x1C, 0, 1, 0, 0, 0, 0, 0, 0, 0x31, 0x63, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 1};
enum AchKind : uint8_t { kAchUnknown, kAchVar, kAchSpecLv, kAchSpecLearned, kAchSpecExec, kAchMaxCharLv };
constexpr int kMaxMissions = 256, kMaxQuests = 128, kMisRows = 80;   // the rows a list can hold (it scrolls)
// r20: CharacterParameter LevelBase +0x20, Talent +0x150 (bit n = TalentID n, SetTalent); GameDefine
// statics maxLevel +0x4C
constexpr uint64_t kCpLevelBase = 0x20, kCpTalent = 0x150, kGdMaxLevel = 0x4C;
// friendship (r18 / r20): UserParameter.emotionParameter +0x40 -> friendEmotions short[] +0x10,
// [from * 14 + to] (SetFriendEmotion writes both ways); the most it can be: GameDefine statics +0x78
constexpr uint64_t kUpEmotion = 0x40, kEmFriend = 0x10, kGdMaxFriend = 0x78;
// 0.6.2 on the Thor: the Equipment numbers are possession unique IDs (2049, 2050 ...), not item IDs.
// r21: UserParameter.possessionItemParameter +0x30 -> possessionItems PossessionItem[] +0x10;
// PossessionItem saveUniqueItemID +0x10 (short), saveItemID +0x12 (short)
constexpr uint64_t kUpPossession = 0x30, kPipItems = 0x10, kPiUnique = 0x10;   // saveItemID right after it
constexpr uint64_t kCpId = 0x10, kCpName = 0x18, kCpLevel = 0x24, kCpExp = 0x28, kCpHpMax = 0x50,
                   kCpHp = 0x54, kCpMpMax = 0x5C, kCpMp = 0x60;

constexpr int kRows = 8;            // rows on the PARTY page
constexpr int kMaxIds = 16;
constexpr int kMaxBattle = 4;         // GameDefine.maxBattleMember (r40: IsBattleMember reads it; 4 in this game)         // party arrays are read up to this many entries
constexpr int kMaxPlayer = 14;      // PlayerID: 1 the hero .. 13 WELCH, 14 MAX
constexpr int kReadEvery = 6;       // samples per memory read (10 Hz); values republished every sample
constexpr int kMapEvery = 2;        // the map's position is read at 30 Hz, so the arrow moves smoothly
constexpr int kNameMax = 40;        // bytes of UTF-8 per name
constexpr int kNameRefresh = 50;    // reads between name lookups (5 s: follows a language change)

// ---------------------------------------------------------------- small helpers
bool eq(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}
bool starts(const char* s, const char* p) {
    while (*p) if (*s++ != *p++) return false;
    return true;
}

struct Str {   // fixed text builder
    char s[256];
    int n = 0;
    Str() { s[0] = 0; }
    Str& clear() { n = 0; s[0] = 0; return *this; }
    Str& add(const char* t) { while (t && *t && n < 255) s[n++] = *t++; s[n] = 0; return *this; }
    Str& ch(char c) { if (n < 255) s[n++] = c; s[n] = 0; return *this; }
    Str& num(int64_t v) {
        char b[24]; int k = 0; bool neg = v < 0; uint64_t u = neg ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
        do { b[k++] = static_cast<char>('0' + u % 10); u /= 10; } while (u);
        if (neg) ch('-');
        while (k) ch(b[--k]);
        return *this;
    }
    Str& hex(uint64_t v) {
        add("0x"); char b[16]; int k = 0;
        do { b[k++] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v);
        while (k) ch(b[--k]);
        return *this;
    }
};

int32_t RoundF(float f) { return static_cast<int32_t>(f < 0 ? f - 0.5f : f + 0.5f); }

// tiny spin lock: the bundle reader is shared by the asset worker (pictures, font atlas) and the
// data reader (font metrics), which may run at the same time
struct Lock {
    int* v;
    explicit Lock(int* p) : v(p) { while (__atomic_exchange_n(v, 1, __ATOMIC_ACQUIRE)) {} }
    ~Lock() { if (v) __atomic_store_n(v, 0, __ATOMIC_RELEASE); }
    // the game's thread never waits for the picture worker: it takes the lock only when free
    struct Try {};
    Lock(int* p, Try) : v(__atomic_exchange_n(p, 1, __ATOMIC_ACQUIRE) ? nullptr : p) {}
    bool held() const { return v != nullptr; }
};
int g_file_lock;

struct Guest {
    const EdenDsmodHostApi* h;
    bool read(uint64_t a, void* out, size_t n) const {
        return a && h->read_memory(h->userdata, a, out, n) == EDEN_DSMOD_TRUE;
    }
    uint64_t ptr(uint64_t a) const { uint64_t v = 0; return read(a, &v, 8) ? v : 0; }
    int32_t i32(uint64_t a, bool* ok = nullptr) const {
        int32_t v = 0; bool r = read(a, &v, 4); if (ok) *ok = r; return v;
    }
    uint8_t u8(uint64_t a) const { uint8_t v = 0; read(a, &v, 1); return v; }
    float f32(uint64_t a, bool* ok = nullptr) const {
        float v = 0; bool r = read(a, &v, 4) && v == v && v > -1e7f && v < 1e7f; if (ok) *ok = r; return r ? v : 0;
    }
    static bool objp(uint64_t p) { return p && !(p & 7); }
    bool class_name(uint64_t obj, char* out, size_t n) const {
        out[0] = 0;
        if (!objp(obj)) return false;
        return cstr(ptr(obj), out, n);
    }
    bool cstr(uint64_t klass, char* out, size_t n) const {
        out[0] = 0;
        if (!objp(klass)) return false;
        uint64_t s = ptr(klass + kClassName);
        if (!s) return false;
        size_t i = 0;
        for (; i + 1 < n; ++i) {
            char c = 0;
            if (!read(s + i, &c, 1)) return false;
            if (!c) break;
            if (c < 0x20 || c > 0x7E) { out[0] = 0; return false; }
            out[i] = c;
        }
        out[i] = 0;
        return i > 0;
    }
    bool is(uint64_t obj, const char* want) const {
        char b[48];
        return class_name(obj, b, sizeof b) && eq(b, want);
    }
    // IL2CPP System.String -> UTF-16 (up to 64 units); length or -1
    int utf16(uint64_t s, uint16_t* w, int cap) const {
        if (!objp(s)) return -1;
        bool ok = false;
        int32_t len = i32(s + kStrLen, &ok);
        if (!ok || len < 0 || len > cap) return -1;
        if (len && !read(s + kStrChars, w, static_cast<size_t>(len) * 2)) return -1;
        return len;
    }
    // IL2CPP System.String -> UTF-8 (BMP only; surrogates dropped; <tags> removed when strip)
    bool string(uint64_t s, char* out, int n, bool strip = false) const {
        out[0] = 0;
        uint16_t w[64];
        int len = utf16(s, w, 64);
        if (len < 0) return false;
        int k = 0;
        bool tag = false;
        for (int i = 0; i < len; ++i) {
            uint32_t c = w[i];
            if (strip && c == '<') { tag = true; continue; }
            if (tag) { if (c == '>') tag = false; continue; }
            if (c >= 0xD800 && c <= 0xDFFF) continue;
            int need = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
            if (k + need >= n) break;
            if (need == 1) out[k++] = static_cast<char>(c);
            else if (need == 2) { out[k++] = static_cast<char>(0xC0 | (c >> 6)); out[k++] = static_cast<char>(0x80 | (c & 0x3F)); }
            else { out[k++] = static_cast<char>(0xE0 | (c >> 12)); out[k++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out[k++] = static_cast<char>(0x80 | (c & 0x3F)); }
        }
        out[k] = 0;
        return true;
    }
    // Il2CppArray of int32: length and up to `max` items
    int ints(uint64_t arr, int32_t* out, int max, int* len_out) const {
        *len_out = -1;
        if (!objp(arr)) return 0;
        bool ok = false;
        int32_t len = i32(arr + kArrLen, &ok);
        if (!ok || len < 0 || len > 4096) return 0;
        *len_out = len;
        int n = len < max ? len : max;
        if (n && !read(arr + kArrItems, out, static_cast<size_t>(n) * 4)) { *len_out = -1; return 0; }
        return n;
    }
};

// ---------------------------------------------------------------- state
struct Member {
    int on, pid, cid, level, exp, hp, hpmax, mp, mpmax, leader;
    int stat[9];            // ATK DEF HIT AVD INT LUC STM CRT GUTS (see kCpStat)
    int equip[7];           // possession unique IDs (0 / -1 = nothing)
    int equip_item[7];      // their item IDs
    int level_base, talent;
    int guest;              // PartyJoiningState 2 (r23)
    int slot, battle;       // 0.9.17: its party slot; a battle member (PartyMemberState Battle = 1, r23)
    int food_known;         // characterFlag (+0x118) bit 5: the favorite food is known (r34)
    int sp;                 // SkillPoint
    int nskl, nssk;         // lengths read
    int8_t skl[kMaxSkill];  // SkillLevels by SkillID
    int8_t ssk[kMaxSpecial];   // SpecialSkillLevels by SpecialSkillID
    int ncmb, bp;
    int8_t cmb[kMaxCombat];    // CombatSkillLevels by CombatSkillID
    int nart;                  // LearningBattleSkills: id, level
    int16_t art_id[kMaxArts], art_lv[kMaxArts];
    int art_eq[2];             // EquipmentBattleSkillIDs: the L and R buttons
    int nelem; int8_t elem[12], elem_base[12];   // ElementResistances / ...Base (r28; by ElementID)
    uint32_t bd;            // BuffDebuffState's first word (bit n = BuffDebuffID n; r19 BitFlag: int[] +0x10)
    char name[kNameMax];
};

struct NameCache {          // PlayerID -> first name, from the game's character messages
    char name[kMaxPlayer][kNameMax];
    char id[kMaxPlayer][32];   // charaNameID (a CHARA_* string)
    uint64_t dict;              // the dictionary they came from
    uint32_t age;
};

#include "so2r_fieldmaps.inc"

// The page's words, from the game's own system messages (r17), looked up once and cached; until a
// word is found the page shows our fallback. A new dictionary (another language) looks them up again.
struct SysWord { const char* point; const char* key; const char* fallback; };
constexpr bool StrEqC(const char* a, const char* b) { return *a == *b && (*a == 0 || StrEqC(a + 1, b + 1)); }
constexpr SysWord kSysWords[] = {
    {"so2r.t.party", "SYS_0324", "Party"},
    {"so2r.t.map", "SYS_0153", "Guide Map"},
    {"so2r.t.leader", "SYS_0036", "LEADER"},
    {"so2r.t.guest", "SYS_0513", "GUEST"},
    // the Skills page (r23)
    {"so2r.t.skills", "SYS_0065", "Skills"}, {"so2r.t.sp", "SYS_0058", "SP"}, {"so2r.t.ic", "SYS_0014", "IC"},
    {"so2r.t.spec", "SYS_0015", "Specialty"}, {"so2r.t.super", "SYS_0026", "Super Specialty"},
    {"so2r.t.bp", "SYS_0059", "BP"}, {"so2r.t.arts", "SYS_0025", "Special Arts/Spells"},
    {"so2r.t.combat", "SYS_0290", "Combat Skill"}, {"so2r.t.button", "BUTTON_PLATFORM_SWITCH", "Button"},
    {"so2r.t.batk", "~Attack", ""}, {"so2r.t.bctr", "~Counter", "Counter"},   // 0.9.95 found by their text; 0.9.98 B: "Counter" (the player's word)
    {"so2r.t.none", "SYS_0004", "None"},
    {"so2r.t.improve", "=Improve", "Improve"},
    {"so2r.t.cont", "=Continue", "Continue"},    // 0.9.152 the title page's button (the title menu's Continue, by its text)   // 0.9.122 the Camp Menu's Improve (found by its text)
    // missions (r23 / r24)
    {"so2r.t.missions", "SYS_0336", "Missions"}, {"so2r.t.cmis", "SYS_0193", "Challenge Missions"},
    {"so2r.t.gmis", "SYS_0031", "Guild Missions"}, {"so2r.t.done", "SYS_0510", "Complete"},
    {"so2r.t.notdone", "SYS_0323", "Incomplete"}, {"so2r.t.nomis", "SYS_3116", "There are no missions"},
    {"so2r.t.fol", "SYS_0056", "FOL"}, {"so2r.t.q", "SYS_0102", "????"}, {"so2r.t.exp2", "SYS_0138", "EXP"},
    {"so2r.t.rank0", "SYS_3106", "Beginner"}, {"so2r.t.rank1", "SYS_3107", "Advanced"},
    {"so2r.t.rank2", "SYS_3108", "Expert"}, {"so2r.t.rank3", "", "Legend"},
    {"so2r.t.lv", "SYS_0040", "Lv"},
    // the element row (r30): the fallbacks when the game's keys can't be read
    {"so2r.t.element", "SYS_0390", "Element"},
    {"so2r.t.food", "SYS_0119", "Favorite Food"},
    {"so2r.t.el0", "SYS_0279", "-"}, {"so2r.t.el1", "SYS_0273", "Weakness"}, {"so2r.t.el2", "SYS_0274", "1/2 Effect"},
    {"so2r.t.el3", "SYS_0275", "No Effect"}, {"so2r.t.el4", "SYS_0276", "Absorb"},
    {"so2r.t.hp", "SYS_0060", "HP"},
    {"so2r.t.mp", "SYS_0061", "MP"},
    {"so2r.t.world", "SYS_0017", "World Map"},
    {"so2r.t.here", "SYS_1407", "Current Location"},
    // 0.9.17 the Map page's quick menu (the field's d-pad / X menu)
    // r40: the game's own words (IC/Specialty is built from the game's IC and Specialty below)
    {"so2r.t.nextobj", "SYS_0412", "Next objective"},    // 0.9.24 the Guide Map's heading (r42)
    {"so2r.t.qitems", "SYS_0016", "Items"}, {"so2r.t.qheal", "SYS_0089", "Quick Heal"}, {"so2r.t.qcamp", "SYS_0012", "Camp Menu"},
    {"so2r.t.qpa", "SYS_0160", "Switch PA"},            // 0.9.25 the field's Y (r17)
    // the Guide Map's legend (r17)
    {"so2r.t.dest", "SYS_0514", "Destination"},
    {"so2r.t.sub", "SYS_0515", "Sub Event"},
    {"so2r.t.pa", "SYS_0516", "PA Event"},
    {"so2r.t.limit", "SYS_0518", "Limited Time"},
    // the Status page (r18)
    {"so2r.t.status", "SYS_0024", "Status"},
    {"so2r.t.equipment", "SYS_0023", "Equipment"},
    {"so2r.t.exp", "SYS_0138", "EXP"},
    {"so2r.t.st0", "SYS_0264", "ATK"}, {"so2r.t.st1", "SYS_0265", "DEF"}, {"so2r.t.st2", "SYS_0266", "HIT"},
    {"so2r.t.st3", "SYS_0267", "AVD"}, {"so2r.t.st4", "SYS_0268", "INT"}, {"so2r.t.st5", "SYS_0269", "LUC"},
    {"so2r.t.st6", "SYS_0270", "STM"}, {"so2r.t.st7", "SYS_0271", "CRT"}, {"so2r.t.st8", "SYS_0272", "GUTS"},
    {"so2r.t.eq0", "SYS_0295", "Weapon"}, {"so2r.t.eq1", "SYS_0183", "Armor"}, {"so2r.t.eq2", "SYS_0184", "Shield"},
    {"so2r.t.eq3", "SYS_0185", "Helmet"}, {"so2r.t.eq4", "SYS_0186", "Greaves"},   // r19
    {"so2r.t.eq5", "SYS_0282", "Accessory 1"}, {"so2r.t.eq6", "SYS_0283", "Accessory 2"},
    // status effects (r19): BuffDebuffID 1-6, the ailments (BuffDebuffType ABNORMAL), BUFF_DEBUFF_000n
    // r20: Next Level, Talents and the ten talents (TalentID 1-10; SYS_0079 Originality, SYS_0080 Taste,
    // SYS_0082 Design Sense seen in r20, the rest in the same run)
    {"so2r.t.next", "SYS_0124", "Next Level"}, {"so2r.t.friend", "SYS_0125", "Friendship Level"},
    {"so2r.t.food", "SYS_0119", "Favorite Food"}, {"so2r.t.swpage", "SYS_0141", "Switch Pages"}, {"so2r.t.talents", "SYS_0120", "Talents"},
    {"so2r.t.tl1", "SYS_0079", "Originality"}, {"so2r.t.tl2", "SYS_0080", "Taste"}, {"so2r.t.tl3", "SYS_0081", "Dexterity"},
    {"so2r.t.tl4", "SYS_0082", "Design Sense"}, {"so2r.t.tl5", "SYS_0083", "Literary"}, {"so2r.t.tl6", "SYS_0084", "Rhythm"},
    {"so2r.t.tl7", "SYS_0085", "Sense of Sound"}, {"so2r.t.tl8", "SYS_0086", "Animal Lover"},
    {"so2r.t.tl9", "SYS_0087", "Intuition"}, {"so2r.t.tl10", "SYS_0088", "Mana Bless"},
    {"so2r.t.bd1", "BUFF_DEBUFF_0001", "Poison"}, {"so2r.t.bd2", "BUFF_DEBUFF_0002", "Paralysis"},
    {"so2r.t.bd3", "BUFF_DEBUFF_0003", "Petrification"}, {"so2r.t.bd4", "BUFF_DEBUFF_0004", "Confusion"},
    {"so2r.t.bd5", "BUFF_DEBUFF_0005", "Silence"}, {"so2r.t.bd6", "BUFF_DEBUFF_0006", "Stun"},
    // 0.9.60 Party Formation (the Camp Menu's Y) and its X "Leader": their keys not known yet, so found by the
    // game's own English text ("=": a system message whose text is this); the key found shows on the diag page
    {"so2r.t.setlead", "SYS_0129", "Leader"},
    // 0.9.70 the Battle page: its tab and the battle command menu's words (found by their text until their keys are
    // known; the keys show on the diag page)
    {"so2r.t.battle", "=Battle", "Battle"}, {"so2r.t.bspell", "=Spells", "Spells"},
    {"so2r.t.bstrat", "=Strategy", "Strategy"}, {"so2r.t.bescape", "=Escape", "Escape"},   // 0.9.62 (found by its text in 0.9.61: SYS_0129; Party Formation SYS_0334)
    // 0.9.81 the battle's Y orders (CommandID 1..4), found by their text ("~": letters and digits only, so the
    // game's own apostrophe either way); no fallback is shown -- the button stays blank until the game's word is found
    // 0.9.83 by key (r62: UIBattleOperationSelector names each row UIDefine.ToMessageID(commandID) -> the system
    // messages COMMAND_0001..0004); the text search found none of them (only SYS / B keys are searched)
    {"so2r.t.ord1", "COMMAND_0001", ""}, {"so2r.t.ord2", "COMMAND_0002", ""},
    {"so2r.t.ord3", "COMMAND_0003", ""}, {"so2r.t.ord4", "COMMAND_0004", ""},
};
constexpr int kSysWordCount = static_cast<int>(sizeof kSysWords / sizeof kSysWords[0]);
constexpr int FindWord(const char* p, int i = 0) {
    return i >= kSysWordCount ? -1 : (StrEqC(kSysWords[i].point, p) ? i : FindWord(p, i + 1));
}
constexpr int kBdWord0 = FindWord("so2r.t.bd1");
constexpr int kOrdWord0 = FindWord("so2r.t.ord1");   // 0.9.81
constexpr int kWorldWord = FindWord("so2r.t.world"), kHereWord = FindWord("so2r.t.here");
constexpr int kIcWord = FindWord("so2r.t.ic"), kSpecWord = FindWord("so2r.t.spec");   // the quick menu's IC/Specialty
static_assert(kWorldWord >= 0 && kHereWord >= 0, "map words");
constexpr int kTlWord0 = FindWord("so2r.t.tl1");
static_assert(kTlWord0 > 0 && FindWord("so2r.t.tl10") == kTlWord0 + 9, "talent words in order");
static_assert(kBdWord0 > 0 && FindWord("so2r.t.bd6") == kBdWord0 + 5, "status effect words in order");
struct SysWords {
    char text[kSysWordCount][64];
    char key[kSysWordCount][24];     // 0.9.60 the key a "=" word was found under (diag)
    bool found[kSysWordCount];
    int nfound;
    uint64_t dict;
    uint32_t age;
};


struct MapState {
    int ok;                 // a picture and a position
    const char* why;
    char area[48];          // the picture (= the map area's key)
    float w, h;             // its size in pixels
    float x, y;             // the player in picture pixels, y up from the bottom edge
    float heading;          // degrees clockwise, 0 = up
    float rot;              // the area's minimap turn (degrees clockwise) as the game draws it
    float mini_half;        // half the game's world minimap window, picture pixels (0 = not read)
    float mini_r;           // world map: picture pixels per world-minimap pixel (0 = not read)
    float wbase;            // world map: the whole-world minimap's width (its icons' scale)
    float wscale;           // world map: picture pixels per round-minimap pixel used
    float wplayer[2];       // world map: the player icon's place on the round minimap (diag)
    float area_half;        // area: half the game's minimap window, picture pixels (0 = not read)
    // how a world position lands on the picture (the same for the player and every icon)
    int pmode;              // 1 area (offset, k), 2 world (matrix), 3 world (presenter scale)
    float pk, pox, poy;     // area: k, offset; world fallback: k, offset (canvas units)
    float pv[16], ptw, pth, pf;
    struct Mark { float x, y; int type; uint32_t argb; } marks[kMaxMarks];   // argb: the game's tint (0 = none)
    int memo_dropped;       // remembered icons the game no longer uses (diag)
    int unused_follow, hidden_drawn;   // 0.9.31 (diag): unused icons with an object; see-through ones drawn
    uint32_t enemy_argb;    // the last enemy icon's colour (diag)
#if SO2R_DIAG
    struct RawMark { int type, follow, list, sub; float w[3], c[3], e[3], lp[3]; char go[20]; uint64_t ip; } raw[8];
    int nraw;
    float win[2];           // the window used to keep icons (half size, minimap pixels)
#endif
    int nmarks, icons_seen;
    int clamped;            // 0.9.37 the round world minimap's destination icons left out (pinned to its rim)
    int list_used[3], list_hidden[3];   // 0.9.37 used / see-through fixed icons per list (diag)
    int mvis[3], mhid[3];   // 0.9.38 per list: used icons whose colour reads visible / see-through (MiniFrom)
    int unverified[3];      // 0.9.38 per list: world icons not placed (no verified geometry yet)
    int ndest;              // 0.9.38 the area's destination icons used this pass
    int mini;               // 0.9.38 the game's minimap shown (MiniState)
    int mstate;             // 0.9.39 UIFieldController.minimapState as read (-1: not read)
    int mstate_run;         // 0.9.39 passes it has held
    int marked;             // 0.9.42 the game-marked objects drawn as the "!" this pass
    int wcarried;           // 0.9.37 the kept world icons carried into a read that lacked them (diag)
    int wcached;            // 0.9.18: the world's icons drawn from the last read (the game's minimap off)
    bool live;              // the position came from the player's live ring
    int rot_field, rot_id;  // the area the turn was looked up for
    char place[64];         // the area's name in the game's words ("" = not found)
    char place_id[40];      // its message key (diag)
    char place_id2[40];     // the guide map's name key (diag)
    char floor[32];         // the floor's name in the game's words (MINIMAP_0001 "1F" ...; "" = none)
    char floor_id[24];      // its key: the minimap row's messageID (diag)
    int place_src;          // 1 location message, 2 system message, 0 none (diag)
    int place_field, place_mid, place_age;   // the area the name was found for
    int fieldmap, worldmap, minimap_id, kind;   // kind 1 area minimap, 2 world map
#if SO2R_DIAG
    float px, py, pz, offx, offy, coeff, scale, euler, extra, wtw, wth, wcw, mbase, mscale;
    float bgcur[2], bgbase[2], bgsc[3], bgfac, bgk;   // the area picture as the game draws it (0.6.5)
    int wp, bgsrc;
    uint64_t ufc, mgr, pres, task;
#endif
};

struct Instance {
    uint64_t main_base;
    uint32_t samples;
    int ok, rows, leader, scene;
    char scene_cls[40];
    uint64_t pm;            // ParameterManager
    MapState map;
    SysWords words;
    int64_t fol;
    int max_level;          // GameDefine.maxLevel (r20)
    int bs_top;             // GameDefine static +0x54 (the level that costs levelupBp[1])
    int max_friend;         // GameDefine +0x78 (r20)
    int16_t friend_[14 * 14];   // friendEmotions
    int sss_lv[kMaxSuper], nsss;   // super specialty levels (UserParameter, party-wide)
    int ic_known, ic_n;            // the game's IC list was read; its length
    bool is_ic[kMaxSpecial];
    int32_t joining[kMaxIds]; int joining_len;
    uint32_t cmb_rel[2];                 // releasedCombatSkillFlag words
    uint32_t mis_clr[8], mis_rel[8];     // missions cleared / released (bit = MissionID)
    uint32_t q_rcv[4], q_clr[4];         // quests received / cleared (bit = QuestID)
    int32_t var_int[160]; int nvar;      // variableIntValues
    uint32_t var_flag[16]; int nvar_flag;   // variableFlag (UserParameter +0x228) words
    int gd_max_skill, gd_max_combat;     // GameDefine.maxSkillLevel +0x50 / maxCombatSkillLevel +0x58
    int mu_combat_target;                // MissionUtility.CombatSkillLevelTargetCount (statics +0)
    int n_skill_rows, n_combat_rows, n_melody;   // the skill / combat skill / daemon melody tables' sizes
    // favorite food (r34): CharacterParameter characterFlag bit 5 says it's known; the dish is
    // ConstPlayerParameter.favoriteDishItemID +0x80
    int fav_item[kMaxPlayer];
    // r35 GetAchievementCount helpers (-1: not read): composed songs (9), item / enemy book rates in %
    // (30, 31), cleared quests (32), formations released (38), fish at their record size (66)
    int c_music, c_item, c_enemy, c_quest, c_form, c_fish;
    int c_pa, c_box, c_loc, c_chal;      // 0.9.61 (r53 / r54) mission types 33, 29, 39, 43: -1 unknown
    int32_t spec_exec[kMaxSpecial]; int nspec_exec;
    int32_t super_exec[kMaxSuper]; int nsuper_exec;
    int flags_ok;
    int el_order[kElMax], nel;           // the element row's ElementIDs, in the game's order
#if SO2R_DIAG
    char ic_cls[24], ic_obj[24];
#endif
    Member m[kRows];
    const char* why;
    uint32_t reads, rejects;
    NameCache names;
#if SO2R_DIAG
    uint64_t klass, klass_ref, slot_via_ref, inst, user, party, charmsg, playerparam;
    int64_t delta;
    char klass_name[48], inst_name[48];
    int hero, over, ids_len, over_len, chars_len, msg_count, pp_count;
    int32_t ids[kMaxIds], over_ids[kMaxIds];
    int rejected_pid[kMaxIds], nrej;
#endif
    // 0.9.17 partyMemberStates: entries read, its length; the battle members read by PlayerID (1), by slot (2)
    // or neither (0: the first four)
    int32_t states[kMaxIds];
    int states_n, states_len, battle_how;
    char obj[200];          // 0.9.24 the Guide Map's next objective (empty: none)
    int obj_prog, obj_row;
    uint64_t save_key;      // 0.9.38 the save's UserParameter (an objective's identity includes it)
    int obj_hero;           // 0.9.40 the hero (UserParameter +0x10), for the map destination table
    // 0.9.19 the game's open windows (r41: GameUIManager.windowController +0x18 -> openWindowStack +0x18,
    // Stack<OpenWindowStackData>: _array +0x10, _size +0x18; OpenWindowStackData.type +0x10 = UIDefine.WindowType)
    int win_n, win_step;    // windows open (-1 unread); how far the path read (diag)
    int win_type[4];        // the top four, top first
    uint64_t win_ctl;       // 0.9.60 the WindowController (its windowDictionary +0x10: WindowType -> window)
    // 0.9.72 a battle (r55 / r56): the BattleManager, the controlled member's CharacterID, the assault members
    uint64_t bm; int bm_step; int ctrl_cid; int ctrl_idx;
    int as_n; int as_cid[4]; int as_state[4]; int as_last;
    int as_pic[4]; int as_rise[4]; float as_t[4], as_base[4]; char as_model[4][24];   // 0.9.74 dot portrait number, ready per mille
    int bn_on, bn_pt, bn_next, bn_ratio, bn_level, bn_rows;   // 0.9.89 the bonus gauge (r64 / r65)
    int bn_n, bn_icon[3];                                      // 0.9.90 the bonus effects in force (r66)
    char bn_text[3][64];
    int op_open;            // 0.9.82 the game's Y list (UIBattleOperationSelector) on top of the battle screens
    int order;              // 0.9.81 the battle's Y order: PartyParameter.commandID (+0x20), 1..4; -1 unknown
    char as_name[4][32];    // 0.9.73 the assault member's name (a party member's, or the assist's own charaNameID)
};
Instance g_inst;
// which d-pad direction calls which entry (0.9.72 on the Thor: entry 0 shows the down arrow, entry 1 the left;
// 0.9.81 with four set: entry 2 up, entry 3 the right arrow);
// -1: not seen yet, no button
constexpr int kAssistBtn[4] = {2, 1, 0, 3};
int g_opick = 0;        // 0.9.82 the Battle page's order list shown (Y tapped)
int g_bord_req = -1;    // 0.9.82 an order picked there (CommandID 1..4)
int g_bo_cid[3] = {-1, -1, -1};   // 0.9.86 the Battle page's member cards: whose (CharacterID) each one is
int g_bsw_req = -1;               // 0.9.86 a card tapped: switch control to that CharacterID
[[maybe_unused]] void BattleDiagLine(const Instance& in, const EdenDsmodHostApi* h, struct Str& l);   // 0.9.72 (diag)
[[maybe_unused]] void CampDiagLine(const Instance& in, const EdenDsmodHostApi* h, struct Str& l);   // 0.9.62 (diag)
// a page word: the game's, else ours
const char* Word(const Instance& in, int i) { return in.words.found[i] ? in.words.text[i] : kSysWords[i].fallback; }
// a BitFlag's words (int[] +0x10); 0 when unreadable
int BitWords(const Guest& g, uint64_t bf, uint32_t* out, int max) {
    for (int i = 0; i < max; ++i) out[i] = 0;
    if (!Guest::objp(bf)) return 0;
    int32_t t[256]; int len = -1;
    const int n = g.ints(g.ptr(bf + 0x10), t, max < 256 ? max : 256, &len);
    for (int i = 0; i < n; ++i) out[i] = static_cast<uint32_t>(t[i]);
    return n;
}
inline bool Bit(const uint32_t* w, int nw, int i) { return i >= 0 && (i >> 5) < nw && (w[i >> 5] >> (i & 31) & 1); }
int g_sel;                          // the Status page's member (a party row)
int g_spage;                        // the Status page's page: 0 status + equipment, 1 talents + friendship
int g_kpage;                        // the Skills subtab's page: 0 skills (SP), 1 combat skills (BP), 2 IC / Specialty / Super Specialty
// 0.9.10 the Party tab: 0 the roster, 1 a member's details; the details' subtab: 0 Status, 1 Equipment,
// 2 Special Arts / Spells, 3 Skills (kept when the member changes; the member kept when the subtab changes)
int g_view, g_sub;
int g_mtab, g_mpage, g_msec;                // the Missions page: tab 0-3 the ranks, 4 Guild Missions; page of the list
// item names, looked up once per item ID (the item table is a few thousand rows)
// unique ID -> its slot in possessionItems (looked up once, checked each time)
struct UniqueSlot { int uid, idx; };
UniqueSlot g_uq[32];
int g_nuq;
// the item ID of a possession unique ID (0 = not found)
int ItemOfUnique(const Guest& g, uint64_t up, int uid) {
    if (uid <= 0 || !Guest::objp(up)) return 0;
    const uint64_t pip = g.ptr(up + kUpPossession);
    const uint64_t arr = Guest::objp(pip) ? g.ptr(pip + kPipItems) : 0;
    bool ok = false;
    const int32_t n = Guest::objp(arr) ? g.i32(arr + kArrLen, &ok) : 0;
    if (!ok || n <= 0 || n > 20000) return 0;
    auto at = [&](int i, int16_t* rec) -> bool {
        const uint64_t pi = g.ptr(arr + kArrItems + static_cast<uint64_t>(i) * 8);
        return Guest::objp(pi) && g.read(pi + kPiUnique, rec, 4);
    };
    int16_t rec[2];
    for (int k = 0; k < g_nuq; ++k)
        if (g_uq[k].uid == uid && at(g_uq[k].idx, rec) && rec[0] == uid) return rec[1];
    // the game's own lookup indexes by unique ID first (ItemManager.GetPossessionItem); then a scan
    int found = -1;
    if (uid < n && at(uid, rec) && rec[0] == uid) found = uid;
    for (int i = 0; found < 0 && i < n; i += 256) {
        uint64_t ptrs[256];
        const int cnt = n - i < 256 ? n - i : 256;
        if (!g.read(arr + kArrItems + static_cast<uint64_t>(i) * 8, ptrs, static_cast<size_t>(cnt) * 8)) break;
        for (int j = 0; j < cnt; ++j)
            if (Guest::objp(ptrs[j]) && g.read(ptrs[j] + kPiUnique, rec, 4) && rec[0] == uid) { found = i + j; break; }
    }
    if (found < 0) return 0;
    UniqueSlot& e = g_uq[g_nuq < 32 ? g_nuq++ : uid % 32];
    e = {uid, found};
    at(found, rec);
    return rec[1];
}

struct ItemName { int id; int tries, src; int cat; char name[48]; char key[24]; };   // cat: ConstItemParameter.itemCategoryType (r32)   // src: 1 dictionary, 2 table (diag)
ItemName g_items[64];
int g_nitems;
int g_map_whole;                    // the Map page's "Whole map" switch (survives a new boot)
int g_town_zoom;                    // towns / dungeons: 0 = the whole area (default), 1 = the game's minimap size
int g_view_kind;                    // the kind of map last shown (1 area, 2 world): what a tap switches
// the minimap window: half its height in picture pixels (the game's round minimap shows about
// 320 px of an area's picture), the map widget's aspect, and how far the arrow may wander from the
// window's centre (as a share of the half size) before the map scrolls
constexpr float kMiniHalfWorld = 180.0f;   // fallback: half the world minimap's height, in world-map pixels

bool sane(const Member& r) {
    return r.level >= 1 && r.level <= 999 && r.hpmax >= 1 && r.hpmax <= 999999 && r.hp >= 0 &&
           r.hp <= r.hpmax && r.mpmax >= 0 && r.mpmax <= 999999 && r.mp >= 0 && r.mp <= r.mpmax &&
           r.exp >= 0;
}

int32_t rd32(const uint8_t* b, uint64_t off) {   // field at object offset `off` inside the block
    int32_t v; memcpy(&v, b + (off - kCpBlock), 4); return v;
}

// ---------------------------------------------------------------- names (the game's own tables)
// charaNameID for every PlayerID from ScriptablePlayerParameter, then each one's firstName from
// characterMessageDictionary -- the same text the game's menus show, in the game's language.
void LoadNames(Instance& in, const Guest& g, uint64_t pm) {
    NameCache& nc = in.names;
    const uint64_t dict = g.ptr(pm + kPmCharMsg);
    if (nc.dict == dict && nc.age++ < static_cast<uint32_t>(kNameRefresh)) return;
    memset(&nc, 0, sizeof nc);
    nc.dict = dict;
    const uint64_t spp = g.ptr(pm + kPmPlayerParam);
#if SO2R_DIAG
    in.charmsg = dict; in.playerparam = spp; in.msg_count = in.pp_count = -1;
#endif
    if (!g.is(spp, "ScriptablePlayerParameter")) return;
    const uint64_t list = g.ptr(spp + kSppList);
    const uint64_t items = g.ptr(list + kListItems);
    bool ok = false;
    const int32_t n = Guest::objp(list) ? g.i32(list + kListSize, &ok) : -1;
#if SO2R_DIAG
    in.pp_count = ok ? n : -1;
#endif
    if (!ok || n <= 0 || n > 64 || !Guest::objp(items)) return;
    uint16_t want[kMaxPlayer][32];
    int wlen[kMaxPlayer];
    for (int i = 0; i < kMaxPlayer; ++i) wlen[i] = -1;
    for (int i = 0; i < n; ++i) {
        const uint64_t p = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
        if (!g.is(p, "ConstPlayerParameter")) continue;
        const int pid = g.i32(p + kCppId);
        if (pid <= 0 || pid >= kMaxPlayer) continue;
        const uint64_t s = g.ptr(p + kCppNameId);
        in.fav_item[pid] = g.i32(p + kCppFavDish);
        wlen[pid] = g.utf16(s, want[pid], 32);
        g.string(s, nc.id[pid], 32);
    }
    if (!g.is(dict, "Dictionary`2")) return;
    const uint64_t entries = g.ptr(dict + kDictEntries);
    const int32_t count = g.i32(dict + kDictCount, &ok);
#if SO2R_DIAG
    in.msg_count = ok ? count : -1;
#endif
    if (!ok || count <= 0 || count > 8192 || !Guest::objp(entries)) return;
    for (int32_t e = 0; e < count; ++e) {
        const uint64_t ea = entries + kArrItems + static_cast<uint64_t>(e) * kEntrySize;
        uint8_t rec[kEntrySize];
        if (!g.read(ea, rec, sizeof rec)) break;
        int32_t hash; memcpy(&hash, rec, 4);
        if (hash < 0) continue;
        uint64_t key, val; memcpy(&key, rec + kEntryKey, 8); memcpy(&val, rec + kEntryValue, 8);
        uint16_t kw[32];
        const int kl = g.utf16(key, kw, 32);
        if (kl <= 6 || kw[0] != 'C' || kw[1] != 'H') continue;   // only CHARA_* keys
        for (int pid = 1; pid < kMaxPlayer; ++pid) {
            if (wlen[pid] != kl || nc.name[pid][0]) continue;
            bool same = true;
            for (int k = 0; k < kl && same; ++k) same = kw[k] == want[pid][k];
            if (!same) continue;
            if (g.is(val, "ConstCharacterMessage"))
                g.string(g.ptr(val + kCcmFirst), nc.name[pid], kNameMax, true);
        }
    }
}

// a Dictionary<string, T> entry's value for a key (UTF-16 compare), 0 when not there
uint64_t DictFind(const Guest& g, uint64_t dict, const uint16_t* want, int wl) {
    if (!g.is(dict, "Dictionary`2") || wl <= 0) return 0;
    const uint64_t entries = g.ptr(dict + kDictEntries);
    bool ok = false;
    const int32_t count = g.i32(dict + kDictCount, &ok);
    if (!ok || count <= 0 || count > 8192 || !Guest::objp(entries)) return 0;
    for (int32_t e = 0; e < count; ++e) {
        uint8_t rec[kEntrySize];
        if (!g.read(entries + kArrItems + static_cast<uint64_t>(e) * kEntrySize, rec, sizeof rec)) break;
        int32_t hash; memcpy(&hash, rec, 4);
        if (hash < 0) continue;
        uint64_t key, val; memcpy(&key, rec + kEntryKey, 8); memcpy(&val, rec + kEntryValue, 8);
        uint16_t kw[40];
        if (g.utf16(key, kw, 40) != wl) continue;
        bool same = true;
        for (int k = 0; k < wl && same; ++k) same = kw[k] == want[k];
        if (same) return val;
    }
    return 0;
}

// EXP needed for level `lv + 1` in total: the game's CalcNeedExp (r20), entry lv - 1 of its table:
// sum over i < lv of (i <= 98 ? (i / 10 + 5) * ((i + 1)^2 + 1) : 15 * (i + 1)^2 + 15)
int64_t NeedExp(int lv) {
    int64_t t = 0;
    for (int i = 0; i < lv; ++i) {
        const int64_t s = static_cast<int64_t>(i + 1) * (i + 1);
        t += i + 1 > 99 ? 15 * s + 15 : (i / 10 + 5) * (s + 1);
    }
    return t;
}

// an item's name in the game's words (cached; retried a few times while the tables load)
const ItemName* ItemNameOf(const Guest& g, uint64_t pm, int id) {
    if (id <= 0) return nullptr;
    int k = 0;
    while (k < g_nitems && g_items[k].id != id) ++k;
    if (k == g_nitems) {
        if (g_nitems < 64) ++g_nitems; else k = id % 64;
        g_items[k] = {};
        g_items[k].id = id;
    }
    ItemName& e = g_items[k];
    if (e.name[0] || e.tries >= 5) return &e;
    ++e.tries;
    uint64_t row = 0;
    const uint64_t dict = g.ptr(pm + kPmItemParam);
    if (g.is(dict, "Dictionary`2")) {
        const uint64_t entries = g.ptr(dict + kDictEntries);
        bool ok = false;
        const int32_t count = g.i32(dict + kDictCount, &ok);
        for (int32_t i = 0; ok && Guest::objp(entries) && i < count && i < 20000; ++i) {
            uint8_t rec[kEntrySize];
            if (!g.read(entries + kArrItems + static_cast<uint64_t>(i) * kEntrySize, rec, sizeof rec)) break;
            int32_t hash, key; memcpy(&hash, rec, 4); memcpy(&key, rec + kEntryKey, 4);
            if (hash < 0 || key != id) continue;
            uint64_t v; memcpy(&v, rec + kEntryValue, 8);
            if (g.is(v, "ConstItemParameter")) { row = v; e.src = 1; }
            break;
        }
    }
    if (!row) {                                  // the table itself
        const uint64_t sip = g.ptr(pm + kPmItemTable);
        const uint64_t list = Guest::objp(sip) ? g.ptr(sip + kMpList) : 0;
        bool ok = false;
        const int32_t n = Guest::objp(list) ? g.i32(list + kListSize, &ok) : 0;
        const uint64_t items = ok ? g.ptr(list + kListItems) : 0;
        for (int32_t i = 0; ok && Guest::objp(items) && i < n && i < 20000; ++i) {
            const uint64_t v = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
            if (Guest::objp(v) && g.i32(v + kCipId) == id) { if (g.is(v, "ConstItemParameter")) { row = v; e.src = 2; } break; }
        }
    }
    if (!row && !g.is(dict, "Dictionary`2") && !Guest::objp(g.ptr(pm + kPmItemTable))) --e.tries;   // no tables yet: not a try
    if (row) e.cat = g.i32(row + kCipCategory);
    {
        const uint64_t nk = row ? g.ptr(row + kCipNameId) : 0;
        uint16_t kw[40];
        const int kl = nk ? g.utf16(nk, kw, 40) : 0;
        g.string(nk, e.key, sizeof e.key);
        const uint64_t msg = kl > 0 && kl < 40 ? DictFind(g, g.ptr(pm + kPmItemMsg), kw, kl) : 0;
        if (g.is(msg, "ConstItemMessage")) g.string(g.ptr(msg + kCimName), e.name, sizeof e.name, true);
    }
    return &e;
}

// ---- skill / specialty / super specialty names (the game's skill messages) and SP lists, in the
// order of the game's tables; read once per message dictionary (retried while the tables load)
struct SkillTable {
    uint64_t dict;
    int tries, n, nss, nsss;
    struct Row { int id; char name[40]; int nsp; int16_t sp[kMaxSpLv]; bool init; } skl[kMaxSkill], ssk[kMaxSpecial], sss[kMaxSuper];
    uint64_t bdict;
    int btries, ncmb;
    Row cmb[kMaxCombat];
};
SkillTable g_sk;
void LoadSkillTable(const Guest& g, uint64_t pm) {
    const uint64_t dict = g.ptr(pm + kPmSkillMsg);
    if (dict == g_sk.dict && (g_sk.n > 0 || g_sk.tries >= 20)) return;
    if (dict != g_sk.dict) { g_sk = SkillTable{}; g_sk.dict = dict; }
    ++g_sk.tries;
    auto fill = [&](uint64_t table, SkillTable::Row* rows, int maxr, bool sp, uint64_t dict = 0, uint64_t spoff = kSkRowSp) -> int {
        if (!dict) dict = g.ptr(pm + kPmSkillMsg);
        const uint64_t sp_ = g.ptr(pm + table);
        const uint64_t list = Guest::objp(sp_) ? g.ptr(sp_ + kMpList) : 0;
        bool ok = false;
        const int32_t n = Guest::objp(list) ? g.i32(list + kListSize, &ok) : 0;
        const uint64_t items = ok ? g.ptr(list + kListItems) : 0;
        int out = 0;
        for (int32_t i = 0; ok && Guest::objp(items) && i < n && i < 256 && out < maxr; ++i) {
            const uint64_t row = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
            if (!Guest::objp(row)) continue;
            SkillTable::Row& r = rows[out];
            r = SkillTable::Row{};
            r.id = g.i32(row + kSkRowId);
            if (r.id <= 0 || r.id >= maxr) continue;
            const uint64_t nk = g.ptr(row + kSkRowName);
            uint16_t kw[40];
            const int kl = nk ? g.utf16(nk, kw, 40) : 0;
            const uint64_t msg = kl > 0 && kl < 40 && Guest::objp(dict) ? DictFind(g, dict, kw, kl) : 0;
            if (Guest::objp(msg)) g.string(g.ptr(msg + kCsmsgName), r.name, sizeof r.name, true);
            if (sp) {
                const uint64_t l2 = g.ptr(row + spoff);
                if (spoff == kCbRowBp) r.init = g.u8(row + kCbRowInit) != 0;
                bool o2 = false;
                const int32_t c = Guest::objp(l2) ? g.i32(l2 + kListSize, &o2) : 0;
                const uint64_t it = o2 ? g.ptr(l2 + kListItems) : 0;
                if (o2 && c > 0 && Guest::objp(it)) {
                    int32_t v[kMaxSpLv];
                    const int m = c < kMaxSpLv ? c : kMaxSpLv;
                    if (g.read(it + kArrItems, v, static_cast<size_t>(m) * 4)) {
                        r.nsp = m;
                        for (int k = 0; k < m; ++k) r.sp[k] = static_cast<int16_t>(v[k] < 0 ? 0 : v[k] > 30000 ? 30000 : v[k]);
                    }
                }
            }
            if (r.name[0]) ++out;
        }
        return out;
    };
    g_sk.n = fill(kPmSkillParam, g_sk.skl, kMaxSkill, true);
    g_sk.nss = fill(kPmSpecialParam, g_sk.ssk, kMaxSpecial, false);
    g_sk.nsss = fill(kPmSuperParam, g_sk.sss, kMaxSuper, false);
}
// combat skills: their names are in the battle skill messages (another dictionary)
void LoadCombatTable(const Guest& g, uint64_t pm) {
    const uint64_t bd = g.ptr(pm + kPmBattleMsg);
    if (bd == g_sk.bdict && (g_sk.ncmb > 0 || g_sk.btries >= 20)) return;
    if (bd != g_sk.bdict) { g_sk.bdict = bd; g_sk.btries = 0; g_sk.ncmb = 0; }
    ++g_sk.btries;
    const uint64_t list = Guest::objp(g.ptr(pm + kPmCombatParam)) ? g.ptr(g.ptr(pm + kPmCombatParam) + kMpList) : 0;
    bool ok = false;
    const int32_t n = Guest::objp(list) ? g.i32(list + kListSize, &ok) : 0;
    const uint64_t items = ok ? g.ptr(list + kListItems) : 0;
    int out = 0;
    for (int32_t i = 0; ok && Guest::objp(items) && i < n && i < 128 && out < kMaxCombat; ++i) {
        const uint64_t row = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
        if (!Guest::objp(row)) continue;
        SkillTable::Row& r = g_sk.cmb[out];
        r = SkillTable::Row{};
        r.id = g.i32(row + kSkRowId);
        if (r.id <= 0 || r.id >= kMaxCombat) continue;
        r.init = g.u8(row + kCbRowInit) != 0;
        uint16_t kw[40];
        const uint64_t nk = g.ptr(row + kSkRowName);
        const int kl = nk ? g.utf16(nk, kw, 40) : 0;
        const uint64_t msg = kl > 0 && kl < 40 && Guest::objp(bd) ? DictFind(g, bd, kw, kl) : 0;
        if (Guest::objp(msg)) g.string(g.ptr(msg + kCsmsgName), r.name, sizeof r.name, true);
        const uint64_t l2 = g.ptr(row + kCbRowBp);
        bool o2 = false;
        const int32_t c = Guest::objp(l2) ? g.i32(l2 + kListSize, &o2) : 0;
        const uint64_t it = o2 ? g.ptr(l2 + kListItems) : 0;
        if (o2 && c > 0 && Guest::objp(it)) {
            int32_t v[kMaxSpLv];
            const int m = c < kMaxSpLv ? c : kMaxSpLv;
            if (g.read(it + kArrItems, v, static_cast<size_t>(m) * 4)) {
                r.nsp = m;
                for (int k = 0; k < m; ++k) r.sp[k] = static_cast<int16_t>(v[k] < 0 ? 0 : v[k] > 30000 ? 30000 : v[k]);
            }
        }
        if (r.name[0]) ++out;
    }
    g_sk.ncmb = out;
}

// special arts / spells: names by BattleSkillID, the learning rows (BP)
struct ArtName { int id; int max; char name[40]; };   // max: ConstBattleSkillParameter.levelLimit +0x7C (r33)
struct LearnRow { int16_t pid, id; int16_t bp[2]; uint8_t nbp; };
struct ArtTable { uint64_t dict; int tries, nn, nl; ArtName names[160]; LearnRow learn[kMaxLearn]; };
ArtTable g_art;
void LoadArtTable(const Guest& g, uint64_t pm) {
    const uint64_t bd = g.ptr(pm + kPmBattleMsg);
    if (bd == g_art.dict && (g_art.nn > 0 || g_art.tries >= 20)) return;
    if (bd != g_art.dict) { g_art.dict = bd; g_art.tries = 0; }
    ++g_art.tries;
    g_art.nn = g_art.nl = 0;
    auto rows = [&](uint64_t field, int32_t* n) -> uint64_t {
        const uint64_t so = g.ptr(pm + field);
        const uint64_t l = Guest::objp(so) ? g.ptr(so + kMpList) : 0;
        bool ok = false;
        *n = Guest::objp(l) ? g.i32(l + kListSize, &ok) : 0;
        if (!ok) *n = 0;
        return ok ? g.ptr(l + kListItems) : 0;
    };
    int32_t n = 0;
    uint64_t it = rows(kPmBsParam, &n);
    for (int32_t i = 0; Guest::objp(it) && i < n && i < 2048 && g_art.nn < 160; ++i) {
        const uint64_t r = g.ptr(it + kArrItems + 8ull * i);
        if (!Guest::objp(r)) continue;
        ArtName& a = g_art.names[g_art.nn];
        a.id = g.i32(r + kSkRowId); a.max = g.i32(r + kBsLevelLimit); a.name[0] = 0;
        uint16_t kw[40];
        const uint64_t nk = g.ptr(r + kSkRowName);
        const int kl = nk ? g.utf16(nk, kw, 40) : 0;
        const uint64_t msg = kl > 0 && kl < 40 && Guest::objp(bd) ? DictFind(g, bd, kw, kl) : 0;
        if (Guest::objp(msg)) g.string(g.ptr(msg + kCsmsgName), a.name, sizeof a.name, true);
        if (a.name[0]) ++g_art.nn;
    }
    it = rows(kPmLearnBs, &n);
    for (int32_t i = 0; Guest::objp(it) && i < n && i < 4096 && g_art.nl < kMaxLearn; ++i) {
        const uint64_t r = g.ptr(it + kArrItems + 8ull * i);
        if (!Guest::objp(r)) continue;
        LearnRow& l = g_art.learn[g_art.nl];
        l = LearnRow{};
        l.pid = static_cast<int16_t>(g.i32(r + kLbPid)); l.id = static_cast<int16_t>(g.i32(r + kLbId));
        const uint64_t lb = g.ptr(r + kLbBp);
        bool o2 = false;
        const int32_t c = Guest::objp(lb) ? g.i32(lb + kListSize, &o2) : 0;
        int32_t v[2] = {0, 0};
        if (o2 && c > 0 && g.read(g.ptr(lb + kListItems) + kArrItems, v, (c < 2 ? c : 2) * 4)) {
            l.nbp = static_cast<uint8_t>(c < 2 ? c : 2);
            l.bp[0] = static_cast<int16_t>(v[0]); l.bp[1] = static_cast<int16_t>(v[1]);
        }
        ++g_art.nl;
    }
}
const char* ArtNameOf(int id) { for (int i = 0; i < g_art.nn; ++i) if (g_art.names[i].id == id) return g_art.names[i].name; return nullptr; }
int ArtMaxOf(int id) { for (int i = 0; i < g_art.nn; ++i) if (g_art.names[i].id == id) return g_art.names[i].max; return 0; }
const LearnRow* LearnOf(int pid, int id) { for (int i = 0; i < g_art.nl; ++i) if (g_art.learn[i].pid == pid && g_art.learn[i].id == id) return &g_art.learn[i]; return nullptr; }

// ---- missions: the table once (names lazily, per message dictionary), rewards per mission
struct Mission { int id, cat, value, rid, nrw, type, spec, super_, prev, ssp; bool tok; int rwtype[3], rwval[3], rwcount[3]; uint64_t key; char name[72]; bool named; };
struct MissionTable { uint64_t qdict, table; int n, tries; Mission m[kMaxMissions]; char token[24]; int tok_ok; char tab[4][40]; int tab_ok[4]; };
MissionTable g_ms;
// the game's GetAchievementCount cases, decoded from its code once
struct AchCase { uint8_t kind; uint16_t var; };
AchCase g_ach[72]; int g_ach_state;     // 0 not yet, 1 decoded, -1 unreadable
int g_ach_known;
void DecodeAchievement(const Instance& in, const Guest& g) {
    if (g_ach_state) return;
    uint16_t tab[71];
    if (!g.read(in.main_base + kAchTable, tab, sizeof tab)) { g_ach_state = -1; return; }
    g_ach_known = 0;
    for (int t = 1; t <= 71; ++t) {
        AchCase& c = g_ach[t];
        c = AchCase{};
        const uint64_t at = kAchBase + 4ull * tab[t - 1];
        uint32_t code[20];
        if (!g.read(in.main_base + at, code, sizeof code)) continue;
        int imm = -1;
        for (int i = 0; i < 20; ++i) {
            const uint32_t w = code[i];
            if ((w & 0xFFE0001Fu) == 0x52800001u) { imm = static_cast<int>((w >> 5) & 0xFFFF); continue; }   // mov w1, #imm
            if ((w & 0x7C000000u) != 0x14000000u) { if (w == 0xD65F03C0u) break; continue; }                // b / bl only
            int32_t off = static_cast<int32_t>(w << 6) >> 6;
            const uint64_t to = at + 4ull * i + static_cast<int64_t>(off) * 4;
            const bool link = (w & 0x80000000u) != 0;
            if (to == 0x1AC590) continue;                     // the class init check
            if (to == kAchVarTail && imm >= 0) { c.kind = kAchVar; c.var = static_cast<uint16_t>(imm); }
            else if (to == kFnMaxSpecLv) c.kind = link ? kAchSpecLearned : kAchSpecLv;
            else if (to == kFnSpecExec) c.kind = kAchSpecExec;
            else if (to == kFnMaxCharLv) c.kind = kAchMaxCharLv;
            if (!link || c.kind) break;
            break;                                            // another method: not one the module knows
        }
        g_ach_known += c.kind != kAchUnknown;
    }
    static int tries;
    g_ach_state = g_ach_known > 0 ? 1 : ++tries > 50 ? -1 : 0;   // none known: the code isn't there yet
}
// a mission the way the game sees it (MissionUtility.IsAchieved, GetAchievementCount,
// GetTargetAchivementCount): false when the module can't tell; *counted when there is a count / target
// to show. IsAchieved's own rules (r27): type 19 value >= variable 0x2D (a best time), 70 variable
// 0x38 > 0, 1 / 6 the specialty learned; 26, 45-49, 57, 58 their own helpers (r34); 41 / 50 not followed; the rest
// value <= GetAchievementCount.
bool MissionState(const Instance& in, const Mission& m, int* count, int* target, bool* achieved, bool* counted) {
    *counted = false;
    if (g_ach_state != 1 || m.type < 1 || m.type > 71) return false;
    static_assert(sizeof kTargetTable == 70, "GetTargetAchivementCount covers types 1..70");
    auto var = [&](int id, int* v) { if (id < 1 || id >= in.nvar) return false; *v = in.var_int[id]; return true; };
    int v = 0;
    switch (m.type) {
    // the game's counter for these is GetAchievementCount = IsAchieved ? 1 : 0 against a target of 1
    case 19: if (!var(0x2D, &v)) return false; *achieved = m.value >= v; *count = *achieved; *target = 1; *counted = true; return true;
    case 70: if (!var(0x38, &v)) return false; *achieved = v > 0; *count = *achieved; *target = 1; *counted = true; return true;
    case 41: case 50: return false;   // the duel rank / arts master rules: not followed
    case 9: case 30: case 31: case 32: case 38: case 66:      // r35: counts read with the flags
    case 29: case 33: case 39: case 43: {                    // 0.9.61 (r54) chests, Private Actions, places, challenges
        v = m.type == 9 ? in.c_music : m.type == 30 ? in.c_item : m.type == 31 ? in.c_enemy
          : m.type == 32 ? in.c_quest : m.type == 38 ? in.c_form : m.type == 66 ? in.c_fish
          : m.type == 29 ? in.c_box : m.type == 33 ? in.c_pa : m.type == 39 ? in.c_loc : in.c_chal;
        if (v < 0) return false;
        *achieved = m.value <= v;
        if (m.value >= 1) { *count = v; *target = m.value; *counted = true; }
        return true;
    }
    case 26: {   // IsCompleteDaemonMelody: variable flags 1..N, N the daemon melody table's size
        if (in.nvar_flag <= 0 || in.n_melody <= 0) return false;
        bool all = true;
        for (int i = 1; i <= in.n_melody; ++i) all = all && Bit(in.var_flag, in.nvar_flag, i);
        *achieved = all; *count = all; *target = 1; *counted = true; return true;
    }
    case 34: case 35: {
        // 0.9.61 (r54) GetLearnedSkillRate / GetLearnedCombatSkillRate: the best joined member's
        // CalcLearned*Rate = (the table's rows they have at a level above 0) x 100 / the rows
        if (in.rows <= 0 || in.joining_len <= 0) return false;
        const int n = m.type == 34 ? g_sk.n : g_sk.ncmb;
        if (n <= 0) return false;
        int best = 0;
        for (int j = 0; j < in.rows; ++j) {
            const Member& r = in.m[j];
            if (!(r.on && r.pid > 0 && r.pid < in.joining_len && in.joining[r.pid] == 1)) continue;
            int c = 0;
            for (int i = 0; i < n; ++i) {
                const int id = m.type == 34 ? g_sk.skl[i].id : g_sk.cmb[i].id;
                c += m.type == 34 ? (id >= 0 && id < r.nskl && r.skl[id] > 0) : (id >= 0 && id < r.ncmb && r.cmb[id] > 0);
            }
            const int rate = c * 100 / n;
            if (rate > best) best = rate;
        }
        *achieved = m.value <= best;
        if (m.value >= 1) { *count = best; *target = m.value; *counted = true; }
        return true;
    }
    case 45: case 46: case 47: case 48: case 49: {
        // r34: the most any joined (not guest) party member has: skills (45, 46) / combat skills (47, 48)
        // at a level, or arts at their top (49)
        auto joined = [&](const Member& r) { return r.on && r.pid > 0 && r.pid < in.joining_len && in.joining[r.pid] == 1; };
        auto most = [&](int kind, int lv) {
            int best = 0;
            for (int j = 0; j < in.rows; ++j) {
                const Member& r = in.m[j];
                if (!joined(r)) continue;
                int c = 0;
                if (kind == 0) { for (int i = 0; i < g_sk.n; ++i) { const int id = g_sk.skl[i].id; c += id >= 0 && id < r.nskl && r.skl[id] >= lv; } }
                else if (kind == 1) { for (int i = 0; i < g_sk.ncmb; ++i) { const int id = g_sk.cmb[i].id; c += id >= 0 && id < r.ncmb && r.cmb[id] >= lv; } }
                else { for (int t = 0; t < r.nart; ++t) { const int mx = ArtMaxOf(r.art_id[t]); c += mx > 0 && r.art_lv[t] >= mx; } }
                if (c > best) best = c;
            }
            return best;
        };
        if (in.rows <= 0 || in.joining_len <= 0) return false;
        int c = 0, t = 0;
        switch (m.type) {
        case 45: c = most(0, m.value); t = 15; *achieved = c > 14; break;
        case 46: if (in.gd_max_skill <= 0 || in.n_skill_rows <= 0) return false;
                 *achieved = most(0, in.gd_max_skill) >= in.n_skill_rows; *count = *achieved; *target = 1; *counted = true; return true;
        case 47: if (in.mu_combat_target <= 0) return false;
                 c = most(1, m.value); t = in.mu_combat_target; *achieved = c >= t; break;
        case 48: if (in.gd_max_combat <= 0 || in.n_combat_rows <= 0) return false;
                 c = most(1, m.value); t = in.n_combat_rows; *achieved = most(1, in.gd_max_combat) >= in.n_combat_rows; break;
        default: c = most(2, 0); t = m.value; *achieved = c >= t; break;
        }
        if (t >= 1) { *count = c; *target = t; *counted = true; }
        return true;
    }
    case 57: case 58: {   // IsCategoryComplete(1 / 2): every other mission of that category cleared or achieved
        const int cat = m.type == 57 ? 1 : 2;
        for (int i = 0; i < g_ms.n; ++i) {
            const Mission& o = g_ms.m[i];
            if (o.cat != cat || o.type == 57 || o.type == 58 || Bit(in.mis_clr, 8, o.id)) continue;
            int a, b; bool ach = false, cn;
            if (!MissionState(in, o, &a, &b, &ach, &cn)) return false;
            if (!ach) { *achieved = false; return true; }
        }
        *achieved = true; return true;
    }
    default: break;
    }
    const int tt = m.type <= 70 ? kTargetTable[m.type - 1] : 0;
    int tg = tt == 0 ? m.value : tt == 1 ? 1 : tt == 0x0A ? 15 : -1;
    const AchCase& c = g_ach[m.type];
    auto maxspec = [&](int id) { int best = 0; for (int j = 0; j < in.rows; ++j) if (in.m[j].on && id < in.m[j].nssk && in.m[j].ssk[id] > best) best = in.m[j].ssk[id]; return best; };
    auto super_lv = [&](int id) { return id >= 1 && id < in.nsss ? in.sss_lv[id] : 0; };
    switch (c.kind) {
    case kAchVar: if (!var(c.var, &v)) return false; break;
    case kAchSpecLv: v = m.spec ? maxspec(m.spec) : super_lv(m.super_); break;
    case kAchSpecLearned: v = (m.spec ? maxspec(m.spec) : super_lv(m.super_)) > 0; tg = 1; break;
    case kAchSpecExec:
        if (m.spec) { if (m.spec >= in.nspec_exec) return false; v = in.spec_exec[m.spec]; }
        else { if (m.super_ < 1 || m.super_ >= in.nsuper_exec) return false; v = in.super_exec[m.super_]; }
        break;
    case kAchMaxCharLv: for (int j = 0; j < in.rows; ++j) if (in.m[j].on && in.m[j].level > v) v = in.m[j].level; break;
    default: return false;
    }
    if (m.type == 1 || m.type == 6) tg = 1;   // IsAchieved: learned
    *achieved = m.type == 1 || m.type == 6 || c.kind == kAchSpecLearned ? v >= 1 : m.value <= v;
    if (tg >= 1) { *count = v < 0 ? 0 : v; *target = tg; *counted = true; }   // every counted kind, 0 / 1 too (the game's)
    return true;
}
// a UTF-16 message -> UTF-8, the game's token replaced by `value`, other tags dropped
void MissionText(const Guest& g, uint64_t s, int value, const uint16_t* tok, int tl, char* out, int n) {
    out[0] = 0;
    uint16_t w[96];
    const int len = g.utf16(s, w, 96);
    if (len <= 0) return;
    int k = 0;
    auto put = [&](uint32_t c) {
        const int need = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
        if (k + need >= n) return;
        if (need == 1) out[k++] = static_cast<char>(c);
        else if (need == 2) { out[k++] = static_cast<char>(0xC0 | (c >> 6)); out[k++] = static_cast<char>(0x80 | (c & 0x3F)); }
        else { out[k++] = static_cast<char>(0xE0 | (c >> 12)); out[k++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out[k++] = static_cast<char>(0x80 | (c & 0x3F)); }
    };
    auto num = [&](int v) { char d[12]; int m = 0; if (v < 0) { put('-'); v = -v; } do { d[m++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && m < 11); while (m) put(static_cast<uint8_t>(d[--m])); };
    for (int i = 0; i < len; ++i) {
        if (tl > 0 && i + tl <= len) {
            bool same = true;
            for (int j = 0; j < tl && same; ++j) same = w[i + j] == tok[j];
            if (same) { num(value); i += tl - 1; continue; }
        }
        if (w[i] == '<') {             // a tag: dropped (the game's own tags, ReplaceGameTag)
            int j = i + 1; while (j < len && w[j] != '>') ++j;
            i = j; continue;
        }
        if (w[i] >= 0xD800 && w[i] <= 0xDFFF) continue;
        put(w[i]);
    }
    out[k] = 0;
}
// The element row's words, as the game picks them: ToSystemMessageID's message IDs, then the words.
struct ElementWords { uint64_t dict; char key[kElTypes][24]; char text[kElTypes][40]; int ok[kElTypes]; char q[24]; int q_ok; };
ElementWords g_elw{};

void LoadElementWords(const Instance& in, const Guest& g, uint64_t pm) {
    const uint64_t dict = g.ptr(pm + kPmSysMsg);
    if (dict != g_elw.dict) { g_elw.dict = dict; for (int t = 0; t < kElTypes; ++t) g_elw.ok[t] = 0; g_elw.q_ok = 0; }
    if (!Guest::objp(dict)) return;
    if (!g_elw.q_ok) {   // the game's "????" (GetQuestionMesasage)
        const uint64_t ks = g.ptr(g.ptr(in.main_base + kQuestionKeyRef));
        uint16_t kw[24];
        const int kl = Guest::objp(ks) ? g.utf16(ks, kw, 24) : 0;
        const uint64_t v = kl > 0 && kl < 24 ? DictFind(g, dict, kw, kl) : 0;
        if (g.is(v, "ConstSystemMessage") && g.string(g.ptr(v + kCsmMessage), g_elw.q, sizeof g_elw.q, true) && g_elw.q[0]) g_elw.q_ok = 1;
    }
    for (int t = 0; t < kElTypes; ++t) {
        if (g_elw.ok[t]) continue;
        const uint64_t ks = g.ptr(g.ptr(in.main_base + kElWordKeys + 8 * static_cast<uint64_t>(t)));
        uint16_t kw[24];
        const int kl = Guest::objp(ks) ? g.utf16(ks, kw, 24) : 0;
        if (kl <= 0 || kl >= 24) continue;
        g.string(ks, g_elw.key[t], sizeof g_elw.key[t]);
        const uint64_t v = DictFind(g, dict, kw, kl);
        if (g.is(v, "ConstSystemMessage") && g.string(g.ptr(v + kCsmMessage), g_elw.text[t], sizeof g_elw.text[t], true) && g_elw.text[t][0]) g_elw.ok[t] = 1;
    }
}

const char* ElementWord(const Instance& in, int type) {
    if (type < 0 || type >= kElTypes) type = 0;
    return g_elw.ok[type] ? g_elw.text[type] : Word(in, FindWord("so2r.t.el0") + type);
}

// The element's own picture (the game's battle_icon_element_*), by ElementID; none for the rest.
const char* ElementIcon(int id) {
    switch (id) {
        case 1: return "module:pic/battle_icon_element_earth";
        case 2: return "module:pic/battle_icon_element_water";
        case 3: return "module:pic/battle_icon_element_fire";
        case 4: return "module:pic/battle_icon_element_wind";
        case 8: return "module:pic/battle_icon_element_lightning";   // LIGHT: the green star (r32: GetElementSprite
                                                                      // has six sprites, for IDs 1-4, 8, 9)
        case 9: return "module:pic/battle_icon_element_dark";
        default: return "";
    }
}

void LoadMissions(const Instance& in, const Guest& g, uint64_t pm) {
    const uint64_t sp = g.ptr(pm + kPmMissionParam);
    const uint64_t list = Guest::objp(sp) ? g.ptr(sp + kMpList) : 0;
    const uint64_t qd = g.ptr(pm + kPmQuestMsg);
    if (list != g_ms.table) {               // the table: once
        g_ms.table = list; g_ms.n = 0; g_ms.qdict = 0;
        bool ok = false;
        const int32_t n = Guest::objp(list) ? g.i32(list + kListSize, &ok) : 0;
        const uint64_t items = ok ? g.ptr(list + kListItems) : 0;
        for (int32_t i = 0; ok && Guest::objp(items) && i < n && g_ms.n < kMaxMissions; ++i) {
            const uint64_t row = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
            if (!Guest::objp(row)) continue;
            Mission& m = g_ms.m[g_ms.n];
            m = Mission{};
            m.id = g.i32(row + kMsId); m.cat = g.i32(row + kMsCat); m.value = g.i32(row + kMsValue);
            m.type = g.i32(row + kMsType); m.spec = g.i32(row + kMsSpec); m.super_ = g.i32(row + kMsSuper);
            m.prev = g.i32(row + kMsPrev); m.ssp = g.i32(row + kMsStart);
            m.key = g.ptr(row + kMsMsg);
            const int rid = g.i32(row + kMsReward);
            if (m.id <= 0 || m.id >= kMaxMissions) continue;
            m.rid = rid;
            ++g_ms.n;
        }
        {   // the rewards: the reward table once, each row to the missions with its rewardID
            const uint64_t rp = g.ptr(pm + kPmRewardParam);
            const uint64_t rl = Guest::objp(rp) ? g.ptr(rp + kMpList) : 0;
            bool o2 = false;
            const int32_t rn = Guest::objp(rl) ? g.i32(rl + kListSize, &o2) : 0;
            const uint64_t ri = o2 ? g.ptr(rl + kListItems) : 0;
            static_assert(kRwType == kRwId + 4 && kRwValue == kRwId + 8 && kRwCount == kRwId + 12, "reward row layout");
            for (int32_t j = 0; o2 && Guest::objp(ri) && j < rn && j < 4096; ++j) {
                const uint64_t r = g.ptr(ri + kArrItems + static_cast<uint64_t>(j) * 8);
                int32_t rec[4];
                if (!Guest::objp(r) || !g.read(r + kRwId, rec, sizeof rec)) continue;
                for (int i = 0; i < g_ms.n; ++i) {
                    Mission& m = g_ms.m[i];
                    if (m.rid != rec[0] || m.nrw >= 3) continue;
                    m.rwtype[m.nrw] = rec[1]; m.rwval[m.nrw] = rec[2]; m.rwcount[m.nrw] = rec[3];
                    ++m.nrw;
                }
            }
        }
    }
    if (!g_ms.tok_ok) {                     // the token GetMissionMessage replaces (once the game has used it)
        const uint64_t ts = g.ptr(g.ptr(in.main_base + kMisTokenRef));
        if (Guest::objp(ts) && g.string(ts, g_ms.token, sizeof g_ms.token) && g_ms.token[0]) { g_ms.tok_ok = 1; g_ms.tries = 0; }
    }
    for (int t = 0; t < 4; ++t) {           // the tabs' words: the game's message IDs (string literals)
        if (g_ms.tab_ok[t]) continue;
        const uint64_t ks = g.ptr(g.ptr(in.main_base + kMisTabKeys + 8 * static_cast<uint64_t>(t)));
        uint16_t kw[24];
        const int kl = Guest::objp(ks) ? g.utf16(ks, kw, 24) : 0;
        const uint64_t v = kl > 0 && kl < 24 ? DictFind(g, g.ptr(pm + kPmSysMsg), kw, kl) : 0;
        if (g.is(v, "ConstSystemMessage") && g.string(g.ptr(v + kCsmMessage), g_ms.tab[t], sizeof g_ms.tab[t], true) && g_ms.tab[t][0]) g_ms.tab_ok[t] = 1;
    }
    if (qd != g_ms.qdict) { g_ms.qdict = qd; g_ms.tries = 0; for (int i = 0; i < g_ms.n; ++i) g_ms.m[i].named = false; }
    if (!Guest::objp(qd) || g_ms.tries > 600) return;
    ++g_ms.tries;
    uint16_t tok[24]; int tl = 0;
    if (g_ms.tok_ok) for (; g_ms.token[tl] && tl < 23; ++tl) tok[tl] = static_cast<uint8_t>(g_ms.token[tl]);
    for (int i = 0; i < g_ms.n; ++i) {
        Mission& m = g_ms.m[i];
        if (m.named && (m.tok || !g_ms.tok_ok)) continue;   // done, or done again once the token is known
        uint16_t kw[40];
        const int kl = m.key ? g.utf16(m.key, kw, 40) : 0;
        const uint64_t msg = kl > 0 && kl < 40 ? DictFind(g, qd, kw, kl) : 0;
        if (!Guest::objp(msg)) continue;
        MissionText(g, g.ptr(msg + kCqmName), m.value, tok, tl, m.name, sizeof m.name);
        m.named = true; m.tok = tl > 0;
    }
}
// the Guild Missions (quests): received, not cleared; names by QUEST_nnn (the questMessageID form, r25)
struct QuestName { int id; char name[64]; };
QuestName g_qn[16]; int g_nqn; uint64_t g_qn_dict;
const char* QuestNameOf(const Guest& g, uint64_t qd, int id) {
    if (qd != g_qn_dict) { g_qn_dict = qd; g_nqn = 0; }
    for (int i = 0; i < g_nqn; ++i) if (g_qn[i].id == id) return g_qn[i].name;
    char key[12] = "QUEST_000";
    key[6] = static_cast<char>('0' + id / 100 % 10); key[7] = static_cast<char>('0' + id / 10 % 10); key[8] = static_cast<char>('0' + id % 10);
    uint16_t kw[12]; for (int i = 0; i < 9; ++i) kw[i] = static_cast<uint8_t>(key[i]);
    const uint64_t msg = Guest::objp(qd) ? DictFind(g, qd, kw, 9) : 0;
    if (!Guest::objp(msg)) return nullptr;
    QuestName& q = g_qn[g_nqn < 16 ? g_nqn++ : id % 16];
    q.id = id;
    g.string(g.ptr(msg + kCqmName), q.name, sizeof q.name, true);
    return q.name;
}

// 0.9.81 two texts the same when their letters and digits are (any case; other characters skipped)
bool LooseEq(const char* a, const char* b) {
    auto an = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    auto lo = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    for (;;) {
        while (*a && !an(*a)) ++a;
        while (*b && !an(*b)) ++b;
        if (!*a || !*b) return !*a && !*b;
        if (lo(*a) != lo(*b)) return false;
        ++a; ++b;
    }
}

void LoadSysWords(Instance& in, const Guest& g, uint64_t pm) {
    SysWords& w = in.words;
    const uint64_t dict = g.ptr(pm + kPmSysMsg);
    if (dict != w.dict) { memset(&w, 0, sizeof w); w.dict = dict; }
    if (w.nfound == kSysWordCount || (w.age++ % 30) != 0) return;   // all cached, or retry every ~3 s
    if (!g.is(dict, "Dictionary`2")) return;
    const uint64_t entries = g.ptr(dict + kDictEntries);
    bool ok = false;
    const int32_t count = g.i32(dict + kDictCount, &ok);
    if (!ok || count <= 0 || count > 8192 || !Guest::objp(entries)) return;
    for (int32_t e = 0; e < count && w.nfound < kSysWordCount; ++e) {
        const uint64_t ea = entries + kArrItems + static_cast<uint64_t>(e) * kEntrySize;
        uint8_t rec[kEntrySize];
        if (!g.read(ea, rec, sizeof rec)) break;
        int32_t hash; memcpy(&hash, rec, 4);
        if (hash < 0) continue;
        uint64_t key, val; memcpy(&key, rec + kEntryKey, 8); memcpy(&val, rec + kEntryValue, 8);
        uint16_t kw[24];
        const int kl = g.utf16(key, kw, 24);
        if (kl < 8 || kl > 20 || !((kw[0] == 'S' && kw[1] == 'Y' && kw[2] == 'S') || kw[0] == 'B' ||   // 0.9.81 any B... key
                                    (kw[0] == 'C' && kw[1] == 'O' && kw[2] == 'M'))) continue;      // 0.9.84 COMMAND_000N
        char vt[64]; bool vread = false;      // 0.9.60 the message's text, read once if a "=" word wants it
        for (int i = 0; i < kSysWordCount; ++i) {
            if (w.found[i]) continue;
            const char* want = kSysWords[i].key;
            bool same = true;
            if (want[0] == '~') {        // 0.9.81 letters and digits only, any case
                if (!vread) {
                    vread = true; vt[0] = 0;
                    if (g.is(val, "ConstSystemMessage")) g.string(g.ptr(val + kCsmMessage), vt, sizeof vt, true);
                }
                same = vt[0] && LooseEq(vt, want + 1);
                if (same) { int k = 0; for (; k < kl && k < 23; ++k) w.key[i][k] = static_cast<char>(kw[k]); w.key[i][k] = 0; }
            } else if (want[0] == '=') {
                if (!vread) {
                    vread = true; vt[0] = 0;
                    if (g.is(val, "ConstSystemMessage")) g.string(g.ptr(val + kCsmMessage), vt, sizeof vt, true);
                }
                same = vt[0] && eq(vt, want + 1);
                if (same) { int k = 0; for (; k < kl && k < 23; ++k) w.key[i][k] = static_cast<char>(kw[k]); w.key[i][k] = 0; }
            } else {
                int k = 0;
                for (; k < kl && same; ++k) same = kw[k] == static_cast<uint8_t>(want[k]);
                same = same && want[kl] == 0;
            }
            if (!same || !g.is(val, "ConstSystemMessage")) continue;
            char t[64];
            g.string(g.ptr(val + kCsmMessage), t, sizeof t, true);
            if (!t[0]) continue;
            memcpy(w.text[i], t, sizeof t);
            w.found[i] = true;
            ++w.nfound;
        }
    }
}

// the game's current scene (SceneType), or -1 when it cannot be read
int ReadScene(Instance& in, const Guest& g) {
    in.scene_cls[0] = 0;
    const uint64_t slot = g.ptr(in.main_base + kSceneRef);
    const uint64_t mi = g.ptr(slot);
    const uint64_t k = Guest::objp(mi) ? g.ptr(mi + kMiKlass) : 0;
    if (!Guest::objp(k) || !(g.u8(k + kKlassInited) & 1)) return -1;
    const uint64_t rg = g.ptr(k + kKlassRgctx);
    const uint64_t c = Guest::objp(rg) ? g.ptr(rg) : 0;
    if (!Guest::objp(c) || !(g.u8(c + kKlassInited) & 1)) return -1;
    const uint64_t st = g.ptr(c + kClassStatics);
    const uint64_t gsm = Guest::objp(st) ? g.ptr(st) : 0;
    if (!g.is(gsm, "GameSceneManager")) return -1;
    g.class_name(g.ptr(gsm + kGsmCurrentScene), in.scene_cls, sizeof in.scene_cls);
    bool ok = false;
    const int v = g.i32(gsm + kGsmScene, &ok);
    return ok && v >= 0 && v < 64 ? v : -1;
}
bool Playing(int scene) { return scene >= 4 && scene <= 7; }   // Fieldmap, Worldmap, Battle, Camp
// the scene object agrees: not the title, opening, logo or boot scene
bool PlayingScene(const Instance& in) {
    if (!Playing(in.scene)) return false;
    const char* c = in.scene_cls;
    return !(starts(c, "Title") || starts(c, "Logo") || starts(c, "Boot") || starts(c, "Opening"));
}

// one full read of the party; on failure leaves the previous good values but clears ok
void ReadParty(Instance& in, const EdenDsmodHostApi* h) {
    Guest g{h};
    ++in.reads;
    in.ok = 0;
    in.scene = ReadScene(in, g);
    in.states_n = 0; in.states_len = -1;
#if SO2R_DIAG
    in.klass = in.klass_ref = in.slot_via_ref = in.inst = in.user = in.party = 0;
    in.klass_name[0] = in.inst_name[0] = 0;
    in.hero = in.over = 0; in.ids_len = in.over_len = in.chars_len = -1;
    in.nrej = 0;
    in.delta = h->get_i64 ? h->get_i64(h->userdata, "__relocation_delta", 0) : 0;
    in.slot_via_ref = g.ptr(in.main_base + kParamMgrRef);
    in.klass_ref = g.ptr(in.slot_via_ref);
#endif
    const uint64_t klass = g.ptr(in.main_base + kParamMgrTypeInfo);
#if SO2R_DIAG
    in.klass = klass;
    g.cstr(klass, in.klass_name, sizeof in.klass_name);
#endif
    if (!Guest::objp(klass)) { in.why = "waiting for the game (class not loaded)"; return; }
    const uint64_t statics = g.ptr(klass + kClassStatics);
    const uint64_t pm = Guest::objp(statics) ? g.ptr(statics) : 0;
    in.pm = pm;
#if SO2R_DIAG
    in.inst = pm;
    g.class_name(pm, in.inst_name, sizeof in.inst_name);
#endif
    if (!g.is(pm, "ParameterManager")) { in.why = "waiting for the game (no ParameterManager)"; return; }
    const uint64_t up = g.ptr(pm + kPmUser);
    {   // 0.9.24 the Guide Map's "Next objective", as GameManager.GetCurrentDestinationMessage picks it (r44):
        // the first destinationParameter row (pm+0x718, its list) whose heroID (+0x10) is the hero
        // (UserParameter +0x10) and whose [startScenarioProgress +0x20, endScenarioProgress +0x24) holds the
        // scenario progress (UserParameter +0xD0; a negative bound is open); its messageID (+0x18) through
        // destinationMessageDictionary (pm+0x3B0, message +0x18). None: no objective.
        static int last_row = -2;
        bool okh = false, okp = false;
        const int32_t hero = Guest::objp(up) ? g.i32(up + kUpHero, &okh) : 0;
        const int32_t prog = Guest::objp(up) ? g.i32(up + kUpScenario, &okp) : 0;
        const uint64_t so = g.ptr(pm + kPmDestParam);
        const uint64_t l = Guest::objp(so) ? g.ptr(so + kMpList) : 0;
        bool okn = false;
        const int32_t n = Guest::objp(l) ? g.i32(l + kListSize, &okn) : 0;
        const uint64_t items = okn ? g.ptr(l + kListItems) : 0;
        int row = -1;
        uint64_t hit = 0;
        if (okh && okp && okn && n > 0 && n < 4096 && Guest::objp(items))
            for (int i = 0; i < n; ++i) {
                const uint64_t r = g.ptr(items + kArrItems + 8 * static_cast<uint64_t>(i));
                if (!Guest::objp(r) || g.i32(r + 0x10) != hero) continue;
                const int32_t a = g.i32(r + 0x20), b = g.i32(r + 0x24);
                if (a >= 0 && prog < a) continue;
                if (b >= 0 && prog >= b) continue;
                row = i; hit = r;
                break;
            }
        in.obj_prog = okp ? prog : -1; in.obj_row = row; in.save_key = up; in.obj_hero = okh ? hero : -1;
        if (row != last_row || !in.obj[0]) {
            in.obj[0] = 0;
            uint16_t kw[40];
            const uint64_t nk = hit ? g.ptr(hit + 0x18) : 0;
            const int kl = nk ? g.utf16(nk, kw, 40) : 0;
            const uint64_t dict = g.ptr(pm + kPmDestMsg);
            const uint64_t msg = kl > 0 && kl < 40 && Guest::objp(dict) ? DictFind(g, dict, kw, kl) : 0;
            if (Guest::objp(msg)) g.string(g.ptr(msg + 0x18), in.obj, sizeof in.obj, true);
            last_row = in.obj[0] ? row : -2;
        }
    }
    {   // friendship: the 14 x 14 table
        const uint64_t em = Guest::objp(up) ? g.ptr(up + kUpEmotion) : 0;
        const uint64_t fa = Guest::objp(em) ? g.ptr(em + kEmFriend) : 0;
        bool okf = false;
        const int32_t fl = Guest::objp(fa) ? g.i32(fa + kArrLen, &okf) : 0;
        if (!okf || fl < 14 * 14 || !g.read(fa + kArrItems, in.friend_, sizeof in.friend_)) memset(in.friend_, 0, sizeof in.friend_);
    }
#if SO2R_DIAG
    in.user = up;
#endif
    if (!g.is(up, "UserParameter")) { in.why = "waiting for the game (no UserParameter)"; return; }
    bool ok_money = false;
    const int32_t money = g.i32(up + kUpMoney, &ok_money);
    const uint64_t pp = g.ptr(up + kUpParty);
    const uint64_t chars = g.ptr(up + kUpChars);
#if SO2R_DIAG
    in.party = pp;
    in.hero = g.i32(up + kUpHero);
#endif
    if (!ok_money || money < 0) { in.why = "money unreadable"; return; }
    if (!g.is(pp, "PartyParameter")) { in.why = "waiting for the game (no PartyParameter)"; return; }
    LoadNames(in, g, pm);
    LoadSysWords(in, g, pm);
    LoadSkillTable(g, pm);
    LoadCombatTable(g, pm);
    LoadArtTable(g, pm);
    LoadMissions(in, g, pm);
    LoadElementWords(in, g, pm);
    {   // the saved flags: combat skills released, missions released / cleared, quests received / cleared
        const int a = BitWords(g, g.ptr(up + kUpCombatReleased), in.cmb_rel, 2);
        BitWords(g, g.ptr(up + kUpMisCleared), in.mis_clr, 8);
        const int b = BitWords(g, g.ptr(up + kUpMisReleased), in.mis_rel, 8);
        BitWords(g, g.ptr(up + kUpQuestReceived), in.q_rcv, 4);
        BitWords(g, g.ptr(up + kUpQuestCleared), in.q_clr, 4);
        in.flags_ok = a > 0 && b > 0;
        int len = -1;
        in.nvar = g.ints(g.ptr(up + kUpVarInt), in.var_int, 160, &len);
        in.nvar_flag = BitWords(g, g.ptr(up + kUpVarFlag), in.var_flag, 16);
        {   // r35: the counts behind mission types 9, 30, 31, 32, 38, 66 (every ~2 s; tables of a few hundred rows)
            static int every;
            static int keep[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
            if (every-- <= 0) {
                every = 60;
                auto rows = [&](uint64_t field, uint64_t* items) -> int {
                    const uint64_t so = g.ptr(pm + field);
                    const uint64_t l = Guest::objp(so) ? g.ptr(so + kMpList) : 0;
                    bool ok = false;
                    const int32_t n = Guest::objp(l) ? g.i32(l + kListSize, &ok) : -1;
                    *items = Guest::objp(l) ? g.ptr(l + kListItems) : 0;
                    return ok && n > 0 && n < 4096 && Guest::objp(*items) ? n : -1;
                };
                static uint32_t bits[256];
                auto row_id = [&](uint64_t items, int i) { return g.i32(g.ptr(items + kArrItems + 8 * static_cast<uint64_t>(i)) + 0x10); };
                // 9: UserParameter musicComposeFlag (+0x160) bits 1..16
                int nb = BitWords(g, g.ptr(up + kUpMusicFlag), bits, 2);
                if (nb > 0) { int c = 0; for (int i = 1; i <= 16; ++i) c += Bit(bits, nb, i); keep[0] = c; }
                // 38: formationFlag (+0x120) bits 1..14
                nb = BitWords(g, g.ptr(up + kUpFormationFlag), bits, 2);
                if (nb > 0) { int c = 0; for (int i = 1; i <= 14; ++i) c += Bit(bits, nb, i); keep[4] = c; }
                uint64_t items = 0;
                // 30: the item book's rows whose obtainedItemFlag (+0xA8) is set, in %
                int n = rows(kPmItemBook, &items);
                nb = BitWords(g, g.ptr(up + kUpObtainedItem), bits, 128);
                if (n > 0 && nb > 0) { int c = 0; for (int i = 0; i < n; ++i) c += Bit(bits, nb, row_id(items, i)); keep[1] = c * 100 / n; }
                // 31: the enemy book's rows defeated (flag +0x98 and a defeat count +0x1B8 above 0), in %
                n = rows(kPmEnemyBook, &items);
                nb = BitWords(g, g.ptr(up + kUpEnemyFlag), bits, 128);
                const uint64_t dc = g.ptr(up + kUpEnemyDefeats);
                if (n > 0 && nb > 0 && Guest::objp(dc)) {
                    bool okl = false; const int32_t dl = g.i32(dc + kArrLen, &okl);
                    int c = 0;
                    for (int i = 0; i < n; ++i) {
                        const int id = row_id(items, i);
                        c += Bit(bits, nb, id) && okl && id >= 0 && id < dl && g.i32(dc + kArrItems + 4 * static_cast<uint64_t>(id)) > 0;
                    }
                    keep[2] = c * 100 / n;
                }
                // 32: the quest list's rows cleared (clearedQuestFlag +0xC0)
                n = rows(kPmQuestList, &items);
                nb = BitWords(g, g.ptr(up + kUpQuestCleared), bits, 128);
                if (n > 0 && nb > 0) { int c = 0; for (int i = 0; i < n; ++i) c += Bit(bits, nb, row_id(items, i)); keep[3] = c; }
                // 66: the fish book's fish caught (+0x1F8, by row) at or past their record size (the fishing
                // fish row's +0x2C; the player's best +0x190, by row)
                n = rows(kPmFishBook, &items);
                const uint64_t caught = g.ptr(up + kUpFishCaught), rec = g.ptr(up + kUpFishRecord);
                uint64_t fitems = 0;
                const uint64_t fso = g.ptr(pm + kPmFishingFish);
                const uint64_t fl = Guest::objp(fso) ? g.ptr(fso + kMpList) : 0;
                bool okf = false;
                const int32_t fn = Guest::objp(fl) ? g.i32(fl + kListSize, &okf) : -1;
                fitems = Guest::objp(fl) ? g.ptr(fl + kListItems) : 0;
                if (n > 0 && Guest::objp(caught) && Guest::objp(rec) && okf && fn > 0 && fn < 4096 && Guest::objp(fitems)) {
                    bool o1 = false, o2 = false;
                    const int32_t cl = g.i32(caught + kArrLen, &o1), rl = g.i32(rec + kArrLen, &o2);
                    int c = 0;
                    for (int i = 0; i < n && o1 && o2; ++i) {
                        if (i >= cl || i >= rl || g.i32(caught + kArrItems + 4 * static_cast<uint64_t>(i)) < 1) continue;
                        const int id = row_id(items, i);
                        for (int j = 0; j < fn; ++j) {
                            const uint64_t fr = g.ptr(fitems + kArrItems + 8 * static_cast<uint64_t>(j));
                            if (g.i32(fr + 0x10) != id) continue;
                            if (g.f32(fr + kFishRecordSize) <= g.f32(rec + kArrItems + 4 * static_cast<uint64_t>(i))) ++c;
                            break;
                        }
                    }
                    keep[5] = c;
                }
                {   // 0.9.61 (r53 / r54): the game counts these against its own tables, row by row:
                    // 33 Private Actions: GetPrivateActionEventGroup... rows (+0x10) set in confirmedPrivateActionFlag
                    // (+0x90); 29 chests: GetTreasureBoxParameterList rows' flag (+0x24), each flag once, set in
                    // GetTreasureBoxFlag's BitFlag; 39 location points: list +0x578 rows released (+0x78); 43
                    // challenge battles: list +0x418 rows cleared (+0x1A0). Where a getter keeps its table is read
                    // from the getter's own code (its "ldr xN, [x19|x0, #off]").
                    auto field_of = [&](uint64_t rva, int base_reg) -> uint64_t {
                        uint32_t code[24];
                        if (!g.read(in.main_base + rva, code, sizeof code)) return 0;
                        for (uint32_t w : code)
                            if ((w & 0xFFC00000u) == 0xF9400000u && static_cast<int>((w >> 5) & 31) == base_reg) {
                                const uint64_t off = ((w >> 10) & 0xFFF) * 8ull;
                                if (off >= 0x40 && off < 0x1000) return off;
                            }
                        return 0;
                    };
                    auto count_rows = [&](uint64_t pm_field, uint64_t up_flag, uint64_t id_off, bool distinct) -> int {
                        if (!pm_field || !up_flag) return -1;
                        uint64_t items = 0;
                        const int n = rows(pm_field, &items);
                        const int nb = BitWords(g, g.ptr(up + up_flag), bits, 256);
                        if (n <= 0 || nb <= 0) return -1;
                        static uint32_t seen[256];
                        if (distinct) memset(seen, 0, sizeof seen);
                        int c = 0;
                        for (int i = 0; i < n; ++i) {
                            const int id = g.i32(g.ptr(items + kArrItems + 8 * static_cast<uint64_t>(i)) + id_off);
                            if (distinct) { if (id < 0 || id >= 8192 || (seen[id >> 5] >> (id & 31) & 1)) continue; seen[id >> 5] |= 1u << (id & 31); }
                            c += Bit(bits, nb, id);
                        }
                        return c;
                    };
                    keep[6] = count_rows(field_of(kFnPaGroupList, 19), kUpPaConfirmed, 0x10, false);
                    keep[7] = count_rows(field_of(kFnBoxList, 19), field_of(kFnBoxFlag, 0), 0x24, true);
                    keep[8] = count_rows(kPmLocation, kUpLocReleased, 0x10, false);
                    keep[9] = count_rows(kPmChallenge, kUpChalCleared, 0x10, false);
                }
            }
            in.c_pa = keep[6]; in.c_box = keep[7]; in.c_loc = keep[8]; in.c_chal = keep[9];
            in.c_music = keep[0]; in.c_item = keep[1]; in.c_enemy = keep[2];
            in.c_quest = keep[3]; in.c_form = keep[4]; in.c_fish = keep[5];
        }
        {   // r34: the tables a few missions count against, and MissionUtility's target
            auto list_n = [&](uint64_t field) {
                const uint64_t so = g.ptr(pm + field);
                const uint64_t l = Guest::objp(so) ? g.ptr(so + kMpList) : 0;
                bool ok = false;
                const int32_t n = Guest::objp(l) ? g.i32(l + kListSize, &ok) : 0;
                return ok && n >= 0 && n < 4096 ? n : 0;
            };
            in.n_skill_rows = list_n(kPmSkillParam);
            in.n_combat_rows = list_n(kPmCombatParam);
            in.n_melody = list_n(kPmMelody);
            const uint64_t mk = g.ptr(g.ptr(in.main_base + kMissionUtilityRef));
            const uint64_t ms = Guest::objp(mk) ? g.ptr(mk + kClassStatics) : 0;
            const int t = Guest::objp(ms) ? g.i32(ms) : 0;
            in.mu_combat_target = t > 0 && t < 1000 ? t : 0;
        }
        in.nspec_exec = g.ints(g.ptr(up + kUpSpecExec), in.spec_exec, kMaxSpecial, &len);
        in.nsuper_exec = g.ints(g.ptr(up + kUpSuperExec), in.super_exec, kMaxSuper, &len);
        DecodeAchievement(in, g);
    }

    // the game's GetPartyMembers rule
    const bool over = g.u8(pp + kPpOver) != 0;
    int32_t ids[kMaxIds]; int ids_len = -1;
    int n_ids = g.ints(g.ptr(pp + (over ? kPpOverIds : kPpIds)), ids, kMaxIds, &ids_len);
    bool ok_chars = false;
    const int32_t chars_len = Guest::objp(chars) ? g.i32(chars + kArrLen, &ok_chars) : -1;
    const int leader = g.i32(pp + kPpLeader);
    int32_t joining[kMaxIds]; int joining_len = -1;
    const int joining_n = g.ints(g.ptr(pp + kPpJoining), joining, kMaxIds, &joining_len);
    memcpy(in.joining, joining, sizeof joining); in.joining_len = joining_len;
    {   // party-wide: super specialty levels; the game's IC list
        int32_t t[kMaxSuper]; int len = -1;
        in.nsss = g.ints(g.ptr(up + kUpSuperLv), t, kMaxSuper, &len);
        for (int k = 0; k < in.nsss; ++k) in.sss_lv[k] = t[k];
        in.ic_known = 0; in.ic_n = 0;
        for (int k = 0; k < kMaxSpecial; ++k) in.is_ic[k] = false;
        const uint64_t uk = g.ptr(g.ptr(in.main_base + kIcListRef));
        const uint64_t st = Guest::objp(uk) ? g.ptr(uk + kClassStatics) : 0;
        const uint64_t lo = Guest::objp(st) ? g.ptr(st + kIcListStatic) : 0;
        {   // the element row's order (r30)
            int32_t e[kElMax]; int el = -1;
            const uint64_t ea = Guest::objp(st) ? g.ptr(st + kElOrderStatic) : 0;
            const int ne = Guest::objp(ea) ? g.ints(ea, e, kElMax, &el) : 0;
            bool sane = ne > 0 && el == ne;
            for (int k = 0; k < ne; ++k) sane = sane && e[k] > 0 && e[k] < 12;
            in.nel = sane ? ne : 0;
            // the order the game's screens draw them in (Status / Equipment: Fire, Water, Wind, Earth,
            // Light, Dark -- the presenters' fixed icons); elements the game lists but these don't, after
            static constexpr int kScreenOrder[6] = {3, 2, 4, 1, 8, 9};
            int nk = 0; bool used[kElMax] = {};
            for (int id : kScreenOrder)
                for (int k = 0; k < in.nel; ++k) if (!used[k] && e[k] == id) { used[k] = true; in.el_order[nk++] = id; }
            for (int k = 0; k < in.nel; ++k) if (!used[k]) in.el_order[nk++] = e[k];
        }
#if SO2R_DIAG
        in.ic_cls[0] = in.ic_obj[0] = 0;
        if (Guest::objp(uk)) g.cstr(uk, in.ic_cls, sizeof in.ic_cls);
        if (Guest::objp(lo)) g.class_name(lo, in.ic_obj, sizeof in.ic_obj);
#endif
        if (Guest::objp(lo)) {
            uint64_t arr = lo; int32_t cnt = -1;
            if (g.is(lo, "List`1")) { arr = g.ptr(lo + kListItems); bool ok = false; cnt = g.i32(lo + kListSize, &ok); if (!ok) cnt = -1; }
            else { bool ok = false; cnt = g.i32(lo + kArrLen, &ok); if (!ok) cnt = -1; }
            if (Guest::objp(arr) && cnt > 0 && cnt <= kMaxSpecial) {
                int32_t ids[kMaxSpecial];
                if (g.read(arr + kArrItems, ids, static_cast<size_t>(cnt) * 4)) {
                    bool sane_ids = true;
                    for (int k = 0; k < cnt; ++k) sane_ids = sane_ids && ids[k] > 0 && ids[k] < kMaxSpecial;
                    if (sane_ids) { for (int k = 0; k < cnt; ++k) in.is_ic[ids[k]] = true; in.ic_known = 1; in.ic_n = cnt; }
                }
            }
        }
    }
#if SO2R_DIAG
    in.over = over ? 1 : 0;
    { int l; g.ints(g.ptr(pp + kPpIds), in.ids, kMaxIds, &l); in.ids_len = l; }
    { int l; g.ints(g.ptr(pp + kPpOverIds), in.over_ids, kMaxIds, &l); in.over_len = l; }
    in.chars_len = ok_chars ? chars_len : -1;
#endif
    {   // 0.9.17 partyMemberStates (PartyMemberState: Invalid 0, Battle 1, Reserve 2) for the Map page's battle members
        int l = -1;
        in.states_n = g.ints(g.ptr(pp + kPpStates), in.states, kMaxIds, &l);
        in.states_len = l;
    }
    if (ids_len < 0) { in.why = "party list unreadable"; return; }
    if (!ok_chars || chars_len <= 0 || chars_len > 64) { in.why = "character list unreadable"; return; }

    Member out[kRows];
    memset(out, 0, sizeof out);
    int rows = 0;
    for (int i = 0; i < n_ids && rows < kRows; ++i) {
        const int pid = ids[i];
        if (pid <= 0 || pid >= kMaxPlayer) continue;      // INVALID / empty slot
        if (pid >= chars_len) { ++in.rejects; continue; }  // the game's own bounds check
        const uint64_t cp = g.ptr(chars + kArrItems + static_cast<uint64_t>(pid) * 8);
        uint8_t b[kCpBlockSize];
        Member r; memset(&r, 0, sizeof r);
        bool good = g.is(cp, "CharacterParameter") && g.read(cp + kCpBlock, b, sizeof b);
        if (good) {
            r.pid = pid;
            r.cid = rd32(b, kCpId);
            r.level = rd32(b, kCpLevel);
            r.exp = rd32(b, kCpExp);
            r.hpmax = rd32(b, kCpHpMax);
            r.hp = rd32(b, kCpHp);
            r.mpmax = rd32(b, kCpMpMax);
            r.mp = rd32(b, kCpMp);
            r.leader = pid == leader ? 1 : 0;
            r.guest = pid < joining_n && joining[pid] == 2 ? 1 : 0;
            {   uint32_t cf[2] = {0, 0};
                r.food_known = BitWords(g, g.ptr(cp + kCpCharaFlag), cf, 2) > 0 && Bit(cf, 2, kFlagFoodKnown); }
            {   // skills and specialties (r22 / r23)
                int32_t t[kMaxSkill]; int len = -1;
                const int ns = g.ints(g.ptr(cp + kCpSkillLv), t, kMaxSkill, &len);
                r.nskl = ns;
                for (int k = 0; k < ns; ++k) r.skl[k] = static_cast<int8_t>(t[k] < -1 ? -1 : t[k] > 100 ? 100 : t[k]);
                const int nsp = g.ints(g.ptr(cp + kCpSpecialLv), t, kMaxSpecial, &len);
                r.nssk = nsp;
                for (int k = 0; k < nsp; ++k) r.ssk[k] = static_cast<int8_t>(t[k] < -1 ? -1 : t[k] > 100 ? 100 : t[k]);
                r.sp = g.i32(cp + kCpSkillPoint);
                const int nc = g.ints(g.ptr(cp + kCpCombatLv), t, kMaxCombat, &len);
                r.ncmb = nc;
                for (int k = 0; k < nc; ++k) r.cmb[k] = static_cast<int8_t>(t[k] < -1 ? -1 : t[k] > 100 ? 100 : t[k]);
                r.bp = g.i32(cp + kCpCombatPoint);
                {   // element resistances (r28: CharacterParameter +0xC8, base +0xC0; ElementResistanceType by ElementID)
                    int32_t e[12]; int el = -1;
                    r.nelem = g.ints(g.ptr(cp + 0xC8), e, 12, &el);
                    for (int k = 0; k < r.nelem; ++k) r.elem[k] = static_cast<int8_t>(e[k]);
                    const int nb = g.ints(g.ptr(cp + 0xC0), e, 12, &el);
                    for (int k = 0; k < 12; ++k) r.elem_base[k] = static_cast<int8_t>(k < nb ? e[k] : -99);
                }
                {   // the arts set on the L / R buttons
                    int32_t eq[4]; int el = -1;
                    const int ne = g.ints(g.ptr(cp + kCpEquipBs), eq, 4, &el);
                    r.art_eq[0] = ne > 0 ? eq[0] : 0; r.art_eq[1] = ne > 1 ? eq[1] : 0;
                }
                {   // special arts / spells learned
                    const uint64_t la = g.ptr(cp + kCpLearnBs);
                    bool o = false;
                    const int32_t cnt = Guest::objp(la) ? g.i32(la + kArrLen, &o) : 0;
                    r.nart = 0;
                    for (int32_t k = 0; o && k < cnt && k < 64 && r.nart < kMaxArts; ++k) {
                        int32_t e[2];
                        if (!g.read(la + kArrItems + 12ull * k, e, sizeof e) || e[0] <= 0 || e[0] > 30000) continue;
                        r.art_id[r.nart] = static_cast<int16_t>(e[0]);
                        r.art_lv[r.nart] = static_cast<int16_t>(e[1] < 0 ? 0 : e[1] > 999 ? 999 : e[1]);
                        ++r.nart;
                    }
                }
            }
            for (int k = 0; k < 9; ++k) r.stat[k] = rd32(b, kCpStat[k]);
            r.level_base = rd32(b, kCpLevelBase);
            r.talent = rd32(b, kCpTalent);
            for (int k = 0; k < kEquipSlots; ++k) {
                r.equip[k] = rd32(b, kCpEquip + 4 * static_cast<uint64_t>(k));
                r.equip_item[k] = ItemOfUnique(g, up, r.equip[k]);
            }
            {
                uint64_t bf; memcpy(&bf, b + (0x30 - kCpBlock), 8);
                const uint64_t arr = Guest::objp(bf) ? g.ptr(bf + 0x10) : 0;
                bool okb = false;
                const int32_t len = Guest::objp(arr) ? g.i32(arr + kArrLen, &okb) : 0;
                r.bd = okb && len > 0 ? static_cast<uint32_t>(g.i32(arr + kArrItems)) : 0;
            }
            uint64_t sp; memcpy(&sp, b + (kCpName - kCpBlock), 8);
            g.string(sp, r.name, kNameMax, true);
            if (!r.name[0]) memcpy(r.name, in.names.name[pid], kNameMax);
            good = sane(r);
        }
        if (!good) {
            ++in.rejects;
#if SO2R_DIAG
            if (in.nrej < kMaxIds) in.rejected_pid[in.nrej++] = pid;
#endif
            continue;
        }
        r.on = 1;
        r.slot = i;
        out[rows++] = r;
    }
    if (!rows) { in.why = "no party members yet"; return; }
    {   // 0.9.19 the battle members, as the game decides (r40: PartyParameter.GetBattleMembers / IsBattleMember):
        // the first maxBattleMember (4) entries of the party list (playerIDs, or overridePlayerIDs while
        // isOverrideBattleMember); the rest are the reserve
        in.battle_how = 3;
        for (int k = 0; k < rows; ++k) out[k].battle = out[k].slot < kMaxBattle;
    }
    if (!PlayingScene(in)) { in.why = in.scene == 3 || starts(in.scene_cls, "Title") ? "title screen" : "not in play (scene)"; return; }
    memcpy(in.m, out, sizeof out);
    in.rows = rows;
    in.fol = money;
    in.leader = leader;
    in.ok = 1;
    in.why = "ok";
}

// ================================================================ game files
// ---------------------------------------------------------------- the index
struct IdxBundle { int id; char path[96]; uint64_t size, stream; };
// 0.9.46 sig_off / sig_hash / full_hash: a texture's fingerprint (numbers only -- no picture bytes): the rolling hash
// (RollHash) of the 32-byte window at sig_off in its data, and the FNV-1a 64 of all of it. With them the module finds
// the texture in a bundle the game's update replaced (another size: 1.0.1's uiscene) instead of refusing it.
struct IdxTex { int id, bundle; uint64_t off, size; uint32_t w, h, fmt, gobs; uint64_t sig_off, sig_hash, full_hash; };
struct IdxSprite { char name[48]; int tex; uint32_t x, y, w, h, rot;
                   uint32_t fw, fh, ox, oy; };   // 0.9.16 'P': the sprite's full rect and where the trimmed picture sits in it
struct IdxFont {
    bool ok; int bundle, tex;
    uint64_t obj, g_at, gn, g_st, c_at, cn, c_st;
    int32_t point, ascent100, line100, descent100, padding;
    uint64_t sig_off, sig_hash;    // 0.9.101 'N': the object's fingerprint (found again in a moved bundle)
    int64_t reloc;                 // -1 not searched, -2 not found, else where the object starts
};
constexpr int kIdxBundles = 320, kIdxTex = 400, kIdxSprites = 1024;
int64_t g_reloc[kIdxTex];             // 0.9.46 per index texture in a moved bundle: -1 not searched, -2 not found, else its offset
struct Index {
    bool loaded; uint32_t tried; int err;
    int nb, nt, ns;
    IdxBundle b[kIdxBundles];
    IdxTex t[kIdxTex];
    IdxSprite s[kIdxSprites];
    IdxFont f;
    IdxFont nf;                    // 0.9.101 the number font (Aldrich-Regular SDF, the bonus gauge's numbers)
};
Index g_ix;
char g_ixbuf[96 * 1024];

uint64_t ParseU(const char*& p) {
    while (*p == ' ') ++p;
    bool neg = *p == '-'; if (neg) ++p;
    uint64_t v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + static_cast<uint64_t>(*p++ - '0');
    return neg ? 0 - v : v;
}
void ParseWord(const char*& p, char* out, size_t cap) {
    while (*p == ' ') ++p;
    size_t k = 0;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r') { if (k + 1 < cap) out[k++] = *p; ++p; }
    out[k] = 0;
}

// under g_file_lock
bool IndexLoad(const EdenDsmodHostApi* h) {
    Index& ix = g_ix;
    if (ix.loaded) return true;
    if (ix.tried++ % 64) return false;               // missing: look again now and then
    if (!h->read_romfs) { ix.err = 1; return false; }
    size_t n = h->read_romfs(h->userdata, "file:pictures.idx", 0, g_ixbuf, sizeof g_ixbuf - 1);
    if (n < 8 || n >= sizeof g_ixbuf - 1) { ix.err = 2; return false; }
    g_ixbuf[n] = 0;
    if (!starts(g_ixbuf, "SO2P 1")) { ix.err = 3; return false; }
    ix.nb = ix.nt = ix.ns = 0; ix.f.ok = false;
    const char* p = g_ixbuf;
    while (*p) {
        const char k = p[0];
        if (p[1] == ' ') {
            const char* q = p + 2;
            if (k == 'B' && ix.nb < kIdxBundles) {
                IdxBundle& b = ix.b[ix.nb++];
                b.id = static_cast<int>(ParseU(q)); ParseWord(q, b.path, sizeof b.path);
                b.size = ParseU(q); b.stream = ParseU(q);
            } else if (k == 'T' && ix.nt < kIdxTex) {
                IdxTex& t = ix.t[ix.nt++];
                t.id = static_cast<int>(ParseU(q)); t.bundle = static_cast<int>(ParseU(q));
                t.off = ParseU(q); t.size = ParseU(q);
                t.w = static_cast<uint32_t>(ParseU(q)); t.h = static_cast<uint32_t>(ParseU(q));
                t.fmt = static_cast<uint32_t>(ParseU(q)); t.gobs = static_cast<uint32_t>(ParseU(q));
                t.sig_off = ParseU(q); t.sig_hash = ParseU(q); t.full_hash = ParseU(q);   // 0.9.46 (0: none)
            } else if (k == 'S' && ix.ns < kIdxSprites) {
                IdxSprite& s = ix.s[ix.ns++];
                ParseWord(q, s.name, sizeof s.name);
                s.tex = static_cast<int>(ParseU(q));
                s.x = static_cast<uint32_t>(ParseU(q)); s.y = static_cast<uint32_t>(ParseU(q));
                s.w = static_cast<uint32_t>(ParseU(q)); s.h = static_cast<uint32_t>(ParseU(q));
                s.rot = static_cast<uint32_t>(ParseU(q));
                s.fw = s.fh = s.ox = s.oy = 0;
            } else if (k == 'P') {                    // 0.9.16: full rect w h, trimmed offset x y (from the bottom left)
                char nm[48]; ParseWord(q, nm, sizeof nm);
                for (int i = 0; i < ix.ns; ++i)
                    if (eq(ix.s[i].name, nm)) {
                        IdxSprite& s = ix.s[i];
                        s.fw = static_cast<uint32_t>(ParseU(q)); s.fh = static_cast<uint32_t>(ParseU(q));
                        s.ox = static_cast<uint32_t>(ParseU(q)); s.oy = static_cast<uint32_t>(ParseU(q));
                        break;
                    }
            } else if ((k == 'F' && !ix.f.ok) || (k == 'N' && !ix.nf.ok)) {
                char nm[48]; ParseWord(q, nm, sizeof nm);
                IdxFont& f = k == 'N' ? ix.nf : ix.f;
                f.bundle = static_cast<int>(ParseU(q)); f.obj = ParseU(q);
                f.g_at = ParseU(q); f.gn = ParseU(q); f.g_st = ParseU(q);
                f.c_at = ParseU(q); f.cn = ParseU(q); f.c_st = ParseU(q);
                f.tex = static_cast<int>(ParseU(q));
                f.point = static_cast<int32_t>(ParseU(q)); f.ascent100 = static_cast<int32_t>(ParseU(q));
                f.line100 = static_cast<int32_t>(ParseU(q)); f.descent100 = static_cast<int32_t>(ParseU(q));
                f.padding = static_cast<int32_t>(ParseU(q));
                f.sig_off = ParseU(q); f.sig_hash = ParseU(q); f.reloc = -1;
                f.ok = f.gn > 0 && f.cn > 0 && f.g_st >= 40 && f.c_st >= 12;
            }
        }
        while (*p && *p != '\n') ++p;
        if (*p) ++p;
    }
    ix.loaded = ix.nb > 0 && ix.nt > 0;
    for (auto& r : g_reloc) r = -1;
    ix.err = ix.loaded ? 0 : 4;
    return ix.loaded;
}
const IdxBundle* FindBundle(int id) {
    for (int i = 0; i < g_ix.nb; ++i) if (g_ix.b[i].id == id) return &g_ix.b[i];
    return nullptr;
}
const IdxTex* FindTex(int id) {
    for (int i = 0; i < g_ix.nt; ++i) if (g_ix.t[i].id == id) return &g_ix.t[i];
    return nullptr;
}
const IdxSprite* FindSprite(const char* name) {
    for (int i = 0; i < g_ix.ns; ++i) if (eq(g_ix.s[i].name, name)) return &g_ix.s[i];
    return nullptr;
}

// ---------------------------------------------------------------- UnityFS bundles
// LZ4 block format (as Unity writes its bundle blocks). False on any malformed input.
bool Lz4(const uint8_t* s, size_t n, uint8_t* d, size_t want) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t t = s[i++];
        size_t lit = t >> 4;
        if (lit == 15) { uint8_t b; do { if (i >= n) return false; b = s[i++]; lit += b; } while (b == 255); }
        if (i + lit > n || o + lit > want) return false;
        memcpy(d + o, s + i, lit); i += lit; o += lit;
        if (i >= n) break;
        if (i + 2 > n) return false;
        size_t off = s[i] | (static_cast<size_t>(s[i + 1]) << 8); i += 2;
        size_t ml = t & 15;
        if (ml == 15) { uint8_t b; do { if (i >= n) return false; b = s[i++]; ml += b; } while (b == 255); }
        ml += 4;
        if (off == 0 || off > o || o + ml > want) return false;
        for (size_t k = 0; k < ml; ++k, ++o) d[o] = d[o - off];
    }
    return o == want;
}
uint32_t Be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
uint64_t Be64(const uint8_t* p) { return (uint64_t{Be32(p)} << 32) | Be32(p + 4); }

constexpr uint32_t kMaxBlocks = 2048, kMaxChunk = 512 * 1024, kMaxInfo = 192 * 1024, kSlots = 4;
struct OpenBundle {
    bool ok; int id; int err; uint32_t use;
    char path[96];
    uint64_t dev_size, idx_size;   // 0.9.44 err 10: the size the device reports (0: not opened) and the index's
    bool moved;                    // 0.9.46 opened at another size: only fingerprinted textures, found by search
    uint32_t n;
    uint32_t us[kMaxBlocks], cs[kMaxBlocks]; uint16_t fl[kMaxBlocks];
    uint64_t ustart[kMaxBlocks + 1], fstart[kMaxBlocks];
    uint32_t nn;                   // 0.9.104 its files (nodes): where each starts in the stream, its name's last part
    uint64_t node_off[64];
    char node_name[64][48];
};
constexpr int kOpenSlots = 8;           // bundles kept open at once (least recently used goes)
OpenBundle g_ob[kOpenSlots];
uint32_t g_ob_use;
uint8_t g_info[kMaxInfo], g_infoz[kMaxInfo];
struct Slot { int bundle; uint32_t block; uint32_t use; uint32_t n; uint8_t data[kMaxChunk]; };
Slot g_slot[kSlots];
uint8_t g_raw[kMaxChunk];
uint32_t g_use;

// under g_file_lock
OpenBundle* Open(const EdenDsmodHostApi* h, int id) {
    for (auto& o : g_ob) if (o.id == id && (o.ok || o.err)) { o.use = ++g_ob_use; return o.ok ? &o : nullptr; }
    const IdxBundle* ib = FindBundle(id);
    OpenBundle* ob = &g_ob[0];
    for (auto& o : g_ob) {
        if (!o.ok && !o.err) { ob = &o; break; }
        if (o.use < ob->use) ob = &o;
    }
    if (!ib) return nullptr;
    memset(ob, 0, sizeof *ob);
    ob->id = id; ob->use = ++g_ob_use;
    memcpy(ob->path, ib->path, sizeof ob->path);
    const uint64_t fsize = h->read_romfs(h->userdata, ob->path, 0, nullptr, 0);
    if (fsize != ib->size) {                                              // another game version
        ob->dev_size = fsize; ob->idx_size = ib->size;
        // 0.9.46: still opened when the index has fingerprints for its textures (they are found by search); a file
        // the host could not open, or one with none: refused as before
        bool sig = false;
        for (int i = 0; i < g_ix.nt && !sig; ++i) sig = g_ix.t[i].bundle == id && g_ix.t[i].sig_hash;
        if (!fsize || !sig) { ob->err = 10; return nullptr; }
        ob->moved = true;
    }
    uint8_t hdr[256];
    if (h->read_romfs(h->userdata, ob->path, 0, hdr, sizeof hdr) != sizeof hdr) { ob->err = 11; return nullptr; }
    if (!starts(reinterpret_cast<const char*>(hdr), "UnityFS")) { ob->err = 12; return nullptr; }
    size_t i = 8;
    const uint32_t ver = Be32(hdr + i); i += 4;
    while (i < 200 && hdr[i]) ++i; ++i;            // player version
    while (i < 200 && hdr[i]) ++i; ++i;            // engine version
    if (i + 20 > sizeof hdr) { ob->err = 13; return nullptr; }
    i += 8;                                         // total size
    const uint32_t csize = Be32(hdr + i), usize = Be32(hdr + i + 4), flags = Be32(hdr + i + 8); i += 12;
    if (flags & 0x1400) { ob->err = 14; return nullptr; }
    if (ver >= 7) i = (i + 15) & ~size_t{15};
    if (csize > kMaxInfo || usize > kMaxInfo) { ob->err = 15; return nullptr; }
    const uint64_t info_at = (flags & 0x80) ? fsize - csize : i;
    if (h->read_romfs(h->userdata, ob->path, info_at, g_infoz, csize) != csize) { ob->err = 16; return nullptr; }
    const uint32_t comp = flags & 0x3F;
    if (comp == 0) { if (csize != usize) { ob->err = 17; return nullptr; } memcpy(g_info, g_infoz, csize); }
    else if (comp == 2 || comp == 3) { if (!Lz4(g_infoz, csize, g_info, usize)) { ob->err = 18; return nullptr; } }
    else { ob->err = 19; return nullptr; }
    size_t p = 16;
    const uint32_t nb = Be32(g_info + p); p += 4;
    if (nb == 0 || nb > kMaxBlocks || p + nb * 10 > usize) { ob->err = 20; return nullptr; }
    uint64_t data = (flags & 0x80) ? i : i + csize;
    if (flags & 0x200) data = (data + 15) & ~uint64_t{15};
    uint64_t u = 0, f = data;
    for (uint32_t k = 0; k < nb; ++k) {
        ob->us[k] = Be32(g_info + p); ob->cs[k] = Be32(g_info + p + 4);
        ob->fl[k] = static_cast<uint16_t>((g_info[p + 8] << 8) | g_info[p + 9]); p += 10;
        if (ob->us[k] > kMaxChunk || ob->cs[k] > kMaxChunk) { ob->err = 21; return nullptr; }
        ob->ustart[k] = u; ob->fstart[k] = f; u += ob->us[k]; f += ob->cs[k];
    }
    ob->ustart[nb] = u;
    ob->n = nb;
    if ((u != ib->stream && !ob->moved) || f > fsize) { ob->err = 22; return nullptr; }   // (0.9.46 a moved one has its own)
    if (p + 4 <= usize) {                           // 0.9.104 the nodes: offset, size (BE 64), flags, name
        const uint32_t nn = Be32(g_info + p); p += 4;
        for (uint32_t k = 0; k < nn && p + 20 < usize; ++k) {
            const uint64_t off = Be64(g_info + p); p += 20;
            size_t e = p; while (e < usize && g_info[e]) ++e;
            if (e >= usize) break;
            size_t b = e; while (b > p && g_info[b - 1] != '/') --b;
            if (ob->nn < 64) {
                size_t L = e - b; if (L > 47) L = 47;
                memcpy(ob->node_name[ob->nn], g_info + b, L); ob->node_name[ob->nn][L] = 0;
                ob->node_off[ob->nn++] = off;
            }
            p = e + 1;
        }
    }
    ob->ok = true;
    return ob;
}
const uint8_t* Chunk(const EdenDsmodHostApi* h, OpenBundle* ob, uint32_t k) {
    Slot* best = &g_slot[0];
    for (auto& s : g_slot) {
        if (s.use && s.bundle == ob->id && s.block == k) { s.use = ++g_use; return s.data; }
        if (s.use < best->use) best = &s;
    }
    const uint32_t c = ob->fl[k] & 0x3F;
    Slot& s = *best;
    s.use = 0;
    if (c == 0) {
        if (h->read_romfs(h->userdata, ob->path, ob->fstart[k], s.data, ob->us[k]) != ob->us[k]) return nullptr;
    } else if (c == 2 || c == 3) {
        if (h->read_romfs(h->userdata, ob->path, ob->fstart[k], g_raw, ob->cs[k]) != ob->cs[k]) return nullptr;
        if (!Lz4(g_raw, ob->cs[k], s.data, ob->us[k])) return nullptr;
    } else {
        return nullptr;
    }
    s.bundle = ob->id; s.block = k; s.n = ob->us[k]; s.use = ++g_use;
    return s.data;
}
// copy n bytes from the bundle's data stream at off
bool StreamRead(const EdenDsmodHostApi* h, OpenBundle* ob, uint64_t off, void* out, size_t n) {
    auto* o = static_cast<uint8_t*>(out);
    while (n) {
        uint32_t lo = 0, hi = ob->n;                 // block holding off
        if (off >= ob->ustart[ob->n]) return false;
        while (hi - lo > 1) { uint32_t mid = (lo + hi) / 2; if (ob->ustart[mid] <= off) lo = mid; else hi = mid; }
        const uint8_t* d = Chunk(h, ob, lo);
        if (!d) return false;
        const uint64_t at = off - ob->ustart[lo];
        size_t take = ob->us[lo] - at; if (take > n) take = n;
        memcpy(o, d + at, take);
        o += take; off += take; n -= take;
    }
    return true;
}

// 0.9.46 a texture's fingerprint (see IdxTex). The rolling hash of a 32-byte window: sum of w[i] * B^(31-i), mod 2^64.
constexpr uint64_t kRollB = 1099511628211ull;
uint64_t RollPow31() { uint64_t p = 1; for (int i = 0; i < 31; ++i) p *= kRollB; return p; }
uint64_t Fnv64(uint64_t h, const uint8_t* d, size_t n) { for (size_t i = 0; i < n; ++i) { h ^= d[i]; h *= kRollB; } return h; }
[[maybe_unused]] int g_reloc_found, g_reloc_tried;
uint8_t g_verify[64 * 1024];
// the whole data's FNV-1a 64 at off (n bytes) equals want?
bool VerifyAt(const EdenDsmodHostApi* h, OpenBundle* ob, uint64_t off, uint64_t n, uint64_t want) {
    uint64_t hv = 14695981039346656037ull;
    while (n) {
        const size_t take = n < sizeof g_verify ? static_cast<size_t>(n) : sizeof g_verify;
        if (!StreamRead(h, ob, off, g_verify, take)) return false;
        hv = Fnv64(hv, g_verify, take);
        off += take; n -= take;
    }
    return hv == want;
}
// find texture t in a moved bundle: one pass over its data stream, the window's rolling hash, then the whole hash
int64_t Relocate(const EdenDsmodHostApi* h, OpenBundle* ob, const IdxTex& t) {
    ++g_reloc_tried;
    const uint64_t p31 = RollPow31(), end = ob->ustart[ob->n];
    uint8_t ring[32] = {};
    uint64_t hv = 0, at = 0;
    for (uint32_t k = 0; k < ob->n; ++k) {
        const uint8_t* d = Chunk(h, ob, k);
        if (!d) return -2;
        for (uint32_t i = 0; i < ob->us[k]; ++i, ++at) {
            const uint8_t b = d[i];
            hv = (at >= 32 ? (hv - ring[at & 31] * p31) : hv) * kRollB + b;
            ring[at & 31] = b;
            if (at < 31 || hv != t.sig_hash) continue;
            const uint64_t win = at - 31;                // the window's first byte
            if (win < t.sig_off) continue;
            const uint64_t start = win - t.sig_off;
            if (start + t.size > end) continue;
            const bool ok = VerifyAt(h, ob, start, t.size, t.full_hash);
            if (ok) { ++g_reloc_found; return static_cast<int64_t>(start); }
            d = Chunk(h, ob, k);                         // (the check may have reused this block's slot)
            if (!d) return -2;
        }
    }
    return -2;
}

// Switch block-linear: byte column xb, row y (GOB 64 B x 8 rows, `gobs` GOBs per block)
uint64_t BlOffset(uint64_t xb, uint64_t y, uint64_t width_bytes, uint64_t gobs) {
    const uint64_t gobs_x = (width_bytes + 63) / 64, bh = 8 * gobs;
    const uint64_t in_gob = ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
    return ((y / bh) * gobs_x + xb / 64) * 512 * gobs + ((y % bh) / 8) * 512 + in_gob;
}

// ---------------------------------------------------------------- BC3
void Rgb565(uint16_t c, uint8_t* o) {
    o[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
    o[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
    o[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
}
// one 16-byte BC3 block -> 4x4 RGBA (row 0 = the block's first row in memory)
void Bc3Block(const uint8_t* b, uint8_t* px) {
    uint8_t a[8];
    a[0] = b[0]; a[1] = b[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; ++i) a[i + 1] = static_cast<uint8_t>(((7 - i) * a[0] + i * a[1]) / 7);
    else { for (int i = 1; i < 5; ++i) a[i + 1] = static_cast<uint8_t>(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
    uint64_t ai = 0; for (int i = 0; i < 6; ++i) ai |= uint64_t{b[2 + i]} << (8 * i);
    const uint16_t c0 = static_cast<uint16_t>(b[8] | (b[9] << 8)), c1 = static_cast<uint16_t>(b[10] | (b[11] << 8));
    uint8_t c[4][3];
    Rgb565(c0, c[0]); Rgb565(c1, c[1]);
    for (int k = 0; k < 3; ++k) {                 // BC3 colour is always the 4-colour mode
        c[2][k] = static_cast<uint8_t>((2 * c[0][k] + c[1][k]) / 3);
        c[3][k] = static_cast<uint8_t>((c[0][k] + 2 * c[1][k]) / 3);
    }
    const uint32_t ci = static_cast<uint32_t>(b[12]) | (static_cast<uint32_t>(b[13]) << 8) |
                        (static_cast<uint32_t>(b[14]) << 16) | (static_cast<uint32_t>(b[15]) << 24);
    for (int i = 0; i < 16; ++i) {
        const uint8_t* cc = c[(ci >> (2 * i)) & 3];
        px[i * 4] = cc[0]; px[i * 4 + 1] = cc[1]; px[i * 4 + 2] = cc[2];
        px[i * 4 + 3] = a[(ai >> (3 * i)) & 7];
    }
}

// 0.9.145 BC1 (DXT1, Unity format 10: the title screen's background): 8-byte blocks, 3 colours + transparent
// when c0 <= c1
void Bc1Block(const uint8_t* b, uint8_t* px) {
    const uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8)), c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
    uint8_t c[4][4];
    Rgb565(c0, c[0]); Rgb565(c1, c[1]);
    c[0][3] = c[1][3] = c[2][3] = c[3][3] = 255;
    for (int k = 0; k < 3; ++k) {
        if (c0 > c1) {
            c[2][k] = static_cast<uint8_t>((2 * c[0][k] + c[1][k]) / 3);
            c[3][k] = static_cast<uint8_t>((c[0][k] + 2 * c[1][k]) / 3);
        } else {
            c[2][k] = static_cast<uint8_t>((c[0][k] + c[1][k]) / 2);
            c[3][k] = 0;
        }
    }
    if (c0 <= c1) c[3][3] = 0;
    const uint32_t ci = static_cast<uint32_t>(b[4]) | (static_cast<uint32_t>(b[5]) << 8) |
                        (static_cast<uint32_t>(b[6]) << 16) | (static_cast<uint32_t>(b[7]) << 24);
    for (int i = 0; i < 16; ++i) memcpy(px + i * 4, c[(ci >> (2 * i)) & 3], 4);
}

// ---------------------------------------------------------------- pictures
// pixels per picture: the world maps (2800x2200) at full size; anything bigger is halved
constexpr uint32_t kPicMax = 2800 * 2200;
constexpr uint32_t kPicOut = 4000000;               // Eden Duo takes module pictures up to 16 MB (4M pixels)
constexpr uint32_t kPic2Max = kPicOut * 4;          // scratch: a compressed map, a rotated sprite, the map canvas
uint8_t g_pic[kPicMax * 4], g_pic2[kPic2Max];
uint8_t g_blur[2048 * 2048];                        // 0.9.23: an area picture's softened outline (alpha)
int g_pics_drawn, g_pics_failed;
[[maybe_unused]] int g_pic_err;   // diag only
EdenDsmodAstcDecoder g_astc;          // from Configure (the host's codec)
void* g_astc_user;

// texture rectangle (bottom-left origin) -> RGBA, top row first, into g_pic. Under g_file_lock.
// 0.9.113 a texture the update changed (its fingerprint gone; the button icons' sheet: picture error 35) found in
// the moved bundle by its shape: a Texture2D whose width, height, complete size and format are the index's, read in
// Unity 2021.3's field order from the width (as FindAtlas): the platform blob at +72 (blob[8..12]: log2 GOBs; none:
// plain rows), then the inline data (u32 size + pixels) or size 0 and the stream data (u64 offset, u32 size, the .resS
// path). Several of that shape: the one nearest the index's place.
int g_shape_found = -1;    // diag: textures of the index's shape seen in the last search
char g_shape_list[96];     // 0.9.114 diag: every texture seen in that bundle, "WxH/fmt" (the first few)
int64_t FindTexByShape(const EdenDsmodHostApi* h, OpenBundle* ob, IdxTex& t) {
    // 0.9.114 every Texture2D in the bundle (fields validated as above, any size and format); the index's shape
    // first, else the same width and height in a format the module decodes (BC3, ASTC 4x4, RGBA32, Alpha8)
    const uint64_t end = ob->ustart[ob->n];
    struct Cand { int64_t off; uint32_t w, h, fmt, gobs, size; };
    static Cand c[32];
    int nc = 0;
    uint64_t at = 0;
    for (uint32_t k = 0; k < ob->n; ++k) {
        const uint8_t* d = Chunk(h, ob, k);
        if (!d) return -2;
        const uint32_t n = ob->us[k];
        for (uint32_t i = 0; i + 20 <= n; ++i) {
            uint32_t W, H, F; memcpy(&W, d + i, 4); memcpy(&H, d + i + 4, 4); memcpy(&F, d + i + 16, 4);
            // 0.9.147 any width a multiple of 4 (title_background is 1920 wide), and only the wanted shape kept (the
            // 32 slots filled by other textures first in a big bundle like uiscene)
            if (W < 8 || W > 8192 || H < 8 || H > 8192 || F == 0 || F > 80 || (W & 3) != 0) continue;
            if (W != t.w || H != t.h) continue;
            const uint64_t P = at + i;
            static uint8_t b[512];
            if (P + sizeof b > end || !StreamRead(h, ob, P, b, sizeof b)) { d = Chunk(h, ob, k); if (!d) return -2; continue; }
            d = Chunk(h, ob, k);
            if (!d) return -2;
            auto u32 = [&](uint32_t o) { uint32_t v; memcpy(&v, b + o, 4); return v; };
            const uint32_t cs = u32(8), L = u32(72);
            if (!cs || cs > (64u << 20) || L > 256) continue;
            const uint32_t r = 76 + ((L + 3) & ~3u);
            if (r + 24 > sizeof b) continue;
            const int gb = L == 0 ? 0 : L >= 12 ? (u32(76 + 8) < 6 ? 1 << u32(76 + 8) : -1) : -1;
            if (gb < 0) continue;
            const uint32_t dsz = u32(r);
            int64_t off = -2; uint32_t size = 0;
            if (dsz && dsz >= cs && dsz < cs * 2 + 65536) { off = static_cast<int64_t>(P + r + 4); size = dsz; }
            else if (dsz == 0 && u32(r + 12) >= cs && u32(r + 12) < cs * 2 + 65536) {
                uint64_t so; memcpy(&so, b + r + 4, 8);
                const uint32_t pl = u32(r + 16);
                if (pl < 5 || pl > 200 || r + 20 + pl > sizeof b) continue;
                const char* path = reinterpret_cast<const char*>(b + r + 20);
                uint32_t bgn = pl; while (bgn > 0 && path[bgn - 1] != '/') --bgn;
                for (uint32_t nd = 0; nd < ob->nn && off < 0; ++nd) {
                    uint32_t m = 0; const char* nm = ob->node_name[nd];
                    while (bgn + m < pl && nm[m] == path[bgn + m]) ++m;
                    if (bgn + m == pl && !nm[m]) off = static_cast<int64_t>(ob->node_off[nd] + so);
                }
                size = u32(r + 12);
            }
            if (off < 0 || nc >= 32) continue;
            c[nc++] = {off, W, H, F, static_cast<uint32_t>(gb), size};
        }
        at += n;
    }
    {   // diag
        size_t o = 0;
        for (int i = 0; i < nc && o + 20 < sizeof g_shape_list; ++i) {
            char tmp[24]; int q = 0;
            auto put = [&](uint32_t v) { char r2[12]; int m = 0; do { r2[m++] = static_cast<char>('0' + v % 10); v /= 10; } while (v); while (m) tmp[q++] = r2[--m]; };
            put(c[i].w); tmp[q++] = 'x'; put(c[i].h); tmp[q++] = '/'; put(c[i].fmt); tmp[q++] = ' ';
            if (o + q >= sizeof g_shape_list) break;
            memcpy(g_shape_list + o, tmp, q); o += q;
        }
        g_shape_list[o] = 0;
    }
    auto pick = [&](bool exact) -> int {
        int best = -1; uint64_t bd = ~0ull;
        for (int i = 0; i < nc; ++i) {
            if (c[i].w != t.w || c[i].h != t.h) continue;
            if (exact ? c[i].fmt != t.fmt : !(c[i].fmt == 12 || c[i].fmt == 48 || c[i].fmt == 4 || c[i].fmt == 1 || c[i].fmt == 10)) continue;
            const uint64_t dn = static_cast<uint64_t>(c[i].off) > t.off ? c[i].off - t.off : t.off - c[i].off;
            if (dn < bd) { bd = dn; best = i; }
        }
        return best;
    };
    int bi = pick(true);
    g_shape_found = 0;
    for (int i = 0; i < nc; ++i) if (c[i].w == t.w && c[i].h == t.h && c[i].fmt == t.fmt) ++g_shape_found;
    if (bi < 0) bi = pick(false);
    if (bi < 0) return -2;
    t.fmt = c[bi].fmt; t.gobs = c[bi].gobs; t.size = c[bi].size;
    return c[bi].off;
}

// `step` > 1 keeps every step-th pixel (a big map drawn at 1/step size; output w/step x hgt/step).
bool DecodeRect(const EdenDsmodHostApi* h, const IdxTex& t, uint32_t x, uint32_t y, uint32_t w, uint32_t hgt,
                uint32_t step = 1, bool whole_texture = false) {
    const uint32_t ow = w / step, oh = hgt / step;
    if (!w || !hgt || !ow || !oh || static_cast<uint64_t>(ow) * oh > kPicMax || x + w > t.w || y + hgt > t.h) { g_pic_err = 30; return false; }
    OpenBundle* ob = Open(h, t.bundle);
    if (!ob) { g_pic_err = 31; return false; }
    uint64_t toff = t.off;
    if (ob->moved) {                                 // 0.9.46 a bundle the update replaced: the texture found by search
        const int ti = static_cast<int>(&t - g_ix.t);
        if (ti < 0 || ti >= g_ix.nt || !t.sig_hash) { g_pic_err = 34; return false; }
        if (g_reloc[ti] == -1) {
            g_reloc[ti] = Relocate(h, ob, t);
            if (g_reloc[ti] < 0) g_reloc[ti] = FindTexByShape(h, ob, g_ix.t[ti]);   // 0.9.113
        }
        if (g_reloc[ti] < 0) { g_pic_err = 35; return false; }
        toff = static_cast<uint64_t>(g_reloc[ti]);
    }
    if (!t.gobs && t.fmt != 4 && t.fmt != 1) { g_pic_err = 34; return false; }   // 0.9.78 plain rows: RGBA32 (0.9.105 and Alpha8)
    if (t.fmt == 12 || t.fmt == 48 || t.fmt == 10) {   // BC3, or ASTC 4x4 (the same 16-byte 4x4 blocks); 0.9.145 BC1
        const EdenDsmodAstcDecoder astc = t.fmt == 48 ? __atomic_load_n(&g_astc, __ATOMIC_ACQUIRE) : nullptr;
        if (t.fmt == 48 && !astc) { g_pic_err = 38; return false; }
        const uint32_t bs = t.fmt == 10 ? 8 : 16;    // bytes a block
        const uint64_t wb = ((t.w + 3) / 4) * bs;
        // a big picture: the whole texture in one read (g_pic2 is free until Rotate), then cut from memory
        const bool bulk = (whole_texture || t.fmt == 48 || t.fmt == 10) && t.size <= sizeof g_pic2;
        if (bulk && !StreamRead(h, ob, toff, g_pic2, t.size)) { g_pic_err = 36; return false; }
        for (uint32_t by = y / 4; by <= (y + hgt - 1) / 4; ++by) {
            for (uint32_t bx = x / 4; bx <= (x + w - 1) / 4; ++bx) {
                uint8_t blk[16], px[64];
                const uint64_t bo = BlOffset(uint64_t{bx} * bs, by, wb, t.gobs);
                if (bulk) {
                    if (bo + bs > t.size) { g_pic_err = 37; return false; }
                    memcpy(blk, g_pic2 + bo, bs);
                } else if (!StreamRead(h, ob, toff + bo, blk, bs)) { g_pic_err = 32; return false; }
                if (astc) { if (!astc(g_astc_user, 4, 4, 4, 4, blk, 16, px, 64)) { g_pic_err = 39; return false; } }
                else if (bs == 8) Bc1Block(blk, px);
                else Bc3Block(blk, px);
                for (uint32_t r = 0; r < 4; ++r) {
                    const uint32_t ty = by * 4 + r;
                    if (ty < y || ty >= y + hgt) continue;
                    const uint32_t orow = (y + hgt - 1) - ty;          // top row first
                    if (orow % step || orow / step >= oh) continue;
                    for (uint32_t q = 0; q < 4; ++q) {
                        const uint32_t tx = bx * 4 + q;
                        if (tx < x || tx >= x + w || (tx - x) % step || (tx - x) / step >= ow) continue;
                        memcpy(g_pic + (uint64_t{orow / step} * ow + (tx - x) / step) * 4, px + (r * 4 + q) * 4, 4);
                    }
                }
            }
        }
        return true;
    }
    if (t.fmt == 4) {                             // 0.9.77 RGBA32 (the pixel characters' sheets): 4 bytes a pixel,
        if (step != 1) { g_pic_err = 35; return false; }   // read 16 bytes (4 pixels, contiguous in a GOB) at a time
        const uint64_t wb = uint64_t{t.w} * 4;
        for (uint32_t ty = y; ty < y + hgt; ++ty) {
            const uint32_t orow = (y + hgt - 1) - ty;
            for (uint32_t gx = x & ~3u; gx < x + w; gx += 4) {
                uint8_t q[16];
                // 0.9.78 gobs 0: the sheet is stored in plain rows (no platform blob, research: the indexer's check)
                const uint64_t bo = t.gobs ? BlOffset(uint64_t{gx} * 4, ty, wb, t.gobs) : uint64_t{ty} * wb + uint64_t{gx} * 4;
                if (gx + 4 > t.w) { const uint32_t n = (t.w - gx) * 4; if (!StreamRead(h, ob, toff + bo, q, n)) { g_pic_err = 33; return false; } }
                else if (!StreamRead(h, ob, toff + bo, q, 16)) { g_pic_err = 33; return false; }
                for (uint32_t k = 0; k < 4; ++k) {
                    const uint32_t tx = gx + k;
                    if (tx < x || tx >= x + w) continue;
                    memcpy(g_pic + (uint64_t{orow} * w + (tx - x)) * 4, q + k * 4, 4);
                }
            }
        }
        return true;
    }
    if (t.fmt == 1) {                             // Alpha8: white, alpha = the value
        if (step != 1) { g_pic_err = 35; return false; }
        for (uint32_t ty = y; ty < y + hgt; ++ty) {
            const uint32_t orow = (y + hgt - 1) - ty;
            for (uint32_t tx = x; tx < x + w; ++tx) {
                uint8_t v;
                const uint64_t bo = t.gobs ? BlOffset(tx, ty, t.w, t.gobs) : uint64_t{ty} * t.w + tx;
                if (!StreamRead(h, ob, toff + bo, &v, 1)) { g_pic_err = 33; return false; }
                uint8_t* o = g_pic + (uint64_t{orow} * w + (tx - x)) * 4;
                o[0] = o[1] = o[2] = 255; o[3] = v;
            }
        }
        return true;
    }
    g_pic_err = 34;
    return false;
}

// Unity's packing rotation, applied after the flip (as UnityPy); returns the final size
void Rotate(uint32_t rot, uint32_t& w, uint32_t& h) {
    if (rot == 0 || rot > 4) return;
    const uint32_t W = w, H = h;
    if (uint64_t{W} * H * 4 > sizeof g_pic2) return;   // never for the game's rotated sprites (all small)
    for (uint32_t r = 0; r < H; ++r)
        for (uint32_t c = 0; c < W; ++c) {
            uint32_t nr = r, nc = c, nw = W;
            if (rot == 1) nc = W - 1 - c;
            else if (rot == 2) nr = H - 1 - r;
            else if (rot == 3) { nr = H - 1 - r; nc = W - 1 - c; }
            else { nr = W - 1 - c; nc = r; nw = H; }   // 90 degrees anticlockwise
            memcpy(g_pic2 + (uint64_t{nr} * nw + nc) * 4, g_pic + (uint64_t{r} * W + c) * 4, 4);
        }
    if (rot == 4) { w = H; h = W; }
    memcpy(g_pic, g_pic2, uint64_t{w} * h * 4);
}

// A map picture is a single-sprite texture whose sprite rectangle is the whole texture (pivot at
// its centre); the index holds only the trimmed part, so map pictures are cut as the whole texture
// -- the frame the game's minimap numbers are measured in.
// The colour the game draws an area's minimap with (read from the game in ReadMap), applied to the
// area pictures as they are cut; and the land's extent on each world map (for the whole-map view).
float g_area_tint[4];
int g_area_tint_ok;
struct LandBox { float x0, y0, x1, y1; int ok; };
LandBox g_land[2];                  // map_expel, map_nede (picture pixels, y up)
// the drawn part of the last area picture cut (its whole view is framed to it, not the empty texture)
struct AreaBox { char name[48]; LandBox b; };
AreaBox g_abox[16];                 // the last areas cut (a picture the host keeps is not cut again)
int g_abox_n, g_abox_seq;           // seq: odd while being written

// ---- the Map page's picture ("canvas"): the area / world picture turned the way the game turns it
// and padded to the Map widget's shape, so the bottom screen shows it as the game does. The page's
// view (zoom, centre) and the arrow are computed from the same geometry (MapCanvas below).
constexpr float kMapRectW = 1208, kMapRectH = 752;   // gen_manifest.py MAP_RECT (its size)
// 0.9.17 the info sidebar: shown, the map is narrower (gen_manifest.py MAP_NARROW); the map's left edge
// 0.9.18 the sidebar is the standard layout; the map sits inside its panel's lines (gen_manifest MAP_INNER)
// 0.9.22 the map's own card (gen_manifest MAP_INNER)
constexpr float kMapNarrowW = 850, kMapNarrowH = 656, kMapX = 16, kMapNarrowX = 366, kMapY = 212, kMapNarrowY = 222;
int g_side = 1;                     // 1 = the sidebar shown (the map narrow), 0 = collapsed (the map full width)
// the quick menu's buttons: 0 d-pad up, 1 left, 2 down, 3 right, 4 X, 5 Y (Switch PA); 6 = B (closing)
constexpr int kQuickPa = 5, kQuickB = 6;
struct QuickMenu {
    int want = -1;          // the button to press once the windows are closed (-1 none)
    int phase;              // 0 idle, 1 closing (B), 2 opening (want)
    int last = -1;          // the button whose menu we opened last
    int fire = -1;          // the button being pressed now (5 = B), -1 none
    int64_t ack;            // so2r.qp when the press was asked for
    uint64_t at;            // the sample it was asked at / the last press
    int presses;            // B presses this close
};
QuickMenu g_qm;
int g_qtap = -1;            // a tap, handed from OnAction to the next sample
// 0.9.60 the leader steps (LeadStep)
struct LeadMacro {
    int state;          // 0 idle, 1 close what is open, 2 Camp Menu (X), 3 wait for it, 4 wait for Party Formation,
                        // 5 the cursor to the member, 6 wait for the leader, 7 close
    int target;         // the PlayerID
    uint64_t at;        // the sample this state began / the last press
    int presses, moves;
    int result;         // 0 none, 1 done, -1 stopped
    int why;            // the step it stopped at (diag)
    int retry;          // 0.9.64 a press the game didn't take, made again (at most 3)
    int seen, before; uint64_t seen_at; bool moving;   // 0.9.65 the cursor read settled (see Settled)
};
LeadMacro g_lm;
// 0.9.65 a cursor read is trusted only once it has moved since the last press and then held still for a while:
// 0.9.64 on the Thor the read lagged the game's cursor, so the steps pressed again before it caught up and ran
// past the member (the leader) or one row too far (Database instead of Missions).
template <class M> bool Settled(M& m, int value, uint64_t now) {
    if (value != m.seen) { m.seen = value; m.seen_at = now; }
    if (m.moving) {
        if (value == m.before && now - m.at < 60) return false;   // the press hasn't shown yet (1 s at most)
        m.moving = false;
    }
    return now - m.seen_at >= 4;                                    // held 0.07 s (0.9.134; 0.1 s 0.9.66; 0.25 s 0.9.65)
}
// 0.9.64 open a Challenge Mission in the game's own Missions list (Camp Menu > Missions > the mission chosen)
struct MisMacro {
    int state;          // 0 idle, 1 close, 2 X, 3 the Camp Menu's Missions row, 4 its Missions list row (if a submenu),
                        // 5 wait for the Missions window, 6 its tab, 7 its cursor to the mission, 8 close (stopped)
    int id, tab;        // the MissionID, its tab (0..3)
    uint64_t at;
    int presses, moves, retry, result, why;
    int seen, before; uint64_t seen_at; bool moving;
    bool claim;         // 0.9.66 achieved and its reward not taken: A on it, then A on the game's CLEAR dialog's OK
    int sub;            // 0.9.122 Improve: its own menu's row (item -2)
    int item;           // 0.9.70 a Camp Menu row only (UIDefine.CampMenuItem, e.g. 3 Special Arts/Spells): A on it, done
    int rsteps;         // 0.9.133 then R this many times: the game's screen on the companion's chosen member
                        // (0.9.135 L when below 0)
    int dslot;          // 0.9.135 then Down (Up when below 0) this many times: the Equipment screen's slot
    bool eq, quick;     // 0.9.135 the Equipment screen (kept as open when done); already open (no long first wait)
    int tmem, tslot;    // 0.9.135 the member and slot it ends on
};
int g_eq_req;           // 0.9.135 an Equipment card: 1 + its slot + 8 x the chosen member
bool g_eq_live, g_eq_view;   // the Equipment screen the companion opened still up; the companion's Equipment subtab shown
int g_eq_mem, g_eq_slot;     // the member and slot the game's Equipment screen was left on
[[maybe_unused]] int g_eq_follows;   // 0.9.137 no longer counted (no follow)
int g_eq_now[2];        // 0.9.136 diag: the Camp window on top, its state (read by the macro each sample)       // 0.9.136 diag: the times the game's Equipment screen followed a pick
int g_eq_sig[4];        // how the game's windows read then (count, top type, Camp window on top, its state)
int g_imp_req;          // 0.9.122 the Skills subtab's Improve: 1 + the Improve menu's row (0 IC/Specialty Skills,
                        // 1 Combat Skill, 2 Special Arts/Spells)
bool g_imp_live;        // 0.9.123 an Improve screen the companion opened is still up (the windows not all closed)
int g_imp_cstate = -1;  // 0.9.123 the Camp Menu's state in the Improve menu (seen there)
int g_art_req;          // 0.9.70 (0.9.133: 1 + the R presses to the chosen member) the Battle page's L / R outside a battle: open the game's Special Arts/Spells
int g_lit_req;                      // 0.9.121 the tapped Battle button + 1 (see Light)
constexpr int kLitSamples = 18;
int g_btn_req = -1;     // 0.9.70 one press of a game button (L / R in a battle, X for its command menu)
MisMacro g_mm;
int g_mis_req = -1;     // a tap: the Missions page's row (-1 none)
int g_mrow_id[80];      // the Missions page's rows: their MissionID (0: not one to open)
int g_lead_req;         // a tap: the PlayerID to make leader (0 none)
int g_brow[4] = {-1, -1, -1, -1};  // the party rows on the Map page's battle member strip
int g_goparty;                      // flips on a party strip tap: the manifest's page_bind opens the Party page
constexpr float kMapMaxZoom = 16.0f;                                                  // ... "max_zoom"
constexpr float kPi = 3.14159265f;

void SinCosDeg(float deg, float& sn, float& cs) {
    int q = static_cast<int>(deg >= 0 ? deg / 90.0f + 0.5f : deg / 90.0f - 0.5f);   // nearest quarter turn
    const float r = (deg - q * 90.0f) * (kPi / 180.0f);                               // |r| <= pi/4
    const float r2 = r * r;
    const float s0 = r * (1 - r2 / 6 * (1 - r2 / 20 * (1 - r2 / 42)));
    const float c0 = 1 - r2 / 2 * (1 - r2 / 12 * (1 - r2 / 30));
    q &= 3;
    if (q == 0) { sn = s0; cs = c0; } else if (q == 1) { sn = c0; cs = -s0; }
    else if (q == 2) { sn = -s0; cs = -c0; } else { sn = -c0; cs = s0; }
}
float SqrtF(float v) {
    if (v <= 0) return 0;
    float x = v > 1 ? v : 1;
    for (int i = 0; i < 30; ++i) x = 0.5f * (x + v / x);
    return x;
}
float AbsF(float v) { return v < 0 ? -v : v; }

struct MapCanvas {
    float w, h;          // the source picture (pixels)
    float sn, cs;        // its turn, clockwise on screen
    float cw, ch;        // the canvas, in source pixels (the widget's shape)
    // a source point (pixels, y up from the bottom edge) -> 0..1 of the canvas (y down)
    void ToCanvas(float x, float yup, float& u, float& v) const {
        const float px = x - w / 2, py = (h - yup) - h / 2;      // from the centre, y down
        const float rx = px * cs - py * sn, ry = px * sn + py * cs;
        u = (rx + cw / 2) / cw; v = (ry + ch / 2) / ch;
    }
};
// pad: an area picture's canvas is twice the picture (0.9.18), so the view can centre its drawn part
MapCanvas MakeCanvas(float w, float h, float rot, bool narrow, float pad = 1) {
    MapCanvas c{};
    c.w = w; c.h = h;
    SinCosDeg(rot, c.sn, c.cs);
    const float bw = AbsF(w * c.cs) + AbsF(h * c.sn), bh = AbsF(w * c.sn) + AbsF(h * c.cs);
    const float a = narrow ? kMapNarrowW / kMapNarrowH : kMapRectW / kMapRectH;
    if (bw / bh > a) { c.cw = bw; c.ch = bw / a; } else { c.ch = bh; c.cw = bh * a; }
    c.cw *= pad; c.ch *= pad;
    return c;
}

bool MapPicture(const char* name) {
    return starts(name, "minimap_") || eq(name, "map_expel") || eq(name, "map_nede");
}

float Atan2Deg(float x, float z);
// One picture, decoded as the index cuts it, into `out` (RGBA, top row first). Under g_file_lock.
// 0.9.115 the button icons' places on the 1.0.1 sheet: the update re-laid the sheet (0.9.114 drew the right sheet but
// icons from the base game's places: R for A, ...), so the rects are read from the 1.0.1 sprite asset itself -- the
// MonoBehaviour named "text_icon_switch" (m_Name at +28) holds m_SpriteCharacterTable (u32 count; each: element type
// 2, codepoint, glyph index, scale 1.0, name (u32 length + bytes, aligned), hash) and after it m_SpriteGlyphTable
// (u32 count; each: index, metrics (5 floats), rect x y w h, scale 1.0, atlas index, class type, sprite PPtr: 64 bytes, checked). Each
// "button_<name>" sprite of the index takes its glyph's rect. Once.
int g_btn_fix = -1;        // diag: -1 not tried, else the icons placed
[[maybe_unused]] int g_btn_names, g_btn_cn, g_btn_gn, g_btn_st;   // 0.9.116 diag: names seen, characters, glyphs, glyph stride
char g_btn_diag[200];      // 0.9.116 diag: the 1.0.1 names with their rects
[[maybe_unused]] void FixButtonSprites(const EdenDsmodHostApi* h, int bundle) {
    if (g_btn_fix != -1) return;
    g_btn_fix = 0;
    OpenBundle* ob = Open(h, bundle);
    if (!ob || !ob->moved) return;
    static const char kName[] = "text_icon_switch";
    const uint32_t nl = sizeof kName - 1;
    const uint64_t end = ob->ustart[ob->n];
    uint64_t at = 0;
    static uint8_t b[24576];
    for (uint32_t k = 0; k < ob->n; ++k) {
        const uint8_t* d = Chunk(h, ob, k);
        if (!d) return;
        const uint32_t n = ob->us[k];
        for (uint32_t i = 0; i + 4 + nl <= n; ++i) {
            if (d[i] != nl || d[i + 1] || d[i + 2] || d[i + 3] || d[i + 4] != 't') continue;
            { uint32_t m = 0; while (m < nl && d[i + 4 + m] == static_cast<uint8_t>(kName[m])) ++m; if (m != nl) continue; }
            ++g_btn_names;
            const uint64_t obj = at + i;
            const uint64_t len = end - obj < sizeof b ? end - obj : sizeof b;
            if (!StreamRead(h, ob, obj, b, len)) { d = Chunk(h, ob, k); if (!d) return; continue; }
            d = Chunk(h, ob, k);
            if (!d) return;
            auto u32 = [&](uint64_t o) { uint32_t v = 0; if (o + 4 <= len) memcpy(&v, b + o, 4); return v; };
            // the character table: a count, then a first record of type 2 with scale 1.0 and a short name
            for (uint64_t q = 4 + nl; q + 64 < len; q += 4) {
                const uint32_t cn = u32(q);
                if (cn < 4 || cn > 200 || u32(q + 4) != 2 || u32(q + 16) != 0x3F800000u || u32(q + 20) == 0 || u32(q + 20) > 40) continue;
                struct Ch { char name[40]; uint32_t glyph; };
                static Ch chs[200];
                uint64_t p = q + 4; bool ok = true;
                for (uint32_t c = 0; c < cn && ok; ++c) {
                    const uint32_t type = u32(p), gi = u32(p + 8), sc = u32(p + 12), L = u32(p + 16);
                    if (type != 2 || sc != 0x3F800000u || L == 0 || L > 39 || p + 20 + L + 8 > len) { ok = false; break; }
                    memcpy(chs[c].name, b + p + 20, L); chs[c].name[L] = 0; chs[c].glyph = gi;
                    p += 20 + ((L + 3) & ~3u) + 4;                 // name aligned, then the hash
                }
                if (!ok) continue;
                // the glyph table right after (a count, 60-byte records with scale 1.0 at +40)
                for (uint64_t g = p; g + 4 + 2 * 68 <= len && g < p + 64; g += 4) {
                    const uint32_t gn = u32(g);
                    if (gn < 4 || gn > 200 || u32(g + 4 + 40) != 0x3F800000u) continue;
                    uint32_t st = 0;                               // the record's size: 64 (52 + the sprite PPtr) or near
                    static constexpr uint32_t kSt[5] = {64, 60, 68, 56, 52};
                    for (uint32_t cand : kSt)
                        if (u32(g + 4 + cand + 40) == 0x3F800000u && u32(g + 4 + cand) == u32(g + 4) + 1) { st = cand; break; }
                    if (!st) for (uint32_t cand : kSt)
                        if (u32(g + 4 + cand + 40) == 0x3F800000u) { st = cand; break; }
                    if (!st || g + 4 + uint64_t{gn} * st > len) continue;
                    int placed = 0;
                    g_btn_cn = static_cast<int>(cn); g_btn_gn = static_cast<int>(gn); g_btn_st = static_cast<int>(st);
                    {   // diag: every name with its glyph's rect
                        size_t o = 0;
                        auto put = [&](const char* z) { while (*z && o + 1 < sizeof g_btn_diag) g_btn_diag[o++] = *z++; };
                        auto num = [&](uint32_t v) { char r2[12]; int m = 0; do { r2[m++] = static_cast<char>('0' + v % 10); v /= 10; } while (v); while (m && o + 1 < sizeof g_btn_diag) g_btn_diag[o++] = r2[--m]; };
                        for (uint32_t c = 0; c < cn; ++c) {
                            const char* nm = chs[c].name;     // the ones the page uses
                            if (!eq(nm, "Circle") && !eq(nm, "Cross") && !eq(nm, "Square") && !eq(nm, "Triangle") &&
                                !eq(nm, "L1") && !eq(nm, "R1")) continue;
                            put(chs[c].name); put("@");
                            bool f = false;
                            for (uint32_t r = 0; r < gn && !f; ++r) {
                                const uint64_t rr = g + 4 + uint64_t{r} * st;
                                if (u32(rr) != chs[c].glyph) continue;
                                num(u32(rr + 24)); put(","); num(u32(rr + 28)); f = true;
                            }
                            if (!f) put("?");
                            put(" ");
                        }
                        g_btn_diag[o] = 0;
                    }
                    for (uint32_t c = 0; c < cn; ++c) {
                        for (uint32_t r = 0; r < gn; ++r) {
                            const uint64_t rr = g + 4 + uint64_t{r} * st;
                            if (u32(rr) != chs[c].glyph) continue;
                            char full[48] = "button_";
                            size_t fl = 7; for (size_t z = 0; chs[c].name[z] && fl + 1 < sizeof full; ++z) full[fl++] = chs[c].name[z];
                            full[fl] = 0;
                            for (int si = 0; si < g_ix.ns; ++si) {
                                IdxSprite& sp = g_ix.s[si];
                                if (!eq(sp.name, full)) continue;
                                sp.x = u32(rr + 24); sp.y = u32(rr + 28); sp.w = u32(rr + 32); sp.h = u32(rr + 36);
                                ++placed;
                            }
                            break;
                        }
                    }
                    g_btn_fix = placed;
                    return;
                }
            }
        }
        at += n;
    }
}
bool CutSprite(const EdenDsmodHostApi* h, const char* name, uint8_t* out, uint64_t cap, uint32_t& w, uint32_t& hh) {
    const IdxSprite* s = FindSprite(name);
    if (!s) { g_pic_err = 50; return false; }
    const IdxTex* t = FindTex(s->tex);
    if (!t) { g_pic_err = 51; return false; }
    // 0.9.119 the 1.0.1 places come from the player's matching (button_cell_*), not FixButtonSprites
    w = s->w; hh = s->h;
    if (uint64_t{w} * hh * 4 > cap) { g_pic_err = 55; return false; }
    if (!DecodeRect(h, *t, s->x, s->y, w, hh)) return false;
    Rotate(s->rot, w, hh);
    memcpy(out, g_pic, uint64_t{w} * hh * 4);
    return true;
}
void Over(uint8_t* d, const uint8_t* s) {          // s over d (straight alpha)
    const uint32_t sa = s[3], da = d[3];
    if (!sa) return;
    const uint32_t oa = sa + da * (255 - sa) / 255;
    for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((s[c] * sa + d[c] * da * (255 - sa) / 255) / (oa ? oa : 1));
    d[3] = static_cast<uint8_t>(oa);
}
uint8_t g_cut[3][200 * 120 * 4];

// "@lvarc:<0..1000>": the camp's level half circle (camp_level_gauze_background, its fill swept
// clockwise from the left like the game's 180-degree radial fill up to the next-level ratio, and
// camp_level_point on the arc at that point), padded so the point fits
bool LevelArc(const EdenDsmodHostApi* h, int permille, void* receiver, EdenDsmodImageSink sink) {
    uint32_t bw, bh, fw, fh, pw, ph;
    if (!CutSprite(h, "camp_level_gauze_background", g_cut[0], sizeof g_cut[0], bw, bh) ||
        !CutSprite(h, "camp_level_gauze_fill", g_cut[1], sizeof g_cut[1], fw, fh) ||
        !CutSprite(h, "camp_level_point", g_cut[2], sizeof g_cut[2], pw, ph) || fw != bw || fh != bh) return false;
    const uint32_t pad = pw / 2 + 1, ow = bw + 2 * pad, oh = bh + 2 * pad;   // room for the point at both ends
    uint8_t* o = g_pic2;
    memset(o, 0, uint64_t{ow} * oh * 4);
    const float ratio = (permille < 0 ? 0 : permille > 1000 ? 1000 : permille) / 1000.0f;
    const float cx = bw / 2.0f, cy = static_cast<float>(bh);         // the bottom edge's centre
    float rs = 0, rn = 0;
    for (uint32_t y = 0; y < bh; ++y)
        for (uint32_t x = 0; x < bw; ++x) {
            const uint8_t* b = g_cut[0] + (uint64_t{y} * bw + x) * 4;
            const uint8_t* f = g_cut[1] + (uint64_t{y} * bw + x) * 4;
            uint8_t* d = o + (uint64_t{y + pad} * ow + x + pad) * 4;
            memcpy(d, b, 4);
            const float dx = x + 0.5f - cx, dy = cy - (y + 0.5f);
            const float ang = Atan2Deg(dx, dy);          // 0 straight up, -90 left, +90 right
            if (f[3] > 128) { rs += SqrtF(dx * dx + dy * dy); rn += 1; }
            if ((ang + 90.0f) / 180.0f <= ratio) Over(d, f);
        }
    if (rn > 0) {                                        // the point, on the arc's middle radius
        const float r = rs / rn, a = (-90.0f + 180.0f * ratio);
        float sn, cs;
        SinCosDeg(a, sn, cs);
        const int px = static_cast<int>(cx + r * sn + pad - pw / 2.0f), py = static_cast<int>(cy - r * cs + pad - ph / 2.0f);
        for (uint32_t y = 0; y < ph; ++y)
            for (uint32_t x = 0; x < pw; ++x) {
                const int ox = px + static_cast<int>(x), oy = py + static_cast<int>(y);
                if (ox < 0 || oy < 0 || ox >= static_cast<int>(ow) || oy >= static_cast<int>(oh)) continue;
                Over(o + (uint64_t(oy) * ow + ox) * 4, g_cut[2] + (uint64_t{y} * pw + x) * 4);
            }
    }
    sink(receiver, ow, oh, o, uint64_t{ow} * oh * 4);
    return true;
}

// 0.9.75 "@ring:<0..1000>": an assault button's gauge the way the game draws it (UIBattleAssistCharacterPresenter,
// research r57): battle_assist_background with battle_assist_fill over it, the fill's Image.fillAmount set to the
// assist point's share of assistPointMax, swept clockwise from the top. Output: the fill's size, the background
// centred in it.
bool AssistRing(const EdenDsmodHostApi* h, int permille, void* receiver, EdenDsmodImageSink sink) {
    uint32_t bw, bh, fw, fh;
    if (!CutSprite(h, "battle_assist_background", g_cut[0], sizeof g_cut[0], bw, bh) ||
        !CutSprite(h, "battle_assist_fill", g_cut[1], sizeof g_cut[1], fw, fh)) return false;
    const uint32_t ow = fw > bw ? fw : bw, oh = fh > bh ? fh : bh;
    uint8_t* o = g_pic2;
    memset(o, 0, uint64_t{ow} * oh * 4);
    const uint32_t bx = (ow - bw) / 2, by = (oh - bh) / 2, fx = (ow - fw) / 2, fy = (oh - fh) / 2;
    for (uint32_t y = 0; y < bh; ++y)
        memcpy(o + (uint64_t{y + by} * ow + bx) * 4, g_cut[0] + uint64_t{y} * bw * 4, uint64_t{bw} * 4);
    const float ratio = (permille < 0 ? 0 : permille > 1000 ? 1000 : permille) / 1000.0f;
    const float cx = fw / 2.0f, cy = fh / 2.0f;
    for (uint32_t y = 0; y < fh && ratio > 0; ++y)
        for (uint32_t x = 0; x < fw; ++x) {
            const float dx = x + 0.5f - cx, dy = cy - (y + 0.5f);
            float ang = Atan2Deg(dx, dy);                  // 0 straight up, clockwise positive
            if (ang < 0) ang += 360.0f;
            if (ang / 360.0f <= ratio) Over(o + (uint64_t{y + fy} * ow + x + fx) * 4, g_cut[1] + (uint64_t{y} * fw + x) * 4);
        }
    sink(receiver, ow, oh, o, uint64_t{ow} * oh * 4);
    return true;
}

// 0.9.89 "@bonus:<0..1000>": the battle's bonus gauge the way the game draws it (UIBattleSphereBonusPresenter, r65):
// battle_bonus_gauge_outline, with battle_bonus_gauge_sphere_core inside it grown by how far the points are into
// the stage (its sphereScaleTransform); full: battle_bonus_gauge_sphere_glow behind.
uint8_t g_cut_big[2][340 * 340 * 4];
// 0.9.92 the tints from the gauge's prefab (r68, ui_battle_sphere_bonus_presenter_2 / Gauge / Scale_Gauge /
// Scale_Noise): Glow (0.8774, 0.3409, 0.1035, 0.4) and Core (0.9451, 0.5608, 0.0471, 0.902) -- the pictures
// themselves are white; the outline stays as it is.
uint8_t g_cut_glow[500 * 500 * 4];
void DrawScaled(uint8_t* o, uint32_t ow, uint32_t oh, const uint8_t* src, uint32_t sw, uint32_t shh, float sc,
                const float tint[4]) {
    const float dw = sw * sc, dh = shh * sc;
    if (dw < 1 || dh < 1) return;
    const float x0 = (ow - dw) / 2.0f, y0 = (oh - dh) / 2.0f;
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x) {
            const float u = (x + 0.5f - x0) / dw, v = (y + 0.5f - y0) / dh;
            if (u < 0 || v < 0 || u >= 1 || v >= 1) continue;
            const uint8_t* p = src + (uint64_t{static_cast<uint32_t>(v * shh)} * sw + static_cast<uint32_t>(u * sw)) * 4;
            uint8_t t[4];
            for (int c = 0; c < 4; ++c) t[c] = static_cast<uint8_t>(p[c] * tint[c] + 0.5f);
            Over(o + (uint64_t{y} * ow + x) * 4, t);
        }
}
// 0.9.101 the gauge's numbers drawn the game's way: TextMeshPro's SDF shader (TMP_SDF: face, outline, glow,
// underlay) with the font Aldrich-Regular SDF and the material "Aldrich-Regular SDF_battle_effect_guts" (r71: face
// white x the picture battle_guts_color (r72), outline (1, .651, .259) 0.03, glow (1, .490, 0, .588) offset .2 inner
// .5 outer .2 power .1, underlay (.821, .210, 0, .498) dilate 1 softness .475, face dilate .1, ScaleRatio A/B/C
// .8571/.6107/.414, GradientScale 7). Layout from the prefab (r70): the count (size 48) centred 1.5 units up, the
// "/ next" (size 24) right-aligned to 50 units right, centred 37.5 units down.
struct NumGlyph { float bx, by, adv; int32_t x, y, w, h; bool have; };
NumGlyph g_ng[11];                       // '0'..'9', '/'
int g_ng_state;                          // 0 not read, 1 read, -1 failed
int g_bn_font = -1;                      // what the last gauge drew: 1 its numbers, 0 not (the page shows text)
int g_ng_err;
int g_nf_names, g_nf_tables, g_nf_found;   // 0.9.103 the search by name: names seen, glyph tables seen, found
int64_t RelocateFont(const EdenDsmodHostApi* h, OpenBundle* ob, IdxFont& f) {
    const uint64_t p31 = RollPow31(), end = ob->ustart[ob->n];
    uint8_t ring[32] = {};
    uint64_t hv = 0, at = 0;
    for (uint32_t k = 0; k < ob->n; ++k) {
        const uint8_t* d = Chunk(h, ob, k);
        if (!d) return -2;
        for (uint32_t i = 0; i < ob->us[k]; ++i, ++at) {
            const uint8_t b = d[i];
            hv = (at >= 32 ? (hv - ring[at & 31] * p31) : hv) * kRollB + b;
            ring[at & 31] = b;
            if (at < 31 || hv != f.sig_hash) continue;
            const uint64_t win = at - 31;
            if (win < f.sig_off) continue;
            const uint64_t start = win - f.sig_off;
            if (start + f.c_at + f.cn * f.c_st > end) continue;
            uint32_t gn = 0, cn = 0;           // the tables' counts sit just before them
            const bool ok = StreamRead(h, ob, start + f.g_at - 4, &gn, 4) && StreamRead(h, ob, start + f.c_at - 4, &cn, 4) &&
                            gn == f.gn && cn == f.cn;
            if (ok) return static_cast<int64_t>(start);
            d = Chunk(h, ob, k);
            if (!d) return -2;
        }
    }
    // 0.9.103 the 1.0.1 update changed the object (Thor: font 0/4 in 0.9.101 and 0.9.102): find it by its name
    // ("Aldrich-Regular SDF", m_Name at +28) and read where its tables are from the object itself: the glyph table
    // (a count, then 52-byte records with scale 1.0 at +40) and, after it, the character table (a count, then 16-byte
    // records: type 1, codepoint, glyph index, scale 1.0)
    static const char kName[] = "Aldrich-Regular SDF";
    const uint32_t nl = sizeof kName - 1;
    at = 0;
    for (uint32_t k = 0; k < ob->n; ++k) {
        const uint8_t* d = Chunk(h, ob, k);
        if (!d) return -2;
        const uint32_t n = ob->us[k];
        for (uint32_t i = 0; i + 4 + nl <= n; ++i) {
            if (d[i] != nl || d[i + 1] || d[i + 2] || d[i + 3] || d[i + 4] != 'A') continue;
            { uint32_t m = 0; while (m < nl && d[i + 4 + m] == static_cast<uint8_t>(kName[m])) ++m; if (m != nl) continue; }
            ++g_nf_names;
            const uint64_t N = at + i;
            if (N < 28) continue;
            const uint64_t obj = N - 28;
            static uint8_t ob2[4096];
            if (obj + sizeof ob2 > end || !StreamRead(h, ob, obj, ob2, sizeof ob2)) { d = Chunk(h, ob, k); if (!d) return -2; continue; }
            d = Chunk(h, ob, k);
            if (!d) return -2;
            auto u32 = [&](uint32_t o) { uint32_t v; memcpy(&v, ob2 + o, 4); return v; };
            for (uint32_t g = (32 + nl + 3) & ~3u; g + 4 + 2 * 52 <= sizeof ob2; g += 4) {
                const uint32_t gn = u32(g);
                if (gn < 11 || gn > 4000 || u32(g + 4 + 40) != 0x3F800000u || u32(g + 4 + 52 + 40) != 0x3F800000u) continue;
                ++g_nf_tables;
                // the character table: within 256 bytes after the glyph table's end (read from the stream)
                const uint64_t gend = obj + g + 4 + uint64_t{gn} * 52;
                uint8_t c[256 + 4 + 64];
                if (gend + sizeof c > end || !StreamRead(h, ob, gend, c, sizeof c)) break;
                d = Chunk(h, ob, k);
                if (!d) return -2;
                for (uint32_t q = 0; q + 4 + 64 <= sizeof c; q += 4) {
                    uint32_t cn; memcpy(&cn, c + q, 4);
                    if (cn < 11 || cn > 8000) continue;
                    bool ok = true;
                    for (int r = 0; r < 4 && ok; ++r) {
                        uint32_t type, u, sc; memcpy(&type, c + q + 4 + r * 16, 4); memcpy(&u, c + q + 8 + r * 16, 4);
                        memcpy(&sc, c + q + 16 + r * 16, 4);
                        ok = type == 1 && u < 0x110000 && sc == 0x3F800000u;
                    }
                    if (!ok) continue;
                    f.g_at = g + 4; f.gn = gn; f.g_st = 52;
                    f.c_at = (gend - obj) + q + 4; f.cn = cn; f.c_st = 16;
                    g_nf_found = 1;
                    return static_cast<int64_t>(obj);
                }
                break;
            }
        }
        at += n;
    }
    return -2;
}
// 0.9.104 the atlas changed in 1.0.1 too (Thor: font 0/10, picture error 35): find its Texture2D by name ("<font>
// Atlas", m_Name first), read its size and format, and where its pixels are -- after its data size (inline) or in
// its m_StreamData (offset, size, the .resS file's path)
int g_na_names, g_na_found, g_na_blob = -1, g_na_gobs = -1;
bool FindAtlas(const EdenDsmodHostApi* h, OpenBundle* ob, IdxTex& t, int64_t& at_out) {
    static const char kName[] = "Aldrich-Regular SDF Atlas";
    const uint32_t nl = sizeof kName - 1;
    const uint64_t end = ob->ustart[ob->n];
    uint64_t at = 0;
    for (uint32_t k = 0; k < ob->n; ++k) {
        const uint8_t* d = Chunk(h, ob, k);
        if (!d) return false;
        const uint32_t n = ob->us[k];
        for (uint32_t i = 0; i + 4 + nl <= n; ++i) {
            if (d[i] != nl || d[i + 1] || d[i + 2] || d[i + 3] || d[i + 4] != 'A') continue;
            { uint32_t m = 0; while (m < nl && d[i + 4 + m] == static_cast<uint8_t>(kName[m])) ++m; if (m != nl) continue; }
            ++g_na_names;
            const uint64_t obj = at + i;
            static uint8_t b[1024];
            if (obj + sizeof b > end || !StreamRead(h, ob, obj, b, sizeof b)) { d = Chunk(h, ob, k); if (!d) return false; continue; }
            d = Chunk(h, ob, k);
            if (!d) return false;
            auto u32 = [&](uint32_t o) { uint32_t v; memcpy(&v, b + o, 4); return v; };
            const uint32_t s0 = (4 + nl + 3) & ~3u;
            for (uint32_t q = s0; q + 20 <= 256; q += 4) {      // width, height, complete size, mips stripped, format
                const uint32_t W = u32(q), H = u32(q + 4), cs = u32(q + 8), fmt = u32(q + 16);
                if (W < 64 || W > 4096 || H < 64 || H > 4096 || fmt != 1 || cs != W * H) continue;
                // the data: a u32 size == cs, then inline pixels -- or 0 and the stream data (u64 offset, u32 size, path)
                // 0.9.106 the fields in their Unity 2021.3 order from the width (q): height +4, complete size +8, mips
                // stripped +12, format +16, mip count +20, four bools +24, streaming priority +28, image count +32,
                // dimension +36, texture settings +40 (24 bytes), lightmap format +64, colour space +68, then the
                // platform blob (u32 length + bytes, aligned) at +72: blob[8..12] = log2 of the GOBs a block (none:
                // plain rows); then the image data (u32 size + pixels) or size 0 and the stream data (u64 offset, u32
                // size, the .resS path). (0.9.105 guessed the blob from the end and took a zero for it.)
                {
                    const uint32_t L = u32(q + 72);
                    const uint32_t r = q + 76 + ((L + 3) & ~3u);
                    if (L <= 256 && r + 24 <= sizeof b) {
                        g_na_blob = static_cast<int>(L);
                        int gb = -1;
                        if (L == 0) gb = 0;
                        else if (L >= 12) { const uint32_t lg = u32(q + 76 + 8); gb = lg < 6 ? 1 << lg : -1; }
                        g_na_gobs = gb;
                        const uint32_t dsz = u32(r);
                        if (gb >= 0 && dsz == cs) {
                            t.w = W; t.h = H; t.size = cs; t.gobs = static_cast<uint32_t>(gb);
                            at_out = static_cast<int64_t>(obj + r + 4);
                            g_na_found = 3;
                            return true;
                        }
                        if (gb >= 0 && dsz == 0 && u32(r + 12) == cs) {
                            uint64_t so; memcpy(&so, b + r + 4, 8);
                            const uint32_t pl = u32(r + 16);
                            if (pl >= 5 && pl <= 200 && r + 20 + pl <= sizeof b) {
                                const char* path = reinterpret_cast<const char*>(b + r + 20);
                                uint32_t bgn = pl; while (bgn > 0 && path[bgn - 1] != '/') --bgn;
                                for (uint32_t nd = 0; nd < ob->nn; ++nd) {
                                    uint32_t m = 0; const char* nm = ob->node_name[nd];
                                    while (bgn + m < pl && nm[m] == path[bgn + m]) ++m;
                                    if (bgn + m != pl || nm[m]) continue;
                                    t.w = W; t.h = H; t.size = cs; t.gobs = static_cast<uint32_t>(gb);
                                    at_out = static_cast<int64_t>(ob->node_off[nd] + so);
                                    g_na_found = 4;
                                    return true;
                                }
                            }
                        }
                    }
                }
                for (uint32_t r = q + 20; r + 16 <= sizeof b; r += 4) {
                    if (u32(r) == cs) {                          // inline
                        t.w = W; t.h = H; t.size = cs;
                        at_out = static_cast<int64_t>(obj + r + 4);
                        g_na_found = 1;
                        return true;
                    }
                    if (u32(r) == 0 && r + 4 + 16 <= sizeof b) {
                        uint64_t so; memcpy(&so, b + r + 4, 8);
                        const uint32_t ss = u32(r + 12), pl = u32(r + 16);
                        if (ss != cs || pl < 5 || pl > 200 || r + 20 + pl > sizeof b) continue;
                        const char* path = reinterpret_cast<const char*>(b + r + 20);
                        uint32_t e = pl, bgn = pl; while (bgn > 0 && path[bgn - 1] != '/') --bgn;
                        for (uint32_t nd = 0; nd < ob->nn; ++nd) {
                            uint32_t m = 0; const char* nm = ob->node_name[nd];
                            while (bgn + m < e && nm[m] == path[bgn + m]) ++m;
                            if (bgn + m != e || nm[m]) continue;
                            t.w = W; t.h = H; t.size = cs;
                            at_out = static_cast<int64_t>(ob->node_off[nd] + so);
                            g_na_found = 2;
                            return true;
                        }
                        g_na_found = -1;                         // a path, but not one of the bundle's files
                    }
                }
                break;
            }
        }
        at += n;
    }
    return false;
}
bool NumGlyphsRead(const EdenDsmodHostApi* h) {
    if (g_ng_state) return g_ng_state > 0;
    g_ng_state = -1;
    IdxFont& f = g_ix.nf;
    if (!f.ok) { g_ng_err = 1; return false; }
    OpenBundle* ob = Open(h, f.bundle);
    if (!ob) { g_ng_err = 2; return false; }
    uint64_t obj = f.obj;
    if (ob->moved) {
        if (!f.sig_hash) { g_ng_err = 3; return false; }
        if (f.reloc == -1) f.reloc = RelocateFont(h, ob, f);
        if (f.reloc < 0) { g_ng_err = 4; return false; }
        obj = static_cast<uint64_t>(f.reloc);
    }
    uint32_t gidx[11];
    for (auto& v : gidx) v = ~0u;
    uint8_t buf[4096];
    for (uint64_t i = 0; i < f.cn;) {
        const uint64_t per = sizeof buf / f.c_st, n = f.cn - i < per ? f.cn - i : per;
        if (!StreamRead(h, ob, obj + f.c_at + i * f.c_st, buf, n * f.c_st)) { g_ng_err = 5; return false; }
        for (uint64_t k = 0; k < n; ++k) {
            uint32_t u, gi; memcpy(&u, buf + k * f.c_st + 4, 4); memcpy(&gi, buf + k * f.c_st + 8, 4);
            if (u >= '0' && u <= '9') gidx[u - '0'] = gi;
            else if (u == '/') gidx[10] = gi;
        }
        i += n;
    }
    memset(g_ng, 0, sizeof g_ng);
    for (uint64_t i = 0; i < f.gn;) {
        const uint64_t per = sizeof buf / f.g_st, n = f.gn - i < per ? f.gn - i : per;
        if (!StreamRead(h, ob, obj + f.g_at + i * f.g_st, buf, n * f.g_st)) { g_ng_err = 6; return false; }
        for (uint64_t k = 0; k < n; ++k) {
            const uint8_t* r = buf + k * f.g_st;
            uint32_t idx; memcpy(&idx, r, 4);
            for (int c = 0; c < 11; ++c) {
                if (gidx[c] != idx || g_ng[c].have) continue;
                memcpy(&g_ng[c].bx, r + 12, 4); memcpy(&g_ng[c].by, r + 16, 4); memcpy(&g_ng[c].adv, r + 20, 4);
                memcpy(&g_ng[c].x, r + 24, 4); memcpy(&g_ng[c].y, r + 28, 4);
                memcpy(&g_ng[c].w, r + 32, 4); memcpy(&g_ng[c].h, r + 36, 4);
                g_ng[c].have = g_ng[c].w > 0 && g_ng[c].h > 0 && g_ng[c].w < 200 && g_ng[c].h < 200;
            }
        }
        i += n;
    }
    for (int c = 0; c < 11; ++c) if (!g_ng[c].have) { g_ng_err = 7; return false; }
    g_ng_state = 1;
    return true;
}
float Sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float Pow01(float x) {                   // x^0.1 for x in 0..1 (the glow's _GlowPower): a guess from the bits, Newton
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    uint32_t b; memcpy(&b, &x, 4);
    const int32_t e = static_cast<int32_t>(b) - 0x3F800000;
    const uint32_t gb = static_cast<uint32_t>(0x3F800000 + e / 10);
    float y; memcpy(&y, &gb, 4);
    for (int i = 0; i < 4; ++i) {
        float y9 = y * y; y9 *= y9; y9 *= y9; y9 *= y;   // y^9
        y -= (y9 * y - x) / (10 * y9);
    }
    return y;
}
uint8_t g_ng_sdf[11][220 * 220];         // each glyph's distance field with its padding around it (atlas pixels)
uint32_t g_ng_sw[11], g_ng_sh[11];
uint8_t g_guts[64 * 64 * 4];
uint32_t g_guts_w, g_guts_h;
bool NumAtlas(const EdenDsmodHostApi* h) {
    static int state;
    if (state) return state > 0;
    state = -1;
    const IdxTex* t = FindTex(g_ix.nf.tex);
    if (!t || t->fmt != 1) { g_ng_err = 8; return false; }
    {   // 0.9.104 in a moved bundle: the atlas by its fingerprint, else by its name
        const int ti = static_cast<int>(t - g_ix.t);
        OpenBundle* ob = Open(h, t->bundle);
        if (ob && ob->moved && ti >= 0 && ti < g_ix.nt) {
            if (g_reloc[ti] == -1) g_reloc[ti] = Relocate(h, ob, *t);
            if (g_reloc[ti] < 0) {
                int64_t at = -2;
                if (FindAtlas(h, ob, g_ix.t[ti], at)) g_reloc[ti] = at;
            }
        }
    }
    const int pad = g_ix.nf.padding;
    for (int c = 0; c < 11; ++c) {
        const NumGlyph& q = g_ng[c];
        const int x0 = q.x - pad, y0 = q.y - pad, w = q.w + 2 * pad, hh = q.h + 2 * pad;
        if (x0 < 0 || y0 < 0 || w > 220 || hh > 220) { g_ng_err = 9; return false; }
        if (!DecodeRect(h, *t, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(w),
                        static_cast<uint32_t>(hh))) { g_ng_err = 10; return false; }
        for (int i = 0; i < w * hh; ++i) g_ng_sdf[c][i] = g_pic[i * 4 + 3];     // top row first
        g_ng_sw[c] = static_cast<uint32_t>(w); g_ng_sh[c] = static_cast<uint32_t>(hh);
    }
    if (!CutSprite(h, "battle_guts_color", g_guts, sizeof g_guts, g_guts_w, g_guts_h)) g_guts_w = g_guts_h = 0;
    state = 1;
    return true;
}
// one string at font size `size` (game units), its left end at ux, baseline at uy (units; y up from the centre)
void NumString(uint8_t* o, uint32_t S, float px, const char* str, float size, float ux, float uy) {
    const float pad = static_cast<float>(g_ix.nf.padding), pt = static_cast<float>(g_ix.nf.point > 0 ? g_ix.nf.point : 36);
    const float k = size / pt * px;                  // output pixels an atlas pixel
    const float scale = 7.0f * k;                    // the shader's scale: _GradientScale x screen px an atlas px
    const float weight = 0.1f * 0.8571f * 0.5f;      // (_WeightNormal / 4 + _FaceDilate) x _ScaleRatioA x 0.5
    const float bias = (0.5f - weight) + 0.5f / scale;
    const float outline = 0.03f * 0.8571f * scale;
    const float uScale = scale / (1 + 0.475f * 0.414f * scale);
    const float uBias = (0.5f - weight) * uScale - 0.5f - (1.0f * 0.414f * 0.5f * uScale);
    static constexpr float kFace[3] = {1, 1, 1}, kLine[4] = {1, 0.651f, 0.2588f, 1}, kGlow[4] = {1, 0.4902f, 0, 0.5882f},
                           kUnder[4] = {0.8208f, 0.2104f, 0, 0.498f};
    float pen = ux;                                  // in atlas pixels from here on, scaled by size / point
    const float ox = S / 2.0f, oy = S / 2.0f;
    for (const char* p = str; *p; ++p) {
        const int c = *p == '/' ? 10 : (*p >= '0' && *p <= '9') ? *p - '0' : -1;
        if (c < 0) continue;
        const NumGlyph& q = g_ng[c];
        const uint32_t sw = g_ng_sw[c], sh = g_ng_sh[c];
        // the glyph's quad (ink box + padding), in units: left, top
        const float left = pen + (q.bx - pad) * size / pt, top = uy + (q.by + pad) * size / pt;
        const float qw = sw * size / pt, qh = sh * size / pt;
        const int X0 = static_cast<int>(ox + left * px) - 1, X1 = static_cast<int>(ox + (left + qw) * px) + 2;
        const int Y0 = static_cast<int>(oy - top * px) - 1, Y1 = static_cast<int>(oy - (top - qh) * px) + 2;
        for (int Y = Y0 < 0 ? 0 : Y0; Y < Y1 && Y < static_cast<int>(S); ++Y)
            for (int X = X0 < 0 ? 0 : X0; X < X1 && X < static_cast<int>(S); ++X) {
                // the atlas position (pixels from the quad's top left), bilinear
                const float ax = ((X + 0.5f - ox) / px - left) / (size / pt) - 0.5f;
                const float ay = (top - (oy - (Y + 0.5f)) / px) / (size / pt) - 0.5f;
                if (ax < -0.5f || ay < -0.5f || ax > sw - 0.5f || ay > sh - 0.5f) continue;
                const int ix = static_cast<int>(ax < 0 ? ax - 1 : ax), iy = static_cast<int>(ay < 0 ? ay - 1 : ay);
                const float fx = ax - ix, fy = ay - iy;
                float a = 0;
                for (int n = 0; n < 4; ++n) {
                    int sx = ix + (n & 1), sy = iy + (n >> 1);
                    sx = sx < 0 ? 0 : sx >= static_cast<int>(sw) ? static_cast<int>(sw) - 1 : sx;
                    sy = sy < 0 ? 0 : sy >= static_cast<int>(sh) ? static_cast<int>(sh) - 1 : sy;
                    a += g_ng_sdf[c][sy * sw + sx] / 255.0f * ((n & 1) ? fx : 1 - fx) * ((n >> 1) ? fy : 1 - fy);
                }
                // face colour x the face picture (the character's quad, its top at the picture's top)
                float face[4] = {kFace[0], kFace[1], kFace[2], 1};
                if (g_guts_w && g_guts_h) {
                    const float u = Sat((ax + 0.5f) / sw), v = Sat((ay + 0.5f) / sh);
                    const uint8_t* g = g_guts + (static_cast<uint32_t>(v * (g_guts_h - 1) + 0.5f) * g_guts_w +
                                                 static_cast<uint32_t>(u * (g_guts_w - 1) + 0.5f)) * 4;
                    for (int ch = 0; ch < 3; ++ch) face[ch] *= g[ch] / 255.0f;
                    face[3] *= g[3] / 255.0f;
                }
                const float sd = (bias - a) * scale;
                // GetColor
                const float faceA = 1 - Sat(sd - outline * 0.5f);
                const float lineA = Sat(sd + outline * 0.5f) * SqrtF(outline < 1 ? outline : 1);
                float col[4];
                for (int ch = 0; ch < 3; ++ch) {
                    const float fc = face[ch] * face[3], lc = kLine[ch] * kLine[3];
                    col[ch] = (fc + (lc - fc) * lineA) * faceA;
                }
                col[3] = (face[3] + (kLine[3] - face[3]) * lineA) * faceA;
                // GetGlowColor, added
                float gl = sd - (0.2f * 0.6107f) * 0.5f * scale;
                const float tt = (gl >= 0 ? 0.2f * 0.6107f : 0.5f) * 0.5f * scale;
                gl = Sat(AbsF(gl / (1 + tt)));
                gl = 1 - Pow01(gl);
                gl *= SqrtF(tt < 1 ? tt : 1);
                const float ga = Sat(kGlow[3] * gl * 2);
                for (int ch = 0; ch < 3; ++ch) col[ch] += kGlow[ch] * ga;
                // the underlay, under what is there
                const float ua = Sat(a * uScale - uBias) * (1 - col[3]);
                for (int ch = 0; ch < 3; ++ch) col[ch] += kUnder[ch] * kUnder[3] * ua;
                col[3] += kUnder[3] * ua;
                // premultiplied (glow is light added) -> straight over the picture
                float m = col[3];
                for (int ch = 0; ch < 3; ++ch) if (col[ch] > m) m = col[ch];
                if (m <= 0.002f) continue;
                if (m > 1) m = 1;
                uint8_t t[4];
                for (int ch = 0; ch < 3; ++ch) t[ch] = static_cast<uint8_t>(Sat(col[ch] / m) * 255 + 0.5f);
                t[3] = static_cast<uint8_t>(m * 255 + 0.5f);
                Over(o + (uint64_t(Y) * S + X) * 4, t);
            }
        pen += q.adv * size / pt;
    }
}
float NumWidth(const char* str, float size) {
    const float pt = static_cast<float>(g_ix.nf.point > 0 ? g_ix.nf.point : 36);
    float w = 0;
    for (const char* p = str; *p; ++p) {
        const int c = *p == '/' ? 10 : (*p >= '0' && *p <= '9') ? *p - '0' : -1;
        if (c >= 0) w += g_ng[c].adv * size / pt;
    }
    return w;
}
// the count and "/ next" on the gauge picture; false: not drawn (no font)
bool BonusNumbers(const EdenDsmodHostApi* h, uint8_t* o, uint32_t S, float px, int pt, int next) {
    if (!NumGlyphsRead(h) || !NumAtlas(h)) return false;
    const float asc = g_ix.nf.ascent100 / 100.0f, desc = g_ix.nf.descent100 / 100.0f, fp = static_cast<float>(g_ix.nf.point);
    char a[16], b[16];
    int n = 0;
    { char r[12]; int m = 0; int v = pt < 0 ? 0 : pt; do { r[m++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && m < 10);
      while (m) a[n++] = r[--m]; a[n] = 0; }
    n = 0; b[n++] = '/';
    { char r[12]; int m = 0; int v = next < 0 ? 0 : next; do { r[m++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && m < 10);
      while (m) b[n++] = r[--m]; b[n] = 0; }
    // middle alignment: the line (ascent .. descent) centred on the box's centre
    const float s1 = 48, s2 = 24;
    const float base1 = 1.5f - (asc - desc) / 2 * s1 / fp, base2 = -37.5f - (asc - desc) / 2 * s2 / fp;
    NumString(o, S, px, a, s1, -NumWidth(a, s1) / 2, base1);
    NumString(o, S, px, b, s2, 50 - NumWidth(b, s2), base2);
    return true;
}

// 0.9.94 sizes from the prefab (r69, in the game's units, the presenter 200 x 200): the ring (Orbit) 212 across,
// the growing group (Scale_Noise, the presenter's sphereScaleTransform) holds the Core 180 across and the Glow
// 320 x 0.7 = 224 across, scaled by how far the points are into the stage. Output: 240 units square at the
// ring picture's own resolution (332 px for 212 units).
bool BonusGauge(const EdenDsmodHostApi* h, int permille, int pt, int next, void* receiver, EdenDsmodImageSink sink) {
    uint32_t ow0, oh0, cw, chh, gw = 0, gh = 0;
    if (!CutSprite(h, "battle_bonus_gauge_outline", g_cut_big[0], sizeof g_cut_big[0], ow0, oh0) ||
        !CutSprite(h, "battle_bonus_gauge_sphere_core", g_cut_big[1], sizeof g_cut_big[1], cw, chh)) return false;
    const bool glow = CutSprite(h, "battle_bonus_gauge_sphere_glow", g_cut_glow, sizeof g_cut_glow, gw, gh);
    const float ratio = (permille < 0 ? 0 : permille > 1000 ? 1000 : permille) / 1000.0f;
    const float px = ow0 / 212.0f;                         // pixels a game unit
    const uint32_t S = static_cast<uint32_t>(240 * px + 0.5f);
    uint8_t* o = g_pic2;
    memset(o, 0, uint64_t{S} * S * 4);
    static constexpr float kGlow[4] = {0.8774f, 0.3409f, 0.1035f, 0.4f};
    static constexpr float kCore[4] = {0.9451f, 0.5608f, 0.0471f, 0.902f};
    static constexpr float kWhite[4] = {1, 1, 1, 1};
    if (glow) DrawScaled(o, S, S, g_cut_glow, gw, gh, 224 * px * ratio / gw, kGlow);
    DrawScaled(o, S, S, g_cut_big[1], cw, chh, 180 * px * ratio / cw, kCore);
    DrawScaled(o, S, S, g_cut_big[0], ow0, oh0, 1.0f, kWhite);
    if (next > 0) g_bn_font = BonusNumbers(h, o, S, px, pt, next) ? 1 : 0;   // 0.9.101
    sink(receiver, S, S, o, uint64_t{S} * S * 4);
    return true;
}

// "@ill:<NNNN>": a character's picture on the Party Formation card, placed the way the game does
// (uiscene UICampCharacterStatusPresenter, research r38/r39): the sprite at its own size (SetNativeSize),
// its full rect's bottom centre at the card's centre, standingPictureDefaultPositionY (-1100) plus the
// character's standingPictureCharacterOffsetPositionYList entry above the canvas centre, seen through
// the card's mask (selectedCharacterWidth x selectedCharacterHeight, 220 x 690, from the card's top at
// canvas y +400) with the RectMask2D's 50-pixel soft top and bottom edges. Output: the mask, 220 x 690.
constexpr int kRosterW = 272, kRosterDx = 284;   // gen_manifest CARD_W / CARD_DX (keep in step)
constexpr int kIllMaskW = 220, kIllMaskH = 690, kIllSoft = 50;
constexpr int kBattleIllH = 552;   // 0.9.93 gen_manifest BATTLE_ILL_H (keep in step)
constexpr int kSkillIllH = 508;    // 0.9.129 gen_manifest SKILL_ILL_H: the Skills pages' member card (keep in step)
constexpr int kIllBottom = 400 + 1100;             // mask top (canvas +400) to the default picture bottom (-1100)
constexpr int kIllOffsetY[13] = {-13, 111, 25, -11, -5, 253, -4, 270, 86, -28, 18, 32, 93};   // by PlayerID - 1
bool Illustration(const EdenDsmodHostApi* h, const char* id, void* receiver, EdenDsmodImageSink sink) {
    char nm[40] = "charaillust_";
    size_t i = 12;
    int pid = 0;
    const char* q = id;
    for (; *q >= '0' && *q <= '9' && i + 1 < sizeof nm; ++q) { nm[i++] = *q; pid = pid * 10 + (*q - '0'); }
    nm[i] = 0;
    // 0.9.93 "@ill:<NNNN>:<H>": the mask only <H> rows tall (the Battle page's shorter card), its soft edge there
    int mh = kIllMaskH;
    if (*q == ':') { int v = 0; for (++q; *q >= '0' && *q <= '9'; ++q) v = v * 10 + (*q - '0'); if (v >= 200 && v < kIllMaskH) mh = v; }
    const IdxSprite* s = FindSprite(nm);
    if (!s) { g_pic_err = 50; return false; }
    const IdxTex* t = FindTex(s->tex);
    if (!t) { g_pic_err = 51; return false; }
    // 0.9.22: only the part the card's mask shows is decoded (about a fifth of the picture)
    const int sw = static_cast<int>(s->w), sh = static_cast<int>(s->h);
    const int fw = s->fw ? static_cast<int>(s->fw) : sw;       // an old index: the trimmed size
    const int off = pid >= 1 && pid <= 13 ? kIllOffsetY[pid - 1] : 0;
    const int bottom = kIllBottom - off;                        // the full rect's bottom, from the mask top
    int left = kIllMaskW / 2 - fw / 2 + static_cast<int>(s->ox);   // the trimmed picture in the mask
    int top = bottom - static_cast<int>(s->oy) - sh;
    uint32_t w = s->w, hh = s->h;
    if (s->rot == 0) {
        const int x0 = left < 0 ? -left : 0, x1 = sw < kIllMaskW - left ? sw : kIllMaskW - left;
        const int y0 = top < 0 ? -top : 0, y1 = sh < mh - top ? sh : mh - top;
        if (x1 <= x0 || y1 <= y0) { g_pic_err = 52; return false; }
        w = static_cast<uint32_t>(x1 - x0); hh = static_cast<uint32_t>(y1 - y0);
        // texture rows run bottom-up: the visible rows' bottom is (sh - y1) above the sprite's bottom
        if (!DecodeRect(h, *t, s->x + static_cast<uint32_t>(x0), s->y + static_cast<uint32_t>(sh - y1), w, hh)) return false;
        left += x0; top += y0;
    } else {
        if (!DecodeRect(h, *t, s->x, s->y, w, hh)) return false;
        Rotate(s->rot, w, hh);
    }
    const uint32_t ow = kIllMaskW, oh = static_cast<uint32_t>(mh);
    uint8_t* o = g_pic2;
    for (uint32_t y = 0; y < oh; ++y) {
        const int ey = static_cast<int>(y) < kIllSoft ? static_cast<int>(y) : static_cast<int>(oh - 1 - y);
        const int fade = ey < kIllSoft ? (ey * 255 + kIllSoft / 2) / kIllSoft : 255;   // the mask's soft edges
        for (uint32_t x = 0; x < ow; ++x) {
            const int sx = static_cast<int>(x) - left, sy = static_cast<int>(y) - top;
            uint8_t* d = o + (uint64_t{y} * ow + x) * 4;
            if (sx < 0 || sx >= static_cast<int>(w) || sy < 0 || sy >= static_cast<int>(hh)) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            memcpy(d, g_pic + (uint64_t(sy) * w + sx) * 4, 4);
            d[3] = static_cast<uint8_t>(d[3] * fade / 255);
        }
    }
    sink(receiver, ow, oh, o, uint64_t{ow} * oh * 4);
    return true;
}

// 0.9.130 "@body:<NNNN>": the character's whole picture (charaillust_NNNN, all of it), scaled to fit a
// kBodyW x kBodyH canvas (gen_manifest BODY_W / BODY_H: the Skills pages' right side), bottom centre, area-averaged
constexpr int kBodyW = 392, kBodyH = 740;
// 0.9.144 a sprite fitted into a kw x kh canvas (bottom centre, or centred when mid): the body, the title logo
// 0.9.145 mode 2 (cover): fills the whole canvas, cut at the sides / top and bottom (the title background)
bool FitSprite(const EdenDsmodHostApi* h, const char* nm, int kBodyW, int kBodyH, int mid, void* receiver,
               EdenDsmodImageSink sink) {
    const IdxSprite* s = FindSprite(nm);
    if (!s) { g_pic_err = 55; return false; }
    const IdxTex* t = FindTex(s->tex);
    if (!t) { g_pic_err = 56; return false; }
    uint32_t w = s->w, hh = s->h;
    if (!DecodeRect(h, *t, s->x, s->y, w, hh)) return false;
    Rotate(s->rot, w, hh);
    const float scw = static_cast<float>(kBodyW) / w, sch = static_cast<float>(kBodyH) / hh;
    const float sc = mid == 2 ? (scw > sch ? scw : sch) : (scw < sch ? scw : sch);
    int dw = static_cast<int>(w * sc), dh = static_cast<int>(hh * sc);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (mid != 2 && dw > kBodyW) dw = kBodyW;
    if (mid != 2 && dh > kBodyH) dh = kBodyH;
    const int ox = (kBodyW - dw) / 2, oy = mid ? (kBodyH - dh) / 2 : kBodyH - dh;
    uint8_t* o = g_pic2;
    memset(o, 0, uint64_t(kBodyW) * kBodyH * 4);
    for (int y = 0; y < dh; ++y) {
        if (oy + y < 0 || oy + y >= kBodyH) continue;
        const int sy0 = static_cast<int>(y / sc);
        int sy1 = static_cast<int>((y + 1) / sc);
        if (sy1 > static_cast<int>(hh)) sy1 = static_cast<int>(hh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dw; ++x) {
            if (ox + x < 0 || ox + x >= kBodyW) continue;
            const int sx0 = static_cast<int>(x / sc);
            int sx1 = static_cast<int>((x + 1) / sc);
            if (sx1 > static_cast<int>(w)) sx1 = static_cast<int>(w);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = sy0; yy < sy1; ++yy)
                for (int xx = sx0; xx < sx1; ++xx) {
                    const uint8_t* q = g_pic + (uint64_t(yy) * w + xx) * 4;
                    for (int c = 0; c < 3; ++c) acc[c] += q[c] * q[3];   // premultiplied, so the edges stay clean
                    acc[3] += q[3]; ++n;
                }
            uint8_t* d = o + (uint64_t(oy + y) * kBodyW + (ox + x)) * 4;
            if (acc[3]) for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(acc[c] / acc[3]);
            d[3] = static_cast<uint8_t>(acc[3] / n);
        }
    }
    sink(receiver, kBodyW, kBodyH, o, uint64_t(kBodyW) * kBodyH * 4);
    return true;
}
bool Body(const EdenDsmodHostApi* h, const char* id, void* receiver, EdenDsmodImageSink sink) {
    char nm[40] = "charaillust_";
    size_t i = 12;
    for (const char* q = id; *q >= '0' && *q <= '9' && i + 1 < sizeof nm; ++q) nm[i++] = *q;
    nm[i] = 0;
    return FitSprite(h, nm, kBodyW, kBodyH, 0, receiver, sink);
}
constexpr int kLogoW = 900, kLogoH = 668;   // 0.9.145 gen_manifest TITLE_LOGO_W / _H (keep in step; logo_title 1024 x 760)
constexpr int kTitleBgW = 1240, kTitleBgH = 1080;   // the page
// 0.9.147 the title screen as the game draws it (the top screen, 1920 x 1080): its background, the globe (core and
// its debris rings) behind the logo, the logo -- composed at the game's scale and cut to the page's middle 1240.
uint8_t g_title[kTitleBgW * kTitleBgH * 4];
// 0.9.149 sc: drawn at that scale (nearest), its centre at cx, cy
bool TitleLayer(const EdenDsmodHostApi* h, const char* nm, int cx, int cy, bool cover, float sc = 1.0f) {
    const IdxSprite* s = FindSprite(nm);
    if (!s) return false;
    const IdxTex* t = FindTex(s->tex);
    if (!t) return false;
    uint32_t w = s->w, hh = s->h;
    if (!DecodeRect(h, *t, s->x, s->y, w, hh)) return false;
    Rotate(s->rot, w, hh);
    const int dw = static_cast<int>(w * sc), dh = static_cast<int>(hh * sc);
    const int ox = cx - dw / 2 - (1920 - kTitleBgW) / 2, oy = cy - dh / 2;
    for (int y = 0; y < dh; ++y) {
        const int ty = oy + y;
        if (ty < 0 || ty >= kTitleBgH) continue;
        const int sy = static_cast<int>(y / sc) < static_cast<int>(hh) ? static_cast<int>(y / sc) : static_cast<int>(hh) - 1;
        for (int x = 0; x < dw; ++x) {
            const int tx = ox + x;
            if (tx < 0 || tx >= kTitleBgW) continue;
            const int sx = static_cast<int>(x / sc) < static_cast<int>(w) ? static_cast<int>(x / sc) : static_cast<int>(w) - 1;
            const uint8_t* q = g_pic + (uint64_t(sy) * w + sx) * 4;
            uint8_t* d = g_title + (uint64_t(ty) * kTitleBgW + tx) * 4;
            if (cover) { memcpy(d, q, 3); d[3] = 255; continue; }
            const uint32_t a = q[3];
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((q[c] * a + d[c] * (255 - a)) / 255);
            d[3] = static_cast<uint8_t>(a + d[3] * (255 - a) / 255);
        }
    }
    return true;
}
bool g_title_ready;   // 0.9.155 the title picture handed over: the title page's button shows with it, not before
bool TitlePicture(const EdenDsmodHostApi* h, void* receiver, EdenDsmodImageSink sink) {
    memset(g_title, 0, sizeof g_title);
    // 0.9.149 measured against the top screen: the game shows its background larger (1.2 x) and lower -- the planet's
    // edge at 660, not 598 -- so it is drawn 1.2 x about (960, 285): its centre at (960, 591)
    if (!TitleLayer(h, "title_background", 960, 591, true, 1.2f)) return false;
    // the globe behind the logo (r80; its centre at 960, 364 as on the top screen; 0.9.148 had 350)
    static const char* const kGlobe[] = {"logo_globe_debris_04", "logo_globe_debris_03", "logo_globe_debris_02",
                                         "logo_globe_debris_01", "logo_globe_core"};
    for (const char* g : kGlobe) TitleLayer(h, g, 960, 349, false);   // 0.9.150 a smidge higher (364)
    TitleLayer(h, "logo_title", 960, 360, false);
    sink(receiver, kTitleBgW, kTitleBgH, g_title, sizeof g_title);
    __atomic_store_n(&g_title_ready, true, __ATOMIC_RELEASE);
    return true;
}

// "@grid:<px>": the Guide Map's background tile (0.9.21): the game's camp_map_grid scaled to <px> (the
// game's cell on its 1080-line screen) and drawn the game's way -- its lines darker than the sea-blue fill
// (colours from the game's Guide Map view)
bool GridTile(const EdenDsmodHostApi* h, int px, void* receiver, EdenDsmodImageSink sink) {
    static uint8_t src[128 * 128 * 4];
    uint32_t w = 0, hh = 0;
    if (!CutSprite(h, "camp_map_grid", src, sizeof src, w, hh) || !w || !hh) return false;
    if (px < 8 || px > 256) px = 72;
    constexpr int kFill[3] = {4, 56, 81}, kLine[3] = {1, 36, 54};
    uint8_t* o = g_pic2;
    for (int y = 0; y < px; ++y)
        for (int x = 0; x < px; ++x) {
            // bilinear sample of the tile (wrapping, so the lines meet across tiles)
            const float fx = (x + 0.5f) * w / px - 0.5f, fy = (y + 0.5f) * hh / px - 0.5f;
            const int x0 = static_cast<int>(fx < 0 ? fx - 1 : fx), y0 = static_cast<int>(fy < 0 ? fy - 1 : fy);
            const float ax = fx - x0, ay = fy - y0;
            float v = 0;
            for (int k = 0; k < 4; ++k) {
                const int sx = ((x0 + (k & 1)) % static_cast<int>(w) + static_cast<int>(w)) % static_cast<int>(w);
                const int sy = ((y0 + (k >> 1)) % static_cast<int>(hh) + static_cast<int>(hh)) % static_cast<int>(hh);
                const uint8_t* q = src + (uint64_t(sy) * w + sx) * 4;
                const float lum = (q[0] + q[1] + q[2]) / (3.0f * 255) * q[3] / 255.0f;
                v += lum * ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
            }
            uint8_t* d = o + (uint64_t(y) * px + x) * 4;
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(kFill[c] + (kLine[c] - kFill[c]) * v + 0.5f);
            d[3] = 160;     // 0.9.23: a little see-through over the card's dark panel
        }
    sink(receiver, static_cast<uint32_t>(px), static_cast<uint32_t>(px), o, uint64_t(px) * px * 4);
    return true;
}

bool PictureImage(const EdenDsmodHostApi* h, const char* key, void* receiver, EdenDsmodImageSink sink) {
    if (starts(key, "@lvarc:") || starts(key, "@ill:") || starts(key, "@body:") || starts(key, "@logo") || starts(key, "@titlebg") || starts(key, "@grid:") || starts(key, "@ring:") || starts(key, "@bonus:")) {
        Lock lk(&g_file_lock);
        if (!IndexLoad(h)) { g_pic_err = 40 + g_ix.err; return false; }
        if (starts(key, "@ill:")) return Illustration(h, key + 5, receiver, sink);
        if (starts(key, "@body:")) return Body(h, key + 6, receiver, sink);    // 0.9.130
        // 0.9.145 (r80) the title screen's own pictures: its background (uiscene title_background, BC1) and logo
        if (starts(key, "@logo")) return FitSprite(h, "logo_title", kLogoW, kLogoH, 1, receiver, sink);
        if (starts(key, "@titlebg")) return TitlePicture(h, receiver, sink);   // 0.9.147 the whole title screen
        if (starts(key, "@grid:")) {
            int v = 0;
            for (const char* q = key + 6; *q >= '0' && *q <= '9'; ++q) v = v * 10 + (*q - '0');
            return GridTile(h, v, receiver, sink);
        }
        if (starts(key, "@bonus:")) {                    // 0.9.89
            // 0.9.101 "@bonus:<permille>:<count>:<next>" (the numbers drawn on it in the game's font)
            int v = 0, pt = 0, nx = 0;
            const char* q = key + 7;
            for (; *q >= '0' && *q <= '9'; ++q) v = v * 10 + (*q - '0');
            if (*q == ':') { for (++q; *q >= '0' && *q <= '9'; ++q) pt = pt * 10 + (*q - '0'); }
            if (*q == ':') { for (++q; *q >= '0' && *q <= '9'; ++q) nx = nx * 10 + (*q - '0'); }
            return BonusGauge(h, v, pt, nx, receiver, sink);
        }
        if (starts(key, "@ring:")) {
            int v = 0;
            for (const char* q = key + 6; *q >= '0' && *q <= '9'; ++q) v = v * 10 + (*q - '0');
            return AssistRing(h, v, receiver, sink);
        }
        int v = 0;
        for (const char* q = key + 7; *q >= '0' && *q <= '9'; ++q) v = v * 10 + (*q - '0');
        return LevelArc(h, v, receiver, sink);
    }
    // "@map:<picture>:<turn>": the Map page's canvas of that picture
    // "@tint:<picture>:<RRGGBB>": the picture multiplied by the colour the game draws it with (the
    // image widget's tint_bind is an index into a fixed list, so the game's colour is applied here)
    char nm[64];
    int turn = 0;
    uint32_t tint = 0;
    const bool canvas = starts(key, "@map:");
    bool narrow = false;
    const char* name = key;
    if (starts(key, "@tint:")) {
        size_t i = 0;
        for (const char* q = key + 6; *q && *q != ':' && i + 1 < sizeof nm; ++q) nm[i++] = *q;
        nm[i] = 0;
        const char* r = key + 6 + i;
        if (*r == ':')
            for (++r; *r; ++r) {
                const char c = *r;
                const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                if (d < 0) break;
                tint = tint << 4 | static_cast<uint32_t>(d);
            }
        tint |= 0xFF000000u;
        name = nm;
    }
    if (canvas) {
        size_t i = 0;
        for (const char* q = key + 5; *q && *q != ':' && i + 1 < sizeof nm; ++q) nm[i++] = *q;
        nm[i] = 0;
        const char* r = key + 5 + i;
        if (*r == ':') for (++r; *r >= '0' && *r <= '9'; ++r) turn = turn * 10 + (*r - '0');
        narrow = r[0] == ':' && r[1] == 'n';                 // "@map:<picture>:<turn>:n": the narrow map
        name = nm;
    }
    Lock lk(&g_file_lock);
    if (!IndexLoad(h)) { g_pic_err = 40 + g_ix.err; return false; }
    // 0.9.77 "@dot:<model NNNN>:<cid>": an assault member's full-body pixel character (dot_cp_NNNN, one still
    // frame of the game's assist_executable_loop, research r61); a pictures.idx without it: the party icon_dot
    if (starts(key, "@dot:")) {
        int a = 0, c = 0;
        const char* q = key + 5;
        for (; *q >= '0' && *q <= '9'; ++q) a = a * 10 + (*q - '0');
        if (*q == ':') for (++q; *q >= '0' && *q <= '9'; ++q) c = c * 10 + (*q - '0');
        Str d;
        d.add("dot_cp_").ch(static_cast<char>('0' + a / 1000 % 10)).ch(static_cast<char>('0' + a / 100 % 10))
         .ch(static_cast<char>('0' + a / 10 % 10)).ch(static_cast<char>('0' + a % 10));
        if (!FindSprite(d.s) && c > 0)
            d.clear().add("icon_dot_").ch(static_cast<char>('0' + c / 1000 % 10)).ch(static_cast<char>('0' + c / 100 % 10))
             .ch(static_cast<char>('0' + c / 10 % 10)).ch(static_cast<char>('0' + c % 10));
        size_t i = 0;
        for (; d.s[i] && i + 1 < sizeof nm; ++i) nm[i] = d.s[i];
        nm[i] = 0;
        name = nm;
    }
    const IdxSprite* s = FindSprite(name);
    if (!s) { g_pic_err = 50; return false; }
    const IdxTex* t = FindTex(s->tex);
    if (!t) { g_pic_err = 51; return false; }
    // 0.9.117 here too: the page's pictures come this way (0.9.115/116 only fixed CutSprite's: diag icons fix -1)
    const bool whole = MapPicture(name);
    uint32_t w = whole ? t->w : s->w, hh = whole ? t->h : s->h, step = 1;
    float land_fx = 1, land_fy = 1;
    while (static_cast<uint64_t>(w / step) * (hh / step) > kPicMax && step < 8) step *= 2;
    if (step > 1 && s->rot) { g_pic_err = 52; return false; }
    if (!DecodeRect(h, *t, whole ? 0 : s->x, whole ? 0 : s->y, w, hh, step, whole)) return false;
    w /= step; hh /= step;
    Rotate(s->rot, w, hh);
    if (!canvas && uint64_t{w} * hh > kPicOut) {   // scaled to what Eden Duo accepts (nearest, in place)
        uint32_t ow = w, oh = hh;
        while (uint64_t{ow} * oh > kPicOut) { ow = ow * 63 / 64; oh = oh * 63 / 64; }
        for (uint32_t y = 0; y < oh; ++y) {
            const uint32_t sy = static_cast<uint32_t>(uint64_t{y} * hh / oh);
            for (uint32_t x = 0; x < ow; ++x) {
                const uint32_t sx = static_cast<uint32_t>(uint64_t{x} * w / ow);
                memmove(g_pic + (uint64_t{y} * ow + x) * 4, g_pic + (uint64_t{sy} * w + sx) * 4, 4);
            }
        }
        step = 0;                       // the land box below is measured on the scaled picture
        land_fx = static_cast<float>(w) / ow; land_fy = static_cast<float>(hh) / oh;
        w = ow; hh = oh;
    }
    if (starts(name, "minimap_mf_") && __atomic_load_n(&g_area_tint_ok, __ATOMIC_ACQUIRE)) {
        int k[4];
        for (int c = 0; c < 4; ++c) k[c] = static_cast<int>(g_area_tint[c] * 256.0f + 0.5f);
        for (uint64_t i = 0, n = uint64_t{w} * hh * 4; i < n; i += 4)
            for (int c = 0; c < 4; ++c) g_pic[i + c] = static_cast<uint8_t>((g_pic[i + c] * k[c]) >> 8);
    }
    // 0.9.77 a pixel character: on a fixed 80 x 80 stage (the frames are up to 74 x 66), feet on the bottom middle,
    // then x3 with nearest pixels -- every character at the game's own scale to the others, and crisp
    if (starts(name, "dot_cp_") && w <= 80 && hh <= 80) {
        constexpr uint32_t S = 80, K = 3;
        static uint8_t stage[S * S * 4];
        memset(stage, 0, sizeof stage);
        const uint32_t ox = (S - w) / 2, oy = S - hh;
        // 0.9.80 mirrored, facing right as the game draws them on its battle screen (UIDotCharacterPresenter.isReverse)
        for (uint32_t y = 0; y < hh; ++y)
            for (uint32_t x = 0; x < w; ++x)
                memcpy(stage + ((uint64_t{oy + y} * S) + ox + (w - 1 - x)) * 4, g_pic + (uint64_t{y} * w + x) * 4, 4);
        for (uint32_t y = 0; y < S * K; ++y)
            for (uint32_t x = 0; x < S * K; ++x)
                memcpy(g_pic + (uint64_t{y} * S * K + x) * 4, stage + (uint64_t{y / K} * S + x / K) * 4, 4);
        w = hh = S * K;
    }
    // 0.9.22 the page's map: an area's picture is drawn solid (the game draws it see-through over its own dark
    // backdrop; over the Guide Map grid it read as faded) -- edges keep a little softness
    if (canvas && starts(name, "minimap_"))
        for (uint64_t i = 3, n = uint64_t{w} * hh * 4; i < n; i += 4)
            g_pic[i] = g_pic[i] >= 85 ? 255 : static_cast<uint8_t>(g_pic[i] * 3);
    const int wm = eq(name, "map_expel") ? 0 : eq(name, "map_nede") ? 1 : -1;
    if (wm >= 0) {                  // the land: bright, warm pixels (the sea and its contour lines are blue)
        uint32_t x0 = w, y0 = hh, x1 = 0, y1 = 0;
        for (uint32_t y = 0; y < hh; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* q = g_pic + (uint64_t{y} * w + x) * 4;
                if (q[3] > 128 && q[0] > 110 && q[0] > q[2] + 20) {
                    if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
                }
            }
        if (x1 > x0 && y1 > y0) {
            const float fx = step ? static_cast<float>(step) : land_fx, fy = step ? static_cast<float>(step) : land_fy;
            LandBox b{x0 * fx, (hh - 1 - y1) * fy, (x1 + 1) * fx, (hh - y0) * fy, 1};
            g_land[wm] = b;
        }
    }
    if (starts(name, "minimap_") && step == 1) {   // the area's drawn part (anything not see-through)
        uint32_t x0 = w, y0 = hh, x1 = 0, y1 = 0;
        for (uint32_t y = 0; y < hh; ++y)
            for (uint32_t x = 0; x < w; ++x)
                if (g_pic[(uint64_t{y} * w + x) * 4 + 3] > 24) {
                    if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
                }
        if (x1 > x0 && y1 > y0) {
            __atomic_add_fetch(&g_abox_seq, 1, __ATOMIC_ACQ_REL);
            int slot = 0;
            while (slot < g_abox_n && !eq(g_abox[slot].name, name)) ++slot;
            if (slot == g_abox_n) { if (g_abox_n < 16) ++g_abox_n; else slot = 0; }
            AreaBox& e = g_abox[slot];
            size_t i = 0; for (; name[i] && i + 1 < sizeof e.name; ++i) e.name[i] = name[i];
            e.name[i] = 0;
            e.b = {static_cast<float>(x0), static_cast<float>(hh - 1 - y1), static_cast<float>(x1 + 1),
                        static_cast<float>(hh - y0), 1};
            __atomic_add_fetch(&g_abox_seq, 1, __ATOMIC_ACQ_REL);
        }
    }
    if (canvas) {
        if (step != 1) { g_pic_err = 53; return false; }
        const MapCanvas c = MakeCanvas(static_cast<float>(w), static_cast<float>(hh), static_cast<float>(turn), narrow,
                                       starts(name, "minimap_") ? 2.0f : 1.0f);
        // small pictures (town minimaps) are enlarged smoothly up to what Eden Duo takes, so a zoomed
        // town is as sharp as the big world map (the game's own minimap is drawn filtered the same way)
        float sc = SqrtF(static_cast<float>(kPicOut) / (c.cw * c.ch));
        if (sc > 4) sc = 4;
        if (c.cw * sc > 4096) sc = 4096 / c.cw;
        if (c.ch * sc > 4096) sc = 4096 / c.ch;
        const uint32_t ow = static_cast<uint32_t>(c.cw * sc), oh = static_cast<uint32_t>(c.ch * sc);
        if (!ow || !oh || uint64_t{ow} * oh * 4 > sizeof g_pic2) { g_pic_err = 54; return false; }
        const float inv = 1.0f / sc;
        // 0.9.23 an area picture's outline, enlarged: its alpha softened over 3 x 3 source pixels and then
        // sharpened again (smoothstep), so the edges come out as smooth curves instead of enlarged steps
        const bool smooth = starts(name, "minimap_") && sc > 1.01f && uint64_t{w} * hh <= sizeof g_blur;
        if (smooth)
            for (uint32_t y = 0; y < hh; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    uint32_t sum = 0, n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = static_cast<int>(x) + dx, yy = static_cast<int>(y) + dy;
                            const uint8_t a = xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(hh)
                                                  ? 0 : g_pic[(uint64_t(yy) * w + xx) * 4 + 3];
                            const uint32_t wgt = (dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1);
                            sum += a * wgt; n += wgt;
                        }
                    g_blur[uint64_t{y} * w + x] = static_cast<uint8_t>(sum / n);
                }
        uint8_t last[3] = {200, 200, 200};
        for (uint32_t oy = 0; oy < oh; ++oy) {
            for (uint32_t ox = 0; ox < ow; ++ox) {
                // canvas pixel -> source pixel: undo the clockwise turn
                const float rx = (ox + 0.5f) * inv - c.cw / 2, ry = (oy + 0.5f) * inv - c.ch / 2;
                const float px = rx * c.cs + ry * c.sn + c.w / 2, py = -rx * c.sn + ry * c.cs + c.h / 2;
                uint8_t* o = g_pic2 + (uint64_t{oy} * ow + ox) * 4;
                if (!(px >= 0 && py >= 0 && px < c.w && py < c.h)) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
                if (sc <= 1.01f) {
                    memcpy(o, g_pic + (uint64_t(static_cast<uint32_t>(py)) * w + static_cast<uint32_t>(px)) * 4, 4);
                    continue;
                }
                // bilinear, weighted by alpha (see-through pixels give no colour to the edge)
                float fx = px - 0.5f, fy = py - 0.5f;
                if (fx < 0) fx = 0; if (fy < 0) fy = 0;
                uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy);
                if (x0 >= w) x0 = w - 1; if (y0 >= hh) y0 = hh - 1;
                const uint32_t x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < hh ? y0 + 1 : y0;
                const float ax = fx - x0, ay = fy - y0;
                const float wt[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
                const uint8_t* q[4] = {g_pic + (uint64_t{y0} * w + x0) * 4, g_pic + (uint64_t{y0} * w + x1) * 4,
                                       g_pic + (uint64_t{y1} * w + x0) * 4, g_pic + (uint64_t{y1} * w + x1) * 4};
                float a = 0, r = 0, gg = 0, b = 0;
                for (int k = 0; k < 4; ++k) {
                    const float wa = wt[k] * q[k][3];
                    a += wa; r += wa * q[k][0]; gg += wa * q[k][1]; b += wa * q[k][2];
                }
                if (smooth) {
                    const float ab = (g_blur[uint64_t{y0} * w + x0] * wt[0] + g_blur[uint64_t{y0} * w + x1] * wt[1] +
                                      g_blur[uint64_t{y1} * w + x0] * wt[2] + g_blur[uint64_t{y1} * w + x1] * wt[3]) / 255.0f;
                    float t = (ab - 0.35f) / 0.3f;
                    t = t < 0 ? 0 : t > 1 ? 1 : t;
                    const float af = t * t * (3 - 2 * t);
                    if (af <= 0.004f) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
                    if (a >= 0.5f) {
                        last[0] = static_cast<uint8_t>(r / a + 0.5f); last[1] = static_cast<uint8_t>(gg / a + 0.5f);
                        last[2] = static_cast<uint8_t>(b / a + 0.5f);
                    }
                    o[0] = last[0]; o[1] = last[1]; o[2] = last[2]; o[3] = static_cast<uint8_t>(af * 255 + 0.5f);
                    continue;
                }
                if (a < 0.5f) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
                o[0] = static_cast<uint8_t>(r / a + 0.5f); o[1] = static_cast<uint8_t>(gg / a + 0.5f);
                o[2] = static_cast<uint8_t>(b / a + 0.5f); o[3] = static_cast<uint8_t>(a + 0.5f);
            }
        }
        sink(receiver, ow, oh, g_pic2, uint64_t{ow} * oh * 4);
        return true;
    }
    if (tint) {
        const uint32_t tr = tint >> 16 & 0xFF, tg = tint >> 8 & 0xFF, tb = tint & 0xFF;
        for (uint64_t i = 0, n = uint64_t{w} * hh; i < n; ++i) {
            uint8_t* p = g_pic + i * 4;
            p[0] = static_cast<uint8_t>(p[0] * tr / 255); p[1] = static_cast<uint8_t>(p[1] * tg / 255);
            p[2] = static_cast<uint8_t>(p[2] * tb / 255);
        }
    }
    sink(receiver, w, hh, g_pic, uint64_t{w} * hh * 4);
    return true;
}

// ---------------------------------------------------------------- the game's font
// TMP font asset (so2r_font_ja_en, FOT-Chiaro Std B): the glyphs of U+0020..U+00FF are cut out
// of its Alpha8 SDF atlas (SDF -> coverage with a 2-pixel edge) and packed into one picture;
// the metrics go to the host through decode_font.
constexpr uint32_t kFontFirst = 0x20, kFontLast = 0xFF, kFontN = kFontLast - kFontFirst + 1;
constexpr uint32_t kAtlasW = 1024, kAtlasH = 1024, kMargin = 2;   // 0.9.53 1024 tall: one cell height for all
struct FontState {
    bool tried, ok; int err;
    uint32_t line;
    EdenDsmodFontGlyph g[kFontN];
    int found;
    bool uniform;          // 0.9.53 every glyph in one cell height
};
FontState g_font;
uint8_t g_fatlas[kAtlasW * kAtlasH * 4];

// under g_file_lock
bool FontBuild(const EdenDsmodHostApi* h) {
    FontState& F = g_font;
    if (F.ok) return true;
    if (!IndexLoad(h)) { F.err = 60 + g_ix.err; return false; }
    const IdxFont& f = g_ix.f;
    if (!f.ok) { F.err = 70; return false; }
    OpenBundle* ob = Open(h, f.bundle);
    const IdxTex* t = FindTex(f.tex);
    if (!ob || !t || t->fmt != 1) { F.err = 71; return false; }
    // codepoint -> glyph index (character table: element type, unicode, glyph index, scale)
    uint32_t gidx[kFontN];
    for (auto& v : gidx) v = ~0u;
    uint8_t buf[4096];
    for (uint64_t i = 0; i < f.cn;) {
        const uint64_t per = sizeof buf / f.c_st, n = f.cn - i < per ? f.cn - i : per;
        if (!StreamRead(h, ob, f.obj + f.c_at + i * f.c_st, buf, n * f.c_st)) { F.err = 72; return false; }
        for (uint64_t k = 0; k < n; ++k) {
            uint32_t u, gi; memcpy(&u, buf + k * f.c_st + 4, 4); memcpy(&gi, buf + k * f.c_st + 8, 4);
            if (u >= kFontFirst && u <= kFontLast) gidx[u - kFontFirst] = gi;
        }
        i += n;
    }
    // glyph records: index, metrics (w, h, bearing x, bearing y, advance), rect (x, y, w, h)
    struct G { float bx, by, adv; int32_t x, y, w, h; bool have; } gl[kFontN];
    memset(gl, 0, sizeof gl);
    for (uint64_t i = 0; i < f.gn;) {
        const uint64_t per = sizeof buf / f.g_st, n = f.gn - i < per ? f.gn - i : per;
        if (!StreamRead(h, ob, f.obj + f.g_at + i * f.g_st, buf, n * f.g_st)) { F.err = 73; return false; }
        for (uint64_t k = 0; k < n; ++k) {
            const uint8_t* r = buf + k * f.g_st;
            uint32_t idx; memcpy(&idx, r, 4);
            for (uint32_t c = 0; c < kFontN; ++c) {
                if (gidx[c] != idx || gl[c].have) continue;
                memcpy(&gl[c].bx, r + 12, 4); memcpy(&gl[c].by, r + 16, 4); memcpy(&gl[c].adv, r + 20, 4);
                memcpy(&gl[c].x, r + 24, 4); memcpy(&gl[c].y, r + 28, 4);
                memcpy(&gl[c].w, r + 32, 4); memcpy(&gl[c].h, r + 36, 4);
                gl[c].have = true;
            }
        }
        i += n;
    }
    // 0.9.53 every glyph in a cell of ONE height, its letter at its own place in it: the host places a glyph at
    // floor(baseline - bearing_y x ratio) and scales it to floor(h x ratio) rows, so glyphs of different boxes
    // came out up to 2 screen px apart (0.9.52 on the Thor: "uid" of "Guide Map" below the "G" and "e"). With
    // the same bearing_y and h for all, every glyph gets the same rows. If the cells do not fit, the glyphs'
    // own boxes as before.
    int32_t top = 0, bot = 0;                      // the highest glyph top above / lowest bottom below the baseline
    for (uint32_t c = 0; c < kFontN; ++c) {
        const G& q = gl[c];
        if (!q.have || q.w <= 0 || q.h <= 0) continue;
        const int32_t t = RoundF(q.by), b = q.h - t;
        if (t > top) top = t;
        if (b > bot) bot = b;
    }
    const int32_t cell = top + bot + 2 * static_cast<int32_t>(kMargin);
    int found = 0;
    for (int uniform = cell > 0 && cell <= 128 ? 1 : 0; uniform >= 0; --uniform) {
        memset(g_fatlas, 0, sizeof g_fatlas);
        memset(F.g, 0, sizeof F.g);
        uint32_t px = 0, py = 0, rowh = 0;
        found = 0;
        bool fits = true;
        for (uint32_t c = 0; c < kFontN && fits; ++c) {
            const G& q = gl[c];
            if (!q.have) continue;
            EdenDsmodFontGlyph& o = F.g[c];
            o.advance = static_cast<uint16_t>(q.adv > 0 ? RoundF(q.adv) : 0);
            ++found;
            if (q.w <= 0 || q.h <= 0) continue;          // a space
            // the glyph's box plus a margin, clamped to the atlas
            const int32_t x0 = q.x - static_cast<int32_t>(kMargin) < 0 ? 0 : q.x - static_cast<int32_t>(kMargin);
            const int32_t y0 = q.y - static_cast<int32_t>(kMargin) < 0 ? 0 : q.y - static_cast<int32_t>(kMargin);
            int32_t x1 = q.x + q.w + static_cast<int32_t>(kMargin), y1 = q.y + q.h + static_cast<int32_t>(kMargin);
            if (x1 > static_cast<int32_t>(t->w)) x1 = static_cast<int32_t>(t->w);
            if (y1 > static_cast<int32_t>(t->h)) y1 = static_cast<int32_t>(t->h);
            const uint32_t w = static_cast<uint32_t>(x1 - x0), hh = static_cast<uint32_t>(y1 - y0);
            if (w > 128 || hh > 128) { F.err = 74; return false; }
            // its rows in the cell: the cell's top is top + margin above the baseline; the box's top is
            // round(bearing y) + its top margin above it
            const int32_t above = RoundF(q.by) + (y1 - (q.y + q.h));
            const uint32_t at = uniform ? static_cast<uint32_t>(top + static_cast<int32_t>(kMargin) - above) : 0;
            const uint32_t ch = uniform ? static_cast<uint32_t>(cell) : hh;
            if (uniform && at + hh > ch) { fits = false; break; }
            if (px + w + 1 > kAtlasW) { px = 0; py += rowh + 1; rowh = 0; }
            if (py + ch > kAtlasH) { if (uniform) { fits = false; break; } F.err = 75; return false; }
            if (!DecodeRect(h, *t, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), w, hh)) { F.err = 76; return false; }
            for (uint32_t r = 0; r < hh; ++r)
                for (uint32_t k = 0; k < w; ++k) {
                    const int v = g_pic[(r * w + k) * 4 + 3];
                    int a = (v - 128) * 6 + 128;           // SDF: 128 is the edge; about 2 px of ramp
                    a = a < 0 ? 0 : a > 255 ? 255 : a;
                    uint8_t* d = g_fatlas + ((uint64_t{py} + at + r) * kAtlasW + px + k) * 4;
                    d[0] = d[1] = d[2] = 255; d[3] = static_cast<uint8_t>(a);
                }
            o.x = static_cast<uint16_t>(px); o.y = static_cast<uint16_t>(py);
            o.w = static_cast<uint16_t>(w); o.h = static_cast<uint16_t>(ch);
            o.bearing_x = static_cast<int16_t>(RoundF(q.bx) - (q.x - x0));
            o.bearing_y = static_cast<int16_t>(uniform ? top + static_cast<int32_t>(kMargin) : above);
            px += w + 1;
            if (ch > rowh) rowh = ch;
        }
        F.uniform = uniform && fits;
        if (fits) break;
    }
    if (found < 90) { F.err = 77; return false; }  // ASCII at least
    F.line = static_cast<uint32_t>(RoundF(static_cast<float>(f.ascent100) / 100.0f));
    if (F.line < 8) F.line = 32;
    F.found = found;
    F.ok = true;
    F.err = 0;
    return true;
}

// "module:fontdata": SO2F, line height, first codepoint, count, then the glyph records
uint8_t g_fdata[16 + kFontN * sizeof(EdenDsmodFontGlyph)];
EdenDsmodBool LoadData(void*, const EdenDsmodHostApi* host, const char* key, void* receiver, EdenDsmodDataSink sink) {
    if (!host || !sink || !eq(key, "module:fontdata")) return EDEN_DSMOD_FALSE;
    Lock lk(&g_file_lock);
    if (!FontBuild(host)) return EDEN_DSMOD_FALSE;
    memcpy(g_fdata, "SO2F", 4);
    const uint32_t hdr[3] = {g_font.line, kFontFirst, kFontN};
    memcpy(g_fdata + 4, hdr, sizeof hdr);
    memcpy(g_fdata + 16, g_font.g, sizeof g_font.g);
    sink(receiver, g_fdata, sizeof g_fdata);
    return EDEN_DSMOD_TRUE;
}
EdenDsmodBool DecodeFont(void*, const uint8_t* bytes, size_t size, void* receiver, EdenDsmodFontSink sink) {
    if (!bytes || !sink || size != sizeof g_fdata || !starts(reinterpret_cast<const char*>(bytes), "SO2F")) return EDEN_DSMOD_FALSE;
    uint32_t hdr[3]; memcpy(hdr, bytes + 4, sizeof hdr);
    if (hdr[1] != kFontFirst || hdr[2] != kFontN) return EDEN_DSMOD_FALSE;
    EdenDsmodFontGlyph gl[kFontN];
    memcpy(gl, bytes + 16, sizeof gl);
    sink(receiver, hdr[0], hdr[1], gl, hdr[2]);
    return EDEN_DSMOD_TRUE;
}
bool FontAtlas(const EdenDsmodHostApi* h, void* receiver, EdenDsmodImageSink sink) {
    Lock lk(&g_file_lock);
    if (!FontBuild(h)) return false;
    sink(receiver, kAtlasW, kAtlasH, g_fatlas, sizeof g_fatlas);
    return true;
}

EdenDsmodBool LoadImage(void*, const EdenDsmodHostApi* host, const char* key, void* receiver, EdenDsmodImageSink sink) {
    if (!host || !key || !sink || !host->read_romfs) return EDEN_DSMOD_FALSE;
    bool ok = false;
    if (eq(key, "module:font")) ok = FontAtlas(host, receiver, sink);
    else if (starts(key, "module:pic/")) ok = PictureImage(host, key + 11, receiver, sink);
    else return EDEN_DSMOD_FALSE;
    __atomic_add_fetch(ok ? &g_pics_drawn : &g_pics_failed, 1, __ATOMIC_RELAXED);
    return ok ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}


// ---------------------------------------------------------------- the map
// The game's own minimap numbers, read live: the area minimap scrolls under the player by
// UIScrollMinimapBackgroundTask.UpdatePosition (main+0x259E700): background at
// -(position.xz + mapOffset) * coefficient, so the player sits at (position.xz + mapOffset) * k
// pixels from the picture's centre, k = GameDefine.worldToMinimapCoffiecient * minimapScale
// (UIMinimapPresenterBase.GetBaseWorldToMinimapCoffiecient, main+0x2561D20). The world map:
// UIWorldMinimapPresenter (backgroundSize 560x440 * worldMinimapScale, icon root at the world's
// offset, k = worldToWorldMinimapCoffiecient * minimapScale * worldMinimapScale), on the
// 2800x2200 world-map picture.
// degrees clockwise from +z (map up) of the direction (x, z); about 0.1 degree accurate
float Atan2Deg(float x, float z) {
    const float ax = x < 0 ? -x : x, az = z < 0 ? -z : z;
    const float mn = ax < az ? ax : az, mx = ax < az ? az : ax;
    const float a = mn / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;   // atan(a), radians
    if (ax > az) r = 1.57079637f - r;
    if (z < 0) r = 3.14159274f - r;
    if (x < 0) r = -r;
    return r * 57.2957795f;
}

uint64_t Singleton(const Instance& in, const Guest& g, uint64_t slot) {
    const uint64_t k = g.ptr(in.main_base + slot);
    if (!Guest::objp(k)) return 0;
    const uint64_t st = g.ptr(k + kClassStatics);
    return Guest::objp(st) ? g.ptr(st) : 0;
}
uint64_t DictInt(const Guest& g, uint64_t dict, int32_t key) {
    if (!Guest::objp(dict)) return 0;
    const uint64_t ent = g.ptr(dict + kDictEntries);
    bool ok = false;
    const int32_t n = g.i32(dict + kDictCount, &ok);
    if (!ok || n < 0 || n > 256 || !Guest::objp(ent)) return 0;
    for (int32_t i = 0; i < n; ++i) {
        const uint64_t e = ent + kArrItems + static_cast<uint64_t>(i) * kEntrySize;
        if (g.i32(e) >= 0 && g.i32(e + kEntryKey) == key) return g.ptr(e + kEntryValue);
    }
    return 0;
}
void Lower(char* d, const char* s, size_t cap) {
    size_t k = 0;
    for (; s[k] && k + 1 < cap; ++k) d[k] = (s[k] >= 'A' && s[k] <= 'Z') ? static_cast<char>(s[k] + 32) : s[k];
    d[k] = 0;
}
bool g_map_file_bad;
// Picture sizes for the map, cached so the game's thread almost never touches the file lock (the
// picture worker holds it while it cuts a big map). 1 = known, 0 = no such picture, -1 = busy.
struct SizeCache { char name[48]; float w, h; int ok; bool bad; };
SizeCache g_sizes[16];
int g_sizes_next;
int PictureSize(const EdenDsmodHostApi* h, const char* name, float& w, float& hh) {
    for (const auto& c : g_sizes)
        if (c.name[0] && eq(c.name, name)) { w = c.w; hh = c.h; g_map_file_bad = c.bad; return c.ok; }
    Lock lk(&g_file_lock, Lock::Try{});
    if (!lk.held()) return -1;
    SizeCache& c = g_sizes[g_sizes_next++ % 16];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof c.name; ++i) c.name[i] = name[i];
    c.name[i] = 0;
    c.ok = 0; c.bad = false; c.w = c.h = 0;
    if (IndexLoad(h)) {
        const IdxSprite* s = FindSprite(name);
        const IdxTex* t = s ? FindTex(s->tex) : nullptr;
        if (s && t) {
            const bool whole = MapPicture(name);
            c.w = static_cast<float>(whole ? t->w : s->w); c.h = static_cast<float>(whole ? t->h : s->h);
            if (Open(h, t->bundle)) c.ok = 1; else c.bad = true;    // the game file is not the indexed one
        }
    }
    w = c.w; hh = c.h; g_map_file_bad = c.bad;
    return c.ok;
}

// a world position -> picture pixels (y up), as the game places it
bool PlaceOnPicture(const MapState& m, float wx, float wy, float wz, float& x, float& y) {
    if (m.pmode == 1) { x = m.w / 2 + (wx + m.pox) * m.pk; y = m.h / 2 + (wz + m.poy) * m.pk; return true; }
    if (m.pmode == 2) {
        const float* pv = m.pv;
        const float cx = pv[0] * wx + pv[4] * wy + pv[8] * wz + pv[12];
        const float cy = pv[1] * wx + pv[5] * wy + pv[9] * wz + pv[13];
        const float cw = pv[3] * wx + pv[7] * wy + pv[11] * wz + pv[15];
        if (cw < 1e-6f && cw > -1e-6f) return false;
        const float sx = (cx / cw + 1) * 0.5f * m.ptw - m.ptw * 0.5f, sy = (cy / cw + 1) * 0.5f * m.pth - m.pth * 0.5f;
        x = m.w / 2 + sx * (m.w / m.ptw); y = m.h / 2 + sy * (m.h / m.pth);
        return x == x && y == y;
    }
    if (m.pmode == 3) { x = m.w / 2 + (wx * m.pk + m.pox) * m.pf; y = m.h / 2 + (wz * m.pk + m.poy) * m.pf; return true; }
    return false;
}

// The game's own minimap icons of one presenter, placed on the picture. Seen on the Thor:
// - a followed icon's cachePosition (+0x50) is its place relative to the player, in minimap pixels
//   (x right, y up), parked at (-760, 340) while off the game's minimap (0.4.1);
// - the icon's own Unity transform holds the same number as its local position (0.4.3: lp == cp), and
//   the world map's town / dungeon icons (no follow task) keep theirs only there;
// - but an icon the game stops updating keeps its last number, so it slides along with the player
//   (0.4.2, 0.4.3). So a fixed icon is drawn only once CONFIRMED: after the player has moved a few
//   picture pixels, its picture position must have stayed put. Moving ones (party, enemies) are live.
constexpr uint64_t kFtCoeff = 0x40;
bool StaticIcon(int t) { return t != 2 && t != 3 && t != 4; }   // not friend / enemy / NPC
bool ShownIcon(int t) { return t != 1 && t != 4 && t != 25 && t != 27; }   // not the player, NPCs, CANNOT_PASS, LIGHT
float g_whole;           // the world map's round minimap: picture pixels per minimap pixel (from the player icon)
struct IconMemo { uint64_t ip; int type; bool conf; float ax, ay, apx, apy, x, y; uint32_t argb; int seen; int miss; };
float Dist2(float ax, float ay, float bx, float by) { return (ax - bx) * (ax - bx) + (ay - by) * (ay - by); }

// ---- 0.9.38 marker state (pure; each piece is unit-tested in so2r_marks_test.cpp) ----------------------------
// Whether the game's minimap is shown: 0.9.39 from the game's own UIFieldController.minimapState (r46: Normal 0,
// Partial 1, Hide 2); not readable or another value: unknown. (0.9.38 took it from the icons' colour; those counts
// stay in the diag only.)
enum MiniState { kMiniUnknown = -1, kMiniOff = 0, kMiniOn = 1 };
int MiniFromState(int st) { return st == 0 || st == 1 ? kMiniOn : st == 2 ? kMiniOff : kMiniUnknown; }
[[maybe_unused]] const char* StateName(int st) { return st == 0 ? "Normal" : st == 1 ? "Partial" : st == 2 ? "Hide" : "?"; }
[[maybe_unused]] const char* MiniName(int s) { return s == kMiniOn ? "on" : s == kMiniOff ? "off" : "unknown"; }

// The world map's icon lists place their icons at picture = C + local x k, local being the icon's position in
// the game's minimap (fixed per icon: 0.9.36 on the Thor, the same four icons read bit for bit alike with the
// minimap off and on). Up to 0.9.37, C came from the player's own icon in that list every pass (C = player -
// player icon x k); with the minimap off that icon stops following the player, so every icon moved by the same
// amount (0.9.37 on the Thor: about (-300, -66) bottom-screen px on all of them). Now C is only taken once
// VERIFIED: the player's icon is updating, and C = player - icon x k stays the same while the player moves 6
// picture px. The verified C (and its k) place the icons; a pass whose C disagrees does not change it.
struct ListGeo {
    bool ok; float k, cx, cy;             // verified: picture = (cx, cy) + local x k
    bool anc; float ax, ay, apx, apy;     // a candidate C and where the player was when it was seen
    bool last; float lpx, lpy, lx, ly;    // the last pass: player icon (minimap px), player (picture px)
    int fresh;                            // the player's icon: 1 updating, -1 left behind, 0 not known yet
    int why;
    int steady;                           // 0.9.39 passes C has held while the game's minimap is shown
};
enum GeoWhy { kGeoNone, kGeoNoPlayer, kGeoAgrees, kGeoVerified, kGeoReplaced, kGeoWaiting, kGeoStale, kGeoByState, kGeoHidden, kGeoPictures };
[[maybe_unused]] const char* GeoWhyName(int w) {
    static const char* n[] = {"-", "no player icon", "agrees", "verified by a move", "re-verified", "waiting",
                              "player icon stale", "verified: minimap shown, steady", "minimap hidden", "from the game's pictures"};
    return w >= 0 && w < 10 ? n[w] : "?";
}
// is the player's icon following the player? (it changed: yes; the player moved 1 px and it did not: no)
void GeoTrack(ListGeo& G, float plx, float ply, float px, float py) {
    if (G.last) {
        if (Dist2(plx, ply, G.lpx, G.lpy) > 1e-6f) G.fresh = 1;
        else if (Dist2(px, py, G.lx, G.ly) > 1.0f) G.fresh = -1;
    }
    G.last = true; G.lpx = plx; G.lpy = ply; G.lx = px; G.ly = py;
}
// shown: the game's minimap state says shown (Normal / Partial) for 10 passes running and this list has icons in
// use -- the list the game is drawing (0.9.36 on the Thor: the round list has icons only in the round view, the
// whole-world one only in the whole-world view and with the minimap off). Its player icon is then the game's own
// live one, so a C that holds 10 passes is taken without a move (0.9.38 waited for one: no icons at load).
// hidden: the state says Hide -- the player's icon is not trusted (0.9.37: it stops following).
void GeoVerify(ListGeo& G, float plx, float ply, float px, float py, float k, bool shown = false, bool hidden = false) {
    const float cx = px - plx * k, cy = py - ply * k;
    if (G.ok && AbsF(k - G.k) <= 0.02f * G.k && Dist2(cx, cy, G.cx, G.cy) <= 4.0f * 4.0f) { G.why = kGeoAgrees; G.anc = false; G.steady = 0; return; }
    if (hidden) { G.why = kGeoHidden; G.anc = false; G.steady = 0; return; }
    if (G.fresh < 0 && !shown) { G.why = kGeoStale; G.anc = false; G.steady = 0; return; }
    if (!G.anc || Dist2(cx, cy, G.ax, G.ay) > 3.0f * 3.0f) {
        G.anc = true; G.ax = cx; G.ay = cy; G.apx = px; G.apy = py; G.why = kGeoWaiting; G.steady = 0; return;
    }
    if (shown && Dist2(cx, cy, G.ax, G.ay) <= 1.0f && ++G.steady >= 10) {
        G.why = G.ok ? kGeoReplaced : kGeoByState;
        G.ok = true; G.k = k; G.cx = cx; G.cy = cy; G.anc = false; G.steady = 0;
        return;
    }
    if (Dist2(px, py, G.apx, G.apy) < 36.0f || G.fresh <= 0) { G.why = kGeoWaiting; return; }
    G.why = G.ok ? kGeoReplaced : kGeoVerified;
    G.ok = true; G.k = k; G.cx = cx; G.cy = cy; G.anc = false;
}
// per world map picture and list (1: round, 2: whole world); kept across scenes (the game's layout)
struct WorldGeo { char area[48]; ListGeo g[3]; };
// 0.9.51 a slot only for a world map (map_...), looked up without taking one by the diag page: 0.9.50 on the Thor
// the diag line took the two slots for the towns it was shown in, so the world map got none and its icons were
// never placed (the "!" was, from its marked object). Four slots: the game has two world maps.
WorldGeo g_wgeo[4];
[[maybe_unused]] const ListGeo* GeoFind(const char* area, int which) {
    if (which < 1 || which > 2) return nullptr;
    for (const WorldGeo& w : g_wgeo) if (w.area[0] && eq(w.area, area)) return &w.g[which];
    return nullptr;
}
ListGeo* GeoFor(const char* area, int which) {
    if (which < 1 || which > 2) return nullptr;
    for (WorldGeo& w : g_wgeo) if (w.area[0] && eq(w.area, area)) return &w.g[which];
    if (!(area[0] == 'm' && area[1] == 'a' && area[2] == 'p' && area[3] == '_')) return nullptr;
    for (WorldGeo& w : g_wgeo) if (!w.area[0]) {
        size_t i = 0; for (; area[i] && i + 1 < sizeof w.area; ++i) w.area[i] = area[i];
        w.area[i] = 0;
        return &w.g[which];
    }
    return nullptr;
}

// An objective's identity: the Guide Map's destination row (r44) with the scenario progress it was read at, and
// the save (UserParameter). The "!" icon's own object or pointer is no identity: the game rebuilds it.
struct ObjId { int row, prog; uint64_t save; };
bool SameObj(const ObjId& a, const ObjId& b) { return a.row == b.row && a.prog == b.prog && a.save == b.save; }

// 0.9.38 the destination "!" of an area (floor). Its position comes from (best first) its object's world
// position, else the game's minimap: the follow task's cached offset from the player, or the icon's own place
// relative to the player's icon -- those two are UI numbers that go stale (they slide with the player), so a
// position from them is only taken once it stays put while the player moves 6 picture px. A verified position
// is kept for its objective and floor: through the minimap being turned off and on, the icon being rebuilt,
// and readings that disagree (shown again only when a NEW position verifies). A new objective, floor or save
// starts over.
enum DestSrc { kSrcNone, kSrcWorld, kSrcFollow, kSrcLocal, kSrcTable };
enum DestWhy { kDwNone, kDwNewId, kDwWorld, kDwAgrees, kDwVerified, kDwReplaced, kDwWaiting, kDwSlid, kDwKept, kDwHidden, kDwNoObj, kDwTable };
[[maybe_unused]] const char* DestWhyName(int w) {
    static const char* n[] = {"-", "new objective/floor/save", "world position", "agrees", "verified by a move",
                              "moved: new position verified", "waiting for a move", "slid with the player: stale",
                              "kept (no reading)", "kept (icon see-through)", "no objective", "the game's table position"};
    return w >= 0 && w < 12 ? n[w] : "?";
}
[[maybe_unused]] const char* DestSrcName(int s) {
    static const char* n[] = {"none", "world", "follow cache", "UI local", "table"};
    return s >= 0 && s < 5 ? n[s] : "?";
}
struct DestTrack {
    char area[48]; ObjId id; bool idset;
    bool seen;                                           // 0.9.40 the game has shown a "!" for this objective here
    bool ok; float x, y; int src; uint32_t argb;        // the verified position
    bool cand; float cx, cy, apx, apy;                   // a reading being verified
    int why, rsrc;                                       // this pass: what was decided, the reading's source
};
DestTrack g_dest;
void DestIdentity(DestTrack& d, const char* area, const ObjId& id) {
    if (d.idset && eq(d.area, area) && SameObj(d.id, id)) return;
    size_t i = 0; for (; area[i] && i + 1 < sizeof d.area; ++i) d.area[i] = area[i];
    d.area[i] = 0;
    d.id = id; d.idset = true; d.ok = false; d.cand = false; d.seen = false; d.why = kDwNewId; d.rsrc = kSrcNone;
}
// a reading this pass (x, y picture px; the player at px, py)
void DestStep(DestTrack& d, int src, float x, float y, uint32_t argb, float px, float py) {
    d.rsrc = src;
    if (d.id.row < 0) { d.ok = false; d.cand = false; d.why = kDwNoObj; return; }
    if (src == kSrcWorld || src == kSrcTable) {
        d.ok = true; d.x = x; d.y = y; d.src = src; d.argb = argb; d.cand = false; d.why = src == kSrcTable ? kDwTable : kDwWorld;
        return;
    }
    if (d.ok && Dist2(x, y, d.x, d.y) <= 4.0f * 4.0f) { d.cand = false; d.argb = argb; d.why = kDwAgrees; return; }
    if (!d.cand) { d.cand = true; d.cx = x; d.cy = y; d.apx = px; d.apy = py; d.why = kDwWaiting; return; }
    const float m2 = Dist2(px, py, d.apx, d.apy);
    if (m2 < 36.0f) {
        // not moved 6 px yet: nothing to verify against. Standing still, a changed reading is the UI settling, so
        // the candidate starts again from it; once the player has moved at all it is kept as it was (a sliding
        // icon must be compared with where it was when the move began, 0.9.38 test)
        if (m2 < 1.0f) { d.cx = x; d.cy = y; d.apx = px; d.apy = py; }
        d.why = kDwWaiting; return;
    }
    // stayed within 2 px + a quarter of the player's move (squared): a real place, not one sliding along
    if (Dist2(x, y, d.cx, d.cy) < 8.0f + 0.125f * m2) {
        d.why = d.ok ? kDwReplaced : kDwVerified;
        d.ok = true; d.x = x; d.y = y; d.src = src; d.argb = argb; d.cand = false;
        return;
    }
    d.cx = x; d.cy = y; d.apx = px; d.apy = py; d.why = kDwSlid;
}
// 0.9.40 (r46 / r47) the game's own map destinations: ParameterManager.mapDestinationParameter (+0x708), a list of
// ConstMapDestinationParameter {fieldmapID +0x10, heroID +0x14, mapDestinationObjectType +0x18, name +0x20,
// startScenarioProgress +0x28, endScenarioProgress +0x2C, disableFlag +0x30, ..., isOverridePosition +0x44,
// position +0x48}. Seen in the table: Krosse Cave (fieldmap 33), progress 1201000-1300000, an override position
// (121.81, 6, 214.92); most other rows name an NPC or event object instead (not used yet: how the game places
// those is still unread). Used only when exactly ONE row matches this field map, hero (0: any) and progress
// (start <= progress < end, a negative bound open) and it has an override position; any other match: none.
constexpr uint64_t kPmMapDest = 0x708, kMdField = 0x10, kMdHero = 0x14, kMdStart = 0x28, kMdEnd = 0x2C, kMdOverride = 0x44, kMdPos = 0x48;
struct TableDest { bool ok; float pos[3]; int nmatch, row; int key_field, key_hero, key_prog; uint64_t key_pm; };
TableDest g_tdest;
void LookupTableDest(const Guest& g, uint64_t pm, int fieldmap, int hero, int prog) {
    TableDest& t = g_tdest;
    if (t.key_pm == pm && t.key_field == fieldmap && t.key_hero == hero && t.key_prog == prog) return;
    t = {}; t.key_pm = pm; t.key_field = fieldmap; t.key_hero = hero; t.key_prog = prog; t.row = -1;
    if (!Guest::objp(pm) || fieldmap <= 0 || prog < 0) return;
    const uint64_t sp = g.ptr(pm + kPmMapDest);
    const uint64_t list = Guest::objp(sp) ? g.ptr(sp + kMpList) : 0;
    const uint64_t items = Guest::objp(list) ? g.ptr(list + kListItems) : 0;
    const int32_t n = Guest::objp(list) ? g.i32(list + kListSize) : 0;
    if (!Guest::objp(items) || n <= 0 || n > 1024) return;
    int over_row = -1;
    for (int32_t i = 0; i < n; ++i) {
        const uint64_t r = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
        if (!Guest::objp(r) || g.i32(r + kMdField) != fieldmap) continue;
        const int h = g.i32(r + kMdHero), st = g.i32(r + kMdStart), en = g.i32(r + kMdEnd);
        if (h != 0 && h != hero) continue;
        if (!((st < 0 || st <= prog) && (en < 0 || prog < en))) continue;
        ++t.nmatch;
        if (g.u8(r + kMdOverride) & 1) { over_row = i; g.read(r + kMdPos, t.pos, sizeof t.pos); }
    }
    if (t.nmatch == 1 && over_row >= 0 && t.pos[0] == t.pos[0] && t.pos[2] == t.pos[2]) { t.ok = true; t.row = over_row; }
}

void DestNoReading(DestTrack& d, bool hidden) {
    d.rsrc = kSrcNone;
    if (d.id.row < 0) { d.ok = false; d.cand = false; d.why = kDwNoObj; return; }
    d.why = hidden ? kDwHidden : kDwKept;
}
int g_mark_tick;          // ReadMarks' pass number: a remembered icon not seen in use this pass is gone
IconMemo g_memo[128];
int g_nmemo;
// 0.9.32 the moving icons (enemies, the party) of an area: with the game's minimap off its list stops giving
// them (0.9.31 on the Thor), so the last ones read are kept with their objects and placed from those objects'
// own world positions each pass until the list gives moving icons again (an object that no longer reads is
// dropped)
struct MoverMemo { uint64_t mgo; int type; uint32_t argb; };
MoverMemo g_movers[64], g_movers_now[64];
int g_nmovers, g_nmovers_now;
[[maybe_unused]] int g_movers_used;   // diag only
#if SO2R_DIAG
struct DestRaw { uint64_t ip; int type, src; float lp[2], pli[2], coeff, cache[2], x, y; bool have_pli, hidden; };
DestRaw g_dest_raw;         // 0.9.38 the destination icon's raw numbers this pass (diag)
char g_dest_diag[160];      // 0.9.32 the destination / event icons as read (diag)
int g_type_n[32], g_type_drawn[32];   // 0.9.33: the area list's used icons by type, and how many were placed
#endif
char g_memo_area[48];
#if SO2R_DIAG
int g_nconf;
#endif

// Unity 2021.3's native transform of a component: managed +0x10 -> native component, +0x30 its
// GameObject, GameObject +0x30 its component list (first = Transform, pointer at +8), Transform +0x38
// hierarchy, +0x40 index; hierarchy +0x18 local positions (0x30 bytes each, position first), +0x20
// parent indices. Gives the local position and the parent's local position (checked on the Thor).
bool NativeLocalPos(const Guest& g, uint64_t comp, float* lp, float* pl, char* go, size_t gon) {
    lp[0] = lp[1] = lp[2] = 0; pl[0] = pl[1] = pl[2] = 0;
    if (go && gon) go[0] = 0;
    const uint64_t nat = g.ptr(comp + 0x10);
    const uint64_t gobj = Guest::objp(nat) ? g.ptr(nat + 0x30) : 0;
    if (!Guest::objp(gobj)) return false;
    if (go && gon) {
        const uint64_t nm = g.ptr(gobj + 0x60);
        char b[24] = {};
        if (nm && g.read(nm, b, sizeof b - 1)) {
            size_t i = 0;
            for (; i + 1 < gon && i + 1 < sizeof b && b[i]; ++i) go[i] = (b[i] >= 32 && b[i] < 127) ? b[i] : '?';
            go[i] = 0;
        }
    }
    const uint64_t comps = g.ptr(gobj + 0x30);
    const uint64_t tr = Guest::objp(comps) ? g.ptr(comps + 8) : 0;
    const uint64_t hier = Guest::objp(tr) ? g.ptr(tr + 0x38) : 0;
    bool oi = false;
    const int32_t idx = Guest::objp(tr) ? g.i32(tr + 0x40, &oi) : 0;
    const uint64_t trs = Guest::objp(hier) ? g.ptr(hier + 0x18) : 0;
    if (!oi || idx < 0 || idx > 100000 || !Guest::objp(trs)) return false;
    if (!g.read(trs + static_cast<uint64_t>(idx) * 0x30, lp, 12)) return false;
    const uint64_t par = g.ptr(hier + 0x20);
    bool op = false;
    const int32_t pi = Guest::objp(par) ? g.i32(par + static_cast<uint64_t>(idx) * 4, &op) : -1;
    if (op && pi >= 0 && pi < 100000 && pi != idx) g.read(trs + static_cast<uint64_t>(pi) * 0x30, pl, 12);
    return lp[0] == lp[0] && lp[1] == lp[1];
}

// The local scale of a component's native transform (scale = floats 8..10 of its 0x30-byte TRS).
bool NativeLocalScale(const Guest& g, uint64_t comp, float* sc) {
    sc[0] = sc[1] = sc[2] = 1;
    const uint64_t nat = Guest::objp(comp) ? g.ptr(comp + 0x10) : 0;
    const uint64_t gobj = Guest::objp(nat) ? g.ptr(nat + 0x30) : 0;
    if (!Guest::objp(gobj)) return false;
    const uint64_t comps = g.ptr(gobj + 0x30);
    const uint64_t tr = Guest::objp(comps) ? g.ptr(comps + 8) : 0;
    const uint64_t hier = Guest::objp(tr) ? g.ptr(tr + 0x38) : 0;
    bool oi = false;
    const int32_t idx = Guest::objp(tr) ? g.i32(tr + 0x40, &oi) : 0;
    const uint64_t trs = Guest::objp(hier) ? g.ptr(hier + 0x18) : 0;
    if (!oi || idx < 0 || idx > 100000 || !Guest::objp(trs)) return false;
    float t[12];
    if (!g.read(trs + static_cast<uint64_t>(idx) * 0x30, t, sizeof t)) return false;
    if (!(t[8] == t[8] && t[9] == t[9] && t[8] > 0.01f && t[8] < 100 && t[9] > 0.01f && t[9] < 100)) return false;
    sc[0] = t[8]; sc[1] = t[9]; sc[2] = t[10];
    return true;
}

// The world position of a managed GameObject (its native transform's local positions walked up its
// parents: p = t + q * (s * p)). The follow task's followObject (+0x28) is the door / shop / person the
// icon stands for, so this is where it really is, however stale the icon's own number.
bool WorldPosOfNative(const Guest& g, uint64_t gobj, float* out);
bool WorldPosOf(const Guest& g, uint64_t mgo, float* out) {
    const uint64_t gobj = Guest::objp(mgo) ? g.ptr(mgo + 0x10) : 0;
    return WorldPosOfNative(g, gobj, out);
}
// 0.9.42 a managed component (a MonoBehaviour such as FieldMapjumpCollision): its native component (+0x10), that
// component's GameObject (+0x30), then the same transform walk
bool WorldPosOfComponent(const Guest& g, uint64_t comp, float* out) {
    const uint64_t nat = Guest::objp(comp) ? g.ptr(comp + 0x10) : 0;
    const uint64_t gobj = Guest::objp(nat) ? g.ptr(nat + 0x30) : 0;
    return WorldPosOfNative(g, gobj, out);
}
bool WorldPosOfNative(const Guest& g, uint64_t gobj, float* out) {
    if (!Guest::objp(gobj)) return false;
    const uint64_t comps = g.ptr(gobj + 0x30);
    const uint64_t tr = Guest::objp(comps) ? g.ptr(comps + 8) : 0;
    const uint64_t hier = Guest::objp(tr) ? g.ptr(tr + 0x38) : 0;
    bool oi = false;
    int32_t idx = Guest::objp(tr) ? g.i32(tr + 0x40, &oi) : -1;
    const uint64_t trs = Guest::objp(hier) ? g.ptr(hier + 0x18) : 0;
    const uint64_t par = Guest::objp(hier) ? g.ptr(hier + 0x20) : 0;
    if (!oi || idx < 0 || idx > 100000 || !Guest::objp(trs) || !Guest::objp(par)) return false;
    float p[3] = {0, 0, 0};
    for (int depth = 0; depth < 48 && idx >= 0 && idx < 100000; ++depth) {
        float t[12];                              // position (+pad), rotation xyzw, scale (+pad)
        if (!g.read(trs + static_cast<uint64_t>(idx) * 0x30, t, sizeof t)) return false;
        if (depth == 0) { p[0] = t[0]; p[1] = t[1]; p[2] = t[2]; }
        else {
            const float v[3] = {p[0] * t[8], p[1] * t[9], p[2] * t[10]};
            const float qx = t[4], qy = t[5], qz = t[6], qw = t[7];
            const float cx = qy * v[2] - qz * v[1] + qw * v[0];
            const float cy = qz * v[0] - qx * v[2] + qw * v[1];
            const float cz = qx * v[1] - qy * v[0] + qw * v[2];
            p[0] = v[0] + 2 * (qy * cz - qz * cy) + t[0];
            p[1] = v[1] + 2 * (qz * cx - qx * cz) + t[1];
            p[2] = v[2] + 2 * (qx * cy - qy * cx) + t[2];
        }
        bool ok = false;
        const int32_t up = g.i32(par + static_cast<uint64_t>(idx) * 4, &ok);
        if (!ok || up == idx) break;
        idx = up;
    }
    if (!(p[0] == p[0] && p[1] == p[1] && p[2] == p[2])) return false;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
    return out[0] != 0 || out[2] != 0;
}

// remember a fixed icon; abs: a world position (true at once), else relative to the player (confirm)
void Remember(const MapState& m, uint64_t ip, int t, float x, float y, bool abs, uint32_t argb) {
    int k = 0;
    while (k < g_nmemo && g_memo[k].ip != ip) ++k;
    if (k == g_nmemo) {
        if (g_nmemo >= 128) return;
        ++g_nmemo;
        g_memo[k] = {ip, t, abs, x, y, m.x, m.y, x, y, argb, g_mark_tick, 0};
        return;
    }
    IconMemo& e = g_memo[k];
    e.type = t; e.argb = argb; e.seen = g_mark_tick; e.miss = 0;
    if (abs) { e.conf = true; e.x = x; e.y = y; return; }
    const float dpx = m.x - e.apx, dpy = m.y - e.apy;
    const float d2 = dpx * dpx + dpy * dpy;
    if (d2 < 36) return;                       // wait until the player has moved 6 picture pixels
    const float dx = x - e.ax, dy = y - e.ay;
    // stayed within 2 px + a quarter of the player's move (squared: no libm here)
    const float dd = dx * dx + dy * dy;
    if (dd < 8.0f + 0.125f * d2) { e.conf = true; e.x = x; e.y = y; }   // stayed put: real
    e.ax = x; e.ay = y; e.apx = m.x; e.apy = m.y;
}

// which: the presenter (diag); r_fixed: picture pixels per minimap pixel (0 = from the follow
// coefficient); win: half the game's minimap window (minimap pixels) for the moving icons, 0 = none
// whole: the world map's whole-world minimap (its icons sit at picture = centre + (local + parent) x whole)
// 0.9.42 (r48 / r49) the objects the game itself marks with the objective "!" -- read where they are, as for any
// object, instead of the minimap's icon numbers. In an area SetDestinationMapIcon writes MapIconType 9 into the
// subIconType (+0x198) of the FieldMapjumpCollision exits that lead toward the destination (FieldManager's
// fieldCollisionList +0xE0); on the world map SetWorldDestinationMapIcon writes it into the iconType / subIconType
// (+0x158 / +0x15C) of the WorldmapSymbol location that leads there (fieldObjectList +0xD8). An object's place:
// its own transform (GameComponent.get_Position: transform +0x18 -> localPosition; WorldPosOf walks the chain).
constexpr uint64_t kFmObjectList = 0xD8, kFmCollisionList = 0xE0, kMjSubIcon = 0x198, kWsIcon = 0x158, kWsSubIcon = 0x15C;
constexpr int kMaxMarked = 8;
struct Marked { int n, total, tick, field; bool world, read; float w[kMaxMarked][3]; uint64_t obj[kMaxMarked]; };
Marked g_marked;
void ScanMarked(const Guest& g, uint64_t fm, bool world, int fieldmap) {
    Marked& k = g_marked;
    // every 15 passes (objects stay put; a new field map at once)
    if (k.read && k.world == world && k.field == fieldmap && ++k.tick < 15) return;
    k = {}; k.world = world; k.field = fieldmap; k.read = true;
    const uint64_t list = Guest::objp(fm) ? g.ptr(fm + (world ? kFmObjectList : kFmCollisionList)) : 0;
    const uint64_t items = Guest::objp(list) ? g.ptr(list + kListItems) : 0;
    const int32_t n = Guest::objp(list) ? g.i32(list + kListSize) : 0;
    if (!Guest::objp(items) || n <= 0 || n > 8192) { k.read = false; return; }
    k.total = n;
    for (int32_t i = 0; i < n && k.n < kMaxMarked; ++i) {
        const uint64_t o = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
        if (!Guest::objp(o)) continue;
        const bool hit = world ? (g.i32(o + kWsIcon) == 9 || g.i32(o + kWsSubIcon) == 9) : g.i32(o + kMjSubIcon) == 9;
        if (!hit || !g.is(o, world ? "WorldmapSymbol" : "FieldMapjumpCollision")) continue;
        if (!WorldPosOfComponent(g, o, k.w[k.n])) continue;
        k.obj[k.n++] = o;
    }
}
// the marked objects on the picture, as the "!" (type 9); returns how many were placed
int AddMarked(MapState& m) {
    int placed = 0;
    for (int i = 0; i < g_marked.n && m.nmarks < kMaxMarks; ++i) {
        float x, y;
        if (!PlaceOnPicture(m, g_marked.w[i][0], g_marked.w[i][1], g_marked.w[i][2], x, y)) continue;
        m.marks[m.nmarks++] = {x, y, 9, 0xFFFFFFFFu};
        ++placed;
    }
    return placed;
}

// 0.9.45 the whole-world list's geometry from the game's own pictures, so its icons show at load with the minimap
// hidden (0.9.44 on the Thor: none until it was shown once). Its minimap picture (minimap_<map>001) is the world map
// picture scaled down, its icons placed around that picture's centre: picture = centre + local x (picture width /
// minimap width) -- 0.9.39-0.9.44 on the Thor verified exactly that for expel (k 5.0 = 2800 / 560, C 1400, 1100 =
// the centre). Only when the minimap picture is whole (not trimmed: its rect is its texture) and both axes give the
// same scale; otherwise (nede's is trimmed) the live checks as before. A live reading that disagrees over a move
// still replaces it (GeoVerify).
bool GeoFromPictures(ListGeo& G, const MapState& m) {
    if (!(m.area[0] == 'm' && m.area[1] == 'a' && m.area[2] == 'p' && m.area[3] == '_') || m.w <= 0 || m.h <= 0) return false;
    char nm[48] = "minimap_";
    size_t i = 8;
    for (const char* q = m.area + 4; *q && i + 4 < sizeof nm; ++q) nm[i++] = *q;
    nm[i++] = '0'; nm[i++] = '0'; nm[i++] = '1'; nm[i] = 0;
    const IdxSprite* sp = FindSprite(nm);
    if (!sp || sp->x || sp->y || !sp->w || !sp->h) return false;
    const IdxTex* tx = FindTex(sp->tex);
    if (!tx || tx->w != sp->w || tx->h != sp->h) return false;
    const float kx = m.w / sp->w, ky = m.h / sp->h;
    if (AbsF(kx - ky) > 0.005f * kx) return false;
    G.ok = true; G.k = kx; G.cx = m.w / 2; G.cy = m.h / 2; G.why = kGeoPictures; G.anc = false;
    return true;
}

void ReadMarks(MapState& m, const Guest& g, uint64_t pres, [[maybe_unused]] int which, float r_fixed, float winx, float winy, bool round,
               float whole = 0) {
#if SO2R_DIAG
    int raw_budget = 4;
    m.win[0] = winx; m.win[1] = winy;
#endif
    const uint64_t list = Guest::objp(pres) ? g.ptr(pres + kMpIconList) : 0;
    bool ok = false;
    const int32_t n = Guest::objp(list) ? g.i32(list + kListSize, &ok) : 0;
    const uint64_t items = ok ? g.ptr(list + kListItems) : 0;
    if (!ok || n <= 0 || n > 1024 || !Guest::objp(items)) return;
    // the player's own icon in this list: icons without a task are placed from it (picture = player +
    // (icon - player icon) x scale), which holds whether the game keeps them around the player (the
    // zoomed round minimap) or around the world's centre (the whole-world one)
    float pli[2] = {0, 0};
    bool have_pli = false;
    ListGeo* geo = r_fixed > 0 ? GeoFor(m.area, which) : nullptr;   // 0.9.38 the world lists' verified geometry
    // 0.9.45 (and its scale is the whole-world scale, in place of the 5.2 first guess)
    if (geo && which == 2 && whole > 0 && !geo->ok && GeoFromPictures(*geo, m) && g_whole <= 0) g_whole = geo->k;
    // 0.9.39 the game's own minimap state: shown (10 passes running) and this list in use, or hidden
    int nused = 0;
    if (geo) for (int32_t i = 0; i < n; ++i) {
        const uint64_t ip = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
        nused += Guest::objp(ip) && (g.u8(ip + kIpUsed) & 1);
    }
    const bool shown = MiniFromState(m.mstate) == kMiniOn && m.mstate_run >= 10 && nused > 1;
    const bool hidden = MiniFromState(m.mstate) == kMiniOff;
    const bool hidden_state = hidden;
    {
        for (int32_t i = 0; i < n; ++i) {
            const uint64_t ip = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
            if (!Guest::objp(ip) || !(g.u8(ip + kIpUsed) & 1) || g.i32(ip + kIpType) != 1) continue;
            float a[3], b[3];
            if (!NativeLocalPos(g, ip, a, b, nullptr, 0)) break;
            pli[0] = a[0]; pli[1] = a[1]; have_pli = true;
            m.wplayer[0] = a[0]; m.wplayer[1] = a[1];
            if (geo) GeoTrack(*geo, a[0], a[1], m.x, m.y);
            if (whole <= 0) break;
            const float ox = m.x - m.w / 2, oy = m.y - m.h / 2;
            const float l2 = a[0] * a[0] + a[1] * a[1], o2 = ox * ox + oy * oy;
            m.wplayer[0] = a[0]; m.wplayer[1] = a[1];
            // 0.9.38: only from a player icon that follows the player (a stale one gave a wrong scale)
            if (l2 > 400 && o2 > 400 && geo && (geo->fresh > 0 || shown) && !hidden) {
                // F = sqrt(o2 / l2) without libm: a few Newton steps from a fair start
                const float q = o2 / l2;
                float f = q > 1 ? q / 2 : 1;
                for (int it = 0; it < 12; ++it) f = 0.5f * (f + q / f);
                if (f > 1 && f < 30) g_whole = g_whole > 0 ? g_whole * 0.8f + f * 0.2f : f;
            }
            break;
        }
        if (whole > 0) { if (g_whole > 0) whole = g_whole; m.wscale = whole; }
        if (geo) {
            if (have_pli) GeoVerify(*geo, pli[0], pli[1], m.x, m.y, whole > 0 ? whole : r_fixed, shown, hidden);
            else geo->why = kGeoNoPlayer;
        }
    }
    for (int32_t i = 0; i < n; ++i) {
        const uint64_t ip = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
        if (!Guest::objp(ip)) continue;
        if (!(g.u8(ip + kIpUsed) & 1)) {
#if SO2R_DIAG
            if (Guest::objp(g.ptr(ip + kIpFollow))) ++m.unused_follow;   // 0.9.31: the minimap off? (diag)
#endif
            continue;
        }
        ++m.icons_seen;
        const int32_t t = g.i32(ip + kIpType);
        if (which >= 0 && which < 3) ++m.list_used[which];
#if SO2R_DIAG
        if (r_fixed <= 0 && t >= 0 && t < 32) ++g_type_n[t];
#endif
        if (t <= 0 || t > 30 || !kIconPic[t] || !ShownIcon(t)) continue;
        // the colour the game draws the icon with (GameImage +0xF0, Graphic.m_Color +0x20): enemies are
        // tinted purple, NPCs white ...; a see-through icon is one the game is not showing
        uint32_t argb = 0;
        float col[4];
        bool hidden = false;
        {
            const uint64_t gi = g.ptr(ip + 0xF0);
            if (Guest::objp(gi) && g.read(gi + kGraphicColor, col, sizeof col) && col[3] == col[3]) {
                // 0.9.31: a see-through icon (the game's minimap turned off, 0.9.30 on the Thor) is still
                // drawn when its object's own world position can be read (below); otherwise it is skipped
                if (col[3] < 0.05f) hidden = true;
                if (which >= 0 && which < 3) ++(hidden ? m.mhid : m.mvis)[which];
                auto c8 = [](float v) { return static_cast<uint32_t>((v < 0 ? 0 : v > 1 ? 1 : v) * 255 + 0.5f); };
                argb = 0xFF000000u | c8(col[0]) << 16 | c8(col[1]) << 8 | c8(col[2]);
                if (argb == 0xFFFFFFFFu) argb = 0;
            }
            // 0.5.3 on the Thor: the image stays white; the game keeps the colours of the moving icons
            // on the presenter (playerColor +0x100, enemyColor +0x110, friendColor +0x120, npcColor
            // +0x130) and puts them on the icon's material, so take them from there
            const uint64_t co = t == 3 ? 0x110 : t == 2 ? 0x120 : t == 4 ? 0x130 : 0;
            if (co && !argb && g.read(ip + co, col, sizeof col) && col[3] == col[3] && col[3] > 0.05f) {
                auto c8 = [](float v) { return static_cast<uint32_t>((v < 0 ? 0 : v > 1 ? 1 : v) * 255 + 0.5f); };
                argb = 0xFF000000u | c8(col[0]) << 16 | c8(col[1]) << 8 | c8(col[2]);
                if (argb == 0xFFFFFFFFu) argb = 0;
            }
#if SO2R_DIAG
            if (t == 3) m.enemy_argb = argb;
#endif
        }
        const uint64_t ft = g.ptr(ip + kIpFollow);
        float lp[3], pl[3];
        char* gname = nullptr; size_t gn = 0;
#if SO2R_DIAG
        MapState::RawMark* rw = nullptr;
        if (m.nraw < 8 && raw_budget > 0) {     // the raw numbers of the first icons, to check them on the Thor
            --raw_budget;
            rw = &m.raw[m.nraw++];
            *rw = {};
            rw->type = t; rw->ip = ip; rw->list = which; rw->sub = g.i32(ip + 0xFC);
            rw->follow = Guest::objp(ft) ? 1 + (g.u8(ft + kFtExists) & 1) : 0;
            g.read(ip + kIpWorld, rw->w, sizeof rw->w);
            if (Guest::objp(ft)) { g.read(ft + kFtPos, rw->c, sizeof rw->c); WorldPosOf(g, g.ptr(ft + 0x28), rw->w); }
            gname = rw->go; gn = sizeof rw->go;
        }
#endif
        const bool nat = NativeLocalPos(g, ip, lp, pl, gname, gn);
#if SO2R_DIAG
        if (rw) { memcpy(rw->lp, lp, sizeof lp); memcpy(rw->e, pl, sizeof pl); }
#endif
        float x = 0, y = 0;
        bool have = false, abs = false;
        float wo[3];
        if (Guest::objp(ft) && WorldPosOf(g, g.ptr(ft + 0x28), wo)) {
            have = PlaceOnPicture(m, wo[0], wo[1], wo[2], x, y); abs = true;   // where the object really is
        } else if (Guest::objp(ft) && (g.u8(ft + kFtExists) & 1)) {
            float c[2];
            if (g.read(ft + kFtPos, c, sizeof c) && c[0] == c[0] && c[1] == c[1] &&
                !(AbsF(c[0] + 760) < 1 && AbsF(c[1] - 340) < 1)) {       // not parked off the minimap
                bool inside = true;                                       // (moving icons: inside the window)
                if (!StaticIcon(t)) {
                    if (round && winx > 1) inside = c[0] * c[0] + c[1] * c[1] < (winx * 0.9f) * (winx * 0.9f);
                    else if (winx > 1 && winy > 1) inside = AbsF(c[0]) < winx * 0.92f && AbsF(c[1]) < winy * 0.92f;
                }
                const float fc = g.f32(ft + kFtCoeff);
                const float r = r_fixed > 0 ? r_fixed : (fc > 0.01f && m.pk > 0 ? m.pk / fc : 1);
                if (inside) { x = m.x + c[0] * r; y = m.y + c[1] * r; have = true; }
            }
        } else {
            float w[3];
            if (g.read(ip + kIpWorld, w, sizeof w) && (w[0] != 0 || w[2] != 0) && w[0] == w[0] && w[2] == w[2]) {
                have = PlaceOnPicture(m, w[0], w[1], w[2], x, y); abs = true;   // a fixed world position
            } else if (nat && r_fixed > 0 && (lp[0] != 0 || lp[1] != 0)) {
                // the world map's town / dungeon / event icons: local position in the game's minimap
                // (minimap pixels; the parent offset left out, 0.4.5 on the Thor) through the list's
                // VERIFIED geometry (0.9.38); none yet: not placed (the kept set is drawn instead)
                // 0.9.52 the round list (L1) is drawn around the player: its icons are placed from the player's
                // icon in the same pass (picture = player + (icon - player icon) x k), only while the game is
                // updating it (its state shown, or its player icon following) -- 0.9.51 on the Thor: a C taken
                // standing still (it was the player's own place) left every icon behind as the player walked, until
                // the minimap was toggled. The whole-world list (L2) keeps its fixed, verified C.
                if (which == 1 && geo && have_pli && !hidden_state && (shown || geo->fresh > 0)) {
                    x = m.x + (lp[0] - pli[0]) * r_fixed; y = m.y + (lp[1] - pli[1]) * r_fixed; have = true; abs = true;
                }
                else if (which != 1 && geo && geo->ok) { x = geo->cx + lp[0] * geo->k; y = geo->cy + lp[1] * geo->k; have = true; abs = true; }
                else { if (which >= 0 && which < 3) ++m.unverified[which]; continue; }
            }
        }
#if SO2R_DIAG
        if ((t == 9 || t == 10) && r_fixed <= 0) {
            Str d; d.add(g_dest_diag).add(t == 9 ? " D" : " S").add(hidden ? "h" : "").add(abs ? "a" : "").add(have ? "+" : "-");
            d.ch('@').num(RoundF(x)).ch(',').num(RoundF(y)).add(Guest::objp(ft) ? " f" : " nf").num(Guest::objp(ft) ? g.u8(ft + kFtExists) & 1 : 0);
            size_t i = 0; for (; d.s[i] && i + 1 < sizeof g_dest_diag; ++i) g_dest_diag[i] = d.s[i];
            g_dest_diag[i] = 0;
        }
#endif
        int dsrc = have ? (abs ? kSrcWorld : kSrcFollow) : kSrcNone;
        if (!have && r_fixed <= 0 && StaticIcon(t)) {
            // 0.9.34 an area's fixed icon whose follow target gives nothing (the destination "!": follow
            // present, its exists bit 0, 0.9.33 diag): its fixed world position, else its own position in
            // the game's minimap from the player's icon (minimap pixels x the picture pixels per minimap
            // pixel); not a world position, so it is drawn once it stays put while the player moves
            float w[3];
            if (g.read(ip + kIpWorld, w, sizeof w) && (w[0] != 0 || w[2] != 0) && w[0] == w[0] && w[2] == w[2]) {
                have = PlaceOnPicture(m, w[0], w[1], w[2], x, y); abs = true;
            } else if (nat && have_pli && (lp[0] != 0 || lp[1] != 0) && !(AbsF(lp[0] + 760) < 1 && AbsF(lp[1] - 340) < 1)) {
                const float fc = Guest::objp(ft) ? g.f32(ft + kFtCoeff) : 0;
                const float r = fc > 0.01f && m.pk > 0 ? m.pk / fc : m.pk > 0 ? m.pk / 5.0f : 0;
                if (r > 0) { x = m.x + (lp[0] - pli[0]) * r; y = m.y + (lp[1] - pli[1]) * r; have = true; dsrc = kSrcLocal; }
#if SO2R_DIAG
                if (t == 9) g_dest_raw.coeff = fc > 0.01f ? fc : 5.0f;
#endif
            }
            if (have && abs) dsrc = kSrcWorld;
#if SO2R_DIAG
            if (t == 9) {
                Str d; d.add(g_dest_diag).add(" -> ").add(have ? (abs ? "world" : "local") : "none")
                    .add(" lp ").num(RoundF(lp[0])).ch(',').num(RoundF(lp[1])).add(" pl ").num(RoundF(pli[0])).ch(',').num(RoundF(pli[1]))
                    .add(" pk ").num(RoundF(m.pk * 100));
                size_t i = 0; for (; d.s[i] && i + 1 < sizeof g_dest_diag; ++i) g_dest_diag[i] = d.s[i];
                g_dest_diag[i] = 0;
            }
#endif
        }
        // 0.9.37 the round world minimap pins a far destination ("!") to its rim as a pointer (0.9.36 on the
        // Thor: drawn beside the player); only one well inside the window is where it really is
        if (have && round && r_fixed > 0 && whole <= 0 && winx > 1 && (t == 9 || t == 10)) {
            const float dx = x - m.x, dy = y - m.y, lim = 0.75f * winx * r_fixed;
            if (dx * dx + dy * dy >= lim * lim) { ++m.clamped; continue; }
        }
        if (hidden && StaticIcon(t) && which >= 0 && which < 3) ++m.list_hidden[which];
        // 0.9.38 the area's destination "!": through its objective's tracker (DestTrack), not the memo
        if (t == 9 && r_fixed <= 0 && m.ndest++ == 0) {
#if SO2R_DIAG
            g_dest_raw.ip = ip; g_dest_raw.type = t; g_dest_raw.src = dsrc; g_dest_raw.hidden = hidden;
            g_dest_raw.lp[0] = lp[0]; g_dest_raw.lp[1] = lp[1]; g_dest_raw.pli[0] = pli[0]; g_dest_raw.pli[1] = pli[1];
            g_dest_raw.have_pli = have_pli; g_dest_raw.x = x; g_dest_raw.y = y;
            if (Guest::objp(ft)) g.read(ft + kFtPos, g_dest_raw.cache, sizeof g_dest_raw.cache);
#endif
            // a see-through icon's UI numbers are not trusted (the game is not updating what it hides)
            // 0.9.39: nor any UI number while the game's own state says Hide
            const bool off = hidden || MiniFromState(m.mstate) == kMiniOff;
            g_dest.seen = true;            // 0.9.40 the game is showing a "!" for this objective on this floor
            float tx, ty;
            if (g_tdest.ok && PlaceOnPicture(m, g_tdest.pos[0], g_tdest.pos[1], g_tdest.pos[2], tx, ty))
                DestStep(g_dest, kSrcTable, tx, ty, argb, m.x, m.y);
            else if (have && (dsrc == kSrcWorld || !off)) DestStep(g_dest, dsrc, x, y, argb, m.x, m.y);
            else DestNoReading(g_dest, off);
            continue;
        }
        if (hidden && !(abs && Guest::objp(ft))) continue;
#if SO2R_DIAG
        if (have && r_fixed <= 0 && t >= 0 && t < 32) ++g_type_drawn[t];
        if (hidden) ++m.hidden_drawn;
#endif
        // fixed icons in an area are remembered; the world map's lists are whole, so drawn live (the
        // game's two minimap modes have their own icons: remembered, both would stack)
        if (have && StaticIcon(t) && r_fixed <= 0) { Remember(m, ip, t, x, y, abs, argb); continue; }
        if (have && abs && !StaticIcon(t) && r_fixed <= 0 && Guest::objp(ft) && g_nmovers_now < 64)
            g_movers_now[g_nmovers_now++] = {g.ptr(ft + 0x28), t, argb};
        if (have && m.nmarks < kMaxMarks && x > -64 && y > -64 && x < m.w + 64 && y < m.h + 64)
            m.marks[m.nmarks++] = {x, y, t, argb};
    }
}
// 0.9.18 the world map's fixed icons (towns, events, ...) come from the game's minimap, which stops filling
// them when the player turns the minimap off: the last ones read stay on the page (per world map) until
// the game's lists fill again
struct WorldIcons { char area[48]; int n; ObjId obj; bool objset; MapState::Mark marks[kMaxMarks]; };
WorldIcons g_wicons;
// 0.9.27 the world map's fixed icons (towns, dungeons, events) after a battle: the game's lists can hold
// icons in the wrong place (0.9.25/0.9.26 on the Thor) until its minimap is shown in its round mode again.
// The last good set (g_wicons) is kept through the battle; after it, a read's fixed icons are only taken once
// they agree with that set (at least half of its icons found again, each within 30 picture px of one of the
// same type) -- until then the kept set is drawn. Outside a battle a set that keeps disagreeing (a changed
// world) is taken after 20 reads. No kept set: the read is drawn as it is.
int g_after_battle;         // a battle was seen since the last read that agreed
int g_disagree;             // reads in a row that disagreed with the kept set
bool AgreesWithKept(const MapState& m) {
    int matched = 0;
    for (int k = 0; k < g_wicons.n; ++k) {
        const MapState::Mark& c = g_wicons.marks[k];
        for (int i = 0; i < m.nmarks; ++i) {
            const MapState::Mark& r = m.marks[i];
            if (r.type != c.type || !StaticIcon(r.type)) continue;
            const float dx = r.x - c.x, dy = r.y - c.y;
            if (dx * dx + dy * dy <= 30.0f * 30.0f) { ++matched; break; }
        }
    }
    return matched * 2 >= g_wicons.n;
}
void WorldIconCache(MapState& m, const ObjId& obj) {
    // a new objective (or save): the kept destination icons are the old one's (0.9.37 by row, 0.9.38 by identity)
    if (!g_wicons.objset || !SameObj(g_wicons.obj, obj)) {
        int keep = 0;
        for (int k = 0; k < g_wicons.n; ++k) if (g_wicons.marks[k].type != 9) g_wicons.marks[keep++] = g_wicons.marks[k];
        g_wicons.n = keep; g_wicons.obj = obj; g_wicons.objset = true;
    }
    int fixed = 0;
    for (int i = 0; i < m.nmarks; ++i) fixed += StaticIcon(m.marks[i].type);
    const bool kept = g_wicons.n > 0 && eq(g_wicons.area, m.area);
    // 0.9.38 a read with icons not placed (no verified geometry for their list yet) is not a whole set
    bool take = fixed > 0 && !(m.unverified[1] + m.unverified[2] > 0 && kept);
    if (take && kept) {
        if (AgreesWithKept(m)) { g_after_battle = 0; g_disagree = 0; }
        else if (!g_after_battle && ++g_disagree >= 20) g_disagree = 0;      // the world changed: take it
        else take = false;
    }
    // 0.9.38 (0.9.37's carry of any missing icon taken out: its "missing" icons were the shifted ones) only the
    // destination: one the round minimap pinned to its rim (left out, m.clamped) keeps the verified "!" of the
    // same objective
    if (take && kept && m.clamped > 0) {
        bool has9 = false;
        for (int i = 0; i < m.nmarks; ++i) has9 |= m.marks[i].type == 9;
        for (int k = 0; k < g_wicons.n && !has9 && m.nmarks < kMaxMarks; ++k)
            if (g_wicons.marks[k].type == 9) { m.marks[m.nmarks++] = g_wicons.marks[k]; ++m.wcarried; break; }
    }
    if (take) {
        size_t i = 0; for (; m.area[i] && i + 1 < sizeof g_wicons.area; ++i) g_wicons.area[i] = m.area[i];
        g_wicons.area[i] = 0;
        g_wicons.n = 0;
        for (int k = 0; k < m.nmarks; ++k) if (StaticIcon(m.marks[k].type)) g_wicons.marks[g_wicons.n++] = m.marks[k];
    } else if (kept) {                     // the read's own fixed icons out, the kept ones in
        int keep = 0;
        for (int k = 0; k < m.nmarks; ++k) if (!StaticIcon(m.marks[k].type)) m.marks[keep++] = m.marks[k];
        m.nmarks = keep;
        for (int k = 0; k < g_wicons.n && m.nmarks < kMaxMarks; ++k) m.marks[m.nmarks++] = g_wicons.marks[k];
        m.wcached = fixed > 0 ? 2 : 1;
    }
}
// start a tick's markers (a new area forgets the last one's icons); end: add the confirmed fixed ones
void BeginMarks(MapState& m) {
    m.nmarks = 0; m.icons_seen = 0; m.wcached = 0; m.clamped = 0; m.wcarried = 0;
    for (int i = 0; i < 3; ++i) m.list_used[i] = m.list_hidden[i] = m.mvis[i] = m.mhid[i] = m.unverified[i] = 0;
    m.ndest = 0; m.marked = 0; m.unused_follow = 0; m.hidden_drawn = 0; ++g_mark_tick;
    g_nmovers_now = 0;
#if SO2R_DIAG
    g_dest_diag[0] = 0; g_dest_raw = {};
    for (int i = 0; i < 32; ++i) g_type_n[i] = g_type_drawn[i] = 0;
#endif
#if SO2R_DIAG
    m.nraw = 0;
#endif
    if (!eq(g_memo_area, m.area)) {
        g_nmemo = 0; g_nmovers = 0;
        size_t i = 0; for (; m.area[i] && i + 1 < sizeof g_memo_area; ++i) g_memo_area[i] = m.area[i];
        g_memo_area[i] = 0;
    }
}
void EndMarks(MapState& m, const Guest* g = nullptr) {
    if (g) {   // 0.9.32 the area's moving icons: this pass's, or (none given) the kept ones from their objects
        if (g_nmovers_now > 0) {
            for (int k = 0; k < g_nmovers_now; ++k) g_movers[k] = g_movers_now[k];
            g_nmovers = g_nmovers_now; g_movers_used = 0;
        } else if (g_nmovers > 0) {
            int keep = 0;
            for (int k = 0; k < g_nmovers; ++k) {
                float w[3], x = 0, y = 0;
                if (!WorldPosOf(*g, g_movers[k].mgo, w) || !PlaceOnPicture(m, w[0], w[1], w[2], x, y)) continue;
                g_movers[keep++] = g_movers[k];
                if (m.nmarks < kMaxMarks && x > -64 && y > -64 && x < m.w + 64 && y < m.h + 64)
                    m.marks[m.nmarks++] = {x, y, g_movers[k].type, g_movers[k].argb};
            }
            g_nmovers = keep; g_movers_used = keep;
        }
    }
#if SO2R_DIAG
    g_nconf = 0;
#endif
    // an icon the game stopped using (an event done, a chest gone) leaves the memo with it (only when
    // the game's list was read this pass: a failed read forgets nothing).
    // 0.9.38: a miss counts only while the game's minimap is known to be shown (MiniFrom, from the icons' own
    // colour), not from "no moving icons read" (0.9.35/0.9.36, a guess); off or unknown: nothing is forgotten.
    // The destination "!" is not in the memo (DestTrack), so its toggles no longer depend on this timeout.
    int keep = 0;
    const int mini = MiniFromState(m.mstate);   // 0.9.39 the game's own state
    m.mini = mini;
    for (int k = 0; k < g_nmemo; ++k) {
        IconMemo& e = g_memo[k];
        if (e.seen != g_mark_tick && m.icons_seen && mini == kMiniOn) ++e.miss;
        if (e.miss < 90) g_memo[keep++] = e;
    }
    m.memo_dropped += g_nmemo - keep;
    g_nmemo = keep;
    // 0.9.38 the area's destination: no "!" read this pass keeps the verified one (DestNoReading); drawn only
    // once verified, and only for this floor
    if (g && m.icons_seen && !m.ndest) DestNoReading(g_dest, false);
    // 0.9.42: the exits the game marks (g_marked, added in ReadMap) are the "!" when there are any; the tracked one
    // then only when it is the table's own position (an in-area target)
    const bool exits = g && g_marked.n > 0 && !g_marked.world;
    if (g && g_dest.ok && eq(g_dest.area, m.area) && m.nmarks < kMaxMarks && (!exits || g_dest.src == kSrcTable))
        m.marks[m.nmarks++] = {g_dest.x, g_dest.y, 9, g_dest.argb};
    for (int k = 0; k < g_nmemo && m.nmarks < kMaxMarks; ++k) {
        if (!g_memo[k].conf) continue;
#if SO2R_DIAG
        ++g_nconf;
#endif
        const float x = g_memo[k].x, y = g_memo[k].y;
        if (x > -64 && y > -64 && x < m.w + 64 && y < m.h + 64) m.marks[m.nmarks++] = {x, y, g_memo[k].type, g_memo[k].argb};
    }
}

void ReadMap(Instance& in, const EdenDsmodHostApi* h) {
    Guest g{h};
    MapState& m = in.map;
    if (in.scene == 6) g_after_battle = 1;               // 0.9.27: the world icons are checked after it
    if (in.scene == 6 || in.scene == 7) return;          // battle, camp: keep showing where we were
    const int keep = m.ok;
    // 0.9.27 the result screen and the loads around a battle are no scene of play: keep showing the map
    if (keep && in.ok && (!PlayingScene(in)) && in.scene != 3 && !starts(in.scene_cls, "Title")) return;
    m.ok = 0;
    m.pmode = 0;
    if (!PlayingScene(in) || !in.ok) { m.why = "waiting for the game"; return; }
    const uint64_t fm = Singleton(in, g, kFieldMgrTypeInfo);
    if (!g.is(fm, "FieldManager")) { m.why = "no field yet"; return; }
    bool okx = false, okz = false;
    float px = g.f32(fm + kFmPos, &okx), pz = g.f32(fm + kFmPos + 8, &okz), py = g.f32(fm + kFmPos + 4);
    if (!okx || !okz) { m.why = "position unreadable"; return; }
    // the live position: the newest entries of the player's ring of recent positions
    m.live = false;
    {
        const uint64_t fp = g.ptr(fm + kFmPlayer);
        const uint64_t arr = g.is(fp, "FieldPlayer") ? g.ptr(fp + kFpPrev) : 0;
        bool ol = false, os = false;
        const int32_t n = Guest::objp(arr) ? g.i32(arr + kArrLen, &ol) : 0;
        const int32_t st = g.i32(fp + kFpPrevStart, &os);
        if (ol && os && n >= 10 && n <= 4096) {
            const int32_t s0 = ((st % n) + n) % n;
            auto at = [&](int32_t back, float* v) {   // the entry `back` places behind the write point
                const uint64_t e = arr + kArrItems + static_cast<uint64_t>(((s0 - back) % n + n) % n) * 12;
                bool a, b, c;
                v[0] = g.f32(e, &a); v[1] = g.f32(e + 4, &b); v[2] = g.f32(e + 8, &c);
                return a && b && c;
            };
            float p1[3], p8[3];
            if (at(1, p1) && at(8, p8) && (p1[0] != 0 || p1[2] != 0)) {
                px = p1[0]; py = p1[1]; pz = p1[2]; m.live = true;
                const float mx = p1[0] - p8[0], mz = p1[2] - p8[2];
                // moving: face the way the player walks. The game's player icon points down; the
                // game turns it by 180 - yaw (UIFollowObjectOnMinimapTask.Run).
                if (mx * mx + mz * mz > 0.01f) m.heading = Atan2Deg(mx, mz) + 180.0f;
            }
        }
    }
    if (!m.live) {
        bool okd = false, okd2 = false;      // playerDirection is the facing vector (Vector3)
        const float dx = g.f32(fm + kFmDir, &okd), dz = g.f32(fm + kFmDir + 8, &okd2);
        if (okd && okd2 && (dx != 0 || dz != 0)) m.heading = Atan2Deg(dx, dz) + 180.0f;
    }
    m.fieldmap = g.i32(fm + kFmFieldmap);
    m.worldmap = g.i32(fm + kFmWorldmap);
    const uint64_t gui = Singleton(in, g, kGameUiTypeInfo);
    const uint64_t ufc = DictInt(g, g.ptr(gui + kGuiControllers), kGuiFieldKey);
    const uint64_t gdk = g.ptr(g.ptr(in.main_base + kGameDefineRef));
    const uint64_t gd = Guest::objp(gdk) ? g.ptr(gdk + kClassStatics) : 0;
    if (Guest::objp(gd)) {
        const int ml = g.i32(gd + kGdMaxLevel); if (ml > 1 && ml < 10000) in.max_level = ml;
        const int bt = g.i32(gd + kGdBsTop); if (bt > 0 && bt < 100) in.bs_top = bt;
        const int sk = g.i32(gd + kGdMaxSkill); if (sk > 0 && sk < 100) in.gd_max_skill = sk;
        const int cb = g.i32(gd + kGdMaxCombat); if (cb > 0 && cb < 100) in.gd_max_combat = cb;
        uint16_t mf = 0; g.read(gd + kGdMaxFriend, &mf, 2); if (mf > 0 && mf < 60000) in.max_friend = mf;
    }
#if SO2R_DIAG
    m.px = px; m.pz = pz; m.py = py; m.ufc = ufc; m.mgr = m.pres = m.task = 0;
    m.offx = m.offy = m.coeff = m.scale = m.euler = m.extra = 0; m.wp = -1;
#endif
    if (!g.is(ufc, "UIFieldController")) { m.mstate = -1; m.mstate_run = 0; m.why = "no field screen yet"; return; }
    {   // 0.9.39 the game's minimap state, and how many passes it has held
        int32_t v = -1;
        const int st = g.read(ufc + kUfcMinimapState, &v, sizeof v) && v >= 0 && v <= 2 ? v : -1;
        static uint64_t s_ufc;                              // a new field screen: counted again from 0
        m.mstate_run = st == m.mstate && ufc == s_ufc ? m.mstate_run + 1 : 0;
        m.mstate = st; s_ufc = ufc;
    }
    char lower[40];
    if (!g.cstr(gdk, lower, sizeof lower) || !eq(lower, "GameDefine") || !Guest::objp(gd)) { m.why = "GameDefine not ready"; return; }
    if (in.scene == 5) {                                   // the world map
        m.kind = 2;
        const uint64_t wp = g.ptr(ufc + kUfcWorldPresenter);
        if (!g.is(wp, "UIWorldMinimapPresenter")) { m.why = "no world minimap"; return; }
        const int wid = m.worldmap;
        if (wid != 1 && wid != 2) { m.why = "world map unknown"; return; }
        const char* pic = wid == 1 ? "map_expel" : "map_nede";
        m.rot = 0; m.rot_field = -1;
        bool o1, o2, o3, o4, o5, o6;
        const float bgw = g.f32(wp + kWpBgSize, &o1);
        const float ox = g.f32(wp + (wid == 1 ? kWpExpelOffset : kWpNedeOffset), &o2);
        const float oy = g.f32(wp + (wid == 1 ? kWpExpelOffset : kWpNedeOffset) + 4, &o3);
        const float ws = g.f32(wp + kWpWorldScale, &o4), ms = g.f32(wp + kMpScale, &o5);
        const float gk = g.f32(gd + kGdWorld, &o6);
#if SO2R_DIAG
        m.pres = wp; m.offx = ox; m.offy = oy; m.scale = ms; m.extra = ws; m.coeff = gk;
#endif
        if (!(o1 && o2 && o3 && o4 && o5 && o6) || bgw < 1 || ws <= 0 || ms <= 0 || gk <= 0) { m.why = "world minimap numbers unreadable"; return; }
        const int ps = PictureSize(h, pic, m.w, m.h);
        if (ps < 0) { m.ok = keep; m.why = keep ? "ok" : "loading"; return; }
        if (!ps) { m.why = "world map picture missing from pictures.idx"; return; }
        { size_t i = 0; for (; pic[i]; ++i) m.area[i] = pic[i]; m.area[i] = 0; }
        // the game's own placement: the minimap camera's matrix onto the world-map texture
        bool placed = false;
        {
            const uint64_t uk = g.ptr(g.ptr(in.main_base + kUiCommonRef));
            char nm[16];
            const uint64_t ust = g.cstr(uk, nm, sizeof nm) && eq(nm, "UICommon") ? g.ptr(uk + kClassStatics) : 0;
            float pv[16];
            bool mok = Guest::objp(ust) && g.read(ust + kUcPvMatrix, pv, sizeof pv);
            float tw = 0, th = 0;
            // the game's round world minimap: the partial world minimap's presenter shows a
            // baseSize window of the world texture at minimapScale -- the same window here
            {
                const uint64_t wpm = g.ptr(ufc + kUfcWorldPartial);
                const uint64_t pp = Guest::objp(wpm) ? g.ptr(wpm + kMmPresenter) : 0;
                bool ob = false, os = false;
                const float bh = Guest::objp(pp) ? g.f32(pp + kMpBaseSize + 4, &ob) : 0;
                const float ms2 = Guest::objp(pp) ? g.f32(pp + kMpScale, &os) : 0;
                const float pth = Guest::objp(wpm) ? g.f32(wpm + kWmTexH) : 0;
                m.mini_half = 0; m.mini_r = 0;
                if (ob && os && bh > 50 && bh < 3000 && ms2 > 0.05f && ms2 < 20 && pth > 1) {
                    m.mini_half = bh / ms2 * (m.h / pth) / 2;   // in world-map picture pixels
                    m.mini_r = (m.h / pth) / ms2;               // picture pixels per minimap pixel
                }
#if SO2R_DIAG
                m.mbase = bh; m.mscale = ms2;
#endif
            }
            const uint64_t ats[2] = {kUfcWorldPartial, kUfcWorldMinimap};
            for (uint64_t at : ats) {
                const uint64_t wm = g.ptr(ufc + at);
                if (!Guest::objp(wm)) continue;
                tw = g.f32(wm + kWmTexW); th = g.f32(wm + kWmTexH);
                if (tw > 1 && th > 1) break;
            }
            const float cw = pv[3] * px + pv[7] * py + pv[11] * pz + pv[15];
#if SO2R_DIAG
            m.wtw = tw; m.wth = th; m.wcw = mok ? cw : 0;
#endif
            if (mok && tw > 1 && th > 1 && (cw > 1e-6f || cw < -1e-6f)) {
                m.pmode = 2; memcpy(m.pv, pv, sizeof pv); m.ptw = tw; m.pth = th;
                placed = PlaceOnPicture(m, px, py, pz, m.x, m.y);
            }
        }
        if (!placed) {                       // the world minimap's own scale (UIWorldMinimapPresenter)
            m.pmode = 3; m.pk = gk * ms * ws; m.pf = m.w / (bgw * ws); m.pox = ox; m.poy = oy;   // canvas -> picture
            PlaceOnPicture(m, px, py, pz, m.x, m.y);
        }
        {   // the world's icons: the round (partial) world minimap's presenter, else the world presenter
            const uint64_t wpm = g.ptr(ufc + kUfcWorldPartial);
            const uint64_t pp = Guest::objp(wpm) ? g.ptr(wpm + kMmPresenter) : 0;
            const float bh = Guest::objp(pp) ? g.f32(pp + kMpBaseSize + 4) : 0;
            BeginMarks(m);
            ReadMarks(m, g, pp, 1, m.mini_r, bh / 2, bh / 2, true);
            float wb[2] = {0, 0};               // the whole-world minimap's size (minimap pixels)
            if (Guest::objp(wp)) g.read(wp + kMpBaseSize, wb, sizeof wb);
            m.wbase = wb[0] > 50 && wb[0] < 3000 ? wb[0] : 350;
            // the scale comes from the player's icon on that minimap; until it is read, the value
            // measured on the Thor (0.4.5: the game's dots against the player, 5.2 picture px each)
            ReadMarks(m, g, wp, 2, m.mini_r, 0, 0, false, 5.2f);
            EndMarks(m);
            WorldIconCache(m, ObjId{in.obj_row, in.obj_prog, in.save_key});
            // 0.9.42 the location the game marks: its "!" replaces the minimap's (read or kept)
            ScanMarked(g, fm, true, m.worldmap);
            if (g_marked.n > 0) {
                int keep = 0;
                for (int i = 0; i < m.nmarks; ++i) if (m.marks[i].type != 9) m.marks[keep++] = m.marks[i];
                m.nmarks = keep;
                m.marked = AddMarked(m);
            }
        }
#if SO2R_DIAG
        m.wp = placed ? 2 : 3;
#endif
        m.minimap_id = 0;
    } else {                                               // an area
        m.kind = 1;
        if (m.fieldmap <= 0 || m.fieldmap >= kFieldmapCount || !kFieldmapName[m.fieldmap][0]) { m.why = "area unknown"; return; }
        const uint64_t mgr = g.ptr(ufc + kUfcFieldMinimap);
        const uint64_t pres = g.ptr(mgr + kMmPresenter);
        const uint64_t task = g.ptr(pres + kMpTask);
        bool oid = false;
        const int mid = g.i32(mgr + kMmCurrentId, &oid);
#if SO2R_DIAG
        m.mgr = mgr; m.pres = pres; m.task = task;
#endif
        if (!g.is(mgr, "FieldMinimapManager") || !g.is(pres, "UIMinimapPresenter")) { m.why = "no area minimap"; return; }
        if (!oid || mid <= 0 || mid > 999) { m.why = "no minimap for this area"; return; }
        if (!__atomic_load_n(&g_area_tint_ok, __ATOMIC_ACQUIRE)) {
            const uint64_t bg = g.ptr(pres + kMpBackground);
            float c[4];
            if (Guest::objp(bg) && g.read(bg + kGraphicColor, c, sizeof c)) {
                bool good = true;
                for (float v : c) good = good && v == v && v >= 0.0f && v <= 1.0f;
                if (good && c[3] > 0.05f) {
                    for (int i = 0; i < 4; ++i) g_area_tint[i] = c[i];
                    __atomic_store_n(&g_area_tint_ok, 1, __ATOMIC_RELEASE);
                }
            }
        }
        m.minimap_id = mid;
        if (m.rot_field != m.fieldmap || m.rot_id != mid) {
        m.rot = 0;
            const uint64_t smp = g.ptr(in.pm + kPmMinimapParam);
            const uint64_t list = g.is(smp, "ScriptableMinimapParameter") ? g.ptr(smp + kMpList) : 0;
            bool ok = false;
            const int32_t n = Guest::objp(list) ? g.i32(list + kListSize, &ok) : 0;
            const uint64_t items = ok ? g.ptr(list + kListItems) : 0;
            if (ok && Guest::objp(items)) { m.rot_field = m.fieldmap; m.rot_id = mid; }   // looked up: keep it
            for (int32_t i = 0; ok && Guest::objp(items) && i < n && i < 1024; ++i) {
                const uint64_t row = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
                if (!Guest::objp(row) || g.i32(row + kCmpField) != m.fieldmap || g.i32(row + kCmpId) != mid) continue;
                bool oa = false;
                const float a = g.f32(row + kCmpAngle, &oa);
                // Unity turns the picture counter-clockwise by the angle; the map view turns clockwise
                if (oa) { m.rot = 360.0f - a; if (m.rot >= 360.0f) m.rot -= 360.0f; }
                break;
            }
        }
        if (m.place_field != m.fieldmap || m.place_mid != mid) {   // the area's name, once per area
            if ((m.place_age++ % 30) == 0) {                         // (retried every ~3 s until found)
                m.place[0] = 0; m.place_id[0] = 0; m.place_id2[0] = 0; m.place_src = 0;
                // a message key -> its text: the location messages first, then the system messages
                auto name_of = [&](uint64_t key, int src_base) -> bool {
                    uint16_t kw[40];
                    const int kl = key ? g.utf16(key, kw, 40) : 0;
                    if (kl <= 0 || kl >= 40) return false;
                    uint64_t v = DictFind(g, g.ptr(in.pm + kPmLocMsg), kw, kl);
                    if (g.is(v, "ConstLocationMessage")) { g.string(g.ptr(v + kClmName), m.place, sizeof m.place, true); m.place_src = src_base + 1; }
                    else if (g.is(v = DictFind(g, g.ptr(in.pm + kPmSysMsg), kw, kl), "ConstSystemMessage")) {
                        g.string(g.ptr(v + kCsmMessage), m.place, sizeof m.place, true); m.place_src = src_base + 2;
                    }
                    return m.place[0] != 0;
                };
                bool listed = false;
                {   // the area's field parameter rows (one per story stretch): the first with a name
                    const uint64_t sfp = g.ptr(in.pm + kPmFieldParam);
                    const uint64_t list = g.is(sfp, "ScriptableFieldParameter") ? g.ptr(sfp + kMpList) : 0;
                    bool ok = false;
                    const int32_t n = Guest::objp(list) ? g.i32(list + kListSize, &ok) : 0;
                    const uint64_t items = ok ? g.ptr(list + kListItems) : 0;
                    listed = ok && Guest::objp(items);
                    for (int32_t i = 0; listed && i < n && i < 4096 && !m.place[0]; ++i) {
                        const uint64_t row = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
                        if (!Guest::objp(row) || g.i32(row + kCfpField) != m.fieldmap) continue;
                        const uint64_t k1 = g.ptr(row + kCfpNameId), k2 = g.ptr(row + kCfpGuideId);
                        if (!m.place_id[0]) g.string(k1, m.place_id, sizeof m.place_id);
                        if (!m.place_id2[0]) g.string(k2, m.place_id2, sizeof m.place_id2);
                        if (!name_of(k1, 0)) name_of(k2, 2);
                    }
                }
                {   // the floor: the minimap row's messageID (MINIMAP_0001 = "1F" ...), empty on one-floor maps
                    m.floor[0] = 0; m.floor_id[0] = 0;
                    const uint64_t smp = g.ptr(in.pm + kPmMinimapParam);
                    const uint64_t list = g.is(smp, "ScriptableMinimapParameter") ? g.ptr(smp + kMpList) : 0;
                    bool ok = false;
                    const int32_t n = Guest::objp(list) ? g.i32(list + kListSize, &ok) : 0;
                    const uint64_t items = ok ? g.ptr(list + kListItems) : 0;
                    for (int32_t i = 0; ok && Guest::objp(items) && i < n && i < 1024; ++i) {
                        const uint64_t row = g.ptr(items + kArrItems + static_cast<uint64_t>(i) * 8);
                        if (!Guest::objp(row) || g.i32(row + kCmpField) != m.fieldmap || g.i32(row + kCmpId) != mid) continue;
                        const uint64_t key = g.ptr(row + kCmpMessageId);
                        uint16_t kw[40];
                        const int kl = key ? g.utf16(key, kw, 40) : 0;
                        if (kl > 0 && kl < 40) {
                            g.string(key, m.floor_id, sizeof m.floor_id);
                            uint64_t v = DictFind(g, g.ptr(in.pm + kPmSysMsg), kw, kl);
                            if (g.is(v, "ConstSystemMessage")) g.string(g.ptr(v + kCsmMessage), m.floor, sizeof m.floor, true);
                            else if (g.is(v = DictFind(g, g.ptr(in.pm + kPmLocMsg), kw, kl), "ConstLocationMessage"))
                                g.string(g.ptr(v + kClmName), m.floor, sizeof m.floor, true);
                        }
                        break;
                    }
                }
                // found, or the game's table has no name key for it: do not look again here
                if ((m.place[0] || (listed && !m.place_id[0] && !m.place_id2[0])) && (m.floor[0] || !m.floor_id[0])) { m.place_field = m.fieldmap; m.place_mid = mid; }
            }
        }
        Lower(lower, kFieldmapName[m.fieldmap], sizeof lower);
        Str n; n.add("minimap_").add(lower);
        if (mid < 100) n.ch('0');
        if (mid < 10) n.ch('0');
        n.num(mid);
        g_map_file_bad = false;
        const int ps = PictureSize(h, n.s, m.w, m.h);
        if (ps < 0) { m.ok = keep; m.why = keep ? "ok" : "loading"; return; }
        if (!ps) {
            m.why = g_map_file_bad ? "this area's map file is not the one in pictures.idx (re-index with the update)"
                                   : "this area has no minimap";
            return;
        }
        bool o1, o2, o3, o4, o5;
        const float ox = g.f32(task + kTaskOffset, &o1), oy = g.f32(task + kTaskOffset + 4, &o2);
        const float ms = g.f32(pres + kMpScale, &o3), gk = g.f32(gd + kGdField, &o4);
        [[maybe_unused]] const float tk = g.f32(task + kTaskCoeff, &o5);
#if SO2R_DIAG
        m.offx = ox; m.offy = oy; m.scale = ms; m.coeff = tk; m.euler = g.f32(pres + kMpEuler + 8); m.extra = gk;
        m.wp = g.u8(task + kTaskWorldPartial);
#endif
        if (!g.is(task, "UIScrollMinimapBackgroundTask")) { m.why = "minimap not shown yet"; return; }
        if (!(o1 && o2 && o3 && o4 && o5) || ms <= 0 || gk <= 0) { m.why = "minimap numbers unreadable"; return; }
        float k = gk * ms;
        {   // 0.6.5: the game moves its background by (position + offset) * coefficient in the
            // background's own space; the picture is drawn at the background image's size and scale,
            // which is not always the texture's (Allen's Mansion: about 3/4), so picture pixels per
            // unit = k * texture width / (drawn width * scale)
            const uint64_t bg = g.ptr(pres + kMpBackground);
            float cur[2] = {0, 0}, base[2] = {0, 0}, sc[3] = {1, 1, 1};
            const bool isimg = Guest::objp(bg);
            if (isimg) { g.read(bg + kGiCurSize, cur, sizeof cur); g.read(bg + kGiBaseSize, base, sizeof base); }
            const bool oks = isimg && NativeLocalScale(g, bg, sc);
            auto sane = [&](const float* v) { return v[0] == v[0] && v[1] == v[1] && v[0] > 16 && v[0] < 16384 && v[1] > 16 && v[1] < 16384; };
            const float* sz = sane(cur) ? cur : sane(base) ? base : nullptr;
            const float dw = (sz ? sz[0] : m.w) * (oks ? sc[0] : 1);
            float fac = dw > 1 && m.w > 1 ? m.w / dw : 1;
            if (!(fac > 0.2f && fac < 5)) fac = 1;
            k *= fac;
#if SO2R_DIAG
            m.bgcur[0] = cur[0]; m.bgcur[1] = cur[1]; m.bgbase[0] = base[0]; m.bgbase[1] = base[1];
            m.bgsc[0] = sc[0]; m.bgsc[1] = sc[1]; m.bgsc[2] = sc[2]; m.bgfac = fac; m.bgk = k;
            m.bgsrc = !isimg ? 0 : (sz == cur ? 1 : sz == base ? 2 : 3) + (oks ? 10 : 0);
#endif
        }
        memcpy(m.area, n.s, sizeof m.area - 1);
        m.area[sizeof m.area - 1] = 0;
        m.pmode = 1; m.pk = k; m.pox = ox; m.poy = oy;
        PlaceOnPicture(m, px, py, pz, m.x, m.y);
        {   // the game's minimap window (baseSize): icons pinned to its edge are left out
            float bs[2] = {0, 0};
            g.read(pres + kMpBaseSize, bs, sizeof bs);
            const bool okw = bs[0] > 50 && bs[0] < 3000 && bs[1] > 50 && bs[1] < 3000;
            // picture pixels per minimap pixel = k / the task's coefficient (minimap pixels per unit)
            m.area_half = okw && tk > 0.01f ? (bs[0] > bs[1] ? bs[0] : bs[1]) / 2 * (k / tk) : 0;
            BeginMarks(m);
            DestIdentity(g_dest, m.area, ObjId{in.obj_row, in.obj_prog, in.save_key});   // 0.9.38
            LookupTableDest(g, in.pm, m.fieldmap, in.obj_hero, in.obj_prog);            // 0.9.40
            ReadMarks(m, g, pres, 1, 0, okw ? bs[0] / 2 : 0, okw ? bs[1] / 2 : 0, false);
            ScanMarked(g, fm, false, m.fieldmap);        // 0.9.42 before EndMarks (it decides the tracked "!")
            EndMarks(m, &g);
            if (g_marked.n > 0) m.marked = AddMarked(m);
        }
    }
    m.ok = 1;
    m.why = "ok";
}

// ---------------------------------------------------------------- publishing
void PicKey(Str& s, const char* prefix, int id, int digits) {
    s.clear().add("module:pic/").add(prefix);
    char d[8]; int k = 0;
    for (int i = 0; i < digits; ++i) { d[digits - 1 - i] = static_cast<char>('0' + id % 10); id /= 10; ++k; }
    d[k] = 0;
    s.add(d);
}

void Publish(Instance& in, const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    Str n, v;
    // 0.9.82 the Battle page's order list (tapped Y, or the game's own list open)
    const bool picking = in.ok && in.scene == 6 && (__atomic_load_n(&g_opick, __ATOMIC_ACQUIRE) || in.op_open);
    h->publish_i64(u, "so2r.ok", in.ok);
    h->publish_text(u, "so2r.why", in.why ? in.why : "");
    h->publish_i64(u, "so2r.n", in.ok ? in.rows : 0);
    h->publish_i64(u, "so2r.scene", in.scene);
    for (int i = 0; i < kSysWordCount; ++i)
        h->publish_text(u, kSysWords[i].point, in.words.found[i] ? in.words.text[i] : kSysWords[i].fallback);
    h->publish_text(u, "so2r.map.place", in.map.ok && in.map.kind == 1 ? in.map.place : "");
    h->publish_i64(u, "so2r.map.hasplace", in.map.ok && in.map.kind == 1 && in.map.place[0]);
    h->publish_text(u, "so2r.map.floor", in.map.ok && in.map.kind == 1 ? in.map.floor : "");
    h->publish_i64(u, "so2r.map.hasfloor", in.map.ok && in.map.kind == 1 && in.map.floor[0]);
    if (in.map.ok) {
        h->publish_text(u, "so2r.map.title", Word(in, in.map.kind == 2 ? kWorldWord : kHereWord));
        // 0.9.17 the map's heading: the world: World Map; a town / dungeon: just its name (Current Location
        // when the game gives it none)
        h->publish_text(u, "so2r.map.head", in.map.kind == 2 ? Word(in, kWorldWord)
                                            : in.map.place[0] ? in.map.place : Word(in, kHereWord));
    }
    {
        const int side = __atomic_load_n(&g_side, __ATOMIC_RELAXED);
        h->publish_i64(u, "so2r.side", side);
        h->publish_i64(u, "so2r.goparty", __atomic_load_n(&g_goparty, __ATOMIC_RELAXED));
    }
    {   // 0.9.19 the quick menu's presses (the manifest's enforce rules press while one reads 1)
        for (int b = 0; b <= 12; ++b) { Str k; k.add("so2r.q.f").num(b); h->publish_i64(u, k.s, g_qm.fire == b); }   // 0.9.64 + A, L, R; 0.9.86 + ZR, ZR+stick down / up
        h->publish_i64(u, "so2r.q.any", g_qm.fire >= 0);
        h->publish_i64(u, "so2r.q.open", in.win_n);
    }
    h->publish_text(u, "so2r.map.obj", in.ok ? in.obj : "");      // 0.9.24 the next objective
    h->publish_i64(u, "so2r.map.hasobj", in.ok && in.obj[0]);
    {   // 0.9.19 the quick menu's IC/Specialty: the game's own two words (SYS_0014 / SYS_0015)
        Str q; q.add(Word(in, kIcWord)).ch('/').add(Word(in, kSpecWord));
        h->publish_text(u, "so2r.t.qic", q.s);
    }
    {   // the map, as Dave the Diver's dive map does it: the picture is an image with a bound view
        // (zoom, centre 0..1) that is re-applied every tick (so2r.map.vreset is the tick count), so
        // a tap shows at once even while the player stands still; the arrow goes through the same view.
        MapState& m = in.map;
        const bool narrow = __atomic_load_n(&g_side, __ATOMIC_RELAXED) != 0;
        const float mw = narrow ? kMapNarrowW : kMapRectW, mh = narrow ? kMapNarrowH : kMapRectH;
        const float ox0 = narrow ? kMapNarrowX : kMapX, oy0 = narrow ? kMapNarrowY : kMapY;
        // towns and dungeons: always the whole area; the world map: Minimap / Whole map by tap
        if (m.ok) __atomic_store_n(&g_view_kind, m.kind, __ATOMIC_RELAXED);
        // the world: Minimap / Whole map by tap; towns and dungeons: whole area / the game's minimap size
        const bool whole = m.kind == 2 ? __atomic_load_n(&g_map_whole, __ATOMIC_RELAXED) != 0
                                       : !(__atomic_load_n(&g_town_zoom, __ATOMIC_RELAXED) && m.area_half > 1);
        h->publish_i64(u, "so2r.map.whole", whole);
        h->publish_i64(u, "so2r.map.ok", m.ok);
        h->publish_text(u, "so2r.map.why", m.why ? m.why : "");
        h->publish_i64(u, "so2r.map.kind", m.ok ? m.kind : 0);
        h->publish_i64(u, "so2r.map.vreset", static_cast<int64_t>(in.samples));
        if (!m.ok) for (int i = 0; i < kMaxMarks; ++i) { Str k4; k4.add("so2r.mk.on").num(i); h->publish_i64(u, k4.s, 0); }
        if (m.ok && h->publish_f64) {
            const int turn = static_cast<int>(m.rot + 0.5f) % 360;
            Str key; key.add("module:pic/@map:").add(m.area).ch(':').num(turn);
            if (narrow) key.add(":n");
            h->publish_text(u, "so2r.map.img", key.s);
            const MapCanvas c = MakeCanvas(m.w, m.h, static_cast<float>(turn), narrow, starts(m.area, "minimap_") ? 2.0f : 1.0f);
            float pu, pv;
            c.ToCanvas(m.x, m.y, pu, pv);
            float z = 1, zx = 0.5f, zy = 0.5f;
            const int wm = m.kind == 2 ? (eq(m.area, "map_expel") ? 0 : 1) : -1;
            if (whole && wm >= 0 && g_land[wm].ok) {       // the whole world: framed to its land
                const LandBox& lb = g_land[wm];
                float u0, v0, u1, v1;
                c.ToCanvas(lb.x0, lb.y1, u0, v0);
                c.ToCanvas(lb.x1, lb.y0, u1, v1);
                const float du = (u1 - u0) * 1.06f, dv = (v1 - v0) * 1.06f;
                z = 1.0f / (du > dv ? du : dv);
                zx = (u0 + u1) / 2; zy = (v0 + v1) / 2;
            } else if (whole && m.kind != 2) {              // a town / dungeon: framed to its drawn part
                AreaBox ab{};
                const int s0 = __atomic_load_n(&g_abox_seq, __ATOMIC_ACQUIRE);
                const int nb = __atomic_load_n(&g_abox_n, __ATOMIC_ACQUIRE);
                for (int i = 0; i < nb && i < 16; ++i) if (eq(g_abox[i].name, m.area)) { memcpy(&ab, &g_abox[i], sizeof ab); break; }
                const int s1 = __atomic_load_n(&g_abox_seq, __ATOMIC_ACQUIRE);
                if (!(s0 & 1) && s0 == s1 && ab.b.ok && eq(ab.name, m.area)) {
                    float u0, v0, u1, v1, u2, v2, u3, v3;     // all four corners (the picture may be turned)
                    c.ToCanvas(ab.b.x0, ab.b.y0, u0, v0); c.ToCanvas(ab.b.x1, ab.b.y1, u1, v1);
                    c.ToCanvas(ab.b.x0, ab.b.y1, u2, v2); c.ToCanvas(ab.b.x1, ab.b.y0, u3, v3);
                    auto mn = [](float a, float b, float c2, float d) { float r = a < b ? a : b; r = r < c2 ? r : c2; return r < d ? r : d; };
                    auto mx = [](float a, float b, float c2, float d) { float r = a > b ? a : b; r = r > c2 ? r : c2; return r > d ? r : d; };
                    const float ua = mn(u0, u1, u2, u3), ub = mx(u0, u1, u2, u3), va = mn(v0, v1, v2, v3), vb = mx(v0, v1, v2, v3);
                    const float du = (ub - ua) * 1.12f, dv = (vb - va) * 1.12f;
                    if (du > 0.01f && dv > 0.01f) { z = 1.0f / (du > dv ? du : dv); zx = (ua + ub) / 2; zy = (va + vb) / 2; }
                }
            } else if (!whole && m.kind == 2) {             // the world minimap: centred on the player
                z = c.ch / (2 * (m.mini_half > 1 ? m.mini_half : kMiniHalfWorld));
                zx = pu; zy = pv;
            } else if (!whole) {                            // a town zoomed: the game's minimap window
                z = c.ch / (2 * m.area_half);
                zx = pu; zy = pv;
            }
            if (z < 1) z = 1;
            if (z > kMapMaxZoom) z = kMapMaxZoom;
            auto keep = [&](float cc) { const float lo = 0.5f / z; return cc < lo ? lo : cc > 1 - lo ? 1 - lo : cc; };
            zx = keep(zx); zy = keep(zy);
            h->publish_f64(u, "so2r.map.zoom", z);
            h->publish_f64(u, "so2r.map.zx", zx);
            h->publish_f64(u, "so2r.map.zy", zy);
            // the arrow: its place in the widget (pixels from the widget's top-left) and its turn
            // 0.9.17: canvas pixels (the map's own left edge moves with the sidebar)
            const float ax = ((pu - zx) * z + 0.5f) * mw, ay = ((pv - zy) * z + 0.5f) * mh;
            const bool aon = ax >= 0 && ay >= 0 && ax <= mw && ay <= mh;
            h->publish_i64(u, "so2r.map.ax", RoundF(ox0 + ax));
            h->publish_i64(u, "so2r.map.ay", RoundF(oy0 + ay));
            h->publish_i64(u, "so2r.map.aon", aon);
            h->publish_f64(u, "so2r.map.adeg", m.heading + static_cast<float>(turn));
            // the game's minimap icons, through the same view
            int shown = 0;
            for (int i = 0; i < m.nmarks && shown < kMaxMarks; ++i) {
                float mu, mv;
                c.ToCanvas(m.marks[i].x, m.marks[i].y, mu, mv);
                const float mx = ((mu - zx) * z + 0.5f) * mw, my = ((mv - zy) * z + 0.5f) * mh;
                // 0.9.154 only icons whole inside the map's lines (half an icon in from each edge), so none sits
                // on the panel's line when zoomed
                const int ty0 = m.marks[i].type;
                const float half = (ty0 == 2 || ty0 == 3 || ty0 == 4) ? 14.0f : 22.0f;
                if (mx < half || my < half || mx > mw - half || my > mh - half) continue;
                Str k1; k1.add("so2r.mk.x").num(shown); h->publish_i64(u, k1.s, RoundF(ox0 + mx));
                Str k2; k2.add("so2r.mk.y").num(shown); h->publish_i64(u, k2.s, RoundF(oy0 + my));
                Str k3; k3.add("so2r.mk.src").num(shown);
                Str v3;
                if (m.marks[i].argb) {      // the game's colour (enemies purple): a tinted copy of the picture
                    v3.add("module:pic/@tint:").add(kIconPic[m.marks[i].type]).ch(':');
                    for (int sh = 20; sh >= 0; sh -= 4) v3.ch("0123456789ABCDEF"[m.marks[i].argb >> sh & 0xF]);
                } else {
                    v3.add("module:pic/").add(kIconPic[m.marks[i].type]);
                }
                h->publish_text(u, k3.s, v3.s);
                Str k5; k5.add("so2r.mk.c").num(shown); h->publish_i64(u, k5.s, m.marks[i].argb);
                {   // the game draws party / enemy / NPC dots smaller than the fixed icons
                    const int ty = m.marks[i].type;
                    Str k6; k6.add("so2r.mk.s").num(shown);
                    h->publish_i64(u, k6.s, ty == 2 || ty == 3 || ty == 4 ? 600 : 1000);   // scale_bind: x1000
                }
                Str k4; k4.add("so2r.mk.on").num(shown); h->publish_i64(u, k4.s, 1);
                ++shown;
            }
            for (int i = shown; i < kMaxMarks; ++i) { Str k4; k4.add("so2r.mk.on").num(i); h->publish_i64(u, k4.s, 0); }
            h->publish_i64(u, "so2r.mk.n", shown);
            // the picture position in source pixels (diag / tests)
            h->publish_f64(u, "so2r.map.x", m.x);
            h->publish_f64(u, "so2r.map.y", m.y);
            h->publish_f64(u, "so2r.map.heading", m.heading);
            h->publish_f64(u, "so2r.map.rot", m.rot);
            h->publish_text(u, "so2r.map.area", m.area);
        }
    }
    if (in.ok) {
        h->publish_i64(u, "so2r.fol", in.fol);
        // 0.9.154 the coin just left of the number (right-aligned at 1224): its width from the digits and commas
        // (scale 8: a digit about 28 px, a comma about 10, measured on the Thor), a 12 px gap, the coin 60 wide
        int nd = 0;
        for (int64_t f = in.fol > 0 ? in.fol : 0; ; f /= 10) { ++nd; if (f < 10) break; }
        const int w = nd * 28 + ((nd - 1) / 3) * 10;
        h->publish_i64(u, "so2r.fol.cx", 1224 - w - 12 - 60);
    }
    for (int i = 0; i < kRows; ++i) {
        const Member& r = in.m[i];
        const bool on = in.ok && i < in.rows && r.on;
        n.clear().add("so2r.on").num(i); h->publish_i64(u, n.s, on ? 1 : 0);
        if (!on) continue;
        n.clear().add("so2r.pid").num(i); h->publish_i64(u, n.s, r.pid);
        n.clear().add("so2r.name").num(i); h->publish_text(u, n.s, r.name);
        n.clear().add("so2r.lv").num(i); h->publish_i64(u, n.s, r.level);
        n.clear().add("so2r.hp").num(i); h->publish_i64(u, n.s, r.hp);
        n.clear().add("so2r.hpmax").num(i); h->publish_i64(u, n.s, r.hpmax);
        n.clear().add("so2r.mp").num(i); h->publish_i64(u, n.s, r.mp);
        n.clear().add("so2r.mpmax").num(i); h->publish_i64(u, n.s, r.mpmax);
        {   // 0.9.57 the game's "974/ 974": the maximum after its slash, padded to four places
            Str t;
            for (int k = 0; k < 2; ++k) {
                const int mx = k ? r.mpmax : r.hpmax;
                t.clear().ch('/');
                for (int d = mx < 10 ? 3 : mx < 100 ? 2 : mx < 1000 ? 1 : 0; d > 0; --d) t.ch(' ');
                t.num(mx);
                n.clear().add(k ? "so2r.mpmx" : "so2r.hpmx").num(i); h->publish_text(u, n.s, t.s);
            }
        }
        n.clear().add("so2r.lead").num(i); h->publish_i64(u, n.s, r.leader);
        n.clear().add("so2r.guest").num(i); h->publish_i64(u, n.s, r.leader ? 0 : r.guest);
        // the game's own dot portrait of this character (common_sprite_atlas icon_dot_<CharacterID>)
        PicKey(v, "icon_dot_", r.cid, 4);
        n.clear().add("so2r.dot").num(i); h->publish_text(u, n.s, v.s);
        n.clear().add("so2r.sel").num(i); h->publish_i64(u, n.s, i == __atomic_load_n(&g_sel, __ATOMIC_RELAXED));
    }
    {   // 0.9.17 the Map page's strip: the battle members (four at most, the party's order); tap: their details
        int k = 0;
        for (int i = 0; i < kRows && k < 4; ++i) {
            const Member& r = in.m[i];
            if (!(in.ok && i < in.rows && r.on && r.battle)) continue;
            __atomic_store_n(&g_brow[k], i, __ATOMIC_RELAXED);
            n.clear().add("so2r.bon").num(k); h->publish_i64(u, n.s, 1);
            n.clear().add("so2r.bname").num(k); h->publish_text(u, n.s, r.name);
            n.clear().add("so2r.blv").num(k); h->publish_i64(u, n.s, r.level);
            n.clear().add("so2r.bhp").num(k); h->publish_i64(u, n.s, r.hp);
            n.clear().add("so2r.bhpmax").num(k); h->publish_i64(u, n.s, r.hpmax);
            n.clear().add("so2r.bmp").num(k); h->publish_i64(u, n.s, r.mp);
            n.clear().add("so2r.bmpmax").num(k); h->publish_i64(u, n.s, r.mpmax);
            n.clear().add("so2r.blead").num(k); h->publish_i64(u, n.s, r.leader);
            n.clear().add("so2r.bguest").num(k); h->publish_i64(u, n.s, r.leader ? 0 : r.guest);
            PicKey(v, "icon_dot_", r.cid, 4);
            n.clear().add("so2r.bdot").num(k); h->publish_text(u, n.s, v.s);
            ++k;
        }
        h->publish_i64(u, "so2r.bn", k);              // 0.9.25 the strip's cards share the width by this count
        for (; k < 4; ++k) {
            __atomic_store_n(&g_brow[k], -1, __ATOMIC_RELAXED);
            n.clear().add("so2r.bon").num(k); h->publish_i64(u, n.s, 0);
            n.clear().add("so2r.blead").num(k); h->publish_i64(u, n.s, 0);
            n.clear().add("so2r.bguest").num(k); h->publish_i64(u, n.s, 0);
        }
    }
    {   // 0.9.10 the Party tab: the roster rows (and their LEADER / GUEST), or the details' picker
        const int vw = __atomic_load_n(&g_view, __ATOMIC_RELAXED), sl = __atomic_load_n(&g_sel, __ATOMIC_RELAXED);
        int shown = 0;
        for (int i = 0; i < kRows; ++i) shown += in.ok && i < in.rows && in.m[i].on;
        h->publish_i64(u, "so2r.cardsw", shown > 0 ? shown * 304 - 8 : 0);   // the picker cards' width, for sideways scrolling
        h->publish_i64(u, "so2r.rostw", shown > 0 ? shown * kRosterDx - (kRosterDx - kRosterW) : 0);   // the roster cards'

        for (int i = 0; i < kRows; ++i) {
            const bool row = in.ok && i < in.rows && in.m[i].on;
            const Member& r = in.m[i];
            n.clear().add("so2r.ro").num(i); h->publish_i64(u, n.s, row && vw == 0);
            n.clear().add("so2r.rlead").num(i); h->publish_i64(u, n.s, row && vw == 0 && r.leader);
            n.clear().add("so2r.rguest").num(i); h->publish_i64(u, n.s, row && vw == 0 && !r.leader && r.guest);
            n.clear().add("so2r.po").num(i); h->publish_i64(u, n.s, row && vw == 1);
            if (row && vw == 0) {   // the card's illustration and the level half circle (next-level ratio)
                // 0.9.16 the game's Party Formation picture: charaillust_NNNN (NNNN = PlayerID) placed like the game
                v.clear().add("module:pic/@ill:");
                if (r.pid < 1000) v.ch('0');
                if (r.pid < 100) v.ch('0');
                if (r.pid < 10) v.ch('0');
                v.num(r.pid);
                n.clear().add("so2r.ill").num(i); h->publish_text(u, n.s, v.s);
                const int lb = r.level_base > 0 ? r.level_base : r.level;
                const bool top = in.max_level > 0 && lb >= in.max_level;
                const int64_t lo = NeedExp(lb - 1), hi = NeedExp(lb);
                int pm = top ? 1000 : hi > lo ? static_cast<int>((r.exp - lo) * 1000 / (hi - lo)) : 0;
                pm = pm < 0 ? 0 : pm > 1000 ? 1000 : pm;
                pm = pm / 10 * 10;   // 1% steps (one picture each)
                v.clear().add("module:pic/@lvarc:").num(pm);
                n.clear().add("so2r.lvarc").num(i); h->publish_text(u, n.s, v.s);
            }
            n.clear().add("so2r.psel").num(i); h->publish_i64(u, n.s, row && vw == 1 && (i == sl || (sl >= in.rows && i == 0)));
        }
    }
    // 0.9.157 the ailments on a battle member (BuffDebuffState, r19: BuffDebuffID 1-6; the game's battle icons
    // battle_icon_abnormal_*, as on its battle HUD's party rows; Stun has none there), packed in the game's order
    auto ailments = [&](const char* pre, const Member* r) {
        static constexpr const char* kAbn[7] = {"", "battle_icon_abnormal_poison", "battle_icon_abnormal_paralyze",
            "battle_icon_abnormal_petrifaction", "battle_icon_abnormal_confusion", "battle_icon_abnormal_silence", ""};
        int na = 0;
        for (int id = 1; id <= 6 && r && na < 3; ++id) {
            if (!(r->bd >> id & 1) || !kAbn[id][0]) continue;
            v.clear().add("module:pic/").add(kAbn[id]);
            n.clear().add(pre).add("abi").num(na); h->publish_text(u, n.s, v.s);
            n.clear().add(pre).add("abon").num(na); h->publish_i64(u, n.s, 1);
            ++na;
        }
        for (; na < 3; ++na) { n.clear().add(pre).add("abon").num(na); h->publish_i64(u, n.s, 0); }
    };
    {   // 0.9.70 the Battle page: the leader (featured, the Party Formation picture), the other battle members (3 at
        // most, the party's order), the leader's L / R arts, in a battle or not
        int lead = -1;
        for (int i = 0; i < in.rows && i < kRows; ++i) if (in.ok && in.m[i].on && in.m[i].leader) { lead = i; break; }
        // 0.9.72 in a battle: the member the player controls (BattleManager's controlPlayerIndex), not the leader
        if (in.scene == 6 && in.ctrl_cid >= 0)
            for (int i = 0; i < in.rows && i < kRows; ++i) if (in.ok && in.m[i].on && in.m[i].cid == in.ctrl_cid) { lead = i; break; }
        h->publish_i64(u, "so2r.bt.islead", lead >= 0 && in.m[lead].leader);
        const bool on = lead >= 0;
        h->publish_i64(u, "so2r.bt.on", on);
        h->publish_i64(u, "so2r.bt.inb", in.ok && in.scene == 6);
        {   // 0.9.121 the tapped button's light
            static int lit = -1; static uint64_t lit_at;
            const int rq = __atomic_exchange_n(&g_lit_req, 0, __ATOMIC_ACQ_REL);
            if (rq > 0) { lit = rq - 1; lit_at = in.samples; }
            if (lit >= 0 && in.samples - lit_at > static_cast<uint64_t>(kLitSamples)) lit = -1;
            static const char* const kLit[20] = {"so2r.bt.lit0", "so2r.bt.lit1", "so2r.bt.lit2", "so2r.bt.lit3", "so2r.bt.lit4",
                                                 "so2r.bt.lit5", "so2r.bt.lit6", "so2r.bt.lit7", "so2r.bt.lit8",
                                                 "so2r.bt.lit9", "so2r.bt.lit10", "so2r.bt.lit11",   // 0.9.124 9..11 Improve
                                                 "so2r.bt.lit12", "so2r.bt.lit13", "so2r.bt.lit14", "so2r.bt.lit15",
                                                 "so2r.bt.lit16", "so2r.bt.lit17", "so2r.bt.lit18",   // 0.9.135 equipment
                                                 "so2r.bt.lit19"};                                     // 0.9.152 title
            for (int k = 0; k < 20; ++k) h->publish_i64(u, kLit[k], lit == k);
            h->publish_i64(u, "so2r.hh.on", g_hh_on);   // 0.9.142 the HideHUD switch, lit while on
            {   // 0.9.144 the page follows the game (manifest page_binds): the title screen, a battle, the field / world
                const bool title = in.scene == 3 || starts(in.scene_cls, "Title");
                h->publish_i64(u, "so2r.v.title", title);
                h->publish_i64(u, "so2r.title.ready", __atomic_load_n(&g_title_ready, __ATOMIC_ACQUIRE));
                h->publish_i64(u, "so2r.v.inbattle", in.scene == 6);
                h->publish_i64(u, "so2r.v.onmap", in.scene == 4 || in.scene == 5);
            }
        }
        h->publish_i64(u, "so2r.bt.outb", in.ok && in.scene != 6);
        if (on) {
            const Member& r = in.m[lead];
            v.clear().add("module:pic/@ill:");
            if (r.pid < 1000) v.ch('0');
            if (r.pid < 100) v.ch('0');
            if (r.pid < 10) v.ch('0');
            v.num(r.pid);
            h->publish_text(u, "so2r.bt.ill", v.s);
            v.add(":").num(kBattleIllH);   // 0.9.93 the Battle page's shorter card
            h->publish_text(u, "so2r.bt.illc", v.s);
            const int lb = r.level_base > 0 ? r.level_base : r.level;
            const bool top = in.max_level > 0 && lb >= in.max_level;
            const int64_t lo = NeedExp(lb - 1), hi = NeedExp(lb);
            int pm = top ? 1000 : hi > lo ? static_cast<int>((r.exp - lo) * 1000 / (hi - lo)) : 0;
            pm = pm < 0 ? 0 : pm > 1000 ? 1000 : pm;
            v.clear().add("module:pic/@lvarc:").num(pm / 10 * 10);
            h->publish_text(u, "so2r.bt.lvarc", v.s);
            h->publish_text(u, "so2r.bt.name", r.name);
            ailments("so2r.bt.", &r);
            h->publish_i64(u, "so2r.bt.lv", r.level);
            h->publish_i64(u, "so2r.bt.hp", r.hp); h->publish_i64(u, "so2r.bt.hpmax", r.hpmax);
            h->publish_i64(u, "so2r.bt.mp", r.mp); h->publish_i64(u, "so2r.bt.mpmax", r.mpmax);
            for (int k = 0; k < 2; ++k) {
                Str t; t.ch('/');
                const int mx = k ? r.mpmax : r.hpmax;
                for (int d = mx < 10 ? 3 : mx < 100 ? 2 : mx < 1000 ? 1 : 0; d > 0; --d) t.ch(' ');
                t.num(mx);
                h->publish_text(u, k ? "so2r.bt.mpmx" : "so2r.bt.hpmx", t.s);
                const char* en = r.art_eq[k] > 0 ? ArtNameOf(r.art_eq[k]) : nullptr;
                n.clear().add("so2r.bt.art").num(k); h->publish_text(u, n.s, en ? en : Word(in, FindWord("so2r.t.none")));
            }
        }
        // 0.9.82 the order list: shown when Y is tapped here or the game's own list is open; the order in force lit
        if (in.scene != 6) __atomic_store_n(&g_opick, 0, __ATOMIC_RELAXED);
        h->publish_i64(u, "so2r.bt.opick", picking);
        for (int k = 1; k <= 4; ++k) {
            const bool has = picking && in.words.found[kOrdWord0 + k - 1];
            n.clear().add("so2r.bt.oon").num(k); h->publish_i64(u, n.s, has);
            n.clear().add("so2r.bt.ocur").num(k); h->publish_i64(u, n.s, has && in.order == k);
            n.clear().add("so2r.bt.ooff").num(k); h->publish_i64(u, n.s, has && in.order != k);
        }
        {   // 0.9.89 the bonus gauge
            const bool on = in.ok && in.bn_on && in.bn_next > 0;   // 0.9.110 outside a battle too (the last one's stages)
            h->publish_i64(u, "so2r.bn.on", on);
            h->publish_i64(u, "so2r.bn.pt", on ? in.bn_pt : 0);
            n.clear().ch('/').num(on ? in.bn_next : 0); h->publish_text(u, "so2r.bn.next", n.s);
            v.clear().add("module:pic/@bonus:").num(on ? in.bn_ratio / 20 * 20 : 0).ch(':').num(on ? in.bn_pt : 0).ch(':').num(on ? in.bn_next : 0);
            h->publish_text(u, "so2r.bn.pic", on ? v.s : "");
            // 0.9.101 the numbers are on the picture in the game's font; as text only if that font could not be read
            h->publish_i64(u, "so2r.bn.txt", on && __atomic_load_n(&g_bn_font, __ATOMIC_RELAXED) == 0);
            static constexpr const char* kIconPic[9] = {"battle_icon_atk_up", "battle_icon_def_up", "battle_icon_hit_up",
                "battle_icon_avd_up", "battle_icon_int_up", "", "", "battle_icon_crt_up", "battle_icon_guts_up"};
            for (int k = 0; k < 3; ++k) {   // 0.9.90 the effects in force
                const bool has = on && k < in.bn_n;
                n.clear().add("so2r.bn.e").num(k); h->publish_i64(u, n.s, has);
                n.clear().add("so2r.bn.et").num(k); h->publish_text(u, n.s, has ? in.bn_text[k] : "");
                const int ic = has ? in.bn_icon[k] : -1;
                v.clear();
                if (ic >= 0 && ic < 9 && kIconPic[ic][0]) v.add("module:pic/").add(kIconPic[ic]);
                n.clear().add("so2r.bn.ei").num(k); h->publish_text(u, n.s, v.s);
            }
        }
        // 0.9.81 the Y order now in force, in the game's words (blank until both are read)
        h->publish_text(u, "so2r.bt.order", in.ok && in.scene == 6 && in.order >= 1 && in.words.found[kOrdWord0 + in.order - 1]
                                               ? in.words.text[kOrdWord0 + in.order - 1] : "");
        int k = 0;
        for (int i = 0; i < kRows && k < 3; ++i) {
            const Member& r = in.m[i];
            if (!(in.ok && i < in.rows && r.on && r.battle) || i == lead) continue;
            n.clear().add("so2r.bo.on").num(k); h->publish_i64(u, n.s, 1);
            n.clear().add("so2r.bo.name").num(k); h->publish_text(u, n.s, r.name);
            n.clear().add("so2r.bo.lv").num(k); h->publish_i64(u, n.s, r.level);
            n.clear().add("so2r.bo.guest").num(k); h->publish_i64(u, n.s, r.guest ? 1 : 0);
            n.clear().add("so2r.bo.hp").num(k); h->publish_i64(u, n.s, r.hp);
            n.clear().add("so2r.bo.hpmax").num(k); h->publish_i64(u, n.s, r.hpmax);
            n.clear().add("so2r.bo.mp").num(k); h->publish_i64(u, n.s, r.mp);
            n.clear().add("so2r.bo.mpmax").num(k); h->publish_i64(u, n.s, r.mpmax);
            PicKey(v, "icon_dot_", r.cid, 4);
            n.clear().add("so2r.bo.dot").num(k); h->publish_text(u, n.s, v.s);
            __atomic_store_n(&g_bo_cid[k], r.cid, __ATOMIC_RELAXED);   // 0.9.86 for a tap on this card
            { Str pre; pre.add("so2r.bo").num(k).ch('.'); ailments(pre.s, &r); }   // 0.9.157
            ++k;
        }
        for (int j = k; j < 3; ++j) __atomic_store_n(&g_bo_cid[j], -1, __ATOMIC_RELAXED);
        for (int a = 0; a < 4; ++a) {   // 0.9.73 the assault members on their d-pad directions
            const bool shown = in.ok && a < in.as_n && kAssistBtn[a] >= 0 && in.as_name[a][0] && !picking;   // 0.9.110 any scene
            n.clear().add("so2r.as.on").num(kAssistBtn[a] < 0 ? 9 : kAssistBtn[a]);
            if (kAssistBtn[a] < 0) continue;
            h->publish_i64(u, n.s, shown);
            n.clear().add("so2r.as.name").num(kAssistBtn[a]); h->publish_text(u, n.s, shown ? in.as_name[a] : "");
            n.clear().add("so2r.as.idx").num(kAssistBtn[a]); h->publish_i64(u, n.s, a);
            n.clear().add("so2r.as.rise").num(kAssistBtn[a]); h->publish_i64(u, n.s, shown && in.as_rise[a] >= 0 ? in.as_rise[a] : 0);
            // 0.9.75 the gauge as the game's ring, in 2% steps (each step is one picture)
            // 0.9.76 a key of its own per direction (":d" is ignored when drawn): two buttons never share one picture
            v.clear().add("module:pic/@ring:").num(shown && in.as_rise[a] >= 0 ? in.as_rise[a] / 20 * 20 : 0).ch(':').num(kAssistBtn[a]);
            n.clear().add("so2r.as.ring").num(kAssistBtn[a]); h->publish_text(u, n.s, v.s);
            n.clear().add("so2r.as.full").num(kAssistBtn[a]); h->publish_i64(u, n.s, shown && in.as_rise[a] >= 1000);
            n.clear().add("so2r.as.dot").num(kAssistBtn[a]);
            // 0.9.77 the full-body pixel character: the model's number (ConstAssistParameter.modelName "cp_NNNN..."),
            // else the member's own (cp_NNNN = CharacterID for the party); the picture falls back to icon_dot
            int model = -1;
            {
                const char* m = in.as_model[a];
                if (m[0] == 'c' && m[1] == 'p' && m[2] == '_') {
                    model = 0;
                    for (int i = 3; i < 7 && m[i] >= '0' && m[i] <= '9'; ++i) model = model * 10 + (m[i] - '0');
                }
            }
            if (model <= 0 && in.as_cid[a] > 0) model = in.as_cid[a];
            if (shown && model > 0) {
                v.clear().add("module:pic/@dot:").num(model).ch(':').num(in.as_cid[a] > 0 ? in.as_cid[a] : 0);
                h->publish_text(u, n.s, v.s);
            } else h->publish_text(u, n.s, "");
        }
        for (int j = k; j < 3; ++j) { Str pre; pre.add("so2r.bo").num(j).ch('.'); ailments(pre.s, nullptr); }   // 0.9.157
        if (!on) ailments("so2r.bt.", nullptr);
        for (; k < 3; ++k) { n.clear().add("so2r.bo.on").num(k); h->publish_i64(u, n.s, 0); n.clear().add("so2r.bo.guest").num(k); h->publish_i64(u, n.s, 0); }
    }
    const int view = __atomic_load_n(&g_view, __ATOMIC_RELAXED), sub = __atomic_load_n(&g_sub, __ATOMIC_RELAXED);
    const bool det = in.ok && view == 1;
    h->publish_i64(u, "so2r.v.roster", in.ok && view == 0);
    h->publish_i64(u, "so2r.v.det", det);
    for (int t = 0; t < 4; ++t) { n.clear().add("so2r.sub").num(t); h->publish_i64(u, n.s, det && sub == t); }
    {   // 0.9.60 the game's Leader (X in Party Formation) for the chosen member; 0.9.62 on the Status subtab
        int sel = __atomic_load_n(&g_sel, __ATOMIC_RELAXED);
        if (sel < 0 || sel >= in.rows) sel = 0;
        const bool on = det && sub == 0 && in.ok && sel < in.rows && in.m[sel].on;
        const Member& r = in.m[sel];
        const bool busy = g_lm.state != 0;
        h->publish_i64(u, "so2r.f.on", on);
        h->publish_i64(u, "so2r.f.lead", on && !busy && r.leader);
        h->publish_i64(u, "so2r.f.can", on && !busy && r.battle && !r.leader && !r.guest && (in.scene == 4 || in.scene == 5));
        h->publish_i64(u, "so2r.f.not", on && !busy && !r.leader && !(r.battle && !r.guest && (in.scene == 4 || in.scene == 5)));
        h->publish_i64(u, "so2r.f.busy", on && busy);
        if (on) {
            h->publish_text(u, "so2r.f.name", r.name);
            Str v; PicKey(v, "icon_dot_", r.cid, 4);
            h->publish_text(u, "so2r.f.dot", v.s);
        }
    }
    {   // the details' Status and Equipment subtabs: the chosen member (the first when the chosen row is gone)
        int sel = __atomic_load_n(&g_sel, __ATOMIC_RELAXED);
        if (sel < 0 || sel >= in.rows) sel = 0;
        const bool on = in.ok && sel < in.rows && in.m[sel].on;
        // pg: 0 Status, 1 Status page 2 (talents, friendship, favorite food), 2 Equipment, -1 neither shown
        const int pg = !det ? -1 : sub == 0 ? __atomic_load_n(&g_spage, __ATOMIC_RELAXED) : sub == 1 ? 2 : -1;
        h->publish_i64(u, "so2r.s.on", on && pg >= 0);
        h->publish_i64(u, "so2r.s.st0on", on && pg >= 0 && pg <= 1);   // the Status subtab's header and switch
        h->publish_i64(u, "so2r.s.p0", on && pg == 0);
        h->publish_i64(u, "so2r.s.p1", on && pg == 1);
        h->publish_i64(u, "so2r.s.p2", on && pg == 2);
        {   // 0.9.147 every row's whole picture, asked for ahead (tiny, nearly clear, on every page) so the Equipment /
            // Skills pages find it decoded instead of loading it on the first visit
            static const char* const kWarm[kRows] = {"so2r.warm0", "so2r.warm1", "so2r.warm2", "so2r.warm3",
                                                     "so2r.warm4", "so2r.warm5", "so2r.warm6", "so2r.warm7"};
            for (int k = 0; k < kRows; ++k) {
                Str b;
                if (in.ok && k < in.rows && in.m[k].on && in.m[k].pid > 0) {
                    const int pid = in.m[k].pid;
                    b.add("module:pic/@body:");
                    if (pid < 1000) b.ch('0');
                    if (pid < 100) b.ch('0');
                    if (pid < 10) b.ch('0');
                    b.num(pid);
                }
                h->publish_text(u, kWarm[k], b.s);
            }
        }
        __atomic_store_n(&g_eq_view, on && pg == 2, __ATOMIC_RELAXED);   // 0.9.135 the game's Equipment follows the pick
        if (on) {
            const Member& r = in.m[sel];
            Guest g{h};
            {   // 0.9.132 the Equipment subtab: the member's whole picture at the right (as on the Skills pages)
                Str b; b.add("module:pic/@body:");
                if (r.pid < 1000) b.ch('0');
                if (r.pid < 100) b.ch('0');
                if (r.pid < 10) b.ch('0');
                b.num(r.pid);
                h->publish_text(u, "so2r.s.body", b.s);
            }
            h->publish_text(u, "so2r.s.name", r.name);
            h->publish_i64(u, "so2r.s.lv", r.level);
            h->publish_i64(u, "so2r.s.exp", r.exp);
            {   // Next Level: the game's table at LevelBase (none at the top level)
                const int lb = r.level_base > 0 ? r.level_base : r.level;
                const bool top = in.max_level > 0 && lb >= in.max_level;
                const int64_t nx = top ? 0 : NeedExp(lb) - r.exp;
                h->publish_i64(u, "so2r.s.hasnext", pg == 0 && !top && nx >= 0);
                h->publish_i64(u, "so2r.s.next", nx < 0 ? 0 : nx);
            }
            {   // favorite food (r34): the dish's name once the game marks it known, else the game's "????"
                const ItemName* it = r.food_known && r.pid > 0 && r.pid < kMaxPlayer && in.pm ? ItemNameOf(g, in.pm, in.fav_item[r.pid]) : nullptr;
                h->publish_text(u, "so2r.s.food", it && it->name[0] ? it->name : (g_elw.q_ok ? g_elw.q : "????"));
            }
            {   // talents held (the game's words), packed for a list
                int nt = 0;
                for (int id = 1; id <= 10; ++id)
                    if (r.talent >> id & 1) {
                        n.clear().add("so2r.s.tlw").num(nt++);
                        h->publish_text(u, n.s, in.words.found[kTlWord0 + id - 1] ? in.words.text[kTlWord0 + id - 1] : kSysWords[kTlWord0 + id - 1].fallback);
                    }
                for (int k = 0; k < 10; ++k) { n.clear().add("so2r.s.tlon").num(k); h->publish_i64(u, n.s, pg == 1 && k < nt); }
            }
            {   // friendship with each other member (the game's table), as a bar of the game's most
                int nf = 0;
                h->publish_i64(u, "so2r.s.frmax", in.max_friend > 0 ? in.max_friend : 1);
                for (int j = 0; j < in.rows; ++j) {
                    const Member& o = in.m[j];
                    if (j == sel || !o.on || o.pid <= 0 || o.pid >= 14 || r.pid <= 0 || r.pid >= 14) continue;
                    const int fv = in.friend_[r.pid * 14 + o.pid];
                    n.clear().add("so2r.s.fr").num(nf); h->publish_i64(u, n.s, fv);
                    {   // the game's 5 hearts, each filled in eighths (FavorabilityHeartPresenter.Set(0..8)):
                        // value / maxEmotion over 40 eighths
                        const int mx = in.max_friend > 0 ? in.max_friend : 1;
                        int pts = static_cast<int>((static_cast<int64_t>(fv < 0 ? 0 : fv) * 40 + mx / 2) / mx);
                        if (pts > 40) pts = 40;
                        for (int hk = 0; hk < 5; ++hk) {
                            int hp = pts - hk * 8; hp = hp < 0 ? 0 : hp > 8 ? 8 : hp;
                            n.clear().add("so2r.s.ht").num(nf).ch('_').num(hk); h->publish_i64(u, n.s, hp);
                        }
                    }
                    PicKey(v, "icon_dot_", o.cid, 4);
                    n.clear().add("so2r.s.frdot").num(nf); h->publish_text(u, n.s, v.s);
                    n.clear().add("so2r.s.frname").num(nf); h->publish_text(u, n.s, o.name);
                    ++nf;
                }
                for (int k = 0; k < 7; ++k) { n.clear().add("so2r.s.fron").num(k); h->publish_i64(u, n.s, pg == 1 && k < nf); }
            }
            h->publish_i64(u, "so2r.s.hp", r.hp); h->publish_i64(u, "so2r.s.hpmax", r.hpmax);
            h->publish_i64(u, "so2r.s.mp", r.mp); h->publish_i64(u, "so2r.s.mpmax", r.mpmax);
            PicKey(v, "icon_dot_", r.cid, 4);
            h->publish_text(u, "so2r.s.dot", v.s);
            for (int k = 0; k < 9; ++k) { n.clear().add("so2r.s.st").num(k); h->publish_i64(u, n.s, r.stat[k]); }
            // the element row (the game's order, icons and words)
            for (int k = 0; k < kElMax; ++k) {
                const int id = k < in.nel ? in.el_order[k] : 0;
                const int ty = id > 0 && id < r.nelem ? r.elem[id] : 0;
                n.clear().add("so2r.s.elic").num(k); h->publish_text(u, n.s, id ? ElementIcon(id) : "");
                n.clear().add("so2r.s.elw").num(k); h->publish_text(u, n.s, id ? ElementWord(in, ty) : "");
                n.clear().add("so2r.s.elon").num(k); h->publish_i64(u, n.s, pg == 0 && id > 0);
                n.clear().add("so2r.s.elbad").num(k); h->publish_i64(u, n.s, pg == 0 && id > 0 && ty == 1);
                n.clear().add("so2r.s.eel").num(k); h->publish_i64(u, n.s, pg == 2 && id > 0);   // the Equipment column
            }
            h->publish_i64(u, "so2r.s.elrow", pg == 0 && in.nel > 0);
            // the ailments on now (the game's words), packed in order for a list
            int nb = 0;
            for (int id = 1; id <= 6; ++id)
                if (r.bd >> id & 1) { n.clear().add("so2r.s.bdw").num(nb++); h->publish_text(u, n.s, in.words.found[kBdWord0 + id - 1] ? in.words.text[kBdWord0 + id - 1] : kSysWords[kBdWord0 + id - 1].fallback); }
            for (int k = 0; k < 6; ++k) { n.clear().add("so2r.s.bdon").num(k); h->publish_i64(u, n.s, pg == 0 && k < nb); }
            static int weapon_cat[kMaxPlayer];   // the last weapon category seen per character (an empty hand keeps it)
            for (int k = 0; k < kEquipSlots; ++k) {
                const ItemName* it = in.pm ? ItemNameOf(g, in.pm, r.equip_item[k]) : nullptr;
                int cat = k < 7 ? kSlotCategory[k] : 0;
                if (k == 0) {
                    if (it && it->cat >= 8 && it->cat <= 17 && r.pid > 0 && r.pid < kMaxPlayer) weapon_cat[r.pid] = it->cat;
                    cat = r.pid > 0 && r.pid < kMaxPlayer ? weapon_cat[r.pid] : 0;
                }
                if (cat > 0 && cat < 23) v.clear().add("module:pic/icon_item_").add(kItemCategoryName[cat]); else v.clear();
                n.clear().add("so2r.s.eqic").num(k); h->publish_text(u, n.s, v.s);
                n.clear().add("so2r.s.eqicon").num(k); h->publish_i64(u, n.s, pg == 2 && cat > 0);
                n.clear().add("so2r.s.eqno").num(k); h->publish_i64(u, n.s, pg == 2 && !(it && it->name[0]));
                n.clear().add("so2r.s.eq").num(k); h->publish_text(u, n.s, it && it->name[0] ? it->name : "");
                n.clear().add("so2r.s.has").num(k); h->publish_i64(u, n.s, pg == 2 && it && it->name[0] ? 1 : 0);
            }
        }
    }
    {   // the Skills page: the chosen member's skills (the game's Improve list) and the party's IC /
        // Specialty / Super Specialty, all in the game's words
        int sel = __atomic_load_n(&g_sel, __ATOMIC_RELAXED);
        if (sel < 0 || sel >= in.rows) sel = 0;
        // pg (as before): 0 skills, 1 special arts / spells, 2 combat skills, 3 IC / Specialty; -1 not shown.
        // The Arts/Spells subtab is page 1; the Skills subtab switches between 0, 2 and 3
        static constexpr int kSkillsPages[3] = {0, 2, 3};
        const int kp = __atomic_load_n(&g_kpage, __ATOMIC_RELAXED);
        const int pg = !det ? -1 : sub == 2 ? 1 : sub == 3 ? kSkillsPages[kp < 0 || kp > 2 ? 0 : kp] : -1;
        const bool on = in.ok && sel < in.rows && in.m[sel].on && g_sk.n > 0 && pg >= 0;
        h->publish_i64(u, "so2r.k.on", on);
        h->publish_i64(u, "so2r.k.sw", on && sub == 3);   // the Skills subtab's Switch Pages
        h->publish_i64(u, "so2r.k.p0", on && pg == 0);
        h->publish_i64(u, "so2r.k.p1", on && pg == 2);   // combat skills
        h->publish_i64(u, "so2r.k.p2", on && pg == 3);   // IC / Specialty / Super Specialty
        h->publish_i64(u, "so2r.k.p3", on && pg == 1);   // special arts / spells
        const bool stc = on && pg == 1;                   // the stats beside the list: the arts page (0.9.129 again)
        h->publish_i64(u, "so2r.k.st", stc);
        for (int k = 0; k < kElMax; ++k) {   // the element row in that panel (the Status page's words)
            const int id = k < in.nel ? in.el_order[k] : 0;
            n.clear().add("so2r.k.elon").num(k); h->publish_i64(u, n.s, stc && id > 0);
        }
        const Member* r = on ? &in.m[sel] : nullptr;
        // 0.9.129 the Skills / Combat Skill pages: the chosen member's Party Formation card beside the list
        h->publish_i64(u, "so2r.kc.on", r && (pg >= 0 && pg <= 2));
        h->publish_i64(u, "so2r.kc.lead", r && (pg >= 0 && pg <= 2) && r->leader);
        if (r && (pg >= 0 && pg <= 2)) {
            v.clear().add("module:pic/@ill:");
            if (r->pid < 1000) v.ch('0');
            if (r->pid < 100) v.ch('0');
            if (r->pid < 10) v.ch('0');
            v.num(r->pid).ch(':').num(kSkillIllH);
            h->publish_text(u, "so2r.kc.ill", v.s);
            v.clear().add("module:pic/@body:");                 // 0.9.130 the whole picture instead of the card
            if (r->pid < 1000) v.ch('0');
            if (r->pid < 100) v.ch('0');
            if (r->pid < 10) v.ch('0');
            v.num(r->pid);
            h->publish_text(u, "so2r.kc.body", v.s);
            const int lb = r->level_base > 0 ? r->level_base : r->level;
            const bool top = in.max_level > 0 && lb >= in.max_level;
            const int64_t lo = NeedExp(lb - 1), hi = NeedExp(lb);
            int pm = top ? 1000 : hi > lo ? static_cast<int>((r->exp - lo) * 1000 / (hi - lo)) : 0;
            pm = pm < 0 ? 0 : pm > 1000 ? 1000 : pm;
            v.clear().add("module:pic/@lvarc:").num(pm / 10 * 10);
            h->publish_text(u, "so2r.kc.lvarc", v.s);
            h->publish_i64(u, "so2r.kc.lv", r->level);
            h->publish_i64(u, "so2r.kc.hp", r->hp); h->publish_i64(u, "so2r.kc.hpmax", r->hpmax);
            h->publish_i64(u, "so2r.kc.mp", r->mp); h->publish_i64(u, "so2r.kc.mpmax", r->mpmax);
            for (int k = 0; k < 2; ++k) {
                Str t; t.ch('/');
                const int mx = k ? r->mpmax : r->hpmax;
                for (int d = mx < 10 ? 3 : mx < 100 ? 2 : mx < 1000 ? 1 : 0; d > 0; --d) t.ch(' ');
                t.num(mx);
                h->publish_text(u, k ? "so2r.kc.mpmx" : "so2r.kc.hpmx", t.s);
            }
        }
        if (r) {
            h->publish_text(u, "so2r.k.name", r->name);
            PicKey(v, "icon_dot_", r->cid, 4);
            h->publish_text(u, "so2r.k.dot", v.s);
            h->publish_i64(u, "so2r.k.sp", r->sp);
            h->publish_i64(u, "so2r.k.bp", r->bp);
        }
        {   // special arts / spells: in the game's order (its learning table for this character), each
            // learned one with Lv and the BP to improve (none when the game has no cost: -1)
            int na = 0;
            bool used[kMaxArts] = {};
            auto put_art = [&](int t) {
                const char* an = ArtNameOf(r->art_id[t]);
                if (!an || na >= 16) return;
                used[t] = true;
                const int lv = r->art_lv[t];
                const LearnRow* lr = LearnOf(r->pid, r->art_id[t]);
                const int idx = in.bs_top > 0 && lv + 1 == in.bs_top ? 1 : 0;
                const bool hc = lr && idx < lr->nbp && lr->bp[idx] > 0 && (in.bs_top <= 0 || lv < in.bs_top);
                n.clear().add("so2r.a.n").num(na); h->publish_text(u, n.s, an);
                n.clear().add("so2r.a.lv").num(na); h->publish_i64(u, n.s, lv);
                {   // "Lv / top" (the art's levelLimit; just the level when the table has none)
                    const int mx = ArtMaxOf(r->art_id[t]);
                    v.clear().num(lv);
                    if (mx > 0 && mx < 100) v.add(" / ").num(mx);
                    n.clear().add("so2r.a.lvt").num(na); h->publish_text(u, n.s, v.s);
                }
                n.clear().add("so2r.a.c").num(na); h->publish_i64(u, n.s, hc ? lr->bp[idx] : 0);
                n.clear().add("so2r.a.hc").num(na); h->publish_i64(u, n.s, pg == 1 && hc);
                ++na;
            };
            for (int i = 0; r && i < g_art.nl; ++i) {
                if (g_art.learn[i].pid != r->pid) continue;
                for (int t = 0; t < r->nart; ++t) if (!used[t] && r->art_id[t] == g_art.learn[i].id) put_art(t);
            }
            for (int t = 0; r && t < r->nart; ++t) if (!used[t]) put_art(t);   // any the table doesn't list
            for (int k = 0; k < 16; ++k) { n.clear().add("so2r.a.on").num(k); h->publish_i64(u, n.s, on && pg == 1 && k < na); }
            h->publish_i64(u, "so2r.a.cnt", on && pg == 1 ? na : 0);   // 0.9.133 the list's scroll: its rows
            for (int b = 0; b < 2; ++b) {   // the L / R buttons: the art set, or the game's None
                const char* en = r && r->art_eq[b] > 0 ? ArtNameOf(r->art_eq[b]) : nullptr;
                n.clear().add("so2r.a.eq").num(b); h->publish_text(u, n.s, en ? en : Word(in, FindWord("so2r.t.none")));
            }
        }
        {   // combat skills (BP): released ones (or learned / initial), Lv and the BP to improve
            int nc = 0;
            for (int t = 0; r && t < g_sk.ncmb && nc < 32; ++t) {
                const SkillTable::Row& row = g_sk.cmb[t];
                const int lv = row.id < r->ncmb ? r->cmb[row.id] : 0;
                // the ones the character has (Lv 1 or more); 0.9.129 and, like the game's Improve list, the released
                // ones not learned yet (Lv 0; releasedCombatSkillFlag, a bit per skill id)
                const bool rel = lv == 0 && row.id >= 0 && row.id < 64 && ((in.cmb_rel[row.id >> 5] >> (row.id & 31)) & 1u);
                if (lv < 0 || (lv == 0 && !rel)) continue;
                n.clear().add("so2r.c.n").num(nc); h->publish_text(u, n.s, row.name);
                n.clear().add("so2r.c.lv").num(nc); h->publish_i64(u, n.s, lv < 0 ? 0 : lv);
                const bool hc = lv >= 0 && lv < row.nsp;
                n.clear().add("so2r.c.c").num(nc); h->publish_i64(u, n.s, hc ? row.sp[lv] : 0);
                n.clear().add("so2r.c.hc").num(nc); h->publish_i64(u, n.s, pg == 2 && hc);
                ++nc;
            }
            for (int k = 0; k < 32; ++k) { n.clear().add("so2r.c.on").num(k); h->publish_i64(u, n.s, on && pg == 2 && k < nc); }
            h->publish_i64(u, "so2r.c.cnt", on && pg == 2 ? nc : 0);   // 0.9.128 the list's scroll: its rows
        }
        {   // 0.9.128 a new member or page: the lists back to the top
            h->publish_i64(u, "so2r.k.key", on ? sel * 10 + pg : -1);
        }
        // skills: a negative level means not learned when the game uses one; else level 0 = not learned
        bool neg = false;
        if (r) for (int k = 0; k < r->nskl; ++k) neg = neg || r->skl[k] < 0;
        const int guts = r && kSkillGuts < r->nskl && r->skl[kSkillGuts] > 0 ? r->skl[kSkillGuts] : 0;
        int nk = 0;
        for (int t = 0; r && t < g_sk.n && nk < 32; ++t) {
            const SkillTable::Row& row = g_sk.skl[t];
            const int lv = row.id < r->nskl ? r->skl[row.id] : -1;
            if (neg ? lv < 0 : lv <= 0) continue;
            n.clear().add("so2r.k.n").num(nk); h->publish_text(u, n.s, row.name);
            n.clear().add("so2r.k.lv").num(nk); h->publish_i64(u, n.s, lv);
            const bool hc = lv >= 0 && lv < row.nsp;
            int c = hc ? row.sp[lv] - 2 * guts : 0; if (hc && c < 1) c = 1;
            n.clear().add("so2r.k.c").num(nk); h->publish_i64(u, n.s, c);
            n.clear().add("so2r.k.hc").num(nk); h->publish_i64(u, n.s, pg == 0 && hc);
            ++nk;
        }
        for (int k = 0; k < 32; ++k) { n.clear().add("so2r.k.on").num(k); h->publish_i64(u, n.s, on && pg == 0 && k < nk); }
        h->publish_i64(u, "so2r.k.cnt", on && pg == 0 ? nk : 0);   // 0.9.128 the list's scroll: its rows
        // IC and Specialty (the game's IC list splits them), then Super Specialty
        int nic = 0, nsp = 0;
        for (int t = 0; on && t < g_sk.nss; ++t) {
            const SkillTable::Row& row = g_sk.ssk[t];
            const bool ic = in.ic_known && in.is_ic[row.id];
            bool learned = false;
            for (int j = 0; j < in.rows; ++j) learned = learned || (row.id < in.m[j].nssk && in.m[j].ssk[row.id] > 0);
            const int lv = row.id < r->nssk ? r->ssk[row.id] : 0;
            const char* pre = ic ? "so2r.k.ic" : "so2r.k.sk";
            const int idx = ic ? nic++ : nsp++;
            if (idx >= 12) continue;
            n.clear().add(pre).add("n").num(idx); h->publish_text(u, n.s, row.name);
            n.clear().add(pre).add("l").num(idx); h->publish_i64(u, n.s, lv > 0 ? lv : 0);
            n.clear().add(pre).add("b").num(idx); h->publish_i64(u, n.s, pg == 3 && learned);
            n.clear().add(pre).add("d").num(idx); h->publish_i64(u, n.s, pg == 3 && !learned);
            n.clear().add(pre).add("h").num(idx); h->publish_i64(u, n.s, pg == 3 && lv > 0);
        }
        for (int k = 0; k < 12; ++k) {
            if (k >= nic) { n.clear().add("so2r.k.icb").num(k); h->publish_i64(u, n.s, 0); n.clear().add("so2r.k.icd").num(k); h->publish_i64(u, n.s, 0); n.clear().add("so2r.k.ich").num(k); h->publish_i64(u, n.s, 0); }
            if (k >= nsp) { n.clear().add("so2r.k.skb").num(k); h->publish_i64(u, n.s, 0); n.clear().add("so2r.k.skd").num(k); h->publish_i64(u, n.s, 0); n.clear().add("so2r.k.skh").num(k); h->publish_i64(u, n.s, 0); }
        }
        h->publish_i64(u, "so2r.k.hasic", on && pg == 3 && nic > 0);
        int nss = 0;
        for (int t = 0; on && t < g_sk.nsss && nss < 12; ++t) {
            const SkillTable::Row& row = g_sk.sss[t];
            const int lv = row.id < in.nsss ? in.sss_lv[row.id] : 0;
            n.clear().add("so2r.k.ssn").num(nss); h->publish_text(u, n.s, row.name);
            n.clear().add("so2r.k.ssl").num(nss); h->publish_i64(u, n.s, lv > 0 ? lv : 0);
            n.clear().add("so2r.k.ssb").num(nss); h->publish_i64(u, n.s, pg == 3 && lv > 0);
            n.clear().add("so2r.k.ssd").num(nss); h->publish_i64(u, n.s, pg == 3 && lv <= 0);
            ++nss;
        }
        for (int k = nss; k < 12; ++k) { n.clear().add("so2r.k.ssb").num(k); h->publish_i64(u, n.s, 0); n.clear().add("so2r.k.ssd").num(k); h->publish_i64(u, n.s, 0); }
    }
    {   // the Missions page: Challenge Missions by the game's tab (cleared first, then released, then the
        // rest as the game's "????"), or the Guild Missions received and not cleared
        const int sec = __atomic_load_n(&g_msec, __ATOMIC_RELAXED);
        const int tab = sec ? 4 : __atomic_load_n(&g_mtab, __ATOMIC_RELAXED);
        const bool on = in.ok && in.flags_ok && g_ms.n > 0;
        h->publish_i64(u, "so2r.m.on", on);
        h->publish_i64(u, "so2r.m.sec0", on && sec == 0);
        h->publish_i64(u, "so2r.m.sec1", on && sec == 1);
        h->publish_i64(u, "so2r.m.tabw", on && sec == 0);
        h->publish_text(u, "so2r.m.tabname", tab < 4 ? (g_ms.tab_ok[tab] ? g_ms.tab[tab] : Word(in, FindWord("so2r.t.rank0") + tab)) : "");
        for (int t = 0; t < 5; ++t) { n.clear().add("so2r.m.tab").num(t); h->publish_i64(u, n.s, t == tab); }
        for (int t = 0; t < 4; ++t) {
            n.clear().add("so2r.m.tw").num(t);
            h->publish_text(u, n.s, g_ms.tab_ok[t] ? g_ms.tab[t] : Word(in, FindWord("so2r.t.rank0") + t));
        }
        // shown by name: released (the game sets it when its menu is opened), or -- without the visit --
        // missions with no story requirement (startScenarioProgress -1) whose previous mission is none
        // or already done
        auto done = [&](int id) {
            for (int i = 0; i < g_ms.n; ++i) if (g_ms.m[i].id == id) {
                int a, b; bool ach = false, c;
                return MissionState(in, g_ms.m[i], &a, &b, &ach, &c) ? ach : Bit(in.mis_clr, 8, id);
            }
            return false;
        };
        auto open_ = [&](const Mission& m) {
            return Bit(in.mis_rel, 8, m.id) || (m.ssp == -1 && (m.prev == 0 || Bit(in.mis_clr, 8, m.prev) || done(m.prev)));
        };
        int ids[kMaxMissions], nid = 0;
        if (on && tab < 4) {
            for (int pass = 0; pass < 3; ++pass)
                for (int i = 0; i < g_ms.n; ++i) {
                    const Mission& m = g_ms.m[i];
                    const int c = m.cat >= 3 && m.cat <= 5 ? m.cat - 2 : 0;   // the game's MissionToCategory
                    if (c != tab) continue;
                    // 0.9.58 like the game's list: a mission whose reward was taken (clearedMissionFlag, the
                    // game's state 5) is gone from it; achieved and not yet taken stays, as Complete
                    if (Bit(in.mis_clr, 8, m.id)) continue;
                    int cn = 0, tg = 0; bool ach = false, cnt = false;
                    const bool known = MissionState(in, m, &cn, &tg, &ach, &cnt);
                    const bool clr = known ? ach : Bit(in.mis_clr, 8, m.id), rel = open_(m);
                    if ((pass == 0 && clr) || (pass == 1 && !clr && rel) || (pass == 2 && !clr && !rel)) ids[nid++] = i;
                }
        }
        int nq = 0, qids[kMaxQuests];
        if (on && tab == 4)
            for (int q = 1; q < kMaxQuests; ++q) if (Bit(in.q_rcv, 4, q) && !Bit(in.q_clr, 4, q)) qids[nq++] = q;
        const int total = tab < 4 ? nid : nq;
        const int pgm = 0;
        h->publish_i64(u, "so2r.m.n", total < kMisRows ? total : kMisRows);   // the scroll's row count
        h->publish_i64(u, "so2r.m.key", sec * 10 + tab);                    // a new list: back to the top
        h->publish_i64(u, "so2r.m.none", on && total == 0);
        for (int k = 0; k < kMisRows; ++k) {
            const int j = pgm * kMisRows + k;
            const bool rowon = on && j < total;
            Str key;
            key.clear().add("so2r.m.on").num(k); h->publish_i64(u, key.s, rowon);
            g_mrow_id[k] = 0;
            if (!rowon) { key.clear().add("so2r.m.ok").num(k); h->publish_i64(u, key.s, 0); key.clear().add("so2r.m.ng").num(k); h->publish_i64(u, key.s, 0);
                          key.clear().add("so2r.m.sel").num(k); h->publish_i64(u, key.s, 0); continue; }
            if (tab == 4) {
                const char* qn = QuestNameOf(Guest{h}, g_ms.qdict, qids[j]);
                key.clear().add("so2r.m.n").num(k); h->publish_text(u, key.s, qn ? qn : Word(in, FindWord("so2r.t.q")));
                key.clear().add("so2r.m.rw").num(k); h->publish_text(u, key.s, "");
                key.clear().add("so2r.m.c").num(k); h->publish_text(u, key.s, "");
                key.clear().add("so2r.m.ok").num(k); h->publish_i64(u, key.s, 0);
                key.clear().add("so2r.m.ng").num(k); h->publish_i64(u, key.s, 1);
                continue;
            }
            const Mission& m = g_ms.m[ids[j]];
            g_mrow_id[k] = (open_(m) || Bit(in.mis_clr, 8, m.id)) && m.name[0] ? m.id : 0;   // 0.9.64 tap: open it in the game
            key.clear().add("so2r.m.sel").num(k); h->publish_i64(u, key.s, g_mm.state != 0 && g_mm.id == m.id);
            int cn = 0, tg = 0; bool ach = false, cnt = false;
            const bool known = MissionState(in, m, &cn, &tg, &ach, &cnt);
            const bool clr = known ? ach : Bit(in.mis_clr, 8, m.id), rel = open_(m);
            key.clear().add("so2r.m.c").num(k); h->publish_text(u, key.s, "");
            if (known && cnt && (rel || clr) && !clr) {   // the counter, like the game's (count / target)
                Str cs; cs.num(cn > tg ? tg : cn).add(" / ").num(tg);
                h->publish_text(u, key.s, cs.s);
            }
            key.clear().add("so2r.m.n").num(k);
            h->publish_text(u, key.s, (rel || clr) && m.name[0] ? m.name : Word(in, FindWord("so2r.t.q")));
            Str rw;                    // the rewards in the game's words: FOL / SP / BP / EXP n, items by name
            for (int r2 = 0; (rel || clr) && r2 < m.nrw; ++r2) {
                if (rw.s[0]) rw.add("  ");
                const int ty = m.rwtype[r2];
                if (ty == 1) {
                    const ItemName* it = in.pm ? ItemNameOf(Guest{h}, in.pm, m.rwval[r2]) : nullptr;
                    if (it && it->name[0]) { rw.add(it->name); if (m.rwcount[r2] > 1) rw.add(" x").num(m.rwcount[r2]); }
                } else if (ty == 2) rw.add(Word(in, FindWord("so2r.t.fol"))).ch(' ').num(m.rwval[r2]);
                else if (ty == 3) rw.add(Word(in, FindWord("so2r.t.sp"))).ch(' ').num(m.rwval[r2]);
                else if (ty == 4) rw.add(Word(in, FindWord("so2r.t.bp"))).ch(' ').num(m.rwval[r2]);
                else if (ty == 7) rw.add(Word(in, FindWord("so2r.t.exp2"))).ch(' ').num(m.rwval[r2]);
            }
            key.clear().add("so2r.m.rw").num(k); h->publish_text(u, key.s, rw.s);
            key.clear().add("so2r.m.ok").num(k); h->publish_i64(u, key.s, clr);
            key.clear().add("so2r.m.ng").num(k); h->publish_i64(u, key.s, !clr);
        }
    }
#if SO2R_DIAG
    Str l;
    int k = 0;
    auto line = [&](const Str& t) { n.clear().add("so2r.d.l").num(k++); h->publish_text(u, n.s, t.s); };
    l.clear().add(kVersion).add(" diag  build ").add(kBuild).add("  reads ").num(in.reads).add("  rejects ").num(in.rejects)
        .add("  scene ").num(in.scene).add(" ").add(in.scene_cls).add("  ").add(in.why ? in.why : "");
    line(l);
    l.clear().add("slot main+417DAE0 -> class ").hex(in.klass).add(" ").add(in.klass_name)
        .add(in.klass_ref == in.klass ? "  ref SAME" : "  ref DIFFERENT").add("  delta ").num(in.delta);
    line(l);
    l.clear().add("ParameterManager ").hex(in.inst).add(" ").add(in.inst_name).add("  UserParameter ").hex(in.user);
    line(l);
    l.clear().add("heroID ").num(in.hero).add("  Fol ").num(in.fol).add("  override ").num(in.over)
        .add("  leaderID ").num(in.leader).add("  characters ").num(in.chars_len);
    line(l);
    auto list = [&](const char* label, const int32_t* vv, int len) {
        l.clear().add(label).add("[").num(len).add("]");
        for (int i = 0; i < len && i < kMaxIds; ++i) l.add(" ").num(vv[i]);
        line(l);
    };
    list("playerIDs", in.ids, in.ids_len);
    list("partyMemberStates", in.states, in.states_len);
    l.clear().add("battle members: ").add(in.battle_how == 3 ? "the first four party slots (r40)" : "?")
        .add(":");
    for (int i = 0; i < in.rows && i < kRows; ++i) if (in.m[i].on && in.m[i].battle) l.add(" ").num(in.m[i].pid);
    line(l);
    {   // 0.9.38 the destination "!": identity, minimap state, source, decision (and its raw numbers further down)
        const MapState& mm = in.map;
        l.clear().add("dest 0.9.38: obj r").num(g_dest.id.row).add(" p").num(g_dest.id.prog).add(" s").hex(g_dest.id.save & 0xFFFFFF)
            .ch(' ').add(g_dest.area).add(" | n").num(mm.ndest).add(" ip ").hex(g_dest_raw.ip & 0xFFFFFF)
            .add(" state ").add(StateName(mm.mstate)).ch('(').num(mm.mstate_run > 999 ? 999 : mm.mstate_run).add(") mini ").add(MiniName(mm.mini))
            .add(" | src ").add(DestSrcName(g_dest.rsrc)).add(g_dest_raw.hidden ? " (see-through)" : "")
            .add(" | ").add(DestWhyName(g_dest.why));
        // 0.9.42 the objects the game marks: exits (area) or a location (world), found / of the list, drawn, the first
        l.add(" | marked ").add(g_marked.world ? "world " : "exits ").num(g_marked.n).ch('/').num(g_marked.total)
            .add(" drawn ").num(mm.marked);
        if (g_marked.n) {
            l.ch(' ');
            for (int c = 0; c < 3; ++c) { const int32_t v = RoundF(g_marked.w[0][c]); l.num(v); if (c < 2) l.ch(','); }
        }
        line(l);
    }
    l.clear().add("objective (0.9.24): progress ").num(in.obj_prog).add(" row ").num(in.obj_row).add(" '").add(in.obj).ch('\'');
    line(l);
    l.clear().add("windows (0.9.19): path ").num(in.win_step).add(" open ").num(in.win_n).add(" top");
    for (int i = 0; i < 4 && i < in.win_n; ++i) l.ch(' ').num(in.win_type[i]);
    l.add("  quick: phase ").num(g_qm.phase).add(" want ").num(g_qm.want).add(" last ").num(g_qm.last)
        .add(" fire ").num(g_qm.fire).add(" B ").num(g_qm.presses);
    line(l);
    list("partyJoiningStates", in.joining, in.joining_len);
    {   // missions (0.8)
        int nclr = 0, nrel = 0, named = 0;
        for (int i = 0; i < g_ms.n; ++i) { nclr += Bit(in.mis_clr, 8, g_ms.m[i].id); nrel += Bit(in.mis_rel, 8, g_ms.m[i].id); named += g_ms.m[i].name[0] != 0; }
        int nk = 0; for (int i = 0; i < g_ms.n; ++i) { int a2, b2; bool c2, d2; nk += MissionState(in, g_ms.m[i], &a2, &b2, &c2, &d2); }
        l.clear().add("progress: decode ").num(g_ach_state).add(" cases known ").num(g_ach_known).add("/71  missions counted ").num(nk)
            .add("  vars ").num(in.nvar).add(" spec exec ").num(in.nspec_exec).add(" super exec ").num(in.nsuper_exec);
        line(l);
        l.clear().add("missions: ").num(g_ms.n).add(" named ").num(named).add(" cleared ").num(nclr).add(" released ").num(nrel)
            .add(" token ").add(g_ms.tok_ok ? g_ms.token : "-").add(" tabs");
        for (int t = 0; t < 4; ++t) l.ch(' ').add(g_ms.tab_ok[t] ? g_ms.tab[t] : "-");
        l.add("  r35 ").num(in.c_music).ch('/').num(in.c_item).ch('/').num(in.c_enemy).ch('/').num(in.c_quest).ch('/').num(in.c_form).ch('/').num(in.c_fish)
            .add(" r54 ").num(in.c_pa).ch('/').num(in.c_box).ch('/').num(in.c_loc).ch('/').num(in.c_chal);
        l.add("  quests rcv ").hex(in.q_rcv[0]).ch('/').hex(in.q_rcv[1]).add(" clr ").hex(in.q_clr[0]).add("  combat rel ").hex(in.cmb_rel[0]).add(" table ").num(g_sk.ncmb);
        line(l);
        // 0.9.59 the Missions page's tab, mission by mission (not taken first): id/type, then K the module
        // follows its count, A achieved, C reward taken (clearedMissionFlag), R released
        const int tab = __atomic_load_n(&g_mtab, __ATOMIC_RELAXED);
        l.clear().add("mis tab ").num(tab).ch(':');
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < g_ms.n; ++i) {
                const Mission& m = g_ms.m[i];
                const int c = m.cat >= 3 && m.cat <= 5 ? m.cat - 2 : 0;
                const bool clr = Bit(in.mis_clr, 8, m.id);
                if (c != tab || clr != (pass == 1)) continue;
                int a2 = 0, b2 = 0; bool ach = false, cn = false;
                const bool k = MissionState(in, m, &a2, &b2, &ach, &cn);
                l.ch(' ').num(m.id).ch('/').num(m.type).ch(' ');
                if (k) l.ch('K');
                if (k && ach) l.ch('A');
                if (clr) l.ch('C');
                if (Bit(in.mis_rel, 8, m.id)) l.ch('R');
            }
        line(l);
    }
    {   // the skills read (0.7)
        int sel = __atomic_load_n(&g_sel, __ATOMIC_RELAXED);
        if (sel < 0 || sel >= in.rows) sel = 0;
        if (sel < in.rows) {
            const Member& r = in.m[sel];
            (void)r;   // 0.9.38: the elements line (passed in 0.7) gave its place to the destination's numbers
            auto f1 = [&](float v) -> Str& { int32_t t = RoundF(v * 10); if (t < 0) { l.ch('-'); t = -t; } l.num(t / 10).ch('.').num(t % 10); return l; };
            l.clear().add("dest raw: lp "); f1(g_dest_raw.lp[0]).ch(','); f1(g_dest_raw.lp[1]).add(" pl ");
            f1(g_dest_raw.pli[0]).ch(','); f1(g_dest_raw.pli[1]).add(g_dest_raw.have_pli ? "" : "(none)").add(" k ");
            f1(g_dest_raw.coeff).add(" cache "); f1(g_dest_raw.cache[0]).ch(','); f1(g_dest_raw.cache[1]).add(" -> ");
            f1(g_dest_raw.x).ch(','); f1(g_dest_raw.y).add(" | shown ");
            if (g_dest.ok) { f1(g_dest.x).ch(','); f1(g_dest.y).add(" (").add(DestSrcName(g_dest.src)).ch(')'); } else l.add("none");
            l.add(" | cand ");
            if (g_dest.cand) { f1(g_dest.cx).ch(','); f1(g_dest.cy).add(" from ");
                               f1(g_dest.apx).ch(','); f1(g_dest.apy); } else l.ch('-');
            l.add(" | player "); f1(in.map.x).ch(','); f1(in.map.y);
            // 0.9.40 the game's map destination table: matches, the row used, its world position
            l.add(" | table n").num(g_tdest.nmatch).add(" row ").num(g_tdest.row);
            if (g_tdest.ok) { l.ch(' '); f1(g_tdest.pos[0]).ch(','); f1(g_tdest.pos[1]).ch(','); f1(g_tdest.pos[2]); }
            line(l);
        }
        l.clear().add("arts: names ").num(g_art.nn).add(" learn rows ").num(g_art.nl).add(" top ").num(in.bs_top);
        if (sel < in.rows) { const Member& r = in.m[sel]; for (int k = 0; k < r.nart; ++k) l.ch(' ').num(r.art_id[k]).ch(':').num(r.art_lv[k]); }
        line(l);
        l.clear().add("skills: table ").num(g_sk.n).ch('/').num(g_sk.nss).ch('/').num(g_sk.nsss).add(" tries ").num(g_sk.tries)
            .add("  ic ").num(in.ic_known).ch(':').num(in.ic_n).ch(' ').add(in.ic_cls).ch('/').add(in.ic_obj).add("  super");
        for (int k = 0; k < in.nsss && k < kMaxSuper; ++k) l.ch(' ').num(in.sss_lv[k]);
        if (sel < in.rows) {
            const Member& r = in.m[sel];
            l.add("  sp ").num(r.sp).add(" skl");
            for (int k = 0; k < r.nskl; ++k) l.ch(' ').num(r.skl[k]);
            l.add(" ssk");
            for (int k = 0; k < r.nssk; ++k) l.ch(' ').num(r.ssk[k]);
        }
        line(l);
    }
    l.clear().add("names: playerParameter ").hex(in.playerparam).add(" (").num(in.pp_count).add(")  characterMessages ")
        .hex(in.charmsg).add(" (").num(in.msg_count).add(")");
    line(l);
    for (int half = 0; half < 2; ++half) {
        l.clear().add(half ? "names 8-13:" : "names 1-7:");
        for (int pid = half ? 8 : 1; pid < (half ? kMaxPlayer : 8); ++pid)
            if (in.names.id[pid][0]) l.add(" ").num(pid).add("=").add(in.names.name[pid][0] ? in.names.name[pid] : "?");
        line(l);
    }
    l.clear().add("files: index ").add(g_ix.loaded ? "loaded" : "missing").add(" err ").num(g_ix.err)
        .add("  sprites ").num(g_ix.ns).add("  textures ").num(g_ix.nt).add("  bundles ").num(g_ix.nb).add("  open");
    for (auto& o : g_ob) if (o.ok || o.err) { l.add(" ").num(o.id).add(o.ok ? ":ok" : ":err").num(o.ok ? o.n : o.err); }
    line(l);
    // 0.9.44 a bundle the device has at another size (err 10): its path, the device's size (0: the host could not
    // open it) and the index's
    // (only when there is one: the page has no room otherwise, the last party row gives way)
    bool size_err = false;
    for (auto& o : g_ob) size_err |= o.err == 10 || (o.moved && !g_reloc_found);   // 0.9.59 a found move: no line
    if (size_err) {
        l.clear().add("bundle sizes (0.9.44):");
        for (auto& o : g_ob) if (o.err == 10 || o.moved) { l.ch(' ').add(o.path).add(" device ").num(int64_t(o.dev_size)).add(" index ").num(int64_t(o.idx_size)); }
        l.add("  0.9.46 search: found ").num(g_reloc_found).ch('/').num(g_reloc_tried);
        line(l);
    }
    {
        const MapState& m = in.map;
        auto f1 = [&](float v) -> Str& { int32_t t = RoundF(v * 10); if (t < 0) { l.ch('-'); t = -t; } l.num(t / 10).ch('.').num(t % 10); return l; };
        l.clear().add("map ").add(m.ok ? "ok " : "- ").add(m.why ? m.why : "").add("  kind ").num(m.kind).add("  fieldmap ").num(m.fieldmap)
            .add(" ").add(m.fieldmap > 0 && m.fieldmap < kFieldmapCount ? kFieldmapName[m.fieldmap] : "?")
            .add("  worldmap ").num(m.worldmap).add("  minimap ").num(m.minimap_id).add("  ").add(m.area);
        line(l);
        l.clear().add("tint ");
        for (int c = 0; c < 4; ++c) { f1(g_area_tint[c] * 100).ch(' '); }
        l.add(g_area_tint_ok ? "ok" : "-").add("  land ");
        for (int k = 0; k < 2; ++k) { f1(g_land[k].x0).ch(','); f1(g_land[k].y0).ch('-'); f1(g_land[k].x1).ch(','); f1(g_land[k].y1).ch(' '); }
        line(l);
        l.clear().add(m.live ? "live " : "entry ").add("pos "); f1(m.px).add(" "); f1(m.py).add(" "); f1(m.pz).add("  dir "); f1(m.heading).add("  pic ");
        f1(m.w).add("x"); f1(m.h).add("  at "); f1(m.x).add(","); f1(m.y).add("  turn "); f1(m.rot);
        line(l);
        l.clear().add("ufc ").hex(m.ufc).add(" mgr ").hex(m.mgr).add(" pres ").hex(m.pres).add(" task ").hex(m.task)
            .add("  off "); f1(m.offx).add(","); f1(m.offy).add("  coeff "); f1(m.coeff).add(" scale "); f1(m.scale)
            .add(" euler.z "); f1(m.euler).add(" extra "); f1(m.extra).add(" wp ").num(m.wp)
            .add(" tex "); f1(m.wtw).add("x"); f1(m.wth).add(" w "); f1(m.wcw * 1000);
        line(l);
        l.clear().add("bg: cur "); f1(m.bgcur[0]).ch('x'); f1(m.bgcur[1]).add(" base "); f1(m.bgbase[0]).ch('x'); f1(m.bgbase[1])
            .add(" scale "); f1(m.bgsc[0] * 100).ch(','); f1(m.bgsc[1] * 100).ch(','); f1(m.bgsc[2] * 100).add("%  src ").num(m.bgsrc)
            .add("  fac "); f1(m.bgfac * 1000).add("/1000  k "); f1(m.bgk);
        line(l);
        l.clear().add("icons: used ").num(m.icons_seen).add(" drawn ").num(m.nmarks).add(" kept ").num(g_nmemo).ch('/').num(g_nconf).add(" gone ").num(m.memo_dropped).add(" enemy ").hex(m.enemy_argb).add(" place ").num(m.pmode).add(" movers ").num(g_nmovers).ch('/').num(g_movers_used).add(" unused+obj ").num(m.unused_follow).add(" hid ").num(m.hidden_drawn).add(" wcache ").num(m.wcached).add(" ab ").num(g_after_battle).add(" kept ").num(g_wicons.n)
            .add(" win "); f1(m.win[0]).ch('x'); f1(m.win[1]).add(" r "); f1(m.mini_r * 100).add(" wscale "); f1(m.wscale).add(" pl "); f1(m.wplayer[0]).ch(','); f1(m.wplayer[1]).add(" types");
        for (int i = 0; i < m.nmarks && i < 14; ++i) l.ch(' ').num(m.marks[i].type);
        line(l);
        for (int j = 0; j < m.nraw; ++j) {     // one icon a line: list, type, follow, world pos, follow pos, native local pos
            const auto& r = m.raw[j];
            l.clear().add("ic L").num(r.list).add(" t").num(r.type).ch('/').num(r.sub).add(" f").num(r.follow).add(" cp ");
            f1(r.c[0]).ch(','); f1(r.c[1]).add(" lp ");
            f1(r.lp[0]).ch(','); f1(r.lp[1]).add(" par ");
            f1(r.e[0]).ch(','); f1(r.e[1]).add(" obj ");
            f1(r.w[0]).ch(','); f1(r.w[1]).ch(','); f1(r.w[2]);
            line(l);
        }
        {
            int sel = g_sel; if (sel < 0 || sel >= in.rows) sel = 0;
            const Member& r = in.m[sel];
            l.clear().add("status ").num(sel).add(" fr ");
            for (int j = 1; j < 3; ++j) l.num(in.friend_[r.pid * 14 + j]).ch('/');
            l.num(in.max_friend).add(" lb ").num(r.level_base).add(" max ").num(in.max_level).add(" tl ").hex(static_cast<uint32_t>(r.talent))
                .add(" bd ").hex(r.bd).add(" st");
            for (int k = 0; k < 9; ++k) l.ch(' ').num(r.stat[k]);
            l.add("  eq");
            for (int k = 0; k < kEquipSlots; ++k) {
                l.ch(' ').num(r.equip[k]);
                if (r.equip[k] > 0) l.ch('>').num(r.equip_item[k]);
                for (int j = 0; j < g_nitems; ++j)
                    if (g_items[j].id == r.equip_item[k] && r.equip_item[k] > 0) { l.ch('=').num(g_items[j].src).ch(':').add(g_items[j].name[0] ? g_items[j].name : (g_items[j].key[0] ? g_items[j].key : "?")); break; }
            }
            line(l);
        }
        l.clear().add("place: key ").add(in.map.place_id[0] ? in.map.place_id : "-").add(" guide ")
            .add(in.map.place_id2[0] ? in.map.place_id2 : "-").add(" floor ").add(in.map.floor_id[0] ? in.map.floor_id : "-")
            .ch('=').add(in.map.floor[0] ? in.map.floor : "-").add(" src ").num(in.map.place_src)
            .add(" name ").add(in.map.place[0] ? in.map.place : "-");
        line(l);
        l.clear().add("words ").num(in.words.nfound).ch('/').num(kSysWordCount).add(":");
        for (int i = 0; i < kSysWordCount; ++i) l.ch(' ').add(in.words.found[i] ? in.words.text[i] : "-");
        line(l);
        l.clear().add("world minimap: base "); f1(m.mbase).add(" scale "); f1(m.mscale).add(" window half ");
        f1(m.mini_half).add(m.mini_half > 1 ? " (game)" : " (fallback 180)");
        line(l);
        // 0.9.38 each world list's geometry: verified?, k, C, its player icon following?, the last decision;
        // its icons used / see-through / not placed; the pinned "!" left out, the kept set
        l.clear().add("geo");
        for (int w = 1; w <= 2; ++w) {
            const ListGeo* G = GeoFind(m.area, w);
            l.add(" L").num(w).ch(' ');
            if (!G) { l.add("- "); continue; }
            l.add(G->ok ? "ok k " : "no k "); f1(G->k).add(" C "); f1(G->cx).ch(','); f1(G->cy)
                .add(" pl ").add(G->fresh > 0 ? "live" : G->fresh < 0 ? "STALE" : "?").add(" '").add(GeoWhyName(G->why)).add("' ")
                .num(m.list_used[w]).ch('/').num(m.list_hidden[w]).ch('/').num(m.unverified[w]).ch(' ');
        }
        l.add("| state ").add(StateName(m.mstate)).add(" | rim ").num(m.clamped).add(" kept ").num(g_wicons.n).add(" cache ").num(m.wcached);
        line(l);
    }
    l.clear().add("font: ").add(g_font.ok ? "ok" : "not yet").add(" err ").num(g_font.err).add("  glyphs ").num(g_font.found)
        .add("  line ").num(g_font.line).add(g_font.uniform ? " cells" : " boxes").add("   pictures drawn ").num(__atomic_load_n(&g_pics_drawn, __ATOMIC_RELAXED))
        .add(" failed ").num(__atomic_load_n(&g_pics_failed, __ATOMIC_RELAXED)).add(" last err ").num(g_pic_err);
    line(l);
    l.clear().add(g_hh_diag[0] ? g_hh_diag : "hide hud: not run yet");   // 0.9.142 (the icons line's place; icons done)
    line(l);
    CampDiagLine(in, h, l);   // 0.9.62
    line(l);
    if (in.scene == 6) {
        BattleDiagLine(in, h, l);   // 0.9.72
        line(l);
    } else
    {   // 0.9.136 (outside a battle, in the battle line's place) the Equipment screen the companion opened: what it remembers, and how the windows read now
        l.clear().add("equip: live ").num(g_eq_live).add(" view ").num(g_eq_view).add(" mem ").num(g_eq_mem).add(" slot ")
            .num(g_eq_slot).add(" sel ").num(__atomic_load_n(&g_sel, __ATOMIC_RELAXED)).add(" | left ").num(g_eq_sig[0]).ch('/')
            .num(g_eq_sig[1]).ch('/').num(g_eq_sig[2]).ch('/').num(g_eq_sig[3]).add(" now ").num(in.win_n).ch('/')
            .num(in.win_n > 0 ? in.win_type[0] : -1).ch('/').num(g_eq_now[0]).ch('/').num(g_eq_now[1])
            .add(" | macro ").num(g_mm.state).add(" result ").num(g_mm.result).add(" why ").num(g_mm.why)
            .add(" follows ").num(g_eq_follows);
        line(l);
    }

    for (int i = 0; i < 4 && k < 38; ++i) {   // the first rows while the page has room (0.9.59: they give way)
        const Member& r = in.m[i];
        l.clear();
        if (i < in.rows && r.on)
            l.add("row ").num(i).add(": pid ").num(r.pid).add(" cid ").num(r.cid).add(" Lv ").num(r.level).add(" exp ").num(r.exp)
                .add(" HP ").num(r.hp).add("/").num(r.hpmax).add(" MP ").num(r.mp).add("/").num(r.mpmax)
                .add(r.leader ? " LEADER " : " ").add(in.names.id[r.pid]).add(" = ").add(r.name);
        else
            l.add("row ").num(i).add(": -");
        line(l);
    }
    h->publish_i64(u, "so2r.d.n", k);
#endif
}

// ---------------------------------------------------------------- ABI callbacks
EdenDsmodBool SupportsBuild(const char* build_id) {
    return build_id && starts(build_id, kBuild) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    if (!host || host->abi_version != EDEN_DSMOD_MODULE_ABI_VERSION || host->abi_hash != EDEN_DSMOD_MODULE_ABI_HASH ||
        host->title_id != kTitle || !host->read_memory || !host->publish_i64 || !host->publish_text) {
        return nullptr;
    }
    memset(&g_inst, 0, sizeof g_inst);
    g_inst.main_base = host->main_base;
    g_inst.why = "waiting for the game";
    g_inst.map.why = "waiting for the game";
    if (host->log) host->log(host->userdata, EDEN_DSMOD_LOG_INFO, SO2R_DIAG ? "SO2R module 1.0.0 (diag) ready" : "SO2R module 1.0.0 ready");
    return &g_inst;
}

void HideHudRelease();   // 1.0.0 (defined with HideHUD below)
void Destroy(void*) { HideHudRelease(); }

// ---- 0.9.19 the quick menu: which game windows are open, and the presses that close one and open another
// GameUIManager (r41, as EventManager.OpenCampWindow reaches it): the pointer at main+0x4946C20 -> the
// method's class (+0x20) -> its generic context (+0xC0) -> SingletonMonoBehaviour<GameUIManager> -> statics
// (+0xB8) -> the instance (+0)
constexpr uint64_t kGameUiRef = 0x4946C20, kGuiWindows = 0x18, kWcStack = 0x18, kStackArr = 0x10, kStackSize = 0x18;
void ReadWindows(Instance& in, const EdenDsmodHostApi* h) {
    Guest g{h};
    in.win_n = -1; in.win_step = 0; in.win_ctl = 0;
    const uint64_t p = g.ptr(in.main_base + kGameUiRef);
    const uint64_t a = Guest::objp(p) ? g.ptr(p) : 0;
    const uint64_t k = Guest::objp(a) ? g.ptr(a + 0x20) : 0;
    const uint64_t r = Guest::objp(k) ? g.ptr(k + 0xC0) : 0;
    const uint64_t k2 = Guest::objp(r) ? g.ptr(r) : 0;
    const uint64_t st = Guest::objp(k2) ? g.ptr(k2 + 0xB8) : 0;
    const uint64_t gui = Guest::objp(st) ? g.ptr(st) : 0;
    if (!g.is(gui, "GameUIManager")) { in.win_step = Guest::objp(st) ? 5 : Guest::objp(k2) ? 4 : Guest::objp(k) ? 2 : 1; return; }
    in.win_step = 6;
    const uint64_t wc = g.ptr(gui + kGuiWindows);
    if (!g.is(wc, "WindowController")) return;
    in.win_ctl = wc;
    in.win_step = 7;
    const uint64_t stk = g.ptr(wc + kWcStack);
    if (!Guest::objp(stk)) return;
    bool ok = false;
    const int32_t n = g.i32(stk + kStackSize, &ok);
    if (!ok || n < 0 || n > 32) return;
    in.win_step = 8;
    in.win_n = n;
    const uint64_t arr = g.ptr(stk + kStackArr);
    for (int i = 0; i < 4; ++i) {
        in.win_type[i] = -1;
        if (i >= n || !Guest::objp(arr)) continue;
        const uint64_t e = g.ptr(arr + kArrItems + 8ull * static_cast<uint64_t>(n - 1 - i));
        if (Guest::objp(e)) in.win_type[i] = g.i32(e + 0x10);
    }
}

// The quick menu (the Map page's sidebar): a tap names a game button (0 up, 1 left, 2 down, 3 right, 4 X).
// Nothing open: that button. Its own menu open: B until the windows are closed. Another open: B until
// closed, then the button. A press is made by the manifest's enforce rules (every 200 ms) while
// so2r.q.f<n> is 1; each press also toggles the runtime flag so2r.qp, which the module reads back
// (@flag:so2r.qp) so every press is exactly one.
// ---- 0.9.60 make a member the leader through the game's own Party Formation (r51 / r52): the companion only
// presses the game's buttons and checks each step against what the game shows; it writes nothing.
// UICampWindow (WindowType 3) +0xC0 UICampMenuSelector: currentState +0xE8 (0 MainMenu, 6 SelectCharacter),
// isDisableInput +0x89, selectCharacterSelector +0xD8 = UICampMenuSelectCharacterSelector: its OnInput takes
// nothing while characterStatusPresenter (+0x90) +0x170 or disableInput +0xF0 is set; the cursor's member =
// playerIDDictionary (+0xD8, presenter -> PlayerID) [currentSelectedPresenter +0xF8]; X = ChangeLeader (only
// a battle member, nothing mid-swap: currentDecisionPresenter +0x100 null). Camp Menu: X; Party Formation: Y.
constexpr uint64_t kCwMenuSel = 0xC0, kCmsState = 0xE8, kCmsDisable = 0x89, kCmsChar = 0xD8;
constexpr uint64_t kCcsStatus = 0x90, kCcsStatusBusy = 0x170, kCcsDisable = 0xF0, kCcsDict = 0xD8, kCcsCur = 0xF8,
                   kCcsDecision = 0x100;
constexpr int kBtnRight = 3, kBtnX = 4, kBtnY = 5, kBtnB = 6;   // 7 A, 8 L, 9 R (0.9.64)
struct CampView { bool camp; int state; bool menu_ready; uint64_t chsel; bool ch_ready; int cur_pid; };
// a window by its WindowType: WindowController.windowDictionary (+0x10; <WindowType, WindowComponent> entries
// 0x18 apart: the key at +8, the window at +0x10)
uint64_t WindowOf(const Instance& in, const Guest& g, int type) {
    if (!Guest::objp(in.win_ctl)) return 0;
    const uint64_t dict = g.ptr(in.win_ctl + 0x10);
    if (!Guest::objp(dict)) return 0;
    bool ok = false;
    const uint64_t entries = g.ptr(dict + kDictEntries);
    const int32_t count = g.i32(dict + kDictCount, &ok);
    if (!ok || count <= 0 || count > 64 || !Guest::objp(entries)) return 0;
    for (int32_t e = 0; e < count; ++e) {
        const uint64_t ea = entries + kArrItems + static_cast<uint64_t>(e) * kEntrySize;
        if (g.i32(ea) >= 0 && g.i32(ea + kEntryKey) == type) return g.ptr(ea + kEntryValue);
    }
    return 0;
}
CampView ReadCamp(const Instance& in, const Guest& g) {
    CampView v{}; v.state = -1; v.cur_pid = -1;
    if (in.win_n <= 0 || in.win_type[0] != 3) return v;          // the Camp Menu on top
    const uint64_t cw = WindowOf(in, g, 3);
    if (!g.is(cw, "UICampWindow")) return v;
    const uint64_t ms = g.ptr(cw + kCwMenuSel);
    if (!g.is(ms, "UICampMenuSelector")) return v;
    v.camp = true;
    bool ok = false;
    v.state = g.i32(ms + kCmsState, &ok);
    if (!ok) v.state = -1;
    v.menu_ready = g.u8(ms + kCmsDisable) == 0;
    const uint64_t cs = g.ptr(ms + kCmsChar);
    if (!g.is(cs, "UICampMenuSelectCharacterSelector")) return v;
    v.chsel = cs;
    const uint64_t sp = g.ptr(cs + kCcsStatus);
    v.ch_ready = g.u8(cs + kCcsDisable) == 0 && (!Guest::objp(sp) || g.u8(sp + kCcsStatusBusy) == 0) &&
                 !Guest::objp(g.ptr(cs + kCcsDecision));
    const uint64_t cur = g.ptr(cs + kCcsCur), dict = g.ptr(cs + kCcsDict);
    if (!Guest::objp(cur) || !Guest::objp(dict)) return v;
    const uint64_t entries = g.ptr(dict + kDictEntries);
    const int32_t count = g.i32(dict + kDictCount, &ok);
    if (!ok || count <= 0 || count > 64 || !Guest::objp(entries)) return v;
    for (int32_t e = 0; e < count; ++e) {
        const uint64_t ea = entries + kArrItems + static_cast<uint64_t>(e) * kEntrySize;
        if (g.i32(ea) < 0) continue;
        if (g.ptr(ea + kEntryKey) == cur) { v.cur_pid = g.i32(ea + kEntryValue); break; }
    }
    return v;
}
// 0.9.62 the Camp Menu as the leader steps read it (open the game's Party Formation to see it)
[[maybe_unused]] void CampDiagLine(const Instance& in, const EdenDsmodHostApi* h, Str& l) {
        Guest g{h};
        const CampView v = ReadCamp(in, g);
        l.clear().add("camp: ").num(v.camp).add(" state ").num(v.state).add(" menu in ").num(v.menu_ready)
            .add(" | chars ").hex(v.chsel);
        if (v.chsel) {
            const uint64_t sp = g.ptr(v.chsel + kCcsStatus);
            bool ok = false; const uint64_t dict = g.ptr(v.chsel + kCcsDict);
            const int32_t dn = Guest::objp(dict) ? g.i32(dict + kDictCount, &ok) : -1;
            l.add(" off ").num(g.u8(v.chsel + kCcsDisable)).ch('/').num(Guest::objp(sp) ? g.u8(sp + kCcsStatusBusy) : -1)
             .ch('/').hex(g.ptr(v.chsel + kCcsDecision)).add(" cur ").hex(g.ptr(v.chsel + kCcsCur)).add(" dict ").num(dn)
             .add(" pid ").num(v.cur_pid).add(" in ").num(v.ch_ready);
        }
        {
            const int wb = FindWord("so2r.t.battle");
            l.add(" | words");
            for (int i = 0; i < 4; ++i) l.ch(' ').add(in.words.found[wb + i] ? in.words.key[wb + i] : "-");
        }
        l.add("  leader steps ").num(g_lm.state).ch('/').num(g_lm.result).ch('/').num(g_lm.why).ch('/').num(g_lm.presses)
         .add("  mission steps ").num(g_mm.state).ch('/').num(g_mm.result).ch('/').num(g_mm.why).ch('/').num(g_mm.id);
}
// one step; true while the macro owns the presses
bool LeadStep(Instance& in, const EdenDsmodHostApi* h, QuickMenu& q) {
    LeadMacro& m = g_lm;
    const int req = __atomic_exchange_n(&g_lead_req, 0, __ATOMIC_ACQ_REL);
    if (req > 0 && m.state == 0) {
        m = LeadMacro{}; m.target = req; m.state = 1; m.at = in.samples;
        q.phase = 0; q.want = -1;                       // a quick menu press waiting: dropped
    }
    if (m.state == 0) return false;
    if (q.fire >= 0) return true;                       // a press on its way
    Guest g{h};
    const CampView v = ReadCamp(in, g);
    const uint64_t since = in.samples - m.at;
    auto press = [&](int b) { q.fire = b; q.ack = h->get_i64 ? h->get_i64(h->userdata, "@flag:so2r.qp", -1) : -1; q.at = in.samples; m.at = in.samples; ++m.presses; };
    auto stop = [&](int why) { m.result = -1; m.why = why; m.state = 7; m.at = in.samples; m.presses = 0; };
    if (m.state == 1 && in.scene != 4 && in.scene != 5) { stop(10); return true; }      // not in the field
    if (m.state != 7 && in.scene != 4 && in.scene != 5 && in.scene != 7) { stop(19); return true; }
    switch (m.state) {
    case 1:                                             // close what is open (B), then the Camp Menu
        if (since < 12) break;
        if (in.win_n < 0) { stop(11); break; }
        if (in.win_n == 0) { m.state = 2; m.presses = 0; break; }
        if (m.presses >= 6) { stop(12); break; }
        press(kBtnB); break;
    case 2:
        if (since < 6) break;
        press(kBtnX); m.state = 3; break;
    case 3:                                             // the Camp Menu's main menu, settled
        if (v.camp && v.state == 0 && v.menu_ready && since >= 20) { press(kBtnY); m.state = 4; break; }
        if (since > 300) stop(13);
        break;
    case 4:                                             // Party Formation, taking input
        if (v.camp && v.state == 6 && v.ch_ready && v.cur_pid > 0 && since >= 12) { m.state = 5; m.moves = 0; m.at = in.samples; m.moving = false; m.seen = -999; break; }
        // 0.9.64 the game didn't take the Y (0.9.63 on the Thor: "150", the Camp Menu still on its main menu): again
        if (v.camp && v.state == 0 && v.menu_ready && since >= 45 && m.retry < 3) { ++m.retry; press(kBtnY); break; }
        // 0.9.62 where it stopped says what was missing: 140 + 1 not the Camp Menu, 2 not state 6, 4 not taking
        // input, 8 no cursor member
        if (since > 300) stop(140 + (v.camp ? 0 : 1) + (v.state == 6 ? 0 : 2) + (v.ch_ready ? 0 : 4) + (v.cur_pid > 0 ? 0 : 8));
        break;
    case 5:                                             // the cursor to the member, then X
        if (!(v.camp && v.state == 6)) { stop(15); break; }
        if (!v.ch_ready || v.cur_pid <= 0 || since < 4) { if (since > 300) stop(160 + (v.ch_ready ? 0 : 4) + (v.cur_pid > 0 ? 0 : 8)); break; }
        if (!Settled(m, v.cur_pid, in.samples)) break;
        if (v.cur_pid == m.target) { press(kBtnX); m.state = 6; break; }
        if (m.moves >= 16) { stop(17); break; }
        ++m.moves; m.before = v.cur_pid; press(kBtnRight); m.moving = true; break;
    case 6:                                             // the game's leader is now this member
        if (in.leader == m.target) { m.result = 1; m.state = 7; m.at = in.samples; m.presses = 0; break; }
        if (v.camp && v.state == 6 && v.ch_ready && v.cur_pid == m.target && since >= 45 && m.retry < 3) { ++m.retry; press(kBtnX); break; }
        if (since > 180) stop(18);
        break;
    case 7:                                             // close the menus again (B until none is open)
        if (since < 14) break;
        if (in.win_n <= 0 || m.presses >= 8) { m.state = 0; break; }
        press(kBtnB); break;
    }
    return true;
}

// ---- 0.9.64 open a Challenge Mission in the game (r52 / r53): the Camp Menu's rows are UIListSelectorBase
// (currentIndex +0x8C, currentDataList +0x98) of UICampMenuItemData (menuItem +0x14: QuestAndMission 23,
// MissionList 25); the Missions window (WindowType 13) +0xC0 UIMissionListSelector: tab currentCategory +0xF8,
// cursor currentIndex +0x8C over currentDataList +0x98 of UIMissionListItemData (missionParameter +0x30 ->
// MissionID +0x10), input off while isDisableInput +0x89. Presses: d-pad up / down to a row, A, L / R for the tab.
constexpr uint64_t kLsIndex = 0x8C, kLsData = 0x98, kCmiItem = 0x14, kMlsCat = 0xF8, kMlidParam = 0x30;
constexpr int kBtnUp = 0, kBtnDown = 2, kBtnA = 7, kBtnL = 8, kBtnR = 9;
constexpr int kBtnZr = 10, kBtnZrDown = 11, kBtnZrUp = 12;   // 0.9.86 ZR held; ZR with the left stick down / up
struct ListView { bool ok; int index, n; uint64_t items; };
ListView ReadList(const Guest& g, uint64_t sel) {
    ListView v{}; v.index = -1;
    const uint64_t l = g.ptr(sel + kLsData);
    bool ok1 = false, ok2 = false;
    v.index = g.i32(sel + kLsIndex, &ok1);
    v.n = Guest::objp(l) ? g.i32(l + kListSize, &ok2) : -1;
    v.items = Guest::objp(l) ? g.ptr(l + kListItems) : 0;
    v.ok = ok1 && ok2 && v.n > 0 && v.n < 512 && Guest::objp(v.items);
    return v;
}
uint64_t ListItem(const Guest& g, const ListView& v, int i) { return g.ptr(v.items + kArrItems + 8ull * static_cast<uint64_t>(i)); }
bool MisStep(Instance& in, const EdenDsmodHostApi* h, QuickMenu& q) {
    MisMacro& m = g_mm;
    const int req = __atomic_exchange_n(&g_mis_req, -1, __ATOMIC_ACQ_REL);
    if (req >= 0 && req < 80 && m.state == 0 && g_lm.state == 0 && g_mrow_id[req] > 0) {
        int tab = -1;
        for (int i = 0; i < g_ms.n; ++i) if (g_ms.m[i].id == g_mrow_id[req]) { const int c = g_ms.m[i].cat; tab = c >= 3 && c <= 5 ? c - 2 : 0; }
        if (tab >= 0) {
            m = MisMacro{}; m.id = g_mrow_id[req]; m.tab = tab; m.state = 1; m.at = in.samples; q.phase = 0; q.want = -1;
            m.seen = -999;
            for (int i = 0; i < g_ms.n; ++i) if (g_ms.m[i].id == m.id) {
                int a, b; bool ach = false, c;
                m.claim = MissionState(in, g_ms.m[i], &a, &b, &ach, &c) && ach && !Bit(in.mis_clr, 8, m.id);
            }
            // 0.9.66 the game's Missions list already open: straight to its tab and cursor
            if (in.win_n > 0 && in.win_type[0] == 13) { m.state = 6; m.at = in.samples - 20; }
        }
    }
    if (const int ar = __atomic_exchange_n(&g_art_req, 0, __ATOMIC_ACQ_REL); ar > 0 && m.state == 0 && g_lm.state == 0) {
        m = MisMacro{}; m.item = 3; m.rsteps = ar - 1; m.state = 1; m.at = in.samples; m.seen = -999; q.phase = 0; q.want = -1;
    }
    {   // 0.9.122 Improve: the Camp Menu row after Special Arts/Spells (item 3), then its own menu's row
        if (in.win_n == 0) g_imp_live = false;          // 0.9.123 back in the field: the next Improve starts over
        const int ir = __atomic_exchange_n(&g_imp_req, 0, __ATOMIC_ACQ_REL);
        if (ir > 0 && m.state == 0 && g_lm.state == 0) {
            // 0.9.133 ir - 1 = the row + 3 x the R presses to the chosen member
            m = MisMacro{}; m.item = -2; m.sub = (ir - 1) % 3; m.rsteps = (ir - 1) / 3; m.state = 1; m.at = in.samples; m.seen = -999; q.phase = 0; q.want = -1;
            // 0.9.123 still in the Improve screens: back out to the Improve menu only (B), not the whole way
            if (g_imp_live && g_imp_cstate >= 0) m.state = 13;
        }
    }
    {   // 0.9.135 Equipment: Camp Menu > Equipment on the member and slot; when the companion's own Equipment screen
        // is still up (the windows read as they did when it was left), only R / L and Down / Up to the new one
        if (in.win_n == 0) g_eq_live = false;
        const int er = __atomic_exchange_n(&g_eq_req, 0, __ATOMIC_ACQ_REL);
        Guest g0{h};
        { const CampView c = ReadCamp(in, g0); g_eq_now[0] = c.camp ? 1 : 0; g_eq_now[1] = c.state; }
        auto same = [&]() {
            if (!g_eq_live || in.win_n <= 0) return false;
            const CampView c = ReadCamp(in, g0);
            // 0.9.136 the window count left out (it may change inside the Equipment screen)
            return in.win_type[0] == g_eq_sig[1] && (c.camp ? 1 : 0) == g_eq_sig[2] && (!c.camp || c.state == g_eq_sig[3]);
        };
        if (m.state == 0 && g_lm.state == 0) {
            if (er > 0) {
                // 0.9.141 the player's choice: Equipment only (Camp Menu > Equipment), no member or slot moves -- the
                // R presses across members landed short; with the screen already up, nothing to do
                const int slot = 0, mem = 0;
                (void)slot;
                // 0.9.139 the same member only: a different one backs out and goes in again (R presses across members
                // drift when repeated)
                if (same()) {
                } else if (false) {
                    m = MisMacro{}; m.eq = m.quick = true; m.rsteps = mem - g_eq_mem; m.dslot = slot - g_eq_slot;
                    m.tmem = mem; m.tslot = slot; m.state = 14; m.at = in.samples; m.seen = -999; q.phase = 0; q.want = -1;
                } else {
                    g_eq_live = false;
                    m = MisMacro{}; m.item = -3; m.eq = true; m.rsteps = mem; m.dslot = 0; m.tmem = mem; m.tslot = 0;
                    m.state = 1; m.at = in.samples; m.seen = -999; q.phase = 0; q.want = -1;
                }
            }
            // 0.9.137 picking another member on the companion leaves the game alone (the player's choice): the next
            // Equipment card tap moves the game to that member and slot
        }
    }
    if (m.state == 0) return false;
    if (q.fire >= 0) return true;
    Guest g{h};
    const uint64_t since = in.samples - m.at;
    auto press = [&](int b) { q.fire = b; q.ack = h->get_i64 ? h->get_i64(h->userdata, "@flag:so2r.qp", -1) : -1; q.at = in.samples; m.at = in.samples; ++m.presses; };
    auto stop = [&](int why) { m.result = -1; m.why = why; m.state = 8; m.at = in.samples; m.presses = 0; };
    auto next = [&](int st) { m.state = st; m.at = in.samples; m.moves = 0; m.retry = 0; m.moving = false; m.seen = -999; };
    if (m.state == 1 && in.scene != 4 && in.scene != 5) { stop(10); return true; }
    const CampView cv = ReadCamp(in, g);
    uint64_t ms = 0;
    if (cv.camp) ms = g.ptr(WindowOf(in, g, 3) + kCwMenuSel);
    // the camp menu row whose item is one of these, and the cursor walked to it
    auto to_row = [&](uint64_t sel, int a, int b, int on_done) {
        const ListView lv = ReadList(g, sel);
        if (!lv.ok) { if (since > 300) stop(30 + m.state); return; }
        int want = -1;
        for (int i = 0; i < lv.n && want < 0; ++i) { const int it = g.i32(ListItem(g, lv, i) + kCmiItem); if (it == a || it == b) want = i; }
        if (want < 0) { stop(40 + m.state); return; }
        if (since < 4 || !Settled(m, lv.index, in.samples)) return;
        if (lv.index == want) { press(kBtnA); next(on_done); return; }
        if (m.moves >= 24) { stop(50 + m.state); return; }
        ++m.moves; m.before = lv.index; press(lv.index < want ? kBtnDown : kBtnUp); m.moving = true;
    };
    switch (m.state) {
    case 1:
        if (since < 6) break;                           // 0.9.134 (12)
        if (in.win_n < 0) { stop(11); break; }
        if (in.win_n == 0) { next(2); break; }
        if (m.presses >= 6) { stop(12); break; }
        press(kBtnB); break;
    case 2:
        if (since < 6) break;
        press(kBtnX); next(3); break;
    case 3:                                             // the main menu's Missions row (QuestAndMission or MissionList)
        if (!(cv.camp && cv.state == 0 && cv.menu_ready)) { if (since > 300) stop(13); break; }
        if (since < 10 && m.moves == 0) break;          // 0.9.134 (20)
        if (m.item > 0) { to_row(ms, m.item, m.item, 11); break; }   // 0.9.70 a Camp Menu row only
        if (m.item == -2 || m.item == -3) {                 // 0.9.122 Improve: the row after Special Arts/Spells
                                                            // 0.9.135 Equipment (-3): the row before it
            const ListView lv = ReadList(g, ms);
            if (!lv.ok) { if (since > 300) stop(33); break; }
            int want = -1;
            for (int i = 0; i < lv.n && want < 0; ++i) if (g.i32(ListItem(g, lv, i) + kCmiItem) == 3) want = i + (m.item == -2 ? 1 : -1);
            if (want < 0 || want >= lv.n) { stop(43); break; }
            if (since < 4 || !Settled(m, lv.index, in.samples)) break;
            if (lv.index == want) { press(kBtnA); next(m.item == -2 ? 12 : 11); break; }
            if (m.moves >= 24) { stop(53); break; }
            ++m.moves; m.before = lv.index; press(lv.index < want ? kBtnDown : kBtnUp); m.moving = true;
            break;
        }
        to_row(ms, 23, 25, 4); break;
    case 11:                                            // 0.9.70 the row taken: the game's own screen is the rest
        if (m.rsteps != 0 || m.dslot != 0 || m.eq) { next(14); break; }   // 0.9.133 first R to the chosen member
        m.result = 1; m.state = 0; break;
    case 14:                                            // 0.9.133 R (the game's next member) rsteps times, the screen
                                                        // given time to open first (it starts on the first member)
        if (since < (m.moves == 0 ? (m.quick ? 2 : 50) : 12)) break;    // 0.9.138 (0.9.134 30 / 10: the first R lost while the screen opened)
        if (m.moves == 0 && !m.quick) {
            // 0.9.140 the first R only once the game's screen is up and taking input (the Camp window on top, past
            // its menu, not disabled) for half a second -- Special Arts/Spells and Equipment open straight from the
            // Camp Menu and take longer than the Improve screens; 3 s at most
            static uint64_t ready_at;
            const bool up = cv.camp && cv.state != 0 && cv.menu_ready;
            if (!up) { ready_at = 0; if (since < 180) break; }
            else {
                if (!ready_at) ready_at = in.samples;
                if (in.samples - ready_at < 30 && since < 180) break;
            }
        }
        {   // 0.9.135 R / L to the member, then Down / Up to the Equipment slot
            const int nr = m.rsteps < 0 ? -m.rsteps : m.rsteps, ns = m.dslot < 0 ? -m.dslot : m.dslot;
            if (m.moves < nr) { ++m.moves; press(m.rsteps > 0 ? kBtnR : kBtnL); break; }
            if (m.moves < nr + ns) { ++m.moves; press(m.dslot > 0 ? kBtnDown : kBtnUp); break; }
            if (m.eq && since < 8) break;   // the last press shown, then how the Equipment screen's windows read
            if (m.eq) {
                g_eq_live = true; g_eq_mem = m.tmem; g_eq_slot = m.tslot;
                g_eq_sig[0] = in.win_n; g_eq_sig[1] = in.win_n > 0 ? in.win_type[0] : -1;
                g_eq_sig[2] = cv.camp ? 1 : 0; g_eq_sig[3] = cv.state;
            }
        }
        m.result = 1; m.state = 0; break;
    case 13:                                            // 0.9.123 B until the Improve menu is on top again
        // 0.9.124 the game's skill screens read just like the Improve menu (same Camp state, menu not disabled), so
        // the first B is always pressed; only after it is the Improve menu trusted
        if (in.win_n == 0) { g_imp_live = false; next(2); break; }        // the menus closed: open the Camp Menu
        if (cv.camp && cv.state == 0 && cv.menu_ready) { next(3); break; }  // at the Camp Menu: its Improve row again
        if (m.presses == 0) { if (since >= 4) press(kBtnB); break; }
        if (since < 12) break;                          // 0.9.134 (20)
        if (cv.camp && cv.state == g_imp_cstate && cv.menu_ready) { next(12); break; }
        if (m.presses >= 5) { g_imp_live = false; next(1); break; }   // lost: start over the long way
        press(kBtnB); break;
    case 12: {                                          // 0.9.122 the Improve menu: its row by place, then A
        if (!(cv.camp && cv.state != 0 && cv.menu_ready)) { if (since > 300) stop(70); break; }
        g_imp_cstate = cv.state;
        if (since < 8 && m.moves == 0) break;           // 0.9.134 (12)
        const ListView lv = ReadList(g, ms);
        if (!lv.ok || m.sub >= lv.n) { if (since > 300) stop(71); break; }
        if (!Settled(m, lv.index, in.samples)) break;
        if (lv.index == m.sub) { press(kBtnA); g_imp_live = true; next(11); break; }
        if (m.moves >= 12) { stop(72); break; }
        ++m.moves; m.before = lv.index; press(lv.index < m.sub ? kBtnDown : kBtnUp); m.moving = true;
        break;
    }
    case 4:                                             // a submenu: its Missions list row; or the window already open
        if (in.win_n > 0 && in.win_type[0] == 13) { next(5); break; }
        if (cv.camp && cv.state != 0 && cv.state != 4) { stop(60 + cv.state); break; }   // 0.9.65 another submenu
        if (cv.camp && cv.state == 4 && cv.menu_ready) { if (since < 12 && m.moves == 0) break; to_row(ms, 25, 25, 5); break; }
        if (cv.camp && cv.state == 0 && cv.menu_ready && since >= 45 && m.retry < 3) { ++m.retry; press(kBtnA); break; }
        if (since > 300) stop(14);
        break;
    case 5: case 6: case 7: {                           // the Missions window: tab, then the cursor
        if (!(in.win_n > 0 && in.win_type[0] == 13)) {
            if (m.state == 5 && cv.camp && cv.state == 4 && cv.menu_ready && since >= 45 && m.retry < 3) { ++m.retry; press(kBtnA); break; }
            if (since > 300) stop(15);
            break;
        }
        const uint64_t mw = WindowOf(in, g, 13);
        const uint64_t sel = g.is(mw, "UIMissionWindow") ? g.ptr(mw + 0xC0) : 0;
        if (!g.is(sel, "UIMissionListSelector")) { if (since > 300) stop(16); break; }
        if (g.u8(sel + kCmsDisable) != 0 || since < 6) { if (since > 300) stop(17); break; }
        const int cat = g.i32(sel + kMlsCat);
        if (m.state == 5) { next(6); break; }
        if (m.state == 6) {
            if (!Settled(m, cat, in.samples)) break;
            if (cat == m.tab) { next(7); break; }
            if (m.moves >= 8) { stop(18); break; }
            ++m.moves; m.before = cat; press(cat < m.tab ? kBtnR : kBtnL); m.moving = true; break;
        }
        const ListView lv = ReadList(g, sel);
        if (cat != m.tab) { next(6); break; }
        if (!lv.ok) { if (since > 300) stop(19); break; }
        int want = -1;
        for (int i = 0; i < lv.n && want < 0; ++i) {
            const uint64_t mp = g.ptr(ListItem(g, lv, i) + kMlidParam);
            if (Guest::objp(mp) && g.i32(mp + kMsId) == m.id) want = i;
        }
        if (want < 0) { stop(20); break; }
        if (!Settled(m, lv.index, in.samples)) break;
        if (lv.index == want) {                                          // there
            if (m.claim) { press(kBtnA); m.state = 9; m.retry = 0; break; }
            m.result = 1; m.state = 0; break;                             // the game's own screen is the rest
        }
        if (m.moves >= 120) { stop(21); break; }
        ++m.moves; m.before = lv.index; press(lv.index < want ? kBtnDown : kBtnUp); m.moving = true; break;
    }
    case 9:                                             // 0.9.66 the CLEAR dialog: its OK
        // 0.9.68 (0.9.67 on the Thor: the dialog stayed, it isn't a Dialog window): the game takes the reward with the
        // first A (clearedMissionFlag set) and shows CLEAR; once that is seen, 0.75 s later, A for its OK
        // 0.9.69 (0.9.68 on the Thor: windows "top 12 13 3"): the CLEAR dialog is a System window (WindowType 12)
        // over the Missions window; the reward's flag is set only after its OK
        if (in.win_n > 0 && (in.win_type[0] == 12 || in.win_type[0] == 1)) { if (since >= 30) { m.state = 10; m.at = in.samples - 45; } break; }
        if (Bit(in.mis_clr, 8, m.id)) { m.state = 10; m.at = in.samples; break; }
        if (since > 180) { m.result = 1; m.state = 0; }  // not taken: leave it where it is
        break;
    case 10:                                            // OK, once; then until the dialog is gone (at most 3)
        if (since < 45) break;
        if (in.win_n > 0 && (in.win_type[0] == 12 || in.win_type[0] == 1) && m.retry < 3) { ++m.retry; press(kBtnA); break; }
        m.result = 1; m.state = 0; break;
    case 8:                                             // stopped: close what it opened
        if (since < 14) break;
        if (in.win_n <= 0 || m.presses >= 8) { m.state = 0; break; }
        press(kBtnB); break;
    }
    return true;
}

// ---- 0.9.72 a battle: the BattleManager by the game's own route to it (BattleStateAssist.OnUpdate: the slot
// main+4947868 -> [+0] -> [+0x20] -> [+0xC0] -> [+0] -> static fields [+0xB8] -> [+0]), checked by its class name;
// battlePlayerList +0x30 [controlPlayerIndex +0x38] -> battleCharacterParameter +0x150 -> CharacterParameter +0x10
// -> CharacterID; battleAssistInfo +0x138 -> assistDataArray +0x10 (characterParameter +0x28, currentState +0x30),
// lastAssistIndex +0x20.
constexpr uint64_t kBmSlot = 0x4947868, kBmPlayers = 0x30, kBmCtrl = 0x38, kBmAssist = 0x138, kBcParam = 0x150,
                   kBpChara = 0x10, kBaArray = 0x10, kBaLast = 0x20, kBadChara = 0x28, kBadState = 0x30,
                   kBadPointMax = 0x18, kBadAssistId = 0x10,   // 0.9.75 r57
                   kPpAssistPoints = 0x90, kBmCommand = 0x148,
                   kUpSystemInts = 0x138, kBonusPointIdx = 49, kBmBonusStages = 0x130,   // 0.9.89 r64 / r65
                   kPmSpecialMsg = 0x330;   // 0.9.90 r66   // 0.9.76 r59 (PartyParameter float[] by assistID)
// 0.9.90 a bonus effect's words as the game makes them (UICommon.GetBonusBuffMessage): the message with its one
// placeholder -- a {..}, [..] or %d token, or a <..> tag holding no '=' -- replaced by the value, the other tags
// (<color=..> ...) dropped. With no placeholder the text is kept as it is.
void BonusText(const char* raw, int value, char* out, int cap) {
    char num[16];
    int nl = 0;
    {
        char t[16]; int tl = 0; unsigned v = value < 0 ? static_cast<unsigned>(-value) : static_cast<unsigned>(value);
        do { t[tl++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && tl < 12);
        if (value < 0) num[nl++] = '-';
        while (tl) num[nl++] = t[--tl];
        num[nl] = 0;
    }
    int o = 0;
    bool done = false;
    for (int i = 0; raw[i] && o < cap - 1;) {
        const char c = raw[i];
        if (!done && (c == '{' || c == '[' || (c == '%' && raw[i + 1] == 'd'))) {
            int j = i + 1;
            if (c == '%') j = i + 2;
            else { const char e = c == '{' ? '}' : ']'; while (raw[j] && raw[j] != e) ++j; if (raw[j]) ++j; }
            for (int k = 0; num[k] && o < cap - 1; ++k) out[o++] = num[k];
            done = true; i = j; continue;
        }
        if (c == '<') {
            int j = i + 1; bool eqs = false;
            while (raw[j] && raw[j] != '>') { if (raw[j] == '=') eqs = true; ++j; }
            if (!done && !eqs && raw[i + 1] != '/' && raw[j]) {
                for (int k = 0; num[k] && o < cap - 1; ++k) out[o++] = num[k];
                done = true;
            }
            i = raw[j] ? j + 1 : j; continue;
        }
        out[o++] = c; ++i;
    }
    out[o] = 0;
}

// 0.9.110 the bonus gauge and the assault members outside a battle too (the player's wish: the page was bare while
// roaming). Their lists live in the BattleManager, which a battle alone has, so the last battle's are kept: the bonus
// stages (each one's count, effect words and icon) and the assault members (assistID, max, name, picture). The
// numbers that change are read live from the save: the bonus points (UserParameter +0x138 [49]) and each assault
// member's point (PartyParameter +0x90 [assistID]). Nothing is shown before the first battle since the game started.
struct BattleKeep {
    bool bn; int rows; int cnt[8]; char text[8][64]; int icon[8];
    bool as; int n; int cid[4], pic[4], aid[4]; float base[4]; char model[4][24]; char name[4][32];
};
BattleKeep g_keep;
void KeepBonusStage(int i, int cnt, const char* text, int icon) {
    if (i < 0 || i >= 8) return;
    g_keep.cnt[i] = cnt; g_keep.icon[i] = icon;
    size_t k = 0; if (text) for (; text[k] && k + 1 < sizeof g_keep.text[0]; ++k) g_keep.text[i][k] = text[k];
    g_keep.text[i][k] = 0;
}
void RestoreBattleKeep(Instance& in, Guest& g) {
    if (!in.ok || !in.pm) return;
    if (g_keep.bn && g_keep.rows > 0) {
        const uint64_t arr = g.ptr(g.ptr(in.pm + kPmUser) + kUpSystemInts);
        bool okl = false;
        const int32_t n = Guest::objp(arr) ? g.i32(arr + 0x18, &okl) : 0;
        if (okl && n > static_cast<int32_t>(kBonusPointIdx)) {
            const int32_t pt = g.i32(arr + kArrItems + 4ull * kBonusPointIdx);
            int rem = pt, total = 0, lvl = 0, ratio = 1000;
            for (int i = 0; i < g_keep.rows; ++i) {
                const int cnt = g_keep.cnt[i];
                if (cnt <= 0) break;
                total += cnt;
                if (rem - cnt < 0) { ratio = rem * 1000 / cnt; break; }
                rem -= cnt; ++lvl;
            }
            in.bn_on = pt >= 0; in.bn_pt = pt; in.bn_next = total; in.bn_ratio = ratio; in.bn_level = lvl; in.bn_rows = g_keep.rows;
            in.bn_n = 0;
            for (int i = 0; i < lvl && i < g_keep.rows && in.bn_n < 3; ++i) {
                if (!g_keep.text[i][0]) continue;
                memcpy(in.bn_text[in.bn_n], g_keep.text[i], sizeof in.bn_text[0]);
                in.bn_icon[in.bn_n] = g_keep.icon[i];
                ++in.bn_n;
            }
        }
    }
    if (g_keep.as && g_keep.n > 0) {
        const uint64_t pts = g.ptr(g.ptr(g.ptr(in.pm + kPmUser) + kUpParty) + kPpAssistPoints);
        bool okn = false;
        const int32_t np = Guest::objp(pts) ? g.i32(pts + 0x18, &okn) : 0;
        for (int i = 0; i < g_keep.n && i < 4; ++i) {
            in.as_cid[i] = g_keep.cid[i]; in.as_pic[i] = g_keep.pic[i]; in.as_state[i] = -1;
            memcpy(in.as_model[i], g_keep.model[i], sizeof in.as_model[0]);
            memcpy(in.as_name[i], g_keep.name[i], sizeof in.as_name[0]);
            float t = 0; bool okp = false;
            const int32_t id = g_keep.aid[i];
            if (okn && id >= 0 && id < np && np < 256) t = g.f32(pts + kArrItems + 4ull * static_cast<uint64_t>(id), &okp);
            const float b = g_keep.base[i];
            in.as_t[i] = t; in.as_base[i] = b;
            in.as_rise[i] = okp && b > 0.01f && t == t ? (t >= b ? 1000 : t <= 0 ? 0 : static_cast<int>(t * 1000 / b)) : -1;
            in.as_n = i + 1;
        }
    }
}

// 0.9.142 HideHUD (defined after Sample)
void HideHudStep(const Instance& in, const EdenDsmodHostApi* host);
void ReadBattle(Instance& in, const EdenDsmodHostApi* h) {
    in.bm = 0; in.bm_step = 0; in.ctrl_cid = -1; in.ctrl_idx = -1; in.as_n = 0; in.as_last = -1; in.order = -1; in.bn_on = 0; in.bn_n = 0;
    if (in.scene != 6 && in.main_base) { Guest g0{h}; RestoreBattleKeep(in, g0); return; }   // 0.9.110
    if (in.scene != 6 || !in.main_base) return;
    Guest g{h};
    uint64_t p = g.ptr(in.main_base + kBmSlot);
    in.bm_step = 1; if (!p) return;
    p = g.ptr(p); in.bm_step = 2; if (!p) return;
    p = g.ptr(p + 0x20); in.bm_step = 3; if (!p) return;
    p = g.ptr(p + 0xC0); in.bm_step = 4; if (!p) return;
    p = g.ptr(p); in.bm_step = 5; if (!p) return;
    p = g.ptr(p + 0xB8); in.bm_step = 6; if (!p) return;
    p = g.ptr(p); in.bm_step = 7;
    if (!g.is(p, "BattleManager")) return;
    in.bm = p; in.bm_step = 8;
    {   // 0.9.81 the Y order; 0.9.85 the battle's own (BattleManager.commandID +0x148, r55: set by the Y list's
        // OnDecision -> BattleManager.SetCommandID). PartyParameter.commandID +0x20 is only the saved one, so it
        // didn't follow a change made in battle.
        bool ok8 = false;
        const int32_t c = g.i32(p + kBmCommand, &ok8);
        in.order = ok8 && c >= 1 && c <= 4 ? c : -1;
    }
    {   // 0.9.89 the bonus gauge (r64 / r65). The points: UserParameter +0x138, an int[], entry 49 (PartyManager.
        // GetBattleSphereBonusPoint). The stages: BattleManager +0x130 -> +0x20, a List<ConstBattleSphereBonus-
        // Parameter> (sphereCount +0x18 each). As UICommon / BattleManager do: walk the stages taking each one's
        // count off the points; the first it can't take is the stage being filled: "points / the counts so far"
        // (the "48 / 50"), the gauge at what's left of it; all taken: full (the last total).
        in.bn_on = 0; in.bn_pt = in.bn_next = in.bn_ratio = in.bn_level = in.bn_rows = 0;
        const uint64_t arr = g.ptr(g.ptr(in.pm + kPmUser) + kUpSystemInts);
        bool okl = false;
        const int32_t n = Guest::objp(arr) ? g.i32(arr + 0x18, &okl) : 0;
        const uint64_t lst = g.ptr(g.ptr(p + kBmBonusStages) + 0x20);
        bool okr = false;
        const int32_t rows = Guest::objp(lst) ? g.i32(lst + 0x18, &okr) : 0;
        const uint64_t items = Guest::objp(lst) ? g.ptr(lst + 0x10) : 0;
        if (okl && n > static_cast<int32_t>(kBonusPointIdx) && okr && rows > 0 && rows <= 8 && Guest::objp(items)) {
            const int32_t pt = g.i32(arr + kArrItems + 4ull * kBonusPointIdx);
            int rem = pt, total = 0, lvl = 0, ratio = 1000;
            bool all = true;
            g_keep.rows = rows < 8 ? rows : 8;                            // 0.9.110 kept for outside a battle
            for (int32_t i = 0; i < rows && i < 8; ++i) {
                const uint64_t r = g.ptr(items + kArrItems + 8ull * static_cast<uint64_t>(i));
                g_keep.cnt[i] = Guest::objp(r) ? g.i32(r + 0x18) : 0;
                g_keep.text[i][0] = 0; g_keep.icon[i] = -1;
            }
            g_keep.bn = true;
            for (int32_t i = 0; i < rows; ++i) {
                const uint64_t r = g.ptr(items + kArrItems + 8ull * static_cast<uint64_t>(i));
                const int32_t cnt = Guest::objp(r) ? g.i32(r + 0x18) : 0;
                if (cnt <= 0) { all = false; break; }
                total += cnt;
                if (rem - cnt < 0) { ratio = rem * 1000 / cnt; all = false; break; }
                rem -= cnt; ++lvl;
            }
            (void)all;
            in.bn_on = pt >= 0; in.bn_pt = pt; in.bn_next = total; in.bn_ratio = ratio; in.bn_level = lvl; in.bn_rows = rows;
            // 0.9.90 the effects in force: the stages passed (r66, as UICommon.GetBonusBuffMessage words them): the
            // stage's specialEffectsMessageID (+0x28) in ParameterManager.specialEffectsMessageDictionary (+0x330),
            // that message's description (+0x20) with its placeholder replaced by the stage's effectValue (+0x20);
            // its icon by UICommon.ToIconType (BonusBuffType +0x1C -> SphereBonusIconType, the game's table)
            in.bn_n = 0;
            const uint64_t dict = g.ptr(in.pm + kPmSpecialMsg);
            for (int32_t i = 0; i < rows && i < 8 && Guest::objp(dict); ++i) {   // 0.9.110 every stage's (kept)
                const uint64_t r = g.ptr(items + kArrItems + 8ull * static_cast<uint64_t>(i));
                if (!Guest::objp(r)) continue;
                uint16_t kw[40];
                const uint64_t ks = g.ptr(r + 0x28);
                const int kl = ks ? g.utf16(ks, kw, 40) : 0;
                const uint64_t msg = kl > 0 && kl < 40 ? DictFind(g, dict, kw, kl) : 0;
                char raw[64];
                raw[0] = 0;
                if (Guest::objp(msg)) g.string(g.ptr(msg + 0x20), raw, sizeof raw, false);
                if (!raw[0]) continue;
                const float ev = g.f32(r + 0x20);
                char txt[64];
                BonusText(raw, static_cast<int>(ev), txt, sizeof txt);
                const int bt = g.i32(r + 0x1C);
                static constexpr int kIcon[22] = {5, 8, 5, 4, 8, 0, 4, 1, 2, 3, 5, 7, 1, 4, 4, 0, 0, 5, 7, 7, 7, 7};
                const int icon = bt >= 1 && bt <= 22 ? kIcon[bt - 1] : -1;
                KeepBonusStage(i, g_keep.cnt[i], txt, icon);
                if (i >= lvl || in.bn_n >= 3) continue;                    // in force: the stages passed
                memcpy(in.bn_text[in.bn_n], txt, sizeof in.bn_text[0]);
                in.bn_icon[in.bn_n] = icon;
                ++in.bn_n;
            }
        }
    }
    bool ok = false;
    const uint64_t list = g.ptr(p + kBmPlayers);
    const int32_t n = Guest::objp(list) ? g.i32(list + kListSize, &ok) : 0;
    const int32_t ci = g.i32(p + kBmCtrl);
    if (ok && n > 0 && n <= 8 && ci >= 0 && ci < n) {
        const uint64_t bp = g.ptr(g.ptr(list + kListItems) + kArrItems + 8ull * static_cast<uint64_t>(ci));
        const uint64_t cp = g.ptr(g.ptr(bp + kBcParam) + kBpChara);
        if (g.is(cp, "CharacterParameter")) { in.ctrl_cid = g.i32(cp + kCpId); in.ctrl_idx = ci; }
    }
    const uint64_t ai = g.ptr(p + kBmAssist);
    if (!Guest::objp(ai)) return;
    in.as_last = g.i32(ai + kBaLast);
    const uint64_t arr = g.ptr(ai + kBaArray);
    const int32_t an = Guest::objp(arr) ? g.i32(arr + 0x18, &ok) : 0;
    for (int32_t i = 0; ok && i < an && i < 4; ++i) {
        const uint64_t d = g.ptr(arr + kArrItems + 8ull * static_cast<uint64_t>(i));
        const uint64_t cp = Guest::objp(d) ? g.ptr(d + kBadChara) : 0;
        in.as_cid[i] = g.is(cp, "CharacterParameter") ? g.i32(cp + kCpId) : -1;
        in.as_state[i] = Guest::objp(d) ? g.i32(d + kBadState) : -1;
        in.as_name[i][0] = 0;
        // 0.9.74 its picture (the member's, or the assist's own playerID: ConstAssistParameter +0x14)
        // 0.9.75 how full its gauge is, the way the game's button is filled (research r57): the assist point
        // (CharacterParameter.assistPoint +0x178, raised by assistRiseValue every assistRiseTime seconds) of
        // assistPointMax (BattleAssistData +0x18); the +0x40 timer read before is only that tick
        {
            const uint64_t ap0 = Guest::objp(d) ? g.ptr(d + 0x38) : 0;
            in.as_pic[i] = in.as_cid[i] > 0 ? in.as_cid[i] : g.is(ap0, "ConstAssistParameter") ? g.i32(ap0 + 0x14) : -1;
            in.as_model[i][0] = 0;
            if (g.is(ap0, "ConstAssistParameter")) g.string(g.ptr(ap0 + 0x18), in.as_model[i], 24, true);
            float t = 0, b = 0;
            bool okp = false, okm = false;
            // 0.9.76 (r59) the point is kept per assistID in the save: ParameterManager +0x7E0 (UserParameter) +0x38
            // (PartyParameter) +0x90 float[assistID]; UIBattleController.UpdateBattleAssist gives the ring
            // min(point / assistPointMax, 1)
            if (Guest::objp(d)) {
                const int32_t id = g.i32(d + kBadAssistId);
                const uint64_t pts = g.ptr(g.ptr(g.ptr(in.pm + kPmUser) + kUpParty) + kPpAssistPoints);
                bool okn = false;
                const int32_t np = Guest::objp(pts) ? g.i32(pts + 0x18, &okn) : 0;
                if (okn && id >= 0 && id < np && np < 256) t = g.f32(pts + kArrItems + 4ull * static_cast<uint64_t>(id), &okp);
                b = g.f32(d + kBadPointMax, &okm);
            }
            in.as_t[i] = t; in.as_base[i] = b;
            in.as_rise[i] = okp && okm && b > 0.01f && t == t ? (t >= b ? 1000 : t <= 0 ? 0 : static_cast<int>(t * 1000 / b)) : -1;
        }
        for (int j = 0; j < in.rows && j < kRows; ++j)
            if (in.m[j].on && in.as_cid[i] > 0 && in.m[j].cid == in.as_cid[i]) { memcpy(in.as_name[i], in.m[j].name, 31); in.as_name[i][31] = 0; }
        if (!in.as_name[i][0] && Guest::objp(d)) {   // not a party member: the assist's own name (ConstAssistParameter
            const uint64_t ap = g.ptr(d + 0x38);       // +0x38, charaNameID +0x28 -> characterMessages firstName)
            uint16_t key[32];
            const int kl = g.is(ap, "ConstAssistParameter") ? g.utf16(g.ptr(ap + 0x28), key, 32) : 0;
            const uint64_t cm = kl > 0 ? DictFind(g, g.ptr(in.pm + kPmCharMsg), key, kl) : 0;
            if (g.is(cm, "ConstCharacterMessage")) g.string(g.ptr(cm + kCcmFirst), in.as_name[i], 32, true);
        }
        in.as_n = i + 1;
    }
    if (in.as_n > 0) {                                                    // 0.9.110 kept for outside a battle
        g_keep.as = true; g_keep.n = in.as_n;
        for (int i = 0; i < in.as_n; ++i) {
            const uint64_t d = g.ptr(arr + kArrItems + 8ull * static_cast<uint64_t>(i));
            g_keep.aid[i] = Guest::objp(d) ? g.i32(d + kBadAssistId) : -1;
            g_keep.cid[i] = in.as_cid[i]; g_keep.pic[i] = in.as_pic[i]; g_keep.base[i] = in.as_base[i];
            memcpy(g_keep.model[i], in.as_model[i], sizeof g_keep.model[0]);
            memcpy(g_keep.name[i], in.as_name[i], sizeof g_keep.name[0]);
        }
    }
}

// ---- 0.9.72 a battle command (the Battle page): X for the game's battle menu, then its cursor to the command
// (UIBattleWindow, WindowType 2: the selector on top of its selectorStack +0x98 must be UIBattleMenuSelector, so a
// d-pad press can't land on the field, where it would be an assault action), then A.
int g_as_req = -1;      // 0.9.73 an assault action: its d-pad button
struct BatMacro { int state, want, moves, presses, result, why, retry; uint64_t at; int seen, before; uint64_t seen_at; bool moving; int kind; };
BatMacro g_bmac;
int g_bcmd_req = -1;
uint64_t BattleTop(const Instance& in, const Guest& g, int* depth) {
    *depth = -1;
    const uint64_t w = WindowOf(in, g, 2);
    if (!g.is(w, "UIBattleWindow")) return 0;
    const uint64_t st = g.ptr(w + 0x98);
    if (!Guest::objp(st)) return 0;
    bool ok = false;
    const int32_t n = g.i32(st + 0x18, &ok);
    const uint64_t arr = g.ptr(st + 0x10);
    if (!ok || n < 0 || n > 16) return 0;
    *depth = n;
    return n > 0 && Guest::objp(arr) ? g.ptr(arr + kArrItems + 8ull * static_cast<uint64_t>(n - 1)) : 0;
}
bool BatStep(Instance& in, const EdenDsmodHostApi* h, QuickMenu& q) {
    BatMacro& m = g_bmac;
    const int req = __atomic_exchange_n(&g_bcmd_req, -1, __ATOMIC_ACQ_REL);
    if (req >= 0 && req < 4 && m.state == 0 && in.scene == 6) { m = BatMacro{}; m.want = req; m.state = 1; m.at = in.samples; m.seen = -999; }
    {   // 0.9.82 the game's Y list on top? (for the page's list, and the order macro below)
        Guest g0{h};
        int d0 = -1;
        const int was = in.op_open;
        in.op_open = in.scene == 6 && g0.is(BattleTop(in, g0, &d0), "UIBattleOperationSelector");
        if (was && !in.op_open && m.state == 0) __atomic_store_n(&g_opick, 0, __ATOMIC_RELEASE);   // 0.9.83 closed in the game
    }
    const int sreq = __atomic_exchange_n(&g_bsw_req, -1, __ATOMIC_ACQ_REL);   // 0.9.86 switch control
    if (sreq > 0 && m.state == 0 && in.scene == 6 && sreq != in.ctrl_cid) {
        m = BatMacro{}; m.kind = 2; m.want = sreq; m.state = 31; m.at = in.samples; m.seen = -999;
    }
    const int oreq = __atomic_exchange_n(&g_bord_req, -1, __ATOMIC_ACQ_REL);
    if (oreq >= 1 && oreq <= 4 && m.state == 0 && in.scene == 6 && oreq != in.order) {
        m = BatMacro{}; m.kind = 1; m.want = oreq; m.state = 21; m.at = in.samples; m.seen = -999;
    }
    {   // 0.9.73 an assault action: its d-pad direction, only with no battle screen up (else it moves a cursor)
        const int b = __atomic_exchange_n(&g_as_req, -1, __ATOMIC_ACQ_REL);   // the d-pad button (0 up .. 3 right)
        int a = -1;
        for (int i = 0; i < 4 && b >= 0; ++i) if (kAssistBtn[i] == b && i < in.as_n) a = i;
        if (a >= 0 && m.state == 0 && in.scene == 6 && q.fire < 0) {
            Guest g{h};
            int depth = -1;
            BattleTop(in, g, &depth);
            if (depth == 0) {
                q.fire = kAssistBtn[a]; q.ack = h->get_i64 ? h->get_i64(h->userdata, "@flag:so2r.qp", -1) : -1; q.at = in.samples;
                return true;
            }
        }
    }
    if (m.state == 0) return false;
    if (q.fire >= 0) return true;
    Guest g{h};
    const uint64_t since = in.samples - m.at;
    auto press = [&](int b) { q.fire = b; q.ack = h->get_i64 ? h->get_i64(h->userdata, "@flag:so2r.qp", -1) : -1; q.at = in.samples; m.at = in.samples; ++m.presses; };
    auto stop = [&](int why) { m.result = -1; m.why = why; m.state = 0; };
    if (in.scene != 6) { stop(10); return true; }
    int depth = -1;
    const uint64_t top = BattleTop(in, g, &depth);
    const bool menu = g.is(top, "UIBattleMenuSelector") && g.u8(top + kCmsDisable) == 0;
    switch (m.state) {
    case 1:                                             // the menu open already, or X
        if (menu) { m.state = 3; m.at = in.samples; break; }
        if (depth > 0) {                                // 0.9.74 another battle screen (Items ...): B back to the menu
            if (since < 12) break;
            if (m.presses >= 4) { stop(11); break; }
            press(kBtnB); break;
        }
        if (depth < 0) { stop(15); break; }
        press(kBtnX); m.state = 2; break;
    case 2:
        if (menu && since >= 10) { m.state = 3; m.at = in.samples; break; }
        if (depth == 0 && since >= 45 && m.retry < 3) { ++m.retry; press(kBtnX); break; }
        if (since > 180) stop(12);
        break;
    // ---- 0.9.82 an order (r62): Y opens UIBattleOperationSelector (a list selector on the battle screens' stack:
    // cursor +0x8C, its rows itemDataList +0xC0, each row's commandID +0x20); the cursor to the row with the wanted
    // order, then A (OnDecision -> BattleManager.SetCommandID); done when PartyParameter.commandID is that order.
    // The d-pad is pressed only with that list on top, so it can never call an assault.
    case 21:                                            // the list open already, or Y
        if (in.op_open) { m.state = 23; m.at = in.samples; break; }
        if (depth > 0) {                                // another battle screen: B back to the field first
            if (since < 12) break;
            if (m.presses >= 4) { stop(21); break; }
            press(kBtnB); break;
        }
        if (depth < 0) { stop(25); break; }
        press(kBtnY); m.state = 22; break;
    case 22:
        if (in.op_open && since >= 10) { m.state = 23; m.at = in.samples; break; }
        if (depth == 0 && since >= 45 && m.retry < 3) { ++m.retry; press(kBtnY); break; }
        if (since > 180) stop(22);
        break;
    case 23: {
        if (!in.op_open) { if (since > 90) stop(23); break; }
        const int idx = g.i32(top + kLsIndex);
        if (!Settled(m, idx, in.samples) || since < 4) break;
        const uint64_t lst = g.ptr(top + 0xC0);
        bool okn = false;
        const int32_t nn = Guest::objp(lst) ? g.i32(lst + 0x18, &okn) : 0;
        const uint64_t arr = Guest::objp(lst) ? g.ptr(lst + 0x10) : 0;
        int ti = -1;
        for (int32_t i = 0; okn && i < nn && i < 8 && Guest::objp(arr); ++i) {
            const uint64_t it = g.ptr(arr + kArrItems + 8ull * static_cast<uint64_t>(i));
            if (Guest::objp(it) && g.i32(it + 0x20) == m.want) ti = i;
        }
        if (ti < 0) { stop(26); break; }
        if (idx == ti) { press(kBtnA); m.state = 24; break; }
        if (idx < 0 || idx >= nn || m.moves >= 8) { stop(24); break; }
        ++m.moves; m.before = idx; press(idx < ti ? kBtnDown : kBtnUp); m.moving = true; break;
    }
    case 24:                                            // the order taken?
        if (in.order == m.want) { m.result = 1; m.state = 0; break; }
        if (since > 120) stop(27);
        break;
    // ---- 0.9.86 switch the controlled character (the player: ZR held, the left stick up / down steps through
    // the party; r63: BattleManager.OnStart/OnEndControlPlayerChangeMode). One step: ZR (held ~0.4 s), then ZR with
    // the left stick down; done when BattleManager.controlPlayerIndex's member is the one tapped, else step again.
    case 31:
        if (in.ctrl_cid == m.want) { m.result = 1; m.state = 0; break; }
        if (depth != 0 || in.op_open) { stop(depth < 0 ? 35 : 31); break; }   // only on the open battle field
        if (m.moves >= 6) { stop(32); break; }
        m.before = in.ctrl_cid; press(kBtnZr); m.state = 32; break;
    case 32:
        // 0.9.156 the stick ~0.1 s after ZR as in 0.9.86-0.9.133 (presses every 100 ms then); at 60 ms it came
        // before the game's change mode was on, ZR alone handed control back to the leader
        if (in.samples - q.at >= 4) { ++m.moves; press(m.retry ? kBtnZrUp : kBtnZrDown); m.state = 33; }   // from ZR's press (q.at: its acknowledgement): ZR+stick at ~0.12 s
        break;
    case 33:
        if (in.ctrl_cid == m.want) { m.result = 1; m.state = 0; break; }
        // 0.9.156 the next step once ZR's hold (24 frames) is over, as the 100 ms presses gave it before 0.9.134
        if (in.ctrl_cid != m.before && since >= 24) { m.state = 31; m.at = in.samples; break; }   // another member: on
        if (since > 40) {                               // nothing changed: once the other way, then give up
            if (m.retry++ < 1) { m.state = 31; m.at = in.samples; break; }
            stop(33);
        }
        break;
    case 3: {
        if (!menu) { if (since > 90) stop(13); break; }
        const int idx = g.i32(top + kLsIndex);
        if (!Settled(m, idx, in.samples) || since < 4) break;
        if (idx == m.want) { press(kBtnA); m.result = 1; m.state = 0; break; }
        if (idx < 0 || idx > 3 || m.moves >= 8) { stop(14); break; }
        ++m.moves; m.before = idx; press(idx < m.want ? kBtnDown : kBtnUp); m.moving = true; break;
    }
    }
    return true;
}

[[maybe_unused]] void BattleDiagLine(const Instance& in, const EdenDsmodHostApi* h, Str& l) {
        Guest g{h};
        int depth = -1;
        const uint64_t top = BattleTop(in, g, &depth);
        l.clear().add("battle: steps ").num(g_bmac.kind).ch(':').num(g_bmac.state).ch('/').num(g_bmac.result).ch('/').num(g_bmac.why)
         .add(" | bm ").num(in.bm_step).add(in.bm ? " ok" : " -").add(" ctrl ").num(in.ctrl_idx).ch('/').num(in.ctrl_cid);
    l.add(" | ab");   // 0.9.157 each battle member's BuffDebuffState word (bit n: BuffDebuffID n)
    for (int i = 0; i < in.rows && i < kRows; ++i) if (in.m[i].on && in.m[i].battle) l.ch(' ').num(in.m[i].cid).ch(':').hex(in.m[i].bd);
    l
         .add(" | stack ").num(depth).ch(' ').add(top ? (g.is(top, "UIBattleMenuSelector") ? "menu" : g.is(top, "UIBattleOperationSelector") ? "orders" : "other") : "-");
        if (top && g.is(top, "UIBattleOperationSelector")) l.add(" cur ").num(g.i32(top + kLsIndex));   // 0.9.82
        if (top && g.is(top, "UIBattleMenuSelector")) l.add(" cur ").num(g.i32(top + kLsIndex));
        l.add(" | btn ").num(g_btn_fix);   // 0.9.115 button icons placed from the 1.0.1 sprite asset
        l.add(" | shape ").num(g_shape_found).ch(' ').add(g_shape_list);   // 0.9.113/114 the last search
        l.add(" | font ").num(g_bn_font).ch('/').num(g_ng_err).ch(' ').num(g_nf_names).ch('/').num(g_nf_tables).ch('/').num(g_nf_found).ch(' ').num(g_na_names).ch('/').num(g_na_found).ch(' ').num(g_na_blob).ch('/').num(g_na_gobs);   // 0.9.101 the gauge's number font (1 drawn; error)
        l.add(" | bonus ").num(in.bn_pt).ch('/').num(in.bn_next).ch(' ').num(in.bn_ratio).add(" lv ").num(in.bn_level).ch('/').num(in.bn_rows);   // 0.9.89
        for (int k = 0; k < in.bn_n; ++k) l.add(" '").add(in.bn_text[k]).ch('\'');   // 0.9.90
        l.add(" | order ").num(in.order);      // 0.9.81 the Y order and the keys its four words were found under
        for (int i = 0; i < 4; ++i) l.ch(' ').add(in.words.found[kOrdWord0 + i] ? in.words.key[kOrdWord0 + i] : "-");
        l.add(" | assault last ").num(in.as_last);
        for (int i = 0; i < in.as_n; ++i) {
            l.ch(' ').num(i).ch(':').num(in.as_cid[i]).ch('/').num(in.as_state[i]).ch('/').num(in.as_pic[i]).ch(' ')
             .num(static_cast<int>(in.as_t[i] * 10)).ch('/').num(static_cast<int>(in.as_base[i] * 10)).ch(' ').add(in.as_model[i]);
        }
        l.add(" | steps ").num(g_bmac.kind).ch(':').num(g_bmac.state).ch('/').num(g_bmac.result).ch('/').num(g_bmac.why);
}

void QuickStep(Instance& in, const EdenDsmodHostApi* h) {
    QuickMenu& q = g_qm;
    const int64_t flag = h->get_i64 ? h->get_i64(h->userdata, "@flag:so2r.qp", -1) : -1;
    if (q.fire >= 0 && flag != q.ack) { q.fire = -1; q.at = in.samples; }        // pressed: exactly once
    if (q.fire >= 0 && in.samples - q.at > 120) { q.fire = -1; q.phase = 0; }  // never pressed (2 s): give up
    if (q.fire < 0 && g_lm.state == 0 && g_mm.state == 0 && g_bmac.state == 0) {     // 0.9.70 the Battle page's single presses
        const int b = __atomic_exchange_n(&g_btn_req, -1, __ATOMIC_ACQ_REL);
        if (b >= 0) { q.fire = b; q.ack = flag; q.at = in.samples; q.phase = 0; q.want = -1; return; }
    }
    if (LeadStep(in, h, q) || MisStep(in, h, q) || BatStep(in, h, q)) { __atomic_exchange_n(&g_qtap, -1, __ATOMIC_ACQ_REL); return; }   // 0.9.60 / 0.9.64
    const int tap = __atomic_exchange_n(&g_qtap, -1, __ATOMIC_ACQ_REL);
    const int open = in.win_n;     // -1: unknown (then a tap just presses its button)
    if (tap >= 0 && tap <= kQuickPa) {
        // 0.9.25 Switch PA (Y) opens no window: a window open is closed first, then Y; never "its own menu"
        if (open <= 0) { q.phase = 2; q.want = tap; }
        else { q.phase = 1; q.want = q.last == tap && tap != kQuickPa ? -1 : tap; q.presses = 0; }
    }
    if (q.fire >= 0) return;                                       // waiting for this press
    if (in.samples - q.at < 24) return;                            // let the game react (0.4 s)
    if (q.phase == 1) {
        if (open <= 0 || q.presses >= 4) { q.phase = q.want >= 0 ? 2 : 0; if (q.phase == 0) q.last = -1; }
        else { q.fire = kQuickB; ++q.presses; }
    }
    if (q.fire < 0 && q.phase == 2 && q.want >= 0) {
        q.fire = q.want; q.last = q.want == kQuickPa ? -1 : q.want; q.phase = 0; q.want = -1;
    }
    if (q.fire >= 0) { q.ack = flag; q.at = in.samples; }
}

void Sample(void* p, const EdenDsmodHostApi* host) {
    Instance& in = *static_cast<Instance*>(p);
    if (in.main_base != host->main_base) {   // a new boot of the game: start over
        memset(&in, 0, sizeof in);
        in.main_base = host->main_base;
        in.why = "waiting for the game";
        in.map.why = "waiting for the game";
    }
    if (in.samples % kReadEvery == 0) ReadParty(in, host);
    if (in.samples % kMapEvery == 0) ReadMap(in, host);
    ReadWindows(in, host);
    ReadBattle(in, host);      // 0.9.72
    HideHudStep(in, host);     // 0.9.142 (defined below)
    QuickStep(in, host);
    ++in.samples;
    Publish(in, host);
}

// ---------------------------------------------------------------- 0.9.142 HideHUD (r77 / r78; the player's spec, built
// the Dave the Diver way: its HideHudStep / stub.S). An optional switch (the Guide Map sidebar, off at every launch): the
// top screen's chosen HUD pieces are scaled to 0 by the guest helper (tools/so2r/hidehud/stub.S, run every frame from
// CriMonoBehaviourManager.Update by the load plan) -- their pictures, their logic and the game's own show / hide are
// untouched, so the companion's pictures (read from the same sheets) stay as they are. The pieces: UIFieldController
// (GameUIManager controller 1) and UIBattleController (controller 2) fields. Mailbox: +0 magic 'SO2H', +4 heartbeat,
// entries of 0x30 bytes from +0x100: +0 component, +8 its class, +0x10 mode (0 skip, 1 read the scale, 2 set it),
// +0x14 frames done, +0x18.. scale read, +0x24.. scale set.
EdenDsmodHostExtensions g_hx; bool g_hx_ok;
constexpr uint32_t kHhMagic = 0x48324F53;   // "SO2H"
struct HhPiece { int ctl; uint32_t off; const char* cls; uint32_t off2 = 0; const char* cls2 = nullptr; };
constexpr int kHhN = 15;
const HhPiece kHh[kHhN] = {
    {1, 0x140, "UIFieldOperationPresenter"}, {1, 0x148, "UIFieldLabelOperationPresenter"},   // the shortcut list
    {1, 0x108, "UIMinimapPresenter"}, {1, 0x110, "UIWorldMinimapPresenter"},                // the minimaps
    {1, 0x118, "UIFieldSymbolNamePresenter"}, {1, 0x120, "UIFieldSubSymbolNamePresenter"},  // the place names
    {1, 0x130, "UIFieldIconSelector"}, {1, 0x128, "UIFieldModePresenter"},                  // the field icons, mode
    {2, 0xB0, "UIBattleStatusSelector"}, {2, 0x120, "UIBattlePlayerStatusPresenter"},       // the party's status
    {2, 0x118, "UIBattleOperationPresenter"}, {2, 0x128, "UIBattleSphereBonusPresenter"},   // button guide, bonus
    {2, 0xF8, "UIBattleAssistPresenter"}, {2, 0x110, "UIBattleItemCoolTimePresenter"},      // assist, item cool time
    // 0.9.143 (r79) the field's left list (Guide Map ... Current Location): UIFieldController.ShowHelp fills the
    // footer's vertical list -- UIFooterController (controller 4) .footerSelector .verticalFooterPresenter
    {4, 0xA8, "UIFooterSelector", 0xA0, "UIFooterVerticalPresenter"}};
struct HhState { uint64_t obj; float orig[3]; int have_orig; uint32_t frames_at; int phase; };   // 0 idle 1 read 2 hidden 3 restore
HhState g_hh[kHhN];
int g_hh_on;                  // the switch (off at launch)
uint32_t g_hh_hb_last; uint64_t g_hh_hb_at; bool g_hh_hb_seen;
char g_hh_diag[256];
bool MbLoad(uint32_t off, uint32_t* v) { return g_hx_ok && g_hx.load_u32(g_hx.userdata, off, v) == EDEN_DSMOD_TRUE; }
bool MbStore(uint32_t off, uint32_t v) { return g_hx_ok && g_hx.store_u32(g_hx.userdata, off, v) == EDEN_DSMOD_TRUE; }
bool MbStore64(uint32_t off, uint64_t v) { return g_hx_ok && g_hx.store_u64(g_hx.userdata, off, v) == EDEN_DSMOD_TRUE; }
float HhF(uint32_t off) { uint32_t v = 0; MbLoad(off, &v); float f; memcpy(&f, &v, 4); return f; }
void HhSetF(uint32_t off, float f) { uint32_t v; memcpy(&v, &f, 4); MbStore(off, v); }
bool HhLive(const Instance& in) { return g_hh_hb_seen && in.samples - g_hh_hb_at < 30; }
void HideHudStep(const Instance& in, const EdenDsmodHostApi* host) {
    Guest g{host};
    uint32_t hb = 0;
    if (MbLoad(4, &hb) && hb != g_hh_hb_last) { g_hh_hb_last = hb; g_hh_hb_at = in.samples; g_hh_hb_seen = true; }
    bool busy = g_hh_on != 0;
    for (const auto& h : g_hh) busy = busy || h.phase != 0;
    if (busy) MbStore(0, kHhMagic);
    const uint64_t gui = g_hx_ok ? Singleton(in, g, kGameUiTypeInfo) : 0;
    const uint64_t dict = gui ? g.ptr(gui + kGuiControllers) : 0;
    const uint64_t ctl[5] = {0, dict ? DictInt(g, dict, 1) : 0, dict ? DictInt(g, dict, 2) : 0, 0,
                             dict ? DictInt(g, dict, 4) : 0};
    Str d;
    d.add("hide hud ").add(g_hh_on ? "ON" : "off").add(!g_hx_ok ? " | no mailbox" : HhLive(in) ? " | helper live" : " | helper not running")
        .add(" | field ").add(ctl[1] && g.is(ctl[1], "UIFieldController") ? "" : "- ");
    for (int i = 0; i < kHhN; ++i) {
        if (i == 8) d.add(" battle ").add(ctl[2] && g.is(ctl[2], "UIBattleController") ? "" : "- ");
        if (i == 14) d.add(" list ");
        HhState& h = g_hh[i];
        const uint32_t e = 0x100 + 0x30 * static_cast<uint32_t>(i);
        const uint64_t c = ctl[kHh[i].ctl];
        const char* cn = kHh[i].ctl == 1 ? "UIFieldController" : kHh[i].ctl == 2 ? "UIBattleController" : "UIFooterController";
        uint64_t o = c && g.is(c, cn) ? g.ptr(c + kHh[i].off) : 0;
        if (o && kHh[i].cls2) o = g.is(o, kHh[i].cls) ? g.ptr(o + kHh[i].off2) : 0;   // 0.9.143 one more step
        const bool alive = o && g.is(o, kHh[i].cls2 ? kHh[i].cls2 : kHh[i].cls) && g.ptr(o + 0x10);
        uint32_t frames = 0; MbLoad(e + 0x14, &frames);
        if (!alive) {   // gone (another scene): stop using the entry; a new object starts over
            if (h.obj) { MbStore(e + 0x10, 0); h = HhState{}; }
            d.ch('-'); continue;
        }
        if (h.obj != o) {   // a new object: forget the old one's scale
            MbStore(e + 0x10, 0); h = HhState{}; h.obj = o;
            MbStore64(e, o); MbStore64(e + 8, g.ptr(o));
        }
        if (g_hh_on) {
            if (!h.have_orig) {
                if (h.phase != 1) { h.phase = 1; h.frames_at = frames; MbStore(e + 0x10, 1); }
                else if (frames != h.frames_at) {   // the helper read it: keep it (caught mid-animation at ~0 -> 1)
                    for (int k = 0; k < 3; ++k) {
                        const float v = HhF(e + 0x18 + 4 * static_cast<uint32_t>(k));
                        h.orig[k] = (v > 0.01f && v < 100.0f) ? v : 1.0f;
                    }
                    h.have_orig = 1;
                }
            }
            if (h.have_orig && h.phase != 2) {
                for (int k = 0; k < 3; ++k) HhSetF(e + 0x24 + 4 * static_cast<uint32_t>(k), 0.0f);
                MbStore(e + 0x10, 2); h.phase = 2;
            }
        } else if (h.phase == 2 || h.phase == 3) {   // switched off: put the scale back, then leave it alone
            if (h.phase == 2) {
                for (int k = 0; k < 3; ++k) HhSetF(e + 0x24 + 4 * static_cast<uint32_t>(k), h.have_orig ? h.orig[k] : 1.0f);
                MbStore(e + 0x10, 2); h.phase = 3; h.frames_at = frames;
            } else if (frames - h.frames_at >= 3) {
                MbStore(e + 0x10, 0); h.phase = 0; h.have_orig = 0;
            }
        } else if (h.phase == 1) {
            MbStore(e + 0x10, 0); h.phase = 0;
        }
        d.ch(h.phase == 2 ? 'H' : h.phase == 1 ? 'r' : h.phase == 3 ? 'R' : 's');
    }
    d.add(" | hb ").num(static_cast<int64_t>(g_hh_hb_last));
    size_t n = static_cast<size_t>(d.n); if (n >= sizeof g_hh_diag) n = sizeof g_hh_diag - 1;
    memcpy(g_hh_diag, d.s, n); g_hh_diag[n] = 0;
}

// 1.0.0 Release on Destroy (package reload, companion off): put every scaled piece back through the mailbox and switch the
// switch off. The helper has no teardown of its own and cannot be waited for here, so a piece that was hidden is handed
// its own original scale in mode 2: the helper then re-applies that scale each frame (harmless) until a new module
// instance (which starts every entry over) or the game's scene change replaces the object. Pieces still being read are
// set to mode 0. While the second screen is hidden the module declares NO_TICK_WHEN_HIDDEN, so it is not sampled and a
// switch that is on stays on (the HUD stays hidden) until the screen is shown again or the module is destroyed.
void HideHudRelease() {
    bool any = false;
    for (int i = 0; i < kHhN; ++i) {
        const uint32_t e = 0x100 + 0x30 * static_cast<uint32_t>(i);
        HhState& h = g_hh[i];
        if (h.phase == 2 || h.phase == 3) {
            for (int k = 0; k < 3; ++k) HhSetF(e + 0x24 + 4 * static_cast<uint32_t>(k), h.have_orig ? h.orig[k] : 1.0f);
            MbStore(e + 0x10, 2); any = true;
        } else if (h.phase == 1) {
            MbStore(e + 0x10, 0);
        }
        h = HhState{};
    }
    if (any) MbStore(0, kHhMagic);
    g_hh_on = 0;
}

void Tick(void*, const EdenDsmodHostApi*) {}
void Configure(void*, const EdenDsmodHostExtensions* x) {
    // the host's ASTC decoder (the character illustrations are ASTC 4x4)
    if (x && x->decode_astc) { g_astc_user = x->userdata; __atomic_store_n(&g_astc, x->decode_astc, __ATOMIC_RELEASE); }
    if (x && x->load_u32 && x->store_u32 && x->store_u64) { g_hx = *x; g_hx_ok = true; }   // 0.9.142 HideHUD's mailbox
}
// 0.9.121 the Battle page's buttons light up when tapped (the main tabs' blue, list_item_selected): the button's number
// (0 A, 1 B, 2 L, 3 R, 4..7 Items / Spells / Strategy / Escape, 8 Y) + 1, lit for kLitSamples samples (0.3 s)
void Light(int k) { __atomic_store_n(&g_lit_req, k + 1, __ATOMIC_RELEASE); }
EdenDsmodBool OnAction(void*, const char* action, int64_t) {
    if (action && eq(action, "so2r.hud_hide")) {     // 0.9.142 the HideHUD switch
        g_hh_on = g_hh_on ? 0 : 1;
        return EDEN_DSMOD_TRUE;
    }
    if (action && eq(action, "so2r.spage")) {       // Status: switch pages (like the game's Y)
        __atomic_store_n(&g_spage, __atomic_load_n(&g_spage, __ATOMIC_RELAXED) ? 0 : 1, __ATOMIC_RELAXED);
        return EDEN_DSMOD_TRUE;
    }
    if (action && eq(action, "so2r.kpage")) {       // Skills: switch pages
        __atomic_store_n(&g_kpage, (__atomic_load_n(&g_kpage, __ATOMIC_RELAXED) + 1) % 3, __ATOMIC_RELAXED);
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.open") && action[9] >= '0' && action[9] <= '7' && !action[10]) {
        __atomic_store_n(&g_sel, action[9] - '0', __ATOMIC_RELAXED);   // a roster row: that member's details
        __atomic_store_n(&g_view, 1, __ATOMIC_RELAXED);
        return EDEN_DSMOD_TRUE;
    }
    if (action && eq(action, "so2r.roster")) {   // 0.9.28 the Party tab: the roster (from any page: goparty flips)
        __atomic_store_n(&g_view, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_goparty, __atomic_load_n(&g_goparty, __ATOMIC_RELAXED) ? 0 : 1, __ATOMIC_RELAXED);
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.mrow")) {      // 0.9.64 a Missions page row: open it in the game
        int k = 0; const char* d = action + 9;
        if (!*d) return EDEN_DSMOD_TRUE;
        for (; *d >= '0' && *d <= '9'; ++d) k = k * 10 + (*d - '0');
        if (!*d && k < 80) __atomic_store_n(&g_mis_req, k, __ATOMIC_RELEASE);
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.kimp") && action[9] >= '0' && action[9] <= '2' && !action[10]) {   // 0.9.122
        Light(9 + (action[9] - '0'));      // 0.9.124 lights up like the Battle page's buttons
        // 0.9.133 and on the companion's chosen member: R that many times once there
        int sel = __atomic_load_n(&g_sel, __ATOMIC_RELAXED);
        if (sel < 0 || sel > 15) sel = 0;
        __atomic_store_n(&g_imp_req, 1 + (action[9] - '0') + 3 * sel, __ATOMIC_RELEASE);
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.keq") && action[8] >= '0' && action[8] <= '6' && !action[9]) {   // 0.9.135
        Light(12 + (action[8] - '0'));                   // an Equipment card: the game's Equipment on it
        int sel = __atomic_load_n(&g_sel, __ATOMIC_RELAXED);
        if (sel < 0 || sel > 15) sel = 0;
        __atomic_store_n(&g_eq_req, 1 + (action[8] - '0') + 8 * sel, __ATOMIC_RELEASE);
        return EDEN_DSMOD_TRUE;
    }
    if (action && (eq(action, "so2r.kart0") || eq(action, "so2r.kart1"))) {   // 0.9.133 the arts subtab's L / R:
        Light(action[9] == '0' ? 2 : 3);                 // the game's Special Arts/Spells on the chosen member
        int sel = __atomic_load_n(&g_sel, __ATOMIC_RELAXED);
        if (sel < 0 || sel > 15) sel = 0;
        __atomic_store_n(&g_art_req, 1 + sel, __ATOMIC_RELEASE);
        return EDEN_DSMOD_TRUE;
    }
    if (action && (eq(action, "so2r.bart0") || eq(action, "so2r.bart1"))) {   // 0.9.70 the Battle page's L / R
        Light(action[9] == '0' ? 2 : 3);
        if (g_inst.scene == 6) __atomic_store_n(&g_btn_req, action[9] == '0' ? 8 : 9, __ATOMIC_RELEASE);
        else __atomic_store_n(&g_art_req, 1, __ATOMIC_RELEASE);
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.bcmd")) {      // 0.9.72 a battle command: the game's battle menu, its row
        if (g_inst.scene == 6 && action[9] >= '0' && action[9] <= '3' && !action[10]) {
            __atomic_store_n(&g_bcmd_req, action[9] - '0', __ATOMIC_RELEASE);
            Light(4 + (action[9] - '0'));
        }
        return EDEN_DSMOD_TRUE;
    }
    if (action && eq(action, "so2r.title_a")) {      // 0.9.152 the title page's button: A, as Dave the Diver's title page
        if (g_inst.scene != 6) { __atomic_store_n(&g_btn_req, 7, __ATOMIC_RELEASE); Light(19); }
        return EDEN_DSMOD_TRUE;
    }
    if (action && (eq(action, "so2r.ba") || eq(action, "so2r.bb"))) {   // 0.9.95 the normal attack (A) / counter (B); 0.9.107 back
        if (g_inst.scene == 6) { __atomic_store_n(&g_btn_req, action[6] == 'a' ? 7 : 6, __ATOMIC_RELEASE); Light(action[6] == 'a' ? 0 : 1); }
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.bas") && action[8] >= '0' && action[8] <= '3' && !action[9]) {   // 0.9.73
        if (g_inst.scene == 6) __atomic_store_n(&g_as_req, action[8] - '0', __ATOMIC_RELEASE);
        return EDEN_DSMOD_TRUE;
    }
    if (action && eq(action, "so2r.by")) {            // 0.9.72 the battle's Y (its orders); 0.9.83 presses Y (the game's
        if (g_inst.scene == 6) {                      // list opens) and shows the four orders on the Battle page; again:
            Light(8);
            const bool on = __atomic_load_n(&g_opick, __ATOMIC_RELAXED) || g_inst.op_open;   // hides them (B closes
            __atomic_store_n(&g_opick, on ? 0 : 1, __ATOMIC_RELEASE);                         // the game's list)
            if (!on) __atomic_store_n(&g_btn_req, 5, __ATOMIC_RELEASE);
            else if (g_inst.op_open) __atomic_store_n(&g_btn_req, 6, __ATOMIC_RELEASE);
        }
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.bsw") && action[8] >= '0' && action[8] <= '2' && !action[9]) {   // 0.9.86
        const int cid = __atomic_load_n(&g_bo_cid[action[8] - '0'], __ATOMIC_RELAXED);
        if (g_inst.scene == 6 && cid > 0) __atomic_store_n(&g_bsw_req, cid, __ATOMIC_RELEASE);
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.bord") && action[9] >= '1' && action[9] <= '4' && !action[10]) {   // 0.9.82
        if (g_inst.scene == 6) __atomic_store_n(&g_bord_req, action[9] - '0', __ATOMIC_RELEASE);
        __atomic_store_n(&g_opick, 0, __ATOMIC_RELEASE);
        return EDEN_DSMOD_TRUE;
    }
    if (action && eq(action, "so2r.lead")) {          // 0.9.60 Party Formation: the chosen member as leader
        const Instance& in = g_inst;
        int sel = __atomic_load_n(&g_sel, __ATOMIC_RELAXED);
        if (sel >= 0 && sel < in.rows && in.m[sel].on && in.m[sel].battle && !in.m[sel].leader && !in.m[sel].guest)
            __atomic_store_n(&g_lead_req, in.m[sel].pid, __ATOMIC_RELEASE);
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.sub") && action[8] >= '0' && action[8] <= '3' && !action[9]) {
        __atomic_store_n(&g_sub, action[8] - '0', __ATOMIC_RELAXED);
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.msec") && action[9] >= '0' && action[9] <= '1' && !action[10]) {
        __atomic_store_n(&g_msec, action[9] - '0', __ATOMIC_RELAXED);
        __atomic_store_n(&g_mpage, 0, __ATOMIC_RELAXED);
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.mtab") && action[9] >= '0' && action[9] <= '3' && !action[10]) {
        __atomic_store_n(&g_mtab, action[9] - '0', __ATOMIC_RELAXED);
        __atomic_store_n(&g_mpage, 0, __ATOMIC_RELAXED);
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.pick") && action[9] >= '0' && action[9] <= '7' && !action[10]) {
        __atomic_store_n(&g_sel, action[9] - '0', __ATOMIC_RELAXED);   // the Status page's member
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.qtap") && action[9] >= '0' && action[9] <= '5' && !action[10]) {
        __atomic_store_n(&g_qtap, action[9] - '0', __ATOMIC_RELEASE);   // 0.9.19 a quick menu button
        return EDEN_DSMOD_TRUE;
    }
    if (action && eq(action, "so2r.side")) {           // 0.9.17 the Map page's info sidebar: collapse / show
        __atomic_store_n(&g_side, __atomic_load_n(&g_side, __ATOMIC_RELAXED) ? 0 : 1, __ATOMIC_RELAXED);
        return EDEN_DSMOD_TRUE;
    }
    if (action && starts(action, "so2r.bopen") && action[10] >= '0' && action[10] <= '3' && !action[11]) {
        // 0.9.17 a battle member on the Map page's strip: their Party details (the row is published per slot)
        const int row = __atomic_load_n(&g_brow[action[10] - '0'], __ATOMIC_RELAXED);
        if (row < 0) return EDEN_DSMOD_TRUE;
        __atomic_store_n(&g_sel, row, __ATOMIC_RELAXED);
        __atomic_store_n(&g_view, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_goparty, __atomic_load_n(&g_goparty, __ATOMIC_RELAXED) ? 0 : 1, __ATOMIC_RELAXED);
        return EDEN_DSMOD_TRUE;
    }
    if (action && eq(action, "so2r.map_whole")) { __atomic_store_n(&g_map_whole, 1, __ATOMIC_RELAXED); return EDEN_DSMOD_TRUE; }
    if (action && eq(action, "so2r.map_mini")) { __atomic_store_n(&g_map_whole, 0, __ATOMIC_RELAXED); return EDEN_DSMOD_TRUE; }
    if (action && eq(action, "so2r.map_toggle")) {      // switches the view of the kind of map shown
        int* sw = __atomic_load_n(&g_view_kind, __ATOMIC_RELAXED) == 2 ? &g_map_whole : &g_town_zoom;
        __atomic_store_n(sw, __atomic_load_n(sw, __ATOMIC_RELAXED) ? 0 : 1, __ATOMIC_RELAXED);
        return EDEN_DSMOD_TRUE;
    }
    return EDEN_DSMOD_FALSE;
}

const EdenDsmodModuleExtensions kExtensions = {
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    Configure, OnAction, LoadImage,
};
const EdenDsmodModuleDataExtensions kDataExtensions = {
    EDEN_DSMOD_DATA_EXT_VERSION, sizeof(EdenDsmodModuleDataExtensions), EDEN_DSMOD_DATA_EXT_HASH, LoadData,
};
const EdenDsmodFontExtensions kFontExtensions = {
    EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions), EDEN_DSMOD_FONT_EXT_HASH, DecodeFont,
};

const EdenDsmodModuleApi kModule = {
    EDEN_DSMOD_MODULE_ABI_VERSION,
    sizeof(EdenDsmodModuleApi),
    0,
    EDEN_DSMOD_MODULE_ABI_HASH,
    kTitle,
    SO2R_DIAG ? "SO2R (diag)" : "SO2R",
    EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN | EDEN_DSMOD_CAP_EXTENSIONS,
    SupportsBuild,
    Create,
    Destroy,
    Sample,
    Tick,
};

}  // namespace

EXPORT const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t host_abi_version, uint64_t host_abi_hash) {
    if (host_abi_version != EDEN_DSMOD_MODULE_ABI_VERSION || host_abi_hash != EDEN_DSMOD_MODULE_ABI_HASH) return nullptr;
    return &kModule;
}
EXPORT const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t version, uint64_t abi_hash) {
    if (version != EDEN_DSMOD_EXT_VERSION || abi_hash != EDEN_DSMOD_EXT_HASH) return nullptr;
    return &kExtensions;
}
EXPORT const EdenDsmodModuleDataExtensions* eden_dsmod_get_data_extensions(uint32_t version, uint64_t abi_hash) {
    if (version != EDEN_DSMOD_DATA_EXT_VERSION || abi_hash != EDEN_DSMOD_DATA_EXT_HASH) return nullptr;
    return &kDataExtensions;
}
EXPORT const EdenDsmodFontExtensions* eden_dsmod_get_font_extensions(uint32_t version, uint64_t abi_hash) {
    if (version != EDEN_DSMOD_FONT_EXT_VERSION || abi_hash != EDEN_DSMOD_FONT_EXT_HASH) return nullptr;
    return &kFontExtensions;
}
