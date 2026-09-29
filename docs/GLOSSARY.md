# Glossary

| Term | Meaning |
|---|---|
| **Accessor / getter** | A small game function that returns or loads a global. Modules decode the global's address from the getter's ADRP + LDR/ADD instructions instead of hardcoding it. |
| **Asset-free package** | A package that ships no game art, fonts or text. Everything is decoded at runtime from the player's own romfs. |
| **Aux screen / aux window** | The second (bottom) display. On desktop it is an SDL3 "Screen 2" window or a windowless virtual canvas; on Android it is a `Presentation` on a secondary display. |
| **`AuxRouting`** | Shared state between the runtime, the renderer and the frontend: published pixels, dirty tiles, touch, haptics, and the `dsm:u` layer binding. |
| **Build ID** | The 32-byte ID of the game's `main` executable. Its first 16 hex digits name the per-build data file. Modules pin supported builds by it. |
| **Composite** | An image the runtime builds from layered sources (`composite:<name>`). |
| **Data extension** | The optional module extension `eden_dsmod_get_data_extensions` → `load_data`. It serves `module:` byte sources such as map geometry or a map-areas JSON object (runtime 12). |
| **Data file** | `dualscreen/<BUILD16>.json`: points, symbols, spies and patches for one executable build. |
| **Derived value** | A value computed each tick from other values (`derived`: terms, select, cmp, all/any_nonzero, …). |
| **Dirty rect** | A canvas region whose widgets' inputs changed. At most 4 per redraw. |
| **`dsm:u`** | HLE service that lets guest code query the aux display, read its touch input, or bind a VI layer to it. |
| **Dynarmic** | The emulator's JIT CPU backend (inherited from upstream Eden). It supports guest breakpoints, so guest calls, sequences and spies work. It is always used on x86-64 desktop. |
| **Enforce rule** | A manifest entry that runs an action periodically while a gate holds. It is used to press native-menu buttons that a module requests. |
| **Fail closed** | When data cannot be verified, publish "not ready" instead of a guess. |
| **Gate** | A published value name, optionally prefixed `!`. It is open when the value exists and is non-zero. |
| **GPU composite** | An alternative publish path. Map widgets are sent as textured quads plus textures, and composited on the GPU. |
| **Guest** | The emulated game, its code and its memory. |
| **Hold (press-and-hold)** | A single finger resting still on a widget with `on_hold` for `hold_ms` (default 600 ms). Runs that action once; the lift that ends it is not a tap (runtime 13). |
| **Guest call** | Running a game function from the runtime by borrowing a game thread at a breakpoint. Dynarmic only. |
| **Load plan** | Code patches plus a guest mailbox, applied when the executable loads (`load_plan`). |
| **Map areas source** | `map.areas_src`: a `module:` key whose JSON object replaces the manifest's inline `map.areas` once the module has generated it (runtime 12). |
| **Manifest** | `dualscreen/manifest.json`: pages, widgets, actions, derived values, fonts and map configuration. |
| **Module (native)** | A per-title shared library, `<TITLEID>.so`, loaded from the package through the C ABI. |
| **`module:` source** | A key the native module resolves. As an image source it goes to `load_image`; as a byte source (a map `geo`, `map.areas_src`, the `font`) it goes to `load_data` (runtime 12). |
| **NCE** | Native Code Execution: guest code runs directly on an ARM64 host CPU. It is the Android default. Guest breakpoints do not reach the runtime under NCE. |
| **Page bind** | An automatic page switch, triggered on the edge of a published value (`page_binds`). |
| **Point** | A declarative memory read: an address or pointer chain, a type, and optional array expansion. |
| **Publish** | Putting a value into the tick's snapshot (module `publish_*`), or handing a rendered frame to `AuxRouting`. |
| **Recipe** | A small program in P5R's `p5r_art.rec` that builds one image from romfs art (crop, scale, composite, text). |
| **Redraw worker** | The low-priority `DSModRedraw` thread that renders pages off the tick thread. |
| **romfs / exefs** | The game's read-only file system, and its executable partition (`main`, `rtld`, `sdk`, …). |
| **Runtime version** | `DualScreenRuntimeVersion` (currently 13). Packages gate on it with `min_runtime`. |
| **Sentinel test** | Change a value reversibly in RAM, check that the native menu and the companion both follow, then restore it. |
| **Sequence** | A manifest-declared chain of guest calls. Dynarmic only. |
| **Snapshot** | `StateSnapshot`: all values published in one tick (ints, floats, texts, addresses). It is cleared every tick. |
| **Spy** | A breakpoint that captures a register value when a game function runs. Dynarmic only. |
| **Tick** | One run of `ModRuntime::Tick`: the CoreTiming event `"DSMod::Tick"` at a fixed 60 Hz. |
| **TILEDIFF** | The publish-side diff that marks only changed 64×64 tiles. It lets the renderer upload just those tiles. |
| **Torn read** | A multi-field read taken while the game was mid-update. It is detected by reading twice and comparing, or by re-checking context. |
| **`UiSignature`** | A hash of everything visible. If it has not changed, the tick draws and publishes nothing. |
| **Write batch** | The optional module extension `eden_dsmod_get_write_extensions` → `write_batch`: up to 16 guarded writes applied as one unit while guest threads are suspended. |
