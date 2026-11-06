#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 AlterDigga
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the load plan (format 2) with the guest helper for the follower actions.

  gen_action_helper.py --verify-image work/exefs/main --output <load-plan.json>
  gen_action_helper.py --verify-image work/exefs/main     (default path in the package)

The plan hooks native/helper/cotl/action_tick.c onto PlayerFarming.Update (0x1CFDCF0). The generator
aborts if the hook bytes in the image do not match. --verify-image takes the NSO file (main)
or an already unpacked image. Tools (clang, ld.lld, llvm-*) come from
$COTL_DEPS/android-ndk-r28c (default ~/cotl-deps) or from PATH.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

TITLE = "01002E7016C46000"
BUILD = "316A4D4881A9F1E077A07262E917917C" + "00" * 16
# (memory offset, size) of .text/.rodata/.data and bss, from the NSO header of main
SEGMENTS = [(0, 90160584), (90161152, 23461888), (113623040, 7687040)]
BSS = 3789952
HOOK = 0x1CFDCF0  # PlayerFarming.Update, erste Anweisung: sub sp, sp, #0x120
HOOK_BYTES = "ff8304d1"
MAILBOX_SIZE = 4096
EPOCH_OFFSET = 4  # session epoch, maintained by the host
MAX_CODE = 32768

SOURCE = os.path.join(ROOT, "native", "helper", "cotl", "action_tick.c")
DEFAULT_OUT = os.path.join(ROOT, "packages", "CultOfTheLamb", "dualscreen", "guest", BUILD,
                           "load-plan.json")


def tool(name):
    """Path of an LLVM tool: the $COTL_DEPS NDK first, then PATH."""
    deps = os.environ.get("COTL_DEPS", os.path.expanduser("~/cotl-deps"))
    path = os.path.join(deps, "android-ndk-r28c", "toolchains", "llvm", "prebuilt",
                        "linux-x86_64", "bin", name)
    if os.path.exists(path):
        return path
    found = shutil.which(name)
    if found:
        return found
    raise FileNotFoundError(f"{name} not found (COTL_DEPS={deps})")


def symbol_offsets(elf):
    out = subprocess.check_output([tool("llvm-nm"), "--defined-only", elf], text=True)
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def check_sections(elf):
    """The binary export takes only .text (the linker script puts .rodata there). Any
    other occupied section (.data, .bss, ...) would silently be dropped: the helper cannot use
    statics."""
    out = subprocess.check_output([tool("llvm-readelf"), "-S", "--wide", elf], text=True)
    row = re.compile(r"^\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+"
                     r"[0-9a-f]+\s*(\S*)\s+\d+\s+\d+\s+\d+\s*$")
    for line in out.splitlines():
        m = row.match(line)
        if m and "A" in m.group(4) and m.group(1) != ".text":
            raise ValueError(f"helper occupies section {m.group(1)}; statics are not carried over")


def compile_helper(source=SOURCE):
    """Compiles the helper (default action_tick.c) and returns (code, symbol table).
    Reproducible: only the source and the compiler version go in."""
    with tempfile.TemporaryDirectory(prefix="cotl-action-") as tmp:
        obj = os.path.join(tmp, "helper.o")
        elf = os.path.join(tmp, "helper.elf")
        binary = os.path.join(tmp, "helper.bin")
        script = os.path.join(tmp, "helper.ld")
        with open(script, "w") as f:
            f.write("SECTIONS { . = 0; .text : { *(.text.entry) *(.text*) *(.rodata*) "
                    ". = ALIGN(4); } /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) } }\n")
        # Only general registers and x18 free: no FP registers (v8-v15 are callee-saved)
        # and no access to the platform registers
        subprocess.run([tool("clang"), "--target=aarch64-none-elf", "-O2", "-ffreestanding",
                        "-fno-builtin", "-fno-jump-tables", "-fno-stack-protector",
                        "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
                        "-mgeneral-regs-only", "-ffixed-x18", "-Wall", "-Wextra", "-Werror",
                        "-c", source, "-o", obj], check=True)
        subprocess.run([tool("ld.lld"), "-T", script, "--entry=cotl_action_entry", obj,
                        "-o", elf], check=True)
        check_sections(elf)
        syms = symbol_offsets(elf)
        if syms.get("cotl_action_entry") != 0:
            raise ValueError("entry point not at offset 0")
        reloc = subprocess.check_output([tool("llvm-readelf"), "-r", elf], text=True)
        if "There are no relocations" not in reloc:
            raise ValueError("helper contains unresolved relocations")
        subprocess.run([tool("llvm-objcopy"), "-O", "binary", "--only-section=.text", elf,
                        binary], check=True)
        with open(binary, "rb") as f:
            code = f.read()
    if not code or len(code) % 4 or len(code) > MAX_CODE:
        raise ValueError("helper code size not allowed")
    return code, syms


def check_instructions(code):
    """Mirror of the host check (mod_load_plan.cpp, GuestCodeSize): it rejects 0xD4xxxxxx
    (SVC/HVC/SMC/BRK) and 0xD5xxxxxx except NOP (MSR/MRS, barriers, ...); the game would then
    run without the helper. The check applies to every word, including embedded constants."""
    for i in range(0, len(code), 4):
        word = struct.unpack_from("<I", code, i)[0]
        if (word & 0xFF000000) == 0xD4000000 or ((word & 0xFF000000) == 0xD5000000 and word != 0xD503201F):
            raise ValueError(f"The host rejects instruction {word:#010x} in the helper at {i:#x}")


def generate(image):
    """Load plan for the unpacked image of main; ValueError if the hook bytes differ."""
    if len(image) < HOOK + 4 or image[HOOK:HOOK + 4].hex() != HOOK_BYTES:
        raise ValueError("hook bytes of PlayerFarming.Update do not match (different build?)")
    code, syms = compile_helper()
    check_instructions(code)
    relocs = [
        dict(offset=syms["cotl_reloc_mailbox_adrp"], kind="aarch64_adrp", target="mailbox", addend=0),
        dict(offset=syms["cotl_reloc_mailbox_adrp"] + 4, kind="aarch64_add_lo12", target="mailbox", addend=0),
        dict(offset=syms["cotl_reloc_main_adrp"], kind="aarch64_adrp", target="main", addend=0),
        dict(offset=syms["cotl_reloc_main_adrp"] + 4, kind="aarch64_add_lo12", target="main", addend=0),
        dict(offset=syms["cotl_reloc_resume"], kind="aarch64_branch26", target="main", addend=HOOK + 4),
    ]
    relocs.sort(key=lambda r: r["offset"])
    return dict(
        format=2, title_id=TITLE, build_id=BUILD, module="main",
        layout=dict(segments=[dict(location=p, size=n) for p, n in SEGMENTS], bss_size=BSS),
        mailbox_size=MAILBOX_SIZE, mailbox_epoch_offset=EPOCH_OFFSET,
        # b code instead of the first instruction; the entry point runs that instruction itself
        patches=[dict(offset=HOOK, expected=HOOK_BYTES, replacement="00000014",
                      relocations=[dict(offset=0, kind="aarch64_branch26", target="code", addend=0)])],
        payloads=[], guest_code=dict(bytes=code.hex(), relocations=relocs))


def render(plan):
    return json.dumps(plan, indent=2) + "\n"


def load_main(data):
    """NSO0 file of main -> (header fields, unpacked image). Needs the lz4 package."""
    import lz4.block
    if data[:4] != b"NSO0":
        raise ValueError("not an NSO file")
    flags = struct.unpack_from("<I", data, 0xC)[0]
    image = bytearray()
    segments = []
    for i, (hdr, csize_off) in enumerate([(0x10, 0x60), (0x20, 0x64), (0x30, 0x68)]):
        file_off, mem_off, size = struct.unpack_from("<III", data, hdr)
        csize = struct.unpack_from("<I", data, csize_off)[0]
        raw = data[file_off:file_off + csize]
        seg = lz4.block.decompress(raw, uncompressed_size=size) if flags & (1 << i) else raw[:size]
        if len(image) < mem_off + size:
            image.extend(b"\0" * (mem_off + size - len(image)))
        image[mem_off:mem_off + size] = seg
        segments.append((mem_off, size))
    header = dict(build_id=data[0x40:0x60].hex().upper(), segments=segments,
                  bss_size=struct.unpack_from("<I", data, 0x3C)[0])
    return header, bytes(image)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--verify-image", required=True, help="main (NSO0) or unpacked image")
    p.add_argument("--output", default=DEFAULT_OUT)
    a = p.parse_args()
    with open(a.verify_image, "rb") as f:
        data = f.read()
    image = data
    if data[:4] == b"NSO0":
        header, image = load_main(data)
        if (header["build_id"], header["segments"], header["bss_size"]) != (BUILD, SEGMENTS, BSS):
            sys.exit("main has a different build or layout than the plan expects")
    try:
        plan = generate(image)
    except ValueError as e:
        sys.exit(str(e))
    os.makedirs(os.path.dirname(os.path.abspath(a.output)), exist_ok=True)
    with open(a.output, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(plan))
    print(a.output, len(plan["guest_code"]["bytes"]) // 2, "bytes of code")


if __name__ == "__main__":
    main()
