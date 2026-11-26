// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Digimon Story Time Stranger module test.
//
// Without arguments: a synthetic self-test. It builds small MDB1 archives in memory holding EXPA tables
// shaped like the game's (items, treasure, shop, side quests, encounters, battle skills and the text
// sheets), with made-up values, creates the module with a matching manifest and checks what it
// publishes. No game files are needed.
//
// With arguments, a harness for checks against the player's own game files
// (tools/digimon-time-stranger/check_gamedata.py):
//   dsmod-dsts-test --file <romfs name>=<local path> ... --config <manifest.json>
//                   [--flag name=value]... --page <id>... [--image <key> <out>]
// serves local files as the romfs, creates the module with the manifest as its config, samples once
// per --page and prints every published value as  PAGE <tab> NAME <tab> VALUE.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

extern "C" const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t, uint64_t);
extern "C" const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t, uint64_t);

namespace {

// ---------------------------------------------------------------- romfs: local files or in-memory archives
std::map<std::string, std::vector<uint8_t>> g_mem;
std::map<std::string, FILE*> g_files;
std::string g_page;
std::map<std::string, int64_t> g_flags;
std::map<std::string, int64_t> g_ints;
std::map<std::string, std::string> g_texts;
bool g_print = false;

size_t Read(void*, const char* path, uint64_t off, void* out, size_t n) {
    if (auto it = g_mem.find(path); it != g_mem.end()) {
        const auto& b = it->second;
        if (!out) return b.size();
        if (off >= b.size()) return 0;
        size_t k = std::min(n, size_t(b.size() - off));
        memcpy(out, b.data() + off, k);
        return k;
    }
    if (auto it = g_files.find(path); it != g_files.end()) {
        FILE* f = it->second;
        if (!out) { fseek(f, 0, SEEK_END); return size_t(ftell(f)); }
        fseek(f, long(off), SEEK_SET);
        return fread(out, 1, n, f);
    }
    return 0;
}
void PubI(void*, const char* n, int64_t v) {
    g_ints[n] = v;
    if (g_print) printf("%s\t%s\t%lld\n", g_page.c_str(), n, (long long)v);
}
void PubT(void*, const char* n, const char* v) {
    g_texts[n] = v;
    if (g_print) printf("%s\t%s\t%s\n", g_page.c_str(), n, v);
}
const char* GetT(void*, const char* n) { return strcmp(n, "__page") ? nullptr : g_page.c_str(); }
int64_t GetI(void*, const char* n, int64_t fb) { auto it = g_flags.find(n); return it == g_flags.end() ? fb : it->second; }

EdenDsmodHostApi MakeHost() {
    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    h.read_romfs = Read; h.publish_i64 = PubI; h.publish_text = PubT; h.get_text = GetT; h.get_i64 = GetI;
    return h;
}

// ---------------------------------------------------------------- building EXPA tables and MDB1 archives
struct Cell { bool str; int32_t i; std::string s; };
Cell I(int32_t v) { return Cell{false, v, {}}; }
Cell S(const char* v) { return Cell{true, 0, v}; }

struct Table { std::string name; std::vector<uint32_t> types; std::vector<std::vector<Cell>> rows; };

void Put32(std::vector<uint8_t>& b, uint32_t v) { for (int k = 0; k < 4; ++k) b.push_back(uint8_t(v >> (8 * k))); }
void Pad(std::vector<uint8_t>& b, size_t a) { while (b.size() % a) b.push_back(0); }

// Column types: 2 = int32, 7 = string (8 bytes, patched through CHNK).
std::vector<uint8_t> Expa(const std::vector<Table>& tables) {
    std::vector<uint8_t> b = {'E', 'X', 'P', 'A'};
    Put32(b, uint32_t(tables.size()));
    struct Patch { uint32_t off; std::string s; };
    std::vector<Patch> patches;
    for (const auto& t : tables) {
        std::string nm = t.name;
        nm.push_back('\0');
        while (nm.size() % 4) nm.push_back('\0');
        Put32(b, uint32_t(nm.size()));
        b.insert(b.end(), nm.begin(), nm.end());
        Put32(b, uint32_t(t.types.size()));
        for (uint32_t ty : t.types) Put32(b, ty);
        std::vector<uint32_t> off;
        uint32_t o = 0;
        for (uint32_t ty : t.types) {
            uint32_t al = ty == 7 ? 8 : 4;
            o = (o + al - 1) / al * al;
            off.push_back(o);
            o += ty == 7 ? 8 : 4;
        }
        Put32(b, o);
        Put32(b, uint32_t(t.rows.size()));
        Pad(b, 8);
        size_t stride = (o + 7) / 8 * 8;
        for (const auto& r : t.rows) {
            size_t base = b.size();
            b.resize(base + stride, 0);
            for (size_t c = 0; c < r.size() && c < t.types.size(); ++c) {
                if (r[c].str) patches.push_back({uint32_t(base + off[c]), r[c].s});
                else memcpy(&b[base + off[c]], &r[c].i, 4);
            }
        }
    }
    b.insert(b.end(), {'C', 'H', 'N', 'K'});
    Put32(b, uint32_t(patches.size()));
    for (const auto& p : patches) {
        std::string s = p.s;
        s.push_back('\0');
        while (s.size() % 4) s.push_back('\0');
        Put32(b, p.off);
        Put32(b, uint32_t(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
    return b;
}

std::vector<uint8_t> Archive(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members) {
    uint32_t fc = uint32_t(members.size()) + 1;
    std::vector<uint8_t> tree(fc * 16), names(fc * 0x80), dent(fc * 16), blob;
    for (uint32_t i = 1; i < fc; ++i) {
        const auto& [nm, data] = members[i - 1];
        memcpy(&tree[i * 16 + 4], &i, 4);
        std::string e = "mbe " + nm;
        memcpy(&names[i * 0x80], e.data(), e.size());
        uint64_t off = blob.size();
        uint32_t n = uint32_t(data.size());
        memcpy(&dent[i * 16], &off, 8);
        memcpy(&dent[i * 16 + 8], &n, 4);
        memcpy(&dent[i * 16 + 12], &n, 4);
        blob.insert(blob.end(), data.begin(), data.end());
    }
    std::vector<uint8_t> out = {'M', 'D', 'B', '1'};
    Put32(out, fc); Put32(out, fc); Put32(out, fc);
    uint64_t start = 0x20 + tree.size() + names.size() + dent.size();
    for (int k = 0; k < 8; ++k) out.push_back(uint8_t(start >> (8 * k)));
    out.resize(0x20, 0);
    for (auto* v : {&tree, &names, &dent, &blob}) out.insert(out.end(), v->begin(), v->end());
    return out;
}

Table Keyed(const char* sheet, std::vector<std::pair<int, const char*>> rows) {
    Table t{"Sheet1", {7, 7}, {}};
    for (auto& [k, v] : rows) t.rows.push_back({S(std::to_string(k).c_str()), S(v)});
    (void)sheet;
    return t;
}

std::vector<Cell> Row(size_t n, std::map<size_t, Cell> set) {
    std::vector<Cell> r(n, I(0));
    for (auto& [c, v] : set) r[c] = v;
    return r;
}

// ---------------------------------------------------------------- the synthetic self-test
void Sample(const EdenDsmodModuleApi* api, void* inst, EdenDsmodHostApi* host, const char* page) {
    g_page = page; g_ints.clear(); g_texts.clear();
    api->sample(inst, host);
}

std::string T(const char* n) { auto it = g_texts.find(n); return it == g_texts.end() ? "<none>" : it->second; }
int64_t N(const char* n) { auto it = g_ints.find(n); return it == g_ints.end() ? -999 : it->second; }

#define CHECK_EQ(a, b) do { auto x_ = (a); auto y_ = (b); if (!(x_ == y_)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #a); std::exit(1); } } while (0)

int SelfTest() {
    std::vector<uint32_t> item_types(28, 2), quest_types(32, 2), skill_types(60, 2), group_types(70, 2);
    // items: 1 (a recovery item: no description text, battle skill 80001), 2 (described), 3 (key item)
    Table items{"item_parameter", item_types, {
        Row(28, {{0, I(1)}, {7, I(40)}, {9, I(300)}, {10, I(60)}, {13, I(80001)}}),
        Row(28, {{0, I(2)}, {7, I(41)}, {9, I(1200)}, {10, I(-1)}, {13, I(0)}}),
        Row(28, {{0, I(3)}, {7, I(73)}, {9, I(-1)}, {10, I(-1)}, {13, I(0)}})}};
    Table skills{"battle_skill_list", skill_types, {
        Row(60, {{0, I(80001)}, {15, I(31011)}, {22, I(9)}, {23, I(1500)}, {25, I(1)}, {33, I(6)}})}};
    // chests: map 101 has items 1 and 2, map 102 item 1; map 20201 (a challenge room) item 2
    Table treasure{"field_treasure", std::vector<uint32_t>(10, 2), {
        Row(10, {{0, I(10101)}, {4, I(1)}}), Row(10, {{0, I(10102)}, {4, I(2)}}),
        Row(10, {{0, I(10201)}, {4, I(1)}}), Row(10, {{0, I(2020101)}, {4, I(2)}})}};
    Table shop{"shop_lineup", std::vector<uint32_t>(10, 2), {Row(10, {{0, I(1)}, {2, I(1)}})}};
    Table quests{"sub_quest_list", quest_types, {
        Row(32, {{0, I(5)}, {8, I(1000)}, {9, I(40)}, {10, I(2)}, {11, I(1)}, {13, I(30311)}, {14, I(1)},
                 {16, I(1)}, {17, I(3)}})}};
    // encounters: field t0101 meets group 900 (enemies 4201 and 5301 = status 42 and 53)
    Table groups{"encount_group", group_types, {Row(70, {{0, I(900)}, {1, I(4201)}, {14, I(5301)}, {26, I(-1)}})}};
    Table enemies{"enemy_parameter", std::vector<uint32_t>(8, 2), {
        Row(8, {{0, I(4201)}, {1, I(42)}}), Row(8, {{0, I(5301)}, {1, I(53)}})}};
    Table field{"field_symbol_enemy", std::vector<uint32_t>(4, 2), {Row(4, {{0, I(1)}, {1, I(900)}})}};

    g_mem["app_0.nx64.mvgl"] = Archive({
        {"data\\item", Expa({items})}, {"data\\battle_skill", Expa({skills})},
        {"data\\field_treasure", Expa({treasure})}, {"data\\shop", Expa({shop})},
        {"data\\quest_sub", Expa({quests})}, {"data\\battle_enemy", Expa({enemies, groups})},
        {"data\\field_battle_t0101", Expa({field})}});
    g_mem["app_text01.nx64.mvgl"] = Archive({
        {"text\\item_name", Expa({Keyed("item_name", {{1, "Test Capsule"}, {2, "Test Chip"}, {3, "Test Key"}})})},
        {"text\\item_explanation", Expa({Keyed("item_explanation", {{2, "Raises a\nstat by 5."}})})},
        {"text\\field_name", Expa({Keyed("field_name", {{10101, "First Field"}, {10102, "Second Field"}})})},
        {"text\\skill_name", Expa({Keyed("skill_name", {{30311, "Test Strike"}})})},
        {"text\\buff_name", Expa({Keyed("buff_name", {{3, "Poison"}})})}});

    // dex numbers: 7 -> status 42, 9 -> status 53
    const char* config = R"({"dsts_data":{"v":1,"items":[1,2,3],"itempages":{"ic0":[0,1,2]},
        "areas":[[10101,[101],[10101]],[10102,[102],[]]],
        "g2s":[-1,-1,-1,-1,-1,-1,-1,42,-1,53],"quests":{"qs5":[5],"qs6":[6]}}})";
    EdenDsmodHostApi host = MakeHost();
    const EdenDsmodModuleApi* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    assert(api);
    void* inst = api->create(&host, config);
    assert(inst);

    Sample(api, inst, &host, "areas");
    CHECK_EQ(N("dsts.data"), int64_t(11));
    CHECK_EQ(T("dsts.arc0"), std::string("2 chests   2 wild Digimon"));
    CHECK_EQ(T("dsts.arc1"), std::string("1 chests   0 wild Digimon"));
    CHECK_EQ(T("dsts.arw0"), std::string("2 Digimon"));

    Sample(api, inst, &host, "ar0");
    CHECK_EQ(N("dsts.ac.n"), int64_t(2));
    CHECK_EQ(N("dsts.aci1"), int64_t(2));
    CHECK_EQ(T("dsts.acn0"), std::string("Test Capsule"));
    CHECK_EQ(T("dsts.ace1"), std::string("Raises a stat by 5."));
    CHECK_EQ(N("dsts.aws0"), int64_t(1042));
    CHECK_EQ(N("dsts.awp1"), int64_t(100009));

    g_flags["@flag:its"] = 0;
    Sample(api, inst, &host, "it");
    CHECK_EQ(T("dsts.it.price"), std::string("Buy 300 yen  /  Sell 60 yen"));
    CHECK_EQ(T("dsts.it.eff"), std::string("Restores 1,500 HP to all allies."));
    CHECK_EQ(T("dsts.it.shop"), std::string("Sold in shops"));
    CHECK_EQ(T("dsts.it.chn"), std::string("In chests (2 areas):"));
    CHECK_EQ(T("dsts.it.ch1"), std::string("Second Field"));
    g_flags["@flag:its"] = 1;
    Sample(api, inst, &host, "it");
    CHECK_EQ(T("dsts.it.price"), std::string("Buy 1,200 yen  (can't be sold)"));
    CHECK_EQ(T("dsts.it.chn"), std::string("In chests (1 area):"));   // the challenge room is left out
    g_flags["@flag:its"] = 2;
    Sample(api, inst, &host, "it");
    CHECK_EQ(T("dsts.it.price"), std::string("No shop price"));
    CHECK_EQ(T("dsts.it.eff"), std::string("No description in the game's own text."));
    CHECK_EQ(T("dsts.it.none").empty(), false);

    g_flags["@flag:dxh"] = 1009;
    Sample(api, inst, &host, "dx");
    CHECK_EQ(T("dsts.dx.wild"), std::string("First Field"));
    g_flags["@flag:dxh"] = -1007;
    Sample(api, inst, &host, "dx");
    CHECK_EQ(T("dsts.dx.wild"), std::string("First Field"));

    Sample(api, inst, &host, "qs5");
    CHECK_EQ(T("dsts.rw0"), std::string("1,000 yen, 40 AP, Test Chip, Test Strike, Test Capsule x3"));
    CHECK_EQ(N("dsts.rwok0"), int64_t(1));
    Sample(api, inst, &host, "qs6");
    CHECK_EQ(N("dsts.rwok0"), int64_t(0));

    api->destroy(inst);

    // No game files at all: the module still loads and says so.
    g_mem.clear();
    inst = api->create(&host, config);
    assert(inst);
    Sample(api, inst, &host, "areas");
    CHECK_EQ(N("dsts.data"), int64_t(-1));
    api->destroy(inst);
    printf("dsmod-dsts self-test passed\n");
    return 0;
}

void Sink(void* r, uint32_t w, uint32_t h, const uint8_t* px, size_t n) {
    fwrite(&w, 4, 1, (FILE*)r); fwrite(&h, 4, 1, (FILE*)r); fwrite(px, 1, n, (FILE*)r);
}

int Harness(int argc, char** argv) {
    std::string config = "{}";
    EdenDsmodHostApi host = MakeHost();
    const EdenDsmodModuleApi* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const EdenDsmodModuleExtensions* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    void* inst = nullptr;
    g_print = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--file" && i + 1 < argc) {
            std::string v = argv[++i];
            size_t eq = v.find('=');
            if (eq == std::string::npos) return 2;
            FILE* f = fopen(v.c_str() + eq + 1, "rb");
            if (!f) { fprintf(stderr, "cannot open %s\n", v.c_str() + eq + 1); return 2; }
            g_files[v.substr(0, eq)] = f;
        } else if (a == "--config" && i + 1 < argc) {
            FILE* f = fopen(argv[++i], "rb");
            if (!f) return 2;
            config.clear();
            char buf[1 << 16];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) config.append(buf, n);
            fclose(f);
        } else if (a == "--flag" && i + 1 < argc) {
            std::string v = argv[++i];
            size_t eq = v.find('=');
            if (eq == std::string::npos) return 2;
            g_flags[v.substr(0, eq)] = atoll(v.c_str() + eq + 1);
        } else if (a == "--page" && i + 1 < argc) {
            if (!inst) inst = api->create(&host, config.c_str());
            if (!inst) return 3;
            g_page = argv[++i];
            api->sample(inst, &host);
        } else if (a == "--image" && i + 2 < argc) {
            if (!inst) inst = api->create(&host, config.c_str());
            if (!inst) return 3;
            FILE* o = fopen(argv[i + 2], "wb");
            if (!o) return 2;
            printf("%s -> %d\n", argv[i + 1], ext->load_image(inst, &host, argv[i + 1], o, Sink));
            fclose(o);
            i += 2;
        } else {
            fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (inst) api->destroy(inst);
    for (auto& [_, f] : g_files) fclose(f);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return argc > 1 ? Harness(argc, argv) : SelfTest();
}
