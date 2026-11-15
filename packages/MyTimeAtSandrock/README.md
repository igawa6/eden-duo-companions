# My Time at Sandrock companion

Release candidate **1.0.0-rc.2 requires Eden Duo 1.4.0 or newer** (runtime 25).
Older runtimes refuse the package with their update-required message.

When the host identifies a unique matching texture, the outdoor Map follows the game's live
fog once per second. Multiple matches leave the Map fogged rather than guessing. Unexplored ground stays
covered by the native fog artwork. Loading, title and unavailable texture states fail closed.
A host without texture readback capability retains the earlier tile-fog implementation.

The live fog is only shown where it looks like a fog (map tiles dense with hard stripes are
treated as noise) or where the companion's own exploration record agrees: the save's fog
(noise tiles ignored) plus the cells the player stamped this session, with a small margin.
A save written by an emulator that never copied the fog back from the GPU (Eden on Android,
New Game) carries uninitialised stripes; the native map shows them, the companion keeps them
fogged outside explored ground. The companion never reveals more than the game.

The static native world art is decoded from the player's game at full resolution in 16×16
tiles (256 px each when zoomed in); each tile carries its own crop of the 256×256 mask.
A tile whose mask is still loading is not drawn, so terrain never shows unmasked.
No game art or font binaries are included in this package's sources.

The parchment fills the companion above the navigation bar. Inventory, Mission, Social,
Workshop rankings, Handbook and Ingredients use continuous drag/fling scrolling. Selecting
an Inventory item keeps its description and bag/storage counts in a right detail pane.

Outdoors, the map follows the actor position independently of fog stamps. The player arrow
uses character facing; the native view cone uses gameplay-camera heading. The default scale
comes from the game's minimap; Reset restores the default north-up view and follow after a pan.
No game data is bundled.

The landscape native menu has seven tabs, with Settings last. Social sorts met townsfolk by
birthdays within the next week, gift still available, then hearts. Its detail pane and dialogue
card show native relationship titles, daily Chat/Gift status, birthdays, and only discovered
likes with owned counts. Inventory adds Liked by and Needed for (the readable tracked item
requirement and unlocked blueprint/recipe materials). Storage counts cover loaded scene boxes.
Untracked mission target kinds and per-machine identity cards remain unavailable. The map's
native Back button resets follow; +/− controls zoom about the current view centre.

The same pages support a 1080×1620 portrait canvas, with separate text wrap widths. Landscape
keeps its original field widths. Selected item and blueprint titles reserve up to three
lines at the essential text size. List fades disappear at the scroll end. Portrait likes retain four columns and scroll through all
discovered slots. Handbook materials use a 126 px row to separate two-line names and counts.

Desktop verification passed for same-page landscape/portrait switches on Today, Social,
Inventory and Map, including retained selection/scroll, outdoor follow, portrait Reset and
centre-preserving zoom. Japanese Social and Inventory portrait checks also passed.
Physical Android device verification remains pending.

## Credits

By igawa6. Source: GPL-3.0-or-later; see the repository LICENSE.
My Time at Sandrock by Pathea Games. Game assets remain the property of their respective owners.
The companion ships no game art or text. Artwork, fonts and localized text are read from your own game files.
Existing third-party decoder and font-rasterizer notices are retained in the source.
