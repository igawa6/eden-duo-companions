// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem: Three Houses 1.2.0 reader (fe3h_reader.h): code pins (fe3h_pins.inc) and the globals decoded from them.

#include "fe3h_reader_internal.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string_view>
#include <unordered_set>

namespace Fe3hReader {

namespace {
#include "fe3h_pins.inc"
} // namespace

// ---- code checks and global decoding --------------------------------------------------------
bool Reader::CheckPin(const Pin& pin) {
    const u64 bytes = pin.words.size() * 4;
    if (pin.offset + bytes > size)
        return false;
    std::vector<u32> live(pin.words.size());
    return Read(base + pin.offset, live.data(), bytes) &&
           std::equal(live.begin(), live.end(), pin.words.begin());
}
bool Reader::CheckHash(const HashPin& pin) {
    if (pin.offset + pin.size > size || pin.size > 0x4000)
        return false;
    std::vector<u8> live(pin.size);
    if (!Read(base + pin.offset, live.data(), live.size()) ||
        dsmod_sdk::Fnv1a64(live.data(), live.size()) != pin.fnv)
        return false;
    hash_bytes[pin.offset] = std::move(live);
    return true;
}
/// A code word the reader may decode: inside a word pin or a verified hash pin.
std::optional<u32> Reader::CodeWord(u64 off) const {
    for (const Pin* p : AllWordPins)
        if (off >= p->offset && off + 4 <= p->offset + p->words.size() * 4 && (off & 3) == 0)
            return p->words[(off - p->offset) / 4];
    for (const auto& [start, bytes] : hash_bytes)
        if (off >= start && off + 4 <= start + bytes.size()) {
            u32 w;
            std::memcpy(&w, bytes.data() + (off - start), 4);
            return w;
        }
    return std::nullopt;
}
/// ADRP Xd + LDR Xt,[Xd,#imm] (unsigned offset): the GOT slot's value (a main address).
u64 Reader::GotTarget(u64 adrp_off, u64 ldr_off) const {
    const auto a = CodeWord(adrp_off), l = CodeWord(ldr_off);
    if (!a || !l || (*a & 0x9f000000u) != 0x90000000u || (*l & 0xffc00000u) != 0xf9400000u ||
        ((*l >> 5) & 31u) != (*a & 31u))
        return 0;
    const u64 imm = ((*a >> 29) & 3u) | (static_cast<u64>((*a >> 5) & 0x7ffffu) << 2);
    const s64 page = static_cast<s64>(adrp_off & ~u64{0xfff}) + SignExtend(imm, 21) * 4096;
    if (page < 0)
        return 0;
    const u64 slot = base + static_cast<u64>(page) + ((*l >> 10) & 0xfffu) * 8;
    if (!InMain(slot, 8))
        return 0;
    const auto v = Get<u64>(slot);
    return v && InMain(*v, 1) ? *v : 0;
}
/// ADRP Xd + ADD Xt,Xd,#imm: the object's address.
u64 Reader::AdrpAddTarget(u64 adrp_off, u64 add_off) const {
    const auto a = CodeWord(adrp_off), d = CodeWord(add_off);
    if (!a || !d || (*a & 0x9f000000u) != 0x90000000u || (*d & 0xff800000u) != 0x91000000u ||
        ((*d >> 5) & 31u) != (*a & 31u))
        return 0;
    const u64 imm = ((*a >> 29) & 3u) | (static_cast<u64>((*a >> 5) & 0x7ffffu) << 2);
    const s64 page = static_cast<s64>(adrp_off & ~u64{0xfff}) + SignExtend(imm, 21) * 4096;
    const u64 add = (*d >> 22) & 1 ? u64{(*d >> 10) & 0xfffu} << 12 : (*d >> 10) & 0xfffu;
    if (page < 0)
        return 0;
    const u64 at = base + static_cast<u64>(page) + add;
    return InMain(at, 1) ? at : 0;
}

bool Reader::Resolve() {
    resolved = true;
    core_ok = battle_ok = academy_ok = false;
    code_diag.clear();
    core_diag.clear();
    battle_diag.clear();
    academy_diag.clear();
    gps_diag.clear();
    talk_diag.clear();
    quests_diag.clear();
    details_diag.clear();
    cal_diag.clear();
    hash_bytes.clear();
    text_cache.clear();
    entry_cache.clear();
    base = host.main_base;
    size = host.main_size;
    if (test_globals) {
        core_ok = battle_ok = academy_ok = stats_ok = gps_ok = talk_ok = affil_ok = quests_ok = details_ok = clsicon_ok = overview_ok = drive_ok = goals_ok = skills_ok = info_ok = lmag_ok = cal_ok = mk_ok = true;
    } else {
        if (!base || !size) {
            code_diag = core_diag = battle_diag = academy_diag = "no main image";
            return false;
        }
        std::unordered_set<std::string_view> ok;
        std::string failed;
        for (const Pin* p : AllWordPins) {
            if (CheckPin(*p))
                ok.insert(p->name);
            else
                failed += std::string{failed.empty() ? "" : ","} + p->name;
        }
        for (const HashPin* p : AllHashPins) {
            if (CheckHash(*p))
                ok.insert(p->name);
            else
                failed += std::string{failed.empty() ? "" : ","} + p->name;
        }
        // every missing pin of a domain is named in that domain's diagnostics
        const auto all = [&](std::initializer_list<const char*> names, std::string& why) {
            bool good = true;
            for (const char* n : names)
                if (!ok.contains(n)) {
                    good = false;
                    why += std::string{why.empty() ? "pin " : ","} + n;
                }
            return good;
        };
        const bool core_pins =
            all({"AppMain", "AppStateNext", "AppStateRun", "AppScene", "InGameVtable", "SeqRead", "BattleUnits", "SaveCard",
                 "Money", "EBase", "Difficulty", "ChapterCtx", "MonthName", "Day", "ActExplore",
                 "ActTeach", "ActBattle", "GetName", "GetHelp", "FieldState", "ChapterRecord",
                 "ProfLevel", "RankText", "Weekday", "CalendarRecord", "WeekdayText",
                 "ChapterTitle", "ChapterTitleUse", "RouteLabel", "HudDate", "HudDateDigits",
                 "MonthNumber", "Medal", "MedalTable0", "MedalTable1", "DigitSprites",
                 "ActExploreMax", "ActTeachMax", "ActBattleMax", "ExploreBonus"},
                core_diag);
        std::string names_why;
        const bool names_pins =
            all({"PersonName", "NameSubst", "ClassName", "PartTwo", "NameTags"}, names_why);
        const bool battle_pins =
            all({"GetUnit", "Cursor", "MapSize", "UnderCursor", "Turn", "Phase", "ForecastUnits",
                 "ForecastArmy", "FcWindow", "FcDraw", "WinActive", "ForecastA", "ForecastB", "FcCount",
                 "UnitHp", "UnitPos", "UnitLarge", "UnitActed", "Level", "Stats", "Arts",
                 "ArtName", "ArtText", "Abilities", "AbilityName", "AbilityText", "PersonCtor",
                 "ItemUses", "ItemMax", "MapId", "ScStage", "MinimapObj", "MinimapFlag",
                 "ItemName", "ItemRecord", "MaxHp", "HpBase", "HpClass", "HpBars", "CursorTile",
                 "ScMap", "UnitSlot", "UnitClassName", "StatClass", "StatClamp", "StatMov",
                 "FcHpAfterA", "FcHpAfterB", "FcItemA", "FcItemB", "FcItemPane", "FcArtLine",
                 "ItemNamePtr", "ItemUsesMax", "ArtId", "UnitSize", "DangerToggle", "DangerBuild", "DangerDraw",
                 "MoveClear", "ItemIcon", "BrokenWeapon", "GreyBacking", "DetailProfile", "AffilFlag",
                 "AffilFlagTable", "MoveType", "MoveTypeJt", "ExpNext", "ExpShown", "UnitCrest",
                 "CrestIcon", "StatColour", "StatCap", "StatCapRoster", "StatCapBonus", "StatsBlock", "StatBar"},
                battle_diag);
        const bool academy_pins = all({"FindPerson", "SkillRank", "MotivationCtor", "Level",
                                       "Stats", "Mastered", "Roster", "HpBase", "StatClass", "StatClamp",
                                       "StatMov", "SkillThreshold", "GoalText"},
                                      academy_diag);
        if (!names_why.empty()) {
            battle_diag += (battle_diag.empty() ? "" : ";") + names_why;
            academy_diag += (academy_diag.empty() ? "" : ";") + names_why;
        }
        core_ok = core_pins;
        battle_ok = core_pins && names_pins && battle_pins;
        academy_ok = core_pins && names_pins && academy_pins;
        (void)failed;

        Globals g;
        g.app = GotTarget(0x512038, 0x51203c);
        if (const u64 vt = GotTarget(0x513930, 0x513934); vt) {
            const auto add = CodeWord(0x513944); // add x8, x8, #0x10
            if (add && (*add & 0xffc00000u) == 0x91000000u)
                g.ingame_vptr = vt + ((*add >> 10) & 0xfffu);
        }
        g.g = GotTarget(0x54c90c, 0x54c910);
        g.unit_var = GotTarget(0x54c924, 0x54c928);
        g.ui_var = GotTarget(0x1751b8, 0x1751bc); // forecast window object variable
        g.grid_var = GotTarget(0xa9acc, 0xa9ad0);
        g.s_var = GotTarget(0x3e119c, 0x3e11a0);
        g.text = GotTarget(0x3f7e54, 0x3f7e58);
        g.f_var = GotTarget(0x52fe4, 0x52fe8);
        g.sc = GotTarget(0x10a170, 0x10a174);
        g.minimap = GotTarget(0x596f64, 0x596f68);
        g.pd_var = GotTarget(0x40da54, 0x40da58);
        g.name_buf = AdrpAddTarget(0x40da20, 0x40da24);
        g.subst_var = GotTarget(0x40dc44, 0x40dc48);
        g.class_var = GotTarget(0x40e060, 0x40e064);
        g.monster_var = GotTarget(0x40e0c4, 0x40e0c8);
        g.hpbonus_var = GotTarget(0xc357c, 0xc3580);
        g.prof_var = GotTarget(0x3e1cb0, 0x3e1cb8);
        g.dlc_var = GotTarget(0x308f68, 0x308f74);
        g.chapter_var = {GotTarget(0x3d8630, 0x3d8634), GotTarget(0x3d865c, 0x3d8660),
                         GotTarget(0x3d8694, 0x3d8698), GotTarget(0x3d86c8, 0x3d86cc),
                         GotTarget(0x3d86f4, 0x3d86f8)};
        g.item_var = {GotTarget(0x40c494, 0x40c498), GotTarget(0x40c4d0, 0x40c4d4),
                      GotTarget(0x40c518, 0x40c51c), GotTarget(0x40c560, 0x40c564),
                      GotTarget(0x40c5a8, 0x40c5ac), GotTarget(0x40c5f0, 0x40c5f4),
                      GotTarget(0x40c638, 0x40c63c)};
        g.art_var = GotTarget(0xea90c, 0xea910);
        g.cal_var = {GotTarget(0x40b02c, 0x40b030), GotTarget(0x40afc4, 0x40afc8),
                     GotTarget(0x40b09c, 0x40b0a0), GotTarget(0x40b104, 0x40b108),
                     GotTarget(0x40b16c, 0x40b170)};
        g.skill_var = GotTarget(0x3d4ac4, 0x3d4ac8);
        g.equip_obj = GotTarget(0x413384, 0x413388);
        g.class_abil_var = GotTarget(0x40e010, 0x40e014);
        g.personal_var = GotTarget(0x40e534, 0x40e538);
        g.battalion_var = GotTarget(0xb04f0, 0xb04f4);
        g.ability_var = GotTarget(0x216ad4, 0x216adc);
        g.actor_var = GotTarget(0x202b4c, 0x202b50);
        g.area_var = GotTarget(0x202d34, 0x202d38);
        g.field_kind = GotTarget(0x30b97c, 0x30b980);
        g.tm_var = GotTarget(0x46ded0, 0x46ded4);
        g.t_var = GotTarget(0x5e92e0, 0x5e92e4);
        g.quest_var = GotTarget(0x1df078, 0x1df07c);
        g.loccell_var = GotTarget(0x2b5b54, 0x2b5b64);
        g.color_table = GotTarget(0x5c6100, 0x5c6104);
        g.lang_var = GotTarget(0x2b5418, 0x2b5420);
        g.scr_obj = GotTarget(0x3fcf50, 0x3fcf54);
        g.bonus_var = GotTarget(0x3ed924, 0x3ed930);
        g.exp_var = GotTarget(0x40f4b8, 0x40f4bc);
        g.status_var = GotTarget(0xb046c, 0xb0470);
        g.weight_var = GotTarget(0xc5164, 0xc5168);
        g.gambit_var = GotTarget(0xc52bc, 0xc52c4);
        g.fog_ox = GotTarget(0x9cd14, 0x9cd20);
        g.goal_var = GotTarget(0x3d48f8, 0x3d48fc);
        g.strength_var = GotTarget(0x3f35b8, 0x3f35bc);
        g.strength_found = GotTarget(0x3f3778, 0x3f377c);
        g.talent_var = GotTarget(0x66425c, 0x664260);
        g.pspell_var = GotTarget(0x40df54, 0x40df58);
        g.pspell_generic = GotTarget(0x40dfe4, 0x40dfe8);
        g.fog_oz = GotTarget(0x9cd28, 0x9cd34);
        g.hud_obj_var = GotTarget(0x200a4c, 0x200a50);
        g.material_var = GotTarget(0x422830, 0x422834);
        g.secret_var = GotTarget(0x2f4260, 0x2f4264);
        g.present_var = GotTarget(0x2b04bc, 0x2b04cc);
        g.tourney_vals = {GotTarget(0x2f41a4, 0x2f41ac), GotTarget(0x2f41a0, 0x2f41a8),
                          GotTarget(0x2f41b4, 0x2f41c0), GotTarget(0x2f41bc, 0x2f41c4)};
        g.count_vals = {GotTarget(0x4228b8, 0x4228c0), GotTarget(0x4228bc, 0x4228c4),
                        GotTarget(0x422898, 0x4228a0), GotTarget(0x42289c, 0x4228a4),
                        GotTarget(0x4228d8, 0x4228e0), GotTarget(0x4228dc, 0x4228e4),
                        GotTarget(0x4228f8, 0x422900), GotTarget(0x4228fc, 0x422904),
                        GotTarget(0x422918, 0x422920), GotTarget(0x42291c, 0x422924),
                        GotTarget(0x422938, 0x422940), GotTarget(0x42293c, 0x422944),
                        GotTarget(0x42296c, 0x422974), GotTarget(0x422970, 0x422978)};
        // Calendar (round 22)
        g.fixdate_var = GotTarget(0x424918, 0x42491c);
        g.deadline_var = GotTarget(0x42420c, 0x424210);
        g.calev_var = {GotTarget(0x1d0a04, 0x1d0a08), GotTarget(0x1d0a80, 0x1d0a84),
                       GotTarget(0x1d0afc, 0x1d0b00), GotTarget(0x1d0b78, 0x1d0b7c)};
        g.tourney_var = GotTarget(0x40b770, 0x40b774);
        g.giver_var = GotTarget(0x3f11ac, 0x3f11b0);
        g.cal_dlc_var = GotTarget(0x3f10fc, 0x3f1100);
        g.cal_actor_var = GotTarget(0x1d0110, 0x1d0114);
        g.cal_field_var = GotTarget(0x1d0150, 0x1d0154);
        if (const u64 fk = GotTarget(0x1d015c, 0x1d0160); fk)
            g.cal_field_kind = fk + 4; // add x8, x8, #4
        g.cal_flag_var = GotTarget(0x1d0204, 0x1d0208);
        g.cal_dlc_flags = GotTarget(0x1d0260, 0x1d0264);
        gl = g;
        using Named = std::pair<const char*, u64>;
        const auto need = [&](bool& dom, std::initializer_list<Named> vals, std::string& why) {
            for (const auto& [name, v] : vals)
                if (!v) {
                    dom = false;
                    why += std::string{why.empty() ? "global " : ","} + name;
                }
        };
        need(core_ok,
             {{"app", g.app}, {"ingame_vptr", g.ingame_vptr}, {"g", g.g}, {"s", g.s_var},
              {"text", g.text}, {"f", g.f_var}, {"prof", g.prof_var},
              {"chapter1", g.chapter_var[0]}, {"chapter2", g.chapter_var[1]},
              {"chapter0", g.chapter_var[2]}, {"chapter5", g.chapter_var[3]},
              {"chapterX", g.chapter_var[4]}, {"calendar0", g.cal_var[0]},
              {"calendar1", g.cal_var[1]}, {"calendar2", g.cal_var[2]},
              {"calendar3", g.cal_var[3]}, {"calendar4", g.cal_var[4]}},
             core_diag);
        need(battle_ok,
             {{"unit_list", g.unit_var}, {"fc_window", g.ui_var}, {"grid", g.grid_var}, {"sc", g.sc}, {"minimap", g.minimap},
              {"persondata", g.pd_var}, {"name_buf", g.name_buf}, {"subst", g.subst_var},
              {"class", g.class_var}, {"monster", g.monster_var}, {"hpbonus", g.hpbonus_var},
              {"item0", g.item_var[0]}, {"item1", g.item_var[1]}, {"item2", g.item_var[2]},
              {"item3", g.item_var[3]}, {"item4", g.item_var[4]}, {"item5", g.item_var[5]},
              {"item6", g.item_var[6]}, {"art", g.art_var}, {"ability", g.ability_var}},
             battle_diag);
        need(academy_ok,
             {{"persondata", g.pd_var}, {"name_buf", g.name_buf}, {"subst", g.subst_var},
              {"dlc", g.dlc_var}, {"class", g.class_var}, {"skill", g.skill_var}},
             academy_diag);
        stats_ok = all({"StatRoutine", "ModPass", "AbilityEvalHead", "AbilityTimingJt",
                        "AbilityCondJt", "EquipSlot", "PersonalAbility", "ClassAbilities",
                        "BrokenWeapon", "ItemRecord"},
                       names_why) &&
                   g.equip_obj && g.class_abil_var && g.personal_var && g.battalion_var;
        battle_ok = battle_ok && core_ok;
        academy_ok = academy_ok && core_ok;
        details_ok = stats_ok &&
                     all({"CombatBlock", "CombatKinds", "CalcCombat", "AsBase", "GambitUses",
                          "BattalionBonus", "Range", "AbilitiesRow", "AbilityIcon", "InventoryPage",
                          "SpellKnown", "SpellUses", "ArtsPage", "ArtIcon", "BattalionPage", "BattalionIcon",
                          "BattalionTier", "BattalionExp", "SkillBox", "SkillCell", "SkillCatIcon",
                          "SkillIconFramed", "SkillIconPlain", "SkillPulse", "ClassCell", "ClassRecord",
                          "SpellMax", "SpellMaxKinds", "EffectSum", "ExpHidden", "ExpAnimal", "ProfileTail",
                          "Adjutant", "AdjutantIcon", "BattalionTri", "AnnaDlcForm"},
                         details_diag);
        overview_ok = all({"Overview", "OverviewCountJt", "UnitShown", "DefeatIndex", "DefeatJt",
                           "DefeatAltTable", "ClassicMode", "BattleTitle", "QuestTitle", "FindPerson", "FogTileSize"},
                          battle_diag) &&
                      g.fog_ox && g.fog_oz;
        drive_ok = all({"BattleState", "FreeCursorUpdate", "Cursor", "Phase", "MapSize", "FcWindow",
                        "WinActive"},
                       battle_diag);
        goals_ok = all({"GoalSkills", "GoalsRow", "GoalSkillCell", "MotivationGauge"}, academy_diag) &&
                   g.goal_var;
        skills_ok = all({"StrengthArrow", "TalentRecord", "TalentProgress", "TalentFound", "SkillsRow",
                         "TalentStars"},
                        academy_diag) &&
                    g.strength_var && g.strength_found && g.talent_var;
        lmag_ok = all({"PersonSpells", "LearnedMagic", "ClassMagic"}, academy_diag) && g.pspell_var && g.pspell_generic;
        info_ok = all({"FullName", "PersonAge", "PersonHeight", "AgeHidden", "StatusPersonal"}, academy_diag);
        cal_ok = core_ok && names_pins &&
                 all({"CalInit", "CalMonthBuild", "CalQuestDays", "CalDayHelpers", "CalAbsent",
                      "CalDayEvents", "CalAnnaShop", "CalGrid", "CalWeekdayCell", "CalSundayCell",
                      "CalEventRow", "CalEventRowJt", "CalActivityRow", "CalSelOffset", "CalPanel",
                      "CalTourney", "CalFreeIndexJt", "CalOngoing", "CalNoteDay",
                      "CalFreeIndex", "CalRecBits", "CalRecFlag0", "EventFlag", "QuestState", "RouteIs5",
                      "FlagBits10C5", "DaysInMonth", "LeapYear", "IconMap", "IconMapJt", "Crest",
                      "CrestJt", "Text14ED", "Text16E1", "Text1939", "QuestName", "QuestDesc",
                      "ActivityTexts", "ActivityIcons", "QuestAvail", "RewardIconRule",
                      "MaterialIcons", "ColorTable", "CalendarRecord", "Weekday", "WeekdayText",
                      "FindPerson", "ItemName", "ItemRecord", "ItemIcon"},
                     cal_diag);
        need(cal_ok,
             {{"fixdate", g.fixdate_var}, {"deadline", g.deadline_var}, {"calev0", g.calev_var[0]},
              {"calev1", g.calev_var[1]}, {"calev2", g.calev_var[2]}, {"calev3", g.calev_var[3]},
              {"tourney", g.tourney_var}, {"giver", g.giver_var}, {"dlc", g.cal_dlc_var},
              {"actor", g.cal_actor_var}, {"field", g.cal_field_var}, {"field_kind", g.cal_field_kind},
              {"flag_obj", g.cal_flag_var}, {"dlc_flags", g.cal_dlc_flags},
              {"color_table", g.color_table}, {"material", g.material_var}, {"present", g.present_var}},
             cal_diag);
        std::string mk_why;
        mk_ok = all({"TextEscapes", "TextEscapeJt", "TextColourReset", "TextColourPick", "ColorTable"}, mk_why) &&
                g.color_table;
        std::string clsicon_why;
        clsicon_ok = all({"ClassCell", "PersonRecordOf", "PartTwoOf"}, clsicon_why);
        need(details_ok,
             {{"status", g.status_var}, {"weight", g.weight_var}, {"gambit", g.gambit_var},
              {"battalion", g.battalion_var}, {"equip", g.equip_obj}, {"item5", g.item_var[5]}},
             details_diag);
        quests_ok = core_ok && names_pins &&
                    all({"QuestTitle", "QuestTextRow", "QuestTextSel", "HudText", "HudColTable",
                         "ScrRow", "ScrCell", "ScrColSizes", "JournalCmp", "JournalBuild",
                         "MonthOnly", "RewardCount", "RewardCountJt", "Rewards", "SecretsCount",
                         "BattalionText", "HudBuilder", "ObjText", "LostBuilder", "LostCount",
                         "MissionSummary", "HudObjSite", "QuestState", "ItemName", "ItemRecord"},
                        quests_diag);
        need(quests_ok,
             {{"scr", g.scr_obj}, {"hud_obj", g.hud_obj_var}, {"material", g.material_var},
              {"secrets", g.secret_var}, {"present", g.present_var}, {"quest", g.quest_var},
              {"battalion", g.battalion_var}, {"tourney0", g.tourney_vals[0]},
              {"tourney1", g.tourney_vals[1]}, {"tourney2", g.tourney_vals[2]},
              {"tourney3", g.tourney_vals[3]}},
             quests_diag);
        for (const u64 v : g.count_vals)
            if (!v)
                quests_ok = false;
        gps_ok = core_ok && names_pins &&
                 all({"PeopleCall", "PeopleBuilder", "FacilityNpc", "FacilityJt", "NpcVar",
                      "EventFlag", "MapIs13", "MapIs46", "MinimapTex", "LocTracker",
                      "LocNameText", "LocNameChooser", "LocPopupText", "PersonRecord",
                      "ArrowPos", "MarkerFilter", "MarkerSprite", "EventCheck", "QuestGate",
                      "QuestIcon", "QuestStateIcon", "QuestOffers", "QuestTargets", "QuestState",
                      "QuestLocGlow", "QuestLocLayer", "LocCentre", "CellToWorld", "TalkspotLayer",
                      "TalkspotPerson", "MapSpecial", "TalkspotSlots", "TalkspotLocs", "ListMarks", "LocContains", "WorldToCell", "CellSize",
                      "ListMarksCall", "ListMarksDraw", "FloorColorIdx", "ColorTable", "LangByte"},
                     gps_diag);
        need(gps_ok,
             {{"actor", g.actor_var}, {"area", g.area_var}, {"field_kind", g.field_kind},
              {"dlc", g.dlc_var}, {"persondata", g.pd_var}, {"quest", g.quest_var},
              {"loccell", g.loccell_var}, {"color_table", g.color_table},
              {"lang", g.lang_var}},
             gps_diag);
        // a message window open (fe.mode 3)
        talk_ok = core_ok && all({"TalkIdle", "TalkWinT", "TalkT"}, talk_diag);
        need(talk_ok, {{"tm", g.tm_var}, {"t", g.t_var}}, talk_diag);
        // the affiliation of a person record (the Status book's profile)
        std::string affil_why;
        affil_ok = all({"Affiliation", "AffilText"}, affil_why);
        if (!core_ok && battle_diag.empty())
            battle_diag = "core";
        if (!core_ok && academy_diag.empty())
            academy_diag = "core";
        for (const auto& [dom, why] : {std::pair{"core", &core_diag}, {"battle", &battle_diag},
                                       {"academy", &academy_diag}, {"gps", &gps_diag},
                                       {"talk", &talk_diag}, {"quests", &quests_diag},
                                       {"details", &details_diag}})
            if (!why->empty())
                code_diag += std::string{code_diag.empty() ? "" : "; "} + dom + ": " + *why;
    }
    persons_tbl = {gl.pd_var, 0x70A0};
    class_tbl = {gl.class_var, 0x968};
    monster_tbl = {gl.monster_var, 0x2D8};
    hpbonus_tbl = {gl.hpbonus_var, 0x200};
    prof_tbl = {gl.prof_var, 0xF8};
    art_tbl = {gl.art_var, 0x788};
    ability_tbl = {gl.ability_var, 0x17F0};
    skill_tbl = {gl.skill_var, 0x128};
    class_abil_tbl = {gl.class_abil_var, 0x968};
    personal_tbl = {gl.personal_var, 0x440};
    battalion_tbl = {gl.battalion_var, 0x12C8};
    status_tbl = {gl.status_var, 0x908};
    goal_tbl = {gl.goal_var, 0x12C8};
    strength_tbl = {gl.strength_var, 0x440};
    pspell_tbl = {gl.pspell_var, 0x440};
    gambit_tbl = {gl.gambit_var, 0x788};
    quest_tbl = {gl.quest_var, 0xE30};
    loccell_tbl = {gl.loccell_var, 0x968};
    material_tbl = {gl.material_var, 0x14F0};
    secret_tbl = {gl.secret_var, 0x1E8};
    present_tbl = {gl.present_var, 0x1700};
    for (std::size_t i = 0; i < cal_tbl.size(); ++i)
        cal_tbl[i] = {gl.cal_var[i], 0x968};
    fixdate_tbl = {gl.fixdate_var, 0x12C8};
    deadline_tbl = {gl.deadline_var, 0x968};
    tourney_tbl = {gl.tourney_var, 0x4B8};
    giver_tbl = {gl.giver_var, 0x2D8};
    for (std::size_t i = 0; i < calev_tbl.size(); ++i)
        calev_tbl[i] = {gl.calev_var[i], 0x4B8};
    cal_fixed = {};
    for (auto& t : chapter_tbl)
        t = {0, 0x230};
    for (std::size_t i = 0; i < chapter_tbl.size(); ++i)
        chapter_tbl[i].var = gl.chapter_var[i];
    static constexpr std::array<u64, 7> ItemHdr{0x2EE8, 0x398, 0x4B8, 0x12C8, 0x50, 0x788, 0x5A8};
    for (std::size_t i = 0; i < item_tbl.size(); ++i)
        item_tbl[i] = {gl.item_var[i], ItemHdr[i]};
    return core_ok;
}

void Reader::UseGlobalsForTest(const Globals& g) {
    gl = g;
    test_globals = true;
    resolved = false;
}

} // namespace Fe3hReader
