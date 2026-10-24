#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline AArch64 execution test for the EoW entry-hook-only request kinds 3
(ACCESSORY_SET), 4 (OUTFIT_SET), and 5 (CONSUME) -- echoes_helper_entry,
format-2 --hook-kind entry. echoes_helper (tail-b) never gained these kinds;
only this entry routine did (owner's explicit scope for this follow-up).

All three share equip_y's EXACT P/Q acquire-guard-chain and lock-release
(en_pq_acquire/en_pq_release in hook.S -- new local subroutines, factored out
so this follow-up did not need to quadruplicate that ~15-instruction sequence
a third and fourth time, and so echoes_helper_entry's existing, already-tested
en_handle_equip_y/en_equip_release stayed completely untouched). All three
were ground-truthed live on the desktop by diffing memory around native
pause-menu actions, not derived from a pre-made disassembly dump the way
equip_y's own guard chain and the HUD-refresh block were -- see
HELPER-TOOLCHAIN.md section 5c.

Scenarios:
  ACCESSORY_SET (kind 3): equip into the first empty slot; already-equipped
    (status 3); full (refuse 18); not acquired (refuse 17); multi-bit and
    zero target (refuse 16); P == 0 (refuse 10, sanity-checks the dispatch
    wiring for this new kind); Q+0x17 bit6 locked (refuse 13); unequip
    compaction using the EXACT native example from the task
    ([2,0x40,0x1000,0x4000,0x8000000] -> unequip 0x2 ->
    [0x40,0x1000,0x4000,0x8000000,0]); unequip-not-equipped (status 3);
    main+0x2229900 (the effect rebuild) called exactly once with
    x0 = Q+0x1850 on every actual change, and never on any refuse/already path.
  OUTFIT_SET (kind 4): write + "model dirty" bit set; same outfit (status 3);
    not owned (refuse 19); Q+0x17 bit6 locked (refuse 13).
  CONSUME (kind 5): decrement + heal; heal clamped to max HP; count already 0
    (refuse 22, with NO HP change); quantity index > 15 (refuse 20); heal
    amount out of sanity range (refuse 21); Q+0x17 bit6 locked (refuse 13).
  Every scenario: the shared P+0x10 lock round-trips back to its exact
  pre-request value (no leak, no double-release).

BOTW's own caveat applies here too: these are checked STUBS, not game proof.

Requires the `unicorn` Python package; run with the same read-only interpreter
the other test files use:

    python3 build_echoes_helper.py --hook-address 0x2595d60 \\
        --original-bytes ff0301d1 --hook-kind entry \\
        --output runs/equip-y-entry
    $UNICORN_VENV/bin/python tests/kind345_entry_hook_test.py
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

from unicorn import UC_ARCH_ARM64, UC_HOOK_CODE, UC_MODE_ARM, Uc
from unicorn.arm64_const import (
    UC_ARM64_REG_LR,
    UC_ARM64_REG_PC,
    UC_ARM64_REG_SP,
    UC_ARM64_REG_X0,
    UC_ARM64_REG_X1,
    UC_ARM64_REG_X2,
    UC_ARM64_REG_X3,
    UC_ARM64_REG_X4,
    UC_ARM64_REG_X5,
    UC_ARM64_REG_X6,
    UC_ARM64_REG_X7,
    UC_ARM64_REG_X25,
    UC_ARM64_REG_X26,
    UC_ARM64_REG_X29,
)

ROOT = Path(__file__).resolve().parents[1]
PLAN_PATH = ROOT / "runs" / "equip-y-entry" / "plan.json"

OFF_REQUEST_EPOCH = 0x008
OFF_ACK_EPOCH = 0x00C
OFF_STATUS = 0x010
OFF_SESSION_EPOCH_COPY = 0x018
OFF_SCOPE_PERMIT = 0x01C
OFF_REQUEST_KIND = 0x020
OFF_TARGET = 0x024
OFF_RESULT_CODE = 0x028
OFF_ARG = 0x02C
OFF_HOST_SESSION_EPOCH = 4092

KIND_ACCESSORY = 3
KIND_OUTFIT = 4
KIND_CONSUME = 5

STATUS_OK = 1
STATUS_REFUSED = 2
STATUS_ALREADY = 3

REFUSE_NO_ACTOR_SELECT_ROOT = 10
REFUSE_LOCKED_FLAG_SET = 13
REFUSE_ACCESSORY_INVALID_TARGET = 16
REFUSE_ACCESSORY_NOT_ACQUIRED = 17
REFUSE_ACCESSORY_SLOTS_FULL = 18
REFUSE_OUTFIT_NOT_OWNED = 19
REFUSE_CONSUME_INVALID_INDEX = 20
REFUSE_CONSUME_INVALID_HEAL_AMOUNT = 21
REFUSE_CONSUME_NONE_LEFT = 22


def relocated_bytes(payload: dict, base: int, main: int, mailbox: int, code_base: int | None = None) -> bytes:
    """Reimplements Core::Mods::ModLoadPlan::Apply's relocation math, standalone
    (see tests/ping_hook_test.py's copy of the same function)."""
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
            f"{PLAN_PATH} does not exist -- run build_echoes_helper.py with "
            "--hook-kind entry first (see this file's module docstring for the "
            "exact command)"
        )
    plan = json.loads(PLAN_PATH.read_text())
    assert plan["format"] == 2
    guest_code = plan["guest_code"]
    patch = plan["patches"][0]

    # entry_resume_target is architecturally patch["offset"] + 4 -- computed
    # directly rather than hunting for "the lone main-target branch26
    # relocation with count == 1", which needed a growing exclusion list for
    # every new fixed-constant symbol kinds 3/4/5/6 added. See
    # tests/equip_y_entry_hook_test.py's matching comment.
    expected_entry_resume_target = patch["offset"] + 4
    branch26_main = [r for r in guest_code["relocations"] if r["target"] == "main" and r["kind"] == "aarch64_branch26"]
    matches = [r for r in branch26_main if r["addend"] == expected_entry_resume_target]
    assert len(matches) == 1, (
        f"expected exactly one main-target branch26 relocation equal to patch_offset+4 "
        f"({expected_entry_resume_target:#x}), found {len(matches)}"
    )
    entry_resume_target = matches[0]["addend"]
    assert entry_resume_target == 0x2595D64

    entry_offset = patch["relocations"][0]["addend"]
    assert entry_offset > 0, "echoes_helper_entry must not be the first routine in hook.S"

    MAIN = 0
    RESUME_ABS = MAIN + entry_resume_target
    CODE = 0x0500_0000
    MAILBOX = 0x0510_0000
    STACK = 0x0600_0000
    ACTOR_SELECT_ROOT_ABS = MAIN + 0x34BC840
    GUARD_FN_ADDR = 0x4020_0000
    NOTIFY_FN_ADDR = 0x4040_0000
    FINALIZE_FN_ADDR = 0x4050_0000
    P_ADDR = 0x4000_0000
    VTABLE_ADDR = 0x4010_0000
    Q_ADDR = 0x4030_0000
    H_ADDR = 0x40A0_0000                  # outfit kind: h = *(Q+0x1850)
    ACCESSORY_EFFECT_REBUILD_ABS = MAIN + 0x2229900

    code_bytes = relocated_bytes(guest_code, CODE, MAIN, MAILBOX)
    assert len(code_bytes) == len(bytes.fromhex(guest_code["bytes"]))
    START = CODE + entry_offset

    patch_bytes = relocated_bytes(
        {"bytes": patch["replacement"], "relocations": patch["relocations"]},
        MAIN + patch["offset"],
        MAIN,
        MAILBOX,
        code_base=CODE,
    )
    (patch_word,) = struct.unpack("<I", patch_bytes)
    assert patch_word & 0xFC000000 == 0x14000000, f"patch must be a plain `b`, got {patch_word:#010x}"
    patch_delta = patch_word & 0x3FFFFFF
    if patch_delta & (1 << 25):
        patch_delta -= 1 << 26
    assert MAIN + patch["offset"] + patch_delta * 4 == START

    RET = bytes.fromhex("c0035fd6")  # `ret`, little-endian

    passed = 0

    def run(
        *,
        kind: int,
        target: int,
        arg: int = 0,
        p_value: int | None = P_ADDR,
        guard_passes: bool = True,
        q_value: int | None = Q_ADDR,
        locked: bool = False,
        initial_lock: int = 0x1000000,
        accessory_acquired_mask: int = 0xFFFFFFFF,
        accessory_flags: int = 0x01FFFFFF,     # -> capacity 5, byte-verified
        accessory_slots: tuple = (0, 0, 0, 0, 0),
        outfit_current: int = 0x0001,
        outfit_owned: int = 0xFFFF,
        outfit_h_w14: int = 0,
        consume_counts: tuple = tuple([5] * 16),
        consume_hp: int = 40,
        consume_max: int = 99,
        expect_status: int,
        expect_result_code: int = 0,
        expect_accessory_slots_after: tuple | None = None,
        expect_rebuild_called: bool | None = None,
        expect_rebuild_arg: int | None = None,
        expect_outfit_current_after: int | None = None,
        expect_h_w14_after: int | None = None,
        expect_consume_count_after: int | None = None,
        expect_hp_after: int | None = None,
        label: str,
    ):
        nonlocal passed
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for base, size in (
            (CODE, 0x1000),
            (MAILBOX, 0x1000),
            (STACK, 0x1000),
            (RESUME_ABS & ~0xFFF, 0x1000),
            (ACTOR_SELECT_ROOT_ABS & ~0xFFF, 0x1000),
            (GUARD_FN_ADDR, 0x1000),
            (NOTIFY_FN_ADDR, 0x1000),
            (FINALIZE_FN_ADDR, 0x1000),
            (P_ADDR, 0x1000),
            (VTABLE_ADDR, 0x1000),
            (Q_ADDR, 0x2000),
            (H_ADDR, 0x1000),
            (ACCESSORY_EFFECT_REBUILD_ABS & ~0xFFF, 0x1000),
        ):
            uc.mem_map(base, size)
        uc.mem_write(CODE, code_bytes)
        uc.mem_write(RESUME_ABS, RET)
        uc.mem_write(GUARD_FN_ADDR, RET)
        uc.mem_write(NOTIFY_FN_ADDR, RET)
        uc.mem_write(FINALIZE_FN_ADDR, RET)
        uc.mem_write(ACCESSORY_EFFECT_REBUILD_ABS, RET)
        uc.mem_write(MAILBOX, bytes(0x1000))
        uc.mem_write(P_ADDR, bytes(0x1000))
        uc.mem_write(VTABLE_ADDR, bytes(0x1000))
        uc.mem_write(Q_ADDR, bytes(0x2000))
        uc.mem_write(H_ADDR, bytes(0x1000))

        uc.mem_write(ACTOR_SELECT_ROOT_ABS, struct.pack("<Q", p_value or 0))
        if p_value is not None:
            uc.mem_write(p_value, struct.pack("<Q", VTABLE_ADDR))
            uc.mem_write(p_value + 8, struct.pack("<Q", q_value or 0))
            uc.mem_write(VTABLE_ADDR + 0x40, struct.pack("<Q", GUARD_FN_ADDR))
            uc.mem_write(VTABLE_ADDR + 0x68, struct.pack("<Q", NOTIFY_FN_ADDR))
            uc.mem_write(VTABLE_ADDR + 0x8, struct.pack("<Q", FINALIZE_FN_ADDR))
        if q_value is not None:
            uc.mem_write(q_value + 0x17, bytes([0x40 if locked else 0x00]))
            # accessory_set
            uc.mem_write(q_value + 0x1874, struct.pack("<I", accessory_flags))
            uc.mem_write(q_value + 0x187c, struct.pack("<I", accessory_acquired_mask))
            for i, slot in enumerate(accessory_slots):
                uc.mem_write(q_value + 0x1880 + 4 * i, struct.pack("<I", slot & 0xFFFFFFFF))
            # outfit_set
            uc.mem_write(q_value + 0x1878, struct.pack("<H", outfit_owned))
            uc.mem_write(q_value + 0x187a, struct.pack("<H", outfit_current))
            uc.mem_write(q_value + 0x1850, struct.pack("<Q", H_ADDR))
            uc.mem_write(H_ADDR + 0x14, struct.pack("<I", outfit_h_w14))
            # consume
            for i, c in enumerate(consume_counts):
                uc.mem_write(q_value + 0x185E + i, bytes([c & 0xFF]))
            uc.mem_write(q_value + 0x17C8, struct.pack("<i", consume_max))
            uc.mem_write(q_value + 0x17CC, struct.pack("<i", consume_hp))

        uc.mem_write(P_ADDR + 0x10, struct.pack("<Q", initial_lock))

        def put(offset: int, value: int, size: int = 4) -> None:
            uc.mem_write(MAILBOX + offset, (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little"))

        def get(offset: int, size: int = 4) -> int:
            return int.from_bytes(uc.mem_read(MAILBOX + offset, size), "little")

        session_epoch = 7
        put(OFF_HOST_SESSION_EPOCH, session_epoch)
        put(OFF_SESSION_EPOCH_COPY, session_epoch)
        put(OFF_REQUEST_KIND, kind)
        put(OFF_TARGET, target)
        put(OFF_ARG, arg & 0xFFFFFFFF)
        put(OFF_REQUEST_EPOCH, 55)
        put(OFF_ACK_EPOCH, 0)
        put(OFF_SCOPE_PERMIT, 55)

        ENTRY_SP = STACK + 0x800
        ENTRY_LR = 0x0888_0000_0000_0666
        ENTRY_X29 = 0x0777_0000_0000_0555
        arg_regs = {
            UC_ARM64_REG_X0: 0xA000_0000_0000_0000,
            UC_ARM64_REG_X1: 0xA000_0000_0000_0001,
            UC_ARM64_REG_X2: 0xA000_0000_0000_0002,
            UC_ARM64_REG_X3: 0xA000_0000_0000_0003,
            UC_ARM64_REG_X4: 0xA000_0000_0000_0004,
            UC_ARM64_REG_X5: 0xA000_0000_0000_0005,
            UC_ARM64_REG_X6: 0xA000_0000_0000_0006,
            UC_ARM64_REG_X7: 0xA000_0000_0000_0007,
        }
        uc.reg_write(UC_ARM64_REG_SP, ENTRY_SP)
        uc.reg_write(UC_ARM64_REG_LR, ENTRY_LR)
        uc.reg_write(UC_ARM64_REG_X29, ENTRY_X29)
        for reg, value in arg_regs.items():
            uc.reg_write(reg, value)
        callee_saved = {UC_ARM64_REG_X25: 0xA25, UC_ARM64_REG_X26: 0xA26}
        for reg, value in callee_saved.items():
            uc.reg_write(reg, value)

        guard_called = False
        rebuild_called = False
        rebuild_arg = None

        def hook(u, address, size, _data):
            nonlocal guard_called, rebuild_called, rebuild_arg
            if address == GUARD_FN_ADDR:
                guard_called = True
                if guard_passes:
                    lock = int.from_bytes(u.mem_read(P_ADDR + 0x10, 8), "little")
                    u.mem_write(P_ADDR + 0x10, struct.pack("<Q", lock + 0x1000000))
                u.reg_write(UC_ARM64_REG_X0, 1 if guard_passes else 0)
            elif address == ACCESSORY_EFFECT_REBUILD_ABS:
                rebuild_called = True
                rebuild_arg = u.reg_read(UC_ARM64_REG_X0)

        uc.hook_add(UC_HOOK_CODE, hook)
        uc.emu_start(START, RESUME_ABS, count=5000)
        landed_at_target = uc.reg_read(UC_ARM64_REG_PC) == RESUME_ABS

        assert landed_at_target, (
            f"[{label}] never reached main+0x2595d64, stopped at "
            f"{uc.reg_read(UC_ARM64_REG_PC):#x} instead"
        )
        assert uc.reg_read(UC_ARM64_REG_SP) == ENTRY_SP - 0x40, f"[{label}] sp not entry_sp - 0x40"
        for reg, value in arg_regs.items():
            assert uc.reg_read(reg) == value, f"[{label}] argument register {reg} not preserved"
        assert uc.reg_read(UC_ARM64_REG_LR) == ENTRY_LR, f"[{label}] x30 not preserved"
        assert uc.reg_read(UC_ARM64_REG_X29) == ENTRY_X29, f"[{label}] x29 clobbered"
        for reg, value in callee_saved.items():
            assert uc.reg_read(reg) == value, f"[{label}] callee-saved register {reg} clobbered"

        assert get(OFF_ACK_EPOCH) == 55, f"[{label}] ack_epoch must echo the request epoch"
        assert get(OFF_STATUS) == expect_status, f"[{label}] status {get(OFF_STATUS)} != {expect_status}"
        assert get(OFF_RESULT_CODE) == expect_result_code, (
            f"[{label}] result_code {get(OFF_RESULT_CODE)} != {expect_result_code}"
        )

        lock_after = int.from_bytes(uc.mem_read(P_ADDR + 0x10, 8), "little")
        assert lock_after == initial_lock, (
            f"[{label}] lock word at P+0x10 = {lock_after:#x}, expected {initial_lock:#x} (leak/double-release)"
        )

        if expect_accessory_slots_after is not None and q_value is not None:
            slots_after = tuple(
                struct.unpack_from("<I", uc.mem_read(q_value + 0x1880 + 4 * i, 4))[0] for i in range(5)
            )
            assert slots_after == tuple(x & 0xFFFFFFFF for x in expect_accessory_slots_after), (
                f"[{label}] accessory slots after = {slots_after}, expected {expect_accessory_slots_after}"
            )
        if expect_rebuild_called is not None:
            assert rebuild_called == expect_rebuild_called, f"[{label}] rebuild_called={rebuild_called}"
            if expect_rebuild_called and expect_rebuild_arg is not None:
                assert rebuild_arg == expect_rebuild_arg, (
                    f"[{label}] rebuild_arg={rebuild_arg:#x} != {expect_rebuild_arg:#x}"
                )
        if expect_outfit_current_after is not None and q_value is not None:
            current_after = struct.unpack_from("<H", uc.mem_read(q_value + 0x187A, 2))[0]
            assert current_after == expect_outfit_current_after, (
                f"[{label}] outfit current after = {current_after:#x} != {expect_outfit_current_after:#x}"
            )
        if expect_h_w14_after is not None:
            w14_after = struct.unpack_from("<I", uc.mem_read(H_ADDR + 0x14, 4))[0]
            assert w14_after == expect_h_w14_after, f"[{label}] h+0x14 after = {w14_after:#x} != {expect_h_w14_after:#x}"
        if expect_consume_count_after is not None and q_value is not None:
            count_after = uc.mem_read(q_value + 0x185E + target, 1)[0]
            assert count_after == expect_consume_count_after, (
                f"[{label}] consume count after = {count_after} != {expect_consume_count_after}"
            )
        if expect_hp_after is not None and q_value is not None:
            hp_after = struct.unpack_from("<i", uc.mem_read(q_value + 0x17CC, 4))[0]
            assert hp_after == expect_hp_after, f"[{label}] hp after = {hp_after} != {expect_hp_after}"

        passed += 1
        print(f"ok: {label}")

    # --- ACCESSORY_SET (kind 3) -----------------------------------------
    run(kind=KIND_ACCESSORY, target=0x2, arg=1,
        accessory_slots=(0x40, 0, 0x4000, 0x8000000, 0),
        expect_status=STATUS_OK, expect_rebuild_called=True,
        expect_rebuild_arg=Q_ADDR + 0x1850,
        expect_accessory_slots_after=(0x40, 0x2, 0x4000, 0x8000000, 0),
        label="accessory: equip into first empty slot")

    run(kind=KIND_ACCESSORY, target=0x2, arg=1,
        accessory_slots=(0x40, 0x2, 0x4000, 0x8000000, 0),
        expect_status=STATUS_ALREADY, expect_rebuild_called=False,
        expect_accessory_slots_after=(0x40, 0x2, 0x4000, 0x8000000, 0),
        label="accessory: already equipped")

    run(kind=KIND_ACCESSORY, target=0x2, arg=1,
        accessory_slots=(0x40, 0x1000, 0x4000, 0x8000000, 0x10),
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_ACCESSORY_SLOTS_FULL,
        expect_rebuild_called=False,
        expect_accessory_slots_after=(0x40, 0x1000, 0x4000, 0x8000000, 0x10),
        label="accessory: full -> refuse")

    run(kind=KIND_ACCESSORY, target=0x2, arg=1, accessory_acquired_mask=0xFFFFFFFD,  # bit1 clear
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_ACCESSORY_NOT_ACQUIRED,
        expect_rebuild_called=False,
        label="accessory: not acquired -> refuse")

    run(kind=KIND_ACCESSORY, target=0x3, arg=1,  # two bits set
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_ACCESSORY_INVALID_TARGET,
        expect_rebuild_called=False,
        label="accessory: multi-bit target -> refuse")

    run(kind=KIND_ACCESSORY, target=0x0, arg=1,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_ACCESSORY_INVALID_TARGET,
        expect_rebuild_called=False,
        label="accessory: zero target -> refuse")

    run(kind=KIND_ACCESSORY, target=0x2, arg=1, p_value=None,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_NO_ACTOR_SELECT_ROOT,
        expect_rebuild_called=False,
        label="accessory: P == 0 -> refuse (dispatch wiring sanity check)")

    run(kind=KIND_ACCESSORY, target=0x2, arg=1, locked=True,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_LOCKED_FLAG_SET,
        expect_rebuild_called=False,
        label="accessory: Q+0x17 bit6 locked -> refuse")

    # Exact native example: [2,0x40,0x1000,0x4000,0x8000000], unequip 0x2.
    run(kind=KIND_ACCESSORY, target=0x2, arg=0,
        accessory_slots=(0x2, 0x40, 0x1000, 0x4000, 0x8000000),
        expect_status=STATUS_OK, expect_rebuild_called=True,
        expect_rebuild_arg=Q_ADDR + 0x1850,
        expect_accessory_slots_after=(0x40, 0x1000, 0x4000, 0x8000000, 0),
        label="accessory: unequip compaction (exact native example)")

    run(kind=KIND_ACCESSORY, target=0x2, arg=0,
        accessory_slots=(0x40, 0x1000, 0x4000, 0x8000000, 0),
        expect_status=STATUS_ALREADY, expect_rebuild_called=False,
        expect_accessory_slots_after=(0x40, 0x1000, 0x4000, 0x8000000, 0),
        label="accessory: unequip not equipped -> already")

    # Equip 0x2 again after the compaction above -> lands in the now-empty last slot.
    run(kind=KIND_ACCESSORY, target=0x2, arg=1,
        accessory_slots=(0x40, 0x1000, 0x4000, 0x8000000, 0),
        expect_status=STATUS_OK, expect_rebuild_called=True,
        expect_accessory_slots_after=(0x40, 0x1000, 0x4000, 0x8000000, 0x2),
        label="accessory: re-equip after compaction lands in the freed slot")

    # --- OUTFIT_SET (kind 4) ---------------------------------------------
    run(kind=KIND_OUTFIT, target=0x0004, outfit_current=0x0001, outfit_owned=0xFFFF,
        outfit_h_w14=0x0,
        expect_status=STATUS_OK, expect_outfit_current_after=0x0004,
        expect_h_w14_after=0x100,
        label="outfit: write + model-dirty bit set")

    run(kind=KIND_OUTFIT, target=0x0004, outfit_current=0x0004, outfit_owned=0xFFFF,
        outfit_h_w14=0x0,
        expect_status=STATUS_ALREADY, expect_outfit_current_after=0x0004,
        expect_h_w14_after=0x0,
        label="outfit: same outfit -> already")

    run(kind=KIND_OUTFIT, target=0x0008, outfit_current=0x0001, outfit_owned=0x0007,  # bit3 not owned
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_OUTFIT_NOT_OWNED,
        expect_outfit_current_after=0x0001,
        label="outfit: not owned -> refuse")

    run(kind=KIND_OUTFIT, target=0x0004, locked=True,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_LOCKED_FLAG_SET,
        label="outfit: Q+0x17 bit6 locked -> refuse")

    # --- CONSUME (kind 5) -------------------------------------------------
    run(kind=KIND_CONSUME, target=3, arg=1, consume_counts=tuple([5] * 16), consume_hp=40, consume_max=99,
        expect_status=STATUS_OK,
        expect_consume_count_after=4, expect_hp_after=41,
        label="consume: decrement + heal (Bubble Kelp: HP 40->41, count 15->14 shape)")

    run(kind=KIND_CONSUME, target=0, arg=50, consume_counts=tuple([2] + [5] * 15), consume_hp=90, consume_max=99,
        expect_status=STATUS_OK,
        expect_consume_count_after=1, expect_hp_after=99,
        label="consume: heal clamped to max HP")

    counts_zero = tuple([0] + [5] * 15)
    run(kind=KIND_CONSUME, target=0, arg=10, consume_counts=counts_zero, consume_hp=40, consume_max=99,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_CONSUME_NONE_LEFT,
        expect_consume_count_after=0, expect_hp_after=40,
        label="consume: count already 0 -> refuse, no HP change")

    run(kind=KIND_CONSUME, target=16, arg=10,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_CONSUME_INVALID_INDEX,
        label="consume: quantity index > 15 -> refuse")

    run(kind=KIND_CONSUME, target=0, arg=-1,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_CONSUME_INVALID_HEAL_AMOUNT,
        label="consume: negative heal amount -> refuse")

    run(kind=KIND_CONSUME, target=0, arg=0x1001,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_CONSUME_INVALID_HEAL_AMOUNT,
        label="consume: heal amount over sanity cap -> refuse")

    run(kind=KIND_CONSUME, target=0, arg=1, locked=True,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_LOCKED_FLAG_SET,
        label="consume: Q+0x17 bit6 locked -> refuse")

    print(json.dumps({"passed": passed, "code_bytes": len(code_bytes), "entry_offset": hex(entry_offset),
                       "entry_resume_target": hex(entry_resume_target)}))


if __name__ == "__main__":
    main()
