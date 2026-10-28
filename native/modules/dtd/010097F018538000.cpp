// Dave the Diver (010097F018538000) dual-screen module: the dive cargo ("Acquired") list.
//
// Freestanding C++: no C/C++ runtime, no allocation, no exceptions. It only uses the host's
// read_memory / publish_* services, so it builds with a plain clang + lld (no Android NDK):
// see tools/dtd/build_module.sh.
//
// Data path (update v1.0.6.1267, build F74B633E480E4E16; IL2CPP v31):
//   DR.Save.SaveSystem_TypeInfo (main+0x9DB54B0) -> parent Singleton<SaveSystem> (+0x58)
//     -> static_fields (+0xB8) -> s_Instance (+0x0)                      [same trick as InGameManager]
//   SaveSystem._GameDataManager (+0x68)
//   LootBox.get_m_Box() == SaveSystem.GetGameSave() + 0x158  (main+0x456AFD0, round 4)
//   GetGameSave() returns an object reached from the game data manager; which field is not
//   pinned yet, so this build finds it by type: the field whose object has, at +0x158, a
//   Dictionary`2 whose values are LootBoxSlot. The offset found is published (cargo.gs_off) so the
//   next build can pin it.
//   LootBoxSlot: m_ItemID 0x10, m_Type 0x24 (int), m_Grade 0x28, m_FinalGrade 0x3C,
//                m_TotalCount 0x50 -- ObscuredInt {key 0x0, hidden 0x4} -> value = hidden ^ key.
//   Dictionary<K,V> (Unity mscorlib): _buckets 0x10, _entries 0x18, _count 0x20, _version 0x2C;
//   Entry {hashCode 0, next 4, key 8, value 0x10}, 0x18 bytes; removed entries have hashCode -1.

#include <stddef.h>
#include <stdint.h>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

// The compiler may emit these for struct copies / zeroing.
extern "C" __attribute__((visibility("hidden"))) void* memcpy(void* d, const void* s, size_t n) {
    auto* o = static_cast<unsigned char*>(d);
    auto* i = static_cast<const unsigned char*>(s);
    while (n--) *o++ = *i++;
    return d;
}
extern "C" __attribute__((visibility("hidden"))) void* memset(void* d, int c, size_t n) {
    auto* o = static_cast<unsigned char*>(d);
    while (n--) *o++ = static_cast<unsigned char>(c);
    return d;
}
extern "C" __attribute__((visibility("hidden"))) void* memmove(void* d, const void* s, size_t n) {
    auto* o = static_cast<unsigned char*>(d);
    auto* i = static_cast<const unsigned char*>(s);
    if (o < i) { while (n--) *o++ = *i++; }
    else { o += n; i += n; while (n--) *--o = *--i; }
    return d;
}

namespace {

constexpr uint64_t kTitle = UINT64_C(0x010097F018538000);
constexpr const char kBuild[] = "F74B633E480E4E161F80A7C16A467F2A";  // first 16 bytes of the build ID
constexpr uint64_t kSaveSystemTypeInfo = 0x9DB54B0;
constexpr uint64_t kParent = 0x58, kStatics = 0xB8;
constexpr uint64_t kGameDataManager = 0x68;
constexpr uint64_t kBoxField = 0x158;
constexpr uint64_t kGameSaveField = 0x60;     // found on device by 0.5.0 (cargo.gs_off = 0x60)
// Metadata slot of "Method$Singleton<DataManager>.get_Instance()" (used by LootBox.ClearDebuff,
// main+0x456B0F8). Once initialised it holds the MethodInfo*, whose klass is Singleton<DataManager>.
constexpr uint64_t kDataManagerMethodSlot = 0x9E0E810;
constexpr int kMaxItems = 40;
constexpr int kMaxRows = 40;
constexpr int kRescanTicks = 15;  // walk the cargo 4x a second; republish every tick

// ---------------------------------------------------------------- tiny string helpers
struct Buf {
    char s[256];
    int n = 0;
    Buf() { s[0] = 0; }   // an untouched Buf publishes as ""
    void clear() { n = 0; s[0] = 0; }
    Buf& add(const char* t) { while (*t && n < 255) s[n++] = *t++; s[n] = 0; return *this; }
    Buf& addc(char c) { if (n < 255) s[n++] = c; s[n] = 0; return *this; }
    Buf& num(int64_t v) {
        char t[24]; int k = 0; bool neg = v < 0; uint64_t u = neg ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
        do { t[k++] = static_cast<char>('0' + u % 10); u /= 10; } while (u);
        if (neg) addc('-');
        while (k) addc(t[--k]);
        return *this;
    }
    Buf& hex(uint64_t v) {
        add("0x"); char t[20]; int k = 0;
        do { t[k++] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v);
        while (k) addc(t[--k]);
        return *this;
    }
};

// The companion's pixel font has no apostrophe (it draws a gap: "CAN T"), so names lose theirs:
// "Duff's" -> "Duffs". Straight ' and the typographic U+2018/U+2019 (E2 80 98/99) are dropped.
// With the game's font in use (it has one) they stay, as a straight ' (the font covers U+0020..U+00FF).
extern bool g_font_apos;
const char* NoApos(const char* t, Buf& out) {
    out.clear();
    for (; *t; ++t) {
        const unsigned char c = static_cast<unsigned char>(*t);
        if (c == '\'') { if (g_font_apos) out.addc('\''); continue; }
        if (c == 0xE2 && static_cast<unsigned char>(t[1]) == 0x80 &&
            (static_cast<unsigned char>(t[2]) == 0x98 || static_cast<unsigned char>(t[2]) == 0x99)) {
            t += 2;
            if (g_font_apos) out.addc('\'');
            continue;
        }
        out.addc(*t);
    }
    return out.s;
}
bool eq(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
bool starts(const char* a, const char* p) { while (*p) { if (*a++ != *p++) return false; } return true; }

// ---------------------------------------------------------------- guest reads
struct Guest {
    const EdenDsmodHostApi* h;
    bool read(uint64_t a, void* o, size_t n) const {
        if (a == 0 || a + n < a) return false;
        if (h->is_mapped && !h->is_mapped(h->userdata, a, n)) return false;
        return h->read_memory(h->userdata, a, o, n) != 0;
    }
    uint64_t ptr(uint64_t a) const { uint64_t v = 0; return read(a, &v, 8) ? v : 0; }
    int32_t s32(uint64_t a, bool* ok = nullptr) const {
        int32_t v = 0; bool r = read(a, &v, 4); if (ok) *ok = r; return v;
    }
    // ObscuredInt: currentCryptoKey @0, hiddenValue @4
    bool obscured(uint64_t a, int32_t* out) const {
        int32_t kv[2];
        if (!read(a, kv, 8)) return false;
        *out = kv[1] ^ kv[0];
        return true;
    }
    // C string at a (class names)
    bool cstr(uint64_t a, char* o, int cap) const {
        if (!a) return false;
        for (int i = 0; i < cap - 1; ++i) {
            char c; if (!read(a + i, &c, 1)) { o[i] = 0; return i > 0; }
            o[i] = c; if (!c) return true;
        }
        o[cap - 1] = 0; return true;
    }
    // Class name of an IL2CPP object (klass @0 -> name @0x10)
    bool cls(uint64_t obj, char* o, int cap) const {
        uint64_t k = ptr(obj); if (!k) return false;
        return cstr(ptr(k + 0x10), o, cap);
    }
    // Il2CppString -> ASCII (length @0x10, UTF-16 @0x14); non-ASCII becomes '?'
    bool str(uint64_t s, char* o, int cap) const {
        o[0] = 0; if (!s) return false;
        int32_t len = s32(s + 0x10); if (len < 0 || len > 512) return false;
        int n = len < cap - 1 ? len : cap - 1;
        uint16_t w[128]; if (n > 128) n = 128;
        if (n && !read(s + 0x14, w, static_cast<size_t>(n) * 2)) return false;
        for (int i = 0; i < n; ++i) o[i] = w[i] < 0x80 ? static_cast<char>(w[i]) : '?';
        o[n] = 0; return true;
    }
};

struct Item {
    char key[48];
    int32_t id, type, grade, fgrade, count;
    int32_t lift;                 // LootBoxSlot.m_AutoLiftedType (part of the slot's key, as the grade)
    // from the game's item table (IntegratedItem), when found
    bool has_info;
    int32_t rank, sell, itype;
    int32_t carve;                // FishInfoData.CarvableCount of the fish whose DropItemID is this item (0 = none)
    float weight;
    char text_id[64];
    char icon[64];
    char name[48];                // display name (English), or the text key until it is resolved
};

struct Row {                      // one catch: which group, and when it was caught
    int item;
    int pos;                      // place in the game's own Acquired list (newest first): the menu route walks it
    char time[32];
    char key[32];                 // sortable form of time
};

struct Ingredient {               // one stack in the sushi bar's ingredient storage (SaveData+0x120)
    int32_t id, count, level, parent;
    bool has_info;
    int32_t rank;
    char name[48];
    char icon[64];                // picture name (Ingredients.ContentsThumbnail, else the item's ItemUIIcon)
};
constexpr int kMaxIngredients = 200;   // the grid scrolls, so every stack fits (empty ones too)
// 0.37.56: other names the game gives an ingredient's picture, looked up only for ingredients whose own names found
// none (IngPicture asks, ScanIngredients answers two per scan): the item table's ItemUIIcon for the same id, and the
// item icon of the fish that drops it (FishInfoDataEntity +0x7C = drop id, key = fish id).
struct AltIcon { int32_t id; char item[64]; char fish[64]; int32_t fish_id; uint8_t done; };
AltIcon g_alt[64]; int g_nalt;
int32_t g_alt_want[16]; int g_nwant;

constexpr int kMaxRecipes = 120, kMaxNeeds = 4;   // the recipe list scrolls
struct Recipe {                   // an unlocked recipe and what it needs (DataManager.RecipeDataDic)
    int32_t id;
    char name[48];
    int nneed;
    int32_t need_id[kMaxNeeds], need_qty[kMaxNeeds], have[kMaxNeeds];
    char need_name[kMaxNeeds][48];
    char icon[64];                // RecipeEntity.Icon (+0x28): the dish picture, e.g. "Sushi_Alaska_Pollock"
    char need_icon[kMaxNeeds][64];
    int32_t need_item[kMaxNeeds];  // the item a catch of it is (IngredientsEntity.ItemsTID): counts the cargo
    int32_t can_make;             // servings possible with what is in storage
    int32_t price;                // RecipeEntity.Price (+0x44): gold per plate (round 24)
    bool on_menu;
};

// 0.38.50: 1024 (was 128). Items, ingredients and recipes share it; later in the game it filled up and every new
// name overwrote the last slot, so names never stuck (keys like "Starfish001 Name" stayed, a crab's name flipped)
constexpr int kNames = 1024;
struct NameCache {                // item id -> display name, resolved once from DataManager.TextDataDic
    int32_t id[kNames];
    char name[kNames][48];
    int n, next;                  // next: the slot reused once full (round robin, oldest first)
};

struct NameSearch {               // an incremental walk of the (large) text dictionary
    bool active;
    int32_t id;
    char key[64];
    int keylen;
    int32_t cursor;
};

struct InfoCache {                // item id -> IntegratedItem object, so the table is walked once per id
    int32_t id[128];
    uint64_t obj[128];
    int n;
};

struct Instance {                  // plain data: zeroed in Create (no static constructor)
    uint64_t main_base;
    uint64_t tick;
    int64_t gs_off;               // field of the game data manager holding the game save (-1 = not found)
    int state;                    // 0 = not found yet, 1 = ok
    char diag[256];
    char box_cls[64];
    int n;
    Item items[kMaxItems];
    int64_t scans;
    // item table
    uint64_t dm_off1, dm_off2;    // DataManager field (and nested field) holding the IntegratedItem dictionary
    int dm_state;                 // 0 = not looked, 1 = found, -1 = looked and failed (retry later)
    uint64_t dm_retry_tick;
    char dm_diag[128];
    InfoCache cache;
    NameCache names;
    NameSearch search;
    uint64_t text_off;            // DataManager field holding TextDataDic (0 = not found yet)
    uint64_t text_retry_tick;
    char text_diag[96];
    uint64_t gs;                  // SaveData (game save)
    int ning;
    Ingredient ing[kMaxIngredients];
    int ing_state;                // 0 = not found, 1 = ok
    int nrec;
    Recipe rec[kMaxRecipes];
    int rec_state;
    char rec_diag[96];
    Row rows[kMaxRows];
    int nrows;
    int sorted;                   // 1 = rows ordered newest first by catch time
    int ncatch_times;
    uint64_t times_list[kMaxItems];   // LootBoxSlot.m_GetTimes (List<string>) per group
};
Instance g_inst;
// Samples since the module was loaded. Unlike Instance.tick it is not reset when the host creates
// the instance again (seen on device mid-dive): the discard job and the list search keep their
// deadlines in this clock, so a re-create can never leave them waiting for a tick far ahead.
uint64_t g_ticks;

// What the player chose on the bottom screen (kept across rescans; ids, not list positions).
int g_cargo_sort;                 // 0 newest (the game's order), 1 weight, 2 name, 3 quality (stars)
int g_ing_filter;                 // 0 all stacks, 1 only the ones in stock (0.37.23: the ALL button toggles)
int32_t g_ing_sel = -1;           // ingredient id whose recipes are shown (-1 = none)
int32_t g_rec_sel = -1;           // recipe id shown in the RECIPES panel (-1 = none)
constexpr int kMaxPins = 1;       // recipes pinned to the DIVE page (0.37.4: one; pinning another replaces it)
int32_t g_pins[kMaxPins] = {-1};
int g_pin_tgt[kMaxPins] = {5};   // servings each pinned recipe aims for
int g_pin_target = 5;             // the target the next pin starts with
int g_pin_sel = -1;               // pinned row tapped on DIVE: the UNPIN / OPEN prompt (-1 = closed)
uint64_t g_nav_sushi_until;       // OPEN: tell the pages to show SUSHI (a page bind on nav.sushi)
int PinSlot(int32_t id) { for (int p = 0; p < kMaxPins; ++p) if (id > 0 && g_pins[p] == id) return p; return -1; }
int PinCount() { int n = 0; for (int p = 0; p < kMaxPins; ++p) if (g_pins[p] > 0) ++n; return n; }

// Is `dict` a Dictionary`2 whose first live value's class is `value_cls`?
bool DictOf(const Guest& g, uint64_t dict, const char* value_cls, bool empty_ok) {
    char c[64];
    if (!dict || !g.cls(dict, c, sizeof c) || !starts(c, "Dictionary`2")) return false;
    uint64_t entries = g.ptr(dict + 0x18);
    if (!entries) return false;
    int32_t cnt = g.s32(dict + 0x20);
    if (cnt <= 0) return empty_ok;
    if (cnt > 100000) return false;
    for (int32_t i = 0; i < cnt && i < 64; ++i) {
        uint64_t e = entries + 0x20 + static_cast<uint64_t>(i) * 0x18;
        uint64_t v = g.ptr(e + 0x10);
        if (g.s32(e) < 0 || !v) continue;
        return g.cls(v, c, sizeof c) && eq(c, value_cls);
    }
    return empty_ok;   // every entry removed (a Dictionary keeps _count after Remove)
}

// Is `dict` a Dictionary`2 whose first live value is a LootBoxSlot?
bool LooksLikeBox(const Guest& g, uint64_t dict) {
    char c[64];
    if (!g.cls(dict, c, sizeof c) || !starts(c, "Dictionary`2")) return false;
    uint64_t entries = g.ptr(dict + 0x18);
    if (!entries) return false;
    int32_t cnt = g.s32(dict + 0x20);
    if (cnt <= 0) return true;   // empty box: the type already matched; accept
    if (cnt > 4096) return false;
    for (int32_t i = 0; i < cnt && i < 64; ++i) {
        uint64_t e = entries + 0x20 + static_cast<uint64_t>(i) * 0x18;
        int32_t hash = g.s32(e);
        uint64_t v = g.ptr(e + 0x10);
        if (hash < 0 || !v) continue;   // a removed entry has hashCode -1
        return g.cls(v, c, sizeof c) && eq(c, "LootBoxSlot");
    }
    return true;   // every entry removed (discarded): still the box, now empty
}

uint64_t DataManager(const Instance& in, const Guest& g) {
    uint64_t mi = g.ptr(in.main_base + kDataManagerMethodSlot);
    if (!mi || (mi & 1)) return 0;   // not initialised yet (an encoded token is odd)
    char c[64];
    const uint64_t koffs[2] = {0x20, 0x18};                    // MethodInfo.klass (v31: 0x20)
    for (uint64_t koff : koffs) {
        uint64_t k = g.ptr(mi + koff);
        if (!k || !g.cstr(g.ptr(k + 0x10), c, sizeof c) || !eq(c, "Singleton`1")) continue;
        uint64_t st = g.ptr(k + kStatics);
        uint64_t dm = st ? g.ptr(st) : 0;
        if (dm && g.cls(dm, c, sizeof c) && eq(c, "DataManager")) return dm;
    }
    return 0;
}

// Locate the Dictionary<?, IntegratedItem> under DataManager: a direct field, or one level deeper.
uint64_t ItemTable(Instance& in, const Guest& g) {
    uint64_t dm = DataManager(in, g);
    Buf d; d.clear();
    if (!dm) { d.add("no DataManager yet"); memcpy(in.dm_diag, d.s, static_cast<size_t>(d.n) + 1); return 0; }
    if (in.dm_state == 1) {
        uint64_t o = g.ptr(dm + in.dm_off1);
        uint64_t t = in.dm_off2 ? (o ? g.ptr(o + in.dm_off2) : 0) : o;
        if (DictOf(g, t, "IntegratedItem", false)) return t;
        in.dm_state = 0;
    }
    if (in.dm_state == -1 && in.tick < in.dm_retry_tick) return 0;
    for (uint64_t a = 0x10; a <= 0x800; a += 8) {
        uint64_t o = g.ptr(dm + a);
        if (!o) continue;
        if (DictOf(g, o, "IntegratedItem", false)) {
            in.dm_off1 = a; in.dm_off2 = 0; in.dm_state = 1;
            d.add("items at DataManager+"); d.hex(a); memcpy(in.dm_diag, d.s, static_cast<size_t>(d.n) + 1);
            return o;
        }
    }
    for (uint64_t a = 0x10; a <= 0x800; a += 8) {
        uint64_t o = g.ptr(dm + a);
        if (!o) continue;
        for (uint64_t b = 0x10; b <= 0x100; b += 8) {
            uint64_t t = g.ptr(o + b);
            if (t && DictOf(g, t, "IntegratedItem", false)) {
                in.dm_off1 = a; in.dm_off2 = b; in.dm_state = 1;
                d.add("items at DataManager+"); d.hex(a); d.add("+"); d.hex(b);
                memcpy(in.dm_diag, d.s, static_cast<size_t>(d.n) + 1);
                return t;
            }
        }
    }
    in.dm_state = -1; in.dm_retry_tick = in.tick + 300;   // try again in ~5 s
    d.add("item table not found under DataManager");
    memcpy(in.dm_diag, d.s, static_cast<size_t>(d.n) + 1);
    return 0;
}

uint64_t FindItem(Instance& in, const Guest& g, uint64_t table, int32_t id) {
    for (int i = 0; i < in.cache.n; ++i) if (in.cache.id[i] == id) return in.cache.obj[i];
    uint64_t entries = g.ptr(table + 0x18);
    int32_t cnt = g.s32(table + 0x20);
    if (!entries || cnt <= 0 || cnt > 100000) return 0;
    for (int32_t i = 0; i < cnt; ++i) {
        uint64_t e = entries + 0x20 + static_cast<uint64_t>(i) * 0x18;
        if (g.s32(e) < 0) continue;
        uint64_t v = g.ptr(e + 0x10);
        if (v && g.s32(v + 0x10) == id) {          // IntegratedItem.TID @0x10
            if (in.cache.n < 128) { in.cache.id[in.cache.n] = id; in.cache.obj[in.cache.n] = v; ++in.cache.n; }
            return v;
        }
    }
    return 0;
}

bool ends(const char* a, const char* suf) {
    int la = 0, ls = 0; while (a[la]) ++la; while (suf[ls]) ++ls;
    return la >= ls && eq(a + la - ls, suf);
}

// DataManager.TextDataDic: Dictionary<string, ITextBase>; every *TextData has name @0x10, english @0x18.
uint64_t TextDict(Instance& in, const Guest& g) {
    uint64_t dm = DataManager(in, g);
    if (!dm) return 0;
    char c[64];
    auto check = [&](uint64_t d) -> bool {
        if (!d || !g.cls(d, c, sizeof c) || !starts(c, "Dictionary`2")) return false;
        uint64_t entries = g.ptr(d + 0x18);
        int32_t cnt = g.s32(d + 0x20);
        if (!entries || cnt < 100 || cnt > 2000000) return false;
        for (int32_t i = 0; i < cnt && i < 32; ++i) {
            uint64_t e = entries + 0x20 + static_cast<uint64_t>(i) * 0x18;
            uint64_t k = g.ptr(e + 8), v = g.ptr(e + 0x10);
            if (g.s32(e) < 0 || !k || !v) continue;
            if (!g.cls(k, c, sizeof c) || !eq(c, "String")) return false;
            return g.cls(v, c, sizeof c) && ends(c, "TextData");
        }
        return false;
    };
    if (in.text_off && check(g.ptr(dm + in.text_off))) return g.ptr(dm + in.text_off);
    if (in.tick < in.text_retry_tick) return 0;
    for (uint64_t a = 0x10; a <= 0x800; a += 8) {
        uint64_t d = g.ptr(dm + a);
        if (check(d)) {
            in.text_off = a;
            Buf b; b.add("names at DataManager+"); b.hex(a); b.add(" entries="); b.num(g.s32(d + 0x20));
            memcpy(in.text_diag, b.s, static_cast<size_t>(b.n) + 1);
            return d;
        }
    }
    in.text_off = 0; in.text_retry_tick = in.tick + 300;
    const char* m = "name table not found yet";
    memcpy(in.text_diag, m, 25);
    return 0;
}

const char* CachedName(const Instance& in, int32_t id) {
    for (int i = 0; i < in.names.n; ++i) if (in.names.id[i] == id) return in.names.name[i];
    return nullptr;
}

void CacheName(Instance& in, int32_t id, const char* name) {
    int slot;
    if (in.names.n < kNames) slot = in.names.n++;
    else { slot = in.names.next; in.names.next = (in.names.next + 1) % kNames; }
    in.names.id[slot] = id;
    int i = 0; for (; name[i] && i < 47; ++i) in.names.name[slot][i] = name[i];
    in.names.name[slot][i] = 0;
}

// Walk up to `budget` entries of the text dictionary looking for the pending key.
void AdvanceSearch(Instance& in, const Guest& g, int budget) {
    NameSearch& s = in.search;
    if (!s.active) return;
    uint64_t d = TextDict(in, g);
    if (!d) return;
    uint64_t entries = g.ptr(d + 0x18);
    int32_t cnt = g.s32(d + 0x20);
    for (int k = 0; k < budget && s.cursor < cnt; ++k, ++s.cursor) {
        uint8_t e[0x18];
        if (!g.read(entries + 0x20 + static_cast<uint64_t>(s.cursor) * 0x18, e, sizeof e)) continue;
        int32_t hash; uint64_t key, val;
        memcpy(&hash, e, 4); memcpy(&key, e + 8, 8); memcpy(&val, e + 0x10, 8);
        if (hash < 0 || !key || !val) continue;
        if (g.s32(key + 0x10) != s.keylen) continue;
        uint16_t w[64];
        if (!g.read(key + 0x14, w, static_cast<size_t>(s.keylen) * 2)) continue;
        bool same = true;
        for (int i = 0; i < s.keylen && same; ++i) same = w[i] == static_cast<uint8_t>(s.key[i]);
        if (!same) continue;
        char name[48];
        if (g.str(g.ptr(val + 0x18), name, sizeof name) && name[0]) CacheName(in, s.id, name);   // english
        else CacheName(in, s.id, s.key);
        s.active = false;
        return;
    }
    if (s.cursor >= cnt) {   // not in the table: remember the key itself so we stop looking
        char t[48]; int i = 0;
        for (; s.key[i] && i < 47; ++i) t[i] = s.key[i] == '_' ? ' ' : s.key[i];
        t[i] = 0;
        CacheName(in, s.id, t);
        s.active = false;
    }
}

// Display name for an item: cached English name, else the text key (and queue a lookup).
void ResolveName(Instance& in, int32_t id, const char* text_id, char* out) {
    if (const char* nm = CachedName(in, id)) {
        int k = 0; for (; nm[k] && k < 47; ++k) out[k] = nm[k]; out[k] = 0;
        return;
    }
    int k = 0; for (; text_id[k] && k < 47; ++k) out[k] = text_id[k] == '_' ? ' ' : text_id[k];
    out[k] = 0;
    if (!in.search.active && text_id[0]) {
        in.search.active = true; in.search.id = id; in.search.cursor = 0;
        int l = 0; for (; text_id[l] && l < 63; ++l) in.search.key[l] = text_id[l];
        in.search.key[l] = 0; in.search.keylen = l;
    }
}

int32_t CarveCount(Instance& in, const Guest& g, int32_t item_id);
// 0.37.20: the SUSHI list's SORT: 0 MENU (tonight's menu first, then most servings), 1 PRICE, 2 AMOUNT, 3 NAME
int g_rec_sort;
int cmp(const char* a, const char* b);
void SortRecipes(Instance& in) {
    auto before = [](const Recipe& a, const Recipe& b2) {
        switch (g_rec_sort) {
        case 1: if (a.price != b2.price) return a.price > b2.price; break;
        case 2: if (a.can_make != b2.can_make) return a.can_make > b2.can_make; break;
        case 3: return cmp(a.name, b2.name) < 0;
        default: if (a.on_menu != b2.on_menu) return a.on_menu; break;
        }
        if (a.can_make != b2.can_make) return a.can_make > b2.can_make;
        return cmp(a.name, b2.name) < 0;
    };
    for (int i = 1; i < in.nrec; ++i) {
        Recipe t = in.rec[i]; int j = i - 1;
        while (j >= 0 && before(t, in.rec[j])) { in.rec[j + 1] = in.rec[j]; --j; }
        in.rec[j + 1] = t;
    }
}

void LookupInfo(Instance& in, const Guest& g) {
    uint64_t table = 0;
    for (int i = 0; i < in.n; ++i) {
        Item& it = in.items[i];
        it.has_info = false;
        if (!table) table = ItemTable(in, g);
        if (!table) return;
        uint64_t v = FindItem(in, g, table, it.id);
        if (!v) continue;
        g.str(g.ptr(v + 0x18), it.text_id, sizeof it.text_id);   // ItemTextID
        ResolveName(in, it.id, it.text_id, it.name);
        g.str(g.ptr(v + 0x38), it.icon, sizeof it.icon);         // ItemUIIcon
        it.rank = g.s32(v + 0x44);                               // ItemRank
        g.read(v + 0x48, &it.weight, 4);                         // ItemWeight
        it.sell = g.s32(v + 0x50);                               // ItemSellPrice
        it.itype = g.s32(v + 0x74);                              // IntegratedType
        it.has_info = true;
        it.carve = CarveCount(in, g, it.id);
    }
}

int cmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}
int len(const char* a) { int n = 0; while (a[n]) ++n; return n; }

// One row per catch (a group of 2 is two rows), ordered newest first by the catch time strings
// (LootBoxSlot.m_GetTimes, one per catch) when they are all the same fixed-width format.
constexpr int32_t kIngKey = 1000000000, kRecKey = 2000000000;   // name-cache namespaces

// DataManager dictionaries pinned by round 8/9 (each checked by its value class before use).
constexpr uint64_t kDmFishInfo = 0x178, kDmIngredients = 0x1B8, kDmRecipes = 0x2F0;

uint64_t DmDict(Instance& in, const Guest& g, uint64_t off, const char* value_cls) {
    uint64_t dm = DataManager(in, g);
    uint64_t d = dm ? g.ptr(dm + off) : 0;
    return DictOf(g, d, value_cls, false) ? d : 0;
}

// Linear lookup of an int-keyed dictionary entry (key @+8, value @+0x10). Small tables only.
uint64_t DictFindInt(const Guest& g, uint64_t d, int32_t key) {
    uint64_t entries = g.ptr(d + 0x18);
    int32_t cnt = g.s32(d + 0x20);
    if (!entries || cnt <= 0 || cnt > 100000) return 0;
    for (int32_t i = 0; i < cnt; ++i) {
        uint8_t e[0x18];
        if (!g.read(entries + 0x20 + static_cast<uint64_t>(i) * 0x18, e, sizeof e)) continue;
        int32_t hash, k; uint64_t v;
        memcpy(&hash, e, 4); memcpy(&k, e + 8, 4); memcpy(&v, e + 0x10, 8);
        if (hash >= 0 && k == key && v) return v;
    }
    return 0;
}

char g_noic[96]; int g_noic_n; extern char g_pfix_diag[128];
char g_ingdiag[128];
int g_ing_hits, g_ing_miss;

// DataManager.IngredientsDataDic: pinned at +0x1B8; if that slot does not hold it, find it by type.
uint64_t IngredientTable(Instance& in, const Guest& g) {
    uint64_t d = DmDict(in, g, kDmIngredients, "IngredientsEntity");
    if (d) return d;
    uint64_t dm = DataManager(in, g);
    char c[64];
    if (dm) {
        for (uint64_t a = 0x10; a <= 0x800; a += 8) {
            uint64_t t = g.ptr(dm + a);
            if (t && g.cls(t, c, sizeof c) && starts(c, "Dictionary`2")) {
                uint64_t en = g.ptr(t + 0x18); int32_t cnt = g.s32(t + 0x20);
                for (int32_t i = 0; en && i < cnt && i < 8; ++i) {
                    uint64_t v = g.ptr(en + 0x20 + static_cast<uint64_t>(i) * 0x18 + 0x10);
                    if (v && g.cls(v, c, sizeof c)) { if (starts(c, "Ingredients")) return t; break; }
                }
            }
        }
    }
    return 0;
}

// Ingredient display name: Ingredients.NameID (@0x18) of DataManager.IngredientsDataDic[id];
// if that key has no text, the linked item's name (IngredientsEntity.ItemsTID @0x54).
void IngredientName(Instance& in, const Guest& g, int32_t id, char* out) {
    Buf b; b.add("ingredient ").num(id);
    memcpy(out, b.s, static_cast<size_t>(b.n) + 1);
    uint64_t d = IngredientTable(in, g);
    uint64_t v = d ? DictFindInt(g, d, id) : 0;
    {
        Buf db; char c[64] = "-";
        if (d) {   // describe the table: value class, count, first key
            uint64_t en = g.ptr(d + 0x18);
            uint64_t v0 = en ? g.ptr(en + 0x20 + 0x10) : 0;
            if (v0) g.cls(v0, c, sizeof c);
            db.add("ingredient table ").add(c).add(" n=").num(g.s32(d + 0x20)).add(" key0=").num(en ? g.s32(en + 0x20 + 8) : 0);
        } else {
            db.add("ingredient table not found");
        }
        if (v) ++g_ing_hits; else ++g_ing_miss;
        db.add(" hit=").num(g_ing_hits).add(" miss=").num(g_ing_miss);
        if (g_pfix_diag[0]) db.add(" | ").add(g_pfix_diag);
        memcpy(g_ingdiag, db.s, static_cast<size_t>(db.n) + 1);
    }
    if (!v) return;
    char key[64];
    if (g.str(g.ptr(v + 0x18), key, sizeof key) && key[0]) { ResolveName(in, kIngKey + id, key, out); return; }
    int32_t item_id = g.s32(v + 0x54);                   // IngredientsEntity.ItemsTID
    uint64_t table = ItemTable(in, g);
    uint64_t it = (table && item_id > 0) ? FindItem(in, g, table, item_id) : 0;
    if (it && g.str(g.ptr(it + 0x18), key, sizeof key) && key[0]) ResolveName(in, item_id, key, out);
}

// Ingredient picture: the linked item's ItemUIIcon (IngredientsEntity.ItemsTID @0x54), else Ingredients.ContentsThumbnail (@0x48).
void DcCopy(char* dst, int cap, const char* src);
void IngredientIconLookup(Instance& in, const Guest& g, int32_t id, char* out, int cap) {
    out[0] = 0;
    uint64_t d = IngredientTable(in, g);
    uint64_t v = d ? DictFindInt(g, d, id) : 0;
    if (!v) return;
    // the item's own picture first (for a fish, ContentsThumbnail names something else, e.g. a
    // bait picture), the ingredient's ContentsThumbnail only when it has no item
    int32_t item_id = g.s32(v + 0x54);
    uint64_t table = ItemTable(in, g);
    uint64_t it = (table && item_id > 0) ? FindItem(in, g, table, item_id) : 0;
    if (it && g.str(g.ptr(it + 0x38), out, cap) && out[0]) return;
    g.str(g.ptr(v + 0x48), out, cap);
}
// The rank the game's storage shows: the linked item's rank (IntegratedItem +0x44, as on CARGO).
int32_t IngredientItemRank(Instance& in, const Guest& g, int32_t id) {
    uint64_t d = IngredientTable(in, g);
    uint64_t v = d ? DictFindInt(g, d, id) : 0;
    int32_t item_id = v ? g.s32(v + 0x54) : 0;
    uint64_t table = ItemTable(in, g);
    uint64_t it = (table && item_id > 0) ? FindItem(in, g, table, item_id) : 0;
    return it ? g.s32(it + 0x44) : 0;
}
constexpr int kIconCache = 256;
struct IconCache { int32_t id[kIconCache]; char icon[kIconCache][64]; int32_t rank[kIconCache]; int32_t item[kIconCache]; uint64_t again[kIconCache]; int n; };
IconCache g_ing_icons;
int32_t IngredientItemLookup(Instance& in, const Guest& g, int32_t id) {
    uint64_t d = IngredientTable(in, g);
    uint64_t v = d ? DictFindInt(g, d, id) : 0;
    return v ? g.s32(v + 0x54) : 0;                       // IngredientsEntity.ItemsTID
}
bool FishIconFor(Instance& in, const Guest& g, int32_t id, char* out, int cap);
void IngredientIcon(Instance& in, const Guest& g, int32_t id, char* out, int cap, int32_t* rank = nullptr, int32_t* item = nullptr) {
    int redo = -1;
    for (int k = 0; k < g_ing_icons.n; ++k)   // looked up once per id (the tables are large)
        if (g_ing_icons.id[k] == id) {
            // 0.37.57: a bait picture (the fish not found yet) is looked up again every few seconds
            if (g_ing_icons.again[k] && g_ticks >= g_ing_icons.again[k]) { redo = k; break; }
            DcCopy(out, cap, g_ing_icons.icon[k]);
            if (rank) *rank = g_ing_icons.rank[k];
            if (item) *item = g_ing_icons.item[k];
            return;
        }
    IngredientIconLookup(in, g, id, out, cap);
    // 0.37.24: a catch whose ingredient names no picture: the fish that gives it (FishInfoData.ItemIcon)
    if (!out[0] || starts(out, "Bait")) {
        char f[64] = "";
        if (FishIconFor(in, g, id, f, sizeof f)) DcCopy(out, cap, f);
        else if (!out[0]) { ++g_noic_n; Buf b; b.add("no picture: ").num(id); DcCopy(g_noic, sizeof g_noic, b.s); }
    }
    const int32_t r = IngredientItemRank(in, g, id), it = IngredientItemLookup(in, g, id);
    if (rank) *rank = r;
    if (item) *item = it;
    // 0.37.57: a picture that is still the bait one (the fish's not found: its tables may not be loaded yet, which
    // used to be kept for good -- Starry Puffer, Clearfin Lionfish blank) is kept but asked again in 5 s
    const uint64_t again = starts(out, "Bait") ? g_ticks + 300 : 0;
    if (redo >= 0) {
        DcCopy(g_ing_icons.icon[redo], 64, out); g_ing_icons.rank[redo] = r; g_ing_icons.item[redo] = it; g_ing_icons.again[redo] = again;
    } else if (g_ing_icons.n < kIconCache && out[0]) {   // 0.37.23: an empty answer is asked again later
        g_ing_icons.id[g_ing_icons.n] = id; DcCopy(g_ing_icons.icon[g_ing_icons.n], 64, out); g_ing_icons.rank[g_ing_icons.n] = r;
        g_ing_icons.item[g_ing_icons.n] = it; g_ing_icons.again[g_ing_icons.n] = again; ++g_ing_icons.n;
    }
}

// Unlocked recipes (SaveData.m_UnlockRecipeData +0x2A8) with their ingredient needs
// (RecipeEntity.IngredientsList +0x68: List<{int id; int quantity; string icon}>, 0x10 per element),
// matched against the storage scanned just before. Tonight's menu: SaveData.m_SushiBarMenuData +0x298.
void ScanRecipes(Instance& in, const Guest& g) {
    in.nrec = 0; in.rec_state = 0;
    Buf b;
    uint64_t un = in.gs ? g.ptr(in.gs + 0x2A8) : 0;
    uint64_t recipes = DmDict(in, g, kDmRecipes, "RecipeEntity");
    if (!DictOf(g, un, "UnlockRecipeSave", true) || !recipes) {
        b.add(recipes ? "no unlocked-recipe list" : "recipe table not found");
        memcpy(in.rec_diag, b.s, static_cast<size_t>(b.n) + 1);
        return;
    }
    // menu recipe ids
    int32_t menu[16]; int nmenu = 0;
    uint64_t md = in.gs ? g.ptr(in.gs + 0x298) : 0;
    if (DictOf(g, md, "SushibarMenuSlotSave", true)) {
        uint64_t me = g.ptr(md + 0x18); int32_t mc = g.s32(md + 0x20);
        for (int32_t i = 0; i < mc && i < 64 && nmenu < 16; ++i) {
            uint64_t e = me + 0x20 + static_cast<uint64_t>(i) * 0x18, v = g.ptr(e + 0x10);
            int32_t rid;
            if (g.s32(e) >= 0 && v && g.obscured(v + 0x24, &rid) && rid > 0) menu[nmenu++] = rid;
        }
    }
    uint64_t ue = g.ptr(un + 0x18); int32_t uc = g.s32(un + 0x20);
    if (!ue || uc < 0 || uc > 4096) return;
    int n = 0;
    for (int32_t i = 0; i < uc && n < kMaxRecipes; ++i) {
        uint64_t e = ue + 0x20 + static_cast<uint64_t>(i) * 0x18, v = g.ptr(e + 0x10);
        int32_t rid;
        if (g.s32(e) < 0 || !v || !g.obscured(v + 0x10, &rid)) continue;
        uint64_t re = DictFindInt(g, recipes, rid);
        if (!re) continue;
        Recipe& r = in.rec[n];
        r.id = rid;
        Buf nb; nb.add("recipe ").num(rid); memcpy(r.name, nb.s, static_cast<size_t>(nb.n) + 1);
        char key[64];
        if (g.str(g.ptr(re + 0x18), key, sizeof key) && key[0]) ResolveName(in, kRecKey + rid, key, r.name);
        g.str(g.ptr(re + 0x28), r.icon, sizeof r.icon);
        r.price = g.s32(re + 0x44);
        if (r.price < 0 || r.price > 1000000) r.price = 0;
        r.on_menu = false;
        for (int k = 0; k < nmenu; ++k) if (menu[k] == rid) r.on_menu = true;
        r.nneed = 0; r.can_make = 0x7FFFFFFF;
        uint64_t list = g.ptr(re + 0x68);
        uint64_t arr = list ? g.ptr(list + 0x10) : 0;
        int32_t ln = list ? g.s32(list + 0x18) : 0;
        for (int32_t k = 0; k < ln && r.nneed < kMaxNeeds && arr; ++k) {
            int32_t idq[2];
            if (!g.read(arr + 0x20 + static_cast<uint64_t>(k) * 0x10, idq, 8) || idq[0] <= 0) continue;
            int j = r.nneed++;
            // the element's own icon string (+8), else the ingredient's picture; and its item id
            {
                char tmp[64]; int32_t item = 0;
                IngredientIcon(in, g, idq[0], tmp, sizeof tmp, nullptr, &item);
                r.need_item[j] = item;
                if (!g.str(g.ptr(arr + 0x20 + static_cast<uint64_t>(k) * 0x10 + 8), r.need_icon[j], sizeof r.need_icon[j]) || !r.need_icon[j][0])
                    DcCopy(r.need_icon[j], sizeof r.need_icon[j], tmp);
            }
            r.need_id[j] = idq[0]; r.need_qty[j] = idq[1] > 0 ? idq[1] : 1;
            r.have[j] = 0;
            for (int s2 = 0; s2 < in.ning; ++s2)   // a recipe names the base ingredient; graded stacks have it as parent
                if (in.ing[s2].id == idq[0] || in.ing[s2].parent == idq[0]) r.have[j] += in.ing[s2].count;
            IngredientName(in, g, idq[0], r.need_name[j]);
            int32_t can = r.have[j] / r.need_qty[j];
            if (can < r.can_make) r.can_make = can;
        }
        if (r.nneed == 0) r.can_make = 0;
        ++n;
    }
    in.nrec = n; in.rec_state = 1;
    SortRecipes(in);
    b.add("recipes unlocked="); b.num(uc); b.add(" menu="); b.num(nmenu);
    memcpy(in.rec_diag, b.s, static_cast<size_t>(b.n) + 1);
}

// "x2" in the Acquired list: CarvableCount of the fish whose DropItemID is this item (probe).
int32_t CarveCount(Instance& in, const Guest& g, int32_t item_id) {
    uint64_t d = DmDict(in, g, kDmFishInfo, "FishInfoDataEntity");
    if (!d) return 0;
    uint64_t entries = g.ptr(d + 0x18);
    int32_t cnt = g.s32(d + 0x20);
    for (int32_t i = 0; i < cnt && i < 20000; ++i) {
        uint64_t e = entries + 0x20 + static_cast<uint64_t>(i) * 0x18, v = g.ptr(e + 0x10);
        if (g.s32(e) < 0 || !v) continue;
        if (g.s32(v + 0x7C) == item_id) return g.s32(v + 0x78);
    }
    return 0;
}

// Sushi bar ingredient storage: SaveData.m_IngredientsData (Dictionary<int, IngredientsSave>, +0x120).
// IngredientsSave: m_IngredientsID 0x10, m_Level 0x24, m_Count 0x4C (ObscuredInt).
void ScanIngredients(Instance& in, const Guest& g) {
    in.ning = 0; in.ing_state = 0;
    uint64_t d = in.gs ? g.ptr(in.gs + 0x120) : 0;
    if (!DictOf(g, d, "IngredientsSave", true)) return;
    uint64_t entries = g.ptr(d + 0x18);
    int32_t cnt = g.s32(d + 0x20);
    if (!entries || cnt < 0 || cnt > 4096) return;
    uint64_t table = ItemTable(in, g);
    int n = 0;
    for (int32_t i = 0; i < cnt && n < kMaxIngredients; ++i) {
        uint64_t e = entries + 0x20 + static_cast<uint64_t>(i) * 0x18;
        uint64_t v = g.ptr(e + 0x10);
        if (g.s32(e) < 0 || !v) continue;
        Ingredient& x = in.ing[n];
        if (!g.obscured(v + 0x10, &x.id) || !g.obscured(v + 0x4C, &x.count)) continue;
        g.obscured(v + 0x24, &x.level);
        g.obscured(v + 0x38, &x.parent);
        if (x.count < 0) x.count = 0;        // used up: the game still lists it, as x0
        x.has_info = false; x.rank = 0;
        IngredientName(in, g, x.id, x.name);
        { int32_t r = 0; IngredientIcon(in, g, x.id, x.icon, sizeof x.icon, &r); if (r > 0) x.rank = r; }
        if (!x.icon[0] && x.parent > 0) IngredientIcon(in, g, x.parent, x.icon, sizeof x.icon);
        if (x.name[0] == 'i' && starts(x.name, "ingredient ")) {   // not an ingredient-table id: try the item table
            uint64_t it = table ? FindItem(in, g, table, x.id) : 0;
            if (it) {
                char tid[64];
                g.str(g.ptr(it + 0x18), tid, sizeof tid);
                ResolveName(in, x.id, tid, x.name);
                x.rank = g.s32(it + 0x44);
                x.has_info = true;
                if (!x.icon[0]) g.str(g.ptr(it + 0x38), x.icon, sizeof x.icon);
            }
        }
        ++n;
    }
    // most first, like a storage list sorted by amount (the stable sort keeps empty stacks last, in save order)
    for (int i = 1; i < n; ++i) {
        Ingredient t = in.ing[i]; int j = i - 1;
        while (j >= 0 && in.ing[j].count < t.count) { in.ing[j + 1] = in.ing[j]; --j; }
        in.ing[j + 1] = t;
    }
    in.ning = n; in.ing_state = 1;
    // 0.37.56: the other picture names for up to two ingredients that asked (IngPicture)
    for (int w = 0; w < 2 && g_nwant > 0 && g_nalt < 64; ++w) {
        const int32_t id = g_alt_want[--g_nwant];
        AltIcon& a = g_alt[g_nalt++];
        a.id = id; a.item[0] = 0; a.fish[0] = 0; a.fish_id = 0;
        if (const uint64_t it = table ? FindItem(in, g, table, id) : 0) g.str(g.ptr(it + 0x38), a.item, sizeof a.item);
        if (const uint64_t fd = DmDict(in, g, kDmFishInfo, "FishInfoDataEntity")) {
            const uint64_t fe = g.ptr(fd + 0x18); const int32_t fc = g.s32(fd + 0x20);
            for (int32_t i = 0; fe && i < fc && i < 20000; ++i) {
                const uint64_t e = fe + 0x20 + static_cast<uint64_t>(i) * 0x18, v = g.ptr(e + 0x10);
                if (g.s32(e) < 0 || !v || g.s32(v + 0x7C) != id) continue;
                a.fish_id = g.s32(e + 8);
                break;
            }
            if (a.fish_id > 0) if (const uint64_t it = table ? FindItem(in, g, table, a.fish_id) : 0) g.str(g.ptr(it + 0x38), a.fish, sizeof a.fish);
        }
        a.done = 1;
    }
}

// "MM/DD/YYYY HH:MM:SS" -> "YYYYMMDDHHMMSS" so catches sort correctly across months and years.
void TimeKey(const char* t, char* out) {
    bool us = len(t) == 19 && t[2] == '/' && t[5] == '/' && t[10] == ' ' && t[13] == ':' && t[16] == ':';
    if (!us) { int i = 0; for (; t[i] && i < 31; ++i) out[i] = t[i]; out[i] = 0; return; }
    const int idx[14] = {6, 7, 8, 9, 0, 1, 3, 4, 11, 12, 14, 15, 17, 18};
    for (int i = 0; i < 14; ++i) out[i] = t[idx[i]];
    out[14] = 0;
}

void SortRows(Instance& in);
void BuildRows(Instance& in, const Guest& g) {
    int r = 0;
    bool same_format = true;
    int fmt_len = -1;
    for (int i = 0; i < in.n && r < kMaxRows; ++i) {
        const Item& it = in.items[i];
        uint64_t list = in.times_list[i];
        uint64_t arr = list ? g.ptr(list + 0x10) : 0;
        int32_t size = list ? g.s32(list + 0x18) : 0;
        if (it.lift == 2) continue;   // 0.38.41: lifted to the boat by the drone (DIAG on device: lift 2) -- not in the game's Acquired list
        for (int c = 0; c < (it.count > 0 ? it.count : 1) && r < kMaxRows; ++c, ++r) {
            Row& row = in.rows[r];
            row.item = i;
            row.time[0] = 0;
            if (arr && c < size) g.str(g.ptr(arr + 0x20 + static_cast<uint64_t>(c) * 8), row.time, sizeof row.time);
            TimeKey(row.time, row.key);
            int l = len(row.key);
            if (l == 0 || (row.key[0] < '0' || row.key[0] > '9')) same_format = false;
            if (fmt_len < 0) fmt_len = l; else if (l != fmt_len) same_format = false;
        }
    }
    in.nrows = r;
    in.sorted = 0;
    if (same_format && r > 1) {   // insertion sort, newest (largest) first
        for (int i = 1; i < r; ++i) {
            Row t = in.rows[i]; int j = i - 1;
            while (j >= 0 && cmp(in.rows[j].key, t.key) < 0) { in.rows[j + 1] = in.rows[j]; --j; }
            in.rows[j + 1] = t;
        }
        in.sorted = 1;
    }
    for (int i = 0; i < r; ++i) in.rows[i].pos = i;
    SortRows(in);
}

// The player's sort over the rows (stable: ties keep the game's newest-first order). Starts from
// the game's order each time, so switching back to NEWEST restores it without a rescan.
void SortRows(Instance& in) {
    const int r = in.nrows;
    for (int i = 1; i < r; ++i) {
        Row t = in.rows[i]; int j = i - 1;
        while (j >= 0 && in.rows[j].pos > t.pos) { in.rows[j + 1] = in.rows[j]; --j; }
        in.rows[j + 1] = t;
    }
    if (g_cargo_sort > 0 && r > 1) {
        auto before = [&](const Row& a, const Row& b) {
            const Item& x = in.items[a.item]; const Item& y = in.items[b.item];
            if (g_cargo_sort == 1) return x.weight > y.weight;
            if (g_cargo_sort == 2) return cmp(x.name, y.name) < 0;
            if (x.grade != y.grade) return x.grade > y.grade;   // quality: stars, then the heavier
            return x.weight > y.weight;
        };
        for (int i = 1; i < r; ++i) {
            Row t = in.rows[i]; int j = i - 1;
            while (j >= 0 && before(t, in.rows[j])) { in.rows[j + 1] = in.rows[j]; --j; }
            in.rows[j + 1] = t;
        }
    }
}

void Scan(Instance& in, const Guest& g) {
    Buf d; d.clear();
    in.n = 0;
    uint64_t ti = g.ptr(in.main_base + kSaveSystemTypeInfo);
    uint64_t single = ti ? g.ptr(ti + kParent) : 0;
    uint64_t statics = single ? g.ptr(single + kStatics) : 0;
    uint64_t ss = statics ? g.ptr(statics) : 0;
    char c[64];
    if (!ss || !g.cls(ss, c, sizeof c) || !eq(c, "SaveSystem")) {
        d.add("no SaveSystem yet"); memcpy(in.diag, d.s, static_cast<size_t>(d.n) + 1); in.state = 0; return;
    }
    uint64_t gdm = g.ptr(ss + kGameDataManager);
    if (!gdm) { d.add("no game data manager"); memcpy(in.diag, d.s, static_cast<size_t>(d.n) + 1); in.state = 0; return; }

    uint64_t box = 0;
    if (in.gs_off < 0) in.gs_off = static_cast<int64_t>(kGameSaveField);
    if (in.gs_off >= 0) {
        uint64_t gs = g.ptr(gdm + static_cast<uint64_t>(in.gs_off));
        uint64_t b = gs ? g.ptr(gs + kBoxField) : 0;
        if (b && LooksLikeBox(g, b)) box = b; else in.gs_off = -1;
    }
    if (!box) {  // find the game-save field by type (first build; the offset gets pinned later)
        for (uint64_t off = 0x10; off <= 0x200 && !box; off += 8) {
            uint64_t gs = g.ptr(gdm + off);
            if (!gs) continue;
            uint64_t b = g.ptr(gs + kBoxField);
            if (b && LooksLikeBox(g, b)) { box = b; in.gs_off = static_cast<int64_t>(off); }
        }
    }
    if (!box) {
        in.n = 0; in.nrows = 0;   // never keep showing a stale list
        d.add("box not found; gdm cls=");
        if (g.cls(gdm, c, sizeof c)) d.add(c);
        // No cargo box before the first dive of a session -- but the save itself is there, and the
        // sushi pages need it: the game save at its pinned field, checked by class.
        uint64_t gs = g.ptr(gdm + kGameSaveField);
        if (gs && ((g.cls(gs, c, sizeof c) && eq(c, "SaveData")) || DictOf(g, g.ptr(gs + 0x2A8), "UnlockRecipeSave", true))) {
            in.gs = gs;
            ScanIngredients(in, g);
            if (in.scans % 4 == 0) ScanRecipes(in, g);
            ++in.scans;
            d.add("; save found, sushi data read");
        }
        memcpy(in.diag, d.s, static_cast<size_t>(d.n) + 1); in.state = 0; return;
    }
    g.cls(box, in.box_cls, sizeof in.box_cls);
    in.gs = g.ptr(gdm + static_cast<uint64_t>(in.gs_off));

    // walk the dictionary twice-safe: read version before and after
    int32_t ver0 = g.s32(box + 0x2C);
    uint64_t entries = g.ptr(box + 0x18);
    int32_t cnt = g.s32(box + 0x20);
    int64_t cap = static_cast<int64_t>(g.ptr(entries + 0x18));
    if (cnt < 0 || cnt > 4096 || cnt > cap) { d.add("bad count"); memcpy(in.diag, d.s, static_cast<size_t>(d.n) + 1); in.state = 0; return; }
    int n = 0;
    for (int32_t i = 0; i < cnt && n < kMaxItems; ++i) {
        uint64_t e = entries + 0x20 + static_cast<uint64_t>(i) * 0x18;
        int32_t hash = g.s32(e);
        uint64_t k = g.ptr(e + 8), v = g.ptr(e + 0x10);
        if (hash < 0 || !v) continue;
        Item& it = in.items[n];
        g.str(k, it.key, sizeof it.key);
        bool ok = g.obscured(v + 0x10, &it.id) && g.obscured(v + 0x28, &it.grade) &&
                  g.obscured(v + 0x3C, &it.fgrade) && g.obscured(v + 0x50, &it.count);
        it.type = g.s32(v + 0x24);
        it.lift = g.s32(v + 0x70);
        if (ok) { in.times_list[n] = g.ptr(v + 0x78); ++n; }
    }
    int32_t ver1 = g.s32(box + 0x2C);
    if (ver0 != ver1) { return; }   // changed under us: keep the last good list, try again next scan
    in.n = n;
    in.state = 1;
    LookupInfo(in, g);
    ScanIngredients(in, g);
    if (in.scans % 4 == 0) ScanRecipes(in, g);   // recipes change rarely: once a second
    BuildRows(in, g);
    d.add("ok gs_off="); d.hex(static_cast<uint64_t>(in.gs_off)); d.add(" entries="); d.num(cnt);
    memcpy(in.diag, d.s, static_cast<size_t>(d.n) + 1);
    ++in.scans;
}

// The recipes that use one ingredient (tap a box on INGREDIENTS). A recipe names the base
// ingredient; a graded stack carries it as its parent, so both ids match.
constexpr int kSelRecipes = 8;
const char* IngPicture(const Instance& in, const EdenDsmodHostApi* h, const Ingredient& x);
void PublishIngredientDetail(const Instance& in, const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    const Ingredient* x = nullptr;
    for (int i = 0; i < in.ning && g_ing_sel >= 0; ++i) if (in.ing[i].id == g_ing_sel) { x = &in.ing[i]; break; }
    h->publish_i64(u, "ingsel.on", x ? 1 : 0);
    if (!x) return;
    Buf t, v;
    h->publish_text(u, "ingsel.name", NoApos(x->name, t));
    { const char* pic = IngPicture(in, h, *x); if (pic[0]) v.add("module:icon/").add(pic); }   // 0.37.28: the same picture as its box
    h->publish_text(u, "ingsel.isrc", v.s);
    h->publish_i64(u, "ingsel.count", x->count);
    h->publish_i64(u, "ingsel.rk", x->rank > 0 ? x->rank : x->level);
    int n = 0;
    for (int i = 0; i < in.nrec && n < kSelRecipes; ++i) {
        const Recipe& r = in.rec[i];
        bool uses = false;
        for (int k = 0; k < r.nneed; ++k)
            if (r.need_id[k] == x->id || (x->parent > 0 && r.need_id[k] == x->parent)) uses = true;
        if (!uses) continue;
        Buf name, a, b;
        name.add("ingsel.rname").num(n); h->publish_text(u, name.s, NoApos(r.name, a));
        if (r.icon[0]) b.add("module:icon/").add(r.icon);
        name.clear(); name.add("ingsel.risrc").num(n); h->publish_text(u, name.s, b.s);
        name.clear(); name.add("ingsel.rcan").num(n); h->publish_i64(u, name.s, r.can_make);
        name.clear(); name.add("ingsel.rcando").num(n); h->publish_i64(u, name.s, r.can_make > 0 ? 1 : 0);
        name.clear(); name.add("ingsel.rcannot").num(n); h->publish_i64(u, name.s, r.can_make > 0 ? 0 : 1);
        name.clear(); name.add("ingsel.ron").num(n); h->publish_i64(u, name.s, 1);
        ++n;
    }
    h->publish_i64(u, "ingsel.n", n);
    h->publish_i64(u, "ingsel.none", n == 0 ? 1 : 0);
}

// Portions a dive brings home for one ingredient: the catches in the cargo whose item is that
// ingredient's item, times the portions each gives (the game's "x2": FishInfoData.CarvableCount).
int32_t CargoPortions(const Instance& in, int32_t item) {
    if (item <= 0) return 0;
    int32_t n = 0;
    for (int i = 0; i < in.n; ++i)
        if (in.items[i].id == item) n += (in.items[i].count > 0 ? in.items[i].count : 1) * (in.items[i].carve > 0 ? in.items[i].carve : 1);
    return n;
}

const Recipe* FindRecipe(const Instance& in, int32_t id) {
    for (int i = 0; i < in.nrec && id > 0; ++i) if (in.rec[i].id == id) return &in.rec[i];
    return nullptr;
}

// The pinned recipes on DIVE (up to 3): per ingredient what the restaurant has in storage, what this
// dive carries ("+ 2"), and the total against what the target servings need ("Blue Tang 2 + 1 / 5").
int EcoPinGoals(int32_t* id, int32_t* have, int32_t* need, const char** nm, const char** ic, int cap);   // 0.37.73
constexpr int kPlEco = 4;
// 0.37.82: EcoWatcher goals on the DIVE map (pickup and fish marks) are on the back burner (user): the pin feeds only
// the HUNT list. true brings the 0.37.77..0.37.81 marks back as they were.
constexpr bool kEcoMapMarks = false;
int g_pl_pin[8]; int32_t g_pl_id[8]; char g_pl_ic[8][64]; int g_pl_n;   // 0.37.7: also kept for the fish marks   // 0.37.2: the pin each line of the map page's ingredient list comes from (first one)
void PublishPinnedRecipe(const Instance& in, const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    int n = 0;
    for (int p = 0; p < kMaxPins; ++p) {
        const Recipe* r = FindRecipe(in, g_pins[p]);
        if (!r) continue;
        const int tgt = g_pin_tgt[p];
        Buf pre; pre.add("pin").num(n).add(".");
        auto key = [&](const char* f, int k = -1) { static Buf b; b.clear(); b.add(pre.s).add(f); if (k >= 0) b.num(k); return b.s; };
        Buf t, v;
        h->publish_i64(u, key("on"), 1);
        h->publish_text(u, key("name"), NoApos(r->name, t));
        if (r->icon[0]) v.add("module:icon/").add(r->icon);
        h->publish_text(u, key("isrc"), v.s);
        h->publish_i64(u, key("target"), tgt);
        int32_t can_all = r->nneed ? 0x7FFFFFFF : 0;
        for (int k = 0; k < 4; ++k) {
            const bool on = k < r->nneed;
            const int32_t store = on ? r->have[k] : 0, dive = on ? CargoPortions(in, r->need_item[k]) : 0;
            const int32_t q = on && r->need_qty[k] > 0 ? r->need_qty[k] : 1, need = on ? r->need_qty[k] * tgt : 0;
            if (on && (store + dive) / q < can_all) can_all = (store + dive) / q;
            Buf a, b;
            h->publish_i64(u, key("on", k), on ? 1 : 0);
            if (!on) continue;
            h->publish_i64(u, key("have", k), store + dive);
            h->publish_i64(u, key("need", k), need);
            h->publish_i64(u, key("ok", k), store + dive >= need ? 1 : 0);
            h->publish_i64(u, key("short", k), store + dive >= need ? 0 : 1);
            h->publish_i64(u, key("dive", k), dive);
            h->publish_i64(u, key("hasdive", k), dive > 0 ? 1 : 0);
            h->publish_text(u, key("nn", k), NoApos(r->need_name[k], a));
            if (r->need_icon[k][0]) b.add("module:icon/").add(r->need_icon[k]);
            h->publish_text(u, key("nisrc", k), b.s);
        }
        h->publish_i64(u, key("canall"), can_all);
        h->publish_i64(u, key("done"), can_all >= tgt ? 1 : 0);
        h->publish_i64(u, key("notdone"), can_all >= tgt ? 0 : 1);
        ++n;
    }
    for (int p = n; p < 2; ++p) {                    // the empty rows (2: the package's pin0/pin1): nothing of an old pin stays shown
        Buf b; b.add("pin").num(p).add(".on"); h->publish_i64(u, b.s, 0);
        for (int k = 0; k < 4; ++k) { Buf c; c.add("pin").num(p).add(".on").num(k); h->publish_i64(u, c.s, 0); }
        static const char* const kF[2] = {"done", "notdone"};
        for (int f = 0; f < 2; ++f) { Buf c; c.add("pin").num(p).add(".").add(kF[f]); h->publish_i64(u, c.s, 0); }
    }
    h->publish_i64(u, "pin.n", n);
    h->publish_i64(u, "pin.on", n > 0 ? 1 : 0);
    { int32_t a[1], b[1], c[1]; const char* d[1]; const char* e[1];
      h->publish_i64(u, "hunt.on", n > 0 || EcoPinGoals(a, b, c, d, e, 1) > 0 ? 1 : 0); }   // 0.37.73: a recipe or a mission pinned
    // 0.37.1: one list of the pinned recipes' ingredients for the map page (an ingredient two pinned
    // recipes share is one line: their needs added up)
    {
        constexpr int kPl = 8;
        int32_t id[kPl], have[kPl], need[kPl]; const char* nm[kPl]; const char* ic[kPl]; int m = 0;
        for (int p = 0; p < kMaxPins; ++p) {
            const Recipe* r = FindRecipe(in, g_pins[p]);
            if (!r) continue;
            for (int k = 0; k < r->nneed && k < 4; ++k) {
                const int32_t nd = r->need_qty[k] * g_pin_tgt[p];
                int at = -1;
                for (int j = 0; j < m; ++j) if (id[j] == r->need_item[k]) at = j;
                if (at >= 0) { need[at] += nd; continue; }
                if (m >= kPl) continue;
                g_pl_pin[m] = p;
                id[m] = r->need_item[k]; have[m] = r->have[k] + CargoPortions(in, r->need_item[k]); need[m] = nd;
                nm[m] = r->need_name[k]; ic[m] = r->need_icon[k]; ++m;
            }
        }
        {   // 0.37.73: the pinned EcoWatcher mission's goals join the list (and so the fish marks); -2 = from EcoWatcher
            const int k0 = m;
            m += EcoPinGoals(id + m, have + m, need + m, nm + m, ic + m, kPl - m);
            for (int j = k0; j < m; ++j) g_pl_pin[j] = -2;
        }
        for (int j = 0; j < kPl; ++j) {
            auto key = [&](const char* f) { static Buf b; b.clear(); b.add("pl.").add(f).num(j); return b.s; };
            h->publish_i64(u, key("on"), j < m ? 1 : 0);
            if (j >= m) continue;
            Buf a, b;
            h->publish_i64(u, key("have"), have[j]);
            h->publish_i64(u, key("need"), need[j]);
            h->publish_i64(u, key("ok"), have[j] >= need[j] ? 1 : 0);
            h->publish_i64(u, key("short"), have[j] >= need[j] ? 0 : 1);
            h->publish_text(u, key("nn"), NoApos(nm[j], a));
            if (ic[j][0]) b.add("module:icon/").add(ic[j]);
            h->publish_text(u, key("isrc"), b.s);
        }
        h->publish_i64(u, "pl.n", m);
        g_pl_n = m;
        // 0.37.20: the hunt is over when every line reaches its need during the session (it was short
        // at some point since it was pinned): the pin goes (a recipe pinned already complete stays)
        {
            static int32_t hunt_id = -2; static bool hunt_short;
            int mr = 0; while (mr < m && g_pl_pin[mr] != -2) ++mr;   // 0.37.73: the recipes' lines only
            bool all = mr > 0; for (int j = 0; j < mr; ++j) all = all && have[j] >= need[j];
            if (g_pins[0] != hunt_id) { hunt_id = g_pins[0]; hunt_short = !all; }
            else if (!all) hunt_short = true;
            else if (hunt_short && g_pins[0] > 0) {
                for (int p = 0; p + 1 < kMaxPins; ++p) { g_pins[p] = g_pins[p + 1]; g_pin_tgt[p] = g_pin_tgt[p + 1]; }
                g_pins[kMaxPins - 1] = -1; g_pin_sel = -1; hunt_short = false;
            }
        }
        for (int j = 0; j < m; ++j) { g_pl_id[j] = id[j]; DcCopy(g_pl_ic[j], sizeof g_pl_ic[j], ic[j]); }
    }
    const Recipe* sr = g_pin_sel >= 0 && g_pin_sel < kMaxPins ? FindRecipe(in, g_pins[g_pin_sel]) : nullptr;
    if (!sr) g_pin_sel = -1;
    h->publish_i64(u, "pinsel.on", sr ? 1 : 0);
    { Buf t, v; if (sr) { NoApos(sr->name, t); if (sr->icon[0]) v.add("module:icon/").add(sr->icon); }
      h->publish_text(u, "pinsel.name", t.s); h->publish_text(u, "pinsel.isrc", v.s); }
    h->publish_i64(u, "nav.sushi", g_ticks < g_nav_sushi_until ? 1 : 0);
}

// The RECIPES panel for one recipe (tap a card): its needs, and PIN TO DIVE with a target.
void PublishRecipeDetail(const Instance& in, const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    const Recipe* r = FindRecipe(in, g_rec_sel);
    h->publish_i64(u, "recsel.on", r ? 1 : 0);
    if (!r) return;
    Buf t, v;
    h->publish_text(u, "recsel.name", NoApos(r->name, t));
    if (r->icon[0]) v.add("module:icon/").add(r->icon);
    h->publish_text(u, "recsel.isrc", v.s);
    h->publish_i64(u, "recsel.can", r->can_make);
    h->publish_i64(u, "recsel.price", r->price);
    h->publish_i64(u, "recsel.hasprice", r->price > 0 ? 1 : 0);
    h->publish_i64(u, "recsel.cando", r->can_make > 0 ? 1 : 0);
    h->publish_i64(u, "recsel.cannot", r->can_make > 0 ? 0 : 1);
    const int ps = PinSlot(r->id);
    h->publish_i64(u, "recsel.pinned", ps >= 0 ? 1 : 0);
    h->publish_i64(u, "recsel.pinfull", 0);   // 0.37.5: never full (one pin; pinning another replaces it)
    (void)ps;
    h->publish_i64(u, "pin.target", ps >= 0 ? g_pin_tgt[ps] : g_pin_target);
    h->publish_i64(u, "recsel.menu", r->on_menu ? 1 : 0);
    for (int k = 0; k < r->nneed; ++k) {
        Buf name, a, b;
        name.add("recsel.on").num(k); h->publish_i64(u, name.s, 1);
        name.clear(); name.add("recsel.nn").num(k); h->publish_text(u, name.s, NoApos(r->need_name[k], a));
        if (r->need_icon[k][0]) b.add("module:icon/").add(r->need_icon[k]);
        name.clear(); name.add("recsel.nisrc").num(k); h->publish_text(u, name.s, b.s);
        name.clear(); name.add("recsel.have").num(k); h->publish_i64(u, name.s, r->have[k]);
        name.clear(); name.add("recsel.qty").num(k); h->publish_i64(u, name.s, r->need_qty[k]);
        name.clear(); name.add("recsel.ok").num(k); h->publish_i64(u, name.s, r->have[k] >= r->need_qty[k] ? 1 : 0);
        name.clear(); name.add("recsel.short").num(k); h->publish_i64(u, name.s, r->have[k] >= r->need_qty[k] ? 0 : 1);
    }
}

// The pictures for the header and the tab bar: the game's own, read from the player's game files
// like every other picture (nothing from the game ships in the package).
void PublishUiIcons(const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    h->publish_text(u, "ui.ic_dave", "module:icon/Mission_Dave_Diver");
    h->publish_text(u, "ui.ic_dive", "module:icon/AppIcon_iDiver");   // 0.37.36: the iDiver app icon (in pictures.idx since 2026-10-18)
    h->publish_text(u, "ui.ic_cargo", "module:icon/CargoBox_Thumbnail");
    h->publish_text(u, "ui.ic_sushi", "module:icon/Sushi_BlueFin_Tuna_Ootoro");
    h->publish_text(u, "ui.ic_tasks", "module:icon/Mission_MainIcon_28");
}

// 0.37.25: an ingredient's picture as the game files actually name it. The name the tables give
// (BlueTang_Thumbnail) is checked against the picture list; failing that the recipe list's picture for it,
// the fish's, and the ingredient's own name (Blue Tang -> BlueTang / Blue_Tang, +_Thumbnail). Once per id.
bool PictureName(const EdenDsmodHostApi* h, const char* want, char* out, int cap);
bool IconIndexReady();
// 0.37.27: keyed by id AND the table's picture name, and only kept for good once the inputs are settled
// (the item's own picture known, the recipe list read). Before that it is worked out again every second,
// so a guess made while the tables were still loading never sticks (0.37.25/26: wrong or no pictures).
struct PicFix { int32_t id; char src[64]; char pic[64]; uint64_t at; bool settled; };
PicFix g_pfix[256]; int g_npfix;
char g_pfix_diag[128];
const char* IngPicture(const Instance& in, const EdenDsmodHostApi* h, const Ingredient& x) {
    PicFix* f = nullptr;
    for (int i = 0; i < g_npfix; ++i) if (g_pfix[i].id == x.id && eq(g_pfix[i].src, x.icon)) { f = &g_pfix[i]; break; }
    if (f && (f->settled || g_ticks - f->at < 60)) return f->pic[0] ? f->pic : x.icon;
    if (!IconIndexReady()) return x.icon;
    if (!f) {
        if (g_npfix >= 256) return x.icon;
        f = &g_pfix[g_npfix++]; f->id = x.id; DcCopy(f->src, 64, x.icon);
    }
    char cand[12][64]; int nc = 0;
    auto add = [&](const char* c) { if (c && c[0] && nc < 12) DcCopy(cand[nc++], 64, c); };
    add(x.icon);                                   // the table's own name first (what worked up to 0.37.24)
    for (int r = 0; r < in.nrec; ++r)              // the recipe list's picture for this very ingredient
        for (int k = 0; k < in.rec[r].nneed; ++k)
            if (in.rec[r].need_id[k] == x.id) add(in.rec[r].need_icon[k]);
    {   char a[64], b[64]; int na = 0, nb = 0;     // the ingredient's name: BlueTang / Blue_Tang
        for (int i = 0; x.name[i] && na < 62; ++i) {
            const char c = x.name[i];
            if (c == ' ') { a[na++] = '_'; continue; }
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { a[na++] = c; if (nb < 62) b[nb++] = c; }
        }
        a[na] = 0; b[nb] = 0; add(b); add(a);
        // 0.37.43: a cut of a fish (Bluefin Tuna Akami / Chutoro / Ootoro) has no picture of its own: the fish's,
        // found by dropping the last word(s) of the name (Bluefin_Tuna -> BlueFin_Tuna_Thumbnail)
        for (int cut = na - 1; cut > 0 && nc < 10; --cut) if (a[cut] == '_') { a[cut] = 0; add(a); }
    }
    // 0.37.56 (replaces 0.37.55's "drop the first word", which gave Clearfin Lionfish / Starry Puffer the white meat
    // picture instead of the fish the game shows): last, the item table's and the fish's own picture names
    const AltIcon* alt = nullptr;
    for (int i = 0; i < g_nalt; ++i) if (g_alt[i].id == x.id) alt = &g_alt[i];
    if (alt && alt->done) { add(alt->fish); add(alt->item); }
    f->pic[0] = 0; f->at = g_ticks;
    for (int q = 0; q < nc && !f->pic[0]; ++q) { char o[64]; if (PictureName(h, cand[q], o, sizeof o)) DcCopy(f->pic, 64, o); }
    f->settled = f->pic[0] && x.icon[0] && in.nrec > 0;
    if (!f->pic[0] && x.icon[0] && in.nrec > 0) {
        if (!alt) {                                 // ask ScanIngredients for the other names
            bool asked = false; for (int i = 0; i < g_nwant; ++i) asked = asked || g_alt_want[i] == x.id;
            if (!asked && g_nwant < 16) g_alt_want[g_nwant++] = x.id;
        }
        Buf d; d.add("no picture for ").add(x.name).add(" (").add(x.icon);
        if (alt && alt->done) d.add(" / item ").add(alt->item[0] ? alt->item : "-").add(" / fish ").num(alt->fish_id).addc(' ').add(alt->fish[0] ? alt->fish : "-");
        d.addc(')'); DcCopy(g_pfix_diag, sizeof g_pfix_diag, d.s);
        if (alt && alt->done) f->settled = 1;       // every name tried: stop asking
    }
    return f->pic[0] ? f->pic : x.icon;
}
// 0.38.40: the catch pop-up on the bottom screen (HideHUD hides the game's own): a new row in the cargo -> a box with
// its picture, name, rank, stars and weight for a few seconds; several in a row queue up. Rows already there when a dive
// starts do not pop up. Lifted catches are not rows (BuildRows), so they do not pop up either.
extern int g_mode;   // (defined with the scene reader further down)
struct CtShow { char icon[64]; char name[48]; int32_t rank, grade, wt, id; };
constexpr int kCtSeen = 256, kCtQ = 6;
constexpr uint64_t kCtTicks = 150;               // 2.5 s each
uint32_t g_ct_seen[kCtSeen]; int g_ct_nseen, g_ct_init;
CtShow g_ct_q[kCtQ]; int g_ct_qn; uint64_t g_ct_until; CtShow g_ct_cur; int g_ct_on;
uint32_t CtHash(const char* t, int32_t id) { uint32_t h = 2166136261u; for (; *t; ++t) h = (h ^ static_cast<uint8_t>(*t)) * 16777619u; return h ^ (static_cast<uint32_t>(id) * 2654435761u); }
bool CtSeen(uint32_t hsh) { for (int i = 0; i < g_ct_nseen; ++i) if (g_ct_seen[i] == hsh) return true; return false; }
void CtAdd(uint32_t hsh) { if (g_ct_nseen < kCtSeen) g_ct_seen[g_ct_nseen++] = hsh; else { for (int i = 1; i < kCtSeen; ++i) g_ct_seen[i - 1] = g_ct_seen[i]; g_ct_seen[kCtSeen - 1] = hsh; } }
void CtSee(const Item& it, const char* time) {   // one catch: queue it the first time it is seen (after the dive's start)
    const uint32_t hsh = CtHash(time, it.id);
    if (CtSeen(hsh)) return;
    CtAdd(hsh);
    if (!g_ct_init || !it.has_info || g_ct_qn >= kCtQ) return;
    CtShow& c = g_ct_q[g_ct_qn++];
    DcCopy(c.name, sizeof c.name, it.name);
    Buf src; if (it.icon[0]) { src.add("module:icon/"); for (int k = 0; it.icon[k]; ++k) { const char ch = it.icon[k];
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
        src.addc(ok ? ch : '_'); } }
    DcCopy(c.icon, sizeof c.icon, src.s);
    c.rank = it.rank; c.grade = it.grade; c.wt = static_cast<int32_t>(it.weight * 10.0f + 0.5f); c.id = it.id;
}
void CatchToast(const Instance& in, const Guest& g, const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    if (g_mode != 1) { g_ct_init = 0; g_ct_nseen = 0; g_ct_qn = 0; g_ct_on = 0; }
    else if (in.state == 1) {
        for (int r = 0; r < in.nrows; ++r) CtSee(in.items[in.rows[r].item], in.rows[r].time);
        // 0.38.42: the drone's catches (lift 2) are not CARGO rows but still pop up (user): their own catch times
        for (int i = 0; i < in.n; ++i) {
            const Item& it = in.items[i];
            if (it.lift != 2) continue;
            const uint64_t list = in.times_list[i], arr = list ? g.ptr(list + 0x10) : 0;
            const int32_t size = list ? g.s32(list + 0x18) : 0;
            for (int c = 0; c < (it.count > 0 ? it.count : 1) && c < 64; ++c) {
                char t[32] = "";
                if (arr && c < size) g.str(g.ptr(arr + 0x20 + static_cast<uint64_t>(c) * 8), t, sizeof t);
                CtSee(it, t);   // the same key as a row's: a catch moved to the drone does not pop up twice
            }
        }
        g_ct_init = 1;
    } else if (g_mode == 1) {
        g_ct_init = 1;   // 0.38.45: no cargo box yet (the game makes it with the first catch): the first catch pops up too (user)
    }
    if (g_ticks >= g_ct_until && g_ct_qn > 0) {   // the next one
        g_ct_cur = g_ct_q[0];
        for (int i = 1; i < g_ct_qn; ++i) g_ct_q[i - 1] = g_ct_q[i];
        --g_ct_qn; g_ct_until = g_ticks + kCtTicks;
    }
    g_ct_on = g_mode == 1 && g_ticks < g_ct_until;
    h->publish_i64(u, "ct.on", g_ct_on);
    Buf t;
    // 0.38.49: a new item's name is still its text key for a moment (the name lookup runs after): take it from the
    // cargo again each tick, so the pop-up shows the real name as soon as it is resolved (user: "Shellfish003 Name")
    for (int i = 0; g_ct_on && i < in.n; ++i)
        if (in.items[i].id == g_ct_cur.id && in.items[i].has_info && in.items[i].name[0]) { DcCopy(g_ct_cur.name, sizeof g_ct_cur.name, in.items[i].name); break; }
    h->publish_text(u, "ct.name", NoApos(g_ct_cur.name, t));
    h->publish_text(u, "ct.isrc", g_ct_cur.icon);
    h->publish_i64(u, "ct.rank", g_ct_cur.rank);
    h->publish_i64(u, "ct.grade", g_ct_cur.grade);
    h->publish_i64(u, "ct.wt", g_ct_cur.wt);
}
void Publish(const Instance& in, const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    h->publish_i64(u, "cargo.alive", 1);
    h->publish_i64(u, "cargo.ready", in.state);
    h->publish_i64(u, "cargo.gs_off", in.gs_off);
    h->publish_i64(u, "cargo.scans", in.scans);
    h->publish_text(u, "cargo.diag", in.diag);
    h->publish_text(u, "cargo.box_cls", in.box_cls);
    h->publish_i64(u, "cargo.n", in.n);
    h->publish_text(u, "cargo.dm_diag", in.dm_diag);
    h->publish_text(u, "cargo.text_diag", in.text_diag);
    h->publish_i64(u, "cargo.names", in.names.n);
    // one row per catch, like the game's Acquired list
    int r = 0;
    for (; r < in.nrows; ++r) {
        const Item& it = in.items[in.rows[r].item];
        Buf name;
        auto pub = [&](const char* f, int64_t v) { name.clear(); name.add("row.").add(f).num(r); h->publish_i64(u, name.s, v); };
        auto pubt = [&](const char* f, const char* v) { Buf t; name.clear(); name.add("row.").add(f).num(r); h->publish_text(u, name.s, NoApos(v, t)); };
        pub("on", 1);
        pub("id", it.id);
        pub("grade", it.grade);
        pub("info", it.has_info ? 1 : 0);
        pubt("time", in.rows[r].time);
        if (it.has_info) {
            pub("rank", it.rank);
            pub("wt", static_cast<int64_t>(it.weight * 10.0f + 0.5f));
            pub("sell", it.sell);
            pub("carve", it.carve);
            pubt("name", it.name);
            pubt("icon", it.icon);
            if (it.icon[0]) {   // picture from the player's own exported icons (user: folder)
                Buf src; src.add("module:icon/");
                for (int k = 0; it.icon[k]; ++k) {
                    char ch = it.icon[k];
                    bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
                    src.addc(ok ? ch : '_');
                }
                pubt("isrc", src.s);
            }
        }
    }
    {   // 0.38.40: each cargo stack's LootBoxSlot.m_AutoLiftedType (+0x70) -- to tell the drone-lifted catches the game
        // leaves out of its Acquired list (the value is not known yet; a filter on lift != 0 would drop normal catches)
        Buf d; d.add("lift: ");
        for (int i = 0; i < in.n; ++i) { const Item& it = in.items[i]; d.add(it.has_info ? it.name : "?").addc(' ').num(it.count).add("x lift ").num(it.lift).add(" | "); }
        h->publish_text(u, "cg.lift", d.s);
    }
    h->publish_i64(u, "row.sorted", in.sorted);
    h->publish_i64(u, "cargo.sort", g_cargo_sort);
    PublishUiIcons(h);
    // The host keeps only what is published on each frame, so the lists go out every frame. They
    // are long now that both pages scroll: publish the live entries, and zeros only for entries
    // that were live on the previous frame.
    static int last_nrec = kMaxRecipes, last_ning = kMaxIngredients;
    const int rec_end = in.nrec > last_nrec ? in.nrec : last_nrec, ing_end = in.ning > last_ning ? in.ning : last_ning;
    last_nrec = in.nrec; last_ning = in.ning;
    h->publish_i64(u, "rec.ready", in.rec_state);
    h->publish_i64(u, "rec.n", in.nrec);
    h->publish_i64(u, "rec.sort", g_rec_sort);
    h->publish_text(u, "rec.diag", in.rec_diag);
    for (int i = 0; i < rec_end && i < kMaxRecipes; ++i) {
        Buf name; bool live = i < in.nrec; const Recipe& r = in.rec[i];
        name.clear(); name.add("rec.on").num(i); h->publish_i64(u, name.s, live ? 1 : 0);
        if (!live) continue;
        { Buf t; name.clear(); name.add("rec.name").num(i); h->publish_text(u, name.s, NoApos(r.name, t)); }
        // gates for the card: servings possible, or none
        name.clear(); name.add("rec.cando").num(i); h->publish_i64(u, name.s, r.can_make > 0 ? 1 : 0);
        name.clear(); name.add("rec.cannot").num(i); h->publish_i64(u, name.s, r.can_make > 0 ? 0 : 1);
        { Buf v; if (r.icon[0]) v.add("module:icon/").add(r.icon); name.clear(); name.add("rec.isrc").num(i); h->publish_text(u, name.s, v.s); }
        name.clear(); name.add("rec.can").num(i); h->publish_i64(u, name.s, r.can_make);
        name.clear(); name.add("rec.price").num(i); h->publish_i64(u, name.s, r.price);
        name.clear(); name.add("rec.hasprice").num(i); h->publish_i64(u, name.s, r.price > 0 ? 1 : 0);
        name.clear(); name.add("rec.menu").num(i); h->publish_i64(u, name.s, r.on_menu ? 1 : 0);
        name.clear(); name.add("rec.pinned").num(i); h->publish_i64(u, name.s, PinSlot(r.id) >= 0 ? 1 : 0);
        for (int k = 0; k < kMaxNeeds; ++k) {
            bool nl = k < r.nneed;
            name.clear(); name.add("rec.n").num(i).add("_").num(k); h->publish_i64(u, name.s, nl ? 1 : 0);
            if (!nl) continue;
            { Buf t; name.clear(); name.add("rec.nn").num(i).add("_").num(k); h->publish_text(u, name.s, NoApos(r.need_name[k], t)); }
            { Buf v; if (nl && r.need_icon[k][0]) v.add("module:icon/").add(r.need_icon[k]); name.clear(); name.add("rec.nisrc").num(i).add("_").num(k); h->publish_text(u, name.s, v.s); }
            name.clear(); name.add("rec.have").num(i).add("_").num(k); h->publish_i64(u, name.s, r.have[k]);
            name.clear(); name.add("rec.qty").num(i).add("_").num(k); h->publish_i64(u, name.s, r.need_qty[k]);
            name.clear(); name.add("rec.short").num(i).add("_").num(k); h->publish_i64(u, name.s, r.have[k] < r.need_qty[k] ? 1 : 0);
            name.clear(); name.add("rec.ok").num(i).add("_").num(k); h->publish_i64(u, name.s, r.have[k] < r.need_qty[k] ? 0 : 1);
        }
    }
    h->publish_text(u, "ing.diag", g_ingdiag);
    h->publish_i64(u, "ing.ready", in.ing_state);
    int stocked = 0;
    for (int i = 0; i < in.ning; ++i) stocked += in.ing[i].count > 0 ? 1 : 0;
    // IN STOCK shows only the stacks with something in them (they come first: sorted by amount)
    const int shown = g_ing_filter == 1 ? stocked : in.ning;
    h->publish_i64(u, "ing.n", shown);
    h->publish_i64(u, "ing.all", in.ning);
    h->publish_i64(u, "ing.filter", g_ing_filter);
    h->publish_i64(u, "ing.stock", stocked);
    h->publish_i64(u, "ing.nempty", in.ning - stocked);
    for (int i = 0; i < ing_end && i < kMaxIngredients; ++i) {
        Buf name; bool live = i < shown; const Ingredient& x = in.ing[i];
        name.clear(); name.add("ing.on").num(i); h->publish_i64(u, name.s, live ? 1 : 0);
        // gates for the grid tile: a stocked box, a dimmed empty one (x0), the rank corner
        name.clear(); name.add("ing.full").num(i); h->publish_i64(u, name.s, live && x.count > 0 ? 1 : 0);
        name.clear(); name.add("ing.empty").num(i); h->publish_i64(u, name.s, live && x.count <= 0 ? 1 : 0);
        name.clear(); name.add("ing.rkon").num(i); h->publish_i64(u, name.s, live && (x.rank > 0 || x.level > 0) ? 1 : 0);
        if (!live) continue;
        name.clear(); name.add("ing.id").num(i); h->publish_i64(u, name.s, x.id);
        name.clear(); name.add("ing.count").num(i); h->publish_i64(u, name.s, x.count);
        name.clear(); name.add("ing.level").num(i); h->publish_i64(u, name.s, x.level);
        name.clear(); name.add("ing.rank").num(i); h->publish_i64(u, name.s, x.rank);
        // the rank the game's storage shows: the linked item's rank, else the stack's level
        name.clear(); name.add("ing.rk").num(i); h->publish_i64(u, name.s, x.rank > 0 ? x.rank : x.level);
        { Buf t; name.clear(); name.add("ing.name").num(i); h->publish_text(u, name.s, NoApos(x.name, t)); }
        { Buf v; const char* pic = IngPicture(in, h, x); if (pic[0]) v.add("module:icon/").add(pic); name.clear(); name.add("ing.isrc").num(i); h->publish_text(u, name.s, v.s); }
    }
    PublishIngredientDetail(in, h);
    PublishRecipeDetail(in, h);
    PublishPinnedRecipe(in, h);
    h->publish_i64(u, "row.n", r);
    for (int k = r; k < kMaxRows; ++k) { Buf name; name.add("row.on").num(k); h->publish_i64(u, name.s, 0); }
    for (int i = 0; i < kMaxItems; ++i) {
        Buf name; const Item& it = in.items[i];
        bool live = i < in.n;
        auto pub = [&](const char* f, int64_t v) { name.clear(); name.add("cargo.").add(f).num(i); h->publish_i64(u, name.s, v); };
        pub("on", live ? 1 : 0);
        if (!live) continue;
        pub("id", it.id); pub("type", it.type); pub("grade", it.grade); pub("fgrade", it.fgrade); pub("count", it.count);
        name.clear(); name.add("cargo.key").num(i); h->publish_text(u, name.s, it.key);
    }
}

// ---------------------------------------------------------------- ABI callbacks
EdenDsmodBool SupportsBuild(const char* build_id) {
    return build_id && starts(build_id, kBuild) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

void AbortDiscard();
void* Create(const EdenDsmodHostApi* host, const char*) {
    if (!host || host->abi_version != EDEN_DSMOD_MODULE_ABI_VERSION || host->abi_hash != EDEN_DSMOD_MODULE_ABI_HASH ||
        host->title_id != kTitle || !host->read_memory || !host->publish_i64 || !host->publish_text) {
        return nullptr;
    }
    memset(&g_inst, 0, sizeof g_inst);
    g_inst.gs_off = -1;
    AbortDiscard();
    g_inst.main_base = host->main_base;
    const char* hello = "waiting for the game";
    memcpy(g_inst.diag, hello, 21);
    if (host->log) host->log(host->userdata, EDEN_DSMOD_LOG_INFO, "DaveTheDiver cargo module ready");
    return &g_inst;
}

void Destroy(void*) {}

// ---------------------------------------------------------------- oxygen tank maximum (round 10)
// PlayerBreathHandler.get_MaxHP (main+0x3F50380): m_CustomMaxHP (+0xBC) when set, else
// m_CharacterStatus (+0x30).GetValue(StatusDefine 2) * m_MaxHPParameter (+0xA4).
// PlayerCharacterStatus.GetValue (main+0x3F69840): m_TotalStatus[key] (+0x10) plus the equipped
// item stats m_ItemStatus[GameInfoSave+0x10] (+0x20, List<Dictionary<StatusDefine, float>>).
// Dictionary<StatusDefine, float> entries: {hash 0, next 4, key 8, value 0xC}, 0x10 bytes each.
constexpr uint64_t kInGameManagerTypeInfo = 0x9DB0400;
struct O2Max { float base, item, param, custom, max; int ok, stage, idx, listn; };
O2Max g_o2;
// Is the game paused (a menu open)? The list's IsFocused flag stays 1 after the menu closes (seen
// on device), so the menu counts as open only while O2 (PlayerBreathHandler.m_HP) also stands
// still -- it drains every frame during a dive and freezes while the pause menu is up.
uint64_t g_bh;            // PlayerBreathHandler (0 = not readable: fall back to IsFocused alone)
float g_o2_last;
int g_o2_still;           // samples O2 has not changed
constexpr int kPausedSamples = 40;
bool GamePaused() { return !g_bh || g_o2_still >= kPausedSamples; }
constexpr int kRunningStill = 50;   // O2 changed within the last ~0.8 s: the game is running
constexpr int kOpenStill = 300;     // O2 unchanged for ~5 s: paused

bool FloatDictGet(const Guest& g, uint64_t d, int32_t key, float* out) {
    uint64_t entries = d ? g.ptr(d + 0x18) : 0;
    int32_t cnt = d ? g.s32(d + 0x20) : 0;
    if (!entries || cnt <= 0 || cnt > 4096) return false;
    for (int32_t i = 0; i < cnt; ++i) {
        uint8_t e[0x10];
        if (!g.read(entries + 0x20 + static_cast<uint64_t>(i) * 0x10, e, sizeof e)) continue;
        int32_t hash, k; float v;
        memcpy(&hash, e, 4); memcpy(&k, e + 8, 4); memcpy(&v, e + 0xC, 4);
        if (hash >= 0 && (k == key || (k & 0xFF) == key)) { *out = v; return true; }   // enum may be 1 byte
    }
    return false;
}

char g_o2keys[160];
void DumpFloatDict(const Guest& g, uint64_t d, Buf& b) {
    uint64_t entries = d ? g.ptr(d + 0x18) : 0;
    int32_t cnt = d ? g.s32(d + 0x20) : 0;
    b.add("n=").num(cnt);
    for (int32_t i = 0; entries && i < cnt && i < 6; ++i) {
        uint8_t e[0x10];
        if (!g.read(entries + 0x20 + static_cast<uint64_t>(i) * 0x10, e, sizeof e)) continue;
        int32_t k; float v; memcpy(&k, e + 8, 4); memcpy(&v, e + 0xC, 4);
        b.add(" ").hex(static_cast<uint32_t>(k)).add("=").num(static_cast<int64_t>(v * 10.0f));
    }
}

void ReadO2Max(Instance& in, const Guest& g) {
    O2Max o{}; char c[64];
    uint64_t ti = g.ptr(in.main_base + kInGameManagerTypeInfo);
    uint64_t single = ti ? g.ptr(ti + kParent) : 0;
    uint64_t st = single ? g.ptr(single + kStatics) : 0;
    uint64_t ig = st ? g.ptr(st) : 0;
    uint64_t ch = ig ? g.ptr(ig + 0xA0) : 0;
    uint64_t bh = ch ? g.ptr(ch + 0x298) : 0;
    o.stage = 1;
    if (!bh || !g.cls(bh, c, sizeof c) || !eq(c, "PlayerBreathHandler")) { g_o2 = o; g_bh = 0; return; }
    g_bh = bh;
    o.stage = 2;
    g.read(bh + 0xA4, &o.param, 4);
    g.read(bh + 0xBC, &o.custom, 4);
    uint64_t status = g.ptr(bh + 0x30);
    if (status) {
        o.stage = 3;
        bool hb = FloatDictGet(g, g.ptr(status + 0x10), 2, &o.base);   // a missing key counts as 0 (as in the game)
        uint64_t list = g.ptr(status + 0x20);
        uint64_t info = in.gs ? g.ptr(in.gs + 0xF0) : 0;      // SaveData.m_GameInfo
        o.idx = info ? g.s32(info + 0x10) : (in.gs ? -2 : -3);
        {   // diagnostics: total-status keys, and the first item-status dictionary
            Buf b; b.add("total "); DumpFloatDict(g, g.ptr(status + 0x10), b);
            uint64_t arr0 = list ? g.ptr(list + 0x10) : 0;
            b.add(" | item0 "); DumpFloatDict(g, arr0 ? g.ptr(arr0 + 0x20) : 0, b);
            memcpy(g_o2keys, b.s, static_cast<size_t>(b.n) + 1);
        }
        o.listn = list ? g.s32(list + 0x18) : 0;
        bool hi = false;
        // equipped-item stats: slot from SaveData.m_GameInfo+0x10 when readable, else slot 0
        // (on device the first slot holds the tank: O2 90, cargo 9.0 kg -- matches the HUD)
        int slot = (o.idx >= 0 && o.idx < o.listn) ? o.idx : (o.listn > 0 ? 0 : -1);
        if (slot >= 0) {
            uint64_t arr = g.ptr(list + 0x10);
            hi = FloatDictGet(g, arr ? g.ptr(arr + 0x20 + static_cast<uint64_t>(slot) * 8) : 0, 2, &o.item);
        }
        if (hb || hi) { o.stage = 4; o.max = (o.base + o.item) * o.param; o.ok = o.max > 0.5f ? 1 : 0; }
    }
    if (o.custom > 0.0001f) { o.max = o.custom; o.ok = 1; }
    g_o2 = o;
}

// ---------------------------------------------------------------- discard probe (round 11)
// The in-dive menu's Acquired list is LootBoxPanel (no static path to it), so the module finds
// the instance by type: the class comes from the metadata slot of its nested "LootBoxPanel.<>c"
// (main+0x9DC4798) -> Il2CppClass.declaringType (+0x50); the heap is scanned (in the background,
// a few MB a frame) for an object whose klass is that class and whose lootsScroll (+0x60) is a
// PauseLootBoxScrollPanel. Fields: lootsScroll 0x60, confirmPanel 0x68, dumpConfirmText 0x78
// (UIDataText: "<name> is selected. Discard?"), IsFocused 0xB0.
constexpr uint64_t kLootBoxPanelNestedTypeInfo = 0x9DC4798;
struct UiProbe {
    uint64_t cls, panel, cursor, begin, end;
    int state;            // 0 idle, 1 scanning, 2 found, 3 class not loaded
    int passes;
    uint64_t retry_tick;
    char confirm[96];
    char scroll_ints[200];
    int focused;
    int cands, cands_last;   // objects whose first word is the LootBoxPanel class (this pass / last pass)
    char cand_info[200];     // what the first few candidates hold at lootsScroll / confirmPanel
    char range[160];         // heap range vs. where known game objects live
    char subs[120];          // names of LootBoxPanel subclasses seen in the search
    int null_run;            // consecutive unmapped 64 KB steps (to stride faster over big holes)
    uint64_t ck[4096];       // cache: klass pointer -> is LootBoxPanel or a subclass of it
    uint8_t cv[4096];
    uint64_t session;     // Instance.main_base when this state was built (reset on a new session)
};
UiProbe g_ui;

bool UiText(const Guest& g, uint64_t uidatatext, char* out, int cap) {   // UIDataText -> TMP m_text
    out[0] = 0;
    uint64_t list = uidatatext ? g.ptr(uidatatext + 0x48) : 0;
    uint64_t items = list ? g.ptr(list + 0x10) : 0;
    uint64_t el = items ? g.ptr(items + 0x20) : 0;
    uint64_t ez = el ? g.ptr(el + 0x18) : 0;
    uint64_t tmp = ez ? g.ptr(ez + 0x30) : 0;
    return tmp && g.str(g.ptr(tmp + 0xD8), out, cap);
}

// Is `k` the LootBoxPanel class or a class derived from it (the in-dive menu may be a subclass)?
bool IsPanelClass(const Guest& g, uint64_t k) {
    UiProbe& u = g_ui;
    if (k == u.cls) return true;
    if (!k || (k & 7)) return false;
    // classes sit in the metadata area next to LootBoxPanel's own class; skip other pointers cheaply
    uint64_t d = k > u.cls ? k - u.cls : u.cls - k;
    if (d > UINT64_C(0x10000000)) return false;
    unsigned i = static_cast<unsigned>((k >> 3) & 4095);
    if (u.ck[i] == k) return u.cv[i] != 0;
    bool yes = false;
    uint64_t p = k;
    for (int n = 0; n < 4 && p; ++n) {      // Il2CppClass.parent @0x58
        p = g.ptr(p + 0x58);
        if (p == u.cls) { yes = true; break; }
    }
    u.ck[i] = k; u.cv[i] = yes ? 1 : 0;
    return yes;
}

bool UiPanelValid(const Guest& g, uint64_t obj, uint64_t cls) {
    char c[64];
    (void)cls;
    if (!obj || !IsPanelClass(g, g.ptr(obj))) return false;
    uint64_t scroll = g.ptr(obj + 0x60);
    return scroll && g.cls(scroll, c, sizeof c) && eq(c, "PauseLootBoxScrollPanel");
}

void UiProbeStep(Instance& in, const Guest& g, const EdenDsmodHostApi* h) {
    UiProbe& u = g_ui;
    if (u.session != in.main_base) { memset(&u, 0, sizeof u); u.session = in.main_base; }
    char c[64];
    if (!u.cls) {
        uint64_t nested = g.ptr(in.main_base + kLootBoxPanelNestedTypeInfo);
        uint64_t cls = nested ? g.ptr(nested + 0x50) : 0;
        if (!cls || !g.cstr(g.ptr(cls + 0x10), c, sizeof c) || !eq(c, "LootBoxPanel")) { u.state = 3; return; }
        u.cls = cls;
    }
    if (u.panel && !UiPanelValid(g, u.panel, u.cls)) { u.panel = 0; u.state = 0; }
    if (!u.panel) {
        if (u.state != 1) {
            if (g_ticks < u.retry_tick) return;
            // On device the game's objects (the save, the classes) are NOT in the host's heap range,
            // so search the address space around them instead: the save data and the class
            // both live in the managed area, and the panel is allocated there too.
            uint64_t lo = in.gs && in.gs < u.cls ? in.gs : u.cls, hi = in.gs > u.cls ? in.gs : u.cls;
            // Even passes: +-512 MB around the class and the save (quick; where the panel was on
            // device). Odd passes: +-3 GB (holes are skipped fast).
            const uint64_t kWin = (u.passes & 1) ? UINT64_C(0xC0000000) : UINT64_C(0x20000000);
            u.begin = lo > kWin ? lo - kWin : 0;
            u.end = hi + kWin;
            if (!h->get_read_pointer) { u.state = 0; u.retry_tick = g_ticks + 600; return; }
            u.cursor = u.begin & ~UINT64_C(4095); u.state = 1; u.cands = 0; u.null_run = 0;
            u.cand_info[0] = 0; u.subs[0] = 0;
            Buf r; r.add("search ").hex(u.begin).add("-").hex(u.end).add(" save ").hex(in.gs);
            r.add(in.gs >= u.begin && in.gs < u.end ? " in" : " OUT");
            uint64_t mb = in.main_base; r.add(" main ").hex(mb);
            memcpy(u.range, r.s, static_cast<size_t>(r.n) + 1);
        }
        for (int pages = 0; pages < 2048 && u.cursor < u.end; ++pages, u.cursor += 4096) {
            const uint8_t* pg = h->get_read_pointer(h->userdata, u.cursor, 4096);
            if (!pg) {   // unmapped: skip to the next 64 KB boundary; after 2 MB of holes, 2 MB at a time
                uint64_t mask = ++u.null_run > 32 ? UINT64_C(0x1FFFFF) : UINT64_C(0xFFFF);
                u.cursor = (u.cursor | mask) + 1 - 4096;
                continue;
            }
            u.null_run = 0;
            for (int off = 0; off < 4096; off += 8) {
                uint64_t v; memcpy(&v, pg + off, 8);
                if (!IsPanelClass(g, v)) continue;
                uint64_t obj = u.cursor + static_cast<uint64_t>(off);
                if (UiPanelValid(g, obj, u.cls)) { u.panel = obj; u.state = 2; break; }
                if (v - obj < 0x400 || obj - v < 0x400) continue;   // a pointer inside the class itself
                if (v != u.cls && g.cstr(g.ptr(v + 0x10), c, sizeof c)) {   // a subclass: note its name
                    int sl = 0; while (u.subs[sl]) ++sl;
                    bool seen = false;
                    for (int a = 0; u.subs[a] && !seen; ++a) {
                        int b2 = 0; while (c[b2] && u.subs[a + b2] == c[b2]) ++b2;
                        seen = !c[b2];
                    }
                    if (!seen && sl + 64 < static_cast<int>(sizeof u.subs)) {
                        Buf sb; sb.add(u.subs).add(c).add(" ");
                        int n2 = sb.n < static_cast<int>(sizeof u.subs) - 1 ? sb.n : static_cast<int>(sizeof u.subs) - 1;
                        memcpy(u.subs, sb.s, static_cast<size_t>(n2)); u.subs[n2] = 0;
                    }
                }
                if (u.cands++ < 4) {   // note what each candidate looks like (diagnostics)
                    Buf b; b.add(u.cand_info);
                    b.hex(obj).add(": ");
                    for (uint64_t fo = 0x58; fo <= 0x78; fo += 8) {
                        uint64_t f = g.ptr(obj + fo);
                        if (f && g.cls(f, c, sizeof c)) b.add(c).add(" "); else b.add("- ");
                    }
                    b.add("| ");
                    int n2 = b.n < static_cast<int>(sizeof u.cand_info) - 1 ? b.n : static_cast<int>(sizeof u.cand_info) - 1;
                    memcpy(u.cand_info, b.s, static_cast<size_t>(n2)); u.cand_info[n2] = 0;
                }
            }
            if (u.panel) break;
        }
        if (!u.panel && u.cursor >= u.end) {
            u.state = 0; ++u.passes; u.retry_tick = g_ticks + 120; u.cands_last = u.cands;
            if (!u.cands) memcpy(u.cand_info, "none", 5);
        }
        if (!u.panel) return;
    }
    // found: read the live state
    u.focused = g.read(u.panel + 0xB0, &c[0], 1) ? (c[0] != 0) : 0;
    UiText(g, g.ptr(u.panel + 0x78), u.confirm, sizeof u.confirm);
    Buf b;
    uint64_t scroll = g.ptr(u.panel + 0x60);
    for (uint64_t off = 0x18; off < 0x110 && b.n < 180; off += 4) {   // small ints in the scroller: find the cursor
        int32_t v = g.s32(scroll + off);
        if (v != 0 && v >= -1 && v <= 64) { b.hex(off).add("=").num(v).add(" "); }
    }
    memcpy(u.scroll_ints, b.s, static_cast<size_t>(b.n) + 1);
}

void UiProbePublish(const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    h->publish_i64(u, "ui.state", g_ui.state);
    h->publish_i64(u, "ui.found", g_ui.panel ? 1 : 0);
    h->publish_i64(u, "ui.focused", g_ui.focused);
    h->publish_i64(u, "ui.passes", g_ui.passes);
    h->publish_i64(u, "ui.scan_mb", g_ui.begin && g_ui.cursor > g_ui.begin ? static_cast<int64_t>((g_ui.cursor - g_ui.begin) >> 20) : 0);
    h->publish_text(u, "ui.confirm", g_ui.confirm);
    h->publish_text(u, "ui.scroll", g_ui.scroll_ints);
    h->publish_i64(u, "ui.cands", g_ui.state == 0 ? g_ui.cands_last : g_ui.cands);
    h->publish_text(u, "ui.cand_info", g_ui.cand_info);
    h->publish_text(u, "ui.range", g_ui.range);
    h->publish_text(u, "ui.subs", g_ui.subs[0] ? g_ui.subs : "no subclasses seen");
    Buf cb; cb.hex(g_ui.cls); h->publish_text(u, "ui.cls", cb.s);
}

void DiscardStep(Instance& in, const Guest& g);
void PatchWatch();
void DcPublish(const EdenDsmodHostApi* h, uint64_t tick);

// ---------------------------------------------------------------- mission icons (round 14)
// The mission tracker the MISSIONS page reads as text (MissionHUD.s_Current, main+0x9DB2820 ->
// statics +0xB8 -> +0x0): slots Dictionary @0x80 (entries @0x18, value @0x20+i*0x18+0x10), each
// slot's conditions List @0x58 (items @0x10, [j] @0x20+j*8), each row's MissionConditionData
// @0x90 with Target @0x20 -- the item a "collect" condition asks for. Its ItemUIIcon names the
// picture, as for the cargo rows.
void DcCopy(char* dst, int cap, const char* src);
constexpr uint64_t kMissionHudTypeInfo = 0x9DB2820;
constexpr int kMTasks = 3, kMConds = 6;   // 0.37.8: up to 6 rows per mission
struct MissionIcons {
    int32_t target[kMTasks][kMConds]; char icon[kMTasks][kMConds][64];
    // the character who gave the mission: MissionHUDSlot.<Data> @0x88 -> MissionData.<NpcData> @0x40
    // (NPCNormal: key @0x18 "Cobra", name id @0x28 "NPC_Name_Cobra", talk id @0x38, mission icon
    // @0x48 "Mission_Cobra" -- the sprite the game's tracker shows; read by content, see below),
    // MissionData.m_NPCID @0x128. The icon name (or, failing that, patterns around the key) finds
    // the picture among the exported ones; see NpcPicture.
    int32_t npc_id[kMTasks]; char npc_strs[kMTasks][200]; char npc_key[kMTasks][48]; char npc_pic[kMTasks][64];
    uint64_t md[kMTasks];         // the slot's MissionData (identifies a pinned mission while it lasts)
    bool main[kMTasks]; int32_t type[kMTasks]; char sym[kMTasks][40];   // sym: the tracker's own kind icon           // MissionData.<Type> @0x38 == MissionType.Main (0): the game's "!" missions
};
MissionIcons g_mi;
// 0.37.21 (round 35): MissionHUDSlot.symbolImage @0x48 is the Image showing the mission's kind (main / sub /
// VIP / party / ...). Its Sprite's name, found by search once: the Image field holding a "Sprite", then the
// C string in the sprite's native object that starts "Mission_".
int g_sym_img = -1, g_sym_nat = -1, g_sym_tries;
bool MissionSymbol(const Guest& g, uint64_t slot, char* out, int cap) {
    out[0] = 0;
    const uint64_t img = slot ? g.ptr(slot + 0x48) : 0;
    if (!img) return false;
    char c[40], t[64];
    auto try_sprite = [&](uint64_t sp, bool learn) {
        if (!sp || !g.cls(sp, c, sizeof c) || !eq(c, "Sprite")) return false;
        const uint64_t nat = g.ptr(sp + 0x10);
        if (!nat) return false;
        if (g_sym_nat >= 0) {
            if (!g.cstr(g.ptr(nat + static_cast<uint64_t>(g_sym_nat)), t, sizeof t)) return false;
        } else {
            if (!learn) return false;
            for (int o = 0; o <= 0x100; o += 8) {
                if (g.cstr(g.ptr(nat + static_cast<uint64_t>(o)), t, sizeof t) && starts(t, "Mission_")) { g_sym_nat = o; break; }
            }
            if (g_sym_nat < 0) return false;
        }
        if (!starts(t, "Mission_")) return false;
        DcCopy(out, cap, t); return true;
    };
    if (g_sym_img >= 0) return try_sprite(g.ptr(img + static_cast<uint64_t>(g_sym_img)), false);
    if (g_sym_tries > 30) return false;
    ++g_sym_tries;
    for (int o = 0x20; o <= 0x180; o += 8)
        if (try_sprite(g.ptr(img + static_cast<uint64_t>(o)), true)) { g_sym_img = o; return true; }
    return false;
}
void ScanMissionIcons(Instance& in, const Guest& g) {
    MissionIcons m{};
    uint64_t cls = g.ptr(in.main_base + kMissionHudTypeInfo);
    uint64_t st = cls ? g.ptr(cls + kStatics) : 0;
    uint64_t hud = st ? g.ptr(st) : 0;
    uint64_t entries = hud ? g.ptr(g.ptr(hud + 0x80) + 0x18) : 0;
    uint64_t table = entries ? ItemTable(in, g) : 0;
    for (int i = 0; i < kMTasks && entries; ++i) {
        uint64_t slot = g.ptr(entries + 0x30 + static_cast<uint64_t>(i) * 0x18);
        uint64_t md = slot ? g.ptr(slot + 0x88) : 0;
        uint64_t npc = md ? g.ptr(md + 0x40) : 0;
        if (md) m.npc_id[i] = g.s32(md + 0x128);
        m.md[i] = md;
        m.type[i] = md ? g.s32(md + 0x38) : -1;
        m.main[i] = md && g.s32(md + 0x38) == 0;   // round 21: MissionType Main = 0, Side = 1, ...
        if (md) MissionSymbol(g, slot, m.sym[i], sizeof m.sym[i]);
        if (npc) {
            Buf b; char c[48];
            for (uint64_t off = 0x10; off < 0x90; off += 8) {   // text fields, for the MISSIONS diag line
                uint64_t sp = g.ptr(npc + off);
                if (!sp || !g.cls(sp, c, sizeof c) || !eq(c, "String")) continue;
                char t[64];
                if (!g.str(sp, t, sizeof t) || !t[0]) continue;
                if (b.n < 150) { b.hex(off).add("=").add(t).add(" "); }
                // found by content, not offset (on device: key @0x18, icon @0x48): the first text is
                // the key ("Cobra"), the one starting "Mission_" is the tracker's face sprite
                if (!m.npc_key[i][0]) DcCopy(m.npc_key[i], sizeof m.npc_key[i], t);
                const char* mp = "Mission_"; int q = 0; while (mp[q] && t[q] == mp[q]) ++q;
                if (!mp[q] && !m.npc_pic[i][0]) DcCopy(m.npc_pic[i], sizeof m.npc_pic[i], t);
            }
            int n2 = b.n < 199 ? b.n : 199; memcpy(m.npc_strs[i], b.s, static_cast<size_t>(n2)); m.npc_strs[i][n2] = 0;
        }
        uint64_t list = slot ? g.ptr(slot + 0x58) : 0;
        uint64_t items = list ? g.ptr(list + 0x10) : 0;
        int32_t n = list ? g.s32(list + 0x18) : 0;
        for (int j = 0; j < kMConds && j < n && items; ++j) {
            uint64_t row = g.ptr(items + 0x20 + static_cast<uint64_t>(j) * 8);
            uint64_t cd = row ? g.ptr(row + 0x90) : 0;
            if (!cd) continue;
            int32_t t = g.s32(cd + 0x20);
            m.target[i][j] = t;
            uint64_t v = (t > 0 && table) ? FindItem(in, g, table, t) : 0;
            if (v) g.str(g.ptr(v + 0x38), m.icon[i][j], sizeof m.icon[i][j]);
        }
    }
    g_mi = m;
}
struct IconIndex;
bool NpcPicture(const EdenDsmodHostApi* h, const char* pic, const char* key, char* out, int cap);
bool IconIndexReady();
char g_pic_key[kMTasks][48], g_pic_file[kMTasks][96];
char g_ic_name[kMTasks][kMConds][64]; bool g_ic_has[kMTasks][kMConds], g_ic_known[kMTasks][kMConds];
bool PictureName(const EdenDsmodHostApi* h, const char* want, char* out, int cap);
// The mission pinned to the DIVE page (by its MissionData; a finished mission simply stops matching).
uint64_t g_pin_md;
// 0.37.47: a mission is pinnable only when every task's target is something the map can mark: an item to find
// (the item table knows it), or an ingredient / fish to catch. "Serve a dish" targets (a customer) are not.
uint8_t g_canpin[3] = {1, 1, 1};   // pinnable until the tables say otherwise
// Main story missions (the game's "!" mark) stay on DIVE with the pinned one (g_mi.main).
bool AmHas(uint64_t md);
void PublishMissionIcons(const EdenDsmodHostApi* h) {
    bool any = false;
    for (int i = 0; i < kMTasks; ++i) {
        const bool pin = g_pin_md && g_mi.md[i] == g_pin_md;
        any = any || pin;
        Buf k; k.add("m.pin").num(i); h->publish_i64(h->userdata, k.s, pin ? 1 : 0);
        k.clear(); k.add("m.canpin").num(i); h->publish_i64(h->userdata, k.s, g_canpin[i] ? 1 : 0);
        k.clear(); k.add("m.main").num(i); h->publish_i64(h->userdata, k.s, g_mi.main[i] ? 1 : 0);
        k.clear(); k.add("m.type").num(i); h->publish_i64(h->userdata, k.s, g_mi.type[i]);
        {   // the tracker's kind icon (by name; the main / sub fallback while it is not read)
            Buf sv; if (g_mi.sym[i][0]) sv.add("module:icon/").add(g_mi.sym[i]);
            k.clear(); k.add("m.sym").num(i); h->publish_text(h->userdata, k.s, sv.s);
            k.clear(); k.add("m.hassym").num(i); h->publish_i64(h->userdata, k.s, g_mi.sym[i][0] ? 1 : 0);
        }
    }
    if (!any && g_pin_md) any = AmHas(g_pin_md);   // 0.38.57: pinned from a MISSIONS card the tracker does not show: its marks show too
    h->publish_i64(h->userdata, "m.anypin", any ? 1 : 0);
    for (int i = 0; i < kMTasks; ++i) {
        Buf k, v;
        if (!eq(g_pic_key[i], g_mi.npc_key[i])) {   // resolve once per character
            DcCopy(g_pic_key[i], sizeof g_pic_key[i], g_mi.npc_key[i]);
            g_pic_file[i][0] = 0;
            if (g_pic_key[i][0] && !NpcPicture(h, g_mi.npc_pic[i], g_pic_key[i], g_pic_file[i], sizeof g_pic_file[i])) g_pic_file[i][0] = 0;
            if (!IconIndexReady()) g_pic_key[i][0] = 0;   // no picture list yet: look again next time
        }
        k.add("m.npcid").num(i); h->publish_i64(h->userdata, k.s, g_mi.npc_id[i]);
        k.clear(); k.add("m.npc").num(i); h->publish_text(h->userdata, k.s, g_mi.npc_strs[i]);
        k.clear(); k.add("m.npckey").num(i); h->publish_text(h->userdata, k.s, g_mi.npc_key[i]);
        k.clear(); k.add("m.picn").num(i); h->publish_text(h->userdata, k.s, g_pic_file[i][0] ? g_pic_file[i] : "-");
        k.clear(); k.add("m.pic").num(i);
        if (g_pic_file[i][0]) { v.add("module:icon/").add(g_pic_file[i]); h->publish_text(h->userdata, k.s, v.s); }
        k.clear(); k.add("m.haspic").num(i); h->publish_i64(h->userdata, k.s, g_pic_file[i][0] ? 1 : 0);
    }
    for (int i = 0; i < kMTasks; ++i)
        for (int j = 0; j < kMConds; ++j) {
            Buf k, v;
            k.add("m.tgt").num(i).add("_").num(j); h->publish_i64(h->userdata, k.s, g_mi.target[i][j]);
            k.clear(); k.add("m.ic").num(i).add("_").num(j);
            if (g_mi.icon[i][j][0]) { v.add("module:icon/").add(g_mi.icon[i][j]); h->publish_text(h->userdata, k.s, v.s); }
            k.clear(); k.add("m.icn").num(i).add("_").num(j); h->publish_text(h->userdata, k.s, g_mi.icon[i][j]);
            if (!eq(g_ic_name[i][j], g_mi.icon[i][j]) || !g_ic_known[i][j]) {   // is there a picture? (once per name)
                char f[96];
                DcCopy(g_ic_name[i][j], sizeof g_ic_name[i][j], g_mi.icon[i][j]);
                g_ic_has[i][j] = g_mi.icon[i][j][0] && PictureName(h, g_mi.icon[i][j], f, sizeof f);
                g_ic_known[i][j] = IconIndexReady();
            }
            k.clear(); k.add("m.hasic").num(i).add("_").num(j); h->publish_i64(h->userdata, k.s, g_ic_has[i][j] ? 1 : 0);
        }
}

// The item table and the exported file names can differ in letter case ("BlueTang_Thumbnail" vs
// the sprite "Bluetang_Thumbnail"), and some names lack the "_Thumbnail" suffix. The export's
// _index.txt lists every picture ("name<TAB>WxH" per line): find the name there ignoring case,
// with or without the suffix, and return the exact file name.
// Two places hold the exported pictures: the user: folder, or the package itself after
// tools/dtd/add_icons.py merged them into the player's own copy (installed by Eden, so readable
// even where Android refuses files another app copied into Eden's folder).
struct IconIndex { char data[128 * 1024]; size_t n; bool loaded; const char* prefix; };
const char* const kIconPrefixes[2] = {"user:icons/", "file:icons/"};
char LowerC(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
bool ResolveIcon(const EdenDsmodHostApi* h, IconIndex& ix, const char* want, char* out, int cap) {
    if (!ix.loaded) {
        for (int k = 0; k < 2 && !ix.loaded && h->read_romfs; ++k) {
            Buf p; p.add(kIconPrefixes[k]).add("_index.txt");
            ix.n = h->read_romfs(h->userdata, p.s, 0, ix.data, sizeof ix.data - 1);
            ix.data[ix.n] = 0; ix.loaded = ix.n > 0; ix.prefix = kIconPrefixes[k];
        }
        if (!ix.loaded) return false;
    }
    for (int pass = 0; pass < 2; ++pass) {
        const char* suffix = pass ? "_thumbnail" : "";
        size_t i = 0;
        while (i < ix.n) {
            size_t e = i; while (e < ix.n && ix.data[e] != '\t' && ix.data[e] != '\n') ++e;
            size_t k = 0; bool same = true;
            for (; want[k]; ++k) if (i + k >= e || LowerC(ix.data[i + k]) != LowerC(want[k])) { same = false; break; }
            if (same) {
                size_t j = 0;
                for (; suffix[j]; ++j) if (i + k + j >= e || LowerC(ix.data[i + k + j]) != suffix[j]) { same = false; break; }
                if (same && i + k + j == e && static_cast<int>(e - i) < cap) {
                    memcpy(out, ix.data + i, e - i); out[e - i] = 0;
                    for (int c = 0; out[c]; ++c) {   // as export_icons.py names the file
                        char ch = out[c];
                        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
                        if (!ok) out[c] = '_';
                    }
                    return true;
                }
            }
            while (e < ix.n && ix.data[e] != '\n') ++e;
            i = e + 1;
        }
    }
    return false;
}
IconIndex g_ix_tick, g_ix_worker;
bool LiveTickLoaded();
bool IconIndexReady() { return g_ix_tick.loaded || LiveTickLoaded(); }

// ---------------------------------------------------------------- pictures from the game files (round 15)
// pictures.idx (in the package; made by tools/dtd/build_picture_index.py from a romfs) says where
// each picture lives -- names and positions only, no pixels:
//   B <bundle> <romfs path> <file size>
//   T <texture> <bundle> <offset in the bundle's data stream> <size> <width> <height> <format> <gobs>
//   S <name> <texture> <x> <y> <w> <h> <rotation>        (rectangle in texture pixels, origin bottom left)
// The module reads the picture's blocks out of the bundle (UnityFS: big-endian header, LZ4 blocks),
// undoes the Switch block-linear layout (gobs > 0), has the host decode the ASTC blocks, then crops
// and flips like UnityPy. Anything unexpected -> false, and the exported pictures are used instead.
struct LiveIndex { char data[256 * 1024]; size_t n; uint32_t tried; bool loaded; };
LiveIndex g_live_tick, g_live_worker;
bool LiveTickLoaded() { return g_live_tick.loaded; }
bool LiveLoad(const EdenDsmodHostApi* h, LiveIndex& ix) {
    if (ix.loaded) return true;
    if (ix.tried++ % 64) return false;              // missing: look again now and then
    if (!h->read_romfs) return false;
    ix.n = h->read_romfs(h->userdata, "file:pictures.idx", 0, ix.data, sizeof ix.data - 1);
    ix.data[ix.n] = 0;
    ix.loaded = ix.n > 6 && ix.data[0] == 'D' && ix.data[1] == 'T' && ix.data[2] == 'D' && ix.data[3] == 'P';
    return ix.loaded;
}
uint64_t ParseU(const char*& p) {
    while (*p == ' ') ++p;
    uint64_t v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + static_cast<uint64_t>(*p++ - '0');
    return v;
}
// [p, e) a name; does it equal want (ignoring case), or want + suffix?
bool NameIs(const char* p, const char* e, const char* want, const char* suffix) {
    size_t k = 0;
    for (; want[k]; ++k) if (p + k >= e || LowerC(p[k]) != LowerC(want[k])) return false;
    size_t j = 0;
    for (; suffix[j]; ++j) if (p + k + j >= e || LowerC(p[k + j]) != suffix[j]) return false;
    return p + k + j == e;
}
// The "S" line for want (exact, then with "_thumbnail"); returns a pointer just past the name.
const char* LiveFind(const LiveIndex& ix, const char* want, char* name, int cap) {
    for (int pass = 0; pass < 2; ++pass) {
        const char* suffix = pass ? "_thumbnail" : "";
        const char* p = ix.data;
        while (*p) {
            if (p[0] == 'S' && p[1] == ' ') {
                const char* s = p + 2; const char* e = s; while (*e && *e != ' ' && *e != '\n') ++e;
                if (NameIs(s, e, want, suffix) && e - s < cap) {
                    if (name) { memcpy(name, s, static_cast<size_t>(e - s)); name[e - s] = 0; }
                    return e;
                }
            }
            while (*p && *p != '\n') ++p;
            if (*p) ++p;
        }
    }
    return nullptr;
}
// The line "<kind> <id> ..." -> pointer just past the id.
const char* LiveLine(const LiveIndex& ix, char kind, uint64_t id) {
    const char* p = ix.data;
    while (*p) {
        if (p[0] == kind && p[1] == ' ') {
            const char* q = p + 2;
            if (ParseU(q) == id && *q == ' ') return q;
        }
        while (*p && *p != '\n') ++p;
        if (*p) ++p;
    }
    return nullptr;
}

// LZ4 block format (as Unity writes its bundle blocks). False on any malformed input.
bool Lz4(const uint8_t* s, size_t n, uint8_t* d, size_t want) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t t = s[i++];
        size_t lit = t >> 4;
        if (lit == 15) { uint8_t b; do { if (i >= n) return false; b = s[i++]; lit += b; } while (b == 255); }
        if (i + lit > n || o + lit > want) return false;
        memcpy(d + o, s + i, lit); i += lit; o += lit;
        if (i >= n) break;
        if (i + 2 > n) return false;
        size_t off = s[i] | (static_cast<size_t>(s[i + 1]) << 8); i += 2;
        size_t ml = t & 15;
        if (ml == 15) { uint8_t b; do { if (i >= n) return false; b = s[i++]; ml += b; } while (b == 255); }
        ml += 4;
        if (off == 0 || off > o || o + ml > want) return false;
        for (size_t k = 0; k < ml; ++k, ++o) d[o] = d[o - off];
    }
    return o == want;
}
uint32_t Be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
uint64_t Be64(const uint8_t* p) { return (uint64_t{Be32(p)} << 32) | Be32(p + 4); }

// One bundle's block table, kept for the last bundle used (the worker is single-threaded).
constexpr uint32_t kMaxBlocks = 8192, kMaxBlock = 512 * 1024, kMaxInfo = 256 * 1024;
struct BundleBlocks {
    char path[200]; bool ok; uint32_t n; uint64_t data;
    uint32_t us[kMaxBlocks], cs[kMaxBlocks]; uint16_t fl[kMaxBlocks]; uint64_t ustart[kMaxBlocks], fstart[kMaxBlocks];
};
BundleBlocks g_bb;
uint8_t g_raw[kMaxBlock];                       // compressed bytes being read
uint8_t g_info[kMaxInfo];
struct BlockSlot { uint32_t gen, k; bool used; uint8_t b[kMaxBlock]; };
BlockSlot g_slot[2];
uint32_t g_bb_gen;
int g_live_err;                                 // last failure (for the CARGO diag line)
int g_live_drawn, g_live_failed;
bool NewArchiveFlags(const char* v) {           // Unity changed the meaning of flag 0x200 in these versions
    uint32_t a[3] = {0, 0, 0}; int k = 0;
    for (const char* p = v; *p && k < 3; ++p) { if (*p >= '0' && *p <= '9') a[k] = a[k] * 10 + static_cast<uint32_t>(*p - '0'); else if (*p == '.') ++k; else break; }
    if (a[0] < 2020) return false;
    if (a[0] == 2020) return a[1] > 3 || (a[1] == 3 && a[2] >= 34);
    if (a[0] == 2021) return a[1] > 3 || (a[1] == 3 && a[2] >= 2);
    if (a[0] == 2022) return a[1] > 1 || (a[1] == 1 && a[2] >= 1);
    return true;
}
int64_t g_live_size[2] = {-1, -1};             // what the two sources said about the last bundle's size
bool OpenBundle(const EdenDsmodHostApi* h, const char* rel, uint64_t fsize) {
    // the game as it runs (update applied) first, then the base game the index was made from
    static const char* const kSrc[2] = {"romfs:", "base:"};
    for (int k = 0; k < 2; ++k) {
        Buf c; c.add(kSrc[k]).add(rel);
        if (g_bb.ok && eq(g_bb.path, c.s)) return true;
    }
    g_bb.ok = false; ++g_bb_gen;
    const char* path = nullptr;
    for (int k = 0; k < 2 && !path; ++k) {
        Buf c; c.add(kSrc[k]).add(rel);
        const size_t sz = h->read_romfs(h->userdata, c.s, 0, nullptr, 0);   // no buffer: the file's size
        g_live_size[k] = static_cast<int64_t>(sz);
        if (sz == fsize) { DcCopy(g_bb.path, sizeof g_bb.path, c.s); path = g_bb.path; }
    }
    if (!path) { g_live_err = (g_live_size[0] > 0 || g_live_size[1] > 0) ? 13 : 10; return false; }
    uint8_t hd[512];
    size_t got = h->read_romfs(h->userdata, path, 0, hd, sizeof hd);
    if (got < 64) { g_live_err = 10; return false; }
    const char sig[] = "UnityFS";
    for (int k = 0; k < 8; ++k) if (hd[k] != static_cast<uint8_t>(sig[k])) { g_live_err = 11; return false; }
    uint32_t ver = Be32(hd + 8);
    size_t i = 12; const char* unity;
    while (i < got && hd[i]) ++i;               // player version
    unity = reinterpret_cast<const char*>(hd + ++i);
    while (i < got && hd[i]) ++i;
    ++i;
    if (i + 20 > got) { g_live_err = 12; return false; }
    uint64_t total = Be64(hd + i); uint32_t csize = Be32(hd + i + 8), usize = Be32(hd + i + 12), flags = Be32(hd + i + 16);
    i += 20;
    if (total != fsize) { g_live_err = 13; return false; }      // not the bundle the index was made from
    const bool nf = NewArchiveFlags(unity);
    if (flags & (nf ? 0x1400u : 0x200u)) { g_live_err = 14; return false; }   // encrypted
    if (ver >= 7) i = (i + 15) & ~size_t{15};
    const uint64_t start = i;
    const uint64_t at = (flags & 0x80) ? total - csize : start;
    if (csize > kMaxBlock || usize > kMaxInfo || usize < 24) { g_live_err = 15; return false; }
    if (h->read_romfs(h->userdata, path, at, g_raw, csize) != csize) { g_live_err = 16; return false; }
    const uint32_t comp = flags & 0x3F;
    if (comp == 0) { if (csize != usize) { g_live_err = 17; return false; } memcpy(g_info, g_raw, csize); }
    else if (comp == 2 || comp == 3) { if (!Lz4(g_raw, csize, g_info, usize)) { g_live_err = 17; return false; } }
    else { g_live_err = 18; return false; }                       // LZMA: not supported
    uint32_t nb = Be32(g_info + 16);
    const size_t step = ver >= 9 ? 18 : 10;
    if (nb > kMaxBlocks || 20 + size_t{nb} * step > usize) { g_live_err = 19; return false; }
    uint64_t u = 0, f = start + ((flags & 0x80) ? 0 : csize);
    if (nf && (flags & 0x200)) f = (f + 15) & ~uint64_t{15};
    g_bb.data = f;
    for (uint32_t k = 0; k < nb; ++k) {
        const uint8_t* b = g_info + 20 + k * step;
        g_bb.us[k] = Be32(b); g_bb.cs[k] = Be32(b + 4); g_bb.fl[k] = static_cast<uint16_t>((b[8] << 8) | b[9]);
        if (g_bb.us[k] > kMaxBlock || g_bb.cs[k] > kMaxBlock) { g_live_err = 20; return false; }
        g_bb.ustart[k] = u; g_bb.fstart[k] = f; u += g_bb.us[k]; f += g_bb.cs[k];
    }
    g_bb.n = nb; g_bb.ok = true;
    return true;
}
// n bytes at offset o of the open bundle's (uncompressed) data stream (may cross a block edge).
bool ReadStream(const EdenDsmodHostApi* h, uint64_t o, uint8_t* out, uint32_t n) {
    while (n) {
        uint32_t lo = 0, hi = g_bb.n;
        while (hi - lo > 1) { uint32_t m = (lo + hi) / 2; if (g_bb.ustart[m] <= o) lo = m; else hi = m; }
        if (!g_bb.n || o < g_bb.ustart[lo] || o >= g_bb.ustart[lo] + g_bb.us[lo]) { g_live_err = 21; return false; }
        BlockSlot* s = nullptr;
        for (auto& c : g_slot) if (c.used && c.gen == g_bb_gen && c.k == lo) s = &c;
        if (!s) {
            static int flip; s = &g_slot[flip ^= 1];
            s->used = false;
            const uint32_t cs = g_bb.cs[lo], us = g_bb.us[lo], c = g_bb.fl[lo] & 0x3F;
            if (h->read_romfs(h->userdata, g_bb.path, g_bb.fstart[lo], g_raw, cs) != cs) { g_live_err = 22; return false; }
            if (c == 0) { if (cs != us) { g_live_err = 23; return false; } memcpy(s->b, g_raw, cs); }
            else if (c == 2 || c == 3) { if (!Lz4(g_raw, cs, s->b, us)) { g_live_err = 23; return false; } }
            else { g_live_err = 24; return false; }
            s->used = true; s->gen = g_bb_gen; s->k = lo;
        }
        const uint64_t at = o - g_bb.ustart[lo];
        uint32_t take = static_cast<uint32_t>(g_bb.us[lo] - at); if (take > n) take = n;
        memcpy(out, s->b + at, take); out += take; o += take; n -= take;
    }
    return true;
}
// Switch block-linear: where block (bx, by) of a texture bxn blocks wide sits (UnityPy's deswizzle).
uint64_t BlockIndex(uint32_t bx, uint32_t by, uint32_t bxn, uint32_t gobs) {
    if (!gobs) return uint64_t{by} * bxn + bx;
    const uint32_t gx = bx & 3, gy = by & 7, j = bx >> 2, gyall = by >> 3, i = gyall / gobs, k = gyall % gobs;
    const uint32_t v = ((gx & 2) << 3) | ((gy & 6) << 1) | ((gx & 1) << 1) | (gy & 1);
    return ((uint64_t{i} * (bxn >> 2) + j) * gobs + k) * 32 + v;
}
EdenDsmodAstcDecoder g_astc;
void* g_astc_user;
constexpr uint32_t kLiveMax = 512;              // largest picture read this way (pixels per side)
constexpr uint32_t kRegionMax = kLiveMax + 12;
uint8_t g_blocks[(kRegionMax / 4 + 1) * (kRegionMax / 4 + 1) * 16];
uint8_t g_region[kRegionMax * kRegionMax * 4];
uint8_t g_live_px[kLiveMax * kLiveMax * 4];
// Decode picture `name` into RGBA rows (top row first). w/h out.
// 0.37.49: `sub` = {x, y, w, h} decodes only that piece of the picture (top-down, unrotated pictures
// only), so one bigger than kLiveMax (the title logo) can be read piece by piece; `full` gets the
// whole picture's size, and sub w = 0 only asks for it.
bool LiveDecode(const EdenDsmodHostApi* h, const char* want, uint32_t* ow, uint32_t* oh,
                const uint32_t* sub = nullptr, uint32_t* full = nullptr) {
    if (!LiveLoad(h, g_live_worker)) { g_live_err = 1; return false; }
    const char* p = LiveFind(g_live_worker, want, nullptr, 128);
    if (!p) { g_live_err = 2; return false; }
    const uint64_t t = ParseU(p);
    uint64_t x = ParseU(p), y = ParseU(p), w = ParseU(p), hh = ParseU(p);
    const uint64_t rot = ParseU(p);
    if (full) { full[0] = static_cast<uint32_t>(w); full[1] = static_cast<uint32_t>(hh); }
    if (sub) {
        if (rot != 0) { g_live_err = 25; return false; }
        if (!sub[2]) return true;                              // size only
        if (!sub[3] || sub[0] + sub[2] > w || sub[1] + sub[3] > hh) { g_live_err = 26; return false; }
        x += sub[0]; y += hh - sub[1] - sub[3];                // rows are stored bottom-up
        w = sub[2]; hh = sub[3];
    }
    const char* q = LiveLine(g_live_worker, 'T', t);
    if (!q) { g_live_err = 3; return false; }
    const uint64_t b = ParseU(q), off = ParseU(q), size = ParseU(q), tw = ParseU(q), th = ParseU(q), fmt = ParseU(q), gobs = ParseU(q);
    const char* r = LiveLine(g_live_worker, 'B', b);
    if (!r) { g_live_err = 4; return false; }
    while (*r == ' ') ++r;
    Buf path;
    while (*r && *r != ' ' && *r != '\n' && *r != '\r') path.addc(*r++);
    const uint64_t fsize = ParseU(r);
    uint32_t bw, bh; bool astc = true;
    if (fmt == 4) { bw = 4; bh = 1; astc = false; }
    else if (fmt >= 48 && fmt <= 59) { const uint32_t s[6] = {4, 5, 6, 8, 10, 12}; bw = bh = s[(fmt - 48) % 6]; }
    else { g_live_err = 5; return false; }
    if (!w || !hh || w > kLiveMax || hh > kLiveMax || x + w > tw || y + hh > th || (astc && !g_astc)) { g_live_err = 6; return false; }
    const uint32_t bxn = gobs ? static_cast<uint32_t>((tw + bw * 4 - 1) / (bw * 4) * 4) : static_cast<uint32_t>((tw + bw - 1) / bw);
    const uint32_t bx0 = static_cast<uint32_t>(x / bw), by0 = static_cast<uint32_t>(y / bh);
    const uint32_t bx1 = static_cast<uint32_t>((x + w + bw - 1) / bw), by1 = static_cast<uint32_t>((y + hh + bh - 1) / bh);
    const uint32_t nbx = bx1 - bx0, nby = by1 - by0, rw = nbx * bw, rh = nby * bh;
    if (size_t{nbx} * nby * 16 > sizeof g_blocks || size_t{rw} * rh * 4 > sizeof g_region) { g_live_err = 7; return false; }
    if (!OpenBundle(h, path.s, fsize)) return false;
    uint8_t* d = g_blocks;
    for (uint32_t by = by0; by < by1; ++by)
        for (uint32_t bx = bx0; bx < bx1; ++bx, d += 16) {
            const uint64_t at = BlockIndex(bx, by, bxn, static_cast<uint32_t>(gobs)) * 16;
            if (at + 16 > size || !ReadStream(h, off + at, d, 16)) { if (at + 16 > size) g_live_err = 8; return false; }
        }
    if (astc) {
        if (!g_astc(g_astc_user, rw, rh, bw, bh, g_blocks, size_t{nbx} * nby * 16, g_region, size_t{rw} * rh * 4)) { g_live_err = 9; return false; }
    } else {
        // RGBA32: each 16-byte "block" is 4 pixels of one row
        for (uint32_t by = 0; by < nby; ++by)
            for (uint32_t bx = 0; bx < nbx; ++bx) memcpy(g_region + (size_t{by} * rw + bx * 4) * 4, g_blocks + (size_t{by} * nbx + bx) * 16, 16);
    }
    // crop (rows bottom-up, as stored), then the packing rotation, then flip to top-down (UnityPy)
    const uint32_t cx = static_cast<uint32_t>(x - bx0 * bw), cy = static_cast<uint32_t>(y - by0 * bh);
    const uint32_t W = static_cast<uint32_t>(w), H = static_cast<uint32_t>(hh);
    const uint32_t fw = rot == 4 ? H : W, fh = rot == 4 ? W : H;
    for (uint32_t v = 0; v < fh; ++v)
        for (uint32_t u = 0; u < fw; ++u) {
            const uint32_t bv = fh - 1 - v;          // final flip
            uint32_t ax = u, ay = bv;                // B(u, bv) = A(ax, ay)
            if (rot == 1) ax = W - 1 - u;
            else if (rot == 2) ay = H - 1 - bv;
            else if (rot == 3) { ax = W - 1 - u; ay = H - 1 - bv; }
            else if (rot == 4) { ax = bv; ay = H - 1 - u; }
            memcpy(g_live_px + (size_t{v} * fw + u) * 4, g_region + (size_t{cy + ay} * rw + cx + ax) * 4, 4);
        }
    *ow = fw; *oh = fh;
    return true;
}
// ---------------------------------------------------------------- the game's own font (0.24.0)
// pictures.idx says where the game's TextMeshPro font sits (build_picture_index.py; positions only):
//   F <name> <texture> <kind> <glyph table> <glyphs> <stride> <character table> <characters> <stride> <sdf> <padding>
// Table positions are in the data stream of the texture's bundle. kind 1 = TMP 1.4+ (glyph table +
// character table, rects from the bottom left), 2 = the older TMP_Glyph list (rects from the top).
// "module:fontdata" -> the glyph metrics (decode_font), "module:font" -> the atlas: the glyphs the
// companion draws (U+0020..U+00FF) cut out of the game's atlas, made solid from the distance field,
// scaled to kFontCap pixels per capital letter and packed into one small picture.
constexpr uint32_t kFontFirst = 0x20, kFontLast = 0xFF, kFontN = kFontLast - kFontFirst + 1;
constexpr uint32_t kFontCap = 34;               // capital height in the packed atlas (pixels)
constexpr uint32_t kFontMargin = 2;             // source pixels kept around each glyph (its soft edge)
constexpr uint32_t kFontAtlasW = 1024, kFontAtlasH = 1024, kFontSrcMax = 512;
struct FontSrc { int32_t x, y, w, h; float bx, by, adv; bool has; uint32_t atlas; };   // atlas: TMP atlas texture index
struct FontState {
    bool ready; int err; uint32_t tries;
    uint64_t toff, tsize; uint32_t tw, th, fmt, gobs; int sdf, pad;
    char bundle[200]; uint64_t bsize; float s;
    uint32_t gi[kFontN];                        // glyph index per character (kind 1)
    FontSrc src[kFontN];
    EdenDsmodFontGlyph out[kFontN]; uint32_t line, atlas_h;
};
FontState g_font;
FontState g_hud;                                // the HUD's digital font (Snowstorm), for the DIVE cards
bool g_font_apos;                               // the game font is in and has an apostrophe
int g_live_lock;                                // the bundle reader is shared: pictures (asset worker) and the font
struct LiveLock {
    LiveLock() { while (__atomic_exchange_n(&g_live_lock, 1, __ATOMIC_ACQUIRE)) {} }
    ~LiveLock() { __atomic_store_n(&g_live_lock, 0, __ATOMIC_RELEASE); }
};
int32_t RoundF(float v) { return v >= 0 ? static_cast<int32_t>(v + 0.5f) : -static_cast<int32_t>(-v + 0.5f); }
int32_t CeilF(float v) { const int32_t i = static_cast<int32_t>(v); return static_cast<float>(i) < v ? i + 1 : i; }
float F32(const uint8_t* p) { float f; memcpy(&f, p, 4); return f; }
uint32_t U32(const uint8_t* p) { uint32_t u; memcpy(&u, p, 4); return u; }
// `want`: the F line whose font name starts with it (nullptr: the first F line).
// cap: the capital height wanted (pixels); grow: may be enlarged to it
bool FontLoadInto(const EdenDsmodHostApi* h, FontState& f, const char* want, bool grow = false, uint32_t want_cap = kFontCap) {
    if (__atomic_load_n(&f.ready, __ATOMIC_ACQUIRE)) return true;
    ++f.tries;
    if (!LiveLoad(h, g_live_worker)) { f.err = 1; return false; }
    const char* p = g_live_worker.data; const char* q = nullptr;
    while (*p && !q) {
        if (p[0] == 'F' && p[1] == ' ') {
            const char* n = p + 2;
            while (*n == ' ') ++n;
            int k = 0;
            if (want) while (want[k] && n[k] == want[k]) ++k;
            if (!want || !want[k]) q = p + 2;
        }
        while (*p && *p != '\n') ++p;
        if (*p) ++p;
    }
    if (!q) { f.err = 2; return false; }                       // this index has no font line
    while (*q == ' ') ++q;
    while (*q && *q != ' ' && *q != '\n') ++q;                 // the font's name
    const uint64_t tex = ParseU(q), kind = ParseU(q), go = ParseU(q), gn = ParseU(q), gs = ParseU(q);
    const uint64_t co = ParseU(q), cn = ParseU(q), cs = ParseU(q), sdf = ParseU(q), pad = ParseU(q);
    const char* t = LiveLine(g_live_worker, 'T', tex);
    if (!t) { f.err = 3; return false; }
    const uint64_t b = ParseU(t);
    f.toff = ParseU(t); f.tsize = ParseU(t);
    f.tw = static_cast<uint32_t>(ParseU(t)); f.th = static_cast<uint32_t>(ParseU(t));
    f.fmt = static_cast<uint32_t>(ParseU(t)); f.gobs = static_cast<uint32_t>(ParseU(t));
    f.sdf = static_cast<int>(sdf); f.pad = static_cast<int>(pad);
    if (f.fmt != 1 && f.fmt != 63 && f.fmt != 4) { f.err = 4; return false; }
    if ((kind != 1 && kind != 2) || !gn || gs < (kind == 1 ? 40u : 32u) || (kind == 1 && (!cn || cs < 12))) { f.err = 5; return false; }
    const char* r = LiveLine(g_live_worker, 'B', b);
    if (!r) { f.err = 3; return false; }
    while (*r == ' ') ++r;
    Buf path;
    while (*r && *r != ' ' && *r != '\n' && *r != '\r') path.addc(*r++);
    f.bsize = ParseU(r);
    DcCopy(f.bundle, sizeof f.bundle, path.s);
    if (!OpenBundle(h, f.bundle, f.bsize)) { f.err = 6; return false; }
    for (auto& s : f.src) s.has = false;
    uint8_t e[40];
    if (kind == 1) {
        for (auto& v : f.gi) v = 0xFFFFFFFFu;
        for (uint64_t k = 0; k < cn; ++k) {                    // TMP_Character: type, unicode, glyph index, scale
            if (!ReadStream(h, co + k * cs, e, 12)) { f.err = 7; return false; }
            const uint32_t u = U32(e + 4);
            if (u >= kFontFirst && u <= kFontLast) f.gi[u - kFontFirst] = U32(e + 8);
        }
        uint8_t e48[48] = {};
        for (uint64_t k = 0; k < gn; ++k) {                    // Glyph: index, metrics (5 floats), rect (4 ints), scale, atlas index
            if (!ReadStream(h, go + k * gs, e48, gs >= 48 ? 48 : 40)) { f.err = 7; return false; }
            memcpy(e, e48, 40);
            const uint32_t idx = U32(e);
            for (uint32_t c = 0; c < kFontN; ++c) {
                if (f.gi[c] != idx) continue;
                FontSrc& s = f.src[c];
                s.bx = F32(e + 12); s.by = F32(e + 16); s.adv = F32(e + 20);
                s.x = static_cast<int32_t>(U32(e + 24)); s.y = static_cast<int32_t>(U32(e + 28));
                s.w = static_cast<int32_t>(U32(e + 32)); s.h = static_cast<int32_t>(U32(e + 36));
                s.atlas = gs >= 48 ? U32(e48 + 44) : 0;
                s.has = true;
            }
        }
    } else {
        for (uint64_t k = 0; k < gn; ++k) {                    // TMP_Glyph: id, x, y, w, h, xOffset, yOffset, xAdvance
            if (!ReadStream(h, go + k * gs, e, 32)) { f.err = 7; return false; }
            const int32_t id = static_cast<int32_t>(U32(e));
            if (id < static_cast<int32_t>(kFontFirst) || id > static_cast<int32_t>(kFontLast)) continue;
            FontSrc& s = f.src[id - kFontFirst];
            s.x = RoundF(F32(e + 4)); s.w = RoundF(F32(e + 12)); s.h = RoundF(F32(e + 16));
            s.y = static_cast<int32_t>(f.th) - RoundF(F32(e + 8)) - s.h;     // from the top -> from the bottom
            s.bx = F32(e + 20); s.by = F32(e + 24); s.adv = F32(e + 28);
            s.atlas = 0; s.has = true;
        }
    }
    // the size: capitals kFontCap pixels high (never enlarged)
    const FontSrc& H = f.src['H' - kFontFirst].has ? f.src['H' - kFontFirst] : f.src['A' - kFontFirst];
    const float cap = H.has ? (H.by > 4 ? H.by : static_cast<float>(H.h)) : 0;
    if (cap < 4) { f.err = 8; return false; }
    f.s = static_cast<float>(want_cap) / cap;
    if (f.s > 1 && !grow) f.s = 1;              // (the HUD cells are drawn for kFontCap capitals: grown)
    // pack the glyphs in rows (the same order and sizes the atlas is drawn with)
    const int32_t m = static_cast<int32_t>(kFontMargin);
    uint32_t x = 1, y = 1, rowh = 0;
    const FontSrc& sp = f.src[0];
    const uint16_t space = static_cast<uint16_t>(sp.has && sp.adv > 0 ? RoundF(sp.adv * f.s) : RoundF(cap * f.s * 0.3f));
    for (uint32_t c = 0; c < kFontN; ++c) {
        const FontSrc& s = f.src[c];
        EdenDsmodFontGlyph& o = f.out[c];
        o = EdenDsmodFontGlyph{0, 0, 0, 0, 0, 0, space};      // a character the font lacks: a blank
        if (!s.has) continue;
        o.advance = static_cast<uint16_t>(s.adv > 0 ? RoundF(s.adv * f.s) : 0);
        if (s.w <= 0 || s.h <= 0) continue;
        if (s.w + 2 * m > static_cast<int32_t>(kFontSrcMax) || s.h + 2 * m > static_cast<int32_t>(kFontSrcMax)) { f.err = 9; return false; }
        const uint32_t dw = static_cast<uint32_t>(CeilF(static_cast<float>(s.w + 2 * m) * f.s));
        const uint32_t dh = static_cast<uint32_t>(CeilF(static_cast<float>(s.h + 2 * m) * f.s));
        if (x + dw + 1 > kFontAtlasW) { x = 1; y += rowh + 1; rowh = 0; }
        if (y + dh + 1 > kFontAtlasH) { f.err = 10; return false; }
        o.x = static_cast<uint16_t>(x); o.y = static_cast<uint16_t>(y);
        o.w = static_cast<uint16_t>(dw); o.h = static_cast<uint16_t>(dh);
        o.bearing_x = static_cast<int16_t>(RoundF((s.bx - static_cast<float>(m)) * f.s));
        o.bearing_y = static_cast<int16_t>(RoundF((s.by + static_cast<float>(m)) * f.s));
        x += dw + 1; if (dh > rowh) rowh = dh;
    }
    f.atlas_h = y + rowh + 1;
    f.line = static_cast<uint32_t>(RoundF(cap * f.s));
    f.err = 0;
    __atomic_store_n(&f.ready, true, __ATOMIC_RELEASE);
    return true;
}
bool FontLoad(const EdenDsmodHostApi* h) {
    if (!FontLoadInto(h, g_font, nullptr)) return false;
    g_font_apos = g_font.src['\'' - kFontFirst].has && g_font.src['\'' - kFontFirst].w > 0;
    return true;
}
// One glyph's coverage (0..255), top row first, margin included, read from the game's atlas.
uint8_t g_fsrc[kFontSrcMax * kFontSrcMax];
bool FontGlyphPixels(const EdenDsmodHostApi* h, const FontState& f, const FontSrc& s, uint32_t sw, uint32_t sh) {
    const uint32_t bpp = f.fmt == 4 ? 4 : 1, bw = 16 / bpp;
    const uint32_t bxn = f.gobs ? (f.tw + bw * 4 - 1) / (bw * 4) * 4 : (f.tw + bw - 1) / bw;
    const int32_t m = static_cast<int32_t>(kFontMargin);
    const float grad = static_cast<float>(f.pad + 1);
    uint8_t blk[16];
    for (uint32_t v = 0; v < sh; ++v) {
        const int32_t row = s.y + s.h - 1 + m - static_cast<int32_t>(v);          // stored bottom-up
        int64_t have = -1;
        for (uint32_t u = 0; u < sw; ++u) {
            const int32_t col = s.x - m + static_cast<int32_t>(u);
            uint8_t a = 0;
            if (row >= 0 && col >= 0 && row < static_cast<int32_t>(f.th) && col < static_cast<int32_t>(f.tw)) {
                const uint32_t bx = static_cast<uint32_t>(col) / bw;
                if (static_cast<int64_t>(bx) != have) {
                    const uint64_t at = BlockIndex(bx, static_cast<uint32_t>(row), bxn, f.gobs) * 16;
                    if (at + 16 > f.tsize || !ReadStream(h, f.toff + at, blk, 16)) return false;
                    have = bx;
                }
                a = blk[(static_cast<uint32_t>(col) % bw) * bpp + (bpp == 4 ? 3 : 0)];
            }
            if (f.sdf) {                                                          // distance -> solid edge
                const float c = (static_cast<float>(a) / 255.0f - 0.5f) * grad + 0.5f;
                a = static_cast<uint8_t>(c <= 0 ? 0 : c >= 1 ? 255 : RoundF(c * 255.0f));
            }
            g_fsrc[v * sw + u] = a;
        }
    }
    return true;
}
uint8_t g_fatlas[2][kFontAtlasW * kFontAtlasH * 4];   // per look, kept: switching pages only hands it over again
bool g_fatlas_done[2];
// Character c of font f, made solid and scaled, into the alpha of an RGBA picture W wide at (ox, oy).
bool FontRenderGlyph(const EdenDsmodHostApi* h, FontState& f, uint32_t c, uint8_t* dst, uint32_t W, uint32_t H, int32_t ox, int32_t oy) {
    const EdenDsmodFontGlyph& o = f.out[c];
    if (!o.w || !o.h) return true;
    const FontSrc& s = f.src[c];
    const uint32_t sw = static_cast<uint32_t>(s.w) + 2 * kFontMargin, sh = static_cast<uint32_t>(s.h) + 2 * kFontMargin;
    if (!FontGlyphPixels(h, f, s, sw, sh)) { f.err = 11; return false; }
    const float inv = 1.0f / f.s;
    for (uint32_t dv = 0; dv < o.h; ++dv)
        for (uint32_t du = 0; du < o.w; ++du) {
            const int32_t X = ox + static_cast<int32_t>(du), Y = oy + static_cast<int32_t>(dv);
            if (X < 0 || Y < 0 || X >= static_cast<int32_t>(W) || Y >= static_cast<int32_t>(H)) continue;
            uint32_t sum = 0;                                 // 4 x 4 samples over the source box
            for (int a = 0; a < 4; ++a)
                for (int b = 0; b < 4; ++b) {
                    const uint32_t sx = static_cast<uint32_t>((static_cast<float>(du) + (static_cast<float>(b) + 0.5f) * 0.25f) * inv);
                    const uint32_t sy = static_cast<uint32_t>((static_cast<float>(dv) + (static_cast<float>(a) + 0.5f) * 0.25f) * inv);
                    if (sx < sw && sy < sh) sum += g_fsrc[sy * sw + sx];
                }
            dst[(static_cast<size_t>(Y) * W + static_cast<size_t>(X)) * 4 + 3] = static_cast<uint8_t>(sum / 16);
        }
    return true;
}
// ---- one font per package, two looks (0.27.1): the DIVE page wears the game HUD's digital font
// (pictures.idx F line "Snowstorm"), every other page the game's text font. The module follows the
// page (ui.page, all page changes go through it) and, when the look changes, bumps "__font_epoch":
// the host decodes the font again and reloads the atlas. A character the HUD font lacks (not in
// it, kept on another atlas texture, or blank in the saved atlas) comes from the text font, so
// no letter ever goes missing.
const char* const kHudFont = "Snowstorm";
struct FontMix {
    bool built; int hud_n;
    EdenDsmodFontGlyph out[kFontN];
    uint8_t from[kFontN];                       // 1 = the HUD font
    uint32_t atlas_h;
};
FontMix g_mix[2];
int g_font_want = 1;                            // 0 text font, 1 HUD look (the DIVE page); the tick writes it
int g_data_want = 1;                            // the look the last metrics were handed out for
bool HudHas(const EdenDsmodHostApi* h, uint32_t c) {
    const FontSrc& s = g_hud.src[c];
    if (!s.has || s.atlas != 0) return false;
    if (s.w <= 0 || s.h <= 0) return c == 0 && s.adv > 0;            // only the space may be empty
    const uint32_t sw = static_cast<uint32_t>(s.w) + 2 * kFontMargin, sh = static_cast<uint32_t>(s.h) + 2 * kFontMargin;
    if (!FontGlyphPixels(h, g_hud, s, sw, sh)) return false;
    uint32_t solid = 0;
    for (size_t k = 0; k < size_t{sw} * sh; ++k) if (g_fsrc[k] >= 128) ++solid;
    return solid >= 4;
}
bool BuildMix(const EdenDsmodHostApi* h, int want) {
    FontMix& m = g_mix[want];
    if (m.built) return true;
    if (!FontLoad(h)) return false;
    bool hud = false;
    if (want == 1) {
        hud = FontLoadInto(h, g_hud, kHudFont, true, g_font.line);
        if (!hud && g_hud.tries < 3 && g_hud.err != 2) return false;  // not readable yet: ask again
    }
    uint32_t x = 1, y = 1, rowh = 0;
    m.hud_n = 0;
    for (uint32_t c = 0; c < kFontN; ++c) {
        const bool fh = hud && HudHas(h, c);
        m.from[c] = fh ? 1 : 0; m.hud_n += fh ? 1 : 0;
        EdenDsmodFontGlyph o = (fh ? g_hud : g_font).out[c];
        if (o.w && o.h) {
            if (x + o.w + 1 > kFontAtlasW) { x = 1; y += rowh + 1; rowh = 0; }
            if (y + o.h + 1 > kFontAtlasH) return false;
            o.x = static_cast<uint16_t>(x); o.y = static_cast<uint16_t>(y);
            x += o.w + 1u; if (o.h > rowh) rowh = o.h;
        }
        m.out[c] = o;
    }
    m.atlas_h = y + rowh + 1;
    m.built = true;
    return true;
}
bool FontAtlas(const EdenDsmodHostApi* h, EdenDsmodImageSink sink, void* receiver) {
    const int want = g_data_want;
    if (!BuildMix(h, want)) return false;
    const FontMix& m = g_mix[want];
    const uint32_t W = kFontAtlasW, Hh = m.atlas_h;
    uint8_t* px = g_fatlas[want];
    if (!g_fatlas_done[want]) {
        for (size_t k = 0; k < size_t{W} * Hh; ++k) { px[k * 4] = px[k * 4 + 1] = px[k * 4 + 2] = 255; px[k * 4 + 3] = 0; }
        for (uint32_t c = 0; c < kFontN; ++c)
            if (!FontRenderGlyph(h, m.from[c] ? g_hud : g_font, c, px, W, Hh, m.out[c].x, m.out[c].y)) return false;
        g_fatlas_done[want] = true;
    }
    sink(receiver, W, Hh, px, size_t{W} * Hh * 4);
    return true;
}
// The page on show (the manifest's page order) and the look it wants.
constexpr int kPageDive = 0, kPageSushi = 4, kPagePhone = 7, kPageEco = 10, kPages = 15;   // 0.37.89: + WEAPON SHOP; 0.37.102: + IDIVER; 0.37.109: + CALL  // gen_dive.py's page order (0.37.62: + PHONE, CALCULATOR; 0.37.67: + MAIL; 0.37.72: + ECOWATCHER; DIAGNOSTICS last when shown)
int g_page = kPageDive;
int g_dive_map = 1;                             // DIVE's lower half: 1 the map (0.37.0: always; the missions view is gone)
int64_t g_font_epoch;
// ---------------------------------------------------------------- 0.37.62: the phone's calculator
// Entirely the bottom screen's own: nothing in the game is read or changed. Keys (calc_key argument):
// 0-9 digits, 10 ".", 11 +, 12 -, 13 x, 14 /, 15 =, 16 %, 17 +/-, 18 delete, 19 clear.
struct Calc { double acc = 0; char entry[24] = "0"; int elen = 1; bool entering = false; int op = 0; bool err = false; };
Calc g_calc;
const char* const kCalcOps[5] = {"", "+", "-", "x", "/"};
double CalcParse(const char* t) {
    double v = 0, scale = 0; bool neg = false;
    if (*t == '-') { neg = true; ++t; }
    for (; *t; ++t) {
        if (*t == '.') { scale = 1; continue; }
        v = v * 10 + (*t - '0');
        if (scale > 0) scale *= 10;
    }
    if (scale > 0) v /= scale;
    return neg ? -v : v;
}
// 12 significant digits at most, no trailing zeros; false when too big to show
bool CalcFormat(double v, Buf& b) {
    if (v != v || v >= 1e15 || v <= -1e15) return false;
    const bool neg = v < 0; const double a = neg ? -v : v;
    uint64_t ip = static_cast<uint64_t>(a); double frac = a - static_cast<double>(ip);
    int nint = 0; for (uint64_t t = ip; t; t /= 10) ++nint;
    int fd = 12 - nint; if (fd < 0) fd = 0; if (fd > 10) fd = 10;
    uint64_t scale = 1; for (int k = 0; k < fd; ++k) scale *= 10;
    uint64_t f = static_cast<uint64_t>(frac * static_cast<double>(scale) + 0.5);
    if (f >= scale) { ++ip; f -= scale; }
    while (fd > 0 && f % 10 == 0) { f /= 10; --fd; }
    if (neg && (ip || f)) b.addc('-');
    b.num(static_cast<int64_t>(ip));
    if (fd > 0) {
        char t[12]; for (int k = fd - 1; k >= 0; --k) { t[k] = static_cast<char>('0' + f % 10); f /= 10; }
        b.addc('.'); for (int k = 0; k < fd; ++k) b.addc(t[k]);
    }
    return true;
}
void CalcSetEntry(double v) {
    Buf b;
    if (!CalcFormat(v, b)) { g_calc.err = true; return; }
    int k = 0; for (; b.s[k] && k < 22; ++k) g_calc.entry[k] = b.s[k];
    g_calc.entry[k] = 0; g_calc.elen = k;
}
int CalcDigits() { int d = 0; for (int k = 0; k < g_calc.elen; ++k) if (g_calc.entry[k] >= '0' && g_calc.entry[k] <= '9') ++d; return d; }
double CalcApply(double a, int op, double b) {
    switch (op) { case 1: return a + b; case 2: return a - b; case 3: return a * b;
                  case 4: if (b == 0) { g_calc.err = true; return 0; } return a / b; }
    return b;
}
bool CalcKey(int64_t k) {
    Calc& c = g_calc;
    if (k < 0 || k > 19) return false;
    if (k == 19 || (c.err && k != 18)) {               // clear (any key after an error starts over, then counts)
        c = Calc{};
        if (k == 19) return true;
    }
    if (k <= 10) {                                      // digits and the point
        if (!c.entering) { c.entry[0] = '0'; c.entry[1] = 0; c.elen = 1; c.entering = true; }
        if (k == 10) {
            for (int i = 0; i < c.elen; ++i) if (c.entry[i] == '.') return true;
            if (c.elen < 22) { c.entry[c.elen++] = '.'; c.entry[c.elen] = 0; }
            return true;
        }
        if (CalcDigits() >= 12) return true;
        if (c.elen == 1 && c.entry[0] == '0') c.elen = 0;
        else if (c.elen == 2 && c.entry[0] == '-' && c.entry[1] == '0') c.elen = 1;
        c.entry[c.elen++] = static_cast<char>('0' + k); c.entry[c.elen] = 0;
        return true;
    }
    if (k >= 11 && k <= 14) {                           // an operation: finish the one before it
        if (c.entering) {
            const double v = CalcParse(c.entry);
            c.acc = c.op ? CalcApply(c.acc, c.op, v) : v;
            c.entering = false;
            if (!c.err) CalcSetEntry(c.acc);
        }
        c.op = static_cast<int>(k - 10);
        return true;
    }
    if (k == 15) {                                      // =
        if (c.op) {
            const double v = c.entering ? CalcParse(c.entry) : c.acc;
            c.acc = CalcApply(c.acc, c.op, v);
            if (!c.err) CalcSetEntry(c.acc);
        }
        c.op = 0; c.entering = false;
        return true;
    }
    if (k == 16) {                                      // %: of the number before (+ / -), else a hundredth
        const double v = CalcParse(c.entry);
        CalcSetEntry(c.op == 1 || c.op == 2 ? c.acc * v / 100 : v / 100);
        c.entering = true;
        return true;
    }
    if (k == 17) {                                      // +/-
        if (c.elen == 1 && c.entry[0] == '0') return true;
        if (c.entry[0] == '-') { for (int i = 0; i < c.elen; ++i) c.entry[i] = c.entry[i + 1]; --c.elen; }
        else if (c.elen < 22) { for (int i = c.elen; i >= 0; --i) c.entry[i + 1] = c.entry[i]; c.entry[0] = '-'; ++c.elen; }
        if (!c.entering) c.acc = CalcParse(c.entry);
        return true;
    }
    if (k == 18) {                                      // delete the last digit typed
        if (c.err) { c = Calc{}; return true; }
        if (!c.entering) return true;
        --c.elen; c.entry[c.elen] = 0;
        if (c.elen == 0 || (c.elen == 1 && c.entry[0] == '-')) { c.entry[0] = '0'; c.entry[1] = 0; c.elen = 1; }
        return true;
    }
    return false;
}
void PublishCalc(const EdenDsmodHostApi* h) {
    const Calc& c = g_calc;
    h->publish_text(h->userdata, "calc.disp", c.err ? "Error" : c.entry);
    Buf e;
    if (c.op && !c.err) { CalcFormat(c.acc, e); e.add(" ").add(kCalcOps[c.op]); }
    h->publish_text(h->userdata, "calc.expr", e.s);
    h->publish_i64(h->userdata, "calc.op", c.err ? 0 : c.op);
    // 0.37.63: the whole sum on one line, as the game's calculator shows it ("2+2"); x and / as the game's signs
    Buf ln;
    if (c.err) ln.add("Error");
    else {
        if (c.op) {
            CalcFormat(c.acc, ln);
            // U+00D7 for x; 0.37.71: "/" for divide, as the game's own calculator shows it ("9/9") -- the game
            // font has no U+00F7, so it drew nothing
            ln.add(c.op == 3 ? "\xC3\x97" : c.op == 4 ? "/" : kCalcOps[c.op]);
            if (c.entering) ln.add(c.entry);
        } else ln.add(c.entry);
    }
    int chars = 0; for (int i = 0; ln.s[i]; ++i) if ((ln.s[i] & 0xC0) != 0x80) ++chars;
    h->publish_text(h->userdata, "calc.line", ln.s);
    h->publish_i64(h->userdata, "calc.long", chars > 15 ? 1 : 0);   // a smaller size for long sums
}
void PublishPage(const EdenDsmodHostApi* h) {
    const int want = 1;                          // 0.27.2: every page wears the HUD's lettering
    if (want != __atomic_load_n(&g_font_want, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_font_want, want, __ATOMIC_RELEASE);
        ++g_font_epoch;
    }
    h->publish_i64(h->userdata, "ui.page", g_page);
    h->publish_i64(h->userdata, "dive.map", g_dive_map);
    h->publish_i64(h->userdata, "__font_epoch", g_font_epoch);
    PublishCalc(h);
}
bool PictureName(const EdenDsmodHostApi* h, const char* want, char* out, int cap) {
    if (LiveLoad(h, g_live_tick) && LiveFind(g_live_tick, want, out, cap)) return true;
    return ResolveIcon(h, g_ix_tick, want, out, cap);
}

// A character's face among the exported pictures: the icon name the character's data holds
// ("Mission_Cobra"), else likely patterns around its key ("Cobra").
bool NpcPicture(const EdenDsmodHostApi* h, const char* pic, const char* key, char* out, int cap) {
    if (pic[0] && PictureName(h, pic, out, cap)) return true;
    static const char* const kPat[][2] = {
        {"Mission_", ""},
        {"", "_Profile"}, {"Profile_", ""}, {"", "_Portrait"}, {"Portrait_", ""}, {"", "_Face"},
        {"Face_", ""}, {"NPC_", ""}, {"Icon_", ""}, {"", "_Icon"}, {"Icon_NPC_", ""}, {"NPC_Icon_", ""},
        {"", "_Profile_01"}, {"", "_Mission"}, {"Mission_", ""}, {"", ""},
    };
    for (const auto& p : kPat) {
        Buf w; w.add(p[0]).add(key).add(p[1]);
        if (PictureName(h, w.s, out, cap)) return true;
    }
    return false;
}

// Picture diagnostics (once a second): is the user: folder there, and can the first row's
// picture be read? Shown on CARGO so a missing or misplaced icons folder is obvious.
char g_icon_diag[240] = "pictures: not checked yet";
void IconDiag(const Instance&, const EdenDsmodHostApi* h) {
    // which source the pictures come from: the game files (pictures.idx) or exported ones
    Buf b;
    b.add("pictures: game files ").add(LiveLoad(h, g_live_tick) ? "indexed" : "no index");
    b.add(", drawn ").num(g_live_drawn).add(", not drawn ").num(g_live_failed);
    if (g_live_failed) b.add(" (code ").num(g_live_err).add(", sizes ").num(g_live_size[0]).add(" / ").num(g_live_size[1]).add(")");
    uint8_t t[4];
    const bool exp = h->read_romfs && (h->read_romfs(h->userdata, "file:icons/_index.txt", 0, t, sizeof t) == sizeof t ||
                                       h->read_romfs(h->userdata, "user:icons/_index.txt", 0, t, sizeof t) == sizeof t);
    b.add(exp ? "; exported pictures present" : "; no exported pictures");
    b.add("; font ");
    if (g_font.ready) b.add("game (").num(g_font.line).add(" px capitals)");
    else if (g_font.tries) b.add("own (game font not read, code ").num(g_font.err).add(")");
    else b.add("own (game font not asked for yet)");
    b.add("; DIVE font ");
    if (g_mix[1].built && g_hud.ready) b.add("game HUD (").num(g_mix[1].hud_n).add(" of ").num(kFontN).add(" characters, the rest from the text font)");
    else if (g_hud.tries) b.add("text font (HUD font not read, code ").num(g_hud.err).add(")");
    else b.add("not asked for yet");
    int n2 = b.n < 239 ? b.n : 239;
    memcpy(g_icon_diag, b.s, static_cast<size_t>(n2)); g_icon_diag[n2] = 0;
}

// Where the player is (round 18), for switching the bottom screen by itself: 1 = diving
// (InGameManager), 2 = the sushi bar (SushiBarManager), 0 = elsewhere. Both are Unity
// Singleton<T>s: TypeInfo -> parent +0x58 -> statics +0xB8 -> instance; a destroyed one keeps its
// managed object but loses the native pointer (m_CachedPtr @0x10).
constexpr uint64_t kSushiBarManagerTypeInfo = 0x9DB6C78;   // (kInGameManagerTypeInfo: above)
bool PatchLive();
bool SingletonAlive(const Instance& in, const Guest& g, uint64_t typeinfo, const char* cls_name) {
    uint64_t cls = g.ptr(in.main_base + typeinfo);
    uint64_t parent = cls ? g.ptr(cls + kParent) : 0;
    uint64_t st = parent ? g.ptr(parent + kStatics) : 0;
    uint64_t obj = st ? g.ptr(st) : 0;
    char c[32];
    return obj && g.ptr(obj + 0x10) && g.cls(obj, c, sizeof c) && eq(c, cls_name);
}
int g_mode;
int g_logo_err = -1, g_logo_w, g_logo_h;
int g_bg_err = -1, g_bg_w, g_bg_h;          // 0.37.59: the title menu's illustration behind it (BgImage)   // 0.37.49: the title logo (LogoImage); -1 = not asked for yet
int g_ld_test;                               // 0.37.49: DIAG "TITLE TEST" shows the loading picture by hand
// 0.37.51 (round 37): SceneLoader's static IsSceneLoading (static fields +0x0) -- set while the game
// changes scenes behind its loading screen. g_ld_on = now; the rest is for the DIAG title line.
constexpr uint64_t kSceneLoaderTypeInfo = 0x9DB55C8;
constexpr uint64_t kSceneMgrTypeInfo = 0x9DB55D8;       // 0.37.52: SceneLoaderManagedBehaviour
int g_ld_title, g_ld_menu, g_scene_seen, g_save_seen; char g_scene_name[96], g_scene_data[40];
void DcCopy(char* dst, int cap, const char* src);
int g_ld_on, g_ld_ok, g_ld_count; uint64_t g_ld_start, g_ld_last_ticks;
void LoadingStep(const Instance& in, const Guest& g) {
    const uint64_t cls = g.ptr(in.main_base + kSceneLoaderTypeInfo);
    char c[32];
    const uint64_t st = cls ? g.ptr(cls + kStatics) : 0;
    uint8_t v = 0;
    g_ld_ok = st && g.cstr(g.ptr(cls + 0x10), c, sizeof c) && eq(c, "SceneLoader") && g.read(st, &v, 1);
    const int on = (g_ld_ok && v == 1) ? 1 : 0;
    if (on && !g_ld_on) { g_ld_start = g_ticks; ++g_ld_count; }
    if (!on && g_ld_on) g_ld_last_ticks = g_ticks - g_ld_start;
    g_ld_on = on;
    // 0.37.52 (round 38): the scene's own manager -- SceneLoaderManagedBehaviour.Current (static +0x0). The title
    // menu is DR.Title.TitleManager; before the first scene has a manager (the start-up screen) it is empty.
    const uint64_t mc = g.ptr(in.main_base + kSceneMgrTypeInfo);
    char mn[40] = "";
    const bool mname = mc && g.cstr(g.ptr(mc + 0x10), mn, sizeof mn);
    const uint64_t ms = (mname && eq(mn, "SceneLoaderManagedBehaviour")) ? g.ptr(mc + kStatics) : 0;
    const uint64_t cur = ms ? g.ptr(ms) : 0;
    char nm[40] = "", ns[24] = "";
    const uint64_t k = cur ? g.ptr(cur) : 0;
    if (k) { g.cstr(g.ptr(k + 0x10), nm, sizeof nm); g.cstr(g.ptr(k + 0x18), ns, sizeof ns); }
    if (nm[0]) g_scene_seen = 1;
    // 0.37.53: why it could not be read (0.37.52 showed "?" everywhere): c = no class, n = another class, s = no statics
    { Buf w;
      if (!mc) w.add("?c");
      else if (!mname || !eq(mn, "SceneLoaderManagedBehaviour")) w.add("?n ").add(mname ? mn : "-");
      else if (!ms) w.add("?s");
      else w.add(nm[0] ? nm : "none");
      // SceneLoader's static CurrentSceneData (+0x18, a GameScene): its class and the first text it holds
      const uint64_t sd = st ? g.ptr(st + 0x18) : 0;
      char dc[32] = "", txt[40] = "";
      if (sd && g.cls(sd, dc, sizeof dc)) {
          for (int o = 0x10; o < 0x60 && !txt[0]; o += 8) {
              const uint64_t q = g.ptr(sd + o); char qc[16];
              if (q && g.cls(q, qc, sizeof qc) && eq(qc, "String")) g.str(q, txt, sizeof txt);
          }
      }
      w.add(" data ").add(dc[0] ? dc : "-");
      if (txt[0]) w.add(" '").add(txt).addc('\'');
      DcCopy(g_scene_name, sizeof g_scene_name, w.s);
      DcCopy(g_scene_data, sizeof g_scene_data, txt); }
    const bool title = (eq(nm, "TitleManager") && eq(ns, "DR.Title")) || eq(g_scene_data, "DR_Title");
    // 0.37.53: the start-up screens come before the save is found; a second guard beside the scene manager
    if (in.gs) g_save_seen = 1;
    const bool startup = (ms && !cur && !g_scene_seen) || !g_save_seen;
    // 0.37.54: one "front of the game" spell from start-up through the title menu to the first scene of play, so the
    // short gaps between them (end of the swimming Dave, after Continue) keep the logo. It starts at start-up or on
    // the title menu and ends once no load is running and the scene is neither the title nor a start-up
    // scene, or when a dive / the sushi bar is running.
    static int front = 1;
    if (title || startup) front = 1;
    auto has = [](const char* a, const char* w) { for (; *a; ++a) if (starts(a, w)) return true; return false; };
    const bool boot_name = starts(g_scene_data, "DR_") &&
        (has(g_scene_data, "Title") || has(g_scene_data, "Logo") || has(g_scene_data, "Entry") ||
         has(g_scene_data, "Intro") || has(g_scene_data, "Splash") || has(g_scene_data, "Boot"));
    // (by state, not by the moment the load ends: the scene's name may change a little after the load flag drops)
    if (g_mode != 0) front = 0;
    else if (!g_ld_on && g_scene_data[0] && !title && !boot_name && g_save_seen) front = 0;
    g_ld_title = front && g_mode == 0 ? 1 : 0;                           // never over a dive or the sushi bar
    g_ld_menu = title && !g_ld_on && g_mode == 0 ? 1 : 0;              // 0.38.41: the title menu itself (CONTINUE button)
}
void ScanMode(const Instance& in, const Guest& g) {
    g_mode = (PatchLive() || SingletonAlive(in, g, kInGameManagerTypeInfo, "InGameManager")) ? 1
           : SingletonAlive(in, g, kSushiBarManagerTypeInfo, "SushiBarManager") ? 2 : 0;
}

// ---- map probe (round 26; the idea from DaveDiverExpansion 2.0 by yslailo, MIT): what a dive map
// can be drawn from. CameraManager (Toolbox.Singleton<CameraManager>, TypeInfo below) keeps the
// camera's position (m_CurrentPosition @0xBC), what it follows (m_LastTargetPosition @0xC8: Dave)
// and the view's corners (@0xF0 top right, @0xFC bottom left). InGameManager keeps the level's
// camera areas: m_SubBoundsCollection @0x60 -> m_BoundsList @0x38 (List<CameraSubBounds>, each
// Bounds @0x3C = center xyz, extents xyz), the current one @0xE0, surfaceHeight @0x78, seaLevelHeight @0x7C.
constexpr uint64_t kCamSingletonTI[2] = {0x9DA59E8, 0x9DA5598};   // Toolbox.Singleton<>, EvilFactory.SceneSingleton<>
void AddF(Buf& b, float v) {                                       // one decimal
    int64_t t = static_cast<int64_t>(v * 10.0f + (v < 0 ? -0.5f : 0.5f));
    if (t < 0) { b.addc('-'); t = -t; }
    b.num(t / 10).addc('.').num(t % 10);
}
void AddV(Buf& b, const Guest& g, uint64_t a, int n) {
    float f[3] = {};
    if (!g.read(a, f, sizeof(float) * static_cast<size_t>(n))) { b.add("?"); return; }
    for (int i = 0; i < n; ++i) { if (i) b.addc(','); AddF(b, f[i]); }
}
char g_map_probe[4][256];
// IL2CPP's class table (one Il2CppClass* per TypeDefIndex, in the runtime's own memory) lets the
// module reach any class by the number Il2CppDumper prints ("// TypeDefIndex: 2963"). It is found
// by its known entry: InGameManager (TypeDefIndex 3490) is the class behind kInGameManagerTypeInfo.
// The pointer to the table sits in main's data; scanned a slice per sample, once per session.
constexpr uint32_t kTdiInGameManager = 3490, kTdiCameraManager = 2963;
constexpr uint64_t kTypeScanFrom = 0x9C00000, kTypeScanTo = 0xB000000;   // main's data and bss (main_size covers less)
struct TypeTableScan { uint64_t at, table; int state; uint32_t hits, blocks; };   // state 0 scanning, 1 found, 2 not found
TypeTableScan g_tt;
[[maybe_unused]] void TypeTableStep(const Instance& in, const Guest& g, const EdenDsmodHostApi*) {
    if (g_tt.state != 0) return;
    const uint64_t igm = g.ptr(in.main_base + kInGameManagerTypeInfo);
    if (!igm) return;                                         // the game is not up yet
    const uint64_t end = in.main_base + kTypeScanTo;
    if (!g_tt.at) g_tt.at = in.main_base + kTypeScanFrom;
    uint64_t blk[512];
    for (int round = 0; round < 16 && g_tt.at < end; ++round, g_tt.at += sizeof blk) {
        const size_t n = g_tt.at + sizeof blk <= end ? sizeof blk : static_cast<size_t>(end - g_tt.at);
        if (!g.read(g_tt.at, blk, n)) continue;
        ++g_tt.blocks;
        for (size_t k = 0; k < n / 8; ++k) {
            const uint64_t q = blk[k];
            // a table lives in the runtime's heap: outside main's image, 8-aligned
            if (!q || (q & 7) || (q >= in.main_base && q < in.main_base + kTypeScanTo)) continue;
            ++g_tt.hits;
            if (g.ptr(q + 8 * kTdiInGameManager) != igm) continue;
            char c[32];
            const uint64_t cm = g.ptr(q + 8 * kTdiCameraManager);
            if (cm && !(g.cstr(g.ptr(cm + 0x10), c, sizeof c) && eq(c, "CameraManager"))) continue;
            g_tt.table = q; g_tt.state = 1; return;
        }
    }
    if (g_tt.at >= end) g_tt.state = 2;
}
void PublishMapProbe(const Instance& in, const Guest& g, const EdenDsmodHostApi*) {
    char c[48];
    Buf l1, l2, l3, l4;                          // short lines: each fits the page without wrapping
    // The singleton keeps its instance among its statics; look at the first few slots (the layout of
    // Toolbox.Singleton<> statics is not known yet) for an object of class CameraManager.
    uint64_t cam = 0; int which = -1, slot = -1;
    char diag[2][40] = {};
    for (int k = 0; k < 2 && !cam; ++k) {
        const uint64_t klass = g.ptr(in.main_base + kCamSingletonTI[k]);
        const uint64_t st = klass ? g.ptr(klass + kStatics) : 0;
        Buf q;
        if (!klass) q.add("NC");
        else if (!st) q.add("NOST");
        else {
            q.add("ST");
            for (int j = 0; j < 8 && !cam; ++j) {
                const uint64_t o = g.ptr(st + 8 * static_cast<uint64_t>(j));
                if (o && g.cls(o, c, sizeof c) && eq(c, "CameraManager")) { cam = o; which = k; slot = j; }
                else q.addc(o ? '+' : '0');
            }
        }
        memcpy(diag[k], q.s, q.n < 39 ? static_cast<size_t>(q.n) + 1 : 39);
    }
    // round 28: the dive uses the plain Singleton<CameraManager> (InGameManager's kind). script.json has
    // no TypeInfo for it, but it has the slots of its methods (Method$Singleton<CameraManager>.get_Instance()
    // and friends): each holds a MethodInfo once the method has run, and the MethodInfo names its class.
    static const uint64_t kCamMethodSlots[4] = {0x9E0E728, 0x9E0E720, 0x9E0E730, 0x9E0E718};
    char mdiag[24] = "M";
    for (int k = 0; k < 4 && !cam; ++k) {
        const uint64_t mi = g.ptr(in.main_base + kCamMethodSlots[k]);
        if (!mi) { mdiag[1 + k] = '0'; continue; }
        char tag = 'x';
        for (int j = 0; j < 8 && !cam; ++j) {
            const uint64_t kl = g.ptr(mi + 8 * static_cast<uint64_t>(j));
            char nm[24];
            if (!kl || !g.cstr(g.ptr(kl + 0x10), nm, sizeof nm) || !eq(nm, "Singleton`1")) continue;
            tag = 'k';
            const uint64_t st = g.ptr(kl + kStatics);
            if (!st) { tag = 's'; continue; }
            for (int q = 0; q < 8 && !cam; ++q) {
                const uint64_t o = g.ptr(st + 8 * static_cast<uint64_t>(q));
                if (o && g.cls(o, c, sizeof c) && eq(c, "CameraManager")) { cam = o; which = 5; slot = k * 10 + q; }
            }
            if (!cam) tag = 'n';
        }
        mdiag[1 + k] = tag;
    }
    if (!cam) { Buf q; q.add(diag[1]).addc(' ').add(mdiag); memcpy(diag[1], q.s, q.n < 39 ? static_cast<size_t>(q.n) + 1 : 39); diag[1][38] = 0; }
    if (!cam && g_tt.state == 1) {                       // by the class table: CameraManager -> Singleton<> statics
        const uint64_t ck = g.ptr(g_tt.table + 8 * kTdiCameraManager);
        const uint64_t par = ck ? g.ptr(ck + kParent) : 0;
        const uint64_t st = par ? g.ptr(par + kStatics) : 0;
        for (int j = 0; st && j < 8 && !cam; ++j) {
            const uint64_t o = g.ptr(st + 8 * static_cast<uint64_t>(j));
            if (o && g.cls(o, c, sizeof c) && eq(c, "CameraManager")) { cam = o; which = 9; slot = j; }
        }
        if (!cam) { Buf q; q.add(ck ? (par ? (st ? "TT ST" : "TT NOST") : "TT NOPAR") : "TT NOCLASS"); memcpy(diag[1], q.s, static_cast<size_t>(q.n) + 1); }
        if (!cam && ck) {                                  // where the class's own statics are, for the next round
            char nm[32] = {}; const uint64_t pn = par ? g.ptr(par + 0x10) : 0;
            if (pn) g.cstr(pn, nm, sizeof nm);
            Buf q; q.add(diag[1]).add(" P ").add(nm[0] ? nm : "-"); memcpy(diag[1], q.s, q.n < 39 ? static_cast<size_t>(q.n) + 1 : 39); diag[1][39 - 1] = 0;
        }
    }
    if (cam) {
        l1.add("CAM").num(which).addc('.').num(slot).add(" "); AddV(l1, g, cam + 0xBC, 2);
        l1.add("  DAVE "); AddV(l1, g, cam + 0xC8, 2);
        l2.add("VIEW "); AddV(l2, g, cam + 0xFC, 2); l2.add(" TO "); AddV(l2, g, cam + 0xF0, 2);
    } else {
        l1.add("NO CAMERA ").add(diag[0]).add(" / ").add(diag[1]);
        if (g_tt.state == 2) l1.add(" NO TABLE (").num(g_tt.hits).addc('/').num(g_tt.blocks).addc(')');
        // which Singleton<> InGameManager uses (namespace.name of its parent class), for comparison
        const uint64_t it = g.ptr(in.main_base + kInGameManagerTypeInfo);
        const uint64_t par = it ? g.ptr(it + kParent) : 0;
        char ns[24] = {}, nm[24] = {};
        if (par && g.cstr(g.ptr(par + 0x18), ns, sizeof ns) && g.cstr(g.ptr(par + 0x10), nm, sizeof nm)) l1.add("  IGM ").add(ns).addc('.').add(nm);
        const uint64_t c0 = g.ptr(in.main_base + kCamSingletonTI[0]);
        if (c0 && g.cstr(g.ptr(c0 + 0x18), ns, sizeof ns) && g.cstr(g.ptr(c0 + 0x10), nm, sizeof nm)) l1.add(" CAM ").add(ns).addc('.').add(nm);
    }
    const uint64_t ti = g.ptr(in.main_base + kInGameManagerTypeInfo);
    const uint64_t single = ti ? g.ptr(ti + kParent) : 0;
    const uint64_t st = single ? g.ptr(single + kStatics) : 0;
    const uint64_t igm = st ? g.ptr(st) : 0;
    if (igm && g.cls(igm, c, sizeof c) && eq(c, "InGameManager")) {
        l3.add("AREA "); AddV(l3, g, igm + 0xE0, 2); l3.add(" +- "); AddV(l3, g, igm + 0xEC, 2);
        float hs[2] = {};
        g.read(igm + 0x78, hs, sizeof hs);
        const uint64_t col = g.ptr(igm + 0x60), lst = col ? g.ptr(col + 0x38) : 0;
        const uint64_t items = lst ? g.ptr(lst + 0x10) : 0;
        const int32_t n = lst ? g.s32(lst + 0x18) : -1;
        l3.add("  N ").num(n);
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
        for (int32_t i = 0; items && i < n && i < 64; ++i) {
            const uint64_t sb = g.ptr(items + 0x20 + 8 * static_cast<uint64_t>(i));
            float bb[6];
            if (!sb || !g.read(sb + 0x3C, bb, sizeof bb)) continue;
            for (int k = 0; k < 2; ++k) {
                if (bb[k] - bb[3 + k] < lo[k]) lo[k] = bb[k] - bb[3 + k];
                if (bb[k] + bb[3 + k] > hi[k]) hi[k] = bb[k] + bb[3 + k];
            }
        }
        l4.add("ALL X ");
        if (n > 0) { AddF(l4, lo[0]); l4.add(".."); AddF(l4, hi[0]); l4.add(" Y "); AddF(l4, lo[1]); l4.add(".."); AddF(l4, hi[1]); }
        l4.add("  SEA "); AddF(l4, hs[1]); l4.add(" SURF "); AddF(l4, hs[0]);
    } else {
        l3.add("NO INGAMEMANAGER");
    }
    // kept: the snapshot is rebuilt every sample, so the lines are published every sample (below)
    memcpy(g_map_probe[0], l1.s, sizeof l1.s); memcpy(g_map_probe[1], l2.s, sizeof l2.s);
    memcpy(g_map_probe[2], l3.s, sizeof l3.s); memcpy(g_map_probe[3], l4.s, sizeof l4.s);
}
// ---- the dive map (0.29.0). The level's box is InGameManager's camera bounds (center @0xE0,
// extents @0xEC); Dave's left-right position is what the camera follows (CameraManager
// m_LastTargetPosition @0xC8), his height comes from the depth the game shows (sea level @0x7C minus
// it), the screen's part of the level from the camera's frustum corners (@0xFC bottom left, @0xF0
// top right). Everything is published in thousandths of the level's height, measured from the
// level's top (ny) and from its middle (dx), so the page scales it to its map box.
uint64_t FindCameraManager(const Instance& in, const Guest& g) {
    static const uint64_t kSlots[4] = {0x9E0E728, 0x9E0E720, 0x9E0E730, 0x9E0E718};
    char c[32];
    for (int k = 0; k < 4; ++k) {
        const uint64_t mi = g.ptr(in.main_base + kSlots[k]);
        if (!mi) continue;
        for (int j = 0; j < 8; ++j) {
            const uint64_t kl = g.ptr(mi + 8 * static_cast<uint64_t>(j));
            char nm[24];
            if (!kl || !g.cstr(g.ptr(kl + 0x10), nm, sizeof nm) || !eq(nm, "Singleton`1")) continue;
            const uint64_t st = g.ptr(kl + kStatics);
            for (int q = 0; st && q < 8; ++q) {
                const uint64_t o = g.ptr(st + 8 * static_cast<uint64_t>(q));
                if (o && g.cls(o, c, sizeof c) && eq(c, "CameraManager")) return o;
            }
        }
    }
    return 0;
}
uint64_t g_map_cam;
constexpr int64_t kMapPad = 32;        // half of Dave's marker on the map / zoom panel
constexpr float kMapZoom = 3.0f;                    // the zoomed window beside the map
// ---- the dive's shape (round 29). Fish find their way with the A* Pathfinding Project: AstarPath.active
// (AstarPath_TypeInfo, statics +0x18) -> data @0x28 -> graphs @0x48 (NavGraph[]); a GridGraph keeps
// width @0x58, depth @0x5C, nodeSize @0x8C and nodes @0x108 (GridNodeBase[]); a node keeps flags @0x14
// (bit 0: walkable = open water) and its position @0x1C (Int3, thousandths of a unit). Read once per
// dive, a slice per sample, into a small picture of the level's box: open water / rock. The picture
// is drawn here from the game's live data; nothing of the game's art is used or shipped.
constexpr uint64_t kAstarPathTI = 0x9DA9460;
constexpr int kTW = 320, kTH = 576;                  // the grid picture's room: one pixel per grid cell (the
                                                     // Blue Hole: 248 x 523); a bigger level is scaled to fit
constexpr int kOW = 2 * kTW, kOH = 2 * kTH;          // the picture handed to Eden: the grid, edges smoothed 2x
struct Terrain {
    uint64_t graph, nodes; int32_t n, w, d; float ns; int cursor; int walk; int gen; bool done;
    float x0, x1, top, bot;                          // the box it is drawn for
    float s; int bw, bh;                             // pixels per unit, the picture's size
    bool ph_set; float phx, phy;                     // where cell centres fall inside a pixel
    char cls[24];
};
Terrain g_tr;
uint8_t g_tr_build[kTW * kTH], g_tr_done[kTW * kTH];
int g_tr_done_gen;                                   // the picture LoadImage hands over (under LiveLock)
int g_tr_done_w, g_tr_done_h;
void TerrainStep(const Instance& in, const Guest& g, const float box[6]) {
    // find the grid
    const uint64_t ak = g.ptr(in.main_base + kAstarPathTI);
    const uint64_t ast = ak ? g.ptr(ak + kStatics) : 0;
    const uint64_t ap = ast ? g.ptr(ast + 0x18) : 0;
    const uint64_t data = ap ? g.ptr(ap + 0x28) : 0;
    const uint64_t graphs = data ? g.ptr(data + 0x48) : 0;
    const int32_t gn = graphs ? g.s32(graphs + 0x18) : 0;
    uint64_t grid = 0; char c[24] = {};
    for (int32_t i = 0; i < gn && i < 8 && !grid; ++i) {
        const uint64_t gr = g.ptr(graphs + 0x20 + 8 * static_cast<uint64_t>(i));
        if (gr && g.cls(gr, c, sizeof c) && (eq(c, "GridGraph") || eq(c, "LayerGridGraph"))) grid = gr;
    }
    if (!grid) {
        g_tr.graph = 0;
        const char* why = gn ? "NO GRID GRAPH" : (ap ? "NO GRAPHS" : "NO ASTARPATH");
        int k = 0; for (; why[k] && k < 23; ++k) g_tr.cls[k] = why[k];
        g_tr.cls[k] = 0;
        return;
    }
    const uint64_t nodes = g.ptr(grid + 0x108);
    if (grid != g_tr.graph || nodes != g_tr.nodes) {      // a new dive (or a rescan): start over
        g_tr.graph = grid; g_tr.nodes = nodes; g_tr.cursor = 0; g_tr.walk = 0; g_tr.done = false;
        g_tr.w = g.s32(grid + 0x58); g_tr.d = g.s32(grid + 0x5C);
        g.read(grid + 0x8C, &g_tr.ns, 4);
        g_tr.n = nodes ? g.s32(nodes + 0x18) : 0;
        memcpy(g_tr.cls, c, sizeof g_tr.cls);
        g_tr.x0 = box[0] - box[3]; g_tr.x1 = box[0] + box[3]; g_tr.top = box[1] + box[4]; g_tr.bot = box[1] - box[4];
        memset(g_tr_build, 0, sizeof g_tr_build);
        // one pixel per cell, unless the level is too big for the picture
        const float wu = g_tr.x1 - g_tr.x0, hu = g_tr.top - g_tr.bot;
        float sc = g_tr.ns > 0.01f ? 1.0f / g_tr.ns : 2.0f;
        if (wu * sc > kTW) sc = kTW / wu;
        if (hu * sc > kTH) sc = kTH / hu;
        g_tr.s = sc;
        g_tr.bw = static_cast<int>(wu * sc + 0.5f); g_tr.bh = static_cast<int>(hu * sc + 0.5f);
        if (g_tr.bw < 1) g_tr.bw = 1; if (g_tr.bw > kTW) g_tr.bw = kTW;
        if (g_tr.bh < 1) g_tr.bh = 1; if (g_tr.bh > kTH) g_tr.bh = kTH;
        g_tr.ph_set = false;
    }
    if (g_tr.done || !nodes || g_tr.n <= 0 || g_tr.n > 4000000) return;
    const float sx = g_tr.s, sy = g_tr.s;
    const bool exact = g_tr.ns > 0.01f && g_tr.s * g_tr.ns > 0.99f;   // one cell = one pixel
    int cw = exact ? 1 : static_cast<int>(g_tr.ns * sx + 0.999f), ch = exact ? 1 : static_cast<int>(g_tr.ns * sy + 0.999f);
    if (cw < 1) cw = 1; if (ch < 1) ch = 1; if (cw > 8) cw = 8; if (ch > 8) ch = 8;
    auto rnd = [](float v) { return v < 0 ? -static_cast<int>(-v + 0.5f) : static_cast<int>(v + 0.5f); };
    uint64_t ptrs[256];
    for (int b = 0; b < 16 && g_tr.cursor < g_tr.n; ++b) {
        const int k = g_tr.n - g_tr.cursor < 256 ? g_tr.n - g_tr.cursor : 256;
        if (!g.read(nodes + 0x20 + 8 * static_cast<uint64_t>(g_tr.cursor), ptrs, 8 * static_cast<size_t>(k))) { g_tr.cursor += k; continue; }
        for (int j = 0; j < k; ++j) {
            uint8_t nd[0x18];                                // flags @0x14, position @0x1C (x, y, z)
            if (!ptrs[j] || !g.read(ptrs[j] + 0x14, nd, sizeof nd)) continue;
            uint32_t flags; int32_t pos[3];
            memcpy(&flags, nd, 4); memcpy(pos, nd + 8, 12);
            if (!(flags & 1)) continue;
            ++g_tr.walk;
            const float wx = static_cast<float>(pos[0]) * 0.001f, wy = static_cast<float>(pos[1]) * 0.001f;
            int px, py;
            if (exact) {
                // cells sit a whole pixel apart: fix the fraction once, so each cell gets its own pixel
                const float fx = (wx - g_tr.x0) * sx, fy = (g_tr.top - wy) * sy;
                if (!g_tr.ph_set) { g_tr.phx = fx - static_cast<float>(rnd(fx)); g_tr.phy = fy - static_cast<float>(rnd(fy)); g_tr.ph_set = true; }
                px = rnd(fx - g_tr.phx); py = rnd(fy - g_tr.phy);
            } else {
                px = static_cast<int>((wx - g_tr.x0) * sx) - cw / 2; py = static_cast<int>((g_tr.top - wy) * sy) - ch / 2;
            }
            for (int v = 0; v < ch; ++v)
                for (int u = 0; u < cw; ++u) {
                    const int X = px + u, Y = py + v;
                    if (X >= 0 && Y >= 0 && X < g_tr.bw && Y < g_tr.bh) g_tr_build[Y * kTW + X] = 1;
                }
        }
        g_tr.cursor += k;
    }
    if (g_tr.cursor >= g_tr.n) {
        g_tr.done = true;
        LiveLock lock;
        memcpy(g_tr_done, g_tr_build, sizeof g_tr_done);
        g_tr_done_w = g_tr.bw; g_tr_done_h = g_tr.bh;
        g_tr_done_gen = ++g_tr.gen;
    }
}
uint8_t g_tr_px[kOW * kOH * 4];
bool TerrainImage(EdenDsmodImageSink sink, void* receiver) {      // under LiveLock (LoadImage)
    if (!g_tr_done_gen) return false;
    // the grid at 2x with its staircase edges smoothed (Scale2x: a corner pixel takes its neighbours'
    // value where two sides agree) -- still only the game's cells, no new shapes
    const int w = g_tr_done_w, h = g_tr_done_h, ow = 2 * w, oh = 2 * h;
    auto at = [&](int x, int y) -> uint8_t {
        x = x < 0 ? 0 : x >= w ? w - 1 : x; y = y < 0 ? 0 : y >= h ? h - 1 : y;
        return g_tr_done[y * kTW + x];
    };
    auto put = [&](int x, int y, uint8_t v) {
        uint8_t* q = g_tr_px + 4 * (y * ow + x);
        if (v) { q[0] = 13; q[1] = 58; q[2] = 92; q[3] = 255; }       // open water
        else { q[0] = 30; q[1] = 38; q[2] = 50; q[3] = 255; }         // rock
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t P = at(x, y), A = at(x, y - 1), B = at(x + 1, y), C = at(x - 1, y), D = at(x, y + 1);
            uint8_t e0 = P, e1 = P, e2 = P, e3 = P;
            if (B != C && A != D) {
                e0 = C == A ? A : P; e1 = A == B ? B : P; e2 = D == C ? C : P; e3 = B == D ? D : P;
            }
            put(2 * x, 2 * y, e0); put(2 * x + 1, 2 * y, e1); put(2 * x, 2 * y + 1, e2); put(2 * x + 1, 2 * y + 1, e3);
        }
    sink(receiver, ow, oh, g_tr_px, static_cast<size_t>(ow) * oh * 4);
    return true;
}
float g_mk_k, g_mk_cx, g_mk_top; bool g_mk_ok;      // the map's scale, for the markers
// 0.36.2: the map's view -- whole level, or kMapZoom around Dave (tap the map); everything drawn on
// it goes through MapDX / MapNY, which give thousandths in the current view
int g_map_zoomed, g_mk_all = 1, g_mk_mask = 0x3F, g_mk_menu;   // 0.37.3: g_mk_menu -- the FILTER list is open
// 0.36.17: g_mk_mask -- the map's toggles: bit 0 chests, 1 O2, 2 weapons, 3 missions                                    // UiAction "map_zoom" / "mk_all"
float g_mv_x0, g_mv_wid, g_mv_top, g_mv_hgt, g_mv_k, g_mv_wl, g_mv_wt, g_mv_z = 1.0f, g_dave_x, g_dave_y;
float MapNX(float x) { return (x - g_mv_x0) / g_mv_wid; }
float MapNYn(float y) { return (g_mv_top - y) / g_mv_hgt; }
// 0.37.12: x in the drawn map's own units (its width is kMapW thousandths whatever the level's shape --
// the picture is stretched to the box; the wreck's cave is far wider than the Blue Hole)
constexpr float kMapW = 475.0f;
int64_t MapDX(float x) { const float v = ((MapNX(x) - g_mv_wl) * g_mv_z - 0.5f) * kMapW; return static_cast<int64_t>(v < 0 ? v - 0.5f : v + 0.5f); }
int64_t MapNY(float y) { return static_cast<int64_t>((MapNYn(y) - g_mv_wt) * g_mv_z * 1000.0f + 0.5f); }
bool g_mp_reset;   // 0.37.6: the level changed inside a dive -> the marker census starts over
bool DaveOwnPos(const Guest& g, uint64_t igm, float* x, float* y);   // below, with the marker probe
int g_dave_src;    // 0.37.50: 0 = camera, 1 = Dave's own position (the camera's was outside the level)
void PublishMap(const Instance& in, const Guest& g, const EdenDsmodHostApi* h) {
    g_mk_ok = false;
    void* u = h->userdata;
    char c[32];
    if (g_map_cam && (g_ticks % 30 == 0) && !(g.cls(g_map_cam, c, sizeof c) && eq(c, "CameraManager"))) g_map_cam = 0;
    {   // 0.38.13: a new scene (a teleport into the record chamber) -> find the camera again (the old one kept Dave still)
        static char last_scene[40];
        if (!eq(last_scene, g_scene_data)) { DcCopy(last_scene, sizeof last_scene, g_scene_data); g_map_cam = 0; }
    }
    if (!g_map_cam && g_ticks % 30 == 15) g_map_cam = FindCameraManager(in, g);
    const uint64_t ti = g.ptr(in.main_base + kInGameManagerTypeInfo);
    const uint64_t single = ti ? g.ptr(ti + kParent) : 0;
    const uint64_t st = single ? g.ptr(single + kStatics) : 0;
    const uint64_t igm = st ? g.ptr(st) : 0;
    float box[6] = {}, sea = 0, tgt[2] = {}, bl[2] = {}, tr[2] = {};
    bool ok = igm && g_map_cam && h->get_f64 && h->get_i64 && h->get_i64(u, "bh", 0) != 0 &&
              g.read(igm + 0xE0, box, sizeof box) && g.read(igm + 0x7C, &sea, 4) &&
              g.read(g_map_cam + 0xC8, tgt, sizeof tgt) && g.read(g_map_cam + 0xFC, bl, sizeof bl) &&
              g.read(g_map_cam + 0xF0, tr, sizeof tr) && box[4] > 1.0f && box[3] > 1.0f;
    h->publish_i64(u, "map.ok", ok ? 1 : 0);
    if (!ok) { h->publish_i64(u, "map.terrain", 0); return; }
    TerrainStep(in, g, box);
    {
        Buf t; t.add("module:map/t").num(g_tr.gen);
        h->publish_text(u, "map.img", t.s);
        h->publish_i64(u, "map.terrain", g_tr.gen > 0 ? 1 : 0);
        Buf d; d.add(g_tr.cls).addc(' ').num(g_tr.w).addc('x').num(g_tr.d).add(" NS "); AddF(d, g_tr.ns);
        d.add(" N ").num(g_tr.n).add(" WATER ").num(g_tr.walk).add(g_tr.done ? " DONE" : " READING ").num(g_tr.done ? 0 : g_tr.cursor);
        h->publish_text(u, "map.p5", d.s);
    }
    const float top = box[1] + box[4], hgt = 2.0f * box[4], k = 1000.0f / hgt;
    {   // 0.37.50: where the camera's target is outside the level (the Sea People hall: Dave drawn in the
        // corner), Dave's own position instead -- the one the markers use; elsewhere unchanged
        const bool out = tgt[0] < box[0] - box[3] - 2.0f || tgt[0] > box[0] + box[3] + 2.0f || tgt[1] > top + 2.0f || tgt[1] < top - hgt - 2.0f;
        float ox = 0, oy = 0;
        g_dave_src = 0;
        if (out && DaveOwnPos(g, igm, &ox, &oy) && ox >= box[0] - box[3] - 2.0f && ox <= box[0] + box[3] + 2.0f && oy <= top + 2.0f && oy >= top - hgt - 2.0f) {
            tgt[0] = ox; tgt[1] = oy; g_dave_src = 1;
        }
    }
    g_mk_k = k; g_mk_cx = box[0]; g_mk_top = top; g_mk_ok = true;
    const float depth = static_cast<float>(h->get_f64(u, "depth", 0)), dmax = static_cast<float>(h->get_f64(u, "depth_max", 0));
    const float wid = 2.0f * box[3];
    auto clamp01 = [](float v) { return v < 0 ? 0.0f : v > 1 ? 1.0f : v; };
    float dyw = sea - depth; if (dyw > top + 2.0f || dyw < top - hgt - 2.0f) dyw = tgt[1];   // 0.37.6 (see below)
    const float dxn = clamp01((tgt[0] - (box[0] - box[3])) / wid), dyn = clamp01((top - dyw) / hgt);
    const float z = g_map_zoomed ? kMapZoom : 1.0f;
    auto win = [&](float n) { const float w = clamp01(n - 0.5f / z); return w > 1 - 1 / z ? 1 - 1 / z : w; };
    g_mv_x0 = box[0] - box[3]; g_mv_wid = wid; g_mv_top = top; g_mv_hgt = hgt; g_mv_k = k;
    static char boxd[240];
    if (g_ticks % 15 == 3) {   // 0.38.12 (research): the level's box, the camera's target and Dave's own place (record chamber)
        float ox = 0, oy = 0; const bool own = DaveOwnPos(g, igm, &ox, &oy);
        Buf b; b.add("box x "); AddF(b, box[0] - box[3]); b.add(".."); AddF(b, box[0] + box[3]); b.add(" y "); AddF(b, top - hgt);
        b.add(".."); AddF(b, top); b.add(" | cam "); AddF(b, tgt[0]); b.addc(','); AddF(b, tgt[1]);
        b.add(" | dave "); if (own) { AddF(b, ox); b.addc(','); AddF(b, oy); } else b.add("?");
        b.add(" | src ").num(g_dave_src).add(" | sea "); AddF(b, sea); b.add(" | scene ").add(g_scene_data);
        DcCopy(boxd, sizeof boxd, b.s);
    }
    h->publish_text(u, "map.boxd", boxd);   // 0.38.13: every tick (the values are cleared each tick: it blinked)
    g_mv_z = z; g_mv_wl = win(dxn); g_mv_wt = win(dyn);
    g_dave_x = tgt[0]; g_dave_y = sea - depth;
    // 0.37.6: a level whose box the depth does not fit (the cave in the wreck: its own box, the Blue Hole's
    // sea level) -- the camera's height instead
    if (g_dave_y > top + 2.0f || g_dave_y < top - hgt - 2.0f) g_dave_y = tgt[1];
    {   // a new level inside the same dive (its box changes): the chest census starts over
        static float last[2];
        const float d0 = box[0] - last[0], d1 = box[1] - last[1];
        if ((d0 > 1.0f || d0 < -1.0f || d1 > 1.0f || d1 < -1.0f)) { if (last[0] != 0 || last[1] != 0) g_mp_reset = true; last[0] = box[0]; last[1] = box[1]; }
    }
    auto ny = [&](float y) { return MapNY(y); };
    auto dx = [&](float x) { return MapDX(x); };
    auto in_y = [](int64_t v) { return v >= 0 && v <= 1000; };
    // Dave's marker stays whole inside the box (kMapPad: half the icon, in thousandths)
    auto keep = [](int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; };
    const int64_t hwk = static_cast<int64_t>(kMapW * 0.5f);
    h->publish_i64(u, "map.dx", keep(dx(tgt[0]), -hwk + kMapPad, hwk - kMapPad));
    h->publish_i64(u, "map.ny", keep(ny(g_dave_y), kMapPad, 1000 - kMapPad));   // 0.37.6: g_dave_y (camera height in a cave)
    h->publish_i64(u, "map.hw", hwk);
    h->publish_i64(u, "map.sea", ny(sea));
    h->publish_i64(u, "map.sea_on", in_y(ny(sea)) ? 1 : 0);
    h->publish_i64(u, "map.lim_on", dmax > 0 && sea - dmax > top - hgt && in_y(ny(sea - dmax)) ? 1 : 0);
    h->publish_i64(u, "map.lim", ny(sea - dmax));
    // the picture follows the same view (an image with pan_zoom, centred on Dave like Eden's DefaultView)
    h->publish_f64(u, "map.zx", dxn);
    h->publish_f64(u, "map.zy", dyn);
    h->publish_f64(u, "map.zoom", z);
    h->publish_i64(u, "map.zoomed", g_map_zoomed);
    h->publish_i64(u, "map.vreset", static_cast<int64_t>(g_ticks));   // 0.37.16: the picture snaps to this view every tick
    h->publish_i64(u, "mk.all", g_mk_all);
    static const char* const kf[6] = {"mk.f0", "mk.f1", "mk.f2", "mk.f3", "mk.f4", "mk.f5"};
    for (int k = 0; k < 6; ++k) h->publish_i64(u, kf[k], (g_mk_mask >> k) & 1);
    h->publish_i64(u, "mk.menu", g_mk_menu);
    static const char* const tk[6] = {"map.t0", "map.t1", "map.t2", "map.t3", "map.t4", "map.t5"};
    static const char* const tko[6] = {"map.t0on", "map.t1on", "map.t2on", "map.t3on", "map.t4on", "map.t5on"};
    for (int i = 0; i < 6; ++i) {                                       // depth marks every 50 m
        const float y = sea - 50.0f * static_cast<float>(i);
        h->publish_i64(u, tk[i], ny(y));
        h->publish_i64(u, tko[i], y > top - hgt && in_y(ny(y)) ? 1 : 0);
    }
}

// ---- map markers probe (0.34.0, research round 32). The game keeps no list of its chests and no
// position of its own for them (rounds 30-31): positions live in Unity's native transforms. This
// probe answers two questions on the device and shows the answers on DIAGNOSTICS:
//  1. census: walk the heap a few MB per sample and count live InstanceItemChest / BreakableLootObject
//     objects (klass pointer at +0; a live Unity object has its native pointer at +0x10);
//  2. position: from Dave's own component (InGameManager+0xA0 -> native +0x10) search a few levels of
//     native pointers for where Unity keeps his position, checking against the camera's copy of it
//     (CameraManager.m_LastTargetPosition). The path found is then tried on the first chests.
constexpr bool kMpResearch = false;   // 0.36.8: the 0.34.x path-finding / Dave-copy research passes (heavy) are off
constexpr bool kMarkerProbe = true;  // the chest markers (0.36.0)   // 0.34.12 stalled the module (per-hit class-name reads); parked
constexpr uint64_t kChestTI = 0x9DB0848, kLootTI = 0x9DAA138;
// 0.36.12 (round 32): the loose pickups -- PickupInstanceItem: pickupItemCommand @0x20 (PickupObjectCommand_SO),
// presetItemID @0x38 (usePreset @0x34), m_ItemData @0x78 -> IntegratedItem.TID @0x10
constexpr uint64_t kPickupTI = 0x9DB3C70;
constexpr uint64_t kPullOutTI = 0x9DB4390;   // 0.37.79 (round 48): PullOutInstanceItem (plants pulled with X), a PickupInstanceItem child
constexpr int kPk = 384, kMm = 16;   // 0.37.79: 384 pickups kept (221 out on a Blue Hole dive before plants)   // pickups kept; mission marks drawn (gen_dive.py MISSION_MARKS)
constexpr int kMpKeep = 160;   // gen_dive.py MARKERS must match
struct MarkerProbe {
    uint64_t lo, hi, cur, chest_k, loot_k, ctl_k;
    int chests, loot, live_chests, live_loot, passes, ctl;
    uint64_t igm, nat;
    char l4[256], l5[256], l6[256];
    uint64_t keep[kMpKeep]; int nkeep;
    // the transform path (-1 = not found yet)
    int state;                         // 0 waiting, 1 found, 2 not found
    int o1, o2, o3, o4, stride, xoff, poff, stage;   // stride: the layout (MpWorld)
    float best, bx, by; int b1, b2, b3, b4, bxo, bpo, bl;    // the closest try
    // 0.34.5: every place in memory holding Dave's x,y (as the camera has it) when its page is scanned;
    // the ones that keep following him as he swims are his real copies
    uint64_t copy[64]; uint8_t copy_live[64]; int ncopy;
    // 0.34.6: which copy sits in a list of {position, rotation, scale} (Unity's own record), and who
    // points at that list (a second pass over the same range, once Dave has swum away from the start)
    float c0x, c0y; bool c0;
    int pass2;                         // 0 not started, 1 running, 2 done
    uint64_t cur2;
    uint64_t rec_p[16], rec_v[16], rec_a[16]; int rec_s[16], nrec;
    int idxoff;                        // where the transform keeps its index (0 = +8)
    int force_idx;                     // > 0: MpWorld reads node force_idx-1 instead (the Dave check)
    int fixed;                         // 0.34.8: the layout read off 0.34.7's record is in use
    int cpass, nc; float cx[24], cy[24]; int co1[24], co2[24], co3[24];   // 0.34.10: chests' own paths
    // 0.34.13: cheap census check (the command field's class, learned once) and the chests read one
    // per sample, kept: mx/my their positions, mhow 1 Dave's path, 2 own path, 3 loose, 0 none
    uint64_t cmdk; int slow, unsure, ci, rdone; float mx[kMpKeep], my[kMpKeep]; uint8_t mhow[kMpKeep];
    // 0.35.2: kind (1 item chest, 2 O2, 3 weapon, 4 pot, 5 other spawner, 0 no spawner found), opened,
    // census rounds (the scan repeats to pick up chests loaded later)
    uint8_t kind[kMpKeep], opened[kMpKeep], owned[kMpKeep]; int rounds, ki; char kinds[96];
    uint8_t seen[kMpKeep];             // 0.36.2: Dave has been near it this dive (fog of war)
    // 0.35.3: the spawners hold their chest (SpawnerChestItem.ChestItem @0x68): the census watches for
    // words equal to a known chest, and names the object 0x68 before it
    uint64_t srt[kMpKeep]; uint8_t sidx[kMpKeep]; int nsrt; uint64_t smin, smax; int spawn_hits, spawn_named;
    char l8[256], l9[256], l10[256];
    char l7[256];  // see MpTransform / MpWorld; stage: 1 transform found, 2 hierarchy read
    // 0.36.4: chests whose class is a child of InstanceItemChest (learned: a class whose parent @0x58
    // is InstanceItemChest), every command class whose name ends in _SO (not just the first one seen),
    // the spawner classes seen (with their kind) and classes known not to be spawners / commands
    uint64_t subk[8]; uint8_t subkind[8]; int nsubk;
    uint64_t cmdks[8]; int ncmdk;
    uint64_t badk[32]; int nbadk;
    uint64_t spk[16]; uint8_t spkind[16]; int nspk;
    uint64_t nspbad[32]; int nnspbad;
    uint8_t dead[kMpKeep]; int rr, ndead, reused;
    char subn[120], cmdn[120], spn[160];
    char l11[256], l12[256];
    // 0.36.5: the spawner that holds each chest (its own transform is the fallback position: 80 of 109
    // chests could not be placed by their own transform in 0.36.4), and where each unplaced read stops
    uint64_t spw[kMpKeep]; uint8_t mst[kMpKeep], tries8[kMpKeep], mat[kMpKeep], matok[kMpKeep], matn[kMpKeep]; int nmat;
    uint64_t flo, fhi; int hotpass;    // 0.36.13: the full census range; 1 while the first pass scans the hot window
    // 0.36.12: pickups (their own list; only the ones a mission in progress asks for are drawn)
    uint64_t pick_k, psubk[8], pcmdk[8]; int npsubk, npcmdk;   // 0.37.79: 8 command classes (PullOutCommand_SO too)
    uint64_t pspw[kPk], psrt[kPk]; uint16_t psidx[kPk]; int npsrt;
    uint8_t pactual[kPk], powned[kPk], pidle[kPk]; int npidle;  // live item data and revalidated reciprocal spawner link
    uint64_t pk[kPk]; float pxx[kPk], pyy[kPk]; uint8_t ph[kPk], pdead[kPk], pkind[kPk]; int32_t ptid[kPk]; int npk, pci, prr, npicks;
    uint64_t pod_k[2]; int npod, podchk; uint64_t pods[24]; float podx[24], pody[24]; uint8_t podh[24], podused[24];   // 0.37.22
    int32_t mtgt[18]; int nmtgt;
    // 0.36.9: mission item (its spawner's SpawnerBase.IsMissionItem @0x40) and no Animator
    // (InstanceItemChest.m_Animator @0x48 null -- a small pickup rather than a box? to be checked)
    uint8_t mis[kMpKeep], noanim[kMpKeep];
    // 0.36.10: each chest's drop list (InstanceItemSpawnHandler.SpawnItemListTID @0x34 via m_ItemSpawner @0x40)
    int32_t tid[kMpKeep]; uint64_t hdk;
    int64_t reads, tries;
    float dx, dy;
    char l1[256], l2[256], l3[256];
};
MarkerProbe g_mp;
int g_po_seeded;   // 0.37.80: 1 PullOutInstanceItem seeded, 2 already known (DIAG pickups line)
// 0.36.13: where in memory the last dive's chests and their spawners were (kept across dives): the next
// dive's first census pass scans that window first, the whole range after
uint64_t g_hot_lo, g_hot_hi, g_hot_nlo, g_hot_nhi;
// Marker identity is never inferred from a coordinate: the same spot can hold a different
// object on the next dive. Only the current object's verified spawner supplies its kind.
void MpHot(uint64_t a) { if (!g_hot_nlo || a < g_hot_nlo) g_hot_nlo = a; if (a > g_hot_nhi) g_hot_nhi = a; }
bool MpChestReal(const Guest& g, uint64_t obj);
bool MpPtrOk(const Guest& g, uint64_t v) {
    return v >= 0x1000000 && v < 0x100000000000ULL && (v & 7) == 0 && g.read(v, &v, 8);
}
bool MpNear(float a, float b) { const float d = a - b; return d > -0.3f && d < 0.3f; }
bool MpHas(const char* s, const char* t) {
    for (int a = 0; s[a]; ++a) { int b = 0; while (t[b] && s[a + b] == t[b]) ++b; if (!t[b]) return true; }
    return false;
}
// a name list for the diagnostics ("A, B, ...")
void MpAddName(char* list, int cap, const char* name) {
    int n = 0; while (list[n]) ++n;
    if (n && n < cap - 3) { list[n++] = ','; list[n++] = ' '; }
    for (int i = 0; name[i] && n < cap - 1; ++i) list[n++] = name[i];
    list[n] = 0;
}
// Marker categories: 1 verified chest class, 2 O2, 3 weapon, 4 pot, 5 Godzilla, 6 pile, 0 unknown.
uint8_t MpKindOf(const char* cn) {
    return MpHas(cn, "O2") ? 2 : MpHas(cn, "Weapon") ? 3 : MpHas(cn, "Pot") ? 4 : MpHas(cn, "Godzilla") ? 5
         : MpHas(cn, "Seachable") || MpHas(cn, "Searchable") ? 6 : eq(cn, "SpawnerChestItem") ? 1 : 0;
}
template <int N> bool MpIn(const uint64_t (&a)[N], int n, uint64_t v) { for (int i = 0; i < n && i < N; ++i) if (a[i] == v) return true; return false; }
template <int N> void MpRing(uint64_t (&a)[N], int* n, uint64_t v) { a[*n % N] = v; ++*n; }
// Both fields are documented in ROADMAP round 32: SpawnerChestItem.ChestItem and
// SpawnerPickupItem.PickupItem at +0x68. A reference alone is not an ownership proof.
bool MpSpawnerOwns(const Guest& g, uint64_t sp, uint64_t obj, bool pickup) {
    if (!sp || !obj || g.ptr(sp + 0x68) != obj || !g.ptr(sp + 0x10)) return false;
    uint64_t k = g.ptr(sp);
    for (int n = 0; k && n < 8; ++n) {
        char cn[64] = "";
        if (!g.cstr(g.ptr(k + 0x10), cn, sizeof cn)) return false;
        if (pickup ? eq(cn, "SpawnerPickupItem") :
            (eq(cn, "SpawnerChestItem") || eq(cn, "SpawnerChestO2") || eq(cn, "SpawnerChestWeapon"))) return true;
        const uint64_t parent = g.ptr(k + 0x58);
        if (parent == k) break;
        k = parent;
    }
    return false;
}
// Return only categories that actually have a matching map icon. Pots, material piles,
// unrelated spawners and unreadable names must not silently become yellow chests.
uint8_t MpChestKind(const Guest& g, uint64_t sp, uint64_t obj) {
    if (!MpSpawnerOwns(g, sp, obj, false)) return 0;
    char cn[64] = "";
    if (!g.cls(sp, cn, sizeof cn)) return 0;
    const uint8_t kind = MpKindOf(cn);
    return kind >= 1 && kind <= 3 ? kind : 0;
}
void MpRefreshChestIdentity(const Guest& g, MarkerProbe& m, int i) {
    m.kind[i] = 0; m.owned[i] = 0; m.matok[i] = 0; m.mat[i] = 0; m.mis[i] = 0;
    const uint64_t sp = m.spw[i];
    if (!MpSpawnerOwns(g, sp, m.keep[i], false)) { m.spw[i] = 0; return; }
    m.owned[i] = 1;
    m.kind[i] = MpChestKind(g, sp, m.keep[i]);
    char ic[64] = "";
    if (g.str(g.ptr(sp + 0x60), ic, sizeof ic) && ic[0]) {
        m.mat[i] = starts(ic, "Gizmo_MXM") ? 1 : 0;
        // Generic spawners also host other objects. Only known chest artwork verifies
        // a chest; an unrelated / new image remains hidden until it is investigated.
        m.matok[i] = starts(ic, "Chest_") || (m.kind[i] == 2 && starts(ic, "O2Box_"));
    }
    uint8_t b = 0; if (g.read(sp + 0x40, &b, 1)) m.mis[i] = b ? 1 : 0;
    // Unreadable pictures stay unverified; elapsed attempts never establish a chest.
}
bool MpChestVisible(const MarkerProbe& m, int i) {
    return i < m.nkeep && m.mhow[i] && !m.opened[i] && !m.dead[i] &&
        m.owned[i] && !m.mat[i] && m.matok[i] && m.kind[i] >= 1 && m.kind[i] <= 3;
}
// 0.38.2: where each pickup was first placed, and whether it has moved since (a real ammo box never moves; the false
// one seen on device follows Dave, 1-2 m from him wherever he swims -- 0.38.1 DIAG, three photos)
// A pickup the game reuses can be moved ONCE to a new spot (a real box from a broken crate): only a pickup seen moving
// on two separate reads is taken as carried along.
float g_pk0x[kPk], g_pk0y[kPk]; uint8_t g_pk0[kPk], g_pkmv[kPk], g_pkmoves[kPk];
// 0.38.4: the pickup's GameObject byte +0x30 (0xFF = not read). 0.38.3 DIAG, A04_01_02: the real ammo box and ~88% of
// all ammo have 06 there; all three false ammo marks (the plant 19.3 m, the ledges 14.8 m and 10.9 m) have 05.
uint8_t g_pkg30[kPk];
constexpr uint8_t kPkGoOff = 0x05;
constexpr float kPkStep = 0.3f;    // units between two reads that count as a move
bool MpAmmoVisible(const MarkerProbe& m, int i) {
    // 0.37.35: the spawner-owner rule hid most real ammo on device (only 6 of ~20 found an owner); live item data
    // (not a preset) is still required. powned is kept for the DIAG line.
    // 0.38.2: + it has not moved (g_pkmv)
    return !m.pdead[i] && m.ph[i] && m.ptid[i] > 0 && m.pactual[i] && m.pkind[i] == 2 && !m.pidle[i] &&
           g_pkg30[i] != kPkGoOff;   // 0.38.4: not switched off by the game (GameObject +0x30 == 05); 0.38.2 moving rule off
}
void MpPickupSrtRebuild(MarkerProbe& m) {
    m.npsrt = 0;
    for (int i = 0; i < m.npk; ++i) if (!m.pdead[i]) {
        m.psrt[m.npsrt] = m.pk[i]; m.psidx[m.npsrt++] = static_cast<uint16_t>(i);
    }
    for (int i = 1; i < m.npsrt; ++i) {
        const uint64_t a = m.psrt[i]; const uint16_t b = m.psidx[i]; int j = i - 1;
        while (j >= 0 && m.psrt[j] > a) { m.psrt[j + 1] = m.psrt[j]; m.psidx[j + 1] = m.psidx[j]; --j; }
        m.psrt[j + 1] = a; m.psidx[j + 1] = b;
    }
}
void MpPickupSpawnerHit(const Guest& g, MarkerProbe& m, uint64_t ref, uint64_t obj) {
    if (!m.npsrt || obj < m.psrt[0] || obj > m.psrt[m.npsrt - 1] || ref < 0x1068) return;
    int lo = 0, hi = m.npsrt - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (m.psrt[mid] == obj) {
            const int i = m.psidx[mid];
            if (!m.pdead[i] && !m.pspw[i] && m.slow < 12) {
                ++m.slow;
                if (MpSpawnerOwns(g, ref - 0x68, obj, true)) m.pspw[i] = ref - 0x68;
            }
            return;
        }
        if (m.psrt[mid] < obj) lo = mid + 1; else hi = mid - 1;
    }
}
// Recheck position and identity together. Failure clears visibility rather than combining
// an old position / ammo kind with a new, unreadable or recycled pickup object.
void MpRefreshPickup(const Guest& g, MarkerProbe& m, int i);
// the position for one native component along the found path (local position in its hierarchy)
// The transform of a native component: Component+o1 -> GameObject, GameObject+o2 -> its component
// list, list+o3 -> the Transform (itself a component: its +o1 points back at the same GameObject).
// 0.37.31 (research): the native GameObject behind a managed component, its name and the bytes around Unity's
// active flags (to tell an object the game has switched off from one in play)
char g_pkgo[400];
// 0.37.33 (research): which byte of the target's native component (C) / GameObject (G) differs from what most of
// its kind have (the others are mostly real, in-play objects): candidates for the game's on/off flag.
// others[]: managed objects of the same kind. Prints "C+57=00(01 94%)" for bytes where >=85% of the others agree.
void Odd(const Guest& g, uint64_t target, const uint64_t* others, int n, int o1, Buf& b, int max_shown = 8) {
    constexpr int kLo = 0x18, kHi = 0x80, kN = kHi - kLo;
    auto load = [&](uint64_t managed, uint8_t* c, uint8_t* go) {
        const uint64_t C = managed ? g.ptr(managed + 0x10) : 0, G = C ? g.ptr(C + o1) : 0;
        return C && G && g.read(C + kLo, c, kN) && g.read(G + kLo, go, kN);
    };
    uint8_t tc[kN], tg[kN];
    if (!load(target, tc, tg)) { b.add(" ODD ?"); return; }
    static uint16_t cntc[kN][256], cntg[kN][256];
    for (int k = 0; k < kN; ++k) for (int v = 0; v < 256; ++v) cntc[k][v] = cntg[k][v] = 0;
    int m = 0;
    for (int i = 0; i < n && m < 64; ++i) {
        if (!others[i] || others[i] == target) continue;
        uint8_t c[kN], go[kN];
        if (!load(others[i], c, go)) continue;
        for (int k = 0; k < kN; ++k) { ++cntc[k][c[k]]; ++cntg[k][go[k]]; }
        ++m;
    }
    b.add(" ODD(").num(m).addc(')');
    static const char hx[] = "0123456789abcdef";
    int shown = 0;
    for (int side = 0; side < 2; ++side)
        for (int k = 0; k < kN && shown < max_shown && m >= 5; ++k) {
            const uint16_t (*cnt)[256] = side ? cntg : cntc;
            int best = 0; for (int v = 1; v < 256; ++v) if (cnt[k][v] > cnt[k][best]) best = v;
            const uint8_t t = side ? tg[k] : tc[k];
            if (t == best || cnt[k][best] * 100 < m * 85) continue;
            b.addc(' ').addc(side ? 'G' : 'C').addc('+').addc(hx[(kLo + k) >> 4]).addc(hx[(kLo + k) & 15]).addc('=')
             .addc(hx[t >> 4]).addc(hx[t & 15]).addc('(').addc(hx[best >> 4]).addc(hx[best & 15]).addc(' ')
             .num(cnt[k][best] * 100 / m).add("%)");
            ++shown;
        }
    if (!shown) b.add(" none");
}
uint64_t MpTransform(const Guest& g, uint64_t native) {
    const MarkerProbe& m = g_mp;
    const uint64_t G = g.ptr(native + m.o1); if (!G) return 0;
    const uint64_t C = g.ptr(G + m.o2); if (!C) return 0;
    const uint64_t T = g.ptr(C + m.o3); if (!T || g.ptr(T + m.o1) != G) return 0;
    return T;
}
// World position of a transform: its hierarchy (T+o4, index at +8) keeps every node's local
// position / rotation / scale (TransformX: t @0, q @16, s @32, `stride` apart, array at H+xoff) and
// parent index (int array at H+poff, -1 at the root; a parent always comes before its child).
// layout: 0 = TransformX {float4 t, q, s} (48 apart), 1 = packed {float3 t, float4 q, float3 s} (40),
// 2 = {float4 t, q, s, pad} (64)
int g_mp_why;   // 0.36.6: why the last MpWorld walk failed (1 node read, 2 parent read, 3 parent is itself, 4 bad parent, 5 too deep)
bool MpWorld(const Guest& g, uint64_t T, int o4, int xoff, int poff, int layout, float* x, float* y) {
    g_mp_why = 0;
    static const int kStride[3] = {48, 40, 64}, kQ[3] = {4, 3, 4}, kS[3] = {8, 7, 8};
    const int stride = kStride[layout], qo = kQ[layout], so = kS[layout];
    const uint64_t H = g.ptr(T + o4); if (!H) return false;
    bool ok = false; int32_t idx = g.s32(T + o4 + (g_mp.idxoff ? g_mp.idxoff : 8), &ok); if (!ok || idx < 0 || idx > 200000) return false;
    if (g_mp.force_idx > 0) idx = g_mp.force_idx - 1;
    const uint64_t A = g.ptr(H + xoff), P = g.ptr(H + poff); if (!A || !P) return false;
    float px = 0, py = 0, pz = 0; bool first = true;
    for (int depth = 0; depth < 64 && idx >= 0; ++depth) {
        float t[12]; if (!g.read(A + static_cast<uint64_t>(idx) * stride, t, 44)) { g_mp_why = 1; return false; }
        if (first) { px = t[0]; py = t[1]; pz = t[2]; first = false; }
        else {
            // p = parent.q * (parent.s * p) + parent.t
            const float sx = px * t[so], sy = py * t[so + 1], sz = pz * t[so + 2];
            const float qx = t[qo], qy = t[qo + 1], qz = t[qo + 2], qw = t[qo + 3];
            const float ux = qy * sz - qz * sy, uy = qz * sx - qx * sz, uz = qx * sy - qy * sx;   // u = q.xyz x v
            const float wx = qy * uz - qz * uy, wy = qz * ux - qx * uz, wz = qx * uy - qy * ux;   // q.xyz x u
            px = sx + 2.0f * (qw * ux + wx) + t[0];
            py = sy + 2.0f * (qw * uy + wy) + t[1];
            pz = sz + 2.0f * (qw * uz + wz) + t[2];
        }
        bool okp = false; const int32_t par = g.s32(P + static_cast<uint64_t>(idx) * 4, &okp);
        // 0.36.6: a parent can sit after its child in the list (0.36.5: 64 chests stopped on "parent >=
        // child"); only a node that is its own parent or a wild index is refused, depth caps the walk
        if (!okp) { g_mp_why = 2; return false; }
        if (par == idx) { g_mp_why = 3; return false; }
        if (par < -1 || par > 200000) { g_mp_why = 4; return false; }
        idx = par;
    }
    if (idx >= 0) { g_mp_why = 5; return false; }
    *x = px; *y = py; return true;
}
// 0.34.9: where a read stops (0 ok, 1 no native, 2 GameObject, 3 component list, 4 transform,
// 5 hierarchy / index, 6 node or parent arrays, 7 parent chain)
[[maybe_unused]] int MpStage(const Guest& g, uint64_t native) {
    const MarkerProbe& m = g_mp;
    if (!native) return 1;
    const uint64_t G = g.ptr(native + m.o1); if (!G) return 2;
    const uint64_t C = g.ptr(G + m.o2); if (!C) return 3;
    const uint64_t T = g.ptr(C + m.o3); if (!T || g.ptr(T + m.o1) != G) return 4;
    const uint64_t H = g.ptr(T + m.o4); bool ok = false; const int32_t idx = g.s32(T + m.o4 + 8, &ok);
    if (!H || !ok || idx < 0 || idx > 200000) return 5;
    if (!g.ptr(H + m.xoff) || !g.ptr(H + m.poff)) return 6;
    float x, y; return MpWorld(g, T, m.o4, m.xoff, m.poff, m.stride, &x, &y) ? 0 : 7;
}
// 0.34.10: a chest's own first steps (Dave's N+0x20 / +0x20 / +0x8 do not fit chests): any
// GameObject-like link whose component list holds a transform pointing back at it, then the
// confirmed hierarchy read (T+0x28 / +0x30, H+0x18 / +0x20, 48-byte nodes)
[[maybe_unused]] bool MpChestPos(const Guest& g, uint64_t native, float* x, float* y, int* f1, int* f2, int* f3) {
    if (!native) return false;
    for (int o1 = 8; o1 < 0x80; o1 += 8) {
        const uint64_t G = g.ptr(native + o1); if (!MpPtrOk(g, G)) continue;
        for (int o2 = 8; o2 < 0x80; o2 += 8) {
            const uint64_t C = g.ptr(G + o2); if (!MpPtrOk(g, C)) continue;
            for (int o3 = 0; o3 < 0x40; o3 += 8) {
                const uint64_t T = g.ptr(C + o3);
                if (!MpPtrOk(g, T) || g.ptr(T + o1) != G) continue;
                float px, py;
                if (!MpWorld(g, T, 0x28, 0x18, 0x20, 0, &px, &py)) continue;
                if (!(px > -70 && px < 70 && py > -250 && py < 40)) continue;
                *x = px; *y = py; *f1 = o1; *f2 = o2; *f3 = o3; return true;
            }
        }
    }
    return false;
}
// 0.34.11: is this a real InstanceItemChest? (its managed fields: command @0x20 a ScriptableObject
// whose class name ends in "_SO"); and a Dave-style pattern of its native object
bool MpChestReal(const Guest& g, uint64_t obj) {
    const uint64_t cmd = g.ptr(obj + 0x20); char c[48];
    if (!cmd || !g.cls(cmd, c, sizeof c)) return false;
    int n = 0; while (c[n]) ++n;
    return n > 3 && c[n - 3] == '_' && c[n - 2] == 'S' && c[n - 1] == 'O';
}
// any transform reachable in 1-2 steps from the native component whose hierarchy reads (no back check)
[[maybe_unused]] bool MpChestLoose(const Guest& g, uint64_t native, float* x, float* y, int* a, int* b) {
    for (int o1 = 8; o1 < 0x80; o1 += 8) {
        const uint64_t P = g.ptr(native + o1); if (!MpPtrOk(g, P)) continue;
        float px, py;
        if (MpWorld(g, P, 0x28, 0x18, 0x20, 0, &px, &py) && px > -70 && px < 70 && py > -250 && py < 40) { *x = px; *y = py; *a = o1; *b = -1; return true; }
        for (int o2 = 0; o2 < 0x80; o2 += 8) {
            const uint64_t Q = g.ptr(P + o2); if (!MpPtrOk(g, Q)) continue;
            if (MpWorld(g, Q, 0x28, 0x18, 0x20, 0, &px, &py) && px > -70 && px < 70 && py > -250 && py < 40) { *x = px; *y = py; *a = o1; *b = o2; return true; }
        }
    }
    return false;
}
bool MpPos(const Guest& g, uint64_t native, float* x, float* y) {
    const MarkerProbe& m = g_mp;
    const uint64_t T = MpTransform(g, native);
    return T && MpWorld(g, T, m.o4, m.xoff, m.poff, m.stride, x, y);
}
bool DaveOwnPos(const Guest& g, uint64_t igm, float* x, float* y) {
    const uint64_t comp = igm ? g.ptr(igm + 0xA0) : 0, nat = comp ? g.ptr(comp + 0x10) : 0;
    return g_mp.fixed && nat && MpPos(g, nat, x, y);
}
void MpFindPath(const Guest& g, uint64_t native, float px, float py) {
    MarkerProbe& m = g_mp;
    m.stage = 0; m.best = 1e30f;
    for (int o1 = 8; o1 < 0x80; o1 += 8) {
        const uint64_t G = g.ptr(native + o1); ++m.reads; if (!MpPtrOk(g, G)) continue;
        for (int o2 = 8; o2 < 0x80; o2 += 8) {
            const uint64_t C = g.ptr(G + o2); ++m.reads; if (!MpPtrOk(g, C)) continue;
            for (int o3 = 0; o3 < 0x40; o3 += 8) {
                const uint64_t T = g.ptr(C + o3); ++m.reads;
                if (!MpPtrOk(g, T) || T == native || g.ptr(T + o1) != G) continue;   // a component of the same GameObject
                if (m.stage < 1) { m.stage = 1; m.o1 = o1; m.o2 = o2; m.o3 = o3; }
                for (int o4 = 8; o4 < 0x100; o4 += 8) {
                    const uint64_t H = g.ptr(T + o4); ++m.reads; if (!MpPtrOk(g, H)) continue;
                    bool ok = false; const int32_t idx = g.s32(T + o4 + 8, &ok);
                    if (!ok || idx < 0 || idx > 200000) continue;
                    for (int xo = 0; xo < 0x80; xo += 8) {
                        for (int po = 0; po < 0x80; po += 8) {
                            if (po == xo) continue;
                            for (int s = 0; s < 3; ++s) {
                                float x, y; ++m.tries;
                                if (!MpWorld(g, T, o4, xo, po, s, &x, &y)) continue;
                                if (m.stage < 2) m.stage = 2;
                                const float d = (x > px ? x - px : px - x) + (y > py ? y - py : py - y);
                                if (d < m.best) {
                                    m.best = d; m.bx = x; m.by = y;
                                    m.b1 = o1; m.b2 = o2; m.b3 = o3; m.b4 = o4; m.bxo = xo; m.bpo = po; m.bl = s;
                                }
                                if (MpNear(x, px) && MpNear(y, py)) {
                                    m.o1 = o1; m.o2 = o2; m.o3 = o3; m.o4 = o4; m.xoff = xo; m.poff = po; m.stride = s;
                                    m.state = 1; return;
                                }
                            }
                        }
                    }
                    if (m.tries > 400000) goto done;
                }
            }
        }
    }
done:
    // 0.34.4: nothing within 0.3, but 0.34.3's closest try (L1, hierarchy +0x28, array +0x18, parents
    // +0x20 -- Unity's usual layout) was off by a few units: take the closest as a candidate and show
    // it live against the camera, to see whether it follows Dave with a fixed offset
    if (m.stage >= 2 && m.best < 20.0f) {
        m.o1 = m.b1; m.o2 = m.b2; m.o3 = m.b3; m.o4 = m.b4; m.xoff = m.bxo; m.poff = m.bpo; m.stride = m.bl;
        m.state = 3; return;
    }
    m.state = 2;
}
// sorted chest addresses for the spawner watch (0.36.7: rebuilt whenever a chest is added, so the
// watch works from the first pass instead of the second)
void MpSrtRebuild(MarkerProbe& m) {
    m.nsrt = 0;
    for (int i = 0; i < m.nkeep; ++i) if (!m.dead[i]) { m.srt[m.nsrt] = m.keep[i]; m.sidx[m.nsrt] = static_cast<uint8_t>(i); ++m.nsrt; }
    for (int i = 1; i < m.nsrt; ++i) {
        const uint64_t a = m.srt[i]; const uint8_t b = m.sidx[i]; int j = i - 1;
        while (j >= 0 && m.srt[j] > a) { m.srt[j + 1] = m.srt[j]; m.sidx[j + 1] = m.sidx[j]; --j; }
        m.srt[j + 1] = a; m.sidx[j + 1] = b;
    }
    m.smin = m.nsrt ? m.srt[0] : 0; m.smax = m.nsrt ? m.srt[m.nsrt - 1] : 0;
}
uint8_t g_pkflag[kPk][2];
uint8_t g_pkraw[kPk][16];   // 0.38.1: each pickup's native +0x40..+0x4f, for DIAG
void MpRefreshPickup(const Guest& g, MarkerProbe& m, int i) {
    m.ph[i] = 0; m.pactual[i] = 0; m.powned[i] = 0;
    if (m.pdead[i]) return;
    {   // 0.37.37: the false ammo near the dive start has 1.0f (00 00 80 3f) at its native component +0x4c;
        // all 17 real ammo pickups read on device had 0 there (0.37.36 DIAG "ODD" line)
        uint8_t b[4] = {}; const uint64_t nn = g.ptr(m.pk[i] + 0x10);
        m.pidle[i] = nn && g.read(nn + 0x4c, b, 4) && b[2] == 0x80 && b[3] == 0x3f ? 1 : 0;
        memset(g_pkraw[i], 0, 16); if (nn) g.read(nn + 0x40, g_pkraw[i], 16);   // 0.38.1: for DIAG (the nearest ammo mark)
        {   // 0.38.4: the GameObject's byte +0x30 (06 in play, 05 for the false ammo)
            const uint64_t G = (nn && m.o1) ? g.ptr(nn + static_cast<uint64_t>(m.o1)) : 0; uint8_t v = 0xFF;
            if (!(G && g.read(G + 0x30, &v, 1))) v = 0xFF;
            g_pkg30[i] = v;
        }
        // ... and the pickup's own flags: m_IsInteractable @0x6C, usePhysics @0x35 (0xFF = not readable)
        g_pkflag[i][0] = g.read(m.pk[i] + 0x6C, &g_pkflag[i][0], 1) ? g_pkflag[i][0] : 0xFF;
        g_pkflag[i][1] = g.read(m.pk[i] + 0x35, &g_pkflag[i][1], 1) ? g_pkflag[i][1] : 0xFF;
    }
    const uint64_t obj = m.pk[i], k = g.ptr(obj), nat = g.ptr(obj + 0x10);
    const uint64_t cmd = g.ptr(obj + 0x20), ck = cmd ? g.ptr(cmd) : 0;
    if ((k != m.pick_k && !MpIn(m.psubk, m.npsubk, k)) || !nat || !MpIn(m.pcmdk, m.npcmdk, ck)) {
        m.pdead[i] = 1; m.ptid[i] = 0; m.pkind[i] = 0; m.pspw[i] = 0; return;
    }
    if (m.pspw[i]) {
        m.powned[i] = MpSpawnerOwns(g, m.pspw[i], obj, true) ? 1 : 0;
        if (!m.powned[i]) m.pspw[i] = 0;
    }
    float x = 0, y = 0;
    const bool placed = MpPos(g, nat, &x, &y) && x > -90 && x < 90 && y > -300 && y < 40 &&
        !(x > -0.01f && x < 0.01f && y > -0.01f && y < 0.01f);
    const uint64_t idata = g.ptr(obj + 0x78);
    bool ok = false; int32_t t = idata ? g.s32(idata + 0x10, &ok) : 0;
    m.pactual[i] = ok && t > 0;
    if (!m.pactual[i]) {
        uint8_t up = 0;
        t = g.read(obj + 0x34, &up, 1) && up == 1 ? g.s32(obj + 0x38) : 0;
    }
    if (t < 0) t = 0;
    if (t != m.ptid[i]) { m.ptid[i] = t; m.pkind[i] = 0; g_pk0[i] = 0; g_pkmv[i] = 0; g_pkmoves[i] = 0; }   // 0.38.2: + the move watch
    if (!m.pactual[i]) m.pkind[i] = 0;  // preset-only objects are never ammo markers
    if (placed && t > 0) {
        m.pxx[i] = x; m.pyy[i] = y; m.ph[i] = 1;
        if (!g_pk0[i]) { g_pk0[i] = 1; g_pk0x[i] = x; g_pk0y[i] = y; }
        else if (!g_pkmv[i] && g_pkflag[i][1] == 0) {   // 0.38.2: no physics, yet it keeps moving -> carried along (follows Dave)
            const float dx = x - g_pk0x[i], dy = y - g_pk0y[i];
            if (dx * dx + dy * dy > kPkStep * kPkStep) { if (++g_pkmoves[i] >= 2) g_pkmv[i] = 1; }
            g_pk0x[i] = x; g_pk0y[i] = y;               // (from the last read: a one-off jump counts once)
        }
    }
}
// 0.36.12: a word equal to PickupInstanceItem's class (or a learned child class): learn children the
// same way as chests (a class whose parent @0x58 is PickupInstanceItem), check the command field is a
// *_SO object, keep it
void MpPickupHit(const Guest& g, MarkerProbe& m, uint64_t obj, const uint8_t* p, int o) {
    if (g.ptr(obj) == m.pick_k && m.npsubk < 8 && m.slow < 10 && obj >= 0x58 + 0x1000) {
        const uint64_t K = obj - 0x58;
        if (K != m.pick_k && !MpIn(m.psubk, m.npsubk, K)) {
            ++m.slow;
            char cn[48]; const uint64_t nm = g.ptr(K + 0x10);
            if (MpPtrOk(g, K) && nm >= 0x1000000 && nm < 0x100000000000ULL && g.cstr(nm, cn, sizeof cn) && cn[0] >= 'A' && cn[0] <= 'Z' &&
                MpHas(cn, "InstanceItem") && !eq(cn, "PickupInstanceItem") && !MpHas(cn, "Chest"))
                m.psubk[m.npsubk++] = K;
        }
    }
    uint64_t nat = 0; if (o + 0x18 <= 4096) memcpy(&nat, p + o + 0x10, 8); else nat = g.ptr(obj + 0x10);
    if (!(nat >= 0x1000000 && nat < 0x100000000000ULL)) return;
    uint64_t cmd = 0; if (o + 0x28 <= 4096) memcpy(&cmd, p + o + 0x20, 8); else cmd = g.ptr(obj + 0x20);
    if (!MpPtrOk(g, cmd)) return;
    const uint64_t ck = g.ptr(cmd); if (!ck) return;
    bool real = MpIn(m.pcmdk, m.npcmdk, ck);
    if (!real) {
        if (MpIn(m.badk, m.nbadk, ck) || m.slow >= 12) return;
        ++m.slow;
        real = MpChestReal(g, obj);          // the command's class name ends in _SO
        if (real && m.npcmdk < 8) m.pcmdk[m.npcmdk++] = ck;
        if (!real) { MpRing(m.badk, &m.nbadk, ck); return; }
    }
    for (int q = 0; q < m.npk; ++q) if (m.pk[q] == obj && !m.pdead[q]) return;
    int slot = -1;
    for (int q = 0; q < m.npk && slot < 0; ++q) if (m.pdead[q]) slot = q;
    if (slot < 0 && m.npk < kPk) slot = m.npk++;
    if (slot < 0) return;
    m.pk[slot] = obj; m.pdead[slot] = 0; m.ph[slot] = 0; m.ptid[slot] = 0; m.pkind[slot] = 0; m.pxx[slot] = m.pyy[slot] = 0; m.pspw[slot] = 0; m.pactual[slot] = m.powned[slot] = m.pidle[slot] = 0; ++m.npicks;
    g_pk0[slot] = 0; g_pkmv[slot] = 0; g_pkmoves[slot] = 0; g_pkg30[slot] = 0xFF;   // 0.38.2 / 0.38.4
    MpPickupSrtRebuild(m);
}
// 0.37.22 (round 36): EscapePodZone / EscapeBellZone have no TypeInfo in the dump, so their class is found
// by name, once per session: (1) the name strings near InstanceItemChest's own name, (2) the class
// whose name pointer (+0x10) is that string, near InstanceItemChest's class. A few pages per tick.
struct KlassFind { int stage; uint64_t base, cur, end; uint64_t nm[2], k[2]; int pages; } g_kf;
const char* const kPodNames[2] = {"EscapePodZone", "EscapeBellZone"};
uint64_t g_podk[2];
void KlassFindStep(const Instance& in, const Guest& g) {
    KlassFind& f = g_kf;
    if (f.stage >= 3) return;
    const uint64_t ck = g.ptr(in.main_base + kChestTI);
    if (!ck) return;
    if (f.stage == 0) {
        const uint64_t np = g.ptr(ck + 0x10);
        if (!np) return;
        f.base = np & ~0xFFFULL; f.cur = f.base > 0x2000000 ? f.base - 0x2000000 : 0x1000; f.end = f.base + 0x2000000; f.stage = 1;
    }
    static uint8_t pg[4096 + 32];
    for (int n = 0; n < 48 && f.stage < 3; ++n) {
        if (f.cur >= f.end) {
            if (f.stage == 1) {       // strings done: now the classes
                if (!f.nm[0] && !f.nm[1]) { f.stage = 3; return; }
                const uint64_t kb = ck & ~0xFFFULL;
                f.cur = kb > 0x4000000 ? kb - 0x4000000 : 0x1000; f.end = kb + 0x4000000; f.stage = 2;
            } else { f.stage = 3; return; }
        }
        ++f.pages;
        if (!g.read(f.cur, pg, 4096 + 32)) { if (!g.read(f.cur, pg, 4096)) { f.cur += 4096; continue; } memset(pg + 4096, 0, 32); }
        if (f.stage == 1) {
            for (int q = 0; q < 2; ++q) {
                if (f.nm[q]) continue;
                const char* w = kPodNames[q]; const int L = len(w);
                for (int i = 1; i < 4096; ++i)
                    if (pg[i] == static_cast<uint8_t>(w[0]) && pg[i - 1] == 0 && pg[i + L] == 0 && [&]{ for (int z = 0; z < L; ++z) if (pg[i + z] != static_cast<uint8_t>(w[z])) return false; return true; }()) { f.nm[q] = f.cur + static_cast<uint64_t>(i); break; }
            }
        } else {
            for (int o = 0x10; o + 8 <= 4096; o += 8) {
                uint64_t v; memcpy(&v, pg + o, 8);
                for (int q = 0; q < 2; ++q) if (v && v == f.nm[q] && !f.k[q]) {
                    const uint64_t K = f.cur + static_cast<uint64_t>(o) - 0x10;
                    char cn[32]; if (g.cstr(g.ptr(K + 0x10), cn, sizeof cn) && eq(cn, kPodNames[q]) && g.ptr(K)) { f.k[q] = K; g_podk[q] = K; }
                }
            }
            if ((f.k[0] || !f.nm[0]) && (f.k[1] || !f.nm[1])) { f.stage = 3; return; }
        }
        f.cur += 4096;
    }
}
uint64_t IngredientTable(Instance& in, const Guest& g);
bool IsFishTid(int32_t t);
constexpr int kAmConds = 4;
int AmPinTargets(int32_t* out, int cap);
void MissionPinStep(Instance&, const Guest&, const EdenDsmodHostApi* h) {
    // 0.37.48: 0.37.47 asked the item / ingredient tables and refused the Amethyst and Microphone (not in either).
    // Now only targets that are known not to be things are refused: 9xxxxxx ids are characters and events
    // (9021002 the noisy customer, 9070015 an NPC unlock, 9060002 a customer talk); items 1xxxxxx / 3xxxxxx and
    // fish 2xxxxxx stay pinnable.
    if (g_ticks % 30 != 17 || !h->get_i64) return;
    for (int a = 0; a < 3; ++a) {
        Buf cn; cn.add("cn").num(a);
        const int64_t n = h->get_i64(h->userdata, cn.s, 0);
        bool ok = true;
        for (int b = 0; b < 6 && b < n && ok; ++b) {
            Buf ct; ct.add("ctgt").num(a).addc('_').num(b);
            const int64_t t = h->get_i64(h->userdata, ct.s, 0);
            ok = !(t >= 9000000 && t < 10000000);
        }
        g_canpin[a] = ok ? 1 : 0;
        if (!ok && g_pin_md && g_mi.md[a] == g_pin_md) g_pin_md = 0;
    }
}
void MarkerProbeStep(const Instance& in, const Guest& g, const EdenDsmodHostApi* h, uint64_t igm, bool diving) {
    MarkerProbe& m = g_mp;
    m.pod_k[0] = g_podk[0]; m.pod_k[1] = g_podk[1];
    if (g_mp_reset) { g_mp_reset = false; diving = false; }   // 0.37.6: a new level: as a new dive
    if (!diving) {                       // a new dive starts over
        if (m.passes || m.cur || m.state || m.fixed) {
            memset(&m, 0, sizeof m);
            if (g_hot_nlo && g_hot_nhi > g_hot_nlo) { g_hot_lo = g_hot_nlo; g_hot_hi = g_hot_nhi; }
            g_hot_nlo = g_hot_nhi = 0;
        }
        Buf a; a.add("waiting for a dive"); memcpy(m.l1, a.s, a.n + 1);
        return;
    }
    float cpos[2] = {};
    const bool have_c = g_map_cam && g.read(g_map_cam + 0xC8, cpos, sizeof cpos);
    // 1. census, about 8 MB per sample
    if (!m.lo && h->get_heap_begin && h->get_heap_end && igm) {
        // the heap the system reports, widened to take in where the game's objects actually are
        // (0.34.1 showed the game's objects sit outside the reported heap: scan 2 GB either side of them)
        const uint64_t a = igm & ~0xFFFFFULL;
        const uint64_t lo = a > 0x80000000ULL ? a - 0x80000000ULL : 0x1000, hi = a + 0x80000000ULL;
        m.lo = lo; m.hi = hi; m.cur = lo; m.igm = igm;
        m.flo = lo; m.fhi = hi;
        if (g_hot_lo && g_hot_hi > g_hot_lo) {            // the last dive's window, 32 MB either side
            const uint64_t wl = (g_hot_lo & ~0xFFFFFULL) - 0x2000000, wh = (g_hot_hi | 0xFFFFF) + 0x2000001;
            if (wl > lo && wh < hi && wh > wl) { m.lo = wl; m.hi = wh; m.cur = wl; m.hotpass = 1; }
        }
        m.ctl_k = g.ptr(igm);           // control: the InGameManager's own class must be found at least once
        // 0.36.8: the path 0.34.x searched for every dive is known -- native +0x20 GameObject, +0x20 its
        // component list, +0x8 the Transform; hierarchy T+0x28 / index +0x30, nodes H+0x18, parents H+0x20
        m.o1 = 0x20; m.o2 = 0x20; m.o3 = 0x8; m.o4 = 0x28; m.idxoff = 8; m.xoff = 0x18; m.poff = 0x20; m.stride = 0;
        m.state = 1; m.fixed = 1;
    }
    if (!m.chest_k) { const uint64_t t = g.ptr(in.main_base + kChestTI); m.chest_k = t; }
    if (!m.loot_k) { const uint64_t t = g.ptr(in.main_base + kLootTI); m.loot_k = t; }
    if (!m.pick_k) { const uint64_t t = g.ptr(in.main_base + kPickupTI); m.pick_k = t; }
    {   // 0.37.79: seed the pull-out child class (the census learns children only when it meets their class data)
        const uint64_t po = g_po_seeded ? 0 : g.ptr(in.main_base + kPullOutTI);
        if (po && !MpIn(m.psubk, m.npsubk, po) && m.npsubk < 8) { m.psubk[m.npsubk++] = po; g_po_seeded = 1; }
        else if (po) g_po_seeded = 2;
    }
    // Eight bounded refreshes per tick: a full 256-slot list updates in <=32 samples.
    if (m.fixed && m.npk) {
        const int count = m.npk < 8 ? m.npk : 8;
        for (int r = 0; r < count; ++r) {
            const int i = m.prr++ % m.npk;
            if (m.prr >= m.npk) m.prr = 0;
            MpRefreshPickup(g, m, i);
        }
    }
    if (m.fixed && m.npod) {   // 0.37.22: escape pods (they do not move): one per sample until placed; used = its pod called
        static int pr; const int i = (pr = (pr + 1) % m.npod);
        const uint64_t obj = m.pods[i], nat = g.ptr(obj + 0x10);
        float x, y;
        if (!m.podh[i] && nat && MpPos(g, nat, &x, &y) && x > -150 && x < 150 && y > -400 && y < 60 && !(x > -0.01f && x < 0.01f && y > -0.01f && y < 0.01f)) {
            m.podx[i] = x; m.pody[i] = y; m.podh[i] = 1;
        }
        m.podused[i] = g.ptr(obj + 0x38) ? 1 : 0;
    }
    // the items the missions in progress ask for (MissionConditionData.Target, read by the package's points)
    if (g_ticks % 30 == 13 && h->get_i64) {
        m.nmtgt = 0;
        for (int a = 0; a < 3; ++a) {
            if (!g_pin_md || g_mi.md[a] != g_pin_md) continue;   // 0.37.20: only the pinned mission's items go on the map
            Buf cn; cn.add("cn").num(a);
            const int64_t n = h->get_i64(h->userdata, cn.s, 0);
            for (int b = 0; b < 6 && b < n; ++b) {
                Buf ct; ct.add("ctgt").num(a).addc('_').num(b);
                const int64_t t = h->get_i64(h->userdata, ct.s, 0);
                // 0.37.50: a task already done (the Stone Tablet 1/1) leaves the map
                Buf cw, cx; cw.add("cnow").num(a).addc('_').num(b); cx.add("cmax").num(a).addc('_').num(b);
                const int64_t now = h->get_i64(h->userdata, cw.s, 0), mx = h->get_i64(h->userdata, cx.s, 0);
                if (mx > 0 && now >= mx) continue;
                if (t > 0 && m.nmtgt < 18) m.mtgt[m.nmtgt++] = static_cast<int32_t>(t);
            }
        }
        {   // 0.38.57: a mission pinned from MISSIONS that the tracker does not show: its open rows' targets
            bool shown = false; for (int a = 0; a < 3; ++a) shown = shown || (g_pin_md && g_mi.md[a] == g_pin_md);
            int32_t pt[kAmConds]; const int pn = shown ? 0 : AmPinTargets(pt, kAmConds);
            for (int q = 0; q < pn; ++q) if (pt[q] > 0 && m.nmtgt < 18) m.mtgt[m.nmtgt++] = pt[q];
        }
        {   // 0.37.74: the pinned EcoWatcher mission's goals not yet met go on the map too (pickups: starfish, shells ...)
            int32_t id[kPlEco], hv[kPlEco], nd[kPlEco]; const char* nm[kPlEco]; const char* ic[kPlEco];
            const int k = kEcoMapMarks ? EcoPinGoals(id, hv, nd, nm, ic, kPlEco) : 0;
            for (int q = 0; q < k; ++q) if (id[q] > 0 && !(nd[q] > 0 && hv[q] >= nd[q]) && m.nmtgt < 18) m.mtgt[m.nmtgt++] = id[q];
        }
    }
    m.slow = 0;
    if (m.lo && m.chest_k && h->get_read_pointer && !m.passes) {
        // 0.36.13: 2 MB a sample again (0.36.7's "freeze" was the GPU driver crash, not the scan)
        for (int pg = 0; pg < 512 && m.cur < m.hi; ++pg, m.cur += 4096) {
            const uint8_t* p = h->get_read_pointer(h->userdata, m.cur, 4096);
            if (!p) continue;
            if (have_c && !m.c0) { m.c0 = true; m.c0x = cpos[0]; m.c0y = cpos[1]; }
            if (kMpResearch && have_c && m.ncopy < 64) {
                for (int o = 0; o + 8 <= 4096 && m.ncopy < 64; o += 4) {
                    float f[2]; memcpy(f, p + o, 8);
                    const float dx = f[0] - cpos[0], dy = f[1] - cpos[1];
                    if (dx > -0.02f && dx < 0.02f && dy > -0.02f && dy < 0.02f && m.cur + o != g_map_cam + 0xC8) {
                        m.copy[m.ncopy] = m.cur + o; m.copy_live[m.ncopy] = 1; ++m.ncopy;
                    }
                }
            }
            for (int o = 0; o < 4096; o += 8) {
                uint64_t v; memcpy(&v, p + o, 8);
                MpPickupSpawnerHit(g, m, m.cur + o, v);
                if (m.nsrt && v >= m.smin && v <= m.smax) {   // a reference to one of our chests?
                    int lo2 = 0, hi2 = m.nsrt - 1;
                    while (lo2 <= hi2) {
                        const int mid = (lo2 + hi2) / 2;
                        if (m.srt[mid] == v) {
                            const int ci2 = m.sidx[mid];
                            ++m.spawn_hits; MpHot(m.cur + o);
                            // 0.36.4: the spawner's class is looked up in what is already known (no
                            // per-hit name reads -- 0.35.x ran out of its budget on other references)
                            if (m.kind[ci2] <= 1 || !m.spw[ci2]) {
                                const uint64_t sp = m.cur + o - 0x68;
                                uint64_t k = 0;
                                if (o >= 0x68) memcpy(&k, p + o - 0x68, 8); else k = g.ptr(sp);
                                if (k && MpSpawnerOwns(g, sp, v, false)) {
                                    int at = -1;
                                    for (int q = 0; q < m.nspk; ++q) if (m.spk[q] == k) at = q;
                                    if (at >= 0) { m.kind[ci2] = m.spkind[at]; m.spw[ci2] = sp; ++m.spawn_named; }
                                    else if (!MpIn(m.nspbad, m.nnspbad, k) && m.slow < 8) {
                                        ++m.slow;
                                        char cn[48];
                                        if (MpPtrOk(g, k) && g.cstr(g.ptr(k + 0x10), cn, sizeof cn) && starts(cn, "Spawner")) {
                                            if (m.nspk < 16) { m.spk[m.nspk] = k; m.spkind[m.nspk] = MpKindOf(cn); ++m.nspk; MpAddName(m.spn, sizeof m.spn, cn + 7); }
                                            m.kind[ci2] = MpKindOf(cn); m.spw[ci2] = sp; ++m.spawn_named;
                                        } else MpRing(m.nspbad, &m.nnspbad, k);
                                    }
                                }
                            }
                            break;
                        }
                        if (m.srt[mid] < v) lo2 = mid + 1; else hi2 = mid - 1;
                    }
                }
                if (v == m.ctl_k && v) { ++m.ctl; continue; }
                if (v && (v == m.pod_k[0] || v == m.pod_k[1])) {      // 0.37.22: escape pod zones
                    const uint64_t obj = m.cur + o;
                    uint64_t nat = 0; if (o + 0x18 <= 4096) memcpy(&nat, p + o + 0x10, 8); else nat = g.ptr(obj + 0x10);
                    bool have = false; for (int q = 0; q < m.npod; ++q) have = have || m.pods[q] == obj;
                    // 0.37.23: a real zone holds its EscapePodCallCommand_SO (pod zone +0x28, bell zone +0x30);
                    // the class pointer alone also sits in many other places (0.37.22 filled up on those)
                    bool real = false;
                    if (!have && nat >= 0x1000000 && nat < 0x100000000000ULL && m.podchk < 4000) {
                        ++m.podchk;
                        const uint64_t cmd = g.ptr(obj + (v == m.pod_k[0] ? 0x28 : 0x30));
                        char cn[40];
                        real = cmd && MpPtrOk(g, cmd) && g.cls(cmd, cn, sizeof cn) && starts(cn, "EscapePodCall");
                    }
                    if (real && m.npod < 24) { m.pods[m.npod] = obj; m.podh[m.npod] = 0; m.podused[m.npod] = 0; ++m.npod; }
                    continue;
                }
                int sub = -1;
                if (v != m.chest_k && m.nsubk && v) for (int q = 0; q < m.nsubk; ++q) if (m.subk[q] == v) sub = q;
                // 0.36.12: pickups
                bool pick = v && v == m.pick_k;
                if (!pick && v && m.npsubk) for (int q = 0; q < m.npsubk; ++q) if (m.psubk[q] == v) pick = true;
                if (pick) { MpPickupHit(g, m, m.cur + o, p, o); continue; }
                if (v != m.chest_k && sub < 0 && (v != m.loot_k || !v)) continue;
                const uint64_t obj = m.cur + o;
                uint64_t nat = 0; if (o + 0x18 <= 4096) memcpy(&nat, p + o + 0x10, 8); else nat = g.ptr(obj + 0x10);
                const bool live = nat >= 0x1000000 && nat < 0x100000000000ULL;
                // 0.36.4: a class whose parent (Il2CppClass+0x58) is InstanceItemChest -- a kind of chest
                // the exact-class check missed (weapon chests?); learned from where the pointer sits
                if (v == m.chest_k && m.nsubk < 8 && m.slow < 10 && obj >= 0x58 + 0x1000) {
                    const uint64_t K = obj - 0x58;
                    if (K != m.chest_k && !MpIn(m.subk, m.nsubk, K)) {
                        ++m.slow;
                        char cn[48];
                        const uint64_t nm = g.ptr(K + 0x10);
                        if (MpPtrOk(g, K) && nm >= 0x1000000 && nm < 0x100000000000ULL && g.cstr(nm, cn, sizeof cn) && cn[0] >= 'A' && cn[0] <= 'Z' &&
                            MpHas(cn, "Chest") && !eq(cn, "InstanceItemChest")) {
                            m.subk[m.nsubk] = K; m.subkind[m.nsubk] = MpKindOf(cn); ++m.nsubk;
                            MpAddName(m.subn, sizeof m.subn, cn);
                        }
                    }
                }
                // 0.34.12: the class pointer also sits in many places that are not chests (CTL counted ~700
                // "InGameManagers"); keep only real objects: their command field is a *_SO ScriptableObject
                if (v == m.chest_k || sub >= 0) {
                    ++m.chests;
                    if (!live) continue;
                    uint64_t cmd = 0; if (o + 0x28 <= 4096) memcpy(&cmd, p + o + 0x20, 8); else cmd = g.ptr(obj + 0x20);
                    if (!MpPtrOk(g, cmd)) continue;
                    // 0.36.4: every command class ending in _SO counts (0.35.x kept only the first one seen)
                    bool real = false;
                    const uint64_t ck = g.ptr(cmd);
                    if (!ck) continue;
                    if (MpIn(m.cmdks, m.ncmdk, ck)) real = true;
                    else if (MpIn(m.badk, m.nbadk, ck)) real = false;
                    else if (m.slow < 12) {
                        ++m.slow;
                        real = MpChestReal(g, obj);
                        if (real && m.ncmdk < 8) {
                            m.cmdks[m.ncmdk++] = ck; if (!m.cmdk) m.cmdk = ck;
                            char cn[48]; if (g.cls(cmd, cn, sizeof cn)) MpAddName(m.cmdn, sizeof m.cmdn, cn);
                        }
                        if (!real) MpRing(m.badk, &m.nbadk, ck);
                    }
                    else { ++m.unsure; continue; }
                    if (real) {
                        MpHot(obj);
                        bool have = false;
                        for (int q = 0; q < m.nkeep && !have; ++q) have = m.keep[q] == obj && !m.dead[q];
                        if (!have) {
                            ++m.live_chests;
                            int slot = -1;
                            for (int q = 0; q < m.nkeep && slot < 0; ++q) if (m.dead[q]) slot = q;
                            if (slot >= 0) { ++m.reused; --m.ndead; }
                            else if (m.nkeep < kMpKeep) slot = m.nkeep++;
                            if (slot >= 0) {
                                m.keep[slot] = obj; m.dead[slot] = 0; m.mhow[slot] = 0; m.opened[slot] = 0; m.seen[slot] = 0;
                                m.kind[slot] = 0; m.owned[slot] = 0; m.tries8[slot] = 0;
                                m.mx[slot] = m.my[slot] = 0; m.spw[slot] = 0; m.mat[slot] = 0; m.matok[slot] = 0; m.matn[slot] = 0; m.mst[slot] = 0; m.mis[slot] = 0; m.noanim[slot] = 0; m.tid[slot] = 0;
                                MpSrtRebuild(m);
                            }
                        }
                    }
                }
                else { ++m.loot; if (live) ++m.live_loot; }
            }
        }
        if (m.cur >= m.hi) {
            if (m.hotpass) { m.hotpass = 0; m.lo = m.flo; m.hi = m.fhi; m.cur = m.flo; MpSrtRebuild(m); }   // then the rest
            else { m.passes = 1; ++m.rounds; }
        }
    }
    {
        Buf a; a.add(m.passes ? "DONE " : "SCAN ");
        if (m.hi > m.lo) a.num(static_cast<int64_t>((m.cur - m.lo) * 100 / (m.hi - m.lo))).add("% ");
        a.add("REAL CHESTS ").num(m.live_chests).add(" (of ").num(m.chests).add(" hits, ").num(m.unsure).add(" unsure)").add(" LOOT ").num(m.live_loot).addc('/').num(m.loot);
        a.add(" CTL ").num(m.ctl).add(" RANGE ").num(static_cast<int64_t>((m.hi - m.lo) >> 20)).add("MB K ").addc(m.chest_k ? 'y' : 'n').addc(m.loot_k ? 'y' : 'n');
        memcpy(m.l1, a.s, a.n + 1);
    }
    // the copies that still follow Dave (within 1 unit of the camera's copy)
    if (kMpResearch && have_c) {
        for (int i = 0; i < m.ncopy; ++i) {
            if (!m.copy_live[i]) continue;
            float f[2];
            if (!g.read(m.copy[i], f, 8)) { m.copy_live[i] = 0; continue; }
            const float dx = f[0] - cpos[0], dy = f[1] - cpos[1];
            if (!(dx > -1.0f && dx < 1.0f && dy > -1.0f && dy < 1.0f)) m.copy_live[i] = 0;
        }
        Buf c; int live = 0;
        for (int i = 0; i < m.ncopy; ++i) live += m.copy_live[i];
        c.add("COPIES ").num(m.ncopy).add(" FOLLOWING ").num(live);
        const uint64_t T = g_mp.o1 ? MpTransform(g, g.ptr((igm ? g.ptr(igm + 0xA0) : 0) + 0x10)) : 0;
        const uint64_t H = T ? g.ptr(T + 0x28) : 0, ARR = H ? g.ptr(H + 0x18) : 0;
        c.add(" T ").hex(T).add(" ARR ").hex(ARR);
        int shown = 0;
        for (int i = 0; i < m.ncopy && shown < 3; ++i) {
            if (!m.copy_live[i]) continue;
            ++shown;
            const uint64_t a = m.copy[i];
            c.add(" | ").hex(a);
            if (ARR && a >= ARR && a - ARR < 0x1000000) c.add(" ARR+").hex(a - ARR);
            if (T && a >= T && a - T < 0x1000) c.add(" T+").hex(a - T);
        }
        memcpy(m.l7, c.s, c.n + 1);
        // which following copies look like Unity's record: a unit quaternion and a sane scale after the
        // position (48 apart: t @0 q @16 s @32; packed 40: t @0 q @12 s @28)
        Buf d; int trs = 0;
        uint64_t trsA[4]; int trsS[4];
        for (int i = 0; i < m.ncopy; ++i) {
            if (!m.copy_live[i]) continue;
            float f[12]; if (!g.read(m.copy[i], f, sizeof f)) continue;
            int st = 0;
            for (int L = 0; L < 2 && !st; ++L) {
                const int qo = L ? 3 : 4, so = L ? 7 : 8;
                const float n = f[qo] * f[qo] + f[qo + 1] * f[qo + 1] + f[qo + 2] * f[qo + 2] + f[qo + 3] * f[qo + 3];
                bool sc = true;
                for (int k = 0; k < 3; ++k) { const float v = f[so + k] < 0 ? -f[so + k] : f[so + k]; if (v < 0.001f || v > 1000.0f) sc = false; }
                if (n > 0.98f && n < 1.02f && sc) st = L ? 40 : 48;
            }
            if (st && trs < 4) { trsA[trs] = m.copy[i]; trsS[trs] = st; ++trs; }
        }
        const bool moved = m.c0 && ((cpos[0] - m.c0x) * (cpos[0] - m.c0x) + (cpos[1] - m.c0y) * (cpos[1] - m.c0y)) > 64.0f;
        if (m.passes && moved && trs && m.pass2 == 0) { m.pass2 = 1; m.cur2 = m.lo; m.nrec = 0; }
        if (m.pass2 == 1 && h->get_read_pointer) {
            for (int pg = 0; pg < 2048 && m.cur2 < m.hi; ++pg, m.cur2 += 4096) {
                const uint8_t* p = h->get_read_pointer(h->userdata, m.cur2, 4096);
                if (!p) continue;
                for (int o = 0; o < 4096 && m.nrec < 16; o += 8) {
                    uint64_t v; memcpy(&v, p + o, 8);
                    for (int t = 0; t < trs; ++t) {
                        const uint64_t A = trsA[t];
                        if (v && v <= A && A - v < 0x400000 && (A - v) % static_cast<uint64_t>(trsS[t]) == 0) {
                            m.rec_p[m.nrec] = m.cur2 + o; m.rec_v[m.nrec] = v; m.rec_a[m.nrec] = A; m.rec_s[m.nrec] = trsS[t]; ++m.nrec;
                            break;
                        }
                    }
                }
            }
            if (m.cur2 >= m.hi) m.pass2 = 2;
        }
        d.add("TRS ").num(trs);
        for (int t = 0; t < trs; ++t) { d.add(" ").hex(trsA[t]).add(t < trs ? (trsS[t] == 48 ? "/48" : "/40") : ""); }
        if (m.pass2 == 0) d.add(m.passes ? (moved ? " (no TRS copy)" : " swim 8+ units away to start pass 2") : " waiting for the scan");
        else if (m.pass2 == 1) d.add(" PASS2 ").num(static_cast<int64_t>((m.cur2 - m.lo) * 100 / (m.hi - m.lo))).add("% REFS ").num(m.nrec);
        else {
            d.add(" REFS ").num(m.nrec);
            // does Dave's transform lead to one of them? T+o -> H, H+x == the pointer's location
            const uint64_t T2 = g_mp.o1 ? MpTransform(g, g.ptr((igm ? g.ptr(igm + 0xA0) : 0) + 0x10)) : 0;
            bool linked = false;
            for (int r = 0; r < m.nrec && !linked; ++r) {
                for (int o = 8; o < 0x100 && !linked && T2; o += 8) {
                    const uint64_t H = g.ptr(T2 + o);
                    if (!H || m.rec_p[r] < H || m.rec_p[r] - H >= 0x100) continue;
                    const int64_t idx = static_cast<int64_t>((m.rec_a[r] - m.rec_v[r]) / static_cast<uint64_t>(m.rec_s[r]));
                    int ko = 0;
                    for (int k = 8; k <= 0x18 && !ko; k += 4) { bool ok = false; if (g.s32(T2 + o + k, &ok) == idx && ok) ko = k; }
                    d.add(" LINK T+").hex(static_cast<uint64_t>(o)).add(" H+").hex(m.rec_p[r] - H).add(" IDX ").num(idx).add(" @+").hex(static_cast<uint64_t>(ko));
                    if (ko && m.state != 1) {
                        m.o4 = o; m.xoff = static_cast<int>(m.rec_p[r] - H); m.poff = m.xoff + 8; m.idxoff = ko;
                        m.stride = m.rec_s[r] == 48 ? 0 : 1; m.state = 1;
                    }
                    linked = true;
                }
            }
            if (!linked) {
                for (int r = 0; r < m.nrec && r < 3; ++r) d.add(" | P ").hex(m.rec_p[r]).add(" +").hex(m.rec_a[r] - m.rec_v[r]);
                d.add(T2 ? " (no link from T)" : " (no T)");
            }
        }
        memcpy(m.l8, d.s, d.n + 1);
        // 0.34.7: the record itself (raw floats around it) and short pointer paths from Dave's own
        // native component / transform that land just below it (a list it is part of)
        if (trs && g_ticks % 30 == 9) {
            const uint64_t A = trsA[0];
            Buf e; e.add("REC ");
            float f[20];
            if (g.read(A - 32, f, sizeof f)) {
                for (int k = 0; k < 20; ++k) { if (k == 8) e.add(" ["); AddF(e, f[k]); e.addc(k == 9 ? ']' : ' '); }
            }
            memcpy(m.l9, e.s, e.n + 1);
            const uint64_t natD = g.ptr((igm ? g.ptr(igm + 0xA0) : 0) + 0x10);
            const uint64_t roots[2] = {natD, g_mp.o1 ? MpTransform(g, natD) : 0};
            Buf q; int found = 0;
            for (int r = 0; r < 2 && found < 4; ++r) {
                if (!roots[r]) continue;
                for (int a1 = 0; a1 < 0x100 && found < 4; a1 += 8) {
                    const uint64_t v1 = g.ptr(roots[r] + a1);
                    if (v1 && v1 <= A && A - v1 < 0x100000) { q.add(r ? "T+" : "N+").hex(static_cast<uint64_t>(a1)).add("=A-").hex(A - v1).addc(' '); ++found; continue; }
                    if (!MpPtrOk(g, v1)) continue;
                    for (int a2 = 0; a2 < 0x100 && found < 4; a2 += 8) {
                        const uint64_t v2 = g.ptr(v1 + a2);
                        if (v2 && v2 <= A && A - v2 < 0x100000) {
                            q.add(r ? "T+" : "N+").hex(static_cast<uint64_t>(a1)).add("+").hex(static_cast<uint64_t>(a2)).add("=A-").hex(A - v2);
                            bool ok = false; const int32_t i8 = g.s32(roots[r] + a1 + 8, &ok);
                            q.add(" i").num(ok ? i8 : -1).addc(' ');
                            ++found; continue;
                        }
                    }
                }
            }
            if (!found) q.add("no short path from Dave");
            memcpy(m.l10, q.s, q.n + 1);
        }
    }
    // 0.34.8: 0.34.7's record showed Unity's layout plainly -- {t, q, s} as float4s, 48 apart, the
    // first node of its list at the list's start -- and that the list hanging off Dave's transform
    // (T+0x28 -> hierarchy, +0x18 -> nodes) holds a node that follows Dave. So take that layout as is:
    // hierarchy T+0x28, index T+0x30, nodes H+0x18, parents H+0x20, and check it on that node.
    if (m.passes && !m.fixed && m.o1) {
        m.fixed = 1; m.state = 1; m.o4 = 0x28; m.idxoff = 8; m.xoff = 0x18; m.poff = 0x20; m.stride = 0;
    }
    if (kMpResearch && m.fixed) {
        float cp[2] = {};
        const bool hc = g_map_cam && g.read(g_map_cam + 0xC8, cp, sizeof cp);
        const uint64_t natD = g.ptr((igm ? g.ptr(igm + 0xA0) : 0) + 0x10);
        const uint64_t T = natD ? MpTransform(g, natD) : 0;
        const uint64_t H = T ? g.ptr(T + 0x28) : 0, ARR = H ? g.ptr(H + 0x18) : 0;
        // the node following Dave: the TRS copy that lies on a 48-byte step of this list
        int kidx = -1;
        for (int i = 0; i < m.ncopy && kidx < 0; ++i) {
            if (!m.copy_live[i] || !ARR || m.copy[i] < ARR) continue;
            const uint64_t d = m.copy[i] - ARR;
            if (d % 48 == 0 && d / 48 < 200000) kidx = static_cast<int>(d / 48);
        }
        Buf b; b.add("FIXED L48 T+28/+30 H+18/+20");
        bool ok0 = false; const int32_t own = T ? g.s32(T + 0x30, &ok0) : -1;
        float x = 0, y = 0;
        if (T && MpWorld(g, T, 0x28, 0x18, 0x20, 0, &x, &y)) { b.add(" OWN#").num(own).addc(' '); AddF(b, x); b.addc(','); AddF(b, y); }
        if (kidx >= 0) {
            m.force_idx = kidx + 1;
            const bool okk = MpWorld(g, T, 0x28, 0x18, 0x20, 0, &x, &y);
            m.force_idx = 0;
            b.add(" FOLLOW#").num(kidx).addc(' ');
            if (okk) { AddF(b, x); b.addc(','); AddF(b, y); } else b.add("-");
        } else b.add(" (no follower in the list)");
        if (hc) { b.add(" CAM "); AddF(b, cp[0]); b.addc(','); AddF(b, cp[1]); }
        memcpy(m.l2, b.s, b.n + 1);
    }
    // 2. Dave's position the engine's way
    float tgt[2] = {};
    const uint64_t comp = igm ? g.ptr(igm + 0xA0) : 0, nat = comp ? g.ptr(comp + 0x10) : 0;
    const bool cam = g_map_cam && g.read(g_map_cam + 0xC8, tgt, sizeof tgt);
    if (m.state == 0 && nat && cam && g_ticks % 30 == 20 && !m.fixed) { m.dx = tgt[0]; m.dy = tgt[1]; MpFindPath(g, nat, tgt[0], tgt[1]); }
    if (kMpResearch && !m.fixed) {
        Buf b;
        if (m.state == 0) b.add(nat ? (cam ? "SEARCHING" : "NO CAMERA") : "NO DAVE");
        else if (m.state == 3) {
            float x = 0, y = 0; const bool ok = nat && MpPos(g, nat, &x, &y);
            b.add("CANDIDATE NOW "); if (ok) { AddF(b, x); b.addc(','); AddF(b, y); } else b.add("-");
            b.add(" CAM "); AddF(b, tgt[0]); b.addc(','); AddF(b, tgt[1]);
            if (ok) { b.add(" DIFF "); AddF(b, x - tgt[0]); b.addc(','); AddF(b, y - tgt[1]); }
            b.add(" L").num(m.stride);
        }
        else if (m.state == 2) {
            b.add("NOT FOUND stage ").num(m.stage).add(" reads ").num(m.reads).add(" tries ").num(m.tries).add(" for "); AddF(b, m.dx); b.addc(','); AddF(b, m.dy);
            if (m.stage >= 1) b.add(" T@").hex(static_cast<uint64_t>(m.o1)).addc('/').hex(static_cast<uint64_t>(m.o2)).addc('/').hex(static_cast<uint64_t>(m.o3));
            if (m.stage >= 2) {
                b.add(" BEST "); AddF(b, m.bx); b.addc(','); AddF(b, m.by);
                b.add(" @").hex(static_cast<uint64_t>(m.b3)).addc('/').hex(static_cast<uint64_t>(m.b4)).addc('/').hex(static_cast<uint64_t>(m.bxo))
                 .addc('/').hex(static_cast<uint64_t>(m.bpo)).add(" L").num(m.bl);
            }
        }
        else {
            float x = 0, y = 0; const bool ok = nat && MpPos(g, nat, &x, &y);
            b.add("PATH ").hex(static_cast<uint64_t>(m.o1)).addc(' ').hex(static_cast<uint64_t>(m.o2)).addc(' ').hex(static_cast<uint64_t>(m.o3))
             .addc(' ').hex(static_cast<uint64_t>(m.o4)).addc(' ').hex(static_cast<uint64_t>(m.xoff)).addc(' ').hex(static_cast<uint64_t>(m.poff)).add(" L").num(m.stride);
            b.add(" NOW "); if (ok) { AddF(b, x); b.addc(','); AddF(b, y); } else b.add("-");
            b.add(" CAM "); AddF(b, tgt[0]); b.addc(','); AddF(b, tgt[1]);
        }
        memcpy(m.l2, b.s, b.n + 1);
    }
    // 0.36.7: the second pass starts right away (the weapon / O2 kinds come from it), later ones every 1800 ticks
    if (m.passes && m.ci >= m.nkeep && m.rounds < 1000 && (m.rounds < 2 || g_ticks % 1800 == 0)) {
        m.passes = 0; m.cur = m.lo; m.rdone = 0; m.spawn_named = 0;
        MpSrtRebuild(m);
    }
    // opened chests (InstanceItemChest.IsOpen @0x63), a few per sample
    for (int r = 0; r < 8 && m.nkeep; ++r) {
        const int i = static_cast<int>((g_ticks * 8 + r) % static_cast<uint64_t>(m.nkeep));
        uint8_t op = 0; m.opened[i] = !g.read(m.keep[i] + 0x63, &op, 1) || op ? 1 : 0;
    }
    {   // 0.34.13: read the real chests, one per sample, then keep the answers
        // 0.36.4: still alive? (a chest unloaded by the game frees its memory: its class pointer goes)
        for (int r = 0; r < 4 && m.nkeep; ++r) {
            const int i = static_cast<int>((g_ticks * 4 + r) % static_cast<uint64_t>(m.nkeep));
            if (m.dead[i]) continue;
            const uint64_t k = g.ptr(m.keep[i]);
            const uint64_t cmd = g.ptr(m.keep[i] + 0x20), ck = cmd ? g.ptr(cmd) : 0;
            if ((k != m.chest_k && !MpIn(m.subk, m.nsubk, k)) || !g.ptr(m.keep[i] + 0x10) || !MpIn(m.cmdks, m.ncmdk, ck)) {
                m.dead[i] = 1; m.mhow[i] = 0; ++m.ndead;
            }
        }
        // 0.36.4: read every chest again and again (0.36.3 read each once: one that was not placed yet
        // stayed unplaced -- "NONE" -- for the whole dive); new ones first
        if (m.fixed && m.nkeep) {
            int i;
            if (m.ci < m.nkeep) i = m.ci++;
            else { m.rr = (m.rr + 1) % m.nkeep; i = m.rr; }
            if (m.dead[i]) i = -1;
            // 0.36.8: an unplaced chest is retried every 8th turn only
            if (i >= 0 && m.ci >= m.nkeep && !m.mhow[i] && (++m.tries8[i] & 7)) i = -1;
            if (i >= 0) {
            MpRefreshChestIdentity(g, m, i);
            const uint64_t cn = g.ptr(m.keep[i] + 0x10);
            float x = 0, y = 0; int a1, a2, a3; uint8_t how = 0;
            if (cn && MpPos(g, cn, &x, &y) && x > -70 && x < 70 && y > -250 && y < 40) how = 1;
            // 0.36.8: the brute-force "own" / "loose" searches (thousands of reads each, never placed a
            // chest in 0.36.4-0.36.6) are no longer run
            (void)a1; (void)a2; (void)a3;
            bool zero = false;
            if (how && x > -0.01f && x < 0.01f && y > -0.01f && y < 0.01f) { how = 0; zero = true; }   // an unplaced chest
            // 0.36.5: the spawner's own transform (the chest sits on it)
            if (!how && cn && m.owned[i]) {
                const uint64_t sn = g.ptr(m.spw[i] + 0x10);
                if (sn && MpPos(g, sn, &x, &y) && x > -90 && x < 90 && y > -300 && y < 40 && !(x > -0.01f && x < 0.01f && y > -0.01f && y < 0.01f)) how = 4;
            }
            if (!how) {   // where the chest's own read stops (1 no native .. 7 parent chain, 8 out of range, 9 at 0,0)
                const int st = MpStage(g, cn);
                m.mst[i] = static_cast<uint8_t>(zero ? 9 : st == 7 ? 10 + g_mp_why : st ? st : 8);
            } else m.mst[i] = 0;
            m.mhow[i] = how;
            if (how) { m.mx[i] = x; m.my[i] = y; }
            m.noanim[i] = g.ptr(m.keep[i] + 0x48) ? 0 : 1;
            {
                m.tid[i] = 0;
                const uint64_t hd = g.ptr(m.keep[i] + 0x40), hk = hd ? g.ptr(hd) : 0;
                if (hk && !m.hdk) { char hn[40]; if (g.cls(hd, hn, sizeof hn) && eq(hn, "InstanceItemSpawnHandler")) m.hdk = hk; }
                if (hk && hk == m.hdk) { bool okt = false; const int32_t t = g.s32(hd + 0x34, &okt); if (okt) m.tid[i] = t; }
            }
            if (m.ci >= m.nkeep) m.rdone = 1;
            }
        }
        Buf c;
        int n[5] = {}, stc[16] = {}, nsp = 0;
        for (int i = 0; i < m.ci && i < m.nkeep; ++i) if (!m.dead[i]) { ++n[m.mhow[i] < 5 ? m.mhow[i] : 0]; if (!m.mhow[i]) ++stc[m.mst[i] < 16 ? m.mst[i] : 0]; }
        for (int i = 0; i < m.nkeep; ++i) nsp += m.spw[i] != 0 && !m.dead[i];
        c.add(m.passes ? (m.rdone ? "DONE " : "READING ") : "waiting for the scan ").num(m.ci).addc('/').num(m.nkeep);
        {
            int k[8] = {}, op = 0;
            for (int i = 0; i < m.nkeep; ++i) if (!m.dead[i]) ++k[m.kind[i] < 8 ? m.kind[i] : 7];
            for (int i = 0; i < m.nkeep; ++i) op += m.opened[i] && !m.dead[i];
            c.add(" ITEM ").num(k[1]).add(" O2 ").num(k[2]).add(" WPN ").num(k[3]).add(" POT ").num(k[4]).add(" GOD ").num(k[5])
             .add(" PILE ").num(k[6]).add(" ?").num(k[0]).add(" OPEN ").num(op).add(" ROUND ").num(m.rounds).add(" REFS ").num(m.spawn_hits).add(" NAMED ").num(m.spawn_named);
        }
        c.add(" PLACED: OWN ").num(n[1] + n[2] + n[3]).add(" SPAWNER ").num(n[4]).add(" NONE ").num(n[0]).add(" (stops");
        for (int k = 0; k < 16; ++k) if (stc[k]) c.add(" s").num(k).addc('=').num(stc[k]);
        c.add(") HAVE-SPAWNER ").num(nsp).add(" GONE ").num(m.ndead);
        { int nm = 0, na = 0; for (int i = 0; i < m.nkeep; ++i) if (!m.dead[i]) { nm += m.mis[i]; na += m.noanim[i]; }
          c.add(" MISSION ").num(nm).add(" NO-ANIM ").num(na); }
        if (m.nkeep && !n[1] && !n[2] && !n[3] && m.ci) {
            const uint64_t nat = g.ptr(m.keep[0] + 0x10);
            c.add(" NAT ");
            for (int o = 0; o < 0x80; o += 8) { const uint64_t v = nat ? g.ptr(nat + o) : 0; c.addc(!nat ? '?' : MpPtrOk(g, v) ? 'p' : v ? '.' : '0'); }
        }
        memcpy(m.l3, c.s, c.n + 1);
    }
    // 0.36.4: the chest nearest Dave, in full (to tell weapon chests / fragments apart on photos)
    if (g_ticks % 30 == 11 && m.nkeep) {
        int best = -1; float bd = 1e30f;
        for (int i = 0; i < m.ci && i < m.nkeep; ++i) {
            if (!MpChestVisible(m, i)) continue;   // 0.37.28: only marks the map draws
            const float dx = m.mx[i] - g_dave_x, dy = m.my[i] - g_dave_y, d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = i; }
        }
        Buf e;
        m.nmat = 0;
        for (int i = 0; i < m.nkeep; ++i) if (!m.dead[i] && m.mat[i]) ++m.nmat;
        e.add("(material piles hidden: ").num(m.nmat).add(") ");
        if (best < 0) e.add("NEAREST: none read yet");
        else {
            const uint64_t obj = m.keep[best], sp = m.spw[best];
            uint8_t fl[4] = {}; g.read(obj + 0x60, fl, 4);
            const float dx = m.mx[best] - g_dave_x, dy = m.my[best] - g_dave_y;
            const int32_t tl = m.tid[best] % 100000;
            static const char* const kAs[] = {"chest", "chest", "O2", "weapon", "chest (pot)", "chest (5)", "chest (search)", "chest (7)"};
            e.add("NEAREST MARK ").add((tl >= 89000 && tl < 90000) ? "mission" : kAs[m.kind[best] & 7])
             .add(" #").num(best).add(" OFF "); AddF(e, dx); e.addc(','); AddF(e, dy);
            e.add(" TID ").num(m.tid[best]).add(" KIND ").num(m.kind[best]).add(" MIS ").num(m.mis[best])
             .add(" ANIM ").num(m.noanim[best] ? 0 : 1).add(" FLAGS ").num(fl[0]).num(fl[1]).num(fl[2]).num(fl[3]);
            if (sp) {
                char a1[40] = "", a2[40] = "", u[48] = "", sn[40] = "";
                g.cls(sp, sn, sizeof sn); g.str(g.ptr(sp + 0x60), a1, sizeof a1); g.str(g.ptr(sp + 0x80), a2, sizeof a2); g.str(g.ptr(sp + 0x20), u, sizeof u);
                e.add(" SP ").add(sn[0] ? sn + (starts(sn, "Spawner") ? 7 : 0) : "?").add(" ICON ").add(a1).addc('/').add(a2).add(" ID ").add(u);
            } else e.add(" (no spawner yet)");
        }
        {   // 0.38.11 (tester): the nearest chest / O2 / weapon box of ANY state, drawn or not, and why it is not drawn
            int bi = -1; float bd2 = 1e30f;
            for (int i = 0; i < m.nkeep && i < kMpKeep; ++i) {
                if (!m.mhow[i]) continue;
                const float dx = m.mx[i] - g_dave_x, dy = m.my[i] - g_dave_y, d = dx * dx + dy * dy;
                if (d < bd2) { bd2 = d; bi = i; }
            }
            e.add(" | NEAREST ANY BOX");
            if (bi < 0) e.add(" none placed");
            else {
                e.add(" #").num(bi).add(" OFF "); AddF(e, m.mx[bi] - g_dave_x); e.addc(','); AddF(e, m.my[bi] - g_dave_y);
                e.add(" TID ").num(m.tid[bi]).add(" kind ").num(m.kind[bi]).add(MpChestVisible(m, bi) ? " DRAWN" : " HIDDEN:");
                if (!MpChestVisible(m, bi)) {
                    if (m.opened[bi]) e.add(" opened");
                    if (m.dead[bi]) e.add(" gone");
                    if (!m.owned[bi]) e.add(" no-spawner");
                    if (m.mat[bi]) e.add(" material-pile");
                    if (!m.matok[bi]) e.add(" not-checked-yet");
                    if (m.kind[bi] < 1 || m.kind[bi] > 3) e.add(" kind-not-box");
                }
            }
            int nh = 0, nd = 0; for (int i = 0; i < m.nkeep && i < kMpKeep; ++i) if (m.mhow[i]) { ++nh; nd += MpChestVisible(m, i) ? 1 : 0; }
            e.add(" | boxes placed ").num(nh).add(" drawn ").num(nd);
            // the FILTER toggles (1 = shown): chests, O2, weapons, missions, ammo, pods
            e.add(" | FILTER chest ").num(g_mk_mask & 1).add(" O2 ").num((g_mk_mask >> 1) & 1).add(" wpn ").num((g_mk_mask >> 2) & 1)
             .add(" ammo ").num((g_mk_mask >> 4) & 1).add(" pod ").num((g_mk_mask >> 5) & 1).add(" all ").num(g_mk_all);
        }
        if (best >= 0) {   // compare with chests that have a spawner (in play)
            static uint64_t oth[kMpKeep]; int no = 0;
            for (int i = 0; i < m.nkeep; ++i) if (!m.dead[i] && m.spw[i] && !m.mat[i]) oth[no++] = m.keep[i];
            Odd(g, m.keep[best], oth, no, m.o1, e);
        }
        {   // the nearest ammo pickup's GameObject, for the "nearest pickup" line
            int ba = -1; float da = 1e9f;
            for (int i = 0; i < m.npk; ++i) if (MpAmmoVisible(m, i)) {
                const float dx = m.pxx[i] - g_dave_x, dy = m.pyy[i] - g_dave_y, d2 = dx * dx + dy * dy;
                if (d2 < da) { da = d2; ba = i; }
            }
            Buf a; a.add(" | AMMO");
            if (ba >= 0) {
                static uint64_t oth[kPk]; int no = 0;
                for (int i = 0; i < m.npk; ++i) if (MpAmmoVisible(m, i)) oth[no++] = m.pk[i];
                Odd(g, m.pk[ba], oth, no, m.o1, a, 14);   // 0.38.3: up to 14 (its own DIAG line now)
                {   // ... and the raw words at native component +0x28 / +0x2C and +0x50 (the bytes seen differing)
                    const uint64_t C = g.ptr(m.pk[ba] + 0x10); uint32_t w[3] = {};
                    if (C) { g.read(C + 0x28, &w[0], 4); g.read(C + 0x2C, &w[1], 4); g.read(C + 0x50, &w[2], 4); }
                    static const char hx[] = "0123456789abcdef";
                    a.add(" | C28 "); for (int q = 7; q >= 0; --q) a.addc(hx[(w[0] >> (q * 4)) & 15]);
                    a.add(" C2C "); for (int q = 7; q >= 0; --q) a.addc(hx[(w[1] >> (q * 4)) & 15]);
                    a.add(" C50 "); for (int q = 7; q >= 0; --q) a.addc(hx[(w[2] >> (q * 4)) & 15]);
                }
            } else a.add(" none");
            DcCopy(g_pkgo, sizeof g_pkgo, a.s);
        }
        memcpy(m.l11, e.s, e.n + 1);
        Buf f;
        // drop lists in use, with how many chests each (to tell boxes from small pickups)
        int32_t tv[16]; int tc[16]; int nt = 0;
        for (int i = 0; i < m.nkeep; ++i) {
            if (m.dead[i] || !m.tid[i]) continue;
            int at = -1; for (int k = 0; k < nt; ++k) if (tv[k] == m.tid[i]) at = k;
            if (at < 0 && nt < 16) { at = nt++; tv[at] = m.tid[i]; tc[at] = 0; }
            if (at >= 0) ++tc[at];
        }
        { int pa = 0, pp = 0, pm = 0;
          for (int i = 0; i < m.npk; ++i) if (!m.pdead[i]) { ++pa; pp += m.ph[i]; for (int q = 0; q < m.nmtgt; ++q) if (m.mtgt[q] == m.ptid[i]) { ++pm; break; } }
          f.add("PICKUPS ").num(pa).add(" PLACED ").num(pp).add(" FOR MISSIONS ").num(pm).add(" (classes ").num(1 + m.npsubk).add(") TARGETS");
          for (int q = 0; q < m.nmtgt; ++q) f.addc(' ').num(m.mtgt[q]);
          f.add(" | "); }
        f.add("DROP LISTS:");
        for (int k = 0; k < nt; ++k) f.addc(' ').num(tv[k] % 100000).addc('x').num(tc[k]);
        memcpy(m.l12, f.s, f.n + 1);
    }
}
void MarkerPublish(const EdenDsmodHostApi* h) {
    MarkerProbe& m = g_mp;
    for (int i = 0; i < kMpKeep; ++i) {
        Buf ko, kx, ky, kt;
        ko.add("mk.on").num(i); kx.add("mk.x").num(i); ky.add("mk.y").num(i); kt.add("mk.t").num(i);
        bool on = g_mk_ok && i < m.ci && MpChestVisible(m, i);
        if (on && !m.seen[i]) {           // about a screen around Dave
            const float ddx = m.mx[i] - g_dave_x, ddy = m.my[i] - g_dave_y;
            if (ddx > -14 && ddx < 14 && ddy > -9 && ddy < 9) m.seen[i] = 1;
        }
        on = on && g_mk_all;                // 0.36.3: the toggle shows / hides them all (fog of war dropped)
        const int64_t X = on ? MapDX(m.mx[i]) : 0, Y = on ? MapNY(m.my[i]) : 0;
        const int64_t hw = static_cast<int64_t>(kMapW * 0.5f);
        on = on && Y >= 0 && Y <= 1000 && X >= -hw && X <= hw;
        h->publish_i64(h->userdata, ko.s, on ? 1 : 0);
        if (!on) continue;
        h->publish_i64(h->userdata, kx.s, X);
        h->publish_i64(h->userdata, ky.s, Y);
        // 0.36.11: mission crates are the ones on drop lists 30890xx ("!" crates: 3089001..3089003 in 0.36.4/0.36.10);
        // IsMissionItem is not it (45 of 109 had it)
        const int32_t tl = m.tid[i] % 100000;
        h->publish_i64(h->userdata, kt.s, (tl >= 89000 && tl < 90000) ? 4 : m.kind[i] == 2 ? 2 : m.kind[i] == 3 ? 3 : 1);
    }
}
// 0.36.12: mission pickups (an item a mission in progress asks for), up to kMm, under the mission icon
int g_mm_unowned;   // 0.37.81: EcoWatcher goal pickups skipped as not owned by a spawner (DIAG)
char g_mm_diag[160];   // 0.37.50: the mission marks drawn (item id @ map x,y), for the DIAG fish line
void MissionPublish(const Guest& g, const EdenDsmodHostApi* h) {
    MarkerProbe& m = g_mp;
    int k = 0;
    Buf md; md.add(" | marks");
    const int64_t hw = static_cast<int64_t>(kMapW * 0.5f);
    g_mm_unowned = 0;
    int32_t eid[kPlEco], ehv[kPlEco], end_[kPlEco]; const char* enm[kPlEco]; const char* eic[kPlEco];
    const int ne = EcoPinGoals(eid, ehv, end_, enm, eic, kPlEco);
    for (int i = 0; i < m.npk && k < kMm; ++i) {
        if (m.pdead[i] || !m.ph[i] || !m.ptid[i] || !g_mk_ok || !g_mk_all) continue;
        bool want = false;
        for (int q = 0; q < m.nmtgt; ++q) if (m.mtgt[q] == m.ptid[i]) want = true;
        if (!want) continue;
        {   // 0.37.78: a pinned EcoWatcher goal's pickup is drawn only when it is a live item (its own item data) and
            // not an idle pooled copy -- the ammo marks' rule; a preset-only object drew a starfish where none was.
            // Story mission items keep the old rule (0.37.50 confirmed).
            bool eco = false; for (int q = 0; q < ne; ++q) if (eid[q] == m.ptid[i]) eco = true;
            if (eco && (!m.pactual[i] || m.pidle[i])) continue;
            // 0.37.81: and only one its spawner still owns (powned) -- the identity rule the chests use; an unowned
            // starfish showed at a Kajime plant (0.37.80 DIAG: i1 p0 o0 for the false one, o1 for a real one)
            if (eco && !m.powned[i]) { ++g_mm_unowned; continue; }
        }
        const int64_t X = MapDX(m.pxx[i]), Y = MapNY(m.pyy[i]);
        if (!(Y >= 0 && Y <= 1000 && X >= -hw && X <= hw)) continue;
        Buf ko, kx, ky; ko.add("mm.on").num(k); kx.add("mm.x").num(k); ky.add("mm.y").num(k);
        h->publish_i64(h->userdata, ko.s, 1); h->publish_i64(h->userdata, kx.s, X); h->publish_i64(h->userdata, ky.s, Y);
        {   // 0.37.77: a pinned EcoWatcher goal's pickup shows its own picture (as the fish marks do); else the mission icon
            Buf ki, src; ki.add("mm.ic").num(k); src.add("module:icon/Mission_MainIcon_28");
            for (int q = 0; q < ne; ++q) if (eid[q] == m.ptid[i] && eic[q] && eic[q][0]) { src.clear(); src.add("module:icon/").add(eic[q]); }
            h->publish_text(h->userdata, ki.s, src.s);
        }
        if (k < 4) {   // 0.37.80: + the pickup's own flags: m_IsInteractable @0x6C, usePhysics @0x35, spawner-owned
            uint8_t it = 0, ph = 0; g.read(m.pk[i] + 0x6C, &it, 1); g.read(m.pk[i] + 0x35, &ph, 1);
            md.addc(' ').num(m.ptid[i]).addc('@').num(X).addc(',').num(Y).add(" i").num(it).add("p").num(ph).add("o").num(m.powned[i]);
        }
        ++k;
    }
    if (!k) md.add(" none");
    DcCopy(g_mm_diag, sizeof g_mm_diag, md.s);
    for (; k < kMm; ++k) { Buf ko; ko.add("mm.on").num(k); h->publish_i64(h->userdata, ko.s, 0); }
    {   // 0.37.76: for the DIAG "marks" line -- what the map wants, and which pickups are out (item id x count)
        Buf e; e.add("want");
        for (int q = 0; q < m.nmtgt; ++q) e.addc(' ').num(m.mtgt[q]);
        e.add(" | ok ").num(g_mk_ok ? 1 : 0).num(g_mk_all ? 1 : 0).add(" | pickups ").num(m.npk).addc(':');
        int32_t ids[6]; int cnt[6]; int nd = 0;
        for (int i = 0; i < m.npk; ++i) {
            if (m.pdead[i] || !m.ptid[i]) continue;
            int at = -1; for (int q = 0; q < nd; ++q) if (ids[q] == m.ptid[i]) at = q;
            if (at < 0 && nd < 6) { at = nd++; ids[at] = m.ptid[i]; cnt[at] = 0; }
            if (at >= 0) ++cnt[at];
        }
        for (int q = 0; q < nd; ++q) e.addc(' ').num(ids[q]).addc('x').num(cnt[q]);
        e.add(" | wanted out:");                                     // 0.37.77: each wanted id among all pickups
        for (int q = 0; q < m.nmtgt; ++q) {
            int c = 0, lv = 0;
            for (int i = 0; i < m.npk; ++i) if (!m.pdead[i] && m.ptid[i] == m.mtgt[q]) { ++c; if (m.pactual[i] && !m.pidle[i]) ++lv; }
            e.addc(' ').num(m.mtgt[q]).addc('x').num(c).add(" live ").num(lv);
        }
        e.add(" | unowned ").num(g_mm_unowned).add(g_mm_diag);
        h->publish_text(h->userdata, "eco.mkdiag", e.s);
        // 0.37.80: the pickup classes the census matches, and how many live pickups of each
        Buf c; c.add("seed ").num(g_po_seeded).add(" cmds ").num(m.npcmdk).add(" |");
        for (int q = -1; q < m.npsubk; ++q) {
            const uint64_t K = q < 0 ? m.pick_k : m.psubk[q];
            char cn[40] = "?"; const uint64_t nm = K ? g.ptr(K + 0x10) : 0; if (nm) g.cstr(nm, cn, sizeof cn);
            int n = 0, nw = 0;
            for (int i = 0; i < m.npk; ++i) if (!m.pdead[i] && g.ptr(m.pk[i]) == K) { ++n; for (int t = 0; t < m.nmtgt; ++t) if (m.mtgt[t] == m.ptid[i]) ++nw; }
            c.addc(' ').add(cn).addc(' ').num(n);
            if (nw) c.add(" want ").num(nw);
            if (q >= 0 && K) {   // a sample item id of that class
                for (int i = 0; i < m.npk; ++i) if (!m.pdead[i] && g.ptr(m.pk[i]) == K && m.ptid[i]) { c.add(" e.g.").num(m.ptid[i]); break; }
            }
        }
        h->publish_text(h->userdata, "eco.pkdiag", c.s);
    }
}
// 0.37.22: pickups by kind (2 = ammo: the item's picture is Ammo_* / AmmoPack_*), a few per tick
char g_pkicons[6][32]; int g_npkicons, g_pkammo; char g_pkic[kPk][32]; int32_t g_ammo_tid[4]; char g_ammo_ic[4][40]; int g_nammo_seen;
void PickupKindStep(Instance& in, const Guest& g) {
    MarkerProbe& m = g_mp;
    const uint64_t table = ItemTable(in, g);
    if (!table) return;
    int work = 0; g_pkammo = 0;
    for (int i = 0; i < m.npk; ++i) if (!m.pdead[i] && m.ptid[i] && m.pactual[i] && m.pkind[i] == 2) ++g_pkammo;
    static int pkcur;   // 0.38.1: a rotating start, so six that cannot be read yet do not hold up the rest
    for (int n = 0; n < m.npk && work < 6; ++n) {
        const int i = (pkcur + n) % m.npk;
        if (m.pdead[i] || !m.ptid[i] || !m.pactual[i] || m.pkind[i]) continue;
        ++work; pkcur = (i + 1) % m.npk;
        const uint64_t it = FindItem(in, g, table, m.ptid[i]);
        char ic[48] = "";
        if (!it || g.s32(it + 0x10) != m.ptid[i] || !g.str(g.ptr(it + 0x38), ic, sizeof ic) || !ic[0]) continue;
        m.pkind[i] = (eq(ic, "Ammo_Thumbnail") || starts(ic, "Ammo_") || starts(ic, "AmmoPack_")) ? 2 : 1;
        DcCopy(g_pkic[i], sizeof g_pkic[i], ic[0] ? ic : "?");
        if (m.pkind[i] == 2 && g_nammo_seen < 4) {
            bool dup = false; for (int q = 0; q < g_nammo_seen; ++q) dup = dup || g_ammo_tid[q] == m.ptid[i];
            if (!dup) { g_ammo_tid[g_nammo_seen] = m.ptid[i]; DcCopy(g_ammo_ic[g_nammo_seen], 40, ic); ++g_nammo_seen; }
        }
        bool have = false; for (int q = 0; q < g_npkicons; ++q) have = have || eq(g_pkicons[q], ic);
        if (!have && g_npkicons < 6 && ic[0]) DcCopy(g_pkicons[g_npkicons++], sizeof g_pkicons[0], ic);
    }
}
constexpr int kAm = 24, kPd = 24;   // gen_dive.py AMMO_MARKS / POD_MARKS
void PodAmmoPublish(const EdenDsmodHostApi* h) {
    MarkerProbe& m = g_mp; void* u = h->userdata;
    const int64_t hw = static_cast<int64_t>(kMapW * 0.5f);
    int k = 0;
    for (int i = 0; i < m.npk && k < kAm; ++i) {
        if (!MpAmmoVisible(m, i) || !g_mk_ok) continue;
        const int64_t X = MapDX(m.pxx[i]), Y = MapNY(m.pyy[i]);
        if (!(Y >= 0 && Y <= 1000 && X >= -hw && X <= hw)) continue;
        Buf ko, kx, ky; ko.add("am.on").num(k); kx.add("am.x").num(k); ky.add("am.y").num(k);
        h->publish_i64(u, ko.s, 1); h->publish_i64(u, kx.s, X); h->publish_i64(u, ky.s, Y); ++k;
    }
    for (; k < kAm; ++k) { Buf ko; ko.add("am.on").num(k); h->publish_i64(u, ko.s, 0); }
    k = 0;
    for (int i = 0; i < m.npod && k < kPd; ++i) {
        if (!m.podh[i] || !g_mk_ok) continue;
        const int64_t X = MapDX(m.podx[i]), Y = MapNY(m.pody[i]);
        if (!(Y >= 0 && Y <= 1000 && X >= -hw && X <= hw)) continue;
        Buf ko, kx, ky, ku; ko.add("pd.on").num(k); kx.add("pd.x").num(k); ky.add("pd.y").num(k); ku.add("pd.used").num(k);
        h->publish_i64(u, ko.s, 1); h->publish_i64(u, kx.s, X); h->publish_i64(u, ky.s, Y); h->publish_i64(u, ku.s, m.podused[i]); ++k;
    }
    for (; k < kPd; ++k) { Buf ko; ko.add("pd.on").num(k); h->publish_i64(u, ko.s, 0); }
    Buf d; d.add("PODS class ").add(g_podk[0] ? "ok" : g_kf.nm[0] ? "name" : "?").addc('/').add(g_podk[1] ? "ok" : g_kf.nm[1] ? "name" : "?")
        .add(" search ").num(g_kf.stage).addc(' ').num(g_kf.pages).add(" found ").num(m.npod).add(" | AMMO ").num(g_pkammo).add(" | pickup pictures:");
    d.add(" | ammo items:");
    for (int q = 0; q < g_nammo_seen; ++q) d.addc(' ').num(g_ammo_tid[q]).addc('=').add(g_ammo_ic[q]);
    {   // the nearest ammo mark to Dave (swim to it and look)
        float best = 1e9f; int bi = -1;
        for (int i = 0; i < m.npk; ++i) if (MpAmmoVisible(m, i)) {
            const float dx = m.pxx[i] - g_dave_x, dy = m.pyy[i] - g_dave_y, d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; bi = i; }
        }
        if (bi >= 0) d.add(" | nearest ").num(m.ptid[bi]).add(" dx ").num(static_cast<int>(m.pxx[bi] - g_dave_x)).add(" dy ").num(static_cast<int>(m.pyy[bi] - g_dave_y));
    }
    {   // 0.38.11 (tester): the nearest escape pod, drawn or not
        int bp = -1; float bd2 = 1e30f;
        for (int i = 0; i < m.npod; ++i) {
            if (!m.podh[i]) continue;
            const float dx = m.podx[i] - g_dave_x, dy = m.pody[i] - g_dave_y, dd = dx * dx + dy * dy;
            if (dd < bd2) { bd2 = dd; bp = i; }
        }
        d.add(" | NEAREST POD");
        if (bp < 0) d.add(" none placed (").num(m.npod).add(" found)");
        else { d.add(" OFF "); AddF(d, m.podx[bp] - g_dave_x); d.addc(','); AddF(d, m.pody[bp] - g_dave_y); d.add(m.podused[bp] ? " used" : " ready");
               d.add(g_mk_ok ? "" : " (map marks off)"); }
    }
    h->publish_text(u, "mk.p13", d.s);
    {   // 0.37.28: the nearest ammo mark and the nearest pickup of any kind, with the game's picture name
        Buf n; int ba = -1, bp = -1; float da = 1e9f, dp = 1e9f;
        for (int i = 0; i < m.npk; ++i) {
            if (m.pdead[i] || !m.ph[i]) continue;
            const float dx = m.pxx[i] - g_dave_x, dy = m.pyy[i] - g_dave_y, d2 = dx * dx + dy * dy;
            if (MpAmmoVisible(m, i) && d2 < da) { da = d2; ba = i; }
            if (d2 < dp) { dp = d2; bp = i; }
        }
        auto one = [&](const char* t, int i) {
            n.add(t);
            if (i < 0) { n.add(" none"); return; }
            n.addc(' ').num(m.ptid[i]).addc(' ').add(m.pkind[i] ? g_pkic[i] : "(not read)")
             .add(" live ").num(m.pactual[i]).add(" owner ").num(m.powned[i]).add(" idle ").num(m.pidle[i]).add(" G30 ").num(g_pkg30[i]).add(" OFF ");
            AddF(n, m.pxx[i] - g_dave_x); n.addc(','); AddF(n, m.pyy[i] - g_dave_y);
        };
        int hid = 0, off = 0, n06 = 0, nother = 0;
        for (int i = 0; i < m.npk; ++i) if (!m.pdead[i] && m.pkind[i] == 2 && m.pactual[i]) {
            hid += m.pidle[i];
            if (g_pkg30[i] == kPkGoOff) ++off; else if (g_pkg30[i] == 0x06) ++n06; else ++nother;
        }
        // 0.38.4: ammo switched off by the game (G+30 = 05) is hidden; how many of each, to check it holds
        n.add("(idle ammo hidden: ").num(hid).add(", switched-off ammo hidden: ").num(off).add(" | G30 06 x").num(n06)
         .add(" other x").num(nother).add(") ");
        one("AMMO MARK", ba);
        if (ba >= 0) {   // 0.38.1: its native component +0x40..+0x4f (the idle rule: 00 00 80 3f at +0x4c)
            const uint8_t* b = g_pkraw[ba];
            static const char hx[] = "0123456789abcdef";
            n.add(" int ").num(g_pkflag[ba][0]).add(" phys ").num(g_pkflag[ba][1]).add(" N40:");
            for (int q = 0; q < 16; ++q) { if (q % 4 == 0) n.addc(' '); n.addc(hx[b[q] >> 4]); n.addc(hx[b[q] & 15]); }
        }
        n.add(" | "); one("ANY PICKUP", bp);
        h->publish_text(u, "mk.p14", n.s);
        h->publish_text(u, "mk.podd", g_pkgo);   // 0.38.3: the comparison on its own DIAG line (it was cut off)
    }
}
// 0.37.5: a mission row that has just been completed (its count reached its max) -> a toast on the map
// page for a few seconds (toast.on, toast.k = the row: mission * 6 + row). Rows seen complete when first
// read, or whose target changed (another mission in the slot), do not toast.
constexpr uint64_t kToastTicks = 210;            // 3.5 s
int32_t g_ms_tgt[18]; uint8_t g_ms_done[18], g_ms_seen[18]; uint64_t g_toast_until; int g_toast_k = -1;
int g_toast_full, g_toast_m = -1;   // 0.38.40: the row that finished the whole mission (every row done) -> MISSION COMPLETE
void MissionToast(const EdenDsmodHostApi* h, bool check) {
    void* u = h->userdata;
    if (check && h->get_i64) {
    for (int a = 0; a < 3; ++a) {
        Buf cn; cn.add("cn").num(a);
        const int64_t n = h->get_i64(u, cn.s, 0);
        int fresh = -1, all = n > 0;
        for (int b = 0; b < 6; ++b) {
            const int k = a * 6 + b;
            if (b >= n) { g_ms_seen[k] = 0; continue; }
            Buf t, nw, mx; t.add("ctgt").num(a).addc('_').num(b); nw.add("cnow").num(a).addc('_').num(b); mx.add("cmax").num(a).addc('_').num(b);
            const int32_t tgt = static_cast<int32_t>(h->get_i64(u, t.s, 0));
            const int64_t now = h->get_i64(u, nw.s, 0), mxv = h->get_i64(u, mx.s, 0);
            const uint8_t done = mxv > 0 && now >= mxv ? 1 : 0;
            if (!g_ms_seen[k] || g_ms_tgt[k] != tgt) { g_ms_seen[k] = 1; g_ms_tgt[k] = tgt; g_ms_done[k] = done; continue; }
            if (done && !g_ms_done[k]) { g_toast_k = k; g_toast_until = g_ticks + kToastTicks; fresh = k; g_toast_full = 0; }
            g_ms_done[k] = done;
            if (!done) all = 0;
        }
        if (fresh >= 0 && all) { g_toast_full = 1; g_toast_m = a; g_toast_until = g_ticks + kToastTicks + 60; }
    }
    }
    h->publish_i64(u, "toast.on", g_toast_until > g_ticks ? 1 : 0);
    h->publish_i64(u, "toast.k", g_toast_k);
    h->publish_i64(u, "toast.full", g_toast_full);
    h->publish_i64(u, "toast.m", g_toast_m);
}
// 0.37.7 (round 33): the fish, read straight from the game's lists -- InGameManager.FishAllocators @0x180
// (List<FishAllocator>), FishAllocator.instancedFishs @0x98 (List<FishAISystem>), FishAISystem.FishDataTID
// @0x110. A fish is kept when its TID is an ingredient of the pinned recipe (its own picture on the map) or
// a target of a mission in progress (the mission mark). Positions through the same transform path as Dave.
constexpr int kFm = 32;   // 0.37.11: live fish first, then spawn areas (area = 1)
struct FishMarks {
    uint64_t obj[kFm]; int32_t tid[kFm]; int8_t pl[kFm]; uint8_t mis[kFm], has[kFm], area[kFm]; float x[kFm], y[kFm];
    int nareas, aknown, apos, nogo, unk, pinspots, pinmaybe;
    int n, rr, allocs, total; int32_t seen[12], sdrop[12]; int nseen;
    int32_t plitem[8]; int nmatch; int32_t mtid; int8_t mhow; int32_t types[64]; int ntypes;
} g_fm;
// 0.37.9: DropItemID (2110xxx) is a FishDropPackage id, not the catch. What a fish gives, per fish TID
// (cached once): DataManager.FishDropPackageDataDic @0x180 [drop] -> PlusItemID @0x28,
// WholedummyItemID @0x2C and each FishDropList (@0x18) node's ints @0x10..0x1C (item id and weights).
struct FishGives { int32_t tid, drop; int n; int32_t id[14]; uint64_t at; int tries; };
constexpr int kFg = 512;
FishGives g_fg[kFg]; int g_nfg;
constexpr uint64_t kDmFishDrop = 0x180;
const FishGives* FishGivesOf(Instance& in, const Guest& g, int32_t tid, int32_t drop) {
    FishGives* ep = nullptr; bool fresh = false;
    for (int i = 0; i < g_nfg && !ep; ++i) if (g_fg[i].tid == tid) ep = &g_fg[i];
    if (ep) {   // 0.38.1: an empty answer (the drop entry not there yet) is asked again, 10 s apart, 3 times at most
        if (ep->n || drop <= 0 || ep->tries >= 3 || (drop == ep->drop && g_ticks - ep->at < 600)) return ep;
    } else {
        if (g_nfg >= kFg) return nullptr;
        ep = &g_fg[g_nfg++]; fresh = true;
    }
    FishGives& e = *ep; const int tries = fresh ? 0 : e.tries + 1;
    e = {}; e.tid = tid; e.drop = drop; e.at = g_ticks; e.tries = tries;
    const uint64_t d = drop > 0 ? DmDict(in, g, kDmFishDrop, "FishDropPackageEntity") : 0;
    if (drop > 0 && !d) { if (fresh) { --g_nfg; return nullptr; } return &e; }    // the table is not loaded yet: ask again later
    const uint64_t v = d ? DictFindInt(g, d, drop) : 0;
    auto add = [&](int32_t x) { if (x > 1000 && e.n < 14) { for (int q = 0; q < e.n; ++q) if (e.id[q] == x) return; e.id[e.n++] = x; } };
    if (v) {
        add(g.s32(v + 0x28)); add(g.s32(v + 0x2C));
        const uint64_t l = g.ptr(v + 0x18), la = l ? g.ptr(l + 0x10) : 0;
        int32_t ln = l ? g.s32(l + 0x18) : 0; if (ln < 0 || ln > 16) ln = 0;
        for (int k = 0; k < ln && la; ++k) {
            const uint64_t nd = g.ptr(la + 0x20 + static_cast<uint64_t>(k) * 8); if (!nd) continue;
            for (uint64_t o = 0x10; o <= 0x1C; o += 4) add(g.s32(nd + o));
        }
    }
    return &e;
}
// 0.37.11 (round 34): fish the game has not created yet. Every FishAllocator (InGameManager.FishAllocators)
// is a spawn spot; it knows its fish only through its prefab (FishPrefabOrGroup @0x58, a GameObject, or the
// first of FishPrefabOrGroups @0x48). Two ways to learn a spot's fish TID:
//  1) seen: a fish it has created (instancedFishs) -- remembered by the spot's position for the session;
//  2) its prefab: the path from the prefab GameObject to its FishAISystem (TID @0x110) is found once by
//     searching from a spot whose fish we have seen (native +o -> component array (stride) -> +c).
// Spots for a pinned / mission fish with none of it swimming there are marked on the map, dimmed.
constexpr int kAc = 512, kSt = 4;    // spots; fish kinds remembered per spot
struct AllocCache { uint64_t ptr[kAc], go0[kAc]; char gname[kAc][40]; float x[kAc], y[kAc]; uint8_t pos[kAc], tried[kAc], nt[kAc], ngo[kAc], guess[kAc], checked[kAc], live[kAc]; int32_t tid[kAc][kSt]; int rr; } g_ac;
struct FishMem { float x, y; int32_t tid; };
inline bool Near05(float d) { return d < 0.5f && d > -0.5f; }
FishMem g_fmem[kAc]; int g_nfmem;
// state 0 unknown, 1 found, 2 gave up. g_pp: GameObject native +o -> components (stride, +8 when 0x10)
// -> +c -> FishAISystem. g_gp (0.37.12, prefabs that are a group of fish): the first component (the
// Transform) +o -> its children (8 apart) -> +c -> the child's GameObject native, then g_pp on it.
struct PrefabPath { int state; uint32_t o, stride, c; int tries; } g_pp;
void FishMemPut(float x, float y, int32_t t) {
    for (int i = 0; i < g_nfmem; ++i) if (Near05(g_fmem[i].x - x) && Near05(g_fmem[i].y - y)) { g_fmem[i].tid = t; return; }
    if (g_nfmem < kAc) g_fmem[g_nfmem++] = {x, y, t};
}
int32_t FishMemGet(float x, float y) {
    for (int i = 0; i < g_nfmem; ++i) if (Near05(g_fmem[i].x - x) && Near05(g_fmem[i].y - y)) return g_fmem[i].tid;
    return 0;
}
bool IsFishTid(int32_t t) { return t >= 2000000 && t < 3000000; }
// 0.37.13: the game's own fish ids (the keys of DataManager.FishInfoDataDic), so a number that only looks
// like a fish id is never taken for one
int32_t g_fset[2048]; int g_nfset;
bool FishSetReady(Instance& in, const Guest& g) {
    if (g_nfset) return true;
    const uint64_t d = DmDict(in, g, kDmFishInfo, "FishInfoDataEntity");
    const uint64_t en = d ? g.ptr(d + 0x18) : 0; const int32_t cnt = d ? g.s32(d + 0x20) : 0;
    if (!en || cnt <= 0 || cnt > 20000) return false;
    int n = 0;
    for (int32_t i = 0; i < cnt && n < 2048; ++i) {
        const uint64_t e = en + 0x20 + static_cast<uint64_t>(i) * 0x18;
        const int32_t k = g.s32(e + 8);
        if (g.s32(e) >= 0 && IsFishTid(k)) {
            int j = n++; while (j > 0 && g_fset[j - 1] > k) { g_fset[j] = g_fset[j - 1]; --j; } g_fset[j] = k;
        }
    }
    g_nfset = n;
    return n > 0;
}
bool IsKnownFish(int32_t t) {
    if (!IsFishTid(t)) return false;
    if (!g_nfset) return true;
    int lo = 0, hi = g_nfset - 1;
    while (lo <= hi) { const int m = (lo + hi) / 2; if (g_fset[m] == t) return true; if (g_fset[m] < t) lo = m + 1; else hi = m - 1; }
    return false;
}
void SpotAdd(int a, int32_t t) {
    if (!IsKnownFish(t)) return;
    for (int q = 0; q < g_ac.nt[a]; ++q) if (g_ac.tid[a][q] == t) return;
    if (g_ac.nt[a] < kSt) g_ac.tid[a][g_ac.nt[a]++] = t;
}
// every prefab a spot may use: FishPrefabOrGroup @0x58, and whatever FishPrefabOrGroups @0x48 holds
// (lists, arrays and their entries, a few levels down) -- 0.37.13: no longer only the first List`1
void GoCollect(const Guest& g, uint64_t obj, int depth, uint64_t* out, int& n, int cap) {
    if (!obj || n >= cap || depth > 3) return;
    char c[64];
    if (!g.cls(obj, c, sizeof c)) return;
    if (eq(c, "GameObject")) { for (int k = 0; k < n; ++k) if (out[k] == obj) return; out[n++] = obj; return; }
    if (starts(c, "List`1")) {
        const uint64_t it = g.ptr(obj + 0x10); int32_t ln = g.s32(obj + 0x18); if (ln < 0 || ln > 16) ln = 0;
        for (int e = 0; it && e < ln && n < cap; ++e) GoCollect(g, g.ptr(it + 0x20 + static_cast<uint64_t>(e) * 8), depth + 1, out, n, cap);
        return;
    }
    const int L = len(c);
    if (L > 2 && c[L - 2] == '[' && c[L - 1] == ']') {
        int32_t ln = g.s32(obj + 0x18); if (ln < 0 || ln > 16) ln = 0;
        for (int e = 0; e < ln && n < cap; ++e) GoCollect(g, g.ptr(obj + 0x20 + static_cast<uint64_t>(e) * 8), depth + 1, out, n, cap);
        return;
    }
    if (eq(c, "String") || starts(c, "Dictionary")) return;
    for (uint64_t o = 0x10; o <= 0x40 && n < cap; o += 8) {
        const uint64_t v = g.ptr(obj + o);
        if (v && MpPtrOk(g, v)) GoCollect(g, v, depth + 1, out, n, cap);
    }
}
int AllocPrefabs(const Guest& g, uint64_t A, uint64_t* out, int cap) {
    int n = 0;
    GoCollect(g, g.ptr(A + 0x58), 2, out, n, cap);
    // 0.37.19: the Switch build uses FishPrefabOrGroupsForSwitch @0x50 when replacedBySwitchPlatform @0x41
    uint8_t sw = 0; g.read(A + 0x41, &sw, 1);
    GoCollect(g, g.ptr(A + (sw ? 0x50 : 0x48)), 0, out, n, cap);
    return n;
}
uint64_t CompAt(const Guest& g, uint64_t P, int e) {
    return g.ptr(P + static_cast<uint64_t>(e) * g_pp.stride + (g_pp.stride == 0x10 ? 8 : 0));
}
// the fish TID on a GameObject (native), through g_pp
int32_t NativeTid(const Guest& g, uint64_t N) {
    const uint64_t P = N ? g.ptr(N + g_pp.o) : 0;
    for (int e = 0; P && e < 16; ++e) {
        const uint64_t C = CompAt(g, P, e), Q = C ? g.ptr(C + g_pp.c) : 0;
        if (!Q) continue;
        const int32_t t = g.s32(Q + 0x110);
        if (IsKnownFish(t)) return t;
        const uint64_t fi = g.ptr(Q + 0x128);                 // 0.37.14: _FishInfoData.TID, when the prefab has it
        const int32_t t2 = fi && MpPtrOk(g, fi) ? g.s32(fi + 0x10) : 0;
        if (IsKnownFish(t2)) return t2;
    }
    return 0;
}
struct ChildPath { int state; uint32_t o, f, c; int tries; } g_cp;
// 0.37.15: prefabs the game has never created have no managed objects (DIAG "fish 2": every component
// "-"), so their FishDataTID is not in memory. Their names are: the native GameObject keeps its name as
// a C string (offset found once, from a prefab whose fish we know). Matched against FishInfoData.FishName
// (@0x18), letters and digits only, lower case, the longest name found inside the prefab's name wins.
struct FishName { int32_t tid; char n[40]; };
int g_nv_t, g_nv_a, g_nm_exact, g_nm_guess; char g_nmissv[2][96]; int g_nmiss;   // 0.37.16: name guesses checked against spots known for sure
FishName g_fnm[1024]; int g_nfnm;
int g_gonm_off = -1, g_gonm_tries;
void Norm(const char* in, char* out, int cap) {
    int k = 0;
    for (int i = 0; in[i] && k < cap - 1; ++i) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[k++] = c;
    }
    out[k] = 0;
}
bool Contains(const char* hay, const char* nd) {
    const int hn = len(hay), nn = len(nd);
    for (int i = 0; i + nn <= hn; ++i) { int j = 0; while (j < nn && hay[i + j] == nd[j]) ++j; if (j == nn) return true; }
    return false;
}
void FishNamesLoad(Instance& in, const Guest& g) {
    if (g_nfnm) return;
    const uint64_t d = DmDict(in, g, kDmFishInfo, "FishInfoDataEntity");
    const uint64_t en = d ? g.ptr(d + 0x18) : 0; const int32_t cnt = d ? g.s32(d + 0x20) : 0;
    if (!en || cnt <= 0 || cnt > 20000) return;
    for (int32_t i = 0; i < cnt && g_nfnm < 1024; ++i) {
        const uint64_t e = en + 0x20 + static_cast<uint64_t>(i) * 0x18, v = g.ptr(e + 0x10);
        if (g.s32(e) < 0 || !v) continue;
        const int32_t t = g.s32(v + 0x10);
        char raw[64];
        if (!IsFishTid(t) || !g.str(g.ptr(v + 0x18), raw, sizeof raw)) continue;
        FishName& f = g_fnm[g_nfnm]; f.tid = t; Norm(raw, f.n, sizeof f.n);
        if (len(f.n) >= 4) ++g_nfnm;
    }
}
const char* FishNameOf(int32_t t) { for (int i = 0; i < g_nfnm; ++i) if (g_fnm[i].tid == t) return g_fnm[i].n; return nullptr; }
bool GoName(const Guest& g, uint64_t N, char* out, int cap) {
    out[0] = 0;
    if (g_gonm_off < 0 || !N) return false;
    char raw[96];
    if (!g.cstr(g.ptr(N + static_cast<uint64_t>(g_gonm_off)), raw, sizeof raw)) return false;
    Norm(raw, out, cap);
    return out[0] != 0;
}
// learn where the native GameObject keeps its name: the C string holding the known fish's name
void GoNameProbe(const Guest& g, uint64_t N, int32_t T) {
    if (g_gonm_off >= 0 || !N || g_gonm_tries > 40) return;
    const char* fn = FishNameOf(T); if (!fn) return;
    ++g_gonm_tries;
    for (int o = 0; o <= 0x100; o += 8) {
        const uint64_t sp = g.ptr(N + static_cast<uint64_t>(o));
        if (!MpPtrOk(g, sp)) continue;
        char raw[96], nm[96];
        if (!g.cstr(sp, raw, sizeof raw)) continue;
        bool printable = raw[0] != 0; for (int i = 0; raw[i]; ++i) printable = printable && raw[i] >= 0x20 && raw[i] < 0x7F;
        if (!printable) continue;
        Norm(raw, nm, sizeof nm);
        if (Contains(nm, fn)) { g_gonm_off = o; return; }
    }
}
// 0.37.18: prefab names carry the fish id itself ("sa2011005titaniumtitantriggerfishfishmon",
// "boidsa2010011seahorse2"): every 7-digit run starting with 2 that is a real fish id
int DigitTids(const Guest& g, uint64_t N, int depth, int32_t* out, int n, int cap) {
    char nm[96];
    if (N && GoName(g, N, nm, sizeof nm)) {
        for (int i = 0; nm[i]; ) {
            if (nm[i] < '0' || nm[i] > '9') { ++i; continue; }
            int j = i; int32_t v = 0; while (nm[j] >= '0' && nm[j] <= '9' && j - i < 9) { v = v * 10 + (nm[j] - '0'); ++j; }
            if (j - i == 7 && IsFishTid(v) && IsKnownFish(v) && n < cap) { bool d = false; for (int k = 0; k < n; ++k) d = d || out[k] == v; if (!d) out[n++] = v; }
            i = j;
        }
    }
    if (depth >= 2 || g_cp.state != 1 || !N) return n;
    const uint64_t P = g.ptr(N + g_pp.o), T0 = P ? CompAt(g, P, 0) : 0, K = T0 ? g.ptr(T0 + g_cp.o) : 0;
    for (int e = 0; K && e < 16 && n < cap; ++e) {
        const uint64_t CT = g.ptr(K + static_cast<uint64_t>(e) * 8);
        if (!CT || g.ptr(CT + g_cp.f) != T0) break;
        n = DigitTids(g, g.ptr(CT + g_cp.c), depth + 1, out, n, cap);
    }
    return n;
}
int g_nm_digit;
// the fish a prefab's name (or one of its children's names) names
int32_t TidByName(const Guest& g, uint64_t N, int depth = 0) {
    char nm[96];
    if (GoName(g, N, nm, sizeof nm)) {
        int best = -1, bl = 0;
        for (int i = 0; i < g_nfnm; ++i) { const int l = len(g_fnm[i].n); if (l > bl && Contains(nm, g_fnm[i].n)) { best = i; bl = l; } }
        if (best >= 0) return g_fnm[best].tid;
    }
    if (depth >= 2 || g_cp.state != 1) return 0;
    const uint64_t P = g.ptr(N + g_pp.o), T0 = P ? CompAt(g, P, 0) : 0, K = T0 ? g.ptr(T0 + g_cp.o) : 0;
    for (int e = 0; K && e < 16; ++e) {
        const uint64_t CT = g.ptr(K + static_cast<uint64_t>(e) * 8);
        if (!CT || g.ptr(CT + g_cp.f) != T0) break;
        const int32_t t = TidByName(g, g.ptr(CT + g_cp.c), depth + 1);
        if (t) return t;
    }
    return 0;
}
// 0.37.14: what an unreadable prefab looks like (DIAG "fish 2"): its components' classes and +0x110, its children
char g_fdiag2[256];
void DescribePrefab(const Guest& g, uint64_t A, uint64_t go, int ng) {
    Buf b; char c[40];
    b.add("UNREAD spot ow=").num(g.s32(A + 0x30)).add(" gos ").num(ng).add(" go:");
    if (go && g.cls(go, c, sizeof c)) b.add(c);
    const uint64_t N = go ? g.ptr(go + 0x10) : 0, P = N ? g.ptr(N + g_pp.o) : 0;
    for (int e = 0; P && e < 8; ++e) {
        const uint64_t C = CompAt(g, P, e); if (!MpPtrOk(g, C)) break;
        const uint64_t Q = g.ptr(C + g_pp.c);
        b.addc(' ');
        if (Q && g.cls(Q, c, sizeof c)) { c[14] = 0; b.add(c); b.addc('=').num(g.s32(Q + 0x110)); } else b.addc('-');
    }
    if (g_cp.state == 1 && P) {
        const uint64_t T0 = CompAt(g, P, 0), K = T0 ? g.ptr(T0 + g_cp.o) : 0;
        int nk = 0; for (int e = 0; K && e < 64; ++e) { const uint64_t CT = g.ptr(K + static_cast<uint64_t>(e) * 8); if (!CT || g.ptr(CT + g_cp.f) != T0) break; ++nk; }
        b.add(" kids ").num(nk);
        char nm[64];
        if (GoName(g, N, nm, sizeof nm)) b.add(" name ").add(nm);
        const uint64_t CT = K ? g.ptr(K) : 0;
        if (nk && GoName(g, g.ptr(CT + g_cp.c), nm, sizeof nm)) b.add(" kid ").add(nm);
    }
    { char nm[64]; if (g_cp.state != 1 && GoName(g, N, nm, sizeof nm)) b.add(" name ").add(nm); }
    b.add(" nameoff ").num(g_gonm_off).add(" names ").num(g_nfnm);
    memcpy(g_fdiag2, b.s, static_cast<size_t>(b.n) + 1);
}
// 0.37.13: the children of a GameObject, found by shape (no fish needed): its Transform (component 0)
// +o -> children (8 apart), each child Transform pointing back at its father (+f), and the child's
// GameObject (+c) whose own component 0 is that child Transform.
void ChildProbe(const Guest& g, uint64_t N) {
    if (g_pp.state != 1 || g_cp.state != 0 || !N) return;
    const uint64_t P = g.ptr(N + g_pp.o), T0 = P ? CompAt(g, P, 0) : 0;
    if (!MpPtrOk(g, T0)) return;
    bool parent = false;                              // 0.37.17: only a GameObject with children counts as a try
    for (uint32_t o = 0; o <= 0x100; o += 8) {
        const uint64_t K = g.ptr(T0 + o);
        if (!MpPtrOk(g, K)) continue;
        const uint64_t CT = g.ptr(K);
        if (!MpPtrOk(g, CT) || CT == T0) continue;
        for (uint32_t f = 0; f <= 0x100; f += 8) {
            if (g.ptr(CT + f) != T0) continue;
            parent = true;
            for (uint32_t c = 0; c <= 0x80; c += 8) {
                const uint64_t G2 = g.ptr(CT + c);
                if (!MpPtrOk(g, G2) || G2 == N) continue;
                const uint64_t P2 = g.ptr(G2 + g_pp.o);
                if (P2 && MpPtrOk(g, P2) && CompAt(g, P2, 0) == CT) { g_cp = {1, o, f, c, g_cp.tries}; return; }
            }
        }
    }
    if (parent && ++g_cp.tries > 60) g_cp.state = 2;
}
// a prefab's fish: its own, else its children's and grandchildren's (a school)
void TidsBelow(const Guest& g, uint64_t N, int depth, int32_t* out, int& n, int cap) {
    if (!N || n >= cap) return;
    const int32_t t = NativeTid(g, N);
    if (t) { for (int k = 0; k < n; ++k) if (out[k] == t) return; out[n++] = t; return; }
    if (depth >= 3) return;
    if (g_cp.state == 0) ChildProbe(g, N);
    if (g_cp.state != 1) return;
    const uint64_t P = g.ptr(N + g_pp.o), T0 = P ? CompAt(g, P, 0) : 0, K = T0 ? g.ptr(T0 + g_cp.o) : 0;
    for (int e = 0; K && e < 32 && n < cap; ++e) {
        const uint64_t CT = g.ptr(K + static_cast<uint64_t>(e) * 8);
        if (!CT || g.ptr(CT + g_cp.f) != T0) break;
        TidsBelow(g, g.ptr(CT + g_cp.c), depth + 1, out, n, cap);
    }
}
int PrefabTids(const Guest& g, uint64_t go, int32_t* out, int cap) {
    if (g_pp.state != 1 || !go) return 0;
    int n = 0;
    TidsBelow(g, g.ptr(go + 0x10), 0, out, n, cap);
    return n;
}
void PrefabProbe(const Guest& g, uint64_t go, int32_t T) {
    if (g_pp.state != 0 || !go) return;
    if (++g_pp.tries > 12) { g_pp.state = 2; return; }
    const uint64_t N = g.ptr(go + 0x10);
    if (!MpPtrOk(g, N)) return;
    for (uint32_t o = 0; o <= 0x100; o += 8) {
        const uint64_t P = g.ptr(N + o);
        if (!MpPtrOk(g, P)) continue;
        for (int si = 0; si < 2; ++si) {
            const uint32_t stride = si ? 0x8u : 0x10u;
            const uint64_t adj = stride == 0x10 ? 8 : 0;
            for (int e = 0; e < 16; ++e) {
                const uint64_t C = g.ptr(P + static_cast<uint64_t>(e) * stride + adj);
                if (!MpPtrOk(g, C)) continue;
                for (uint32_t c = 0; c <= 0x60; c += 8) {
                    const uint64_t Q = g.ptr(C + c);
                    if (Q && MpPtrOk(g, Q) && g.s32(Q + 0x110) == T) { g_pp = {1, o, stride, c, g_pp.tries}; return; }
                }
            }
        }
    }
}
// 0.37.24: the fish that gives an ingredient (its drop package lists it), for the ingredient's picture:
// FishInfoData.ItemIcon @0xA8 (else CardIcon @0xB0). Every fish's drop list read once (~285 fish).
bool FishIconFor(Instance& in, const Guest& g, int32_t id, char* out, int cap) {
    out[0] = 0;
    const uint64_t d = DmDict(in, g, kDmFishInfo, "FishInfoDataEntity");
    const uint64_t en = d ? g.ptr(d + 0x18) : 0; const int32_t cnt = d ? g.s32(d + 0x20) : 0;
    if (!en || cnt <= 0 || cnt > 20000) return false;
    static bool all; static int cur;
    while (!all) {                                 // fill the fish -> gives table once
        if (cur >= cnt) { all = true; break; }
        const uint64_t e = en + 0x20 + static_cast<uint64_t>(cur) * 0x18, v = g.ptr(e + 0x10); ++cur;
        if (g.s32(e) < 0 || !v) continue;
        const int32_t t = g.s32(v + 0x10);
        // 0.37.57: the drop table not loaded yet -> this fish is read again next time (it was skipped for good)
        if (IsFishTid(t) && !FishGivesOf(in, g, t, g.s32(v + 0x7C)) && g_nfg < kFg) { --cur; return false; }
    }
    for (int i = 0; i < g_nfg; ++i) {
        const FishGives& fg = g_fg[i]; bool gv = false;
        for (int q = 0; q < fg.n; ++q) gv = gv || fg.id[q] == id;
        if (!gv) continue;
        const uint64_t v = DictFindInt(g, d, fg.tid);
        if (v && ((g.str(g.ptr(v + 0xA8), out, cap) && out[0]) || (g.str(g.ptr(v + 0xB0), out, cap) && out[0]))) return true;
    }
    return false;
}
// 0.37.8: the game's fish id (FishDataTID, 2010xxx) is not the ingredient id (1011xxx). The fish's own
// FishInfoData (FishAISystem._FishInfoData @0x128) has DropItemID @0x7C (its TID @0x10 checks it); the
// pinned ingredient leads to the same item through IngredientsEntity.ItemsTID @0x54.
void FishStep(Instance& in, const Guest& g, uint64_t igm) {
    FishMarks& f = g_fm;
    if (!igm || !g_mp.fixed) { f.n = 0; return; }
    if (g_ticks % 30 == 21) {                   // the list, twice a second
        FishMarks nf = {};
        {
            const uint64_t it = IngredientTable(in, g);
            for (int j = 0; j < g_pl_n && j < 8; ++j) {
                const uint64_t v = it ? DictFindInt(g, it, g_pl_id[j]) : 0;
                nf.plitem[j] = v ? g.s32(v + 0x54) : 0;
            }
        }
        const uint64_t al = g.ptr(igm + 0x180), aa = al ? g.ptr(al + 0x10) : 0;
        bool ok = false; int32_t an = al ? g.s32(al + 0x18, &ok) : 0;
        if (!ok || an < 0 || an > 512) an = 0;
        nf.allocs = an;
        if (an > kAc) an = kAc;
        // which spots still lack a fish of a wanted kind (filled below; areas after the walk)
        FishSetReady(in, g);
        FishNamesLoad(in, g);
        {   // 0.37.17: what every fish gives, 16 fish per walk (for "fish 3" and the spots' matching)
            static int cur;
            const uint64_t d = DmDict(in, g, kDmFishInfo, "FishInfoDataEntity");
            const uint64_t en = d ? g.ptr(d + 0x18) : 0; const int32_t cnt = d ? g.s32(d + 0x20) : 0;
            for (int k = 0; en && k < 16 && cur < cnt && cur < 20000; ++k, ++cur) {
                const uint64_t e = en + 0x20 + static_cast<uint64_t>(cur) * 0x18, v = g.ptr(e + 0x10);
                if (g.s32(e) < 0 || !v) continue;
                const int32_t t = g.s32(v + 0x10);
                if (IsFishTid(t)) FishGivesOf(in, g, t, g.s32(v + 0x7C));
            }
        }
        static uint8_t live_want[kAc];
        int probes = 0, lookups = 0;                     // keep each walk light
        for (int a = 0; a < an; ++a) {
            const uint64_t A = aa ? g.ptr(aa + 0x20 + static_cast<uint64_t>(a) * 8) : 0;
            live_want[a] = 0;
            if (g_ac.ptr[a] != A) { g_ac.ptr[a] = A; g_ac.pos[a] = 0; g_ac.tried[a] = 0; g_ac.nt[a] = 0; g_ac.ngo[a] = 0; g_ac.go0[a] = 0; g_ac.gname[a][0] = 0; g_ac.guess[a] = 0; g_ac.checked[a] = 0; }
            if (!A) continue;
            const uint64_t fl = g.ptr(A + 0x98), fa = fl ? g.ptr(fl + 0x10) : 0;
            bool ok2 = false; int32_t fn = fl ? g.s32(fl + 0x18, &ok2) : 0;
            if (!ok2 || fn < 0 || fn > 256 || !fa) fn = 0;
            g_ac.live[a] = fn > 0 ? 1 : 0;
            if (fn > 0 && g_ac.guess[a]) { g_ac.nt[a] = 0; g_ac.guess[a] = 0; }   // a fish it made beats a name guess
            if (fn > 0) {                                   // learn the spot's fish from what it made
                for (int i = 0; i < fn && i < 8; ++i) {
                    const uint64_t Fi = g.ptr(fa + 0x20 + static_cast<uint64_t>(i) * 8);
                    const int32_t ti = Fi ? g.s32(Fi + 0x110) : 0;
                    if (!IsFishTid(ti)) continue;
                    SpotAdd(a, ti);
                    if (g_ac.pos[a]) FishMemPut(g_ac.x[a], g_ac.y[a], ti);
                    if (g_pp.state == 0 && probes < 1 && !g_ac.tried[a]) {
                        uint64_t gos[8]; const int ng = AllocPrefabs(g, A, gos, 8);
                        if (ng) { ++probes; g_ac.tried[a] = 1; PrefabProbe(g, gos[0], ti); }
                    }
                }
            }
            // read the prefabs (once per spot; again later while the child path is still unknown)
            if (g_ac.tried[a] < 2 && g_pp.state == 1 && lookups < 16) {
                ++lookups; g_ac.tried[a] = 2;
                uint64_t gos[8]; const int ng = AllocPrefabs(g, A, gos, 8);
                g_ac.ngo[a] = static_cast<uint8_t>(ng);
                for (int k = 0; k < ng; ++k) {
                    int32_t ts[kSt]; const int nt = PrefabTids(g, gos[k], ts, kSt);
                    for (int q = 0; q < nt; ++q) SpotAdd(a, ts[q]);
                    if (nt == 1 && g_gonm_off < 0) GoNameProbe(g, g.ptr(gos[k] + 0x10), ts[0]);
                }
                SpotAdd(a, g.s32(A + 0x30));                           // _overwriteFishDataTID
                g_ac.go0[a] = ng ? gos[0] : 0;
                if (!g_ac.nt[a] && g_gonm_off < 0) g_ac.tried[a] = 0;  // read again once the names are known
                if (g_gonm_off < 0 && g_ac.nt[a] == 1 && ng == 1) GoNameProbe(g, g.ptr(gos[0] + 0x10), g_ac.tid[a][0]);
                if (!g_ac.nt[a] && g_cp.state == 0) g_ac.tried[a] = 0;
                if (!g_ac.nt[a] && (!g_fdiag2[0] || g_ticks % 600 < 30)) DescribePrefab(g, A, ng ? gos[0] : 0, ng);
            }
            if (!g_ac.nt[a] && g_ac.pos[a]) SpotAdd(a, FishMemGet(g_ac.x[a], g_ac.y[a]));
            for (int i = 0; i < fn; ++i) {
                const uint64_t F = g.ptr(fa + 0x20 + static_cast<uint64_t>(i) * 8); if (!F) continue;
                bool ok3 = false; const int32_t t = g.s32(F + 0x110, &ok3); if (!ok3 || t <= 0) continue;
                ++nf.total;
                const uint64_t fi = g.ptr(F + 0x128);
                const int32_t drop = (fi && g.s32(fi + 0x10) == t) ? g.s32(fi + 0x7C) : 0;
                { bool h2 = false; for (int q = 0; q < nf.ntypes; ++q) h2 = h2 || nf.types[q] == t;
                  if (!h2 && nf.ntypes < 64) nf.types[nf.ntypes++] = t; }
                bool have = false; for (int q = 0; q < nf.nseen; ++q) have = have || nf.seen[q] == t;
                if (!have && nf.nseen < 12) { nf.sdrop[nf.nseen] = drop; nf.seen[nf.nseen++] = t; }
                const FishGives* fg = FishGivesOf(in, g, t, drop);
                int8_t how = 0;
                auto hit = [&](int32_t w) {
                    if (w <= 0) return false;
                    if (w == t || w == drop) { how = 1; return true; }
                    for (int q = 0; fg && q < fg->n; ++q) if (fg->id[q] == w) { how = 2; return true; }
                    return false;
                };
                int8_t pl = -1;
                for (int j = 0; j < g_pl_n && j < 8; ++j) if ((kEcoMapMarks || g_pl_pin[j] != -2) && (hit(g_pl_id[j]) || hit(nf.plitem[j]))) pl = static_cast<int8_t>(j);
                uint8_t mis = 0; for (int q = 0; q < g_mp.nmtgt; ++q) if (hit(g_mp.mtgt[q])) mis = 1;
                if ((pl >= 0 || mis) && !nf.nmatch++) { nf.mtid = t; nf.mhow = how; }
                if (pl >= 0 || mis) live_want[a] = 1;
                if ((pl < 0 && !mis) || nf.n >= kFm) continue;
                int at = -1; for (int q = 0; q < f.n; ++q) if (f.obj[q] == F) at = q;   // keep the last spot
                const int k = nf.n++;
                nf.obj[k] = F; nf.tid[k] = t; nf.pl[k] = pl; nf.mis[k] = mis;
                if (at >= 0) { nf.has[k] = f.has[at]; nf.x[k] = f.x[at]; nf.y[k] = f.y[at]; }
            }
        }
        // 0.37.16: spots the prefab could not tell: (1) another spot with the same prefab, or a prefab of the
        // same name, whose fish is known for sure; (2) only when the name guess has proven right on the known
        // spots (>= 90% of at least 10), the longest fish name inside the prefab's name
        {
            const bool guess_ok = g_nv_t >= 10 && g_nv_a * 10 >= g_nv_t * 9;
            int work = 0;
            for (int a = 0; a < an && work < 48; ++a) {
                if (!g_ac.ptr[a] || g_ac.tried[a] < 2) continue;
                if (!g_ac.gname[a][0] && g_gonm_off >= 0 && g_ac.go0[a]) { ++work; GoName(g, g.ptr(g_ac.go0[a] + 0x10), g_ac.gname[a], sizeof g_ac.gname[a]); }
                if ((!g_ac.nt[a] || g_ac.guess[a]) && g_gonm_off >= 0 && !(g_ac.checked[a] & 2)) {   // 0.37.18: the id in the name
                    ++work; g_ac.checked[a] |= 2;
                    uint64_t gos[8]; const int ng = AllocPrefabs(g, g_ac.ptr[a], gos, 8);
                    int32_t ts[kSt]; int nt = 0;
                    for (int k = 0; k < ng; ++k) nt = DigitTids(g, g.ptr(gos[k] + 0x10), 0, ts, nt, kSt);
                    if (nt) { g_ac.nt[a] = 0; g_ac.guess[a] = 0; for (int q = 0; q < nt; ++q) SpotAdd(a, ts[q]); ++g_nm_digit; }
                }
                if (g_ac.nt[a] && !g_ac.guess[a]) {
                    if (!(g_ac.checked[a] & 1) && g_ac.gname[a][0] && g_nfnm) {      // check the guesser on a known spot
                        ++work; g_ac.checked[a] |= 1;
                        const int32_t t = TidByName(g, g.ptr(g_ac.go0[a] + 0x10));
                        if (t) {
                            ++g_nv_t; bool okv = false;
                            for (int q = 0; q < g_ac.nt[a]; ++q) if (g_ac.tid[a][q] == t) { ++g_nv_a; okv = true; break; }
                            if (!okv && g_nmiss < 2) {                       // what a wrong guess looks like (DIAG fish 3)
                                Buf m; m.add(g_ac.gname[a]).add(" guess ").add(FishNameOf(t) ? FishNameOf(t) : "?").add(" is ").add(FishNameOf(g_ac.tid[a][0]) ? FishNameOf(g_ac.tid[a][0]) : "?");
                                DcCopy(g_nmissv[g_nmiss++], sizeof g_nmissv[0], m.s);
                            }
                        }
                    }
                    continue;
                }
                if (g_ac.nt[a] && g_ac.guess[a] && guess_ok) continue;
                ++work;
                int from = -1;
                for (int b = 0; b < an && from < 0; ++b)
                    if (b != a && g_ac.nt[b] && !g_ac.guess[b] &&
                        ((g_ac.go0[a] && g_ac.go0[b] == g_ac.go0[a]) || (g_ac.gname[a][0] && eq(g_ac.gname[b], g_ac.gname[a])))) from = b;
                if (from >= 0) {
                    g_ac.nt[a] = 0; g_ac.guess[a] = 0;
                    for (int q = 0; q < g_ac.nt[from]; ++q) SpotAdd(a, g_ac.tid[from][q]);
                    ++g_nm_exact;
                } else if (g_ac.guess[a] && !guess_ok) {
                    g_ac.nt[a] = 0; g_ac.guess[a] = 0;                 // the guesser lost its trust
                } else if (!g_ac.nt[a] && guess_ok && g_ac.go0[a]) {
                    const int32_t t = TidByName(g, g.ptr(g_ac.go0[a] + 0x10));
                    if (t) { SpotAdd(a, t); if (g_ac.nt[a]) { g_ac.guess[a] = 1; ++g_nm_guess; } }
                }
            }
        }
        // spawn spots of a wanted fish with none of it swimming there (dimmed marks after the live ones)
        for (int a = 0; a < an; ++a) {
            if (g_ac.nt[a]) ++nf.aknown;
            if (g_ac.pos[a]) ++nf.apos;
            if (!g_ac.nt[a]) { if (g_ac.tried[a] == 2 && !g_ac.ngo[a]) ++nf.nogo; else ++nf.unk; continue; }
            int8_t pl = -1; uint8_t mis = 0; int32_t t = 0;
            for (int q = 0; q < g_ac.nt[a]; ++q) {
                const int32_t tq = g_ac.tid[a][q];
                int32_t drop = 0;
                const FishGives* fg = nullptr;
                for (int i = 0; i < g_nfg; ++i) if (g_fg[i].tid == tq) fg = &g_fg[i];
                if (fg) drop = fg->drop;
                else { const uint64_t d = DmDict(in, g, kDmFishInfo, "FishInfoDataEntity"); const uint64_t v = d ? DictFindInt(g, d, tq) : 0;
                       drop = v ? g.s32(v + 0x7C) : 0; fg = FishGivesOf(in, g, tq, drop); }
                auto hit = [&](int32_t w) {
                    if (w <= 0) return false;
                    if (w == tq || w == drop) return true;
                    for (int r = 0; fg && r < fg->n; ++r) if (fg->id[r] == w) return true;
                    return false;
                };
                for (int j = 0; j < g_pl_n && j < 8; ++j) if ((kEcoMapMarks || g_pl_pin[j] != -2) && (hit(g_pl_id[j]) || hit(nf.plitem[j]))) { pl = static_cast<int8_t>(j); t = tq; }
                for (int r = 0; r < g_mp.nmtgt; ++r) if (hit(g_mp.mtgt[r])) { mis = 1; if (!t) t = tq; }
            }
            // 0.37.19: a spot that picks one fish among several at random is only a "maybe"; a spot with fish out
            // now and none of them wanted has already picked something else. Only sure spots are drawn.
            const bool sure = g_ac.nt[a] == 1 && !g_ac.live[a];
            if (pl >= 0) { if (sure) ++nf.pinspots; else ++nf.pinmaybe; }
            if (pl < 0 && !mis) continue;
            if (!sure || !g_ac.pos[a] || live_want[a] || nf.n >= kFm) continue;
            const int k = nf.n++;
            nf.obj[k] = 0; nf.tid[k] = t; nf.pl[k] = pl; nf.mis[k] = mis; nf.area[k] = 1;
            nf.x[k] = g_ac.x[a]; nf.y[k] = g_ac.y[a]; nf.has[k] = 1; ++nf.nareas;
        }
        nf.rr = f.rr; f = nf;
    }
    // spot positions (they do not move): a few per frame until all are known
    for (int r = 0; r < 4; ++r) {
        const int an2 = f.allocs < kAc ? f.allocs : kAc;
        if (an2 <= 0) break;
        const int a = (g_ac.rr = (g_ac.rr + 1) % an2);
        if (!g_ac.ptr[a] || g_ac.pos[a]) continue;
        const uint64_t nat = g.ptr(g_ac.ptr[a] + 0x10);
        float x, y;
        if (nat && MpPos(g, nat, &x, &y) && x > -150 && x < 150 && y > -400 && y < 60) {
            g_ac.x[a] = x; g_ac.y[a] = y; g_ac.pos[a] = 1;
            if (g_ac.nt[a]) FishMemPut(x, y, g_ac.tid[a][0]);
        }
    }
    for (int r = 0; r < 6 && f.n; ++r) {        // positions: they swim, a few per frame
        const int i = (f.rr = (f.rr + 1) % f.n);
        if (f.area[i] || !f.obj[i]) continue;
        const uint64_t nat = g.ptr(f.obj[i] + 0x10);
        float x, y;
        if (nat && MpPos(g, nat, &x, &y) && x > -150 && x < 150 && y > -400 && y < 60) { f.x[i] = x; f.y[i] = y; f.has[i] = 1; }
    }
}
void FishPublish(const EdenDsmodHostApi* h) {
    FishMarks& f = g_fm; void* u = h->userdata;
    const int64_t hw = static_cast<int64_t>(kMapW * 0.5f);
    for (int k = 0; k < kFm; ++k) {
        Buf ko, kx, ky, kt, ks; ko.add("fm.on").num(k); kx.add("fm.x").num(k); ky.add("fm.y").num(k); kt.add("fm.t").num(k); ks.add("fm.src").num(k);
        bool on = g_mk_ok && k < f.n && f.has[k];
        const int64_t X = on ? MapDX(f.x[k]) : 0, Y = on ? MapNY(f.y[k]) : 0;
        on = on && Y >= 0 && Y <= 1000 && X >= -hw && X <= hw;
        h->publish_i64(u, ko.s, on ? 1 : 0);
        if (!on) continue;
        h->publish_i64(u, kx.s, X); h->publish_i64(u, ky.s, Y);
        h->publish_i64(u, kt.s, (f.pl[k] >= 0 ? 1 : 2) + (f.area[k] ? 2 : 0));   // 3 / 4: a spawn spot
        Buf src; if (f.pl[k] >= 0 && g_pl_ic[f.pl[k]][0]) src.add("module:icon/").add(g_pl_ic[f.pl[k]]);
        h->publish_text(u, ks.s, src.s);
    }
    Buf d; d.add("FISH lists ").num(f.allocs).add(" fish ").num(f.total).add(" kept ").num(f.n).add(" | TIDs");
    d.add(" spots ").num(f.aknown).addc('/').num(f.apos).add(" areas ").num(f.nareas)
     .add(" prefab ").add(g_pp.state == 1 ? "ok" : g_pp.state == 2 ? "no" : "?");
    d.add(" digits ").num(g_nm_digit).add(" byname ").num(g_nm_exact).addc('+').num(g_nm_guess).add(" check ").num(g_nv_a).addc('/').num(g_nv_t);
    d.add(" school ").add(g_cp.state == 1 ? "ok" : g_cp.state == 2 ? "no" : "?").add(" noprefab ").num(f.nogo).add(" unknown ").num(f.unk)
     .add(" | PINNED FISH: ");
    if (!g_pl_n) d.add("none pinned");
    else if (f.pinspots || f.pinmaybe) d.add("at ").num(f.pinspots).add(" spots, maybe ").num(f.pinmaybe);
    else if (!f.unk && !f.nogo && f.apos) d.add("not in this dive");
    else d.add("not found yet");
    d.add(" kinds ").num(f.ntypes).add(" match ").num(f.nmatch);
    if (f.nmatch) d.add(" (").num(f.mtid).add(f.mhow == 1 ? " id" : f.mhow == 2 ? " drop" : " numbering").addc(')');
    for (int q = 0; q < f.nseen && q < 4; ++q) {
        d.addc(' ').num(f.seen[q]).addc('>');
        const FishGives* fg = nullptr; for (int i = 0; i < g_nfg; ++i) if (g_fg[i].tid == f.seen[q]) fg = &g_fg[i];
        if (!fg || !fg->n) d.addc('-');
        for (int k = 0; fg && k < fg->n && k < 3; ++k) { if (k) d.addc(','); d.num(fg->id[k]); }
    }
    d.add(" | pinned");
    for (int j = 0; j < g_pl_n && j < 8; ++j) d.addc(' ').num(g_pl_id[j]).addc('>').num(f.plitem[j]);
    d.add(" | mission"); for (int q = 0; q < g_mp.nmtgt; ++q) d.addc(' ').num(g_mp.mtgt[q]);
    d.add(g_mm_diag); d.add(g_dave_src ? " | dave own" : " | dave cam");
    h->publish_text(u, "fm.diag", d.s);
    h->publish_text(u, "fm.diag2", g_fdiag2);
    {   // 0.37.40: every fish kind out now, with what it gives (night dives: are the pinned ones among them?)
        Buf e; e.add("SEEN ").num(f.nseen).addc(':');
        for (int q = 0; q < f.nseen; ++q) {
            e.addc(' ').num(f.seen[q] % 100000).addc('>');
            const FishGives* fg = nullptr; for (int i = 0; i < g_nfg; ++i) if (g_fg[i].tid == f.seen[q]) fg = &g_fg[i];
            if (!fg || !fg->n) e.addc('-');
            for (int k = 0; fg && k < fg->n && k < 2; ++k) { if (k) e.addc(','); e.num(fg->id[k] % 100000); }
        }
        e.add(" | PIN"); for (int j = 0; j < g_pl_n && j < 4; ++j) e.addc(' ').num(g_pl_id[j] % 100000);
        h->publish_text(u, "fm.seen", e.s);

    }
    {   // 0.37.17 "fish 3": the fish that give the first pinned ingredient, the spots holding them, and wrong name guesses
        Buf e; e.add("PIN ").num(g_pl_n ? g_pl_id[0] : 0).add(" from:");
        int nfish = 0;
        for (int i = 0; i < g_nfg && g_pl_n; ++i) {
            const FishGives& fg = g_fg[i]; bool gv = fg.tid == g_pl_id[0] || fg.drop == g_pl_id[0];
            for (int q = 0; q < fg.n; ++q) gv = gv || fg.id[q] == g_pl_id[0];
            if (!gv) continue;
            if (nfish++ < 3) { e.addc(' ').add(FishNameOf(fg.tid) ? FishNameOf(fg.tid) : "?").addc('/').num(fg.tid); }
        }
        e.add(" (").num(nfish).add(" fish of ").num(g_nfg).add(" read)");
        for (int k = 0; k < g_nmiss; ++k) e.add(" | ").add(g_nmissv[k]);
        h->publish_text(u, "fm.diag3", e.s);
    }
}
void MarkerProbeAddrs(const Instance& in, const Guest& g, const EdenDsmodHostApi* h, uint64_t igm) {
    MarkerProbe& m = g_mp;
    const uint64_t comp = igm ? g.ptr(igm + 0xA0) : 0, nat = comp ? g.ptr(comp + 0x10) : 0;
    Buf a;
    a.add("MAIN ").hex(in.main_base).add(" HEAP ").hex(h->get_heap_begin ? h->get_heap_begin(h->userdata) : 0)
     .addc('-').hex(h->get_heap_end ? h->get_heap_end(h->userdata) : 0).add(" IGM ").hex(igm);
    memcpy(m.l4, a.s, a.n + 1);
    Buf a2; a2.add("CAM ").hex(g_map_cam).add(" DAVE ").hex(comp).add(" NAT ").hex(nat).add(" CHESTK ").hex(m.chest_k);
    memcpy(m.l6, a2.s, a2.n + 1);
    // what Dave's native object holds: p = a readable pointer, 0 = zero, . = other; 16 qwords, then
    // the same for the first pointer in it
    Buf b; b.add("NAT ");
    uint64_t first = 0;
    for (int o = 0; o < 0x80; o += 8) {
        const uint64_t v = nat ? g.ptr(nat + o) : 0;
        const bool p = MpPtrOk(g, v);
        if (p && !first && o) first = v;
        b.addc(!nat ? '?' : p ? 'p' : v ? '.' : '0');
    }
    b.add(" NEXT ");
    for (int o = 0; o < 0x80; o += 8) {
        const uint64_t v = first ? g.ptr(first + o) : 0;
        b.addc(!first ? '?' : MpPtrOk(g, v) ? 'p' : v ? '.' : '0');
    }
    memcpy(m.l5, b.s, b.n + 1);
}
void MarkerProbePublish(const EdenDsmodHostApi* h) {
    h->publish_text(h->userdata, "mk.p4", g_mp.l4);
    h->publish_text(h->userdata, "mk.p5", g_mp.l5);
    h->publish_text(h->userdata, "mk.p6", g_mp.l6);
    h->publish_text(h->userdata, "mk.p7", g_mp.l7);
    h->publish_text(h->userdata, "mk.p8", g_mp.l8);
    h->publish_text(h->userdata, "mk.p9", g_mp.l9);
    h->publish_text(h->userdata, "mk.p10", g_mp.l10);
    h->publish_text(h->userdata, "mk.p1", g_mp.l1);
    h->publish_text(h->userdata, "mk.p2", g_mp.l2);
    h->publish_text(h->userdata, "mk.p3", g_mp.l3);
    h->publish_text(h->userdata, "mk.p11", g_mp.l11);
    h->publish_text(h->userdata, "mk.p12", g_mp.l12);
}

void PublishMapProbeText(const EdenDsmodHostApi* h) {
    static const char* const keys[4] = {"map.p1", "map.p2", "map.p3", "map.p4"};
    for (int k = 0; k < 4; ++k) h->publish_text(h->userdata, keys[k], g_map_probe[k]);
}

// ---- the watch (round 23): the game's own date, time of day and weather, from the save's GameInfo
// (SaveData+0xF0, GameInfoSave): m_TodayDate ObscuredLong @0x88 (DateTime ticks: key @0, hidden @8),
// m_LastDayTime ObscuredInt @0xD8 (DayTimeState: 0 morning, 1 afternoon, 2 evening, 4 before
// evening), m_LastDayWeatherInfo @0xF8 -> WeatherInfo, type @0x10 (WeatherType: 0 sunny, 1 rainy,
// 2 heavy rain, 3 fog) -- the same object WeatherManager.CurrentWeatherType reads -- and
// m_NewMoonStartDate DateTime @0x130 (the moon at night).
constexpr uint64_t kTicksPerDay = UINT64_C(864000000000);
constexpr uint64_t kTicksMin = UINT64_C(630822816000000000), kTicksMax = UINT64_C(662380416000000000);  // 2000..2100
bool TicksOk(uint64_t t) { return t >= kTicksMin && t < kTicksMax; }
void CivilFromTicks(uint64_t ticks, int* y, int* mo, int* d, int* dow) {
    const int64_t days = static_cast<int64_t>(ticks / kTicksPerDay);   // since 0001-01-01, a Monday
    *dow = static_cast<int>((days + 1) % 7);                           // 0 = Sunday, as DayOfWeek
    int64_t z = days - 719162 + 719468;                                // days since 1970-03-01 era base
    const int64_t era = z / 146097, doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    *mo = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    *y = static_cast<int>(yoe + era * 400 + (*mo <= 2 ? 1 : 0));
}
int MoonStateForDay(int32_t d) {            // DayManager.CalculateMoonState, after "days % 30"
    if (d == 0) return 0;
    if (d < 1) return 7;
    if (d < 8) return 1;
    if (d == 8) return 2;
    if (d < 15) return 3;
    if (d == 15) return 4;
    if (d < 23) return 5;
    return d == 23 ? 6 : 7;
}
// The player's money (0.25.0): SaveData.m_PlayerInfo (+0x240) -> PlayerInfoSave.m_Gold (+0x10) and
// m_Bei (+0x24), both ObscuredInt (round 8 / round 23). Read from the save object the game keeps
// up to date (get_gold reads the same field), so it follows every sale and purchase.
uint64_t MainCanvas(const Instance& in, const Guest& g);   // (further down)
// 0.38.52 (round 66): the game's tutorial step, PlayerInfoSave.m_TutorialStep @0x4C (ObscuredInt, enum TutorialStep:
// 26 AllDone). While Bancho's tutorial runs (1..25) the bottom screen does not edit tonight's menu: adding a dish before
// his "Register a dish to the Menu!" step soft-locks the game (beta tester). -1 = not read (no block).
int32_t g_tut_step = -1;
bool TutorialRunning() { return g_tut_step >= 1 && g_tut_step < 26; }
void PublishGold(const Instance& in, const Guest& g, const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    const uint64_t pi = in.gs ? g.ptr(in.gs + 0x240) : 0;
    int32_t gold = 0, bei = 0;
    const bool ok = pi && g.obscured(pi + 0x10, &gold) && g.obscured(pi + 0x24, &bei) &&
                    gold >= 0 && gold < 1000000000 && bei >= 0 && bei < 1000000000;
    h->publish_i64(u, "gold.ok", ok ? 1 : 0);
    h->publish_i64(u, "gold.v", ok ? gold : 0);
    h->publish_i64(u, "bei.v", ok ? bei : 0);
    { int32_t ts = -1; g_tut_step = (pi && g.obscured(pi + 0x4C, &ts) && ts >= 0 && ts < 1000) ? ts : -1;
      h->publish_i64(u, "tut.step", g_tut_step); h->publish_i64(u, "tut.on", TutorialRunning() ? 1 : 0); }
    {   // 0.38.51 (round 65): Bei as the game writes it, and Credit / Trust: PlayerInfoSave.m_TrustPoint @0xF4 (ObscuredInt);
        // its maximum is the village HUD's TrustInfoPanelUI.MAX_Count @0x28 (MainCanvasManager +0x148), else 100
        char b[24]; int bn = 0; char br[24]; int bk = 0; uint32_t bv = ok ? static_cast<uint32_t>(bei) : 0;
        do { if (bk && bk % 3 == 0) br[bn++] = ','; br[bn++] = static_cast<char>('0' + bv % 10); bv /= 10; ++bk; } while (bv && bn < 22);
        for (int q = 0; q < bn; ++q) b[q] = br[bn - 1 - q];
        b[bn] = 0;
        h->publish_text(u, "bei.txt", ok ? b : "");
        h->publish_i64(u, "bei.dx", ok ? (bn < 7 ? 7 - bn : 0) * 23 : 0);   // 0.38.54: shifts the coin + amount so it ends by the title
        int32_t tp = -1; const bool tok = pi && g.obscured(pi + 0xF4, &tp) && tp >= 0 && tp < 100000;
        int32_t tmax = 100;
        const uint64_t mcm = MainCanvas(in, g), tpu = mcm ? g.ptr(mcm + 0x148) : 0; char c[32];
        if (tpu && g.cls(tpu, c, sizeof c) && eq(c, "TrustInfoPanelUI")) {
            float mx = 0; if (g.read(tpu + 0x28, &mx, 4) && mx >= 1.0f && mx < 100000.0f) tmax = static_cast<int32_t>(mx + 0.5f);
        }
        h->publish_i64(u, "trust.ok", tok ? 1 : 0);
        h->publish_i64(u, "trust.v", tok ? tp : 0);
        h->publish_i64(u, "trust.max", tmax);
        h->publish_i64(u, "trust.pct", tok ? (tp >= tmax ? 1000 : tp * 1000 / tmax) : 0);
        { Buf t; if (tok) t.num(tp).addc('/').num(tmax); h->publish_text(u, "trust.txt", t.s); }
    }
    // as the game writes it: 1,234,567
    char t[24]; int n = 0; char r[24]; int k = 0; uint32_t v = ok ? static_cast<uint32_t>(gold) : 0;
    do { if (k && k % 3 == 0) r[n++] = ','; r[n++] = static_cast<char>('0' + v % 10); v /= 10; ++k; } while (v);
    for (int i = 0; i < n; ++i) t[i] = r[n - 1 - i];
    t[n] = 0;
    h->publish_text(u, "gold.txt", t);
}
// ---------------------------------------------------------------- 0.37.67: text keys -> the game's words, many at once
// The phone apps show text keys (Mail_Title_DiverCity02 ...). Each is looked up in DataManager.TextDataDic (english
// @0x18 of the *TextData); all pending keys share one walk of the dictionary (kTextBudget entries per sample).
constexpr int kTexts = 384, kTextBudget = 5000;   // 0.38.57: 192 (was 64); 0.38.59: 384 -- + the missions' titles, givers and rows
struct TextEntry { char key[64]; char text[128]; int keylen; int state; int64_t start; };   // state 0 free, 1 pending, 2 found, 3 not in table; start: L.walked when asked
// 0.38.59: a new key no longer restarts the walk (with many keys asked in turn the walk never got far: on the boat the
// missions' titles stayed unread); the walk goes round, and a key is "not in the table" after one whole round of its own
struct TextLookup { TextEntry e[kTexts]; int32_t cursor; int pending; int next; int64_t walked; };
TextLookup g_tl;
// The words for `key` once known; until then (or if the table has none) the key itself, '_' shown as ' '.
const char* TextFor(const char* key, char* fallback, int cap) {
    if (!key[0]) { fallback[0] = 0; return fallback; }
    int len = 0; while (key[len] && len < 63) ++len;
    for (auto& t : g_tl.e) if (t.state && t.keylen == len && eq(t.key, key)) {
        if (t.state == 2) return t.text;
        break;
    }
    bool queued = false;
    for (auto& t : g_tl.e) if (t.state && t.keylen == len && eq(t.key, key)) { queued = true; break; }
    if (!queued) {
        TextEntry& t = g_tl.e[g_tl.next++ % kTexts];            // oldest slot is reused when full
        if (t.state == 1) --g_tl.pending;
        memcpy(t.key, key, static_cast<size_t>(len)); t.key[len] = 0; t.keylen = len; t.state = 1; t.text[0] = 0;
        t.start = g_tl.walked; ++g_tl.pending;                    // looked for from where the walk is now, for one round
    }
    int k = 0; for (; key[k] && k < cap - 1; ++k) fallback[k] = key[k] == '_' ? ' ' : key[k];
    fallback[k] = 0;
    return fallback;
}
// 1 the words are known, 0 still looking (or not asked yet), -1 the table has none
int TextKnown(const char* key) {
    int len = 0; while (key[len] && len < 63) ++len;
    for (auto& t : g_tl.e) if (t.state && t.keylen == len && eq(t.key, key)) return t.state == 2 ? 1 : t.state == 3 ? -1 : 0;
    return 0;
}
void TextLookupStep(Instance& in, const Guest& g) {
    TextLookup& L = g_tl;
    if (!L.pending) return;
    const uint64_t d = TextDict(in, g);
    if (!d) return;
    const uint64_t entries = g.ptr(d + 0x18);
    const int32_t cnt = g.s32(d + 0x20);
    if (cnt <= 0 || !entries) return;
    if (L.cursor >= cnt) L.cursor = 0;
    for (int b = 0; b < kTextBudget && L.pending; ++b, ++L.walked, L.cursor = L.cursor + 1 >= cnt ? 0 : L.cursor + 1) {
        uint8_t e[0x18];
        if (!g.read(entries + 0x20 + static_cast<uint64_t>(L.cursor) * 0x18, e, sizeof e)) continue;
        int32_t hash; uint64_t key, val;
        memcpy(&hash, e, 4); memcpy(&key, e + 8, 8); memcpy(&val, e + 0x10, 8);
        if (hash < 0 || !key || !val) continue;
        const int32_t kl = g.s32(key + 0x10);
        TextEntry* hit = nullptr;
        for (auto& t : L.e) if (t.state == 1 && t.keylen == kl) { hit = &t; break; }
        if (!hit) continue;
        uint16_t w[64];
        if (kl <= 0 || kl > 63 || !g.read(key + 0x14, w, static_cast<size_t>(kl) * 2)) continue;
        for (auto& t : L.e) {
            if (t.state != 1 || t.keylen != kl) continue;
            bool same = true;
            for (int i = 0; i < kl && same; ++i) same = w[i] == static_cast<uint8_t>(t.key[i]);
            if (!same) continue;
            if (!g.str(g.ptr(val + 0x18), t.text, sizeof t.text) || !t.text[0]) { t.state = 3; }
            else t.state = 2;
            --L.pending;
        }
    }
    for (auto& t : L.e)                                           // a whole round since it was asked: not in the table
        if (t.state == 1 && L.walked - t.start >= cnt) { t.state = 3; --L.pending; }
}

// ---------------------------------------------------------------- 0.37.65: the phone's open app (round 40 / 41)
// DavePhoneConroller (Singleton<DavePhoneConroller>, its generic class at main+9DA5928: statics +0xB8 -> instance):
// m_ContentsDic Dictionary<PhoneAppList, PhoneAppBase> @0x88 (every app opened this session), m_ExecuteApp @0xA8,
// IsPhoneOpened @0xF8. MailPanel (PhoneAppList.Message 14060006): m_DataList List<MailCellData> @0x68 (as shown),
// m_SelectedIndex @0x74. MailCellData: mailData @0x18 (MailData: SenderName @0x40, Title @0x48), receiveDate
// DateTime @0x20, isRead @0x28. Probe only: what the open Mail screen lists, on DIAG.
constexpr uint64_t kSingletonDavePhoneTI = 0x9DA5928;
constexpr int32_t kAppMessage = 14060006, kAppEcoWatcher = 14060018, kAppDuffShop = 14060002;
constexpr int kMailRows = 16;                 // 0.37.67: the Mail page shows up to this many
struct MailRow { char key_t[64], key_s[48], icon[48]; uint64_t ticks; int unread; int32_t tid, sortv; };
struct PhoneProbe { uint64_t dpc, mail; int32_t exec; int opened, state, n, sel, from_save, total; MailRow row[kMailRows]; char diag[200]; };
PhoneProbe g_ph;
int g_ph_show;                               // 0.37.66: the probe panel on APPS (tap Mail), as DIAG is out of reach on the boat
uint64_t DavePhone(const Instance& in, const Guest& g) {
    char c[32];
    const uint64_t k = g.ptr(in.main_base + kSingletonDavePhoneTI);
    const uint64_t st = k ? g.ptr(k + kStatics) : 0;
    for (int j = 0; st && j < 4; ++j) {
        const uint64_t o = g.ptr(st + 8 * static_cast<uint64_t>(j));
        if (o && g.ptr(o + 0x10) && g.cls(o, c, sizeof c) && eq(c, "DavePhoneConroller")) return o;
    }
    return 0;
}
uint64_t PhoneApp(const Guest& g, uint64_t dpc, int32_t app) {   // m_ContentsDic[app]
    const uint64_t d = g.ptr(dpc + 0x88);
    const uint64_t entries = d ? g.ptr(d + 0x18) : 0;
    const int32_t cnt = d ? g.s32(d + 0x20) : 0;
    for (int32_t i = 0; entries && i < cnt && i < 64; ++i) {
        const uint64_t e = entries + 0x20 + static_cast<uint64_t>(i) * 0x18;
        if (g.s32(e) >= 0 && g.s32(e + 8) == app) return g.ptr(e + 0x10);
    }
    return 0;
}
// 0.37.69: the mail as the save keeps it, so the list is there before Mail was ever opened (the Mail screen only
// fills its list when shown). SaveData.m_MailSaveData @0x1A0 -> MailSave.m_ReceiveMails Dictionary<string, MailInfo>
// @0x10. MailInfo (Anti-Cheat Toolkit values): m_MailID ObscuredInt @0x18, m_ReceiveDate ObscuredLong @0x30
// {key @0, hidden @8}, m_IsRead ObscuredBool @0x58 {key byte @0, hidden int @4: (hidden ^ key) == 213 true, 181
// false; else fakeValue @9}, m_SortingValue ObscuredInt @0x64. Texts from DataManager.MailDic @0x418
// (Dictionary<int, MailData>). Ordered as the Mail screen orders them: newest first, then SortingValue.
uint64_t IntDictGet(const Guest& g, uint64_t d, int32_t key) {
    const uint64_t entries = d ? g.ptr(d + 0x18) : 0;
    const int32_t cnt = d ? g.s32(d + 0x20) : 0;
    for (int32_t i = 0; entries && i < cnt && i < 4096; ++i) {
        const uint64_t e = entries + 0x20 + static_cast<uint64_t>(i) * 0x18;
        if (g.s32(e) >= 0 && g.s32(e + 8) == key) return g.ptr(e + 0x10);
    }
    return 0;
}
void MailFromSave(Instance& in, const Guest& g, Buf& d) {
    PhoneProbe& p = g_ph;
    const uint64_t ms = in.gs ? g.ptr(in.gs + 0x1A0) : 0;
    const uint64_t dict = ms ? g.ptr(ms + 0x10) : 0;
    const uint64_t entries = dict ? g.ptr(dict + 0x18) : 0;
    const int32_t cnt = dict ? g.s32(dict + 0x20) : 0;
    const uint64_t dm = DataManager(in, g);
    const uint64_t mdic = dm ? g.ptr(dm + 0x418) : 0;
    if (!entries || !mdic) { d.add(" | save mail ").add(!in.gs ? "no save" : !dict ? "none" : "no MailDic"); return; }
    MailRow tmp[kMailRows]; int n = 0, total = 0;
    char c[24];
    for (int32_t i = 0; i < cnt && i < 512; ++i) {
        const uint64_t e = entries + 0x20 + static_cast<uint64_t>(i) * 0x18;
        if (g.s32(e) < 0) continue;
        const uint64_t mi = g.ptr(e + 0x10);
        if (!mi) continue;
        MailRow r{}; int32_t id = 0, sv = 0;
        if (!g.obscured(mi + 0x18, &id)) continue;
        g.obscured(mi + 0x64, &sv);
        uint64_t kv[2] = {0, 0}; g.read(mi + 0x30, kv, sizeof kv); r.ticks = (kv[0] ^ kv[1]) & UINT64_C(0x3FFFFFFFFFFFFFFF);
        uint8_t bk = 0, fake = 0; int32_t bh = 0; g.read(mi + 0x58, &bk, 1); bh = g.s32(mi + 0x5C); g.read(mi + 0x61, &fake, 1);
        const int32_t bv = bh ^ bk;
        const bool read = bv == 213 ? true : bv == 181 ? false : fake != 0;
        r.unread = read ? 0 : 1; r.tid = id; r.sortv = sv;
        const uint64_t md = IntDictGet(g, mdic, id);
        if (!md || !g.cls(md, c, sizeof c) || !eq(c, "MailData")) continue;
        g.str(g.ptr(md + 0x48), r.key_t, sizeof r.key_t); g.str(g.ptr(md + 0x40), r.key_s, sizeof r.key_s);
        g.str(g.ptr(md + 0x38), r.icon, sizeof r.icon);
        ++total;
        // keep the newest kMailRows (insertion into a short sorted list)
        auto before = [](const MailRow& a, const MailRow& b) { return a.ticks != b.ticks ? a.ticks > b.ticks : a.sortv > b.sortv; };
        int at = n; while (at > 0 && before(r, tmp[at - 1])) --at;
        if (at >= kMailRows) continue;
        for (int k = (n < kMailRows ? n : kMailRows - 1); k > at; --k) tmp[k] = tmp[k - 1];
        tmp[at] = r; if (n < kMailRows) ++n;
    }
    for (int i = 0; i < kMailRows; ++i) p.row[i] = i < n ? tmp[i] : MailRow{};
    p.n = total < kMailRows ? total : kMailRows; p.sel = -1; p.from_save = 1; p.total = total;
    d.add(" | save mail ").num(total);
}
int PhoneState(const Guest& g, uint64_t dpc);
extern int g_mail_shown;
void PhoneStep(Instance& in, const Guest& g) {
    PhoneProbe& p = g_ph;
    char c[40];
    p.dpc = DavePhone(in, g);
    p.from_save = 0; p.total = 0;
    Buf d;
    if (!p.dpc) { p.state = -1; p.mail = 0; p.n = 0; d.add("phone: not loaded"); MailFromSave(in, g, d); memcpy(p.diag, d.s, static_cast<size_t>(d.n) + 1); return; }
    p.exec = g.s32(p.dpc + 0xA8);
    uint8_t ob = 0; g.read(p.dpc + 0xF8, &ob, 1); p.opened = ob ? 1 : 0;
    p.state = PhoneState(g, p.dpc);                                   // 0.37.85: NowState (0 closed) for BACK -> APPS
    if (p.state != 2 || p.exec != kAppMessage) g_mail_shown = 0;      // 0.37.86: Mail left -> no open mail
    const uint64_t dict = g.ptr(p.dpc + 0x88);
    d.add("phone: open ").num(p.opened).add(" app ").num(p.exec).add(" apps ").num(dict ? g.s32(dict + 0x20) : -1);
    const uint64_t mp = PhoneApp(g, p.dpc, kAppMessage);
    if (mp && g.cls(mp, c, sizeof c) && eq(c, "MailPanel")) {
        p.mail = mp;
        const uint64_t list = g.ptr(mp + 0x68);
        const uint64_t items = list ? g.ptr(list + 0x10) : 0;
        const int n = list ? g.s32(list + 0x18) : 0;
        p.n = n < 0 ? 0 : n;
        p.sel = g.s32(mp + 0x74);
        d.add(" | mail ").num(p.n).add(" sel ").num(p.sel);
        for (int i = 0; i < kMailRows; ++i) {
            MailRow& r = p.row[i];
            r.key_t[0] = r.key_s[0] = r.icon[0] = 0; r.ticks = 0; r.unread = 0;
            if (!items || i >= p.n) continue;
            const uint64_t cell = g.ptr(items + 0x20 + 8 * static_cast<uint64_t>(i));
            const uint64_t md = cell ? g.ptr(cell + 0x18) : 0;
            if (md) { g.str(g.ptr(md + 0x48), r.key_t, sizeof r.key_t); g.str(g.ptr(md + 0x40), r.key_s, sizeof r.key_s);
                      g.str(g.ptr(md + 0x38), r.icon, sizeof r.icon); r.tid = g.s32(md + 0x10); }
            g.read(cell + 0x20, &r.ticks, 8); r.ticks &= UINT64_C(0x3FFFFFFFFFFFFFFF);
            uint8_t rd = 1; g.read(cell + 0x28, &rd, 1); r.unread = rd ? 0 : 1;
        }
    } else { p.mail = 0; p.n = 0; d.add(mp ? " | Message app is not a MailPanel" : " | mail not opened yet"); }
    if (!p.n) MailFromSave(in, g, d);                                // 0.37.69: before Mail is shown, the save's own list
    memcpy(p.diag, d.s, static_cast<size_t>(d.n) + 1);
}
// ---------------------------------------------------------------- 0.37.72: EcoWatcher (round 44)
// The list as EcoWatcherListPanel.AddCellData builds it: MissionManager (Singleton<MissionManager>, TypeInfo
// main+9DB2850) <InProgressList> @0x38 where Type == OceanLab (2), then a divider, then the cleared OceanLab
// missions (m_MissionList Dictionary<int, MissionData> @0x68, State Done). MissionData: TID 0x10, NameTextID 0x28,
// Type 0x38, State 0x3C (2 in progress, 3 clear = ready to deliver, 4 done), IsNew 0xE4. Research: SaveData.
// m_PlayerInfo (+0x240) research points ObscuredInt @0xE0; levels from DataManager.ResearchLevelUpDataDic @0x2E0
// (LevelUpPoint: Level 0x14, RequiredPoint 0x18).
int PhoneState(const Guest& g, uint64_t dpc);
constexpr uint64_t kMissionManagerTI = 0x9DB2850;
constexpr int kEcoRows = 14;
constexpr int kEcoGoals = 4;
struct EcoRow { uint64_t md; int32_t tid, state, rp; int div, isnew, ng; char key[64];
                int32_t gid[kEcoGoals], gnow[kEcoGoals], gmax[kEcoGoals], gtype[kEcoGoals]; char gkey[kEcoGoals][48], gicon[kEcoGoals][48], gname[kEcoGoals][48]; };
struct EcoState { EcoRow row[kEcoRows]; int n, inprog, done, lv, rp, rp_next, rp_base, sel; char diag[320]; };
EcoState g_eco;
uint64_t SingletonObj(const Instance& in, const Guest& g, uint64_t typeinfo, const char* cls_name) {
    const uint64_t cls = g.ptr(in.main_base + typeinfo);
    const uint64_t parent = cls ? g.ptr(cls + kParent) : 0;
    const uint64_t st = parent ? g.ptr(parent + kStatics) : 0;
    const uint64_t obj = st ? g.ptr(st) : 0;
    char c[32];
    return obj && g.cls(obj, c, sizeof c) && eq(c, cls_name) ? obj : 0;
}
// 0.37.74 (round 46) / 0.37.75 (round 47): what Dave holds of an item, as the EcoWatcher row counts a Delivering
// goal: the loot box (LootBox.TotalItemCount: the cargo's slots of that item) + either the restaurant's storage
// (the item's Items.ItemType @0x38 is Ingredient 4 / Ingredient_Catch 11 / SeahorseRacer 18 / PasteBait 24:
// IngredientsStorage of Items.ItemDataID @0x60) or the save's inventory (SaveData.GetInventoryItemTotalCount:
// m_InventoryItemSlotData Dictionary<string, InventoryItemSlotSave> @0x140, ItemID ObscuredInt @0x2C,
// TotalCount ObscuredInt @0x40). Items from DataManager.ItemBaseDataDic @0x1C0.
int32_t g_eh_item[32], g_eh_ing[32]; int g_neh;
int32_t EcoIngredientOf(Instance& in, const Guest& g, int32_t item) {
    for (int i = 0; i < g_neh; ++i) if (g_eh_item[i] == item) return g_eh_ing[i];
    const uint64_t d = IngredientTable(in, g);
    const uint64_t en = d ? g.ptr(d + 0x18) : 0; const int32_t cnt = d ? g.s32(d + 0x20) : 0;
    if (!en || cnt <= 0 || cnt > 20000) return 0;                   // not loaded yet: asked again next time
    int32_t ing = 0;
    for (int32_t i = 0; i < cnt && !ing; ++i) {
        const uint64_t e = en + 0x20 + static_cast<uint64_t>(i) * 0x18, v = g.ptr(e + 0x10);
        if (g.s32(e) >= 0 && v && g.s32(v + 0x54) == item) ing = g.s32(v + 0x10);
    }
    if (g_neh < 32) { g_eh_item[g_neh] = item; g_eh_ing[g_neh++] = ing; }
    return ing;
}
int32_t g_eh_box, g_eh_store, g_eh_inv, g_eh_type = -1, g_eh_ingid;   // the last goal's parts (eco.diag)
int32_t g_eco_pin;                           // 0.37.73: the pinned EcoWatcher mission (its TID; 0 none)
int32_t InventoryCount(const Instance& in, const Guest& g, int32_t item) {
    const uint64_t d = in.gs ? g.ptr(in.gs + 0x140) : 0;
    const uint64_t en = d ? g.ptr(d + 0x18) : 0; const int32_t cnt = d ? g.s32(d + 0x20) : 0;
    if (!en || cnt <= 0 || cnt > 4096) return 0;
    int32_t n = 0;
    for (int32_t i = 0; i < cnt; ++i) {
        const uint64_t e = en + 0x20 + static_cast<uint64_t>(i) * 0x18, v = g.ptr(e + 0x10);
        int32_t id = 0, c = 0;
        if (g.s32(e) < 0 || !v || !g.obscured(v + 0x2C, &id) || id != item || !g.obscured(v + 0x40, &c)) continue;
        if (c > 0 && c < 100000) n += c;
    }
    return n;
}
int32_t EcoHeld(Instance& in, const Guest& g, int32_t item) {
    int32_t box = 0, store = 0, inv = 0;
    for (int i = 0; i < in.n; ++i) if (in.items[i].id == item) box += in.items[i].count > 0 ? in.items[i].count : 1;
    // the item's kind, once per item (ItemBaseDataDic holds several IItemBase classes: only an Items counts)
    static int32_t ci[32], cty[32], cing[32]; static int cn;
    int at = -1; for (int i = 0; i < cn; ++i) if (ci[i] == item) at = i;
    if (at < 0) {
        const uint64_t dm = DataManager(in, g);
        const uint64_t itd = dm ? g.ptr(dm + 0x1C0) : 0;
        char c[32];
        const uint64_t v = itd && g.cls(itd, c, sizeof c) && starts(c, "Dictionary`2") ? DictFindInt(g, itd, item) : 0;
        if (v && g.cls(v, c, sizeof c) && eq(c, "Items") && cn < 32) { at = cn++; ci[at] = item; cty[at] = g.s32(v + 0x38); cing[at] = g.s32(v + 0x60); }
    }
    const bool it = at >= 0;
    int32_t ty = -1, ing = 0;
    if (it) { ty = cty[at]; if (ty == 4 || ty == 11 || ty == 18 || ty == 24) ing = cing[at]; }
    else ing = EcoIngredientOf(in, g, item);                         // Items not found: both halves (fallback)
    // 0.37.76: graded stacks name the base ingredient as their parent (as the recipe counts do)
    for (int s2 = 0; ing && s2 < in.ning; ++s2) if (in.ing[s2].id == ing || in.ing[s2].parent == ing) store += in.ing[s2].count;
    if (!it || !ing) inv = InventoryCount(in, g, item);
    g_eh_box = box; g_eh_store = store; g_eh_inv = inv; g_eh_type = ty; g_eh_ingid = ing;
    return box + store + inv;
}
void EcoStep(Instance& in, const Guest& g) {
    EcoState& e = g_eco;
    Buf d;
    e.n = e.inprog = e.done = 0; e.sel = -1;
    const uint64_t mm = SingletonObj(in, g, kMissionManagerTI, "MissionManager");
    const uint64_t items_tbl = ItemTable(in, g);
    const uint64_t fish_tbl = DmDict(in, g, kDmFishInfo, "FishInfoDataEntity");
    Buf gdb; gdb.add("goals");
    if (!mm) { d.add("eco: no MissionManager"); DcCopy(e.diag, sizeof e.diag, d.s); return; }
    auto add = [&](uint64_t md, int div) {
        if (e.n >= kEcoRows) return;
        EcoRow& r = e.row[e.n++]; r = EcoRow{}; r.md = md; r.div = div;
        if (!md) return;
        r.tid = g.s32(md + 0x10); r.state = g.s32(md + 0x3C);
        uint8_t nw = 0; g.read(md + 0xE4, &nw, 1); r.isnew = nw ? 1 : 0;
        g.str(g.ptr(md + 0x28), r.key, sizeof r.key);
        // 0.37.73 (round 45): the goals -- the current task (m_CurrentTaskNode @0x100, else m_TaskList's first,
        // @0xF8 head @0x10; LinkedListNode item @0x28) -> ConditionGroup @0x98 (MissionConditionData: Type 0x14,
        // Target 0x20 = an item, NowCount 0x28, MaxCount 0x2C); the item's name / picture from the item table.
        // RP: the first reward's Count (RewardGroup @0x68, Reward.Count @0x38).
        uint64_t node = g.ptr(md + 0x100);
        if (!node) { const uint64_t ll = g.ptr(md + 0xF8); node = ll ? g.ptr(ll + 0x10) : 0; }
        const uint64_t task = node ? g.ptr(node + 0x28) : 0;
        char tc[24];
        if (task && g.cls(task, tc, sizeof tc) && eq(tc, "MissionTaskData")) {
            const uint64_t cl = g.ptr(task + 0x98);
            const uint64_t ci = cl ? g.ptr(cl + 0x10) : 0;
            const int32_t cn = cl ? g.s32(cl + 0x18) : 0;
            for (int32_t k = 0; ci && k < cn && k < 8 && r.ng < kEcoGoals; ++k) {
                const uint64_t cd = g.ptr(ci + 0x20 + 8 * static_cast<uint64_t>(k));
                if (!cd) continue;
                // 0.37.74 (round 46): EcoWatcherMissionCellView.SetConditionInfo shows only Kill (15, 29..31: a fish,
                // FishInfoDataDic; count NowCount clamped) and Delivering (18: an item; until met, what Dave holds:
                // the storage + the loot box -- not NowCount, which moves only on delivery)
                const int32_t ty = g.s32(cd + 0x14), tg = g.s32(cd + 0x20);
                const int32_t now0 = g.s32(cd + 0x28), mx = g.s32(cd + 0x2C);
                const int32_t clamped = now0 < 0 ? 0 : now0 < mx ? now0 : mx;
                char gk[48] = {0}, gi[48] = {0}; int32_t gn = 0;
                if (ty == 15 || (ty >= 29 && ty <= 31)) {
                    const uint64_t fv = fish_tbl && tg > 0 ? DictFindInt(g, fish_tbl, tg) : 0;
                    if (!fv) continue;
                    g.str(g.ptr(fv + 0x18), gk, sizeof gk);
                    if (!(g.str(g.ptr(fv + 0xA8), gi, sizeof gi) && gi[0])) g.str(g.ptr(fv + 0xB0), gi, sizeof gi);
                    gn = clamped;
                } else if (ty == 18) {
                    const uint64_t iv = items_tbl && tg > 0 ? DictFindInt(g, items_tbl, tg) : 0;
                    if (!iv) continue;
                    g.str(g.ptr(iv + 0x18), gk, sizeof gk); g.str(g.ptr(iv + 0x38), gi, sizeof gi);
                    gn = now0 >= mx ? mx : EcoHeld(in, g, tg);
                    if (now0 < mx && (r.tid == g_eco_pin || (!g_eco_pin && e.n == 1)) && gdb.n < 150)
                        gdb.addc(' ').num(tg).addc(':').num(g_eh_box).addc('+').num(g_eh_store).addc('+').num(g_eh_inv)
                           .add(" k").num(g_eh_type).add(" ing").num(g_eh_ingid);
                } else continue;                                         // talk, photo ...: the game's row shows none
                const int q = r.ng++;
                r.gid[q] = tg; r.gnow[q] = gn; r.gmax[q] = mx; r.gtype[q] = ty;
                DcCopy(r.gkey[q], sizeof r.gkey[q], gk); DcCopy(r.gicon[q], sizeof r.gicon[q], gi);
            }
        }
        const uint64_t rl = g.ptr(md + 0x68);
        const uint64_t ri = rl ? g.ptr(rl + 0x10) : 0;
        const uint64_t rw = ri && g.s32(rl + 0x18) > 0 ? g.ptr(ri + 0x20) : 0;
        r.rp = rw ? g.s32(rw + 0x38) : 0;
    };
    const uint64_t ipl = g.ptr(mm + 0x38);
    const uint64_t ipi = ipl ? g.ptr(ipl + 0x10) : 0;
    const int32_t ipn = ipl ? g.s32(ipl + 0x18) : 0;
    for (int32_t i = 0; ipi && i < ipn && i < 256; ++i) {
        const uint64_t md = g.ptr(ipi + 0x20 + 8 * static_cast<uint64_t>(i));
        if (md && g.s32(md + 0x38) == 2) { ++e.inprog; add(md, 0); }
    }
    const uint64_t ml = g.ptr(mm + 0x68);
    const uint64_t ment = ml ? g.ptr(ml + 0x18) : 0;
    const int32_t mcnt = ml ? g.s32(ml + 0x20) : 0;
    bool divided = false;
    for (int32_t i = 0; ment && i < mcnt && i < 8192; ++i) {
        const uint64_t en = ment + 0x20 + static_cast<uint64_t>(i) * 0x18;
        if (g.s32(en) < 0) continue;
        const uint64_t md = g.ptr(en + 0x10);
        if (!md || g.s32(md + 0x38) != 2 || g.s32(md + 0x3C) != 4) continue;
        ++e.done;
        if (!divided) { add(0, 1); divided = true; }
        add(md, 0);
    }
    // research level / points
    const uint64_t pi = in.gs ? g.ptr(in.gs + 0x240) : 0;
    int32_t rp = 0; if (pi) g.obscured(pi + 0xE0, &rp);
    e.rp = rp; e.lv = 0; e.rp_next = 0; e.rp_base = 0;
    const uint64_t dm = DataManager(in, g);
    const uint64_t ld = dm ? g.ptr(dm + 0x2E0) : 0;
    const uint64_t lent = ld ? g.ptr(ld + 0x18) : 0;
    const int32_t lcnt = ld ? g.s32(ld + 0x20) : 0;
    for (int32_t i = 0; lent && i < lcnt && i < 512; ++i) {
        const uint64_t en = lent + 0x20 + static_cast<uint64_t>(i) * 0x18;
        if (g.s32(en) < 0) continue;
        const uint64_t lv = g.ptr(en + 0x10);
        if (!lv) continue;
        const int32_t L = g.s32(lv + 0x14), req = g.s32(lv + 0x18);
        if (req <= rp && L > e.lv) { e.lv = L; e.rp_base = req; }
        if (req > rp && (!e.rp_next || req < e.rp_next)) e.rp_next = req;
    }
    // the cursor, when EcoWatcher is open on the top screen
    const uint64_t dpc = DavePhone(in, g);
    if (dpc && PhoneState(g, dpc) == 2 && g.s32(dpc + 0xA8) == kAppEcoWatcher) {
        const uint64_t ep = PhoneApp(g, dpc, kAppEcoWatcher);
        const uint64_t lp = ep ? g.ptr(ep + 0x38) : 0;
        const uint64_t list = lp ? g.ptr(lp + 0x88) : 0, items = list ? g.ptr(list + 0x10) : 0;
        const int32_t f = lp ? g.s32(lp + 0x90) : -1;
        const uint64_t cd = items && f >= 0 ? g.ptr(items + 0x20 + 8 * static_cast<uint64_t>(f)) : 0;
        const uint64_t fmd = cd ? g.ptr(cd + 0x18) : 0;
        for (int i = 0; i < e.n && fmd; ++i) if (e.row[i].md == fmd) e.sel = i;
    }
    d.add("eco: in progress ").num(e.inprog).add(" done ").num(e.done).add(" | rp ").num(rp).add(" lv ").num(e.lv).add(" next ").num(e.rp_next)
     .add(" | ").add(gdb.s);
    DcCopy(e.diag, sizeof e.diag, d.s);
}
int g_eco_off;                                // 0.37.76: the first row shown (the page shows kEcoVis rows)
constexpr int kEcoVis = 6;
const EcoRow* EcoPinned() {
    for (int i = 0; i < g_eco.n && g_eco_pin; ++i) if (!g_eco.row[i].div && g_eco.row[i].tid == g_eco_pin && g_eco.row[i].state != 4) return &g_eco.row[i];
    return nullptr;
}
int EcoPinGoals(int32_t* id, int32_t* have, int32_t* need, const char** nm, const char** ic, int cap) {
    const EcoRow* r = EcoPinned();
    int m = 0;
    for (int k = 0; r && k < r->ng && m < cap; ++k, ++m) { id[m] = r->gid[k]; have[m] = r->gnow[k]; need[m] = r->gmax[k]; nm[m] = r->gname[k]; ic[m] = r->gicon[k]; }
    return m;
}
void EcoPublish(const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    EcoState& e = g_eco;
    if (g_eco_pin && e.n && !EcoPinned()) g_eco_pin = 0;          // done (or gone): the pin goes, as a finished recipe hunt does
    h->publish_text(u, "eco.diag", e.diag);
    h->publish_i64(u, "eco.ok", e.n > 0 ? 1 : 0);
    h->publish_i64(u, "eco.lv", e.lv);
    { Buf b; b.num(e.rp); if (e.rp_next) b.add(" / ").num(e.rp_next); h->publish_text(u, "eco.rptxt", b.s); }
    h->publish_i64(u, "eco.rp_in", e.rp - e.rp_base);
    h->publish_i64(u, "eco.rp_span", e.rp_next > e.rp_base ? e.rp_next - e.rp_base : 1);
    h->publish_i64(u, "eco.inprog", e.inprog);
    h->publish_i64(u, "eco.done", e.done);
    Buf k, t2; char fb[128];
    { const int mx = e.n > kEcoVis ? e.n - kEcoVis : 0; if (g_eco_off > mx) g_eco_off = mx; if (g_eco_off < 0) g_eco_off = 0;
      h->publish_i64(u, "eco.can_up", g_eco_off > 0 ? 1 : 0); h->publish_i64(u, "eco.can_dn", g_eco_off < mx ? 1 : 0);
      h->publish_i64(u, "eco.rows", e.n < kEcoRows ? e.n : kEcoRows); }   // 0.38.5: the scrolled list's length
    for (int i = 0; i < kEcoRows; ++i) {
        const int j = i + g_eco_off;                                 // the page's row i shows mission row j
        const bool on = j < e.n && j < kEcoRows;
        const EcoRow& r = e.row[on ? j : 0];
        k.clear(); k.add("eco.on").num(i); h->publish_i64(u, k.s, on && !r.div ? 1 : 0);
        k.clear(); k.add("eco.div").num(i); h->publish_i64(u, k.s, on && r.div ? 1 : 0);
        k.clear(); k.add("eco.t").num(i); h->publish_text(u, k.s, on && !r.div ? NoApos(TextFor(r.key, fb, sizeof fb), t2) : "");
        k.clear(); k.add("eco.st").num(i); h->publish_i64(u, k.s, on && !r.div ? (r.state == 4 ? 2 : r.state == 3 ? 1 : 0) : -1);
        k.clear(); k.add("eco.new").num(i); h->publish_i64(u, k.s, on && !r.div && r.isnew ? 1 : 0);
        k.clear(); k.add("eco.cur").num(i); h->publish_i64(u, k.s, on && j == e.sel ? 1 : 0);
        k.clear(); k.add("eco.rp").num(i); h->publish_i64(u, k.s, on && !r.div ? r.rp : 0);
        k.clear(); k.add("eco.pin").num(i); h->publish_i64(u, k.s, on && !r.div && r.tid == g_eco_pin && r.state != 4 ? 1 : 0);
        k.clear(); k.add("eco.canpin").num(i); h->publish_i64(u, k.s, on && !r.div && r.state != 4 && r.ng > 0 ? 1 : 0);
        for (int q = 0; q < 3; ++q) {                                  // up to 3 goals on the row
            const bool gon = on && !r.div && q < r.ng;
            EcoRow& w = e.row[on ? j : 0];
            if (gon) { const char* nmv = TextFor(w.gkey[q], fb, sizeof fb); DcCopy(w.gname[q], sizeof w.gname[q], nmv); }
            Buf gv; if (gon) gv.num(r.gnow[q]).addc('/').num(r.gmax[q]);
            k.clear(); k.add("eco.gc").num(i).addc('_').num(q); h->publish_text(u, k.s, gv.s);
            gv.clear(); if (gon && r.gicon[q][0]) gv.add("module:icon/").add(r.gicon[q]);
            k.clear(); k.add("eco.gi").num(i).addc('_').num(q); h->publish_text(u, k.s, gv.s);
            k.clear(); k.add("eco.gon").num(i).addc('_').num(q); h->publish_i64(u, k.s, gon ? 1 : 0);
            k.clear(); k.add("eco.gok").num(i).addc('_').num(q); h->publish_i64(u, k.s, gon && r.gnow[q] >= r.gmax[q] ? 1 : 0);
        }
    }
    h->publish_i64(u, "eco.pinned", EcoPinned() ? 1 : 0);
}
// ---------------------------------------------------------------- 0.38.58: the active missions the tracker does not show
// 0.38.57 (round 67) listed MissionManager.<InProgressList> @0x38 whole: 17 on device, most of them the game's own
// bookkeeping (no title in the text table: "Mission Group Name 100199xx", or "(Not in Use)"). The game's All Missions list
// showed 4. Kept now: Main (0) / Side (1), State InProgress (2) or Clear (3), ShowHUD @0xA8, a title the text table knows
// (not "(...)"), and not one of the tracker's 3 (those keep their own cards, with the tracker's words). Each card shows what
// the game's list row shows: the giver's face, the kind, the title, "#<giver>" (NpcData NameTextID @0x28), and the tasks
// done / tasks (m_TaskList LinkedList @0xF8: head @0x10, count @0x18; done = the current task's place in it).
// MissionData: NameTextID 0x28, Type 0x38, State 0x3C, NpcData 0x40 (CharacterType 0x18), ShowHUD 0xA8, Selected 0xE5,
// m_CurrentTaskNode 0x100 (item @0x28, next @0x18) -> MissionTaskData Type 0x88 (3 VIP), ConditionGroup 0x98 (Target 0x20,
// NowCount 0x28, MaxCount 0x2C: the PIN's map targets).
constexpr int kAmMis = 24, kAmShow = 6;
constexpr int kAmRow = 4;   // 0.38.61: the task's rows on the card, as the tracker's
struct AmRow { int32_t tgt, now, mx, type; int cnt; char key[64], icon[48]; };
struct AmMis { uint64_t md; int32_t tid, type, state, vip, hud, done, ntask, nt, nrow, nrowdone; int32_t tgt[kAmConds]; int canpin;
               AmRow row[kAmRow]; int nr;
               char key[64], npckey[48], npcpic[48], npcname[64]; };
struct AmState { AmMis mis[kAmMis]; int nm, nlist; char diag[480]; };
AmState g_am;
int g_am_vis[kAmShow], g_am_nvis;                 // the cards shown (indexes into g_am.mis), set by the publish
char g_am_pkey[kAmShow][48], g_am_pfile[kAmShow][96];
char g_om_icn[kAmShow * 4][48]; bool g_om_ichas[kAmShow * 4], g_om_icknown[kAmShow * 4];   // per card row (kAmRow)
int TextKnown(const char* key);
void AllMissionsStep(Instance& in, const Guest& g) {
    AmState a{};
    const uint64_t mm = SingletonObj(in, g, kMissionManagerTI, "MissionManager");
    const uint64_t ipl = mm ? g.ptr(mm + 0x38) : 0;
    const uint64_t ipi = ipl ? g.ptr(ipl + 0x10) : 0;
    const int32_t ipn = ipl ? g.s32(ipl + 0x18) : 0;
    a.nlist = ipn;
    uint64_t mds[64]; int nmd = 0;
    // 0.38.59 (round 68): as the game's mission list builds itself (MissionListPanel.AddCellData): InProgressList
    // ordered by Type (Main 0, Side 1, Intermission 3; OceanLab 2 and the Jungle ones left out), ShowHUD only
    static const int32_t kOrder[3] = {0, 1, 3};
    for (int pass = 0; pass < 3; ++pass)
        for (int32_t i = 0; ipi && i < ipn && i < 256 && nmd < 64; ++i) {
            const uint64_t md = g.ptr(ipi + 0x20 + 8 * static_cast<uint64_t>(i));
            if (!md) continue;
            uint8_t hud = 0; g.read(md + 0xA8, &hud, 1);
            if (g.s32(md + 0x38) == kOrder[pass] && hud) mds[nmd++] = md;
        }
    for (int q = 0; q < nmd && a.nm < kAmMis; ++q) {
        const uint64_t md = mds[q];
        AmMis& m = a.mis[a.nm];
        m = AmMis{};
        m.md = md; m.tid = g.s32(md + 0x10); m.type = g.s32(md + 0x38); m.state = g.s32(md + 0x3C);
        uint8_t b = 0; g.read(md + 0xA8, &b, 1); m.hud = b ? 1 : 0;
        g.str(g.ptr(md + 0x28), m.key, sizeof m.key);
        if (!m.key[0]) continue;
        const uint64_t npc = g.ptr(md + 0x40);
        if (npc) {
            g.str(g.ptr(npc + 0x18), m.npckey, sizeof m.npckey);
            g.str(g.ptr(npc + 0x28), m.npcname, sizeof m.npcname);
            char c[24], t[48];
            for (uint64_t off = 0x18; off <= 0x50 && !m.npcpic[0]; off += 8) {
                const uint64_t sp = g.ptr(npc + off);
                if (sp && g.cls(sp, c, sizeof c) && eq(c, "String") && g.str(sp, t, sizeof t) && starts(t, "Mission_"))
                    DcCopy(m.npcpic, sizeof m.npcpic, t);
            }
        }
        // tasks: the list's length, and how many come before the current one
        const uint64_t ll = g.ptr(md + 0xF8);
        const uint64_t head = ll ? g.ptr(ll + 0x10) : 0;
        m.ntask = ll ? g.s32(ll + 0x18) : 0;
        if (m.ntask < 0 || m.ntask > 64) m.ntask = 0;
        uint64_t node = g.ptr(md + 0x100);
        if (node && head) {
            uint64_t n = head; int k = 0;
            while (n && n != node && k < m.ntask) { n = g.ptr(n + 0x18); ++k; if (n == head) { k = 0; break; } }
            m.done = n == node ? k : 0;
        }
        if (m.state >= 3 && m.ntask) m.done = m.ntask;
        if (!node) node = head;
        const uint64_t task = node ? g.ptr(node + 0x28) : 0;
        char tc[24];
        const bool tok = task && g.cls(task, tc, sizeof tc) && eq(tc, "MissionTaskData");
        m.vip = tok && g.s32(task + 0x88) == 3 ? 1 : 0;
        const uint64_t cl = tok ? g.ptr(task + 0x98) : 0;
        const uint64_t ci = cl ? g.ptr(cl + 0x10) : 0;
        const int32_t cn = cl ? g.s32(cl + 0x18) : 0;
        m.canpin = 1;
        const uint64_t itbl = cn > 0 ? ItemTable(in, g) : 0;
        for (int32_t k = 0; ci && k < cn && k < 12; ++k) {
            const uint64_t cd = g.ptr(ci + 0x20 + 8 * static_cast<uint64_t>(k));
            if (!cd) continue;
            const int32_t tg = g.s32(cd + 0x20), now = g.s32(cd + 0x28), mx = g.s32(cd + 0x2C);
            {   // a row the tracker shows (MissionHUDCondition.Bind: IsShowHUD @0x30, not DDay 10 / TimeLimit 16)
                uint8_t sh = 0; g.read(cd + 0x30, &sh, 1);
                const int32_t ct = g.s32(cd + 0x14);
                if (sh && ct != 10 && ct != 16) {
                    ++m.nrow; if (mx > 0 && now >= mx) ++m.nrowdone;
                    if (m.nr < kAmRow) {   // 0.38.61: the row as MissionHUDCondition shows it
                        AmRow& r = m.row[m.nr++]; r = AmRow{};
                        uint8_t sc = 0; g.read(cd + 0x31, &sc, 1);
                        r.tgt = tg; r.mx = mx; r.type = ct;
                        r.now = now < 0 ? 0 : (mx > 0 && now > mx) ? mx : now;   // UpdateCount: clamped to 0 .. MaxCount
                        r.cnt = sc || ct == 18 || ct == 405 ? 1 : 0;             // ShowCountText: IsShowCount, or Delivering
                        g.str(g.ptr(cd + 0x18), r.key, sizeof r.key);           // Bind: UIDataText.SetText(TextID)
                        const uint64_t iv = (tg > 0 && tg < 9000000 && itbl) ? FindItem(in, g, itbl, tg) : 0;
                        if (iv) g.str(g.ptr(iv + 0x38), r.icon, sizeof r.icon);
                    }
                }
            }
            if (tg >= 9000000 && tg < 10000000) m.canpin = 0;   // a character / event: nothing to mark (as m_pin)
            if (tg > 0 && !(mx > 0 && now >= mx) && m.nt < kAmConds) m.tgt[m.nt++] = tg;
        }
        if (!m.nt) m.canpin = 0;
        ++a.nm;
    }
    g_am = a;
}
bool AmHas(uint64_t md) { for (int i = 0; i < g_am_nvis; ++i) if (g_am.mis[g_am_vis[i]].md == md) return true; return false; }
// The pinned mission's open targets, when it is not one of the 3 the tracker shows (the map marks read these).
int AmPinTargets(int32_t* out, int cap) {
    if (!g_pin_md) return 0;
    for (int i = 0; i < g_am.nm; ++i) if (g_am.mis[i].md == g_pin_md) {
        int n = 0; for (int k = 0; k < g_am.mis[i].nt && n < cap; ++k) out[n++] = g_am.mis[i].tgt[k];
        return n;
    }
    return 0;
}
// The words without the game's rich-text tags ("<color=#..>Kelp</color>" -> "Kelp").
const char* AmPlain(const char* t, Buf& out) {
    out.clear();
    for (; *t; ++t) {
        if (*t == '<') { const char* e = t; while (*e && *e != '>') ++e; if (*e) { t = e; continue; } }
        out.addc(*t);
    }
    return out.s;
}
void AllMissionsPublish(const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    const AmState& a = g_am;
    Buf d, k, v, t2; char fb[128];
    d.add("other missions: list ").num(a.nlist).add(", main / side ").num(a.nm);
    g_am_nvis = 0;
    for (int i = 0; i < a.nm; ++i) {   // which are the game's active ones (see above)
        const AmMis& m = a.mis[i];
        bool tracked = false; for (int s = 0; s < kMTasks; ++s) tracked = tracked || (g_mi.md[s] && g_mi.md[s] == m.md);
        const char* t = TextFor(m.key, fb, sizeof fb);
        if (m.npcname[0]) { char f2[128]; TextFor(m.npcname, f2, sizeof f2); }   // queue the giver's name too
        const int known = TextKnown(m.key);
        Buf pl; AmPlain(t, pl);
        const bool ok = known == 1 && pl.n > 0 && pl.s[0] != ' ' && !tracked;   // 0.38.59: the list's own rule (above) + a title read
        if (d.n < 440) { d.add(" | ").num(m.tid).addc(' ').num(m.type).addc('/').num(m.state).add(m.hud ? " hud" : " -")
                          .add(known == 1 ? (t[0] == '(' ? " (..)" : " txt") : known == 0 ? " ..." : " notxt")
                          .add(tracked ? " T" : "").add(ok ? " SHOW" : ""); }
        if (ok && g_am_nvis < kAmShow) g_am_vis[g_am_nvis++] = i;
    }
    h->publish_text(u, "om.diag", d.s);
    h->publish_i64(u, "om.n", g_am_nvis);
    for (int c = 0; c < kAmShow; ++c) {
        const bool on = c < g_am_nvis;
        const AmMis& m = a.mis[on ? g_am_vis[c] : 0];
        if (on && !eq(g_am_pkey[c], m.npckey)) {   // the giver's face, resolved once per character
            DcCopy(g_am_pkey[c], sizeof g_am_pkey[c], m.npckey);
            g_am_pfile[c][0] = 0;
            if (m.npckey[0] && !NpcPicture(h, m.npcpic, m.npckey, g_am_pfile[c], sizeof g_am_pfile[c])) g_am_pfile[c][0] = 0;
            if (!IconIndexReady()) g_am_pkey[c][0] = 0;
        }
        const bool hp = on && g_am_pfile[c][0];
        k.clear(); k.add("om.on").num(c); h->publish_i64(u, k.s, on ? 1 : 0);
        { Buf pl; k.clear(); k.add("om.t").num(c); h->publish_text(u, k.s, on ? NoApos(AmPlain(TextFor(m.key, fb, sizeof fb), pl), t2) : ""); }
        v.clear();
        if (on && m.npcname[0] && TextKnown(m.npcname) == 1) v.addc('#').add(TextFor(m.npcname, fb, sizeof fb));
        k.clear(); k.add("om.tag").num(c); h->publish_text(u, k.s, NoApos(v.s, t2));
        v.clear(); if (on && m.nrow) v.num(m.state >= 3 ? m.nrow : m.nrowdone).addc('/').num(m.nrow);   // 0.38.59: rows, as the list
        k.clear(); k.add("om.pg").num(c); h->publish_text(u, k.s, v.s);
        k.clear(); k.add("om.pic").num(c);
        if (hp) { v.clear(); v.add("module:icon/").add(g_am_pfile[c]); h->publish_text(u, k.s, v.s); }
        k.clear(); k.add("om.haspic").num(c); h->publish_i64(u, k.s, hp ? 1 : 0);
        k.clear(); k.add("om.nopic").num(c); h->publish_i64(u, k.s, on && !hp ? 1 : 0);
        for (int kd = 0; kd < 3; ++kd) { k.clear(); k.add("om.k").num(kd).add("_").num(c);
            h->publish_i64(u, k.s, on && (m.type == 0 ? 0 : m.vip ? 2 : 1) == kd ? 1 : 0); }
        const bool pn = on && g_pin_md && m.md == g_pin_md;
        k.clear(); k.add("om.pon").num(c); h->publish_i64(u, k.s, pn ? 1 : 0);
        k.clear(); k.add("om.poff").num(c); h->publish_i64(u, k.s, on && !pn && m.canpin ? 1 : 0);
        k.clear(); k.add("om.nr").num(c); h->publish_i64(u, k.s, on ? m.nr : 0);
        for (int r = 0; r < kAmRow; ++r) {   // 0.38.61: the task's rows (tick box, item picture, words, count)
            const bool rv = on && r < m.nr;
            const AmRow& w = m.row[rv ? r : 0];
            const bool dn = rv && w.mx > 0 && w.now >= w.mx;
            Buf pl, t3; const char* tx = rv ? AmPlain(TextFor(w.key, fb, sizeof fb), pl) : "";
            {   // a "{0}" in the words takes the row's count (as the game's text table writes some)
                const char* q = tx; t3.clear();
                for (; *q; ++q) { if (q[0] == '{' && q[1] >= '0' && q[1] <= '9' && q[2] == '}') { t3.num(w.mx); q += 2; } else t3.addc(*q); }
            }
            const int ln = t3.n;
            k.clear(); k.add("om.rv").num(c).addc('_').num(r); h->publish_i64(u, k.s, rv ? 1 : 0);
            k.clear(); k.add("om.rt").num(c).addc('_').num(r); h->publish_text(u, k.s, NoApos(t3.s, t2));
            k.clear(); k.add("om.ro").num(c).addc('_').num(r); h->publish_i64(u, k.s, rv && !dn ? 1 : 0);
            k.clear(); k.add("om.rd").num(c).addc('_').num(r); h->publish_i64(u, k.s, dn ? 1 : 0);
            k.clear(); k.add("om.rl").num(c).addc('_').num(r); h->publish_i64(u, k.s, ln);
            v.clear(); if (rv && w.cnt) v.addc('(').num(w.now).addc('/').num(w.mx).addc(')');
            k.clear(); k.add("om.rn").num(c).addc('_').num(r); h->publish_text(u, k.s, v.s);
            k.clear(); k.add("om.rno").num(c).addc('_').num(r); h->publish_i64(u, k.s, rv && w.cnt && !dn ? 1 : 0);
            k.clear(); k.add("om.rnd").num(c).addc('_').num(r); h->publish_i64(u, k.s, rv && w.cnt && dn ? 1 : 0);
            const int ci2 = c * kAmRow + r;
            if (!eq(g_om_icn[ci2], rv ? w.icon : "") || !g_om_icknown[ci2]) {
                char f[96];
                DcCopy(g_om_icn[ci2], sizeof g_om_icn[ci2], rv ? w.icon : "");
                g_om_ichas[ci2] = g_om_icn[ci2][0] && PictureName(h, g_om_icn[ci2], f, sizeof f);
                g_om_icknown[ci2] = IconIndexReady();
            }
            k.clear(); k.add("om.ri").num(c).addc('_').num(r);
            if (g_om_ichas[ci2]) { v.clear(); v.add("module:icon/").add(g_om_icn[ci2]); h->publish_text(u, k.s, v.s); }
            k.clear(); k.add("om.rhi").num(c).addc('_').num(r); h->publish_i64(u, k.s, rv && g_om_ichas[ci2] ? 1 : 0);
        }
    }
}
// ---------------------------------------------------------------- 0.37.89: Weapon Shop (rounds 50..52)
// The list as CraftPlanScrollPanel.AddCellData builds it: SaveData.m_GunCraftSaveData (Dictionary<int, GunCraftInfoSave>
// @0xF8) in order, kept when the craft is a root one (GetRootGunCraftEnumerator's filter: Craft.InCraftList @0x40 empty).
// GunCraftInfoSave: CraftID ObscuredInt @0x10, DevelopedCount @0x24, GunCraftState @0x4C (0 undeveloped, 1 developed,
// 2 used), UIState @0x54 (2 new, 3 newly developed, 4 craftable). Craft (DataManager.CraftDataDic @0xD8): Price @0x20,
// RequireItemIDs @0x28 / Counts @0x30 (List<int>), ProductItemID @0x38 -> the item (name, picture). Develop: Blueprint
// (BlueprintDataDic @0xD0) whose ProductCraftID @0x34 is the craft, MaxDevelopProgressCount @0x30.
constexpr int kShopRows = 24, kShopMats = 4;
struct ShopRow { uint64_t craft; int32_t tid, item, state, ui, dev, devmax, price, nmat, tracked; int32_t mid[kShopMats], mneed[kShopMats], mhave[kShopMats];
                 char key[64], icon[64], mkey[kShopMats][48], micon[kShopMats][64]; };
struct ShopState { ShopRow row[kShopRows]; int n, sel, tab, gold; char diag[200]; };
ShopState g_shop;
int g_shop_confirm; uint64_t g_shop_confirm_at;   // 0.37.91: the game's craft popup asked (CONFIRM / CANCEL shown)
int32_t ListInt(const Guest& g, uint64_t list, int i) {
    const uint64_t a = list ? g.ptr(list + 0x10) : 0;
    return a ? g.s32(a + 0x20 + 4 * static_cast<uint64_t>(i)) : 0;
}
// 0.37.91: tracked weapons (ZL "Tracking"): SaveData +0xD0 (SaveDataExploration) +0x20 HashSet<int> (IsExplorationCraft).
// HashSet<int> (.NET): _entries @0x18 (Entry {int hashCode, int next, int value}, 12 bytes); a live entry has next >= -1.
char g_trk_diag[240];   // 0.37.92: the tracked set as read (DIAG "shop" line) -- 0.37.91 never showed TRACKED
void ShopTrackedDiag(const Instance& in, const Guest& g) {
    Buf d; d.add("trk");
    const uint64_t ex = in.gs ? g.ptr(in.gs + 0xD0) : 0, hs = ex ? g.ptr(ex + 0x20) : 0;
    char c1[40] = "?", c2[40] = "?";
    if (ex) g.cls(ex, c1, sizeof c1);
    if (hs) g.cls(hs, c2, sizeof c2);
    d.add(" ex ").add(c1).add(" hs ").add(c2).add(" |");
    for (uint64_t o = 0x10; hs && o <= 0x3C; o += 4) d.addc(' ').num(g.s32(hs + o));
    for (uint64_t o = 0x10; hs && o <= 0x20; o += 8) {
        const uint64_t a = g.ptr(hs + o);
        char ac[24] = "-"; if (a) g.cls(a, ac, sizeof ac);
        d.add(" | @").hex(o).addc(' ').add(ac).add(" n").num(a ? g.s32(a + 0x18) : -1).addc(':');
        for (int i = 0; a && i < 12; ++i) d.addc(' ').num(g.s32(a + 0x20 + 4 * static_cast<uint64_t>(i)));
    }
    DcCopy(g_trk_diag, sizeof g_trk_diag, d.s);
}
bool IntSetHas(const Guest& g, uint64_t hs, int32_t id);
bool ShopTracked(const Instance& in, const Guest& g, int32_t id) {
    const uint64_t ex = in.gs ? g.ptr(in.gs + 0xD0) : 0, hs = ex ? g.ptr(ex + 0x20) : 0;
    return IntSetHas(g, hs, id);
}
bool IntSetHas(const Guest& g, uint64_t hs, int32_t id) {   // 0.37.103: any HashSet<int> (was ShopTracked's own)
    // 0.37.92: slot layouts tried: {hash, next, value} and {hash, value, next}; the slot array at +0x18 (or +0x20);
    // only the first `count` slots (count: the int after the arrays) and, when it can be told, not freed ones
    for (uint64_t ao = 0x18; hs && ao <= 0x20; ao += 8) {
        const uint64_t en = g.ptr(hs + ao);
        const int32_t n = en ? g.s32(en + 0x18) : 0;
        if (!en || n <= 0 || n > 4096) continue;
        for (int32_t i = 0; i < n && i < 512; ++i) {
            const uint64_t e = en + 0x20 + 12 * static_cast<uint64_t>(i);
            const int32_t a = g.s32(e), b = g.s32(e + 4), c = g.s32(e + 8);
            if (a == id && c == id && b >= -1) return true;          // {hash, next, value}
            if (a == id && b == id && c >= -1) return true;          // {hash, value, next}
        }
    }
    return false;
}
void ShopStep(Instance& in, const Guest& g) {
    ShopState& sh = g_shop;
    Buf d;
    sh.n = 0; sh.sel = -1; sh.tab = -1;
    sh.gold = -1;
    { const uint64_t pi = in.gs ? g.ptr(in.gs + 0x240) : 0; int32_t gv = 0; if (pi && g.obscured(pi + 0x10, &gv)) sh.gold = gv; }
    const uint64_t gc = in.gs ? g.ptr(in.gs + 0xF8) : 0;
    const uint64_t dm = DataManager(in, g);
    const uint64_t cd = dm ? g.ptr(dm + 0xD8) : 0, bd = dm ? g.ptr(dm + 0xD0) : 0;
    const uint64_t items = ItemTable(in, g);
    const uint64_t en = gc ? g.ptr(gc + 0x18) : 0; const int32_t cnt = gc ? g.s32(gc + 0x20) : 0;
    if (!en || cnt <= 0 || cnt > 4096 || !cd) { d.add("shop: no craft save / table"); DcCopy(sh.diag, sizeof sh.diag, d.s); return; }
    int roots = 0;
    for (int32_t i = 0; i < cnt && sh.n < kShopRows; ++i) {
        const uint64_t e = en + 0x20 + static_cast<uint64_t>(i) * 0x18, v = g.ptr(e + 0x10);
        if (g.s32(e) < 0 || !v) continue;
        int32_t cid = 0; if (!g.obscured(v + 0x10, &cid) || cid <= 0) continue;
        const uint64_t c = DictFindInt(g, cd, cid);
        if (!c) continue;
        const uint64_t inl = g.ptr(c + 0x40);
        if (inl && g.s32(inl + 0x18) > 0) continue;                  // not a root craft
        ++roots;
        ShopRow& r = sh.row[sh.n++]; r = ShopRow{};
        r.craft = c; r.tid = cid; r.item = g.s32(c + 0x38); r.price = g.s32(c + 0x20);
        r.state = g.s32(v + 0x4C); r.ui = g.s32(v + 0x54);
        if (!g.obscured(v + 0x24, &r.dev)) r.dev = 0;
        const uint64_t iv = items && r.item > 0 ? DictFindInt(g, items, r.item) : 0;
        if (iv) { g.str(g.ptr(iv + 0x18), r.key, sizeof r.key); g.str(g.ptr(iv + 0x38), r.icon, sizeof r.icon); }
        {   // the blueprint (develop progress)
            const uint64_t be = bd ? g.ptr(bd + 0x18) : 0; const int32_t bn = bd ? g.s32(bd + 0x20) : 0;
            for (int32_t k = 0; be && k < bn && k < 2048; ++k) {
                const uint64_t b = g.ptr(be + 0x20 + static_cast<uint64_t>(k) * 0x18 + 0x10);
                if (b && g.s32(be + 0x20 + static_cast<uint64_t>(k) * 0x18) >= 0 && g.s32(b + 0x34) == cid) {
                    r.devmax = g.s32(b + 0x30);
                    // 0.37.93: undeveloped -> the blueprint drawing, as the game's card (BlueprintIcon @0x20, else Thumb @0x28)
                    if (!r.icon[0] || r.state == 0) {
                        char bi[64] = {0};
                        if (!(g.str(g.ptr(b + 0x20), bi, sizeof bi) && bi[0])) g.str(g.ptr(b + 0x28), bi, sizeof bi);
                        if (bi[0]) DcCopy(r.icon, sizeof r.icon, bi);
                    }
                    break;
                }
            }
        }
        r.tracked = ShopTracked(in, g, cid) ? 1 : 0;
        const uint64_t il = g.ptr(c + 0x28), nl = g.ptr(c + 0x30);
        const int32_t m = il ? g.s32(il + 0x18) : 0;
        for (int32_t k = 0; k < m && k < kShopMats; ++k) {
            const int q = r.nmat++;
            r.mid[q] = ListInt(g, il, k); r.mneed[q] = ListInt(g, nl, k);
            r.mhave[q] = InventoryCount(in, g, r.mid[q]);
            const uint64_t mv = items && r.mid[q] > 0 ? DictFindInt(g, items, r.mid[q]) : 0;
            if (mv) { g.str(g.ptr(mv + 0x18), r.mkey[q], sizeof r.mkey[q]); g.str(g.ptr(mv + 0x38), r.micon[q], sizeof r.micon[q]); }
        }
    }
    // the cursor, when the Weapon Shop is open on the top screen
    const uint64_t dpc = DavePhone(in, g);
    if (dpc && PhoneState(g, dpc) == 2 && g.s32(dpc + 0xA8) == kAppDuffShop) {
        const uint64_t wp = PhoneApp(g, dpc, kAppDuffShop);
        const uint64_t sc = wp ? g.ptr(wp + 0x70) : 0;
        sh.tab = wp ? g.s32(wp + 0x78) : -1;
        const uint64_t list = sc ? g.ptr(sc + 0x88) : 0, li = list ? g.ptr(list + 0x10) : 0;
        const int32_t f = sc ? g.s32(sc + 0x90) : -1;
        const uint64_t cell = li && f >= 0 ? g.ptr(li + 0x20 + 8 * static_cast<uint64_t>(f)) : 0;
        const uint64_t fc = cell ? g.ptr(cell + 0x18) : 0;
        for (int i = 0; i < sh.n && fc; ++i) if (sh.row[i].craft == fc) sh.sel = i;
    }
    ShopTrackedDiag(in, g);
    d.add("shop: saved ").num(cnt).add(" root ").num(roots).add(" shown ").num(sh.n).add(" tab ").num(sh.tab).add(" sel ").num(sh.sel);
    DcCopy(sh.diag, sizeof sh.diag, d.s);
}
bool PjRunning();
void ShopPublish(const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    const ShopState& sh = g_shop;
    h->publish_text(u, "shop.diag", sh.diag);
    h->publish_text(u, "shop.trkdiag", g_trk_diag);
    h->publish_i64(u, "shop.ok", sh.n > 0 ? 1 : 0);
    h->publish_i64(u, "shop.rows", sh.n);
    if (g_shop_confirm && (g_ticks > g_shop_confirm_at + 60 * 30 || g_mode == 1)) g_shop_confirm = 0;
    h->publish_i64(u, "shop.confirm", g_shop_confirm && !PjRunning() ? 1 : 0);
    const int32_t gold = sh.gold;
    Buf k, v, t2; char fb[128];
    for (int i = 0; i < kShopRows; ++i) {
        const ShopRow& r = sh.row[i];
        const bool on = i < sh.n;
        k.clear(); k.add("shop.on").num(i); h->publish_i64(u, k.s, on ? 1 : 0);
        k.clear(); k.add("shop.t").num(i); h->publish_text(u, k.s, on ? NoApos(TextFor(r.key, fb, sizeof fb), t2) : "");
        v.clear(); if (on && r.icon[0]) v.add("module:icon/").add(r.icon);
        k.clear(); k.add("shop.ic").num(i); h->publish_text(u, k.s, v.s);
        k.clear(); k.add("shop.cur").num(i); h->publish_i64(u, k.s, on && i == sh.sel ? 1 : 0);
        k.clear(); k.add("shop.price").num(i); h->publish_i64(u, k.s, on ? r.price : 0);
        // 0 undeveloped (develop %), 1 craftable, 2 new, 3 ready (developed)
        const int badge = !on ? -1 : r.state == 0 ? 0 : r.ui == 4 ? 1 : r.ui == 2 ? 2 : 3;
        k.clear(); k.add("shop.st").num(i); h->publish_i64(u, k.s, badge);
        const int pr = r.devmax > 0 ? (r.dev * 200 + r.devmax) / (2 * r.devmax) : 0;   // 0.37.91: rounded as the game (2/3 = 67%)
        const int pct = pr > 100 ? 100 : pr;
        bool mats = true; for (int q = 0; q < r.nmat; ++q) if (r.mhave[q] < r.mneed[q]) mats = false;
        const bool can = on && r.state != 0 && mats && gold >= r.price;
        k.clear(); k.add("shop.can").num(i); h->publish_i64(u, k.s, can ? 1 : 0);
        k.clear(); k.add("shop.short").num(i); h->publish_i64(u, k.s, on && r.state != 0 && !can ? 1 : 0);
        k.clear(); k.add("shop.trk").num(i); h->publish_i64(u, k.s, on && r.tracked ? 1 : 0);
        k.clear(); k.add("shop.pct").num(i); h->publish_i64(u, k.s, on ? pct : 0);
        for (int q = 0; q < kShopMats; ++q) {
            const bool mon = on && q < r.nmat;
            v.clear(); if (mon) v.num(r.mhave[q]).addc('/').num(r.mneed[q]);
            k.clear(); k.add("shop.mc").num(i).addc('_').num(q); h->publish_text(u, k.s, v.s);
            v.clear(); if (mon && r.micon[q][0]) v.add("module:icon/").add(r.micon[q]);
            k.clear(); k.add("shop.mi").num(i).addc('_').num(q); h->publish_text(u, k.s, v.s);
            k.clear(); k.add("shop.mon").num(i).addc('_').num(q); h->publish_i64(u, k.s, mon ? 1 : 0);
            k.clear(); k.add("shop.mok").num(i).addc('_').num(q); h->publish_i64(u, k.s, mon && r.mhave[q] >= r.mneed[q] ? 1 : 0);
        }
    }
}
// ---------------------------------------------------------------- 0.37.102: iDiver (rounds 53, 54)
// The list as LobbyEquipUpgradeScrollPanel.AddCellData builds it: one row per SubEquipmentType (o2_tank 1, diving_suit 2,
// inventory 3, Harpoon 4, drone 5, basic_knife 6, crab_trap 7; jungle_diving_suit 8 only in the jungle), its row data
// ScrollerCellData.subEquiptype @0x14. What a row shows (SubEquipmentManager.SetSubEquipUIInfo): the equipped piece of
// that type (SubEquipmentManager.m_EquipSubEquips Dictionary<type, id> @0x18) = SubEquipment (DataManager @0xE0): name
// NameTextID @0x18, picture UIIcon @0x28, level @0x34; the next one = id + 1 (none: max level); price = (next, else the
// current).Price[0] (@0x38); UnlockMission = next's @0x40. State (CalcUpgradeState): max -> MaxLevel; a mission not done
// -> Lock; price > gold -> NoMoney; else Enable. 0.37.103 (round 55): mission done = in MissionManager.m_ClearList
// (HashSet<int> @0x70), IsDoneMission's first test (its later ones -- tasks done, scene -- are not followed).
// Harpoon / knife: the level is a different weapon (EquipmentItemID @0x54): its name, now > next.
// The app open on top: IDiverPanel (14060003) m_UpgradeScroll @0x30: m_ScrollCellDataList @0x88, focusIndex @0x90,
// m_UIState @0x114 (0 list, 1 detail -- its A buys the level --, 2 level-up screen).
constexpr int32_t kAppIDiver = 14060003;
constexpr uint64_t kSubEquipMgrSlot = 0x9E0E4E8;   // Method$SingletonNoMono<SubEquipmentManager>.get_Instance()
constexpr int kIdvRows = 10;
struct IdvRow { int32_t type, id, lv, price, mission, max, nexists, lock, nlv, ui_stat; float cur, nxt; char key[64], icon[64], ckey[64], nkey[64]; };
struct IdvState { IdvRow row[kIdvRows]; int n, sel, ui, gold, from; int32_t order[kIdvRows]; int norder; char diag[240]; };
IdvState g_idv;
int g_idv_confirm; uint64_t g_idv_confirm_at;
uint64_t SubEquipManager(const Instance& in, const Guest& g) {
    const uint64_t mi = g.ptr(in.main_base + kSubEquipMgrSlot);
    if (!mi || (mi & 1)) return 0;
    char c[40];
    for (uint64_t koff = 0x18; koff <= 0x20; koff += 8) {
        const uint64_t k = g.ptr(mi + koff);
        if (!k || !g.cstr(g.ptr(k + 0x10), c, sizeof c) || !eq(c, "SingletonNoMono`1")) continue;
        const uint64_t st = g.ptr(k + kStatics);
        for (int j = 0; st && j < 8; ++j) {
            const uint64_t o = g.ptr(st + 8 * static_cast<uint64_t>(j));
            if (o && g.cls(o, c, sizeof c) && eq(c, "SubEquipmentManager")) return o;
        }
    }
    return 0;
}
// the equipped id of each type: the manager's Dictionary<SubEquipmentType, int> (16-byte entries {hash, next, key, value})
int IdvEquipped(const Guest& g, uint64_t mgr, int32_t* ids /* [10] by type */) {
    const uint64_t d = mgr ? g.ptr(mgr + 0x18) : 0, en = d ? g.ptr(d + 0x18) : 0;
    const int32_t cnt = d ? g.s32(d + 0x20) : 0;
    int n = 0;
    for (int32_t i = 0; en && i < cnt && i < 64; ++i) {
        const uint64_t e = en + 0x20 + 16 * static_cast<uint64_t>(i);
        const int32_t h = g.s32(e), k = g.s32(e + 8), v = g.s32(e + 12);
        if (h >= 0 && k > 0 && k < kIdvRows && v > 0) { ids[k] = v; ++n; }
    }
    return n;
}
float IdvStat(const Guest& g, uint64_t se, int32_t type) {   // the number the level raises (as the detail panel lists it)
    if (!se) return 0;
    float f = 0;
    switch (type) {
        case 1: g.read(se + 0x44, &f, 4); return f;                    // O2 tank: MaxHP
        case 2: case 8: g.read(se + 0x48, &f, 4); return f;            // diving suit: MaxDepthWater
        case 3: g.read(se + 0x4C, &f, 4); return f;                    // cargo: LootboxWeight
        case 5: return static_cast<float>(g.s32(se + 0x58));          // drone: DroneCount
        case 7: return static_cast<float>(g.s32(se + 0x5C));          // crab trap: TrapCount
        default: return 0;
    }
}
void IdvStep(Instance& in, const Guest& g) {
    IdvState& v = g_idv;
    Buf d;
    v.n = 0; v.sel = -1; v.ui = -1; v.gold = -1;
    { const uint64_t pi = in.gs ? g.ptr(in.gs + 0x240) : 0; int32_t gv = 0; if (pi && g.obscured(pi + 0x10, &gv)) v.gold = gv; }
    const uint64_t dm = DataManager(in, g);
    const uint64_t se = dm ? g.ptr(dm + 0xE0) : 0;
    const uint64_t mgr = SubEquipManager(in, g);
    const uint64_t mm = SingletonObj(in, g, kMissionManagerTI, "MissionManager");
    const uint64_t clear = mm ? g.ptr(mm + 0x70) : 0;
    const uint64_t items = ItemTable(in, g);
    int32_t ids[kIdvRows] = {0};
    const int ne = IdvEquipped(g, mgr, ids);
    // the order: the game's own list when iDiver is open on top (kept for later), else by type (no jungle suit)
    const uint64_t dpc = DavePhone(in, g);
    uint64_t sc = 0;
    if (dpc && PhoneState(g, dpc) == 2 && g.s32(dpc + 0xA8) == kAppIDiver) {
        const uint64_t ip = PhoneApp(g, dpc, kAppIDiver);
        char ic[24];
        if (ip && g.cls(ip, ic, sizeof ic) && eq(ic, "IDiverPanel")) sc = g.ptr(ip + 0x30);
    }
    if (sc) {
        const uint64_t list = g.ptr(sc + 0x88), li = list ? g.ptr(list + 0x10) : 0;
        const int32_t n = list ? g.s32(list + 0x18) : 0;
        if (li && n > 0 && n <= kIdvRows) {
            v.norder = 0;
            for (int32_t i = 0; i < n; ++i) { const uint64_t c = g.ptr(li + 0x20 + 8 * static_cast<uint64_t>(i)); v.order[v.norder++] = c ? g.s32(c + 0x14) : 0; }
            v.from = 1;
            const int32_t f = g.s32(sc + 0x90);
            v.ui = g.s32(sc + 0x114);
            if (f >= 0 && f < n) v.sel = f;
        }
    }
    if (!v.norder) { for (int32_t t = 1; t <= 7; ++t) if (ids[t]) v.order[v.norder++] = t; v.from = 0; }
    Buf ic;
    for (int i = 0; i < v.norder && v.n < kIdvRows; ++i) {
        const int32_t t = v.order[i];
        IdvRow& r = v.row[v.n]; r = IdvRow{}; r.type = t; r.id = t > 0 && t < kIdvRows ? ids[t] : 0;
        const uint64_t cur = se && r.id ? DictFindInt(g, se, r.id) : 0;
        const uint64_t nxt = se && r.id ? DictFindInt(g, se, r.id + 1) : 0;
        if (cur) {
            g.str(g.ptr(cur + 0x18), r.key, sizeof r.key); g.str(g.ptr(cur + 0x28), r.icon, sizeof r.icon);
            r.lv = g.s32(cur + 0x34);
            r.price = ListInt(g, g.ptr((nxt ? nxt : cur) + 0x38), 0);
            r.mission = nxt ? g.s32(nxt + 0x40) : 0;
            r.max = nxt ? 0 : 1; r.nexists = nxt ? 1 : 0;
            r.cur = IdvStat(g, cur, t); r.nxt = IdvStat(g, nxt, t);
            r.nlv = nxt ? g.s32(nxt + 0x34) : 0;
            // 0.37.108: the game's own numbers for the row (SubEquipmentManager.m_SubEquipUIInfo @0x20, Dictionary<type,
            // SubEquipmentUIInfo>; subEquipStatus @0x34 {statusName, current @0x38, next @0x3C, currentLevel @0x40,
            // nextLevel @0x44 -- floats}; subEquipID @0x20, subEquipLevel @0x24): the harpoon's damage (no number in its
            // SubEquipment) and the level as the game shows it
            const uint64_t uid = mgr ? g.ptr(mgr + 0x20) : 0, ui = uid ? DictFindInt(g, uid, t) : 0;
            if (ui) {
                float sc = 0, sn = 0;
                g.read(ui + 0x38, &sc, 4); g.read(ui + 0x3C, &sn, 4);
                if (sc > 0 && sc < 100000) { r.cur = sc; r.nxt = sn; r.ui_stat = 1; }
                // subEquipLevel @0x24: the level as the game shows it (SetSubEquipUIInfo takes the suit's offset off)
                const int32_t lv = g.s32(ui + 0x24);
                if (lv >= 1 && lv < 100 && g.s32(ui + 0x20) == r.id) { r.lv = lv; if (nxt) r.nlv = lv + 1; }
            }
            r.lock = r.mission && !(clear && IntSetHas(g, clear, r.mission)) ? 1 : 0;
            if (t == 4 || t == 6) {                                     // a weapon: its item's name, now and next
                const int32_t ci = g.s32(cur + 0x54), ni = nxt ? g.s32(nxt + 0x54) : 0;
                const uint64_t cv = items && ci > 0 ? DictFindInt(g, items, ci) : 0, nv = items && ni > 0 ? DictFindInt(g, items, ni) : 0;
                if (cv) g.str(g.ptr(cv + 0x18), r.ckey, sizeof r.ckey);
                if (nv) g.str(g.ptr(nv + 0x18), r.nkey, sizeof r.nkey);
            }
        }
        if (ic.n < 170) ic.addc(' ').num(t).addc(':').num(static_cast<int64_t>(r.cur)).addc('>').num(static_cast<int64_t>(r.nxt)).add(r.ui_stat ? "u" : "").add(r.lock ? "L" : "");
        ++v.n;
    }
    d.add("idv: mgr ").add(mgr ? "ok" : "none").add(" eq ").num(ne).add(" rows ").num(v.n).add(v.from ? " (open)" : "")
     .add(" sel ").num(v.sel).add(" ui ").num(v.ui).add(" clr ").add(clear ? "ok" : "-").add(" |").add(ic.s);
    DcCopy(v.diag, sizeof v.diag, d.s);
}
void IdvPublish(const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    const IdvState& v = g_idv;
    h->publish_text(u, "idv.diag", v.diag);
    h->publish_i64(u, "idv.ok", v.n > 0 ? 1 : 0);
    h->publish_i64(u, "idv.rows", v.n);
    if (g_idv_confirm && (g_ticks > g_idv_confirm_at + 60 * 30 || g_mode == 1)) g_idv_confirm = 0;
    // 0.37.105: from the game's own screen when iDiver is open: its detail up -> CONFIRM / CANCEL, the level-up -> OK
    h->publish_i64(u, "idv.confirm", !PjRunning() && (v.ui == 1 || (v.ui < 0 && g_idv_confirm)) ? 1 : 0);
    h->publish_i64(u, "idv.lvup", !PjRunning() && v.ui == 2 ? 1 : 0);
    Buf k, t, t2; char fb[128];
    for (int i = 0; i < kIdvRows; ++i) {
        const IdvRow& r = v.row[i];
        const bool on = i < v.n && r.id;
        k.clear(); k.add("idv.on").num(i); h->publish_i64(u, k.s, on ? 1 : 0);
        k.clear(); k.add("idv.t").num(i); h->publish_text(u, k.s, on ? NoApos(TextFor(r.key, fb, sizeof fb), t2) : "");
        t.clear(); if (on && r.icon[0]) t.add("module:icon/").add(r.icon);
        k.clear(); k.add("idv.ic").num(i); h->publish_text(u, k.s, t.s);
        k.clear(); k.add("idv.lv").num(i); h->publish_i64(u, k.s, on ? r.lv : 0);
        k.clear(); k.add("idv.price").num(i); h->publish_i64(u, k.s, on ? r.price : 0);
        {   // 0.37.106: as the game writes it (1,500)
            char dg[16]; int nd = 0; int32_t pv = on && r.price > 0 ? r.price : 0;
            do { dg[nd++] = static_cast<char>('0' + pv % 10); pv /= 10; } while (pv && nd < 12);
            t.clear(); for (int q = nd - 1; q >= 0; --q) { t.addc(dg[q]); if (q && q % 3 == 0) t.addc(','); }
            k.clear(); k.add("idv.ptxt").num(i); h->publish_text(u, k.s, on ? t.s : "");
        }
        k.clear(); k.add("idv.cur").num(i); h->publish_i64(u, k.s, on && i == v.sel ? 1 : 0);
        // 0 can upgrade, 1 not enough gold, 2 max level, 3 locked (a mission first) -- CalcUpgradeState's order
        const int st = !on ? -1 : r.max ? 2 : r.lock ? 3 : (v.gold >= 0 && r.price > v.gold) ? 1 : 0;
        k.clear(); k.add("idv.st").num(i); h->publish_i64(u, k.s, st);
        k.clear(); k.add("idv.nlv").num(i); h->publish_i64(u, k.s, on ? r.nlv : 0);
        // 0.37.103: the raised number now / next (a weapon: its name now / next)
        auto f1 = [&](float x) { const int64_t q = static_cast<int64_t>(x * 10 + (x < 0 ? -0.5f : 0.5f)); t.num(q / 10); if (q % 10) t.addc('.').num(q % 10 < 0 ? -(q % 10) : q % 10); };
        t.clear();
        if (on && r.ckey[0] && !r.ui_stat) t.add(NoApos(TextFor(r.ckey, fb, sizeof fb), t2)); else if (on && (r.cur != 0 || r.nxt != 0)) f1(r.cur);
        k.clear(); k.add("idv.scur").num(i); h->publish_text(u, k.s, t.s);
        t.clear();
        if (on && !r.max && r.nkey[0] && !r.ui_stat) t.add(NoApos(TextFor(r.nkey, fb, sizeof fb), t2)); else if (on && !r.max && (r.cur != 0 || r.nxt != 0)) f1(r.nxt);
        k.clear(); k.add("idv.snxt").num(i); h->publish_text(u, k.s, t.s);
    }
}
// ---------------------------------------------------------------- 0.37.109: Call (Contacts, round 56)
// ContactsPanel (14060001) ChildPanels @0x30 (List<ContactsChildPanel>): ContactsChildPanelList -> Scroller @0x28
// (ContactsListScroller, a BaseScrollController: m_ScrollCellDataList @0x88, focusIndex @0x90) -> its rows
// ContactsListScroller.Data {NpcID @0x10, ProfileResource @0x18, Name @0x20, IsNew @0x28}. The list is built when the
// app is shown, so it is read from there (kept when the phone closes). List input: X (action 4) calls the focused
// contact (OnCall), B closes; while calling, B cancels.
constexpr int32_t kAppContacts = 14060001;
constexpr int kCallRows = 24;
struct CallRow { int32_t npc, isnew; char prof[64], name[64]; };
struct CallState { CallRow row[kCallRows]; int n, sel, live; char diag[250]; };
CallState g_call;
uint64_t CallScroller(const Guest& g, uint64_t dpc) {
    const uint64_t cp = PhoneApp(g, dpc, kAppContacts);
    char c[32];
    if (!cp || !g.cls(cp, c, sizeof c) || !eq(c, "ContactsPanel")) return 0;
    const uint64_t list = g.ptr(cp + 0x30), items = list ? g.ptr(list + 0x10) : 0;
    const int32_t n = list ? g.s32(list + 0x18) : 0;
    for (int32_t i = 0; items && i < n && i < 8; ++i) {
        const uint64_t ch = g.ptr(items + 0x20 + 8 * static_cast<uint64_t>(i));
        if (ch && g.cls(ch, c, sizeof c) && eq(c, "ContactsChildPanelList")) return g.ptr(ch + 0x28);
    }
    return 0;
}
// 0.37.110 (round 57): the list from the save, as ContactsChildPanelList.Show builds it -- DataManager.NPCNormalDataDic
// (@0x258, in its order), kept by ContactAdditionType @0x58: 1 always, 2 mission accepted, 3 mission cleared (value
// @0x5C), 4 contents unlocked; DLCType @0x68 must be 0 (owned DLC is not read: those wait for the app's own list).
// Mission cleared = MissionManager.m_ClearList (@0x70); accepted = cleared or in m_MissionList (@0x68). Contents
// unlock (4) is not read yet: kept. Portrait ThumbnailHUD @0x40, name NameTextID @0x28; NEW = not in
// SaveData.m_ContactsSaveData (@0x80) .Checked (@0x18). The app's own list wins once it has been read this session.
int g_call_live_seen;
int CallFromSave(Instance& in, const Guest& g, CallRow* out, int max, Buf& d) {
    const uint64_t dm = DataManager(in, g);
    const uint64_t nd = dm ? g.ptr(dm + 0x258) : 0, en = nd ? g.ptr(nd + 0x18) : 0;
    const int32_t cnt = nd ? g.s32(nd + 0x20) : 0;
    if (!en || cnt <= 0 || cnt > 4096) { d.add("no NPC table"); return 0; }
    const uint64_t mm = SingletonObj(in, g, kMissionManagerTI, "MissionManager");
    const uint64_t clear = mm ? g.ptr(mm + 0x70) : 0, ml = mm ? g.ptr(mm + 0x68) : 0;
    const uint64_t cs = in.gs ? g.ptr(in.gs + 0x80) : 0, checked = cs ? g.ptr(cs + 0x18) : 0;
    int n = 0, skip4 = 0, dlc = 0;
    for (int32_t i = 0; i < cnt && n < max; ++i) {
        const uint64_t e = en + 0x20 + static_cast<uint64_t>(i) * 0x18, v = g.ptr(e + 0x10);
        if (g.s32(e) < 0 || !v) continue;
        const int32_t type = g.s32(v + 0x58), val = g.s32(v + 0x5C);
        if (g.s32(v + 0x68) != 0) { ++dlc; continue; }
        bool ok = false;
        if (type == 1) ok = true;
        else if (type == 3) ok = clear && IntSetHas(g, clear, val);
        else if (type == 2) ok = (clear && IntSetHas(g, clear, val)) || (ml && DictFindInt(g, ml, val) != 0);
        else if (type == 4) { ok = true; ++skip4; }
        if (!ok) continue;
        CallRow& r = out[n++]; r = CallRow{};
        r.npc = g.s32(v + 0x10);
        g.str(g.ptr(v + 0x40), r.prof, sizeof r.prof); g.str(g.ptr(v + 0x28), r.name, sizeof r.name);
        r.isnew = checked && !IntSetHas(g, checked, r.npc) ? 1 : 0;
    }
    d.add("save ").num(n).add(" (dlc ").num(dlc).add(", unlock kept ").num(skip4).addc(')');
    return n;
}
void CallStep(Instance& in, const Guest& g) {
    CallState& v = g_call;
    Buf d;
    v.sel = -1; v.live = 0;
    const uint64_t dpc = DavePhone(in, g);
    const uint64_t sc = dpc ? CallScroller(g, dpc) : 0;
    const uint64_t list = sc ? g.ptr(sc + 0x88) : 0, li = list ? g.ptr(list + 0x10) : 0;
    const int32_t n = list ? g.s32(list + 0x18) : 0;
    static CallRow saved[kCallRows]; Buf sd;
    const int ns = CallFromSave(in, g, saved, kCallRows, sd);
    if (li && n > 0 && n <= 256) {                       // keep the last list read (the phone may close)
        g_call_live_seen = 1;
        v.n = 0;
        for (int32_t i = 0; i < n && v.n < kCallRows; ++i) {
            const uint64_t c = g.ptr(li + 0x20 + 8 * static_cast<uint64_t>(i));
            if (!c) continue;
            CallRow& r = v.row[v.n++]; r = CallRow{};
            r.npc = g.s32(c + 0x10); { uint8_t nb = 0; g.read(c + 0x28, &nb, 1); r.isnew = nb ? 1 : 0; }
            g.str(g.ptr(c + 0x18), r.prof, sizeof r.prof); g.str(g.ptr(c + 0x20), r.name, sizeof r.name);
        }
        if (dpc && PhoneState(g, dpc) == 2 && g.s32(dpc + 0xA8) == kAppContacts) { v.live = 1; const int32_t f = g.s32(sc + 0x90); if (f >= 0 && f < v.n) v.sel = f; }
    }
    if (!g_call_live_seen && ns > 0) { v.n = ns; for (int i = 0; i < ns; ++i) v.row[i] = saved[i]; }
    // the save's list against the app's (to check the rules): ids only in one of them
    d.add("call: ").add(sc ? "app list" : "no app list").add(" rows ").num(v.n).add(" sel ").num(v.sel).add(" | ").add(sd.s);
    if (g_call_live_seen) {
        d.add(" | only app:");
        for (int i = 0; i < v.n && d.n < 170; ++i) { bool f = false; for (int k = 0; k < ns; ++k) if (saved[k].npc == v.row[i].npc) f = true; if (!f) d.addc(' ').num(v.row[i].npc); }
        d.add(" | only save:");
        for (int k = 0; k < ns && d.n < 195; ++k) { bool f = false; for (int i = 0; i < v.n; ++i) if (saved[k].npc == v.row[i].npc) f = true; if (!f) d.addc(' ').num(saved[k].npc); }
    }
    DcCopy(v.diag, sizeof v.diag, d.s);
}
void CallPublish(const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    const CallState& v = g_call;
    h->publish_text(u, "call.diag", v.diag);
    h->publish_i64(u, "call.ok", v.n > 0 ? 1 : 0);
    h->publish_i64(u, "call.rows", v.n);
    Buf k, t, t2; char fb[128];
    for (int i = 0; i < kCallRows; ++i) {
        const CallRow& r = v.row[i];
        const bool on = i < v.n;
        k.clear(); k.add("call.on").num(i); h->publish_i64(u, k.s, on ? 1 : 0);
        k.clear(); k.add("call.t").num(i); h->publish_text(u, k.s, on ? NoApos(TextFor(r.name, fb, sizeof fb), t2) : "");
        t.clear(); if (on && r.prof[0]) t.add("module:icon/").add(r.prof);
        k.clear(); k.add("call.ic").num(i); h->publish_text(u, k.s, t.s);
        k.clear(); k.add("call.cur").num(i); h->publish_i64(u, k.s, on && i == v.sel ? 1 : 0);
        k.clear(); k.add("call.new").num(i); h->publish_i64(u, k.s, on && r.isnew ? 1 : 0);
    }
}
// ---------------------------------------------------------------- 0.37.68: open a mail on the top screen (round 43)
// Driven with the game's own buttons, one checked step at a time, Dave on the boat or at the sushi bar:
// the phone closed -> + ; its home grid -> D-pad to Mail, A ; another app open -> B ; Mail open -> Up / Down to the
// tapped row (MailPanel.m_SelectedIndex), A. The phone's state: NowState (ReactiveProperty<State> @0xB0, value
// @+0x10: 0 Close, 1 Open, 2 Execute); the open app m_ExecuteApp @0xA8 counts only in Execute (IsOpenedApp).
// The home grid: phoneAppController @0x48 (GridScrollController): data List @0x88 (PhoneAppGridRowCellView.Data,
// AppID @0x10), focusIndex @0x90, maxColumnCount @0xDC.
enum PjBtn { kPbNone = 0, kPbA, kPbB, kPbPlus, kPbUp, kPbDown, kPbLeft, kPbRight, kPbZL, kPbX };   // 0.37.91: + ZL (Weapon Shop tracking); 0.37.109: + X (Call)
enum PjStep { kPjIdle = 0, kPjRun, kPjDone };
struct PhoneJob { int step, target, btn, plus_tries, stuck, last_pos, moves, kind, backs, mail_b, app_b, grid_a, act, one; uint64_t app_b_at, grid_a_at; int32_t tid, app; uint64_t ticks, md; uint64_t btn_until, wait_until, deadline; char status[96]; };   // kind 0 open a mail, 1 back (one B), 2 close the phone
PhoneJob g_pj;
void PjStatus(const char* a, int64_t n = INT64_MIN) {
    Buf b; b.add(a); if (n != INT64_MIN) b.num(n);
    const int k = b.n < 95 ? b.n : 95; memcpy(g_pj.status, b.s, static_cast<size_t>(k)); g_pj.status[k] = 0;
}
// 0.37.85: cursor moves wait 4 ticks (was 8 / 10) -- a far row is reached ~2x sooner; a read before the game moved
// the cursor just counts as 'not moved yet', so the stuck limit is 6 such reads (was 3)
constexpr int kPjMoveSettle = 3, kPjStuck = 7;   // 0.37.86: 3 (was 4)
int g_mail_shown;   // 0.37.86: the companion opened a mail on top (Mail's popup is up)
void PjPress(int btn, int settle) { g_pj.btn = btn; g_pj.btn_until = g_ticks + 1; g_pj.wait_until = g_ticks + 2 + static_cast<uint64_t>(settle); }
void PjStop(const char* why, int64_t n = INT64_MIN) { g_pj.step = kPjIdle; g_pj.btn = kPbNone; PjStatus(why, n); }
int PhoneState(const Guest& g, uint64_t dpc) {
    const uint64_t rp = g.ptr(dpc + 0xB0);
    const int32_t v = rp ? g.s32(rp + 0x10) : -1;
    return v >= 0 && v <= 2 ? v : -1;
}
void PhoneJobStep(const Instance& in, const Guest& g) {
    PhoneJob& j = g_pj;
    if (j.step != kPjRun || g_ticks < j.wait_until) return;
    if (g_ticks > j.deadline) { PjStop("Stopped: the phone did not get there in time"); return; }
    if (g_mode == 1) { PjStop("Not while diving: the phone opens on the boat or at the sushi bar"); return; }
    if (g_ld_on) { PjStop("Stopped: the game is loading"); return; }   // 0.38.1
    const uint64_t dpc = DavePhone(in, g);
    if (!dpc) { PjStop("Stopped: the phone is not loaded here"); return; }
    const int st = PhoneState(g, dpc);
    if (st < 0) { PjStop("Stopped: cannot read the phone's state"); return; }
    if (j.kind == 1) {                                               // BACK: one B (closes the open mail / app)
        if (j.backs++ == 0 && st != 0) { g_mail_shown = 0; PjStatus("Back"); PjPress(kPbB, 20); return; }
        j.step = kPjDone; PjStatus(st == 0 ? "The phone is closed" : "Back"); return;
    }
    if (j.kind == 3) {                                               // 0.37.91: one button (the craft popup's CONFIRM / CANCEL)
        // 0.38.1: checked again right before the press -- the phone still open on the Weapon Shop / iDiver (a tap just as
        // the phone closed must not send A to the boat)
        if (j.backs == 0) {
            const int32_t ex = g.s32(dpc + 0xA8);
            if (st != 2 || (ex != kAppDuffShop && ex != kAppIDiver)) { PjStop("Stopped: the phone is no longer on that screen"); return; }
        }
        if (j.backs++ == 0) { PjStatus(j.one == kPbA ? "Confirming..." : "Cancelled"); PjPress(j.one, 20); return; }
        j.step = kPjDone; PjStatus(j.one == kPbA ? "Done (see the top screen)" : "Cancelled"); return;
    }
    if (j.kind == 2) {                                               // CLOSE: B until the phone is closed
        if (st == 0) { j.step = kPjDone; PjStatus("Phone closed"); return; }
        if (j.backs++ >= 6) { PjStop("Stopped: the phone did not close"); return; }
        PjStatus("Closing the phone..."); PjPress(kPbB, 28); return;
    }
    if (st == 0) {                                                   // closed: + opens it
        if (j.plus_tries++ >= 2) { PjStop("Stopped: the phone did not open (open it yourself, then tap again)"); return; }
        PjStatus("Opening the phone..."); PjPress(kPbPlus, 36); return;
    }
    const int32_t exec = g.s32(dpc + 0xA8);
    const int32_t want = j.app ? j.app : kAppMessage;               // 0.37.72: Mail or EcoWatcher
    const char* app_name = want == kAppEcoWatcher ? "EcoWatcher" : want == kAppDuffShop ? "Weapon Shop" : want == kAppIDiver ? "iDiver" : want == kAppContacts ? "Call" : "Mail";
    // 0.37.88: B from an app shows the phone's home screen, but NowState stays Execute and m_ExecuteApp keeps the old app
    // (0.37.87 on device: it waited, pressed B again and closed the phone). So after that one B the job goes on as on
    // the home grid; a second B only for a mail the companion opened on top (that B closes just the mail).
    if (st == 2 && exec != want && !j.app_b) {
        if (g_mail_shown) { g_mail_shown = 0; PjStatus("Closing the open mail..."); PjPress(kPbB, 14); return; }
        j.app_b = 1; PjStatus("Back to the phone's home screen..."); PjPress(kPbB, 22); return;
    }
    if (st == 1 || (st == 2 && exec != want)) {                      // the home grid (0.37.88: also after that B): focus Mail, A
        const uint64_t pc = g.ptr(dpc + 0x48);
        const uint64_t list = pc ? g.ptr(pc + 0x88) : 0, items = list ? g.ptr(list + 0x10) : 0;
        const int32_t n = list ? g.s32(list + 0x18) : 0;
        int t = -1;
        for (int32_t i = 0; items && i < n && i < 64 && t < 0; ++i) {
            const uint64_t dd = g.ptr(items + 0x20 + 8 * static_cast<uint64_t>(i));
            if (dd && g.s32(dd + 0x10) == want) t = i;
        }
        if (t < 0) { Buf m; m.add("Stopped: ").add(app_name).add(" is not on the phone's home screen"); PjStop(m.s); return; }
        const int32_t f = g.s32(pc + 0x90);
        int32_t cols = g.s32(pc + 0xDC); if (cols < 1 || cols > 8) cols = 4;
        if (f == t) {
            // 0.37.88: A once; then wait for the app to report open (a second A would act inside the app)
            if (j.grid_a && g_ticks < j.grid_a_at + 120) { j.wait_until = g_ticks + 3; return; }
            if (j.grid_a >= 2) { PjStop("Stopped: the app did not open"); return; }
            ++j.grid_a; j.grid_a_at = g_ticks;
            Buf m; m.add("Opening ").add(app_name).add("..."); PjStatus(m.s); j.stuck = 0; j.last_pos = -1; PjPress(kPbA, 36); return;
        }
        if (f == j.last_pos && ++j.stuck >= kPjStuck) { PjStop("Stopped: the home screen's focus does not move (at ", f); return; }
        if (f != j.last_pos) j.stuck = 0;
        j.last_pos = f;
        if (++j.moves > 40) { PjStop("Stopped: too many moves"); return; }
        const int btn = f / cols < t / cols ? kPbDown : f / cols > t / cols ? kPbUp : f < t ? kPbRight : kPbLeft;
        { Buf m; m.add("Going to ").add(app_name).add("..."); PjStatus(m.s); } PjPress(btn, kPjMoveSettle); return;
    }
    if (want == kAppDuffShop) {                                      // 0.37.89: Weapon Shop open -> the cursor to the weapon (no A:
        const uint64_t wp = PhoneApp(g, dpc, kAppDuffShop);          // A would start crafting; the details show as the cursor lands)
        char wc[24];
        if (!wp || !g.cls(wp, wc, sizeof wc) || !eq(wc, "WeaponCraftPanel")) { PjStop("Stopped: the Weapon Shop's screen not found"); return; }
        if (g.s32(wp + 0x78) != 0) { PjStop("Switch the Weapon Shop to its Weapon Craft tab, then tap again"); return; }
        const uint64_t sc = g.ptr(wp + 0x70);
        const uint64_t list = sc ? g.ptr(sc + 0x88) : 0, items = list ? g.ptr(list + 0x10) : 0;
        const int32_t n = list ? g.s32(list + 0x18) : 0;
        if (n <= 0) { PjStatus("Waiting for the Weapon Shop..."); j.wait_until = g_ticks + 10; return; }
        int t = -1;
        for (int32_t i = 0; items && i < n && i < 256 && t < 0; ++i) {
            const uint64_t cd = g.ptr(items + 0x20 + 8 * static_cast<uint64_t>(i));
            if (cd && g.ptr(cd + 0x18) == j.md) t = i;               // ScrollerCellData.craftData
        }
        if (t < 0) { PjStop("Stopped: that weapon is not in the Weapon Shop's list"); return; }
        const int32_t f = g.s32(sc + 0x90);
        int32_t cols = g.s32(sc + 0xDC); if (cols < 1 || cols > 8) cols = 1;
        if (f == t) {
            // 0.37.91: then CRAFT presses A (the game asks to confirm: CONFIRM / CANCEL on the bottom screen), TRACK presses ZL
            j.step = kPjDone;
            if (j.act == 1) { g_shop_confirm = 1; g_shop_confirm_at = g_ticks; PjStatus("Craft it? CONFIRM or CANCEL"); PjPress(kPbA, 20); return; }
            if (j.act == 2) { PjStatus("Tracking switched on the top screen"); PjPress(kPbZL, 20); return; }
            PjStatus("Shown on the top screen"); return;
        }
        if (f == j.last_pos && ++j.stuck >= kPjStuck) { PjStop("Stopped: the Weapon Shop's cursor does not move (at ", f); return; }
        if (f != j.last_pos) j.stuck = 0;
        j.last_pos = f;
        if (++j.moves > 80) { PjStop("Stopped: too many moves"); return; }
        const int btn = f / cols < t / cols ? kPbDown : f / cols > t / cols ? kPbUp : f < t ? kPbRight : kPbLeft;
        PjStatus("Finding the weapon..."); PjPress(btn, kPjMoveSettle); return;
    }
    if (want == kAppContacts) {                                      // 0.37.109: Call open -> the cursor to the contact; CALL: X
        const uint64_t sc = CallScroller(g, dpc);
        if (!sc) { PjStatus("Waiting for Call..."); j.wait_until = g_ticks + 10; if (++j.backs > 12) PjStop("Stopped: Call's list not found"); return; }
        const uint64_t list = g.ptr(sc + 0x88), items = list ? g.ptr(list + 0x10) : 0;
        const int32_t n = list ? g.s32(list + 0x18) : 0;
        if (n <= 0) { PjStatus("Waiting for Call..."); j.wait_until = g_ticks + 10; return; }
        int t = -1;
        for (int32_t i = 0; items && i < n && i < 256 && t < 0; ++i) {
            const uint64_t cd = g.ptr(items + 0x20 + 8 * static_cast<uint64_t>(i));
            if (cd && static_cast<uint64_t>(g.s32(cd + 0x10)) == j.md) t = i;   // Data.NpcID
        }
        if (t < 0) { PjStop("Stopped: that contact is not in the list"); return; }
        const int32_t f = g.s32(sc + 0x90);
        if (f == t) {
            j.step = kPjDone;
            if (j.act == 1) { PjStatus("Calling... (BACK hangs up)"); PjPress(kPbX, 20); return; }
            PjStatus("Shown on the top screen"); return;
        }
        if (f == j.last_pos && ++j.stuck >= kPjStuck) { PjStop("Stopped: Call's cursor does not move (at ", f); return; }
        if (f != j.last_pos) j.stuck = 0;
        j.last_pos = f;
        if (++j.moves > 60) { PjStop("Stopped: too many moves"); return; }
        PjStatus("Finding the contact..."); PjPress(f < t ? kPbDown : kPbUp, kPjMoveSettle); return;
    }
    if (want == kAppIDiver) {                                        // 0.37.102: iDiver open -> the cursor to the gear; UPGRADE: A (the
        const uint64_t ip = PhoneApp(g, dpc, kAppIDiver);             // game's detail opens; its A buys -> CONFIRM / CANCEL here)
        char ic[24];
        if (!ip || !g.cls(ip, ic, sizeof ic) || !eq(ic, "IDiverPanel")) { PjStop("Stopped: iDiver's screen not found"); return; }
        const uint64_t sc = g.ptr(ip + 0x30);
        if (!sc) { PjStatus("Waiting for iDiver..."); j.wait_until = g_ticks + 10; return; }
        const int32_t ui = g.s32(sc + 0x114);
        if (ui == 1) { if (j.backs++ >= 2) { PjStop("Stopped: iDiver's detail did not close"); return; } PjStatus("Closing the detail..."); PjPress(kPbB, 20); return; }
        if (ui == 2) { if (j.backs++ >= 3) { PjStop("Stopped: iDiver's level-up screen did not close"); return; } PjStatus("Closing the level-up..."); PjPress(kPbA, 24); return; }
        const uint64_t list = g.ptr(sc + 0x88), items = list ? g.ptr(list + 0x10) : 0;
        const int32_t n = list ? g.s32(list + 0x18) : 0;
        if (n <= 0) { PjStatus("Waiting for iDiver..."); j.wait_until = g_ticks + 10; return; }
        int t = -1;
        for (int32_t i = 0; items && i < n && i < 32 && t < 0; ++i) {
            const uint64_t cd = g.ptr(items + 0x20 + 8 * static_cast<uint64_t>(i));
            if (cd && static_cast<uint64_t>(g.s32(cd + 0x14)) == j.md) t = i;   // ScrollerCellData.subEquiptype
        }
        if (t < 0) { PjStop("Stopped: that gear is not in iDiver's list"); return; }
        const int32_t f = g.s32(sc + 0x90);
        if (f == t) {
            j.step = kPjDone;
            if (j.act == 1) { g_idv_confirm = 1; g_idv_confirm_at = g_ticks; PjStatus("Upgrade it? CONFIRM or CANCEL"); PjPress(kPbA, 20); return; }
            PjStatus("Shown on the top screen"); return;
        }
        if (f == j.last_pos && ++j.stuck >= kPjStuck) { PjStop("Stopped: iDiver's cursor does not move (at ", f); return; }
        if (f != j.last_pos) j.stuck = 0;
        j.last_pos = f;
        if (++j.moves > 40) { PjStop("Stopped: too many moves"); return; }
        PjStatus("Finding the gear..."); PjPress(f < t ? kPbDown : kPbUp, kPjMoveSettle); return;
    }
    if (want == kAppEcoWatcher) {                                    // 0.37.72: EcoWatcher open -> Up / Down to the mission, A
        const uint64_t ep = PhoneApp(g, dpc, kAppEcoWatcher);
        char ec[24];
        if (!ep || !g.cls(ep, ec, sizeof ec) || !eq(ec, "EcoWatcherPanel")) { PjStop("Stopped: EcoWatcher's screen not found"); return; }
        const uint64_t lp = g.ptr(ep + 0x38);
        const uint64_t list = lp ? g.ptr(lp + 0x88) : 0, items = list ? g.ptr(list + 0x10) : 0;
        const int32_t n = list ? g.s32(list + 0x18) : 0;
        if (n <= 0) { PjStatus("Waiting for EcoWatcher..."); j.wait_until = g_ticks + 10; return; }
        int t = -1;
        for (int32_t i = 0; items && i < n && i < 256 && t < 0; ++i) {
            const uint64_t cd = g.ptr(items + 0x20 + 8 * static_cast<uint64_t>(i));
            if (cd && g.ptr(cd + 0x18) == j.md) t = i;               // EcoWatcherListData.RawData
        }
        if (t < 0) { PjStop("Stopped: that mission is not in EcoWatcher's list"); return; }
        const int32_t f = g.s32(lp + 0x90);
        if (f == t) { j.step = kPjDone; PjStatus("Opened on the top screen"); PjPress(kPbA, 30); return; }
        if (f == j.last_pos && ++j.stuck >= kPjStuck) { PjStop("Stopped: EcoWatcher's cursor does not move (at ", f); return; }
        if (f != j.last_pos) j.stuck = 0;
        j.last_pos = f;
        if (++j.moves > 80) { PjStop("Stopped: too many moves"); return; }
        PjStatus("Finding the mission..."); PjPress(f < t ? kPbDown : kPbUp, kPjMoveSettle); return;
    }
    // st == 2, Mail open: Up / Down to the tapped mail, A
    const uint64_t mp = PhoneApp(g, dpc, kAppMessage);
    char c[24];
    if (!mp || !g.cls(mp, c, sizeof c) || !eq(c, "MailPanel")) { PjStop("Stopped: Mail's screen not found"); return; }
    const uint64_t list = g.ptr(mp + 0x68);
    const uint64_t items = list ? g.ptr(list + 0x10) : 0;
    const int32_t n = list ? g.s32(list + 0x18) : 0;
    if (n <= 0) { PjStatus("Waiting for Mail..."); j.wait_until = g_ticks + 10; return; }
    // 0.37.69: the tapped mail by what it is (MailData.TID @0x10, receiveDate), not by its row on the bottom screen
    int found = -1;
    for (int32_t i = 0; items && i < n && i < 512 && found < 0; ++i) {
        const uint64_t cell = g.ptr(items + 0x20 + 8 * static_cast<uint64_t>(i));
        const uint64_t md = cell ? g.ptr(cell + 0x18) : 0;
        uint64_t tk = 0; if (cell) g.read(cell + 0x20, &tk, 8); tk &= UINT64_C(0x3FFFFFFFFFFFFFFF);
        if (md && g.s32(md + 0x10) == j.tid && (!j.ticks || tk == j.ticks)) found = i;
    }
    if (found >= 0) j.target = found;
    if (j.target >= n) { PjStop("Stopped: that mail is no longer in the list"); return; }
    const int32_t sel = g.s32(mp + 0x74);
    // 0.37.86: a mail already open on top -> one B closes it first, then on to the tapped one (no BACK needed)
    if (g_mail_shown) { g_mail_shown = 0; PjStatus("Closing the open mail..."); j.stuck = 0; j.last_pos = -1; PjPress(kPbB, 14); return; }
    if (sel == j.target) { j.step = kPjDone; g_mail_shown = 1; PjStatus("Opened on the top screen"); PjPress(kPbA, 30); return; }
    if (sel == j.last_pos && ++j.stuck >= kPjStuck) {
        // 0.37.86: a mail opened by hand blocks the list: one B, then try again (once)
        if (!j.mail_b) { j.mail_b = 1; j.stuck = 0; j.last_pos = -1; PjStatus("Closing the open mail..."); PjPress(kPbB, 14); return; }
        PjStop("Stopped: Mail's cursor does not move (at ", sel); return;
    }
    if (sel != j.last_pos) j.stuck = 0;
    j.last_pos = sel;
    if (++j.moves > 60) { PjStop("Stopped: too many moves"); return; }
    PjStatus("Finding the mail..."); PjPress(sel < j.target ? kPbDown : kPbUp, kPjMoveSettle);
}
bool PhoneJobStart(int target, int kind = 0, int32_t app = 0) {
    if (g_pj.step == kPjRun) return true;                             // one job at a time
    g_pj = PhoneJob{}; g_pj.step = kPjRun; g_pj.target = target; g_pj.last_pos = -1; g_pj.kind = kind; g_pj.app = app;
    if (app == kAppEcoWatcher && target >= 0 && target < kEcoRows) g_pj.md = g_eco.row[target].md;
    if (app == kAppDuffShop && target >= 0 && target < kShopRows) g_pj.md = g_shop.row[target].craft;
    if (app == kAppIDiver && target >= 0 && target < kIdvRows) g_pj.md = static_cast<uint64_t>(g_idv.row[target].type);
    if (app == kAppContacts && target >= 0 && target < kCallRows) g_pj.md = static_cast<uint64_t>(g_call.row[target].npc);
    if (kind == 0 && !app && target >= 0 && target < kMailRows) { g_pj.tid = g_ph.row[target].tid; g_pj.ticks = g_ph.row[target].ticks; }
    g_pj.deadline = g_ticks + 60 * 20; g_pj.wait_until = g_ticks;
    PjStatus("Starting...");
    return true;
}
bool PjRunning() { return g_pj.step == kPjRun; }
bool PhoneJobBusy() {
    return g_pj.step == kPjRun || (g_pj.btn != kPbNone && g_ticks < g_pj.btn_until);
}
void PhoneJobPublish(const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    const bool held = g_pj.btn != kPbNone && g_ticks < g_pj.btn_until;
    if (g_pj.step == kPjDone && !held) g_pj.step = kPjIdle;
    static const char* const kKeys[10] = {"", "ph.p_a", "ph.p_b", "ph.p_plus", "ph.p_up", "ph.p_down", "ph.p_left", "ph.p_right", "ph.p_zl", "ph.p_x"};
    for (int b = 1; b < 10; ++b) h->publish_i64(u, kKeys[b], held && g_pj.btn == b ? 1 : 0);
    h->publish_i64(u, "ph.busy", g_pj.step == kPjRun ? 1 : 0);
    h->publish_text(u, "ph.status", g_pj.status);
    h->publish_i64(u, "ph.has_status", g_pj.status[0] ? 1 : 0);
}
int g_mail_off;                               // 0.37.83: the first mail shown
constexpr int kMailVis = kMailRows;   // 0.37.84: the page scrolls the list itself (drag); no offset
void PhonePublish(const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    const PhoneProbe& p = g_ph;
    h->publish_text(u, "ph.diag", p.diag);
    h->publish_i64(u, "ph.show", g_ph_show);
    h->publish_i64(u, "mail.ok", p.n > 0 || p.mail ? 1 : 0);                        // a list to show (the screen's, or the save's)
    h->publish_i64(u, "mail.live", p.mail && p.opened && p.exec == kAppMessage ? 1 : 0);   // ... and is open now
    h->publish_i64(u, "mail.n", p.n);
    h->publish_i64(u, "mail.rows", p.n < kMailRows ? p.n : kMailRows);              // 0.37.84: the scrolling list's rows
    h->publish_i64(u, "ph.closed", !p.dpc || p.state == 0 ? 1 : 0);                 // 0.37.84: BACK -> APPS when closed (0.37.85: NowState)
    h->publish_i64(u, "mail.sel", p.sel);
    Buf k, v, t2; char fb[128], fb2[64];
    {   // 0.37.83: taller rows, kMailVis shown, UP / DOWN (mail_scroll)
        const int nn = p.n < kMailRows ? p.n : kMailRows, mx = nn > kMailVis ? nn - kMailVis : 0;
        if (g_mail_off > mx) g_mail_off = mx; if (g_mail_off < 0) g_mail_off = 0;
        h->publish_i64(u, "mail.can_up", g_mail_off > 0 ? 1 : 0); h->publish_i64(u, "mail.can_dn", g_mail_off < mx ? 1 : 0);
    }
    for (int i = 0; i < kMailRows; ++i) {
        const int j = i + g_mail_off;                               // the page's row i shows mail j
        const MailRow& r = p.row[j < kMailRows ? j : 0];
        const bool on = j < p.n && j < kMailRows;
        const char* title = on ? TextFor(r.key_t, fb, sizeof fb) : "";
        const char* sender = on ? TextFor(r.key_s, fb2, sizeof fb2) : "";
        k.clear(); k.add("mail.t").num(i); h->publish_text(u, k.s, NoApos(title, t2));
        k.clear(); k.add("mail.s").num(i); h->publish_text(u, k.s, NoApos(sender, t2));
        v.clear();
        if (on && TicksOk(r.ticks)) { int y, mo, dd, dw; CivilFromTicks(r.ticks, &y, &mo, &dd, &dw);
            if (mo < 10) v.addc('0'); v.num(mo).addc('.'); if (dd < 10) v.addc('0'); v.num(dd); }
        k.clear(); k.add("mail.d").num(i); h->publish_text(u, k.s, v.s);
        k.clear(); k.add("mail.new").num(i); h->publish_i64(u, k.s, on && r.unread ? 1 : 0);
        k.clear(); k.add("mail.on").num(i); h->publish_i64(u, k.s, on ? 1 : 0);
        k.clear(); k.add("mail.cur").num(i); h->publish_i64(u, k.s, on && j == p.sel ? 1 : 0);
        // 0.37.69: the sender picture -- SenderIcon names one of UI_Mail_Sender_Icon_01..07 (MailAtlas); a bare number
        // or a name ending in one is mapped to it
        v.clear();
        if (on && r.icon[0]) {
            int e = 0; while (r.icon[e]) ++e;
            int b = e; while (b > 0 && r.icon[b - 1] >= '0' && r.icon[b - 1] <= '9') --b;
            if (starts(r.icon, "UI_Mail_Sender_Icon_") || b == e) v.add("module:icon/").add(r.icon);
            else { const int num = (e - b <= 3) ? [&] { int x = 0; for (int q = b; q < e; ++q) x = x * 10 + (r.icon[q] - '0'); return x; }() : 0;
                   v.add("module:icon/UI_Mail_Sender_Icon_"); if (num < 10) v.addc('0'); v.num(num); }
        }
        k.clear(); k.add("mail.icon").num(i); h->publish_text(u, k.s, v.s);
        if (i < 6) {                                                                  // the probe panel's lines
            v.clear(); if (on) { v.num(i).add(r.unread ? ". N " : ". ").add(title).add(" / ").add(sender).add(" [").add(r.icon).add("]"); }
            k.clear(); k.add("ph.mail").num(i); h->publish_text(u, k.s, v.s);
        }
    }
    const int all = p.from_save ? p.total : p.n;
    h->publish_i64(u, "mail.more", all > kMailRows ? all - kMailRows : 0);
}
void PublishClock(const Instance& in, const Guest& g, const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    const uint64_t gi = in.gs ? g.ptr(in.gs + 0xF0) : 0;
    uint64_t today = 0;
    int32_t dt = -1, wx = -1;
    Buf diag;
    if (gi) {
        uint64_t kv[4] = {};                                  // key, hidden, (inited), fake
        if (g.read(gi + 0x88, kv, sizeof kv)) {
            const uint64_t a = (kv[1] ^ kv[0]) & UINT64_C(0x3FFFFFFFFFFFFFFF), b = kv[3] & UINT64_C(0x3FFFFFFFFFFFFFFF);
            today = TicksOk(a) ? a : TicksOk(b) ? b : 0;
            diag.add("date ").hex(kv[1] ^ kv[0]);
        }
        if (!g.obscured(gi + 0xD8, &dt)) dt = -1;
        const uint64_t wi = g.ptr(gi + 0xF8);
        if (wi) wx = g.s32(wi + 0x10);
        diag.add(" time ").num(dt).add(" weather ").num(wx);
    } else {
        diag.add(in.gs ? "no game info" : "no save yet");
    }
    char date[16] = "";
    if (today) {
        static const char* kDays[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        int y, mo, d, dow; CivilFromTicks(today, &y, &mo, &d, &dow);
        Buf b; b.add(kDays[dow]).add(" ");
        if (mo < 10) b.add("0");
        b.num(mo).add(".");
        if (d < 10) b.add("0");
        b.num(d);
        memcpy(date, b.s, static_cast<size_t>(b.n) + 1);
    }
    const char* tsrc = "";
    if (dt == 0) tsrc = "module:icon/UI_Watch_Icon_Morning";
    else if (dt == 1 || dt == 4) tsrc = "module:icon/UI_Watch_Icon_Afternoon";
    else if (dt == 2 || dt == 3) {
        // the moon, the way the game's DayManager.CalculateMoonState (main+45A97A0) works it out:
        // whole days from the save's new-moon date (GameInfoSave+0x130) to today, modulo 30:
        // 0 new, 1-7 waxing crescent, 8 first quarter, 9-14 waxing gibbous, 15 full, 16-22 waning
        // gibbous, 23 last quarter, 24-29 (and anything negative) waning crescent (round 24)
        static const char* kMoon[8] = {"NewMoon", "WaxingCrescent", "FirstQuarter", "WaxingGibbous",
                                       "FullMoon", "WaningGibbous", "LastQuarter", "WaningCrescent"};
        uint64_t nm = 0; g.read(gi + 0x130, &nm, 8); nm &= UINT64_C(0x3FFFFFFFFFFFFFFF);
        int k = 7;                                     // unknown: a crescent, as the game's evening watch usually shows
        if (today && TicksOk(nm)) {
            const int64_t days = (static_cast<int64_t>(today) - static_cast<int64_t>(nm)) / static_cast<int64_t>(kTicksPerDay);
            k = MoonStateForDay(static_cast<int32_t>(days % 30));
            diag.add(" moon day ").num(days);
        } else {
            diag.add(" moon ").hex(nm);
        }
        static char moon[64];
        Buf b; b.add("module:icon/UI_Watch_Icon_Moon_").add(kMoon[k]);
        memcpy(moon, b.s, static_cast<size_t>(b.n) + 1);
        tsrc = moon;
    }
    const char* wsrc = wx == 0 ? "module:icon/Icon_Weather_Sunny" : wx == 1 ? "module:icon/Icon_Weather_Rainy"
                     : wx == 2 ? "module:icon/Icon_Weather_HeavyRainy" : wx == 3 ? "module:icon/Icon_Weather_Cloudy" : "";
    h->publish_i64(u, "clock.ok", date[0] ? 1 : 0);
    h->publish_text(u, "clock.date", date);
    h->publish_i64(u, "clock.has_time", tsrc[0] ? 1 : 0);
    h->publish_text(u, "clock.tsrc", tsrc);
    h->publish_i64(u, "clock.has_wx", wsrc[0] ? 1 : 0);
    h->publish_text(u, "clock.wsrc", wsrc);
    const char* ttxt = dt == 0 ? "MORNING" : (dt == 1 || dt == 4) ? "AFTERNOON" : (dt == 2 || dt == 3) ? "EVENING" : "";
    const char* wtxt = wx == 0 ? "SUNNY" : wx == 1 ? "RAINY" : wx == 2 ? "HEAVY RAIN" : wx == 3 ? "FOG" : "";
    h->publish_text(u, "clock.ttxt", ttxt);
    h->publish_text(u, "clock.wtxt", wtxt);
    h->publish_i64(u, "clock.daytime", dt);
    h->publish_i64(u, "clock.weather", wx);
    h->publish_text(u, "clock.diag", diag.s);
}

void MenuWatch();
void MenuStep(Instance& in, const Guest& g);
void MenuPublish(const Instance& in, const EdenDsmodHostApi* h);
// ---------------------------------------------------------------- 0.38.7 (research): the Sea People Village map
// MainCanvasManager (Singleton`1, reached through Method$Singleton<MainCanvasManager>.get_Instance() @main+9E0ECC0)
// @0x1F0 MVMinimapUIController: curMinimapType @0x20, @0x40 the detail panel (the - button's map), @0x48 the downsize
// panel (the corner map). MVMinimapDetailPanel: listArea @0x28 (List<MVMinimapArea>, AreaType @0x20), convertRatio
// @0x34, DaveIconObj @0x38, InnerMapObj @0x48, totalOffset @0x114. UpdateDavePosition (corner map): innerMap.local =
// -(dave world xy) * ratio * 0.5 - totalOffset * 0.5. DIAG prints the pieces' positions (UI world, as the map's
// marks are read) to tell where each building and Dave's pin go.
constexpr uint64_t kMainCanvasSlot = 0x9E0ECC0;
char g_vm_diag[1200];
int g_vm_zoomed, g_vm_from;
struct VmMap { int ok, pin; float k; int16_t x[16], y[16]; uint8_t on[16]; int16_t px, py; } g_vm;
uint64_t MainCanvas(const Instance& in, const Guest& g) {
    const uint64_t mi = g.ptr(in.main_base + kMainCanvasSlot);
    if (!mi || (mi & 1)) return 0;
    char c[40];
    for (uint64_t koff = 0x18; koff <= 0x20; koff += 8) {
        const uint64_t k = g.ptr(mi + koff);
        if (!k || !g.cstr(g.ptr(k + 0x10), c, sizeof c) || !eq(c, "Singleton`1")) continue;
        const uint64_t st = g.ptr(k + kStatics);
        for (int j = 0; st && j < 8; ++j) {
            const uint64_t o = g.ptr(st + 8 * static_cast<uint64_t>(j));
            if (o && g.cls(o, c, sizeof c) && eq(c, "MainCanvasManager")) return o;
        }
    }
    return 0;
}
// The transform layout the map's marks learned on a dive, kept for the session (the marks start over on every new
// level, the village included -- so the village reads the copy taken on the last dive)
struct VmLayout { int ok, o1, o2, o3, o4, xoff, poff, stride; } g_vml;
float g_vm_lx[16], g_vm_ly[16], g_vm_u = 1.0f; int g_vm_lok;   // 0.38.21 DIAG: the local reads, units -> picture px
void VmKeepLayout() {
    const MarkerProbe& m = g_mp;
    if (m.fixed && m.o1) g_vml = {1, m.o1, m.o2, m.o3, m.o4, m.xoff, m.poff, m.stride};
}
// a native GameObject -> its Transform's position (GameObject + o2 -> components, + o3 -> Transform)
bool VmNatGoPos(const Guest& g, uint64_t G, float* x, float* y) {
    const VmLayout& L = g_vml;
    if (!G || !L.ok) return false;
    const uint64_t C = g.ptr(G + L.o2); if (!C) return false;
    const uint64_t T = g.ptr(C + L.o3); if (!T || g.ptr(T + L.o1) != G) return false;
    return MpWorld(g, T, L.o4, L.xoff, L.poff, L.stride, x, y);
}
bool VmGoPos(const Guest& g, uint64_t go, float* x, float* y) { return go && VmNatGoPos(g, g.ptr(go + 0x10), x, y); }
bool VmCompPos(const Guest& g, uint64_t comp, float* x, float* y) {   // a managed component -> its GameObject's transform
    const uint64_t nat = comp ? g.ptr(comp + 0x10) : 0;
    return nat && g_vml.ok && VmNatGoPos(g, g.ptr(nat + g_vml.o1), x, y);
}
void VillageMapDiag(const Instance& in, const Guest& g) {
    Buf b; char c[48];
    const uint64_t mcm = MainCanvas(in, g);
    b.add("mcm ").add(mcm ? "ok" : "?");
    if (!g_vml.ok) b.add(" | layout not learned (dive once this session)");
    const uint64_t ctl = mcm ? g.ptr(mcm + 0x1F0) : 0;
    if (!ctl || !g.cls(ctl, c, sizeof c) || !eq(c, "MVMinimapUIController")) { b.add(" | ctrl ?"); DcCopy(g_vm_diag, sizeof g_vm_diag, b.s); return; }
    b.add(" | type ").num(g.s32(ctl + 0x20));
    float x = 0, y = 0;
    {
        const uint64_t igt = g.ptr(in.main_base + kInGameManagerTypeInfo), sg = igt ? g.ptr(igt + kParent) : 0;
        const uint64_t st = sg ? g.ptr(sg + kStatics) : 0, igm = st ? g.ptr(st) : 0;
        if (igm && DaveOwnPos(g, igm, &x, &y)) { b.add(" | dave "); AddF(b, x); b.addc(','); AddF(b, y); }
    }
    for (int k = 0; k < 2; ++k) {
        const uint64_t p = g.ptr(ctl + (k ? 0x48 : 0x40));
        b.add(k ? " || CORNER" : " || DETAIL");
        if (!p) { b.add(" none"); continue; }
        float r = 0; g.read(p + 0x34, &r, 4);
        float to[3] = {}; g.read(p + 0x114, to, 12);
        b.add(" r "); AddF(b, r * 1000.0f); b.add("/1000 off "); AddF(b, to[0]); b.addc(','); AddF(b, to[1]);
        if (VmGoPos(g, g.ptr(p + 0x48), &x, &y)) { b.add(" map "); AddF(b, x); b.addc(','); AddF(b, y); }
        if (VmGoPos(g, g.ptr(p + 0x38), &x, &y)) { b.add(" pin "); AddF(b, x); b.addc(','); AddF(b, y); }
        const uint64_t l = g.ptr(p + 0x28), it = l ? g.ptr(l + 0x10) : 0;
        int n = l ? g.s32(l + 0x18) : 0; if (n < 0 || n > 32) n = 0;
        b.add(" areas ").num(n).addc(':');
        for (int i = 0; i < n && it; ++i) {
            const uint64_t a = g.ptr(it + 0x20 + 8 * static_cast<uint64_t>(i)); if (!a) continue;
            b.addc(' ').num(g.s32(a + 0x20));
            if (VmCompPos(g, a, &x, &y)) { b.addc('@'); AddF(b, x); b.addc(','); AddF(b, y); }
        }
    }
    b.add(g_vm.ok ? " | placed" : " | not placed yet").add(" from ").num(g_vm_from).add(" local ").num(g_vm_lok)
     .add(" u "); AddF(b, g_vm_u * 100.0f); b.add("% gate "); AddF(b, g_vm_lx[14]); b.addc(','); AddF(b, g_vm_ly[14]); b.add(" parlor "); AddF(b, g_vm_lx[15]); b.addc(','); AddF(b, g_vm_ly[15]);
    DcCopy(g_vm_diag, sizeof g_vm_diag, b.s);
}
// 0.38.8: the village map's pieces where the game puts them. The panel's map-pixel scale k comes from the game's own
// numbers: Dave's pin sits (dave.x * ratio + totalOffset.x) map pixels right of the map's centre (UpdateDavePosition),
// so k = that / (pin.x - map.x) in the panel's UI space; then any piece of that panel (each area, the pin) is at
// (piece - map) * k map pixels from the centre of the 1858 x 804 picture. A panel whose pieces are folded away (the
// detail map when closed reads x 0) or whose k is off is skipped; the last good places are kept.
// 0.38.21: why the buildings moved -- the big map folds shut by scaling its x to 0 (closed it reads every x as 0, the
// y's unchanged), and while it opens / closes the x scale is part way. The scale was taken from x (the gate) and used
// for y too, so each frame of that animation stretched the y's -- and those were kept. Now every piece is read in the
// inner map's OWN units (its local space: walked up the transform tree only as far as the inner map), which no fold,
// scroll or animation of the panels above it changes, and the corner map's inner map holds the same picture, so both
// panels give the same numbers. Dave's pin: the game's own rule (UpdateDavePosition) -- Dave sits at
// dave * ratio + totalOffset in the inner map's units.
// T's position in the local space of its ancestor A (same hierarchy); false if A is not an ancestor
bool VmLocalIn(const Guest& g, uint64_t T, uint64_t A, float* x, float* y) {
    const VmLayout& L = g_vml;
    static const int kStride[3] = {48, 40, 64}, kQ[3] = {4, 3, 4}, kS[3] = {8, 7, 8};
    if (!T || !A || L.stride < 0 || L.stride > 2) return false;
    const int stride = kStride[L.stride], qo = kQ[L.stride], so = kS[L.stride];
    const uint64_t H = g.ptr(T + L.o4); if (!H || g.ptr(A + L.o4) != H) return false;
    bool ok = false, ok2 = false;
    int32_t idx = g.s32(T + L.o4 + 8, &ok); const int32_t aidx = g.s32(A + L.o4 + 8, &ok2);
    if (!ok || !ok2 || idx < 0 || aidx < 0 || idx > 200000 || aidx > 200000) return false;
    const uint64_t N = g.ptr(H + L.xoff), P = g.ptr(H + L.poff); if (!N || !P) return false;
    float px = 0, py = 0, pz = 0; bool first = true;
    for (int depth = 0; depth < 32; ++depth) {
        if (idx == aidx) { *x = px; *y = py; return !first; }
        float t[12]; if (!g.read(N + static_cast<uint64_t>(idx) * stride, t, 44)) return false;
        if (first) { px = t[0]; py = t[1]; pz = t[2]; first = false; }
        else {
            const float sx = px * t[so], sy = py * t[so + 1], sz = pz * t[so + 2];
            const float qx = t[qo], qy = t[qo + 1], qz = t[qo + 2], qw = t[qo + 3];
            const float ux = qy * sz - qz * sy, uy = qz * sx - qx * sz, uz = qx * sy - qy * sx;
            const float wx = qy * uz - qz * uy, wy = qz * ux - qx * uz, wz = qx * uy - qy * ux;
            px = sx + 2.0f * (qw * ux + wx) + t[0]; py = sy + 2.0f * (qw * uy + wy) + t[1]; pz = sz + 2.0f * (qw * uz + wz) + t[2];
        }
        bool okp = false; const int32_t par = g.s32(P + static_cast<uint64_t>(idx) * 4, &okp);
        if (!okp || par == idx || par < 0 || par > 200000) return false;
        idx = par;
    }
    return false;
}
uint64_t VmGoT(const Guest& g, uint64_t G) {   // a native GameObject -> its Transform
    const VmLayout& L = g_vml;
    if (!G || !L.ok) return 0;
    const uint64_t C = g.ptr(G + L.o2); if (!C) return 0;
    const uint64_t T = g.ptr(C + L.o3); return (T && g.ptr(T + L.o1) == G) ? T : 0;
}
void VillageMapStep(const Instance& in, const Guest& g) {
    const uint64_t mcm = MainCanvas(in, g), ctl = mcm ? g.ptr(mcm + 0x1F0) : 0;
    char c[40];
    if (!ctl || !g.cls(ctl, c, sizeof c) || !eq(c, "MVMinimapUIController") || !g_vml.ok) return;
    float dx = 0, dy = 0;
    {
        const uint64_t igt = g.ptr(in.main_base + kInGameManagerTypeInfo), sg = igt ? g.ptr(igt + kParent) : 0;
        const uint64_t st = sg ? g.ptr(sg + kStatics) : 0, igm = st ? g.ptr(st) : 0;
        if (!igm || !DaveOwnPos(g, igm, &dx, &dy)) return;
    }
    const uint64_t pd = g.ptr(ctl + 0x40);              // the big map's panel: its ratio and offset (fixed numbers)
    float r0 = 0, off0[3] = {};
    if (pd) { g.read(pd + 0x34, &r0, 4); g.read(pd + 0x114, off0, 12); }
    const float yfix = off0[1] * 0.25f;                 // (0.38.9: the picture sits this far above the inner map's middle)
    // the inner map's units -> the picture's pixels: the gate and the Game Parlor stand 1298 px apart on the picture
    // (858 left / 440 right of its middle, 0.38.8); 1 until both have been read
    float& u = g_vm_u;
    auto toX = [&](float lx) { return static_cast<int>(500.0f + lx * u / 1858.0f * 1000.0f); };
    auto toY = [&](float ly) { return static_cast<int>(500.0f - (ly * u - yfix) / 804.0f * 1000.0f); };
    g_vm_lok = 0;
    for (int k = 0; k < 2; ++k) {                       // either panel: the same picture, the same local numbers
        const uint64_t p = g.ptr(ctl + (k ? 0x48 : 0x40));
        const uint64_t im = p ? g.ptr(p + 0x48) : 0;    // InnerMapObj (managed GameObject)
        const uint64_t IT = im ? VmGoT(g, g.ptr(im + 0x10)) : 0;
        if (!IT) continue;
        const uint64_t l = g.ptr(p + 0x28), it = l ? g.ptr(l + 0x10) : 0;
        int n = l ? g.s32(l + 0x18) : 0; if (n < 0 || n > 32) n = 0;
        int got = 0;
        int16_t X[16] = {}, Y[16] = {}; uint8_t on[16] = {};
        for (int i = 0; i < n && it; ++i) {
            const uint64_t a = g.ptr(it + 0x20 + 8 * static_cast<uint64_t>(i)); if (!a) continue;
            const int32_t t = g.s32(a + 0x20); if (t < 1 || t > 15) continue;
            const uint64_t nat = g.ptr(a + 0x10), AT = nat ? VmGoT(g, g.ptr(nat + g_vml.o1)) : 0;
            float lx, ly;
            if (!AT || !VmLocalIn(g, AT, IT, &lx, &ly)) continue;
            g_vm_lx[t] = lx; g_vm_ly[t] = ly;
            // 0.38.22: the scale from the two upper portals (7 left, 15 right: 878 px apart on the game's map screen, 0.38.21
            // photos) -- the gate's own point sits off its mark, so with it in the pair every other piece came out ~5% too
            // close to the middle; the gate itself is moved 47 px right to where the game draws its mark
            if (t == 15 && g_vm_lx[7] != 0 && g_vm_lx[7] < g_vm_lx[15] - 1.0f) { const float uu = 878.0f / (g_vm_lx[15] - g_vm_lx[7]); if (uu > 0.05f && uu < 50.0f) u = uu; }
            const int x = toX(lx), y = toY(ly);   // 0.38.23: the gate's building back on its point (the page moves only its mark)
            if (x < -50 || x > 1050 || y < -50 || y > 1050) continue;
            X[t] = static_cast<int16_t>(x); Y[t] = static_cast<int16_t>(y); on[t] = 1; ++got;
        }
        if (got < 5) continue;                          // not this panel (not laid out yet): the other one
        for (int t = 1; t < 16; ++t) if (on[t]) { g_vm.x[t] = X[t]; g_vm.y[t] = Y[t]; g_vm.on[t] = 1; }
        g_vm.ok = 1; g_vm.k = 1; g_vm_from = k ? 1 : 2; g_vm_lok = got;
        break;
    }
    if (r0 > 0) {   // Dave's pin by the game's rule; y as measured on device (0.38.8: 7.7 -> -110, 9.3 -> -109 = dave.y x 12 - 110)
        // 0.38.22: toX applies u itself (it was applied twice)
        const float ry = dy * r0 * u - 110.0f;
        const int PX = toX(dx * r0 + off0[0]), PY = static_cast<int>(500.0f - ry / 804.0f * 1000.0f);
        if (PX >= -20 && PX <= 1020 && PY >= -20 && PY <= 1020) { g_vm.px = static_cast<int16_t>(PX); g_vm.py = static_cast<int16_t>(PY); g_vm.pin = 1; }
    }
}
// 0.38.14: tap -> x3 around Dave's pin (the window kept inside the map, as the dive map's); the pieces are moved into it
// each building picture's size in the game's map pixels (pictures.idx), by building type -- to tell which fit when zoomed
const int16_t kVmPw[16] = {0, 84, 206, 196, 135, 117, 106, 106, 368, 236, 124, 88, 53, 122, 48, 106};
const int16_t kVmPh[16] = {0, 89, 154, 189, 157, 146, 150, 150, 312, 317, 202, 86, 86, 44, 143, 150};
void VillageMapPublish(const EdenDsmodHostApi* h) {
    void* u = h->userdata; Buf k;
    const float z = g_vm_zoomed ? 3.0f : 1.0f;
    // 0.38.16: smooth -- the pin shown eases toward the one read (read every 2 ticks), and zoomed, the view stays put
    // while Dave is in its middle part and glides when he nears an edge (as a camera), instead of jumping with him
    static float spx = -1, spy = -1, vcx = 0.5f, vcy = 0.5f;
    if (g_vm.pin) {
        if (spx < 0) { spx = g_vm.px; spy = g_vm.py; }
        spx += (g_vm.px - spx) * 0.35f; spy += (g_vm.py - spy) * 0.35f;
    }
    const float pnx = spx < 0 ? 0.5f : spx / 1000.0f, pny = spy < 0 ? 0.5f : spy / 1000.0f;
    {
        const float bx = 0.5f / z * 0.45f, by = 0.5f / z * 0.45f;   // the still part: 45% of the half view
        float tx = vcx, ty = vcy;
        if (pnx > vcx + bx) tx = pnx - bx; else if (pnx < vcx - bx) tx = pnx + bx;
        if (pny > vcy + by) ty = pny - by; else if (pny < vcy - by) ty = pny + by;
        if (!g_vm_zoomed) { tx = pnx; ty = pny; }
        vcx += (tx - vcx) * 0.25f; vcy += (ty - vcy) * 0.25f;
    }
    auto win = [&](float n) { float w = n - 0.5f / z; if (w < 0) w = 0; if (w > 1 - 1 / z) w = 1 - 1 / z; return w; };
    const float wl = win(vcx), wt = win(vcy);
    auto zx = [&](float v) { return static_cast<int64_t>((v / 1000.0f - wl) * z * 1000.0f); };
    auto zy = [&](float v) { return static_cast<int64_t>((v / 1000.0f - wt) * z * 1000.0f); };
    for (int t = 1; t < 16; ++t) {
        const int64_t X = zx(g_vm.x[t]), Y = zy(g_vm.y[t]);
        const bool in = g_vm.on[t] && X >= 0 && X <= 1000 && Y >= 0 && Y <= 1000;
        const float hw = kVmPw[t] * z / 1858.0f * 500.0f, hh = kVmPh[t] * z / 804.0f * 1000.0f;
        // 0.38.16: zoomed buildings are drawn whenever any of them is in view (the page covers what reaches past the map)
        const bool fit = g_vm.on[t] && X + hw >= 0 && X - hw <= 1000 && Y >= 0 && Y - hh <= 1000;
        k.clear(); k.add("vm.on").num(t); h->publish_i64(u, k.s, in ? 1 : 0);
        k.clear(); k.add("vm.fit").num(t); h->publish_i64(u, k.s, fit ? 1 : 0);
        k.clear(); k.add("vm.x").num(t); h->publish_i64(u, k.s, X);
        k.clear(); k.add("vm.y").num(t); h->publish_i64(u, k.s, Y);
    }
    h->publish_i64(u, "vm.pin", g_vm.pin); h->publish_i64(u, "vm.px", zx(spx)); h->publish_i64(u, "vm.py", zy(spy));
    h->publish_i64(u, "vm.zoomed", g_vm_zoomed);
    h->publish_i64(u, "vm.vreset", static_cast<int64_t>(g_ticks));   // snapped to the module's view every tick
    if (h->publish_f64) { h->publish_f64(u, "vm.zoom", z); h->publish_f64(u, "vm.zx", wl + 0.5 / z); h->publish_f64(u, "vm.zy", wt + 0.5 / z); }
}
// ---------------------------------------------------------------- 0.38.24 (research): the loadout (round 61)
// SaveData.m_EquipSlotSave @0xB0 (EquipSlotSave, ObscuredInt each): melee @0x10, gun @0x24, harpoon @0x40, harpoon
// head @0x54, charms @0x68 / 0x7C / 0x90, sub-helpers @0xA4 / 0xB8. Each id -> IntegratedItem (name key @0x18, picture
// @0x38). DIAG only for now: which slot holds what, to match against the HUD's B / Y / A slots.
char g_lo_diag[900];
char g_lo_ic[9][64];   // 0.38.25: each slot's picture name (the same order as kSl)
// 0.38.29: the weapon card (user): the harpoon tip by default; a tap shows the gun and presses R once (the game's swap),
// a tap again the tip. Kept by the companion (the game's own "which weapon is out" is round 62); back to the tip off a dive.
int g_lo_side, g_lo_r_req;
uint64_t g_lo_r_at; int g_lo_r_set, g_lo_r_btn;   // 0.38.31: the button asked for (1 R: weapon, 2 L: item)
int g_lo_yside;                                   // 0.38.31: the item card (0 item 1, 1 item 2), kept by the companion
bool LoPressHeld() {   // one sample: the request turns into "held on this tick" the first time it is asked this tick
    if (g_lo_r_req) { g_lo_r_btn = g_lo_r_req; g_lo_r_req = 0; g_lo_r_at = g_ticks; g_lo_r_set = 1; }
    return g_lo_r_set && g_lo_r_at == g_ticks;
}
void LoadoutDiag(Instance& in, const Guest& g) {
    Buf b;
    const uint64_t es = in.gs ? g.ptr(in.gs + 0xB0) : 0;
    if (!es) { b.add("no EquipSlotSave"); DcCopy(g_lo_diag, sizeof g_lo_diag, b.s); return; }
    static const struct { const char* n; uint32_t o; } kSl[] = {{"melee", 0x10}, {"gun", 0x24}, {"harpoon", 0x40}, {"head", 0x54},
        {"charm1", 0x68}, {"charm2", 0x7C}, {"charm3", 0x90}, {"sub1", 0xA4}, {"sub2", 0xB8}};
    const uint64_t tbl = ItemTable(in, g);
    for (const auto& sl : kSl) {
        int32_t id = 0;
        b.add(sl.n).addc(' ');
        if (!g.obscured(es + sl.o, &id)) { b.add("? | "); continue; }
        b.num(id);
        const uint64_t it = (tbl && id > 0) ? FindItem(in, g, tbl, id) : 0;
        char key[64] = "", ic[64] = "";
        if (it) { g.str(g.ptr(it + 0x18), key, sizeof key); g.str(g.ptr(it + 0x38), ic, sizeof ic); }
        if (it) { char fb[96]; b.addc(' ').add(TextFor(key, fb, sizeof fb)).add(" [").add(ic).addc(']'); }
        DcCopy(g_lo_ic[&sl - kSl], sizeof g_lo_ic[0], it ? ic : "");
        b.add(" | ");
    }
    DcCopy(g_lo_diag, sizeof g_lo_diag, b.s);
}
// 0.38.30 (round 62): the live loadout while diving. InGameManager +0xA0 = PlayerCharacter; its m_InstanceItemInven @0x2A0
// (InstanceItemInventory): meleeHandler @0x20, harpoonHandler @0x28, gunHandler @0x30. Each handler's m_currentEquipInfo
// @0x20 (SpecDataBase); the harpoon's tip HarpoonWeaponHandler.m_HeadSpec @0xF0. The gun: _MaxAmmo @0x78, m_CurrentAmmo
// @0x7C. Which weapon is out: PlayerCharacter.rangeActionSwapRP @0x570 (ReactiveProperty<RangeActionType>, value @+0x10;
// 0 Harpoon, 1 Gun). The spec's item id: the first int in 3000000..3999999 among its first 0x80 bytes that the item
// table knows (offset kept per spec class) -- the diag line prints what was found.
char g_lv_ic[4][64];          // live pictures: melee, gun, harpoon, tip (the same order as g_lo_ic 0..3)
int32_t g_lv_tid[4];
int32_t g_lv_lvl[4];           // 0.38.38 (round 63): IntegratedItem.ItemLevel @0x14 of each live weapon (the HUD's Lv.N)
int g_lv_act = -1, g_lv_ammo = -1, g_lv_max = -1, g_lv_ok;
char g_lv_diag[700];
char g_lv_diag2[300];          // 0.38.31: the item (sub-helper) inventory probe
char g_lv_sub[2][64];          // 0.38.32: the two equipped items' pictures, live
int32_t g_lv_subn[2] = {-1, -1}; // 0.38.40: their counts (the HUD's xN), -1 unread
int g_lv_yfocus = -1;          // 0.38.35: which item is in hand (SubHelperItemInventory +0x38), -1 unread
struct LvOff { char c[48]; int off; };
LvOff g_lv_off[8]; int g_lv_noff;
int32_t SpecTid(Instance& in, const Guest& g, uint64_t tbl, uint64_t spec, Buf& d) {
    char c[48] = "";
    if (!spec || !g.cls(spec, c, sizeof c)) { d.add("-"); return 0; }
    d.add(c);
    for (int i = 0; i < g_lv_noff; ++i)
        if (eq(g_lv_off[i].c, c)) { const int32_t v = g.s32(spec + static_cast<uint64_t>(g_lv_off[i].off)); d.add("@").hex(static_cast<uint64_t>(g_lv_off[i].off)); return v; }
    int32_t w[32] = {};
    if (!g.read(spec + 0x10, w, sizeof w)) { d.add(" unreadable"); return 0; }
    for (int k = 0; k < 30; ++k) {
        if (w[k] < 3000000 || w[k] > 3999999) continue;
        if (!tbl || !FindItem(in, g, tbl, w[k])) continue;
        if (g_lv_noff < 8) { DcCopy(g_lv_off[g_lv_noff].c, sizeof g_lv_off[0].c, c); g_lv_off[g_lv_noff].off = 0x10 + 4 * k; ++g_lv_noff; }
        d.add("@").hex(static_cast<uint64_t>(0x10 + 4 * k));
        return w[k];
    }
    d.add(" ints");
    for (int k = 0; k < 8; ++k) d.addc(' ').num(w[k]);
    return 0;
}
void LiveLoadout(Instance& in, const Guest& g) {
    g_lv_ok = 0; g_lv_act = -1; g_lv_ammo = -1; g_lv_max = -1;
    for (auto& x : g_lv_ic) x[0] = 0;
    Buf d; d.add("live: ");
    char c[64];
    const uint64_t ti = g.ptr(in.main_base + kInGameManagerTypeInfo);
    const uint64_t single = ti ? g.ptr(ti + kParent) : 0;
    const uint64_t st = single ? g.ptr(single + kStatics) : 0;
    const uint64_t ig = st ? g.ptr(st) : 0;
    const uint64_t ch = ig ? g.ptr(ig + 0xA0) : 0;
    if (!ch || !g.cls(ch, c, sizeof c) || !eq(c, "PlayerCharacter")) { d.add("no PlayerCharacter"); DcCopy(g_lv_diag, sizeof g_lv_diag, d.s); return; }
    const uint64_t inv = g.ptr(ch + 0x2A0);
    {   // 0.38.32: the items. PlayerCharacter +0x2A8 = SubHelperItemInventory (0.38.31 probe): +0x20 SubHelperHandlerDictionary,
        // +0x28 SubHelperSlotData[] (the equipped items). Each slot's item id: an int (plain or ObscuredInt) the item table
        // knows, or a *SpecData pointer (SpecTid) -- lv.diag2 shows what each slot gave and the inventory's small ints
        // (to find which item is in hand).
        Buf p; p.add("items: ");
        for (auto& x : g_lv_sub) x[0] = 0;
        const uint64_t si = g.ptr(ch + 0x2A8); char sc[48] = "";
        const uint64_t tb = ItemTable(in, g);
        if (!si || !g.cls(si, sc, sizeof sc) || !eq(sc, "SubHelperItemInventory")) { p.add("no SubHelperItemInventory"); g_lv_yfocus = -1; }
        else {
            p.add("ints");
            for (uint64_t f = 0x30; f <= 0x48; f += 4) p.addc(' ').num(g.s32(si + f));
            { const int32_t fo = g.s32(si + 0x38); g_lv_yfocus = (fo == 0 || fo == 1) ? fo : -1; p.add(" | in hand ").num(fo); }
            const uint64_t arr = g.ptr(si + 0x28);
            const int32_t n = arr ? g.s32(arr + 0x18) : -1;
            p.add(" | slots ").num(n);
            for (int32_t i = 0; i < n && i < 2; ++i) {
                g_lv_subn[i] = -1;
                const uint64_t e = g.ptr(arr + 0x20 + 8 * static_cast<uint64_t>(i)); char ec[40] = "";
                p.add(" | #").num(i).addc(' ').add(e && g.cls(e, ec, sizeof ec) ? ec : "-");
                if (!e || !ec[0]) continue;
                int32_t tid = 0; uint64_t at = 0;
                for (uint64_t f = 0x10; f <= 0x48 && !tid; f += 4) {
                    const int32_t v = g.s32(e + f);
                    if (v >= 3000000 && v <= 3999999 && tb && FindItem(in, g, tb, v)) { tid = v; at = f; break; }
                    int32_t ob = 0;
                    if (g.obscured(e + f, &ob) && ob >= 3000000 && ob <= 3999999 && tb && FindItem(in, g, tb, ob)) { tid = ob; at = f | 0x1000; break; }
                }
                for (uint64_t f = 0x10; f <= 0x40 && !tid; f += 8) {
                    const uint64_t q = g.ptr(e + f); char qc[40] = "";
                    if (q > 0x1000000 && g.cls(q, qc, sizeof qc) && ends(qc, "SpecData")) { Buf t; tid = SpecTid(in, g, tb, q, t); at = f | 0x2000; }
                }
                if (!tid) { p.add(" ?"); for (uint64_t f = 0x10; f <= 0x28; f += 4) p.addc(' ').num(g.s32(e + f)); continue; }
                p.addc(' ').num(tid).add("@").hex(at);
                {   // 0.38.40: the count (the HUD's xN) -- not known yet: the field after the id (spec pointer at +0x10 ->
                    // +0x18; an ObscuredInt id -> 0x14 further), read as an ObscuredInt, else as a plain int, if 0..999.
                    // The raw ints from +0x18 go to DIAG to confirm.
                    const uint64_t nx = (at & 0x1000) ? e + (at & 0xFFF) + 0x14 : e + (at & 0xFFF) + 0x8;
                    int32_t cnt = -1, ob = -1;
                    if (g.obscured(nx, &ob) && ob >= 0 && ob <= 999) cnt = ob;
                    else { const int32_t pl = g.s32(nx); if (pl >= 0 && pl <= 999) cnt = pl; }
                    g_lv_subn[i] = cnt;
                    p.add(" n").num(cnt).add(" [");
                    for (uint64_t f = 0x18; f <= 0x34; f += 4) p.addc(' ').num(g.s32(e + f));
                    p.addc(']');
                }
                const uint64_t it = FindItem(in, g, tb, tid);
                char ic[64] = ""; if (it) g.str(g.ptr(it + 0x38), ic, sizeof ic);
                p.add(" [").add(ic).addc(']');
                DcCopy(g_lv_sub[i], sizeof g_lv_sub[0], ic);
            }
        }
        DcCopy(g_lv_diag2, sizeof g_lv_diag2, p.s);
    }
    if (!inv || !g.cls(inv, c, sizeof c) || !eq(c, "InstanceItemInventory")) { d.add("no InstanceItemInventory (").add(inv ? c : "0").addc(')'); DcCopy(g_lv_diag, sizeof g_lv_diag, d.s); return; }
    const uint64_t rp = g.ptr(ch + 0x570);
    bool ok = false; const int32_t act = rp ? g.s32(rp + 0x10, &ok) : -1;
    g_lv_act = ok && (act == 0 || act == 1) ? act : -1;
    d.add("out ").add(g_lv_act == 0 ? "HARPOON" : g_lv_act == 1 ? "GUN" : "?").add(" (").num(act).add(")");
    const uint64_t hm = g.ptr(inv + 0x20), hh = g.ptr(inv + 0x28), hg = g.ptr(inv + 0x30);
    if (hg) {
        const int32_t mx = g.s32(hg + 0x78), cu = g.s32(hg + 0x7C), add = g.s32(hg + 0x98);
        if (cu >= 0 && cu <= 999) g_lv_ammo = cu;
        if (mx >= 0 && mx <= 999) g_lv_max = mx + (add > 0 && add < 999 ? add : 0);
        d.add(" | ammo ").num(cu).addc('/').num(mx).add(" +").num(add);
        bool eok = false; uint8_t en = 0; eok = g.read(hg + 0x31, &en, 1);
        d.add(" gunOn ").num(eok ? en : -1);
    }
    if (hh) { uint8_t en = 0; if (g.read(hh + 0x31, &en, 1)) d.add(" harpoonOn ").num(en); }
    const uint64_t tbl = ItemTable(in, g);
    const uint64_t specs[4] = {hm ? g.ptr(hm + 0x20) : 0, hg ? g.ptr(hg + 0x20) : 0, hh ? g.ptr(hh + 0x20) : 0, hh ? g.ptr(hh + 0xF0) : 0};
    static const char* const kN[4] = {"melee", "gun", "harpoon", "tip"};
    for (int i = 0; i < 4; ++i) {
        d.add(" | ").add(kN[i]).addc(' ');
        const int32_t tid = SpecTid(in, g, tbl, specs[i], d);
        g_lv_tid[i] = tid; g_lv_lvl[i] = 0;
        const uint64_t it = tid > 0 && tbl ? FindItem(in, g, tbl, tid) : 0;
        if (!it) continue;
        { const int32_t lv = g.s32(it + 0x14); g_lv_lvl[i] = (lv > 0 && lv < 100) ? lv : 0; d.add(" Lv").num(g_lv_lvl[i]); }
        char ic[64] = ""; g.str(g.ptr(it + 0x38), ic, sizeof ic);
        d.addc(' ').num(tid).add(" [").add(ic).addc(']');
        DcCopy(g_lv_ic[i], sizeof g_lv_ic[0], ic);
        if (ic[0]) g_lv_ok = 1;
    }
    DcCopy(g_lv_diag, sizeof g_lv_diag, d.s);
}
// 0.38.39: HideHUD lives with the guest helper's mailbox code further down
extern bool g_hx_ok;
extern int g_hh_on;
extern char g_hh_diag[320];
void HideHudStep(const Instance& in, const Guest& g);
void Sample(void* p, const EdenDsmodHostApi* host) {
    auto& in = *static_cast<Instance*>(p);
    Guest g{host};
    ++g_ticks;
    if (in.tick % kRescanTicks == 7) ReadO2Max(in, g);
    if (in.tick % 2 == 1) UiProbeStep(in, g, host);
    if (g_bh) {
        float v = 0;
        if (g.read(g_bh + 0xA0, &v, 4) && v == g_o2_last) { if (g_o2_still < 100000) ++g_o2_still; }
        else { g_o2_still = 0; g_o2_last = v; }
    }
    PatchWatch();
    if (g_ticks % 15 == 7) {
        static int last_mode = -1;
        ScanMode(in, g);
        // the bottom screen follows the player: SUSHI on arriving at the restaurant, DIVE in the sea
        // 0.37.114: back on the boat (not diving, not at the sushi bar) -> the APPS page
        if (g_mode != last_mode) { if (g_mode == 2) g_page = kPageSushi; else if (g_mode == 1) g_page = kPageDive; else if (g_mode == 0) g_page = kPagePhone; }
        last_mode = g_mode;
    }
    host->publish_i64(host->userdata, "game.mode", g_mode);
    // 0.38.5: the Sea People Village (scene MermanVillage) -- no dive map there, depth ??? (as the game's HUD)
    const bool vill = eq(g_scene_data, "MermanVillage");   // 0.38.8: exactly (the record chamber is not the village)
    host->publish_i64(host->userdata, "scene.village", vill ? 1 : 0);
    // 0.38.10: the record chamber -- a small level of its own; the map's grid there is still the Blue Hole's
    host->publish_i64(host->userdata, "scene.rec", eq(g_scene_data, "MermanVillage_Recordroom") ? 1 : 0);
    VmKeepLayout();
    if (g_ticks % 30 == 13 && vill) VillageMapDiag(in, g);   // 0.38.7 (research)
    if (vill && (g_vm_zoomed ? g_ticks % 2 == 1 : g_ticks % 5 == 4)) VillageMapStep(in, g);   // 0.38.15: every 2 ticks zoomed (was 10: choppy)
    VillageMapPublish(host);
    if (g_ticks % 30 == 17) LoadoutDiag(in, g);   // 0.38.24 (research)
    if (g_mode == 1 && g_ticks % 15 == 4) LiveLoadout(in, g);
    if (g_mode != 1) { g_lo_side = 0; g_lv_ok = 0; g_lv_act = -1; g_lv_ammo = -1; g_lv_sub[0][0] = g_lv_sub[1][0] = 0; }
    // 0.38.30: the card follows the game's own "which weapon is out" when it reads; else the companion's own toggle
    if (g_hx_ok) HideHudStep(in, g);   // 0.38.39
    host->publish_i64(host->userdata, "hh.on", g_hh_on);
    host->publish_text(host->userdata, "hh.diag", g_hh_diag[0] ? g_hh_diag : "hide hud: helper mailbox not available");
    host->publish_i64(host->userdata, "lo.side", g_lv_act >= 0 ? g_lv_act : g_lo_side);
    { Buf a; if (g_lv_ammo >= 0) a.addc('x').num(g_lv_ammo); host->publish_text(host->userdata, "lo.ammo", a.s);
      host->publish_i64(host->userdata, "lo.ammoon", g_lv_ammo >= 0 ? 1 : 0); }
    {   // 0.38.38: the gun's and the harpoon tip's levels (Lv.N), as the HUD shows them on the A slot
        Buf a, b; const bool ok = g_lv_ok && g_mode == 1;
        if (ok && g_lv_lvl[1] > 0) a.add("Lv.").num(g_lv_lvl[1]);
        if (ok && g_lv_lvl[3] > 0) b.add("Lv.").num(g_lv_lvl[3]);
        host->publish_text(host->userdata, "lo.lvg", a.s); host->publish_i64(host->userdata, "lo.lvgon", a.n ? 1 : 0);
        host->publish_text(host->userdata, "lo.lvt", b.s); host->publish_i64(host->userdata, "lo.lvton", b.n ? 1 : 0);
    }
    { const bool hd = LoPressHeld();
      host->publish_i64(host->userdata, "lo.p_r", hd && g_lo_r_btn == 1 ? 1 : 0);
      host->publish_i64(host->userdata, "lo.p_l", hd && g_lo_r_btn == 2 ? 1 : 0); }
    if (g_mode != 1) g_lo_yside = 0;
    if (g_mode != 1) g_lv_yfocus = -1;
    for (int i = 0; i < 2; ++i) {   // 0.38.40: the items' counts, xN as the HUD shows (more than one)
        Buf a, k, ko; k.add("lo.sn").num(i + 1); ko.add("lo.sn").num(i + 1).add("on");
        const bool on = g_mode == 1 && g_lv_sub[i][0] && g_lv_subn[i] > 1;
        if (on) a.addc('x').num(g_lv_subn[i]);
        host->publish_text(host->userdata, k.s, a.s); host->publish_i64(host->userdata, ko.s, on ? 1 : 0);
    }
    host->publish_i64(host->userdata, "lo.yside", g_lv_yfocus >= 0 ? g_lv_yfocus : g_lo_yside);   // 0.38.35: the game's own when read
    host->publish_text(host->userdata, "lv.diag2", g_lv_diag2[0] ? g_lv_diag2 : "items: not read yet (dive first)");
    host->publish_text(host->userdata, "lo.diag", g_lo_diag[0] ? g_lo_diag : "loadout: not read yet");
    host->publish_text(host->userdata, "lv.diag", g_lv_diag[0] ? g_lv_diag : "live: not read yet (dive first)");
    {   // 0.38.25: the loadout strip -- each slot's picture (module:icon/<name>) and whether it holds anything
        static const char* const kk[9] = {"lo.k", "lo.g", "lo.h", "lo.hh", "lo.c1", "lo.c2", "lo.c3", "lo.s1", "lo.s2"};
        for (int i = 0; i < 9; ++i) {
            // 0.38.26: the weapons at their own shape (module:frame/; icon/ centres on a square: they came out tiny)
            // 0.38.30: while diving, the live weapons (a gun / tip picked up in a chest) over the save's
            const char* pic = (i < 4 && g_lv_ok && g_lv_ic[i][0]) ? g_lv_ic[i] : g_lo_ic[i];
            if (i >= 7 && g_lv_sub[i - 7][0]) pic = g_lv_sub[i - 7];   // 0.38.32: the live items (lo.s1 / lo.s2)
            // 0.38.33: weapons and items at the game's own scale (module:fit/: no stretching), charms as before
            // 0.38.35: the weapons padded to their card's picture box (B 101x56, A 157x56 -- gen_dive.py _LBOX), the items 1:1
            Buf t; if (pic[0]) { t.add(i < 4 || i >= 7 ? "module:fit/" : "module:icon/").add(pic); if (i == 0) t.add("@101x56"); else if (i < 4) t.add("@157x56"); }
            host->publish_text(host->userdata, kk[i], t.s);
            Buf o; o.add(kk[i]).add("on"); host->publish_i64(host->userdata, o.s, pic[0] ? 1 : 0);
        }
    }
    host->publish_text(host->userdata, "vm.diag", g_vm_diag[0] ? g_vm_diag : "village map: not in the village yet");
    if (g_ticks % 2 == 0) LoadingStep(in, g);
    host->publish_i64(host->userdata, "ld.show", (g_ld_test || g_ld_on || g_ld_title) ? 1 : 0);
    host->publish_i64(host->userdata, "ld.menu", g_ld_menu);   // 0.37.51: the loading picture while the game loads
    { Buf b; b.add("loading ").add(!g_ld_ok ? "unreadable" : g_ld_on ? "NOW" : "no").add(" x").num(g_ld_count).addc(' ');
      AddF(b, static_cast<float>(g_ld_last_ticks) / 60.0f); b.add("s | logo ");
      if (g_logo_err < 0) b.add("not drawn yet");
      else if (g_logo_err) b.add("failed ").num(g_logo_err);
      else b.num(g_logo_w).addc('x').num(g_logo_h);
      b.add(" | bg ");
      if (g_bg_err < 0) b.add("-"); else if (g_bg_err) b.add("failed ").num(g_bg_err); else b.num(g_bg_w).addc('x').num(g_bg_h);
      b.add(" | scene ").add(g_scene_name).add(g_ld_title ? " (logo)" : "");
      if (g_ld_test) b.add(" | test on");
      host->publish_text(host->userdata, "ld.diag", b.s); }
    DiscardStep(in, g);
    MenuWatch();
    MenuStep(in, g);
    if (in.tick++ % kRescanTicks == 0) Scan(in, g);
    else AdvanceSearch(in, g, 6000);   // spread the name lookup over several frames
    Publish(in, host);
    CatchToast(in, g, host);   // 0.38.40
    void* u = host->userdata;
    host->publish_i64(u, "o2.ok", g_o2.ok);
    host->publish_i64(u, "o2.max_x10", static_cast<int64_t>(g_o2.max * 10.0f + 0.5f));
    host->publish_i64(u, "o2.base_x10", static_cast<int64_t>(g_o2.base * 10.0f + 0.5f));
    host->publish_i64(u, "o2.item_x10", static_cast<int64_t>(g_o2.item * 10.0f + 0.5f));
    host->publish_i64(u, "o2.param_x100", static_cast<int64_t>(g_o2.param * 100.0f + 0.5f));
    UiProbePublish(host);
    DcPublish(host, g_ticks);
    MenuPublish(in, host);
    PublishClock(in, g, host);
    PublishGold(in, g, host);
    if (g_ticks % 10 == 7) PhoneStep(in, g);   // 0.37.65: the phone's open app
    if (g_ticks % 10 == 2) EcoStep(in, g);     // 0.37.72: EcoWatcher
    EcoPublish(host);
    if (g_ticks % 10 == 5) AllMissionsStep(in, g);   // 0.38.58: the active missions the tracker does not show (MISSIONS; points om.* -- 0.38.60: am.* were the ammo marks')
    AllMissionsPublish(host);
    if (g_ticks % 10 == 4) ShopStep(in, g);    // 0.37.89: Weapon Shop
    ShopPublish(host);
    if (g_ticks % 10 == 6) IdvStep(in, g);     // 0.37.102: iDiver
    IdvPublish(host);
    if (g_ticks % 10 == 8) CallStep(in, g);    // 0.37.109: Call
    CallPublish(host);
    PhoneJobStep(in, g);                       // 0.37.68: opening a mail on the top screen
    PhoneJobPublish(host);
    TextLookupStep(in, g);                     // 0.37.67: the apps' text keys -> words
    PhonePublish(host);
    if (g_ticks % 10 == 3) PublishMapProbe(in, g, host);
    PublishMapProbeText(host);
    PublishMap(in, g, host);
    PublishPage(host);          // 0.35.4: before the marker work, so the views never lose their switch
    if (kMarkerProbe) {   // map markers probe (0.34.x; off since 0.35.0 -- see ROADMAP)
        const uint64_t ti = g.ptr(in.main_base + kInGameManagerTypeInfo);
        const uint64_t single = ti ? g.ptr(ti + kParent) : 0;
        const uint64_t st = single ? g.ptr(single + kStatics) : 0;
        const uint64_t igm = st ? g.ptr(st) : 0;
        MarkerProbeStep(in, g, host, igm, igm && host->get_i64 && host->get_i64(u, "bh", 0) != 0);
        MissionPinStep(in, g, host);
        FishStep(in, g, igm);
        KlassFindStep(in, g);
        PickupKindStep(in, g);
        if (g_ticks % 30 == 7) MarkerProbeAddrs(in, g, host, igm);
        MarkerPublish(host);
        MissionPublish(g, host);
        MissionToast(host, g_ticks % 15 == 4);   // 0.37.6: published every tick (the snapshot is cleared each tick)
        MarkerProbePublish(host);
        FishPublish(host);
        PodAmmoPublish(host);
    }
    if (g_ticks % 60 == 5) IconDiag(in, host);
    if (g_ticks % 30 == 11) ScanMissionIcons(in, g);
    PublishMissionIcons(host);
    host->publish_text(host->userdata, "icon.diag", g_icon_diag);
    host->publish_i64(u, "o2.stage", g_o2.stage);
    host->publish_text(u, "o2.keys", g_o2keys);
    host->publish_i64(u, "o2.idx", g_o2.idx);
    host->publish_i64(u, "o2.listn", g_o2.listn);
}

// ---------------------------------------------------------------- discard (round 12)
// The companion drives the game's own menu, never its memory: it asks the host for button presses
// (manifest "button" actions, gated on dc.p_* and fired by enforce rules while dc.busy is 1) and
// checks every step on screen first. One press = the request published for one sample (the
// enforce rules run every tick).
//   + (unless the Acquired list already has focus) -> Down x row -> A -> the popup must name the
//   chosen catch ("<color=yellow>Blue Tang</color> is selected. Discard?") -> A; otherwise B
//   (cancel) and try the next row down. Done when the cargo shrinks; then B closes the menu.
enum DcBtn { kBtnNone = 0, kBtnA, kBtnB, kBtnPlus, kBtnDown };
enum DcStep { kDcIdle = 0, kDcOpen, kDcMove, kDcAsk, kDcCheck, kDcCancel, kDcNext, kDcConfirm, kDcClose,
              kDcSend, kDcWait, kDcVerify };
struct Discard {
    int step;
    int pick;                 // CARGO row picked (-1 none)
    char pick_name[48];
    char pick_icon[64];       // its picture (the game's icon name)
    int btn; uint64_t btn_until;   // the press being requested and until which sample
    uint64_t wait_until, deadline;
    int moves_left, tries;
    int opened;               // this job pressed + itself
    uint64_t asked;           // sample A was pressed for the popup
    int rows_before;
    uint64_t text_before;     // popup text string object before pressing A (a new one = popup shown)
    char seen[48];            // name the popup showed
    char status[96];
    int result;               // 0 none, 1 done, 2 stopped
    uint64_t session;
    // menu-free discard (round 17): what the game's own Discard passes, captured at the tap
    int32_t id, grade, fgrade, lift; char time[32];
    uint32_t seq; int alt;
};
Discard g_dc;

void DcStatus(const char* a, const char* b, const char* c);
bool MbStore(uint32_t off, uint32_t v);
void AbortDiscard() {
    if (g_dc.step == kDcWait) MbStore(8, g_dc.seq);   // withdraw a request the game has not taken yet
    if (g_dc.step != 0) {   // the host re-created the module mid-job: stop, never resume blind
        g_dc.step = 0; g_dc.btn = 0; g_dc.result = 2; g_dc.pick = -1;
        DcStatus("Stopped: the companion restarted; try again", "", "");
    }
}
uint64_t g_dc_status_tick;        // when the status line last changed (it fades 4 s after the job)
void DcStatus(const char* a, const char* b = "", const char* c = "") {
    g_dc_status_tick = g_ticks;
    Buf s; s.add(a).add(b).add(c);
    int n = s.n < static_cast<int>(sizeof g_dc.status) - 1 ? s.n : static_cast<int>(sizeof g_dc.status) - 1;
    memcpy(g_dc.status, s.s, static_cast<size_t>(n)); g_dc.status[n] = 0;
}
void DcCopy(char* dst, int cap, const char* src) { int i = 0; for (; src[i] && i < cap - 1; ++i) dst[i] = src[i]; dst[i] = 0; }

uint64_t UiTextObj(const Guest& g, uint64_t uidatatext) {   // the TMP m_text string object
    uint64_t list = uidatatext ? g.ptr(uidatatext + 0x48) : 0;
    uint64_t items = list ? g.ptr(list + 0x10) : 0;
    uint64_t el = items ? g.ptr(items + 0x20) : 0;
    uint64_t ez = el ? g.ptr(el + 0x18) : 0;
    uint64_t tmp = ez ? g.ptr(ez + 0x30) : 0;
    return tmp ? g.ptr(tmp + 0xD8) : 0;
}
// "<color=yellow>Blue Tang</color> is selected. Discard?" -> "Blue Tang"
void PopupName(const char* t, char* out, int cap) {
    int i = 0, n = 0;
    if (t[0] == '<') { while (t[i] && t[i] != '>') ++i; if (t[i]) ++i; }
    while (t[i] && t[i] != '<' && n < cap - 1) {
        if (t[i] == ' ' && t[i + 1] == 'i' && t[i + 2] == 's' && t[i + 3] == ' ') break;
        out[n++] = t[i++];
    }
    out[n] = 0;
}

void DcPress(int btn, uint64_t tick, int settle) {
    // held for exactly one sample: the enforce rules run every tick, so that is one press
    g_dc.btn = btn; g_dc.btn_until = tick + 1; g_dc.wait_until = tick + 2 + static_cast<uint64_t>(settle);
}
// Stopping: if this job opened the menu itself, close it again (B) so the dive carries on.
void DcStop(const char* why, const char* extra = "") {
    DcStatus("Stopped: ", why, extra); g_dc.result = 2;
    g_dc.step = g_dc.opened ? kDcClose : kDcIdle;
}

// ---- discard without the menu (round 17): Eden's dual-screen mailbox + the package's load plan
// (plans/<build>.json, tools/dtd/discard_patch). The patch makes InGameManager.LateUpdate look at
// the mailbox every frame and call LootBox.Remove_Impl(id, 1, grade, lift, true, time) -- what the
// menu's DumpCell calls -- on the game's own thread. Mailbox: +0 magic, +4 request seq, +8 done
// seq, +0xC requests run, +0x10 LootBox*, +0x18 id, +0x1C grade, +0x20 lift, +0x24 count,
// +0x28 time (DateTime ticks), +0x30 heartbeat (counted every frame the patch runs).
EdenDsmodHostExtensions g_hx; bool g_hx_ok;
constexpr uint32_t kMbMagic = 0x51445444;   // "DTDQ"
constexpr uint64_t kLootBoxTypeInfo = 0x9DB1EA8;
uint32_t g_hb_last; uint64_t g_hb_tick; bool g_hb_seen;
bool MbLoad(uint32_t off, uint32_t* v) { return g_hx_ok && g_hx.load_u32 && g_hx.load_u32(g_hx.userdata, off, v); }
bool MbStore(uint32_t off, uint32_t v) { return g_hx_ok && g_hx.store_u32 && g_hx.store_u32(g_hx.userdata, off, v); }
bool MbStore64(uint32_t off, uint64_t v) { return g_hx_ok && g_hx.store_u64 && g_hx.store_u64(g_hx.userdata, off, v); }
void PatchWatch() {
    uint32_t hb;
    if (MbLoad(0x30, &hb) && hb != g_hb_last) { g_hb_last = hb; g_hb_tick = g_ticks; g_hb_seen = true; }
}
bool PatchLive() { return g_hb_seen && g_ticks - g_hb_tick < 30; }   // the game ran the patch in the last half second
// ---------------------------------------------------------------- 0.38.39: HideHUD (rounds 63 / 64; user spec)
// An optional switch (APPS page, off at start): the top screen's gauge panel, weapon slots, mission list and catch /
// pickup pop-ups are scaled to 0 by the guest helper (stub.S, InGameManager.LateUpdate) -- their pictures, their logic
// and the game's own show / hide are untouched, so the companion's pictures (read from the same sheets) stay as they are.
// The pieces (MainCanvasManager fields): +0x150 PlayerStateInfoPanelUI (O2 / depth), +0x1C0 LootsInfoPanel (the weight
// under it), +0x190 ActionSwitchUI (B / Y / A), +0x78 MissionHUD, +0x158 GetInfoPanelUI (catch plate),
// +0x1E0 GetPickupItemInfoPanel (weapon / ammo pickup). Mailbox entries (0x30 bytes from +0x100): +0 component,
// +8 its class, +0x10 mode (0 skip, 1 read the scale, 2 set it), +0x14 frames done, +0x18.. scale read, +0x24.. scale set.
struct HhPiece { uint32_t off; const char* cls; const char* tag; };
constexpr int kHhN = 8;   // 0.38.56: + the Sea People Village's Bei (+0x140) and Credit (+0x148) panels (round 65; user)
const HhPiece kHh[kHhN] = {{0x150, "PlayerStateInfoPanelUI", "O2"}, {0x1C0, "LootsInfoPanel", "kg"}, {0x190, "ActionSwitchUI", "slots"},
                        {0x78, "MissionHUD", "missions"}, {0x158, "GetInfoPanelUI", "catch"}, {0x1E0, "GetPickupItemInfoPanel", "pickup"},
                        {0x140, "BeiInfoPanelUI", "bei"}, {0x148, "TrustInfoPanelUI", "credit"}};
struct HhState { uint64_t obj; float orig[3]; int have_orig; uint32_t frames_at; int phase; };   // phase 0 idle, 1 reading, 2 hidden, 3 restoring
HhState g_hh[kHhN];
int g_hh_on;                 // the switch
char g_hh_diag[320];
float HhF(uint32_t off) { uint32_t v = 0; MbLoad(off, &v); float f; memcpy(&f, &v, 4); return f; }
void HhSetF(uint32_t off, float f) { uint32_t v; memcpy(&v, &f, 4); MbStore(off, v); }
void HideHudStep(const Instance& in, const Guest& g) {
    Buf d; d.add("hide hud ").add(g_hh_on ? "ON" : "off").add(PatchLive() ? " | helper live" : " | helper not running");
    const uint64_t mcm = MainCanvas(in, g);
    if (!mcm) d.add(" | no MainCanvasManager");
    // the helper runs the HideHUD entries only with the magic word set (discard sets it with its first request; the
    // magic alone does nothing for discard: that waits for its own request number)
    bool busy = g_hh_on != 0;
    for (const auto& h : g_hh) busy = busy || h.phase != 0;
    if (busy) MbStore(0, kMbMagic);
    for (int i = 0; i < kHhN; ++i) {
        HhState& h = g_hh[i];
        const uint32_t e = 0x100 + 0x30 * static_cast<uint32_t>(i);
        char c[48] = "";
        const uint64_t o = mcm ? g.ptr(mcm + kHh[i].off) : 0;
        const bool alive = o && g.cls(o, c, sizeof c) && eq(c, kHh[i].cls) && g.ptr(o + 0x10);
        uint32_t frames = 0; MbLoad(e + 0x14, &frames);
        d.add(" | ").add(kHh[i].tag).addc(' ');
        if (!alive) {   // gone (another scene): stop using the entry; a new object starts over
            if (h.obj) { MbStore(e + 0x10, 0); h = HhState{}; }
            d.add("-"); continue;
        }
        if (h.obj != o) {   // a new object: forget the old one's scale
            MbStore(e + 0x10, 0); h = HhState{}; h.obj = o;
            MbStore64(e, o); MbStore64(e + 8, g.ptr(o));
        }
        if (g_hh_on) {
            if (!h.have_orig) {
                if (h.phase != 1) { h.phase = 1; h.frames_at = frames; MbStore(e + 0x10, 1); }
                else if (frames != h.frames_at) {   // the helper read it: keep it (a piece caught mid-animation at ~0 -> 1)
                    for (int k = 0; k < 3; ++k) { const float v = HhF(e + 0x18 + 4 * static_cast<uint32_t>(k)); h.orig[k] = (v > 0.01f && v < 100.0f) ? v : 1.0f; }
                    h.have_orig = 1;
                }
            }
            if (h.have_orig && h.phase != 2) {
                for (int k = 0; k < 3; ++k) HhSetF(e + 0x24 + 4 * static_cast<uint32_t>(k), 0.0f);
                MbStore(e + 0x10, 2); h.phase = 2;
            }
        } else if (h.phase == 2 || h.phase == 3) {   // switched off: put the scale back, then leave it alone
            if (h.phase == 2) {
                for (int k = 0; k < 3; ++k) HhSetF(e + 0x24 + 4 * static_cast<uint32_t>(k), h.have_orig ? h.orig[k] : 1.0f);
                MbStore(e + 0x10, 2); h.phase = 3; h.frames_at = frames;
            } else if (frames - h.frames_at >= 3) {
                MbStore(e + 0x10, 0); h.phase = 0; h.have_orig = 0;
            }
        } else if (h.phase == 1) {
            MbStore(e + 0x10, 0); h.phase = 0;
        }
        d.add(h.phase == 2 ? (g_hh_on ? "hidden" : "restoring") : h.phase == 1 ? "reading" : "shown");
        if (h.have_orig) { d.add(" ("); AddF(d, h.orig[0]); d.addc(')'); }
    }
    DcCopy(g_hh_diag, sizeof g_hh_diag, d.s);
}
// LootBox (SingletonNoMono<LootBox>): TypeInfo -> parent +0x58 -> statics +0xB8 -> instance +0
uint64_t LootBoxInstance(const Instance& in, const Guest& g) {
    uint64_t cls = g.ptr(in.main_base + kLootBoxTypeInfo);
    uint64_t parent = cls ? g.ptr(cls + kParent) : 0;
    uint64_t st = parent ? g.ptr(parent + kStatics) : 0;
    uint64_t lb = st ? g.ptr(st) : 0;
    char c[32];
    return lb && g.cls(lb, c, sizeof c) && eq(c, "LootBox") ? lb : 0;
}
// "10/10/2026 15:07:41" (how the game wrote the catch time) -> DateTime ticks; 0 if not that shape.
// The game turns the ticks back into that text and removes the catch with exactly that time.
uint64_t TimeTicks(const char* s) {
    int v[6] = {0, 0, 0, 0, 0, 0}, k = 0, digits = 0;
    for (int i = 0; s[i] && k < 6; ++i) {
        if (s[i] >= '0' && s[i] <= '9') { v[k] = v[k] * 10 + (s[i] - '0'); ++digits; }
        else if (digits) { ++k; digits = 0; }
    }
    if (digits) ++k;
    const int mo = v[0], d = v[1], y = v[2], hh = v[3], mi = v[4], ss = v[5];
    if (k != 6 || mo < 1 || mo > 12 || d < 1 || d > 31 || y < 1 || hh > 23 || mi > 59 || ss > 59) return 0;
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    const int64_t y1 = y - 1;
    int64_t days = y1 * 365 + y1 / 4 - y1 / 100 + y1 / 400 + cum[mo - 1] + (leap && mo > 2 ? 1 : 0) + (d - 1);
    return static_cast<uint64_t>(days * 86400 + hh * 3600 + mi * 60 + ss) * 10000000ull;
}

void DiscardStep(Instance& in, const Guest& g) {
    Discard& d = g_dc;
    const uint64_t t = g_ticks;
    if (d.session != in.main_base) { memset(&d, 0, sizeof d); d.pick = -1; d.session = in.main_base; }
    if (d.step == kDcIdle || t < d.wait_until) return;
    if (g_ld_on && d.step != kDcWait) { DcStop("the game is loading"); d.step = kDcIdle; return; }   // 0.38.1
    if (d.step == kDcSend) {
        const uint64_t lb = LootBoxInstance(in, g);
        uint32_t seq = 0;
        if (!lb || !MbLoad(4, &seq)) {   // cannot ask the game directly: use its menu
            d.step = kDcOpen; d.wait_until = 0; DcStatus("Opening the menu..."); return;
        }
        d.seq = seq + 1;
        const bool ok = MbStore64(0x10, lb) && MbStore(0x18, static_cast<uint32_t>(d.id)) &&
                        MbStore(0x1C, static_cast<uint32_t>(d.alt ? d.fgrade : d.grade)) &&
                        MbStore(0x20, static_cast<uint32_t>(d.lift)) && MbStore(0x24, 1) &&
                        MbStore64(0x28, TimeTicks(d.time)) && MbStore(0, kMbMagic) && MbStore(4, d.seq);  // seq last
        if (!ok) { DcStop("could not reach the game"); d.step = kDcIdle; return; }
        DcStatus("Discarding ", d.pick_name, "...");
        d.rows_before = in.nrows; d.deadline = t + 120; d.step = kDcWait;
        return;
    }
    if (d.step == kDcWait) {
        uint32_t done = 0;
        if (MbLoad(8, &done) && done == d.seq) { d.step = kDcVerify; d.deadline = t + 75; return; }
        if (t > d.deadline) {
            MbStore(8, d.seq);   // withdraw it, so a later dive does not act on it
            DcStop("the game did not take the request"); d.step = kDcIdle;
        }
        return;
    }
    if (d.step == kDcVerify) {
        if (in.nrows < d.rows_before) {
            DcStatus("Discarded ", d.pick_name); d.result = 1; d.pick = -1; d.step = kDcIdle;
            return;
        }
        if (t > d.deadline) {
            if (!d.alt && d.fgrade != d.grade) { d.alt = 1; d.step = kDcSend; return; }   // the slot may be keyed by the final grade
            DcStop("the game kept the catch"); d.step = kDcIdle;
        }
        return;
    }
    UiProbe& u = g_ui;
    if (!u.panel && d.step != kDcOpen && d.step != kDcClose) { DcStop("lost the Acquired list"); return; }
    switch (d.step) {
    case kDcOpen:
        if (!u.panel) {   // not found yet (first discard of the session): open the menu, then wait for it
            if (!d.deadline) {
                DcStatus("Opening the menu and finding the Acquired list...");
                DcPress(kBtnPlus, t, 30); d.deadline = t + 60 * 45; d.opened = 1;   // (opened: see below)
                if (u.state != 1) u.retry_tick = 0;   // search again right away
                break;
            }
            if (t > d.deadline) DcStop("could not find the Acquired list");
            break;
        }
        if (d.opened) {   // pressed + ourselves: the menu is open once the list takes focus
            if (u.focused) { d.step = kDcMove; d.wait_until = t + 10; break; }
            if (t > d.deadline) DcStop("the menu did not open");
            break;
        }
        // Is the menu already open? The list's focus flag stays 1 after the menu closes, so it
        // only says "closed" (0) reliably. O2 tells the rest: a change proves the game is running
        // (menu closed); no change for 5 s means paused (menu open). In between, keep watching.
        {
            const bool open = u.focused && (!g_bh || g_o2_still >= kOpenStill);
            const bool closed = !u.focused || (g_bh && g_o2_still < kRunningStill);
            if (open) { d.step = kDcMove; d.wait_until = t + 10; break; }
            if (closed) {
                DcStatus("Opening the menu..."); DcPress(kBtnPlus, t, 30); d.deadline = t + 180; d.opened = 1;
                break;
            }
            if (!d.deadline) { DcStatus("Checking whether the game menu is open..."); d.deadline = t + kOpenStill + 120; }
            if (t > d.deadline) DcStop("could not tell whether the menu is open");
        }
        break;
    case kDcMove:
        if (!u.focused) { DcStop("the Acquired list is not selected"); break; }
        if (d.moves_left > 0) { --d.moves_left; DcPress(kBtnDown, t, 8); break; }
        d.step = kDcAsk;
        break;
    case kDcAsk:
        d.text_before = UiTextObj(g, g.ptr(u.panel + 0x78));
        DcStatus("Checking the selected catch...");
        DcPress(kBtnA, t, 10); d.deadline = t + 2 + 60; d.step = kDcCheck; d.asked = t;
        break;
    case kDcCheck: {
        uint64_t now = UiTextObj(g, g.ptr(u.panel + 0x78));
        // A new string object = the popup was filled in. The same object can also be a popup for a
        // catch with the same name as last time (TextMeshPro keeps an identical text), so after a
        // moment that counts too when it names the chosen catch and the game is paused.
        bool same_ok = false;
        if (now && now == d.text_before && t >= d.asked + 25) {
            char txt[96], nm[48]; g.str(now, txt, sizeof txt); PopupName(txt, nm, sizeof nm);
            same_ok = eq(nm, d.pick_name);
        }
        if (now && (now != d.text_before || same_ok)) {
            char txt[96]; g.str(now, txt, sizeof txt);
            PopupName(txt, d.seen, sizeof d.seen);
            if (eq(d.seen, d.pick_name)) {
                DcStatus("Discarding ", d.pick_name, "...");
                d.rows_before = in.nrows;
                DcPress(kBtnA, t, 10); d.deadline = t + 12 + 180; d.step = kDcConfirm;
            } else {
                d.step = kDcCancel;
            }
            break;
        }
        if (t > d.deadline) DcStop("no Discard? popup appeared");
        break;
    }
    case kDcCancel:   // wrong catch selected: cancel the popup, then look one row further down
        DcPress(kBtnB, t, 15);
        d.step = ++d.tries <= in.nrows ? kDcNext : kDcClose;
        if (d.step == kDcClose) DcStatus("Stopped: ", d.pick_name, " not found in the list");
        break;
    case kDcNext:
        DcPress(kBtnDown, t, 10); d.step = kDcAsk;
        break;
    case kDcConfirm:
        if (in.nrows < d.rows_before) {
            DcStatus("Discarded ", d.pick_name); d.result = 1;
            // the game moves focus away from the emptied list and plays its own animation first;
            // a B too early is swallowed (seen on device: the menu stayed open)
            d.pick = -1; d.step = kDcClose; d.wait_until = t + 45;
            break;
        }
        if (t > d.deadline) DcStop("pressed Discard but the cargo did not change");
        break;
    case kDcClose:
        DcPress(kBtnB, t, 10);
        d.step = kDcIdle;
        if (!d.result) d.result = 2;
        break;
    }
}

bool DcAction(Instance& in, const char* action, int64_t arg) {
    Discard& d = g_dc;
    if (eq(action, "dc_pick")) {
        if (d.step != kDcIdle || arg < 0 || arg >= in.nrows) return false;
        const Item& it = in.items[in.rows[arg].item];
        if (!it.has_info) return false;
        d.pick = static_cast<int>(arg); DcCopy(d.pick_name, sizeof d.pick_name, it.name); DcCopy(d.pick_icon, sizeof d.pick_icon, it.icon);
        d.result = 0; DcStatus("Discard ", d.pick_name, "?");
        return true;
    }
    if (eq(action, "dc_cancel")) {
        if (d.step != kDcIdle) return false;
        d.pick = -1; d.status[0] = 0; d.result = 0;
        return true;
    }
    if (eq(action, "dc_go")) {
        if (d.step != kDcIdle || d.pick < 0) return false;
        if (d.pick >= in.nrows || !eq(in.items[in.rows[d.pick].item].name, d.pick_name)) {
            d.pick = -1; DcStatus("The cargo changed; pick the catch again"); return false;
        }
        d.moves_left = in.rows[d.pick].pos; d.tries = 0; d.deadline = 0; d.result = 0; d.seen[0] = 0; d.opened = 0;
        d.wait_until = 0;
        const Row& r = in.rows[d.pick]; const Item& it = in.items[r.item];
        d.id = it.id; d.grade = it.grade; d.fgrade = it.fgrade; d.lift = it.lift; d.alt = 0;
        DcCopy(d.time, sizeof d.time, r.time);
        d.step = PatchLive() ? kDcSend : kDcOpen;   // straight to the game when the patch is running
        return true;
    }
    return false;
}

bool PhoneJobBusy();
void DcPublish(const EdenDsmodHostApi* h, uint64_t tick) {
    void* u = h->userdata;
    const Discard& d = g_dc;
    const bool held = d.btn != kBtnNone && tick < d.btn_until;
    h->publish_i64(u, "dc.busy", d.step != kDcIdle || held || PhoneJobBusy() || LoPressHeld() ? 1 : 0);   // the enforce gate (0.37.68: + the phone job)
    h->publish_i64(u, "dc.p_a", held && d.btn == kBtnA ? 1 : 0);
    h->publish_i64(u, "dc.p_b", held && d.btn == kBtnB ? 1 : 0);
    h->publish_i64(u, "dc.p_plus", held && d.btn == kBtnPlus ? 1 : 0);
    h->publish_i64(u, "dc.p_down", held && d.btn == kBtnDown ? 1 : 0);
    h->publish_i64(u, "dc.picked", d.pick >= 0 && d.step == kDcIdle ? 1 : 0);
    h->publish_i64(u, "dc.pick", d.pick);
    { Buf t; h->publish_text(u, "dc.pick_name", NoApos(d.pick_name, t)); }
    {   // the picked catch's picture, for the confirm prompt (same source form as row.isrc)
        Buf src;
        if (d.pick >= 0 && d.pick_icon[0]) {
            src.add("module:icon/");
            for (int k = 0; d.pick_icon[k]; ++k) {
                char ch = d.pick_icon[k];
                bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
                src.addc(ok ? ch : '_');
            }
        }
        h->publish_text(u, "dc.pick_isrc", src.s);
        h->publish_i64(u, "dc.has_icon", d.pick >= 0 && d.pick_icon[0] ? 1 : 0);
    }
    { Buf t; h->publish_text(u, "dc.status", NoApos(d.status, t)); }
    h->publish_i64(u, "dc.has_status", d.status[0] && (d.step != kDcIdle || tick - g_dc_status_tick < 240) ? 1 : 0);
    h->publish_i64(u, "dc.result", d.result);
    h->publish_text(u, "dc.seen", d.seen);
    h->publish_i64(u, "dc.step", d.step);
    h->publish_i64(u, "dc.paused", GamePaused() ? 1 : 0);
    h->publish_i64(u, "dc.o2_still", g_o2_still);
    h->publish_i64(u, "dc.direct", PatchLive() ? 1 : 0);
    uint32_t runs = 0; MbLoad(0xC, &runs);
    Buf m; m.add(PatchLive() ? "discard: straight to the game" : g_hx_ok ? "discard: via the menu (patch not running)" : "discard: via the menu (no patch)");
    m.add(", heartbeat ").num(g_hb_last).add(", requests run ").num(runs);
    h->publish_text(u, "dc.mode", m.s);
}

// ---- tonight's menu from the bottom screen (round 22). The package's load plan also makes
// SushiBarManager.Update (every frame at the sushi bar) look at the mailbox and call
// SushiBarMenuManager.AddMenu(slot, recipe, &bonus, Place.Main, plates) -- what the game's own "Add"
// calls (RecipeScrollerSheet): it checks the storage, fills the slot, uses the ingredients,
// refreshes the game's menu screen and saves. Mailbox: +0x40 heartbeat, +0x44 request seq, +0x48 done
// seq, +0x4C requests run, +0x50 SushiBarMenuManager*, +0x58 slot, +0x5C recipe, +0x60 bonus (out),
// +0x64 plates, +0x68 AddMenu's result.
// SushiBarMenuManager is a SingletonNoMono: the game's own Method$SingletonNoMono<..>.get_Instance()
// slot (initialised once the game has used it) -> MethodInfo.klass @0x18 -> statics -> instance.
// Its m_Menu @0x10 is a Dictionary<int, SushibarMenuSlot> (slotID 0x10, recipeID 0x14, plates now
// 0x18 / max 0x1C, unlocked 0x28, place 0x30); main slots are 1000.., branch 2000.., system 9999999.
constexpr uint64_t kMenuMgrGetInstance = 0x9E0E530;
enum { kMaIdle, kMaSend, kMaWait, kMaVerify };
enum { kMkAdd, kMkRemove, kMkSwap };                 // what the request does (mailbox +0x6C)
struct MenuAdd { int step; uint32_t seq; int32_t recipe; int32_t slot; int32_t before; int amount; uint64_t deadline;
                 char name[48]; char status[96]; uint64_t session; int kind; char old_name[48]; };
MenuAdd g_ma;
// the game's pink banner with the dish's name, for a moment after it goes on the menu
constexpr uint64_t kSplashTicks = 75;             // 1.25 s (0.37.1: was 2.5 s; closing the panel ends it too)
uint64_t g_splash_until; char g_splash_name[48];
void Splash(const char* name) { DcCopy(g_splash_name, sizeof g_splash_name, name); g_splash_until = g_ticks + kSplashTicks; }
int g_menu_amt = 1;                                   // plates to add
// Tonight's main menu as the game's SushiBarMenuManager holds it (for SWAP): up to 4 slots in use.
constexpr int kMenuSlots = 8;
struct MenuSlotView { int32_t sid, rid, plates; };   // tonight's main menu: 4 slots, more as the bar ranks up
MenuSlotView g_ms[kMenuSlots]; int g_nms;
bool g_swap_open; uint64_t g_rm_armed_until;          // SWAP chooser open; REMOVE asked once (tap again)
uint32_t g_hb2_last; uint64_t g_hb2_tick; bool g_hb2_seen;
void MenuWatch() {
    uint32_t hb;
    if (MbLoad(0x40, &hb) && hb != g_hb2_last) { g_hb2_last = hb; g_hb2_tick = g_ticks; g_hb2_seen = true; }
}
bool MenuLive() { return g_hb2_seen && g_ticks - g_hb2_tick < 30; }   // the sushi bar ran the patch lately
// 0 = found; 1 = the game has not resolved the slot yet (its own menu not opened this session);
// 2 = resolved but no manager behind it.
int g_mm_why;
uint64_t MenuManager(const Instance& in, const Guest& g) {
    const uint64_t mi = g.ptr(in.main_base + kMenuMgrGetInstance);
    if (!mi || (mi & 1)) { g_mm_why = 1; return 0; }    // not resolved by the game yet
    char c[32];
    const uint64_t koffs[2] = {0x20, 0x18};             // MethodInfo.klass (v31: 0x20), as DataManager
    for (uint64_t koff : koffs) {
        const uint64_t k = g.ptr(mi + koff);
        if (!k || !g.cstr(g.ptr(k + 0x10), c, sizeof c) || !eq(c, "SingletonNoMono`1")) continue;
        const uint64_t st = g.ptr(k + kStatics), mgr = st ? g.ptr(st) : 0;   // <s_Instance> @0x0
        if (mgr && g.cls(mgr, c, sizeof c) && eq(c, "SushiBarMenuManager")) { g_mm_why = 0; return mgr; }
    }
    g_mm_why = 2;
    return 0;
}
// The slot to fill: the recipe's own slot when it is already on the menu (more plates), else the
// open main slot with the lowest id. -1 = none. *plates = plates of that recipe now on the menu.
int32_t MenuSlotFor(const Guest& g, uint64_t mgr, int32_t recipe, int32_t* plates) {
    *plates = 0;
    const uint64_t d = g.ptr(mgr + 0x10), en = d ? g.ptr(d + 0x18) : 0;
    const int32_t cnt = d ? g.s32(d + 0x20) : 0;
    int32_t best = -1;
    for (int32_t i = 0; en && i < cnt && i < 256; ++i) {
        const uint64_t e = en + 0x20 + static_cast<uint64_t>(i) * 0x18, v = g.ptr(e + 0x10);
        if (g.s32(e) < 0 || !v) continue;
        const int32_t sid = g.s32(v + 0x10), rid = g.s32(v + 0x14), place = g.s32(v + 0x30);
        uint8_t unlock = 0; g.read(v + 0x28, &unlock, 1);
        if (sid < 1000 || sid >= 2000 || place != 0) continue;
        if (rid == recipe && recipe > 0) { *plates = g.s32(v + 0x18); return sid; }
        if (rid == 0 && unlock && (best < 0 || sid < best)) best = sid;
    }
    return best;
}
void MenuReadSlots(const Guest& g, uint64_t mgr) {
    g_nms = 0;
    const uint64_t d = mgr ? g.ptr(mgr + 0x10) : 0, en = d ? g.ptr(d + 0x18) : 0;
    const int32_t cnt = d ? g.s32(d + 0x20) : 0;
    MenuSlotView tmp[kMenuSlots]; int n = 0;
    for (int32_t i = 0; en && i < cnt && i < 256 && n < kMenuSlots; ++i) {
        const uint64_t e = en + 0x20 + static_cast<uint64_t>(i) * 0x18, v = g.ptr(e + 0x10);
        if (g.s32(e) < 0 || !v) continue;
        const int32_t sid = g.s32(v + 0x10), rid = g.s32(v + 0x14), place = g.s32(v + 0x30);
        if (sid < 1000 || sid >= 2000 || place != 0 || rid <= 0) continue;
        tmp[n++] = {sid, rid, g.s32(v + 0x18)};
    }
    for (int a = 1; a < n; ++a)                       // slot order, as the game lists them
        for (int b = a; b > 0 && tmp[b].sid < tmp[b - 1].sid; --b) { MenuSlotView t = tmp[b]; tmp[b] = tmp[b - 1]; tmp[b - 1] = t; }
    for (int k = 0; k < n; ++k) g_ms[k] = tmp[k];
    g_nms = n;
}
int32_t MenuSlotOfRecipe(int32_t recipe) {
    for (int k = 0; k < g_nms; ++k) if (g_ms[k].rid == recipe) return g_ms[k].sid;
    return -1;
}
template <typename... T> void MaStatus(T... parts) {
    Buf b; (b.add(parts), ...);
    DcCopy(g_ma.status, sizeof g_ma.status, b.s);
}
void MenuStep(Instance& in, const Guest& g) {
    MenuAdd& a = g_ma;
    const uint64_t t = g_ticks;
    if (a.session != in.main_base) { memset(&a, 0, sizeof a); a.session = in.main_base; }
    if (a.step == kMaSend) {
        const uint64_t mgr = MenuManager(in, g);
        uint32_t seq = 0;
        if (!MenuLive() || g_mode != 2) { MaStatus("Go to the sushi bar to add it to the menu"); a.step = kMaIdle; return; }
        if (!mgr) {
            MaStatus(g_mm_why == 1 ? "Open the game's Menu once, then try again" : "Could not find the game's menu");
            a.step = kMaIdle; return;
        }
        if (!MbLoad(0x44, &seq)) { MaStatus("Could not reach the game"); a.step = kMaIdle; return; }
        int32_t plates = 0;
        MenuReadSlots(g, mgr);
        if (a.kind == kMkAdd) {
            a.slot = MenuSlotFor(g, mgr, a.recipe, &plates);
            if (a.slot < 0) { MaStatus("No free menu slot - use SWAP"); a.step = kMaIdle; return; }
        } else if (a.kind == kMkRemove) {
            a.slot = MenuSlotOfRecipe(a.recipe);
            if (a.slot < 0) { MaStatus(a.name, " is not on the menu"); a.step = kMaIdle; return; }
        } else {                                       // swap: a.slot was chosen; it must still hold a dish
            if (MenuSlotOfRecipe(a.recipe) >= 0) { MaStatus(a.name, " is already on the menu"); a.step = kMaIdle; return; }
            bool held = false;
            for (int k = 0; k < g_nms; ++k) if (g_ms[k].sid == a.slot) held = true;
            if (!held) { MaStatus("That menu slot changed - try again"); a.step = kMaIdle; return; }
        }
        a.before = plates; a.seq = seq + 1;
        const bool ok = MbStore64(0x50, mgr) && MbStore(0x58, static_cast<uint32_t>(a.slot)) &&
                        MbStore(0x5C, static_cast<uint32_t>(a.recipe)) && MbStore(0x60, 0) &&
                        MbStore(0x64, static_cast<uint32_t>(a.amount)) && MbStore(0x68, 0) &&
                        MbStore(0x6C, static_cast<uint32_t>(a.kind)) && MbStore(0x70, 0) &&
                        MbStore(0, kMbMagic) && MbStore(0x44, a.seq);   // seq last
        if (!ok) { MaStatus("Could not reach the game"); a.step = kMaIdle; return; }
        if (a.kind == kMkRemove) MaStatus("Removing ", a.name, "...");
        else if (a.kind == kMkSwap) MaStatus("Swapping ", a.old_name, " for ", a.name, "...");
        else MaStatus("Adding ", a.name, "...");
        a.step = kMaWait; a.deadline = t + 120;
        return;
    }
    if (a.step == kMaWait) {
        uint32_t done = 0, res = 0;
        if (MbLoad(0x48, &done) && done == a.seq) {
            uint32_t rres = 0;
            MbLoad(0x68, &res); MbLoad(0x70, &rres);
            if (a.kind != kMkAdd && !(rres & 0xFF)) { MaStatus("The game did not remove ", a.kind == kMkSwap ? a.old_name : a.name); a.step = kMaIdle; return; }
            if (a.kind == kMkSwap && !(res & 0xFF)) { MaStatus("Removed ", a.old_name, " but could not add ", a.name); a.step = kMaIdle; return; }
            if (a.kind == kMkAdd && !(res & 0xFF)) { MaStatus("The game did not add ", a.name, " (not enough ingredients?)"); a.step = kMaIdle; return; }
            a.step = kMaVerify; a.deadline = t + 60;
            return;
        }
        if (t > a.deadline) { MbStore(0x48, a.seq); MaStatus("The game did not take the request"); a.step = kMaIdle; }
        return;
    }
    if (a.step == kMaVerify) {
        const uint64_t mgr = MenuManager(in, g);
        if (a.kind == kMkAdd) {
            int32_t plates = 0;
            if (mgr) MenuSlotFor(g, mgr, a.recipe, &plates);
            if (plates > a.before || t > a.deadline) {
                Buf n; n.num(a.amount);
                MaStatus("Added ", n.s, " x ", a.name, " to the menu");
                if (plates > a.before) Splash(a.name);
                a.step = kMaIdle;
            }
            return;
        }
        MenuReadSlots(g, mgr);
        const bool on = MenuSlotOfRecipe(a.recipe) >= 0;
        if (a.kind == kMkRemove && (!on || t > a.deadline)) { MaStatus("Removed ", a.name, " from the menu"); a.step = kMaIdle; }
        if (a.kind == kMkSwap && (on || t > a.deadline)) { MaStatus("Swapped ", a.old_name, " for ", a.name); if (on) Splash(a.name); a.step = kMaIdle; }
        return;
    }
    if (a.step == kMaIdle && g_ticks % 15 == 3) {     // keep the slot list fresh for SWAP
        const uint64_t mgr = (MenuLive() && g_mode == 2) ? MenuManager(in, g) : 0;
        if (mgr) MenuReadSlots(g, mgr); else g_nms = 0;
    }
}
void MenuPublish(const Instance& in, const EdenDsmodHostApi* h) {
    void* u = h->userdata;
    const Recipe* r = FindRecipe(in, g_rec_sel);
    const bool live = MenuLive() && g_mode == 2 && !TutorialRunning();   // 0.38.52: not during Bancho's tutorial
    h->publish_i64(u, "menu.live", live ? 1 : 0);
    h->publish_i64(u, "menu.busy", g_ma.step != kMaIdle ? 1 : 0);
    h->publish_i64(u, "menu.amt", g_menu_amt);
    h->publish_i64(u, "menu.ready", live && r && r->can_make > 0 && g_ma.step == kMaIdle ? 1 : 0);
    { Buf t; h->publish_text(u, "menu.status", NoApos(g_ma.status, t)); }
    h->publish_i64(u, "menu.has_status", g_ma.status[0] ? 1 : 0);
    h->publish_i64(u, "menu.splash", g_splash_until > g_ticks ? 1 : 0);
    { Buf t; h->publish_text(u, "menu.splash_name", NoApos(g_splash_name, t)); }
    const bool idle = live && g_ma.step == kMaIdle;
    const bool on = r && MenuSlotOfRecipe(r->id) >= 0;
    if (!r || !live) g_swap_open = false;
    const bool armed = g_rm_armed_until > g_ticks;
    h->publish_i64(u, "menu.can_rm", idle && on ? 1 : 0);
    h->publish_i64(u, "menu.rm_armed", idle && on && armed ? 1 : 0);
    h->publish_i64(u, "menu.rm_plain", idle && on && !armed ? 1 : 0);
    h->publish_i64(u, "menu.can_swap", idle && r && !on && r->can_make > 0 && g_nms > 0 ? 1 : 0);
    h->publish_i64(u, "menu.swap_open", g_swap_open && idle && r && !on ? 1 : 0);
    h->publish_i64(u, "menu.sn", g_nms);
    for (int k = 0; k < kMenuSlots; ++k) {
        Buf key, nm, t;
        const Recipe* sr = k < g_nms ? FindRecipe(in, g_ms[k].rid) : nullptr;
        key.add("menu.son").num(k); h->publish_i64(u, key.s, k < g_nms ? 1 : 0);
        Buf src;
        if (k < g_nms) nm.add(sr ? sr->name : "dish");
        if (sr && sr->icon[0]) src.add("module:icon/").add(sr->icon);
        key.clear(); key.add("menu.sname").num(k); h->publish_text(u, key.s, NoApos(nm.s, t));
        key.clear(); key.add("menu.sisrc").num(k); h->publish_text(u, key.s, src.s);
        key.clear(); key.add("menu.splates").num(k); h->publish_i64(u, key.s, k < g_nms ? g_ms[k].plates : 0);
    }
    uint32_t runs = 0; MbLoad(0x4C, &runs);
    Buf m; m.add(live ? "menu: at the sushi bar, patch running" : MenuLive() ? "menu: patch running" : "menu: patch not running (sushi bar only)");
    m.add(", heartbeat ").num(g_hb2_last).add(", requests run ").num(runs);
    h->publish_text(u, "menu.mode", m.s);
}

void Tick(void*, const EdenDsmodHostApi*) {}

void Configure(void*, const EdenDsmodHostExtensions* x) {
    if (x && x->struct_size >= sizeof(EdenDsmodHostExtensions)) {
        g_astc = x->decode_astc; g_astc_user = x->userdata;
        g_hx = *x; g_hx_ok = x->mailbox_size >= 0x40 && x->load_u32 && x->store_u32 && x->store_u64;
    }
}
// The bottom screen's own choices (sorting, filters, the ingredient panel).
bool UiAction(Instance& in, const char* action, int64_t arg) {
    if (eq(action, "dive_view")) { g_dive_map = arg ? 1 : 0; return true; }
    if (eq(action, "map_zoom")) { g_map_zoomed = g_map_zoomed ? 0 : 1; return true; }
    if (eq(action, "vm_zoom")) { g_vm_zoomed = 0; return true; }   // 0.38.17: the village zoom is off (user)
    if (eq(action, "mk_all")) { g_mk_all = arg ? 1 : 0; return true; }
    if (eq(action, "mk_every")) { g_mk_mask = (g_mk_mask & 0x37) == 0x37 ? 8 : 0x3F; return true; }   // 0.37.22: + ammo, pods   // 0.37.20: bit 3 (missions) is the pin's   // 0.37.4: ALL in the FILTER list
    if (eq(action, "mk_menu")) { g_mk_menu = g_mk_menu ? 0 : 1; return true; }
    if (eq(action, "hud_hide")) { g_hh_on = g_hh_on ? 0 : 1; return true; }   // 0.38.39: HideHUD switch
    if (eq(action, "lo_swap")) { if (g_mode != 1) return false; if (g_lv_act < 0) g_lo_side ^= 1; g_lo_r_req = 1; return true; }
    if (eq(action, "lo_item")) { if (g_mode != 1) return false; if (g_lv_yfocus < 0) g_lo_yside ^= 1; g_lo_r_req = 2; return true; }   // 0.38.31   // 0.38.29
    if (eq(action, "mk_toggle")) { if (arg < 0 || arg > 5) return false; g_mk_mask ^= 1 << arg; return true; }
    if (eq(action, "calc_key")) return CalcKey(arg);   // 0.37.62: the phone's calculator
    if (eq(action, "ph_probe")) { g_ph_show = g_ph_show ? 0 : 1; return true; }   // 0.37.66: the Mail probe panel
    if (eq(action, "mail_scroll")) { g_mail_off += arg ? 3 : -3; return true; }   // 0.37.83: 0 up, 1 down
    if (eq(action, "mail_open")) {                                          // 0.37.68: open this mail on the top screen
        arg += g_mail_off;                                                  // 0.37.83: the page's row -> the list's
        if (arg < 0 || arg >= g_ph.n) return false;
        return PhoneJobStart(static_cast<int>(arg));
    }
    if (eq(action, "eco_scroll")) { g_eco_off = 0; (void)arg; return true; }   // 0.38.5: the page scrolls itself (UP / DOWN gone)
    if (eq(action, "eco_pin")) {                                            // 0.37.73: pin / unpin a mission's goals to the map
        arg += g_eco_off;                                                   // 0.37.76: the page's row -> the list's
        if (arg < 0 || arg >= g_eco.n || g_eco.row[arg].div || g_eco.row[arg].state == 4) return false;
        g_eco_pin = g_eco_pin == g_eco.row[arg].tid ? 0 : g_eco.row[arg].tid;
        return true;
    }
    if (eq(action, "shop_open") || eq(action, "shop_craft") || eq(action, "shop_track")) {   // 0.37.89 / 0.37.91
        if (arg < 0 || arg >= g_shop.n || !g_shop.row[arg].craft) return false;
        if (g_pj.step == kPjRun) g_pj.step = kPjIdle;
        g_shop_confirm = 0;
        const bool ok = PhoneJobStart(static_cast<int>(arg), 0, kAppDuffShop);
        g_pj.act = eq(action, "shop_craft") ? 1 : eq(action, "shop_track") ? 2 : 0;
        return ok;
    }
    if (eq(action, "shop_confirm") || eq(action, "shop_cancel")) {        // 0.37.91: answer the game's craft popup
        if (!g_shop_confirm || g_pj.step == kPjRun) return false;
        g_shop_confirm = 0;
        const bool ok = PhoneJobStart(-1, 3);
        g_pj.one = eq(action, "shop_confirm") ? kPbA : kPbB;
        return ok;
    }
    if (eq(action, "call_open") || eq(action, "call_call")) {             // 0.37.109: Call -- show / call (X)
        if (arg < 0 || arg >= g_call.n || !g_call.row[arg].npc) return false;
        if (g_pj.step == kPjRun) g_pj.step = kPjIdle;
        const bool ok = PhoneJobStart(static_cast<int>(arg), 0, kAppContacts);
        g_pj.act = eq(action, "call_call") ? 1 : 0;
        return ok;
    }
    if (eq(action, "idv_open") || eq(action, "idv_up")) {                 // 0.37.102: iDiver -- show / upgrade (A, then CONFIRM)
        if (arg < 0 || arg >= g_idv.n || !g_idv.row[arg].id) return false;
        if (eq(action, "idv_up") && (g_idv.row[arg].max || g_idv.row[arg].lock)) return false;
        if (g_pj.step == kPjRun) g_pj.step = kPjIdle;
        g_idv_confirm = 0;
        const bool ok = PhoneJobStart(static_cast<int>(arg), 0, kAppIDiver);
        g_pj.act = eq(action, "idv_up") ? 1 : 0;
        return ok;
    }
    if (eq(action, "idv_ok")) {                                           // 0.37.105: the level-up screen: A closes it
        if (g_idv.ui != 2 || g_pj.step == kPjRun) return false;
        const bool ok = PhoneJobStart(-1, 3);
        g_pj.one = kPbA;
        return ok;
    }
    if (eq(action, "idv_confirm") || eq(action, "idv_cancel")) {          // the game's detail: A buys the level, B closes it
        if ((!g_idv_confirm && g_idv.ui != 1) || g_pj.step == kPjRun) return false;
        g_idv_confirm = 0;
        const bool ok = PhoneJobStart(-1, 3);
        g_pj.one = eq(action, "idv_confirm") ? kPbA : kPbB;
        return ok;
    }
    if (eq(action, "eco_open")) {                                           // 0.37.72: open this mission on the top screen
        arg += g_eco_off;
        if (arg < 0 || arg >= g_eco.n || g_eco.row[arg].div || !g_eco.row[arg].md) return false;
        if (g_pj.step == kPjRun) g_pj.step = kPjIdle;
        return PhoneJobStart(static_cast<int>(arg), 0, kAppEcoWatcher);
    }
    if (eq(action, "ph_stop")) { if (g_pj.step == kPjRun) PjStop("Stopped"); return true; }
    // 0.37.69: BACK / CLOSE on the bottom screen; 0.37.70: they first stop a job still running (there is no STOP)
    // 0.37.85: BACK while diving or with the phone closed (nothing open on top) -> the bottom screen's APPS page
    // (the page bind ph_nav does the same; this covers it from the module too). CLOSE while diving -> APPS.
    if (eq(action, "ph_back")) {
        if (g_mode == 1 || !g_ph.dpc || g_ph.state == 0) { if (g_pj.step == kPjRun) g_pj.step = kPjIdle; g_page = kPagePhone; return true; }
        if (g_pj.step == kPjRun) g_pj.step = kPjIdle; return PhoneJobStart(-1, 1);
    }
    if (eq(action, "ph_close")) {
        if (g_mode == 1) { g_page = kPagePhone; return true; }
        if (g_pj.step == kPjRun) g_pj.step = kPjIdle; return PhoneJobStart(-1, 2);
    }
    if (eq(action, "nav")) {                       // every page change goes through here (ui.page)
        if (arg < 0 || arg >= kPages) return false;
        g_page = static_cast<int>(arg);
        return true;
    }
    if (eq(action, "cargo_sort")) {
        if (arg < 0 || arg > 3) return false;
        if (g_dc.step == kDcIdle) g_dc.pick = -1;   // rows move: drop a pending pick
        g_cargo_sort = static_cast<int>(arg);
        SortRows(in);
        return true;
    }
    if (eq(action, "rec_sort")) {
        if (arg < 0 || arg > 3) return false;
        g_rec_sort = static_cast<int>(arg);
        SortRecipes(in);
        return true;
    }
    if (eq(action, "ing_filter")) {
        if (arg < 0 || arg > 2) return false;
        g_ing_filter = arg == 2 ? (g_ing_filter ? 0 : 1) : static_cast<int>(arg);   // 2: the ALL toggle
        return true;
    }
    if (eq(action, "ing_pick")) {
        int shown = in.ning;
        if (g_ing_filter == 1) { shown = 0; for (int i = 0; i < in.ning; ++i) shown += in.ing[i].count > 0 ? 1 : 0; }
        if (arg < 0 || arg >= shown) return false;
        g_ing_sel = in.ing[arg].id;
        return true;
    }
    if (eq(action, "ing_close")) { g_ing_sel = -1; return true; }
    if (eq(action, "rec_pick")) {
        if (arg < 0 || arg >= in.nrec) return false;
        g_rec_sel = in.rec[arg].id;
        g_menu_amt = in.rec[arg].can_make > 0 ? in.rec[arg].can_make : 1;   // all you can make, like the game offers
        if (g_menu_amt > 99) g_menu_amt = 99;
        if (g_ma.step == kMaIdle) g_ma.status[0] = 0;
        g_swap_open = false; g_rm_armed_until = 0;
        return true;
    }
    if (eq(action, "rec_close")) { g_splash_until = 0; g_rec_sel = -1; g_swap_open = false; g_rm_armed_until = 0; return true; }
    if (eq(action, "menu_add")) { if (TutorialRunning()) return false;   // 0.38.52                  // put the open recipe on tonight's menu
        const Recipe* r = FindRecipe(in, g_rec_sel);
        if (!r || g_ma.step != kMaIdle || r->can_make <= 0) return false;
        g_ma.recipe = r->id; DcCopy(g_ma.name, sizeof g_ma.name, r->name);
        g_ma.amount = g_menu_amt < 1 ? 1 : g_menu_amt > r->can_make ? r->can_make : g_menu_amt;
        g_ma.kind = kMkAdd; g_ma.step = kMaSend; g_ma.status[0] = 0;
        return true;
    }
    if (eq(action, "menu_remove")) { if (TutorialRunning()) return false;   // 0.38.52               // take the open recipe off the menu (tap twice)
        const Recipe* r = FindRecipe(in, g_rec_sel);
        if (!r || g_ma.step != kMaIdle || MenuSlotOfRecipe(r->id) < 0) return false;
        if (g_rm_armed_until <= g_ticks) { g_rm_armed_until = g_ticks + 180; return true; }
        g_rm_armed_until = 0;
        g_ma.recipe = r->id; DcCopy(g_ma.name, sizeof g_ma.name, r->name);
        g_ma.amount = 0; g_ma.kind = kMkRemove; g_ma.step = kMaSend; g_ma.status[0] = 0;
        return true;
    }
    if (eq(action, "menu_swap_open")) { if (TutorialRunning()) return false; g_swap_open = true; return true; }   // 0.38.52: not in the tutorial
    if (eq(action, "menu_swap_close")) { g_swap_open = false; return true; }
    if (eq(action, "menu_swap")) { if (TutorialRunning()) return false;   // 0.38.52                 // replace menu slot `arg` with the open recipe
        const Recipe* r = FindRecipe(in, g_rec_sel);
        if (!r || g_ma.step != kMaIdle || arg < 0 || arg >= g_nms || r->can_make <= 0) return false;
        const Recipe* old = FindRecipe(in, g_ms[arg].rid);
        g_ma.recipe = r->id; DcCopy(g_ma.name, sizeof g_ma.name, r->name);
        DcCopy(g_ma.old_name, sizeof g_ma.old_name, old ? old->name : "the dish");
        g_ma.slot = g_ms[arg].sid;
        g_ma.amount = g_menu_amt < 1 ? 1 : g_menu_amt > r->can_make ? r->can_make : g_menu_amt;
        g_ma.kind = kMkSwap; g_ma.step = kMaSend; g_ma.status[0] = 0; g_swap_open = false;
        return true;
    }
    if (eq(action, "menu_dec")) { if (g_menu_amt > 1) --g_menu_amt; return true; }
    if (eq(action, "menu_inc")) {
        const Recipe* r = FindRecipe(in, g_rec_sel);
        if (r && g_menu_amt < r->can_make && g_menu_amt < 99) ++g_menu_amt;
        return true;
    }
    if (eq(action, "rec_pin")) {                   // pin the open recipe, or unpin it
        if (!FindRecipe(in, g_rec_sel)) return false;
        const int ps = PinSlot(g_rec_sel);
        if (ps >= 0) {                              // unpin: the others close up
            for (int p = ps; p + 1 < kMaxPins; ++p) { g_pins[p] = g_pins[p + 1]; g_pin_tgt[p] = g_pin_tgt[p + 1]; }
            g_pins[kMaxPins - 1] = -1;
        } else {
            const int n = PinCount();
            const int at = n >= kMaxPins ? kMaxPins - 1 : n;   // full: the new pin takes the last one's place
            g_pins[at] = g_rec_sel; g_pin_tgt[at] = g_pin_target;
        }
        return true;
    }
    if (eq(action, "pin_pick")) {                  // a pinned recipe on DIVE: ask UNPIN or OPEN
        if (arg < 0 || arg >= kMaxPins || g_pins[arg] <= 0) return false;
        g_pin_sel = static_cast<int>(arg);
        return true;
    }
    if (eq(action, "pin_cancel")) { g_pin_sel = -1; return true; }
    if (eq(action, "pin_unpin")) {
        if (g_pin_sel < 0 || g_pins[g_pin_sel] <= 0) return false;
        for (int p = g_pin_sel; p + 1 < kMaxPins; ++p) { g_pins[p] = g_pins[p + 1]; g_pin_tgt[p] = g_pin_tgt[p + 1]; }
        g_pins[kMaxPins - 1] = -1; g_pin_sel = -1;
        return true;
    }
    if (eq(action, "pl_open")) {                   // 0.37.2: tap an ingredient on the map page: its recipe on SUSHI
        if (arg < 0 || arg >= 8) return false;
        if (g_pl_pin[arg] == -2) { g_page = kPageEco; return true; }   // 0.37.73: an EcoWatcher goal -> the EcoWatcher page
        g_pin_sel = g_pl_pin[arg];
        action = "pin_open";
    }
    if (eq(action, "pin_open")) {                  // its panel on SUSHI > RECIPES
        if (g_pin_sel < 0 || g_pins[g_pin_sel] <= 0) return false;
        const Recipe* r = FindRecipe(in, g_pins[g_pin_sel]);
        g_rec_sel = g_pins[g_pin_sel]; g_pin_sel = -1; g_swap_open = false; g_rm_armed_until = 0;
        if (r) { g_menu_amt = r->can_make > 0 ? (r->can_make > 99 ? 99 : r->can_make) : 1; }
        g_nav_sushi_until = g_ticks + 20;
        g_page = kPageSushi;
        return true;
    }
    if (eq(action, "pin_clear")) { for (int p = 0; p < kMaxPins; ++p) g_pins[p] = -1; return true; }
    if (eq(action, "pin_dec") || eq(action, "pin_inc")) {   // the open recipe's target (or the next pin's)
        const int ps = PinSlot(g_rec_sel), d = eq(action, "pin_inc") ? 1 : -1;
        int& t = ps >= 0 ? g_pin_tgt[ps] : g_pin_target;
        if (t + d >= 1 && t + d <= 99) t += d;
        return true;
    }
    if (eq(action, "am_pin")) {                    // 0.38.58: MISSIONS: pin / unpin one of the other active missions (its card)
        if (arg < 0 || arg >= g_am_nvis) return false;
        const AmMis& am = g_am.mis[g_am_vis[arg]];
        if (g_pin_md != am.md && !am.canpin) return true;
        g_pin_md = g_pin_md == am.md ? 0 : am.md;
        return true;
    }
    if (eq(action, "m_pin")) {                     // pin a mission to DIVE, or unpin it
        if (arg < 0 || arg >= kMTasks || !g_mi.md[arg]) return false;
        if (g_pin_md != g_mi.md[arg] && arg < 3 && !g_canpin[arg]) return true;   // 0.37.47: not pinnable
        g_pin_md = g_pin_md == g_mi.md[arg] ? 0 : g_mi.md[arg];
        return true;
    }
    if (eq(action, "ld_test")) { if ((g_ld_on || g_ld_title) && !g_ld_test) return true; g_ld_test = g_ld_test ? 0 : 1; return true; }   // a tap on the real loading picture does nothing   // 0.37.49: DIAG TITLE TEST
    if (eq(action, "noop")) return true;          // a panel's background: swallows taps meant for it
    return false;
}

EdenDsmodBool OnAction(void* p, const char* action, int64_t argument) {
    Instance& in = *static_cast<Instance*>(p);
    if (UiAction(in, action, argument)) return EDEN_DSMOD_TRUE;
    return DcAction(in, action, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}
// ---------------------------------------------------------------- item pictures (round 13)
// "module:icon/<name>" -> the player's own exported picture, user:icons/<name>.rgba, written by
// tools/dtd/export_icons.py from their romfs: "DTDI", u32 width, u32 height, then RGBA8 rows.
// The package ships no game art. Runs on the host's single asset worker, so one static buffer.
constexpr uint32_t kIconMax = 256;
uint8_t g_icon_px[kIconMax * kIconMax * 4];
uint8_t g_sq_px[kIconMax * kIconMax * 4];
// The pages draw a picture stretched to its box, and their boxes are square: hand pictures over
// centred on a transparent square, so a wide fish or a tall bottle keeps its shape.
void SinkSquare(EdenDsmodImageSink sink, void* receiver, uint32_t w, uint32_t h, const uint8_t* px) {
    if (w == h || w > kIconMax || h > kIconMax) { sink(receiver, w, h, px, size_t{w} * h * 4); return; }
    const uint32_t side = w > h ? w : h, ox = (side - w) / 2, oy = (side - h) / 2;
    for (size_t k = 0; k < size_t{side} * side * 4; ++k) g_sq_px[k] = 0;
    for (uint32_t v = 0; v < h; ++v)
        for (uint32_t u = 0; u < w * 4; ++u) g_sq_px[((v + oy) * side + ox) * 4 + u] = px[v * w * 4 + u];
    sink(receiver, side, side, g_sq_px, size_t{side} * side * 4);
}
EdenDsmodBool LoadImageLocked(const EdenDsmodHostApi* host, const char* key, void* receiver, EdenDsmodImageSink sink);
// 0.37.49: "module:logo/<name>" -- a picture bigger than kLiveMax (the game's title logo, 1474x656),
// read in pieces of up to kLiveMax and shrunk (box average) to fit kLogoW x kLogoH.
constexpr uint32_t kLogoW = 768, kLogoH = 384;
uint8_t g_logo_px[kLogoW * kLogoH * 4];
bool LogoImage(const EdenDsmodHostApi* host, const char* name, void* receiver, EdenDsmodImageSink sink) {
    uint32_t full[2] = {0, 0}, dw = 0, dh = 0;
    const uint32_t ask[4] = {0, 0, 0, 0};
    if (!LiveDecode(host, name, &dw, &dh, ask, full) || !full[0] || !full[1]) { g_logo_err = 100 + g_live_err; return false; }
    uint32_t f = 1;
    while (full[0] / f > kLogoW || full[1] / f > kLogoH) ++f;
    const uint32_t nw = full[0] / f, nh = full[1] / f, tile = ((kLiveMax - 24) / f) * f;   // room for block edges (up to 12 px each side)
    for (uint32_t ty = 0; ty < nh * f; ty += tile)
        for (uint32_t tx = 0; tx < nw * f; tx += tile) {
            const uint32_t sw = (nw * f - tx < tile) ? nw * f - tx : tile, sh = (nh * f - ty < tile) ? nh * f - ty : tile;
            const uint32_t piece[4] = {tx, ty, sw, sh};
            if (!LiveDecode(host, name, &dw, &dh, piece, nullptr) || dw != sw || dh != sh) { g_logo_err = 200 + g_live_err; return false; }
            for (uint32_t v = 0; v < sh / f; ++v)
                for (uint32_t u = 0; u < sw / f; ++u)
                    for (int c = 0; c < 4; ++c) {
                        uint32_t sum = 0;
                        for (uint32_t a = 0; a < f; ++a) for (uint32_t b = 0; b < f; ++b) sum += g_live_px[((v * f + a) * sw + u * f + b) * 4 + c];
                        g_logo_px[((ty / f + v) * nw + tx / f + u) * 4 + c] = static_cast<uint8_t>(sum / (f * f));
                    }
        }
    g_logo_err = 0; g_logo_w = static_cast<int>(nw); g_logo_h = static_cast<int>(nh);
    sink(receiver, nw, nh, g_logo_px, size_t{nw} * nh * 4);
    return true;
}
// 0.37.59: "module:bg/<name>" -- the title menu's illustration (DR_Illust) behind the loading picture: the centre
// cut to the bottom screen's shape (1240:1080), read in pieces like LogoImage and shrunk to fit kBgW x kBgH.
constexpr uint32_t kBgW = 960, kBgH = 840;
uint8_t g_bg_px[kBgW * kBgH * 4], g_bg_tmp[kBgW * kBgH * 4];
bool BgImage(const EdenDsmodHostApi* host, const char* key, void* receiver, EdenDsmodImageSink sink) {
    // 0.37.97: "<name>@<w>x<h>" cuts the centre to that box's shape instead (the Weapon Shop's camo, PhoneBg_Duff)
    // 0.37.101: "...@<w>x<h>r<n>" also rounds its corners (n = the radius in that box's pixels), so it lies flush in a
    // rounded card (EcoWatcher_bg in Box_30rad)
    char name[96]; uint32_t aw = 1240, ah = 1080, rad = 0; bool stretch = false, toponly = false;
    size_t n = 0;
    while (key[n] && key[n] != '@' && n + 1 < sizeof name) { name[n] = key[n]; ++n; }
    name[n] = 0;
    if (key[n] == '@') {
        const char* q = key + n + 1; uint32_t a = 0, b = 0;
        while (*q >= '0' && *q <= '9') a = a * 10 + static_cast<uint32_t>(*q++ - '0');
        if (*q == 'x') { ++q; while (*q >= '0' && *q <= '9') b = b * 10 + static_cast<uint32_t>(*q++ - '0'); }
        if (a && b && a <= 4096 && b <= 4096) { aw = a; ah = b; }
        if (*q == 'r') { ++q; while (*q >= '0' && *q <= '9') rad = rad * 10 + static_cast<uint32_t>(*q++ - '0'); }
        // 0.37.105: 's' = the whole picture stretched to the box (no centre cut; a bar), 't' = round the top corners only
        for (; *q; ++q) { if (*q == 's') stretch = true; else if (*q == 't') toponly = true; }
    }
    uint32_t full[2] = {0, 0}, dw = 0, dh = 0;
    const uint32_t ask[4] = {0, 0, 0, 0};
    if (!LiveDecode(host, name, &dw, &dh, ask, full) || !full[0] || !full[1]) { g_bg_err = 100 + g_live_err; return false; }
    const uint32_t W = full[0], H = full[1];
    uint32_t cw = W, ch = H;                                  // the centre at aw:ah (1240:1080 unless asked)
    if (stretch) {}
    else if (uint64_t{W} * ah > uint64_t{H} * aw) cw = static_cast<uint32_t>(uint64_t{H} * aw / ah);
    else ch = static_cast<uint32_t>(uint64_t{W} * ah / aw);
    const uint32_t ox = (W - cw) / 2, oy = (H - ch) / 2;
    uint32_t f = 1;
    while (cw / f > kBgW || ch / f > kBgH) ++f;
    const uint32_t nw = cw / f, nh = ch / f, tile = ((kLiveMax - 24) / f) * f;
    for (uint32_t ty = 0; ty < nh * f; ty += tile)
        for (uint32_t tx = 0; tx < nw * f; tx += tile) {
            const uint32_t sw = (nw * f - tx < tile) ? nw * f - tx : tile, sh = (nh * f - ty < tile) ? nh * f - ty : tile;
            const uint32_t piece[4] = {ox + tx, oy + ty, sw, sh};
            if (!LiveDecode(host, name, &dw, &dh, piece, nullptr) || dw != sw || dh != sh) { g_bg_err = 200 + g_live_err; return false; }
            for (uint32_t v = 0; v < sh / f; ++v)
                for (uint32_t u = 0; u < sw / f; ++u)
                    for (int c = 0; c < 4; ++c) {
                        uint32_t sum = 0;
                        for (uint32_t a = 0; a < f; ++a) for (uint32_t b = 0; b < f; ++b) sum += g_live_px[((v * f + a) * sw + u * f + b) * 4 + c];
                        g_bg_px[((ty / f + v) * nw + tx / f + u) * 4 + c] = static_cast<uint8_t>(sum / (f * f));
                    }
        }
    // 0.37.117: a stretched picture (a thin gradient strip like Address_BG, 8 px wide) is spread to the box's own shape
    // first (nearest pixel), so its corners can be rounded as finely as the others' (8 px could not be)
    uint32_t ow = nw, oh = nh;
    if (stretch) {
        uint32_t g = 1;
        while ((aw + g - 1) / g > kBgW || (ah + g - 1) / g > kBgH) ++g;
        ow = aw / g; oh = ah / g;
        if (ow != nw || oh != nh) {
            for (size_t i = 0; i < size_t{nw} * nh * 4; ++i) g_bg_tmp[i] = g_bg_px[i];
            for (uint32_t v = 0; v < oh; ++v)
                for (uint32_t u = 0; u < ow; ++u) {
                    const uint32_t su = static_cast<uint32_t>(uint64_t{u} * nw / ow), sv = static_cast<uint32_t>(uint64_t{v} * nh / oh);
                    for (int c = 0; c < 4; ++c) g_bg_px[(size_t{v} * ow + u) * 4 + c] = g_bg_tmp[(size_t{sv} * nw + su) * 4 + c];
                }
        }
    }
    const uint32_t nw2 = ow, nh2 = oh;
    if (rad) {   // see-through outside the rounded corners, measured in the box's pixels (1 px soft edge)
        const float sx = static_cast<float>(aw) / static_cast<float>(nw2), sy = static_cast<float>(ah) / static_cast<float>(nh2);
        const float r = static_cast<float>(rad), bw = static_cast<float>(aw), bh = static_cast<float>(ah);
        const float lo = (r - 0.5f) * (r - 0.5f), hi = (r + 0.5f) * (r + 0.5f);   // (no libm: by the squares)
        for (uint32_t v = 0; v < nh2; ++v)
            for (uint32_t u = 0; u < nw2; ++u) {
                const float px = (u + 0.5f) * sx, py = (v + 0.5f) * sy;
                if (toponly && py > r) continue;
                const float cx = px < r ? r : (px > bw - r ? bw - r : px), cy = py < r ? r : (py > bh - r ? bh - r : py);
                const float dx = px - cx, dy = py - cy, d2 = dx * dx + dy * dy;
                if (d2 <= lo) continue;
                float k = (hi - d2) / (hi - lo); k = k < 0 ? 0 : (k > 1 ? 1 : k);
                uint8_t* al = &g_bg_px[(size_t{v} * nw2 + u) * 4 + 3];
                *al = static_cast<uint8_t>(*al * k);
            }
    }
    g_bg_err = 0; g_bg_w = static_cast<int>(nw2); g_bg_h = static_cast<int>(nh2);
    sink(receiver, nw2, nh2, g_bg_px, size_t{nw2} * nh2 * 4);
    return true;
}
// 0.38.33: "module:fit/<name>" -- a loadout picture at the game's own scale: centred, pixel for pixel, on a fixed
// kFitW x kFitH see-through canvas (the HUD shows every thumbnail 1:1, so a knife stays smaller than a gun and an O2
// capsule stays tall). A picture bigger than the canvas is shrunk by a whole factor to fit. The card draws the canvas in
// a box of the same shape, so nothing is stretched.
constexpr uint32_t kFitW = 84, kFitH = 48;        // 0.38.35: the items' canvas (84x48: the Y card is narrower now)
constexpr uint32_t kFitMax = 512;
uint8_t g_fit_px[kFitMax * kFitMax * 4];
// 0.38.35: "module:fit/<name>@<W>x<H>" -- the weapons: the picture padded (see-through) to the box's shape W:H, not
// scaled, so the card's box shows it as big as fits without stretching (the HUD draws its weapons that big).
// Without "@": the items' fixed 1:1 canvas (as 0.38.33).
bool FitImage(const EdenDsmodHostApi* host, const char* key, void* receiver, EdenDsmodImageSink sink) {
    char name[96]; size_t n = 0;
    while (key[n] && key[n] != '@' && n + 1 < sizeof name) { name[n] = key[n]; ++n; }
    name[n] = 0;
    uint32_t aw = 0, ah = 0;
    if (key[n] == '@') {
        const char* q = key + n + 1;
        while (*q >= '0' && *q <= '9') aw = aw * 10 + static_cast<uint32_t>(*q++ - '0');
        if (*q == 'x') { ++q; while (*q >= '0' && *q <= '9') ah = ah * 10 + static_cast<uint32_t>(*q++ - '0'); }
    }
    uint32_t w = 0, h = 0;
    if (!LiveDecode(host, name, &w, &h) || !w || !h) { ++g_live_failed; return false; }
    ++g_live_drawn;
    uint32_t cw = kFitW, ch = kFitH;
    uint32_t f = 1;
    if (aw && ah) {
        while ((w / f) > kFitMax / 2 || (h / f) > kFitMax / 2) ++f;
        const uint32_t pw = w / f, ph = h / f;
        if (static_cast<uint64_t>(pw) * ah > static_cast<uint64_t>(ph) * aw) { cw = pw; ch = (pw * ah + aw - 1) / aw; }
        else { ch = ph; cw = (ph * aw + ah - 1) / ah; }
        if (cw > kFitMax) cw = kFitMax;
        if (ch > kFitMax) ch = kFitMax;
    } else {
        while (w / f > kFitW || h / f > kFitH) ++f;
    }
    const uint32_t nw = w / f, nh = h / f, ox = (cw - nw) / 2, oy = (ch - nh) / 2;
    const size_t bytes = size_t{cw} * ch * 4;
    for (size_t k = 0; k < bytes; ++k) g_fit_px[k] = 0;
    for (uint32_t v = 0; v < nh; ++v)
        for (uint32_t u = 0; u < nw; ++u)
            for (int c = 0; c < 4; ++c) {
                uint32_t sum = 0;
                for (uint32_t a = 0; a < f; ++a) for (uint32_t b = 0; b < f; ++b) sum += g_live_px[((v * f + a) * w + u * f + b) * 4 + c];
                g_fit_px[((v + oy) * cw + u + ox) * 4 + c] = static_cast<uint8_t>(sum / (f * f));
            }
    sink(receiver, cw, ch, g_fit_px, bytes);
    return true;
}
EdenDsmodBool LoadImage(void*, const EdenDsmodHostApi* host, const char* key, void* receiver, EdenDsmodImageSink sink) {
    if (!host || !host->read_romfs || !sink) return EDEN_DSMOD_FALSE;
    LiveLock lock;
    if (eq(key, "module:font")) return FontAtlas(host, sink, receiver) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    if (starts(key, "module:map/t")) return TerrainImage(sink, receiver) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    if (starts(key, "module:bg/")) return BgImage(host, key + 10, receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    if (starts(key, "module:logo/")) return LogoImage(host, key + 12, receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    if (starts(key, "module:fit/")) return FitImage(host, key + 11, receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    return LoadImageLocked(host, key, receiver, sink);
}
EdenDsmodBool LoadImageLocked(const EdenDsmodHostApi* host, const char* key, void* receiver, EdenDsmodImageSink sink) {
    // "module:icon/<name>": a picture for a square box (centred on a transparent square);
    // "module:frame/<name>": a UI frame / button / banner, handed over at its own shape (9-slice)
    const bool frame = starts(key, "module:frame/");
    const char* pre = frame ? "module:frame/" : "module:icon/";
    int i = 0;
    for (; pre[i]; ++i) if (key[i] != pre[i]) return EDEN_DSMOD_FALSE;
    if (!host || !host->read_romfs || !sink) return EDEN_DSMOD_FALSE;
    if (frame) {
        uint32_t fw = 0, fh = 0;
        if (!LiveDecode(host, key + i, &fw, &fh)) { ++g_live_failed; return EDEN_DSMOD_FALSE; }
        ++g_live_drawn;
        sink(receiver, fw, fh, g_live_px, size_t{fw} * fh * 4);
        return EDEN_DSMOD_TRUE;
    }
    uint32_t lw = 0, lh = 0;
    if (LiveDecode(host, key + i, &lw, &lh)) {      // straight from the game files
        ++g_live_drawn;
        if (lw > kIconMax || lh > kIconMax) {        // shrink (box average) to what the page needs
            const uint32_t m = lw > lh ? lw : lh, f = (m + kIconMax - 1) / kIconMax;
            const uint32_t nw = lw / f, nh = lh / f;
            for (uint32_t v = 0; v < nh; ++v)
                for (uint32_t u = 0; u < nw; ++u)
                    for (int c = 0; c < 4; ++c) {
                        uint32_t sum = 0;
                        for (uint32_t a = 0; a < f; ++a) for (uint32_t b = 0; b < f; ++b) sum += g_live_px[((v * f + a) * lw + u * f + b) * 4 + c];
                        g_icon_px[(v * nw + u) * 4 + c] = static_cast<uint8_t>(sum / (f * f));
                    }
            SinkSquare(sink, receiver, nw, nh, g_icon_px);
        } else {
            SinkSquare(sink, receiver, lw, lh, g_live_px);
        }
        return EDEN_DSMOD_TRUE;
    }
    ++g_live_failed;
    char file[96];
    if (!ResolveIcon(host, g_ix_worker, key + i, file, sizeof file)) return EDEN_DSMOD_FALSE;
    Buf path; uint8_t hdr[12];
    path.add(g_ix_worker.prefix).add(file).add(".rgba");
    if (host->read_romfs(host->userdata, path.s, 0, hdr, sizeof hdr) != sizeof hdr) return EDEN_DSMOD_FALSE;
    if (hdr[0] != 'D' || hdr[1] != 'T' || hdr[2] != 'D' || hdr[3] != 'I') return EDEN_DSMOD_FALSE;
    uint32_t w, h; memcpy(&w, hdr + 4, 4); memcpy(&h, hdr + 8, 4);
    if (!w || !h || w > kIconMax || h > kIconMax) return EDEN_DSMOD_FALSE;
    const size_t bytes = static_cast<size_t>(w) * h * 4;
    if (host->read_romfs(host->userdata, path.s, sizeof hdr, g_icon_px, bytes) != bytes) return EDEN_DSMOD_FALSE;
    SinkSquare(sink, receiver, w, h, g_icon_px);
    return EDEN_DSMOD_TRUE;
}

// "module:fontdata": the font's metrics for decode_font -- "DTDF", line height, first character,
// count, then the glyphs. Not readable yet: "DTDW" + the reason, and decode_font declines (the host
// asks again about once a second).
uint8_t g_fdata[16 + sizeof(EdenDsmodFontGlyph) * kFontN];
EdenDsmodBool LoadData(void*, const EdenDsmodHostApi* host, const char* key, void* receiver, EdenDsmodDataSink sink) {
    if (!eq(key, "module:fontdata") || !host || !host->read_romfs || !sink) return EDEN_DSMOD_FALSE;
    LiveLock lock;
    const int want = __atomic_load_n(&g_font_want, __ATOMIC_ACQUIRE);
    if (!BuildMix(host, want)) {
        uint8_t w[8] = {'D', 'T', 'D', 'W'};
        const uint32_t e = static_cast<uint32_t>(g_font.err ? g_font.err : 20 + g_hud.err); memcpy(w + 4, &e, 4);
        sink(receiver, w, sizeof w);
        return EDEN_DSMOD_TRUE;
    }
    const uint32_t hdr[3] = {g_font.line, kFontFirst, kFontN};
    g_fdata[0] = 'D'; g_fdata[1] = 'T'; g_fdata[2] = 'D'; g_fdata[3] = 'F';
    memcpy(g_fdata + 4, hdr, sizeof hdr);
    memcpy(g_fdata + 16, g_mix[want].out, sizeof g_mix[want].out);
    g_data_want = want;                               // the atlas that follows is drawn for this look
    sink(receiver, g_fdata, sizeof g_fdata);
    return EDEN_DSMOD_TRUE;
}
EdenDsmodBool DecodeFont(void*, const uint8_t* bytes, size_t size, void* receiver, EdenDsmodFontSink sink) {
    if (!bytes || !sink || size != sizeof g_fdata || bytes[0] != 'D' || bytes[1] != 'T' || bytes[2] != 'D' || bytes[3] != 'F') return EDEN_DSMOD_FALSE;
    uint32_t hdr[3]; memcpy(hdr, bytes + 4, sizeof hdr);
    if (!hdr[0] || hdr[2] != kFontN) return EDEN_DSMOD_FALSE;
    static EdenDsmodFontGlyph glyphs[kFontN];
    memcpy(glyphs, bytes + 16, sizeof glyphs);
    sink(receiver, hdr[0], hdr[1], glyphs, hdr[2]);
    return EDEN_DSMOD_TRUE;
}
const EdenDsmodModuleDataExtensions kDataExtensions = {
    EDEN_DSMOD_DATA_EXT_VERSION, sizeof(EdenDsmodModuleDataExtensions), EDEN_DSMOD_DATA_EXT_HASH, LoadData,
};
const EdenDsmodFontExtensions kFontExtensions = {
    EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions), EDEN_DSMOD_FONT_EXT_HASH, DecodeFont,
};

const EdenDsmodModuleExtensions kExtensions = {
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    Configure, OnAction, LoadImage,
};

const EdenDsmodModuleApi kModule = {
    EDEN_DSMOD_MODULE_ABI_VERSION,
    sizeof(EdenDsmodModuleApi),
    0,
    EDEN_DSMOD_MODULE_ABI_HASH,
    kTitle,
    "Dave the Diver cargo",
    EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN | EDEN_DSMOD_CAP_EXTENSIONS,
    SupportsBuild,
    Create,
    Destroy,
    Sample,
    Tick,
};

}  // namespace

EXPORT const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t host_abi_version, uint64_t host_abi_hash) {
    if (host_abi_version != EDEN_DSMOD_MODULE_ABI_VERSION || host_abi_hash != EDEN_DSMOD_MODULE_ABI_HASH) return nullptr;
    return &kModule;
}

EXPORT const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t version, uint64_t abi_hash) {
    if (version != EDEN_DSMOD_EXT_VERSION || abi_hash != EDEN_DSMOD_EXT_HASH) return nullptr;
    return &kExtensions;
}

EXPORT const EdenDsmodModuleDataExtensions* eden_dsmod_get_data_extensions(uint32_t version, uint64_t abi_hash) {
    if (version != EDEN_DSMOD_DATA_EXT_VERSION || abi_hash != EDEN_DSMOD_DATA_EXT_HASH) return nullptr;
    return &kDataExtensions;
}

EXPORT const EdenDsmodFontExtensions* eden_dsmod_get_font_extensions(uint32_t version, uint64_t abi_hash) {
    if (version != EDEN_DSMOD_FONT_EXT_VERSION || abi_hash != EDEN_DSMOD_FONT_EXT_HASH) return nullptr;
    return &kFontExtensions;
}
