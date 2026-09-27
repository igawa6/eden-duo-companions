#!/usr/bin/env python3
"""recipe -- game art as small deterministic recipes (asset-free package).

An `Art` is a PIL image plus the op list that rebuilds it from the user's own romfs. Every method
evaluates eagerly with Pillow (exactly the calls the generators made before, so PNG mode output is
unchanged) and appends the matching recipe op. In module mode the generators save the op list
(p5r_art.rec) and reference "module:p5r:<id>"; the P5R module replays it with Pillow-exact
C++ ops (p5r_recipes.h).

Ops (stack machine, see recipes.md): sources push an image, transforms rewrite the top, `a`/`P`
pop the top and composite/paste it onto the next one, drawing ops paint on the top in place.
"""
from __future__ import annotations

import io
import struct
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

from p5r_paths import FONT_DIR, ROMFS  # noqa: F401 (builder inputs, see p5r_paths.py)

# SPR0 sheets by alias (romfs path). The alias table is written into the recipe file header.
SHEETS = {
    'camp00': 'EN/INIT/P5CAMP_00SPD.SPD',
    'partypanel': 'EN/BATTLE/GUI/P5_BATTLE_PARTYPANEL.SPD',
    'minimap': 'EN/FIELD/PANEL/P5MINIMAP_01.SPD',
    'money': 'EN/CAMP/SHARED/MONEY.SPD',
    'nowload': 'EN/FIELD/PANEL/WIPE/SYMBOL/NOWLOADING.SPD',
    'saferoom': 'EN/FIELD/SAFETY_ROOM/P5_AZITO_SAFEROOM.SPD',
    'btlbtn': 'EN/BATTLE/GUI/BTL_BTN.SPD',
    'damage': 'EN/BATTLE/GUI/DAMAGE_DATA.SPD',
    'musmenu': 'EN/MYPALACE/SOUND/MENU.SPD',
    'mustitle': 'EN/MYPALACE/SOUND/MUSIC_TITLE_001.SPD',
}
ICON_DDS = 'EN/FONT/ICON.DDS'
FONT_FNT = 'EN/FONT/FONT0.FNT'

_sheet_cache: dict[str, dict[int, Image.Image]] = {}
_file_cache: dict[str, Image.Image] = {}
_font = None


def _romfs_file(rel: str) -> Path:
    """Local unpacked romfs (ALL_USEU; the update's PATCH1 overrides it like the module's Romfs)."""
    patch = ROMFS.parent / 'PATCH1' / rel
    return patch if patch.exists() else ROMFS / rel


def _sheet(alias: str) -> dict[int, Image.Image]:
    """All sprites of one SPR0 sheet, cropped in memory exactly like spd_extract.py."""
    if alias not in _sheet_cache:
        d = _romfs_file(SHEETS[alias]).read_bytes()
        tc, sc = struct.unpack_from('<HH', d, 0x14)
        to, so = struct.unpack_from('<II', d, 0x18)
        tex = {}
        for i in range(tc):
            tid, _, off, size, _w, _h = struct.unpack_from('<IIIIII', d, to + i * 0x30)
            tex[tid] = (off, size)
        dec, out = {}, {}
        for i in range(sc):
            b = so + i * 0xA0
            sid, tid = struct.unpack_from('<II', d, b)
            x, y, w, h = struct.unpack_from('<IIII', d, b + 0x20)
            if not w or not h or tid not in tex:
                continue
            if tid not in dec:
                off, size = tex[tid]
                dec[tid] = Image.open(io.BytesIO(d[off:off + size])).convert('RGBA')
            out[sid] = dec[tid].crop((x, y, x + w, y + h))
        _sheet_cache[alias] = out
    return _sheet_cache[alias]


def _dds(rel: str) -> Image.Image:
    if rel not in _file_cache:
        _file_cache[rel] = Image.open(_romfs_file(rel)).convert('RGBA')
    return _file_cache[rel]


def font():
    """(atlas LA image, cuts) of EN/FONT/FONT0.FNT, decoded by fnt_decode.py."""
    global _font
    if _font is None:
        import json
        atlas = Image.open(FONT_DIR / 'EN_FONT_FONT0_atlas.png')
        cuts = json.load(open(FONT_DIR / 'EN_FONT_FONT0_metrics.json'))['cuts']
        _font = (atlas, cuts)
    return _font


def hexc(c) -> str:
    """'#AARRGGBB' / (r, g, b[, a]) -> 'AARRGGBB'."""
    if isinstance(c, str):
        assert c[0] == '#' and len(c) == 9, c
        return c[1:].upper()
    r, g, b = c[:3]
    a = c[3] if len(c) > 3 else 255
    return '%02X%02X%02X%02X' % (a, r, g, b)


def rgba_of(c) -> tuple:
    if isinstance(c, str):
        v = int(c[1:], 16)
        return ((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255)
    return tuple(c) if len(c) == 4 else tuple(c) + (255,)


def _esc(text: str) -> str:
    out = []
    for ch in text:
        if ch in ' ,%' or ord(ch) < 0x21 or ord(ch) > 0x7E:
            assert ord(ch) < 0x100, ('text op is Latin-1 only', text)
            out.append('%%%02X' % ord(ch))
        else:
            out.append(ch)
    return ''.join(out)


def _num(v) -> str:
    if isinstance(v, float) and v.is_integer():
        v = int(v)
    return repr(v) if isinstance(v, float) else str(v)


def _op(name: str, *args) -> str:
    return name if not args else name + ':' + ','.join(_num(a) for a in args)


class Art:
    """PIL RGBA image + the recipe ops that rebuild it (immutable: every method returns a new Art)."""
    __slots__ = ('im', 'ops')

    def __init__(self, im: Image.Image, ops: list[str]):
        assert im.mode == 'RGBA', im.mode
        self.im, self.ops = im, ops

    # --------------------------------------------------------------- info
    @property
    def size(self):
        return self.im.size

    @property
    def width(self):
        return self.im.width

    @property
    def height(self):
        return self.im.height

    def recipe(self) -> str:
        return ' '.join(self.ops)

    def _then(self, im, *ops) -> 'Art':
        return Art(im, self.ops + list(ops))

    # --------------------------------------------------------------- sources
    @staticmethod
    def sprite(alias: str, sid: int) -> 'Art':
        return Art(_sheet(alias)[sid].copy(), [_op('s', alias, sid)])

    @staticmethod
    def dds(rel: str) -> 'Art':
        return Art(_dds(rel).copy(), [_op('d', rel)])

    @staticmethod
    def icon(n: int) -> 'Art':
        """EN/FONT/ICON.DDS: 6 x 14 cells, pitch 126 x 45, content 122 x 41 at (3 + 126c, 3 + 45r)."""
        c, r = n % 6, n // 6
        im = _dds(ICON_DDS).crop((3 + 126 * c, 3 + 45 * r, 3 + 126 * c + 122, 3 + 45 * r + 41))
        return Art(im, [_op('i', n)])

    @staticmethod
    def new(w: int, h: int, color=(0, 0, 0, 0)) -> 'Art':
        c = rgba_of(color)
        return Art(Image.new('RGBA', (w, h), c), [_op('n', w, h, hexc(c)) if c != (0, 0, 0, 0) else _op('n', w, h)])

    @staticmethod
    def text(text: str, cap: float, color, track: float = 0) -> 'Art':
        """One line in the game font (FONT0), cap height `cap` px, recoloured (alpha scaled by the
        colour's alpha), `track` px added after every glyph and space. Same maths as
        p5style.Kit.text_image (track 0) and waiting._text."""
        atlas, cuts = font()
        cells = []
        for ch in text:
            if ch == ' ':
                cells.append(None)
                continue
            i = ord(ch) - 0x20
            assert 0 <= i < 0x5F, ('text op draws printable ASCII', text)
            lft, rgt = cuts[i]
            cells.append(atlas.crop(((i % 16) * 48 + lft, (i // 16) * 48, (i % 16) * 48 + rgt, (i // 16) * 48 + 48)))
        w = sum(13 if c is None else c.width for c in cells) + int(track * len(cells))
        line = Image.new('RGBA', (w, 48), (0, 0, 0, 0))
        x = 0
        for c in cells:
            if c is not None:
                line.alpha_composite(c.convert('RGBA'), (x, 0))
                x += c.width
            else:
                x += 13
            x += int(track)
        k = cap / 27.0
        line = line.resize((max(1, round(w * k)), round(48 * k)), Image.LANCZOS)
        c = rgba_of(color)
        out = Image.new('RGBA', line.size, c[:3] + (255,))
        a = line.getchannel('A')
        if c[3] != 255:
            a = a.point(lambda v: v * c[3] // 255)
        out.putalpha(a)
        return Art(out, [_op('t', cap, hexc(c), track, _esc(text))])

    # --------------------------------------------------------------- transforms
    def crop(self, box) -> 'Art':
        box = tuple(int(v) for v in box)
        if box == (0, 0) + self.size:
            return self
        return self._then(self.im.crop(box), _op('c', *box))

    def bbox(self) -> 'Art':
        """Alpha bounding box (baked as an explicit crop)."""
        return self.crop(self.im.getbbox())

    def resize(self, w: int, h: int, filt: str = 'l') -> 'Art':
        if (w, h) == self.size:
            return self
        res = {'l': Image.LANCZOS, 'c': Image.BICUBIC, 'b': Image.BILINEAR}[filt]
        return self._then(self.im.resize((w, h), res), _op('z', w, h) if filt == 'l' else _op('z', w, h, filt))

    def fit(self, h=None, w=None) -> 'Art':
        """p5style.Kit.fit / menu_art.fit: keep aspect, height and/or width bound, round()."""
        if h and w:
            k = min(h / self.height, w / self.width)
        elif h:
            k = h / self.height
        elif w:
            k = w / self.width
        else:
            return self
        return self.resize(max(1, round(self.width * k)), max(1, round(self.height * k)))

    def tint(self, mul) -> 'Art':
        """RGB * mul // 255 (menu_art.tint, map_gen.tint)."""
        mul = tuple(mul[:3])
        if mul == (255, 255, 255):
            return self
        r, g, b, a = self.im.split()
        im = Image.merge('RGBA', (r.point(lambda v: v * mul[0] // 255), g.point(lambda v: v * mul[1] // 255),
                                  b.point(lambda v: v * mul[2] // 255), a))
        return self._then(im, _op('m', *mul))

    def recolor(self, color) -> 'Art':
        """Solid colour, alpha kept (scaled by the colour's alpha): Kit.recolor / waiting._tint."""
        c = rgba_of(color)
        solid = Image.new('RGBA', self.size, c[:3] + (255,))
        a = self.im.getchannel('A')
        if c[3] != 255:
            a = a.point(lambda v: v * c[3] // 255)
        solid.putalpha(a)
        return self._then(solid, _op('k', hexc(c)))

    def pad(self, w: int, h: int, align: str = 'center') -> 'Art':
        x = {'left': 0, 'center': (w - self.width) // 2, 'right': w - self.width}[align]
        y = (h - self.height) // 2
        # (x or y < 0 is legal: PIL alpha_composite clips, the art is cut)
        if (w, h) == self.size:
            return self
        out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        out.alpha_composite(self.im, (x, y))
        return self._then(out, _op('p', w, h, align[0]))

    def underlay(self, color) -> 'Art':
        back = Image.new('RGBA', self.size, rgba_of(color))
        back.alpha_composite(self.im)
        return self._then(back, _op('u', hexc(color)))

    def rotate(self, deg: float, expand: bool = True) -> 'Art':
        im = self.im.rotate(deg, resample=Image.BICUBIC, expand=expand)
        return self._then(im, _op('o', deg) if expand else _op('o', deg, 0))

    def rim(self, r: int = 3) -> 'Art':
        """Black outline around the alpha (MaxFilter(2r+1)), art on top (build_menu.rimmed)."""
        a = self.im.getchannel('A').filter(ImageFilter.MaxFilter(2 * r + 1))
        out = Image.new('RGBA', self.size, (0, 0, 0, 0))
        out.paste((0, 0, 0, 255), (0, 0), a)
        out.alpha_composite(self.im)
        return self._then(out, _op('x', r))

    def fade_right(self, w: int, fade: int = 56) -> 'Art':
        """music_art.fade_right: crop to w, fade the last `fade` px out."""
        if self.width <= w:
            return self
        im = self.im.crop((0, 0, w, self.height))
        a = im.getchannel('A')
        ramp = Image.new('L', (fade, im.height))
        ramp.putdata([round(255 * (1 - x / (fade - 1))) for _y in range(im.height) for x in range(fade)])
        tail = ImageChops.multiply(a.crop((w - fade, 0, w, im.height)), ramp)
        a.paste(tail, (w - fade, 0))
        im.putalpha(a)
        return self._then(im, _op('f', w, fade))

    def fog(self, bg, num: int, den: int) -> 'Art':
        """map_gen.red_patch: opaque bg, the art's red channel over it at alpha round(a*num/den)."""
        ratio = num / den
        r, g, b, a = self.im.split()
        out = Image.new('RGBA', self.size, rgba_of(bg)[:3] + (255,))
        red = Image.merge('RGBA', (r, Image.new('L', self.size, 0), Image.new('L', self.size, 0),
                                   a.point(lambda x: round(x * ratio))))
        out.alpha_composite(red)
        return self._then(out, _op('g', hexc(bg), num, den))

    # --------------------------------------------------------------- composition
    def over(self, top: 'Art', x: int = 0, y: int = 0) -> 'Art':
        """alpha_composite `top` onto a copy of self at (x, y)."""
        im = self.im.copy()
        im.alpha_composite(top.im, (int(x), int(y)))
        return Art(im, self.ops + top.ops + [_op('a', int(x), int(y))])

    def paste(self, top: 'Art', x: int = 0, y: int = 0) -> 'Art':
        im = self.im.copy()
        im.paste(top.im, (int(x), int(y)))
        return Art(im, self.ops + top.ops + [_op('P', int(x), int(y))])

    # --------------------------------------------------------------- drawing (PIL ImageDraw, blend 0)
    def _draw(self, fn, op) -> 'Art':
        im = self.im.copy()
        fn(ImageDraw.Draw(im))
        return self._then(im, op)

    @staticmethod
    def _pts(pts):
        flat = []
        for p in pts:
            flat += [int(p[0]), int(p[1])]
        return flat

    def polygon(self, pts, fill) -> 'Art':
        flat = self._pts(pts)
        return self._draw(lambda d: d.polygon(pts, fill=rgba_of(fill)), _op('G', hexc(fill), *flat))

    def polygon_outline(self, pts, color, width: int) -> 'Art':
        flat = self._pts(pts)
        return self._draw(lambda d: d.polygon(pts, outline=rgba_of(color), width=width),
                          _op('H', hexc(color), width, *flat))

    def line(self, pts, color, width: int = 1) -> 'Art':
        flat = self._pts(pts)
        return self._draw(lambda d: d.line(pts, fill=rgba_of(color), width=width),
                          _op('L', hexc(color), width, *flat))

    def ellipse(self, box, fill) -> 'Art':
        box = [int(v) for v in box]
        return self._draw(lambda d: d.ellipse(box, fill=rgba_of(fill)), _op('E', hexc(fill), *box))

    def ellipse_outline(self, box, color, width: int) -> 'Art':
        box = [int(v) for v in box]
        return self._draw(lambda d: d.ellipse(box, outline=rgba_of(color), width=width),
                          _op('e', hexc(color), width, *box))

    def pieslice(self, box, start: float, end: float, fill) -> 'Art':
        box = [int(v) for v in box]
        return self._draw(lambda d: d.pieslice(box, start, end, fill=rgba_of(fill)),
                          _op('Q', hexc(fill), *box, start, end))

    def rectangle(self, box, fill) -> 'Art':
        box = [int(v) for v in box]
        return self._draw(lambda d: d.rectangle(box, fill=rgba_of(fill)), _op('R', hexc(fill), *box))


# ------------------------------------------------------------------ recipe table (module mode)
class Table:
    """id -> recipe. Generic ids are short base-36 upper-case strings ("module:p5r:" + <= 4 chars
    stays inside std::string's 15-char SSO buffer, like the old "file:d/b7.png" digit names);
    families addressed by a src_format / {i} template get an explicit lower-case prefixed id."""
    ALPH = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'

    def __init__(self):
        self.by_recipe: dict[str, str] = {}
        self.rows: dict[str, tuple[str, str]] = {}   # id -> (recipe, note)
        self.n = 0

    def _next(self) -> str:
        while True:
            v, s = self.n, ''
            self.n += 1
            while True:
                s = self.ALPH[v % 36] + s
                v //= 36
                if not v:
                    break
            if s not in self.rows:
                return s

    def add(self, art: Art, note: str = '', rid: str | None = None) -> str:
        return self.add_recipe(art.recipe(), note, rid)

    def add_recipe(self, rec: str, note: str = '', rid: str | None = None) -> str:
        if rid is None:
            if rec in self.by_recipe:
                return self.by_recipe[rec]
            rid = self._next()
        else:
            assert rid[0].islower(), rid
            if rid in self.rows:
                assert self.rows[rid][0] == rec, ('recipe id reused for different art', rid)
                return rid
        self.rows[rid] = (rec, note)
        self.by_recipe.setdefault(rec, rid)
        return rid

    @classmethod
    def load(cls, text: str) -> 'Table':
        t = cls()
        for line in text.splitlines()[1:]:
            if not line or line[0] in '@#':
                continue
            rid, rest = line.split(' ', 1)
            rec, _, note = rest.partition('  # ')
            t.rows[rid] = (rec, note)
            t.by_recipe.setdefault(rec, rid)
        t.n = 0
        return t

    def text(self) -> str:
        lines = ['P5RREC 1']
        for alias, path in SHEETS.items():
            lines.append(f'@{alias} {path}')
        for rid, (rec, note) in self.rows.items():
            lines.append(f'{rid} {rec}' + (f'  # {note}' if note else ''))
        return '\n'.join(lines) + '\n'


# ------------------------------------------------------------------ replay (tests / tooling)
def _unesc(s: str) -> str:
    out, i = [], 0
    while i < len(s):
        if s[i] == '%' and i + 2 < len(s):
            out.append(chr(int(s[i + 1:i + 3], 16)))
            i += 3
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


def replay(ops: str, aliases: dict[str, str] | None = None) -> Art:
    """Evaluate a recipe line with the Pillow reference ops (what the module must reproduce)."""
    inv = {v: k for k, v in SHEETS.items()}
    st: list[Art] = []
    for tok in ops.split(' '):
        if not tok:
            continue
        name, _, rest = tok.partition(':')
        a = rest.split(',') if rest else []
        I = lambda i: int(a[i])  # noqa: E731
        F = lambda i: float(a[i])  # noqa: E731
        C = lambda i: '#' + a[i]  # noqa: E731
        pts = lambda v: [(v[k], v[k + 1]) for k in range(0, len(v), 2)]  # noqa: E731
        if name == 's':
            alias = a[0] if aliases is None else inv[aliases[a[0]]]
            st.append(Art.sprite(alias, I(1)))
        elif name == 'd':
            st.append(Art.dds(a[0]))
        elif name == 'i':
            st.append(Art.icon(I(0)))
        elif name == 'n':
            st.append(Art.new(I(0), I(1), C(2) if len(a) > 2 else (0, 0, 0, 0)))
        elif name == 't':
            st.append(Art.text(_unesc(a[3]), F(0), C(1), F(2)))
        elif name in ('a', 'P'):
            top = st.pop()
            st[-1] = st[-1].over(top, I(0), I(1)) if name == 'a' else st[-1].paste(top, I(0), I(1))
        else:
            t = st[-1]
            if name == 'c':
                t = t.crop([I(k) for k in range(4)])
            elif name == 'z':
                t = t.resize(I(0), I(1), a[2] if len(a) > 2 else 'l')
            elif name == 'm':
                t = t.tint([I(k) for k in range(3)])
            elif name == 'k':
                t = t.recolor(C(0))
            elif name == 'p':
                t = t.pad(I(0), I(1), {'l': 'left', 'c': 'center', 'r': 'right'}[a[2]])
            elif name == 'u':
                t = t.underlay(C(0))
            elif name == 'o':
                t = t.rotate(F(0), expand=(len(a) < 2 or a[1] != '0'))
            elif name == 'x':
                t = t.rim(I(0))
            elif name == 'f':
                t = t.fade_right(I(0), I(1))
            elif name == 'g':
                t = t.fog(C(0), I(1), I(2))
            elif name == 'G':
                t = t.polygon(pts([int(v) for v in a[1:]]), C(0))
            elif name == 'H':
                t = t.polygon_outline(pts([int(v) for v in a[2:]]), C(0), I(1))
            elif name == 'L':
                t = t.line(pts([int(v) for v in a[2:]]), C(0), I(1))
            elif name == 'E':
                t = t.ellipse([I(k) for k in range(1, 5)], C(0))
            elif name == 'e':
                t = t.ellipse_outline([I(k) for k in range(2, 6)], C(0), I(1))
            elif name == 'Q':
                t = t.pieslice([I(k) for k in range(1, 5)], F(5), F(6), C(0))
            elif name == 'R':
                t = t.rectangle([I(k) for k in range(1, 5)], C(0))
            else:
                raise ValueError(tok)
            st[-1] = t
    assert len(st) == 1, (ops, len(st))
    return st[0]
