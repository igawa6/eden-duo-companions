#!/usr/bin/env python3
"""Package contract checks; no emulator or game assets required."""
import json
import os
import re
import struct
import unittest
import importlib.util
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
PACKAGE=Path(os.environ.get('RE4_TEST_PACKAGE',ROOT/'packages/ResidentEvil4'))
M=json.loads((PACKAGE/'dualscreen/manifest.json').read_text())
BG=json.loads((ROOT/'tools/resident-evil-4/background_layout.json').read_text())
def bg_sources(key):
    return list(dict.fromkeys(e['src'] for e in BG['layouts']['module:re4/'+key]))

# Separate Ways native areas 18..25 (Switch mapDataInit / getAreaNo) and the native treasure
# marks each area draws (scf_check_treasure mask 0x01BE7A5E: 22 and 25 draw none).
SW_AREA={f'stage5-{r:03x}':a for a,rooms in {18:[0x500,0x501,0x502,0x503],
    19:[0x504,0x505,0x506,0x507,0x508,0x509,0x50a,0x50b],20:[0x50c,0x50d,0x50e,0x50f,0x510],
    21:[0x512,0x514],22:[0x513],23:[0x515,0x516,0x517],24:[0x518,0x519,0x51a,0x51b,0x51c],
    25:[0x51d,0x51e]}.items() for r in rooms}
SW_TREASURES={18:4,19:7,20:3,21:2,22:0,23:2,24:4,25:0}
def original_gate(widget):
    gate=widget.get('need_bind','')
    if gate.startswith('ui.font.'):
        expr=next(d['expr'] for d in M['derived'] if d['name']==gate)
        match=re.fullmatch(r"'re4.font.installed' && \((!?)'([^']+)'\)",expr)
        assert match,expr
        return match[1]+match[2]
    return gate

class PackageContract(unittest.TestCase):
    def test_packaged_file_allowlist(self):
        dualscreen=PACKAGE/'dualscreen'
        provenance=json.loads((dualscreen/'ASSETS.provenance.json').read_text())
        allowed=set(provenance['files'])|{'manifest.json','ASSETS.provenance.json'}
        build=json.loads((PACKAGE/'package.json').read_text())['module']['build_ids'][0]
        allowed.add(M['load_plan'].replace('<build>',build))
        for library in json.loads((PACKAGE/'package.json').read_text())['module'].get('libraries',{}).values():
            allowed.add(library['path'])
        actual={p.relative_to(dualscreen).as_posix() for p in dualscreen.rglob('*') if p.is_file()}
        self.assertEqual(actual,allowed,'Package must contain only declared assets and metadata/module payloads')

    def test_no_packaged_game_map_rasters(self):
        forbidden=re.compile(r'(?:map-|leon-map-arrow|r5-(?:map-frame|legend|(?:village|castle|island)-label))')
        names=[p.name for p in (PACKAGE/'dualscreen/assets').glob('*.png') if forbidden.match(p.stem)]
        self.assertEqual(names,[], 'Use a clean regenerated package; old extracted images must never ship')
        from provenance import sources
        refs=set(sources(M))
        self.assertFalse([s for s in refs if s.startswith('file:assets/') and forbidden.match(s.removeprefix('file:assets/'))])
        from PIL import Image
        provenance=json.loads((PACKAGE/'dualscreen/ASSETS.provenance.json').read_text())
        for area in M['map']['areas'].values():
            path=area['image'].removeprefix('file:')
            self.assertIn(path,provenance['files'])
            with Image.open(PACKAGE/'dualscreen'/path) as placeholder:
                self.assertEqual(placeholder.size,(1,1))
                self.assertEqual(placeholder.convert('RGBA').getpixel((0,0)),(0,0,0,0))
        labels=[w for p in M['pages'] for w in p['widgets'] if w.get('empty_src','').startswith('module:re4/map/')]
        self.assertTrue(labels)
        self.assertTrue(all(w.get('src_bind')=='re4.asset.blank' for w in labels))

    def test_inventory_ui_runtime_only(self):
        from provenance import classify
        assets=PACKAGE/'dualscreen/assets'
        game=[p.name for p in assets.iterdir() if classify(p.name)[0]=='game-derived']
        self.assertEqual(game,[])
        self.assertFalse((assets/'font.bin').exists())
        self.assertFalse((assets/'font.png').exists())
        self.assertEqual(json.loads((PACKAGE/'package.json').read_text())['min_runtime'],25)
        self.assertEqual(M['preload'][0],'module:re4/geometry/1/0')
        self.assertEqual(M['preload'][1:6],[*[f'module:re4/map/frame/{n}' for n in range(4)],'module:re4/mark/arrow'])
        self.assertTrue(all(not k.startswith(('module:re4/room/','module:re4/case/03','module:re4/weapon/')) for k in M['preload']))
        self.assertFalse(any(k.startswith('module:re4/case/background/') for k in M['preload']))
        field=next(p for p in M['pages'] if p['id']=='field')
        current=next(w for w in field['widgets'] if w.get('src_bind')=='re4.map.current.art')
        self.assertEqual(current['src_bind'],'re4.map.current.art')
        self.assertEqual(current['color'],'#00FFFFFF')
        self.assertFalse(current['src_hold'])
        standby=next(p for p in M['pages'] if p['id']=='standby')
        repair=[w for w in standby['widgets'] if w.get('color')=='#00FFFFFF']
        self.assertTrue(repair)
        self.assertEqual(repair[0]['src_bind'],'re4.geometry.art')
        self.assertEqual(repair[1]['src_bind'],'re4.map.warm.{i}.art')
        for page in M['pages']:
            for w in page['widgets']:
                if w['type']=='image' and (w.get('src_bind') or w.get('src','').startswith('module:re4/')):
                    self.assertIs(w.get('src_hold'),False,(page['id'],w))
        pickers=[p for p in M['pages'] if p['id'].startswith('picker_')]
        for i,page in enumerate(pickers):
            self.assertTrue(all(any(w.get('src')==key for w in page['widgets']) for key in bg_sources('ui/picker')))
            self.assertTrue(any(w.get('src')==f'module:re4/ui/picker-number-{i+1}' and w['rect'][1]==86 for w in page['widgets']))

    def test_field_preload_order_and_bound_requests(self):
        pre=M['preload']
        maps=['module:re4/geometry/1/0',*[f'module:re4/map/frame/{n}' for n in range(4)],'module:re4/mark/arrow',
              'module:re4/map/backing',*bg_sources('ui/field'),
              *[f'module:re4/map/label/{s}' for s in ('village','castle','island')],
              'module:re4/map/unmapped','module:re4/mark/atlas']
        self.assertEqual(pre[:len(maps)],maps)
        self.assertEqual(pre[len(maps):len(maps)+15],[entry['src'] for entry in BG['hp'].values()])
        self.assertEqual(len(pre),len(set(pre)))
        self.assertTrue(all(f'module:re4/digit/{n}' in pre for n in range(10)))
        for page in M['pages']:
            warm=[w for w in page['widgets'] if w.get('color')=='#00FFFFFF']
            self.assertEqual(warm[0]['src_bind'],'re4.geometry.art')
            self.assertNotIn('need_bind',warm[0])
            warm=warm[1:]
            self.assertEqual(warm[0]['src_bind'],'re4.map.warm.{i}.art')
            self.assertEqual(warm[0]['repeat_bind'],'re4.map.warm.count')
            self.assertEqual(warm[0]['need_bind'],'re4.map.warm.ready')
            binds=[w.get('src_bind') for w in warm]
            self.assertLess(binds.index('re4.slot.0.art'),binds.index('re4.picker.0.art'))
            self.assertTrue(all(w['rect']==[0,0,1,1] and w['src_hold'] is False for w in warm if w.get('src_bind')!='re4.map.current.art'))
            self.assertFalse(any(w.get('src') in pre for w in warm))

    def test_background_budget_and_canvas_parity(self):
        from backgrounds import FRAME_BOXES
        routes=('ui/field','ui/picker','ui/key-items','ui/settings',
                'case/background/15','ui/standby','ui/nav')
        keys={key for route in routes for key in bg_sources(route)}
        size=sum(BG['sizes'][key][0]*BG['sizes'][key][1]*4 for key in keys)
        self.assertLessEqual(size,20*1024*1024)
        all_keys={w['src'] for page in M['pages'] for w in page['widgets'] if w.get('src') in BG['sizes']}
        self.assertLessEqual(sum(BG['sizes'][key][0]*BG['sizes'][key][1]*4 for key in all_keys),32*1024*1024)
        binary=os.environ.get('RE4_TEST_BACKGROUND_BINARY')
        if binary:
            import tempfile
            from background_golden_test import check
            with tempfile.TemporaryDirectory(prefix='re4-background-parity-') as tmp:
                results=check(Path(binary),Path(tmp))
            self.assertTrue(all(v['changed_pixels']==0 for v in results.values()))

        for page in M['pages']:
            for w in page['widgets']:
                key=w.get('src',w.get('empty_src',''))
                if key.startswith('module:re4/bg/'):
                    self.assertIn(key,BG['sizes'])
                    self.assertFalse(w.get('src_hold',True))
                    self.assertNotIn('src_bind',w)
                self.assertNotEqual(w.get('src_bind'),'re4.case.art')
        # Canvas references are compact, disjoint partitions. Native fixture tests
        # reconstruct these partitions and compare every RGBA pixel with full keys.
        for key,entries in BG['layouts'].items():
            width,height=(1240,120) if key.endswith('/nav') else (1240,1080)
            rects=[e['rect'] for e in entries]
            for x,y,w,h in rects:
                self.assertTrue(0<=x<x+w<=width and 0<=y<y+h<=height)
            for i,(x,y,w,h) in enumerate(rects):
                for a,b,c,d in rects[:i]:
                    self.assertFalse(x<a+c and a<x+w and y<b+d and b<y+h)
        self.assertEqual(sum((r-x)*(b-y)*4 for x,y,r,b in FRAME_BOXES),1042068)

    def test_worker_font_contract(self):
        self.assertEqual(M['font'],'romfs:bio4/font/common_p.fnt')
        self.assertEqual(M['font_atlas'],'module:re4/font/atlas/{p}')
        self.assertEqual(M['font_page_h'],576)
        for page in M['pages']:
            for w in page['widgets']:
                if w.get('text') or w.get('bind_text') or w['type']=='value':
                    gate=w.get('need_bind','')
                    self.assertTrue(gate=='re4.font.installed' or gate.startswith('ui.font.'),(page['id'],w))
                    original_gate(w)
        self.assertEqual(json.loads((PACKAGE/'dualscreen/ASSETS.provenance.json').read_text())['deferred_game_files'],{})

    def test_native_map_binary_decoders(self):
        spec=importlib.util.spec_from_file_location('map_legend',ROOT/'tools/resident-evil-4/map_legend.py')
        legend=importlib.util.module_from_spec(spec);spec.loader.exec_module(legend)
        units=bytearray(8+2*0xa8);units[:4]=b'2.00';units[5]=2
        units[12]=1;units[8+0x58:8+0x5c]=bytes((135,135,135,255))
        units[8+0xa8+4]=17;units[8+0xa8+0x58:8+0xa8+0x5c]=bytes((255,0,0,255))
        self.assertEqual(legend.id_colours(units),{1:(135,135,135,255),17:(255,0,0,255)})
        with self.assertRaises(ValueError):legend.id_colours(units[:-1])
        # Native typewriter/goal parts can have nonsequential partsNo values.
        model=bytearray(0xb8);struct.pack_into('<Q',model,0,0x98);model[0x29]=2
        struct.pack_into('<I',model,0x5c,0x20030818)
        struct.pack_into('<BBBBfff',model,0x98,1,255,0,0,10,20,30)
        struct.pack_into('<BBBBfff',model,0xa8,3,255,0,0,-10,-20,-30)
        self.assertEqual(legend.part_positions(model),[(10,20,30),(-10,-20,-30)])
        model[0xa9]=0
        with self.assertRaises(ValueError):legend.part_positions(model)
        model[0xa9]=255;struct.pack_into('<f',model,0xac,float('nan'))
        with self.assertRaises(ValueError):legend.part_positions(model)
        box,variants=legend.room_images([(0,0,0),(100,0,0),(0,0,100)],[(0,1,2)],1,[(100,80,60)])
        self.assertLess(box[0],0);self.assertGreater(box[2],100)
        self.assertEqual(variants[0].mode,'P')
        self.assertEqual(variants[0].convert('RGBA').getpixel((14,14)),(100,80,60,225))
        with self.assertRaises(ValueError):legend.room_images([(0,0,0)],[(0,1,2)],1,[(100,80,60)])
        with self.assertRaises(ValueError):legend.room_images([(float('nan'),0,0)],[],1,[(100,80,60)])
        _,doors=legend.door_images([(0,0,0),(100,0,0),(0,0,100)],[(0,1,2)],90,[(0,255,0)])
        self.assertEqual(doors[0].getpalette()[3:6],[0,255,0])
        bounds={'min':[0,0],'max':[100,100]}
        legend.check_mark_bounds({'x':50,'y':50},bounds,'fixture')
        for x,y in ((101,50),(50,-1),(float('nan'),50),(0,50)):
            with self.assertRaises(ValueError):legend.check_mark_bounds({'x':x,'y':y},bounds,'fixture')
    def test_identity(self):
        meta=json.loads((PACKAGE/'package.json').read_text())
        self.assertEqual(meta['module'],M['module'])
        self.assertEqual(M['title_id'],'010099A00BC1E000')
        self.assertEqual(M['module']['build_ids'],['82C2A04A21D3E0B850F273D4149C9003'+'0'*32])
        self.assertEqual((M['canvas_w'],M['canvas_h'],M['min_runtime']),(1240,1080,25))
    def test_initial_page_is_standby(self):
        # Cold-boot fix (analysis/20261103-title-fix.md): mod_pages.cpp's DrivePageBinds
        # captures a bind's first valid read as a silent baseline and never fires an edge
        # on it, so pages[0] is exactly what a cold boot shows before re4.state's first
        # real transition. It used to be 'unavailable' (built from field() widgets), so a
        # cold boot showed the Field layout even though re4.state/re4.page already read
        # 1/"standby". prepare_r5.py now SWAPS 'standby' into index 0 instead of
        # reordering, so this is the only page whose index moved from the pre-fix layout.
        self.assertEqual(M['pages'][0]['id'],'standby')
        # Swap, not reorder: every other page keeps its original index -- a mid-game
        # manifest reload (which never resets current_page, only re-arms page_bind_state
        # cold) must not land a returning player on the wrong page by index shift. Prove
        # no page was dropped/duplicated and 'unavailable' (the former pages[0]) still
        # exists with its usual field()-built nav, just at a different slot.
        ids=[p['id'] for p in M['pages']]
        self.assertEqual(len(ids),len(set(ids)))
        self.assertIn('unavailable',ids)
        unavailable=next(p for p in M['pages'] if p['id']=='unavailable')
        self.assertTrue(any(w.get('on_tap')=='open_case' for w in unavailable['widgets']))
    def test_runtime_text_units(self):
        # Runtime text_scale is five-pixel units, not an atlas multiplier.
        # The first desktop render made text_scale=1 effectively invisible.
        for page in M['pages']:
            for w in page['widgets']:
                if w.get('text') or w.get('bind_text') or w['type']=='value':
                    self.assertGreaterEqual(w.get('text_scale',0),7)
    def test_field_ammo_strip(self):
        field=next(p for p in M['pages'] if p['id']=='field')
        ammo=next(w for w in field['widgets'] if w.get('bind_text')=='re4.ammo.reserve.text')
        self.assertEqual(original_gate(ammo),'re4.ammo.reserve.available')
        self.assertEqual(ammo['text_scale'],14)
        self.assertNotIn('fit_text',ammo)
        self.assertLessEqual(ammo['rect'][0]+ammo['rect'][2],757)
        money=next(w for w in field['widgets'] if w.get('bind')=='re4.money')
        self.assertLessEqual(money['rect'][0]+money['rect'][2],ammo['rect'][0])
        # Native glyph dimensions and reserve fit are verified by font_golden_test.py.

    def test_state_coverage(self):
        ids={p['id'] for p in M['pages']}
        self.assertEqual([p['equals'] for p in M['page_binds'] if p.get('point')=='re4.state'],list(range(34)))
        for b in M['page_binds']:
            self.assertTrue(b.get('point'))
            self.assertIn(b['when_equal']['page'],ids)
        self.assertEqual(M['page_binds'][31]['when_equal']['page'],'dimmed')
        # 33 = ShootingRange (merchant's range session/round): dimmed, never Field/Case.
        self.assertEqual({k:M['page_binds'][33][k] for k in ('point','equals')},{'point':'re4.state','equals':33})
        self.assertEqual(M['page_binds'][33]['when_equal']['page'],'dimmed')
    def test_local_actions_only(self):
        for action in M['actions'].values():self.assertIn(action['kind'],('page','flag','button','module'))
        for key in ('points','patches','symbols','enforce'):self.assertNotIn(key,M)
    def test_helper_identity_and_mailbox(self):
        build=M['module']['build_ids'][0]
        plan=json.loads((PACKAGE/'dualscreen'/M['load_plan'].replace('<build>',build)).read_text())
        self.assertEqual((plan['title_id'],plan['build_id']),(M['title_id'],build))
        self.assertEqual(plan['format'],2)
        self.assertEqual(plan['mailbox_size']%4096,0)
        self.assertGreaterEqual(plan['mailbox_size'],36)
        self.assertEqual(plan['mailbox_epoch_offset'],4)
        self.assertEqual(len(plan['patches']),1)
        self.assertEqual(plan['patches'][0]['offset'],0x1EA7C)
        self.assertEqual(plan['patches'][0]['expected'],'91060014')
        self.assertEqual(plan['patches'][0]['replacement'],'00000014')
    def test_settings_and_haptics(self):
        for key in ('hide_leon','hide_ashley','hide_ammo','haptics'):
            self.assertEqual(M['flags']['opt.'+key],1)
            self.assertIn('opt.'+key,M['persist_flags'])
        # PER-SAVE SLOT ISOLATION: a bounded, pre-declared set of buckets, not one global
        # flag (see native/modules/010099A00BC1E000.cpp SaveBucket()).
        self.assertNotIn('slots.assignment',M['persist_flags'])
        for bucket in range(4):
            self.assertIn(f'slots.assignment.{bucket}',M['persist_flags'])
            self.assertEqual(M['flags'][f'slots.assignment.{bucket}'],-1)
        self.assertIn('motion.reduced',M['persist_flags'])
        self.assertEqual(M['flags']['motion.reduced'],0)
        for page in M['pages']:
            for w in page['widgets']:
                for event in ('on_tap','on_hold','drop_action'):
                    if event in w:self.assertIn(w[event],M['actions'])
    def test_no_fixture_values(self):
        text=json.dumps(M)
        self.assertNotIn('1751510',text)
        # Item names 0x27/0xA2/0xB9 are decoded from the player's RomFS fixture, never kept here.
        rom=os.environ.get('RE4_TEST_ROMFS')
        core=Path(rom)/'bio4/etc/core.udas' if rom else None
        if core is not None and core.is_file():
            from prepare_item_names import decode
            names=decode(core.read_bytes())
            for item in (0x27,0xA2,0xB9):
                self.assertNotIn(names[item]['name'],text)
        # Every displayed game numeric reader is availability-gated.
        for page in M['pages']:
            for w in page['widgets']:
                if w.get('type')=='value' and w.get('bind','').startswith('re4.'):
                    self.assertEqual(original_gate(w),w['bind']+'.available')
    def test_native_ashley_toggle(self):
        action=M['actions']['ashley_toggle']
        self.assertEqual((action['kind'],action['button']),('button','R'))
        self.assertEqual(action['enabled_bind'],'re4.ashley_command.available')
        self.assertFalse(any(a['kind'] in ('call','sequence','write') for a in M['actions'].values()))
        for page in M['pages']:
            buttons=[w for w in page['widgets'] if w.get('on_tap')=='ashley_toggle']
            self.assertEqual(len(buttons),2 if page['id'] in ('field','unavailable','hud_only') else 0)
    def test_map_spoiler_setting(self):
        self.assertEqual(M['flags']['opt.map_spoilers'],0)
        self.assertEqual(M['persist_flags'].count('opt.map_spoilers'),1)
        self.assertEqual(M['actions']['toggle_map_spoilers'],{'kind':'flag','flag':'opt.map_spoilers'})
        settings=next(p for p in M['pages'] if p['id']=='settings')
        widgets=settings['widgets']
        self.assertTrue(any(w.get('on_tap')=='toggle_map_spoilers' for w in widgets))
        self.assertTrue(any(w.get('bind')=='@flag:opt.map_spoilers' for w in widgets))
        self.assertTrue(any(w.get('text','').startswith('Spoilers:') for w in widgets))

    def test_map_contract(self):
        areas=M['map']['areas']
        rooms=list(range(0x100,0x110))+[0x117,0x11c,0x11d,0x11e,0x11f,0x200]
        # Castle/island rooms from CASTLE_AREAS/ISLAND_AREAS (deduplicated).
        # Mercenaries data is development-only and must not ship.
        # Stage 5: the Switch-native mapDataInit areas 18..25 (analysis/20261114-gallery-sw.md B1).
        # r200 spans the story boundary. Native SCF_ST2_IN selects these existing
        # distinct keys; both images must remain available to that dispatch.
        self.assertEqual(areas['village-200']['image'], 'file:assets/village-overview.png')
        self.assertEqual(areas['castle-200']['image'], 'file:assets/castle-overview.png')
        castle_rooms=[0x200,0x201,0x202,0x203,0x204,0x205,0x206,0x207,0x208,0x209,0x20a,
            0x20b,0x20c,0x20d,0x20e,0x20f,0x210,0x211,0x212,0x213,0x214,0x215,0x216,0x217,
            0x218,0x219,0x21a,0x21b,0x21d,0x220,0x221,0x222,0x223,0x224,0x225,0x226,0x227,
            0x228,0x229,0x22a,0x814]
        island_rooms=[0x300,0x301,0x303,0x304,0x305,0x306,0x307,0x308,0x309,0x30a,0x30b,
            0x30c,0x30d,0x30e,0x30f,0x310,0x311,0x312,0x315,0x316,0x317,0x318,0x31a,0x31b,
            0x31c,0x31d,0x320,0x321,0x325,0x326,0x327,0x329,0x330,0x331,0x332,0x333]
        self.assertEqual(len(castle_rooms),41);self.assertEqual(len(island_rooms),36)
        self.assertFalse(any(k.startswith('stage4-') for k in areas))
        self.assertFalse(any('stage4' in k for k in M['sprite_map']))
        for header in ('re4_map_bounds.h','re4_map_state_tables.h'):
            self.assertNotIn('stage4', (ROOT/'native/modules'/header).read_text())
        import re
        tables=(ROOT/'native/modules/re4_map_state_tables.h').read_text()
        for array in ('MapRoomRules','MapDoorDefs','MapMarkDefs','MapGoalFlags'):
            body=tables.split(array+'[] = {',1)[1].split('};',1)[0]
            self.assertFalse(any(11<=int(n)<=17 for n in re.findall(r'\{(\d+),',body)))
        saved=tables.split('MapSaveRooms[] = {',1)[1].split('};',1)[0]
        self.assertFalse(any(int(n)>>8==4 for n in re.findall(r'\{(\d+),',saved)))
        self.assertIn('MapSaveCount=222;',tables) # native record stride/index range is unchanged
        selectors=(ROOT/'native/modules/re4_map_art_tables.h').read_text()
        self.assertNotIn('stage4/',selectors)
        expected={f'village-{r:03x}' for r in rooms}
        expected|={f'castle-{r:03x}' for r in castle_rooms}
        expected|={f'island-{r:03x}' for r in island_rooms}
        sw_rooms=[0x500,0x501,0x502,0x503,0x504,0x505,0x506,0x507,0x508,0x509,0x50a,0x50b,
            0x50c,0x50d,0x50e,0x50f,0x510,0x512,0x513,0x514,0x515,0x516,0x517,0x518,0x519,
            0x51a,0x51b,0x51c,0x51d,0x51e]
        self.assertEqual(len(sw_rooms),30) # never 0x511 (no native row) or 0x51F/0x520 (placeholders)
        expected|={f'stage5-{r:03x}' for r in sw_rooms}
        self.assertEqual(set(areas),expected)
        counts={'village':(14,15),'castle':(17,3),'island':(7,2)}
        for key,area in areas.items():
            self.assertTrue(all(lo<hi for lo,hi in zip(area['min'],area['max'])))
            if key.startswith('stage5-'):
                # Native SW treasure marks per area (mask 0x01BE7A5E), before S/goal; no medallions.
                count=SW_TREASURES[SW_AREA[key]]
                kinds=[m['i'] for m in area['icons']]
                self.assertNotIn('medallion',kinds)
                self.assertEqual(kinds[:count],['treasure']*count)
                self.assertEqual(kinds.count('treasure'),count)
                self.assertTrue(all(m['show_bind']=='re4.treasure.show' for m in area['icons'] if m['i']=='treasure'))
                stage='stage5'
            else:
                stage=key.split('-')[0]
                self.assertEqual(sum(m['i']=='treasure' for m in area['icons']),counts[stage][0])
                self.assertEqual(sum(m['i']=='medallion' for m in area['icons']),counts[stage][1])
                self.assertEqual([i['i'] for i in area['icons'] if i['i'] in ('treasure','medallion')],
                                 ['medallion']*counts[stage][1]+['treasure']*counts[stage][0])
            medal_index=0
            for n,marker in enumerate(area['icons']):
                if marker['i'].startswith('door-'):
                    self.assertEqual(marker['k'],'door_location')
                    self.assertEqual(marker['size'],40)
                    self.assertFalse(marker['structural'])
                    self.assertRegex(marker['show_bind'],r'^re4.map.door.\d+.\d+.[123].show$')
                elif marker['i'] in ('goal','save'):
                    self.assertEqual(marker['k'],marker['i']+'_location')
                    self.assertRegex(marker['show_bind'],rf'^re4.map.{marker["i"]}.\d+.\d+.show$')
                    self.assertEqual(marker['size'],80 if marker['i']=='save' else 152)
                else:
                    self.assertEqual(marker['k'],marker['i']+'_location')
                    if stage=='stage5':
                        self.assertEqual(marker['show_bind'],'re4.treasure.show');self.assertEqual(marker['size'],48)
                        for value,lo,hi in zip((marker['x'],marker['y']),area['min'],area['max']):
                            self.assertLess(lo,value);self.assertLess(value,hi)
                        continue
                    bind='re4.treasure.show' if marker['i']=='treasure' else (
                        f're4.medallion.{medal_index}.show' if stage=='village' else 're4.submission.show')
                    if marker['i']=='medallion':medal_index+=1
                    self.assertEqual(marker['show_bind'],bind)
                    self.assertEqual(marker['size'],12 if marker['i']=='medallion' else 48)
                self.assertFalse(marker['collectible'])
                for value,lo,hi in zip((marker['x'],marker['y']),area['min'],area['max']):
                    self.assertLessEqual(lo,value);self.assertLessEqual(value,hi)
        self.assertEqual(M['map']['atlas'],'module:re4/mark/atlas')
        self.assertEqual(M['map']['icons']['medallion'],{'r':0,'c':1})
        for page in M['pages']:
            maps=[w for w in page['widgets'] if w['type']=='map']
            if page['id'] in ('field','unavailable','hud_only') or page['id'].startswith('picker_'):
                self.assertEqual(len(maps),2)
                self.assertEqual(maps[0]['need_bind'],'re4.map.solo')
                self.assertEqual(maps[1]['need_bind'],'re4.map.escort')
                # Owner round4 fix (20261103-visual-fixes.md): the escort map used to stop
                # short (rect height 526, not solo's 738) to protect a floating Wait/Follow
                # bubble in its bottom-right corner. That bubble moved into the top bar, so
                # the escort map now reclaims the exact same full rect as solo -- nothing
                # left to protect.
                self.assertEqual(maps[1]['rect'],maps[0]['rect'])
                self.assertEqual(maps[0]['room_bind'],'re4.map.area')
                self.assertEqual(maps[0]['marker_rotate_bind'],'re4.map.heading')
                self.assertEqual(maps[0]['bg'],'#00000000')
                self.assertEqual(maps[0]['overlays'],M['map']['state_overlays'])
                self.assertEqual(maps[1]['overlays'],maps[0]['overlays'])
            else:self.assertEqual(maps,[])
    def test_separate_ways_native_frame(self):
        import re
        tables=(ROOT/'native/modules/re4_map_state_tables.h').read_text()
        rows={k:(int(r),int(a)) for k,r,a in re.findall(r'\{"(stage5-\w+)", (\d+), (\d+)',tables)}
        self.assertEqual({k:a for k,(_,a) in rows.items()},SW_AREA)
        self.assertTrue(all(k==f'stage5-{r:03x}' for k,(r,_) in rows.items()))
        overlays=M['map']['state_overlays']
        rooms={o['show_bind'].split('.')[3]:o for o in overlays if o['show_bind'].startswith('re4.map.room.stage5-')}
        self.assertEqual(set(rooms),set(SW_AREA))
        # Native placed frame: SW doors sit on a room of their own native area. 19.17 and 25.5
        # border only the unmapped placeholder pieces (0x520, 0x51F).
        off=[]
        for door in overlays:
            parts=door['show_bind'].split('.')
            if parts[2]!='door' or not 18<=int(parts[3])<=25:continue
            cx,cy=(door['x0']+door['x1'])/2,(door['y0']+door['y1'])/2
            if not any(SW_AREA[k]==int(parts[3]) and o['x0']-50<=cx<=o['x1']+50 and o['y0']-50<=cy<=o['y1']+50
                       for k,o in rooms.items()):off.append('.'.join(parts[3:5]))
        self.assertEqual(off,['19.17','25.5'])
        # The initial SW camera is the room's own box; S/goal/treasure only extend pan bounds.
        bounds=(ROOT/'native/modules/re4_map_bounds.h').read_text()
        camera=bounds[bounds.index('MapCameraBounds'):]
        for key,area in M['map']['areas'].items():
            if not key.startswith('stage5-'):continue
            box=[float(v) for v in re.search(r'area=="'+key+r'"\)return std::array<double,4>\{([^}]*)\}',camera)[1].split(',')]
            o=rooms[key]
            # same placed frame as the room art: the art's centre is inside the floor-fitted box
            self.assertTrue(box[0]<(o['x0']+o['x1'])/2<box[2] and box[1]<(o['y0']+o['y1'])/2<box[3])
            self.assertLessEqual(area['min'][0],box[0]);self.assertGreaterEqual(area['max'][0],box[2])
    def test_native_map_overlays(self):
        self.assertLess(len(json.dumps(M,ensure_ascii=False,separators=(',',':')).encode()),4*1024*1024)
        overlays=M['map']['state_overlays']
        rooms=[o for o in overlays if o['show_bind'].startswith('re4.map.room.')]
        doors=[o for o in overlays if o['show_bind'].startswith('re4.map.door.')]
        self.assertEqual(len(rooms),129)
        self.assertEqual({o['show_bind'].removeprefix('re4.map.room.').removesuffix('.show') for o in rooms},set(M['map']['areas']))
        self.assertEqual(len(doors),214)
        for o in overlays:
            parts=o['show_bind'].split('.')
            if parts[2] in ('door','save','goal'):self.assertNotIn(int(parts[3]),range(11,18))
        self.assertEqual(len({o['show_bind'] for o in overlays}),len(overlays))
        for overlay in overlays:
            self.assertLess(overlay['x0'],overlay['x1']);self.assertLess(overlay['y0'],overlay['y1'])
            self.assertEqual(overlay['src_bind'],overlay['show_bind'].removesuffix('.show')+'.art')
            self.assertTrue(overlay['src'].startswith('module:re4/room/' if overlay in rooms else 'module:re4/door/'))
            self.assertTrue(overlay['src'].endswith('/1'))
        tables=json.loads((ROOT/'tools/resident-evil-4/map_legend_tables.json').read_text())
        self.assertEqual([len(tables['doors'][str(i)]) for i in range(11)],
                         [0,31,52,13,5,2,7,11,6,29,7])
        self.assertEqual(len(tables['save_records']),222)
        self.assertEqual(tables['goals']['1'],[0,17,18,19,20,21,44,167,168,23,11,46])
        self.assertEqual(tables['goal_model_mask']&1,0)
        self.assertEqual(tables['typewriter_mask']&1,0)
    def test_r5_slots(self):
        self.assertNotIn('_status',M)
        self.assertNotIn('open_files',M['actions'])
        self.assertFalse(any(p['id']=='files' for p in M['pages']))
        for p in M['pages']:
            for w in p['widgets']:
                self.assertNotIn(w.get('text'), ('-', '- / -', 'State -'))
            if p['id'] not in ('field','unavailable'):continue
            # PER-SAVE SLOT ISOLATION: one widget variant per bucket (need_bind-gated, only
            # one ever shown at a time) x haptics on/off, not a single static button.
            slots=[w for w in p['widgets'] if 'on_hold' in w and w.get('on_tap')=='equip_unavailable']
            self.assertEqual(len(slots),4*4*2) # 4 slots x 4 buckets x {haptics on, off}
            self.assertEqual({w['haptic'] for w in slots},{'reject','off'}) # a distinct, deliberate refusal feel
            rects=sorted({tuple(w['rect']) for w in slots})
            self.assertEqual(len(rects),4)
            for n,rect in enumerate(rects):
                self.assertEqual(list(rect),[999,7+239*n,231,227])
                self.assertEqual((rect[2]-28,rect[3]-24),(203,203))
                same_rect=[w for w in slots if tuple(w['rect'])==rect]
                self.assertTrue(all(w['on_hold']==f'picker_{n}' and w['draggable'] for w in same_rect))
                self.assertEqual({w['drop_action'] for w in same_rect},{f'swap_{b}_{n}' for b in range(4)})
                # Regression (20261109-equip-regression.md): each slot widget's own
                # 'payload' must actually be n -- the mechanism direct tap-to-equip
                # depends on, since the shared 'equip_unavailable' action (below) has no
                # idea which of the 4 slots was tapped except via this per-widget value.
                self.assertTrue(all(w['payload']==n for w in same_rect))
            maps=[w for w in p['widgets'] if w['type']=='map']
            # Owner round4 fix (20261103-visual-fixes.md): the top bar grew 86->96px to
            # host an >=88px Wait/Follow button, so everything below it (map included)
            # shifted down by the same 10px (DY) to stay flush against the frame's new top
            # edge.
            self.assertEqual(maps[0]['rect'],[110,206,810,738])
            self.assertIn('view_rect_y0_bind',maps[0])
        for n in range(4):
            for bucket in range(4):
                self.assertEqual(M['actions'][f'clear_{bucket}_{n}']['kind'],'flag')
                self.assertEqual(M['actions'][f'clear_{bucket}_{n}']['flag'],f'slots.assignment.{bucket}')
                self.assertEqual(M['actions'][f'swap_{bucket}_{n}']['kind'],'flag')
                self.assertEqual(M['actions'][f'swap_{bucket}_{n}']['flag'],f'slots.assignment.{bucket}')
        self.assertEqual(M['actions']['equip_unavailable']['kind'],'module')
        # Regression (20261109-equip-regression.md): a 'module' action with no explicit
        # 'argument' field gets a constant 0 on every dispatch regardless of the tapped
        # widget's own payload (mod_manifest.cpp's parser default) -- silently reverted by
        # a manifest regen on 2026-11-02 (3286c0c) because the fix had only ever been
        # hand-patched into the generated manifest.json, never into prepare_r5.py, the
        # generator that actually owns this file. Every occupied-slot tap therefore always
        # requested the same case slot no matter which of the 4 quickbar positions was
        # tapped, and the one it requested was never served, so re4.weapon never changed on
        # any slot. Same pattern as the already-correct case_select action just below.
        self.assertEqual(M['actions']['equip_unavailable'].get('argument'),'$payload')
    def test_picker_viewport(self):
        for page in M['pages']:
            if not page['id'].startswith('picker_'):continue
            region,=page['scrolls']
            self.assertEqual(region['id'],'owned')
            self.assertEqual(region['axes'],'y')
            self.assertEqual(region['content_h_bind'],'re4.picker.height')
            x,y,w,h=region['rect']
            self.assertGreaterEqual(x,140)
            self.assertLessEqual(x+w,1100)
            self.assertGreaterEqual(y,163)
            self.assertGreaterEqual(h,570) # nearly five original 120 px rows
            buttons=[a for a in page['widgets'] if a.get('on_tap','').startswith('clear_')]
            self.assertTrue(buttons)
            self.assertLess(y+h,min(a['rect'][1] for a in buttons))
            rows=[a for a in page['widgets'] if a.get('color')!='#00FFFFFF' and (a.get('need_bind','').startswith('re4.picker.') or a.get('on_tap','').startswith('assign_'))]
            self.assertTrue(rows)
            for a in rows:
                self.assertEqual(a.get('scroll'),'owned')
                self.assertEqual(a.get('repeat'),1) # rt23 expansion owns clipping AND taps
            assignments={a['on_tap'] for a in rows if 'on_tap' in a}
            self.assertEqual(len(assignments),30*4)
            display,= [a for a in page['widgets'] if a.get('text')=='Display only']
            dx,dy,dw,dh=display['rect']
            self.assertTrue(140<=dx and dx+dw<=1100 and 70<=dy and dy+dh<=y)
            self.assertTrue(display['fit_text'])
            self.assertGreaterEqual(display['text_min_scale'],7)
            for a in buttons+[a for a in page['widgets'] if a.get('on_tap')=='open_field']:
                self.assertNotIn('scroll',a)
    def test_picker_input_blocks(self):
        for page in M['pages']:
            if not page['id'].startswith('picker_'):continue
            viewport=page['scrolls'][0]['rect']
            x,y,w,h=viewport
            blocks=[a for a in page['widgets'] if a.get('input_block')]
            self.assertTrue(blocks)
            for block in blocks:
                bx,by,bw,bh=block['rect']
                self.assertFalse(bx<x+w and x<bx+bw and by<y+h and y<by+bh,block)
            # Check every partition cell, including seams, for complete canvas coverage.
            rects=[viewport]+[a['rect'] for a in blocks if not a.get('need_bind')]
            xs=sorted({0,1240}|{r[0] for r in rects}|{r[0]+r[2] for r in rects})
            ys=sorted({0,1080}|{r[1] for r in rects}|{r[1]+r[3] for r in rects})
            for px in xs[:-1]:
                for py in ys[:-1]:
                    self.assertTrue(any(rx<=px<rx+rw and ry<=py<ry+rh for rx,ry,rw,rh in rects),(page['id'],px,py))
            for a in page['widgets']:
                self.assertFalse(any(a.get(k) for k in ('pan_zoom','draggable','select_group','accept_group','drop_action','on_hold')))
                if a.get('on_tap'):
                    self.assertTrue(a['on_tap'].startswith(('assign_','clear_')) or a['on_tap']=='open_field')

    def test_fit_text_width(self):
        for page in M['pages']:
            for widget in page['widgets']:
                if widget.get('fit_text'):
                    self.assertGreater(widget.get('wrap_width',0),0,(page['id'],widget))

    def test_dynamic_key_art_registry(self):
        for key in ('icon/b9','icon/ba','icon/c9','icon/a2','slot/27/missing'):
            self.assertIn('module:re4/'+key,M['sprite_map'])
            self.assertNotIn('module:re4/'+key,M['preload'])

    def test_inventory_rasters_match_all_consuming_widget_sizes(self):
        for page in M['pages']:
            for w in page['widgets']:
                if w.get('color')=='#00FFFFFF':continue
                bind=w.get('src_bind','')
                if bind.startswith('re4.slot.') and bind.endswith(('.art','.missing_art')):
                    self.assertEqual(w['rect'][2:],[163,163])
                    self.assertNotIn('scale_bind',w)
                if bind.startswith('re4.picker.') and bind.endswith('.art'):
                    self.assertEqual(w['rect'][2:],[180,90])
                    self.assertNotIn('scale_bind',w)
                if w.get('empty_src') in ('module:re4/map/frame','module:re4/map/backing','module:re4/map/unmapped'):
                    self.assertEqual(w['rect'],[6,114,981,838])

    def test_slot_art_triad_complete(self):
        ids={k.removeprefix('module:re4/weapon/') for k in M['sprite_map'] if k.startswith('module:re4/weapon/')}
        self.assertEqual(len(ids),30)
        for id in ids:
            for suffix in ('','/missing'):
                self.assertIn('module:re4/slot/'+id+suffix,M['sprite_map'])

    def test_unmapped_material(self):
        for page in M['pages']:
            if page['id'] not in ('field','unavailable','hud_only') and not page['id'].startswith('picker_'):continue
            frame=next(w for w in page['widgets'] if w.get('empty_src')=='module:re4/map/frame/0')
            fallback=next(w for w in page['widgets'] if w.get('src') in bg_sources('ui/field') and w.get('need_bind')=='!re4.map.available')
            self.assertEqual(frame['need_bind'],'re4.map.available')
            self.assertEqual(fallback['need_bind'],'!re4.map.available')

    def test_native_ammo_badges(self):
        for page in M['pages']:
            if page['id'] not in ('field','unavailable','hud_only') and not page['id'].startswith('picker_'):continue
            for n in range(4):
                key=f're4.slot.{n}'
                badge=[w for w in page['widgets'] if w.get('src_bind')==key+'.badge']
                self.assertEqual(len(badge),1)
                self.assertEqual(badge[0]['need_bind'],f'ui.slot.{n}.ammo.safe')
                gate=next(d for d in M['derived'] if d['name']==f'ui.slot.{n}.ammo.safe')
                self.assertEqual(gate['expr'],f"'{key}.ammo.available' && 're4.equip.safe'")
                self.assertFalse(any(w.get('bind_text')==key+'.ammo' for w in page['widgets']))
                for d in range(5):
                    self.assertTrue(any(w.get('src_bind')==key+f'.digit.{d}' and 'x_bind' in w and w.get('need_bind')==f'ui.slot.{n}.ammo.safe' for w in page['widgets']))
        for source in ('r5-badge-1.png','r5-badge-5.png'):
            self.assertIn('file:assets/'+source,M['sprite_map'])

    def test_unique_derived_names(self):
        names=[d['name'] for d in M['derived']]
        self.assertEqual(len(names),len(set(names)))

    def test_hardening_touch_spacing(self):
        def overlap(a,b):
            return a[0]<b[0]+b[2] and b[0]<a[0]+a[2] and a[1]<b[1]+b[3] and b[1]<a[1]+a[3]
        for page in M['pages']:
            buttons=[w for w in page['widgets'] if w['type']=='button']
            for w in buttons:
                if w.get('on_tap')=='ashley_toggle':
                    self.assertGreaterEqual(min(w['rect'][2:]),112)
                    for other in buttons:
                        if other.get('on_tap')!='ashley_toggle':self.assertFalse(overlap(w['rect'],other['rect']))
                if w.get('on_tap','').startswith('assign_'):
                    self.assertEqual(w['rect'][3],108)
            if page['id']=='key_items':
                for c in (1,2):
                    cells=list(dict.fromkeys(tuple(w['rect']) for w in buttons if w.get('on_tap','').startswith(f'key_select_{c}_')))
                    self.assertEqual(len(cells),8)
                    self.assertEqual(cells[1][0]-cells[0][0]-cells[0][2],12)
                    self.assertEqual(cells[4][1]-cells[0][1]-cells[0][3],12)
                    more=next(w['rect'] for w in buttons if w.get('on_tap')==f'key_next_{c}')
                    self.assertGreaterEqual(min(more[2:]),112)
                    self.assertLessEqual(more[1]+more[3],960)
                    last_row_bottom=max(w['rect'][1]+w['rect'][3] for w in buttons if w.get('on_tap','').startswith('key_select_'))
                    self.assertGreaterEqual(more[1]-last_row_bottom,12)

    def test_geometry(self):
        for page in M['pages']:
            for w in page['widgets']:
                x,y,width,height=w['rect']
                self.assertGreaterEqual(min(x,y,width,height),0)
                self.assertLessEqual(x+width,1240)
                if not w.get('scroll'):self.assertLessEqual(y+height,1080)
                if w['type']=='button' and (w.get('on_tap') or w.get('on_hold') or w.get('draggable')):
                    floor=112 if w.get('on_tap')=='ashley_toggle' else 108
                    self.assertGreaterEqual(min(width,height),floor)

    def test_money_unit_suffix(self):
        # B1: Case/Key Items/Settings show a "PTAS" suffix right after the digits, matching
        # case.png/keyitems.png (Field already has its own baked "PTAS" prefix, untouched here).
        for page in M['pages']:
            if page['id'] not in ('case','key_items','settings'):continue
            value=[w for w in page['widgets'] if w.get('type')=='value' and w.get('bind')=='re4.money']
            suffix=[w for w in page['widgets'] if w.get('text')=='PTAS']
            self.assertEqual(len(value),1);self.assertEqual(len(suffix),1)
            self.assertEqual(original_gate(value[0]),'re4.money.available')
            self.assertEqual(original_gate(suffix[0]),'re4.money.available')
            self.assertEqual(value[0]['align'],'right')
            # Suffix starts right at the value's fixed right edge, so it never drifts loose.
            self.assertEqual(suffix[0]['rect'][0],value[0]['rect'][0]+value[0]['rect'][2])

    def test_case_fill_and_badge(self):
        from PIL import Image
        for cols in (10,11,12,15):
            self.assertIn(f'module:re4/case/background/{cols}',M['sprite_map'])
        case=next(p for p in M['pages'] if p['id']=='case')
        self.assertFalse(any(w.get('text')=='Ammo' for w in case['widgets']))
        self.assertFalse(any(w.get('bind_text')=='re4.case.ammo' and w.get('align')=='right' for w in case['widgets']))
        badges=[w for w in case['widgets'] if w.get('src')=='file:assets/r5-case-ammo-badge.png']
        self.assertEqual(len(badges),1)
        self.assertEqual((badges[0]['x_bind'],badges[0]['y_bind']),('re4.case.badge.x','re4.case.badge.y'))
        self.assertEqual(badges[0]['need_bind'],'re4.case.ammo.available')
        name=next(w for w in case['widgets'] if w.get('bind_text')=='re4.case.name')
        self.assertTrue(name['fit_text']);self.assertGreaterEqual(name['text_min_scale'],11)
        self.assertEqual(name['wrap_width'],1112)
        self.assertGreaterEqual(name['rect'][1],872)
        taps=[w for w in case['widgets'] if w.get('on_tap')=='case_select']
        self.assertTrue(taps)
        for tap in taps:self.assertEqual(tap['rect'],[20,104,1200,768])
        self.assertEqual(M['actions']['case_select']['argument'],'$payload')
        items=[w for w in case['widgets'] if w.get('src_bind','').startswith('re4.case.shape.')]
        self.assertTrue(items)
        for item in items:
            shape=item['src_bind'].split('.')[3]
            w,h=map(int,shape.split('x'))
            self.assertEqual(item['rect'],[0,0,1080,720])
            import struct
            f32=lambda v:struct.unpack('f',struct.pack('f',v))[0]
            u0,v0,u1,v1=map(f32,item['src_rect'])
            self.assertEqual(int(f32(u0*(w*120))),-(1080-w*120)//2)
            self.assertEqual(int(f32(v0*(h*120))),-(720-h*120)//2)
            self.assertEqual(int(f32(f32(u1-u0)*(w*120))),1080)
            self.assertEqual(int(f32(f32(v1-v0)*(h*120))),720)
            self.assertEqual(item['repeat_bind'],f're4.case.shape.{shape}.count')
            self.assertEqual(item['scale_bind'],'re4.case.scale')
        self.assertIn('module:re4/case/23/120',M['sprite_map'])

    def test_native_case_name_glyphs_and_size(self):
        # The ASCII adapter needs explicit native punctuation. The two multiplication
        # glyph names use generated labels, keeping the original native strings intact.
        # Punctuation and baseline equivalence are verified by font_golden_test.py.
        for item in ('012','013'):
            self.assertIn(f'module:re4/case/name/{item}',M['sprite_map'])
        for page in M['pages']:
            if not page['id'].startswith('picker_'):continue
            names=[w for w in page['widgets'] if w.get('bind_text','').endswith('.name')]
            for name in names:
                self.assertGreaterEqual(name['text_min_scale'],11)
                self.assertEqual(name['max_lines'],2)

    def test_mercenaries_page(self):
        # B4: the real a2-direction layout, not the old double-background placeholder.
        merc=next(p for p in M['pages'] if p['id']=='mercenaries')
        srcs={w.get('src') for w in merc['widgets']}
        self.assertTrue(set(bg_sources('ui/mercenaries'))<=srcs)
        self.assertNotIn('file:assets/a2-neutral.png',srcs)
        self.assertFalse(any(w.get('text')=='Mercenaries data not available' for w in merc['widgets']))
        binds={'re4.merc.time':'label','re4.merc.combo':'value','re4.merc.combo_window':'value',
               're4.merc.score':'value','re4.merc.best':'value'}
        for name,kind in binds.items():
            ws=[w for w in merc['widgets'] if w.get('bind')==name or w.get('bind_text')==name]
            self.assertGreaterEqual(len(ws),1,name)
            for w in ws:self.assertEqual(original_gate(w),name+'.available')
        self.assertTrue(any(w['type']=='bar' and w.get('bind')=='re4.merc.combo_window' for w in merc['widgets']))
        # Dedicated 2-tab nav ("Mercenaries | Case"), not the generic Field/Case/Key Items strip.
        self.assertIn('open_mercenaries',M['actions']);self.assertIn('open_case',M['actions'])
        self.assertFalse(any(w.get('on_tap')=='open_field' for w in merc['widgets']))
        self.assertFalse(any(w.get('on_tap')=='open_key_items' for w in merc['widgets']))

    def test_pause_case_live_ready(self):
        # B5: wired and ready, fails closed until Group C publishes re4.menu.case -- pause still
        # maps to 'dimmed' (index 31, unchanged) until then.
        self.assertEqual(M['page_binds'][31]['point'],'re4.state')
        self.assertEqual(M['page_binds'][31]['equals'],31)
        self.assertEqual(M['page_binds'][31]['when_equal']['page'],'dimmed')
        d=next(d for d in M['derived'] if d['name']=='d.pause.case_live')
        self.assertIn('re4.state',d['expr']);self.assertIn('re4.menu.case',d['expr'])
        ready=[b for b in M['page_binds'] if b.get('point')=='d.pause.case_live']
        self.assertEqual(len(ready),1)
        self.assertEqual(ready[0]['when_equal']['page'],'case')

    def test_states_sheet(self):
        # Title/menus/Loading/GameOver(desaturated)/Unsupported, restyled to r5 materials.
        self.assertTrue(any(d['name']=='d.state.loading' for d in M['derived']))
        self.assertTrue(any(d['name']=='d.state.gameover' for d in M['derived']))
        standby=next(p for p in M['pages'] if p['id']=='standby')
        plain=[w for w in standby['widgets'] if w.get('src') in bg_sources('ui/standby')]
        gray=[w for w in standby['widgets'] if w.get('src')=='module:re4/ui/standby-gray']
        loading=[w for w in standby['widgets'] if w.get('src')=='module:re4/ui/loading-band']
        self.assertEqual(plain[0]['need_bind'],'!d.state.gameover')
        self.assertEqual(gray[0]['need_bind'],'d.state.gameover')
        self.assertEqual(loading[0]['need_bind'],'d.state.loading')
        unsupported=next(p for p in M['pages'] if p['id']=='unsupported')
        self.assertTrue(any(w.get('src')=='module:re4/ui/unsupported-band' for w in unsupported['widgets']))
        # No leftover nav strip on pre-game states (no save loaded yet to navigate to).
        for page in (standby,unsupported):
            self.assertFalse(any(w.get('on_tap','').startswith('open_') for w in page['widgets']))

    def test_round4_visual_fixes(self):
        # Owner review fixes, 2026-11-03 (analysis/20261103-visual-fixes.md), locked in so
        # they can't silently regress.
        #
        # (1) No full-screen native art is ever stretched: 'dimmed' (states 6/14/17/31,
        # menus + Pause-without-case) used to keep the OLD manifest's raw 'standby.png'
        # with fill:stretch verbatim -- squashing the native 1280x720 collage into
        # [0,0,1240,960]. It must now use the same properly aspect-fit r5-standby.png the
        # 'standby'/'unsupported' pages use, and no widget anywhere may request a stretch
        # fill at all.
        self.assertFalse(any(w.get('fill')=='stretch' for p in M['pages'] for w in p['widgets']))
        dimmed=next(p for p in M['pages'] if p['id']=='dimmed')
        self.assertTrue(any(w.get('src') in bg_sources('ui/standby') for w in dimmed['widgets']))
        self.assertFalse(any(w.get('src')=='file:assets/standby.png' for w in dimmed['widgets']))
        # (2) VILLAGE/CASTLE/ISLAND all sit in the exact same plate box (ISLAND used to
        # render far larger than the plate and overflow it).
        field=next(p for p in M['pages'] if p['id']=='field')
        label_rects={tuple(w['rect']) for w in field['widgets']
                     if w.get('empty_src','').startswith('module:re4/map/label/')}
        self.assertEqual(len(label_rects),1,label_rects)
        # (3) Wait/Follow lives in the top bar now, not a floating map bubble: a compact,
        # visual stays 88px tall; the transparent target extends to y112, above the map
        # (the old bubble sat at y=742).
        toggles=[w for w in field['widgets'] if w.get('on_tap')=='ashley_toggle']
        images=[w for w in field['widgets'] if w.get('src_bind')=='re4.ashley.bubble']
        self.assertTrue(toggles and images)
        for w in toggles+images:
            x,y,width,height=w['rect']
            self.assertGreaterEqual(height,88)
            self.assertLessEqual(y+height,112)
            self.assertGreater(x,600)  # right side of the bar, clear of the pesetas value
if __name__=='__main__':unittest.main()
