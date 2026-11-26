#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Parts ported from TotK Native (MIT, Copyright (c) 2026 MacNativeGames contributors), see the docstring.
"""Build the optional art pack of the Tears of the Kingdom companion from the player's own game files.

    make_art.py --romfs <romfs dir> [--romfs <update romfs dir>] --out <dir> [--lang USen]
    make_art.py --nup <game .nup> --nup-py <Peanuts Native dm/tools/nup.py> --out <dir>      (alternative input)

--romfs is the player's own extracted RomFS of 1.4.3 (Eden Duo: right-click the game, Dump RomFS, with the update
installed; or the base RomFS followed by the update's: later folders win). Writes <dir>/art/ (font, UI sprites, item
icons, item text in the chosen language, the three map layers). The companion reads it through the `user:` source, so
<dir> should be the title's user folder (see README); nothing here ships in the package or the repository. Keep <dir>
outside any git checkout. Requires Python 3.14 (compression.zstd), Pillow and texture2ddecoder.

The container readers (SARC, BYML, MSBT, BNTX, block-linear deswizzle, texture decode) and the table logic that
names pouch items (rsdb, base_actors, pouch_name_sources), the UI texture names and the map tile selection are ported
from TotK Native's games/totk/scripts/make_companion_assets.py, MIT license:

    Copyright (c) 2026 MacNativeGames contributors (https://github.com/AurelioB/totk-android)
    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
    documentation files (the "Software"), to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
    to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above
    copyright notice and this permission notice shall be included in all copies or substantial portions of the
    Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND (full text: NOTICE-TotKNative.txt).
"""
import argparse
import json
import re
import struct
import sys
import time
from pathlib import Path

try:
    from compression import zstd
    from PIL import Image, ImageChops, ImageDraw, ImageFont
    import texture2ddecoder as decoder
except ImportError:
    sys.exit('Needs Python 3.14 (compression.zstd), Pillow and texture2ddecoder.')

# ---- ported from TotK Native (MIT) ----
ASTC = dict(zip(range(0x2d, 0x3b), ((4,4),(5,4),(5,5),(6,5),(6,6),(8,5),(8,6),(8,8),(10,5),(10,6),(10,8),(10,10),(12,10),(12,12))))
LAYERS = {'G': 'Ground', 'S': 'Sky', 'U': 'UnderGround'}


def sarc(data):
    """Read named little-endian SARC entries, as in make_output_res_overlay.py."""
    if data[:4] != b'SARC' or data[6:8] != b'\xff\xfe':
        raise ValueError('Expected little-endian SARC')
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    if size != len(data):
        raise ValueError('Truncated SARC')
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16 + 8
    out = {}
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        at = names + (attr & 0xffffff) * 4
        name = data[at:data.index(0, at, start)].decode()
        out[name] = data[start + lo:start + hi]
    return out


def byml(data):
    """The little-endian BYML v7 subset used by the sort/location tables."""
    if data[:2] != b'YB':
        raise ValueError('Expected little-endian BYML')
    kt, st, root = struct.unpack_from('<III', data, 4)
    def strings(at):
        if not at:
            return []
        n = struct.unpack_from('<I', data, at)[0] >> 8
        offsets = struct.unpack_from(f'<{n}I', data, at + 4)
        return [data[at + o:data.index(0, at + o)].decode() for o in offsets]
    keys, values = strings(kt), strings(st)
    def value(t, v):
        if t == 0xa0: return values[v]
        if t == 0xc0:
            n = struct.unpack_from('<I', data, v)[0] >> 8
            start = (v + 4 + n + 3) & ~3
            return [value(data[v + 4 + i], struct.unpack_from('<I', data, start + i * 4)[0]) for i in range(n)]
        if t == 0xc1:
            n = struct.unpack_from('<I', data, v)[0] >> 8
            return {keys[struct.unpack_from('<I', data, a)[0] & 0xffffff]: value(data[a + 3], struct.unpack_from('<I', data, a + 4)[0]) for a in range(v + 4, v + 4 + n * 8, 8)}
        if t == 0xd0: return bool(v)
        if t == 0xd1: return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == 0xd2: return struct.unpack('<f', struct.pack('<I', v))[0]
        if t == 0xd3: return v
        if t == 0xd4: return struct.unpack_from('<q', data, v)[0]
        if t == 0xd5: return struct.unpack_from('<Q', data, v)[0]
        if t == 0xd6: return struct.unpack_from('<d', data, v)[0]
        if t == 0xff: return None
        raise ValueError(f'Unsupported BYML type {t:#x}')
    return value(data[root], root)

def msbt(data):
    """LBL1 bucket labels -> TXT2 strings, dropping formatting/control payloads.

    MSBT 0x0e tags carry group, type and a byte-length payload; 0x0f closing
    tags carry group/type only. Skip formatting parameters; declension tags
    (201/10) contain length-prefixed grammatical forms: keep the first, the standalone/nominative name.
    Preserve ordinary text/newlines and ruby base text.
    """
    if data[:8] != b'MsgStdBn':
        raise ValueError('Expected MSBT')
    endian = '<' if data[8:10] == b'\xff\xfe' else '>'
    if data[12] != 1:
        raise ValueError('Expected UTF-16 MSBT')
    sections = {}
    at = 0x20
    for _ in range(struct.unpack_from(endian + 'H', data, 14)[0]):
        size = struct.unpack_from(endian + 'I', data, at + 4)[0]
        sections[data[at:at + 4]] = data[at + 16:at + 16 + size]
        at = (at + 16 + size + 15) & ~15
    labels, texts = sections[b'LBL1'], sections[b'TXT2']
    count = struct.unpack_from(endian + 'I', texts)[0]
    offsets = struct.unpack_from(endian + f'{count}I', texts, 4)
    decoded = []
    for start in offsets:
        pos, pieces = start, bytearray()
        while pos + 2 <= len(texts):
            ch = struct.unpack_from(endian + 'H', texts, pos)[0]
            pos += 2
            if ch == 0: break
            if ch == 0xe:
                group, kind, length = struct.unpack_from(endian + '3H', texts, pos)
                if group == 201 and kind == 10:
                    size = struct.unpack_from(endian + 'H', texts, pos + 6)[0]
                    if length < 2 or size % 2 or size > length - 2 or pos + 6 + length > len(texts):
                        raise ValueError('Invalid MSBT declension')
                    pieces += texts[pos + 8:pos + 8 + size]
                pos += 6 + length
            elif ch == 0xf:
                pos += 4
            else:
                pieces += struct.pack(endian + 'H', ch)
        decoded.append(pieces.decode('utf-16-le' if endian == '<' else 'utf-16-be').strip())
    out = {}
    for bucket in range(struct.unpack_from(endian + 'I', labels)[0]):
        n, pos = struct.unpack_from(endian + 'II', labels, 4 + bucket * 8)
        for _ in range(n):
            size = labels[pos]
            label = labels[pos + 1:pos + 1 + size].decode()
            number = struct.unpack_from(endian + 'I', labels, pos + 1 + size)[0]
            out[label] = decoded[number]
            pos += 5 + size
    return out

def textures(data):
    if data[:4] != b'BNTX':
        raise ValueError('Expected BNTX')
    nx = data.index(b'NX  ', 0, 0x40)
    count, table = struct.unpack_from('<IQ', data, nx + 4)
    out = {}
    for i in range(count):
        at = struct.unpack_from('<Q', data, table + i * 8)[0]
        if data[at:at + 4] != b'BRTI': raise ValueError('Invalid BRTI')
        fmt, _, width, height, depth, arrays, block = struct.unpack_from('<7I', data, at + 0x1c)
        name_at = struct.unpack_from('<Q', data, at + 0x60)[0]
        name = data[name_at + 2:data.index(0, name_at + 2)].decode()
        pointers = struct.unpack_from('<Q', data, at + 0x70)[0]
        mip0 = struct.unpack_from('<Q', data, pointers)[0]
        out[name] = (fmt >> 8, width, height, block & 7, mip0, tuple(data[at + 0x58:at + 0x5c]), struct.unpack_from('<H', data, at + 0x12)[0])
    return out


def deswizzle(data, width, height, bpp, log_height):
    bh = 1 << log_height
    while bh > 1 and (height + 7) // 8 <= bh // 2:
        bh //= 2
    gobs = (width * bpp + 63) // 64
    row = width * bpp
    out = bytearray(row * height)
    for y in range(height):
        gy = y // (8 * bh) * 512 * bh * gobs + y % (8 * bh) // 8 * 512
        yp = y % 8 // 2 * 64 + y % 2 * 16
        dest = y * row
        for x in range(0, row, bpp):
            src = gy + x // 64 * 512 * bh + x % 64 // 32 * 256 + yp + x % 32 // 16 * 32 + x % 16
            block = data[src:src + bpp]
            if len(block) != bpp: raise ValueError('Truncated BNTX mip')
            out[dest:dest + bpp] = block
            dest += bpp
    return bytes(out)


def decode_texture(data, info):
    fmt, w, h, block, offset, selectors, tile_mode = info
    bw, bh, bpp = (*ASTC[fmt], 16) if fmt in ASTC else (4, 4, 8 if fmt in (0x1a, 0x1d) else 16)
    if fmt == 0x0b: bw, bh, bpp = 1, 1, 4
    if fmt == 0x02: bw, bh, bpp = 1, 1, 1
    if fmt in (0x07, 0x09): bw, bh, bpp = 1, 1, 2
    if fmt not in ASTC and fmt not in (0x02, 0x07, 0x09, 0x0b, 0x1a, 0x1c, 0x1d, 0x1e, 0x20):
        raise ValueError(f'Unsupported BNTX format {fmt:#x}')
    wb, hb = (w + bw - 1) // bw, (h + bh - 1) // bh
    linear = data[offset:offset + wb * hb * bpp] if tile_mode == 1 else deswizzle(memoryview(data)[offset:], wb, hb, bpp, block)
    if fmt in ASTC:
        pixels = decoder.decode_astc(linear, w, h, bw, bh)
    elif fmt == 0x02:
        red = Image.frombytes('L', (w, h), linear)
        image = Image.merge('RGBA', (red, Image.new('L', (w,h)), Image.new('L', (w,h)), Image.new('L', (w,h), 255)))
        pixels = None
    elif fmt == 0x07:
        image = Image.frombytes('RGB', (w, h), linear, 'raw', 'BGR;16').convert('RGBA')
        pixels = None
    elif fmt == 0x09:
        image = Image.merge('RGBA', (Image.frombytes('L', (w,h), linear[::2]),
                                   Image.frombytes('L', (w,h), linear[1::2]),
                                   Image.new('L', (w,h)), Image.new('L', (w,h), 255)))
        pixels = None
    elif fmt == 0x0b:
        image = Image.frombytes('RGBA', (w, h), linear)
        pixels = None
    else:
        method = {0x1a: decoder.decode_bc1, 0x1c: decoder.decode_bc3, 0x1d: decoder.decode_bc4, 0x1e: decoder.decode_bc5, 0x20: decoder.decode_bc7}[fmt]
        pixels = method(linear, w, h)
    if pixels is not None:
        image = Image.frombytes('RGBA', (w, h), pixels, 'raw', 'BGRA')
    channels = (Image.new('L', (w, h), 0), Image.new('L', (w, h), 255), *image.split())
    if any(s > 5 for s in selectors): raise ValueError(f'Invalid BNTX channel selectors {selectors}')
    return Image.merge('RGBA', [channels[s] for s in selectors])


UI_TEXTURES = {
    'heart': 'HeartW_00^s', 'heart_empty': 'HeartFrame_00^s',
    'heart_yellow': 'HeartW_00^s', 'heart_broken': 'HeartBreakW_00^s',
    'rupee': 'Nt_RupeeIconNew_00^w', 'poe': 'NumDisplay_82x82_MinusRupee^w',
    'battery': 'EnergyGaugeBigBaseFrame_00^s', 'battery_fill': 'EnergyGaugeCore_00^s',
    'stamina_ring': 'BonusIcon72_Ganbari_00^s',
    'cat_weapons': 'IconWeapon_72x72^s', 'cat_bows': 'IconBow_72x72^s',
    'cat_shields': 'IconShield_72x72^s', 'cat_armor': 'IconEquipment_72x72^s',
    'cat_materials': 'IconMaterial_72x72^s', 'cat_food': 'IconCuisine_72x72^s',
    # PaPouchTab_00_Pattern frames 6 and 8: capsule silhouette / key-item star.
    'cat_zonai': 'IconRecycle_72x72^s', 'cat_keyItems': 'Nt_PauseIconL_06^s',
    'map_player': 'Nt_MapPlayerIconNew_00^s',
    'marker_shrine': 'MapIconDungeon_L_Icon^s', 'marker_lightroot': 'LoadArrivalPoint_00^w',
    'marker_tower': 'MapIconTower_00^s',
    'marker_stable': 'MapIconBuildingStableInn_58x58^w',
    'marker_town': 'MapIconBuildingVillage_58x58^w',
    'background': 'PlayerStatusBG_00^t', 'frame_corner': 'Nt_PauseCorner_00^s',
}
for _id, _texture in {'Wind':'Wind', 'Fire':'Fire', 'Water':'Water', 'Electric':'Thunder', 'Soul':'Soul'}.items():
    UI_TEXTURES['sage_' + _id] = f'SageSoulIcon_{_texture}^s'
for _id, _texture in {'Sword':'Weapon','Pot':'Cuisine','Human':'Human','Rhombus':'Square','Heart':'Heart','Star':'Star','TreasureBox':'TreasureBox','Skull':'Skull','Leaf':'Leaf','Ore':'Ore'}.items():
    UI_TEXTURES['stamp_' + _id] = f'MapIcon{_texture}_72x72^w'
for _n, _id in enumerate(('Red','Blue','Yellow','Green','Purple','LightBlue')):
    UI_TEXTURES['pin_' + _id] = f'MapPinNewFLSmall{_n}_00^' + ('s' if _n == 5 else 'w')
EFFECT_TEXTURES = {
    'AttackUp':'AttackCook', 'DefenseUp':'ShieldUp', 'AllSpeed':'GoGo',
    'ResistCold':'Cool', 'ResistHot':'Hot', 'ResistBurn':'Fire',
    'ResistElectric':'Elect', 'QuietnessUp':'Noise', 'SwimSpeedUp':'Swim',
    'ClimbSpeedUp':'Mountain', 'SandMoveUp':'Sabaku', 'SnowMoveUp':'Sabaku',
    'LifeMaxUp':'Heart', 'StaminaRecover':'Ganbari', 'ExStaminaMaxUp':'GanbariEx',
    'LifeRepair':'HeartRepair', 'MiasmaGuard':'MiasmaGuard', 'NotSlippy':'Slip',
    'LightEmission':'Luminous', 'RupeeGuard':'Rupee', 'DivingMobility':'Diving',
    'DecreaseZonauEnergy':'DecreaseZonauEnergy', 'AttackUpCold':'AttackCold',
    'AttackUpHot':'AttackHot', 'AttackUpThunderstorm':'AttackThun',
}
ABILITY_ACTORS = {
    'UltraHand':'Obj_UltraHand', 'OneTouchBond':'Obj_OneTouchBond',
    'CeilingClipper':'Obj_Tooreroof', 'ReverseRecorder':'Obj_ReverseRecorder',
    'AutoBuilder':'Obj_AutoBuilder',
}
SAGE_ACTORS = {'Wind':'Rito','Fire':'Goron','Water':'Zora','Electric':'Gerudo','Soul':'Zonau'}

def rsdb(reader, table):
    """RSDB/<table>.Product.<version>.rstbl.byml.zs: the rows (a list of dicts keyed by __RowId)."""
    paths = [p for p in sorted(reader.files) if p.startswith(f'RSDB/{table}.Product.') and p.endswith('.rstbl.byml.zs')]
    if len(paths) != 1: raise ValueError(f'Expected one RSDB {table} table, found {len(paths)}')
    return byml(reader.get(paths[0]))


def base_actors(reader, actor):
    """The actors <actor> is built from: its ActorParam $parent chain, then its GameParameterTable and that table's
    $parent chain (stems of the Work/... paths, in that order, without <actor>)."""
    path = f'Pack/Actor/{actor}.pack.zs'
    if path not in reader.files: return []
    pack, out = sarc(reader.get(path)), []
    def stem(work): return work.rsplit('/', 1)[-1].split('.', 1)[0]
    def follow(member, pattern):
        while member in pack:
            parent = byml(pack[member]).get('$parent')
            if not parent: break
            out.append(stem(parent))
            member = pattern.format(stem(parent))
    param = f'Actor/{actor}.engine__actor__ActorParam.bgyml'
    if param not in pack: return []
    follow(param, 'Actor/{}.engine__actor__ActorParam.bgyml')
    table = (byml(pack[param]).get('Components') or {}).get('GameParameterTableRef')
    if table:
        table = table.lstrip('?')
        out.append(stem(table))
        follow(table, 'GameParameter/GameParameterTable/{}.engine__actor__GameParameterTable.bgyml')
    seen = {actor}
    return [a for a in out if not (a in seen or seen.add(a))]


def pouch_name_sources(reader, named, common):
    """Pouch/fuse actors -> their display actor, or @<AttachmentCommonName> message label.

    Pouch names use ActorInfo ActorName and the named actor's $parent chains. Fuse-only objects additionally use
    ActorMsg/Attachment and AttachmentActorInfo.AttachmentCommonName -> StaticMsg/AttachmentCommonName. These are
    the game's authored material names, not the adjectives used to construct a combined weapon's title.
    """
    info = {row['__RowId']: row.get('ActorName') for row in rsdb(reader, 'ActorInfo')}
    attachments = rsdb(reader, 'AttachmentActorInfo')
    out = {row['__RowId']: '@' + row['AttachmentCommonName'] for row in attachments
           if row['__RowId'] not in named and row.get('AttachmentCommonName', '') + '_Name' in common}
    named = named | out.keys()
    for row in rsdb(reader, 'PouchActorInfo') + attachments:
        actor = row['__RowId']
        if actor in named or actor in out: continue
        alias = info.get(actor)
        target = alias if alias and alias != actor and alias in named else next((a for a in base_actors(reader, actor) if a in named), None)
        if target: out[actor] = target
    return out



# ---- driver (this project) ----
COMMON = 'UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs'


class Reader:
    """The game's RomFS (a dump directory, or the RomFS inside a .nup), with the game's zstd dictionaries."""

    def __init__(self):
        self.files = {}
        self.dicts = {}

    def finish(self):
        for blob in sarc(zstd.decompress(self.raw('Pack/ZsDic.pack.zs'))).values():
            z = zstd.ZstdDict(blob)
            self.dicts[z.dict_id] = z
        return self

    def get(self, path):
        data = self.raw(path)
        if path.endswith('.zs'):
            did = zstd.get_frame_info(data).dictionary_id
            return zstd.decompress(data, zstd_dict=self.dicts[did] if did else None)
        return data


class DirReader(Reader):
    """One or more extracted RomFS folders; a later folder (the update) overrides an earlier one."""

    def __init__(self, roots):
        super().__init__()
        for root in roots:
            root = Path(root)
            if not root.is_dir():
                sys.exit(f'not a folder: {root}')
            for f in root.rglob('*'):
                if f.is_file():
                    self.files[f.relative_to(root).as_posix()] = f
        if 'Pack/ZsDic.pack.zs' not in self.files:
            sys.exit('no Pack/ZsDic.pack.zs: this is not the Tears of the Kingdom RomFS (give the folder that holds '
                     'Pack/, UI/, Mals/ ...)')
        self.finish()

    def raw(self, path):
        return self.files[path].read_bytes()


class NupReader(Reader):
    """The RomFS inside a .nup (Peanuts Native's game container), read with its nup.py."""

    def __init__(self, nup_py, nup_path):
        super().__init__()
        sys.path.insert(0, str(Path(nup_py).parent))
        import nup
        self.n = nup.Nup(str(nup_path))
        for p in self.n.packages:
            try:
                k = self.n.find('romfs', ('pkg', p.index))
            except FileNotFoundError:
                continue
            for d, _, files in self.n.walk(k):
                for f in files:
                    self.files[f'{d}/{f}' if d else f] = k
        self.finish()

    def raw(self, path):
        with self.n.open(self.n.find(path, self.files[path])) as r:
            return r.read()


def save_png(image, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, 'PNG', optimize=False)


def first_texture(data):
    infos = textures(data)
    return decode_texture(data, next(iter(infos.values())))


def make_ui(reader, out):
    data = sarc(reader.get(COMMON))['timg/__Combined.bntx']
    infos = textures(data)
    done = {}
    for key, name in sorted(UI_TEXTURES.items()):
        if name not in infos:
            continue
        image = decode_texture(data, infos[name])
        if key in ('marker_stable', 'marker_town'):
            alpha = ImageChops.multiply(ImageChops.invert(image.convert('L')), image.getchannel('A'))
            white = Image.new('L', image.size, 255)
            image = Image.merge('RGBA', (white, white, white, alpha))
        save_png(image, out / 'ui' / f'{key}.png')
        done[key] = list(image.size)
    return done


def make_text(reader, out, lang):
    found = sorted(f for f in reader.files if f.startswith(f'Mals/{lang}.Product.') and f.endswith('.sarc.zs'))
    if not found:
        sys.exit(f'no text for language {lang}: have ' + ', '.join(sorted({f.split('/')[1].split('.')[0] for f in reader.files if f.startswith('Mals/')})))
    path = found[-1]
    archive = sarc(reader.get(path))
    pouch = msbt(archive['ActorMsg/PouchContent.msbt'])
    attachment = msbt(archive['ActorMsg/Attachment.msbt'])
    common = msbt(archive['StaticMsg/AttachmentCommonName.msbt'])
    items = {}
    for label, text in pouch.items():
        for suffix, key in (('_Name', 'name'), ('_Caption', 'desc')):
            if label.endswith(suffix):
                items.setdefault(label[:-len(suffix)], {})[key] = text
                break
    for label, text in attachment.items():
        if label.endswith('_Name'):
            items.setdefault(label[:-5], {}).setdefault('name', text)
    sources = pouch_name_sources(reader, {a for a, i in items.items() if 'name' in i}, common)
    for actor, target in sources.items():
        src = {'name': common[target[1:] + '_Name']} if target.startswith('@') else items.get(target, {})
        if actor not in items and 'name' in src:
            items[actor] = dict(src)
    layout = {n: msbt(d) for n, d in archive.items() if n.startswith(('LayoutMsg/', 'StaticMsg/')) and n.endswith('.msbt')}
    cats = layout['LayoutMsg/Pouch_00.msbt']
    ui = {k: cats[f'Pa_SlideList_00-Pa_Category_{i:02d}-T_Text_00']
          for i, k in enumerate(('armor', 'bows', 'shields', 'weapons', 'materials', 'food', 'zonai', 'keyItems'))}
    lines = []
    for actor, item in sorted(items.items()):
        if item.get('name'):
            lines.append('\t'.join([actor, item['name'].replace('\t', ' ').replace('\n', ' '),
                                    item.get('desc', '').replace('\t', ' ').replace('\n', ' ')]))
    (out / 'text').mkdir(parents=True, exist_ok=True)
    (out / 'text' / 'items.tsv').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    (out / 'text' / 'lang.txt').write_text(lang + '\n', encoding='utf-8')
    (out / 'text' / 'ui.json').write_text(json.dumps(ui, ensure_ascii=False, indent=1), encoding='utf-8')
    return items, sources


def make_icons(reader, out, actors, sources, size):
    """Icons are saved by index (icons/<n>.png) and listed in text/icons.tsv (line n = actor id), so the manifest can
    name them with a printf pattern and carries no game names."""
    sort = byml(reader.get('UI/PouchSortTable/PouchSortTable.game__ui__PouchSortTable.bgyml'))
    colors = set(sort['ArmorDyeColor']) - {'None'}
    ids = []
    for path in sorted(reader.files):
        if not path.startswith(('UI/Tex/Icon/', 'UI/Tex/SpecialParts/')) or not path.endswith('.bntx.zs'):
            continue
        actor = Path(path).name[:-8]
        if actor.startswith('Armor_') and actor.rsplit('_', 1)[-1] in colors:
            continue
        if actor not in actors and not actor.startswith(('Item_Cook_', 'SpObj_')):
            continue
        if actor in ids:
            continue
        image = first_texture(reader.get(path))
        image.thumbnail((size, size), Image.Resampling.LANCZOS)
        save_png(image, out / 'icons' / f'{len(ids)}.png')
        ids.append(actor)
    index = {a: i for i, a in enumerate(ids)}
    lines = list(ids)
    # Pouch items that borrow another actor's icon share its file.
    for actor, target in sorted(sources.items()):
        if not target.startswith('@') and target in index and actor not in index:
            index[actor] = index[target]
            lines.append(f'{actor}\t{index[target]}')
    (out / 'text' / 'icons.tsv').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return len(ids)


def make_gauges(out):
    """Pre-coloured gauge frames: hearts filled left to right in quarters, stamina rings filled clockwise in 5% steps."""
    heart = Image.open(out / 'ui' / 'heart.png').convert('RGBA')
    ring = Image.open(out / 'ui' / 'stamina_ring.png').convert('RGBA')

    def tinted(image, rgb):
        r, g, b, a = image.split()
        solid = Image.new('RGBA', image.size, rgb + (255,))
        solid.putalpha(a)
        return solid

    for name, rgb in (('heart', (232, 80, 74)), ('heartx', (242, 212, 92))):
        full = tinted(heart, rgb)
        for q in range(5):
            keep = Image.new('L', heart.size, 0)
            if q:
                ImageDraw.Draw(keep).rectangle((0, 0, int(round(heart.width * q / 4)) - 1, heart.height), fill=255)
            piece = full.copy()
            piece.putalpha(ImageChops.multiply(piece.getchannel('A'), keep))
            save_png(piece, out / 'ui' / f'{name}_{q}.png')
    frame = Image.open(out / 'ui' / 'battery.png').convert('RGBA')
    core = Image.open(out / 'ui' / 'battery_fill.png').convert('RGBA')
    fw, fh = frame.size
    inner = core.resize((fw - 28, fh - 28), Image.Resampling.LANCZOS)
    for name, rgb in (('bat', (127, 232, 139)), ('batx', (242, 193, 76))):
        paint = tinted(inner, rgb)
        for n in range(21):
            keep = Image.new('L', paint.size, 0)
            if n:
                ImageDraw.Draw(keep).rectangle((0, 0, int(round(paint.width * n / 20)) - 1, paint.height), fill=255)
            layer = paint.copy()
            layer.putalpha(ImageChops.multiply(layer.getchannel('A'), keep))
            piece = frame.copy()
            piece.alpha_composite(layer, (14, 14))
            save_png(piece, out / 'ui' / f'{name}_{n}.png')
    for name, rgb in (('ring', (127, 232, 139)), ('ringx', (242, 193, 76))):
        full = tinted(ring, rgb)
        w, h = ring.size
        for n in range(21):
            keep = Image.new('L', ring.size, 0)
            if n >= 20:
                keep = Image.new('L', ring.size, 255)
            elif n:
                ImageDraw.Draw(keep).pieslice((-w, -h, 2 * w, 2 * h), -90, -90 + 360 * n / 20, fill=255)
            piece = full.copy()
            piece.putalpha(ImageChops.multiply(piece.getchannel('A'), keep))
            save_png(piece, out / 'ui' / f'{name}_{n}.png')


def make_map(reader, out, tile=200):
    pattern = re.compile(r'UI/Map/MainField/([GSU])_(\d\d)-(\d\d)_(01|02)(?:_([TF]+))?(?:_([AB](?:_[AB])*))?\.bntx\.zs$')
    selected = {}
    for path in sorted(reader.files):
        m = pattern.fullmatch(path)
        if not m:
            continue
        layer, col, row, lod, variant, event = m.groups()
        key = (layer, int(col), int(row), int(lod))
        rank = ((variant or '').count('T'), len(event or ''), event or '', variant or '')
        if key not in selected or rank > selected[key][0]:
            selected[key] = (rank, path)
    for layer, name in LAYERS.items():
        image = Image.new('RGB', (12 * tile, 10 * tile))
        for col in range(12):
            for row in range(10):
                entry = selected.get((layer, col, row, 2))
                if not entry:
                    continue
                t = first_texture(reader.get(entry[1])).convert('RGB')
                image.paste(t.resize((tile, tile), Image.Resampling.LANCZOS), (col * tile, row * tile))
        save_png(image, out / 'map' / f'{name.lower()}.png')


def make_font(reader, out):
    archive = sarc(reader.get('Font/Font.Nin_NX_NVN.bfarc.zs'))
    blob = archive['scft/ninP_RodinNTLG-M_DH_003_subset.bfotf']
    key = struct.unpack_from('<I', blob)[0] ^ 0x18029a7f
    size = int.from_bytes(struct.pack('<I', struct.unpack_from('<I', blob, 4)[0] ^ key), 'big')
    body, font = blob[8:8 + size], bytearray()
    for i in range(0, size - size % 4, 4):
        font += struct.pack('<I', struct.unpack_from('<I', body, i)[0] ^ key)
    font += body[size - size % 4:]
    return bytes(font)


def build_font_atlas(font_bytes, out, px=40, first=0x20, last=0x17F):
    """Render the game's font into an RGBA atlas (white, alpha = coverage) and an MFNT metrics file the runtime parses."""
    import io
    face = ImageFont.truetype(io.BytesIO(font_bytes), px, layout_engine=ImageFont.Layout.BASIC)
    ascent, descent = face.getmetrics()
    cell_w, cell_h = px + 8, ascent + descent + 4
    cols = 32
    count = last - first + 1
    rows = (count + cols - 1) // cols
    atlas = Image.new('RGBA', (cols * cell_w, rows * cell_h), (255, 255, 255, 0))
    records = []
    for i in range(count):
        cp = first + i
        ch = chr(cp)
        cx, cy = (i % cols) * cell_w, (i // cols) * cell_h
        adv = int(round(face.getlength(ch))) if cp != 0xa0 else int(round(face.getlength(' ')))
        mask = Image.new('L', (cell_w, cell_h), 0)
        ImageDraw.Draw(mask).text((2, 2), ch, font=face, fill=255)
        bbox = mask.getbbox()
        if not bbox or ch.isspace():
            records.append((cx, cy, 0, 0, 0, 0, max(adv, px // 3)))
            continue
        x0, y0, x1, y1 = bbox
        white = Image.new('RGBA', (x1 - x0, y1 - y0), (255, 255, 255, 255))
        white.putalpha(mask.crop(bbox))
        atlas.paste(white, (cx, cy))
        # pen at x=2, baseline at y=2+ascent in the cell
        records.append((cx, cy, x1 - x0, y1 - y0, x0 - 2, (2 + ascent) - y0, adv))
    header = bytearray(0x60)
    header[0:4] = b'MFNT'
    struct.pack_into('<III', header, 0x1c, ascent + descent, count + 1, 0)
    struct.pack_into('<I', header, 0x28, 0x60)
    body = bytearray(14)  # glyph 0 (notdef)
    for r in records:
        body += struct.pack('<HHHHhhH', *[v & 0xffff if k < 4 or k == 6 else v for k, v in enumerate(r)])
    (out / 'font').mkdir(parents=True, exist_ok=True)
    (out / 'font' / 'game.mfnt').write_bytes(bytes(header) + bytes(body))
    save_png(atlas, out / 'font' / 'game.png')
    return ascent + descent


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--romfs', action='append', default=[], help='extracted RomFS folder; repeat for base then update')
    ap.add_argument('--nup', help='alternative input: a Peanuts Native .nup (needs --nup-py)')
    ap.add_argument('--nup-py', help="Peanuts Native's dm/tools/nup.py")
    ap.add_argument('--out', required=True, help='output folder (outside any repository); art/ is created inside')
    ap.add_argument('--lang', default='USen', help='in-game language: USen (default), EUen, USes, EUes, EUfr, EUde, EUit, JPja, ...')
    ap.add_argument('--icon-size', type=int, default=96)
    ap.add_argument('--only', help='comma list of: ui,text,icons,map,font')
    args = ap.parse_args()
    steps = set((args.only or 'ui,text,icons,map,font').split(','))
    out = Path(args.out) / 'art'
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.monotonic()
    if args.romfs:
        reader = DirReader(args.romfs)
    elif args.nup and args.nup_py:
        reader = NupReader(args.nup_py, args.nup)
    else:
        ap.error('give --romfs <folder> (or --nup with --nup-py)')
    print(f'{len(reader.files)} files', flush=True)
    items, sources = {}, {}
    if 'ui' in steps:
        print('ui', len(make_ui(reader, out)), flush=True)
        make_gauges(out)
    if 'text' in steps or 'icons' in steps:
        items, sources = make_text(reader, out, args.lang)
        print('text', len(items), flush=True)
    if 'icons' in steps:
        print('icons', make_icons(reader, out, set(items), sources, args.icon_size), flush=True)
    if 'map' in steps:
        make_map(reader, out)
        print('map ok', flush=True)
    if 'font' in steps:
        print('font line height', build_font_atlas(make_font(reader, out), out), flush=True)
    print(f'done in {time.monotonic() - t0:.0f} s -> {out}')


if __name__ == '__main__':
    main()
