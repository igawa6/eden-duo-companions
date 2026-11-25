# SPDX-FileCopyrightText: Copyright 2026 Sickops
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack a redistributable Noto CJK font into the existing runtime-19 MFNT format.

Sparse BMP metrics share one compact atlas; unsupported/control characters occupy no pixels.
No platform font API or new host runtime is required on Android.
"""
import argparse
import struct
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

def cmap(path):
    data=path.read_bytes()
    u16=lambda p:struct.unpack_from('>H',data,p)[0]
    u32=lambda p:struct.unpack_from('>I',data,p)[0]
    at=next(u32(12+i*16+8) for i in range(u16(4)) if data[12+i*16:16+i*16]==b'cmap')
    result=set()
    for i in range(u16(at+2)):
        p=at+u32(at+4+i*8+4);fmt=u16(p)
        if fmt==12:
            for j in range(u32(p+12)):
                a,b,g=struct.unpack_from('>III',data,p+16+j*12)
                result.update(range(max(32,a),min(0xFFFE,b+1)))
        elif fmt==4:
            n=u16(p+6)//2;end=p+14;start=end+2*n+2;delta=start+2*n;offset=delta+2*n
            for j in range(n):
                for cp in range(max(32,u16(start+2*j)),min(0xFFFE,u16(end+2*j)+1)):
                    d=u16(delta+2*j);r=u16(offset+2*j)
                    glyph=(cp+d)&65535 if not r else u16(offset+2*j+r+2*(cp-u16(start+2*j)))
                    if glyph:result.add(cp)
    return result

def main():
    p=argparse.ArgumentParser();p.add_argument('--font',type=Path,required=True);p.add_argument('--license',type=Path,required=True);p.add_argument('--corpus',type=Path);a=p.parse_args()
    covered=cmap(a.font)
    if a.corpus:
        needed={ord(c) for c in a.corpus.read_text('utf-8')}|{cp for cp in covered if cp<0x3000 or 0x3040<=cp<0x3100}
        missing=needed-covered-{9,10,13}
        if missing:raise ValueError(f'Missing font characters: {sorted(missing)}')
        covered &= needed
    font=ImageFont.truetype(str(a.font),40 if a.corpus else 18)
    atlas=Image.new('RGBA',(4096,4096),(255,255,255,0));draw=ImageDraw.Draw(atlas)
    records=[bytes(14)];x=y=1;row=0;glyphs=0
    for cp in range(32,0xFFFE):
        if cp not in covered or 0xD800<=cp<=0xF8FF:
            records.append(struct.pack('<HHHHhhH',0,0,0,0,0,0,10));continue
        ch=chr(cp);left,top,right,bottom=font.getbbox(ch,anchor='ls');w,h=right-left,bottom-top
        if x+w+1>4096:x=1;y+=row+1;row=0
        if y+h+1>4096:raise ValueError('font atlas overflow')
        draw.text((x-left,y-top),ch,font=font,anchor='ls',fill='white')
        records.append(struct.pack('<HHHHhhH',x,y,w,h,left,-top,round(font.getlength(ch))))
        x+=w+1;row=max(row,h);glyphs+=1
    fallback=records[ord('?')-31]
    for cp in range(32,0xFFFE):
        if cp not in covered:records[cp-31]=fallback
    # The CJK font gives the curly apostrophe a full-em advance. French and
    # other Latin text use it inside words, where the narrow glyph fits.
    if 0x2019 in covered and 0x27 in covered:
        records[0x2019-31]=records[0x27-31]
    header=bytearray(0x60);header[:4]=b'MFNT'
    struct.pack_into('<I',header,0x1c,42 if a.corpus else 20);struct.pack_into('<I',header,0x20,len(records));struct.pack_into('<I',header,0x28,len(header))
    out=Path(__file__).resolve().parents[2]/'packages/PokemonLegendsZA/dualscreen/font'
    (out/'za.mfnt').write_bytes(header+b''.join(records));atlas.crop((0,0,4096,y+row+1)).save(out/'za.png',optimize=True)
    (out/'LICENSE-Noto.txt').write_bytes(a.license.read_bytes())
    (Path(__file__).parent/'font_coverage.generated.json').write_text(json.dumps(sorted(covered)),encoding='utf-8')
    print(f'Unicode font: {glyphs} glyphs; atlas used height {y+row}; BMP metrics {len(records)}')

if __name__=='__main__':main()
