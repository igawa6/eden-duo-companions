#!/usr/bin/env python3
"""Map where each item / mission picture sits inside YOUR romfs, so the companion can read the
pictures straight from the game files on the device (no export, no add_icons step).

    python3 -I build_picture_index.py <romfs folder> [out folder]

Writes <out>/pictures.idx: names, sizes and positions only (which bundle, where the texture's data
starts, the picture's rectangle) -- no pixels, nothing of the game's art. Then checks the whole
method: for every picture it decodes the picture exactly the way the companion module will (reads
the bundle's raw bytes, LZ4 blocks, Switch block-linear layout, ASTC, crop, flip) and compares the
result with what UnityPy produces. Prints a report; send that and pictures.idx back.

Also finds the game's text font (a TextMeshPro font asset): where its glyph tables and its atlas
sit (an "F" line: positions only), so the companion can draw its text with the game's lettering.
Writes font_preview.png (every font found, drawn the way the companion will) -- send it back too.

Default out folder: ./dtd-pictures
Merged romfs: DTD_BASE=<base romfs> keeps the base pictures first (see step 2).
Extra names: DTD_EXTRA="<pattern>" before the command adds pictures whose names match it.
Another font: DTD_FONT="<pattern>" picks the font whose name matches (names are in the report).
"""
import os, re, struct, sys, time, collections, math

try:
    import UnityPy
    import lz4.block
    from UnityPy.export import Texture2DConverter
    from UnityPy.enums import BuildTarget
except ImportError as e:
    sys.exit(f"missing: {e}  (pip install UnityPy)")

if len(sys.argv) < 2:
    sys.exit(__doc__)
root = os.path.abspath(sys.argv[1])
out = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "dtd-pictures")
os.makedirs(out, exist_ok=True)
# the same pictures the export takes: item thumbnails and the mission tracker's character faces
WANT = re.compile(r"(_Thumbnail(_32)?$|_Profile(_\d+)?$|Portrait|_Face$|^Mission_[A-Za-z]+[A-Za-z0-9_]*$"
                  r"|^Sushi_|^Menu_|ingredient"   # + the dishes (RecipeEntity.Icon: Sushi_*, other dishes Menu_*) and ingredients
                  # + what the companion's pages use: weather / watch icons, the game's UI frames, the coin
                  r"|^Icon_Weather_|^UI_Watch_Icon_|^UI_EdgeFrame_Stroke$|^UI_Inven_TitleFrame$|^UI_Sushi_Pop_Panel$"
                  r"|^UI_Btn_Orange$|^UI_Btn_Inactive$|^UI_Sushi_Recipe_Menu_box$|^UI_Sushi_item_Bg$|^Coin32$"
                  r"|^UI_EdgeFrame$|^UI_btn_pink$|^UI_Sushi_Amount_Box$|^UI_Sushi_Menu_PopAmountBg$"
                  r"|^UI_SushiBar_Menu_Pattern01$|^UI_SushiBar_Pattern01$|arrow"
                  # + the pink OK button (and its dark twin), the menu's pink brush banner, the amount box's outline
                  r"|^UI_Sushi_Btn_Pink64$|^UI_Sushi_Btn_Black64$|^UI_Sushi_MenuTitleBg$|^UI_SushiBar_Storage_PopFocusLine$"
                  # + the game's brown (Close) and grey buttons, and controller-button glyphs ((A) OK, (B) Close):
                  # every small picture named like a key / button goes in, and glyphs.png shows them
                  r"|^StarIcon_Big$|^UI_O2_Gauge_re$|^UI_O2_Stroke_re$|^UI_O2_DAlarm_re$|^UI_O2_Frame_re$"
                  r"|^UI_Sushi_SpecialMission_GaugeOut$|^UI_Sushi_FarmGauge$"
                  r"|^AppIcon_iDiver$|^iDiver_Logo$|^iDiver_Icon_Tank$|^O2Box_(Body|Head)$|^Item_O2Capsule$"   # 0.37.27: DIVE icon, map O2 box
                  r"|Escape_?Pod|EscapeBell|^Pod_|_Pod$|_Pod_"   # escape pod candidates (map POD mark)
                  r"|radio|walkie|transceiver|beacon"   # the escape pod call radio (map POD mark)
                  r"|^DaveTheDiver_TitleLogo$|^DR_Illust$"   # the game's title logo (loading screen on the bottom screen)
                  r"|^UI_btn_brown$|^UI_btn_gray$|(^|_)(key|keys|btn|button|pad|input|gamepad|joycon|npad))", re.I)
GLYPHS = re.compile(r"(^|_)(key|keys|btn|button|pad|input|gamepad|joycon|npad)", re.I)
# + any other names you choose (e.g. the companion's tab icons): DTD_EXTRA="mask|goggle" python ...
_extra = os.environ.get("DTD_EXTRA", "").strip()
if _extra:
    WANT = re.compile(WANT.pattern + "|" + _extra, re.I)
    print(f"also indexing names matching: {_extra}", flush=True)

# ---------------------------------------------------------------- UnityFS, the way the module reads it
def lz4_block(src, usize):
    """Plain LZ4 block decoder (what the module carries; checked against lz4.block below)."""
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

def unity_version_new_flags(v):
    m = re.match(r"(\d+)\.(\d+)\.(\d+)", v)
    if not m: return True
    a = tuple(int(x) for x in m.groups())
    return not (a < (2020,) or (a[0] == 2020 and a < (2020, 3, 34)) or (a[0] == 2021 and a < (2021, 3, 2))
                or (a[0] == 2022 and a < (2022, 1, 1)))

class Bundle:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.size = os.path.getsize(path)
        h = self.f.read(min(self.size, 512))
        if not h.startswith(b"UnityFS\0"): raise ValueError("not UnityFS")
        i = 8
        self.version = struct.unpack(">I", h[i:i + 4])[0]; i += 4
        j = h.index(b"\0", i); self.player = h[i:j].decode(); i = j + 1
        j = h.index(b"\0", i); self.unity = h[i:j].decode(); i = j + 1
        _total, csize, usize, flags = struct.unpack(">qIII", h[i:i + 20]); i += 20
        self.flags = flags
        new = unity_version_new_flags(self.unity)
        if (flags & (0x1400 if new else 0x200)): raise ValueError("encrypted bundle")
        if self.version >= 7: i = (i + 15) & ~15
        start = i
        if flags & 0x80:
            info_at = self.size - csize
        else:
            info_at = start
        self.f.seek(info_at); raw = self.f.read(csize)
        comp = flags & 0x3F
        self.info_comp = comp
        if comp == 0: info = raw
        elif comp in (2, 3): info = lz4_block(raw, usize)
        else: raise ValueError(f"blocks info compression {comp}")
        p = 16
        nb = struct.unpack(">i", info[p:p + 4])[0]; p += 4
        self.blocks = []
        for _ in range(nb):
            us, cs, bf = struct.unpack(">IIH", info[p:p + 10]); p += 10
            if self.version >= 9: p += 8
            self.blocks.append((us, cs, bf))
        nn = struct.unpack(">i", info[p:p + 4])[0]; p += 4
        self.nodes = []
        for _ in range(nn):
            off, sz, nf = struct.unpack(">qqI", info[p:p + 20]); p += 20
            j = info.index(b"\0", p); name = info[p:j].decode(); p = j + 1
            self.nodes.append((name, off, sz))
        data = start if flags & 0x80 else start + csize
        if new and flags & 0x200: data = (data + 15) & ~15
        self.data = data
        # where each block starts, uncompressed and in the file
        self.ustart, self.fstart = [], []
        u, fpos = 0, data
        for us, cs, bf in self.blocks:
            self.ustart.append(u); self.fstart.append(fpos); u += us; fpos += cs
        self.cache = {}

    def block(self, k, own_lz4=False):
        if (k, own_lz4) in self.cache: return self.cache[(k, own_lz4)]
        us, cs, bf = self.blocks[k]
        self.f.seek(self.fstart[k]); raw = self.f.read(cs)
        c = bf & 0x3F
        if c == 0: b = raw
        elif c in (2, 3): b = lz4_block(raw, us) if own_lz4 else lz4.block.decompress(raw, uncompressed_size=us)
        else: raise ValueError(f"block compression {c}")
        if len(self.cache) > 64: self.cache.clear()
        self.cache[(k, own_lz4)] = b
        return b

    def read(self, off, n):
        import bisect
        outb = bytearray()
        while n > 0:
            k = bisect.bisect_right(self.ustart, off) - 1
            b = self.block(k)
            o = off - self.ustart[k]
            take = min(n, len(b) - o)
            if take <= 0: raise ValueError("read past the end of the bundle")   # 2026-10-19: was an endless loop
            outb += b[o:o + take]; off += take; n -= take
        return bytes(outb)

# ---------------------------------------------------------------- 1. every bundle's files
t0 = time.time()
paths = sorted(os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs if f.endswith(".bundle"))
print(f"{len(paths)} bundles", flush=True)
node_at = {}          # node name -> (bundle path, node offset in the uncompressed stream, size)
bundles = {}
stats = collections.Counter()
for p in paths:
    try:
        b = Bundle(p)
    except Exception as e:
        stats[f"bundle not readable: {e}"] += 1; continue
    bundles[p] = b
    stats[f"header v{b.version} unity {b.unity} flags 0x{b.flags:x}"] += 1
    stats[f"block info compression {b.info_comp}"] += 1
    for us, cs, bf in b.blocks:
        stats[f"data block compression {bf & 0x3F}"] += 1
    for name, off, sz in b.nodes:
        node_at[name] = (p, off, sz)
big = max((us for b in bundles.values() for us, _, _ in b.blocks), default=0)
print(f"parsed {len(bundles)} bundle headers in {time.time() - t0:.0f} s; largest block {big} bytes", flush=True)

# ---------------------------------------------------------------- 2. the wanted sprites and their textures
with_sprites, with_atlas, font_bundles = [], [], []
for i, p in enumerate(paths):
    try:
        env = UnityPy.load(p)
    except Exception:
        continue
    kinds = {o.type.name for o in env.objects}
    if "SpriteAtlas" in kinds: with_atlas.append(p)
    if "MonoBehaviour" in kinds and "Texture2D" in kinds:     # a TextMeshPro font: its asset + "<name> Atlas"
        for o in env.objects:
            try:
                if o.type.name == "Texture2D" and "atlas" in (o.peek_name() or "").lower():
                    font_bundles.append(p); break
            except Exception:
                pass
    if "Sprite" in kinds:
        for o in env.objects:
            if o.type.name == "Sprite":
                try:
                    if WANT.search(o.peek_name() or ""): with_sprites.append(p); break
                except Exception:
                    with_sprites.append(p); break
    if i % 200 == 0: print(f"  scanned {i}/{len(paths)}", flush=True)
# 2026-10-19: DTD_BASE=<base romfs> -- a merged romfs (base dump + bundles pulled from v1.0.6): pictures in
# bundles the base dump also has are taken from there first (unchanged index lines), the pulled bundles
# only add the names the base does not have (Clearfin_Lionfish_Thumbnail).
_base = os.environ.get("DTD_BASE")
_pick = sorted(set(with_sprites + with_atlas))
# 0.37.96: DTD_BUNDLE=<part of a bundle's file name>[,<another>...] -- only those bundles (a name like "Background" is in many bundles;
# this takes it from the one named, e.g. the Weapon Shop's 3af03b0b)
# 0.38.34: several parts, comma-separated (a sprite packed in a SpriteAtlas needs the atlas's bundle loaded too)
if os.environ.get("DTD_BUNDLE"):
    _parts = [q.strip().lower() for q in os.environ["DTD_BUNDLE"].split(",") if q.strip()]
    _pick = [p for p in _pick if any(q in os.path.basename(p).lower() for q in _parts)]
    print(f"DTD_BUNDLE: {len(_pick)} bundle(s)", flush=True)
# DTD_V106_ONLY=1 (with DTD_BASE): only the bundles pulled from v1.0.6 -- every player on v1.0.6 has them,
# whatever base release they own (the public index)
if _base and os.environ.get("DTD_V106_ONLY"):
    _pick = [p for p in _pick if not os.path.exists(os.path.join(_base, os.path.relpath(p, root)))]
    print(f"DTD_V106_ONLY: {len(_pick)} pulled bundles", flush=True)
if _base:
    _old = [p for p in _pick if os.path.exists(os.path.join(_base, os.path.relpath(p, root)))]
    _new = [p for p in _pick if p not in _old]
    print(f"DTD_BASE: {len(_old)} bundles as in the base dump, {len(_new)} pulled (only add pictures)", flush=True)
    _objs = list(UnityPy.load(*_old).objects) + (list(UnityPy.load(*_new).objects) if _new else [])
else:
    _objs = list(UnityPy.load(*_pick).objects)

def deref(pp):
    return pp.deref_parse_as_object() if hasattr(pp, "deref_parse_as_object") else pp.read()

BLOCK = {4: (4, 1)}                              # RGBA32: 16 bytes = 4 pixels in the swizzle
for f, s in zip(range(48, 54), (4, 5, 6, 8, 10, 12)): BLOCK[f] = (s, s)   # ASTC RGB
for f, s in zip(range(54, 60), (4, 5, 6, 8, 10, 12)): BLOCK[f] = (s, s)   # ASTC RGBA

textures, tex_key = [], {}   # [(bundle, data offset, size, w, h, fmt, gobs)]
_facts = {}
sprites = {}                 # lower name -> (name, tex index, x, y, w, h, rot, tight, sprite obj)
for o in _objs:
    if o.type.name != "Sprite": continue
    try:
        name = o.peek_name()
    except Exception:
        name = None
    if not name or not WANT.search(name) or name.lower() in sprites or any(c.isspace() for c in name): continue
    try:
        s = o.read()
        atlas = deref(s.m_SpriteAtlas) if s.m_SpriteAtlas and s.m_SpriteAtlas.path_id else None
        if atlas:
            rd = next(v for k, v in atlas.m_RenderDataMap if k == s.m_RenderDataKey)
        else:
            rd = s.m_RD
        if rd.alphaTexture and rd.alphaTexture.path_id:
            stats["skipped: separate alpha texture"] += 1; continue
        tex = deref(rd.texture)
        tr = rd.textureRect
        raw = rd.settingsRaw
        packed, tight, rot = raw & 1, (raw >> 1) & 1, (raw >> 2) & 0xF
        sd = getattr(tex, "m_StreamData", None)
        if not sd or not sd.path:
            stats["skipped: texture stored inline"] += 1; continue
        cab = sd.path.split("/")[-1]
        if cab not in node_at:
            stats["skipped: resource file not found in any bundle"] += 1; continue
        fmt = int(tex.m_TextureFormat)
        if fmt not in BLOCK:
            stats[f"skipped: texture format {fmt}"] += 1; continue
        _tex_facts = f"data {sd.size} bytes, blob {bytes(getattr(tex, 'm_PlatformBlob', None) or []).hex()}, mips {getattr(tex, 'm_MipCount', '?')}"
        plat = getattr(tex.object_reader, "platform", None)
        blob = getattr(tex, "m_PlatformBlob", None) or []
        gobs = 0
        if plat == BuildTarget.Switch and len(blob) >= 12:
            g = 1 << int.from_bytes(bytes(blob[8:12]), "little")
            gobs = g if g > 1 else 0
        bp, noff, nsz = node_at[cab]
        key = (bp, noff + sd.offset)
        if key not in tex_key:
            tex_key[key] = len(textures)
            textures.append((bp, noff + sd.offset, sd.size, tex.m_Width, tex.m_Height, fmt, gobs))
        x0, y0 = round(tr.x), round(tr.y)
        x1, y1 = round(tr.x + tr.width), round(tr.y + tr.height)
        stats[f"format {fmt}, gobs {gobs}"] += 1
        stats[f"rotation {rot if packed else 0}"] += 1
        if tight: stats["tight packed (mesh outline; the module crops the rectangle)"] += 1
        sprites[name.lower()] = (name, tex_key[key], x0, y0, x1 - x0, y1 - y0, rot if packed else 0, tight, s)
        _facts[name.lower()] = _tex_facts
    except Exception as e:
        stats[f"skipped: {type(e).__name__}"] += 1
print(f"{len(sprites)} pictures in {len(textures)} textures", flush=True)

# ---------------------------------------------------------------- 4. decode each picture the module's way
GOB_INV = {}
for v in range(32):
    GOB_INV[(((v >> 3) & 2) | ((v >> 1) & 1), ((v >> 1) & 6) | (v & 1))] = v

def block_index(bx, by, bxn, gobs):
    if not gobs:
        return by * bxn + bx
    gcx = bxn // 4
    j, gx = divmod(bx, 4)
    gyall, gy = divmod(by, 8)
    i, k = divmod(gyall, gobs)
    return ((i * gcx + j) * gobs + k) * 32 + GOB_INV[(gx, gy)]

from PIL import Image, ImageChops, ImageDraw
def module_way(t, x, y, w, h, rot, own_lz4=False):
    bp, off, size, tw, th, fmt, gobs = textures[t]
    bw, bh = BLOCK[fmt]
    if gobs:
        bxn = -(-tw // (bw * 4)) * 4          # padded to whole GOBs
    else:
        bxn = -(-tw // bw)
    bx0, by0 = x // bw, y // bh
    bx1, by1 = -(-(x + w) // bw), -(-(y + h) // bh)
    b = bundles[bp]
    region = bytearray()
    for by in range(by0, by1):
        for bx in range(bx0, bx1):
            o = off + block_index(bx, by, bxn, gobs) * 16
            if own_lz4:
                import bisect
                k = bisect.bisect_right(b.ustart, o) - 1
                blk = b.block(k, True); q = o - b.ustart[k]
                region += blk[q:q + 16]
            else:
                region += b.read(o, 16)
    rw, rh = (bx1 - bx0) * bw, (by1 - by0) * bh
    if fmt == 4:
        img = Image.frombytes("RGBA", (rw, rh), bytes(region))
    else:
        img = Texture2DConverter.parse_image_data(bytes(region), rw, rh, fmt, (2021, 3, 0, 0),
                                                  BuildTarget.UnknownPlatform, None, False).convert("RGBA")
    img = img.crop((x - bx0 * bw, y - by0 * bh, x - bx0 * bw + w, y - by0 * bh + h))
    T = Image.Transpose
    if rot == 1: img = img.transpose(T.FLIP_LEFT_RIGHT)
    elif rot == 2: img = img.transpose(T.FLIP_TOP_BOTTOM)
    elif rot == 3: img = img.transpose(T.ROTATE_180)
    elif rot == 4: img = img.transpose(T.ROTATE_270)
    return img.transpose(T.FLIP_TOP_BOTTOM)

def close_enough(a, b):
    # 2026-10-18: compare what is actually seen (colour weighted by alpha). Fully see-through pixels can hold any
    # colour, and ASTC decoders round a little differently; a wrong layout instead differs everywhere, by a lot.
    import array
    pa, pb = a.tobytes(), b.tobytes()
    n = len(pa) // 4; big = 0; tot = 0
    for i in range(0, len(pa), 4):
        aa, ab = pa[i + 3], pb[i + 3]
        d = abs(aa - ab)
        for c in range(3):
            d = max(d, abs(pa[i + c] * aa - pb[i + c] * ab) // 255)
        tot += d
        if d > 24: big += 1
    return n and big <= n // 200 and tot / n < 3

_tex_users = collections.Counter(v[1] for v in sprites.values())
def _retry_layout(ti, x, y, w, h, rot, ref, name):
    """2026-10-19: a texture of its own that comes out wrong as a plain (linear) layout: try the Switch block-linear
    layout with 1..16 GOBs (a block height of 1 GOB reads as 0 in the texture's settings). Kept only if exact."""
    bp, off, size, tw, th, fmt, gobs = textures[ti]
    if _tex_users[ti] != 1: return False
    for g in (1, 2, 4, 8, 16):
        if g == gobs: continue
        bw, bh = BLOCK[fmt]; bxn = -(-tw // (bw * 4)) * 4
        far = max(block_index(bx, by, bxn, g) for bx in (0, bxn - 1) for by in (0, -(-th // bh) - 1))
        if (far + 1) * 16 > size: continue                 # this layout would not fit in the texture's data
        textures[ti] = (bp, off, size, tw, th, fmt, g)
        try:
            m = module_way(ti, x, y, w, h, rot)
            if m.size == ref.size and (ImageChops.difference(m, ref).getbbox() is None or close_enough(m, ref)):
                stats[f"layout fixed: {g} GOB(s) instead of {gobs} ({name})"] += 1
                return True
        except Exception:
            pass
    textures[ti] = (bp, off, size, tw, th, fmt, gobs)
    return False

def _roughness(img, ox=0, oy=0):
    """How much colour jumps across the 4x4 block edges (a block-scrambled picture jumps a lot there). img is the
    picture as shown (flipped); ox, oy: where its rectangle starts in the texture."""
    img = img.convert("RGBA").transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    px = img.load(); W, H = img.size; tot = n = 0
    for yy in range(H):
        for xx in range(1, W):
            if (xx + ox) % 4: continue
            a, b = px[xx - 1, yy], px[xx, yy]
            tot += sum(abs(a[c] * a[3] - b[c] * b[3]) for c in range(3)) / 255; n += 1
    for xx in range(W):
        for yy in range(1, H):
            if (yy + oy) % 4: continue
            a, b = px[xx, yy - 1], px[xx, yy]
            tot += sum(abs(a[c] * a[3] - b[c] * b[3]) for c in range(3)) / 255; n += 1
    return tot / max(n, 1)

def _one_gob(ti, x, y, w, h, rot, name):
    """2026-10-19: Unity 6 stores some small textures block-linear with a block height of 1 GOB (setting 0). UnityPy
    reads those as plain rows and gets a block-scrambled picture, so it can't be the reference. The data size says
    which layout it is (1 GOB pads to whole 64-byte x 8-row GOBs); the picture is kept only if that reading is far
    smoother across block edges than the plain one. Both are saved side by side in differ/ to look at."""
    bp, off, size, tw, th, fmt, gobs = textures[ti]
    if gobs or _tex_users[ti] != 1 or fmt not in BLOCK or fmt == 4: return False
    bw, bh = BLOCK[fmt]; nbx, nby = -(-tw // bw), -(-th // bh)
    if size != (-(-nbx // 4) * 4) * (-(-nby // 8) * 8) * 16 or size == nbx * nby * 16: return False
    try:
        lin = module_way(ti, x, y, w, h, rot)
        textures[ti] = (bp, off, size, tw, th, fmt, 1)
        bl = module_way(ti, x, y, w, h, rot)
    except Exception:
        textures[ti] = (bp, off, size, tw, th, fmt, gobs); return False
    r_lin, r_bl = _roughness(lin, x, y), _roughness(bl, x, y)
    try:
        _dd = os.path.join(out, "differ"); os.makedirs(_dd, exist_ok=True)
        sb = Image.new("RGBA", (w * 2 + 8, h), (255, 0, 255, 255)); sb.paste(lin, (0, 0)); sb.paste(bl, (w + 8, 0))
        sb.save(os.path.join(_dd, name + ".1gob.png"))
    except Exception:
        pass
    if r_bl < 0.5 * r_lin:
        stats[f"layout fixed: 1 GOB (UnityPy reads it scrambled) roughness {r_lin:.1f} -> {r_bl:.1f} ({name})"] += 1
        return True
    stats[f"layout not fixed: 1 GOB roughness {r_lin:.1f} -> {r_bl:.1f} ({name})"] += 1
    textures[ti] = (bp, off, size, tw, th, fmt, gobs)
    return False

same, differ, failed, tight_differ = 0, [], [], 0
bad = set()   # pictures the module's way does not reproduce: left out of the index
own_checked = 0
t1 = time.time()
for n, key in enumerate(sorted(sprites)):
    name, ti, x, y, w, h, rot, tight, s = sprites[key]
    try:
        mine = module_way(ti, x, y, w, h, rot, own_lz4=own_checked < 5)
        own_checked += 1
        ref = s.image.convert("RGBA")
        if mine.size == ref.size and ImageChops.difference(mine, ref).getbbox() is None:
            same += 1
        elif mine.size == ref.size and close_enough(mine, ref):
            same += 1; stats["same once see-through pixels are ignored (ASTC rounding / colour under alpha 0)"] += 1
        elif tight:
            tight_differ += 1
        elif _one_gob(ti, x, y, w, h, rot, name):
            same += 1
        else:
            diff = ImageChops.difference(mine.convert("RGB"), ref.convert("RGB")) if mine.size == ref.size else None
            bad.add(key)
            try:                                       # 2026-10-19: side by side (mine | UnityPy) for the report
                _dd = os.path.join(out, "differ"); os.makedirs(_dd, exist_ok=True)
                _sb = Image.new("RGBA", (mine.width + ref.width + 8, max(mine.height, ref.height)), (255, 0, 255, 255))
                _sb.paste(mine, (0, 0)); _sb.paste(ref, (mine.width + 8, 0)); _sb.save(os.path.join(_dd, name + ".png"))
                stats[f"differ: info rot {rot} tight {tight} rect {x},{y} {w}x{h} tex {textures[ti][3]}x{textures[ti][4]} fmt {textures[ti][5]} gobs {textures[ti][6]}, {_facts.get(key)} ({name})"] += 1
            except Exception:
                pass
            differ.append(f"{name}: mine {mine.size} unitypy {ref.size}" + (f" max diff {max(e[1] for e in diff.getextrema())}" if diff else ""))
    except Exception as e:
        bad.add(key)
        failed.append(f"{name}: {type(e).__name__} {e}")
    if n % 100 == 0: print(f"  checked {n}/{len(sprites)} ({time.time() - t1:.0f} s)", flush=True)

# ---------------------------------------------------------------- 4a. a sheet of the key / button pictures
try:
    gl_pics = sorted((v for k, v in sprites.items() if k not in bad and GLYPHS.search(v[0]) and v[4] <= 160 and v[5] <= 160),
                     key=lambda v: v[0].lower())
    if gl_pics:
        from PIL import ImageDraw as _D
        CW, CH, COLS = 300, 120, 4
        sheet = Image.new("RGBA", (CW * COLS, CH * (-(-len(gl_pics) // COLS))), (40, 44, 52, 255))
        d = _D.Draw(sheet)
        for n, v in enumerate(gl_pics):
            x, y = (n % COLS) * CW, (n // COLS) * CH
            im = v[8].image.convert("RGBA"); im.thumbnail((90, 90))
            sheet.alpha_composite(im, (x + 6, y + 24))
            d.text((x + 6, y + 4), f"{v[0]} {v[4]}x{v[5]}", fill=(255, 230, 120, 255))
        sheet.convert("RGB").save(os.path.join(out, "glyphs.png"))
        print(f"wrote glyphs.png ({len(gl_pics)} key / button pictures)", flush=True)
except Exception as e:
    print(f"glyph sheet: {type(e).__name__}: {e}", flush=True)

# ---------------------------------------------------------------- 4b. the game's font (TextMeshPro)
# The companion module reads the font the same way as the pictures: the glyph tables out of the
# font asset's bytes and the glyph shapes out of its atlas texture, both straight from the bundle.
# Only where they sit goes in the index (the "F" line); the check below reads them the module's
# way and compares with UnityPy.
FONT_FIRST, FONT_LAST, FONT_CAP = 0x20, 0xFF, 34      # the module's kFontFirst / kFontLast / kFontCap
SDF_MODES = {4134, 8230, 16422, 32806, 4165, 4169}    # GlyphRenderMode: SDF, SDF8/16/32, SDFAA(_HINTED)
font_notes, fonts = [], []

def _ser(d):
    """A typetree element's numbers, serialized in field order (what the bundle holds)."""
    outb = b""
    for v in d.values():
        if isinstance(v, dict): outb += _ser(v)
        elif isinstance(v, bool): raise ValueError("bool field")
        elif isinstance(v, float): outb += struct.pack("<f", v)
        elif isinstance(v, int): outb += struct.pack("<i" if v < 0 else "<I", v & 0xFFFFFFFF if v >= 0 else v)
        else: raise ValueError(f"field type {type(v).__name__}")
    return outb

def _locate(raw, lst):
    """Where an array of these elements sits in the object's bytes: (first element, stride)."""
    if not lst: raise ValueError("empty table")
    e0 = _ser(lst[0]); pat = struct.pack("<i", len(lst)) + e0
    at = raw.find(pat)
    if at < 0: raise ValueError("table not found in the object's bytes")
    if raw.find(pat, at + 1) >= 0: raise ValueError("table found twice")
    base = at + 4
    stride = len(e0)
    if len(lst) > 1:
        at1 = raw.find(_ser(lst[1]), base + len(e0))
        if at1 < 0: raise ValueError("second element not found")
        stride = at1 - base
    for k, e in enumerate(lst):
        b = _ser(e)
        if raw[base + k * stride: base + k * stride + len(b)] != b: raise ValueError(f"element {k} does not line up")
    return base, stride

def _stream_start(o, raw):
    """Where an object's bytes start in its bundle's (uncompressed) data stream."""
    cab = o.assets_file.name
    if cab not in node_at: raise ValueError(f"file {cab} not in any bundle")
    bp, noff, _ = node_at[cab]
    b = bundles[bp]
    cands = [o.byte_start]
    try: cands.append(o.byte_start + o.assets_file.header.data_offset)
    except Exception: pass
    for c in cands:
        if b.read(noff + c, len(raw)) == raw: return bp, noff + c
    raise ValueError("object bytes not found in the bundle stream")

def _resolve(o, pptr):
    fid, pid = pptr.get("m_FileID", 0), pptr.get("m_PathID", 0)
    if fid != 0 or not pid: raise ValueError(f"atlas texture in another file ({fid})")
    return o.assets_file.objects[pid]

def analyse_font(o, tt, kind):
    name = re.sub(r"\s+", "_", tt.get("m_Name") or "font")
    raw = o.get_raw_data()
    bp, start = _stream_start(o, raw)
    chars = {}
    if kind == 1:
        gl, ch = tt["m_GlyphTable"], tt["m_CharacterTable"]
        if list(gl[0].keys())[:3] != ["m_Index", "m_Metrics", "m_GlyphRect"]: raise ValueError(f"glyph fields {list(gl[0].keys())}")
        if list(gl[0]["m_Metrics"].keys())[:5] != ["m_Width", "m_Height", "m_HorizontalBearingX", "m_HorizontalBearingY", "m_HorizontalAdvance"]:
            raise ValueError(f"metric fields {list(gl[0]['m_Metrics'].keys())}")
        if list(gl[0]["m_GlyphRect"].keys())[:4] != ["m_X", "m_Y", "m_Width", "m_Height"]: raise ValueError("rect fields")
        if list(ch[0].keys())[:3] != ["m_ElementType", "m_Unicode", "m_GlyphIndex"]: raise ValueError(f"character fields {list(ch[0].keys())}")
        g_at, g_st = _locate(raw, gl); c_at, c_st = _locate(raw, ch)
        by_index = {g["m_Index"]: g for g in gl}
        for c in ch:
            g = by_index.get(c["m_GlyphIndex"])
            if g and FONT_FIRST <= c["m_Unicode"] <= FONT_LAST:
                m, r = g["m_Metrics"], g["m_GlyphRect"]
                chars[c["m_Unicode"]] = (r["m_X"], r["m_Y"], r["m_Width"], r["m_Height"], m["m_HorizontalBearingX"],
                                         m["m_HorizontalBearingY"], m["m_HorizontalAdvance"])
        atlas = _resolve(o, tt["m_AtlasTextures"][0])
        mode, pad = int(tt.get("m_AtlasRenderMode", 0)), int(tt.get("m_AtlasPadding", 0))
        gn, cn = len(gl), len(ch)
    else:
        gl = tt["m_glyphInfoList"]
        if list(gl[0].keys())[:8] != ["id", "x", "y", "width", "height", "xOffset", "yOffset", "xAdvance"]: raise ValueError(f"glyph fields {list(gl[0].keys())}")
        g_at, g_st = _locate(raw, gl); c_at, c_st, cn = 0, 0, 0
        atlas = _resolve(o, tt["atlas"])
        cs = tt.get("fontAssetCreationSettings") or tt.get("m_CreationSettings") or {}
        mode = int(cs.get("renderMode", 0)); pad = int((tt.get("m_fontInfo") or {}).get("Padding", cs.get("padding", 0)))
        th0 = atlas.read().m_Height
        for g in gl:
            if FONT_FIRST <= g["id"] <= FONT_LAST:   # legacy rects run from the top: turn them over
                r = lambda v: int(v + 0.5)       # the module rounds half up
                chars[g["id"]] = (r(g["x"]), th0 - r(g["y"]) - r(g["height"]), r(g["width"]), r(g["height"]),
                                  g["xOffset"], g["yOffset"], g["xAdvance"])
        gn = len(gl)
    tex = atlas.read()
    fmt = int(tex.m_TextureFormat)
    if fmt not in (1, 4, 63): raise ValueError(f"atlas format {fmt} (the module reads Alpha8, R8, RGBA32)")
    blob = getattr(tex, "m_PlatformBlob", None) or []
    gobs = 0
    if getattr(tex.object_reader, "platform", None) == BuildTarget.Switch and len(blob) >= 12:
        gg = 1 << int.from_bytes(bytes(blob[8:12]), "little"); gobs = gg if gg > 1 else 0
    sd = getattr(tex, "m_StreamData", None)
    if sd and sd.path:
        cab = sd.path.split("/")[-1]
        if cab not in node_at: raise ValueError("atlas resource file not found")
        tbp, noff, _ = node_at[cab]; toff, tsize = noff + sd.offset, sd.size
    else:                                                     # stored inside the texture object
        traw = atlas.get_raw_data(); img = bytes(tex.image_data)
        tbp, tstart = _stream_start(atlas, traw)
        at = traw.rfind(struct.pack("<i", len(img)) + img[:64])
        if at < 0 or traw[at + 4: at + 4 + len(img)] != img: raise ValueError("atlas pixels not found in the texture object")
        toff, tsize = tstart + at + 4, len(img)
    if tbp != bp: raise ValueError("atlas texture in another bundle")
    ascii_n = sum(1 for c in range(0x21, 0x7F) if c in chars)
    return {"name": name, "kind": kind, "bundle": bp, "g_at": start + g_at, "gn": gn, "g_st": g_st,
            "c_at": start + c_at if kind == 1 else 0, "cn": cn, "c_st": c_st, "sdf": 1 if mode in SDF_MODES else 0,
            "mode": mode, "pad": pad, "chars": chars, "ascii": ascii_n, "tex": tex, "tex_obj": atlas,
            "t": (bp, toff, tsize, tex.m_Width, tex.m_Height, fmt, gobs)}

# 2026-10-19: v1.0.6's own bundles (Unity 6) are built without type trees, so UnityPy cannot read their font
# assets by itself. The base dump's font bundles (Unity 2020) do have them: borrow that layout (only to find the
# glyph tables here, on your PC; nothing of the base dump goes into pictures.idx) and accept a v1.0.6 asset only
# when it reads back exactly, byte for byte.
_borrowed, _base_fonts = [], []
def _borrow_font_layouts(paths):
    for bp_ in paths:
        try:
            benv = UnityPy.load(bp_)
        except Exception:
            continue
        for bo in benv.objects:
            if bo.type.name != "MonoBehaviour": continue
            try:
                btt = bo.read_typetree()
                if not (btt.get("m_GlyphTable") or btt.get("m_glyphInfoList")): continue   # "_F": empty fallback assets
                try:                                   # its tables, to find the same font in v1.0.6 by its bytes
                    bk = 1 if ("m_GlyphTable" in btt and "m_CharacterTable" in btt) else 2
                    bf = analyse_font(bo, btt, bk); braw = bo.get_raw_data(); bst = bf["g_at"] - _stream_start(bo, braw)[1]
                    bf["g_bytes"] = braw[bst - 4: bst + bf["gn"] * bf["g_st"]]
                    if bk == 1:
                        cst = bf["c_at"] - _stream_start(bo, braw)[1]
                        bf["c_bytes"] = braw[cst - 4: cst + bf["cn"] * bf["c_st"]]
                    bf["atlas_name"] = bf["tex"].m_Name; bf["src_name"] = btt.get("m_Name")
                    _base_fonts.append(bf)
                except Exception as e:
                    import traceback
                    where = " < ".join(f"{fr.name}:{fr.lineno}" for fr in reversed(traceback.extract_tb(e.__traceback__)[-3:]))
                    stats[f"font: base font not usable as a pattern ({type(e).__name__}: {e} at {where}) {btt.get('m_Name')}"] += 1
                node = getattr(getattr(bo, "serialized_type", None), "node", None) or bo._get_typetree_node()
            except Exception:
                continue
            if node is not None and all(node is not n for n in _borrowed):
                _borrowed.append(node)
                stats[f"font: layout borrowed from the base dump ({btt.get('m_Name')})"] += 1

def _read_font_tt(o):
    try:
        return o.read_typetree()
    except Exception as e:
        first = e
    for node in _borrowed:
        try:
            tt = o.read_typetree(node, check_read=True)
        except TypeError:
            try: tt = o.read_typetree(node)
            except Exception: continue
        except Exception:
            continue
        if isinstance(tt, dict) and ("m_GlyphTable" in tt or "m_glyphInfoList" in tt):
            stats["font scan: read with the borrowed base layout"] += 1
            return tt
    raise first

def _mb_name(raw):
    """A MonoBehaviour's name without a type tree: m_GameObject (12), m_Enabled (4), m_Script (12), then m_Name."""
    n = struct.unpack_from("<i", raw, 28)[0]
    if not 0 < n < 200: return None
    return raw[32:32 + n].decode("utf-8", "replace")

def _scan_glyphs(raw, st, tw, th):
    """TMP glyph records (index, 5 metric floats, 4 rect ints, scale, atlas index, ...) of stride st: (where the first
    one starts, list). Every record must have the right shape; 95% must also sit inside the atlas with the rect and
    metric sizes agreeing (2026-10-19: a font with a second atlas page or odd glyphs no longer fails outright)."""
    best = None
    for p in range(32, len(raw) - 8, 4):
        n = struct.unpack_from("<i", raw, p)[0]
        if not 20 <= n <= 20000 or p + 4 + n * st > len(raw): continue
        if best is not None and n <= len(best[1]): continue
        out = []; good = 0
        for k in range(n):
            r = p + 4 + k * st
            idx = struct.unpack_from("<I", raw, r)[0]
            mw, mh, bx, by, adv = struct.unpack_from("<5f", raw, r + 4)
            x, y, w, h = struct.unpack_from("<4i", raw, r + 24)
            ai = struct.unpack_from("<i", raw, r + 44)[0] if st >= 48 else 0
            if not (idx < 1 << 20 and 0 <= x < 16384 and 0 <= y < 16384 and 0 <= w < 1024 and 0 <= h < 1024 and 0 <= ai < 16
                    and all(math.isfinite(v) and abs(v) < 4096 for v in (mw, mh, bx, by, adv)) and mw >= 0 and mh >= 0):
                break
            if ai == 0 and x + w <= tw and y + h <= th and abs(w - mw) <= 2 and abs(h - mh) <= 2: good += 1
            out.append((idx, mw, mh, bx, by, adv, x, y, w, h, ai))
        if len(out) == n and good >= n * 0.95:
            best = (p + 4, out)
    return best

def _scan_chars(raw, st, glyph_ids):
    best = None
    for p in range(32, len(raw) - 8, 4):
        n = struct.unpack_from("<i", raw, p)[0]
        if not 20 <= n <= 20000 or p + 4 + n * st > len(raw): continue
        out = []
        for k in range(n):
            r = p + 4 + k * st
            et, uni, gi = struct.unpack_from("<3I", raw, r)
            if et > 2 or uni >= 0x110000 or gi >= 1 << 20: break
            out.append((uni, gi))
        # 2026-10-19: a few characters may point at glyphs not in the table (left out); most must be there
        if len(out) == n and sum(1 for _, g in out if g in glyph_ids) >= n * 0.9 \
                and any(u == 0x41 and g in glyph_ids for u, g in out) and (best is None or n > len(best[1])):
            best = (p + 4, out)
    return best

def _font_by_bytes(o):
    """2026-10-19: a v1.0.6 font asset without a type tree. Its name says which base font it is; the base font gives
    the record sizes; the tables are then found in the v1.0.6 bytes by their shape and every record is checked
    (rectangles inside the atlas, sizes agreeing). The glyph pixels are compared with UnityPy afterwards (font_check)."""
    if not _base_fonts: return None
    raw = o.get_raw_data()
    nm = _mb_name(raw)
    bf = next((f for f in _base_fonts if f["src_name"] == nm), None)
    if bf is None or bf["kind"] != 1: return None
    atlas = None
    for t in o.assets_file.objects.values():           # its atlas: the texture of the same name in the same file
        if t.type.name != "Texture2D": continue
        try:
            if t.peek_name() == bf["atlas_name"]: atlas = t; break
        except Exception:
            continue
    if atlas is None: raise ValueError(f"{nm}: atlas {bf['atlas_name']} not found in v1.0.6")
    tex = atlas.read(); fmt = int(tex.m_TextureFormat)
    if fmt not in (1, 4, 63): raise ValueError(f"atlas format {fmt}")
    # the record sizes may have grown in Unity 6 (new fields go at the end): the base size first, then others
    gs = g_st = None
    for g_st in [bf["g_st"]] + [v for v in range(40, 81, 4) if v != bf["g_st"]]:
        gs = _scan_glyphs(raw, g_st, tex.m_Width, tex.m_Height)
        if gs: break
    if not gs:
        hint = []                                       # where the base font's counts appear, and what follows
        for want in (bf["gn"], bf["cn"]):
            p0 = raw.find(struct.pack("<i", want))
            if p0 >= 0: hint.append(f"count {want} at {p0}: {raw[p0 + 4:p0 + 68].hex()}")
        raise ValueError(f"{nm}: glyph table not found (record sizes 40-80, base {bf['g_st']}, {len(raw)} bytes, "
                         f"atlas {tex.m_Width}x{tex.m_Height}) {'; '.join(hint) or 'counts not found'}")
    g_at, gl = gs
    cs = c_st = None
    for c_st in [bf["c_st"]] + [v for v in range(12, 41, 4) if v != bf["c_st"]]:
        cs = _scan_chars(raw, c_st, {g[0] for g in gl})
        if cs: break
    if not cs:
        hint = []
        for want in (bf["cn"], len(gl)):
            p0 = raw.find(struct.pack("<i", want))
            if p0 >= 0: hint.append(f"count {want} at {p0}: {raw[p0 + 4:p0 + 68].hex()}")
        raise ValueError(f"{nm}: character table not found (record sizes 12-40, base {bf['c_st']}, glyph table at {g_at} "
                         f"with {len(gl)}, base had {bf['gn']} glyphs / {bf['cn']} characters) {'; '.join(hint) or 'counts not found'}")
    c_at, chl = cs
    if (g_st, c_st) != (bf["g_st"], bf["c_st"]):
        stats[f"font: record sizes in v1.0.6 {g_st}/{c_st} (base {bf['g_st']}/{bf['c_st']}) ({nm})"] += 1
    by_index = {g[0]: g for g in gl}
    chars = {}
    for uni, gi in chl:
        if FONT_FIRST <= uni <= FONT_LAST and gi in by_index:
            idx, mw, mh, bx, by, adv, x, y, w, h, ai = by_index[gi]
            if ai == 0 and x + w <= tex.m_Width and y + h <= tex.m_Height:
                chars[uni] = (x, y, w, h, bx, by, adv)
    blob = getattr(tex, "m_PlatformBlob", None) or []; gobs = 0
    if getattr(tex.object_reader, "platform", None) == BuildTarget.Switch and len(blob) >= 12:
        gg = 1 << int.from_bytes(bytes(blob[8:12]), "little"); gobs = gg if gg > 1 else 0
    bp, start = _stream_start(o, raw)
    sd = getattr(tex, "m_StreamData", None)
    if sd and sd.path:
        cab = sd.path.split("/")[-1]
        if cab not in node_at: raise ValueError("atlas resource file not found")
        tbp, noff, _ = node_at[cab]; toff, tsize = noff + sd.offset, sd.size
    else:
        traw = atlas.get_raw_data(); img = bytes(tex.image_data)
        tbp, tstart = _stream_start(atlas, traw)
        at = traw.rfind(struct.pack("<i", len(img)) + img[:64])
        if at < 0 or traw[at + 4: at + 4 + len(img)] != img: raise ValueError("atlas pixels not found")
        toff, tsize = tstart + at + 4, len(img)
    if tbp != bp: raise ValueError("atlas texture in another bundle")
    stats[f"font: found in v1.0.6 by its tables ({nm}: {len(gl)} glyphs, {len(chl)} characters)"] += 1
    return {"name": bf["name"], "kind": 1, "bundle": bp, "g_at": start + g_at, "gn": len(gl), "g_st": g_st,
            "c_at": start + c_at, "cn": len(chl), "c_st": c_st, "sdf": bf["sdf"], "mode": bf["mode"], "pad": bf["pad"],
            "chars": chars, "ascii": sum(1 for c in range(0x21, 0x7F) if c in chars), "tex": tex, "tex_obj": atlas,
            "t": (bp, toff, tsize, tex.m_Width, tex.m_Height, fmt, gobs)}

if _base and os.environ.get("DTD_V106_ONLY"):        # the font from v1.0.6's own files too
    _borrow_font_layouts([p for p in font_bundles if os.path.exists(os.path.join(_base, os.path.relpath(p, root)))])
    font_bundles = [p for p in font_bundles if not os.path.exists(os.path.join(_base, os.path.relpath(p, root)))]
    print(f"font: {len(_borrowed)} layout(s) borrowed, {len(_base_fonts)} base font(s) to look for in v1.0.6", flush=True)
for p in font_bundles:
    try:
        fenv = UnityPy.load(p)
    except Exception:
        continue
    for o in fenv.objects:
        if o.type.name != "MonoBehaviour": continue
        try:
            tt = _read_font_tt(o)
        except Exception as e:
            stats[f"font scan: MonoBehaviour without a readable type tree ({type(e).__name__})"] += 1
            try:
                f106 = _font_by_bytes(o)
            except Exception as e2:
                font_notes.append(f"by bytes: {type(e2).__name__}: {e2}"); f106 = None
            if f106: fonts.append(f106)
            continue
        kind = 1 if ("m_GlyphTable" in tt and "m_CharacterTable" in tt) else 2 if "m_glyphInfoList" in tt else 0
        if not kind or not (tt.get("m_GlyphTable") or tt.get("m_glyphInfoList")): continue
        try:
            fonts.append(analyse_font(o, tt, kind))
        except Exception as e:
            font_notes.append(f"{tt.get('m_Name')}: {type(e).__name__}: {e}")
print(f"{len(fonts)} usable fonts in {len(font_bundles)} bundles ({len(font_notes)} not usable)", flush=True)

def font_module_read(f, x, y, w, h):
    """The atlas pixels of one glyph rect read the module's way (rows bottom-up, as stored)."""
    bp, off, size, tw, th, fmt, gobs = f["t"]
    bpp = 4 if fmt == 4 else 1; bw = 16 // bpp
    bxn = -(-tw // (bw * 4)) * 4 if gobs else -(-tw // bw)
    b = bundles[bp]; rows = []
    for yy in range(y, y + h):
        row = bytearray()
        for xx in range(x, x + w):
            o = off + block_index(xx // bw, yy, bxn, gobs) * 16 + (xx % bw) * bpp + (3 if fmt == 4 else 0)
            row.append(b.read(o, 1)[0])
        rows.append(bytes(row))
    return rows

def font_reference(f):
    """UnityPy's atlas as one channel, rows bottom-up (as stored)."""
    if "_ref" in f: return f["_ref"]
    img = f["tex"].image
    if img.mode in ("RGBA", "LA"):    # the one channel that carries the glyphs (UnityPy may put it in alpha or in grey)
        chans = [img.getchannel(c) for c in (("A", "R") if img.mode == "RGBA" else ("A", "L"))]
        ch = max(chans, key=lambda c: (lambda e: e[1] - e[0])(c.getextrema()))
    else:
        ch = img.convert("L")
    f["_ref"] = ch.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    return f["_ref"]

def font_check(f):
    ref = font_reference(f); bad = []
    for c in [ord(ch) for ch in "AgW0"] + [0x21]:
        if c not in f["chars"]: continue
        x, y, w, h = f["chars"][c][:4]
        if not w or not h: continue
        mine = font_module_read(f, x, y, w, h)
        want = [bytes(ref.crop((x, yy, x + w, yy + 1)).tobytes()) for yy in range(y, y + h)]
        if mine != want: bad.append(chr(c))
    return bad

def font_pack(f, ref):
    """The module's packed atlas (coverage, kFontCap capitals) and its glyph boxes."""
    ch = f["chars"]
    H = ch.get(ord("H")) or ch.get(ord("A"))
    cap = H[5] if H and H[5] > 4 else (H[3] if H else 0)
    if cap < 4: raise ValueError("no capital H / A")
    s = min(1.0, FONT_CAP / cap); m = 2; grad = f["pad"] + 1
    W, x, y, rowh, glyphs, pieces = 1024, 1, 1, 0, {}, []
    for c in range(FONT_FIRST, FONT_LAST + 1):
        if c not in ch: continue
        gx, gy, gw, gh, bx, by, adv = ch[c]
        if gw <= 0 or gh <= 0:
            glyphs[c] = (0, 0, 0, 0, 0, 0, round(adv * s)); continue
        sw, sh = gw + 2 * m, gh + 2 * m
        dw, dh = max(1, math.ceil(sw * s)), max(1, math.ceil(sh * s))
        if x + dw + 1 > W: x, y, rowh = 1, y + rowh + 1, 0
        glyphs[c] = (x, y, dw, dh, round((bx - m) * s), round((by + m) * s), round(adv * s))
        src = ref.crop((gx - m, gy - m, gx - m + sw, gy - m + sh)).transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        if f["sdf"]:
            src = src.point(lambda a: max(0, min(255, round(((a / 255 - 0.5) * grad + 0.5) * 255))))
        pieces.append((src.resize((dw, dh), Image.BOX), x, y))
        x += dw + 1; rowh = max(rowh, dh)
    atlas = Image.new("L", (W, y + rowh + 1), 0)
    for im, px, py in pieces: atlas.paste(im, (px, py))
    return atlas, glyphs, round(cap * s)

def font_render(f, text, size=40):
    atlas, glyphs, line = font_pack(f, font_reference(f))
    k = size / line
    width = int(sum(glyphs.get(ord(c), (0,) * 6 + (line // 2,))[6] for c in text) * k) + 20
    canvas = Image.new("L", (max(width, 10), int(size * 1.8)), 0)
    pen, base = 10.0, int(size * 1.25)
    for c in text:
        g = glyphs.get(ord(c))
        if not g: pen += size / 2; continue
        gx, gy, gw, gh, bx, by, adv = g
        if gw and gh:
            piece = atlas.crop((gx, gy, gx + gw, gy + gh)).resize((max(1, int(gw * k)), max(1, int(gh * k))), Image.NEAREST)
            canvas.paste(Image.new("L", piece.size, 255), (int(pen + bx * k), int(base - by * k)), piece)
        pen += adv * k
    return canvas

font, font_alt = None, []
if fonts:
    # the game's menu lettering is 210 Youth Bold (Snowstorm is its digital-watch face, Noto / Roboto
    # cover other languages); DTD_FONT="<pattern>" picks another
    pick = re.compile(os.environ.get("DTD_FONT") or r"^210YouthB", re.I)
    good = re.compile(r"(?i)(eng|_en\b|latin|main|default|base|regular)")
    badn = re.compile(r"(?i)(outline|shadow|jp|kr|cn|tc|sc|ko|ja|zh|ru|thai|arab|number|digit|icon|emoji|_sdf_?\d|fallback)")
    usable = [f for f in fonts if f["ascii"] >= 90]
    usable.sort(key=lambda f: (not (pick and pick.search(f["name"])), not good.search(f["name"]), bool(badn.search(f["name"])),
                               -len(f["chars"]), -f["t"][3] * f["t"][4]))
    for f in usable:
        try:
            bad = font_check(f)
        except Exception as e:
            bad = [f"{type(e).__name__}: {e}"]
        f["bad"] = bad
        if not bad and font is None:
            font = f
    font_alt = [f for f in usable if f.get("bad") == [] and f is not font]   # also listed, after the one in use
    try:
        rows = []
        for n, f in enumerate([f for f in fonts if f["ascii"] >= 60][:16], 1):
            mark = " <- used" if f is font else ""
            label_img = Image.new("L", (900, 30), 0)
            ImageDraw.Draw(label_img).text((6, 6), f"{n}. {f['name']}  ({len(f['chars'])} glyphs, sdf {f['sdf']}, pad {f['pad']}){mark}", fill=200)
            rows += [label_img, font_render(f, "DAVE THE DIVER  Dave the Diver 0123456789"), font_render(f, "Bancho Sushi: Tuna Nigiri x2  can make 5")]
        if rows:
            prev = Image.new("L", (max(r.width for r in rows), sum(r.height for r in rows)), 0)
            yy = 0
            for r in rows: prev.paste(r, (0, yy)); yy += r.height
            prev.save(os.path.join(out, "font_preview.png"))
    except Exception as e:
        font_notes.append(f"preview: {type(e).__name__}: {e}")

# ---------------------------------------------------------------- 5. pictures.idx (only the pictures that came out right)
bundle_ids = {}
lines = ["DTDP 1"]
font_bundle = [f["t"][0] for f in ([font] + font_alt if font else [])]
for bp in sorted({t[0] for t in textures} | set(font_bundle)):
    bundle_ids[bp] = len(bundle_ids)
    lines.append(f"B {bundle_ids[bp]} {os.path.relpath(bp, root).replace(os.sep, '/')} {os.path.getsize(bp)}")
for i, (bp, off, size, w, h, fmt, gobs) in enumerate(textures):
    lines.append(f"T {i} {bundle_ids[bp]} {off} {size} {w} {h} {fmt} {gobs}")
# the fonts: each one's atlas texture, then where its glyph tables sit (positions only). The module
# uses the first "F" line; the others are there so another font needs no new index.
for n, f in enumerate([font] + font_alt if font else []):
    ti = len(textures) + n
    lines.append(f"T {ti} {bundle_ids[f['t'][0]]} " + " ".join(str(v) for v in f["t"][1:]))
    lines.append(f"F {f['name']} {ti} {f['kind']} {f['g_at']} {f['gn']} {f['g_st']} {f['c_at']} {f['cn']} {f['c_st']} {f['sdf']} {f['pad']}")
for key in sorted(k for k in sprites if k not in bad):
    name, ti, x, y, w, h, rot, tight, _ = sprites[key]
    lines.append(f"S {name} {ti} {x} {y} {w} {h} {rot}")
idx = os.path.join(out, "pictures.idx")
with open(idx, "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print(f"wrote {idx} ({os.path.getsize(idx)} bytes)", flush=True)

print("\n== report")
print(f"pictures in index: {len(sprites)}, textures: {len(textures)}, bundles: {len(bundle_ids)}, largest block {big}")
print(f"decoded the module's way: {same} identical to UnityPy, {len(differ)} different, "
      f"{tight_differ} different but tight-packed, {len(failed)} failed")
print(f"own LZ4 decoder used for the first {min(own_checked, 5)} pictures")
for k, v in sorted(stats.items()):
    print(f"  {k}: {v}")
for tx in sorted({(t[3], t[4], t[5], t[6]) for t in textures})[:20]:
    print(f"  texture {tx[0]}x{tx[1]} format {tx[2]} gobs {tx[3]}")
mx = max((sprites[k][4] for k in sprites), default=0), max((sprites[k][5] for k in sprites), default=0)
print(f"  largest picture {mx[0]} x {mx[1]}")
print(f"\n== font: {len(fonts)} TextMeshPro fonts found")
for f in fonts:
    print(f"  {f['name']}: kind {f['kind']}, {f['gn']} glyphs, {len(f['chars'])} in U+0020..U+00FF ({f['ascii']}/94 ASCII), "
          f"atlas {f['t'][3]}x{f['t'][4]} format {f['t'][5]} gobs {f['t'][6]}, render mode {f['mode']} (sdf {f['sdf']}), "
          f"padding {f['pad']}, check: {'ok' if f.get('bad') == [] else f.get('bad', 'not checked')}" + ("   <- used" if f is font else ""))
for n in font_notes[:20]: print("  not usable:", n)
print("  font: " + (f"{font['name']} (preview in font_preview.png)" if font else "none usable -- the companion keeps its own lettering"))
for d in differ: print("  differ:", d)
for d in failed: print("  failed:", d)
for d in failed[:15]: print("  failed:", d)
