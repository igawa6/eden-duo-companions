// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Digimon Story Time Stranger companion: the game data layer (included by 010062E01FE0C000.cpp).
//
// Everything the guide shows about items, chests, wild Digimon and side-quest rewards is read here at
// run time from the player's own game tables, never shipped in the package:
//   data tables (EXPA .mbe):  item, field_treasure, shop, quest_sub, battle_enemy, battle_skill,
//                             field_battle_<map>
//   text tables (EXPA .mbe):  item_name, item_explanation, field_name, skill_name, buff_name
// The manifest's "dsts_data" block carries only identifiers (item ids, map ids, quest ids, dex
// numbers) saying which entries a page shows; the values and the wording that comes from the game
// are worked out here and published for the page on screen. tools/digimon-time-stranger/
// check_gamedata.py is the reference these results are tested against.

// ---------------------------------------------------------------- EXPA tables
struct ExpaStr { u32 off; u32 at; u32 len; };
struct Expa {
    u8* b; size_t n;
    ExpaStr* strs; size_t nstr;   // CHNK cells, sorted by the cell offset they patch
};
struct ExpaTab {
    const Expa* e;
    size_t start, stride; u32 count, ncol, esize;
    u32 off[160]; u8 ty[160]; u8 bit[160];
};

void ExpaSize(u32 t, u32* al, u32* sz) {
    switch (t) {
    case 3: *al = 2; *sz = 2; break;
    case 4: *al = 1; *sz = 1; break;
    case 6: case 7: case 8: *al = 8; *sz = 8; break;
    default: *al = 4; *sz = 4; break;
    }
}

// Column offsets, as the game lays a row out (bit columns share 32-bit words). With z10 a type-10
// column takes no space (some tables need that to fit their row size).
u32 ExpaLayout(ExpaTab* t, const u32* types, u32 nc, bool z10) {
    u32 off = 0, base = 0; int bitn = -1;
    for (u32 c = 0; c < nc; ++c) {
        u32 ty = types[c];
        t->ty[c] = u8(ty);
        if (ty == 9) {
            if (bitn < 0 || bitn >= 32) { off = (off + 3) / 4 * 4; base = off; off += 4; bitn = 0; }
            t->off[c] = base; t->bit[c] = u8(bitn++);
            continue;
        }
        bitn = -1;
        u32 al, sz; ExpaSize(ty, &al, &sz);
        if (ty == 10 && z10) { al = 1; sz = 0; }
        off = (off + al - 1) / al * al;
        t->off[c] = off; t->bit[c] = 0;
        off += sz;
    }
    if (!nc) return 0;
    u32 last = nc - 1, ty = types[last];
    if (ty == 9) return t->off[last] + 4;
    u32 al, sz; ExpaSize(ty, &al, &sz);
    if (ty == 10 && z10) sz = 0;
    return t->off[last] + sz;
}

void ExpaFree(Expa* e) { if (e) { free(e->b); free(e->strs); e->b = nullptr; e->strs = nullptr; } }

// Takes ownership of b.
bool ExpaOpen(Expa* e, u8* b, size_t n) {
    e->b = b; e->n = n; e->strs = nullptr; e->nstr = 0;
    if (n < 8 || !(b[0] == 'E' && b[1] == 'X' && b[2] == 'P' && b[3] == 'A')) return false;
    u32 nt = Le32(b + 4);
    size_t p = 8;
    for (u32 k = 0; k < nt; ++k) {
        if (p + 4 > n) return false;
        u32 nl = Le32(b + p); p += 4 + nl;
        if (p + 4 > n) return false;
        u32 sc = Le32(b + p); p += 4 + size_t(sc) * 4;
        if (p + 8 > n) return false;
        u32 esize = Le32(b + p), ec = Le32(b + p + 4); p += 8;
        p = (p + 7) / 8 * 8;
        p += (size_t(esize) + 7) / 8 * 8 * ec;
        if (p > n) return false;
    }
    size_t ci = p >= 16 ? p - 16 : 0;
    while (ci + 8 <= n && !(b[ci] == 'C' && b[ci + 1] == 'H' && b[ci + 2] == 'N' && b[ci + 3] == 'K')) ++ci;
    if (ci + 8 > n) return true;   // a table without strings
    u32 cnt = Le32(b + ci + 4);
    if (cnt > n / 8) return false;
    e->strs = (ExpaStr*)malloc((cnt ? cnt : 1) * sizeof(ExpaStr));
    if (!e->strs) return false;
    size_t q = ci + 8;
    bool sorted = true;
    for (u32 k = 0; k < cnt && q + 8 <= n; ++k) {
        u32 off = Le32(b + q), sz = Le32(b + q + 4); q += 8;
        if (q + sz > n) break;
        u32 len = 0;
        while (len < sz && b[q + len]) ++len;
        if (e->nstr && e->strs[e->nstr - 1].off >= off) sorted = false;
        e->strs[e->nstr++] = ExpaStr{off, u32(q), len};
        q += sz;
    }
    if (!sorted) {   // insertion sort; CHNK is written in offset order, so this is a fallback only
        for (size_t i = 1; i < e->nstr; ++i) {
            ExpaStr v = e->strs[i]; size_t j = i;
            while (j && e->strs[j - 1].off > v.off) { e->strs[j] = e->strs[j - 1]; --j; }
            e->strs[j] = v;
        }
    }
    return true;
}

bool ExpaTable(const Expa* e, const char* name, ExpaTab* t) {
    const u8* b = e->b; size_t n = e->n;
    if (!b || n < 8) return false;
    u32 nt = Le32(b + 4);
    size_t p = 8, nlen = StrLen(name);
    for (u32 k = 0; k < nt; ++k) {
        u32 nl = Le32(b + p); p += 4;
        const char* tn = (const char*)b + p;
        size_t tl = 0; while (tl < nl && tn[tl]) ++tl;
        p += nl;
        u32 sc = Le32(b + p); p += 4;
        const u8* tys = b + p; p += size_t(sc) * 4;
        u32 esize = Le32(b + p), ec = Le32(b + p + 4); p += 8;
        p = (p + 7) / 8 * 8;
        size_t stride = (size_t(esize) + 7) / 8 * 8;
        if (tl == nlen && memcmp(tn, name, nlen) == 0 && sc <= 160) {
            u32 types[160];
            for (u32 c = 0; c < sc; ++c) types[c] = Le32(tys + size_t(c) * 4);
            t->e = e; t->start = p; t->stride = stride; t->count = ec; t->ncol = sc; t->esize = esize;
            if (ExpaLayout(t, types, sc, false) > esize) ExpaLayout(t, types, sc, true);
            return true;
        }
        p += stride * ec;
    }
    return false;
}

int64_t TInt(const ExpaTab& t, u32 row, u32 col) {
    if (row >= t.count || col >= t.ncol) return -1;
    size_t a = t.start + size_t(row) * t.stride + t.off[col];
    const u8* b = t.e->b;
    if (a + 4 > t.e->n) return -1;
    switch (t.ty[col]) {
    case 9: return (Le32(b + a) >> t.bit[col]) & 1;
    case 3: return int16_t(u16(b[a] | b[a + 1] << 8));
    case 4: return b[a];
    case 5: case 6: case 7: case 8: return -1;
    default: return int32_t(Le32(b + a));
    }
}

// The string in a string cell, or nullptr.
const char* TStr(const ExpaTab& t, u32 row, u32 col, size_t* len) {
    if (row >= t.count || col >= t.ncol) return nullptr;
    u8 ty = t.ty[col];
    if (ty != 6 && ty != 7 && ty != 8) return nullptr;
    u32 a = u32(t.start + size_t(row) * t.stride + t.off[col]);
    size_t lo = 0, hi = t.e->nstr;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (t.e->strs[mid].off < a) lo = mid + 1; else hi = mid; }
    if (lo >= t.e->nstr || t.e->strs[lo].off != a) return nullptr;
    *len = t.e->strs[lo].len;
    return (const char*)t.e->b + t.e->strs[lo].at;
}

// ---------------------------------------------------------------- small sorted maps
struct KV { int64_t k; int64_t v; };
struct IMap {   // int -> int, sorted by key (first value wins for a repeated key)
    KV* a; size_t n, cap;
    void Add(int64_t k, int64_t v) {
        if (n == cap) { size_t c = cap ? cap * 2 : 64; KV* q = (KV*)Grow(a, cap * sizeof(KV), c * sizeof(KV)); if (!q) return; a = q; cap = c; }
        a[n++] = KV{k, v};
    }
    void Sort() {   // stable merge sort, so the first of equal keys stays first
        if (n < 2) return;
        KV* tmp = (KV*)malloc(n * sizeof(KV));
        if (!tmp) return;
        for (size_t w = 1; w < n; w *= 2) {
            for (size_t lo = 0; lo < n; lo += 2 * w) {
                size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
                size_t i = lo, j = mid, k = lo;
                while (i < mid && j < hi) tmp[k++] = a[j].k < a[i].k ? a[j++] : a[i++];
                while (i < mid) tmp[k++] = a[i++];
                while (j < hi) tmp[k++] = a[j++];
            }
            memcpy(a, tmp, n * sizeof(KV));
        }
        free(tmp);
    }
    const KV* Get(int64_t k) const {
        size_t lo = 0, hi = n;
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (a[mid].k < k) lo = mid + 1; else hi = mid; }
        return lo < n && a[lo].k == k ? &a[lo] : nullptr;
    }
    void Free() { free(a); a = nullptr; n = cap = 0; }
};

struct SMap {   // int key -> cleaned game string (owned)
    IMap idx; char** s; size_t n, cap;
    void Add(int64_t k, char* v) {
        if (n == cap) { size_t c = cap ? cap * 2 : 64; char** q = (char**)Grow(s, cap * sizeof(char*), c * sizeof(char*)); if (!q) { free(v); return; } s = q; cap = c; }
        s[n] = v; idx.Add(k, int64_t(n)); ++n;
    }
    const char* Get(int64_t k) const { const KV* e = idx.Get(k); return e ? s[e->v] : nullptr; }
    void Free() { for (size_t i = 0; i < n; ++i) free(s[i]); free(s); s = nullptr; n = cap = 0; idx.Free(); }
};

// ---------------------------------------------------------------- the data
const int kChestSlots = 12, kWildSlots = 25, kChestRows = 6, kQuestSlots = 32;

struct AreaDef { int64_t name_key; IntList maps; IntList fields; IntList chests; IntList wild; };
struct PageList { char* page; IntList ids; };

struct GameData {
    int status;                // number of game tables read; -1 none found
    // from the game
    IMap icon, buy, sell;      // item id -> icon / buy / sell price
    IMap item_skill;           // item id -> the battle skill it uses
    IMap skill_row; int32_t (*skill)[6]; size_t nskill;   // skill id -> kind, mode, amount, stat, target, status
    IMap shop;                 // item id -> 1 (sold somewhere)
    IMap s2g;                  // status id -> dex number
    SMap item_name, item_expl, field_name, skill_name, buff_name;
    // treasure rows, in table order: map id and item id
    int32_t* tr_map; int32_t* tr_item; size_t ntr;
    // side quests: id -> row of reward values (yen, AP, 3 x item id + count)
    IMap quest_row; int32_t (*quest)[8]; size_t nquest;
    // from the manifest (identifiers only)
    IntList items;             // item page index -> item id
    PageList* item_pages; size_t nitem_pages;
    AreaDef* areas; size_t nareas;
    IntList g2s;               // dex number -> status id
    IntList dex_name;          // dex number -> template index of its name (Text)
    PageList* quest_pages; size_t nquest_pages;
    // computed text
    char** item_eff;           // per item page index
    char** item_price;
    char** area_counts;        // per area: "N chests   M wild Digimon"
    char** area_nch; char** area_nwild;
    char name[96];
};

void FmtThousands(Buf* o, int64_t v) {
    char t[24]; size_t n = PutInt(t, v);
    size_t neg = t[0] == '-';
    for (size_t i = 0; i < n; ++i) {
        if (i > neg && (n - i) % 3 == 0) o->Ch(',');
        o->Ch(t[i]);
    }
}

char* BufTake(Buf* b) { if (!b->p) b->Ch(0); char* p = b->p; b->p = nullptr; b->n = b->cap = 0; return p; }

// "Buy 300 yen  /  Sell 60 yen" (== build_dsts_package.item_price)
char* PriceText(const GameData* d, int64_t id) {
    const KV* b = d->buy.Get(id); const KV* s = d->sell.Get(id);
    int64_t buy = b && b->v > 0 ? b->v : 0, sell = s && s->v > 0 ? s->v : 0;
    Buf o{};
    if (buy && sell) { o.Put("Buy ", 4); FmtThousands(&o, buy); o.Put(" yen  /  Sell ", 14); FmtThousands(&o, sell); o.Put(" yen", 4); }
    else if (buy) { o.Put("Buy ", 4); FmtThousands(&o, buy); o.Put(" yen  (can't be sold)", 21); }
    else if (sell) { o.Put("Sells for ", 10); FmtThousands(&o, sell); o.Put(" yen", 4); }
    else o.Put("No shop price", 13);
    return BufTake(&o);
}

// Our one-line description of a recovery item that has no description text of its own, from the
// numbers of the battle skill it uses (== check_gamedata.skill_effect).
char* SkillEffect(const GameData* d, int64_t id) {
    const KV* sk = d->item_skill.Get(id);
    const KV* row = sk ? d->skill_row.Get(sk->v) : nullptr;
    if (!row) return nullptr;
    const int32_t* q = d->skill[row->v];
    int32_t kind = q[0], mode = q[1], amount = q[2], stat = q[3], target = q[4], status = q[5];
    const char* who = target == 6 ? "all allies" : "one ally";
    const char* what = stat == 1 ? "HP" : stat == 2 ? "SP" : stat == 3 ? "HP and SP" : nullptr;
    Buf o{};
    if ((kind == 31011 || kind == 31013) && what && amount > 0 && (mode == 9 || mode == 10)) {
        o.Put("Restores ", 9); FmtThousands(&o, amount);
        if (mode == 10) o.Put("% of ", 5); else o.Ch(' ');
        o.Put(what, StrLen(what)); o.Put(" to ", 4); o.Put(who, StrLen(who)); o.Ch('.');
    } else if (kind == 31031 && amount > 0) {
        o.Put(target == 6 ? "Revives KO'd allies with " : "Revives one KO'd ally with ", target == 6 ? 25 : 27);
        FmtThousands(&o, amount); o.Put("% HP.", 5);
    } else if ((kind == 31041 || kind == 31042) && status >= 1000) {
        int64_t b = status / 1000 - 100;
        const char* a = d->buff_name.Get(b);
        const char* c = kind == 31041 ? d->buff_name.Get(b + 5) : nullptr;
        if (!a) return nullptr;
        o.Put("Cures ", 6); o.Put(a, StrLen(a));
        if (c) { o.Put(" and ", 5); o.Put(c, StrLen(c)); }
        o.Put(" on ", 4); o.Put(who, StrLen(who)); o.Ch('.');
    } else if (kind == 31051) {
        o.Put("Removes stat drops from ", 24); o.Put(who, StrLen(who)); o.Ch('.');
    } else if (kind == 31061) {
        o.Put("Removes status problems, Injury and Disease from ", 49); o.Put(who, StrLen(who)); o.Ch('.');
    } else {
        return nullptr;
    }
    return BufTake(&o);
}

// The game's own description, or our short note where the game has none.
char* EffectText(const GameData* d, int64_t id) {
    const char* e = d->item_expl.Get(id);
    if (e && *e) return Dup(e, StrLen(e));
    if (char* sk = SkillEffect(d, id)) return sk;
    static const char* const kStat[][2] = {{"HP", "max HP"}, {"SP", "max SP"}, {"ATK", "ATK"}, {"DEF", "DEF"},
        {"INT", "INT"}, {"SPI", "SPI"}, {"SPD", "SPD"}, {"ACU", "accuracy"}, {"EVA", "evasion"}, {"CRT", "critical rate"}};
    const char* nm = d->item_name.Get(id);
    if (nm) {   // "<STAT> Attachment I", "... II", "... III"
        size_t a = 0; while (nm[a] >= 'A' && nm[a] <= 'Z') ++a;
        const char* rest = nm + a;
        if (a && StartsWith(rest, " Attachment ") && rest[12] == 'I') {
            const char* r = rest + 12; while (*r == 'I') ++r;
            if (!*r) {
                for (auto& st : kStat) {
                    if (StrLen(st[0]) == a && memcmp(st[0], nm, a) == 0) {
                        Buf o{}; o.Put("Equipment that raises ", 22); o.Put(st[1], StrLen(st[1]));
                        o.Put(". Higher numbers are stronger.", 30);
                        return BufTake(&o);
                    }
                }
            }
        }
    }
    const char kNone[] = "No description in the game's own text.";
    return Dup(kNone, sizeof(kNone) - 1);
}

bool HasInt(const IntList& l, int64_t v) { for (size_t i = 0; i < l.n; ++i) if (l.v[i] == v) return true; return false; }
void ListAdd(IntList* l, int32_t v) {
    int32_t* q = (int32_t*)Grow(l->v, l->n * sizeof(int32_t), (l->n + 1) * sizeof(int32_t));
    if (!q) return;
    l->v = q; l->v[l->n++] = v;
}

// ---------------------------------------------------------------- reading the archives
// One wanted .mbe member, matched by its bare name (after the last '\' or '/').
struct Want { const char* name; bool text; Expa e; bool got; };

void LoadMembers(const EdenDsmodHostApi* host, const char* path, Want* w, size_t nw) {
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
    for (u32 i = 1; ok && i < fc && i < nc; ++i) {
        const char* ext = (const char*)names + size_t(i) * 0x80;
        if (!(ext[0] == 'm' && ext[1] == 'b' && ext[2] == 'e')) continue;
        const char* nm = ext + 4;
        const char* base = nm;
        for (const char* q = nm; *q && q < ext + 0x80; ++q) if (*q == '\\' || *q == '/') base = q + 1;
        bool in_text = StartsWith(nm, "text\\") || StartsWith(nm, "text/");
        Want* hit = nullptr;
        for (size_t k = 0; k < nw; ++k)
            if (!w[k].got && w[k].text == in_text && Eq(base, w[k].name)) { hit = &w[k]; break; }
        if (!hit) continue;
        u32 did = Le32(tree + size_t(i) * 16 + 4);
        if (did >= dc) continue;
        const u8* de = data + size_t(did) * 16;
        u64 off = Le64(de); u32 full = Le32(de + 8), comp = Le32(de + 12);
        if (full == 0 || full > (64u << 20) || comp > (64u << 20)) continue;
        u8* raw = (u8*)malloc(comp ? comp : 1);
        if (!raw) continue;
        u8* file = nullptr;
        if (ReadAt(host, path, data_start + off, raw, comp) == comp) {
            if (comp == full) { file = raw; raw = nullptr; }
            else {
                file = (u8*)malloc(full);
                if (file && !Lz4(raw, comp, file, full)) { free(file); file = nullptr; }
            }
        }
        free(raw);
        if (file && ExpaOpen(&hit->e, file, full)) hit->got = true;
        else ExpaFree(&hit->e);
    }
    free(tree); free(names); free(data);
}

// Every (key, last string column) row of a text table, cleaned, into m.
void LoadKeyed(const Expa& e, SMap* m) {
    ExpaTab t;
    if (!ExpaTable(&e, "Sheet1", &t)) return;
    u32 last = t.ncol;
    for (u32 c = t.ncol; c-- > 0;) if (t.ty[c] == 6 || t.ty[c] == 7 || t.ty[c] == 8) { last = c; break; }
    if (last >= t.ncol) return;
    Buf clean{}, tmp{};
    for (u32 r = 0; r < t.count; ++r) {
        size_t kl = 0, vl = 0;
        const char* k = TStr(t, r, 0, &kl);
        const char* v = TStr(t, r, last, &vl);
        if (!k || !v || !kl) continue;
        int64_t key = 0; bool neg = false, num = true; size_t i = 0;
        if (k[0] == '-') { neg = true; i = 1; }
        if (i >= kl) num = false;
        for (; i < kl; ++i) { if (k[i] < '0' || k[i] > '9') { num = false; break; } key = key * 10 + (k[i] - '0'); }
        if (!num) continue;
        Clean(v, vl, &clean, &tmp);
        if (!clean.n) continue;
        m->Add(neg ? -key : key, Dup(clean.p, clean.n));
    }
    m->idx.Sort();
    free(clean.p); free(tmp.p);
}

// "t0101" for field key 10101, "d0102" for 20102, "h0201" for 30201.
bool FieldCode(int64_t key, char* out) {
    static const char kPfx[] = {0, 't', 'd', 'h', 'e'};
    int64_t p = key / 10000, num = key % 10000;
    if (p < 1 || p > 4) return false;
    out[0] = kPfx[p];
    out[1] = char('0' + num / 1000 % 10); out[2] = char('0' + num / 100 % 10);
    out[3] = char('0' + num / 10 % 10); out[4] = char('0' + num % 10); out[5] = 0;
    return true;
}

bool ListFrom(const JVal* a, IntList* l) { l->v = nullptr; l->n = 0; return ToIntList(a, l); }

PageList* PagesFrom(const JVal* o, size_t* n) {
    *n = 0;
    if (!o || o->kind != JVal::Obj) return nullptr;
    PageList* p = (PageList*)calloc(o->count + 1, sizeof(PageList));
    for (size_t i = 0; p && i < o->count; ++i) {
        if (!ToIntList(o->items[i], &p[*n].ids)) continue;
        p[*n].page = Dup(o->keys[i], StrLen(o->keys[i]));
        ++*n;
    }
    return p;
}

void SortInts(IntList* l) {
    for (size_t i = 1; i < l->n; ++i) { int32_t v = l->v[i]; size_t j = i; while (j && l->v[j - 1] > v) { l->v[j] = l->v[j - 1]; --j; } l->v[j] = v; }
}

GameData* LoadGameData(const EdenDsmodHostApi* host, const char* cfg) {
    if (!cfg) return nullptr;
    const char* key = "\"dsts_data\"";
    const char* at = nullptr;
    for (const char* q = cfg; *q; ++q) if (StartsWith(q, key)) { at = q + StrLen(key); break; }
    if (!at) return nullptr;
    while (*at && *at != ':') ++at;
    if (!*at) return nullptr;
    JParse jp{at + 1, at + 1 + StrLen(at + 1), false};
    JVal* root = jp.Value(0);
    if (jp.bad || !root || root->kind != JVal::Obj) { JFree(root); return nullptr; }
    GameData* d = (GameData*)calloc(1, sizeof(GameData));
    if (!d) { JFree(root); return nullptr; }
    ListFrom(JGet(root, "items"), &d->items);
    ListFrom(JGet(root, "g2s"), &d->g2s);
    ListFrom(JGet(root, "dexname"), &d->dex_name);
    d->item_pages = PagesFrom(JGet(root, "itempages"), &d->nitem_pages);
    d->quest_pages = PagesFrom(JGet(root, "quests"), &d->nquest_pages);
    const JVal* areas = JGet(root, "areas");
    if (areas && areas->kind == JVal::Arr) {
        d->areas = (AreaDef*)calloc(areas->count + 1, sizeof(AreaDef));
        for (size_t i = 0; d->areas && i < areas->count; ++i) {
            const JVal* a = areas->items[i];
            AreaDef& ad = d->areas[d->nareas];
            if (!a || a->kind != JVal::Arr || a->count != 3 || !a->items[0] || a->items[0]->kind != JVal::Num) continue;
            ad.name_key = int64_t(a->items[0]->num);
            ListFrom(a->items[1], &ad.maps);
            ListFrom(a->items[2], &ad.fields);
            ++d->nareas;
        }
    }
    JFree(root);

    // The tables. Wild Digimon come from one field_battle_<map> table per listed field.
    size_t nf = 0;
    for (size_t i = 0; i < d->nareas; ++i) nf += d->areas[i].fields.n;
    const size_t kFixed = 11;
    size_t nw = kFixed + nf;
    Want* w = (Want*)calloc(nw, sizeof(Want));
    char (*fnames)[24] = (char(*)[24])calloc(nf + 1, 24);
    if (!w || !fnames) { free(w); free(fnames); d->status = -1; return d; }
    const char* fixed[] = {"item", "field_treasure", "shop", "quest_sub", "battle_enemy", "battle_skill"};
    for (int k = 0; k < 6; ++k) w[k].name = fixed[k];
    const char* texts[] = {"item_name", "item_explanation", "field_name", "skill_name", "buff_name"};
    for (int k = 0; k < 5; ++k) { w[6 + k].name = texts[k]; w[6 + k].text = true; }
    size_t fi = 0;
    for (size_t i = 0; i < d->nareas; ++i)
        for (size_t k = 0; k < d->areas[i].fields.n; ++k, ++fi) {
            char code[8];
            if (!FieldCode(d->areas[i].fields.v[k], code)) { fnames[fi][0] = 0; }
            else { memcpy(fnames[fi], "field_battle_", 13); memcpy(fnames[fi] + 13, code, 6); }
            w[kFixed + fi].name = fnames[fi];
        }
    static const char* const kArch[] = {"app_0.nx64.mvgl", "app_1.nx64.mvgl", "app_2.nx64.mvgl", "app_3.nx64.mvgl",
        "app_text01.nx64.mvgl", "app_text00.nx64.mvgl", "app_text.nx64.mvgl", "app_4.nx64.mvgl", "app_5.nx64.mvgl"};
    static const char* const kPre[] = {"", "/", "romfs:/"};
    for (const char* nm : kArch) {
        bool all = true;
        for (size_t k = 0; k < nw; ++k) if (!w[k].got && w[k].name[0]) { all = false; break; }
        if (all) break;
        for (const char* pre : kPre) {
            char path[64]; size_t a = StrLen(pre), b = StrLen(nm);
            memcpy(path, pre, a); memcpy(path + a, nm, b + 1);
            if (ReadAt(host, path, 0, nullptr, 0) > 0x20) { LoadMembers(host, path, w, nw); break; }
        }
    }
    int got = 0;
    for (size_t k = 0; k < kFixed; ++k) got += w[k].got;
    d->status = got ? got : -1;

    ExpaTab t;
    if (w[0].got && ExpaTable(&w[0].e, "item_parameter", &t)) {
        for (u32 r = 0; r < t.count; ++r) {
            int64_t id = TInt(t, r, 0);
            d->icon.Add(id, TInt(t, r, 7)); d->buy.Add(id, TInt(t, r, 9)); d->sell.Add(id, TInt(t, r, 10));
            d->item_skill.Add(id, TInt(t, r, 13));
        }
        d->icon.Sort(); d->buy.Sort(); d->sell.Sort(); d->item_skill.Sort();
    }
    if (w[5].got && ExpaTable(&w[5].e, "battle_skill_list", &t)) {
        d->skill = (int32_t(*)[6])malloc((t.count + 1) * sizeof(int32_t[6]));
        for (u32 r = 0; d->skill && r < t.count; ++r) {
            static const u32 kCols[6] = {15, 22, 23, 25, 33, 57};
            for (int c = 0; c < 6; ++c) d->skill[d->nskill][c] = int32_t(TInt(t, r, kCols[c]));
            d->skill_row.Add(TInt(t, r, 0), int64_t(d->nskill));
            ++d->nskill;
        }
        d->skill_row.Sort();
    }
    if (w[1].got && ExpaTable(&w[1].e, "field_treasure", &t)) {
        d->tr_map = (int32_t*)malloc((t.count + 1) * sizeof(int32_t));
        d->tr_item = (int32_t*)malloc((t.count + 1) * sizeof(int32_t));
        for (u32 r = 0; d->tr_map && d->tr_item && r < t.count; ++r) {
            d->tr_map[d->ntr] = int32_t(TInt(t, r, 0) / 100);
            d->tr_item[d->ntr] = int32_t(TInt(t, r, 4));
            ++d->ntr;
        }
    }
    if (w[2].got && ExpaTable(&w[2].e, "shop_lineup", &t)) {
        for (u32 r = 0; r < t.count; ++r) d->shop.Add(TInt(t, r, 2), 1);
        d->shop.Sort();
    }
    if (w[3].got && ExpaTable(&w[3].e, "sub_quest_list", &t)) {
        d->quest = (int32_t(*)[8])malloc((t.count + 1) * sizeof(int32_t[8]));
        for (u32 r = 0; d->quest && r < t.count; ++r) {
            static const u32 kCols[8] = {8, 9, 10, 11, 13, 14, 16, 17};
            for (int c = 0; c < 8; ++c) d->quest[d->nquest][c] = int32_t(TInt(t, r, kCols[c]));
            d->quest_row.Add(TInt(t, r, 0), int64_t(d->nquest));
            ++d->nquest;
        }
        d->quest_row.Sort();
    }
    for (int k = 0; k < 5; ++k) {
        if (!w[6 + k].got) continue;
        SMap* m[] = {&d->item_name, &d->item_expl, &d->field_name, &d->skill_name, &d->buff_name};
        LoadKeyed(w[6 + k].e, m[k]);
    }
    // status -> dex number (lowest dex number first)
    for (size_t g = 0; g < d->g2s.n; ++g) if (d->g2s.v[g] >= 0) d->s2g.Add(d->g2s.v[g], int64_t(g));
    d->s2g.Sort();

    // Areas: chests (item ids, table order) and wild Digimon (dex numbers, ascending).
    IMap enemy{}, group_at{};
    ExpaTab eg{}; bool have_eg = false;
    if (w[4].got) {
        if (ExpaTable(&w[4].e, "enemy_parameter", &t)) {
            for (u32 r = 0; r < t.count; ++r) enemy.Add(TInt(t, r, 0), TInt(t, r, 1));
            enemy.Sort();
        }
        if (ExpaTable(&w[4].e, "encount_group", &eg)) {
            have_eg = true;
            for (u32 r = 0; r < eg.count; ++r) group_at.Add(TInt(eg, r, 0), int64_t(r));
            group_at.Sort();
        }
    }
    fi = 0;
    for (size_t i = 0; i < d->nareas; ++i) {
        AreaDef& ad = d->areas[i];
        for (size_t k = 0; k < ad.maps.n; ++k)
            for (size_t r = 0; r < d->ntr; ++r)
                if (d->tr_map[r] == ad.maps.v[k] && d->tr_item[r] > 0) ListAdd(&ad.chests, d->tr_item[r]);
        for (size_t k = 0; k < ad.fields.n; ++k, ++fi) {
            Want& fw = w[kFixed + fi];
            if (!fw.got || !have_eg || !ExpaTable(&fw.e, "field_symbol_enemy", &t)) continue;
            for (u32 r = 0; r < t.count; ++r) {
                const KV* gr = group_at.Get(TInt(t, r, 1));
                if (!gr) continue;
                static const u32 kSlot[6] = {1, 14, 26, 38, 50, 62};
                for (u32 s : kSlot) {
                    int64_t e = TInt(eg, u32(gr->v), s);
                    if (e <= 0) continue;
                    const KV* st = enemy.Get(e);
                    int64_t status = st ? st->v : e / 100;
                    const KV* g = d->s2g.Get(status);
                    if (!g || g->v == 1 || HasInt(ad.wild, g->v)) continue;
                    ListAdd(&ad.wild, int32_t(g->v));
                }
            }
        }
        SortInts(&ad.wild);
    }
    enemy.Free(); group_at.Free();
    for (size_t k = 0; k < nw; ++k) ExpaFree(&w[k].e);
    free(w); free(fnames);

    // Fixed texts, worked out once.
    size_t ni = d->items.n;
    d->item_eff = (char**)calloc(ni + 1, sizeof(char*));
    d->item_price = (char**)calloc(ni + 1, sizeof(char*));
    for (size_t g = 0; d->item_eff && d->item_price && g < ni; ++g) {
        d->item_eff[g] = EffectText(d, d->items.v[g]);
        d->item_price[g] = PriceText(d, d->items.v[g]);
    }
    d->area_counts = (char**)calloc(d->nareas + 1, sizeof(char*));
    d->area_nch = (char**)calloc(d->nareas + 1, sizeof(char*));
    d->area_nwild = (char**)calloc(d->nareas + 1, sizeof(char*));
    for (size_t i = 0; d->area_counts && d->area_nch && d->area_nwild && i < d->nareas; ++i) {
        const AreaDef& ad = d->areas[i];
        Buf o{}; FmtThousands(&o, int64_t(ad.chests.n)); o.Put(" chests   ", 10); FmtThousands(&o, int64_t(ad.wild.n));
        o.Put(" wild Digimon", 13);
        d->area_counts[i] = BufTake(&o);
        Buf c{}; FmtThousands(&c, int64_t(ad.chests.n)); c.Put(" chests", 7); d->area_nch[i] = BufTake(&c);
        Buf m{}; FmtThousands(&m, int64_t(ad.wild.n)); m.Put(" Digimon", 8); d->area_nwild[i] = BufTake(&m);
    }
    return d;
}

void FreeGameData(GameData* d) {
    if (!d) return;
    d->icon.Free(); d->buy.Free(); d->sell.Free(); d->shop.Free(); d->s2g.Free(); d->quest_row.Free();
    d->item_skill.Free(); d->skill_row.Free();
    d->item_name.Free(); d->item_expl.Free(); d->field_name.Free(); d->skill_name.Free(); d->buff_name.Free();
    free(d->tr_map); free(d->tr_item); free(d->quest); free(d->skill);
    for (size_t i = 0; i < d->nitem_pages; ++i) { free(d->item_pages[i].page); free(d->item_pages[i].ids.v); }
    for (size_t i = 0; i < d->nquest_pages; ++i) { free(d->quest_pages[i].page); free(d->quest_pages[i].ids.v); }
    for (size_t i = 0; i < d->nareas; ++i) {
        AreaDef& a = d->areas[i];
        free(a.maps.v); free(a.fields.v); free(a.chests.v); free(a.wild.v);
        if (d->area_counts) free(d->area_counts[i]);
        if (d->area_nch) free(d->area_nch[i]);
        if (d->area_nwild) free(d->area_nwild[i]);
    }
    for (size_t g = 0; g < d->items.n; ++g) { if (d->item_eff) free(d->item_eff[g]); if (d->item_price) free(d->item_price[g]); }
    free(d->item_eff); free(d->item_price); free(d->area_counts); free(d->area_nch); free(d->area_nwild);
    free(d->item_pages); free(d->quest_pages); free(d->areas);
    free(d->items.v); free(d->g2s.v); free(d->dex_name.v);
    free(d);
}

// ---------------------------------------------------------------- publishing
void PubText(GameData* d, const EdenDsmodHostApi* host, const char* a, int64_t i, const char* b, const char* v) {
    if (!v || !host->publish_text) return;
    size_t la = StrLen(a);
    memcpy(d->name, a, la);
    size_t n = la;
    if (i >= 0) n += PutInt(d->name + n, i);
    size_t lb = b ? StrLen(b) : 0;
    if (lb) memcpy(d->name + n, b, lb);
    d->name[n + lb] = 0;
    host->publish_text(host->userdata, d->name, v);
}
void PubInt(GameData* d, const EdenDsmodHostApi* host, const char* a, int64_t i, const char* b, int64_t v) {
    size_t la = StrLen(a);
    memcpy(d->name, a, la);
    size_t n = la;
    if (i >= 0) n += PutInt(d->name + n, i);
    size_t lb = b ? StrLen(b) : 0;
    if (lb) memcpy(d->name + n, b, lb);
    d->name[n + lb] = 0;
    host->publish_i64(host->userdata, d->name, v);
}

// Area names an item's chests are in, in table order, each once (== check_gamedata.item_chests).
size_t ItemChests(const GameData* d, int64_t id, const char** out, size_t cap) {
    size_t n = 0;
    for (size_t r = 0; r < d->ntr; ++r) {
        if (d->tr_item[r] != id) continue;
        int64_t key = int64_t(d->tr_map[r]) + 10000;
        if (key >= 30000) continue;   // the Outer Dungeon's challenge rooms
        const char* nm = d->field_name.Get(key);
        if (!nm) continue;
        bool dup = false;
        for (size_t k = 0; k < n && k < cap; ++k) if (Eq(out[k], nm)) { dup = true; break; }
        if (dup) continue;
        if (n < cap) out[n] = nm;
        ++n;
    }
    return n;
}

// "Shinjuku, Gear Forest: ..." (== build_dsts_package.found_text, limit 240)
char* FoundText(const GameData* d, int64_t num) {
    const char* names[256]; size_t n = 0;
    for (size_t i = 0; i < d->nareas && n < 256; ++i)
        if (HasInt(d->areas[i].wild, num)) {
            const char* nm = d->field_name.Get(d->areas[i].name_key);
            if (nm) names[n++] = nm;
        }
    if (!n) return nullptr;
    Buf o{};
    for (size_t k = 0; k < n; ++k) {
        size_t cand = o.n + (o.n || k ? 2 : 0) + StrLen(names[k]);
        if (k == 0) cand = StrLen(names[k]);
        if (cand > 240 - 16) {
            o.Put(", and ", 6); FmtThousands(&o, int64_t(n - k)); o.Put(" more (see Area guide)", 22);
            return BufTake(&o);
        }
        if (k) o.Put(", ", 2);
        o.Put(names[k], StrLen(names[k]));
    }
    return BufTake(&o);
}

// "1,000 yen, 40 AP, ATK Attachment I, Heavy Strike I, Friendship S x3"
char* RewardText(const GameData* d, int64_t qid) {
    const KV* r = d->quest_row.Get(qid);
    if (!r) return nullptr;
    const int32_t* q = d->quest[r->v];
    Buf o{};
    auto sep = [&]() { if (o.n) o.Put(", ", 2); };
    if (q[0] > 0) { sep(); FmtThousands(&o, q[0]); o.Put(" yen", 4); }
    if (q[1] > 0) { sep(); FmtThousands(&o, q[1]); o.Put(" AP", 3); }
    for (int k = 0; k < 3; ++k) {
        int32_t id = q[2 + 2 * k], cnt = q[3 + 2 * k];
        if (id <= 0) continue;
        const char* nm = d->item_name.Get(id);
        if (!nm) nm = d->skill_name.Get(id);
        if (!nm) continue;
        sep(); o.Put(nm, StrLen(nm));
        if (cnt > 1) { o.Put(" x", 2); FmtThousands(&o, cnt); }
    }
    if (!o.n) return nullptr;
    return BufTake(&o);
}

const char* PageIs(const char* page, const char* name) { return Eq(page, name) ? page : nullptr; }

void PublishGameData(GameData* d, const Text* text, const EdenDsmodHostApi* host) {
    host->publish_i64(host->userdata, "dsts.data", d->status);
    if (d->status <= 0) return;
    for (size_t i = 0; i < d->nareas; ++i) PubText(d, host, "dsts.arc", int64_t(i), nullptr, d->area_counts[i]);
    if (!host->get_text) return;
    const char* page = host->get_text(host->userdata, "__page");
    if (!page) return;
    auto flag = [&](const char* f) { return host->get_i64 ? host->get_i64(host->userdata, f, -1) : int64_t(-1); };

    if (PageIs(page, "areas"))
        for (size_t i = 0; i < d->nareas; ++i) {
            PubText(d, host, "dsts.arch", int64_t(i), nullptr, d->area_nch[i]);
            PubText(d, host, "dsts.arw", int64_t(i), nullptr, d->area_nwild[i]);
        }

    int64_t a = -1;   // an area page: "ar<index>"
    if (page[0] == 'a' && page[1] == 'r' && page[2] >= '0' && page[2] <= '9') {
        a = 0;
        for (const char* q = page + 2; *q; ++q) { if (*q < '0' || *q > '9') { a = -1; break; } a = a * 10 + (*q - '0'); }
    }
    {
        if (a >= 0 && size_t(a) < d->nareas) {
            const AreaDef& ad = d->areas[a];
            size_t nc = ad.chests.n < size_t(kChestSlots) ? ad.chests.n : size_t(kChestSlots);
            size_t nw = ad.wild.n < size_t(kWildSlots) ? ad.wild.n : size_t(kWildSlots);
            PubInt(d, host, "dsts.ac.n", -1, nullptr, int64_t(ad.chests.n));
            PubInt(d, host, "dsts.aw.n", -1, nullptr, int64_t(ad.wild.n));
            Buf c{}; c.Put("CHESTS (", 8); FmtThousands(&c, int64_t(ad.chests.n)); c.Ch(')');
            PubText(d, host, "dsts.ac.tab", -1, nullptr, c.p); free(c.p);
            Buf m{}; m.Put("DIGIMON (", 9); FmtThousands(&m, int64_t(ad.wild.n)); m.Ch(')');
            PubText(d, host, "dsts.aw.tab", -1, nullptr, m.p); free(m.p);
            for (size_t k = 0; k < nc; ++k) {
                int64_t id = ad.chests.v[k];
                PubInt(d, host, "dsts.ac", int64_t(k), nullptr, 1);
                PubInt(d, host, "dsts.aci", int64_t(k), nullptr, id);
                const char* nm = d->item_name.Get(id);
                PubText(d, host, "dsts.acn", int64_t(k), nullptr, nm ? nm : d->skill_name.Get(id));
                for (size_t g = 0; g < d->items.n; ++g)
                    if (d->items.v[g] == id) { PubText(d, host, "dsts.ace", int64_t(k), nullptr, d->item_eff[g]); break; }
            }
            for (size_t k = 0; k < nw; ++k) {
                int64_t num = ad.wild.v[k];
                PubInt(d, host, "dsts.aw", int64_t(k), nullptr, 1);
                int64_t st = size_t(num) < d->g2s.n ? d->g2s.v[num] : -1;
                PubInt(d, host, "dsts.aws", int64_t(k), nullptr, st >= 0 ? 1000 + st : 0);
                PubInt(d, host, "dsts.awp", int64_t(k), nullptr, (100 + a) * 1000 + num);
                int64_t tpl = size_t(num) < d->dex_name.n ? d->dex_name.v[num] : -1;
                if (text && tpl >= 0 && size_t(tpl) < text->ntpl && text->shown)
                    PubText(d, host, "dsts.awn", int64_t(k), nullptr, text->shown[tpl]);
            }
        }
    }

    if (PageIs(page, "it")) {
        int64_t g = flag("@flag:its");
        if (g >= 0 && size_t(g) < d->items.n) {
            int64_t id = d->items.v[g];
            PubInt(d, host, "dsts.it.id", -1, nullptr, id);
            PubText(d, host, "dsts.it.price", -1, nullptr, d->item_price[g]);
            PubText(d, host, "dsts.it.eff", -1, nullptr, d->item_eff[g]);
            bool shop = d->shop.Get(id) != nullptr;
            const char* ch[kChestRows];
            size_t n = ItemChests(d, id, ch, kChestRows);
            if (shop) PubText(d, host, "dsts.it.shop", -1, nullptr, "Sold in shops");
            if (n) {
                Buf o{}; o.Put("In chests (", 11); FmtThousands(&o, int64_t(n)); o.Put(n == 1 ? " area):" : " areas):", n == 1 ? 7 : 8);
                PubText(d, host, "dsts.it.chn", -1, nullptr, o.p); free(o.p);
            }
            for (size_t r = 0; r < n && r < size_t(kChestRows); ++r) {
                PubInt(d, host, "dsts.it.r", int64_t(r), nullptr, 1);
                PubText(d, host, "dsts.it.ch", int64_t(r), nullptr, ch[r]);
            }
            if (n > size_t(kChestRows)) {
                Buf o{}; o.Put("and ", 4); FmtThousands(&o, int64_t(n - kChestRows)); o.Put(" more", 5);
                PubText(d, host, "dsts.it.more", -1, nullptr, o.p); free(o.p);
            }
            if (!shop && !n)
                PubText(d, host, "dsts.it.none", -1, nullptr,
                        "Not sold in shops or found in chests. It comes from quests, battles or crafting.");
        }
    }

    for (size_t i = 0; i < d->nitem_pages; ++i) {
        if (!Eq(d->item_pages[i].page, page)) continue;
        const IntList& l = d->item_pages[i].ids;
        for (size_t r = 0; r < l.n; ++r)
            if (l.v[r] >= 0 && size_t(l.v[r]) < d->items.n) PubText(d, host, "dsts.ie", int64_t(r), nullptr, d->item_eff[l.v[r]]);
        break;
    }

    if (PageIs(page, "dx")) {
        int64_t v = flag("@flag:dxh");
        if (v < 0) v = -v;
        v %= 1000;
        if (v > 0) {
            char* f = FoundText(d, v);
            if (f) { PubText(d, host, "dsts.dx.wild", -1, nullptr, f); free(f); }
            else PubText(d, host, "dsts.dx.nowild", -1, nullptr, "Not found wild in the game data");
        }
    }

    for (size_t i = 0; i < d->nquest_pages; ++i) {
        if (!Eq(d->quest_pages[i].page, page)) continue;
        const IntList& l = d->quest_pages[i].ids;
        for (size_t s = 0; s < l.n && s < size_t(kQuestSlots); ++s) {
            char* r = l.v[s] > 0 ? RewardText(d, l.v[s]) : nullptr;
            PubInt(d, host, "dsts.rwok", int64_t(s), nullptr, r ? 1 : 0);
            if (r) { PubText(d, host, "dsts.rw", int64_t(s), nullptr, r); free(r); }
        }
        break;
    }
}

// Item icon picture for an item id ("module:dsts:itemicon_<id>" -> ui_icon_item_<icon>).
bool ItemIconKey(const GameData* d, int64_t id, char* out, size_t cap) {
    if (!d) return false;
    const KV* ic = d->icon.Get(id);
    if (!ic || ic->v < 0 || cap < 40) return false;
    memcpy(out, "module:dsts:ui_icon_item_", 25);
    char num[24]; size_t n = PutInt(num, ic->v);
    size_t pad = n < 3 ? 3 - n : 0;
    for (size_t k = 0; k < pad; ++k) out[25 + k] = '0';
    memcpy(out + 25 + pad, num, n + 1);
    return true;
}
