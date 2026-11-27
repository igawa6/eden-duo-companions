# Pokémon Legends: Arceus companion

Title `01001F5010DFA000`, game update 1.1.1
(build `AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E`).
Companion 1.0.0 for Eden Duo 1.4.1 (runtime 25) or newer; any other build shows a notice and
reads nothing. Companion text is shown in English, whatever language the game uses.

The bottom screen shows a live area map with the throw dock, the party, the Pokédex, the
Satchel and Missions, and turns into a battle view during turn battles. See
[docs/pokemon-legends-arceus.md](../../docs/pokemon-legends-arceus.md) for the full list.

## This companion changes your game

Most of the companion only reads the game, but two features change it while it runs:

- **Tap to select.** Tapping a Poké Ball, item or Pokémon in the throw dock makes the game
  select it. To do this, Eden Duo installs a small hook in the game's code
  (`dualscreen/guest/<build id>/load-plan.json`) that runs the game's own selector code. The
  hook is installed when the game starts with the companion and stays in place until the game
  closes. It only acts after a tap; otherwise the game's code runs as normal.
- **Hidden throw selector.** While the Map tab is shown, the companion clears one visibility
  flag so the game's own throw selector on the top screen is hidden (the dock replaces it).
  It is put back on every other tab, when the bottom screen goes away and when the companion
  closes. Settings "Show throw selector on top" keeps it visible.

Neither feature changes your save file. "Show throw selector on top" in Settings stops the
flag change; the hook stays installed but does nothing unless you tap the dock.

The package ships no game art. Map art, portraits, icons, the font and all text are read from
the player's own game files at runtime.

## Layout

| Path | What it is |
|---|---|
| `dualscreen/manifest.json` | Generated pages, widgets, actions and settings (`tools/pokemon-legends-arceus/gen_manifest.py`). |
| `dualscreen/guest/<build id>/load-plan.json` | Build-pinned tap-select hook and the patched call site. |
| `package.json` | Package identity, version and minimum runtime. |
