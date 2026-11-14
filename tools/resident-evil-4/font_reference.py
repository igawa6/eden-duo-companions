# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline Python golden font builder; never writes game glyphs into the package."""
import importlib.util
import struct
from PIL import Image


def build(research):
    samples = research / 'assets/samples'
    spec = importlib.util.spec_from_file_location('re4_font_source', research/'assets/tools/font_extract.py')
    module = importlib.util.module_from_spec(spec)
    # Research helper imports its own sibling decoder. Suppress Python cache writes there.
    import sys
    sys.dont_write_bytecode = True
    spec.loader.exec_module(module)
    atlas = Image.open(samples/'font_native_atlas_0f000001.png').convert('RGBA')
    assert atlas.size == (2048,768)
    atlas = atlas.resize((1536,576),Image.Resampling.LANCZOS)
    # Existing runtime Story-of-Seasons metrics adapter, using native atlas pixels.
    # Crop bounds provide advances; .fnt spacing/localization refinement stays open.
    cmap = bytearray(b'\xff\xff'*65536)
    records = bytearray()
    # Additional native glyphs used by the decoded English item names.
    charmap=dict(module.CHARMAP, **{"'":163, '×':181, '(':24, ')':25})
    for cp in [*range(32,127), ord('×')]:
        ch=chr(cp)
        idx=charmap.get(ch)
        if ch==' ': box=(0,0,0,0); x=y=0; advance=12
        elif idx is None: continue
        else:
            x=(idx%32)*48; y=(idx//32)*48
            box=atlas.crop((x,y,x+48,y+48)).getbbox()
            if not box: continue
            x+=box[0];y+=box[1];advance=box[2]-box[0]+2
        w=box[2]-box[0];h=box[3]-box[1]
        struct.pack_into('<H',cmap,cp*2,len(records)//12)
        records+=struct.pack('<HHHBBbBH',cp,x,y,w,h,0,box[1],advance*16)
    return atlas, bytes(cmap+records)
