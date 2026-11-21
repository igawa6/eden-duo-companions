#!/usr/bin/env python3
"""Writes packages/PokemonLegendsZA/dualscreen/manifest.json.

Six French tabs on a 1240x1080 logical canvas. Values come from the player's
live game state or save via the native module; names, icons and the map come from their romfs.
Only the redistributable DejaVu font atlas/metrics ship with the package.

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

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "packages/PokemonLegendsZA/dualscreen/manifest.json"
if "--output" in sys.argv:
    OUT = Path(sys.argv[sys.argv.index("--output") + 1])

W, H = 1240, 1080
MAP_MAX_ZOOM = 12  # Match kMapMaxZoom in the native module.
RUNTIME_25 = "--runtime25" in sys.argv
LEGACY_NAV = RUNTIME_25 or "--legacy-nav" in sys.argv

# palette (AARRGGBB)
BG = "#FF142A3C"
HEADER = "#FFEDF5F2"
LINE = "#FF527087"
PANEL = "#FF213B50"
PANEL_HI = "#FF2D4B62"
WELL = "#FF0B111C"
RED = "#FF55C6BE"
GOLD = "#FFF2C33D"
TEXT = "#FFE8EEF7"
DIM = "#FFBBCBDC"
GREEN = "#FF3FD07A"
BLUE = "#FF57A9F5"
CLEAR = "#00000000"

# the game's box and party geometry (za_save.h)
BOX_SLOTS = 30
PARTY_SLOTS = 6

# PA9 nature ids, French names written ASCII (the built-in font draws e-acute as "e", but
# spelling them plainly keeps the manifest readable and the widths predictable).
NATURES = [
    "Hardi", "Solitaire", "Brave", "Rigide", "Mauvais",
    "Assuré", "Docile", "Relax", "Malin", "Lâche",
    "Timide", "Pressé", "Sérieux", "Jovial", "Naïf",
    "Modeste", "Doux", "Calme", "Pudique", "Foufou",
    "Serein", "Gentil", "Malpoli", "Prudent", "Bizarre",
]
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

TYPE_COLORS = ["#FF9099A0", "#FFC34E48", "#FF92A9DE", "#FFAA65B7", "#FFC5A971", "#FFB9A65C",
               "#FF93AE3D", "#FF6C71B6", "#FF83A2B4", "#FFE58A41", "#FF4C98DB", "#FF65B55A",
               "#FFE4C547", "#FFDC75A3", "#FF75C9CF", "#FF697FC7", "#FF80727A", "#FFDA91C6"]
TYPE_TEXT = ["#FF080D16" if i not in (1,7,16) else "#FFFFFFFF" for i in range(len(TYPE_COLORS))]


def type_badge(x, y, w, bind, scale=3, **gate):
    """Unknown types are -1. A single-type Pokémon has type2=-1, so no false Normal badge."""
    out=[]
    for i, color in enumerate(TYPE_COLORS):
        out += [rect(x,y,w,40,bg=color,frame=0,hide_bind=bind,keep_min=i,keep_max=i,**gate),
                image(x+5,y+5,30,30,src=f"module:type/{i}",hide_bind=bind,keep_min=i,keep_max=i,**gate),
                bound_text(x+42+(w-42)//2,y+10,f"za.type{i}.name",scale,TYPE_TEXT[i],align="center",
                      wrap_width=w-52,max_lines=1,fit_text=True,
                      hide_bind=bind,keep_min=i,keep_max=i,**gate)]
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
          image(GRID_X+38,GRID_Y+2,114,114,src_format="module:icon/%d",bind="za.b{i}.icon",**grid,**present),
          bound_text(GRID_X+10,GRID_Y+124,"za.b{i}.name",5,TEXT,wrap_width=170,max_lines=1,fit_text=True,**grid,**present),
          rect(GRID_X+6,GRID_Y+6,86,34,bg="#EC102536",frame=0,**grid,**present),
          value(GRID_X+49,GRID_Y+10,"za.b{i}.lvl",5,TEXT,text="N.",align="center",
                wrap_width=78,max_lines=1,fit_text=True,**grid,**present),
          rect(GRID_X+164,GRID_Y+10,12,12,bg=GOLD,frame=0,need_bind="za.b{i}.shiny",**grid),
          label(GRID_X+CELL_W//2,GRID_Y+60,"VIDE",3,DIM,align="center",wrap_width=CELL_W-12,
                max_lines=1,fit_text=True,hide_bind="za.b{i}.v",hide_eq=1,**grid)]
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
    for i,name in enumerate(["ILLUMIS","MÉGA-DIMENSION","MÉGA-ÉVOLUTIONS"]):
        x=24+i*400
        w += [button(x,126,388,58,name,f"dex_gen_{i+1}",scale=5),
              rect(x,126,388,58,color=GOLD,frame=3,hide_bind="za.dex.gen",keep_min=i+1,keep_max=i+1)]
    for i,name in enumerate(["TOUS","CAPTURÉS","MANQUANTS","VUS","EN STOCK"]):
        x=24+i*198
        w += [button(x,204,186,50,name,f"dex_filter_{i}",scale=5),
              rect(x,204,186,50,color=GOLD,frame=3,hide_bind="za.dex.filter",keep_min=i,keep_max=i)]
    w += [button(1014,204,198,50,"Rechercher","search_from_dex",scale=5)]
    grid=dict(repeat=512,repeat_bind="za.dex.rows",repeat_dy=110,scroll="za_dex")
    gate=dict(need_bind="za.d{i}.v")
    w += [rect(24,276,516,102,bg=PANEL,color=LINE,**grid,**gate),
          image(28,279,96,96,src_format="module:icon/%d",bind="za.d{i}.icon",**grid,**gate),
          bound_text(132,286,"za.d{i}.name",6,TEXT,wrap_width=390,max_lines=1,fit_text=True,**grid,**gate),
          value(132,328,"za.d{i}.number",4,GOLD,text="No. ",pad=3,**grid,**gate),
          value(274,328,"za.d{i}.state",4,DIM,names=DEX_STATES,**grid,**gate),
          rect(24,276,516,102,color=GOLD,frame=3,need_bind="za.d{i}.selected",**grid),
          button(24,276,516,102,"","dex_pick",bg=CLEAR,color=CLEAR,border=0,payload="{i}",**grid,**gate),
          panel(560,276,652,780),rect(560,276,652,104,bg=HEADER,frame=0),
          bound_text(580,292,"za.detail.name",8,"#FF24364B",wrap_width=612,max_lines=1,fit_text=True),
          value(580,344,"za.detail.number",5,"#FF426C80",text="No. ",pad=3),
          label(766,344,"MÉGA : statuts de l'espèce de base",5,"#FF426C80",
                wrap_width=420,max_lines=1,fit_text=True,hide_bind="za.dex.gen",keep_min=3,keep_max=3),
          image(724,384,316,316,src_format="module:icon/%d",bind="za.detail.icon"),
          value(580,662,"za.detail.state",5,GREEN,names=DEX_STATES),
          value(1186,662,"za.detail.owned",5,DIM,text="EN STOCK ",align="right",need_bind="za.detail.owned"),
          bound_text(580,762,"za.detail.description",5,TEXT,wrap_width=600,max_lines=128,line_gap=9,scroll="za_dex_text"),
          value(24,938,"za.dex.rows",3,DIM,text="POKÉMON ")]
    w += type_badge(580,706,290,"za.detail.type1",4)+type_badge(890,706,290,"za.detail.type2",4)
    w += not_ready()
    return dict(id="dex",title="Pokédex",nav_order=nav_order(),widgets=w,scrolls=[
        dict(scroll_region("za_dex",[24,276,516,650],"za.dex.rows",110,"za.dex.reset"),
             follow_bind="za.dex.follow",follow_open=True),
        scroll_region("za_dex_text",[580,762,612,112],"za.detail.lines",34,"za.detail.sp")])

def bag_page():
    w=header("bag")
    captions=["TOUT","SOINS","BALLS","OBJETS","TRÉSORS","CLÉS","BAIES","CT","MÉGA","AUTRES"]
    for i,name in enumerate(captions):
        x=24+i%5*240;y=22+i//5*64
        w += [button(x,y,228,54,name,f"bag_category_{i}",scale=5),
              rect(x,y,228,54,color=GOLD,frame=3,hide_bind="za.bag.category",keep_min=i,keep_max=i)]
    w += [label(24,153,"Favoris",5,GOLD)]
    for i in range(4):
        x=24+i*300
        key=f"za.bf{i}."
        has=dict(need_bind=key+"id")
        w += [rect(x,186,288,86,bg=PANEL,color=LINE),
              image(x+8,197,64,64,src_format="module:item/%d",bind=key+"id",**has),
              bound_text(x+79,199,key+"name",5,TEXT,wrap_width=197,max_lines=1,fit_text=True,**has),
              value(x+79,235,key+"count",5,GOLD,text="x",**has),
              label(x+144,216,"Vide",5,DIM,align="center",need_bind="!"+key+"id"),
              button(x,186,288,86,"","bag_favorite_pick",payload=i,bg=CLEAR,color=CLEAR,border=0,**has)]
    grid=dict(repeat=1024,repeat_bind="za.bag.rows",repeat_dy=124,scroll="za_bag")
    gate=dict(need_bind="za.i{i}.v")
    w += [rect(24,294,598,116,bg=PANEL,color=LINE,**grid,**gate),
          image(32,304,96,96,src_format="module:item/%d",bind="za.i{i}.id",**grid,**gate),
          bound_text(144,306,"za.i{i}.name",6,TEXT,wrap_width=360,max_lines=2,fit_text=True,**grid,**gate),
          value(602,372,"za.i{i}.count",5,GOLD,text="x",align="right",**grid,**gate),
          rect(24,294,598,116,color=GOLD,frame=3,need_bind="za.i{i}.selected",**grid),
          button(24,294,598,116,"","bag_pick",payload="{i}",bg=CLEAR,color=CLEAR,border=0,**grid,**gate),
          panel(642,294,570,650),
          image(850,349,150,150,src_format="module:item/%d",bind="za.item.id"),
          bound_text(666,515,"za.item.name",7,TEXT,wrap_width=520,max_lines=2,line_gap=6,fit_text=True),
          value(1188,318,"za.item.count",6,GOLD,text="x",align="right"),
          button(664,306,345,46,"Ajouter aux favoris","bag_favor",scale=5,need_bind="za.bag.fav_add"),
          button(664,306,345,46,"Retirer des favoris","bag_favor",scale=5,need_bind="za.bag.fav_remove"),
          label(666,320,"Favoris pleins",5,DIM,need_bind="za.bag.fav_full"),
          bound_text(666,658,"za.item.description",5,TEXT,wrap_width=516,max_lines=128,line_gap=7,scroll="za_bag_text"),
          button(930,806,252,58,"Objet tenu","bag_tool_hold",scale=5,need_bind="za.item.hold"),
          button(666,876,516,56,"Retour aux évolutions","guide_item_back",scale=5,need_bind="za.bag.guide"),
          button(666,876,516,56,"Quitter le choix d’objet","bag_hold_end",scale=5,need_bind="za.bag.held_context")]
    for mode,caption,action in [(1,"Soins rapides","bag_tool_heal"),(3,"Compatibilité CT","bag_tool_tm"),
                                (4,"Évolutions","bag_tool_evolve")]:
        w.append(button(666,806,252,58,caption,action,scale=5,
                        hide_bind="za.item.primary_tool",keep_min=mode,keep_max=mode))
    w+=type_badge(666,601,516,"za.item.type",5)+not_ready()
    return dict(id="bag",title="Sac",nav_order=nav_order(),widgets=w,scrolls=[
        scroll_region("za_bag",[24,294,598,642],"za.bag.rows",124,"za.bag.reset"),
        scroll_region("za_bag_text",[666,658,520,132],"za.item.lines",32,"za.item.id")])


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
    for i, caption in enumerate(QUEST_CATEGORIES):
        x = 24 + i * 301
        w += [button(x,92,289,68,caption,f"quest_category_{i}",wid=f"quest_category_{i}",scale=5),
              rect(x,92,289,68,bg=RED,hide_bind="za.quest.category",keep_min=i,keep_max=i),
              label(x+144,113,caption,5,"#FF173F49",align="center",wrap_width=276,max_lines=1,fit_text=True,
                    hide_bind="za.quest.category",keep_min=i,keep_max=i)]

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
        label(24,800,"COMBATS NOCTURNES",5,DIM,wrap_width=156,max_lines=3,line_gap=6,**wide),
        value(24,898,"za.map.active_battles",6,RED,**wide),
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

def tabbar():
    icons=("module:menu/0","module:menu/1","module:menu/2","module:center",
           "module:item/4","module:menu/4","module:menu/3","module:map-pin")
    menu=dict(need_bind="za.nav.menu")
    edit=dict(need_bind="za.nav.editing")
    ink="#FF123241"
    out=[
        rect(0,0,W,958,bg="#B0081825",frame=0,input_block=True,**menu),
        rect(24,162,1192,778,bg=PANEL,color=LINE,**menu),
        label(56,208,"Onglets",8,TEXT,**menu),
        button(708,184,234,72,"","nav_edit",wid="nav_edit",**menu),
        label(825,208,"Modifier",5,TEXT,align="center",hide_bind="za.nav.editing",hide_eq=1,**menu),
        label(825,208,"Terminé",5,TEXT,align="center",**edit),
        button(958,184,226,72,"Fermer","nav_more",scale=5,**menu),
        label(56,276,"Choisis une place, puis un onglet.",5,DIM,**edit),
    ]
    for i in range(3):
        x=56+i*380
        out += [
            button(x,314,368,70,"",f"nav_slot_{i}",wid=f"nav_slot_{i}",**edit),
            rect(x,314,368,70,color=RED,frame=3,hide_bind="za.nav.edit_slot",
                 keep_min=i+1,keep_max=i+1,**edit),
            bound_text(x+184,336,f"za.nav.slot{i}.name",6,TEXT,align="center",
                       wrap_width=344,max_lines=1,fit_text=True,**edit),
        ]
    for i,((_,name),icon) in enumerate(zip(TABS,icons),1):
        x=56+((i-1)%2)*574
        y=416+((i-1)//2)*120
        action=f"nav_pick_{i}"
        out += [
            button(x,y,554,100,"",action,wid=action,**menu),
            rect(x,y,554,100,color=RED,frame=3,hide_bind="za.nav.active",keep_min=i,keep_max=i,**menu),
            image(x+20,y+24,52,52,src=icon,**menu),
            label(x+90,y+35,name.title(),6,TEXT,wrap_width=430,max_lines=1,fit_text=True,**menu),
        ]
    out.append(rect(0,958,W,122,bg=HEADER))
    for i,bind in enumerate(("za.nav.slot0","za.nav.slot1","za.nav.slot2","za.nav.more")):
        x=16+i*306
        action=f"nav_shortcut_{i}" if i<3 else "nav_more"
        active=bind+".active"
        out += [
            button(x,972,290,92,"",action,wid=action),
            rect(x,972,290,92,bg=RED,frame=0,need_bind=active),
            bound_text(x+163,1004,bind+".name",5,TEXT,align="center",wrap_width=226,
                       max_lines=1,fit_text=True,hide_bind=active,hide_eq=1),
            bound_text(x+163,1004,bind+".name",5,ink,align="center",wrap_width=226,
                       max_lines=1,fit_text=True,need_bind=active),
        ]
        for tab,icon in enumerate(icons,1):
            out.append(image(x+12,999,38,38,src=icon,hide_bind=bind+".id",keep_min=tab,keep_max=tab))
        if i==3:
            out += [label(x+31,1001,"+",7,TEXT,align="center",hide_bind=bind+".id",hide_eq=-1,
                          keep_min=0,keep_max=0),
                    label(x+264,1000,"…",6,ink,align="center",need_bind=active)]
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
    base = bytes.fromhex(PANEL_HI[3:])
    accent = bytes.fromhex(color[3:])
    rgb = bytes(round(background * 0.74 + foreground * 0.26)
                for background, foreground in zip(base, accent))
    return "#FF" + rgb.hex().upper()


ATTACK_FILLS = [_attack_fill(color) for color in TYPE_COLORS]
ATTACK_LABELS = [
    "#FF" + bytes(round(component * 0.32 + 255 * 0.68)
                  for component in bytes.fromhex(color[3:])).hex().upper()
    for color in TYPE_COLORS
]


def attacks(prefix,positions,width=556,interactive=False):
    out=[]
    for i,(x,y) in enumerate(positions):
        p=f"{prefix}.m{i}"
        height=116 if interactive else 80
        if interactive:
            out += [
                rect(x, y, width, height, bg="#FFFFFFFF", frame=0,
                     tint_bind=p+".type", tint_colors=ATTACK_FILLS,
                     need_bind=p+".id", hide_bind=p+".type", keep_min=0, keep_max=17),
                button(x, y, width, height, "", f"attack_{i}", wid=f"attack_{i}",
                       bg=CLEAR, need_bind=p+".id"),
            ]
        else:
            out.append(rect(x,y,width,height,bg=PANEL_HI,color=LINE))
        out += [
                image(x+12,y+14,48,48,src_format="module:type/%d",bind=p+".type"),
                bound_text(x+72,y+8,p+".name",7,TEXT,wrap_width=width-(282 if interactive else 92),
                           max_lines=1,fit_text=True),
                ]
        if not interactive:
            out.append(value(x+width-18,y+50,p+".pp",4,DIM,text="PP ",align="right"))
        if interactive:
            gate=dict(need_bind=p+".id")
            out += [bound_text(x+223,y+49,p+".category",4,DIM,wrap_width=120,max_lines=1,fit_text=True,**gate),
                    bar(x+14,y+92,width-158,15,p+".recharge",RED,maxv=1000,**gate),
                    bound_text(x+width-18,y+88,p+".recharge_text",4,TEXT,align="right",
                               wrap_width=120,max_lines=1,fit_text=True,**gate)]
            for offset,caption,key in ((172,"Pui.","power"),(66,"Préc.","accuracy")):
                pos=x+width-offset
                out += [label(pos,y+16,caption,4,DIM,align="center",wrap_width=100,max_lines=1,fit_text=True,**gate),
                        bound_text(pos,y+44,p+"."+key,5,TEXT,align="center",**gate)]
        for t,color in enumerate(ATTACK_LABELS if interactive else TYPE_COLORS):
            out.append(bound_text(x+72,y+49,f"za.type{t}.name",5,color,
                                  wrap_width=144 if interactive else width-172,max_lines=1,fit_text=True,
                                  hide_bind=p+".type",keep_min=t,keep_max=t))
    return out

def team_page():
    w=header("team")
    for i in range(6):
        x=24+i*200;p=f"za.p{i}";gate=dict(need_bind=p+".v")
        w += [rect(x,24,188,294,bg=PANEL_HI,color=LINE),
              image(x+4,30,170,170,src_format="module:icon/%d",bind=p+".icon",**gate),
              bound_text(x+12,204,p+".name",5,TEXT,wrap_width=164,max_lines=1,fit_text=True,**gate),
              bar(x+14,234,160,14,p+".hp",GREEN,max_bind=p+".hpmax",**gate),
              bar(x+14,255,160,8,p+".xp",BLUE,max_bind=p+".xpmax",**gate),
              value(x+14,273,p+".lvl",5,text="N. ",**gate),
              value(x+172,273,p+".g",4,DIM,names=GENDERS,align="right",**gate),
              rect(x+8,36,172,33,bg="#FF123F42",need_bind=p+".deployed"),
              label(x+94,41,"Sur le terrain",5,"#FFC6FFF0",align="center",
                    wrap_width=160,max_lines=1,fit_text=True,need_bind=p+".deployed"),
              rect(x,24,188,294,color=GOLD,frame=4,need_bind=p+".selected"),
              button(x,24,188,294,"","team_select",wid=f"party_{i}",payload=i,bg=CLEAR,color=CLEAR,border=0,**gate)]
        for move in range(4):
            w.append(bar(x+14+move*41,266,37,6,f"{p}.r{move}",RED,maxv=1000,
                         need_bind=f"{p}.c{move}"))
    w += [panel(24,338,1188,606),rect(24,338,1188,76,bg=HEADER,frame=0),
          bound_text(48,358,"za.sel.name",7,"#FF24364B",wrap_width=372,max_lines=1,fit_text=True),
          image(440,348,56,56,src_format="module:item/%d",bind="za.sel.item",need_bind="za.sel.item"),
          bound_text(506,354,"za.sel.item_name",5,"#FF42596A",wrap_width=168,max_lines=2,line_gap=4,fit_text=True)]
    w += type_badge(694,356,236,"za.sel.type1",4)+type_badge(948,356,236,"za.sel.type2",4)
    w += [label(48,438,"PV",4,GREEN),bar(112,438,470,20,"za.sel.hp",GREEN,max_bind="za.sel.hpmax"),
          value(582,473,"za.sel.hp",5,TEXT,max_bind="za.sel.hpmax",max_sep=" / ",align="right"),
          label(660,438,"EXP",4,BLUE),bar(746,438,438,20,"za.sel.xp",BLUE,max_bind="za.sel.xpmax"),
          value(1184,473,"za.sel.xp",5,TEXT,max_bind="za.sel.xpmax",max_sep=" / ",align="right")]
    w += attacks("za.sel",[(336,510),(636,634),(36,634),(336,758)],width=568,interactive=True)
    w += [button(1084,832,104,104,"","send_out",wid="send_out",bg=RED),
          image(1104,852,64,64,src="file:shapes/send-arrow.png"),
          bound_text(760,885,"za.send.status",4,DIM,wrap_width=306,max_lines=2,line_gap=3,fit_text=True)]
    w += [button(48,884,264,52,"CAPACITÉS","moves_team",scale=5),
          button(328,884,244,52,"Objet tenu","bag_hold_team",scale=5,need_bind="za.sel.v"),
          button(582,884,164,52,"Soins","team_heal_open",scale=5,need_bind="za.sel.v")]
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
       bound_text(48,45,"za.boxdetail.name",7,"#FF24364B",wrap_width=820,max_lines=1,fit_text=True),
       value(1100,47,"za.boxdetail.lvl",6,"#FF24364B",text="N. ",align="right"),
       value(1176,50,"za.boxdetail.g",5,"#FF24364B",names=GENDERS,align="right"),
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
    w += attacks("za.boxdetail",[(342,604),(48,692),(644,692),(342,780)])
    w += [button(48,876,238,64,"RETOUR","go_box",scale=5),
          button(310,876,340,64,"CAPACITÉS","moves_box",scale=5)]
    return dict(id="box_detail",title="Fiche Pokémon",nav_order=nav_order(),widgets=w)

def progress_page():
    ink="#FF163E3C"
    w=header("progress")+[label(32,24,"MON PARCOURS À ILLUMIS",8),
        label(34,78,"Recherche · aventures · Royale Z-A",5,DIM),
        rect(24,126,1192,218,bg="#FFDBE9E3",frame=0),
        image(888,126,308,218,src="module:progress/3"),
        image(42,146,180,180,src="file:shapes/research-medallion.png"),
        label(132,180,"NIVEAU",5,ink,align="center"),
        value(132,222,"za.progress.mable_level",13,ink,align="center"),
        label(250,150,"RECHERCHES DE MYOSOTIS",6,ink,wrap_width=610,max_lines=1,fit_text=True),
        bound_text(250,202,"za.progress.research_fraction",9,ink),
        bar(250,264,610,20,"za.progress.research_xp",color="#FF2A7567",max_bind="za.progress.research_xpmax",bg="#FFB0D0C3"),
        label(250,302,"POINTS MANQUANTS",5,ink,need_bind="!za.progress.research_max"),
        value(850,298,"za.progress.research_missing",6,ink,align="right",need_bind="!za.progress.research_max"),
        label(250,298,"NIVEAU MAXIMUM",6,ink,need_bind="za.progress.research_max"),
        button(888,278,308,54,"TOUS LES NIVEAUX","research_open_1",scale=5)]
    for i,title in enumerate(["NIVEAU PRÉCÉDENT","NIVEAU ACTUEL","NIVEAU SUIVANT"]):
        x=24+i*404;key=f"za.progress.r{i}.";gate=dict(need_bind=key+"valid")
        w += [panel(x,364,384,224,bg=PANEL_HI if i==1 else PANEL,edge=RED if i==1 else LINE),
              label(x+20,384,title,5,RED if i==1 else DIM,wrap_width=344,max_lines=1,fit_text=True),
              value(x+20,428,key+"level",9,TEXT,**gate),
              value(x+360,442,key+"points",5,GOLD,align="right",**gate),
              image(x+16,494,82,82,src_format="module:item/%d",bind=key+"icon",**gate),
              bound_text(x+110,500,key+"reward",5,TEXT,wrap_width=254,max_lines=2,fit_text=True,line_gap=5,**gate),
              label(x+20,450,"—",9,DIM,need_bind="!"+key+"valid"),
              button(x,364,384,224,"",f"research_open_{i}",bg=CLEAR,color=CLEAR,border=0,need_bind="!za.research.open")]
    w += [panel(24,610,1192,192),label(48,630,"ROYALE Z-A INFINI",7,GOLD,need_bind="za.progress.is_infinite"),
          label(48,630,"ROYALE Z-A",7,GOLD,need_bind="!za.progress.is_infinite"),
          label(1040,636,"RANG",5,DIM,align="right"),bound_text(1188,630,"za.progress.rank",9,GOLD,align="right"),
          bound_text(48,678,"za.progress.rank_fraction",9,TEXT),
          bar(48,746,730,18,"za.progress.rank_points",color=GOLD,max_bind="za.progress.rank_target"),
          label(810,678,"PROCHAIN TICKET",5,DIM,wrap_width=360,max_lines=1,fit_text=True),
          value(810,722,"za.progress.rank_missing",9,GOLD,need_bind="za.progress.rank_known"),
          label(48,772,"POINTS TICKET",5,DIM),
          label(1206,82,"Données en direct",5,RED,align="right",need_bind="za.progress.research_live"),
          label(1206,82,"Données de votre dernière sauvegarde",5,DIM,align="right",need_bind="!za.progress.research_live")]
    for i,(title,key,color) in enumerate([("ARGENT DISPONIBLE","money",GOLD),("RECHERCHE INTERDIMENSIONNELLE","hyperspace",BLUE)]):
        x=24+i*606;bind="za.progress."+key
        w += [panel(x,822,586,106),label(x+22,836,title,5,DIM,wrap_width=542,max_lines=1,fit_text=True),
              value(x+22,872,bind,9,color)]
    gate=dict(need_bind="za.research.open")
    w += [rect(24,364,1192,564,bg=PANEL,frame=2,**gate),
          label(48,384,"NIVEAUX DE RECHERCHE",6,GOLD,**gate),
          button(1014,378,178,58,"FERMER","research_close",scale=5,**gate),
          panel(430,450,762,456,bg=PANEL_HI)]
    w[-1].update(gate)
    rows=dict(repeat=50,repeat_bind="za.research.rows",repeat_dy=120,scroll="za_research",**gate)
    w += [rect(48,450,356,112,bg=PANEL_HI,**rows),
          rect(48,450,356,112,color=GOLD,frame=3,hide_bind="za.rl{i}.selected",keep_min=1,keep_max=1,**rows),
          bound_text(64,462,"za.rl{i}.label",6,TEXT,wrap_width=322,max_lines=1,fit_text=True,**rows),
          image(64,500,52,52,src_format="module:item/%d",bind="za.rl{i}.icon",**rows),
          bound_text(130,500,"za.rl{i}.reward",5,DIM,wrap_width=258,max_lines=2,line_gap=4,fit_text=True,**rows),
          button(48,450,356,112,"","research_pick",payload="{i}",bg=CLEAR,color=CLEAR,border=0,**rows),
          value(448,466,"za.research.selected",6,GOLD,text="NIVEAU ",**gate),
          image(448,506,96,96,src_format="module:item/%d",bind="za.research.icon",**gate),
          bound_text(560,516,"za.research.reward",6,TEXT,wrap_width=604,max_lines=2,fit_text=True,line_gap=5,**gate),
          label(448,620,"POINTS CUMULÉS",5,DIM,**gate),
          value(1168,616,"za.research.points",7,GOLD,align="right",**gate),
          bound_text(448,680,"za.research.description",5,TEXT,wrap_width=720,max_lines=20,line_gap=8,scroll="za_reward_text",**gate)]
    badge=type_badge(936,464,232,"za.research.type",5)
    for item in badge:item.update(gate)
    w+=badge
    w += not_ready()
    return dict(id="progress",title="Progression",nav_order=nav_order(),widgets=w,
        scrolls=[dict(scroll_region("za_research",[48,450,356,456],"za.research.rows",120,"za.research.open"),
                      show_bind="za.research.open",follow_bind="za.research.follow",follow_open=True),
                 dict(scroll_region("za_reward_text",[448,680,720,210],"za.research.lines",38,"za.research.selected"),show_bind="za.research.open")])

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
              "Only a redistributable font is included.",
    "min_runtime": 25 if RUNTIME_25 else (19 if LEGACY_NAV else 22),
    "font": "file:font/za.mfnt",
    "font_atlas": "file:font/za.png",
    "requires_module": True,
    "canvas_w": W,
    "canvas_h": H,
    "background": BG,
    "nav": {"color": RED, "frame": 4},
    "tables": {"natures": NATURES},
    "actions": actions,
    "page_binds": [dict(point="za.quest.show_map", equals=1, ready_bind="za.ready",
                       when_equal=dict(page="map", transition="fade", duration_ms=120))] +
                  [dict(point="za.guide.page",equals=i,ready_bind="za.ready",
                        when_equal=dict(page=page,transition="fade",duration_ms=120))
                   for i,page in enumerate(("locations","combat","missions","evolutions","capture","search","dex","bag","bag_tools","team","team_heal"),1)],
    "enforce":[{"action":f"input_{command}","every_ms":1} for command in ["left","right","send","aim","attack_0","attack_1","attack_2","attack_3"]],
    "pages": [team_page(), dex_page(), box_page(), box_detail_page(), map_page(), bag_page(), missions_page(),
              progress_page(),notebook_page(),locations_page(),combat_page(),evolutions_page(),capture_page(),search_page(),bag_tools_page(),team_heal_page()],
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
manifest["persist_flags"].append("za_nav_shortcuts")
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
    if page["id"] in {"team","box_detail","progress","map","missions","notebook","locations","combat","evolutions","capture","search","bag","bag_tools","team_heal"}:continue
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
        page["widgets"].append(button(674,876,514,64,"ÉCHANGER AVEC L'ÉQUIPE","native_box_prepare",scale=5,need_bind="za.nativebox.available"))
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
          label(40,342,"CAPACITÉS DISPONIBLES",5,GOLD),
          bound_text(616,342,"za.moves.name",8,TEXT,wrap_width=568,max_lines=2,fit_text=True,**detail)]
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
    w += [button(616,856,568,76,"REMPLACER LA CAPACITÉ","moves_apply",scale=5,need_bind="za.moves.can_apply")]
    scrolls=[scroll_region("za_moves",[40,398,516,526],"za.moves.rows",126,"za.moves.reset"),
             dict(id="za_move_text",rect=[602,342,598,432],reset_bind="za.moves.detail_reset",
                  fling=True,pad=10,bar=GOLD,bar_track=LINE,bar_w=7)]
    return dict(id="moves",title="Capacités",widgets=w,scrolls=scrolls)

manifest["pages"].append(move_menu_page())
manifest["page_binds"].append(dict(point="za.moves.toggle",equals=0,ready_bind="za.ready",
    when_equal=dict(page="moves",transition="fade",duration_ms=120),
    when_not_equal=dict(page="moves",transition="fade",duration_ms=120)))


def round_texture(bg, edge, border, radius=12):
    name=hashlib.sha256(f"{bg}/{edge}/{border}/{radius}".encode()).hexdigest()[:16]+".png"
    directory=OUT.parent/"shapes";directory.mkdir(parents=True,exist_ok=True)
    target=directory/name
    def rgba(color):
        n=int(color.lstrip("#"),16);return [(n>>16)&255,(n>>8)&255,n&255,(n>>24)&255]
    fill,frame=rgba(bg),rgba(edge);raw=bytearray()
    for y in range(48):
        raw.append(0)
        for x in range(48):
            qx=max(radius-(x+.5),0,(x+.5)-(48-radius));qy=max(radius-(y+.5),0,(y+.5)-(48-radius))
            distance=math.hypot(qx,qy)-radius
            coverage=min(1,max(0,.5-distance))
            color=frame if border and distance>-border else fill
            raw.extend(color[:3]+[round(color[3]*coverage)])
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    target.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',48,48,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(raw),9))+chunk(b'IEND',b''))
    return "file:shapes/"+name


def round_pages(pages):
    for page in pages:
        out=[]
        for w in page["widgets"]:
            kind=w.get("type");width,height=w["rect"][2:]
            # Keep small accents and the player marker sharp; round the surfaces around them.
            if kind in ("rect","button") and width>=48 and height>=24:
                bg=w.get("bg",CLEAR);edge=w.get("color",CLEAR);border=w.get("border",2) if kind=="button" else w.get("frame",2)
                if bg!=CLEAR or (edge!=CLEAR and border):
                    surface={k:v for k,v in w.items() if k not in {"type","id","text","text_scale","align","on_tap","on_hold","nav","border","color","bg","frame","bind_text","table","max_lines","wrap_width","fit_text","input_block"}}
                    surface.update(type="image",src=round_texture(bg,edge,border),fill="slice",slice=[12,12,12,12],color="#FFFFFFFF")
                    if kind=="rect":
                        if w.get("input_block"):surface["input_block"]=True
                        out.append(surface);continue
                    out.append(surface);w=dict(w,bg=CLEAR,border=0)
            elif kind=="bar":
                radius=max(1,min(6,height//2));bg=w.get("bg",WELL)
                background={k:v for k,v in w.items() if k in {"rect","repeat","repeat_cols","repeat_dx","repeat_dy","repeat_row_dy","need_bind","hide_bind","keep_min","keep_max","hide_eq"}}
                background.update(type="image",src=round_texture(bg,CLEAR,0,radius),fill="slice",slice=[radius]*4,color="#FFFFFFFF")
                out.append(background)
                w=dict(w,bg=CLEAR,image=round_texture(w["color"],CLEAR,0,radius),fill="slice",slice=[radius]*4,frame=0)
            out.append(w)
        page["widgets"]=out


def write_send_arrow():
    raw=bytearray()
    for y in range(64):
        raw.append(0)
        for x in range(64):
            on=(12<=y<=34 and abs(x-31.5)<=y-11) or (27<=x<=36 and 30<=y<=54)
            raw.extend([18,50,65,255 if on else 0])
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    # Small companion-only portal glyphs; all actual game artwork is still
    # decoded from the player's romfs. Distinct silhouettes remain readable zoomed out.
    for kind,color in enumerate(((175,128,255),(255,150,78))):
        portal=bytearray()
        for y in range(64):
            portal.append(0)
            for x in range(64):
                dx,dy=x-31.5,y-31.5;d=math.hypot(dx,dy)
                ring=22<d<28;inner=abs(dx)/15+abs(dy)/21<1
                glyph=(abs(dx)<2 or abs(dy)<2) and d<9 if kind==0 else (abs(dx-dy)<3 or abs(dx+dy)<3) and d<11
                pixel=(249,250,255,255) if glyph else (*color,255) if ring else (31,29,63,245) if inner else (0,0,0,0)
                portal.extend(pixel)
        directory=OUT.parent/"shapes";directory.mkdir(parents=True,exist_ok=True)
        (directory/f"dimension-{kind}.png").write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',64,64,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(portal),9))+chunk(b'IEND',b''))
    arrow=bytearray()
    for y in range(64):
        arrow.append(0)
        for x in range(64):
            width=0 if y<5 or y>=57 else min(23,(y-5)*.52)
            inside=width>0 and abs(x-31.5)<width and y<57-abs(x-31.5)*.50
            outline=width>0 and (width-abs(x-31.5)<3 or y>52-abs(x-31.5)*.50)
            arrow.extend((18,36,58,255) if inside and outline else (87,222,254,255) if inside else (0,0,0,0))
    (OUT.parent/"shapes/map-arrow.png").write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',64,64,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(arrow),9))+chunk(b'IEND',b''))
    target=OUT.parent/"shapes/send-arrow.png";target.parent.mkdir(parents=True,exist_ok=True)
    target.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',64,64,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(raw),9))+chunk(b'IEND',b''))

def write_backdrop():
    raw=bytearray()
    for y in range(192):
        raw.append(0)
        for x in range(192):
            glow=math.exp(-(((x-65)/115)**2+((y-140)/95)**2))*18
            raw.extend([int(20+glow),int(38+glow),int(54+glow),255])
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    target=OUT.parent/"shapes/menu-background.png";target.parent.mkdir(parents=True,exist_ok=True)
    target.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',192,192,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(raw),9))+chunk(b'IEND',b''))
    for page in manifest["pages"]:page["widgets"].insert(0,image(0,0,W,H,src="file:shapes/menu-background.png"))

write_backdrop()

def write_research_medallion():
    raw=bytearray()
    for y in range(264):
        raw.append(0)
        for x in range(264):
            r=math.hypot(x-131.5,y-131.5)
            if r<114:color=[245,252,247,255]
            elif r<120:color=[42,117,103,255]
            elif r<126:color=[176,208,195,255]
            else:color=[0,0,0,0]
            raw.extend(color)
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    (OUT.parent/"shapes/research-medallion.png").write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',264,264,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(raw),9))+chunk(b'IEND',b''))

write_research_medallion()

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
            elif widget.get("need_bind") in ("!za.ready","za.nav.menu","za.box.picker") and widget.get("input_block"):
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
    page["widgets"] += tabbar()
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
    native_build=OUT.parent/"B1F12FD919EAE86A.json"
    native_build.write_text(json.dumps({"symbols":{"za_scene_update":"main+0x1B48FD0",
            "za_box_exchange":"main+0xB32BD4"}},indent=1)+"\n",encoding="utf-8")
    OUT.write_text(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print("wrote", OUT)
