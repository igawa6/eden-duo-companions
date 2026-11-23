#!/usr/bin/env python3
"""Verify the 15 reconstructed geometric PNGs against audited RGBA pixels.

This intentionally hashes decoded RGBA pixels instead of compressed PNG bytes:
PNG compression can vary with Pillow/zlib version without changing the art.
No original game resources or release ZIP are required.
"""
from __future__ import annotations

import argparse
import hashlib
import tempfile
import zipfile
from pathlib import Path

from PIL import Image
from generate_art_v1810_geometry import generate_all
from v1810_reference import verify_archive

EXPECTED_RGBA_SHA256 = {
    "band_battle_0.png": "79dc7c78fc6d5ed34fc5a0a45b700a781aac51292a91c2534e49eafd745b9d19",
    "band_battle_1.png": "6f613d07cda49b20fae129f42320ea3a9929fbfe39b83a5131209733338f0854",
    "band_main_0.png": "dd3806684fa0390530fa0cea7e35fe015d48cc303c40d616e58014845c1bfcff",
    "band_main_1.png": "bae5e51ae93cfd024145694b01eb76a6638f396dfe1b5046bcae999527ef4f15",
    "band_page_0.png": "077e5281b5c865c1f0c0a7f1fccd1dabb10b026a2899742345d101522f4b2ccf",
    "band_page_1.png": "70ca71002ab949d4a7b535af7d72dcc0c178463ee5c733de04993ef8fa4ee1d2",
    "bg_battle_0.png": "59bc8cd2e43dd6ec2958e99af5921319a73ca353caafc09e9e054d169396b1a7",
    "bg_battle_1.png": "30ad467171d374873049855b30cc08ad265f2bcb9927c223f5a2ff9754598b9d",
    "bg_main_0.png": "8ac7ac6a4e01e8cdf9abce2888f6b71b7f3cda7a1d922659a75183956bcf2c59",
    "bg_main_1.png": "95ab30b6f46708b4960e0f3de3503718960f3ac46b6761f5adec7b3186685cec",
    "bg_page_0.png": "b237318564e36d4813a142910796580da651548f8217597dc4f8e4e7dd7e3042",
    "bg_page_1.png": "1ec7b7334d9193b3cc892b38820ad350a7ae13b145359a95d0cc270efaf8b9d2",
    "moon_mask.png": "495719fb9066ad5ebd1b7b94b3354620a63a5ca5788eb7ef1ca5e75c353f18d1",
    "slant.png": "75e5adb932cb40f30efdfb291ad023df3ac9124007543b8bbc972245ab906e47",
    "slant_wide.png": "b9651e882a4e64d48cfbf9331b1c3d1b3b0ce90b9bbb74e7eb40689509828118",
}

def rgba_bytes(path: Path) -> bytes:
    with Image.open(path) as image:
        if image.mode != "RGBA":
            raise ValueError(f"{path.name} is not RGBA")
        return image.tobytes()

def verify_directory(directory: Path, archive: Path | None = None) -> None:
    generated = {file.name for file in directory.glob("*.png")}
    if generated != set(EXPECTED_RGBA_SHA256):
        raise AssertionError(f"Unexpected generated inventory: {generated ^ set(EXPECTED_RGBA_SHA256)}")
    if archive is not None:
        verify_archive(archive)

    with zipfile.ZipFile(archive) if archive is not None else _NoArchive() as original:
        for name, expected in EXPECTED_RGBA_SHA256.items():
            pixels = rgba_bytes(directory / name)
            digest = hashlib.sha256(pixels).hexdigest()
            if digest != expected:
                raise AssertionError(f"Wrong RGBA pixels for {name}: {digest}")
            if original is not None:
                from io import BytesIO
                with Image.open(BytesIO(original.read("dualscreen/img/" + name))) as ref:
                    if ref.convert("RGBA").tobytes() != pixels:
                        raise AssertionError(f"Original ZIP pixel mismatch: {name}")

class _NoArchive:
    def __enter__(self):
        return None
    def __exit__(self, exc_type, exc, tb):
        return False

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, help="optional canonical v1.8.10 ZIP")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="p3p-geometry-") as tmp:
        destination = Path(tmp)
        files = generate_all(destination)
        if len(files) != 15:
            raise AssertionError(f"Expected 15 generated files, got {len(files)}")
        verify_directory(destination, args.archive)
    print("15/15 regenerated v1.8.10 geometric PNGs match canonical RGBA pixel hashes")

if __name__ == "__main__":
    main()
