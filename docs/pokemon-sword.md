# Pokémon Sword companion

The companion puts the Town Map, party reference, Pokédex, Bag and League Card on the second
screen. Battles switch to a separate touch view with move effectiveness and direct commands.

This companion is in development.

## Supported game

- Pokémon Sword, title `0100ABF008968000`, update **1.3.2**.
- Isle of Armor and Crown Tundra data are supported when discovered in the current save.
- Eden Duo runtime **22** or later, module ABI **1**. Korean and Chinese companion text also
  needs runtime **23** and the console's own shared system fonts; without them those two
  languages are not offered.
- Other game builds keep commands disabled and show the update requirement.

## Field screens

- **Town Map:** the game's regional map, current zone marker, weather, Flying Taxi destinations
  unlocked by the game, Bike control and party HP. Drag and zoom the map to look around.
  The map stays full size behind the tools. Its focus moves between the visible left area and
  the full centre when the slim tools bar is expanded or collapsed.
  Pinch to zoom. Tap a named town or route to select it, then tap **Fly**. When a location
  has several available landing points, choose one from the filtered list.
- **Pokémon:** tap a party row for stats, nature, ability, moves and type matchups. The
  Weaknesses tab pairs each named weakness, resistance and immunity with its type icon; these describe types and
  exclude changes from abilities or held items. Tap a move for its native description.
- **Pokédex:** regional entry lists, known species details and known evolution steps. Long
  evolution families scroll horizontally; undiscovered stages are omitted.
- **Bag:** pocket and item selection, larger descriptions and one **Use** button for supported
  items. Medicine opens a party picker over the Bag.
  Supported medicine includes HP restoration, status cures, Revives, Ether/Elixir and PP Up/Max.
  Items that affect one move open a move picker after selecting the party member.
  Rare Candy, EXP Candy and evolution stones use the game's native effects. Finish any
  move-learning or evolution prompts on the top screen before using another item.
- **League Card:** the real saved trainer card, including its 3D player picture, loads
  automatically while the player has control in the field. Money, Watts, BP, badges,
  regional caught counts and companion settings appear alongside it. The picture is
  cached for the session and refreshed when the saved card changes.

Selections on these screens belong to the companion. Opening and closing the game's menus
keeps the selected companion screen.

The map marker follows the game's named zone, as on its Town Map. Raid den information comes
from the last save and is labelled on screen.

## Battles

Both battlers show current and maximum HP. White sprite pills show remaining HP over a black
depleted track, without percentage labels. The selected companion command has a black pill
and selection arrow; cancelling its native menu retains that selection.

The left pane has **Foe**, **Own** and **Field** tabs. Field shows weather, terrain and active
conditions, including their remaining turns when the game supplies a duration. **Fight** opens
four move buttons and a Move/Foe/Own/Field detail pane. Hold a move for its native description,
category, power and PP; tap a move to commit it. Move pills retain native proportions and
spacing, and status moves leave the effectiveness line blank. **Bag** and **Pokémon** open the game's native menus on the top screen. The companion
keeps matchup information visible, shows where to choose items and offers type matchup
suggestions while choosing a Pokémon. Suggestions exclude abilities, items, stats and field effects.
The rectangular **X / Poké Ball** shortcut opens a bottom-screen ball selector. Browse with
arrows or a swipe, read the description, then tap **Use** to throw. Browsing and Back do not
spend a turn. **Y / Info** opens native battle information. Commands use the game's native paths.

Offline Max Raids show the boss matchups, your HP and three allied Pokémon. Open **Fight**,
then tap the native **Dynamax** emblem to preview Max moves. Move names change to the native preview while PP stays
attached to the original move. The display also shows your remaining Dynamax turns.

Commands are limited to supported battle formats. An unavailable command stays
locked; tapping it gives refusal feedback. Native HP plates stay visible by default.
Turning **Battle HP on top** off hides them while a companion battle page is shown;
hiding the companion restores the native plates.

## Settings

Open settings from the League Card.

- **Language:** English, French, German, Italian, Spanish, Japanese (kanji or kana), and Korean,
  Simplified or Traditional Chinese where the system fonts are available. Every word the
  companion takes from the game (names, descriptions, type and pocket names, battle commands, stat
  names, place names, game messages) is read from your own game files in that language; the
  package carries only the companion's own text.
- **Reduced motion:** removes page transitions and the short readout fades.
- **Battle HP on top:** on by default. Turning it off hides the native HP plates while a
  companion battle page is shown; hiding the companion restores them.
- **Battle menu on top:** off by default, so the battle command and move menus are hidden on the
  top screen while the companion battle page is the choice surface. Turn it on to keep them.
- **Hide battle detail pane:** the collapsed battle layout, kept for the next battle. The corner
  arrow or a swipe also switches it.

Touch feedback follows the device's system setting.

## Current limits

- An emulation-exit crash has been observed during testing; its cause remains unresolved.
- Some item categories need additional native flows before they can be offered.
- Doubles, institutional battles and online formats keep companion commands locked.
- Raid commands cover Fight and Dynamax against the boss or yourself. Use the game's controls
  for other targets, cheering, items or exiting. Single-battle Dynamax commands remain locked.
- Y-Comm does not have an independent companion interface.

## Build

Build native modules from `native/modules` with `EDEN_SOURCE_ROOT` pointing to the matching
Eden Duo source tree and `BUILD_TESTING=ON`. Run `ctest -R pokemon-sword` in that build directory.

`tools/pokemon-sword/gen_manifest.py` owns the manifest. It measures the game's own words for the
layout, so it needs `SW_ROMFS_DIR` set to your extracted romfs (the manifest stores no game text:
`strings.json` names those lines and the module reads them at run time, see `sw_strings.py`). With
`SW_ROMFS_DIR` set, `ctest` also proves that the runtime text equals the earlier baked tables and
that the manifest, the module sources and the built module contain no game text
(`check_no_game_text.py`). To build a package from Linux and
Android arm64 modules, run `tools/pokemon-sword/make_package.py` with `--nso`, `--linux-so`,
`--android-so`, `--android-strip` and `--output`. The helper builder verifies the supported
executable's patch bytes before emitting its load plan. The package builder strips modules
and records their hashes.

The repository holds no game files and no save data: the reader tests run on synthetic PK8s,
bag/Pokédex blocks and den slots (`native/modules/sw_test_fixtures.h`). The same checks against
real bytes are `dsmod-pokemon-sword-reader-private` and `dsmod-pokemon-sword-reader-live-private`:
they read the game's files from `SW_ROMFS_DIR` and the blocks of a save you dumped yourself from
`SW_FIXTURES_DIR` (binary files named in the tests, e.g. `k_item.bin`), and are skipped without
both.

Everything title-specific (title id, supported build and its main offsets, romfs version suffix,
Pokédex entry file, package identity) is one variant record in `tools/pokemon-sword/sw_variants.py`;
`gen_variant_header.py` writes the module's copy, `native/modules/sw_variant_data.h`. The tools take
`--variant` (`sword`, the default, or `shield`: see [pokemon-shield.md](pokemon-shield.md)). Each
main offset carries a byte signature: `tools/pokemon-sword/sw_sigscan.py check --nso <main>` proves
they resolve to the verified offsets, and `scan --nso <other main>` resolves the whole table for
another build without guessing. Set `SWORD_NSO` to the dumped 1.3.2 `exefs/main` for `ctest` to run
that check too.
