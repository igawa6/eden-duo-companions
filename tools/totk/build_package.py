#!/usr/bin/env python3
"""Pack the Tears of the Kingdom companion: <TITLEID>-TearsOfTheKingdom-<version>.dsmod.zip

    tools/totk/build_package.py --linux-so X.so --android-so Y.so --out dist [--version 1.2.0]

The manifest points at `file:art/...` pictures that only exist on the player's device (art pack made from their own
game by make_art.py). tools/build_dualscreen_package.py insists those files exist, so the package is staged with empty
placeholders and the placeholders are removed from the finished archive again: the zip ships no art.
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TITLE = "0100F2C0115B6000"


def refs(value, out):
    if isinstance(value, dict):
        for v in value.values():
            refs(v, out)
    elif isinstance(value, list):
        for v in value:
            refs(v, out)
    elif isinstance(value, str) and value.startswith("file:art/"):
        out.add(value[5:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--linux-so", required=True)
    ap.add_argument("--android-so", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", default="1.2.0")
    a = ap.parse_args()
    subprocess.run([sys.executable, str(ROOT / "tools/totk/gen_manifest.py")], check=True, cwd=ROOT)
    src = ROOT / "packages/TearsOfTheKingdom"
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "TearsOfTheKingdom"
        shutil.copytree(src, stage)
        found = set()
        refs(json.loads((stage / "dualscreen/manifest.json").read_text()), found)
        for rel in found:
            f = stage / "dualscreen" / rel
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(b"")
        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, str(ROOT / "tools/build_dualscreen_package.py"), "--package", str(stage),
                        "--output", tmp, "--version", a.version, "--name", "Tears of the Kingdom companion",
                        "--module", f"linux-x86_64={a.linux_so}", "--module", f"android-arm64-v8a={a.android_so}",
                        "--build-id", "277178B7DBA1B6D4", "--min-runtime", "22"], check=True)
        built = Path(tmp) / f"{TITLE}.dsmod.zip"
        target = out / f"{TITLE}-TearsOfTheKingdom-{a.version}.dsmod.zip"
        with zipfile.ZipFile(built) as zin, zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zout:
            for info in zin.infolist():
                if info.filename.startswith("dualscreen/art/"):
                    continue
                clone = zipfile.ZipInfo(info.filename, date_time=info.date_time)
                clone.compress_type = zipfile.ZIP_DEFLATED
                clone.external_attr = info.external_attr
                zout.writestr(clone, zin.read(info.filename))
        print(target)


if __name__ == "__main__":
    main()
