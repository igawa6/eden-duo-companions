#!/usr/bin/env python3
"""Bake Metroid Dread's water-pool table into the package manifest (map.areas[<area>].water_pools).

Per area, from the game's own romfs:
  * every mapWaterPoolGeos record of <area>.bmmap -- the quad the area map draws, which is the
    pool's FULL-level box (min/max of the record's vertices; equals the CWaterPoolComponent bounds
    the game saves in <scenario>:WATER_VOLUMES at level 1.0),
  * that actor's CWaterPoolComponent.vWaterLevelChanges from <area>.brfld -- the level fractions
    the pool steps through; <actor>:WATERPOOL:ChangeIdx (blackboard) indexes it, absent == 0,
    an empty list == always full,
  * every CWaterTriggerChangeComponent (origin pool, target pool, fChangeTime, fDelay) -- the
    drains/fills that step those indices at run time.

Entry: {"n": actor, "b": [minX, minY, maxX, maxY], "lv": [levels...]}
Trigger: {"n": actor, "o": origin pool, "t": target pool, "s": fChangeTime, "d": fDelay}
The level-L box is [minX, minY, maxX, minY + L*(maxY-minY)] (verified against saved
WATER_VOLUMES: PRP_CV_watercave08a at 0.48936 -> top -2725.002).

Idempotent. Run with the Mercury venv:
  tools/venv/bin/python3 dread_water_table.py [package_dualscreen_dir] [--check]
"""
import os
import json
import sys
from pathlib import Path

from mercury_engine_data_structures.file_tree_editor import FileTreeEditor
from mercury_engine_data_structures.game_check import Game
from mercury_engine_data_structures.romfs import ExtractedRomFs

ROMFS = Path(os.environ.get("DREAD_ROMFS", "romfs"))  # extracted romfs (contains packs/, system/, textures/)
PKG = Path(os.environ.get("DREAD_LEGACY_PKG", "legacy/dualscreen"))  # an asset-carrying (pre-1.0.0) package dir; never the release package
AREAS = ['s010_cave', 's020_magma', 's030_baselab', 's040_aqua', 's050_forest',
         's060_quarantine', 's070_basesanc', 's080_shipyard', 's090_skybase']


def f32(v):
    # Keep the game's float32 values exactly (json float of the widened double).
    return float(v)


def actor_name(link):
    # "Root:pScenario:rEntitiesLayer:dctSublayers:<layer>:dctActors:<actor>" -> "<actor>"
    s = str(link)
    return '' if s in ('', '{EMPTY}') else s.rsplit(':', 1)[-1]


def extract(ed, area):
    base = f'maps/levels/c10_samus/{area}/{area}'
    geos = ed.get_parsed_asset(base + '.bmmap').raw['Root'].get('mapWaterPoolGeos') or {}
    layers = ed.get_parsed_asset(base + '.brfld').raw['Root']['pScenario']['rEntitiesLayer'][
        'dctSublayers']
    levels, triggers = {}, []
    for _, layer in layers.items():
        for aname, actor in layer['dctActors'].items():
            for _, comp in (actor.get('pComponents') or {}).items():
                t = comp.get('@type', '')
                if t == 'CWaterPoolComponent':
                    levels[str(aname)] = [f32(x) for x in comp.get('vWaterLevelChanges', [])]
                elif t == 'CWaterTriggerChangeComponent':
                    o = actor_name(comp.get('wpOriginWaterTrigger', ''))
                    tg = actor_name(comp.get('wpTargetWaterTrigger', ''))
                    if o or tg:
                        triggers.append({'n': str(aname), 'o': o, 't': tg,
                                         's': f32(comp.get('fChangeTime', 0.0)),
                                         'd': f32(comp.get('fDelay', 0.0))})
    pools = []
    for name, g in geos.items():
        vs = [(f32(p[0]), f32(p[1])) for p in g['aVertex']]
        if not vs:
            continue
        box = [min(x for x, _ in vs), min(y for _, y in vs),
               max(x for x, _ in vs), max(y for _, y in vs)]
        pools.append({'n': str(name), 'b': box, 'lv': levels.get(str(name), [])})
    pools.sort(key=lambda p: p['n'])
    triggers.sort(key=lambda t: t['n'])
    return pools, triggers


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    pkg = Path(args[0]) if args else PKG
    check = '--check' in sys.argv
    ed = FileTreeEditor(ExtractedRomFs(ROMFS), Game.DREAD)
    table = {a: extract(ed, a) for a in AREAS}
    path = pkg / 'manifest.json'
    d = json.load(open(path))
    changed = 0
    for area, v in d['map']['areas'].items():
        pools, triggers = table.get(area, ([], []))
        want = {'water_pools': pools, 'water_changes': triggers}
        for k, val in want.items():
            if not val:
                if k in v:
                    del v[k]
                    changed += 1
            elif v.get(k) != val:
                v[k] = val
                changed += 1
    for area, (pools, triggers) in table.items():
        multi = sum(1 for p in pools if len(p['lv']) > 1)
        print(f'{area}: {len(pools)} pools ({multi} with level changes), {len(triggers)} triggers')
    if check:
        print('manifest up to date' if changed == 0 else f'{changed} field(s) out of date')
        sys.exit(1 if changed else 0)
    if changed:
        with open(path, 'w') as f:
            json.dump(d, f, indent=1)  # the package's committed layout
            f.write('\n')
    print(f'updated {changed} field(s) in {path}')


if __name__ == '__main__':
    main()
