// Synthetic test for the Dave the Diver module: builds a fake guest memory image with the
// IL2CPP objects the module walks (SaveSystem singleton -> game data manager -> game save ->
// Dictionary<string, LootBoxSlot>), loads the linux-x86_64 .so and checks what it publishes.
//
//   clang++ -std=c++17 -I <eden-duo>/src native/modules/dtd/dtd_module_test.cpp -ldl -o /tmp/dtd_test
//   /tmp/dtd_test build-dtd/linux-x86_64/010097F018538000.so
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

static std::vector<uint8_t> mem(0x400000);
static const uint64_t BASE = 0x80000000;   // guest address of mem[0]
static uint64_t bump = 0x1000;
static uint64_t alloc(size_t n) { uint64_t a = BASE + bump; bump += (n + 15) & ~size_t(15); return a; }
template <class T> static void put(uint64_t a, T v) { std::memcpy(&mem[a - BASE], &v, sizeof v); }
static uint64_t cstr(const char* s) { uint64_t a = alloc(strlen(s) + 1); std::memcpy(&mem[a - BASE], s, strlen(s) + 1); return a; }
static uint64_t klass(const char* name) { uint64_t k = alloc(0x100); put<uint64_t>(k + 0x10, cstr(name)); return k; }
static uint64_t object(uint64_t k, size_t size) { uint64_t o = alloc(size); put<uint64_t>(o, k); return o; }
static uint64_t il2str(const char* s) {
    uint64_t o = object(klass("String"), 0x14 + 2 * strlen(s) + 2);
    put<int32_t>(o + 0x10, (int32_t)strlen(s));
    for (size_t i = 0; i < strlen(s); ++i) put<uint16_t>(o + 0x14 + 2 * i, (uint16_t)s[i]);
    return o;
}
static void obscured(uint64_t a, int32_t v, int32_t key) { put<int32_t>(a, key); put<int32_t>(a + 4, v ^ key); put<uint8_t>(a + 8, 1); }


// ---- a small UnityFS bundle for the game-file pictures test (round 15)
static std::vector<uint8_t> lz4c(const std::vector<uint8_t>& d) {   // greedy LZ4 block compressor
    std::vector<uint8_t> o; std::map<uint32_t, size_t> tab; size_t i = 0, anchor = 0, n = d.size();
    auto len = [&](size_t v) { while (v >= 255) { o.push_back(255); v -= 255; } o.push_back(static_cast<uint8_t>(v)); };
    auto seq = [&](size_t a, size_t b, size_t off, size_t ml) {
        size_t L = b - a; o.push_back(static_cast<uint8_t>((std::min<size_t>(L, 15) << 4) | (ml ? std::min<size_t>(ml - 4, 15) : 0)));
        if (L >= 15) len(L - 15);
        o.insert(o.end(), d.begin() + a, d.begin() + b);
        if (ml) { o.push_back(off & 255); o.push_back(off >> 8); if (ml - 4 >= 15) len(ml - 4 - 15); }
    };
    while (n >= 9 && i + 4 <= n - 5) {
        uint32_t k; std::memcpy(&k, &d[i], 4);
        auto it = tab.find(k); size_t j = it == tab.end() ? SIZE_MAX : it->second; tab[k] = i;
        if (j != SIZE_MAX && i - j < 65536) {
            size_t ml = 4; while (i + ml < n - 5 && d[j + ml] == d[i + ml]) ++ml;
            seq(anchor, i, i - j, ml); i += ml; anchor = i;
        } else ++i;
    }
    seq(anchor, n, 0, 0);
    return o;
}
static void be32(std::vector<uint8_t>& v, uint32_t x) { for (int k = 3; k >= 0; --k) v.push_back(static_cast<uint8_t>(x >> (k * 8))); }
static void be64(std::vector<uint8_t>& v, uint64_t x) { be32(v, static_cast<uint32_t>(x >> 32)); be32(v, static_cast<uint32_t>(x)); }
static std::vector<uint8_t> g_bundle;
static std::string g_pidx;
static size_t g_font_g, g_font_c, g_font_t;
static uint8_t font_px(int col, int row) { return static_cast<uint8_t>(1 + ((col * 7 + row * 3) % 250)); }   // the atlas, stored rows
// texture 64x64 ASTC 4x4, Switch block-linear with 2 GOBs per block, written the way UnityPy's
// swizzle() writes it; each 16-byte block holds its own block coordinates (bx, by)
static void make_bundle() {
    const int bxn = 16, byn = 16, gobs = 2;
    std::vector<uint8_t> lin(bxn * byn * 16), swz(lin.size());
    for (int by = 0; by < byn; ++by) for (int bx = 0; bx < bxn; ++bx) { lin[(by * bxn + bx) * 16] = bx; lin[(by * bxn + bx) * 16 + 1] = by; lin[(by * bxn + bx) * 16 + 2] = 0x5A; }
    size_t w = 0;
    for (int i = 0; i < byn / 8 / gobs; ++i) for (int j = 0; j < bxn / 4; ++j) for (int k = 0; k < gobs; ++k)
        for (int v = 0; v < 32; ++v) {
            int gx = ((v >> 3) & 2) | ((v >> 1) & 1), gy = ((v >> 1) & 6) | (v & 1);
            int src = ((i * gobs + k) * 8 + gy) * bxn + j * 4 + gx;
            std::memcpy(&swz[w], &lin[src * 16], 16); w += 16;
        }
    std::vector<uint8_t> stream(300, 0xEE);            // other files of the bundle first
    stream.insert(stream.end(), swz.begin(), swz.end());
    stream.resize(stream.size() + 100, 0xDD);
    // the game font (0.24.0): TMP glyph table (stride 48), character table (stride 16) and an
    // Alpha8 atlas 64x64, block-linear (16 pixels per 16-byte block, 2 GOBs)
    auto le32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) stream.push_back(static_cast<uint8_t>(v >> (k * 8))); };
    auto lef = [&](float f) { uint32_t u; std::memcpy(&u, &f, 4); le32(u); };
    g_font_g = stream.size();
    struct G { uint32_t idx; float w, h, bx, by, adv; uint32_t x, y, rw, rh; } gl[4] = {
        {1, 10, 20, 1, 20, 12, 4, 8, 10, 20}, {2, 6, 7, 0.5f, 7, 8, 30, 40, 6, 7}, {3, 0, 0, 0, 0, 5, 0, 0, 0, 0}, {9, 4, 4, 0, 4, 5, 50, 50, 4, 4}};
    for (auto& g : gl) { le32(g.idx); lef(g.w); lef(g.h); lef(g.bx); lef(g.by); lef(g.adv); le32(g.x); le32(g.y); le32(g.rw); le32(g.rh); lef(1.0f); le32(0); }
    g_font_c = stream.size();
    const uint32_t ch[4][2] = {{'H', 1}, {'a', 2}, {' ', 3}, {0x3042, 9}};
    for (auto& c : ch) { le32(1); le32(c[0]); le32(c[1]); lef(1.0f); }
    {
        const int fbxn = 4, fbyn = 64, fg = 2;
        std::vector<uint8_t> flin(fbxn * fbyn * 16), fswz(flin.size());
        for (int r = 0; r < 64; ++r) for (int c = 0; c < 64; ++c) flin[r * 64 + c] = font_px(c, r);
        size_t q = 0;
        for (int i = 0; i < fbyn / 8 / fg; ++i) for (int j = 0; j < fbxn / 4; ++j) for (int k = 0; k < fg; ++k)
            for (int v = 0; v < 32; ++v) {
                int gx = ((v >> 3) & 2) | ((v >> 1) & 1), gy = ((v >> 1) & 6) | (v & 1);
                int src = ((i * fg + k) * 8 + gy) * fbxn + j * 4 + gx;
                std::memcpy(&fswz[q], &flin[src * 16], 16); q += 16;
            }
        g_font_t = stream.size();
        stream.insert(stream.end(), fswz.begin(), fswz.end());
    }
    stream.resize(stream.size() + 64, 0xCC);
    // data blocks of 1500 bytes (so a picture spans several), LZ4 (flag 2)
    std::vector<std::vector<uint8_t>> comp; std::vector<uint32_t> us;
    for (size_t o = 0; o < stream.size(); o += 1500) {
        std::vector<uint8_t> part(stream.begin() + o, stream.begin() + std::min(stream.size(), o + 1500));
        us.push_back(static_cast<uint32_t>(part.size())); comp.push_back(lz4c(part));
    }
    std::vector<uint8_t> info(16, 0); be32(info, static_cast<uint32_t>(comp.size()));
    for (size_t k = 0; k < comp.size(); ++k) { be32(info, us[k]); be32(info, static_cast<uint32_t>(comp[k].size())); info.push_back(0); info.push_back(2); }
    be32(info, 1); be64(info, 0); be64(info, stream.size()); be32(info, 4); for (char c : std::string("CAB-test")) info.push_back(c); info.push_back(0);
    std::vector<uint8_t> cinfo = lz4c(info);
    std::vector<uint8_t> h; for (char c : std::string("UnityFS")) h.push_back(c); h.push_back(0);
    be32(h, 7); for (char c : std::string("5.x.x")) h.push_back(c); h.push_back(0);
    for (char c : std::string("2021.3.15f1")) h.push_back(c); h.push_back(0);
    const size_t total_at = h.size(); be64(h, 0); be32(h, static_cast<uint32_t>(cinfo.size())); be32(h, static_cast<uint32_t>(info.size()));
    be32(h, 0x243);                                   // LZ4HC info, combined, padding before the data
    while (h.size() % 16) h.push_back(0);
    h.insert(h.end(), cinfo.begin(), cinfo.end());
    while (h.size() % 16) h.push_back(0);
    for (auto& c : comp) h.insert(h.end(), c.begin(), c.end());
    for (int k = 0; k < 8; ++k) h[total_at + k] = static_cast<uint8_t>(uint64_t{h.size()} >> ((7 - k) * 8));
    g_bundle = h;
    g_pidx = "DTDP 1\nB 0 Data/test.bundle " + std::to_string(h.size()) + "\nT 0 0 300 4096 64 64 48 2\n"
             "S Pic_A 0 10 6 20 12 0\nS Pic_B_Thumbnail 0 33 40 7 9 4\n"
             "T 1 0 " + std::to_string(g_font_t) + " 4096 64 64 1 2\n"
             "F Test_Font 1 1 " + std::to_string(g_font_g) + " 4 48 " + std::to_string(g_font_c) + " 4 16 0 0\n"
             "F Snowstorm_SDF 1 1 " + std::to_string(g_font_g) + " 4 48 " + std::to_string(g_font_c) + " 1 16 0 0\n";   // the HUD font: only H
}
static std::map<std::string, std::string> out;
static uint64_t main_base = 0;
static EdenDsmodBool is_mapped(void*, uint64_t a, uint64_t n) { return a >= BASE && a + n <= BASE + mem.size(); }
static EdenDsmodBool read_memory(void*, uint64_t a, void* o, size_t n) {
    if (!is_mapped(nullptr, a, n)) return 0; std::memcpy(o, &mem[a - BASE], n); return 1;
}
static uint64_t g_tmp = 0, g_panel = 0;   // the popup's TextMeshPro (the discard simulation rewrites its text)
static void pub_i(void*, const char* n, int64_t v) { out[n] = std::to_string(v); }
static void pub_t(void*, const char* n, const char* v) { out[n] = v; }
static void logf(void*, uint32_t, const char* m) { std::printf("log: %s\n", m); }

static uint8_t mb[0x1000];   // Eden's mailbox (the discard and menu tests)
uint64_t g_test_rec_list;   // Recipe_Tuna's ingredient list (the menu test trims it to the rice)
int main(int argc, char** argv) {
    void* so = dlopen(argv[1], RTLD_NOW);
    if (!so) { std::printf("dlopen: %s\n", dlerror()); return 1; }
    auto get = (EdenDsmodGetModuleFn)dlsym(so, "eden_dsmod_get_module");
    const EdenDsmodModuleApi* m = get(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    if (!m || m->struct_size != sizeof(EdenDsmodModuleApi)) { std::puts("bad module table"); return 1; }
    std::printf("module: %s caps=%llx\n", m->name, (unsigned long long)m->capabilities);
    if (!m->supports_build("F74B633E480E4E161F80A7C16A467F2A00000000000000000000000000000000") ||
        m->supports_build("0000000000000000")) { std::puts("supports_build wrong"); return 1; }

    // main image: TypeInfo slot -> SaveSystem class -> parent (Singleton<SaveSystem>) -> statics
    main_base = alloc(0x10000000 > mem.size() ? 0x100 : 0);  // placeholder, fixed below
    uint64_t ss_cls = klass("SaveSystem"), single = klass("Singleton`1"), statics = alloc(0x40);
    put<uint64_t>(ss_cls + 0x58, single); put<uint64_t>(single + 0xB8, statics);
    uint64_t ss = object(ss_cls, 0x80); put<uint64_t>(statics, ss);
    uint64_t gdm = object(klass("SaveSystemGameDataManager"), 0x100); put<uint64_t>(ss + 0x68, gdm);
    put<uint64_t>(gdm + 0x18, object(klass("Decoy"), 0x200));          // a field that is not the save
    uint64_t gs = object(klass("GameSave"), 0x200); put<uint64_t>(gdm + 0x30, gs);
    uint64_t dict = object(klass("Dictionary`2"), 0x60); put<uint64_t>(gs + 0x158, dict);
    uint64_t slot_cls = klass("LootBoxSlot");
    const int N = 3; uint64_t entries = object(klass("Entry[]"), 0x20 + 0x18 * 4); put<uint64_t>(entries + 0x18, 4);
    struct { const char* key; int id, grade, fgrade, count; int hash; } rows[N] = {
        {"1010001", 1010001, 2, 2, 2, 5}, {"removed", 0, 0, 0, 0, -1}, {"3020004", 3020004, 0, 0, 1, 9}};
    for (int i = 0; i < N; ++i) {
        uint64_t e = entries + 0x20 + 0x18 * i;
        put<int32_t>(e, rows[i].hash); put<int32_t>(e + 4, -1);
        if (rows[i].hash < 0) continue;
        uint64_t slot = object(slot_cls, 0x80);
        obscured(slot + 0x10, rows[i].id, 0x5A5A1234); put<int32_t>(slot + 0x24, 1);
        obscured(slot + 0x28, rows[i].grade, 777); obscured(slot + 0x3C, rows[i].fgrade, -3);
        obscured(slot + 0x50, rows[i].count, 0x7FFFFFFF);
        put<uint64_t>(e + 8, il2str(rows[i].key)); put<uint64_t>(e + 0x10, slot);
        // m_GetTimes: List<string> @0x78 (_items @0x10, _size @0x18), one time per catch
        const char* times[3][2] = {{"20261009120000", "20261009120500"}, {}, {"20261009120200", nullptr}};
        uint64_t list = object(klass("List`1"), 0x20), arr = object(klass("String[]"), 0x20 + 16);
        put<uint64_t>(list + 0x10, arr); put<int32_t>(list + 0x18, rows[i].count);
        for (int c = 0; c < rows[i].count; ++c) put<uint64_t>(arr + 0x20 + 8 * c, il2str(times[i][c]));
        put<uint64_t>(slot + 0x78, list);
    }
    put<uint64_t>(dict + 0x18, entries); put<int32_t>(dict + 0x20, N); put<int32_t>(dict + 0x2C, 7);
    // ingredient storage: SaveData+0x120 Dictionary<int, IngredientsSave> (id 0x10, level 0x24, count 0x4C)
    {
        uint64_t idict2 = object(klass("Dictionary`2"), 0x60), ient2 = object(klass("Entry[]"), 0x20 + 0x18 * 3);
        put<uint64_t>(ient2 + 0x18, 3);
        uint64_t ic = klass("IngredientsSave");
        int ids[3] = {1010001, 555, 3020004}, counts[3] = {3, 7, 0};
        for (int i = 0; i < 3; ++i) {
            uint64_t e = ient2 + 0x20 + 0x18 * i, v = object(ic, 0xA0);
            put<int32_t>(e, ids[i]); put<int32_t>(e + 8, ids[i]); put<uint64_t>(e + 0x10, v);
            obscured(v + 0x10, ids[i], 99); obscured(v + 0x24, 1, 5); obscured(v + 0x4C, counts[i], 1234);
        }
        put<uint64_t>(idict2 + 0x18, ient2); put<int32_t>(idict2 + 0x20, 3);
        put<uint64_t>(gs + 0x120, idict2);
    }
    // item table: DataManager (via the Singleton<DataManager>.get_Instance MethodInfo) -> field
    // object -> Dictionary<int, IntegratedItem>
    uint64_t dm_single = klass("Singleton`1"), dm_statics = alloc(0x40);
    put<uint64_t>(dm_single + 0xB8, dm_statics);
    uint64_t dm = object(klass("DataManager"), 0x600); put<uint64_t>(dm_statics, dm);
    uint64_t holder = object(klass("ItemContainer"), 0x100); put<uint64_t>(dm + 0x1A8, holder);
    uint64_t idict = object(klass("Dictionary`2"), 0x60); put<uint64_t>(holder + 0x20, idict);
    uint64_t ii_cls = klass("IntegratedItem");
    uint64_t ient = object(klass("Entry[]"), 0x20 + 0x18 * 3); put<uint64_t>(ient + 0x18, 3);
    struct { int id; const char* text; int rank; float w; } info[3] = {
        {999, "Item_Name_999", 1, 1.0f}, {1010001, "Item_Name_BlueTang", 1, 0.5f}, {3020004, "Item_Name_Wood", 0, 0.2f}};
    for (int i = 0; i < 3; ++i) {
        uint64_t e = ient + 0x20 + 0x18 * i, v = object(ii_cls, 0x80);
        put<int32_t>(e, info[i].id); put<int32_t>(e + 8, info[i].id); put<uint64_t>(e + 0x10, v);
        put<int32_t>(v + 0x10, info[i].id); put<uint64_t>(v + 0x18, il2str(info[i].text));
        put<uint64_t>(v + 0x38, il2str("icon_x")); put<int32_t>(v + 0x44, info[i].rank); put<float>(v + 0x48, info[i].w);
    }
    put<uint64_t>(idict + 0x18, ient); put<int32_t>(idict + 0x20, 3);
    // DataManager.TextDataDic: Dictionary<string, ItemNameTextData> (name @0x10, english @0x18)
    {
        const int T = 150;
        uint64_t tdict = object(klass("Dictionary`2"), 0x60), tent = object(klass("Entry[]"), 0x20 + 0x18 * T);
        put<uint64_t>(tent + 0x18, T);
        uint64_t tcls = klass("ItemNameTextData");
        for (int i = 0; i < T; ++i) {
            char key[32]; std::snprintf(key, sizeof key, i == 120 ? "Item_Name_BlueTang" : "Other_%d", i);
            uint64_t e = tent + 0x20 + 0x18 * i, v = object(tcls, 0x90);
            put<int32_t>(e, 1000 + i); put<uint64_t>(e + 8, il2str(key)); put<uint64_t>(e + 0x10, v);
            put<uint64_t>(v + 0x10, il2str(key)); put<uint64_t>(v + 0x18, il2str(i == 120 ? "Blue Tang" : "x"));
        }
        put<uint64_t>(tdict + 0x18, tent); put<int32_t>(tdict + 0x20, T);
        put<uint64_t>(dm + 0x240, tdict);
    }
    // DataManager.IngredientsDataDic (+0x1B8): Dictionary<int, IngredientsEntity> (NameID @0x18)
    // DataManager.RecipeDataDic (+0x2F0): Dictionary<int, RecipeEntity> (NameTextID @0x18, IngredientsList @0x68)
    auto intdict = [&](const char* vcls, std::vector<std::pair<int, uint64_t>> kv) {
        uint64_t d = object(klass("Dictionary`2"), 0x60), en = object(klass("Entry[]"), 0x20 + 0x18 * kv.size());
        put<uint64_t>(en + 0x18, kv.size());
        for (size_t i = 0; i < kv.size(); ++i) {
            uint64_t e = en + 0x20 + 0x18 * i;
            put<int32_t>(e, kv[i].first); put<int32_t>(e + 8, kv[i].first); put<uint64_t>(e + 0x10, kv[i].second);
        }
        put<uint64_t>(d + 0x18, en); put<int32_t>(d + 0x20, (int32_t)kv.size());
        (void)vcls; return d;
    };
    {
        uint64_t ie = object(klass("IngredientsEntity"), 0x60); put<int32_t>(ie + 0x10, 555); put<uint64_t>(ie + 0x18, il2str("Ing_Rice"));
        put<int32_t>(ie + 0x54, 1010001);   // ItemsTID: a catch of it is item 1010001 (2 in the cargo)
        put<uint64_t>(dm + 0x1B8, intdict("IngredientsEntity", {{555, ie}}));
        uint64_t re = object(klass("RecipeEntity"), 0x80); put<int32_t>(re + 0x10, 77); put<uint64_t>(re + 0x18, il2str("Recipe_Tuna")); put<int32_t>(re + 0x44, 25);
        uint64_t lst = object(klass("List`1"), 0x20), arr = object(klass("Ingredients[]"), 0x20 + 0x10 * 2);
        put<int32_t>(arr + 0x20, 555); put<int32_t>(arr + 0x24, 2); put<int32_t>(arr + 0x30, 9999); put<int32_t>(arr + 0x34, 1);
        put<uint64_t>(lst + 0x10, arr); put<int32_t>(lst + 0x18, 2); put<uint64_t>(re + 0x68, lst);
        g_test_rec_list = lst;
        put<uint64_t>(dm + 0x2F0, intdict("RecipeEntity", {{77, re}}));
        uint64_t ur = object(klass("UnlockRecipeSave"), 0x40); obscured(ur + 0x10, 77, 4242);
        put<uint64_t>(gs + 0x2A8, intdict("UnlockRecipeSave", {{77, ur}}));
        uint64_t ms = object(klass("SushibarMenuSlotSave"), 0xA0); obscured(ms + 0x10, 1, 3); obscured(ms + 0x24, 77, 31337);
        put<uint64_t>(gs + 0x298, intdict("SushibarMenuSlotSave", {{1, ms}}));
    }
    uint64_t mi = alloc(0x60); put<uint64_t>(mi + 0x20, dm_single);
    // main_base chosen so main_base + 0x9DB54B0 lands on a slot holding the class pointer;
    // the DataManager MethodInfo slot (main + 0x9E0E810) sits 0x53360 bytes after it
    uint64_t slot_addr = alloc(0x60000); put<uint64_t>(slot_addr, ss_cls);
    main_base = slot_addr - 0x9DB54B0;
    put<uint64_t>(main_base + 0x9E0E810, mi);
    // discard probe: LootBoxPanel.<>c TypeInfo slot -> nested class -> declaringType (+0x50) = LootBoxPanel
    {
        uint64_t lbp_cls = klass("LootBoxPanel"), nested = klass("<>c");
        put<uint64_t>(nested + 0x50, lbp_cls);
        put<uint64_t>(main_base + 0x9DC4798, nested);
        uint64_t scroll = object(klass("PauseLootBoxScrollPanel"), 0x120);
        put<int32_t>(scroll + 0x30, 2);
        object(lbp_cls, 0xD0);   // a bare LootBoxPanel that is not the live menu (no scroll)
        uint64_t sub_cls = klass("PauseLootBoxPanel"); put<uint64_t>(sub_cls + 0x58, lbp_cls);   // the menu is a subclass
        uint64_t panel = object(sub_cls, 0xD0);
        put<uint64_t>(panel + 0x60, scroll);
        // dumpConfirmText (+0x78): UIDataText -> _elements(+0x48) -> _items(+0x10) -> [0](+0x20) -> _ezText(+0x18) -> TMP(+0x30) -> m_text(+0xD8)
        uint64_t udt = object(klass("UIDataText"), 0x60), lst = object(klass("List`1"), 0x20), arr = object(klass("Element[]"), 0x28);
        uint64_t el = object(klass("Element"), 0x40), ez = object(klass("EzText"), 0x40), tmp = object(klass("TextMeshProUGUI"), 0xE0);
        put<uint64_t>(udt + 0x48, lst); put<uint64_t>(lst + 0x10, arr); put<uint64_t>(arr + 0x20, el);
        put<uint64_t>(el + 0x18, ez); put<uint64_t>(ez + 0x30, tmp); put<uint64_t>(tmp + 0xD8, il2str("Blue Tang is selected. Discard?")); g_tmp = tmp;
        put<uint64_t>(panel + 0x78, udt); put<uint8_t>(panel + 0xB0, 1); g_panel = panel;
    }

    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION; h.struct_size = sizeof h; h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.title_id = 0x010097F018538000; h.main_base = main_base;
    h.is_mapped = is_mapped; h.read_memory = read_memory; h.log = logf;
    h.publish_i64 = pub_i; h.publish_text = pub_t;
    h.get_heap_begin = [](void*) -> uint64_t { return BASE; };
    h.get_heap_end = [](void*) -> uint64_t { return BASE + mem.size(); };
    h.get_read_pointer = [](void*, uint64_t a, size_t n) -> const uint8_t* {
        if (a < BASE || a + n > BASE + mem.size()) return nullptr; return &mem[a - BASE]; };
    void* inst = m->create(&h, "{}");
    if (!inst) { std::puts("create failed"); return 1; }
    for (int i = 0; i < 20; ++i) m->sample(inst, &h);   // scans + incremental name lookup
    m->sample(inst, &h);
    for (int i = 0; i < 14; ++i) m->sample(inst, &h);   // next scan picks up the resolved names
    for (auto& [k, v] : out) if (k.find("cargo.on") == std::string::npos || v == "1") std::printf("%s = %s\n", k.c_str(), v.c_str());
    bool ok = out["cargo.ready"] == "1" && out["cargo.n"] == "2" && out["cargo.gs_off"] == "48" &&
              out["cargo.id0"] == "1010001" && out["cargo.count0"] == "2" && out["cargo.grade0"] == "2" &&
              out["cargo.key1"] == "3020004" && out["cargo.count1"] == "1" && out["cargo.on2"] == "0" &&
              out["row.n"] == "3" && out["row.sorted"] == "1" &&
              out["row.time0"] == "20261009120500" && out["row.name0"] == "Blue Tang" &&
              out["row.name1"] == "Item Name Wood" && out["row.rank1"] == "0" && out["row.wt1"] == "2" &&
              out["row.name2"] == "Blue Tang" && out["row.wt2"] == "5" && out["row.on3"] == "0" &&
              out["ing.n"] == "3" && out["ing.name0"] == "Ing Rice" && out["ing.count0"] == "7" &&
              out["ing.name1"] == "Blue Tang" && out["ing.count1"] == "3" && out["ing.name0"] != "" &&
              out["ing.count2"] == "0" && out["ing.empty2"] == "1" && out["ing.full2"] == "0" &&
              out["ing.full0"] == "1" && out["ing.empty0"] == "0" && out["ing.stock"] == "2" &&
              out["ing.nempty"] == "1" && out["ing.on3"] == "0" && out["ing.empty3"] == "0" &&
              out["rec.n"] == "1" && out["rec.name0"] == "Recipe Tuna" && out["rec.menu0"] == "1" && out["rec.price0"] == "25" &&
              out["rec.nn0_0"] == "Ing Rice" && out["rec.have0_0"] == "7" && out["rec.qty0_0"] == "2" &&
              out["rec.short0_1"] == "1" && out["rec.can0"] == "0" &&
              out["ui.found"] == "1" && out["ui.focused"] == "1" && out["ui.confirm"] == "Blue Tang is selected. Discard?";
    // bottom-screen choices: cargo sorting, the IN STOCK filter, the recipes of one ingredient
    {
        auto getx = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
        const EdenDsmodModuleExtensions* x = getx ? getx(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : nullptr;
        auto frame = [&]() { out.clear(); m->sample(inst, &h); };
        bool oku = x && x->on_action(inst, "cargo_sort", 1);
        frame();
        oku = oku && out["cargo.sort"] == "1" && out["row.name0"] == "Blue Tang" && out["row.name1"] == "Blue Tang" &&
              out["row.name2"] == "Item Name Wood";
        oku = oku && x->on_action(inst, "cargo_sort", 0); frame();
        oku = oku && out["row.name1"] == "Item Name Wood";
        oku = oku && !x->on_action(inst, "cargo_sort", 9);
        oku = oku && x->on_action(inst, "ing_filter", 1); frame();
        oku = oku && out["ing.n"] == "2" && out["ing.all"] == "3" && out["ing.on2"] == "0";
        oku = oku && x->on_action(inst, "ing_filter", 0); frame();
        oku = oku && out["ing.n"] == "3" && out["ing.on2"] == "1";
        oku = oku && x->on_action(inst, "ing_pick", 0); frame();
        oku = oku && out["ingsel.on"] == "1" && out["ingsel.name"] == "Ing Rice" && out["ingsel.n"] == "1" &&
              out["ingsel.rname0"] == "Recipe Tuna" && out["ingsel.count"] == "7";
        oku = oku && x->on_action(inst, "ing_pick", 2); frame();   // an empty stack no recipe uses
        oku = oku && out["ingsel.on"] == "1" && out["ingsel.n"] == "0" && out["ingsel.none"] == "1";
        oku = oku && x->on_action(inst, "ing_close", 0); frame();
        oku = oku && out["ingsel.on"] == "0" && !x->on_action(inst, "ing_pick", 99);
        oku = oku && out["ui.ic_dave"] == "module:icon/Mission_Dave_Diver";
        // a recipe panel, then PIN TO DIVE: storage 7 + 2 caught (Blue Tang is the rice's item) of 2 x 5
        oku = oku && x->on_action(inst, "rec_pick", 0); frame();
        oku = oku && out["recsel.on"] == "1" && out["recsel.name"] == "Recipe Tuna" && out["recsel.pinned"] == "0" &&
              out["recsel.have0"] == "7" && out["pin.on"] == "0";
        oku = oku && x->on_action(inst, "rec_pin", 0); frame();
        oku = oku && out["pin.on"] == "1" && out["recsel.pinned"] == "1" && out["rec.pinned0"] == "1" && out["pin.target"] == "5" &&
              out["pin.n"] == "1" && out["pin0.on"] == "1" && out["pin1.on"] == "0" && out["pin0.target"] == "5" &&
              out["pin0.dive0"] == "2" && out["pin0.have0"] == "9" && out["pin0.need0"] == "10" &&
              out["pin0.short0"] == "1" && out["pin0.hasdive0"] == "1";
        oku = oku && x->on_action(inst, "pin_dec", 0) && x->on_action(inst, "pin_dec", 0); frame();
        oku = oku && out["pin.target"] == "3" && out["pin0.need0"] == "6" && out["pin0.ok0"] == "1";
        oku = oku && x->on_action(inst, "rec_close", 0) && x->on_action(inst, "pin_inc", 0) && x->on_action(inst, "pin_inc", 0); frame();
        oku = oku && out["recsel.on"] == "0" && out["pin.on"] == "1";
        oku = oku && !x->on_action(inst, "rec_pin", 0);   // no panel open
        oku = oku && x->on_action(inst, "pin_clear", 0); frame();
        oku = oku && out["pin.on"] == "0" && !x->on_action(inst, "rec_pick", 50);
        std::printf("recipe pin: have=%s dive=%s need=%s\n", out["pin0.have0"].c_str(), out["pin0.dive0"].c_str(), out["pin0.need0"].c_str());
        std::printf("bottom-screen choices: ok=%d row0='%s' ing.n=%s ingsel.n=%s\n", oku, out["row.name0"].c_str(),
                    out["ing.n"].c_str(), out["ingsel.n"].c_str());
        ok = ok && oku;
    }
    // discard via the game's own menu: pick row 1 (Wood); the fake game moves a cursor on Down,
    // shows "<color=yellow>NAME</color> is selected. Discard?" on A, removes the catch on A again
    {
        auto getx = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
        const EdenDsmodModuleExtensions* x = getx ? getx(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : nullptr;
        bool okx = x && x->on_action && x->configure && (m->capabilities & EDEN_DSMOD_CAP_EXTENSIONS);
        const char* names[3] = {"Blue Tang", "Item Name Wood", "Blue Tang"};
        int cur = 0, presses_a = 0, presses_b = 0, presses_plus = 0, presses_down = 0;
        bool popup = false, removed = false;
        okx = okx && x->on_action(inst, "dc_pick", 1);
        out.clear(); m->sample(inst, &h);
        okx = okx && out["dc.picked"] == "1" && out["dc.pick_name"] == "Item Name Wood";
        std::printf("discard prompt: name='%s' isrc='%s' has_icon=%s\n", out["dc.pick_name"].c_str(), out["dc.pick_isrc"].c_str(), out["dc.has_icon"].c_str());
        okx = okx && x->on_action(inst, "dc_go", 0);
        for (int i = 0; i < 600 && okx; ++i) {
            out.clear(); m->sample(inst, &h);
            if (out["dc.p_down"] == "1") { ++presses_down; if (cur < 2) ++cur; }
            if (out["dc.p_plus"] == "1") ++presses_plus;
            if (out["dc.p_b"] == "1") { ++presses_b; popup = false; }
            if (out["dc.p_a"] == "1") {
                ++presses_a;
                if (!popup) {
                    popup = true;
                    std::string t = std::string("<color=yellow>") + names[cur] + "</color> is selected. Discard?";
                    put<uint64_t>(g_tmp + 0xD8, il2str(t.c_str()));
                } else if (!removed) {
                    popup = false; removed = true;
                    put<int32_t>(entries + 0x20 + 0x18 * 2, -1); put<int32_t>(dict + 0x2C, 20);
                }
            }
            if (removed && out["dc.step"] == "0") break;
        }
        bool ok_dc = okx && removed && out["dc.result"] == "1" && presses_a == 2 && presses_b == 1 &&
                     presses_plus == 0 && presses_down == 1 && cur == 1;
        std::printf("discard: ok=%d A=%d B=%d +=%d down=%d cur=%d status='%s' seen='%s'\n", ok_dc, presses_a, presses_b,
                    presses_plus, presses_down, cur, out["dc.status"].c_str(), out["dc.seen"].c_str());
        ok = ok && ok_dc;
        // second run: menu closed (not focused) -> + opens it; the game's cursor was left on row 1,
        // (the game's list differs from ours) so the first popup names the wrong catch -> B, Down, A
        const char* names2[3] = {"Item Name Wood", "Blue Tang", "Blue Tang"};
        put<uint8_t>(g_panel + 0xB0, 0);
        cur = 0; presses_a = presses_b = presses_plus = presses_down = 0; popup = false; removed = false;
        for (int i = 0; i < 30; ++i) { out.clear(); m->sample(inst, &h); }   // let the scan settle
        bool ok2 = out["row.n"] == "2" && x->on_action(inst, "dc_pick", 0) && x->on_action(inst, "dc_go", 0);
        int start_cur = 0;
        for (int i = 0; i < 900 && ok2; ++i) {
            out.clear(); m->sample(inst, &h);
            if (out["dc.p_plus"] == "1") { ++presses_plus; put<uint8_t>(g_panel + 0xB0, 1); cur = start_cur; }
            if (out["dc.p_down"] == "1") { ++presses_down; if (cur < 1) ++cur; }
            if (out["dc.p_b"] == "1") { ++presses_b; popup = false; }
            if (out["dc.p_a"] == "1") {
                ++presses_a;
                const char* nm = cur == 0 ? "Item Name Wood" : names2[cur];
                if (!popup) {
                    popup = true;
                    std::string t = std::string("<color=yellow>") + nm + "</color> is selected. Discard?";
                    put<uint64_t>(g_tmp + 0xD8, il2str(t.c_str()));
                } else if (!removed) {
                    popup = false; removed = true;
                    put<int32_t>(entries + 0x20, -1); put<int32_t>(dict + 0x2C, 21);
                }
            }
            if (removed && out["dc.step"] == "0") break;
        }
        bool ok_dc2 = ok2 && removed && out["dc.result"] == "1" && presses_plus == 1 && presses_a == 3 &&
                      presses_b == 2 && presses_down == 1;
        std::printf("discard2: ok=%d A=%d B=%d +=%d down=%d status='%s'\n", ok_dc2, presses_a, presses_b,
                    presses_plus, presses_down, out["dc.status"].c_str());
        ok = ok && ok_dc2;
        // third run: the menu is closed but the list still says "focused" (seen on device), and the
        // popup names the same catch as last time, so its text object does not change. O2 drains
        // while the game runs and stands still while the menu is open.
        uint64_t ig_ti = klass("InGameManager"), ig_single = klass("Singleton`1"), ig_st = alloc(0x40);
        put<uint64_t>(main_base + 0x9DB0400, ig_ti); put<uint64_t>(ig_ti + 0x58, ig_single);
        put<uint64_t>(ig_single + 0xB8, ig_st);
        uint64_t igm = object(klass("InGameManager"), 0x100), chr = object(klass("PlayerCharacter"), 0x300);
        uint64_t bh = object(klass("PlayerBreathHandler"), 0x100);
        put<uint64_t>(ig_st, igm); put<uint64_t>(igm + 0xA0, chr); put<uint64_t>(chr + 0x298, bh);
        float o2 = 80.0f; put<float>(bh + 0xA0, o2);
        put<int32_t>(entries + 0x20, 5); put<int32_t>(dict + 0x2C, 22);   // the Blue Tangs are back
        put<uint8_t>(g_panel + 0xB0, 1);                                  // stale focus
        bool menu = false; popup = false; removed = false;
        presses_a = presses_b = presses_plus = presses_down = 0; int a_in_dive = 0;
        for (int i = 0; i < 60; ++i) { out.clear(); m->sample(inst, &h); o2 -= 0.01f; put<float>(bh + 0xA0, o2); }
        bool ok3 = x->on_action(inst, "dc_pick", 0) && x->on_action(inst, "dc_go", 0);
        for (int i = 0; i < 900 && ok3; ++i) {
            out.clear(); m->sample(inst, &h);
            if (!menu) { o2 -= 0.01f; put<float>(bh + 0xA0, o2); }
            if (out["dc.p_plus"] == "1") { ++presses_plus; menu = !menu; }
            if (out["dc.p_down"] == "1") ++presses_down;
            if (out["dc.p_b"] == "1") { ++presses_b; if (popup) popup = false; else menu = false; }
            if (out["dc.p_a"] == "1") {
                ++presses_a;
                if (!menu) ++a_in_dive;
                else if (!popup) popup = true;   // same name as before: the text object stays
                else if (!removed) { popup = false; removed = true; put<int32_t>(entries + 0x20, -1); put<int32_t>(dict + 0x2C, 23); }
            }
            if (removed && out["dc.step"] == "0") break;
        }
        bool ok_dc3 = ok3 && removed && out["dc.result"] == "1" && presses_plus == 1 && presses_a == 2 &&
                      a_in_dive == 0 && presses_b == 1 && !menu;
        std::printf("discard3: ok=%d A=%d (in dive %d) B=%d +=%d status='%s'\n", ok_dc3, presses_a, a_in_dive,
                    presses_b, presses_plus, out["dc.status"].c_str());
        ok = ok && ok_dc3;
        // fourth run: the player opened the menu themselves (O2 frozen) -> no + press
        put<int32_t>(entries + 0x20, 5); put<int32_t>(dict + 0x2C, 24);
        menu = true; popup = false; removed = false; presses_a = presses_b = presses_plus = presses_down = 0; a_in_dive = 0;
        for (int i = 0; i < 400; ++i) { out.clear(); m->sample(inst, &h); }   // O2 not changing
        bool ok4 = x->on_action(inst, "dc_pick", 0) && x->on_action(inst, "dc_go", 0);
        for (int i = 0; i < 900 && ok4; ++i) {
            out.clear(); m->sample(inst, &h);
            if (!menu) { o2 -= 0.01f; put<float>(bh + 0xA0, o2); }
            if (out["dc.p_plus"] == "1") { ++presses_plus; menu = !menu; }
            if (out["dc.p_b"] == "1") { ++presses_b; if (popup) popup = false; else menu = false; }
            if (out["dc.p_a"] == "1") {
                ++presses_a;
                if (!menu) ++a_in_dive;
                else if (!popup) popup = true;
                else if (!removed) { popup = false; removed = true; put<int32_t>(entries + 0x20, -1); put<int32_t>(dict + 0x2C, 25); }
            }
            if (removed && out["dc.step"] == "0") break;
        }
        bool ok_dc4 = ok4 && removed && out["dc.result"] == "1" && presses_plus == 0 && presses_a == 2 && a_in_dive == 0;
        std::printf("discard4: ok=%d A=%d +=%d B=%d status='%s'\n", ok_dc4, presses_a, presses_plus, presses_b, out["dc.status"].c_str());
        ok = ok && ok_dc4;
        // fifth: the host re-creates the module in the middle of a job -> the job stops (no hang)
        put<int32_t>(entries + 0x20, 5); put<int32_t>(dict + 0x2C, 26);
        for (int i = 0; i < 30; ++i) { out.clear(); m->sample(inst, &h); o2 -= 0.01f; put<float>(bh + 0xA0, o2); }
        bool ok5 = x->on_action(inst, "dc_pick", 0) && x->on_action(inst, "dc_go", 0);
        for (int i = 0; i < 3; ++i) { out.clear(); m->sample(inst, &h); }
        inst = m->create(&h, "{}");
        for (int i = 0; i < 40; ++i) { out.clear(); m->sample(inst, &h); }   // every frame must carry the sushi lists
        ok5 = ok5 && out["dc.step"] == "0" && out["dc.status"].find("restarted") != std::string::npos && out["ui.found"] == "1";
        std::printf("discard5: ok=%d status='%s' ui.found=%s\n", ok5, out["dc.status"].c_str(), out["ui.found"].c_str());
        ok = ok && ok5;
        // sixth: discard without the menu. The package's load plan makes the game look at Eden's
        // mailbox every frame; here a stand-in does what the patch does (heartbeat, then
        // LootBox.Remove_Impl with the posted arguments). No button may be pressed.
        {
            EdenDsmodHostExtensions hx{};
            hx.version = EDEN_DSMOD_EXT_VERSION; hx.struct_size = sizeof hx; hx.abi_hash = EDEN_DSMOD_EXT_HASH;
            hx.mailbox_address = 0x7000000; hx.mailbox_size = sizeof mb;
            hx.load_u32 = [](void*, uint32_t o, uint32_t* v) -> EdenDsmodBool { if (o + 4 > sizeof mb) return EDEN_DSMOD_FALSE; std::memcpy(v, mb + o, 4); return EDEN_DSMOD_TRUE; };
            hx.load_u64 = [](void*, uint32_t o, uint64_t* v) -> EdenDsmodBool { if (o + 8 > sizeof mb) return EDEN_DSMOD_FALSE; std::memcpy(v, mb + o, 8); return EDEN_DSMOD_TRUE; };
            hx.store_u32 = [](void*, uint32_t o, uint32_t v) -> EdenDsmodBool { if (o + 4 > sizeof mb) return EDEN_DSMOD_FALSE; std::memcpy(mb + o, &v, 4); return EDEN_DSMOD_TRUE; };
            hx.store_u64 = [](void*, uint32_t o, uint64_t v) -> EdenDsmodBool { if (o + 8 > sizeof mb) return EDEN_DSMOD_FALSE; std::memcpy(mb + o, &v, 8); return EDEN_DSMOD_TRUE; };
            x->configure(inst, &hx);
            // LootBox singleton: TypeInfo (main+9DB1EA8) -> parent +0x58 -> statics +0xB8 -> instance
            uint64_t lbc = klass("LootBox"), lbp = klass("SingletonNoMono`1"), lbs = alloc(0x10), lb = object(lbc, 0x70);
            put<uint64_t>(main_base + 0x9DB1EA8, lbc); put<uint64_t>(lbc + 0x58, lbp); put<uint64_t>(lbp + 0xB8, lbs); put<uint64_t>(lbs, lb);
            // the Blue Tangs, with catch times as the game writes them, picked up the "lift" way 3
            put<int32_t>(entries + 0x20, 5); put<int32_t>(dict + 0x2C, 27);
            uint64_t slot0 = 0; std::memcpy(&slot0, &mem[entries + 0x20 + 0x10 - BASE], 8);
            put<int32_t>(slot0 + 0x70, 3);
            uint64_t tl = 0; std::memcpy(&tl, &mem[slot0 + 0x78 - BASE], 8);
            uint64_t ta = 0; std::memcpy(&ta, &mem[tl + 0x10 - BASE], 8);
            put<uint64_t>(ta + 0x20, il2str("10/09/2026 12:00:00")); put<uint64_t>(ta + 0x28, il2str("10/09/2026 12:05:00"));
            uint32_t hb = 0, runs = 0; int presses = 0; uint32_t got_id = 0, got_grade = 0, got_lift = 0, got_count = 0; uint64_t got_lb = 0, got_ticks = 0;
            auto game_frame = [&]() {   // what the patch does each frame
                ++hb; std::memcpy(mb + 0x30, &hb, 4);
                uint32_t magic, req, done; std::memcpy(&magic, mb, 4); std::memcpy(&req, mb + 4, 4); std::memcpy(&done, mb + 8, 4);
                if (magic != 0x51445444 || req == done) return;
                std::memcpy(&got_lb, mb + 0x10, 8); std::memcpy(&got_id, mb + 0x18, 4); std::memcpy(&got_grade, mb + 0x1C, 4);
                std::memcpy(&got_lift, mb + 0x20, 4); std::memcpy(&got_count, mb + 0x24, 4); std::memcpy(&got_ticks, mb + 0x28, 8);
                put<int32_t>(entries + 0x20, -1); put<int32_t>(dict + 0x2C, 28);   // the game removed it
                ++runs; std::memcpy(mb + 0xC, &runs, 4); std::memcpy(mb + 8, &req, 4);
            };
            for (int i = 0; i < 40; ++i) { out.clear(); m->sample(inst, &h); game_frame(); }
            bool ok6 = out["dc.direct"] == "1" && out["row.n"] == "2";
            std::string t0 = out["row.time0"];
            ok6 = ok6 && x->on_action(inst, "dc_pick", 0) && x->on_action(inst, "dc_go", 0);
            for (int i = 0; i < 200 && ok6; ++i) {
                out.clear(); m->sample(inst, &h); game_frame();
                if (out["dc.p_a"] == "1" || out["dc.p_b"] == "1" || out["dc.p_plus"] == "1" || out["dc.p_down"] == "1") ++presses;
                if (out["dc.result"] == "1" && out["dc.step"] == "0") break;
            }
            // DateTime ticks of the newest catch (row 0), worked out independently: days since 0001-01-01
            // row 0 is the newest catch, 12:05:00 (the module publishes its time as it shows it)
            const bool newest_first = t0.find("12:05") != std::string::npos || t0 == "20261009120500";
            const uint64_t hh = 12, mi = 5;
            const uint64_t day_ticks = 864000000000ull, epoch1970 = 621355968000000000ull;
            const uint64_t days_since_1970 = 20735;   // 2026-10-09 (python: date(2026,10,9) - date(1970,1,1))
            uint64_t want = epoch1970 + days_since_1970 * day_ticks + (uint64_t(hh) * 3600 + uint64_t(mi) * 60) * 10000000ull;
            ok6 = ok6 && out["dc.result"] == "1" && presses == 0 && runs == 1 && got_lb == lb && got_id == 1010001 &&
                  got_grade == 2 && got_lift == 3 && got_count == 1 && got_ticks == want && newest_first;
            std::printf("discard6 (no menu): row0 time %s\n", t0.c_str());
            std::printf("discard6 (no menu): ok=%d presses=%d runs=%u id=%u grade=%u lift=%u ticks=%llu want=%llu status='%s' mode='%s'\n",
                        ok6, presses, runs, got_id, got_grade, got_lift, (unsigned long long)got_ticks, (unsigned long long)want,
                        out["dc.status"].c_str(), out["dc.mode"].c_str());
            ok = ok && ok6;
            put<int32_t>(entries + 0x20, 5); put<int32_t>(dict + 0x2C, 29);
        }
        // where the player is: the sushi bar's manager singleton is alive -> game.mode 2 (the
        // bottom screen switches to SUSHI); destroyed (native pointer gone) -> 0
        {
            uint64_t sbc = klass("SushiBarManager"), sbp = klass("Singleton`1"), sbs = alloc(0x10), sb = object(sbc, 0x190);
            put<uint64_t>(main_base + 0x9DB6C78, sbc); put<uint64_t>(sbc + 0x58, sbp); put<uint64_t>(sbp + 0xB8, sbs); put<uint64_t>(sbs, sb);
            put<uint64_t>(sb + 0x10, 0x1234);   // m_CachedPtr: alive
            for (int i = 0; i < 100; ++i) { out.clear(); m->sample(inst, &h); }   // no heartbeat: the dive is over
            std::string m_in = out["game.mode"];
            put<uint64_t>(sb + 0x10, 0);        // the scene unloaded
            for (int i = 0; i < 20; ++i) { out.clear(); m->sample(inst, &h); }
            bool okm2 = m_in == "2" && out["game.mode"] == "0";
            std::printf("mode: ok=%d at the sushi bar=%s after=%s\n", okm2, m_in.c_str(), out["game.mode"].c_str());
            ok = ok && okm2;
            // tonight's menu from the bottom screen: the patch in SushiBarManager.Update calls
            // SushiBarMenuManager.AddMenu with what the module posts; a stand-in plays the game here
            auto getx3 = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
            const EdenDsmodModuleExtensions* x3 = getx3 ? getx3(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : nullptr;
            put<uint64_t>(sb + 0x10, 0x1234);
            put<int32_t>(g_test_rec_list + 0x18, 1);   // only the rice: 7 in storage, 2 a plate -> 3 plates
            uint64_t mmc = klass("SushiBarMenuManager"), gen = klass("SingletonNoMono`1"), mst = alloc(0x10), mgr = object(mmc, 0x40);
            uint64_t minfo = alloc(0x40); put<uint64_t>(minfo + 0x20, gen); put<uint64_t>(gen + 0xB8, mst); put<uint64_t>(mst, mgr);
            put<uint64_t>(main_base + 0x9E0E530, minfo);
            uint64_t sl[3]; const int32_t sid[3] = {1001, 1000, 2000}, splace[3] = {0, 0, 1}; const uint8_t sun[3] = {0, 1, 1};
            uint64_t sd = object(klass("Dictionary`2"), 0x60), se = object(klass("Entry[]"), 0x20 + 0x18 * 3);
            for (int k = 0; k < 3; ++k) {
                sl[k] = object(klass("SushibarMenuSlot"), 0x38);
                put<int32_t>(sl[k] + 0x10, sid[k]); put<int32_t>(sl[k] + 0x30, splace[k]); mem[sl[k] + 0x28 - BASE] = sun[k];
                uint64_t e = se + 0x20 + 0x18 * k; put<int32_t>(e, sid[k]); put<int32_t>(e + 8, sid[k]); put<uint64_t>(e + 0x10, sl[k]);
            }
            put<uint64_t>(sd + 0x18, se); put<int32_t>(sd + 0x20, 3); put<uint64_t>(mgr + 0x10, sd);
            uint32_t hb2 = 0, runs2 = 0, got_slot = 0, got_rec = 0, got_amt = 0, got_kind = 0; uint64_t got_mgr = 0;
            auto sushi_frame = [&]() {   // what the patch does each frame at the sushi bar
                ++hb2; std::memcpy(mb + 0x40, &hb2, 4);
                uint32_t magic, req, done; std::memcpy(&magic, mb, 4); std::memcpy(&req, mb + 0x44, 4); std::memcpy(&done, mb + 0x48, 4);
                if (magic != 0x51445444 || req == done) return;
                std::memcpy(&got_mgr, mb + 0x50, 8); std::memcpy(&got_slot, mb + 0x58, 4); std::memcpy(&got_rec, mb + 0x5C, 4);
                std::memcpy(&got_amt, mb + 0x64, 4);
                uint32_t kind = 0; std::memcpy(&kind, mb + 0x6C, 4); got_kind = kind;
                uint32_t one = 1;
                if (kind != 0) {                                    // RemoveMenu(slot)
                    for (int k = 0; k < 3; ++k)
                        if (sid[k] == (int32_t)got_slot) { put<int32_t>(sl[k] + 0x14, 0); put<int32_t>(sl[k] + 0x18, 0); }
                    std::memcpy(mb + 0x70, &one, 4);
                }
                if (kind != 1) {                                    // AddMenu(slot, recipe, plates)
                    for (int k = 0; k < 3; ++k)
                        if (sid[k] == (int32_t)got_slot) { put<int32_t>(sl[k] + 0x14, (int32_t)got_rec); put<int32_t>(sl[k] + 0x18, (int32_t)got_amt); }
                    std::memcpy(mb + 0x68, &one, 4);
                }
                ++runs2; std::memcpy(mb + 0x4C, &runs2, 4); std::memcpy(mb + 0x48, &req, 4);
            };
            for (int i = 0; i < 80; ++i) { out.clear(); m->sample(inst, &h); sushi_frame(); }
            bool okmenu = x3 && out["menu.live"] == "1" && x3->on_action(inst, "rec_pick", 0);
            out.clear(); m->sample(inst, &h); sushi_frame();
            okmenu = okmenu && out["menu.amt"] == "3" && out["menu.ready"] == "1" && x3->on_action(inst, "menu_inc", 0) &&
                     x3->on_action(inst, "menu_dec", 0);
            out.clear(); m->sample(inst, &h); sushi_frame();
            okmenu = okmenu && out["menu.amt"] == "2" && x3->on_action(inst, "menu_add", 0);
            for (int i = 0; i < 40; ++i) { out.clear(); m->sample(inst, &h); sushi_frame(); if (out["menu.busy"] == "0" && out["menu.has_status"] == "1") break; }
            okmenu = okmenu && runs2 == 1 && got_mgr == mgr && got_slot == 1000 && got_rec == 77 && got_amt == 2 &&
                     out["menu.status"].find("Added 2 x") != std::string::npos &&
                     out["menu.splash"] == "1" && out["menu.splash_name"] == "Recipe Tuna";      // the pink banner
            std::printf("menu: ok=%d slot=%u recipe=%u plates=%u status='%s' mode='%s'\n", okmenu, got_slot, got_rec, got_amt,
                        out["menu.status"].c_str(), out["menu.mode"].c_str());
            // REMOVE (asks twice) takes it off; SWAP puts it in place of another dish
            for (int i = 0; i < 20; ++i) { out.clear(); m->sample(inst, &h); sushi_frame(); }
            bool okrs = out["menu.can_rm"] == "1" && out["menu.rm_plain"] == "1" && x3->on_action(inst, "menu_remove", 0);
            out.clear(); m->sample(inst, &h); sushi_frame();
            okrs = okrs && out["menu.rm_armed"] == "1" && runs2 == 1 && x3->on_action(inst, "menu_remove", 0);
            for (int i = 0; i < 40; ++i) { out.clear(); m->sample(inst, &h); sushi_frame(); if (out["menu.busy"] == "0" && out["menu.has_status"] == "1") break; }
            int32_t rid0 = 0; std::memcpy(&rid0, &mem[sl[1] + 0x14 - BASE], 4);
            okrs = okrs && runs2 == 2 && got_kind == 1 && got_slot == 1000 && rid0 == 0 && out["menu.status"].find("Removed") != std::string::npos;
            std::printf("remove: ok=%d status='%s'\n", okrs, out["menu.status"].c_str());
            put<int32_t>(sl[0] + 0x14, 55); put<int32_t>(sl[0] + 0x18, 4);           // another dish in slot 1001
            for (int i = 0; i < 20; ++i) { out.clear(); m->sample(inst, &h); sushi_frame(); }
            okrs = okrs && out["menu.can_swap"] == "1" && out["menu.sn"] == "1" && out["menu.splates0"] == "4" &&
                   x3->on_action(inst, "menu_swap_open", 0);
            out.clear(); m->sample(inst, &h); sushi_frame();
            okrs = okrs && out["menu.swap_open"] == "1" && x3->on_action(inst, "menu_swap", 0);
            for (int i = 0; i < 40; ++i) { out.clear(); m->sample(inst, &h); sushi_frame(); if (out["menu.busy"] == "0" && out["menu.has_status"] == "1") break; }
            int32_t rid1 = 0; std::memcpy(&rid1, &mem[sl[0] + 0x14 - BASE], 4);
            okrs = okrs && runs2 == 3 && got_kind == 2 && got_slot == 1001 && rid1 == 77 && out["menu.status"].find("Swapped") != std::string::npos;
            std::printf("swap: ok=%d sname0='%s' status='%s'\n", okrs, out["menu.sname0"].c_str(), out["menu.status"].c_str());
            okmenu = okmenu && okrs;
            // away from the sushi bar the request is refused before reaching the game
            put<uint64_t>(sb + 0x10, 0);
            for (int i = 0; i < 40; ++i) { out.clear(); m->sample(inst, &h); }
            okmenu = okmenu && out["menu.live"] == "0" && x3->on_action(inst, "menu_add", 0);
            for (int i = 0; i < 5; ++i) { out.clear(); m->sample(inst, &h); }
            okmenu = okmenu && runs2 == 3 && out["menu.status"].find("sushi bar") != std::string::npos;
            ok = ok && okmenu;
            put<int32_t>(g_test_rec_list + 0x18, 2); x3->on_action(inst, "rec_close", 0);
        }
        // pictures straight from the game files: pictures.idx + a UnityFS bundle in "romfs"
        {
            make_bundle();
            EdenDsmodHostExtensions hx{};
            hx.version = EDEN_DSMOD_EXT_VERSION; hx.struct_size = sizeof hx; hx.abi_hash = EDEN_DSMOD_EXT_HASH;
            // stand-in ASTC decoder: every pixel becomes its texel position (from the block's bx, by)
            hx.decode_astc = [](void*, uint32_t w, uint32_t hh, uint32_t bw, uint32_t bh, const uint8_t* in, size_t n, uint8_t* o, size_t on) -> EdenDsmodBool {
                if (bw != 4 || bh != 4 || n != (w / 4) * (hh / 4) * 16 || on != size_t{w} * hh * 4) return EDEN_DSMOD_FALSE;
                for (uint32_t y = 0; y < hh; ++y) for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t* b = in + ((y / 4) * (w / 4) + x / 4) * 16;
                    if (b[2] != 0x5A) return EDEN_DSMOD_FALSE;
                    uint8_t* p = o + (size_t{y} * w + x) * 4; p[0] = b[0] * 4 + x % 4; p[1] = b[1] * 4 + y % 4; p[2] = 0; p[3] = 255;
                }
                return EDEN_DSMOD_TRUE;
            };
            x->configure(inst, &hx);
            EdenDsmodHostApi hw = h;
            hw.read_romfs = [](void*, const char* path, uint64_t off, void* dst, size_t n) -> size_t {
                const uint8_t* src = nullptr; size_t len = 0;
                if (std::strcmp(path, "file:pictures.idx") == 0) { src = reinterpret_cast<const uint8_t*>(g_pidx.data()); len = g_pidx.size(); }
                // only the base game has it (as when the running game's romfs does not answer)
                else if (std::strcmp(path, "base:Data/test.bundle") == 0) { src = g_bundle.data(); len = g_bundle.size(); }
                if (src && !dst) return len;                     // size query
                if (!src || off >= len) return 0;
                size_t m = std::min(n, static_cast<size_t>(len - off)); std::memcpy(dst, src + off, m); return m;
            };
            struct Pic { uint32_t w = 0, h = 0; std::vector<uint8_t> px; } a, b;
            auto grab = [](void* r, uint32_t w, uint32_t hh, const uint8_t* px, size_t n) { auto& g = *static_cast<Pic*>(r); g.w = w; g.h = hh; g.px.assign(px, px + n); };
            bool la = x->load_image(inst, &hw, "module:icon/pic_a", &a, grab);
            bool lb = x->load_image(inst, &hw, "module:icon/Pic_B", &b, grab);   // "_Thumbnail" added, rotated 90
            // pictures come centred on a transparent square: A 20x12 -> 20x20 (4 rows above),
            // B 9x7 -> 9x9 (1 row above)
            Pic fa;   // a frame keeps its own shape (no square)
            bool lf = x->load_image(inst, &hw, "module:frame/pic_a", &fa, grab) && fa.w == 20 && fa.h == 12 &&
                      fa.px[3] == 255 && fa.px[0] == 10 && fa.px[1] == 6 + 11;
            bool okl = la && lb && lf && a.w == 20 && a.h == 20 && b.w == 9 && b.h == 9;
            const uint32_t ah = 12, aoy = 4, bh = 7, boy = 1;
            for (uint32_t v = 0; okl && v < a.h; ++v) for (uint32_t u = 0; u < a.w; ++u) {
                const uint8_t* q = &a.px[(v * a.w + u) * 4];
                if (v < aoy || v >= aoy + ah) { okl = q[3] == 0; continue; }
                const uint32_t sv = v - aoy;   // A: (u, sv) = texel (10 + u, 6 + (11 - sv))
                okl = q[0] == 10 + u && q[1] == 6 + (11 - sv);
            }
            // B (rotation 4 = UnityPy ROTATE_270 then flip): see module
            for (uint32_t v = 0; okl && v < b.h; ++v) for (uint32_t u = 0; u < b.w; ++u) {
                const uint8_t* q = &b.px[(v * b.w + u) * 4];
                if (v < boy || v >= boy + bh) { okl = q[3] == 0; continue; }
                const uint32_t sv = v - boy, bv = bh - 1 - sv, ax = bv, ay = 9 - 1 - u;
                okl = q[0] == 33 + ax && q[1] == 40 + ay;
            }
            h.read_romfs = hw.read_romfs; for (int i = 0; i < 70; ++i) m->sample(inst, &h);
            std::printf("game-file pictures: ok=%d A %ux%u B %ux%u la %d lb %d (%s)\n", okl, a.w, a.h, b.w, b.h, la, lb, out["icon.diag"].c_str());
            ok = ok && okl;
            // the game font: metrics through the data + font extensions, the atlas through load_image
            {
                auto gd = (EdenDsmodGetDataExtensionsFn)dlsym(so, "eden_dsmod_get_data_extensions");
                auto gf = (EdenDsmodGetFontExtensionsFn)dlsym(so, "eden_dsmod_get_font_extensions");
                const EdenDsmodModuleDataExtensions* dx = gd ? gd(EDEN_DSMOD_DATA_EXT_VERSION, EDEN_DSMOD_DATA_EXT_HASH) : nullptr;
                const EdenDsmodFontExtensions* fx = gf ? gf(EDEN_DSMOD_FONT_EXT_VERSION, EDEN_DSMOD_FONT_EXT_HASH) : nullptr;
                std::vector<uint8_t> blob;
                bool okf = dx && fx && dx->load_data(inst, &hw, "module:fontdata", &blob,
                                                     [](void* r, const uint8_t* b, size_t n) { static_cast<std::vector<uint8_t>*>(r)->assign(b, b + n); });
                struct FM { uint32_t line = 0, first = 0; std::vector<EdenDsmodFontGlyph> g; } fm;
                okf = okf && fx->decode_font(inst, blob.data(), blob.size(), &fm,
                                             [](void* r, uint32_t lh, uint32_t first, const EdenDsmodFontGlyph* g, uint32_t n) {
                                                 auto& f = *static_cast<FM*>(r); f.line = lh; f.first = first; f.g.assign(g, g + n); });
                // capitals 20 px in the source (< 34): kept 1:1; 2 px margin around each glyph
                const EdenDsmodFontGlyph* H = fm.g.size() > 0x48 ? &fm.g['H' - 0x20] : nullptr;
                const EdenDsmodFontGlyph* A = fm.g.size() > 0x61 ? &fm.g['a' - 0x20] : nullptr;
                okf = okf && fm.line == 20 && fm.first == 0x20 && fm.g.size() == 224 && H && A &&
                      H->w == 14 && H->h == 24 && H->bearing_x == -1 && H->bearing_y == 22 && H->advance == 12 &&
                      A->w == 10 && A->h == 11 && A->advance == 8 && fm.g[0].advance == 5 && fm.g[0].w == 0 &&
                      fm.g['Q' - 0x20].w == 0 && fm.g['Q' - 0x20].advance == 5;    // missing: a blank as wide as a space
                Pic at;
                okf = okf && x->load_image(inst, &hw, "module:font", &at, grab) && at.w == 1024;
                // atlas pixel (u, v) of H = stored texel (4 - 2 + u, 8 + 20 - 1 + 2 - v), white with that coverage
                for (uint32_t v = 0; okf && H && v < H->h; ++v) for (uint32_t u = 0; u < H->w; ++u) {
                    const int col = 2 + static_cast<int>(u), row = 29 - static_cast<int>(v);
                    const uint8_t* q = &at.px[((H->y + v) * at.w + H->x + u) * 4];
                    okf = q[0] == 255 && q[3] == font_px(col, row);
                    if (!okf) std::printf("  font atlas H (%u,%u): %u want %u\n", u, v, q[3], font_px(col, row));
                }
                h.read_romfs = hw.read_romfs; for (int i = 0; i < 70; ++i) { out.clear(); m->sample(inst, &h); }
                okf = okf && out["icon.diag"].find("font game (20 px") != std::string::npos;
                std::printf("game font: ok=%d line %u glyphs %zu H %ux%u at %u,%u atlas %ux%u (%s)\n", okf, fm.line, fm.g.size(),
                            H ? H->w : 0, H ? H->h : 0, H ? H->x : 0, H ? H->y : 0, at.w, at.h, out["icon.diag"].c_str());
                ok = ok && okf;
            }
            // the DIVE page's look (0.27.1): the HUD font where it has the character, the text font
            // elsewhere; changing look (nav) moves __font_epoch so the host decodes again
            {
                auto gd = (EdenDsmodGetDataExtensionsFn)dlsym(so, "eden_dsmod_get_data_extensions");
                const EdenDsmodModuleDataExtensions* dx = gd ? gd(EDEN_DSMOD_DATA_EXT_VERSION, EDEN_DSMOD_DATA_EXT_HASH) : nullptr;
                bool okh = dx && x->on_action(inst, "nav", 0) && !x->on_action(inst, "nav", 15);   // 0.37.109: 15 pages (+ CALL)
                out.clear(); m->sample(inst, &h);
                const long e0 = std::atol(out["__font_epoch"].c_str());
                std::vector<uint8_t> blob;
                okh = okh && dx->load_data(inst, &hw, "module:fontdata", &blob,
                                           [](void* r, const uint8_t* b, size_t n) { static_cast<std::vector<uint8_t>*>(r)->assign(b, b + n); }) &&
                      blob.size() > 16 && blob[3] == 'F';
                for (int i = 0; i < 61; ++i) { out.clear(); m->sample(inst, &h); }
                okh = okh && out["ui.page"] == "0" && out["icon.diag"].find("DIVE font game HUD (1 of 224") != std::string::npos;
                const std::string d0 = out["icon.diag"];
                x->on_action(inst, "nav", 3); out.clear(); m->sample(inst, &h);
                okh = okh && out["ui.page"] == "3" && std::atol(out["__font_epoch"].c_str()) == e0;
                x->on_action(inst, "nav", 5); out.clear(); m->sample(inst, &h);
                okh = okh && std::atol(out["__font_epoch"].c_str()) == e0;          // one look everywhere
                x->on_action(inst, "nav", 0); out.clear(); m->sample(inst, &h);
                okh = okh && out["ui.page"] == "0" && std::atol(out["__font_epoch"].c_str()) == e0;
                std::printf("dive font: ok=%d page %s epoch %ld -> %s (%s)\n", okh, out["ui.page"].c_str(), e0, out["__font_epoch"].c_str(), d0.c_str());
                // 0.37.62: the calculator (the module's own arithmetic): 12 + 7 x 3 = 57, then 8 / 0 = Error
                {
                    auto keys = [&](const char* k) { for (; *k; ++k) {
                        const int64_t a = *k >= '0' && *k <= '9' ? *k - '0' : *k == '.' ? 10 : *k == '+' ? 11 : *k == '-' ? 12 :
                                          *k == '*' ? 13 : *k == '/' ? 14 : *k == '=' ? 15 : *k == '%' ? 16 : *k == 'n' ? 17 : *k == 'd' ? 18 : 19;
                        x->on_action(inst, "calc_key", a); } };
                    keys("c12+7*"); out.clear(); m->sample(inst, &h);
                    const std::string mid = out["calc.expr"] + "|" + out["calc.disp"] + "|" + out["calc.op"];
                    keys("4"); out.clear(); m->sample(inst, &h);
                    const std::string line = out["calc.line"];          // 0.37.63: the whole sum on one line
                    keys("d");
                    keys("3="); out.clear(); m->sample(inst, &h);
                    const std::string r1 = out["calc.disp"];
                    keys("8/0="); out.clear(); m->sample(inst, &h);
                    const std::string r2 = out["calc.disp"];
                    keys("c0.1+0.2="); out.clear(); m->sample(inst, &h);
                    const std::string r3 = out["calc.disp"];
                    const bool okc = mid == "19 x|19|3" && line == "19\xC3\x97" "4" && r1 == "57" && r2 == "Error" && r3 == "0.3" && !x->on_action(inst, "calc_key", 20) &&
                                     x->on_action(inst, "nav", 8) && (out.clear(), m->sample(inst, &h), out["ui.page"] == "8");
                    x->on_action(inst, "nav", 0); out.clear(); m->sample(inst, &h);
                    std::printf("calculator: ok=%d mid='%s' line='%s' 57='%s' err='%s' 0.3='%s'\n", okc, mid.c_str(), line.c_str(), r1.c_str(), r2.c_str(), r3.c_str());
                    ok = ok && okc;
                }
                ok = ok && okh;
            }
            // the tick side knows the names too (MISSIONS: is there a picture for this condition?)
            h.read_romfs = hw.read_romfs;
        }
        // item pictures: the row names "module:icon/icon_x"; load_image reads user:icons/icon_x.rgba
        for (int i = 0; i < 30; ++i) { out.clear(); m->sample(inst, &h); }
        h.read_romfs = [](void*, const char* path, uint64_t off, void* dst, size_t n) -> size_t {
            static uint8_t file[12 + 2 * 3 * 4];
            static const char idx[] = "Other_Thumbnail\t1x1\nIcon_X_Thumbnail\t2x3\nPortrait_Cobra\t2x3\nMission_Duff\t2x3\n";
            if (std::strcmp(path, "user:icons/_index.txt") == 0) {
                size_t m = sizeof idx - 1 > off ? sizeof idx - 1 - off : 0; if (m > n) m = n;
                std::memcpy(dst, idx + off, m); return m;
            }
            if (std::strcmp(path, "user:icons/Icon_X_Thumbnail.rgba") != 0) return 0;   // case and suffix differ
            std::memcpy(file, "DTDI", 4); uint32_t w = 2, hh = 3; std::memcpy(file + 4, &w, 4); std::memcpy(file + 8, &hh, 4);
            for (int k = 0; k < 24; ++k) file[12 + k] = static_cast<uint8_t>(k);
            if (off + n > sizeof file) return 0;
            std::memcpy(dst, file + off, n); return n;
        };
        struct Got { uint32_t w = 0, h = 0; uint8_t last = 0, pad = 1; } got;
        bool okimg = out["row.isrc0"] == "module:icon/icon_x" && x->load_image &&
                     x->load_image(inst, &h, "module:icon/icon_x", &got,
                                   [](void* r, uint32_t w, uint32_t hh, const uint8_t* px, size_t n) {
                                       auto& g = *static_cast<Got*>(r); g.w = w; g.h = hh;
                                       g.last = px[(2 * 3 + 1) * 4 + 3]; g.pad = px[n - 1]; }) &&
                     got.w == 3 && got.h == 3 && got.last == 23 && got.pad == 0 &&   // 2x3 centred on 3x3
                     !x->load_image(inst, &h, "module:icon/missing", &got, [](void*, uint32_t, uint32_t, const uint8_t*, size_t) {});
        std::printf("icons: ok=%d isrc0='%s' %ux%u\n", okimg, out["row.isrc0"].c_str(), got.w, got.h);
        ok = ok && okimg;
        // mission icons: MissionHUD slot 0, condition 0 asks for item 1010001 (icon "icon_x")
        {
            uint64_t mcls = klass("MissionHUD"), mst = alloc(0x40), hud = object(mcls, 0x100);
            put<uint64_t>(main_base + 0x9DB2820, mcls); put<uint64_t>(mcls + 0xB8, mst); put<uint64_t>(mst, hud);
            uint64_t d = object(klass("Dictionary`2"), 0x60), en = object(klass("Entry[]"), 0x20 + 0x18 * 2);
            put<uint64_t>(hud + 0x80, d); put<uint64_t>(d + 0x18, en);
            uint64_t slot = object(klass("MissionHUDSlot"), 0x100); put<uint64_t>(en + 0x30, slot);
            uint64_t lst = object(klass("List`1"), 0x20), arr = object(klass("Row[]"), 0x20 + 8 * 2);
            put<uint64_t>(slot + 0x58, lst); put<uint64_t>(lst + 0x10, arr); put<int32_t>(lst + 0x18, 1);
            uint64_t row = object(klass("MissionHUDConditionRow"), 0xA0), cd = object(klass("MissionConditionData"), 0x40);
            put<uint64_t>(arr + 0x20, row); put<uint64_t>(row + 0x90, cd); put<int32_t>(cd + 0x20, 1010001);
            // the mission's character: slot.<Data> @0x88 -> MissionData.<NpcData> @0x40 -> key @0x10
            uint64_t md = object(klass("MissionData"), 0x140), npc = object(klass("NPCNormal"), 0x98);
            put<int32_t>(md + 0x38, 1);   // MissionType.Side: not a main mission
            put<uint64_t>(slot + 0x88, md); put<uint64_t>(md + 0x40, npc); put<uint64_t>(npc + 0x18, il2str("cobra"));   // as on device
            put<uint64_t>(npc + 0x28, il2str("NPC_Name_Cobra"));
            put<uint64_t>(npc + 0x48, il2str("Mission_Duff"));   // the icon name wins over the key patterns
            for (int i = 0; i < 40; ++i) { out.clear(); m->sample(inst, &h); }   // every frame must carry the sushi lists
            bool okm = out["m.ic0_0"] == "module:icon/icon_x" && out["m.tgt0_0"] == "1010001" && out.count("m.ic0_1") == 0 &&
                       out["m.npckey0"] == "cobra" && out["m.pic0"] == "module:icon/Mission_Duff" && out["m.haspic0"] == "1" &&
                       out["m.haspic1"] == "0";
            {   // pin the mission to DIVE, then unpin it
                auto getx2 = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
                const EdenDsmodModuleExtensions* x2 = getx2 ? getx2(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : nullptr;
                okm = okm && x2 && out["m.anypin"] == "0" && x2->on_action(inst, "m_pin", 0);
                out.clear(); m->sample(inst, &h);
                okm = okm && out["m.pin0"] == "1" && out["m.pin1"] == "0" && out["m.anypin"] == "1" && out["m.main0"] == "0";
                okm = okm && x2->on_action(inst, "m_pin", 0) && !x2->on_action(inst, "m_pin", 2);   // slot 2 holds no mission
                out.clear(); m->sample(inst, &h);
                okm = okm && out["m.pin0"] == "0" && out["m.anypin"] == "0";
                put<int32_t>(md + 0x38, 0);   // now a main mission (MissionType.Main)
                for (int i = 0; i < 31; ++i) { out.clear(); m->sample(inst, &h); }
                okm = okm && out["m.main0"] == "1";
            }
            std::printf("missions: ok=%d ic0_0='%s' pic0='%s'\n", okm, out["m.ic0_0"].c_str(), out["m.pic0"].c_str());
            ok = ok && okm;
        }
    }
    // the watch: date, time of day and weather from the save's GameInfo (SaveData+0xF0)
    {
        uint64_t gi = alloc(0x280), wi = object(klass("WeatherInfo"), 0x20);
        const uint64_t key = 0x1234567890ABCDEFull, ticks = 638006112000000000ull;   // Thu 2022-10-06
        put<uint64_t>(gi + 0x88, key); put<uint64_t>(gi + 0x90, ticks ^ key);
        put<int32_t>(gi + 0xD8, 0x55); put<int32_t>(gi + 0xDC, 1 ^ 0x55);           // Afternoon
        put<uint64_t>(gi + 0xF8, wi); put<int32_t>(wi + 0x10, 1);                     // Rainy
        put<uint64_t>(gs + 0xF0, gi);
        out.clear(); m->sample(inst, &h);
        bool okc = out["clock.ok"] == "1" && out["clock.date"] == "THU 10.06" &&
                   out["clock.tsrc"] == "module:icon/UI_Watch_Icon_Afternoon" && out["clock.wsrc"] == "module:icon/Icon_Weather_Rainy";
        put<int32_t>(gi + 0xDC, 2 ^ 0x55);                                            // Evening: the moon
        put<uint64_t>(gi + 0x130, ticks - 4 * 864000000000ull);                       // new moon 4 days ago
        out.clear(); m->sample(inst, &h);
        okc = okc && out["clock.tsrc"] == "module:icon/UI_Watch_Icon_Moon_WaxingCrescent" && out["clock.ttxt"] == "EVENING";
        {   // the game's own table (DayManager.CalculateMoonState): days since the new moon, modulo 30
            const struct { int day; const char* moon; } kCases[] = {
                {0, "NewMoon"}, {8, "FirstQuarter"}, {12, "WaxingGibbous"}, {15, "FullMoon"}, {20, "WaningGibbous"},
                {23, "LastQuarter"}, {28, "WaningCrescent"}, {30, "NewMoon"}, {31, "WaxingCrescent"}, {-2, "WaningCrescent"}};
            for (const auto& c : kCases) {
                put<uint64_t>(gi + 0x130, ticks - static_cast<int64_t>(c.day) * 864000000000ll);
                out.clear(); m->sample(inst, &h);
                const std::string want = std::string("module:icon/UI_Watch_Icon_Moon_") + c.moon;
                if (out["clock.tsrc"] != want) { okc = false; std::printf("moon day %d: got %s\n", c.day, out["clock.tsrc"].c_str()); }
            }
            put<uint64_t>(gi + 0x130, ticks - 4 * 864000000000ull);
            out.clear(); m->sample(inst, &h);
        }
        std::printf("clock: ok=%d date='%s' time='%s' weather='%s' diag='%s'\n", okc, out["clock.date"].c_str(),
                    out["clock.tsrc"].c_str(), out["clock.wsrc"].c_str(), out["clock.diag"].c_str());
        ok = ok && okc;
        // 0.37.65: the phone's open app -- Singleton<DavePhoneConroller> -> m_ContentsDic[Message] = MailPanel -> rows
        {
            uint64_t sg = klass("Singleton`1"), st = alloc(0x10), dpc = object(klass("DavePhoneConroller"), 0x100);
            put<uint64_t>(main_base + 0x9DA5928, sg); put<uint64_t>(sg + 0xB8, st); put<uint64_t>(st, dpc);
            put<uint64_t>(dpc + 0x10, 0x1234); put<int32_t>(dpc + 0xA8, 14060006); put<uint8_t>(dpc + 0xF8, 1);
            uint64_t mp = object(klass("MailPanel"), 0x80), lst = alloc(0x20), arr = alloc(0x20 + 8 * 2);
            uint64_t d = object(klass("Dictionary`2"), 0x60), en = object(klass("Entry[]"), 0x20 + 0x18);
            put<int32_t>(en + 0x20, 1); put<int32_t>(en + 0x28, 14060006); put<uint64_t>(en + 0x30, mp);
            put<uint64_t>(d + 0x18, en); put<int32_t>(d + 0x20, 1); put<uint64_t>(dpc + 0x88, d);
            put<uint64_t>(mp + 0x68, lst); put<uint64_t>(lst + 0x10, arr); put<int32_t>(lst + 0x18, 2); put<int32_t>(mp + 0x74, 1);
            const char* titles[2] = {"Tuna Yacht Party!", "Best Diving Spot"}; const char* senders[2] = {"Blue Hole", "Item_Name_BlueTang"};   // a text key: looked up in the text table
            for (int i = 0; i < 2; ++i) {
                uint64_t cell = alloc(0x30), md = alloc(0x68);
                put<uint64_t>(arr + 0x20 + 8 * i, cell); put<uint64_t>(cell + 0x18, md); put<int32_t>(md + 0x10, 900 + i);
                put<uint64_t>(cell + 0x20, 638006112000000000ull); put<uint8_t>(cell + 0x28, i == 0 ? 1 : 0);
                put<uint64_t>(md + 0x48, il2str(titles[i])); put<uint64_t>(md + 0x40, il2str(senders[i]));
            }
            for (int i = 0; i < 12; ++i) { out.clear(); m->sample(inst, &h); }
            const bool okp = out["ph.mail0"] == "0. Tuna Yacht Party! / Blue Hole []" && out["ph.mail1"] == "1. N Best Diving Spot / Blue Tang []" &&
                             out["mail.d1"] == "10.06" && out["mail.new1"] == "1" && out["mail.new0"] == "0" && out["mail.cur1"] == "1" &&
                             out["ph.diag"].find("mail 2 sel 1") != std::string::npos;
            std::printf("phone: ok=%d diag='%s' 0='%s' 1='%s'\n", okp, out["ph.diag"].c_str(), out["ph.mail0"].c_str(), out["ph.mail1"].c_str());
            ok = ok && okp;
            // 0.37.68: tap row 0 with the phone in Mail (cursor on 1): Up, then (cursor on 0) A
            auto getx5 = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
            const EdenDsmodModuleExtensions* x5 = getx5 ? getx5(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : nullptr;
            uint64_t rp = alloc(0x30); put<uint64_t>(dpc + 0xB0, rp); put<int32_t>(rp + 0x10, 2);   // Execute
            bool okj = x5 && x5->on_action(inst, "mail_open", 0);
            std::string pressed;
            for (int i = 0; i < 40; ++i) {
                out.clear(); m->sample(inst, &h);
                if (out["ph.p_up"] == "1") { pressed += "U"; put<int32_t>(mp + 0x74, 0); }
                if (out["ph.p_a"] == "1") pressed += "A";
            }
            const std::string st1 = out["ph.status"];
            // 0.37.86: mail 0 is open on top; tap mail 1 -> B (close it), Down, A
            okj = okj && x5->on_action(inst, "mail_open", 1);
            std::string pz;
            for (int i = 0; i < 40; ++i) {
                out.clear(); m->sample(inst, &h);
                if (out["ph.p_b"] == "1") pz += "B";
                if (out["ph.p_down"] == "1") { pz += "D"; put<int32_t>(mp + 0x74, 1); }
                if (out["ph.p_a"] == "1") pz += "A";
            }
            std::printf("next mail: presses '%s'\n", pz.c_str());
            okj = okj && pz == "BDA";
            // the phone on its home grid (Mail 6th, focus 1, 4 columns): Down first
            put<int32_t>(rp + 0x10, 1);
            uint64_t pc = alloc(0x100), gl = alloc(0x20), ga = alloc(0x20 + 8 * 8);
            put<uint64_t>(dpc + 0x48, pc); put<uint64_t>(pc + 0x88, gl); put<uint64_t>(gl + 0x10, ga); put<int32_t>(gl + 0x18, 8);
            for (int i = 0; i < 8; ++i) { uint64_t dd = alloc(0x28); put<int32_t>(dd + 0x10, i == 5 ? 14060006 : 14060001 + i); put<uint64_t>(ga + 0x20 + 8 * i, dd); }
            put<int32_t>(pc + 0x90, 1); put<int32_t>(pc + 0xDC, 4);
            okj = okj && x5->on_action(inst, "mail_open", 1);
            std::string p2;
            for (int i = 0; i < 6; ++i) { out.clear(); m->sample(inst, &h); if (out["ph.p_down"] == "1") p2 += "D"; if (out["ph.p_right"] == "1") p2 += "R"; }
            x5->on_action(inst, "ph_stop", 0); out.clear(); m->sample(inst, &h);
            {   // 0.37.87: Mail open (no mail on top), another app wanted: one B, then wait -- no second B while the
                // state still reads Execute (a quick second B closed the phone on device)
                put<int32_t>(rp + 0x10, 2); put<int32_t>(dpc + 0xA8, 14060006);
                x5->on_action(inst, "ph_close", 0); x5->on_action(inst, "ph_stop", 0);
                x5->on_action(inst, "ph_stop", 0);
                int nb = 0;
                // a job for an app that is not open: reuse mail_open with the Message app replaced by "EcoWatcher open"
                put<int32_t>(dpc + 0xA8, 14060001);
                x5->on_action(inst, "mail_open", 0);
                for (int i = 0; i < 40; ++i) { out.clear(); m->sample(inst, &h); if (out["ph.p_b"] == "1") ++nb; }
                x5->on_action(inst, "ph_stop", 0); out.clear(); m->sample(inst, &h);
                put<int32_t>(dpc + 0xA8, 14060006);
                std::printf("app switch: B presses %d (want 1)\n", nb);
                okj = okj && nb == 1;
            }
            {   // 0.37.88: another app open; after its one B the phone shows home but NowState stays Execute and
                // m_ExecuteApp the old app -> the job moves on the grid (D ...), no second B
                put<int32_t>(rp + 0x10, 2); put<int32_t>(dpc + 0xA8, 14060001); put<int32_t>(pc + 0x90, 1);
                x5->on_action(inst, "mail_open", 1);
                std::string p3;
                for (int i = 0; i < 60; ++i) { out.clear(); m->sample(inst, &h);
                    if (out["ph.p_b"] == "1") p3 += "B"; if (out["ph.p_down"] == "1") { p3 += "D"; put<int32_t>(pc + 0x90, 5); }
                    if (out["ph.p_a"] == "1") p3 += "A"; }
                x5->on_action(inst, "ph_stop", 0); out.clear(); m->sample(inst, &h);
                put<int32_t>(dpc + 0xA8, 14060006); put<int32_t>(rp + 0x10, 1);
                std::printf("app switch home: presses '%s' (want BDA)\n", p3.c_str());
                okj = okj && p3 == "BDA";
            }
            okj = okj && pressed == "UA" && st1 == "Opened on the top screen" && p2.size() && p2[0] == 'D' && out["ph.busy"] == "0";
            std::printf("phone job: ok=%d presses '%s' status '%s' home '%s'\n", okj, pressed.c_str(), st1.c_str(), p2.c_str());
            ok = ok && okj;
            // 0.37.69: before Mail is shown, the list comes from the save (MailSave -> MailInfo, DataManager.MailDic)
            {
                put<int32_t>(lst + 0x18, 0);                                         // the screen's list still empty
                uint64_t msv = alloc(0x30), md2 = object(klass("Dictionary`2"), 0x60), me = object(klass("Entry[]"), 0x20 + 0x18 * 2);
                uint64_t dd2 = object(klass("Dictionary`2"), 0x60), de = object(klass("Entry[]"), 0x20 + 0x18 * 2);
                const int32_t ids[2] = {501, 502}; const uint64_t tks[2] = {638006112000000000ull, 638006976000000000ull};
                const char* tt[2] = {"Older mail", "Newer mail"};
                for (int i = 0; i < 2; ++i) {
                    uint64_t mi = alloc(0x70), mdd = object(klass("MailData"), 0x68);
                    put<int32_t>(mi + 0x18, 0x55); put<int32_t>(mi + 0x1C, ids[i] ^ 0x55);
                    put<uint64_t>(mi + 0x30, 0x77); put<uint64_t>(mi + 0x38, tks[i] ^ 0x77);
                    put<uint8_t>(mi + 0x58, 0x11); put<int32_t>(mi + 0x5C, (i == 0 ? 213 : 181) ^ 0x11);   // 0 read, 1 unread
                    put<int32_t>(me + 0x20 + 0x18 * i, 1); put<uint64_t>(me + 0x28 + 0x18 * i, il2str(i ? "b" : "a")); put<uint64_t>(me + 0x30 + 0x18 * i, mi);
                    put<int32_t>(mdd + 0x10, ids[i]); put<uint64_t>(mdd + 0x48, il2str(tt[i])); put<uint64_t>(mdd + 0x40, il2str("Mom"));
                    put<int32_t>(de + 0x20 + 0x18 * i, 1); put<int32_t>(de + 0x28 + 0x18 * i, ids[i]); put<uint64_t>(de + 0x30 + 0x18 * i, mdd);
                }
                put<uint64_t>(md2 + 0x18, me); put<int32_t>(md2 + 0x20, 2); put<uint64_t>(msv + 0x10, md2); put<uint64_t>(gs + 0x1A0, msv);
                put<uint64_t>(dd2 + 0x18, de); put<int32_t>(dd2 + 0x20, 2); put<uint64_t>(dm + 0x418, dd2);
                for (int i = 0; i < 24; ++i) { out.clear(); m->sample(inst, &h); }
                const bool oks = out["mail.ok"] == "1" && out["mail.t0"] == "Newer mail" && out["mail.new0"] == "1" &&
                                 out["mail.t1"] == "Older mail" && out["mail.new1"] == "0" && out["mail.on2"] == "0";
                std::printf("mail from the save: ok=%d 0='%s' %s 1='%s' %s diag='%s'\n", oks, out["mail.t0"].c_str(), out["mail.new0"].c_str(),
                            out["mail.t1"].c_str(), out["mail.new1"].c_str(), out["ph.diag"].c_str());
                ok = ok && oks;
            }
        }
        put<uint64_t>(gs + 0xF0, 0);
    }
    // gold and Bei: SaveData+0x240 -> PlayerInfoSave, m_Gold +0x10, m_Bei +0x24 (ObscuredInt)
    {
        out.clear(); m->sample(inst, &h);
        bool okg = out["gold.ok"] == "0";
        uint64_t pinfo = object(klass("PlayerInfoSave"), 0x200);
        obscured(pinfo + 0x10, 123456, 0x3C3C); obscured(pinfo + 0x24, 789, 0x77);
        put<uint64_t>(gs + 0x240, pinfo);
        out.clear(); m->sample(inst, &h);
        okg = okg && out["gold.ok"] == "1" && out["gold.v"] == "123456" && out["bei.v"] == "789" && out["gold.txt"] == "123,456";
        std::printf("gold: ok=%d gold=%s bei=%s\n", okg, out["gold.v"].c_str(), out["bei.v"].c_str());
        ok = ok && okg;
        // 0.37.72: EcoWatcher -- MissionManager.InProgressList (OceanLab only), a divider, the done OceanLab missions;
        // research points 150 with ranks at 0 / 100 / 300 -> rank 2, 150 / 300
        {
            uint64_t mmc = klass("MissionManager"), sgc = klass("Singleton`1"), sst = alloc(0x10), mm = object(mmc, 0x80);
            put<uint64_t>(main_base + 0x9DB2850, mmc); put<uint64_t>(mmc + 0x58, sgc); put<uint64_t>(sgc + 0xB8, sst); put<uint64_t>(sst, mm);
            auto mission = [&](int tid, int type, int state, const char* key) {
                uint64_t md = object(klass("MissionData"), 0x130);
                put<int32_t>(md + 0x10, tid); put<uint64_t>(md + 0x28, il2str(key)); put<int32_t>(md + 0x38, type); put<int32_t>(md + 0x3C, state);
                return md;
            };
            uint64_t a = mission(1, 2, 2, "Eco_Seahorse"), b = mission(2, 0, 2, "Main_Story"), c2 = mission(3, 2, 4, "Eco_Turtle");
            {   // 0.37.73: mission 1's current task: one goal, item 1010001 (Blue Tang) 1/5; its reward 20 RP
                uint64_t node = alloc(0x30), task = object(klass("MissionTaskData"), 0xB0), cl = alloc(0x20), ca = alloc(0x28), cd = alloc(0x60);
                put<uint64_t>(a + 0x100, node); put<uint64_t>(node + 0x28, task); put<uint64_t>(task + 0x98, cl);
                put<uint64_t>(cl + 0x10, ca); put<int32_t>(cl + 0x18, 1); put<uint64_t>(ca + 0x20, cd);
                put<int32_t>(cd + 0x14, 18); put<int32_t>(cd + 0x20, 1010001); put<int32_t>(cd + 0x28, 1); put<int32_t>(cd + 0x2C, 5);
                uint64_t rl = alloc(0x20), ra = alloc(0x28), rw = alloc(0x40);
                put<uint64_t>(a + 0x68, rl); put<uint64_t>(rl + 0x10, ra); put<int32_t>(rl + 0x18, 1); put<uint64_t>(ra + 0x20, rw); put<int32_t>(rw + 0x38, 20);
            }
            uint64_t il = alloc(0x20), ia = alloc(0x20 + 16); put<uint64_t>(il + 0x10, ia); put<int32_t>(il + 0x18, 2);
            put<uint64_t>(ia + 0x20, a); put<uint64_t>(ia + 0x28, b); put<uint64_t>(mm + 0x38, il);
            uint64_t dd = object(klass("Dictionary`2"), 0x60), de = object(klass("Entry[]"), 0x20 + 0x18 * 3);
            uint64_t ms[3] = {a, b, c2};
            for (int i = 0; i < 3; ++i) { put<int32_t>(de + 0x20 + 0x18 * i, 1); put<int32_t>(de + 0x28 + 0x18 * i, i + 1); put<uint64_t>(de + 0x30 + 0x18 * i, ms[i]); }
            put<uint64_t>(dd + 0x18, de); put<int32_t>(dd + 0x20, 3); put<uint64_t>(mm + 0x68, dd);
            obscured(pinfo + 0xE0, 150, 0x1234);
            uint64_t ld = object(klass("Dictionary`2"), 0x60), le = object(klass("Entry[]"), 0x20 + 0x18 * 3);
            const int reqs[3] = {0, 100, 300};
            for (int i = 0; i < 3; ++i) { uint64_t lv = alloc(0x30); put<int32_t>(lv + 0x14, i + 1); put<int32_t>(lv + 0x18, reqs[i]);
                put<int32_t>(le + 0x20 + 0x18 * i, 1); put<int32_t>(le + 0x28 + 0x18 * i, i + 1); put<uint64_t>(le + 0x30 + 0x18 * i, lv); }
            put<uint64_t>(ld + 0x18, le); put<int32_t>(ld + 0x20, 3); put<uint64_t>(dm + 0x2E0, ld);
            for (int i = 0; i < 12; ++i) { out.clear(); m->sample(inst, &h); }
            const bool oke = out["eco.t0"] == "Eco Seahorse" && out["eco.st0"] == "0" && out["eco.div1"] == "1" &&
                             out["eco.t2"] == "Eco Turtle" && out["eco.st2"] == "2" && out["eco.on3"] == "0" &&
                             out["eco.lv"] == "2" && out["eco.rptxt"] == "150 / 300";
            std::printf("ecowatcher: ok=%d diag='%s' 0='%s' 2='%s' rank %s %s\n", oke, out["eco.diag"].c_str(), out["eco.t0"].c_str(),
                        out["eco.t2"].c_str(), out["eco.lv"].c_str(), out["eco.rptxt"].c_str());
            ok = ok && oke;
            // 0.37.73: the goal on the row, its RP, and PIN -> the map's hunt list; 0.37.74: a Delivering goal counts what Dave holds (cargo 2 + storage 7)
            auto getx6 = (EdenDsmodGetExtensionsFn)dlsym(so, "eden_dsmod_get_extensions");
            const EdenDsmodModuleExtensions* x6 = getx6 ? getx6(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH) : nullptr;
            bool okp2 = x6 && x6->on_action(inst, "eco_pin", 0) && !x6->on_action(inst, "eco_pin", 2);   // a done one cannot be pinned
            for (int i = 0; i < 4; ++i) { out.clear(); m->sample(inst, &h); }
            std::string pl;
            for (int j = 0; j < 8; ++j) if (out["pl.on" + std::to_string(j)] == "1") pl += out["pl.nn" + std::to_string(j)] + " " + out["pl.have" + std::to_string(j)] + "/" + out["pl.need" + std::to_string(j)] + ";";
            okp2 = okp2 && out["eco.gc0_0"] == "9/5" && out["eco.rp0"] == "20" && out["eco.pin0"] == "1" && out["hunt.on"] == "1" &&
                   pl.find("Blue Tang 9/5") != std::string::npos;
            x6->on_action(inst, "eco_pin", 0); for (int i = 0; i < 2; ++i) { out.clear(); m->sample(inst, &h); }
            okp2 = okp2 && out["eco.pin0"] == "0";
            // 0.37.75: an item that is not an ingredient (Items.ItemType 5, Loot) counts the cargo + the save's inventory
            {
                uint64_t id = object(klass("Dictionary`2"), 0x60), ie = object(klass("Entry[]"), 0x20 + 0x18);
                uint64_t iv = object(klass("Items"), 0xD0); put<int32_t>(iv + 0x10, 1010001); put<int32_t>(iv + 0x38, 5);
                put<int32_t>(ie + 0x20, 1); put<int32_t>(ie + 0x28, 1010001); put<uint64_t>(ie + 0x30, iv);
                put<uint64_t>(id + 0x18, ie); put<int32_t>(id + 0x20, 1); put<uint64_t>(dm + 0x1C0, id);
                uint64_t vd = object(klass("Dictionary`2"), 0x60), ve = object(klass("Entry[]"), 0x20 + 0x18);
                uint64_t sl = object(klass("InventoryItemSlotSave"), 0x60); obscured(sl + 0x2C, 1010001, 0x1357); obscured(sl + 0x40, 3, 0x2468);
                put<int32_t>(ve + 0x20, 1); put<uint64_t>(ve + 0x30, sl);
                put<uint64_t>(vd + 0x18, ve); put<int32_t>(vd + 0x20, 1); put<uint64_t>(gs + 0x140, vd);
                for (int i = 0; i < 12; ++i) { out.clear(); m->sample(inst, &h); }   // EcoStep runs every 10th sample
                const bool oki = out["eco.gc0_0"] == "5/5";
                std::printf("ecowatcher inventory: ok=%d goal '%s' diag '%s'\n", oki, out["eco.gc0_0"].c_str(), out["eco.diag"].c_str());
                okp2 = okp2 && oki;
            }
            std::printf("ecowatcher pin: ok=%d goal '%s' rp %s hunt '%s'\n", okp2, out["eco.gc0_0"].c_str(), out["eco.rp0"].c_str(), pl.c_str());
            ok = ok && okp2;
            {   // 0.37.89: Weapon Shop -- two saved crafts, one root (kept) and one with an InCraftList (left out);
                // the root one: 3 of 10 develop (Blueprint MaxDevelopProgressCount), price 500, a material 3 / 2
                auto dict1 = [&](uint64_t key, uint64_t val, int k2) {   // a one-entry (or two) Dictionary<int, ...>
                    (void)k2; uint64_t d = object(klass("Dictionary`2"), 0x60), e = object(klass("Entry[]"), 0x20 + 0x18 * 2);
                    put<int32_t>(e + 0x20, 1); put<int32_t>(e + 0x28, static_cast<int32_t>(key)); put<uint64_t>(e + 0x30, val);
                    put<uint64_t>(d + 0x18, e); put<int32_t>(d + 0x20, 1); return std::pair<uint64_t, uint64_t>(d, e); };
                uint64_t c1 = object(klass("Craft"), 0x60), c2 = object(klass("Craft"), 0x60);
                put<int32_t>(c1 + 0x10, 501); put<int32_t>(c1 + 0x20, 500); put<int32_t>(c1 + 0x38, 1010001);
                uint64_t il = alloc(0x20), ia = alloc(0x28), nl = alloc(0x20), na = alloc(0x28);
                put<uint64_t>(il + 0x10, ia); put<int32_t>(il + 0x18, 1); put<int32_t>(ia + 0x20, 1010001);
                put<uint64_t>(nl + 0x10, na); put<int32_t>(nl + 0x18, 1); put<int32_t>(na + 0x20, 2);
                put<uint64_t>(c1 + 0x28, il); put<uint64_t>(c1 + 0x30, nl);
                put<int32_t>(c2 + 0x10, 502); uint64_t inl = alloc(0x20); put<int32_t>(inl + 0x18, 1); put<uint64_t>(c2 + 0x40, inl);
                auto cdp = dict1(501, c1, 0);
                put<int32_t>(cdp.second + 0x38, 1); put<int32_t>(cdp.second + 0x40, 502); put<uint64_t>(cdp.second + 0x48, c2);
                put<int32_t>(cdp.first + 0x20, 2); put<uint64_t>(dm + 0xD8, cdp.first);
                uint64_t bp = object(klass("Blueprint"), 0x40); put<int32_t>(bp + 0x30, 10); put<int32_t>(bp + 0x34, 501);
                put<uint64_t>(dm + 0xD0, dict1(7001, bp, 0).first);
                uint64_t s1 = object(klass("GunCraftInfoSave"), 0x60), s2 = object(klass("GunCraftInfoSave"), 0x60);
                obscured(s1 + 0x10, 501, 0x55); obscured(s1 + 0x24, 3, 0x66); put<int32_t>(s1 + 0x4C, 0);
                obscured(s2 + 0x10, 502, 0x77);
                auto gsd = dict1(501, s1, 0);
                put<int32_t>(gsd.second + 0x38, 1); put<int32_t>(gsd.second + 0x40, 502); put<uint64_t>(gsd.second + 0x48, s2);
                put<int32_t>(gsd.first + 0x20, 2); put<uint64_t>(gs + 0xF8, gsd.first);
                for (int i = 0; i < 12; ++i) { out.clear(); m->sample(inst, &h); }
                const bool oks = out["shop.rows"] == "1" && out["shop.pct0"] == "30" && out["shop.price0"] == "500" &&
                                 out["shop.st0"] == "0" && out["shop.mc0_0"] == "3/2" && out["shop.t0"] == "Blue Tang";
                std::printf("weapon shop: ok=%d rows %s pct %s price %s st %s mat %s name '%s' diag '%s'\n", oks, out["shop.rows"].c_str(),
                            out["shop.pct0"].c_str(), out["shop.price0"].c_str(), out["shop.st0"].c_str(), out["shop.mc0_0"].c_str(),
                            out["shop.t0"].c_str(), out["shop.diag"].c_str());
                ok = ok && oks;
                // 0.37.91: developed + materials + gold -> CRAFT; tracked (SaveData+0xD0 -> +0x20 HashSet<int>)
                put<int32_t>(s1 + 0x4C, 1);
                uint64_t ex = alloc(0x30), hs = alloc(0x40), he = alloc(0x20 + 12 * 3);
                put<uint64_t>(gs + 0xD0, ex); put<uint64_t>(ex + 0x20, hs); put<uint64_t>(hs + 0x18, he); put<int32_t>(he + 0x18, 3);
                put<int32_t>(he + 0x20 + 12, 501); put<int32_t>(he + 0x24 + 12, -1); put<int32_t>(he + 0x28 + 12, 501);
                for (int i = 0; i < 12; ++i) { out.clear(); m->sample(inst, &h); }
                const bool okc = out["shop.can0"] == "1" && out["shop.short0"] == "0" && out["shop.trk0"] == "1";
                std::printf("weapon shop craft/track: ok=%d can %s short %s tracked %s\n", okc, out["shop.can0"].c_str(),
                            out["shop.short0"].c_str(), out["shop.trk0"].c_str());
                ok = ok && okc;
            }
        }
    }
    // straight to the sushi bar after loading (no dive yet, so no cargo box): the sushi pages still
    // read the save (SaveData at its pinned field, gdm+0x60)
    {
        uint64_t gs2 = object(klass("SaveData"), 0x300);
        uint64_t ingd = 0; std::memcpy(&ingd, &mem[gs + 0x120 - BASE], 8);
        put<uint64_t>(gs2 + 0x120, ingd); put<uint64_t>(gdm + 0x60, gs2);
        put<uint64_t>(gs + 0x158, 0);   // the box is gone
        for (int i = 0; i < 40; ++i) { out.clear(); m->sample(inst, &h); }   // every frame must carry the sushi lists
        bool oks = out["ing.n"] == "3" && out["cargo.n"] == "0" && out["cargo.diag"].find("sushi data read") != std::string::npos;
        std::printf("no box yet: ok=%d ing.n=%s diag='%s'\n", oks, out["ing.n"].c_str(), out["cargo.diag"].c_str());
        ok = ok && oks;
        put<uint64_t>(gs + 0x158, dict); put<uint64_t>(gdm + 0x60, 0);
    }
    // discard everything: Dictionary.Remove leaves _count at 3 with every hashCode -1
    for (int i = 0; i < N; ++i) put<int32_t>(entries + 0x20 + 0x18 * i, -1);
    put<int32_t>(dict + 0x2C, 8);
    for (int i = 0; i < 15; ++i) { out.clear(); m->sample(inst, &h); }
    bool ok_rm = out["cargo.n"] == "0" && out["cargo.ready"] == "1" && out["row.on0"] != "1";
    if (!ok_rm) std::printf("all-removed: n=%s ready=%s diag=%s\n", out["cargo.n"].c_str(), out["cargo.ready"].c_str(), out["cargo.diag"].c_str());
    ok = ok && ok_rm;
    // empty the box: list must clear
    put<int32_t>(dict + 0x20, 0); put<int32_t>(dict + 0x2C, 9);
    for (int i = 0; i < 15; ++i) m->sample(inst, &h);
    ok = ok && out["cargo.n"] == "0" && out["cargo.ready"] == "1";
    m->destroy(inst);
    std::puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
