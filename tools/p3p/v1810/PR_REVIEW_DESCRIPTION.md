# Suggested PR #31 title and body (paste in the upstream PR editor)

**Suggested title:** Review request: P3P AYN Thor v1.8.10 UI source reference and reconstruction

---

## Review request: Persona 3 Portable AYN Thor v1.8.10 UI

This PR documents and begins a source port of the community-tested,
12-page Persona 3 Portable AYN Thor second-screen redesign. It is ready for
**source architecture, artwork policy and integration feedback**, but
**not yet for merge or redistribution as a complete mod**.

**Please start with the [maintainer review guide](https://github.com/speerit1/eden-duo-companions/blob/p3p-thor-ui-source-port/tools/p3p/v1810/MAINTAINER_REVIEW_GUIDE.md)**,
including the reproduction commands, CI evidence and open merge gates.

### What reviewers can verify today

- The full v1.8.10 FINAL manifest is preserved as a checksum-verified
  reference: 12 screens, 61 actions and 2,798 native outputs.
- **55/62 graphics regenerate from source**. GitHub CI reconstructs and
  checks all 55: 38 byte-identical PNG files and 17 pixel-identical RGBA
  graphics. Official Inter 4.1 font files are temporarily obtained with
  pinned SHA-256 checksums and are never committed/distributed.
- Linux x86-64 (with native unit tests) and Android ARM64 (NDK r28) C++
  builds pass using the pinned compatible Eden Duo SDK.
- A local, integrity-checked package assembled from source-built ELF
  modules retains all 62 original UI PNGs and manifest functionality.
  AYN Thor smoke-test feedback was positive ("seems good"), but this is
  not a complete gameplay regression test.
- No game-extracted images, ROMs, keys, saves, compiled modules, font
  binaries or original custom PNGs are committed to the source PR.

### Maintainer feedback requested

1. For seven custom art assets whose old generator scripts are missing
   (two Start backgrounds, and BATTLE, MAP, PERSONA 3, PORTABLE and TALK
   wordmarks), are original independently owned and explicitly licensed
   PNGs with clear provenance acceptable, or must every original graphic
   be generated from code?
2. Should this Thor design be offered as an opt-in layout, a separate
   P3P package, or the P3P default after full verification?
3. What source/binary attribution and device QA coverage must be completed
   for this contribution to be merge-ready?

### Explicitly not yet merge-ready

Seven artwork sources, rights/licensing decisions for all art/wordmarks,
a fully redistributable source-only package and comprehensive on-device
gameplay and Battle testing remain open. **Please do not merge the reference
alone**. The currently working v1.8.10 FINAL is distributed separately.

Credits: original P3P companion creator **jschroeder1**. Redesign built
with extensive assistance from **Claude (Anthropic)** and refined and
source-audited with **ChatGPT (OpenAI)**.

The [working unofficial v1.8.10 FINAL release](https://github.com/speerit1/eden-duo-companions/releases/tag/p3p-thor-v1.8.10-community)
is separate from this PR.
