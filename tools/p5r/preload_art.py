#!/usr/bin/env python3
"""preload_art.py <dualscreen dir>: request the next page's art before that page opens.

The host builds a "module:p5r:<id>" image only when a draw first asks for it. It runs the P5R
module's builder on its own worker and takes at most 4 finished images per tick, so the ~40
images of the START MENU hub need about 10 ticks to arrive. A page transition draws its target
page once when it starts. After that it redraws the target only when a value changes, and an
image landing from the module does not count as a change (the landing also does not mark the
page stale after the fade ends). On the first hub open after loading a save, the tiles appeared
at full strength 7 frames into the fade, and 4 money digits and both commas never appeared, so
"9,893,651" showed as "51" until the next page change. Hub -> SKILL showed the same thing with
the HP/SP labels and the skill cost boxes.

Fix in the package: each page that can open a page also draws that page's art as invisible 1x1
widgets (rect [0,0,1,1], color #00000000, no gates), appended last. A full redraw of the host
page then asks the host for those images. They land during the seconds before the user taps,
and the host's image LRU keeps them warm while the host page is drawn. The hub then opens from
the cache and is complete in its first fade frame.

  field, battle, dialogue, social, analysis (MENU button, or the game's own menu) -> live
  live -> the 8 hub pages;  m_equip -> m_equipc;  m_confidant -> m_cfdetail

Preloaded: every fixed "module:p5r:" src of the target's image widgets, the distinct entries of
a src_names list with at most MAX_NAMES distinct images (money digits, party heads, arcana cards,
confidant portraits, calendar day glyphs), and the whole family of the menu's src_format icon
("module:p5r:i%d", 74 small icons). The music title list (107 images) is left to load when shown;
the title playing is already on screen in every page's bottom bar. Ids that the host page already
draws without a gate are skipped.

Memory: hub art ~16 MB on the game-state pages, the 8 hub pages ~43 MB on the hub. The host keeps
module images in a 64 MiB LRU; one page's full draw must stay below that, or a redraw would evict
and rebuild its own images in a loop, so main() refuses a page whose art passes BUDGET_MB. Cost
per full redraw: one cache lookup and a clipped 1x1 blit per widget (a partial redraw rejects them
by rect).
"""
import json
import re
import sys
from pathlib import Path

MAX_NAMES = 40
BUDGET_MB = 60  # the host keeps module images in a 64 MiB (67.1 MB) LRU
PREFIX = 'module:p5r:'
HUB_HOSTS = ['field', 'battle', 'dialogue', 'social', 'analysis']
# The hub's pages in tile order: the host loads the preloads in widget order, 4 per tick (~1.5 s
# for all of them), so the first tiles' pages are ready first.
HUB_TARGETS = ['m_skill', 'm_item', 'm_equip', 'm_persona', 'm_stats', 'm_confidant', 'm_request',
               'm_calendar']
PLAN = {**{h: ['live'] for h in HUB_HOSTS}, 'live': HUB_TARGETS, 'm_equip': ['m_equipc'],
        'm_confidant': ['m_cfdetail']}
GATES = ('need_bind', 'hide_bind', 'show_bind', 'bind', 'src_names', 'src_format', 'src_bind')


def fixed(src):
    return (isinstance(src, str) and src.startswith(PREFIX) and len(src) > len(PREFIX)
            and not re.search(r'[{%]', src))


def page_art(page, rec_ids):
    out = []
    for w in page['widgets']:
        if w.get('type') != 'image':
            continue
        names = w.get('src_names') or []
        fmt = w.get('src_format', '')
        if fmt.startswith(PREFIX) and fmt.count('%') == 1 and fmt.endswith('%d'):
            stem = fmt[len(PREFIX):-2]
            fam = sorted((r for r in rec_ids if re.fullmatch(re.escape(stem) + r'\d+', r)),
                         key=lambda r: int(r[len(stem):]))
            out += [PREFIX + r for r in fam]
        elif names:
            uniq = list(dict.fromkeys(s for s in names if fixed(s)))
            if len(uniq) <= MAX_NAMES:
                out += uniq
        elif fixed(w.get('src')):
            out.append(w['src'])
    return out


def art_mb(ids, table, cache):
    """MB of RGBA the host holds for these images (each recipe replayed with Pillow once)."""
    import recipe
    total = 0
    for s in ids:
        rid = s[len(PREFIX):]
        if rid not in cache:
            cache[rid] = recipe.replay(table.rows[rid][0]).size
        w, h = cache[rid]
        total += w * h * 4
    return total / 1e6


def main(dual):
    mp = Path(dual) / 'manifest.json'
    m = json.loads(mp.read_text())
    rec_ids = {l.split(' ', 1)[0] for l in (Path(dual) / 'p5r_art.rec').read_text().splitlines()[1:]
               if l and l[0] not in '@#'}
    pages = {p['id']: p for p in m['pages']}
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import recipe
    table = recipe.Table.load((Path(dual) / 'p5r_art.rec').read_text())
    sizes = {}
    report = []
    for host, targets in PLAN.items():
        hp = pages[host]
        drawn = {w['src'] for w in hp['widgets']
                 if w.get('type') == 'image' and not any(k in w for k in GATES) and 'src' in w}
        want = []
        for t in targets:
            want += page_art(pages[t], rec_ids)
        want = [s for s in dict.fromkeys(want) if s not in drawn and s[len(PREFIX):] in rec_ids]
        hp['widgets'] += [dict(type='image', src=s, rect=[0, 0, 1, 1], color='#00000000')
                          for s in want]
        mb = art_mb(dict.fromkeys(page_art(hp, rec_ids)), table, sizes)
        assert mb <= BUDGET_MB, f'{host}: {mb:.1f} MB of art per full draw (budget {BUDGET_MB})'
        report.append(f'{host}+{len(want)} ({mb:.0f} MB)')
    mp.write_text(json.dumps(m, ensure_ascii=False, separators=(',', ':')) + '\n')
    print('preload: ' + ', '.join(report))


if __name__ == '__main__':
    main(sys.argv[1])
