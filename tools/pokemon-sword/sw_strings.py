#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/pokemon-sword/strings.json, resolved the way the module resolves it (sw_strings.h).

A key is either companion text (a column per language, written for the companion) or a game line
("game": "<dir>/<file>#<line>"): every language without its own column then reads that line of
romfs bin/message/<Folder>/<dir>/<file> from the player's game at run time, cleaned up by the
row's "fix" steps (FIXES below == sw_strings.h ApplyFix). Nothing of the game's text is stored in
the repository, the manifest or the module; "fallback" is the companion's own English placeholder
shown when the line cannot be read.

    SW_ROMFS_DIR=<extracted romfs> python3 tools/pokemon-sword/sw_strings.py [key ...]
"""
import json
import os
import re
import struct
import sys
from functools import lru_cache
from pathlib import Path

HERE = Path(__file__).resolve().parent
STRINGS_JSON = HERE / "strings.json"
LANGS = ["en", "fr", "de", "it", "es", "ja", "ja-kana", "ko", "zh-Hans", "zh-Hant"]  # = sw_lang::Lang 0..9
FOLDERS = ["English", "French", "German", "Italian", "Spanish", "JPN_KANJI", "JPN", "Korean", "Simp_Chinese",
           "Trad_Chinese"]  # = sw_lang.h kFolders
# The cleanup steps a row may name (sw_strings.h kFix* bits, same order).
FIXES = ["trim", "flat", "space", "num", "words3"]


def load():
    return {k: v for k, v in json.loads(STRINGS_JSON.read_text(encoding="utf-8")).items() if not k.startswith("_")}


STRINGS = load()


def is_game(key):
    return "game" in STRINGS[key]


def game_ref(key):
    """(dir, file, line) of a game row."""
    m = re.fullmatch(r"(\w+)/([\w.]+)#(\d+)", STRINGS[key]["game"])
    return m.group(1), m.group(2), int(m.group(3))


def fixes(key, lang):
    entry = STRINGS[key]
    return list(entry.get("fix", [])) + list(entry.get(f"fix.{lang}", []))


def authored(key, lang):
    """The companion's own text of a key in a language, None when it is read from the game."""
    return STRINGS[key].get(lang)


# ---------------------------------------------------------------------------- message tables
def _parse(units):
    """sw_text.h ParseLineString: control codes as bracketed placeholders, U+202F -> U+00A0."""
    s, i, n = [], 0, len(units)
    while i < n:
        v = units[i]
        i += 1
        if v == 0:
            break
        if v == 0x10:
            if i + 2 > n:
                break
            var = units[i + 1]
            i += 2
            if var == 0xBE00:
                s.append("\\r")
            elif var == 0xBE01:
                s.append("\\c")
            elif var == 0xBE02:
                if i + 1 > n:
                    break
                s.append(f"[WAIT {units[i]}]")
                i += 1
            elif var == 0xBDFF:
                if i + 1 > n:
                    break
                s.append(f"[~ {units[i]}]")
                i += 1
            else:
                s.append(f"[VAR {var:04X}]")
        elif v == 0x0A:
            s.append("\\n")
        elif 0x20 <= v < 0xD800 or v >= 0xE000:
            s.append(chr(0xA0 if v == 0x202F else v))
        else:
            s.append(f"\\u{v:04X}")
    return "".join(s)


def decode_dat(data):
    """Every line of a GameFreak text .dat (sw_text.h ReadDat), decoded like the module does."""
    sections, count, total, initial, sdo = struct.unpack_from("<HHIII", data, 0)
    if initial != 0 or sections != 1 or struct.unpack_from("<I", data, sdo)[0] != total:
        raise ValueError("not a message table")
    out, key = [], 0x7C89
    for i in range(count):
        off, length = struct.unpack_from("<iH", data, sdo + 4 + 8 * i)
        units = struct.unpack_from(f"<{length}H", data, sdo + off)
        k, plain = key, []
        for u in units:
            plain.append(u ^ k)
            k = ((k << 3) | (k >> 13)) & 0xFFFF
        out.append(_parse(plain))
        key = (key + 0x2983) & 0xFFFF
    return out


def decode_tbl(data):
    """AHTB label table: [label, ...] (cp1252, NUL-trimmed)."""
    count = struct.unpack_from("<I", data, 4)[0]
    off, out = 8, []
    for _ in range(count):
        length = struct.unpack_from("<H", data, off + 8)[0]
        off += 10
        out.append(data[off:off + length].rstrip(b"\0").decode("cp1252", "replace"))
        off += length
    return out


class Romfs:
    def __init__(self, root=None):
        root = root or os.environ.get("SW_ROMFS_DIR")
        if not root:
            raise SystemExit("SW_ROMFS_DIR=<extracted romfs> is required (the game's own text)")
        self.base = Path(root) / "bin/message"

    @lru_cache(maxsize=None)
    def lines(self, folder, dir_, file):
        return decode_dat((self.base / folder / dir_ / file).read_bytes())

    @lru_cache(maxsize=None)
    def labels(self, folder, dir_, file):
        return decode_tbl((self.base / folder / dir_ / file.replace(".dat", ".tbl")).read_bytes())


# ---------------------------------------------------------------------------- cleanup (== sw_strings.h)
CONTROL = re.compile(r"\\[cr]|\[WAIT \d+\]")


def clean(raw, steps):
    """A decoded game line -> the text the companion shows; None = not plain text (fallback)."""
    t = CONTROL.sub("", raw)
    if "num" in steps:
        t = t.replace("[VAR 0202]", "%03d")
    t = t.replace("\\n", "\n").replace("\u3000", " ")
    if "[VAR " in t or "[~ " in t or "\\u" in t or "\\" in t or "%" in t.replace("%03d", ""):
        return None
    if "trim" in steps:
        t = t.strip(" ")
    if "flat" in steps:
        t = t.replace("\n", " ")
    if "words3" in steps:
        t = " ".join(t.split(" ")[:3])
    if "space" in steps:
        t += " "
    return t or None


def game_text(key, lang, romfs):
    """The game's line of a game row in a language, cleaned (None = unreadable)."""
    dir_, file, line = game_ref(key)
    lines = romfs.lines(FOLDERS[LANGS.index(lang)], dir_, file)
    return clean(lines[line], fixes(key, lang)) if line < len(lines) else None


def resolve(key, lang, romfs=None):
    """What the module shows for a key in a language (romfs needed for game rows)."""
    own = authored(key, lang)
    if own is not None:
        return own
    if not is_game(key):
        return STRINGS[key]["en"]
    text = game_text(key, lang, romfs) if romfs else None
    return STRINGS[key]["fallback"] if text is None else text


def main():
    romfs = Romfs()
    for key in sys.argv[1:] or sorted(k for k in STRINGS if is_game(k)):
        print(key, [resolve(key, lang, romfs) for lang in LANGS])


if __name__ == "__main__":
    main()
