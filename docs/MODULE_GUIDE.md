# Writing a native per-title module

A declarative package can read pointer chains and draw widgets. Some games need more than that:

- state spread across linked lists or task tables;
- values the game computes on demand;
- art packed in proprietary archives;
- menu actions that must be reproduced exactly.

A **native module** handles those cases. It is a shared library, built for one title and loaded
from the package. It talks to the emulator only through a small, stable C ABI.

Most games start as a declarative package and add a module only when they need one. If you are
new to companions, read [CONTRIBUTE.md](CONTRIBUTE.md) and
[PORTING_A_GAME.md](PORTING_A_GAME.md) first.

**Where things live.** Game-specific sources, their tests and build targets live in this
companions repository under `native/modules/`. The generic ABI headers and shared SDK remain
in [Eden Duo](https://github.com/igawa6/eden-duo). Select a compatible checkout with
`-DEDEN_SOURCE_ROOT=/absolute/path/to/eden-duo`; see [native/README.md](../native/README.md).
Paths starting with `src/` below refer to Eden Duo; `native/`, `packages/` and `tools/` refer
to this repository.

The headers are:

- `src/core/mods/dsmod_module_abi.h`: the base ABI. It has no emulator dependencies.
- `src/core/mods/dsmod_module_extensions.h`: the five optional extensions.
- `src/core/mods/modules/dsmod_module_sdk.h`: header-only helpers shared by the shipped modules
  (§1.5).

The worked examples are the Persona 5 Royal module (`native/modules/01005CA01580E000.cpp`, `p5r_reader.h`,
the `p5r_reader_*.cpp` area files and the header-only `p5r_*.h` decoders; §7) and the Metroid
Dread module's map generator (`native/modules/010093801237C000.cpp` plus `dread_*.cpp`; §8).

## 1. The ABI at a glance

```
 module exports                      host provides (EdenDsmodHostApi)
 ─────────────────────────────────   ───────────────────────────────────────────
 eden_dsmod_get_module(ver, hash)    title_id, build_id[0x20], main_base, main_size
   → EdenDsmodModuleApi              get_tick, get_heap_begin, get_heap_end
       supports_build(build_hex)     is_mapped, read_memory, get_read_pointer
       create(host, config_json)     write_memory (≤64 B)
       sample(inst, host)            read_romfs (romfs:, file:, base:, aoc:, user:)
       tick(inst, host)              publish_i64/f64/text/address/map
       destroy(inst)                 get_i64/f64/text (read the current snapshot)
                                     log
 optional:
 eden_dsmod_get_extensions           configure, on_action, load_image
 eden_dsmod_get_font_extensions      decode_font
 eden_dsmod_get_save_extensions      configure(read_save_file)
 eden_dsmod_get_write_extensions     configure(write_batch)
 eden_dsmod_get_data_extensions      load_data                    (runtime 12)
```

**Constants.**

- `EDEN_DSMOD_MODULE_ABI_VERSION = 1` and `EDEN_DSMOD_MODULE_ABI_HASH = 0x8d3f5b1e6a70c429`.
- Capability bits (`EDEN_DSMOD_CAP_*`), one `uint64_t` in both the module table and the host
  struct. Adding bits did not change the ABI version or hash.

  | Bit | Name | Since | Meaning |
  |---|---|---|---|
  | 0 | `WRITE_MEMORY` | | Host: `write_memory` works |
  | 1 | `GUEST_CALL` | | Never granted (see §1.3) |
  | 2 | `ROMFS_READ` | | Host: `read_romfs` works |
  | 3 | `MAP_OUTPUT` | | Host: `publish_map` works |
  | 4 | `EXTENSIONS` | | Module: exports the base extensions (defined in the extensions header) |
  | 5 | `TICK_WHEN_HIDDEN` | 15 | Module flag: tick while the second screen is hidden (§1.2) |
  | 6 | `NO_TICK_WHEN_HIDDEN` | 15 | Module flag: never tick while hidden |
  | 7 | `SOURCE_PREFIXES` | 15 | Host: `read_romfs` resolves `<prefix>:<path>` through the source registry (§1.3) |
  | 8 | `SOURCE_BASE` | 15 | Host: the `base:` source is registered |
  | 9 | `SOURCE_AOC` | 15 | Host: the `aoc:` source is registered |
  | 10 | `SOURCE_USER` | 17 | Host: the `user:` source is registered |

  A runtime-15+ host advertises bits 5 and 6, so a module may set either. An **older host
  refuses** a module that sets one, because the module's bits must be a subset of the host's;
  such a module's package should declare `"min_runtime": 15`. Bits 7–10 describe the host; check
  them in `host->capabilities` rather than requesting them.

**Borrowing rule.** Every pointer passed into a callback is borrowed for that call only. Copy
anything you keep, including the host struct itself.

### 1.1 Entry point and module table

```c
const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t host_abi_version, uint64_t host_abi_hash);
```

Return null to reject a host. The returned `EdenDsmodModuleApi` must contain:

- `abi_version`, `struct_size = sizeof(EdenDsmodModuleApi)` and `abi_hash`;
- your `title_id` and a display `name`;
- the `capabilities` you require;
- all five callbacks.

`GameModule::Load` rejects the module if any field mismatches or any callback is null.

### 1.2 Lifecycle and threads

| Callback | When | Thread |
|---|---|---|
| `supports_build(build_hex)` | Once, after the loader's own build-ID check. `build_hex` is the running build ID in upper-case hex | Loader |
| `create(host, config_json)` | Once, when the runtime starts or reloads. `config_json` is the **whole `manifest.json`**, with `_build_match` and `_build_id` added. Since runtime 16 the host is fully usable inside it: `read_romfs` works for every source, and a game romfs that is not ready yet is opened again on a later read (at most once a second) instead of staying unavailable for the session | Tick thread |
| `sample(inst, host)` | Every tick (60 Hz) while the second screen is present, after the declarative points are sampled | Tick thread |
| `tick(inst, host)` | Every tick, after touch capture and before derived values | Tick thread |
| `on_action` (extension) | When a `module` action fires; serialized with `sample`/`tick`. Since runtime 16, returning false refuses the action (§2.1) | Tick thread |
| `load_image` (extension) | On request for a `module:` image | **Asset worker**; may overlap `sample`/`tick` |
| `load_data` (data extension) | On a `module:` byte read (map geometry, `map.areas_src`) | **Any runtime thread** (for example the redraw worker or the map-areas thread); calls are serialized and may block |
| `destroy(inst)` | Shutdown or reload, after the map-areas thread and the asset worker are joined | Tick thread |

- **Hidden screen.** While the second screen is absent, `sample` does not run. Whether `tick`
  runs is decided, first match wins, by:
  1. the manifest's `module_tick_hidden` (`true` / `false`, runtime 15);
  2. the module's `EDEN_DSMOD_CAP_TICK_WHEN_HIDDEN` or `EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN` flag
     (runtime 15); set `NO_TICK_WHEN_HIDDEN` when `tick` is a full scan;
  3. otherwise the long-standing rule: `tick` runs if the module has `on_action`, so that
     queued actions (and a guest mailbox) can finish.
- **The snapshot is cleared every tick.** Publish every value on every `sample`, or the value
  disappears.
- **Exceptions.** A callback that throws sets `module_error`, and the module is shut down. Catch
  everything inside your callbacks: the ABI is C, and exceptions must not cross it.

### 1.3 Host services

| Service | Contract (`mod_module_host.cpp`) |
|---|---|
| `is_mapped(addr, size)` | Size ≤64 MiB, no overflow, and the address is inside a mapped guest region |
| `read_memory(addr, out, size)` | Checks `is_mapped`, then copies. Returns false on any failure |
| `get_read_pointer(addr, size)` | Zero-copy host pointer. Returns **null if the range crosses a 4 KiB guest page** |
| `write_memory(addr, in, size)` | ≤64 bytes, mapped only. Aligned 1/2/4/8-byte writes are single stores. Works under NCE and Dynarmic |
| `read_romfs(path, off, out, size)` | Paths without a prefix mean `romfs:`. `file:` means the package's `dualscreen/` folder. Since runtime 15, `base:` (the unpatched program romfs) and `aoc:` (the DLC data romfs) work too, and since 17 `user:` (files the player supplies; read-only, 32 MiB per file). Since runtime 15 an unknown prefix returns 0 (it used to be read as a romfs path that could only miss); `module:` is not readable here. `out == NULL` returns the size. `#` sub-paths read into containers. Paths ≤4096 characters, capped at 64 MiB per call |
| `publish_i64/f64/text/address(name, v)` | Writes into the snapshot under the bare `name`. Names ≤256 bytes, text ≤1 MiB. Non-finite doubles are dropped |
| `publish_map(frame)` | Map fog, water, walls and points for an area declared in the manifest. The visibility grid is 650×300 with values 0/1/2 |
| `get_i64/f64/text(name, …)` | Reads the current snapshot, including `@` values such as `@map_tap_x` / `@map_tap_y` / `@map_tap_seq` (runtime 14) and `@clock.*` (runtime 16; published only when the package's manifest or data file mentions `@clock.` / `@game.` or uses `countdown`). Special names: `get_i64("__relocation_delta")`, `get_text("__sequence:<name>")`, and since runtime 15 `get_i64("__source:<prefix>")`: 1 = the source is available, 0 = known but unavailable (no DLC installed; for `user:`, the folder cannot be created), the fallback = a prefix this host does not know (an older runtime) |
| `get_heap_begin/end` | Read from the live page table. Correct under NCE, where the heap is elsewhere |
| `begin_output`, `end_output` | Present in the struct, but **no-ops** in the current host |
| `queue_guest_call`, `poll_guest_result` | Present in the struct, but **never set by the current host** (always null). `CAP_GUEST_CALL` is never granted. Do not rely on them |

**Capabilities.** The host grants `ROMFS_READ | MAP_OUTPUT | EXTENSIONS | WRITE_MEMORY`, and
since runtime 15 also `TICK_WHEN_HIDDEN | NO_TICK_WHEN_HIDDEN | SOURCE_PREFIXES` plus one
`SOURCE_*` bit per registered source. A module whose `capabilities` include any bit outside the
host's set is rejected.

```c
/* Prefer the DLC's copy of a table when the host has aoc: and the DLC is installed. */
const char* path = "data/table.bin";
if ((host->capabilities & EDEN_DSMOD_CAP_SOURCE_AOC) != 0 &&
    host->get_i64(host->userdata, "__source:aoc", 0) == 1) {
    path = "aoc:data/table.bin";
}
size_t size = host->read_romfs(host->userdata, path, 0, NULL, 0);
```

### 1.4 How module values reach the page

- **Values.** Published names land directly in the snapshot, with no prefix. A manifest refers
  to them like any other value: `"bind": "live.money"`, derived-value terms, gates, `$name`
  action arguments.
- **`module_outputs`.** The manifest's `module_outputs` list is documentation only. The runtime
  does not read it.
- **Images.** An image source starting with `module:` goes to `GetModuleImage`:
  - The host checks its image cache first. On a miss, it queues the key for the asset worker
    (up to 128 pending) and returns nothing for now.
  - The worker calls your `load_image`. Finished images are drained on the next tick, into a
    64 MiB LRU cache.
  - A key that failed is asked for again after 2, 4, 6 and 8 seconds, then given up (5 attempts;
    runtime 14), so a decoder that needs the running game can succeed later.
  - Keys are at most 4096 characters since runtime 16 (256 before). `module:` data keys keep the
    256 limit.
  - When module images land, the page is repainted in full.
- **Other image paths.** A `src_bind` text value starting with `module:` is used directly as an
  image source. Composite layers and `font_atlas` also accept `module:` keys.
- **Bytes (runtime 12).** A `module:` source read as bytes rather than as an image (a map `geo`
  blob, `map.areas_src`, the manifest `font`) goes to the data extension's `load_data` (§2.5).

### 1.5 The module SDK

`src/core/mods/modules/dsmod_module_sdk.h` collects helpers the shipped modules used to copy
privately. The Mario Kart 8 Deluxe, Metroid Dread, Persona 5 Royal and Super Mario Bros. Wonder
modules all include it. It depends only on the two ABI headers and the standard library.

| Helper | Purpose |
|---|---|
| `dsmod_sdk::int_types` | `u8` … `s64` aliases |
| `Le16/32/64`, `Be16/32/64` | Byte-order loads from a raw pointer (the caller checks bounds) |
| `Fnv1a64`, `Fnv1a64Step` | FNV-1a 64-bit hashing for code fingerprints and key hashes |
| `InMain(base, size, at, n)` | Overflow-safe "is this range inside that image" check |
| `ReadGuest<MissingIsMapped>`, `RangeMapped`, `Get` | Guest reads over `EdenDsmodHostApi`, with an explicit policy for a host that leaves `is_mapped` null |
| `HostAbiMatches(host)` | The ABI prologue every `create()` starts with |
| `DSMOD_SDK_EXPORT_MODULE`, `_EXTENSIONS`, `_FONT_EXTENSIONS`, `_WRITE_EXTENSIONS`, `_DATA_EXTENSIONS` | Define the `extern "C"` getters, each returning the table only for the exact version and hash |

There is no `DSMOD_SDK_EXPORT_*` macro for the save extension; a module that wants it defines
`eden_dsmod_get_save_extensions` itself. Which getters end up visible in the `.so` is decided by
the target's `EXPORTS` list (§4), not by the macros.

## 2. Optional extensions

**Negotiation.** Each extension has its own exported getter, version and hash, and the host
passes its own version and hash to the getter. The loader requires the returned struct's
`version`, `struct_size == sizeof(…)` and `abi_hash` to match exactly. A mismatch is a load
error.

**Why separate symbols.** Extensions are separate symbols, never new fields in an existing
struct. Growing a struct would change its `sizeof` and break every module already built.

**When absence is an error.** A missing symbol only means "not offered". The exception is
`eden_dsmod_get_extensions`, which is required when the module sets `EDEN_DSMOD_CAP_EXTENSIONS`.

### 2.1 Base extensions: `eden_dsmod_get_extensions`

Constants: `EDEN_DSMOD_EXT_VERSION 1`, `EDEN_DSMOD_EXT_HASH 0x719d8b206e4fa351`.

| Callback | Purpose |
|---|---|
| `configure(inst, const EdenDsmodHostExtensions*)` | **Required.** Receives the guest mailbox (`mailbox_address`, `mailbox_size`, plus aligned atomic `load_u32/u64` and `store_u32/u64` limited to the mailbox) and `decode_astc` |
| `on_action(inst, action, argument)` | Handles `{"kind":"module","action":…,"argument":…}`. Return true for "accepted", **not** "done". Publish the outcome through normal values. Since runtime 16, returning false (or throwing) **refuses** the action: the package's `refused` haptic plays and nothing after it runs. Older runtimes ignored the return value |
| `load_image(inst, host, key, receiver, sink)` | Decodes a `module:` key (≤4096 characters since runtime 16). Call `sink(receiver, w, h, rgba, w*h*4)` once, with straight RGBA8, at most 4096×4096 and 16 MiB. Runs on the asset worker, so keep decoder state isolated from `sample`. A paged font atlas (runtime 17) asks for each page as its own key, within the same limits |

The guest mailbox exists only when the package has a `load_plan`: load-time code patches that
reserve a mailbox in the executable. None of the published modules uses it; they all read the
game directly.

### 2.2 Font: `eden_dsmod_get_font_extensions`

`decode_font(inst, bytes, size, receiver, sink)` is called synchronously with the raw bytes of
the manifest's `font` asset. If no decoder recognises the bytes and the module exports
`decode_font`, the host tries again about once a second, up to 30 times (runtime 14), so a
decoder that needs the running game can succeed once it is ready. Report the result through the
sink:

- `line_height`;
- `first_codepoint`;
- one `EdenDsmodFontGlyph` per glyph: `x`, `y`, `w`, `h`, `bearing_x`, `bearing_y` and
  `advance`.

Return false if you do not recognise the bytes; the host then tries its built-in parsers. With a
paged atlas (`font_page_h`, runtime 17) report glyph y in the virtual atlas of all pages stacked
top to bottom; the host splits it into pages.

### 2.3 Save (read-only): `eden_dsmod_get_save_extensions`

`configure(inst, const EdenDsmodHostSaveApi*)` is called once, right after `create`. Copy the
struct you receive.

`read_save_file(relative_path, offset, out, size)` reads only inside the running title's own
save directory:

- relative paths only: forward slashes, no `..`, at most 256 characters;
- at most 64 MiB per call;
- `out == NULL` returns the file size.

There is deliberately no write path. Re-read on a slow schedule, not every tick. No shipped
module uses this extension yet.

### 2.4 Write batch: `eden_dsmod_get_write_extensions`

Constants: `EDEN_DSMOD_WRITE_EXT_VERSION 1`, `EDEN_DSMOD_WRITE_EXT_HASH 0x5e3b9d17a2c4f680`,
`EDEN_DSMOD_WRITE_BATCH_MAX_OPS 16`, `EDEN_DSMOD_WRITE_BATCH_MAX_BYTES 64`.

`configure(inst, const EdenDsmodHostWriteApi*)` is called once, right after `create`; copy the
struct. It hands you `write_batch(userdata, ops, count)`, which applies up to 16
`EdenDsmodWriteOp`s as one unit. Each op is `{address, size (1–64), reserved (0), expect, value}`;
a null `expect` skips the check for that op.

- **Validate first.** Every op is checked before anything is stored: size and `reserved`, a
  non-null `value`, and a mapped range.
- **Then compare and store.** With guest threads suspended (multi-core, through
  `System::RunWithGuestThreadsSuspended`) or between time slices (single-core, where `sample` and
  `tick` already run with no guest code executing), the host re-reads every op's `expect` bytes.
  If any differ, nothing is written. Otherwise every `value` is stored.
- **Failure.** The call returns false when the batch was not applied, including when the guest
  cannot be paused right now (a pause or resume in progress). Retry on a later sample.

The host side is `InitializeModuleWriteExtensions` in `mod_module_services.cpp`. Use this for
state the game must never see half-changed, such as reordering a list. Single stores go through
the base `write_memory` service instead.

### 2.5 Data: `eden_dsmod_get_data_extensions`

Added in runtime 12. Constants: `EDEN_DSMOD_DATA_EXT_VERSION 1`,
`EDEN_DSMOD_DATA_EXT_HASH 0x3a91c7e05bd2f648`.

```c
EdenDsmodBool (*load_data)(void* instance, const EdenDsmodHostApi* host, const char* key,
                           void* receiver, EdenDsmodDataSink sink);
typedef void (*EdenDsmodDataSink)(void* receiver, const uint8_t* bytes, size_t size);
```

- **What it serves.** Opaque bytes behind a `module:` source that the runtime reads as a file,
  not as an image: map `geo` blobs, the JSON object named by `map.areas_src`, or any other byte
  source read through `ReadAssetBytes`. This lets an asset-free package generate game-derived
  data from the player's own romfs instead of shipping it.
- **Contract.** `key` includes the `module:` prefix. Call `sink` at most once and return true, or
  return false for an unknown key or a failed generation. The bytes are borrowed for the call;
  the host copies them (at most 64 MiB). Calls can come from any runtime thread, are serialized by
  the host, and may block while the module finishes generating.
- **Host side.** `LoadModuleData`, `StartModuleAreas` and `InstallModuleAreas` in
  `mod_module_services.cpp`; see [ARCHITECTURE.md §2.10](ARCHITECTURE.md#210-module-generated-data-runtime-12).
- **Versioning.** A package that depends on this sets `"min_runtime": 12`, so an older runtime
  shows its update page instead of an empty map.

### 2.6 What each shipped module exports

| Symbol | MK8D | Dread | P5R | Wonder |
|---|---|---|---|---|
| `eden_dsmod_get_module` | ✓ | ✓ | ✓ | ✓ |
| `eden_dsmod_get_extensions` | ✓ | ✓ | ✓ | ✓ |
| `eden_dsmod_get_font_extensions` | | ✓ | ✓ | ✓ |
| `eden_dsmod_get_write_extensions` | ✓ | | ✓ | |
| `eden_dsmod_get_data_extensions` | | ✓ | | |
| `eden_dsmod_get_save_extensions` | | | | |

Link's Awakening has no module.

These match the `EXPORTS` lists in `native/modules/CMakeLists.txt`. Dread's base
extensions provide `on_action` but no `load_image`.

## 3. Build ids and `supports_build`

Build support is checked in two places, and both must pass.

1. **The package.** `module.build_ids` in `manifest.json` lists the supported builds as 16- or
   64-character hex strings. The loader accepts the running build if it **starts with** any
   entry.
2. **The module.** `supports_build(build_hex)` is the module's own check, and should be
   stricter. P5R accepts only the exact 64-character ID, and `create` compares all 32 bytes
   of `host->build_id` again.

A module must **never imply support for an unlisted build**. Code offsets move between game
updates, so every supported build must be verified separately.

## 4. Build

Title modules live in this companions repository under `native/modules/`. It is a
**standalone CMake project**, never part of the APK build. Each module is declared with the
`dsmod_add_module(<target> TITLE <id> SOURCES … EXPORTS …)` helper in its `CMakeLists.txt`.

| Target | Output | Default build |
|---|---|---|
| `dsmod-p5r` | `01005CA01580E000.so` (`01005CA01580E000.cpp`, `p5r_reader_*.cpp`, `p5r_romfs_assets.cpp`) | No (`EXCLUDE_FROM_ALL`); build it by name |
| `dsmod-dread` | `010093801237C000.so` (`010093801237C000.cpp`, `dread_romfs.cpp`, `dread_rfl.cpp`, `dread_mapgen.cpp`, `dread_mapgen_util.cpp`, `dread_maproom.cpp`) | No (`EXCLUDE_FROM_ALL`) |
| `dsmod-mk8d` | `0100152000022000.so` (`0100152000022000.cpp`, `mk8d_reader.cpp`, `mk8d_ids.cpp`, `mk8d_anim.cpp`, `mk8d_assets.cpp`, code pins in `mk8d_pins*.inc`) | No (`EXCLUDE_FROM_ALL`) |
| `dsmod-acnh` | `01006F8002326000.so` | No (`EXCLUDE_FROM_ALL`) |
| `dsmod-fe3h` | `010055D009F78000.so` | No (`EXCLUDE_FROM_ALL`) |
| `dsmod-isaac` | `010021C000B6A000.so` | No (`EXCLUDE_FROM_ALL`) |
| `dsmod-wonder` | `010015100B514000.so` (`010015100B514000.cpp`, `wonder_assets.cpp`, `wonder_catalog.cpp`, `wonder_font.cpp`, `wonder_glyphs.cpp`, plus zstd's decompressor from the emulator's CPM cache) | No (`EXCLUDE_FROM_ALL`) |

Target properties that matter:

- **Output name.** `PREFIX ""` and `OUTPUT_NAME <TITLEID>`: the file is named by title ID.
- **Visibility.** `CXX_VISIBILITY_PRESET hidden`, plus `-Wl,--version-script=<exports>` and
  `-Wl,--no-undefined`. On Android, also `-Wl,--exclude-libs,ALL`.
- **Exports.** `dsmod_add_module` generates `<target>.exports`, a linker version script
  `{ global: <EXPORTS>; local: *; };`, from the target's `EXPORTS` list. Only the listed C
  getters become dynamic symbols. Nothing else leaks, including the statically linked C++
  runtime; a getter defined in the source but not listed stays private. Add the symbol to
  `EXPORTS` whenever you add an extension getter.
- **Dependencies.** Header-only only, such as fmt or nlohmann/json from the emulator's CPM cache.
  Never link the emulator's `core` or `common`.
- **Floating point.** The Dread map generator's files are compiled with `-ffp-contract=off`, so
  arm64 builds produce the same numbers as x86-64.
- **Android STL.** Android requires `-DANDROID_STL=c++_static`; the CMake file fails otherwise.
  The APK does not ship `libc++_shared.so`.

```sh
# Linux x86_64
cmake -S native/modules -B /tmp/mods-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DEDEN_SOURCE_ROOT="/absolute/path/to/eden-duo"
cmake --build /tmp/mods-linux --target dsmod-p5r

# Android arm64 (NDK r28c, API 24+)
cmake -S native/modules -B /tmp/mods-android -G Ninja \
  -DEDEN_SOURCE_ROOT="/absolute/path/to/eden-duo" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_SDK_ROOT/ndk/28.2.13676358/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/mods-android --target dsmod-p5r
```

Do not place the Android build directory under Gradle's `.cxx` tree. Gradle may package the
library into the APK.

### Tests

Host-side tests are built when `BUILD_TESTING` is on and the target is not Android:

```sh
cmake --build /tmp/mods-linux --target dsmod-p5r-reader-test dsmod-p5r-assets-test dsmod-p5r-dwrite-test
ctest --test-dir /tmp/mods-linux -R dsmod-
```

- **Available tests:** `dsmod-p5r-{reader,assets,dwrite,recipes,font,map-overlay,dialogue}`,
  `dsmod-dread`, `dsmod-dread-rfl`, `dsmod-dread-mapgen` and `dsmod-mk8d-{reader,anim,assets}`.
- **Real-data tests.** Some take real data through environment variables (for example
  `P5R_ROMFS` or `DREAD_ROMFS` pointing at your own romfs dump) and skip when it is absent.
- **Host-side tests.** The emulator's own Catch2 tests cover the loader with a fake module
  (`src/tests/core/mods/module.cpp`, `fake_module.cpp`), `min_runtime` gating (`min_runtime.cpp`)
  and module-sourced map areas (`map_areas_src.cpp`).

## 5. Packaging and SHA-256 pins

```json
"requires_module": true,
"module": {
  "abi": 1,
  "build_ids": ["D4B150B29A931CD381E2A30318FC299F2B0EE36D000000000000000000000000"],
  "libraries": {
    "linux-x86_64":      {"path": "modules/linux-x86_64/01005CA01580E000.so",      "sha256": "<64 lower-case hex>"},
    "android-arm64-v8a": {"path": "modules/android-arm64-v8a/01005CA01580E000.so", "sha256": "<64 lower-case hex>"}
  }
}
```

- **Where the block lives.** Put the same block in `package.json` and `dualscreen/manifest.json`.
  The runtime validates the manifest copy; the Android installer requires the two to be equal.
- **File placement.** Each library goes at `dualscreen/modules/<platform>/<TITLEID>.so`. The
  Android installer rejects any other file under `modules/`, so place data files such as P5R's
  `p5r_art.rec` elsewhere in `dualscreen/`.
- **What the loader checks, in order:**
  1. the SHA-256, compared case-insensitively;
  2. the file is copied into a private, freshly created directory as a read-only file (on
     Android, the app's code-cache directory);
  3. the copy is `dlopen`ed.

  A mismatch logs "checksum mismatch; reinstall the package", and the pages run without the
  module.
- **Tooling.** This repository's `tools/build_dualscreen_package.py --module <platform>=<path.so> --abi 1
  --build-id <ID>` copies the library, computes the hash and writes the block into both files.
  Always rebuild the package after rebuilding a module; a stale hash disables it.

## 6. Rules the project follows

These rules come from the project's working brief and from experience. Each is enforced in the
P5R code.

| Rule | Why |
|---|---|
| **Bounded, validated reads.** Every read goes through `is_mapped` + `read_memory`, with overflow checks. Addresses derived from the game are bounded to the main image (`InMain`) or a known structure | A bad pointer must fail closed, not crash or show garbage |
| **Resolve globals from accessor instructions, not hardcoded data addresses.** Read the game's own getter at a build-pinned code offset, check its exact opcodes, and decode the ADRP + LDR/ADD pair to find the global | Updates move `.bss` and data. P5R's scene slot moved by 0x20 between builds while the getter's shape stayed the same |
| **No heuristics.** Every published value and rule is derived from the game's code or data, and verified live against the game's own UI. No invented thresholds, no guessed ranges | A heuristic that is right today is wrong on the next save |
| **Reject torn reads.** The guest runs while you read. Read twice and compare, and re-check the context (scene, party, dialogue header) after the reads. On a mismatch, publish "not ready" | Half-updated structures produce plausible but wrong values |
| **Fail closed per domain.** One domain's resolve failure hides that domain (`persona_resolved`, `inv_resolved`, …), and diagnostics explain why | The rest of the companion keeps working |
| **Lazy publishing.** Heavy lists are read and published only while their page is open. Output that the manifest never names is not published | Keeps `sample` cheap. Tick time competes with the game |
| **Report sample cost.** Measure average µs per `sample` | See the rule in [ARCHITECTURE.md §5](ARCHITECTURE.md#5-performance-design) |

## 7. Worked example: Persona 5 Royal (`01005CA01580E000`)

### 7.1 Structure

```
01005CA01580E000.cpp       ABI glue: module table, create/destroy/sample/tick (tick is empty),
                           the extension tables (OnAction, LoadImage, DecodeFont,
                           ConfigureWrite) and the exported getters
p5r_reader.h               class Reader, ArtLibrary and the shared snapshot structs
p5r_reader_core.cpp        Resolve / Global / Sample / ReadSnapshot / publish plumbing
p5r_reader_party.cpp       party members, camp rosters, member names
p5r_reader_persona.cpp     persona stock, skill lists, skill help text
p5r_reader_inventory.cpp   items, equipment, party stats, camp-menu cursor mirror
p5r_reader_social.cpp      social stats, confidants, calendar, requests, DATA2 extras
p5r_reader_battle.cpp      Analyze panel, battle2 lists, battle party personas and skills
p5r_reader_map.cpp         field map, overlay, player icon
p5r_reader_dialogue.cpp    dialogue text and choices
p5r_reader_bgm.cpp         now-playing BGM
p5r_reader_drive.cpp       native-menu button drives (pdrv.*)
p5r_reader_direct.cpp      direct state changes (guarded guest writes)
p5r_reader_actions.cpp     module actions (the dispatcher chain) and companion page tracking
p5r_reader_assets.cpp      ArtLibrary, recipe prewarm, game-text load, load_image / decode_font
p5r_romfs_assets.cpp/.h    CPK/CRILAYLA/DDS/SPR0/PAK decoders
p5r_task_walk.h            one shared walk of the game's registered-task lists per sample
p5r_*_reader.h             header-only game-structure decoders (persona, inventory, social,
                           social_menu, data2, battle2, enemy, analyze, map, bgm, dialogue)
p5r_game_text.h            names/tables loaded from romfs
p5r_recipes.h              image recipe engine (module:p5r:<id>)
p5r_font.h                 game font decoder
p5r_prewarm.h              background recipe prewarm
p5r_publish_filter.h       drop outputs the manifest never names
p5r_direct_write.h         reproduced menu actions as guarded writes
```

- **Domain readers.** The header-only decoders expose `Resolve(read, main_base, main_size,
  roots)` and `Sample(read, roots)`; the `p5r_reader_*.cpp` area files call them from `Reader`.
- **Re-resolve.** `Reader::Sample` (`p5r_reader_core.cpp`) resolves again whenever `main_base`,
  `main_size`, the title or the build changes.
- **`tick()` is empty.** All work happens in `sample`.

### 7.2 Resolving a global from its accessor

The module never trusts a hardcoded data address. It first checks the exact instruction words of
the game's getter, where only the immediates may vary. Then it decodes the address. From
`Reader::Global` (`p5r_reader_core.cpp`):

```cpp
// Resolve ADRP + unsigned-immediate LDR/ADD. Require the expected opcode and registers;
// only page/offset immediates may vary.
if ((adrp & 0x9f00001f) != (0x90000000u | ((access >> 5) & 31)) ||
    (access & ~0x003ffc00u) != expected_access) return false;
...
if (result <= 0 || !InMain(static_cast<u64>(result), 8)) return false;
```

- **Fingerprinting longer routines.** Longer routines are fingerprinted with an FNV-1a hash of
  their bytes. A mismatch fails that domain closed.
- **Offsets are build-pinned.** The getter offsets, such as `MoneyGetter = 0x8808c0`, are code
  offsets for the supported build. On a new build, re-find the getters and re-check their
  shapes.

### 7.3 Torn-read rejection

From `p5r_social_reader.h`:

```cpp
out.confidant_ready = ok && ReadConfidants(read, roots, table_again, again) &&
                      table_again == table && again == bytes;
```

- **Whole snapshot.** `Reader::ReadSnapshot` ends by re-reading the field context, the party IDs
  and the scene. If a transition began during the reads, the whole sample is rejected.
- **Dialogue.** The dialogue reader compares its header before and after decoding. On a
  mismatch it clears the text and sets `torn`.

### 7.4 Lazy publishing

1. **Page tracking.** The module cannot see which companion page is showing, so the manifest
   tells it. The START menu sends a module action (`ui_open <page>`), and the module publishes
   `ui.page`, which `page_binds` follow. Heavy readers run only while their page is open.
   Slower data refreshes on its own period.
2. **`p5r_publish::Filter`** (`p5r_publish_filter.h`).
   - At `create`, it scans the manifest JSON for every string that could be a value name. It
     reduces each name to a "family" by replacing digit runs, `{i}` and `%d` with `#`, then
     hashes the family.
   - `Wants(key)` drops indexed keys whose family the package never names.
   - `EDEN_DSMOD_P5R_ALL_OUTPUTS=1` disables the filter for diagnostics.

### 7.5 Asset-free art

The package ships no images, fonts or game text. At runtime the module decodes them from the
player's own romfs, through `host.read_romfs`:

- **Archive access.** `p5r_assets::Romfs::Open` parses the CPK tables of contents once. The main
  archive is 15 GB, so it is only ever read in ranges. The update archive overrides the base.
- **Formats.** Decoders cover CPK @UTF, CRILAYLA, DDS (DXT1/3/5, BC4/5, BC7), SPR0 sprite sheets,
  Atlus PAK, and the game's `.FNT` font. All are bounds-checked and fail closed.
- **Recipes.** `module:p5r:<id>` keys are **recipes**: short stack-machine programs (crop,
  resize, rotate, composite, draw text) stored in the package file `p5r_art.rec`. The engine in
  `p5r_recipes.h` reproduces the page-generator's Pillow integer maths exactly, so the output
  matches the offline design pixel for pixel. The package ships the recipe program, not the
  pixels.
- **Font.** The manifest's `font` is `file:p5r_art.rec`. `DecodeFont` recognises the recipe
  table's magic and decodes the game's own font from romfs instead.
- **Prewarm.** `p5r_prewarm.h` prebuilds the menu pages' recipes on a nice-19 background thread,
  within a 32 MiB budget, so the first menu open is a cache hit.

### 7.6 Actions: direct writes with guards

- **What is reproduced.** Camp-menu actions (use item, equip, change persona) are reproduced as
  data changes. The changes were derived from disassembly of the game's own menu routines, for
  the deterministic branches only.
- **Guards.** Each store goes through `Reader::DirectWrite` (`p5r_reader_direct.cpp`), which
  re-reads the target and requires it to still equal the expected value:

  ```cpp
  if (size == 0 || size > now.size() || !InMain(at, size) || !ReadBytes(at, now.data(), size) ||
      std::memcmp(now.data(), expect, size) != 0) return false;
  ...
  return host.write_memory(host.userdata, at, value, size) != 0;
  ```

- **Multi-field changes.** Rotating the persona stock uses `write_batch`, so the game never sees
  a half-rotated list.
- **Fallback.** Branches that were not reproduced (random outcomes, revives and similar) fall
  back to **driving the native menu with button presses**:
  - the module publishes `pdrv.*` gates;
  - the manifest's `enforce` rules press the buttons.
- **Off switches** for direct writes: manifest `"p5r_direct_writes": false`,
  `EDEN_DSMOD_P5R_DIRECT_WRITES=0`, or the module action `direct_writes 0`.
- **Status outputs:** `pdrv.direct` and `pdrv.direct_on`.

See [PORTING_A_GAME.md §5](PORTING_A_GAME.md#5-driving-the-native-menu) for the tradeoffs between
button presses, writes and guest calls.

## 8. Worked example: Metroid Dread map data (`010093801237C000`)

The Dread package is asset-free: it ships the authored map template, and the module builds the
game-derived map data from the player's own romfs at game load. It is the reference user of the
data extension (§2.5) and `map.areas_src`.

### 8.1 Files

```
010093801237C000.cpp       the module: live state readers, map publishing, MapJob,
                           load_data, the exported getters
dread_romfs.cpp/.h         romfs access: an asset by path, from the .pkg that lists its CRC-64
                           asset id, else a loose file; ranged reads only
dread_rfl.cpp/.h           Mercury Engine reflection reader for .bmmap and .brfld files
dread_rfl_types.inc        generated type layouts (modules/tools/gen_dread_rfl_types.py)
dread_maproom.cpp/.h       the minimap room model (.bcmdl) and collision cameras (.bmscc)
dread_mapgen_util.cpp/.h   inflate/gunzip, Python-exact round and hypot
dread_mapgen.cpp/.h        Generate(): the map-areas object and the geometry blobs
```

### 8.2 Flow

1. **Package.** The manifest sets `"map": {"areas_src": "module:dread:areas", "areas": {…}}`. Each
   inline area is an authored template whose `geo` and layer `geo` sources are `module:` keys,
   for example `module:dread:map/s010_cave.geo`. The manifest declares `"min_runtime": 12`.
2. **Start.** `create` sees that `areas_src` and starts a `MapJob` on its own thread at a lower
   OS priority. The job's inputs are the template areas, the `map.icons` names and
   `map.style.raster_px`.
3. **Generate.** `dread_mapgen::Generate` reads the level files (`.bmmap`, `.brfld`, `.bmscc`)
   and maproom models through `host->read_romfs`, and produces:
   - `areas`: the template completed with the derived fields (bounds, icons, camera rects,
     occluders, vignettes, overview regions, water pools, room-category polygons);
   - `blobs`: every `.geo` the template references, keyed `map/<area>[.<layer>].geo`.
   The job also loads the game's own localization table for page labels.
4. **Serve.** `load_data` answers `module:dread:areas` with the areas JSON and
   `module:dread:<blob key>` with a geometry blob. It blocks until the job is done and returns
   false if generation failed.
5. **Install.** The runtime's map-areas thread fetches `module:dread:areas`, and the tick thread
   installs the parsed areas ([ARCHITECTURE.md §2.10](ARCHITECTURE.md#210-module-generated-data-runtime-12)).
   Geometry blobs are fetched through `load_data` as the rasteriser needs them. On the module
   side, `Reader::InstallMapJob` moves the generated areas into the module's own copy of the
   manifest on the tick thread, and the localized strings are published as `loc:<KEY>` values,
   gated by the `need_bind` of the labels that show them.

### 8.3 Exactness

The generator replays the offline tools that produced the earlier (1.0.0) package's map files,
aiming at byte-identical output. Python's `round` and `hypot` are reproduced bit-exactly
(`dread_mapgen_util`), and the generator's files are built with `-ffp-contract=off` so arm64
matches x86-64. `dsmod-dread` and `dsmod-dread-rfl` read a real romfs when `DREAD_ROMFS` is set;
`dsmod-dread-mapgen` takes the romfs and manifest paths as command-line arguments.

## 9. Checklist for a new module

- [ ] `eden_dsmod_get_module` returns a table with exact sizes, version, hash and title ID.
- [ ] `supports_build` accepts only builds you have verified.
- [ ] Every callback catches all exceptions.
- [ ] All reads are bounded and checked; torn reads are rejected; each domain fails closed.
- [ ] Globals are resolved from accessor code, with opcode checks.
- [ ] Every value is published on every `sample`, heavy ones only when needed.
- [ ] `load_image` shares no mutable state with `sample` without a lock.
- [ ] The target's `EXPORTS` list names every getter you export.
- [ ] Data the package would otherwise ship from game files is served through `load_data`, and
      the package declares `"min_runtime": 12` if it relies on that.
- [ ] `on_action` returns false only to refuse an action (runtime 16 plays `refused`).
- [ ] Optional sources (`aoc:`, `user:`) are checked with `__source:<prefix>` and have a fallback.
- [ ] A module that sets `TICK_WHEN_HIDDEN` / `NO_TICK_WHEN_HIDDEN` ships in a package with
      `"min_runtime": 15`.
- [ ] Linux and Android builds are done, hashes are pinned, and the package is rebuilt.
- [ ] Sample cost is measured.


### Font refresh (runtime 18)

Publish `__font_epoch` as an integer whenever the font's glyph set changes. Check the host's `EDEN_DSMOD_CAP_FONT_EPOCH` before relying on refresh; do not add that bit to the module's required capabilities, because older hosts reject unknown requirements. The module ABI remains version 1. Decode requests are bounded and serialized with image decoding; preserve valid decoder state if a refresh is declined.
