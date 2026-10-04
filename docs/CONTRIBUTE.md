# Contribute to Another Game

Eden Duo is not limited to the games in the [Supported Games](../README.md#supported-games)
table. Anyone can write a companion for another game and share it as a `.dsmod.zip` package.
You do not change or rebuild the emulator to do this.

This page is the starting point. It explains what a companion is, what the runtime gives you,
which tools help, and in which order to read the other documents. It is current for **Eden Duo
1.1.0, runtime 18**.

## What a companion is

A **companion** fills the second screen with a live, touchable page for the game on the main
screen: a map, a menu, a status panel. It reads the running game's memory and the player's own
game files.

A companion ships as a **package**, `<TITLEID>.dsmod.zip`, which holds:

- `package.json`: the archive's identity (title ID, name, version, `min_runtime`);
- `dualscreen/manifest.json`: the **manifest**, a JSON description of the pages, widgets, values
  and actions;
- an optional per-build **data file**, `<BUILD16>.json`, that says where values live in memory
  for one exact game build;
- an optional **native module**, a small shared library (`.so`) written against a stable C ABI,
  for data that JSON cannot describe.

Players install the package from the game's **Add-ons** menu (**Install**, then **Dual screen
mods**). The emulator's **runtime** discovers it, checks the game build, and draws the pages.

A package ships **no game assets**. Art, fonts and text are decoded at runtime from the
player's own game files.

## What the runtime gives you

| Layer | What you get |
|---|---|
| Package (JSON) | Pages built from **9 widget types**: `rect`, `label`, `value`, `bar`, `button`, `pips`, `image`, `map` and `chart`. Widgets support bindings, derived values, flags, page transitions, animations, scroll lists, repeat templates, taps, press-and-hold and swipe gestures with haptic feedback, drag and drop, pan and zoom, coloured words inside text, and outlined game-font text |
| Actions | **10 action kinds**: `write`, `slot_write`, `button` (including two-button chords such as `L+R`), `page`, `flag`, `view_reset`, `module`, `map_select`, `call` and `sequence` |
| Memory points | Pointer chains, static fields, pattern scans and per-build address tables, all checked against the game's build ID before loading |
| Game files | Art, fonts and text read from the player's own files: `romfs:` (the updated game), `base:` (the unpatched game), `aoc:` (installed DLC) and `user:` (files the player adds) |
| Native module (C ABI) | Bounded memory reads and writes, the player's romfs, and publishing of values and map frames. Five optional extensions add actions and module images, font decoding, save-file reads, atomic write batches, and module-generated data |
| Compatibility | `min_runtime` in the package makes an older Eden Duo show an "update" page instead of failing |

**New in runtimes 16–18.** These are the features you are most likely to miss in older
examples:

- **Images** (16): `rotate`, `scale_bind` and `pivot`; `tint` and `tint_colors`; `fill` as
  stretch, tile or 9-slice.
- **Bars** (16): `fill_dir` (fill from any side) and a picture for the filled part.
- **Clock values** (16): `@clock.hour` … `@clock.epoch`, `@game.seconds`, and the `countdown`
  derived form.
- **Refused actions** (16): a module can refuse an action; the page then plays the `refused`
  haptic.
- **Chart widget** (17): a line or bar history of one value.
- **`expr` derived values** (17): arithmetic and logic such as `"hp * 100 / max(hp_max, 1)"`.
- **Text layout** (17): `auto_w` sizes a label or button to its text; `group` adds thousands
  separators; `{i}` works in every field of a repeat template.
- **Paged font atlases** (17): large fonts, such as CJK fonts, load one page at a time.
- **Controller navigation** (17): a chord (default `LS+RS`) lets the player move a focus frame
  over the page with the D-pad and press A to tap.
- **`user:` source** (17): files the player supplies, such as a portrait or a translation.
- **Settings page** (17): a `settings` list in the manifest becomes a built-in `@settings` page,
  and its choices are saved between sessions.

- **Text fitting** (18): `fit_text` reduces a single-line label to `text_min_scale` before ellipsis; `text_center_h` centers its lines vertically.
- **Game-art scrollbars and map markers** (18): `bar_src` / `bar_track_src` replace scrollbar colours with images; `size_max` caps dynamic marker size in canvas pixels.
- **Font refresh** (18): a native module can publish `__font_epoch` when its glyph set changes, including a language discovered after initialization. The host retries decoding and refreshes the atlas.

The full key reference, with the runtime that added each key, is in
[PACKAGE_FORMAT.md](PACKAGE_FORMAT.md). Declare the highest runtime you use as `min_runtime`
([PACKAGE_FORMAT.md §6](PACKAGE_FORMAT.md#6-runtime-history-and-min_runtime)).

## Tools for reverse engineering

These live in the desktop development build of Eden Duo. Details are in
[PORTING_A_GAME.md](PORTING_A_GAME.md).

- **Headless desktop build.** `eden-cli` runs a game with its companion and no window
  (`--aux-virtual`), or with a real second window (`--aux-window`). It captures both screens
  (`--screenshot-prefix`) and takes scripted button presses, stick moves and taps.
- **Live memory console.** Build with `EDEN_DSMOD_BUILD_DEV_TOOLS=ON` and set
  `EDEN_DSMOD_CMD=<file>`. Commands include:
  - value search and change tracking: `findi`, `findf`, `sfind`, `snap`, `diff`;
  - pointer search: `ptrto`;
  - memory: `watch`, `hexdump`, `readi`, `writeb`;
  - object and manager finders: `objfind`, `mgrfind`;
  - companion control: `value`, `tap`, `drag`, `page`, `action`, `reload`;
  - image dumps: `imgdump`.
- **GDB stub** for breakpoints and memory watches on the running game.
- **Guest function calls** on the desktop CPU backend (Dynarmic), for research. Set
  `EDEN_DSMOD_NO_GUEST_BRIDGE=1` to test under the same conditions as an Android handheld, where
  guest calls are not available.
- **Profiling.** Per-stage timings are logged every 5 seconds by default (`EDEN_DSMOD_PROFILE=0`
  turns them off; `EDEN_DSMOD_PROFILE_WIDGETS=1` adds per-widget draw times). Use them to keep a
  companion cheap on handheld hardware.

## Your path

Read in this order:

1. **[PORTING_A_GAME.md](PORTING_A_GAME.md).** The step-by-step method: dump the game, find its
   state, validate against the game's own screens, build the package, test headless, deploy. It
   ends with a list of common problems.
2. **[PACKAGE_FORMAT.md](PACKAGE_FORMAT.md).** The reference for `package.json`, the manifest and
   the data file. Look things up here while you write the manifest.
3. **[MODULE_GUIDE.md](MODULE_GUIDE.md).** Only if you need a native module: the C ABI, the
   extensions, how to build, and worked examples.
4. **[ARCHITECTURE.md](ARCHITECTURE.md).** How the runtime works inside: the tick, the redraw
   worker, threads, performance. Read it when something behaves unexpectedly or is slow.
5. **[GLOSSARY.md](GLOSSARY.md).** Short definitions of the terms used everywhere.

Then study the five published packages as working examples, in
[`packages/`](../packages/):

| Package | Shows |
|---|---|
| Link's Awakening | A declarative package with no native module |
| Persona 5 Royal | A large native module, direct writes, art recipes |
| Metroid Dread | A map generated by the module from the player's romfs |
| Mario Kart 8 Deluxe | Two game versions with one data file per build, press-and-hold toggles, a guarded write batch |
| Super Mario Bros. Wonder | Outlined game-font numbers and art decoded from romfs |

The native module sources are in this companions repository under
[`native/modules/`](https://github.com/igawa6/eden-duo-companions/tree/main/native/modules).
The page generators for the published packages are in [`tools/`](../tools/README.md).

## Share your companion

When your companion works:

1. Build the package with `tools/build_dualscreen_package.py` (see
   [PORTING_A_GAME.md §4](PORTING_A_GAME.md#4-build-the-package)).
2. Make sure it contains **no game assets**: no art, text, audio, level data, keys or firmware
   ([Game Assets](../README.md#game-assets)).
3. Open an issue or a pull request on this repository. Name the game, its title ID and the game
   version you support, and the Eden Duo version you tested with. A screenshot of both screens
   helps.

## AI assistance is welcome

Building a companion is mostly careful reverse engineering and repetitive verification. AI
coding assistants are good at both. Give your assistant these docs and an existing package as a
template. Let it drive the headless desktop build, the memory console and the screenshots. Then
check every value it finds against the game's own screens before you trust it. Companions made
with AI help are welcome here.
