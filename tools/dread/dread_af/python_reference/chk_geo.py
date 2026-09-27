import sys, gen_ref as G
from pathlib import Path
M = Path("before/dualscreen/map")
for area in G.AREAS:
    blobs = G.geo_blobs(area)
    for suf, b in blobs.items():
        f = M / f"{area}{suf}.geo"
        print(area, suf or "base", "shipped" if f.exists() else "NOT-SHIPPED", len(b), (f.read_bytes() == b) if f.exists() else "")
    for f in sorted(M.glob(f"{area}.*.geo")):
        suf = f.name[len(area):-4]
        if suf not in blobs and suf != ".magnet":
            print(area, "shipped but not generated:", f.name)
    rc = G.room_categories_polys(area)
    import json
    man = json.load(open("before/dualscreen/manifest.json"))
    for c in man["map"]["areas"][area]["room_categories"]:
        d = G.first_diff(G.norm(rc[c["id"]]), G.norm(c.get("polys", [])))
        print(area, "room_cat", c["id"], "OK" if not d else d)
