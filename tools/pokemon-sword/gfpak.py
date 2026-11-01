# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal GFLXPACK (GameFreak .gfpak) reader: header, path-hash table, folder tables, LZ4/zlib/none members."""
import struct, zlib, lz4.block

def fnv1a64(s: bytes) -> int:
    h = 0xCBF29CE484222645
    for b in s:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h

class GFPak:
    def __init__(self, data: bytes):
        assert data[:8] == b"GFLXPACK", "not a gfpak"
        self.data = data
        ver, reloc, nfiles, nfolders = struct.unpack_from("<IIII", data, 8)
        ftab, hashoff = struct.unpack_from("<QQ", data, 0x18)
        folder_offs = struct.unpack_from("<%dQ" % nfolders, data, 0x28)
        self.path_hashes = list(struct.unpack_from("<%dQ" % nfiles, data, hashoff))
        self.folders = []
        for fo in folder_offs:
            fh, cnt, _ = struct.unpack_from("<QII", data, fo)
            ents = []
            for i in range(cnt):
                nh, idx, _ = struct.unpack_from("<QII", data, fo + 0x10 + i * 0x10)
                ents.append((nh, idx))
            self.folders.append((fh, ents))
        self.files = []
        for i in range(nfiles):
            level, comp, dsz, csz, _, off = struct.unpack_from("<HHIIIQ", data, ftab + i * 0x18)
            self.files.append((comp, dsz, csz, off))

    def member(self, i: int) -> bytes:
        comp, dsz, csz, off = self.files[i]
        raw = self.data[off:off + csz]
        if comp == 0: return raw
        if comp == 1: return zlib.decompress(raw)
        if comp == 2: return lz4.block.decompress(raw, uncompressed_size=dsz)
        raise NotImplementedError("compression %d (Oodle?)" % comp)

    def index_of_path(self, path: str):
        h = fnv1a64(path.encode())
        return self.path_hashes.index(h) if h in self.path_hashes else None

    def index_of_name(self, name: str):
        h = fnv1a64(name.encode())
        for fh, ents in self.folders:
            for nh, idx in ents:
                if nh == h: return idx
        return None
