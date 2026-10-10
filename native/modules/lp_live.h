// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Live reader for Pokémon Brilliant Diamond 1.3.0 (main 94CEAE325C205C4B…), with or without the
// Luminescent Platinum 2.2F mod (its IPS keeps the build id). All offsets come from the game's
// IL2CPP layout (Luminescent's generated il2cpp.h) and were verified live on desktop; see the
// private research contract live-readers-bd130.md.
//
// Static roots are main-relative IL2CPP .bss slots, filled by the game at runtime. Under NCE the
// writable segments sit at a constant offset from their Dynarmic position, so the reader learns
// that delta from RELATIVE relocations in .data (each slot holds main + a known .text offset)
// instead of trusting main + offset.

#pragma once

#include "lp_pk8.h"
#include "lp_profile.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lp_live {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct BatchOp {
    u64 addr;
    std::vector<u8> expect, value;
};

struct Guest {
    std::function<bool(u64 addr, void* out, std::size_t size)> read;
    // all-or-nothing store with expect-bytes checks (host write_batch); null = unavailable
    std::function<bool(const std::vector<BatchOp>&)> batch;
    std::function<bool(u64 addr, const void* in, std::size_t size)> write;
    u64 main_base = 0;
};

// ---- offsets (BD 1.3.0) -------------------------------------------------------------------------
namespace off {
constexpr u64 EntityManagerTypeInfo = lp_profile::Active.EntityManagerTypeInfo;
// EvDataManager._instanse (static +0) -> +0x568 List<FieldObjectEntity>; entity +0xD0 EventParams
// (+0x20 VanishFlagIndex, +0x50 Dowsing), entity +0x28 / +0x30 world x / z (contracts/poketch-dowsing.md).
constexpr u64 EvDataManagerTypeInfo = lp_profile::Active.EvDataManagerTypeInfo, EvFieldObjects = 0x568, EntityParams = 0xD0;
constexpr u64 ParamVanish = 0x20, ParamDowsing = 0x50, EntityWorldX = 0x28, EntityWorldZ = 0x30;
constexpr u64 PlayerWorkTypeInfo = lp_profile::Active.PlayerWorkTypeInfo;        // PlayerWork_TypeInfo slot
constexpr u64 FieldManagerTypeInfo = lp_profile::Active.FieldManagerTypeInfo;      // FieldManager_TypeInfo slot (statics +0 = _Instance)
constexpr u64 BattleViewCoreGetInstance = lp_profile::Active.BattleViewCoreGetInstance; // MethodInfo slot of SingletonMonoBehaviour<BattleViewCore>.get_Instance
// .data RELATIVE relocations: slot -> main + target (from main's .rela.dyn), used to learn the
// NCE delta and to prove the image is the expected one.
using Reloc = lp_profile::Reloc;
constexpr auto Fingerprints = lp_profile::Active.Fingerprints;
// managed object fields
constexpr u64 KlassStatics = 0xB8;
constexpr u64 MethodKlass = 0x18;
constexpr u64 PlayerWorkInstance = 0x10; // static field holding the PlayerWork instance
constexpr u64 PwRomCode = 0xF4; // MYSTATUS.rom_code: native cassetVersion (0 Diamond, nonzero Pearl)
constexpr u64 PwRivalName = 0x38; // native PlayerWork.get_rivalName, both BD/SP1.3.0
constexpr u64 PwUserName = 0xE0; // native PlayerWork.get_userName
constexpr u64 PwPlayerSex = 0xF0; // PlayerWork.get_playerSex: true Lucas, false Dawn
constexpr u64 PwMoney = 0xEC, PwBodyType = 0xF6, PwFashion = 0xF7; // MYSTATUS gold / body_type / fashion
// CONFIG (SaveData +0x90 -> PlayerWork +0xA8): msg_lang_id (MessageEnumData.MsgLangId, int32: 1 JPN,
// 2 USA, 3 FRA, 4 ITA, 5 DEU, 7 ESP, 8 KOR, 9 SCH, 10 TCH) and is_kanji. PlayerWork$$get_msgLangID
// is ldr w0, [PlayerWork, #0xac]. Save file offset 0x79B78 (contracts/language-mods.md §7).
constexpr u64 PwMsgLangId = 0xAC, PwIsKanji = 0xB0;
constexpr u64 PwTownMapLocation = 0x300;
// Verified BD 1.3.0 save offsets; older generated headers place Poketch 0x10 too late.
constexpr u64 PoketchWindowTypeInfo = lp_profile::Active.PoketchWindowTypeInfo;
constexpr u64 PwTopMenu = 0x240, PwEventFlags = 0x28;
constexpr u64 FlagPokedex = 105, FlagTownMap = 201, FlagPoketch = 203;
// DPData.POKETCH_DATA inline at 0x410: color_type +4, app_index +9, app_flag +0x10, pedometer
// +0x18, dotart_data +0x20, calendar_markbit (u32 per month) +0x28.
constexpr u64 PwPoketchColour = 0x414, PwPoketchFlags = 0x420, PwPedometer = 0x428;
constexpr u64 PwPoketchCalendar = 0x438;
// contracts/poketch-apps.md (PlayerWork = 0x18 + SaveData offset)
constexpr u64 PwWorks = 0x20, PwSysFlags = 0x30, PwRoamers = 0x2A0, PwDotArtModified = 0x412,
              PwDotArt = 0x430, PwMarkMap = 0x440, PwHistory = 0x448, PwDayCare = 0x450,
              PwEggExists = 0x458, PwChainRanking = 0x620;
constexpr u64 SwayGrassTypeInfo = lp_profile::Active.SwayGrassTypeInfo, SwayChainCount = 0x20, SwayChainMons = 0x24;
// enc_sv_data (contracts/field-item-move-uses.md 2.2): repel units left, repel type (1 Repel,
// 2 Super, 3 Max), Vs. Seeker and Poké Radar charge
constexpr u64 PwSprayCount = 0x2AA, PwSprayType = 0x2AC, PwSeekerCharge = 0x2AD, PwRadarCharge = 0x2AE;
constexpr u64 PwBattlePoints = 0x384; // SaveData.btlTowerSave (+0x360) .btl_point (+0xC): BtlTowerWork.GetBP
constexpr u64 PwShortcut = 0x58;
// ItemWork._instance (static +0): +0x18 List<ItemInfo>[] per pocket (CategoryType = fld_pocket),
// +0x20 ItemListMemory[] per UIBag.BootType (0 = the X-menu Bag): +0x10 CategoryIndex (pocket BUTTON
// index; Key Items pocket 8 is button 7), +0x18 int[] Indexes, +0x20 float[] ScrollPositions.
constexpr u64 ItemWorkTypeInfo = lp_profile::Active.ItemWorkTypeInfo, IwCategorized = 0x18, IwListMemories = 0x20;
constexpr u64 MemCategory = 0x10, MemIndexes = 0x18, MemScroll = 0x20; // saveItemShortcut ushort[] (the Y registered items in LP)
constexpr u64 PwZukan = 0x130; // inline SaveData.zukanData.get_status
constexpr u64 PwSaveItem = 0x48;   // SaveData.saveItem: SaveItem[item no] (Count i32 first, 0xC each)
// BD1.3.0: native battle lifecycle flag, followed by _battleSetupParam at +0x800.
constexpr u64 PwIsBattling = 0x7F0;
constexpr u64 SaveItemSize = 0xC;
constexpr u64 ListItems = 0x10, ListSize = 0x18;
constexpr u64 ItemInfoWorkNo = 0x10; // Dpr.Item.ItemInfo._workNo (u16): the item number
constexpr u64 UiDecoImage = 0x18, GraphicColorA = 0x2C;
constexpr u64 PwParty = 0x808;
// FieldManager: exists from the loading screen on; _initFlag once the field runs
constexpr u64 FmZone = 0x50, FmMenuOpen = 0x120, FmInit = 0x144;
constexpr u64 PartyMembers = 0x10, PartyCount = 0x18;
constexpr u64 ArrayData = 0x20, ArrayLength = 0x18;
constexpr u64 ParamCore = 0x10, ParamCalc = 0x18;
// battle view
constexpr u64 BvcViewSystem = 0x28, BvcUiSystem = 0x30;
constexpr u64 UiActionList = 0x38, UiWazaList = 0x40, UiPokeBallList = 0x70;
// BD/SP 1.3.0 IL2CPP: battle message window and its input model.
constexpr u64 UiMessageOpen = 0xB2, UiMessageSleep = 0xB1, UiMessageWindow = 0xD8;
constexpr u64 UiMessageKeyWait = 0xBC, UiMessageQueue = 0xC8;
constexpr u64 MessageModel = 0x40, MessageAuto = 0x69;
constexpr u64 MessageState = 0x40, MessageInput = 0x64, MessageParam = 0x10;
constexpr u64 MessageCloseInput = 0x39;
constexpr u64 ActionMinIndex = 0xA4, ActionMaxIndex = 0xA8; // native visible command bounds
constexpr u64 ActionBallEnable = 0xA0; // BUIActionList._isBallEnable: 0 in trainer battles
constexpr u64 CanvasTransitionType = 0x18, CanvasHideAnchor = 0x1C, CanvasShowAnchor = 0x24;
constexpr u64 CanvasGroup = 0x40, NativePtr = 0x10, NativeCanvasGroupAlpha = 0x3C;
constexpr u64 CanvasMaxIndex = 0x58, CanvasCurrentIndex = 0x5C, CanvasIsFocus = 0x60, CanvasIsShow = 0x61;
constexpr u64 BallListBalls = 0xA8;
constexpr u64 ViewSystemEnv = 0x20, EnvPokecon = 0x10, PokeconParty = 0x18;
constexpr u64 BtlPartyMembers = 0x10, BtlPartyCount = 0x18;
constexpr u64 BppCore = 0x10, BppWaza = 0x30, BppWazaCount = 0x3C;
// BTL_POKEPARAM core, read as one 0x10 block from monsno: monsno +0, form +2, hpMax +4, hp +6,
// item +8, level +0xE. A move (WAZA_CORE) is read as 8 bytes: number (i32), pp, ppMax.
constexpr u64 CoreMonsNo = 0x20;
constexpr u64 WazaSetSurface = 0x18, WazaNumber = 0x10;
// BTL_POKEPARAM battle-time state (getters disassembled, BD 1.3.0):
//   +0x18 BASE_PARAM: atk/def/spa/spd/spe u16 at +0x14..+0x1C (GetValue_Base), type1/type2/type_ex
//         u8 at +0x1E/+0x1F/+0x20 (GetPokeType; 18 = none)
//   +0x20 VARIABLE_PARAM: 7 rank bytes at +0x10 (atk, def, spa, spd, spe, accuracy, evasion),
//         0..12 with 6 neutral (effrank_Init, getRankVaryStatus)
//   +0x38 m_tokusei u16 (the ability now, ChangeTokusei); +0x60 m_contFlag byte[] (bit 23 Burn Up)
//   core +0x48 sickCont BTL_SICKCONT[] (u64 each, active when byte0 & 7; GetPokeSick 1..5)
constexpr u64 BppBase = 0x18, BppVary = 0x20, BppTokusei = 0x38, BppContFlag = 0x60;
constexpr u64 BaseStats = 0x14, BaseTypes = 0x1E, VaryRanks = 0x10, CoreSickCont = 0x48;
constexpr int SickKonran = 6, SickHaneyasume = 25, TypeNull = 18, RankNeutral = 6;
constexpr int SickMax = 45; // native WAZASICK_MAX (BtlAIBaseScript)
// BattleEnv (ViewSystem +0x20) +0x18 FieldStatus -> +0x10 Data: weather u8 +0x10, weatherTurn u32
// +0x14 (0xFF permanent), weatherTurnCount u32 +0x1C, currentGround u8 +0x21, per-EffectType
// arrays cont (BTL_SICKCONT) +0x28, turnCount (u32) +0x30, enableFlag (bool) +0x48.
// BattleEnv +0x90 BattleCounter -> +0x10 ulong[]: [0] = BATTLE_TURN_COUNT.
constexpr u64 EnvFieldStatus = 0x18, FieldStatusData = 0x10;
constexpr u64 DataWeather = 0x10, DataGround = 0x21, DataCont = 0x28, DataTurnCount = 0x30, DataEnable = 0x48;
constexpr u64 EnvCounter = 0x90, CounterValues = 0x10;
constexpr int FieldEffects = 10; // EffectType WEATHER..KAGAKUHENKAGAS
// Double battles (contracts/double-battles-bd130.md): ViewSystem +0x10 MainModule (rule u32 +0x70,
// setup param +0x10 -> multiMode u8 +0x39, my client id +0x94, my original position +0x95);
// ViewSystem +0x18 BTL_CLIENT (m_procPokeIdx u8 +0x198: your slot choosing its command);
// UISystem +0x80 BUITargetSelect (canvas fields, IsValid +0x62, _isSingleTarget +0xAC).
constexpr u64 VsMainModule = 0x10, VsClient = 0x18;
constexpr u64 MainSetup = 0x10, MainRule = 0x70, MainMyClient = 0x94, MainMyOrgPos = 0x95;
constexpr u64 SetupMultiMode = 0x39;
constexpr u64 ClientProcPokeIdx = 0x198;
constexpr u64 UiTargetSelect = 0x80, CanvasIsValid = 0x62, TargetSingle = 0xAC;
constexpr u64 CanvasIsTransition = 0x63; // BattleViewUICanvasBase._IsTransition (a show / hide slide)
constexpr int RuleDouble = 1, ClientNone = 5, ViewNone = 255;
// The game's own Bag / Pokémon windows opened from the battle menu (contracts/battle-bag-party-ui-bd130.md):
// UISystem +0xE8 _uiWindow (OpenMenuUI<T> stores the new window and clears +0xAC _isMenuUIEnd; the
// window's onClosed sets it again). The window's +0x48 UIManager.UIInstance holds _uiWindow +0x10
// (cleared when the window goes back to the pool) and _windowId +0x18 (UIWindowID: 3 BAG, 15
// POKEMON_BATTLE); UIWindow +0x60 IsClosing. UISystem +0x124 _outMemberIndex: the slot the
// Pokémon list replaces.
constexpr u64 UiMenuEnd = 0xAC, UiMenuWindow = 0xE8;
constexpr u64 WindowInstance = 0x48, WindowClosing = 0x60, InstanceWindow = 0x10, InstanceWindowId = 0x18;
constexpr int WindowIdBag = 3, WindowIdPokemonBattle = 15, WindowIdLevelUp = 12;
constexpr u64 WindowInput = 0x18, InputEnabled = 0x10;
constexpr u64 ExpStatusPanel = 0x80, ExpMessageController = 0xA8, ExpGaugeAnimating = 0xB0, ExpWaitExit = 0xB3;
constexpr u64 ExpControllerMessage = 0x10, ExpControllerWait = 0x28, ExpControllerParam = 0x38;
constexpr u64 ExpStatusAnimating = 0x28, ExpStatusShown = 0x29;
// The top screen's HP gauges (contracts/battle-hp-gauge-bd130.md): UISystem +0x20 _statusWindows
// (BUIStatusWindow[], one per view position). A window: +0xD8 DoDisplay, +0xE0 _maxHP, +0xE5 _pokeID
// (CORE_PARAM.myID), +0x108 _isInitialized, +0xA0 _hpBar (Dpr.UI.HpBar: +0x18 barSlider, +0x48 max).
// The bar tweens the slider value (0..1, Slider.m_Value +0x118) and HpBar.UpdateHp prints
// (int)(value * max + 0.5): the HP the top screen shows now.
constexpr u64 UiStatusWindows = 0x20;
constexpr u64 StatusHpBar = 0xA0, StatusDisplay = 0xD8, StatusMaxHp = 0xE0, StatusPokeId = 0xE5,
              StatusInitialized = 0x108;
constexpr u64 HpBarSlider = 0x18, HpBarMax = 0x48, SliderValue = 0x118;
constexpr u64 CoreMyId = 0x2F; // CORE_PARAM.myID (the last byte of the monsno block)
} // namespace off

// ---- double battles: who stands where (pure, from the game's tables) ----------------------------
constexpr int HiddenMoveBadge(int button, bool luminescent) {
    constexpr int Badge[8] = {124, 125, 129, 126, 127, 128, 130, 131};
    return button < 0 || button >= 8 ? -1 : Badge[luminescent && (button == 4 || button == 5) ? 9 - button : button];
}

// The client owning battle position `pos` (0..3) in a double with `multi` (BATTLE_SETUP_PARAM
// multiMode): GetPosCoverClientId_Double. 5 = nobody; -1 = a multiMode the table does not know.
constexpr int PosOwner(int multi, int pos) {
    constexpr int T[7][4] = {{0, 1, 0, 1}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3},
                             {0, 1, 0, 3}, {0, 1, 2, 5}, {0, 1, 2, 1}};
    return multi < 0 || multi > 6 || pos < 0 || pos > 3 ? -1 : T[multi][pos];
}
// The member index of `pos` in its owner's party: lower positions with the same owner
// (btlPos_to_cliendID_and_posIdx). -1 for no owner.
constexpr int PosMemberIndex(int multi, int pos) {
    const int owner = PosOwner(multi, pos);
    if (owner < 0 || owner == off::ClientNone)
        return -1;
    int n = 0;
    for (int p = 0; p < pos; ++p)
        n += PosOwner(multi, p) == owner ? 1 : 0;
    return n;
}
// Battle position -> view position (0 near 1, 1 far 1, 2 near 2, 3 far 2; 255 none) in a double:
// BtlPosToViewPos with rule_double_vpos {{1, 3}, {0, 2}} (multiMode 5: {{1, 255}, {0, 2}}).
constexpr int BtlPosToView(int multi, int my_org_pos, int pos) {
    if (pos < 0 || pos > 3)
        return off::ViewNone;
    const int enemy = (pos ^ my_org_pos) & 1, slot = pos >> 1;
    constexpr int Normal[2][2] = {{1, 3}, {0, 2}}, Single[2][2] = {{1, off::ViewNone}, {0, 2}};
    return multi == 5 ? Single[enemy ^ 1][slot] : Normal[enemy ^ 1][slot];
}

// Stat after a rank stage, as calc.StatusRank does it: raw stage 0..12 (6 neutral) picks
// {2,8-raw} below 6 and {raw-4,2} from 6 on; result = ((stat * num) & 0xFFFF) / den.
constexpr int StageStat(int stat, int raw_stage) {
    if (stat < 0 || raw_stage < 0 || raw_stage > 12)
        return -1;
    const int num = raw_stage < 6 ? 2 : raw_stage - 4;
    const int den = raw_stage < 6 ? 8 - raw_stage : 2;
    return ((stat * num) & 0xFFFF) / den;
}

// FieldStatus effect slots (EffectType)
enum FieldEffect : int { EffTrickRoom = 1, EffGravity = 2, EffWonderRoom = 4, EffGround = 8 };

struct BattleField {
    bool valid = false;            // FieldStatus.Data was read
    int weather = -1;              // BtlWeather: 0 none, 1 sun, 2 rain, 3 hail, 4 sand, 5 heavy rain, 6 harsh sun, 7 strong winds
    int weather_turns = -1;        // turns left as GetWeatherRemainingTurn: 0 none, 255 permanent
    int terrain = -1;              // BtlGround: 0 none, 1 grassy, 2 misty, 3 electric, 4 psychic
    int terrain_turns = -1;        // turns left (0 = no limit / none)
    std::array<bool, off::FieldEffects> effect_on{};  // enableFlag[EffectType]
    std::array<int, off::FieldEffects> effect_turns{}; // turns left per EffectType (0 = no limit / off)
    int64_t turn = -1;             // BATTLE_TURN_COUNT (raw; 0/1 base on the first menu not checked live)
};

struct HiddenItem {
    int grid_x = 0, grid_y = 0, dowsing = 0; // dowsing: the sonar level up to which it is pinpointed
};

struct PartyMon {
    lp_pk8::Mon mon;
    bool valid = false;
};

struct BattleMon {
    u16 species = 0, form = 0, hp = 0, hp_max = 0, item = 0;
    u8 level = 0;
    int id = -1;                       // CORE_PARAM.myID (pokeID), the key of its top-screen status window
    // the HP the top screen's gauge shows now (it drains / fills after the hit), -1 when no status
    // window shows this battler: the companion shows it so both screens move together
    u8 gender = 2; // native PB8:0 male,1 female,2 genderless or unreadable
    bool shiny = false, is_egg = false; // from CORE_PARAM.ppSrc, never inferred by species
    int hp_view = -1;
    int waza_count = 0;
    std::array<int, 4> waza{};
    std::array<int, 4> pp{}, pp_max{};
    bool valid = false;
    // battle-time state (each part independent; defaults when its pointer is null or bad)
    int ability = -1;                  // m_tokusei: the ability now (after Skill Swap etc.), -1 unknown
    bool stages_ok = false;
    std::array<int, 7> stages{};       // -6..+6: atk, def, spa, spd, spe, accuracy, evasion
    std::array<int, 5> stats{-1, -1, -1, -1, -1};  // BASE_PARAM atk, def, spa, spd, spe (before stages)
    std::array<int, 5> staged{-1, -1, -1, -1, -1}; // stats after their stage (StageStat); no Wonder Room swap
    int status = -1;                   // GetPokeSick: 0 none, 1 paralysis, 2 sleep, 3 freeze, 4 burn, 5 poison
    bool toxic = false, confused = false;
    // Native BTL_SICKCONT activation only; its encoded turn limit is not a remaining countdown.
    std::array<bool, off::SickMax> volatile_on{};
    std::array<int, 3> types_raw{-1, -1, -1}; // BASE_PARAM type1, type2, type_ex (18 = none)
    std::array<int, 2> types{-1, -1};  // as GetPokeType shows them (Roost / Burn Up); 18 = none
                                       // the third type stays types_raw[2]
};

struct CanvasState {
    u64 obj = 0;
    int current = -1, max = 0;
    bool focus = false, show = false;
    bool valid = false;      // IsValid (BUITargetSelect: a target was chosen)
    bool transition = false; // IsTransition: sliding in or out (between the lists' focus changes)
};

// One battler spot of a double, indexed by view position.
struct DoubleSlot {
    bool exists = false;    // the battle has this position (an owner and a view position)
    int client = -1, index = -1, btl_pos = -1;
    BattleMon mon;          // party[client].member[index]; invalid when missing
    bool Present() const {  // somebody is standing there (a fainted one leaves the spot empty)
        return exists && mon.valid && mon.hp > 0;
    }
};

struct DoubleState {
    bool valid = false;       // the MainModule was read and the multiMode is known
    bool is_double = false;   // rule == double
    int rule = -1, multi = -1, my_client = -1, my_org_pos = -1;
    std::array<DoubleSlot, 4> view{}; // 0 near 1, 1 far 1, 2 near 2, 3 far 2
    int cover = 0;            // positions your client commands (2 in a normal double, 1 in a multi)
    int proc_index = -1;      // m_procPokeIdx while it names one of your positions, else -1
    int proc_view = -1;       // the view position of the Pokémon choosing its command, -1 none
    bool target_open = false; // BUITargetSelect: IsShow && IsFocus && !IsValid
    bool target_moving = false; // BUITargetSelect IsTransition: sliding in (after a move) or out
    bool target_single = false; // _isSingleTarget (0 = a spread move: the cursor index is meaningless)
    std::array<bool, 4> target_enabled{}; // native BUITargetButton.IsGrayOut, view order
    int target_view = -1;     // CurrentIndex (a view position) while open, else -1
};

struct Snapshot {
    // field
    bool player_ok = false;
    u32 money = 0;
    int game_version = -1; // save version: 0 Diamond, 1 Pearl; -1 unreadable
    int msg_lang_id = 0;     // the save's text language (0 = unreadable or out of range)
    bool is_kanji = false;   // Japanese with kanji
    bool is_battling = false;
    bool field = false;      // FieldManager exists and is initialised (not title / loading)
    bool demo_active = false; // native DemoSceneManager: evolution / hatch / scripted demos
    bool menu_open = false;  // the X menu (or a window opened from it) is up
    bool poketch_large = false;
    int zone = 0;
    bool position_ok = false, poketch_ok = false;
    bool pokedex_acquired = false, poketch_acquired = false, map_acquired = false;
    std::array<int32_t,500> map_work{};
    int map_guide = -1; // EvWork 249 TOWNMAP_GUIDE_SEQUENCE; -1 when not readable
    std::array<u8,1000> map_sys{};
    int grid_x = 0, grid_y = 0, map_zone = 0, map_grid_x = 0, map_grid_y = 0;
    int fashion = 0, body_type = 0;
    bool player_sex = true;
    std::string player_name, rival_name;
    u32 steps = 0;
    bool steps_ok = false;
    std::array<bool, 20> poketch_apps{};
    // independent Pokétch save data
    bool dotart_ok = false, dotart_modified = false;
    std::array<u8, 192> dotart{};
    std::array<std::array<u16, 2>, 12> history{};    // {monsno, form}, oldest first
    std::array<std::array<u16, 2>, 6> marks{};        // marker centres, LCD px (0,0 = in the tray)
    struct Roamer { int zone_index = -1, monsno = 0, status = 0; };
    std::array<Roamer, 2> roamers{};
    int chain_count = 0, chain_mons = 0;
    std::array<std::array<int, 2>, 3> chain_ranking{}; // {monsno, count}, highest first
    std::array<PartyMon, 2> daycare{};
    bool egg_exists = false;
    std::array<int, 4> map_works{};                   // works 278..281 (hidden Marking Map parts)
    std::array<int, 8> hidden_moves{};                // per button: 0 not owned, 1 owned (gray), 2 usable (black)
    int repel_units = 0, seeker_charge = 0, radar_charge = 0;
    u32 battle_points = 0;
    int poketch_colour = 0;
    std::array<u32, 12> calendar_marks{};
    bool party_ok = false; // a valid count was read, including an empty party
    int party_count = 0;
    std::array<PartyMon, 6> party{};
    // battle
    bool in_battle = false;
    bool battle_continue = false; // an input-enabled native dialogue, never a selectable menu
    CanvasState action, waza, ball;
    int action_min = 0, action_max = 3; // unknown bounds retain the normal four-command layout
    // [0] player, [1] opponent: party member 0 of each side; in a double [0] is the Pokémon
    // choosing its command (m_procPokeIdx) while one is
    std::array<BattleMon, 2> front{};
    DoubleState dbl;
    int own_count = 0;
    std::array<BattleMon, 6> own{};
    // the opponent side's party (POKECON party[1]): every member, the lead first; count 0 = not read
    int foe_count = 0;
    std::array<BattleMon, 6> foe{};
    // Terrain page: both opposing clients' full parties in a multi battle (up to 6 + 6).
    std::vector<BattleMon> opponents;
    BattleField battle_field;
    BattleMon switch_foe; // exact opponent announced by the native shift-rule prompt
    std::vector<int> balls; // item ids in the ball quick list order
    bool ball_enabled = true; // false in trainer battles
    // the game's own window over the battle menu (top screen): 0 none, off::WindowIdBag (the Bag) or
    // off::WindowIdPokemonBattle (the Pokémon list: a switch, also the forced one after a faint)
    int battle_window = 0;
};

class Reader {
public:
    explicit Reader(Guest g) : guest{std::move(g)} {}
    // Re-learns the delta when not yet known (cheap after success).
    bool Resolve(int64_t host_delta);
    Snapshot Sample();
    void SetLuminescent(bool value) { luminescent = value; }
    // Recheck live native focus before sending a dialogue A press.
    bool CanContinueBattle() const;
    // Battle-menu helpers (writes go through Guest::write; all are 1-4 byte field stores).
    bool SelectFieldMenu(int type);
    bool SetActionIndex(int index);
    bool SetWazaIndex(int index);
    bool SetBallIndex(int index);
    bool CanTargetInput(int index = -2) const; // -2 any/cancel, -1 spread confirmation
    bool SetTargetIndex(int index);
    // The translucent Poké Ball behind the top-screen command list (UISystem._decoImage):
    // alpha of the RawImage's managed colour; takes effect at the next canvas rebuild.
    bool SetDecoAlpha(float alpha);
    // Park the command list and the move list off-screen (slide transition with equal hide/show
    // anchors); `false` writes the originals back. Includes the Poké Ball quick list.
    bool HideBattleMenus(bool hide);
    // Field party edits through the atomic write batch (every op checks the bytes it replaces).
    bool SwapParty(int a, int b);
    bool SwapMoves(int slot, int a, int b, const lp_pk8::Mon& expected);
    bool ResetPedometer();
    // Restores `amount` HP (255 = full; 254 = half on a revive) to member `slot`, cures / restores
    // PP per the native ItemTable recovery flags (the selected move for Ether / Max Ether) and
    // takes one `item` from the Bag. Refuses fainted (unless reviving) / unchanged members.
    // Returns HP restored or 1 for a status / PP cure; 0 never consumes the item.
    int UseMedicine(int slot, int item, int hp_amount, u32 flags, int pp_amount,
                    const std::array<int, 4>& max_pp, int move_slot, const std::array<int, 3>& friend_change = {},
                    const lp_pk8::Mon* expected = nullptr);
    // Edits several party members and (optionally) takes one `item` from the Bag in one atomic
    // write batch. Each edit gets the decrypted core and the decoded view, returns false to
    // refuse. One/two-member edits compare the complete encrypted core. Larger party-wide
    // edits compare compact changed spans plus EC/checksum; a simultaneous checksum-colliding
    // native change outside those spans cannot be detected within the host's 16-op limit.
    using MonEdit = std::function<bool(std::array<u8, lp_pk8::CoreSize>& plain, const lp_pk8::Mon& mon)>;
    bool EditParty(const std::vector<std::pair<int, MonEdit>>& edits, int item);
    // Repel / Super Repel / Max Repel: start the effect (`units` as the item's param) and take one.
    bool UseRepel(int item, int units, int type);
    // Takes one `item` out of the Bag of the save `owner` (PlayerWorkObject when the use started),
    // only over the count read now: what the game's Bag takes for a field use it has run.
    bool ConsumeItem(u64 owner, int item);
    // Writes a ushort into saveItemShortcut[slot] when it currently holds `expect`.
    bool SetShortcut(int slot, int expect, int value);
    std::vector<int> Shortcuts() const;
    // Points the game's own Bag (X menu) at `item` in `pocket`: the pocket and the item's row in the
    // pocket's live list (items with a count), scroll 0 so the Bag scrolls to it. Bag must be closed.
    bool SetBagCursor(int pocket, int item);
    // Bag counts indexed by item number (empty when PlayerWork is not ready).
    std::vector<int> BagCounts() const;
    // One Bag count, BagCounts()[item] without reading the whole array (0 when out of range).
    int BagCount(int item) const;
    // Hidden items the native Dowsing Machine can still find (spawned, dowsing level > 0, vanish
    // flag not set), from the game's own field-object list.
    std::vector<HiddenItem> HiddenItems() const;
    std::vector<int> DexStatuses(bool packed) const;
    int64_t Delta() const {
        return delta;
    }
    // Guest addresses for game-thread calls (lp_guest.h): a main method (the image's own
    // pointers are main_base relative, see Fingerprint) and the field singletons (0 = none).
    u64 Code(u64 rva) const {
        return resolved ? guest.main_base + rva : 0;
    }
    // Lifetime token for pending field operations; never a persisted save identifier.
    u64 PlayerWorkObject() const { return resolved ? PlayerWork() : 0; }
    u64 FieldManagerObject() const;
    u64 EvDataManagerObject() const;
    // Existing initialized audio singleton; never instantiates a Unity object.
    u64 AudioManagerObject() const;

private:
    template <typename T>
    std::optional<T> Rd(u64 addr) const {
        T v{};
        if (!addr || !guest.read(addr, &v, sizeof(T)))
            return std::nullopt;
        return v;
    }
    std::string PersonName(u64 object) const;
    u64 Ptr(u64 addr) const {
        return Rd<u64>(addr).value_or(0);
    }
    u64 Slot(u64 main_off) const {
        return guest.main_base + main_off + static_cast<u64>(delta);
    }
    // TypeInfo slot -> klass -> static fields block (0 = not initialised yet).
    u64 Statics(u64 type_info) const {
        const u64 k = Ptr(Slot(type_info));
        return k ? Ptr(k + off::KlassStatics) : 0;
    }
    // A pointer-sized static field of the class behind `type_info`.
    u64 StaticPtr(u64 type_info, u64 field) const {
        const u64 s = Statics(type_info);
        return s ? Ptr(s + field) : 0;
    }
    bool Fingerprint(int64_t d) const;
    bool SetListAlpha(u64 list, float v);
    u64 BattleViewCore() const;
    u64 PlayerWork() const;
    u64 PoketchWindow() const;
    // work_ok / sys_ok: s.map_work / s.map_sys hold this sample's bulk reads.
    void SamplePlayerItems(u64 pw, Snapshot& s, bool sys_ok) const;
    void SamplePoketchData(u64 pw, Snapshot& s, bool work_ok) const;
    // Validated member-array data; count 0 is a valid empty party, -1 is unavailable/corrupt.
    u64 PartyMembers(u64 party, int& count) const;
    // Party member `slot`'s PokemonParam (0 = no such member or a count above six).
    u64 PartyParam(u64 pw, int slot) const;
    // Bag op taking one `item` (count expected to be unchanged), or nullopt when none is held.
    std::optional<BatchOp> TakeOne(u64 pw, int item) const;
    CanvasState Canvas(u64 obj) const;
    bool SetDecoAlpha(u64 ui, float alpha); // with the UISystem already resolved
    BattleMon ReadBpp(u64 bpp) const;
    void ReadBppState(u64 bpp, u64 core, BattleMon& m) const;
    BattleField ReadField(u64 env) const;
    // the double-battle layout, procPokeIdx and the target select (fills s.dbl; adjusts s.front[0])
    void ReadDouble(u64 vs, u64 ui, u64 parties, Snapshot& s) const;
    // the Bag / Pokémon window opened from the battle menu (fills battle_window)
    void ReadBattleWindow(u64 ui, Snapshot& s) const;
    void ReadSwitchFoe(u64 vs, Snapshot& s) const;
    bool CanContinueBattle(u64 ui) const;
    bool CanContinueExperience(u64 window) const;
    // the HP the top-screen gauges show now (fills hp_view of every battler a status window shows)
    void ReadGauges(u64 ui, Snapshot& s) const;
    // managed array data (+0x20) when the length (+0x18) is at least `need` and at most `max`
    u64 ArrayAt(u64 arr, u64 need, u64 max) const;
    // a List<T>'s items array data when 0 < _size <= max and _size fits the items array (size = _size),
    // else 0 (size = 0)
    u64 ListData(u64 list, int& size, int max) const;
    bool ReadByteArray(u64 arr, std::size_t expect, std::vector<u8>& out) const;

    Guest guest;
    bool luminescent = false;
    int64_t delta = 0;
    bool resolved = false;
    u32 last_steps = 0;
    int sweep_backoff = 0; // unresolved samples to skip before the next full delta sweep
    struct Saved {
        u64 obj = 0;
        u8 type = 0;
        float hide_x = 0, show_x = 0;
        bool saved = false;
    };
    std::array<Saved, 3> saved_lists{};
};

} // namespace lp_live
