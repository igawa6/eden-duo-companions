"""Find HP / max HP / level fields in live CPP_UnitParameter objects (development only).
usage: find_unitparam.py <run dir> hp1,hp2,..."""
import struct, sys
from probe_client import Probe
run, want = sys.argv[1], [int(x) for x in sys.argv[2].split(',')]
p = Probe()
rows = [l.rstrip('\n').split('\t') for l in open(run + '/objects.tsv')]
ups = [(int(r[1], 16), r[3]) for r in rows if r[2] in ('CPP_UnitParameter', 'CPP_BtlUnitParameter') and 'Default__' not in r[3]]
print(len(ups), 'unit parameter objects')
for a, path in ups[:40]:
    b = p.read(a, 0x360)
    if not b: continue
    ints = struct.unpack('<%di' % (len(b) // 4), b)
    hits = [i * 4 for i, v in enumerate(ints) if v in want]
    if not hits: continue
    print(hex(a), path[-70:], 'hits', [hex(h) for h in hits])
    for h in hits[:3]:
        lo = max(0, h - 0x30)
        print('   ', ' '.join(f'{ints[k // 4]:d}' for k in range(lo, min(len(b), h + 0x40), 4)))
