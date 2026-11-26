# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shape of a libc++ std::map (red-black tree) after inserting n keys in ascending order.
Returns, for each in-order rank, the left/right path from the root (0 = left, 1 = right)."""


def rb_paths(n):
    NIL = None

    class N:
        __slots__ = ("k", "l", "r", "p", "red")

        def __init__(s, k):
            s.k, s.l, s.r, s.p, s.red = k, None, None, None, True

    root = None

    def rot_left(x):
        nonlocal root
        y = x.r
        x.r = y.l
        if y.l: y.l.p = x
        y.p = x.p
        if not x.p: root = y
        elif x is x.p.l: x.p.l = y
        else: x.p.r = y
        y.l = x; x.p = y

    def rot_right(x):
        nonlocal root
        y = x.l
        x.l = y.r
        if y.r: y.r.p = x
        y.p = x.p
        if not x.p: root = y
        elif x is x.p.r: x.p.r = y
        else: x.p.l = y
        y.r = x; x.p = y

    for k in range(n):
        z = N(k)
        y, x = None, root
        while x:
            y = x
            x = x.l if k < x.k else x.r
        z.p = y
        if not y: root = z
        elif k < y.k: y.l = z
        else: y.r = z
        while z.p and z.p.red:
            g = z.p.p
            if z.p is g.l:
                u = g.r
                if u and u.red:
                    z.p.red = False; u.red = False; g.red = True; z = g
                else:
                    if z is z.p.r:
                        z = z.p; rot_left(z)
                    z.p.red = False; z.p.p.red = True; rot_right(z.p.p)
            else:
                u = g.l
                if u and u.red:
                    z.p.red = False; u.red = False; g.red = True; z = g
                else:
                    if z is z.p.l:
                        z = z.p; rot_right(z)
                    z.p.red = False; z.p.p.red = True; rot_left(z.p.p)
        root.red = False
    paths = {}

    def walk(nd, path):
        if not nd: return
        paths[nd.k] = path
        walk(nd.l, path + [0]); walk(nd.r, path + [1])
    walk(root, [])
    return [paths[k] for k in range(n)]


if __name__ == "__main__":
    p = rb_paths(454)
    print(max(len(x) for x in p), p[0], p[453], p[226])
