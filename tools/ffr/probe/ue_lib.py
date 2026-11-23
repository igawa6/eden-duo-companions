"""Helpers over a ue_discover.py run folder (names.bin, objects.tsv, ue_roots.json) plus a live probe."""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from probe_client import Probe  # noqa: E402
from ue_discover import Names  # noqa: E402


class Run:
    def __init__(self, d, live=True):
        self.d = d
        self.roots = json.load(open(os.path.join(d, 'ue_roots.json')))
        blocks, b = [], open(os.path.join(d, 'names.bin'), 'rb').read()
        o = 0
        while o < len(b):
            n = struct.unpack_from('<I', b, o)[0]; o += 4
            blocks.append(b[o:o + n]); o += n
        self.names = Names(blocks)
        self.by_addr, self.by_path, self.rows = {}, {}, []
        for line in open(os.path.join(d, 'objects.tsv')):
            i, a, cls, path = line.rstrip('\n').split('\t')
            r = (int(i), int(a, 16), cls, path)
            self.rows.append(r); self.by_addr[r[1]] = r; self.by_path[path] = r
        self.p = Probe() if live else None

    def fname(self, b, o=0):
        c, n = struct.unpack_from('<II', b, o)
        return self.names.name(c, n)

    def of_class(self, cls):
        return [r for r in self.rows if r[2] == cls]
