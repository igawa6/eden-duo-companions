// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Private declarations of the P5R (01005CA01580E000) native module: the shared constants and
// snapshot structs, ArtLibrary (romfs-backed art) and class Reader. Reader's member definitions
// are split by area:
//   p5r_reader_core.cpp       Resolve / Sample / ReadSnapshot / Publish plumbing
//   p5r_reader_party.cpp      party members, camp rosters, member names
//   p5r_reader_persona.cpp    persona stock, skill lists, skill help text
//   p5r_reader_inventory.cpp  items, equipment, party stats, camp-menu cursor mirror
//   p5r_reader_social.cpp     social stats, confidants, calendar, requests, DATA2 extras
//   p5r_reader_battle.cpp     Analyze panel, battle2 lists, battle party personas and skills
//   p5r_reader_map.cpp        field map, overlay, player icon
//   p5r_reader_dialogue.cpp   dialogue text and choices
//   p5r_reader_bgm.cpp        now-playing BGM
//   p5r_reader_drive.cpp      native-menu button drives (pdrv.*)
//   p5r_reader_direct.cpp     direct state changes (guarded guest writes)
//   p5r_reader_actions.cpp    module actions (the dispatcher chain) and companion page tracking
//   p5r_reader_assets.cpp     ArtLibrary, recipe prewarm, game-text load, load_image / decode_font
// 01005CA01580E000.cpp holds the ABI glue and the exported C entry points. The game-structure
// decoders are the header-only p5r_*_reader.h files. The hot accessors (guest reads, publish
// helpers, a few gates) stay defined in the class so that every area file inlines them.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "p5r_analyze_reader.h"
#include "p5r_battle2_reader.h"
#include "p5r_bgm_reader.h"
#include "p5r_data2_reader.h"
#include "p5r_dialogue_reader.h"
#include "p5r_direct_write.h"
#include "p5r_enemy_names.h"
#include "p5r_enemy_reader.h"
#include "p5r_font.h"
#include "p5r_game_text.h"
#include "p5r_inventory_reader.h"
#include "p5r_map_assets.h"
#include "p5r_map_overlay.h"
#include "p5r_map_reader.h"
#include "p5r_persona_reader.h"
#include "p5r_prewarm.h"
#include "p5r_publish_filter.h"
#include "p5r_recipes.h"
#include "p5r_romfs_assets.h"
#include "p5r_social_menu_reader.h"
#include "p5r_social_reader.h"
#include "p5r_task_walk.h"
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace p5r_module {

using namespace dsmod_sdk::int_types;
inline constexpr u64 TitleId = 0x01005CA01580E000;
inline constexpr std::string_view BuildId =
    "D4B150B29A931CD381E2A30318FC299F2B0EE36D000000000000000000000000";
inline constexpr std::array<u8, 32> BuildBytes{0xd4, 0xb1, 0x50, 0xb2, 0x9a, 0x93, 0x1c,
                                               0xd3, 0x81, 0xe2, 0xa3, 0x03, 0x18, 0xfc,
                                               0x29, 0x9f, 0x2b, 0x0e, 0xe3, 0x6d};
inline constexpr size_t PartySize = 4;
inline constexpr u64 UnitStride = 0x2a0;
// Party unit ids 1..10 (protagonist .. Kasumi). Names come from the game's NAME.TBL
// (p5r_game_text.h).
inline constexpr u16 MemberIdLimit = 11;
inline constexpr std::array<const char*, 7> Phases{
    "Early Morning", "Morning", "Lunchtime", "Afternoon", "After School", "Evening", "Late Night"};
inline constexpr std::array<const char*, 7> Weekdays{"SUN", "MON", "TUE", "WED",
                                                     "THU", "FRI", "SAT"};

struct Member {
    bool present{};
    u16 id{}, level{};
    u32 hp{}, sp{};
};
// Camp member lists (D4B1). The native camp menus do not list the active party
// but a roster built when the menu opens:
//   7E3970 (SKILL, ITEM targets, camp main): protagonist, then 2,3,4,5,6,7,9,10 when joined
//          (7E3AE0) or in the party array (880710, units+0x1CE0 u16[10]);
//   7E3BB0 (STATS, EQUIP): protagonist, then 2,3,4,5,6,7,9,10 when joined, then Futaba (8)
//          when joined.
// 7E3AE0 joined test: id 1 always, id 2..10 = BIT_CHK(0x40000030 + k) through the jump table
// main+15E3EAC (2->30 3->31 4->32 5->33 6->34 7->35 8->36 9->37 10->38).
inline constexpr size_t RosterMax = 10;
inline constexpr std::array<u32, 11> JoinFlag{0,          0,          0x40000030, 0x40000031,
                                              0x40000032, 0x40000033, 0x40000034, 0x40000035,
                                              0x40000036, 0x40000037, 0x40000038};
inline constexpr std::array<u16, 8> RosterOrder{2, 3, 4, 5, 6, 7, 9, 10};
// Fingerprinted native routines the rosters and member names reproduce (FNV-1a over the bytes).
struct CodeRange {
    u64 offset, size, hash;
};
inline constexpr std::array<CodeRange, 8> RosterCode{{
    {0x7e3970, 0x168, 0x007cd1b4e69395baULL}, // SKILL/ITEM member list
    {0x7e3bb0, 0x178, 0x7c2d39e5171ccdfcULL}, // STATS/EQUIP member list
    {0x7e3ae0, 0xc8, 0x6fc51abdac70c6c6ULL},  // joined test (flag per member)
    {0x15e3eac, 0xa, 0x9cb3ff1f739879b2ULL},  // its jump table
    {0x880710, 0xb0, 0x002a5859c780e65fULL},  // "in the party array"
    {0x893ff0, 0x40, 0x907e826fcaf42276ULL},  // member name: id 10 -> name 0x20 when 0x849
    {0x8821e0, 0x60, 0x2cec1788391cec43ULL},  // BIT_CHK (section table from its ADRP/ADD)
    {0x899240, 0xfc, 0xa522cf5271bed7c6ULL},  // BIT_CHK's own-answer table (0x4000xxxx)
}};
inline constexpr u32 FlagSumireName = 0x849;
struct Roster {
    bool ready{};
    unsigned count{};
    std::array<Member, RosterMax> m{};
    std::array<u8, RosterMax> party{}; // 2 = leader (slot 0), 1 = party slot 1..3, 0 = reserve
};
// Battle party member: the persona it fights with (its current persona slot, unit+0x40) and
// the native battle SKILL list rows (that persona's non-passive skills in slot order).
struct BattleSkill {
    u16 id{};
    int icon{-1};
    int element{-1}; // 0..9 in aff order, -1 other
    bool cost_hp{};
    s64 cost{-1}; // as the native row prints it; -1 = not reproduced
    std::string name;
};
struct BattleAlly {
    bool present{};
    int roster{-1}, eroster{-1}; // row in the camp rosters (7E3970 / 7E3BB0)
    u16 member{};
    p5r_persona::Persona persona;
    unsigned skill_count{};
    std::array<BattleSkill, 8> skills{};
};
inline const p5r_persona::Snapshot& NoPersona() {
    static const p5r_persona::Snapshot empty{};
    return empty;
}
struct Snapshot {
    bool ready{}, date_ready{};
    bool dialogue{}, menu{}, map_ready{}, field_variant_ready{};
    u8 field_variant{};
    p5r_map::Map map;
    bool overlay_ready{};
    p5r_map_overlay::Frame overlay;
    bool player_rotation_ready{};
    float player_rotation{};
    p5r_enemy::Snapshot battle;
    bool analysis_ready{};
    unsigned analysis_slot{}; // enemy.N row holding the analyzed unit
    p5r_analyze::Frame analysis;
    std::string analysis_name;
    bool battle2_ok{}; // p5r_battle2 lists read (affinities / party / turn object)
    p5r_battle2::Frame battle2;
    std::array<BattleAlly, PartySize> bally{}; // per party slot (party array order)
    p5r_dialogue::Snapshot talk;
    s32 scene{-1};
    u16 field_major{}, field_minor{}, at_dungeon{};
    u32 money{};
    std::string date;
    std::array<Member, PartySize> party{};
    Roster roster;  // 7E3970 list (SKILL / ITEM)
    Roster eroster; // 7E3BB0 list (STATS / EQUIP)
    bool sumire{};  // member 10 is named "Sumire" (893FF0: flag 0x849)
    int roster_stage{
        4}; // diagnostics: 0 ok, 1 code mismatch, 2 flag refused, 3 member unreadable, 4 not read
    p5r_social::Snapshot social;
    // The persona/skill cache (65 KiB, ~1,200 strings) is referenced, not copied, per sample:
    // NoPersona() while no page needs it, else the Reader's persona_cache (unchanged until the
    // next sample's SamplePersona, which runs before any reader of this snapshot).
    const p5r_persona::Snapshot* persona{&NoPersona()};
    std::string skill_desc;
    s64 skill_desc_id{-1};
    // Native-menu persona change driver (button gates for the manifest's enforce rules).
    int drive_button{-1}; // index into DriveButtons, -1 none
    int drive_state{};    // 0 idle, 1 running, 2 done, 3 aborted
    const char* drive_status{"idle"};
    bool inventory_ready{}, party_stats_ready{};
};
// The one pdrv gate channel shared by both native-menu drivers (PublishDrive). Index = button:
// persona/skill driver 0..4, item/equip driver 0..6 (enum DriveButton).
inline constexpr std::array<const char*, 7> DriveButtons{"pdrv.x",    "pdrv.a", "pdrv.b", "pdrv.up",
                                                         "pdrv.down", "pdrv.l", "pdrv.r"};

// Asset-free package (p5r_recipes.h): every "module:p5r:<id>" image is a recipe of
// dualscreen/p5r_art.rec (modules/p5r_art.rec in 0.9.x test packages) replayed over the game's own
// romfs. One instance per Reader; the host calls load_image from one asset worker, the mutex
// covers any second caller (composite layers).
struct ArtLibrary {
    std::mutex lock;
    p5r_assets::Romfs romfs;
    bool romfs_tried{}, book_tried{}, book_ok{};
    p5r_recipes::Book book;
    std::unique_ptr<p5r_recipes::RomfsSources> sources;
    std::unique_ptr<p5r_recipes::Engine> engine;
    unsigned built{}, failed{};
    double build_ms{};
    static void Log(const EdenDsmodHostApi& host, uint32_t level, const std::string& msg) {
        if (host.log)
            host.log(host.userdata, level, msg.c_str());
    }
    bool Romfs(const EdenDsmodHostApi& host);
    bool Ready(const EdenDsmodHostApi& host);
    bool Build(const EdenDsmodHostApi& host, std::string_view id, p5r_recipes::Image& out);
    bool Font(const EdenDsmodHostApi& host, std::vector<uint8_t>& fnt);
    // Prewarm (p5r_prewarm.h): images built ahead by the worker, handed over (moved out) on the
    // host's first request. Both paths hold `lock` around check + build, so no recipe is built
    // twice; ids the host already asked for are never prebuilt.
    static constexpr size_t WarmBudget = size_t(32) << 20;
    std::mutex warm_lock;
    std::unordered_map<std::string, p5r_recipes::Image> warm;
    std::unordered_set<std::string> requested;
    size_t warm_bytes{};
    unsigned warm_hits{};
    // The host's load_image path.
    bool Get(const EdenDsmodHostApi& host, std::string_view id, p5r_recipes::Image& out);
    // One prewarm step; false once the budget is spent or the table is unusable.
    bool Warm(const EdenDsmodHostApi& host, const std::string& id);
};

// One Reader per create(). sample() and on_action() are serialized by the host on its tick
// thread; ArtLibrary is also used from the asset worker (own mutexes), and the text / prewarm
// workers are started by the first sample.
// Class layout: nested types and constants, inline accessors, the member functions grouped by
// the file that defines them, then the data members (their order is the construction order).
class Reader {
public:
    struct TextJob {
        p5r_text::GameText text;
        double ms{};
    };
    // ---- Companion page tracking (lazy outputs) -------------------------------------------
    // The module cannot see the companion page, so the START MENU navigates through module
    // action ui_open(P): the module publishes ui.page = P and the manifest's page_binds on ui.page
    // switch the page. Heavy lists (item rows, equipment, persona stock, skill lists, confidant
    // detail, calendar, requests) are read and published only while their page is open (or a
    // native-menu drive needs them). Any game-state change that fires one of the manifest's own
    // page_binds (state.mode / controls.choice edges) resets ui.page to -1 here as well.
    enum UiPage : int {
        PageNone = -1,
        PageHub = 1,
        PageSkill = 2,
        PageItem = 3,
        PageEquip = 4,
        PagePersona = 5,
        PageStats = 6,
        PageConfidant = 7,
        PageCfDetail = 8,
        PageRequest = 9,
        PageCalendar = 10,
        PageEquipCand = 11,
        PageField = 20,
        PageBattle = 21,
        PageSocial = 22,
        PageAll = 99 // diagnostics/tests: every list published (the pre-0.8 behaviour)
    };

private:
    // Native camp-menu actions, driven only by button presses the manifest's enforce rules send
    // while one pdrv.* gate is 1 (never a memory write). Live D4B1 CAMP_MAIN context fields:
    // ctx+4 sub-state of the open command, ctx+0x34 command cursor (10 entries, wraps; SKILL = 0,
    // PERSONA = 3); SKILL sub-context ctx+0x6C0 (the 80C150 builder's x0): +0x26 member row,
    // +0x28 skill row, +0x2C target row, +0x248 member ids, +0x260 rows, +0x6E0 row count.
    // CHANGE PERSONA: X -> PERSONA -> A -> list (state 4, row u16 ctx+0x4D6A6, opens at 0) -> A
    // equips (no confirmation) -> B -> B (the game then moves the equipped persona to slot 0).
    // USE SKILL: X -> SKILL -> A -> member (state 4) -> A -> skill list (state 6) -> A -> target
    // (state 8 one ally, cursor = party slot; state 10 all allies) -> A uses it immediately (SP
    // drops; nothing happens when no target needs it) -> B x4 closes the menu.
    // One button per step; the next step waits until the observed state has changed and settled.
    struct DriveState {
        bool active{}, used{}, backout{};
        int kind{};              // 0 persona change, 1 use skill
        u16 target{};            // persona id / skill id
        u16 member{};            // skill user member id
        unsigned slot{}, ally{}; // skill user party slot, target party slot
        u32 start_sp{};
        unsigned frames{}, steps{}, settle{}, waiting{}, confirms{};
        std::array<s64, 9> seen{};
        int result{}; // 0 idle, 2 done, 3 aborted
        const char* status{"idle"};
    };
    static constexpr unsigned DriveSettle = 10, DriveRetry = 60, DriveTimeout = 60 * 25,
                              DriveMaxSteps = 64, CampEntries = 10, CampSkill = 0, CampPersona = 3;
    static constexpr u64 SkillSub = 0x6c0;
    struct Camp {
        bool open{};
        s32 state{-1}, command{-1};
        u16 list_row{};
        s16 member_row{}, skill_row{}, target_row{};
    };
    struct MenuKeys {
        std::array<std::array<std::string, 5>, p5r_social::ConfidantSlots> row;
        std::array<std::array<std::string, 8>, p5r_social_menu::MaxAbilities> ability;
        std::array<std::array<std::string, 3>, 32> day;
        std::array<std::array<std::string, 4>, 42> cell;
        std::array<std::array<std::string, 5>, p5r_social_menu::calendar::MaxEvents> event;
        std::array<std::array<std::string, 7>, p5r_social_menu::request::MaxRequests> request;
        MenuKeys() {
            constexpr std::array<const char*, 7> RequestFields{
                "present", "id", "state", "name", "target", "difficulty", "desc"};
            for (size_t k = 0; k < request.size(); ++k)
                for (size_t f = 0; f < RequestFields.size(); ++f)
                    request[k][f] = "request." + std::to_string(k) + "." + RequestFields[f];
            for (size_t c = 0; c < cell.size(); ++c) {
                cell[c][0] = "calendar.cell." + std::to_string(c) + ".day";
                cell[c][1] = "calendar.cell." + std::to_string(c) + ".mark";
                cell[c][2] = "calendar.cell." + std::to_string(c) + ".deadline";
                cell[c][3] = "calendar.cell." + std::to_string(c) + ".holiday";
            }
            for (size_t d = 1; d < day.size(); ++d) {
                day[d][0] = "calendar." + std::to_string(d) + ".mark";
                day[d][1] = "calendar." + std::to_string(d) + ".deadline";
                day[d][2] = "calendar." + std::to_string(d) + ".holiday";
            }
            constexpr std::array<const char*, 5> EventFields{"present", "day", "kind", "active",
                                                             "label"};
            for (size_t k = 0; k < event.size(); ++k)
                for (size_t f = 0; f < EventFields.size(); ++f)
                    event[k][f] = "calendar.event." + std::to_string(k) + "." + EventFields[f];
            constexpr std::array<const char*, 5> RowFields{"detail", "person", "short", "portrait",
                                                           "portrait_key"};
            constexpr std::array<const char*, 8> AbilityFields{
                "present", "name", "desc", "rank", "unlocked", "hidden", "early", "icon"};
            for (size_t i = 0; i < row.size(); ++i)
                for (size_t f = 0; f < RowFields.size(); ++f)
                    row[i][f] = "confidant." + std::to_string(i) + "." + RowFields[f];
            for (size_t i = 0; i < ability.size(); ++i)
                for (size_t f = 0; f < AbilityFields.size(); ++f)
                    ability[i][f] =
                        "confidant.sel.ability." + std::to_string(i) + "." + AbilityFields[f];
        }
    };
    // ---- Native ITEM / EQUIP driver (button presses only; the same pdrv protocol as the
    // persona driver: the module raises one pdrv.<button> gate, the manifest's enforce rules
    // press it). Paths verified live on D4B1: USE ITEM X -> command ITEM (cursor 1) -> A -> tab L/R
    // (6 tabs, wraps) -> row Up/Down (wraps) -> A -> target (state 8, cursor = party slot) -> A
    // (qty drops) -> B until closed. CHANGE EQUIP X -> EQUIP (2) -> A -> member (state 5) -> A ->
    // slot row (state 7, 5 rows) -> A -> candidate list (state 10) -> A (equips) -> B out.
    struct DriveRequest {
        int kind{}; // 1 use item, 2 change equipment
        int tab{-1}, row{-1}, slot{-1};
        u16 id{}, member{};
        u8 qty{};
    };
    struct Drive {
        bool active{}, done{}, backout{};
        DriveRequest req{};
        unsigned frames{}, steps{}, settle{}, waiting{}, confirms{};
        std::array<s64, 10> seen{};
        int result{};
        const char* status{"idle"};
        int button{-1};
    };
    enum DriveButton { BtnX, BtnA, BtnB, BtnUp, BtnDown, BtnL, BtnR };
    static constexpr unsigned ItemDriveSettle = 10, ItemDriveRetry = 60, ItemDriveTimeout = 60 * 30,
                              ItemDriveMaxSteps = 160;
    // ---- Direct state changes (p5r_direct_write.h) ------------------------------------------
    // USE ITEM / CHANGE EQUIPMENT / CHANGE PERSONA reproduced as the exact guest-memory writes
    // the native routines make (no guest calls: works under NCE). Module flag default ON
    // (package "p5r_direct_writes": false, EDEN_DSMOD_P5R_DIRECT_WRITES=0 or the action
    // direct_writes 0 turn it off); anything not reproduced keeps the native-menu drive. A
    // request is applied in one sample, only in field free control (the CanDrive gate plus no
    // camp menu), from values read in that sample and read again right before each write.
    struct DirectRequest {
        bool pending{};
        int kind{};   // 1 use item, 2 change equipment, 3 change persona
        u16 id{};     // item / equipment / persona id
        int slot{-1}; // item: ITEM target row (7E3970, 9 = none); equip: EQUIP row (7E3BB0)
        int row{-1};  // equip: native EQUIP slot row
        u16 member{}; // equip: member id; item: target member id (single)
        unsigned age{};
        DriveRequest native{}; // item / equip: the native drive to run instead (camp menu open)
        s64 persona_row{-1};   // persona: stock row for the native drive
    };
    static constexpr unsigned DirectWait = 90; // samples a request may wait for field control
    // The consumable record (88CF70 table) and its use skill row (89B520), read now.
    struct DirectItem {
        u16 skill{};
        p5r_dwrite::Skill s;
        bool single{};
    };
    struct SocialKeys {
        std::string social_ready{"social.ready"}, confidant_ready{"confidant.ready"},
            confidant_count{"confidant.count"};
        std::array<std::array<std::string, 6>, p5r_social::StatCount> stat;
        std::array<std::array<std::string, 8>, p5r_social::ConfidantSlots> confidant;
        SocialKeys() {
            // goal = points + next: the cumulative threshold of the next rank (== points at rank
            // 5), so a manifest gauge can use points / goal without arithmetic.
            constexpr std::array<const char*, 6> StatFields{"rank", "points", "next",
                                                            "name", "title",  "goal"};
            constexpr std::array<const char*, 8> ConfidantFields{
                "present", "id", "arcana", "rank", "max", "reversed", "broken", "name"};
            for (size_t i = 0; i < stat.size(); ++i)
                for (size_t f = 0; f < StatFields.size(); ++f)
                    stat[i][f] = "social." + std::to_string(i) + "." + StatFields[f];
            for (size_t i = 0; i < confidant.size(); ++i)
                for (size_t f = 0; f < ConfidantFields.size(); ++f)
                    confidant[i][f] = "confidant." + std::to_string(i) + "." + ConfidantFields[f];
        }
    };
    struct PersonaKeys {
        std::string present, id, level, arcana, next, name, arcana_name, skill_count;
        std::array<std::string, p5r_persona::Stats> stat;
        std::array<std::string, p5r_persona::SkillSlots> skill_name, skill_icon;
        std::array<std::string, p5r_persona::Elements> aff;
        explicit PersonaKeys(const std::string& p = {}) {
            present = p + "present";
            id = p + "id";
            level = p + "level";
            arcana = p + "arcana";
            next = p + "next";
            name = p + "name";
            arcana_name = p + "arcana_name";
            skill_count = p + "skill_count";
            constexpr std::array<const char*, p5r_persona::Stats> Stat{"st", "ma", "en", "ag",
                                                                       "lu"};
            for (size_t i = 0; i < stat.size(); ++i)
                stat[i] = p + Stat[i];
            for (size_t i = 0; i < skill_name.size(); ++i) {
                const auto k = p + "skill." + std::to_string(i) + ".";
                skill_name[i] = k + "name";
                skill_icon[i] = k + "icon";
            }
            for (size_t i = 0; i < aff.size(); ++i)
                aff[i] = p + "aff." + std::to_string(i);
        }
    };
    static constexpr size_t SkillRowsPublished = 64; // rows per member (camp SKILL list)
    struct SkillRowKeys {
        std::string count;
        std::array<std::array<std::string, 8>, SkillRowsPublished> rows;
    };
    struct PersonaKeySet {
        std::string persona_ready{"persona.ready"}, persona_count{"persona.count"},
            persona_current{"persona.current"}, derived{"persona.derived"},
            desc_id{"skill.desc.id"}, desc{"skill.desc"};
        std::array<PersonaKeys, p5r_persona::StockSlots> persona;
        PersonaKeys det{"persona.det."}; // the persona.sel row (one detail panel)
        std::string det_shown{"persona.det.shown"};
        std::array<PersonaKeys, RosterMax> ally;
        std::array<SkillRowKeys, RosterMax> skill_rows;
        PersonaKeySet() {
            constexpr std::array<const char*, 8> Row{"id",      "",     "icon",   "cost",
                                                     "cost_hp", "name", "usable", "grey"};
            for (size_t m = 0; m < skill_rows.size(); ++m) {
                const auto p = "skill." + std::to_string(m) + ".";
                skill_rows[m].count = p + "count";
                for (size_t r = 0; r < SkillRowsPublished; ++r)
                    for (size_t f = 0; f < Row.size(); ++f)
                        skill_rows[m].rows[r][f] = p + std::to_string(r) + "." + Row[f];
            }
            for (size_t i = 0; i < persona.size(); ++i)
                persona[i] = PersonaKeys{"persona." + std::to_string(i) + "."};
            for (size_t i = 0; i < ally.size(); ++i)
                ally[i] = PersonaKeys{"ally." + std::to_string(i) + ".persona."};
        }
    };
    static constexpr unsigned Battle2AffPeriod = 4;
    static constexpr unsigned SocialPeriod = 8;
    static constexpr unsigned PersonaPeriod = 8;
    static constexpr unsigned InventoryPeriod = 30, InvSlowPeriod = 6;
    static constexpr unsigned DriveResultShown = 600;

public:
    /// Write extension (ConfigureWrite): persona stock rotations are stored as one batch.
    void SetWriteApi(const EdenDsmodHostWriteApi& h) {
        write_api = h;
        write_api_ok = true;
    }
    explicit Reader(const EdenDsmodHostApi& api) : host{api} {}
    ~Reader() {
        prewarm_stop = true;
        if (prewarm_thread.joinable())
            prewarm_thread.join();
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    const EdenDsmodHostApi& Host() const {
        return host;
    }
    bool On(int page) const {
        return ui_page == page || ui_page == PageAll;
    }

private:
    // A host without is_mapped reads anyway: read_memory bounds-checks by itself
    // (dsmod_sdk::MissingIsMapped::AssumeMapped).
    bool ReadBytes(u64 at, void* out, size_t size) const {
        return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::AssumeMapped>(host, at, out, size);
    }
    template <typename T>
    bool Read(u64 at, T& out) const {
        return ReadBytes(at, &out, sizeof(T));
    }
    template <typename T>
    bool ReadAt(u64 base, u64 offset, T& out) const {
        return base && offset <= std::numeric_limits<u64>::max() - base && Read(base + offset, out);
    }
    bool InMain(u64 at, size_t size) const {
        return dsmod_sdk::InMain(host.main_base, host.main_size, at, size);
    }
    void F64(const char* name, double value) const {
        if (!publish_filter.Wants(name))
            return;
        if (host.publish_f64)
            host.publish_f64(host.userdata, name, value);
    }
    // A native-menu drive can start: field control (or the camp menu already open) and no drive.
    bool CanDrive(const Snapshot& out) const {
        return out.ready && out.scene == 4 && !out.dialogue && out.battle.state_known &&
               !out.battle.active && !DriveBusy();
    }
    // The native camp SKILL confirm (812990) accepts the row: the member can pay for it
    // (873680 + 840D00, p5r_persona FieldSkill::usable). HP-cost skills are not offered by
    // skill_use. The native list itself never dims a row.
    static bool SkillUsable(const Snapshot& out, size_t m, const p5r_persona::FieldSkill& row) {
        return out.roster.ready && m < out.roster.count && !row.cost_hp && row.cost >= 0 &&
               row.usable == 1;
    }

public:
    void SetDirectWrites(bool on) {
        direct_enabled = on;
    }

private:
    const std::string& CachedName(u16 id) const {
        static const std::string empty;
        const auto it = inv_names.find(id);
        return it == inv_names.end() ? empty : it->second;
    }
    const std::string& CachedPersona(u16 id) const {
        static const std::string empty;
        const auto it = inv_persona_names.find(id);
        return it == inv_persona_names.end() ? empty : it->second;
    }
    const std::string& CachedHelp(u16 id) const {
        static const std::string empty;
        const auto it = inv_help.find(id);
        return it == inv_help.end() ? empty : it->second;
    }
    // Exactly one native-menu drive runs at a time (DriveBusy refuses a second start), and this
    // is the only writer of the pdrv.* gates: one gate per sample at most. pdrv.state/status/
    // kind describe the driver that was started last. pdrv.kind: 0 none, 1 persona change,
    // 4 skill use (persona driver); 2 item use, 3 equip change (item/equip driver).
    bool DriveBusy() const {
        return drive.active || idrv.active || dreq.pending;
    }
    // const char* overloads: the hot item/equip rows format into a stack buffer, no temporaries.
    void I64(const char* name, s64 value) const {
        if (!publish_filter.Wants(name))
            return;
        if (host.publish_i64)
            host.publish_i64(host.userdata, name, value);
    }
    void Text(const char* name, const char* value) const {
        if (!publish_filter.Wants(name))
            return;
        if (host.publish_text)
            host.publish_text(host.userdata, name, value);
    }
    void I64(const std::string& name, s64 value) const {
        if (!publish_filter.Wants(name.c_str()))
            return;
        if (host.publish_i64)
            host.publish_i64(host.userdata, name.c_str(), value);
    }
    void Text(const std::string& name, const char* value) const {
        if (!publish_filter.Wants(name.c_str()))
            return;
        if (host.publish_text)
            host.publish_text(host.userdata, name.c_str(), value);
    }

public:
    // ---- resolve, sample and publish plumbing (p5r_reader_core.cpp) ----
    void Sample(const EdenDsmodHostApi& api);

private:
    bool Global(u64 pc, u32 adrp, u32 access, u32 expected_access, unsigned scale, u64& out) const;
    bool Resolve();
    void ResolveText();
    bool Scene(u64& context, u32& flags, s32& scene) const;
    bool ReadSnapshot(Snapshot& out) const;
    bool Field(u64& context, u16& field_major, u16& field_minor, u16& at_dungeon) const;
    u64 FindTask(std::string_view wanted, u64 target = 0) const;
    void ReadDate(Snapshot& out) const;
    void Publish(const Snapshot& out) const;
    // ---- party members and the camp rosters (p5r_reader_party.cpp) ----
    bool ReadMember(u16 id, Member& member) const;
    std::optional<bool> GameFlag(u32 flag) const;
    void ReadRosters(const std::array<u16, RosterMax>& party_array, Snapshot& out,
                     bool lists) const;
    void PublishParty(const Snapshot& out, const std::string& hero) const;
    void PublishRosters(const Snapshot& out, const std::string& hero) const;
    const char* MemberName(u16 id, const std::string& hero, bool sumire) const;
    // ---- persona stock and skills (p5r_reader_persona.cpp) ----
    bool NeedPersona() const;
    void SamplePersona(Snapshot& next);
    s64 SelectedSkillId(const p5r_persona::Snapshot& p) const;
    const p5r_persona::Skill* SelectedSkill(const p5r_persona::Snapshot& p) const;
    void PublishPersona(const Snapshot& out) const;
    // ---- items, equipment and party stats (p5r_reader_inventory.cpp) ----
    void SampleInventory(Snapshot& out);
    template <class Inv>
    void CacheText(Inv& inv, u16 id);
    bool NeedInventory() const;
    void FollowItemSelection();
    void PublishInventory(const Snapshot& out) const;
    // ---- social stats, confidants, calendar and requests (p5r_reader_social.cpp) ----
    void PublishSocial(const Snapshot& out) const;
    void RefreshData2();
#ifdef P5R_DATA2_TRACE
    void TraceData2();
#endif
    void PublishJobs(const char* prefix, const p5r_data2::Jobs& j, bool ok) const;
    void PublishData2(const Snapshot& out) const;
    void RefreshMenu();
    void PublishMenu(const Snapshot& out, bool list) const;
#ifdef P5R_MENU_TRACE
    void TraceMenu();
#endif
    void PublishRequests(bool ready) const;
    void PublishCalendar(bool ready) const;
    // Calendar month order of the in-game year: April 0 .. March 11 (-1 = not a month).
    static int MonthOrder(int month) {
        return month >= 1 && month <= 12 ? (month + 8) % 12 : -1;
    }
    // ---- battle analysis, battle party and turn/target (p5r_reader_battle.cpp) ----
    void SampleBattle(Snapshot& out) const;
    void RecheckAnalysis(Snapshot& out) const;
    void PublishBattle(const Snapshot& out) const;
    void SampleBattleAllies(Snapshot& next);
    template <class Lister>
    s64 BattleHpCost(const Lister& lister, u16 member, u64 entry, u16 skill, u32 hp_max,
                     bool& cost_hp) const;
    template <class Lister>
    bool RandomCostRecord(const Lister& lister, u64 entry, bool& out) const;
    const p5r_inventory::Member* BattleStats(u16 id) const;
    void PublishBattle2(const Snapshot& out, bool battle_ready, const std::string& hero) const;
    // ---- field map and overlay (p5r_reader_map.cpp) ----
    void SampleMap(Snapshot& out, u64 field_context) const;
    void PublishOverlay(bool ready, const p5r_map_overlay::Frame& o) const;
    void PublishMap(const Snapshot& out, bool map_ready) const;
    // ---- dialogue and choices (p5r_reader_dialogue.cpp) ----
    bool Dialogue(p5r_dialogue::Snapshot& talk) const;
#ifdef P5R_DIALOGUE_TRACE
    void Trace(const p5r_dialogue::Snapshot& t) const;
#endif
    void PublishDialogue(const Snapshot& out) const;
    // ---- now-playing BGM (p5r_reader_bgm.cpp) ----
    void SampleBgm(bool ready);
    void PublishBgm() const;
    // ---- native-menu drives (p5r_reader_drive.cpp) ----
    void Abort(const char* why);
    bool CampState(Camp& c) const;
    void DriveMenu(Snapshot& next);

public:
    bool StartChange(s64 argument, bool allow_direct = true);
    bool StartSkill(s64 argument);

private:
    void DriveAbort(const char* why);
    void RunDrive(const Snapshot& out);
    void PublishDrive(const Snapshot& out) const;
    // ---- direct state changes (p5r_reader_direct.cpp) ----
    bool DirectAvailable() const;
    bool DirectGate(const Snapshot& out) const;
    bool DirectWrite(u64 at, const void* expect, const void* value, size_t size);
    void LogDirectWrite(u64 at, const void* expect, const void* value, size_t size);
    template <typename T>
    bool DirectWriteValue(u64 at, T expect, T value);
    bool DirectItemRule(u16 id, DirectItem& d, const char*& why);
    bool DirectUnit(u16 member, p5r_dwrite::Unit& u);
    u64 CountAddress(u16 id) const;
    void DirectFinish(bool ok, const char* why);
    bool StartDirect(const DirectRequest& r);
    void RunDirect(const Snapshot& out);
    void DirectToNative();
    void DirectItemUse(const Snapshot& out);
    void DirectEquip(const Snapshot& out);
    void DirectPersona();

public:
    // ---- companion-side selections and page tracking (p5r_reader_actions.cpp) ----
    bool DispatchAction(const char* action, s64 argument);
    static bool ValidPage(s64 p);
    void OpenPage(int page);
    bool UiAction(const char* name, s64 argument);
    bool ItemAction(const char* name, s64 argument);

private:
    static int ModeOf(const Snapshot& out);
    static bool ChoiceOf(const Snapshot& out);
    void TrackPage(const Snapshot& next);

public:
    bool OnAction(std::string_view action, s64 argument);
    bool Data2Action(const char* name, s64 argument);
    bool Action(const char* name, s64 argument);
    // ---- romfs art, prewarm and game text (p5r_reader_assets.cpp) ----
    static std::unique_ptr<TextJob> LoadText(ArtLibrary& art, EdenDsmodHostApi api);
    void EnsureText();
    void StartPrewarm();
    void TakeText(std::unique_ptr<TextJob> job);
    // Asset-free package: the game's own romfs (p5r_romfs_assets decoders + p5r_recipes). Touched
    // only by LoadImage / DecodeFont; ArtLibrary serializes its own state.
    ArtLibrary art;
    // Game-text tables (arcana / member / stat names, music titles) read from the same romfs.
    // Loaded once on a worker thread started by the first sample (the ~120-190 ms romfs TOC +
    // NAME.TBL/CMM/MYP/ACB reads used to stall that sample); taken over by the sampling thread
    // when finished, then read-only. Until then (and when unreadable) every name output stays
    // empty (fail closed).
    p5r_text::GameText text;
    bool text_tried{};
    // Hosts that attach a package manifest (create() config with "pages") get the worker-thread
    // load; a bare config (unit tests pass "{}") keeps the synchronous first-sample load.
    bool text_async{};
    // Recipe prewarm (p5r_prewarm.h): ids from create()'s manifest; started once the text worker
    // has opened the romfs, at the lowest scheduling priority, stopped by the destructor.
    std::vector<std::string> prewarm_ids;
    std::atomic<bool> prewarm_stop{};
    std::thread prewarm_thread;
    // Declared after `art` and `text`: destroyed (joined) before them.
    std::future<std::unique_ptr<TextJob>> text_job;

private:
#ifdef P5R_DATA2_TRACE
    std::string last_data2_trace;
#endif
#ifdef P5R_MENU_TRACE
    std::string last_menu_trace;
#endif
    EdenDsmodHostApi host{};
    EdenDsmodHostWriteApi write_api{}; ///< write extension (copied in ConfigureWrite)
    bool write_api_ok{};

public:
    // Indexed output families the installed package never names are not published
    // (p5r_publish_filter.h); configured once from create()'s manifest.
    p5r_publish::Filter publish_filter;

private:
    mutable p5r_tasks::Walk task_walk; // this sample's task-list walk (ReadSnapshot)
    bool battle2_code{};               // p5r_battle2::Code fingerprints matched
    mutable unsigned battle2_age{Battle2AffPeriod};
    mutable p5r_battle2::Frame battle2_last{};
    mutable u64 battle2_ctx{};
    bool battle_seen{};         // previous sample was in a ready battle (roster rows needed)
    bool battle_stats_wanted{}; // battle party lacks hp_max/sp_max: ask for one stats pass
    std::array<BattleAlly, PartySize> bally_cache{};
    std::array<u16, PartySize> bally_ids{};
    unsigned bally_age{PersonaPeriod};
    bool resolved{}, social_resolved{};
    bool bgm_resolved{};
    p5r_bgm::Roots bgm_roots;
    p5r_bgm::Tracker bgm;
    p5r_bgm::Frame bgm_frame;
    s64 bgm_logged_cue{-1}, bgm_logged_change{};
    p5r_social::Roots social_roots{};
    p5r_social::Snapshot social_cache{};
    unsigned social_age{SocialPeriod};
    SocialKeys keys;
    PersonaKeySet persona_keys;
    bool persona_resolved{}, selection_dirty{};
    p5r_persona::Roots persona_roots{};
    p5r_persona::Snapshot persona_cache{};
    std::array<u16, RosterMax> persona_party{};
    unsigned persona_age{PersonaPeriod};
    s64 selection{-1}, desc_cache_id{-1}, skill_sel{-1}, persona_sel{-1};
    DriveState drive;
    std::array<Member, PartySize> last_party{};
    Roster last_roster{};
    std::string desc_cache;
    bool menu_resolved{};
    p5r_social_menu::Roots menu_roots{};
    bool roster_code{}; // RosterCode fingerprints matched
    u64 roster_flags{}; // BIT_CHK section table (main+1D834C8 in D4B1)
    mutable bool roster_published{};
    std::array<p5r_social_menu::Row, p5r_social::ConfidantSlots> menu_rows{};
    p5r_social_menu::Detail menu_detail{};
    unsigned menu_request{}, menu_selected{};
    MenuKeys menu_keys;
    bool calendar_resolved{};
    p5r_social_menu::calendar::Roots calendar_roots{};
    p5r_social_menu::calendar::Month calendar_cache{};
    bool request_resolved{};
    p5r_social_menu::request::Roots request_roots{};
    p5r_social_menu::request::List request_cache{};
    // DATA2 extras (p5r_data2_reader.h)
    bool data2_resolved{};
    p5r_data2::Roots data2_roots{};
    p5r_data2::Stats data2_stats{};
    p5r_data2::Jobs data2_today{}, data2_sel{};
    p5r_social_menu::calendar::Month data2_sel_month{};
    p5r_data2::DailyLog data2_log{}; // the selected day's Daily Log (past days only)
    p5r_data2::Requests data2_req{};
    s64 data2_sel_date{-1}; // calendar_select: month*100+day, -1 = today
    int data2_req_tab{};    // request_tab: 0 Recent, 1 Progress, 2 Difficulty
    bool last_ready{};
    s32 last_scene{-2};
    u64 money{}, units{}, date_slot{}, scene_slot{};
    u64 field_slot{}, field_token_slot{};
    u64 message_table{}, message_index_slot{};
    p5r_dialogue::Globals text_globals{};
    unsigned inv_slow_age{InvSlowPeriod};
    bool inv_party_ok{};
    int inv_stage{1};
    unsigned inv_retry{};
    std::array<u16, RosterMax> inv_ids{};
#ifdef P5R_INV_TRACE
    bool inv_trace_pending{};
#endif
    bool inv_resolved{}, inv_lists_ok{};
    unsigned inv_age{};
    p5r_inventory::Roots inv_roots{};
    p5r_inventory::RecordCache inv_cache{};
    std::vector<u8> inv_counts;
    std::array<p5r_inventory::Tab, p5r_inventory::MenuTabs> inv_tabs{};
    std::array<p5r_inventory::Member, RosterMax> inv_party{};
    std::unordered_map<u16, std::string> inv_names, inv_help, inv_persona_names;
    int inv_view_tab{-1};
    s64 inv_select{-1}, inv_equip_select{-1};
    bool inv_cand_ok{};
    size_t inv_cand_total{}; // native candidate rows (inv_cand keeps the first 40)
    p5r_inventory::MenuState inv_menu{};
    Drive idrv{};
    DirectRequest dreq{};
    bool direct_enabled{true}; // p5r_direct_writes (default ON)
    bool last_direct{};        // the last finished action was a direct write
    int ui_page{PageNone}, last_mode{-1};
    bool ui_reopen{};
    unsigned drive_quiet{DriveResultShown};
    bool last_choice{};
    s64 persona_follow{-1};
    u16 inv_select_id{};
    int drive_owner{}; // 0 none yet, 1 persona/skill driver, 2 item/equip driver (last started)
    std::vector<p5r_inventory::Candidate> inv_cand;
#ifdef P5R_DIALOGUE_TRACE
    mutable std::string last_trace;
#endif
};

// Extension entry points (p5r_reader_assets.cpp); the tables are in 01005CA01580E000.cpp.
EdenDsmodBool LoadImage(void* p, const EdenDsmodHostApi* host, const char* key, void* receiver,
                        EdenDsmodImageSink sink);
EdenDsmodBool DecodeFont(void* p, const uint8_t* bytes, size_t size, void* receiver,
                         EdenDsmodFontSink sink);

} // namespace p5r_module
