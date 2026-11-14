#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Recorder isolation and refusal tests, using synthetic texture leaves only."""
import subprocess,sys,unittest
from pathlib import Path
class RecipeContract(unittest.TestCase):
    def test_constants_independent_of_native_pixels(self):
        subprocess.run([sys.executable,'-c', '''
import ui_recipe as r
from PIL import Image,ImageDraw
r.install()
def recipe(colour):
 im=r.source(Image.new('RGBA',(12,12),colour),'39000009',1)
 im=im.resize((20,18),Image.Resampling.LANCZOS)
 ImageDraw.Draw(im).rounded_rectangle((3,3,16,14),3,outline=(80,50,20,200),width=2)
 return r.ref(im)
a=recipe((255,0,0,255))
b=recipe((0,0,255,255))
assert a==b, 'native pixels must never enter recipe constants'
cut=r.region(a,(4,4,8,9))
assert r.sizes[cut]==(4,5)
assert cut==r.region(b,(4,4,8,9)), 'regional constants must stay independent of native pixels'
resampled={i for i,n in enumerate(r.nodes) if n[0]=='resize'}
def ancestors(n):
 return {n}.union(*(ancestors(i) for i in r.nodes[n][1]))
assert resampled & ancestors(cut), 'crop must preserve the original resampling grid'
try:
 r.source(Image.new('RGBA',(4,4)),'39000009',1).transpose(Image.Transpose.FLIP_LEFT_RIGHT)
except ValueError:pass
else:raise AssertionError('unrecorded image operation accepted')
assert all(n[0]!='constant' or n[2][0:2]!=[12,12] for n in r.nodes)
'''],cwd=Path(__file__).parent,check=True)
if __name__=='__main__':unittest.main()
