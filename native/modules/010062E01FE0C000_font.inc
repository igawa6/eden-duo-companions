// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// 010062E01FE0C000_font.inc -- the game's own menu font (FOT-Rodin, an OpenType/CFF file at "font\commonfont" inside
// app_text01.nx64.mvgl), drawn into a glyph atlas for Eden's text. Nothing of the font ships: the module
// reads the player's own copy. Included by 010062E01FE0C000.cpp (freestanding: no STL, malloc/free only).
//
// Coverage: U+0020..U+00FF, one dense run (Eden's font sink takes one run). Eden draws a glyph run so
// that `line_height` atlas pixels equal the requested cap height, so line_height = the capital H height.

namespace font {

u16 Be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
u32 Be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }

struct Span { const u8* p; size_t n; };

// ---------------------------------------------------------------- CFF INDEX
struct Index {
    u32 count; u8 osz; const u8* offs; const u8* data; const u8* end;
    bool Ok() const { return data != nullptr; }
    u32 Off(u32 i) const { u32 v = 0; for (u8 k = 0; k < osz; ++k) v = v << 8 | offs[size_t(i) * osz + k]; return v; }
    Span Get(u32 i) const {
        if (i >= count) return {nullptr, 0};
        u32 a = Off(i), b = Off(i + 1);
        if (b < a || data + b - 1 > end) return {nullptr, 0};
        return {data + a - 1, size_t(b - a)};
    }
};

// Reads an INDEX at p (bounded by end). Returns the byte after it, or nullptr.
const u8* ReadIndex(const u8* p, const u8* end, Index* ix) {
    *ix = Index{0, 0, nullptr, nullptr, end};
    if (p + 2 > end) return nullptr;
    ix->count = Be16(p);
    if (ix->count == 0) { ix->data = p + 2; return p + 2; }
    if (p + 3 > end) return nullptr;
    ix->osz = p[2];
    if (ix->osz < 1 || ix->osz > 4) return nullptr;
    ix->offs = p + 3;
    if (ix->offs + size_t(ix->count + 1) * ix->osz > end) return nullptr;
    ix->data = ix->offs + size_t(ix->count + 1) * ix->osz;
    const u8* after = ix->data + ix->Off(ix->count) - 1;
    if (after > end) return nullptr;
    return after;
}

// ---------------------------------------------------------------- CFF DICT
struct Dict { double v[48]; int n; };
// Calls fn(op, operands) for each operator.
template <class F> void WalkDict(Span d, F fn) {
    Dict st{}; const u8* p = d.p; const u8* e = d.p + d.n;
    while (p < e) {
        u8 b = *p;
        if (b <= 21) {
            int op = b; ++p;
            if (b == 12) { if (p >= e) return; op = 1200 + *p++; }
            fn(op, st);
            st.n = 0;
        } else if (b == 28) { if (p + 3 > e) return; if (st.n < 48) st.v[st.n++] = int16_t(Be16(p + 1)); p += 3; }
        else if (b == 29) { if (p + 5 > e) return; if (st.n < 48) st.v[st.n++] = int32_t(Be32(p + 1)); p += 5; }
        else if (b == 30) {   // real number: nibbles; good enough for what we read (FontMatrix ignored)
            ++p; double val = 0; bool neg = false, frac = false; double scale = 0.1; bool done = false;
            while (p < e && !done) {
                u8 by = *p++;
                for (int h = 0; h < 2 && !done; ++h) {
                    u8 nb = h ? (by & 15) : (by >> 4);
                    if (nb <= 9) { if (frac) { val += nb * scale; scale *= 0.1; } else val = val * 10 + nb; }
                    else if (nb == 0xa) frac = true;
                    else if (nb == 0xe) neg = true;
                    else if (nb == 0xf) done = true;
                }
            }
            if (st.n < 48) st.v[st.n++] = neg ? -val : val;
        } else if (b >= 32 && b <= 246) { if (st.n < 48) st.v[st.n++] = int(b) - 139; ++p; }
        else if (b >= 247 && b <= 250) { if (p + 2 > e) return; if (st.n < 48) st.v[st.n++] = (int(b) - 247) * 256 + p[1] + 108; p += 2; }
        else if (b >= 251 && b <= 254) { if (p + 2 > e) return; if (st.n < 48) st.v[st.n++] = -(int(b) - 251) * 256 - p[1] - 108; p += 2; }
        else ++p;
    }
}

// ---------------------------------------------------------------- outlines
struct Edge { float x0, y0, x1, y1; };
struct Path {
    Edge* e; size_t n, cap;
    float sx, sy, cx, cy; bool open;
    float scale;           // font units -> pixels
    bool Push(float x0, float y0, float x1, float y1) {
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 256;
            Edge* ne = (Edge*)malloc(nc * sizeof(Edge));
            if (!ne) return false;
            if (e) { memcpy(ne, e, n * sizeof(Edge)); free(e); }
            e = ne; cap = nc;
        }
        e[n++] = Edge{x0 * scale, y0 * scale, x1 * scale, y1 * scale};
        return true;
    }
    void Close() { if (open && (cx != sx || cy != sy)) Push(cx, cy, sx, sy); open = false; }
    void Move(float x, float y) { Close(); sx = cx = x; sy = cy = y; open = true; }
    void Line(float x, float y) { Push(cx, cy, x, y); cx = x; cy = y; }
    void Curve(float x1, float y1, float x2, float y2, float x3, float y3) {
        // flatten: segment count from the control polygon length in pixels
        float d = (Abs(x1 - cx) + Abs(y1 - cy) + Abs(x2 - x1) + Abs(y2 - y1) + Abs(x3 - x2) + Abs(y3 - y2)) * scale;
        int segs = int(d / 2.0f) + 2; if (segs > 48) segs = 48;
        float px = cx, py = cy;
        for (int i = 1; i <= segs; ++i) {
            float t = float(i) / segs, u = 1 - t;
            float x = u * u * u * cx + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
            float y = u * u * u * cy + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
            Push(px, py, x, y); px = x; py = y;
        }
        cx = x3; cy = y3;
    }
    static float Abs(float v) { return v < 0 ? -v : v; }
};

struct Cff {
    const u8* base; const u8* end;
    Index cs, gsubr;
    Index lsubr_default;               // non-CID fonts: the one Private dict's Subrs
    Index fd_lsubr[64]; int nfd;       // CID fonts: per Font DICT
    const u8* fdselect; bool cid;
    int nominal[64]; int nominal_default;
};

int Bias(u32 n) { return n < 1240 ? 107 : n < 33900 ? 1131 : 32768; }

// Type 2 charstring interpreter (no hint processing).
struct Interp {
    const Cff* f; Path* path; const Index* lsubr;
    float st[48]; int sp; int nstems; bool width_done; float x, y; int depth;
    bool Run(Span s) {
        if (++depth > 10) return false;
        const u8* p = s.p; const u8* e = s.p + s.n;
        while (p < e) {
            u8 b = *p++;
            if (b >= 32) {
                float v;
                if (b <= 246) v = float(int(b) - 139);
                else if (b <= 250) { if (p >= e) return false; v = float((int(b) - 247) * 256 + *p++ + 108); }
                else if (b <= 254) { if (p >= e) return false; v = float(-(int(b) - 251) * 256 - *p++ - 108); }
                else { if (p + 4 > e) return false; v = float(int32_t(Be32(p))) / 65536.0f; p += 4; }
                if (sp < 48) st[sp++] = v;
                continue;
            }
            if (b == 28) { if (p + 2 > e) return false; if (sp < 48) st[sp++] = float(int16_t(Be16(p))); p += 2; continue; }
            switch (b) {
            case 1: case 3: case 18: case 23:              // hstem vstem hstemhm vstemhm
                if (sp & 1) Width(1);
                width_done = true; nstems += sp / 2; sp = 0; break;
            case 19: case 20: {                             // hintmask cntrmask
                if (sp & 1) Width(1);
                width_done = true; nstems += sp / 2; sp = 0;
                p += (nstems + 7) / 8; break;
            }
            case 21:                                        // rmoveto
                if (sp > 2) Width(1);
                if (sp < 2) return false;
                x += st[sp - 2]; y += st[sp - 1]; path->Move(x, y); sp = 0; width_done = true; break;
            case 22:                                        // hmoveto
                if (sp > 1) Width(1);
                if (sp < 1) return false;
                x += st[sp - 1]; path->Move(x, y); sp = 0; width_done = true; break;
            case 4:                                         // vmoveto
                if (sp > 1) Width(1);
                if (sp < 1) return false;
                y += st[sp - 1]; path->Move(x, y); sp = 0; width_done = true; break;
            case 5:                                         // rlineto
                for (int i = 0; i + 1 < sp; i += 2) { x += st[i]; y += st[i + 1]; path->Line(x, y); }
                sp = 0; break;
            case 6: case 7: {                               // hlineto vlineto
                bool h = b == 6;
                for (int i = 0; i < sp; ++i) { if (h) x += st[i]; else y += st[i]; path->Line(x, y); h = !h; }
                sp = 0; break;
            }
            case 8:                                         // rrcurveto
                for (int i = 0; i + 5 < sp; i += 6) Cur(st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
                sp = 0; break;
            case 24: {                                      // rcurveline
                int i = 0;
                for (; i + 5 < sp - 2; i += 6) Cur(st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
                if (i + 1 < sp) { x += st[i]; y += st[i + 1]; path->Line(x, y); }
                sp = 0; break;
            }
            case 25: {                                      // rlinecurve
                int i = 0;
                for (; i + 1 < sp - 6; i += 2) { x += st[i]; y += st[i + 1]; path->Line(x, y); }
                if (i + 5 < sp) Cur(st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
                sp = 0; break;
            }
            case 26: {                                      // vvcurveto
                int i = 0; float dx1 = 0;
                if (sp & 1) dx1 = st[i++];
                for (; i + 3 < sp; i += 4) { Cur(dx1, st[i], st[i + 1], st[i + 2], 0, st[i + 3]); dx1 = 0; }
                sp = 0; break;
            }
            case 27: {                                      // hhcurveto
                int i = 0; float dy1 = 0;
                if (sp & 1) dy1 = st[i++];
                for (; i + 3 < sp; i += 4) { Cur(st[i], dy1, st[i + 1], st[i + 2], st[i + 3], 0); dy1 = 0; }
                sp = 0; break;
            }
            case 30: case 31: {                             // vhcurveto hvcurveto
                bool h = b == 31; int i = 0;
                while (sp - i >= 4) {
                    bool lastone = sp - i == 5;
                    if (h) Cur(st[i], 0, st[i + 1], st[i + 2], lastone ? st[i + 4] : 0, st[i + 3]);
                    else Cur(0, st[i], st[i + 1], st[i + 2], st[i + 3], lastone ? st[i + 4] : 0);
                    i += lastone ? 5 : 4; h = !h;
                }
                sp = 0; break;
            }
            case 10: case 29: {                             // callsubr callgsubr
                if (sp < 1) return false;
                const Index* ix = b == 10 ? lsubr : &f->gsubr;
                if (!ix || !ix->Ok()) return false;
                int n = int(st[--sp]) + Bias(ix->count);
                if (n < 0) return false;
                Span sub = ix->Get(u32(n));
                if (!sub.p) return false;
                if (!Run(sub)) return false;
                if (done) { --depth; return true; }
                break;
            }
            case 11: --depth; return true;                  // return
            case 14:                                        // endchar
                if (sp > 0 && !width_done) Width(1);
                path->Close(); done = true; --depth; return true;
            case 12: {                                      // escape: flex family; anything else is dropped
                if (p >= e) return false;
                u8 op = *p++;
                if (op == 35 && sp >= 12) {                 // flex
                    Cur(st[0], st[1], st[2], st[3], st[4], st[5]); Cur(st[6], st[7], st[8], st[9], st[10], st[11]);
                } else if (op == 34 && sp >= 7) {           // hflex
                    float y0 = y;
                    Cur(st[0], 0, st[1], st[2], st[3], 0);
                    Cur(st[4], 0, st[5], y0 - y, st[6], 0);
                } else if (op == 36 && sp >= 9) {           // hflex1
                    float y0 = y;
                    Cur(st[0], st[1], st[2], st[3], st[4], 0);
                    Cur(st[5], 0, st[6], st[7], st[8], y0 - (y + st[7]));
                } else if (op == 37 && sp >= 11) {          // flex1
                    float dx = 0, dy = 0;
                    for (int i = 0; i < 10; i += 2) { dx += st[i]; dy += st[i + 1]; }
                    float dx6, dy6;
                    if (Path::Abs(dx) > Path::Abs(dy)) { dx6 = st[10]; dy6 = -dy; } else { dx6 = -dx; dy6 = st[10]; }
                    Cur(st[0], st[1], st[2], st[3], st[4], st[5]);
                    Cur(st[6], st[7], st[8], st[9], dx6, dy6);
                }
                sp = 0; break;
            }
            default: sp = 0; break;
            }
        }
        --depth; return true;
    }
    bool done;
    void Width(int) { if (!width_done) { width_done = true; for (int i = 1; i < sp; ++i) st[i - 1] = st[i]; --sp; } }
    void Cur(float a, float b, float c, float d, float e, float g) {
        float x1 = x + a, y1 = y + b, x2 = x1 + c, y2 = y1 + d, x3 = x2 + e, y3 = y2 + g;
        path->Curve(x1, y1, x2, y2, x3, y3); x = x3; y = y3;
    }
};

// ---------------------------------------------------------------- rasterizer
// Nonzero winding, 5 sub-scanlines per pixel row with exact horizontal span coverage.
void Raster(const Path* p, float ox, float oy, u8* out, int w, int h) {
    const int SUB = 5;
    float* acc = (float*)calloc(size_t(w) + 2, sizeof(float));
    struct X { float x; int d; };
    X* xs = (X*)malloc((p->n + 1) * sizeof(X));
    if (!acc || !xs) { free(acc); free(xs); return; }
    for (int row = 0; row < h; ++row) {
        for (int i = 0; i < w + 2; ++i) acc[i] = 0;
        for (int s = 0; s < SUB; ++s) {
            float sy = row + (s + 0.5f) / SUB;
            int nx = 0;
            for (size_t i = 0; i < p->n; ++i) {
                const Edge& e = p->e[i];
                // device space: x right, y down. Font y up -> flip.
                float y0 = oy - e.y0, y1 = oy - e.y1;
                if (y0 == y1) continue;
                int dir = 1;
                float xa = e.x0 + ox, xb = e.x1 + ox;
                if (y0 > y1) { float t = y0; y0 = y1; y1 = t; t = xa; xa = xb; xb = t; dir = -1; }
                if (sy < y0 || sy >= y1) continue;
                xs[nx++] = X{xa + (sy - y0) / (y1 - y0) * (xb - xa), dir};
            }
            for (int i = 1; i < nx; ++i) { X k = xs[i]; int j = i - 1; while (j >= 0 && xs[j].x > k.x) { xs[j + 1] = xs[j]; --j; } xs[j + 1] = k; }
            int wind = 0;
            for (int i = 0; i + 1 < nx; ++i) {
                wind += xs[i].d;
                if (wind == 0) continue;
                float a = xs[i].x, b = xs[i + 1].x;
                if (a < 0) a = 0;
                if (b > w) b = float(w);
                if (b <= a) continue;
                int ia = int(a), ib = int(b);
                if (ia == ib) { acc[ia] += b - a; continue; }
                acc[ia] += (ia + 1) - a;
                for (int k = ia + 1; k < ib; ++k) acc[k] += 1;
                if (ib < w) acc[ib] += b - ib;
            }
        }
        for (int i = 0; i < w; ++i) {
            float v = acc[i] / SUB; if (v > 1) v = 1;
            out[size_t(row) * w + i] = u8(v * 255.0f + 0.5f);
        }
    }
    free(acc); free(xs);
}

// ---------------------------------------------------------------- the font
struct Built {
    u32 line_height;              // cap height in atlas pixels
    EdenDsmodFontGlyph glyph[224];
    u8* atlas; u32 aw, ah;        // RGBA, white with coverage in alpha
};

const u32 kFirst = 0x20, kCount = 224;
const float kCap = 44.0f;         // atlas pixels per capital H

bool FindTable(const u8* f, size_t n, const char* tag, Span* out) {
    if (n < 12) return false;
    u16 nt = Be16(f + 4);
    if (12 + size_t(nt) * 16 > n) return false;
    for (u16 i = 0; i < nt; ++i) {
        const u8* r = f + 12 + size_t(i) * 16;
        if (r[0] == tag[0] && r[1] == tag[1] && r[2] == tag[2] && r[3] == tag[3]) {
            u32 off = Be32(r + 8), len = Be32(r + 12);
            if (size_t(off) + len > n) return false;
            *out = {f + off, len}; return true;
        }
    }
    return false;
}

// cmap: codepoint -> glyph id (format 4 or 12, Unicode subtables).
u32 MapCode(Span cm, u32 c) {
    if (cm.n < 4) return 0;
    u16 n = Be16(cm.p + 2);
    const u8* best = nullptr; u16 bestfmt = 0;
    for (u16 i = 0; i < n && 4 + size_t(i) * 8 + 8 <= cm.n; ++i) {
        const u8* r = cm.p + 4 + size_t(i) * 8;
        u16 pid = Be16(r), eid = Be16(r + 2); u32 off = Be32(r + 4);
        if (off + 4 > cm.n) continue;
        if (!(pid == 0 || (pid == 3 && (eid == 1 || eid == 10)))) continue;
        u16 fmt = Be16(cm.p + off);
        if ((fmt == 12 && bestfmt != 12) || (fmt == 4 && !best)) { best = cm.p + off; bestfmt = fmt; }
    }
    if (!best) return 0;
    const u8* end = cm.p + cm.n;
    if (bestfmt == 4) {
        u16 segx2 = Be16(best + 6);
        const u8* ends = best + 14; const u8* starts = ends + segx2 + 2;
        const u8* deltas = starts + segx2; const u8* ranges = deltas + segx2;
        if (ranges + segx2 > end) return 0;
        for (u16 s = 0; s < segx2 / 2; ++s) {
            u16 e = Be16(ends + s * 2), st = Be16(starts + s * 2);
            if (c > e) continue;
            if (c < st) return 0;
            u16 d = Be16(deltas + s * 2), ro = Be16(ranges + s * 2);
            if (ro == 0) return u16(c + d);
            const u8* g = ranges + s * 2 + ro + (c - st) * 2;
            if (g + 2 > end) return 0;
            u16 gid = Be16(g);
            return gid ? u16(gid + d) : 0;
        }
        return 0;
    }
    u32 ng = Be32(best + 12);
    for (u32 i = 0; i < ng && best + 16 + size_t(i) * 12 + 12 <= end; ++i) {
        const u8* g = best + 16 + size_t(i) * 12;
        u32 a = Be32(g), b = Be32(g + 4);
        if (c >= a && c <= b) return Be32(g + 8) + (c - a);
    }
    return 0;
}

bool OpenCff(Span c, Cff* f) {
    memset(f, 0, sizeof(*f));
    f->base = c.p; f->end = c.p + c.n;
    if (c.n < 4) return false;
    const u8* p = c.p + c.p[2];
    Index names, top, strings;
    p = ReadIndex(p, f->end, &names); if (!p) return false;
    p = ReadIndex(p, f->end, &top); if (!p || top.count < 1) return false;
    p = ReadIndex(p, f->end, &strings); if (!p) return false;
    p = ReadIndex(p, f->end, &f->gsubr); if (!p) return false;
    long csoff = -1, priv_size = 0, priv_off = -1, fdarr = -1, fdsel = -1;
    WalkDict(top.Get(0), [&](int op, const Dict& d) {
        if (op == 17 && d.n >= 1) csoff = long(d.v[0]);
        else if (op == 18 && d.n >= 2) { priv_size = long(d.v[0]); priv_off = long(d.v[1]); }
        else if (op == 1236 && d.n >= 1) fdarr = long(d.v[0]);
        else if (op == 1237 && d.n >= 1) fdsel = long(d.v[0]);
        else if (op == 1230) f->cid = true;
    });
    if (csoff < 0 || !ReadIndex(c.p + csoff, f->end, &f->cs)) return false;
    auto load_private = [&](long size, long off, Index* subrs, int* nominal) {
        *nominal = 0;
        if (off < 0 || size <= 0 || size_t(off + size) > c.n) return;
        long subr = -1;
        WalkDict(Span{c.p + off, size_t(size)}, [&](int op, const Dict& d) {
            if (op == 19 && d.n >= 1) subr = long(d.v[0]);
            else if (op == 21 && d.n >= 1) *nominal = int(d.v[0]);
        });
        if (subr >= 0) ReadIndex(c.p + off + subr, f->end, subrs);
    };
    if (f->cid && fdarr >= 0 && fdsel >= 0) {
        Index fda;
        if (!ReadIndex(c.p + fdarr, f->end, &fda)) return false;
        f->nfd = fda.count > 64 ? 64 : int(fda.count);
        for (int i = 0; i < f->nfd; ++i) {
            long ps = 0, po = -1;
            WalkDict(fda.Get(u32(i)), [&](int op, const Dict& d) { if (op == 18 && d.n >= 2) { ps = long(d.v[0]); po = long(d.v[1]); } });
            load_private(ps, po, &f->fd_lsubr[i], &f->nominal[i]);
        }
        f->fdselect = c.p + fdsel;
    } else {
        load_private(priv_size, priv_off, &f->lsubr_default, &f->nominal_default);
    }
    return true;
}

int FdOf(const Cff* f, u32 gid) {
    if (!f->fdselect) return -1;
    const u8* s = f->fdselect;
    if (s >= f->end) return -1;
    if (s[0] == 0) return s + 1 + gid < f->end ? s[1 + gid] : -1;
    if (s[0] == 3) {
        u16 nr = Be16(s + 1);
        for (u16 i = 0; i < nr; ++i) {
            const u8* r = s + 3 + size_t(i) * 3;
            if (r + 5 > f->end) return -1;
            u16 first = Be16(r), next = Be16(r + 3);
            if (gid >= first && gid < next) return r[2];
        }
    }
    return -1;
}

bool Outline(const Cff* f, u32 gid, Path* path) {
    Span s = f->cs.Get(gid);
    if (!s.p) return false;
    Interp in{};
    in.f = f; in.path = path;
    int fd = FdOf(f, gid);
    in.lsubr = (fd >= 0 && fd < f->nfd) ? &f->fd_lsubr[fd] : &f->lsubr_default;
    bool ok = in.Run(s);
    path->Close();
    return ok;
}

struct Box { float x0, y0, x1, y1; };
Box Bounds(const Path* p) {
    Box b{1e9f, 1e9f, -1e9f, -1e9f};
    for (size_t i = 0; i < p->n; ++i) {
        const Edge& e = p->e[i];
        if (e.x0 < b.x0) b.x0 = e.x0;
        if (e.x1 < b.x0) b.x0 = e.x1;
        if (e.y0 < b.y0) b.y0 = e.y0;
        if (e.y1 < b.y0) b.y0 = e.y1;
        if (e.x0 > b.x1) b.x1 = e.x0;
        if (e.x1 > b.x1) b.x1 = e.x1;
        if (e.y0 > b.y1) b.y1 = e.y0;
        if (e.y1 > b.y1) b.y1 = e.y1;
    }
    return b;
}

int Floor(float v) { int i = int(v); return (float(i) > v) ? i - 1 : i; }
int Ceil(float v) { int i = int(v); return (float(i) < v) ? i + 1 : i; }

// Builds the atlas from an OpenType/CFF file. Returns false if it cannot.
bool Build(const u8* otf, size_t n, Built* out) {
    memset(out, 0, sizeof(*out));
    if (n < 12 || Be32(otf) != 0x4F54544F) return false;      // "OTTO"
    Span cff, cmap, hmtx, hhea, head;
    if (!FindTable(otf, n, "CFF ", &cff) || !FindTable(otf, n, "cmap", &cmap) || !FindTable(otf, n, "hmtx", &hmtx) ||
        !FindTable(otf, n, "hhea", &hhea) || !FindTable(otf, n, "head", &head)) return false;
    if (hhea.n < 36 || head.n < 20) return false;
    u16 upem = Be16(head.p + 18); if (!upem) upem = 1000;
    u16 nhm = Be16(hhea.p + 34);
    Cff* f = (Cff*)malloc(sizeof(Cff));
    if (!f) return false;
    if (!OpenCff(cff, f)) { free(f); return false; }
    // scale from the capital H
    float scale = 0;
    {
        Path hp{}; hp.scale = 1;
        u32 g = MapCode(cmap, 'H');
        if (g && Outline(f, g, &hp) && hp.n) { Box b = Bounds(&hp); if (b.y1 > 0) scale = kCap / b.y1; }
        free(hp.e);
    }
    if (scale <= 0) scale = kCap / (upem * 0.7f);
    // render every glyph into its own bitmap, then pack
    struct G { u8* px; int w, h; int bx, by; u16 adv; };
    G* gs = (G*)calloc(kCount, sizeof(G));
    if (!gs) { free(f); return false; }
    for (u32 i = 0; i < kCount; ++i) {
        u32 c = kFirst + i;
        u32 gid = MapCode(cmap, c);
        if (c == 0xA0 && !gid) gid = MapCode(cmap, ' ');
        u16 aw = 0;
        if (gid) {
            u32 k = gid < nhm ? gid : (nhm ? nhm - 1u : 0u);
            if (hmtx.n >= size_t(k) * 4 + 2) aw = Be16(hmtx.p + size_t(k) * 4);
        }
        gs[i].adv = u16(aw * scale + 0.5f);
        if (!gid) { if (c != ' ') gs[i].adv = 0; continue; }
        Path pth{}; pth.scale = scale;
        if (!Outline(f, gid, &pth) || pth.n == 0) { free(pth.e); continue; }
        Box b = Bounds(&pth);
        int x0 = Floor(b.x0) - 1, x1 = Ceil(b.x1) + 1, y0 = Floor(b.y0) - 1, y1 = Ceil(b.y1) + 1;
        int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0 || w > 400 || h > 400) { free(pth.e); continue; }
        gs[i].px = (u8*)calloc(size_t(w) * h, 1);
        if (gs[i].px) {
            Raster(&pth, float(-x0), float(y1), gs[i].px, w, h);
            gs[i].w = w; gs[i].h = h; gs[i].bx = x0; gs[i].by = y1;
        }
        free(pth.e);
    }
    free(f);
    // pack in rows
    const u32 AW = 1024;
    u32 x = 1, y = 1, rowh = 0;
    for (u32 i = 0; i < kCount; ++i) {
        if (!gs[i].px) continue;
        if (x + gs[i].w + 1 > AW) { x = 1; y += rowh + 1; rowh = 0; }
        out->glyph[i].x = u16(x); out->glyph[i].y = u16(y);
        x += gs[i].w + 1; if (u32(gs[i].h) > rowh) rowh = gs[i].h;
    }
    u32 AH = y + rowh + 1; if (AH < 8) AH = 8;
    out->atlas = (u8*)calloc(size_t(AW) * AH * 4, 1);
    if (!out->atlas) { for (u32 i = 0; i < kCount; ++i) free(gs[i].px); free(gs); return false; }
    out->aw = AW; out->ah = AH;
    for (u32 i = 0; i < kCount; ++i) {
        EdenDsmodFontGlyph& g = out->glyph[i];
        g.advance = gs[i].adv;
        if (!gs[i].px) { g.x = g.y = g.w = g.h = 0; g.bearing_x = g.bearing_y = 0; continue; }
        g.w = u16(gs[i].w); g.h = u16(gs[i].h);
        g.bearing_x = int16_t(gs[i].bx); g.bearing_y = int16_t(gs[i].by);
        for (int r = 0; r < gs[i].h; ++r)
            for (int c = 0; c < gs[i].w; ++c) {
                u8* d = out->atlas + ((size_t(g.y) + r) * AW + g.x + c) * 4;
                d[0] = d[1] = d[2] = 255; d[3] = gs[i].px[size_t(r) * gs[i].w + c];
            }
        free(gs[i].px);
    }
    free(gs);
    out->line_height = u32(kCap + 0.5f);
    return true;
}

} // namespace font
