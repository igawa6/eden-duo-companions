import gen_ref as G
from pathlib import Path
M = Path("before/dualscreen/map")
for area in G.AREAS:
    b = G.magnet_blob(area); f = M / f"{area}.magnet.geo"
    print(area, None if b is None else len(b), f.exists(), f.exists() and b == f.read_bytes())
