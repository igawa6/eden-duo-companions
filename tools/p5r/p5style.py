#!/usr/bin/env python3
"""p5style -- Persona 5 Royal game-art style kit for the DSMod bottom-screen manifest.

Reusable by any page generator:

    from p5style import Kit
    kit = Kit(Path('package/dualscreen'))            # assets land in dualscreen/ui/
    w  = kit.header('ENEMY ANALYSIS')                  # red banner + title + live status
    w += kit.button(32, 968, 368, 86, 'CLOSE', 'analysis.close', 'analysis.ready')
    w += kit.number('analysis.hp', right=640, y=250, digit_set='big', digits=4,
                    need='analysis.ready')            # italic game digits, font fallback above 9999
    w += kit.tile(32, 392, 224, 250, need='analysis.ready')
    ...
    kit.write_provenance()                            # ui/PROVENANCE.json (source SPD + sprite id)

Runtime facts this kit is built on (Eden DSMod, verified in mod_ui.cpp):
  * Text: the package font is the game's own EN/FONT/FONT0.FNT, decoded by the P5R module
    (p5r_font.h). `text_scale` s draws a cap height of 5*s px; the atlas cap is 27 px and is sampled
    nearest-neighbour, so s = 5..6 is 1:1-ish and crisp; 4 is fine; 3 and below get crunchy (use a
    pre-rendered image for small static text: kit.text_image()).
  * label/value: rect[0] is the anchor; align='center' centres on rect[0], 'right' ends at rect[0].
    rect[1] is the cap top (baseline = y + 5*s).
  * image: stretched to rect with nearest sampling -> every sprite is pre-scaled to the exact rect,
    and every value-selected set (digits, stamps, portraits, plates) is padded to one cell size.
  * image by value: `bind` + `src_names` (index = bind*mul/div+add; out of range or "" -> nothing),
    `color` tints (white art -> any colour). No modulo exists, so kit.number() gives each decimal
    place its own src_names table indexed by value/10^k (table length 10^(digits-k)).
  * gating: need_bind (non-zero) and hide_bind with keep_min/keep_max (inclusive keep range).
Colours are "#AARRGGBB".
"""
from __future__ import annotations

import glob
import json
import os
from pathlib import Path

from PIL import Image, ImageDraw

from recipe import Art, Table

# ASSET_MODE=png (default): write PNGs, reference file:... (fast iteration)
# ASSET_MODE=module: write recipes (p5r_art.rec), reference module:p5r:<id> (asset-free package)
ASSET_MODE = os.environ.get('ASSET_MODE', 'png')
assert ASSET_MODE in ('png', 'module'), ASSET_MODE
# dualscreen/p5r_art.rec: NOT under modules/ -- the Android package installer accepts only
# modules/<platform>/<title>.so there and rejects the whole package otherwise.
RECIPE_FILE = 'p5r_art.rec'
# Oldest dual-screen runtime the package works with (package.json + manifest.json "min_runtime";
# Eden runtime/mod_runtime.h DualScreenRuntimeVersion). 11 = scroll regions, text dirty-rect fix,
# "module:" composite layers + repaint on module images, min_runtime gating. Both asset modes use
# scroll regions, so both need 11; older runtimes ignore the key (no gate there).
MIN_RUNTIME = 11

from p5r_paths import FONT_DIR  # noqa: E402
SPD_SOURCES = {
    'camp00': 'EN/INIT/P5CAMP_00SPD.SPD',
    'partypanel': 'EN/BATTLE/GUI/P5_BATTLE_PARTYPANEL.SPD',
    'minimap': 'EN/FIELD/PANEL/P5MINIMAP_01.SPD',
    'money': 'EN/CAMP/SHARED/MONEY.SPD',
}

# Palette (approved mock p5r_analyze_grid_gameart.png)
RED = '#FFE5191C'
BG = '#FF100C0E'
TILE = '#FF221A1E'
TILE_HI = '#FF2E2328'
INK = '#FFFFFFFF'
BLACK = '#FF000000'
DIM = '#FFAAA0A5'
HP_COL = '#FFF2E14C'   # the game's camp HP gauge yellow-green is close; used for gauges only
SP_COL = '#FF4FC1E9'


def rgba(c: str):
    v = int(c[1:], 16)
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255)


def cap_px(scale: int) -> int:
    return 5 * scale


class Kit:
    def __init__(self, dualscreen: Path, subdir: str = 'ui', mode: str | None = None, fresh_table: bool = False):
        self.root = Path(dualscreen)
        self.subdir = subdir
        self.mode = mode or ASSET_MODE
        if self.mode == 'png':
            (self.root / subdir).mkdir(parents=True, exist_ok=True)
        self.provenance: dict[str, dict] = {}
        self._digit_sets: dict[str, dict] = {}
        self._font_metrics = None
        self._shapes: dict[str, str] = {}
        self.art_of: dict[str, Art] = {}   # src -> the Art it was made from (reuse without re-reading)
        self.table = Table()
        tf = self.root / RECIPE_FILE
        if self.mode == 'module' and tf.exists() and not fresh_table:
            self.table = Table.load(tf.read_text())

    # ------------------------------------------------------------------ assets
    def _save(self, img: Art, name: str, prov: dict, rid: str | None = None) -> str:
        """PNG mode: ui/<name>.png ("file:..."). Module mode: a recipe row ("module:p5r:<id>").
        Digit cells live at the package root in PNG mode ("file:d/b7.png", 13 chars): src_names tables
        hold thousands of these and a <=15-char std::string never allocates when widgets are copied;
        module ids are <= 4 chars for the same reason ("module:p5r:" + id)."""
        assert isinstance(img, Art), ('generators hand Art (recipe.py) to _save', name)
        rel = name + '.png' if name.startswith('d/') else f'{self.subdir}/{name}.png'
        if self.mode == 'png':
            (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
            img.im.save(self.root / rel, optimize=True)
            src = 'file:' + rel
        else:
            src = 'module:p5r:' + self.table.add(img, rel, rid)
            prov = dict(prov, recipe=src[11:])
        self.provenance[rel] = prov
        self.art_of[src] = img
        return src

    def load_sprite(self, spd: str, sid: int) -> Art:
        return Art.sprite(spd, sid)

    @staticmethod
    def fit(img: Art, height: int | None = None, width: int | None = None) -> Art:
        return img.fit(h=height, w=width)

    @staticmethod
    def recolor(img: Art, color: str) -> Art:
        return img.recolor(color)

    @staticmethod
    def pad(img: Art, w: int, h: int, align: str = 'center') -> Art:
        return img.pad(w, h, align)

    def sprite(self, spd: str, sid: int, name: str, height=None, width=None, color=None,
               box=None, align='center', note='', rid=None) -> tuple[str, int, int]:
        """Crop (already cropped by spd_extract), scale, recolour, pad; returns (src, w, h)."""
        img = self.fit(self.load_sprite(spd, sid), height, width)
        if color:
            img = self.recolor(img, color)
        if box:
            img = self.pad(img, box[0], box[1], align)
        src = self._save(img, name, {'source': SPD_SOURCES[spd], 'sprite_id': sid,
                                     'size': list(img.size), 'note': note}, rid)
        return src, img.width, img.height

    def sprite_set(self, spd: str, ids, prefix: str, height=None, width=None, color=None,
                   align='center', pad_x=0, bg=None, cell=None) -> tuple[list[str], int, int]:
        """A value-selected family: all members padded to one common cell."""
        imgs = [self.fit(self.load_sprite(spd, i), height, width) for i in ids]
        if color:
            imgs = [self.recolor(im, color) for im in imgs]
        cw = max(im.width for im in imgs) + pad_x
        ch = max(im.height for im in imgs)
        if cell:
            cw, ch = max(cw, cell[0]), max(ch, cell[1])
        srcs = []
        for i, im in zip(ids, imgs):
            cell = self.pad(im, cw, ch, align)
            if bg:  # opaque backing (covers whatever is drawn under it)
                cell = cell.underlay(bg)
            srcs.append(self._save(cell, f'{prefix}{ids.index(i)}',
                                   {'source': SPD_SOURCES[spd], 'sprite_id': i, 'size': [cw, ch]}))
        return srcs, cw, ch

    def shape(self, kind: str, w: int, h: int, name: str | None = None, fill='#FFFFFFFF',
              line=None, skew=None) -> str:
        """Game-style flat shapes, 4x supersampled. White by default (tint with widget color)."""
        name = name or f'{kind}_{w}x{h}' + ('' if fill == '#FFFFFFFF' else '_' + fill[3:].lower()) + \
            ('_l' if line else '')
        rel = f'{self.subdir}/{name}.png'
        if rel in self._shapes:
            return self._shapes[rel]
        s = 4
        W, H = w * s, h * s
        k = (skew if skew is not None else max(6, h // 7)) * s
        if kind == 'banner':      # page title band: long, slightly rising, sheared right end
            poly = [(0, 5 * s), (W, 0), (W - k * 2, H - 4 * s), (0, H)]
        elif kind == 'button':    # touch button: parallelogram with a tilted top edge
            poly = [(k, 6 * s), (W, 0), (W - k, H - 6 * s), (0, H)]
        elif kind == 'tile':      # panel: gentle parallelogram
            poly = [(k, 0), (W, 0), (W - k, H), (0, H)]
        elif kind == 'plate':     # name plate: rhombus-ish
            poly = [(k, 0), (W, 3 * s), (W - k, H), (0, H - 3 * s)]
        elif kind == 'rect':
            poly = [(0, 0), (W, 0), (W, H), (0, H)]
        else:
            raise ValueError(kind)
        img = Art.new(W, H).polygon(poly, fill)
        if line:  # red top rule, following the top edge
            col, t = line
            img = img.line([poly[0], poly[1]], col, t * s)
        img = img.resize(w, h)
        src = self._save(img, name, {'source': 'generated (p5style.shape)', 'kind': kind})
        self._shapes[rel] = src
        return src

    # Small static text, pre-rendered from the decoded game font with a proper downscale filter.
    def text_art(self, text: str, px_cap: float, color=INK, track: float = 0) -> Art:
        return Art.text(text, px_cap, color, track)

    def text_image(self, text: str, px_cap: float, name: str, color=INK) -> tuple[str, int, int]:
        img = self.text_art(text, px_cap, color)
        src = self._save(img, name, {'source': 'EN/FONT/FONT0.FNT (pre-rendered)', 'text': text})
        return src, img.width, img.height

    def _font(self):
        if self._font_metrics is None:
            atlas = Image.open(FONT_DIR / 'EN_FONT_FONT0_atlas.png')
            cuts = json.load(open(FONT_DIR / 'EN_FONT_FONT0_metrics.json'))['cuts']
            self._font_metrics = (atlas, cuts)
        return self._font_metrics

    def measure(self, text: str, scale: int) -> int:
        """Width the runtime will give `text` at `scale` with the FONT0 metrics (p5r_font.h)."""
        _, cuts = self._font()
        k = cap_px(scale) / 27.0
        w = 0.0
        for ch in text:
            w += 13 * k if ch == ' ' else (cuts[ord(ch) - 0x20][1] - cuts[ord(ch) - 0x20][0]) * k
        return int(w)

    def digits(self, key: str, height: int, base: int = 274, pitch: float = 0.78) -> dict:
        """Italic camp digits 0-9 (P5CAMP_00SPD #274-283 large / #264-273 small) as one cell set."""
        if key not in self._digit_sets:
            srcs, cw, ch = self.sprite_set('camp00', list(range(base, base + 10)), f'd/{key}',
                                           height=height)
            self._digit_sets[key] = dict(srcs=srcs, w=cw, h=ch, pitch=round(cw * pitch))
        return self._digit_sets[key]

    def write_provenance(self):
        prov = self.provenance
        if self.mode == 'png':
            path = self.root / self.subdir / 'PROVENANCE.json'
            path.write_text(json.dumps(dict(sorted(prov.items())), indent=1, ensure_ascii=False) + '\n')
        else:
            tf = self.root / RECIPE_FILE
            tf.parent.mkdir(parents=True, exist_ok=True)
            tf.write_text(self.table.text())

    # ------------------------------------------------------------------ widgets
    @staticmethod
    def image(src, x, y, w, h, **kw) -> dict:
        kw.setdefault('color', INK)  # the runtime's default widget colour is #FFE6ECF2, not white
        return dict(type='image', src=src, rect=[x, y, w, h], **kw)

    @staticmethod
    def text(x, y, text='', scale=5, color=INK, w=0, align='left', **kw) -> dict:
        h = cap_px(scale) + 3 * scale  # module lowers glyphs; descenders reach 2.6*scale below cap
        return dict(type='label', rect=[x, y, w, h], text=text, text_scale=scale, color=color,
                    align=align, **kw)

    @staticmethod
    def value(x, y, bind, scale=5, color=INK, prefix='', align='left', **kw) -> dict:
        return dict(type='value', rect=[x, y, 0, cap_px(scale) + 3 * scale], bind=bind, text=prefix,
                    text_scale=scale, color=color, align=align, **kw)

    def header(self, title: str, status=True, title_scale=7) -> list:
        w = [self.image(self.shape('rect', 1240, 10), 0, 0, 1240, 10, color=RED),
             self.image(self.shape('banner', 760, 92), 0, 24, 760, 92, color=RED),
             self.text(36, 70 - cap_px(title_scale) // 2, title, title_scale)]
        if status:
            w.append(self.text(1208, 56, bind_text='status', scale=4, color=RED, align='right'))
        return w

    def button(self, x, y, w, h, label, action, gate, scale=6, color=RED, ink=INK) -> list:
        src = self.shape('button', w, h)
        return [self.image(src, x, y, w, h, color=color, on_tap=action, need_bind=gate),
                self.text(x + w // 2, y + (h - cap_px(scale)) // 2, label, scale, ink,
                          align='center', on_tap=action, need_bind=gate)]

    def tile(self, x, y, w, h, need=None, line=True, fill=TILE, skew=None, **kw) -> dict:
        src = self.shape('tile', w, h, fill=fill, line=(RED, 4) if line else None, skew=skew)
        kw = dict(kw)
        if need:
            kw['need_bind'] = need
        return self.image(src, x, y, w, h, **kw)

    def number(self, bind, right, y, digit_set='big', digits=3, need=None, fallback_scale=8,
               color=None, height=None, base=274) -> list:
        """Right-aligned italic game digits for 0..10^digits-1, runtime-font value above that."""
        ds = self.digits(digit_set, height or 60, base)
        out = []
        limit = 10 ** digits - 1
        for k in range(digits):
            names = []
            for i in range(10 ** (digits - k)):
                names.append(ds['srcs'][i % 10] if (i > 0 or k == 0) else '')
            x = right - ds['w'] - k * ds['pitch']
            wd = dict(type='image', src=ds['srcs'][0], rect=[x, y, ds['w'], ds['h']], bind=bind,
                      src_names=names, hide_bind=bind, keep_max=limit, color=color or INK)
            if k:
                wd['div'] = 10 ** k
            if need:
                wd['need_bind'] = need
            out.append(wd)
        fb = self.value(right, y + (ds['h'] - cap_px(fallback_scale)) // 2, bind, fallback_scale,
                        color or INK, align='right', hide_bind=bind, keep_min=limit + 1)
        if need:
            fb['need_bind'] = need
        out.append(fb)
        return out

    def by_value(self, bind, srcs: list[str], x, y, w, h, need=None, **kw) -> dict:
        """One image picked by an integer bind (src_names). "" entries draw nothing."""
        kw.setdefault('color', INK)
        wd = dict(type='image', src=next((s for s in srcs if s), ''), rect=[x, y, w, h], bind=bind,
                  src_names=srcs, **kw)
        if need:
            wd['need_bind'] = need
        return wd
