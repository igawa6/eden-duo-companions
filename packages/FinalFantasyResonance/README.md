# FINAL FANTASY RESONANCE companion

English companion for Switch update **1.0.1**, title `0100606026F12000`,
build `84E182F72EE1DA4F477804BD05B54C1500000000000000000000000000000000`.
Requires runtime 25 (Eden Duo 1.4.0 or later). Tested on Peanuts Native on an AYN Thor; not yet verified on Eden Duo. Includes Android arm64-v8a and Linux x86-64 modules.

- **Map.** The default tab. The current town's or dungeon's floor plan, or the world map with its islands over the game's sea chart,
  which stays still while the map moves and zooms. Your position and heading move live: you stay in the centre and the map moves around you, at the zoom you set with +/-, kept for the next visit.
  Drag or pinch to look around; Home brings the map back to you at the zoom on screen and follows you again. Shops, the inn and save points
  are marked with the game's own icons. The story objective is marked in gold and side quests in progress in blue, and an arrow
  at the edge of the map points to each objective that is out of view.
- **Fast travel.** On the world map, the places you know are marked; tap one for its card, then Fast Travel. The game's own
  travel runs, with its own rules: no travel out of a dungeon, during a story event or to a place you have not visited yet, and
  the card says why when the game refuses. In a town or dungeon, a World switch shows the world map, and Back returns to where you are.
- **Party.** Your party and the members who have joined, with the game's character art, HP and MP, and the equipped weapon, armor
  and vision. Tap a member for the full portrait, stats and every equipment slot.
- **Items.** Outside battle, use recovery items with one finger: tap an item, then the member to use it on. The members it can
  help light up, the others say why not, and the card shows what it did ("+120 HP", "Revived!"). Party items heal everyone in
  one tap; battle-only items are dimmed. Key Items and Artifacts are listed with the game's descriptions.
- **Equipment.** Pick a member, a slot, then a piece. Only the pieces the game lets that member wear are listed, each with its
  stat changes in the game's stat icons, its description in the game's words (such as more EXP or gil), and the face of whoever
  wears it now. The piece in the chosen slot shows its description beside the slots. One tap equips; accessories can be removed.
  Items and equipment work with the game's menu open too (not during a scene or a save).
- **Quests.** The journal's Main and Side tabs, each quest with its place and story so far. Track or stop tracking a quest, as
  the journal's Track does; the story quest is always followed, as in the game.
- **Battle.** Opens by itself in a battle and takes the whole screen; the tabs come back when it ends. It shows enemies on the left and the
  party and any guest ally on the right, with HP and MP, a tag on staggered enemies, and each vision beside the character it is equipped to.
- **Battle commands by touch.** The command window is mirrored on the second screen: tap a command, a skill or an item, then
  tap a target. The game's target pointer is shown, and the resonance wheel's choices can be tapped too. Limit Bursts appear
  only when the game lists them.

Battle commands are sent as ordinary button presses, one at a time, and only while the game shows the matching menu. A press of A
goes out only once the game's own cursor is on the tapped entry or target.

Fast travel, items, equipment, quest tracking and the menu sounds use a guest helper (`dualscreen/guest/<build>/load-plan.json`,
source `native/helper/ffr_guest_helper.S`). It hooks one function the game calls every frame and, when the companion asks, runs
the game's own code on the game's thread: its fast-travel check and travel (what Yes on the world map's dialog runs), its item
check and use, its equippable check, equip and unequip, its journal's Track, and its UI sounds by tag. The game's own rules
decide, and the companion shows its answer. When the helper is not installed or does not answer, fast travel opens the game's
world map and travels through it with button presses instead, and the other actions say the helper is not running. The companion
never writes the game's memory or saves itself; what the game's code changes, the game saves as usual.

On Peanuts Native, the helper is installed while the game starts, and frame generation keeps that from working: with frame
generation on at launch, the helper is not installed. Start the game with frame generation off, and turn it on in the in-game
panel once the game is running; the helper keeps working.

Tested on Peanuts Native on an AYN Thor; not yet verified on Eden Duo. Reports from Eden Duo are welcome.

**Warning: this companion changes your game.** Fast travel, using items, equipping and unequipping, tracking a quest and the menu
sounds run the game's own code, so items are used up, gear changes and your party travels, as if you had done it in the game's own
menus. The game saves those changes at its next save, as usual. The helper's hook stays installed until the game closes.
If the build is not the supported one, or the module fails to load, the page says so and the companion stays off.

The companion ships no game art. Its fonts (Vogel and Garrick), icons, portraits, map captures, menu art and English text are
decoded at runtime from your own `FFRS-Switch.utoc/.ucas` and `FFRS-Switch.pak`. Oodle decoding uses the vendored ooz
(GPL-3.0-or-later). The code is GPL-3.0-or-later; see the repository LICENSE.

Unsupported builds are refused, and the page says why.
