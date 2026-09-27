#!/usr/bin/env python3
"""map_merge_af.py <dualscreen dir> [map_gen_af out dir] -- module-mode map merge.

Runs map_gen_af.py (module mode) unless its output exists, then applies the same v0.7 map snippet
(composites, area patch, atlas, dynamic markers) to the manifest, with every "file:maps/<png>"
replaced by "module:p5r:<id>" and the map recipes appended to p5r_art.rec. Nothing is
written under maps/: the package stays asset-free.
"""
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
from p5style import RECIPE_FILE  # noqa: E402
from recipe import Table  # noqa: E402

dst = Path(sys.argv[1])
from p5r_paths import MAPGEN, PACKAGE  # noqa: E402
gen = Path(sys.argv[2]) if len(sys.argv) > 2 else MAPGEN
assert PACKAGE not in dst.resolve().parents, 'refusing to write the canonical package'
if not (gen / 'map_recipes.json').exists():
    subprocess.run([sys.executable, str(HERE / 'map_gen_af.py'), str(gen), 'module'], check=True)
s = json.loads((gen / 'snippet.json').read_text())
recipes = json.loads((gen / 'map_recipes.json').read_text())

tf = dst / RECIPE_FILE
table = Table.load(tf.read_text()) if tf.exists() else Table()
ids = {fn: table.add_recipe(rec, f'maps/{fn}') for fn, rec in sorted(recipes.items())}


def swap(v):
    if isinstance(v, str) and v.startswith('file:maps/'):
        return 'module:p5r:' + ids[v[10:]]
    if isinstance(v, dict):
        return {k: swap(x) for k, x in v.items()}
    if isinstance(v, list):
        return [swap(x) for x in v]
    return v


s = swap(s)
m = json.loads((dst / 'manifest.json').read_text())
m['composites'] = {k: v for k, v in m.get('composites', {}).items() if not k.startswith('rmap_')}
m['composites'].update(s['composites'])
m['map'].update(s['map'])
for name, patch in s['area_patch'].items():
    m['map']['areas'][name].update(patch)
for a in m['map']['areas'].values():
    a['dynamic_markers'] = s['dynamic_markers_all_areas']
for p in m['pages']:
    for w in p['widgets']:
        if w.get('type') == 'map':
            for k in s['map_widget_drop']:
                w.pop(k, None)
for o in ['map.overlay.ready', 'map.parts.count', 'map.poi.count', 'map.radar.ready', 'map.enemy.count',
          'map.fog.red', 'map.fog.dark', 'map.player.count']:
    if o not in m['module_outputs']:
        m['module_outputs'].append(o)
text = json.dumps(m, ensure_ascii=False, separators=(',', ':'))
assert 'file:maps/' not in text, 'a map file reference survived'
tf.parent.mkdir(parents=True, exist_ok=True)
tf.write_text(table.text())
(dst / 'manifest.json').write_text(text + '\n')
print('merged', len(s['composites']), 'composites;', len(ids), 'map recipes')
