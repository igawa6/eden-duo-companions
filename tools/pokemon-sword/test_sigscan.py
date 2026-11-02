#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the variant config (sw_variants.py) and its pattern resolvers (sw_sigscan.py).

    python3 tools/pokemon-sword/test_sigscan.py           synthetic images + config consistency
    python3 tools/pokemon-sword/test_sigscan.py --main    the real 1.3.2 main (SWORD_NSO); 77 = skip

ctest: dsmod-pokemon-sword-sigscan / dsmod-pokemon-sword-sigscan-main.
"""
from __future__ import annotations

import array
import os
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sw_sigscan as S  # noqa: E402
import sw_variants as V  # noqa: E402
from sw_variants import Site  # noqa: E402

SKIP = 77


# --- AArch64 encoders (just what the tests need) --------------------------------------------------

def adrp(rd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | rd


def add_imm(rd, rn, imm):
    return 0x91000000 | (imm << 10) | (rn << 5) | rd


def ldr_x(rt, rn, imm):
    return 0xF9400000 | ((imm // 8) << 10) | (rn << 5) | rt


def bl(pc, target):
    return 0x94000000 | (((target - pc) // 4) & 0x03FFFFFF)


RET = 0xD65F03C0
NOP = 0xD503201F
STP = 0xA9BF7BFD   # stp x29, x30, [sp, #-0x10]!
MOV = 0xAA0003F3   # mov x19, x0


def image(words, text_off=0, data_off=0x10000, data=b"", relatives=None):
    """A synthetic Image: .text = words at text_off, one data segment, RELATIVE relocations."""
    text = struct.pack(f"<{len(words)}I", *words)
    seg = [(text_off, len(text), text), (text_off + len(text), 0, b""), (data_off, len(data), data)]
    w = array.array("I")
    w.frombytes(text)
    slot_addends = dict(relatives or {})
    rel = {}
    for slot, addend in slot_addends.items():
        rel.setdefault(addend, []).append(slot)
    return S.Image(seg, 0, "00" * 32, w, text_off, rel, slot_addends)


def sig_at(img, off, n):
    i = (off - img.text_off) // 4
    return S.format_sig(S.mask_words(img.words[i:i + n]))


# --- synthetic tests -----------------------------------------------------------------------------

def test_mask_and_format():
    pc = 0x1000
    words = [adrp(8, pc, 0x20000), add_imm(8, 8, 0x123), bl(pc + 8, 0x4000), STP, ldr_x(9, 8, 0x10)]
    masked = S.mask_words(words)
    v, m = masked[0]
    # nibble granularity: Rd's low nibble survives, the nibble shared with immhi does not
    assert (m & 0xF) == 0xF and (m & (0x7FFFF << 5)) == 0, "ADRP keeps its register, masks the page"
    assert masked[1][1] & (0xFFF << 10) == 0, "ADD off an ADRP register masks its low 12 bits"
    assert masked[2][1] == 0xF0000000, "BL masks its target (imm26, widened to whole nibbles)"
    assert masked[3] == (STP, 0xFFFFFFFF), "an ordinary instruction is kept whole"
    assert masked[4][1] & (0xFFF << 10) == 0, "LDR off an ADRP register masks its offset"
    text = S.format_sig(masked)
    assert S.parse_sig(text) == masked, "format / parse round trip"
    assert S.format_sig(S.mask_words([STP])) == "fd7bbfa9", "little-endian bytes, like `expected`"


def test_code_and_uniqueness():
    f = [STP, MOV, 0xF9400260, 0x91000400, RET]          # a function
    words = [NOP] * 4 + f + [NOP] * 3 + [STP, MOV, RET] + [NOP] * 2
    img = image(words)
    off = 4 * 4
    s = Site("f", off, "code", sig=sig_at(img, off, 3))
    assert S.resolve(img, s) == off
    # the 2-word prefix also matches the second function: ambiguous -> never a guess
    assert S.resolve(img, Site("f", off, "code", sig=sig_at(img, off, 2))) is None
    # at: the instruction of interest inside the match
    s = Site("f+8", off + 8, "code", sig=sig_at(img, off, 3), at=8)
    assert S.resolve(img, s) == off + 8
    d = S.derive(img, Site("f", off, "code"), min_words=1)
    assert d is not None and d[1] == 0 and S.resolve(img, Site("f", off, "code", sig=d[0])) == off
    assert S.resolve(img, Site("none", 0, "code", sig="11223344")) is None


def test_xref_got_branch():
    data_off = 0x40000
    target = data_off + 0x238       # a data object
    got = data_off + 0x800          # a GOT slot holding a .bss pointer
    bss_obj = 0x80010
    words = [NOP] * 8
    pc = 4 * len(words)
    words += [adrp(8, pc, target), add_imm(8, 8, target & 0xFFF), STP, RET]
    pc2 = 4 * len(words)
    words += [adrp(9, pc2, got), MOV, ldr_x(9, 9, got & 0xFFF), 0xF9400129, RET]
    callee = 4 * len(words)
    words += [STP, RET]
    pc3 = 4 * len(words)
    words += [bl(pc3, callee), 0x2A0003E1, RET]
    img = image(words, data_off=data_off, data=bytes(0x1000), relatives={got: bss_obj})
    assert S.decode_xref(img, pc) == target
    assert S.resolve(img, Site("x", target, "xref", sig=sig_at(img, pc, 3))) == target
    assert S.decode_xref(img, pc2) == got, "ADRP + LDR (scaled) on the same register, not adjacent"
    assert S.resolve(img, Site("g", bss_obj, "got", sig=sig_at(img, pc2, 4))) == bss_obj
    assert S.resolve(img, Site("g16", bss_obj + 16, "got", sig=sig_at(img, pc2, 4), bias=16)) == bss_obj + 16
    assert S.decode_branch(img, pc3) == callee
    assert S.resolve(img, Site("b", callee, "branch", sig=sig_at(img, pc3, 3))) == callee
    # an ADRP whose completion is not on its register is not decoded
    img2 = image([adrp(8, 0, target), add_imm(9, 10, 1), RET, RET, RET, RET, RET])
    assert S.decode_xref(img2, 0) is None


def test_vtable():
    vt = 0x40100
    f0, f1, f2 = 0x20, 0x40, 0x60
    words = [NOP] * 8 + [STP, MOV, 0xF9400000, RET] + [NOP] * 4 + [STP, 0xAA0103F4, RET, NOP] + [NOP] * 4 + [STP, RET]
    rel = {vt: f0, vt + 8: f1, vt + 16: f2, 0x40400: f2}   # f2 is also in a second table
    img = image(words, data_off=0x40000, data=bytes(0x1000), relatives=rel)
    s = Site("vt", vt, "vtable", sig=sig_at(img, f0, 3), fn=f0)
    assert S.resolve(img, s) == vt
    s = Site("vt", vt, "vtable", sig=sig_at(img, f1, 3), fn=f1, slot=1)
    assert S.resolve(img, s) == vt
    s = Site("getter", f2, "vtable", sig=sig_at(img, f1, 3), fn=f1, entry=1)
    assert S.resolve(img, s) == f2, "entry: the sibling slot's function"
    s = Site("vt", vt, "vtable", sig=sig_at(img, f2, 2), fn=f2, slot=2)
    assert S.resolve(img, s) is None, "a function in two tables names no single vtable"


def test_resolve_all():
    words = [NOP] * 2 + [STP, MOV, 0xF9400260, RET] + [NOP] * 2
    img = image(words)
    good = Site("good", 8, "code", sig=sig_at(img, 8, 3))
    unsigned = Site("unsigned", 0x1234, "code")
    assert S.resolve_all(img, [good, unsigned], known_build=True) == {"good": 8, "unsigned": 0x1234}
    for sites, known in (([good, unsigned], False), ([Site("moved", 12, "code", sig=good.sig)], True)):
        try:
            S.resolve_all(img, sites, known_build=known)
        except SystemExit:
            continue
        raise AssertionError("an unknown build may not fall back; a disagreeing scan is an error")


def test_variant_config():
    V.validate()
    sword = V.get("sword")
    assert V.DEFAULT == "sword" and list(V.VARIANTS) == ["sword", "shield"], "Sword (default) and its twin Shield"
    assert sword.title_hex == "0100ABF008968000"
    assert sword.build.build16 == "A3B75BCD3311385A" and len(sword.build.build_id) == 64
    assert sword.encounter_tables == ("encount_k.bin", "encount_symbol_k.bin")
    assert sword.title_arc == "title_demo_k_00_eng" and sword.title_logo == "grptext_titledemo_logo_k_00^q"
    assert sword.package_dir == "PokemonSword" and sword.encounter_tables == ("encount_k.bin", "encount_symbol_k.bin")
    shield = V.get("shield")
    assert shield.title_hex == "01008DB008C2C000" and V.SHIELD_TITLE_ID == shield.title_id
    assert shield.build.build16 == "A16802625E7826BF" and len(shield.build.build_id) == 64
    assert shield.encounter_tables == ("encount_t.bin", "encount_symbol_t.bin")
    assert shield.title_arc == "title_demo_t_00_eng" and shield.title_logo == "grptext_titledemo_logo_t_00^q"
    assert shield.package_dir == "PokemonShield" and shield.encounter_tables == ("encount_t.bin", "encount_symbol_t.bin")
    assert shield.dex_comment_file == "zukan_comment_B.dat" and shield.save_game_version == 45
    # the League Card check (sw_trainer_portrait.h) derives TrainerCard8's version byte (0 / 1) from this
    assert (sword.save_game_version - 44, shield.save_game_version - 44) == (0, 1)
    assert shield.aoc_title_ids == (0x01008DB008C2D001, 0x01008DB008C2D002) and shield.other_builds == ()
    for v in (sword, shield):
        # every site has a signature, and every signature parses
        for s in v.build.sites:
            assert s.sig and S.parse_sig(s.sig), (v.key, s.name)
            assert s.kind in ("code", "xref", "got", "branch", "vtable"), (v.key, s.name)
        # the fixed expectations cover every helper patch site
        hooks = {v.build.site(site).offset for site, _entry in V.HELPER_HOOKS}
        assert hooks <= set(v.build.expect), v.key
    # the twins name the same sites in the same order with the same signatures (one scan finds both)
    assert [(s.name, s.kind, s.sig) for s in shield.build.sites] == [(s.name, s.kind, s.sig) for s in sword.build.sites]
    # the generated module record is current
    subprocess.run([sys.executable, str(HERE / "gen_variant_header.py"), "--check"], check=True,
                   stdout=subprocess.DEVNULL)


def run_synthetic():
    for t in (test_mask_and_format, test_code_and_uniqueness, test_xref_got_branch, test_vtable,
              test_resolve_all, test_variant_config):
        t()
    print("sw_sigscan: synthetic resolvers + variant config ok")
    return 0


def run_main(key="sword"):
    """The variant's real main (Sword: SWORD_NSO, Shield: SHIELD_NSO)."""
    env = f"{key.upper()}_NSO"
    nso = os.environ.get(env)
    if not nso or not Path(nso).is_file():
        print(f"{env} not set: skipping the real-main signature check")
        return SKIP
    v = V.get(key)
    img = S.load(nso)
    assert img.build_id == v.build.build_id, f"{nso} is not {v.name} {v.build.version}"
    assert img.end <= v.build.image_span, "image_span bounds the whole image (incl. .bss)"
    bad = [s.name for s in v.build.sites if S.resolve(img, s) != s.offset]
    assert not bad, f"signatures not resolving to their verified offset: {bad}"
    # as if this were an unknown build: every site scans, no fallback is needed
    got = S.resolve_all(img, v.build.sites, known_build=False)
    assert got == {s.name: s.offset for s in v.build.sites}
    for off, expect in v.build.expect.items():
        assert img.code_at(off).hex() == expect, f"main+{off:#x}"
    print(f"sw_sigscan: {len(v.build.sites)}/{len(v.build.sites)} signatures find their verified "
          f"offset on {v.name} {v.build.version} ({img.build_id[:16]})")
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    key = args[args.index("--variant") + 1] if "--variant" in args else "sword"
    sys.exit(run_main(key) if "--main" in args else run_synthetic())
