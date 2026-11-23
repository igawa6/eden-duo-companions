"""Find Unreal Engine 5.6's FNamePool and FUObjectArray in the running FINAL FANTASY RESONANCE
through ffr_probe, then dump names and every UObject (development only).

usage: ue_discover.py <out dir>
Writes <out>/ue_roots.json, <out>/names.bin (the pool's blocks) and <out>/objects.tsv.
"""
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from probe_client import Probe

DATA_OFF, DATA_SIZE, BSS_SIZE = 0x67d6000, 0xfed9f8, 0x2390608


def find_name_pool(seg, base, p):
    """FNamePool: ... u32 CurrentBlock, u32 CurrentByteCursor, FNameEntryAllocator Blocks[8192]. Blocks[0..CurrentBlock] are heap
    pointers, the rest zero. Its first entry is "None"."""
    q = struct.unpack_from(f'<{len(seg) // 8}Q', seg)
    hits = []
    for i in range(1, len(q) - 4):
        cur_block, cursor = q[i - 1] & 0xffffffff, q[i - 1] >> 32
        if not 1 <= cur_block < 8192 or not 0 < cursor <= 0x40000:
            continue
        run = q[i:i + cur_block + 1]
        if len(run) != cur_block + 1 or not all(run) or q[i + cur_block + 1] != 0:
            continue
        if any(abs(v - run[0]) > (1 << 36) for v in run):
            continue
        head = p.read(run[0], 64)
        if head and b'None' in head[:8]:
            hits.append((base + 8 * i, cur_block, cursor, head))
    return hits


def find_object_array(seg, base):
    """FChunkedFixedUObjectArray: Objects**, PreAllocated*, i32 Max, i32 Num, i32 MaxChunks, i32 NumChunks"""
    hits = []
    for o in range(0, len(seg) - 32, 8):
        objs, pre, mx, num, mxc, nc = struct.unpack_from('<QQiiii', seg, o)
        if not objs or mx < 0x10000 or not 1000 < num <= mx:
            continue
        if mxc != (mx + 0xffff) // 0x10000 or nc != (num + 0xffff) // 0x10000:
            continue
        hits.append((base + o, objs, pre, mx, num, mxc, nc))
    return hits


class Names:
    def __init__(self, blocks):
        self.blocks = blocks  # list of bytes

    def entry(self, idx):
        b, off = idx >> 16, (idx & 0xffff) * 2
        if b >= len(self.blocks) or off + 2 > len(self.blocks[b]):
            return None
        blk = self.blocks[b]
        h = struct.unpack_from('<H', blk, off)[0]
        wide, ln = h & 1, h >> 6
        if wide:
            return blk[off + 2:off + 2 + 2 * ln].decode('utf-16-le', 'replace')
        return blk[off + 2:off + 2 + ln].decode('latin1')

    def name(self, cmp, num=0):
        s = self.entry(cmp)
        if s is None:
            return f'?{cmp:#x}'
        return s if not num else f'{s}_{num - 1}'


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    p = Probe()
    info = p.info()
    print({k: hex(v) for k, v in info.items()})
    mb = info['main_base']
    t0 = time.time()
    seg = p.read_chunked(mb + DATA_OFF, DATA_SIZE + BSS_SIZE)
    print(f'read data+bss {len(seg) >> 20} MiB in {time.time() - t0:.1f}s')

    pools = find_name_pool(seg, mb + DATA_OFF, p)
    for a, cb, cur, head in pools:
        print(f'name pool candidate: blocks at main+{a - mb:#x}, current block {cb}, cursor {cur:#x}, head {head[:24]!r}')
    if not pools:
        sys.exit('no name pool')
    blocks_at, cur_block, cursor, _ = pools[0]
    ptrs = struct.unpack_from(f'<{cur_block + 1}Q', seg, blocks_at - mb - DATA_OFF)
    # a block holds 65536 slots of 2 bytes (FNameBlockOffsetBits 16, stride 2); the current block is filled to the cursor
    blocks = [p.read(b, 0x20000 if i < cur_block else cursor) or bytes(0x20000 if i < cur_block else cursor) for i, b in enumerate(ptrs)]
    names = Names(blocks)
    print('names 0..3:', [names.entry(i) for i in (0, 2, 4)])

    arrs = find_object_array(seg, mb + DATA_OFF)
    for h in arrs:
        print(f'object array candidate: main+{h[0] - mb:#x} objects {h[1]:#x} max {h[3]} num {h[4]}')
    if not arrs:
        sys.exit('no object array')
    oa, objs, _, mx, num, _, nc = arrs[0]
    chunks = struct.unpack(f'<{nc}Q', p.read(objs, 8 * nc))
    # FUObjectItem stride: the first item points at object 0, whose InternalIndex (+0xc) is 0, and the next item's object has index 1
    c0 = p.read(chunks[0], 0x40 * 4)
    stride = None
    for s in (0x10, 0x18, 0x20, 0x28):
        o0, o1 = struct.unpack_from('<Q', c0, 0)[0], struct.unpack_from('<Q', c0, s)[0]
        if p.i32(o0 + 0xc) == 0 and p.i32(o1 + 0xc) == 1:
            stride = s
            break
    print('item stride', hex(stride) if stride else None)
    roots = dict(main_base=mb, name_pool_blocks=blocks_at - mb, name_pool_current_block=cur_block,
                 object_array=oa - mb, object_count=num, item_stride=stride)
    json.dump(roots, open(os.path.join(out, 'ue_roots.json'), 'w'), indent=1)
    with open(os.path.join(out, 'names.bin'), 'wb') as f:
        for b in blocks:
            f.write(struct.pack('<I', len(b)) + b)
    if not stride:
        sys.exit('unknown item stride')

    # objects: address, then the header (vtable, flags, index, class, name, outer)
    addrs = []
    for c in range(nc):
        n = min(0x10000, num - c * 0x10000)
        raw = p.read_chunked(chunks[c], n * stride)
        addrs += [struct.unpack_from('<Q', raw, i * stride)[0] for i in range(n)]
    hdr = {}
    t0 = time.time()
    live = [a for a in addrs if a]
    for a, b in zip(live, p.read_many([(a, 0x28) for a in live])):
        if b:
            hdr[a] = struct.unpack('<QiiQIIQ', b)
    print(f'  headers {time.time() - t0:.0f}s')

    def path(a, depth=0):
        h = hdr.get(a)
        if not h or depth > 16:
            return f'?{a:#x}'
        nm = names.name(h[4], h[5])
        return path(h[6], depth + 1) + '.' + nm if h[6] else nm

    with open(os.path.join(out, 'objects.tsv'), 'w') as f:
        for i, a in enumerate(addrs):
            h = hdr.get(a)
            if not h:
                continue
            cls = hdr.get(h[3])
            f.write(f'{i}\t{a:#x}\t{names.name(cls[4], cls[5]) if cls else "?"}\t{path(a)}\n')
    print('done', len(hdr), 'objects')


if __name__ == '__main__':
    main()
