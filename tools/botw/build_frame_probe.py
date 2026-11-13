#!/usr/bin/env python3
"""Build a counter-only 1.9.0 evidence plan; this does not enable support.

The UI thunk tail-calls native_ui_update. Its replacement must remain B so
the probe returns to the thunk's caller, rather than back into the thunk.
Only install in an owned, stopped disposable harness label.
"""
from __future__ import annotations

import argparse
import json
import struct
import tempfile
from pathlib import Path

from build_helper_plan import ROOT, RELOCATION_RE, RELOCATION_KINDS, tool

BUILD_ID = "CD57B23FA4BBAD65803D9788C01821EE00000000000000000000000000000000"
SITE = 0xE79530
NATIVE_UI_UPDATE = 0xE666EC
LAYOUT = {
    "segments": [{"location": 0, "size": 22682724},
                 {"location": 22683648, "size": 6124336},
                 {"location": 28811264, "size": 2327000}],
    "bss_size": 941608,
}


def branch_word(site: int, target: int, link: bool = False) -> bytes:
    delta = target - site
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError("branch target is unaligned or out of range")
    return struct.pack("<I", (0x94000000 if link else 0x14000000) | ((delta // 4) & 0x3FFFFFF))


def build_plan(work: Path) -> dict:
    obj, binary = work / "probe.o", work / "probe.bin"
    tool("clang", "--target=aarch64-none-elf", "-c",
         str(ROOT / "native/helper/botw/ui-frame-probe.S"), "-o", str(obj))
    tool("llvm-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(binary))
    code = binary.read_bytes()
    relocations = []
    for line in tool("llvm-readelf", "-r", str(obj)).splitlines():
        match = RELOCATION_RE.fullmatch(line.strip())
        if not match:
            if line.strip().startswith("0000"):
                raise ValueError(f"unparsed relocation: {line}")
            continue
        offset, kind, symbol, sign, addend = match.groups()
        if symbol not in ("bridge_mailbox", "native_ui_update") or kind not in RELOCATION_KINDS:
            raise ValueError(f"unexpected relocation: {line}")
        value = int(addend, 16) * (1 if sign == "+" else -1)
        relocations.append(dict(offset=int(offset, 16), kind=RELOCATION_KINDS[kind],
                                target="mailbox" if symbol == "bridge_mailbox" else "main",
                                addend=value + (0 if symbol == "bridge_mailbox" else NATIVE_UI_UPDATE)))
    if len(relocations) != 3:
        raise ValueError("expected mailbox ADRP/ADD and original native BL")
    return dict(title_id="01007EF00011E000", build_id=BUILD_ID, module="main", layout=LAYOUT,
                mailbox_size=4096, mailbox_epoch_offset=4092, format=2,
                patches=[dict(offset=SITE, expected=branch_word(SITE, NATIVE_UI_UPDATE).hex(),
                              replacement=branch_word(0, 0).hex(),
                              relocations=[dict(offset=0, kind="aarch64_branch26", target="code", addend=0)])],
                guest_code=dict(bytes=code.hex(), relocations=relocations))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        plan = build_plan(Path(work))
    # Refuse to overwrite a previous evidence artifact.
    with args.output.open("x") as output:
        output.write(json.dumps(plan, indent=2) + "\n")


if __name__ == "__main__":
    main()
