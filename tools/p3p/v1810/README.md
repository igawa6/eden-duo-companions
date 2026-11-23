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

Combining the initial 31 exact PNGs, 15 pixel-identical shapes, and five
byte-identical late revisions gives **51 of 62 reproducible original assets**.
The **remaining 11** older v1.7.1 graphics still need source reconstruction
or an explicitly approved original-asset policy.
Do not treat these partial generators as a complete release builder.

## Provenance

See `ASSET_PROVENANCE.csv` and `PROVENANCE_AND_UPSTREAM.md`. Comparison with the local build history identified **26** unchanged older assets (15 now pixel-reproducible), **31** matching the procedural v1.8.0 design-system build, and **5** later revisions whose exact source has now been recovered. The traceable visual artwork appears abstract/procedural; completeness of the source and licensing review is **not yet established**.

Before upstream merge, either produce/verify generators for all assets, or obtain the maintainer's agreement to include genuinely original custom files. In both cases verify fonts' licenses, preserve GPL notices and build the native modules from corresponding source. Never add game dumps, proprietary game assets, ROMs, keys or copyrighted game fonts.

## Maintainer question

For genuinely original, non-game graphical UI assets, would you prefer
pixel-reproducible Pillow/source generators only, or are independently
licensed original PNG assets permissible with clear provenance?
This draft remains open while source and native-module reproducibility work
is completed; please do not merge the fixture alone.
