// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "p3p_reader.h"
#include "p3p_font.h"
#include "p3p_art.h"
#include "p3p_map.h"
#include "p3p_help.h"
#include "p3p_drive.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

static void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
static void Require(bool value, const std::string& why) { if (!value) throw std::runtime_error(why); }
int main() {
    try {
        auto api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
        Require(api != nullptr, "ABI entry point");
        Require(!eden_dsmod_get_module(99, 0), "wrong ABI must be refused");
        Require(api->supports_build("663052C2C7E8C357BA6EAD36641EE2792397EB1F000000000000000000000000"), "supported build");
        Require(!api->supports_build("663052C2C7E8C357"), "short build must be refused");
        Require(!(api->capabilities & (EDEN_DSMOD_CAP_WRITE_MEMORY | EDEN_DSMOD_CAP_GUEST_CALL)), "read-only capabilities");
        EdenDsmodHostApi host{};
        host.title_id = UINT64_C(0x0100DCD01525A000);
        const unsigned char build[]{0x66,0x30,0x52,0xc2,0xc7,0xe8,0xc3,0x57,0xba,0x6e,0xad,0x36,0x64,0x1e,0xe2,0x79,0x23,0x97,0xeb,0x1f};
        std::memcpy(host.build_id, build, sizeof(build));
        std::map<std::string,int64_t> output;
        host.userdata = &output;
        host.is_mapped = [](void*,uint64_t,uint64_t)->EdenDsmodBool{return EDEN_DSMOD_FALSE;};
        host.read_memory = [](void*,uint64_t,void*,size_t)->EdenDsmodBool{return EDEN_DSMOD_FALSE;};
        host.publish_i64 = [](void* p,const char* key,int64_t v){(*static_cast<decltype(output)*>(p))[key]=v;};
        host.publish_text = [](void*,const char*,const char*){};
        void* instance = api->create(&host,"{}"); Require(instance, "create");
        // Mirrors the host's cleared-every-tick publication contract. In particular,
        // skipping nine out of ten samples must not silently reintroduce flicker.
        for (int tick=0;tick<100;++tick) {
            output.clear(); api->sample(instance,&host);
            Require(output.contains("live.ready") && output.at("live.ready")==0, "publish unavailable on every tick");
            Require(output.at("member.0.present")==0, "clear stale member visibility");
        }
        auto ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
        Require(ext && ext->on_action && (api->capabilities & EDEN_DSMOD_CAP_EXTENSIONS), "action extension");
        Require(ext->on_action(instance, "ui_open", 8) && !ext->on_action(instance, "ui_open", 9), "page range");
        Require(ext->on_action(instance, "ui_open", 10) && !ext->on_action(instance, "ui_open", 11), "map page");
        Require(ext->on_action(instance, "ui_open", 8), "back to calendar");
        Require(ext->on_action(instance, "member_select", 9) && !ext->on_action(instance, "member_select", 10), "member range");
        Require(!ext->on_action(instance, "press", 0), "unknown actions refused");
        Require(ext->on_action(instance, "persona_select", 11) && !ext->on_action(instance, "persona_select", 12), "persona pick range");
        Require(ext->on_action(instance, "item_tab", 2) && !ext->on_action(instance, "item_tab", 3), "item tab range");
        output.clear(); api->sample(instance, &host);
        Require(output.at("ui.page") == 11 && output.at("battle.active") == 0 && output.at("ui.member") == 9 && output.at("skill.ready") == 0, "start page before a save is loaded; state published while unavailable");
        Require(p3p::Maximum(101,{0x237},{},false)==202,"HP gain");
        Require(p3p::Maximum(101,{0x237},{},true)==51,"SP loss rounds down before subtract");
        Require(p3p::Maximum(101,{0x238},{},false)==51,"HP loss");
        Require(p3p::Maximum(101,{0x238},{},true)==202,"SP gain");
        Require(p3p::Maximum(101,{0x219,0x21a,0x21b},{},false)==161,"independent percentage rounding");
        Require(p3p::Maximum(900,{0x219},{{0x1e,2}},false)==999,"cap after equipment");
        Require(p3p::Maximum(5000,{},{},false,true)==5000,"enemies are not capped at 999");
        // 0x74a10 cost rules. Record: flags, ?, ?, type, u16 cost, u16 extra.
        auto rec = [](unsigned flags, unsigned type, unsigned cost, unsigned extra) {
            p3p::Bytes e(0x28); e[0] = flags; e[3] = type; e[4] = cost & 255; e[5] = cost >> 8; e[6] = extra & 255; e[7] = extra >> 8; return e; };
        Require(p3p::SkillCost(rec(0, 2, 3, 0), false, 130, 90, {}) == 3, "flat SP");
        Require(p3p::SkillCost(rec(0, 2, 3, 0), true, 130, 90, {}) == 0, "cost-free unit");
        Require(p3p::SkillCost(rec(0, 2, 3, 0), false, 130, 90, {0x235}) == 1, "Spell Master halves");
        Require(p3p::SkillCost(rec(0, 2, 1, 0), false, 130, 90, {0x235}) == 1, "halving keeps minimum 1");
        Require(p3p::SkillCost(rec(0, 2, 0, 0), false, 130, 90, {}) == 0, "zero SP stays free");
        Require(p3p::SkillCost(rec(0x10, 2, 10, 0), false, 130, 95, {}) == 9, "percent SP floors");
        Require(p3p::SkillCost(rec(0, 1, 7, 0), false, 130, 90, {}) == 9, "HP percent floors");
        Require(p3p::SkillCost(rec(0, 1, 7, 0), false, 130, 90, {0x236}) == 4, "Arms Master halves");
        Require(p3p::SkillCost(rec(0, 1, 0, 0), false, 130, 90, {}) == 1, "HP cost minimum 1");
        Require(p3p::SkillCost(rec(0, 1, 7, 0), false, 130, 90, {0x235}) == 9, "Spell Master ignores HP");
        Require(p3p::SkillCost(rec(0, 0, 7, 0), false, 130, 90, {}) == 0, "untyped skill is free");
        // 0x108770 list order and protagonist duplicate merging.
        std::map<int, p3p::Bytes> table{{0x13, rec(0, 2, 4, 0)}, {0x3d, rec(0, 2, 5, 0)}, {0xc0, rec(8, 2, 4, 0)},
                                        {0xc9, rec(8, 2, 3, 0)}, {0xd2, rec(8, 2, 6, 0)}, {0x70, rec(0, 1, 10, 0)}};
        auto record = [&](int id) { return table.at(id); };
        p3p::Snapshot snap; snap.stock_capacity = 6;
        p3p::Member hero; hero.id = 1; hero.base_hp = 130; hero.base_sp = 90;
        snap.personas.push_back({7, 8, 0, true, {0x13, 0x3d, 0xc9, 0xd2, 0xc0}});
        auto ids = [](const std::vector<p3p::Skill>& list) { std::vector<int> r; for (auto& s : list) r.push_back(s.id); return r; };
        Require(ids(p3p::SkillList(snap, hero, record)) == std::vector<int>{0xc0, 0xc9, 0xd2, 0x13, 0x3d}, "native sort: 0xc0-0xff first");
        snap.personas.push_back({46, 2, 1, false, {0x13, 0x70}});
        auto merged = p3p::SkillList(snap, hero, record);
        Require(ids(merged) == std::vector<int>{0xc0, 0xc9, 0xd2, 0x13, 0x3d, 0x70}, "stock skills merged once");
        Require(merged[3].cost_known && merged[3].cost == 4, "equal duplicate costs stay known");
        Require(merged[5].type == 1 && merged[5].cost == 13, "HP cost uses that Persona's maximum");
        snap.personas[1].skills = {0x70, 0x236};
        snap.personas.push_back({50, 3, 2, false, {0x70}});
        auto find = [](const std::vector<p3p::Skill>& list, int id) { return *std::find_if(list.begin(), list.end(), [&](auto& s) { return s.id == id; }); };
        auto tie = p3p::SkillList(snap, hero, record);
        Require(!find(tie, 0x70).cost_known, "battle-effect tie with differing costs is unknown");
        Require(tie.back().id == 0x236 && tie.back().type == 0, "passives listed last without cost");
        snap.personas[2].skills = {0x70, 0x235};
        auto spell = p3p::SkillList(snap, hero, record);
        Require(find(spell, 0x70).cost_known && find(spell, 0x70).cost == 13, "Spell Master copy wins");
        snap.personas.push_back({60, 3, 4, false, {0xc0}});
        Require(p3p::SkillList(snap, hero, record).size() == 8, "slots beyond the present count are skipped like 0x1d6c60's loop");
        p3p::Member ally; ally.id = 2; ally.base_hp = 111; ally.base_sp = 104; ally.skills = {0x3d, 0xc0};
        Require(ids(p3p::SkillList(snap, ally, record)) == std::vector<int>{0xc0, 0x3d}, "ally lists own Persona only");
        // 0x1d7f30 / 0x1d7e70 / 0x1ddb30 rules.
        p3p::Bytes acc(0x38); acc[0x18] = 2;                          // Ma field
        Require(p3p::AccessoryBonus(acc, {}, 1) == 2 && p3p::AccessoryBonus(acc, {}, 0) == 0, "accessory field per stat");
        Require(p3p::AccessoryBonus(acc, {{0x0c, 1}, {0x85, 1}}, 1) == 2 + 2 + 10, "stat effects rank and x10");
        Require(p3p::AccessoryBonus(acc, {{0x8a, 2}}, 4) == 6, "all-stat effects");
        p3p::Bytes per(0x34); per[0x1c] = 98; per[0x21] = 0xff; per[0x26] = 3;
        Require(p3p::PersonaStat(per, 0, 0) == 99, "clamp at 99");
        per[0x1c] = 3; per[0x26] = 0;
        Require(p3p::PersonaStat(per, 0, 0) == 2, "signed bonus");
        Require(p3p::PersonaStat(per, 0, 300) == 46, "accessory bonus truncated to a signed byte");
        per[0x1c] = 0;
        Require(p3p::PersonaStat(per, 0, 0) == 0, "clamp at 0");
        Require(p3p::SocialRank(0, {10, 20, 30, 40, 50, 60}) == 1 && p3p::SocialRank(10, {10, 20, 30, 40, 50, 60}) == 2, "rank thresholds are cumulative");
        Require(p3p::SocialRank(29, {10, 20, 30, 40, 50, 60}) == 2 && p3p::SocialRank(500, {10, 20, 30, 40, 50, 60}) == 6, "rank cap");
        // 0x1d09f0 and the three cmpItem.c builders.
        p3p::Bytes counts(p3p::ItemIds), flags(0x100);
        std::vector<p3p::u32> type(p3p::ItemIds), category(p3p::ItemIds);
        counts[3] = 2; category[3] = 0x100;                       // Equip, second pass
        counts[7] = 5; category[7] = 0x800;                       // Equip, first pass (mask 0x802)
        counts[9] = 1; category[9] = 0x10 | 0x400;                // passes 0x401 and bit 4: listed twice
        counts[0x210] = 1; counts[0x205] = 1;                     // Equip, ID order after the passes
        counts[0x572] = 2; counts[0x501] = 3; counts[0x700] = 1;  // Use: 0x500..0x5ff then 0x690..0x78f
        counts[0x650] = 1;                                        // Key
        type[0x401] = 5; flags[0x46cc - 0x4640] = 0x02;           // flag-bit item: bit 1 = ID 0x401
        counts[0x401] = 9;                                        // ignored for type 5
        using List = std::vector<std::pair<int, int>>;
        Require(p3p::ItemList(1, counts, flags, type, category) == List{{7, 5}, {3, 2}, {9, 1}, {9, 1}, {0x205, 1}, {0x210, 1}, {0x401, 1}},
                "Equip tab order, duplicates and flag-bit counts");
        Require(p3p::ItemList(0, counts, flags, type, category) == List{{0x501, 3}, {0x572, 2}, {0x700, 1}}, "Use tab ranges");
        Require(p3p::ItemList(2, counts, flags, type, category) == List{{0x650, 1}}, "Key tab range");
        // ID 0x3ff: bit -1, word (-1 + 31) >> 5 = 0, shift 31 (0x1d09f0's csel/asr/lsr).
        Require(p3p::ItemCount(counts, flags, 0x3ff, 5) == 0, "negative flag index reads word 0");
        flags[0x46cc - 0x4640 + 3] = 0x80;
        Require(p3p::ItemCount(counts, flags, 0x3ff, 5) == 1, "negative flag index uses bit 31 of word 0");
        // Persona details rules.
        Require(p3p::PersonaExpForLevel(3, 1) == 126, "Orpheus Lv 3 needs 126 (native NEXT EXP 23 at 103)");
        Require(p3p::PersonaExpForLevel(1, 1) == 0 && p3p::PersonaExpForLevel(150, 1) == p3p::PersonaExpForLevel(99, 1), "level clamps");
        Require(p3p::AffinityLabel(0x14).empty() && p3p::AffinityLabel(0x800) == "Wk" && p3p::AffinityLabel(0x1002) == "Str", "weak and resist");
        Require(p3p::AffinityLabel(0x100) == "Nul" && p3p::AffinityLabel(0x200) == "Rpl" && p3p::AffinityLabel(0x400) == "Abs", "null, repel, absorb");
        p3p::Bytes plain(0x34); plain[0x1c] = 98; plain[0x26] = 5;
        Require(p3p::PersonaStatPlain(plain, 0) == 99, "plain stat cap");
        // Time labels (0xd27c0) and holidays (0x1d2e30).
        Require(std::string(p3p::TimeLabel(5, false, false)) == "Evening" && std::string(p3p::TimeLabel(5, true, false)) == "Dark Hour", "evening and dark hour");
        Require(std::string(p3p::TimeLabel(3, false, true)) == "Daytime" && std::string(p3p::TimeLabel(3, false, false)) == "Afternoon", "holiday afternoon");
        Require(std::string(p3p::TimeLabel(3, true, true)) == "Daytime" && !p3p::TimeLabel(7, false, false), "holiday precedence and range");
        Require(p3p::MonthDay(0) == std::pair{4, 1} && p3p::MonthDay(30) == std::pair{5, 1} && p3p::MonthDay(364) == std::pair{3, 31}, "calendar");
        Require(p3p::Holiday(4, {}) && p3p::Holiday(11, {}) && !p3p::Holiday(5, {}), "Sundays (April 5)");
        Require(p3p::Holiday(28, {{4, 29}}) && !p3p::Holiday(27, {{4, 29}}), "holiday table");
        // Moon phase (0xd5dc0 / 0xd5de0) and weekday.
        p3p::Bytes moon(0x200, 0); moon[38 - 4] = 14; moon[0] = 3;
        Require(p3p::MoonPhase(moon, 2) == 3 && p3p::MoonPhase(moon, 38) == 14, "phase index clamps below day 5");
        Require(p3p::DaysToFullMoon(moon, 26) == 12 && p3p::DaysToFullMoon(moon, 38) == 0, "days to full moon");
        Require(p3p::Weekday(4) == 0 && p3p::Weekday(0) == 3 && p3p::MonthStart(1) == 30, "weekday and month start");
        {
            // Map: only seen floor cells are drawn; walls on closed sides; exit diamond once seen.
            p3p::FloorMap m;
            m.kind[4 * 16 + 10] = m.kind[4 * 16 + 11] = m.kind[5 * 16 + 2] = 1;
            m.sides[4 * 16 + 10] = 8; m.sides[4 * 16 + 11] = 2; // a corridor open between the two
            m.seen[4 * 16 + 10] = m.seen[4 * 16 + 11] = 0xff;
            m.exit_x = 2; m.exit_y = 5;
            auto px = [&](const p3p_art::Image& img, int x, int y) { return img.rgba[(size_t(y) * img.w + x) * 4 + 3]; };
            auto img = p3p_map::Render(m, true);
            // Footprint columns 2..11, rows 4..5: 10 wide -> 86 px cells, centred vertically.
            auto f = p3p_map::Fit(m);
            const int c = f.cell;
            Require(c == 86 && f.ox == -2 * c + 2 && f.oy == (864 - 2 * c) / 2 - 4 * c, "footprint fit");
            auto at = [&](int cx, int cy) { return std::pair{f.ox + cx * c + c / 2, f.oy + cy * c + c / 2}; };
            auto [ax, ay] = at(10, 4); auto [bx, by] = at(11, 4); auto [ex, ey] = at(2, 5);
            Require(img.w == 864 && px(img, ax, ay) && px(img, bx, by), "seen cells drawn");
            Require(!px(img, ex, ey), "unseen cell stays hidden");
            Require(p3p_map::Centre(m, 2, 5).x == ex && p3p_map::Centre(m, 2, 5).y == ey, "cell centre");
            Require(img.rgba[((size_t(f.oy + 4 * c) * img.w) + ax) * 4] == 106, "closed north side is a wall");
            Require(img.rgba[((size_t(ay) * img.w) + f.ox + 11 * c) * 4] == 255, "open side between cells has no wall");
            m.x = 8500; m.z = 3200; m.forward_x = 1; m.forward_z = 0;
            auto k = p3p_map::Leader(m);
            Require(k.x == f.ox + 957 && k.y == f.oy + 387 && k.degrees == 90, "leader marker east (cell 11.1, 4.5)");
            m.forward_x = 0; m.forward_z = 1;
            Require(p3p_map::Leader(m).degrees == 180, "leader marker south (+z)");
            p3p::FloorMap n = m; n.x = 0;
            Require(p3p_map::SamePicture(m, n) && !p3p_map::SamePicture(m, p3p::FloorMap{}), "position does not redraw");
        }
        {
            // Help text (MSG1): page 0 flattened, function codes and glyph pairs dropped.
            p3p::Bytes b(0x20 + 8 + 32 + 0x40); std::memcpy(&b[8], "MSG1", 4); b[0x18] = 1;
            b[0x24] = 8;  // dialog at +0x28: name[24], pages 1, speaker, page offset
            b[0x28 + 24] = 1; b[0x28 + 28] = 8 + 32;
            const char text[] = "\xF2\x05\xFF\xFFHeals 1\nally.\x81\x40";
            std::memcpy(&b[0x48], text, sizeof text);
            std::vector<std::string> out;
            Require(p3p_help::ParseMsg(b, out) && out.size() == 1 && out[0] == "Heals 1 ally.", "help text");
            p3p_help::Tables t; t.armor = {"", "", "Wool."}; t.skill_card = {"", "Card."};
            Require(t.Item(0x202) == "Wool." && t.Item(0x691) == "Card." && t.Item(0x203).empty(), "help item ranges");
        }
        {
            // Message window (p3p_talk.h): a message with two pages and a speaker, and a selection
            // whose second option prints a number (not reproduced) unless the mask hides it.
            p3p::Bytes b(0x38);
            std::memcpy(&b[8], "MSG1", 4); b[0x18] = 2;
            auto put32 = [&](size_t at, uint32_t v) { for (int k = 0; k < 4; ++k) b[at + k] = uint8_t(v >> (8 * k)); };
            auto append = [&](std::string_view s) { size_t at = b.size(); b.insert(b.end(), s.begin(), s.end()); b.push_back(0); return uint32_t(at - 0x20); };
            const size_t message = b.size(); b.resize(b.size() + 40);
            b[message + 24] = 2; // pages 2, speaker 0
            put32(message + 28, append("\xF2\x05\xFF\xFF\xF1\x41Hi, \xF1\x82.\n\x80\x80\n\xF1\x21"));
            put32(message + 32, append("\xF1\x25Teacher\nDone, \xF1\x83!"));
            const size_t select = b.size(); b.resize(b.size() + 48);
            b[select + 26] = 3;
            put32(select + 32, append("\xF2\x05\xFF\xFF\xF1\x41Yes"));
            put32(select + 36, append("\xF2\x44\x01\x01 yen"));
            put32(select + 40, append("\x80\xAEo"));
            const uint32_t name = append("Mitsuru");
            const size_t names = b.size(); b.resize(b.size() + 4); put32(names, name);
            put32(0x20, 0); put32(0x24, uint32_t(message - 0x20)); put32(0x28, 1); put32(0x2c, uint32_t(select - 0x20));
            put32(0x30, uint32_t(names - 0x20)); put32(0x34, 1);
            const p3p_talk::Names who{"Shiomi", "Kotone", "Kotone Shiomi"};
            p3p_talk::Window w{0x1000, 0x1000 + message, 0, 2, 0, -1, 0, 0, 0};
            auto t = p3p_talk::Parse(b, w, who);
            Require(t.text_ok && t.text == "Hi, Kotone." && t.speaker == "Mitsuru" && !t.choosing, "talk page 0");
            w.page = 1; t = p3p_talk::Parse(b, w, who);
            Require(t.text == "Done, Kotone Shiomi!" && t.speaker == "Teacher", "talk inline speaker and full name");
            w.page = 2; Require(p3p_talk::Parse(b, w, who).text == "Done, Kotone Shiomi!", "finished message keeps its last page");
            w.state = 2; w.selection = 1; w.mask = 2; w.shown = 2; w.cursor = 1;
            t = p3p_talk::Parse(b, w, who);
            Require(t.choosing && t.choices_ok && t.choices == std::vector<std::string>{"Yes", "No"} && t.selected == 1,
                    "talk choices skip hidden options");
            w.mask = 0; w.shown = 3; t = p3p_talk::Parse(b, w, who);
            Require(t.choosing && !t.choices_ok && t.choices.empty() && t.selected == -1, "talk number fails choices closed");
            w.shown = 2; bool threw = false;
            try { p3p_talk::Parse(b, w, who); } catch (const std::exception&) { threw = true; }
            Require(threw, "talk shown count must match the file");
            std::string out;
            Require(!p3p_talk::Decode(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("\x81\x40"), 3), who, out) &&
                    !p3p_talk::Decode(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("\xF1\x83"), 3), {}, out),
                    "talk glyphs and missing names fail closed");
        }
        {
            // Item drive (p3p_drive.h) against a model of the camp menu that reacts to presses.
            struct Menu {
                bool open{}; p3p::CampMenu c; int sp_full{};
                void Press(p3p_drive::Button b) {
                    using B = p3p_drive::Button;
                    if (!open) { if (b == B::X) { open = true; c = {}; c.state = 2; c.input = 1; c.commands = {0, 1, 2, 3, 4, 5, 6}; } return; }
                    if (c.state == 2) {
                        if (b == B::Down) ++c.cursor; else if (b == B::Up) --c.cursor;
                        else if (b == B::A && c.commands[c.cursor] == 1) {
                            c.state = 4; c.sub = 3; c.tab = 1; c.targets = {1, 2, 5};
                            c.use_list = {{0x501, 3}, {0x502, 1}, {0x504, 5}, {0x509, 1}, {0x520, 1}, {0x521, 2}};
                        } else if (b == B::B) open = false;
                        return;
                    }
                    if (c.sub == 3) {
                        const int at = c.row + c.scroll;
                        if (b == B::L) c.tab = std::max(0, c.tab - 1);
                        else if (b == B::Down) { if (c.row < 4) ++c.row; else ++c.scroll; }
                        else if (b == B::Up) { if (c.row > 0) --c.row; else --c.scroll; }
                        else if (b == B::A && at == 2) c.sub = 4;
                        else if (b == B::B) { c = {}; c.state = 2; c.input = 1; c.cursor = 1; c.commands = {0, 1, 2, 3, 4, 5, 6}; }
                        return;
                    }
                    if (b == B::Down) ++c.target; else if (b == B::Up) --c.target;
                    else if (b == B::A && !(sp_full >> c.target & 1)) --c.use_list[2].second;
                    else if (b == B::B) c.sub = 3;
                }
            };
            auto run = [](Menu& menu, p3p_drive::Drive& use, int limit) {
                std::string presses;
                for (int i = 0; i < limit && use.active(); ++i) {
                    p3p_drive::Seen s{true, false, false, menu.open ? std::optional{menu.c} : std::nullopt};
                    auto b = use.Step(s);
                    if (b != p3p_drive::Button::None) { presses += "ABXLud"[int(b)]; menu.Press(b); }
                }
                return presses;
            };
            Menu menu; p3p_drive::Drive use;
            using K = p3p_drive::Request::Kind;
            Require(use.Start({K::Use, 0x504, 5, 0, "Snuff Soul", "Junpei"}, {true, false, false, std::nullopt}), "drive starts");
            std::string presses = run(menu, use, 2000);
            Require(!use.active() && use.succeeded() && use.status() == "Used Snuff Soul on Junpei" && !menu.open &&
                    menu.c.use_list.empty() && presses == "XdALddAddABBB", "drive uses an item and backs out: " + presses);
            Menu full; full.sp_full = 1; p3p_drive::Drive again;
            again.Start({K::Use, 0x504, 1, 0, "Snuff Soul", "Kotone"}, {true, false, false, std::nullopt});
            presses = run(full, again, 2000);
            Require(!again.succeeded() && again.status() == "Snuff Soul had no effect on Kotone" && !full.open,
                    "drive reports no effect: " + presses);
            Require(presses == "XdALddAAAABBB", "no effect retries A twice before backing out: " + presses);
            Menu open; open.open = true; open.c.state = 2; open.c.commands = {0, 1};
            p3p_drive::Drive refused;
            Require(!refused.Start({K::Use, 0x504, 1, 0, "Snuff Soul", "Kotone"}, {true, false, false, open.c}) &&
                    refused.status() == "Close the game's menu first", "drive refuses an open menu");
            Menu stop; p3p_drive::Drive halted;
            halted.Start({K::Use, 0x504, 1, 0, "Snuff Soul", "Kotone"}, {true, false, false, std::nullopt});
            run(stop, halted, 40);
            Require(halted.Step({true, true, false, std::nullopt}) == p3p_drive::Button::None && !halted.active(),
                    "drive stops for a battle or message");
            // Equipment: EQUIP (command 3), Yukari (second member), body (slot 1), the second choice.
            struct Equip {
                bool open{}; p3p::CampMenu c; int worn{0x200};
                void Press(p3p_drive::Button b) {
                    using B = p3p_drive::Button;
                    auto top = [&] { c = {}; c.state = 2; c.input = 1; c.cursor = 3; c.commands = {0, 1, 2, 3, 4, 5, 6}; };
                    if (!open) { if (b == B::X) { open = true; top(); c.cursor = 0; } return; }
                    if (c.state == 2) {
                        if (b == B::Down) ++c.cursor; else if (b == B::Up) --c.cursor;
                        else if (b == B::A && c.commands[c.cursor] == 3) { c.state = 6; c.sub = 5; c.members = {1, 2, 5}; }
                        else if (b == B::B) open = false;
                        return;
                    }
                    int& cursor = c.sub == 5 ? c.member : c.sub == 7 ? c.slot : c.equip_row;
                    if (b == B::Down) ++cursor; else if (b == B::Up) --cursor;
                    else if (b == B::B) { if (c.sub == 5) top(); else c.sub = c.sub == 10 ? 7 : 5; }
                    else if (b == B::A && c.sub == 5) c.sub = 7;
                    else if (b == B::A && c.sub == 7) { c.sub = 10; c.equip_row = 0; c.equip_list = {{0x200, 1}, {0x204, 2}, {0x210, 1}}; }
                    else if (b == B::A && c.sub == 10) worn = c.equip_list[c.equip_row].first;
                }
            };
            Equip eq; p3p_drive::Drive change; presses.clear();
            Require(change.Start({K::Equip, 0x204, 2, 1, "Wool Sweater", "Yukari"}, {true, false, false, std::nullopt, 0x200}),
                    "equip drive starts");
            for (int i = 0; i < 3000 && change.active(); ++i) {
                p3p_drive::Seen s{true, false, false, eq.open ? std::optional{eq.c} : std::nullopt, eq.worn};
                auto b = change.Step(s);
                if (b != p3p_drive::Button::None) { presses += "ABXLud"[int(b)]; eq.Press(b); }
            }
            Require(change.succeeded() && eq.worn == 0x204 && !eq.open && presses == "XdddAdAdAdABBBB",
                    "equip drive changes the slot and backs out: " + presses);
            p3p_drive::Drive same;
            Require(!same.Start({K::Equip, 0x204, 2, 1, "Wool Sweater", "Yukari"}, {true, false, false, std::nullopt, 0x204}) &&
                    same.status() == "Yukari already has Wool Sweater", "equip drive refuses what is already worn");
            // Persona: PERSONA (command 2), the third stock row (Persona ID 4); X is never pressed inside.
            struct Stock {
                bool open{}; p3p::CampMenu c; int selected{0}; std::vector<int> slots{0, 1, 4};
                void Press(p3p_drive::Button b) {
                    using B = p3p_drive::Button;
                    auto top = [&] { c = {}; c.state = 2; c.input = 1; c.cursor = 2; c.commands = {0, 1, 2, 3, 4, 5, 6}; };
                    if (!open) { if (b == B::X) { open = true; top(); c.cursor = 0; } return; }
                    if (c.state == 2) {
                        if (b == B::Down) ++c.cursor; else if (b == B::Up) --c.cursor;
                        else if (b == B::A && c.commands[c.cursor] == 2) { c.state = 7; c.sub = 6; c.personas = 3; }
                        else if (b == B::B) open = false;
                        return;
                    }
                    if (b == B::X) slots.clear(); // Discard: must never happen
                    else if (b == B::Down) ++c.persona_row; else if (b == B::Up) --c.persona_row;
                    else if (b == B::A) selected = slots[size_t(c.persona_row)];
                    else if (b == B::B) top();
                }
            };
            Stock st; p3p_drive::Drive swap; presses.clear();
            Require(swap.Start({K::Persona, 4, 3, 2, "Pixie", ""}, {true, false, false, std::nullopt, -1, 0}), "persona drive starts");
            for (int i = 0; i < 3000 && swap.active(); ++i) {
                p3p_drive::Seen s{true, false, false, st.open ? std::optional{st.c} : std::nullopt, -1, st.selected};
                auto b = swap.Step(s);
                if (b != p3p_drive::Button::None) { presses += "ABXLud"[int(b)]; st.Press(b); }
            }
            Require(swap.succeeded() && st.selected == 4 && st.slots.size() == 3 && !st.open && presses == "XddAddABB",
                    "persona drive switches and backs out: " + presses);
            Stock other; p3p_drive::Drive mismatch;
            mismatch.Start({K::Persona, 4, 2, 1, "Pixie", ""}, {true, false, false, std::nullopt, -1, 0});
            for (int i = 0; i < 3000 && mismatch.active(); ++i) {
                p3p_drive::Seen s{true, false, false, other.open ? std::optional{other.c} : std::nullopt, -1, other.selected};
                auto b = mismatch.Step(s);
                if (b != p3p_drive::Button::None) other.Press(b);
            }
            Require(!mismatch.succeeded() && mismatch.status() == "The Persona list changed" && other.selected == 0 && !other.open,
                    "persona drive refuses a stock that differs from the companion's");
            // The native Equip list (0xe71b0): equipped first by ID order, owned wearables of the slot.
            p3p::Bytes counts(0x790), flags(0x100);
            counts[0x120] = 1; counts[0x130] = 1; counts[0x1] = 3; counts[0x30a] = 1;
            std::vector<p3p::u32> type(0x790), wear(0x790);
            wear[0x120] = wear[0x122] = wear[0x130] = 0x8000; wear[0x1] = 1 << 5; wear[0x30a] = 0x8000;
            auto list = p3p::EquipChoices(1, 0, 0x122, true, counts, flags, type, wear);
            Require(list == std::vector<std::pair<int, int>>{{0x120, 1}, {0x122, 1}, {0x130, 1}}, "equip choices (female lead, weapon)");
            Require(p3p::EquipChoices(1, 0, 0x122, false, counts, flags, type, wear) == std::vector<std::pair<int, int>>{{0x1, 3}, {0x122, 1}},
                    "equip choices (male lead)");
        }
        // Ailment bits (unit +0xc), named only where SKILL.TBL inflict/cure masks pin them down.
        Require(p3p::Ailments(0).empty() && p3p::Ailments(0x100) == "Poison" && p3p::Ailments(0x80002) == "KO  Charm",
                "ailment names");
        Require(p3p::Ailments(0x100001).empty() && p3p::Ailments(0x200).empty(), "unconfirmed status bits stay unnamed");
        // Game font: ASCII only (font0.fnt continues with kana past '~'); without romfs, fail closed.
        Require(p3p_font::GlyphIndex(' ') == 0 && p3p_font::GlyphIndex('A') == 33 && p3p_font::GlyphIndex('~') == 94 &&
                p3p_font::GlyphIndex(0x7F) < 0 && p3p_font::GlyphIndex(0xE9) < 0, "font ASCII mapping");
        Require(api->capabilities & EDEN_DSMOD_CAP_ROMFS_READ, "romfs capability for the font");
        auto font = eden_dsmod_get_font_extensions(EDEN_DSMOD_FONT_EXT_VERSION, EDEN_DSMOD_FONT_EXT_HASH);
        Require(font && font->decode_font && ext->load_image, "font extensions");
        const char marker[] = "P3PFONT";
        Require(!font->decode_font(instance, reinterpret_cast<const uint8_t*>(marker), 7, nullptr,
                                   [](void*, uint32_t, uint32_t, const EdenDsmodFontGlyph*, uint32_t) {}),
                "font fails closed without romfs");
        Require(!ext->load_image(instance, &host, "module:p3p_font", nullptr,
                                 [](void*, uint32_t, uint32_t, const uint8_t*, size_t) {}),
                "atlas fails closed without romfs");
        // Camp art (p3p_art.h): only module:p3p: keys, fails closed without romfs.
        Require(!ext->load_image(instance, &host, "module:p3p:spr/9", nullptr,
                                 [](void*, uint32_t, uint32_t, const uint8_t*, size_t) {}) &&
                !ext->load_image(instance, &host, "module:p3p:persona/1", nullptr,
                                 [](void*, uint32_t, uint32_t, const uint8_t*, size_t) {}),
                "art fails closed without romfs");
        api->destroy(instance);
        {
            // TMX: header at +0x10, data at +0x40; PS2 alpha doubles; CSM1 swaps index bits 3/4.
            auto tmx = [](uint8_t format, uint16_t w, uint16_t h) {
                p3p::Bytes t(0x40); std::memcpy(&t[8], "TMX0", 4);
                t[0x10] = 1; t[0x12] = w & 255; t[0x13] = w >> 8; t[0x14] = h & 255; t[0x15] = h >> 8; t[0x16] = format; return t; };
            auto t = tmx(0x00, 1, 1); t.insert(t.end(), {10, 20, 30, 0x40});
            p3p_art::Image img;
            Require(p3p_art::DecodeTmx(t, 0, img) && img.w == 1 && img.rgba == std::vector<uint8_t>{10, 20, 30, 0x80}, "TMX RGBA32");
            t = tmx(0x13, 1, 1); t.resize(0x40 + 1024 + 1); t[0x40 + 16 * 4] = 7; t[0x40 + 1024] = 8; // index 8 reads entry 16
            Require(p3p_art::DecodeTmx(t, 0, img) && img.rgba[0] == 7, "TMX 8-bit CSM1 palette");
            t = tmx(0x14, 2, 1); t.resize(0x40 + 64 + 1); t[0x40 + 4] = 5; t[0x40 + 8] = 9; t[0x40 + 64] = 0x21;
            Require(p3p_art::DecodeTmx(t, 0, img) && img.rgba[0] == 5 && img.rgba[4] == 9, "TMX 4-bit low nibble first");
            Require(!p3p_art::DecodeTmx(tmx(0x00, 4, 4), 0, img), "short TMX refused");
            // SPR0: one 8x8 texture, sprite (1,0)-(2,1) in PSP texels = 4x4 at x 4.
            p3p::Bytes s(0x30 + 0x80); std::memcpy(&s[8], "SPR0", 4);
            s[0x14] = 1; s[0x16] = 1; s[0x18] = 0x20; s[0x1c] = 0x28; s[0x24] = s.size() & 255; s[0x24 + 1] = s.size() >> 8;
            s[0x2c] = 0x30; s[0x30 + 0x54] = 1; s[0x30 + 0x5c] = 2; s[0x30 + 0x60] = 1;
            auto tex = tmx(0x00, 8, 8); tex.resize(0x40 + 256);
            for (int i = 0; i < 64; ++i) tex[0x40 + i * 4] = uint8_t(i);
            s.insert(s.end(), tex.begin(), tex.end());
            Require(p3p_art::SpriteImage(s, 0, img) && img.w == 4 && img.h == 4 && img.rgba[0] == 4 && img.rgba[4 * 4] == 12,
                    "SPR0 sprite crop at 4x");
            Require(!p3p_art::SpriteImage(s, 1, img), "missing sprite refused");
        }
        std::puts("P3P ABI, cleared snapshots, stale visibility and maximum-stat, action, skill, stat, social-rank, item, Persona-detail, calendar, ailment, map, help, talk, drive, font and art regressions passed");
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;}
}
