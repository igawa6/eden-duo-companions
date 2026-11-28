#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Sickops
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes packages/PokemonLegendsZA/dualscreen/manifest.json.

Eight tabs on a 1240x1080 logical canvas. Values come from the player's
live game state or save via the native module; names, icons and the map come from their romfs.
The UI font is decoded at runtime from the player's romfs via za_font.cpp (font/za_font.txt is a non-empty placeholder).

  header   0..108
  content  120..950
  tab bar  958..1080

The map shows live world coordinates on verified build 2.0.2, with save fallback. The outdoor player marker uses measured world-to-texture calibration.

Run from anywhere: python tools/za/gen_manifest.py
"""
import json
import math
import struct
import zlib
import hashlib
import sys
from localize_manifest import localize
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "packages/PokemonLegendsZA/dualscreen/manifest.json"
if "--output" in sys.argv:
    OUT = Path(sys.argv[sys.argv.index("--output") + 1])

W, H = 1240, 1080
MAP_MAX_ZOOM = 12  # Match kMapMaxZoom in the native module.
RUNTIME_25 = "--runtime25" in sys.argv
LEGACY_NAV = RUNTIME_25 or "--legacy-nav" in sys.argv

# Native Z-A .bflyt palette (AARRGGBB)
BG = "#FF101422"
HEADER = "#FF1E2740"
LINE = "#FF384466"
LINE_HI = "#FF566A98"
PANEL = "#FF191F32"
PANEL_HI = "#FF212941"
WELL = "#FF0C101C"
RED = "#FF2FB3EB"
CYAN_LIGHT = "#FFABE4FF"
LIME = "#FFB5D64D"
GOLD = "#FFF4C94E"
MAGENTA = "#FFF20D77"
PINK_GLOW = "#FFFF4A95"
TEAL_MINT = "#FF25C5C2"
TEXT = "#FFF8F9FC"
DIM = "#FF95A4C4"
GREEN = "#FF45D986"
BLUE = "#FF2FB3EB"
INK_DARK = "#FF161B2E"
CLEAR = "#00000000"

# the game's box and party geometry (za_save.h)
BOX_SLOTS = 30
PARTY_SLOTS = 6

LANGUAGES = ["-", "JAP", "ANG", "FRA", "ITA", "ALL", "-", "ESP", "COR", "CHS", "CHT"]
GENDERS = ["♂", "♀", "—"]

TABS = [("team", "ÉQUIPE"), ("dex", "POKÉDEX"), ("box", "BOÎTE"),
        ("map", "CARTE"), ("bag", "SAC"), ("missions", "MISSIONS"), ("progress", "PROGRESSION"),
        ("notebook", "Carnet")]
PAGE_TITLE = dict(TABS)


# ---- small builders ------------------------------------------------------------------------

def rect(x, y, w, h, bg=CLEAR, color=CLEAR, frame=2, **kw):
    out = {"type": "rect", "rect": [x, y, w, h], "bg": bg, "color": color}
    if frame != 2:
        out["frame"] = frame
    out.update(kw)
    return out


def panel(x, y, w, h, bg=PANEL, edge=LINE):
    return rect(x, y, w, h, bg=bg, color=edge, frame=2)


def label(x, y, text, scale, color=TEXT, align="left", **kw):
    out = {"type": "label", "rect": [x, y, 0, scale * 5], "text": text,
           "text_scale": scale, "color": color, "align": align}
    out.update(kw)
    return out


def value(x, y, bind, scale, color=TEXT, align="left", **kw):
    out = {"type": "value", "rect": [x, y, 0, scale * 5], "bind": bind,
           "text_scale": scale, "color": color, "align": align}
    out.update(kw)
    return out


def bound_text(x, y, bind, scale, color=TEXT, align="left", **kw):
    return label(x, y, "", scale, color, align, bind_text=bind, **kw)


def image(x, y, w, h, **kw):
    return {"type": "image", "rect": [x, y, w, h], **kw}


def bar(x, y, w, h, bind, color=BLUE, max_bind=None, maxv=None, bg=WELL, frame=0, **kw):
    out = {"type": "bar", "rect": [x, y, w, h], "bind": bind, "color": color, "bg": bg,
           "frame": frame}
    if max_bind:
        out["max_bind"] = max_bind
    if maxv is not None:
        out["max"] = maxv
    out.update(kw)
    return out


def button(x, y, w, h, text, action, wid=None, scale=4, bg=PANEL_HI, color=TEXT, **kw):
    out = {"type": "button", "rect": [x, y, w, h], "text": text, "text_scale": scale,
           "align": "center", "bg": bg, "color": color, "border": 2, "on_tap": action}
    if wid:
        out["id"] = wid
    out.update(kw)
    return out


def header(page_id):
    """A quiet title strip. Loading/save diagnostics belong in the content, not the header."""
    return [
        rect(0, 0, W, 108, bg=HEADER),
        image(1, 1, 1, 1, src="module:ready"),
        rect(0, 104, W, 4, bg=RED, frame=0),
        label(28, 18, "LÉGENDES POKÉMON Z-A", 6,"#FF24364B"),
        label(30, 66, PAGE_TITLE[page_id], 3, "#FF426C80"),
        bound_text(1212, 28, "za.ot", 5, "#FF24364B",align="right"),
    ]


def not_ready():
    """A panel over the content while no save has been read yet (the tab bar stays usable)."""
    gate = {"need_bind": "!za.ready"}
    return [
        rect(0, 0, W, 958, bg="#F0070A11", frame=0, input_block=True, **gate),
        label(620, 360, "SAUVEGARDE NON LUE", 6, RED, align="center", **gate),
        label(620, 430, "Z-A 2.0.2 requis pour le suivi en direct.", 4, TEXT,
              align="center", **gate),
        bound_text(620, 490, "za.status", 4, DIM, align="center", **gate),
    ]


def nav_order(*extra):
    return ["nav_shortcut_0","nav_shortcut_1","nav_shortcut_2","nav_more",
            "nav_edit",*[f"nav_slot_{i}" for i in range(3)],
            *[f"nav_pick_{i}" for i in range(1,9)],*extra]


# ---- EQUIPE ---------------------------------------------------------------------------------

# Official Pokémon Legends: Z-A vibrant type palette with crisp white text (dark only on bright Electric)
TYPE_COLORS = ["#FF868E98", "#FFD24236", "#FF6898E6", "#FF984AC0", "#FFBF8638", "#FFA69244",
               "#FF82A622", "#FF6854A8", "#FF6090A8", "#FFE66224", "#FF2B8AE8", "#FF3AA64A",
               "#FFF0C628", "#FFE4528A", "#FF3CB2CC", "#FF5266D4", "#FF5A5064", "#FFE06EB8"]
TYPE_TEXT = ["#FF121826" if i == 12 else "#FFFFFFFF" for i in range(len(TYPE_COLORS))]


def type_badge(x, y, w, bind, scale=5, **gate):
    """Unknown types are -1. A single-type Pokémon has type2=-1, so no false Normal badge."""
    out=[]
    h = 42
    for i, color in enumerate(TYPE_COLORS):
        out += [rect(x, y, w, h, bg=color, color="#40FFFFFF", frame=1, pill=True,
                     hide_bind=bind, keep_min=i, keep_max=i, **gate),
                rect(x+6, y+6, 30, 30, bg="#38080D1A", frame=0, pill=True,
                     hide_bind=bind, keep_min=i, keep_max=i, **gate),
                image(x+10, y+10, 22, 22, src=f"module:type/{i}",
                      hide_bind=bind, keep_min=i, keep_max=i, **gate),
                bound_text(x+42+(w-48)//2, y, f"za.type{i}.name", scale, TYPE_TEXT[i], align="center",
                      wrap_width=w-54, max_lines=1, fit_text=True, text_center_h=h,
                      hide_bind=bind, keep_min=i, keep_max=i, **gate)]
    return out


def move_type_badge(x,y,w,bind,**gate):
    valid=dict(hide_bind=bind,keep_min=0,keep_max=17)
    out=[rect(x,y,w,40,bg="#FFFFFFFF",frame=0,tint_bind=bind,tint_colors=TYPE_COLORS,**valid,**gate),
         image(x+5,y+5,30,30,src_format="module:type/%d",bind=bind,**valid,**gate)]
    for i in range(len(TYPE_COLORS)):
        out.append(bound_text(x+44,y+8,f"za.type{i}.name",5,TYPE_TEXT[i],wrap_width=w-56,max_lines=1,fit_text=True,
                         hide_bind=bind,keep_min=i,keep_max=i,**gate))
    return out


def team_page():
    w=header("team")
    for i in range(PARTY_SLOTS):
        x=24+i*200
        present={"need_bind":f"za.p{i}.v"}
        w += [rect(x,126,188,304,bg=PANEL_HI,color=LINE,frame=2),
              image(x+14,138,160,160,src_format="module:icon/%d",bind=f"za.p{i}.icon",**present),
              bound_text(x+14,304,f"za.p{i}.name",4,TEXT,wrap_width=160,max_lines=1,fit_text=True,**present),
              bar(x+14,340,160,13,f"za.p{i}.hp",GREEN,max_bind=f"za.p{i}.hpmax",**present),
              bar(x+14,364,160,7,f"za.p{i}.xp","#FF36B8EA",max_bind=f"za.p{i}.xpmax",**present),
              value(x+14,386,f"za.p{i}.lvl",5,TEXT,text="N. ",**present),
              value(x+170,392,f"za.p{i}.g",3,DIM,names=GENDERS,align="right",**present),
              label(x+94,224,"VIDE",3,DIM,align="center",hide_bind=f"za.p{i}.v",hide_eq=1),
              rect(x,126,188,304,color=GOLD,frame=4,hide_bind="za.team.selected",keep_min=i,keep_max=i),
              button(x,126,188,304,"","team_select",wid=f"party_{i}",bg=CLEAR,color=CLEAR,border=0,payload=i,**present)]
    w += [panel(24,448,1188,496,bg="#FF202A44"),rect(24,448,1188,64,bg="#FFE8EEF7",frame=0),
          bound_text(48,460,"za.sel.name",7,"#FF202738",wrap_width=760,max_lines=1,fit_text=True),
          value(1188,466,"za.sel.lvl",5,"#FF202738",text="N. ",align="right"),
          bar(48,532,330,20,"za.sel.hp",GREEN,max_bind="za.sel.hpmax"),
          value(404,526,"za.sel.hp",5,TEXT,max_bind="za.sel.hpmax",max_sep=" / "),
          value(1188,565,"za.sel.nature",3,DIM,table="natures",align="right"),
          button(1084,832,104,104,"","send_out",wid="send_out",scale=5,bg=RED),image(1104,852,64,64,src="file:shapes/send-arrow.png"),bound_text(48,876,"za.send.status",4,DIM,wrap_width=770,max_lines=1),bound_text(48,908,"za.live.status",4,DIM,wrap_width=770,max_lines=2,line_gap=2)]
    w += type_badge(48,566,198,"za.sel.type1")+type_badge(260,566,198,"za.sel.type2")
    for i,(x,y) in enumerate([(342,618),(48,706),(644,706),(342,794)]):
        w += [rect(x,y,556,76,bg=PANEL_HI,color=LINE,frame=2),
              image(x+12,y+14,48,48,src_format="module:type/%d",bind=f"za.sel.m{i}.type"),
              bound_text(x+70,y+8,f"za.sel.m{i}.name",7,TEXT,wrap_width=440,max_lines=1,fit_text=True),
              value(x+532,y+48,f"za.sel.m{i}.pp",5,DIM,text="PP ",align="right",need_bind="za.sel.moves_ready")]
        for t,color in enumerate(TYPE_COLORS):
            gate=dict(hide_bind=f"za.sel.m{i}.type",keep_min=t,keep_max=t)
            w += [rect(x,y,5,76,bg=color,frame=0,**gate),
                  bound_text(x+72,y+46,f"za.type{t}.name",5,color,**gate)]
    # Memory failures need a readable explanation on a phone, not a tiny footer.
    problem={"need_bind":"za.live.problem"}
    w += [rect(40,618,1160,210,bg=PANEL_HI,color=LINE,frame=2,**problem),
          label(64,640,"DONNÉES EN DIRECT INDISPONIBLES",6,GOLD,**problem),
          bound_text(64,692,"za.live.status",6,TEXT,wrap_width=1100,max_lines=4,line_gap=8,**problem)]
    w += not_ready()
    return {"id":"team","title":"Équipe","nav_order":nav_order(*[f"party_{i}" for i in range(PARTY_SLOTS)],"send_out"),"widgets":w}


# ---- POKEDEX --------------------------------------------------------------------------------

DEX_STATES = ["MANQUANT", "VU", "CAPTURÉ", "EN STOCK"]
DEX_FILTERS = ["TOUS", "CAPTURÉS", "MANQUANTS", "VUS SEULEMENT", "EN STOCK"]
POUCHES = ["SOINS", "BALLS", "OBJETS", "TRÉSORS", "OBJETS CLÉS", "BAIES", "CT", "MÉGA-GEMMES", "AUTRES"]


def dex_page():
    w = header("dex")
    w += [value(24, 132, "za.dex.caught", 6, GOLD, text="CAPTURÉS "),
          value(440, 132, "za.dex.known", 6, BLUE, text="VUS "),
          value(850, 142, "za.dex.gen", 4, TEXT, text="GÉNÉRATION ")]
    for gen in range(1,10):
        w.append(button(24+(gen-1)*132, 194, 124, 48, f"GEN {gen}", f"dex_gen_{gen}",
                        wid=f"dex_gen_{gen}", scale=3))
        w.append(rect(24+(gen-1)*132,194,124,48,color=GOLD,frame=3,
                      hide_bind="za.dex.gen",keep_min=gen,keep_max=gen))
    for f, caption in enumerate(DEX_FILTERS):
        w.append(button(24+f*240,254,228,48,caption,f"dex_filter_{f}",
                        wid=f"dex_filter_{f}",scale=3))
        w.append(rect(24+f*240,254,228,48,color=GOLD,frame=3,
                      hide_bind="za.dex.filter",keep_min=f,keep_max=f))
    grid={"repeat":12,"repeat_cols":3,"repeat_dx":408,"repeat_row_dy":144}
    gate={"need_bind":"za.d{i}.v"}
    w += [rect(24,322,388,136,bg=PANEL,color=LINE,**grid,**gate),
          image(28,326,128,128,src_format="module:icon/%d",bind="za.d{i}.icon",
                hide_bind="za.d{i}.available",hide_eq=0,**grid,**gate),
          bound_text(164,338,"za.d{i}.name",5,TEXT,wrap_width=234,max_lines=1,fit_text=True,**grid,**gate),
          value(164,380,"za.d{i}.sp",3,DIM,text="NO.",pad=3,**grid,**gate),
          value(164,406,"za.d{i}.state",3,DIM,names=DEX_STATES,**grid,**gate),
          value(164,432,"za.d{i}.owned",2,GREEN,text="EN STOCK ",hide_bind="za.d{i}.owned",hide_eq=0,**grid,**gate),
          button(24,322,388,136,"","dex_pick",wid="dex_row_{i}",bg=CLEAR,color=CLEAR,border=0,
                 payload="{i}",**grid,**gate),
          value(24,914,"za.dex.rows",3,DIM,text="RÉSULTATS "),
          value(400,910,"za.dex.page",4,TEXT,text="PAGE ",max_bind="za.dex.pages",max_sep=" / "),
          button(850,900,170,44,"PRÉCÉDENT","dex_prev",wid="dex_prev",scale=3),
          button(1038,900,174,44,"SUIVANT","dex_next",wid="dex_next",scale=3)]
    w.append(label(580,938,"MÉGA : statuts de l'espèce de base",4,DIM,hide_bind="za.dex.gen",keep_min=3,keep_max=3))
    w += not_ready()
    return {"id":"dex","title":"Pokédex", "nav_order":nav_order(
        *[f"dex_gen_{i}" for i in range(1,10)],*[f"dex_filter_{i}" for i in range(5)],
        "dex_prev","dex_next",*[f"dex_row_{i}" for i in range(12)]),"widgets":w}


def dex_detail_page():
    w=header("dex")
    w += [image(52,206,344,344,src_format="module:icon/%d",bind="za.detail.icon"),
          bound_text(442,144,"za.detail.name",10,TEXT,wrap_width=748,max_lines=1,fit_text=True),
          value(442,212,"za.detail.sp",5,GOLD,text="NO. ",pad=3),
          value(754,218,"za.detail.state",4,DIM,names=DEX_STATES),
          value(442,270,"za.detail.owned",4,GREEN,text="EN STOCK "),
          panel(420,396,792,394),
          bound_text(444,418,"za.detail.description",5,TEXT,wrap_width=740,max_lines=8,line_gap=10),
          bound_text(52,630,"za.detail.height",5,TEXT,wrap_width=300,max_lines=1),
          bound_text(52,714,"za.detail.weight",5,TEXT,wrap_width=300,max_lines=1),
          button(24,836,320,76,"RETOUR AU POKÉDEX","go_dex",wid="dex_back",scale=5),bound_text(48,876,"za.send.status",4,DIM,wrap_width=770,max_lines=1),bound_text(48,908,"za.live.status",4,DIM,wrap_width=770,max_lines=2,line_gap=2)]
    w += type_badge(442,328,238,"za.detail.type1",4)
    w += type_badge(698,328,238,"za.detail.type2",4)
    w.append(label(580,938,"MÉGA : statuts de l'espèce de base",4,DIM,hide_bind="za.dex.gen",keep_min=3,keep_max=3))
    w += not_ready()
    return {"id":"dex_detail","title":"Fiche Pokémon","nav_order":nav_order("dex_back"),"widgets":w}


def bag_page():
    w=header("bag")
    w += [value(24,130,"za.bag.total",5,GOLD,text="TYPES D OBJETS "),
          value(800,136,"za.bag.category",4,TEXT,names=["TOUT LE SAC"]+POUCHES)]
    captions=["TOUT","SOINS","BALLS","OBJETS","TRÉSORS","CLÉS","BAIES","CT","MÉGA","AUTRES"]
    for i,caption in enumerate(captions):
        x=24+(i%5)*240; y=190+(i//5)*58
        w += [button(x,y,228,50,caption,f"bag_category_{i}",wid=f"bag_category_{i}",scale=5),
              rect(x,y,228,50,color=GOLD,frame=3,hide_bind="za.bag.category",keep_min=i,keep_max=i)]
    grid={"repeat":8,"repeat_cols":2,"repeat_dx":608,"repeat_row_dy":142}
    gate={"need_bind":"za.i{i}.v"}
    w += [rect(24,320,584,132,bg=PANEL,color=LINE,**grid,**gate),
          image(34,338,96,96,src_format="module:item/%d",bind="za.i{i}.id",**grid,**gate),
          bound_text(148,332,"za.i{i}.name",6,TEXT,wrap_width=290,max_lines=2,**grid,**gate),
          value(586,334,"za.i{i}.count",7,GOLD,text="x",align="right",**grid,**gate),
          value(148,414,"za.i{i}.pouch",5,DIM,names=POUCHES,**grid,**gate),
          label(390,414,"FAVORI",4,GOLD,need_bind="za.i{i}.favorite",**grid),
          label(492,414,"ÉQUIPÉ",4,BLUE,need_bind="za.i{i}.held",**grid),
          value(24,914,"za.bag.rows",5,DIM,text="RÉSULTATS "),
          value(400,910,"za.bag.page",5,TEXT,text="PAGE ",max_bind="za.bag.pages",max_sep=" / "),
          button(850,900,170,44,"PRÉCÉDENT","bag_prev",wid="bag_prev",scale=4),
          button(1038,900,174,44,"SUIVANT","bag_next",wid="bag_next",scale=4)]
    w += not_ready()
    return {"id":"bag","title":"Sac","nav_order":nav_order(
        *[f"bag_category_{i}" for i in range(10)],"bag_prev","bag_next"),"widgets":w}


# ---- BOITE ----------------------------------------------------------------------------------

CELL_W, CELL_H = 190, 156
CELL_DX, CELL_DY = 202, 164
GRID_X, GRID_Y = 20, 196


def box_page():
    w = header("box")
    w += [
        button(20,124,368,56,"","box_picker",wid="box_picker",need_bind="za.ready"),
        value(204, 140, "za.box.view1", 5, TEXT, text="BOÎTE ",align="center",
              max_bind="za.box.count", max_sep=" / ",wrap_width=326,max_lines=1,fit_text=True),
        label(358,139,"∨",5,DIM,align="center",need_bind="za.ready"),
        value(420, 134, "za.box.view.used", 4, DIM, pad=2),
        label(420 + 2 * 16, 134, f" / {BOX_SLOTS} PLACES", 4, DIM),
        button(638,124,242,56,"Rechercher","search_from_box",scale=5),
        button(900, 124, 160, 56, "Précédente", "box_prev", wid="box_prev", scale=5),
        button(1072, 124, 160, 56, "Suivante", "box_next", wid="box_next", scale=5),
    ]
    grid = {"repeat": BOX_SLOTS, "repeat_cols": 6,
            "repeat_dx": CELL_DX, "repeat_row_dy": CELL_DY}
    present = {"need_bind": "za.b{i}.v"}
    w += [rect(GRID_X,GRID_Y,CELL_W,CELL_H,bg=PANEL,color=LINE,frame=2,**grid),
          image(GRID_X+68,GRID_Y+48,54,54,src="module:empty-ball",**grid,hide_bind="za.b{i}.v",hide_eq=1),
          image(GRID_X+38,GRID_Y+2,114,114,src_format="module:icon/%d",bind="za.b{i}.icon",**grid,**present),
          bound_text(GRID_X+10,GRID_Y+124,"za.b{i}.name",5,TEXT,wrap_width=170,max_lines=1,fit_text=True,**grid,**present),
          rect(GRID_X+6,GRID_Y+6,86,34,bg="#EC102536",frame=0,**grid,**present),
          value(GRID_X+49,GRID_Y+10,"za.b{i}.lvl",5,TEXT,text="N.",align="center",
                wrap_width=78,max_lines=1,fit_text=True,**grid,**present),
          rect(GRID_X+164,GRID_Y+10,12,12,bg=GOLD,frame=0,need_bind="za.b{i}.shiny",**grid),
]
    w += [
        value(24, 908, "za.box.used", 4, DIM, text="TOTAL "),
        value(300, 908, "za.box.count", 4, DIM, text="BOÎTES "),
        value(560, 908, "za.box.unlocked", 4, DIM, text="DÉBLOQUÉES "),
        label(790, 924, "Glisse pour changer de boîte", 5, DIM,wrap_width=420,max_lines=1,
              fit_text=True),
    ]
    w += not_ready()
    return {"id": "box", "title": "Boite",
            "nav_order": nav_order("box_picker","box_prev","box_next"), "widgets": w}


# ---- CARTE ----------------------------------------------------------------------------------

RADAR_X, RADAR_Y, RADAR_W, RADAR_H = 96, 300, 1048, 560
DOT = 18


read_only_box_page = box_page

# ---- MISSIONS ------------------------------------------------------------------------------

QUEST_CATEGORIES=["PRINCIPALES","EXTRAS","SECONDAIRES","En cours"]
QUEST_STATES=["ÉTAT INCONNU", "DISPONIBLE", "EN COURS", "TERMINÉE", "À DÉCOUVRIR"]



actions = {f"go_{pid}": {"kind": "page", "page": pid, "transition": "slide_up", "duration_ms": 180}
           for pid, _ in TABS}
actions["box_prev"] = {"kind": "module", "action": "box_prev"}
actions["box_next"] = {"kind": "module", "action": "box_next"}
actions["box_follow"] = {"kind": "module", "action": "box_follow"}
actions["box_picker"] = {"kind": "module", "action": "box_picker"}
actions["box_set"] = {"kind": "module", "action": "box_set", "argument": "$payload"}
actions["refresh"] = {"kind": "module", "action": "refresh"}
actions["team_select"]={"kind":"module","action":"team_select","argument":"$payload"}
actions["quest_pick"]={"kind":"module","action":"quest_pick","argument":"$payload"}
actions["dex_pick"]={"kind":"module","action":"dex_pick","argument":"$payload"}
actions["send_out"]={"kind":"module","action":"send_out"}
for command,token in [("left","DLeft"),("right","DRight"),("send","DUp"),("map_open","Plus"),("map_destinations","Y"),("map_up","DUp"),("map_down","DDown"),("map_confirm","A")]:
    actions[f"input_{command}"]={"kind":"button","button":token,"frames":4 if command!="send" else 8,"enabled_bind":f"za.cmd.{command}"}
for name,token,frames in [("party_left","DLeft",8),("party_right","DRight",8),("game_map","Plus",10),("game_back","B",10),("game_zoom_in","RY+",30),("game_zoom_out","RY-",30),("game_left","LX-",30),("game_right","LX+",30),("game_up","LY+",30),("game_down","LY-",30),("game_confirm","A",10),("game_destinations","Y",10)]:
    actions[name]={"kind":"button","button":token,"frames":frames}
for i in range(len(QUEST_CATEGORIES)):
    actions[f"quest_category_{i}"]={"kind":"module","action":"quest_category","argument":i}
actions["quest_map"] = dict(kind="module", action="quest_map")
for gen in range(1,10):
    actions[f"dex_gen_{gen}"]={"kind":"module","action":"dex_gen","argument":gen}
for f in range(5):
    actions[f"dex_filter_{f}"]={"kind":"module","action":"dex_filter","argument":f}
for i in range(10):
    actions[f"bag_category_{i}"]={"kind":"module","action":"bag_category","argument":i-1}
for action in ["dex_prev","dex_next","bag_prev","bag_next"]:
    actions[action]={"kind":"module","action":action}

for i in range(12):
    actions[f"center_pick_{i}"]={"kind":"module","action":"center_pick","argument":i}
actions["center_travel"]={"kind":"module","action":"center_travel"}
actions["travel_cancel"]={"kind":"module","action":"travel_cancel"}
actions["center_close"]={"kind":"module","action":"center_close"}

# ---- 0.9: persistent detail panes and touch scrolling ------------------------
def scroll_region(name,rect,count,row_h,reset=None):
    out=dict(id=name,rect=rect,count_bind=count,row_h=row_h,fling=True,
             bar=GOLD,bar_track=LINE,bar_w=7)
    if reset:out["reset_bind"]=reset
    return out

def dex_page():
    w=header("dex")
    # Header title + count badge cleanly in top-left (NO MORE bottom-bar clipping!)
    w += [label(24,30,"POKÉDEX",7,TEXT),
          rect(204,24,154,46,bg="#FF1A2A42",color=RED,frame=1),
          value(281,34,"za.dex.rows",5,CYAN_LIGHT,align="center")]
    # Segmented catalog switcher right-aligned on top row (y=22..76)
    for i,name in enumerate(["ILLUMIS","MÉGA-DIMENSION","MÉGA-ÉVOLUTIONS"]):
        x=376+i*284
        w += [button(x,22,276,54,name,f"dex_gen_{i+1}",scale=5,bg=PANEL,color=DIM,border_color=LINE),
              rect(x,22,276,54,bg=RED,color=CYAN_LIGHT,frame=2,hide_bind="za.dex.gen",keep_min=i+1,keep_max=i+1),
              label(x+138,36,name,5,INK_DARK,align="center",wrap_width=256,max_lines=1,fit_text=True,
                    hide_bind="za.dex.gen",keep_min=i+1,keep_max=i+1)]
    # Sleek borderless Lumiose filter strip (y=94..144)
    w.append(rect(24,94,966,50,bg="#FF121828",color="#FF24304E",frame=1))
    for i,name in enumerate(["TOUS","CAPTURÉS","MANQUANTS","VUS","EN STOCK"]):
        x=30+i*191
        w += [button(x,98,185,42,name,f"dex_filter_{i}",scale=5,bg=CLEAR,color=DIM,border=0),
              rect(x,98,185,42,bg="#FF1F3854",color=CLEAR,frame=0,pill=True,hide_bind="za.dex.filter",keep_min=i,keep_max=i),
              rect(x+44,137,97,3,bg=CYAN_LIGHT,frame=0,hide_bind="za.dex.filter",keep_min=i,keep_max=i),
              label(x+92,98,name,5,CYAN_LIGHT,align="center",wrap_width=172,max_lines=1,fit_text=True,
                    text_center_h=42,hide_bind="za.dex.filter",keep_min=i,keep_max=i)]
    w += [button(1006,94,210,50,"Rechercher","search_from_dex",scale=5,bg="#FF1E2A44",color=CYAN_LIGHT,border_color="#FF345078")]
    grid=dict(repeat=512,repeat_bind="za.dex.rows",repeat_dy=110,scroll="za_dex")
    gate=dict(need_bind="za.d{i}.v")
    w += [rect(24,164,496,100,bg=PANEL,color=LINE,frame=1,**grid,**gate),
          rect(36,172,84,84,bg=WELL,color="#FF2C3858",frame=1,**grid,**gate),
          image(40,176,76,76,src_format="module:icon/%d",bind="za.d{i}.icon",**grid,**gate),
          bound_text(136,176,"za.d{i}.name",6,TEXT,wrap_width=366,max_lines=1,fit_text=True,**grid,**gate),
          value(136,220,"za.d{i}.number",5,GOLD,text="No. ",pad=3,**grid,**gate),
          value(470,220,"za.d{i}.state",5,CYAN_LIGHT,names=DEX_STATES,align="right",**grid,**gate),
          image(478,218,28,28,src="module:dex-get",hide_bind="za.d{i}.state",keep_min=2,keep_max=3,**grid,**gate),
          image(478,218,28,28,src="module:dex-find",hide_bind="za.d{i}.state",keep_min=1,keep_max=1,**grid,**gate),
          rect(24,164,496,100,color=RED,frame=3,need_bind="za.d{i}.selected",**grid),
          button(24,164,496,100,"","dex_pick",bg=CLEAR,color=CLEAR,border=0,payload="{i}",**grid,**gate),
          # Right Detail Showcase Card (y=164..952, NO white slab!)
          panel(564,164,652,788,bg="#FF161C2E",edge="#FF485C88"),
          rect(580,180,620,90,bg=HEADER,color="#FF485E8E",frame=1),
          rect(582,192,6,66,bg=RED,frame=0),
          bound_text(604,192,"za.detail.name",7,TEXT,wrap_width=400,max_lines=1,fit_text=True),
          value(604,238,"za.detail.number",5,CYAN_LIGHT,text="No. ",pad=3),
          label(776,238,"MÉGA : statuts de l'espèce de base",5,DIM,
                wrap_width=410,max_lines=1,fit_text=True,hide_bind="za.dex.gen",keep_min=3,keep_max=3),
          rect(1032,202,152,44,bg="#FF222B44",color=LINE_HI,frame=1,pill=True,hide_bind="za.detail.state",keep_min=0,keep_max=1),
          value(1108,202,"za.detail.state",5,DIM,names=DEX_STATES,align="center",text_center_h=44,hide_bind="za.detail.state",keep_min=0,keep_max=1),
          rect(1032,202,152,44,bg="#FF1E7E58",color="#FF70EEAC",frame=1,pill=True,hide_bind="za.detail.state",keep_min=2,keep_max=3),
          value(1108,202,"za.detail.state",5,TEXT,names=DEX_STATES,align="center",text_center_h=44,hide_bind="za.detail.state",keep_min=2,keep_max=3),
          # Lumiose Holographic Radial Showcase Stage behind 3D portrait!
          image(580,284,620,320,src="file:shapes/dex-stage.png"),
          image(742,296,296,296,src_format="module:icon/%d",bind="za.detail.icon"),
          value(1182,566,"za.detail.owned",5,DIM,text="EN STOCK ",align="right",need_bind="za.detail.owned"),
          rect(580,682,620,174,bg="#FF1C2338",color=LINE,frame=1),
          bound_text(602,698,"za.detail.description",5,"#FFFAF8E9",wrap_width=572,max_lines=128,line_gap=9,scroll="za_dex_text")]
    w += type_badge(580,622,224,"za.detail.type1",5)+type_badge(818,622,224,"za.detail.type2",5)
    w += [button(580,872,144,62,"Habitats","guide_locations",scale=5,bg=PANEL_HI,color=TEXT,border_color=LINE_HI,need_bind="za.guide.dex_ready"),
          button(738,872,144,62,"Combat","guide_combat",scale=5,bg=PANEL_HI,color=TEXT,border_color=LINE_HI,need_bind="za.guide.dex_ready"),
          button(896,872,144,62,"Évolutions","guide_evolutions",scale=5,bg=PANEL_HI,color=TEXT,border_color=LINE_HI,need_bind="za.guide.dex_ready"),
          button(1054,872,146,62,"Épingler","guide_dex_pin",scale=5,bg="#FF263856",color=CYAN_LIGHT,border_color=RED,need_bind="za.guide.dex_ready",hide_bind="za.guide.dex_pin",hide_eq=1),
          button(1054,872,146,62,"Épinglé","guide_dex_pin",scale=5,bg=RED,color=INK_DARK,border_color=CYAN_LIGHT,need_bind="za.guide.dex_pin")]
    w += not_ready()
    return dict(id="dex",title="Pokédex",nav_order=nav_order(),widgets=w,scrolls=[
        dict(scroll_region("za_dex",[24,164,514,782],"za.dex.rows",110,"za.dex.reset"),
             follow_bind="za.dex.follow",follow_open=True),
        scroll_region("za_dex_text",[602,698,582,144],"za.detail.lines",34,"za.detail.sp")])

def bag_page():
    w=header("bag")
    captions=["TOUT","SOINS","BALLS","OBJETS","TRÉSORS","CLÉS","BAIES","CT","MÉGA","AUTRES"]
    w.append(rect(24,18,1192,72,bg="#FF121828",color="#FF283658",frame=1))
    for i,name in enumerate(captions):
        x=32+i*118
        w += [button(x,24,112,60,"",f"bag_category_{i}",bg=CLEAR,color=CLEAR,border=0),
              rect(x,24,112,60,bg="#FF243452",color=GOLD,frame=2,pill=True,hide_bind="za.bag.category",keep_min=i,keep_max=i)]
        if i == 0:
            w.append(label(x+56,24,"TOUT",5,TEXT,align="center",text_center_h=60))
        elif i == 9:
            w.append(label(x+56,24,"AUTRES",5,TEXT,align="center",wrap_width=102,max_lines=1,fit_text=True,text_center_h=60))
        else:
            w.append(image(x+36,34,40,40,src=f"module:bag-tab/{i}"))
    for i,name in enumerate(captions):
        w.append(label(28,104,name,6,GOLD,hide_bind="za.bag.category",keep_min=i,keep_max=i))
    w += [label(290,108,"Favoris",5,DIM)]
    for i in range(4):
        x=24+i*300
        key=f"za.bf{i}."
        has=dict(need_bind=key+"id")
        w += [rect(x,144,288,82,bg="#FF161D30",color="#FF2B3858",frame=1),
              image(x+10,153,64,64,src_format="module:item/%d",bind=key+"id",**has),
              bound_text(x+80,156,key+"name",5,TEXT,wrap_width=196,max_lines=1,fit_text=True,**has),
              value(x+80,192,key+"count",5,GOLD,text="x",**has),
              label(x+144,144,"Vide",5,"#FF5C6C8E",align="center",text_center_h=82,need_bind="!"+key+"id"),
              button(x,144,288,82,"","bag_favorite_pick",payload=i,bg=CLEAR,color=CLEAR,border=0,**has)]
    grid=dict(repeat=1024,repeat_bind="za.bag.rows",repeat_dy=124,scroll="za_bag")
    gate=dict(need_bind="za.i{i}.v")
    w += [rect(24,244,598,116,bg=PANEL,color=LINE,**grid,**gate),
          image(32,254,96,96,src_format="module:item/%d",bind="za.i{i}.id",**grid,**gate),
          bound_text(144,256,"za.i{i}.name",6,TEXT,wrap_width=360,max_lines=2,fit_text=True,**grid,**gate),
          value(602,322,"za.i{i}.count",5,GOLD,text="x",align="right",**grid,**gate),
          rect(24,244,598,116,color=GOLD,frame=3,need_bind="za.i{i}.selected",**grid),
          button(24,244,598,116,"","bag_pick",payload="{i}",bg=CLEAR,color=CLEAR,border=0,**grid,**gate),
          panel(642,244,570,700),
          button(666,266,344,48,"Ajouter aux favoris","bag_favor",scale=5,need_bind="za.bag.fav_add"),
          button(666,266,344,48,"Retirer des favoris","bag_favor",scale=5,need_bind="za.bag.fav_remove"),
          label(666,266,"Favoris pleins",5,DIM,text_center_h=48,need_bind="za.bag.fav_full"),
          value(1188,266,"za.item.count",6,GOLD,text="x",align="right",text_center_h=48),
          image(852,348,150,150,src_format="module:item/%d",bind="za.item.id"),
          bound_text(666,520,"za.item.name",7,TEXT,wrap_width=520,max_lines=2,line_gap=6,fit_text=True),
          bound_text(666,644,"za.item.description",5,TEXT,wrap_width=516,max_lines=128,line_gap=7,scroll="za_bag_text"),
          button(930,814,252,56,"Objet tenu","bag_tool_hold",scale=5,need_bind="za.item.hold"),
          button(666,878,516,52,"Retour aux évolutions","guide_item_back",scale=5,need_bind="za.bag.guide"),
          button(666,878,516,52,"Quitter le choix d’objet","bag_hold_end",scale=5,need_bind="za.bag.held_context")]
    for mode,caption,action in [(1,"Soins rapides","bag_tool_heal"),(3,"Compatibilité CT","bag_tool_tm"),
                                (4,"Évolutions","bag_tool_evolve")]:
        w.append(button(666,814,252,56,caption,action,scale=5,
                        hide_bind="za.item.primary_tool",keep_min=mode,keep_max=mode))
    w+=type_badge(666,592,516,"za.item.type",5)+not_ready()
    return dict(id="bag",title="Sac",nav_order=nav_order(),widgets=w,scrolls=[
        scroll_region("za_bag",[24,244,598,692],"za.bag.rows",124,"za.bag.reset"),
        scroll_region("za_bag_text",[666,644,520,154],"za.item.lines",32,"za.item.id")])


def bag_tools_page():
    rows=dict(repeat=1000,repeat_bind="za.bagtool.rows",repeat_dy=118,scroll="za_bag_targets")
    w=[button(982,20,234,62,"SAC","go_bag",scale=5,need_bind="!za.bagtool.from_team"),
       button(982,20,234,62,"Retour","team_heal_back",scale=5,need_bind="za.bagtool.from_team"),
       image(24,24,122,122,src_format="module:item/%d",bind="za.bagtool.item",need_bind="za.bagtool.item"),
       bound_text(158,74,"za.bagtool.name",7,TEXT,wrap_width=780,max_lines=1,fit_text=True,need_bind="za.bagtool.item"),
       value(158,118,"za.bagtool.stock",5,GOLD,text="EN STOCK ",need_bind="za.bagtool.item")]
    for gate,caption in [("heal","Soins rapides"),("hold","Objet tenu"),("tm","Compatibilité CT"),("evolve","Évolutions possibles")]:
        w.append(label(158,31,caption,5,GOLD,need_bind="za.bagtool."+gate))
    w += [label(24,164,"Aucun Pokémon compatible",5,DIM,need_bind="za.bagtool.empty"),
          label(24,164,"Aucun soin nécessaire",5,DIM,need_bind="za.bagtool.heal_empty"),
          rect(24,212,586,108,bg=PANEL,color=LINE,**rows),
          rect(24,212,586,108,bg=PANEL_HI,color=RED,frame=3,need_bind="za.bt{i}.selected",**rows),
          image(36,222,88,88,src_format="module:icon/%d",bind="za.bt{i}.icon",**rows),
          bound_text(136,223,"za.bt{i}.name",6,TEXT,wrap_width=450,max_lines=1,fit_text=True,**rows),
          bound_text(136,267,"za.bt{i}.context",5,DIM,wrap_width=234,max_lines=1,fit_text=True,**rows),
          bound_text(380,267,"za.bt{i}.status",5,GREEN,wrap_width=212,max_lines=1,fit_text=True,
                     need_bind="za.bt{i}.eligible",hide_bind="za.bagtool.heal",hide_eq=1,**rows),
          bound_text(380,267,"za.bt{i}.status",5,DIM,wrap_width=212,max_lines=1,fit_text=True,
                     need_bind="!za.bt{i}.eligible",hide_bind="za.bagtool.heal",hide_eq=1,**rows),
          bound_text(136,292,"za.bt{i}.status",5,DIM,wrap_width=432,max_lines=1,fit_text=True,
                     need_bind="za.bagtool.heal",**rows),
          value(588,270,"za.bt{i}.hp",5,TEXT,text="PV ",max_bind="za.bt{i}.hpmax",align="right",**rows),
          rect(130,315,434,3,bg=GREEN,need_bind="za.bt{i}.eligible",**rows),
          button(24,212,586,108,"","bag_target",payload="{i}",bg=CLEAR,color=CLEAR,border=0,**rows),
          panel(630,212,586,724),
          bound_text(654,237,"za.bagtool.move",7,TEXT,wrap_width=530,max_lines=2,fit_text=True,need_bind="za.bagtool.tm"),
          label(654,355,"PUISSANCE",5,DIM,need_bind="za.bagtool.tm"),
          value(654,390,"za.bagtool.power",6,TEXT,need_bind="za.bagtool.tm"),
          label(928,355,"PRÉCISION",5,DIM,need_bind="za.bagtool.tm"),
          value(928,390,"za.bagtool.accuracy",6,TEXT,need_bind="za.bagtool.tm"),
          label(654,237,"Cible choisie",5,GOLD,need_bind="za.bagtool.target",hide_bind="za.bagtool.tm",hide_eq=1),
          label(24,164,"Choisis un Pokémon",5,DIM,need_bind="za.bagtool.choose",
                hide_bind="za.bagtool.heal_empty",hide_eq=1),
          bound_text(654,270,"za.bagtool.target_name",6,TEXT,wrap_width=530,max_lines=1,fit_text=True,
                     need_bind="za.bagtool.target",hide_bind="za.bagtool.tm",hide_eq=1),
          bound_text(654,450,"za.bagtool.target_name",6,TEXT,wrap_width=530,max_lines=1,fit_text=True,need_bind="za.bagtool.tm"),
          bound_text(654,518,"za.bagtool.description",5,TEXT,wrap_width=524,max_lines=64,line_gap=7,
                     scroll="za_bag_effect",need_bind="za.bagtool.describe"),
          bound_text(654,796,"za.bagtool.heal_status",5,GOLD,
                     wrap_width=530,max_lines=2,line_gap=4,need_bind="za.bagtool.heal"),
          label(654,458,"PV de la dernière sauvegarde",5,DIM,need_bind="za.bagtool.heal",
                hide_bind="za.bagtool.hp_live",hide_eq=1),
          button(654,852,538,62,"Ouvrir les capacités","moves_bag",scale=5,need_bind="za.bagtool.moves"),
          button(654,852,538,62,"Ouvrir la fiche","bag_fiche",scale=5,need_bind="za.bagtool.fiche"),
          label(654,324,"Objet actuel",5,DIM,need_bind="za.bagtool.compare"),
          image(654,359,68,68,src_format="module:item/%d",bind="za.bagtool.current_icon",need_bind="za.bagtool.current_icon"),
          bound_text(732,377,"za.bagtool.current_item",6,TEXT,wrap_width=440,max_lines=1,fit_text=True,need_bind="za.bagtool.compare"),
          label(654,526,"Remplacement proposé",5,GOLD,need_bind="za.bagtool.give"),
          bound_text(654,570,"za.bagtool.wanted_item",6,TEXT,wrap_width=524,max_lines=2,fit_text=True,need_bind="za.bagtool.give"),
          label(654,526,"Retrait proposé",5,GOLD,need_bind="za.bagtool.removal"),
          bound_text(654,638,"za.bagtool.held_status",5,DIM,wrap_width=530,max_lines=2,line_gap=4),
          button(654,852,264,62,"Choisir un objet","bag_hold_browse",scale=5,need_bind="za.bagtool.compare"),
          button(930,852,262,62,"Préparer le retrait","bag_hold_clear",scale=5,need_bind="za.bagtool.change_preview")]
    for y,height,caption,action,mode in (
        (852,62,"Appliquer le soin","bag_heal_apply","heal"),
        (746,68,"Confirmer l'objet tenu","bag_hold_apply","held"),
    ):
        offered=f"za.bagtool.{mode}_apply"
        ready=f"za.bagtool.{mode}_ready"
        disabled=dict(need_bind=offered,hide_bind=ready,hide_eq=1)
        w.extend((
            rect(654,y,538,height,bg="#FF303943",color="#FF5B646D",**disabled),
            label(923,y+(height-25)//2,caption,5,"#FF9CA5AE",align="center",wrap_width=514,
                  max_lines=1,fit_text=True,**disabled),
            button(654,y,538,height,caption,action,scale=5,need_bind=ready),
        ))
    for widget in w:
        if widget.get("bind")=="za.bt{i}.hp":
            widget["need_bind"]="za.bagtool.heal"
    w+=type_badge(654,305,530,"za.bagtool.type",5,need_bind="za.bagtool.tm")
    return dict(id="bag_tools",title="Actions du sac",nav_order=nav_order(),widgets=w,
                scrolls=[scroll_region("za_bag_targets",[24,212,586,720],"za.bagtool.rows",118,"za.bagtool.revision"),
                         dict(scroll_region("za_bag_effect",[654,518,530,280],"za.bagtool.lines",32,"za.bagtool.revision"),
                              show_bind="za.bagtool.describe")])

def mission_rows(x, y, width, group, scroll, visible):
    rows = dict(repeat=128, repeat_bind=group+".rows", repeat_dy=52, scroll=scroll, need_bind=visible)
    style = group+".{i}.style"
    caption = group+".{i}.text"
    return [
        bound_text(x+2,y+8,caption,5,TEXT,wrap_width=width-8,max_lines=1,
                   fit_text=True,hide_bind=style,keep_min=0,keep_max=0,**rows),
        bound_text(x+2,y+14,caption,5,GOLD,wrap_width=width-8,max_lines=1,
                   fit_text=True,hide_bind=style,keep_min=1,keep_max=1,**rows),
        rect(x,y,width-8,48,bg=PANEL_HI,hide_bind=style,keep_min=2,keep_max=4,**rows),
        image(x+5,y+2,44,44,src_format="module:item/%d",bind=group+".{i}.icon",
              hide_bind=style,keep_min=2,keep_max=2,**rows),
        image(x+5,y+2,44,44,src="module:progress/2",
              hide_bind=style,keep_min=4,keep_max=4,**rows),
        image(x+5,y+2,44,44,src="module:reward-money",
              hide_bind=style,keep_min=3,keep_max=3,**rows),
        bound_text(x+60,y+9,caption,5,TEXT,wrap_width=width-78,max_lines=1,
                   fit_text=True,hide_bind=style,keep_min=2,keep_max=4,**rows),
    ]


def missions_page():
    w = header("missions")
    w += [image(24,20,52,52,src="module:menu/4"),
          label(96,28,"MISSIONS",8,TEXT),
          value(1210,44,"za.quest.rows",5,DIM,align="right")]
    w.append(rect(24,92,1192,62,bg="#FF121828",color="#FF24304E",frame=1))
    for i, caption in enumerate(QUEST_CATEGORIES):
        x = 30 + i * 296
        w += [button(x,98,290,50,caption,f"quest_category_{i}",wid=f"quest_category_{i}",scale=5,bg=CLEAR,color=DIM,border=0),
              rect(x,98,290,50,bg=RED,color=CYAN_LIGHT,frame=1,pill=True,hide_bind="za.quest.category",keep_min=i,keep_max=i),
              label(x+145,98,caption,5,INK_DARK,align="center",wrap_width=270,max_lines=1,fit_text=True,
                    text_center_h=50,hide_bind="za.quest.category",keep_min=i,keep_max=i)]

    grid = dict(repeat=1024, repeat_bind="za.quest.rows", repeat_dy=122, scroll="za_quests",
                need_bind="za.q{i}.v")
    w += [rect(24,180,454,116,bg=PANEL,color=LINE,**grid),
          rect(24,180,454,116,bg=PANEL_HI,color=RED,frame=3,hide_bind="za.q{i}.selected",hide_eq=0,**grid),
          rect(30,190,6,90,bg=GOLD,hide_bind="za.q{i}.state",keep_min=2,keep_max=2,**grid),
          value(46,194,"za.q{i}.number",5,BLUE,pad=3,hide_bind="za.q{i}.state",keep_min=2,keep_max=2,**dict(grid,need_bind="za.q{i}.number")),
          value(46,194,"za.q{i}.number",5,"#FF8292A1",pad=3,hide_bind="za.q{i}.state",hide_eq=2,**dict(grid,need_bind="za.q{i}.number")),
          bound_text(402,194,"za.q{i}.category",4,DIM,align="right",wrap_width=248,max_lines=1,fit_text=True,**grid),
          bound_text(46,223,"za.q{i}.title",6,TEXT,wrap_width=410,max_lines=2,line_gap=4,hide_bind="za.q{i}.state",keep_min=2,keep_max=2,**grid),
          bound_text(46,223,"za.q{i}.title",6,"#FF8292A1",wrap_width=410,max_lines=2,line_gap=4,hide_bind="za.q{i}.state",hide_eq=2,**grid),
          image(418,190,36,36,src="module:menu/3",color=GOLD,hide_bind="za.q{i}.state",keep_min=2,keep_max=2,**grid),
          image(418,190,36,36,src="module:menu/4",color=DIM,hide_bind="za.q{i}.state",keep_min=3,keep_max=3,**grid),
          button(24,180,454,116,"","quest_pick",payload="{i}",bg=CLEAR,color=CLEAR,border=0,**grid)]

    detail = dict(need_bind="za.quest.detail.open")
    w += [label(48,220,"Aucune mission en cours",5,DIM,wrap_width=405,max_lines=2,need_bind="za.quest.active_empty"),
          rect(500,180,716,748,bg=PANEL,color=LINE),
          value(530,204,"za.quest.detail.state",5,GOLD,names=QUEST_STATES,**detail),
          value(1184,204,"za.quest.detail.number",5,BLUE,pad=3,align="right",
                hide_bind="za.quest.detail.number",hide_eq=0,**detail),
          bound_text(530,247,"za.quest.detail.title",7,TEXT,wrap_width=656,
                     max_lines=2,line_gap=5,fit_text=True,**detail),
          rect(530,338,656,2,bg=LINE,**detail),
          *mission_rows(530,353,652,"za.quest.body","za_quest_details","za.quest.detail.open"),
          button(530,838,405,66,"VOIR SUR LA CARTE","quest_map",wid="quest_map",scale=6,
                 need_bind="za.quest.detail.pin"),
          button(945,838,240,66,"Épingler","quest_pin",scale=5,
                 hide_bind="za.guide.quest_pin",hide_eq=1,**detail),
          button(945,838,240,66,"Désépingler","quest_pin",scale=5,need_bind="za.guide.quest_pin"),
          button(530,838,405,66,"Charger les repères","quest_load_map",scale=5,
                 need_bind="za.quest.detail.load"),
          bound_text(530,855,"za.guide.no_quest_location",5,DIM,wrap_width=405,max_lines=1,fit_text=True,
                     need_bind="za.quest.detail.no_location")]
    w += not_ready()
    return dict(id="missions", title="Missions",
                nav_order=nav_order(*[f"quest_category_{i}" for i in range(len(QUEST_CATEGORIES))],"quest_map"), widgets=w,
                scrolls=[scroll_region("za_quests",[24,180,464,748],"za.quest.rows",122,"za.quest.category"),
                         dict(scroll_region("za_quest_details",[516,353,690,473],"za.quest.body.rows",52,
                                            "za.quest.detail.row"),show_bind="za.quest.detail.open")])


def map_page():
    x, panel_width = 762, 458
    left, content_width = x + 18, panel_width - 36
    compact = dict(hide_bind="za.map.wide",hide_eq=1)
    wide = dict(need_bind="za.map.wide")
    map_image = dict(src_format="module:map/%d",bind="za.map.image",on_tap="map_tap",payload="$tap",
                     pan_zoom=True,min_zoom=1,max_zoom=MAP_MAX_ZOOM,pan_bounds=[0,0,1,1],
                     view_zoom_bind="za.map.home.zoom",view_cx_bind="za.map.home.x",
                     view_cy_bind="za.map.home.y",view_reset_bind="za.map.home.reset")
    widgets = header("map") + [
        label(24, 39, "PLAN D'ILLUMIS", 5, GOLD, wrap_width=236, max_lines=1, fit_text=True,
              need_bind="!za.map.recenter"),
        button(20,20,232,64,"Ma position","map_recenter",scale=5,need_bind="za.map.recenter"),
        button(272, 20, 254, 64, "Charger les repères", "quest_load_map", scale=5,
               need_bind="za.map.quest_load"),
        button(536, 20, 204, 64, "FILTRES", "map_control_9", scale=5),
        image(20,112,720,720,id="lumiose",**map_image,**compact),
        image(176,70,888,888,src="file:shapes/map-ring.png",**wide),
        image(200,94,840,840,id="lumiose_large",**map_image,**wide),
        button(1012,20,204,64,"Détails","map_panel",scale=5,**wide),
        button(536,906,204,44,"Agrandir","map_panel",scale=5,**compact),
        button(20,846,346,62,"PLACER UN REPÈRE","map_mark_start",scale=5,need_bind="za.map.mark.start_compact",
               enabled_bind="za.map.mark.can_start"),
        button(20,846,346,62,"ANNULER","map_mark_start",scale=5,need_bind="za.map.mark.placing",**compact),
        button(382,846,358,62,"EFFACER LE REPÈRE","map_mark_clear",scale=5,need_bind="za.map.mark.set",
               enabled_bind="za.map.mark.can_start",**compact),
        button(20,112,164,64,"Repère","map_mark_start",scale=5,need_bind="za.map.mark.start_wide",
               enabled_bind="za.map.mark.can_start"),
        button(20,112,164,64,"ANNULER","map_mark_start",scale=5,need_bind="za.map.mark.cancel_wide"),
        button(20,190,164,64,"Effacer","map_mark_clear",scale=5,need_bind="za.map.mark.clear_wide",
               enabled_bind="za.map.mark.can_start"),
        bound_text(24,918,"za.map.mark.message",5,GOLD,wrap_width=490,max_lines=1,
                   need_bind="za.map.mark.notice",**compact),
        bound_text(24,272,"za.map.mark.message",5,GOLD,wrap_width=156,max_lines=4,
                   hide_bind="za.map.mark.notice",hide_eq=0,**wide),
        label(20,924,"COMBATS NOCTURNES",5,DIM,need_bind="!za.map.mark.notice",**compact),
        value(497,924,"za.map.active_battles",5,RED,align="right",need_bind="!za.map.mark.notice",**compact),
        rect(18,834,206,108,bg="#FF181028",color=MAGENTA,frame=2,**wide),
        rect(20,848,6,80,bg=MAGENTA,frame=0,**wide),
        label(38,850,"COMBATS NOCTURNES",5,PINK_GLOW,wrap_width=174,max_lines=2,line_gap=3,fit_text=True,**wide),
        value(38,900,"za.map.active_battles",6,TEXT,**wide),
    ]
    for i in range(20):
        prefix = f"za.zone{i}"
        anchor = dict(need_bind=f"{prefix}.shown", x_bind=f"{prefix}.x", y_bind=f"{prefix}.y")
        widgets += [
            rect(-15, -15, 30, 30, bg="#E0156042", color="#FF41E995", **anchor),
            value(0, -13, f"{prefix}.number", 5, TEXT, align="center", **anchor),
        ]
    for kind, count, sprite, diameter in (
        ("center", 12, "module:center", 44), ("cafe", 24, "module:cafe", 46),
    ):
        for i in range(count):
            prefix = f"za.{kind}{i}"
            anchor = dict(need_bind=f"{prefix}.shown", x_bind=f"{prefix}.x", y_bind=f"{prefix}.y")
            widgets += [
                image(-diameter // 2, -diameter // 2, diameter, diameter, src=sprite, **anchor),
                button(-26, -26, 52, 52, "", f"{kind}_pick_{i}", bg=CLEAR,
                       color=CLEAR, border=0, **anchor),
            ]
    for i in range(50):
        prefix = f"za.dim{i}"
        anchor = dict(need_bind=f"{prefix}.shown", x_bind=f"{prefix}.x", y_bind=f"{prefix}.y")
        widgets.append(image(-22,-22,44,44,src="module:map-dimension-base",**anchor))
        for kind, sprite in enumerate(("module:map-portal-wild", "module:map-portal-battle", "module:map-portal-unknown")):
            widgets.append(image(-25, -25, 50, 50, src=sprite, hide_bind=f"{prefix}.kind",
                                 keep_min=kind, keep_max=kind, **anchor))
        widgets.append(button(-26, -26, 52, 52, "", f"dimension_pick_{i}", bg=CLEAR,
                              color=CLEAR, border=0, **anchor))
    for i in range(128):
        prefix = f"za.mq{i}"
        anchor = dict(need_bind=f"{prefix}.shown", x_bind=f"{prefix}.x", y_bind=f"{prefix}.y")
        for side, sprite in enumerate(("module:map-quest-main", "module:map-quest-side")):
            widgets.append(image(-23, -23, 46, 46, src=sprite, hide_bind=f"{prefix}.side",
                                 keep_min=side, keep_max=side, **anchor))
        widgets.append(button(-24, -24, 48, 48, "", f"map_quest_pick_{i}", bg=CLEAR,
                              color=CLEAR, border=0, **anchor))
    for i in range(32):
        prefix = f"za.landmark{i}"
        anchor = dict(need_bind=f"{prefix}.shown",x_bind=f"{prefix}.x",y_bind=f"{prefix}.y")
        widgets += [image(-22,-22,44,44,src="module:map-landmark",**anchor),
                    button(-22,-22,44,44,"",f"landmark_pick_{i}",bg=CLEAR,color=CLEAR,border=0,**anchor)]
    group = dict(repeat=144,repeat_bind="za.map.groups",x_bind="za.mg{i}.x",y_bind="za.mg{i}.y")
    widgets += [
        rect(-28,-28,56,56,bg="#FF173F49",color=RED,frame=3,**group),
        value(0,-12,"za.mg{i}.count",5,TEXT,align="center",**group),
        button(-30,-30,60,60,"","map_group",payload="{i}",bg=CLEAR,color=CLEAR,border=0,**group),
    ]
    # Draw the selected quest above the other landmarks and clusters.
    quest_focus = dict(repeat=128,repeat_bind="za.map.quest.pin_count",
                       need_bind="za.mq{i}.selected",x_bind="za.mq{i}.x",y_bind="za.mq{i}.y")
    widgets += [
        rect(-39,-39,78,78,bg="#FF17243A",color="#FF0B1621",frame=5,**quest_focus),
        rect(-34,-34,68,68,color="#FFFFD84E",frame=6,**quest_focus),
    ]
    for side, sprite in enumerate(("module:map-quest-main", "module:map-quest-side")):
        widgets.append(image(-26,-26,52,52,src=sprite,hide_bind="za.mq{i}.side",
                             keep_min=side,keep_max=side,**quest_focus))
    widgets += [
        image(-20,-34,40,40,src="module:map-pin",need_bind="za.map.mark.visible",
              x_bind="za.map.mark.x",y_bind="za.map.mark.y"),
        button(20,112,720,720,"","map_mark_at",payload="$tap",bg=CLEAR,color=CLEAR,border=0,
               need_bind="za.map.mark.placing",**compact),
        button(200,94,840,840,"","map_mark_at",payload="$tap",bg=CLEAR,color=CLEAR,border=0,
               need_bind="za.map.mark.cancel_wide"),
    ]
    player = dict(x_bind="za.pmx", y_bind="za.pmy")
    widgets += [
        rect(-8, -8, 16, 16, bg=BLUE, color=TEXT, frame=2, need_bind="za.map.marker", **player),
        image(-25, -25, 50, 50, src="module:map-player", need_bind="za.map.arrow",
              rotate_bind="za.map.heading", **player),
        rect(x,24,panel_width,920,bg=PANEL,color=LINE,**compact),
        value(left, 54, "za.zone.selected", 6, GOLD, text="ZONE SAUVAGE ",
              need_bind="za.zone.list_visible"),
        label(left,112,"RENCONTRES",6,GOLD,need_bind="za.map.help"),
        label(left, 147, "Touche une zone verte pour voir ses Pokémon.", 5, DIM,
              wrap_width=content_width,max_lines=3,line_gap=9,need_bind="za.map.help"),
    ]
    zone_rows = dict(repeat=128, repeat_bind="za.zone.rows", repeat_dy=112, scroll="za_zone",
                     need_bind="za.zp{i}.v")
    zone_panel = dict(need_bind="za.zone.list_visible")
    widgets += [
        value(left, 96, "za.zone.missing", 5, TEXT, text="Non capturés : ", **zone_panel),
        button(left,132,content_width,50,"Non capturés uniquement","zone_missing_toggle",
               scale=5,hide_bind="za.zone.missing_only",hide_eq=1,**zone_panel),
        button(left,132,content_width,50,"Afficher tout","zone_missing_toggle",
               scale=5,bg=BLUE,hide_bind="za.zone.missing_only",keep_min=1,keep_max=1,**zone_panel),
        label(left,215,"Tous les Pokémon de cette zone sont capturés.",5,GREEN,
              wrap_width=content_width,max_lines=3,need_bind="za.zone.complete"),
        rect(left, 196, content_width, 104, bg=PANEL_HI, **zone_rows),
        image(left + 4, 200, 96, 96, src_format="module:icon/%d", bind="za.zp{i}.icon", **zone_rows),
        bound_text(left + 108, 206, "za.zp{i}.name", 5, TEXT,
                   wrap_width=content_width - 128, max_lines=1, fit_text=True, **zone_rows),
        bound_text(left + 108, 242, "za.zp{i}.levels", 5, DIM,
                   wrap_width=content_width - 128, max_lines=1, **zone_rows),
        value(left + 108, 272, "za.zp{i}.state", 5, GREEN, names=DEX_STATES, **zone_rows),
    ]
    place = dict(need_bind="za.center.basic")
    widgets += [
        rect(x, 24, panel_width, 920, bg=PANEL, color=LINE, input_block=True, **place),
        label(left, 54, "CENTRE POKÉMON", 5, GOLD, hide_bind="za.center.cafe", keep_min=0, keep_max=0, **place),
        label(left, 54, "CAFÉS", 5, GOLD, hide_bind="za.center.cafe", keep_min=1, keep_max=1, **place),
        label(left, 54, "LIEUX D’INTÉRÊT", 5, GOLD, hide_bind="za.center.cafe", keep_min=2, keep_max=2, **place),
        bound_text(left, 118, "za.center.name", 7, TEXT, wrap_width=content_width,
                   max_lines=3, line_gap=7, **place),
        bound_text(left, 272, "za.center.description", 5, DIM, wrap_width=content_width,
                   max_lines=13, line_gap=7, **place),
        button(left, 864, content_width, 60, "FERMER", "center_close", scale=5, **place),
    ]
    dim = dict(need_bind="za.dimension.open")
    widgets += [
        # The underlying zone list is disabled by its show_bind. Blocking input
        # here would also swallow drags meant for the dimension scroll region.
        rect(x, 24, panel_width, 920, bg=PANEL, color=LINE, **dim),
        bound_text(left, 46, "za.center.name", 6, TEXT, wrap_width=content_width,
                   max_lines=2, line_gap=4, **dim),
        bound_text(left, 121, "za.dimension.stars", 7, GOLD,
                   wrap_width=content_width, max_lines=1, **dim),
        bound_text(left, 169, "za.dimension.stats", 5, TEXT, wrap_width=content_width,
                   max_lines=2, line_gap=4, **dim),
        label(left, 328, "Pokémon possibles", 5, DIM, need_bind="za.dimension.wild"),
        bound_text(left,169,"za.center.description",5,DIM,wrap_width=content_width,
                   max_lines=3,need_bind="za.dimension.unknown"),
        label(left, 328, "Combats de Dresseurs", 5, DIM, wrap_width=content_width,
              max_lines=2, need_bind="za.dimension.battle"),
        button(left, 866, content_width, 60, "FERMER", "center_close", scale=5, **dim),
    ]
    for i in range(3):
        for badge in type_badge(left + (i % 2) * 214, 231 + (i // 2) * 44, 206,
                                f"za.dimension.type{i}", 5):
            badge.update(dim)
            widgets.append(badge)
    dim_rows = dict(repeat=60, repeat_bind="za.dimension.rows", repeat_dy=110,
                    scroll="za_dimension", **dim)
    widgets += [
        rect(left, 371, content_width - 8, 102, bg=PANEL_HI, **dim_rows),
        image(left + 6, 372, 100, 100, src_format="module:icon/%d", bind="za.dp{i}.icon", **dim_rows),
        bound_text(left + 122, 389, "za.dp{i}.name", 5, TEXT, wrap_width=content_width - 146,
                   max_lines=2, line_gap=6, text_center_h=64, **dim_rows),
    ]
    quest = dict(need_bind="za.map.quest.open")
    widgets += [
        rect(x, 24, panel_width, 920, bg=PANEL, color=LINE, **quest),
        label(left, 52, "PRINCIPALES", 5, GOLD, hide_bind="za.map.quest.side", hide_eq=1, **quest),
        label(left, 52, "SECONDAIRES", 5, BLUE, hide_bind="za.map.quest.side", hide_eq=0, **quest),
        value(left + content_width, 52, "za.map.quest.number", 5, DIM, pad=3, align="right",
              hide_bind="za.map.quest.number", hide_eq=0, **quest),
        bound_text(left, 110, "za.map.quest.title", 7, TEXT, wrap_width=content_width,
                   max_lines=3, line_gap=5, **quest),
        *mission_rows(left,262,content_width,"za.map.body","za_map_quest","za.map.quest.open"),
        button(left, 866, content_width, 60, "FERMER", "center_close", scale=5, **quest),
    ]
    filters = dict(need_bind="za.map.filters_open")
    widgets += [
        rect(x, 24, panel_width, 920, bg=PANEL, color=LINE, input_block=True, **filters),
        label(left, 54, "POINTS D’INTÉRÊT", 6, GOLD, **filters),
    ]
    for row, (index, title, key, color) in enumerate([
        (0, "ZONES SAUVAGES", "wild", GREEN),
        (1, "COMBATS NOCTURNES", "battle", "#FFEA696F"),
        (2, "CENTRES", "centers", BLUE), (8, "CAFÉS", "cafes", "#FFFFAE70"),
        (11, "DIM. SAUVAGES", "dim_wild", "#FFBA95FF"), (12, "DIM. COMBAT", "dim_battle", "#FFFFA265"),
        (13, "MISSIONS", "quests", BLUE),
        (14, "DIM. INCONNUES", "dim_unknown", "#FFD8B0FF"),
        (15, "LIEUX D’INTÉRÊT", "landmarks", BLUE),
    ]):
        y = 112 + row * 80
        widgets += [
            button(left, y, content_width, 66, title, f"map_control_{index}", scale=5, **filters),
            rect(left + 14, y + 54, content_width - 28, 5, bg=color, frame=0,
                 need_bind=f"za.map.{key}", hide_bind="za.map.filters_open", keep_min=1, keep_max=1),
        ]
    widgets += [button(left, 866, content_width, 60, "FERMER", "map_control_9", scale=5, **filters)]
    choices=dict(need_bind="za.map.choices")
    choice_rows=dict(repeat=272,repeat_bind="za.map.choices",repeat_dy=102,scroll="za_map_choices")
    widgets += [
        # Eden checks input blockers before scroll regions. This pane sits beside the map.
        rect(x,24,panel_width,920,bg=PANEL,color=LINE,**choices),
        label(left,54,"Repères proches",6,GOLD,**choices),
        rect(left,110,content_width,92,bg=PANEL_HI,**choice_rows),
        bound_text(left+68,121,"za.mc{i}.name",5,TEXT,wrap_width=content_width-86,max_lines=2,
                   line_gap=5,text_center_h=70,**choice_rows),
    ]
    for i,sprite in enumerate(("module:menu/3","module:center","module:cafe",
                               "module:map-portal-wild","module:map-quest-side","module:map-landmark")):
        widgets.append(image(left+12,134,44,44,src=sprite,hide_bind="za.mc{i}.type",keep_min=i,keep_max=i,**choice_rows))
    widgets += [
        button(left,110,content_width,92,"","map_choice",payload="{i}",bg=CLEAR,color=CLEAR,border=0,**choice_rows),
        button(left,866,content_width,60,"FERMER","center_close",scale=5,**choices),
    ]
    widgets += not_ready()
    return dict(id="map", title="Carte", nav_order=nav_order(), widgets=widgets, scrolls=[
        dict(scroll_region("za_zone", [left, 196, content_width, 562], "za.zone.rows",
                           112, "za.zone.scroll_reset"), show_bind="za.zone.list_visible"),
        dict(scroll_region("za_dimension", [left, 368, content_width, 480], "za.dimension.rows",
                           110, "za.dimension.selected"), show_bind="za.dimension.open"),
        dict(scroll_region("za_map_quest", [left, 262, content_width, 582], "za.map.body.rows",
                           52, "za.map.quest.key"), show_bind="za.map.quest.open"),
        dict(scroll_region("za_map_choices",[left,110,content_width,732],"za.map.choices",
                           102,"za.map.choices_reset"),show_bind="za.map.choices"),
    ])

actions["bag_pick"]={"kind":"module","action":"bag_pick","argument":"$payload"}
actions["map_tap"]={"kind":"module","action":"map_tap","argument":"$payload"}
for i in range(16):actions[f"map_control_{i}"]=dict(kind="module",action="map_control",argument=i)
for i in range(32):actions[f"landmark_pick_{i}"]=dict(kind="module",action="landmark_pick",argument=i)
for action in ("map_mark_start","map_mark_clear"):
    actions[action]=dict(kind="module",action=action)
actions["map_mark_at"]=dict(kind="module",action="map_mark_at",argument="$payload")
for i in range(128):actions[f"map_quest_pick_{i}"]=dict(kind="module",action="map_quest_pick",argument=i)
for i in range(24):actions[f"cafe_pick_{i}"]=dict(kind="module",action="cafe_pick",argument=i)
for i in range(50):actions[f"dimension_pick_{i}"]=dict(kind="module",action="dimension_pick",argument=i)
for i in range(3):actions[f"research_open_{i}"]=dict(kind="module",action="research_open",argument=i-1)
actions["research_pick"]=dict(kind="module",action="research_pick",argument="$payload")
actions["research_close"]=dict(kind="module",action="research_close")
actions["game_menu"]={"kind":"button","button":"X","frames":10}
for i in range(20):actions[f"zone_pick_{i}"]={"kind":"module","action":"zone_pick","argument":i}

_old_box_page=box_page
def box_page():
    page=_old_box_page();w=page["widgets"]
    w=[v for v in w if v.get("text")!="PREC ET SUIV CHANGENT DE BOITE"]
    slot=next(i for i,v in enumerate(w) if v.get("need_bind")=="!za.ready")
    extra=[button(GRID_X,GRID_Y,CELL_W,CELL_H,"","box_pick",payload="{i}",bg=CLEAR,color=CLEAR,border=0,
                  repeat=30,repeat_cols=6,repeat_dx=CELL_DX,repeat_row_dy=CELL_DY,need_bind="za.b{i}.v"),
           button(884,896,328,48,"COMMANDES BOÎTES","go_box_controls",scale=4)]
    gate=dict(need_bind="za.swap.open")
    extra += [rect(0,108,W,850,bg="#F0070A11",frame=0,input_block=True,**gate),
              panel(56,156,1128,784),
              label(86,180,"ÉCHANGE BOÎTE / ÉQUIPE",6,GOLD,**gate),
              image(86,234,106,106,src_format="module:icon/%d",bind="za.swap.icon",**gate),
              bound_text(216,242,"za.swap.name",7,TEXT,wrap_width=936,max_lines=1,fit_text=True,**gate),
              label(216,296,"Ouvre les boîtes du jeu avant de confirmer.",4,DIM,**gate)]
    extra[-5]["need_bind"]="za.swap.open"
    for i in range(6):
        x=86+i%2*538;y=364+i//2*136
        extra += [rect(x,y,514,124,bg=PANEL_HI,color=LINE,**gate),
                  image(x+8,y+8,100,100,src_format="module:icon/%d",bind=f"za.p{i}.icon",**gate),
                  bound_text(x+124,y+20,f"za.p{i}.name",6,TEXT,wrap_width=366,max_lines=1,fit_text=True,**gate),
                  value(x+124,y+72,f"za.p{i}.lvl",4,DIM,text="N. ",**gate),
                  rect(x,y,514,124,color=GOLD,frame=3,hide_bind="za.swap.party",keep_min=i,keep_max=i,**gate),
                  button(x,y,514,124,"","box_swap_pick",payload=i,bg=CLEAR,color=CLEAR,border=0,**gate)]
    extra += [button(86,786,732,64,"EFFECTUER L'ÉCHANGE","box_swap_confirm",scale=5,**gate),
              button(838,786,322,64,"ANNULER","box_swap_cancel",scale=5,**gate),
              bound_text(86,870,"za.swap.status",5,GOLD,wrap_width=1060,max_lines=2,line_gap=5,**gate)]
    w[slot:slot]=extra;page["widgets"]=w;return page

def box_controls_page():
    w=header("box")+[label(620,140,"COMMANDES DES BOÎTES DU JEU",6,GOLD,align="center")]
    for x,y,text,action in [(450,300,"HAUT","game_box_up"),(120,430,"GAUCHE","game_box_left"),(780,430,"DROITE","game_box_right"),(450,560,"BAS","game_box_down"),
                            (120,720,"Y - PRENDRE","game_destinations"),(450,720,"A - POSER","game_confirm"),(780,720,"B - RETOUR","game_back"),
                            (120,840,"BOÎTE -","game_box_previous"),(450,840,"SECOND ÉCRAN","go_box"),(780,840,"BOÎTE +","game_box_next")]:
        w.append(button(x,y,300,80,text,action,scale=5))
    return dict(id="box_controls",title="Commandes des boîtes",nav_order=nav_order(),widgets=w)

actions["go_box_controls"]={"kind":"page","page":"box_controls"}
for name in ["box_pick","box_swap_pick"]:actions[name]={"kind":"module","action":name,"argument":"$payload"}
for name in ["box_swap_confirm","box_swap_cancel"]:actions[name]={"kind":"module","action":name}
for name,key in [("left","DLeft"),("right","DRight"),("up","DUp"),("down","DDown"),("next","R"),("previous","L")]:
    actions[f"game_box_{name}"]={"kind":"button","button":key,"frames":6}
for name,key in [("left","DLeft"),("right","DRight"),("up","DUp"),("down","DDown"),("pick","Y"),("confirm","A"),("back","B"),("next","R"),("previous","L")]:
    actions[f"input_swap_{name}"]={"kind":"button","button":key,"frames":6 if name in ["left","right","up","down"] else 8,"enabled_bind":f"za.cmd.swap_{name}"}

box_page = read_only_box_page

# Headerless layouts and the fixed shortcut bar.
def header(page_id):
    return [image(1,1,1,1,src="module:ready")]

def refusal_overlay():
    """Shown instead of every page when the host refuses the native module (wrong game build,
    wrong platform, damaged package). Plain English text with no module binding: the module is
    not running, so nothing bound to it can draw. `module_error_message` is the host's own reason."""
    gate={"need_bind":"module_error","_static":True}
    return [
        rect(0,0,W,H,bg=BG,frame=0,input_block=True,**gate),
        rect(40,300,1160,420,bg=PANEL,color=LINE,frame=2,**gate),
        label(70,336,"POKEMON LEGENDS Z-A COMPANION",6,GOLD,**gate),
        label(70,408,"The companion module did not start.",6,TEXT,**gate),
        bound_text(70,486,"module_error_message",5,DIM,wrap_width=1100,max_lines=3,line_gap=8,**gate),
        label(70,590,"This companion supports Pokemon Legends Z-A 2.0.2 only.",5,TEXT,wrap_width=1100,max_lines=2,line_gap=8,**gate),
        label(70,650,"Enable the 2.0.2 update for the game in Eden Duo and restart.",5,TEXT,wrap_width=1100,max_lines=2,line_gap=8,**gate),
    ]


def tabbar():
    icons=("module:menu/0","module:menu/1","module:menu/2","module:center",
           "module:item/4","module:menu/4","module:menu/3","module:map-pin")
    menu=dict(need_bind="za.nav.menu")
    edit=dict(need_bind="za.nav.editing")
    out=[
        rect(0,0,W,966,bg="#C8080B14",frame=0,input_block=True,**menu),
        rect(24,154,1192,790,bg="#FF141A2C",color=LINE_HI,frame=2,**menu),
        label(56,196,"Onglets",8,TEXT,**menu),
        button(708,176,234,68,"","nav_edit",wid="nav_edit",bg=PANEL_HI,color=TEXT,border_color=LINE_HI,**menu),
        label(825,198,"Modifier",5,TEXT,align="center",hide_bind="za.nav.editing",hide_eq=1,**menu),
        label(825,198,"Terminé",5,CYAN_LIGHT,align="center",**edit),
        button(958,176,226,68,"Fermer","nav_more",scale=5,bg=PANEL_HI,color=TEXT,border_color=LINE_HI,**menu),
        label(56,266,"Choisis une place, puis un onglet.",5,DIM,**edit),
    ]
    for i in range(3):
        x=56+i*380
        out += [
            button(x,306,368,68,"",f"nav_slot_{i}",wid=f"nav_slot_{i}",bg=PANEL_HI,color=TEXT,border_color=LINE,**edit),
            rect(x,306,368,68,color=RED,frame=3,hide_bind="za.nav.edit_slot",
                 keep_min=i+1,keep_max=i+1,**edit),
            bound_text(x+184,328,f"za.nav.slot{i}.name",6,TEXT,align="center",
                       wrap_width=344,max_lines=1,fit_text=True,**edit),
        ]
    for i,((_,name),icon) in enumerate(zip(TABS,icons),1):
        x=56+((i-1)%2)*574
        y=406+((i-1)//2)*122
        action=f"nav_pick_{i}"
        out += [
            button(x,y,554,102,"",action,wid=action,bg=PANEL_HI,color=TEXT,border_color=LINE,**menu),
            rect(x,y,554,102,color=RED,frame=3,hide_bind="za.nav.active",keep_min=i,keep_max=i,**menu),
            image(x+22,y+25,52,52,src=icon,**menu),
            label(x+92,y+36,name.title(),6,TEXT,wrap_width=430,max_lines=1,fit_text=True,**menu),
        ]
    # Sleek borderless Lumiose glass bottom dock (no double-bordered boxes!)
    out += [
        rect(0, 968, W, 112, bg="#FF0D1220", frame=0),
        rect(0, 968, W, 2, bg="#FF283A60", frame=0),
    ]
    for i,bind in enumerate(("za.nav.slot0","za.nav.slot1","za.nav.slot2","za.nav.more")):
        x=24+i*302
        action=f"nav_shortcut_{i}" if i<3 else "nav_more"
        active=bind+".active"
        out += [
            button(x,982,286,74,"",action,wid=action,bg=CLEAR,color=TEXT,border=0),
            *( [rect(x-8, 996, 2, 46, bg="#FF1E2942", frame=0)] if i > 0 else [] ),
            rect(x,982,286,74,bg=RED,color=CYAN_LIGHT,frame=1,pill=True,need_bind=active),
            rect(x+86,968,114,4,bg=CYAN_LIGHT,frame=0,need_bind=active),
            bound_text(x+166,982,bind+".name",5,DIM,align="center",wrap_width=192,
                       max_lines=1,fit_text=True,text_center_h=74,hide_bind=active,hide_eq=1),
            bound_text(x+166,982,bind+".name",5,INK_DARK,align="center",wrap_width=192,
                       max_lines=1,fit_text=True,text_center_h=74,need_bind=active),
        ]
        for tab,icon in enumerate(icons,1):
            out.append(image(x+32,1000,38,38,src=icon,hide_bind=bind+".id",keep_min=tab,keep_max=tab))
        if i==3:
            out += [image(x+32,1000,38,38,src="file:shapes/more-icon.png",
                          hide_bind=bind+".id",hide_eq=-1,keep_min=0,keep_max=0),
                    label(x+258,982,"…",6,INK_DARK,align="center",text_center_h=74,need_bind=active)]
    return out

for name in ("nav_more","nav_edit"):
    actions[name]=dict(kind="module",action=name)
for i in range(3):
    actions[f"nav_shortcut_{i}"]=dict(kind="module",action="nav_shortcut",argument=i)
    actions[f"nav_slot_{i}"]=dict(kind="module",action="nav_slot",argument=i)
for i in range(1,9):
    actions[f"nav_pick_{i}"]=dict(kind="module",action="nav_pick",argument=i)
actions["box_details"]=dict(kind="module",action="box_details",argument="$payload")
actions["go_progress"]=dict(kind="page",page="progress")

def _attack_fill(color: str) -> str:
    base = bytes.fromhex(PANEL[3:])
    accent = bytes.fromhex(color[3:])
    rgb = bytes(round(background * 0.84 + foreground * 0.16)
                for background, foreground in zip(base, accent))
    return "#FF" + rgb.hex().upper()


ATTACK_FILLS = [_attack_fill(color) for color in TYPE_COLORS]
ATTACK_LABELS = [
    "#FF" + bytes(round(component * 0.45 + 255 * 0.55)
                  for component in bytes.fromhex(color[3:])).hex().upper()
    for color in TYPE_COLORS
]


def attacks(prefix,positions,width=478,interactive=False,compact=False,gate_suffix=".id"):
    out=[]
    for i,(x,y) in enumerate(positions):
        p=f"{prefix}.m{i}"
        height=(104 if compact else 150) if interactive else 80
        gate=dict(need_bind=p+gate_suffix)
        if interactive:
            out += [
                rect(x, y, width, height, bg="#FFFFFFFF", color=LINE_HI, frame=2,
                     tint_bind=p+".type", tint_colors=ATTACK_FILLS,
                     hide_bind=p+".type", keep_min=0, keep_max=17, **gate),
                button(x, y, width, height, "", f"attack_{i}", wid=f"attack_{i}" + ("_d" if compact else ""),
                       bg=CLEAR, color=CLEAR, border=0, **gate),
            ]
            for t, color in enumerate(TYPE_COLORS):
                out.append(rect(x+2, y+12, 7, height-24, bg=color, frame=0,
                                hide_bind=p+".type", keep_min=t, keep_max=t, **gate))
            if compact:
                # Sleek ABXY Diamond card (width=556, height=104) with colored Type Mini-Pill + Z-A Category Icon!
                out += [
                    image(x+18, y+11, 36, 36, src_format="module:type/%d", bind=p+".type", **gate),
                    bound_text(x+62, y+11, p+".name", 6, TEXT, wrap_width=width-238, max_lines=1,
                               fit_text=True, text_center_h=36, **gate),
                    image(x+184, y+46, 26, 22, src_format="module:cat/%d", bind=p+".cat_id", **gate),
                    bound_text(x+216, y+44, p+".category", 4, DIM, wrap_width=126, max_lines=1,
                               fit_text=True, text_center_h=24, **gate),
                    bar(x+18, y+78, width-142, 12, p+".recharge", "#FF38E0BA", maxv=1000, **gate),
                    rect(x+width-116, y+66, 104, 30, bg="#FF1A5248", color="#FF38E0BA", frame=1, pill=True, **gate),
                    bound_text(x+width-64, y+66, p+".recharge_text", 4, TEXT, align="center",
                               wrap_width=92, max_lines=1, fit_text=True, text_center_h=30, **gate),
                ]
                for offset,caption,key in ((124,"Pui.","power"),(48,"Préc.","accuracy")):
                    pos=x+width-offset
                    out += [label(pos,y+10,caption,4,DIM,align="center",wrap_width=74,max_lines=1,fit_text=True,**gate),
                            bound_text(pos,y+34,p+"."+key,5,TEXT,align="center",**gate)]
                for t,color in enumerate(TYPE_COLORS):
                    tgate = dict(hide_bind=p+".type", keep_min=t, keep_max=t, **gate)
                    out += [
                        rect(x+62, y+44, 114, 24, bg=color, color="#30FFFFFF", frame=1, pill=True, **tgate),
                        bound_text(x+119, y+44, f"za.type{t}.name", 4, TYPE_TEXT[t], align="center",
                                   wrap_width=102, max_lines=1, fit_text=True, text_center_h=24, **tgate),
                    ]
            else:
                # Clean 2x2 Tactile Move Card with REAL Z-A Category Icon (module:cat/%d) + Type Mini-Pill!
                out += [
                    image(x+20, y+16, 42, 42, src_format="module:type/%d", bind=p+".type", **gate),
                    bound_text(x+72, y+16, p+".name", 6, TEXT, wrap_width=width-236, max_lines=1,
                               fit_text=True, text_center_h=42, **gate),
                    image(x+204, y+66, 32, 26, src_format="module:cat/%d", bind=p+".cat_id", **gate),
                    bound_text(x+242, y+64, p+".category", 4, DIM, wrap_width=102, max_lines=1,
                               fit_text=True, text_center_h=30, **gate),
                    bar(x+20, y+110, width-152, 16, p+".recharge", "#FF38E0BA", maxv=1000, **gate),
                    rect(x+width-120, y+100, 106, 36, bg="#FF1A5248", color="#FF38E0BA", frame=1, pill=True, **gate),
                    bound_text(x+width-67, y+100, p+".recharge_text", 5, TEXT, align="center",
                               wrap_width=94, max_lines=1, fit_text=True, text_center_h=36, **gate),
                ]
                for offset,caption,key in ((128,"Pui.","power"),(52,"Préc.","accuracy")):
                    pos=x+width-offset
                    out += [label(pos,y+16,caption,5,DIM,align="center",wrap_width=80,max_lines=1,fit_text=True,**gate),
                            bound_text(pos,y+46,p+"."+key,5,TEXT,align="center",**gate)]
                for t,color in enumerate(TYPE_COLORS):
                    tgate = dict(hide_bind=p+".type", keep_min=t, keep_max=t, **gate)
                    out += [
                        rect(x+72, y+64, 122, 30, bg=color, color="#30FFFFFF", frame=1, pill=True, **tgate),
                        bound_text(x+133, y+64, f"za.type{t}.name", 4, TYPE_TEXT[t], align="center",
                                   wrap_width=108, max_lines=1, fit_text=True, text_center_h=30, **tgate),
                    ]
        else:
            out += [
                rect(x,y,width,height,bg=PANEL_HI,color=LINE),
                image(x+12,y+14,48,48,src_format="module:type/%d",bind=p+".type"),
                bound_text(x+72,y+8,p+".name",7,TEXT,wrap_width=width-92,max_lines=1,fit_text=True),
                value(x+width-18,y+50,p+".pp",4,DIM,text="PP ",align="right"),
            ]
            for t,color in enumerate(TYPE_COLORS):
                out.append(bound_text(x+72,y+49,f"za.type{t}.name",5,color,
                                      hide_bind=p+".type",keep_min=t,keep_max=t))
    return out

def team_page():
    w=header("team")
    # 6 Sleek Party Cards (y=20..294) with portrait wells, fixed Lv., and ACTIVE badge clear of head!
    for i in range(6):
        x=22+i*202;p=f"za.p{i}";gate=dict(need_bind=p+".v")
        w += [rect(x,20,186,274,bg=PANEL,color=LINE,frame=2),
              rect(x+12,32,162,146,bg=WELL,color="#FF2C3858",frame=1,**gate),
              image(x+20,34,146,142,src_format="module:icon/%d",bind=p+".icon",**gate),
              bound_text(x+14,186,p+".name",5,TEXT,wrap_width=158,max_lines=1,fit_text=True,**gate),
              bar(x+14,216,158,12,p+".hp",GREEN,max_bind=p+".hpmax",**gate),
              bar(x+14,234,158,7,p+".xp",BLUE,max_bind=p+".xpmax",**gate),
              value(x+14,252,p+".lvl",5,DIM,text="N. ",**gate),
              image(x+148,250,24,24,src_format="module:gender/%d",bind=p+".g",**gate),
              image(x+61,116,64,64,src="module:empty-ball",hide_bind=p+".v",hide_eq=1),
              # Sleek Z-A Lime ACTIVE badge cleanly tucked at top-left, never covering the Pokemon's face
              rect(x+14,34,92,26,bg=LIME,frame=0,need_bind=p+".deployed"),
              label(x+60,36,"ACTIF",5,INK_DARK,align="center",
                    wrap_width=86,max_lines=1,fit_text=True,need_bind=p+".deployed"),
              rect(x,20,186,274,color=LIME,frame=3,need_bind=p+".selected"),
              button(x,20,186,274,"","team_select",wid=f"party_{i}",payload=i,bg=CLEAR,color=CLEAR,border=0,**gate)]
        for move in range(4):
            w.append(bar(x+14+move*40,244,36,5,f"{p}.r{move}",RED,maxv=1000,
                         need_bind=f"{p}.c{move}"))

    # Main Battle Command Deck (y=310..954)
    w += [panel(22,310,1196,644,bg="#FF151B2C",edge="#FF42547C"),
          # Dark architectural hero strip (NO glaring white block!)
          rect(38,326,1164,88,bg=HEADER,color="#FF485E8E",frame=1),
          rect(38,340,7,60,bg=LIME,frame=0),
          bound_text(62,352,"za.sel.name",7,TEXT,wrap_width=230,max_lines=1,fit_text=True),
          value(298,357,"za.sel.lvl",5,LIME,text="N. ",need_bind="za.sel.v"),
          image(366,356,28,28,src_format="module:gender/%d",bind="za.sel.g",need_bind="za.sel.v"),
          # Transparent Held Item inside sleek dark pill (zero white square behind Mega Stones!)
          rect(400,342,324,56,bg="#FF141A2C",color="#FF405076",frame=1,need_bind="za.sel.item"),
          image(412,348,44,44,src_format="module:item/%d",bind="za.sel.item",need_bind="za.sel.item"),
          bound_text(466,357,"za.sel.item_name",5,"#FFFAF8E9",wrap_width=248,max_lines=1,fit_text=True,need_bind="za.sel.item")]
    w += type_badge(756,349,208,"za.sel.type1",5)+type_badge(978,349,208,"za.sel.type2",5)

    # Balanced Dual HP & EXP Gauges (y=430..494)
    w += [label(44,432,"PV",5,GREEN),
          value(598,432,"za.sel.hp",5,TEXT,max_bind="za.sel.hpmax",max_sep=" / ",align="right"),
          bar(44,464,554,18,"za.sel.hp",GREEN,max_bind="za.sel.hpmax"),
          label(642,432,"EXP",5,BLUE),
          value(1196,432,"za.sel.xp",5,DIM,max_bind="za.sel.xpmax",max_sep=" / ",align="right"),
          bar(642,464,554,18,"za.sel.xp",BLUE,max_bind="za.sel.xpmax")]

    # Mode 0 (default): Clean 2x2 Tactile Move Grid + Tall Send Out Pillar
    grid_m = dict(need_bind="za.team.grid_mode")
    diam_m = dict(need_bind="za.team.diam_mode")
    w += attacks("za.sel",[(44,506),(540,506),(44,672),(540,672)],width=478,interactive=True,compact=False,gate_suffix=".grid_on")
    w += [button(1038,506,158,316,"","send_out",wid="send_out",bg="#FF18749E",color=TEXT,border_color=CYAN_LIGHT,**grid_m),
          image(1069,580,96,96,src="file:shapes/send-arrow.png",**grid_m),
          label(1117,694,"ENVOYER",5,TEXT,align="center",wrap_width=144,max_lines=1,fit_text=True,**grid_m),
          bound_text(1117,746,"za.send.status",5,CYAN_LIGHT,align="center",wrap_width=144,max_lines=2,line_gap=4,fit_text=True,**grid_m)]
    # Default (ABXY Diamond): Switch X/Y/A/B diamond moves + Corner Send Out button
    w += attacks("za.sel",[(342,502),(44,614),(640,614),(342,726)],width=556,interactive=True,compact=True,gate_suffix=".diam_on")
    w += [button(1052,726,144,104,"","send_out",wid="send_out_d",bg="#FF18749E",color=TEXT,border_color=CYAN_LIGHT,**diam_m),
          image(1095,735,58,58,src="file:shapes/send-arrow.png",**diam_m),
          label(1124,797,"ENVOYER",4,TEXT,align="center",wrap_width=132,max_lines=1,fit_text=True,**diam_m)]

    # Sleek subtle action strip (y=854..924)
    w += [button(44,854,368,70,"CAPACITÉS","moves_team",scale=5,bg="#FF1E2942",color=TEXT,border_color="#FF2D3E64"),
          button(432,854,368,70,"Objet tenu","bag_hold_team",scale=5,bg="#FF1E2942",color=TEXT,border_color="#FF2D3E64",need_bind="za.sel.v"),
          button(820,854,376,70,"Soins","team_heal_open",scale=5,bg="#FF1E2942",color=TEXT,border_color="#FF2D3E64",need_bind="za.sel.v")]

    problem={"need_bind":"za.live.problem"}
    w += [rect(40,600,1160,220,bg=PANEL_HI,color=LINE_HI,frame=2,input_block=True,**problem),
          label(64,624,"DONNÉES EN DIRECT INDISPONIBLES",6,GOLD,**problem),
          bound_text(64,676,"za.live.status",5,TEXT,wrap_width=1100,max_lines=4,line_gap=8,**problem)]
    w += not_ready()
    return dict(id="team",title="Équipe",nav_order=nav_order(*[f"party_{i}" for i in range(6)],"send_out"),widgets=w)

def team_heal_page():
    rows = dict(repeat=1024,repeat_bind="za.heal.rows",repeat_dy=124,scroll="za_team_heal")
    target = dict(need_bind="za.heal.target")
    w = [panel(24,20,1192,140),
         button(986,38,206,68,"Équipe","go_team",scale=5),
         image(40,28,116,116,src_format="module:icon/%d",bind="za.heal.icon",**target),
         label(176,38,"Soins compatibles",5,GOLD,wrap_width=780,max_lines=1,fit_text=True),
         bound_text(176,74,"za.heal.name",7,TEXT,wrap_width=780,max_lines=1,fit_text=True,**target),
         value(176,125,"za.heal.hp",5,GREEN,text="PV ",max_bind="za.heal.maxhp",**target),
         bound_text(430,125,"za.heal.status",5,DIM,wrap_width=516,max_lines=1,fit_text=True,**target),
         label(40,208,"Aucun soin compatible en stock",6,DIM,need_bind="za.heal.empty",
               wrap_width=1136,max_lines=2),
         label(40,208,"Pokémon introuvable dans l'équipe.",6,DIM,need_bind="!za.heal.target",
               wrap_width=1136,max_lines=2),
         rect(24,180,1178,114,bg=PANEL,color=LINE,**rows),
         image(38,190,94,94,src_format="module:item/%d",bind="za.hm{i}.id",**rows),
         bound_text(152,195,"za.hm{i}.name",6,TEXT,wrap_width=875,max_lines=1,fit_text=True,**rows),
         bound_text(152,236,"za.hm{i}.description",4,DIM,wrap_width=1020,max_lines=2,line_gap=4,**rows),
         value(1178,195,"za.hm{i}.stock",5,GOLD,text="×",align="right",**rows),
         button(24,180,1178,114,"","team_heal_pick",payload="$za.hm{i}.id",
                bg=CLEAR,color=CLEAR,border=0,**rows)]
    return dict(id="team_heal",title="Soins",nav_order=nav_order(),widgets=w+not_ready(),
                scrolls=[scroll_region("za_team_heal",[24,180,1192,752],"za.heal.rows",124,"za.heal.reset")])


def box_detail_page():
    w=header("box")+[panel(24,24,1188,920),rect(24,24,1188,76,bg=HEADER,frame=0),
       bound_text(48,45,"za.boxdetail.name",7,TEXT,wrap_width=820,max_lines=1,fit_text=True),
       value(1100,47,"za.boxdetail.lvl",6,LIME,text="N. ",align="right"),
       image(1150,46,28,28,src_format="module:gender/%d",bind="za.boxdetail.g"),
       image(40,112,220,220,src_format="module:icon/%d",bind="za.boxdetail.icon"),
       value(286,138,"za.boxdetail.sp",5,text="N° ",pad=3),
       bound_text(286,174,"za.boxdetail.position",5,DIM,wrap_width=428,max_lines=1,fit_text=True)]
    w += type_badge(282,208,210,"za.boxdetail.type1",4)+type_badge(282,262,210,"za.boxdetail.type2",4)
    w += [panel(48,350,444,112),label(66,368,"OBJET",5,DIM),
          button(254,356,228,46,"Objet tenu","bag_hold_box",scale=5),
          bound_text(66,410,"za.boxdetail.item_name",6,TEXT,wrap_width=408,max_lines=1,fit_text=True),
          panel(48,480,444,112),label(66,498,"NATURE",4,DIM),
          value(66,540,"za.boxdetail.nature",6,TEXT,table="natures"),
          image(706,226,280,280,src_format="module:radar/%d",bind="za.boxdetail.open")]
    for title,key,x,y in [("PV","hpmax",846,144),("ATTAQUE","atk",1090,254),("DÉFENSE","def",1090,420),
                          ("VITESSE","spe",846,496),("DÉF. SPÉ.","spd",598,420),("ATQ. SPÉ.","spa",598,254)]:
        w += [label(x,y,title,4,DIM,align="center"),value(x,y+38,"za.boxdetail."+key,6,TEXT,align="center")]
    w += attacks("za.boxdetail",[(342,598),(48,684),(644,684),(342,770)])
    w += [button(48,860,238,62,"RETOUR","go_box",scale=5),
          button(310,860,340,62,"CAPACITÉS","moves_box",scale=5)]
    return dict(id="box_detail",title="Fiche Pokémon",nav_order=nav_order(),widgets=w)

def progress_page():
    w=header("progress")+[
        label(28,22,"MON PARCOURS À ILLUMIS",7,TEXT),
        label(28,70,"Recherche · aventures · Royale Z-A",5,DIM),
        label(1212,32,"Données en direct",5,GREEN,align="right",need_bind="za.progress.research_live"),
        label(1212,32,"Données de votre dernière sauvegarde",5,DIM,align="right",need_bind="!za.progress.research_live"),
        # Mable's Research Hero Banner (exact mresearch_top teal gradient + FULL unobstructed Mable torso!)
        image(24,108,1192,236,src="file:shapes/research-banner.png"),
        image(872,110,330,232,src="module:progress/3"),
        image(46,136,180,180,src="file:shapes/research-medallion.png"),
        label(136,174,"NIVEAU",5,"#FF9EEEE4",align="center"),
        value(136,214,"za.progress.mable_level",12,TEXT,align="center"),
        label(254,132,"RECHERCHES DE MYOSOTIS",5,"#FFA6F5EC",wrap_width=590,max_lines=1,fit_text=True),
        bound_text(254,174,"za.progress.research_fraction",9,TEXT),
        bar(254,234,586,20,"za.progress.research_xp",color=LIME,max_bind="za.progress.research_xpmax",bg="#FF0A2634"),
        label(254,282,"POINTS MANQUANTS",5,"#FFD6F6F2",need_bind="!za.progress.research_max"),
        value(616,278,"za.progress.research_missing",6,LIME,align="right",need_bind="!za.progress.research_max"),
        label(254,280,"NIVEAU MAXIMUM",6,LIME,need_bind="za.progress.research_max"),
        # All levels button tucked cleanly below the bar, NEVER covering Mable!
        button(636,270,204,54,"TOUS LES NIVEAUX","research_open_1",scale=5,bg="#FF0E3042",color=TEXT,border_color="#FF8CF4EA"),
    ]
    # 3 Research Level Cards with BOTH 1 x TM038 AND the localized TM move name right underneath!
    for i,title in enumerate(["NIVEAU PRÉCÉDENT","NIVEAU ACTUEL","NIVEAU SUIVANT"]):
        x=24+i*404;key=f"za.progress.r{i}.";gate=dict(need_bind=key+"valid")
        w += [panel(x,362,384,214,bg="#FF1C2E42" if i==1 else PANEL,edge=TEAL_MINT if i==1 else LINE),
              label(x+20,380,title,5,TEAL_MINT if i==1 else DIM,wrap_width=214,max_lines=1,fit_text=True),
              value(x+364,380,key+"points",5,GOLD,align="right",**gate),
              value(x+20,420,key+"level",8,TEXT,text="N. ",**gate),
              image(x+20,484,72,72,src_format="module:item/%d",bind=key+"icon",**gate),
              bound_text(x+106,488,key+"reward",5,"#FFFAF8E9",wrap_width=258,max_lines=1,fit_text=True,**gate),
              bound_text(x+106,524,key+"move",5,CYAN_LIGHT,wrap_width=258,max_lines=1,fit_text=True,**gate),
              label(x+20,436,"—",8,DIM,need_bind="!"+key+"valid"),
              button(x,362,384,214,"",f"research_open_{i}",bg=CLEAR,color=CLEAR,border=0,need_bind="!za.research.open")]
    # Z-A Royale Neon Magenta Hero Card (exact ryl_top_00.bflyt colors!)
    w += [image(24,594,1192,212,src="file:shapes/royale-card.png"),
          label(54,616,"ROYALE Z-A INFINI",6,PINK_GLOW,need_bind="za.progress.is_infinite"),
          label(54,616,"ROYALE Z-A",6,PINK_GLOW,need_bind="!za.progress.is_infinite"),
          bound_text(54,660,"za.progress.rank_fraction",9,TEXT),
          bar(54,724,820,20,"za.progress.rank_points",color=PINK_GLOW,max_bind="za.progress.rank_target",bg="#FF1C1636"),
          label(54,760,"POINTS TICKET",5,DIM),
          label(590,760,"PROCHAIN TICKET",5,DIM,align="right",wrap_width=230,max_lines=1,fit_text=True),
          value(874,756,"za.progress.rank_missing",6,GOLD,align="right",need_bind="za.progress.rank_known"),
          rect(948,616,236,166,bg="#FF1A0F2E",color=GOLD,frame=2),
          label(1066,626,"RANG",4,GOLD,align="center"),
          image(1010,656,112,112,src_format="module:royale-rank/%d",bind="za.progress.rank_id",need_bind="za.progress.rank_id"),
          bound_text(1066,684,"za.progress.rank",12,TEXT,align="center",need_bind="!za.progress.rank_id")]
    # Bottom Money (with native Pokedollar icon!) & Hyperspace Survey Points
    w += [panel(24,824,586,126,bg=PANEL,edge=LINE),
          rect(26,838,7,98,bg=GOLD,frame=0),
          label(52,842,"ARGENT DISPONIBLE",5,DIM,wrap_width=530,max_lines=1,fit_text=True),
          image(52,882,48,48,src="module:reward-money"),
          value(112,884,"za.progress.money",8,GOLD),
          panel(630,824,586,126,bg=PANEL,edge=LINE),
          rect(632,838,7,98,bg=RED,frame=0),
          label(658,842,"RECHERCHE INTERDIMENSIONNELLE",5,DIM,wrap_width=530,max_lines=1,fit_text=True),
          value(658,884,"za.progress.hyperspace",8,CYAN_LIGHT)]
    # Research levels drawer overlay
    gate=dict(need_bind="za.research.open")
    w += [rect(24,362,1192,588,bg="#FF141A2C",color=LINE_HI,frame=2,input_block=True,**gate),
          label(48,382,"NIVEAUX DE RECHERCHE",6,GOLD,**gate),
          button(1014,376,178,56,"FERMER","research_close",scale=5,bg=PANEL_HI,color=TEXT,border_color=LINE_HI,**gate),
          panel(430,448,762,478,bg=PANEL_HI)]
    w[-1].update(gate)
    rows=dict(repeat=50,repeat_bind="za.research.rows",repeat_dy=120,scroll="za_research",**gate)
    w += [rect(48,448,356,112,bg=PANEL,color=LINE,**rows),
          rect(48,448,356,112,color=RED,frame=3,hide_bind="za.rl{i}.selected",keep_min=1,keep_max=1,**rows),
          bound_text(64,460,"za.rl{i}.label",6,TEXT,wrap_width=322,max_lines=1,fit_text=True,**rows),
          image(64,498,52,52,src_format="module:item/%d",bind="za.rl{i}.icon",**rows),
          bound_text(130,498,"za.rl{i}.reward",5,DIM,wrap_width=258,max_lines=2,line_gap=4,fit_text=True,**rows),
          button(48,448,356,112,"","research_pick",payload="{i}",bg=CLEAR,color=CLEAR,border=0,**rows),
          image(458,472,88,88,src_format="module:item/%d",bind="za.research.icon",**gate),
          bound_text(566,476,"za.research.reward",6,TEXT,wrap_width=596,max_lines=2,line_gap=5,fit_text=True,**gate),
          label(566,554,"POINTS CUMULÉS",5,DIM,**gate), value(1168,554,"za.research.points",5,GOLD,align="right",**gate)]
    w += type_badge(566,596,240,"za.research.type",4,**gate)
    w += [bound_text(458,656,"za.research.description",5,TEXT,wrap_width=704,max_lines=32,line_gap=8,scroll="za_research_text",**gate)]
    w += not_ready()
    return dict(id="progress",title="Progression",nav_order=nav_order(),widgets=w,scrolls=[
        dict(scroll_region("za_research",[48,448,368,478],"za.research.rows",120),
             show_bind="za.research.open",follow_bind="za.research.follow",follow_open=True),
        dict(scroll_region("za_research_text",[458,656,718,244],"za.research.lines",34),
             show_bind="za.research.open")])

def notebook_page():
    rows = dict(repeat=64, repeat_bind="za.guide.objective_rows", repeat_dy=108, scroll="za_objectives")
    w = [label(28,32,"Carnet",8,TEXT),
         button(868,28,348,62,"Plan de capture","guide_capture",scale=5,bg=RED,color="#FF15343D"),
         label(28,100,"Épingle des Pokémon et des missions pour les retrouver ici.",5,DIM,
               hide_bind="za.guide.objective_rows",keep_min=0,keep_max=0),
         rect(24,148,1192,98,bg=PANEL,color=LINE,**rows),
         image(36,158,80,80,src_format="module:icon/%d",bind="za.go{i}.icon",
               hide_bind="za.go{i}.mission",hide_eq=1,**rows),
         image(53,174,46,46,src="module:menu/4",need_bind="za.go{i}.mission",**rows),
         bound_text(130,181,"za.go{i}.label",6,TEXT,wrap_width=632,max_lines=1,fit_text=True,**rows),
         button(790,166,200,62,"Ouvrir","guide_open",payload="{i}",scale=5,**rows),
         button(1002,166,198,62,"Retirer","guide_remove",payload="{i}",scale=5,**rows)]
    return dict(id="notebook",title="Carnet",nav_order=nav_order(),widgets=w,
                scrolls=[scroll_region("za_objectives",[24,148,1192,784],"za.guide.objective_rows",108)])


def capture_page():
    rows=dict(repeat=20,repeat_bind="za.capture.rows",repeat_dy=154,scroll="za_capture")
    w=[
        label(24,34,"Plan de capture",8,TEXT),
        button(970,24,246,66,"Carnet","go_notebook",scale=5),
        value(24,96,"za.capture.targets",5,DIM,text="Cibles à capturer :"),
        value(24,134,"za.capture.unknown",5,DIM,text="Sans zone connue :",need_bind="za.capture.unknown"),
        label(34,212,"Épingle des Pokémon non capturés dans le Pokédex.",5,DIM,
              hide_bind="za.capture.targets",keep_min=0,keep_max=0),
        rect(24,185,1192,142,bg=PANEL,color=LINE,**rows),
        rect(24,185,1192,142,bg="#FF244452",color=RED,frame=2,need_bind="za.cp{i}.recommended",**rows),
        value(50,202,"za.cp{i}.zone",7,TEXT,text="ZONE SAUVAGE ",**rows),
        value(1182,207,"za.cp{i}.count",5,GOLD,text="Cibles :",align="right",**rows),
        bound_text(50,252,"za.cp{i}.names",5,DIM,wrap_width=778,max_lines=2,line_gap=5,**rows),
        button(882,244,302,66,"VOIR SUR LA CARTE","guide_capture_map",payload="{i}",scale=5,**rows),
        label(50,892,"Les zones avec le plus de cibles apparaissent en premier.",5,DIM,
              wrap_width=1140,max_lines=1,fit_text=True,need_bind="za.capture.rows"),
    ]
    return dict(id="capture",title="Plan de capture",nav_order=nav_order(),widgets=w,
                scrolls=[scroll_region("za_capture",[24,185,1192,690],"za.capture.rows",154)])


def search_page():
    keys=dict(repeat=30,repeat_bind="za.search.keys",repeat_cols=6,repeat_dx=77,repeat_row_dy=66)
    rows=dict(repeat=1024,repeat_bind="za.search.rows",repeat_dy=96,scroll="za_search")
    w=[
        label(24,30,"Rechercher",8,TEXT),
        button(506,20,350,62,"POKÉDEX","search_mode_dex",scale=5),
        button(866,20,350,62,"BOÎTES","search_mode_box",scale=5),
        rect(506,20,350,62,color=GOLD,frame=3,hide_bind="za.search.mode",keep_min=0,keep_max=0),
        rect(866,20,350,62,color=GOLD,frame=3,need_bind="za.search.mode"),
        rect(24,112,460,94,bg=PANEL_HI,color=LINE),
        bound_text(42,136,"za.search.query",7,TEXT,wrap_width=425,max_lines=1,fit_text=True),
        label(42,143,"Nom ou numéro",5,DIM,need_bind="!za.search.typing"),
        button(24,222,222,56,"‹","search_keyboard_prev",scale=6,need_bind="za.search.previous"),
        button(258,222,222,56,"›","search_keyboard_next",scale=6,need_bind="za.search.next"),
        rect(24,294,69,58,bg=PANEL_HI,color=LINE,**keys),
        bound_text(58,309,"za.sk{i}.key",6,TEXT,align="center",**keys),
        button(24,294,69,58,"","search_key",payload="{i}",bg=CLEAR,color=CLEAR,border=0,**keys),
        button(24,802,222,60,"Dernière lettre","search_erase",scale=5),
        button(258,802,222,60,"Effacer tout","search_clear",scale=5),
        button(24,882,460,56,"POKÉDEX","go_dex",scale=5,need_bind="!za.search.mode"),
        button(24,882,460,56,"BOÎTES","go_box",scale=5,need_bind="za.search.mode"),
        value(514,113,"za.search.rows",5,GOLD,text="Résultats :",need_bind="za.search.typing"),
        label(514,185,"Aucun résultat",6,DIM,need_bind="za.search.empty"),
        rect(506,162,710,88,bg=PANEL,color=LINE,**rows),
        image(518,166,80,80,src_format="module:icon/%d",bind="za.sr{i}.icon",**rows),
        bound_text(612,176,"za.sr{i}.name",6,TEXT,wrap_width=572,max_lines=1,fit_text=True,**rows),
        bound_text(612,214,"za.sr{i}.context",5,DIM,wrap_width=572,max_lines=1,fit_text=True,**rows),
        button(506,162,710,88,"","search_pick",payload="{i}",bg=CLEAR,color=CLEAR,border=0,**rows),
        bound_text(514,905,"za.search.error",5,GOLD,wrap_width=688,max_lines=1,fit_text=True),
    ]
    for i in range(10):
        w.append(button(24+i%5*92,650+i//5*70,82,60,str(i),"search_digit",payload=i,scale=6))
    return dict(id="search",title="Rechercher",nav_order=nav_order(),widgets=w,
                scrolls=[scroll_region("za_search",[506,162,710,724],"za.search.rows",96,"za.search.reset")])


def guide_header(caption):
    return [image(30,22,126,126,src_format="module:icon/%d",bind="za.guide.icon"),
            label(172,33,caption,5,DIM),
            bound_text(172,77,"za.guide.name",8,TEXT,wrap_width=800,max_lines=1,fit_text=True),
            button(1000,28,216,66,"POKÉDEX","go_dex",scale=5)]


def locations_page():
    rows = dict(repeat=80, repeat_bind="za.guide.location_rows", repeat_dy=100, scroll="za_locations")
    w = guide_header("Où le trouver ?") + [
        label(30,164,"Rencontres de l’espèce, toutes formes confondues",5,DIM),
        label(30,245,"Aucun lieu connu dans les données disponibles.",6,DIM,
              hide_bind="za.guide.location_rows",keep_min=0,keep_max=0),
        rect(24,222,1192,90,bg=PANEL,color=LINE,**rows),
        image(44,244,44,44,src="module:map-portal-wild",need_bind="za.gl{i}.live",**rows),
        bound_text(108,251,"za.gl{i}.label",6,TEXT,wrap_width=724,max_lines=1,fit_text=True,**rows),
        button(868,236,328,62,"VOIR SUR LA CARTE","guide_map",payload="{i}",scale=5,**rows)]
    return dict(id="locations",title="Où le trouver ?",nav_order=nav_order(),widgets=w,
                scrolls=[scroll_region("za_locations",[24,222,1192,710],"za.guide.location_rows",100)])


def combat_page():
    w = guide_header("Adversaire choisi dans le Pokédex")
    w += type_badge(172,122,208,"za.guide.type1",5)+type_badge(398,122,208,"za.guide.type2",5)
    w += [label(28,180,"Faiblesses",6,TEXT),
          value(1212,184,"za.guide.ready_moves",5,GREEN,text="Attaques efficaces prêtes :",align="right",
                need_bind="za.guide.ready_known"),
          label(1212,278,"Prêtes à utiliser en vert",5,DIM,align="right",need_bind="za.guide.ready_known")]
    for i in range(18):
        x=28+i%9*132
        y=218+i//9*47
        gate=dict(hide_bind="za.guide.weaknesses",keep_min=i+1,keep_max=18)
        w += [rect(x,y,120,41,bg=PANEL_HI,**gate),
              image(x+12,y+5,31,31,src_format="module:type/%d",bind=f"za.gw{i}.type",**gate),
              bound_text(x+55,y+8,f"za.gw{i}.factor",5,GOLD,**gate)]
    for slot in range(6):
        x=24+slot%2*602
        y=322+slot//2*203
        gate=dict(need_bind=f"za.p{slot}.v")
        w += [rect(x,y,588,190,bg=PANEL,color=LINE,**gate),
              image(x+8,y+8,56,56,src_format="module:icon/%d",bind=f"za.p{slot}.icon",**gate),
              bound_text(x+69,y+17,f"za.p{slot}.name",6,TEXT,wrap_width=505,max_lines=1,fit_text=True,**gate)]
        for i in range(4):
            a=x+12+(i%2)*284
            b=y+66+(i//2)*56
            prefix=f"za.gc{slot*4+i}"
            w += [rect(a,b,276,48,bg=PANEL_HI,**gate),
                  rect(a,b,276,48,bg="#FF594B23",need_bind=prefix+".strong"),
                  rect(a,b,276,48,bg="#FF145B46",color=GREEN,frame=3,need_bind=prefix+".ready"),
                  bound_text(a+8,b+11,prefix+".name",5,TEXT,wrap_width=207,max_lines=1,fit_text=True,**gate),
                  bound_text(a+266,b+11,prefix+".factor",5,GOLD,align="right",**gate)]
    return dict(id="combat",title="Aide au combat",nav_order=nav_order(),widgets=w)


def evolutions_page():
    rows = dict(repeat=64,repeat_bind="za.guide.evolution_rows",repeat_dy=172,scroll="za_evolutions")
    w = guide_header("Évolutions") + [
        label(32,204,"Aucune évolution connue pour cette forme.",6,DIM,
              hide_bind="za.guide.evolution_rows",keep_min=0,keep_max=0),
        rect(24,174,1192,158,bg=PANEL,color=LINE,**rows),
        image(38,184,112,112,src_format="module:icon/%d",bind="za.ge{i}.from_icon",**rows),
        bound_text(161,204,"za.ge{i}.from",6,TEXT,wrap_width=250,max_lines=1,fit_text=True,**rows),
        label(435,204,"→",6,GOLD,**rows),
        image(476,184,112,112,src_format="module:icon/%d",bind="za.ge{i}.to_icon",**rows),
        bound_text(604,204,"za.ge{i}.to",7,TEXT,wrap_width=570,max_lines=1,fit_text=True,**rows),
        bound_text(604,245,"za.ge{i}.condition",5,DIM,wrap_width=570,max_lines=2,line_gap=5,fit_text=True,**rows),
        value(603,300,"za.ge{i}.stock",5,GOLD,text="EN STOCK ",need_bind="za.ge{i}.item",**rows),
        image(870,280,42,42,src_format="module:item/%d",bind="za.ge{i}.item",need_bind="za.ge{i}.item",**rows),
        button(924,280,276,44,"Voir dans le sac","guide_item",payload="{i}",
               scale=5,need_bind="za.ge{i}.item",**rows),
    ]
    return dict(id="evolutions",title="Évolutions",nav_order=nav_order(),widgets=w,
                scrolls=[scroll_region("za_evolutions",[24,174,1192,758],"za.guide.evolution_rows",172)])


for guide_action in ("guide_locations","guide_combat","guide_dex_pin","guide_evolutions","quest_pin","quest_load_map",
                     "guide_open","guide_remove","guide_map","zone_missing_toggle","guide_capture","guide_capture_map",
                     "guide_item","guide_item_back","map_panel","map_group","map_choice","search_key","search_digit",
                     "search_clear","search_erase","search_pick"):
    actions[guide_action]=dict(kind="module",action=guide_action)
    if guide_action in ("guide_open","guide_remove","guide_map","guide_capture_map","guide_item","map_group",
                        "map_choice","search_key","search_digit","search_pick"):
        actions[guide_action]["argument"]="$payload"
for suffix, mode in (("dex",0),("box",1)):
    actions[f"search_from_{suffix}"]=dict(kind="module",action="search_open",argument=mode)
    actions[f"search_mode_{suffix}"]=dict(kind="module",action="search_mode",argument=mode)
actions["search_keyboard_prev"]=dict(kind="module",action="search_keyboard",argument=-1)
actions["search_keyboard_next"]=dict(kind="module",action="search_keyboard",argument=1)
for suffix, mode in (("heal",1),("hold",2),("tm",3),("evolve",4)):
    actions[f"bag_tool_{suffix}"]=dict(kind="module",action="bag_help",argument=mode)
for action in ("bag_favor","bag_favorite_pick","bag_target","bag_fiche","bag_hold_team","bag_hold_box",
               "bag_hold_browse","bag_hold_clear","bag_hold_end","bag_hold_apply","bag_heal_apply","moves_bag"):
    actions[action]=dict(kind="module",action=action)
    if action in ("bag_favorite_pick","bag_target"):
        actions[action]["argument"]="$payload"
actions["go_bag_tools"]=dict(kind="page",page="bag_tools")
for action in ("map_recenter","team_heal_open","team_heal_pick","team_heal_back"):
    actions[action]=dict(kind="module",action=action)
actions["team_heal_pick"]["argument"]="$payload"


manifest = {
    "format": 1,
    "title_id": "0100F43008C44000",
    "name": "Pokemon Legends Z-A dual screen",
    "_about": "Seven tabs read from the player's active state and save by the native module. "
              "Art, names and UI translations are decoded from the player's romfs in the save language. "
              "The UI font is decoded at runtime from the player's romfs.",
    "min_runtime": 25 if RUNTIME_25 else (19 if LEGACY_NAV else 22),
    "font": "file:font/za_font.txt",
    "font_atlas": "module:font/{p}",
    "font_page_h": 1024,
    "requires_module": True,
    "canvas_w": W,
    "canvas_h": H,
    "background": BG,
    "nav": {"color": RED, "frame": 4},
    "actions": actions,
    "page_binds": [dict(point="za.quest.show_map", equals=1, ready_bind="za.ready",
                       when_equal=dict(page="map", transition="fade", duration_ms=120))] +
                  [dict(point="za.guide.page",equals=i,ready_bind="za.ready",
                        when_equal=dict(page=page,transition="fade",duration_ms=120))
                   for i,page in enumerate(("locations","combat","missions","evolutions","capture","search","dex","bag","bag_tools","team","team_heal"),1)],
    "enforce":[{"action":f"input_{command}","every_ms":1} for command in ["left","right","send","aim","attack_0","attack_1","attack_2","attack_3"]],
    "pages": [team_page(), dex_page(), box_page(), box_detail_page(), map_page(), bag_page(), missions_page(),
              progress_page(),notebook_page(),locations_page(),combat_page(),evolutions_page(),capture_page(),search_page(),bag_tools_page(),team_heal_page()],
    "flags": {"za_move_grid": 0},
    "settings": [
        {
            "flag": "za_move_grid",
            "label": "Move layout",
            "type": "toggle",
            "default": 0,
            "choices": ["ABXY Diamond", "2x2 Grid"],
        }
    ],
    "persist_flags": [f"za_notebook_{slot}" for slot in range(64)],
}

manifest["page_binds"] += [
    dict(point="za.nav.destination",equals=i,when_equal=dict(page=page,transition="fade",duration_ms=120))
    for i,(page,_) in enumerate(TABS,1)
]

for slot, flag in enumerate(manifest["persist_flags"]):
    action = f"store_notebook_{slot}"
    actions[action] = dict(kind="flag",flag=flag,value=f"$za.note.value{slot}",
                           enabled_bind=f"za.note.pending{slot}")
    manifest["enforce"].append(dict(action=action,every_ms=1))
manifest["persist_flags"].extend(["za_nav_shortcuts", "za_move_grid"])
actions["store_nav_shortcuts"]=dict(kind="flag",flag="za_nav_shortcuts",value="$za.nav.encoded",
                                   enabled_bind="za.nav.pending")
manifest["enforce"].append(dict(action="store_nav_shortcuts",every_ms=100))
for slot in range(4):
    flag=f"za_bag_favorite_{slot}"
    action=f"store_bag_favorite_{slot}"
    manifest["persist_flags"].append(flag)
    actions[action]=dict(kind="flag",flag=flag,value=f"$za.bf{slot}.value",enabled_bind=f"za.bf{slot}.pending")
    manifest["enforce"].append(dict(action=action,every_ms=1))

actions["input_aim"]=dict(kind="button",button="ZL",frames=26,enabled_bind="za.cmd.aim")
actions["input_marker_open"]=dict(kind="button",button="Plus",frames=8,enabled_bind="za.cmd.marker_open")
actions["input_marker_close"]=dict(kind="button",button="B",frames=8,enabled_bind="za.cmd.map_close")
manifest["enforce"].append(dict(action="input_marker_open",every_ms=1))
manifest["enforce"].append(dict(action="input_marker_close",every_ms=1))
for i,token in enumerate("XAYB"):
    actions[f"attack_{i}"]=dict(kind="module",action="attack",argument=i)
    actions[f"input_attack_{i}"]=dict(kind="button",button=token,frames=8,enabled_bind=f"za.cmd.attack{i}")

manifest["page_binds"].append(dict(point="za.boxdetail.toggle",equals=0,ready_bind="za.ready",
    when_equal=dict(page="box_detail",transition="fade",duration_ms=120),
    when_not_equal=dict(page="box_detail",transition="fade",duration_ms=120)))
# Recover the old header space on existing pages without changing footer or bound map markers.
for page in manifest["pages"]:
    if page["id"] in {"team","dex","box_detail","progress","map","missions","notebook","locations","combat","evolutions","capture","search","bag","bag_tools","team_heal"}:continue
    for w in page["widgets"]:
        if "y_bind" not in w and 108<=w["rect"][1]<932:
            w["rect"][1]-=104
    for scroll in page.get("scrolls",[]):
        scroll["rect"][1]-=104
        if scroll["id"]!="za_dimension":scroll["rect"][3]+=104
    if page["id"]=="dex":
        # Keep these controls below the full description viewport.
        page["widgets"] += [
            button(568,882,140,62,"Habitats","guide_locations",scale=5,need_bind="za.guide.dex_ready"),
            button(716,882,140,62,"Combat","guide_combat",scale=5,need_bind="za.guide.dex_ready"),
            button(864,882,166,62,"Évolutions","guide_evolutions",scale=5,need_bind="za.guide.dex_ready"),
            button(1038,882,166,62,"Épingler","guide_dex_pin",scale=5,need_bind="za.guide.dex_ready",
                   hide_bind="za.guide.dex_pin",hide_eq=1),
            button(1038,882,166,62,"Désépingler","guide_dex_pin",scale=5,need_bind="za.guide.dex_pin"),
        ]
    if page["id"]=="box":
        swipe=dict(on_swipe_left="box_next",on_swipe_right="box_prev",swipe_px=54,need_bind="za.ready")
        page["widgets"].append(rect(GRID_X,GRID_Y-104,CELL_DX*5+CELL_W,CELL_DY*4+CELL_H,
                                    frame=0,**swipe))
        cells=dict(swipe,need_bind="za.b{i}.v")
        page["widgets"].append(button(GRID_X,GRID_Y-104,CELL_W,CELL_H,"","box_details",payload="{i}",
            bg=CLEAR,color=CLEAR,border=0,repeat=30,repeat_cols=6,repeat_dx=CELL_DX,repeat_row_dy=CELL_DY,**cells))
        picker=dict(need_bind="za.box.picker")
        tiles=dict(**picker,repeat=32,repeat_bind="za.box.count",repeat_cols=8,repeat_dx=142,repeat_row_dy=116)
        page["widgets"] += [
            rect(0,0,W,958,bg="#B0081825",frame=0,input_block=True,**picker),
            rect(24,162,1192,662,bg=PANEL,color=LINE,**picker),
            label(54,217,"Choisir une boîte",8,TEXT,**picker),
            button(958,190,226,70,"Fermer","box_picker",scale=5,**picker),
            button(60,300,126,96,"","box_set",payload="{i}",**tiles),
            rect(60,300,126,96,color=RED,frame=4,need_bind="za.box.tile{i}.selected",
                 **{k:v for k,v in tiles.items() if k!="need_bind"}),
            value(123,334,"za.box.tile{i}.number",7,TEXT,align="center",**tiles),
        ]
        for widget in page["widgets"]:
            if widget.get("text")=="Glisse pour changer de boîte":
                widget["rect"][1]=924
        for w in page["widgets"]:
            if w.get("bind") in {"za.box.used","za.box.count","za.box.unlocked"} and w["rect"][1]>700:w["rect"][1]=924


# The companion offers no menu-driven teleportation or automatic storage exchange.
for page in manifest["pages"]:
    page["widgets"]=[w for w in page["widgets"] if w.get("on_tap") not in {"center_travel","travel_cancel","game_map","game_destinations","game_menu","go_box_controls","box_pick","box_swap_confirm"}]
for key in list(actions):
    if key.startswith(("input_swap_","input_map_","game_box_")) or key in {"center_travel","travel_cancel","box_pick","box_swap_confirm","box_swap_pick","box_swap_cancel","go_box_controls"}:actions.pop(key)

# A host that advertises native calls can borrow the scene thread for a one-shot exchange.
# No input enforce rule and no periodically repeated sequence: exchange is involutive.
actions["native_box_prepare"]=dict(kind="module",action="native_box_prepare")
actions["native_box_party"]=dict(kind="module",action="native_box_party",argument="$payload")
actions["native_box_cancel"]=dict(kind="module",action="native_box_cancel")
actions["native_box_exchange"]=dict(kind="sequence",sequence="za_box_exchange",enabled_bind="za.nativebox.ready")
manifest["frame_hook"]="$za_scene_update"
manifest["sequences"]={"za_box_exchange":dict(steps=[dict(fn="za_box_exchange",
    args=["0","$za.nativebox.box","$za.nativebox.slot","$za.nativebox.party"],out="za.nativebox.result")])}
for page in manifest["pages"]:
    if page["id"]=="box_detail":
        page["widgets"].append(button(674,860,514,62,"ÉCHANGER AVEC L'ÉQUIPE","native_box_prepare",scale=5,need_bind="za.nativebox.available"))
w=[label(48,32,"ÉCHANGER AVEC L'ÉQUIPE",8),
   image(48,104,160,160,src_format="module:icon/%d",bind="za.nativebox.icon"),
   bound_text(250,154,"za.nativebox.name",8,TEXT,wrap_width=930,max_lines=1,fit_text=True),
   label(48,274,"Choisissez le Pokémon de l'équipe à échanger.",5,DIM,wrap_width=1140,max_lines=2)]
for i in range(6):
    x=48+(i%2)*576;y=330+(i//2)*134;p=f"za.p{i}."
    w += [panel(x,y,550,116),image(x+8,y+6,104,104,src_format="module:icon/%d",bind=p+"icon"),
          bound_text(x+130,y+22,p+"sname",6,TEXT,wrap_width=396,max_lines=1,fit_text=True),
          value(x+130,y+70,p+"lvl",5,DIM,text="N. "),
          rect(x,y,550,116,color=GOLD,frame=3,hide_bind="za.nativebox.party",keep_min=i,keep_max=i),
          button(x,y,550,116,"","native_box_party",payload=i,bg=CLEAR,color=CLEAR,border=0,need_bind=p+"v")]
w += [button(48,756,832,76,"EFFECTUER L'ÉCHANGE","native_box_exchange",scale=6,need_bind="za.nativebox.ready"),
      button(904,756,284,76,"ANNULER","native_box_cancel",scale=5,hide_bind="za.nativebox.done",hide_eq=1),
      button(904,756,284,76,"RETOUR","native_box_cancel",scale=5,need_bind="za.nativebox.done"),
      bound_text(48,858,"za.nativebox.status",5,GOLD,wrap_width=1140,max_lines=2,line_gap=8)]
manifest["pages"].append(dict(id="box_exchange",title="Échange",widgets=w,
    scrolls=[]))
manifest["page_binds"].append(dict(point="za.nativebox.open",equals=1,ready_bind="za.ready",
    when_equal=dict(page="box_exchange",transition="fade",duration_ms=120),
    when_not_equal=dict(page="box",transition="fade",duration_ms=120)))
for name in ["moves_team","moves_box"]:
    actions[name]=dict(kind="module",action=name)
for name in ["moves_slot","moves_pick"]:
    actions[name]=dict(kind="module",action=name,argument="$payload")
actions["go_box_detail"]=dict(kind="page",page="box_detail")
actions["moves_apply"]=dict(kind="call",fn="main+0x2D75328",
    args=["$za.moves.target","$za.moves.slot","$za.moves.desired"],enabled_bind="za.moves.can_apply")
if LEGACY_NAV:
    manifest["load_plan"] = "load-plans/<build>.json"
    manifest.pop("frame_hook")
    manifest.pop("sequences")
    actions["native_box_exchange"] = dict(kind="module", action="native_box_exchange_guest",
                                          enabled_bind="za.nativebox.ready")
    actions["moves_apply"] = dict(kind="module", action="moves_apply_guest",
                                  enabled_bind="za.moves.can_apply")
    actions["native_map_travel"] = dict(kind="module", action="native_map_travel",
                                        enabled_bind="za.map.travel_ready")
    map_page = next(page for page in manifest["pages"] if page["id"] == "map")
    map_widgets = map_page["widgets"]
    for y, gate in ((786, "za.center.basic"), (828, "za.zone.list_visible")):
        disabled = dict(need_bind=gate, hide_bind="za.map.travel_ready", hide_eq=1)
        enabled = dict(need_bind=gate, hide_bind="za.map.travel_ready", hide_eq=0)
        map_widgets += [
            bound_text(780, y-64, "za.map.travel_status", 5, GOLD, wrap_width=422,
                       max_lines=2, need_bind=gate),
            rect(780, y, 422, 62, bg="#FF303943", color="#FF5B646D", **disabled),
            bound_text(991, y+18, "za.map.travel_caption", 5, "#FF9CA5AE", align="center",
                       wrap_width=398, max_lines=1, fit_text=True, **disabled),
            button(780, y, 422, 62, "", "native_map_travel", scale=5, **enabled),
            bound_text(991, y+18, "za.map.travel_caption", 5, TEXT, align="center",
                       wrap_width=398, max_lines=1, fit_text=True, **enabled),
        ]

def move_menu_page():
    detail=dict(scroll="za_move_text")
    w=header("moves")+[panel(24,24,1188,104),
        image(40,32,88,88,src_format="module:icon/%d",bind="za.moves.icon"),
        bound_text(148,38,"za.moves.pokemon",8,TEXT,wrap_width=790,max_lines=1,fit_text=True),
        label(150,92,"CAPACITÉS",4,DIM),
        button(1000,42,188,64,"RETOUR","go_team",scale=5,need_bind="za.moves.team_origin"),
        button(1000,42,188,64,"SAC","go_bag_tools",scale=5,need_bind="za.moves.frombag"),
        button(1000,42,188,64,"RETOUR","go_box_detail",scale=5,need_bind="za.moves.frombox")]
    for i in range(4):
        x=24+i*300;p=f"za.moves.e{i}."
        w += [panel(x,150,288,132),
              bound_text(x+14,162,p+"name",5,TEXT,wrap_width=260,max_lines=2,line_gap=4,fit_text=True)]
        w += move_type_badge(x+12,230,264,p+"type")
        w += [rect(x,150,288,132,color=GOLD,frame=3,need_bind=p+"selected"),
              button(x,150,288,132,"","moves_slot",payload=i,bg=CLEAR,color=CLEAR,border=0)]
    w += [label(32,292,"Choisissez une capacité à remplacer, puis une capacité disponible.",4,DIM,wrap_width=1176,max_lines=1,fit_text=True),
          panel(24,332,548,612),panel(592,332,620,612),
          label(40,350,"CAPACITÉS DISPONIBLES",5,GOLD),
          bound_text(616,350,"za.moves.name",8,TEXT,wrap_width=568,max_lines=2,fit_text=True,**detail)]
    grid=dict(repeat=192,repeat_bind="za.moves.rows",repeat_dy=126,scroll="za_moves")
    gate=dict(need_bind="za.lm{i}.v")
    w += [rect(40,398,516,116,bg=PANEL,color=LINE,frame=2,**grid,**gate),
          bound_text(56,410,"za.lm{i}.name",6,TEXT,wrap_width=484,max_lines=1,fit_text=True,**grid,**gate),
          bound_text(300,450,"za.lm{i}.origin",5,DIM,wrap_width=240,max_lines=2,line_gap=3,text_center_h=54,fit_text=True,**grid,**gate)]
    w += move_type_badge(56,456,224,"za.lm{i}.type",**grid,**gate)
    w += [rect(40,398,516,116,color=GOLD,frame=3,need_bind="za.lm{i}.selected",**grid),
          button(40,398,516,116,"","moves_pick",payload="{i}",bg=CLEAR,color=CLEAR,border=0,**grid,**gate)]
    w+=type_badge(616,458,568,"za.moves.type",5,**detail)
    for x,title,key in [(616,"CATÉGORIE","category"),(806,"PUISSANCE","power"),(996,"PRÉCISION","accuracy")]:
        w += [label(x,532,title,4,DIM,**detail),
              bound_text(x,574,"za.moves."+key,6,TEXT,wrap_width=180,max_lines=1,fit_text=True,**detail)]
    w += [bound_text(616,638,"za.moves.description",6,TEXT,wrap_width=568,max_lines=128,line_gap=8,
                     auto_w=True,repeat=1,**detail),
          bound_text(616,608,"za.moves.compare",5,DIM,wrap_width=568,max_lines=1,fit_text=True,need_bind="za.moves.frombag",**detail),
          bound_text(616,788,"za.moves.status",4,GOLD,wrap_width=568,max_lines=2,line_gap=5)]
    w += [button(616,854,568,68,"REMPLACER LA CAPACITÉ","moves_apply",scale=5,need_bind="za.moves.can_apply")]
    scrolls=[scroll_region("za_moves",[40,398,516,526],"za.moves.rows",126,"za.moves.reset"),
             dict(id="za_move_text",rect=[602,350,598,424],reset_bind="za.moves.detail_reset",
                  fling=True,pad=10,bar=GOLD,bar_track=LINE,bar_w=7)]
    return dict(id="moves",title="Capacités",widgets=w,scrolls=scrolls)

manifest["pages"].append(move_menu_page())
manifest["page_binds"].append(dict(point="za.moves.toggle",equals=0,ready_bind="za.ready",
    when_equal=dict(page="moves",transition="fade",duration_ms=120),
    when_not_equal=dict(page="moves",transition="fade",duration_ms=120)))


def round_texture(bg, edge, border, radius=14):
    size=max(48, radius*2+16)
    name=hashlib.sha256(f"v3/{bg}/{edge}/{border}/{radius}/{size}".encode()).hexdigest()[:16]+".png"
    directory=OUT.parent/"shapes";directory.mkdir(parents=True,exist_ok=True)
    target=directory/name
    def rgba(color):
        n=int(color.lstrip("#"),16);return [(n>>16)&255,(n>>8)&255,n&255,(n>>24)&255]
    fill,frame=rgba(bg),rgba(edge)
    img=Image.new("RGBA",(size,size),(0,0,0,0))
    px=img.load()
    for y in range(size):
        for x in range(size):
            qx=max(radius-(x+.5),0,(x+.5)-(size-radius))
            qy=max(radius-(y+.5),0,(y+.5)-(size-radius))
            dist=math.hypot(qx,qy)-radius
            cov=min(1.0,max(0.0,0.5-dist))
            if cov<=0: continue
            if border and dist>-border:
                c=frame
            else:
                c=fill
                # Subtle architectural top inner highlight on filled cards
                if c[3]>180 and y==int(border)+1 and radius<x<size-radius:
                    c=[min(255,c[0]+22),min(255,c[1]+26),min(255,c[2]+34),c[3]]
            px[x,y]=(c[0],c[1],c[2],round(c[3]*cov))
    img.save(target,optimize=True)
    return "file:shapes/"+name


def enforce_scroll_gutters(pages, min_gap=11):
    """Ensure NO list row card or button ever overlaps or crowds a scroll_region's vertical bar."""
    for page in pages:
        scrolls = {sc["id"]: sc for sc in page.get("scrolls", [])}
        if not scrolls:
            continue
        for w in page["widgets"]:
            sid = w.get("scroll")
            if sid not in scrolls:
                continue
            sc = scrolls[sid]
            sx, sy, sw, sh = sc["rect"]
            bar_x0 = sx + sw - sc.get("bar_w", 7)
            max_right = bar_x0 - min_gap
            r = w["rect"]
            if r[2] > 0 and r[0] + r[2] > max_right:
                shrink = (r[0] + r[2]) - max_right
                r[2] = max(24, r[2] - shrink)
            elif r[2] == 0 and w.get("align") == "right" and r[0] > max_right - 14:
                r[0] = max_right - 14
            if w.get("wrap_width") and r[0] + w["wrap_width"] > max_right - 12:
                w["wrap_width"] = max(60, (max_right - 12) - r[0])


def round_pages(pages):
    enforce_scroll_gutters(pages, min_gap=11)
    for page in pages:
        out=[]
        for w in page["widgets"]:
            kind=w.get("type");width,height=w["rect"][2:]
            is_pill=bool(w.pop("pill", False))
            if kind in ("rect","button") and ((is_pill and width>=24 and height>=24) or (width>=48 and height>=24)):
                bg=w.get("bg",CLEAR)
                edge=w.pop("border_color", LINE_HI if kind=="button" else w.get("color",CLEAR))
                border=w.get("border",2) if kind=="button" else w.get("frame",2)
                if bg!=CLEAR or (edge!=CLEAR and border):
                    surface={k:v for k,v in w.items() if k not in {"type","id","text","text_scale","align","on_tap","on_hold","nav","border","color","bg","frame","bind_text","table","max_lines","wrap_width","fit_text","input_block","border_color","pill"}}
                    rad=(min(width,height)//2) if is_pill else (14 if height>=44 else max(6,height//3))
                    surface.update(type="image",src=round_texture(bg,edge,border,rad),fill="slice",slice=[rad,rad,rad,rad],color="#FFFFFFFF")
                    if kind=="rect":
                        if w.get("input_block"):surface["input_block"]=True
                        out.append(surface);continue
                    out.append(surface);w=dict(w,bg=CLEAR,border=0)
            elif kind=="bar":
                radius=max(1,min(8,height//2));bg=w.get("bg",WELL)
                background={k:v for k,v in w.items() if k in {"rect","repeat","repeat_cols","repeat_dx","repeat_dy","repeat_row_dy","need_bind","hide_bind","keep_min","keep_max","hide_eq"}}
                background.update(type="image",src=round_texture(bg,"#FF344060",1,radius),fill="slice",slice=[radius]*4,color="#FFFFFFFF")
                out.append(background)
                w=dict(w,bg=CLEAR,image=round_texture(w["color"],CLEAR,0,radius),fill="slice",slice=[radius]*4,frame=0)
            out.append(w)
        page["widgets"]=out


def write_send_arrow():
    d=OUT.parent/"shapes";d.mkdir(parents=True,exist_ok=True)
    # 4x supersampled anti-aliased Lumiose up-arrow for dedicated Send Out pillar
    sa4=Image.new("RGBA",(384,384),(0,0,0,0))
    dr=ImageDraw.Draw(sa4)
    dr.ellipse([32,32,352,352],fill=(255,255,255,44),outline=(171,228,255,235),width=12)
    dr.polygon([(192,96),(280,200),(228,200),(228,288),(156,288),(156,200),(104,200)],fill=(255,255,255,255))
    sa4.resize((96,96),Image.Resampling.LANCZOS).save(d/"send-arrow.png",optimize=True)
    # 4x supersampled anti-aliased centered More (+) icon for bottom nav dock
    mi4=Image.new("RGBA",(152,152),(0,0,0,0))
    md=ImageDraw.Draw(mi4)
    md.ellipse([10,10,142,142],fill=(47,179,235,34),outline=(132,216,255,230),width=10)
    md.rounded_rectangle([67,38,85,114],radius=7,fill=(171,228,255,255))
    md.rounded_rectangle([38,67,114,85],radius=7,fill=(171,228,255,255))
    mi4.resize((38,38),Image.Resampling.LANCZOS).save(d/"more-icon.png",optimize=True)

    # Dimension portals & map arrow
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    for kind,color in enumerate(((175,128,255),(255,150,78))):
        portal=bytearray()
        for y in range(64):
            portal.append(0)
            for x in range(64):
                dx,dy=x-31.5,y-31.5;dist=math.hypot(dx,dy)
                ring=22<dist<28;inner=abs(dx)/15+abs(dy)/21<1
                glyph=(abs(dx)<2 or abs(dy)<2) and dist<9 if kind==0 else (abs(dx-dy)<3 or abs(dx+dy)<3) and dist<11
                pixel=(249,250,255,255) if glyph else (*color,255) if ring else (31,29,63,245) if inner else (0,0,0,0)
                portal.extend(pixel)
        (d/f"dimension-{kind}.png").write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',64,64,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(portal),9))+chunk(b'IEND',b''))
    arrow=bytearray()
    for y in range(64):
        arrow.append(0)
        for x in range(64):
            width=0 if y<5 or y>=57 else min(23,(y-5)*.52)
            inside=width>0 and abs(x-31.5)<width and y<57-abs(x-31.5)*.50
            outline=width>0 and (width-abs(x-31.5)<3 or y>52-abs(x-31.5)*.50)
            arrow.extend((18,36,58,255) if inside and outline else (47,179,235,255) if inside else (0,0,0,0))
    (d/"map-arrow.png").write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',64,64,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(arrow),9))+chunk(b'IEND',b''))


def write_backdrop():
    d=OUT.parent/"shapes";d.mkdir(parents=True,exist_ok=True)
    # Full 1:1 native 1240x1080 Parisian night indigo + crisp Lumiose dot matrix + cyan/lime top razor
    bw,bh=W,H
    img=Image.new("RGBA",(bw,bh),(16,20,34,255))
    dr=ImageDraw.Draw(img)
    for y in range(bh):
        t=y/bh
        dr.line([(0,y),(bw,y)],fill=(int(16*(1-t)+11*t),int(20*(1-t)+14*t),int(34*(1-t)+24*t),255))
    glow=Image.new("RGBA",(bw,bh),(0,0,0,0))
    gdr=ImageDraw.Draw(glow)
    gdr.ellipse([bw//2-520,-260,bw//2+520,420],fill=(47,179,235,24))
    gdr.ellipse([bw-420,bh-360,bw+240,bh+180],fill=(181,214,77,14))
    glow=glow.filter(ImageFilter.GaussianBlur(85))
    img=Image.alpha_composite(img,glow)
    dots=Image.new("RGBA",(bw,bh),(0,0,0,0))
    ddr=ImageDraw.Draw(dots)
    for y in range(20,bh,32):
        for x in range(20,bw,32):
            ddr.rectangle([x,y,x+1,y+1],fill=(47,179,235,24))
    img=Image.alpha_composite(img,dots)
    dr=ImageDraw.Draw(img)
    for x in range(bw):
        t=x/bw
        dr.line([(x,0),(x,3)],fill=(int(47*(1-t)+181*t),int(179*(1-t)+214*t),int(235*(1-t)+77*t),225))
    img.save(d/"menu-background.png",optimize=True)
    for page in manifest["pages"]:page["widgets"].insert(0,image(0,0,W,H,src="file:shapes/menu-background.png"))

    # Full 1:1 native 1192x236 Mable's Research teal gradient banner
    rw,rh=1192,236
    rb=Image.new("RGBA",(rw,rh),(0,0,0,0))
    rb_fill=Image.new("RGBA",(rw,rh),(0,0,0,0))
    rdr=ImageDraw.Draw(rb_fill)
    for x in range(rw):
        t=x/rw
        rdr.line([(x,0),(x,rh)],fill=(int(12*(1-t)+26*t),int(74*(1-t)+126*t),int(104*(1-t)+134*t),255))
    rmask=Image.new("L",(rw,rh),0)
    ImageDraw.Draw(rmask).rounded_rectangle([0,0,rw-1,rh-1],radius=20,fill=255)
    rb.paste(rb_fill,(0,0),rmask)
    ImageDraw.Draw(rb).rounded_rectangle([1,1,rw-2,rh-2],radius=20,outline=(88,224,214,225),width=2)
    rb.save(d/"research-banner.png",optimize=True)

    # Full 1:1 native 1192x212 Z-A Royale neon magenta card
    yw,yh=1192,212
    rc=Image.new("RGBA",(yw,yh),(0,0,0,0))
    rc_fill=Image.new("RGBA",(yw,yh),(14,16,40,252))
    rgl=Image.new("RGBA",(yw,yh),(0,0,0,0))
    ImageDraw.Draw(rgl).ellipse([yw-340,-30,yw+40,yh+40],fill=(242,13,119,58))
    rgl=rgl.filter(ImageFilter.GaussianBlur(36))
    rc_fill=Image.alpha_composite(rc_fill,rgl)
    ImageDraw.Draw(rc_fill).rounded_rectangle([3,14,11,yh-14],radius=4,fill=(242,13,119,255))
    ymask=Image.new("L",(yw,yh),0)
    ImageDraw.Draw(ymask).rounded_rectangle([0,0,yw-1,yh-1],radius=20,fill=255)
    rc.paste(rc_fill,(0,0),ymask)
    ImageDraw.Draw(rc).rounded_rectangle([1,1,yw-2,yh-2],radius=20,outline=(242,13,119,235),width=2)
    rc.save(d/"royale-card.png",optimize=True)

    # Full 1:1 native 888x888 Lumiose Map cyan blueprint rings
    mr=Image.new("RGBA",(888,888),(0,0,0,0))
    mdr=ImageDraw.Draw(mr)
    mdr.ellipse([8,8,879,879],outline=(47,179,235,44),width=1)
    mdr.ellipse([22,22,865,865],outline=(47,179,235,115),width=2)
    mr.save(d/"map-ring.png",optimize=True)

write_backdrop()


def write_research_medallion():
    d=OUT.parent/"shapes";d.mkdir(parents=True,exist_ok=True)
    med4=Image.new("RGBA",(1056,1056),(0,0,0,0))
    dr=ImageDraw.Draw(med4)
    dr.ellipse([32,32,1024,1024],fill=(14,42,58,244),outline=(88,224,214,255),width=32)
    dr.ellipse([88,88,968,968],outline=(181,214,77,235),width=20)
    med4.resize((264,264),Image.Resampling.LANCZOS).save(d/"research-medallion.png",optimize=True)

def write_dex_stage():
    d=OUT.parent/"shapes";d.mkdir(parents=True,exist_ok=True)
    sw,sh=1240,640
    st=Image.new("RGBA",(sw,sh),(11,16,30,255))
    dr=ImageDraw.Draw(st)
    cx,cy=sw//2,sh//2+16
    for r in range(520,0,-8):
        t=1.0-(r/520.0)
        a=int(52*(t**1.8))
        dr.ellipse([cx-r,cy-int(r*0.72),cx+r,cy+int(r*0.72)],fill=(47,179,235,a))
    for rad,col,w0 in [(260,(47,179,235,80),3),(190,(181,214,77,52),2),(360,(72,110,168,45),2)]:
        dr.ellipse([cx-rad,cy-int(rad*0.72),cx+rad,cy+int(rad*0.72)],outline=col,width=w0)
    dr.line([(48,cy),(sw-48,cy)],fill=(47,179,235,34),width=2)
    dr.line([(cx,36),(cx,sh-36)],fill=(47,179,235,28),width=2)
    ret=(47,179,235,175)
    for rx,ry,dx,dy in [(24,24,36,36),(sw-24,24,-36,36),(24,sh-24,36,-36),(sw-24,sh-24,-36,-36)]:
        dr.line([(rx,ry),(rx+dx,ry)],fill=ret,width=4)
        dr.line([(rx,ry),(rx,ry+dy)],fill=ret,width=4)
    mask=Image.new("L",(sw,sh),0)
    ImageDraw.Draw(mask).rounded_rectangle([0,0,sw-1,sh-1],radius=28,fill=255)
    out_im=Image.new("RGBA",(sw,sh),(0,0,0,0))
    out_im.paste(st,(0,0),mask)
    ImageDraw.Draw(out_im).rounded_rectangle([1,1,sw-2,sh-2],radius=28,outline=(52,74,116,220),width=2)
    out_im.resize((620,320),Image.Resampling.LANCZOS).save(d/"dex-stage.png",optimize=True)

write_research_medallion()
write_dex_stage()
write_send_arrow()
def configure_runtime25() -> None:
    extra_height = 160
    manifest["canvas_alt"] = {"w": W, "h": H + extra_height, "when": "portrait"}

    portraits = {"za.sel.icon", "za.detail.icon", "za.boxdetail.icon", "za.moves.icon",
                 "za.nativebox.source_icon"}
    for page in manifest["pages"]:
        is_map = page["id"] == "map"
        for widget in page["widgets"]:
            if widget.get("bind") in portraits and widget["type"] == "image":
                widget["src_hold"] = False
            x, y, width, height = widget["rect"]
            if widget.get("src") == "file:shapes/menu-background.png":
                height += extra_height
            elif widget.get("need_bind") in ("!za.ready","za.nav.menu","za.box.picker","module_error") and widget.get("input_block"):
                height += extra_height
            elif y >= 958:
                y += extra_height
            elif is_map:
                # Preserve the map rectangle shared with the native hit tests.
                if x == 762 and y == 24 and height == 920:
                    height += extra_height
                elif x >= 762 and y >= 720 and not widget.get("scroll"):
                    if widget.get("need_bind") == "za.map.filters_open" and y < 860:
                        continue
                    y += extra_height
            elif widget.get("src") != "module:ready":
                y += extra_height // 2
            if [x, y, width, height] != widget["rect"]:
                widget["rect_alt"] = [x, y, width, height]

        for scroll in page.get("scrolls", []):
            x, y, width, height = scroll["rect"]
            if not is_map:
                scroll["rect_alt"] = [x, y + extra_height // 2, width, height]
            elif scroll["id"] in {"za_zone", "za_dimension", "za_map_quest","za_map_choices"}:
                scroll["rect_alt"] = [x, y, width, height + extra_height]


for page in manifest["pages"]:
    page["widgets"] += tabbar()+refusal_overlay()
    for scroll in page.get("scrolls", []):
        gate = scroll.get("show_bind")
        # Eden paints scrollbars after widgets, so the modal must gate their regions.
        scroll["show_bind"] = f"{gate}.scroll" if gate else "!za.nav.menu"

round_pages(manifest["pages"])
localize(manifest["pages"],ROOT)
if RUNTIME_25:
    configure_runtime25()

manifest["enforce_gate"] = dict(point="za.enforce.active")

if __name__ == "__main__":
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print("wrote", OUT)
