#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 jadesigns.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic game files for so2r_module_test.cpp (no game data): a small UnityFS bundle with a
block-linear BC3 texture, a block-linear Alpha8 font atlas and TextMeshPro-style glyph and
character tables, plus the pictures.idx that points into it.

    python3 native/modules/so2r/so2r_make_fixture.py <out folder>

Writes <out>/Data/StreamingAssets/fixture_bundle and <out>/pictures.idx. Every value is made up
from simple formulas the test recomputes; nothing comes from a game.
"""
import os
import struct
import sys

out = sys.argv[1]
os.makedirs(os.path.join(out, "Data", "StreamingAssets"), exist_ok=True)


def bl_offset(xb, y, width_bytes, gobs):
    gobs_x = (width_bytes + 63) // 64
    bh = 8 * gobs
    in_gob = ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((xb % 32) // 16) * 32 + (y % 2) * 16 + (xb % 16)
    return ((y // bh) * gobs_x + xb // 64) * 512 * gobs + ((y % bh) // 8) * 512 + in_gob


def lz4c(d):
    """Greedy LZ4 block compressor (valid stream; not tuned)."""
    o = bytearray(); tab = {}; i = 0; anchor = 0; n = len(d)

    def length(v):
        while v >= 255: o.append(255); v -= 255
        o.append(v)

    def seq(a, b, off, ml):
        L = b - a
        o.append((min(L, 15) << 4) | (min(ml - 4, 15) if ml else 0))
        if L >= 15: length(L - 15)
        o.extend(d[a:b])
        if ml:
            o.append(off & 255); o.append(off >> 8)
            if ml - 4 >= 15: length(ml - 4 - 15)

    while n >= 13 and i + 4 <= n - 5:
        k = d[i:i + 4]
        j = tab.get(k); tab[k] = i
        if j is not None and i - j < 65536:
            ml = 4
            while i + ml < n - 5 and d[j + ml] == d[i + ml]: ml += 1
            seq(anchor, i, i - j, ml); i += ml; anchor = i
        else:
            i += 1
    seq(anchor, n, 0, 0)
    return bytes(o)


GOBS = 2
# ---- BC3 texture 64x64: each 4x4 block one solid colour from its block coordinates
TW = TH = 64
bxn = TW // 4
bc3_size = bl_offset(0, 0, bxn * 16, GOBS)  # placeholder
rows_pad = -(-(TH // 4) // (8 * GOBS)) * 8 * GOBS
bc3 = bytearray(((bxn * 16 + 63) // 64 * 64) * rows_pad)
for by in range(TH // 4):
    for bx in range(bxn):
        c = ((bx * 2) << 11) | ((by * 4) << 5) | 7
        blk = bytes([200, 200, 0, 0, 0, 0, 0, 0]) + struct.pack("<HH", c, c) + bytes(4)
        o = bl_offset(bx * 16, by, bxn * 16, GOBS)
        bc3[o:o + 16] = blk
# ---- Alpha8 atlas 128x128: value = (x*7 + y*3) & 255
AW = AH = 128
a8 = bytearray(((AW + 63) // 64 * 64) * (-(-AH // (8 * GOBS)) * 8 * GOBS))
for y in range(AH):
    for x in range(AW):
        a8[bl_offset(x, y, AW, GOBS)] = (x * 7 + y * 3) & 255
# ---- font tables: one glyph per codepoint 0x20..0x7E; glyph index = cp + 100
glyphs, chars = b"", b""
cps = list(range(0x20, 0x7F))
for k, cp in enumerate(cps):
    gi = cp + 100
    if cp == 0x20:
        w = h = 0; x = y = 0
    else:
        w, h = 5 + k % 4, 7 + k % 3
        x, y = (k * 9) % 110 + 3, (k * 13) % 100 + 3
    bx, byy, adv = 0.5 + (k % 3), 9.0 + (k % 4), 6.25 + (k % 5)
    glyphs += struct.pack("<I5f4ifii", gi, float(w), float(h), bx, byy, adv, x, y, w, h, 1.0, 0, 0)  # stride 52
    chars += struct.pack("<iIIf", 1, cp, gi, 1.0)
GST, CST = 52, 16
font_obj = b"\x11" * 40 + struct.pack("<i", len(cps)) + glyphs + b"\x22" * 12 + struct.pack("<i", len(cps)) + chars + b"\x33" * 20
g_at = 40 + 4
c_at = g_at + len(glyphs) + 12 + 4
# ---- the data stream: padding, BC3, padding, Alpha8, padding, font object
stream = bytearray(b"\xEE" * 300)
bc3_off = len(stream); stream += bc3
stream += b"\xDD" * 77
a8_off = len(stream); stream += a8
stream += b"\xCC" * 5
font_off = len(stream); stream += font_obj
stream += b"\xBB" * 33
# ---- UnityFS: LZ4 blocks of 1500 bytes; block info LZ4 at the end of the file (flag 0x80)
blocks, comp = [], []
for i in range(0, len(stream), 1500):
    part = bytes(stream[i:i + 1500])
    c = lz4c(part)
    blocks.append((len(part), len(c))); comp.append(c)
info = bytes(16) + struct.pack(">i", len(blocks))
for us, cs in blocks:
    info += struct.pack(">IIH", us, cs, 2)
info += struct.pack(">i", 1) + struct.pack(">qqI", 0, len(stream), 4) + b"CAB-fixture\0"
cinfo = lz4c(info)
hdr = b"UnityFS\0" + struct.pack(">I", 7) + b"5.x.x\0" + b"2021.3.22f1\0"
total_at = len(hdr)
hdr += struct.pack(">qIII", 0, len(cinfo), len(info), 0x80 | 0x40 | 2)
while len(hdr) % 16: hdr += b"\0"
body = hdr + b"".join(comp)
body += cinfo
body = body[:total_at] + struct.pack(">q", len(body)) + body[total_at + 8:]
rel = "Data/StreamingAssets/fixture_bundle"
with open(os.path.join(out, rel), "wb") as f:
    f.write(body)
lines = ["SO2P 1",
         f"B 0 {rel} {len(body)} {len(stream)} {len(blocks)}",
         f"T 0 0 {bc3_off} {len(bc3)} {TW} {TH} 12 {GOBS}",
         f"T 1 0 {a8_off} {len(a8)} {AW} {AH} 1 {GOBS}",
         "S icon_dot_0001 0 8 12 20 16 0",
         "S icon_dot_0002 0 4 4 8 12 4",
         "S camp_icon_money 0 0 0 64 64 0 15 15 15 15",
         "S minimap_mf_0003_21a002 0 0 0 64 32 0",   # map pictures: only their sizes matter here
         "S map_expel 0 0 0 56 44 0",
         f"F fixture_font 0 {font_off} {g_at} {len(cps)} {GST} {c_at} {len(cps)} {CST} 1 36 3168 5200 432 6"]
with open(os.path.join(out, "pictures.idx"), "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print(f"fixture: bundle {len(body)} bytes, stream {len(stream)}, {len(blocks)} blocks")
