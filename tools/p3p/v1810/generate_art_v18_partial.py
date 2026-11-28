#!/usr/bin/env python3
"""Regenerate 31 verified procedural UI PNGs from the v1.8.0 design source.

This is a historical *partial* source reconstruction for v1.8.10 FINAL;
it does NOT regenerate its other 31 PNG assets, and does not package
copyrighted game art. Requires Pillow and a locally installed Inter font.

Run: python3 generate_art_v18_partial.py --output /tmp/p3p-v1.8-art
"""
from __future__ import annotations
from argparse import ArgumentParser
from pathlib import Path
import os
from PIL import Image, ImageDraw, ImageFont

T={
    'width':1240,'height':1080,
    'gold':(255,182,53), 'gold_light':(255,224,124),
    'navy':(8,48,123), 'teal':(78,193,249),
    'blue_line':(57,117,183), 'text_blue':(14,54,117)
}
font_title=Path(os.environ.get('P3P_INTER_FONT_DIR', '/usr/share/fonts/opentype/inter')) / 'Inter-BlackItalic.otf'
if not font_title.exists():
    raise RuntimeError('This historical generator requires a locally installed Inter Black Italic OTF. Font binaries are NOT redistributed.')

TITLES={
    'personas':'PERSONA','links':'S.LINK','skills':'SKILL',
    'equip':'EQUIP','stats':'STATUS','items':'ITEM',
    'calendar':'CALENDAR','battle':'BATTLE','map':'MAP','talk':'TALK'
}

def draw_title_image(word:str, path:Path):
    ss=3
    im=Image.new('RGBA',(T['width']*ss,116*ss),(0,0,0,0))
    dr=ImageDraw.Draw(im)
    fs=92*ss
    ft=ImageFont.truetype(str(font_title),fs)
    bb=dr.textbbox((0,0),word,font=ft,stroke_width=0)
    width=bb[2]-bb[0]
    maxw=880*ss
    layer=Image.new('RGBA',(width+40*ss,105*ss),(0,0,0,0))
    ld=ImageDraw.Draw(layer)
    at=(12*ss-bb[0],6*ss-bb[1])
    ld.text((at[0]+2*ss,at[1]+3*ss),word,font=ft,fill=(8,54,139,255),stroke_width=2*ss,stroke_fill=(8,54,139,255))
    ld.text(at,word,font=ft,fill=(*T['gold'],255),stroke_width=0)
    if layer.width>maxw:
        layer=layer.resize((maxw,layer.height),Image.Resampling.LANCZOS)
    im.alpha_composite(layer,(46*ss,4*ss))
    dr.line([(46*ss,92*ss),(940*ss,92*ss)],fill=(*T['navy'],85),width=2*ss)
    dr.line([(46*ss,95*ss),(145*ss,95*ss)],fill=(*T['gold'],220),width=3*ss)
    im=im.resize((1240,116),Image.Resampling.LANCZOS)
    im.save(path)

def draw_watermark(word:str,path:Path,position:str,female:bool=False):
    ss=3
    im=Image.new('RGBA',(1240*ss,1080*ss),(0,0,0,0))
    dr=ImageDraw.Draw(im)
    ft=ImageFont.truetype(str(font_title),154*ss)
    bb=dr.textbbox((0,0),word,font=ft)
    textw=bb[2]-bb[0]; texth=bb[3]-bb[1]
    maxw=840*ss
    layer=Image.new('RGBA',(textw+30*ss,texth+15*ss),(0,0,0,0))
    ld=ImageDraw.Draw(layer)
    ld.text((10*ss-bb[0],6*ss-bb[1]),word,font=ft,fill=((187,31,97,90) if female else (13,123,220,94)))
    if layer.width>maxw:
        layer=layer.resize((maxw,layer.height),Image.Resampling.LANCZOS)
    bottom_y={'calendar':938,'links':916,'default':930}.get(position,930)*ss
    x=(1190*ss-layer.width)
    y=bottom_y-layer.height
    im.alpha_composite(layer,(max(290*ss,x),max(0,y)))
    im.resize((1240,1080),Image.Resampling.LANCZOS).save(path)

def draw_stats_stripe(path:Path):
    im=Image.new('RGBA',(1240,1080),(0,0,0,0))
    dr=ImageDraw.Draw(im,'RGBA')
    dr.rounded_rectangle((41,247,1203,302),radius=7,fill=(239,249,255,200))
    dr.line([(47,301),(1200,301)],fill=(45,115,188,95),width=2)
    im.save(path)

def main():
    parser=ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args=parser.parse_args()
    dest=args.output
    dest.mkdir(parents=True, exist_ok=True)
    for pid,word in TITLES.items():
        draw_title_image(word,dest/f'ui_title_{pid}.png')
        draw_watermark(word,dest/f'ui_ghost_{pid}_blue.png',pid,False)
        draw_watermark(word,dest/f'ui_ghost_{pid}_red.png',pid,True)
    draw_stats_stripe(dest/'stats_header_strip.png')
    print(f'Generated {len(list(dest.glob("*.png")))} original procedural UI assets in {dest}')

if __name__=='__main__':
    main()
