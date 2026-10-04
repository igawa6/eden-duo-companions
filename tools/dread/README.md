# Metroid Dread tools

`packages/MetroidDread/dualscreen/manifest.json` is asset-free. It holds our page layout, the map
style, and area templates whose geometry is `module:dread:...`. The textures and the font are
`romfs:` references. Everything derived from level data is built at game load by the Dread module
in this repository (`dread_mapgen.cpp`, `dread_maproom.cpp`, `dread_rfl.cpp`, `dread_romfs.cpp` under
`native/modules`). That covers the room geometry, icons, doors, water pools, occluders,
camera rects and so on. `646761F643AFEBB3.json` is the per-build address table.

## Regenerating the manifest

```sh
python3 tools/dread/dread_hud_page.py            # rewrites packages/MetroidDread/dualscreen/manifest.json
python3 tools/dread/dread_hud_page.py <dir>      # or a copy in <dir>/manifest.json
```

`dread_hud_page.py` builds the single bottom-screen page: the status header, the map, the pause
menu mirror, and the loading and version-mismatch screens. It also sets the map style. On the
asset-free manifest it then binds labels that are game strings to `loc:<KEY>`, using
`dread_af/dread_af_manifest.py`. Running it on the committed manifest reproduces that manifest
exactly. It needs no game files.

## Python reference for the module's map generator

`dread_af/` holds the one-off conversion from the old asset-carrying package and the Python
reference implementation that the C++ `dread_mapgen` was checked against. See
[`dread_af/README.md`](dread_af/README.md). The reference imports the bakers in this directory:

| Baker | Derives |
|-------|---------|
| `dread_map_extract.py` | room geometry from `<area>.bmmap` |
| `dread_magnet.py` | spider-magnet surfaces |
| `dread_add_doors.py` | door icons and boxes |
| `dread_occluders.py` | occluder boxes |
| `dread_vignettes.py` | vignette tags |
| `dread_room_classes.py` | room categories from the maproom models |
| `dread_overview.py` | overview regions |
| `dread_water_table.py` | water pools and water level changes |
| `bctex_decode.py` | BCTEX texture decoder (deswizzle, BC4/R8/RGBA8) |

These scripts read your extracted romfs (`DREAD_ROMFS`). Run on their own, they write
game-derived data into a manifest. That is the **old** package layout, so they write only to the
package named by `DREAD_LEGACY_PKG` (default `./legacy/dualscreen`), never to
`packages/MetroidDread`. Never commit their output.

Requirements: Pillow; mercury-engine-data-structures for everything that reads romfs.
