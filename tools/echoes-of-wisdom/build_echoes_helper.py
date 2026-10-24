#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a format-2 guest-helper load plan for Echoes of Wisdom (EoW). Adapted
from the Breath of the Wild format-2 builder
(a sibling Breath of the Wild format-2 builder script); see
HELPER-TOOLCHAIN.md section 4 for the full side-by-side comparison.

This script ONLY assembles, relocates and writes a plan.json under this directory's
own runs/ folder. It never touches a package, a manifest, or any other game's
research tree, and never installs anything into a running Eden Duo data directory.

hook.S contains THREE independent guest routines, selected by --hook-kind:
  bl      echoes_dsmod_ping_hook -- the original toolchain-proof skeleton. The
          hook site is an existing `bl <callee>`; the hook replays that call
          itself (so the call still happens, in order) and then `ret`s to the
          instruction after the patch site. Ping (request kind 1) only.
  tail-b  echoes_helper -- the real EoW DoAction-HUD hook (main+0x24ff4d0). The
          hook site is an unconditional TAIL branch (`b <callee>`, no `bl`),
          with x30 already restored to the caller's return address. The hook
          must not `ret`; it preserves x0/x30, does mailbox work, restores
          them, and finishes with the same tail branch. Ping (kind 1) +
          equip_y (kind 2, "set the Y-button Echo/Automaton directly").
  entry   echoes_helper_entry -- the real EoW HeartGauge-HUD hook
          (main+0x2595d60), hooking a function's very FIRST instruction
          instead of a call/tail-branch site. The replaced instruction is an
          ordinary non-branching instruction (here `sub sp, sp, #0x40`) that is
          just the first half of the function's own prologue -- x0-x7 may all
          be live incoming arguments, x30 is THIS function's own (not yet
          saved) return address. The hook builds its OWN frame, preserves
          x0-x7/x30/every callee-saved register it borrows, does mailbox work,
          restores everything, undoes its own frame, THEN executes a VERBATIM
          COPY of the replaced instruction's exact bytes (spliced in after
          assembly, not re-typed -- see is_pc_relative() below), and finally
          branches to main+hook_address+4 (the instruction right after the one
          replaced). Same ping/equip_y protocol as tail-b, duplicated rather
          than shared -- see HELPER-TOOLCHAIN.md section 2b.
Every routine is always assembled (hook.S is one file); --hook-kind only picks
which one's offset becomes the patch's branch target, which opcode the patch
writes at the hook site, and (for "entry" only) where the verbatim-replay
splice lands.

--hook-address/--original-bytes describe the hook site, which this script does
NOT discover by itself: a separate RE pass identifies the real EoW hook address
and the instruction it replaces, then this script cross-checks the claim before
building anything:
  - bl/tail-b: --original-bytes must decode as that exact branch kind, and its
    decoded target must match --original-call-target (required for these two
    kinds).
  - entry: --original-bytes must decode as NON-PC-relative (is_pc_relative()
    below) -- otherwise copying it verbatim to the helper's own code address
    would silently compute something different than the original instruction
    did. --original-call-target is NOT required for "entry" (the resume
    address is architecturally fixed at hook_address + 4); if given anyway, it
    must equal hook_address + 4 or the build refuses.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TITLE_ID = "01008CF01BAAC000"
# Verified 2026-10-23 directly from this research tree's own NSO fixture:
#   fixtures/package-v196608/main
# bytes [0x40:0x60] (the NSO0 header's build_id field) == this value exactly,
# confirming that fixture is the same build this task names. Read-only inspection
# of a fixed-offset binary header, not reverse engineering.
BUILD_ID_HEX = "9F221E1AFA65BA5443E955158AB97DA871E5E41F" + "0" * 24
assert len(BUILD_ID_HEX) == 64
# Same fixture, same header: segment file-offset/location/size triplets at header
# offsets 0x10/0x20/0x30 (location at +4, size at +8, matching the NSO0 spec and the
# exact field extraction BOTW's tools/build-ui-hook.py uses), and bss_size at the
# fixed absolute offset 0x3C (60) that the NSO0 format packs into the data segment
# header's fourth word.
DEFAULT_LAYOUT = {
    "segments": [
        {"location": 0x0, "size": 0x2836550},  # .text
        {"location": 0x2837000, "size": 0x83AC74},  # .rodata
        {"location": 0x3072000, "size": 0x40C6F0},  # .data
    ],
    "bss_size": 0x54910,
}

# main-relative addresses for the equip_y guard chain (echoes_helper's and
# echoes_helper_entry's eh_/en_handle_equip_y). These come from the
# owner-approved task spec's disassembly (main+0x24d9150 -> the bl at
# main+0x24d924c, the native ActorSelect release guard chain); this script
# takes them as given -- it does not perform or re-verify the underlying RE
# itself. See HELPER-TOOLCHAIN.md section 5a.
ACTOR_SELECT_ROOT_PTR = 0x34BC840  # P = *(u64*)(main + this): ActorSelect root ptr
EQUIP_Y_NATIVE_CALL = 0x2228A60  # call target(x0=Q+0x1940, w1=id) that commits the Y assign

# main-relative addresses for the HUD Y-icon refresh that runs on the equip_y
# SUCCESS path only, after EQUIP_Y_NATIVE_CALL and before the P+0x10 lock
# release (eh_equip_release/en_equip_release). Byte-verified against native
# main+0x24d9250..0x24d931c (identical copy at main+0x24d57a0) during this
# task -- not re-derived independently beyond that verification. See
# HELPER-TOOLCHAIN.md section 5a's HUD-refresh writeup.
HUD_REFRESH_FLAG_PTR = 0x34C8F60  # ldarb this: bit0==0 skips the whole refresh
HUD_MESSAGE_BUILDER = 0x1591110  # bl(x0=&msg[0x30 zeroed bytes]) -- fills msg
HUD_HANDLER_UPDATE = 0x25A5FF4  # bl(x0=handler, w1=id) when the cached field differs
HUD_RELEASE_FINALIZE = 0x2833E50  # bl(x0=listener) -- always follows vtable[0x10] when freeing
HUD_MSG_EXTRA_CALL = 0x2834260  # bl(x0=msg.u64[0x8]) when msg.byte[0x10] != 0

# main-relative address for kind 3 (ACCESSORY_SET, ENTRY HOOK ONLY -- see
# en_handle_accessory in hook.S). Called with x0 = Q+0x1850 only when a slot
# actually changed, while the P+0x10 lock is still held (matches native's own
# revoke path at main+0x21bf9a0..0x21bf9dc). Live-verified on the desktop by
# diffing memory around native pause-menu actions, not independently
# re-derived by this script.
ACCESSORY_EFFECT_REBUILD = 0x2229900

# main-relative addresses for kind 6 (WARP, ENTRY HOOK ONLY -- see
# en_handle_warp in hook.S). Ground-truthed byte-exact against the owner's
# own disassembly trace of the Map menu's confirm code and
# main+0x163f560/main+0x25b8394's release pattern -- this script did not
# independently re-derive any of these addresses or offsets.
WARP_MGR_PTR = 0x34BB8D8  # mgr = *(main + this); mgr == 0 -> refuse
WARP_NAME_CTOR = 0x15C3A90  # bl(out=&entryNameRef, work, &{ptr,len})
WARP_CONTAINER_CTOR = 0x28025F0  # bl(out=&containerHandle, capacityHint=2)
WARP_CONTAINER_SET = 0x2802F50  # bl(container, nameStr, typeTag, &{ptr,len}) -- level(5)/locator(7)
WARP_FLOW_START = 0x163F560  # bl(&result, mgr, &entryNameRef, containerPtr)
WARP_LEVEL_STR = 0x2878C34  # rodata "level\0"
WARP_LOCATOR_STR = 0x2895817  # rodata "locator\0"

# Correction (post-review): main+0x15c3a90's second argument is NOT scratch --
# it is a pointer to a 16-byte {obj,ctrl} "holder" built from a loaded MapWarp
# flow resource. These five additional constants build that holder (steps
# A-D in en_handle_warp); taken as given from the owner's own reading of the
# menu's full function (text.bin + rodata), not independently re-derived.
WARP_PATH_STR = 0x288C304  # rodata "event/MapWarp.bfevfl\0" (len 0x14)
WARP_RESOURCE_MGR_PTR = 0x34D23B0  # a DIFFERENT manager pointer than WARP_MGR_PTR
WARP_RESOURCE_LOAD = 0x2736940  # bl(&res, resourceMgr, &pathview)
WARP_RESOURCE_PREPARE = 0x38A680  # bl(res.obj) -- only if res.obj != 0, no return checked
WARP_HOLDER_CTOR = 0x65EE00  # bl(&holder, &res)

RELOCATION_KINDS = {
    "R_AARCH64_CALL26": "aarch64_branch26",
    "R_AARCH64_JUMP26": "aarch64_branch26",
    "R_AARCH64_ADR_PREL_PG_HI21": "aarch64_adrp",
    "R_AARCH64_ADD_ABS_LO12_NC": "aarch64_add_lo12",
    "R_AARCH64_LDST64_ABS_LO12_NC": "aarch64_ldr64_lo12",
    "R_AARCH64_LDST32_ABS_LO12_NC": "aarch64_ldr32_lo12",
}

# Which guest_code entry symbol (see hook.S) becomes the patch's branch target,
# per --hook-kind. Looked up with llvm-nm rather than hardcoded, because only
# echoes_dsmod_ping_hook is first (offset 0); the others are not.
ENTRY_SYMBOL = {
    "bl": "echoes_dsmod_ping_hook",
    "tail-b": "echoes_helper",
    "entry": "echoes_helper_entry",
}

# The opcode the patch itself writes at --hook-address. For "bl" it MUST be a
# `bl` (the patch instruction itself sets x30, exactly like the original did).
# For "tail-b" AND "entry" it MUST be a plain `b` (Branch26 -> Code): "tail-b"
# because the patch must not touch x30 at all (the original tail branch
# didn't either); "entry" because the patch simply redirects the function's
# first instruction to the helper, which is also just an ordinary
# non-link-setting branch. Using the wrong opcode here would corrupt x30 (or,
# for entry, needlessly set it) before the helper even starts running.
PATCH_OPCODE = {"bl": 0x94000000, "tail-b": 0x14000000, "entry": 0x14000000}

# Verified directly against `llvm-readelf -r`'s real output, grouped by
# routine offset range via llvm-nm (not hand-derived -- incremental arithmetic
# for this count has proved error-prone across several additions, so it is
# always re-checked against the tool after every change, not trusted from a
# running total). Current breakdown: 3 from echoes_dsmod_ping_hook, 14 from
# echoes_helper, 44 from echoes_helper_entry (kinds 1/2/HUD-refresh/3/4/5 plus
# kind 6/WARP's corrected object-construction sequence: 5 new fixed symbols
# for the resource/holder chain -- warp_path_str and warp_resource_mgr_ptr
# (adrp+add each), warp_resource_load, warp_resource_prepare,
# warp_holder_ctor -- plus 8 additional `bl hud_release_finalize` call sites,
# one per release_once() block in the now-unified container/name/holder/res
# cleanup, duplicated once for the refuse exit and once for the ok exit).
# 3 + 14 + 44 = 61, regardless of which --hook-kind is requested, since
# hook.S is one file and every routine is always assembled together.
EXPECTED_RELOCATIONS = 61


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--hook-address",
        required=True,
        type=lambda s: int(s, 0),
        help="main-relative NSO offset of the instruction being replaced (required; "
        "find this by RE, this script does not discover it)",
    )
    p.add_argument(
        "--original-bytes",
        required=True,
        help="8 hex chars: the 4 bytes currently at --hook-address (little-endian "
        "instruction word), used as the patch's 'expected' field and, for "
        "--hook-kind entry, also spliced verbatim into the helper's own replay slot",
    )
    p.add_argument(
        "--original-call-target",
        default=None,
        type=lambda s: int(s, 0),
        help="main-relative address the replaced instruction branches to. "
        "REQUIRED for --hook-kind bl/tail-b: the replaced instruction must decode "
        "as exactly that kind of branch to this address. NOT required for "
        "--hook-kind entry (the resume address is architecturally hook_address+4, "
        "computed automatically); if given anyway for entry, it must equal that "
        "computed value or the build refuses.",
    )
    p.add_argument(
        "--hook-kind",
        choices=["bl", "tail-b", "entry"],
        default="bl",
        help="instruction kind at --hook-address. 'bl' (default, keeps the "
        "ping-skeleton builds unchanged): a BOTW-style call-site hook -- "
        "echoes_dsmod_ping_hook replays the call then rets. 'tail-b': an "
        "unconditional tail branch with x30 already restored to the caller; "
        "echoes_helper preserves x0/x30, does mailbox work, restores them, then "
        "tail-branches onward. 'entry': a function's own first instruction "
        "(non-branching, non-PC-relative only); echoes_helper_entry preserves "
        "x0-x7/x30, does mailbox work, restores them, replays the instruction "
        "verbatim, then branches to hook_address+4. See hook.S for all three.",
    )
    p.add_argument("--title-id", default=TITLE_ID)
    p.add_argument("--build-id", default=BUILD_ID_HEX)
    p.add_argument(
        "--layout-json",
        type=Path,
        default=None,
        help="path to a JSON file with {'segments': [...], 'bss_size': N} overriding "
        "the default layout read from this tree's own EoW NSO fixture",
    )
    p.add_argument("--mailbox-size", type=lambda s: int(s, 0), default=4096)
    p.add_argument("--output", type=Path, default=ROOT / "runs" / "ping-hook")
    return p.parse_args()


def decode_branch_target(word: int, pc: int, kind: str) -> int | None:
    """Decodes an AArch64 unconditional-branch-immediate word (`bl` for
    kind=='bl', plain `b` for kind=='tail-b') and returns its absolute target, or
    None if `word` is not that exact kind of branch. Both `b` and `bl` share the
    same imm26-at-[25:0], PC-relative-words encoding; only bits[31:26] (the
    opcode) differ (`b`=0b000101, `bl`=0b100101)."""
    if word & 0xFC000000 != PATCH_OPCODE[kind]:
        return None
    imm = word & 0x3FFFFFF
    if imm & (1 << 25):
        imm -= 1 << 26
    return pc + imm * 4


def is_pc_relative(word: int) -> bool:
    """True if `word` is an AArch64 instruction whose meaning depends on its own
    address (so copying its exact bytes to a new address -- the --hook-kind entry
    verbatim-replay strategy -- would silently change what it does). Covers every
    PC-relative-addressing class in the AArch64 base ISA: B/BL, B.cond, CBZ/CBNZ,
    TBZ/TBNZ, ADR/ADRP, and the literal-addressing loads (LDR/LDRSW/PRFM
    (literal), including the SIMD&FP literal-load encodings). Verified by
    assembling one example of each class plus `sub sp, sp, #0x40` with clang and
    checking this function's verdict against every one of them -- see the task
    history for the exact check. NOT exhaustive for every conceivable "this
    instruction's semantics depend on context" case (e.g. `blr`/`br`/`ret` are
    classified False -- they ARE position-independent bytes -- but are still
    control-flow instructions a human should look twice at before trusting this
    generic verbatim-replay strategy for; see HELPER-TOOLCHAIN.md section 2b)."""
    if word & 0xFC000000 in (0x14000000, 0x94000000):  # B / BL
        return True
    if word & 0xFF000010 == 0x54000000:  # B.cond
        return True
    if word & 0x7E000000 == 0x34000000:  # CBZ / CBNZ
        return True
    if word & 0x7E000000 == 0x36000000:  # TBZ / TBNZ
        return True
    if word & 0x1F000000 == 0x10000000:  # ADR / ADRP
        return True
    if word & 0x3B000000 == 0x18000000:  # LDR/LDRSW/PRFM (literal), incl. SIMD&FP
        return True
    return False


def find_symbol_offset(obj: Path, name: str) -> int:
    """Looks up a defined symbol's byte offset into .text via llvm-nm, the same
    tool HELPER-TOOLCHAIN.md section 4 already documents using for this purpose."""
    nm_text = subprocess.check_output(
        ["llvm-nm", "--defined-only", "--format=posix", str(obj)], text=True
    )
    for line in nm_text.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == name:
            return int(parts[2], 16)
    raise SystemExit(
        f"symbol '{name}' not found in {obj} -- hook.S changed without updating "
        f"ENTRY_SYMBOL, or the build picked up stale object code. llvm-nm output:\n{nm_text}"
    )


def main() -> None:
    args = parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)

    original_bytes = bytes.fromhex(args.original_bytes)
    assert len(original_bytes) == 4, "--original-bytes must be exactly 4 bytes (one AArch64 word)"
    original_word = struct.unpack("<I", original_bytes)[0]

    if args.hook_kind == "entry":
        if is_pc_relative(original_word):
            raise SystemExit(
                f"--original-bytes {args.original_bytes} (word={original_word:#010x}) is "
                "PC-relative (a branch, ADR/ADRP, or a literal-pool load) -- copying it "
                "verbatim to echoes_helper_entry's own code address would compute a "
                "DIFFERENT result than it did at --hook-address. --hook-kind entry only "
                "supports replacing a non-PC-relative first instruction; see "
                "HELPER-TOOLCHAIN.md section 2b."
            )
        computed_target = args.hook_address + 4
        if args.original_call_target is not None and args.original_call_target != computed_target:
            raise SystemExit(
                f"--hook-kind entry's resume address is architecturally "
                f"--hook-address + 4 ({computed_target:#x}); --original-call-target "
                f"{args.original_call_target:#x} does not match. Omit the flag (it is not "
                "required for entry) or pass the correct value."
            )
        original_call_target = computed_target
    else:
        if args.original_call_target is None:
            raise SystemExit(f"--original-call-target is required for --hook-kind {args.hook_kind}")
        original_call_target = args.original_call_target
        decoded_target = decode_branch_target(original_word, args.hook_address, args.hook_kind)
        kind_name = "bl" if args.hook_kind == "bl" else "unconditional b"
        if decoded_target is None:
            raise SystemExit(
                f"--original-bytes {args.original_bytes} is not a `{kind_name}` instruction "
                f"(word={original_word:#010x}) for --hook-kind {args.hook_kind}. This builder "
                "only knows how to replay a replaced `bl` (hook-kind bl), a replaced "
                "unconditional tail `b` (hook-kind tail-b), or a replaced non-PC-relative "
                "first instruction (hook-kind entry); a different instruction kind "
                "(conditional branch, cbz/tbz, indirect blr/br) needs a different replay "
                "strategy in hook.S and a new --hook-kind value before this script can be "
                "extended to it -- see HELPER-TOOLCHAIN.md section 2a/2b."
            )
        if decoded_target != original_call_target:
            raise SystemExit(
                f"--original-bytes decodes to a `{kind_name}` targeting {decoded_target:#x}, "
                f"which does not match --original-call-target {original_call_target:#x}. "
                "Refusing to build a helper that would call/branch to the wrong function."
            )

    obj = out / "hook.o"
    binary = out / "hook.bin"
    subprocess.run(
        ["clang", "--target=aarch64-none-elf", "-c", str(ROOT / "hook.S"), "-o", str(obj)],
        check=True,
    )
    subprocess.run(
        ["llvm-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(binary)],
        check=True,
    )
    code = bytearray(binary.read_bytes())
    assert code and len(code) % 4 == 0, "assembled .text must be a whole number of instructions"
    assert len(code) <= 65536, "guest code exceeds MaxGuestCodeBytes (64 KiB, mod_load_plan.cpp)"

    entry_symbol = ENTRY_SYMBOL[args.hook_kind]
    entry_offset = find_symbol_offset(obj, entry_symbol)
    assert entry_offset % 4 == 0 and entry_offset < len(code)

    if args.hook_kind == "entry":
        # Splice --original-bytes verbatim into the placeholder word
        # echoes_helper_entry reserves for it (hook.S: entry_replay_slot). This is
        # a direct byte overwrite, not a relocation -- is_pc_relative() above is
        # what makes this safe (the bytes mean the same thing at any address).
        replay_offset = find_symbol_offset(obj, "entry_replay_slot")
        assert replay_offset % 4 == 0 and replay_offset + 4 <= len(code)
        code[replay_offset : replay_offset + 4] = original_bytes
    code = bytes(code)

    symbols = {
        "bridge_mailbox": 0,
        "original_call_target": original_call_target if args.hook_kind != "entry" else 0,
        "actor_select_root": ACTOR_SELECT_ROOT_PTR,
        "equip_y_native_call": EQUIP_Y_NATIVE_CALL,
        "hud_refresh_flag": HUD_REFRESH_FLAG_PTR,
        "hud_message_builder": HUD_MESSAGE_BUILDER,
        "hud_handler_update": HUD_HANDLER_UPDATE,
        "hud_release_finalize": HUD_RELEASE_FINALIZE,
        "hud_msg_extra_call": HUD_MSG_EXTRA_CALL,
        "accessory_effect_rebuild": ACCESSORY_EFFECT_REBUILD,
        "warp_mgr_ptr": WARP_MGR_PTR,
        "warp_name_ctor": WARP_NAME_CTOR,
        "warp_container_ctor": WARP_CONTAINER_CTOR,
        "warp_container_set": WARP_CONTAINER_SET,
        "warp_flow_start": WARP_FLOW_START,
        "warp_level_str": WARP_LEVEL_STR,
        "warp_locator_str": WARP_LOCATOR_STR,
        "warp_path_str": WARP_PATH_STR,
        "warp_resource_mgr_ptr": WARP_RESOURCE_MGR_PTR,
        "warp_resource_load": WARP_RESOURCE_LOAD,
        "warp_resource_prepare": WARP_RESOURCE_PREPARE,
        "warp_holder_ctor": WARP_HOLDER_CTOR,
        # Always hook_address + 4, independent of --hook-kind, so echoes_helper_entry's
        # own resume branch resolves correctly even when it is not the build's active
        # entry point (dead code in that build, same as every other routine's shared
        # symbols -- see HELPER-TOOLCHAIN.md section 2a's note on this).
        "entry_resume_target": args.hook_address + 4,
    }
    reloc_text = subprocess.check_output(["llvm-readelf", "-r", str(obj)], text=True)
    (out / "relocations.txt").write_text(reloc_text)
    relocations = []
    for line in reloc_text.splitlines():
        match = re.fullmatch(
            r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(R_AARCH64_\S+)\s+[0-9a-fA-F]+\s+(\S+)\s+([+-])\s+([0-9a-fA-F]+)",
            line.strip(),
        )
        if not match:
            assert not line.strip().startswith("0000"), f"unrecognised relocation line: {line}"
            continue
        off_text, kind, symbol, sign, addend_text = match.groups()
        assert kind in RELOCATION_KINDS, f"unhandled relocation kind {kind}"
        assert symbol in symbols, (
            f"undefined symbol '{symbol}' has no address -- add it to `symbols` above "
            "(a real request kind's own game-structure symbols go here, exactly like "
            "BOTW's build-allocated-ui-hook.py --holding/--warp/etc. symbol blocks)"
        )
        at = int(off_text, 16)
        assert at % 4 == 0 and at + 4 <= len(code)
        addend = int(addend_text, 16) * (1 if sign == "+" else -1)
        assert addend >= 0
        relocations.append(
            {
                "offset": at,
                "kind": RELOCATION_KINDS[kind],
                "target": "mailbox" if symbol == "bridge_mailbox" else "main",
                "addend": symbols[symbol] + addend,
            }
        )
    print("Relocation count:", len(relocations))
    # hook.S always assembles ALL THREE guest routines (echoes_dsmod_ping_hook,
    # echoes_helper, echoes_helper_entry) regardless of --hook-kind -- see
    # EXPECTED_RELOCATIONS above for the 3+6+6 breakdown. A real request kind
    # that adds its own new structure/vtable symbol grows this count, the same
    # way BOTW's level count grows with each --holding/--warp/etc. flag
    # (build-allocated-ui-hook.py line 71).
    assert len(relocations) == EXPECTED_RELOCATIONS, (
        f"expected {EXPECTED_RELOCATIONS} relocations across all three guest routines, "
        f"found {len(relocations)} -- hook.S changed without updating this "
        "assertion, or the build picked up stale object code"
    )

    if args.layout_json:
        layout = json.loads(args.layout_json.read_text())
    else:
        layout = DEFAULT_LAYOUT

    mailbox_epoch_offset = args.mailbox_size - 4
    patch_replacement = struct.pack("<I", PATCH_OPCODE[args.hook_kind])
    # The branch26 relocation below fills in the real displacement from the hook
    # site to wherever the guest-code page lands (ModLoadPlan::Apply,
    # RelocationTarget::Code), landing exactly on `entry_symbol`'s offset within
    # that page -- 0 for echoes_dsmod_ping_hook (hook-kind bl, always first in
    # hook.S), entry_offset for echoes_helper/echoes_helper_entry.
    plan = {
        "format": 2,
        "title_id": args.title_id,
        "build_id": args.build_id,
        "module": "main",
        "layout": layout,
        "mailbox_size": args.mailbox_size,
        "mailbox_epoch_offset": mailbox_epoch_offset,
        "patches": [
            {
                "offset": args.hook_address,
                "expected": original_bytes.hex(),
                "replacement": patch_replacement.hex(),
                "relocations": [
                    {
                        "offset": 0,
                        "kind": "aarch64_branch26",
                        "target": "code",
                        "addend": entry_offset,
                    }
                ],
            }
        ],
        "guest_code": {"bytes": code.hex(), "relocations": relocations},
    }
    path = out / "plan.json"
    path.write_text(json.dumps(plan, indent=2) + "\n")
    notes = {
        "bl": "toolchain proof -- NOT installed, NOT packaged",
        "tail-b": "real EoW hook (main+0x24ff4d0, tail-b) -- NOT installed, NOT packaged, "
        "native ActorSelect guard-chain addresses taken from the owner-approved task "
        "spec, not independently re-verified by this script",
        "entry": "real EoW hook (main+0x2595d60, entry) -- NOT installed, NOT packaged, "
        "verbatim-replay splice + native ActorSelect guard-chain addresses taken from "
        "the owner-approved task spec, not independently re-verified by this script",
    }
    print(
        json.dumps(
            {
                "plan": str(path),
                "code_bytes": len(code),
                "relocations": len(relocations),
                "hook_kind": args.hook_kind,
                "entry_symbol": entry_symbol,
                "entry_offset": hex(entry_offset),
                "hook_address": hex(args.hook_address),
                "original_call_target": hex(original_call_target),
                "note": notes[args.hook_kind],
            }
        )
    )


if __name__ == "__main__":
    main()
