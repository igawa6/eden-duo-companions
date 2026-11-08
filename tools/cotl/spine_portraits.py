#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 AlterDigga
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads bones, slots and the head attachments per skin from Follower.skel (Spine 3.8, binary).

  spine_portraits.py <Follower.skel> <Follower.atlas.txt> (--emit OUT.inc | --dump SKIN ...)

Basis for the follower images (native/follower_portrait): head from HEAD_SKIN_BTM and
HEAD_SKIN_TOP, on top markings, eyes and mouth in the rest pose of the skeleton.
Format after spine-runtimes 3.8 SkeletonBinary, like tools/cotl/spine_skins.py.
"""
import math
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from spine_skins import R  # noqa: E402


class Skel:
    pass


def parse(path):
    r = R(open(path, "rb").read())
    sk = Skel()
    r.string(); version = r.string()
    assert version.startswith("3.8"), version
    for _ in range(4):
        r.float()
    nonessential = r.boolean()
    if nonessential:
        r.float(); r.string(); r.string()
    strings = [r.string() for _ in range(r.varint())]

    def ref():
        i = r.varint()
        return None if i == 0 else strings[i - 1]

    sk.bones = []
    for i in range(r.varint()):
        name = r.string()
        parent = r.varint() if i else -1
        rot, x, y, sx, sy, shx, shy, length = (r.float() for _ in range(8))
        mode = r.varint(); r.boolean()
        if nonessential:
            r.int()
        sk.bones.append(dict(name=name, parent=parent, rot=rot, x=x, y=y, sx=sx, sy=sy, shx=shx,
                             shy=shy, mode=mode))
    sk.slots = []
    for _ in range(r.varint()):
        name = r.string(); bone = r.varint(); color = r.int(); r.int(); att = ref(); r.varint()
        sk.slots.append(dict(name=name, bone=bone, color=color, att=att))
    for _ in range(r.varint()):  # IK
        r.string(); r.varint(); r.boolean()
        for _ in range(r.varint()):
            r.varint()
        r.varint(); r.float(); r.float(); r.byte(); r.boolean(); r.boolean(); r.boolean()
    for _ in range(r.varint()):  # Transform
        r.string(); r.varint(); r.boolean()
        for _ in range(r.varint()):
            r.varint()
        r.varint(); r.boolean(); r.boolean()
        for _ in range(10):
            r.float()
    for _ in range(r.varint()):  # Path
        r.string(); r.varint(); r.boolean()
        for _ in range(r.varint()):
            r.varint()
        r.varint(); r.varint(); r.varint(); r.varint()
        for _ in range(5):
            r.float()

    def vertices(n):
        if not r.boolean():
            return [r.float() for _ in range(n * 2)], None
        weighted = []
        for _ in range(n):
            weighted.append([(r.varint(), r.float(), r.float(), r.float()) for _ in range(r.varint())])
        return None, weighted

    def attachment(key):
        name = ref() or key
        t = r.byte()
        if t == 0:
            p = ref() or name
            rot, x, y, sx, sy, w, h = (r.float() for _ in range(7))
            r.int()
            return dict(kind="region", path=p, rot=rot, x=x, y=y, sx=sx, sy=sy, w=w, h=h)
        if t == 1:
            vertices(r.varint())
            if nonessential:
                r.int()
            return None
        if t == 2:
            p = ref() or name; r.int(); n = r.varint()
            uvs = [r.float() for _ in range(n * 2)]
            for _ in range(r.varint()):
                r.p += 2
            flat, weighted = vertices(n); r.varint()
            if nonessential:
                for _ in range(r.varint()):
                    r.p += 2
                r.float(); r.float()
            return dict(kind="mesh", path=p, uvs=uvs, verts=flat, weighted=weighted)
        if t == 3:
            p = ref() or name; r.int(); s = ref(); parent = ref(); r.boolean()
            if nonessential:
                r.float(); r.float()
            return dict(kind="linkedmesh", path=p, skin=s, parent=parent)
        if t == 4:
            r.boolean(); r.boolean(); n = r.varint(); vertices(n)
            for _ in range(n // 3):
                r.float()
            if nonessential:
                r.int()
            return None
        if t == 5:
            r.float(); r.float(); r.float()
            if nonessential:
                r.int()
            return None
        if t == 6:
            r.varint(); n = r.varint(); vertices(n)
            if nonessential:
                r.int()
            return None
        raise SystemExit(f"Attachment type {t} not supported")

    def skin(default):
        if default:
            n = r.varint()
            if n == 0:
                return None, {}
            name = "default"
        else:
            name = ref()
            for _ in range(4):
                for _ in range(r.varint()):
                    r.varint()
            n = r.varint()
        atts = {}
        for _ in range(n):
            slot = r.varint()
            for _ in range(r.varint()):
                key = ref()
                a = attachment(key)
                if a:
                    atts[(slot, key)] = a
        return name, atts

    sk.skins = {}
    name, atts = skin(True)
    sk.skins["default"] = atts
    for _ in range(r.varint()):
        name, atts = skin(False)
        sk.skins[name] = atts
    return sk


def world(sk):
    """World matrices (a, b, c, d, x, y) of the rest pose; only normal inheritance (mode 0)."""
    out = []
    for b in sk.bones:
        rx = math.radians(b["rot"] + b["shx"]); ry = math.radians(b["rot"] + 90 + b["shy"])
        la, lb = math.cos(rx) * b["sx"], math.cos(ry) * b["sy"]
        lc, ld = math.sin(rx) * b["sx"], math.sin(ry) * b["sy"]
        if b["parent"] < 0:
            out.append((la, lb, lc, ld, b["x"], b["y"]))
            continue
        pa, pb, pc, pd, px, py = out[b["parent"]]
        out.append((pa * la + pb * lc, pa * lb + pb * ld, pc * la + pd * lc, pc * lb + pd * ld,
                    pa * b["x"] + pb * b["y"] + px, pc * b["x"] + pd * b["y"] + py))
    return out



PORTRAIT_SLOTS = ("EXTRA_BTM", "HEAD_SKIN_BTM", "HEAD_SKIN_TOP", "MARKINGS", "EXTRA_TOP",
                  "EYE_LEFT", "EYE_RIGHT", "MOUTH")


def find_att(sk, skin, slot, key):
    for s in (skin, "default"):
        a = sk.skins.get(s, {}).get((slot, key))
        if a:
            return a
    return None


def mesh_points(sk, wm, slot, a, skin):
    """World points and UVs of a mesh in the rest pose (linked meshes use the parent mesh)."""
    if a["kind"] == "linkedmesh":
        parent = find_att(sk, a["skin"] or "default", slot, a["parent"])
        if parent is None or parent["kind"] != "mesh":
            return None
        a = dict(parent, path=a["path"])
    pts = []
    if a["verts"] is not None:
        m = wm[sk.slots[slot]["bone"]]
        v = a["verts"]
        for i in range(0, len(v), 2):
            pts.append((m[0] * v[i] + m[1] * v[i + 1] + m[4], m[2] * v[i] + m[3] * v[i + 1] + m[5]))
    else:
        for infl in a["weighted"]:
            x = y = 0.0
            for bone, vx, vy, w in infl:
                m = wm[bone]
                x += (m[0] * vx + m[1] * vy + m[4]) * w
                y += (m[2] * vx + m[3] * vy + m[5]) * w
            pts.append((x, y))
    uvs = [(a["uvs"][i], a["uvs"][i + 1]) for i in range(0, len(a["uvs"]), 2)]
    return pts, uvs


def fit_affine(src, dst):
    """Least squares: dst ≈ (a·u + b·v + tx, c·u + d·v + ty); returns matrix and largest error."""
    n = len(src)
    su = sum(u for u, _ in src); sv = sum(v for _, v in src)
    suu = sum(u * u for u, _ in src); svv = sum(v * v for _, v in src); suv = sum(u * v for u, v in src)
    mat = [[suu, suv, su], [suv, svv, sv], [su, sv, n]]

    def solve(rhs):
        import copy
        a = copy.deepcopy(mat); b = list(rhs)
        for i in range(3):
            p = max(range(i, 3), key=lambda k: abs(a[k][i]))
            a[i], a[p] = a[p], a[i]; b[i], b[p] = b[p], b[i]
            for k in range(i + 1, 3):
                f = a[k][i] / a[i][i]
                for j in range(i, 3):
                    a[k][j] -= f * a[i][j]
                b[k] -= f * b[i]
        x = [0.0] * 3
        for i in (2, 1, 0):
            x[i] = (b[i] - sum(a[i][j] * x[j] for j in range(i + 1, 3))) / a[i][i]
        return x

    ax = solve([sum(u * x for (u, _), (x, _) in zip(src, dst)), sum(v * x for (_, v), (x, _) in zip(src, dst)),
                sum(x for x, _ in dst)])
    ay = solve([sum(u * y for (u, _), (_, y) in zip(src, dst)), sum(v * y for (_, v), (_, y) in zip(src, dst)),
                sum(y for _, y in dst)])
    err = max(math.hypot(ax[0] * u + ax[1] * v + ax[2] - x, ay[0] * u + ay[1] * v + ay[2] - y)
              for (u, v), (x, y) in zip(src, dst))
    return (ax[0], ax[1], ay[0], ay[1], ax[2], ay[2]), err


def region_affine(wm, bone, a):
    """Region: pixel (px, py) of the image (origin top left) → world."""
    r = math.radians(a["rot"])
    cos, sin = math.cos(r), math.sin(r)
    w, h = a["w"], a["h"]
    # local: x = (px - w/2)·sx, y = (h/2 - py)·sy, rotated, translated
    la, lb = cos * a["sx"], -sin * -a["sy"]
    lc, ld = sin * a["sx"], cos * -a["sy"]
    lx = a["x"] + la * (-w / 2) + lb * (-h / 2)
    ly = a["y"] + lc * (-w / 2) + ld * (-h / 2)
    m = wm[bone]
    return (m[0] * la + m[1] * lc, m[0] * lb + m[1] * ld, m[2] * la + m[3] * lc, m[2] * lb + m[3] * ld,
            m[0] * lx + m[1] * ly + m[4], m[2] * lx + m[3] * ly + m[5])



def det(m):
    return m[0] * m[3] - m[1] * m[2]


def compose_translate(m, tx, ty):
    """m ∘ T(tx, ty): first translate by (tx, ty), then m."""
    return (m[0], m[1], m[2], m[3], m[0] * tx + m[1] * ty + m[4], m[2] * tx + m[3] * ty + m[5])


def portrait_parts(sk, wm, skin, atlas):
    """[(slot name, region path, matrix, error)] in draw order.

    Matrix: pixel in the original frame of the region (origin top left, unrotated) → world (y up).
    atlas: path → dict(w, h, orig_w, orig_h, off_x, off_y) from atlas_info. Placeholders with a scale
    near 0 are dropped.
    """
    out = []
    for i, s in enumerate(sk.slots):
        if s["name"] not in PORTRAIT_SLOTS or not s["att"]:
            continue
        a = find_att(sk, skin, i, s["att"])
        if a is None or a["path"] not in atlas:
            continue
        r = atlas[a["path"]]
        if a["kind"] == "region":
            m, err = region_affine(wm, s["bone"], dict(a, w=r["orig_w"], h=r["orig_h"])), 0.0
        else:
            got = mesh_points(sk, wm, i, a, skin)
            if got is None:
                continue
            pts, uvs = got
            # Spine 3.8: mesh UVs refer to the packed (trimmed) cut-out
            m, err = fit_affine([(u * r["w"], v * r["h"]) for u, v in uvs], pts)
            m = compose_translate(m, -r["off_x"], -(r["orig_h"] - r["off_y"] - r["h"]))
        if abs(det(m)) < 1e-3:
            continue
        out.append((s["name"], a["path"], m, err))
    return out


def atlas_info(atlas_path):
    info, name = {}, None
    for line in open(atlas_path):
        s = line.strip()
        if not line.startswith(" ") and ":" not in line:
            name = s
            info[name] = {}
        elif name and ":" in s:
            k, v = s.split(":", 1)
            vals = [x.strip() for x in v.split(",")]
            if k == "size" and len(vals) == 2:
                info[name]["w"], info[name]["h"] = int(vals[0]), int(vals[1])
            elif k == "orig":
                info[name]["orig_w"], info[name]["orig_h"] = int(vals[0]), int(vals[1])
            elif k == "offset":
                info[name]["off_x"], info[name]["off_y"] = int(vals[0]), int(vals[1])
    return {k: v for k, v in info.items() if {"w", "orig_w", "off_x"} <= v.keys()}


def cfloat(v):
    s = f"{v:g}"
    if s == "-0":
        s = "0"
    return s + ("f" if any(ch in s for ch in ".e") else ".0f")


def emit(sk, atlas, out_path):
    """Table for native/modules/cotl_follower_portrait.cpp: parts (deduplicated) and their indices per skin."""
    wm = world(sk)
    parts, index, skins = [], {}, []
    for name in sk.skins:
        if name == "default":
            continue
        ps = portrait_parts(sk, wm, name, atlas)
        if not any(p[0].startswith("HEAD_SKIN") for p in ps):
            continue
        ids = []
        for slot, path, m, _ in ps:
            key = (slot, path, tuple(round(v, 4) for v in m))
            if key not in index:
                index[key] = len(parts)
                parts.append(key)
            ids.append(index[key])
        skins.append((name, ids))
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("// Generated by tools/cotl/spine_portraits.py from Follower.skel/Follower.atlas "
                "(Cult of the Lamb 1.5.27). Do not edit.\n")
        f.write("#ifdef COTL_PORTRAIT_PART\n")
        for slot, path, m in parts:
            f.write(f'COTL_PORTRAIT_PART("{slot}", "{path}", {", ".join(cfloat(v) for v in m)})\n')
        f.write("#endif\n#ifdef COTL_PORTRAIT_SKIN\n")
        for name, ids in skins:
            f.write(f'COTL_PORTRAIT_SKIN("{name}", {", ".join(map(str, ids))})\n')
        f.write("#endif\n")
    print(f"{len(skins)} skins, {len(parts)} parts -> {out_path}")


def main(argv):
    """spine_portraits.py <Follower.skel> <Follower.atlas.txt> (--emit OUT.inc | --dump SKIN ...)"""
    sk = parse(argv[1])
    atlas = atlas_info(argv[2])
    if "--emit" in argv:
        emit(sk, atlas, argv[argv.index("--emit") + 1])
        return
    wm = world(sk)
    for name in argv[argv.index("--dump") + 1:] if "--dump" in argv else []:
        print("==", name)
        for slot, path, m, err in portrait_parts(sk, wm, name, atlas):
            print(f"  {slot:14} {path:32} {[round(v, 3) for v in m]} err {err:.2f}")


if __name__ == "__main__":
    main(sys.argv)
