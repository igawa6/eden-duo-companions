// SPDX-FileCopyrightText: Copyright 2026 jadesigns.
// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic test for the Star Ocean: The Second Story R module: builds a fake guest memory image
// with the IL2CPP objects the module walks (ParameterManager singleton -> UserParameter ->
// PartyParameter / CharacterParameter[]), loads a linux-x86_64 .so and checks what it publishes.
// No game files are needed.
//
// The game-file part reads a synthetic bundle made by so2r_make_fixture.py (no game data).
//
//   python3 native/modules/so2r/so2r_make_fixture.py /tmp/so2r_fx
//   clang++ -std=c++17 -I <eden-duo>/src native/modules/so2r/so2r_module_test.cpp -ldl -o /tmp/so2r_test
//   /tmp/so2r_test build-so2r/release/linux-x86_64/010065301A2E0000.so release /tmp/so2r_fx   # prints PASS
//   /tmp/so2r_test build-so2r/diag/linux-x86_64/010065301A2E0000.so diag /tmp/so2r_fx   # prints PASS
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cmath>
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
static uint64_t g_test_icons[4];
static uint64_t g_test_ufc, g_test_pres;   // the field controller and its minimap presenter (Hide HUD test)
static uint32_t g_mb[0x400];                // the fake guest mailbox (Hide HUD)
static EdenDsmodBool mb_load(void*, uint32_t o, uint32_t* v) { if (o + 4 > sizeof g_mb) return EDEN_DSMOD_FALSE; *v = g_mb[o / 4]; return EDEN_DSMOD_TRUE; }
static EdenDsmodBool mb_store(void*, uint32_t o, uint32_t v) { if (o + 4 > sizeof g_mb) return EDEN_DSMOD_FALSE; g_mb[o / 4] = v; return EDEN_DSMOD_TRUE; }
static EdenDsmodBool mb_store64(void*, uint32_t o, uint64_t v) { if (o + 8 > sizeof g_mb) return EDEN_DSMOD_FALSE; std::memcpy(&g_mb[o / 4], &v, 8); return EDEN_DSMOD_TRUE; }   // the synthetic minimap icons (to change them later)

static std::vector<uint8_t> mem(0x5000000);
static const uint64_t MAIN = 0x80000000;   // guest address of mem[0]; main is at the start
static uint64_t bump = 0x4A00000;          // heap objects after main's .data
static uint64_t alloc(size_t n) { uint64_t a = MAIN + bump; bump += (n + 15) & ~size_t(15); return a; }
template <class T> static void put(uint64_t a, T v) { std::memcpy(&mem[a - MAIN], &v, sizeof v); }
static uint64_t rd64(uint64_t a) { uint64_t v; std::memcpy(&v, &mem[a - MAIN], 8); return v; }
static uint64_t cstr(const char* s) { uint64_t a = alloc(strlen(s) + 1); std::memcpy(&mem[a - MAIN], s, strlen(s) + 1); return a; }
static uint64_t klass(const char* name) { uint64_t k = alloc(0x140); put<uint64_t>(k + 0x10, cstr(name)); return k; }
static uint64_t object(uint64_t k, size_t size) { uint64_t o = alloc(size); put<uint64_t>(o, k); return o; }
static uint64_t il2str(const char* s) {
    uint64_t o = object(klass("String"), 0x14 + 2 * strlen(s) + 2);
    put<int32_t>(o + 0x10, (int32_t)strlen(s));
    for (size_t i = 0; i < strlen(s); ++i) put<uint16_t>(o + 0x14 + 2 * i, (uint16_t)(unsigned char)s[i]);
    return o;
}
static uint64_t int_array(const std::vector<int32_t>& v) {
    uint64_t a = object(klass("PlayerID[]"), 0x20 + 4 * v.size());
    put<int32_t>(a + 0x18, (int32_t)v.size());
    for (size_t i = 0; i < v.size(); ++i) put<int32_t>(a + 0x20 + 4 * i, v[i]);
    return a;
}

static std::map<std::string, int64_t> ints;
static std::map<std::string, std::string> texts;
static EdenDsmodBool rd(void*, uint64_t a, void* out, size_t n) {
    if (a < MAIN || a + n > MAIN + mem.size()) return EDEN_DSMOD_FALSE;
    std::memcpy(out, &mem[a - MAIN], n);
    return EDEN_DSMOD_TRUE;
}
static void pi(void*, const char* n, int64_t v) { ints[n] = v; }
static void pt(void*, const char* n, const char* v) { texts[n] = v; }
static std::map<std::string, double> reals;
static void pf(void*, const char* n, double v) { reals[n] = v; }
static void lg(void*, uint32_t, const char* m) { std::printf("  log: %s\n", m); }
static int64_t g_flag_qp = -1;    // the runtime flag so2r.qp (the quick menu's press toggles it)
static int64_t gi(void*, const char* n, int64_t f) { return std::strcmp(n, "@flag:so2r.qp") == 0 ? g_flag_qp : f; }
static std::string g_fx;
// 1.0.1 SO2R_TEST_FLAKY=1: a host like Peanut was seen to be: a game file's size query and its read at offset 0
// answer the first time only; =2: also no size query answers (the module falls back to the index's size)
static std::map<std::string, int> g_flaky_seen;
static size_t romfs(void*, const char* path, uint64_t off, void* out, size_t n) {
    std::string p = g_fx + "/" + (std::strncmp(path, "file:", 5) == 0 ? path + 5 : path);
    if (std::getenv("SO2R_TEST_FLAKY") && std::strncmp(path, "file:", 5) != 0 && off == 0) {
        int& seen = g_flaky_seen[std::string(path) + (out ? "#r" : "#s")];
        if (seen++) return 0;
        if (!out && std::getenv("SO2R_TEST_FLAKY")[0] == '2') return 0;   // 2: no size query answers at all
    }
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    size_t size = (size_t)std::ftell(f);
    if (!out) { std::fclose(f); return size; }
    if (off >= size) { std::fclose(f); return 0; }
    std::fseek(f, (long)off, SEEK_SET);
    size_t got = std::fread(out, 1, n, f);
    std::fclose(f);
    return got;
}
static std::vector<uint8_t> g_img; static uint32_t g_iw, g_ih;
static void img_sink(void*, uint32_t w, uint32_t h, const uint8_t* px, size_t size) { g_iw = w; g_ih = h; g_img.assign(px, px + size); }
static std::vector<uint8_t> g_data;
static void data_sink(void*, const uint8_t* b, size_t n) { g_data.assign(b, b + n); }
static std::vector<EdenDsmodFontGlyph> g_glyphs; static uint32_t g_line, g_first;
static void font_sink(void*, uint32_t line, uint32_t first, const EdenDsmodFontGlyph* g, uint32_t n) { g_line = line; g_first = first; g_glyphs.assign(g, g + n); }
static int r565(int v, int bits) { return v * 255 / ((1 << bits) - 1); }

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main(int argc, char** argv) {
    if (argc < 4) { std::printf("usage: %s <module.so> release|diag <fixture folder>\n", argv[0]); return 2; }
    g_fx = argv[3];
    const bool diag = argc > 2 && std::strcmp(argv[2], "diag") == 0;
    void* so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!so) { std::printf("dlopen: %s\n", dlerror()); return 2; }
    auto get = (EdenDsmodGetModuleFn)dlsym(so, "eden_dsmod_get_module");
    CHECK(get && !get(EDEN_DSMOD_MODULE_ABI_VERSION, 0x1234));
    const EdenDsmodModuleApi* m = get(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    CHECK(m && m->title_id == 0x010065301A2E0000ull && m->struct_size == sizeof(EdenDsmodModuleApi));
    CHECK(m->supports_build("D214BA64D902AEDA4197B57DEBABD01B00000000000000000000000000000000"));
    CHECK(!m->supports_build("F74B633E480E4E161F80A7C16A467F2A00000000000000000000000000000000"));

    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION; h.struct_size = sizeof h; h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.title_id = 0x010065301A2E0000ull; h.main_base = MAIN; h.main_size = 0x4A00000;
    h.read_romfs = romfs; h.read_memory = rd; h.publish_i64 = pi; h.publish_text = pt; h.publish_f64 = pf; h.log = lg; h.get_i64 = gi;
    void* inst = m->create(&h, "{}");
    CHECK(inst);
    auto run = [&](int n) { for (int i = 0; i < n; ++i) { ints.clear(); texts.clear(); reals.clear(); m->sample(inst, &h); } };

    // 1. title screen: the TypeInfo slot is not initialised yet (IL2CPP keeps an odd token there)
    put<uint64_t>(MAIN + 0x417DAE0, 0x20000123);
    run(7);
    CHECK(ints["so2r.ok"] == 0 && ints["so2r.n"] == 0 && ints["so2r.on0"] == 0 && !ints.count("so2r.fol"));
    std::printf("  title: %s\n", texts["so2r.why"].c_str());

    // 2. in game
    const uint64_t gen = klass("SingletonBasicMonoBehaviour`1");
    const uint64_t statics = alloc(0x10);
    put<uint64_t>(gen + 0xB8, statics);
    put<uint64_t>(MAIN + 0x417DAE0, gen);
    const uint64_t slotref = MAIN + 0x417DAE0;
    put<uint64_t>(MAIN + 0x49496E8, slotref);
    const uint64_t pm = object(klass("ParameterManager"), 0x800);
    put<uint64_t>(statics, pm);
    // names: ScriptablePlayerParameter (+0x490) and characterMessageDictionary (+0x310)
    const uint64_t spp = object(klass("ScriptablePlayerParameter"), 0x20);
    put<uint64_t>(pm + 0x490, spp);
    const uint64_t plist = object(klass("List`1"), 0x20);
    put<uint64_t>(spp + 0x18, plist);
    const uint64_t parr = object(klass("ConstPlayerParameter[]"), 0x20 + 8 * 13);
    put<uint64_t>(plist + 0x10, parr); put<int32_t>(plist + 0x18, 13);
    const char* ids[] = {"CHARA_HERO", "CHARA_RENA", "CHARA_CELINE", "CHARA_BOWMAN", "CHARA_DIAS", "CHARA_PRECIS", "CHARA_ASHTON"};
    const char* firsts[] = {"Hero", "Rena", "<SPACE=0>Celine", "Bowman", "Dias", "Precis", "Ashton"};
    const uint64_t cppk = klass("ConstPlayerParameter");
    for (int i = 0; i < 7; ++i) {
        uint64_t o = object(cppk, 0x90);
        put<int32_t>(o + 0x10, i + 1); put<uint64_t>(o + 0x18, il2str(ids[i]));
        put<int32_t>(o + 0x80, 100);                                     // favoriteDishItemID (item 100)
        put<uint64_t>(parr + 0x20 + 8 * i, o);
    }
    const uint64_t dict = object(klass("Dictionary`2"), 0x40);
    put<uint64_t>(pm + 0x310, dict);
    const uint64_t ents = object(klass("Entry[]"), 0x20 + 0x18 * 10);
    put<uint64_t>(dict + 0x18, ents); put<int32_t>(dict + 0x20, 9);
    const uint64_t ccmk = klass("ConstCharacterMessage");
    for (int i = 0; i < 9; ++i) {
        uint64_t e = ents + 0x20 + 0x18 * i;
        if (i == 2) { put<int32_t>(e, -1); continue; }                    // a free entry
        int c = i < 2 ? i : i - 1;
        if (c >= 7) { put<int32_t>(e, 5); put<uint64_t>(e + 8, il2str("SYS_0001")); continue; }
        uint64_t m = object(ccmk, 0x28);
        put<uint64_t>(m + 0x10, il2str(ids[c])); put<uint64_t>(m + 0x18, il2str(firsts[c])); put<uint64_t>(m + 0x20, il2str("Surname"));
        put<int32_t>(e, 1234 + i); put<uint64_t>(e + 8, il2str(ids[c])); put<uint64_t>(e + 0x10, m);
    }
    // the game's words: systemMessageDictionary (+0x300); "Lv" is in another language, HP is missing
    const uint64_t sdict = object(klass("Dictionary`2"), 0x40), sents = object(klass("Entry[]"), 0x20 + 0x18 * 4);
    put<uint64_t>(pm + 0x300, sdict); put<uint64_t>(sdict + 0x18, sents); put<int32_t>(sdict + 0x20, 3);
    {
        const char* keys[] = {"SYS_0040", "SYS_0324", "SYS_0001"};
        const char* msgs[] = {"Nv", "Gruppe", "Yes"};
        const uint64_t csm = klass("ConstSystemMessage");
        for (int i = 0; i < 3; ++i) {
            uint64_t e = sents + 0x20 + 0x18 * i, mo = object(csm, 0x20);
            put<uint64_t>(mo + 0x10, il2str(keys[i])); put<uint64_t>(mo + 0x18, il2str(msgs[i]));
            put<int32_t>(e, 77 + i); put<uint64_t>(e + 8, il2str(keys[i])); put<uint64_t>(e + 0x10, mo);
        }
    }
    const uint64_t up = object(klass("UserParameter"), 0x220);
    {   // possessions: unique 2049 -> item 100 (in the item dictionary), 2050 -> item 101 (table only)
        const uint64_t pip = object(klass("PossessionItemParameter"), 0x20);
        const uint64_t arr = object(klass("PossessionItem[]"), 0x20 + 8 * 3);
        put<int32_t>(arr + 0x18, 3);
        const int16_t pairs[3][2] = {{7, 55}, {2049, 100}, {2050, 101}};
        for (int i = 0; i < 3; ++i) {
            const uint64_t pi = object(klass("PossessionItem"), 0x28);
            put<int16_t>(pi + 0x10, pairs[i][0]); put<int16_t>(pi + 0x12, pairs[i][1]);
            put<uint64_t>(arr + 0x20 + 8 * i, pi);
        }
        put<uint64_t>(pip + 0x10, arr); put<uint64_t>(up + 0x30, pip);
    }
    put<uint64_t>(pm + 0x7E0, up);
    put<int32_t>(up + 0x10, 1);           // heroID, the hero
    put<int32_t>(up + 0x14, 1234567);     // Fol
    const uint64_t cpk = klass("CharacterParameter");
    const uint64_t chars = object(klass("CharacterParameter[]"), 0x20 + 8 * 14);
    put<int32_t>(chars + 0x18, 14);
    struct C { int pid; const char* name; int lv, hp, hpmax, mp, mpmax; };
    const C cs[] = {{1, "", 12, 310, 420, 20, 35}, {2, "", 11, 250, 260, 88, 120},     // the game leaves names empty
                    {3, "", 10, 190, 200, 140, 150}, {7, "", 15, 0, 600, 10, 60},
                    {5, "", 30, 999, 900, 0, 50}};   // Dias: hp > max -> must be rejected
    uint64_t cpOf[14] = {};
    for (const C& c : cs) {
        uint64_t cp = object(cpk, 0x1D8);
        cpOf[c.pid] = cp;
        put<int32_t>(cp + 0x10, c.pid); put<uint64_t>(cp + 0x18, il2str(c.name));
        put<int32_t>(cp + 0x24, c.lv); put<int32_t>(cp + 0x28, 1000 * c.lv);
        put<int32_t>(cp + 0x50, c.hpmax); put<int32_t>(cp + 0x54, c.hp);
        put<int32_t>(cp + 0x5C, c.mpmax); put<int32_t>(cp + 0x60, c.mp);
        // the stats (ATK DEF HIT AVD INT LUC STM CRT GUTS) and the weapon: item 100
        const int so[9] = {0x68, 0x70, 0x80, 0x88, 0x78, 0xB0, 0xB8, 0x98, 0x90};
        for (int k = 0; k < 9; ++k) put<int32_t>(cp + so[k], c.pid * 100 + k);
        put<int32_t>(cp + 0xFC, 2049); put<int32_t>(cp + 0x100, 2050);   // possession unique IDs (like the game)
        put<int32_t>(cp + 0x20, c.lv);                                   // LevelBase
        if (c.pid == 2) put<int32_t>(cp + 0x28, 2000);                   // Rena: 707 to go at Lv 11
        put<int32_t>(cp + 0x150, (1 << 2) | (1 << 9));                   // Taste, Intuition
        if (c.pid == 2) {                 // Rena poisoned and silenced: BuffDebuffState (BitFlag, int[] +0x10)
            const uint64_t bf = object(klass("BitFlag"), 0x20);
            put<uint64_t>(bf + 0x10, int_array({(1 << 1) | (1 << 5) | (1 << 12)}));
            put<uint64_t>(cp + 0x30, bf);
            const uint64_t cf = object(klass("BitFlag"), 0x20);         // characterFlag bit 5: food known
            put<uint64_t>(cf + 0x10, int_array({1 << 5}));
            put<uint64_t>(cp + 0x118, cf);
        }
        put<uint64_t>(chars + 0x20 + 8 * c.pid, cp);
    }
    put<uint64_t>(up + 0x28, chars);
    const uint64_t pp = object(klass("PartyParameter"), 0xB8);
    put<uint64_t>(up + 0x38, pp);
    put<uint64_t>(pp + 0x10, int_array({2, 1, 0, 3, 7, 5, 0, 0}));   // the game's order, with gaps
    put<uint64_t>(pp + 0xA0, int_array({1, 0, 0, 0}));
    put<int32_t>(pp + 0x18, 2);           // leaderID RENA
    put<uint8_t>(pp + 0xA8, 0);
    put<uint64_t>(pp + 0x80, int_array({0, 1, 1, 1, 0, 2, 0, 2, 0, 0, 0, 0, 0, 0}));
    // the scene: usage slot -> MethodInfo -> klass (+0x20) -> rgctx (+0xC0) -> [0] class -> statics -> GameSceneManager
    const uint64_t gsmk = klass("SingletonBasicMonoBehaviour`1");
    put<uint8_t>(gsmk + 0x132, 1);
    const uint64_t gsmst = alloc(0x10); put<uint64_t>(gsmk + 0xB8, gsmst);
    const uint64_t gsm = object(klass("GameSceneManager"), 0x40); put<uint64_t>(gsmst, gsm);
    const uint64_t smk = klass("SingletonMonoBehaviour`1"); put<uint8_t>(smk + 0x132, 1);
    const uint64_t rg = alloc(0x10); put<uint64_t>(rg, gsmk); put<uint64_t>(smk + 0xC0, rg);
    const uint64_t mi = alloc(0x40); put<uint64_t>(mi + 0x20, smk);
    const uint64_t uslot = alloc(0x10); put<uint64_t>(uslot, mi);
    put<uint64_t>(MAIN + 0x4949418, uslot);
    put<int32_t>(gsm + 0x1C, 3);          // the title screen: the game's boot-time party is not shown
    run(7);
    CHECK(ints["so2r.ok"] == 0 && ints["so2r.scene"] == 3 && ints["so2r.on0"] == 0);
    put<int32_t>(gsm + 0x1C, 4);          // the scene type says field, but the scene object is the title
    put<uint64_t>(gsm + 0x28, object(klass("TitleScene"), 0x20));
    run(7);
    CHECK(ints["so2r.ok"] == 0 && texts["so2r.why"] == "title screen");
    put<uint64_t>(gsm + 0x28, object(klass("FieldmapScene"), 0x20));
    run(7);
    CHECK(ints["so2r.ok"] == 1 && ints["so2r.scene"] == 4);
    CHECK(ints["so2r.fol"] == 1234567);
    CHECK(ints["so2r.n"] == 4);   // 2,1,3,7 -- 0s skipped, Dias rejected
    CHECK(texts["so2r.name0"] == "Rena" && ints["so2r.pid0"] == 2 && ints["so2r.lv0"] == 11);
    CHECK(texts["so2r.name1"] == "Hero" && ints["so2r.hp1"] == 310 && ints["so2r.hpmax1"] == 420);
    CHECK(texts["so2r.hpmx1"] == "/ 420");   // 0.9.57 the game's "/ 420": four places after the slash
    // 0.9.70 the Battle page: Rena leads (featured); the other battle members after her
    CHECK(ints["so2r.bt.on"] == 1 && texts["so2r.bt.name"] == "Rena" && texts["so2r.bt.ill"] == "module:pic/@ill:0002");
    CHECK(ints["so2r.bo.on0"] == 1 && texts["so2r.bo.name0"] != "Rena" && !texts["so2r.bo.name0"].empty());
    CHECK(texts["so2r.name2"] == "Celine" && ints["so2r.mp2"] == 140 && ints["so2r.mpmax2"] == 150);
    CHECK(texts["so2r.name3"] == "Ashton" && ints["so2r.hp3"] == 0);
    {   // the Status page: the first row (Rena) until a row is picked; stats; the weapon's name
        const uint64_t idict = object(klass("Dictionary`2"), 0x40), ient = object(klass("Entry[]"), 0x20 + 0x18 * 2);
        put<uint64_t>(pm + 0x7B0, idict); put<uint64_t>(idict + 0x18, ient); put<int32_t>(idict + 0x20, 2);
        const uint64_t cip = object(klass("ConstItemParameter"), 0xD0);
        put<int32_t>(cip + 0x10, 100); put<uint64_t>(cip + 0x18, il2str("ITEM_0100")); put<int32_t>(cip + 0xB8, 8);   // SWORD
        put<int32_t>(ient + 0x20, -1);                                         // a free entry
        put<int32_t>(ient + 0x38, 7); put<int32_t>(ient + 0x40, 100); put<uint64_t>(ient + 0x48, cip);
        {   // the item table (ScriptableItemParameter +0x640): item 101 is only there
            const uint64_t sip = object(klass("ScriptableItemParameter"), 0x20), il = object(klass("List`1"), 0x20);
            const uint64_t ia = object(klass("ConstItemParameter[]"), 0x20 + 8 * 2);
            const uint64_t c101 = object(klass("ConstItemParameter"), 0xD0);
            put<int32_t>(c101 + 0x10, 101); put<uint64_t>(c101 + 0x18, il2str("ITEM_0100"));
            put<uint64_t>(pm + 0x640, sip); put<uint64_t>(sip + 0x18, il); put<uint64_t>(il + 0x10, ia); put<int32_t>(il + 0x18, 2);
            put<uint64_t>(ia + 0x20, cip); put<uint64_t>(ia + 0x28, c101);
        }
        const uint64_t mdict = object(klass("Dictionary`2"), 0x40), ment = object(klass("Entry[]"), 0x20 + 0x18);
        put<uint64_t>(pm + 0x320, mdict); put<uint64_t>(mdict + 0x18, ment); put<int32_t>(mdict + 0x20, 1);
        const uint64_t cim = object(klass("ConstItemMessage"), 0x30);
        put<uint64_t>(cim + 0x10, il2str("ITEM_0100")); put<uint64_t>(cim + 0x18, il2str("Longsword"));
        put<int32_t>(ment + 0x20, 9); put<uint64_t>(ment + 0x28, il2str("ITEM_0100")); put<uint64_t>(ment + 0x30, cim);
        auto ge0 = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
        const EdenDsmodModuleExtensions* ex0 = ge0 ? ge0(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : nullptr;
        run(2);
        // 0.9.10 the Party tab: the roster first (rows, no details), tap a row for its details
        CHECK(ints["so2r.v.roster"] == 1 && ints["so2r.ro0"] == 1 && ints["so2r.s.on"] == 0 && ints["so2r.po0"] == 0);
        CHECK(texts["so2r.ill0"] == "module:pic/@ill:0002" && texts["so2r.lvarc0"].rfind("module:pic/@lvarc:", 0) == 0);
        // 0.9.21 the Guide Map grid tile: the fixture has no camp_map_grid, so it is refused (no picture, no crash)
        CHECK(ex0 && ex0->load_image && !ex0->load_image(inst, &h, "module:pic/@grid:72", nullptr, img_sink));
        std::printf("  roster: %s %s\n", texts["so2r.ill0"].c_str(), texts["so2r.lvarc0"].c_str());
        {   // 0.9.17 the Map page's strip: the battle members (partyMemberStates Battle); a tap opens their details
            CHECK(ints["so2r.bn"] == 3 && ints["so2r.bon0"] == 1 && ints["so2r.bon2"] == 1 && ints["so2r.bon3"] == 0 && texts["so2r.bname0"] == texts["so2r.name0"]);
            const int64_t g0 = ints["so2r.goparty"];
            CHECK(ex0 && ex0->on_action(inst, "so2r.bopen1", 0));
            run(1);
            CHECK(ints["so2r.goparty"] != g0 && ints["so2r.v.det"] == 1 && ints["so2r.sel1"] == 1);
            CHECK(ex0->on_action(inst, "so2r.roster", 0));
            run(1);
            CHECK(ints["so2r.v.roster"] == 1);
        }
        CHECK(ex0 && ex0->on_action(inst, "so2r.open0", 0));
        run(1);
        CHECK(ints["so2r.v.det"] == 1 && ints["so2r.ro0"] == 0 && ints["so2r.po0"] == 1 && ints["so2r.psel0"] == 1 && ints["so2r.sub0"] == 1);
        CHECK(ints["so2r.s.on"] == 1 && ints["so2r.s.st0on"] == 1 && texts["so2r.s.name"] == "Rena" && ints["so2r.sel0"] == 1);
        CHECK(ints["so2r.s.st0"] == 200 && ints["so2r.s.st8"] == 208 && ints["so2r.s.st4"] == 204);
        CHECK(ints["so2r.s.has0"] == 0 && ints["so2r.s.p2"] == 0);          // equipment: its own subtab
        CHECK(ex0->on_action(inst, "so2r.sub1", 0));
        run(1);
        CHECK(ints["so2r.s.p2"] == 1 && ints["so2r.s.p0"] == 0 && ints["so2r.s.st0on"] == 0);
        CHECK(texts["so2r.s.eq0"] == "Longsword" && ints["so2r.s.has0"] == 1 && texts["so2r.s.eq1"] == "Longsword" && ints["so2r.s.has2"] == 0);
        CHECK(texts["so2r.s.eqic0"] == "module:pic/icon_item_sword" && texts["so2r.s.eqic1"] == "module:pic/icon_item_armor"
              && texts["so2r.s.eqic4"] == "module:pic/icon_item_greeve" && texts["so2r.s.eqic6"] == "module:pic/icon_item_accessory");
        CHECK(ints["so2r.s.eqicon2"] == 1 && ints["so2r.s.eqno2"] == 1 && ints["so2r.s.eqno0"] == 0);
        CHECK(ex0->on_action(inst, "so2r.sub0", 0));
        run(1);
        CHECK(ints["so2r.s.hasnext"] == 1 && ints["so2r.s.next"] == 707);   // the game's table, Lv 11
        CHECK(texts["so2r.s.food"] == "Longsword");                          // Rena knows hers (item 100)
        CHECK(texts["so2r.s.tlw0"] == "Taste" && texts["so2r.s.tlw1"] == "Intuition" && ints["so2r.s.tlon2"] == 0);
        CHECK(texts["so2r.s.bdw0"] == "Poison" && texts["so2r.s.bdw1"] == "Silence" && ints["so2r.s.bdon1"] == 1 && ints["so2r.s.bdon2"] == 0);
        CHECK(ex0 && ex0->on_action(inst, "so2r.pick1", 0));
        run(1);
        CHECK(texts["so2r.s.name"] == "Hero" && ints["so2r.s.st0"] == 100 && ints["so2r.sel1"] == 1 && ints["so2r.sel0"] == 0);
        CHECK(ints["so2r.sub0"] == 1 && ints["so2r.psel1"] == 1);           // the subtab kept
        CHECK(ex0 && ex0->on_action(inst, "so2r.pick0", 0));
        run(1);
        {   // page 2: talents and friendship (Rena <-> Hero 40 of the game's most)
            const uint64_t em = object(klass("EmotionParameter"), 0x20);
            const uint64_t fa = object(klass("Int16[]"), 0x20 + 2 * 196);
            put<int32_t>(fa + 0x18, 196); put<int16_t>(fa + 0x20 + 2 * (2 * 14 + 1), 40);
            put<uint64_t>(em + 0x10, fa); put<uint64_t>(up + 0x40, em);
            run(30);
            CHECK(ints["so2r.s.p0"] == 1 && ints["so2r.s.p1"] == 0 && ints["so2r.s.tlon0"] == 0);
            CHECK(ex0->on_action(inst, "so2r.spage", 0));
            run(1);
            CHECK(ints["so2r.s.p1"] == 1 && ints["so2r.s.tlon0"] == 1 && ints["so2r.s.has0"] == 0);
            // hearts: 40 of the game's most -> 40 eighths x 40 / max (max read from GameDefine; 1 in the
            // fixture -> all five full)
            CHECK(ints["so2r.s.ht0_0"] == 8 && ints["so2r.s.ht0_4"] == 8);
            CHECK(ints["so2r.s.fron0"] == 1 && ints["so2r.s.fr0"] == 40 && texts["so2r.s.frname0"] == "Hero");
            CHECK(ex0->on_action(inst, "so2r.spage", 0));
            run(1);
        }
    }
    CHECK(ints["so2r.on3"] == 1 && ints["so2r.on4"] == 0 && !texts.count("so2r.name4"));
    CHECK(texts["so2r.name2"] == "Celine");                 // <SPACE=0> tag removed
    CHECK(ints["so2r.lead0"] == 1 && ints["so2r.lead1"] == 0);
    CHECK(texts["so2r.dot0"] == "module:pic/icon_dot_0002" && texts["so2r.dot3"] == "module:pic/icon_dot_0007");
    {   // 0.7: GUEST (PartyJoiningState 2) and the Skills page
        put<uint64_t>(pp + 0x78, int_array({0, 1, 1, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}));   // Celine a guest
        // the skill messages (skillMessageDictionary +0x340)
        const char* mk[] = {"SKL_0011", "SKL_0014", "SKL_0015", "SSKL_0001", "SSKL_0002", "SSSKL_0001"};
        const char* mn[] = {"Determination", "Biology", "Mental Science", "Art", "Oracle", "Master Chef"};
        const uint64_t sd = object(klass("Dictionary`2"), 0x40), se = object(klass("Entry[]"), 0x20 + 0x18 * 6);
        put<uint64_t>(pm + 0x340, sd); put<uint64_t>(sd + 0x18, se); put<int32_t>(sd + 0x20, 6);
        for (int i = 0; i < 6; ++i) {
            const uint64_t mo = object(klass("ConstSkillMessage"), 0x28);
            put<uint64_t>(mo + 0x10, il2str(mk[i])); put<uint64_t>(mo + 0x18, il2str(mn[i]));
            const uint64_t e = se + 0x20 + 0x18 * i;
            put<int32_t>(e, 300 + i); put<uint64_t>(e + 8, il2str(mk[i])); put<uint64_t>(e + 0x10, mo);
        }
        auto table = [&](uint64_t at, std::vector<std::pair<int, const char*>> rows, bool sp) {
            const uint64_t so_ = object(klass("ScriptableSkillParameter"), 0x20), li = object(klass("List`1"), 0x20);
            const uint64_t ar = object(klass("Const[]"), 0x20 + 8 * rows.size());
            put<uint64_t>(pm + at, so_); put<uint64_t>(so_ + 0x18, li); put<uint64_t>(li + 0x10, ar);
            put<int32_t>(li + 0x18, static_cast<int32_t>(rows.size()));
            for (size_t i = 0; i < rows.size(); ++i) {
                const uint64_t r = object(klass("ConstSkillParameter"), 0x30);
                put<int32_t>(r + 0x10, rows[i].first); put<uint64_t>(r + 0x18, il2str(rows[i].second));
                if (sp) {
                    const uint64_t l2 = object(klass("List`1"), 0x20);
                    put<uint64_t>(l2 + 0x10, int_array({10, 16, 16, 20})); put<int32_t>(l2 + 0x18, 4);
                    if (rows[i].first == 14) put<uint64_t>(l2 + 0x10, int_array({40, 44, 50}));
                    put<uint64_t>(r + 0x20, l2);
                }
                put<uint64_t>(ar + 0x20 + 8 * i, r);
            }
        };
        table(0x618, {{11, "SKL_0011"}, {14, "SKL_0014"}, {15, "SKL_0015"}}, true);
        table(0x630, {{1, "SSKL_0001"}, {2, "SSKL_0002"}}, false);
        table(0x638, {{1, "SSSKL_0001"}}, false);
        std::vector<int32_t> sk(34, 0); sk[11] = 2; sk[14] = 1;
        put<uint64_t>(cpOf[2] + 0x128, int_array(sk)); put<int32_t>(cpOf[2] + 0x130, 176);
        std::vector<int32_t> ss(19, 0); ss[1] = 1;
        put<uint64_t>(cpOf[2] + 0x148, int_array(ss));
        put<uint64_t>(up + 0x50, int_array({0, 0}));
        {   // combat skills: Guardbreak (1, released), Feint (5, neither), Power Burst (12, Lv 1)
            const char* bk[] = {"CSKL_0001", "CSKL_0005", "CSKL_0012"};
            const char* bn[] = {"Guardbreak", "Feint", "Power Burst"};
            const uint64_t bd = object(klass("Dictionary`2"), 0x40), be = object(klass("Entry[]"), 0x20 + 0x18 * 3);
            put<uint64_t>(pm + 0x350, bd); put<uint64_t>(bd + 0x18, be); put<int32_t>(bd + 0x20, 3);
            const int ids2[] = {1, 5, 12};
            const uint64_t so_ = object(klass("ScriptableCombatSkillParameter"), 0x20), li = object(klass("List`1"), 0x20);
            const uint64_t ar = object(klass("Const[]"), 0x20 + 8 * 3);
            put<uint64_t>(pm + 0x620, so_); put<uint64_t>(so_ + 0x18, li); put<uint64_t>(li + 0x10, ar); put<int32_t>(li + 0x18, 3);
            for (int i = 0; i < 3; ++i) {
                const uint64_t mo = object(klass("ConstBattleSkillMessage"), 0x30);
                put<uint64_t>(mo + 0x10, il2str(bk[i])); put<uint64_t>(mo + 0x18, il2str(bn[i]));
                const uint64_t e = be + 0x20 + 0x18 * i;
                put<int32_t>(e, 400 + i); put<uint64_t>(e + 8, il2str(bk[i])); put<uint64_t>(e + 0x10, mo);
                const uint64_t r = object(klass("ConstCombatSkillParameter"), 0x40);
                put<int32_t>(r + 0x10, ids2[i]); put<uint64_t>(r + 0x18, il2str(bk[i]));
                const uint64_t l2 = object(klass("List`1"), 0x20);
                put<uint64_t>(l2 + 0x10, int_array({i == 0 ? 30 : 15, 10, 20})); put<int32_t>(l2 + 0x18, 3);
                put<uint64_t>(r + 0x30, l2);
                put<uint64_t>(ar + 0x20 + 8 * i, r);
            }
            std::vector<int32_t> cl(27, 0); cl[12] = 1;
            put<uint64_t>(cpOf[2] + 0x138, int_array(cl)); put<int32_t>(cpOf[2] + 0x140, 128);
            const uint64_t bf = object(klass("BitFlag"), 0x20);
            put<uint64_t>(bf + 0x10, int_array({1 << 1})); put<uint64_t>(up + 0x220, bf);
        }
        {   // missions: 140 "Fight 10 battles!" (cleared), 23 (released), 7 (neither), 200 (another tab)
            const uint64_t qd = object(klass("Dictionary`2"), 0x40), qe = object(klass("Entry[]"), 0x20 + 0x18 * 3);
            put<uint64_t>(pm + 0x380, qd); put<uint64_t>(qd + 0x18, qe); put<int32_t>(qd + 0x20, 3);
            const char* qk[] = {"MISSION_140", "MISSION_023", "QUEST_004"};
            const char* qn[] = {"Fight [V] battles!", "Use <C1>Art</C1> [V] times!", "Fishing Mission 1"};
            for (int i = 0; i < 3; ++i) {
                const uint64_t mo = object(klass("ConstQuestMessage"), 0x40);
                put<uint64_t>(mo + 0x10, il2str(qk[i])); put<uint64_t>(mo + 0x18, il2str(qn[i]));
                const uint64_t e = qe + 0x20 + 0x18 * i;
                put<int32_t>(e, 500 + i); put<uint64_t>(e + 8, il2str(qk[i])); put<uint64_t>(e + 0x10, mo);
            }
            {   // the token literal: GOT main+0x4958190 -> slot -> string
                const uint64_t sl = alloc(0x10); put<uint64_t>(sl, il2str("[V]")); put<uint64_t>(MAIN + 0x4958190, sl);
            }
            struct M2 { int id, cat, value, rid; const char* key; };
            const M2 ms[] = {{23, 1, 30, 2023, "MISSION_023"}, {7, 2, 5, 2007, "MISSION_007"}, {140, 1, 10, 2140, "MISSION_140"},
                             {200, 3, 2, 2200, "MISSION_200"}};
            {   // GetAchievementCount's table and one case: type 3 -> mov w1, #7; b <GetVariableIntValue tail>
                std::vector<uint16_t> tab(71, 0); tab[2] = 0x100;
                for (int i = 0; i < 71; ++i) put<uint16_t>(MAIN + 0x35D55CE + 2 * i, tab[i]);
                const uint64_t at = 0x22D3CAC + 0x400;
                put<uint32_t>(MAIN + at, 0x52800001u | (7u << 5));
                put<uint32_t>(MAIN + at + 4, 0x14000000u | (static_cast<uint32_t>((0x22D43ACll - static_cast<long long>(at + 4)) / 4) & 0x3FFFFFF));
                std::vector<int32_t> vars(100, 0); vars[7] = 12; vars[0x2D] = 3;   // best time 3 (mission 7: under 5)
                put<uint64_t>(up + 0x138, int_array(vars));
            }
            const uint64_t so_ = object(klass("ScriptableMissionParameter"), 0x20), li = object(klass("List`1"), 0x20);
            const uint64_t ar = object(klass("Const[]"), 0x20 + 8 * 4);
            put<uint64_t>(pm + 0x6B0, so_); put<uint64_t>(so_ + 0x18, li); put<uint64_t>(li + 0x10, ar); put<int32_t>(li + 0x18, 4);
            for (int i = 0; i < 4; ++i) {
                const uint64_t r = object(klass("ConstMissionParameter"), 0x40);
                put<int32_t>(r + 0x10, ms[i].id); put<int32_t>(r + 0x14, ms[i].cat); put<uint64_t>(r + 0x20, il2str(ms[i].key));
                put<int32_t>(r + 0x38, ms[i].value); put<int32_t>(r + 0x3C, ms[i].rid);
                put<int32_t>(r + 0x18, ms[i].id == 23 ? 3 : ms[i].id == 7 ? 19 : ms[i].id == 200 ? 49 : 0);   // 49: arts at their top
                put<uint64_t>(ar + 0x20 + 8 * i, r);
            }
            // rewards: 2140 FOL 200 + BP 5; 2023 SP 50
            const int rw[][4] = {{2140, 2, 200, 1}, {2140, 4, 5, 1}, {2023, 3, 50, 1}};
            const uint64_t rs = object(klass("ScriptableRewardParameter"), 0x20), rl = object(klass("List`1"), 0x20);
            const uint64_t ra = object(klass("Const[]"), 0x20 + 8 * 3);
            put<uint64_t>(pm + 0x668, rs); put<uint64_t>(rs + 0x18, rl); put<uint64_t>(rl + 0x10, ra); put<int32_t>(rl + 0x18, 3);
            for (int i = 0; i < 3; ++i) {
                const uint64_t r = object(klass("ConstRewardParameter"), 0x28);
                for (int k = 0; k < 4; ++k) put<int32_t>(r + 0x10 + 4 * k, rw[i][k]);
                put<uint64_t>(ra + 0x20 + 8 * i, r);
            }
            std::vector<int32_t> rel(8, 0), clr(8, 0), qr(4, 0);
            rel[0] = 1 << 23; rel[140 >> 5] |= 1 << (140 & 31); rel[200 >> 5] |= 1 << (200 & 31); clr[140 >> 5] |= 1 << (140 & 31); qr[0] = 1 << 4;
            auto bitflag = [&](const std::vector<int32_t>& v) { const uint64_t b = object(klass("BitFlag"), 0x20); put<uint64_t>(b + 0x10, int_array(v)); return b; };
            put<uint64_t>(up + 0x110, bitflag(rel)); put<uint64_t>(up + 0xC8, bitflag(clr));
            put<uint64_t>(up + 0xB8, bitflag(qr)); put<uint64_t>(up + 0xC0, bitflag(std::vector<int32_t>(4, 0)));
        }
        {   // the game's IC list: a static IList of SpecialSkillID ({ART})
            const uint64_t ck = klass("UICommon"), st = alloc(0x100), sl = alloc(0x10);
            put<uint64_t>(ck + 0xB8, st); put<uint64_t>(sl, ck); put<uint64_t>(MAIN + 0x49497C0, sl);
            const uint64_t li = object(klass("List`1"), 0x20);
            put<uint64_t>(li + 0x10, int_array({1})); put<int32_t>(li + 0x18, 1);
            put<uint64_t>(st + 0x98, li);
            // r30 the element row: the order (ElementIDs) at +0xD8, Ashton's resistances by ElementID, and
            // ToSystemMessageID's key for DOUBLE (a word the dictionary has; the others fall back)
            put<uint64_t>(st + 0xD8, int_array({1, 2, 3, 4, 8, 9}));
            put<uint64_t>(cpOf[2] + 0xC8, int_array({0, 0, 2, 1, 0, 0, 0, 0, 0, 0, 0}));
            const uint64_t slot = alloc(8); put<uint64_t>(slot, il2str("SYS_0001"));
            put<uint64_t>(MAIN + 0x420DD38 + 8, slot);
        }
        auto gx = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
        const EdenDsmodModuleExtensions* exx = gx ? gx(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : nullptr;
        CHECK(exx && exx->on_action(inst, "so2r.sub0", 0));   // the element row: under the Status subtab's stats
        run(30);
        CHECK(texts["so2r.s.elw0"] == "Yes" && texts["so2r.s.elw1"] == "1/2 Effect" && texts["so2r.s.elw2"] == "-");   // the screens' order: Fire, Water, Wind ...
        CHECK(texts["so2r.s.elic4"] == "module:pic/battle_icon_element_lightning" && texts["so2r.s.elic5"] == "module:pic/battle_icon_element_dark" && ints["so2r.s.elbad0"] == 1 && ints["so2r.s.elbad1"] == 0 && texts["so2r.s.elic0"] == "module:pic/battle_icon_element_fire");
        CHECK(ints["so2r.s.elon5"] == 1 && ints["so2r.s.elon6"] == 0 && ints["so2r.k.elon0"] == 0 && ints["so2r.s.elrow"] == 1);
        CHECK(exx->on_action(inst, "so2r.sub1", 0));          // and one column on the Equipment subtab
        run(1);
        CHECK(ints["so2r.s.eel5"] == 1 && ints["so2r.s.eel6"] == 0 && ints["so2r.s.elon0"] == 0 && ints["so2r.cardsw"] == 4 * 304 - 8 && ints["so2r.rostw"] == 4 * 284 - 12);
        CHECK(exx->on_action(inst, "so2r.sub3", 0));          // the Skills subtab
        run(1);
        CHECK(ints["so2r.k.st"] == 0 && ints["so2r.s.elon0"] == 0 && ints["so2r.k.sw"] == 1 && ints["so2r.kc.on"] == 1);   // 0.9.129 the card beside Skills
        CHECK(ints["so2r.guest2"] == 1 && ints["so2r.guest1"] == 0 && ints["so2r.guest0"] == 0);
        CHECK(ints["so2r.k.on"] == 1 && ints["so2r.k.p0"] == 1 && ints["so2r.k.sp"] == 176);
        CHECK(texts["so2r.k.n0"] == "Determination" && ints["so2r.k.lv0"] == 2 && ints["so2r.k.c0"] == 12);
        CHECK(texts["so2r.k.n1"] == "Biology" && ints["so2r.k.lv1"] == 1 && ints["so2r.k.c1"] == 40);
        CHECK(ints["so2r.k.on2"] == 0);
        auto ge1 = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
        const EdenDsmodModuleExtensions* ex1 = ge1 ? ge1(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : nullptr;
        {   // special arts: Air Slash (id 3, Lv 1), learning rows: Hero (pid 1) Air Slash BP 50 / 80
            const uint64_t bs = object(klass("ScriptableBattleSkillParameter"), 0x20), bl = object(klass("List`1"), 0x20);
            const uint64_t ba = object(klass("Const[]"), 0x28);
            put<uint64_t>(pm + 0x4D8, bs); put<uint64_t>(bs + 0x18, bl); put<uint64_t>(bl + 0x10, ba); put<int32_t>(bl + 0x18, 1);
            const uint64_t br = object(klass("ConstBattleSkillParameter"), 0x90);
            put<int32_t>(br + 0x10, 3); put<uint64_t>(br + 0x18, il2str("CSKL_0001")); put<int32_t>(br + 0x7C, 5); put<uint64_t>(ba + 0x20, br);
            const uint64_t ls = object(klass("ScriptableLearningBattleSkillParameter"), 0x20), ll = object(klass("List`1"), 0x20);
            const uint64_t la = object(klass("Const[]"), 0x28);
            put<uint64_t>(pm + 0x4E8, ls); put<uint64_t>(ls + 0x18, ll); put<uint64_t>(ll + 0x10, la); put<int32_t>(ll + 0x18, 1);
            const uint64_t lr = object(klass("ConstLearningBattleSkillParameter"), 0x38);
            put<int32_t>(lr + 0x10, 2); put<int32_t>(lr + 0x1C, 3);
            const uint64_t l2 = object(klass("List`1"), 0x20); put<uint64_t>(l2 + 0x10, int_array({50, 80})); put<int32_t>(l2 + 0x18, 2);
            put<uint64_t>(lr + 0x20, l2); put<uint64_t>(la + 0x20, lr);
            const uint64_t arr = object(klass("LearningBattleSkill[]"), 0x20 + 12 * 2);
            put<int32_t>(arr + 0x18, 2); put<int32_t>(arr + 0x20, 3); put<int32_t>(arr + 0x24, 1);
            put<uint64_t>(cpOf[2] + 0x160, arr);
            put<uint64_t>(cpOf[2] + 0x158, int_array({3, 0}));
        }
        CHECK(ex1 && ex1->on_action(inst, "so2r.sub2", 0));   // the Arts/Spells subtab
        run(30);
        CHECK(ints["so2r.k.p3"] == 1 && ints["so2r.k.sw"] == 0 && texts["so2r.a.n0"] == "Guardbreak" && ints["so2r.a.lv0"] == 1 && ints["so2r.a.c0"] == 50);
        CHECK(texts["so2r.a.lvt0"] == "1 / 5");
        CHECK(ints["so2r.a.on1"] == 0 && texts["so2r.a.eq0"] == "Guardbreak" && texts["so2r.a.eq1"] == "None");
        CHECK(ex1->on_action(inst, "so2r.sub3", 0));
        CHECK(ex1->on_action(inst, "so2r.kpage", 0));
        run(1);
        CHECK(ints["so2r.k.p1"] == 1 && ints["so2r.k.on0"] == 0 && ints["so2r.k.bp"] == 128);
        // 0.9.129 Guardbreak: Lv 0 but released, shown like the game; Feint: neither, not shown
        CHECK(texts["so2r.c.n0"] == "Guardbreak" && ints["so2r.c.lv0"] == 0 && ints["so2r.c.on0"] == 1);
        CHECK(texts["so2r.c.n1"] == "Power Burst" && ints["so2r.c.lv1"] == 1 && ints["so2r.c.c1"] == 10 && ints["so2r.c.hc1"] == 1);
        CHECK(ints["so2r.c.on2"] == 0 && ints["so2r.c.cnt"] == 2 && ints["so2r.kc.on"] == 1);
        CHECK(ex1->on_action(inst, "so2r.kpage", 0));
        run(1);
        CHECK(ints["so2r.k.p2"] == 1 && ints["so2r.c.on0"] == 0);
        CHECK(texts["so2r.k.icn0"] == "Art" && ints["so2r.k.icb0"] == 1 && ints["so2r.k.icl0"] == 1 && ints["so2r.k.ich0"] == 1);
        CHECK(texts["so2r.k.skn0"] == "Oracle" && ints["so2r.k.skd0"] == 1 && ints["so2r.k.skb0"] == 0);
        CHECK(texts["so2r.k.ssn0"] == "Master Chef" && ints["so2r.k.ssd0"] == 1);
        CHECK(ex1->on_action(inst, "so2r.kpage", 0));
        run(1);
        CHECK(ints["so2r.k.p0"] == 1);
        // the Missions page: Beginner = categories 1 and 2; cleared first, then released, then "????"
        CHECK(ints["so2r.m.on"] == 1 && ints["so2r.m.tab0"] == 1);
        // 0.9.58: 140 "Fight 10 battles!" has its reward taken (clearedMissionFlag): gone, like the game's list
        CHECK(texts["so2r.m.n0"] == "????" && ints["so2r.m.ok0"] == 1);   // type 19: value >= the best time
        CHECK(texts["so2r.m.n1"] == "Use Art 30 times!" && ints["so2r.m.ng1"] == 1 && texts["so2r.m.rw1"] == "SP 50");
        CHECK(texts["so2r.m.c1"] == "12 / 30" && texts["so2r.m.c0"] == "");
        CHECK(ints["so2r.m.on2"] == 0 && ints["so2r.m.n"] == 2);
        CHECK(ex1->on_action(inst, "so2r.mtab1", 0));
        run(1);
        CHECK(ints["so2r.m.tab1"] == 1 && texts["so2r.m.n0"] == "????" && ints["so2r.m.on1"] == 0);
        CHECK(texts["so2r.m.c0"] == "0 / 2");   // type 49 (r34): no joined member has an art at its top yet
        CHECK(ex1->on_action(inst, "so2r.msec1", 0));
        run(1);
        CHECK(ints["so2r.m.sec1"] == 1 && ints["so2r.m.tabw"] == 0);
        CHECK(texts["so2r.m.n0"] == "Fishing Mission 1" && ints["so2r.m.on1"] == 0 && ints["so2r.m.none"] == 0);
        CHECK(ex1->on_action(inst, "so2r.msec0", 0));
        run(1);
        CHECK(ints["so2r.m.tabw"] == 1 && ints["so2r.m.tab1"] == 1);
        CHECK(ex1->on_action(inst, "so2r.mtab0", 0));
        run(1);
    }

    // values republished on every sample, read again within 6 samples
    put<int32_t>(up + 0x14, 99);
    run(1);
    CHECK(ints["so2r.fol"] == 1234567 || ints["so2r.fol"] == 99);
    run(6);
    CHECK(ints["so2r.fol"] == 99);

    // 3. the game's override rule (coliseum etc.): override list replaces playerIDs
    put<uint8_t>(pp + 0xA8, 1);
    run(7);
    CHECK(ints["so2r.ok"] == 1 && ints["so2r.n"] == 1 && texts["so2r.name0"] == "Hero");
    put<uint8_t>(pp + 0xA8, 0);

    // 4. a wrong object on the path (stale pointer) -> nothing published
    put<uint64_t>(pm + 0x7E0, pp);
    run(7);
    CHECK(ints["so2r.ok"] == 0 && ints["so2r.on0"] == 0 && !ints.count("so2r.fol"));
    put<uint64_t>(pm + 0x7E0, up);
    run(7);
    CHECK(ints["so2r.ok"] == 1);

    // 5. game files: pictures and the font from the synthetic bundle
    auto ge = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
    auto gd = (EdenDsmodGetDataExtensionsFn)dlsym(so, "eden_dsmod_get_data_extensions");
    auto gf = (EdenDsmodGetFontExtensionsFn)dlsym(so, "eden_dsmod_get_font_extensions");
    const EdenDsmodModuleExtensions* ext = ge ? ge(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : nullptr;
    const EdenDsmodModuleDataExtensions* dext = gd ? gd(EDEN_DSMOD_DATA_EXT_VERSION, EDEN_DSMOD_DATA_EXT_HASH) : nullptr;
    const EdenDsmodFontExtensions* fext = gf ? gf(EDEN_DSMOD_FONT_EXT_VERSION, EDEN_DSMOD_FONT_EXT_HASH) : nullptr;
    CHECK(ext && dext && fext && (m->capabilities & EDEN_DSMOD_CAP_EXTENSIONS));
    if (ext && dext && fext) {
        auto block_rgba = [](uint32_t tx, uint32_t ty, uint8_t* o) {   // the fixture's BC3 colours
            int bx = tx / 4, by = ty / 4;
            o[0] = (uint8_t)r565(bx * 2, 5); o[1] = (uint8_t)r565(by * 4, 6); o[2] = (uint8_t)r565(7, 5); o[3] = 200;
        };
        CHECK(ext->load_image(inst, &h, "module:pic/icon_dot_0001", nullptr, img_sink));
        CHECK(g_iw == 20 && g_ih == 16);
        int bad = 0;
        for (uint32_t r = 0; r < g_ih; ++r) for (uint32_t c = 0; c < g_iw; ++c) {
            uint8_t e[4]; block_rgba(8 + c, 12 + 16 - 1 - r, e);      // top row first = texture row y+h-1
            if (std::memcmp(e, &g_img[(r * g_iw + c) * 4], 4)) ++bad;
        }
        CHECK(bad == 0);
        CHECK(ext->load_image(inst, &h, "module:pic/icon_dot_0002", nullptr, img_sink));   // rotation 4
        CHECK(g_iw == 12 && g_ih == 8);
        bad = 0;
        for (uint32_t r = 0; r < 12; ++r) for (uint32_t c = 0; c < 8; ++c) {   // unrotated picture 8x12
            uint8_t e[4]; block_rgba(4 + c, 4 + 12 - 1 - r, e);
            uint32_t nr = 8 - 1 - c, nc = r;                                    // anticlockwise 90
            if (std::memcmp(e, &g_img[(nr * 12 + nc) * 4], 4)) ++bad;
        }
        CHECK(bad == 0);
        CHECK(ext->load_image(inst, &h, "module:pic/camp_icon_money", nullptr, img_sink) && g_iw == 64 && g_ih == 64);
        CHECK(!ext->load_image(inst, &h, "module:pic/not_there", nullptr, img_sink));
        {   // a tinted copy: every texel multiplied by the colour (alpha kept)
            CHECK(ext->load_image(inst, &h, "module:pic/@tint:icon_dot_0001:8040FF", nullptr, img_sink) && g_iw == 20 && g_ih == 16);
            int tb = 0;
            for (uint32_t r = 0; r < g_ih; ++r) for (uint32_t c = 0; c < g_iw; ++c) {
                uint8_t e[4]; block_rgba(8 + c, 12 + 16 - 1 - r, e);
                const uint8_t* o = &g_img[(r * g_iw + c) * 4];
                if (o[0] != e[0] * 0x80 / 255 || o[1] != e[1] * 0x40 / 255 || o[2] != e[2] || o[3] != e[3]) ++tb;
            }
            CHECK(tb == 0);
        }
        // the font: metrics through load_data + decode_font, letters through load_image
        CHECK(dext->load_data(inst, &h, "module:fontdata", nullptr, data_sink));
        CHECK(fext->decode_font(inst, g_data.data(), g_data.size(), nullptr, font_sink));
        CHECK(g_line == 32 && g_first == 0x20 && g_glyphs.size() == 224);
        CHECK(ext->load_image(inst, &h, "module:font", nullptr, img_sink) && g_iw == 1024 && g_ih == 1024);
        int fbad = 0, checked = 0;
        // 0.9.53 one cell height for every glyph: top = the highest round(bearing y), bot = the lowest bottom
        int top = 0, bot = 0;
        for (int cp = 0x21; cp < 0x7F; ++cp) {
            const int k = cp - 0x20, hh = 7 + k % 3, t = (int)std::lround(9.0f + (float)(k % 4));
            top = t > top ? t : top; bot = hh - t > bot ? hh - t : bot;
        }
        const int cell = top + bot + 4;
        for (int cp = 0x21; cp < 0x7F; ++cp) {
            const int k = cp - 0x20;
            const int w = 5 + k % 4, hh = 7 + k % 3, x = (k * 9) % 110 + 3, y = (k * 13) % 100 + 3;
            const float bx = 0.5f + (float)(k % 3), by = 9.0f + (float)(k % 4), adv = 6.25f + (float)(k % 5);
            const EdenDsmodFontGlyph& g = g_glyphs[(size_t)k];
            const int x0 = x - 2 < 0 ? 0 : x - 2, y0 = y - 2 < 0 ? 0 : y - 2;
            const int x1 = x + w + 2 > 128 ? 128 : x + w + 2, y1 = y + hh + 2 > 128 ? 128 : y + hh + 2;
            if (g.w != x1 - x0 || g.h != cell) ++fbad;
            if (g.advance != (int)std::lround(adv)) ++fbad;
            if (g.bearing_x != (int)std::lround(bx) - (x - x0) || g.bearing_y != top + 2) ++fbad;
            // the letter's rows inside its cell: same height above the baseline as before
            const int above = (int)std::lround(by) + (y1 - (y + hh)), at = top + 2 - above;
            for (int r = 0; r < g.h; ++r) for (int c = 0; c < g.w; ++c) {
                const int tx = x0 + c, ty = y1 - 1 - (r - at);
                int a = 0;
                if (r >= at && r < at + (y1 - y0)) { int v = (tx * 7 + ty * 3) & 255; a = (v - 128) * 6 + 128; }
                a = a < 0 ? 0 : a > 255 ? 255 : a;
                if (g_img[((g.y + r) * 1024 + g.x + c) * 4 + 3] != a) ++fbad;
                ++checked;
            }
        }
        CHECK(fbad == 0 && checked > 1000);
        CHECK(g_glyphs[0].advance == 6 && g_glyphs[0].w == 0);   // the space
        std::printf("  files: pictures and font OK (%d glyph pixels checked)\n", checked);
    }

    // 5b. the page's words: the game's where found, our fallback otherwise
    CHECK(texts["so2r.t.lv"] == "Nv" && texts["so2r.t.party"] == "Gruppe");
    CHECK(texts["so2r.t.hp"] == "HP" && texts["so2r.t.map"] == "Guide Map");

    // 6. the map: FieldManager / GameUIManager singletons, GameDefine, the live minimap objects
    {
        CHECK(ints["so2r.map.ok"] == 0);
        auto singleton = [&](uint64_t slot, const char* name, size_t size) {
            const uint64_t k = klass("SingletonBasicMonoBehaviour`1"), st = alloc(0x10);
            put<uint64_t>(k + 0xB8, st);
            const uint64_t o = object(klass(name), size); put<uint64_t>(st, o);
            put<uint64_t>(MAIN + slot, k);
            return o;
        };
        const uint64_t fm = singleton(0x417DAC8, "FieldManager", 0x300);
        put<float>(fm + 0x80, 10.0f); put<float>(fm + 0x84, 1.5f); put<float>(fm + 0x88, -4.0f); put<float>(fm + 0x8C, 1.0f); put<float>(fm + 0x94, 0.0f);   // facing +x
        put<int32_t>(fm + 0x184, 9);             // MF_0003_21A
        put<int32_t>(fm + 0x190, 1);             // EXPEL
        const uint64_t gui = singleton(0x417DAD0, "GameUIManager", 0x40);
        const uint64_t ufc = object(klass("UIFieldController"), 0x220);
        const uint64_t d = object(klass("Dictionary`2"), 0x40), e = object(klass("Entry[]"), 0x20 + 0x18 * 3);
        put<uint64_t>(gui + 0x28, d); put<uint64_t>(d + 0x18, e); put<int32_t>(d + 0x20, 2);
        put<int32_t>(e + 0x20 + 0, 0); put<int32_t>(e + 0x20 + 8, 0); put<uint64_t>(e + 0x20 + 0x10, object(klass("UIOther"), 0x10));
        put<int32_t>(e + 0x38 + 0, 1); put<int32_t>(e + 0x38 + 8, 1); put<uint64_t>(e + 0x38 + 0x10, ufc);
        const uint64_t gdk = klass("GameDefine"), gdst = alloc(0x200);
        put<uint64_t>(gdk + 0xB8, gdst); put<float>(gdst + 0x14C, 5.0f); put<float>(gdst + 0x150, 0.27f);
        const uint64_t gdslot = alloc(0x10); put<uint64_t>(gdslot, gdk);
        put<uint64_t>(MAIN + 0x4942D00, gdslot);       // GOT -> TypeInfo slot -> class
        const uint64_t mgr = object(klass("FieldMinimapManager"), 0x40);
        const uint64_t pres = object(klass("UIMinimapPresenter"), 0x190);
        g_test_ufc = ufc; g_test_pres = pres;
        const uint64_t task = object(klass("UIScrollMinimapBackgroundTask"), 0x70);
        put<uint64_t>(ufc + 0x1C0, mgr); put<uint64_t>(mgr + 0x10, pres); put<int32_t>(mgr + 0x18, 2);
        put<uint64_t>(pres + 0x178, task); put<float>(pres + 0x14C, 3.0f);
        put<float>(pres + 0x120, 60.0f); put<float>(pres + 0x124, 60.0f);   // the game's minimap window
        put<float>(task + 0x40, 2.0f); put<float>(task + 0x44, 1.0f); put<float>(task + 0x64, 15.0f);
        {   // the game's minimap icons: an inn (fixed), an NPC (followed), a used-off shop, a cannot-pass
            const uint64_t il = object(klass("List`1"), 0x20), ia = object(klass("UIMinimapIconPresenter[]"), 0x20 + 8 * 4);
            put<uint64_t>(pres + 0x118, il); put<uint64_t>(il + 0x10, ia); put<int32_t>(il + 0x18, 4);
            const uint64_t ipk = klass("UIMinimapIconPresenter");
            const int types[4] = {20, 3, 23, 4};   // inn, enemy (followed), off shop, NPC (never drawn)
            for (int i = 0; i < 4; ++i) {
                uint64_t ip = object(ipk, 0x190);
                put<int32_t>(ip + 0xF8, types[i]); put<uint8_t>(ip + 0x15C, i == 2 ? 0 : 1);
                put<float>(ip + 0x160, -2.0f + i); put<float>(ip + 0x168, -1.0f);
                put<uint64_t>(ia + 0x20 + 8 * i, ip);
                g_test_icons[i] = ip;
                if (i == 1) {                     // the enemy's GameImage: tinted purple by the game
                    const uint64_t gi = object(klass("GameImage"), 0x40);
                    put<float>(gi + 0x20, 1.0f); put<float>(gi + 0x24, 1.0f); put<float>(gi + 0x28, 1.0f); put<float>(gi + 0x2C, 1.0f);
                    // ... the game's enemyColor on the presenter (+0x110)
                    put<float>(ip + 0x110, 0.6f); put<float>(ip + 0x114, 0.2f); put<float>(ip + 0x118, 1.0f); put<float>(ip + 0x11C, 1.0f);
                    put<uint64_t>(ip + 0xF0, gi);
                }
                if (i == 1) {                     // followed: its follow task's cached position wins
                    uint64_t ft = object(klass("UIFollowObjectOnMinimapTask"), 0x90);
                    put<uint8_t>(ft + 0x7C, 1); put<float>(ft + 0x50, -180.0f); put<float>(ft + 0x54, 45.0f);
                    put<uint64_t>(ip + 0x148, ft);
                    // its followObject's native transform: a child at (0,0,-2) under a parent at
                    // (0,0,-1) turned 90 degrees about y -> world (-2, 0, -1), the inn's spot
                    const uint64_t mgo = alloc(0x20), ngo = alloc(0x80), comps = alloc(0x20), tr = alloc(0x50);
                    const uint64_t hier = alloc(0x30), trs = alloc(0x60), par = alloc(0x10);
                    put<uint64_t>(ft + 0x28, mgo); put<uint64_t>(mgo + 0x10, ngo); put<uint64_t>(ngo + 0x30, comps);
                    put<uint64_t>(comps + 8, tr); put<uint64_t>(tr + 0x38, hier); put<int32_t>(tr + 0x40, 1);
                    put<uint64_t>(hier + 0x18, trs); put<uint64_t>(hier + 0x20, par);
                    put<int32_t>(par + 0, -1); put<int32_t>(par + 4, 0);
                    const float h = 0.70710678f;
                    put<float>(trs + 0x08, -1.0f); put<float>(trs + 0x14, h); put<float>(trs + 0x1C, h);
                    put<float>(trs + 0x20, 1.0f); put<float>(trs + 0x24, 1.0f); put<float>(trs + 0x28, 1.0f);
                    put<float>(trs + 0x30 + 0x08, -2.0f); put<float>(trs + 0x30 + 0x1C, 1.0f);
                    put<float>(trs + 0x30 + 0x20, 1.0f); put<float>(trs + 0x30 + 0x24, 1.0f); put<float>(trs + 0x30 + 0x28, 1.0f);
                }
            }
        }
        run(7);
        CHECK(ints["so2r.map.ok"] == 1 && ints["so2r.map.kind"] == 1);
        // icons: the inn and the NPC drawn (the off one and cannot-pass not), the inn at world (-2, -1)
        // (the NPC: live, its follow task's minimap offset from the player; the inn: fixed, remembered)
        CHECK(ints["so2r.mk.n"] == 2 && texts["so2r.mk.src0"] == "module:pic/@tint:icon_map_enemy:9933FF" &&
              texts["so2r.mk.src1"] == "module:pic/icon_map_inn" && ints["so2r.mk.on2"] == 0);
        // the enemy is drawn where its object really is (the inn's spot), not from its stale offset
        CHECK(std::llabs(ints["so2r.mk.x0"] - ints["so2r.mk.x1"]) <= 1 && std::llabs(ints["so2r.mk.y0"] - ints["so2r.mk.y1"]) <= 1 && ints["so2r.mk.x0"] != 0);
        // the game's tint: the enemy purple (ARGB), the inn untinted
        CHECK(texts["so2r.mk.src0"] == "module:pic/@tint:icon_map_enemy:9933FF" && texts["so2r.mk.src1"] == "module:pic/icon_map_inn");
        {   // the inn's icon no longer used by the game (an event done): it leaves the map
            const uint64_t ia2 = g_test_icons[0];
            put<uint8_t>(ia2 + 0x15C, 0);
            run(3);
            CHECK(ints["so2r.mk.on1"] == 1);                     // 0.9.35: kept a few passes (minimap toggles)
            run(40);
            CHECK(ints["so2r.mk.on1"] == 1);                     // 0.9.36: ~3 s of misses with the minimap on
            run(400);
            CHECK(ints["so2r.mk.on1"] == 0 && texts["so2r.mk.src0"].rfind("module:pic/@tint:icon_map_enemy", 0) == 0);
            put<uint8_t>(ia2 + 0x15C, 1);
            run(3);
        }
        CHECK(texts["so2r.map.area"] == "minimap_mf_0003_21a002");
        CHECK(std::fabs(reals["so2r.map.x"] - (32 + 12 * 15)) < 1e-3 && std::fabs(reals["so2r.map.y"] - (32 - 3 * 15)) < 1e-3);
        CHECK(ext && ext->load_image(inst, &h, "module:pic/minimap_mf_0003_21a002", nullptr, img_sink) && g_iw == 64 && g_ih == 64);
        CHECK(std::fabs(reals["so2r.map.heading"] - 270) < 1e-2);
        run(1);                              // the picture cut: its drawn part is known
        // an area is shown whole until tapped -- 0.9.18: its canvas is twice the picture, so the view frames
        // (and centres) the drawn part: here the whole 64 x 64 picture, 1.12 x its height in a 128-high canvas
        const double zwhole = 1 / (64.0 / 128 * 1.12);
        std::printf("  area whole: zoom %.4f (want %.4f) centre %.4f %.4f  ax %lld\n", reals["so2r.map.zoom"], zwhole,
                    reals["so2r.map.zx"], reals["so2r.map.zy"], (long long)ints["so2r.map.ax"]);
        CHECK(ints["so2r.map.whole"] == 1 && std::fabs(reals["so2r.map.zoom"] - zwhole) < 1e-3 &&
              std::fabs(reals["so2r.map.zx"] - 0.5) < 1e-3 && std::fabs(reals["so2r.map.zy"] - 0.5) < 1e-3);
        // the sidebar layout: the map beside it (850 x 656 at 366, 222); the heading: the area's name
        CHECK(ints["so2r.side"] == 1 && texts["so2r.map.img"] == "module:pic/@map:minimap_mf_0003_21a002:0:n");
        CHECK(!texts["so2r.map.head"].empty());
        {   // the canvas: the widget's shape, the picture centred in it
            CHECK(ext && ext->load_image(inst, &h, "module:pic/@map:minimap_mf_0003_21a002:0:n", nullptr, img_sink));
            CHECK(std::fabs(double(g_iw) / g_ih - 850.0 / 656.0) < 0.03);
            CHECK(ext->load_image(inst, &h, "module:pic/@map:minimap_mf_0003_21a002:90:n", nullptr, img_sink) && g_iw > g_ih);
            // the arrow goes through the same geometry (canvas pixels): picture x 212 of 64
            const double cwn = 128 * 850.0 / 656.0, axn = 366 + ((212 - 32) / cwn * zwhole + 0.5) * 850;
            CHECK(std::abs(ints["so2r.map.ax"] - std::lround(axn)) <= 1 && ints["so2r.map.aon"] == 0);
            const int64_t v0 = ints["so2r.map.vreset"];
            run(1);
            CHECK(ints["so2r.map.vreset"] != v0);    // changes every tick: the view is re-applied every tick
        }
        CHECK(ext && ext->on_action(inst, "so2r.map_toggle", 0) && !ext->on_action(inst, "nope", 0));
        run(1);
        // tapped: the game's minimap window (60 minimap px = 60 picture px here) around the player
        CHECK(ints["so2r.map.whole"] == 0 && std::fabs(reals["so2r.map.zoom"] - 128.0 / 60.0) < 1e-3);
        CHECK(ext->on_action(inst, "so2r.map_toggle", 0));
        run(1);
        CHECK(ints["so2r.map.whole"] == 1 && std::fabs(reals["so2r.map.zoom"] - zwhole) < 1e-3);
        CHECK(ext->on_action(inst, "so2r.map_mini", 0));
        run(100);                            // let the post-switch wiggle run out
        CHECK(texts["so2r.map.area"] == "minimap_mf_0003_21a002" && std::fabs(reals["so2r.map.rot"]) < 1e-3);
        // the area's turn (ConstMinimapParameter.angle) and the live ring of recent positions
        {
            const uint64_t smp = object(klass("ScriptableMinimapParameter"), 0x30);
            const uint64_t lst = object(klass("List`1"), 0x20), arr = object(klass("ConstMinimapParameter[]"), 0x20 + 8 * 2);
            put<uint64_t>(pm + 0x750, smp); put<uint64_t>(smp + 0x18, lst);
            put<uint64_t>(lst + 0x10, arr); put<int32_t>(lst + 0x18, 2);
            const uint64_t r0 = object(klass("ConstMinimapParameter"), 0x38), r1 = object(klass("ConstMinimapParameter"), 0x38);
            put<int32_t>(r0 + 0x10, 9); put<int32_t>(r0 + 0x14, 1); put<float>(r0 + 0x18, 90.0f);
            put<int32_t>(r1 + 0x10, 9); put<int32_t>(r1 + 0x14, 2); put<float>(r1 + 0x18, 270.0f);
            put<uint64_t>(arr + 0x20, r0); put<uint64_t>(arr + 0x28, r1);
            {   // the area's name key: ConstFieldParameter.fieldmapNameID (looked up below)
                const uint64_t sfp = object(klass("ScriptableFieldParameter"), 0x30);
                const uint64_t fl = object(klass("List`1"), 0x20), fa = object(klass("ConstFieldParameter[]"), 0x20 + 8 * 2);
                put<uint64_t>(pm + 0x508, sfp); put<uint64_t>(sfp + 0x18, fl); put<uint64_t>(fl + 0x10, fa); put<int32_t>(fl + 0x18, 2);
                const uint64_t f0 = object(klass("ConstFieldParameter"), 0x50), f1 = object(klass("ConstFieldParameter"), 0x50);
                put<int32_t>(f0 + 0x14, 3); put<uint64_t>(f0 + 0x20, il2str("LOC_0001"));
                put<int32_t>(f1 + 0x14, 9); put<uint64_t>(f1 + 0x20, il2str("LOC_0007")); put<uint64_t>(f1 + 0x28, il2str("LOC_0001"));
                put<uint64_t>(fa + 0x20, f0); put<uint64_t>(fa + 0x28, f1);
            }
            const uint64_t fp = object(klass("FieldPlayer"), 0x240), ring = object(klass("Vector3[]"), 0x20 + 12 * 16);
            put<uint64_t>(fm + 0x78, fp); put<uint64_t>(fp + 0x230, ring); put<int32_t>(fp + 0x238, 5);
            put<int32_t>(ring + 0x18, 16);
            put<float>(ring + 0x20 + 4 * 12, 12.0f); put<float>(ring + 0x20 + 4 * 12 + 8, -5.0f);    // newest
            put<float>(ring + 0x20 + 13 * 12, 12.0f); put<float>(ring + 0x20 + 13 * 12 + 8, -9.0f);  // 8 back
            run(7);
            CHECK(std::fabs(reals["so2r.map.rot"] - 90) < 1e-3);
            CHECK(std::fabs(reals["so2r.map.x"] - (32 + 14 * 15)) < 2e-2 && std::fabs(reals["so2r.map.y"] - (32 - 4 * 15)) < 1e-3);
            CHECK(std::fabs(reals["so2r.map.heading"] - 180) < 1e-3);
            put<uint64_t>(fm + 0x78, 0);         // no player: back to FieldManager's position
            run(7);
            CHECK(std::fabs(reals["so2r.map.x"] - (32 + 12 * 15)) < 1e-3);            // the area's name: its row's messageID in locationMessageDictionary (+0x390)
            const uint64_t ld = object(klass("Dictionary`2"), 0x40), le = object(klass("Entry[]"), 0x20 + 0x18 * 2);
            put<uint64_t>(pm + 0x390, ld); put<uint64_t>(ld + 0x18, le); put<int32_t>(ld + 0x20, 2);
            const uint64_t clm = object(klass("ConstLocationMessage"), 0x28);
            put<uint64_t>(clm + 0x10, il2str("LOC_0007")); put<uint64_t>(clm + 0x18, il2str("Arlia"));
            put<int32_t>(le + 0x20, 5); put<uint64_t>(le + 0x28, il2str("LOC_0001"));
            put<int32_t>(le + 0x38, 6); put<uint64_t>(le + 0x40, il2str("LOC_0007")); put<uint64_t>(le + 0x48, clm);
            run(400);                            // the name is retried every ~30 reads until found
            CHECK(texts["so2r.map.place"] == "Arlia" && ints["so2r.map.hasplace"] == 1);
        }
        put<int32_t>(mgr + 0x18, 7);             // a minimap the area does not have
        run(7);
        CHECK(ints["so2r.map.ok"] == 0 && texts["so2r.map.why"] == "this area has no minimap");
        put<int32_t>(mgr + 0x18, 2);
        run(7);
        put<int32_t>(gsm + 0x1C, 6);             // battle: the map stays where it was
        run(7);
        CHECK(ints["so2r.map.ok"] == 1 && texts["so2r.map.area"] == "minimap_mf_0003_21a002");
        CHECK(texts["so2r.map.title"] == "Current Location");
        // the world map
        put<int32_t>(gsm + 0x1C, 5);
        const uint64_t wp = object(klass("UIWorldMinimapPresenter"), 0x190);
        put<uint64_t>(ufc + 0x110, wp);
        put<float>(wp + 0x150, 560.0f); put<float>(wp + 0x154, 440.0f);
        put<float>(wp + 0x158, 30.0f); put<float>(wp + 0x15C, -20.0f);
        put<float>(wp + 0x14C, 1.0f); put<float>(wp + 0x170, 1.0f);
        run(7);
        CHECK(ints["so2r.map.ok"] == 1 && ints["so2r.map.kind"] == 2 && texts["so2r.map.area"] == "map_expel");
        CHECK(texts["so2r.map.title"] == "World Map");
        CHECK(std::fabs(reals["so2r.map.x"] - (32 + (10 * 0.27 + 30) * (64.0 / 560))) < 1e-3);
        CHECK(std::fabs(reals["so2r.map.y"] - (32 + (-4 * 0.27 - 20) * (64.0 / 560))) < 1e-3);
        // the game's own placement: UICommon.pvMatrix onto the world-map texture
        {
            const uint64_t uk = klass("UICommon"), ust = alloc(0x100), slot = alloc(0x10);
            put<uint64_t>(uk + 0xB8, ust); put<uint64_t>(slot, uk); put<uint64_t>(MAIN + 0x4947080, slot);
            put<float>(ust + 0x78 + 0 * 4, 0.001f);     // m00: ndc.x = x / 1000
            put<float>(ust + 0x78 + 9 * 4, 0.002f);     // m12: ndc.y = z / 500
            put<float>(ust + 0x78 + 15 * 4, 1.0f);      // m33
            const uint64_t wm = object(klass("WorldMinimapManager"), 0x30);
            put<uint64_t>(ufc + 0x1C8, wm); put<float>(wm + 0x1C, 560.0f); put<float>(wm + 0x20, 440.0f);
            run(7);
            CHECK(std::fabs(reals["so2r.map.x"] - (32 + (10 / 1000.0 * 280) * (64.0 / 560))) < 2e-2);
            CHECK(std::fabs(reals["so2r.map.y"] - (32 + (-4 / 500.0 * 220) * (64.0 / 440))) < 1e-3);
        }
        // the world minimap (default) vs the whole world (tap)
        CHECK(ints["so2r.map.whole"] == 0 && texts["so2r.map.img"] == "module:pic/@map:map_expel:0:n");
        CHECK(ext->on_action(inst, "so2r.map_toggle", 0));
        run(1);
        CHECK(ints["so2r.map.whole"] == 1 && reals["so2r.map.zoom"] >= 1);
        CHECK(ext->on_action(inst, "so2r.map_toggle", 0));
        run(1);
        CHECK(ints["so2r.map.whole"] == 0);
        put<int32_t>(gsm + 0x1C, 3);             // back to the title: no map
        run(7);
        CHECK(ints["so2r.map.ok"] == 0);
        put<int32_t>(gsm + 0x1C, 4);
        run(7);
        CHECK(ints["so2r.map.ok"] == 1);
        std::printf("  map: area and world map OK\n");
    }

    {   // 0.9.24 the Guide Map's next objective: hero 1, progress 1200500 -> DESTINATION_0020 (rows for hero 2 and
        // an earlier range are passed over); its text through destinationMessageDictionary (+0x3B0)
        const uint64_t up0 = rd64(pm + 0x7E0);
        put<int32_t>(up0 + 0xD0, 1200500);
        const uint64_t sdp = object(klass("ScriptableDestinationParameter"), 0x20), dl = object(klass("List`1"), 0x20);
        const uint64_t da = object(klass("ConstDestinationParameter[]"), 0x20 + 8 * 3);
        const int rowsv[3][3] = {{2, 1200000, 1300000}, {1, 1100000, 1200000}, {1, 1200000, 1201000}};
        const char* idsv[3] = {"DESTINATION_0099", "DESTINATION_0019", "DESTINATION_0020"};
        for (int i = 0; i < 3; ++i) {
            const uint64_t r = object(klass("ConstDestinationParameter"), 0x30);
            put<int32_t>(r + 0x10, rowsv[i][0]); put<uint64_t>(r + 0x18, il2str(idsv[i]));
            put<int32_t>(r + 0x20, rowsv[i][1]); put<int32_t>(r + 0x24, rowsv[i][2]);
            put<uint64_t>(da + 0x20 + 8 * i, r);
        }
        put<uint64_t>(pm + 0x718, sdp); put<uint64_t>(sdp + 0x18, dl); put<uint64_t>(dl + 0x10, da); put<int32_t>(dl + 0x18, 3);
        const uint64_t dd = object(klass("Dictionary`2"), 0x40), de = object(klass("Entry[]"), 0x20 + 0x18);
        const uint64_t dm = object(klass("ConstDestinationMessage"), 0x20);
        put<uint64_t>(dm + 0x10, il2str("DESTINATION_0020")); put<uint64_t>(dm + 0x18, il2str("Go to Krosse Cave."));
        put<int32_t>(de + 0x20, 77); put<uint64_t>(de + 0x28, il2str("DESTINATION_0020")); put<uint64_t>(de + 0x30, dm);
        put<uint64_t>(pm + 0x3B0, dd); put<uint64_t>(dd + 0x18, de); put<int32_t>(dd + 0x20, 1);
        run(30);
        CHECK(texts["so2r.map.obj"] == "Go to Krosse Cave." && ints["so2r.map.hasobj"] == 1);
        put<int32_t>(up0 + 0xD0, 1201000); run(30);                   // past its end: no row, no objective
        CHECK(texts["so2r.map.obj"].empty() && ints["so2r.map.hasobj"] == 0);
    }
    {   // 0.9.19 the quick menu: the game's open windows (GameUIManager -> WindowController -> openWindowStack)
        const uint64_t P = alloc(8), A = alloc(0x40), K = klass("SingletonMonoBehaviour`1"), R = alloc(8);
        const uint64_t K2 = klass("SingletonMonoBehaviour`1"), ST = alloc(0x10);
        const uint64_t gui = object(klass("GameUIManager"), 0x40), wc = object(klass("WindowController"), 0x30);
        const uint64_t stk = object(klass("Stack`1"), 0x28), arr = object(klass("OpenWindowStackData[]"), 0x20 + 8 * 4);
        const uint64_t w3 = object(klass("OpenWindowStackData"), 0x18);
        put<uint64_t>(MAIN + 0x4946C20, P); put<uint64_t>(P, A); put<uint64_t>(A + 0x20, K); put<uint64_t>(K + 0xC0, R);
        put<uint64_t>(R, K2); put<uint64_t>(K2 + 0xB8, ST); put<uint64_t>(ST, gui); put<uint64_t>(gui + 0x18, wc);
        put<uint64_t>(wc + 0x18, stk); put<uint64_t>(stk + 0x10, arr); put<int32_t>(stk + 0x18, 0);
        put<int32_t>(w3 + 0x10, 3); put<uint64_t>(arr + 0x20, w3);
        run(30);
        CHECK(ints["so2r.q.open"] == 0 && ints["so2r.q.any"] == 0);
        // nothing open: Items (d-pad left) at once, exactly one press
        CHECK(ext->on_action(inst, "so2r.qtap1", 0));
        run(1);
        CHECK(ints["so2r.q.f1"] == 1 && ints["so2r.q.any"] == 1);
        g_flag_qp = 1; run(1);                                   // the manifest pressed it (the flag toggled)
        CHECK(ints["so2r.q.any"] == 0);
        put<int32_t>(stk + 0x18, 1); run(30);                    // the camp window (3) opens
        CHECK(ints["so2r.q.open"] == 1);
        // Items open, Guide Map tapped: B, then (closed) d-pad up
        CHECK(ext->on_action(inst, "so2r.qtap0", 0));
        run(30);
        CHECK(ints["so2r.q.f6"] == 1);
        g_flag_qp = 0; put<int32_t>(stk + 0x18, 0); run(1);
        CHECK(ints["so2r.q.any"] == 0);
        run(30);
        CHECK(ints["so2r.q.f0"] == 1);
        g_flag_qp = 1; put<int32_t>(stk + 0x18, 1); run(30);
        CHECK(ints["so2r.q.any"] == 0);
        // Guide Map tapped again while it is open: B only
        CHECK(ext->on_action(inst, "so2r.qtap0", 0));
        run(30);
        CHECK(ints["so2r.q.f6"] == 1);
        g_flag_qp = 0; put<int32_t>(stk + 0x18, 0); run(60);
        CHECK(ints["so2r.q.any"] == 0 && ints["so2r.q.f0"] == 0);
        // 0.9.25 Switch PA (Y) with nothing open: Y at once; with a window open: B, then Y
        CHECK(ext->on_action(inst, "so2r.qtap5", 0));
        run(30);
        CHECK(ints["so2r.q.f5"] == 1);
        g_flag_qp = 1; run(30);
        CHECK(ints["so2r.q.any"] == 0);
        put<int32_t>(stk + 0x18, 1);
        CHECK(ext->on_action(inst, "so2r.qtap5", 0));
        run(30);
        CHECK(ints["so2r.q.f6"] == 1);
        g_flag_qp = 0; put<int32_t>(stk + 0x18, 0); run(30);
        CHECK(ints["so2r.q.f5"] == 1);
        g_flag_qp = 1; run(30);
        CHECK(ints["so2r.q.any"] == 0);

        // 0.9.60 Party Formation: make Hero (pid 1, a battle member) the leader through the game's own menus.
        // The windows by type (WindowController.windowDictionary +0x10), the Camp Menu, its Party Formation.
        const uint64_t wd = object(klass("Dictionary`2"), 0x40), we = object(klass("Entry[]"), 0x20 + 0x18);
        const uint64_t cw = object(klass("UICampWindow"), 0x300), ms = object(klass("UICampMenuSelector"), 0x110);
        const uint64_t cs = object(klass("UICampMenuSelectCharacterSelector"), 0x110), sp = alloc(0x180);
        const uint64_t cd = object(klass("Dictionary`2"), 0x40), ce = object(klass("Entry[]"), 0x20 + 0x18 * 3);
        const uint64_t pr[3] = {alloc(0x40), alloc(0x40), alloc(0x40)};
        const int pids[3] = {2, 1, 3};
        put<uint64_t>(wc + 0x10, wd); put<uint64_t>(wd + 0x18, we); put<int32_t>(wd + 0x20, 1);
        put<int32_t>(we + 0x20, 3); put<int32_t>(we + 0x24, -1); put<int32_t>(we + 0x28, 3); put<uint64_t>(we + 0x30, cw);
        put<uint64_t>(cw + 0xC0, ms); put<uint64_t>(ms + 0xD8, cs); put<uint64_t>(cs + 0x90, sp);
        put<uint64_t>(cs + 0xD8, cd); put<uint64_t>(cd + 0x18, ce); put<int32_t>(cd + 0x20, 3);
        for (int i = 0; i < 3; ++i) { const uint64_t e = ce + 0x20 + 0x18 * i; put<int32_t>(e, i); put<uint64_t>(e + 8, pr[i]); put<int32_t>(e + 0x10, pids[i]); }
        put<uint64_t>(cs + 0xF8, pr[0]);
        CHECK(ext->on_action(inst, "so2r.open1", 0) && ext->on_action(inst, "so2r.sub0", 0));
        run(1);
        CHECK(ints["so2r.f.on"] == 1 && ints["so2r.f.can"] == 1 && texts["so2r.f.name"] == "Hero");
        CHECK(ext->on_action(inst, "so2r.lead", 0));
        run(40);                                                  // nothing open: the Camp Menu (X)
        CHECK(ints["so2r.q.f4"] == 1 && ints["so2r.f.busy"] == 1);
        g_flag_qp = 0; put<int32_t>(stk + 0x18, 1); put<int32_t>(ms + 0xE8, 0); run(60);   // open: Y
        CHECK(ints["so2r.q.f5"] == 1);
        g_flag_qp = 1; run(50);                                   // 0.9.64 the game didn't take it: Y again
        CHECK(ints["so2r.q.f5"] == 1);
        g_flag_qp = 0; put<int32_t>(ms + 0xE8, 6); run(60);       // Party Formation, the cursor on Rena: right
        CHECK(ints["so2r.q.f3"] == 1);
        put<uint8_t>(cs + 0xF0, 1);                               // the game busy: nothing pressed meanwhile
        g_flag_qp = 1; put<uint64_t>(cs + 0xF8, pr[1]); run(40);
        CHECK(ints["so2r.q.any"] == 0);
        put<uint8_t>(cs + 0xF0, 0); run(30);                      // on Hero: X (Leader)
        CHECK(ints["so2r.q.f4"] == 1);
        g_flag_qp = 0; put<int32_t>(pp + 0x18, 1); run(40);       // the game's leader is Hero: B to close
        CHECK(ints["so2r.q.f6"] == 1);
        g_flag_qp = 1; put<int32_t>(stk + 0x18, 0); run(60);
        CHECK(ints["so2r.q.any"] == 0 && ints["so2r.f.busy"] == 0 && ints["so2r.f.lead"] == 1 && ints["so2r.f.can"] == 0);
        put<int32_t>(pp + 0x18, 2); run(12);                       // (Rena leads again for the rest)

        // 0.9.64 a Missions page row: open it in the game (Camp Menu, its Missions row, the Missions list row,
        // the Missions window's tab and cursor). Row 1 of Beginner is mission 23 ("Use Art 30 times!").
        auto list_of = [&](std::vector<uint64_t> items) {
            const uint64_t l = object(klass("List`1"), 0x20), a = object(klass("Object[]"), 0x20 + 8 * items.size());
            put<uint64_t>(l + 0x10, a); put<int32_t>(l + 0x18, (int32_t)items.size());
            for (size_t i = 0; i < items.size(); ++i) put<uint64_t>(a + 0x20 + 8 * i, items[i]);
            return l;
        };
        auto menu_item = [&](int it) { const uint64_t o = object(klass("UICampMenuItemData"), 0x40); put<int32_t>(o + 0x14, it); return o; };
        put<uint64_t>(ms + 0x98, list_of({menu_item(1), menu_item(23)})); put<int32_t>(ms + 0x8C, 0); put<int32_t>(ms + 0xE8, 0);
        CHECK(ext->on_action(inst, "so2r.mtab0", 0) && ext->on_action(inst, "so2r.msec0", 0));
        run(2);
        CHECK(texts["so2r.m.n1"] == "Use Art 30 times!");
        CHECK(ext->on_action(inst, "so2r.mrow1", 0));
        run(40);                                                  // nothing open: X
        CHECK(ints["so2r.q.f4"] == 1 && ints["so2r.m.sel1"] == 1);
        g_flag_qp = 0; put<int32_t>(stk + 0x18, 1); run(40);      // the Camp Menu, on Items: down to Missions
        CHECK(ints["so2r.q.f2"] == 1);
        g_flag_qp = 1; put<int32_t>(ms + 0x8C, 1); run(30);       // on it: A
        CHECK(ints["so2r.q.f7"] == 1);
        g_flag_qp = 0; put<int32_t>(ms + 0xE8, 4);                // its submenu: Guild Missions, Challenge Missions
        put<uint64_t>(ms + 0x98, list_of({menu_item(24), menu_item(25)})); put<int32_t>(ms + 0x8C, 0); run(40);
        CHECK(ints["so2r.q.f2"] == 1);
        g_flag_qp = 1; put<int32_t>(ms + 0x8C, 1); run(30);
        CHECK(ints["so2r.q.f7"] == 1);
        // the Missions window (13) on top: its list of mission 7 and 23, the cursor on 7
        const uint64_t w13 = object(klass("OpenWindowStackData"), 0x18), mw = object(klass("UIMissionWindow"), 0x100);
        const uint64_t mls = object(klass("UIMissionListSelector"), 0x110);
        const uint64_t wd2 = object(klass("Dictionary`2"), 0x40), we2 = object(klass("Entry[]"), 0x20 + 0x18 * 2);
        put<int32_t>(w13 + 0x10, 13); put<uint64_t>(arr + 0x28, w13);
        put<uint64_t>(wc + 0x10, wd2); put<uint64_t>(wd2 + 0x18, we2); put<int32_t>(wd2 + 0x20, 2);
        put<int32_t>(we2 + 0x20, 3); put<int32_t>(we2 + 0x28, 3); put<uint64_t>(we2 + 0x30, cw);
        put<int32_t>(we2 + 0x38, 13); put<int32_t>(we2 + 0x40, 13); put<uint64_t>(we2 + 0x48, mw);
        put<uint64_t>(mw + 0xC0, mls); put<int32_t>(mls + 0xF8, 0); put<int32_t>(mls + 0x8C, 0);
        auto mis_item = [&](int id) {
            const uint64_t o = object(klass("UIMissionListItemData"), 0x40), mp = object(klass("ConstMissionParameter"), 0x40);
            put<int32_t>(mp + 0x10, id); put<uint64_t>(o + 0x30, mp); return o;
        };
        put<uint64_t>(mls + 0x98, list_of({mis_item(7), mis_item(23)}));
        g_flag_qp = 0; put<int32_t>(stk + 0x18, 2); run(90);      // the right tab already: down to 23
        CHECK(ints["so2r.q.f2"] == 1);
        g_flag_qp = 1; put<int32_t>(mls + 0x8C, 1); run(40);      // on it: done, left open in the game
        CHECK(ints["so2r.q.any"] == 0 && ints["so2r.m.sel1"] == 0);
        // 0.9.66 tapped again with the game's Missions list still open: no X, no closing; the cursor up to 7 is
        // not needed (row 1 = 23 again, already on it): nothing pressed at all
        CHECK(ext->on_action(inst, "so2r.mrow1", 0));
        bool pressed = false;
        for (int i = 0; i < 60; ++i) { run(1); pressed |= ints["so2r.q.any"] == 1; }
        CHECK(!pressed && ints["so2r.m.sel1"] == 0);
        put<int32_t>(mls + 0x8C, 0); run(2);                      // the cursor moved away in the game: up again
        CHECK(ext->on_action(inst, "so2r.mrow1", 0));
        run(40);
        CHECK(ints["so2r.q.f2"] == 1 && ints["so2r.q.f4"] == 0 && ints["so2r.q.f6"] == 0);
        g_flag_qp = 0; put<int32_t>(mls + 0x8C, 1); run(40);
        CHECK(ints["so2r.q.any"] == 0 && ints["so2r.m.sel1"] == 0);
        put<int32_t>(stk + 0x18, 0); run(10);
    }
    // 7. diag lines only in the diag build
    if (diag) {
        CHECK(ints["so2r.d.n"] >= 10 && ints["so2r.d.n"] <= 38);
        CHECK(texts["so2r.d.l1"].find("SAME") != std::string::npos);
        for (int i = 0; i < ints["so2r.d.n"]; ++i) std::printf("  d.l%d: %s\n", i, texts["so2r.d.l" + std::to_string(i)].c_str());
    } else {
        CHECK(!ints.count("so2r.d.n") && !texts.count("so2r.d.l0"));
    }

    // 7b. Hide HUD: Destroy puts a hidden piece back (its own scale, through the mailbox) and the switch off
    {
        auto gxe = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
        const EdenDsmodModuleExtensions* hx = gxe ? gxe(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : nullptr;
        CHECK(hx && hx->configure && hx->on_action);
        if (hx && hx->configure && hx->on_action && g_test_ufc && rd64(MAIN + 0x417DAD0)) {
            EdenDsmodHostExtensions xh{};
            xh.version = EDEN_DSMOD_EXT_VERSION; xh.struct_size = sizeof xh; xh.abi_hash = EDEN_DSMOD_EXT_HASH;
            xh.load_u32 = mb_load; xh.store_u32 = mb_store; xh.store_u64 = mb_store64;
            hx->configure(inst, &xh);
            put<uint64_t>(g_test_pres + 0x10, 1);                  // a live native object
            put<uint64_t>(g_test_ufc + 0x108, g_test_pres);        // UIFieldController.minimap piece = entry 2
            const uint32_t e = 0x100 + 0x30 * 2;
            auto fl = [&](uint32_t o) { float f; std::memcpy(&f, &g_mb[o / 4], 4); return f; };
            CHECK(hx->on_action(inst, "so2r.hud_hide", 0));
            for (int i = 0; i < 12; ++i) {                          // the guest helper's part: read the scale, count frames
                run(1);
                if (g_mb[(e + 0x10) / 4] == 1) { const float v = 0.8f; for (int k = 0; k < 3; ++k) std::memcpy(&g_mb[(e + 0x18) / 4 + k], &v, 4); ++g_mb[(e + 0x14) / 4]; }
            }
            CHECK(g_mb[0] == 0x48324F53 && g_mb[(e + 0x10) / 4] == 2 && fl(e + 0x24) == 0.0f);   // hidden
            m->destroy(inst);
            CHECK(g_mb[(e + 0x10) / 4] == 2 && std::fabs(fl(e + 0x24) - 0.8f) < 1e-6f && std::fabs(fl(e + 0x2C) - 0.8f) < 1e-6f);   // its own scale back
            inst = m->create(&h, "{}");                              // a new instance starts over
            CHECK(inst);
        }
    }

    // 8. a new boot (main moved) starts over without crashing
    h.main_base = MAIN + 0x1000;
    run(2);
    CHECK(ints["so2r.ok"] == 0);
    m->destroy(inst);

    std::printf("%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
