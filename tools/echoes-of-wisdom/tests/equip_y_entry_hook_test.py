#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline AArch64 execution test for the EoW HeartGauge-HUD hook
(echoes_helper_entry, format-2 --hook-kind entry) and equip_y through that
third hook kind.

Unlike echoes_dsmod_ping_hook (replays a replaced `bl`, then `ret`s) and
echoes_helper (preserves x0/x30, tail-branches onward), echoes_helper_entry
hooks a function's very FIRST instruction -- an ordinary non-branching,
non-PC-relative instruction (`sub sp, sp, #0x40` at main+0x2595d60) that is
just the first half of the function's own prologue. See HELPER-TOOLCHAIN.md
section 2b for the full replay-strategy writeup this test exercises:

  1. entry passthrough -- nothing pending: x0-x7/x30/callee-saved
     byte-identical to entry, SP ends up EXACTLY `entry_sp - 0x40` (the real
     effect of the verbatim-replayed instruction, applied only after this
     routine's own 128-byte frame has been fully unwound), landing at
     main+0x2595d64 (hook_address + 4).
  2. equip_y success/refuse through the entry path -- the exact same guard
     chain as tests/equip_y_hook_test.py's tail-b scenarios, duplicated here
     because echoes_helper_entry is a deliberate additive duplicate of
     echoes_helper's dispatch body (HELPER-TOOLCHAIN.md section 2b).
  3. lock release (en_equip_release) -- same invariant as
     tests/equip_y_hook_test.py's scenario 7: every path past the acquire
     releases the shared lock at P+0x10 exactly once (no leak, no
     double-release), refuse 10/11 leave it untouched, and one "last reader"
     scenario (lock starts at 0) makes both the notify (vtable+0x68) and
     finalize (vtable+0x8) calls fire.
  4. HUD Y-icon refresh (en_refresh_hud_y_icon) -- same invariant as
     tests/equip_y_hook_test.py's scenario 8 (see that file's docstring for
     the full writeup, including the handler==0 discrepancy against this
     task's own re-verified raw disassembly).

Requires the `unicorn` Python package; run with the same read-only interpreter
the other two test files use:

    python3 build_echoes_helper.py --hook-address 0x2595d60 \\
        --original-bytes ff0301d1 --hook-kind entry \\
        --output runs/equip-y-entry
    $UNICORN_VENV/bin/python \\
        tests/equip_y_entry_hook_test.py
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

OFF_MAGIC = 0x000
OFF_HEARTBEAT = 0x004
OFF_REQUEST_EPOCH = 0x008
OFF_ACK_EPOCH = 0x00C
OFF_STATUS = 0x010
OFF_PING_COUNTER = 0x014
OFF_SESSION_EPOCH_COPY = 0x018
OFF_SCOPE_PERMIT = 0x01C
OFF_REQUEST_KIND = 0x020
OFF_EQUIP_Y_TARGET_ID = 0x024
OFF_RESULT_CODE = 0x028
OFF_HOST_SESSION_EPOCH = 4092

KIND_PING = 1
KIND_EQUIP_Y = 2

STATUS_IDLE = 0
STATUS_OK = 1
STATUS_REFUSED = 2
STATUS_ALREADY = 3

REFUSE_NO_ACTOR_SELECT_ROOT = 10
REFUSE_RELEASE_GUARD_FAILED = 11
REFUSE_NO_SECONDARY_POINTER = 12
REFUSE_LOCKED_FLAG_SET = 13
REFUSE_NEVER_CLEAR_Y = 14
REFUSE_NATIVE_ASSIGN_FAILED = 15


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

    # entry_resume_target is architecturally patch["offset"] + 4 (the
    # instruction right after the replaced one -- build_echoes_helper.py
    # computes it the same way). Computing it directly here, rather than
    # hunting for "the one main-target branch26 relocation with count == 1",
    # is deliberately robust against every new fixed-constant symbol kinds
    # 3/4/5/6 have added over time (each one of those needed its own
    # exclusion in the old approach; this needs none).
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
    RESUME_ABS = MAIN + entry_resume_target  # 0x2595d64
    CODE = 0x0500_0000
    MAILBOX = 0x0510_0000
    STACK = 0x0600_0000
    ACTOR_SELECT_ROOT_ABS = MAIN + 0x34BC840
    EQUIP_Y_NATIVE_FN_ABS = MAIN + 0x2228A60
    P_ADDR = 0x4000_0000
    VTABLE_ADDR = 0x4010_0000
    GUARD_FN_ADDR = 0x4020_0000
    Q_ADDR = 0x4030_0000
    NOTIFY_FN_ADDR = 0x4040_0000    # P->vtable[0x68]: en_equip_release's "notify"
    FINALIZE_FN_ADDR = 0x4050_0000  # P->vtable[0x8]: en_equip_release's "finalize"

    # HUD Y-icon refresh (en_refresh_hud_y_icon), success-path-only stubs.
    HUD_REFRESH_FLAG_ABS = MAIN + 0x34C8F60
    HUD_MESSAGE_BUILDER_ABS = MAIN + 0x1591110
    HUD_HANDLER_UPDATE_ABS = MAIN + 0x25A5FF4
    HUD_RELEASE_FINALIZE_ABS = MAIN + 0x2833E50
    HUD_MSG_EXTRA_CALL_ABS = MAIN + 0x2834260
    HANDLER_ADDR = 0x4060_0000
    LISTENER_ADDR = 0x4070_0000
    LISTENER_VTABLE_ADDR = 0x4080_0000
    LISTENER_VTABLE_FN_ADDR = 0x4090_0000  # listener's vtable[0x10]: the "free" call
    MSG_EXTRA_ARG = 0x4A00_0000_0000_0001  # arbitrary sentinel, msg.u64[0x8]

    code_bytes = relocated_bytes(guest_code, CODE, MAIN, MAILBOX)
    assert len(code_bytes) == len(bytes.fromhex(guest_code["bytes"]))
    START = CODE + entry_offset

    # Sanity: the patch's own relocation must reproduce a plain `b` (NOT `bl`)
    # from the hook site into CODE + entry_offset.
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

    # Confirm the verbatim-replay splice landed: the exact task bytes
    # ff0301d1 ("sub sp, sp, #0x40") must appear, UN-relocated, inside the
    # relocated code blob (this word has no relocation entry at all, by
    # design -- a direct byte splice, not a symbolic reference).
    assert bytes.fromhex("ff0301d1") in code_bytes, "verbatim-replay splice not found in guest_code bytes"

    RET = bytes.fromhex("c0035fd6")  # `ret`, little-endian

    passed = 0

    def run(
        *,
        pending: bool = True,
        kind: int = KIND_EQUIP_Y,
        matching_permit: bool = True,
        matching_session: bool = True,
        target_id: int = 0x1234,
        p_value: int | None = P_ADDR,
        guard_passes: bool = True,
        q_value: int | None = Q_ADDR,
        locked: bool = False,
        existing_target_id: int = 0x0001,
        native_call_writes_correct: bool = True,
        expect_status: int,
        expect_result_code: int = 0,
        expect_native_called: bool | None = None,
        expect_guard_called: bool | None = None,
        initial_lock: int = 0x1000000,  # "1 other reader" by default (BOTW/native convention)
        expect_notify_called: bool = False,
        expect_finalize_called: bool = False,
        refresh_flag_bit0: bool = True,
        refresh_handler: int | None = HANDLER_ADDR,  # None means handler == 0
        refresh_listener: int | None = LISTENER_ADDR,  # None means listener == 0
        refresh_handler_field: int = 0x9999,  # mismatches every target_id used below by default
        refresh_listener_initial_refcount: int = 1,
        refresh_msg_extra_flag: bool = False,
        expect_message_builder_called: bool | None = None,
        expect_handler_update_calls: list | None = None,
        expect_release_finalize_count: int | None = None,
        expect_msg_extra_called: bool | None = None,
        expect_listener_refcount_after: int | None = None,
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
            (EQUIP_Y_NATIVE_FN_ABS & ~0xFFF, 0x1000),
            (P_ADDR, 0x1000),
            (VTABLE_ADDR, 0x1000),
            (GUARD_FN_ADDR, 0x1000),
            (Q_ADDR, 0x2000),
            (NOTIFY_FN_ADDR, 0x1000),
            (FINALIZE_FN_ADDR, 0x1000),
            (HUD_REFRESH_FLAG_ABS & ~0xFFF, 0x1000),
            (HUD_MESSAGE_BUILDER_ABS & ~0xFFF, 0x1000),
            (HUD_HANDLER_UPDATE_ABS & ~0xFFF, 0x1000),
            (HUD_RELEASE_FINALIZE_ABS & ~0xFFF, 0x1000),
            (HUD_MSG_EXTRA_CALL_ABS & ~0xFFF, 0x1000),
            (HANDLER_ADDR, 0x1000),
            (LISTENER_ADDR, 0x1000),
            (LISTENER_VTABLE_ADDR, 0x1000),
            (LISTENER_VTABLE_FN_ADDR, 0x1000),
        ):
            uc.mem_map(base, size)
        uc.mem_write(CODE, code_bytes)
        uc.mem_write(RESUME_ABS, RET)
        uc.mem_write(GUARD_FN_ADDR, RET)
        uc.mem_write(EQUIP_Y_NATIVE_FN_ABS, RET)
        uc.mem_write(NOTIFY_FN_ADDR, RET)
        uc.mem_write(FINALIZE_FN_ADDR, RET)
        uc.mem_write(HUD_MESSAGE_BUILDER_ABS, RET)
        uc.mem_write(HUD_HANDLER_UPDATE_ABS, RET)
        uc.mem_write(HUD_RELEASE_FINALIZE_ABS, RET)
        uc.mem_write(HUD_MSG_EXTRA_CALL_ABS, RET)
        uc.mem_write(LISTENER_VTABLE_FN_ADDR, RET)
        uc.mem_write(MAILBOX, bytes(0x1000))
        uc.mem_write(P_ADDR, bytes(0x1000))
        uc.mem_write(VTABLE_ADDR, bytes(0x1000))
        uc.mem_write(Q_ADDR, bytes(0x2000))
        uc.mem_write(HANDLER_ADDR, bytes(0x1000))
        uc.mem_write(LISTENER_ADDR, bytes(0x1000))
        uc.mem_write(LISTENER_VTABLE_ADDR, bytes(0x1000))

        uc.mem_write(ACTOR_SELECT_ROOT_ABS, struct.pack("<Q", p_value or 0))
        if p_value is not None:
            uc.mem_write(p_value, struct.pack("<Q", VTABLE_ADDR))
            uc.mem_write(p_value + 8, struct.pack("<Q", q_value or 0))
            uc.mem_write(VTABLE_ADDR + 0x40, struct.pack("<Q", GUARD_FN_ADDR))
            uc.mem_write(VTABLE_ADDR + 0x68, struct.pack("<Q", NOTIFY_FN_ADDR))
            uc.mem_write(VTABLE_ADDR + 0x8, struct.pack("<Q", FINALIZE_FN_ADDR))
        if q_value is not None:
            uc.mem_write(q_value + 0x17, bytes([0x40 if locked else 0x00]))
            uc.mem_write(q_value + 0x1970, struct.pack("<H", existing_target_id))

        # Shared release-guard lock word at P+0x10: initialised BEFORE every
        # scenario regardless of p_value (see tests/equip_y_hook_test.py's
        # matching comment).
        uc.mem_write(P_ADDR + 0x10, struct.pack("<Q", initial_lock))

        uc.mem_write(HUD_REFRESH_FLAG_ABS, bytes([1 if refresh_flag_bit0 else 0]))
        uc.mem_write(LISTENER_ADDR, struct.pack("<Q", LISTENER_VTABLE_ADDR))
        uc.mem_write(LISTENER_ADDR + 8, struct.pack("<Q", refresh_listener_initial_refcount & 0xFFFFFFFFFFFFFFFF))
        uc.mem_write(LISTENER_VTABLE_ADDR + 0x10, struct.pack("<Q", LISTENER_VTABLE_FN_ADDR))
        uc.mem_write(HANDLER_ADDR + 0x800, struct.pack("<H", refresh_handler_field & 0xFFFF))

        def put(offset: int, value: int, size: int = 4) -> None:
            uc.mem_write(MAILBOX + offset, (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little"))

        def get(offset: int, size: int = 4) -> int:
            return int.from_bytes(uc.mem_read(MAILBOX + offset, size), "little")

        session_epoch = 7
        put(OFF_HOST_SESSION_EPOCH, session_epoch)
        put(OFF_SESSION_EPOCH_COPY, session_epoch if matching_session else session_epoch + 1)
        if pending:
            put(OFF_REQUEST_KIND, kind)
            put(OFF_EQUIP_Y_TARGET_ID, target_id)
            put(OFF_REQUEST_EPOCH, 55)
            put(OFF_ACK_EPOCH, 0)
            put(OFF_SCOPE_PERMIT, 55 if matching_permit else 0)
        else:
            put(OFF_REQUEST_EPOCH, 0)
            put(OFF_ACK_EPOCH, 0)
            put(OFF_SCOPE_PERMIT, 0)

        ENTRY_SP = STACK + 0x800
        ENTRY_LR = 0x0888_0000_0000_0666  # sentinel: THIS function's own (unsaved) return address
        ENTRY_X29 = 0x0777_0000_0000_0555  # sentinel: the CALLER's frame pointer, untouched throughout
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
        native_called = False
        notify_called = False
        finalize_called = False
        message_builder_called = False
        handler_update_calls = []
        release_finalize_calls = []
        listener_vtable_fn_calls = []
        msg_extra_calls = []

        def hook(u, address, size, _data):
            nonlocal guard_called, native_called, notify_called, finalize_called, message_builder_called
            if address == GUARD_FN_ADDR:
                guard_called = True
                if guard_passes:
                    lock = int.from_bytes(u.mem_read(P_ADDR + 0x10, 8), "little")
                    u.mem_write(P_ADDR + 0x10, struct.pack("<Q", lock + 0x1000000))
                u.reg_write(UC_ARM64_REG_X0, 1 if guard_passes else 0)
            elif address == EQUIP_Y_NATIVE_FN_ABS:
                native_called = True
                if native_call_writes_correct:
                    x0 = u.reg_read(UC_ARM64_REG_X0)  # Q + 0x1940
                    w1 = u.reg_read(UC_ARM64_REG_X1) & 0xFFFF  # id
                    u.mem_write(x0 + 0x30, struct.pack("<H", w1))  # writes Q+0x1970
            elif address == NOTIFY_FN_ADDR:
                notify_called = True
            elif address == FINALIZE_FN_ADDR:
                finalize_called = True
            elif address == HUD_MESSAGE_BUILDER_ABS:
                message_builder_called = True
                msg_ptr = u.reg_read(UC_ARM64_REG_X0)
                u.mem_write(msg_ptr + 0x18, struct.pack("<Q", refresh_handler or 0))
                u.mem_write(msg_ptr + 0x20, struct.pack("<Q", refresh_listener or 0))
                u.mem_write(msg_ptr + 0x10, bytes([1 if refresh_msg_extra_flag else 0]))
                u.mem_write(msg_ptr + 0x8, struct.pack("<Q", MSG_EXTRA_ARG))
            elif address == HUD_HANDLER_UPDATE_ABS:
                handler_update_calls.append((u.reg_read(UC_ARM64_REG_X0), u.reg_read(UC_ARM64_REG_X1) & 0xFFFF))
            elif address == HUD_RELEASE_FINALIZE_ABS:
                release_finalize_calls.append(u.reg_read(UC_ARM64_REG_X0))
            elif address == LISTENER_VTABLE_FN_ADDR:
                listener_vtable_fn_calls.append(u.reg_read(UC_ARM64_REG_X0))
            elif address == HUD_MSG_EXTRA_CALL_ABS:
                msg_extra_calls.append(u.reg_read(UC_ARM64_REG_X0))

        uc.hook_add(UC_HOOK_CODE, hook)
        # `until=RESUME_ABS` makes Unicorn stop exactly WHEN PC reaches that
        # address, without fetching/executing (or hooking) the instruction
        # there -- read back PC/SP after emu_start returns, same as
        # tests/equip_y_hook_test.py's tail-b scenarios.
        uc.emu_start(START, RESUME_ABS, count=5000)
        landed_at_target = uc.reg_read(UC_ARM64_REG_PC) == RESUME_ABS

        assert landed_at_target, (
            f"[{label}] never reached main+0x2595d64, stopped at "
            f"{uc.reg_read(UC_ARM64_REG_PC):#x} instead"
        )
        assert get(OFF_MAGIC) == 0x31574F45, f"[{label}] magic not written: {get(OFF_MAGIC):#x}"
        assert get(OFF_HEARTBEAT) == 1, f"[{label}] heartbeat not incremented"

        # The verbatim-replayed `sub sp, sp, #0x40` must have applied to the
        # ORIGINAL entry sp, not to this routine's own (already-unwound) frame.
        assert uc.reg_read(UC_ARM64_REG_SP) == ENTRY_SP - 0x40, (
            f"[{label}] sp = {uc.reg_read(UC_ARM64_REG_SP):#x}, expected "
            f"{ENTRY_SP - 0x40:#x} (entry_sp - 0x40)"
        )
        for reg, value in arg_regs.items():
            assert uc.reg_read(reg) == value, f"[{label}] argument register {reg} not preserved"
        assert uc.reg_read(UC_ARM64_REG_LR) == ENTRY_LR, f"[{label}] x30 not preserved"
        assert uc.reg_read(UC_ARM64_REG_X29) == ENTRY_X29, f"[{label}] x29 (caller's frame pointer) clobbered"
        for reg, value in callee_saved.items():
            assert uc.reg_read(reg) == value, f"[{label}] callee-saved register {reg} clobbered"

        if not pending:
            assert get(OFF_ACK_EPOCH) == 0, f"[{label}] ack_epoch must stay 0 with nothing pending"
            assert get(OFF_STATUS) == STATUS_IDLE, f"[{label}] status must stay idle with nothing pending"
        else:
            assert get(OFF_ACK_EPOCH) == 55, f"[{label}] ack_epoch must echo the request epoch (55)"
            assert get(OFF_STATUS) == expect_status, (
                f"[{label}] status {get(OFF_STATUS)} != expected {expect_status}"
            )
            assert get(OFF_RESULT_CODE) == expect_result_code, (
                f"[{label}] result_code {get(OFF_RESULT_CODE)} != expected {expect_result_code}"
            )
            if kind == KIND_PING:
                if expect_status == STATUS_OK:
                    assert get(OFF_PING_COUNTER) == 1, f"[{label}] ping_counter must increment on success"
                else:
                    assert get(OFF_PING_COUNTER) == 0, f"[{label}] ping_counter must not increment on refusal"

        if expect_guard_called is not None:
            assert guard_called == expect_guard_called, f"[{label}] guard_called={guard_called}"
        if expect_native_called is not None:
            assert native_called == expect_native_called, f"[{label}] native_called={native_called}"

        lock_after = int.from_bytes(uc.mem_read(P_ADDR + 0x10, 8), "little")
        assert lock_after == initial_lock, (
            f"[{label}] lock word at P+0x10 = {lock_after:#x}, expected it back at "
            f"its pre-request value {initial_lock:#x} (leak or double-release)"
        )
        assert notify_called == expect_notify_called, f"[{label}] notify_called={notify_called}"
        assert finalize_called == expect_finalize_called, f"[{label}] finalize_called={finalize_called}"

        if expect_message_builder_called is not None:
            assert message_builder_called == expect_message_builder_called, (
                f"[{label}] message_builder_called={message_builder_called}"
            )
        if expect_handler_update_calls is not None:
            assert handler_update_calls == expect_handler_update_calls, (
                f"[{label}] handler_update_calls={handler_update_calls} != {expect_handler_update_calls}"
            )
        if expect_release_finalize_count is not None:
            assert len(release_finalize_calls) == expect_release_finalize_count, (
                f"[{label}] release_finalize_calls={release_finalize_calls} "
                f"(count {len(release_finalize_calls)} != {expect_release_finalize_count})"
            )
            assert len(listener_vtable_fn_calls) == expect_release_finalize_count, (
                f"[{label}] listener_vtable_fn_calls={listener_vtable_fn_calls} "
                f"(count {len(listener_vtable_fn_calls)} != {expect_release_finalize_count})"
            )
        if expect_listener_refcount_after is not None:
            listener_refcount_after = int.from_bytes(uc.mem_read(LISTENER_ADDR + 8, 8), "little")
            expected = expect_listener_refcount_after & 0xFFFFFFFFFFFFFFFF
            assert listener_refcount_after == expected, (
                f"[{label}] listener refcount after = {listener_refcount_after:#x}, "
                f"expected {expected:#x}"
            )
        if expect_msg_extra_called is not None:
            assert bool(msg_extra_calls) == expect_msg_extra_called, (
                f"[{label}] msg_extra_calls={msg_extra_calls}"
            )
            if expect_msg_extra_called:
                assert msg_extra_calls == [MSG_EXTRA_ARG], f"[{label}] msg_extra_calls={msg_extra_calls}"
        passed += 1
        print(f"ok: {label}")

    # 1. Entry passthrough: nothing pending, every argument/callee-saved
    #    register and x30/x29 untouched, SP ends up entry_sp - 0x40, lands
    #    exactly at main+0x2595d64.
    run(pending=False, expect_status=STATUS_IDLE, expect_guard_called=False,
        expect_native_called=False, label="entry passthrough")

    # 2. equip_y refuse paths through the entry hook.
    run(p_value=None, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_NO_ACTOR_SELECT_ROOT,
        expect_guard_called=False, expect_native_called=False, label="entry equip_y refuse 10 (P==0)")
    run(guard_passes=False, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_RELEASE_GUARD_FAILED,
        expect_guard_called=True, expect_native_called=False, label="entry equip_y refuse 11 (guard r&1==0)")
    run(q_value=None, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_NO_SECONDARY_POINTER,
        expect_guard_called=True, expect_native_called=False, label="entry equip_y refuse 12 (Q==0)")
    run(locked=True, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_LOCKED_FLAG_SET,
        expect_guard_called=True, expect_native_called=False, label="entry equip_y refuse 13 (locked flag)")
    run(target_id=0xFFFF, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_NEVER_CLEAR_Y,
        expect_guard_called=True, expect_native_called=False, label="entry equip_y refuse 14 (id==0xFFFF)")
    run(native_call_writes_correct=False, expect_status=STATUS_REFUSED,
        expect_result_code=REFUSE_NATIVE_ASSIGN_FAILED, expect_guard_called=True,
        expect_native_called=True, label="entry equip_y refuse 15 (native assign didn't take)")

    # 3. Already equipped.
    run(target_id=0x4242, existing_target_id=0x4242, expect_status=STATUS_ALREADY,
        expect_guard_called=True, expect_native_called=False, label="entry equip_y already equipped")

    # 4. Success.
    run(target_id=0x4242, existing_target_id=0x0001, expect_status=STATUS_OK,
        expect_guard_called=True, expect_native_called=True, label="entry equip_y success")

    # 5. ping still works through the entry hook too.
    run(kind=KIND_PING, expect_status=STATUS_OK, expect_guard_called=False,
        expect_native_called=False, label="ping via echoes_helper_entry (kind 1)")

    # 6. Stale session epoch / cancelled permit -- generic refusal before kind
    #    dispatch, SP still gets the verbatim-replay effect either way.
    run(matching_session=False, expect_status=STATUS_REFUSED, expect_result_code=0,
        expect_guard_called=False, expect_native_called=False, label="entry equip_y stale session epoch")
    run(matching_permit=False, expect_status=STATUS_REFUSED, expect_result_code=0,
        expect_guard_called=False, expect_native_called=False, label="entry equip_y cancelled permit")

    # 7. Last reader through the entry hook too.
    run(target_id=0x5555, existing_target_id=0x0001, initial_lock=0,
        expect_status=STATUS_OK, expect_guard_called=True, expect_native_called=True,
        expect_notify_called=True, expect_finalize_called=True,
        label="entry equip_y success as last reader (notify+finalize fire)")

    # 4. HUD Y-icon refresh through the entry hook too (see
    #    tests/equip_y_hook_test.py's matching item 8 for the full writeup).
    run(target_id=0x6001, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x9999, expect_message_builder_called=True,
        expect_handler_update_calls=[(HANDLER_ADDR, 0x6001)],
        expect_release_finalize_count=0, expect_listener_refcount_after=0,
        label="entry hud refresh: handler field != id -> hud_handler_update called")
    run(target_id=0x6002, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x6002, expect_message_builder_called=True,
        expect_handler_update_calls=[], expect_release_finalize_count=0,
        expect_listener_refcount_after=0,
        label="entry hud refresh: handler field == id -> hud_handler_update not called")
    # handler == 0, listener != 0 -> no compare/call, but the FINAL release
    # (outside the handler gate, msg's own reference) still runs once:
    # refcount N=1 -> N-1=0, no free (old=1 != 0). Reconciled against the
    # far-jump blocks the owner traced -- see HELPER-TOOLCHAIN.md section 5b.
    run(target_id=0x6003, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler=None, refresh_listener_initial_refcount=1,
        expect_message_builder_called=True, expect_handler_update_calls=[],
        expect_release_finalize_count=0, expect_listener_refcount_after=0,
        label="entry hud refresh: handler == 0, listener != 0 -> one release, no free")
    # handler != 0, listener == 0 -> hud_handler_update still called on
    # mismatch; nothing ever touches a refcount (every AddRef/release site is
    # gated on listener != 0).
    run(target_id=0x6009, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x9999, refresh_listener=None,
        expect_message_builder_called=True,
        expect_handler_update_calls=[(HANDLER_ADDR, 0x6009)],
        expect_release_finalize_count=0,
        label="entry hud refresh: handler != 0, listener == 0 -> update called, no refcount touch")
    run(target_id=0x6004, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x6004, refresh_listener_initial_refcount=1,
        expect_release_finalize_count=0, expect_listener_refcount_after=0,
        label="entry hud refresh: listener refcount N=1 -> nets to 0, no free")
    run(target_id=0x6005, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x6005, refresh_listener_initial_refcount=0,
        expect_release_finalize_count=1, expect_listener_refcount_after=0xFFFFFFFFFFFFFFFF,
        label="entry hud refresh: listener refcount N=0 -> free fires once")
    run(target_id=0x6006, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x6006, refresh_msg_extra_flag=True,
        expect_msg_extra_called=True,
        label="entry hud refresh: msg.byte[0x10] != 0 -> hud_msg_extra_call called")
    run(target_id=0x6007, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x6007, refresh_msg_extra_flag=False,
        expect_msg_extra_called=False,
        label="entry hud refresh: msg.byte[0x10] == 0 -> hud_msg_extra_call not called")
    run(target_id=0x6008, existing_target_id=0x0001, expect_status=STATUS_OK,
        expect_native_called=True, refresh_flag_bit0=False,
        expect_message_builder_called=False, expect_handler_update_calls=[],
        expect_release_finalize_count=0, expect_msg_extra_called=False,
        expect_listener_refcount_after=1,
        label="entry hud refresh: flag bit0 == 0 -> entire refresh skipped, setter still ran")

    print(json.dumps({"passed": passed, "code_bytes": len(code_bytes), "entry_offset": hex(entry_offset),
                       "entry_resume_target": hex(entry_resume_target)}))


if __name__ == "__main__":
    main()
