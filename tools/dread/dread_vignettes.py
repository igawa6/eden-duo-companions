#!/usr/bin/env python3
"""Bake mapVignetteGeos into the manifest: per-vignette concealment quads that black out whole
hidden rooms until dispelled (blackboard dict OCCLUDER_VIGNETTES[name] -> true). Also tags every
manifest icon that a vignette suppresses (mapVignetteOccludedIcons name lists, resolved to world
positions) with "v": <vignette name> so the renderer hides it while the vignette is active.
Emits per area: "vignettes": [{"n": name, "t": [[x0,y0,x1,y1,x2,y2], ...]}]. Idempotent."""
import os
import json
from pathlib import Path
from mercury_engine_data_structures.romfs import ExtractedRomFs
from mercury_engine_data_structures.file_tree_editor import FileTreeEditor
from mercury_engine_data_structures.game_check import Game
from mercury_engine_data_structures.formats.bmmap import Bmmap

ROMFS = Path(os.environ.get("DREAD_ROMFS", "romfs"))  # extracted romfs (contains packs/, system/, textures/)
MANIFEST = Path(os.environ.get("DREAD_LEGACY_PKG", "legacy/dualscreen")) / "manifest.json"  # an asset-carrying (pre-1.0.0) package dir; never the release package
AREAS = ["s010_cave","s020_magma","s030_baselab","s040_aqua","s050_forest",
         "s060_quarantine","s070_basesanc","s080_shipyard","s090_skybase"]

def geo_tris(g):
    tris=[]
    vs=g.get("aVertex") or []; idx=g.get("aIndex") or []
    for k in range(0,len(idx)-2,3):
        a,b,c=idx[k],idx[k+1],idx[k+2]
        if max(a,b,c)>=len(vs): continue
        tris.append([round(float(vs[a][0]),1),round(float(vs[a][1]),1),
                     round(float(vs[b][0]),1),round(float(vs[b][1]),1),
                     round(float(vs[c][0]),1),round(float(vs[c][1]),1)])
    return tris

def main():
    fte=FileTreeEditor(ExtractedRomFs(ROMFS),Game.DREAD)
    man=json.load(open(MANIFEST))
    for area in AREAS:
        if area not in man["map"]["areas"]: continue
        bm=fte.get_parsed_asset(f"maps/levels/c10_samus/{area}/{area}.bmmap",type_hint=Bmmap)
        root=bm.raw.Root
        vg=root.get("mapVignetteGeos") or {}
        out=[]
        for name,g in vg.items():
            # value may be one SGeoData or a container of them
            tris=[]
            if hasattr(g,"get") and g.get("aVertex") is not None:
                tris=geo_tris(g)
            elif hasattr(g,"items"):
                for _,gg in g.items(): tris+=geo_tris(gg)
            if tris: out.append({"n":str(name),"t":tris})
        man["map"]["areas"][area]["vignettes"]=out
        # resolve occluded icon names -> world positions
        occicons=root.get("mapVignetteOccludedIcons") or {}
        pos_lookup={}
        for cat in ("mapDoors","mapItems","mapUsables","mapProps","mapBlockages","mapCentralUnits","mapBosses"):
            for k,v in (root.get(cat) or {}).items():
                p=v.get("vPos") if hasattr(v,"get") else None
                if p is not None: pos_lookup[str(k)]=(float(p[0]),float(p[1]))
        tagged=0; unresolved=[]
        icons=man["map"]["areas"][area]["icons"]
        for it in icons: it.pop("v",None)  # idempotent
        for vname,namelist in (occicons.items() if hasattr(occicons,"items") else []):
            for ent in (namelist or []):
                nm=str(ent).split(" (")[0]  # "Door046 (PW-CL,OP)" -> "Door046"
                p=pos_lookup.get(nm) or pos_lookup.get(str(ent))
                if p is None:
                    unresolved.append(str(ent)); continue
                for it in icons:
                    if abs(it["x"]-p[0])<=200 and abs(it["y"]-p[1])<=200:
                        it["v"]=str(vname); tagged+=1
        print(f"{area}: {len(out)} vignettes, {tagged} icons tagged"+ (f", unresolved {unresolved}" if unresolved else ""))
    json.dump(man,open(MANIFEST,"w"),indent=1)
    print("manifest updated (vignettes)")

if __name__=="__main__": main()
