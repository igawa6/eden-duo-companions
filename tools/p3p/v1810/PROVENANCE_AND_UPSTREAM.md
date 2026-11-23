# v1.8.10 artwork provenance and upstream integration status

## What can be established from the available local files

The final archive includes 62 PNGs. Pixel assets are abstract color bands, background shapes, a stylized moon, titles, watermark lettering, masks, rings, slants and Persona detail panels. The package does not appear to bundle ripped character art or fonts; the game-native character art is obtained through runtime `module:p3p:` loaders. This is a **visual and file-inventory assessment, not a legal ownership certification**.

The source port now includes a separate procedural reconstruction of **17**
previously unrecovered v1.7.1 graphics: 12 route-specific backgrounds/accent
bands, two slants, one moon mask, and two original rings. Every generated graphic matches the
decoded original v1.8.10 RGBA pixels; compressed PNG bytes may differ with
Pillow/zlib versions. The new verifier pins decoded-pixel SHA-256 digests to
the original archive. No source PNG data is embedded or checked in.

The five later-revised UI assets are now recovered: two Persona detail
surfaces (corrected gold underline) and three MAIN title/watermark assets
(corrected positioning and opacity). All five are byte-identical to final PNGs.
Two additional footer-lettering images regenerate byte-for-byte from locally
installed InterDisplay Bold Italic OTF. That font is not included. Source
reproducibility is **53/62**, with seven early graphics remaining.

The original `p3p_thor_design_system_v1_8.py` generator exists as a development artifact. Its 31 matching procedural assets have now been recreated by `generate_art_v18_partial.py` and verified byte-for-byte against v1.8.10 with `verify_generated_31.py`. No font binary or game-extracted media has been added. A byte-level comparison of available archives shows:

- **26** final PNGs are byte-identical to the **v1.7.1** archive. Fifteen geometry assets are now independently pixel-reproducible, and two footer-lettering assets are byte-reproducible. Seven older graphics remain.
- **31** final PNGs are byte-identical to **v1.8.0** and have a traceable intermediate design-system source file (not necessarily bit-for-bit reproducible across font versions).
- **5** final PNGs were revised after **v1.8.0**, and all five are now byte-reproducible using source scripts recovered from the historical design system.

`ASSET_PROVENANCE.csv` lists a SHA-256 and origin bucket for each of the 62.

## What may safely enter the upstream draft PR

- The **exact final manifest**, for design reference, as a compressed text fixture. The included `v1810_reference.py` verifies its SHA-256 and extracts its original bytes.
- Deterministic manifest tests comparing page geometry, action names, binding names, derived points, composites and animation settings.
- The graphics' procedural generation scripts **after** ownership, font licenses, and completeness are reviewed.

## What must be done before claiming a reproducible upstream release

1. Recover or reimplement source generation for the **remaining 7** PNGs and verify them. The other 55 have independently verified source reconstruction: 31 original byte-identical PNGs, 17 decoded-pixel-identical geometric/ring assets, 5 byte-identical late revisions and 2 byte-identical footer-lettering assets. A source script that produces the same *look* is not proof of byte- or pixel-identical assets.
2. Review fonts, wordmarks and asset originality, license compatibility, and provenance. Do **not** introduce game-extracted resources, copyrighted game fonts, keys, ROMs, saves, firmware, or game data to upstream.
3. Linux x86-64 and Android ARM64 now **compile successfully from C++ source**
   against the pinned Eden Duo SDK, and their ELF exports are validated in
   GitHub Actions. The Linux native unit tests pass. **Still outstanding:**
   establish binary/source correspondence with the originally distributed
   modules, document toolchain/reproducibility details, and review upstream GPL
   redistribution obligations. CI source compilation is not proof that the
   binaries are byte-identical to the original release.
4. Test the new from-source package on an AYN Thor and inspect actual screenshots: the earlier source-port approximation had obvious spacing and skill-icon regressions.
5. Keep PR #31 a draft until these steps pass. Maintainer consent to include original non-game assets would be one alternative; **consent is not yet obtained**.

This reference-first process avoids rewriting a superior, tested design from scratch.
