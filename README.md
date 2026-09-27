<h1 align="center">Eden Duo Companions</h1>

<p align="center">
  Second-screen companions for <a href="https://github.com/igawa6/eden-duo">Eden Duo</a>, one installable package per game.
</p>

---

A companion turns the second screen of a dual-screen Android handheld into a live, touchable panel for the game you are playing: maps, menus, status and more, read from the running game. Each companion ships as a `.dsmod.zip` package and is installed from the game's **Add-ons** menu in Eden Duo.

This repository holds the companion packages, their documentation and screenshots. The emulator itself lives in [Eden Duo](https://github.com/igawa6/eden-duo).

## Supported Games

| No | Game | Title ID | Patch Version | Companion | Requires | Download |
|---:|------|----------|---------------|-----------|----------|----------|
| 1 | [Persona 5 Royal](#persona-5-royal) | `01005CA01580E000` | 1.0.2 | 1.0.0 | Eden Duo 1.0.0 | [.dsmod.zip](https://github.com/igawa6/eden-duo-companions/releases/tag/persona5royal-1.0.0) |
| 2 | [Metroid Dread](#metroid-dread) | `010093801237C000` | 2.1.0 | 1.0.0 | Eden Duo 1.0.0 | [.dsmod.zip](https://github.com/igawa6/eden-duo-companions/releases/tag/metroid-dread-1.0.0) |
| 3 | [The Legend of Zelda: Link's Awakening](#the-legend-of-zelda-links-awakening) | `01006BB00C6F0000` | 1.0.1 | 1.0.0 | Eden Duo 1.0.0 | [.dsmod.zip](https://github.com/igawa6/eden-duo-companions/releases/tag/links-awakening-1.0.0) |

Each companion supports one exact game version, the one in **Patch Version**. It checks the running build before it loads. On any other version it does not load and shows a notice, instead of reading memory it does not understand.

## Install

1. Install [Eden Duo](https://github.com/igawa6/eden-duo/releases), at least the version in **Requires**.
2. Update the game to the version in **Patch Version**.
3. Download the game's `.dsmod.zip` from the table above. Do not rename it; the file name carries the title ID, name and version.
4. In Eden Duo, long-press the game, open **Add-ons** and tap **Install**. In the **Content type** dialog choose **Dual screen mods**, tap **OK**, then select the file. It appears in the Add-ons list as, for example, `Persona5RoyalDS-1.0.0`.
5. Launch the game. The companion appears on the second screen once gameplay starts.

![Installing a companion: Add-ons, Install, Dual screen mods](screenshots/eden-duo/addons_dualscreen.png)

Installing a newer version of a companion replaces the older one.

---

## Persona 5 Royal

`01005CA01580E000` · game version 1.0.2 · companion 1.0.0

<!-- screenshots/persona5royal/*.png -->
| Field | Menu | Battle |
|:-----:|:----:|:------:|
| ![Field](screenshots/persona5royal/field.png) | ![Menu](screenshots/persona5royal/menu.png) | ![Battle](screenshots/persona5royal/battle.png) |

A full bottom-screen version of the game's own start menu, with live data:

- **Skill, Item, Equip, Persona, Stats, Confidant, Request, Calendar.** Laid out like the native camp menu.
  - Confidants show the character, arcana, rank and rank abilities.
  - Stats include baton pass and down shot.
- **Use items, change equipment and change Persona from the touch screen.** Changes apply instantly by writing the same values the game's own menu writes. Items and skills the game would refuse are greyed out. Anything that is not reproduced exactly, or any request made while the camp menu is already open, is carried out by driving the native menu with button presses.
- **Calendar.** Browse months with L/R and tap any day for its plans. Past days show the game's Daily Log.
- **Battle.** Choose TACTICAL (enemy grid, selected enemy's details and affinities as in Analyze, party status) or FOLLOW, a lighter view that follows the action.
- **Field.** Area map with your position, date, time of day and money.
- **Dialogue.** Only choices are shown, as large buttons; tapping one selects it.
- **Now playing.** The current background music with its title.

The companion ships no game art: menus, fonts and icons are drawn from your own game files when the page first opens.

## Metroid Dread

`010093801237C000` · game version 2.1.0 · companion 1.0.0

<!-- screenshots/metroid-dread/*.png -->
| Map | EMMI zone | Water drain |
|:---:|:---------:|:-----------:|
| ![Map](screenshots/metroid-dread/map.png) | ![EMMI zone](screenshots/metroid-dread/emmi.png) | ![Water](screenshots/metroid-dread/water.png) |

A live area map on the second screen, drawn the way the game draws its own:

- **Area map** with Samus's position, rooms revealed as you explore, doors, items and your custom markers.
- **EMMI zones**: grey while the EMMI is active and green once it has been destroyed, read from the game's own state.
- **Water** as the game's map shows it, including the level moving while a pool drains or fills.
- **Status**: energy and tanks, missiles, power bombs and item collection percentage.
- **Clean top screen**: hide the game's own HUD and minimap while the companion shows the map.

The companion ships no game art: map geometry and icons are built from your own game files.

## The Legend of Zelda: Link's Awakening

`01006BB00C6F0000` · game version 1.0.1 · companion 1.0.0

<!-- screenshots/links-awakening/*.png -->
| Map | Gear | Items |
|:---:|:----:|:-----:|
| ![Map](screenshots/links-awakening/map.png) | ![Gear](screenshots/links-awakening/gear.png) | ![Items](screenshots/links-awakening/items.png) |

- **Map** of the overworld and dungeons with Link's live position, region names, rupees, seashells and what is on X, Y and B. Dungeon maps show rooms, chests, stairs and the boss room, and switch automatically when you enter a dungeon. Place, change and remove your own map pins.
- **Gear**: the items you can assign, such as magic powder, bombs, arrows, hookshot, rod, boomerang and bottles, with their counts. Equip to X or Y by dragging an item onto a slot, or by tapping the item and then the slot.
- **Items**: sword, shield, tunic, bracelet, boots, flippers and other equipment, the eight instruments, ocarina songs, trading item, heart pieces, secret seashells and secret stones.

The companion ships no game art: it uses your own game files.

---

## Building Packages

| Path | Contents |
|------|----------|
| [`packages/`](packages/) | Package sources, one folder per game: `dualscreen/manifest.json`, the per-build address table `<BUILDID>.json` and, for Persona 5 Royal, the art recipe table `p5r_art.rec`. |
| [`tools/`](tools/) | `build_release.sh` builds the three `.dsmod.zip` archives. The page generators for Persona 5 Royal ([`tools/p5r/`](tools/p5r/)) and Metroid Dread ([`tools/dread/`](tools/dread/)) are here too. |
| [`docs/`](docs/) | How the companion runtime works, the package format, writing a native module and porting a new game. |
| [`screenshots/`](screenshots/) | Captures of both screens for each game. |

The native modules for Persona 5 Royal and Metroid Dread are C++ and live in the Eden Duo repository under [`src/core/mods/modules`](https://github.com/igawa6/eden-duo/tree/main/src/core/mods/modules). Build them there, strip them, and pass them to the release script:

```sh
P5R_LINUX_SO=... P5R_ANDROID_SO=... DREAD_LINUX_SO=... DREAD_ANDROID_SO=... tools/build_release.sh dist
```

See [`tools/README.md`](tools/README.md) for the details. Link's Awakening needs no native module.

## Reporting Problems

Open an issue and include:

- the game, its version and its title ID;
- the companion version (in **Add-ons**);
- the Eden Duo version;
- the device.

A screenshot of both screens helps.

## Game Assets

Companion packages contain **no game assets**: no art, text, audio or level data. Everything game-specific that a companion shows is read at runtime from the player's own game files and from the running game. Do not upload game files, keys, firmware or ROMs to this repository.

## AI Assistance

These companions were developed with AI assistance. The companion modules, the reverse engineering of each game's data and the package generators were written with an AI coding assistant (Claude, by Anthropic), then reviewed, tested and verified against each game's own screens.

## License

Companion packages and tools are free software, released under the [GNU General Public License v3.0](LICENSE). They are not affiliated with or endorsed by Nintendo, Atlus, SEGA, or the Eden project. All trademarks belong to their respective owners.
