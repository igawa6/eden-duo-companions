#!/usr/bin/env python3
"""Prepare village/castle/island/stage4 and native Separate Ways room outlines from the existing private map decoder/
extraction. Native treasure locations require the owned map (village 0xA9, castle 0x54,
island 0x55 -- stage4 has no map item of its own and is gated on island's 0x55, since
ss_map.cpp's own getStageNo() never returns a value past 4="island" even while physically in
a stage4/St4* room, i.e. the native game itself treats this area as part of the island
chapter for ownership purposes). Marker positions use the native cParts::pos path,
without a floor-mesh test. Bounds include every native point; out-of-bounds marks fail generation.
TREASURE_GATE preserves mark_model_tbl's per-file selection. Blue medallion/sub-mission
parts come from sub-file 9 under column 3 of that table.

Stage 4 (Mercenaries) is unavailable in the companion. Its generator path is retained
only for development with --include-stage4; release regeneration must omit that flag.
The default emits no stage-4 map areas, images or generated header rows.

Stage 5 (Separate Ways) comes from the Switch-native mapDataInit areas 18..25
(map_obj5a..5h.dat; analysis/20261114-gallery-sw.md B1), in the native placed frame like every
other stage. Each room keeps its own image and framing. Native SW treasure marks are drawn per
area where the native mark mask selects the area's own model (areas 22 and 25 draw none).

Run after prepare_assets.py, before prepare_r5.py.
"""
import argparse
import importlib.util
import json
import sys
import struct
from pathlib import Path
from PIL import Image
from map_legend import id_colours, prepare_marks, write_tables, native_room_model, room_model_images, native_goal_sprite, map_material

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('research', type=Path)
p.add_argument('map_file', type=Path)
p.add_argument('--golden-dir', type=Path, help='fixture-only reference raster output')
p.add_argument('--include-stage4', action='store_true', help='development only: emit unused Mercenaries map data')
a = p.parse_args()
if a.golden_dir:a.golden_dir.mkdir(parents=True, exist_ok=True)
sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[2]
package = root/'packages/ResidentEvil4/dualscreen'
spec = importlib.util.spec_from_file_location('re4_map_source', a.research/'assets/tools/map_decode.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)
spec = importlib.util.spec_from_file_location('re4_room_tables', a.research/'work/maps/room_tables.py')
room_tables = importlib.util.module_from_spec(spec)
spec.loader.exec_module(room_tables)
cmn_dir = a.map_file.resolve().parent
common_buf, common_entries = decoder.load_area(cmn_dir.parent/'eng/ss_map.dat')
native_colours = id_colours(decoder.slice_entry(common_buf, common_entries[5-4]))
spec = importlib.util.spec_from_file_location('re4_art', a.research/'design/mockups/direction-a2/render.py')
art = importlib.util.module_from_spec(spec)
spec.loader.exec_module(art)
room_texture = art.tex('3a000009',27)


# The SAT mesh is local to the first room model, not in stage-map coordinates.
# ss_map.cpp mapModelInit / mapPositionCheck apply parts[0].mat; r10e also scales x10.
geometry = {}
visual_models = {}
art_sources = {}
art_rows = []
multi_rooms = set()
state_overlays = []
state_rooms = []
# Switch mapDataInit tables add a castle room absent from the older GC list.
# It contributes geometry only; MapArea intentionally does not enable a live marker.
room_tables.CASTLE_AREAS = [(name, table+([(0x814, 0x26)] if name == 'map_obj2a.dat' else []))
                            for name, table in room_tables.CASTLE_AREAS]
NATIVE_HEADING = {0x10d:-40, 0x117:-70, 0x222:-90, 0x226:-90, 0x227:-90,
                  0x228:-90, 0x22a:-90, 0x21a:-90, 0x21b:-90, 0x503:-70}

def placed_room(buf, entry, room_id, key, filename):
    room = decoder.decode_room(buf, entry)
    sub = decoder.slice_entry(buf, entry)
    models = [decoder.slice_entry(sub, e) for e in decoder.read_container(sub) if e[0] == 'BIN']
    model = models[0]
    if model[0x29] != 1:
        raise ValueError(f'{key}: unsupported room model hierarchy')
    head = struct.unpack_from('<Q', model)[0]
    part, parent, _, _, tx, ty, tz = struct.unpack_from('<4B3f', model, head)
    if (part, parent) != (0, 255):
        raise ValueError(f'{key}: unsupported room root')
    scale = 10 if room_id == 0x10e else 1
    visual_models[key] = [native_room_model(m,scale) for m in models]
    art_sources[key] = (filename, decoder.read_container(buf).index(entry), scale)
    local, world = room['sats']
    vertices = [(scale*(v[0]+tx), scale*(v[1]+ty), scale*(v[2]+tz)) for v in local['vtx']]
    room['sats'][0] = dict(local, vtx=vertices)
    polys = world['polys'][:world['n_floor']+world['n_slope']]
    if any(max(p[:3]) >= len(vertices) for p in polys):
        raise ValueError(f'{key}: incompatible native hit meshes')
    # Separate Ways only: where the map mesh is exactly world/10 (+ root), the native transfer
    # is that affine map, kept off-mesh as the earlier live SW contract. Rotated/distorted
    # pieces never guess off-mesh.
    affine = key.startswith('stage5-') and all(abs(l[0]-w[0]/10) < 1 and abs(l[2]-w[2]/10) < 1
                                               for l, w in zip(local['vtx'], world['vtx']))
    geometry[key] = (world['vtx'], vertices, polys,
                     len(models)>1 or room_id in (0x107, 0x10f, 0x217, 0x320),
                     NATIVE_HEADING.get(room_id, 0), affine, (scale*tx, scale*tz))
    print(f'{key}: root=({tx}, {tz}), scale={scale}, floors={len(polys)}')
    return room


def build_overview(rooms, order, png_name, markers=()):
    """rooms: {room_id: decoded room}. order: room-id list giving poly contribution order
    (village precedent: ROOM_TABLE_ST1's own order). Returns (lo_x, hi_x, lo_z, hi_z)."""
    sats = [rooms[room_id]['sats'][0] for room_id in order]
    vertices = [v for sat in sats for v in sat['vtx']] + list(markers)
    xs, zs = [v[0] for v in vertices], [v[2] for v in vertices]
    # Keep a complete 48px star inside the fitted overview, including edge marks.
    margin = max(120, .04*max(max(xs)-min(xs), max(zs)-min(zs))) if markers else 120
    lo_x, hi_x = min(xs)-margin, max(xs)+margin
    lo_z, hi_z = min(zs)-margin, max(zs)+margin
    scale = min(1600/(hi_x-lo_x), 1600/(hi_z-lo_z))
    # Room visibility and colour belong to the native save state, not the base image.
    # Preserve the exact bounds and placement while leaving hidden rooms transparent.
    Image.new('RGBA', (1, 1)).save(package/'assets'/png_name)
    stage = png_name.removesuffix('.png').removesuffix('-overview')
    for room_id, sat in zip(order, sats):
        key = stage if stage.startswith('stage5-') else f'{stage}-{room_id:03x}'
        from map_legend import room_images
        models=visual_models[key]
        box,_=room_images([v for model in models for v in model['vertices']],[],scale,[(0,0,0)])
        multi=len(models)>1
        art_rows.append((key,*art_sources[key],scale))
        if multi:multi_rooms.add(key)
        if a.golden_dir:
            _,variants,_=room_model_images(models,scale,room_texture,native_colours)
            for n,variant in variants.items():
                variant.save(a.golden_dir/f'map-room-{key}-{n}.png',optimize=True,compress_level=9)
        state_overlays.append(dict(src='module:re4/room/'+key.replace('-','/')+'/1',
            src_bind=f're4.map.room.{key}.art', show_bind=f're4.map.room.{key}.show',
            x0=box[0], y0=box[1], x1=box[2], y1=box[3]))
        state_rooms.append((key, room_id))
    return lo_x, hi_x, lo_z, hi_z


areas = {}

# ---- Village ----
buf, entries = decoder.load_area(a.map_file)
treasures = decoder.scan_marks(decoder.slice_entry(buf, entries[8-4]))
if len(treasures) != 14:
    raise ValueError('unsupported village treasure part count')
village_order = [room_id for room_id, _ in decoder.ROOM_TABLE_ST1]
village_rooms = {room_id: placed_room(buf, entries[number-4], room_id, f'village-{room_id:03x}', a.map_file.name) for room_id, number in decoder.ROOM_TABLE_ST1}
medallions = decoder.scan_marks(decoder.slice_entry(buf, entries[9-4]))
assert len(medallions) == 15
lo_x, hi_x, lo_z, hi_z = build_overview(village_rooms, village_order, 'village-overview.png', treasures+medallions)
def marker_icons(points, kind, bounds, stage, binds):
    x0, x1, z0, z1 = bounds
    result = []
    for n, (x, y, z) in enumerate(points):
        if not (x0 < x < x1 and z0 < z < z1):
            raise ValueError(f'{stage} {kind}[{n}]: native point ({x}, {z}) outside overview {bounds}')
        result.append({'k':kind+'_location', 'i':kind, 'x':x, 'y':-z,
                       'size':12 if kind == 'medallion' else 48,
                       'collectible':False, 'show_bind':binds(n)})
    return result

# Map renderer iterates in list order: treasure stars stay above sub-mission clusters.
icons = marker_icons(medallions, 'medallion', (lo_x, hi_x, lo_z, hi_z), 'village',
                     lambda n:f're4.medallion.{n}.show')
icons += marker_icons(treasures, 'treasure', (lo_x, hi_x, lo_z, hi_z), 'village',
                      lambda n:'re4.treasure.show')
for room_id in village_order:
    areas[f'village-{room_id:03x}'] = {'image':'file:assets/village-overview.png',
        'min':[lo_x,-hi_z], 'max':[hi_x,-lo_z], 'icons':icons}

# ---- Castle (st2) / Island (st3) / Stage4: multi-file stages.
# Verified Separate Ways rooms are handled separately below. ----
EXPECTED_TREASURES = {'castle': 17, 'island': 7, 'stage4': 35}
# ss_map.cpp mark_model_tbl[18][5] column 2 (treasure), by map_objNN.dat: whether this area's
# OWN "no=8" sub-file is used for treasure marks at all (1) or the area falls back to a
# shared/common model instead (0) -- in which case this file's own "no=8" BIN, even if
# well-formed, is never shown as a treasure mark by the native game and must not be scanned.
# village (area1) and every castle file (areas2-4) are always 1 (omitted below, default True).
TREASURE_GATE = {
    'map_obj3a.dat': False, 'map_obj3b.dat': True,  'map_obj3c.dat': False,
    'map_obj3d.dat': False, 'map_obj3e.dat': True,  'map_obj3f.dat': False,
    'map_obj4a.dat': True,  'map_obj4b.dat': True,  'map_obj4c.dat': True,
    'map_obj4d.dat': True,  'map_obj4e.dat': False, 'map_obj4f.dat': False,
    'map_obj4g.dat': False,
}
def map_stages(tables, include_stage4=False):
    stages = [('castle', tables.CASTLE_AREAS), ('island', tables.ISLAND_AREAS)]
    if include_stage4:
        stages.append(('stage4', tables.STAGE4_AREAS))
    return stages


for stage, stage_areas in map_stages(room_tables, a.include_stage4):
    rooms, order, stage_treasures, stage_submissions = {}, [], [], []
    for fname, table in stage_areas:
        s_buf, s_entries = decoder.load_area(cmn_dir/fname)
        for room_id, no in table:
            # First file that defines a room id wins (island's 0x306 is shared by St3B/St3C
            # with identical geometry in both, per MAPS-REPORT.md).
            if room_id not in rooms:
                rooms[room_id] = placed_room(s_buf, s_entries[no-4], room_id, f'{stage}-{room_id:03x}', fname)
                order.append(room_id)
        idx = 8-4
        if TREASURE_GATE.get(fname, True) and idx < len(s_entries) and s_entries[idx][0] == 'BIN':
            stage_treasures.extend(decoder.scan_marks(decoder.slice_entry(s_buf, s_entries[idx])))
        # ss_map.cpp mark_model_tbl column 3: only these non-village areas
        # own a sub-mission model. Other sub-file 9 payloads are not markers.
        if fname in ('map_obj2a.dat', 'map_obj2b.dat', 'map_obj3b.dat', 'map_obj3e.dat'):
            stage_submissions.extend(decoder.scan_marks(decoder.slice_entry(s_buf, s_entries[9-4])))
    expected = EXPECTED_TREASURES[stage]
    if len(stage_treasures) != expected:
        raise ValueError(f'{stage}: expected {expected} decoded treasure marks, got {len(stage_treasures)}')
    s_lo_x, s_hi_x, s_lo_z, s_hi_z = build_overview(rooms, order, f'{stage}-overview.png', stage_treasures+stage_submissions)
    s_icons = marker_icons(stage_submissions, 'medallion', (s_lo_x, s_hi_x, s_lo_z, s_hi_z),
                           stage, lambda n:'re4.submission.show')
    s_icons += marker_icons(stage_treasures, 'treasure', (s_lo_x, s_hi_x, s_lo_z, s_hi_z),
                            stage, lambda n:'re4.treasure.show')
    for room_id in order:
        areas[f'{stage}-{room_id:03x}'] = {'image':f'file:assets/{stage}-overview.png',
            'min':[s_lo_x,-s_hi_z], 'max':[s_hi_x,-s_lo_z], 'icons':s_icons}
    print(f'{stage}: {len(order)} rooms, {len(stage_treasures)} treasures + {len(stage_submissions)} sub-mission markers')

# Separate Ways: Switch-native mapDataInit areas 18..25 (jump table main+0xA129D0; getAreaNo
# stage-5 table main+0xA137D0). 0x511 has no native row (area 0); 0x51F (5h no=0x0D) and 0x520
# (5b no=0x13, no room file) are 1000x1000 placeholder hit squares: never mapped.
SEPARATE_WAYS_AREAS = [
    (18, 'map_obj5a.dat', [(0x500,0x0B),(0x501,0x0C),(0x502,0x0D),(0x503,0x0E)]),
    (19, 'map_obj5b.dat', [(0x504,0x0B),(0x505,0x0C),(0x506,0x0D),(0x507,0x0E),(0x508,0x0F),
                           (0x509,0x10),(0x50A,0x11),(0x50B,0x12)]),
    (20, 'map_obj5c.dat', [(0x50C,0x0B),(0x50D,0x0C),(0x50E,0x0D),(0x50F,0x0E),(0x510,0x0F)]),
    (21, 'map_obj5d.dat', [(0x512,0x0B),(0x514,0x0C)]),
    (22, 'map_obj5e.dat', [(0x513,0x0B)]),
    (23, 'map_obj5f.dat', [(0x515,0x0B),(0x516,0x0C),(0x517,0x0D)]),
    (24, 'map_obj5g.dat', [(0x518,0x0B),(0x519,0x0C),(0x51A,0x0D),(0x51B,0x0E),(0x51C,0x0F)]),
    (25, 'map_obj5h.dat', [(0x51D,0x0B),(0x51E,0x0C)]),
]
# scf_check_treasure / markTreasureInit mask (main+0x4C228C / +0x4BF60C): areas whose own no=8
# treasure marks native draws; 22 (5e) and 25 (5h) never draw theirs.
SW_TREASURE_MASK = 0x01BE7A5E
SW_TREASURE_COUNT = {18:4, 19:7, 20:3, 21:2, 23:2, 24:4}
sw_treasures = {}
for area_no, fname, table in SEPARATE_WAYS_AREAS:
    sw_buf, sw_entries = decoder.load_area(cmn_dir/fname)
    marks = (decoder.scan_marks(decoder.slice_entry(sw_buf, sw_entries[8-4]))
             if (SW_TREASURE_MASK >> area_no) & 1 else [])
    if len(marks) != SW_TREASURE_COUNT.get(area_no, 0):
        raise ValueError(f'{fname}: expected {SW_TREASURE_COUNT.get(area_no, 0)} SW treasure marks, got {len(marks)}')
    for room_id, no in table:
        key = f'stage5-{room_id:03x}'
        room = placed_room(sw_buf, sw_entries[no-4], room_id, key, fname)
        x0, x1, z0, z1 = build_overview({room_id: room}, [room_id], f'{key}.png')
        areas[key] = {'image':f'file:assets/{key}.png', 'min':[x0,-z1], 'max':[x1,-z0], 'icons':[]}
        sw_treasures[key] = marks

icon_cells={'treasure':{'r':0,'c':0},'medallion':{'r':0,'c':1},
            'save':{'r':0,'c':2},'goal':{'r':0,'c':3}}
if a.golden_dir:
    # Reuse the accepted native map arrow, including its native alpha and colour.
    arrow = Image.new('RGBA', (64, 64))
    art.player_arrow(arrow, 32, 32, 0)
    arrow.save(a.golden_dir/'leon-map-arrow.png',optimize=True,compress_level=9)
    art.tex('3a000009',2).resize((64,64),Image.Resampling.LANCZOS).save(a.golden_dir/'treasure-star.png',optimize=True,compress_level=9)
    # Native blue medallion art: imagepackhd/3a000009.pack.fen texture 23.
    atlas = Image.new('RGBA', (256, 64))
    icon_cells={'treasure':{'r':0,'c':0},'medallion':{'r':0,'c':1},
                'save':{'r':0,'c':2},'goal':{'r':0,'c':3}}
    atlas.alpha_composite(Image.open(a.golden_dir/'treasure-star.png').convert('RGBA'), (0, 0))
    (a.golden_dir/'treasure-star.png').unlink() # Atlas-only intermediate, recreated on each run.
    atlas.alpha_composite(art.tex('3a000009',23).resize((64,64),Image.Resampling.LANCZOS), (64, 0))
    # The native S pin's tip, rather than its centre, is at the projected save point.
    pin = art.tex('3a000009',20)
    # S is an ID sprite, not a light-mask-4 model: retain its full diffuse colour.
    pin = Image.fromarray(map_material(pin,diffuse=1))
    pin.thumbnail((32,32),Image.Resampling.LANCZOS)
    atlas.alpha_composite(pin,(128+(64-pin.width)//2,32-pin.height))
    atlas.alpha_composite(native_goal_sprite(art),(192,0))
legend_tables = prepare_marks(package,cmn_dir,decoder,art,areas,state_overlays,state_rooms,
                              common_buf,common_entries,include_stage4=a.include_stage4,golden_dir=a.golden_dir)
# SW treasure stars at their native placed positions, before S/goal in draw order like the
# campaign. Pan bounds grow to contain them; the initial camera keeps the room's own box.
for key, marks in sw_treasures.items():
    area = areas[key]
    for x, _, z in marks:
        area['min'] = [min(area['min'][0], x-120), min(area['min'][1], -z-120)]
        area['max'] = [max(area['max'][0], x+120), max(area['max'][1], -z+120)]
    area['icons'][0:0] = marker_icons(marks, 'treasure', (area['min'][0], area['max'][0],
        -area['max'][1], -area['min'][1]), key, lambda n:'re4.treasure.show')
if a.golden_dir:atlas.save(a.golden_dir/'map-markers.png',optimize=True,compress_level=9)
manifest_path = package/'manifest.json'
m = json.loads(manifest_path.read_text())
m['map'] = {'areas': areas, 'atlas':'module:re4/mark/atlas',
            'cell':64,'icons':icon_cells,
            'state_overlays':state_overlays}
manifest_path.write_text(json.dumps(m, indent=2)+'\n')
write_tables(root/'native/modules/re4_map_state_tables.h',legend_tables,state_rooms,multi_rooms,
             include_stage4=a.include_stage4)
bounds=['// SPDX-License-Identifier: GPL-3.0-or-later', '#pragma once', '#include <array>', '#include <algorithm>', '#include <optional>', '#include <string_view>', 'namespace re4 {', 'inline std::optional<std::array<double,4>> MapBounds(std::string_view area) {']
for key,area in areas.items():
    bounds.append(f'    if(area=="{key}")return std::array<double,4>{{{area["min"][0]}, {area["min"][1]}, {area["max"][0]}, {area["max"][1]}}};')
bounds += ['    return {};', '}', '}']
# Retain the approved initial Separate Ways framing as native marks extend pan bounds.
bounds[-1:-1] = ['inline std::optional<std::array<double,4>> MapCameraBounds(std::string_view area) {']
for key, box in legend_tables['camera_bounds'].items():
    if key.startswith('stage5-'):
        bounds[-1:-1] = [f'    if(area=="{key}")return std::array<double,4>{{'+', '.join(map(str,box))+'};']
bounds[-1:-1] = ['    return MapBounds(area);','}']
(root/'native/modules/re4_map_bounds.h').write_text('\n'.join(bounds)+'\n')

# Collision meshes are decoded from the player RomFS on the module asset worker.
# Source selectors and raster scale only; visual vertices/UVs/colours are read at runtime.
art_table = ['// SPDX-License-Identifier: GPL-3.0-or-later', '#pragma once',
             'namespace re4::assets {',
             'struct MapArtRoom { const char* key; const char* file; unsigned entry; double scale,ppu; };',
             'inline constexpr MapArtRoom MapArtRooms[] = {']
for key,filename,entry,model_scale,ppu in art_rows:
    art_table.append(f'    {{"{key.replace("-", "/")}", "{filename}", {entry}, {model_scale}, {ppu!r}}},')
art_table += ['};', '}']
(root/'native/modules/re4_map_art_tables.h').write_text('\n'.join(art_table)+'\n')
