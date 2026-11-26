# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Public build, phase 2: take the game's own text out of the manifest.

Every label, button caption and lookup-table entry that contains game text becomes a "slot". The
manifest keeps a template per slot: our own wording, with each piece of game text replaced by a
reference (\\x01<n>[case]\\x02, case l/u/c/f when the place writes the word in lower or upper case or
with its first letter changed). A reference is the 64-bit FNV-1a hash of the cleaned game string, never
the string. At run time the native module reads the player's own text archive, hashes every string
it finds the same way, keeps the ones referenced, and publishes the filled-in slots for the page on
screen (bind_text). Tables picked by a selection (item page, dex summary) are filled per selection.

clean() must stay byte-for-byte identical to Clean() in module/dsts_module.cpp.
"""
import json
import re

# Short game terms (types, attributes, elements, stats, stages): always referenced, single words too.
TERM_SHEETS = {"belong", "digimon_type", "element", "status_name", "digimon_class_name", "generation_name",
               "buff_name", "item_name", "item_ruby"}
NAME_SHEETS = {"char_name", "item_name", "item_ruby", "field_name", "worldmap_place_name", "worldmap_group_name",
               "quest_title", "main_quest_title", "quest_client", "skill_name", "jogress_skill_name",
               "tamer_skill_name", "personality_skill_name", "item_pack_name", "trophy", "tutorial_title", "bgm_name"}
# Single-word names from these sheets are ordinary words ("Battery", "Guard"): kept as our own text,
# unless the word is a coined game name.
WORDY_SHEETS = NAME_SHEETS - {"char_name", "field_name", "worldmap_place_name", "worldmap_group_name",
                              "quest_title", "main_quest_title", "quest_client"}
STOP = {"man", "woman", "everyone", "device", "both", "wild digimon", "digimon", "boy", "girl", "old man",
        "old woman", "voice", "???", "entrance", "digifarm", "side missions", "agent rank", "equipment"}
LONG_MIN = 25
# Content tables: always from the game, short entries included.
CONTENT_SHEETS = {"item_explanation", "quest_step", "main_quest_step", "quest_outline", "digimon_profile",
                  "worldmap_place_explanation"}
CONTENT_MIN = 8
WS = re.compile(r"[ \t\n\r\v\f]+")


def clean(s):
    """Drop {..} markup (icons, images, variables), keep the text inside {fcN...} colour tags,
    collapse ASCII whitespace."""
    def walk(t):
        out, i = [], 0
        while i < len(t):
            if t[i] == "{":
                depth, j = 1, i + 1
                while j < len(t) and depth:
                    depth += (t[j] == "{") - (t[j] == "}")
                    j += 1
                inner = t[i + 1:j - 1] if depth == 0 else t[i + 1:j]
                m = re.match(r"fc[0-9]+", inner)
                if m:
                    out.append(walk(inner[m.end():]))
                i = j
            else:
                out.append(t[i])
                i += 1
        return "".join(out)
    return " ".join(x for x in WS.split(walk(s)) if x)


def fnv(s):
    h = 0xcbf29ce484222325
    for b in s.encode("utf8"):
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def coined(w):
    w = w.lower()
    return w.endswith("mon") or "digi" in w or any(c.isdigit() for c in w)


def dictionary(text_json):
    t = json.load(open(text_json, encoding="utf8"))
    d = {}
    for sheet in sorted(t):   # sorted: the same game string wins whatever order the dump has
        rows = t[sheet]
        base = sheet.split("/")[-1].split(":")[0]
        for r in rows:
            if len(r) < 2 or not isinstance(r[1], str):
                continue
            c = clean(r[1])
            if not c:
                continue
            k = c.lower()
            if base in TERM_SHEETS:
                if len(c) < 2 or not re.search(r"[A-Za-z]", c):
                    continue
            elif base in NAME_SHEETS:
                if len(c) < 3 or k in STOP:
                    continue
                if base in WORDY_SHEETS and " " not in c and not coined(c):
                    continue
            elif len(c) < (CONTENT_MIN if base in CONTENT_SHEETS else LONG_MIN):
                continue
            d.setdefault(k, c)
    return d


class Matcher:
    def __init__(self, d):
        self.d = d
        keys = sorted(d, key=len, reverse=True)
        self.rx = re.compile(r"(?<![A-Za-z0-9])(?:" + "|".join(re.escape(k) for k in keys) + r")(?![A-Za-z0-9])",
                             re.I)

    def find(self, s):
        return [(m.start(), m.end(), self.d[m.group(0).lower()]) for m in self.rx.finditer(s)]


class Texts:
    def __init__(self, matcher):
        self.m = matcher
        self.refs, self.ref_ix = [], {}
        self.tpl, self.tpl_ix = [], {}
        self.orig = {}   # template index -> the original string (for tests)

    def ref(self, game):
        if game not in self.ref_ix:
            self.ref_ix[game] = len(self.refs)
            self.refs.append(game)
        return self.ref_ix[game]

    def template(self, s):
        """Template index for s, or None when s holds no game text."""
        hits = self.m.find(s)
        if not hits:
            return None
        out, last = [], 0
        for a, b, game in hits:
            out.append(s[last:a])
            sub = s[a:b]
            # how this place writes the game's word: as the game does, or in lower/upper case, or with the
            # first letter changed (the module applies the same change when it fills the reference in)
            mode = ("" if sub == game else "l" if sub == game.lower() else "u" if sub == game.upper()
                    else "c" if sub == game[:1].upper() + game[1:] else "f" if sub == game[:1].lower() + game[1:]
                    else "")
            out.append(f"\x01{self.ref(game)}{mode}\x02")
            last = b
        out.append(s[last:])
        k = self.add_tpl("".join(out))
        self.orig.setdefault(k, s)
        return k

    def literal(self, s):
        k = self.add_tpl(s)
        self.orig.setdefault(k, s)
        return k

    def add_tpl(self, t):
        if t not in self.tpl_ix:
            self.tpl_ix[t] = len(self.tpl)
            self.tpl.append(t)
        return self.tpl_ix[t]


def expand_repeat(w):
    n, dx, dy = w["repeat"], w.get("repeat_dx", 0), w.get("repeat_dy", 0)
    out = []
    for i in range(n):
        js = re.sub(r"\{i([+-]\d+)?\}", lambda mm: str(i + int(mm.group(1) or 0)), json.dumps(w))
        e = json.loads(js)
        for k in ("repeat", "repeat_dx", "repeat_dy"):
            e.pop(k, None)
        e["rect"] = [e["rect"][0] + i * dx, e["rect"][1] + i * dy] + e["rect"][2:]
        out.append(e)
    return out


def items_to_flag(man):
    """The item page's selection: @sel:it -> flag "its" (a module cannot read @sel: in time).
    Rows set the flag; page_binds open the page and the back buttons' targets."""
    for d in man["derived"]:
        for k in ("a", "b", "select", "then", "else"):
            if d.get(k) == "@sel:it":
                d[k] = "@flag:its"
        for k in ("all_nonzero", "any_nonzero"):
            if k in d:
                d[k] = ["@flag:its" if x == "@sel:it" else x for x in d[k]]
        if "terms" in d:
            d["terms"] = [["@flag:its"] + t[1:] if t[0] == "@sel:it" else t for t in d["terms"]]
    man["flags"]["its"] = -1
    man["actions"]["itsel"] = {"kind": "flag", "flag": "its", "value": "$payload"}
    binds = [{"point": "it.any", "equals": 1, "when_equal": {"page": "it", "transition": "none"}}]
    for p in man["pages"]:
        for w in p["widgets"]:
            if w.get("bind") == "@sel:it":
                w["bind"] = "@flag:its"
            if w.get("select_group") == "it":
                del w["select_group"]
                w["on_tap"] = "itsel"
            if p["id"] == "it" and w.get("type") == "button" and str(w.get("on_tap", "")).startswith("go_"):
                target = man["actions"][w["on_tap"]]["page"]
                if target.startswith("ic") or target == "items":
                    v = -100 if target == "items" else -2 - int(target[2:])
                    name = f"itback_{target}"
                    man["actions"][name] = {"kind": "flag", "flag": "its", "value": v}
                    w["on_tap"] = name
                    dn = f"itb.{target}"
                    if not any(d["name"] == dn for d in man["derived"]):
                        man["derived"].append({"name": dn, "cmp": "eq", "a": "@flag:its", "b": v})
                    binds.append({"point": dn, "equals": 1,
                                  "when_equal": {"page": target, "transition": "none"}})
    man["page_binds"] = man.get("page_binds", []) + binds
    assert "@sel:it" not in json.dumps(man)


# Keys that place a widget (repeat templates, scrolling lists): kept on every converted widget.
PLACE = ("repeat", "repeat_dx", "repeat_dy", "repeat_cols", "repeat_row_dy", "scroll", "y_bind", "x_bind",
         "hide_bind", "keep_min", "keep_max")
KEYED = {"@flag:its": ("it", "its", 0), "dx.num": ("dx", "dxh", 1)}


def textify(man, text_json):
    items_to_flag(man)
    T = Texts(Matcher(dictionary(text_json)))
    page_slots = {}
    keyed = {}
    rowsets = {}
    derived_names = {d["name"] for d in man["derived"]}
    used_tables = set()

    def derive(d):
        if d["name"] not in derived_names:
            derived_names.add(d["name"])
            man["derived"].append(d)
        return d["name"]

    def slot_label(w, tpl, x, y):
        lab = {"type": "label", "rect": [x, y, 0, 0], "bind_text": f"s{tpl}",
               "text_scale": w.get("text_scale", 3), "color": w.get("color", "#FFE6ECF2")}
        for k in ("align", "need_bind", "wrap_width", "max_lines", "line_gap") + PLACE:
            if k in w:
                lab[k] = w[k]
        return lab

    for p in man["pages"]:
        if p.get("title") and T.m.find(p["title"]):
            p["title"] = p["id"]
        out, slots = [], []
        for w0 in p["widgets"]:
            ws = [w0]
            if "repeat" in w0 and "{i" in w0.get("text", "") and T.m.find(w0.get("text", "")):
                ws = expand_repeat(w0)
            for w in ws:
                t = w.get("type")
                wrap = w.pop("_wrap", None)
                if t == "label" and w.get("text"):
                    tpl = T.template(w["text"])
                    if tpl is not None:
                        del w["text"]
                        w["bind_text"] = f"s{tpl}"
                        slots.append(tpl)
                    out.append(w)
                elif t == "button" and w.get("text"):
                    tpl = T.template(w["text"])
                    out.append(w)
                    if tpl is not None:
                        txt, w["text"] = w["text"], ""
                        x, y, rw, rh = w["rect"]
                        sc = w.get("text_scale", 3)
                        if w.get("align") == "center":
                            lab = slot_label(w, tpl, x + rw // 2, y + (rh - sc * 5) // 2)
                        else:
                            ins = w.get("text_inset", 12)
                            lab = slot_label(w, tpl, x + ins, y + ins)
                            lab.pop("align", None)
                        out.append(lab)
                        slots.append(tpl)
                elif t == "value" and "table" in w:
                    name = w["table"]
                    rows = man["tables"][name]
                    tpls = [T.template(s) if s else None for s in rows]
                    has_game = any(x is not None for x in tpls)
                    assert not (w.get("text") and T.m.find(w["text"])), w
                    if not has_game and not wrap:
                        out.append(w)
                        used_tables.add(name)
                        continue
                    bind = w["bind"]
                    lab = {"type": "label", "rect": list(w["rect"]), "text_scale": w.get("text_scale", 3),
                           "color": w.get("color", "#FFE6ECF2")}
                    for k in ("align", "need_bind") + PLACE:
                        if k in w:
                            lab[k] = w[k]
                    if wrap:
                        width, nl, lh = wrap
                        lab["wrap_width"] = width
                        lab["max_lines"] = nl
                        if nl > 1:
                            lab["line_gap"] = lh - 5 * lab["text_scale"]
                    full = [(T.template(w.get("text", "") + s + w.get("suffix", "")) if s else None) for s in rows]
                    full = [(x if x is not None else (T.literal(w.get("text", "") + s + w.get("suffix", ""))
                                                       if s else None)) for x, s in zip(full, rows)]
                    if bind == "ci.{i}":
                        rs = rowsets.setdefault(p["id"], {})
                        rs[name] = [-1 if x is None else x for x in full]
                        lab["bind_text"] = f"tb.{name}.{{i}}"
                        out.append(lab)
                    elif bind in KEYED:
                        page, flag, mode = KEYED[bind]
                        assert p["id"] == page, (p["id"], name)
                        g = keyed.setdefault(page, {"page": page, "flag": flag, "mode": mode, "tables": {}})
                        g["tables"][name] = [-1 if x is None else x for x in full]
                        lab["bind_text"] = f"tb.{name}"
                        out.append(lab)
                    else:
                        # Index chosen by a derived value the module cannot read: one gated label per entry.
                        for i, x in enumerate(full):
                            if x is None:
                                continue
                            gi = derive({"name": f"tx.{bind}.{i}", "cmp": "eq", "a": bind, "b": i})
                            if "need_bind" in w:
                                gi = derive({"name": f"tx.{bind}.{i}.{w['need_bind']}",
                                             "all_nonzero": [gi, w["need_bind"]]})
                            e = dict(lab)
                            e["need_bind"] = gi
                            e["bind_text"] = f"s{x}"
                            out.append(e)
                            slots.append(x)
                else:
                    if w.get("text"):
                        assert not T.m.find(w["text"]), (p["id"], w)
                    out.append(w)
        p["widgets"] = out
        if slots:
            page_slots[p["id"]] = sorted(set(slots))
    man["tables"] = {k: v for k, v in man["tables"].items() if k in used_tables}
    for p in man["pages"]:
        if p["id"] == "live":
            p["widgets"] += [
                {"type": "label", "rect": [24, 1046, 0, 0], "text": "Game text found in your game files:",
                 "text_scale": 3, "color": "#FF6FB8D6"},
                {"type": "value", "rect": [700, 1046, 0, 0], "bind": "dsts.text", "max_bind": "dsts.text_total",
                 "max_sep": " of ", "text_scale": 3, "color": "#FFFF8A1F"}]
    man["dsts_text"] = {"v": 1, "refs": [f"{fnv(r):016x}" for r in T.refs], "tpl": T.tpl,
                        "pages": page_slots, "keyed": list(keyed.values()),
                        "rows": [{"page": pg, "tables": tb} for pg, tb in rowsets.items()],
                        "img": man.pop("dsts_img", {})}
    return man, T


def leftovers(man, text_json):
    """Game text still literal anywhere in the manifest (should be none)."""
    m = Matcher(dictionary(text_json))
    found = []

    shown = {"text", "title", "suffix", "max_sep", "_note", "name"}

    def walk(o, path, disp):
        if isinstance(o, dict):
            for k, v in o.items():
                if k in ("dsts_text", "derived", "actions", "flags", "persist_flags", "page_binds", "module"):
                    continue
                walk(v, path + "/" + k, disp or k in ("tables",) or (k in shown and not isinstance(v, (dict, list))))
        elif isinstance(o, list):
            for i, v in enumerate(o):
                walk(v, path + f"[{i}]", disp)
        elif isinstance(o, str) and disp:
            for a, b, g in m.find(o):
                found.append((path, o[a:b]))
    walk(man, "", False)
    return found
