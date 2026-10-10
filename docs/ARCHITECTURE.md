# Eden Duo dual-screen architecture

This document covers how a sampled guest value becomes pixels on the second screen, and how a
touch travels back. It names components by class and function. Read it after
[PORTING_A_GAME.md](PORTING_A_GAME.md) and [PACKAGE_FORMAT.md](PACKAGE_FORMAT.md), when you need
to know why the runtime behaves as it does (see [CONTRIBUTE.md](CONTRIBUTE.md) for the reading
order). All paths are in the
[Eden Duo](https://github.com/igawa6/eden-duo) repository; the runtime lives in
`src/core/mods/`, split by area:

| File | Holds |
|---|---|
| `mod_runtime.h`, `mod_runtime.cpp` | `ModRuntime`, `DualScreenRuntimeVersion`, `Initialize` and the per-frame `Tick` |
| `mod_manifest.cpp` | `Discover`, the `min_runtime` gate, `ParseManifestJson` and the other `Parse*` helpers, console `reload` |
| `mod_state.cpp` | `SampleState`, `ResolvePoint` / `ReadPoint`, `EvaluateDerived`, `ApplyEnforceRules` |
| `mod_input.cpp` | `UpdateGestures`, `DrainTaps`, `FlushHaptic`, `PublishMapState`, `PublishScroll`, `PublishInteraction` |
| `mod_input_hold.h`, `mod_input_swipe.h`, `mod_input_drag.h` | Press-and-hold, swipe and drag trackers, kept free of `ModRuntime` for unit tests |
| `mod_nav.cpp`, `mod_types_nav.h` | Controller focus mode (runtime 17); the HID side is `src/hid_core/resources/npad/dsmod_pad_gate.h` |
| `mod_clock.cpp`, `mod_expr.cpp`, `mod_chart.h` | `@clock.*` / `@game.seconds` and `countdown` (runtime 16); the `expr` compiler and the chart sampler (runtime 17) |
| `mod_settings.cpp`, `mod_persist.cpp` | The built-in `@settings` page (runtime 17); persisted flags (runtime 15) |
| `mod_view_default.h`, `mod_map_view_rect.h` | Bound default views: non-map `pan_zoom` widgets (runtime 15) and the map's view rect (runtime 14) |
| `mod_actions.cpp` | `RunAction` and action operands |
| `mod_pages.cpp` | `DrivePageBinds`, `DrivePageTransition`, widget-group animations |
| `mod_redraw.cpp` | `UiSignature`, per-widget dependency hashes, `BuildRenderExtras`, `PublishUi`, `DispatchRedraw`, the `DSModRedraw` worker |
| `mod_ui.cpp` and `mod_ui_*.cpp` | `RenderPage` and the widget drawing: canvas, text, image, map widget, scroll, expansion and hit-testing, transitions, widget state |
| `mod_assets.cpp` | `ReadAssetBytes`, source registration, font loading, the image cache |
| `mod_sources.cpp`, `mod_romfs_sources.cpp`, `mod_user_source.cpp` | The `AssetSources` registry: `file:`, `romfs:`, `module:`, `base:` and `aoc:` (runtime 15), `user:` (runtime 17) |
| `mod_font_epoch.h` | Runtime 18 font refresh requests and atlas-page matching |
| `mod_font_pages.cpp` | Paged font atlases loaded on demand (runtime 17) |
| `mod_map.cpp` | Fog of war, map geometry and the `map:` rasteriser |
| `mod_nx_runtime.cpp`, `mod_nx_assets.cpp`, `mod_msbt.cpp` | Nintendo asset references, composites, the Nx asset worker, MSBT text |
| `engine_mercury.cpp`, `engine_il2cpp.cpp`, `engine_ichigo.cpp`, `engine_formats.h` | Per-engine code: Mercury (Metroid Dread), Unity IL2CPP lookups, Ichigo |
| `mod_guest_bridge.cpp` | Guest calls, spies, patches, symbol resolution (Dynarmic only) |
| `mod_module.cpp`, `mod_module_host.cpp`, `mod_module_services.cpp` | Module loader, host API, and the extensions, module images, module data and module map areas |
| `mod_load_plan.cpp` | Load-time patches and the guest mailbox |
| `mod_types*.h` | Manifest and snapshot types |
| `mod_console.cpp`, `mod_re_tools.cpp` | Dev tools (only with `EDEN_DSMOD_BUILD_DEV_TOOLS=ON`) |

## 1. Lifecycle

1. **Registration.** When the NSO loader maps the executable named `main`, it calls
   `System::RegisterDualScreenMod(build_id, base, size)`. Other modules such as `rtld` and `sdk`
   are ignored, so `main+X` offsets and the build ID always refer to the game itself. The NRO
   loader registers homebrew too, with an all-zero build ID, so such a package uses `data.json`.
2. **Discovery.** `ModRuntime::Discover(system, title_id, build_id)` (`mod_manifest.cpp`):
   - scans `load/<TITLEID>/*/dualscreen/manifest.json` in name order, skipping disabled add-ons;
   - checks `title_id` (when present);
   - applies the `min_runtime` gate (§4);
   - checks `format == 1`;
   - parses the manifest (`ParseManifestJson`);
   - loads the per-build data file (`<BUILD16>.json`, then its lower-case form, then
     `data.json`).
   The first usable folder wins. A title with no package gets `IdleManifest`, which shows the
   game icon, dimmed, on black.
3. **Start.** `ModRuntime::Initialize()`:
   - creates the CoreTiming event `"DSMod::Tick"`;
   - loads the native module (`InitializeGameModule`, `mod_module_host.cpp`), which also starts
     the module asset worker and, for a package with `map.areas_src`, the module map-areas fetch
     (`StartModuleAreas`, §2.10);
   - seeds runtime flags from `flags` (and `settings` defaults), then restores `persist_flags`
     from their file (runtime 15);
   - resolves the GPU-composite mode;
   - disables the guest-call bridge when NCE is on.
4. **Shutdown.** The runtime is reset when the process shuts down. The module map-areas thread
   and the module's asset worker are joined before `destroy()` runs, and the library is unloaded
   after that.

## 2. Per-frame data flow

```
 CoreTiming "DSMod::Tick" (fixed 60 Hz, ScheduleLoopingEvent)
   │
   ├─ InstallModuleAreas (once, when module-generated map areas are ready; §2.10)
   ├─ DrainModuleImages, DrainGuestBridgeResults, headless input drivers
   ├─ aux screen not present? → focus mode off; module tick only if it ticks hidden (§2.2) → return
   │
   ├─ SampleState ─────────── points (pointer chains) → ints/floats/texts/addresses
   │                          spies, sequence outputs, counters (@name)
   │                          module sample()  → publish_* into the same snapshot
   ├─ touch capture ───────── AuxRouting::GetTouch (+ console synthetic drags)
   ├─ module tick()           runtime flags → @flag:<name>, @clock.* / @game.seconds (if used)
   ├─ EvaluateDerived (pass 1, cached dependency order)
   ├─ ApplyViewDefaults, view_custom:*, PublishMapState, PublishScroll (fling physics)
   ├─ UpdateGestures ──────── tap / hold / swipe / pan / pinch / drag-and-drop / scroll
   ├─ UpdateNav ───────────── controller focus mode; its A press queues a tap
   ├─ (taps queued?) PublishInteraction + volatile derived: @sel / @drag* before the taps
   ├─ DrainTaps ───────────── hit-test (last frame's draw records) → RunAction
   ├─ FlushHaptic (≤1 per tick)
   ├─ PublishInteraction, EvaluateDerived (pass 2: volatile entries only)
   ├─ ApplyEnforceRules, DrivePageBinds
   ├─ chart sampler (every chart widget, every page)
   └─ PublishUi
        ├─ PumpNxAssets (decoded romfs art lands)
        ├─ throttle: every 2nd tick (≈30 Hz) unless an animation needs 60
        ├─ UiSignature unchanged? → nothing drawn, nothing published (idle = 0 Hz)
        ├─ page transition? → DrivePageTransition renders both pages on the tick thread
        ├─ BuildRenderExtras → per-widget dependency hashes → ≤4 dirty rects
        └─ DispatchRedraw(RedrawJob) ──► "DSModRedraw" worker
                                          RenderPage → CPU canvas (+ AuxDrawList)
                                          ├─ PublishUi / PublishUiPartial (tile diff)
                                          └─ PublishGpuComposite (map quads + textures)
                                                    │
                            GPU thread: RendererVulkan::Composite
                              RenderAuxModUi (upload dirty tiles, blit)  or
                              RenderAuxModUiGpu (quad pipeline)
                              → aux swapchain → own present thread → second display
```

### 2.1 Tick

The tick rate is fixed: `ModTickHz = 60`, and the event is registered with
`CoreTiming::ScheduleLoopingEvent`. The manifest's `poll_hz` is legacy. A value other than 60
only logs a warning.

In multi-core mode the callback runs on CoreTiming's host timing thread while the timing lock is
held, so **every microsecond the tick spends delays other timed events**. This is the main
reason for the worker split in §2.6. Single-core mode also runs the callback from the timing
advance, but that path has not been profiled separately.

### 2.2 Sample

`SampleState` (`mod_state.cpp`) fills a `StateSnapshot`. This is a set of `SnapshotMap`s (hash maps keyed by name)
for `ints`, `floats`, `texts` and `addresses`. The snapshot is **cleared every tick**, so every
value must be republished on every tick.

| Source | Mechanism |
|---|---|
| Points | `ResolvePoint` walks pointer chains, then `ReadPoint` / `ReadPointText` read the value. Arrays expand to `name0..nameN-1`. |
| Spies | Values captured by a guest breakpoint, drained from `pending_spy_values`. Dynarmic only. |
| Sequences | Results of guest-call chains (`sequence_*`). Dynarmic only. |
| Counters | Button-action counters, published as `@<counter>`. |
| Module | `api->sample(instance, host)`, called through `RunGameModule` (`mod_module_host.cpp`). The module calls `publish_i64/f64/text/address/map`. |

The module's `tick()` runs after touch capture. Runtime flags are then published as
`@flag:<name>`, followed by the clock points (`@clock.*`, `@game.seconds`; runtime 16), so
derived values can read both. The clock points are published only when the manifest or data file
mentions them (`JsonReferencesClockKeys`: a string or key containing `@clock.` or `@game.`, or a
`countdown` entry), because they change every second and would otherwise churn `UiSignature`. A
native module reading them by name in a package that never mentions them gets nothing.

- **Hidden screen.** While the second screen is absent, only the module's `tick` runs, and only
  if `module_tick_hidden`, the module's tick flags, or (by default) an `on_action` export says so
  (runtime 15; [MODULE_GUIDE.md §1.2](MODULE_GUIDE.md#12-lifecycle-and-threads)).

### 2.3 Derived values

`EvaluateDerived` computes the `derived` entries (see
[PACKAGE_FORMAT.md](PACKAGE_FORMAT.md#310-derived-values)).

- The dependency order is computed once (`derived_order`), and entries may reference each other
  in any order.
- A second pass after interaction recomputes only **volatile** entries, meaning those that read
  `@...` interaction values or `view_custom:` values. The clock points do not make an entry
  volatile.
- Since runtime 16, on a tick with queued taps or drops the runtime runs `PublishInteraction`
  and the volatile pass once **before** the taps too, so an `enabled_bind` judges a tap or drop
  against the selection and drag state it acts on.
- `expr` entries (runtime 17) are compiled once at load (`CompileExpr`) and take part in the same
  dependency order as the other forms.

### 2.4 Interaction

- **Touch input.** `AuxRouting::SetTouch` / `GetTouch` carry up to 16 fingers as `AuxTouchPoint`,
  where bit 0 marks the start of a touch and bit 1 the end. A touch that both starts and ends
  between two reads is split into two observations, so short taps are never lost.
- **Gestures.** `UpdateGestures` (`mod_input.cpp`) classifies each touch:
  - tap (12 px slop), pan, pinch-zoom (`pan_zoom` widgets), drag-and-drop (`draggable` /
    `accept_group`), or scroll-region drag with fling;
  - press-and-hold (runtime 13): the first finger arms `HoldTracker` (`mod_input_hold.h`) when
    an `on_hold` widget is under it. The hold fires once, on the first tick after `hold_ms`
    (wall clock), is queued like a tap with the widget's action (no hit test, log source
    "hold"), and the lift that ends it is not a tap. Moving past the slop, a second finger, a
    drag, a page transition or any page change cancels it;
  - swipe (runtime 14 horizontal, 15 vertical): `SwipeTracker` (`mod_input_swipe.h`) arms on
    the topmost swipe widget under the first finger, locks a direction when the finger leaves
    the slop (|dx| > 2|dy| or the reverse), and fires at the lift if `swipe_px` was reached
    within 600 ms. The lift is not a tap;
  - hold and drag on one widget (runtime 16): a hold may arm over a drag candidate; leaving the
    slop before `hold_ms` cancels the hold and starts the drag;
  - input is ignored while a page transition runs, and `input_block` widgets absorb touches.
- **Controller focus mode (runtime 17).** `UpdateNav` (`mod_nav.cpp`) reads the real buttons of
  player 1 and handheld, toggles the mode on the `nav.toggle` chord, moves the focus among the
  page's tappable widgets (`NavCandidates`) and queues an **A** press as a tap at the focused
  centre, which then takes the normal tap path. The chord does not enter the mode on a page
  with nothing to focus. Without a `nav` key the mode is enabled only when the package's
  `min_runtime` is 17 or higher (`NavDefaultOn`, manifest or `package.json`); older packages
  opt in with `"nav": true` or a `nav` object. While the mode is on, `Core::HID::DSModPadGate`
  (a set of process-wide atomics applied in `NPad::RequestPadStateUpdate`) gives the game a
  neutral pad for every frontend, and buttons still held when it ends stay hidden until
  released. `@nav.*` values are uncovered draw inputs, so a focus move repaints the page.
- **Taps.** `DrainTaps` hit-tests queued taps against the **previous render's** map draw
  records (`map_draw_records_published`). This one-frame lag is deliberate: it tests the tap
  against what the user actually saw. Hits run `RunAction`.
- **Actions.** `RunAction` (`mod_actions.cpp`) supports these kinds: `write`, `slot_write`, `button` (virtual pad;
  a two-button chord such as `"L+R"` presses both on the same tick),
  `page` (including the runtime 17 targets `@settings` and `@back`), `call`, `sequence`,
  `flag`/`set_value`, `view_reset`, `module` (forwarded to the module's `on_action`), and
  `map_select`. Since runtime 16 a `module` action whose `on_action` returns false is
  **Refused**, which plays the `refused` haptic and stops what follows. A flag change that
  touches a `persist_flags` entry rewrites the persisted-flags file (runtime 15).
- **Haptics.** A fired hold plays the `hold` kind (runtime 13), and a fired swipe the `swipe`
  kind (runtime 14), instead of the action's tap haptic. At most one haptic is raised per tick (`FlushHaptic` →
  `AuxRouting::RaiseHaptic`). A separate notifier thread delivers it to the frontend.

### 2.5 Page binds and transitions

`DrivePageBinds` (`mod_pages.cpp`) implements `page_binds`, which switch pages automatically. Each bind watches a
published value.

- A bind arms only once its `point` (and `ready_bind`, if present) reads valid. It disarms
  whenever either becomes unreadable.
- It fires on the **edge** into or out of `equals`.
- A switch is deferred while a finger is down.
- A switch is dropped while the current page is `no_auto_leave`.

A page change can animate: `slide_up`, `slide_down`, `grow`, `shrink`, `fade` or `none`.
`DrivePageTransition` renders the "from" and "to" pages on the tick thread and publishes each
blended frame. It also bumps `redraw_dispatch_generation` and `redraw_authority_generation`, so
any worker job still in flight is
discarded.

### 2.6 The redraw decision and the worker

Redraws are skipped whenever possible, at three levels (all in `mod_redraw.cpp`):

1. **Throttle.** A redraw is attempted only when `tick_count % 2 == 0` (≈30 Hz). The exception
   is a running animation with `anim_hz >= 60`, which gets every tick.
2. **Signature.** `UiSignature` hashes everything visible:
   - ints and texts;
   - floats, quantised;
   - map generations and marker state;
   - the view transform;
   - an animation epoch, but only when a visible widget animates itself.
   If the hash is unchanged, nothing is drawn or published.
3. **Dirty regions.** `BuildRenderExtras` hashes each widget's inputs (`WidgetDependencyHash`,
   or `RepeatTemplateDependencyHash` for repeats) and compares the hash with the last drawn
   one.
   - The changed boxes, plus any animation boxes, are merged by `MergeDirtyRects` into **at
     most 4 disjoint rectangles** (24 px merge gap).
   - The redraw is **partial** only when all of these hold:
     - the dirty area is at most 60% of the canvas;
     - the page is the same as last time;
     - no drag is active;
     - no module images landed this tick.
   - Otherwise the whole page is redrawn.

The drawing itself happens off the tick thread:

- `DispatchRedraw` places a `RedrawJob` in a **single coalescing mailbox**
  (`redraw_pending_job`), guarded by `redraw_job_mutex` and `redraw_job_cv`.
  - The job carries a shared, immutable copy of the page and the fonts, an owned copy of the
    snapshot, the dirty rects, the view state and a generation number.
  - A newer job replaces an unconsumed one. Two partial jobs for the same page merge their rects;
    any other combination becomes a full redraw.
- The **`DSModRedraw`** thread (low OS priority) runs `RunRedrawJob`, which calls `RenderPage`
  once per dirty rect into the worker's own canvas.
  - A job is stale only when the tick thread took over the screen after it was dispatched (a
    page transition or the synchronous carve-out bumped `redraw_authority_generation`). A stale
    job's result is thrown away. A job that was merely superseded by a newer dispatch still
    publishes: the single worker finishes jobs in order (runtime 13).
  - As a safety net, `UnpublishedRegions` remembers what a stale job painted, and the next
    published job publishes those regions too, or publishes full (runtime 13). Before runtime 13
    every newer dispatch made the running job stale. A slow page could then publish nothing
    until it settled, and widgets moved by `y_bind` could stay at their old place on screen.
  - An animation group's settle frame is cleared once it is dispatched, so a settled group no
    longer repaints its box every tick (runtime 13).
  - `EDEN_DSMOD_SYNC_REDRAW=1` restores the synchronous path, which exists as a kill switch.

### 2.7 Publish path

There are two transports, both implemented in `VideoCore::DSMod::AuxRouting`.

**CPU canvas and tile diff (default).**

- `PublishUi` / `PublishUiPartial` copy the rendered pixels into `ui_pixels` under `ui_mutex`.
- With TILEDIFF enabled (the default; `EDEN_DSMOD_UI_DIFF=0` turns it off), `DiffCopyRect`
  compares the rows with `memcmp`. It then copies only the 64×64 tile segments that actually
  differ and marks them in a `TileMask`.
- A publish whose pixels are identical to the held copy does **not** bump `ui_serial`, so the
  renderer neither uploads nor re-presents.
- On the GPU thread, `RendererVulkan::RenderAuxModUi` checks the atomic `ui_serial` without
  locking. It then uploads only the dirty tiles through a persistent staging buffer (one copy
  region per merged run, with no `scheduler.Finish`) and blits the result to the aux swapchain.

**GPU quad compositor.**

- This transport is used when the manifest sets `"flags": {"gpu_composite": true}` or the
  environment sets `EDEN_DSMOD_GPU_COMPOSITE=1`, and the page contains a map widget.
- `RenderPage` emits an `AuxDrawList` of map quads. `PublishGpuComposite` then publishes:
  - up to 6 texture slots: the current map, the icon atlas, the HUD canvas, a pulse texture, the
    previous map endpoint, and fade weights;
  - the quad list.
- The map texture is re-uploaded only when its generation stamp (`MapStamp`) changes. There is
  no per-pixel hashing.
- `RenderAuxModUiGpu` draws the quads with a small coverage-blend pipeline and nearest
  sampling.
- The choice is made per tick. When the map is not in that tick's dirty set, the canvas path
  runs instead. `ui_composite_pending` then forces one full resync, so the canvas never patches
  a stale buffer.

**Renderer priority** in `RendererVulkan::Composite`, from highest to lowest:

1. GPU composite
2. CPU UI
3. a VI layer bound through `dsm:u`
4. a mirror crop of the game frame (a `mirror` page)
5. a mirror of the whole frame

The aux surface has its own swapchain and a dedicated present thread
(`force_present_thread`), so presenting the second screen never blocks the main render thread.

### 2.8 Second display

| | Desktop (`eden-cli`) | Android |
|---|---|---|
| Surface | `AuxWindow_SDL3`: a resizable "Screen 2" window, or `--aux-virtual` for a windowless 1240×1080 canvas | `AuxPresentation`: an Android `Presentation` holding one `SurfaceView` on the best non-default display |
| Attach | `Renderer().SetAuxWindow(...)` | `auxSurfaceChanged` → JNI → `EmulationSession::AttachAuxWindowLocked` |
| Touch | Mouse or finger → one `AuxTouchPoint` → `AuxRouting::SetTouch` | `onAuxTouch` reports every pointer → JNI → `SetTouch` |
| Haptics | Logged | `PlayAuxHaptic` → `AuxPresentation.playHaptic` (falls back to the vibrator) |

Android chooses the display from the union of `DISPLAY_CATEGORY_PRESENTATION` and all displays:

- physical panels come before virtual or overlay ones;
- a display named "Screen-2" is preferred;
- each candidate is tried until one accepts the window.

Some handhelds list the second panel only under the presentation category, which is why the
union is needed.

### 2.9 `dsm:u`

`dsm:u` is an HLE service for guest code, such as homebrew or an exefs mod. Its IPC version is 1.

| Cmd | Name | Behaviour |
|---|---|---|
| 0 | `GetVersion` | Returns 1. |
| 1 | `GetAuxDisplayInfo` | Returns `present`, `width`, `height`, `rotation` and `refresh_hz` (24 bytes). |
| 2 | `GetAuxTouch` | Returns touch points. Returns **0 points** while a package owns the screen, because the widgets consume the touches. |
| 3 | `BindLayer(mode, layer_id)` | Routes one VI layer to the second screen. Mode 0 unbinds. |

Guest code must probe for the service before calling `smGetService("dsm:u")`, for example with
sm command 65100 (`AtmosphereHasService`). The service manager defers unknown service names
forever, so an unprobed request never returns.

### 2.10 Module-generated data (runtime 12)

A module that exports the data extension (`eden_dsmod_get_data_extensions`, see
[MODULE_GUIDE.md §2.5](MODULE_GUIDE.md#25-data-eden_dsmod_get_data_extensions)) can serve package
data that would otherwise ship as files.

- **Byte sources.** `ReadAssetBytes` (`mod_assets.cpp`) sends any `module:` source to
  `LoadModuleData` (`mod_module_services.cpp`), which calls the module's `load_data`. This covers
  every byte read that goes through `ReadAssetBytes`, such as map `geo` blobs and the manifest
  `font`. Calls may come from any runtime thread and are serialized by `module_data_mutex`; the
  module may block while it generates the data. Results are capped at 64 MiB and keys at 256
  bytes.
- **Map areas.** When the manifest sets `map.areas_src`, `StartModuleAreas` fetches that key on
  its own thread (`module_areas_thread`) and parses the JSON with `ParseMapAreasJson`, the same
  parser the inline `map.areas` use. The inline areas stay in use as a template until then.
- **Install.** The tick thread installs the parsed areas once (`InstallModuleAreas`). It waits
  for a tick on which the redraw worker has no job running or queued (joining it anyway after 60
  busy ticks), stops the worker, swaps `manifest.map_areas` under `map_state_mutex`, and drops
  the geometry caches and every map-derived image so they rebuild from the new areas.
- **Failure.** If the module has no data extension, or the fetch or parse fails, an error is
  logged and the map keeps its inline template areas.

## 3. Threads and locks

| Thread | Runs | Talks to others via |
|---|---|---|
| Host timing ("tick thread") | `ModRuntime::Tick`: sampling, module `sample`/`tick`/`on_action`, derived values, gestures, actions, page binds, transitions, dispatch | `redraw_job_mutex`/`cv`, `tap_mutex`, `view_mutex`, `map_state_mutex`, `AuxRouting` locks |
| Guest CPU cores | `OnGuestBreakpoint` (guest calls and spies; Dynarmic only) | `guest_bridge_mutex`, atomic `call_state`; results drained on the next tick |
| `DSModRedraw` | `RunRedrawJob` → `RenderPage` | Owns `worker_canvas`; publishes into `AuxRouting` |
| Nx asset worker | romfs decode, composites, MSBT | `NxAssetState` `queue_mutex`/`cv`, `io_mutex`, `state_mutex` |
| Module asset worker | Module `load_image` (one thread; at most 4 finished images buffered) | `module_asset_mutex`/`cv`, `module_loader_mutex` |
| Module map-areas thread | `load_data` for `map.areas_src`, then the JSON parse (runtime 12) | `module_data_mutex`, `module_areas_mutex`, atomic `module_areas_ready`; installed by the tick thread |
| Font page worker | Loads one page of a paged font atlas at a time (runtime 17) | `FontPages` mutex; a landed page triggers a repaint on the next tick |
| Haptic notifier | Frontend haptic sink | `haptic_mutex`/`cv` (queue bounded at 8) |
| GPU thread | `RendererVulkan::Composite`, `SyncAuxWindow`, aux uploads | `ui_mutex`, `comp_mutex`, atomic serials, `aux_mutex` |
| Aux present thread | Presents the aux swapchain | Scheduler submit lock |
| Service thread (`dsm:u`) | Guest IPC | Atomic `bound_layer`, `touch_mutex` |
| Frontend UI thread | SDL events or Android callbacks → `SetTouch`, `SetAuxWindow` | `touch_mutex`, `aux_mutex` |

Ownership rules that keep this race-free:

- A `RedrawJob` **owns** everything it reads: a copy of the snapshot, a `shared_ptr` to an
  immutable page and fonts, and a copy of the draw list. The worker never reads the tick
  thread's live snapshot.
- `ModRuntime::canvas` belongs to the tick thread (transitions and the synchronous path).
  `worker_canvas` belongs to the worker.
- The image cache stores `shared_ptr<const Image>`. A handle stays valid after eviction.
- `map_state_mutex` (recursive) guards all map state (visited cells, geometry, markers,
  generations). Both the tick thread and the worker take it.
- `AuxRouting::ui_serial` is atomic, so the GPU thread can check for new frames without
  locking. The producer holds `ui_mutex` for its whole diff-copy.
- A boot-free ThreadSanitizer harness exists (`src/tests/core/mods/tsan_redraw_harness.cpp`).

## 4. Runtime version and `min_runtime`

`DualScreenRuntimeVersion` in `src/core/mods/mod_runtime.h` is the contract version that this
build implements. It is currently **19**, the version Eden Duo 1.2.0 ships.

| Version | Added |
|---|---|
| ≤10 | Unversioned. V9 added grow/shrink transitions; V10 added `page_binds`. |
| 11 | Scroll regions (page `scrolls`), a text dirty-rect fix, `module:` composite layers with a full repaint when module images land, and `min_runtime` gating. The module `write_memory` service and the write-batch extension arrived in the same release |
| 12 | The module data extension (`load_data`): `module:` byte sources read through `ReadAssetBytes`, such as map geometry, and `map.areas_src`, map areas generated by the module from the game's romfs (§2.10) |
| 13 | Press-and-hold (`on_hold`, `hold_ms`) and the `hold` haptic kind (§2.4). The redraw worker no longer drops a job that a newer dispatch superseded, and a stale job's region is published by the next job (`UnpublishedRegions`, §2.6), which fixes stuck rows and late module images. `EDEN_DSMOD_IMAGE_TIMING` (§7) |
| 14 | Horizontal swipe (`on_swipe_left` / `on_swipe_right`, `swipe_px`) and the `swipe` haptic kind; map widget `image_bind`, `overlays`, per-slot dynamic-marker pictures, bars, dim, frame and tint; map `view_rect_*_bind`; `@map_tap_x` / `_y` / `_seq` readable by modules; label `color_markup`; module image and font retries |
| 15 | Named asset sources (`AssetSources`): `base:` and `aoc:`, module `read_romfs` through them, `EDEN_DSMOD_CAP_SOURCE_*` and `get_i64("__source:<prefix>")`, unknown prefixes refused; `module_tick_hidden` and the `TICK_WHEN_HIDDEN` / `NO_TICK_WHEN_HIDDEN` module flags; `outline_copy`; button `border` / `text_inset`, pips `gap`, bar `frame`, map `label_offset` and the `map.style` door, collectible, blink and pin keys; more `{i}` fields; vertical swipe; the bound default view of a non-map `pan_zoom` widget; `persist_flags`. Runtimes 14 and 15 first shipped together (Eden Duo 1.0.2), which also brought text `outline` / `outline_px` / `rise` and `"L+R"` button chords |
| 16 | Hold and drag on one widget; a module action returning false is Refused; `@sel:` / `@drag*` published before the taps; `read_romfs` from `create()` (romfs source `retry_ms`); module image keys up to 4096 characters; image `rotate` / `rotate_bind` / `pivot` / `scale_bind`, `tint` / `tint_bind` / `tint_colors`, `fill` stretch / tile / slice + `slice`, bar `fill_dir` + `image`; `@clock.*`, `@game.seconds` and derived `countdown` |
| 17 | Widget type `chart`; derived `expr`; `auto_w`, value `group` / `group_sep`; `{i}` in every repeat string field; paged font atlases (`font_page_h`); controller focus mode (`nav`, page `nav_order`, `@nav.*`, HID pad gate); the `user:` source (`EDEN_DSMOD_CAP_SOURCE_USER`); manifest `settings`, the built-in `@settings` page and the `@back` page target |
| 18 | Font refresh epochs (`__font_epoch`); fitted and centered labels; image scrollbars; marker size caps; parser and rendering fixes. Shipped in Eden Duo 1.1.0 |
| 19 | Format-2 guest helpers (separate RX code, main/code relocations, mailbox epochs) with Ready/page/auxiliary lifecycle; exact bounded metadata reads (4 MiB) and transactional reload validation; plans stay 1 MiB, native libraries 64 MiB. Shipped in Eden Duo 1.2.0 |

- **How the requirement is read.** A package declares the oldest runtime it needs as
  `min_runtime`, in `manifest.json` and/or `package.json`. `PackageMinRuntime(manifest, package)`
  takes the larger of the two.
  - A missing value means 0.
  - A malformed value (negative, float, bool, or a non-digit string) means `UINT32_MAX`, so the
    package is always gated and never half-loaded.
- **Where it is checked.**
  - In `Discover` (`mod_manifest.cpp`), on the raw JSON, before anything else is parsed. If the requirement is newer
    than this build, the runtime shows the built-in `UpdateRequiredManifest` page ("UPDATE EDEN
    DUO", "runtime N, have 17") and loads none of the package.
  - In `DiscoverModLoadPlan` (`mod_load_plan.cpp`): no load-time patches are applied.
  - On a console `reload`: the reload is refused with "restart the game".
- **Older runtimes.** Runtimes before 11 do not know the key. They ignore it, and may reject
  unknown fields silently. A runtime-11 build does gate a package that declares
  `"min_runtime": 12`, and shows its update page. This is why the APK and the package should be
  shipped together.
- **Unknown keys degrade silently.** Every runtime ignores manifest keys it does not know, so a
  package using a runtime-N key without declaring `"min_runtime": N` still loads on an older
  runtime, minus that feature. Declare N whenever the package depends on the key
  ([PACKAGE_FORMAT.md §6](PACKAGE_FORMAT.md#6-runtime-history-and-min_runtime)).

Only the integer constant is compared against `min_runtime`.

## 5. Performance design

**Rule: only tick-thread time competes with emulation.**

- The tick runs inside the timing callback, so time spent there costs the game directly. Worker
  time only delays the companion.
- A full redraw is pixel-identical to a partial one, so pixel tests cannot catch a performance
  regression. The tick cost must be measured.

| Technique | Where | Effect |
|---|---|---|
| Reused snapshot, `ankerl::unordered_dense` maps (`SnapshotMap`) | `mod_types.h` | Publish, derived and signature stages measured at about 0.4–0.6× of the previous map type's cost |
| Cached derived order and volatile-only second pass | `EvaluateDerived` | One large package went from 0.95 to 0.48 ms per tick |
| `UiSignature` idle gate | `PublishUi` | 0 redraws/s when nothing changes |
| ≈30 Hz redraw throttle | `PublishUi` | Halves redraw attempts; animations can opt into 60 Hz |
| Per-widget dependency hashes, ≤4 dirty rects, 60% cutover | `BuildRenderExtras`, `MergeDirtyRects` | Partial redraws of only what changed |
| Off-thread redraw with a coalescing mailbox and shared immutable job data | `DispatchRedraw`, `RunRedrawJob` | Job copy dropped from 91–370 µs to 8–18 µs; the tick no longer pays for drawing |
| TILEDIFF with 64×64 tiles; identical frames do not bump the serial | `AuxRouting::DiffCopyRect`, `TileMask` | One measured page: 401.8 MB uploaded per 5 s fell to 1.39 MB; GPU-thread time per upload went from ≈1100 µs to ≈7.6 µs |
| Async tile upload through persistent staging, no `Finish` | `RendererVulkan` | Removed a ≈6 ms per-frame stall on a mobile GPU |
| Dedicated aux present thread; skip re-present when unchanged | `PresentManager(force_present_thread)` | The second screen never blocks the main present |
| GPU compositor, keyed by generation stamps | `PublishGpuComposite` | The map uploads only when its content generation changes |
| Sliced heap scans and raw host-page reads | readers and RE tools | Avoids multi-millisecond tick spikes |

Measured examples (one P5R test run, redraw worker ms per job, before → after): field page
9.11 → 0.42, item page 12.31 → 1.02, music page 12.15 → 2.68. In the same run, tick-thread
totals fell to 0.29–0.52× of the baseline.

**Profiling.** Stage timers are **on by default** and print "DSMod perf CPU <stage>: calls avg
max …" every 5 s. `EDEN_DSMOD_PROFILE=0` turns them off. `EDEN_DSMOD_PROFILE_WIDGETS=1` adds
per-widget draw times.

## 6. Extension points

| Point | Mechanism | Negotiation |
|---|---|---|
| Declarative package | `manifest.json` + per-build data file | `min_runtime` |
| Native module | `eden_dsmod_get_module` → `EdenDsmodModuleApi` (`supports_build`, `create`, `sample`, `tick`, `destroy`) | ABI version 1 + ABI hash, `struct_size`, capability bits (module flags since runtime 15; host source bits since 15, `SOURCE_USER` since 17), SHA-256 pin, build-ID allow-list |
| Asset sources | `AssetSources::Register` (`mod_sources.h`): a prefix with a directory root (`open_dir`) or a byte reader (`read_bytes`), optional `capability`, `accept_path`, `max_file_size`, `retry_ms` | Runtime-internal. A module asks `get_i64("__source:<prefix>")`; a package declares `min_runtime` |
| Base extensions | `eden_dsmod_get_extensions` → `configure`, `on_action`, `load_image`. The host provides a guest mailbox (atomic `load_*`/`store_*`) and an ASTC decoder | Own version and hash. Required if the module sets `EDEN_DSMOD_CAP_EXTENSIONS` |
| Font | `eden_dsmod_get_font_extensions` → `decode_font`, called once for the manifest's `font` asset | Optional; independent version and hash |
| Save (read-only) | `eden_dsmod_get_save_extensions` → `configure(read_save_file)`, limited to the title's own save directory | Optional. No shipped module uses it yet |
| Write batch | `eden_dsmod_get_write_extensions` → `configure(write_batch)`: up to 16 ops with expect-checks, applied as one unit with guest threads suspended | Optional |
| Data (runtime 12) | `eden_dsmod_get_data_extensions` → `load_data`: bytes behind `module:` sources and `map.areas_src` | Optional; packages that rely on it set `min_runtime` 12 |
| Load plan | `load_plan` in the manifest: load-time code patches and a guest mailbox | Plan JSON stays ≤1 MiB; runtime 19 metadata ≤4 MiB (runtime 18 discovery ≤1 MiB); format-2 helpers require `min_runtime` ≥19 |
| Guest service | `dsm:u` | IPC version 1 |

Every extension has its own export symbol, version and hash (`dsmod_module_extensions.h`). None
of them grows an existing struct. A module built before an extension existed keeps loading unchanged, because the host
checks `struct_size == sizeof(...)` exactly. See [MODULE_GUIDE.md](MODULE_GUIDE.md).

## 7. Environment variables

Desktop only. Android has no per-process environment, so it uses manifest `flags` instead.

| Variable | Effect |
|---|---|
| `EDEN_AUX_WINDOW=1` / `--aux-window` | Opens the SDL3 second window. |
| `EDEN_AUX_VIRTUAL=1` / `--aux-virtual` | Windowless 1240×1080 aux canvas. |
| `EDEN_DSMOD_GPU_COMPOSITE=0/1` | Forces the quad compositor off or on. |
| `EDEN_DSMOD_GPU_READBACK=1` | Reads the GPU-composited frame back so screenshots work. Roughly halves fps. |
| `EDEN_DSMOD_UI_DIFF=0` | Disables TILEDIFF. |
| `EDEN_DSMOD_SYNC_REDRAW=1` | Redraws on the tick thread (kill switch). |
| `EDEN_DSMOD_REDRAW_NORMAL_PRIORITY=1` | Worker at default OS priority. |
| `EDEN_DSMOD_FORCE_FULL_REDRAW=1` | Disables the per-widget dirty scan. |
| `EDEN_DSMOD_PROFILE=0`, `EDEN_DSMOD_PROFILE_WIDGETS=1` | Profiling off; per-widget profiling on. |
| `EDEN_DSMOD_PATCHES=1` | Applies data-file patches marked `optional`. |
| `EDEN_DSMOD_NO_GUEST_BRIDGE=1` | Disables guest calls, sequences and spies on a Dynarmic desktop, to reproduce NCE conditions. |
| `EDEN_DSMOD_CMD=<file>` | Enables the live console (dev-tools builds). |
| `EDEN_DSMOD_AUTOSTART`, `EDEN_DSMOD_INPUT`, `EDEN_DSMOD_ASSUME_GAMEPLAY` | Headless input and gameplay gating. |
| `EDEN_DSMOD_ANIM_DUMP=<dir>`, `EDEN_DSMOD_ANIM_SCALE=<f>` | Dump or slow down animations. |
| `EDEN_DSMOD_IMAGE_TIMING=1` | Logs when each module image lands: queued → decoded → drained, in ms (runtime 13). |
| `EDEN_DSMOD_DUMP_ROMFS=<romfs path>:<out>` | Saves one file from the running game's merged romfs. |
| `EDEN_DSMOD_FIND`, `SCAN`, `DIFF`, `MOTION`, `RANGE`, `FIELD`, `HEAPFIND`, `HEAPDUMP`, `CLASSDUMP`, `ARRAY`, `DUMP` (all `EDEN_DSMOD_`-prefixed) | Reverse-engineering scanners; see [PORTING_A_GAME.md](PORTING_A_GAME.md). |

Not documented here in detail, because their exact semantics were not checked:
`EDEN_DSMOD_INPUT_LIVE`, `EDEN_DSMOD_INPUT_DELAY`, `EDEN_DSMOD_GAMEPLAY_TRIGGER` and
`EDEN_DSMOD_RTLOG`.
