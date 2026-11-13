#!/usr/bin/env python3
"""Inventory update-sensitive constants without treating candidates as supported.

The literal appendix deliberately includes non-address constants. It is a review
superset, not a C++ semantic analysis: computed offsets and implicit struct layout
still need a manual contract/disassembly review. Nothing here changes a load plan.
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# Consume comments/strings first so their numbers are not mistaken for code.
TOKENS = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    r'|\b(?:0[xX][0-9a-fA-F]+|[0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?)[uUlLfF]*\b',
    re.DOTALL,
)


def inventory(findings: dict) -> dict:
    plan = ROOT / "tools/botw/build_helper_plan.py"
    symbols = {}
    sites = {}
    for node in ast.parse(plan.read_text()).body:
        if not isinstance(node, ast.Assign):
            continue
        if any(isinstance(t, ast.Name) and t.id == "SYMBOLS" for t in node.targets):
            symbols = {k.arg: ast.literal_eval(k.value) for k in node.value.keywords}
        for target in node.targets:
            names = target.elts if isinstance(target, ast.Tuple) else [target]
            if any(isinstance(n, ast.Name) and n.id in {"UI_UPDATE_SITE", "FOOD_SITE"} for n in names):
                values = ast.literal_eval(node.value)
                sites.update({n.id: v for n, v in zip(names, values) if isinstance(v, int)})
    records = []
    for name, old in (symbols | sites).items():
        previous = findings.get("priority1_call_sites_and_functions", {}).get(name)
        previous = previous or findings.get("symbols", {}).get(name, {})
        if previous and int(previous["old_addr"], 16) != old:
            raise ValueError(f"{name}: findings do not match the checked-out source")
        records.append({
            "name": name, "old": hex(old), "candidate": previous.get("new_addr"),
            "confidence": previous.get("confidence", "unresolved"),
            "evidence": previous.get("evidence", "No relocation evidence recorded"),
            "live_verified": False,
            "kind": "mailbox-relative" if name == "bridge_mailbox" else "main-relative",
        })
    files = [ROOT / "native/modules/01007EF00011E000.cpp"]
    files += sorted((ROOT / "native/modules").glob("botw_*.h"))
    files += sorted((ROOT / "native/helper/botw").glob("*"))
    files = [p for p in files if p.is_file() and "test" not in p.name]
    literals, sources = [], {}
    for path in files:
        source = path.read_text()
        relative = str(path.relative_to(ROOT))
        sources[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
        seen = {}
        for match in TOKENS.finditer(source):
            token = match.group()
            if not token[0].isdigit():
                continue
            line = source.count("\n", 0, match.start()) + 1
            seen.setdefault(token, set()).add(line)
        for token, lines in seen.items():
            literals.append({"source": relative, "literal": token, "lines": sorted(lines),
                             "candidate": None, "confidence": "unreviewed",
                             "live_verified": False})
    return {"schema": 1, "source_hashes": sources,
            "helper_source_sha256": hashlib.sha256(plan.read_bytes()).hexdigest(),
            "helper": records, "literal_review_superset": literals}


def markdown(data: dict) -> str:
    out = ["# Update constant inventory", "",
           "Static candidates from the supplied previous findings; none live-verified by this inventory.",
           "The reader/helper literal appendix includes non-address values. Decimal fields are included;",
           "computed offsets, macro expansions and implicit C++ struct offsets require manual review.",
           "No candidate in this file authorizes enabling a build.", "",
           "## Helper symbols and patch sites", "",
           "| Name | 1.6.0 | 1.9.0 candidate | Confidence | Live verified | Evidence |",
           "|---|---|---|---|---|---|"]
    for r in data["helper"]:
        evidence = r["evidence"].replace("|", "\\|").replace("\n", " ")
        out.append(f'| {r["name"]} | {r["old"]} | {r["candidate"] or "unresolved"} | '
                   f'{r["confidence"]} | no | {evidence} |')
    out += ["", "## Literal review superset", "",
            "Every entry below remains unreviewed for 1.9.0. No field is assumed unchanged.", "",
            "| Source | Lines | 1.6.0 literal | 1.9.0 | Signals | Confidence | Live verified |",
            "|---|---|---|---|---|---|---|"]
    for r in data["literal_review_superset"]:
        lines = ", ".join(map(str, r["lines"]))
        out.append(f'| {r["source"]} | {lines} | {r["literal"]} | unresolved | '
                   'source occurrence only | unreviewed | no |')
    return "\n".join(out) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--findings", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, required=True)
    args = parser.parse_args()
    data = inventory(json.loads(args.findings.read_text()))
    args.json.write_text(json.dumps(data, indent=2) + "\n")
    args.markdown.write_text(markdown(data))
    print(f'{len(data["helper"])} helper entries; '
          f'{len(data["literal_review_superset"])} literal/file entries; '
          f'{len(data["source_hashes"])} source files')


if __name__ == "__main__":
    main()
