# AYN Thor UI source prototype (draft)

This is a **source-based, asset-free prototype** inspired by a community-tested
Persona 3 Portable dual-screen layout (local test version **v1.8.10**).

It is **not yet equivalent** to the user's tested `.dsmod.zip` release. That
local build uses original, procedural user-interface graphics. The source port
currently recreates the single-column **Main** navigation and **Battle** party
readability using the existing `module:p3p:` art and fill image, and applies
conservative text fitting to remaining pages.

The source port intentionally does not embed copyrighted game resources, bundle
the local binary archive, modify the native memory reader, or change any action
definitions or published game-state outputs.

## Build a manifest for visual review

From the repository root:

```sh
python3 tools/p3p/gen_manifest.py
python3 tools/p3p/thor_ui.py
python3 tools/p3p/test_thor_ui.py
```

The final command is an offline structural test, not an emulator test. Run the
usual native module build and package process documented in
[tools/p3p/README.md](README.md) after generating the manifest. **Do not
publish this prototype as a release** before full on-device validation.

## What is implemented so far

- Main: eight full-width aligned label/touch rows, clearer date/portrait/status
  layout, room between the last menu entry and HP/SP area.
- Battle: pale-gold party names, separated HP/SP value/bar columns, five enemy
  rows, and text-fitting safeguards.
- Other screens: a limited readability pass on dynamic names and details.
- Core: the 12 screens, all defined actions, and 2,798 reader outputs preserved.

## Remaining work before upstream merge

- Reproduce the local v1.8.10 layouts across Persona, Skill, Status, S.Link,
  Calendar, Equip, Item, Map and Talk using maintainable source generators.
- Review legal/provenance status of any procedural art before adding it. Do not
  commit game-extracted assets.
- Validate touch-to-label alignment on an AYN Thor.
- Confirm the exact Support symbol and skill-category mappings in Eden Duo.
  The native module currently emits 0 for some skill categories, including
  Support, so this prototype does not claim a verified icon fix.
- Perform a real Battle test with multiple party members and five enemies.
- Record the exact tested Eden Duo app version.
- Compare both protagonist color themes.

Game: Persona 3 Portable Switch **1.0.0**, title ID
`0100DCD01525A000`. Minimum runtime: **19**.

The local v1.8.10 test archive is **not** part of this PR because the upstream
project expects a reproducible, game-asset-free source contribution.
