# Persona 3 Portable AYN Thor — canonical v1.8.10 source reference

The accompanying compressed fixture is the **byte-exact** `dualscreen/manifest.json` from the user-tested **v1.8.10 FINAL** archive. It is now the authoritative layout/design target for this contribution. It is **not** the flawed source-port approximation.

The fixture is stored as LZMA + base85 text, **not** as an unreadable binary artifact: run `python3 tools/p3p/v1810/v1810_reference.py --output /tmp/p3p_v1810.json` to recover the ordinary, human-readable 799,718-byte JSON file. Its SHA-256 is `d9079a3852e24a24c07608ccfcb32b0b4c9c1283476650204e2a72cb30c9e379`. The script rejects corrupted data. The reference contains original page geometry, all animations, composites, derived points and font/image references.

## Why this is a reference, not a release

The original tested ZIP also includes **62 custom PNGs** and two compiled native modules, neither of which is committed to this source PR. The source fixture keeps its `file:img/...` references, so **it cannot be installed or shipped on its own**. The public v1.8.10 working archive is deliberately hosted on a [separate branch of this fork](https://github.com/speerit1/eden-duo-companions/tree/p3p-v1.8.10-release/community-releases/p3p-v1.8.10), with provenance notes, credits and archive checksums.

## How to verify the reference

```sh
python3 tools/p3p/gen_manifest.py
python3 tools/p3p/v1810/test_reference.py
python3 tools/p3p/v1810/v1810_reference.py --output /tmp/p3p_v1810.json
```

The old hand-recreated `--thor-ui` prototype was **removed from the active
source tree** after device testing exposed regressions. It is preserved in
the PR branch's Git history only. The normal P3P generator remains unchanged.
Future source work should derive from this exact reference and preserve native
game-state integration rather than guess at layout positions.

## Recovered exact procedural artwork

The source files [generate_art_v18_partial.py](generate_art_v18_partial.py)
and [verify_generated_31.py](verify_generated_31.py) now regenerate **31
of the 62** original v1.8.10 PNGs *byte-for-byte*, independently verified
against the original file hashes using Pillow and the locally installed Inter
Black Italic font. The font is not included in the repository.

```sh
python3 tools/p3p/v1810/generate_art_v18_partial.py --output /tmp/p3p-art
python3 tools/p3p/v1810/verify_generated_31.py /tmp/p3p-art
```

The [asset audit](audit_assets.py) validates the provenance records for all
62 PNGs and checks every static manifest image reference. It can also verify
the original, checksum-pinned v1.8.10 archive or compare any *subset* of
locally regenerated PNGs against the exact final release bytes. No artwork is
copied into this source branch.

```sh
python3 tools/p3p/v1810/audit_assets.py
python3 tools/p3p/v1810/audit_assets.py --archive /path/to/v1.8.10-FINAL.dsmod.zip
python3 tools/p3p/v1810/audit_assets.py --candidate-dir /tmp/p3p-art
# Only use this gate when the full source generator is ready:
python3 tools/p3p/v1810/audit_assets.py --candidate-dir /tmp/p3p-art --require-complete
```

## Recovered exact-pixel geometry (15 additional PNGs)

[generate_art_v1810_geometry.py](generate_art_v1810_geometry.py)
independently reconstructs the main/page/battle backgrounds and diagonal bands
for both routes (12 PNGs), two white slant masks and the circular moon mask.
The artwork was reverse-engineered from the audited v1.8.10 original, **not**
obtained by embedding existing PNG bytes.

All 15 reproduce the **exact decoded RGBA pixels** of the original release.
The output PNG byte streams may differ due to PNG encoder/compression versions;
pixel reproducibility is distinct from byte-identical file reproduction.

```sh
python3 -m pip install Pillow==12.3.0
python3 tools/p3p/v1810/generate_art_v1810_geometry.py --output /tmp/p3p-geometry
python3 tools/p3p/v1810/verify_generated_geometry.py
# Optional comparison against original audited ZIP (no game data in repo):
python3 tools/p3p/v1810/verify_generated_geometry.py --archive /path/to/v1.8.10-FINAL.dsmod.zip
```

## Final v1.8.10 alignment recoveries (5 additional PNGs)

The last five assets revised after v1.8.0 have now been traced to small
changes in the historical design generator: the Persona-detail gold underline
was moved +7 pixels; the MAIN title uses the same v1.8 title renderer; and
the MAIN route watermarks use x=47, bottom=970 and alpha=74.

[generate_art_v1810_persona.py](generate_art_v1810_persona.py) regenerates both
Persona-detail panels **byte-for-byte** without fonts. The corresponding
[verifier](verify_generated_persona.py) runs in CI. The
[main-screen generator](generate_art_v1810_main.py) recreates the three
remaining PNGs byte-for-byte using a locally installed Inter Black Italic OTF
(no font binary is distributed).

```sh
python3 tools/p3p/v1810/generate_art_v1810_persona.py --output /tmp/p3p-persona
python3 tools/p3p/v1810/verify_generated_persona.py /tmp/p3p-persona
python3 tools/p3p/v1810/generate_art_v1810_main.py --output /tmp/p3p-main
python3 tools/p3p/v1810/audit_assets.py --candidate-dir /tmp/p3p-main
```

## Historical footer lettering (2 additional PNGs)

[generate_art_v1810_footers.py](generate_art_v1810_footers.py) regenerates
the CALENDAR and S.LINK footer lettering from its original InterDisplay Bold
Italic typography, coordinates and translucent turquoise fill. Both images
were independently confirmed **byte-identical** to the final release using
Pillow and a locally installed font. Neither the font file nor rendered
original PNGs are committed.

```sh
python3 tools/p3p/v1810/generate_art_v1810_footers.py --output /tmp/p3p-footers
python3 tools/p3p/v1810/audit_assets.py --candidate-dir /tmp/p3p-footers
```

Combining the initial 31 exact PNGs, 15 pixel-identical shapes, five
byte-identical late revisions and two exact historical footer graphics gives
**53 of 62 reproducible original assets** as of that recovery step.
The later two recovered rings bring the source total to **55/62**, leaving
**seven** older v1.7.1 graphics need source reconstruction or
an explicitly approved original-asset policy.
Do not treat these partial generators as a complete release builder.

## Original circle and tick artwork (2 additional PNGs)

[generate_art_v1810_rings.py](generate_art_v1810_rings.py) recovers both
original circular decorations without copying their PNG bytes. The two
rings use 2x antialiased line/ellipse drawing and final-specific floating
point endpoint corrections. Both are identical to the **decoded RGBA pixels**
from the original v1.8.10 release, as proven by
[verify_generated_rings.py](verify_generated_rings.py) in CI.

```sh
python3 tools/p3p/v1810/generate_art_v1810_rings.py --output /tmp/p3p-rings
python3 tools/p3p/v1810/verify_generated_rings.py
# Optional comparison to the exact original local archive:
python3 tools/p3p/v1810/verify_generated_rings.py --archive /path/to/v1.8.10-FINAL.dsmod.zip
```

**55/62 graphics now reproduce from independent source:** 38 byte-identical
on the validated local environment and 17 pixel-identical RGBA graphics.
Only seven original visuals remain: two Start backgrounds (both routes)
and five large white wordmarks. This source PR still is not a self-contained
release builder.

## Native module source builds (CI verified)

The repository's P3P standalone C++ module now builds on **Linux x86-64**
and **Android ARM64** against the same pinned Eden Duo SDK revision
(`8c28fd5d`). The [Linux CI](https://github.com/speerit1/eden-duo-companions/actions/runs/45105484042)
also passes native unit tests; the [Android ARM64 CI](https://github.com/speerit1/eden-duo-companions/actions/runs/45105836302)
checks the ELF architecture and exported entrypoints.

These are **source compile and interface checks**, not proof of byte-identical
recreation of the compiled modules in the original v1.8.10 FINAL ZIP.
Linkage/source compliance, complete UI assets, and on-device QA remain
separate gates before this draft is merge-ready.

## Local source-module package QA (all 62 original visuals retained)

The [local package builder](rebuild_v1810_local.py) creates a **test-only**
v1.8.10 package from the user's own **byte-exact, SHA-256-pinned** v1.8.10
FINAL ZIP and newly compiled P3P Linux/Android modules. It copies every
original graphic, font notice, page, action and UI binding unchanged,
but replaces the two native `.so` files and adjusts the SHA-256 declarations
in both `package.json` and `dualscreen/manifest.json`.

The builder verifies all 62 original PNG hashes, canonical manifest
checksum, archive CRC, Linux x86-64 / Android AArch64 ELF types, module hash
declarations and output ZIP integrity. It **never modifies the input**.
Its [synthetic, no-original-art unit tests](test_rebuild_v1810_local.py)
run in GitHub Actions; a local regression test against the genuine original
ZIP also confirmed that only the two modules and their two JSON hash
declarations change when swapping binary inputs.

Use the [Linux module workflow](https://github.com/speerit1/eden-duo-companions/actions/workflows/p3p-source-native-linux.yml)
and [Android module workflow](https://github.com/speerit1/eden-duo-companions/actions/workflows/p3p-source-native-android.yml)
to obtain the two **source-built** module artifacts, or compile with the
documented `native/README.md` instructions. Each CI artifact is temporary
(14-day retention). After extracting the two `.so` files into different
local directories:

```sh
python3 tools/p3p/v1810/rebuild_v1810_local.py \
  --reference /path/to/0100DCD01525A000-Persona3PortableDS-v1.8.10-FINAL.dsmod.zip \
  --linux-module /path/to/linux-source/0100DCD01525A000.so \
  --android-module /path/to/android-source/0100DCD01525A000.so \
  --output /tmp/P3P-v1.8.10-SOURCE-MODULES-TEST-ONLY.dsmod.zip
```

**Important:** this is an optional *local test bridge* requiring a separately
obtained original archive, not a standalone source release or approval to
redistribute all its artwork. It does **not** resolve the seven incomplete
independent image generators, graphic ownership/licensing, source-to-original
binary correspondence, or on-device QA. Do not mark the PR ready or publish
the combined ZIP before those separate checks.

## Provenance

See `ASSET_PROVENANCE.csv` and `PROVENANCE_AND_UPSTREAM.md`. Comparison with the local build history identified **26** unchanged older assets (19 now reproducible: 17 pixel-identical and 2 byte-identical), **31** matching the procedural v1.8.0 design-system build, and **5** later revisions whose exact source has now been recovered. The traceable visual artwork appears abstract/procedural; completeness of the source and licensing review is **not yet established**.

Before upstream merge, either produce/verify generators for all assets, or obtain the maintainer's agreement to include genuinely original custom files. In both cases verify graphic ownership and font licenses, preserve GPL notices, and establish source-to-distributed-native-binary correspondence. Never add game dumps, proprietary game assets, ROMs, keys or copyrighted game fonts.

For the exact seven remaining graphic names, CI status and blockers, see
[UPSTREAM_HANDOFF.md](UPSTREAM_HANDOFF.md). The audit command now prints source
coverage and offers a strict `--require-full-source-art` completion gate.

## Maintainer question

For genuinely original, non-game graphical UI assets, would you prefer
pixel-reproducible Pillow/source generators only, or are independently
licensed original PNG assets permissible with clear provenance?
This draft remains open while source and native-module reproducibility work
is completed; please do not merge the fixture alone.
