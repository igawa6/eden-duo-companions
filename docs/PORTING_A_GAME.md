# Porting a game to Eden Duo

This is the method used to build companions for Persona 5 Royal (P5R), Link's Awakening (LA),
Hollow Knight (HK) and Metroid Dread. It works from your own legally obtained copy of the game.
Nothing extracted from it is shipped.

```
 1 dump           2 find state          3 validate          4 build package
 romfs + exefs ─► disassembly, gdb, ─►  live vs native ─►   manifest + data file
 build id         cheats, registries,   menu, sentinel      (+ module), asset-free
                  saves, live scans     tests, 2 boots
                                                              │
 7 deploy ◄──────────── 6 test headless ◄───── 5 actions ◄────┘
 Android handheld       eden-cli, .btn/.shot   buttons / writes / calls
```

## 1. Dump the game

1. **If your dump is NSZ, convert it to NSP.** This workflow uses NSP. The pip package `nsz`
   decompresses with your own keys and prints `[VERIFIED]` for each NCA:
   ```sh
   nsz -D -V -w -o out/ game.nsz
   ```
2. **Dump romfs and exefs** with Eden Duo's CLI:
   ```sh
   eden-cli --dump-romfs <outdir> game.nsp   # exefs files (main, main.npdm, rtld, sdk, ...) + romfs
   ```
   Also useful:
   - `--dump-il2cpp <outdir>` writes only the exefs files plus `global-metadata.dat`, for Unity
     games.
   - `EDEN_DSMOD_DUMP_ROMFS="<romfs path>:<out>"` saves one file from the *running* game's merged
     base-plus-update romfs. This is the easy way to get files that an update changed, because
     an update NSP on its own is only a delta.
3. **Record the build ID.** It is 32 bytes at offset 0x40 of the `main` NSO:
   ```sh
   xxd -s 0x40 -l 32 -p main
   ```
   The first 8 bytes, in upper-case hex, name the data file `<BUILD16>.json`. **Work on the build
   that actually runs**, meaning base plus update. Offsets from the base NSO are wrong for an
   updated game.
4. **Prepare the executable for static analysis.** Decompress the NSO and apply its relocations
   (small scripts, not part of this repository). The image then matches runtime `main+X`
   offsets, and vtables and pointer tables hold real offsets.
   - Alternatively, read the loaded image out of a running game over the gdbstub (§2.3).
5. **Very large archives.** One P5R archive is 15 GB. For those, extract the romfs directly with a
   small NCA/RomFS parser instead of the emulator's dump path.

## 2. Find the state

Static analysis was done with custom Python scripts on top of capstone (not part of this
repository). Live analysis uses the runtime console and the gdbstub. The methods below are listed from most to
least reliable.

### 2.1 Anchors from public cheats

Public cheat databases give fast first anchors. For example, HK's `PlayerData` sits at
`[[main+0x027EDE78]+0xA0]+0`, and P5R's money and party records were found the same way. ASM
cheats are especially useful because they show the code that accesses the value.

Treat every cheat as a lead, not a fact, and confirm it from code.

### 2.2 Accessor disassembly

Find the game's own getter for the value. Record the getter's **code offset** and resolve the
global's address from its ADRP + LDR/ADD pair at runtime. Updates move `.bss`, but getters keep
their shape. [MODULE_GUIDE.md §7.2](MODULE_GUIDE.md#72-resolving-a-global-from-its-accessor)
shows the check in code.

### 2.3 gdbstub (GDB remote serial protocol)

Enable the gdbstub in an **isolated** profile's config (`use_gdbstub=true`, `gdbstub_port=…`). A
small stdlib RSP client is enough: `read`, `write`, `regs`, `threads`, `watch`, `continue`,
`monitor info`.

| Behaviour | What it means for you |
|---|---|
| Connecting pauses emulation | |
| There is no detach | Closing the socket while stopped leaves the game frozen, so always end with `c` |
| `c` gets no reply until the next stop | |
| The `watch:` field in a stop reply is the watchpoint's start address | It is not the address that was accessed |
| A write watchpoint drops the store and rewinds the PC | Remove the watchpoint, single-step, then re-insert it |
| Only 4 watchpoints; `Z1` is rejected | |

`monitor info` prints module bases. A watchpoint on a value you can see change is the fastest
way to find the code that writes it.

### 2.4 Script-command registries

Games with a scripting layer register native functions by name. Those tables map names to code.

- **P5R flowscript.** Records are `{fn u64, argc u64, name ptr u64}`, with a stride of 0x18.
  Following `GET_MONTH` → its function → the date getter gives the date global. The same method
  found the scene, dungeon, message and choice commands.
  - Get the field order right. Misreading it as `(name, fn, argc)` shifted every ID by one row.
- **Dread (Lua 5.1).** First name the Lua C API statically, using an anchor chain of recognisable
  instruction patterns and string literals. Then read the `{name, lua_CFunction}` registration
  tables.
  - `.data` pointers are unrelocated in the file. Read the tables from the running module, or
    relocate the image first.
  - This build uses a **float** `LUA_NUMBER`.
- **IL2CPP (HK).** Run Il2CppDumper on `global-metadata.dat`. Singletons are
  `[[<Type>_TypeInfo+0xA0]+0]`. Methods resolve by name at runtime (`symbols`:
  `{"class_name":…,"method":…}`).
  - The class layout depends on the metadata version, so take the offsets from the generated
    `il2cpp.h`.

### 2.5 Save-file decoding

A save file is a static map of what the game keeps and in what shape.

| Game | Save format |
|---|---|
| P5R | CRC, then AES-CBC, then zlib, then tagged blocks |
| LA | A raw RAM copy: save offset S is live at a fixed base + S |
| Dread | Blackboard names with type tags |

- **Diff two saves** to locate structures, then confirm the live address from code.
- **Beware snapshots.** Some structures are save-only snapshots. Dread's blackboard minimap data
  does not grow while you play; the live grid is a different object.

### 2.6 Live scans

Use these when no pointer route exists. They need a build with `EDEN_DSMOD_BUILD_DEV_TOOLS=ON`.

**The console.** Set `EDEN_DSMOD_CMD=<file>` and write one command per line. Results go to the
log and to `<file>.out`.

| Group | Commands |
|---|---|
| Read | `readi`, `readf`, `hexdump`, `chain`, `value` |
| Search | `findi`, `findf`, `ptrto`, `vfind`, `objfind`, `cluster`, `pairfind`, `sfind` |
| Differential | `snap` / `diff`, `cscan` / `cnarrow` / `clist`, `isnap` / `idiff` |
| Poke and drive | `writeb`, `tap`, `drag`, `page`, `action`, `reload` |
| Assets | `imgdump <src> <out.png>`, `msbt` |

**The scanners** (environment variables): `EDEN_DSMOD_FIND`, `FINDFLOAT`, `DIFF`, `SCAN` (a
position found by motion), `MOTION`, `RANGE`, `FIELD`, `HEAPFIND`, `HEAPDUMP`, `CLASSDUMP` and
`ARRAY`.

Scanning rules that avoid confident wrong answers:

- **Rank by agreement** with the value the game displays, never by how often a value changes.
- **Seed after gameplay starts.** Search floats as well as integers.
- **Score behaviour separately per state.** When finding the player by motion, score "pad held"
  and "pad idle" separately, over at least 45 frames each.
- **Turn the result into a chain.** A heap address is valid for one boot only. Convert it into a
  pointer chain from a static root (the console's `ptrto`), then test the chain on a second
  boot.

## 3. Validate live against the native menu

Each published value must match what the game itself shows.

1. Use an **isolated profile copy**, never your real saves.
2. Boot headless (§6) and drive to a known save.
3. Open the game's **own** menu. Capture `-screen1.png` (the game) next to `-screen2.png` (the
   companion) for every field.
4. **Sentinel test:**
   - change the value reversibly in RAM, for example money 87398 → 87399, with `writeb`;
   - confirm that **both** the native menu and the companion follow;
   - restore the value, and do not save.
5. Repeat on a **second boot with a different save**, which also gives a different ASLR base.
6. For actions, perform the action natively and through the companion. Then diff the persistent
   state byte for byte. They must be identical.
7. Record the module's `sample` cost in µs.

A value that cannot pass these steps is not published.

## 4. Build the package

1. **Data file.** `<BUILD16>.json` holds the points, symbols and patches for this build.
2. **Manifest.** `manifest.json` holds the pages, widgets, derived values, actions and
   `page_binds`. Keep compound logic in `derived` entries, because gates are single names.
3. **Native module** (optional). Add one when chains are not enough; see
   [MODULE_GUIDE.md](MODULE_GUIDE.md).
4. **`min_runtime`.** Set it to the runtime version whose features you use (currently 12). Use 12
   if the package reads `module:` byte sources or `map.areas_src`, 11 for scroll regions or
   `module:` composite layers.
5. **Build** with this repository's package builder:
   ```sh
   python3 tools/build_dualscreen_package.py --package packages/<Game> --output out/ \
       --version 1.0.0 [--module android-arm64-v8a=<so> --module linux-x86_64=<so> --abi 1 --build-id <ID>]
   ```
   The builder is reproducible: it sorts entries and fixes timestamps. It checks `file:`
   references and drops investigation artefacts such as caches, `.bak` and `.log` files.

### Asset-free packages

The package ships **layout and code, never game assets**. All art, fonts and text are decoded at
runtime from the player's own files.

| Need | Declarative route | Module route |
|---|---|---|
| A texture | `"src": "romfs:/path/file.bntx#tex"`, plus `src_rect` for a sprite from a sheet | `module:<key>`, decoded in `load_image` from `read_romfs` bytes |
| Derived data (map geometry, map areas) | None: without a module it has to ship as `file:` data you generated | `module:<key>` byte sources served by `load_data`, plus `map.areas_src` (runtime 12) |
| Layered art | `composites` whose layers are `romfs:` or `module:` sources | The same |
| Font | `"font": "romfs:/…bffnt"` (built-in parsers) | `decode_font` for proprietary formats |
| Text | `msbt` aliases and `text_src: "msbt:<alias>#<label>"` | Module decodes its own tables |

- **P5R** goes furthest. Its art is a table of *recipes*: small programs that crop, scale,
  composite and draw game art. They reproduce the offline page designs exactly
  ([MODULE_GUIDE.md §7.5](MODULE_GUIDE.md#75-asset-free-art)).
- **LA, Dread and Story of Seasons** use host-side decoders (BNTX, SARC, BFFNT, MSBT, bctex,
  lzs/xtx).
- **Dread** also generates its map geometry and map areas in the module from the game's level
  files ([MODULE_GUIDE.md §8](MODULE_GUIDE.md#8-worked-example-metroid-dread-map-data-010093801237c000)).
- **Caching.** Decoders run on worker threads and results are cached.
- **`file:` sources.** These are fine for material *you* authored. Never use them for extracted
  game art.

## 5. Driving the native menu

A companion that *changes* game state has three options.

| Route | How | Pros | Cons |
|---|---|---|---|
| **Button presses** | `button` actions, or module-published gates + `enforce` rules that press them (a verified state machine over the game's menu) | The game makes the change itself, with every side effect. Works under **NCE and Dynarmic** | Slow and visible. Needs menu-state RE. Can be derailed by unexpected dialogs |
| **Direct writes** | `write` / `slot_write` actions, or module `write_memory` / `write_batch`, guarded by expect-bytes | Instant. **NCE-safe** | You must reproduce every side effect of the native routine exactly. HUDs or caches can go stale |
| **Guest calls** | `call` / `sequence`: a breakpoint at a per-frame hook borrows a game thread to call a game function | The game's own code runs, with exact semantics | **Dynarmic only.** Does not work under NCE, the Android default |

How the shipped games use these:

- **P5R** uses direct writes for the deterministic menu branches. Those writes were derived from
  disassembly and verified byte-identical against the native menu. Everything else falls back to
  button presses.
- **LA** equips items with plain writes; the game's HUD refreshes on its own.
- **HK** uses guest calls on desktop, for example to refill soul and refresh the HUD.

### NCE vs Dynarmic

On ARM64 Android, Eden Duo (like upstream Eden) defaults to **NCE**, which runs guest code natively on the CPU. Desktop
x86-64 always uses Dynarmic, the JIT. So **NCE behaviour can only be tested on the device.**

| Under NCE | Consequence | Fix |
|---|---|---|
| Guest `BRK` breakpoints never reach the runtime | `call`, `sequence` and spies are disabled (a warning is logged) | Button presses or writes. Forcing Dynarmic on the device works but costs frame rate |
| Code patched after load needs real instruction-cache maintenance | Can hang or black-screen | Mark such patches `optional` (they apply only with `EDEN_DSMOD_PATCHES=1`), or use a load-time `load_plan` |
| The heap is at a different address | Fixed heap windows find nothing | Use `get_heap_begin/end` (from the page table) |
| The data segment sits at a constant offset from its Dynarmic position | `main+vtable` matches fail | Learn the delta once from a known structure (`get_i64("__relocation_delta")`); `.text` offsets are unaffected |

A guest-call bridge that works under NCE has been designed and compiled, but has not been run on
a device. Treat it as unavailable.

## 6. Test headless on Linux

```sh
env EDEN_VSYNC=0 EDEN_DSMOD_CMD=/tmp/run/cmd.in \
  build/bin/eden-cli --aux-virtual --screenshot-prefix /tmp/run/p \
  --filter "*:Info" game.nsp > /tmp/run/stdout.log 2>&1 &
```

- **Second screen.** `--aux-virtual` gives a windowless 1240×1080 second screen. Prefer it to
  `--aux-window` when headless: a compositor that hides the window stops frame callbacks and
  stalls the GPU thread.
- **Input: `<prefix>.btn`.** Write it atomically (write a temp file, then `mv`). It is consumed
  when read, one step per line:
  - buttons: `A`, `B`, `X`, `Y`, `L`, `R`, `ZL`, `ZR`, `Plus`, `Minus`, `DUp`, `DDown`, `DLeft`,
    `DRight`, combined with `+`, with an optional duration in ms;
  - `wait <ms>`, `hold <btn>`, `release <btn>`, `stick <x> <y> [ms]`;
  - touch steps normalised to the aux screen: `tap x y [ms]`, `drag x0 y0 x1 y1 [ms]`,
    `pinch cx cy s0 s1 [ms]`.
- **Screenshots: `<prefix>.shot`.** Touching the file, or sending `SIGUSR1`, writes
  `<prefix>-screen1.png` (the game) and `<prefix>-screen2.png` (the companion). On the GPU
  compositor path, also set `EDEN_DSMOD_GPU_READBACK=1`.
- **Readiness.** The log prints `DSMod snapshot: …` lines every 180 ticks. Wait for your
  gameplay signal there before capturing.
- **No display server?** Use a headless compositor (for example `cage` with a headless
  backend), or Xvfb with a software Vulkan WSI. Unity games stall on Xvfb.

## 7. Deploy to an Android dual-screen handheld

1. **Build the APK** ([README.md](README.md#android)) and the arm64 module
   ([MODULE_GUIDE.md §4](MODULE_GUIDE.md#4-build)). Rebuild the package so the module hash
   matches.
2. **Install the package.** Go to Add-ons → Install → **Dual-screen mods**, then pick
   `<TITLEID>.dsmod.zip`. The installer removes the title's older installer-created packages;
   disable any hand-made dual-screen folder for the same title.
3. **Env vars do not exist on Android.** Use manifest `flags` (for example `gpu_composite`) and
   the app settings (CPU backend) instead.
4. **Verify by capture.** The displays are secure, so `adb screencap` returns black. Use scrcpy
   against each display and stack the two frames. Capture before changing anything: someone may
   be holding the device.

## 8. Gotchas

| Symptom | Cause | Fix |
|---|---|---|
| Values are junk after a game update | `.bss` and data offsets moved | One data file per build; resolve globals from accessor code |
| Chain reads one level too deep or too shallow | The first `ptr` entry is the base address, not a read | N dereferences need N+1 entries |
| IL2CPP chain resolves but reads junk | Metadata version changes the class layout | Take offsets from the generated `il2cpp.h` |
| Script-command IDs off by one | Registry record fields misread | Verify the field order live |
| Heap or vtable scans find nothing on the device | NCE moves the heap and shifts data from text | Page-table heap bounds; learn the relocation delta |
| Two copies of a structure; the device shows a frozen one | Static template copies exist | Pick the copy whose values change |
| Frozen picture while audio plays | Heap sweep doing too many page-table reads per tick | Raw host-page reads, sliced across ticks |
| A performance regression passes every screenshot test | A full redraw is pixel-identical to a partial one | Measure tick-thread cost, not pixels |
| Offline registry scan finds nothing | `.data` pointers are unrelocated on disk | Relocate the image, or read the running module |
| Game freezes after a guest call | Returned to the hook address | Return through a trampoline |
| Guest calls or patches do nothing on the device | NCE | Buttons or writes; `optional` patches |
| Package silently ignored on an old APK | Old runtimes ignore unknown keys and fail quietly | Set `min_runtime`; ship APK and package together |
| "checksum mismatch; reinstall the package" | Module rebuilt without rebuilding the package | Rebuild with the builder, which rewrites the hashes |
| Android installer rejects the zip | A non-`.so` file under `modules/`, an upper-case hash, or `package.json` ≠ manifest `module` | Move data files out of `modules/`; use the builder |
| Direct `adb push` into the app's data dir fails, or files are unreadable | Ownership and permissions of shell-pushed files | Stage via `/data/local/tmp`, copy, then fix permissions; prefer the in-app installer |
| Wrong button pressed in `.btn` scripts | Tokens are case-sensitive; unknown tokens fall back to A | Use exact names (`DDown`, `Plus`) |
| Character keeps walking after a `stick` step | The stick does not recentre | Append `stick 0 0 100` |
| `.btn` loaded 0 steps | Script file read mid-write | Write a temp file, then `mv` |
| Headless run stalls, screenshots stop | Hidden aux window gets no frame callbacks | `--aux-virtual`, `EDEN_VSYNC=0` |
| Config edits lost | The emulator rewrites its config files on exit | Use `--config` / `--filter` and env overrides |
| Dread crashes about 30 s after boot on desktop | Known null-pointer crash on the second menu confirm | `EDEN_DSMOD_PATCHES=1` applies the package's guard patches |
| A 60 fps patch works on desktop but kills the device build | NCE patched-NSO path | Keep such exefs patches disabled on device |
| Garbled romfs images | Runtime asset reads shared the game's unlocked decryption layers | The runtime uses its own serialized romfs chain; do not bypass it |
| Near-black BC decode | Decoder expects `dst` already offset to the block corner | Offset `dst` to the block's corner before each block decode |
| `smGetService("dsm:u")` never returns | sm defers unknown service names forever | Probe with sm command 65100 first |
| Second screen not found on a handheld | The panel is listed only as a presentation display | The runtime already takes the union; check `AuxPresentation` if you fork it |
| Map looks blurred | Bilinear sampling or low raster size | Nearest sampling; raise `map.style.raster_px` |
| First page transition misses art | Snapshot taken before an async decode finished | Fixed via `asset_epoch` in the signature (runtime 11) |
| A `rect` draws a faint frame | Default outline | `"color": "#00000000"` |
| gdb session left the game frozen | Socket closed while stopped | Always `c` before disconnecting |
