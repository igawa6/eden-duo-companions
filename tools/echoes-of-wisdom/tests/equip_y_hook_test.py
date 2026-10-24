#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline AArch64 execution test for the EoW REAL hook (echoes_helper, format-2
--hook-kind tail-b) and its first real request kind, equip_y ("set the Y-button
Echo/Automaton directly").

Runs the *exact relocated bytes* build_echoes_helper.py produced for
runs/equip-y-hook/plan.json under Unicorn -- same approach as
tests/ping_hook_test.py and BOTW's prototype/tests/*-hook-test.py (see
HELPER-TOOLCHAIN.md section 7). `relocated_bytes` is re-derived standalone here
(small, deliberate duplication of ping_hook_test.py's copy) so each test file
stays independently runnable with no cross-file import.

Scenarios (HELPER-TOOLCHAIN.md section 5a for the guard-chain this mirrors):
  1. idle passthrough      -- nothing pending: x0/x30/callee-saved byte-identical
                               to entry, lands exactly at main+0x26cda10; the
                               shared lock at P+0x10 is never touched.
  2. equip_y refuse 10..15 -- each native guard in the ActorSelect release chain,
                               one at a time.
  3. equip_y already       -- target id already equals the current one (status 3).
  4. equip_y success       -- full happy path through both native stub calls.
  5. ping via echoes_helper -- kind 1 still works, sharing the same tail-b hook.
  6. stale session epoch / cancelled permit -- generic refusal before kind
     dispatch, exercised against an in-flight equip_y request.
  7. lock release (eh_equip_release) -- every path PAST the acquire (refuse
     12-15, already, success) must release the shared lock at P+0x10 exactly
     once (no leak, no double-release: the lock word returns to its exact
     pre-request value every time), while refuse 10/11 (never acquired) must
     leave it untouched. One extra scenario starts the lock at 0 ("we are the
     last reader") so both conditional notify (vtable+0x68) and finalize
     (vtable+0x8) calls fire; every other scenario starts it at 0x1000000
     ("one other reader") so neither fires.
  8. HUD Y-icon refresh (eh_refresh_hud_y_icon) -- runs only on the SUCCESS
     path, after the setter and before eh_equip_release. handler+0x800 != id
     calls hud_handler_update once with (handler, id); == id does not call it;
     handler == 0 skips the entire block (no AddRef, no compare/call, no
     release at all -- this is the task's explicit spec, which diverges from
     this task's own re-verified raw disassembly where the second,
     msg's-own-reference release is reachable even when handler == 0; see
     HELPER-TOOLCHAIN.md section 5a and this task's report). The listener
     refcount at listener+8 always nets -1 (AddRef +1, two releases -1 each);
     when it starts at 0, the second release's pre-decrement value is exactly
     0, so the free fires exactly once (vtable[0x10] + hud_release_finalize);
     otherwise it never fires. msg.byte[0x10] != 0 calls hud_msg_extra_call
     with msg.u64[0x8]; == 0 does not. flag bit0 == 0 skips the WHOLE refresh
     (message builder never even called) while the setter itself still ran.

BOTW's own caveat applies here too: these are checked STUBS, not game proof --
this proves the guest assembly's control flow and memory protocol are correct,
never that the real game routine at main+0x2228a60 (or the ActorSelect vtable
call) does what the task's pseudocode assumes it does.

Requires the `unicorn` Python package; run with the same read-only interpreter
tests/ping_hook_test.py uses:

    python3 build_echoes_helper.py --hook-address 0x24ff4d0 \\
        --original-bytes 50390714 --hook-kind tail-b \\
        --original-call-target 0x26cda10 --output runs/equip-y-hook
    $UNICORN_VENV/bin/python \\
        tests/equip_y_hook_test.py
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
    UC_ARM64_REG_X19,
    UC_ARM64_REG_X20,
    UC_ARM64_REG_X21,
    UC_ARM64_REG_X22,
    UC_ARM64_REG_X23,
    UC_ARM64_REG_X24,
    UC_ARM64_REG_X25,
    UC_ARM64_REG_X26,
    UC_ARM64_REG_X29,
)

ROOT = Path(__file__).resolve().parents[1]
PLAN_PATH = ROOT / "runs" / "equip-y-hook" / "plan.json"

# Mailbox field offsets -- must match echoes_helper_mailbox.h's `offset` namespace.
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
            "--hook-kind tail-b first (see this file's module docstring for the "
            "exact command)"
        )
    plan = json.loads(PLAN_PATH.read_text())
    assert plan["format"] == 2
    guest_code = plan["guest_code"]
    patch = plan["patches"][0]

    # equip_y_native_call is now referenced TWICE (echoes_helper +
    # echoes_helper_entry, both with the SAME fixed addend 0x2228a60), and
    # echoes_helper_entry also adds its own entry_resume_target (referenced
    # ONCE, build-specific addend = that build's hook_address + 4) -- so a
    # simple count==2 filter is no longer unambiguous by itself. Exclude the
    # known-fixed equip_y_native_call value first, then the remaining addend
    # shared by >=2 sites is original_call_target (entry_resume_target's lone
    # reference never reaches count 2). See tests/ping_hook_test.py's matching
    # comment and HELPER-TOOLCHAIN.md section 2a/2b.
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
    assert original_call_target == 0x26CDA10

    # entry_offset: where echoes_helper actually starts inside the combined
    # guest_code blob (echoes_dsmod_ping_hook occupies offset 0). Read from the
    # plan's own patch relocation addend, exactly what build_echoes_helper.py
    # computed via llvm-nm -- this test does not hardcode it.
    entry_offset = patch["relocations"][0]["addend"]
    assert entry_offset > 0, "echoes_helper must not be the first routine in hook.S"

    MAIN = 0
    CALL_TARGET_ABS = MAIN + original_call_target  # 0x26cda10
    CODE = 0x0500_0000
    MAILBOX = 0x0510_0000
    STACK = 0x0600_0000
    ACTOR_SELECT_ROOT_ABS = MAIN + 0x34BC840
    EQUIP_Y_NATIVE_FN_ABS = MAIN + 0x2228A60
    P_ADDR = 0x4000_0000
    VTABLE_ADDR = 0x4010_0000
    GUARD_FN_ADDR = 0x4020_0000
    Q_ADDR = 0x4030_0000
    NOTIFY_FN_ADDR = 0x4040_0000    # P->vtable[0x68]: eh_equip_release's "notify"
    FINALIZE_FN_ADDR = 0x4050_0000  # P->vtable[0x8]: eh_equip_release's "finalize"

    # HUD Y-icon refresh (eh_refresh_hud_y_icon), success-path-only stubs.
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

    # Sanity: replaying the patch's own relocation must reproduce a plain `b`
    # (opcode 0x14000000, NOT 0x94000000/bl) from the hook site straight into
    # CODE + entry_offset -- the one instruction the runtime writes directly
    # into the game's own image.
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
        expect_handler_update_calls: list | None = None,  # list of (handler, id) tuples, or None to skip
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
            (CALL_TARGET_ABS & ~0xFFF, 0x1000),
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
        uc.mem_write(CALL_TARGET_ABS, RET)
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

        # HUD-refresh stub wiring: the flag byte, the listener object (vtable
        # pointer at +0, refcount word at +8), and the handler's field at
        # +0x800. The message-builder stub (hooked below) writes msg.u64[0x18]
        # (handler) / msg.u64[0x20] (listener) / msg.byte[0x10] / msg.u64[0x8]
        # directly into the CALLER's &msg at call time, since that address is
        # only known once the guest code actually computes it (our own local
        # stack carve-out, sp-relative).
        uc.mem_write(HUD_REFRESH_FLAG_ABS, bytes([1 if refresh_flag_bit0 else 0]))
        uc.mem_write(LISTENER_ADDR, struct.pack("<Q", LISTENER_VTABLE_ADDR))
        uc.mem_write(LISTENER_ADDR + 8, struct.pack("<Q", refresh_listener_initial_refcount & 0xFFFFFFFFFFFFFFFF))
        uc.mem_write(LISTENER_VTABLE_ADDR + 0x10, struct.pack("<Q", LISTENER_VTABLE_FN_ADDR))
        uc.mem_write(HANDLER_ADDR + 0x800, struct.pack("<H", refresh_handler_field & 0xFFFF))

        # Wire up main+0x34bc840 -> P, and (if P is real) P's vtable pointer and
        # P+8 -> Q, and Q's own fields.
        uc.mem_write(ACTOR_SELECT_ROOT_ABS, struct.pack("<Q", p_value or 0))
        if p_value is not None:
            uc.mem_write(p_value, struct.pack("<Q", VTABLE_ADDR))  # *P = vtable
            uc.mem_write(p_value + 8, struct.pack("<Q", q_value or 0))  # *(P+8) = Q
            uc.mem_write(VTABLE_ADDR + 0x40, struct.pack("<Q", GUARD_FN_ADDR))
            uc.mem_write(VTABLE_ADDR + 0x68, struct.pack("<Q", NOTIFY_FN_ADDR))
            uc.mem_write(VTABLE_ADDR + 0x8, struct.pack("<Q", FINALIZE_FN_ADDR))
        if q_value is not None:
            uc.mem_write(q_value + 0x17, bytes([0x40 if locked else 0x00]))
            uc.mem_write(q_value + 0x1970, struct.pack("<H", existing_target_id))

        # Shared release-guard lock word at P+0x10: initialised BEFORE every
        # scenario regardless of p_value, so "did we leak or double-release it"
        # is checked even for scenarios that never touch it (it just comes back
        # unchanged in that case).
        uc.mem_write(P_ADDR + 0x10, struct.pack("<Q", initial_lock))

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

        ENTRY_X0 = 0x7E57_1948_0000_0148  # sentinel "this+0x148" tail-call argument
        ENTRY_LR = 0x0999_0000_0000_0777  # sentinel "caller's return address"
        uc.reg_write(UC_ARM64_REG_SP, STACK + 0x800)
        uc.reg_write(UC_ARM64_REG_LR, ENTRY_LR)
        uc.reg_write(UC_ARM64_REG_X0, ENTRY_X0)
        callee_saved = {
            UC_ARM64_REG_X25: 0xA25,
            UC_ARM64_REG_X26: 0xA26,
            UC_ARM64_REG_X29: 0xA29,
        }
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
                    # Simulate the native acquire's side effect: increment the
                    # refcount word at P+0x10 by 0x1000000, exactly what
                    # eh_equip_release's CAS loop expects to later undo.
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
        # `until=CALL_TARGET_ABS` makes Unicorn stop exactly WHEN PC reaches that
        # address, without fetching/executing (or hooking) the instruction there --
        # so "did we land on the real tail-branch target" is read back from PC
        # after emu_start returns, not from a hook firing at that address.
        uc.emu_start(START, CALL_TARGET_ABS, count=5000)
        landed_at_target = uc.reg_read(UC_ARM64_REG_PC) == CALL_TARGET_ABS

        assert landed_at_target, (
            f"[{label}] never reached main+0x26cda10, stopped at "
            f"{uc.reg_read(UC_ARM64_REG_PC):#x} instead"
        )
        assert get(OFF_MAGIC) == 0x31574F45, f"[{label}] magic not written: {get(OFF_MAGIC):#x}"
        assert get(OFF_HEARTBEAT) == 1, f"[{label}] heartbeat not incremented"
        assert uc.reg_read(UC_ARM64_REG_X0) == ENTRY_X0, f"[{label}] x0 (tail-call arg) not preserved"
        assert uc.reg_read(UC_ARM64_REG_LR) == ENTRY_LR, f"[{label}] x30 (caller return addr) not preserved"
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

    # 1. Idle passthrough: nothing pending, x0/x30/callee-saved untouched, lands
    #    exactly at main+0x26cda10 -- byte-identical to the unmodified `b`.
    run(pending=False, expect_status=STATUS_IDLE, expect_guard_called=False,
        expect_native_called=False, label="idle passthrough")

    # 2. equip_y refuse paths, one native guard at a time.
    run(p_value=None, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_NO_ACTOR_SELECT_ROOT,
        expect_guard_called=False, expect_native_called=False, label="equip_y refuse 10 (P==0)")
    run(guard_passes=False, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_RELEASE_GUARD_FAILED,
        expect_guard_called=True, expect_native_called=False, label="equip_y refuse 11 (guard r&1==0)")
    run(q_value=None, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_NO_SECONDARY_POINTER,
        expect_guard_called=True, expect_native_called=False, label="equip_y refuse 12 (Q==0)")
    run(locked=True, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_LOCKED_FLAG_SET,
        expect_guard_called=True, expect_native_called=False, label="equip_y refuse 13 (locked flag)")
    run(target_id=0xFFFF, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_NEVER_CLEAR_Y,
        expect_guard_called=True, expect_native_called=False, label="equip_y refuse 14 (id==0xFFFF)")
    run(native_call_writes_correct=False, expect_status=STATUS_REFUSED,
        expect_result_code=REFUSE_NATIVE_ASSIGN_FAILED, expect_guard_called=True,
        expect_native_called=True, label="equip_y refuse 15 (native assign didn't take)")

    # 3. Already equipped: target id already matches, native call never reached.
    run(target_id=0x4242, existing_target_id=0x4242, expect_status=STATUS_ALREADY,
        expect_guard_called=True, expect_native_called=False, label="equip_y already equipped")

    # 4. Success: full happy path, both native stubs reached, id ends up set.
    run(target_id=0x4242, existing_target_id=0x0001, expect_status=STATUS_OK,
        expect_guard_called=True, expect_native_called=True, label="equip_y success")

    # 5. ping still works, dispatched through the SAME real hook.
    run(kind=KIND_PING, expect_status=STATUS_OK, expect_guard_called=False,
        expect_native_called=False, label="ping via echoes_helper (kind 1)")

    # 6. Stale session epoch / cancelled permit: generic refusal BEFORE kind
    #    dispatch -- result_code untouched (stays 0), guard/native never reached.
    run(matching_session=False, expect_status=STATUS_REFUSED, expect_result_code=0,
        expect_guard_called=False, expect_native_called=False, label="equip_y stale session epoch")
    run(matching_permit=False, expect_status=STATUS_REFUSED, expect_result_code=0,
        expect_guard_called=False, expect_native_called=False, label="equip_y cancelled permit")

    # 7. Last reader: lock starts at 0 (nobody else holds it); our acquire
    #    brings it to 0x1000000, our release brings it back to 0 exactly --
    #    both the notify (vtable+0x68) and finalize (vtable+0x8) calls must
    #    fire exactly once.
    run(target_id=0x5555, existing_target_id=0x0001, initial_lock=0,
        expect_status=STATUS_OK, expect_guard_called=True, expect_native_called=True,
        expect_notify_called=True, expect_finalize_called=True,
        label="equip_y success as last reader (notify+finalize fire)")

    # 8. HUD Y-icon refresh -- all six scenarios run on the equip_y success
    #    path (target_id/existing_target_id chosen so the setter succeeds).
    # 8.1: handler+0x800 != id -> hud_handler_update called once with (handler, id).
    run(target_id=0x6001, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x9999, expect_message_builder_called=True,
        expect_handler_update_calls=[(HANDLER_ADDR, 0x6001)],
        expect_release_finalize_count=0, expect_listener_refcount_after=0,
        label="hud refresh: handler field != id -> hud_handler_update called")
    # 8.2: handler+0x800 == id -> hud_handler_update NOT called.
    run(target_id=0x6002, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x6002, expect_message_builder_called=True,
        expect_handler_update_calls=[], expect_release_finalize_count=0,
        expect_listener_refcount_after=0,
        label="hud refresh: handler field == id -> hud_handler_update not called")
    # 8.3: handler == 0, listener != 0 -> no compare/call (no handler to read),
    #      but the FINAL release (outside the handler gate, msg's own
    #      reference) still runs exactly once: refcount N=1 -> N-1=0, no free
    #      (old=1 != 0). Reconciled against the far-jump blocks the owner
    #      traced (main+0x24d9a98/0x24d9aa4) -- see HELPER-TOOLCHAIN.md
    #      section 5b.
    run(target_id=0x6003, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler=None, refresh_listener_initial_refcount=1,
        expect_message_builder_called=True, expect_handler_update_calls=[],
        expect_release_finalize_count=0, expect_listener_refcount_after=0,
        label="hud refresh: handler == 0, listener != 0 -> one release, no free")
    # 8.3b: handler != 0, listener == 0 -> hud_handler_update is still called
    #       on mismatch (the compare/call doesn't depend on listener at all),
    #       but nothing ever touches a refcount (every AddRef/release site is
    #       gated on listener != 0).
    run(target_id=0x6009, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x9999, refresh_listener=None,
        expect_message_builder_called=True,
        expect_handler_update_calls=[(HANDLER_ADDR, 0x6009)],
        expect_release_finalize_count=0,
        label="hud refresh: handler != 0, listener == 0 -> update called, no refcount touch")
    # 8.4a: listener refcount starts at 1 ("msg's ref already counted") ->
    #       nets to 0 (N-1), no free (neither release's pre-decrement value
    #       is ever exactly zero).
    run(target_id=0x6004, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x6004, refresh_listener_initial_refcount=1,
        expect_release_finalize_count=0, expect_listener_refcount_after=0,
        label="hud refresh: listener refcount N=1 -> nets to 0, no free")
    # 8.4b: listener refcount starts at 0 -> still nets to N-1 (0xFFFF...FFFF,
    #       i.e. -1 as u64), but the SECOND release's pre-decrement value is
    #       exactly 0 -> free fires exactly once (vtable[0x10] + hud_release_finalize).
    run(target_id=0x6005, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x6005, refresh_listener_initial_refcount=0,
        expect_release_finalize_count=1, expect_listener_refcount_after=0xFFFFFFFFFFFFFFFF,
        label="hud refresh: listener refcount N=0 -> free fires once")
    # 8.5a: msg.byte[0x10] != 0 -> hud_msg_extra_call called with msg.u64[0x8].
    run(target_id=0x6006, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x6006, refresh_msg_extra_flag=True,
        expect_msg_extra_called=True,
        label="hud refresh: msg.byte[0x10] != 0 -> hud_msg_extra_call called")
    # 8.5b: msg.byte[0x10] == 0 -> hud_msg_extra_call NOT called.
    run(target_id=0x6007, existing_target_id=0x0001, expect_status=STATUS_OK,
        refresh_handler_field=0x6007, refresh_msg_extra_flag=False,
        expect_msg_extra_called=False,
        label="hud refresh: msg.byte[0x10] == 0 -> hud_msg_extra_call not called")
    # 8.6: refresh flag bit0 == 0 -> the WHOLE refresh is skipped (message
    #      builder never even called), but the setter itself still ran.
    run(target_id=0x6008, existing_target_id=0x0001, expect_status=STATUS_OK,
        expect_native_called=True, refresh_flag_bit0=False,
        expect_message_builder_called=False, expect_handler_update_calls=[],
        expect_release_finalize_count=0, expect_msg_extra_called=False,
        expect_listener_refcount_after=1,
        label="hud refresh: flag bit0 == 0 -> entire refresh skipped, setter still ran")

    print(json.dumps({"passed": passed, "code_bytes": len(code_bytes), "entry_offset": hex(entry_offset),
                       "original_call_target": hex(original_call_target)}))


if __name__ == "__main__":
    main()
