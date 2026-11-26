#!/usr/bin/env python3
"""Check the pinned historical v1.8.10 original archive, if locally available.

The installable v1.1.0 layout is now plain JSON, NOT an LZMA/base85 fixture.
This helper remains only for optionally comparing generated historical art
to the previously tested archive. No game art is embedded in source.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import zipfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
SOURCE=ROOT/"tools/p3p/layout_v1_1.json"
ARCHIVE_SHA256="70faf618f1fd4777c78d5204688b5427e80eeb0e57c2d16ec30e9b02ed3e4513"
TITLE_ID="0100DCD01525A000"


def manifest_bytes():
    """Read the current uncompressed, human-reviewable manifest source."""
    return SOURCE.read_bytes()


def verify_manifest(data):
    if data.get("title_id")!=TITLE_ID:
        raise ValueError("Wrong P3P title ID")
    if (data.get("canvas_w"),data.get("canvas_h"))!=(1240,1080):
        raise ValueError("Wrong P3P canvas dimensions")
    if (len(data["pages"]),len(data["actions"]),len(data["module_outputs"]))!=(12,61,2798):
        raise ValueError("Source manifest incomplete")
    return True


def verify_archive(path):
    """Authenticate the archived legacy distribution, not the v1.1 package."""
    path=Path(path)
    actual=hashlib.sha256(path.read_bytes()).hexdigest()
    if actual!=ARCHIVE_SHA256:
        raise ValueError(f"Original v1.8.10 FINAL archive SHA mismatch: {actual}")
    with zipfile.ZipFile(path) as archive:
        if archive.testzip():
            raise ValueError("Legacy reference ZIP is corrupt")
        if len([n for n in archive.namelist() if n.endswith(".png")])!=62:
            raise ValueError("Legacy reference artwork inventory mismatch")
        old=json.loads(archive.read("dualscreen/manifest.json"))
        verify_manifest(old)
    return {"archive_sha256":actual,"pngs":62}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--archive",type=Path)
    args=p.parse_args()
    verify_manifest(json.loads(manifest_bytes()))
    if args.archive:
        print(verify_archive(args.archive))
    else:
        print("PASS: readable 1.1.0 source manifest validated")


if __name__=="__main__":
    main()
