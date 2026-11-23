#!/usr/bin/env python3
"""Audit v1.8.10 artwork provenance and compare locally reconstructed PNGs.

Runs with the Python standard library; it never extracts or commits artwork.
An optional local copy of the original release ZIP can be checked against the
canonical manifest and the per-file SHA-256 / byte-size inventory.

Examples:
  python tools/p3p/v1810/audit_assets.py
  python tools/p3p/v1810/audit_assets.py --archive /path/to/v1.8.10-FINAL.dsmod.zip
  python tools/p3p/v1810/audit_assets.py --candidate-dir /tmp/p3p-art
  python tools/p3p/v1810/audit_assets.py --candidate-dir /tmp/p3p-art --require-complete
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import zipfile
from pathlib import Path

from v1810_reference import manifest_bytes, verify_archive

HERE = Path(__file__).resolve().parent
INVENTORY = HERE / "ASSET_PROVENANCE.csv"
PREFIX = "dualscreen/img/"
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}\Z")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_inventory() -> dict[str, dict[str, str]]:
    with INVENTORY.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        required = {"index", "asset", "size_bytes", "sha256", "origin"}
        if not required.issubset(reader.fieldnames or []):
            raise ValueError("Provenance CSV is missing required columns")
        rows = list(reader)

    if len(rows) != 62:
        raise ValueError(f"Expected 62 original PNGs, got {len(rows)}")

    inventory: dict[str, dict[str, str]] = {}
    for index, row in enumerate(rows):
        name = row["asset"]
        if row["index"] != str(index):
            raise ValueError(f"Unexpected provenance index for {name}")
        if (not name.startswith(PREFIX) or not name.endswith(".png")
                or Path(name).name != name[len(PREFIX):]
                or ".." in name or "\\" in name):
            raise ValueError(f"Unsafe or unexpected PNG path: {name}")
        if name in inventory:
            raise ValueError(f"Duplicate PNG name: {name}")
        if not SHA256_PATTERN.fullmatch(row["sha256"]):
            raise ValueError(f"Invalid SHA-256 for {name}")
        if not row["size_bytes"].isdigit() or int(row["size_bytes"]) <= 0:
            raise ValueError(f"Invalid byte count for {name}")
        if not row["origin"].strip():
            raise ValueError(f"Missing provenance for {name}")
        inventory[name] = row
    return inventory


def manifest_image_references(node: object) -> set[str]:
    """Find statically referenced PNGs anywhere in the exact manifest."""
    refs: set[str] = set()

    def visit(value: object) -> None:
        if isinstance(value, dict):
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)
        elif isinstance(value, str) and value.startswith("file:img/"):
            # file: paths in manifests are relative to the dualscreen directory.
            path = "dualscreen/" + value[len("file:"):]
            if path.endswith(".png"):
                refs.add(path)

    visit(node)
    return refs


def check_archive(path: Path, inventory: dict[str, dict[str, str]]) -> int:
    # First require that this is precisely the originally audited release,
    # never an unrelated or partially reconstructed ZIP.
    verify_archive(path)
    failures: list[str] = []
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            failures.append("Archive contains duplicate ZIP member names")
        actual_pngs = {name for name in names if name.endswith(".png")}
        missing = sorted(set(inventory) - actual_pngs)
        extra = sorted(actual_pngs - set(inventory))
        if missing:
            failures.append(f"PNG members absent from archive: {missing}")
        if extra:
            failures.append(f"Unexpected PNG members in archive: {extra}")
        for name, row in inventory.items():
            if name not in actual_pngs:
                continue
            content = archive.read(name)
            if len(content) != int(row["size_bytes"]):
                failures.append(f"{name}: wrong byte count")
            if sha256(content) != row["sha256"]:
                failures.append(f"{name}: SHA-256 mismatch")
    if failures:
        raise ValueError("\n".join(failures))
    return len(inventory)


def check_candidates(directory: Path, inventory: dict[str, dict[str, str]]) -> set[str]:
    """Verify any subset of independently generated files by exact bytes.

    Accepts either a flat directory produced by generate_art_v18_partial.py,
    or a tree containing dualscreen/img/*.png. Matching is by unique basename
    because all 62 known assets have unique filenames.
    """
    if not directory.is_dir():
        raise ValueError(f"Candidate directory does not exist: {directory}")
    expected = {Path(path).name: row for path, row in inventory.items()}
    validated: set[str] = set()
    failures: list[str] = []
    for file in sorted(directory.rglob("*.png")):
        name = file.name
        if name in validated:
            failures.append(f"Duplicate candidate PNG basename: {name}")
            continue
        if name not in expected:
            failures.append(f"Unrecognized candidate PNG: {file}")
            continue
        row = expected[name]
        data = file.read_bytes()
        if len(data) != int(row["size_bytes"]) or sha256(data) != row["sha256"]:
            failures.append(f"{name}: regenerated bytes differ from the final release")
        else:
            validated.add(name)
    if failures:
        raise ValueError("\n".join(failures))
    return validated


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, help="original audited v1.8.10 ZIP")
    parser.add_argument("--candidate-dir", type=Path, help="locally generated PNGs to compare")
    parser.add_argument("--require-complete", action="store_true",
                        help="fail unless all 62 candidate PNGs match")
    args = parser.parse_args()

    if args.require_complete and not args.candidate_dir:
        parser.error("--require-complete requires --candidate-dir")

    inventory = load_inventory()
    manifest = json.loads(manifest_bytes())
    refs = manifest_image_references(manifest)
    unknown = sorted(refs - set(inventory))
    if unknown:
        raise ValueError(f"Manifest references PNGs absent from provenance: {unknown}")

    print(f"Provenance: {len(inventory)} unique, indexed PNGs; "
          f"{len(refs)} static PNG references verified against inventory")
    if args.archive:
        print(f"Original ZIP: {check_archive(args.archive, inventory)} PNGs "
              "match the exact audited release")

    if args.candidate_dir:
        valid = check_candidates(args.candidate_dir, inventory)
        remaining = sorted(Path(name).name for name in inventory
                           if Path(name).name not in valid)
        print(f"Reconstruction: {len(valid)}/62 byte-identical PNGs; "
              f"{len(remaining)} still need verified source")
        if remaining:
            print("Remaining: " + ", ".join(remaining))
            if args.require_complete:
                raise SystemExit(1)


if __name__ == "__main__":
    main()
