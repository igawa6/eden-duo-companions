#!/usr/bin/env python3
"""Generate all 62 original, non-game P3P 1.1.0 UI assets from source.

55 historical assets match the tested v1.8.10 design at byte or RGBA level.
The other seven are explicitly new, original procedural alternatives with
no attempt to copy the game's logo. All fonts are independently fetched
from the official OFL Inter release and verified by SHA-256; none are
distributed in this source repository.

Run: python3 tools/p3p/gen_thor_assets.py [--output PATH]
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
SCRIPTS=HERE/"v1810"
DEFAULT_OUT=ROOT/"packages"/"Persona3Portable"/"dualscreen"/"img"
EXPECTED=62


def run(script: str, *args: str) -> None:
    cmd=[sys.executable,str(SCRIPTS/script),*map(str,args)]
    subprocess.run(cmd,check=True)


def make_assets(output: Path, *, verify: bool=True) -> None:
    output.mkdir(parents=True,exist_ok=True)
    if any(output.iterdir()):
        # Avoid carrying stale graphics into source package.
        for old in output.glob("*.png"):
            old.unlink()
    font_dir=Path(os.environ.get("P3P_INTER_FONT_DIR","/usr/share/fonts/opentype/inter"))
    for name in ("Inter-BlackItalic.otf","InterDisplay-BoldItalic.otf"):
        if not (font_dir/name).is_file():
            raise FileNotFoundError(f"{font_dir/name}: use prepare_fonts_for_checks.py")
    groups=[
        ("generate_art_v18_partial.py",31),
        ("generate_art_v1810_main.py",3),
        ("generate_art_v1810_footers.py",2),
        ("generate_art_v1810_geometry.py",15),
        ("generate_art_v1810_persona.py",2),
        ("generate_art_v1810_rings.py",2),
        ("generate_art_v11_original.py",7)
    ]
    import tempfile
    with tempfile.TemporaryDirectory(prefix="p3p-v1_1-") as temp:
        for script,count in groups:
            folder=Path(temp)/script.removesuffix(".py")
            run(script,"--output",str(folder))
            pngs=list(folder.glob("*.png"))
            if len(pngs)!=count:
                raise ValueError(f"{script}: expected {count} graphics, got {len(pngs)}")
            if verify:
                if script=="generate_art_v18_partial.py":
                    run("verify_generated_31.py",str(folder))
                elif script in ("generate_art_v1810_main.py","generate_art_v1810_footers.py"):
                    run("audit_assets.py","--candidate-dir",str(folder))
                elif script=="generate_art_v1810_geometry.py":
                    run("verify_generated_geometry.py") # self-generates and checks pinned pixels
                elif script=="generate_art_v1810_persona.py":
                    run("verify_generated_persona.py",str(folder))
                elif script=="generate_art_v1810_rings.py":
                    run("verify_generated_rings.py")  # self-generates and checks pinned pixels
            for src in pngs:
                dest=output/src.name
                if dest.exists():
                    raise ValueError(f"Duplicated generated name: {src.name}")
                dest.write_bytes(src.read_bytes())
    files=sorted(output.glob("*.png"))
    if len(files)!=EXPECTED:
        raise ValueError(f"Expected {EXPECTED} independent, generated assets; got {len(files)}")
    print("PASS: generated 55 verified historical UI assets plus 7 new source-original assets")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,default=DEFAULT_OUT)
    parser.add_argument("--skip-verification",action="store_true",
                        help="only for development; never use for a submitted release")
    args=parser.parse_args()
    make_assets(args.output,verify=not args.skip_verification)

if __name__=="__main__":
    main()
