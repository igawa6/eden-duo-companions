#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare real module pieces with full module canvases, without tolerances."""
import json
import os
import subprocess
from pathlib import Path
from PIL import Image,ImageChops
from backgrounds import FRAME_BOXES


def check(binary,tmp):
    root=Path(__file__).resolve().parents[2]
    metadata=json.loads((root/'tools/resident-evil-4/background_layout.json').read_text())
    cache={};results={}
    def load(key):
        if key not in cache:
            raw=Path(tmp)/'background.rgba'
            run=subprocess.run([str(binary.resolve()),'--image',key,str(raw)],capture_output=True,text=True,check=True)
            w,h=map(int,run.stdout.splitlines()[-1].split())
            cache[key]=Image.frombytes('RGBA',(w,h),raw.read_bytes())
        return cache[key]
    for key,entries in metadata['layouts'].items():
        original=load(key);restored=Image.new('RGBA',original.size)
        for e in entries:
            x,y,w,h=e['rect'];piece=load(e['src'])
            assert list(piece.size)==metadata['sizes'][e['src']],e
            assert piece.size==(w,h) or piece.size==(1,1),e
            if piece.size!=(w,h):piece=piece.resize((w,h),Image.Resampling.NEAREST)
            restored.paste(piece,(x,y))
        diff=ImageChops.difference(original,restored)
        assert diff.getbbox(alpha_only=False) is None,(key,'RGBA reconstruction differs')
        # A raw RGBA equality proves every underlay/fade value; also exercise the
        # composited canvas on opaque and transparent destinations explicitly.
        for colour in ((0,0,0,0),(0,0,0,255),(31,67,113,255)):
            a=Image.new('RGBA',original.size,colour);b=a.copy()
            a.alpha_composite(original);b.alpha_composite(restored)
            assert ImageChops.difference(a,b).getbbox(alpha_only=False) is None,key
        results[key]=dict(passed=True,changed_pixels=0,width=original.width,height=original.height)
    for level in range(15):
        restored=load(metadata['hp']['base']['src']).copy()
        for wedge in range(1,level+1):
            e=metadata['hp'][str(wedge)];restored.alpha_composite(load(e['src']),tuple(e['rect'][:2]))
        key=f'module:re4/ui/hp-{level}'
        assert ImageChops.difference(load(key),restored).getbbox(alpha_only=False) is None,key
        results[key]=dict(passed=True,changed_pixels=0)
    original=load('module:re4/map/frame');restored=Image.new('RGBA',original.size)
    for n,(x,y,r,b) in enumerate(FRAME_BOXES):
        strip=load(f'module:re4/map/frame/{n}')
        assert strip.size==(r-x,b-y)
        restored.paste(strip,(x,y))
    assert ImageChops.difference(original,restored).getbbox(alpha_only=False) is None,'map frame strips'
    results['module:re4/map/frame']=dict(passed=True,changed_pixels=0)
    # Removing neutral below standby is valid only while standby stays opaque.
    assert load('module:re4/ui/standby').getchannel('A').getextrema()==(255,255)
    report=os.environ.get('RE4_TEST_BACKGROUND_REPORT')
    if report:Path(report).write_text(json.dumps(results,indent=2)+'\n')
    return results
