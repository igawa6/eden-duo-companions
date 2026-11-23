# v1.8.10 artwork provenance and upstream integration status

## What can be established from the available local files

The final archive includes 62 PNGs. Pixel assets are abstract color bands, background shapes, a stylized moon, titles, watermark lettering, masks, rings, slants and Persona detail panels. The package does not appear to bundle ripped character art or fonts; the game-native character art is obtained through runtime `module:p3p:` loaders. This is a **visual and file-inventory assessment, not a legal ownership certification**.

The original `p3p_thor_design_system_v1_8.py` generator exists as a development artifact. Its 31 matching procedural assets have now been recreated by `generate_art_v18_partial.py` and verified byte-for-byte against v1.8.10 with `verify_generated_31.py`. No font binary or game-extracted media has been added. A byte-level comparison of available archives shows:

- **26** final PNGs are byte-identical to the **v1.7.1** archive. Their earlier generation source has not been recovered.
- **31** final PNGs are byte-identical to **v1.8.0** and have a traceable intermediate design-system source file (not necessarily bit-for-bit reproducible across font versions).
- **5** final PNGs were revised after **v1.8.0** and do not have recovered exact-generation scripts.

`ASSET_PROVENANCE.csv` lists a SHA-256 and origin bucket for each of the 62.

## What may safely enter the upstream draft PR

- The **exact final manifest**, for design reference, as a compressed text fixture. The included `v1810_reference.py` verifies its SHA-256 and extracts its original bytes.
- Deterministic manifest tests comparing page geometry, action names, binding names, derived points, composites and animation settings.
- The graphics' procedural generation scripts **after** ownership, font licenses, and completeness are reviewed.

## What must be done before claiming a reproducible upstream release

1. Recover or reimplement source generation for the **remaining 31** PNGs and verify them. The other 31 already regenerate byte-for-byte on the tested local environment. A source script to draw the same *look* is not proof of byte-identical assets.
2. Review fonts, wordmarks and asset originality, license compatibility, and provenance. Do **not** introduce game-extracted resources, copyrighted game fonts, keys, ROMs, saves, firmware, or game data to upstream.
3. Demonstrate that the packaged Android/Linux modules correspond to buildable source and meet upstream GPL obligations.
4. Test the new from-source package on an AYN Thor and inspect actual screenshots: the earlier source-port approximation had obvious spacing and skill-icon regressions.
5. Keep PR #31 a draft until these steps pass. Maintainer consent to include original non-game assets would be one alternative; **consent is not yet obtained**.

This reference-first process avoids rewriting a superior, tested design from scratch.
