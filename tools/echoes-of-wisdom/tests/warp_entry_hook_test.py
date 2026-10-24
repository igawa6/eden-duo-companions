#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline AArch64 execution test for the EoW entry-hook-only request kind 6
(WARP) -- echoes_helper_entry, format-2 --hook-kind entry. Starts the game's
MapWarp event flow directly, exactly as the Map menu's confirm code does.
ENTRY HOOK ONLY -- echoes_helper (tail-b) never gained kind 6 either. There is
NO save-data P lock here at all -- this mechanism does not use it.

This is the CORRECTED sequence (the owner's own reading of the menu's full
function, text.bin + rodata, after an earlier pass mis-modeled
main+0x15c3a90's second argument as plain scratch). See HELPER-TOOLCHAIN.md
section 5d for the full writeup. Steps:

  A. pathview = {ptr = rodata "event/MapWarp.bfevfl" (main+0x288c304), len = 0x14}
  B. resource manager = *(main+0x34d23b0); == 0 -> refuse(27); else
     bl main+0x2736940(&res, resourceMgr, &pathview)      -- res: 16B {obj,ctrl}
  C. if res.obj != 0: bl main+0x38a680(res.obj)            -- no return checked
  D. bl main+0x65ee00(&holder, &res)                       -- holder: 16B {obj,ctrl}
  E. bl main+0x15c3a90(&name, &holder, &entryview)          -- name: 16B {obj,ctrl}
     name.obj == 0 -> refuse(28)
  F. bl main+0x28025f0(&container, 2)                       -- container: 16B {obj,ctrl}
  G. IF level_len > 0: bl main+0x2802f50(container.obj, "level", 5, &{ptr,len})
  H. bl main+0x2802f50(container.obj, "locator", 7, &{ptr,len}) -- always
     mgr = *(main+0x34bb8d8); == 0 -> refuse(23); else
     bl main+0x163f560(&result, mgr, &name, &container) -- the 4th argument is
     the ADDRESS of the 16-byte container pair (native main+0x25b8380..
     0x25b8390: `add x3, sp, #0xa0`), NOT container.obj's value -- a bug in an
     earlier pass (x3 = x21 = container.obj) hung the game live (gdb: stuck at
     PC=0x4, LR=blob+0xd1c, inside the result.ctrl free right after this call)
  I. release result.ctrl right after main+0x163f560 (if it ran), THEN -- on
     EVERY exit path from step A onward, including every refusal -- release,
     in this order: container.ctrl, name.ctrl, holder.ctrl, res.ctrl. Each
     release is the SAME ldaxr/stlxr-decrement-then-maybe-free pattern used
     throughout this file (free triggers when the PRE-decrement value is
     exactly zero); a null ctrl is always a safe no-op, which is what makes
     running the full cleanup unconditionally on every path safe -- an object
     never reached on a given path simply still has ctrl == 0 from
     en_warp_begin's initial zeroing.

Scenarios: full success (call order A-I, pathview bytes, all five ctrl
decrements by exactly 1, no frees); res.obj == 0 (prepare skipped, everything
else still succeeds); resource manager == 0 (refuse 27, nothing called,
cleanup is a no-op); name.obj == 0 (refuse 28, res/holder DID run and their
ctrls decrement, container/name never did); mgr == 0 (refuse 23, res/holder/
name/container ALL ran and decrement, only result.ctrl is untouched); level
skipped (level_len == 0); the four length-bound refusals; existing tests
still pass (checked separately, not in this file).

Requires the `unicorn` Python package; run with the same read-only interpreter
the other test files use:

    python3 build_echoes_helper.py --hook-address 0x2595d60 \\
        --original-bytes ff0301d1 --hook-kind entry \\
        --output runs/equip-y-entry
    $UNICORN_VENV/bin/python tests/warp_entry_hook_test.py
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
OFF_RESULT_CODE = 0x028
OFF_ENTRY_LEN = 0x030
OFF_LEVEL_LEN = 0x034
OFF_LOCATOR_LEN = 0x038
OFF_ENTRY_BYTES = 0x100
OFF_LEVEL_BYTES = 0x120
OFF_LOCATOR_BYTES = 0x160
OFF_HOST_SESSION_EPOCH = 4092

KIND_WARP = 6

STATUS_OK = 1
STATUS_REFUSED = 2

REFUSE_WARP_NO_MANAGER = 23
REFUSE_WARP_INVALID_ENTRY_LEN = 24
REFUSE_WARP_INVALID_LEVEL_LEN = 25
REFUSE_WARP_INVALID_LOCATOR_LEN = 26
REFUSE_WARP_NO_RESOURCE_MANAGER = 27
REFUSE_WARP_EMPTY_NAME = 28

PATH_STR_EXPECTED = b"event/MapWarp.bfevfl"
assert len(PATH_STR_EXPECTED) == 0x14


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
    WARP_NAME_CTOR_ABS = MAIN + 0x15C3A90
    WARP_CONTAINER_CTOR_ABS = MAIN + 0x28025F0
    WARP_CONTAINER_SET_ABS = MAIN + 0x2802F50
    WARP_FLOW_START_ABS = MAIN + 0x163F560
    WARP_MGR_PTR_ABS = MAIN + 0x34BB8D8
    WARP_LEVEL_STR_ABS = MAIN + 0x2878C34
    WARP_LOCATOR_STR_ABS = MAIN + 0x2895817
    HUD_RELEASE_FINALIZE_ABS = MAIN + 0x2833E50
    WARP_PATH_STR_ABS = MAIN + 0x288C304
    WARP_RESOURCE_MGR_PTR_ABS = MAIN + 0x34D23B0
    WARP_RESOURCE_LOAD_ABS = MAIN + 0x2736940
    WARP_RESOURCE_PREPARE_ABS = MAIN + 0x38A680
    WARP_HOLDER_CTOR_ABS = MAIN + 0x65EE00

    CONTAINER_OBJ_SENTINEL = 0xC0C0_0000_0000_0001  # never dereferenced by the guest
    NAME_OBJ_SENTINEL = 0x0A0A_0000_0000_0001
    HOLDER_OBJ_SENTINEL = 0x0B0B_0000_0000_0001
    RES_OBJ_SENTINEL = 0x0C0C_0000_0000_0001

    # The five ctrl targets ARE dereferenced by the guest's release_once()
    # (vtable ptr at +0, refcount at +8), so they need real backing memory.
    # One shared vtable/vtable-fn stub suffices -- the test only needs to know
    # THAT a release/free happened and verify each object's own refcount, not
    # which specific vtable slot was walked to get there.
    SHARED_CTRL_VTABLE_ADDR = 0x40E0_0000
    SHARED_CTRL_VTABLE_FN_ADDR = 0x40F0_0000
    RESULT_CTRL_ADDR = 0x4100_0000
    CONTAINER_CTRL_ADDR = 0x4110_0000
    NAME_CTRL_ADDR = 0x4120_0000
    HOLDER_CTRL_ADDR = 0x4130_0000
    RES_CTRL_ADDR = 0x4140_0000

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
        entry: bytes = b"MapWarpDirectEntry",
        level: bytes = b"Level1",
        locator: bytes = b"MyLocator",
        entry_len: int | None = None,
        level_len: int | None = None,
        locator_len: int | None = None,
        resource_mgr_present: bool = True,
        res_obj_nonzero: bool = True,
        name_obj_nonzero: bool = True,
        mgr_present: bool = True,
        result_ctrl_refcount: int | None = 3,   # None = ctrl is null
        container_ctrl_refcount: int | None = 3,
        name_ctrl_refcount: int | None = 3,
        holder_ctrl_refcount: int | None = 3,
        res_ctrl_refcount: int | None = 3,
        flow_start_x0: int = 0x7,
        expect_status: int,
        expect_result_code: int | None = None,
        expect_call_order: list[str] | None = None,
        expect_result_ctrl_delta: int = 0,     # -1 if released, 0 if untouched
        expect_container_ctrl_delta: int = 0,
        expect_name_ctrl_delta: int = 0,
        expect_holder_ctrl_delta: int = 0,
        expect_res_ctrl_delta: int = 0,
        expect_finalize_calls: int | None = None,
        label: str,
    ):
        nonlocal passed
        if entry_len is None:
            entry_len = len(entry)
        if level_len is None:
            level_len = len(level)
        if locator_len is None:
            locator_len = len(locator)

        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        pages = {
            CODE, MAILBOX, STACK,
            RESUME_ABS & ~0xFFF,
            WARP_NAME_CTOR_ABS & ~0xFFF,
            WARP_CONTAINER_CTOR_ABS & ~0xFFF,
            WARP_CONTAINER_SET_ABS & ~0xFFF,
            WARP_FLOW_START_ABS & ~0xFFF,
            WARP_MGR_PTR_ABS & ~0xFFF,
            HUD_RELEASE_FINALIZE_ABS & ~0xFFF,
            WARP_LEVEL_STR_ABS & ~0xFFF,
            WARP_LOCATOR_STR_ABS & ~0xFFF,
            WARP_PATH_STR_ABS & ~0xFFF,
            WARP_RESOURCE_MGR_PTR_ABS & ~0xFFF,
            WARP_RESOURCE_LOAD_ABS & ~0xFFF,
            WARP_RESOURCE_PREPARE_ABS & ~0xFFF,
            WARP_HOLDER_CTOR_ABS & ~0xFFF,
            SHARED_CTRL_VTABLE_ADDR, SHARED_CTRL_VTABLE_FN_ADDR,
            RESULT_CTRL_ADDR, CONTAINER_CTRL_ADDR, NAME_CTRL_ADDR,
            HOLDER_CTRL_ADDR, RES_CTRL_ADDR,
        }
        for base in sorted(pages):
            uc.mem_map(base, 0x1000)
        uc.mem_write(CODE, code_bytes)
        uc.mem_write(RESUME_ABS, RET)
        uc.mem_write(WARP_NAME_CTOR_ABS, RET)
        uc.mem_write(WARP_CONTAINER_CTOR_ABS, RET)
        uc.mem_write(WARP_CONTAINER_SET_ABS, RET)
        uc.mem_write(WARP_FLOW_START_ABS, RET)
        uc.mem_write(HUD_RELEASE_FINALIZE_ABS, RET)
        uc.mem_write(WARP_RESOURCE_LOAD_ABS, RET)
        uc.mem_write(WARP_RESOURCE_PREPARE_ABS, RET)
        uc.mem_write(WARP_HOLDER_CTOR_ABS, RET)
        uc.mem_write(SHARED_CTRL_VTABLE_FN_ADDR, RET)
        uc.mem_write(WARP_LEVEL_STR_ABS, b"level\x00")
        uc.mem_write(WARP_LOCATOR_STR_ABS, b"locator\x00")
        uc.mem_write(WARP_PATH_STR_ABS, PATH_STR_EXPECTED + b"\x00")
        uc.mem_write(MAILBOX, bytes(0x1000))
        uc.mem_write(SHARED_CTRL_VTABLE_ADDR, bytes(0x1000))
        uc.mem_write(SHARED_CTRL_VTABLE_ADDR + 0x10, struct.pack("<Q", SHARED_CTRL_VTABLE_FN_ADDR))

        ctrl_addrs = {
            "result": RESULT_CTRL_ADDR,
            "container": CONTAINER_CTRL_ADDR,
            "name": NAME_CTRL_ADDR,
            "holder": HOLDER_CTRL_ADDR,
            "res": RES_CTRL_ADDR,
        }
        ctrl_initial = {
            "result": result_ctrl_refcount,
            "container": container_ctrl_refcount,
            "name": name_ctrl_refcount,
            "holder": holder_ctrl_refcount,
            "res": res_ctrl_refcount,
        }
        for key, addr in ctrl_addrs.items():
            uc.mem_write(addr, bytes(0x1000))
            uc.mem_write(addr, struct.pack("<Q", SHARED_CTRL_VTABLE_ADDR))
            refcount = ctrl_initial[key]
            if refcount is not None:
                uc.mem_write(addr + 8, struct.pack("<Q", refcount & 0xFFFFFFFFFFFFFFFF))

        uc.mem_write(WARP_RESOURCE_MGR_PTR_ABS, struct.pack("<Q", 0x6B6B_0000 if resource_mgr_present else 0))
        uc.mem_write(WARP_MGR_PTR_ABS, struct.pack("<Q", 0x5A5A5A5A if mgr_present else 0))

        def put(offset: int, value: int, size: int = 4) -> None:
            uc.mem_write(MAILBOX + offset, (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little"))

        def get(offset: int, size: int = 4) -> int:
            return int.from_bytes(uc.mem_read(MAILBOX + offset, size), "little")

        session_epoch = 7
        put(OFF_HOST_SESSION_EPOCH, session_epoch)
        put(OFF_SESSION_EPOCH_COPY, session_epoch)
        put(OFF_REQUEST_KIND, KIND_WARP)
        put(OFF_ENTRY_LEN, entry_len)
        put(OFF_LEVEL_LEN, level_len)
        put(OFF_LOCATOR_LEN, locator_len)
        uc.mem_write(MAILBOX + OFF_ENTRY_BYTES, entry.ljust(32, b"\x00")[:32])
        uc.mem_write(MAILBOX + OFF_LEVEL_BYTES, level.ljust(64, b"\x00")[:64])
        uc.mem_write(MAILBOX + OFF_LOCATOR_BYTES, locator.ljust(64, b"\x00")[:64])
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

        call_order: list[str] = []
        finalize_calls = 0
        pathview_bytes: bytes | None = None
        entryview_bytes: bytes | None = None
        container_set_args = []
        flow_start_args = None

        def hook(u, address, size, _data):
            nonlocal finalize_calls, pathview_bytes, entryview_bytes, flow_start_args
            if address == WARP_RESOURCE_LOAD_ABS:
                call_order.append("resource_load")
                out = u.reg_read(UC_ARM64_REG_X0)
                mgr2 = u.reg_read(UC_ARM64_REG_X1)
                pathview_addr = u.reg_read(UC_ARM64_REG_X2)
                assert mgr2 == 0x6B6B_0000, f"resource_load mgr arg wrong: {mgr2:#x}"
                ptr, length = struct.unpack("<QI", u.mem_read(pathview_addr, 12))
                pathview_bytes = (u.mem_read(ptr, length), length)
                obj = RES_OBJ_SENTINEL if res_obj_nonzero else 0
                ctrl = RES_CTRL_ADDR if res_ctrl_refcount is not None else 0
                u.mem_write(out, struct.pack("<QQ", obj, ctrl))
            elif address == WARP_RESOURCE_PREPARE_ABS:
                call_order.append("resource_prepare")
                arg = u.reg_read(UC_ARM64_REG_X0)
                assert arg == RES_OBJ_SENTINEL, f"resource_prepare arg wrong: {arg:#x}"
            elif address == WARP_HOLDER_CTOR_ABS:
                call_order.append("holder_ctor")
                out = u.reg_read(UC_ARM64_REG_X0)
                res_in = u.reg_read(UC_ARM64_REG_X1)
                res_obj, res_ctrl = struct.unpack("<QQ", u.mem_read(res_in, 16))
                expected_obj = RES_OBJ_SENTINEL if res_obj_nonzero else 0
                expected_ctrl = RES_CTRL_ADDR if res_ctrl_refcount is not None else 0
                assert (res_obj, res_ctrl) == (expected_obj, expected_ctrl), (
                    f"holder_ctor saw res={res_obj:#x},{res_ctrl:#x}"
                )
                obj = HOLDER_OBJ_SENTINEL
                ctrl = HOLDER_CTRL_ADDR if holder_ctrl_refcount is not None else 0
                u.mem_write(out, struct.pack("<QQ", obj, ctrl))
            elif address == WARP_NAME_CTOR_ABS:
                call_order.append("name_ctor")
                out = u.reg_read(UC_ARM64_REG_X0)
                holder_in = u.reg_read(UC_ARM64_REG_X1)
                entryview_addr = u.reg_read(UC_ARM64_REG_X2)
                holder_obj, holder_ctrl = struct.unpack("<QQ", u.mem_read(holder_in, 16))
                assert holder_obj == HOLDER_OBJ_SENTINEL, f"name_ctor saw holder.obj={holder_obj:#x}"
                ptr, length = struct.unpack("<QI", u.mem_read(entryview_addr, 12))
                entryview_bytes = (u.mem_read(ptr, length), length)
                obj = NAME_OBJ_SENTINEL if name_obj_nonzero else 0
                ctrl = NAME_CTRL_ADDR if (name_obj_nonzero and name_ctrl_refcount is not None) else 0
                u.mem_write(out, struct.pack("<QQ", obj, ctrl))
            elif address == WARP_CONTAINER_CTOR_ABS:
                call_order.append("container_ctor")
                out = u.reg_read(UC_ARM64_REG_X0)
                cap = u.reg_read(UC_ARM64_REG_X1) & 0xFFFFFFFF
                assert cap == 2, f"container_ctor capacityHint != 2: {cap}"
                obj = CONTAINER_OBJ_SENTINEL
                ctrl = CONTAINER_CTRL_ADDR if container_ctrl_refcount is not None else 0
                u.mem_write(out, struct.pack("<QQ", obj, ctrl))
            elif address == WARP_CONTAINER_SET_ABS:
                container = u.reg_read(UC_ARM64_REG_X0)
                name_str_addr = u.reg_read(UC_ARM64_REG_X1)
                type_tag = u.reg_read(UC_ARM64_REG_X2) & 0xFFFFFFFF
                pair_addr = u.reg_read(UC_ARM64_REG_X3)
                ptr, length = struct.unpack("<QI", u.mem_read(pair_addr, 12))
                value_bytes = u.mem_read(ptr, length) if length else b""
                name_str = u.mem_read(name_str_addr, 8).split(b"\x00", 1)[0]
                assert container == CONTAINER_OBJ_SENTINEL, f"container_set container arg wrong: {container:#x}"
                container_set_args.append((name_str, type_tag, value_bytes, length))
                call_order.append("level_set" if type_tag == 5 else "locator_set")
            elif address == WARP_FLOW_START_ABS:
                call_order.append("flow_start")
                result_out = u.reg_read(UC_ARM64_REG_X0)
                mgr = u.reg_read(UC_ARM64_REG_X1)
                name_ref = u.reg_read(UC_ARM64_REG_X2)
                container_arg = u.reg_read(UC_ARM64_REG_X3)
                name_obj, name_ctrl = struct.unpack("<QQ", u.mem_read(name_ref, 16))
                flow_start_args = (mgr, name_obj, container_arg)
                # Regression check for the live hang this bug caused: x3 MUST be
                # the ADDRESS of the 16-byte container pair (current sp + 0x10,
                # per en_handle_warp's own frame layout), never container.obj's
                # bare value. `bl` does not alter sp, so the sp this hook reads
                # here is exactly en_handle_warp's frame -- no hardcoded absolute
                # offset needed.
                current_sp = u.reg_read(UC_ARM64_REG_SP)
                expected_container_addr = current_sp + 0x10
                assert container_arg == expected_container_addr, (
                    f"flow_start container arg wrong: got {container_arg:#x}, "
                    f"expected &container (sp+0x10) = {expected_container_addr:#x}"
                )
                container_obj_at_addr, _container_ctrl_at_addr = struct.unpack(
                    "<QQ", u.mem_read(container_arg, 16)
                )
                assert container_obj_at_addr == CONTAINER_OBJ_SENTINEL, (
                    f"flow_start container pair .obj wrong: {container_obj_at_addr:#x}"
                )
                second = RESULT_CTRL_ADDR if result_ctrl_refcount is not None else 0
                u.mem_write(result_out, struct.pack("<QQ", 0, second))
                u.reg_write(UC_ARM64_REG_X0, flow_start_x0 & 0xFFFFFFFF)
            elif address == HUD_RELEASE_FINALIZE_ABS:
                finalize_calls += 1

        uc.hook_add(UC_HOOK_CODE, hook)
        uc.emu_start(START, RESUME_ABS, count=20000)
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
        if expect_result_code is not None:
            assert get(OFF_RESULT_CODE) == (expect_result_code & 0xFFFFFFFF), (
                f"[{label}] result_code {get(OFF_RESULT_CODE):#x} != {expect_result_code:#x}"
            )
        if expect_call_order is not None:
            assert call_order == expect_call_order, f"[{label}] call_order={call_order} != {expect_call_order}"

        deltas = {
            "result": expect_result_ctrl_delta,
            "container": expect_container_ctrl_delta,
            "name": expect_name_ctrl_delta,
            "holder": expect_holder_ctrl_delta,
            "res": expect_res_ctrl_delta,
        }
        for key, addr in ctrl_addrs.items():
            initial = ctrl_initial[key]
            if initial is None:
                continue  # null ctrl: nothing to read back, by construction untouched
            after = struct.unpack_from("<Q", uc.mem_read(addr + 8, 8))[0]
            expected = (initial + deltas[key]) & 0xFFFFFFFFFFFFFFFF
            assert after == expected, (
                f"[{label}] {key}.ctrl refcount after = {after:#x}, expected {expected:#x} "
                f"(initial {initial:#x}, delta {deltas[key]})"
            )

        if expect_finalize_calls is not None:
            assert finalize_calls == expect_finalize_calls, (
                f"[{label}] finalize_calls={finalize_calls} != {expect_finalize_calls}"
            )

        if pathview_bytes is not None:
            assert pathview_bytes == (PATH_STR_EXPECTED, 0x14), f"[{label}] pathview_bytes={pathview_bytes}"
        if entryview_bytes is not None:
            assert entryview_bytes == (entry.ljust(32, b"\x00")[:32][:entry_len], entry_len), (
                f"[{label}] entryview_bytes={entryview_bytes}"
            )

        passed += 1
        print(f"ok: {label}")

    # --- Full success: call order A-I, pathview, five ctrl decrements, no frees ---
    run(
        expect_status=STATUS_OK, expect_result_code=0x7,
        expect_call_order=[
            "resource_load", "resource_prepare", "holder_ctor", "name_ctor",
            "container_ctor", "level_set", "locator_set", "flow_start",
        ],
        expect_result_ctrl_delta=-1, expect_container_ctrl_delta=-1,
        expect_name_ctrl_delta=-1, expect_holder_ctrl_delta=-1, expect_res_ctrl_delta=-1,
        expect_finalize_calls=0,
        label="warp: full success -- call order A-I, pathview, all five ctrls -1, no frees",
    )

    # --- res.obj == 0: resource_prepare (step C) skipped; everything else still succeeds ---
    run(
        res_obj_nonzero=False,
        expect_status=STATUS_OK,
        expect_call_order=[
            "resource_load", "holder_ctor", "name_ctor",
            "container_ctor", "level_set", "locator_set", "flow_start",
        ],
        expect_result_ctrl_delta=-1, expect_container_ctrl_delta=-1,
        expect_name_ctrl_delta=-1, expect_holder_ctrl_delta=-1, expect_res_ctrl_delta=-1,
        expect_finalize_calls=0,
        label="warp: res.obj == 0 -- resource_prepare skipped, rest still succeeds",
    )

    # --- resource manager == 0: refuse(27), NOTHING called, cleanup a no-op ---
    run(
        resource_mgr_present=False,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_WARP_NO_RESOURCE_MANAGER,
        expect_call_order=[],
        expect_finalize_calls=0,
        label="warp: resource manager == 0 -- refuse(27), no calls at all",
    )

    # --- name.obj == 0: refuse(28); res/holder DID run and decrement; container/name never did ---
    run(
        name_obj_nonzero=False,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_WARP_EMPTY_NAME,
        expect_call_order=["resource_load", "resource_prepare", "holder_ctor", "name_ctor"],
        expect_result_ctrl_delta=0, expect_container_ctrl_delta=0,
        expect_name_ctrl_delta=0,  # name.ctrl is never populated when name.obj == 0
        expect_holder_ctrl_delta=-1, expect_res_ctrl_delta=-1,
        expect_finalize_calls=0,
        label="warp: name.obj == 0 -- refuse(28), res/holder released, container/name untouched",
    )

    # --- mgr == 0 (the original check, after everything else succeeded) ---
    run(
        mgr_present=False,
        expect_status=STATUS_REFUSED, expect_result_code=REFUSE_WARP_NO_MANAGER,
        expect_call_order=[
            "resource_load", "resource_prepare", "holder_ctor", "name_ctor",
            "container_ctor", "level_set", "locator_set",
        ],
        expect_result_ctrl_delta=0,  # flow_start never ran -- result.ctrl untouched
        expect_container_ctrl_delta=-1, expect_name_ctrl_delta=-1,
        expect_holder_ctrl_delta=-1, expect_res_ctrl_delta=-1,
        expect_finalize_calls=0,
        label="warp: mgr == 0 -- refuse(23), everything up to flow_start already ran and released",
    )

    # --- level skipped when level_len == 0; locator still sent ---
    run(
        level_len=0,
        expect_status=STATUS_OK,
        expect_call_order=[
            "resource_load", "resource_prepare", "holder_ctor", "name_ctor",
            "container_ctor", "locator_set", "flow_start",
        ],
        expect_result_ctrl_delta=-1, expect_container_ctrl_delta=-1,
        expect_name_ctrl_delta=-1, expect_holder_ctrl_delta=-1, expect_res_ctrl_delta=-1,
        label="warp: level_len == 0 -- level Set() skipped, locator Set() still sent",
    )

    # --- A null ctrl anywhere is a safe no-op (no crash, no spurious finalize) ---
    run(
        container_ctrl_refcount=None, holder_ctrl_refcount=None,
        expect_status=STATUS_OK,
        expect_result_ctrl_delta=-1, expect_name_ctrl_delta=-1, expect_res_ctrl_delta=-1,
        expect_finalize_calls=0,
        label="warp: null container.ctrl and holder.ctrl -- safe no-ops",
    )

    # --- All five ctrls start at 0 (pre-decrement == 0 -> free fires for
    # EACH one independently): confirms the free path is reachable from
    # every one of the five NEW/extended release sites, not just assumed by
    # analogy with the already-proven result.ctrl/P-lock/HUD-listener cases,
    # and that five back-to-back frees in the same cleanup block don't
    # interfere with each other (no missed or double frees).
    run(
        result_ctrl_refcount=0, container_ctrl_refcount=0, name_ctrl_refcount=0,
        holder_ctrl_refcount=0, res_ctrl_refcount=0,
        expect_status=STATUS_OK,
        expect_result_ctrl_delta=-1, expect_container_ctrl_delta=-1,
        expect_name_ctrl_delta=-1, expect_holder_ctrl_delta=-1, expect_res_ctrl_delta=-1,
        expect_finalize_calls=5,
        label="warp: all five ctrls start at 0 -- each frees exactly once, no interference",
    )

    # --- length-bound refusals (unchanged from before the correction) ---
    run(entry_len=0, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_WARP_INVALID_ENTRY_LEN,
        expect_call_order=[], label="warp: entry_len == 0 -> refuse")
    run(entry_len=33, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_WARP_INVALID_ENTRY_LEN,
        expect_call_order=[], label="warp: entry_len > 32 -> refuse")
    run(level_len=65, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_WARP_INVALID_LEVEL_LEN,
        expect_call_order=[], label="warp: level_len > 64 -> refuse")
    run(locator_len=0, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_WARP_INVALID_LOCATOR_LEN,
        expect_call_order=[], label="warp: locator_len == 0 -> refuse")
    run(locator_len=65, expect_status=STATUS_REFUSED, expect_result_code=REFUSE_WARP_INVALID_LOCATOR_LEN,
        expect_call_order=[], label="warp: locator_len > 64 -> refuse")

    print(json.dumps({"passed": passed, "code_bytes": len(code_bytes), "entry_offset": hex(entry_offset),
                       "entry_resume_target": hex(entry_resume_target)}))


if __name__ == "__main__":
    main()
