# Package format

A package contains `package.json`, `dualscreen/manifest.json`, an optional per-build data file,
optional package files, and an optional native module. This reference is derived from the parser
code. Parse functions are named so you can read the exact behaviour, and every example is copied
from a real package.

Parser locations (all in the [Eden Duo](https://github.com/igawa6/eden-duo) repository, under
`src/core/mods/` unless noted):

- `mod_manifest.cpp`: `ParseManifestJson`, `ParseGate`, `ParseColor`, `ParseNumber`,
  `ParseValueType`, `ParseWidgetType`, `ParsePoints`, `ParseDerived`, `ParseScrollRegion`,
  `ParseWidgetAnim`, `ParseMapWidgetExtras`, `ParseMapAreasJson`, `ParseHaptics`,
  `ParsePageBindTarget`, `PackageMinRuntime`, `ModRuntime::Discover`
- `mod_nx_runtime.cpp`: `NxAssets::ParseManifestExtras`, `ParseMsbtConfig`
- `mod_guest_bridge.cpp`: `ResolveCallArg`
- `mod_module.cpp`: `GameModule::Load`
- `mod_load_plan.cpp`: `DiscoverModLoadPlan`, `ParseWrites`
- Android (`src/android/.../utils/DualScreenPackageInstaller.kt`):
  `DualScreenPackageInstaller.validateExtractedPackage`

## 1. Layout

```
<TITLEID>.dsmod.zip
├── package.json                      archive identity (installer, builder, min_runtime)
└── dualscreen/                       asset root; file: paths are relative to this
    ├── manifest.json                 pages, widgets, actions, derived values, ...
    ├── <BUILD16>.json                per-build data: points, symbols, spies, patches
    ├── ...                           optional package files
    └── modules/<platform>/<TITLEID>.so
```

- **Per-build data file.** `Discover` takes the running build ID in upper-case hex, trims
  trailing zeros (never below 16 characters), and keeps the first 16 characters. It then tries
  these files in order and uses the first that parses:
  1. `<BUILD16>.json`
  2. the lower-case form
  3. `data.json`
- **Android ZIP limits.** The installer enforces:
  - the file name `<TITLEID>.dsmod.zip` or `<TITLEID>-<Name>-<X.Y.Z>.dsmod.zip` (upper-case title
    ID matching the game; `Name` is 1–64 characters of `[A-Za-z0-9_]`, and the version must equal
    `package.json`'s);
  - only `package.json` and `dualscreen/…` at the top level;
  - nothing under `dualscreen/modules/` except `<platform>/<TITLEID>.so`;
  - at most 512 MiB compressed, 1 GiB expanded, 512 MiB per entry and 10 000 entries;
  - at most 4 MiB each for `package.json` and `manifest.json`;
  - zip-slip, `..` and duplicate-entry checks.

## 2. `package.json`

The core runtime reads **only** `min_runtime` from this file (`PackageMinRuntime`). Everything
else is checked by the Android installer and by this repository's
`tools/build_dualscreen_package.py`.

| Key | Type | Rule |
|---|---|---|
| `format` | int | Must be `1` |
| `type` | string | Must be `"dual-screen-mod"` |
| `title_id` | string | 16 upper-case hex characters; must equal the game |
| `name` | string | Not blank; at most 256 characters |
| `version` | string | `MAJOR.MINOR.PATCH[-+suffix]` |
| `requires_module` | bool | Informational here; the value that counts is the one in `manifest.json` |
| `module` | object | Must **deep-equal** `manifest.json`'s `module` (see §3.2) |
| `min_runtime` | int or digit string | Oldest runtime this package works with |

A declarative-only package (Link's Awakening):

```json
{"format":1,"type":"dual-screen-mod","title_id":"01006BB00C6F0000",
 "name":"Link's Awakening dual screen","version":"0.10.12"}
```

A package with a native module (Persona 5 Royal; hash shortened here):

```json
"requires_module": true,
"module": {"abi": 1,
  "build_ids": ["D4B150B29A931CD381E2A30318FC299F2B0EE36D000000000000000000000000"],
  "libraries": {"linux-x86_64": {"path": "modules/linux-x86_64/01005CA01580E000.so",
                                  "sha256": "4bc5d69e…7b57"}, "android-arm64-v8a": {…}}},
"min_runtime": 11
```

The asset-free Metroid Dread package declares `"min_runtime": 12` in its manifest, because its
map data comes from the module's data extension (§3.12). The Mario Kart 8 Deluxe package declares
`"min_runtime": 13`, because its theme and row-format toggles are press-and-hold gestures (§3.5).

## 3. `manifest.json`: top level

### 3.1 Scalars (`ParseManifestJson`)

| Key | Type | Default | Notes |
|---|---|---|---|
| `format` | uint | 1 | Anything else: the package is skipped |
| `name` | string | `"dual screen mod"` | |
| `title_id` | string | — | Optional for a declarative package; **required** with `module` |
| `min_runtime` | uint / digit string | — | See [ARCHITECTURE.md §4](ARCHITECTURE.md#4-runtime-version-and-min_runtime). Use 13 for `on_hold`, 12 for `module:` byte sources or `map.areas_src` |
| `canvas_w`, `canvas_h` | uint | 0 (panel size) | Logical canvas; stretched to the panel |
| `background` | colour | — | |
| `poll_hz` | uint | 60 | Legacy; the tick is fixed at 60 Hz |
| `anim_hz` | number | 60 | 1–240. At ≥60, animations publish every tick |
| `font`, `font_atlas` | image source | "" | Long spellings `font_metrics_src`, `font_atlas_src` are used only if the short key is empty |
| `debug_page` | bool | false | Adds a built-in `__debug` page |
| `flags` | object | — | Initial runtime flags (bool or number). `gpu_composite: true` enables the quad compositor |
| `tables` | object | — | name → string array, used by value widgets' `table` |
| `sprite_map` | object | — | key → image source, used by `src_bind` |
| `haptics` | bool / object | — | `ParseHaptics` |
| `enforce`, `enforce_gate` | array / object | — | Periodic actions (§3.9) |
| `load_plan`, `load_plan_module` | string | — | Load-time patches (`DiscoverModLoadPlan`); the manifest must be ≤1 MiB for this path |
| `module_outputs` | array | — | **Ignored by the runtime.** It only documents a module's output names |
| `_about`, `_note`, `_*` | any | — | Ignored (comments) |

A manifest with no pages (and no `debug_page`) is rejected with "has no pages".

### 3.2 `module` (`GameModule::Load`)

This block is read from `manifest.json`, not from `package.json`.

| Key | Rule |
|---|---|
| `abi` | Must equal `EDEN_DSMOD_MODULE_ABI_VERSION` (1). The extensions, including runtime 12's data extension, are negotiated separately and do not change this number |
| `build_ids` | 1–256 strings, each 16 or 64 hex characters. The running build ID must **start with** one of them |
| `libraries.<platform>.path` | Must be exactly `modules/<platform>/<TITLEID>.so` |
| `libraries.<platform>.sha256` | 64 hex characters, matching the file. The Android installer additionally requires lower case |

- **Platform names:** `linux-x86_64`, `linux-aarch64`, `android-arm64-v8a`, `android-x86_64`. The
  package builder currently accepts only `linux-x86_64` and `android-arm64-v8a`.
- **Missing module.** If `requires_module` is true and `module` is absent, loading fails.
- **Failures do not stop the page.** A module that fails to load still lets the pages render. The
  host publishes `module_ready`, `module_error`, `module_error_message` and `build_match`, so a
  page can show why.

### 3.3 Value types

| Kind | Parser | Accepts |
|---|---|---|
| Colour | `ParseColor` | Unsigned int `0xAARRGGBB`, or string `"#AARRGGBB"` / `"AARRGGBB"`. Six hex digits or fewer get alpha FF. Anything bad falls back silently |
| Number | `ParseNumber` | JSON integer, or string: decimal, `"0x…"`, or signed (`"-0x2C"`). A JSON float gives 0 |
| Gate | `ParseGate` | `"name"` or `"!name"`. Open when the named int/float value exists and is non-zero (negated with `!`). A missing value counts as closed |

**Names.** A gate or bind is a single name with no expression syntax. Build compound conditions
as derived values. Persona 5 Royal, for example, gates a scroll region on a derived value whose
name happens to contain `+`:

```json
{"name": "ui.and.item.ready+ui.iv.0", "all_nonzero": ["item.ready", "ui.iv.0"]}
```

Names that binds, derived values and `$refs` can use:

| Namespace | Holds |
|---|---|
| (bare names) | Points, derived values, sequence outputs, spy values, and module outputs |
| `@flag:<name>` | Runtime flags |
| `@sel:<group>` | The selected payload of a `select_group` |
| `@map_sel:<group>` | The selected map marker |
| `@last:<group>` | The last selection in a group |
| `@scroll:<id>`, `@scroll_max:<id>`, `@scroll_count:<id>`, `@scroll_first:<id>` | Scroll-region state |
| `@drag`, `@drag_payload`, `@drag_x`, `@drag_y`, `@drag_hover` | Drag state |
| `@map_tap_x`, `@map_tap_y` | The last map tap position |
| `@<counter>` | Button-action counters |
| `@fade:<name>` | Fade progress |
| `view_custom:<key>` | Custom view state |

### 3.4 Pages

| Key | Type | Default | Notes |
|---|---|---|---|
| `id` | string | "" | Target for `page` actions and `page_binds` |
| `title` | string | "" | |
| `widgets` | array | — | Drawn in order |
| `scrolls` | array | — | Scroll regions (`ParseScrollRegion`) |
| `mirror` | [x,y,w,h] floats | — | Shows a normalised crop of the game frame instead of widgets |
| `no_auto_leave` | bool | false | `page_binds` never navigate away from this page |

### 3.5 Widgets

`type` is parsed by `ParseWidgetType`. The valid types are `rect`, `label`, `value`, `bar`,
`button`, `pips`, `image` and `map`. **An unknown or missing type becomes `label` silently.**
Lists and repetition are not separate types: `repeat` and `scroll` are keys that work on any
widget.

**Common keys**

| Key | Default | Meaning |
|---|---|---|
| `rect` | — | `[x, y, w, h]` in canvas pixels (exactly 4 numbers) |
| `id` | "" | Target for anims, `origin` and `view_reset` |
| `text`, `bind` | "" | Literal text; published value |
| `color`, `bg` | `#FFE6ECF2`, 0 | Foreground and background |
| `text_scale` | 3 | Integer glyph scale |
| `align` | left | `left`, `center`, `right` |
| `on_tap` | — | Action name |
| `on_hold`, `hold_ms` | —, 600 | Runtime 13. `on_hold` is an action run once when a single finger rests on the widget for `hold_ms` milliseconds (absent or ≤ 0 means 600). The target is the topmost visible `on_hold` widget under the first finger, chosen independently of `on_tap`: a large `on_hold` area under small buttons gets the hold while the buttons keep their taps. An `input_block` above it blocks the hold. It is not armed on a draggable widget with a payload or during a page transition. Moving more than the 12 px tap slop, a second finger, a drag, a page transition or any page change cancels it. The lift that ends a fired hold is not a tap. A still finger on a map or scroll list can hold; after the hold fires the finger may still pan or scroll. `{i}` is substituted in repeat templates, as for `on_tap` |
| `hide_bind`, `keep_min`, `keep_max`, `hide_eq`, `need_bind` | — | Visibility. `hide_eq` must be an integer; a string there **throws** |
| `tap_block`, `input_block` | false | Absorb touches |
| `x_bind`, `y_bind`, `x_scale`, `y_scale` | — | Offset the widget by a published value |
| `anim` | — | Animation (`ParseWidgetAnim`): `bind` (required gate), `group`, `from` (right/left/top/bottom/fade/`widget:<id>`), `ms` (≤5000), `easing`, `box` |
| `haptic` | — | Per-widget haptic override |

**Per type**

| Type | Extra keys |
|---|---|
| `value` | `names` (index → string), `table`, `pad`, `div`, `mul`, `add`, `suffix`, `max_bind` / `max` / `max_const`, `max_sep` |
| `label` | `bind_text` (string point), `text_src` (`msbt:<alias>#<label>`), `text_bind` + `text_map` (int → text or `msbt:`), `wrap_width`, `max_lines`, `line_gap`, `icon_style` |
| `rect`, `button` | `frame` (outline px, default 2), `pulse`, `pill` |
| `bar`, `pips` | `bind`, `max_bind` / `max` |
| `image` | `src`, `src_bind`, `src_names`, `src_thresholds` (`[{le, src}]`), `src_format` (printf of the value), `src_rect` (normalised UV), `empty_src`, `empty_rect`, `fill_bind`, `flip_x`, `flip_y`, `spin` (deg/s), `shake` |
| `map` | `area`, `area_bind`, `room_bind`, marker keys (`marker_x_bind`, `marker_y_bind`, `marker_icon`, …), actor keys, `follow_window`. Map-only (`ParseMapWidgetExtras`): `groups`, `label_style`, `on_map_tap`, `on_marker_tap`, `marker_hit_px`, `marker_tap_groups` |
| any | Pan/zoom: `pan_zoom`, `min_zoom`, `max_zoom`, `view_idle_ms` |
| any | Repeat: `repeat`, `repeat_bind`, `repeat_div`, `repeat_dx`, `repeat_dy`, `repeat_cols`, `repeat_row_dy`, `pack`. `{i}` in any string becomes the element index |
| any | Scroll: `scroll`, set to a region id or an inline region object |
| any | Drag and drop: `payload`, `select_group`, `draggable`, `drag_scale`, `drop_action`, `accept_group`, `highlight_src`, `highlight_color`, `drag_under_*` |

Real examples:

```json
{"type":"rect","rect":[0,0,1240,1080],"bg":"#FF12100C"}                       // StoryOfSeasonsFoMT
{"type":"value","rect":[330,160,0,0],"bind":"geo","text_scale":7,"color":"#FFF2E7A8"}  // HollowKnight
{"type":"value","bind":"missile","max_bind":"missile_max","max_sep":" / ","need_bind":"map_ready"} // MetroidDread (excerpt)
{"type":"value","bind":"dungeon","table":"dungeon_names","align":"center"}  // LinksAwakening (excerpt)
{"type":"bar","rect":[480,340,500,40],"bind":"soul","max_bind":"max_mp","color":"#FF3EC6E0","bg":"#FF1C2733"} // DSModTest
{"type":"image","src":"module:p5r:Q","rect":[0,0,1240,10]}                   // Persona5Royal
{"type":"map","rect":[40,90,1160,880],"area_bind":"map_zone","room_bind":"scene_name","area":"Crossroads"} // HollowKnight (excerpt)
```

The `//` comments are annotations for this document. JSON itself has no comments, so use
`_note` keys in a real manifest.

A repeated, scrolling list (Persona5Royal, `m_skill` page). The widget draws 10 rows, 92 px
apart, inside the region `mem_ui.skm`:

```json
{"type":"image","src":"module:p5r:17P","rect":[24,132,462,84],"repeat":10,"repeat_dy":92,"scroll":"mem_ui.skm"}
```

### 3.6 Scroll regions (`ParseScrollRegion`)

| Key | Default | Notes |
|---|---|---|
| `id`, `rect` | **required** | If missing, the region is skipped with a warning |
| `count_bind` | "" | Number of rows |
| `row_h`, `cols`, `pad` | 0, 1, 0 | |
| `show_bind`, `reset_bind` | — | Gate; a value whose change resets the scroll position |
| `fling`, `friction` | true, 0.135 | Friction is clamped to 0.0001–0.99 |
| `bar`, `bar_track`, `bar_w` | 0, 0, 6 | Scrollbar colours (0 = none) and width |

```json
{"id":"mem_ui.skm","rect":[24,132,472,640],"count_bind":"roster.count","row_h":92,
 "bar":"#FFE5191C","bar_track":"#33FFFFFF","bar_w":6,"show_bind":"roster.ready"}   // Persona5Royal
```

The region publishes `@scroll:<id>` and related values (§3.3).

### 3.7 Actions (`actions`, name → object)

Every action has `kind`, and may have `enabled_bind` (a gate; the action is skipped while it is
closed) and `haptic`. **An unknown or missing `kind` drops the action silently.**

Action values (`value`, `argument`) may be an integer, a float, a bool, `"$payload"` (the
dragged or selected widget's payload), `"$<name>"` (any published value, e.g. `$map_x` or
`$@flag:x`), or a number string.

| Kind | Keys | Real example |
|---|---|---|
| `button` | `button` (A B X Y L R ZL ZR Plus Minus DUp DDown DLeft DRight), `frames` (4), `repeat`, `counter`, `target`, `modulo`, `delta`, `button_neg`, `gap` (18 ticks), `hold_map` | `{"kind":"button","button":"Minus","frames":8}` (HollowKnight) |
| `page` | `page`, `transition` (slide_up, slide_down, grow, shrink, fade, none), `duration_ms` (≤2000), `easing`, `shadow`, `origin` | `{"kind":"page","page":"map","transition":"fade","duration_ms":200,"easing":"ease_in"}` (LinksAwakening) |
| `module` | `action` (string passed to the module's `on_action`), `argument` | `{"kind":"module","action":"calendar_select","argument":"$payload","enabled_bind":"calendar.ready"}` (Persona5Royal) |
| `flag` / `set_value` | `flag`, `value` (omit to toggle), `cycle` | `{"kind":"flag","flag":"pin_panel"}` (LinksAwakening) |
| `write` | `point`, `value`, `swap_point` | `{"kind":"write","point":"equip_x","value":"$payload","swap_point":"equip_y"}` (LinksAwakening) |
| `slot_write` | `slot`, `count`, `free_value`, `select`, `writes[{point,value}]` | LinksAwakening map pins: finds a free slot and writes x/y/kind |
| `call` | `fn` (`"$Symbol"` or an address), `args` | `{"kind":"call","fn":"$MaxHealth","args":["$hero_instance"]}` (HollowKnight) |
| `sequence` | `sequence` (a name in `sequences`) | `{"kind":"sequence","sequence":"hud_on"}` (MetroidDread) |
| `view_reset` | `view` (widget id) | `{"kind":"view_reset","view":"dread_map"}` (MetroidDread) |
| `map_select` | `group`, `value` | No example in a shipped package |

- **Guest calls under NCE.** `call` and `sequence` run guest code through a breakpoint bridge that
  only works with the Dynarmic CPU backend. Under NCE (the Android default) they are disabled,
  and a warning is logged.
- **Call arguments** (`ResolveCallArg`):

  | Form | Means |
  |---|---|
  | plain string | Parsed as a number |
  | `$L` | Lua state |
  | `$ret` | Result of the previous call |
  | `$#slot` | A saved slot |
  | `$"literal"` | A staged string |
  | `$@symbol` | `main` + the symbol's offset |
  | `$&addr` | A published address |
  | `$name` | A published int |

### 3.8 `page_binds` (V10)

`page_binds` is an array. Each entry has `point` and `equals` (both required), plus optional
`ready_bind`, `when_equal` and `when_not_equal`. Each target (`ParsePageBindTarget`) takes the
same keys as a `page` action. A bind fires on the edge into or out of `equals`.

```json
{"point":"any_menu_open","equals":1,"when_equal":{"page":"standby"},"when_not_equal":{"page":"map"}}  // LinksAwakening
{"point":"state.mode","equals":0,"when_equal":{"page":"waiting","transition":"fade","duration_ms":250}} // Persona5Royal
```

### 3.9 `flags`, `enforce`, `haptics`

```json
"flags": {"bt.sel":0,"bt.mode":0,"ui.skm":0}                          // Persona5Royal (excerpt)
"flags": {"gpu_composite":true}                                     // MetroidDread
"enforce": [{"action":"pdrv.press.x","every_ms":1}],
"enforce_gate": {"point":"pdrv.pending","max":1}                    // Persona5Royal
"haptics": {"enabled":true,"respect_system":true,"tap":"click","drop":"confirm","refused":"off"} // LinksAwakening (excerpt)
"haptics": {"enabled":true,"respect_system":true,"tap":"light","hold":"heavy","refused":"off"}   // MarioKart8Deluxe
```

- **`enforce`.** Each entry runs `action` every `every_ms`. It runs only while its optional
  `flag` equals `value`, and only while the `enforce_gate` point is between 1 and `max`.
  Persona 5 Royal uses this to press native-menu buttons that its module requests (see
  [PORTING_A_GAME.md §5](PORTING_A_GAME.md#5-driving-the-native-menu)).
- **`haptics` kinds:** `tap`, `write`, `select`, `drag`, `drop`, `marker`, `refused` and, since
  runtime 13, `hold` (default `heavy`). It plays when a hold's action ran; a refused action plays
  `refused` instead. Widget and action `haptic` overrides do not apply to holds.
- **`haptics` strengths:** `off`, `light`, `click`, `confirm`, `heavy`, `reject`.

### 3.10 Derived values

`derived` is an array, parsed by `ParseDerived`. Entries may appear in both the manifest and
the data file; a data-file entry with the same `name` replaces the manifest entry. Each entry has
a `name` and exactly one form. Evaluation checks the forms in this order:
`any_eq` > `select` > `hold_last_nonzero` > `cmp` > `all_nonzero` / `any_nonzero` > `terms`.

| Form | Keys | Result |
|---|---|---|
| terms | `terms` (`"src"`, `["src", factor]` or `{point, factor}`), `add`, `floor` / `round` | Σ value·factor + add |
| select | `select`, `then`, `else` | The `then` value while `select` is non-zero, else the `else` value |
| hold | `hold_last_nonzero`, `hold_gate` | The last non-zero value |
| any_eq | `{array, count, value}` | Count of `array0..N-1` equal to `value` |
| cmp | `cmp` (eq, ne, ge, gt, le, lt), `a`, `b` (name or constant) | 1 or 0 |
| all_nonzero / any_nonzero | list of names | 1 or 0. Fails closed if any source is missing |

```json
{"name":"pieces_hearts","terms":[["heartpiece_count",0.25]],"floor":true}          // LinksAwakening
{"name":"max_hearts","terms":[["container_count",1],["pieces_hearts",1]],"add":3}  // LinksAwakening
{"name":"bt.focus","select":"bt.tsel","then":"bt.f0","else":"bt.f2"}              // Persona5Royal
{"name":"bt.fge","cmp":"ge","a":"bt.focus","b":0}                                 // Persona5Royal
```

### 3.11 Image, font and data sources

| Prefix | Resolved by | Notes |
|---|---|---|
| `file:<path>` | Package file | Relative to `dualscreen/` |
| `romfs:/<path>[#member…]` | Nx asset worker | The **player's own** game files. Container paths: `x.arc#member#tex` (SARC→BNTX), `x.bntx#tex`, `x.bffnt`, `.lzs#member.xtx`, `.bctex`, `.dds`, `.png` |
| `module:<key>` (image) | Module `load_image` | Asynchronous: null until the module delivers the image. The module defines the key format |
| `module:<key>` (bytes) | Module `load_data` (runtime 12) | Any byte read through `ReadAssetBytes`: map `geo` and layer blobs, `map.areas_src`, the `font` metrics. May block while the module generates the data; empty if no module serves the key |
| `composite:<name>` | `composites` | Layered image built by the runtime |
| `map:<area>@<w>x<h>` | Map rasteriser | |
| `icon:` | Title icon | |

```json
"src":"romfs:/textures/gui/textures/czdr-rwk.bctex","src_rect":[0.006836,0.556641,0.053711,0.648438]  // MetroidDread
"font":"romfs:/region_common/ui/jpn_main.bffnt"                                   // LinksAwakening
"font":"file:p5r_art.rec","font_atlas":"module:p5r_font:romfs:EN/FONT/FONT0.FNT"  // Persona5Royal
```

- **Composites** (`NxAssets::ParseManifestExtras`) take `w`, `h` (≤8192), `background`, `flat`,
  `levels` and `layers[]`. Each layer has `src`, `rect`, `src_rect` / `src_px`, `mask_src`,
  `show_bind` / `hide_bind`, `fade_ms` and `opacity`. From Persona5Royal:

  ```json
  "rmap_10_4_0":{"w":164,"h":311,"layers":[{"src":"module:p5r:AF","rect":[0,0,164,311]}]}
  ```

- **Game text** comes from `msbt` (alias → `romfs:` path with `{REGION}` / `{LANG}`),
  `msbt_lang` and `msbt_lang_fallback`, parsed by `ParseMsbtConfig`.
- **Fonts.** The font metrics are decoded by the module's `decode_font` when it exports one.
  Otherwise the runtime uses a built-in parser (MFNT, the Story of Seasons layout, or BFFNT).

### 3.12 Map

Map configuration is a large subsystem, parsed in `ParseManifestJson` (the per-area part is
`ParseMapAreasJson`):

- `map.atlas`, `cell`, `style{…}`, `icons{…}`, `areas{…}` and `areas_src`. These are read **only
  when** `map.areas` or `map.areas_src` exists.
- **`areas_src` (runtime 12).** A `module:` key whose bytes are a JSON object in the same shape as
  `map.areas`. The module generates it through its data extension; the runtime fetches and
  parses it on a worker thread and then replaces the areas once
  ([ARCHITECTURE.md §2.10](ARCHITECTURE.md#210-module-generated-data-runtime-12)). The inline
  `areas` are the authored template shown until then, and stay in use if the fetch fails. The
  module needs the data extension, and the package should declare `"min_runtime": 12`.
- Each area can carry:
  - `geo` / `image`, and the bounds `min` / `max`;
  - `layers`, `room_categories`, `icons`, `dynamic_markers`, `labels`;
  - `occluders`, `vignettes`, `camera_rects`, `overview_regions`.
- `map.zone_area` and `map.rooms` are used by room-sprite maps.

Real examples:

```json
"areas_src":"module:dread:areas"                                                   // MetroidDread
"s010_cave":{"geo":"module:dread:map/s010_cave.geo","layers":[{"geo":"module:dread:map/s010_cave.water.geo","kind":"water",…}],…}  // MetroidDread template (excerpt)
{"group":"pins","count":30,"x":"pin_x{i}","y":"pin_y{i}","kind":"pin_kind{i}","hide_when_kind":0,"icon_default":"pin_8","size":70} // LinksAwakening dynamic marker (excerpt)
```

The style keys and their defaults are in the `MapStyle` struct in `mod_types_map.h`.

## 4. Per-build data file

`<BUILD16>.json` holds everything that depends on the exact executable. The keys `points`,
`symbols`, `spies`, `patches` and `metadata_anchor` are read **only** from this file, never from
`manifest.json`.

### 4.1 `points` (`ParsePoints`)

| Key | Meaning |
|---|---|
| `type` | `u8`, `s8`, `u16`, `s16`, `u32`, `u64`, `s64`, `f32`, `bool`, `string`/`utf16`, `cstring`, `utf32`. **Unknown types become `s32` silently** |
| `addr` | A single address token: `"main+0x…"`, `"abs0x…"` or `"0x…"` |
| `ptr` | Hop list. The first hop is the base address; each later hop dereferences and adds. Object hops: `{offset, stride, index / index_bind, list_next, list_node_at, static_fields}` |
| `offset` | Added after the last hop |
| `count`, `count_bind`, `stride` | An array, published as `name0..nameN-1`. `"$i"` in a hop is the element index |
| `shift`, `mask`, `bit`, `popcount` | Integer post-processing |
| `pointer` | Publish the value as an address |
| `find`, `class_name`, `player`, `array`, `text_scan`, `root` | Alternative address sources: pattern scan, IL2CPP class, and heap searches |

A chain with N dereferences needs N+1 entries, because the first entry is the base address and
is not read. A point without an address source is **dropped silently**.

```json
"seashell_count": {"type":"u64","ptr":["main+0x1CC0768"],"offset":"0x0","popcount":true}   // LinksAwakening
"dun_keys": {"type":"u8","ptr":["main+0x1CC11B0",{"offset":0,"stride":8,"index":"$i"},"0x0"],"offset":"0x0","count":10} // LinksAwakening
```

### 4.2 Other sections

| Key | Shape | Real example |
|---|---|---|
| `symbols` | name → `"main+0x…"` or `{class_name, method}` (IL2CPP) | `{"GetMaxHealth":"main+0xDF8260"}` (LinksAwakening) |
| `spies` | `[{name, symbol, reg}]`: capture a register when code runs (Dynarmic only) | |
| `patches` | `[{at, write:[hex u32…], why, optional}]`. Optional patches apply only with `EDEN_DSMOD_PATCHES=1` | `{"optional":true,"at":"main+0x11CDC38","write":["B4000280"],"why":"null-guard …"}` (MetroidDread) |
| `metadata_anchor` | Address token (IL2CPP) | |

## 5. Validation behaviour

| Situation | Result |
|---|---|
| Unknown keys | Ignored everywhere; there is no schema warning |
| Unknown widget `type` / point `type` / action `kind` | Becomes a label / becomes `s32` / action dropped. **All silent** |
| Unknown transition, easing, haptic or anim `from` | Falls back, with a warning |
| Wrong JSON type for a typed key (e.g. `"pad":"3"`, `"hide_eq":"1"`, a sequence without `steps`) | The parser throws, and **the whole package is rejected** with "DSMod: ignoring invalid package …". Discovery moves on to the next folder |
| JSON syntax error | The folder is skipped; the error is logged |
| Malformed data file | Skipped. The package loads without points |
| `min_runtime` newer than the runtime | The "UPDATE EDEN" page is shown, and scanning stops |

There is no standalone schema. Treat the C++ parser as authoritative: Eden Duo exposes it to
tools as `ParseDualScreenManifest` and `IsUsableDualScreenManifest` (`mod_runtime.h`), and the
surest check is to boot the game and read the log. `tools/build_dualscreen_package.py` checks the
package metadata and every `file:` reference, but not the manifest's semantics.
