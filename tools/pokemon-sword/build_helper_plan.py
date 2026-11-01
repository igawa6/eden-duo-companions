#!/usr/bin/env python3
"""Build the format-2 load plan for the Pokemon Sword 1.3.2 guest helper (battle commands).

Same technique as tools/belmont/build_helper_plan.py: compile native/helper/sw_guest_helper.S
with clang (aarch64-none-elf), take its .text bytes and ELF relocations, resolve every external
symbol to a (target, addend) pair and emit the plan JSON `mod_load_plan.cpp` parses.

Offline only. The game's executable is never part of this repository: pass the player's dumped
exefs/main (NSO) with --nso or SWORD_NSO. The NSO's segments are decompressed here to check the
hook's `expected` bytes and every native call target's first instruction.

Everything title-specific (title id, build id, the hook sites, every native symbol's main offset
and the expected instruction bytes) is the variant's (sw_variants.py, --variant, default sword).
Each site is found by its byte signature (sw_sigscan.py) and cross-checked against the verified
offset; a site whose signature does not match uniquely falls back to that offset (the build id
already matched), and a scan that disagrees with it is an error.

    python3 tools/pokemon-sword/build_helper_plan.py --nso <exefs/main> [--variant sword] \
        [--output packages/PokemonSword/dualscreen/guest/<build>/load-plan.json]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sw_sigscan  # noqa: E402
import sw_variants  # noqa: E402

REPO_ROOT = HERE.parents[1]
HELPER_SRC = REPO_ROOT / "native/helper/sw_guest_helper.S"

MAILBOX_SIZE = 4096
MAILBOX_EPOCH_OFFSET = 4092
MAX_GUEST_CODE_BYTES = 64 * 1024
BRANCH_PLACEHOLDER = struct.pack("<I", 0x14000000).hex()  # b . (the plan relocates it to the entry)


def default_output(variant) -> Path:
    return REPO_ROOT / f"packages/{variant.package_dir}/dualscreen/guest/{variant.build.build_id}/load-plan.json"


RELOC_KIND_MAP = {
    "R_AARCH64_CALL26": "aarch64_branch26",
    "R_AARCH64_JUMP26": "aarch64_branch26",
    "R_AARCH64_ADR_PREL_PG_HI21": "aarch64_adrp",
    "R_AARCH64_ADD_ABS_LO12_NC": "aarch64_add_lo12",
}
# phase 1: mailbox adrp+add (2) + 2 state adrp+add (4) + 4 bl + 2 b = 12 (the lower bound the
# object's own relocation table is checked against below, len(relocations) >= 12)
# phase 2: alive mailbox adrp+add (2) + finish adrp+add (2) + 1 b (alive resume) + the new bl's


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nso", type=Path, default=Path(os.environ["SWORD_NSO"]) if "SWORD_NSO" in os.environ else None)
    ap.add_argument("--output", type=Path)
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--objcopy", default="llvm-objcopy")
    ap.add_argument("--readelf", default="llvm-readelf")
    sw_variants.add_argument(ap)
    a = ap.parse_args()
    if a.nso is None:
        ap.error("pass --nso <exefs/main> (or SWORD_NSO)")
    variant = sw_variants.get(a.variant)
    build = variant.build
    output = a.output or default_output(variant)
    sw_variants.validate()

    image = sw_sigscan.load(a.nso)
    assert image.build_id == build.build_id, f"NSO build id {image.build_id} != {build.build_id}"
    seg, bss = image.segments, image.bss_size
    code_at = image.code_at
    # Every site by signature, cross-checked against the verified offsets of this build.
    main_off = sw_sigscan.resolve_all(image, build.sites, known_build=True)

    for off, expect in build.expect.items():
        got = code_at(off)
        assert got == bytes.fromhex(expect), f"main+{off:#x} = {got.hex()}, expected {expect}"
    symbols = {}  # symbol -> (target, addend)
    for name, (site, extra) in sw_variants.HELPER_SYMBOLS.items():
        symbols[name] = ("mailbox", 0) if site is None else ("main", main_off[site] + extra)
    hooks = [(main_off[site], entry) for site, entry in sw_variants.HELPER_HOOKS]
    entry_names = [entry for _site, entry in sw_variants.HELPER_HOOKS]

    with tempfile.TemporaryDirectory() as tmp:
        obj = Path(tmp) / "sw_guest_helper.o"
        binf = Path(tmp) / "sw_guest_helper.bin"
        subprocess.run([a.clang, "--target=aarch64-none-elf", "-c", str(HELPER_SRC), "-o", str(obj)], check=True)
        subprocess.run([a.objcopy, "-O", "binary", "--only-section=.text", str(obj), str(binf)], check=True)
        code = binf.read_bytes()
        reloc_text = subprocess.run([a.readelf, "-r", str(obj)], check=True, capture_output=True, text=True).stdout
        sym_text = subprocess.run([a.readelf, "-s", str(obj)], check=True, capture_output=True, text=True).stdout
    entry_offsets = {}
    for line in sym_text.splitlines():
        f = line.split()
        if len(f) >= 8 and f[-1] in entry_names:
            entry_offsets[f[-1]] = int(f[1], 16)
    assert entry_offsets.get(entry_names[0]) == 0, entry_offsets
    assert all(k in entry_offsets for k in entry_names), entry_offsets

    assert code and len(code) % 4 == 0 and len(code) <= MAX_GUEST_CODE_BYTES
    # mod_load_plan.cpp refuses system instructions (SVC/MRS/MSR/barriers) in guest code.
    for at in range(0, len(code), 4):
        word = struct.unpack_from("<I", code, at)[0]
        assert (word & 0xFF000000) != 0xD4000000 and ((word & 0xFF000000) != 0xD5000000 or word == 0xD503201F), \
            f"system instruction at +{at:#x}: {word:08x}"

    pattern = re.compile(r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(R_AARCH64_\S+)\s+[0-9a-fA-F]+\s+(\S+)\s+([+-])\s+([0-9a-fA-F]+)")
    relocations = []
    for line in reloc_text.splitlines():
        m = pattern.fullmatch(line.strip())
        if not m:
            continue
        off_s, kind, sym, sign, add_s = m.groups()
        assert kind in RELOC_KIND_MAP, f"unhandled relocation {kind}"
        assert sym in symbols, f"undeclared symbol {sym}"
        extra = int(add_s, 16) * (1 if sign == "+" else -1)
        assert extra >= 0
        target, addend = symbols[sym]
        relocations.append(dict(offset=int(off_s, 16), kind=RELOC_KIND_MAP[kind], target=target, addend=addend + extra))
    # every bl/b in the object that names an external symbol must have been resolved above
    n_ext = sum(1 for line in reloc_text.splitlines() if pattern.fullmatch(line.strip()))
    assert len(relocations) == n_ext and len(relocations) >= 12, f"relocation count {len(relocations)}"

    layout = {
        "segments": [{"location": s[0], "size": s[1]} for s in seg],
        "bss_size": bss,
    }
    plan = {
        "title_id": variant.title_hex,
        "build_id": build.build_id,
        "module": "main",
        "format": 2,
        "layout": layout,
        "mailbox_size": MAILBOX_SIZE,
        "mailbox_epoch_offset": MAILBOX_EPOCH_OFFSET,
        "patches": [{
            "offset": off,
            "expected": code_at(off).hex(),
            "replacement": BRANCH_PLACEHOLDER,
            "relocations": [{"offset": 0, "kind": "aarch64_branch26", "target": "code",
                             "addend": entry_offsets[entry]}],
        } for off, entry in hooks],
        "guest_code": {"bytes": code.hex(), "relocations": relocations},
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(plan, indent=1) + "\n")
    print(json.dumps(dict(plan=str(output), code_bytes=len(code), relocations=len(relocations)), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
