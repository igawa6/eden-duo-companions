#!/usr/bin/env python3
"""Generate native/modules/ot0_pak_pins.inc for Octopath Traveler 0 (Switch 1.0.5).

Inputs are the developer's OWN files (nothing here is shipped):
  --members  merged-pak-members.json  (index of the merged base+update pakchunk0-Switch.pak)
  --pak      the merged pakchunk0-Switch.pak
  --native   folder holding decoded members at their archive paths (Octopath_Traveler0/Content/...)
For every member in MEMBERS the in-data FPakEntry is re-read from the archive and checked against
the index record; the stored hash comes from that entry, the decoded hash from the decoded file.
An explicitly listed uncached member up to 1MiB can instead be decoded in RAM with
--oodle-library; this does not write game assets to disk.
Textures additionally record their inline first-mip layout (format, size, data offset).
"""
import argparse, ctypes, hashlib, json, struct, sys
from pathlib import Path

C = 'Octopath_Traveler0/Content/'
# (enum name, archive path) -- the only members the module reads.
MEMBERS = [
    ('InBox', 'UI/Common/Texture/UiTX_Common_InBox.uexp'),
    ('CharaFrame', 'UI/Common/Texture/UiTX_Common_CharaSelect_Frame.uexp'),
    ('CharaLight', 'UI/Common/Texture/UiTX_Common_CharaSelect_Light.uexp'),
    ('FocusLight', 'UI/Common/Texture/UiTX_Common_ListFocus_lightC.uexp'),
    ('MapNameLine', 'UI/Menu/Texture/MainMenu/Texture/UiTX_MainMenu_MapName_Line_Test.uexp'),
    ('Hand', 'UI/Menu/Texture/UiTX_MainMenu_Cursor_Menu.uexp'),
    ('IconParty', 'UI/Common/Texture/UiTX_Common_MenuListIcon_Mask_PartyFormation.uexp'),
    ('WorldMap', 'UI/Common/Texture/UiTX_Common_WorldMap_00.uexp'),
    ('Logo', 'UI/Title/Texture/UiTX_EndRoll_Logo_OCTOPATHTRAVELER.uexp'),
    ('SpriteViator', 'Character/Resource/V1_0_0/Ply/ChrTX_Chd_Ply_10101_J01/ChrTX_Chd_Ply_10101_J01_B.uexp'),
    ('SpriteCelsus', 'Character/Resource/V1_0_0/Ply/ChrTX_Chd_Ply_10503_J03/ChrTX_Chd_Ply_10503_J03_B.uexp'),
    ('SpriteMacy', 'Character/Resource/V1_0_0/Ply/ChrTX_Chd_Ply_10804_J02/ChrTX_Chd_Ply_10804_J02_B.uexp'),
    ('SpritePhenn', 'Character/Resource/V1_0_0/Ply/ChrTX_Chd_Ply_10905_J06/ChrTX_Chd_Ply_10905_J06_B.uexp'),
    ('SpriteAlexia', 'Character/Resource/V1_0_0/Ply/ChrTX_Chd_Ply_11407_J04/ChrTX_Chd_Ply_11407_J04_B.uexp'),
    ('SpriteStia', 'Character/Resource/V1_0_0/Ply/ChrTX_Chd_Ply_40204_J18/ChrTX_Chd_Ply_40204_J18_B.uexp'),
    ('ArtLeader', 'ExtTex/Character/UiTX_Ply_0901_J01/UiTX_Ply_0901_J01_1024.uexp'),
    ('ArtViator', 'ExtTex/Character/UiTX_Ply_10101_J01/UiTX_Ply_10101_J01_1024.uexp'),
    ('ArtCelsus', 'ExtTex/Character/UiTX_Ply_10503_J03/UiTX_Ply_10503_J03_1024.uexp'),
    ('ArtMacy', 'ExtTex/Character/UiTX_Ply_10804_J02/UiTX_Ply_10804_J02_1024.uexp'),
    ('ArtPhenn', 'ExtTex/Character/UiTX_Ply_10905_J06/UiTX_Ply_10905_J06_1024.uexp'),
    ('ArtAlexia', 'ExtTex/Character/UiTX_Ply_11407_J04/UiTX_Ply_11407_J04_1024.uexp'),
    ('ArtStia', 'ExtTex/Character/UiTX_Ply_40204_J18/UiTX_Ply_40204_J18_1024.uexp'),
    ('FontFranco', 'UI/Font/9PCS_Franco_Default.ufont'),
    ('FontFrancoBold', 'UI/Font/9PCS_Franco_B.ufont'),
    ('FontRocio', 'UI/Font/9PCS_Rocio_Default.ufont'),
    ('FontClearTone', 'UI/Font/9PCS_ClearTone_Default.ufont'),
    ('CharaPlayer', 'Local/DataBase/Character/CharaPlayer.uexp'),
    ('TextCharacter', 'Local/DataBase/GameText/Localize/EN-US/SystemText/GameTextCharacter.uexp'),
    ('TextMap', 'Local/DataBase/GameText/Localize/EN-US/SystemText/GameTextMap.uexp'),
    ('TextItem', 'Local/DataBase/GameText/Localize/EN-US/SystemText/GameTextItem.uexp'),
    # Map header: per-map region text / emblem ids, and the icon-id -> asset path table.
    ('MapListDb', 'Local/DataBase/Map/MapListTable.uexp'),
    ('TextureDb', 'Local/DataBase/Texture/TextureID.uexp'),
    ('ItemTypeDb', 'Local/DataBase/Item/ItemType.uexp'),
    ('QuestDb', 'Local/DataBase/Quest/QuestList.uexp'),
    ('TextQuest', 'Local/DataBase/GameText/Localize/EN-US/SystemText/GameTextQuest.uexp'),
    ('IconMap', 'UI/Common/Texture/UiTX_Common_MenuListIcon_Mask_WorldMap.uexp'),
    # Native main-menu 'Miscellaneous' glyph: the companion's Settings tab (footer).
    ('IconMisc', 'UI/Common/Texture/UiTX_Common_MenuListIcon_Miscellaneous.uexp'),
    # Native notification ring (minimap quest-marker ripple), soft left half used.
    ('NotifyRing', 'UI/Common/Texture/UiTX_Common_Notification_Ring.uexp'),
    ('MinimapIcons', 'UI/Menu/Texture/Textures/MiniMapIcon.uexp'),
    ('Minimap01', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_01_png.uexp'),
    # Treasure chest (EMAPICONLIST 2) and opened chest (3).
    ('Minimap02', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_02_png.uexp'),
    ('Minimap03', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_03_png.uexp'),
    ('Minimap08', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_08_png.uexp'),
    ('Minimap09', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_09_png.uexp'),
    ('Minimap10', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_10_png.uexp'),
    ('Minimap13', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_13_png.uexp'),
    ('Minimap26', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_26_png.uexp'),
    ('Minimap37', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_37_png.uexp'),
    ('Minimap39', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_39_png.uexp'),
    ('MinimapBack', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_Back_png.uexp'),
    ('MinimapRevealMask', 'UI/MiniMap/BP/T_mask.uexp'),
    ('BattleIcon', 'UI/Battle/Texture/Textures/Icon.uexp'),
    ('BattleDeco', 'UI/Battle/Texture/Textures/Decoration.uexp'),
    ('ShieldIcon', 'UI/Battle/Texture/UiTX_Battle_EnmBreake_Icon_001.uexp'),
    ('ShieldCrack', 'UI/Battle/Texture/UiTX_Battle_EnmBreakeCrack_75per.uexp'),
    ('BpDisc', 'UI/Battle/Texture/UiTX_Battle_Dot_Enem_MtexShine04.uexp'),
    ('BpStar', 'UI/Battle/Texture/UiTX_Battle_Dot_MtexShine02.uexp'),
    ('OrderBase', 'UI/Battle/Texture/UiTX_Battle_Oder_Base.uexp'),
    ('OrderMask', 'UI/Battle/Texture/UiTX_Battle_Oder_Mask.uexp'),
    ('OrderNext', 'UI/Battle/Texture/UiTX_Battle_Order_NextTurn_Back.uexp'),
    ('CommonIcon', 'UI/Common/Texture/Textures/Icon.uexp'),
    ('ListBase', 'UI/Common/Texture/UiTX_Common_ListBase_01.uexp'),
    ('EnemyDb', 'Local/DataBase/Enemy/EnemyID.uexp'),
    ('EnemyTypeDb', 'Local/DataBase/Enemy/EnemyTypeID.uexp'),
    ('EnemyAnimDb', 'Local/DataBase/Enemy/EnemyBattleAnimSet.uexp'),
    ('EnemyTexDb', 'Local/DataBase/Texture/EnemyTexID.uexp'),
    ('EnemyWeakDb', 'Local/DataBase/Enemy/EnemyWeakID.uexp'),
    ('TextEnemy', 'Local/DataBase/GameText/Localize/EN-US/SystemText/GameTextEnemy.uexp'),
    ('ItemDb', 'Local/DataBase/Item/ItemList.uexp'),
    ('Minimap11', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_11_png.uexp'),
    ('Minimap15', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_15_png.uexp'),
    ('Minimap23', 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_23_png.uexp'),
    ('CharaStatusDb', 'Local/DataBase/Character/CharaStatusID.uexp'),
    ('AilmentTypeDb', 'Local/DataBase/Skill/SkillAilmentType.uexp'),
    ('TextUI', 'Local/DataBase/GameText/Localize/EN-US/SystemText/GameTextUI.uexp'),
    ('TextSkill', 'Local/DataBase/GameText/Localize/EN-US/SystemText/GameTextSkill.uexp'),
    ('BattleIcon2', 'UI/Battle/Texture/Textures/Icon2.uexp'),
]
FORMATS = {'PF_B8G8R8A8': 'Bgra8', 'PF_DXT1': 'Bc1', 'PF_DXT5': 'Bc3'}

def texture_pin(b, name):
    p = b.find(b'PF_')
    if p < 0:
        return None
    end = b.index(b'\0', p); fmt = b[p:end].decode()
    w, h, slices = struct.unpack_from('<iii', b, p - 16)
    flen = struct.unpack_from('<i', b, p - 4)[0]
    assert flen == end - p + 1, (name, 'format FString length')
    assert slices == 1 and fmt in FORMATS, (name, fmt, slices)
    first, nmips, reserved = struct.unpack_from('<III', b, end + 1)
    assert first == 0 and nmips >= 1 and reserved == 0, (name, first, nmips, reserved)
    data = end + 1 + 12
    size = {'PF_B8G8R8A8': w * h * 4, 'PF_DXT1': ((w + 3) // 4) * ((h + 3) // 4) * 8,
            'PF_DXT5': ((w + 3) // 4) * ((h + 3) // 4) * 16}[fmt]
    assert struct.unpack_from('<iii', b, data + size) == (w, h, 1), (name, 'trailer')
    return dict(format=FORMATS[fmt], width=w, height=h, format_at=p, data_at=data, data_bytes=size, mips=nmips)

def decode_uncached(pak, entry, header, library_path):
    """One explicitly pinned small member, decoded in memory; never writes game assets."""
    assert 0 < entry['size'] <= 1024 * 1024 and not entry['encrypted']
    blocks = [tuple(b) for b in entry['blocks']] if entry['compression'] else []
    if not blocks:
        assert entry['compression'] == 0 and entry['compressed_size'] == entry['size']
        pak.seek(entry['offset'] + len(header)); data = pak.read(entry['size'])
        assert len(data) == entry['size'] and hashlib.sha1(data).digest() == header[28:48]
        return data
    assert entry['compression'] == 1
    assert blocks[0][0] == len(header)
    assert all(blocks[i][0] == blocks[i-1][1] for i in range(1,len(blocks)))
    assert blocks[-1][1] - blocks[0][0] == entry['compressed_size']
    pak.seek(entry['offset'] + blocks[0][0]); stored = pak.read(entry['compressed_size'])
    assert len(stored) == entry['compressed_size'] and hashlib.sha1(stored).digest() == header[28:48]
    library = ctypes.CDLL(str(Path(library_path).resolve()))
    decode = library.OodleLZ_Decompress
    decode.argtypes = [ctypes.c_void_p,ctypes.c_long,ctypes.c_void_p,ctypes.c_long,
                       ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_void_p,ctypes.c_long,
                       ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_long,ctypes.c_int]
    decode.restype = ctypes.c_long
    result = bytearray()
    for a,b in blocks:
        assert 0 < b-a <= len(stored)
        n = min(entry['block_size'] or entry['size'], entry['size']-len(result)); assert n > 0
        raw = stored[a-blocks[0][0]:b-blocks[0][0]]
        target = ctypes.create_string_buffer(n)
        assert decode(raw,len(raw),target,n,1,0,0,None,0,None,None,None,0,3) == n
        result.extend(target.raw)
    assert len(result) == entry['size']
    return bytes(result)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--members', required=True); ap.add_argument('--pak', required=True)
    ap.add_argument('--native', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--oodle-library', help='Existing reference library for uncached <=1MiB members; decoded only in RAM')
    a = ap.parse_args()
    idx = json.loads(Path(a.members).read_text())['members']
    # All native minimap PaperSprite frames share the verified atlas/259-byte
    # serialization. Pins only supply artwork; live native filtering decides
    # whether any marker is visible.
    prefix = C + 'UI/Menu/Texture/Frames/UiTX_MainMenu_MiniMapIcon_'
    existing = {C + rel for _, rel in MEMBERS}
    for path, entry in sorted(idx.items()):
        if path.startswith(prefix) and path.endswith('_png.uexp') and path not in existing:
            suffix = path[len(prefix):-len('_png.uexp')]
            assert suffix.isascii() and suffix.replace('_', '').isalnum() and entry['size'] == 259
            MEMBERS.append(('MinimapFrame' + suffix, path[len(C):]))

    # Every playable character's field sprite sheet (looked up by party id in the folder name).
    sprites = C + 'Character/Resource/V1_0_0/Ply/ChrTX_Chd_Ply_'
    for path, entry in sorted(idx.items()):
        if path.startswith(sprites) and path.endswith('_B.uexp') and path not in existing:
            folder = path[len(sprites):].split('/')[0]
            assert folder.replace('_', '').isalnum(), path
            MEMBERS.append(('CharaSprite' + folder, path[len(C):]))
    # Every common UI frame (item, tab and equipment-slot icons on the CommonIcon atlas).
    frames = C + 'UI/Common/Texture/Frames/UiTX_Common_'
    for path, entry in sorted(idx.items()):
        if path.startswith(frames) and path.endswith('_png.uexp') and path not in existing:
            suffix = path[len(frames):-len('_png.uexp')]
            if '-' in suffix:
                continue  # localized text art (e.g. _EN-US), not icons
            assert suffix.isascii() and suffix.replace('_', '').isalnum() and entry['size'] == 259, path
            MEMBERS.append(('CommonFrame' + suffix, path[len(C):]))
    # Every native region emblem (the map header shows the current map's own one).
    emblem = C + 'UI/Common/Texture/UiTX_Common_AreaEmblem_'
    for path, entry in sorted(idx.items()):
        if path.startswith(emblem) and path.endswith('Mtex.uexp') and path not in existing:
            suffix = path[len(emblem):-len('Mtex.uexp')]
            assert suffix.isascii() and suffix.isalnum()
            MEMBERS.append(('Emblem' + suffix, path[len(C):]))
    pak = Path(a.pak).open('rb'); pak_size = Path(a.pak).stat().st_size
    out = ['// Generated by tools/octopath-traveler-0/gen_pak_pins.py -- do not edit.',
           f'// Merged pakchunk0-Switch.pak, {pak_size} bytes (Switch 1.0.5).', '']
    rows = []; tex = []
    for enum, rel in MEMBERS:
        path = C + rel; e = idx[path]
        assert not e['encrypted'] and e['compression'] in (0,1), path
        assert 0 <= e['offset'] < pak_size and 53 <= e['header_bytes'] <= pak_size-e['offset']
        pak.seek(e['offset']); hdr = pak.read(e['header_bytes'])
        assert len(hdr) == e['header_bytes']
        assert struct.unpack_from('<QQQI', hdr, 0) == (0, e['compressed_size'], e['size'], e['compression']), path
        stored = hdr[28:48]
        blocks = [tuple(b) for b in e['blocks']] if e['compression'] else []
        assert len(hdr) == (57 + 16*len(blocks) if e['compression'] else 53)
        assert hdr[-5] == 0
        assert bool(blocks) == bool(e['compression'])
        if blocks:
            assert struct.unpack_from('<I',hdr,48)[0] == len(blocks)
            assert [struct.unpack_from('<QQ',hdr,52+16*i) for i in range(len(blocks))] == blocks
            assert blocks[-1][1] <= pak_size-e['offset']
            assert blocks[0][0] == len(hdr)
            assert all(start < end for start,end in blocks)
            assert all(blocks[i][0] == blocks[i-1][1] for i in range(1,len(blocks)))
            assert blocks[-1][1]-blocks[0][0] == e['compressed_size']
        else:
            assert e['compressed_size'] == e['size'] <= pak_size-e['offset']-len(hdr)
        # The compact index omits the one-block size (0); its in-data FPakEntry
        # stores the decoded size. Pin the actual header, checking that normalization.
        header_block_size = struct.unpack_from('<I', hdr, len(hdr) - 4)[0]
        expected_block_size = e['block_size']
        if blocks and len(blocks) == 1 and expected_block_size == 0:
            expected_block_size = e['size']
        assert header_block_size == expected_block_size, (path, 'header block size')
        if blocks:
            assert header_block_size > 0
            assert len(blocks) == (e['size']+header_block_size-1)//header_block_size
        cached = Path(a.native) / path
        if cached.exists(): data = cached.read_bytes()
        else:
            if not a.oodle_library: raise RuntimeError('Missing cached member; supply --oodle-library to pin in RAM: '+path)
            data = decode_uncached(pak,e,hdr,a.oodle_library)
        assert len(data) == e['size'], path
        decoded = hashlib.sha1(data).digest()
        if blocks:
            out.append(f'constexpr Block {enum}Blocks[] = {{' + ', '.join(f'{{{s}, {t}}}' for s, t in blocks) + '};')
        hx = lambda d: '{' + ', '.join(f'0x{x:02x}' for x in d) + '}'
        rows.append(f'    {{"{rel}", {e["offset"]}ULL, {e["compressed_size"]}ULL, {e["size"]}ULL, {e["compression"]}, '
                    f'{e["header_bytes"]}, {header_block_size}, ' + (f'{enum}Blocks' if blocks else '{}') +
                    f', {hx(stored)}, {hx(decoded)}}},')
        t = texture_pin(data, path) if rel.endswith('.uexp') and not rel.startswith('Local/') else None
        tex.append((enum, t))
    out.append('constexpr PakMember Members[] = {'); out += rows; out.append('};')
    out.append('constexpr std::optional<TexturePin> TexturePins[] = {')
    for enum, t in tex:
        out.append(f'    TexturePin{{TextureFormat::{t["format"]}, {t["width"]}, {t["height"]}, {t["format_at"]}, {t["data_at"]}, {t["data_bytes"]}}}, // {enum}'
                   if t else f'    std::nullopt, // {enum}')
    out.append('};')
    Path(a.out).write_text('\n'.join(out) + '\n')
    Path(a.out).with_name('ot0_member_ids.inc').write_text('// Generated by gen_pak_pins.py\n' + '\n'.join(f'X({e})' for e, _ in MEMBERS) + '\n')
    print('pinned', len(MEMBERS), 'members;', sum(1 for _, t in tex if t), 'textures; pak', pak_size)

if __name__ == '__main__':
    main()
