#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Author's tool: writes guide_data.tok.json from the working data modules, with every piece of game text
replaced by a token (see guidedata.py), using the same matcher the package build uses. Then checks that
filling the tokens back in gives the data back exactly.

  python tokenize_data.py          (needs ../data/text122.json or DSTS_TEXT_JSON)
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import guidedata
import public_text


def main():
    data = guidedata.load()
    data.pop("gametext")
    d = public_text.dictionary(guidedata.text_json())
    m = public_text.Matcher(d)
    stats = {"tokens": 0, "kept": 0}

    def tok(s):
        out, last = [], 0
        for a, b, g in m.find(s):
            sub = s[a:b]
            mode = ("g" if sub == g else "l" if sub == g.lower() else "u" if sub == g.upper()
                    else "c" if sub == g[:1].upper() + g[1:] else "f" if sub == g[:1].lower() + g[1:] else None)
            if mode is None:
                stats["kept"] += 1
                continue
            out.append(s[last:a])
            out.append(f"⟦{public_text.fnv(g):016x}{mode}⟧")
            last = b
            stats["tokens"] += 1
        out.append(s[last:])
        return "".join(out)

    def walk(o):
        if isinstance(o, str):
            return tok(o)
        if isinstance(o, list):
            return [walk(x) for x in o]
        if isinstance(o, dict):
            return {(walk(k) if isinstance(k, str) else k): walk(v) for k, v in o.items()}
        return o

    enc = walk(guidedata.encode(data))
    json.dump(enc, open(guidedata.TOK_FILE, "w", encoding="utf8"), ensure_ascii=False, indent=0)
    table = guidedata.game_strings()
    back = guidedata.decode(json.load(open(guidedata.TOK_FILE, encoding="utf8")),
                            lambda s: guidedata.fill(s, table))
    assert back == data, "round trip differs"
    print(f"{guidedata.TOK_FILE}: {stats['tokens']} tokens, {stats['kept']} odd-case matches kept as written, "
          f"round trip exact")


if __name__ == "__main__":
    main()
