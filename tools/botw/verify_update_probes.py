#!/usr/bin/env python3
"""Check BOTW 1.9.0 round-two recorded memory probes; never enable a build.

Input is a directory containing memdump .bin files and their harness .bin.json
sidecars. Only the recorded bytes are examined. Passing does not establish live
object lifetime, hook execution, native effects, or complete companion support.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from pathlib import Path


class Probes:
    def __init__(self, directory: Path):
        self.directory = directory
        self.identity = None
        self.used = {}

    def read(self, name: str) -> tuple[int, bytes]:
        path = self.directory / (name + ".bin")
        data = path.read_bytes()
        meta = json.loads(path.with_suffix(".bin.json").read_text())
        if len(data) != meta["length"] or hashlib.sha256(data).hexdigest() != meta["sha256"]:
            raise ValueError(f"{name}: length/hash mismatch")
        identity = tuple(meta[k] for k in ("game", "title_id", "label", "run_id", "port", "game_build_id"))
        if self.identity is None:
            self.identity = identity
        if identity != self.identity:
            raise ValueError(f"{name}: mixed harness sessions")
        address = int(meta["addr"], 16)
        self.used[name] = meta["sha256"]
        return address, data


def number(data: bytes, offset: int, kind: str = "Q"):
    return struct.unpack_from("<" + kind, data, offset)[0]


def target_flag(data: bytes, offset: int, kind: str, main_base: int) -> dict:
    expected_type, expected_maximum = (1, 999999) if kind == "i" else (2, 360.0)
    minimum = number(data, offset + 16, kind)
    maximum = number(data, offset + 20, kind)
    value = number(data, offset + 24, kind)
    if (data[offset + 8] != expected_type or minimum != 0 or maximum != expected_maximum
            or not math.isfinite(value) or not minimum <= value <= maximum):
        raise ValueError("target flag: invalid type, bounds or value")
    return {"hash": hex(number(data, offset + 28, "I")), "type": expected_type,
            "vtable_offset": hex(number(data, offset) - main_base),
            "minimum": minimum, "maximum": maximum, "value": value}


def verify(directory: Path, main_base: int) -> dict:
    probes = Probes(directory)
    roots = {}
    for name in ("ui", "screen", "player", "pouch", "gamedata", "rune", "quest"):
        slot, pointer = probes.read(name + "-slot")
        address, data = probes.read(name + "-header")
        if len(pointer) != 8 or number(pointer, 0) != address or address % 8:
            raise ValueError(f"{name}: root/header mismatch")
        vtable = number(data, 0) - main_base
        if not 0 < vtable < 0x1E98000:
            raise ValueError(f"{name}: vtable outside candidate image")
        roots[name] = {"slot_offset": hex(slot - main_base), "pointer": hex(address),
                       "vtable_offset": hex(vtable)}

    ui_flags = {}
    ui = int(roots["ui"]["pointer"], 16)
    for state in ("title", "world", "menu", "world-return"):
        address, data = probes.read("ui-" + state + "-fields")
        if address != ui + 0x64C24:
            raise ValueError(f"{state}: UI field address mismatch")
        ui_flags[state] = hex(number(data, 12))

    base, pouch = probes.read("pouch-manager")
    if base != int(roots["pouch"]["pointer"], 16):
        raise ValueError("pouch: wrong manager")
    count, node_offset = struct.unpack_from("<ii", pouch, 0x78)
    if not 0 <= count <= 420 or node_offset != 8:
        raise ValueError("pouch: invalid list metadata")
    head, node, previous = base + 0x68, number(pouch, 0x68), base + 0x68
    seen, items = set(), []
    for _ in range(count):
        offset = node - base - node_offset
        if node in seen or node == head or node % 8 or not 0 <= offset <= len(pouch) - 0xA0:
            raise ValueError("pouch: invalid/cyclic node")
        seen.add(node)
        if number(pouch, offset + 0x10) != previous:
            raise ValueError("pouch: broken backlink")
        kind = number(pouch, offset + 0x18, "i")
        if not 0 <= kind <= 9 or pouch[offset + 0x24] > 1 or pouch[offset + 0x25] > 1:
            raise ValueError("pouch: invalid type/boolean")
        name_offset = number(pouch, offset + 0x30) - base
        if not 0 <= name_offset <= len(pouch) - 64:
            raise ValueError("pouch: name outside snapshot")
        raw = pouch[name_offset:name_offset + 64].split(b"\0", 1)[0]
        if not raw or len(raw) == 64 or any(c not in b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_" for c in raw):
            raise ValueError("pouch: invalid actor name")
        items.append({"actor": raw.decode("ascii"), "type": kind,
                      "equipped": bool(pouch[offset + 0x24]),
                      "value": number(pouch, offset + 0x20, "i")})
        previous, node = node, number(pouch, offset + 8)
    if node != head or number(pouch, 0x70) != previous:
        raise ValueError("pouch: list does not close")

    flags = {}
    for name, wanted, record_kind in (("int", 0x23149BF8, "i"), ("float", 0x9C79A188, "f")):
        _, array = probes.read(name + "-flag-array")
        base, records = probes.read(name + "-flag-records")
        hashes, found = [], []
        for (pointer,) in struct.iter_unpack("<Q", array):
            offset = pointer - base
            if not 0 <= offset <= len(records) - 40:
                raise ValueError(f"{name}: flag outside snapshot")
            flag_hash = number(records, offset + 28, "I")
            hashes.append(flag_hash)
            if flag_hash == wanted:
                found.append(target_flag(records, offset, record_kind, main_base))
        if hashes != sorted(set(hashes)) or len(found) != 1:
            raise ValueError(f"{name}: unordered/duplicate hashes or missing target")
        flags[name] = {"count": len(hashes), "target": found[0]}

    return {"scope": "Recorded-byte invariants only; no support or live-action claim",
            "harness_identity": probes.identity, "main_base": hex(main_base),
            "roots": roots, "ui_flags": ui_flags, "pouch_count": count,
            "equipped_items": [item for item in items if item["equipped"]],
            "flag_arrays": flags, "checked_sha256": probes.used}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--main-base", type=lambda value: int(value, 0), required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.directory, args.main_base), indent=2))
    except (ValueError, KeyError, OSError, struct.error) as error:
        parser.exit(1, f"Probe verification failed: {error}\n")


if __name__ == "__main__":
    main()
