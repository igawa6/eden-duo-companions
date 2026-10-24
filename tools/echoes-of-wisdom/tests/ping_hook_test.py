#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline AArch64 execution test for the EoW ping guest helper.

Runs the *exact relocated bytes* this tree's build_echoes_helper.py produced (not a
re-derivation, not a mock) under Unicorn, the same approach BOTW's
prototype/tests/holding-hook-test.py uses (and the same `relocated_bytes` relocation
math as BOTW's tools/_archived-20261019-unused/live_load_plan.py, reimplemented here
standalone so this tree has no read dependency on BOTW's research tree at test time).

Requires the `unicorn` Python package. The system interpreter on this host does not
have it; BOTW's prototype/tests already depend on a local venv that does
(runs/guard-venv under the BOTW research tree) -- this script is read-only with
respect to that venv (it is only ever invoked as an interpreter, never written to)
and does not otherwise touch any BOTW file. Run it with:

    python3 build_echoes_helper.py --hook-address 0x1000000 \\
        --original-bytes 00004094 --original-call-target 0x2000000
    $UNICORN_VENV/bin/python \\
        tests/ping_hook_test.py
"""
from __future__ import annotations

import json
import struct
import subprocess
from pathlib import Path

from unicorn import UC_ARCH_ARM64, UC_HOOK_CODE, UC_MODE_ARM, Uc
from unicorn.arm64_const import (
    UC_ARM64_REG_LR,
    UC_ARM64_REG_SP,
    UC_ARM64_REG_X0,
    UC_ARM64_REG_X1,
    UC_ARM64_REG_X21,
    UC_ARM64_REG_X22,
    UC_ARM64_REG_X29,
)

ROOT = Path(__file__).resolve().parents[1]
PLAN_PATH = ROOT / "runs" / "ping-hook" / "plan.json"


def relocated_bytes(payload: dict, base: int, main: int, mailbox: int, code_base: int | None = None) -> bytes:
    """Reimplements Core::Mods::ModLoadPlan::Apply's relocation math
    (the Eden Duo runtime's src/core/mods/mod_load_plan.cpp) in
    Python, for offline byte-for-byte verification. Same four relocation kinds this
    plan uses; aarch64_ldr32_lo12/aarch64_ldr64_lo12 are omitted because the ping
    skeleton has none, exactly like BOTW's live_load_plan.py keeps the full set."""
    expected = bytearray.fromhex(payload["bytes"])
    for r in payload["relocations"]:
        at = r["offset"]
        pc = base + at
        target = {"main": main, "code": base if code_base is None else code_base, "mailbox": mailbox}[
            r["target"]
        ] + r.get("addend", 0)
        (word,) = struct.unpack_from("<I", expected, at)
        kind = r["kind"]
        if kind == "aarch64_branch26":
            delta = target - pc
            assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), (r, delta)
            word = (word & 0xFC000000) | ((delta // 4) & 0x3FFFFFF)
        elif kind == "aarch64_adrp":
            pages = ((target & ~4095) - (pc & ~4095)) // 4096
            assert -(1 << 20) <= pages < (1 << 20)
            bits = pages & 0x1FFFFF
            word = (word & 0x9F00001F) | ((bits & 3) << 29) | ((bits >> 2) << 5)
        elif kind == "aarch64_add_lo12":
            word = (word & ~(4095 << 10)) | ((target & 4095) << 10)
        else:
            raise AssertionError(f"unhandled relocation kind in this test: {kind}")
        struct.pack_into("<I", expected, at, word)
    return bytes(expected)


def main() -> None:
    if not PLAN_PATH.exists():
        raise SystemExit(
            f"{PLAN_PATH} does not exist -- run build_echoes_helper.py first (see this "
            "file's module docstring for the exact command)"
        )
    plan = json.loads(PLAN_PATH.read_text())
    assert plan["format"] == 2
    guest_code = plan["guest_code"]
    patch = plan["patches"][0]

    # guest_code's "main"-target branch26 relocations are: echoes_dsmod_ping_hook's
    # `bl original_call_target` (used by this test), echoes_helper's own
    # `b original_call_target` tail branch, AND echoes_helper's
    # `bl equip_y_native_call` -- hook.S always assembles BOTH guest routines
    # together (HELPER-TOOLCHAIN.md section 2a). The first two share the SAME
    # build-time value (symbols["original_call_target"] in build_echoes_helper.py);
    # equip_y_native_call has its own fixed address (0x2228a60, EQUIP_Y_NATIVE_CALL
    # in build_echoes_helper.py) that never changes with --original-call-target, so
    # it is excluded here by that known, fixed addend rather than assumed away.
    # equip_y_native_call is now referenced TWICE (echoes_helper +
    # echoes_helper_entry, both with the SAME fixed addend 0x2228a60), and
    # echoes_helper_entry also adds its own entry_resume_target (referenced
    # ONCE, build-specific addend = that build's hook_address + 4) -- so a
    # simple count==2 filter is no longer unambiguous by itself. Exclude the
    # known-fixed equip_y_native_call value first, then the remaining addend
    # shared by >=2 sites is original_call_target (entry_resume_target's lone
    # reference never reaches count 2).
    from collections import Counter
    # Every branch26+main relocation whose addend is a FIXED constant
    # (doesn't vary with --original-call-target) must be excluded before
    # looking for "the" shared original_call_target addend: equip_y_native_call
    # (2 refs: echoes_helper + echoes_helper_entry), and the four HUD-refresh
    # targets added for the Y-icon refresh (hud_message_builder,
    # hud_handler_update, hud_msg_extra_call: 2 refs each; hud_release_finalize:
    # 4 refs, called twice per routine) -- see HELPER-TOOLCHAIN.md section 5a.
    # 0x2802F50 (warp_container_set) is called TWICE within en_handle_warp
    # itself (the level and locator Set() sites), so it also needs excluding
    # -- same reasoning as the other fixed constants below.
    FIXED_MAIN_BRANCH_TARGETS = {0x2228A60, 0x1591110, 0x25A5FF4, 0x2833E50, 0x2834260, 0x2802F50}
    candidates = [
        r for r in guest_code["relocations"]
        if r["target"] == "main" and r["kind"] == "aarch64_branch26"
        and r["addend"] not in FIXED_MAIN_BRANCH_TARGETS
    ]
    counts = Counter(r["addend"] for r in candidates)
    shared = [addend for addend, c in counts.items() if c >= 2]
    assert len(shared) == 1, f"expected exactly one addend shared by >=2 routines (original_call_target), found {shared}"
    original_call_target = shared[0]

    # MAIN is symbolic (0): every "main"-relative value in the plan (the patch
    # offset, original_call_target) is already a main-relative NSO offset, so using
    # 0 as the module's base lets those offsets double as absolute test addresses
    # directly. CODE/MAILBOX/the replayed call target must stay within +-128 MiB of
    # each other and of the patch site for the two `bl`s (hook-site -> CODE, CODE ->
    # the replayed call target) to be AArch64-branch26-encodable -- exactly as the
    # real loader's layout keeps the guest-code page inside the same module image
    # (nso.cpp places it right after the mailbox, which sits right after the
    # module). The real addresses, once the other agent finds the hook site, already
    # satisfy this by construction; these are placeholders only to exercise the
    # encoder/decoder end to end.
    MAIN = 0
    CALL_TARGET_ABS = MAIN + original_call_target
    CODE = 0x0500_0000
    MAILBOX = 0x0510_0000
    STACK = 0x0600_0000
    END = 0x0700_0000  # LR sentinel: the instruction right after the patched site

    code_bytes = relocated_bytes(guest_code, CODE, MAIN, MAILBOX)
    assert len(code_bytes) == len(bytes.fromhex(guest_code["bytes"]))

    # Sanity: replaying the patch's own relocation against the SAME base/main/mailbox
    # must reproduce a `bl` from the patch site straight into CODE. This is the one
    # instruction the runtime writes directly into the game's own image.
    patch_bytes = relocated_bytes(
        {"bytes": patch["replacement"], "relocations": patch["relocations"]},
        MAIN + patch["offset"],
        MAIN,
        MAILBOX,
        code_base=CODE,
    )
    (patch_word,) = struct.unpack("<I", patch_bytes)
    assert patch_word & 0xFC000000 == 0x94000000
    patch_delta = patch_word & 0x3FFFFFF
    if patch_delta & (1 << 25):
        patch_delta -= 1 << 26
    assert MAIN + patch["offset"] + patch_delta * 4 == CODE

    RET = bytes.fromhex("c0035fd6")  # `ret`, little-endian

    passed = 0

    def run(pending: bool, matching_permit: bool, matching_session: bool, expect_status: int):
        nonlocal passed
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for base in (CALL_TARGET_ABS, CODE, MAILBOX, STACK, END):
            uc.mem_map(base & ~0xFFF, 0x1000)
        uc.mem_write(CODE, code_bytes)
        uc.mem_write(CALL_TARGET_ABS, RET)
        uc.mem_write(MAILBOX, bytes(0x1000))

        def put(offset: int, value: int, size: int = 4) -> None:
            uc.mem_write(MAILBOX + offset, (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little"))

        def get(offset: int, size: int = 4) -> int:
            return int.from_bytes(uc.mem_read(MAILBOX + offset, size), "little")

        session_epoch = 7
        put(4092, session_epoch)  # host-owned session epoch, last 4 bytes of the mailbox
        put(0x18, session_epoch if matching_session else session_epoch + 1)  # module's stamped copy
        if pending:
            put(0x08, 55)  # request_epoch: "there is a request"
            put(0x0C, 0)  # ack_epoch: not yet acked (!= 55)
            put(0x1C, 55 if matching_permit else 0)  # scope_permit
        else:
            put(0x08, 0)
            put(0x0C, 0)
            put(0x1C, 0)

        uc.reg_write(UC_ARM64_REG_SP, STACK + 0x800)
        uc.reg_write(UC_ARM64_REG_LR, END)
        saved = {UC_ARM64_REG_X21: 0xA21, UC_ARM64_REG_X22: 0xA22, UC_ARM64_REG_X29: 0xA29}
        for reg, value in saved.items():
            uc.reg_write(reg, value)

        def hook(u, address, size, _data):
            if address == CALL_TARGET_ABS:
                u.reg_write(UC_ARM64_REG_X0, 0x11112222)
                u.reg_write(UC_ARM64_REG_X1, 0x33334444)
            elif address == END:
                u.emu_stop()

        uc.hook_add(UC_HOOK_CODE, hook)
        uc.emu_start(CODE, END, count=2000)

        assert get(0x00) == 0x31574F45, f"magic not written: {get(0x00):#x}"
        assert get(0x04) == 1, f"heartbeat not incremented: {get(0x04)}"
        assert uc.reg_read(UC_ARM64_REG_X0) == 0x11112222, "x0 (bl return value) not preserved"
        assert uc.reg_read(UC_ARM64_REG_X1) == 0x33334444, "x1 (bl return value) not preserved"
        for reg, value in saved.items():
            assert uc.reg_read(reg) == value, f"callee-saved register {reg} clobbered"

        if not pending:
            assert get(0x0C) == 0, "ack_epoch must stay untouched with nothing pending"
            assert get(0x10) == 0, "status must stay untouched with nothing pending"
            assert get(0x14) == 0, "ping_counter must stay untouched with nothing pending"
        else:
            assert get(0x0C) == 55, "ack_epoch must equal the request epoch once handled"
            assert get(0x10) == expect_status
            if expect_status == 1:
                assert get(0x14) == 1, "ping_counter must increment on a successful ping"
            else:
                assert get(0x14) == 0, "ping_counter must NOT increment on a refused ping"
        passed += 1

    run(pending=False, matching_permit=True, matching_session=True, expect_status=0)
    run(pending=True, matching_permit=True, matching_session=True, expect_status=1)
    run(pending=True, matching_permit=False, matching_session=True, expect_status=2)
    run(pending=True, matching_permit=True, matching_session=False, expect_status=2)

    print(json.dumps({"passed": passed, "code_bytes": len(code_bytes), "original_call_target": hex(original_call_target)}))


if __name__ == "__main__":
    main()
