#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the native module's game data layer (module/dsts_data.inc) against an independent reading
of the same game tables in Python.

  python3 check_gamedata.py <manifest.json> <dsts_test> <app_0 archive> <app_text01 archive>

<dsts_test> is the module built as a host test program (see README.md). The archives are the
player's own app_0.nx64.mvgl and app_text01.nx64.mvgl (from the game's romfs, update 1.2.2). The
script samples every page the data layer fills, with every item and Digimon selected, and compares
each published value with what this file works out. Nothing is written; no game data is printed
unless a value differs.
"""
import json
import struct
import subprocess
import sys


# ---------------------------------------------------------------- archive + EXPA (the game's formats)
def lz4_block(src, size):
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        t = src[i]; i += 1
        ll = t >> 4
        if ll == 15:
            while True:
                b = src[i]; i += 1; ll += b
                if b != 255:
                    break
        dst += src[i:i + ll]; i += ll
        if i >= n or len(dst) >= size:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = t & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255:
                    break
        ml += 4
        st = len(dst) - off
        for k in range(ml):
            dst.append(dst[st + k])
    return bytes(dst)


def members(path):
    """{(is_text_dir, bare name): bytes} for every .mbe member of an MDB1 archive."""
    out = {}
    with open(path, "rb") as f:
        h = f.read(0x20)
        if h[:4] != b"MDB1":
            return out
        fc, nc, dc, ds = struct.unpack_from("<IIIQ", h, 4)
        tree, names, data = f.read(fc * 16), f.read(nc * 0x80), f.read(dc * 16)
        for i in range(1, min(fc, nc)):
            did = struct.unpack_from("<I", tree, i * 16 + 4)[0]
            if did >= dc:
                continue
            e = names[i * 0x80:(i + 1) * 0x80]
            if e[:3] != b"mbe":
                continue
            nm = e[4:].split(b"\0")[0].decode("utf8", "replace")
            base = nm.replace("/", "\\").split("\\")[-1]
            text = nm.startswith(("text\\", "text/"))
            off, full, comp = struct.unpack_from("<QII", data, did * 16)
            f.seek(ds + off)
            b = f.read(comp)
            out.setdefault((text, base), b if comp == full else lz4_block(b, full))
    return out


SZ = {2: (4, 4), 3: (2, 2), 4: (1, 1), 5: (4, 4), 6: (8, 8), 7: (8, 8), 8: (8, 8), 10: (4, 4)}


def layout(types, z10=False):
    off, cols, bit, base = 0, [], None, 0
    for t in types:
        if t == 9:
            if bit is None or bit >= 32:
                off = (off + 3) // 4 * 4; base = off; off += 4; bit = 0
            cols.append((t, base, bit)); bit += 1
            continue
        bit = None
        al, sz = SZ.get(t, (4, 4))
        if t == 10 and z10:
            al, sz = 1, 0
        off = (off + al - 1) // al * al
        cols.append((t, off, None)); off += sz
    return cols


def expa(b):
    """{table name: [row, ...]} with ints, floats and strings (None for an empty string cell)."""
    assert b[:4] == b"EXPA"
    nt = struct.unpack_from("<I", b, 4)[0]; p = 8; tables = []
    for _ in range(nt):
        nl = struct.unpack_from("<I", b, p)[0]; p += 4
        name = b[p:p + nl].split(b"\0")[0].decode("utf8", "replace"); p += nl
        sc = struct.unpack_from("<I", b, p)[0]; p += 4
        types = list(struct.unpack_from("<%dI" % sc, b, p)); p += 4 * sc
        esize, ec = struct.unpack_from("<II", b, p); p += 8
        p = (p + 7) // 8 * 8; stride = (esize + 7) // 8 * 8
        tables.append((name, types, esize, ec, p, stride)); p += stride * ec
    ci = b.find(b"CHNK", p - 16 if p >= 16 else 0)
    strs = {}
    if ci >= 0:
        n = struct.unpack_from("<I", b, ci + 4)[0]; q = ci + 8
        for _ in range(n):
            off, sz = struct.unpack_from("<II", b, q); q += 8
            strs[off] = b[q:q + sz].split(b"\0")[0].decode("utf8", "replace"); q += sz
    out = {}
    for name, types, esize, ec, start, stride in tables:
        cols = layout(types)
        if cols:
            t, o, _ = cols[-1]
            end = o + 4 if t == 9 else o + SZ.get(t, (4, 4))[1]
            if end > esize:
                cols = layout(types, True)
        rows = []
        for r in range(ec):
            a0 = start + r * stride; row = []
            for t, o, bit in cols:
                a = a0 + o
                if t in (6, 7, 8):
                    row.append(strs.get(a))
                elif t == 9:
                    row.append((struct.unpack_from("<I", b, a)[0] >> bit) & 1)
                elif t == 5:
                    row.append(struct.unpack_from("<f", b, a)[0])
                elif t == 3:
                    row.append(struct.unpack_from("<h", b, a)[0])
                elif t == 4:
                    row.append(b[a])
                else:
                    row.append(struct.unpack_from("<i", b, a)[0])
            rows.append(row)
        out[name] = rows
    return out


def clean(s):
    """== public_text.clean (drop {..} markup, keep {fcN..} inner text, collapse whitespace)."""
    def walk(t):
        out, i = [], 0
        while i < len(t):
            if t[i] == "{":
                depth, j = 1, i + 1
                while j < len(t) and depth:
                    depth += (t[j] == "{") - (t[j] == "}")
                    j += 1
                inner = t[i + 1:j - 1] if depth == 0 else t[i + 1:j]
                k = 2
                if inner[:2] == "fc" and len(inner) > 2 and inner[2].isdigit():
                    while k < len(inner) and inner[k].isdigit():
                        k += 1
                    out.append(walk(inner[k:]))
                i = j
            else:
                out.append(t[i]); i += 1
        return "".join(out)
    return " ".join(walk(s).split())


def keyed(b):
    t = expa(b)["Sheet1"]
    out = {}
    for row in t:
        k, v = row[0], [x for x in row if isinstance(x, str) or x is None][-1]
        if isinstance(k, str) and k.lstrip("-").isdigit() and v and clean(v):
            out.setdefault(int(k), clean(v))
    return out


# ---------------------------------------------------------------- what the module should publish
STATS = {"HP": "max HP", "SP": "max SP", "ATK": "ATK", "DEF": "DEF", "INT": "INT", "SPI": "SPI", "SPD": "SPD",
         "ACU": "accuracy", "EVA": "evasion", "CRT": "critical rate"}


def expected(man, app0, text01):
    D = man["dsts_data"]
    tbl = {}
    for path in (app0, text01):
        for k, v in members(path).items():
            tbl.setdefault(k, v)
    data = lambda n: expa(tbl[(False, n)])
    item_name, item_expl = keyed(tbl[(True, "item_name")]), keyed(tbl[(True, "item_explanation")])
    field_name, skill_name = keyed(tbl[(True, "field_name")]), keyed(tbl[(True, "skill_name")])
    buff_name = keyed(tbl[(True, "buff_name")])
    skills = {}
    for r in data("battle_skill")["battle_skill_list"]:
        skills.setdefault(r[0], (r[15], r[22], r[23], r[25], r[33], r[57]))
    items = {r[0]: r for r in data("item")["item_parameter"]}
    treasure = [(r[0] // 100, r[4]) for r in data("field_treasure")["field_treasure"]]
    shop = {r[2] for r in data("shop")["shop_lineup"]}
    quests = {r[0]: r for r in data("quest_sub")["sub_quest_list"]}
    be = data("battle_enemy")
    enemy = {}
    for r in be["enemy_parameter"]:
        enemy.setdefault(r[0], r[1])
    groups = {}
    for r in be["encount_group"]:
        groups.setdefault(r[0], r)
    g2s = D["g2s"]
    s2g = {}
    for g, st in enumerate(g2s):
        if st >= 0:
            s2g.setdefault(st, g)

    def th(v):
        return f"{v:,}"

    def price(i):
        r = items.get(i)
        buy = r[9] if r and r[9] > 0 else 0
        sell = r[10] if r and r[10] > 0 else 0
        if buy and sell:
            return f"Buy {th(buy)} yen  /  Sell {th(sell)} yen"
        if buy:
            return f"Buy {th(buy)} yen  (can't be sold)"
        if sell:
            return f"Sells for {th(sell)} yen"
        return "No shop price"

    def skill_effect(i):
        r = items.get(i)
        q = skills.get(r[13]) if r else None
        if not q:
            return None
        kind, mode, amount, stat, target, status = q
        who = "all allies" if target == 6 else "one ally"
        what = {1: "HP", 2: "SP", 3: "HP and SP"}.get(stat)
        if kind in (31011, 31013) and what and amount > 0 and mode in (9, 10):
            return f"Restores {th(amount)}{'% of ' if mode == 10 else ' '}{what} to {who}."
        if kind == 31031 and amount > 0:
            return f"{'Revives KO' + chr(39) + 'd allies' if target == 6 else 'Revives one KO' + chr(39) + 'd ally'} with {th(amount)}% HP."
        if kind in (31041, 31042) and status >= 1000:
            b = status // 1000 - 100
            a = buff_name.get(b)
            c = buff_name.get(b + 5) if kind == 31041 else None
            if not a:
                return None
            return f"Cures {a}{' and ' + c if c else ''} on {who}."
        if kind == 31051:
            return f"Removes stat drops from {who}."
        if kind == 31061:
            return f"Removes status problems, Injury and Disease from {who}."
        return None

    def effect(i):
        if item_expl.get(i):
            return item_expl[i]
        if skill_effect(i):
            return skill_effect(i)
        nm = item_name.get(i, "")
        import re
        m = re.fullmatch(r"([A-Z]+) Attachment (I+)", nm)
        if m and m.group(1) in STATS:
            return f"Equipment that raises {STATS[m.group(1)]}. Higher numbers are stronger."
        return "No description in the game's own text."

    areas = []
    for key, maps, fields in D["areas"]:
        chests = [it for m in maps for mm, it in treasure if mm == m and it > 0]
        wild = set()
        for fid in fields:
            code = "?tdhe"[fid // 10000] + f"{fid % 10000:04d}"
            b = tbl.get((False, f"field_battle_{code}"))
            if not b:
                continue
            for r in expa(b).get("field_symbol_enemy", []):
                gr = groups.get(r[1])
                if not gr:
                    continue
                for s in (1, 14, 26, 38, 50, 62):
                    e = gr[s]
                    if e <= 0:
                        continue
                    g = s2g.get(enemy.get(e, e // 100))
                    if g is not None and g != 1:
                        wild.add(g)
        areas.append((key, chests, sorted(wild)))

    def item_chests(i):
        out = []
        for m, it in treasure:
            key = m + 10000
            if it == i and key < 30000 and field_name.get(key) and field_name[key] not in out:
                out.append(field_name[key])
        return out

    def found(num):
        names = [field_name[k] for k, _, w in areas if num in w and k in field_name]
        if not names:
            return None
        out = []
        for k, a in enumerate(names):
            if len(", ".join(out + [a])) > 240 - 16:
                return ", ".join(out) + f", and {len(names) - k} more (see Area guide)"
            out.append(a)
        return ", ".join(out)

    def reward(q):
        r = quests.get(q)
        if not r:
            return None
        out = []
        if r[8] > 0:
            out.append(f"{th(r[8])} yen")
        if r[9] > 0:
            out.append(f"{th(r[9])} AP")
        for c in (10, 13, 16):
            i, n = r[c], r[c + 1]
            nm = item_name.get(i) or skill_name.get(i) if i > 0 else None
            if nm:
                out.append(nm + (f" x{th(n)}" if n > 1 else ""))
        return ", ".join(out) or None

    exp = {}   # (page, flags) -> {name: value}
    base = {f"dsts.arc{i}": f"{th(len(c))} chests   {th(len(w))} wild Digimon" for i, (_, c, w) in enumerate(areas)}
    base["dsts.data"] = "11"
    exp[("areas", ())] = dict(base, **{f"dsts.arch{i}": f"{th(len(c))} chests" for i, (_, c, _) in enumerate(areas)},
                              **{f"dsts.arw{i}": f"{th(len(w))} Digimon" for i, (_, _, w) in enumerate(areas)})
    tpl = man["dsts_text"]
    for i, (_, c, w) in enumerate(areas):
        e = dict(base)
        e["dsts.ac.n"] = str(len(c)); e["dsts.aw.n"] = str(len(w))
        e["dsts.ac.tab"] = f"CHESTS ({th(len(c))})"; e["dsts.aw.tab"] = f"DIGIMON ({th(len(w))})"
        for k, it in enumerate(c[:12]):
            e[f"dsts.ac{k}"] = "1"; e[f"dsts.aci{k}"] = str(it)
            if item_name.get(it) or skill_name.get(it):
                e[f"dsts.acn{k}"] = item_name.get(it) or skill_name.get(it)
            if it in D["items"]:
                e[f"dsts.ace{k}"] = effect(it)
        for k, num in enumerate(w[:25]):
            e[f"dsts.aw{k}"] = "1"
            e[f"dsts.aws{k}"] = str(1000 + g2s[num] if g2s[num] >= 0 else 0)
            e[f"dsts.awp{k}"] = str((100 + i) * 1000 + num)
            e[f"dsts.awn{k}"] = ("TEMPLATE", D["dexname"][num])
        exp[(f"ar{i}", ())] = e
    for g, i in enumerate(D["items"]):
        e = dict(base)
        e["dsts.it.id"] = str(i); e["dsts.it.price"] = price(i); e["dsts.it.eff"] = effect(i)
        ch = item_chests(i)
        if i in shop:
            e["dsts.it.shop"] = "Sold in shops"
        if ch:
            e["dsts.it.chn"] = f"In chests ({th(len(ch))} area{'' if len(ch) == 1 else 's'}):"
        for r, a in enumerate(ch[:6]):
            e[f"dsts.it.r{r}"] = "1"; e[f"dsts.it.ch{r}"] = a
        if len(ch) > 6:
            e["dsts.it.more"] = f"and {th(len(ch) - 6)} more"
        if i not in shop and not ch:
            e["dsts.it.none"] = "Not sold in shops or found in chests. It comes from quests, battles or crafting."
        exp[("it", (("@flag:its", g),))] = e
    for page, gs in D["itempages"].items():
        exp[(page, ())] = dict(base, **{f"dsts.ie{r}": effect(D["items"][g]) for r, g in enumerate(gs)})
    for num in range(1, len(g2s)):
        if g2s[num] < 0:
            continue
        e = dict(base)
        f = found(num)
        e["dsts.dx.wild" if f else "dsts.dx.nowild"] = f or "Not found wild in the game data"
        exp[("dx", (("@flag:dxh", 1000 + num),))] = e
    for page, qs in D["quests"].items():
        e = dict(base)
        for s, q in enumerate(qs):
            r = reward(q) if q > 0 else None
            e[f"dsts.rwok{s}"] = "1" if r else "0"
            if r:
                e[f"dsts.rw{s}"] = r
        exp[(page, ())] = e
    return exp


def main():
    man_path, test, app0, text01 = sys.argv[1:5]
    man = json.load(open(man_path, encoding="utf8"))
    exp = expected(man, app0, text01)
    args = [test, "--file", f"app_0.nx64.mvgl={app0}", "--file", f"app_text01.nx64.mvgl={text01}",
            "--config", man_path]
    order = list(exp)
    for page, flags in order:
        for k, v in flags:
            args += ["--flag", f"{k}={v}"]
        args += ["--page", page]
    out = subprocess.run(args, capture_output=True, text=True)
    assert out.returncode == 0, out.stderr[-3000:]
    got, seq, last = {}, -1, None
    for line in out.stdout.split("\n"):
        if line.count("\t") < 2:
            continue
        page, name, val = line.split("\t", 2)
        if name == "dsts.module":   # the first value of every sample
            seq += 1
        if not (name.startswith("dsts.") and name not in ("dsts.module", "dsts.archive", "dsts.pictures",
                                                         "dsts.text", "dsts.text_total")):
            continue
        got.setdefault(seq, {})[name] = val
    shown = {}
    bad = checked = 0
    for n, key in enumerate(order):
        g, e = got.get(n, {}), exp[key]
        for name, want in e.items():
            if isinstance(want, tuple):   # a dex name: the module's filled template
                if name not in g:
                    bad += 1; print("missing", key, name)
                continue
            checked += 1
            if g.get(name) != want:
                bad += 1
                if bad <= 20:
                    print("differs", key, name, repr(g.get(name)), "want", repr(want))
        for name in g:
            if name not in e:
                bad += 1
                if bad <= 20:
                    print("extra", key, name, repr(g[name]))
    print(f"{len(order)} samples, {checked} values checked, {bad} differences")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
