// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem: Three Houses (010055D009F78000) game-memory reader, 1.2.0 only
// (build 89048449BA238C8C...). Every offset it uses is encoded in a build-pinned code site
// (fe3h_pins.inc): short accessors are checked word for word, the routines the reader
// reproduces (person/class/item names, max HP, professor level, chapter record, rank letters,
// the recruited-unit filter) by an FNV-1a 64 fingerprint. Globals are decoded from the ADRP+LDR
// (GOT) / ADRP+ADD pairs of those sites, never hardcoded. Names come from the game's own
// in-memory text manager (part 1 = table +0x50, part 2 = table +0x60), with the game's own index
// rules. Reads are bounded (is_mapped + read_memory, main-image checks); each group is read
// twice and compared, and the mode context is re-read after the reads (torn -> not ready).
// Domains fail closed on their own: fe.ready, bt.ready, ac.ready.
//
// Outputs: the values the package binds (p5r_publish::Filter drops indexed families it never
// names); every domain publishes its own ready / diag. Implementation: fe3h_reader.cpp (build
// check, guest reads, Sample, actions) and one fe3h_reader_<domain>.cpp per area, sharing the
// private fe3h_reader_internal.h.

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string_view>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "p5r_publish_filter.h"

#include "fe3h_assets.h"

namespace Fe3hReader {
using namespace dsmod_sdk::int_types;

/// One build-pinned code site: main offset + the exact instruction words expected there.
struct Pin {
    u64 offset;
    std::span<const u32> words;
    const char* name;
};
/// A longer routine the reader reproduces: FNV-1a 64 over [offset, offset + size).
struct HashPin {
    u64 offset;
    u64 size;
    u64 fnv;
    const char* name;
};

/// Only 1.2.0: build id 89048449BA238C8CF565518B83BF02D3 + 16 zero bytes. SupportsBuildHex takes
/// the 64-character upper-case hex that supports_build() receives; SupportsBuildId the 32 bytes.
bool SupportsBuildHex(const char* hex);
bool SupportsBuildId(const std::uint8_t* build_id32);

/// Companion pages (module action "ui_open <page>", published as ui.page).
enum Page : int {
    PageStandby = 0,
    PageBattle = 1,
    PageAcademy = 2,
    PageMonastery = 3,
    PageQuests = 4,
    PageUnits = 5, ///< battle Units page: the battle data as on page 1 (battle only)
};
inline constexpr int MaxUnits = 100;   ///< unit list capacity (GetUnit idx < 0x69; 100 allocated)
inline constexpr int MaxPersons = 60;  ///< S+0x644, 60 x 0x24C
inline constexpr int SkillCats = 11; ///< weapon/skill categories with ranks (setter clamps < 11)
inline constexpr std::size_t UnitBytes = 0x108; ///< unit bytes read (person part .. +0x104 gender)

/// Addresses decoded from the pins (tests may inject them, see Reader::UseGlobalsForTest).
struct Globals {
    u64 app{};          ///< static App object
    u64 ingame_vptr{};  ///< vptr of the in-game scene object
    u64 g{};            ///< static battle object G
    u64 unit_var{};     ///< pointer variable -> unit list manager
    u64 ui_var{};       ///< variable -> the battle UI object holding the forecast window (+0x20498)
    u64 grid_var{};     ///< pointer variable -> battle tile grid (32 x 32 x 0x30)
    u64 s_var{};        ///< pointer variable -> save object S
    u64 text{};         ///< static text manager
    u64 f_var{};        ///< pointer variable -> monastery field controller F
    u64 sc{};           ///< static scene descriptor SC
    u64 minimap{};      ///< static minimap UI object
    u64 pd_var{};       ///< fixed persondata manager variable
    u64 name_buf{};     ///< static default-name buffer (flag byte, name at +0x5C)
    u64 subst_var{};    ///< name substitution object variable
    u64 class_var{};    ///< fixed classdata manager variable
    u64 monster_var{};  ///< monster (HP bar) table variable
    u64 hpbonus_var{};  ///< HP bonus table variable (unit +0xB4..+0xB7)
    u64 prof_var{};     ///< professor level threshold table variable
    u64 dlc_var{};      ///< DLC flags object variable (roster filter, pid 0x416)
    std::array<u64, 5> chapter_var{}; ///< route 1, 2, {0,4}, 5, other
    std::array<u64, 7> item_var{};    ///< weapons, 4000.., 600.., 1000.., 5000.., 6000.., 7000..
    u64 art_var{};
    u64 ability_var{};
    std::array<u64, 5> cal_var{};     ///< calendar tables: C28 (common), C30, C38, C40, C48
    u64 skill_var{};                  ///< skill rank threshold table
    u64 equip_obj{};      ///< static equipment slot object (0x413370: indices 10, 4000.., ...)
    u64 class_abil_var{}; ///< class ability table (0x40E010)
    u64 personal_var{};   ///< personal ability table (0x40E520)
    u64 battalion_var{};  ///< battalion table (0xB04F0)
    // monastery / dialogue (round 7)
    u64 actor_var{};      ///< field actor manager variable (People builder 0x202A80)
    u64 area_var{};       ///< area table variable (People locations)
    u64 field_kind{};     ///< static object whose +4 = field kind while F+0x28020 b0 is clear
    u64 tm_var{};         ///< talk manager variable (TM+0x5A9 = message window open)
    u64 t_var{};          ///< monastery talk object variable (T+0x1A49 = window open)
    u64 quest_var{};      ///< questdata table variable (hdr 0xE30, 151 x 0x34)
    u64 loccell_var{};    ///< location cell-rect table variable (hdr 0x968, 100 x {z0, x0, z1, x1})
    u64 color_table{};    ///< static runtime text colour table (u32 per index)
    u64 lang_var{};       ///< variable -> object with the language at +0x158
    // quests (round 9)
    u64 scr_obj{};        ///< static scrdata part-2 object (+0 loaded, +0x60 XL)
    u64 bonus_var{};      ///< explore-bonus table variable (hdr 0x3C8, 40 rows)
    u64 exp_var{};        ///< level exp threshold table variable (hdr 0x938)
    u64 hud_obj_var{};    ///< variable -> objective text object (0x47FBC0)
    u64 material_var{};   ///< materialdata table variable (hdr 0x14F0, row +8 rarity)
    u64 secret_var{};     ///< secrets-quest reward table variable (hdr 0x1E8)
    u64 present_var{};    ///< presentdata table variable (hdr 0x1700, row +8 lost-item class)
    std::array<u64, 4> tourney_vals{}; ///< quests 0x50..0x53: renown early / late, money late / early
    std::array<u64, 14> count_vals{};  ///< reward counts {lo, hi} per id range (0x422820)
    // unit Details (round 14)
    u64 status_var{};     ///< status effect table (hdr 0x908, +3 type, +0 s8 value; 0xB046C)
    u64 weight_var{};     ///< s32 extra weapon weight (0xC5164)
    u64 gambit_var{};     ///< gambit uses table (hdr 0x788, row unit+0x1F, +7 max; 0xC52B0)
    u64 fog_ox{}, fog_oz{}; ///< f32 battle map origin x / z (0x9CCE0 position -> tile)
    u64 goal_var{};        ///< goal table (hdr 0x12C8, u16 skill mask per goal; 0x3D48F0)
    u64 strength_var{};    ///< skill strength table (hdr 0x440, +0x14 + k; 0x3F35B0)
    u64 strength_found{};  ///< s32 strength value of a found budding talent (3)
    u64 talent_var{};      ///< budding talent table (hdr 0x1688, +0 pid, +2 value, +3 skill)
    u64 pspell_var{};      ///< personal spell table (hdr 0x440; 0x40DF40)
    u64 pspell_generic{};  ///< the generic units' spell record (0x40DFE4)
    // Calendar (round 22)
    u64 fixdate_var{};     ///< fixed-date table (hdr 0x12C8, 200 rows: +0 pid, +6 day, +7 type, +8 month, +0xB part bits; 0x424918)
    u64 deadline_var{};    ///< paralogue deadline table (hdr 0x968, row id: s8 +0x14 + route chapter, +0x18 + route day; 0x42420C)
    std::array<u64, 4> calev_var{}; ///< day event tables (hdr 0x4B8) behind record +0xB / +0xA / +5 / +7 (0x1D0A04..)
    u64 tourney_var{};     ///< tournament table (hdr 0x4B8: +2 u16 reward, +0x12 name; 0x40B770)
    u64 giver_var{};       ///< paralogue giver table (hdr 0x2D8, row +2 / +4 pid; 0x3F11AC)
    u64 cal_dlc_var{};     ///< DLC flags object variable (+0x24 / +0x30 / +0x64 bits; 0x3F10FC)
    u64 cal_actor_var{};   ///< field actor manager variable (+8: 500 actors; 0x1D0110)
    u64 cal_field_var{};   ///< field controller variable (F+0x28020 b0 -> F+0x570; 0x1D0150)
    u64 cal_field_kind{};  ///< static field-kind word (+4 while F+0x28020 b0 is clear; 0x1D015C)
    u64 cal_flag_var{};    ///< variable -> object with the bit field +0x10C5 (0x1D0204)
    u64 cal_dlc_flags{};   ///< static u32[4]: event flag of pids 0x410..0x413 (0x1D0260)
};

/// One battle sample's tile-grid layers (row y: bit x = tile (x, y)).
struct DangerLayers {
    std::array<u32, 32> dz{}, mk{}, mv{}, at{};
};
/// Content-keyed overlay images of the danger / range layers, shared between the tick thread
/// (Key) and the asset worker (Render).
class DangerStore {
public:
    // A fixed underlying type lets Key reject invalid values without enum UB.
    enum Layer : int { Dz = 0, Sel = 1, Mk = 2 };
    /// "module:fe3h:danger/<dz|sel|mk>/<hash>", or "" when the layer is empty.
    std::string Key(Layer layer, const DangerLayers& layers);
    /// key without "module:"; false when unknown.
    bool Render(std::string_view key, std::vector<u8>& rgba, u32& w, u32& h);

private:
    std::mutex mutex;
    std::unordered_map<u64, std::array<u32, 64>> entries;
    std::deque<u64> order;
    std::array<std::pair<std::array<u32, 64>, std::string>, 3> last_masks{}; // tick thread only
};

class Reader {
public:
    Reader(const EdenDsmodHostApi& host, const char* config_json);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /// Reads the game and publishes every output (every call; the snapshot is cleared per tick).
    void Sample(const EdenDsmodHostApi& host);
    /// The host ticks hidden companions without sampling them; cancel stale cursor requests.
    void Tick(const EdenDsmodHostApi& host);
    /// "ui_open <page>", "bt_select <i>", "ac_select <i>" and "qs_select <i>" (-1 clears). True = accepted.
    bool OnAction(const char* action, std::int64_t argument);

    // ---- test / diagnostics ----
    bool CodeOk() const {
        return core_ok;
    }
    bool BattleCodeOk() const {
        return battle_ok;
    }
    bool AcademyCodeOk() const {
        return academy_ok;
    }
    bool CalendarCodeOk() const {
        return cal_ok;
    }
    const std::string& CalendarDiag() const {
        return cal_diag;
    }
    const std::string& CodeDiag() const {
        return code_diag;
    }
    const Globals& Resolved() const {
        return gl;
    }
    double AverageSampleUs() const {
#ifndef NDEBUG
        return cost_n ? cost_sum_us / static_cast<double>(cost_n % 600 ? cost_n % 600 : 600) : cost_published;
#else
        return 0.0;
#endif
    }
    /// Skip the code checks and use these globals (synthetic-memory tests only).
    void UseGlobalsForTest(const Globals& g);
    /// The asset library (item icons); owned by the module, thread-safe.
    void SetAssets(fe3h_assets::Library* lib) {
        assets = lib;
    }
    /// The overlay images (thread-safe; load_image renders from it).
    DangerStore& Danger() {
        return *danger;
    }

    // The game's own rules, reproduced (exposed for the tests). Empty string = the game shows none.
    std::string RankText(int rank);
    std::string PersonName(int pid);
    std::string ClassName(int cls, int pid);
    std::string ItemName(int id);
    /// The name renderer's rule (main+0x390F98): ESC 'N' + one byte is not drawn.
    static std::string StripNameTags(std::string s);
    /// First line, at most ~60 characters cut at a word boundary (quest list objective).
    static std::string ShortLine(const std::string& s);
    int ItemMaxUses(int id);
    int UnitMaxHp(const u8* unit, u64 s);
    int ProfLevel(u64 s);
    std::string UnitClassNameForTest(const u8* unit) {
        return UnitClassName(unit);
    }

private:
    struct Table {
        u64 var{};
        u64 hdr{}; ///< offset of the section-header pointer (count at [hdr]+4)
    };
    struct Unit {
        u64 at{};
        std::array<u8, UnitBytes> b{};
    };
    struct Out; // published values of one sample

    bool Read(u64 at, void* out, std::size_t n) const;
    template <typename T>
    std::optional<T> Get(u64 at) const {
        T v{};
        if (!Read(at, &v, sizeof(T)))
            return std::nullopt;
        return v;
    }
    std::optional<u64> Ptr(u64 at) const;
    bool InMain(u64 at, u64 n) const;
    std::optional<std::string> CString(u64 at, std::size_t max) const;

    bool Resolve();
    bool CheckPin(const Pin& pin);
    bool CheckHash(const HashPin& pin);
    std::optional<u32> CodeWord(u64 off) const;
    u64 GotTarget(u64 adrp_off, u64 ldr_off) const;  ///< [GOT slot] (main address), 0 = fail
    u64 AdrpAddTarget(u64 adrp_off, u64 add_off) const;

    // fixed data (mgr = [var]; count = u32 [[mgr+hdr]+4]; entry i = mgr+8+i*0x18)
    std::optional<u64> Entry(const Table& t, s64 i) const;
    std::optional<u64> Record(const Table& t, s64 i) const;
    std::string Text(u64 table_off, u32 index);
    std::optional<u64> ChapterRecord(int route, s32 chapter) const;
    std::optional<u64> ItemRecord(int id) const;

    bool ReadHeader(Out& o, u64 s);
    void ReadBattle(Out& o, u64 s);
    void ReadAcademy(Out& o, u64 s);
    bool ReadAcademyDetail(Out& o, u64 s);
    std::vector<std::pair<int, u16>> ac_roster; ///< {person slot, pid} of the published ac.s{i}
    void ReadSelected(Out& o, const u8* unit, u64 s, u64 at = 0);
    void ReadDetails(Out& o, const u8* unit, u64 s, u64 at);
    void ReadDetails2(Out& o, const u8* unit, u64 s, bool unit_mode = true);
    void PersonSkillRows(Out& o, const u8* person);
    void PersonInfo(Out& o, const u8* person, u64 s);
    int AffiliationIndex(int pid, u64 s);
    void DriveCursor(Out& o, bool battle);
    bool StartDrive(s64 argument);
    void ReadOverview(Out& o, u64 s, const std::vector<Unit>& units, const std::array<s32, 4>& bounds);
    bool UnitVisible(const Unit& un, u8 fog, const std::array<s32, 4>& bounds);
    int DefeatIndex(u64 s, u8 g453, bool live, const std::vector<Unit>& units);
    std::string BattleText(u32 index, bool& exact);
    /// The personal ability id (0x40E520); -1 = unreadable.
    int PersonalAbility(const u8* unit, u64 s);
    std::array<int, 2> GoalSkills(int goal);
    /// Class icon key (0x5CF440 -> 0x5CF7E0): `map` = the battle unit (gender +0x104, army +0xF1),
    /// false = a person record off the map (persondata +0x26 gender, army 0).
    std::string ClassIcon(const u8* unit, u64 s, bool map);
    /// The item behind an equipment index (0x413370); id -1 = none, read false = unreadable.
    struct EquipSlot {
        int id{-1}, uses{}; ///< id -1: no slot, an empty slot (0xFFFF) or unreadable
        u16 raw{0xFFFF};    ///< the slot's id as stored (0xFFFF included)
        bool slot{};        ///< the index names a slot
        bool read{true};    ///< false: the global slot is unreadable
    };
    EquipSlot EquipItem(const u8* unit, u16 equip);
    /// The broken-weapon rule 0x40C780 (the id whose record a 0-use weapon shows).
    std::optional<int> EffectiveItemId(int id, int uses);
    /// 0xA7E80(unit, t): s8 values of effect type t over the equipped abilities (condition 1 /
    /// 0xF), the personal ability and the class abilities (any condition). nullopt = unreadable.
    std::optional<int> EffectSum(const u8* unit, u64 s, int t);
    /// Spell max uses (0x4108C0 -> 0x410600) of spell i in the unit's spell slot j; -1 unknown.
    int SpellMax(const u8* unit, u64 s, int j, int i);
    int StatCap(const u8* unit, int k, u64 s);
    int FacilityBonus(const u8* m2b, u64 field);
    struct UnitKeys {
        std::string x, y, army, hp, hpmax, acted, lv, name, cls, dport_s;
    };
    std::deque<UnitKeys> unit_keys; ///< stable addresses (deque): published by reference
    const UnitKeys& UnitKeysFor(std::size_t i);
    void PublishAll(const EdenDsmodHostApi& h, const Out& o);
    std::string PortraitKey(int pid, u64 s);
    std::string UnitPortrait(const u8* unit, u64 s);
    bool PersonGrey(int pid, u64 s);
    std::string OvalPortrait(int pid, u64 s);
    std::optional<fe3h_assets::UnitFaceInput> FaceInput(const u8* unit, u64 s);
    // monastery GPS / People / dialogue (round 7)
    void ReadGps(Out& o, u64 s, u64 f);
    struct PeopleJob;
    bool StepPeople(u64 s, u64 f, double scale, int& kind);
    bool PeopleGather(PeopleJob& j, u64 s, u64 f);
    bool PeopleGather2(PeopleJob& j, u64 s, u64 f);
    void PeopleBuild(PeopleJob& j);
    void PeopleRows(PeopleJob& j, u64 s, Out& o);
    void PeopleMarkers(PeopleJob& j, Out& o);
    void PeopleIcons(PeopleJob& j, u64 s, u64 f, Out& o);
    void PeopleLegend(Out& o);
    const std::array<std::string, 4>& PersonKeys(s32 cid, u64 s);
    bool QuestRecords();
    bool LocCells();
    // quests page
    std::string QuestText(int row, int col, u64 s, bool raw = false);
    /// Text with the game's colour codes as {c:#AARRGGBB}..{/c} spans (TextColour*).
    std::string MarkupSpans(const std::string& s);
    int RewardCount(int id);
    void QuestEntry(Out& o, const std::string& p, int q, u8 st, const u8* rec, u64 s, bool full);
    bool StepQuests(u64 s);
    bool LostRows();
    void QuestRecordsStep();
    void LostRowsStep();
    void ReadMission(Out& o, u64 s);
    static constexpr std::size_t QuestKeyBytes = 0x97 + 0xE1 + 0x14 + 1;
    struct QuestJob {
        int step{};
        std::array<u8, 151> st{};
        std::vector<int> order;
        std::size_t next{};
        std::array<u8, QuestKeyBytes> key{};
        std::unique_ptr<Out> out;
        int fixed_steps{};
        bool lost_listed{}, lost_ok{};
        std::vector<int> lost;
        std::size_t lost_next{};
        std::unique_ptr<Out> lost_out;
    };
    std::unique_ptr<QuestJob> quest_job;
    std::unique_ptr<Out> quest_cache, mission_cache;
    std::array<u8, QuestKeyBytes> quest_key{};
    std::vector<int> quest_order;          ///< quest ids of the published rows
    std::array<u8, 151> quest_states{};
    u64 quest_gen{};                       ///< bumped per published list
    int quest_sel{-1};                     ///< qs_select row, -1 = none
    int quest_sel_id{-1};                  ///< the selected quest id (tracked across rebuilds)
    std::unique_ptr<Out> quest_detail;
    int quest_view{};                      ///< qs_view: 0 quests, 1 lost items
    std::unique_ptr<Out> lost_cache, lost_detail;
    std::vector<int> lost_order;           ///< lost-item indices of the published rows
    int lost_sel{-1}, lost_sel_id{-1}, lost_detail_id{-1};
    int quest_fill_next{}, lost_fill_next{};
    u64 quest_fill_mgr{}, lost_fill_mgr{};
    std::pair<int, u64> quest_detail_key{-1, 0};
    u64 quest_age{1000};
    u64 mission_at{};
    std::array<s32, 4> mission_key{};
    std::array<u8, 0xF5> lost_rows{};
    bool lost_rows_ok{};
    u64 lost_rows_mgr{};
    bool TalkOpen(bool& open);
    std::string LocationName(const u8* rec, u8 part, u8 route);
    void ReadObjects(std::span<const u64> at, std::size_t n,
                     const std::function<u8*(std::size_t)>& dst, std::vector<bool>& ok);
    std::string UnitClassName(const u8* unit);
    std::string ItemIcon(int id, int uses);
    int StatTotal(const u8* person, int k);
    struct StatPart {
        int value{};
        bool exact{};
    };
    StatPart AbilityStat(int id, int k);
    StatPart EquipStat(const u8* unit, u16 equip, int k);
    StatPart DisplayedStat(const u8* unit, int k, bool is_unit, u64 s);
    int SkillThreshold(int rank);
    std::optional<u64> CalendarRecord(u8 part, int route, int month, int day);
    // Calendar (round 22): the month schedule of the native X > Calendar screen
    struct CalEntry {
        u64 at{};      ///< fixed-date table entry (its +0x10 = the row index)
        u32 index{};
        u8 type{};     ///< row +7: 1 = fixed event, 2 = birthday
        u16 pid{};
        bool shown{};  ///< 0x1CFF30: the birthday is shown
    };
    struct CalEvent {
        u8 kind{};
        u16 arg{};
    };
    struct CalDay {
        u64 rec{};                ///< the day object's calendar record (+0x20), 0 = none
        std::array<u8, 0x10> r{}; ///< its bytes
        u64 raw{};                ///< the month's record of that day (stored or not)
        std::array<u8, 0x10> rr{};
        bool note{}, byleth{};
        std::vector<CalEntry> entries; ///< <= 3 (+8 / +0x10 / +0x18)
        std::vector<u16> quests;       ///< <= 23 (+0x2C..)
        int act{5};                    ///< +0x17C
        std::vector<CalEvent> ev;      ///< <= 30 (+0x8C..)
    };
    struct CalFixed {
        std::array<u64, 5> key{};
        std::array<u64, 32> rec{};
        std::array<std::array<u8, 0x10>, 32> r{};
        std::vector<std::pair<int, CalEntry>> dates; ///< (day, entry) of the month in table order
        std::vector<std::pair<int, int>> free_days;  ///< (day, index) of the month's type-2 records
    };
    bool CalBuild(u64 s);
    void ReadCalendar(Out& o, u64 s);
    bool CalFixedData(u8 part, int route, int month);
    bool CalQuestOpen(u64 s, int id, const std::vector<u8>& persons);
    bool CalAbsent(u64 s, int pid, const std::vector<u8>& persons);
    bool CalBirthday(u64 s, const CalEntry& e, const std::vector<u8>& persons);
    std::string RewardIcon(int id);
    CalFixed cal_fixed;
    /// The save's person records (S+0x644, 60 x 0x24C) read once per sample on first use; nullptr
    /// when unreadable. Callers that need a torn check read their own second copy.
    const std::vector<u8>* Persons(u64 s);
    std::vector<u8> persons_buf;
    u64 persons_s{}, persons_sample{~u64{0}};
    bool persons_ok{};
    std::optional<std::unordered_map<int, bool>> cal_actors; ///< cid -> hidden (+0xD2 b4), per build
    std::array<CalDay, 32> cal_days;
    struct CalMonth {
        bool ok{};
        u8 part{};
        int route{}, chapter{}, month{-1}, mon_n{}, ndays{}, first{}, today{}, ctype{};
        std::string why;
    } cal_m;
    bool cal_ok{};
    std::unique_ptr<Out> cal_cache;
    u64 cal_age{1000};
    int cal_sel{}; ///< cal_day: selected day, 0 = today
    /// {PersonName(pid), ClassName(cls, pid)}, cached for NameCacheSamples samples.
    const std::pair<std::string, std::string>& Names(int pid, int cls);

    EdenDsmodHostApi host{};
    fe3h_assets::Library* assets{};
    p5r_publish::Filter filter;
    std::unique_ptr<DangerStore> danger{std::make_unique<DangerStore>()};
    bool resolved{}, core_ok{}, battle_ok{}, academy_ok{}, test_globals{};
    std::string code_diag, core_diag, battle_diag, academy_diag;
    std::string bt_why, ac_why; ///< why the last battle / academy read was not ready
    u64 resolve_sample{}, resolve_count{};
    u64 base{}, size{};
    Globals gl;
    std::unordered_map<u64, std::vector<u8>> hash_bytes; ///< verified routine bytes by offset
    Table persons_tbl, class_tbl, monster_tbl, hpbonus_tbl, prof_tbl, art_tbl, ability_tbl;
    std::array<Table, 5> chapter_tbl;
    std::array<Table, 7> item_tbl;
    std::array<Table, 5> cal_tbl;
    Table skill_tbl, class_abil_tbl, personal_tbl, battalion_tbl, quest_tbl, loccell_tbl,
        material_tbl, secret_tbl, present_tbl, status_tbl, gambit_tbl, goal_tbl, strength_tbl, pspell_tbl;
    bool details_ok{}, clsicon_ok{}, overview_ok{}, drive_ok{}, goals_ok{}, skills_ok{}, info_ok{}, lmag_ok{};
    std::string cal_diag;
    bool mk_ok{}; ///< the text colour-code rule is pinned
    Table fixdate_tbl, deadline_tbl, tourney_tbl, giver_tbl;
    std::array<Table, 4> calev_tbl;
    struct Drive {
        bool active{}, waiting{};
        int tx{-1}, ty{-1}, px{}, py{}, dir{-1}, wait{}, idle{}, samples{}, presses{}, result{};
        std::string status;
    } drv;
    bool drv_tap_pending{};
    std::pair<s64, s64> drv_tap{-1, -1}; ///< tile of the last bt_goto_tap
    std::unique_ptr<Out> ov_cache;
    std::array<s64, 6> ov_key{};
    u64 ov_at{};
    std::string details_diag;
    bool stats_ok{}, gps_ok{}, talk_ok{}, affil_ok{}, quests_ok{};
    std::string gps_diag, talk_diag, quests_diag;

    u64 text_tables[3]{}; ///< part 1 / part 2 table pointers the cache belongs to
    std::unordered_map<u64, std::string> text_cache;
    std::unordered_map<u32, std::pair<std::string, std::string>> name_cache;
    u64 name_cache_age{}, name_epoch{};
    static constexpr u64 NameCacheSamples = 120;
    u64 NameEpoch(u64 s);
    /// Fixed-data entries by (var, index): {mgr, entry}. Fixed data is loaded once at boot; an
    /// entry is reused only while [var] still names the same manager.
    mutable std::unordered_map<u64, std::pair<u64, u64>> entry_cache;

    int ui_page{PageStandby};
    int battle_layout{}; // 0 All, 1 Map, 2 Char; shared by tabs and unit inspection
    int last_mode{-1};
    s32 last_seq{-1};
    int ac_sel{-1};
    u64 bt_pick_at{};                ///< tapped unit (bt_select), 0 = follow the cursor
    std::vector<u64> bt_last_units;  ///< unit addresses of the last published bt.u{i} list
    struct UnitDerived {
        std::array<u8, 12> key{};
        u64 at{};
        int hpmax{-1};
        std::string name, cls, dport_s;
    };
    static constexpr u64 UnitCacheSamples = 30;
    std::unordered_map<u64, UnitDerived> unit_cache;
    struct HeaderDerived {
        bool valid{};
        std::array<s32, 5> key{};
        u64 at{};
        int month{-1}, level{-1}, medal{-1};
        std::string chapter_title, why;
    };
    static constexpr u64 HeaderCacheSamples = 120;
    HeaderDerived hdr_cache;
    std::unique_ptr<Out> sel_cache;
    std::unique_ptr<Out> dt_cache; ///< ReadDetails2 of the shown unit (the sample after sel_cache)
    bool dt_pending{};
#ifndef NDEBUG
    double sel_max_us{}, dt_max_us{};
#endif
    std::unique_ptr<Out> battle_out; ///< last battle read (republished on the odd samples)
    u64 sel_key{}, sel_at{};
    std::unique_ptr<Out> academy_cache; ///< last academy read (re-published between reads)
    std::unique_ptr<Out> academy_detail; ///< last ac.d.* read (its own sample)
    bool academy_detail_due{};
    std::unique_ptr<Out> people_cache;   ///< last People read (2 Hz, monastery page only)
    u64 people_age{};
    std::unique_ptr<PeopleJob> people_job; ///< the refresh in progress (StepPeople)
    std::vector<std::array<u8, 0x34>> quest_recs;
    bool quest_recs_ok{}, loc_cells_ok{};
    u64 quest_recs_mgr{}, quest_recs_at{}, loc_cells_mgr{}, loc_cells_at{};
    std::array<std::array<u16, 4>, 100> loc_cells{};
#ifndef NDEBUG
    double people_us{};
    double people_max{};
    int people_n{};
    std::array<double, 8> people_parts{}; ///< max time per PeopleJob step kind
    std::array<int, 2> slow_samples{};    ///< samples > 150 / > 200 us in the cost window
#endif
    std::array<std::size_t, 3> last_counts{}; ///< ref counts of the last sample (reserve)
    s64 gps_map_key{-1};
    u64 gps_map_at{};
    u8 gps_map_type{};
    std::unordered_map<s32, std::array<std::string, 4>> person_keys_cache; ///< name, portrait, dport_s, dport
    u64 person_keys_key{}, person_keys_at{};
    double people_scale{};
    u64 academy_age{};
    u64 samples{};
    std::optional<u64> sampled_tick;
    std::chrono::steady_clock::time_point born{std::chrono::steady_clock::now()}; ///< fe.clock_ds origin
#ifndef NDEBUG
    double cost_sum_us{}, cost_max_us{};
    u64 cost_n{};
    double cost_published{};
    std::array<double, 10> part_us{}; ///< reads, battle, publish, grid (cost log)
#endif

};

} // namespace Fe3hReader
