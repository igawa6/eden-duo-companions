# Persona 3 Portable AYN Thor v1.8.10 — upstream handoff / blockers

> **Status: draft, not merge-ready.** The separately released, originally
> tested v1.8.10 FINAL mod is the **authoritative functional/artwork reference**.
> No tested archive, game media, compiled `.so` files, or copyrighted fonts
> are included in the source PR.

## What is backed by the source branch

- The byte-exact 12-page `dualscreen/manifest.json` reference is embedded as
  a compressed text fixture and SHA-256 verified.
- The original game-state output and action schemas are compared against the
  upstream standard P3P generator, without changing native reader behavior.
- Artwork inventory and checksums cover all 62 custom PNGs.
- 53/62 graphic generators have been recovered: **38 byte-identical** on the
  validated local environment, and **15 identical after RGBA decoding**.
  The latter may have different PNG compressed bytes across Pillow versions.
- CI validates the reference schema, all art provenance records, and the
  15 geometric and 2 Persona-detail graphic reconstructions.
- The working community v1.8.10 ZIP remains available separately; **the PR
  source alone is not an installable mod**.

## The nine remaining art sources

| Asset | Scope | Resolution path |
|---|---|---|
| `bg_start_0.png` | Blue-route opening background and textured moon | Recover procedural drawing / independently license original |
| `bg_start_1.png` | Pink-route opening background and textured moon | Recover procedural drawing / independently license original |
| `ring_a.png` | Fine circular tick ring | Recover accurate antialiased source |
| `ring_b.png` | Coarse circular tick ring | Recover accurate antialiased source |
| `word_battle.png` | Original BATTLE wordmark | Recover original font and render transform |
| `word_map.png` | Original MAP wordmark | Recover original font and render transform |
| `word_persona3.png` | Original PERSONA 3 wordmark | Recover original font and render transform |
| `word_portable.png` | Original PORTABLE wordmark | Recover original font and render transform |
| `word_talk.png` | Original TALK wordmark | Recover original font and render transform |

**Do not replace missing graphics with approximate designs and call them
pixel-perfect.** The original release can be used locally for verification,
but its PNGs should not be added to this PR without a provenance and
compatible-license decision.

## Typeface and rendering provenance

The local validation environment uses **Inter version 4.001**
(`git-9221beed3`). The font binaries remain outside the repository.

| Font used | Local OTF SHA-256 |
|---|---|
| Inter Black Italic | `54302c38cf020111ea06deaa137ba0c17b703d0f8acf0a5cd37f054c09f11c51` |
| Inter Display Bold Italic | `095f2bd9b95275826bd361f80ce40b3619b4cb586c7bc4f64d5210cedb29fd89` |

The [Inter typeface](https://github.com/rsms/inter) is under the
[SIL Open Font License 1.1](https://opensource.org/license/OFL-1.1).
The font license does not automatically attach to rasterized graphics
*created using* that font. It also does **not** establish ownership of
the accompanying original artwork, justify embedding game graphics,
or establish that the output images may be sublicensed by this
contributor. Track the drawing code, rights and attributions separately.

Pixel/byte matches can depend on the actual OTF binary,
Pillow version, and PNG encoder. The source branch intentionally never
redistributes OTF files.

## Remaining release/merge gates

- [ ] **Artwork policy**: maintainer guidance on accepting separately licensed,
      genuinely original PNG files vs requiring procedural source for all 62.
- [ ] **Art provenance**: review ownership/permissions, attribution, wordmarks,
      Inter font license and any source-supply obligations.
- [ ] **Artwork completeness**: all 62 PNG sources verified against final
      assets, or maintainers explicitly accept equivalent licensed originals.
- [ ] **Source-built native modules**: build Android ARM64 and Linux x86-64
      modules from compatible Eden Duo SDK / C++ source, document required
      toolchain and linkable source, and validate functionality. Shipping
      module SHA-256s alone does *not* establish source reproducibility.
- [ ] **Source-built package QA**: build an installable artifact and compare
      original 12-page layout/navigation and images; test real AYN Thor screens.
      Battle is preview-reviewed but is **not hardware-verified**.
- [ ] **Upstream review**: after these pass, change this draft PR to ready.

## Suggested maintainer question

> We have reconstructed 53/62 original, non-game UI PNGs from source (38
> byte-identical on our validated environment, 15 pixel-identical). For the
> remaining nine, would you accept independently licensed custom UI PNGs with
> explicit ownership/provenance and credits, or do you require generators for
> all of them? The source PR intentionally excludes game assets and the
> independently distributed binary release. We are keeping the PR as a draft
> until the art policy, native source builds and device tests are resolved.

The source inventory is machine-auditable:

```sh
python3 tools/p3p/v1810/audit_assets.py
# Release readiness gate: deliberately fails while 9 PNGs remain unsourced
python3 tools/p3p/v1810/audit_assets.py --require-full-source-art
```

This status does not assert legal ownership or licensing compliance.
