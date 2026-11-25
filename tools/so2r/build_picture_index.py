#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 jadesigns.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Map where the Star Ocean: The Second Story R companion's pictures and font sit inside YOUR romfs, so the
module can read them straight from the game files on the device. Writes pictures.idx: names, sizes and
positions only (which bundle, where a texture's data starts, each picture's rectangle, where the font's
glyph tables sit) -- no pixels, nothing of the game's art.

    python3 tools/so2r/build_picture_index.py <romfs> [out folder] [update romfs]
    (default out folder: ./pictures; a bundle the update replaces is read from the update's romfs,
    since that is the file the game opens)

It indexes the UI sprite atlases, the area minimaps and world maps, the character illustrations and dot
portraits, the assault members' pixel characters, the bonus gauge pictures, the controller button icons,
the title screen pictures, the game font and its number font.

Then it checks the whole method: every picture is decoded exactly the way the module does it (bundle
header, LZ4 blocks, Switch block-linear layout, BC1 / BC3 / Alpha8 / ASTC, crop, flip) and compared with
UnityPy's own decode of the same texture, and writes check_*.png (the module-way decode of a few pictures
and of the font's letters) to look at.

Needs UnityPy (with texture2ddecoder), lz4 and Pillow:  pip install UnityPy Pillow lz4
"""
import bisect
import collections
import os
import re
import struct
import sys

try:
    import UnityPy
    import lz4.block
    import texture2ddecoder
    from PIL import Image
except ImportError as e:
    sys.exit(f"missing: {e}  (pip install UnityPy Pillow lz4)")

if len(sys.argv) < 2:
    sys.exit(__doc__)
ROMFS = os.path.abspath(sys.argv[1])
OUT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "pictures")
os.makedirs(OUT, exist_ok=True)
UPD = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 else None
if UPD and not os.path.isdir(os.path.join(UPD, "Data", "StreamingAssets")):
    sys.exit(f"update romfs not found: {UPD} has no Data/StreamingAssets (dump the update's romfs first)")
SA_REL = "Data/StreamingAssets"
BUNDLES = ["uiatlas", "so2r_font_ja_en"]          # every sprite of uiatlas; the text font
FONT_NAME = "so2r_font_ja_en"
# Unity format -> (name, block px, bytes per block)
FORMATS = {12: ("BC3", 4, 16), 1: ("Alpha8", 1, 1), 4: ("RGBA32", 1, 4), 10: ("BC1", 4, 8), 25: ("BC7", 4, 16), 48: ("ASTC4x4", 4, 16)}
MAP_BUNDLES = ["map_expel", "map_nede"]
stats = collections.Counter()


# ---------------------------------------------------------------- UnityFS, the way the module reads it
def lz4_block(src, usize):
    dst = bytearray(); i = 0; n = len(src)
    while i < n:
        t = src[i]; i += 1
        lit = t >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        dst += src[i:i + lit]; i += lit
        if i >= n: break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = t & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        s = len(dst) - off
        if off <= 0 or s < 0: raise ValueError("bad lz4 offset")
        for k in range(ml): dst.append(dst[s + k])
    if len(dst) != usize: raise ValueError(f"lz4 size {len(dst)} != {usize}")
    return bytes(dst)


class Bundle:
    def __init__(self, path):
        self.path = path
        self.size = os.path.getsize(path)
        self.f = open(path, "rb")
        h = self.f.read(min(self.size, 512))
        if not h.startswith(b"UnityFS\0"): raise ValueError("not UnityFS")
        i = 8
        self.version = struct.unpack(">I", h[i:i + 4])[0]; i += 4
        j = h.index(b"\0", i); i = j + 1
        j = h.index(b"\0", i); self.unity = h[i:j].decode(); i = j + 1
        _total, csize, usize, flags = struct.unpack(">qIII", h[i:i + 20]); i += 20
        self.flags = flags
        if flags & 0x1400: raise ValueError("encrypted bundle")
        if self.version >= 7: i = (i + 15) & ~15
        start = i
        self.f.seek(self.size - csize if flags & 0x80 else start)
        raw = self.f.read(csize)
        comp = flags & 0x3F
        info = raw if comp == 0 else lz4_block(raw, usize) if comp in (2, 3) else None
        if info is None: raise ValueError(f"block info compression {comp}")
        p = 16
        nb = struct.unpack(">i", info[p:p + 4])[0]; p += 4
        self.blocks = []
        for _ in range(nb):
            us, cs, bf = struct.unpack(">IIH", info[p:p + 10]); p += 10
            self.blocks.append((us, cs, bf))
        nn = struct.unpack(">i", info[p:p + 4])[0]; p += 4
        self.nodes = {}
        for _ in range(nn):
            off, sz, _nf = struct.unpack(">qqI", info[p:p + 20]); p += 20
            j = info.index(b"\0", p); name = info[p:j].decode(); p = j + 1
            self.nodes[name] = (off, sz)
        data = start if flags & 0x80 else start + csize
        if flags & 0x200: data = (data + 15) & ~15
        self.data = data
        self.ustart, self.fstart = [], []
        u, fp = 0, data
        for us, cs, bf in self.blocks:
            self.ustart.append(u); self.fstart.append(fp); u += us; fp += cs
        self.total = u
        self.cache = {}
        self.header = (self.version, flags, csize, usize, nb)

    def block(self, k):
        if k in self.cache: return self.cache[k]
        us, cs, bf = self.blocks[k]
        self.f.seek(self.fstart[k]); raw = self.f.read(cs)
        c = bf & 0x3F
        b = raw if c == 0 else lz4_block(raw, us) if c in (2, 3) else None
        if b is None: raise ValueError(f"block compression {c}")
        if len(self.cache) > 16: self.cache.clear()
        self.cache[k] = b
        return b

    def read(self, off, n):
        out = bytearray()
        while n > 0:
            k = bisect.bisect_right(self.ustart, off) - 1
            b = self.block(k)
            o = off - self.ustart[k]
            take = min(n, len(b) - o)
            if take <= 0: raise ValueError("read past the end")
            out += b[o:o + take]; off += take; n -= take
        return bytes(out)


# ---------------------------------------------------------------- Switch block-linear (the module's formula)
def bl_offset(xb, y, width_bytes, gobs):
    """Byte offset of byte column xb, row y in a block-linear surface (GOB 64 B x 8 rows,
    `gobs` GOBs per block vertically)."""
    gobs_x = (width_bytes + 63) // 64
    bh = 8 * gobs
    in_gob = ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((xb % 32) // 16) * 32 + (y % 2) * 16 + (xb % 16)
    return ((y // bh) * gobs_x + xb // 64) * 512 * gobs + ((y % bh) // 8) * 512 + in_gob


def module_way(b, t, x, y, w, h, rot):
    """A picture's RGBA, top row first, the way the module decodes it."""
    off, size, tw, th, fmt, gobs = t
    name, bpx, bpb = FORMATS[fmt]
    bxn = -(-tw // bpx)
    bx0, by0 = x // bpx, y // bpx
    bx1, by1 = -(-(x + w) // bpx), -(-(y + h) // bpx)
    rw, rh = (bx1 - bx0) * bpx, (by1 - by0) * bpx
    region = bytearray()
    for by in range(by0, by1):
        if not gobs:        # 0.9.78 plain rows (RGBA32 without a platform blob)
            region += b.read(off + (by * bxn + bx0) * bpb, (bx1 - bx0) * bpb)
            continue
        for bx in range(bx0, bx1):
            o = off + bl_offset(bx * bpb, by, bxn * bpb, gobs)
            region += b.read(o, bpb)
    if fmt == 4:
        img = Image.frombytes("RGBA", (rw, rh), bytes(region))
    elif fmt != 1:
        dec = {12: texture2ddecoder.decode_bc3, 10: texture2ddecoder.decode_bc1, 25: texture2ddecoder.decode_bc7,
               48: lambda d, w, h: texture2ddecoder.decode_astc(d, w, h, 4, 4)}[fmt]
        bgra = dec(bytes(region), rw, rh)
        img = Image.frombytes("RGBA", (rw, rh), bgra, "raw", "BGRA")
    else:
        img = Image.frombytes("L", (rw, rh), bytes(region)).convert("RGBA")
    # rows in the texture run bottom-up: region row 0 is y = by0 * bpx
    img = img.crop((x - bx0 * bpx, y - by0 * bpx, x - bx0 * bpx + w, y - by0 * bpx + h))
    T = Image.Transpose
    img = img.transpose(T.FLIP_TOP_BOTTOM)        # to top row first
    if rot == 1: img = img.transpose(T.FLIP_LEFT_RIGHT)
    elif rot == 2: img = img.transpose(T.FLIP_TOP_BOTTOM)
    elif rot == 3: img = img.transpose(T.ROTATE_180)
    elif rot == 4: img = img.transpose(T.ROTATE_90)
    return img


def same(a, b):
    """Compare what is seen: colour weighted by alpha; BC rounding allowed."""
    if a.size != b.size: return False, 999
    pa, pb = a.load(), b.load()
    worst = 0
    for yy in range(0, a.size[1], max(1, a.size[1] // 40)):
        for xx in range(0, a.size[0], max(1, a.size[0] // 40)):
            ra, rb = pa[xx, yy], pb[xx, yy]
            d = abs(ra[3] - rb[3])
            if ra[3] > 16 or rb[3] > 16:
                d = max(d, max(abs(ra[k] * ra[3] - rb[k] * rb[3]) // 255 for k in range(3)))
            worst = max(worst, d)
    return worst <= 12, worst


# ---------------------------------------------------------------- 1. bundles
lines = ["SO2P 1"]
bund = {}


def add_bundle(name, quiet=False):
    rel = f"{SA_REL}/{name}"
    path = os.path.join(ROMFS, rel)
    if UPD and os.path.isfile(os.path.join(UPD, rel)):
        path = os.path.join(UPD, rel); stats["bundles read from the update"] += 1
        print(f"   {name}: the update replaces it")
    b = Bundle(path)
    bid = len(bund)
    bund[name] = (bid, b)
    BUNDLES.append(name) if name not in BUNDLES else None
    lines.append(f"B {bid} {rel} {b.size} {b.total} {len(b.blocks)}")
    stats["bundle blocks max"] = max(stats["bundle blocks max"], len(b.blocks))
    stats["bundle block size max"] = max(stats["bundle block size max"], max(us for us, _, _ in b.blocks))
    if not quiet:
        print(f"bundle {name}: {b.size} bytes, unity {b.unity}, flags 0x{b.flags:x}, {len(b.blocks)} blocks, "
              f"largest {max(us for us, _, _ in b.blocks)}, compression {sorted({bf & 0x3F for *_, bf in b.blocks})}, "
              f"nodes {list(b.nodes)}")
    return bid, b


for name in list(BUNDLES):
    add_bundle(name)

textures = {}        # (bundle id, offset) -> tex id
tex_rows = []


# 0.9.46 a texture's fingerprint, so the module can find it in a bundle the game's update replaced (another size):
# numbers only -- the rolling hash of one 32-byte window of its data (one that occurs once in it, with the most
# different bytes, near the middle) and the FNV-1a 64 of all of it. Must match the module's RollHash / Fnv64.
ROLL_B, M64 = 1099511628211, (1 << 64) - 1
def fingerprint(data):
    n = len(data)
    if n < 64: raise ValueError("texture too small to fingerprint")
    p31 = pow(ROLL_B, 31, 1 << 64)
    h, win, seen = 0, [0] * (n - 31), {}
    for i, b in enumerate(data):
        if i >= 32: h = (h - data[i - 32] * p31) & M64
        h = (h * ROLL_B + b) & M64
        if i >= 31:
            win[i - 31] = h
            seen[h] = seen.get(h, 0) + 1
    best, mid = None, n // 2
    for off in range(0, n - 31, 16):
        if seen[win[off]] != 1: continue
        key = (len(set(data[off:off + 32])), -abs(off - mid))
        if best is None or key > best[0]: best = (key, off)
    if best is None: raise ValueError("no window occurs once")
    full = 14695981039346656037
    for b in data: full = ((full ^ b) * ROLL_B) & M64
    return best[1], win[best[1]], full


def tex_id(bname, tex, obj=None, sig=False):
    bid, b = bund[bname]
    fmt = int(tex.m_TextureFormat)
    if fmt not in FORMATS: raise ValueError(f"texture format {fmt}")
    blob = bytes(getattr(tex, "m_PlatformBlob", None) or b"")
    gobs = 1 << int.from_bytes(blob[8:12], "little") if len(blob) >= 12 else 0
    # 0.9.78 the pixel characters' RGBA32 sheets carry no platform blob: stored in plain rows (gobs 0)
    if gobs < 1 and fmt != 4: raise ValueError("no block-linear setting")
    sd = tex.m_StreamData
    if sd and sd.path:
        cab = sd.path.split("/")[-1]
        if cab not in b.nodes: raise ValueError(f"{cab} not in {bname}")
        off, size = b.nodes[cab][0] + sd.offset, sd.size
    else:
        # 0.9.41: pixels stored inline in the serialized file (uiscene's field_mode_background): where those bytes
        # sit -- the object's place in its node plus the data's place in the object (found by its own bytes)
        if obj is None: raise ValueError("texture stored inline")
        data = bytes(getattr(tex, "image_data", None) or getattr(tex, "m_ImageData", None) or b"")
        if not data: raise ValueError("inline texture without data")
        raw = bytes(obj.get_raw_data())
        at = raw.find(data)
        if at < 0 or raw.find(data, at + 1) >= 0: raise ValueError("inline data not found once")
        node = obj.assets_file.name.split("/")[-1]
        if node not in b.nodes: raise ValueError(f"{node} not in {bname}")
        off, size = b.nodes[node][0] + obj.byte_start + at, len(data)
        stats["inline textures"] += 1
    key = (bid, off)
    if key not in textures:
        extra = ""
        if sig:   # 0.9.46
            data = bytes(getattr(tex, "image_data", None) or getattr(tex, "m_ImageData", None) or b"")
            if not data and sd and sd.path:     # 0.9.78b data kept in the bundle's .resS: read it where it sits
                data = b.read(off, size)
            if len(data) != size: raise ValueError(f"fingerprint: data {len(data)} bytes, expected {size}")
            so, sh, fh = fingerprint(data)
            extra = f" {so} {sh} {fh}"
            stats["fingerprinted textures"] += 1
        textures[key] = len(tex_rows)            # 0.9.78b only once its fingerprint is made (no orphan rows)
        tex_rows.append((bid, off, size, tex.m_Width, tex.m_Height, fmt, gobs, tex.m_Name, tex))
        lines.append(f"T {textures[key]} {bid} {off} {size} {tex.m_Width} {tex.m_Height} {fmt} {gobs}{extra}")
    return textures[key]


def deref(pp):
    return pp.deref_parse_as_object() if hasattr(pp, "deref_parse_as_object") else pp.read()


# ---------------------------------------------------------------- 2. every uiatlas sprite
env = UnityPy.load(bund["uiatlas"][1].path)
sprites = []
borders = {}
for o in env.objects:
    if o.type.name != "Sprite": continue
    try:
        s = o.read()
        atlas = deref(s.m_SpriteAtlas) if s.m_SpriteAtlas and s.m_SpriteAtlas.path_id else None
        rd = next(v for k, v in atlas.m_RenderDataMap if k == s.m_RenderDataKey) if atlas else s.m_RD
        if rd.alphaTexture and rd.alphaTexture.path_id:
            stats["skipped: separate alpha texture"] += 1; continue
        tex = deref(rd.texture)
        t = tex_id("uiatlas", tex)
        tr = rd.textureRect
        raw = rd.settingsRaw
        packed, rot = raw & 1, (raw >> 2) & 0xF
        x0, y0 = round(tr.x), round(tr.y)
        w, h = round(tr.x + tr.width) - x0, round(tr.y + tr.height) - y0
        r = rot if packed else 0
        stats[f"rotation {r}"] += 1
        # the game's own 9-slice insets (Sprite.m_Border: left, bottom, right, top) -- so frames are
        # drawn with the game's fixed edges, never stretched
        bd = s.m_Border
        bl, bb, br, bt = (round(bd.x), round(bd.y), round(bd.z), round(bd.w)) if bd is not None else (0, 0, 0, 0)
        if (bl or bb or br or bt):
            stats["sprites with 9-slice borders"] += 1
            if (round(s.m_Rect.width), round(s.m_Rect.height)) != (w, h):
                stats["  ... of them trimmed (borders shifted)"] += 1
            borders[s.m_Name] = (bl, bb, br, bt)
        sprites.append((s.m_Name, t, x0, y0, w, h, r))
    except Exception as e:
        stats[f"sprite skipped: {type(e).__name__}: {e}"] += 1
n_ui = len(sprites)


pivots = {}


def plain_sprites(bname, want):
    """Sprites of one bundle whose name matches `want` (not packed into an atlas)."""
    found = 0
    for o in UnityPy.load(bund[bname][1].path).objects:
        if o.type.name != "Sprite": continue
        try:
            s = o.read()
            if not want.match(s.m_Name): continue
            atlas = deref(s.m_SpriteAtlas) if s.m_SpriteAtlas and s.m_SpriteAtlas.path_id else None
            rd = next(v for k, v in atlas.m_RenderDataMap if k == s.m_RenderDataKey) if atlas else s.m_RD
            if rd.alphaTexture and rd.alphaTexture.path_id:
                stats["map skipped: separate alpha texture"] += 1; continue
            tex = deref(rd.texture)
            stats[f"map texture format {int(tex.m_TextureFormat)}"] += 1
            t = tex_id(bname, tex)
            tr = rd.textureRect
            raw = rd.settingsRaw
            packed, rot = raw & 1, (raw >> 2) & 0xF
            x0, y0 = round(tr.x), round(tr.y)
            w, h = round(tr.x + tr.width) - x0, round(tr.y + tr.height) - y0
            if (round(s.m_Pivot.x * 100), round(s.m_Pivot.y * 100), round(s.m_PixelsToUnits)) != (50, 50, 100):
                stats["map sprite: pivot / ppu not centre / 100"] += 1
            if s.m_Name.startswith("charaillust_"):
                # 0.9.16: where the trimmed picture sits in the sprite's full rect, and the pivot the
                # game's Image places it by (Party Formation cards); positions only, no pixels
                tro = getattr(rd, "textureRectOffset", None)
                pivots[s.m_Name] = (round(s.m_Rect.width), round(s.m_Rect.height),
                                    round(tro.x) if tro is not None else 0, round(tro.y) if tro is not None else 0,
                                    round(s.m_Pivot.x * 1000), round(s.m_Pivot.y * 1000), round(s.m_PixelsToUnits))
            sprites.append((s.m_Name, t, x0, y0, w, h, rot if packed else 0))
            found += 1
        except Exception as e:
            stats[f"map sprite skipped: {type(e).__name__}: {str(e)[:60]}"] += 1
    return found


# the two world maps
for name in MAP_BUNDLES:
    if not os.path.isfile(os.path.join(ROMFS, SA_REL, name)):
        print(f"world map {name}: not found"); continue
    add_bundle(name)
    print(f"world map {name}: {plain_sprites(name, re.compile(r'map_(expel|nede)$'))} sprite(s)")
# every area minimap: '<area>_pack' bundles holding 'minimap_<area>NNN'
packs = sorted(f for f in os.listdir(os.path.join(ROMFS, SA_REL)) if f.endswith("_pack"))
n_packs = 0
for name in packs:
    area = name[:-5]
    try:
        bid, b = add_bundle(name, quiet=True)
    except Exception as e:
        stats[f"pack skipped: {type(e).__name__}"] += 1; continue
    k = plain_sprites(name, re.compile(r"minimap_" + re.escape(area) + r"\d+$", re.I))
    if not k and not any(r[0] == bid for r in tex_rows):   # nothing for the map: drop the bundle again
        lines.pop(lines.index(f"B {bid} {SA_REL}/{name} {b.size} {b.total} {len(b.blocks)}"))
        del bund[name]; BUNDLES.remove(name)
        continue
    n_packs += 1
print(f"area minimaps: {len(sprites) - n_ui} sprites in {n_packs} of {len(packs)} '_pack' bundles")
# 0.9.13: the character illustrations (charaillust_NNNN: one ASTC 4x4 sprite each, the camp menu's
# pictures) for the Party page's cards
n_ill = 0
for name in sorted(f for f in os.listdir(os.path.join(ROMFS, SA_REL)) if re.fullmatch(r"charaillust_\d{4}", f)):
    try:
        add_bundle(name, quiet=True)
        n_ill += plain_sprites(name, re.compile(r"charaillust_\d{4}$"))
    except Exception as e:
        stats[f"illustration skipped: {type(e).__name__}: {str(e)[:60]}"] += 1
print(f"character illustrations: {n_ill}")
# 0.9.41: plain textures of uiscene the companion draws whole: field_mode_background (the game's dotted blue
# panel behind its field mode label, r45), for the bottom navigation bar
def plain_textures(bname, want):
    found = 0
    for o in UnityPy.load(bund[bname][1].path).objects:
        if o.type.name != "Texture2D": continue
        try:
            tex = o.read()
            if not want.match(tex.m_Name): continue
            t = tex_id(bname, tex, o, sig=True)
            sprites.append((tex.m_Name, t, 0, 0, tex.m_Width, tex.m_Height, 0))
            found += 1
        except Exception as e:
            stats[f"uiscene texture skipped: {type(e).__name__}: {str(e)[:60]}"] += 1
    return found


try:
    add_bundle("uiscene", quiet=True)
    # 0.9.46: also the game's own bottom band (Guide Map screen): the blue glow and the dot tile
    print(f"uiscene textures: {plain_textures('uiscene', re.compile(r'(field_mode_background|system_background_01|field_minimap_background_tile|field_information_get_background_glow|battle_bonus_gauge_(outline|sphere_core|sphere_glow)|battle_guts_color|title_background|logo_title|logo_globe_core|logo_globe_debris_0[1-4])$'))}")   # 0.9.145 the title page (r80)
    # 0.9.67 the Guide Map side buttons' background (the game's item-get band), a texture or a sprite
    if not any(sp[0] == "field_information_get_background_glow" for sp in sprites):
        print(f"uiscene sprites: {plain_sprites('uiscene', re.compile(r'field_information_get_background_glow$'))}")
except Exception as e:
    stats[f"uiscene skipped: {type(e).__name__}: {str(e)[:60]}"] += 1


# 0.9.77 the assault members' pixel characters (r61): each cp_NNNN_40 bundle's clip assist_executable_loop is a
# streamed clip whose curves 2..5 are the material's Tiling (x, y) and Offset (x, y) -- the frame's place on the
# sheet, as a fraction of it, bottom-left origin -- and curve 6 picks the sheet (pptrCurveMapping). Its first
# frame (time 0) is the still the companion shows.
def streamed_frames(words):
    b = struct.pack(f"<{len(words)}I", *words)
    pos, frames = 0, []
    while pos + 8 <= len(b):
        t, n = struct.unpack_from("<fi", b, pos); pos += 8
        keys = {}
        for _ in range(max(n, 0)):
            if pos + 20 > len(b): break
            idx = struct.unpack_from("<i", b, pos)[0]
            keys[idx] = struct.unpack_from("<4f", b, pos + 4)[3]
            pos += 20
        frames.append((t, keys))
    return frames


n_dot = 0
for name in sorted(f for f in os.listdir(os.path.join(ROMFS, SA_REL)) if re.fullmatch(r"cp_\d{4}_40", f)):
    try:
        add_bundle(name, quiet=True)
        env = UnityPy.load(bund[name][1].path)
        texs = {o.path_id: o for o in env.objects if o.type.name == "Texture2D"}
        clip = next(o.read_typetree() for o in env.objects if o.type.name == "AnimationClip"
                    and o.read_typetree().get("m_Name") == "assist_executable_loop")
        cl = ((clip.get("m_MuscleClip") or {}).get("m_Clip") or {}).get("data") or {}
        st = {}
        for t, keys in streamed_frames((cl.get("m_StreamedClip") or {}).get("data", [])):
            st.update(keys)
            if t >= 0: break
        mapping = (clip.get("m_ClipBindingConstant") or {}).get("pptrCurveMapping", [])
        pid = mapping[int(round(st.get(6, 0)))].get("m_PathID")
        o = texs[pid]
        tex = o.read()
        W, H = tex.m_Width, tex.m_Height
        x0, y0 = round(st[4] * W), round(st[5] * H)
        w, h = round(st[2] * W), round(st[3] * H)
        if w <= 0 or h <= 0 or x0 + w > W or y0 + h > H: raise ValueError(f"frame {x0},{y0} {w}x{h} outside {W}x{H}")
        t = tex_id(name, tex, o, sig=True)
        sprites.append((f"dot_{name[:7]}", t, x0, y0, w, h, 0))
        print(f"   dot_{name[:7]}: {tex.m_Name} {W}x{H} frame {x0},{y0} {w}x{h}")
        n_dot += 1
    except Exception as e:
        stats[f"pixel character skipped ({name}): {type(e).__name__}: {str(e)[:60]}"] += 1
print(f"assault pixel characters: {n_dot}")
# 0.9.112 the game's controller button icons (r73): the TextMeshPro sprite asset 'text_icon_switch' (its own
# bundle): each icon's rect on its sheet (m_SpriteGlyphTable m_GlyphRect, bottom-left origin) under its name in
# m_SpriteCharacterTable (named after the PlayStation buttons: Circle = A, Cross = B, Triangle = X, Square = Y,
# L1 / R1 = L / R, L2 / R2 = ZL / ZR) -- indexed as button_<name>
n_btn = 0
try:
    add_bundle("text_icon_switch", quiet=True)
    env = UnityPy.load(bund["text_icon_switch"][1].path)
    objs = {o.path_id: o for o in env.objects}
    for o in env.objects:
        if o.type.name != "MonoBehaviour": continue
        try:
            tt = o.read_typetree()
        except Exception:
            continue
        if tt.get("m_Name") != "text_icon_switch" or "m_SpriteGlyphTable" not in tt: continue
        sheet = tt.get("spriteSheet") or {}
        so = objs.get(sheet.get("m_PathID"))
        if so is None: raise ValueError("sheet not in the bundle")
        tex = so.read()
        t = tex_id("text_icon_switch", tex, so, sig=True)
        glyphs = {g["m_Index"]: g["m_GlyphRect"] for g in tt["m_SpriteGlyphTable"]}
        for c in tt.get("m_SpriteCharacterTable", []):
            rc = glyphs.get(c.get("m_GlyphIndex"))
            if not rc or rc["m_Width"] <= 0: continue
            sprites.append((f"button_{c['m_Name']}", t, rc["m_X"], rc["m_Y"], rc["m_Width"], rc["m_Height"], 0))
            n_btn += 1
        # 0.9.119 the 1.0.1 update re-laid the sheet (same cells, other icons): every 65-px cell as
        # button_cell_<col>_<row>, which the page picks by what the player saw in them
        for cy in range(tex.m_Height // 65):
            for cx in range(tex.m_Width // 65):
                sprites.append((f"button_cell_{cx}_{cy}", t, cx * 65, cy * 65, 64, 64, 0))
        print(f"button icons: sheet {tex.m_Name} {tex.m_Width}x{tex.m_Height} fmt {int(tex.m_TextureFormat)}, {n_btn} icons")
        break
except Exception as e:
    print(f"BUTTON ICONS: {type(e).__name__}: {e}")
if not n_btn:
    print("BUTTON ICONS NOT FOUND")
for nm, t, x0, y0, w, h, r in sorted(sprites):
    b = borders.get(nm)
    lines.append(f"S {nm} {t} {x0} {y0} {w} {h} {r}" + (" %d %d %d %d" % b if b else ""))
for nm in sorted(pivots):
    lines.append("P %s %d %d %d %d %d %d %d" % ((nm,) + pivots[nm]))
print("character illustration pivots (full rect w h, trimmed offset x y, pivot x y per 1000, ppu):")
for nm in sorted(pivots):
    sp = next(x for x in sprites if x[0] == nm)
    print(f"   {nm}: trimmed {sp[4]}x{sp[5]}  rect/offset/pivot/ppu {pivots[nm]}")
print(f"{len(sprites)} sprites in {len(tex_rows)} textures, {len(bund)} bundles")
print(f"9-slice sprites (left bottom right top), {len(borders)}:")
for nm in sorted(borders):
    sp = next(x for x in sprites if x[0] == nm)
    print(f"   {nm}: {sp[4]}x{sp[5]}  borders {borders[nm]}")
print(f"bundles read from the update: {stats['bundles read from the update']}" if UPD else
      "NOTE: no update romfs given -- bundles the 1.0.1 update replaced will not load on the device")

# ---------------------------------------------------------------- 3. the font
def ser(d):
    out = b""
    for v in d.values():
        if isinstance(v, dict): out += ser(v)
        elif isinstance(v, bool): raise ValueError("bool field")
        elif isinstance(v, float): out += struct.pack("<f", v)
        elif isinstance(v, int): out += struct.pack("<i" if v < 0 else "<I", v)
        else: raise ValueError(f"field {type(v).__name__}")
    return out


def locate(raw, lst):
    e0 = ser(lst[0]); pat = struct.pack("<i", len(lst)) + e0
    at = raw.find(pat)
    if at < 0 or raw.find(pat, at + 1) >= 0: raise ValueError("table not found once")
    base = at + 4
    stride = raw.find(ser(lst[1]), base + len(e0)) - base
    for k, e in enumerate(lst):
        bb = ser(e)
        if raw[base + k * stride: base + k * stride + len(bb)] != bb: raise ValueError(f"element {k} misaligned")
    return base, stride


def index_font(bname, fname, tag):
    """A TMP font asset of bundle `bname`: where its object (glyph and character tables) sits in the bundle stream,
    and its atlas texture. Its line: tag, name (spaces as _), then as the 'F' line."""
    fbid, fb = bund[bname]
    fenv = UnityPy.load(fb.path)
    for o in fenv.objects:
        if o.type.name != "MonoBehaviour": continue
        try:
            if fname.encode() not in bytes(o.get_raw_data()[:96]): continue     # its m_Name near the start (fast)
            tt = o.read_typetree()
        except Exception:
            continue
        if tt.get("m_Name") != fname or "m_GlyphTable" not in tt: continue
        raw = o.get_raw_data()
        cab = o.assets_file.name.split("/")[-1]
        if cab not in fb.nodes: raise SystemExit(f"{cab} not in {bname}")
        noff = fb.nodes[cab][0]
        start = None
        for c in (o.byte_start, o.byte_start + getattr(getattr(o.assets_file, "header", None), "data_offset", 0)):
            if fb.read(noff + c, len(raw)) == raw:
                start = noff + c; break
        if start is None: raise SystemExit(f"font {fname}: object bytes not found in the bundle stream")
        gl, ch = tt["m_GlyphTable"], tt["m_CharacterTable"]
        g_at, g_st = locate(raw, gl)
        c_at, c_st = locate(raw, ch)
        atl = tt["m_AtlasTextures"][0]
        if atl.get("m_FileID", 0) != 0: raise SystemExit(f"font {fname}: atlas in another file")
        objs = o.assets_file.objects
        tobj = objs[atl["m_PathID"]] if isinstance(objs, dict) else next(x for x in objs if x.path_id == atl["m_PathID"])
        tex = tobj.read()
        t = tex_id(bname, tex, tobj, sig=(bname != FONT_NAME))
        fi = tt["m_FaceInfo"]
        lines.append(f"{tag} {fname.replace(' ', '_')} {fbid} {start} {g_at} {len(gl)} {g_st} {c_at} {len(ch)} {c_st} {t} "
                     f"{fi['m_PointSize']} {round(fi['m_AscentLine'] * 100)} {round(fi['m_LineHeight'] * 100)} "
                     f"{round(-fi['m_DescentLine'] * 100)} {tt['m_AtlasPadding']}"
                     + ("" if tag == "F" else " %d %d %d" % fingerprint(bytes(raw))))   # 0.9.101 found again in a moved bundle
        print(f"font {fname}: object at {start} ({len(raw)} bytes), glyphs {len(gl)} @+{g_at} stride {g_st}, "
              f"characters {len(ch)} @+{c_at} stride {c_st}, atlas tex {t} ({tex.m_Width}x{tex.m_Height} fmt "
              f"{int(tex.m_TextureFormat)}), point {fi['m_PointSize']}, ascent {fi['m_AscentLine']}, "
              f"line {fi['m_LineHeight']}, padding {tt['m_AtlasPadding']}")
        return gl, ch, t, fb
    return None


font_ok = False
r = index_font(FONT_NAME, FONT_NAME, "F")
if r:
    font_ok = True
    font_tables = r[:3]
    fb = r[3]
# 0.9.101 the number font of the bonus gauge (uiscene)
num_tables = None
try:
    num_tables = index_font("uiscene", "Aldrich-Regular SDF", "N")
except SystemExit as e:
    print(f"NUMBER FONT: {e}")
except Exception as e:
    print(f"NUMBER FONT: {type(e).__name__}: {e}")
if not num_tables:
    print("NUMBER FONT NOT FOUND")
if not font_ok:
    print("FONT NOT FOUND")

idx_path = os.path.join(OUT, "pictures.idx")
with open(idx_path, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print(f"wrote {idx_path} ({os.path.getsize(idx_path)} bytes, {len(lines)} lines)")

# ---------------------------------------------------------------- 4. check: module way vs UnityPy
print("\n== check")
full = {}
size_bad = 0
for tid, (bid, off, size, tw, th, fmt, gobs, tname, tex) in enumerate(tex_rows):
    bname = BUNDLES[bid]
    b = bund[bname][1]
    name, bpx, bpb = FORMATS[fmt]
    rows = -(-th // bpx)
    exp = (-(-tw // bpx) * bpb + 63) // 64 * 64 * (-(-rows // (8 * gobs)) * 8 * gobs) if gobs else tw * th * bpb
    if exp != size: size_bad += 1
    if tid < 12 or exp != size:
        print(f"   tex {tid} {tname}: {tw}x{th} {name} gobs {gobs}, data {size} bytes, block-linear size {exp} "
              f"{'OK' if exp == size else 'DIFFERENT'}")
print(f"   textures: {len(tex_rows) - size_bad}/{len(tex_rows)} have the block-linear size the module expects")


def unitypy_image(t):
    if t not in full:
        if len(full) > 6: full.clear()
        full[t] = tex_rows[t][8].image.convert("RGBA")       # UnityPy: top row first
    return full[t]


bad, n = [], 0
ui = [sp for sp in sprites if tex_rows[sp[1]][0] < 2]
maps = [sp for sp in sprites if tex_rows[sp[1]][0] >= 2]
# every uiatlas picture; a spread of the map pictures (the pure-Python check is slow on big maps)
pick = sorted(maps, key=lambda sp: sp[0])
pick = ([sp for sp in pick if re.match(r"dot_cp_\d{4}$|map_(expel|nede)$|charaillust_000[12]$|field_mode_background$|system_background_01$|field_minimap_background_tile$|field_information_get_background_glow$|battle_bonus_gauge_(outline|sphere_core|sphere_glow)$|title_background$|logo_title$|logo_globe_core$|logo_globe_debris_01$", sp[0])]
        + [sp for sp in pick if not sp[0].startswith("charaillust_")][::max(1, len(pick) // 10)][:10])
order = sorted(ui, key=lambda s: (not re.match(r"(icon_dot|charaillust_select|gauge_|camp_icon_money|camp_icon_label)", s[0]), s[0])) + pick
for nm, t, x0, y0, w, h, r in order:
    bid, off, size, tw, th, fmt, gobs, tname, tex = tex_rows[t]
    mine = module_way(bund[BUNDLES[bid]][1], (off, size, tw, th, fmt, gobs), x0, y0, w, h, r)
    ref = unitypy_image(t).crop((x0, th - y0 - h, x0 + w, th - y0))
    T = Image.Transpose
    if r == 1: ref = ref.transpose(T.FLIP_LEFT_RIGHT)
    elif r == 2: ref = ref.transpose(T.FLIP_TOP_BOTTOM)
    elif r == 3: ref = ref.transpose(T.ROTATE_180)
    elif r == 4: ref = ref.transpose(T.ROTATE_90)
    ok, worst = same(mine, ref)
    n += 1
    if not ok: bad.append((nm, worst))
    if re.match(r"(icon_dot_000[1-3]|charaillust_select_list_001|gauge_color_hp_01|camp_icon_money|camp_icon_label_leader)$", nm):
        mine.save(os.path.join(OUT, f"check_{nm}.png"))
    if nm.startswith("button_"):           # 0.9.112 every button icon, 2x
        mine.resize((w * 2, h * 2), Image.NEAREST).save(os.path.join(OUT, f"check_{nm}.png"))
    if nm.startswith("dot_cp_"):           # 0.9.78 every pixel character, 4x, to check the cut
        mine.resize((w * 4, h * 4), Image.NEAREST).save(os.path.join(OUT, f"check_{nm}.png"))
    if nm in borders:                      # the game's frames, to choose the Map page's frame from
        mine.save(os.path.join(OUT, f"check_9s_{nm}.png"))
    if (nm, t) in [(sp[0], sp[1]) for sp in pick[:4]] or nm.startswith("charaillust_") or nm in ("title_background", "logo_title", "logo_globe_core", "logo_globe_debris_01", "field_mode_background", "system_background_01", "field_minimap_background_tile", "field_information_get_background_glow",
                                                                                       "battle_bonus_gauge_outline", "battle_bonus_gauge_sphere_core", "battle_bonus_gauge_sphere_glow", "battle_guts_color"):
        mine.thumbnail((800, 800)); mine.save(os.path.join(OUT, f"check_{nm}.png"))
print(f"   sprites: {n - len(bad)}/{n} decode the same as UnityPy" + (f"; different: {bad[:20]}" if bad else ""))

if font_ok:
    gl, ch, t = font_tables
    bid, off, size, tw, th, fmt, gobs, tname, tex = tex_rows[t]
    full_font = tex.image.convert("RGBA")
    by_index = {g["m_Index"]: g for g in gl}
    cps = {c["m_Unicode"]: by_index.get(c["m_GlyphIndex"]) for c in ch}
    missing = [hex(c) for c in range(0x20, 0x7F) if c not in cps]
    print(f"   font: ASCII missing {missing or 'none'}")
    sheet = Image.new("RGBA", (40 * 16, 56 * 6), (20, 30, 50, 255))
    worst_all = 0
    for k, c in enumerate(range(0x21, 0x7F)):
        g = cps.get(c)
        if not g: continue
        rc = g["m_GlyphRect"]
        x, y, w, h = rc["m_X"], rc["m_Y"], rc["m_Width"], rc["m_Height"]
        if w <= 0 or h <= 0: continue
        mine = module_way(fb, (off, size, tw, th, fmt, gobs), x, y, w, h, 0)
        ref = full_font.crop((x, th - y - h, x + w, th - y))
        a, bb = mine.split()[0], ref.split()[3] if ref.mode == "RGBA" and ref.getextrema()[3][0] < 255 else ref.convert("L")
        worst = max(abs(p - q) for p, q in zip(a.getdata(), bb.getdata()))
        worst_all = max(worst_all, worst)
        glyph = Image.new("RGBA", (w, h), (255, 255, 255, 0))
        glyph.putalpha(a.point(lambda v: max(0, min(255, int((v - 127.5) * 255 / 42.5 + 127.5)))))
        sheet.alpha_composite(glyph, ((k % 16) * 40 + 2, (k // 16) * 56 + 2))
    sheet.save(os.path.join(OUT, "check_font.png"))
    print(f"   font atlas: module-way glyph pixels vs UnityPy, largest difference {worst_all} (0 = identical)")
if num_tables:      # 0.9.101 the number font's digits, decoded the module's way
    gl, ch, t, nb = num_tables
    bid, off, size, tw, th, fmt, gobs, tname, tex = tex_rows[t]
    by_index = {g["m_Index"]: g for g in gl}
    cps = {c["m_Unicode"]: by_index.get(c["m_GlyphIndex"]) for c in ch}
    need = "0123456789/"
    print(f"   number font: missing {[c for c in need if ord(c) not in cps] or 'none'}; format {FORMATS[fmt][0]} gobs {gobs}")
    sheet = Image.new("RGBA", (80 * len(need), 110), (20, 30, 50, 255))
    for k, c in enumerate(need):
        g = cps.get(ord(c))
        if not g: continue
        rc = g["m_GlyphRect"]
        x, y, w, h = rc["m_X"], rc["m_Y"], rc["m_Width"], rc["m_Height"]
        if w <= 0 or h <= 0: continue
        try:
            mine = module_way(nb, (off, size, tw, th, fmt, gobs), x, y, w, h, 0)
        except Exception as e:
            print(f"   number font glyph {c}: {type(e).__name__}: {e}"); continue
        a = mine.split()[3] if fmt in (4, 12, 25, 48, 10) else mine.split()[0]
        glyph = Image.new("RGBA", (w, h), (255, 255, 255, 0))
        glyph.putalpha(a.point(lambda v: max(0, min(255, int((v - 127.5) * 255 / 42.5 + 127.5)))))
        sheet.alpha_composite(glyph.crop((0, 0, min(w, 78), min(h, 108))), (k * 80 + 1, 1))
        print(f"   number font '{c}': rect {x},{y} {w}x{h}, metrics {g['m_Metrics']}")
    sheet.save(os.path.join(OUT, "check_number_font.png"))
for k, v in sorted(stats.items()):
    print(f"   {v:5d}  {k}")
print(f"\nsend back: this report, {idx_path}, and the check_*.png files in {OUT}")
