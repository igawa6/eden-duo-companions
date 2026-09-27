# Link's Awakening package

`packages/LinksAwakening/dualscreen/` (`manifest.json` and the per-build address table
`909E904AF78AC1B8.json`) is maintained directly in this repository. Edit those files and rebuild
the archive with `tools/build_release.sh`.

The generator that first wrote them is not published. It is a private layout spec that also holds
research notes, and those cannot be separated from it cleanly. The JSON files are the source of truth.
They contain only our own layout values, memory addresses and flag numbers. Every piece of game
art is a `romfs:` / `msbt:` reference that Eden Duo resolves from the player's own game files at
runtime.

The package needs no native module.
