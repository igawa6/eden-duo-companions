# Super Mario Bros. Wonder tools

`packages/SuperMarioWonder/dualscreen/` is asset-free. `manifest.json` holds our page layout,
colours and page logic; every piece of game art is a `module:wonder:...` key that the Wonder
module in this repository decodes from the player's own game files at runtime (`wonder_assets.cpp`,
`wonder_font.cpp` and `wonder_catalog.cpp` under `native/modules`: zstd, SARC, BNTX, the
game font, BYML and MSBT). `wonder_font.txt` names the game font the module rasterises. The
per-build data files are small constant tables:

| File | Build |
|------|-------|
| `FF773E90972D544E.json` | 1.2.1, supported |
| `CD6E42AEE7934F4D.json` | 1.0.0 base game without an update: shows the wrong-version screen |
| `F91868B88F60D3D5.json` | 1.0.1: shows the wrong-version screen |
| `data.json` | any other build: shows the wrong-version screen |

The live reader (course, area progress, coins, seeds, power-up and item balloon) is the module
itself (`010015100B514000.cpp`).

## Regenerating the package

```sh
python3 tools/wonder/gen_manifest.py            # rewrites packages/SuperMarioWonder/dualscreen/
python3 tools/wonder/gen_manifest.py <dir>      # or writes the same files into <dir>
```

`gen_manifest.py` builds the three pages (title, course and world map) and the data files.
Running it reproduces the committed files exactly. It needs no game files. The package needs
Eden Duo 1.0.2 (runtime 15) for the outlined game-font text.
