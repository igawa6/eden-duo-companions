#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static verification of the XCX guest helper's format-2 load plan.

--plan-only (no game data; the CTest): the shipped plan JSON is structurally what mod_load_plan.cpp
accepts, its guest code and relocation table equal a fresh assembly of native/helper/xcx_guest_helper.S,
every relocation sits on the instruction form its kind requires, every mailbox offset the assembly
uses is one xcx_helper_mailbox.h defines, no system instruction is present, all internal branches
land inside the code, and the hook is reproduce-and-resume (first helper word == displaced word, last
helper word branches to hook+4).

With --main-img and --nso (or XCX_MAIN_IMG / XCX_NSO) it also decodes the plan against the player's main
image: the build id and layout, the hook's `expected` bytes, every signature (exactly one match, at the
verified offset), every relocation target resolved with synthetic bases and compared with the address
it must name, the GOT slots of the data symbols, and it prints the disassembly of the original and the
patched hook region and of the relocated helper.

    python3 -I tools/xcx/verify_helper_plan.py --plan-only
    python3 -I tools/xcx/verify_helper_plan.py --main-img <main.img> --nso <exefs/main> [--disasm]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_helper_plan as bp  # noqa: E402
import xcx_sites  # noqa: E402

REPO_ROOT = HERE.parents[1]
PLAN = REPO_ROOT / ("packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/guest/"
                    f"{xcx_sites.BUILD_ID}/load-plan.json")
HEADER = REPO_ROOT / "native/modules/xcx_helper_mailbox.h"
MANIFEST = REPO_ROOT / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json"

# Synthetic bases for relocation checks. Real ones are chosen by the loader; the decode below only
# needs them distinct, page-aligned, and within the branch range of main (+-128 MiB).
MAIN_BASE = 0
CODE_BASE = 0x4800000      # just after main's bss (0x47bf000)
MAILBOX_BASE = 0x4810000


def fail(msg: str):
    print("FAIL:", msg)
    sys.exit(1)


def sx(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def apply_relocations(code: bytes, relocations, code_base=CODE_BASE, main_base=MAIN_BASE,
                      mailbox_base=MAILBOX_BASE) -> bytes:
    """Patch `code` the way the loader must: branch26 / adrp / add_lo12 to main, mailbox or code."""
    out = bytearray(code)
    base = {"main": main_base, "mailbox": mailbox_base, "code": code_base}
    for r in relocations:
        at = r["offset"]
        pc = code_base + at
        target = base[r["target"]] + r["addend"]
        word = struct.unpack_from("<I", out, at)[0]
        kind = r["kind"]
        if kind == "aarch64_branch26":
            assert (word & 0x7C000000) == 0x14000000, f"+{at:#x}: not b/bl"
            delta = target - pc
            assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), f"+{at:#x}: branch out of range"
            word = (word & 0xFC000000) | ((delta >> 2) & 0x03FFFFFF)
        elif kind == "aarch64_adrp":
            assert (word & 0x9F000000) == 0x90000000, f"+{at:#x}: not adrp"
            pages = (target >> 12) - (pc >> 12)
            assert -(1 << 20) <= pages < (1 << 20), f"+{at:#x}: adrp out of range"
            word = (word & 0x9F00001F) | ((pages & 3) << 29) | (((pages >> 2) & 0x7FFFF) << 5)
        elif kind == "aarch64_add_lo12":
            assert (word & 0x7F800000) == 0x11000000, f"+{at:#x}: not add imm"
            word = (word & ~(0xFFF << 10)) | ((target & 0xFFF) << 10)
        else:
            raise AssertionError(kind)
        struct.pack_into("<I", out, at, word)
    return bytes(out)


def decode_targets(code: bytes, relocations, code_base=CODE_BASE):
    """Independent decode of the patched code: {reloc offset: address the instruction(s) name}."""
    out = {}
    adrp_page = {}
    for r in sorted(relocations, key=lambda r: r["offset"]):
        at = r["offset"]
        word = struct.unpack_from("<I", code, at)[0]
        pc = code_base + at
        if r["kind"] == "aarch64_branch26":
            out[at] = pc + 4 * sx(word & 0x03FFFFFF, 26)
        elif r["kind"] == "aarch64_adrp":
            imm = sx(((word >> 29) & 3) | (((word >> 5) & 0x7FFFF) << 2), 21)
            adrp_page[word & 31] = ((pc >> 12) + imm) << 12
            out[at] = None
        else:
            rn = (word >> 5) & 31
            out[at] = adrp_page[rn] + ((word >> 10) & 0xFFF)
    return out


def parse_header_offsets():
    text = HEADER.read_text()
    offs = {}
    for name, val in re.findall(r"inline constexpr std::uint32_t (Offset\w+|EpochOffset)\s*=\s*(0x[0-9a-fA-F]+|\d+)", text):
        offs[name] = int(val, 0)
    return offs


def asm_mailbox_offsets():
    """Every immediate used with the mailbox base register x19 in the assembly source."""
    src = (REPO_ROOT / "native/helper/xcx_guest_helper.S").read_text()
    offs = set()
    for m in re.finditer(r"\[x19(?:,\s*#(0x[0-9a-fA-F]+|\d+))?\]", src):
        offs.add(int(m.group(1), 0) if m.group(1) else 0)
    for m in re.finditer(r"add\s+x\d+,\s*x19,\s*#(0x[0-9a-fA-F]+|\d+)", src):
        offs.add(int(m.group(1), 0))
    return offs


def check_plan_structure(plan):
    assert plan["format"] == 2 and plan["module"] == "main"
    assert plan["title_id"] == xcx_sites.TITLE_ID and plan["build_id"] == xcx_sites.BUILD_ID
    assert plan["mailbox_size"] == bp.MAILBOX_SIZE == 4096
    assert plan["mailbox_epoch_offset"] == bp.MAILBOX_EPOCH_OFFSET and plan["mailbox_epoch_offset"] % 4 == 0
    assert len(plan["layout"]["segments"]) == 3 and plan["layout"]["bss_size"] > 0
    assert len(plan["patches"]) == 1
    patch = plan["patches"][0]
    assert patch["offset"] == bp.HOOK_OFFSET and patch["expected"] == bp.HOOK_EXPECTED
    assert len(bytes.fromhex(patch["replacement"])) == 4
    assert patch["relocations"] == [{"offset": 0, "kind": "aarch64_branch26", "target": "code", "addend": 0}]
    code = bytes.fromhex(plan["guest_code"]["bytes"])
    assert len(code) % 4 == 0 and 0 < len(code) <= 64 * 1024
    return code


def check_code_shape(code: bytes, relocations):
    n = len(code) // 4
    reloc_at = {r["offset"] for r in relocations}
    words = struct.unpack("<%dI" % n, code)
    # reproduce-and-resume
    assert words[0] == int(bp.HOOK_EXPECTED[6:8] + bp.HOOK_EXPECTED[4:6] + bp.HOOK_EXPECTED[2:4] + bp.HOOK_EXPECTED[0:2], 16)
    resume = [r for r in relocations if r["target"] == "main" and r["addend"] == bp.HOOK_OFFSET + 4]
    assert len(resume) == 1 and resume[0]["kind"] == "aarch64_branch26"
    assert (words[resume[0]["offset"] // 4] & 0xFC000000) == 0x14000000, "resume must be a plain b"
    # the instruction right before `ldp x29,x30 / add sp` ... is covered by emulation; here: no
    # system instruction, every internal b/bl/cbz/tbz/b.cond target inside the code
    for i, w in enumerate(words):
        at = 4 * i
        assert (w & 0xFF000000) != 0xD4000000 and (w & 0xFF000000) != 0xD5000000, f"+{at:#x}: system instruction"
        tgt = None
        if at not in reloc_at and (w & 0x7C000000) == 0x14000000:
            tgt = at + 4 * sx(w & 0x03FFFFFF, 26)
        elif (w & 0xFF000010) == 0x54000000:                 # b.cond
            tgt = at + 4 * sx((w >> 5) & 0x7FFFF, 19)
        elif (w & 0x7E000000) == 0x34000000:                 # cbz / cbnz
            tgt = at + 4 * sx((w >> 5) & 0x7FFFF, 19)
        elif (w & 0x7E000000) == 0x36000000:                 # tbz / tbnz
            tgt = at + 4 * sx((w >> 5) & 0x3FFF, 14)
        if tgt is not None:
            assert 0 <= tgt < len(code) and tgt % 4 == 0, f"+{at:#x}: branch leaves the code ({tgt:#x})"
    # only ldar / stlr style acquire/release forms on the sequence words: both are present
    assert any((w & 0xFFFFFC00) == 0x88DFFC00 for w in words), "no ldar w"
    assert any((w & 0xFFFFFC00) == 0x889FFC00 for w in words), "no stlr w"
    # only stp-pairs / sub sp / add sp touch sp: frame must be balanced (equal sub and add counts)
    subs = sum(1 for w in words if (w & 0xFFC003FF) == 0xD10003FF)
    adds = sum(1 for w in words if (w & 0xFFC003FF) == 0x910003FF)
    assert subs == adds == 1, f"scratch frame not balanced (sub {subs}, add {adds})"


def check_mailbox_offsets():
    header = parse_header_offsets()
    asm = asm_mailbox_offsets()
    known = set(header.values())
    missing = sorted(hex(o) for o in asm if o not in known)
    assert not missing, f"assembly uses mailbox offsets the header does not define: {missing}"
    assert header["EpochOffset"] == bp.MAILBOX_EPOCH_OFFSET == 0xFFC
    return header, asm


def disassemble(code: bytes, base: int, label: str):
    with tempfile.TemporaryDirectory() as tmp:
        raw, obj = Path(tmp) / "x.bin", Path(tmp) / "x.o"
        raw.write_bytes(code)
        subprocess.run(["llvm-objcopy", "-I", "binary", "-O", "elf64-littleaarch64",
                        "--rename-section", ".data=.text,alloc,load,readonly,code,contents",
                        str(raw), str(obj)], check=True)
        text = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", f"--adjust-vma={base:#x}", str(obj)],
                              check=True, capture_output=True, text=True).stdout
    print(f"--- {label}")
    print("\n".join(line for line in text.splitlines() if re.match(r"\s+[0-9a-f]+:", line)))


def check_manifest():
    m = json.loads(MANIFEST.read_text())
    assert m["load_plan"] == f"guest/{xcx_sites.BUILD_ID}/load-plan.json", m.get("load_plan")
    assert m["load_plan_module"] == "main"
    assert m["min_runtime"] >= 19, "format 2 needs runtime 19"
    assert m["actions"]["switch_set"]["action"] == "switch_set"
    assert m["actions"]["return_hold"]["action"] == "return_hold"
    assert "skell_button" in m["actions"] and m["actions"]["skell_button"]["action"] == "skell_button"
    return m


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plan-only", action="store_true")
    ap.add_argument("--main-img", default=os.environ.get("XCX_MAIN_IMG"))
    ap.add_argument("--nso", default=os.environ.get("XCX_NSO"))
    ap.add_argument("--disasm", action="store_true")
    a = ap.parse_args()
    if not PLAN.exists():
        fail(f"plan missing: {PLAN}")
    plan = json.loads(PLAN.read_text())
    code = check_plan_structure(plan)
    fresh_code, relocs, entry = bp.assemble()
    assert fresh_code == code, "the shipped plan's guest code differs from a fresh assembly of the .S"
    assert bp.plan_relocations(relocs) == plan["guest_code"]["relocations"], "relocation table differs"
    check_code_shape(code, plan["guest_code"]["relocations"])
    header, asm = check_mailbox_offsets()
    check_manifest()
    patched = apply_relocations(code, plan["guest_code"]["relocations"])
    targets = decode_targets(patched, plan["guest_code"]["relocations"])
    want = {}
    for r in plan["guest_code"]["relocations"]:
        base = {"main": MAIN_BASE, "mailbox": MAILBOX_BASE}[r["target"]]
        if r["kind"] == "aarch64_adrp":
            continue
        want[r["offset"]] = base + r["addend"]
    for at, tgt in targets.items():
        if tgt is not None:
            assert tgt == want[at], f"+{at:#x}: decodes to {tgt:#x}, plan says {want[at]:#x}"
    print(f"plan structure OK: guest code {len(code)} bytes, {len(relocs)} relocations, "
          f"{len(asm)} distinct mailbox offsets all defined in xcx_helper_mailbox.h")
    if a.plan_only or not (a.main_img and a.nso):
        if not a.plan_only:
            print("(no --main-img/--nso: main-image checks skipped)")
        return 0

    image = xcx_sites.load(a.main_img, a.nso)
    bp.verify_image(image)
    assert plan["build_id"] == image.build_id
    assert plan["layout"]["segments"] == [{"location": s[0], "size": s[1]} for s in image.segments]
    assert plan["layout"]["bss_size"] == image.bss_size
    patch = plan["patches"][0]
    got = image.code(patch["offset"], len(bytes.fromhex(patch["expected"]))).hex()
    assert got == patch["expected"], f"hook bytes {got} != plan {patch['expected']}"
    problems, report = xcx_sites.check(image)
    print("\n".join(report))
    assert not problems, problems
    callers = xcx_sites.decode_bl_callers(image, bp.HOOK_OFFSET)
    print("direct callers of the hook function:", [(hex(a_), k) for a_, k in callers])
    # every call target is a function entry the signatures pin; every data symbol is in bss/data
    for r in plan["guest_code"]["relocations"]:
        if r["target"] != "main":
            continue
        off = r["addend"]
        if r["kind"] == "aarch64_branch26":
            image.code(off)  # in .text
        else:
            assert off >= image.segments[2][0] + image.segments[2][1] or off >= image.segments[1][0], \
                f"data symbol {off:#x} not in rodata/data/bss"
    # reproduce-and-resume against the real bytes
    hook = bp.HOOK_OFFSET
    original = image.code(hook - 4, 0x18)
    patched_hook = bytearray(original)
    patched_hook[4:8] = struct.pack("<I", 0x14000000 | (((CODE_BASE - hook) >> 2) & 0x03FFFFFF))
    assert code[:4].hex() == image.code(hook).hex(), "helper must start with the displaced instruction"
    resume_at = [r["offset"] for r in plan["guest_code"]["relocations"]
                 if r["target"] == "main" and r["addend"] == hook + 4][0]
    last = struct.unpack_from("<I", patched, resume_at)[0]
    assert (last & 0xFC000000) == 0x14000000 and \
        (CODE_BASE + resume_at) + 4 * sx(last & 0x03FFFFFF, 26) == hook + 4, \
        "the helper must resume by branching to hook+4"
    assert (struct.unpack_from("<I", image.code(hook + 4))[0] & 0xFC000000) != 0x14000000
    print(f"hook main+{hook:#x}: original {image.code(hook).hex()} (stp x29,x30,[sp,#-0x20]!), "
          f"patched b {CODE_BASE:#x} (helper entry); helper word0 {code[:4].hex()}, "
          f"helper resumes at main+{hook + 4:#x} ({image.code(hook + 4).hex()})")
    # every data symbol the helper reads is one the game's own code references (GOT slot and/or direct)
    for name, (off, _got, _note) in xcx_sites.DATA_SYMBOLS.items():
        if name == "return_flag_off":
            continue
        derived = {"btl_lock": (0x1d598e8, 0x168), "mode_1618": (0x1d54eb8, 0x180),
                   "event_byte": (0x1d55e88, 0x110)}   # [GOT slot] + field offset
        if name in derived:
            slot, delta = derived[name]
            assert image.u64(slot) + delta == off, f"{name}: GOT {slot:#x} + {delta:#x} != {off:#x}"
            print(f"data {name:15} main+{off:#09x} = [GOT {slot:#x}] ({image.u64(slot):#x}) + {delta:#x}")
            continue
        slots, direct = xcx_sites.references(image, off)
        assert slots or direct, f"data symbol {name} {off:#x} is referenced by nothing in the image"
        print(f"data {name:15} main+{off:#09x} GOT slots {[hex(x) for x in slots][:2]} direct refs {len(direct)}")
    print("every expected byte, signature, relocation target and GOT cross-check passed")
    if a.disasm:
        disassemble(original, hook - 4, "original main[0x409ed0..0x409ee8)")
        disassemble(bytes(patched_hook), hook - 4, "patched main[0x409ed0..0x409ee8) (b -> helper entry)")
        disassemble(patched, CODE_BASE, f"relocated helper (code base {CODE_BASE:#x}, mailbox base {MAILBOX_BASE:#x}, main base 0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
