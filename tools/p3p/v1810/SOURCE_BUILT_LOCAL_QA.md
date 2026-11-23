# P3P v1.8.10 FINAL UI — source-built module package QA (local)

**Date:** 2026-11-22 (America/New_York)
**Status:** TEST ONLY; not a source-only distributable release. User reports an initial successful AYN Thor smoke test of this source-built test package; complete on-device QA remains open.

## Inputs and provenance

- Original community-tested UI ZIP:
  `0100DCD01525A000-Persona3PortableDS-v1.8.10-FINAL.dsmod.zip`
  - pinned SHA-256: `70faf618f1fd4777c78d5204688b5427e80eeb0e57c2d16ec30e9b02ed3e4513`
  - art and native assets obtained **only locally** from that exact input;
    no PNGs or precompiled modules added to the source PR.
- Linux x86-64 native module: compiled by
  [source-build workflow run 45107519752](https://github.com/speerit1/eden-duo-companions/actions/runs/45107519752).
  C++ tests and exported ELF symbols passed.
- Android ARM64 module: compiled with NDK r28 by
  [source-build workflow run 45107522134](https://github.com/speerit1/eden-duo-companions/actions/runs/45107522134).
  ARM64 ELF architecture and exported symbols passed.
- Both newly compiled modules had **debug sections removed**, preserving ELF
  architecture and the expected exported functions. The test ZIP does not
  embed or use the old precompiled modules.

## Exact test package

Locally created `Persona3PortableDS-v1.8.10-SOURCE-BUILT-THOR-TEST-ONLY.dsmod.zip`
via [`rebuild_v1810_local.py`](rebuild_v1810_local.py) from the
original audited ZIP and the two newly compiled modules.

| Item | SHA-256 |
|---|---|
| Entire local test ZIP | `e839ca742f5a5e549858ac80d959407528eccc881e5e3851b56e4d9c8a7f9b64` |
| Linux x86-64 module, debug sections removed | `66d44e51410b4ce3ce1cc76a18b04e25695570a0b40dc54ef2970afd34f18ae8` |
| Android ARM64 module, debug sections removed | `0c79ec53ae60269e04573d2a651c50ee684a2b57ed5f8a3404f343301bb4136b` |

- ZIP size: **1,725,234 bytes**.
- All **67 ZIP entries** present, CRC check passes.
- All **62 original PNG files are byte-for-byte unchanged**.
- All **12 pages**, **61 actions**, **2,798 native output declarations**, font
  settings, and game-state binding behavior in the manifest are unchanged.
- Comparing uncompressed contents against v1.8.10 FINAL, **only four ZIP
  entries differ**: `dualscreen/manifest.json`,
  `dualscreen/modules/linux-x86_64/0100DCD01525A000.so`,
  `dualscreen/modules/android-arm64-v8a/0100DCD01525A000.so`, and
  `package.json`.
- `package.json` and `dualscreen/manifest.json` declare exactly matching
  freshly computed module hashes.
- This test ZIP is **not committed or released** on the upstream source PR.

## Outstanding gates

- **Preliminary AYN Thor smoke test**: on 2026-11-22 (America/New_York), the
  user reported testing the source-built test package and that it **"seems good"**.
  This is a positive first-hand report, not a full regression-test certificate;
  exact screens, touch actions, duration and game-state cases were not specified.
  The Battle screen remains **not confirmed tested on hardware**. The previous
  FINAL release's other screens had earlier positive device feedback.
- **Independent artwork source**: 55/62 assets recovered; seven older
  backgrounds and wordmarks still require exact reconstruction or explicit
  maintainer-approved ownership/provenance/licensing for original custom art.
- **Binary provenance**: a compiled-from-source replacement is not necessarily
  byte-identical or functionally identical to the native binaries in the
  prior community release. Device tests and runtime behavior comparison matter.
- **Upstream PR permissions**: this GitHub integration may commit to the
  contributor's fork, but upstream PR edit/comment operations returned HTTP
  403. Maintainer artwork policy question must be posted by the contributor
  through GitHub UI or a suitably authorized credential.

**Do not** mark PR #31 ready or merge it based on a preliminary positive smoke
report. Keep the last known-good v1.8.10 FINAL ZIP as a rollback option until
broader device and game-state testing is complete.
