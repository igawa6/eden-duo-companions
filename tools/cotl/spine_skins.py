#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 AlterDigga
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads skins from a Spine 3.8 binary file (.skel) and prints the region paths per skin.

  spine_skins.py <datei.skel>

Format per spine-runtimes 3.8 SkeletonBinary; only as far as needed for skins.
"""
import struct
import sys


class R:
    def __init__(self, b):
        self.b, self.p = b, 0

    def byte(self):
        v = self.b[self.p]; self.p += 1; return v

    def boolean(self):
        return self.byte() != 0

    def varint(self):
        r = 0
        for s in range(0, 35, 7):
            b = self.byte(); r |= (b & 0x7F) << s
            if not b & 0x80:
                break
        return r

    def int(self):
        v = struct.unpack_from(">i", self.b, self.p)[0]; self.p += 4; return v

    def float(self):
        v = struct.unpack_from(">f", self.b, self.p)[0]; self.p += 4; return v

    def string(self):
        n = self.varint()
        if n == 0:
            return None
        s = self.b[self.p:self.p + n - 1].decode("utf-8"); self.p += n - 1; return s


def main(path):
    r = R(open(path, "rb").read())
    r.string(); version = r.string()
    assert version.startswith("3.8"), version
    for _ in range(4):
        r.float()
    nonessential = r.boolean()
    if nonessential:
        r.float(); r.string(); r.string()
    strings = [r.string() for _ in range(r.varint())]
    ref = lambda: (lambda i: None if i == 0 else strings[i - 1])(r.varint())
    for i in range(r.varint()):  # Bones
        r.string()
        if i:
            r.varint()
        for _ in range(8):
            r.float()
        r.varint(); r.boolean()
        if nonessential:
            r.int()
    slots = []
    for _ in range(r.varint()):  # Slots
        slots.append(r.string()); r.varint(); r.int(); r.int(); ref(); r.varint()
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
            for _ in range(n * 2):
                r.float()
            return
        for _ in range(n):
            for _ in range(r.varint()):
                r.varint(); r.float(); r.float(); r.float()

    def attachment(key):
        name = ref() or key
        t = r.byte()
        if t == 0:  # region
            p = ref() or name
            for _ in range(7):
                r.float()
            r.int()
            return p
        if t == 1:  # boundingbox
            n = r.varint(); vertices(n)
            if nonessential:
                r.int()
            return None
        if t == 2:  # mesh
            p = ref() or name; r.int(); n = r.varint()
            for _ in range(n * 2):
                r.float()
            for _ in range(r.varint()):
                r.p += 2
            vertices(n); r.varint()
            if nonessential:
                for _ in range(r.varint()):
                    r.p += 2
                r.float(); r.float()
            return p
        if t == 3:  # linkedmesh
            p = ref() or name; r.int(); ref(); ref(); r.boolean()
            if nonessential:
                r.float(); r.float()
            return p
        if t == 4:  # path
            r.boolean(); r.boolean(); n = r.varint(); vertices(n)
            for _ in range(n // 3):
                r.float()
            if nonessential:
                r.int()
            return None
        if t == 5:  # point
            r.float(); r.float(); r.float()
            if nonessential:
                r.int()
            return None
        if t == 6:  # clipping
            r.varint(); n = r.varint(); vertices(n)
            if nonessential:
                r.int()
            return None
        raise SystemExit(f"attachment type {t} not supported (skin reader)")

    def skin(default):
        if default:
            n = r.varint()
            if n == 0:
                return None, []
            name = "default"
        else:
            name = ref()
            for _ in range(4):
                for _ in range(r.varint()):
                    r.varint()
            n = r.varint()
        paths = []
        for _ in range(n):
            slot = slots[r.varint()]
            for _ in range(r.varint()):
                key = ref()
                p = attachment(key)
                if p:
                    paths.append((slot, key, p))
        return name, paths

    out = [skin(True)]
    for _ in range(r.varint()):
        out.append(skin(False))
    for name, paths in out:
        if name:
            print(name + "\t" + "\t".join(f"{s}:{k}={p}" for s, k, p in paths))


if __name__ == "__main__":
    main(sys.argv[1])
