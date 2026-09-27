#!/usr/bin/env python3
"""P5R map overlay package snippet (area maps, fog, icons, player marker).
Reads (read-only, see p5r_paths.py): the road-map export, the v0.6 map area bounds, the extracted
minimap sprites and the D4B1 main image (icon table). Writes only the given output directory.
Native rules reproduced:
  * composite per area = floor art (type-0 composition), type 1/2 pieces in PARTS order
    (conditional ones gated by map.part.<i>), then per visible cover (type 18/41 rect, 27/42 quad):
      - an opaque background patch (native: cover alpha excludes the floor art), then
      - the red floor-art patch, hidden by map.fog.dark: C32D40/C331F0 draw the floor art tinted
        RGBA(255,0,0, 75+20cos t) under everything when the map is known (module map.fog.red).
  * every composite is cropped to its content bbox (art alpha, pieces, icon points) and the
    area's world min/max are moved by the same amount, so map coordinates are unchanged.
  * icons tinted by the per-type RGB at main+163EC74+0xC (C36044 / C378B0 vertex colour).
  * player: sprite 12 (C36EE0, pivot 10,19, white vertex colour) pre-rotated in 64 buckets
    (5.625 deg, screen-clockwise, bucket 0 = as authored) around the pivot.
"""
# Asset-free generator: every image is a recipe Art (replayed by the P5R module from the user's romfs).
# usage: map_gen_af.py <out dir> [png|module]  -> <out>/snippet.json (+ <out>/maps/*.png in png mode,
# <out>/map_recipes.json {file name: recipe} in module mode). Same art, same numbers.
import json, struct, sys
from pathlib import Path
from PIL import Image
sys.path.insert(0, str(Path(__file__).parent))
from recipe import Art
OUT = Path(sys.argv[1]); MODE = sys.argv[2] if len(sys.argv) > 2 else 'png'; MAPS = OUT / 'maps'
if MAPS.exists():
    for f in MAPS.iterdir(): f.unlink()
MAPS.mkdir(parents=True, exist_ok=True)
RECIPES = {}
def save(art, fn):
    RECIPES[fn] = art.recipe()
    if MODE == 'png':
        art.im.save(MAPS / fn)
def rmap(a, b, layer):
    return Art.dds(f'BASE/FIELD/PANEL/ROADMAP/RMAP_{a:03}_{b}_{layer}.DDS')
from p5r_paths import MAIN_IMG as IMG, MAP_AREAS as MANIFEST, MINIMAP_SPRITES as SPRITES, ROADMAP_EXPORT as EXPORT  # noqa: E402
# MANIFEST: the pre-crop area bounds of the v0.6 package (map.areas of its manifest)
BG = (0x10, 0x0C, 0x0E)          # map widget "bg" #FF100C0E (what the companion shows behind art)
RED_ALPHA = 75 / 255             # C32D90: 75 + 20*cos(t); the companion uses the mean
CELL = 64; MARGIN = 12; ICON_PAD = 24; BUCKETS = 64

maps = json.load(open(EXPORT / 'maps.json'))
variants = {}
for f in maps['fields']:
    for v in f['variants']:
        variants[(v['asset_major'], v['asset_minor'])] = v
areas = json.load(open(MANIFEST))['map']['areas']

def red_patch(art):
    """native covered look over the widget background: bg*(1-a') + (R,0,0)*a', opaque"""
    return art.fog(BG + (255,), 75, 255)

def identity(p):
    x, y, w, h, dx, dy = p['rect']
    return p['condition_flag'] == -1 and p['type'] == 1 and x == dx and y == dy

composites = {}; area_patch = {}; assets = []; crops = {}
save(Art.new(4, 4, BG + (255,)), 'P5RFOGBG.png'); assets.append('P5RFOGBG.png')
for name, area in sorted(areas.items()):
    _, a, b, layer = name.split('_'); a, b, layer = int(a), int(b), int(layer)
    v = variants[(a, b)]
    img_entry = next(i for i in v['images'] if i['layer'] == layer)
    minx, miny, maxx, maxy = img_entry['map_bounds']
    assert minx == 0 and miny == 0
    W, H = round(maxx), round(maxy)
    assert Image.open(EXPORT / img_entry['composed_png']).size == (W, H), name
    raw = rmap(a, b, layer)
    # visible art = type-0 stencil (W x H) intersected with the unconditional identity type-1
    # copies (C32FE0 stencil pass, C347C0 colour pass); other pieces become layers below
    base = Art.new(W, H)
    for p in v['parts']:
        if identity(p):
            x, y, w, h, _, _ = p['rect']
            bx1, by1 = min(W, round(x + w)), min(H, round(y + h))
            if bx1 > x and by1 > y:
                base = base.paste(raw.crop((round(x), round(y), bx1, by1)), round(x), round(y))
    # --- content bbox
    bb = base.im.split()[3].getbbox() or (0, 0, W, H)
    x0, y0, x1, y1 = bb
    for p in v['parts']:
        if p['type'] in (1, 2) and not identity(p):
            _, _, w, h, dx, dy = p['rect']; x0 = min(x0, dx); y0 = min(y0, dy); x1 = max(x1, dx + w); y1 = max(y1, dy + h)
    for p in v['icon']:
        dx, dy = p['rect'][4], p['rect'][5]
        x0 = min(x0, dx - ICON_PAD); y0 = min(y0, dy - ICON_PAD); x1 = max(x1, dx + ICON_PAD); y1 = max(y1, dy + ICON_PAD)
    x0 = max(0, int(x0) - MARGIN); y0 = max(0, int(y0) - MARGIN)
    x1 = min(W, int(x1 + 0.999) + MARGIN); y1 = min(H, int(y1 + 0.999) + MARGIN)
    CW, CH = x1 - x0, y1 - y0
    crops[name] = (x0, y0, x1, y1)
    base_fn = f'P5RBASE_{a:03}_{b}_{layer}.png'
    save(base.crop((x0, y0, x1, y1)), base_fn); assets.append(base_fn)
    layers = [dict(src=f'file:maps/{base_fn}', rect=[0, 0, CW, CH])]
    for i, p in enumerate(v['parts']):
        t = p['type']; x, y, w, h, dx, dy = p['rect']
        if t in (1, 2):
            if identity(p):
                continue  # unconditional texture-to-same-place copy: already in the base art
            fn = f'P5RPIECE_{a:03}_{b}_{layer}_{i}.png'
            save(raw.crop((round(x), round(y), round(x + w), round(y + h))), fn); assets.append(fn)
            L = dict(src=f'file:maps/{fn}', rect=[dx - x0, dy - y0, w, h])
            if p['condition_flag'] != -1: L['show_bind'] = f'map.part.{i}'
            layers.append(L)
    red_fn = f'P5RFOGRED_{a:03}_{b}_{layer}.png'; need_red = False
    for i, p in enumerate(v['parts']):
        t = p['type']
        if t not in (18, 27, 41, 42): continue
        if t in (18, 41):
            _, _, w, h, dx, dy = p['rect']
            if w <= 0 or h <= 0: continue
            box = [dx - x0, dy - y0, w, h]; extra = {}
        else:
            q = struct.unpack_from('<8f', bytes.fromhex(p['raw_hex']), 0x24)
            xs, ys = q[0::2], q[1::2]; qx0, qy0, qx1, qy1 = min(xs), min(ys), max(xs), max(ys)
            if qx1 - qx0 < 1 or qy1 - qy0 < 1: continue
            P = [(q[2 * k] - qx0, q[2 * k + 1] - qy0) for k in range(4)]
            m = Art.new(round(qx1 - qx0), round(qy1 - qy0)).polygon([P[0], P[1], P[2]], (255, 255, 255, 255)) \
                .polygon([P[1], P[2], P[3]], (255, 255, 255, 255))
            fn = f'P5RFOGQ_{a:03}_{b}_{layer}_{i}.png'; save(m, fn); assets.append(fn)
            box = [qx0 - x0, qy0 - y0, qx1 - qx0, qy1 - qy0]; extra = {'mask_src': f'file:maps/{fn}'}
        layers.append(dict(src='file:maps/P5RFOGBG.png', rect=box, show_bind=f'map.part.{i}', **extra))
        layers.append(dict(src=f'file:maps/{red_fn}', src_px=list(box), rect=box,
                           show_bind=f'map.part.{i}', hide_bind='map.fog.dark', **extra))
        need_red = True
    if need_red:
        save(red_patch(base.crop((x0, y0, x1, y1))), red_fn); assets.append(red_fn)
    composites[name] = dict(w=CW, h=CH, layers=layers)
    mn, mx = area['min'], area['max']
    assert mn[0] == 0 and mx[1] == 0 and mx[0] == W and mn[1] == -H, (name, mn, mx, W, H)
    area_patch[name] = {'image': f'composite:{name}', 'min': [float(x0), float(-y1)], 'max': [float(x1), float(-y0)]}

# --- icon atlas
img = IMG.read_bytes()
table = {t: (struct.unpack_from('<I', img, 0x163ec74 + 0x14 * t)[0], img[0x163ec74 + 0x14 * t + 0xc:0x163ec74 + 0x14 * t + 0xf])
         for t in range(0, 120)}
files = {int(p.name[:4]): p for p in SPRITES.glob('*.png')}
spr = lambda sid: Art.sprite('minimap', sid)
used_types = sorted({p['type'] for v in variants.values() for p in v['icon']})

def tint(s, rgb):
    return s.tint(rgb)
def fit(s):
    sc = (CELL - 4) / max(s.size)
    return s.resize(max(1, round(s.width * sc)), max(1, round(s.height * sc)))

cells = []; colours = {}
for t in used_types:
    sid, rgb = table[t]
    if sid in files:
        colours[t] = rgb.hex(); cells.append((f'p5r_t{t}', fit(tint(spr(sid), tuple(rgb)))))
cells.append(('p5r_enemy', fit(tint(spr(13), (255, 0, 0)))))
# player: sprite 12, pivot (10,19) from C371A4/C371AC; square canvas centred on the pivot
ps = spr(12); PX, PY = 10, 19
R = int(max(((x - PX) ** 2 + (y - PY) ** 2) ** 0.5 for x in (0, ps.width) for y in (0, ps.height))) + 1
sq = Art.new(2 * R, 2 * R).over(ps, R - PX, R - PY)
for k in range(BUCKETS):
    rot = sq.rotate(-k * 360 / BUCKETS, expand=False)   # PIL rotates counter-clockwise
    cells.append((f'p5r_player{k}', rot.resize(CELL, CELL)))
cols = 16; rows = (len(cells) + cols - 1) // cols
atlas = Art.new(cols * CELL, rows * CELL); icons = {}
for k, (nm, s) in enumerate(cells):
    r_, c_ = divmod(k, cols)
    atlas = atlas.over(s, c_ * CELL + (CELL - s.width) // 2, r_ * CELL + (CELL - s.height) // 2)
    icons[nm] = {'r': r_, 'c': c_}
save(atlas, 'P5R_MINIMAP_ATLAS.png'); assets.append('P5R_MINIMAP_ATLAS.png')
dyn = [
    {'group': 'p5r_poi', 'count': 64, 'x': 'map.poi.{i}.x', 'y': 'map.poi.{i}.y', 'kind': 'map.poi.{i}.kind',
     'icon_by_kind': {str(t): f'p5r_t{t}' for t in used_types if f'p5r_t{t}' in icons}, 'anchor': [0.5, 0.5],
     'size': 64, 'show_bind': 'map.overlay.ready'},
    {'group': 'p5r_radar', 'count': 20, 'x': 'map.enemy.{i}.x', 'y': 'map.enemy.{i}.y', 'kind': 'map.enemy.{i}.kind',
     'icon_by_kind': {'1': 'p5r_enemy'}, 'anchor': [0.5, 0.5], 'size': 30, 'show_bind': 'map.radar.ready'},
    {'group': 'p5r_player', 'count': 1, 'x': 'map.player.{i}.x', 'y': 'map.player.{i}.y', 'kind': 'map.player.{i}.kind',
     'icon_by_kind': {str(k): f'p5r_player{k}' for k in range(BUCKETS)}, 'anchor': [0.5, 0.5], 'size': 60},
]
snippet = {
    '_about': 'v0.7 map fixes: composites (cropped, red fog), area image+min/max patch, atlas, dynamic markers for every area, map widget marker binds removed (player drawn as the native rotated triangle).',
    'composites': composites,
    'map': {'atlas': 'file:maps/P5R_MINIMAP_ATLAS.png', 'cell': CELL, 'icons': icons},
    'area_patch': area_patch,
    'dynamic_markers_all_areas': dyn,
    'map_widget_drop': ['marker_x_bind', 'marker_y_bind', 'marker_src', 'marker_size', 'marker_anchor'],
    'icon_colours': colours,
}
(OUT / 'snippet.json').write_text(json.dumps(snippet, indent=1))
(OUT / 'assets.txt').write_text('\n'.join(sorted(set(assets))) + '\n')
(OUT / 'crops.json').write_text(json.dumps(crops))
(OUT / 'map_recipes.json').write_text(json.dumps(RECIPES))
print('composites', len(composites), 'assets', len(set(assets)), 'icons', len(icons), 'player canvas', 2 * R)
