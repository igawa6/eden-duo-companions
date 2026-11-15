#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Map where the companion's pictures sit inside YOUR Monster Sanctuary romfs (update v524288,
Ver 2.1.0.32), so the module reads them straight from the game files on the device.

    python3 -I build_picture_index.py <romfs folder> [out file]

Writes pictures.idx: names, sizes and positions only -- which serialized file inside
Data/data.unity3d, where a texture's pixel data starts, each sprite's rectangle in its texture,
and the game font's glyph rectangles and advances. No pixels, nothing of the game's art. The
same approach as tools/dtd/build_picture_index.py.
After regenerating, run build_string_index.py with the same romfs and output path to append
the native localization positions (Q records; no display strings shipped).

The game draws its UI with 2D Toolkit: sprites live in tk2dSpriteCollectionData MonoBehaviours
and fonts in tk2dFontData MonoBehaviours. This build ships no typetrees for them, so both are
parsed from their raw bytes (layout from the game's IL2CPP field list; every serialized bool is
4-byte aligned).

Lines:
  M <version>
  N <node index> <serialized file name inside data.unity3d>
  T <tex id> <node> <pixel data offset in node> <bytes> <width> <height> <TextureFormat>
  S <name> <tex id> <x> <y> <w> <h> <rot>      (pixels, top-left origin; rot 1 = stored turned)
  G <font> <codepoint> <x> <y> <w> <h> <left> <top> <bottom> <advance> <rot>
                     (atlas rect as stored; left/top/bottom in font units, y up; rot 1 = turned)
  L <font> <line height> <tex id>
  A <set> <fps> <wrap mode> <loop start> <frame sprite> <frame sprite> ...
                     (a battle monster's idle clip: frames in clip order, repeats kept = holds)
  O <sprite> <left> <top>    (where the S rect's top-left goes, in texture pixels, y down,
                              relative to the sprite's anchor; given for every A frame)
  P <set> <id> <parent> <texture sprite> key=value ...
                     (one Unity ParticleSystem a monster shows with its idle clip; parent -1 =
                      a root system of the set's particle list, else the system whose particle
                      births fire this one's burst; keys under PARTICLES below)

Battle monsters animate with tk2dSpriteAnimator + tk2dSpriteAnimation (clips[]: name,
frames[] {spriteCollection, spriteId, triggerEvent, eventInfo, eventInt, eventFloat}, fps,
loopStart, wrapMode 0 Loop / 1 LoopSection / 2 Once / 3 PingPong / 4 RandomFrame /
5 RandomLoop / 6 Single). The clip a monster plays while it idles in battle is the one named
"idle": Monster.Start / PlayAnimation / MonsterEntersCombat / PositionForCombat and
MonsterStateManager.ActionFinished load the literal "idle" ("combatIdle" is only used by the
keeper, PlayerController / NPC). Every Monster prefab's library has one, and it is also the
animator's default clip. <set> is the Monster prefab's GameObject name, which is what the
module looks up ("Thanatos", "Vaero"), plus "_light" / "_dark" for the shifted versions;
the sprite names need not match it ("Vaero" plays "birdy/0".."birdy/9").
A shifted monster plays the same library: tk2dBaseSprite.CheckShift swaps each frame's
spriteId for the definition's LightShiftLink / DarkShiftLink, and when that is -1 SetupLink
looks up "<name before />" + "_light" / "_dark" + "/<rest>" in the same collection (where both
are set they agree). The shifted A records apply that rule, and are only written when every
frame resolves (it does for every monster in this build).
PARTICLES: some monsters carry Unity ParticleSystems (the Spectral familiars' blue fire,
Specter, Frosty, IceBlob's snow ...). Monster has NormalParticles / LightParticles /
DarkParticles (the shifted lists are the inactive copies the shift turns on) and
ShowParticlesInMenus (the game shows them in its menus too; SteamGolem's steam it does not).
MonsterParticleOffset moves an emitter with the frame shown: the number after "/" in the frame's
sprite name indexes Offsets (UpdateParticleOffset 0x3D50E0: GetOriginalOffset +
Offsets[int.Parse(name.Substring(i + 1))], no animation check). Only what these systems use is
written (Circle / Box shapes; rate emission or one t=0 burst; birth sub-emitters; size / colour /
velocity over lifetime; limit velocity; gravity; force; billboards); anything else stops the build.
  x=,y=        root emitter position relative to the sprite anchor (px, y down)
  off=n:dx,dy;...   MonsterParticleOffset by frame number (y down)
  loop= dur= rate= burst=min,max
  life=a,b speed=a,b size=a,b grav=g   (start values; gravity modifier, Unity's y up)
  c0=r,g,b,a c1=r,g,b,a   start colour (random between the two)
  shape=box,sx,sy | circle,radius,thickness
  sz=<curve>  vx=<mmc> vy=<mmc>  lim=separate,lx,ly,<mmc>,dampen  force=x,y
  grad=mode/t,r,g,b;.../t,a;...   colour over lifetime (mode 0 blend, 1 fixed)
  ren=0|1      the renderer is enabled
  <mmc> = c:v | r:a,b | k:<curve> | rk:<curve>|<curve>    <curve> = t,v,in,out;...  (scalar folded in)
O: tk2d bakes the anchor into each definition's quad positions (texelSize 1 = pixels here):
left = min x, top = -max y, so frames of a clip line up when each S rect is drawn at
anchor + (left, top); a rot 1 rect is turned back first.
"""
from __future__ import annotations

import os
import re
import struct
import sys

try:
    import UnityPy
except ImportError as e:
    sys.exit(f"missing: {e}  (pip install UnityPy)")

if len(sys.argv) < 2:
    sys.exit(__doc__)
ROOT = os.path.abspath(sys.argv[1])
OUT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "pictures.idx")
DATA = os.path.join(ROOT, "Data", "data.unity3d")

# The "UI" collection carries every chrome sprite in the game (window frames, controller button
# glyphs, buff/debuff icons, map pins, ...); only these are drawn (gen_manifest.py's sliced()/spr()
# calls, plus ms_publish.cpp's WeaknessSign/ResistanceSign and ElementSprite()'s Element<Name>).
UI_SHIPPED = {
    "layout", "menuHeader", "menuItem", "menuItemOpaque", "Selector", "icon_mana",
    "WeaknessSign", "ResistanceSign",
    "ElementFire", "ElementWater", "ElementWind", "ElementEarth", "ElementPhysical",
    "ElementMagical", "ElementNeutral", "ElementDebuff", "ElementQuad",
    # owner round 2: the title logo, the title prompt's A glyph (A starts the game), the
    # Teleporter panel's collapse arrow, the Monster Info card's health icon
    "Title", "ButtonASwitch", "ArrowDown", "icon_health",
    # small gaps 2026-11-12: the item / switch window Back and the target Back carry the B glyph
    # (B does exactly that natively, GLYPH-AUDIT.md)
    "ButtonBSwitch",
    # owner round 3: the focused skill's "[Y] Select Level" (Y opens the level popup natively)
    "ButtonYSwitch",
}
# sprite collections the pages use, by their tk2d collection name
COLLECTIONS = {
    "UI": lambda n: n in UI_SHIPPED or n.startswith(("MapIcon", "MapMarker")),       # windows, rows, selector, element / stat icons
    "Icons": lambda n: True,              # skill, item and equipment icons (game data, any may be needed)
}
# Any of these names identifies the forest backdrop collection; only the three the mockups
# actually draw (scene_bg() in gen_manifest.py) are shipped (see BG_SHIPPED below).
BACKGROUND = {"Treetops1", "Treetops2", "Treetops3", "Treetops4", "FirForest", "mountains1", "Hills"}
BG_SHIPPED = {"DailySky", "mountains1", "FirForest"}
MONSTER_FRAME = re.compile(r"^[A-Za-z]+(_light|_dark)?/0$")  # battle sprites: first idle frame
# the English UI fonts carry no names; they are identified by path id in sharedassets0.assets for
# this build, and checked by line height (bold 14, regular 12, small 12)
FONTS = {"bold": 10422, "regular": 10470, "small": 11048}


class R:
    def __init__(s, b, o=0):
        s.b, s.o = b, o

    def i(s):
        v = struct.unpack_from("<i", s.b, s.o)[0]
        s.o += 4
        return v

    def f(s):
        v = struct.unpack_from("<f", s.b, s.o)[0]
        s.o += 4
        return v

    def al(s):
        s.o = (s.o + 3) & ~3

    def bo(s):
        v = s.b[s.o]
        s.o += 4
        return v

    def s(s_):
        n = s_.i()
        v = s_.b[s_.o:s_.o + n].decode("utf8", "replace")
        s_.o += n
        s_.al()
        return v

    def v(s, n):
        return [s.f() for _ in range(n)]

    def arr(s, fn):
        return [fn() for _ in range(s.i())]

    def pp(s):
        fid = s.i()
        pid = struct.unpack_from("<q", s.b, s.o)[0]
        s.o += 8
        return fid, pid


def header(r):
    r.pp(); r.bo(); r.al(); r.pp()
    return r.s()


def fontchar(r):
    p0 = r.v(3); p1 = r.v(3); uv0 = r.v(3); uv1 = r.v(3); fl = r.bo(); r.al()
    r.arr(lambda: r.v(2)); adv = r.f(); r.i()
    return dict(p0=p0, p1=p1, uv0=uv0, uv1=uv1, flipped=fl, advance=adv)


def font(raw):
    r = R(raw); name = header(r); r.i(); lh = r.f()
    chars = r.arr(lambda: fontchar(r)); keys = r.arr(r.i); vals = r.arr(lambda: fontchar(r))
    r.arr(r.s); r.arr(r.s); r.pp(); [r.bo() for _ in range(2)]; sc = r.pp()
    return dict(name=name, lineHeight=lh, chars=chars, keys=keys, vals=vals, collection=sc)


def spritedef(r):
    d = {}
    d["dark"] = r.i(); d["light"] = r.i(); r.arr(r.i); d["name"] = r.s()
    r.arr(lambda: r.v(3)); r.arr(lambda: r.v(3)); d["texel"] = r.v(2)
    d["pos"] = r.arr(lambda: r.v(3)); r.arr(lambda: r.v(3)); r.arr(lambda: r.v(4)); d["uvs"] = r.arr(lambda: r.v(2))
    r.arr(lambda: r.v(2)); r.arr(r.i); r.pp(); d["mat"] = r.i(); r.arr(r.i); r.s(); r.bo(); r.al()
    [r.i() for _ in range(4)]; d["flipped"] = r.i(); r.bo(); r.al(); r.i(); r.i()

    def cc():
        r.i(); r.v(3); r.f(); r.s(); r.arr(lambda: r.v(3)); r.arr(r.f)
    r.arr(cc); r.arr(lambda: r.v(3)); r.arr(r.i); r.arr(r.i); r.bo(); r.bo(); r.al()
    c2 = lambda: r.arr(lambda: r.v(2))
    r.arr(c2); r.arr(c2); r.arr(lambda: (r.s(), r.v(3), r.f()))
    return d


def collection(raw):
    r = R(raw); header(r); r.i(); r.bo(); r.bo()
    defs = r.arr(lambda: spritedef(r)); r.bo(); r.al(); r.pp(); mats = r.arr(r.pp); tex = r.arr(r.pp)
    name = "?"
    try:
        r.arr(r.pp); r.arr(r.i); r.i(); r.bo(); r.bo(); r.al(); r.s(); name = r.s()
    except Exception:
        pass
    return dict(defs=defs, textures=tex, name=name)


def animation(raw):
    """tk2dSpriteAnimation: the clips of one library (every byte accounted for)."""
    r = R(raw); header(r)

    def frame():
        f = dict(collection=r.pp(), sprite=r.i()); r.bo(); r.s(); r.i(); r.f()
        return f

    def clip():
        return dict(name=r.s(), frames=r.arr(frame), fps=r.f(), loop=r.i(), wrap=r.i())
    clips = r.arr(clip)
    if r.o != len(raw):
        raise ValueError("tk2dSpriteAnimation layout")
    return clips


def animator_library(raw):
    """tk2dSpriteAnimator: (GameObject path id, library PPtr)."""
    r = R(raw); go = r.pp(); r.bo(); r.al(); r.pp(); r.s()
    return go[1], r.pp()


def shifted(c, sid, shift):
    """tk2dBaseSprite.CheckShift / SetupLink: the sprite a light/dark shifted monster shows."""
    link = c["defs"][sid][shift]
    if link != -1:
        return link
    if "ids" not in c:
        c["ids"] = {d["name"]: i for i, d in enumerate(c["defs"])}
    head, sep, tail = c["defs"][sid]["name"].partition("/")
    return c["ids"].get(head + "_" + shift + sep + tail, -1)


def monster_particles(raw):
    """Monster: (ShowParticlesInMenus, [Normal, Light, Dark] lists of ParticleSystem PPtrs).
    Field order from the IL2CPP dump: Referenceable.ID, Name, three MonsterBaseStats of five ints,
    PortraitOffset ... MonsterTypes, then the three lists."""
    r = R(raw); header(r)
    r.i(); r.s()
    for _ in range(15):
        r.i()
    r.f(); r.f(); r.i(); r.f()
    r.arr(r.pp); r.arr(r.pp); r.bo(); r.pp(); r.i(); r.f(); r.f(); r.bo(); r.i(); r.f(); r.f()
    menus = r.bo()
    r.pp(); r.f(); r.f(); r.pp(); r.f(); r.f(); r.f(); r.arr(r.i)
    return menus, [r.arr(r.pp) for _ in range(3)]


def particle_offset_data(raw):
    """MonsterParticleOffset.OffsetData: [(GameObject PPtr, [(x, y, z) by frame number])]."""
    r = R(raw); header(r)
    out = []
    for _ in range(r.i()):
        r.i()  # EMonsterAnimation: UpdateParticleOffset does not check it
        go = r.pp()
        out.append((go, r.arr(lambda: (r.f(), r.f(), r.f()))))
    return out


def _g(v):
    return f"{v:.5g}"


def _curve(c, scalar=1.0):
    keys = c.get("m_Curve", [])
    if not keys:
        return f"0,{_g(scalar)},0,0"
    return ";".join(f"{_g(k['time'])},{_g(k['value'] * scalar)},{_g(k['inSlope'] * scalar)},"
                    f"{_g(k['outSlope'] * scalar)}" for k in keys)


def _mmc(c):
    st = c["minMaxState"]
    if st == 0:
        return f"c:{_g(c['scalar'])}"
    if st == 3:
        return f"r:{_g(c['minScalar'])},{_g(c['scalar'])}"
    if st == 1:
        return "k:" + _curve(c["maxCurve"], c["scalar"])
    if st == 2:
        return "rk:" + _curve(c["minCurve"], c["scalar"]) + "|" + _curve(c["maxCurve"], c["scalar"])
    raise SystemExit(f"MinMaxCurve state {st}")


def _range(c):
    st = c["minMaxState"]
    if st == 0:
        return f"{_g(c['scalar'])},{_g(c['scalar'])}"
    if st == 3:
        return f"{_g(c['minScalar'])},{_g(c['scalar'])}"
    raise SystemExit(f"start value state {st}")


def _gradient(g):
    keys = [(g[f"ctime{i}"] / 65535, g[f"key{i}"]) for i in range(g["m_NumColorKeys"])]
    alphas = [(g[f"atime{i}"] / 65535, g[f"key{i}"]["a"]) for i in range(g["m_NumAlphaKeys"])]
    return (f"{g.get('m_Mode', 0)}/" + ";".join(f"{_g(t)},{_g(k['r'])},{_g(k['g'])},{_g(k['b'])}" for t, k in keys)
            + "/" + ";".join(f"{_g(t)},{_g(a)}" for t, a in alphas))


PARTICLE_MODULES = {"InitialModule", "ShapeModule", "EmissionModule", "SizeModule", "ColorModule", "VelocityModule",
                    "ClampVelocityModule", "SubModule", "InheritVelocityModule", "ForceModule"}


def particle_system(tt, renderer):
    """key=value tokens of one ParticleSystem (only the modules these monsters use)."""
    I, S, E = tt["InitialModule"], tt["ShapeModule"], tt["EmissionModule"]
    used = {k for k, v in tt.items() if isinstance(v, dict) and v.get("enabled")}
    if used - PARTICLE_MODULES:
        raise SystemExit(f"unhandled particle modules {sorted(used - PARTICLE_MODULES)}")
    t = [f"loop={int(tt['looping'])}", f"dur={_g(tt['lengthInSec'])}"]
    rate = E["rateOverTime"]
    if E["enabled"]:
        if rate["minMaxState"] == 0 and rate["scalar"] > 0:
            t.append(f"rate={_g(rate['scalar'])}")
        elif rate["minMaxState"] == 3:
            t.append(f"rate={_g((rate['scalar'] + rate['minScalar']) / 2)}")
        for b in E.get("m_Bursts", []):
            if b["time"] != 0 or b["cycleCount"] != 1 or b.get("probability", 1) != 1:
                raise SystemExit("only one burst at t=0 is handled")
            t.append(f"burst={_range(b['countCurve'])}")
    t += [f"life={_range(I['startLifetime'])}", f"speed={_range(I['startSpeed'])}", f"size={_range(I['startSize'])}",
          f"grav={_g(I['gravityModifier']['scalar'])}"]
    col = I["startColor"]
    if col["minMaxState"] not in (0, 2):
        raise SystemExit("start colour mode")
    c0, c1 = (col["maxColor"], col["maxColor"]) if col["minMaxState"] == 0 else (col["minColor"], col["maxColor"])
    t += ["c0=" + ",".join(_g(c0[k]) for k in "rgba"), "c1=" + ",".join(_g(c1[k]) for k in "rgba")]
    if S["enabled"]:
        if S["type"] == 5:
            t.append(f"shape=box,{_g(S['m_Scale']['x'])},{_g(S['m_Scale']['y'])}")
        elif S["type"] == 10:
            rad = S["radius"]["value"] if isinstance(S["radius"], dict) else S["radius"]
            if (S["arc"]["value"] if isinstance(S["arc"], dict) else S["arc"]) != 360:
                raise SystemExit("circle arc")
            t.append(f"shape=circle,{_g(rad)},{_g(S.get('radiusThickness', 1))}")
        else:
            raise SystemExit(f"shape {S['type']}")
    if "SizeModule" in used:
        sz = tt["SizeModule"]["curve"]
        if sz["minMaxState"] != 1 or tt["SizeModule"].get("separateAxes"):
            raise SystemExit("size over lifetime mode")
        t.append("sz=" + _curve(sz["maxCurve"], sz["scalar"]))
    if "VelocityModule" in used:
        v = tt["VelocityModule"]
        if v.get("inWorldSpace"):
            raise SystemExit("world-space velocity")
        t += ["vx=" + _mmc(v["x"]), "vy=" + _mmc(v["y"])]
    if "ClampVelocityModule" in used:
        c = tt["ClampVelocityModule"]
        t.append(f"lim={int(c['separateAxis'])},{_g(c['x']['scalar'])},{_g(c['y']['scalar'])},"
                 f"{_mmc(c['magnitude'])},{_g(c['dampen'])}")
    if "ColorModule" in used:
        g = tt["ColorModule"]["gradient"]
        if g["minMaxState"] != 1:
            raise SystemExit("colour over lifetime mode")
        t.append("grad=" + _gradient(g["maxGradient"]))
    if "ForceModule" in used:
        f = tt["ForceModule"]
        if f["x"]["minMaxState"] != 0 or f["y"]["minMaxState"] != 0 or f.get("inWorldSpace"):
            raise SystemExit("force mode")
        t.append(f"force={_g(f['x']['scalar'])},{_g(f['y']['scalar'])}")
    t.append(f"ren={int(bool(renderer and renderer['m_Enabled']))}")
    return t


def particles(bundle, monsters, monster_raw, particle_offsets, clips_a, texture, sprites):
    """P records: the particle systems of every monster with an indexed idle clip that the game
    also shows in its menus (Monster.ShowParticlesInMenus)."""
    have_clip = {key for key, _idle, _ids in clips_a}
    textures_done, lines = {}, []

    def obj_of(af, pp):
        f = af if pp[0] == 0 else bundle.files[af.externals[pp[0] - 1].path.split("/")[-1]]
        return f, f.objects[pp[1]]

    def components(go):
        for c in go.m_Components:
            yield (c.component if hasattr(c, "component") else c).deref()

    def tex_sprite(af, renderer):
        mats = [m for m in renderer["m_Materials"] if m["m_PathID"]]
        if not mats:
            return "-"
        mf, mo = obj_of(af, (mats[0]["m_FileID"], mats[0]["m_PathID"]))
        for _name, tex_env in mo.read().m_SavedProperties.m_TexEnvs:
            ref_ = tex_env.m_Texture
            if ref_ and ref_.path_id:
                key = (mf.name, ref_.file_id, ref_.path_id)
                if key not in textures_done:
                    tid, w, h = texture(mf, ref_.file_id, ref_.path_id)
                    name = "fx/" + ref_.read().m_Name
                    sprites.append(f"S {name} {tid} 0 0 {w} {h} 0")
                    textures_done[key] = name
                return textures_done[key]
        return "-"

    seen = set()
    for af, go in monsters:
        gname = af.objects[go].read().m_Name
        if " " in gname or gname in seen or (af.name, go) not in monster_raw:
            continue
        menus, lists = monster_particles(monster_raw[(af.name, go)])
        if not any(lists):
            continue
        seen.add(gname)
        if not menus:
            print(f"{gname}: particles not shown in menus (ShowParticlesInMenus off), skipped")
            continue
        root = next(o for o in components(af.objects[go].read()) if o.type.name == "Transform").path_id
        offs = particle_offsets.get((af.name, go), [])
        # MonsterParticleOffset.Awake records the active (Normal) direct-child systems' local
        # positions by name, and UpdateParticleOffset places every system whose name has offset
        # data at that recorded position + the frame's offset: the inactive Light / Dark copies
        # (same names) burn where the Normal one does, whatever their own transform says.
        normal_at = {}
        for shift, lst in zip(("", "light", "dark"), lists):
            key = gname + ("_" + shift if shift else "")
            if key not in have_clip or not lst:
                continue
            count = [0]

            def emit(sf, so, parent, head=()):
                sid = count[0]
                count[0] += 1
                tt = so.read_typetree()
                g = so.read().m_GameObject.read()
                rend = next((o.read_typetree() for o in components(g) if o.type.name == "ParticleSystemRenderer"), None)
                tex = tex_sprite(sf, rend) if rend else "-"
                lines.append(f"P {key.lower()} {sid} {parent} {tex} " + " ".join(list(head) + particle_system(tt, rend)))
                if tt["SubModule"].get("enabled"):
                    for e in tt["SubModule"]["subEmitters"]:
                        if e["type"] != 0 or e.get("emitProbability", 1) != 1:
                            raise SystemExit(f"{key}: only birth sub-emitters are handled")
                        emit(*obj_of(sf, (e["emitter"]["m_FileID"], e["emitter"]["m_PathID"])), sid)

            for pp in lst:
                sf, so = obj_of(af, pp)
                g = so.read().m_GameObject.read()
                tobj = next(o for o in components(g) if o.type.name == "Transform")
                t = tobj.read()
                x = y = 0.0
                while tobj.path_id != root:
                    if abs(t.m_LocalScale.x - 1) > 1e-3 or abs(t.m_LocalScale.y - 1) > 1e-3:
                        raise SystemExit(f"{key}: scaled emitter transform")
                    x += t.m_LocalPosition.x
                    y += t.m_LocalPosition.y
                    if not t.m_Father or not t.m_Father.path_id:
                        raise SystemExit(f"{key}: emitter is not under the monster")
                    tobj = t.m_Father.deref()
                    t = tobj.read()
                off = next((values for ogo, values in offs
                            if values and obj_of(af, ogo)[1].read().m_Name == g.m_Name), None)
                if not shift:
                    normal_at.setdefault(g.m_Name, (x, y))
                elif off is not None and g.m_Name in normal_at:
                    x, y = normal_at[g.m_Name]
                head = [f"x={_g(x)}", f"y={_g(-y)}"]
                if off is not None:
                    head.append("off=" + ";".join(f"{n}:{_g(vx)},{_g(-vy)}" for n, (vx, vy, _vz) in enumerate(off)))
                emit(sf, so, -1, head)
    # The native map's teleporter sparkle: MinimapTileView's Teleporter child carries a Rect2x2
    # particle system (set "maptp"; x, y relative to the tile view's own origin).
    tiles = bundle.files["sharedassets1.assets"]
    for pid, o in tiles.objects.items():
        if o.type.name != "ParticleSystem":
            continue
        g = o.read().m_GameObject.read()
        t = next(c for c in components(g) if c.type.name == "Transform").read()
        chain, x, y = [g.m_Name], 0.0, 0.0
        while t.m_Father and t.m_Father.path_id:
            x += t.m_LocalPosition.x
            y += t.m_LocalPosition.y
            t = t.m_Father.read()
            chain.append(t.m_GameObject.read().m_Name)
        if chain == ["Particle System", "Teleporter", "MinimapTileView"]:
            rend = next((c.read_typetree() for c in components(g) if c.type.name == "ParticleSystemRenderer"), None)
            lines.append(f"P maptp 0 -1 {tex_sprite(tiles, rend)} x={_g(x)} y={_g(-y)} "
                         + " ".join(particle_system(o.read_typetree(), rend)))
            break
    else:
        raise SystemExit("MinimapTileView/Teleporter/Particle System not found")
    return lines


def main():
    env = UnityPy.load(DATA)
    bundle = list(env.files.values())[0]
    nodes = list(bundle.files.keys())
    node_of = {n: i for i, n in enumerate(nodes)}
    scripts = {}
    for o in env.objects:
        if o.type.name == "MonoScript":
            scripts[(o.assets_file.name, o.path_id)] = o.read().m_ClassName
    tex_ids, tex_lines = {}, []

    def texture(af, fid, pid):
        target = af if fid == 0 else bundle.files[af.externals[fid - 1].path.split("/")[-1]]
        key = (target.name, pid)
        if key in tex_ids:
            return tex_ids[key]
        obj = target.objects[pid]
        t = obj.read()
        data = bytes(t.image_data)
        raw = obj.get_raw_data()
        at = raw.find(data[:256]) if data else -1
        if at < 0 or raw[at:at + len(data)] != data:
            raise SystemExit(f"texture {key}: pixel data not found inline (streamed?)")
        tid = len(tex_lines)
        tex_lines.append(f"T {tid} {node_of[target.name]} {obj.byte_start + at} {len(data)} "
                         f"{t.m_Width} {t.m_Height} {int(t.m_TextureFormat)}")
        tex_ids[key] = (tid, t.m_Width, t.m_Height)
        return tex_ids[key]

    sprites, glyphs, lines_l, offsets, anims_l = [], [], [], [], []
    fonts_found = {}
    found, libraries, animators, monsters = [], {}, {}, []
    monster_raw, particle_offsets = {}, {}

    def ref(af, pp):
        return (af.name if pp[0] == 0 else af.externals[pp[0] - 1].path.split("/")[-1], pp[1])
    for o in env.objects:
        if o.type.name != "MonoBehaviour":
            continue
        raw = o.get_raw_data()
        fid, pid = struct.unpack_from("<iq", raw, 16)
        af = o.assets_file
        key = (af.name, pid) if fid == 0 else (af.externals[fid - 1].path.split("/")[-1], pid)
        cls = next((c for (n, p), c in scripts.items() if p == key[1] and n.lower().endswith(key[0].lower())), None)
        if cls == "tk2dSpriteCollectionData":
            try:
                found.append((af, o.path_id, collection(raw)))
            except Exception:
                continue
        elif cls == "tk2dSpriteAnimation":
            libraries[(af.name, o.path_id)] = (af, animation(raw))
        elif cls == "tk2dSpriteAnimator":
            go, lib = animator_library(raw)
            animators.setdefault((af.name, go), ref(af, lib))
        elif cls == "Monster":
            monsters.append((af, struct.unpack_from("<q", raw, 4)[0]))  # its GameObject
            monster_raw[(af.name, monsters[-1][1])] = raw
        elif cls == "MonsterParticleOffset":
            particle_offsets[(af.name, struct.unpack_from("<q", raw, 4)[0])] = particle_offset_data(raw)
        elif cls == "tk2dFontData":
            try:
                f = font(raw)
            except Exception:
                continue
            if af.name == "sharedassets0.assets":
                fonts_found[o.path_id] = (af, f)

    # battle monsters: the idle clip of each Monster prefab's library, plain and shifted
    cols = {(af.name, pid): c for af, pid, c in found}
    clips_found, no_idle, clips_a, frames_of = {}, [], [], {}
    for af, go in monsters:
        gname = af.objects[go].read().m_Name
        if " " in gname:  # scene copies ("GoblinBrute (1)" in level92); the prefab has the clip
            continue
        lib = libraries.get(animators.get((af.name, go)))
        idle = next((c for c in lib[1] if c["name"] == "idle"), None) if lib else None
        if not idle or not idle["frames"]:
            no_idle.append(gname)
            continue
        frames = [(ref(lib[0], f["collection"]), f["sprite"]) for f in idle["frames"]]
        if clips_found.setdefault(gname, (idle, frames))[1] != frames:
            raise SystemExit(f"{gname}: prefabs disagree on the idle clip")
    if no_idle:
        print("monsters without an idle clip:", sorted(set(no_idle)))
    for gname, (idle, frames) in sorted(clips_found.items()):
        for shift in ("", "light", "dark"):
            ids = [(ck, shifted(cols[ck], sid, shift) if shift else sid) for ck, sid in frames]
            if any(sid < 0 for _, sid in ids):
                print(f"{gname}: not every frame has a {shift} sprite, clip skipped")
                continue
            clips_a.append((gname + ("_" + shift if shift else ""), idle, ids))
            for ck, sid in ids:
                frames_of.setdefault(ck, set()).add(sid)

    sprite_name = {}  # (collection, sprite id) -> name as written in its S record
    for af, pid, c in found:
        ck = (af.name, pid)
        clip_ids = frames_of.get(ck, set())
        wanted = COLLECTIONS.get(c["name"])
        if not wanted and {"Vaero", "Silvaero"} <= {d["name"] for d in c["defs"]}:
            c["name"] = "Big"  # one still per monster: fallback when no battle frame /0 is indexed
            wanted = lambda n: True
        if not wanted and any(d["name"] in BACKGROUND for d in c["defs"]):
            c["name"] = "BG"  # the forest backdrop layers the mockups use behind windows
            wanted = lambda n: n in BG_SHIPPED
        is_monster = any(MONSTER_FRAME.match(d["name"]) for d in c["defs"])
        if not wanted and not is_monster and not clip_ids:
            continue
        for sid, d in enumerate(c["defs"]):
            if not d["uvs"] or (is_monster and not wanted and not MONSTER_FRAME.match(d["name"])
                                and sid not in clip_ids):
                continue
            if not is_monster and wanted and not wanted(d["name"]):
                continue
            tf, tp = c["textures"][min(d["mat"], len(c["textures"]) - 1)]
            tid, W, H = texture(af, tf, tp)
            us = [u for u, v in d["uvs"]]; vs = [v for u, v in d["uvs"]]
            x0, x1 = round(min(us) * W), round(max(us) * W)
            y0, y1 = round((1 - max(vs)) * H), round((1 - min(vs)) * H)
            prefix = "" if is_monster and not wanted else c["name"].lower() + "/"
            name = prefix + d["name"]
            sprites.append(f"S {name} {tid} {x0} {y0} {x1 - x0} {y1 - y0} {1 if d['flipped'] else 0}")
            if sid not in clip_ids:
                continue
            sprite_name[(ck, sid)] = name
            xs = [p[0] / d["texel"][0] for p in d["pos"]]; ys = [p[1] / d["texel"][1] for p in d["pos"]]
            w, h = (y1 - y0, x1 - x0) if d["flipped"] else (x1 - x0, y1 - y0)
            if abs(max(xs) - min(xs) - w) > 0.5 or abs(max(ys) - min(ys) - h) > 0.5:
                raise SystemExit(f"{name}: quad {max(xs) - min(xs)}x{max(ys) - min(ys)} is not its rect {w}x{h}")
            offsets.append(f"O {name} {round(min(xs))} {round(-max(ys))}")
    for key, idle, ids in clips_a:
        frame_names = [sprite_name[i] for i in ids]
        if any(" " in n for n in frame_names):
            print(f"{key}: a frame name has a space, clip skipped")
            continue
        anims_l.append(f"A {key} {idle['fps']:g} {idle['wrap']} {idle['loop']} " + " ".join(frame_names))
    fx_lines = particles(bundle, monsters, monster_raw, particle_offsets, clips_a, texture, sprites)
    for short, want in FONTS.items():
        if want not in fonts_found:
            print("font not found:", short, want, sorted(fonts_found))
            continue
        af, f = fonts_found[want]
        cf, cp = f["collection"]
        target = af if cf == 0 else bundle.files[af.externals[cf - 1].path.split("/")[-1]]
        col = None
        for o in target.objects.values():
            if o.path_id == cp:
                col = collection(o.get_raw_data())
        tid, W, H = texture(target, *col["textures"][0]) if col else (None, 0, 0)
        lines_l.append(f"L {short} {f['lineHeight']:g} {tid}")
        table = dict(zip(f["keys"], f["vals"])) if f["keys"] else dict(enumerate(f["chars"]))
        for cp_, ch in sorted(table.items()):
            if cp_ < 32 or cp_ > 0xFFFF:
                continue
            u0, v0 = ch["uv0"][0], ch["uv0"][1]; u1, v1 = ch["uv1"][0], ch["uv1"][1]
            x, y = round(min(u0, u1) * W), round((1 - max(v0, v1)) * H)
            w, h = round(abs(u1 - u0) * W), round(abs(v1 - v0) * H)
            glyphs.append(f"G {short} {cp_} {x} {y} {w} {h} {round(ch['p0'][0])} {round(ch['p0'][1])} "
                          f"{round(ch['p1'][1])} {round(ch['advance'])} {1 if ch['flipped'] else 0}")
    # Native map teleporter particles use Rect2x2 (sharedassets0 material 44 -> texture 88),
    # not a MapIcon sprite. Keep the original RGB24 texture in the same range-read pipeline.
    particle = bundle.files["sharedassets0.assets"].objects[88].read()
    assert particle.m_Name == "Rect2x2" and particle.m_Width == particle.m_Height == 2
    tid, W, H = texture(bundle.files["sharedassets0.assets"], 0, 88)
    sprites.append(f"S map/teleporter {tid} 0 0 {W} {H} 0")
    with open(OUT, "w") as out:
        out.write("M 1\n")
        for i, n in enumerate(nodes):
            out.write(f"N {i} {n}\n")
        out.write("\n".join(tex_lines + lines_l + sprites + anims_l + offsets + fx_lines + glyphs) + "\n")
    print(f"{OUT}: {len(tex_lines)} textures, {len(sprites)} sprites, {len(anims_l)} idle clips, "
          f"{len(offsets)} offsets, {len(fx_lines)} particle systems, {len(glyphs)} glyphs, "
          f"fonts {sorted(fonts_found)}")


main()
