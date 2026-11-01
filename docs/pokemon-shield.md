# Pokémon Shield companion

Pokémon Shield's companion is the [Pokémon Sword companion](pokemon-sword.md) built for Shield:
the same pages, battle view, direct commands, field actions, languages and settings. Everything
on that page applies; this page lists only what differs.

This companion is in development.

## Supported game

- Pokémon Shield, title `01008DB008C2C000`, update **1.3.2** (main build `A16802625E7826BF`).
- Isle of Armor and Crown Tundra data are supported when discovered in the current save.
- Eden Duo runtime **22** or later, module ABI **1**. Korean and Chinese companion text also
  needs runtime **23** and the console's own shared system fonts.
- Other game builds keep commands disabled and show the update requirement.

## What is Shield's own

- **Wild Pokémon:** the Town Map route list and grid show Shield's encounter tables
  (`encount_t`, `encount_symbol_t`), so the version-exclusive species are Shield's.
- **Pokédex entries:** the Shield entry text (`zukan_comment_B.dat`, the file the Shield game
  itself reads).
- **Title page:** the Shield title logo from the player's own game.
- Everything else (names, items, moves, places, fonts) is read from the player's romfs exactly
  as on Sword.

## Build

The module is the same source tree with `SW_VARIANT_SHIELD=1`: CMake target
`dsmod-pokemon-shield` (library `01008DB008C2C000.so`), package `packages/PokemonShield`. Pass
`--variant shield` to the tools (`gen_manifest.py`, `gen_route_encounters.py`, `build_helper_plan.py`,
`make_package.py`, `text_stress.py`, `check_no_game_text.py`). Set `SW_ROMFS_DIR` to the extracted
Shield romfs when generating Shield's manifest and route tables. The package art (`img/`) is the
Sword bake; `make_package.py` refuses a Shield package whose `img/` differs.

`ctest -R pokemon-shield` runs the Shield twins of the variant, route, signature and no-game-text
tests. Set `SHIELD_NSO` to the dumped Shield 1.3.2 `exefs/main` and `SHIELD_ROMFS_DIR` to its
romfs for the last two.
