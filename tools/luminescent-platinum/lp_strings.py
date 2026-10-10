#!/usr/bin/env python3
"""The companion's own words and the game labels it borrows, in every language the mods add.

One registry for the manifest and the module:

- lp_strings.tsv (next to this file) is our translation table: one row per string the game has no
  text for (class D), or whose game label needs rewording or a template (class C), in en, fr, de,
  es-ES, es-419, pt-BR, ko, zh-Hans and zh-Hant, with notes and review flags. Its `use_label`
  column lists the variants whose game label already reads exactly like our row (after the
  normalisation below), so the module shows the game's own text there.
- LABEL_KEYS below are the game labels used as they are (class B): the module reads them from the
  language's message tables.

The manifest binds every literal through t(key) (gen_manifest.py) to lp.t.<key>, which the module
publishes per language: the game label (B, and C where use_label lists the variant), else our row,
else English. write_header() compiles all of it into native/modules/lp_strings_data.h.

    python3 tools/luminescent-platinum/lp_strings.py --labels <dir of decoded message tables>
        recomputes the use_label column from the decoded LP and mod tables (dump_mods.py output:
        en.tsv, common.tsv, mod_<mod>.tsv)
"""

from __future__ import annotations

import argparse
import csv
import re
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
TSV = HERE / "lp_strings.tsv"
# Italian and Japanese, the two languages only vanilla Brilliant Diamond has (no Luminescent Platinum
# mod adds them): key, it, ja, notes. A row here overrides English for that language; a game label
# row (LABEL_KEYS) given a text here shows it instead of the label in that language.
VANILLA_TSV = HERE / "lp_strings_vanilla.tsv"
VANILLA_COLUMNS = ["it", "ja"]

# Columns of the table, and lp_lang::Variant order in the module (ja and it come from
# lp_strings_vanilla.tsv, else English).
COLUMNS = ["en", "fr", "de", "es-ES", "es-419", "pt-BR", "ko", "zh-Hans", "zh-Hant"]
VARIANTS = ["en", "pt-BR", "fr", "de", "es-ES", "es-419", "ko", "zh-Hans", "zh-Hant", "ja", "it"]

UPPER_FIRST = 1  # upper-case the first letter (the es Pokétch app names sit mid-sentence in the mod)

# Game labels used as they are, in every language: key, English default, message table, label.
LABEL_KEYS = [
    ("hp", "HP", "ss_status", "SS_status_28"),
    ("pp", "PP", "ss_status", "SS_status_42"),
    ("nav_party", "Party", "ss_box", "SS_box_000"),
    ("nav_dex", "Pokédex", "ss_pokedex", "SS_pokedex_134"),
    ("dex_sinnoh", "Sinnoh Pokédex", "ss_pokedex", "SS_pokedex_183"),
    ("bag", "Bag", "ss_btl_app", "msg_ui_btl_01"),
    ("fly", "Fly", "ss_wazaname", "WAZANAME_019"),
    ("poketch_history", "Obtained Pokémon", "dp_poketch", "DP_poketch_08"),
    ("poketch_nursery", "Pokémon Nursery", "dp_poketch", "DP_poketch_72"),
    ("cancel", "Cancel", "ss_bag", "SS_bag_096"),
    ("use", "Use", "ss_btl_app", "msg_ui_btl_hokaku_03"),
    ("back", "Back", "ss_btl_app", "msg_ui_btl_08"),
    ("cmd_battle", "Battle", "ss_btl_app", "msg_ui_btl_00"),
    ("cmd_pokemon", "Pokémon", "ss_btl_app", "msg_ui_btl_02"),
    ("cmd_run", "Run", "ss_btl_app", "msg_ui_btl_04"),
    ("ability", "Ability", "ss_btl_app", "msg_ui_btl_wazainfo_tokusei"),
    ("power", "Power", "ss_btl_app", "msg_ui_btl_wazainfo_03"),
    ("accuracy", "Accuracy", "ss_btl_app", "msg_ui_btl_wazainfo_02"),
    ("stage_accuracy", "Accuracy", "ss_btl_app", "msg_ui_btl_meichu"),
    ("stat_attack", "Attack", "ss_btl_app", "msg_ui_btl_kougeki"),
    ("stat_defense", "Defense", "ss_btl_app", "msg_ui_btl_bougyo"),
    ("stat_sp_atk", "Sp. Atk", "ss_btl_app", "msg_ui_btl_tokukou"),
    ("stat_sp_def", "Sp. Def", "ss_btl_app", "msg_ui_btl_tokubou"),
    ("stat_speed", "Speed", "ss_btl_app", "msg_ui_btl_subayasa"),
    ("eff_super", "Super effective", "ss_btl_app", "msg_ui_btl_kouka_00"),
    ("eff_neutral", "Effective", "ss_btl_app", "msg_ui_btl_kouka_01"),
    ("eff_not_very", "Not very effective", "ss_btl_app", "msg_ui_btl_kouka_02"),
    ("no_effect", "No effect", "ss_btl_app", "msg_ui_btl_kouka_03"),
    ("toast_no_effect", "It won’t have any effect.", "ss_bag", "SS_bag_115"),
    ("status_poisoned", "Poisoned", "dp_options", "DP_options_000"),
    ("status_paralyzed", "Paralyzed", "dp_options", "DP_options_001"),
    ("status_asleep", "Asleep", "dp_options", "DP_options_002"),
    ("status_burned", "Burned", "dp_options", "DP_options_003"),
    ("status_frozen", "Frozen", "dp_options", "DP_options_004"),
    ("status_bad_poison", "Badly Poisoned", "ss_btl_state", "BTR_STATE_59_01"),
    ("weather_sun", "Harsh Sunlight", "ss_btl_state", "BTR_STATE_01_01"),
    ("weather_rain", "Rain", "ss_btl_state", "BTR_STATE_02_01"),
    ("weather_sand", "Sandstorm", "ss_btl_state", "BTR_STATE_03_01"),
    ("weather_hail", "Hail", "ss_btl_state", "BTR_STATE_04_01"),
    ("terrain_electric", "Electric Terrain", "ss_btl_state", "BTR_STATE_05_01"),
    ("terrain_grassy", "Grassy Terrain", "ss_btl_state", "BTR_STATE_06_01"),
    ("terrain_misty", "Misty Terrain", "ss_btl_state", "BTR_STATE_07_01"),
    ("terrain_psychic", "Psychic Terrain", "ss_wazaname", "WAZANAME_678"),
    ("effect_trick_room", "Trick Room", "ss_btl_state", "BTR_STATE_08_01"),
    ("effect_magic_room", "Magic Room", "ss_btl_state", "BTR_STATE_09_01"),
    ("effect_wonder_room", "Wonder Room", "ss_btl_state", "BTR_STATE_10_01"),
    ("effect_gravity", "Gravity", "ss_btl_state", "BTR_STATE_29_01"),
    ("effect_imprison", "Imprison", "ss_btl_state", "BTR_STATE_46_01"),
    ("effect_ion_deluge", "Ion Deluge", "ss_wazaname", "WAZANAME_569"),
    ("effect_fairy_lock", "Fairy Lock", "ss_wazaname", "WAZANAME_587"),
    ("effect_neutralizing_gas", "Neutralizing Gas", "ss_tokusei", "TOKUSEI_256"),
    # the Pokédex page: the Box search's Gender heading; an evolution member not seen yet
    ("dex_gender", "Gender", "ss_box", "SS_box_112"),
    ("dex_unknown", "?????", "ss_pokedex", "SS_pokedex_029"),
] + [
    (f"pocket_{n}", e, "ss_bag_pocket", f"SS_bag_pocket_{k:03d}")
    for k, (n, e) in enumerate([("medicine", "Medicine"), ("balls", "Poké Balls"), ("battle", "Battle Items"),
                                ("berries", "Berries"), ("other", "Other Items"), ("tms", "TMs"),
                                ("treasures", "Treasures"), ("key", "Key Items")], 1)
] + [
    # the Pokétch apps in lp_poketch's order (DP_poketch_21..43 without 28, 34 and 36)
    (f"poketch_app_{k}", e, "dp_poketch", f"DP_poketch_{n}", UPPER_FIRST)
    for k, (n, e) in enumerate(zip(
        [21, 22, 23, 24, 25, 26, 27, 29, 30, 31, 32, 33, 35, 37, 38, 39, 40, 41, 42, 43],
        ["Digital Watch", "Calculator", "Memo Pad", "Pedometer", "Pokémon List", "Friendship Checker",
         "Dowsing Machine", "Egg Monitor", "Pokémon History", "Counter", "Analog Watch", "Marking Map",
         "Coin Toss", "Calendar", "Dot Artist", "Spinner", "Chain Counter", "Kitchen Timer",
         "Color Changer", "Hidden Moves"]))
]

# Language-neutral texts the layout measures (digits are drawn in the language's own face: the
# Korean and Chinese faces have their own Latin and digits).
NEUTRAL_KEYS = [("num_100", "100")]

# Slot names a template may use (lp_strings.h fills them by name).
SLOTS = {"item", "who", "move", "type", "foe", "ability", "effect", "n", "min", "max"}
KO_PARTICLES = {"이/가", "을/를", "은/는", "과/와", "으로/로"}


@dataclass
class Row:
    key: str
    english: str
    table: str = ""
    label: str = ""
    flags: int = 0
    text: dict = field(default_factory=dict)  # column -> text (our table)
    use_label: set = field(default_factory=set)  # variants reading the game label
    klass: str = "B"
    notes: str = ""
    confidence: str = ""

    @property
    def template(self) -> bool:
        return "{" in self.english


def load(path: Path = TSV) -> dict[str, Row]:
    rows: dict[str, Row] = {}
    with open(path, encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            obj, lbl = r["game_object"].strip(), r["game_label"].strip()
            row = Row(r["key"], r["en"], obj, lbl, int(r.get("flags") or 0),
                      {c: r[c] for c in COLUMNS}, set(filter(None, (r.get("use_label") or "").split(","))),
                      r["class"], r["notes"], r["confidence"])
            if row.template:
                row.table = row.label = ""  # tags drop out of labels: templates are always ours
            rows[row.key] = row
    for spec in LABEL_KEYS:
        key, english, table, label = spec[:4]
        assert key not in rows, key
        rows[key] = Row(key, english, table, label, spec[4] if len(spec) > 4 else 0, {"en": english},
                        set(VARIANTS), "B")
    for key, english in NEUTRAL_KEYS:
        rows[key] = Row(key, english, text={"en": english}, klass="N")
    if VANILLA_TSV.exists():
        with open(VANILLA_TSV, encoding="utf-8", newline="") as f:
            for r in csv.DictReader(f, delimiter="\t"):
                assert r["key"] in rows, f"{VANILLA_TSV.name}: unknown key {r['key']!r}"
                row = rows[r["key"]]
                for col in VANILLA_COLUMNS:
                    if (r.get(col) or "").strip():
                        row.text[col] = r[col]
                        row.use_label.discard(col)
    check(rows)
    return rows


def check(rows: dict[str, Row]):
    for row in rows.values():
        slots = set(re.findall(r"\{([a-z]+)\}", row.english))
        assert slots <= SLOTS, (row.key, slots)
        for col, text in row.text.items():
            if not text:
                continue
            assert set(re.findall(r"\{([a-z]+)\}", text)) == slots, (row.key, col, text)
            for tok in re.findall(r"\{([^}a-z][^}]*|[^}]*/[^}]*)\}", text):
                assert tok in KO_PARTICLES and col == "ko", (row.key, col, tok)
        assert re.fullmatch(r"[a-z][a-z0-9_]*", row.key), row.key


# ---- the game label as the module reads it ---------------------------------------------------

def _spaceless(ch: str) -> bool:
    cp = ord(ch)
    return (0x3000 <= cp <= 0x30FF or 0x3400 <= cp <= 0x4DBF or 0x4E00 <= cp <= 0x9FFF or
            0xF900 <= cp <= 0xFAFF or 0xFF00 <= cp <= 0xFFEF)


def normalize(text: str, flags: int = 0) -> str:
    """lp_strings::NormalizeLabel: line breaks joined (nothing between Chinese / Japanese text,
    else a space), runs of spaces collapsed, then spaces, no-break spaces and a trailing colon
    trimmed at both ends."""
    out = ""
    for i, ch in enumerate(text):
        if ch != "\n":
            out += ch
            continue
        out = out.rstrip(" ")
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if out and nxt and not (_spaceless(out[-1]) or _spaceless(nxt)):
            out += " "
    out = re.sub(" {2,}", " ", out)
    ws = "   　"
    out = out.strip(ws)
    while out.endswith((":", "：")):
        out = out[:-1].rstrip(ws)
    if flags & UPPER_FIRST and out and out[0].islower():
        out = out[0].upper() + out[1:]
    return out


def _strip_tags(text: str) -> str:
    # tag words carry no str in the message objects: the module never sees them
    return re.sub(r"\{[0-9A-Za-z]+\.[0-9A-Za-z]+\}", "", text)


PREFIX = {"en": "english", "pt-BR": "english", "fr": "french", "de": "german", "es-ES": "spanish",
          "es-419": "spanish", "ko": "korean", "zh-Hans": "simp_chinese", "zh-Hant": "trad_chinese"}
MOD = {"pt-BR": "ptbr", "fr": "fr", "de": "de", "es-ES": "es-es", "es-419": "es-latam", "ko": "ko",
       "zh-Hans": "zh-hans", "zh-Hant": "zh-hant"}


def recompute_use_label(rows: dict[str, Row], dumps: Path):
    """use_label = the variants whose label (as the module reads it, English where the language's
    is empty) equals our row."""
    def read(name):
        out = {}
        p = dumps / name
        if p.exists():
            for line in p.read_text(encoding="utf-8").splitlines():
                parts = line.split("\t")
                if len(parts) >= 4:
                    out[(parts[0], parts[1])] = parts[3].replace("\\n", "\n")
        return out
    stock = {**read("en.tsv"), **read("common.tsv")}
    mods = {v: read(f"mod_{m}.tsv") for v, m in MOD.items()}

    def label(variant, table, lbl):
        pre = PREFIX[variant]
        mod = mods.get(variant, {})
        text = mod.get((f"{pre}_{table}", lbl), stock.get((f"{pre}_{table}", lbl), ""))
        text = _strip_tags(text)
        if not text.strip():
            text = _strip_tags(stock.get((f"english_{table}", lbl), ""))
        return text

    tsv_keys = _tsv_keys()
    for row in rows.values():
        if row.key not in tsv_keys:
            continue  # LABEL_KEYS read the label everywhere
        row.use_label = set()
        if not row.label:
            continue
        for v in COLUMNS:
            if row.text.get(v) and normalize(label(v, row.table, row.label), row.flags) == row.text[v]:
                row.use_label.add(v)


def _tsv_keys():
    with open(TSV, encoding="utf-8", newline="") as f:
        return {r["key"] for r in csv.DictReader(f, delimiter="\t")}


def save_tsv(rows: dict[str, Row]):
    with open(TSV, encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        fields = list(reader.fieldnames)
        data = list(reader)
    if "use_label" not in fields:
        fields.append("use_label")
    for r in data:
        row = rows[r["key"]]
        r["use_label"] = ",".join(v for v in COLUMNS if v in row.use_label)
    with open(TSV, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, delimiter="\t", lineterminator="\n", quoting=csv.QUOTE_NONE,
                           escapechar=None)
        w.writeheader()
        w.writerows(data)


# ---- the module's header ------------------------------------------------------------------------

def _c(s: str) -> str:
    out = '"'
    for ch in s:
        if ch == "\\":
            out += "\\\\"
        elif ch == '"':
            out += '\\"'
        elif ch == "\n":
            out += "\\n"
        else:
            out += ch
    return out + '"'


def write_header(path: Path, rows: dict[str, Row], widths=(), fits=(), sums=(), catalog_fits=()) -> bool:
    """Writes lp_strings_data.h; returns whether it changed (the module must then be rebuilt)."""
    keys = list(rows)
    lines = [
        "// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork",
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        "// GENERATED by tools/luminescent-platinum/gen_manifest.py (lp_strings.py) from lp_strings.tsv and",
        "// the manifest's t() bindings. Do not edit: change the table or the generator and run it again.",
        "",
        "#pragma once",
        "",
        "#include \"lp_strings.h\"",
        "",
        "namespace lp_strings {",
        "",
        "enum class Key : std::uint16_t {",
    ]
    lines += [f"    {k}," for k in keys]
    lines += ["    Count", "};", ""]
    lines.append("// key, game table, label, flags, variants reading the label (bit = lp_lang::Variant), text per")
    lines.append("// variant (en, pt-BR, fr, de, es-ES, es-419, ko, zh-Hans, zh-Hant, ja, it; \"\" = English)")
    lines.append(f"inline constexpr std::array<Row, {len(keys)}> Rows{{{{")
    for k in keys:
        row = rows[k]
        mask = 0
        for i, v in enumerate(VARIANTS):
            if v in row.use_label or (v in VANILLA_COLUMNS and "en" in row.use_label and not row.text.get(v)):
                mask |= 1 << i
        texts = [row.text.get(v, "") if v in COLUMNS + VANILLA_COLUMNS else "" for v in VARIANTS]
        if not texts[0]:
            texts[0] = row.english
        # a variant equal to English stays empty (falls back)
        texts = [texts[0]] + [t if t != texts[0] else "" for t in texts[1:]]
        lines.append(f"    {{{_c(k)}, {_c(row.table)}, {_c(row.label)}, {row.flags}, 0x{mask:03X},")
        lines.append("     {" + ", ".join(_c(t) for t in texts) + "}},")
    lines.append("}};")
    lines.append("")
    k16 = lambda k: f"static_cast<std::uint16_t>(Key::{k})"
    end = "static_cast<std::uint16_t>(Key::Count)"
    lines.append("// Widths the module publishes as lp.t.<key>.w<scale> (the manifest positions by them): the")
    lines.append("// width a label of that scale (fit_text down to min_scale in `wrap` px, 0 = no fit) draws.")
    lines.append(f"inline constexpr std::array<WidthSpec, {max(1, len(widths))}> Widths{{{{")
    for k, s_, m, w in widths:
        lines.append(f"    {{{k16(k)}, {s_}, {m}, {w}}},")
    if not widths:
        lines.append(f"    {{{end}, 0, 0, 0}},")
    lines.append("}};")
    lines.append("")
    lines.append("// Rows of labels the module adds up (lp.t.sum.<name>; with a limit also lp.t.sum.<name>.fits).")
    lines.append(f"inline constexpr std::array<SumSpec, {max(1, len(sums))}> Sums{{{{")
    for name, scale, limit, parts in sums:
        ks = ", ".join([k16(p) for p in parts] + [end] * (6 - len(parts)))
        lines.append(f"    {{{_c(name)}, {scale}, {limit}, {{{ks}}}}},")
    if not sums:
        lines.append(f"    {{\"\", 0, 0, {{{', '.join([end] * 6)}}}}},")
    lines.append("}};")
    lines.append("")
    lines.append("// Where the manifest draws each string (the dev fit report, lp-unity-tool fit): scale, smallest")
    lines.append("// scale (fit_text), width available (0 = unbounded), lines allowed (0 = any), and the page.")
    lines.append(f"inline constexpr std::array<FitSpec, {max(1, len(fits))}> Fits{{{{")
    for k, s_, m, w, n, where in fits:
        lines.append(f"    {{{k16(k)}, {s_}, {m}, {w}, {n}, {_c(where)}}},")
    if not fits:
        lines.append(f"    {{{end}, 0, 0, 0, 0, \"\"}},")
    lines.append("}};")
    lines.append("")
    lines.append("// Labels showing the game's names (move, item, ability, species): every name of the kind is checked.")
    lines.append(f"inline constexpr std::array<CatalogFitSpec, {max(1, len(catalog_fits))}> CatalogFits{{{{")
    for kind, s_, m, w, n, where in catalog_fits:
        lines.append(f"    {{{_c(kind)}, {s_}, {m}, {w}, {n}, {_c(where)}}},")
    if not catalog_fits:
        lines.append("    {\"\", 0, 0, 0, 0, \"\"},")
    lines.append("}};")
    lines.append("")
    lines.append("} // namespace lp_strings")
    text = "\n".join(lines) + "\n"
    old = path.read_text(encoding="utf-8") if path.exists() else ""
    if old != text:
        path.write_text(text, encoding="utf-8")
        return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", type=Path, help="decoded message tables (en.tsv, common.tsv, mod_*.tsv)")
    args = ap.parse_args()
    rows = load()
    if args.labels:
        recompute_use_label(rows, args.labels)
        save_tsv(rows)
        for row in rows.values():
            if row.key in _tsv_keys() and row.label:
                print(f"{row.key:24} {','.join(v for v in COLUMNS if v in row.use_label) or '-'}")


if __name__ == "__main__":
    main()
