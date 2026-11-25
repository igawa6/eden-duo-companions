# FINAL FANTASY RESONANCE tools

The companion's module is `native/modules/0100606026F12000.cpp` with `ffr_*.cpp`. It draws every page itself in
the game's own look, and the manifest only shows that picture and places the tap areas. Research notes, with every
offset and how it was found, are in [NOTES.md](NOTES.md).

| File | What it does |
|---|---|
| `gen_manifest.py` | Writes `packages/FinalFantasyResonance/dualscreen/manifest.json`. Its tap rectangles follow the layout constants in `ffr_pages.h`, so run it after changing those. Standard library only. |
| `build_android.sh <eden-duo checkout> <out dir> [version]` | A quick Android arm64 build and package without CMake, using the NDK under `ANDROID_NDK` or the Android SDK. Releases use `dsmod-ffr` and `tools/build_release.sh` (see [the tools README](../README.md)). |
| `build_helper_plan.py --nso <exefs main> --main-bin <flat image of main>` | Assembles the guest helper (fast travel, items, equipment, quest tracking, menu sounds; `native/helper/ffr_guest_helper.S`) with the NDK's clang and writes `packages/FinalFantasyResonance/dualscreen/guest/<build>/load-plan.json`. It refuses another build: it checks main's build id, the hooked instruction and the first instruction of each game function the helper calls. Any flat image of main (file offset equal to the offset from main's base) serves. |
| `dev/render_tool.cpp` | Renders every page to raw RGBA files from sample state and your own extracted RomFS, with no device. |
| `dev/assets_tool.cpp` | Checks the asset layer (IoStore, pak, textures, fonts, localization) against your own extracted RomFS. |

All of these read only your own game files or the running game. Nothing they read goes into the package.
