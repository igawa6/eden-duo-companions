#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Byte-pattern resolvers for main-relative offsets (offline, against the player's exefs/main).

Every main+ offset the module and the guest helper use is listed once in sw_variants.py as a Site:
its fixed, verified offset for a known build plus a signature that finds the same place again in
another build (a game update, or the Sword/Shield twin) without guessing:

  code    the signature matches at the instruction itself (`at` = its byte offset in the match).
  xref    the instruction at `at` is an ADRP; the first later ADD/LDR (unsigned offset) on the same
          register within 5 instructions completes the address (a GOT slot, a .data/.bss/.rodata
          object). The target is that address.
  got     as xref, but the address is a GOT slot: the target is that slot's R_AARCH64_RELATIVE
          addend (a .bss object reached through the GOT; plus `bias`, e.g. a vtable's +0x10
          address point after its GOT entry).
  branch  the instruction at `at` is a B / BL; the target is the function it calls (for a function
          whose own body is not unique: a template twin, a PLT stub, a two-instruction getter).
  vtable  the signature finds one of the class's own virtual functions (`at`); the vtable is the
          only R_AARCH64_RELATIVE slot in the image pointing at it, minus `slot` * 8. With `entry`,
          the target is instead the function held `entry` slots after (or before) that one in the
          same table (a getter too small to sign, found through a unique sibling).

A signature is little-endian instruction words ("f657bda9 ????0090"), '?' a wildcard nibble.
`derive` builds them from a known build: the words from the site on, with every PC-relative field
masked (ADRP/ADR pages, B/BL targets, LDR literals, and the low-12 immediate of an ADD/LDR off an
ADRP register), grown until exactly one place in .text matches.

Resolution never guesses: zero or several matches, or a vtable function referenced by more than one
relocation, is a failure (resolve() returns None). Callers that know the build fall back to the
fixed offset only after the build id itself matched, and cross-check a successful scan against it.

    python3 tools/pokemon-sword/sw_sigscan.py check  --nso <exefs/main> [--variant sword]
    python3 tools/pokemon-sword/sw_sigscan.py derive --nso <exefs/main> [--variant sword] [--name N]
    python3 tools/pokemon-sword/sw_sigscan.py scan   --nso <other main>  [--variant sword]
"""
from __future__ import annotations

import argparse
import array
import collections
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

R_AARCH64_RELATIVE = 0x403
XREF_WINDOW = 5      # instructions after an ADRP searched for its ADD/LDR
MAX_SIG_WORDS = 32   # derive: longest signature tried before giving up on a start
MAX_BACK_WORDS = 16  # derive: how far before the site a signature may start
MIN_SIG_WORDS = 4    # derive: words kept from the anchor on even when fewer are already unique


# --- NSO image -----------------------------------------------------------------------------------

@dataclass
class Image:
    """A main NSO decompressed into its memory layout (offsets = main-relative)."""
    segments: list  # [(mem_offset, size, bytes)] text, rodata, data
    bss_size: int
    build_id: str   # 64 upper-case hex characters
    words: array.array  # .text as little-endian u32
    text_off: int
    relatives: dict  # R_AARCH64_RELATIVE: addend -> [slot offsets]
    slot_addends: dict  # R_AARCH64_RELATIVE: slot offset -> addend

    @property
    def text(self) -> bytes:
        return self.segments[0][2]

    def word(self, off: int) -> int:
        i = (off - self.text_off) // 4
        return self.words[i]

    def code_at(self, off: int, n: int = 4) -> bytes:
        t_off, size, body = self.segments[0]
        if not (t_off <= off and off + n <= t_off + size):
            raise ValueError(f"{off:#x} outside .text")
        return body[off - t_off:off - t_off + n]

    @property
    def end(self) -> int:
        mem_off, size, _ = self.segments[2]
        return mem_off + size + self.bss_size


def nso_segments(data: bytes):
    """(segments, bss_size, build_id) of an NSO0 file: [(mem_offset, size, decompressed bytes)]."""
    import lz4.block

    if data[:4] != b"NSO0":
        raise SystemExit("not an NSO0 image")
    flags = struct.unpack_from("<I", data, 0xC)[0]
    seg = []
    for i in range(3):
        file_off, mem_off, size, _ = struct.unpack_from("<IIII", data, 0x10 + i * 0x10)
        comp_size = struct.unpack_from("<I", data, 0x60 + i * 4)[0]
        raw = data[file_off:file_off + comp_size]
        body = lz4.block.decompress(raw, uncompressed_size=size) if flags & (1 << i) else raw[:size]
        seg.append((mem_off, size, body))
    bss = struct.unpack_from("<I", data, 0x3C)[0]
    build_id = data[0x40:0x60].hex().upper()
    return seg, bss, build_id


def _relatives(seg) -> dict:
    """{slot: addend} of every R_AARCH64_RELATIVE, from the MOD0 header's dynamic section."""
    end = max(m + s for m, s, _ in seg)
    img = bytearray(end)
    for m, s, b in seg:
        img[m:m + s] = b
    mod0 = struct.unpack_from("<I", img, 4)[0]
    if img[mod0:mod0 + 4] != b"MOD0":
        return {}
    dyn = mod0 + struct.unpack_from("<i", img, mod0 + 4)[0]
    tags = {}
    while dyn + 16 <= end:
        tag, val = struct.unpack_from("<QQ", img, dyn)
        dyn += 16
        if tag == 0:
            break
        tags.setdefault(tag, val)
    rela, relasz = tags.get(7), tags.get(8)  # DT_RELA, DT_RELASZ
    out = {}
    if rela is None or relasz is None:
        return out
    for at in range(rela, rela + relasz, 24):
        off, info, addend = struct.unpack_from("<QQq", img, at)
        if info & 0xFFFFFFFF == R_AARCH64_RELATIVE:
            out[off] = addend
    return out


def load(path) -> Image:
    seg, bss, build_id = nso_segments(Path(path).read_bytes())
    words = array.array("I")
    words.frombytes(seg[0][2][:len(seg[0][2]) // 4 * 4])
    if sys.byteorder != "little":
        words.byteswap()
    slot_addends = _relatives(seg)
    relatives = collections.defaultdict(list)
    for slot, addend in slot_addends.items():
        relatives[addend].append(slot)
    return Image(seg, bss, build_id, words, seg[0][0], dict(relatives), slot_addends)


# --- signatures ----------------------------------------------------------------------------------

def _nibble_mask(mask: int) -> int:
    """Widen a bit mask so every nibble is all-kept or all-wildcard (the text format's unit)."""
    out = 0
    for n in range(8):
        if (mask >> (4 * n)) & 0xF == 0xF:
            out |= 0xF << (4 * n)
    return out


def mask_words(words) -> list:
    """[(value, mask)] for instruction words, PC-relative and ADRP-page-relative fields masked."""
    out, page_regs = [], set()
    for w in words:
        m = 0xFFFFFFFF
        if (w & 0x1F000000) == 0x10000000:      # ADR / ADRP: immlo + immhi
            m &= ~((3 << 29) | (0x7FFFF << 5))
            if w & 0x80000000:
                page_regs.add(w & 31)
        elif (w & 0x7C000000) == 0x14000000:    # B / BL: imm26
            m &= ~0x03FFFFFF
        elif (w & 0x3B000000) == 0x18000000:    # LDR (literal): imm19
            m &= ~(0x7FFFF << 5)
        elif (w & 0x7F800000) == 0x11000000 and (w >> 5) & 31 in page_regs:  # ADD imm off an ADRP
            m &= ~(0xFFF << 10)
        elif (w & 0x3B000000) == 0x39000000 and (w >> 5) & 31 in page_regs:  # LDR/STR uimm off an ADRP
            m &= ~(0xFFF << 10)
        m = _nibble_mask(m)
        out.append((w & m, m))
    return out


def format_sig(masked) -> str:
    parts = []
    for v, m in masked:
        b = struct.pack("<I", v)
        bm = struct.pack("<I", m)
        s = ""
        for byte, bmask in zip(b, bm):
            for shift in (4, 0):
                s += "%x" % ((byte >> shift) & 0xF) if (bmask >> shift) & 0xF == 0xF else "?"
        parts.append(s)
    return " ".join(parts)


def parse_sig(text: str) -> list:
    out = []
    for part in text.split():
        if len(part) != 8:
            raise ValueError(f"signature word {part!r} is not 8 hex digits")
        val = mask = 0
        for i in range(4):
            for j, shift in enumerate((4, 0)):
                ch = part[2 * i + j]
                if ch != "?":
                    val |= int(ch, 16) << (8 * i + shift)
                    mask |= 0xF << (8 * i + shift)
        out.append((val, mask))
    return out


def find(image: Image, sig) -> list:
    """Every .text offset (main-relative) where the masked words match."""
    words, n = image.words, len(sig)
    if not sig:
        return []
    full = [i for i, (_v, m) in enumerate(sig) if m == 0xFFFFFFFF]
    hits = []
    if full:
        a = full[0]
        needle = struct.pack("<I", sig[a][0])
        text, pos = image.text, image.text.find(needle)
        while pos >= 0:
            if pos % 4 == 0:
                start = pos // 4 - a
                if 0 <= start and start + n <= len(words) and all(
                        (words[start + i] & m) == v for i, (v, m) in enumerate(sig)):
                    hits.append(image.text_off + 4 * start)
            pos = text.find(needle, pos + 1)
        return hits
    v0, m0 = sig[0]  # no fully fixed word: linear scan
    for start in range(len(words) - n + 1):
        if (words[start] & m0) == v0 and all((words[start + i] & m) == v for i, (v, m) in enumerate(sig)):
            hits.append(image.text_off + 4 * start)
    return hits


# --- sites ---------------------------------------------------------------------------------------

def decode_xref(image: Image, adrp_off: int):
    """Target of the ADRP at `adrp_off` + its ADD / LDR (unsigned offset) completion, or None."""
    w = image.word(adrp_off)
    if (w & 0x9F000000) != 0x90000000:
        return None
    rd = w & 31
    imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
    if imm & (1 << 20):
        imm -= 1 << 21
    page = (adrp_off & ~0xFFF) + (imm << 12)
    for k in range(1, XREF_WINDOW + 1):
        try:
            w2 = image.word(adrp_off + 4 * k)
        except IndexError:
            return None
        if (w2 >> 5) & 31 != rd:
            continue
        if (w2 & 0x7F800000) == 0x11000000 and not (w2 >> 22) & 1:     # ADD (imm, no shift)
            return page + ((w2 >> 10) & 0xFFF)
        if (w2 & 0x3B000000) == 0x39000000 and not (w2 >> 26) & 1:     # LDR/STR (uimm), GPR
            return page + (((w2 >> 10) & 0xFFF) << ((w2 >> 30) & 3))
        return None
    return None


def decode_branch(image: Image, off: int):
    """Target of the B / BL at `off`, or None."""
    w = image.word(off)
    if (w & 0x7C000000) != 0x14000000:
        return None
    imm = w & 0x03FFFFFF
    if imm & (1 << 25):
        imm -= 1 << 26
    return off + 4 * imm


def resolve(image: Image, site):
    """The site's offset in `image` by its signature, or None (no unique match: never a guess)."""
    if not site.sig:
        return None
    hits = find(image, parse_sig(site.sig))
    if len(hits) != 1:
        return None
    at = hits[0] + site.at
    return None if (got := _target(image, site, at)) is None else got + site.bias


def _target(image: Image, site, at: int):
    if site.kind == "code":
        return at
    if site.kind == "xref":
        return decode_xref(image, at)
    if site.kind == "got":
        slot = decode_xref(image, at)
        return image.slot_addends.get(slot) if slot is not None else None
    if site.kind == "branch":
        return decode_branch(image, at)
    if site.kind == "vtable":
        slots = image.relatives.get(at, [])
        if len(slots) != 1:
            return None
        if site.entry is not None:
            return image.slot_addends.get(slots[0] + 8 * site.entry)
        return slots[0] - 8 * site.slot
    raise ValueError(f"{site.name}: unknown site kind {site.kind!r}")


def resolve_all(image: Image, sites, known_build: bool):
    """{name: offset}: the scan's value, cross-checked against the fixed offset on the known build.

    known_build (the image's build id is the one these fixed offsets were verified on): a failed
    scan falls back to the fixed offset, a scan that disagrees is an error. Another build: every
    site must scan, or this raises (nothing is guessed).
    """
    out, failed, wrong = {}, [], []
    for s in sites:
        got = resolve(image, s)
        if got is None:
            if known_build and s.offset is not None:
                out[s.name] = s.offset
            else:
                failed.append(s.name)
        elif known_build and got != s.offset:
            wrong.append(f"{s.name}: scan {got:#x} != fixed {s.offset:#x}")
        else:
            out[s.name] = got
    if wrong:
        raise SystemExit("signature scan disagrees with the verified offsets:\n  " + "\n  ".join(wrong))
    if failed:
        raise SystemExit("no unique signature match (never guessed):\n  " + "\n  ".join(failed))
    return out


def derive(image: Image, site, max_words: int = MAX_SIG_WORDS, min_words: int = MIN_SIG_WORDS):
    """(signature, at) for a site: the masked word run starting at its anchor instruction (the
    function, the ADRP, the B/BL or the vtable's function), as short as stays unique in .text.
    Starting at the anchor is preferred (nothing from a neighbouring function); only if no run
    up to max_words is unique does the start move back, one word at a time."""
    anchor = {"code": site.offset, "vtable": site.fn}.get(site.kind, site.site)
    i_anchor = (anchor - image.text_off) // 4
    for back in range(0, MAX_BACK_WORDS + 1):
        i0 = i_anchor - back
        if i0 < 0:
            break
        masked = mask_words(image.words[i0:i0 + back + max_words])
        n = back + 1
        hits = find(image, masked[:n])
        while len(hits) > 1 and n < len(masked):
            v, m = masked[n]
            hits = [h for h in hits if (h - image.text_off) // 4 + n < len(image.words)
                    and (image.words[(h - image.text_off) // 4 + n] & m) == v]
            n += 1
        if len(hits) == 1 and hits[0] == image.text_off + 4 * i0:
            # a little more than strictly unique: one changed neighbour must not make it ambiguous
            return format_sig(masked[:max(n, back + min_words)]), 4 * back
    return None


def main() -> int:
    here = Path(__file__).resolve().parent
    sys.path.insert(0, str(here))
    import sw_variants

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("check", "derive", "scan"))
    ap.add_argument("--nso", default=os.environ.get("SWORD_NSO"), help="exefs/main (or SWORD_NSO)")
    ap.add_argument("--variant", default=sw_variants.DEFAULT, choices=sw_variants.VARIANTS)
    ap.add_argument("--name", action="append", help="derive: only these sites")
    a = ap.parse_args()
    if not a.nso:
        ap.error("--nso (or SWORD_NSO) is required")
    variant = sw_variants.VARIANTS[a.variant]
    image = load(a.nso)
    build = variant.build
    sites = build.sites
    if a.command == "derive":
        for s in sites:
            if a.name and s.name not in a.name:
                continue
            d = derive(image, s)
            print(f"{s.name}: " + (f'sig="{d[0]}", at={d[1]:#x}' if d else "NO UNIQUE SIGNATURE"))
        return 0
    known = image.build_id == build.build_id
    if a.command == "check":
        if not known:
            raise SystemExit(f"{a.nso}: build {image.build_id} is not {variant.key}'s {build.build_id}")
        bad = []
        for s in sites:
            got = resolve(image, s)
            if got != s.offset:
                bad.append(f"{s.name}: scan {'none' if got is None else hex(got)} != fixed {s.offset:#x}")
        for line in bad:
            print("MISMATCH", line)
        print(f"{len(sites) - len(bad)}/{len(sites)} sites: the signature scan finds the fixed offset"
              f" on {image.build_id[:16]}")
        return 1 if bad else 0
    resolved = resolve_all(image, sites, known)
    for s in sites:
        print(f"{s.name:32} {resolved[s.name]:#09x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
