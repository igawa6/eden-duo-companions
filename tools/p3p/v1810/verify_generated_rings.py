#!/usr/bin/env python3
"""Prove original ring_a/ring_b pixel equality without redistributing PNGs.

Pin decoded RGBA SHA-256 from the verified v1.8.10 FINAL community archive,
not PNG container hashes (compression and metadata vary across Pillow).
"""
from __future__ import annotations

import argparse
import hashlib
import io
import tempfile
import zipfile
from pathlib import Path

from PIL import Image
from generate_art_v1810_rings import generate_all
from v1810_reference import verify_archive

EXPECTED = {
    "ring_a.png": "bf53cfd8e030d6b74860869fd46374f4ac0640187f1257cd09e68bdb9525a704",
    "ring_b.png": "cf6868434d7f4e493280c42a2e3a566d4e4351251ac6efb4d4851761296bd7bc",
}


def _rgba(image: Image.Image) -> bytes:
    if image.mode != "RGBA" or image.size != (800, 800):
        raise ValueError("Expected an 800x800 original RGBA image")
    return image.tobytes()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path,
                        help="optional local, hash-verified original v1.8.10 release")
    args = parser.parse_args()

    if args.archive:
        verify_archive(args.archive)

    with tempfile.TemporaryDirectory(prefix="p3p-rings-") as temp:
        folder = Path(temp)
        generate_all(folder)
        if {f.name for f in folder.iterdir()} != set(EXPECTED):
            raise AssertionError("Unexpected ring output inventory")
        with (zipfile.ZipFile(args.archive) if args.archive else _NoArchive()) as original:
            for name, known_sha in EXPECTED.items():
                with Image.open(folder / name) as image:
                    pixels = _rgba(image)
                if hashlib.sha256(pixels).hexdigest() != known_sha:
                    raise AssertionError(f"Reconstructed ring pixels differ: {name}")
                if original is not None:
                    with Image.open(io.BytesIO(original.read("dualscreen/img/" + name))) as ref:
                        if _rgba(ref.convert("RGBA")) != pixels:
                            raise AssertionError(f"Original archive image differs: {name}")
    print("2/2 v1.8.10 ring PNGs reproduced at exactly identical decoded RGBA pixels")


class _NoArchive:
    def __enter__(self):
        return None

    def __exit__(self, exc_type, exc_value, traceback):
        return False


if __name__ == "__main__":
    main()
