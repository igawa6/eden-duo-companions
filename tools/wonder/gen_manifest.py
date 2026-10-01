#!/usr/bin/env python3
"""gen_manifest.py [<out dir>] - Super Mario Bros. Wonder companion pages (1240x1080).

Writes manifest.json and the per-build data files into packages/SuperMarioWonder/dualscreen/
(or <out dir>). The font descriptor wonder_font.txt next to them is hand-written.

Layout follows the reference photos of the third-party Eden-DS Wonder companion (title / world
map / course), rebuilt with Eden Duo widgets. Art: game textures decoded by the module from the
player's romfs (module:wonder:<key>) plus package-generated backgrounds (module:wonder:gen/*).
"""
import json, os, sys

W, H = 1240, 1080
INK, CREAM, YELLOW = '#FF232723', '#FFFFFAE5', '#FFFFD336'
GREY_PILL, GREY_TEXT, PILL_EDGE, PANEL = '#FFE2E0DA', '#FF55524A', '#FF6E6A62', '#FFF6F5F1'
WORLDS = ['Pipe-Rock Plateau', 'Petal Isles', 'Fluff-Puff Peaks', 'Shining Falls', 'Sunbaked Desert',
          'Fungi Mines', 'Deep Magma Bog', "Bowser's Castle", 'Special World']
BUILD = 'FF773E90972D544EB79406EAA65396D53C43EFB9000000000000000000000000'
BASE_BUILD = 'CD6E42AEE7934F4D8393CD43893AE25EDC83D338000000000000000000000000'  # 1.0.0: wrong-pipe page only
V101_BUILD = 'F91868B88F60D3D59009DB3389FDE314A6A32FCD000000000000000000000000'  # 1.0.1: wrong-pipe page only


def rect(x, y, w, h, bg, **kw):
    return {'type': 'rect', 'rect': [x, y, w, h], 'bg': bg, 'color': '#00000000', **kw}


def label(x, y, text, scale, color, **kw):
    return {'type': 'label', 'rect': [x, y, 0, 0], 'text': text, 'text_scale': scale, 'color': color, **kw}


def value(x, y, bind, scale, color, **kw):
    return {'type': 'value', 'rect': [x, y, 0, 0], 'bind': bind, 'text_scale': scale, 'color': color, **kw}


def image(x, y, w, h, src, **kw):
    return {'type': 'image', 'rect': [x, y, w, h], 'src': src, 'color': '#FFFFFFFF', **kw}


def img_fmt(x, y, w, h, bind, fmt, fallback, **kw):
    return {'type': 'image', 'rect': [x, y, w, h], 'bind': bind, 'src': fallback, 'src_format': fmt,
            'color': '#FFFFFFFF', **kw}


def glyph(x, y, d, letter, bg='#FFFFFFFF', fg=INK, ring=None, **kw):
    """A controller-button glyph: round chip with the letter in the game font. (The game draws its
    own prompts from the Switch system font, which is firmware, not game data.)"""
    scale = max(3, d * 11 // 100)
    halo = [] if ring is None else [
        {'type': 'button', 'rect': [x - 3, y - 3, d + 6, d + 6], 'bg': ring, 'color': '#00000000', 'frame': 0,
         'pill': True, **kw}]
    return halo + [
        {'type': 'button', 'rect': [x, y, d, d], 'bg': bg, 'color': fg if ring is None else bg,
         'frame': max(2, d // 16), 'pill': True, **kw},
        label(x + d // 2, y + (d - scale * 5 + 1) // 2, letter, scale, fg, align='center', _plain=True, **kw),
    ]


def hud(y=22):
    """Character + lives, gold coins, purple coins (the game's own HUD order)."""
    return [
        img_fmt(28, y, 96, 96, 'wonder.character', 'module:wonder:chara/%d', 'module:wonder:chara/0'),
        value(132, y + 26, 'wonder.lives', 8, '#FFFFFFFF', pad=2),
        image(300, y + 14, 70, 70, 'module:wonder:lyt/LCoin/IconCoin^w'),
        value(378, y + 26, 'wonder.gold', 8, '#FFFFFFFF', pad=2),
        image(560, y + 14, 70, 70, 'module:wonder:lyt/LCoinRandom/IconCoinRandom^w'),
        value(638, y + 26, 'wonder.purple', 8, '#FFFFFFFF', pad=3),
    ]


def setup_page():
    return {'id': 'setup', 'title': 'Super Mario Bros. Wonder', 'widgets': [
        rect(0, 0, W, H, CREAM),
        # Faint checker pattern on the cream lower area (as on the reference).
        {'type': 'image', 'rect': [0, 0, 310, 310], 'src': 'module:wonder:lyt/Title/CmnPtnCheckFractal^r',
         'color': '#10A89880', 'repeat': 16, 'repeat_cols': 4, 'repeat_dx': 310, 'repeat_row_dy': 310},
        # The title art at its own 16:9 shape across the top, fading into the cream.
        image(0, 0, W, 698, 'module:wonder:lyt/Title/TitleBG^w'),
        image(0, 470, W, 230, 'module:wonder:gen/fade_cream'),
        image(250, 170, 740, 444, 'module:wonder:lyt/TitleLogo/TitleLogo^w'),
        *wrong_pipe_card(),
        *fallback_card(),
    ]}


def wrong_pipe_card():
    """Shown on an older game version (1.0.0, 1.0.1): game art + font, no data. The module
    publishes the version line ("Your game is Ver. 1.0.1.")."""
    wb = {'need_bind': 'wonder.wrong_build'}
    return [
        rect(0, 0, W, H, '#CC232723', **wb),
        image(80, 150, 1080, 780, 'module:wonder:gen/card/1080x780/40/F7FFFAE5', **wb),
        image(500, 190, 240, 260, 'module:wonder:gen/pipe', **wb),
        label(620, 480, 'WRONG PIPE!', 16, INK, align='center', **wb),
        label(620, 590, 'This companion is for Super Mario Bros. Wonder Ver. 1.2.1.', 5, INK, align='center', **wb),
        {'type': 'label', 'rect': [620, 640, 0, 0], 'bind_text': 'wonder.game_version_line', 'text_scale': 5,
         'color': GREY_TEXT, 'align': 'center', **wb},
        label(620, 730, 'Install the latest game update, then come back through the right pipe.', 5,
              INK, align='center', **wb),
        label(620, 840, 'Nothing is read from the game until then.', 4, GREY_TEXT, align='center', **wb),
    ]


def fallback_card():
    """Any other unknown build: the module is not loaded (no game art or font), so this card uses
    only shapes and the runtime's built-in lettering. It sits on the first page, which is the page
    the runtime shows when nothing switches it."""
    fb = {'need_bind': 'wonder.fallback'}
    ws = [
        rect(0, 0, W, H, '#FF1E3A26'),
        rect(80, 150, 1080, 780, CREAM, pill=False),
        rect(470, 210, 300, 70, '#FF2FA84A'), rect(500, 280, 240, 190, '#FF2FA84A'),
        rect(510, 222, 30, 46, '#FFB4F5AA'), rect(530, 290, 26, 170, '#FFB4F5AA'),
        label(620, 520, 'WRONG PIPE!', 12, INK, align='center'),
        label(620, 640, 'THIS COMPANION IS FOR', 5, INK, align='center'),
        label(620, 690, 'SUPER MARIO BROS. WONDER VER. 1.2.1', 5, INK, align='center'),
        label(620, 780, 'UPDATE THE GAME, THEN COME BACK', 4, GREY_TEXT, align='center'),
    ]
    for wd in ws:
        wd.update(fb)
        if wd['type'] == 'label':
            wd['_plain'] = True
    return ws


def world_grid(x0, y0):
    out = []
    cw, ch, gx, gy = 368, 60, 28, 16  # 3*368 + 2*28 = 1160: 40 px margins both sides
    for i, name in enumerate(WORLDS):
        c, r = i % 3, i // 3
        x, y = x0 + c * (cw + gx), y0 + r * (ch + gy)
        wid = i + 1
        out.append({'type': 'button', 'rect': [x, y, cw, ch], 'bg': GREY_PILL, 'color': PILL_EDGE, 'frame': 3,
                    'pill': True, 'need_bind': f'ui.world.{wid}.off'})
        out.append({'type': 'button', 'rect': [x, y, cw, ch], 'bg': YELLOW, 'color': PILL_EDGE, 'frame': 3,
                    'pill': True, 'need_bind': f'ui.world.{wid}'})
        out.append(label(x + cw // 2, y + 16, name, 5, INK, align='center'))
    return out


def map_page():
    w = [
        image(0, 0, W, H, 'module:wonder:gen/sky'),
        img_fmt(0, 0, W, 700, 'wonder.map.course', 'module:wonder:blur/course/%03d', 'module:wonder:gen/sky',
                need_bind='wonder.map.has_course'),
        image(0, 0, W, 700, 'module:wonder:gen/shade', need_bind='wonder.map.has_course'),
        *hud(),
        {'type': 'button', 'rect': [800, 160, 400, 76], 'bg': YELLOW, 'color': INK, 'frame': 4, 'pill': True,
         'on_tap': 'open_courses'},
        label(1054, 182, 'OPEN COURSES', 6, INK, align='right', _plain=True),
        *glyph(1066, 172, 54, 'L'),
        label(48, 170, 'WORLD MAP', 6, YELLOW),
        value(48, 214, 'wonder.world', 11, '#FFFFFFFF', table='world'),
        value(48, 300, 'wonder.world_seeds', 6, YELLOW, suffix=' WONDER SEEDS'),
        image(40, 380, 1160, 210, 'module:wonder:gen/card/1160x210/26/F0FFFFFF'),
        label(76, 404, 'SELECTED COURSE', 5, '#FF8A857A'),
        {'type': 'label', 'rect': [76, 446, 1080, 0], 'bind_text': 'wonder.map.course_name', 'text_scale': 9,
         'color': INK, 'need_bind': 'wonder.map.has_course'},
        label(76, 530, 'Enter this course on the upper screen', 5, GREY_TEXT, need_bind='wonder.map.has_course'),
        label(76, 450, 'Move onto a course to see its details', 7, INK, need_bind='ui.map.no_course'),
        label(76, 520, 'The map and counters remain live', 5, GREY_TEXT, need_bind='ui.map.no_course'),
        rect(0, 660, W, 420, PANEL),
        label(48, 690, 'FLOWER KINGDOM', 6, INK),
        label(48, 734, 'World index - use Open Courses to travel in the game\'s menu', 4, GREY_TEXT),
        *world_grid(40, 790),
    ]
    return {'id': 'map', 'title': 'World map', 'widgets': w}


RAIL_X, RAIL_Y, RAIL_W = 120, 470, 1000
MARK, ICON, PLAYER = 76, 60, 104          # marker disc, marker icon, player disc (px)
TRACK = '#80FFFFFF'                       # translucent white track
DISC = '#B3FFFFFF'                        # translucent white marker discs


def rail():
    """Progress rail (start -> goal of the current area), markers and the player riding it."""
    x0, y = RAIL_X, RAIL_Y
    rv = {'need_bind': 'wonder.rail.valid'}
    white = '#FFFFFFFF'

    def disc(d, bg, edge, frame, **kw):
        return {'type': 'button', 'rect': [x0 - d // 2, y - d // 2, d, d], 'bg': bg, 'color': edge,
                'frame': frame, 'pill': True, **kw}

    def icon(src, **kw):
        return {'type': 'image', 'rect': [x0 - ICON // 2, y - ICON // 2, ICON, ICON], 'src': src,
                'color': white, **kw}

    w = [
        {'type': 'button', 'rect': [x0 - 20, y - 18, RAIL_W + 40, 36], 'bg': TRACK, 'color': '#00000000',
         'frame': 0, 'pill': True, **rv},
        label(x0, y + 58, 'START', 5, white, align='center', **rv),
        label(x0, y + 50, 'GOAL', 5, white, align='center', x_bind='wonder.rail.goal_px', **rv),
        {**disc(MARK, DISC, '#00000000', 0, **rv), 'x_bind': 'wonder.rail.goal_px'},
        {'type': 'image', 'rect': [x0 - 30, y - 30, 60, 60], 'x_bind': 'wonder.rail.goal_px',
         'src': 'module:wonder:lyt/LMotherSeed/IconMotherSeedBlank^s', 'color': '#FF6F6B74',
         'need_bind': 'ui.goal.miss'},
        {'type': 'image', 'rect': [x0 - 30, y - 30, 60, 60], 'x_bind': 'wonder.rail.goal_px', 'src': 'module:wonder:pict/SeedBlue',
         'color': '#FFFFFFFF', 'need_bind': 'ui.goal.got'},
        {'type': 'image', 'rect': [x0 + 10, y - 44, 30, 34], 'x_bind': 'wonder.rail.goal_px', 'src': 'module:wonder:pict/GoalFlag',
         'color': '#FFFFFFFF', **rv},
        # Secret exit: hidden until discovered (its seed earned); then a branch lane rises off the
        # rail before the goal and runs to the secret pole's seed.
        {'type': 'rect', 'rect': [x0 - 18, y - 118, 36, 100], 'bg': TRACK, 'color': '#00000000',
         'x_bind': 'wonder.rail.branch_px', 'need_bind': 'ui.secret.got'},
        {'type': 'rect', 'rect': [x0 + 18, y - 118, 20, 36], 'bg': TRACK, 'color': '#00000000',
         'x_bind': 'wonder.rail.branch_px', 'repeat': 60, 'repeat_dx': 20,
         'repeat_bind': 'wonder.rail.branch_n', 'need_bind': 'ui.secret.got'},
        {**disc(MARK, DISC, '#00000000', 0), 'rect': [x0 - MARK // 2, y - 100 - MARK // 2, MARK, MARK],
         'x_bind': 'wonder.rail.secret_px', 'need_bind': 'ui.secret.got'},
        {**icon('module:wonder:pict/SeedBlue'), 'rect': [x0 - ICON // 2, y - 100 - ICON // 2, ICON, ICON],
         'x_bind': 'wonder.rail.secret_px', 'need_bind': 'ui.secret.got'},
        {'type': 'label', 'rect': [x0 - 30, y - 116, 0, 0], 'text': 'SECRET', 'text_scale': 5, 'color': white,
         'align': 'right', 'x_bind': 'wonder.rail.branch_px', 'need_bind': 'ui.secret.got'},
    ]
    m = 'wonder.rail.m.{i}.'
    rep12 = {'repeat': 12, 'repeat_dx': 0}
    at = lambda k: {'x_bind': m + 'x', 'need_bind': m + k, **rep12}
    w += [
        disc(MARK, DISC, '#00000000', 0, **at('on')),
        icon('module:wonder:lyt/LBigRandomCoinIcon/IconCoinRandomBigBlank^s', color='#FF6F6B74', **at('k1miss')),
        icon('module:wonder:lyt/LBigRandomCoinIcon/IconCoinRandomBig^w', **at('k1got')),
        icon('module:wonder:pict/BlueFlower', **at('k2')),
        icon('module:wonder:lyt/LMotherSeed/IconMotherSeedBlank^s', color='#FF6F6B74', **at('k3miss')),
        icon('module:wonder:pict/SeedBlue', **at('k3got')),
        icon('module:wonder:lyt/LCrownIcon/IconFlagTopBlank^s', color='#FF6F6B74', **at('k4miss')),
        icon('module:wonder:lyt/LCrownIcon/IconFlagTop^w', **at('k4got')),
        {'type': 'label', 'rect': [x0, y + 48, 0, 0], 'text': 'CHECK', 'text_scale': 5, 'color': white,
         'align': 'center', **at('k4')},
        # The player riding the rail: white disc with a black ring, character icon inside.
        {**disc(PLAYER, white, INK, 6), 'x_bind': 'wonder.rail.px', 'y_bind': 'wonder.rail.player_dy', **rv},
        {'type': 'image', 'rect': [x0 - 42, y - 42, 84, 84], 'bind': 'wonder.character',
         'src': 'module:wonder:chara/0', 'src_format': 'module:wonder:chara/%d', 'color': white,
         'x_bind': 'wonder.rail.px', 'y_bind': 'wonder.rail.player_dy', **rv},
        # AREA nn% pill, top right on the counter row (like the map's OPEN COURSES pill)
        {'type': 'button', 'rect': [930, 42, 250, 56], 'bg': YELLOW, 'color': INK, 'frame': 3, 'pill': True, **rv},
        label(1010, 56, 'AREA', 5, INK, align='center', **rv, _plain=True),
        value(1120, 56, 'wonder.rail.pct', 5, INK, align='center', suffix='%', **rv, _plain=True),
    ]
    return w


def course_page():
    w = [
        image(0, 0, W, 700, 'module:wonder:gen/sky'),
        img_fmt(0, 0, W, 700, 'wonder.course', 'module:wonder:blur/course/%03d', 'module:wonder:gen/sky'),
        image(0, 0, W, 700, 'module:wonder:gen/shade'),
        *hud(),

        value(48, 160, 'wonder.world', 6, YELLOW, table='world_caps'),
        {'type': 'label', 'rect': [48, 200, 1150, 0], 'bind_text': 'wonder.course_name', 'text_scale': 11,
         'color': '#FFFFFFFF', 'need_bind': 'wonder.shelf'},
        {'type': 'label', 'rect': [48, 300, 0, 0], 'bind_text': 'wonder.course_line', 'text_scale': 5,
         'color': '#FFFFFFFF'},
        *rail(),
        image(0, 620, W, 110, 'module:wonder:gen/wave'),
        rect(0, 720, W, 360, CREAM),

        label(48, 740, '10-FLOWER COINS', 6, INK),
    ]
    for i in range(3):
        x = 48 + i * 150
        w.append(image(x, 800, 130, 130, 'module:wonder:lyt/LBigRandomCoinIcon/IconCoinRandomBig^w',
                       need_bind=f'wonder.flower.{i}'))
        w.append(image(x, 800, 130, 130, 'module:wonder:lyt/LBigRandomCoinIcon/IconCoinRandomBigBlank^s', color='#FF6F6B74',
                       need_bind=f'ui.flower.{i}.off'))
    w += [
        rect(520, 750, 4, 262, '#FFE0DCCF'),
        label(631, 740, 'WORLD SEEDS', 5, INK, align='center'),
        image(561, 785, 140, 140, 'module:wonder:icon/IconMotherSeed00'),
        value(631, 930, 'wonder.world_seeds', 6, INK, align='center'),
        value(631, 980, 'wonder.run_seeds', 4, GREY_TEXT, align='center', suffix=' THIS RUN'),
        label(855, 740, 'CURRENT FORM', 5, INK, align='center'),
        img_fmt(790, 790, 130, 130, 'wonder.power_up', 'module:wonder:power/%d', 'module:wonder:power/1',
                need_bind='wonder.has_power'),
        img_fmt(790, 790, 130, 130, 'wonder.character', 'module:wonder:chara/%d', 'module:wonder:chara/0',
                need_bind='ui.small'),
        value(855, 940, 'wonder.power_up', 5, GREY_TEXT, align='center', table='power_caps'),
        label(1090, 740, 'ITEM BALLOON', 5, INK, align='center'),
        # Item balloon (176 px, centre 1090,878): dark border, white ring, light and dark lavender.
        {'type': 'button', 'rect': [1002, 790, 176, 176], 'bg': '#FFFFFFFF', 'color': '#FF6B5A86', 'frame': 2,
         'pill': True},
        {'type': 'button', 'rect': [1012, 800, 156, 156], 'bg': '#FFBCB2CE', 'color': '#00000000', 'frame': 0,
         'pill': True},
        {'type': 'button', 'rect': [1042, 830, 96, 96], 'bg': '#FF9D8CB8', 'color': '#00000000', 'frame': 0,
         'pill': True},
        img_fmt(1038, 826, 104, 104, 'wonder.reserve_item', 'module:wonder:power/%d', 'module:wonder:power/1',
                need_bind='wonder.has_item'),
        *glyph(1134, 920, 46, 'A', bg=YELLOW, ring='#FFFFFFFF'),
        label(1090, 982, 'EMPTY', 5, GREY_TEXT, align='center', need_bind='wonder.item_empty'),
        label(1090, 982, 'HOLD', 5, GREY_TEXT, align='center', need_bind='wonder.has_item'),
        # Footer under the right-hand columns, clear of the separator.
        value(882, 1036, 'wonder.world', 4, GREY_TEXT, align='center', table='world_live'),
        # Courses without shelf items (unnamed prologue courses): cream + the title's checker pattern.
        rect(0, 712, W, 368, CREAM, need_bind='wonder.no_shelf'),
        {'type': 'image', 'rect': [0, 712, 310, 310], 'src': 'module:wonder:lyt/Title/CmnPtnCheckFractal^r',
         'color': '#10A89880', 'repeat': 8, 'repeat_cols': 4, 'repeat_dx': 310, 'repeat_row_dy': 310,
         'need_bind': 'wonder.no_shelf'},
        # Pause veil last so it covers everything.
        rect(0, 0, W, H, '#B3232723', need_bind='wonder.paused'),
        label(620, 500, 'PAUSED', 14, CREAM, align='center', need_bind='wonder.paused'),
    ]
    return {'id': 'course', 'title': 'Course', 'widgets': w}


def gallery_page():
    """Hidden art review page (console: page gallery): candidate icons with their keys."""
    keys = [f'icon/IconWonderFlower{n:02d}' for n in (0, 2, 4, 5, 6, 7, 8, 9, 10)] + \
           [f'icon/IconItemWonderFlower{n:02d}' for n in (0, 2, 4, 5, 6, 7, 8, 10)] + \
           ['icon/IconMotherSeed00', 'lyt/LOfferingCounterIcon/IconOfferingBlank^s',
            'lyt/LWonderFlowerMorphing/IconOfferingMono^s']
    w = [rect(0, 0, W, H, '#FF3A3F4A')]
    for i, k in enumerate(keys):
        x, y = 20 + (i % 6) * 204, 20 + (i // 6) * 262
        w.append(image(x + 22, y, 160, 160, 'module:wonder:' + k))
        w.append(label(x + 102, y + 170, k.split('/')[-1][-22:], 3, '#FFFFFFFF', align='center'))
    return {'id': 'gallery', 'title': 'Art gallery', 'widgets': w}


derived = [
    {'name': 'ui.goal.got', 'all_nonzero': ['wonder.rail.valid', 'wonder.rail.goal_got']},
    {'name': 'ui.goal.miss0', 'cmp': 'eq', 'a': 'wonder.rail.goal_got', 'b': 0},
    {'name': 'ui.goal.miss', 'all_nonzero': ['wonder.rail.valid', 'ui.goal.miss0']},
    {'name': 'ui.secret.got', 'all_nonzero': ['wonder.rail.secret', 'wonder.rail.secret_got']},
    {'name': 'ui.small', 'cmp': 'eq', 'a': 'wonder.power_up', 'b': 0},
    {'name': 'ui.map.no_course', 'cmp': 'eq', 'a': 'wonder.map.has_course', 'b': 0},
]
derived += [{'name': f'ui.flower.{i}.off', 'cmp': 'eq', 'a': f'wonder.flower.{i}', 'b': 0} for i in range(3)]
for wid in range(1, 10):
    derived.append({'name': f'ui.world.{wid}', 'cmp': 'eq', 'a': 'wonder.world', 'b': wid})
    derived.append({'name': f'ui.world.{wid}.off', 'cmp': 'ne', 'a': 'wonder.world', 'b': wid})

manifest = {
    'format': 1,
    'title_id': '010015100B514000',
    'name': 'Super Mario Bros. Wonder dual screen',
    'min_runtime': 15,  # outline / rise text (Eden Duo 1.0.2)
    'requires_module': True,
    'module': {'abi': 1, 'build_ids': [BUILD, BASE_BUILD, V101_BUILD]},
    'canvas_w': W, 'canvas_h': H,
    'background': CREAM,
    'font': 'file:wonder_font.txt',
    'font_atlas': 'module:wonder:font/Nin-SuperMarioBros.V2.0:30',
    '_note': 'Generated by tools/wonder/gen_manifest.py. All game art is decoded at runtime from the player\'s romfs.',
    'tables': {
        'world': [''] + WORLDS,
        'world_caps': [''] + [n.upper() for n in WORLDS],
        'world_live': [''] + ['LIVE - ' + n.upper() for n in WORLDS],
        'power_caps': ['SMALL', 'SUPER', 'FIRE', 'ELEPHANT', '', '', 'DRILL', '', '', 'BUBBLE'],
    },
    'derived': derived,
    'actions': {
        'open_courses': {'kind': 'button', 'button': 'L', 'frames': 8},
    },
    'page_binds': [
        {'point': 'wonder.page', 'equals': i, 'when_equal': {'page': p, 'transition': 'fade', 'duration_ms': 250}}
        for i, p in enumerate(['setup', 'course', 'map'])
    ],
    'pages': [setup_page(), course_page(), map_page()],
}
RISE_BINDS = {'wonder.lives', 'wonder.gold', 'wonder.purple', 'wonder.world_seeds'}


def text_effects(widgets):
    """The game's lettering: every label/value outlined in the opposite colour (about 10% of the
    cap height), and the HUD counters' digits stepping up glyph by glyph."""
    for wd in widgets:
        if wd.get('type') not in ('label', 'value'):
            continue
        if wd.pop('_plain', False):
            continue
        c = wd.get('color', '#FFE6ECF2')
        r, g, b = int(c[3:5], 16), int(c[5:7], 16), int(c[7:9], 16)
        light = 0.299 * r + 0.587 * g + 0.114 * b > 150
        wd['outline'] = INK if light else '#FFFFFFFF'
        wd['outline_px'] = max(2, wd.get('text_scale', 3) * 5 // 10)
        if wd.get('type') == 'value' and wd.get('bind') in RISE_BINDS:
            wd['rise'] = round(wd.get('text_scale', 3) * 0.08, 1)


for page in manifest['pages']:
    if page['id'] != 'gallery':
        text_effects(page['widgets'])
    for wd in page['widgets']:
        wd.pop('_plain', None)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'packages', 'SuperMarioWonder', 'dualscreen')
os.makedirs(out_dir, exist_ok=True)
json.dump(manifest, open(os.path.join(out_dir, 'manifest.json'), 'w'), indent=1)

# Per-build data files: a known build clears wonder.fallback; any other build (data.json) sets it.
for name, fb, note in (('FF773E90972D544E.json', 0, 'Ver. 1.2.1 (supported)'),
                       ('CD6E42AEE7934F4D.json', 0, 'Ver. 1.0.0 (module shows the wrong-pipe page)'),
                       ('F91868B88F60D3D5.json', 0, 'Ver. 1.0.1 (module shows the wrong-pipe page)'),
                       ('data.json', 1, 'any other build: no module, plain wrong-pipe page')):
    json.dump({'_game': 'Super Mario Bros. Wonder', '_build': note,
               'derived': [{'name': 'wonder.fallback', 'terms': [], 'add': fb}]},
              open(os.path.join(out_dir, name), 'w'), indent=1)
print('pages', len(manifest['pages']), 'widgets', sum(len(p['widgets']) for p in manifest['pages']))
