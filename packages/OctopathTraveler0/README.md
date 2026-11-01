# Octopath Traveler 0 companion 1.0.0

By igawa6. English companion for Switch update **1.0.5**, title `01005270232F2000`,
build `9FE60A212CFF9B9A3656AB63D4F882E500000000000000000000000000000000`.
Requires runtime 22 (Eden Duo 1.3.2 or newer for the live HUD settings protocol).
Includes Linux x86_64 and Android arm64-v8a modules.

- **Map.** Pan and zoom the current revealed native section with original map markers,
  player position and front/back party HP/SP. Hidden markers remain hidden.
- **Party.** Inspect members, available native status values and equipped items.
- **Items and Quests.** Browse inventory categories and available quests on the bottom screen.
- **Battle.** Live party resources, BP and boost forecast, equipped weapon, enemy HP,
  shields, Break, revealed weaknesses and visible status icons. Auto follows native target
  selection; Overview enables companion-only enemy focus. Taps do not execute battle commands.
- **Healing Skills.** On the field map, select a member, preview a learned affordable skill
  and confirm its native HP/SP transaction. Stale and locked contexts refuse the action.
- **Settings.** Re-show the moved top HUD, disable touch feedback or reduce motion.
  The native minimap is hidden only with a complete rendered replacement and a live lease.
  The battle HUD and command transform restore when the companion is hidden or disabled.

Art and fonts are decoded from the player's game archive after pinned header and payload checks.
No game art, fonts, keys or saves are included. Code is GPL-3.0-or-later; see the repository LICENSE
and the preserved third-party notices for bundled decoder code.

Limits: English/current section only; the custom protagonist name displays as Leader.
Unsupported builds are refused. The native helper requires zero relocation delta; unsupported
relocation retains native map information and leaves computed details unavailable.
Consumables are read-only. Native revival is fixture-tested, with live verification pending.
Android cross-compilation and desktop smoke do not establish physical device legibility,
haptics, NCE safety or performance. A newer game update has not been verified.
