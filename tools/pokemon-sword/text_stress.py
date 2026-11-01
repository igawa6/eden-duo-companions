#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Text-length stress test of the generated manifest, offline (no emulator).

For every label on every page, in every offered language and in a pseudo-locale (each companion
string padded to ~140 % of its longest real translation), the longest text the label can show is
measured with the package font's own advances and laid out the way the runtime does
(text_fit.py): fit_text shrink, word wrap, max_lines ellipsis. Static labels use their text_map;
module-published labels (bind_text) get the worst real values of their data source in that
language -- species/move/item/ability/nature/place names, descriptions, Pokédex category, the
companion's own strings (tools/pokemon-sword/strings.json) and the formats built from them.

Findings per label and language:
  truncated   text cut with an ellipsis (wrap + max_lines)
  overflow    wider than the label's wrap width / rect width, or past the panel edge
  floor       shrunk below the legibility floor (LEGIBILITY.md: essential cap >= 22 px -> text
              scale 5, secondary >= 18 px -> scale 4; cap = 5 * scale)
  overlap     two always-together labels on one page whose drawn boxes intersect
Each finding notes whether English has it too (pre-existing) or it is new in that language.

    SW_ROMFS_DIR=<extracted romfs> python3 tools/pokemon-sword/text_stress.py [--json out.json]
"""
import argparse
import itertools
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import text_fit  # noqa: E402
from gen_zone_names import read_dat, read_tbl  # noqa: E402

ROOT = HERE.parents[1]
import sw_variants  # noqa: E402
VARIANT = sw_variants.get()  # main() takes --variant
MANIFEST = ROOT / f"packages/{VARIANT.package_dir}/dualscreen/manifest.json"
import sw_strings  # noqa: E402
STRINGS = sw_strings.STRINGS  # game rows resolve from the romfs, like the module (sw_strings.py)
ROMFS = None
LANGS = ["en", "fr", "de", "it", "es", "ja", "ja-kana", "ko", "zh-Hans", "zh-Hant"]
LATIN = LANGS[:5]
FOLDER = {"en": "English", "fr": "French", "de": "German", "it": "Italian", "es": "Spanish",
          "ja": "JPN_KANJI", "ja-kana": "JPN", "ko": "Korean", "zh-Hans": "Simp_Chinese", "zh-Hant": "Trad_Chinese"}


def measure_as(loc):
    """Japanese / Korean / Chinese are drawn from the module's atlas (text_fit's metrics of the
    Rodin face / the shared system fonts), the rest (and the Latin pseudo-locale) in the package
    font."""
    text_fit.set_lang(loc if loc in text_fit.MODULE_LANGS else "en")


def join_cjk_spaces(t):
    """sw_text_fit.h JoinCjkSpaces: Chinese drops a space between two wide characters (the module
    publishes game text that way, sw_reader.h PubT)."""
    wide = lambda c: c and ("\u2e80" <= c <= "\u9fff" or "\uf900" <= c <= "\ufaff" or "\ufe30" <= c <= "\ufe4f"
                            or "\uff00" <= c <= "\uffef")
    return re.sub(r"(?<=(.)) +(?=(.))", lambda m: "" if wide(m.group(1)) and wide(m.group(2)) else m.group(0), t)


def flat(t, loc):
    """A game text as the reader publishes it on one line (line breaks -> spaces; Chinese joins)."""
    t = t.replace("\n", " ")
    return join_cjk_spaces(t) if loc in ("zh-Hans", "zh-Hant") else t
LOCALES = LANGS + ["pseudo"]
SECONDARY = {"#FF555555", "#FF9A9A9A"}  # LABEL_GREY, UNAV
TOP = 8                                  # worst candidates kept per data source
# Z-Moves (Gen 7) are in wazaname.dat but cannot occur in Sword: never a shown name.
Z_MOVES = set(range(622, 659)) | set(range(695, 704)) | {719} | set(range(723, 729))
EVO_ITEMS = (80, 81, 82, 83, 84, 85, 107, 108, 109, 110, 221, 233, 235, 252, 321, 322, 323, 324, 325, 326, 327,
             537, 646, 647, 849, 1109, 1110, 1111, 1112, 1113, 1114, 1115, 1116, 1117, 1253, 1254, 1582, 1592)
EVO_MOVES = (23, 102, 205, 246, 458)  # Stomp, Mimic, Rollout, Ancient Power, Double Hit
FIELD_ONLY = ("res.fly_not_now", "res.fly_locked", "res.bad_dest", "res.repel", "res.not_usable", "res.no_item",
              "res.target_changed", "res.not_free", "res.bike_blocked", "res.progression")


def pseudo(text):
    """~140 % of the text's width with accented letters (the same rule gen_manifest.py uses)."""
    if not text.strip():
        return text
    target = text_fit.width(text, 5) * 1.4
    pad, out, i = "ÀÉÎÕÜ àéîõü ", text, 0
    while text_fit.width(out, 5) < target:
        out += pad[i % len(pad)]
        i += 1
    return out.rstrip()


def S(key, loc):
    """What the module shows for a strings.json key (a game row: its line from the romfs)."""
    if loc == "pseudo":
        return pseudo(max((sw_strings.resolve(key, c, ROMFS) for c in LATIN), key=lambda t: text_fit.width(t, 5)))
    return sw_strings.resolve(key, loc, ROMFS)


class Game:
    """Message tables of the romfs, per language, with the control codes stripped."""

    def __init__(self, romfs):
        self.base = Path(romfs) / "bin/message"
        self.cache = {}

    def lines(self, loc, file, folder="common"):
        lang = FOLDER["de" if loc == "pseudo" else loc]  # the pseudo-locale uses the German data
        key = (lang, folder, file)
        if key not in self.cache:
            raw = read_dat(self.base / lang / folder / file)
            clean = []
            for t in raw:
                t = re.sub("\0.*", "", t, flags=re.S)  # a {VAR} line is not shown text
                if loc in text_fit.JA_LANGS or loc == "ko":
                    t = t.replace("\u3000", " ")  # the module publishes U+3000 as a break (sw_reader.h PubT)
                clean.append(t)
            self.cache[key] = clean
        return self.cache[key]

    def wild_places(self, loc):
        """Town Map names of the places with wild Pokemon (sw_route_encounters.h's zones; the two
        unlabelled zones and Route 10's second one show their _01 sibling's name), worst first."""
        measure_as(loc)
        zones = re.findall(r"// (z_\w+)", (ROOT / "native/modules" / VARIANT.route_header).read_text())
        alias = {"z_r0202": "z_r0201", "z_d0103": "z_d0101", "z_r1002": "z_r1001"}
        lang = FOLDER["de" if loc == "pseudo" else loc]
        labels = read_tbl(self.base / lang / "script/place_name.tbl")
        lines = self.lines(loc, "place_name.dat", folder="script")
        index = {label: i for i, label in enumerate(labels)}
        names = [lines[index[f"PLACENAME_{alias.get(z, z).upper()}_01"]] for z in zones]
        names = sorted(set(names), key=lambda t: -text_fit.width(t, 4))[:TOP]
        return [pseudo(t) for t in names[:2]] + names if loc == "pseudo" else names

    def ev_yields(self):
        import struct
        data = (self.base.parent / "pml/personal/personal_total.bin").read_bytes()
        out = set()
        for sp in range(1, 899):
            ev = struct.unpack_from("<H", data, sp * 0xB0 + 0x0A)[0]
            out.add(tuple((ev >> (2 * i)) & 3 for i in range(6)))
        return sorted(c for c in out if any(c))

    def worst(self, loc, file, scale=5, folder="common", one_line=True, limit=None, skip=()):
        measure_as(loc)
        lines = [t for i, t in enumerate(self.lines(loc, file, folder)[:limit]) if t.strip() and i not in skip]
        if one_line:
            lines = [flat(t, loc) for t in lines]
        lines.sort(key=lambda t: -text_fit.width(t, scale))
        out = lines[:TOP]
        return [pseudo(t) for t in out[:2]] + out if loc == "pseudo" else out


def break_hyphens(text, scale, width):
    """sw_text_fit.h BreakLongHyphenated."""
    return " ".join(w.replace("-", "- ") if text_fit.width(w, scale) > width and "-" in w[:-1] else w
                    for w in text.split(" "))


# Japanese and Chinese break between any two characters (the runtime splits a too-wide run by
# character), except before closing punctuation / small kana / the long-vowel mark and after an
# opening bracket (kinsoku shori, JIS X 4051 in short; the Chinese rules are the same set).
NO_LINE_START = set("、。，．・：；？！ー）」』】〉》ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮ…％”’")
NO_LINE_END = set("（「『【〈《“‘")


def kinsoku(lines):
    """A line of a split Japanese text that starts or ends where Japanese typesetting forbids."""
    return any(l and l[0] in NO_LINE_START for l in lines[1:]) or any(l and l[-1] in NO_LINE_END for l in lines[:-1])


def descr(lines, loc="en"):
    """Descriptions the reader shows as one paragraph (sw_party_details OneLine; Chinese joined)."""
    return [flat(t, loc) for t in lines]


def candidates(bind, loc, game):
    """The worst texts a module-published label can show in a language ([] = language-neutral)."""
    k = re.sub(r"\{i\}|\d+", "N", bind)
    s = lambda key: S(key, loc)
    species = lambda: game.worst(loc, "monsname.dat", limit=899) + [s("egg")]
    moves = lambda: game.worst(loc, "wazaname.dat", skip=Z_MOVES)
    items = lambda: game.worst(loc, "itemname.dat")
    places = lambda: game.worst(loc, "place_name.dat", folder="script")
    if k.endswith((".description",)) and ("move" in k or "move_detail" in k):
        return descr(game.worst(loc, "wazainfo.dat", one_line=False), loc)
    if k == "sw.dex.detail.description":
        return descr(game.worst(loc, VARIANT.dex_comment_file, one_line=False), loc)
    if k in ("sw.bag.detail.description", "sw.battle.ball.description"):
        return descr(game.worst(loc, "iteminfo.dat", one_line=False), loc)
    if k == "sw.party.detail.ability_description":
        return descr(game.worst(loc, "tokuseiinfo.dat", one_line=False), loc)
    if k.startswith("sw.battle.field.row.N.description"):
        return descr([t for i, t in enumerate(game.lines(loc, "btl_state.dat")) if i % 2 == 1 and t.strip()], loc)
    if k.startswith("sw.battle.field.row.N.name"):
        names = [t for i, t in enumerate(game.lines(loc, "btl_state.dat")) if i % 2 == 0 and t.strip()]
        names.sort(key=lambda t: -text_fit.width(t, 5))
        side = max((s("field.your_side"), s("field.opposing_side")), key=lambda t: text_fit.width(t, 5))
        return [side + n for n in names[:TOP]] + names[:TOP] + [s("field.no_weather"), s("field.no_terrain")]
    if "turns_text" in k:
        return [s("field.ongoing"), S("field.turns", loc).replace("%d", "8"), S("field.turn", loc).replace("%d", "1")]
    if k.endswith("moveN.name") or k == "sw.battle.move_detail.name":
        return moves() + game.worst(loc, "gwazaname.dat")
    if k.endswith((".item_name",)) or k in ("sw.bag.row.N.name", "sw.bag.detail.name", "sw.battle.ball.name"):
        return items() + [s("common.none")]
    if k in ("sw.map.wild.place", "sw.route.name"):
        return game.wild_places(loc)
    if k == "sw.route.N.name":
        return species()
    if k in ("sw.route.N.method", "sw.route.N.method_after_icon", "sw.route.N.methodN"):
        # the method pairs / third methods 1.3.2 really has (sw_route.h Shown: 0 Wanders, 1 Flying,
        # 2 Surfing, 3 Grass, 4 Tree, 5 Fishing); after the glyph: first method Surfing/Tree/Fishing
        rate = lambda m: S("route.rate", loc).replace("%s", s(f"route.method.{m}"), 1).replace("%d", "88", 1).replace("%%", "%")
        if k.endswith("methodN"):
            return [rate(m) for m in (2, 3, 4, 5)]
        pairs = [(0, 1), (0, 2), (0, 3), (0, 4), (0, 5), (1, 2), (1, 3), (2, 0), (2, 3), (2, 5), (3, 4), (3, 5)]
        if k.endswith("after_icon"):
            return [rate(a) + " · " + rate(b) for a, b in pairs if a in (2, 4, 5)] + [rate(m) for m in (2, 4, 5)]
        return [rate(a) + " · " + rate(b) for a, b in pairs if a not in (2, 4, 5)] + [rate(m) for m in (0, 1, 3)]
    if k == "sw.route.N.level":
        return [S("route.levels", loc).replace("%d", "60", 1).replace("%d", "65", 1), S("evo.level", loc).replace("%d", "60")]
    if k == "sw.route.weather.name":
        return [s(f"route.weather.{i}") for i in range(9)]
    if k == "sw.map.wild.more":
        return ["+99"]
    if k in ("sw.map.selected.name", "sw.location.zone_name", "sw.fly.row.N.name"):
        return places() + [p + " (2)" for p in places()[:2]]
    if k in ("sw.fly.row.N.region", "sw.location.region"):
        return [s(x) for x in ("region.galar", "region.wild_area", "region.armor", "region.crown")]
    if k == "sw.bag.pocket_name":
        return [s(f"bag.pocket.{i}") for i in range(9)]
    if k == "sw.bag.overflow_text":
        return [S("bag.overflow", loc).replace("%d", "99")]
    if k in ("sw.party.detail.ability", "sw.dex.detail.hidden_ability"):
        return game.worst(loc, "tokusei.dat")
    if k == "sw.dex.detail.abilities":
        a = game.worst(loc, "tokusei.dat")[:3]
        return [x + " / " + y for x, y in itertools.permutations(a, 2)]
    if k == "sw.party.detail.nature":
        return game.worst(loc, "seikaku.dat")
    if k == "sw.dex.detail.category":
        return game.worst(loc, "zkn_type.dat")
    if k in ("sw.dex.detail.height", "sw.dex.detail.weight"):
        return game.worst(loc, "zkn_height.dat" if "height" in k else "zkn_weight.dat")
    if k == "sw.party.detail.gender_text":
        return [s("gender.male"), s("gender.female"), s("gender.none")]
    if k == "sw.dex.detail.gender":
        dec, pct = s("num.decimal"), s("num.percent")
        ratio = S("gender.ratio", loc).replace("%s", "87" + dec + "5" + pct, 1).replace("%s", "12" + dec + "5" + pct, 1)
        return [ratio, s("gender.none"), s("gender.male_only"), s("gender.female_only")]
    if k == "sw.dex.detail.egg_groups":
        groups = [s(x) for x in STRINGS if x.startswith("egg.")]
        groups.sort(key=lambda t: -text_fit.width(t, 4))
        return [a + " / " + b for a, b in itertools.permutations(groups[:4], 2)]
    if k == "sw.dex.detail.growth":
        return [s(x) for x in STRINGS if x.startswith("growth.")]
    if k == "sw.dex.detail.ev_yield":  # every yield the personal table really has (sw_dex_info EvYield)
        keys = ("stat.hp", "stat.attack", "stat.defense", "stat.speed", "stat.sp_atk", "stat.sp_def")
        return [", ".join(f"{v} {s(keys[i])}" for i, v in enumerate(c) if v) for c in game.ev_yields()]
    if k == "sw.dex.detail.hatch":
        return [S("dex.hatch_cycles", loc).replace("%d", "120")]
    if k == "sw.party.detail.status_text":
        return [s(x) for x in STRINGS if x.startswith("status.")]
    if k.endswith("category_text"):
        return [s("move.cat.status"), s("move.cat.physical"), s("move.cat.special")]
    if k.endswith("catpow_text"):
        return [c + " · 250" for c in (s("move.cat.status"), s("move.cat.physical"), s("move.cat.special"))]
    if k.endswith("effectiveness_text"):
        return [s("mt.super"), s("mt.not_very"), s("common.no_effect"), *game.lines(loc, "btl_app.dat")[18:19]]
    if re.search(r"\.(weak|resist|immune)N?\.text$", k) or k.endswith(("weak{i}.text", "resist{i}.text", "immune{i}.text")):
        return [s(f"type.{i}") + " ×½" for i in range(18)]
    if k == "sw.party.detail.matchup_note":
        return [s("party.matchup_note")]
    toasts = [s(x) for x in STRINGS if x.startswith(("res.", "ref.", "direct."))]
    if k == "sw.field.status":     # the module splits a two-line refusal at its newline
        return [t.split("\n")[0] for t in toasts]
    if k == "sw.field.statusN":
        return [t.split("\n")[1] for t in toasts if "\n" in t]
    if k == "sw.direct.status":
        return [s(x) for x in STRINGS if x.startswith(("res.", "ref.", "direct.")) and x not in FIELD_ONLY]
    if re.match(r"sw\.battle\.switch_suggest\.N\.reasonN", k):
        m = game.worst(loc, "wazaname.dat", skip=Z_MOVES)[:2]
        types = sorted((s(f"type.{i}") for i in range(18)), key=lambda t: -text_fit.width(t, 5))
        return ([s("switch.weak_to") + types[0], s("switch.immune"), s("switch.resists"), s("switch.neutral"),
                 s("switch.low_hp"), s("switch.no_advantage")] + [x + ": ×4" for x in m])
    if k.endswith("accev"):
        return [s("stage.accuracy") + "+6   " + s("stage.evasion") + "-6"]
    if k.endswith("level_text"):
        return [s("level.prefix") + "100"]
    if k.endswith("dex_no_text"):
        return [S("dex.number", loc).replace("%03d", "400")]
    if k.endswith("evo.N.method_text"):  # the items / moves Sword's evolution table names
        it = sorted((game.lines(loc, "itemname.dat")[i] for i in EVO_ITEMS), key=lambda t: -text_fit.width(t, 4))[:2]
        mv = sorted((game.lines(loc, "wazaname.dat")[i] for i in EVO_MOVES), key=lambda t: -text_fit.width(t, 4))[:2]
        lv = S("evo.level", loc).replace("%d", "100")
        out = [lv + s("evo.hold") + it[0] + s("evo.night"), S("evo.trade_holding", loc).replace("%s", it[0]),
               s("evo.party_species"), s("evo.trade_shelmet"), s("evo.move_type"), lv + s("evo.dark_party"),
               lv + s("evo.version"), it[0] + s("evo.female"), mv[0]]
        return [break_hyphens(t, 4, 168) for t in out]  # == the module (sw_text_fit.h)
    if k.endswith(".name") and any(x in k for x in ("party", "battle", "dex", "raid", "target", "bench", "switch_suggest")):
        return species()
    if k == "sw.bag.use.target_name":
        return species()
    return []  # numbers, player names, build label: language-neutral


def values(page):
    out = []

    def walk(o):
        if isinstance(o, dict):
            if o.get("type") == "value" and o.get("bind"):
                out.append(o)
            for x in o.values():
                walk(x)
        elif isinstance(o, list):
            for x in o:
                walk(x)
    walk(page["widgets"])
    return out


def shown_in(w, loc):
    """Widgets gated on one language (gen_manifest V_lang / the party header runs) only exist in
    that language; the pseudo-locale is index 10 of a stress build."""
    m = re.match(r"d\.lang(\d+)\.", w.get("need_bind") or "")
    if m:
        return int(m.group(1)) == LOCALES.index(loc)
    # a derived gate on the language in effect ("... && d.lang != 5 && d.lang != 6")
    expr = DERIVED.get(w.get("need_bind") or "", "")
    i = LOCALES.index(loc)
    eq = [int(n) for n in re.findall(r"d\.lang == (\d+)", expr)]
    ne = [int(n) for n in re.findall(r"d\.lang != (\d+)", expr)]
    return (not eq or i in eq) and i not in ne


def widgets(page):
    def walk(o, out):
        if isinstance(o, dict):
            if o.get("type") == "label":
                out.append(o)
            for v in o.values():
                walk(v, out)
        elif isinstance(o, list):
            for x in o:
                walk(x, out)
        return out
    return walk(page["widgets"], [])


def compatible(a, b):
    """Two labels checked for overlap: shown together for sure -- the same gate, one gate a
    refinement of the other ("d.party.hdr" / "d.party.hdr.avg"), or both ungated. Labels moved at
    run time (x_bind / y_bind) and alternatives at one anchor are left to the live renders."""
    if "y_bind" in a or "y_bind" in b or "x_bind" in a or "x_bind" in b:
        return False
    ga, gb = gate(a), gate(b)
    if ga == gb:
        return True
    # groupgate() gives every widget its own derived gate "<group> && (<own need>)": compare
    # what the gates stand for
    sa, sb = atoms(a.get("need_bind") or ""), atoms(b.get("need_bind") or "")
    if ga[1:] == gb[1:] and sa is not None and sb is not None and (sa <= sb or sb <= sa):
        return True
    na, nb = a.get("need_bind") or "", b.get("need_bind") or ""
    if ga[1:] == gb[1:] and na and nb and (na.startswith(nb + ".") or nb.startswith(na + ".")):
        return True
    return False


def gate(w):
    return tuple(w.get(k) for k in ("need_bind", "hide_bind", "hide_eq", "keep_min", "keep_max"))


def texts_of(w, loc, game):
    if "text_map" in w and w.get("text_bind") == "d.lang":
        tm = w["text_map"]
        if loc == "pseudo":
            measure_as("en")
            return [pseudo(max((tm[str(i)] for i in range(5)), key=lambda t: text_fit.width(t, 5)))]
        return [tm[str(LANGS.index(loc))]]
    if w.get("bind_text", "").startswith("sw.str."):  # a game line the module publishes (F13)
        key = w["bind_text"][len("sw.str."):]
        if loc == "pseudo":
            measure_as("en")
        return [S(key, loc)]
    if w.get("bind_text"):
        found = candidates(w["bind_text"], loc, game)
        if found:
            return found
        # language-neutral numbers: a wide plausible reading, for the overlap check
        b = w["bind_text"]
        for tail, sample in (("power_text", "250"), ("pp_text", "40/40"), ("hp_text", "714/714"),
                             ("level_text", "Lv.100"), ("count_text", "x999")):
            if b.endswith(tail):
                return [sample]
        return []
    return [w["text"]] if w.get("text") else []


# Tab captions are secondary text (LEGIBILITY.md: "inactive tabs" >= 18 px), also when selected.
TAB_KEYS = {k for k in STRINGS if k.startswith(("tab.", "party.tab.", "dex.tab."))} | {
    "battle.move", "battle.foe", "battle.own", "battle.field"}


def tab_text():
    return {S(k, "en") for k in TAB_KEYS}


TAB_TEXT = set()  # tab_text(), once the romfs is known (run)


def floor_scale(w):
    """Smallest legible text scale for this label (cap = 5 * scale)."""
    if (w.get("text_scale", 1) <= 4 or w.get("color") in SECONDARY
            or w.get("text_map", {}).get("0") in TAB_TEXT or w.get("bind_text", "")[len("sw.str."):] in TAB_KEYS):
        return 4   # secondary: >= 18 px -> 20 px
    return 5       # essential: >= 22 px -> 25 px


def extent(w, scale, lines):
    x, y = w["rect"][0], w["rect"][1]
    width = max((text_fit.width(l, scale) for l in lines), default=0)
    align = w.get("align", "left")
    x0 = x - width // 2 if align == "center" else x - width if align == "right" else x
    pitch = scale * 5 + (w.get("line_gap", -1) if w.get("line_gap", -1) >= 0 else scale * 3)
    return (x0, y, x0 + width, y + scale * 5 + pitch * (len(lines) - 1))


DERIVED = {}


def atoms(name, depth=0):
    """The conjunction a gate stands for, as a set of terms (derived gates expanded); None when
    it is not a plain conjunction (an || / comparison makes containment undecidable here)."""
    if not name:
        return frozenset()
    expr = DERIVED.get(name)
    if expr is None or depth > 12:
        return frozenset([name])
    if "||" in expr:
        return None
    out = set()
    for term in expr.split("&&"):
        term = term.strip().strip("()").strip()
        if re.fullmatch(r"!?[\w.@:]+", term):
            sub = atoms(term, depth + 1) if not term.startswith("!") else frozenset([term])
            if sub is None:
                return None
            out |= sub
        else:
            out.add(term)
    return frozenset(out)


def run(romfs):
    global ROMFS, TAB_TEXT
    ROMFS = sw_strings.Romfs(romfs)
    TAB_TEXT = tab_text()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    DERIVED.clear()
    DERIVED.update({d["name"]: d.get("expr", "") for d in manifest.get("derived", []) if isinstance(d, dict)})
    game = Game(romfs)
    findings = []
    for page in manifest["pages"]:
        labels = widgets(page)
        boxes = {loc: [] for loc in LOCALES}
        for idx, w in enumerate(labels):
            what = w.get("bind_text") or (w.get("text_map", {}).get("0")) or w.get("text", "")
            for loc in LOCALES:
                if not shown_in(w, loc):
                    continue
                worst = None
                texts = texts_of(w, loc, game)
                measure_as(loc)
                for text in texts:
                    text_fit.BROKEN.clear()
                    scale, lines, truncated, overflow = text_fit.fit(w, text)
                    if text_fit.BROKEN and len(lines) > 1 and (loc not in text_fit.CHAR_BREAK_LANGS or kinsoku(lines)):
                        overflow = max(overflow, 1)  # a word split across lines (Japanese / Chinese: at a forbidden place)
                    box = extent(w, scale, lines)
                    edge = max(0, box[2] - 1228, 12 - box[0])
                    bad = (truncated, overflow > 0 or edge > 0, scale < floor_scale(w) and scale < w.get("text_scale", 1))
                    score = (sum(bad), overflow + edge, -scale, text_fit.width(text, w.get("text_scale", 1)))
                    if worst is None or score > worst[0]:
                        worst = (score, text, scale, lines, truncated, overflow + edge, box)
                if worst is None:
                    continue
                _, text, scale, lines, truncated, over, box = worst
                boxes[loc].append((idx, box, gate(w), what))
                issues = []
                if truncated:
                    issues.append("truncated")
                if over > 0:
                    issues.append(f"overflow {over}px")
                if scale < floor_scale(w) and scale < w.get("text_scale", 1):
                    issues.append(f"floor (scale {scale} = cap {scale * 5}px)")
                band = w.get("text_center_h", 0)
                if band and box[3] - box[1] > band:
                    issues.append(f"overflow-v {box[3] - box[1] - band}px")
                if issues:
                    findings.append({"page": page["id"], "widget": idx, "label": what, "locale": loc,
                                     "issues": issues, "text": text, "shown": lines, "scale": scale,
                                     "rect": w["rect"], "wrap": w.get("wrap_width", 0),
                                     "max_lines": w.get("max_lines", 0)})
        # number widgets (value): drawn as their widest plausible reading + suffix
        for idx2, v in enumerate(values(page)):
            for loc in LOCALES:
                if not shown_in(v, loc):
                    continue
                measure_as(loc)
                sample = {"sw.party.able": "6", "sw.party.count": "6", "sw.party.avg_level": "100"}
                text = sample.get(v["bind"], "888") + v.get("suffix", "")
                scale = v.get("text_scale", 1)
                labels.append(v)
                boxes[loc].append((len(labels) - 1, extent(v, scale, [text]), gate(v), v["bind"]))
        # overlaps between labels shown together (same gate), per locale
        seen_en = set()
        for loc in LOCALES:
            items = boxes[loc]
            for (i, a, ga, wa), (j, b, gb, wb) in itertools.combinations(items, 2):
                if i == j or ga == ("same-anchor",) or gb == ("same-anchor",):
                    continue
                if (labels[i]["rect"][:2] == labels[j]["rect"][:2]) or not compatible(labels[i], labels[j]):
                    continue
                if a[0] < b[2] - 2 and b[0] < a[2] - 2 and a[1] < b[3] - 2 and b[1] < a[3] - 2:
                    if loc == "en":
                        seen_en.add((i, j))
                    findings.append({"page": page["id"], "widget": i, "label": wa, "locale": loc,
                                     "issues": [f"overlap with #{j} {wb}"], "text": "", "shown": [], "scale": 0,
                                     "rect": list(a), "wrap": 0, "max_lines": 0})
    # mark what English already has
    kinds = lambda f: {i.split(" ")[0] if not i.startswith("overlap") else i for i in f["issues"]}
    english = {}
    for f in findings:
        if f["locale"] == "en":
            english.setdefault((f["page"], f["widget"]), set()).update(kinds(f))
    for f in findings:
        f["also_en"] = kinds(f) <= english.get((f["page"], f["widget"]), set())
    return findings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json")
    ap.add_argument("--all", action="store_true", help="also list findings English shares")
    sw_variants.add_argument(ap)
    args = ap.parse_args()
    global VARIANT, MANIFEST
    VARIANT = sw_variants.get(args.variant)
    MANIFEST = ROOT / f"packages/{VARIANT.package_dir}/dualscreen/manifest.json"
    romfs = os.environ.get("SW_ROMFS_DIR")
    if not romfs:
        raise SystemExit("SW_ROMFS_DIR=<extracted romfs> is required (game names per language)")
    findings = run(romfs)
    if args.json:
        Path(args.json).write_text(json.dumps(findings, ensure_ascii=False, indent=1), encoding="utf-8")
    shown = [f for f in findings if args.all or not f["also_en"]]
    by = {}
    for f in shown:
        by.setdefault((f["page"], f["widget"], f["label"]), []).append(f)
    for (page, idx, label), fs in sorted(by.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        print(f"[{page} #{idx}] {label}")
        for f in fs:
            tag = " (en too)" if f["also_en"] else ""
            print(f"   {f['locale']:6} {', '.join(f['issues'])}{tag}: {f['text'][:70]!r} -> {f['shown']}")
    counts = {}
    for f in findings:
        counts[f["locale"]] = counts.get(f["locale"], 0) + 1
    print("findings per locale:", counts, "| new vs English:",
          {loc: sum(1 for f in findings if f["locale"] == loc and not f["also_en"]) for loc in LOCALES})


if __name__ == "__main__":
    main()
