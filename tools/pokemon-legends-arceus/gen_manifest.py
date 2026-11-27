#!/usr/bin/env python3
"""Generate the approved companion layout and read-only native page bindings."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PKG = ROOT / 'packages/PokemonLegendsArceus'
BUILD = 'AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E000000000000000000000000'
WHOLE_ZOOM = 740 / 1080 / 3
INK, PAPER, SLATE, GOLD = '#FF334D67', '#FFF1EBD0', '#FF2E343D', '#FFC8B460'

MARKER = 'module:pla:marker'


def label(text, rect, **kw):
    return dict(type='label', rect=rect, text=text, color=INK, text_scale=5, **kw)


def baked(text, rect, cap, **kw):
    return dict(type='image', rect=rect, src=f'module:pla:text:{cap}:{rect[2]}:{rect[3]}:{text}', **kw)


def gate(name, value=1):
    return dict(hide_bind=name, keep_min=value, keep_max=value)


def button(text, rect, action, **kw):
    return dict(type='button', rect=rect, text=text, on_tap=action, text_scale=5,
                color=PAPER, bg='#FF4E5051', align='center', **kw)


def chrome(active):
    widgets = [dict(type='rect', rect=[1104, 0, 136, 1080], color=SLATE),
               dict(type='rect', rect=[1104, 0, 6, 1080], color='#FF524E47'),
               dict(type='rect', rect=[1112,0,2,1080], color='#FF5C646E'),
               # Separates the six page tabs from the two page controls below them.
               dict(type='rect', rect=[1122,754,100,3], color='#FF8A929C')]
    for i, page in enumerate(['map', 'pokemon', 'pokedex', 'satchel', 'missions', 'settings']):
        title = ''
        x = 1104 if page == active else 1116
        widgets.append(dict(type='image', rect=[x,12+i*124,118,112],
                            src='module:pla:tab:' + ('gold' if page == active else 'grey')))
        # Reduced motion (Settings) swaps the faded page change for an instant one.
        for still in [0, 1]:
            widgets.append(button(title, [x, 12 + i * 124, 112, 112],
                                  'page_' + page + ('_still' if still else ''), border=0,
                                  **gate('@flag:pla.reduce_motion', still)))
        if page == 'map':
            widgets.append(dict(type='image',rect=[x+18,28,65,79],src='module:pla:map_icon'))
        if page in ['settings','satchel','pokemon']:
            size,dx,dy = (76,13,18) if page == 'settings' else (70,16,21)
            icon = 'settings' if page == 'settings' else 'party' if page == 'pokemon' else 'bag'
            widgets.append(dict(type='image',rect=[x+dx,12+i*124+dy,size,size],
                                src='module:pla:tab_icon:'+icon))
        if page in ['pokedex','missions']:
            width,height,dx,dy = (84,68,9,22) if page == 'pokedex' else (76,76,13,18)
            widgets.append(dict(type='image',rect=[x+dx,12+i*124+dy,width,height],
                                src='module:pla:tab_icon:'+page))
    return widgets


def dock():
    widgets = []
    def rect(box, color, **kw):
        return dict(type='rect', rect=box, color=color, **kw)
    for mode, y in [('items',758),('items',890),('party',894)]:
        widgets.append(rect([12,y,1080,4], GOLD, **gate(f'pla.{mode}_armed')))
    widgets.append(dict(type='image', rect=[12,764,324,124], src_bind='pla.hero', need_bind='pla.selector_valid', tint='#FFFFFFFF'))
    widgets.append(dict(type='image', id='pla_item_cells', rect=[322,764,122,124], repeat=64,
        repeat_bind='pla.item_count', repeat_dx=108, repeat_dy=0, scroll='pla_items',
        src_bind='pla.item.{i}.image', need_bind='pla.selector_valid', tint='#FFFFFFFF'))
    widgets.append(dict(type='button', id='pla_item_hits', rect=[348,764,96,124], repeat=64,
        repeat_bind='pla.item_count', repeat_dx=108, repeat_dy=0, scroll='pla_items',
        text='', bg='#00000000', border=0, need_bind='pla.selector_valid',
        on_tap='select_item', payload='{i}', enabled_bind='pla.tap_ready'))
    widgets.append(dict(type='rect',rect=[372,774,48,6],color=INK,
        repeat=64,repeat_bind='pla.item_count',repeat_dx=108,repeat_dy=0,scroll='pla_items',
        need_bind='pla.item.{i}.pending'))
    # Fade only the icon band: numerals remain fully opaque below it.
    widgets.append(dict(type='image',rect=[1068,774,24,84],src='module:pla:edge',need_bind='pla.items_more'))
    widgets.append(dict(type='image',rect=[348,774,24,84],src='module:pla:edge:left',need_bind='pla.items_before'))
    widgets.append(baked('Party unavailable', [24,932,1040,80],23,need_bind='pla.party_missing'))
    # Exact native-font party cards keep portrait/status/HP treatment together.
    for j in range(6):
        x=12+j*182; key=f'pla.party.{j}'
        widgets.append(dict(type='image',rect=[x,900,170,168],src_bind=key+'.card',
                            need_bind=key+'.valid',tint='#FFFFFFFF'))
        widgets.append(baked('Unavailable',[x+5,974,164,48],23,need_bind=key+'.missing'))
    widgets.append(dict(type='image',rect=[0,0,56,56],src='module:pla:bag_cursor',
        x_bind='pla.party_cursor_x',y_bind='pla.party_cursor_y',
        need_bind='pla.party_cursor_valid',tint='#FFFFFFFF'))
    # Hit regions match the full plates and preserve the approved 12px gaps.
    for j in range(6):
        x=12+j*182
        widgets.append(dict(type='button',rect=[x,900,170,168],text='',bg='#00000000',border=0,
            on_tap='select_party',payload=str(j),need_bind=f'pla.party.{j}.valid',enabled_bind='pla.tap_ready'))
        widgets.append(dict(type='rect',rect=[x+4,900,64,6],color=GOLD,
            need_bind=f'pla.party.{j}.pending',**gate(f'pla.party.{j}.selected',0)))
    return widgets


def haptic_twins(page_widgets, derived):
    """Haptics On/Off (Settings, persisted flag pla.haptics), the Monster Sanctuary recipe:
    every touch widget becomes a "light" twin shown while the flag is 1 and an "off" twin
    shown while it is 0. An existing gate is folded into a derived expression."""
    out = []
    names = {d['name'] for d in derived}
    for w in page_widgets:
        if 'on_tap' not in w:
            out.append(w)
            continue
        hb = w.get('hide_bind')
        # Strength by moment (MOTION-AND-HAPTICS.md): page/tab change = click, a direct native
        # action (tap-select) = confirm, other taps = light.
        tap = w['on_tap']
        strength = ('click' if tap.startswith('page_') else
                    'confirm' if tap in ('select_item', 'select_party') else 'light')
        for on in (1, 0):
            twin = dict(w, haptic=strength if on else 'off')
            for key in ('hide_bind', 'keep_min', 'keep_max'):
                twin.pop(key, None)
            if hb is None:
                twin.update(gate('@flag:pla.haptics', on))
            else:
                assert w['keep_min'] == w['keep_max'], w
                name = f"pla.h{on}.{hb.replace('@flag:', 'f.')}.{w['keep_min']}"
                if name not in names:
                    derived.append(dict(name=name, expr=f"{'' if on else '!'}@flag:pla.haptics && {hb} == {w['keep_min']}"))
                    names.add(name)
                twin.update(gate(name, 1))
            out.append(twin)
    return out


def manifest():
    actions = {}
    for p in ['map', 'pokemon', 'pokedex', 'satchel', 'missions', 'settings']:
        actions[f'page_{p}'] = dict(kind='page', page=p, transition='fade', duration_ms=150, easing='ease_out')
        actions[f'page_{p}_still'] = dict(kind='page', page=p, transition='none')
    actions['bag_everyday'] = dict(kind='flag', flag='pla.bag_key', value=0)
    actions['bag_key'] = dict(kind='flag', flag='pla.bag_key', value=1)
    actions['bag_recipes'] = dict(kind='flag', flag='pla.bag_key', value=2)
    actions['bag_filter'] = dict(kind='flag', flag='pla.bag_filter')
    actions['bag_inspect'] = dict(kind='flag', flag='pla.bag_item', value='$payload')
    actions['tap_select'] = dict(kind='flag',flag='pla.tap_select')
    for kind in ['item','party']:
        actions[f'select_{kind}'] = dict(kind='module',action=f'select_{kind}',argument='$payload',enabled_bind='pla.tap_ready')
    actions['follow_on'] = dict(kind='flag', flag='pla.follow', value=True)
    actions['follow'] = dict(kind='flag', flag='pla.follow')
    actions['area_next'] = dict(kind='flag', flag='pla.area_override', cycle=7)
    # Owner 2026-11-27: pinch to zoom, drag to pan; one button brings the map back to the player
    # at the default zoom (the bound home view; the runtime glides back to it).
    actions['map_reset'] = dict(kind='view_reset', view='pla_map', enabled_bind='pla.map_valid')
    widgets = [dict(type='rect', rect=[4,4,1096,756], color='#FF524E47'),
               dict(type='rect', rect=[12,12,1080,740], color=PAPER),
        dict(type='map', id='pla_map', rect=[12,12,1080,740],
            area_bind='pla.area', area_label=False,
            marker_x_bind='pla.map_x', marker_y_bind='pla.map_y',
            marker_src=MARKER, marker_size=[64,64], marker_anchor=[.5,.5], marker_rotate_bind='pla.heading',
            pan_zoom=True, min_zoom=WHOLE_ZOOM, max_zoom=9, view_idle_ms=0,
            view_rect_x0_bind='pla.view_x0', view_rect_y0_bind='pla.view_y0',
            view_rect_x1_bind='pla.view_x1', view_rect_y1_bind='pla.view_y1',
            need_bind='pla.map_valid'),
        baked('Choose area manually', [80,270,944,100],37, need_bind='pla.position_valid', **gate('pla.map_valid',0)),
        button('Area: Auto / Village / areas 1 to 5', [48,400,992,112], 'area_next', need_bind='pla.position_valid', **gate('pla.area_valid',0)),
        baked('Unavailable', [80,270,944,100],37, need_bind='pla.map_unavailable')]
    widgets.insert(3, dict(type='image', rect=[4,4,1096,756], src='module:pla:map_frame'))
    widgets += chrome('map')
    for follow in [0,1]:
        widgets.append(dict(type='image', rect=[1110,768,124,112],
            src='module:pla:control:' + ('on' if follow else 'off'), **gate('@flag:pla.follow',follow)))
        widgets.append(button('', [1110,768,124,112], 'follow_on' if follow else 'follow',
                              on_hold='follow', hold_ms=600, enabled_bind='pla.position_valid',
                              **gate('@flag:pla.follow', follow)))
        widgets[-1].update(bg='#00000000', border=0)
        widgets.append(dict(type='image', rect=[1140,792,64,64], src=MARKER,
                            **gate('@flag:pla.follow', follow)))

    widgets.append(dict(type='image', rect=[1110,892,124,112], src='module:pla:control:off', need_bind='pla.map_valid'))
    widgets.append(dict(type='image', rect=[1130,906,84,84], src='module:pla:recentre', need_bind='pla.map_valid'))
    widgets.append(button('', [1110,892,124,112], 'map_reset'))
    widgets[-1].update(bg='#00000000', border=0, need_bind='pla.map_valid')
    widgets += dock()
    widgets.append(dict(type='image', rect=[24,24,600,48], src_bind='pla.area_title', **gate('pla.area_title_visible')))
    settings = [dict(type='image',rect=[0,0,1104,1080],src='module:pla:settings_page',tint='#FFFFFFFF')]
    for row,flag,prefix in [(0,'pla.follow','settings_follow'),(1,'pla.reduce_motion','settings_motion'),
                            (2,'pla.haptics','settings_haptics'),
                            (3,'pla.show_selector','settings_selector')]:
        y=36+124*row
        for on in [0,1]:
            settings.append(dict(type='image',rect=[712,y+30,350,52],src=f'module:pla:toggle:{on}',
                                 tint='#FFFFFFFF',**gate('@flag:'+flag,on)))
        settings += [dict(type='button',rect=[712+180*opt,y,170,112],on_tap=action,
                          text='',bg='#00000000',border=0)
                     for opt,action in enumerate([prefix+'_on',prefix+'_off'])]
    settings += chrome('settings')
    settings += [dict(type='image',rect=[1124,800,96,96],src='module:pla:settings_emblem',tint='#FFFFFFFF')]
    satchel = [dict(type='image',rect=[0,0,1104,1080],src_bind='pla.recipes.image',
                    tint='#FFFFFFFF',**gate('@flag:pla.bag_key',2)),
               dict(type='rect',rect=[12,112,1080,488],color='#00000000',
                    on_swipe_up='rec_scroll_down',on_swipe_down='rec_scroll_up',swipe_px=60,
                    **gate('@flag:pla.bag_key',2)),
               dict(type='image',rect=[164,812,928,1600],src_bind='pla.recipes.body_image',
                    scroll='pla_recipe_body',need_bind='pla.recipes.body',tint='#FFFFFFFF',
                    **gate('@flag:pla.bag_key',2))]
    for j in range(5):
        satchel.append(button('',[12,112+100*j,1080,100],'rec_row',payload=str(j),border=0,
                              **gate('@flag:pla.bag_key',2)))
        satchel[-1]['bg']='#00000000'
    actions['rec_row']=dict(kind='module',action='rec_row',argument='$payload')
    actions['rec_scroll_up']=dict(kind='module',action='rec_scroll',argument=0)
    actions['rec_scroll_down']=dict(kind='module',action='rec_scroll',argument=1)
    for key_mode in [0, 1, 2]:
        satchel.append(dict(type='image',rect=[12,12,1080,88],
            src=f'module:pla:bag_tabs:{key_mode}',tint='#FFFFFFFF',
            **gate('@flag:pla.bag_key',key_mode)))
        for index, action in enumerate(['bag_everyday', 'bag_key', 'bag_recipes']):
            satchel.append(button('', [12 + index*360,12,360,88], action,
                **gate('@flag:pla.bag_key', key_mode)))
            satchel[-1].update(bg='#00000000',border=0)
    satchel += [dict(type='image',rect=[12,112,528,88],repeat=100,repeat_bind='pla.bag_count',
                    repeat_cols=2,repeat_dx=546,repeat_row_dy=100,scroll='pla_bag',
                    src_bind='pla.bag.{i}.image',on_tap='bag_inspect',payload='$pla.bag.{i}.id',
                    need_bind='pla.bag_valid',tint='#FFFFFFFF'),
                dict(type='image',rect=[0,0,56,56],src='module:pla:bag_cursor',
                    x_bind='pla.bag_cursor_x',y_bind='pla.bag_cursor_y',scroll='pla_bag',
                    repeat=1,need_bind='pla.bag_cursor_valid',tint='#FFFFFFFF'),
                dict(type='image',rect=[12,812,1080,256],src_bind='pla.bag_detail',need_bind='pla.bag_valid',tint='#FFFFFFFF'),
                baked('Satchel unavailable', [48,332,992,80],23,need_bind='pla.bag_unavailable'),
                baked('No items', [48,332,992,80],23,need_bind='pla.bag_empty'),
                ] + chrome('satchel')
    satchel += [dict(type='image',rect=[1144,782,56,56],src='module:pla:bag_capacity',tint='#FFFFFFFF',need_bind='pla.bag_capacity_valid'),
                dict(type='label',rect=[1172,832,62,60],bind_text='pla.bag_capacity',text_scale=5,color=PAPER,align='center',need_bind='pla.bag_capacity_valid')]
    for active in [0,1]:
        satchel.append(dict(type='image',rect=[1110,892,124,112],src=f'module:pla:bag_filter:{active}',
            on_tap='bag_filter',need_bind='pla.bag_filter_available',tint='#FFFFFFFF',
            **gate('pla.bag_filter_on',active)))
    pokemon = [dict(type='image',rect=[0,0,1104,1080],src_bind='pla.pokemon.image',tint='#FFFFFFFF')]
    for j in range(6):
        pokemon.append(button('',[36,12+176*j,416,164],'pokemon_inspect',
            payload=str(j),enabled_bind=f'pla.pokemon.row.{j}',border=0))
        pokemon[-1]['bg']='#00000000'
    for j,name in enumerate(('stats','moves','weak')):
        pokemon.append(button('',[480+198*j,420,198,88],'pokemon_'+name,border=0))
        pokemon[-1]['bg']='#00000000'
    for j in range(4):
        pokemon.append(button('',[480,518+100*j,596,88],'pokemon_move',
            payload=str(j),need_bind='pla.pokemon.moves',border=0))
        pokemon[-1]['bg']='#00000000'
    pokemon.append(dict(type='image',rect=[480,522,596,1600],src_bind='pla.pokemon.weak_image',
        scroll='pla_weak',need_bind='pla.pokemon.weak',tint='#FFFFFFFF'))
    pokemon += chrome('pokemon')
    actions['pokemon_inspect']=dict(kind='flag',flag='pla.pokemon_slot',value='$payload')
    actions['pokemon_move']=dict(kind='flag',flag='pla.pokemon_move',value='$payload')
    for j,name in enumerate(('stats','moves','weak')):
        actions['pokemon_'+name]=dict(kind='flag',flag='pla.pokemon_tab',value=j)
    pokedex = [dict(type='image',rect=[0,0,1104,1080],
                    src_bind='pla.pokedex.image',tint='#FFFFFFFF'),
               # Swipe the list to page it; taps on rows still select.
               dict(type='rect',rect=[36,136,416,927],color='#00000000',
                    on_swipe_up='dex_scroll_down',on_swipe_down='dex_scroll_up',swipe_px=60)]
    for j,box in enumerate([[12,12,324,112],[348,12,366,112],[726,12,366,112]]):
        pokedex.append(button('',box,'dex_area',payload=str(j),border=0,need_bind='pla.pokedex.valid'))
        pokedex[-1]['bg']='#00000000'
    for j in range(9):
        pokedex.append(button('',[36,136+103*j,416,103],'dex_row',payload=str(j),border=0,need_bind='pla.pokedex.valid'))
        pokedex[-1]['bg']='#00000000'
    for j in range(3):
        pokedex.append(button('',[484+196*j,412,196,88],'dex_tab',payload=str(j),border=0,need_bind='pla.pokedex.valid'))
        pokedex[-1]['bg']='#00000000'
    pokedex.append(dict(type='image',rect=[480,522,596,1600],src_bind='pla.pokedex.body_image',
        scroll='pla_dex_body',need_bind='pla.pokedex.body',tint='#FFFFFFFF'))
    pokedex += chrome('pokedex')
    for kind,y,flag,action in [('dex_sort',768,'pla.pokedex.sort','dex_sort'),
                               ('dex_unrep',892,'pla.pokedex.unreported','dex_unreported')]:
        for lit in [0,1]:
            pokedex.append(dict(type='image',rect=[1110,y,124,112],src=f'module:pla:control2:{kind}:{lit}',
                                tint='#FFFFFFFF',need_bind='pla.pokedex.valid',**gate(flag,lit)))
        pokedex.append(button('',[1110,y,124,112],action,border=0,need_bind='pla.pokedex.valid'))
        pokedex[-1]['bg']='#00000000'
        actions[action]=dict(kind='module',action=action,argument=0)
    for name in ['dex_row','dex_tab','dex_area']:
        actions[name]=dict(kind='module',action=name,argument='$payload')
    actions['dex_scroll_up']=dict(kind='module',action='dex_scroll',argument=0)
    actions['dex_scroll_down']=dict(kind='module',action='dex_scroll',argument=1)
    missions = [dict(type='image',rect=[0,0,1104,1080],
                    src_bind='pla.missions.image',tint='#FFFFFFFF'),
                dict(type='rect',rect=[48,136,392,800],color='#00000000',
                     on_swipe_up='mis_scroll_down',on_swipe_down='mis_scroll_up',swipe_px=60)] + chrome('missions')
    for request in [0,1]:
        actions['missions_category_'+str(request)] = dict(kind='module',action='mis_category',argument=request)
        missions.append(button('',[16+540*request,0,532,112],
                               'missions_category_'+str(request),border=0))
        missions[-1]['bg']='#00000000'
    for j in range(8):
        missions.append(button('',[48,136+100*j,392,100],'mis_row',payload=str(j),border=0,need_bind='pla.missions.valid'))
        missions[-1]['bg']='#00000000'
    for lit in [0,1]:
        missions.append(dict(type='image',rect=[1110,892,124,112],src=f'module:pla:control2:mis_active:{lit}',
                             tint='#FFFFFFFF',need_bind='pla.missions.valid',**gate('pla.missions.active_only',lit)))
    missions.append(button('',[1110,892,124,112],'mis_active',border=0,need_bind='pla.missions.valid'))
    missions[-1]['bg']='#00000000'
    actions['mis_active']=dict(kind='module',action='mis_active',argument=0)
    actions['mis_row']=dict(kind='module',action='mis_row',argument='$payload')
    actions['mis_scroll_up']=dict(kind='module',action='mis_scroll',argument=0)
    actions['mis_scroll_down']=dict(kind='module',action='mis_scroll',argument=1)
    for widget in widgets:
        if widget.get('on_tap','').startswith('follow') or widget.get('src','').startswith(('module:pla:control:', 'module:pla:marker')):
            widget['need_bind'] = 'pla.map_valid'
    battle = [dict(type='image',rect=[0,0,1104,1080],src_bind='pla.battle.image',tint='#FFFFFFFF')]
    for j in range(4):
        battle.append(button('',[56,176+100*j,1024,100],'battle_move',payload=str(j),border=0,
                             need_bind='pla.battle.moves'))
        battle[-1]['bg']='#00000000'
    battle += chrome('map')
    battle.append(dict(type='image',rect=[1110,768,124,296],src_bind='pla.battle.legend',
                       tint='#FFFFFFFF',need_bind='pla.battle.moves'))
    actions['battle_move']=dict(kind='flag',flag='pla.battle_move',value='$payload')
    # LEGIBILITY dense cells: 12 px between neighbouring hit rects (pictures unchanged).
    rows = {'rec_row', 'dex_row', 'mis_row', 'battle_move'}
    tabs = {'bag_everyday', 'bag_key', 'bag_recipes', 'pokemon_stats', 'pokemon_moves',
            'pokemon_weak', 'dex_tab', 'missions_category_0', 'missions_category_1'}
    for page_widgets in [satchel, pokemon, pokedex, missions, battle]:
        for w in page_widgets:
            tap = w.get('on_tap', '')
            if w.get('type') != 'button' or w.get('_gap'):
                continue
            x, y, rw, rh = w['rect']
            if tap in rows:
                w['rect'] = [x, y + 6, rw, rh - 12]
            elif tap in tabs:
                inset = 6 if not tap.startswith('missions_category') else 2
                w['rect'] = [x + inset, y, rw - 2 * inset, rh]
            else:
                continue
            w['_gap'] = True
    for page_widgets in [satchel, pokemon, pokedex, missions, battle]:
        for w in page_widgets:
            w.pop('_gap', None)
    for page, title in [('map','Map'), ('settings','Settings'), ('satchel','Satchel'), ('pokemon','Pokemon'), ('pokedex','Pokedex'), ('missions','Missions'), ('battle','Battle')]:
        for w in {'map':widgets,'satchel':satchel,'pokemon':pokemon,'settings':settings,'pokedex':pokedex,'missions':missions,'battle':battle}[page]:
            if w.get('on_tap', '').startswith('page_'):
                w['bg'] = '#00000000'
                w['color'] = '#FF272D34'
    # Loading (approved state): the page stays, dimmed under a paper scrim with the Arc Phone
    # emblem at the map centre; touches are absorbed so nothing stale can be acted on.
    for page_widgets in [widgets, settings, satchel, pokemon, pokedex, missions, battle]:
        page_widgets += [dict(type='rect',rect=[0,0,1240,1080],color='#B8F1EBD0',input_block=True,
                              **gate('pla.state',2)),
                         dict(type='image',rect=[556,318,128,128],src='module:pla:load_emblem',
                              tint='#FFFFFFFF',**gate('pla.state',2))]
    # Title / save select and wrong version are full-screen overlays on every page: a page bind
    # only records a baseline on its first read (mod_pages.cpp), so it cannot open a page for a
    # state the game starts in. Both absorb touches.
    title_overlay = [dict(type='image',rect=[0,0,1240,1080],src='module:pla:paper',input_block=True,
                          **gate('pla.state',1)),
                     dict(type='image',rect=[320,300,600,440],src='module:pla:title_logo',tint='#FFFFFFFF',
                          **gate('pla.state',1))]
    # Wrong version: the host refuses the module and publishes module_error, so everything here is
    # host-drawn or read straight from the player's own game files (logo); host font is the allowed
    # fallback.
    logo = 'romfs:/bin/appli/title_demo/bin/title_demo_01_eng.arc#timg/__Combined.bntx#grptxt_title_logo_00^u'
    def version_widgets():
        # Host text is left-anchored at rect x for a stable result; the pixel font advances about
        # 4 * scale px per character (measured live: 22 chars at scale 5 = 434 px), so each line
        # starts at 620 - width / 2 to sit centred on the page.
        def centred(text, y, scale, color=INK):
            width = 4 * scale * len(text)
            return dict(type='label', rect=[620 - width // 2, y, width + 8, scale * 7], text=text,
                        color=color, text_scale=scale, align='left')
        return [dict(type='rect',rect=[0,0,1240,1080],color=PAPER,input_block=True),
                dict(type='rect',rect=[24,24,1192,1032],color='#FFE7DFC0'),
                dict(type='rect',rect=[33,33,1174,1014],color=PAPER),
                dict(type='image',rect=[420,120,400,293],src=logo,tint='#FFFFFFFF'),
                dict(type='rect',rect=[380,470,480,150],color='#FFF7F1DC'),
                centred('Supported game version', 492, 5),
                centred('1.1.1', 552, 8),
                dict(type='rect',rect=[260,690,720,84],color=SLATE),
                centred('Update the game to 1.1.1', 714, 5, PAPER),
                centred('The companion stays off on other versions.', 830, 5)]
    for page_widgets in [widgets, settings, satchel, pokemon, pokedex, missions, battle]:
        page_widgets += title_overlay
        page_widgets += [dict(w, **gate('module_error',1)) for w in version_widgets()]
    version_page = version_widgets()
    for w in version_page:
        if w['type'] == 'rect':
            w['bg'], w['color'] = w['color'], '#00000000'
    for page_widgets in [widgets, settings, satchel, pokemon, pokedex, missions, battle]:
        page_widgets.insert(0, dict(type='image',rect=[0,0,1240,1080],src='module:pla:paper'))
        for widget in page_widgets:
            # Rows and slot cards change identity, so they must not show the previous picture
            # while the new one decodes. Whole pages and scroll bodies keep the runtime's hold:
            # the old picture stays until the new one lands, so a tap or scroll never blanks the
            # page (owner on the Thor, 2026-11-27: blips on Pokedex/Battle/Missions).
            if widget['type'] == 'image' and 'src_bind' in widget:
                bind = widget['src_bind']
                if '{i}' in bind or bind.startswith(('pla.party.', 'pla.item.')) or bind == 'pla.hero':
                    widget['src_hold'] = False
            if widget['type'] == 'rect':
                widget['bg'], widget['color'] = widget['color'], '#00000000'
    actions['settings_follow_on']=dict(kind='flag',flag='pla.follow',value=1)
    actions['settings_follow_off']=dict(kind='flag',flag='pla.follow',value=0)
    actions['settings_haptics_on']=dict(kind='flag',flag='pla.haptics',value=1)
    actions['settings_haptics_off']=dict(kind='flag',flag='pla.haptics',value=0)
    actions['settings_motion_on']=dict(kind='flag',flag='pla.reduce_motion',value=1)
    actions['settings_motion_off']=dict(kind='flag',flag='pla.reduce_motion',value=0)
    actions['settings_selector_on']=dict(kind='flag',flag='pla.show_selector',value=1)
    actions['settings_selector_off']=dict(kind='flag',flag='pla.show_selector',value=0)
    derived=[]
    for page_widgets in [widgets, settings, satchel, pokemon, pokedex, missions, battle]:
        page_widgets[:] = haptic_twins(page_widgets, derived)
    return dict(format=1, title_id='01001F5010DFA000', name='Pokemon Legends Arceus companion',
        version='1.0.0', min_runtime=25, requires_module=True, canvas_w=1240, canvas_h=1080,
        background=PAPER, nav=False, module_tick_hidden=True,
        load_plan='guest/<build>/load-plan.json',load_plan_module='main',
        module=dict(abi=1, build_ids=[BUILD[:16]], libraries={}),
        font='romfs:/bin/font/FOT-UDKakugoC80Pro-DB.BFOTF', font_atlas='module:pla:font',
        preload=['module:pla:pokemon_static', 'module:pla:pokedex_unavailable',
                 'module:pla:missions_unavailable:0', 'module:pla:missions_unavailable:1',
                 'module:pla:settings_page','module:pla:toggle:0','module:pla:toggle:1',
                 'module:pla:title_logo','module:pla:load_emblem','module:pla:recentre',
                 'module:pla:settings_emblem',
                 'module:pla:control2:mis_active:0','module:pla:control2:mis_active:1',
                 'module:pla:control2:dex_sort:0','module:pla:control2:dex_sort:1',
                 'module:pla:control2:dex_unrep:0','module:pla:control2:dex_unrep:1',
                 'module:pla:bag_static','module:pla:bag_cursor','module:pla:bag_capacity',
                 'module:pla:bag_filter:0','module:pla:bag_filter:1',
                 'module:pla:bag_tabs:0','module:pla:bag_tabs:1','module:pla:bag_tabs:2',
                 # Warm-up last: page caches for Pokedex, Battle and Missions (no picture).
                 'module:pla:warm:dex','module:pla:warm:battle','module:pla:warm:missions'],
        flags={'pla.follow': True, 'pla.area_override': 0, 'pla.tap_select': True, 'pla.bag_key': False, 'pla.bag_filter': False, 'pla.bag_item': 0, 'pla.pokemon_slot': 0, 'pla.pokemon_tab': 0, 'pla.pokemon_move': 0, 'pla.haptics': 1, 'pla.show_selector': 0, 'pla.reduce_motion': 0, 'pla.battle_move': 0}, persist_flags=['pla.follow', 'pla.area_override', 'pla.tap_select', 'pla.haptics', 'pla.show_selector', 'pla.reduce_motion'],
        derived=derived,
        # Steward 2026-11-18: every global kind off; each touch widget has a "light" twin shown
        # while pla.haptics is on and an "off" twin otherwise (swipes stay silent).
        haptics=dict(enabled=True, respect_system=True, **{k: 'off' for k in
            ['tap','write','select','drag','drop','marker','refused','hold','swipe']}),
        map=dict(zone_area={str(i):str(i) for i in [0,1,2,3,4,5]}, areas={str(i): dict(min=[0,0], max=[2048,2048], clamp_view=True,
                                    image=f'module:pla:map:{i}') for i in [0,1,2,3,4,5]}),
        page_binds=[dict(point='module_error', equals=1, when_equal=dict(page='version')),
                    # A turn battle takes over the Map tab and gives it back when it ends.
                    dict(point='pla.battle.active', equals=1,
                         when_equal=dict(page='battle', transition='fade', duration_ms=150),
                         when_not_equal=dict(page='map', transition='fade', duration_ms=150))],
        pages=[dict(id='map', title='Map', widgets=widgets, scrolls=[dict(id='pla_items', rect=[322,764,770,124], axes='x', content_w_bind='pla.item_width', content_h=124, show_bind='pla.selector_valid', fling=True, bar='#00000000')]),
               dict(id='pokemon', title='Pokemon', widgets=pokemon,scrolls=[dict(id='pla_weak',rect=[480,522,596,546],axes='y',content_w=596,content_h_bind='pla.pokemon.weak_height',show_bind='pla.pokemon.weak',fling=True)]),
               dict(id='pokedex', title='Pokedex', widgets=pokedex,scrolls=[dict(id='pla_dex_body',rect=[480,522,596,546],axes='y',content_w=596,content_h_bind='pla.pokedex.body_height',show_bind='pla.pokedex.body',fling=True)]),
               dict(id='satchel', title='Satchel', widgets=satchel,scrolls=[dict(id='pla_bag',rect=[0,112,1088,688],axes='y',content_w=1088,content_h_bind='pla.bag_height',reset_bind='@flag:pla.bag_key',bar='#FF788088',bar_w=6,fling=True),dict(id='pla_recipe_body',rect=[164,812,928,256],axes='y',content_w=928,content_h_bind='pla.recipes.body_height',show_bind='pla.recipes.body',fling=True)]),
               dict(id='missions', title='Missions', widgets=missions),
               dict(id='settings', title='Settings', widgets=settings),
               dict(id='battle', title='Battle', widgets=battle),
               dict(id='version', title='Version', widgets=version_page)],
        actions=actions)


if __name__ == '__main__':
    PKG.joinpath('dualscreen/manifest.json').write_text(json.dumps(manifest(), indent=2)+'\n')
    PKG.joinpath('package.json').write_text(json.dumps(dict(format=1, type='dual-screen-mod',
        title_id='01001F5010DFA000', name='Pokemon Legends Arceus companion', version='1.0.0',
        requires_module=True, min_runtime=25, module=dict(abi=1,build_ids=[BUILD[:16]],libraries={})), indent=2)+'\n')
