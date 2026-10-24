// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "p3p_reader.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

static void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
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
        Require(ext->on_action(instance, "ui_open", 4) && !ext->on_action(instance, "ui_open", 5), "page range");
        Require(ext->on_action(instance, "member_select", 9) && !ext->on_action(instance, "member_select", 10), "member range");
        Require(!ext->on_action(instance, "press", 0), "unknown actions refused");
        output.clear(); api->sample(instance, &host);
        Require(output.at("ui.page") == 4 && output.at("ui.member") == 9 && output.at("skill.ready") == 0, "page state published while unavailable");
        api->destroy(instance);
        Require(p3p::Maximum(101,{0x237},{},false)==202,"HP gain");
        Require(p3p::Maximum(101,{0x237},{},true)==51,"SP loss rounds down before subtract");
        Require(p3p::Maximum(101,{0x238},{},false)==51,"HP loss");
        Require(p3p::Maximum(101,{0x238},{},true)==202,"SP gain");
        Require(p3p::Maximum(101,{0x219,0x21a,0x21b},{},false)==161,"independent percentage rounding");
        Require(p3p::Maximum(900,{0x219},{{0x1e,2}},false)==999,"cap after equipment");
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
        std::puts("P3P ABI, cleared snapshots, stale visibility and maximum-stat, action, skill-cost and skill-list regressions passed");
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;}
}
