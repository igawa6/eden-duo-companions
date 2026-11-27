# Pokémon Legends: Arceus companion

The companion puts a live area map, the throw dock, the party, the Pokédex, the Satchel and
Missions on the second screen. Turn battles switch the Map tab to a battle view with both
Pokémon, moves, effectiveness and action order.

## Supported game

- Pokémon Legends: Arceus, title `01001F5010DFA000`, update **1.1.1**
  (build `AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E`).
- Eden Duo runtime **25** or later, module ABI **1**.
- Other game builds show a page asking for update 1.1.1; nothing is read or written.
- Companion text, names and descriptions are shown in English, whatever language the game uses.

## Screens

- **Map:** the game's own area map with the player marker and heading. The map follows the
  player; pinch to zoom and drag to look around, and the recentre button glides back to the
  player at the default zoom. With a game menu open the map shows the whole area.
  Below the map, the throw dock lists the Poké Balls and items in the throw selector and the
  six party plates. Tap one to select it in the game.
- **Pokémon:** the party with HP and level, and a detail card with Stats (effort levels),
  Moves (power, accuracy, PP, style effects) and Weak (type matchups).
- **Pokédex:** Seen and Caught counts per area, the list with research levels, and Entry
  (description, height, weight), Research tasks and Weak. Order by number or research level,
  or show only species with unreported research.
- **Satchel:** Everyday Items, Key Items and Recipes, with how many of each recipe can be
  crafted from what the Satchel holds.
- **Missions:** Missions and Requests, the tracked one first, with an Active only filter.
- **Battle:** replaces the Map during a turn battle: your Pokémon and the opponent, HP, status,
  moves with PP and effectiveness (tap a move to inspect it), the action order and the party.
  When your Pokémon faints it asks for the next one. The Map returns when the battle ends.
- **Settings:** Follow player by default, Reduced motion, Haptics and Show throw selector on
  top. The game's throw selector on the top screen is hidden by default because the dock
  shows it (only while the Map tab is shown); the setting brings it back.

The title screen shows the game logo, loading shows a dimmed emblem, and the game menus keep
the whole-area map with tap selection off.

## Reading the game

All reads check the full build ID and title first, bound every pointer hop and read
twice, keeping the last good copy when the two reads disagree.

- **Position and heading:** the active player object; the heading comes from the change in
  position, so it shows the direction of travel.
- **Area, Pokédex, Satchel, Missions, Recipes:** the game's save blocks (encrypted SCBlock
  entries, decoded with the SCXorShift32 scheme also described by PKHeX/pkNX).
- **Party and throw selector:** the in-use party buffers and the throw selector state, so
  values change as soon as the game shows them.
- **Battle:** the battle roster (species, form, level, HP, moves, PP, status) and the action
  order, shown only while the field HUD is off.

The companion changes the game in two ways (see the package README): it clears the throw
selector's visibility flag while the Map tab is shown, putting it back on other tabs, when the
bottom screen goes away and when the companion closes; and tap selection installs a small hook
that runs the game's own selector code after a tap and stays until the game closes.

Map art, names, descriptions, portraits and fonts are read from the player's own game files at
run time; the package contains no game assets.

## Credits

Save block format and encryption follow the public documentation of
[PKHeX](https://github.com/kwsch/PKHeX) and [pkNX](https://github.com/kwsch/pkNX) (MIT).
