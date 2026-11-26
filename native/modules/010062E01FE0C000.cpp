// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Digimon Story Time Stranger companion module (title 010062E01FE0C000, update 1.2.2).
//
// Serves the package everything that comes from the game, straight from the player's own romfs, so
// the package itself ships no game pixels, no game text and no game data:
// - Text: the manifest's "dsts_text" block (the config handed to create() is the whole manifest)
//   lists templates whose game words are references: 64-bit FNV-1a hashes of the cleaned string. The
//   module scans the text archive's .mbe tables, hashes every string the same way (Clean(), identical
//   to tools/digimon-time-stranger/public_text.py), keeps the referenced ones, and publishes the filled
//   templates for the page on screen.
// - Game data: prices, item descriptions, chests, wild Digimon and side-quest rewards, read from the
//   game's own tables (010062E01FE0C000_data.inc) for the identifiers in the manifest's "dsts_data".
// - Pictures: image key "module:dsts:<name>" -> images/<name>.img inside app_0.nx64.mvgl (only
//   names starting with "ui_" are served), plus "module:dsts:itemicon_<item id>".
//   The archive is MDB1 (LZ4-block compressed members); pictures are DDS (BC7, BC1 or BC3), stored
//   upside down.
// - Font: the game's menu font (010062E01FE0C000_font.inc) through the font extension.
// Written without the C++ library (no STL, no exceptions; malloc/free only), so nothing can throw
// across the C ABI.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

const u64 kTitle = 0x010062E01FE0C000ull;
const char kBuild[] = "135C83CDEA06AEC2";

u32 Le32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 Le64(const u8* p) { return u64(Le32(p)) | u64(Le32(p + 4)) << 32; }

size_t StrLen(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
bool StartsWith(const char* s, const char* p) { while (*p) if (*s++ != *p++) return false; return true; }
u64 Hash(const char* s, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ u8(s[i])) * 0x100000001b3ull;
    return h;
}

// ---------------------------------------------------------------- archive index
struct Entry { u64 hash; u64 offset; u32 full; u32 comp; };

struct Module {
    char path[64];
    bool found;
    u64 data_start;
    Entry* table;   // open-addressed hash table of "images/" members, keyed by bare name
    u32 cap;
    u32 count;
    int status;     // 0 not tried, 1 ready, -1 no archive, -2 bad archive
    bool published;
    struct Text* text;
    const EdenDsmodHostApi* host;   // kept for decode_font, which is not handed one
    struct FontState* font;
    struct GameData* data;
};

size_t ReadAt(const EdenDsmodHostApi* host, const char* path, u64 off, void* out, size_t n) {
    return host->read_romfs(host->userdata, path, off, out, n);
}

void Insert(Module* m, u64 h, u64 off, u32 full, u32 comp) {
    u32 i = u32(h) & (m->cap - 1);
    while (m->table[i].hash) {
        if (m->table[i].hash == h) return;
        i = (i + 1) & (m->cap - 1);
    }
    m->table[i] = Entry{h, off, full, comp};
    ++m->count;
}

const Entry* Find(const Module* m, u64 h) {
    if (!m->table) return nullptr;
    u32 i = u32(h) & (m->cap - 1);
    while (m->table[i].hash) {
        if (m->table[i].hash == h) return &m->table[i];
        i = (i + 1) & (m->cap - 1);
    }
    return nullptr;
}

void BuildIndex(Module* m, const EdenDsmodHostApi* host) {
    static const char* const kPaths[] = {"app_0.nx64.mvgl", "/app_0.nx64.mvgl", "romfs:/app_0.nx64.mvgl"};
    m->status = -1;
    for (const char* p : kPaths) {
        if (ReadAt(host, p, 0, nullptr, 0) > 0x20) {
            size_t n = StrLen(p);
            memcpy(m->path, p, n + 1);
            m->found = true;
            break;
        }
    }
    if (!m->found) return;
    m->status = -2;
    u8 hdr[0x20];
    if (ReadAt(host, m->path, 0, hdr, 0x20) != 0x20) return;
    if (!(hdr[0] == 'M' && hdr[1] == 'D' && hdr[2] == 'B' && hdr[3] == '1')) return;
    u32 fc = Le32(hdr + 4), nc = Le32(hdr + 8), dc = Le32(hdr + 12);
    m->data_start = Le64(hdr + 16);
    if (fc == 0 || fc > 4000000 || nc > 4000000 || dc > 4000000) return;
    u64 tree_at = 0x20, names_at = tree_at + u64(fc) * 16, data_at = names_at + u64(nc) * 0x80;
    size_t tree_n = size_t(fc) * 16, names_n = size_t(nc) * 0x80, data_n = size_t(dc) * 16;
    u8* tree = (u8*)malloc(tree_n);
    u8* names = (u8*)malloc(names_n);
    u8* data = (u8*)malloc(data_n);
    bool ok = tree && names && data && ReadAt(host, m->path, tree_at, tree, tree_n) == tree_n &&
              ReadAt(host, m->path, data_at, data, data_n) == data_n;
    // names can be large; read in chunks below the 64 MiB host limit
    for (size_t done = 0; ok && done < names_n;) {
        size_t step = names_n - done > (32u << 20) ? (32u << 20) : names_n - done;
        ok = ReadAt(host, m->path, names_at + done, names + done, step) == step;
        done += step;
    }
    if (ok) {
        m->cap = 1;
        while (m->cap < 4096 || m->cap < fc / 8) m->cap <<= 1;
        m->table = (Entry*)calloc(m->cap, sizeof(Entry));
        ok = m->table != nullptr;
    }
    if (ok) {
        for (u32 i = 1; i < fc && i < nc; ++i) {
            u32 did = Le32(tree + size_t(i) * 16 + 4);
            if (did >= dc) continue;
            const char* ext = (const char*)names + size_t(i) * 0x80;
            if (!(ext[0] == 'i' && ext[1] == 'm' && ext[2] == 'g')) continue;
            const char* nm = ext + 4;
            size_t len = 0;
            while (len < 0x7C && nm[len]) ++len;
            if (len < 8 || !(StartsWith(nm, "images\\") || StartsWith(nm, "images/"))) continue;
            const u8* d = data + size_t(did) * 16;
            if (m->count * 2 >= m->cap) break;
            Insert(m, Hash(nm + 7, len - 7), Le64(d), Le32(d + 8), Le32(d + 12));
        }
        m->status = 1;
    }
    free(tree); free(names); free(data);
}

// ---------------------------------------------------------------- LZ4 block
bool Lz4(const u8* src, size_t n, u8* dst, size_t full) {
    size_t i = 0, o = 0;
    while (i < n) {
        u8 t = src[i++];
        size_t ll = t >> 4;
        if (ll == 15) { u8 b; do { if (i >= n) return false; b = src[i++]; ll += b; } while (b == 255); }
        if (i + ll > n || o + ll > full) return false;
        memcpy(dst + o, src + i, ll); i += ll; o += ll;
        if (i >= n || o >= full) break;
        if (i + 2 > n) return false;
        size_t off = src[i] | size_t(src[i + 1]) << 8; i += 2;
        size_t ml = t & 15;
        if (ml == 15) { u8 b; do { if (i >= n) return false; b = src[i++]; ml += b; } while (b == 255); }
        ml += 4;
        if (off == 0 || off > o || o + ml > full) return false;
        for (size_t k = 0; k < ml; ++k) dst[o + k] = dst[o - off + k];
        o += ml;
    }
    return o == full;
}


// ---------------------------------------------------------------- small helpers
void* Grow(void* p, size_t old_bytes, size_t new_bytes) {
    void* q = malloc(new_bytes);
    if (!q) return nullptr;
    if (p) { memcpy(q, p, old_bytes); free(p); }
    return q;
}
char* Dup(const char* s, size_t n) {
    char* d = (char*)malloc(n + 1);
    if (!d) return nullptr;
    memcpy(d, s, n); d[n] = 0;
    return d;
}
bool Eq(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
size_t PutInt(char* o, int64_t v) {
    char t[24]; size_t n = 0;
    bool neg = v < 0; u64 u = neg ? u64(-v) : u64(v);
    do { t[n++] = char('0' + u % 10); u /= 10; } while (u);
    size_t k = 0;
    if (neg) o[k++] = '-';
    while (n) o[k++] = t[--n];
    o[k] = 0;
    return k;
}

// A growable byte buffer.
struct Buf {
    char* p; size_t n, cap;
    bool Put(const char* s, size_t k) {
        if (n + k + 1 > cap) {
            size_t c = cap ? cap * 2 : 64;
            while (c < n + k + 1) c *= 2;
            char* q = (char*)Grow(p, n, c);
            if (!q) return false;
            p = q; cap = c;
        }
        memcpy(p + n, s, k); n += k; p[n] = 0;
        return true;
    }
    bool Ch(char c) { return Put(&c, 1); }
};

// ---------------------------------------------------------------- text cleaning (== public_text.clean)
bool IsWs(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

void Walk(const char* t, size_t n, Buf* out) {
    size_t i = 0;
    while (i < n) {
        if (t[i] == '{') {
            int depth = 1; size_t j = i + 1;
            while (j < n && depth) { depth += (t[j] == '{') - (t[j] == '}'); ++j; }
            size_t a = i + 1, b = depth == 0 ? j - 1 : j;   // inner = t[a, b)
            if (b - a >= 3 && t[a] == 'f' && t[a + 1] == 'c' && t[a + 2] >= '0' && t[a + 2] <= '9') {
                size_t k = a + 2;
                while (k < b && t[k] >= '0' && t[k] <= '9') ++k;
                Walk(t + k, b - k, out);
            }
            i = j;
        } else {
            out->Ch(t[i]);
            ++i;
        }
    }
}

// Cleaned copy of t[0, n) in *out (out->n is its length).
void Clean(const char* t, size_t n, Buf* out, Buf* tmp) {
    tmp->n = 0; if (tmp->p) tmp->p[0] = 0;
    Walk(t, n, tmp);
    out->n = 0; if (out->p) out->p[0] = 0;
    bool pending = false;
    for (size_t i = 0; i < tmp->n; ++i) {
        u8 c = u8(tmp->p[i]);
        if (IsWs(c)) { pending = out->n > 0; continue; }
        if (pending) { out->Ch(' '); pending = false; }
        out->Ch(char(c));
    }
    if (!out->p) out->Ch(0), out->n = 0;
}

// ---------------------------------------------------------------- minimal JSON (the dsts_text block)
struct JVal {
    enum Kind { Null, Num, Str, Arr, Obj } kind;
    double num;
    char* str;                  // Str: the decoded text; Obj: unused
    JVal** items; char** keys;  // Arr: items; Obj: keys + items
    size_t count;
};

struct JParse {
    const char* p; const char* end; bool bad;
    void Ws() { while (p < end && IsWs(u8(*p))) ++p; }
    JVal* New(JVal::Kind k) { JVal* v = (JVal*)calloc(1, sizeof(JVal)); if (v) v->kind = k; else bad = true; return v; }
    static void Utf8(Buf* b, u32 c) {
        if (c < 0x80) b->Ch(char(c));
        else if (c < 0x800) { b->Ch(char(0xC0 | c >> 6)); b->Ch(char(0x80 | (c & 63))); }
        else if (c < 0x10000) { b->Ch(char(0xE0 | c >> 12)); b->Ch(char(0x80 | ((c >> 6) & 63))); b->Ch(char(0x80 | (c & 63))); }
        else { b->Ch(char(0xF0 | c >> 18)); b->Ch(char(0x80 | ((c >> 12) & 63))); b->Ch(char(0x80 | ((c >> 6) & 63))); b->Ch(char(0x80 | (c & 63))); }
    }
    u32 Hex4() {
        u32 v = 0;
        for (int k = 0; k < 4; ++k) {
            if (p >= end) { bad = true; return 0; }
            char c = *p++; v <<= 4;
            if (c >= '0' && c <= '9') v |= u32(c - '0');
            else if (c >= 'a' && c <= 'f') v |= u32(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= u32(c - 'A' + 10);
            else bad = true;
        }
        return v;
    }
    char* String() {   // at the opening quote
        ++p;
        Buf b{};
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { b.Ch(c); continue; }
            if (p >= end) { bad = true; break; }
            char e = *p++;
            switch (e) {
            case 'n': b.Ch('\n'); break; case 't': b.Ch('\t'); break; case 'r': b.Ch('\r'); break;
            case 'b': b.Ch('\b'); break; case 'f': b.Ch('\f'); break;
            case 'u': {
                u32 c1 = Hex4();
                if (c1 >= 0xD800 && c1 < 0xDC00 && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
                    p += 2; u32 c2 = Hex4();
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                }
                Utf8(&b, c1); break;
            }
            default: b.Ch(e);
            }
        }
        if (p < end) ++p; else bad = true;
        if (!b.p) b.Ch(0), b.n = 0;
        return b.p;
    }
    JVal* Value(int depth) {
        Ws();
        if (p >= end || depth > 16) { bad = true; return nullptr; }
        char c = *p;
        if (c == '"') { JVal* v = New(JVal::Str); if (v) v->str = String(); return v; }
        if (c == '{' || c == '[') {
            bool obj = c == '{'; ++p;
            JVal* v = New(obj ? JVal::Obj : JVal::Arr);
            if (!v) return nullptr;
            size_t cap = 0;
            Ws();
            if (p < end && *p == (obj ? '}' : ']')) { ++p; return v; }
            while (!bad) {
                char* key = nullptr;
                if (obj) {
                    Ws();
                    if (p >= end || *p != '"') { bad = true; break; }
                    key = String(); Ws();
                    if (p >= end || *p != ':') { bad = true; free(key); break; }
                    ++p;
                }
                JVal* item = Value(depth + 1);
                if (v->count == cap) {
                    size_t nc = cap ? cap * 2 : 8;
                    v->items = (JVal**)Grow(v->items, cap * sizeof(JVal*), nc * sizeof(JVal*));
                    if (obj) v->keys = (char**)Grow(v->keys, cap * sizeof(char*), nc * sizeof(char*));
                    if (!v->items || (obj && !v->keys)) { bad = true; break; }
                    cap = nc;
                }
                v->items[v->count] = item;
                if (obj) v->keys[v->count] = key;
                ++v->count;
                Ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == (obj ? '}' : ']')) { ++p; break; }
                bad = true;
            }
            return v;
        }
        // number / literal
        JVal* v = New(JVal::Num);
        if (!v) return nullptr;
        bool neg = false; double num = 0;
        if (*p == '-') { neg = true; ++p; }
        if (p < end && (*p == 't' || *p == 'f' || *p == 'n')) {
            v->kind = JVal::Null;
            while (p < end && *p >= 'a' && *p <= 'z') ++p;
            return v;
        }
        while (p < end && *p >= '0' && *p <= '9') num = num * 10 + (*p++ - '0');
        if (p < end && *p == '.') { ++p; double f = 0.1; while (p < end && *p >= '0' && *p <= '9') { num += (*p++ - '0') * f; f /= 10; } }
        if (p < end && (*p == 'e' || *p == 'E')) { bad = true; }
        v->num = neg ? -num : num;
        return v;
    }
};

void JFree(JVal* v) {
    if (!v) return;
    for (size_t i = 0; i < v->count; ++i) { JFree(v->items[i]); if (v->keys) free(v->keys[i]); }
    free(v->items); free(v->keys); free(v->str); free(v);
}
const JVal* JGet(const JVal* o, const char* key) {
    if (!o || o->kind != JVal::Obj) return nullptr;
    for (size_t i = 0; i < o->count; ++i) if (o->keys[i] && Eq(o->keys[i], key)) return o->items[i];
    return nullptr;
}

// ---------------------------------------------------------------- text state
struct IntList { int32_t* v; size_t n; };
struct PageSlots { char* page; IntList slots; };
struct KeyTable { char* name; char* pub; IntList rows; };
struct KeyGroup { char* page; char* flag; int mode; KeyTable* tables; size_t ntables; };
// A page's per-row tables (repeat templates): "tb.<name>.<row>" for every row.
struct RowSet { char* page; KeyTable* tables; size_t ntables; };
// Dex row pictures: "module:dsts:dg<k>_<row>" -> ui_chara_icon_<id>.
struct ImgList { char* name; IntList ids; };

struct Text {
    u64* hashes; size_t nrefs;      // the references, in manifest order
    u32* order;                      // ref indexes sorted by hash
    char** ref_text; size_t found;   // resolved (cleaned) game strings
    char** tpl; char** shown; size_t ntpl;
    PageSlots* pages; size_t npages;
    KeyGroup* groups; size_t ngroups;
    RowSet* rows; size_t nrows;
    ImgList* imgs; size_t nimgs;
    char name[160];
};

bool ToIntList(const JVal* a, IntList* out) {
    if (!a || a->kind != JVal::Arr) return false;
    out->v = (int32_t*)malloc((a->count ? a->count : 1) * sizeof(int32_t));
    if (!out->v) return false;
    out->n = a->count;
    for (size_t i = 0; i < a->count; ++i)
        out->v[i] = (a->items[i] && a->items[i]->kind == JVal::Num) ? int32_t(a->items[i]->num) : -1;
    return true;
}

u64 ParseHex(const char* s) {
    u64 v = 0;
    for (; *s; ++s) {
        char c = *s; v <<= 4;
        if (c >= '0' && c <= '9') v |= u64(c - '0');
        else if (c >= 'a' && c <= 'f') v |= u64(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= u64(c - 'A' + 10);
    }
    return v;
}

void SortOrder(Text* t) {   // insertion sort is too slow for 1-2k; use heap sort
    size_t n = t->nrefs;
    u32* a = t->order;
    for (size_t i = 0; i < n; ++i) a[i] = u32(i);
    auto key = [&](u32 i) { return t->hashes[i]; };
    auto sift = [&](size_t start, size_t end) {
        size_t root = start;
        while (2 * root + 1 < end) {
            size_t child = 2 * root + 1, sw = root;
            if (key(a[sw]) < key(a[child])) sw = child;
            if (child + 1 < end && key(a[sw]) < key(a[child + 1])) sw = child + 1;
            if (sw == root) return;
            u32 tmp = a[root]; a[root] = a[sw]; a[sw] = tmp; root = sw;
        }
    };
    if (n < 2) return;
    for (size_t s = (n - 2) / 2 + 1; s-- > 0;) sift(s, n);
    for (size_t e = n - 1; e > 0; --e) { u32 tmp = a[0]; a[0] = a[e]; a[e] = tmp; sift(0, e); }
}

Text* LoadConfig(const char* cfg) {
    if (!cfg) return nullptr;
    // Find "dsts_text": and parse its value only.
    const char* key = "\"dsts_text\"";
    const char* at = nullptr;
    for (const char* q = cfg; *q; ++q) if (StartsWith(q, key)) { at = q + StrLen(key); break; }
    if (!at) return nullptr;
    while (*at && *at != ':') ++at;
    if (!*at) return nullptr;
    JParse jp{at + 1, at + 1 + StrLen(at + 1), false};
    JVal* root = jp.Value(0);
    Text* t = nullptr;
    const JVal* refs = JGet(root, "refs");
    const JVal* tpl = JGet(root, "tpl");
    const JVal* pages = JGet(root, "pages");
    const JVal* keyed = JGet(root, "keyed");
    if (jp.bad || !refs || refs->kind != JVal::Arr || !tpl || tpl->kind != JVal::Arr) { JFree(root); return nullptr; }
    t = (Text*)calloc(1, sizeof(Text));
    if (!t) { JFree(root); return nullptr; }
    t->nrefs = refs->count;
    t->hashes = (u64*)calloc(t->nrefs + 1, sizeof(u64));
    t->order = (u32*)calloc(t->nrefs + 1, sizeof(u32));
    t->ref_text = (char**)calloc(t->nrefs + 1, sizeof(char*));
    for (size_t i = 0; t->hashes && i < t->nrefs; ++i)
        if (refs->items[i] && refs->items[i]->kind == JVal::Str) t->hashes[i] = ParseHex(refs->items[i]->str);
    t->ntpl = tpl->count;
    t->tpl = (char**)calloc(t->ntpl + 1, sizeof(char*));
    t->shown = (char**)calloc(t->ntpl + 1, sizeof(char*));
    for (size_t i = 0; t->tpl && i < t->ntpl; ++i) {
        const JVal* v = tpl->items[i];
        if (v && v->kind == JVal::Str) { t->tpl[i] = v->str; tpl->items[i]->str = nullptr; }
    }
    if (pages && pages->kind == JVal::Obj) {
        t->pages = (PageSlots*)calloc(pages->count + 1, sizeof(PageSlots));
        for (size_t i = 0; t->pages && i < pages->count; ++i) {
            PageSlots& ps = t->pages[t->npages];
            if (!ToIntList(pages->items[i], &ps.slots)) continue;
            ps.page = Dup(pages->keys[i], StrLen(pages->keys[i]));
            ++t->npages;
        }
    }
    if (keyed && keyed->kind == JVal::Arr) {
        t->groups = (KeyGroup*)calloc(keyed->count + 1, sizeof(KeyGroup));
        for (size_t i = 0; t->groups && i < keyed->count; ++i) {
            const JVal* g = keyed->items[i];
            const JVal* page = JGet(g, "page"); const JVal* flag = JGet(g, "flag");
            const JVal* mode = JGet(g, "mode"); const JVal* tabs = JGet(g, "tables");
            if (!page || page->kind != JVal::Str || !flag || flag->kind != JVal::Str || !tabs || tabs->kind != JVal::Obj) continue;
            KeyGroup& kg = t->groups[t->ngroups];
            kg.page = Dup(page->str, StrLen(page->str));
            Buf f{}; f.Put("@flag:", 6); f.Put(flag->str, StrLen(flag->str));
            kg.flag = f.p;
            kg.mode = mode && mode->kind == JVal::Num ? int(mode->num) : 0;
            kg.tables = (KeyTable*)calloc(tabs->count + 1, sizeof(KeyTable));
            for (size_t k = 0; kg.tables && k < tabs->count; ++k) {
                KeyTable& kt = kg.tables[kg.ntables];
                if (!ToIntList(tabs->items[k], &kt.rows)) continue;
                kt.name = Dup(tabs->keys[k], StrLen(tabs->keys[k]));
                Buf pb{}; pb.Put("tb.", 3); pb.Put(kt.name, StrLen(kt.name));
                kt.pub = pb.p;
                ++kg.ntables;
            }
            ++t->ngroups;
        }
    }
    const JVal* rows = JGet(root, "rows");
    if (rows && rows->kind == JVal::Arr) {
        t->rows = (RowSet*)calloc(rows->count + 1, sizeof(RowSet));
        for (size_t i = 0; t->rows && i < rows->count; ++i) {
            const JVal* page = JGet(rows->items[i], "page"); const JVal* tabs = JGet(rows->items[i], "tables");
            if (!page || page->kind != JVal::Str || !tabs || tabs->kind != JVal::Obj) continue;
            RowSet& rs = t->rows[t->nrows];
            rs.page = Dup(page->str, StrLen(page->str));
            rs.tables = (KeyTable*)calloc(tabs->count + 1, sizeof(KeyTable));
            for (size_t k = 0; rs.tables && k < tabs->count; ++k) {
                KeyTable& kt = rs.tables[rs.ntables];
                if (!ToIntList(tabs->items[k], &kt.rows)) continue;
                kt.name = Dup(tabs->keys[k], StrLen(tabs->keys[k]));
                Buf pb{}; pb.Put("tb.", 3); pb.Put(kt.name, StrLen(kt.name)); pb.Ch('.');
                kt.pub = pb.p;
                ++rs.ntables;
            }
            ++t->nrows;
        }
    }
    const JVal* imgs = JGet(root, "img");
    if (imgs && imgs->kind == JVal::Obj) {
        t->imgs = (ImgList*)calloc(imgs->count + 1, sizeof(ImgList));
        for (size_t i = 0; t->imgs && i < imgs->count; ++i) {
            ImgList& il = t->imgs[t->nimgs];
            if (!ToIntList(imgs->items[i], &il.ids)) continue;
            il.name = Dup(imgs->keys[i], StrLen(imgs->keys[i]));
            ++t->nimgs;
        }
    }
    JFree(root);
    if (!t->hashes || !t->order || !t->ref_text || !t->tpl || !t->shown) return t;
    SortOrder(t);
    return t;
}

// Index of hash h among the references (any one of equal hashes), or -1.
long FindRef(const Text* t, u64 h) {
    size_t lo = 0, hi = t->nrefs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        u64 v = t->hashes[t->order[mid]];
        if (v < h) lo = mid + 1; else hi = mid;
    }
    return (lo < t->nrefs && t->hashes[t->order[lo]] == h) ? long(lo) : -1;
}

void Offer(Text* t, const char* s, size_t n, Buf* clean, Buf* tmp) {
    if (!n) return;
    Clean(s, n, clean, tmp);
    if (!clean->n) return;
    u64 h = Hash(clean->p, clean->n);
    long at = FindRef(t, h);
    if (at < 0) return;
    for (size_t k = size_t(at); k < t->nrefs && t->hashes[t->order[k]] == h; ++k) {
        u32 r = t->order[k];
        if (!t->ref_text[r]) { t->ref_text[r] = Dup(clean->p, clean->n); if (t->ref_text[r]) ++t->found; }
    }
}

// The string cells of one EXPA (.mbe) file: its CHNK section.
void ScanExpa(Text* t, const u8* b, size_t n, Buf* clean, Buf* tmp) {
    if (n < 8 || !(b[0] == 'E' && b[1] == 'X' && b[2] == 'P' && b[3] == 'A')) return;
    u32 nt = Le32(b + 4);
    size_t p = 8;
    for (u32 k = 0; k < nt && p + 4 <= n; ++k) {
        u32 nl = Le32(b + p); p += 4 + nl;
        if (p + 4 > n) return;
        u32 sc = Le32(b + p); p += 4 + size_t(sc) * 4;
        if (p + 8 > n) return;
        u32 esize = Le32(b + p), ec = Le32(b + p + 4); p += 8;
        p = (p + 7) / 8 * 8;
        size_t stride = (size_t(esize) + 7) / 8 * 8;
        p += stride * ec;
        if (p > n) return;
    }
    size_t ci = p >= 16 ? p - 16 : 0;
    while (ci + 8 <= n && !(b[ci] == 'C' && b[ci + 1] == 'H' && b[ci + 2] == 'N' && b[ci + 3] == 'K')) ++ci;
    if (ci + 8 > n) return;
    u32 cnt = Le32(b + ci + 4);
    size_t q = ci + 8;
    for (u32 k = 0; k < cnt && q + 8 <= n; ++k) {
        u32 sz = Le32(b + q + 4); q += 8;
        if (q + sz > n) return;
        size_t len = 0;
        while (len < sz && b[q + len]) ++len;
        Offer(t, (const char*)b + q, len, clean, tmp);
        q += sz;
    }
}

// Every text table ("text\..." .mbe members) of one MDB1 archive.
void ScanArchive(Text* t, const EdenDsmodHostApi* host, const char* path, bool text_dir_only) {
    u8 hdr[0x20];
    if (ReadAt(host, path, 0, hdr, 0x20) != 0x20) return;
    if (!(hdr[0] == 'M' && hdr[1] == 'D' && hdr[2] == 'B' && hdr[3] == '1')) return;
    u32 fc = Le32(hdr + 4), nc = Le32(hdr + 8), dc = Le32(hdr + 12);
    u64 data_start = Le64(hdr + 16);
    if (fc == 0 || fc > 4000000 || nc > 4000000 || dc > 4000000) return;
    u64 tree_at = 0x20, names_at = tree_at + u64(fc) * 16, data_at = names_at + u64(nc) * 0x80;
    size_t tree_n = size_t(fc) * 16, names_n = size_t(nc) * 0x80, data_n = size_t(dc) * 16;
    u8* tree = (u8*)malloc(tree_n);
    u8* names = (u8*)malloc(names_n);
    u8* data = (u8*)malloc(data_n);
    bool ok = tree && names && data && ReadAt(host, path, tree_at, tree, tree_n) == tree_n &&
              ReadAt(host, path, data_at, data, data_n) == data_n;
    for (size_t done = 0; ok && done < names_n;) {
        size_t step = names_n - done > (32u << 20) ? (32u << 20) : names_n - done;
        ok = ReadAt(host, path, names_at + done, names + done, step) == step;
        done += step;
    }
    Buf clean{}, tmp{};
    for (u32 i = 1; ok && i < fc && i < nc && t->found < t->nrefs; ++i) {
        u32 did = Le32(tree + size_t(i) * 16 + 4);
        if (did >= dc) continue;
        const char* ext = (const char*)names + size_t(i) * 0x80;
        if (!(ext[0] == 'm' && ext[1] == 'b' && ext[2] == 'e')) continue;
        const char* nm = ext + 4;
        if (text_dir_only && !(StartsWith(nm, "text\\") || StartsWith(nm, "text/"))) continue;
        const u8* d = data + size_t(did) * 16;
        u64 off = Le64(d); u32 full = Le32(d + 8), comp = Le32(d + 12);
        if (full == 0 || full > (64u << 20) || comp > (64u << 20)) continue;
        u8* raw = (u8*)malloc(comp ? comp : 1);
        if (!raw) continue;
        if (ReadAt(host, path, data_start + off, raw, comp) == comp) {
            if (comp == full) ScanExpa(t, raw, full, &clean, &tmp);
            else {
                u8* un = (u8*)malloc(full);
                if (un && Lz4(raw, comp, un, full)) ScanExpa(t, un, full, &clean, &tmp);
                free(un);
            }
        }
        free(raw);
    }
    free(clean.p); free(tmp.p);
    free(tree); free(names); free(data);
}

void LoadGameText(Text* t, const EdenDsmodHostApi* host) {
    static const char* const kNames[] = {"app_text01.nx64.mvgl", "app_text00.nx64.mvgl", "app_text02.nx64.mvgl",
                                         "app_text03.nx64.mvgl", "app_text04.nx64.mvgl", "app_text05.nx64.mvgl",
                                         "app_text06.nx64.mvgl", "app_text07.nx64.mvgl", "app_text08.nx64.mvgl",
                                         "app_text09.nx64.mvgl", "app_text.nx64.mvgl", "app_0.nx64.mvgl"};
    static const char* const kPre[] = {"", "/", "romfs:/"};
    for (const char* nm : kNames) {
        if (t->found >= t->nrefs) break;
        for (const char* pre : kPre) {
            char path[64]; size_t a = StrLen(pre), b = StrLen(nm);
            memcpy(path, pre, a); memcpy(path + a, nm, b + 1);
            if (ReadAt(host, path, 0, nullptr, 0) > 0x20) { ScanArchive(t, host, path, nm[4] == '0'); break; }
        }
    }
    // Fill the templates. One whose game text was not found stays unpublished (blank).
    for (size_t i = 0; i < t->ntpl; ++i) {
        const char* s = t->tpl[i];
        if (!s) continue;
        Buf out{}; bool missing = false;
        while (*s) {
            if (*s == '\x01') {
                ++s; long r = 0;
                while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
                char mode = 0;   // l lower, u upper, c first letter upper, f first letter lower (ASCII)
                if (*s >= 'a' && *s <= 'z') mode = *s++;
                if (*s == '\x02') ++s;
                const char* g = (r >= 0 && size_t(r) < t->nrefs) ? t->ref_text[r] : nullptr;
                if (!g) { missing = true; break; }
                size_t at = out.n, gl = StrLen(g);
                out.Put(g, gl);
                for (size_t k = 0; k < gl && out.p; ++k) {
                    char& c = out.p[at + k];
                    bool up = mode == 'u' || (mode == 'c' && k == 0), low = mode == 'l' || (mode == 'f' && k == 0);
                    if (up && c >= 'a' && c <= 'z') c = char(c - 32);
                    if (low && c >= 'A' && c <= 'Z') c = char(c + 32);
                }
            } else {
                out.Ch(*s++);
            }
        }
        if (missing || !out.p) { free(out.p); continue; }
        t->shown[i] = out.p;
    }
}

void PublishText(Text* t, const EdenDsmodHostApi* host) {
    host->publish_i64(host->userdata, "dsts.text", int64_t(t->found));
    host->publish_i64(host->userdata, "dsts.text_total", int64_t(t->nrefs));
    if (!host->get_text) return;
    const char* page = host->get_text(host->userdata, "__page");
    if (!page) return;
    for (size_t i = 0; i < t->npages; ++i) {
        if (!Eq(t->pages[i].page, page)) continue;
        const IntList& sl = t->pages[i].slots;
        for (size_t k = 0; k < sl.n; ++k) {
            int32_t s = sl.v[k];
            if (s < 0 || size_t(s) >= t->ntpl || !t->shown[s]) continue;
            t->name[0] = 's';
            PutInt(t->name + 1, s);
            host->publish_text(host->userdata, t->name, t->shown[s]);
        }
        break;
    }
    for (size_t i = 0; i < t->nrows; ++i) {
        RowSet& rs = t->rows[i];
        if (!Eq(rs.page, page)) continue;
        for (size_t k = 0; k < rs.ntables; ++k) {
            KeyTable& kt = rs.tables[k];
            size_t pl = StrLen(kt.pub);
            if (pl + 24 > sizeof(t->name)) continue;
            memcpy(t->name, kt.pub, pl);
            for (size_t r = 0; r < kt.rows.n; ++r) {
                int32_t s = kt.rows.v[r];
                if (s < 0 || size_t(s) >= t->ntpl || !t->shown[s]) continue;
                PutInt(t->name + pl, int64_t(r));
                host->publish_text(host->userdata, t->name, t->shown[s]);
            }
        }
    }
    for (size_t i = 0; i < t->ngroups; ++i) {
        KeyGroup& g = t->groups[i];
        if (!Eq(g.page, page)) continue;
        int64_t v = host->get_i64 ? host->get_i64(host->userdata, g.flag, -1) : -1;
        if (g.mode == 1) { if (v < 0) v = -v; v %= 1000; }
        if (v < 0) continue;
        for (size_t k = 0; k < g.ntables; ++k) {
            KeyTable& kt = g.tables[k];
            if (size_t(v) >= kt.rows.n) continue;
            int32_t s = kt.rows.v[v];
            if (s < 0 || size_t(s) >= t->ntpl || !t->shown[s]) continue;
            host->publish_text(host->userdata, kt.pub, t->shown[s]);
        }
    }
}

void FreeText(Text* t) {
    if (!t) return;
    for (size_t i = 0; i < t->nrefs; ++i) if (t->ref_text) free(t->ref_text[i]);
    for (size_t i = 0; i < t->ntpl; ++i) { if (t->tpl) free(t->tpl[i]); if (t->shown) free(t->shown[i]); }
    for (size_t i = 0; i < t->npages; ++i) { free(t->pages[i].page); free(t->pages[i].slots.v); }
    for (size_t i = 0; i < t->ngroups; ++i) {
        KeyGroup& g = t->groups[i];
        for (size_t k = 0; k < g.ntables; ++k) { free(g.tables[k].name); free(g.tables[k].pub); free(g.tables[k].rows.v); }
        free(g.tables); free(g.page); free(g.flag);
    }
    for (size_t i = 0; i < t->nrows; ++i) {
        RowSet& rs = t->rows[i];
        for (size_t k = 0; k < rs.ntables; ++k) { free(rs.tables[k].name); free(rs.tables[k].pub); free(rs.tables[k].rows.v); }
        free(rs.tables); free(rs.page);
    }
    for (size_t i = 0; i < t->nimgs; ++i) { free(t->imgs[i].name); free(t->imgs[i].ids.v); }
    free(t->rows); free(t->imgs);
    free(t->hashes); free(t->order); free(t->ref_text); free(t->tpl); free(t->shown); free(t->pages); free(t->groups);
    free(t);
}

#include "010062E01FE0C000_data.inc"

// ---------------------------------------------------------------- BC1 / BC3 / BC7
void Rgb565(u16 c, u8* o) {
    u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    o[0] = u8(r << 3 | r >> 2); o[1] = u8(g << 2 | g >> 4); o[2] = u8(b << 3 | b >> 2);
}

void DecodeBc1(const u8* b, u8* px, bool force4) {  // px: 16 * RGBA
    u16 c0 = u16(b[0] | b[1] << 8), c1 = u16(b[2] | b[3] << 8);
    u8 pal[4][4];
    Rgb565(c0, pal[0]); Rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || force4) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = u8((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = u8((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) { pal[2][k] = u8((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
        pal[2][3] = 255; pal[3][3] = 0;
    }
    u32 idx = Le32(b + 4);
    for (int i = 0; i < 16; ++i) memcpy(px + i * 4, pal[(idx >> (2 * i)) & 3], 4);
}

void DecodeBc3(const u8* b, u8* px) {
    DecodeBc1(b + 8, px, true);
    u8 a[8]; a[0] = b[0]; a[1] = b[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; ++i) a[i + 1] = u8(((7 - i) * a[0] + i * a[1]) / 7);
    else {
        for (int i = 1; i < 5; ++i) a[i + 1] = u8(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0; a[7] = 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; ++i) bits |= u64(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) px[i * 4 + 3] = a[(bits >> (3 * i)) & 7];
}

const u16 kP2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80,
    0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310,
    0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA,
    0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC,
    0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6,
    0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
const u8 kP3[64][16] = {
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2},{0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},{0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},
    {0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},{0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2},{0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},
    {0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1},{0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},{0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},
    {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2},{0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2},{0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},{0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},
    {0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},{0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2},{0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},
    {0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2},{0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},{0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},
    {0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},{0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2},{0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0},{0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},{0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},
    {0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},{0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2},{0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},
    {0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1},{0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},{0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},
    {0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},{0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2},{0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0},{0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},{0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},
    {0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},{0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1},{0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},
    {0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1},{0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},{0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},
    {0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},{0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1},{0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2},{0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},{0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},
    {0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},{0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2},{0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},
    {0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},{0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},
    {0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},{0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2},{0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1},{0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},{0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},
    {0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0}};
const u8 kA2[64] = {15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,2,8,2,2,8,8,15,2,8,2,2,8,8,2,2,
                    15,15,6,8,2,8,15,15,2,8,2,2,2,15,15,6,6,2,6,8,15,15,2,2,15,15,15,15,15,2,2,15};
const u8 kA3a[64] = {3,3,15,15,8,3,15,15,8,8,6,6,6,5,3,3,3,3,8,15,3,3,6,10,5,8,8,6,8,5,15,15,
                     8,15,3,5,6,10,8,15,15,3,15,5,15,15,15,15,3,15,5,5,5,8,5,10,5,10,8,13,15,12,3,3};
const u8 kA3b[64] = {15,8,8,3,15,15,3,8,15,15,15,15,15,15,15,8,15,8,15,3,15,8,15,8,3,15,6,10,15,15,10,8,
                     15,3,15,10,10,8,9,10,6,15,8,15,3,6,6,8,15,3,15,15,15,15,15,15,15,15,15,15,3,15,15,8};
const u8 kW2[4] = {0, 21, 43, 64};
const u8 kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const u8 kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Bits {
    const u8* b; u32 pos;
    u32 Get(u32 n) {
        u32 v = 0;
        for (u32 i = 0; i < n; ++i, ++pos) v |= u32((b[pos >> 3] >> (pos & 7)) & 1) << i;
        return v;
    }
};

u8 Interp(u32 e0, u32 e1, u32 idx, u32 bits) {
    u32 w = bits == 2 ? kW2[idx] : bits == 3 ? kW3[idx] : kW4[idx];
    return u8(((64 - w) * e0 + w * e1 + 32) >> 6);
}
u8 Expand(u32 v, u32 bits) { v <<= (8 - bits); return u8(v | (v >> bits)); }

void DecodeBc7(const u8* blk, u8* px) {
    static const u8 kNs[8] = {3, 2, 3, 2, 1, 1, 1, 2}, kPb[8] = {4, 6, 6, 6, 0, 0, 0, 6},
                    kRb[8] = {0, 0, 0, 0, 2, 2, 0, 0}, kIsb[8] = {0, 0, 0, 0, 1, 0, 0, 0},
                    kCb[8] = {4, 6, 5, 7, 5, 7, 7, 5}, kAb[8] = {0, 0, 0, 0, 6, 8, 7, 5},
                    kEpb[8] = {1, 0, 0, 1, 0, 0, 1, 1}, kSpb[8] = {0, 1, 0, 0, 0, 0, 0, 0},
                    kIb[8] = {3, 3, 2, 2, 2, 2, 4, 2}, kIb2[8] = {0, 0, 0, 0, 3, 2, 0, 0};
    u32 mode = 0;
    while (mode < 8 && !((blk[0] >> mode) & 1)) ++mode;
    if (mode == 8) { memset(px, 0, 64); return; }
    Bits r{blk, mode + 1};
    u32 ns = kNs[mode], part = r.Get(kPb[mode]), rot = r.Get(kRb[mode]), isel = r.Get(kIsb[mode]);
    u32 ep[6][4];
    for (u32 c = 0; c < 3; ++c)
        for (u32 e = 0; e < ns * 2; ++e) ep[e][c] = r.Get(kCb[mode]);
    for (u32 e = 0; e < ns * 2; ++e) ep[e][3] = kAb[mode] ? r.Get(kAb[mode]) : 255;
    u32 cb = kCb[mode], ab = kAb[mode];
    if (kEpb[mode] || kSpb[mode]) {
        u32 pb[6];
        if (kEpb[mode]) for (u32 e = 0; e < ns * 2; ++e) pb[e] = r.Get(1);
        else for (u32 s = 0; s < ns; ++s) { pb[2 * s] = pb[2 * s + 1] = r.Get(1); }
        for (u32 e = 0; e < ns * 2; ++e) {
            for (u32 c = 0; c < 3; ++c) ep[e][c] = ep[e][c] << 1 | pb[e];
            if (ab) ep[e][3] = ep[e][3] << 1 | pb[e];
        }
        ++cb; if (ab) ++ab;
    }
    for (u32 e = 0; e < ns * 2; ++e) {
        for (u32 c = 0; c < 3; ++c) ep[e][c] = Expand(ep[e][c], cb);
        if (ab) ep[e][3] = Expand(ep[e][3], ab);
    }
    u8 sub[16];
    for (u32 i = 0; i < 16; ++i)
        sub[i] = ns == 1 ? 0 : ns == 2 ? u8((kP2[part] >> i) & 1) : kP3[part][i];
    auto anchor = [&](u32 i) {
        if (i == 0) return true;
        if (ns == 2) return i == kA2[part];
        if (ns == 3) return i == kA3a[part] || i == kA3b[part];
        return false;
    };
    u32 ib = kIb[mode], ib2 = kIb2[mode], idx1[16], idx2[16];
    for (u32 i = 0; i < 16; ++i) idx1[i] = r.Get(anchor(i) ? ib - 1 : ib);
    if (ib2) for (u32 i = 0; i < 16; ++i) idx2[i] = r.Get(i == 0 ? ib2 - 1 : ib2);
    for (u32 i = 0; i < 16; ++i) {
        u32 s = sub[i];
        const u32* e0 = ep[2 * s];
        const u32* e1 = ep[2 * s + 1];
        u8* o = px + i * 4;
        if (!ib2) {
            for (u32 c = 0; c < 4; ++c) o[c] = Interp(e0[c], e1[c], idx1[i], ib);
        } else {
            u32 ci = isel ? idx2[i] : idx1[i], cbits = isel ? ib2 : ib;
            u32 ai = isel ? idx1[i] : idx2[i], abits = isel ? ib : ib2;
            for (u32 c = 0; c < 3; ++c) o[c] = Interp(e0[c], e1[c], ci, cbits);
            o[3] = Interp(e0[3], e1[3], ai, abits);
        }
        if (!ab) o[3] = 255;
        if (rot) { u8 t = o[3]; o[3] = o[rot - 1]; o[rot - 1] = t; }
    }
}

// Summary-page portraits: a circle with a soft edge (same maths as build_dsts_package.round_portrait).
void RoundMask(u8* rgba, u32 w, u32 h) {
    const float cx = w * 0.5f, cy = h * 0.5f, rr = 1.0f * (w < h ? w : h) * 0.5f, edge = 1.5f;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float k = (rr - __builtin_sqrtf(dx * dx + dy * dy)) / edge + 0.5f;
            if (k >= 1.0f) continue;
            if (k < 0.0f) k = 0.0f;
            u8* a = rgba + (size_t(y) * w + x) * 4 + 3;
            *a = u8(*a * k + 0.5f);
        }
}

// DDS (DX10 header) -> RGBA8, flipped upright. Returns malloc'd pixels.
u8* DecodeDds(const u8* f, size_t n, u32* w_out, u32* h_out) {
    if (n < 148 || !(f[0] == 'D' && f[1] == 'D' && f[2] == 'S' && f[3] == ' ')) return nullptr;
    u32 h = Le32(f + 12), w = Le32(f + 16);
    if (!(f[84] == 'D' && f[85] == 'X' && f[86] == '1' && f[87] == '0')) return nullptr;
    u32 fmt = Le32(f + 128);
    u32 bsize = (fmt == 71 || fmt == 72) ? 8 : (fmt == 77 || fmt == 78 || fmt == 98 || fmt == 99) ? 16 : 0;
    if (!bsize || w == 0 || h == 0 || w > 4096 || h > 4096) return nullptr;
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (148 + size_t(bw) * bh * bsize > n) return nullptr;
    u8* out = (u8*)malloc(size_t(w) * h * 4);
    if (!out) return nullptr;
    u8 px[64];
    const u8* blk = f + 148;
    for (u32 by = 0; by < bh; ++by)
        for (u32 bx = 0; bx < bw; ++bx, blk += bsize) {
            if (bsize == 8) DecodeBc1(blk, px, false);
            else if (fmt == 77 || fmt == 78) DecodeBc3(blk, px);
            else DecodeBc7(blk, px);
            for (u32 y = 0; y < 4; ++y) {
                u32 yy = by * 4 + y;
                if (yy >= h) break;
                u32 oy = h - 1 - yy;  // stored upside down
                for (u32 x = 0; x < 4; ++x) {
                    u32 xx = bx * 4 + x;
                    if (xx >= w) break;
                    memcpy(out + (size_t(oy) * w + xx) * 4, px + (y * 4 + x) * 4, 4);
                }
            }
        }
    *w_out = w; *h_out = h;
    return out;
}

// ---------------------------------------------------------------- module callbacks
EdenDsmodBool SupportsBuild(const char* id) {
    return (id && StartsWith(id, kBuild)) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

#include "010062E01FE0C000_font.inc"

// ---------------------------------------------------------------- game font
struct FontState { int state; font::Built built; };   // state: 0 not tried, 1 ready, -1 failed

// Reads "font\commonfont" (OpenType/CFF) out of the game's app_text01 archive.
u8* ReadFontFile(const EdenDsmodHostApi* host, size_t* size) {
    static const char* const kPaths[] = {"app_text01.nx64.mvgl", "/app_text01.nx64.mvgl", "romfs:/app_text01.nx64.mvgl"};
    for (const char* path : kPaths) {
        if (ReadAt(host, path, 0, nullptr, 0) <= 0x20) continue;
        u8 hdr[0x20];
        if (ReadAt(host, path, 0, hdr, 0x20) != 0x20 || !(hdr[0] == 'M' && hdr[1] == 'D' && hdr[2] == 'B' && hdr[3] == '1')) return nullptr;
        u32 fc = Le32(hdr + 4), nc = Le32(hdr + 8), dc = Le32(hdr + 12);
        u64 data_start = Le64(hdr + 16);
        if (fc == 0 || fc > 4000000 || nc > 4000000 || dc > 4000000) return nullptr;
        u64 tree_at = 0x20, names_at = tree_at + u64(fc) * 16, data_at = names_at + u64(nc) * 0x80;
        u8 rec[16], nm[0x80];
        for (u32 i = 1; i < fc && i < nc; ++i) {
            if (ReadAt(host, path, names_at + u64(i) * 0x80, nm, 0x80) != 0x80) return nullptr;
            const char* name = (const char*)nm + 4;
            if (!(nm[0] == 'b' && nm[1] == 'i' && nm[2] == 'n')) continue;
            if (!(Eq(name, "font\\commonfont") || Eq(name, "font/commonfont") || Eq(name, "commonfont"))) continue;
            if (ReadAt(host, path, tree_at + u64(i) * 16, rec, 16) != 16) return nullptr;
            u32 did = Le32(rec + 4);
            if (did >= dc) continue;
            u8 d[16];
            if (ReadAt(host, path, data_at + u64(did) * 16, d, 16) != 16) return nullptr;
            u64 off = Le64(d); u32 full = Le32(d + 8), comp = Le32(d + 12);
            if (full == 0 || full > (32u << 20) || comp > (32u << 20)) return nullptr;
            u8* raw = (u8*)malloc(comp ? comp : 1);
            if (!raw) return nullptr;
            if (ReadAt(host, path, data_start + off, raw, comp) != comp) { free(raw); return nullptr; }
            if (comp == full) { *size = full; return raw; }
            u8* un = (u8*)malloc(full);
            bool ok = un && Lz4(raw, comp, un, full);
            free(raw);
            if (!ok) { free(un); return nullptr; }
            *size = full; return un;
        }
        return nullptr;
    }
    return nullptr;
}

FontState* EnsureFont(Module* m, const EdenDsmodHostApi* host) {
    if (!m) return nullptr;
    if (!m->font) m->font = (FontState*)calloc(1, sizeof(FontState));
    FontState* f = m->font;
    if (!f || f->state != 0 || !host) return f;
    size_t n = 0;
    u8* otf = ReadFontFile(host, &n);
    f->state = (otf && font::Build(otf, n, &f->built)) ? 1 : -1;
    free(otf);
    return f;
}

// Eden hands us the package's font asset (a small marker file); the font itself comes from the game.
EdenDsmodBool DecodeFont(void* inst, const uint8_t*, size_t, void* receiver, EdenDsmodFontSink sink) {
    Module* m = (Module*)inst;
    FontState* f = EnsureFont(m, m ? m->host : nullptr);
    if (!f || f->state != 1) return EDEN_DSMOD_FALSE;
    sink(receiver, f->built.line_height, font::kFirst, f->built.glyph, font::kCount);
    return EDEN_DSMOD_TRUE;
}

void* Create(const EdenDsmodHostApi* host, const char* config) {
    if (!host || host->abi_version != EDEN_DSMOD_MODULE_ABI_VERSION || !host->read_romfs) return nullptr;
    Module* m = (Module*)calloc(1, sizeof(Module));
    if (!m) return nullptr;
    m->host = host;
    BuildIndex(m, host);
    m->text = LoadConfig(config);
    if (m->text && m->text->hashes && m->text->tpl && m->text->shown) LoadGameText(m->text, host);
    m->data = LoadGameData(host, config);
    return m;
}

void Destroy(void* inst) {
    Module* m = (Module*)inst;
    if (!m) return;
    free(m->table);
    FreeText(m->text);
    FreeGameData(m->data);
    if (m->font) { free(m->font->built.atlas); free(m->font); }
    free(m);
}

void Sample(void* inst, const EdenDsmodHostApi* host) {
    Module* m = (Module*)inst;
    if (!m) return;
    // The host clears its snapshot every tick, so these are published on every sample.
    host->publish_i64(host->userdata, "dsts.module", 1);
    host->publish_i64(host->userdata, "dsts.archive", m->status);
    host->publish_i64(host->userdata, "dsts.pictures", m->count);
    if (m->text) PublishText(m->text, host);
    if (m->data) PublishGameData(m->data, m->text, host);
}

void Tick(void*, const EdenDsmodHostApi*) {}

void Configure(void*, const EdenDsmodHostExtensions*) {}
EdenDsmodBool OnAction(void*, const char*, int64_t) { return EDEN_DSMOD_FALSE; }

EdenDsmodBool LoadImage(void* inst, const EdenDsmodHostApi* host, const char* key, void* receiver,
                        EdenDsmodImageSink sink) {
    Module* m = (Module*)inst;
    if (m && key && Eq(key, "module:dsts:font")) {   // the game font's glyph atlas
        FontState* f = EnsureFont(m, host);
        if (!f || f->state != 1) return EDEN_DSMOD_FALSE;
        sink(receiver, f->built.aw, f->built.ah, f->built.atlas, size_t(f->built.aw) * f->built.ah * 4);
        return EDEN_DSMOD_TRUE;
    }
    if (!m || m->status != 1 || !key) return EDEN_DSMOD_FALSE;
    char alias[48];
    if (StartsWith(key, "module:dsts:dg") && m->text) {
        // A dex row picture: "dg<k>_<row>" -> that row's portrait.
        const char* q = key + 12;          // "dg<k>_<row>"
        const char* us = q; while (*us && *us != '_') ++us;
        if (!*us || us - q > 8) return EDEN_DSMOD_FALSE;
        char grp[12]; memcpy(grp, q, size_t(us - q)); grp[us - q] = 0;
        long row = 0; for (const char* d = us + 1; *d; ++d) { if (*d < '0' || *d > '9') return EDEN_DSMOD_FALSE; row = row * 10 + (*d - '0'); }
        int32_t id = -1;
        for (size_t i = 0; i < m->text->nimgs; ++i)
            if (Eq(m->text->imgs[i].name, grp) && size_t(row) < m->text->imgs[i].ids.n) id = m->text->imgs[i].ids.v[row];
        if (id < 0) return EDEN_DSMOD_FALSE;
        memcpy(alias, "module:dsts:ui_chara_icon_", 26);
        PutInt(alias + 26, id);
        key = alias;
    }
    char icon[48];
    if (StartsWith(key, "module:dsts:itemicon_")) {   // an item's icon, by item id
        int64_t id = 0;
        for (const char* d = key + 21; *d; ++d) { if (*d < '0' || *d > '9') return EDEN_DSMOD_FALSE; id = id * 10 + (*d - '0'); }
        if (!ItemIconKey(m->data, id, icon, sizeof(icon))) return EDEN_DSMOD_FALSE;
        key = icon;
    }
    bool round = false;
    char plain[64];
    if (StartsWith(key, "module:dsts:round_")) {
        size_t n = StrLen(key + 18);
        if (n + 13 > sizeof(plain)) return EDEN_DSMOD_FALSE;
        memcpy(plain, "module:dsts:", 12); memcpy(plain + 12, key + 18, n + 1);
        key = plain; round = true;
    }
    if (!StartsWith(key, "module:dsts:ui_")) return EDEN_DSMOD_FALSE;
    const char* name = key + 12;
    size_t len = StrLen(name);
    if (len > 100) return EDEN_DSMOD_FALSE;
    const Entry* e = Find(m, Hash(name, len));
    if (!e || e->full > (16u << 20) || e->comp > (16u << 20)) return EDEN_DSMOD_FALSE;
    u8* raw = (u8*)malloc(e->comp ? e->comp : 1);
    if (!raw) return EDEN_DSMOD_FALSE;
    EdenDsmodBool ok = EDEN_DSMOD_FALSE;
    if (ReadAt(host, m->path, m->data_start + e->offset, raw, e->comp) == e->comp) {
        u8* file = raw;
        u8* unpacked = nullptr;
        if (e->comp != e->full) {
            unpacked = (u8*)malloc(e->full);
            if (unpacked && Lz4(raw, e->comp, unpacked, e->full)) file = unpacked;
            else file = nullptr;
        }
        u32 w = 0, h = 0;
        u8* rgba = file ? DecodeDds(file, e->full, &w, &h) : nullptr;
        if (rgba) {
            if (round) RoundMask(rgba, w, h);
            sink(receiver, w, h, rgba, size_t(w) * h * 4);
            free(rgba);
            ok = EDEN_DSMOD_TRUE;
        }
        free(unpacked);
    }
    free(raw);
    return ok;
}

const EdenDsmodModuleApi kApi = {
    EDEN_DSMOD_MODULE_ABI_VERSION, sizeof(EdenDsmodModuleApi), 0, EDEN_DSMOD_MODULE_ABI_HASH, kTitle,
    "Digimon Story Time Stranger guide",
    EDEN_DSMOD_CAP_ROMFS_READ | EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN,
    SupportsBuild, Create, Destroy, Sample, Tick};

const EdenDsmodModuleExtensions kExt = {EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
                                        EDEN_DSMOD_EXT_HASH, Configure, OnAction, LoadImage};

const EdenDsmodFontExtensions kFontExt = {EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions),
                                          EDEN_DSMOD_FONT_EXT_HASH, DecodeFont};

} // namespace

EXPORT const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t v, uint64_t h) {
    return (v == EDEN_DSMOD_MODULE_ABI_VERSION && h == EDEN_DSMOD_MODULE_ABI_HASH) ? &kApi : nullptr;
}

EXPORT const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t v, uint64_t h) {
    return (v == EDEN_DSMOD_EXT_VERSION && h == EDEN_DSMOD_EXT_HASH) ? &kExt : nullptr;
}

EXPORT const EdenDsmodFontExtensions* eden_dsmod_get_font_extensions(uint32_t v, uint64_t h) {
    return (v == EDEN_DSMOD_FONT_EXT_VERSION && h == EDEN_DSMOD_FONT_EXT_HASH) ? &kFontExt : nullptr;
}
