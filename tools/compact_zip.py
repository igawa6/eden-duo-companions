#!/usr/bin/env python3
"""Rewrite dualscreen/manifest.json compactly inside a .dsmod.zip, in place.

build_dualscreen_package.py writes the manifest indented (readable, stable diffs). The Android
installer caps dualscreen/manifest.json at 4 MiB, so release archives carry it without whitespace.
The JSON value is unchanged (checked), and every other member is copied byte for byte with its
original name, order, timestamp and attributes.

    compact_zip.py <archive.dsmod.zip> [<archive.dsmod.zip> ...]
"""
import io
import json
import sys
import zipfile

MANIFEST = "dualscreen/manifest.json"
LIMIT = 4 * 1024 * 1024


def compact(path: str) -> int:
    with zipfile.ZipFile(path) as zin:
        items = [(info, zin.read(info.filename)) for info in zin.infolist()]
    out = io.BytesIO()
    size = -1
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zout:
        for info, data in items:
            if info.filename == MANIFEST:
                value = json.loads(data)
                data = (json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
                assert json.loads(data) == value
                size = len(data)
            zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = info.external_attr
            zout.writestr(zi, data)
    if size < 0:
        raise SystemExit(f"{path}: no {MANIFEST}")
    if size > LIMIT:
        raise SystemExit(f"{path}: {MANIFEST} is {size} bytes even compacted (installer limit {LIMIT})")
    with open(path, "wb") as f:
        f.write(out.getvalue())
    return size


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    for path in argv:
        print(f"{path}: {MANIFEST} {compact(path)} bytes")


if __name__ == "__main__":
    main(sys.argv[1:])
