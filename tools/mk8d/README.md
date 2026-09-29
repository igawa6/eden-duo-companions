# Mario Kart 8 Deluxe tools

`packages/MarioKart8Deluxe/dualscreen/` is asset-free. `manifest.json` holds our page layout,
colours and the page logic; every piece of game art is a `module:mk8d:...` key or a `romfs:`
reference that the MK8D module in Eden Duo decodes from the player's own game files at runtime
(`mk8d_assets.cpp` under `src/core/mods/modules`: SARC, Yaz0, BNTX, BFFNT and MSBT). The three
per-build data files are small constant tables:

| File | Build |
|------|-------|
| `2C336A9BCF79C304.json` | 4.0.0 (AArch64), supported |
| `6A85262F21B90364.json` | 3.0.3 (AArch32), supported, with or without CTGP-DX |
| `B5C39B5B7C62A88E.json` | 1.0.0 base game without an update: shows the wrong-version screen |
| `data.json` | any other build: shows the wrong-version screen |

The live reader (race state, ranks, items, map positions, course and cup) is the module itself
(`0100152000022000.cpp`, `mk8d_reader.cpp`, `mk8d_ids.cpp` and the code pins in
`mk8d_pins*.inc`).

## Regenerating the package

```sh
python3 tools/mk8d/gen_manifest.py            # rewrites packages/MarioKart8Deluxe/dualscreen/
python3 tools/mk8d/gen_manifest.py <dir>      # or writes the same files into <dir>
```

`gen_manifest.py` builds all eight pages (waiting, loading and race pages, each in a light and a
dark theme; the race pages carry both rank-row formats as animation groups) and the data files.
Running it reproduces the committed files exactly. It needs no game files.

The package requires runtime 13 (`min_runtime` 13, Eden Duo 1.0.1): the theme and row-format
toggles are press-and-hold gestures (`on_hold`).
