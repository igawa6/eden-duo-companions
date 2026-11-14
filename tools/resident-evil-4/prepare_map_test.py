#!/usr/bin/env python3
"""Coordinate-preservation contract, independent of extracted map assets."""
import ast
from pathlib import Path
import unittest
import struct
from PIL import Image
import map_legend

source = ast.parse(Path(__file__).with_name('prepare_map.py').read_text())
function = next(n for n in source.body if isinstance(n, ast.FunctionDef) and n.name == 'marker_icons')
namespace = {}
exec(compile(ast.Module(body=[function], type_ignores=[]), '<marker contract>', 'exec'), namespace)
marker_icons = namespace['marker_icons']


class StageEmissionContract(unittest.TestCase):
    def test_stage4_header_rows_are_opt_in_and_save_indices_stay_native(self):
        import tempfile
        tables=dict(room_areas={'village-100':1,'stage4-400':11},
            rooms={'1':[(256,1,2,3)],'11':[(1024,4,5,6)]},
            save_records=[dict(room=256,record=0),dict(room=1024,record=1)],
            draw_doors=[dict(area=a,index=0,angle=0,type=0,flag=0,item=0) for a in (1,11)],
            draw_marks=[dict(area=a,index=0,kind='goal') for a in (1,11)],
            goals={'1':[1],'11':[99]})
        with tempfile.TemporaryDirectory(prefix='re4-header-test-') as directory:
            path=Path(directory)/'tables.h'
            for include in (False,True):
                map_legend.write_tables(path,tables,[('village-100',256),('stage4-400',1024)],set(),include)
                text=path.read_text()
                self.assertEqual('stage4-400' in text,include)
                self.assertEqual('{11,' in text,include)
                self.assertEqual('{1024, 1}' in text,include)
                self.assertIn('{256, 0}',text)
                self.assertIn('MapSaveCount=2;',text)

    def test_stage4_legend_areas_are_opt_in_without_renumbering(self):
        self.assertEqual([n for n,_ in map_legend.area_files()],list(range(1,11))+list(range(18,26)))
        self.assertEqual(map_legend.area_files(True),list(enumerate(map_legend.AREA_FILES,1)))
        self.assertEqual(dict(map_legend.area_files())[18],'map_obj5a.dat')
        self.assertTrue(all(not f.startswith('map_obj4') for _,f in map_legend.area_files()))

    def test_stage4_is_explicitly_opt_in(self):
        from types import SimpleNamespace
        function = next(n for n in source.body if isinstance(n, ast.FunctionDef) and n.name == 'map_stages')
        namespace = {}
        exec(compile(ast.Module(body=[function], type_ignores=[]), '<stage contract>', 'exec'), namespace)
        tables = SimpleNamespace(CASTLE_AREAS=[('map_obj2a.dat', [(0x200,11)])],
            ISLAND_AREAS=[('map_obj3a.dat', [(0x300,11)])],
            STAGE4_AREAS=[('map_obj4a.dat', [(0x400,11)])])
        stages = namespace['map_stages']
        self.assertEqual(stages(tables), [('castle',tables.CASTLE_AREAS),('island',tables.ISLAND_AREAS)])
        self.assertEqual(stages(tables,True), stages(tables)+[('stage4',tables.STAGE4_AREAS)])


class MarkerContract(unittest.TestCase):
    def test_native_positions_are_never_moved(self):
        points = [(-15043.6552734375, 0, 7159.544921875), (67984.5, 0, 33068.296875)]
        markers = marker_icons(points, 'treasure', (-30000, 71000, -24000, 36000),
                               'village', lambda n: 're4.treasure.show')
        self.assertEqual([(m['x'], m['y']) for m in markers], [(x, -z) for x, _, z in points])

    def test_outside_and_edge_points_fail(self):
        for x, z in [(-2, 0), (2, 0), (0, -2), (0, 2), (-1, 0), (0, 1)]:
            with self.subTest(x=x, z=z), self.assertRaises(ValueError):
                marker_icons([(x, 0, z)], 'treasure', (-1, 1, -1, 1), 'test', lambda n: '')


def module_value(name):
    node = next(n for n in source.body if isinstance(n, ast.Assign) and
                any(getattr(t, 'id', None) == name for t in n.targets))
    return ast.literal_eval(node.value)


class SeparateWaysContract(unittest.TestCase):
    # Switch mapDataInit areas 18..25 (room, piece) as decoded from the ELF jump table
    # main+0xA129D0, including the two placeholder rows that must stay unmapped.
    NATIVE = {18: [(0x500, 11), (0x501, 12), (0x502, 13), (0x503, 14)],
              19: [(0x504, 11), (0x505, 12), (0x506, 13), (0x507, 14), (0x508, 15), (0x509, 16),
                   (0x50A, 17), (0x50B, 18), (0x520, 19)],
              20: [(0x50C, 11), (0x50D, 12), (0x50E, 13), (0x50F, 14), (0x510, 15)],
              21: [(0x512, 11), (0x514, 12)], 22: [(0x513, 11)],
              23: [(0x515, 11), (0x516, 12), (0x517, 13)],
              24: [(0x518, 11), (0x519, 12), (0x51A, 13), (0x51B, 14), (0x51C, 15)],
              25: [(0x51D, 11), (0x51E, 12), (0x51F, 13)]}

    def test_room_table_is_the_native_table_minus_placeholders(self):
        areas = module_value('SEPARATE_WAYS_AREAS')
        self.assertEqual([a for a, _, _ in areas], list(range(18, 26)))
        self.assertEqual([f for _, f, _ in areas], [f'map_obj5{c}.dat' for c in 'abcdefgh'])
        for area, _, rows in areas:
            self.assertEqual(rows, [r for r in self.NATIVE[area] if r[0] not in (0x51F, 0x520)])
        rooms = [r for _, _, rows in areas for r, _ in rows]
        self.assertEqual(len(rooms), 30)
        self.assertNotIn(0x511, rooms)
        self.assertEqual(module_value('NATIVE_HEADING')[0x503], -70)

    def test_treasure_counts_follow_the_native_mask(self):
        mask = module_value('SW_TREASURE_MASK')
        counts = module_value('SW_TREASURE_COUNT')
        self.assertEqual(mask, 0x01BE7A5E)
        self.assertEqual(sorted(counts), [a for a in range(18, 26) if (mask >> a) & 1])
        self.assertEqual(sum(counts.values()), 22)


class NativeModelContract(unittest.TestCase):
    def model(self):
        data=bytearray(0x140)
        for at,value in ((0,0x98),(0x18,0xd0),(0x30,0xf0),(0x48,0xa8)):
            struct.pack_into('<Q',data,at,value)
        data[0x29]=1;struct.pack_into('<H',data,0x2a,1)
        struct.pack_into('<I',data,0x3c,1);struct.pack_into('<H',data,0x58,3)
        struct.pack_into('<I',data,0x5c,0x20030818)
        struct.pack_into('<BBBBfff',data,0x98,0,255,0,0,200,100,300)
        struct.pack_into('<9f',data,0xa8,0,0,0,100,0,0,0,0,100)
        struct.pack_into('<6f',data,0xd0,0,0,1,0,0,1)
        struct.pack_into('<I',data,0xf0+24,8)
        struct.pack_into('<IHH',data,0xf0+32,1,5,3)
        return data

    def test_native_mesh_and_parts_transform(self):
        data=self.model();model=map_legend.native_room_model(data)
        self.assertEqual(model['vertices'],[(200,100,300),(300,100,300),(200,100,400)])
        self.assertEqual(model['triangles'],[(0,1,2)])
        self.assertEqual(model['floor'],1)
        self.assertEqual(map_legend.native_room_model(data,10)['vertices'][0],(2000,1000,3000))
        data[0x29]=2
        with self.assertRaises(ValueError):map_legend.native_room_model(data)

    def test_texture_and_lit_map_composite(self):
        colours={0:(255,255,255,255),1:(135,135,135,255),2:(170,70,70,255),3:(75,40,40,255)}
        box,states,multi=map_legend.room_model_images([map_legend.native_room_model(self.model())],1,
            Image.new('RGBA',(2,2),(200,160,120,255)),colours)
        self.assertFalse(multi)
        self.assertEqual(states['1'].convert('RGBA').getpixel((14,14)),(134,98,64,255))
        self.assertEqual(states['2'].convert('RGBA').getpixel((14,14)),(157,64,38,255))
        self.assertEqual(states['3'].convert('RGBA').getpixel((14,14)),(95,49,27,255))
        self.assertEqual(states['4'].convert('RGBA').getpixel((14,14)),(212,161,111,255))
        self.assertEqual(states['4'].convert('RGBA').getpixel((0,0))[3],0)

    def test_current_room_other_floor_uses_native_alpha(self):
        lower=map_legend.native_room_model(self.model());lower['floor']=0
        upper=map_legend.native_room_model(self.model());upper['vertices']=[(x+200,y+100,z) for x,y,z in upper['vertices']]
        colours={0:(255,255,255,255),1:(135,135,135,255),2:(170,70,70,255),3:(75,40,40,255),
                 5:(135,135,135,255),6:(255,255,255,30)}
        _,states,multi=map_legend.room_model_images([lower,upper],1,Image.new('RGBA',(2,2),'white'),colours)
        self.assertTrue(multi)
        self.assertEqual(states['4-f0'].convert('RGBA').getpixel((214,14)),(255,240,223,30))
        self.assertEqual(states['4-f1'].convert('RGBA').getpixel((214,14)),(255,240,223,255))


    def test_measured_native_map_interiors(self):
        # Independent native capture medians at registered, eroded room interiors.
        # These are framebuffer targets, not the UWF legend swatches.
        texture=[[[162,135,115,255]]]
        for tint,native in [((255,255,255,255),(176,138,105)),
                            ((135,135,135,255),(121,89,63)),
                            ((170,70,70,255),(137,60,37)),
                            ((75,40,40,255),(81,43,25))]:
            actual=map_legend.map_material(texture,tint)[0,0,:3]
            self.assertLessEqual(max(abs(int(a)-b) for a,b in zip(actual,native)),8)
        self.assertGreater(map_legend.map_material(texture,(75,40,40,255))[0,0,0],66)

    def test_door_animation_and_native_composite(self):
        data=bytearray(8+3*0xa8);data[:4]=b'2.00';data[5]=3
        for i,(unit,colour) in enumerate([(16,(0,255,0,255)),(17,(255,0,0,255)),(18,(0,255,255,255))]):
            at=8+i*0xa8;data[at+4]=unit;data[at+0x58:at+0x5c]=bytes(colour)
        data[8+2*0xa8+0x5c:8+2*0xa8+0x60]=bytes((0,60,60,255))
        for actual,native in zip(map_legend.door_material(data),[(55,244,11),(255,32,11),(53,130,109)]):
            self.assertLessEqual(max(abs(int(a)-b) for a,b in zip(actual,native)),8)

    def test_measured_s_pin_uses_sprite_material(self):
        # Native sprite interior, separate from the models' light mask 4.
        actual=map_legend.map_material([[[0,105,99,255]]],diffuse=1)[0,0,:3]
        self.assertLessEqual(max(abs(int(a)-b) for a,b in zip(actual,(46,132,106))),8)

    def test_exact_palette_and_transparent_material(self):
        import numpy as np
        pixels=np.array([[[162,135,115,255],[90,70,50,0],[255,255,255,30]]],dtype='uint8')
        lit=map_legend.map_material(pixels)
        self.assertEqual(tuple(lit[0,1]),(0,0,0,0))
        self.assertEqual(int(lit[0,2,3]),30)
        image=Image.fromarray(lit)
        self.assertEqual(image.tobytes(),map_legend.palette_image(image).convert('RGBA').tobytes())


if __name__ == '__main__':
    unittest.main()
