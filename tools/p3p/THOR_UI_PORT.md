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

The final command is an offline structural test, not an emulator test.
The updated structural tests still need to be executed in a full repository
checkout; source changes in this draft have not been validated on-device.
Run the usual native module build and package process documented in
[tools/p3p/README.md](README.md) after generating the manifest. **Do not
publish this prototype as a release** before full on-device validation.

## What is implemented so far

- Main: eight full-width aligned label/touch rows, clearer date/portrait/status
  layout, and space between navigation and HP/SP.
- Battle: pale-gold party names, separated HP/SP value/bar columns, five enemy
  rows, and text-fitting safeguards.
- Main and Battle: original pause overlays retained, including the input-blocking
  layer; the Main error-state message is also retained.
- Persona: native fill-backed detail panel, 12 wider selection/touch rows,
  header and arcana alignment, smaller native art inset, five stat gauges,
  nine affinity icons, an eight-skill two-column area, and bottom progression
  text lanes. The existing Persona action is labelled "EQUIP" and placed next
  to the header, with its original live action/bind intact.
- Skill: party-tab hitboxes widened, live skill icon and row text moved to match
  the community-tested v1.8.10 proportions. The game's native icon lookup
  remains unchanged.
- S.Link, Item, Equip, Status, Calendar and Talk: targeted alignment and
  dynamic-text fitting rules, preserving generated controls and data bindings.
- Core: all 12 screens, existing action definitions and 2,798 reader outputs
  preserved. Added offline checks for pause behavior, actions, 42 calendar cells,
  and the original skill-icon binding.

## Remaining work before upstream merge

- Reproduce the *full* v1.8.10 visual design across Persona, Skill, Status,
  S.Link, Calendar, Equip, Item, Map, Talk and the start screen using
  maintainable, game-asset-free source generators. The changes above are
  partial layout and typography passes, not complete screen recreations.
- Review legal/provenance status of any procedural art before adding it. Do not
  commit game-extracted assets.
- Validate touch-to-label alignment, Persona action placement, and long
  skill descriptions on an AYN Thor.
- Confirm the exact Support symbol and skill-category mappings in Eden Duo.
  The native module currently emits 0 for some skill categories, including
  Support, so this prototype does not claim a verified icon fix.
- Perform a real Battle test with multiple party members and five enemies.
- Record the exact tested Eden Duo app version.
- Compare both protagonist color themes on-device. The Persona detail
  background, section headings and divider lines now use route-specific
  blue or pink variants, but their appearance needs renderer validation.
- Execute the updated structural test against a clean source-generated
  manifest and run end-to-end build checks before requesting merge.

Game: Persona 3 Portable Switch **1.0.0**, title ID
`0100DCD01525A000`. Minimum runtime: **19**.

The local v1.8.10 test archive is **not** part of this PR because the upstream
project expects a reproducible, game-asset-free source contribution.
