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

The existing opt-in `--thor-ui` generator is an **early, incomplete prototype**, not the reference source of truth. Do **not** release that experimental version in place of v1.8.10. Subsequent changes should derive from this exact reference and preserve native game state integration rather than re-create layout positions from guesswork.

## Provenance

See `ASSET_PROVENANCE.csv` and `PROVENANCE_AND_UPSTREAM.md`. Comparison with the local build history identified **26** unchanged older assets, **31** matching the procedural v1.8.0 design-system build, and **5** edited after v1.8.0 without a recovered exact generator. The traceable visual artwork appears abstract/procedural; completeness of the source and licensing review is **not yet established**.

Before upstream merge, either produce/verify generators for all assets, or obtain the maintainer's agreement to include genuinely original custom files. In both cases verify fonts' licenses, preserve GPL notices and build the native modules from corresponding source. Never add game dumps, proprietary game assets, ROMs, keys or copyrighted game fonts.
