#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate dread_rfl_types.inc: the Mercury "standard format" type layouts the Dread module parses
(CMinimapData for .bmmap, gameeditor::CGameModelRoot for .brfld), read out of
mercury_engine_data_structures' own constructs (formats/dread_types.py) so the C++ reader parses
exactly what MEDS parses. Only the closure reachable from the two roots is emitted; any construct
class this script does not know is an error.

Run with a Python that has MEDS installed:  python3 gen_dread_rfl_types.py <out.inc>
"""
import sys
import construct
from mercury_engine_data_structures.formats import dread_types as T
from mercury_engine_data_structures.object import Object
from mercury_engine_data_structures.pointer_set import PointerAdapter
from mercury_engine_data_structures.common_types import DictConstruct, CVectorConstruct
from mercury_engine_data_structures.construct_extensions.enum import StrictEnum
from mercury_engine_data_structures.construct_extensions.strings import StringEncodedRobust
from mercury_engine_data_structures.formats.property_enum import CRCAdapter
from mercury_engine_data_structures.crc import crc64

KINDS = ["Str", "Bool", "I32", "U32", "F32", "U16", "U64", "Prop", "Vec2", "Vec3", "Vec4",
         "Bytes", "Enum", "Object", "Vector", "Dict", "Pointer"]
FMT = {"<l": "I32", "<L": "U32", "<i": "I32", "<I": "U32", "<f": "F32", "<H": "U16", "<Q": "U64"}


class Gen:
    def __init__(self):
        self.types = []      # (kind, a, b)
        self.fields = []     # (hash, type)
        self.opts = []       # (typeid, type or -1)
        self.memo = {}       # id(construct) -> index
        self.prim = {}       # kind name -> index

    def prim_index(self, kind):
        if kind not in self.prim:
            self.prim[kind] = len(self.types)
            self.types.append([kind, 0, 0])
        return self.prim[kind]

    def unwrap(self, c):
        while isinstance(c, construct.Renamed):
            c = c.subcon
        return c

    def idx(self, c):
        c = self.unwrap(c)
        key = id(c)
        if key in self.memo:
            return self.memo[key]
        if isinstance(c, StringEncodedRobust):
            return self.prim_index("Str")
        if isinstance(c, construct.core.Flag.__class__) or c is construct.Flag:
            return self.prim_index("Bool")
        if isinstance(c, construct.FormatField):
            if c.fmtstr not in FMT:
                raise SystemExit(f"unknown FormatField {c.fmtstr}")
            return self.prim_index(FMT[c.fmtstr])
        if isinstance(c, CRCAdapter):
            return self.prim_index("Prop")
        if isinstance(c, CVectorConstruct):
            return self.prim_index(f"Vec{c._length}")
        if isinstance(c, StrictEnum):
            return self.prim_index("Enum")
        if isinstance(c, construct.Prefixed) and isinstance(self.unwrap(c.subcon), construct.GreedyBytes.__class__):
            return self.prim_index("Bytes")
        if isinstance(c, Object):
            i = len(self.types)
            self.memo[key] = i
            self.types.append(["Object", 0, 0])
            items = list(c.fields.items())
            sub = [(crc64(name), self.idx(v)) for name, v in items]
            hashes = [h for h, _ in sub]
            if len(set(hashes)) != len(hashes):
                raise SystemExit("hash collision in object")
            sub.sort()
            self.types[i][1] = len(self.fields)
            self.types[i][2] = len(sub)
            self.fields.extend(sub)
            return i
        if isinstance(c, PointerAdapter):
            i = len(self.types)
            self.memo[key] = i
            self.types.append(["Pointer", 0, 0])
            sub = []
            for tid, v in c.types.items():
                v = self.unwrap(v)
                sub.append((tid, -1 if v is construct.Pass else self.idx(v)))
            sub.sort()
            self.types[i][1] = len(self.opts)
            self.types[i][2] = len(sub)
            self.opts.extend(sub)
            return i
        if isinstance(c, DictConstruct):
            if self.unwrap(c.count_type).fmtstr not in ("<I", "<L"):
                raise SystemExit("dict count type")
            i = len(self.types)
            self.memo[key] = i
            self.types.append(["Dict", 0, 0])
            self.types[i][1] = self.idx(c.key_type)
            self.types[i][2] = self.idx(c.value_type)
            return i
        if isinstance(c, construct.FocusedSeq):
            # common_types.make_vector: "count"/Rebuild(Int32ul) then Array(this.count, value)
            subs = [self.unwrap(s) for s in c.subcons]
            if (c.parsebuildfrom != "items" or len(subs) != 2 or not isinstance(subs[0], construct.Rebuild)
                    or self.unwrap(subs[0].subcon).fmtstr not in ("<I", "<L") or not isinstance(subs[1], construct.Array)):
                raise SystemExit(f"unknown FocusedSeq {c}")
            i = len(self.types)
            self.memo[key] = i
            self.types.append(["Vector", 0, 0])
            self.types[i][1] = self.idx(subs[1].subcon)
            return i
        raise SystemExit(f"unhandled construct {type(c)} {c}")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "dread_rfl_types.inc"
    g = Gen()
    r0 = g.idx(T.CMinimapData)
    r1 = g.idx(T.gameeditor_CGameModelRoot)
    with open(out, "w") as f:
        f.write("// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork\n")
        f.write("// SPDX-License-Identifier: GPL-3.0-or-later\n")
        f.write("// GENERATED by tools/gen_dread_rfl_types.py from mercury_engine_data_structures'\n")
        f.write("// formats/dread_types.py -- do not edit. Layout knowledge only (name CRC64s + kinds).\n")
        f.write(f"// {len(g.types)} types, {len(g.fields)} fields, {len(g.opts)} pointer options.\n\n")
        f.write(f"constexpr uint32_t kRootMinimapData = {r0};\n")
        f.write(f"constexpr uint32_t kRootGameModelRoot = {r1};\n\n")
        f.write("constexpr TypeDesc kTypes[] = {\n")
        for k, a, b in g.types:
            f.write(f"    {{TypeKind::{k}, {a}u, {b}u}},\n")
        f.write("};\n\nconstexpr FieldDesc kFields[] = {\n")
        for h, t in g.fields:
            f.write(f"    {{0x{h:016X}ull, {t}u}},\n")
        f.write("};\n\nconstexpr PtrDesc kPtrOpts[] = {\n")
        for h, t in g.opts:
            f.write(f"    {{0x{h:016X}ull, {'kVoid' if t < 0 else str(t) + 'u'}}},\n")
        f.write("};\n")
    print(f"{len(g.types)} types, {len(g.fields)} fields, {len(g.opts)} ptr options -> {out}")


if __name__ == "__main__":
    main()
