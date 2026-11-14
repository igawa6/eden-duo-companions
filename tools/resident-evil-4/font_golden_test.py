#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare worker font bytes/pixels and extension glyphs with pre-port Python goldens."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary',type=Path)
    a=p.parse_args()
    golden=Path(os.environ.get('RE4_TEST_GOLDENS','.'))
    if not os.environ.get('RE4_TEST_ROMFS') or not all((golden/n).is_file() for n in ('font.bin','font.png')):
        print('SKIP: RE4_TEST_ROMFS and pre-port RE4_TEST_GOLDENS required')
        return 77
    from PIL import Image
    expected=(golden/'font.bin').read_bytes()
    def glyph(c):
        index=struct.unpack_from('<H',expected,ord(c)*2)[0]
        return None if index==65535 else struct.unpack_from('<HHHBBbBH',expected,0x20000+index*12)
    ascent=max(glyph(c)[4]+glyph(c)[6] for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789')
    records=[]
    for cp in range(32,127):
        g=glyph(chr(cp))
        records.append(struct.pack('<HHHHhhH',g[1],g[2],g[3],g[4],g[5],ascent-g[6],g[7]//16 or g[3]+2) if g else bytes(14))
    for c in "()'":assert glyph(c)
    assert min(glyph(c)[4]*55/ascent for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789')>=34
    assert min(int(glyph(c)[4]*70/ascent) for c in '0123456789')>=34
    assert sum(glyph(c)[7]//16 for c in '99999')*70/ascent<=205
    with tempfile.TemporaryDirectory(prefix='re4-font-golden-') as tmp:
        result=subprocess.run([str(a.binary.resolve()),'--font',tmp],check=True,capture_output=True,text=True)
        print(result.stdout,end='')
        actual=Path(tmp)
        assert (actual/'font.bin').read_bytes()==expected,'packed metrics differ'
        assert (actual/'glyphs.bin').read_bytes()==b''.join(records),'extension baseline/advance differs'
        pixels=Image.open(golden/'font.png').convert('RGBA').tobytes()
        assert (actual/'font.rgba').read_bytes()==pixels,'atlas pixels differ'
        print(json.dumps(dict(metrics_bytes=len(expected),metrics_sha256=hashlib.sha256(expected).hexdigest(),
                              atlas_pixel_sha256=hashlib.sha256(pixels).hexdigest(),extension_glyphs=95,
                              line_height=ascent,byte_and_pixel_equal=True),indent=2))
    return 0


if __name__=='__main__':
    raise SystemExit(main())
