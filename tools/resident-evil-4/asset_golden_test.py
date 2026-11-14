#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare module RGBA with a clean full Python regeneration, no game pixels shipped.

RE4_TEST_ROMFS is the extracted RomFS root (containing bio4).
RE4_TEST_GOLDENS is a clean regenerated dualscreen/assets directory.
The three initial keys are pixel-exact with Pillow 13.0.0. Exit 77 without fixtures.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary',type=Path)
    a=p.parse_args()
    rom=os.environ.get('RE4_TEST_ROMFS','')
    golden=os.environ.get('RE4_TEST_GOLDENS','')
    if not rom or not golden or not (Path(rom)/'bio4/etc/core.udas').is_file():
        print('SKIP: set RE4_TEST_ROMFS and RE4_TEST_GOLDENS')
        return 77
    from PIL import Image
    samples=[('font', 'font.png'), ('castle-label','r5-castle-label.png'), ('key-3b','key-art-3b.png')]
    if not all((Path(golden)/png).is_file() for _,png in samples):
        print('SKIP: clean generated sample PNGs absent')
        return 77
    with tempfile.TemporaryDirectory(prefix='re4-asset-golden-') as scratch:
        subprocess.run([str(a.binary.resolve()),'--fixture',scratch],check=True)
        results={}
        for raw,png in samples:
            im=Image.open(Path(golden)/png).convert('RGBA')
            expected=im.tobytes()
            actual=(Path(scratch)/(raw+'.rgba')).read_bytes()
            assert len(actual)==len(expected), png+' dimensions differ'
            maximum=max(abs(a-b) for a,b in zip(actual,expected))
            results[png]=dict(width=im.width,height=im.height,max_channel_error=maximum)
            assert maximum==0, png+' pixels differ'
        print(json.dumps(results,indent=2))
    return 0


if __name__=='__main__':
    raise SystemExit(main())
