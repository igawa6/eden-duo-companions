#!/usr/bin/env python3
"""Record the counter-only CD57 frame probe through an owned harness label.

Read back the B patch, relocated mailbox ADRP/ADD and original callee BL before
accepting the marker. Every dump has the harness provenance sidecar. No writes
to guest memory or production support declarations are performed here.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
import time
from pathlib import Path

from build_frame_probe import SITE, NATIVE_UI_UPDATE


def branch_target(word: int, address: int, link: bool) -> int:
    if word & 0xFC000000 != (0x94000000 if link else 0x14000000):
        raise ValueError("unexpected branch opcode (B/BL)")
    delta = word & 0x3FFFFFF
    if delta & 0x2000000:
        delta -= 0x4000000
    return address + delta * 4


def record(directory: Path, prefix: str, driver: list[str]) -> dict:
    def harness(*parts):
        return subprocess.run([*driver, *parts],
                              check=True, capture_output=True, text=True).stdout

    if not re.fullmatch(r"[a-zA-Z0-9-]+", prefix):
        raise ValueError("neutral alphanumeric prefix required")
    if list(directory.glob(prefix + "*")):
        raise ValueError("fresh evidence prefix required")
    started = time.monotonic()
    chain = harness("con", "chain 1E328D8 0x64C30")
    match = re.search(r"\[([0-9A-Fa-f]+)\]=([0-9A-Fa-f]+).*->([0-9A-Fa-f]+)", chain)
    if not match:
        raise ValueError("cannot resolve this run's UI root")
    base = int(match[1], 16) - 0x1E328D8
    ui = int(match[2], 16)

    def dump(name, address, length):
        path = directory / (prefix + "-" + name + ".bin")
        harness("memdump", hex(address), str(length), str(path))
        return path.read_bytes()

    site = base + SITE
    code_address = branch_target(struct.unpack("<I", dump("site", site, 4))[0], site, False)
    code = dump("code", code_address, 92)
    adrp, add = struct.unpack_from("<II", code, 12)
    if adrp & 0x9F00001F != 0x90000013 or add & 0xFFC003FF != 0x91000273:
        raise ValueError("unexpected mailbox ADRP/ADD")
    immediate = ((adrp >> 5) & 0x7FFFF) << 2 | (adrp >> 29) & 3
    if immediate & 0x100000:
        immediate -= 0x200000
    mailbox = ((code_address + 12) & ~4095) + immediate * 4096 + ((add >> 10) & 4095)
    native = branch_target(struct.unpack_from("<I", code, 60)[0], code_address + 60, True)
    if native != base + NATIVE_UI_UPDATE:
        raise ValueError("wrong original native target")
    raw = dump("mailbox", mailbox, 64)
    marker, entered = struct.unpack_from("<II", raw)
    completed = struct.unpack_from("<I", raw, 56)[0]
    if marker != 0x49553352:
        raise ValueError("wrong diagnostic marker")
    native_counter = struct.unpack("<I", dump("native-counter", ui + 0x64C60, 4))[0]
    result = dict(scope="counter diagnostic only; no feature parity claim", main_base=hex(base),
                  ui=hex(ui), flags=hex(int(match[3], 16)), code=hex(code_address),
                  mailbox=hex(mailbox), native_target=hex(native), entered=entered,
                  completed=completed, native_counter=native_counter,
                  x0=hex(struct.unpack_from("<Q", raw, 8)[0]),
                  caller_lr=hex(struct.unpack_from("<Q", raw, 24)[0]),
                  monotonic_start=started, monotonic_end=time.monotonic(), chain=chain.strip())
    (directory / (prefix + ".json")).write_text(json.dumps(result, indent=2) + "\n")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("prefix")
    parser.add_argument("--driver", nargs="+", required=True,
                        help="owned profile controller command supporting con and memdump")
    args = parser.parse_args()
    print(json.dumps(record(args.directory, args.prefix, args.driver), indent=2))
