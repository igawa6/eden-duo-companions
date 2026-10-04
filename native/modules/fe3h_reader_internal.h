// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Private to the fe3h_reader*.cpp translation units: the offsets (each one encoded in the pin
// named next to it, fe3h_pins.inc), small shared helpers and the per-sample output type.

#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fe3h_assets.h"
#include "fe3h_reader.h"

namespace Fe3hReader {
namespace detail {

/// 89048449BA238C8CF565518B83BF02D3, then 16 zero bytes.
constexpr std::array<u8, 16> Build120{0x89, 0x04, 0x84, 0x49, 0xBA, 0x23, 0x8C, 0x8C,
                                      0xF5, 0x65, 0x51, 0x8B, 0x83, 0xBF, 0x02, 0xD3};

// ---- offsets (each one is encoded in the pin named next to it) ------------------------------
// App / scene (AppScene, InGameVtable, SeqRead)
constexpr u64 AppScene = 0x40, SceneSeq = 0x100, AppState = 0x30; // AppStateRun: App+0x30
constexpr s32 AppInGame = 7;
constexpr s32 SeqMonastery = 0x0B;
// G (BattleUnits, Cursor, MapSize, UnderCursor, Turn, Phase, Forecast*)
constexpr u64 GCursorX = 0x4, GXMin = 0x3B18, GYMin = 0x3B1C, GXMax = 0x3B20, GYMax = 0x3B24;
constexpr u64 GTurn = 0x3D40, GPhase = 0x3D42, GBattleActive = 0x3D51;
constexpr u64 GFcA = 0x1C90, GFcB = 0x1D68;                           // unit* per side
constexpr u64 GFcHitA = 0x1CF0, GFcDmgA = 0x1D00, GFcCritA = 0x1D08;  // ForecastA
constexpr u64 GFcHitB = 0x1DC8, GFcDmgB = 0x1DD8, GFcCritB = 0x1DE0;  // ForecastB
constexpr u64 GFcCountA = 0x1D0C, GFcCountB = 0x1DE4;                 // FcCount (x<n> when >= 2)
constexpr u64 GFcItemA = 0x1C98, GFcItemB = 0x1D70, GFcArt = 0x3B68; // FcItemA/B, FcArtLine
constexpr u32 TxtNoWeapon = 0x13E, TxtNoArts = 0x13F;
constexpr u64 GFcTotalA = 0x1C90 + 0x74, GFcTotalB = 0x1C90 + 0xD8 + 0x74; // total damage
constexpr u64 GBlockA = 0x1C90, GBlockAEnd = 0x1DE8;                  // one read covers both
// UI (UiRoot, ForecastWin, ForecastGate)
// forecast window (FcWindow, FcDraw, WinActive): W = [var] + 0x20498; open = the window's
// active byte +0x21 and its layout +0x80 (live: both set with the forecast up, both clear on the
// weapon list and the move range)
constexpr u64 FcWindowOff = 0x20498, WinActive = 0x21, WinLayout = 0x80;
// unit list (BattleUnits, GetUnit)
constexpr u64 ListUnits = 0x8;
// unit / person layout
constexpr u64 UPid = 0x24, ULevel = 0x4A, UClass = 0x4B, UHp = 0x4C, UHpExtra = 0x4D;
constexpr u64 UStats = 0x4E;                                  // Stats: 9 bytes Str..Cha
constexpr u64 UAbilities = 0x7F, UArts = 0x84;                // Abilities, Arts
constexpr u64 UHpBonus = 0xB4;                                // MaxHp: 4 ids
constexpr u64 UFlags = 0xD6, UFlags2 = 0xD7, UExcl = 0xDB;    // BattleUnits, UnitLarge
constexpr u64 UCurHp = 0xDE, UHpBar = 0xDF;                   // UnitHp, MaxHp
constexpr u64 UArmy = 0xF1, UX = 0xF2, UY = 0xF3;             // ForecastArmy, UnitPos
constexpr u64 USlot = 0xF0, UGender = 0x104;                  // UnitSlot, UnitClassName
constexpr u64 ScMap = 0x8;                                    // ScMap: u16 battle map
constexpr u16 ItemEmpty = 0xFFFF;                             // PersonCtor
constexpr u8 AbilityEmptyA = 0xFF, AbilityEmptyB = 0xF0;      // Abilities
constexpr int ArtMax = 0x4F;                                  // ArtName
// persons (FindPerson, PersonCtor, MotivationCtor, SkillRank, Roster, Mastered)
constexpr u64 SPersons = 0x644, PersonSize = 0x24C;
constexpr u64 PRoster = 0xAC, PRoster2 = 0xAE, PHidden = 0xB2, PMotivation = 0xC4;
constexpr u64 PSkillRank = 0x1DC;
// save S (SaveCard, Money, EBase, Difficulty, ChapterCtx, MapId, PartTwo)
constexpr u64 SMoney = 0x24274, SChapter = 0x2427C, SDifficulty = 0x2449C;
constexpr u64 SRoute = 0x2449E, SPart = 0x2449F, SMap = 0x244A0, SChapterType = 0x244A1;
constexpr u64 SBlock = 0x24270, SBlockEnd = 0x244A2;
constexpr u64 SPlayerName = 0x9014, SSpecial = 0x23208, SpecialSize = 0x1B0, SpecialPid = 0x34;
constexpr u32 MoneyMax = 9999999;
constexpr u64 SM = 0x250C8;                                   // SaveCard: M = S + 0x250C8
constexpr u64 MRenown = 0xC, MExp = 0x12, MDay = 0x26, MBlock = 0x30;
// field F (FieldState; PersonName)
constexpr u64 FState = 0x5FC, FFlags = 0x28020;
// SC / minimap (ScStage, MinimapObj, MinimapFlag)
constexpr u64 ScStage = 0x4, MinimapFlag = 0x30544;
// text manager (GetName, GetHelp)
constexpr u64 TextPart1 = 0x50, TextPart2 = 0x60;
constexpr u32 Part1Max = 0x1A95, Part2Max = 0x3C24;
constexpr u32 TxtPerson = 0x485, TxtClass = 0xD7D, TxtClassAlt = 0xDE1, TxtMonth = 0x299C;
constexpr u32 TxtArt = 0x177E, TxtAbility = 0x1C44, TxtRankE = 0x208, TxtRankS = 0x4F1;
constexpr u32 TxtRankPlus = 0x20D, TxtHidden71 = 0x6F5, TxtHidden1CA = 0x6F6;
// persondata record (PersonName, ClassName)
constexpr u64 PdName = 0x12, PdGender = 0x26;
// name substitution object (NameSubst, PersonName)
constexpr u64 SubstPairs = 0x159C4, SubstMode = 0x158;
constexpr int SubstCount = 0x200;
// chapter record (ChapterCtx, MonthName)
constexpr u64 ChType = 0x19, ChMonth = 0x1D, ChTitle = 0x1C, ChRouteLabel = 0x20;
constexpr u32 TxtChapterTitle = 0x25F, TxtWeekday = 0x490, TxtGoal = 0x378B;
constexpr u64 TextPart3 = 0x70;
constexpr u32 Part3Max = 0x1B9;
// stat totals (StatClass, StatClamp, StatMov): class record +0x27+k, +0x52 b0 -> +0x30+k
constexpr u64 CrStatMod = 0x27, CrMonsterFlag = 0x52, CrMonsterMod = 0x30;
// danger area (DangerToggle, DangerBuild, MoveClear): tile +0x26 b0 move, b1 attack; +0x27 b0 all
// enemies' reach, b1 marked enemies' reach; G+0x3D52 native display on
constexpr u64 GDangerOn = 0x3D52, TileRange = 0x26, TileDanger = 0x27, TileSize = 0x30,
              TileRow = 0x600;
constexpr u64 UFootprint = 0xDC;                              // UnitSize
constexpr u64 PGoal = 0xDB, PSkillExp = 0xFC;                 // GoalText, SkillThreshold

inline bool EnvFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && std::string_view{v} == "1";
}

inline s64 SignExtend(u64 v, int bits) {
    const u64 m = u64{1} << (bits - 1);
    return static_cast<s64>((v ^ m) - m);
}


constexpr u64 URanks = 0x88, USkillExp = 0x32, UClassExp = 0x48, UMastered = 0x93;
constexpr u64 USpellUses = 0x94, USpellIds = 0xA0, UBt = 0x18, UGambitUses = 0xF5;
constexpr u64 UBtId = 0x1E, UGambitRow = 0x1F, UEquipW = 0x2E, UEquipE = 0x30;
constexpr u32 SpriteAbilityEmpty = 0x38, TxtNoneEquipped = 320, TxtHeal = 0x118, TxtAtk = 275;
constexpr u64 SkillIconFramed = 0xD5281C, SkillIconPlain = 0xD52848;
constexpr u64 SpellMaxKinds = 0xCEA45C;

constexpr u64 GFogFlag = 0x451, GDefeatLive = 0x3D5E;
constexpr u64 SQuestBattle = 0x2555F, SClassic = 0x2449D, ScName = 0xA;
constexpr u32 TxtConditions = 0x2E82, TxtBattleQuest = 0x3049;
constexpr u64 DefeatAltTable = 0xCDC63C;
inline bool DefeatMapRange(u32 m) {
    return m - 0x3D < 0x14 || m - 0x51 < 5;
}

constexpr u64 GState = 0x3CC0;          // battle sub-state (BattleState pin): 0 = free cursor
constexpr int DrvSettle = 6;             // samples after a move before the next press (~100 ms)
constexpr int DrvBlocked = 30;           // no cursor move this many samples after a press -> blocked
constexpr int DrvMaxSamples = 60 * 20;   // whole drive
inline const std::string DrvKeys[4] = {"bt.drv.up", "bt.drv.down", "bt.drv.left", "bt.drv.right"};

// field controller F / player P / walking state W (ArrowPos, LocTracker, PeopleBuilder)
constexpr u64 FPlayer = 0x380, FWalk = 0x398, FKind = 0x570, FZone = 0x620, FFixed = 0x28023;
constexpr u64 PPosX = 0x10, PPosZ = 0x18, PYaw = 0x50, WLocation = 0x2C;
constexpr u32 KindGarregMach = 0x3C, KindAbyss = 0x37;
// actor manager (PeopleBuilder): count +0xFA8 (<= 500), actor* at +8 + i*8
constexpr u64 ActCount = 0xFA8, ActList = 8, ActActive = 8, ActHide1 = 0xD2, ActHide2 = 0xD3,
              ActCid = 0xE8, ActBytes = 0x17C;
// map markers (0x2B6548 filter, 0x2B7230 sprite rule, 0x1DFAB0): +0x158 no marker, +0xB4 talk id,
// +0x178 talk distance, +0xBE parent actor index (0x12C = none), +0x6C..+0xB2 event ids
constexpr u64 ActNoMarker = 0x158, ActTalk = 0xB4, ActTalkDist = 0x178, ActParent = 0xBE,
              ActEvents = 0x6C, ActEventsEnd = 0xB4;
/// The map icon sprites in the fixed, append-only order of research/fe3h/re4/ICON_LIST.md
/// (gps.p{i}.icon_k / gps.i{k}.icon_k): every sprite FacilityMarkerSprite (0x5D9290) and the
/// quest icon rule (0x5D9CF0 / 0x5D9F80) can return, then the glow and the visitor bubble.
constexpr int IconSprites[] = {0x79D, 0x79E, 0x79F, 0x7A0, 0x7A1, 0x7A2, 0x7A3, 0x7A5,
                               0x7A7, 0x7AB, 0x7AD, 0x7AE, 0x7AF, 0x7B0, 0x7B1, 0x7B2,
                               0x7B3, 0x7B4, 0x7B8, 0x7C6, 0x6FA, 0x7BE, 0x7BF, 0x7C0,
                               0x7C1, 0x7C2, 0x7C3, 0x6FB, 0x6FC, 0x6FD, 0x7BB, 0x7CB};
// quests (QuestState / QuestOffers / QuestTargets): M+0x3F4 states, questdata rec +4 giver,
// +0x14/+0x16/+0x18 targets, +0x2B red; quest locations M+0x366; visitors M+0x14 u16[7]
constexpr int QuestCount = 0x97;
constexpr u64 MQuestState = 0x3F4, MQuestLoc = 0x366, MVisitors = 0x14, QuestRecBytes = 0x34;
constexpr u64 FCellOrigin = 0x610, TalkspotSlots = 0xCE37F0, TalkspotLocs = 0xCE2CF4;
constexpr int ActMax = 500;
// area table A (PeopleCall / PeopleBuilder / LocNameChooser)
constexpr u64 AreaRecs = 8, AreaCount = 0x328, AreaList = 0x32C, AreaRecBytes = 0x28;
constexpr int AreaRecMax = 100, PeopleSlots = 30;
// save S: route / part (SRoute / SPart), map id SChapterType, event flags (EventFlag), Anna var
constexpr u64 SEventFlags = 0x244A2, SAnnaVar = 0x244B0;
constexpr u32 FlagAnna = 0x75;
constexpr u64 DlcFlags = 0x64, DlcFlags2 = 0x18;
// text (LocNameText, LocPopupText)
constexpr u32 TxtLocation = 4140, TxtLocPopup = 0x30EA;
// talk (TalkIdle, TalkWinT)
constexpr u64 TmOpen = 0x5A9, TOpen = 0x1A49;
// persondata (Affiliation), save person (PersonRecord)
constexpr u64 PdAffil = 0x24;
constexpr u64 AffilPartTwo = 0xCB9350;
constexpr float Pi = 3.14159274f, TwoPi = 6.28318548f;

/// FacilityNpc (0x8A6B0 + its jump table): 0x12C..0x18C entries, Anna, 0x2AF.
inline bool FacilityNpc(s32 cid) {
    static constexpr u16 Set[] = {300, 301, 302, 304, 305, 306, 307, 309, 310, 312, 313, 314, 315,
                                  316, 317, 318, 343, 347, 348, 349, 350, 392, 393, 394, 395, 396};
    if (cid == 0x416 || cid == 0x2AF)
        return true;
    return std::find(std::begin(Set), std::end(Set), cid) != std::end(Set);
}

struct MapCtx {
    int m{-1};     ///< map id (S+0x244A1, 100 -> chapter record +0x19)
    u32 kind{};    ///< field kind (F+0x570 while F+0x28020 b0, else the static +4)
    u8 part{}, route{};
    int tex{};     ///< abbey texture (6074 #n)
    double scale{}; ///< world units per texture pixel
};

constexpr std::size_t ActorsPerStep = 100; // ~0x1E0-byte actors: ~48 KB of reads per sample
constexpr int PersonKeysPerStep = 12;       // PersonName + portrait keys per sample
inline float RecF(const std::array<u8, AreaRecBytes>& r, std::size_t off) {
    float v;
    std::memcpy(&v, r.data() + off, 4);
    return v;
}

constexpr int QuestN = 151;
constexpr u64 QMoney = 0, QRenown = 2, QClient = 4, QBattalion = 0x1A, QItems = 0x20, QRed = 0x2B,
              QMonthOnly = 0x30;
constexpr u32 TxtQuestTitle = 0x3049, TxtThisMonth = 1343, TxtRenown = 0x28B, TxtPts = 0x1CF,
              TxtMoney = 0x28C, TxtG = 0x18C, TxtBattalion = 0x2388, TxtMissionSummary = 0x12C,
              TxtMissionSpecial = 0x190, TxtLostName = 10142, TxtLostDesc = 10397;
// scrdata part 2 (quest texts): obj +0 loaded, +0x60 XL; XL +6 cols, +8 rows, +0xA row bytes,
// +0xC header size, +0x10 column types (0 = string offset); ScrCell 0x3FB9E0
constexpr u64 ScrXl = 0x60;
constexpr int ScrMaxRow = 0x12D;
constexpr int ScrColDesc = 1, ScrColMsg = 0;
constexpr u8 ColSize[8] = {4, 4, 2, 1, 4, 2, 1, 4}; // main+0xCBAAC0
constexpr u64 MSecrets = 0x56B;
// lost items (0x2B0440 / 0x3EB2E0): presentdata row +8, storehouse counts
constexpr u64 SLostCountA = 0x24690, SLostCountB = 0x24F93;
constexpr int LostN = 0xF5;
constexpr u32 SpriteLostBag = 0x1FB;
constexpr double QuestSliceUs = 150.0; // a quests-page build step stops after this much

/// Text markup the companion does not draw: ESC 'N' x (name tags), ESC 'C' d (colour),
/// ESC 'Q' / ESC 'R' (highlight on / off), any other ESC + 1.
inline std::string StripMarkup(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size()) {
            const char c = s[i + 1];
            i += (c == 'N' || c == 'C') ? 2 : 1;
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

constexpr u64 SBylethMonth = 0x903D, SBylethDay = 0x903E; // month number - 1, day
constexpr u64 SActivity = 0x23B78;                         // E+0x978: free-day activity bits
constexpr u64 SFlags = 0x244A2;                            // event flags (EventFlag)
constexpr u64 SChapterBits19 = 0x244B6, SChapterBits6 = 0x244B4;
constexpr u64 MParalogue = 0x545, MDlcQuest = 0x568, MQuestStates = 0x3F4;
constexpr int CalSlotsQuest = 23, CalSlotsEvent = 30;
constexpr u32 SprCoin = 0x84F, SprNote = 0x7D1, SprTodayFrame = 0x77A, SprBylethBday = 0x790,
              SprFixedDate = 0x780, SprBirthday = 0x78F, SprMonster = 0x79A, SprParalogue = 0x79B,
              SprMission = 0x79C, SprShopAnna = 0x7B0;
// IconMap (0x5D9290) of 0x12C / 0x12D / 0x133 / 0x135 and 0x139..0x13B, 0x136
constexpr u32 SprEvB = 0x7A1, SprEvA = 0x7A0, SprEv5 = 0x7A2, SprEv7 = 0x79F;
constexpr u32 SprShopS = 0x7B1, SprShopE = 0x7B2, SprShopD = 0x7B3;
constexpr u32 ColSunRed = 0xAF, ColSun = 0xB0, ColDayWhite = 0xB3, ColDay = 0xB4;
constexpr u32 TxtEvents = 0x407, TxtActivity = 0x543, TxtRest = 0x4BC, TxtNoEvents = 0x51D,
              TxtBirthday = 0x4A1, TxtReward = 0x409, TxtWeekdayShort = 0x497;

/// FindPerson (0x3CAF30) over a copy of the save's person records (S+0x644, MaxPersons x
/// PersonSize): pid <= 1 -> the first record with pid <= 1, else the first with that pid
/// (<= 0x4B0). nullptr = none.
inline const u8* FindPerson(std::span<const u8> ps, int pid) {
    for (std::size_t off = 0; off + PersonSize <= ps.size(); off += PersonSize) {
        const u16 v = dsmod_sdk::Le16(ps.data() + off + UPid);
        if (v <= 0x4B0 && (static_cast<u32>(pid) <= 1 ? v <= 1 : v == pid))
            return ps.data() + off;
    }
    return nullptr;
}
/// The first record whose pid is exactly `pid` (<= 0x4B0).
inline const u8* FindPersonExact(std::span<const u8> ps, int pid) {
    for (std::size_t off = 0; off + PersonSize <= ps.size(); off += PersonSize) {
        const u16 v = dsmod_sdk::Le16(ps.data() + off + UPid);
        if (v <= 0x4B0 && v == pid)
            return ps.data() + off;
    }
    return nullptr;
}

} // namespace detail
using namespace detail;

// One sample's published values (plain lists; published in order).
struct Reader::Out {
    std::vector<std::pair<std::string, s64>> ints;
    std::vector<std::pair<std::string, std::string>> texts;
    void I(std::string k, s64 v) {
        ints.emplace_back(std::move(k), v);
    }
    void T(std::string k, std::string v) {
        texts.emplace_back(std::move(k), std::move(v));
    }
    std::vector<std::pair<std::string, double>> floats;
    void F(std::string k, double v) {
        floats.emplace_back(std::move(k), v);
    }
    // borrowed key / value (valid until the sample is published): no string copies
    std::vector<std::pair<const std::string*, s64>> int_refs;
    std::vector<std::pair<const std::string*, const std::string*>> text_refs;
    void R(const std::string& k, s64 v) {
        int_refs.emplace_back(&k, v);
    }
    void R(const std::string& k, const std::string& v) {
        text_refs.emplace_back(&k, &v);
    }
    std::vector<std::pair<const std::string*, double>> float_refs;
    std::vector<const Out*> pre; ///< whole prefiltered Outs published as they are
    void RF(const std::string& k, double v) {
        float_refs.emplace_back(&k, v);
    }
};

/// One People / marker / icon refresh, spread over several samples (StepPeople): the reads and
/// the build work on this snapshot; the finished Out replaces people_cache in one swap.
struct Reader::PeopleJob {
    enum Step { GatherA, GatherB, Actors, Compute, Warm, Rows, Markers, Icons };
    int step{GatherA};
    double scale{};
    std::string fail;
    // gathered inputs
    s32 chapter{};
    u8 route{}, part{}, fixed{};
    int m{};
    bool anna_flag{}, anna_var{}, dlc_anna{}, dlc_2af{};
    std::vector<u8> grid;
    std::vector<s32> list;
    std::array<std::array<u8, AreaRecBytes>, AreaRecMax> recs{};
    std::vector<u64> actors;
    std::vector<std::array<u8, ActBytes>> ab;
    std::vector<bool> ab_ok;
    std::size_t next_actor{};
    std::vector<u8> ps;
    bool ps_ok{};
    u64 player{};
    // build results
    struct Person {
        s32 cid;
        int loc, var;
    };
    struct Marker {
        s32 cid;
        float x, z;
        int kind;
        std::size_t ai;
    };
    std::vector<Person> found;
    std::vector<Marker> markers;
    std::map<std::pair<int, int>, std::vector<std::size_t>> table; // (loc, var) -> found indices
    bool exact{true};
    int pi{}, ri{};
    std::unique_ptr<Out> out;

    const std::array<u8, AreaRecBytes>& RecOf(s32 id) const {
        for (const auto& r : recs)
            if (r[1] == static_cast<u8>(id) && id >= 0 && id <= 0xFF)
                return r;
        return recs[0];
    }
};

} // namespace Fe3hReader
