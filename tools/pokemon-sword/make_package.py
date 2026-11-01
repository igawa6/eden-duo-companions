#!/usr/bin/env python3
"""Build a Pokemon Sword / Shield dual-screen package zip from the two module builds (review P1/P3).

One reproducible step instead of hand-maintained pins:
  1. copy both module builds and strip them (`strip --strip-all`, the NDK's llvm-strip),
  2. write their sha256 into packages/PokemonSword/package.json and dualscreen/manifest.json,
  3. rebuild the guest helper's load plan (build_helper_plan.py, needs the player's exefs/main)
     and the manifest (gen_manifest.py),
  4. package with tools/build_dualscreen_package.py (its hygiene check refuses unstripped or
     path-leaking modules), with the version taken from package.json,
  5. print the module and zip sha256.

    python3 tools/pokemon-sword/make_package.py --nso <exefs/main> --output <dir> [--variant sword] \
        --linux-so <build-linux>/0100ABF008968000.so --android-so <build-android>/0100ABF008968000.so \
        --android-strip <ndk>/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip

The package directory, title id and build id are the variant's (sw_variants.py, default sword);
Shield: --variant shield, the Shield main (SHIELD_NSO) and build-native-*/01008DB008C2C000.so.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import sw_variants  # noqa: E402

PLATFORMS = (("linux-x86_64", "linux"), ("android-arm64-v8a", "android"))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pin(pkg: Path, hashes: dict[str, str]) -> None:
    for f in (pkg / "package.json", pkg / "dualscreen/manifest.json"):
        d = json.loads(f.read_text(encoding="utf-8"))
        for platform, h in hashes.items():
            d["module"]["libraries"][platform]["sha256"] = h
        f.write_text(json.dumps(d, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nso", help="the player's dumped exefs/main (default: $SWORD_NSO / $SHIELD_NSO)")
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--linux-so", type=Path, help="default: build-native-linux/<title id>.so")
    ap.add_argument("--android-so", type=Path, help="default: build-native-android/<title id>.so")
    ap.add_argument("--strip", default="strip")
    ap.add_argument("--android-strip", default=os.environ.get("ANDROID_LLVM_STRIP", "llvm-strip"))
    sw_variants.add_argument(ap)
    a = ap.parse_args()
    variant = sw_variants.get(a.variant)
    a.nso = a.nso or os.environ.get(f"{variant.key.upper()}_NSO")
    if not a.nso:
        ap.error(f"--nso (or {variant.key.upper()}_NSO) is required")
    PKG = ROOT / "packages" / variant.package_dir
    if variant.key != sw_variants.DEFAULT:
        # The art is shared: a twin's img/ and its provenance are the default variant's bake.
        ref = ROOT / "packages" / sw_variants.get().package_dir / "dualscreen"
        mine = PKG / "dualscreen"
        names = lambda d: sorted(f.name for f in (d / "img").iterdir())
        if names(mine) != names(ref) or any((mine / "img" / n).read_bytes() != (ref / "img" / n).read_bytes()
                                            for n in names(ref)) \
                or (mine / "ASSETS.provenance.json").read_bytes() != (ref / "ASSETS.provenance.json").read_bytes():
            raise SystemExit(f"{mine}/img differs from {ref}/img (shared art): copy the bake over")
    TITLE = variant.title_hex
    BUILD16 = variant.build.build16
    a.linux_so = a.linux_so or ROOT / f"build-native-linux/{TITLE}.so"
    a.android_so = a.android_so or ROOT / f"build-native-android/{TITLE}.so"
    if not (PKG / f"dualscreen/{BUILD16}.json").exists():  # the host's per-build data-file probe
        raise SystemExit(f"{PKG}/dualscreen/{BUILD16}.json is missing (the build_match data file)")
    a.output.mkdir(parents=True, exist_ok=True)
    version = json.loads((PKG / "package.json").read_text(encoding="utf-8"))["version"]
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        stripped = {}
        for (platform, short), src, tool in zip(PLATFORMS, (a.linux_so, a.android_so), (a.strip, a.android_strip)):
            dst = tmp / f"{short}.so"
            shutil.copyfile(src, dst)
            subprocess.run([tool, "--strip-all", str(dst)], check=True)
            stripped[platform] = dst
        hashes = {p: sha256(f) for p, f in stripped.items()}
        pin(PKG, hashes)
        subprocess.run([sys.executable, str(ROOT / "tools/pokemon-sword/build_helper_plan.py"), "--nso", a.nso,
                        "--variant", variant.key], check=True, stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, str(ROOT / "tools/pokemon-sword/gen_manifest.py"), "--variant", variant.key],
                       check=True, stdout=subprocess.DEVNULL)
        cmd = [sys.executable, str(ROOT / "tools/build_dualscreen_package.py"), "--package", str(PKG),
               "--output", str(a.output), "--version", version, "--min-runtime", "22", "--abi", "1",
               "--build-id", BUILD16]
        for platform, f in stripped.items():
            cmd += ["--module", f"{platform}={f}"]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    zip_path = a.output / f"{TITLE}.dsmod.zip"
    for platform, h in hashes.items():
        print(f"{platform} {h}")
    print(f"{zip_path.name} {sha256(zip_path)} (version {version})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
