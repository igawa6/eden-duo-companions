#!/usr/bin/env python3
"""Synthetic package/geometry contract: no game files or emulator."""
import json
import unittest
from gen_manifest import manifest, ROOT, BUILD, WHOLE_ZOOM

class PackageTest(unittest.TestCase):
    def setUp(self):
        self.raw=json.loads((ROOT/'packages/PokemonLegendsArceus/dualscreen/manifest.json').read_text())
        # Geometry/action contracts read the haptics-on set; test_haptic_twins checks the pairs.
        self.m=json.loads(json.dumps(self.raw))
        for page in self.m['pages']:
            page['widgets']=[w for w in page['widgets'] if w.get('haptic')!='off']
        self.widgets=self.m['pages'][0]['widgets']

    def test_generated(self):
        self.assertEqual(self.raw,manifest())

    def test_haptic_twins(self):
        h=self.raw['haptics']
        self.assertTrue(h['enabled'])
        self.assertTrue(all(v=='off' for k,v in h.items() if k not in ('enabled','respect_system')))
        self.assertIn('pla.haptics',self.raw['persist_flags'])
        derived={d['name']:d['expr'] for d in self.raw['derived']}
        for page in self.raw['pages']:
            touch=[w for w in page['widgets'] if 'on_tap' in w]
            on=[w for w in touch if w.get('haptic') in ('light','click','confirm')]
            off=[w for w in touch if w.get('haptic')=='off']
            self.assertEqual(len(on),len(off))
            self.assertEqual(len(on)+len(off),len(touch))
            for a,b in zip(on,off):
                self.assertEqual({k:v for k,v in a.items() if k not in ('haptic','hide_bind','keep_min','keep_max')},
                                 {k:v for k,v in b.items() if k not in ('haptic','hide_bind','keep_min','keep_max')})
                for w,value in ((a,1),(b,0)):
                    gate=w['hide_bind']
                    if gate=='@flag:pla.haptics':
                        self.assertEqual(w['keep_min'],value)
                    else:
                        self.assertTrue(derived[gate].startswith('@flag:pla.haptics' if value else '!@flag:pla.haptics'))

    def test_unowned_position_hides_map_and_shows_unavailable(self):
        maps=[w for w in self.widgets if w['type']=='map']
        self.assertTrue(all(w['need_bind']=='pla.map_valid' for w in maps))
        notice=next(w for w in self.widgets if w.get('src','').endswith(':Unavailable'))
        # Title and loading have their own views; the map notice covers only other failures.
        self.assertEqual(notice['need_bind'],'pla.map_unavailable')

    def test_composed_pokemon_images_preserve_native_colors(self):
        self.assertEqual(self.m['preload'],['module:pla:pokemon_static',
            'module:pla:pokedex_unavailable','module:pla:missions_unavailable:0',
            'module:pla:missions_unavailable:1','module:pla:settings_page',
            'module:pla:toggle:0','module:pla:toggle:1','module:pla:title_logo','module:pla:load_emblem','module:pla:recentre','module:pla:settings_emblem',
            'module:pla:control2:mis_active:0','module:pla:control2:mis_active:1',
            'module:pla:control2:dex_sort:0','module:pla:control2:dex_sort:1',
            'module:pla:control2:dex_unrep:0','module:pla:control2:dex_unrep:1',
            'module:pla:bag_static','module:pla:bag_cursor','module:pla:bag_capacity',
            'module:pla:bag_filter:0','module:pla:bag_filter:1',
            'module:pla:bag_tabs:0','module:pla:bag_tabs:1','module:pla:bag_tabs:2','module:pla:warm:dex','module:pla:warm:battle','module:pla:warm:missions'])
        page=next(p for p in self.m['pages'] if p['id']=='pokemon')
        images={w.get('src_bind'):w for w in page['widgets'] if w['type']=='image'}
        for name in ('pla.pokemon.image','pla.pokemon.weak_image'):
            self.assertEqual(images[name]['tint'],'#FFFFFFFF')

    def test_rows_do_not_hold_but_pages_do(self):
        for page in self.m['pages']:
            for widget in page['widgets']:
                if widget['type']!='image' or 'src_bind' not in widget:
                    continue
                bind=widget['src_bind']
                row='{i}' in bind or bind.startswith(('pla.party.','pla.item.')) or bind=='pla.hero'
                if row:
                    self.assertIs(widget['src_hold'],False,widget)
                else:
                    self.assertNotIn('src_hold',widget,widget)  # runtime default: hold

    def test_dense_hit_rects_have_12px_gaps(self):
        # LEGIBILITY dense cells (steward spot-check M4): neighbouring row/tab hits >= 12 px apart.
        for page in self.m['pages']:
            groups={}
            for w in page['widgets']:
                if w['type']=='button' and w.get('on_tap') in {'rec_row','dex_row','mis_row','battle_move','dex_tab',
                        'bag_everyday','bag_key','bag_recipes','pokemon_stats','pokemon_moves','pokemon_weak',
                        'missions_category_0','missions_category_1'}:
                    key=w['on_tap'].rstrip('_01') if w['on_tap'].startswith('missions_category') else w['on_tap']
                    key='bag_tab' if key.startswith('bag_') else 'pokemon_tab' if key.startswith('pokemon_') else key
                    groups.setdefault((key,json.dumps({k:v for k,v in w.items() if k in ('hide_bind','keep_min')})),[]).append(w['rect'])
            for (key,_),rects in groups.items():
                rects=sorted(rects)
                for a,b in zip(rects,rects[1:]):
                    gap=max(b[0]-(a[0]+a[2]), b[1]-(a[1]+a[3]))
                    self.assertGreaterEqual(gap,12,(page['id'],key,a,b))

    def test_geometry_and_actions(self):
        self.assertEqual((self.m['canvas_w'],self.m['canvas_h']),(1240,1080))
        self.assertEqual([p['id'] for p in self.m['pages']],['map','pokemon','pokedex','satchel','missions','settings','battle','version'])
        for page in self.m['pages']:
            hits=[]
            for w in page['widgets']:
                x,y,width,height=w['rect']
                if w.get('scroll') in ('pla_weak','pla_dex_body'):
                    self.assertEqual(w['rect'],[480,522,596,1600])
                elif w.get('scroll')=='pla_recipe_body':
                    self.assertEqual(w['rect'],[164,812,928,1600])
                else:
                    self.assertTrue(x>=0 and y>=0 and x+width<=1240 and y+height<=1080,w)
                if 'on_tap' in w:
                    self.assertGreaterEqual(width,96 if w.get('id')=='pla_item_hits' else 112);self.assertGreaterEqual(height,88 if page['id'] in ('satchel','pokemon','pokedex','missions','battle') else 112)
                    self.assertIn(w['on_tap'],self.m['actions'])
                    # Recipe rows exist only while the Recipes tab is shown; the Everyday/Key list never is then.
                    hits.append((w['rect'],w['on_tap']=='rec_row'))
            for a,ra in hits:
                for b,rb in hits:
                    if a==b or ra!=rb:continue # mutually exclusive controls
                    ax,ay,aw,ah=a;bx,by,bw,bh=b
                    self.assertTrue(ax+aw<=bx or bx+bw<=ax or ay+ah<=by or by+bh<=ay,(page['id'],a,b))
        self.assertTrue(all(a['kind'] in ['page','flag','view_reset','module'] for a in self.m['actions'].values()))
        maps=[w for w in self.widgets if w['type']=='map']
        self.assertEqual(len(maps),1)
        for w in maps:
            self.assertEqual(w['rect'],[12,12,1080,740]);self.assertEqual(w['need_bind'],'pla.map_valid')
            self.assertTrue(w['pan_zoom'])

    def test_dock_taps_and_scrolls(self):
        cells=next(w for w in self.widgets if w.get('id')=='pla_item_cells')
        self.assertEqual(cells['rect'],[322,764,122,124]);self.assertEqual(cells['repeat_dx'],108)
        self.assertEqual(cells['repeat'],64);self.assertEqual(cells['scroll'],'pla_items')
        scroll=self.m['pages'][0]['scrolls'][0]
        self.assertEqual(scroll['axes'],'x');self.assertEqual(scroll['rect'],[322,764,770,124])
        self.assertEqual(scroll['content_w_bind'],'pla.item_width')
        hits=next(w for w in self.widgets if w.get('id')=='pla_item_hits')
        self.assertEqual(hits['rect'],[348,764,96,124])
        self.assertEqual(hits['on_tap'],'select_item');self.assertEqual(hits['payload'],'{i}')
        taps=[w for w in self.widgets if w.get('on_tap')=='select_party']
        self.assertEqual([w['payload'] for w in taps],list(map(str,range(6))))
        self.assertTrue(all(w['rect'][2:]==[170,168] for w in taps))
        self.assertTrue(self.m['module_tick_hidden'])
        self.assertTrue(self.m['flags']['pla.tap_select'])
        self.assertIn('pla.tap_select',self.m['persist_flags'])
        pending=[w for w in self.widgets if w.get('need_bind','').endswith('.pending')]
        self.assertEqual(len(pending),7)
        self.assertEqual(pending[0]['rect'],[372,774,48,6])
        self.assertEqual(pending[0]['bg'],'#FF334D67')
        self.assertEqual(pending[1]['rect'],[16,900,64,6])
        self.assertEqual(pending[1]['keep_max'],0)
        for a,b in zip(taps,taps[1:]):
            self.assertGreaterEqual(b['rect'][0]-(a['rect'][0]+a['rect'][2]),12)
        self.assertEqual(self.m['load_plan'],'guest/<build>/load-plan.json')
        fade=next(w for w in self.widgets if w.get('src')=='module:pla:edge')
        self.assertEqual(fade['rect'],[1068,774,24,84]) # excludes numeral baseline
        cards=[w for w in self.widgets if w.get('src_bind','').endswith('.card')]
        self.assertEqual(len(cards),6)
        self.assertEqual([w['rect'] for w in cards],[[12+i*182,900,170,168] for i in range(6)])
        self.assertTrue(all(w['tint']=='#FFFFFFFF' for w in cards))
        cursor=next(w for w in self.widgets if w.get('need_bind')=='pla.party_cursor_valid')
        self.assertEqual(cursor['x_bind'],'pla.party_cursor_x')
        self.assertNotIn('on_tap',cursor)

    def test_satchel_read_only(self):
        page=next(p for p in self.m['pages'] if p['id']=='satchel')
        row=next(w for w in page['widgets'] if w.get('scroll')=='pla_bag')
        self.assertEqual(row['repeat_cols'],2)
        self.assertEqual(row['repeat_row_dy'],100)
        self.assertNotIn('repeat_dy',row)
        self.assertEqual(row['payload'],'$pla.bag.{i}.id')
        self.assertEqual(row['on_tap'],'bag_inspect')
        self.assertEqual(self.m['actions']['bag_inspect']['kind'],'flag')
        self.assertEqual(self.m['actions']['bag_everyday']['value'],0)
        self.assertEqual(self.m['actions']['bag_key']['value'],1)
        self.assertEqual(page['scrolls'][0]['rect'],[0,112,1088,688])
        self.assertEqual(page['scrolls'][0]['reset_bind'],'@flag:pla.bag_key')
        self.assertEqual(page['scrolls'][0]['bar_w'],6)
        self.assertEqual(self.m['actions']['bag_recipes'],dict(kind='flag',flag='pla.bag_key',value=2))
        recipes=next(w for w in page['widgets'] if w.get('src_bind')=='pla.recipes.image')
        self.assertEqual((recipes['hide_bind'],recipes['keep_min'],recipes['keep_max']),('@flag:pla.bag_key',2,2))
        self.assertEqual(len([w for w in page['widgets'] if w.get('on_tap')=='rec_row']),5)
        self.assertTrue(all(self.m['actions'][a]['kind']=='module' for a in ('rec_row','rec_scroll_up','rec_scroll_down')))
        cursor=next(w for w in page['widgets'] if w.get('src')=='module:pla:bag_cursor')
        self.assertEqual(cursor['need_bind'],'pla.bag_cursor_valid')
        self.assertEqual(cursor['repeat'],1) # runtime25 applies ScrollShift in repeat expansion.
        self.assertNotIn('on_tap',cursor)
        capacity=next(w for w in page['widgets'] if w.get('bind_text')=='pla.bag_capacity')
        self.assertEqual(capacity['need_bind'],'pla.bag_capacity_valid')
        self.assertEqual(capacity['rect'][0],1172) # DrawTextAligned centres on x, not x+w/2.

    def test_pokemon_inspection_only(self):
        page=next(p for p in self.m['pages'] if p['id']=='pokemon')
        for widget in page['widgets']:
            action=widget.get('on_tap','')
            if not action or action.startswith('page_'): continue
            self.assertTrue(action.startswith('pokemon_'))
            self.assertEqual(self.m['actions'][action]['kind'],'flag')
        rows=[w for w in page['widgets'] if w.get('on_tap')=='pokemon_inspect']
        self.assertEqual([w['payload'] for w in rows],list(map(str,range(6))))
        self.assertEqual(rows[0]['rect'],[36,12,416,164])
        moves=[w for w in page['widgets'] if w.get('on_tap')=='pokemon_move']
        self.assertEqual(len(moves),4)
        self.assertTrue(all(w['rect'][3]>=88 for w in moves))
        self.assertTrue(all(b['rect'][1]-(a['rect'][1]+a['rect'][3])>=12 for a,b in zip(moves,moves[1:])))
        self.assertTrue(all(w['need_bind']=='pla.pokemon.moves' for w in moves))
        self.assertFalse(any(w.get('enabled_bind')=='pla.tap_ready' for w in page['widgets']))
        self.assertTrue(all(self.m['flags']['pla.pokemon_'+k]==0 for k in ('slot','tab','move')))

    def test_pokedex_page_is_live_and_read_only(self):
        page=next(p for p in self.m['pages'] if p['id']=='pokedex')
        content=next(w for w in page['widgets'] if w['type']=='image' and w['rect']==[0,0,1104,1080])
        self.assertEqual(content['src_bind'],'pla.pokedex.image')
        rows=[w for w in page['widgets'] if w.get('on_tap')=='dex_row']
        self.assertEqual(len(rows),9)
        self.assertTrue(all(b['rect'][1]>=a['rect'][1]+a['rect'][3] for a,b in zip(rows,rows[1:])))
        for w in page['widgets']:
            for key in ('on_tap','on_swipe_up','on_swipe_down'):
                if not w.get(key):continue
                action=self.m['actions'][w[key]]
                self.assertTrue(action['kind'] in ('page','flag') or action['action'].startswith('dex_'))

    def test_missions_page_is_live_and_read_only(self):
        page=next(p for p in self.m['pages'] if p['id']=='missions')
        content=next(w for w in page['widgets'] if w['type']=='image' and w['rect']==[0,0,1104,1080])
        self.assertEqual(content['src_bind'],'pla.missions.image')
        self.assertEqual(len([w for w in page['widgets'] if w.get('on_tap')=='mis_row']),8)
        for w in page['widgets']:
            for key in ('on_tap','on_swipe_up','on_swipe_down'):
                if not w.get(key):continue
                action=self.m['actions'][w[key]]
                self.assertTrue(action['kind'] in ('page','flag') or action['action'].startswith('mis_'))
        self.assertEqual(self.m['actions']['missions_category_0']['argument'],0)
        self.assertEqual(self.m['actions']['missions_category_1']['argument'],1)

    def test_zoom_and_area(self):
        # Owner 2026-11-27: one pinch/drag map whose home follows the player; one reset button.
        maps=[w for w in self.widgets if w['type']=='map']
        self.assertEqual([m['id'] for m in maps],['pla_map'])
        self.assertTrue(maps[0]['pan_zoom'] and maps[0]['view_idle_ms']==0)
        self.assertEqual(maps[0]['view_rect_x0_bind'],'pla.view_x0')
        self.assertEqual(self.m['actions']['map_reset'],dict(kind='view_reset',view='pla_map',enabled_bind='pla.map_valid'))
        controls=[w for w in self.widgets if w.get('on_tap','').startswith(('map_reset','follow'))]
        self.assertEqual(set(tuple(w['rect']) for w in controls),{(1110,768,124,112),(1110,892,124,112)})
        # The pulse ring was dropped: it never drew live and kept the map redrawing.
        self.assertNotIn('dynamic_markers',self.m['map']['areas']['0'])
        self.assertEqual(self.m['actions']['area_next']['cycle'],7)
        self.assertEqual(self.m['flags']['pla.area_override'],0)
        self.assertEqual(set(self.m['map']['areas']),set('012345'))
        self.assertIn('pla.area_override',self.m['persist_flags'])

    def test_native_sources_and_build(self):
        self.assertEqual(len(BUILD),64);self.assertEqual(self.m['module']['build_ids'],[BUILD[:16]])
        self.assertEqual(self.m['min_runtime'],25)
        for page in self.m['pages'][:2]:
            tabs=[w for w in page['widgets'] if w.get('on_tap','').startswith('page_')]
            # Each tab has a faded and a Reduced-motion (instant) twin.
            self.assertEqual([w['rect'][1] for w in tabs],[y for y in [12,136,260,384,508,632] for _ in (0,1)])
            self.assertTrue(all(w['on_tap'].endswith('_still')==bool(n%2) for n,w in enumerate(tabs)))
        self.assertEqual(self.m['actions']['page_map']['transition'],'fade')
        self.assertEqual(self.m['actions']['page_map_still']['transition'],'none')
        self.assertIn('pla.reduce_motion',self.m['persist_flags'])
        self.assertTrue(all(w.get('marker_rotate_bind')=='pla.heading' for w in self.widgets if w['type']=='map'))
        self.assertTrue(any(w.get('src')=='module:pla:tab_icon:settings' for w in self.widgets))
        settings=next(p for p in self.m['pages'] if p['id']=='settings')['widgets']
        self.assertFalse(any(w.get('bind_text','').startswith('pla.raw.') for w in settings))
        self.assertEqual(sum(w.get('src','')=='module:pla:settings_page' for w in settings),1)
        self.assertEqual(sum(w.get('src','').startswith('module:pla:toggle:') for w in settings),8)
        self.assertFalse(any(w.get('on_tap') in ['tap_select','area_next'] for w in settings))
        for p in (ROOT/'packages/PokemonLegendsArceus').rglob('*'):
            if p.is_file():self.assertIn(p.suffix,['.json','.md'])

if __name__=='__main__':unittest.main()
