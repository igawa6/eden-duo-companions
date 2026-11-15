# The Legend of Zelda: Skyward Sword HD companion

Title `01002DA013484000`, game version 1.0.1 (update v65536, main build
`5441AF3C89F97E1D25A8CCF14C742BF82336D8A8`). Requires Eden Duo 1.3.3 or newer (runtime 23). The
module refuses other builds; the companion displays an unsupported-build message. Current package:
`1.0.0`.

The eight pages cover:

- **Standby** on the title and Quest Logs.
- **Ready** on foot:
  - a round minimap that follows Link, with the stamina ring around it, the map compass, Gratitude
    Crystals and Goddess Cubes on its rim, and the area name below;
  - top left, the **Shield** corner: tap for a shield bash;
  - top right, the **ZR** corner: the current ZR item; tap to open Gear;
  - between the ZR and L corners, two spin buttons (horizontal and vertical spin attack);
  - bottom right, the **L** corner: it follows L (the eye while the game sees L held, the sword
    otherwise); tap to lock L for the free camera, tap again to release it; a lock badge shows
    on its top edge while it is locked;
  - bottom left, the **R** corner: the current R item; tap to open the Bag on its Pouch tab;
  - the left pane: rupees and the game's own D-pad cluster (display only);
  - the footer: **Gear**, **Map** and **Bag**.
  - Hold and drag the swipe pad to guide the sword; a quick stroke and release slashes. A gold
    sparkle trail follows the stroke. On Eden Duo 1.4.0 (runtime 25) or newer it is drawn under the
    finger; on Eden Duo 1.3.3 to 1.3.5.1 (runtime 23 and 24) it draws the stroke's shape out of the
    minimap centre, where Link's head is.
  - Tap the round map for a sword stab. It needs the sword drawn and is refused like the spins, and
    also in the Silent Realms and while swimming. The map rim lights up when the stab is sent.
  - When an action cannot run now, a dark pill says **Not now** for a moment: under the Shield
    corner or the spin button, on the minimap for a swipe or a map tap, and between the ring and the
    hub on Gear and Bag. When the reason is an item in Link's hands, it says **Put away** instead.
- **Flying** with the sky map (drag to pan, pinch to zoom, Reset), the Loftwing marker, altitude
  gauge, dash feathers, placed beacons and the beacons left.
- **Map**: drag to pan and pinch to zoom (from the whole area to close up), and a Reset plank back to
  the Link-centred view. Dungeons with floors (Ancient Cistern, Fire Sanctuary, Sandship) get the
  game's floor tabs.
  - Every stage has a map: eight field areas, the Shipyard and the sailing Sand Sea, seven dungeons
    (Skyview Temple, Earth Temple, Lanayru Mining Facility, Ancient Cistern, Fire Sanctuary, Sandship
    and Sky Keep), the four Silent Realms, the Isle of Songs, the Sky and the smaller stages.
  - Markers in the game's own colours: Link's view cone, bird statues, doors and locked doors,
    chests (opened ones marked), Goddess Chests, NPC balloons, area name plates and island
    names.
  - Dungeon maps show visited rooms until the Dungeon Map is owned. Bag > Options can show the
    layout before that.
- **Gear** with the current sword and shield, and the owned B-item wheel as a ring around a hub
  showing the current ZR item; the title bar shows the current R item.
  - Tap a ring item and then the hub, or drag it onto the hub, to make it the ZR item (the game's own
    ZR-wheel commit, run by the helper). Like the native wheel it works with the sword drawn. An item
    in Link's hands is put away first, and the next ZR press draws the new item.
  - Refused on the Loftwing, in the pause menu and map, and in cutscenes. A drop that arrives while
    another request runs, during a sword attack or a short event waits a moment instead.
- **Bag**, opened from the footer on its **Pouch** tab, with three tabs:
  - **Pouch**: the eight slots on a ring around a hub showing the current R item, beside the 60-slot
    Item Check storage in three scrolling columns. Scroll it with the wide strip on its right edge,
    or by dragging an empty slot or the gaps between the icons. Drag a ring item onto the hub to make
    it the R item, onto another ring slot to rearrange, or onto storage to store; drag storage onto
    the ring to retrieve. Tap then tap works too.
  - **Collection**: owned bugs and treasures as the game's icons with counts, scrolling.
  - **Options**: dungeon layout, the native Simple HUD, and the top-screen D-pad and rupees. The top
    D-pad and rupees are hidden by default because the bottom screen shows them. The Simple option
    is restored when switched off or when the companion unloads.

Every refusal is logged with its reason.

## Simulated inputs

The companion presses buttons for the player only in these owner-approved cases:

- **L hold** while the L corner is locked, added by the helper to the game's own button word
  (it lets go by itself a few frames after the companion stops).
- **L3** for one shield bash from the Shield corner.
- **R3 for 4 frames** for one sword stab from a tap on the round map.
- **Right-stick sequences** from the swipe pad and the spin buttons, through the helper's stick
  mailbox. The game decides what a stroke does.
- **Right-stick flicks** as the fallback of the swipe pad when the helper is not ready: a swipe up,
  down, left or right becomes one fixed 6-frame flick in that direction. The same refusals apply
  as for the helper's strokes (menus, events and cutscenes, the title and loads, the Loftwing,
  the Silent Realms, an item in Link's hands, no sword).
- **B for 4 frames** to put away an item in Link's hands, only right before a ZR equip from Gear.

The format-2 helper mirrors the right stick, adds these buttons (L, L3, R3 and B) to the game's own
button word only while the game is in play (never in the pause menu or the map, and the L hold not
during an event), and runs the pouch, storage and equip requests on the game thread. Physical
controls remain available.

The package ships no game art. Every picture is a `module:sshd:` key decoded from the player's own
romfs: U8 archives, GX TPL textures, RFNT fonts, MSBT item names, and LZ11 stage archives containing
KCL collision meshes. Parsed room geometry and decoded layout textures are cached per module instance.

## Sources and checks

| Path | Purpose |
|---|---|
| `dualscreen/manifest.json` | Generated pages, bindings and actions. |
| `dualscreen/5441AF3C89F97E1D.json` | Per-build declarative points. |
| `dualscreen/load-plans/<build>.json` | Build-gated helper patch (right stick, pouch, storage, equip, HUD hiding). |
| `native/helper/sshd_stick_helper.S` | Helper source. |
| `native/modules/01002DA013484000.cpp` | Readers, build gate, helper requests and image callbacks. |
| `native/modules/sshd_art.{h,cpp}` | Art decoders and compositors. |
| `native/modules/sshd_*_test.cpp` | Module regressions (art, maps, markers, story flags, pouch and equip, spins, swipes, L lock, D-pad, HUD hiding). |
| `tools/skyward-sword-hd/test_manifest.py` | Generated binding and page-layout regressions. |

## Rebuilding

Use the `VERSION` from `tools/skyward-sword-hd/gen_manifest.py` as the package version.

```sh
python3 tools/skyward-sword-hd/gen_manifest.py
# Requires your own dumped 1.0.1 main NSO, Python lz4, clang and LLVM tools:
python3 tools/skyward-sword-hd/build_helper_plan.py --nso /path/to/exefs/main
cmake -S native/modules -B build-sshd -G Ninja -DCMAKE_BUILD_TYPE=Release -DEDEN_SOURCE_ROOT=/path/to/eden-duo -DBUILD_TESTING=ON
cmake --build build-sshd --target dsmod-sshd
cmake --build build-sshd
ctest --test-dir build-sshd -R sshd --output-on-failure
python3 tools/skyward-sword-hd/test_manifest.py
strip --strip-all build-sshd/01002DA013484000.so
python3 tools/build_dualscreen_package.py --package packages/SkywardSwordHD --output dist \
  --title-id 01002DA013484000 --version <VERSION> --module linux-x86_64=build-sshd/01002DA013484000.so \
  --build-id 5441AF3C89F97E1D25A8CCF14C742BF82336D8A8000000000000000000000000 --min-runtime 23
```

For real-data art checks, set `SSHD_ROMFS` to colon-separated romfs roots and optionally
`SSHD_ART_OUT` to an existing PNG output directory, then run `dsmod-sshd-art-test [keys...]`.
No keys renders every registered map. Android companion modules use the NDK CMake toolchain with
`ANDROID_ABI=arm64-v8a` and `ANDROID_STL=c++_static`; pass the resulting module to the package tool
as an additional `--module android-arm64-v8a=<path>`.

## Credits

- Eden Duo companion by igawa6.
- The Legend of Zelda: Skyward Sword HD by Nintendo. A fan project, not affiliated with or endorsed
  by Nintendo or the Eden project.
- Game art is decoded at runtime from the player's own copy of the game; none is shipped.

Companion code: GPL-3.0-or-later.
