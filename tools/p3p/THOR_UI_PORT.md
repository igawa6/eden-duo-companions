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
# Standard P3P manifest (unchanged):
python3 tools/p3p/gen_manifest.py
# Explicitly opt into this unfinished Thor source-preview layout:
python3 tools/p3p/gen_manifest.py --thor-ui
# Offline regression tests (run against the standard manifest):
python3 tools/p3p/gen_manifest.py
python3 tools/p3p/test_thor_ui.py
```

The `--thor-ui` option is **opt-in**: running `gen_manifest.py`
without it still generates the original P3P companion. The separate
`thor_ui.py` pass is idempotent when run twice on the same generated preview;
it rejects unknown game title IDs or unexpected source manifest names instead
of silently changing them. To build an optional preview package, rerun the
generator **with** `--thor-ui` after completing the offline tests.

The final command is an offline structural test, not an emulator test.
The draft-only GitHub Actions workflow at
`.github/workflows/p3p-thor-source-port.yml` generated the P3P manifest,
validated Python syntax and JSON output, and **passed the Thor UI structural
tests** on the fork's branch on 2026-11-22 (see
[successful run](https://github.com/speerit1/eden-duo-companions/actions/runs/44989684822)).
This validates both standard and opt-in manifest generation, stability
of the repeated UI pass, and structural invariants, **not** native
module compilation, rendering on Eden Duo, or hardware interaction.
Run the usual native module build and package process documented in
[tools/p3p/README.md](README.md) after generating the manifest. **Do not
publish this prototype as a release** before full on-device validation.

## What is implemented so far

- Start: asset-free route-aware title treatment with native Persona art,
  larger lettering, legible PORTABLE text and a left-aligned start cue (the
  first test build clipped that prompt). The local release's original
  procedural rings and rasterized wordmarks are deliberately not included.
- Main: eight labels use the tested 68-pixel pitch and matching bullet
  positions (the earlier source port put bullets a row too high). Live moon
  phase and four gated party-member cards are preserved. Numeric HP/SP text
  is right-anchored inside each card instead of clipping off the left edge.
- Battle: pale-gold party names, route-colored HP/SP values,
  five enemy rows with route-aware names, help and affinities, live ailments,
  and text-fitting safeguards. Existing enemy HP bars and native outputs
  remain unchanged.
- Main and Battle: original pause overlays retained, including the input-blocking
  layer; the Main error-state message is also retained.
- Persona: native fill-backed detail panel, 12 wider selection/touch rows,
  header and arcana alignment, smaller native art inset, five stat gauges,
  nine affinity icons, an eight-skill two-column area, and bottom progression
  text lanes. The existing Persona action is labelled "EQUIP" and placed next
  to the header, with its original live action/bind intact.
- Skill: party-tab hitboxes widened, row text moved to match v1.8.10.
  Restored the **sprite 135 native fallback** under the live skill icon
  because the game reader can publish a zero category for Support skills
  including Rakukaja. The overlay retains the original icon binding.
  This fixes the source-manifest omission found on a 2026-11-22 Thor test;
  rendering still needs rechecking on the device.
- S.Link: route-colored card edges and dividers between rank, guidance
  and social details; the multiline people field can use up to four lines.
- Equipment: larger party-tab hitboxes, restored readable item-help text,
  route-colored edges on all four equipped-item cards, and clearer picker counts.
- Items: route-colored item-row edge and fitted party target names within their
  original use-item touch buttons; item-category tabs, select and cancel
  actions are preserved.
- Status: member hitboxes match the 236x72 Thor party-tabs; route accents
  on the main stat panel and three social-attribute cards; longer names fit.
- Calendar: all 42 live day cells use the finished v1.8.10 six-week grid
  (100-pixel row pitch, 91-pixel panels). Today, holidays, full-moon sprites
  and Prev/Next actions keep their existing live bindings.
- Map: route-colored frame behind the original live map. Floor labels get a
  wider title lane; map tiles, animated position indicators and all coordinate
  bindings are unchanged.
- Talk: each of five dialogue choices gets a route-colored edge and two-line
  text fitting, while preserving every original choice/selection action.
- Status, Calendar, Map and Talk still need hardware visual validation.
- Core: all 12 screens, existing action definitions and 2,798 reader outputs
  preserved. Added offline checks for pause behavior, actions, 42 calendar cells,
  and the original skill-icon binding.

## Remaining work before upstream merge

- Reproduce the *full* v1.8.10 visual design and animation across
  all twelve screens using maintainable, game-asset-free source generators.
  Every screen now has a basic source-port treatment, but these are partial
  layout and typography passes, not visually equivalent recreations.
- Review legal/provenance status of any procedural art before adding it. Do not
  commit game-extracted assets.
- Validate touch-to-label alignment, Persona action placement, long
  skill descriptions, equipment selection, item-use target labels,
  multi-line S.Link and dialogue text, the full six-week Calendar,
  and live Tartarus map overlays on an AYN Thor.
- Confirm the exact Support symbol and skill-category mappings in Eden Duo.
  The native module currently emits 0 for some skill categories, including
  Support, so this prototype does not claim a verified icon fix.
- Perform a real Battle test with multiple party members and five enemies.
- Record the exact tested Eden Duo app version.
- Compare both protagonist color themes on-device. The Persona detail
  background, section headings and divider lines now use route-specific
  blue or pink variants, but their appearance needs renderer validation.
- Run the native CMake/ctest module build and end-to-end packaging checks
  before requesting merge; GitHub Actions currently covers only source generation
  and manifest/UI structural checks.

Game: Persona 3 Portable Switch **1.0.0**, title ID
`0100DCD01525A000`. Minimum runtime: **19**.

The local v1.8.10 test archive is **not** part of this PR because the upstream
project expects a reproducible, game-asset-free source contribution.
