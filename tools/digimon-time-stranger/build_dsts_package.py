#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the Time Stranger Field Guide companion (.dsmod.zip) for Digimon Story Time Stranger.
Usage: python3 build_dsts_package.py <TITLEID> [--personal]

Default: the public build. It ships no game pictures, no game text and no game data: the native
module reads pictures, text, prices, chests, wild Digimon and rewards from the player's own game
files at run time. Everything it needs is in this folder, plus text122.json, which
tools/dump_text.py makes from the builder's own copy of the game (the build takes the game text out
of the package again; see public_text.py).
--personal: the author's private variant with pictures baked in. It needs picture folders that are
not part of the repository and its package must never be shared.
"""
import json, os, re, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PORTRAITS = os.path.join(HERE, "portraits")
GPORTRAITS = os.path.join(HERE, "portraits_game")
GAMEICONS = os.path.join(HERE, "gameicons")
PICSRC = {}
for _d in (PORTRAITS, GPORTRAITS):  # game portraits win over the PDF ones
    if os.path.isdir(_d):
        for _f in os.listdir(_d):
            if _f.endswith(".png"):
                PICSRC[int(_f[:-4])] = os.path.join(_d, _f)
PUBLIC = "--personal" not in sys.argv
G2S = {int(k): v for k, v in json.load(open(os.path.join(HERE, "guide2status.json"))).items()}
if PUBLIC:
    PICSRC = {n: None for n in G2S}
HASPIC = set(PICSRC)
import guidedata
_GDATA = guidedata.load()   # the guide's data (see guidedata.py: no game text in the repository)
GI = _GDATA["gameindex"]     # identifiers only: item groups and the map/field ids of each area
DEXGROUPS, ATTR, ROWS = (_GDATA["dexdata"][k] for k in ("DEXGROUPS", "ATTR", "ROWS"))
# Official English spellings from the game's own name table
for _n, _nm in {18: "Bukamon", 148: "Geremon", 166: "BlackGatomon", 174: "Ankylomon", 180: "Shurimon",
                274: "Groundramon", 303: "Paildramon", 306: "Beowolfmon", 394: "Pharaohmon"}.items():
    if _n in ROWS:
        ROWS[_n] = (ROWS[_n][0], _nm) + tuple(ROWS[_n][2:])
for _k, (_t, _rows) in enumerate(DEXGROUPS):
    DEXGROUPS[_k] = (_t, [ROWS[_r[0]] for _r in _rows])
NRANK = len(DEXGROUPS)        # the rank lists; after them come the find-by-letter and find-by-type lists
_RANKOF = {r[0]: _t for _t, _rows in DEXGROUPS for r in _rows}
_ALLDEX = sorted({r[0] for _t, _rows in DEXGROUPS for r in _rows}, key=lambda n: (ROWS[n][1].lower(), n))
LETTERS = sorted({ROWS[n][1][0].upper() for n in _ALLDEX})
for _L in LETTERS:
    DEXGROUPS.append((f"Names starting with {_L}", [ROWS[n] for n in _ALLDEX if ROWS[n][1][0].upper() == _L]))
FIND_TYPES = [a for a in ("V", "D", "R", "F", "X", "U", "N") if any(ROWS[n][2] == a for n in _ALLDEX)]
for _a in FIND_TYPES:
    DEXGROUPS.append((f"{ATTR[_a]} Digimon", [ROWS[n] for n in _ALLDEX if ROWS[n][2] == _a]))
LOC = _GDATA["dexloc"]["LOC"]
MAIN, SIDE, TIP_PARTS, ATTR_CHART, ELEMENTS, SIDE_REMAINING = (_GDATA["questdata"][k] for k in (
    "MAIN", "SIDE", "TIP_PARTS", "ATTR_CHART", "ELEMENTS", "SIDE_REMAINING"))

# Official text tables, from text122.json (dumped from the builder's own game by tools/dump_text.py).
GT = _GDATA["gametext"]


def _norm(t):
    return "".join(c for c in t.lower() if c.isalnum())


def _clean(t):
    return " ".join((t or "").replace("\n", " ").split())


QTITLE = {_norm(v): int(k) for k, v in GT.get("text/quest_title:Sheet1", []) if v}
QSTEPS, QCLIENT, QOUT, MSTEPS = {}, {}, {}, {}
for k, v in GT.get("text/quest_step:Sheet1", []):
    if v:
        QSTEPS.setdefault(int(k) // 1000, []).append((int(k) % 1000, _clean(v)))
for k, v in GT.get("text/quest_client:Sheet1", []):
    if v:
        QCLIENT[int(k)] = _clean(v)
for k, v in GT.get("text/quest_outline:Sheet1", []):
    if v:
        QOUT[int(k)] = _clean(v)
for k, v in GT.get("text/main_quest_step:Sheet1", []):
    if v:
        MSTEPS.setdefault(int(k) // 1000, []).append((int(k) % 1000, _clean(v)))


def game_steps(qid):
    return "  ".join(f"{n + 1}) {t}" for n, (_, t) in enumerate(sorted(QSTEPS.get(qid, []))))


ALIAS = {"memoriesofbond": "memoriesofbonds"}
# Steps: the game's own objective list for each mission (game text, filled in from the player's files).
MTITLE = {_norm(v): int(k) for k, v in GT.get("text/main_quest_title:Sheet1", []) if v}
MAIN = [list(m) for m in MAIN]
for _m in MAIN:
    _q = MTITLE.get(_norm(_m[0]))
    _m[1] = ("  ".join(f"{n + 1}) {t}" for n, (_, t) in enumerate(sorted(MSTEPS[_q])))
             if _q in MSTEPS else "Steps not listed in the game's data.")
    if _m[2].startswith("Steps not added"):
        _m[2] = "No boss listed yet."
SIDE = [list(x) + [None] for x in SIDE]
for x in SIDE:
    q = QTITLE.get(_norm(x[1]))
    x[6] = q
    x[3] = game_steps(q) if q in QSTEPS else "Steps not listed in the game's data."
for t in SIDE_REMAINING:
    q = QTITLE.get(ALIAS.get(_norm(t), _norm(t)))
    if not q or q not in QSTEPS:
        continue
    SIDE.append([100 + q, t, "Not added yet. Most of these open after A Gift from the Hosts (Time Jump).",
                 game_steps(q), "Not added yet.", "Not added yet.", q])
SIDE_LEFT = [t for t in SIDE_REMAINING if not QTITLE.get(ALIAS.get(_norm(t), _norm(t)))]

VERSION = "1.0.0"
PKG_NAME = "Time Stranger Field Guide"
PKG_FILE = "TimeStrangerFieldGuide"     # public file: <TITLEID>-<PKG_FILE>-<X.Y.Z>.dsmod.zip
MIN_RUNTIME = 19   # manifest over 1 MiB needs runtime 19 discovery (Eden Duo 1.2.0+)
W, H = 1240, 1080

# Text sizes (glyph scale). Raise these if text is still too small.
T_BODY = 4      # quest steps, tips, item text
T_ROW = 4       # list rows
T_TITLE = 5     # page titles
T_SMALL = 3     # captions

MATCH = {"V": ("Virus", "Data"), "D": ("Vaccine", "Virus"), "R": ("Data", "Vaccine")}
_byname = {r[1].lower(): n for n, r in ROWS.items()}
INTO = {}
for _n, _t in LOC.items():
    for _pre in ("Evolve from ", "Mode change from "):
        if _t.startswith(_pre) or ". " + _pre in _t or ("Story. " + _pre) in _t:
            _seg = _t.split(_pre, 1)[1].split(". ")[0].split(" after ")[0]
            for _p in re.split(r", | or ", _seg):
                _p = _p.strip()
                _k = _byname.get(_p.lower()) or _byname.get(re.sub(r" \(.*\)$", "", _p).lower())
                if _k and ROWS[_n][1] not in INTO.setdefault(_k, []):
                    INTO[_k].append(ROWS[_n][1])

# Theme A "Digivice": navy, cyan edges, orange highlights
BG, PANEL, INK, MUTE, ACC, WARN = "#FF061529", "#FF0B2647", "#FFE6F7FF", "#FF6FB8D6", "#FF2BE4FF", "#FFFF8A1F"
SEL = "#FFFF8A1F"
DARK = "#FF1A0B00"
ATTRCOL = {"V": "#FF3FC1B8", "D": "#FF5B9CF5", "R": "#FFB06CF0", "F": "#FFF2A64B", "X": "#FFE8C547",
           "U": "#FF93ADB0", "N": "#FF93ADB0"}

ACTIONS, DERIVED, FLAGS, PERSIST = {}, [], {}, []
ATTRICON = {"V": "000", "D": "020", "R": "040", "F": "060", "X": "080", "U": "100", "N": "120"}


def flag(name, persist=False, value=0):
    FLAGS[name] = value
    if persist and name not in PERSIST:
        PERSIST.append(name)


def go(page):
    a = f"go_{page}"
    ACTIONS[a] = {"kind": "page", "page": page, "transition": "none"}
    return a


def toggle(f):
    a = f"t_{f}"
    ACTIONS[a] = {"kind": "flag", "flag": f}
    return a


def setf(f, v):
    a = f"s_{f}_{v}"
    ACTIONS[a] = {"kind": "flag", "flag": f, "value": v}
    return a


def dx_open(code):
    """Open the shared Digimon page for code = source * 1000 + dex number (works from a hold or a tap).
    Sets flag dxh > 0; a page bind on dx.open switches to the page. Back sets it negative."""
    a = f"dxo_{code}"
    ACTIONS[a] = {"kind": "flag", "flag": "dxh", "value": code}
    return a


def derive(d):
    DERIVED.append(d)
    return d["name"]


def derive_all(*gates):
    """A gate open while all of gates are (each may start with "!")."""
    name = "and." + ".".join(g.replace("!", "not_") for g in gates)
    if not any(x["name"] == name for x in DERIVED):
        parts = []
        for g in gates:
            if g.startswith("!"):
                g = derive({"name": f"not.{g[1:]}", "cmp": "eq", "a": g[1:], "b": 0}) if not any(
                    x["name"] == f"not.{g[1:]}" for x in DERIVED) else f"not.{g[1:]}"
            parts.append(g)
        DERIVED.append({"name": name, "all_nonzero": parts})
    return name


def eq(f, v):
    return derive({"name": f"eq.{f}.{v}", "cmp": "eq", "a": f"@flag:{f}", "b": v}) if not any(
        x["name"] == f"eq.{f}.{v}" for x in DERIVED) else f"eq.{f}.{v}"


TABLES = {}
CW = 4.2   # built-in font: canvas px per character per text_scale step (a little generous)


def wrap_lines(text, width, scale, nlines):
    maxc = max(4, int(width / (CW * scale)))
    lines, cur = [], ""
    for wd in (text or "").split():
        cand = (cur + " " + wd).strip()
        if len(cand) <= maxc:
            cur = cand
        else:
            if cur:
                lines.append(cur)
            cur = wd[:maxc]
    if cur:
        lines.append(cur)
    if len(lines) > nlines:
        lines = lines[:nlines]
        lines[-1] = lines[-1][:maxc - 3].rstrip(" ,.") + "..."
    return lines + [""] * (nlines - len(lines))


def table(name, size, entries):
    """entries: index -> text. Stored once in the manifest; value widgets show entry [bind]."""
    arr = [""] * size
    for k, v in entries.items():
        arr[k] = v
    TABLES[name] = arr
    TKEYS[name] = sorted(k for k, v in entries.items() if v)
    return name


TKEYS = {}
COVER = {}   # selector name -> (index -> gate name, all indexes)


def tval(x, y, bind, tab, scale, color, gate=None):
    """A value widget showing a table entry. Entries that are blank stay hidden (no stray numbers)."""
    w = {"type": "value", "rect": [x, y, 0, 0], "bind": bind, "table": tab, "text_scale": scale, "color": color}
    if bind in COVER and not gate:
        gfn, dom = COVER[bind]
        keys = TKEYS[tab]
        if set(keys) != set(dom):
            gate = derive({"name": f"tg.{tab}", "any_nonzero": [gfn(k) for k in keys] or [gfn(dom[0]) + ".none"]})
    if gate:
        w["need_bind"] = gate
    return w


def ttext(x, y, bind, name, size, texts, width, scale, color, nlines, line_h, gate=None):
    """Multi-line text from tables, one table per line."""
    out = []
    if PUBLIC:
        # Public build: one full-text table; public_text turns it into a wrapping module label.
        table(name, size, {k: " ".join((t or "").split()) for k, t in texts.items()})
        if TKEYS[name]:
            w = tval(x, y, bind, name, scale, color, gate)
            w["_wrap"] = [width, nlines, line_h]
            out.append(w)
        return out
    per = {k: wrap_lines(t, width, scale, nlines) for k, t in texts.items()}
    for ln in range(nlines):
        table(f"{name}{ln}", size, {k: v[ln] for k, v in per.items()})
        if not TKEYS[f"{name}{ln}"]:
            continue
        out.append(tval(x, y + ln * line_h, bind, f"{name}{ln}", scale, color, gate))
    return out


def background(src="file:ui/bg.png"):
    # The manifest background colour fills behind this full-canvas picture.
    return [{"type": "image", "rect": [0, 0, W, H], "src": src}]


def label(x, y, text, scale=T_BODY, color=INK, wrap=None, lines=None, gate=None, **kw):
    w = {"type": "label", "rect": [x, y, 0, 0], "text": text, "text_scale": scale, "color": color}
    if wrap:
        w["wrap_width"] = wrap
        if lines != 1:
            w["line_gap"] = 6
    if lines:
        w["max_lines"] = lines
    if gate:
        w["need_bind"] = gate
    w.update(kw)
    return w


def button(rect, text, act, scale=T_SMALL, bg=PANEL, color=INK, gate=None, align="center", **kw):
    w = {"type": "button", "rect": rect, "text": text, "text_scale": scale, "bg": bg, "color": color,
         "on_tap": act, "align": align}
    if gate:
        w["need_bind"] = gate
    w.update(kw)
    return w


def rect(r, color, gate=None, outline=None):
    """A filled box. Eden draws a 1 px outline in the widget's colour (default light grey) unless the
    colour is transparent: pass outline="#00000000" for an invisible cover."""
    w = {"type": "rect", "rect": r, "bg": color, "frame": 0}
    if outline:
        w["color"] = outline
    if gate:
        w["need_bind"] = gate
    return w


def box(fl, x, y, size, gate=None, on_gate=None):
    """Native drawn check box. gate = visibility, on_gate = ticked AND visible."""
    p = 6 if size < 60 else 8
    ws = [rect([x, y, size, size], MUTE, gate), rect([x + 4, y + 4, size - 8, size - 8], PANEL, gate),
          rect([x + p + 4, y + p + 4, size - 2 * p - 8, size - 2 * p - 8], WARN, on_gate or f"@flag:{fl}")]
    return ws


def pager(key, npages, x, y, w, h, scale=T_SMALL):
    """Row of numbered page buttons. Gate key_<p> is 1 on page p."""
    flag(f"p.{key}")
    out = []
    cw = w // npages
    for p in range(npages):
        g = derive({"name": f"{key}_{p}", "cmp": "eq", "a": f"@flag:p.{key}", "b": p})
        out.append(rect([x + p * cw - 2, y - 2, cw, h + 4], SEL, g))
        out.append(button([x + p * cw + 3, y + 3, cw - 10, h - 6], str(p + 1), setf(f"p.{key}", p), scale, border=2))
    return out


TABS = [("Now", "now"), ("Quests", "missions"), ("Digimon dex", "dex"), ("Type chart", "types"), ("Tips", "tips")]


TAB_Y = 1004          # tabs along the bottom edge (0.32.0)
BAR_Y, BAR_H = 10, 60  # the location bar along the top
BAR_DET = 500          # width of the bar's right-hand details (counts, LIVE, GUIDE >)


def tabs(active):
    """Tab row (bottom) and the location bar (top). Pages that use it are re-spaced by bottom_tabs()."""
    out = []
    tw = (W - 28) // len(TABS)
    for i, (lab, page) in enumerate(TABS):
        on = i == active
        b = button([14 + i * tw, TAB_Y, tw - 8, 70], lab, go(page), T_SMALL, SEL if on else PANEL,
                   DARK if on else INK)
        b["_fixed"] = 1
        out.append(b)
    for wd in location_bar():
        wd["_fixed"] = 1
        out.append(wd)
    return out


def location_bar():
    """Top bar: where you are, what is here, LIVE. Tapping it opens this area's guide page."""
    y = BAR_Y
    det = W - 14 - BAR_DET + 14
    nar = len(area_groups())
    return [{"type": "image", "rect": [14, y, W - 28, BAR_H], "src": "file:ui/locbar.png"},
            tval(94, y + 16, "here.idx", "herename", T_BODY, INK, "here.match"),
            label(94, y + 16, "Not on the field right now", T_BODY, MUTE, gate="here.notfield"),
            label(94, y + 16, "This spot isn't in the guide", T_BODY, MUTE, gate="here.unknown"),
            {"type": "label", "rect": [det, y + 21, 0, 0], "bind_text": "dsts.arc{i}", "text_scale": T_SMALL,
             "color": ACC, "need_bind": "here.a{i}", "repeat": nar, "repeat_dx": 0, "repeat_dy": 0},
            label(det, y + 21, "Nothing to find here", T_SMALL, MUTE, gate="here.noarea"),
            {"type": "image", "rect": [W - 214, y + 17, 26, 26], "src": "file:ui/live_dot.png"},
            label(W - 184, y + 21, "LIVE", 2, ACC),
            label(W - 112, y + 21, "GUIDE", 2, ACC),
            label(W - 56, y + 12, ">", 5, ACC),
            button([14, y, W - 28, BAR_H], "", "go_ar{i}", gate="here.a{i}", bg="#00000000", border=0,
                   repeat=nar, repeat_dx=0, repeat_dy=0),
            button([14, y, W - 28, BAR_H], "", go("areas"), gate="here.nohas", bg="#00000000", border=0)]


# Pages with tabs were laid out for content between y 112 and 1068. They now get 84 .. 994.
CT_TOP, CT_BOT = 84, 994
CT_K = (CT_BOT - CT_TOP) / (1068 - 112)


def ct_y(y):
    return int(round(CT_TOP + (y - 112) * CT_K))


def bottom_tabs(pages):
    """Re-space every tab page into the room between the location bar and the bottom tabs: boxes keep their
    edges in proportion, text and small pictures keep their size and move with their spot."""
    for p in pages:
        ws = p["widgets"]
        if not any(w.get("_fixed") for w in ws):
            continue
        for w in ws:
            if w.pop("_fixed", None):
                continue
            r = w["rect"]
            if r[1] < 100 or (r[0] == 0 and r[1] == 0 and w["type"] == "image"):
                continue
            if w["type"] in ("rect", "button") or (w["type"] == "image" and r[3] > 200):
                y0, y1 = ct_y(r[1]), ct_y(r[1] + r[3])
                r[1], r[3] = y0, max(1, y1 - y0)
            elif w["type"] == "image":
                r[1] = int(round(ct_y(r[1] + r[3] / 2) - r[3] / 2))
            else:
                r[1] = ct_y(r[1])
        for sc in p.get("scrolls", []):
            r = sc["rect"]
            y0, y1 = ct_y(r[1]), ct_y(r[1] + r[3])
            r[1], r[3] = y0, y1 - y0
    for p in pages:
        for w in p["widgets"]:
            w.pop("_fixed", None)


def back(page, text="< BACK"):
    return button([14, 14, 240, 86], text, go(page), T_SMALL)


# ---------------------------------------------------------------- quest lists
QROWS = 7


def quest_list(pid, title, tab, entries, done_flag, open_page, header):
    """entries: list of (index, title). Rows open a detail page; check box shows done."""
    w = background() + tabs(tab) + header
    key = f"{pid}pg"
    n = (len(entries) + QROWS - 1) // QROWS
    for k, (i, name) in enumerate(entries):
        pg, r = divmod(k, QROWS)
        y = 186 + r * 114
        g = f"{key}_{pg}"
        on = derive({"name": f"{pid}on{i}", "all_nonzero": [f"@flag:{done_flag(i)}", g]})
        w.append(button([14, y, W - 28, 106], "", go(open_page(i)), gate=g))
        w.append(label(40, y + 34, f"{k + 1}.", T_SMALL, MUTE, gate=g))
        w.append(label(110, y + 30, name, T_ROW, INK, wrap=980, lines=1, gate=g))
        w.extend(box(done_flag(i), W - 104, y + 23, 60, g, on))
    w.extend(pager(key, n, 14, 994, W - 28, 72))
    return {"id": pid, "title": title, "widgets": w}


def view_buttons(vf, labels, x0=14, y=114):
    """Sub-view switcher. Returns widgets; gates are eq.<vf>.<n>."""
    out = []
    for n, lab in enumerate(labels):
        g = eq(vf, n)
        x = x0 + n * 310
        out.append(rect([x - 4, y - 4, 308, 84], SEL, g))
        out.append(button([x, y, 300, 76], lab, setf(vf, n), T_SMALL, border=2))
    return out


BOSSES = _GDATA["bosscards"]["BOSSES"]
ELEMIDX = {"Null": 0, "Fire": 1, "Ice": 2, "Plant": 3, "Water": 4, "Electricity": 5, "Steel": 6, "Wind": 7,
           "Earth": 8, "Light": 9, "Dark": 10}
ATTRBYNAME = {v: k for k, v in ATTR.items()}
_BYNAME = None


def boss_pic(name):
    global _BYNAME
    if _BYNAME is None:
        _BYNAME = {r[1]: n for n, r in ROWS.items()}
    def ok(n):
        return n is not None and ((PUBLIC and n in G2S) or (not PUBLIC and n in HASPIC))
    if not name:
        return None
    for cand in (name, re.sub(r" \(.*\)$", "", name), name.split(" ")[0]):
        n = _BYNAME.get(cand)
        if ok(n):
            return f"file:rnd/{n}.png"
    return None


def weak_icon(e):
    if e in ELEMIDX:
        return f"file:gi/elem_{ELEMIDX[e]:02d}.png"
    return f"file:gi/attr_{ATTRICON[ATTRBYNAME[e]]}.png"


def boss_cards(cards, gate, top=214, bottom=960):
    """The BOSSES view as cards: round portrait, name, weak-to icons, tips, good picks. 1-3 bosses stack full
    width; 4 or more go in a two-column grid."""
    out = []
    n = len(cards)
    cols = 1 if n <= 3 else 2
    rows = (n + cols - 1) // cols
    gap = 14 if cols == 1 else 12
    avail = bottom - top
    if n == 1:
        c0 = cards[0]
        hh = 230 + 46 * len(c0["tips"]) + 20
        heights = [min(avail, max(300, hh))]
    else:
        heights = [(avail - (rows - 1) * gap) // rows] * rows
    cw = (W - 28 - (cols - 1) * 12) // cols
    for k, c in enumerate(cards):
        r, col = divmod(k, cols)
        x = 14 + col * (cw + 12)
        y = top + sum(heights[:r]) + r * gap
        h = heights[r]
        compact = cols == 2 or h < 260
        pic = min(150, h - 40) if not compact else min(104, h - 34)
        out.append(rect([x, y, cw, h], PANEL, gate, outline="#FF1D4D73"))
        out.append(rect([x, y, 8, h], WARN, gate))
        src = boss_pic(c["pic"])
        py = y + (24 if not compact else (h - pic) // 2 if h < 200 else 18)
        out.append({"type": "image", "rect": [x + 26, py, pic, pic], "src": src or "file:ui/boss_unknown.png",
                    "need_bind": gate})
        tx = x + 26 + pic + 24
        ty = y + (18 if not compact else 12)
        if c.get("tag"):
            out.append(label(tx, ty, c["tag"], 2, WARN, gate=gate))
            ty += 24
        out.append(label(tx, ty, c["name"], 5 if not compact else 4, INK, gate=gate))
        ty += 60 if not compact else 42
        if c["weak"]:
            isz = 44 if not compact else 34
            if not compact:
                out.append(label(tx, ty, "WEAK TO", 2, MUTE, gate=gate))
                ty += 22
            ix = tx
            for e in c["weak"]:
                out.append({"type": "image", "rect": [ix, ty, isz, isz], "src": weak_icon(e), "need_bind": gate})
                if not compact:
                    out.append(label(ix + isz // 2, ty + isz + 6, "Elec" if e == "Electricity" else e, 2, INK,
                                     align="center", gate=gate))
                ix += isz + (30 if not compact else 8)
            ty += isz + (36 if not compact else 10)
        # tips: under the portrait row on big cards, beside it on compact ones
        tips_x = x + 36 if not compact else tx
        tips_y = max(ty, y + 26 + pic + 14) if not compact else ty
        width = cw - (tips_x - x) - 30
        for t in c["tips"]:
            if tips_y > y + h - 30:
                break
            nl = 2 if not compact else 2
            out.append(rect([tips_x, tips_y + 9, 10, 10], ACC, gate))
            out.append(label(tips_x + 24, tips_y, t, T_SMALL, INK, wrap=width - 24, lines=nl, line_gap=6, gate=gate))
            est = len(t) * 12.6
            tips_y += (1 if est < width - 24 else 2) * 24 + (14 if not compact else 8)
        if c["picks"] and not compact:
            out.append(label(x + cw - 340, y + 20, "GOOD PICKS", 2, ACC, gate=gate))
            for j, pk in enumerate(c["picks"][:3]):
                psrc = boss_pic(pk)
                px = x + cw - 340 + j * 108
                if psrc:
                    out.append({"type": "image", "rect": [px, y + 46, 88, 88], "src": psrc, "need_bind": gate})
                out.append(label(px + 44, y + 140, pk, 2, INK, align="center", gate=gate))
    return out


def main_page(i):
    title, steps, boss = MAIN[i]
    done, spf, vf = f"m.done_{i}", f"sp.{i}", f"mv.{i}"
    flag(spf, True); flag(vf)
    show = derive({"name": f"msh{i}", "any_nonzero": ["@flag:sp.all", f"@flag:{spf}"]})
    hide = derive({"name": f"mhd{i}", "cmp": "eq", "a": show, "b": 0})
    only = derive({"name": f"mon{i}", "all_nonzero": [f"@flag:{spf}", "sp.notall"]})
    st = derive({"name": f"mst{i}", "all_nonzero": [show, eq(vf, 0)]})
    bs = derive({"name": f"mbs{i}", "all_nonzero": [show, eq(vf, 1)]})
    w = background() + [back("missions")]
    w.append(label(280, 22, title, T_TITLE, INK, wrap=940, lines=2))
    w += view_buttons(vf, ["STEPS", "BOSSES"])
    w.append(button([640, 114, 586, 76], "SHOW STORY", toggle(spf), T_SMALL, WARN, DARK, gate=hide))
    w.append(button([640, 114, 586, 76], "HIDE STORY", toggle(spf), T_SMALL, gate=only))
    w.append(label(660, 138, "Spoilers on for all", T_SMALL, MUTE, gate="@flag:sp.all"))
    # spoiler cover
    w.append(rect([14, 214, W - 28, 300], PANEL, hide))
    w.append(label(44, 250, "STORY SPOILERS HIDDEN", T_TITLE, WARN, gate=hide))
    w.append(label(44, 330, "Tap SHOW STORY above to see the steps and bosses for this mission.", T_BODY, INK,
                   wrap=1150, lines=3, gate=hide))
    w.append(label(30, 216, steps, T_BODY, INK, wrap=1180, lines=19, gate=st))
    if i + 1 in BOSSES:
        w += boss_cards(BOSSES[i + 1], bs)
    else:
        w.append(label(30, 216, boss, T_BODY, WARN, wrap=1180, lines=19, gate=bs))
    w.append(button([14, 976, W - 28, 92], "MARK DONE", toggle(done), T_BODY, align="left", text_inset=30))
    w.extend(box(done, W - 110, 990, 64))
    return {"id": f"qm{i}", "title": title, "widgets": w}


def side_page(entry):
    sid, title, unlock, steps, boss, reward, q = entry
    done, vf = f"s.done_{sid}", f"sv.{sid}"
    flag(done, True); flag(vf)
    w = background() + [back(f"sqm{SIDE_OF[sid]}" if sid in SIDE_OF else "missions")]
    w.append(label(280, 22, title, T_TITLE, INK, wrap=940, lines=2))
    w += view_buttons(vf, ["STEPS", "INFO"])
    g0, g1 = eq(vf, 0), eq(vf, 1)
    req = QOUT.get(q) if q else None
    y = 216
    if req:
        w.append(label(30, y, "THE REQUEST", T_SMALL, MUTE, gate=g0))
        w.append(label(30, y + 36, req, T_SMALL, ACC, wrap=1180, lines=4, gate=g0))
        y += 170
    w.append(label(30, y, steps, T_BODY, INK, wrap=1180, lines=(19 if y == 216 else 15), gate=g0))
    client = QCLIENT.get(q) if q else None
    w.append(label(30, 214, "CLIENT", T_SMALL, MUTE, gate=g1))
    w.append(label(30, 250, client or "Not added yet.", T_BODY, ACC, wrap=1180, lines=1, gate=g1))
    w.append(label(30, 320, "WHEN IT UNLOCKS", T_SMALL, MUTE, gate=g1))
    w.append(label(30, 356, unlock, T_BODY, INK, wrap=1180, lines=3, gate=g1))
    w.append(label(30, 500, "BOSS", T_SMALL, MUTE, gate=g1))
    w.append(label(30, 536, boss, T_BODY, WARN, wrap=1180, lines=5, gate=g1))
    w.append(label(30, 730, "REWARD", T_SMALL, MUTE, gate=g1))
    DATA["quests"][f"qs{sid}"] = [q or 0]
    w.append(blabel(30, 766, "dsts.rw0", T_BODY, ACC, wrap=1180, lines=4, gate=derive_all(g1, "dsts.rwok0")))
    w.append(label(30, 766, "Not listed in the game's data.", T_BODY, MUTE, gate=derive_all(g1, "!dsts.rwok0")))
    w.append(button([14, 976, W - 28, 92], "MARK DONE", toggle(done), T_BODY, align="left", text_inset=30))
    w.extend(box(done, W - 110, 990, 64))
    return {"id": f"qs{sid}", "title": title, "widgets": w}


def mission_state():
    """Current mission = first one not ticked done. Gates: mdn{i} done, mcur{i} current, mfut{i} later."""
    n = len(MAIN)
    for i in range(n):
        flag(f"m.done_{i}", True)
        derive({"name": f"mnd{i}", "cmp": "eq", "a": f"@flag:m.done_{i}", "b": 0})
        derive({"name": f"mcur{i}", "all_nonzero": [f"@flag:m.done_{j}" for j in range(i)] + [f"mnd{i}"]})
        derive({"name": f"mnc{i}", "cmp": "eq", "a": f"mcur{i}", "b": 0})
        derive({"name": f"mfut{i}", "all_nonzero": [f"mnd{i}", f"mnc{i}"]})
        derive({"name": f"mshow{i}", "any_nonzero": [f"@flag:m.done_{i}", f"mcur{i}", "@flag:sp.all"]})
        derive({"name": f"mhide{i}", "cmp": "eq", "a": f"mshow{i}", "b": 0})
    derive({"name": "m.count", "terms": [[f"@flag:m.done_{i}", 1] for i in range(n)]})
    derive({"name": "m.alldone", "cmp": "eq", "a": "m.count", "b": n})


def current_mission_card(x, y, w, h, tap=True):
    """Shows the current mission (first one not ticked) with a tap-through to its page."""
    out = [rect([x, y, w, h], PANEL), rect([x, y, 8, h], WARN),
           label(x + 30, y + 16, "CURRENT MISSION", T_SMALL, WARN)]
    for i, (t, _, _) in enumerate(MAIN):
        if tap:
            out.append(button([x + 8, y, w - 8, h], "", go(f"qm{i}"), gate=f"mcur{i}", bg="#00000000", border=0))
        out.append(label(x + 30, y + 56, f"{i + 1}   {t}", T_BODY if len(t) < 34 else T_SMALL, INK,
                         wrap=w - 60, lines=1, gate=f"mcur{i}"))
    out.append(label(x + 30, y + 56, "All main missions ticked. Well done!", T_BODY, ACC, gate="m.alldone"))
    out.append({"type": "value", "rect": [x + w - 30, y + 18, 0, 0], "bind": "m.count", "text_scale": T_SMALL,
                "color": MUTE, "align": "right", "suffix": f"  of {len(MAIN)} done"})
    return out


MROWS = 6


def side_groups():
    """Side quests grouped under the main mission during which they open (from their unlock note)."""
    titles = [m[0] for m in MAIN]
    groups, other = {}, []
    for x in SIDE:
        best = None
        for i, t in enumerate(titles):
            k = x[2].find(t)
            if k >= 0 and (best is None or k < best[0]):
                best = (k, i)
        if best:
            groups.setdefault(best[1], []).append(x)
        else:
            other.append(x)
    return groups, other


QL_TOP = 300      # content origin of the quest list (region rect starts 4 px above)
QL_ROW = 82       # mission row pitch
QL_SIDE = 62      # side quest row pitch


def missions_list():
    """Quests: every main mission in one scrolling list. Finished missions fold into one row; the
    side quests that open during a mission expand under it. Rows below an expanded or folded part
    move with y_bind offsets (derived sums), so the list never leaves gaps."""
    flag("sp.all", True)
    derive({"name": "sp.notall", "cmp": "eq", "a": "@flag:sp.all", "b": 0})
    flag("ql.done")             # 1 = show finished missions
    flag("ql.side", True, 1)    # 1 = side quests shown
    flag("ql.x", False, -1)     # expanded mission: -1 = the current one, -2 = none, else its index
    flag("tc.never")            # always 0: an all_nonzero with it is never true
    sc = "ql"
    groups, other = side_groups()
    n = len(MAIN)
    w = background() + tabs(1)
    w += current_mission_card(14, 114, W - 28, 112)
    w.append(label(24, 252, "Spoilers", T_SMALL, MUTE))
    w.append(button([180, 236, 420, 50], "HIDDEN  (tap to show all)", toggle("sp.all"), 2, gate="sp.notall"))
    w.append(button([180, 236, 420, 50], "SHOWN  (tap to hide all)", toggle("sp.all"), 2, WARN, DARK,
                    gate="@flag:sp.all"))
    derive({"name": "ql.sideoff", "cmp": "eq", "a": "@flag:ql.side", "b": 0})
    w.append(button([826, 236, 400, 50], "SIDE QUESTS: SHOWN", toggle("ql.side"), 2, gate="@flag:ql.side"))
    w.append(button([826, 236, 400, 50], "SIDE QUESTS: HIDDEN", toggle("ql.side"), 2, gate="ql.sideoff"))

    def sw(wd, yb):   # put a widget in the scrolling list, offset by derived value yb
        wd["scroll"] = sc
        wd["repeat"] = 1
        wd["repeat_dy"] = 0
        if yb:
            wd["y_bind"] = yb
        return wd

    # finished missions fold into one row
    derive({"name": "m.any", "cmp": "gt", "a": "m.count", "b": 0})
    derive({"name": "ql.hidedone", "cmp": "eq", "a": "@flag:ql.done", "b": 0})
    g_fold = derive({"name": "ql.fold", "all_nonzero": ["m.any", "ql.hidedone"]})
    g_open = derive({"name": "ql.unfold", "all_nonzero": ["m.any", "@flag:ql.done"]})
    derive({"name": "ql.s", "terms": [["m.any", 72]]})
    y = QL_TOP
    w.append(sw(button([14, y, W - 28, 64], "", toggle("ql.done"), gate="m.any"), None))
    w.append(sw({"type": "value", "rect": [44, y + 24, 0, 0], "bind": "m.count", "text_scale": T_SMALL, "color": MUTE,
                 "need_bind": "m.any"}, None))
    w.append(sw(label(110, y + 24, "MISSIONS DONE", T_SMALL, MUTE, gate="m.any"), None))
    w.append(sw(label(W - 40, y + 26, "TAP TO SHOW  >", 2, ACC, align="right", gate=g_fold), None))
    w.append(sw(label(W - 40, y + 26, "TAP TO FOLD  ^", 2, ACC, align="right", gate=g_open), None))

    ys, ex_h = [], []
    for i in range(n):
        sq = groups.get(i, [])
        vis = derive({"name": f"qv{i}", "any_nonzero": [f"@flag:ql.done", f"mnd{i}"]})
        # this mission's side quests are open
        derive({"name": f"qxe{i}", "cmp": "eq", "a": "@flag:ql.x", "b": i})
        derive({"name": f"qxa{i}", "all_nonzero": [f"ql.xauto", f"mcur{i}"]})
        derive({"name": f"qxo{i}", "any_nonzero": [f"qxe{i}", f"qxa{i}"]})
        ex = derive({"name": f"qex{i}", "all_nonzero": [vis, "@flag:tc.never"]})   # nothing expands now
        cl = derive({"name": f"qcl{i}", "all_nonzero": [vis, "@flag:ql.side", f"mshow{i}"]
                     + ([] if sq else ["@flag:tc.never"])})
        yb = derive({"name": f"qy{i}", "terms": [["ql.s", 1]] + [t for j in range(i) for t in
                     ([[f"qv{j}", QL_ROW]] + ([[f"qex{j}", QL_SIDE * len(groups[j]) + 4]] if groups.get(j) else []))]})
        ys.append(yb)
        y = QL_TOP
        gc = derive({"name": f"qlc{i}", "all_nonzero": [vis, f"mcur{i}"]})
        gd = derive({"name": f"qld{i}", "all_nonzero": [vis, f"@flag:m.done_{i}"]})
        gf = derive({"name": f"qlf{i}", "all_nonzero": [vis, f"mfut{i}"]})
        gs = derive({"name": f"qls{i}", "all_nonzero": [vis, f"mshow{i}"]})
        gh = derive({"name": f"qlh{i}", "all_nonzero": [vis, f"mhide{i}"]})
        w.append(sw(button([14, y, W - 28, 74], "", go(f"qm{i}"), gate=vis), yb))
        w.append(sw(rect([14, y + 4, 8, 66], WARN, gc), yb))
        for col, gg in ((MUTE, gd), (WARN, gc), (MUTE, gf)):
            w.append(sw(label(44, y + 28, str(i + 1), T_BODY, col, gate=gg), yb))
        w.append(sw(label(110, y + 28, MAIN[i][0], T_BODY, INK, wrap=640, lines=1, gate=gs), yb))
        w.append(sw(label(110, y + 28, "- - - - -", T_BODY, MUTE, gate=gh), yb))
        w.append(sw(label(300, y + 32, "Hidden until you get there", 2, MUTE, gate=gh), yb))
        if sq:   # this mission's side quests have their own page (0.32.1)
            w.append(sw(button([880, y + 14, 190, 46], f"{len(sq)} SIDE  >", go(f"sqm{i}"), 2, gate=cl), yb))
        w.extend(sw(b_, yb) for b_ in box(f"m.done_{i}", W - 100, y + 14, 46, vis, gd))
    derive({"name": "ql.xauto", "cmp": "eq", "a": "@flag:ql.x", "b": -1})
    # side quests that do not open during a particular mission
    last = n - 1
    yend = derive({"name": "qy.end", "terms": [[ys[last], 1], [f"qv{last}", QL_ROW]]
                   + ([[f"qex{last}", QL_SIDE * len(groups[last]) + 4]] if groups.get(last) else [])})
    if other:
        y = QL_TOP + 8
        w.append(sw(label(24, y + 8, "OTHER SIDE QUESTS", T_SMALL, MUTE, gate="@flag:ql.side"), yend))
        for r, x in enumerate(other):
            sid, title, unlock = x[0], x[1], x[2]
            yy = QL_TOP + 50 + r * QL_SIDE
            on = derive({"name": f"qso{sid}", "all_nonzero": ["@flag:ql.side", f"@flag:s.done_{sid}"]})
            w.append(sw(button([14, yy, W - 28, 56], "", go(f"qs{sid}"), bg="#FF08203C", border=0,
                               gate="@flag:ql.side"), yend))
            w.append(sw(rect([14, yy, 4, 56], ACC, "@flag:ql.side"), yend))
            w.append(sw(label(40, yy + 10, title, T_SMALL, INK, wrap=1000, lines=1, gate="@flag:ql.side"), yend))
            w.append(sw(label(40, yy + 34, unlock, 2, MUTE, wrap=1060, lines=1, gate="@flag:ql.side"), yend))
            w.extend(sw(b_, yend) for b_ in box(f"s.done_{sid}", W - 94, yy + 12, 32, "@flag:ql.side", on))
    derive({"name": "ql.h", "terms": [["qy.end", 1], ["@flag:ql.side", (58 + QL_SIDE * len(other)) if other else 0]],
            "add": 24})
    page = {"id": "missions", "title": "Quests", "widgets": w,
            "scrolls": [{"id": sc, "rect": [0, QL_TOP - 4, W, H - QL_TOP + 4], "row_h": 1, "count_bind": "ql.h",
                         "bar": ACC, "bar_track": "#331D4D73", "bar_w": 6}]}
    return page


SIDE_OF = {}   # side quest id -> its mission index (for Back)


def side_mission_page(i, sq):
    """0.32.1: one mission's side quests as cards (title, when it opens, fight, reward, done box).
    Cards are fixed in place inside one scroll region, so a long list scrolls."""
    sc = f"sqm{i}"
    w = background() + [back("missions")]
    w.append(label(280, 22, "SIDE QUESTS", 2, ACC))
    w.append(label(280, 46, f"{i + 1}   {MAIN[i][0]}", T_TITLE, INK, wrap=930, lines=1))
    n = len(sq)
    w.append(label(24, 124, f"{n} side quest{'s' if n != 1 else ''} open during this mission. Tap one for its steps.",
                   T_SMALL, MUTE))
    top = 170
    y = top

    def sw(wd):
        wd["scroll"] = sc
        wd["repeat"] = 1
        wd["repeat_dy"] = 0
        return wd
    DATA["quests"][sc] = [x[6] or 0 for x in sq]
    for k, x in enumerate(sq):
        sid, title, unlock, steps, boss, reward = x[0], x[1], x[2], x[3], x[4], x[5]
        SIDE_OF[sid] = i
        fight = not boss.startswith(("No boss", "Not added"))
        ex = 26 if len(unlock) * 12.6 > 930 else 0   # a second "when it opens" line pushes the rest down
        h = (236 if fight else 176) + ex
        on = derive({"name": f"sqd{sid}", "terms": [[f"@flag:s.done_{sid}", 1]]})
        w.append(sw(button([14, y, W - 28, h], "", go(f"qs{sid}"))))
        w.append(sw(rect([14, y, 8, h], ACC)))
        w.append(sw(label(44, y + 18, str(k + 1), T_BODY, MUTE)))
        w.append(sw(label(96, y + 16, title, T_TITLE, INK, wrap=930, lines=1)))
        w.append(sw(label(96, y + 70, "WHEN IT OPENS", 2, MUTE)))
        w.append(sw(label(96, y + 92, unlock, T_SMALL, INK, wrap=930, lines=2, line_gap=6)))
        if fight:
            w.append(sw(label(96, y + 140 + ex, "FIGHT", 2, WARN)))
            w.append(sw(label(96, y + 162 + ex, boss, T_SMALL, WARN, wrap=930, lines=1)))
        w.append(sw(label(96, y + h - 40, "REWARD", 2, ACC)))
        w.append(sw(blabel(200, y + h - 40, f"dsts.rw{k}", 2, MUTE, wrap=830, lines=1)))
        w.append(sw(label(200, y + h - 40, "Not listed in the game's data.", 2, MUTE, gate=f"!dsts.rwok{k}")))
        w.append(sw(button([W - 128, y + 18, 100, 104], "", toggle(f"s.done_{sid}"), bg="#00000000", border=0)))
        w.extend(sw(b_) for b_ in box(f"s.done_{sid}", W - 110, y + 30, 64, None, on))
        w.append(sw(label(W - 118, y + 104, "DONE", 2, MUTE)))
        w.append(sw(label(W - 60, y + h - 46, ">", 5, ACC)))
        y += h + 14
    hgt = y - top + 20
    derive({"name": f"sqh{i}", "terms": [["@flag:tc.never", 0]], "add": hgt})
    return {"id": sc, "title": f"Side quests: {MAIN[i][0]}", "widgets": w,
            "scrolls": [{"id": sc, "rect": [0, top - 4, W, H - top + 4], "row_h": 1, "count_bind": f"sqh{i}",
                         "bar": ACC, "bar_track": "#331D4D73", "bar_w": 6}]}


TIPRANGES = []   # (part index, first mission, last mission) for "Missions a to b" parts


S2G = {}
for _g, _st in sorted(G2S.items()):
    S2G.setdefault(_st, _g)
PARTY_PIC = 112


def party_strip(x0, y0, pw, ph):
    """Your party: 3 battle + 3 reserve, read live (slot order as the game's party menu)."""
    w = [rect([x0, y0, pw, ph], PANEL)]
    w.append(label(x0 + 26, y0 + 12, "YOUR PARTY", T_SMALL, MUTE))
    anyp = derive({"name": "party.any", "any_nonzero": [f"pf{i}" for i in range(6)]})
    nonep = derive({"name": "party.none", "cmp": "eq", "a": anyp, "b": 0})
    w.append(label(x0 + pw - 26, y0 + 12, "LIVE", 2, WARN, align="right", gate=anyp))
    w.append(label(x0 + pw // 2, y0 + ph // 2 - 10, "Load your save to see your party here.", T_SMALL, MUTE,
                   align="center", gate=nonep))
    cw, gap = 186, 8
    mid = 24
    tops = y0 + 58
    w.append(label(x0 + 16, y0 + 38, "BATTLE", 2, ACC, gate=anyp))
    w.append(label(x0 + 16 + 3 * (cw + gap) + mid, y0 + 38, "RESERVE", 2, MUTE, gate=anyp))
    smax = max(S2G) + 1
    names = {st: ROWS[g][1] for st, g in S2G.items() if g in ROWS}
    for i in range(6):
        cx = x0 + 16 + i * (cw + gap) + (mid if i >= 3 else 0)
        used = derive({"name": f"pu{i}", "cmp": "ne", "a": f"pf{i}", "b": 0})
        empty = derive({"name": f"pe{i}", "all_nonzero": [anyp, derive({"name": f"pz{i}", "cmp": "eq", "a": f"pf{i}", "b": 0})]})
        derive({"name": f"plv{i}", "terms": [[f"plb{i}", 1]]})
        res = i >= 3
        w.append(rect([cx, tops, cw, ph - 70], "#FF081D38", anyp, outline="#FF1D4D73"))
        w.append(rect([cx, tops, cw, 5], MUTE if res else ACC, anyp))
        w.append(label(cx + cw // 2, tops + 80, "Empty", T_SMALL, "#FF2E5A78", align="center", gate=empty))
        src = ("module:dsts:round_ui_chara_icon_%d" if PUBLIC else "file:rst/%d.png")
        img = {"type": "image", "rect": [cx + (cw - PARTY_PIC) // 2, tops + 12, PARTY_PIC, PARTY_PIC],
               "bind": f"ps{i}", "src_format": src, "need_bind": used}
        if PUBLIC:
            img["add"] = 1000
        if res:
            img["tint"] = "#B0FFFFFF"
        w.append(img)
        # name: one template over every status id, shown only where the slot's id equals its index (no
        # per-name gates; the public build's module fills the names in as one row set)
        nm = INK if not res else "#FFAFC6D6"
        table("pnm", smax, names)
        CI_MAX[0] = max(CI_MAX[0], smax)
        w.append({"type": "value", "rect": [cx + cw // 2, tops + 132, 0, 0], "bind": "ci.{i}", "table": "pnm",
                  "text_scale": T_SMALL, "color": nm, "align": "center", "need_bind": used,
                  "hide_bind": f"ps{i}", "keep_min": "{i}", "keep_max": "{i}",
                  "repeat": smax, "repeat_dx": 0, "repeat_dy": 0})
        if SHOW_LEVEL:
            lc = ACC if not res else MUTE
            w.append(label(cx + cw // 2 - 4, tops + 164, "LV", 2, lc, align="right", gate=used))
            w.append({"type": "value", "rect": [cx + cw // 2 + 6, tops + 164, 0, 0], "bind": f"plv{i}",
                      "text_scale": 2, "color": lc, "need_bind": used})
    return w


def now_page():
    """Home page: where you are, your mission and its boss, tips for this part."""
    for k, (t, _) in enumerate(TIP_PARTS):
        m = re.match(r"Missions (\d+) to (\d+)", t)
        if m:
            TIPRANGES.append((k, int(m.group(1)), int(m.group(2))))
    w = background()
    # this area (the location bar above names the place)
    w.append(rect([14, 114, W - 28, 150], PANEL))
    w.append(rect([14, 114, 8, 150], WARN))
    w.append(label(44, 128, "THIS AREA", T_SMALL, WARN))
    ag = area_groups()
    w.append(label(44, 176, "Chests, wild Digimon and items for where you are.", T_SMALL, MUTE, gate="here.hasarea"))
    w.append(label(44, 176, "Nothing listed for this spot.", T_SMALL, MUTE, gate="here.noarea"))
    w.append(button([700, 178, 300, 70], "OPEN AREA GUIDE  >", "go_ar{i}", T_SMALL, gate="here.a{i}",
                    repeat=len(ag), repeat_dx=0, repeat_dy=0))
    w.append(button([700, 178, 300, 70], "AREA GUIDE  >", go("areas"), T_SMALL, gate="here.nohas"))
    w += auto_toggle(1012, 178, 200, 70)[:1] + [label(1036, 190, "AUTO-OPEN", 2, MUTE),
                                                label(1036, 216, "ON", T_SMALL, WARN, gate="@flag:auto.here"),
                                                label(1036, 216, "OFF", T_SMALL, MUTE, gate="here.autooff")]
    w += party_strip(14, 276, W - 28, 262)
    # mission (its boss is on the mission's own page, BOSSES view)
    w += current_mission_card(14, 552, W - 28, 112)
    # tips for this part of the story
    w.append(rect([14, 678, W - 28, 390], PANEL))
    w.append(label(44, 694, "TIPS FOR THIS PART OF THE STORY", T_SMALL, MUTE))
    for k, lo, hi in TIPRANGES:
        g = derive({"name": f"nowtp{k}", "any_nonzero": [f"mcur{i - 1}" for i in range(lo, hi + 1) if i - 1 < len(MAIN)]})
        _, tips = TIP_PARTS[k]
        w.append(label(860, 694, f"Missions {lo} to {hi}", T_SMALL, ACC, gate=g))
        for r, (tt, body) in enumerate(tips[:2]):
            y = 742 + r * 108
            w.append(rect([44, y + 8, 18, 18], WARN, g))
            w.append(label(80, y, tt, T_BODY, INK, wrap=1110, lines=1, gate=g))
            w.append(label(80, y + 42, body, T_SMALL, MUTE, wrap=1110, lines=2, line_gap=4, gate=g))
        w.append(button([W - 14 - 330, 990, 300, 64], "ALL TIPS  >", go(f"tp{k}"), T_SMALL, gate=g))
    w += tabs(0)
    return {"id": "now", "title": "Now", "widgets": w}


def side_list():
    hdr = [label(24, 132, f"{len(SIDE)} side missions, in story order"
                 + (f" ({len(SIDE_LEFT)} DLC one not added)" if SIDE_LEFT else ""), T_SMALL, MUTE)]
    return quest_list("side", "Side quests", 2, [(s[0], s[1]) for s in SIDE],
                      lambda i: f"s.done_{i}", lambda i: f"qs{i}", hdr)


# ---------------------------------------------------------------- dex (unchanged layout)
def dex_hub():
    """Ranks (two columns), then find by first letter and by type: each opens one scrolling list."""
    w = background() + tabs(2)
    w.append(label(30, 124, "Recorded", T_SMALL, MUTE))
    w.append({"type": "value", "rect": [260, 124, 0, 0], "bind": "dex.count", "max_bind": "dex.total",
              "max_sep": " / ", "text_scale": T_SMALL, "color": INK})
    for k in range(NRANK):
        title = DEXGROUPS[k][0]
        x, y = 14 + (k % 2) * 616, 168 + (k // 2) * 96
        w.append(button([x, y, 602, 86], "", go(f"dexg{k}")))
        w.append(label(x + 30, y + 26, title.upper(), T_BODY, INK))
        w.append({"type": "value", "rect": [x + 440, y + 32, 0, 0], "bind": f"dg{k}.count",
                  "max_bind": f"dg{k}.total", "max_sep": " / ", "text_scale": T_SMALL, "color": ACC})
    yy = 168 + ((NRANK + 1) // 2) * 96 + 14
    w.append(label(30, yy, "FIND BY NAME", T_SMALL, MUTE))
    w.append(label(300, yy + 4, "Tap a letter", 2, MUTE))
    per = 13
    for i, L_ in enumerate(LETTERS):
        x, y2 = 14 + (i % per) * 94, yy + 40 + (i // per) * 92
        w.append(button([x, y2, 86, 82], L_, go(f"dexg{NRANK + i}"), 5, align="center"))
    y3 = yy + 40 + ((len(LETTERS) + per - 1) // per) * 92 + 14
    w.append(label(30, y3, "FIND BY TYPE", T_SMALL, MUTE))
    tw = (W - 28) // len(FIND_TYPES)
    for i, a in enumerate(FIND_TYPES):
        x = 14 + i * tw
        w.append(button([x, y3 + 40, tw - 8, 90], "", go(f"dexg{NRANK + len(LETTERS) + i}")))
        w.append({"type": "image", "rect": [x + 12, y3 + 57, 56, 56], "src": f"file:gi/attr_{ATTRICON[a]}.png"})
        w.append(label(x + 76, y3 + 74, ATTR[a], T_SMALL, INK))
    return {"id": "dex", "title": "Digimon dex", "widgets": w}


DROWS = 6


DGROW = 132   # dex list row pitch
CI_MAX = [0]  # constants ci.0 .. ci.<max> (a repeat row's own index, for table lookups)


def dex_group(k):
    """One scrolling list per rarity. Real repeat templates (one per row element, {i} = row), so the
    runtime only builds the rows in view: portraits load as they scroll in."""
    title, rows = DEXGROUPS[k]
    n = len(rows)
    sc = f"dg{k}"
    CI_MAX[0] = max(CI_MAX[0], n)
    w = background() + tabs(2)
    w.append(button([14, 112, 220, 66], "< BACK", go("dex"), T_SMALL))
    w.append(label(264, 122, title.upper(), T_BODY, INK))
    w.append({"type": "value", "rect": [900, 126, 0, 0], "bind": f"dg{k}.count", "max_bind": f"dg{k}.total",
              "max_sep": " / ", "text_scale": T_SMALL, "color": ACC})
    w.append(label(264, 166, "Tap a row to record it. Hold a row for its summary page.", 2, MUTE))
    # per row: tap / hold actions, recorded state, attribute
    names, infos = {}, {}
    for i, (num, name, a, typ) in enumerate(rows):
        ACTIONS[f"dgt{k}_{i}"] = {"kind": "flag", "flag": f"d.{num}"}
        ACTIONS[f"dgh{k}_{i}"] = ACTIONS[dx_open((k + 1) * 1000 + num)]
        derive({"name": f"dgr{k}.{i}", "terms": [[f"@flag:d.{num}", 1]]})
        derive({"name": f"dga{k}{a}.{i}", "terms": [["@flag:tc.a", 0]], "add": 1})
        names[i] = name
        infos[i] = f"#{num:03d}   {ATTR[a]}   {typ}" if k < NRANK else f"#{num:03d}   {ATTR[a]}   {_RANKOF.get(num, '')}"
        if num in HASPIC and (PUBLIC or k < NRANK):
            DGPIC[(k, i)] = num
    table(f"dgn{k}", n, names)
    table(f"dgi{k}", n, infos)
    y = 192

    def rw(wd):   # one repeat template: n rows, DGROW apart, in the scrolling list
        wd.update({"repeat": n, "repeat_dx": 0, "repeat_dy": DGROW, "scroll": sc})
        return wd
    w.append(rw({"type": "button", "rect": [14, y, W - 28, 124], "text": "", "bg": PANEL, "on_tap": f"dgt{k}_{{i}}",
                 "on_hold": f"dgh{k}_{{i}}", "hold_ms": 450}))
    for a in sorted({r[2] for r in rows}):
        w.append(rw(rect([14, y, 12, 124], ATTRCOL[a], f"dga{k}{a}.{{i}}")))
        if a in ATTRICON:
            w.append(rw({"type": "image", "rect": [166, y + 70, 34, 34], "src": f"file:gi/attr_{ATTRICON[a]}.png",
                         "need_bind": f"dga{k}{a}.{{i}}"}))
    if PUBLIC or k < NRANK:
        w.append(rw({"type": "image", "rect": [34, y + 6, 112, 112], "src": f"file:dg{k}/{{i}}.png"}))
    else:   # find lists reuse the dex pictures: the row's own index picks its file
        w.append(rw({"type": "image", "rect": [34, y + 6, 112, 112], "bind": "ci.{i}",
                     "src_names": [f"file:img/{r[0]}.png" if r[0] in HASPIC else "" for r in rows]}))
    w.append(rw({"type": "value", "rect": [166, y + 14, 0, 0], "bind": "ci.{i}", "table": f"dgn{k}", "text_scale": 5,
                 "color": INK}))
    w.append(rw({"type": "value", "rect": [212, y + 76, 0, 0], "bind": "ci.{i}", "table": f"dgi{k}",
                 "text_scale": T_SMALL, "color": MUTE}))
    w.append(rw(rect([916, y + 44, 192, 40], WARN, f"dgr{k}.{{i}}")))
    w.append(rw(label(930, y + 54, "RECORDED", T_SMALL, DARK, gate=f"dgr{k}.{{i}}")))
    w.extend(rw(b_) for b_ in box("x", 1130, y + 30, 64, None, f"dgr{k}.{{i}}"))
    w.append(rw(button([1090, y, W - 14 - 1090, 124], "", f"dgt{k}_{{i}}", bg="#00000000", border=0)))
    return {"id": f"dexg{k}", "title": title, "widgets": w,
            "scrolls": [{"id": sc, "rect": [0, 186, W, H - 186], "row_h": DGROW, "pad": 12,
                         "bar": ACC, "bar_track": "#331D4D73", "bar_w": 6}]}


DGPIC = {}    # (group, row) -> dex number with a picture (personal build: dg<k>/<row>.png)


DXN = 476   # dex numbers go up to 475


def dex_detail():
    """One page for every Digimon. Flag dxh = +code while open, -code after Back (code = source*1000 + number)."""
    flag("dxh")
    derive({"name": "dx.open", "cmp": "gt", "a": "@flag:dxh", "b": 0})
    derive({"name": "dx.closed", "cmp": "lt", "a": "@flag:dxh", "b": 0})
    derive({"name": "dx.neg", "terms": [["@flag:dxh", -1]]})
    derive({"name": "dx.abs", "select": "dx.open", "then": "@flag:dxh", "else": "dx.neg"})
    derive({"name": "dx.num", "expr": "dx.abs % 1000"})
    derive({"name": "dx.src", "expr": "floor(dx.abs / 1000)"})
    ACTIONS["dxclose"] = {"kind": "flag", "flag": "dxh", "value": "$dx.neg"}
    PAGE_BINDS.append({"point": "dx.open", "equals": 1, "when_equal": {"page": "dx", "transition": "none"}})
    nums = sorted(ROWS)
    COVER["dx.num"] = (lambda k: f"dxs{k}", nums)
    for n in nums:
        derive({"name": f"dxs{n}", "cmp": "eq", "a": "dx.num", "b": n})
        derive({"name": f"dxon{n}", "all_nonzero": [f"@flag:d.{n}", f"dxs{n}"]})
    for a in ATTRICON:
        ns = [f"dxs{n}" for n in nums if ROWS[n][2] == a]
        if ns:
            derive({"name": f"dxa{a}", "any_nonzero": ns})
    derive({"name": "dxpic", "any_nonzero": [f"dxs{n}" for n in nums if n in HASPIC]})
    derive({"name": "dxnopic", "cmp": "eq", "a": "dxpic", "b": 0})
    w = background("file:ui/bg_dx.png")
    # data rain in the corner no summary text ever reaches (bgart.RAIN_BOX), behind everything else
    w.append({"type": "image", "rect": [846, 400, W - 846, H - 400], "src": "file:ui/dx_rain.png"})
    w.append(button([14, 14, 320, 96], "< BACK", go("dex"), 4))
    # placeholder only while no Digimon is open (no cover box: Eden outlines boxes)
    derive({"name": "dx.nonum", "cmp": "eq", "a": "dx.num", "b": 0})
    w.append(label(360, 300, "Tap a Digimon in the dex to open it here.", T_BODY, MUTE, wrap=800, lines=2,
                   gate="dx.nonum"))
    # back: to the dex group or area page the Digimon was opened from
    w.append(button([14, 14, 320, 96], "< BACK", "dxclose", 4, gate="dx.open"))
    srcs = [(k + 1, f"dexg{k}") for k in range(len(DEXGROUPS))] + [(100 + i, f"ar{i}") for i in range(len(area_groups()))]
    for sv, pg in srcs:
        derive({"name": f"dxsrc{sv}", "cmp": "eq", "a": "dx.src", "b": sv})
        derive({"name": f"dxret{sv}", "all_nonzero": ["dx.closed", f"dxsrc{sv}"]})
        PAGE_BINDS.append({"point": f"dxret{sv}", "equals": 1, "when_equal": {"page": pg, "transition": "none"}})
    for a in ATTRICON:
        if f"dxa{a}" in {d["name"] for d in DERIVED}:
            w.append(rect([0, 130, 16, 200], ATTRCOL[a], f"dxa{a}"))
            w.append({"type": "image", "rect": [36, 232, 52, 52], "src": f"file:gi/attr_{ATTRICON[a]}.png", "need_bind": f"dxa{a}"})
    w.append(tval(40, 140, "dx.num", table("dxname7", DXN, {n: ROWS[n][1] for n in nums if len(ROWS[n][1]) <= 16}), 7, INK))
    w.append(tval(40, 150, "dx.num", table("dxname5", DXN, {n: ROWS[n][1] for n in nums if len(ROWS[n][1]) > 16}), 5, INK))
    w += ttext(100, 244, "dx.num", "dxinfo", DXN, {n: f"#{n}   {ATTR[ROWS[n][2]]}   {ROWS[n][3]}" for n in nums}, 700, 4, ACC, 2, 40)
    # portrait: the whole picture as a circle, its ring just outside (DX_PIC wide, centred on DX_CX, DX_CY)
    rs = DX_RING_SIZE
    w.append({"type": "image", "rect": [DX_CX - rs // 2, DX_CY - rs // 2, rs, rs], "src": "file:ui/ring_dx.png",
              "need_bind": "dxpic"})
    w.append({"type": "image", "rect": [DX_CX - DX_PIC // 2, DX_CY - DX_PIC // 2, DX_PIC, DX_PIC],
              "src": "file:rnd/{i}.png", "need_bind": "dxs{i}", "repeat": DXN, "repeat_dx": 0, "repeat_dy": 0})
    derive({"name": "dxnopic.open", "all_nonzero": ["dxnopic", "dx.num"]})
    w.append(label(DX_CX, DX_CY - 8, "No picture", T_SMALL, MUTE, align="center", gate="dxnopic.open"))
    adv = {n: MATCH.get(ROWS[n][2]) for n in nums}
    w.append(tval(40, 330, "dx.num", table("dxstrong", DXN, {n: f"Strong vs {m[0]}" for n, m in adv.items() if m}), 5, ACC))
    w.append(tval(40, 400, "dx.num", table("dxweak", DXN, {n: f"Weak to {m[1]}" for n, m in adv.items() if m}), 5, WARN))
    w.append(tval(40, 330, "dx.num", table("dxnoadv", DXN, {n: "No attribute advantage" for n, m in adv.items() if not m}), 4, MUTE))
    w.append(label(40, 520, "HOW TO GET", T_SMALL, MUTE))
    w += ttext(40, 552, "dx.num", "dxloc", DXN, {n: LOC[n] for n in nums}, 1160, 4, INK, 3, 38)
    w.append(label(40, 676, "WILD IN", T_SMALL, MUTE))
    w.append(blabel(40, 708, "dsts.dx.wild", 3, WARN, wrap=1160, lines=3, line_gap=15))
    w.append(blabel(40, 708, "dsts.dx.nowild", 3, MUTE))
    w.append(label(40, 812, "EVOLVES INTO", T_SMALL, MUTE))
    w += ttext(40, 844, "dx.num", "dxinto", DXN,
               {n: ", ".join(INTO.get(n, [])) or "Nothing recorded in the guide" for n in nums}, 1160, 4, ACC, 3, 38)
    w.append(button([14, 976, 780, 92], "RECORDED", "dex_{i}", 4, align="left", text_inset=24, gate="dxs{i}",
                    repeat=DXN, repeat_dx=0, repeat_dy=0))
    w.append(rect([712, 990, 64, 64], MUTE))
    w.append(rect([716, 994, 56, 56], PANEL))
    w.append({"type": "rect", "rect": [728, 1006, 32, 32], "bg": WARN, "frame": 0, "need_bind": "dxon{i}",
              "repeat": DXN, "repeat_dx": 0, "repeat_dy": 0})
    return {"id": "dx", "title": "Digimon", "widgets": w}


# ---------------------------------------------------------------- type chart
def types_page():
    flag("tc.a")
    w = background() + tabs(3)
    w.append(label(24, 122, "ATTRIBUTES  (tap one)", T_SMALL, MUTE))
    bw = (W - 28) // len(ATTR_CHART)
    cols = ["#FF3FC1B8", "#FF5B9CF5", "#FFB06CF0", "#FFF2A64B", "#FFE8C547", "#FF93ADB0", "#FF93ADB0"]
    icons = ["000", "020", "040", "060", "080", "100", "120"]
    for n, (nm, strong, weak, note) in enumerate(ATTR_CHART):
        g = eq("tc.a", n)
        x = 14 + n * bw
        w.append(rect([x - 2, 146, bw - 2, 124], SEL, g))
        w.append(button([x + 2, 150, bw - 10, 116], "", setf("tc.a", n), T_SMALL))
        w.append({"type": "image", "rect": [x + (bw - 6) // 2 - 28, 158, 56, 56], "src": f"file:gi/attr_{icons[n]}.png"})
        w.append(label(x + 12, 226, nm, 2, INK, wrap=bw - 24, lines=1))
        w.append({"type": "image", "rect": [24, 290, 84, 84], "src": f"file:gi/attr_{icons[n]}.png", "need_bind": g})
        w.append(label(124, 306, nm.upper(), 6, cols[n], gate=g))
        neutral = strong == "Nothing"
        w.append(label(40, 400, "DEALS 2x DAMAGE TO", T_SMALL, MUTE, gate=g))
        w.append(label(40, 436, strong, 5, ACC, gate=g))
        w.append(label(640, 400, "TAKES 2x DAMAGE FROM", T_SMALL, MUTE, gate=g))
        w.append(label(640, 436, weak, 5, WARN, gate=g))
        w.append(label(40, 512, "RESISTS (TAKES 0.5x FROM)", T_SMALL, MUTE, gate=g))
        w.append(label(40, 548, "Nothing" if neutral else strong, 5, ACC, gate=g))
        w.append(label(40, 620, note, T_SMALL, MUTE, wrap=1160, lines=2, gate=g))
    w.append(rect([14, 680, W - 28, 4], PANEL))
    w.append(label(24, 696, "Vaccine beats Virus, Virus beats Data, Data beats Vaccine. Reverse flips it for that battle; Bug Recovery fixes it.",
                   T_SMALL, INK, wrap=1190, lines=2))
    w.append(label(24, 770, "ELEMENTS  (each Digimon has its own marks; check with Analyze, L2)", T_SMALL, MUTE))
    ew = (W - 28) // len(ELEMENTS)
    for n, e in enumerate(ELEMENTS):
        idx = {"Null": 0, "Fire": 1, "Ice": 2, "Plant": 3, "Water": 4, "Electricity": 5, "Steel": 6, "Wind": 7,
               "Earth": 8, "Light": 9, "Dark": 10}[e]
        x = 14 + n * ew
        w.append({"type": "image", "rect": [x + (ew - 64) // 2, 806, 64, 64], "src": f"file:gi/elem_{idx:02d}.png"})
        w.append(label(x + 4, 876, "Elec" if e == "Electricity" else e, 2, INK, wrap=ew - 6, lines=1))
    marks = [("2x", "big weak", WARN), ("1.5x", "weak", WARN), ("1x", "normal", INK), ("0.5x", "resists", ACC),
             ("0x", "immune", ACC)]
    mw = (W - 28) // len(marks)
    for n, (v, t, c) in enumerate(marks):
        x = 14 + n * mw
        w.append(rect([x, 930, mw - 8, 130], PANEL))
        w.append(label(x + 16, 948, v, T_BODY, c))
        w.append(label(x + 16, 1006, t, T_SMALL, MUTE))
    return {"id": "types", "title": "Type chart", "widgets": w}


# ---------------------------------------------------------------- tips and items
TROWS = 3


def tips_hub():
    w = background() + tabs(4)
    w.append(label(24, 120, "Tips by part of the story. Parts follow the main mission numbers.", T_SMALL, MUTE))
    entries = [(t, go(f"tp{k}")) for k, (t, _) in enumerate(TIP_PARTS)] + [("ITEMS", go("items")),
                                                                           ("AREA GUIDE", go("areas"))]
    for n, (t, act) in enumerate(entries):
        col, row = n % 2, n // 2
        x, y = 14 + col * 612, 160 + row * 152
        hi = t in ("ITEMS", "AREA GUIDE")
        w.append(button([x, y, 600, 140], t, act, T_BODY, SEL if hi else PANEL, DARK if hi else INK))
    return {"id": "tips", "title": "Tips", "widgets": w}


def tip_part(k):
    title, tips = TIP_PARTS[k]
    key = f"tpk{k}"
    w = background() + [back("tips")]
    w.append(label(280, 36, title.upper(), T_TITLE, INK, wrap=940, lines=1))
    n = (len(tips) + TROWS - 1) // TROWS
    for i, (t, body) in enumerate(tips):
        pg, r = divmod(i, TROWS)
        y = 120 + r * 280
        g = f"{key}_{pg}"
        w.append(rect([14, y, W - 28, 266], PANEL, g))
        w.append(label(40, y + 18, t, T_BODY, ACC, wrap=1160, lines=1, gate=g))
        w.append(label(40, y + 72, body, T_BODY, INK, wrap=1160, lines=5, gate=g))
    if n > 1:
        w.extend(pager(key, n, 14, 994, W - 28, 72))
    else:
        flag(f"p.{key}")
        derive({"name": f"{key}_0", "cmp": "eq", "a": f"@flag:p.{key}", "b": 0})
    return {"id": f"tp{k}", "title": title, "widgets": w}


IROWS = 7


def blabel(x, y, bind, scale=T_BODY, color=INK, wrap=None, lines=None, gate=None, **kw):
    """A label whose text the native module publishes (bind_text), from the player's own game files."""
    w = {"type": "label", "rect": [x, y, 0, 0], "bind_text": bind, "text_scale": scale, "color": color}
    if wrap:
        w["wrap_width"] = wrap
        if lines != 1:
            w["line_gap"] = 6
    if lines:
        w["max_lines"] = lines
    if gate:
        w["need_bind"] = gate
    w.update(kw)
    return w


def item_icon(r, item_id, gate=None):
    """An item's icon: the module looks the icon up in the player's item table by item id."""
    w = {"type": "image", "rect": r, "src": f"module:dsts:itemicon_{item_id}"}
    if gate:
        w["need_bind"] = gate
    return w


# Item groups: our labels, the game's item ids (names come from the game's item_name table).
ITEMNAME = {int(k): _clean(v) for k, v in GT.get("text/item_name:Sheet1", []) if v and k.isdigit()}
ICATS = [(cat, [{"id": i, "name": ITEMNAME.get(i, f"Item {i}")} for i in ids]) for cat, ids in GI["itemcats"]]
ALLITEMS = [(k, it) for k, (_, lst) in enumerate(ICATS) for it in lst]
ITEMIDX = {it["id"]: g for g, (_, it) in enumerate(ALLITEMS)}
CHROWS = 6
DATA = {"itempages": {}, "quests": {}}   # the manifest's "dsts_data" block (identifiers only)


def items_list():
    w = background() + [back("tips")]
    w.append(label(280, 36, "ITEMS", T_TITLE, INK))
    w.append(label(24, 116, f"{sum(len(l) for _, l in ICATS)} items from the game's own data. Pick a group.", T_SMALL, MUTE))
    for k, (cat, lst) in enumerate(ICATS):
        col, row = k % 2, k // 2
        x, y = 14 + col * 612, 160 + row * 166
        w.append(button([x, y, 600, 152], "", go(f"ic{k}")))
        if lst:
            w.append(item_icon([x + 24, y + 38, 76, 76], lst[0]["id"]))
        w.append(label(x + 120, y + 40, cat, T_BODY, INK, wrap=460, lines=1))
        w.append(label(x + 120, y + 92, f"{len(lst)} items", T_SMALL, ACC))
    return {"id": "items", "title": "Items", "widgets": w}


def item_cat_page(k):
    cat, lst = ICATS[k]
    w = background() + [back("items")]
    w.append(label(280, 36, cat.upper(), T_TITLE, INK, wrap=940, lines=1))
    key = f"icp{k}"
    n = (len(lst) + IROWS - 1) // IROWS
    DATA["itempages"][f"ic{k}"] = [ITEMIDX[it["id"]] for it in lst]
    for i, it in enumerate(lst):
        pg, r = divmod(i, IROWS)
        y = 124 + r * 122
        g = f"{key}_{pg}"
        w.append(button([14, y, W - 28, 112], "", go("it"), gate=g, select_group="it", payload=str(ITEMIDX[it["id"]]), draggable=False))
        w.append(item_icon([30, y + 20, 72, 72], it["id"], g))
        w.append(label(120, y + 14, it["name"], T_ROW, INK, wrap=1080, lines=1, gate=g))
        w.append(blabel(120, y + 66, f"dsts.ie{i}", T_SMALL, MUTE, wrap=1080, lines=1, gate=g))
    if n > 1:
        w.extend(pager(key, n, 14, 994, W - 28, 72, 2 if n > 10 else 3))
    else:
        flag(f"p.{key}")
        derive({"name": f"{key}_0", "cmp": "eq", "a": f"@flag:p.{key}", "b": 0})
    return {"id": f"ic{k}", "title": cat, "widgets": w}


def item_detail():
    """One page for every item. Rows set @sel:it = the item's index in ALLITEMS. What the item does,
    its price and where it is found come from the module (the player's own item, shop and treasure tables)."""
    n = len(ALLITEMS)
    COVER["@sel:it"] = (lambda k: f"its{k}", list(range(n)))
    for g in range(n):
        derive({"name": f"its{g}", "cmp": "eq", "a": "@sel:it", "b": g})
    w = background()
    w.append(button([14, 14, 240, 86], "< BACK", go("items"), T_SMALL))
    derive({"name": "it.any", "cmp": "ge", "a": "@sel:it", "b": 0})
    derive({"name": "it.none", "cmp": "eq", "a": "it.any", "b": 0})
    w.append(label(300, 300, "Tap an item in a list to open it here.", T_BODY, MUTE, wrap=800, lines=2,
                   gate="it.none"))
    for k in range(len(ICATS)):
        gk = derive({"name": f"itc{k}", "any_nonzero": [f"its{g}" for g, (kk, _) in enumerate(ALLITEMS) if kk == k]})
        w.append(button([14, 14, 240, 86], "< BACK", go(f"ic{k}"), T_SMALL, gate=gk))
    nm = {g: it["name"] for g, (_, it) in enumerate(ALLITEMS)}
    w.append(tval(280, 24, "@sel:it", table("itname6", n, {g: v for g, v in nm.items() if len(v) <= 20}), 6, INK))
    w.append(tval(280, 34, "@sel:it", table("itname4", n, {g: v for g, v in nm.items() if len(v) > 20}), 4, INK))
    w.append(tval(282, 92, "@sel:it", table("itcat", n, {g: ICATS[k][0].upper() for g, (k, _) in enumerate(ALLITEMS)}), T_SMALL, ACC))
    w.append({"type": "image", "rect": [930, 124, 290, 290], "src": "file:ui/glow.png"})
    w.append({"type": "image", "rect": [995, 189, 160, 160], "bind": "dsts.it.id",
              "src_format": "module:dsts:itemicon_%d", "need_bind": "it.any"})
    w.append(rect([14, 136, 900, 278], PANEL))
    w.append(rect([14, 136, 8, 278], ACC))
    w.append(label(44, 152, "WHAT IT DOES", T_SMALL, MUTE))
    w.append(blabel(44, 192, "dsts.it.eff", 4, INK, wrap=850, lines=5, line_gap=20))
    w.append(rect([14, 428, W - 28, 112], PANEL))
    w.append(rect([14, 428, 8, 112], WARN))
    w.append(label(44, 444, "PRICE", T_SMALL, MUTE))
    w.append(blabel(44, 482, "dsts.it.price", T_BODY, WARN))
    w.append(rect([14, 554, W - 28, 404], PANEL))
    w.append(rect([14, 554, 8, 404], ACC))
    w.append(label(44, 570, "WHERE TO GET IT", T_SMALL, MUTE))
    w.append(blabel(44, 610, "dsts.it.shop", T_BODY, ACC))
    w.append(blabel(44, 662, "dsts.it.chn", T_SMALL, MUTE))
    for r in range(CHROWS):
        y = 700 + r * 40
        w.append(rect([40, y, 1160, 36], BG, f"dsts.it.r{r}"))
        w.append(blabel(56, y + 8, f"dsts.it.ch{r}", 3, INK, wrap=1120, lines=1))
    w.append(blabel(44, 942, "dsts.it.more", 2, MUTE))
    w.append(blabel(44, 610, "dsts.it.none", T_BODY, INK, wrap=1150, lines=3, line_gap=22))
    # previous / next item (one repeated button each; only the one for the shown item is visible)
    w.append(button([14, 976, 600, 92], "", go("it"), T_SMALL, gate="its{i+1}", select_group="it", payload="{i}", draggable=False,
                    repeat=n - 1, repeat_dx=0, repeat_dy=0))
    w.append(button([626, 976, 600, 92], "", go("it"), T_SMALL, gate="its{i}", select_group="it", payload="{i+1}", draggable=False,
                    repeat=n - 1, repeat_dx=0, repeat_dy=0))
    cut = (lambda v: v) if PUBLIC else (lambda v: v[:24])
    w.append(tval(40, 1010, "@sel:it", table("itprev", n, {g: "< " + cut(nm[g - 1]) for g in range(1, n)}), T_SMALL, INK))
    w.append(tval(652, 1010, "@sel:it", table("itnext", n, {g: cut(nm[g + 1]) + " >" for g in range(n - 1)}), T_SMALL, INK))
    if PUBLIC:
        w[-2]["_wrap"] = [560, 1, 30]
        w[-1]["_wrap"] = [560, 1, 30]
    return {"id": "it", "title": "Item", "widgets": w}


# ---------------------------------------------------------------- areas (chests and wild Digimon)
FIELDNAME = {int(k): v for k, v in GT.get("text/field_name:Sheet1", []) if v}
# (name, treasure map ids, field ids) per area. What is in them comes from the module.
AREAS = [(FIELDNAME.get(key, f"Area {key}"), maps, fields) for key, maps, fields in GI["areas"]]
AROWS = 8
CH_SLOTS, WILD_SLOTS = 12, 25        # == kChestSlots / kWildSlots in the module


def area_groups():
    return AREAS


def areas_list():
    groups = area_groups()
    w = background() + [back("tips")]
    w.append(label(280, 36, "AREA GUIDE", T_TITLE, INK))
    w.append(button([980, 20, 246, 76], "LIVE CHECK", go("live"), T_SMALL))
    w += here_strip(14, 108, W - 28 - 212, 112, T_BODY)
    w += auto_toggle(W - 14 - 200, 108, 200, 112)
    key = "arpg"
    n = (len(groups) + AROWS - 1) // AROWS
    for i, (name, _, _) in enumerate(groups):
        pg, r = divmod(i, AROWS)
        y = 232 + r * 94
        g = f"{key}_{pg}"
        w.append(button([14, y, W - 28, 86], "", go(f"ar{i}"), gate=g))
        w.append(label(40, y + 24, name, T_ROW if len(name) < 40 else 3, INK, wrap=880, lines=1, gate=g))
        w.append(blabel(930, y + 14, f"dsts.arch{i}", 3, WARN, gate=g))
        w.append(blabel(930, y + 50, f"dsts.arw{i}", 3, ACC, gate=g))
    w.extend(pager(key, n, 14, 994, W - 28, 72, 2 if n > 9 else 3))
    return {"id": "areas", "title": "Areas", "widgets": w}


def area_gates():
    """Gates shared by every area page (one view flag for all of them): which layout fits the module's
    count of chests / wild Digimon, and each slot of each layout."""
    flag("av")
    g0, g1 = eq("av", 0), eq("av", 1)
    n_c, n_w = "dsts.ac.n", "dsts.aw.n"
    derive({"name": "ac.none", "all_nonzero": [g0, derive({"name": "ac.z", "cmp": "eq", "a": n_c, "b": 0})]})
    derive({"name": "ac.some", "all_nonzero": [g0, derive({"name": "ac.nz", "cmp": "ge", "a": n_c, "b": 1})]})
    derive({"name": "aw.none", "all_nonzero": [g1, derive({"name": "aw.z", "cmp": "eq", "a": n_w, "b": 0})]})
    derive({"name": "aw.some", "all_nonzero": [g1, derive({"name": "aw.nz", "cmp": "ge", "a": n_w, "b": 1})]})

    def band(name, var, lo, hi, view):
        parts = [view, derive({"name": f"{name}.lo", "cmp": "ge", "a": var, "b": lo})]
        if hi is not None:
            parts.append(derive({"name": f"{name}.hi", "cmp": "le", "a": var, "b": hi}))
        return derive({"name": name, "all_nonzero": parts})
    cv = [(band("acv1", n_c, 1, 4, g0), 4, 170), (band("acv2", n_c, 5, 5, g0), 5, 138),
          (band("acv3", n_c, 6, 6, g0), 6, 114), (band("acv4", n_c, 7, None, g0), CH_SLOTS, None)]
    for v, (g, slots, _) in enumerate(cv):
        for k in range(slots):
            derive({"name": f"acs{v}.{k}", "all_nonzero": [g, f"dsts.ac{k}"]})
    wv = [(band("awv1", n_w, 1, 3, g1), 3, (3, 220, 4), 1), (band("awv2", n_w, 4, 8, g1), 8, (4, 170, 3), 2),
          (band("awv3", n_w, 9, 12, g1), 12, (4, 120, 3), 3), (band("awv4", n_w, 13, None, g1), WILD_SLOTS, (5, 100, 2), 5)]
    for v, (g, slots, _, _) in enumerate(wv):
        for k in range(slots):
            derive({"name": f"aws{v}.{k}", "all_nonzero": [g, f"dsts.aw{k}"]})
    for k in range(WILD_SLOTS):   # a wild Digimon opens its dex page (dxh = area page * 1000 + dex number)
        ACTIONS[f"awt{k}"] = {"kind": "flag", "flag": "dxh", "value": f"$dsts.awp{k}"}
    return cv, wv


AREA_GATES = None


def area_page(i, name):
    global AREA_GATES
    if AREA_GATES is None:
        AREA_GATES = area_gates()
    cv, wv = AREA_GATES
    g0, g1 = eq("av", 0), eq("av", 1)
    w = background() + [back("areas")]
    w.append(label(280, 24, name, T_TITLE if len(name) < 30 else 4, INK, wrap=940, lines=2))
    w.append(label(1010, 124, "YOU ARE HERE", T_SMALL, WARN, gate=f"here.a{i}"))
    w += view_buttons("av", ["", ""])
    w.append(blabel(14 + 150, 114 + 30, "dsts.ac.tab", T_SMALL, INK, align="center"))
    w.append(blabel(324 + 150, 114 + 30, "dsts.aw.tab", T_SMALL, INK, align="center"))
    w.append(label(30, 230, "No treasure chests in this area.", T_BODY, MUTE, gate="ac.none"))
    w.append(label(30, 204, "Some chests only appear later in the story.", 2, MUTE, gate="ac.some"))

    def rep(wd, n, dy=0, cols=0, dx=0, row_dy=0):
        wd["repeat"] = n
        wd["repeat_dx"] = dx
        wd["repeat_dy"] = dy
        if cols:
            wd["repeat_cols"] = cols
            wd["repeat_row_dy"] = row_dy
        return wd
    for v, (_, slots, rh) in enumerate(cv):
        gate = f"acs{v}.{{i}}"
        if rh:   # roomy full-width rows with the item's effect
            y, isz, big = 236, min(120, rh - 24), rh >= 150
            dy = rh + 6
            w.append(rep(rect([14, y, W - 28, rh], PANEL, gate), slots, dy))
            w.append(rep(rect([14, y, 8, rh], WARN, gate), slots, dy))
            w.append(rep({"type": "image", "rect": [40, y + (rh - isz) // 2, isz, isz], "bind": "dsts.aci{i}",
                          "src_format": "module:dsts:itemicon_%d", "need_bind": gate}, slots, dy))
            w.append(rep(blabel(190, y + (30 if big else 18), "dsts.acn{i}", 5 if big else T_BODY, INK, wrap=1000,
                                lines=1, gate=gate), slots, dy))
            w.append(rep(blabel(190, y + (96 if big else 66), "dsts.ace{i}", T_SMALL, ACC if big else MUTE, wrap=1000,
                                lines=1, gate=gate), slots, dy))
        else:    # two columns of compact cards
            x, y = 14, 236
            g = dict(cols=2, dx=612, row_dy=122)
            w.append(rep(rect([x, y, 600, 112], PANEL, gate), slots, **g))
            w.append(rep(rect([x, y, 6, 112], WARN, gate), slots, **g))
            w.append(rep({"type": "image", "rect": [x + 20, y + 16, 80, 80], "bind": "dsts.aci{i}",
                          "src_format": "module:dsts:itemicon_%d", "need_bind": gate}, slots, **g))
            w.append(rep(blabel(x + 116, y + 26, "dsts.acn{i}", T_BODY, INK, wrap=470, lines=2, gate=gate,
                                fit_text=True, text_min_scale=3), slots, **g))
    w.append(label(30, 230, "No wild Digimon found here in the game data.", T_BODY, MUTE, gate="aw.none"))
    w.append(label(30, 204, "Not every one is here at every point in the story. Tap to open its dex page.", 2, MUTE,
                   gate="aw.some"))
    for v, (_, slots, (cols, pic, nscale), rows) in enumerate(wv):
        gate = f"aws{v}.{{i}}"
        cw = (W - 28) // cols
        chh = min(pic + 170, (980 - 236) // rows)
        pic = max(pic, min(cw - 40, chh - (110 if nscale > 2 else 60)))
        g = dict(cols=cols, dx=cw, row_dy=chh)
        x, y = 14, 236
        w.append(rep(button([x, y, cw - 8, chh - 8], "", "awt{i}", gate=gate), slots, **g))
        w.append(rep({"type": "image", "rect": [x + (cw - 8) // 2 - pic // 2, y + 6, pic, pic], "bind": "dsts.aws{i}",
                      "src_format": "module:dsts:ui_chara_icon_%d", "need_bind": gate}, slots, **g))
        w.append(rep(blabel(x + 12, y + pic + 12, "dsts.awn{i}", nscale, INK, wrap=cw - 30, lines=1, gate=gate),
                     slots, **g))
    return {"id": f"ar{i}", "title": name, "widgets": w}


# ---------------------------------------------------------------- live location (game update 1.2.2 only)
BUILD16 = "135C83CDEA06AEC2"   # 1.2.2 (old 1.0.x build was 63E71520BBB81E1E)
GAME = "main+0x1A90A10"            # global game manager pointer
FIELDMGR = "main+0x146F500"        # field manager; [+0x40] = current map info
# The game's own "current map id" code: map id = prefix offset (field_map_prefix) + [info+0x1F0],
# prefix text at info+0x1F4 ("t", "d", "h", "e"). The area name key is map id + 10000.
MAPINFO = [FIELDMGR, "0x40", "0x0"]
GDATA = "main+0x1482620"           # game data global (1.0.x: main+0x146EB08)
# Field Guide: S = [FM+0x10] holds a std::map (begin node at S+0x70, size at S+0x80) whose values are one
# contiguous array of 16-byte per-species records in status-id order: u16 id, u16 value, f32, u64.
RECS = [FIELDMGR, "0x10", "0x70", "0x28", "0x0"]
NREC = 454
SHOW_LEVEL = True    # 0.32.2: level = u16 D+0x68 (LEVEL TEST photos: 1 -> 2; HP 0x74, SP 0x78 matched too)
PARTY_TEST = False # 0.31.0 probe: party slots [S+8][i] (S = [FM+0x10]); slot+0 used flag, slot+8 -> Digimon object D
PARTY_WORDS = [0x40 + 4 * k for k in range(64)]   # 0.32.1 level probe: D+0x40..0x13C (base s16 0x6A, bonus 0x108)
FG_TEST = False   # True brings back the Field Guide test page and its 1,800 memory points
from rbpaths import rb_paths
# Test 3 showed the records are not one block, so walk the map's red-black tree: root at S+0x78, node: left +0,
# right +8, key +0x20, record pointer +0x28. Shape assumes the 454 keys were inserted in ascending order.
TREE = [FIELDMGR, "0x10", "0x78"]
RBP = rb_paths(NREC)


def node_chain(r):
    return TREE + [("0x8" if step else "0x0") for step in RBP[r]]
PFX = {"t": (116, 0), "d": (100, 10000), "h": (104, 20000), "e": (101, 30000)}


def build_points():
    pts = {"g_ptr": {"type": "u64", "addr": GAME, "pointer": True},
           "scene_state": {"type": "u32", "ptr": [GAME, "0xE8", "0x38", "0x0"], "offset": "0x40"},
           "map_num": {"type": "u32", "ptr": MAPINFO, "offset": "0x1F0"},
           "map_pfx": {"type": "u16", "ptr": MAPINFO, "offset": "0x1F4"}}
    # party (0.32.0): 6 slots [S+8][i] (S = [FM+0x10]); slot+0 used, slot+8 -> Digimon D.
    # D+4 = status id (owner photo: Patamon 96). Level = u16 D+0x68 (owner photos, 0.32.1 probe; the game
    # getter 0x51cd34 works on a different record). D+0x74 = HP, D+0x78 = SP.
    for i in range(6):
        slot = [FIELDMGR, "0x10", "0x8", hex(8 * i), "0x0"]
        dig = [FIELDMGR, "0x10", "0x8", hex(8 * i), "0x8", "0x0"]
        pts[f"pf{i}"] = {"type": "u32", "ptr": slot, "offset": "0x0"}
        pts[f"ps{i}"] = {"type": "u32", "ptr": dig, "offset": "0x4"}
        pts[f"plb{i}"] = {"type": "u16", "ptr": dig, "offset": "0x68"}   # level (owner photos: 1 -> 2)
    if PARTY_TEST:
        pv = [FIELDMGR, "0x10", "0x0"]
        pts["pv.b"] = {"type": "u64", "ptr": pv, "offset": "0x8"}
        pts["pv.e"] = {"type": "u64", "ptr": pv, "offset": "0x10"}
        for i in range(1):   # 0.32.1: slot 1 only
            slot = [FIELDMGR, "0x10", "0x8", hex(8 * i), "0x0"]
            for o in PARTY_WORDS:
                pts[f"pd{i}_{o:x}"] = {"type": "u32", "ptr": [FIELDMGR, "0x10", "0x8", hex(8 * i), "0x8", "0x0"],
                                       "offset": hex(o)}
    for r in range(NREC if FG_TEST else 0):
        pts[f"fk{r}"] = {"type": "u32", "ptr": node_chain(r) + ["0x0"], "offset": "0x20"}
        rec = node_chain(r) + ["0x28", "0x0"]
        pts[f"fs{r}"] = {"type": "f32", "ptr": rec, "offset": "0x4"}
        pts[f"fe{r}"] = {"type": "u32", "ptr": rec, "offset": "0x8"}
        pts[f"ff{r}"] = {"type": "u32", "ptr": rec, "offset": "0xC"}
    return {"points": pts}


def fg_test_page():
    """Field Guide test 5: the rest of each species record. Only records with data show numbers."""
    flag("fgv")
    nv = (NREC + 63) // 64
    for k in range(NREC):
        derive({"name": f"fnz{k}", "any_nonzero": [f"fs{k}", f"fe{k}", f"ff{k}"]})
    derive({"name": "fnzc", "terms": [[f"fnz{k}", 1] for k in range(NREC)]})
    w = background() + [back("live")]
    w.append(label(280, 24, "FIELD GUIDE TEST 5", T_TITLE, INK))
    w.append(label(280, 80, "Species with data:", T_SMALL, MUTE))
    w.append({"type": "value", "rect": [640, 80, 0, 0], "bind": "fnzc", "text_scale": T_SMALL, "color": WARN})
    w.append(label(24, 120, "Photo every view that has orange numbers in it.", T_SMALL, MUTE))
    for v in range(nv):
        g = eq("fgv", v)
        x = 14 + (v % 4) * 303
        y = 160 + (v // 4) * 70
        has = derive({"name": f"fvh{v}", "any_nonzero": [f"fnz{k}" for k in range(v * 64, min(NREC, v * 64 + 64))]})
        w.append(rect([x - 4, y - 4, 299, 66], SEL, g))
        w.append(rect([x - 4, y - 4, 299, 66], WARN, has))
        w.append(button([x, y, 291, 58], f"VIEW {v + 1}", setf("fgv", v), 2, border=2))
        for i in range(64):
            k = v * 64 + i
            if k >= NREC:
                break
            col, row = i % 4, i // 4
            cx, cy = 14 + col * 306, 312 + row * 48
            gk = derive({"name": f"fsh{k}", "all_nonzero": [g, f"fnz{k}"]})
            w.append({"type": "value", "rect": [cx, cy, 0, 0], "bind": f"fk{k}", "text_scale": 2,
                      "color": ACC, "need_bind": g})
            w.append({"type": "value", "rect": [cx + 60, cy, 0, 0], "bind": f"fs{k}", "text_scale": 2,
                      "color": WARN, "need_bind": gk})
            w.append({"type": "value", "rect": [cx, cy + 22, 0, 0], "bind": f"fe{k}", "text_scale": 2,
                      "color": INK, "need_bind": gk})
            w.append({"type": "value", "rect": [cx + 150, cy + 22, 0, 0], "bind": f"ff{k}", "text_scale": 2,
                      "color": MUTE, "need_bind": gk})
    return {"id": "fgt", "title": "Field guide test", "widgets": w}


HERE_NAMES = []      # (name, gate, area page index or None) for every named field
PAGE_BINDS = []


def here_setup():
    """Derived values that turn the live map id into an area name and one gate per area page."""
    for c, (code, off) in PFX.items():
        derive({"name": f"here.{c}", "cmp": "eq", "a": "map_pfx", "b": code})
    derive({"name": "here.key", "terms": [["map_num", 1]] + [[f"here.{c}", off] for c, (_, off) in PFX.items() if off],
            "add": 10000})
    derive({"name": "here.pfx", "any_nonzero": [f"here.{c}" for c in PFX]})
    derive({"name": "here.field", "cmp": "eq", "a": "scene_state", "b": 5})
    derive({"name": "here.ok", "all_nonzero": ["here.pfx", "here.field"]})
    byname = {}
    for fid, nm in sorted(FIELDNAME.items()):
        if nm.strip("?") and fid < 99999:
            byname.setdefault(nm, []).append(fid)
    areaidx = {nm: i for i, (nm, _, _) in enumerate(area_groups())}
    for j, (nm, fids) in enumerate(byname.items()):
        for fid in fids:
            derive({"name": f"here.f{fid}", "cmp": "eq", "a": "here.key", "b": fid})
        derive({"name": f"here.n{j}", "any_nonzero": [f"here.f{fid}" for fid in fids]})
        g = derive({"name": f"here.g{j}", "all_nonzero": [f"here.n{j}", "here.ok"]})
        HERE_NAMES.append((nm, g, areaidx.get(nm)))
    table("herename", len(HERE_NAMES) + 1, {j + 1: nm for j, (nm, _, _) in enumerate(HERE_NAMES)})
    derive({"name": "here.idx", "terms": [[g, j + 1] for j, (_, g, _) in enumerate(HERE_NAMES)]})
    derive({"name": "here.match", "cmp": "ne", "a": "here.idx", "b": 0})
    flag("auto.here", True)
    for i in range(len(areaidx)):
        gs = [g for _, g, ai in HERE_NAMES if ai == i]
        derive({"name": f"here.a{i}", "any_nonzero": gs})
        derive({"name": f"here.go{i}", "all_nonzero": [f"here.a{i}", "@flag:auto.here"]})
        PAGE_BINDS.append({"point": f"here.go{i}", "equals": 1,
                           "when_equal": {"page": f"ar{i}", "transition": "fade", "duration_ms": 200}})
    derive({"name": "here.hasarea", "any_nonzero": [f"here.a{i}" for i in range(len(areaidx))]})
    derive({"name": "here.nohas", "cmp": "eq", "a": "here.hasarea", "b": 0})
    derive({"name": "here.noarea", "all_nonzero": ["here.match", "here.nohas"]})
    derive({"name": "here.nomatch", "cmp": "eq", "a": "here.match", "b": 0})
    derive({"name": "here.unknown", "all_nonzero": ["here.nomatch", "here.ok"]})
    derive({"name": "here.notfield", "cmp": "eq", "a": "here.field", "b": 0})
    derive({"name": "here.autooff", "cmp": "eq", "a": "@flag:auto.here", "b": 0})


def here_line(x, y):
    """One-line live area name, for the strip under the tabs."""
    return [label(x, y + 2, "HERE", 2, MUTE),
            rect([x + 62, y - 2, 64, 26], WARN), label(x + 70, y + 2, "LIVE", 2, DARK),
            tval(x + 140, y, "here.idx", "herename", T_SMALL, WARN, "here.match"),
            label(x + 140, y, "Not on the field", T_SMALL, MUTE, gate="here.notfield"),
            label(x + 140, y, "Somewhere not in the guide", T_SMALL, MUTE, gate="here.unknown")]


def auto_toggle(x, y, w, h):
    return [button([x, y, w, h], "", toggle("auto.here"), T_SMALL),
            label(x + 24, y + 16, "AUTO-OPEN", 2, MUTE),
            label(x + 24, y + h - 40, "ON", T_BODY, WARN, gate="@flag:auto.here"),
            label(x + 24, y + h - 40, "OFF", T_BODY, MUTE, gate="here.autooff")]


def here_strip(x, y, w, h, scale=T_BODY):
    """A live 'You are here' panel. Tapping it opens that area's guide page."""
    nar = len(area_groups())
    out = [rect([x, y, w, h], PANEL), rect([x, y, 8, h], WARN),
           button([x + 8, y, w - 8, h], "", "go_ar{i}", gate="here.a{i}", bg="#00000000", border=0,
                  repeat=nar, repeat_dx=0, repeat_dy=0),
           label(x + 30, y + 16, "YOU ARE HERE", T_SMALL, MUTE),
           label(x + w - 250, y + 16, "OPEN GUIDE >", T_SMALL, ACC, gate="here.hasarea"),
           label(x + w - 330, y + 16, "No chests or Digimon", T_SMALL, MUTE, gate="here.noarea")]
    ty = y + h // 2 - 4
    out.append(tval(x + 30, ty, "here.idx", "herename", scale, WARN, "here.match"))
    out.append(label(x + 30, ty, "Not on the field right now", scale, MUTE, gate="here.notfield"))
    out.append(label(x + 30, ty, "This spot isn't in the guide", scale, MUTE, gate="here.unknown"))
    return out


def live_page():
    w = background() + [back("areas")]
    w.append(label(280, 30, "LIVE CHECK", T_TITLE, INK))
    w.append(label(24, 104, "What the companion reads from the game. Only needed if YOU ARE HERE looks wrong.",
                   T_SMALL, MUTE, wrap=1190, lines=2))
    rows = [("Game found", "g_ptr"), ("Scene (5 = walking around)", "scene_state"),
            ("Map letter code", "map_pfx"), ("Map number", "map_num"), ("Area key", "here.key")]
    for k, (t, b) in enumerate(rows):
        y = 190 + k * 92
        w.append(rect([14, y, W - 28, 80], PANEL))
        w.append(label(40, y + 26, t, T_BODY, INK))
        w.append({"type": "value", "rect": [800, y + 24, 0, 0], "bind": b, "text_scale": T_BODY, "color": WARN})
    w.append(label(24, 670, "Letter codes: 116 = town, 100 = Digital World, 104 = other places, 101 = special.",
                   T_SMALL, MUTE, wrap=1190, lines=2))
    w += here_strip(14, 760, W - 28, 160)
    if FG_TEST:
        w.append(button([14, 950, W - 28, 110], "FIELD GUIDE TEST", go("fgt"), T_BODY, SEL, DARK))
    if PARTY_TEST:
        w.append(button([14, 930, W - 28, 70], "LEVEL TEST", go("ptest"), T_SMALL, SEL, DARK))
    return {"id": "live", "title": "Live check", "widgets": w}


def party_test_page():
    """0.32.1 level probe: party slot 1's Digimon data, D+0x40..0x13C as 32-bit words (4 columns), plus the
    two numbers the level is built from. Photos at two levels show which word is the level."""
    w = background() + [back("live")]
    w.append(label(280, 30, "LEVEL TEST", T_TITLE, INK))
    w.append(label(24, 104, "Photo this page. Level up the Digimon in slot 1 (one fight is enough), then photo it again.",
                   T_SMALL, MUTE, wrap=1190, lines=2))
    y = 150
    for k, (t, b) in enumerate((("LEVEL (68)", "plb0"), ("SHOWN LEVEL", "plv0"))):
        x = 24 + k * 400
        w.append(label(x, y, t, 2, MUTE))
        w.append({"type": "value", "rect": [x, y + 24, 0, 0], "bind": b, "text_scale": T_SMALL, "color": WARN})
    y0, rh, cols = 230, 46, 4
    per = (len(PARTY_WORDS) + cols - 1) // cols
    cw = (W - 28) // cols
    for n, o in enumerate(PARTY_WORDS):
        c, r = divmod(n, per)
        x, y = 14 + c * cw, y0 + r * rh
        if r % 2 == 0 and c == 0:
            w.append(rect([14, y - 8, W - 28, rh], "#FF0B2238"))
        w.append(label(x + 8, y, f"{o:X}", 2, MUTE))
        w.append({"type": "value", "rect": [x + 64, y - 2, 0, 0], "bind": f"pd0_{o:x}", "text_scale": T_SMALL,
                  "color": INK})
    return {"id": "ptest", "title": "Level test", "widgets": w}


# ---------------------------------------------------------------- manifest
SKINS = set()


def skin_pass(pages):
    """Put a pre-drawn angled panel image under every button and make the button itself transparent."""
    for p in pages:
        out = []
        for w in p["widgets"]:
            if w.get("type") == "button" and w.get("bg") != "#00000000":
                x, y, bw, bh = w["rect"]
                kind = "on" if w.get("bg") in (SEL, WARN, ACC) else "panel"
                SKINS.add((kind, bw, bh))
                img = {"type": "image", "rect": [x, y, bw, bh], "src": f"file:ui/{kind}_{bw}x{bh}.png"}
                for kk in ("need_bind", "repeat", "repeat_dx", "repeat_dy", "repeat_cols", "repeat_row_dy", "scroll",
                           "y_bind", "x_bind"):
                    if kk in w:
                        img[kk] = w[kk]
                out.append(img)
                w["bg"] = "#00000000"
                w["border"] = 0
            out.append(w)
        p["widgets"] = out


def draw_skin(kind, w, h):
    from PIL import Image, ImageDraw
    c = 16 if min(w, h) >= 60 else 10
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    poly = [(c, 0), (w - 1, 0), (w - 1, h - 1 - c), (w - 1 - c, h - 1), (0, h - 1), (0, c)]
    if kind == "on":
        d.polygon(poly, fill=(255, 138, 31, 255))
        d.line(poly + [poly[0]], fill=(255, 196, 140, 255), width=3)
    else:
        top, bot = (14, 46, 85), (8, 32, 61)
        grad = Image.new("RGBA", (w, h))
        gd = ImageDraw.Draw(grad)
        for yy in range(h):
            t = yy / max(1, h - 1)
            gd.line([(0, yy), (w, yy)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (240,))
        mask = Image.new("L", (w, h), 0)
        ImageDraw.Draw(mask).polygon(poly, fill=255)
        im.paste(grad, (0, 0), mask)
        d.line(poly + [poly[0]], fill=(43, 228, 255, 255), width=3)
        d.line([(c + 8, 4), (min(w - 8, c + 8 + w // 5), 4)], fill=(43, 228, 255, 120), width=2)
    return im


def draw_bg():
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (W, H), (6, 21, 41, 255))
    d = ImageDraw.Draw(im)
    for x in range(0, W, 48):
        d.line([(x, 0), (x, H)], fill=(18, 52, 82, 255), width=2)
    for y in range(0, H, 48):
        d.line([(0, y), (W, y)], fill=(18, 52, 82, 255), width=2)
    return im


def draw_dot(col):
    from PIL import Image, ImageDraw
    c = tuple(int(col[i:i + 2], 16) for i in (3, 5, 7)) + (255,)
    im = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    ImageDraw.Draw(im).ellipse([4, 4, 252, 252], fill=c)
    return im.resize((64, 64), Image.LANCZOS)


ROUND_R = 1.0    # portrait circle radius, as a fraction of half the picture (the whole picture, corners off)
DX_PIC, DX_CX, DX_CY = 470, 962, 272   # summary page portrait: size and centre
DX_RING_GAP = 12                       # ring radius = DX_PIC / 2 + gap
DX_RING_SIZE = 2 * (DX_PIC // 2 + DX_RING_GAP) + 8
ROUND_EDGE = 1.5  # soft edge width in source pixels


def round_portrait(path):
    """The summary page portrait: cropped to a circle with a soft edge (same maths as the module's
    RoundMask, so personal and public builds look the same)."""
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    px = im.load()
    cx, cy, rr = w / 2.0, h / 2.0, ROUND_R * min(w, h) / 2.0
    for yy in range(h):
        for xx in range(w):
            d = ((xx + 0.5 - cx) ** 2 + (yy + 0.5 - cy) ** 2) ** 0.5
            k = min(1.0, max(0.0, (rr - d) / ROUND_EDGE + 0.5))
            if k < 1.0:
                r_, g_, b_, a_ = px[xx, yy]
                px[xx, yy] = (r_, g_, b_, int(a_ * k + 0.5))
    return im


def draw_ring_dx():
    """The summary page's glow and dashed ring, sized to sit just outside the round portrait."""
    from PIL import Image, ImageDraw
    import math
    size, rr = DX_RING_SIZE, DX_PIC // 2 + DX_RING_GAP
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = im.load()
    c = size / 2
    for yy in range(size):
        for xx in range(size):
            r = math.hypot(xx + 0.5 - c, yy + 0.5 - c) / rr
            if r < 1:
                px[xx, yy] = (43, 228, 255, int(110 * (1 - r) ** 1.4))
    d = ImageDraw.Draw(im)
    for k in range(0, 360, 12):
        d.arc([c - rr, c - rr, c + rr, c + rr], k, k + 6, fill=(43, 228, 255, 150), width=3)
    return im


def draw_glow():
    from PIL import Image, ImageDraw
    s = 400
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    px = im.load()
    for yy in range(s):
        for xx in range(s):
            r = ((xx - s / 2) ** 2 + (yy - s / 2) ** 2) ** 0.5 / (s / 2)
            if r < 1:
                a = int(110 * (1 - r) ** 1.4)
                px[xx, yy] = (43, 228, 255, a)
    d = ImageDraw.Draw(im)
    import math
    for k in range(0, 360, 12):
        a0 = math.radians(k)
        d.arc([22, 22, s - 22, s - 22], k, k + 6, fill=(43, 228, 255, 150), width=3)
    return im


def manifest():
    here_setup()
    mission_state()
    pages = [now_page(), missions_list(), dex_hub()] + [dex_group(k) for k in range(len(DEXGROUPS))]
    pages += [types_page(), tips_hub(), items_list()] + [tip_part(k) for k in range(len(TIP_PARTS))]
    _groups, _ = side_groups()
    pages += [side_mission_page(i, sq) for i, sq in sorted(_groups.items())]
    pages += [main_page(i) for i in range(len(MAIN))] + [side_page(s) for s in SIDE]
    pages += [item_cat_page(k) for k in range(len(ICATS))]
    pages += [item_detail()]
    pages += [live_page()] + ([fg_test_page()] if FG_TEST else []) + ([party_test_page()] if PARTY_TEST else [])
    pages += [areas_list()] + [area_page(i, nm) for i, (nm, _, _) in enumerate(area_groups())]
    pages += [dex_detail()]
    allnums = [r[0] for _, rows in DEXGROUPS[:NRANK] for r in rows]
    for n in allnums:
        flag(f"d.{n}", True)
        ACTIONS[f"dex_{n}"] = {"kind": "flag", "flag": f"d.{n}"}
    for k, (_, rows) in enumerate(DEXGROUPS):
        DERIVED.append({"name": f"dg{k}.count", "terms": [[f"@flag:d.{r[0]}", 1] for r in rows]})
        DERIVED.append({"name": f"dg{k}.total", "terms": [["@flag:tc.a", 0]], "add": len(rows)})
    DERIVED.append({"name": "dex.count", "terms": [[f"dg{k}.count", 1] for k in range(NRANK)]})
    DERIVED.append({"name": "dex.total", "terms": [["@flag:tc.a", 0]], "add": len(allnums)})
    for c in range(CI_MAX[0]):
        DERIVED.append({"name": f"ci.{c}", "terms": [["@flag:tc.a", 0]], "add": c})
    bottom_tabs(pages)
    skin_pass(pages)
    version_notice(pages)
    names = [d["name"] for d in DERIVED]
    assert len(names) == len(set(names)), "duplicate derived"
    return {"format": 1, "name": PKG_NAME, "min_runtime": MIN_RUNTIME,
            "canvas_w": W, "canvas_h": H, "background": BG,
            "_note": "Second-screen companion for Digimon Story Time Stranger (update 1.2.2, build 135C83CDEA06AEC2).",
            "nav": False, "tables": TABLES, "page_binds": PAGE_BINDS,
            "flags": FLAGS, "persist_flags": PERSIST, "derived": DERIVED, "actions": ACTIONS,
            "haptics": {"enabled": True, "respect_system": True, "tap": "click"},
            "pages": pages, "dsts_data": dsts_data()}


def dsts_data():
    """What the module needs to know to fill the pages: identifiers only (see module/dsts_data.inc)."""
    top = max(ROWS) + 1
    return {"v": 1,
            "items": [it["id"] for _, it in ALLITEMS],
            "itempages": DATA["itempages"],
            "areas": GI["areas"],
            "g2s": [G2S.get(n, -1) if n in ROWS else -1 for n in range(top)],
            "quests": DATA["quests"]}


def version_notice(pages):
    """On every page: a cover that says why the guide is empty when the module did not load, which is
    what happens on any game version but 1.2.2 (the host publishes module_error then)."""
    derive({"name": "dsts.badver", "cmp": "eq", "a": "module_error", "b": 1})
    for p in pages:
        p["widgets"] += [
            {"type": "rect", "rect": [0, 0, W, H], "bg": "#F5061529", "frame": 0, "color": "#00000000",
             "need_bind": "dsts.badver", "input_block": True},
            label(W // 2, 330, "Needs game version 1.2.2", 6, WARN, align="center", gate="dsts.badver"),
            label(W // 2 - 520, 440, "This guide reads Digimon Story Time Stranger update 1.2.2, build "
                                     "135C83CDEA06AEC2, from your game files. Update the game to 1.2.2, "
                                     "then start it again.", T_BODY, INK, wrap=1040, lines=4, gate="dsts.badver"),
            {"type": "label", "rect": [W // 2 - 520, 680, 0, 0], "bind_text": "module_error_message",
             "text_scale": T_SMALL, "color": MUTE, "wrap_width": 1040, "max_lines": 3, "need_bind": "dsts.badver"}]


MODULE_SO = os.environ.get("DSTS_MODULE_SO") or os.path.join(HERE, "module", "010062E01FE0C000.so")


def module_src(src):
    """Public build: a packaged game picture path -> the module key that serves it from the romfs."""
    m = re.fullmatch(r"file:rnd/(\d+)\.png", src)
    if m:
        n = int(m.group(1))
        return f"module:dsts:round_ui_chara_icon_{1000 + G2S[n]}" if n in G2S else None
    m = re.fullmatch(r"file:img/(\d+)\.png", src)
    if m:
        n = int(m.group(1))
        return f"module:dsts:ui_chara_icon_{1000 + G2S[n]}" if n in G2S else None
    m = re.fullmatch(r"file:ii/(\d+)\.png", src)
    if m:
        return f"module:dsts:ui_icon_item_{ALLITEMS[int(m.group(1))][1]['icon']:03d}"
    m = re.fullmatch(r"file:gi/attr_(\d+)\.png", src)
    if m:
        return f"module:dsts:ui_icon_type_{m.group(1)}"
    m = re.fullmatch(r"file:gi/elem_(\d+)\.png", src)
    if m:
        return f"module:dsts:ui_icon_skill_{int(m.group(1)):03d}"
    m = re.fullmatch(r"file:gi/item_(\d+)\.png", src)
    if m:
        return f"module:dsts:ui_icon_item_{m.group(1)}"
    return src


def publicize(man):
    """Swap every game picture for a module key; expand repeated picture widgets one by one."""
    known = {d["name"] for d in man["derived"]} | {"@flag:" + f for f in man["flags"]}
    for p in man["pages"]:
        out = []
        for w in p["widgets"]:
            src = w.get("src", "")
            mdg = re.fullmatch(r"file:dg(\d+)/\{i\}\.png", src)
            if mdg:
                w["src"] = f"module:dsts:dg{mdg.group(1)}_{{i}}"
                out.append(w)
                continue
            if not src.startswith(("file:img/", "file:ii/", "file:gi/", "file:rnd/")):
                out.append(w)
                continue
            if "repeat" in w and "{i" not in src:
                w["src"] = module_src(src)
                if w["src"]:
                    out.append(w)
                continue
            if "repeat" in w:
                n, dx, dy = w["repeat"], w.get("repeat_dx", 0), w.get("repeat_dy", 0)
                for i in range(n):
                    js = re.sub(r"\{i([+-]\d+)?\}", lambda mm: str(i + int(mm.group(1) or 0)), json.dumps(w))
                    e = json.loads(js)
                    for k in ("repeat", "repeat_dx", "repeat_dy"):
                        e.pop(k, None)
                    e["rect"] = [e["rect"][0] + i * dx, e["rect"][1] + i * dy] + e["rect"][2:]
                    e["src"] = module_src(e["src"])
                    if e["src"] and (e.get("need_bind") is None or e["need_bind"] in known):
                        out.append(e)
            else:
                w["src"] = module_src(src)
                if w["src"]:
                    out.append(w)
        p["widgets"] = out
    rows = {}
    for (gk, gi), n in DGPIC.items():
        if n in G2S:
            rows.setdefault(f"dg{gk}", {})[gi] = 1000 + G2S[n]
    man["dsts_img"] = {g: [r.get(i, -1) for i in range(max(r) + 1)] for g, r in rows.items()}
    for w in man["pages"]:
        if w["id"] == "live":
            w["widgets"] += [label(24, 1010, "Pictures found in your game files:", T_SMALL, MUTE),
                             {"type": "value", "rect": [700, 1010, 0, 0], "bind": "dsts.pictures",
                              "text_scale": T_SMALL, "color": WARN}]
    return man


def main():
    argv = sys.argv[1:]
    pkg_dir = None
    if "--package-dir" in argv:
        k = argv.index("--package-dir")
        pkg_dir = argv[k + 1]
        del argv[k:k + 2]
    args = [a for a in argv if a not in ("--public", "--personal")]
    if len(args) != 1 or not re.fullmatch(r"[0-9A-F]{16}", args[0]):
        sys.exit("usage: build_dsts_package.py <TITLEID> [--package-dir DIR] [--personal]")
    if pkg_dir and not PUBLIC:
        sys.exit("--package-dir writes the public package only")
    tid = args[0]
    pkg = {"format": 1, "type": "dual-screen-mod", "title_id": tid, "name": PKG_NAME,
           "version": VERSION, "min_runtime": MIN_RUNTIME}
    out = os.path.join(HERE, f"{tid}-DigimonGuide-{VERSION}.dsmod.zip")
    man = manifest()
    # Both builds carry the module: it draws the game's own font from the player's game files (the public
    # build also needs it for every picture and game text). Without it, Eden keeps its built-in font.
    import hashlib
    libs = {}   # platform -> library bytes, for a zip built here (the repository's release tool adds its own)
    if not pkg_dir:
        libs["android-arm64-v8a"] = open(MODULE_SO, "rb").read()
        if os.environ.get("DSTS_MODULE_SO_LINUX"):
            libs["linux-x86_64"] = open(os.environ["DSTS_MODULE_SO_LINUX"], "rb").read()
    mod = {"abi": 1, "build_ids": [BUILD16]}
    if libs:
        mod["libraries"] = {pl: {"path": f"modules/{pl}/010062E01FE0C000.so", "sha256": hashlib.sha256(b).hexdigest()}
                            for pl, b in sorted(libs.items())}
    man["font"] = "file:ui/font.txt"
    man["font_atlas"] = "module:dsts:font"
    if not PUBLIC:
        man["module"] = mod
        pkg["module"] = mod
    if PUBLIC:
        man = publicize(man)
        import public_text
        _tj = guidedata.text_json()
        man, _T = public_text.textify(man, _tj)
        # wild Digimon names on the area pages: the module publishes the dex name template by dex number
        man["dsts_data"]["dexname"] = [(_T.template(ROWS[n][1]) if _T.template(ROWS[n][1]) is not None
                                        else _T.literal(ROWS[n][1])) if n in ROWS else -1
                                       for n in range(len(man["dsts_data"]["g2s"]))]
        for _p in man["pages"]:
            if _p["id"] == "live":
                _p["widgets"] += [label(24, 974, "Game tables found in your game files:", T_SMALL, MUTE),
                                  {"type": "value", "rect": [700, 974, 0, 0], "bind": "dsts.data", "max_const": 11,
                                   "max_sep": " of ", "text_scale": T_SMALL, "color": WARN}]
        _left = public_text.leftovers(man, _tj)
        print(f"game text: {len(_T.refs)} references, {len(_T.tpl)} templates, leftovers {len(_left)}")
        for _l in _left[:40]:
            print("  leftover", _l)
        if os.environ.get("DSTS_TEXT_DUMP"):
            json.dump({str(k): v for k, v in _T.orig.items()}, open(os.environ["DSTS_TEXT_DUMP"], "w"))
        man["title_id"] = tid
        man["requires_module"] = True
        man["module"] = mod
        man["_note"] = ("Ships no game pictures, text or data: the native module reads them from the player's "
                        "own game files (update 1.2.2, build 135C83CDEA06AEC2).")
        pkg["requires_module"] = True
        pkg["module"] = mod
        out = os.path.join(HERE, f"{tid}-{PKG_FILE}-{VERSION}.dsmod.zip")
    # Peanuts Native finds a package by "title_id" in the manifest's first 4 KiB (peanuts.c rt_scan_root);
    # Eden Duo checks the same field wherever it is. Keep it first.
    man.pop("title_id", None)
    man = {"title_id": tid, **man}
    man = json.dumps(man, separators=(",", ":"))

    class DirOut:   # the same files, as a source package folder (packages/TimeStrangerFieldGuide)
        def __init__(self, root):
            self.root = root

        def writestr(self, name, data):
            if name == "package.json":
                return
            path = os.path.join(self.root, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data.encode("utf8") if isinstance(data, str) else data)

        def write(self, src, name):
            self.writestr(name, open(src, "rb").read())

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False
    if pkg_dir:
        import shutil
        shutil.rmtree(os.path.join(pkg_dir, "dualscreen"), ignore_errors=True)
        out = pkg_dir
    with (DirOut(pkg_dir) if pkg_dir else zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED)) as z:
        z.writestr("package.json", json.dumps(pkg, indent=1))
        z.writestr("dualscreen/manifest.json", man)
        z.writestr(f"dualscreen/{BUILD16}.json", json.dumps(build_points(), indent=1))
        import io
        def png(im):
            b = io.BytesIO(); im.save(b, "PNG", optimize=True); return b.getvalue()
        import bgart
        z.writestr("dualscreen/ui/bg.png", png(bgart.draw_bg_even()))
        z.writestr("dualscreen/ui/bg_dx.png", png(bgart.draw_bg()))
        z.writestr("dualscreen/ui/dx_rain.png", png(bgart.draw_dx_rain()))
        z.writestr("dualscreen/ui/glow.png", png(draw_glow()))
        z.writestr("dualscreen/ui/ring_dx.png", png(draw_ring_dx()))
        z.writestr("dualscreen/ui/boss_unknown.png", png(bgart.draw_unknown_boss()))
        z.writestr("dualscreen/ui/locbar.png", png(bgart.draw_locbar(W - 28, BAR_H, BAR_DET)))
        z.writestr("dualscreen/ui/live_dot.png", png(bgart.draw_live_dot()))
        z.writestr("dualscreen/ui/font.txt", "Digimon guide: the game font comes from the player's own game files "
                                             "(module decode_font).\n")
        for nm, col in (("dot_acc", ACC), ("dot_warn", WARN), ("dot_dim", "#FF1D4D73")):
            z.writestr(f"dualscreen/ui/{nm}.png", png(draw_dot(col)))
        for kind, bw, bh in sorted(SKINS):
            z.writestr(f"dualscreen/ui/{kind}_{bw}x{bh}.png", png(draw_skin(kind, bw, bh)))
        for pl, b in sorted(libs.items()):
            z.writestr(f"dualscreen/modules/{pl}/010062E01FE0C000.so", b)
        if not PUBLIC:
            for n in sorted(HASPIC):
                z.write(PICSRC[n], f"dualscreen/img/{n}.png")
            from PIL import Image
            for st, g in sorted(S2G.items()):   # party portraits, by status id
                if g in HASPIC:
                    im = round_portrait(PICSRC[g]).resize((PARTY_PIC, PARTY_PIC), Image.LANCZOS)
                    z.writestr(f"dualscreen/rst/{st}.png", png(im))
            for (gk, gi), n in sorted(DGPIC.items()):
                z.write(PICSRC[n], f"dualscreen/dg{gk}/{gi}.png")
            for n in sorted(HASPIC):
                z.writestr(f"dualscreen/rnd/{n}.png", png(round_portrait(PICSRC[n])))
            for _f in sorted(os.listdir(GAMEICONS)) if os.path.isdir(GAMEICONS) else []:
                z.write(os.path.join(GAMEICONS, _f), f"dualscreen/gi/{_f}")
    print("wrote", out, "manifest bytes", len(man))


if __name__ == "__main__":
    main()
