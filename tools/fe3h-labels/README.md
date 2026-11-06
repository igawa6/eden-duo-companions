# Fire Emblem: Three Houses: the fixed words in the game's language

The manifest's `label` widgets used to carry English (`"text":"Stats"`). They now bind
`ui.l.<key>` (`bind_text`), and the module (`native/modules/fe3h_labels.*`) publishes the words of
the game's language under those keys.

- A **game term** (a stat, a weapon skill, "Victory Conditions" ...) is read from the game's own
  message data at run time, in the game's language: `refs.py` maps the English word to a message
  part and index. No game text is stored here, only the numbers.
- A phrase the **game has no word for** (the loading messages, "Waiting for the battle map" ...)
  is the companion's own text: `own.py`, one column per language (en ja de fr es it ko twn chn).
- `layout_fix.py` makes the turn label and number of the battle pages fit the game's wider words
  (TURN, TURNO).

```sh
python3 tools/fe3h-labels/gen_labels.py     # from the root of the repository
```

It rewrites `packages/FireEmblemThreeHouses/dualscreen/manifest.json` (a label that is already
converted is left alone) and `native/modules/fe3h_labels_data.inc`. It reads no game file.

To add a word: add it to `refs.py` (a game term) or `own.py` (an own phrase), make the manifest
widget a fixed `text` label, and run the script. The translations in `own.py` were written with the
help of an AI coding assistant and have not been reviewed by native speakers of every language;
corrections are welcome.
