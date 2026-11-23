# FINAL FANTASY RESONANCE companion

English companion for Switch update **1.0.1**, title `0100606026F12000`,
build `84E182F72EE1DA4F477804BD05B54C1500000000000000000000000000000000`.
Requires runtime 22. Tested on an AYN Thor with Peanuts Native 0.1.7. Includes Android arm64-v8a and Linux x86-64 modules.

- **Map.** The default tab. The current town's or dungeon's floor plan, or the world map with its islands. Your position and
  heading move live. Shops, the inn, save points and the current objective are marked with the game's own icons. Pinch or use
  +/- to zoom, and reset to come back to the player.
- **Party.** Your party and the members who have joined, with the game's character art, HP and MP, and the equipped weapon, armor
  and vision. Tap a member for the full portrait, stats and every equipment slot.
- **Items.** Items, Equipment, Key Items and Artifacts, with counts and the game's descriptions. Equipment shows who has each piece
  equipped. Tap an item to read its description, and swipe to scroll.
- **Battle.** Opens by itself in a battle, then returns to the tab that was open before. It shows enemies on the left and the
  party and any guest ally on the right, with HP and MP, a tag on staggered enemies, and each vision beside the character it is equipped to.
- **Battle commands by touch.** The command window is mirrored on the second screen: tap a command, a skill or an item, then
  tap a target. The game's target pointer is shown, and the resonance wheel's choices can be tapped too. Limit Bursts appear
  only when the game lists them.

The commands are sent as ordinary button presses, one at a time, and only while the game shows the matching menu. A press of A
goes out only once the game's own cursor is on the tapped entry or target. The companion never writes the game's memory and never
touches its saves.

The companion ships no game art. Its fonts (Vogel and Garrick), icons, portraits, map captures, menu art and English text are
decoded at runtime from your own `FFRS-Switch.utoc/.ucas` and `FFRS-Switch.pak`. Oodle decoding uses the vendored ooz
(GPL-3.0-or-later). The code is GPL-3.0-or-later; see the repository LICENSE.

Unsupported builds are refused.
