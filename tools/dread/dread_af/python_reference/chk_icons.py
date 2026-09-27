import gen_ref as G, json
man = json.load(open("before/dualscreen/manifest.json"))
known = set(man["map"]["icons"])
for area in G.AREAS:
    got = G.norm(G.icons(area, known)); want = G.norm(man["map"]["areas"][area]["icons"])
    d = G.first_diff(got, want)
    print(area, "OK" if not d else d)
    if d and "len" not in d:
        idx = int(d.split("]")[0][1:]); print("   got ", got[idx]); print("   want", want[idx])
