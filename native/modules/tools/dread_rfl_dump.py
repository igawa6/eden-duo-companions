#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Canonical dump of MEDS' parse of every area's .bmmap/.brfld, in the exact text format
dread_rfl_test.cpp writes, plus the raw asset bytes (<area>.<ext>.meds.raw) from
FileTreeEditor.get_raw_asset. Type-directed via the same construct walk the table generator uses.
Run with a Python that has mercury_engine_data_structures installed:
  python3 dread_rfl_dump.py <out_dir> <extracted_romfs_dir>
(the romfs dir may also be given as DREAD_ROMFS)"""
import enum, os, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import construct
from gen_dread_rfl_types import Gen
from mercury_engine_data_structures.formats import dread_types as T
from mercury_engine_data_structures.crc import crc64
from mercury_engine_data_structures.romfs import ExtractedRomFs
from mercury_engine_data_structures.file_tree_editor import FileTreeEditor
from mercury_engine_data_structures.game_check import Game
from mercury_engine_data_structures import dread_data

AREAS = ["s010_cave", "s020_magma", "s030_baselab", "s040_aqua", "s050_forest",
         "s060_quarantine", "s070_basesanc", "s080_shipyard", "s090_skybase"]
NAME2ID = dread_data.all_name_to_property_id()


def prop(v):
    return v if isinstance(v, int) else NAME2ID.get(v, crc64(v))


def sstr(s):
    b = s.encode("utf-8", "surrogateescape") if isinstance(s, str) else s
    out = bytearray(b'"')
    for c in b:
        if c in (0x22, 0x5C):
            out += bytes([0x5C, c])
        elif c < 0x20:
            out += b"\\u%04x" % c
        else:
            out.append(c)
    return bytes(out + b'"')


def fbits(x):
    return b'"f:%08x"' % struct.unpack("<I", struct.pack("<f", x))[0]


class Dumper:
    def __init__(self, g):
        self.g = g
        self.o = []

    def dump(self, t, v):
        o = self.o
        kind, a, b = self.g.types[t]
        if kind == "Str":
            o.append(sstr(v))
        elif kind == "Bool":
            o.append(b"true" if v else b"false")
        elif kind in ("I32", "U32", "U16", "U64", "Enum"):
            o.append(b"%d" % int(v))
        elif kind == "Prop":
            o.append(b'"p:%016x"' % prop(v))
        elif kind == "F32":
            o.append(fbits(v))
        elif kind in ("Vec2", "Vec3", "Vec4"):
            o.append(b"[" + b",".join(fbits(x) for x in v.raw) + b"]")
        elif kind == "Bytes":
            o.append(b'"b:' + bytes(v).hex().encode() + b'"')
        elif kind == "Object":
            fields = {h: ft for h, ft in self.g.fields[a:a + b]}
            if isinstance(v, construct.ListContainer):
                raise SystemExit("duplicate fields not handled")
            o.append(b"{")
            first = True
            for name, fv in v.items():
                if name.startswith("@") or name.startswith("_"):
                    continue
                h = crc64(name)
                o.append((b"" if first else b",") + b'"%016x":' % h)
                first = False
                self.dump(fields[h], fv)
            o.append(b"}")
        elif kind == "Vector":
            o.append(b"[")
            for i, x in enumerate(v):
                if i:
                    o.append(b",")
                self.dump(a, x)
            o.append(b"]")
        elif kind == "Dict":
            o.append(b'{"@d":[')
            for i, (k, x) in enumerate(v.items()):
                o.append((b"," if i else b"") + b"[")
                self.dump(a, k)
                o.append(b",")
                self.dump(b, x)
                o.append(b"]")
            o.append(b"]}")
        elif kind == "Pointer":
            opts = self.g.opts[a:a + b]
            real = [(h, ot) for h, ot in opts if ot >= 0]
            void = [h for h, ot in opts if ot < 0]
            if v is None:
                o.append(b'{"@p":"%016x","v":null}' % void[0])
                return
            if len(real) == 1 and len(opts) == (2 if void else 1):
                h, ot = real[0]
            else:
                h = prop(v["@type"])
                ot = dict(real)[h]
                if "@value" in v:
                    v = v["@value"]
            o.append(b'{"@p":"%016x","v":' % h)
            self.dump(ot, v)
            o.append(b"}")
        else:
            raise SystemExit(kind)


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: dread_rfl_dump.py <out_dir> [<extracted_romfs_dir>]")
    out = Path(sys.argv[1])
    romfs = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("DREAD_ROMFS")
    if not romfs:
        raise SystemExit("no romfs: pass <extracted_romfs_dir> or set DREAD_ROMFS")
    g = Gen()
    roots = {"bmmap": g.idx(T.CMinimapData), "brfld": g.idx(T.gameeditor_CGameModelRoot)}
    fte = FileTreeEditor(ExtractedRomFs(Path(romfs)), Game.DREAD)
    for area in AREAS:
        base = f"maps/levels/c10_samus/{area}/{area}"
        for ext, path in (("bmmap", base + ".bmmap"), ("brfld", base + ".brfld"),
                          ("bmscc", base + ".bmscc"),
                          ("bcmdl", f"system/minimap/maproom/{area}/{area}.bcmdl")):
            (out / f"{area}.{ext}.meds.raw").write_bytes(fte.get_raw_asset(path))
            if ext in roots:
                d = Dumper(g)
                d.dump(roots[ext], fte.get_parsed_asset(path).raw["Root"])
                (out / f"{area}.{ext}.py.dump").write_bytes(b"".join(d.o) + b"\n")
        print(area, "done")


if __name__ == "__main__":
    main()
