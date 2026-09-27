#!/usr/bin/env python3
"""Trim an Atlus .FNT to its first N glyphs, keeping the exact format (same header, palette,
Huffman dictionary and bit stream; only counts/tables shortened). usage: trim_fnt.py in out [N]"""
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 320
d = bytearray(open(src, 'rb').read())
hsz = struct.unpack_from('<I', d, 0)[0]
count, w, h, bsz = struct.unpack_from('<4H', d, 14)
assert N <= count
bpp = bsz * 8 // (w * h)
p = hsz + 4 * (1 << bpp)
head = bytearray(d[:p])
wsz = struct.unpack_from('<I', d, p)[0]; cuts = d[p + 4:p + 4 + wsz]; p += 4 + wsz
usz = struct.unpack_from('<I', d, p)[0]; unk = d[p + 4:p + 4 + usz]; p += 4 + usz
p += 4 * count
ch = list(struct.unpack_from('<iiiiHHiii', d, p)); chsz, dsz, csz = ch[0], ch[1], ch[2]; gtc = ch[6]
chdr_extra = d[p + 32:p + chsz]; p += chsz
dic = d[p:p + dsz]; p += dsz
pos = struct.unpack_from('<%di' % gtc, d, p); p += 4 * gtc
comp = d[p:p + csz]
nbytes = (pos[N] + 7) // 8
out = bytearray(head)
struct.pack_into('<H', out, 14, N)
out += struct.pack('<I', 2 * N) + cuts[:2 * N]
out += struct.pack('<I', len(unk)) + unk
out += b'\0' * (4 * N)
ch[2] = nbytes; ch[6] = N + 1; ch[8] = N * bsz
out += struct.pack('<iiiiHHiii', *ch) + chdr_extra + dic
out += struct.pack('<%di' % (N + 1), *pos[:N + 1]) + comp[:nbytes]
struct.pack_into('<I', out, 4, len(out))
open(dst, 'wb').write(out)
print(f'{src}: {count} -> {N} glyphs, {len(d)} -> {len(out)} bytes')
