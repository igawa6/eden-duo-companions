# FINAL FANTASY RESONANCE tools

The companion's module is `native/modules/0100606026F12000.cpp` with `ffr_*.cpp`. It draws every page itself in
the game's own look, and the manifest only shows that picture and places the tap areas. Research notes, with every
offset and how it was found, are in [NOTES.md](NOTES.md).

| File | What it does |
|---|---|
| `gen_manifest.py` | Writes `packages/FinalFantasyResonance/dualscreen/manifest.json`. Its tap rectangles follow the layout constants in `ffr_pages.h`, so run it after changing those. Standard library only. |
| `build_android.sh <eden-duo checkout> <out dir> [version] [--dev]` | A quick Android arm64 build and package without CMake, using the NDK under `ANDROID_NDK` or the Android SDK. `--dev` adds a loopback memory server for `dev/live_tool.cpp` and the probe scripts. Never publish a `--dev` build. Releases use `dsmod-ffr` and `tools/build_release.sh` (see [the tools README](../README.md)). |
| `dev/render_tool.cpp` | Renders every page to raw RGBA files from sample state and your own extracted RomFS, with no device. |
| `dev/assets_tool.cpp` | Checks the asset layer (IoStore, pak, textures, fonts, localization) against your own extracted RomFS. |
| `dev/live_tool.cpp` | Runs the module's reader on the desktop against a `--dev` build on the device (`adb forward tcp:47123 tcp:47123`). |
| `probe/` | The research probe: a separate module that serves guest memory reads over the same loopback port, and the scripts that found Unreal Engine's name pool and object array and dumped the game's reflection. Not shipped. |

All of these read only your own game files or the running game. Nothing they read goes into the package.
