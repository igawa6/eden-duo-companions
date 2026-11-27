#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Main-relative sites of the Xenoblade Chronicles X DE 1.0.2 guest helper, and byte-pattern scans.

Offline only. The game's executable is never part of this repository: pass the player's flat
unpacked main image (text/rodata/data at their NSO memory offsets, no bss) with --main-img (or
XCX_MAIN_IMG) and its NSO header file with --nso (or XCX_NSO, only the first 0x100 bytes are read).

A signature is little-endian instruction words ("fd7bbea9 ????0090"), '?' a wildcard nibble. Every
PC-relative field (ADR/ADRP pages, B/BL targets, LDR literals, the low-12 immediate of an ADD/LDR
off an ADRP register) is masked, so a signature survives a relink that only moves things. A site is
accepted only when its signature matches exactly once in .text AND at the verified offset.

    python3 -I tools/xcx/xcx_sites.py check  --main-img <main.img> --nso <exefs/main>
    python3 -I tools/xcx/xcx_sites.py derive --main-img <main.img> --nso <exefs/main>
"""
from __future__ import annotations

import argparse
import array
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

TITLE_ID = "0100453019AA8000"
BUILD_ID = "2A720C7CE5C84905C1C6847211F670FBB31842B6000000000000000000000000"

MAX_SIG_WORDS = 90
MIN_SIG_WORDS = 6


@dataclass(frozen=True)
class Site:
    name: str
    offset: int          # main-relative, verified on BUILD_ID
    sig: str = ""        # masked words from `offset` (empty: data, no signature)
    note: str = ""


# Code sites: the hook and the game functions the helper calls. Signatures are filled by `derive`
# and pasted below (SIGNATURES); the offsets are what the 1.0.2 build was verified at.
CODE_SITES = [
    ("hook",          0x409ed4, "field HUD per-frame update (called from vf_40ce08 once per frame)"),
    ("resume",        0x409ed8, "instruction after the hook: stp x20,x19,[sp,#0x10]"),
    ("ride_request",  0x2052fc, "tryRideOn(character_actor*): canRideOn + command 0x3d"),
    ("exit_request",  0x20538c, "tryGetOff(skell_actor*): singleton gate + canGetOff + command 0x3e"),
    ("can_ride_on",   0x1b9c14, "canRideOn(character_actor*)"),
    ("can_get_off",   0x1b9fbc, "canGetOff(skell_actor*)"),
    ("resolve",       0x01a2dc, "handle resolver: x0 = u32* handle, returns object* or null"),
    ("slot_handle",   0x174eec, "x8 = u32* out, w0 = party slot: the slot's character handle"),
    ("slot_record",   0x175014, "w0 = party slot: member record pointer (u8 kind +0x78, u8 id +0x79) or null"),
    ("init_tmp",      0x00d75c, "x0 = 0x400-byte scratch: initialise a member-detail buffer"),
    ("lookup",        0x56bed0, "w0 = member id, x1 = scratch: fills it, returns bool (aux u8 +0x3f4, u16 +0x3f6)"),
    ("stage",         0x41a704, "x0 = request block R, w1 slot, w2 kind, w3 id, w4 aux1, w5 aux2, w6 = 1"),
    ("commit",        0x41a7a0, "x0 = R, w1 = 1: queue the staged switch for the consumer at 0x409f98"),
    ("can_return",    0x0f16b0, "no arguments: bool, the native Return to Skell can-return predicate"),
    ("field_ok",      0x0f0b44, "no arguments: bool, the game's generic field-action-allowed predicate"),
    ("cooldown",      0x0f0388, "no arguments: bool, 1 = a party actor lacks +0xd0 bit1 (switch not allowed yet)"),
]
# Data (bss) symbols: main-relative addresses, cross-checked against the game's own GOT slots.
# name -> (main offset, GOT slot offset holding it in the image or 0, note)
DATA_SYMBOLS = {
    "party_manager":  (0x1df58d0, 0x1d55110, "4 records of 0x14 from +0xc"),
    "exit_singleton": (0x1e0bec0, 0x1d55888, "u32 at +0x90 must be 0 for tryGetOff"),
    "hud_global":     (0x1df5978, 0x1d55158, "pointer to the field HUD object (hook x0)"),
    "prompt_gate":    (0x1feffc0, 0,         "pointer; byte +0x158 == 0 hides the action prompts (diagnostic)"),
    "input_object":   (0x1fefb70, 0x1d55ec0, "object whose byte +0xb0 bit2 (0x1e1bdc(obj,4)) is the game's input-suppressed mark, tested first by canRideOn"),
    "return_flag_off": (0x263a5, 0, "H + 0x263a5: byte the Return to Skell menu entry sets to 1 (0x41e8d4: strb 1,[H+0x262b8+0xed]); consumed by 0x41dd80 at 0x40a024"),
    "action_block":   (0x1ff3698, 0, "pointer; u32 +0x44 is the action-block mask read by 0xf0b44"),
    "btl_lock":       (0x26e1950, 0, "u32 battle-lock counter"),
    "flag_2308":      (0x26e2308, 0, "u8 read by 0xf0b44"),
    "mode_1618":      (0x1dc1618, 0, "u32 read by 0xf0b44 / 0x130258"),
    "ui_singleton":   (0x1dc1410, 0, "pointer; fade object = *(U+0x94b98)"),
    "zone_ptr":       (0x1df5968, 0, "pointer to the zone record; u32 +0x140 world id, +0xa8 zone"),
    "event_byte":     (0x1fefb38, 0, "u8 isEventScene"),
    "obj_1f91e30":    (0x1f91e30, 0, "pointer; u8 +0x14 read by 0xf0b44"),
    "obj_1fefa20":    (0x1fefa20, 0, "pointer; pointer +0x30 read by 0xf0b44"),
    "ctrl_object":    (0x1df5960, 0x1d550f0, "pointer; read by cooldown (0xf0388): must be non-null before calling it"),
}

SIGNATURES = {
    # from `derive`: the shortest masked run from the site that matches exactly once in .text
    "hook": "fd7bbea9 f44f01a9 fd030091 ?8?????0 f30300aa 08????f9 08fddf08 880e0036",
    "resume": "f44f01a9 fd030091 ?8?????0 f30300aa 08????f9 08fddf08 880e0036",
    "ride_request": "600400b4 fd7bbda9 f50b00f9 f44f02a9 fd030091 f30300aa",
    "exit_request": "200300b4 fd7bbea9 f30b00f9 fd030091 ?8?????0 f30300aa",
    "can_ride_on": "ffc302d1 e83b00fd fd7b08a9 f65709a9 f44f0aa9 fd030291 ?8?????0",
    "can_get_off": "000200b4 fd7bbda9 f50b00f9 f44f02a9 fd030091 08404339",
    "resolve": "080040b9 1f490072 60030054 ff8300d1 fd7b01a9 fd430091",
    # slot_handle: its masked form equals its sibling 0x1751f8 (the +0xc / +0x10 loads are masked as
    # page-relative), so this one is an exact, unmasked run of 14 words, unique in .text
    "slot_handle": "ff8300d1 fd7b01a9 fd430091 09df00b0 298540f9 29fddf08 09020036 1f0c0071 a9000054 fd7b41a9 1f0100b9 ff830091 c0035fd6 09df00b0",
    "slot_record": "ff8300d1 fd7b01a9 fd430091 ?8?????0 08????91 08fddf08 a8030036 1f0c0071 a9000054 e0031faa fd7b41a9 ff830091 c0035fd6 ?8?????0 89028052 08????f9 0820a99b a01300d1 01????91 ??????9? a01300d1 ??????9? 20010036 a01300d1 ??????9? ??????9? 00feffb4 000440f9 fd7b41a9 ff830091 c0035fd6 e0031faa fd7b41a9 ff830091 c0035fd6 ?8?????0 08????91 e00b00b9 e00308aa ??????9? e803002a e00b40b9 a8fbff34 ?0?????0 00????91 ??????9? ?0?????0 00????91 ??????9? e00b40b9 ??????1? ff8300d1 fd7b01a9 fd430091 ?8?????0 08????f9 08fddf08 88010036 1f0c0071 e8000054 ?8?????0",
    "init_tmp": "fd7bbea9 f44f01a9 fd030091 f30300aa 00800091 7f020039",
    "lookup": "1f580071 68040054 ?8?????0 08????91 08????f9 e80300b4",
    "stage": "3f0c0071 c8020054 09e00091 0ba00291 df000072 6911899a",
    "commit": "01dc0239 01010036 28008052 6800a072 08b000b9 a8008052",
    "can_return": "ff8300d1 fd7b01a9 fd430091 ??????9? 00050036 ?8?????0",
    "field_ok": "ffc300d1 fd7b01a9 f44f02a9 fd430091 ?4?????0 94????91 88????f9 880000b4",
    "cooldown": "fd7bbea9 f30b00f9 fd030091 ?3?????0 73????91 60????f9 08404039",
}


# --- flat image ------------------------------------------------------------------------------

class Image:
    def __init__(self, main_img: bytes, nso_header: bytes):
        if nso_header[:4] != b"NSO0":
            raise SystemExit("not an NSO0 header")
        self.segments = []
        for i in range(3):
            _file_off, mem_off, size, _align = struct.unpack_from("<IIII", nso_header, 0x10 + i * 0x10)
            self.segments.append((mem_off, size))
        self.bss_size = struct.unpack_from("<I", nso_header, 0x3C)[0]
        self.build_id = nso_header[0x40:0x60].hex().upper()
        self.data = main_img
        mem_off, size = self.segments[2]
        if len(main_img) != mem_off + size:
            raise SystemExit(f"main image is {len(main_img):#x} bytes, layout says {mem_off + size:#x}")
        text_off, text_size = self.segments[0]
        self.text_off = text_off
        self.text = main_img[text_off:text_off + text_size]
        self.words = array.array("I")
        self.words.frombytes(self.text[: len(self.text) // 4 * 4])
        if sys.byteorder != "little":
            self.words.byteswap()

    def u32(self, off: int) -> int:
        return struct.unpack_from("<I", self.data, off)[0]

    def u64(self, off: int) -> int:
        return struct.unpack_from("<Q", self.data, off)[0]

    def code(self, off: int, n: int = 4) -> bytes:
        if not (self.text_off <= off and off + n <= self.text_off + len(self.text)):
            raise ValueError(f"{off:#x} outside .text")
        return self.data[off:off + n]

    def word(self, off: int) -> int:
        return self.words[(off - self.text_off) // 4]


def load(main_img: str, nso: str) -> Image:
    return Image(Path(main_img).read_bytes(), Path(nso).read_bytes()[:0x100])


# --- signatures ------------------------------------------------------------------------------

def _nibble_mask(mask: int) -> int:
    out = 0
    for n in range(8):
        if (mask >> (4 * n)) & 0xF == 0xF:
            out |= 0xF << (4 * n)
    return out


def mask_words(words):
    """[(value, mask)]: PC-relative and ADRP-page-relative fields masked."""
    out, page_regs = [], set()
    for w in words:
        m = 0xFFFFFFFF
        if (w & 0x1F000000) == 0x10000000:      # ADR / ADRP
            m &= ~((3 << 29) | (0x7FFFF << 5))
            if w & 0x80000000:
                page_regs.add(w & 31)
        elif (w & 0x7C000000) == 0x14000000:    # B / BL
            m &= ~0x03FFFFFF
        elif (w & 0x3B000000) == 0x18000000:    # LDR literal
            m &= ~(0x7FFFF << 5)
        elif (w & 0x7F800000) == 0x11000000 and (w >> 5) & 31 in page_regs:   # ADD off ADRP
            m &= ~(0xFFF << 10)
        elif (w & 0x3B000000) == 0x39000000 and (w >> 5) & 31 in page_regs:   # LDR/STR off ADRP
            m &= ~(0xFFF << 10)
        m = _nibble_mask(m)
        out.append((w & m, m))
    return out


def format_sig(masked) -> str:
    parts = []
    for v, m in masked:
        b, bm = struct.pack("<I", v), struct.pack("<I", m)
        s = ""
        for byte, bmask in zip(b, bm):
            for shift in (4, 0):
                s += "%x" % ((byte >> shift) & 0xF) if (bmask >> shift) & 0xF == 0xF else "?"
        parts.append(s)
    return " ".join(parts)


def parse_sig(text: str):
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


def find(image: Image, sig):
    """Every .text offset (main-relative) where the masked words match."""
    words, n = image.words, len(sig)
    full = [i for i, (_v, m) in enumerate(sig) if m == 0xFFFFFFFF]
    if not sig or not full:
        raise ValueError("a signature needs at least one fully fixed word")
    a = full[0]
    needle = struct.pack("<I", sig[a][0])
    hits, pos = [], image.text.find(needle)
    while pos >= 0:
        if pos % 4 == 0:
            start = pos // 4 - a
            if 0 <= start and start + n <= len(words) and all(
                    (words[start + i] & m) == v for i, (v, m) in enumerate(sig)):
                hits.append(image.text_off + 4 * start)
        pos = image.text.find(needle, pos + 1)
    return hits


def derive(image: Image, offset: int):
    """Shortest masked run starting at `offset` that is unique in .text (at least MIN_SIG_WORDS)."""
    i0 = (offset - image.text_off) // 4
    masked = mask_words(image.words[i0:i0 + MAX_SIG_WORDS])
    for n in range(MIN_SIG_WORDS, len(masked) + 1):
        if not any(m == 0xFFFFFFFF for _v, m in masked[:n]):
            continue
        hits = find(image, masked[:n])
        if hits == [offset]:
            return format_sig(masked[:n])
    return None


def decode_bl_callers(image: Image, target: int):
    """Direct B/BL instructions in .text whose target is `target` (an indirect call is not seen)."""
    hits = []
    words = image.words
    for i, w in enumerate(words):
        if (w & 0x7C000000) == 0x14000000:
            imm = w & 0x03FFFFFF
            if imm & (1 << 25):
                imm -= 1 << 26
            at = image.text_off + 4 * i
            if at + 4 * imm == target:
                hits.append((at, "bl" if w & 0x80000000 else "b"))
    return hits


def references(image: Image, addr: int):
    """Evidence that `addr` is a data symbol the game's own code uses: a GOT-style u64 in rodata/data
    holding it, and/or direct ADRP+ADD/LDR/STR references in .text. Returns (got slots, direct refs)."""
    import struct as _s
    lo = image.segments[1][0]
    got = [off for off in range(lo, len(image.data) - 7, 8) if _s.unpack_from("<Q", image.data, off)[0] == addr]
    page = addr & ~0xFFF
    direct = []
    for i, w in enumerate(image.words):
        if (w & 0x9F000000) != 0x90000000:
            continue
        at = image.text_off + 4 * i
        imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
        if imm & (1 << 20):
            imm -= 1 << 21
        if ((at & ~0xFFF) + (imm << 12)) != page:
            continue
        got_t = decode_xref(image, at)
        if got_t == addr:
            direct.append(at)
    return got, direct


def decode_xref(image: Image, adrp_off: int):
    w = image.word(adrp_off)
    rd = w & 31
    imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
    if imm & (1 << 20):
        imm -= 1 << 21
    page = (adrp_off & ~0xFFF) + (imm << 12)
    for k in range(1, 6):
        try:
            w2 = image.word(adrp_off + 4 * k)
        except IndexError:
            return None
        if (w2 >> 5) & 31 != rd:
            continue
        if (w2 & 0x7F800000) == 0x11000000 and not (w2 >> 22) & 1:
            return page + ((w2 >> 10) & 0xFFF)
        if (w2 & 0x3B000000) == 0x39000000 and not (w2 >> 26) & 1:
            return page + (((w2 >> 10) & 0xFFF) << ((w2 >> 30) & 3))
        return None
    return None


def check(image: Image):
    """(problems, report lines): every code site found by its signature exactly at its offset."""
    problems, report = [], []
    if image.build_id != BUILD_ID:
        problems.append(f"NSO build id {image.build_id} != {BUILD_ID}")
    for name, off, _note in CODE_SITES:
        sig = SIGNATURES.get(name)
        if not sig:
            continue
        hits = find(image, parse_sig(sig))
        ok = hits == [off]
        report.append(f"{name:14} main+{off:#08x} sig words={len(sig.split()):2} hits={[hex(h) for h in hits]}")
        if not ok:
            problems.append(f"{name}: signature hits {[hex(h) for h in hits]}, expected [{off:#x}]")
    return problems, report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("check", "derive"))
    ap.add_argument("--main-img", default=os.environ.get("XCX_MAIN_IMG"))
    ap.add_argument("--nso", default=os.environ.get("XCX_NSO"))
    a = ap.parse_args()
    if not a.main_img or not a.nso:
        ap.error("--main-img and --nso (or XCX_MAIN_IMG / XCX_NSO) are required")
    image = load(a.main_img, a.nso)
    if a.command == "derive":
        for name, off, _note in CODE_SITES:
            sig = derive(image, off)
            print(f'    "{name}": "{sig}",' if sig else f"# {name}: NO UNIQUE SIGNATURE")
        return 0
    problems, report = check(image)
    print("\n".join(report))
    for p in problems:
        print("PROBLEM", p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
