#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit a regenerated asset tree. Unknown names refuse classification.

Game geometry and glyphs count as game-derived even when rasterized by code.
Transparent overview placeholders have no geometry or pixels from the game.
"""
import argparse
import collections
import hashlib
import json
import re
from pathlib import Path


def classify(name):
    if re.fullmatch(r'r5-(badge-[1-5]|case-select-(?:\d+-)?\d+x\d+|case-ammo-badge|slot-edit-[0-3]|picker-highlight)\.png', name):
        return 'code-drawn', 'prepare_r5.py: ImageDraw fills and outlines only'
    if re.fullmatch(r'(?:village|castle|island|stage4)(?:-overview|-[0-9a-f]{3})?\.png|stage5-[0-9a-f]{3}\.png', name):
        return 'code-drawn', 'prepare_map.py build_overview: transparent 1x1 placeholder, no geometry'
    if name.startswith(('map-room-', 'map-door-', 'map-mark-', 'map-save-', 'map-goal-')):
        return 'game-derived', 'prepare_map.py/map_legend.py: native visual models, materials, marks or glyphs'
    if name in ('leon-map-arrow.png', 'map-markers.png', 'treasure-star.png'):
        return 'game-derived', 'prepare_map.py: native sprites and marker atlas'
    if re.fullmatch(r'(weapon|case-art|slot-art|slot-missing)-[0-9a-f]{2}\.png', name):
        return 'game-derived', 'prepare_weapon_art.py/prepare_r5.py: native BIN geometry and 3b000001 textures'
    if re.fullmatch(r'key-art-[0-9a-f]+\.png', name):
        return 'game-derived', 'prepare_r5.py: 39000009 native item sprites'
    if name in ('font.png', 'font.bin'):
        return 'game-derived', 'prepare_assets.py: 0f000001 glyph pixels or glyph-derived metrics'
    if name=='standby.png' or name.startswith('r5-') and name.endswith('.png'):
        return 'game-derived', 'prepare_r5.py + approved art helper: native textures, glyphs or collage'
    raise ValueError(f'Unclassified asset: {name}')


def sources(node):
    if isinstance(node, str) and node.startswith(('file:assets/', 'module:')):
        yield node
    elif isinstance(node, dict):
        for key, value in node.items():
            yield from sources(key)
            yield from sources(value)
    elif isinstance(node, list):
        for value in node:
            yield from sources(value)


def audit(package):
    files={}
    for path in sorted((package/'assets').iterdir()):
        if not path.is_file():
            raise ValueError(f'Unexpected directory: {path.name}')
        kind, source=classify(path.name)
        files['assets/'+path.name]=dict(classification=kind, source=source,
            bytes=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    refs={}
    for src in sorted(set(sources(json.loads((package/'manifest.json').read_text())))):
        if src.startswith('module:'):
            refs[src]=dict(classification='runtime', source='player RomFS')
        else:
            name=src.removeprefix('file:')
            if name not in files:
                raise ValueError(f'Missing manifest source: {src}')
            refs[src]=dict(classification=files[name]['classification'], source=files[name]['source'])
    counts=collections.Counter(f['classification'] for f in files.values())
    return dict(format=1, files=files, manifest_sources=refs, counts=dict(counts),
        intermediates={name:dict(classification=classify(name)[0],source=classify(name)[1])
                       for name in ('standby.png','treasure-star.png')})


def write_provenance(package):
    result=audit(package)
    provenance=dict(format=1,files={name:dict(origin='generated',source=row['source'])
        for name,row in result['files'].items() if row['classification']=='code-drawn'})
    provenance['runtime_sources']={key:dict(origin='romfs',source='player RomFS; native BIN geometry and texture packs; generated composition instructions')
        for key in result['manifest_sources'] if key.startswith('module:re4/')}
    provenance['runtime_key_families']=['module:re4/room/<stage>/<room>/<state>[/f<floor>]',
        'module:re4/door/<angle>/<state>', 'module:re4/mark/<name>',
        'module:re4/bg/<piece>', 'module:re4/hp/base', 'module:re4/hp/wedge/<1..14>',
        'module:re4/map/frame/<0..3>', 'module:re4/map/backing', 'module:re4/map/unmapped', 'module:re4/map/frame', 'module:re4/map/legend', 'module:re4/map/label/<stage>']
    provenance['runtime_key_families'] += ['module:re4/case/<id>/120','module:re4/case/background/<columns>', 'module:re4/case/name/<id>', 'module:re4/weapon/<id>', 'module:re4/slot/<id>[/missing]', 'module:re4/icon/<id>', 'module:re4/digit/<0..9>', 'module:re4/ui/<name>']
    provenance['runtime_key_families'] += ['module:re4/font/atlas', 'module:re4/font/atlas/0']
    provenance['font'] = dict(metrics='romfs:bio4/font/common_p.fnt', atlas='module:re4/font/atlas/{p}', source='0f000001 texture; worker-derived pixel bounds and advances; immutable module cache')
    provenance['deferred_game_files'] = {}
    provenance['composition_constants'] = 'Only code-drawn fills, masks, outlines and shadows; native textures are read from player RomFS at runtime'
    (package/'ASSETS.provenance.json').write_text(json.dumps(provenance,indent=1)+'\n')
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('package',type=Path)
    p.add_argument('--audit',type=Path)
    a=p.parse_args()
    result=write_provenance(a.package)
    if a.audit:a.audit.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result['counts'],sort_keys=True))
