# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tiny schema-less FlatBuffers reader (little-endian)."""
import struct
class T:
    def __init__(self, buf, pos):
        self.b = buf; self.pos = pos
        vt = pos - struct.unpack_from("<i", buf, pos)[0]
        self.vt = vt
        self.vtsize, self.objsize = struct.unpack_from("<HH", buf, vt)
        self.nf = (self.vtsize - 4) // 2
    def off(self, i):
        if i >= self.nf: return 0
        return struct.unpack_from("<H", self.b, self.vt + 4 + 2 * i)[0]
    def scalar(self, i, fmt, default=0):
        o = self.off(i)
        return struct.unpack_from("<" + fmt, self.b, self.pos + o)[0] if o else default
    def table(self, i):
        o = self.off(i)
        if not o: return None
        p = self.pos + o
        return T(self.b, p + struct.unpack_from("<I", self.b, p)[0])
    def vec_pos(self, i):
        o = self.off(i)
        if not o: return None, 0
        p = self.pos + o; p += struct.unpack_from("<I", self.b, p)[0]
        n = struct.unpack_from("<I", self.b, p)[0]
        return p + 4, n
    def tables(self, i):
        p, n = self.vec_pos(i)
        out = []
        for k in range(n):
            q = p + 4 * k
            out.append(T(self.b, q + struct.unpack_from("<I", self.b, q)[0]))
        return out
    def string(self, i):
        p, n = self.vec_pos(i)
        return None if p is None else self.b[p:p + n].decode("utf-8", "replace")
    def layout(self):
        return [(i, self.off(i)) for i in range(self.nf)]
def root(buf):
    return T(buf, struct.unpack_from("<I", buf, 0)[0])
