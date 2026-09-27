#!/usr/bin/env python3
"""Prove the module's generated map data equals the 1.0.0 package's shipped data.

  check_generated.py <shipped package dualscreen dir> <generated dir (areas.json + map/*.geo)>

* every .geo the shipped manifest references: byte-identical to the generated blob;
* every area of the shipped manifest.map.areas: equal to the generated area after mapping the
  geometry references (file:map/X -> module:dread:map/X) and dropping water_changes (unused by the
  runtime and the module), comparing numbers as the runtime reads them (float32);
* the generated JSON has no extra keys.
Exit 0 only when everything matches.
"""
import json
import struct
import sys
from pathlib import Path


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def norm(v):
    if isinstance(v, bool) or v is None or isinstance(v, str):
        return v
    if isinstance(v, (int, float)):
        return f32(float(v))
    if isinstance(v, list):
        return [norm(x) for x in v]
    if isinstance(v, dict):
        return {k: norm(x) for k, x in v.items()}
    return v


def first_diff(a, b, path=""):
    if type(a) != type(b):
        return f"{path}: type {type(a).__name__} vs {type(b).__name__}"
    if isinstance(a, dict):
        if set(a) != set(b):
            return f"{path}: keys {sorted(set(a) ^ set(b))}"
        for k in a:
            d = first_diff(a[k], b[k], path + "/" + k)
            if d:
                return d
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path}: len {len(a)} vs {len(b)}"
        for i, (x, y) in enumerate(zip(a, b)):
            d = first_diff(x, y, f"{path}[{i}]")
            if d:
                return d
        return None
    if a == b:
        return None
    return f"{path}: {a!r} vs {b!r}"


def main():
    shipped = Path(sys.argv[1])
    gen = Path(sys.argv[2])
    man = json.load(open(shipped / "manifest.json"))
    areas = json.load(open(gen / "areas.json"))
    bad = 0
    blobs = 0
    for name, A in man["map"]["areas"].items():
        want = json.loads(json.dumps(A))
        want.pop("water_changes", None)
        refs = [want["geo"]] + [l["geo"] for l in want.get("layers", [])]
        want["geo"] = "module:dread:" + want["geo"][len("file:"):]
        for l in want.get("layers", []):
            l["geo"] = "module:dread:" + l["geo"][len("file:"):]
        got = areas.get(name)
        d = "missing area" if got is None else first_diff(norm(got), norm(want))
        print(f"{name:16s} json   " + ("IDENTICAL" if not d else "DIFF " + d))
        bad += bool(d)
        for ref in refs:
            rel = ref[len("file:"):]
            a = (shipped / rel).read_bytes()
            p = gen / rel
            b = p.read_bytes() if p.exists() else None
            same = a == b
            blobs += 1
            print(f"{name:16s} blob   {rel:34s} {len(a):7d} B " + ("IDENTICAL" if same else "DIFF"))
            bad += not same
    extra = set(areas) - set(man["map"]["areas"])
    if extra:
        print("extra areas:", extra)
        bad += 1
    print(f"{len(man['map']['areas'])} areas, {blobs} geometry blobs: " +
          ("ALL IDENTICAL" if bad == 0 else f"{bad} MISMATCHES"))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
