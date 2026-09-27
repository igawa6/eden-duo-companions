#!/usr/bin/env python3
"""Extract Dread's magnetic-surface polylines (mapMagnetSurfaces) into a thin blue map layer.
Each surface is an open polyline; expand every segment into a thin quad and emit <area>.magnet.geo
in the packed blob format, then add a magnet layer (#FF1085E9) to the manifest. Idempotent: the
previous magnet layer entry and blob of every area are replaced (or removed), never appended.

Line width is derived per area from the runtime's raster, not a fixed world-unit constant:
mod_ui.cpp rasterises an area into an image whose longest side is RASTER_LONG px (aspect-correct
over the manifest bounds; other side truncated, min 16), mod_runtime.cpp maps u16 -> px as
floor(v*(side-1)/65535) and Canvas::FillTriangle fills with an INCLUSIVE edge test and drops
area==0 triangles. So the quad is LINE_PX raster px wide. LINE_PX sits a hair above 1.0: after u16
quantisation each edge jitters by up to 1535/65535 px, and a 1.0-px quad whose two long edges land in
the same pixel column is area==0 -> dropped -> the wall vanishes. Note that with integer vertices and
inclusive edges the thinnest non-degenerate axis-aligned quad still paints 2 px (columns c and c+1);
1 px would need a runtime change (top-left fill rule or a line rasteriser). Diagonal segments get
the half-width scaled by 1/max(|cos|,|sin|) so their cross-axis vertex offset stays >= LINE_PX px."""
import os
import json, math, struct
from pathlib import Path
from mercury_engine_data_structures.romfs import ExtractedRomFs
from mercury_engine_data_structures.file_tree_editor import FileTreeEditor
from mercury_engine_data_structures.game_check import Game
from mercury_engine_data_structures.formats.bmmap import Bmmap

ROMFS = Path(os.environ.get("DREAD_ROMFS", "romfs"))  # extracted romfs (contains packs/, system/, textures/)
PKG = Path(os.environ.get("DREAD_LEGACY_PKG", "legacy/dualscreen"))  # an asset-carrying (pre-1.0.0) package dir; never the release package
AREAS=["s010_cave","s020_magma","s030_baselab","s040_aqua","s050_forest",
       "s060_quarantine","s070_basesanc","s080_shipyard","s090_skybase"]
COLOR="#FF1085E9"
RASTER_LONG=3072  # manifest map.style.raster_px: longest side of the cached area raster
LINE_PX=1.05      # quad width in raster px: 1 px + margin > 2*(1535/65535) so no edge pair collapses
HALF_MIN=0.5      # world-unit floor on the half-width (never a zero-width quad)

def raster_size(sx,sy):
    """Same rule as mod_ui.cpp: longest side RASTER_LONG, the other truncated, min 16."""
    if sx>=sy: return RASTER_LONG, max(16,int(RASTER_LONG*sy/sx))
    return max(16,int(RASTER_LONG*sx/sy)), RASTER_LONG

def half_width(sx,sy):
    """Half-width in world units for a LINE_PX-wide quad in this area's raster."""
    iw,ih=raster_size(sx,sy)
    upp=max(sx/(iw-1),sy/(ih-1))  # world units per raster px; u16 65535 lands on px side-1
    return max(HALF_MIN,0.5*LINE_PX*upp),upp,(iw,ih)

def write_geo(path, verts, idx):
    with open(path,"wb") as f:
        f.write(struct.pack("<II",len(verts),len(idx)))
        for x,y in verts: f.write(struct.pack("<HH",x,y))
        for i in idx: f.write(struct.pack("<I",i))

def main():
    fte=FileTreeEditor(ExtractedRomFs(ROMFS),Game.DREAD)
    man=json.load(open(PKG/"manifest.json"))
    (PKG/"map").mkdir(exist_ok=True)
    for area in AREAS:
        bm=fte.get_parsed_asset(f"maps/levels/c10_samus/{area}/{area}.bmmap",type_hint=Bmmap)
        root=bm.raw.Root if hasattr(bm.raw,"Root") else bm.raw
        ms=getattr(root,"mapMagnetSurfaces",None)
        a=man["map"]["areas"][area]; mnx,mny=a["min"]; mxx,mxy=a["max"]
        sx=(mxx-mnx) or 1.0; sy=(mxy-mny) or 1.0
        half,upp,(iw,ih)=half_width(sx,sy)
        nv=lambda x,y:(int(max(0,min(65535,(x-mnx)/sx*65535))),int(max(0,min(65535,(y-mny)/sy*65535))))
        verts,idx=[],[]
        items = ms.items() if hasattr(ms,"items") else (ms or {}).items() if ms else []
        for _,sd in items:
            pts=[(float(s.vPos[0]),float(s.vPos[1])) for s in sd.oPolyLine.oSegmentData]
            for i in range(len(pts)-1):
                (x0,y0),(x1,y1)=pts[i],pts[i+1]; dx,dy=x1-x0,y1-y0
                L=math.hypot(dx,dy) or 1.0
                # Diagonal guard: keep the cross-axis vertex offset >= LINE_PX px (x1.414 at 45 deg,
                # x1 for axis-aligned) so no vertex pair floors into one pixel -- that drops a
                # triangle (area==0) and draws the wall as a tapered wedge.
                h=half/(max(abs(dx),abs(dy))/L or 1.0); px,py=-dy/L*h,dx/L*h
                b=len(verts)
                for qx,qy in [(x0+px,y0+py),(x1+px,y1+py),(x1-px,y1-py),(x0-px,y0-py)]:
                    verts.append(nv(qx,qy))
                idx += [b,b+1,b+2, b,b+2,b+3]
        # replace any existing magnet layer entry / blob (idempotent)
        a["layers"]=[L for L in a.get("layers",[]) if "magnet" not in L.get("geo","")]
        blob=PKG/"map"/f"{area}.magnet.geo"
        if idx:
            write_geo(blob,verts,idx)
            a["layers"].append({"geo":f"file:map/{area}.magnet.geo","kind":"magnet",
                                 "color":COLOR})
        else:
            blob.unlink(missing_ok=True)
        print(f"{area}: {len(idx)//6} magnet segments; raster {iw}x{ih}, {upp:.3f} units/px, "
              f"half-width {half:.2f} units ({2*half/upp:.3f} px wide)")
    json.dump(man,open(PKG/"manifest.json","w"),indent=1)
    print("manifest updated with magnet layers")

if __name__=="__main__": main()
