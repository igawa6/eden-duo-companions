#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""F13 (2026-11-01): the companion ships no text copied from the game. Scans what is shipped for
the game's own lines, in every language, read from the player's romfs (SW_ROMFS_DIR):

  game lines  every strings.json "game" row's line (as decoded, and as the module shows it), the
              Wild Area zone names (sw_zone_names.h lines), and every Town Map place / facility
              name (script/place_name, common/townmap_facility) -- each also with ' for ’;
  scanned     the generated manifest.json (every string value), the module's sources (every C++
              string literal of 0100ABF008968000.cpp, sw_modfont.cpp and the headers they include,
              comments excluded) and the built modules (build-native-*/0100ABF008968000.so, the loaded
              sections: what the stripped package module keeps).

A shipped string equal to a game line fails, and so does one containing a long game line (20+
characters, 10+ for Japanese / Korean / Chinese): short words like "Bag" also occur in the
companion's own text, so they count only as a whole string.

    SW_ROMFS_DIR=<extracted romfs> python3 tools/pokemon-sword/check_no_game_text.py [--module <.so> ...] \
        [--variant sword|shield] [--romfs-env SHIELD_ROMFS_DIR]
(--module: the built modules to scan, default every build-native-*/<title id>.so of the variant;
--manifest: e.g. a packaged manifest.json instead of the variant's generated one; --romfs-env: read
the romfs path from that variable instead of SW_ROMFS_DIR.)
Exit 0 = clean, 1 = game text found, 77 = no SW_ROMFS_DIR (skipped).
"""
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sw_strings  # noqa: E402
import sw_variants  # noqa: E402

ROOT = HERE.parents[1]
MODULES = ROOT / "native/modules"
MANIFEST = ROOT / f"packages/{sw_variants.get().package_dir}/dualscreen/manifest.json"
MODULE_SOURCES = ["0100ABF008968000.cpp", "sw_modfont.cpp", "acnh_tex.cpp"]
CJK = re.compile(r"[぀-ヿ㐀-鿿가-힯豈-﫿]")


def variants(text):
    """A game line as it may have been copied: control codes off, breaks either way, ’ or '."""
    out = set()
    base = sw_strings.CONTROL.sub("", text)
    for t in (base, base.replace("\\n", "\n"), base.replace("\\n", " "), base.replace("　", " ")):
        t = t.replace("\\n", "\n").replace("　", " ")
        for u in (t, t.replace("’", "'"), t.strip(), t.replace("\n", " ")):
            if u.strip() and "[" not in u and "\\" not in u:
                out.add(u)
    return out


def game_lines(romfs):
    """{language code: {game line text: where it comes from}}."""
    by_lang = {lang: {} for lang in sw_strings.LANGS}

    def add(lang, text, where):
        for v in variants(text):
            by_lang[lang].setdefault(v, where)

    for key in sw_strings.STRINGS:
        if not sw_strings.is_game(key):
            continue
        dir_, file, line = sw_strings.game_ref(key)
        for lang, folder in zip(sw_strings.LANGS, sw_strings.FOLDERS):
            add(lang, romfs.lines(folder, dir_, file)[line], f"{key} ({lang})")
            shown = sw_strings.game_text(key, lang, romfs)
            if shown:
                add(lang, shown, f"{key} ({lang})")
    zone_lines = [int(n) for n in re.findall(r"^    (\d+), //", (MODULES / "sw_zone_names.h").read_text(), re.M)]
    assert len(zone_lines) == 44
    for lang, folder in zip(sw_strings.LANGS, sw_strings.FOLDERS):
        places = romfs.lines(folder, "script", "place_name.dat")
        for i in zone_lines:
            add(lang, places[i], f"zone name ({lang})")
        for i, text in enumerate(places):
            add(lang, text, f"place_name #{i} ({lang})")
        for i, text in enumerate(romfs.lines(folder, "common", "townmap_facility.dat")):
            add(lang, text, f"townmap_facility #{i} ({lang})")
    return by_lang


# The companion's own words that are also a game word, for something else (same language, a
# different line): they stay companion text. (key, language): why.
SAME_WORD = {
    ("bag.res.cured", "ko"): "'recovered' (the Medicine pocket is also named 'recovery')",
    ("egg.bug", "*"): "Egg Group names: the official names are the type words",
    ("egg.dragon", "*"): "Egg Group names: the official names are the type words",
    ("egg.fairy", "*"): "Egg Group names: the official names are the type words",
    ("egg.flying", "*"): "Egg Group names: the official names are the type words",
    ("egg.grass", "*"): "Egg Group names: the official names are the type words",
    ("evo.item", "*"): "'item' (an evolution item; the game's Held Item / Items pocket words)",
    ("route.method.1", "*"): "the 'Flying' encounter method (the type word)",
    ("route.method.2", "es"): "'water' encounters (the Water type word)",
    ("route.method.4", "ja-kana"): "berry-tree encounters (the Berries pocket word)",
    ("route.weather.0", "en"): "'Normal' weather (the Normal type word)",
    ("settings.title", "*"): "the companion's own Settings page title, the plain word for settings (it must "
                             "show without the module, so it cannot be a game line the module publishes)",
    ("status.frozen", "*"): "the frozen status (the Ice type word in Japanese / Korean)",
    ("status.poisoned", "*"): "the poisoned status (the Poison type word in Japanese / Korean)",
}


def same_word(key, lang):
    return (key, lang) in SAME_WORD or (key, "*") in SAME_WORD


def long_enough(text):
    return len(text) >= (10 if CJK.search(text) else 20)


def manifest_strings(path=None):
    found = []

    def walk(o):
        if isinstance(o, dict):
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
        elif isinstance(o, str):
            found.append(o)
    walk(json.loads(Path(path or MANIFEST).read_text(encoding="utf-8")))
    return found


def includes(name, seen):
    if name in seen or not (MODULES / name).exists():
        return
    seen.add(name)
    for inc in re.findall(r'^\s*#\s*include\s+"([^"]+)"', (MODULES / name).read_text(encoding="utf-8"), re.M):
        includes(inc, seen)


LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|//[^\n]*|/\*.*?\*/', re.S)


def c_literals(source):
    """Every string literal of a C++ source (comments skipped, adjacent pieces joined), decoded."""
    out, pending, last_end = [], None, 0
    for m in LITERAL.finditer(source):
        tok = m.group(0)
        if not tok.startswith('"'):
            continue
        gap = source[last_end:m.start()]
        text = decode_c(tok[1:-1])
        if pending is not None and gap.strip() == "":
            pending += text
        else:
            if pending is not None:
                out.append(pending)
            pending = text
        last_end = m.end()
    if pending is not None:
        out.append(pending)
    return [t.decode("utf-8", "replace") for t in out]


def decode_c(body):
    raw, i = bytearray(), 0
    simple = {"n": 10, "t": 9, "r": 13, "0": 0, "\\": 92, '"': 34, "'": 39, "a": 7}
    while i < len(body):
        c = body[i]
        if c == "\\":
            n = body[i + 1]
            if n == "x":
                j = i + 2
                while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                    j += 1
                raw.append(int(body[i + 2:j], 16) & 0xFF)
                i = j
                continue
            raw.append(simple.get(n, ord(n)))
            i += 2
            continue
        raw.extend(c.encode("utf-8"))
        i += 1
    return bytes(raw)


def source_strings():
    seen = set()
    for name in MODULE_SOURCES:
        includes(name, seen)
    out = []
    for name in sorted(seen):
        out += [(name, t) for t in c_literals((MODULES / name).read_text(encoding="utf-8"))]
    return out, sorted(seen)


def loaded_bytes(data):
    """The sections of an ELF64 module that are loaded (SHF_ALLOC): what a stripped package
    module keeps. Debug info (type and enumerator names of an unstripped build) is not shipped."""
    import struct
    if data[:4] != b"\x7fELF" or data[4] != 2:
        return data
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    out = bytearray()
    for i in range(shnum):
        _name, sh_type, flags, _addr, offset, size = struct.unpack_from("<IIQQQQ", data, shoff + i * shentsize)
        if flags & 0x2 and sh_type != 8:  # SHF_ALLOC, not NOBITS
            out += data[offset:offset + size] + b"\x00"
    return bytes(out)


def binary_strings(path):
    """NUL-separated UTF-8 runs of a built module's loaded sections. Runs under 3 bytes are left
    to the source scan (two-letter words such as "PP" also occur as data bytes in any binary)."""
    out = []
    for run in re.split(rb"\x00", loaded_bytes(path.read_bytes())):
        if len(run) < 3:
            continue
        try:
            text = run.decode("utf-8")
        except UnicodeDecodeError:
            continue
        if text.isprintable() or "\n" in text:
            out.append(text)
    return out


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--module", action="append", default=None, help="a built module to scan")
    ap.add_argument("--manifest", help="another manifest.json to scan (default: the variant's generated one)")
    ap.add_argument("--romfs-env", default="SW_ROMFS_DIR",
                    help="the environment variable naming this variant's romfs (Shield's ctest: SHIELD_ROMFS_DIR)")
    sw_variants.add_argument(ap)
    args = ap.parse_args()
    global MANIFEST
    variant = sw_variants.get(args.variant)
    MANIFEST = ROOT / f"packages/{variant.package_dir}/dualscreen/manifest.json"
    if not os.environ.get(args.romfs_env):
        print(f"{args.romfs_env} not set: no-game-text check skipped")
        return 77
    os.environ["SW_ROMFS_DIR"] = os.environ[args.romfs_env]  # sw_strings.Romfs() reads this one
    romfs = sw_strings.Romfs()
    by_lang = game_lines(romfs)
    every = {}
    for lang in sw_strings.LANGS:
        for text, where in by_lang[lang].items():
            every.setdefault(text, where)
    long_lines = {lang: [t for t in by_lang[lang] if long_enough(t)] for lang in sw_strings.LANGS}
    long_every = [t for t in every if long_enough(t)]

    def match(text, lines, longs):
        if text in lines:
            return lines[text]
        for g in longs:
            if g in text:
                return lines[g] + " (inside)"
        return None

    # The companion's own text per language (strings.json columns): a cell is checked against
    # that language's game lines (a Japanese word that is also a Chinese game word is not a copy).
    authored, hits, allowed = {}, [], []
    for key, entry in sw_strings.STRINGS.items():
        for lang in sw_strings.LANGS:
            text = entry.get(lang)
            if text is None:
                continue
            authored.setdefault(text, []).append((key, lang))
            what = match(text, by_lang[lang], long_lines[lang])
            if what and same_word(key, lang):
                allowed.append((key, lang, text, what))
            elif what:
                hits.append(("strings.json", f"{key} ({lang}) {text}", what))
        if "fallback" in entry:  # the English placeholder: no game line of any language
            what = match(entry["fallback"], every, long_every)
            if what:
                hits.append(("strings.json", f"{key} fallback {entry['fallback']}", what))

    def check(where, texts):
        for text in texts:
            if text in authored:
                continue  # companion text, checked per language above
            what = match(text, every, long_every)
            if what:
                hits.append((where, text, what))

    manifest = manifest_strings(args.manifest)
    check("manifest.json", manifest)
    sources, files = source_strings()
    for name, text in sources:
        check(name, [text])
    binaries = [Path(m) for m in args.module] if args.module else sorted(ROOT.glob(f"build-native-*/{variant.title_hex}.so"))
    missing = [b for b in binaries if not b.exists()]
    binaries = [b for b in binaries if b.exists()]
    for so in binaries:
        check(str(so), binary_strings(so))
    rows = sum(1 for k in sw_strings.STRINGS if sw_strings.is_game(k))
    print(f"{len(every)} game line texts (the {rows} strings.json game rows, the 44 Wild Area zone names, every Town Map "
          f"place / facility name; 10 languages, as decoded and as shown); scanned strings.json's companion text "
          f"per language, manifest.json ({len(manifest)} strings), {len(files)} module sources ({len(sources)} "
          f"literals), {len(binaries)} built module(s): {', '.join(str(b) for b in binaries)}"
          + (f" (not built, skipped: {', '.join(str(b) for b in missing)})" if missing else ""))
    print(f"{len(allowed)} companion word(s) that are also a game word for something else (SAME_WORD):")
    for key, lang, text, what in allowed:
        print(f"  {key} ({lang}) {text!r} ~ {what}")
    for where, text, what in hits[:60]:
        print(f"  GAME TEXT in {where}: {text[:80]!r} = {what}")
    if hits:
        print(f"FAIL: {len(hits)} game text occurrence(s)")
        return 1
    print("PASS: no game text")
    return 0


if __name__ == "__main__":
    sys.exit(main())
