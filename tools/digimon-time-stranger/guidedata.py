# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""The guide's data for the generator, with no game text in this repository.

The guide's own notes (missions, side quests, dex, locations, bosses) are kept in
guide_data.tok.json. Every piece of game text inside them (a Digimon, place, item or mission name, a game
sentence) is stored as a token, never as the text:  U+27E6 <16 hex digits> <case> U+27E7, where the hex is
the FNV-1a 64 hash of the cleaned game string (public_text.clean / fnv) and <case> is g (as the game writes
it), l (lower case), u (upper case), c (first letter upper case) or f (first letter lower case).

At build time the tokens are filled in from text122.json, which tools/dump_text.py makes from the builder's
own copy of the game. The built package then takes the game text out again (public_text.textify), so it ships
none either.

It also holds an index of identifiers only ("gameindex": which item ids sit in which item group, which map
and field ids make up each area). Prices, item effects, chests, wild Digimon and rewards are not in it: the
native module reads them from the player's own game tables at run time.

If the original data modules (questdata.py, dexdata.py, dexloc.py, bosscards.py, gamedata.json) sit next to
this file, they are used as they are (the author's working copy); gamedata.json is reduced to the index.
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
TOK_FILE = os.path.join(HERE, "guide_data.tok.json")
TOKEN = re.compile("⟦([0-9a-f]{16})([glcuf])⟧")
NAMES = {"questdata": ["MAIN", "SIDE", "TIP_PARTS", "ATTR_CHART", "ELEMENTS", "SIDE_REMAINING"],
         "dexdata": ["DEXGROUPS", "ATTR", "ROWS"], "dexloc": ["LOC"], "bosscards": ["BOSSES"]}
GT_SHEETS = ["text/quest_title:Sheet1", "text/quest_step:Sheet1", "text/quest_client:Sheet1",
             "text/quest_outline:Sheet1", "text/main_quest_title:Sheet1", "text/main_quest_step:Sheet1",
             "text/item_name:Sheet1", "text/item_explanation:Sheet1", "text/char_name:Sheet1",
             "text/digimon_profile:Sheet1", "text/field_name:Sheet1", "text/worldmap_place_name:Sheet1"]


def text_json():
    return os.environ.get("DSTS_TEXT_JSON") or os.path.join(HERE, "..", "data", "text122.json")


def have_originals():
    return all(os.path.exists(os.path.join(HERE, f"{m}.py")) for m in NAMES) and os.path.exists(
        os.path.join(HERE, "gamedata.json"))


def encode(o):
    """JSON form that keeps tuples and integer dict keys."""
    if isinstance(o, tuple):
        return {"__t": [encode(x) for x in o]}
    if isinstance(o, list):
        return [encode(x) for x in o]
    if isinstance(o, dict):
        if all(isinstance(k, str) for k in o):
            return {k: encode(v) for k, v in o.items()}
        return {"__d": [[encode(k), encode(v)] for k, v in o.items()]}
    return o


def decode(o, s):
    if isinstance(o, str):
        return s(o)
    if isinstance(o, list):
        return [decode(x, s) for x in o]
    if isinstance(o, dict):
        if "__t" in o and len(o) == 1:
            return tuple(decode(x, s) for x in o["__t"])
        if "__d" in o and len(o) == 1:
            return {decode(k, s): decode(v, s) for k, v in o["__d"]}
        return {decode(k, s) if isinstance(k, str) else k: decode(v, s) for k, v in o.items()}
    return o


def game_strings():
    """hash -> cleaned game string, for every string the matcher can reference."""
    import public_text
    d = public_text.dictionary(text_json())
    return {public_text.fnv(c): c for c in d.values()}


def fill(s, table):
    def one(m):
        g = table.get(int(m.group(1), 16))
        if g is None:
            raise SystemExit("guidedata: a game string is missing from text122.json. Dump it from game update "
                             "1.2.2 with tools/dump_text.py.")
        mode = m.group(2)
        return (g if mode == "g" else g.lower() if mode == "l" else g.upper() if mode == "u"
                else g[:1].upper() + g[1:] if mode == "c" else g[:1].lower() + g[1:])
    return TOKEN.sub(one, s)


def build_index(gd, field_names, dex_numbers):
    """The identifiers the guide needs from the author's game tables (no values, no text):
    itemcats  [[group label, [item id, ...]], ...]
    areas     [[name key, [treasure map id, ...], [field id, ...]], ...], one per area name, in map order.
    Field ids of 30000 and up (the Outer Dungeon's challenge rooms) and fields whose only wild Digimon is
    Kuramon (a quest, not wild) are left out, as in the guide's area list."""
    itemcats = [[cat, [it["id"] for it in lst]] for cat, lst in gd.get("itemcats", [])]
    merged, order = {}, []
    for m, name, _its in gd.get("treasure", []):
        if name not in merged:
            merged[name] = [m + 10000, [], []]
            order.append((m + 10000, name))
        merged[name][1].append(m)
    for fid, nums in gd.get("wild", {}).items():
        fid = int(fid)
        name = field_names.get(fid)
        if not name or fid >= 30000:
            continue
        if not [n for n in nums if n != 1 and n in dex_numbers]:
            continue
        if name not in merged:
            merged[name] = [fid, [], []]
            order.append((fid, name))
        merged[name][2].append(fid)
    order.sort()
    return {"itemcats": itemcats, "areas": [merged[nm] for _, nm in order]}


def load():
    """All guide data: {module: {name: value}}, plus "gameindex" and "gametext"."""
    t = json.load(open(text_json(), encoding="utf8"))
    if have_originals():
        import importlib
        out = {m: {n: getattr(importlib.import_module(m), n) for n in names} for m, names in NAMES.items()}
        gd = json.load(open(os.path.join(HERE, "gamedata.json"), encoding="utf8"))
        fields = {int(k): v for k, v in t.get("text/field_name:Sheet1", []) if v}
        out["gameindex"] = build_index(gd, fields, set(out["dexdata"]["ROWS"]))
        # The package shows the game's own objective lists and reward tables (read at run time), so the
        # author's own step notes and reward notes are not kept.
        q = out["questdata"]
        q["MAIN"] = [(m[0], "", m[2]) for m in q["MAIN"]]
        q["SIDE"] = [tuple(x[:3]) + ("", x[4], "") + tuple(x[6:]) for x in q["SIDE"]]
    else:
        tok = json.load(open(TOK_FILE, encoding="utf8"))
        table = game_strings()
        out = decode(tok, lambda s: fill(s, table))
    out["gametext"] = {k: t[k] for k in GT_SHEETS if k in t}
    return out
