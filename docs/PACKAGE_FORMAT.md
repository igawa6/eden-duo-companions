# Package format

A package contains `package.json`, `dualscreen/manifest.json`, an optional per-build data file,
optional package files, and an optional native module. This reference is derived from the parser
code. Parse functions are named so you can read the exact behaviour, and every example is copied
from a real package.

Parser locations (all in the [Eden Duo](https://github.com/igawa6/eden-duo) repository, under
`src/core/mods/` unless noted):

- `mod_manifest.cpp`: `ParseManifestJson`, `ParseGate`, `ParseColor`, `ParseNumber`,
  `ParseValueType`, `ParseWidgetType`, `ParsePoints`, `ParseDerived`, `ParseScrollRegion`,
  `ParseWidgetAnim`, `ParseMapWidgetExtras`, `ParseMapAreasJson`, `ParseHaptics`, `ParseNav`,
  `ParseImageStyleKeys`, `ParseChartSpec`, `ParsePageBindTarget`, `PackageMinRuntime`,
  `ModRuntime::Discover`
- `mod_settings.cpp`: `ParseSettings`, `ApplySettings` (the built-in `@settings` page)
- `mod_expr.cpp`: `CompileExpr` (derived `expr`); `mod_nav.cpp`: `Nav::ParseChord`
- `mod_sources.cpp`, `mod_romfs_sources.cpp`, `mod_user_source.cpp`: asset sources (`base:`,
  `aoc:`, `user:`)
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
The Super Mario Bros. Wonder package declares `"min_runtime": 15`; its HUD numbers use the
outlined, rising text keys that runtime 15 shipped (§3.14). See §6 for which runtime added which key.

## 3. `manifest.json`: top level

**The Since column** in the tables below gives the runtime version that added a key. A blank
cell means runtime 13 or earlier. A package that uses a key from runtime N should declare
`"min_runtime": N` (§6).

### 3.1 Scalars (`ParseManifestJson`)

| Key | Type | Default | Since | Notes |
|---|---|---|---|---|
| `format` | uint | 1 | | Anything else: the package is skipped |
| `name` | string | `"dual screen mod"` | | Also names the persisted-flags file (§3.18) |
| `title_id` | string | — | | Optional for a declarative package; **required** with `module` |
| `min_runtime` | uint / digit string | — | 11 | See §6 and [ARCHITECTURE.md §4](ARCHITECTURE.md#4-runtime-version-and-min_runtime) |
| `canvas_w`, `canvas_h` | uint | 0 (panel size) | | Logical canvas; stretched to the panel |
| `background` | colour | — | | |
| `poll_hz` | uint | 60 | | Legacy; the tick is fixed at 60 Hz |
| `anim_hz` | number | 60 | | 1–240. At ≥60, animations publish every tick |
| `font`, `font_atlas` | image source | "" | | Long spellings `font_metrics_src`, `font_atlas_src` are used only if the short key is empty |
| `font_page_h` | uint px | 0 | 17 | Paged font atlas: with `{p}` in `font_atlas`, the atlas is split into pages of this many rows (≤65535). See §3.14 |
| `debug_page` | bool | false | | Adds a built-in `__debug` page |
| `flags` | object | — | | Initial runtime flags (bool or number). `gpu_composite: true` enables the quad compositor |
| `persist_flags` | string array | — | 15 | Runtime flags saved on change and restored at load (§3.18) |
| `settings` | array | — | 17 | Rows of the built-in `@settings` page (§3.18) |
| `tables` | object | — | | name → string array, used by value widgets' `table` |
| `sprite_map` | object | — | | key → image source, used by `src_bind` |
| `haptics` | bool / object | — | | `ParseHaptics` |
| `nav` | bool / object | on with `min_runtime` ≥ 17, else off | 17 | Controller navigation of the second screen (§3.17) |
| `enforce`, `enforce_gate` | array / object | — | | Periodic actions (§3.9) |
| `module_tick_hidden` | bool | unset | 15 | Tick the native module while the second screen is hidden (`true`) or not (`false`). Unset: the module's own flag decides ([MODULE_GUIDE.md §1.2](MODULE_GUIDE.md#12-lifecycle-and-threads)). A non-bool value throws |
| `load_plan`, `load_plan_module` | string | — | | Load-time patches (`DiscoverModLoadPlan`); the manifest must be ≤1 MiB for this path |
| `module_outputs` | array | — | | **Ignored by the runtime.** It only documents a module's output names |
| `_about`, `_note`, `_*` | any | — | | Ignored (comments) |

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

| Namespace | Since | Holds |
|---|---|---|
| (bare names) | | Points, derived values, sequence outputs, spy values, and module outputs |
| `@flag:<name>` | | Runtime flags |
| `@sel:<group>` | | The selected payload of a `select_group`. Since runtime 16 it is published before the tick's taps run, so an `enabled_bind` sees the selection as it was before the tap |
| `@map_sel:<group>` | | The selected map marker |
| `@last:<group>` | | The last selection in a group |
| `@scroll:<id>`, `@scroll_max:<id>`, `@scroll_count:<id>`, `@scroll_first:<id>` | | Scroll-region state |
| `@drag`, `@drag_payload`, `@drag_x`, `@drag_y`, `@drag_hover` | | Drag state. Since runtime 16, on the tick a drag is dropped they describe the dropped drag while its drop action runs (`@drag_x` / `@drag_y` = the release point) |
| `@map_tap_x`, `@map_tap_y` | | The last map tap position (float and int) |
| `@map_tap_seq` | 14 | Bumped on every map tap, so a reader can tell a new tap from the last one. Modules can read all three `@map_tap_*` values through `get_i64` / `get_f64` |
| `@clock.hour`, `.minute`, `.second`, `.day`, `.month`, `.year`, `.wday`, `.epoch` | 16 | Device local time (`wday`: Sunday = 0; `epoch`: Unix seconds, UTC). Change at most once a second. Published only when the manifest or data file mentions them: any string or key containing `@clock.` or `@game.`, or a `countdown` entry |
| `@game.seconds` | 16 | Whole seconds since the emulated system started. Same publishing rule as `@clock.*` |
| `@nav.active`, `@nav.index`, `@nav.count`, `@nav.x`, `@nav.y`, `@nav.w`, `@nav.h` | 17 | Controller focus state (§3.17) |
| `@chart:<key>` | 17 | Number of samples a chart widget has taken (§3.16) |
| `@<counter>` | | Button-action counters |
| `@fade:<name>` | | Fade progress |
| `view_custom:<key>` | | Custom view state |

### 3.4 Pages

| Key | Type | Default | Since | Notes |
|---|---|---|---|---|
| `id` | string | "" | | Target for `page` actions and `page_binds`. Ids starting with `@` are reserved for built-in pages (`@settings`) |
| `title` | string | "" | | |
| `widgets` | array | — | | Drawn in order |
| `scrolls` | array | — | | Scroll regions (`ParseScrollRegion`) |
| `mirror` | [x,y,w,h] floats | — | | Shows a normalised crop of the game frame instead of widgets |
| `no_auto_leave` | bool | false | | `page_binds` never navigate away from this page |
| `nav_order` | string array | — | 17 | Controller focus order, as widget ids (§3.17) |

### 3.5 Widgets

`type` is parsed by `ParseWidgetType`. The valid types are `rect`, `label`, `value`, `bar`,
`button`, `pips`, `image`, `map` and, since runtime 17, `chart` (§3.16). **An unknown or missing
type becomes `label` silently**, so a `chart` on an older runtime is an empty label that draws
nothing. Lists and repetition are not separate types: `repeat` and `scroll` are keys that work on
any widget.

**Common keys**

| Key | Default | Since | Meaning |
|---|---|---|---|
| `rect` | — | | `[x, y, w, h]` in canvas pixels (exactly 4 numbers) |
| `id` | "" | | Target for anims, `origin`, `view_reset` and `nav_order`; names a chart's buffer |
| `text`, `bind` | "" | | Literal text; published value |
| `color`, `bg` | `#FFE6ECF2`, 0 | | Foreground and background |
| `text_scale` | 3 | | Integer glyph scale |
| `fit_text`, `text_min_scale` | false, 1 | 18 | For a single-line label with `wrap_width`, reduce glyph scale to fit, down to the minimum; existing `max_lines` ellipsis then applies |
| `text_center_h` | 0 | 18 | Center the rendered label lines inside this height in canvas pixels; 0 disables it |
| `align` | left | | `left`, `center`, `right` |
| `on_tap` | — | | Action name |
| `on_hold`, `hold_ms` | —, 600 | 13 | `on_hold` is an action run once when a single finger rests on the widget for `hold_ms` milliseconds (absent or ≤ 0 means 600). The target is the topmost visible `on_hold` widget under the first finger, chosen independently of `on_tap`: a large `on_hold` area under small buttons gets the hold while the buttons keep their taps. An `input_block` above it blocks the hold. It is not armed during a page transition; through runtime 15 it was also not armed on a draggable widget with a payload (runtime 16 allows hold and drag on one widget, §3.13). Moving more than the 12 px tap slop, a second finger, a drag, a page transition or any page change cancels it. The lift that ends a fired hold is not a tap. A still finger on a map or scroll list can hold; after the hold fires the finger may still pan or scroll. `{i}` is substituted in repeat templates, as for `on_tap` |
| `on_swipe_left`, `on_swipe_right` | — | 14 | Action run once when a finger that landed on the widget travels sideways and lifts (§3.13) |
| `on_swipe_up`, `on_swipe_down` | — | 15 | The same on the vertical axis (§3.13) |
| `swipe_px` | 60 | 14 | Minimum swipe travel in canvas px (≤ 0 means 60) |
| `hide_bind`, `keep_min`, `keep_max`, `hide_eq`, `need_bind` | — | | Visibility. `hide_eq` must be an integer; a string there **throws** |
| `tap_block`, `input_block` | false | | Absorb touches |
| `x_bind`, `y_bind`, `x_scale`, `y_scale` | — | | Offset the widget by a published value |
| `anim` | — | | Animation (`ParseWidgetAnim`): `bind` (required gate), `group`, `from` (right/left/top/bottom/fade/`widget:<id>`), `ms` (≤5000), `easing`, `box` |
| `haptic` | — | | Per-widget haptic override |
| `outline`, `outline_px` | none, 2 | 15 | Game-font text: an outline ring of `outline_px` px (0–12) in the `outline` colour (§3.14) |
| `rise` | 0 | 15 | Game-font text: digit *i* is drawn *i* × `rise` px higher (−8 to 8) (§3.14) |
| `color_markup` | false | 14 | Label/value: `{c:#AARRGGBB}…{/c}` colour spans in the text (§3.14) |
| `outline_copy` | false | 15 | Label/value: this widget is an outline or shadow copy of another (§3.14) |
| `auto_w`, `pad` | false, 0 | 17 | Label/button: the width follows the text plus `pad` px on each side (§3.14) |

**Per type**

| Type | Extra keys |
|---|---|
| `value` | `names` (index → string), `table`, `pad`, `div`, `mul`, `add`, `suffix`, `max_bind` / `max` / `max_const`, `max_sep`; `group`, `group_sep` (17, §3.14) |
| `label` | `bind_text` (string point), `text_src` (`msbt:<alias>#<label>`), `text_bind` + `text_map` (int → text or `msbt:`), `wrap_width`, `max_lines`, `line_gap`, `icon_style`; with `auto_w` (17), `bg` and `pill` draw a box or capsule behind the text |
| `rect` | `frame` (outline px, default 2), `pulse` |
| `button` | `pill`; `border` (outline px, default 3, since 15), `text_inset` (a left-aligned caption's inset, default 12, since 15) |
| `bar` | `bind`, `max_bind` / `max`; `frame` (outline px in `color`, default 2, `0` = none, since 15); `fill_dir`, `image` (16, §3.15) |
| `pips` | `bind`, `max_bind` / `max`; `gap` (px between pips, since 15; default −1 = 8 for sprite pips, a third of a drawn pip); `tint`, `tint_bind`, `tint_colors` (16, §3.15) |
| `image` | `src`, `src_bind`, `src_names`, `src_thresholds` (`[{le, src}]`), `src_format` (printf of the value), `src_rect` (normalised UV), `empty_src`, `empty_rect`, `fill_bind`, `flip_x`, `flip_y`, `spin` (deg/s), `shake`; `rotate`, `rotate_bind`, `pivot`, `scale_bind`, `tint`, `tint_bind`, `tint_colors`, `fill`, `slice` (16, §3.15) |
| `map` | `area`, `area_bind`, `room_bind`, marker keys (`marker_x_bind`, `marker_y_bind`, `marker_icon`, …), actor keys, `follow_window`, `label_offset` (15). Map-only (`ParseMapWidgetExtras`): `groups`, `label_style`, `on_map_tap`, `on_marker_tap`, `marker_hit_px`, `marker_tap_groups`; `image_bind`, `overlays`, `view_rect_*_bind`, `view_rect_pad` (14). See §3.12 |
| `chart` | `bind`, `samples`, `interval_ms`, `style`, `min`, `max`, `color`, `bg` (17, §3.16) |
| any | Pan/zoom: `pan_zoom`, `min_zoom`, `max_zoom`, `view_idle_ms`; on a non-map `pan_zoom` widget, `view_zoom_bind`, `view_cx_bind`, `view_cy_bind`, `view_reset_bind` (15, §3.15) |
| any | Repeat: `repeat`, `repeat_bind`, `repeat_div`, `repeat_dx`, `repeat_dy`, `repeat_cols`, `repeat_row_dy`, `pack`. `{i}` becomes the element index (see below) |
| any | Scroll: `scroll`, set to a region id or an inline region object |
| any | Drag and drop: `payload`, `select_group`, `draggable`, `drag_scale`, `drop_action`, `accept_group`, `highlight_src`, `highlight_color`, `drag_under_*` |

**`{i}` in repeat templates.** `{i}` is the element index, and `{i+N}` / `{i-N}` the index offset
by a constant (a payload that also encodes a tab: `"{i+1000}"`). Since runtime 17 it is replaced in every string field of a
repeat template, except three that cannot vary per element: `scroll` (it places the template),
the page-level `anim` group, and the map widget's shared extras (`groups`, `overlays`,
`label_style` and the tap keys). Earlier runtimes replace it only in a subset:

| Runtime | Fields where `{i}` is replaced |
|---|---|
| ≤ 14 | `text`, `bind`, `max_bind`, `bind_text`, `x_bind`, `y_bind`, `src`, `src_bind`, `fill_bind`, `on_tap`, `on_hold`, the swipe actions, `id`, `hide_bind`, `need_bind`, `payload`, `select_group`, `accept_group`, `drop_action`, `highlight_src`, `drag_under_src`, `text_src`, `text_bind`, and `keep_min` / `keep_max` in the `{i}` form |
| 15 | adds `src_names`, `empty_src`, `suffix`, `max_sep`, `table` and the `text_map` values |
| 16 | adds `rotate_bind`, `scale_bind`, `tint_bind` and a bar's `image` |
| 17 | adds `names`, `hidden_icons`, `src_thresholds` sources, `src_format`, `group_sep`, `area`, `area_bind`, `room_bind`, the marker and actor keys and the `view_*_bind` keys |

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
| `button` | `button` (A B X Y L R ZL ZR Plus Minus DUp DDown DLeft DRight; a stick shove `LX+`, `LY-`, `RX+`, …; since runtime 15 a two-button chord such as `"L+R"`, both pressed on the same tick), `frames` (4), `repeat`, `counter`, `target`, `modulo`, `delta`, `button_neg`, `gap` (18 ticks), `hold_map` | `{"kind":"button","button":"Minus","frames":8}` (HollowKnight) |
| `page` | `page`, `transition` (slide_up, slide_down, grow, shrink, fade, none), `duration_ms` (≤2000), `easing`, `shadow`, `origin`. Since runtime 17, `page` may be `"@settings"` (the built-in settings page) or `"@back"` (the page shown when `@settings` opened; §3.18) | `{"kind":"page","page":"map","transition":"fade","duration_ms":200,"easing":"ease_in"}` (LinksAwakening) |
| `module` | `action` (string passed to the module's `on_action`), `argument`. Since runtime 16, a module that returns false (or throws) **refuses** the action: the `refused` haptic plays and nothing after it runs | `{"kind":"module","action":"calendar_select","argument":"$payload","enabled_bind":"calendar.ready"}` (Persona5Royal) |
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
- **`haptics` kinds:** `tap`, `write`, `select`, `drag`, `drop`, `marker`, `refused`; since
  runtime 13, `hold` (default `heavy`); since runtime 14, `swipe` (default `light`). A hold or
  swipe kind plays when its action ran; a refused action plays `refused` instead. Widget and
  action `haptic` overrides do not apply to holds or swipes. Since runtime 17, `haptics.nav`
  sets the controller-focus move haptic, like `nav.haptic` (§3.17).
- **`haptics` strengths:** `off`, `light`, `click`, `confirm`, `heavy`, `reject`.

### 3.10 Derived values

`derived` is an array, parsed by `ParseDerived`. Entries may appear in both the manifest and
the data file; a data-file entry with the same `name` replaces the manifest entry. Each entry has
a `name` and exactly one form. Evaluation checks the forms in this order:
`any_eq` > `select` > `hold_last_nonzero` > `cmp` > `all_nonzero` / `any_nonzero` > `countdown`
> `expr` > `terms`.

| Form | Since | Keys | Result |
|---|---|---|---|
| terms | | `terms` (`"src"`, `["src", factor]` or `{point, factor}`), `add`, `floor` / `round` | Σ value·factor + add |
| select | | `select`, `then`, `else` | The `then` value while `select` is non-zero, else the `else` value |
| hold | | `hold_last_nonzero`, `hold_gate` | The last non-zero value |
| any_eq | | `{array, count, value}` | Count of `array0..N-1` equal to `value` |
| cmp | | `cmp` (eq, ne, ge, gt, le, lt), `a`, `b` (name or constant) | 1 or 0 |
| all_nonzero / any_nonzero | | list of names | 1 or 0. Fails closed if any source is missing |
| countdown | 16 | `countdown` (constant or name), `now` (name, default `"@clock.epoch"`) | max(0, target − now); missing when either side is missing |
| expr | 17 | `expr` (string), `floor` / `round` | The expression's value (below) |

An older runtime ignores `countdown` and `expr`; such an entry then has no form and publishes
`add` (default 0). Declare the matching `min_runtime`.

**`expr` (runtime 17, `mod_expr.cpp`).** The expression is compiled once at load.

- **Operators**, lowest precedence first: `?:` (right-associative), `||`, `&&`, `==` `!=`,
  `<` `<=` `>` `>=`, `+` `-`, `*` `/` `%`, unary `!` `-` `+`, and parentheses.
- **Numbers.** Literals such as `12`, `2.5`, `.5`, `1e3`, `0x1F`. Every value is a 64-bit float.
  `/` is real division (use `floor(...)` or `"floor": true` for integer division); `/` and `%` by
  zero give 0; `%` keeps the dividend's sign.
- **Logic.** Comparisons and logic give 1 or 0, with the same 1e-6 tolerance as `cmp`. `&&`,
  `||` and `?:` short-circuit.
- **Functions:** `min(a, b, …)`, `max(a, b, …)`, `abs`, `clamp(x, lo, hi)`, `floor`, `ceil`,
  `round`.
- **Names.** Any published value: points (including array elements such as `items3`), derived
  values, module outputs, `@flag:<name>`, `@sel:…`, `@clock.*`, `@game.seconds`. A bare name
  matches `[A-Za-z_][A-Za-z0-9_.]*`, or `@` followed by `[A-Za-z0-9_.:]*`; quote anything else in
  single quotes (`'view_custom:map'`). Put spaces around a ternary `:` after an `@` name
  (`@flag:x ? 1 : 0`).
- **Missing names.** A name that is not published makes the result missing, as for every other
  form, unless a short-circuit or the untaken ternary branch skips it.
- **Order.** An expression may read derived values declared before or after it; the order is
  resolved once per manifest.
- **Errors.** A parse error is logged once at load (`DSMod: derived '<name>': expr "...": <error>
  at column N (publishes 0)`) and the value publishes 0. Limits: 4096 characters, 1024 nodes,
  nesting depth 64.

```json
{"name":"pieces_hearts","terms":[["heartpiece_count",0.25]],"floor":true}          // LinksAwakening
{"name":"max_hearts","terms":[["container_count",1],["pieces_hearts",1]],"add":3}  // LinksAwakening
{"name":"bt.focus","select":"bt.tsel","then":"bt.f0","else":"bt.f2"}              // Persona5Royal
{"name":"bt.fge","cmp":"ge","a":"bt.focus","b":0}                                 // Persona5Royal
{"name":"event_left","countdown":"event_end_epoch"}                               // runtime 16
{"name":"boot_timer","countdown":300,"now":"@game.seconds"}                        // runtime 16
{"name":"hp_pct","expr":"hp * 100 / max(hp_max, 1)","floor":true}                 // runtime 17
{"name":"night","expr":"@clock.hour >= 19 || @clock.hour < 6"}                    // runtime 17
```

The runtime 16 and 17 lines are illustrations, not from a shipped package.

### 3.11 Image, font and data sources

| Prefix | Since | Resolved by | Notes |
|---|---|---|---|
| `file:<path>` | | Package file | Relative to `dualscreen/` |
| `romfs:/<path>[#member…]` | | Nx asset worker | The **player's own** game files (with the update and LayeredFS applied). Container paths: `x.arc#member#tex` (SARC→BNTX), `x.bntx#tex`, `x.bffnt`, `.lzs#member.xtx`, `.bctex`, `.dds`, `.png`. Since runtime 16 a romfs that was not ready yet is opened again on a later read (at most once a second) |
| `base:/<path>` | 15 | Asset sources | The program romfs as the base game ships it, without the update or LayeredFS. Files the update dropped are still there |
| `aoc:/<path>` | 15 | Asset sources | The add-on content (DLC) data romfs the game mounts. Missing when no DLC is installed or it is disabled |
| `user:<path>` | 17 | Asset sources | A file the player put in `<Eden data>/dualscreen/user/<TITLEID>/<path>` (below) |
| `module:<key>` (image) | | Module `load_image` | Asynchronous: null until the module delivers the image. The module defines the key format (≤4096 characters since runtime 16, 256 before) |
| `module:<key>` (bytes) | 12 | Module `load_data` | Any byte read through `ReadAssetBytes`: map `geo` and layer blobs, `map.areas_src`, the `font` metrics. May block while the module generates the data; empty if no module serves the key |
| `composite:<name>` | | `composites` | Layered image built by the runtime |
| `map:<area>@<w>x<h>` | | Map rasteriser | |
| `icon:` | | Title icon | |

- **One registry.** `file:`, `romfs:`, `base:`, `aoc:`, `user:` and `module:` are resolved by one
  `AssetSources` registry (`mod_sources.cpp`), so every prefix works wherever a source string
  works: image `src`, composite layers, map data, fonts, and a module's `read_romfs`. `#member`
  paths work in the directory sources.
- **Failure is quiet.** An unknown prefix, an unavailable source (no DLC) and a missing file all
  read as missing, so the widget shows its fallback (`empty_src`, nothing).
- **`user:` rules (runtime 17, `mod_user_source.cpp`).** Read-only. `<TITLEID>` is 16 upper-case
  hex digits, and the runtime creates the folder on first use so players can find it. A leading
  `/` is ignored. Paths with `..`, `.` or empty components, `\`, `:`, NUL, or a link resolving
  outside the folder read as missing, as does a file over 32 MiB. The folder is
  `Android/data/<app id>/files/dualscreen/user/<TITLEID>/` on Android,
  `~/.local/share/eden/dualscreen/user/<TITLEID>/` on Linux and
  `%APPDATA%\eden\dualscreen\user\<TITLEID>\` on Windows. Use it for material the player supplies
  (a portrait, a translation): a package cannot put files there, it only reads what the player
  added.

```json
{"type":"image","rect":[20,20,200,200],"src":"user:portraits/hero.png","empty_src":"file:portrait_blank.png"}
```

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
  Otherwise the runtime uses a built-in parser (MFNT, the built-in legacy font layout, or BFFNT).

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

**Map widget extras (runtime 14, `ParseMapWidgetExtras`).**

| Key | Type | Default | Description |
|---|---|---|---|
| `image_bind` | text point | — | Names the area's base picture (any image source), drawn over the area's world box like the area's own `image` and overriding it. Empty or missing = the area's own |
| `overlays` | array | — | Pictures placed in world space, over the base picture and under the markers; they pan and zoom with the map. Each has `src` or `src_bind` (a text point naming the key; wins when set), the world box `x0`, `y0`, `x1`, `y1` (the top edge is at the larger y), `show_bind` (gate) and `opacity` (0–1, default 1). An entry without a source or with an empty box is ignored with a warning |
| `view_rect_x0_bind`, `view_rect_y0_bind`, `view_rect_x1_bind`, `view_rect_y1_bind` | names | — | A bound default view in world units: when all four resolve, the base view (zoom 1, no pan) fits that rect instead of the whole area. Only some of the four: ignored with a warning |
| `view_rect_pad` | number | 0 | Padding around the view rect, in world units |
| `label_offset` | `[right, up]` px | `[12, 24]` | Since runtime 15: the area name's offset from the widget's bottom-left corner |

```json
{"type":"map","rect":[0,0,1240,1080],"area":"overworld","pan_zoom":true,
 "image_bind":"map.base_key",
 "overlays":[{"src_bind":"map.weather_key","x0":0,"y0":0,"x1":4096,"y1":4096,"show_bind":"weather.on","opacity":0.6}],
 "view_rect_x0_bind":"party.min_x","view_rect_y0_bind":"party.min_y",
 "view_rect_x1_bind":"party.max_x","view_rect_y1_bind":"party.max_y","view_rect_pad":64,
 "on_map_tap":"map_tapped"}
```

A map tap publishes `@map_tap_x` / `@map_tap_y` and bumps `@map_tap_seq` (runtime 14), which a
module can read to tell a new tap from the last one.

**Per-slot dynamic markers (runtime 14).** A `dynamic_markers` entry can style each slot from
array points, with `{i}` replaced as in `x` / `y` / `kind`:

| Key | Default | Description |
|---|---|---|
| `icon_src_bind` | — | Text point naming an image key; when non-empty the slot draws that picture instead of its atlas icon (a portrait). A picture still loading draws nothing |
| `size_world` | 0 | Marker size in world units, so it scales with zoom; 0 = the pixel `size` |
| `bar_bind`, `bar_max_bind`, `bar_max` | —, —, 100 | A bar under the icon: value / max |
| `bar_color`, `bar_bg`, `bar_h` | green, `#C0000000`, 0 | Bar colours; `bar_h` px tall (0 = an eighth of the icon, at least 3) |
| `dim_bind` | — | Gate: the slot draws dimmed |
| `frame_color_bind`, `frame_px` | —, 0 | Int point holding an ARGB frame colour (0 or missing = no frame); thickness in px (0 = a sixteenth of the icon, at least 2) |
| `tint_bind` | — | Int point holding an ARGB colour that multiplies the icon (0 or missing = none) |

```json
{"group":"units","count":24,"x":"unit{i}.x","y":"unit{i}.y","icon_src_bind":"unit{i}.portrait",
 "size_world":48,"bar_bind":"unit{i}.hp","bar_max_bind":"unit{i}.hp_max","dim_bind":"unit{i}.done",
 "frame_color_bind":"unit{i}.team_color"}
```

**`map.style` atlas naming, blink and pin keys (runtime 15).** The defaults are the conventions of
the Metroid Dread minimap atlas, so a package that sets none of them draws as before. A marker's
own `open_icon`, `collected_icon`, `collectible` or `structural` still wins.

| Key | Default | Description |
|---|---|---|
| `door_prefix` | `"Door"` | Icons starting with this are doors |
| `structural_prefixes` | `["Door", "Blockage"]` | Icons starting with any of these are structural (drawn at `door_icon` size) |
| `door_closed_suffix`, `door_open_suffix` | `"Closed"`, `"Open"` | An opened door `<base><closed>` shows `<base><open>` |
| `door_opened_left`, `door_opened_right` | `"DoorOpenedL"`, `"DoorOpenedR"` | The opened cell for a door ending in `L` / `R` |
| `collected_suffix`, `collected_fallback` | `"Adquired"`, `"ItemAdquired"` | A collected item shows `<icon><suffix>`, else the fallback (the spelling is the atlas's) |
| `collectible_kind` | `"Items"` | Markers of this kind are collectibles |
| `item_blink_period`, `item_blink_low` | 72, 0.45 | Uncollected-item blink: period in ticks, lowest alpha |
| `player_blink_period`, `player_blink_low` | 48, 0.55 | The atlas player marker's pulse |
| `pin_outer`, `pin_inner`, `pin_core` | 22, 18, 8 | The fallback player pin (no atlas cell): two diamonds of these radii and a square core, px (0–256) |

### 3.13 Gestures: swipes, hold and drag (runtimes 14–16)

**Swipes** (`mod_input_swipe.h`). A widget with `on_swipe_left` / `on_swipe_right` (runtime 14)
or `on_swipe_up` / `on_swipe_down` (runtime 15) runs that action once when a single finger that
landed on it travels at least `swipe_px` (default 60) along one axis and lifts.

- **Direction lock.** When the finger leaves the 12 px tap slop, a start with |dx| > 2|dy| is a
  horizontal candidate and |dy| > 2|dx| a vertical one, each only if the widget has an action on
  that axis. The swipe then owns the gesture; anything else disarms it, so vertical drags keep
  scrolling lists. A vertical swipe is never armed over a scroll region that can scroll.
- **Speed.** The distance must be reached within 600 ms of the lock. It fires at the lift, if the
  lift point still qualifies; that lift is not a tap.
- **Target.** The topmost swipe widget under the finger, chosen independently of `on_tap` (a
  card-sized swipe area under small buttons). An `input_block`, a map or a `pan_zoom` widget
  above it stops the swipe; a swipe never arms on a draggable widget.
- **Cancelled** by a second finger, a page transition or change, a fired hold, or a drag.
- **Haptic:** the `swipe` kind (default `light`).

```json
{"type":"rect","rect":[0,120,1240,840],"on_swipe_left":"next_tab","on_swipe_right":"prev_tab","swipe_px":80}
```

**Hold and drag on one widget (runtime 16).** A hold can now arm on a draggable widget, either its
own `on_hold` or an `on_hold` widget drawn above it. If the finger leaves the tap slop before
`hold_ms`, the hold is cancelled and the drag starts. If the hold fires first, that touch does
not drag and its lift is no tap. `on_hold` carries no payload; use `{i}` for per-item actions.

```json
{"type":"image","rect":[40,200,96,96],"repeat":20,"repeat_dx":100,"draggable":true,
 "payload":"{i}","select_group":"bag","on_hold":"inspect_{i}","hold_ms":500}
```

**Refused module actions (runtime 16).** A `module` action whose `on_action` returns false (or
throws) is refused: the `refused` haptic plays, nothing after it runs, and the log says
`DSMod: action '<name>' refused: module declined '<action>'`. Combined with `@sel:` / `@drag*`
being published before the taps (§3.3), an `enabled_bind` can judge a drop before it happens:

```json
"derived": [{"name":"drop_ok","cmp":"ne","a":"@drag_payload","b":"bag.held_slot"}],
"actions": {"drop_on_slot": {"kind":"module","action":"bag.move","argument":"$payload","enabled_bind":"drop_ok"}}
```

### 3.14 Text: outline, colour spans, boxes, grouped numbers, paged fonts (runtimes 14–17)

| Key | Type | Default | Since | On | Description |
|---|---|---|---|---|---|
| `outline` | colour | none | 15 | game-font text | An outline ring around each glyph |
| `outline_px` | int px | 2 | 15 | game-font text | Ring width, 0–12; read only with `outline` |
| `rise` | number px | 0 | 15 | game-font text | Digit *i* is drawn *i* × `rise` px higher, like a HUD number that steps upward (−8 to 8) |
| `color_markup` | bool | false | 14 | label, value | `{c:#AARRGGBB}…{/c}` colour spans. Tags must use exactly eight hex digits. Wrapping and `max_lines` never cut a tag, and an open span is closed before the ellipsis |
| `outline_copy` | bool | false | 15 | label, value | Marks an outline or shadow copy of another label: with `color_markup` it lays out the tags like the main copy but draws everything in its own colour |
| `auto_w` | bool | false | 17 | label, button | The width follows the text. The declared `w` is the minimum |
| `pad` | int px | 0 | 17 | label, button with `auto_w` | Padding around the text. On a **value** widget `pad` still means zero-padding |
| `group` | bool | false | 17 | value | Thousands separators (`12,345`, `-1,234`; with `pad: 6`, `1234` shows `001,234`). With `max_sep` + `max_bind` the maximum is grouped too |
| `group_sep` | string | `","` | 17 | value | The separator, e.g. `"."`, `" "` or `""` |

- **`auto_w` on a button.** With `"align": "center"` the box grows about the declared rect's
  centre and is text + 2·pad wide; otherwise it grows from the left edge and is `text_inset` +
  text + `pad` wide. `y` and `h` keep their declared values.
- **`auto_w` on a label.** The box sits around the text at its anchor and reaches `pad` px past
  it on every side; a wrapped label is as wide as its widest line. The label's `bg` is drawn
  behind the text (a capsule with `"pill": true`). Taps, dirty rects and drawing all use the
  measured box.
- **`max_lines`** (older key, behaviour unchanged): the last kept line ends in `…` when the font
  has U+2026, else `...`, trimmed so the ellipsis fits. With `auto_w`, the box measures the
  lines after they are cut.

```json
{"type":"value","rect":[132,48,0,0],"bind":"wonder.lives","text_scale":8,"color":"#FFFFFFFF","pad":2,"outline":"#FF232723","outline_px":4,"rise":0.6}  // SuperMarioWonder
{"type":"label","rect":[40,300,0,0],"bind_text":"quest.title","color_markup":true,"wrap_width":600,"max_lines":2}
{"type":"button","rect":[300,20,0,48],"text":"Items","align":"center","auto_w":true,"pad":18,"pill":true,"bg":"#FF2E7D32"}
{"type":"value","rect":[40,160,0,0],"bind":"wallet.money","group":true,"suffix":" G"}
```

The second label's `bind_text` might hold `"Bring {c:#FFFF4040}3 apples{/c} home"`.

**Paged font atlases (runtime 17, `mod_font_pages.cpp`).** With `{p}` in `font_atlas` and
`font_page_h` > 0, the atlas is split into pages of `font_page_h` rows. Glyph y counts down
through the pages stacked top to bottom (page = y / `font_page_h`), so a module's `decode_font`
still reports y in the virtual stacked atlas.

- Page `p` is loaded from `font_atlas` with `{p}` replaced by `p`: a `module:` image or a package
  or romfs picture. It loads on a worker the first time one of its glyphs is drawn; until then
  that glyph draws blank, and the page repaints when it lands.
- Pages share an LRU capped at 32 MiB. A page drawn within the last 250 ms is never evicted; a
  page that does not fit is retried after 1 s; a failed page is retried at most 5 times.
- Each page must be ≤4096×4096 and ≤16 MiB; 1024×1024 (4 MiB) is a good size.
- Without `{p}` or `font_page_h` the font is one atlas, as before. BFFNT fonts are not paged.

```json
"font":"module:game_font_metrics","font_atlas":"module:game_font_page_{p}","font_page_h":1024
```

### 3.15 Images, bars and views (runtimes 15–16)

All keys are optional; without them a widget draws exactly as before.

| Key | Type | Default | Since | On | Description |
|---|---|---|---|---|---|
| `rotate` | degrees | 0 | 16 | image | Fixed rotation, clockwise, about `pivot` |
| `rotate_bind` | name | — | 16 | image | Degrees added to `rotate` (missing = 0) |
| `scale_bind` | name | — | 16 | image | Scale × 1000 (1000 = 1×), about `pivot`, clamped to 0–16×; missing = 1× |
| `pivot` | `[x, y]` px | rect centre | 16 | image | Rotate and scale centre, from the rect's top-left |
| `tint` | colour | unset | 16 | image, pips, bar `image` | Multiplies the texels. Replaces `color` as an image's tint and as the lit pips' colour (unlit pips still use `bg`); tints a bar's `image` (default white) |
| `tint_bind` | name | — | 16 | same | Index into `tint_colors`; out of range or missing falls back to `tint`, then `color` |
| `tint_colors` | colour array | `[]` | 16 | same | The colours `tint_bind` picks from |
| `fill` | `"stretch"` / `"tile"` / `"slice"` | `"stretch"` | 16 | image, bar `image` | `tile` repeats the source region at its own pixel size from the top-left, clipped; `slice` is a 9-slice |
| `slice` | `[l, t, r, b]` source px | `[0,0,0,0]` | 16 | same | 9-slice insets. Corners keep their size and shrink in proportion when the rect is smaller than two corners |
| `fill_dir` | `"right"` / `"left"` / `"up"` / `"down"` | `"right"` | 16 | bar | The side the bar fills from |
| `image` | image source | — | 16 | bar | A picture for the filled part instead of flat `color`; uses `src_rect`, `fill`, `slice`, `tint`. With stretch or tile the picture spans the bar and the fill reveals it; with slice the 9-slice is laid out on the filled part. Unloadable = `color`. The frame is still drawn in `color` (`frame: 0` turns it off) |
| `view_zoom_bind`, `view_cx_bind`, `view_cy_bind` | names | — | 15 | non-map `pan_zoom` | The view's bound home: zoom (clamped to `min_zoom`–`max_zoom`) centred on (cx, cy) in 0–1 of the picture. All three are required, else ignored with a warning |
| `view_reset_bind` | name | — | 15 | same | A change snaps the view to its home even if the user moved it |

- **Transforms.** `spin` adds to the angle; `flip_x` / `flip_y` mirror the picture before it is
  turned; `src_rect` and pan/zoom crop as before. `fill_bind` (the bottom-up gauge) ignores the
  transform keys. With a transform, the filled picture is built at the rect's size and turned as
  a whole. Taps still use the unrotated rect; the redraw box is the transformed bounding box.
- **Bound home view.** It is where a new view starts, where `view_reset` and `view_idle_ms`
  glide back to, and `view_custom:<id>` is 1 while the view is away from it. When the bound home
  changes, a view still at its old home follows; a view the user moved stays put until
  `view_reset_bind` changes. A map widget uses `view_rect_*_bind` instead (§3.12).
- **Rendering path.** Image, pips, bar and chart always draw on the CPU canvas, also with
  `gpu_composite`, so these keys need no fallback.

```json
{"type":"image","rect":[100,40,8,60],"src":"file:hand.png","rotate_bind":"clock_deg","pivot":[4,56]}
{"type":"pips","rect":[10,10,24,24],"src":"file:heart.png","bind":"hp","max":10,"tint_bind":"hp_low","tint_colors":["#FFFFFFFF","#FFFF4040"]}
{"type":"image","rect":[20,20,300,120],"src":"file:panel.png","fill":"slice","slice":[12,12,12,12]}
{"type":"bar","rect":[20,200,24,120],"bind":"stamina","max":100,"fill_dir":"up","image":"file:stamina_fill.png","fill":"slice","slice":[0,6,0,6],"bg":"#40000000","frame":0}
{"type":"image","rect":[0,0,1240,900],"src":"file:world.png","id":"world","pan_zoom":true,"max_zoom":4,"view_zoom_bind":"world.zoom","view_cx_bind":"world.cx","view_cy_bind":"world.cy"}
```

### 3.16 Chart widget (runtime 17, `mod_chart.h`)

`"type": "chart"` shows the recent history of a value. Each chart has its own ring buffer,
sampled on the tick thread for every page, so the history is already there when the page opens.

| Key | Type | Default | Description |
|---|---|---|---|
| `bind` | name | — | The value to sample (int or float point, derived value, module output, flag). An unresolved sample is skipped |
| `samples` | int | 60 | Ring length, 2–1024. The newest sample is at the right edge |
| `interval_ms` | int | 1000 | Time between samples, 16–3 600 000, on a monotonic clock. After a pause the chart takes one sample, not a burst |
| `style` | `"line"` / `"bar"` | `"line"` | A 2 px line, or one bar per sample slot (1 px gap when there is room) |
| `min`, `max` | number | auto | Vertical range; a missing bound uses the smallest or largest sample shown. A flat series is centred |
| `color`, `bg` | colour | `#FFE6ECF2`, transparent | Line or bar colour; background |

The buffer is keyed by the widget's `id`, else `"<page id>#<index on the page>"`, and
`@chart:<key>` publishes the number of samples taken. A chart inside a `repeat` template is not
sampled.

```json
{"type":"chart","id":"hp_hist","rect":[20,600,400,120],"bind":"hp","samples":120,"interval_ms":500,"min":0,"max":100,"color":"#FF7CFC00","bg":"#40000000"}
```

### 3.17 Controller navigation (runtime 17, `mod_nav.cpp`)

A chord on the game controller toggles a **focus mode** on the companion page. The D-pad (or the
left stick's directions) moves the focus among the page's tappable widgets, repeating after
400 ms and then every 120 ms; **A** taps the focused widget at its centre through the normal tap
path; **B** or the chord again leaves. While the mode is on, the game sees a neutral pad, except
buttons the companion itself presses through a `button` action. The mode also ends when the
second screen goes away; a page change keeps it on and refocuses the first widget. On a page
with nothing to focus the chord does not enter the mode, and the game keeps its pad, chord
included.

**Default.** Without a `nav` key, focus mode is on only for a package whose `min_runtime` is 17
or higher (from `manifest.json` or `package.json`). An older package keeps its controller
behaviour unless it opts in with `"nav": true` or a `nav` object; an object opts in even without
`enabled`.

Tappable means a visible widget with `on_tap`, `select_group`, or `drop_action` +
`accept_group`, whose centre a tap would reach (not under an `input_block` or popup). Scroll-list
rows count; moving past the visible rows scrolls the list by one row.

| Key | Type | Default | Description |
|---|---|---|---|
| `nav` | bool / object | see **Default** | `true` or an object opts in; `false` or `{"enabled": false}` opts out: no chord, no frame, no pad suppression |
| `nav.enabled` | bool | true (inside an object) | As above |
| `nav.toggle` | string | `"LS+RS"` | Two or more buttons joined by `+`: A B X Y L R ZL ZR Plus Minus DUp DDown DLeft DRight (or Up/Down/Left/Right), LS/RS (also L3/R3, StickL/StickR); case-insensitive. Invalid: warning, default kept |
| `nav.color` | colour | `#FFFFC107` | Focus frame colour |
| `nav.frame` | int px | 3 | Frame width; ≤ 0 fills the rect |
| `nav.src` | image source | — | Drawn over the focused rect instead of the frame |
| `nav.haptic` | strength | none | Played when the focus moves (`haptics.nav` works too; needs `haptics` enabled) |
| page `nav_order` | string array | — | Focus order by widget id; a repeat template's own id (`"slot_{i}"`) stands for all its elements. Unlisted widgets cannot be focused. With it, every direction steps through the list (Down/Right next, Up/Left previous, wrapping); without it, the D-pad moves to the nearest widget in that direction |

The default chord is `LS+RS` because games rarely ask for both stick clicks at once, while ZL+ZR
is aim and fire in many games. The `@nav.*` values (§3.3) let a package draw its own cursor:
`@nav.active` (1/0; not published when opted out), `@nav.index` (−1 for none), `@nav.count`, and
the focused rect `@nav.x`, `@nav.y`, `@nav.w`, `@nav.h`.

```json
"nav": {"toggle":"ZL+ZR","color":"#FF4FC3F7","frame":4,"haptic":"light"},
"pages": [{"id":"items","nav_order":["tab_bag","tab_key","slot_{i}","close"],"widgets":[…]}]
```

### 3.18 Settings page and persisted flags (runtimes 15, 17)

**`persist_flags` (runtime 15, `mod_persist.cpp`).** An array of runtime flag names. Each listed
flag is written whenever a flag action changes it, and restored when the package loads, over the
`flags` default. The file is `<Eden data>/dualscreen/persist/<TITLEID>/<package>.json`, where
`<package>` is the manifest `name` (the mod folder name when empty) reduced to `[A-Za-z0-9._-]`.
It lives outside the package folder (so updates keep it) and outside the game's save data.

```json
"flags": {"map.labels": 1}, "persist_flags": ["map.labels"]
```

**`settings` (runtime 17, `mod_settings.cpp`).** A top-level array; each entry becomes one row of
a built-in page with id `@settings`.

| Key | Type | Default | Description |
|---|---|---|---|
| `flag` | string | required | The runtime flag the row changes (read as `@flag:<name>`) |
| `label` | string | the flag name | Row caption |
| `type` | `"toggle"` / `"choice"` | `"toggle"` | A toggle is 0/1; a choice is an index into `choices`. Unknown type: entry skipped |
| `choices` | string array | toggle: `["OFF","ON"]` | A choice needs at least 2 (else skipped); a toggle may rename OFF/ON with exactly 2 |
| `default` | int / bool | 0 | Initial value, clamped to the choices. A value in `flags` wins |

- Settings flags are persisted automatically, as if listed in `persist_flags`.
- The page has a title, a `< BACK` pill and one large row per entry; tapping a row steps to the
  next value (wrapping). It uses the package font when there is one, and has `no_auto_leave`.
- Open it with `{"kind":"page","page":"@settings"}`. BACK runs a page action to `@back`, which
  returns to the page shown when `@settings` opened; packages may use `@back` too.
- No page is generated when the package has no pages or defines its own `@settings` page. Avoid
  the generated ids: page `@settings`, actions `@settings:back` and `@settings:<flag>`, widgets
  `@settings.*`.
- Rows are ordinary buttons, so controller navigation (§3.17) works on them.

```json
"settings": [
  {"flag":"map.dark","label":"Dark map","type":"toggle","default":true},
  {"flag":"hud.size","label":"HUD size","type":"choice","choices":["Small","Medium","Large"],"default":1}
],
"actions": {"open_settings": {"kind":"page","page":"@settings","transition":"fade"}}
```

An older runtime ignores `settings`, and its `@settings` page action does nothing (there is no
such page), so declare `"min_runtime": 17` or keep the button harmless.

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
| Unknown keys | Ignored everywhere; there is no schema warning. This is also how an older runtime treats a newer key (§6) |
| Unknown widget `type` / point `type` / action `kind` | Becomes a label / becomes `s32` / action dropped. **All silent** |
| Unknown transition, easing, haptic or anim `from` | Falls back, with a warning |
| Wrong JSON type for a typed key (e.g. `"pad":"3"`, `"hide_eq":"1"`, a sequence without `steps`) | The parser throws, and **the whole package is rejected** with "DSMod: ignoring invalid package …". Discovery moves on to the next folder |
| JSON syntax error | The folder is skipped; the error is logged |
| Malformed data file | Skipped. The package loads without points |
| `min_runtime` newer than the runtime | The "UPDATE EDEN DUO" page is shown, and scanning stops |

There is no standalone schema. Treat the C++ parser as authoritative: Eden Duo exposes it to
tools as `ParseDualScreenManifest` and `IsUsableDualScreenManifest` (`mod_runtime.h`), and the
surest check is to boot the game and read the log. `tools/build_dualscreen_package.py` checks the
package metadata and every `file:` reference, but not the manifest's semantics.

## 6. Runtime history and `min_runtime`

`DualScreenRuntimeVersion` is currently **18** (Eden Duo 1.1.0). What each version added to the
package format:

| Runtime | Added for packages |
|---|---|
| ≤ 10 | Unversioned: grow/shrink transitions, `page_binds` |
| 11 | Page `scrolls`, `module:` composite layers, `min_runtime` gating |
| 12 | `module:` byte sources (`load_data`), `map.areas_src` |
| 13 | `on_hold`, `hold_ms`, the `hold` haptic |
| 14 | `on_swipe_left` / `on_swipe_right`, `swipe_px`, the `swipe` haptic; map `image_bind`, `overlays`, `view_rect_*_bind`, `view_rect_pad`, per-slot dynamic-marker keys; `@map_tap_seq`; `color_markup` |
| 15 | `base:` and `aoc:` sources; `module_tick_hidden`; `persist_flags`; `on_swipe_up` / `on_swipe_down`; `view_zoom_bind` / `view_cx_bind` / `view_cy_bind` / `view_reset_bind`; `outline_copy`; button `border` / `text_inset`, pips `gap`, bar `frame`, map `label_offset`; the `map.style` door, collectible, blink and pin keys; more `{i}` fields. Also first released here: `outline` / `outline_px` / `rise` and `"L+R"` button chords |
| 16 | Hold and drag on one widget; refused module actions; `@sel:` / `@drag*` published before taps; image `rotate` / `rotate_bind` / `pivot` / `scale_bind`, `tint` / `tint_bind` / `tint_colors`, `fill` / `slice`; bar `fill_dir` / `image`; `@clock.*`, `@game.seconds`, derived `countdown` |
| 17 | `chart` widget; derived `expr`; `auto_w`, value `group` / `group_sep`; `{i}` in every repeat field; `font_page_h`; `nav`, page `nav_order`, `@nav.*`; `user:` source; `settings`, the `@settings` page and the `@back` target |
| 18 | Font refresh epochs; fitted/centered labels; image scrollbars; marker size caps; parser and rendering fixes |

Runtimes 14 and 15 were first released together, so the outline, rise and chord keys, whose exact
version the source does not record, are listed under 15.

**Guidance.**

- A package that uses a key from runtime N should declare `"min_runtime": N` (in `manifest.json`
  and/or `package.json`; the larger wins). Declare the highest N among the keys you use.
- An older runtime ignores keys it does not know, so a newer key there degrades silently: a
  swipe does nothing, a `chart` is an empty label, an `expr` publishes 0. It shows the "UPDATE
  EDEN DUO" page only when `min_runtime` is higher than its own version; a package without
  `min_runtime` is loaded as far as that runtime understands it.
- Only declare a higher `min_runtime` when the package depends on the feature. A purely cosmetic
  key (an outline, a tint) may be left to degrade on older runtimes.
- Runtimes before 11 ignore `min_runtime` itself.


### Runtime 18 layout details

Widget `rect`, `repeat_dx`, `repeat_dy` and `repeat_row_dy` accept decimal numbers rounded to the nearest canvas pixel (half values away from zero). This does not change integer memory addresses. Null label `text` reads as an empty string. `need_bind` accepts a leading `!`, including in repeat templates, consistently for drawing, taps and dirty tracking. Images may use `src_format`, `src_names` or `src_thresholds` without a placeholder `src`.

A scroll region accepts `bar_src` (thumb image) and `bar_track_src` (track image). Its existing colours remain fallback/background values. Dynamic map markers accept `size_max`: a positive canvas-pixel cap applied after zoom; 0 keeps their previous size behavior.

A module font decoder can refresh its glyphs by publishing integer `__font_epoch`. Missing means 0. A changed epoch starts a bounded decode retry, keeping the previous font until decoding succeeds. The host invalidates matching atlas pages and stale worker results after success. `EDEN_DSMOD_CAP_FONT_EPOCH` is a host capability; modules must not require it in their own capability mask. Set `min_runtime` to 18 if the package depends on these features. Eden Duo 1.1.0 includes runtime 18; Eden Duo 1.0.2 includes runtime 15.
