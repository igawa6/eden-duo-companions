# Persona 3 Portable — AYN Thor v1.8.10 UI source port: maintainer review guide

**Review type:** early **source architecture / artwork policy review**.
**Merge status:** not ready to merge. **Repository branch:** `p3p-thor-ui-source-port`.
**Working reference:** the separately released, user-tested v1.8.10 FINAL.
**PR:** [upstream #31](https://github.com/igawa6/eden-duo-companions/pull/31).

## Requested maintainer decisions

1. **Artwork contribution format.** For seven custom, non-game-derived graphics whose
   original generation scripts have not been recovered, is a separate, explicit
   compatible license/permission for original PNGs (with attribution/provenance)
   acceptable, or must every graphical resource be generated procedurally?
   **No original PNG bytes are introduced by this draft PR.**
2. **Integration surface.** Is it acceptable to introduce this tested 12-page
   AYN Thor UI as an alternative layout to the current P3P package, with the
   existing native reader unchanged? Should it be a distinct opt-in package,
   preset, or the default UI after full verification?
3. **Provenance review.** Confirm preferred wording for game names, custom
   white wordmarks, original creator attribution, and AI-assisted art authorship.
   The contributor must establish rights in artwork independently of font
   licensing.
4. **Build acceptance.** Confirm whether separately validated Linux and
   Android ARM64 source builds plus test-package device smoke testing suffice
   for the initial integration review, and what additional full-game regression
   test matrix is expected before merge.

These questions do not imply prior maintainer approval.

## Precisely what is in this PR

- **Reference manifest:** canonical `dualscreen/manifest.json` from
  community-tested v1.8.10, stored as compressed, inspectable text
  (`canonical_manifest_v1.8.10.lzma.b85`). SHA-256
  `d9079a3852e24a24c07608ccfcb32b0b4c9c1283476650204e2a72cb30c9e379`.
  It defines 12 pages, 61 actions, and 2,798 native output declarations.
- **Art source:** 55 of 62 custom procedural PNGs reproducible from included
  Pillow scripts; 38 match original PNG **file hashes**, 17 match original
  decoded **RGBA pixel hashes**. The same official Inter v4.1 static fonts
  used for historical design are obtained from the publisher temporarily for
  CI only, authenticated by their exact SHA-256 hashes; fonts are not committed
  or published with packages. GitHub Actions regenerates and verifies **all 55**.
- **Source module CI:** the preexisting P3P C++ native module builds with the
  pinned Eden Duo SDK `8c28fd5d745d1ba5756a85dee00f5587ce0b35ad`;
  Linux x86-64 native tests and exported functions pass, and Android ARM64
  NDK r28 compile and exported-ELF checks pass.
- **Local QA bridge:** `rebuild_v1810_local.py` validates an externally supplied
  byte-exact v1.8.10 FINAL archive, substitutes independently built Linux and
  Android modules, and updates the hash declarations in the two JSON files.
  It never changes or uploads the original input. Synthetic tests of its
  integrity and rejection paths run in CI.
- **Initial device feedback:** the contributor tested the assembled source-built
  test ZIP on an AYN Thor and reported it **"seems good"**. This is encouraging
  smoke-test feedback, **not** a complete regression pass. Battle is not
  confirmed on-device.

## What is *not* in this PR

- No original artwork PNGs or Inter font binaries.
- No compiled `.so` modules, ROM, decrypted game data, copyrighted game art,
  firmware, game keys, or saves.
- No installable self-contained release; the reference manifest alone points
  to graphics and modules not in this branch.
- No change to native module logic or the upstream P3P generator's default
  game-state outputs.
- No claim of license ownership, final emulator compatibility for every
  action or on-device Battle validation.

## Seven original graphics still lacking exact source generators

| Original name | Issue |
| --- | --- |
| `bg_start_0.png` | Blue route Start background and moon texture |
| `bg_start_1.png` | Pink route Start background and moon texture |
| `word_battle.png` | Original white italic BATTLE wordmark |
| `word_map.png` | Original white italic MAP wordmark |
| `word_persona3.png` | Original white italic PERSONA 3 wordmark |
| `word_portable.png` | Original white italic PORTABLE wordmark |
| `word_talk.png` | Original white italic TALK wordmark |

These are included in the original working community archive, but **not**
introduced as original binary blobs to this PR. Source regeneration or
maintainer-approved permissions/provenance are still necessary for merge.

## Reproduction / inspection commands

From the fork branch with Python 3.11+, Pillow 12.3.0 and its installed
dependencies:

```sh
python tools/p3p/gen_manifest.py
python tools/p3p/v1810/test_reference.py
python tools/p3p/v1810/audit_assets.py
python tools/p3p/v1810/test_asset_audit.py
python tools/p3p/v1810/test_rebuild_v1810_local.py
python tools/p3p/v1810/verify_generated_geometry.py
python tools/p3p/v1810/verify_generated_rings.py
python tools/p3p/v1810/generate_art_v1810_persona.py --output /tmp/p3p-persona
python tools/p3p/v1810/verify_generated_persona.py /tmp/p3p-persona
```

For all historical **byte-identical** generators, retrieve pinned official
fonts *at test time* and run:

```sh
python tools/p3p/v1810/prepare_fonts_for_checks.py --output /tmp/p3p-inter
export P3P_INTER_FONT_DIR=/tmp/p3p-inter
python tools/p3p/v1810/generate_art_v18_partial.py --output /tmp/p3p-historical
python tools/p3p/v1810/verify_generated_31.py /tmp/p3p-historical
python tools/p3p/v1810/generate_art_v1810_main.py --output /tmp/p3p-main
python tools/p3p/v1810/audit_assets.py --candidate-dir /tmp/p3p-main
python tools/p3p/v1810/generate_art_v1810_footers.py --output /tmp/p3p-footers
python tools/p3p/v1810/audit_assets.py --candidate-dir /tmp/p3p-footers
```

The full checks are also executed automatically in
[`p3p-v1810-reference.yml`](../../../.github/workflows/p3p-v1810-reference.yml).
The separate `p3p-source-native-linux.yml` and
`p3p-source-native-android.yml` workflows build the native source and
retain their temporary CI artifacts for local QA. See
[`SOURCE_BUILT_LOCAL_QA.md`](SOURCE_BUILT_LOCAL_QA.md) for the actual
source-built test package digest and provenance.

## License boundaries

The upstream repository is GPL-3.0 licensed. The independently obtained
[Inter](https://github.com/rsms/inter) 4.1 fonts are under the
[SIL Open Font License 1.1](https://openfontlicense.org/); their binaries
are neither committed nor redistributed by this PR. That **does not**
establish permission to distribute separate original art or any
game-related wordmark. The source references contain no game-extracted PNGs
to our knowledge, but an explicit ownership, originality, attribution and
license review is still mandatory before including the original artwork.

The original P3P companion was authored by **jschroeder1**.
The Thor redesign was developed with significant assistance from
**Claude (Anthropic)** and refined and tested with **ChatGPT (OpenAI)**.
Maintain source and attribution notices consistently with upstream policy.

## Merge gates (intentionally open)

- [ ] Maintainer chooses acceptable licensed-art/source strategy.
- [ ] Provenance and rights for *all* graphics are established; seven
      sources recovered or authorized alternative accepted.
- [ ] Full-from-source **redistributable** UI package is assembled under the
      agreed licenses, with native source/provenance documented.
- [ ] Specific AYN Thor screens, tap/navigation and live game-state regression
      scenarios are tested, including Battle.
- [ ] Maintainer reviews/approves integration and documentation.

**Request:** review the design reference, verified generator strategy and
native build/QA approach now, and advise on the **artwork acceptance format**
and preferred integration path. **Do not merge this incomplete reference
into a shippable package yet.**
