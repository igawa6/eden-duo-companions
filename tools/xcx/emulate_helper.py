#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the XCX guest helper's relocated machine code in a CPU emulator (Unicorn) against fake game
memory, for every protocol path. Offline: no game data, no emulator of the console, no device.

The shipped load plan's guest code is relocated with synthetic bases (main at 0x10000000), the hook is
entered the way the game enters it (x0 = HUD object, sp 16-aligned, every other register holding a
recognisable value), and the run stops when execution reaches the resume address main+0x409ed8.

Checked on every run: the registers and stack the original function expects are exactly what the
original `stp x29,x30,[sp,#-0x20]!` alone would leave (x0, x19-x30, sp, and the pair stored at sp);
the helper writes nothing but its mailbox and its own stack frame; every call into game code has a
16-byte aligned sp and sees the argument the protocol promises; the game's functions are stubs that
clobber every caller-saved register, so a dependence on one would show.

    <python with unicorn> tools/xcx/emulate_helper.py        (exit 77 when unicorn is not importable)
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

try:
    from unicorn import UC_ARCH_ARM64, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc
    from unicorn import arm64_const as c
except ImportError:  # pragma: no cover
    print("unicorn is not importable; skipping")
    sys.exit(77)

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import json  # noqa: E402

import verify_helper_plan as vp  # noqa: E402

MAIN = 0x10000000
CODE = MAIN + 0x4800000
MAILBOX = CODE + 0x10000
HEAP = 0x30000000
STACK_TOP = 0x7F000000
HUD_OBJ = HEAP + 0x100000
GATE_OBJ = HEAP + 0x200000
ACTORS = HEAP + 0x300000

PARTY = MAIN + 0x1DF58D0
HUD_GLOBAL = MAIN + 0x1DF5978
GATE_GLOBAL = MAIN + 0x1FEFFC0
SINGLETON = MAIN + 0x1E0BEC0
RESUME = MAIN + 0x409ED8
FUNCS = {"resolve": 0x1A2DC, "ride": 0x2052FC, "exit": 0x20538C, "can_ride": 0x1B9C14, "can_exit": 0x1B9FBC,
         "slot_handle": 0x174EEC, "slot_record": 0x175014, "init_tmp": 0xD75C, "lookup": 0x56BED0,
         "stage": 0x41A704, "commit": 0x41A7A0, "cooldown": 0xF0388, "can_return": 0xF16B0, "field_ok": 0xF0B44}
CTRL_OBJECT = MAIN + 0x1DF5960
INPUT_GLOBAL = MAIN + 0x1FEFB70
INPUT_OBJ = HEAP + 0x500000
R_BLOCK = HUD_OBJ + 0x34130
AUX = {0: (6, 1), 11: (6, 2), 12: (6, 3)}      # the live 0x56bed0 table; id 14 fails (aux 0,0)

MAGIC_M, MAGIC_G = 0x314D4358, 0x31474358
O = dict(input=0x70, req_arg=0x5C, req_target=0x60, slot0=0x64, sub_token=0x68, sub_count=0x6C, module_magic=0x00, guest_magic=0x04, heartbeat=0x08, state=0x0C, poll_beat=0x10, countdown=0x14,
         ctrl_handle=0x18, req_seq=0x20, req_op=0x24, req_epoch=0x28, cancel=0x2C, ack=0x30, code=0x34,
         value=0x38, ack_beat=0x3C, d_char=0x40, d_skell=0x48, d_ctrl=0x50, answered=0x58, epoch=0xFFC)
S_VALID, S_PILOT, S_FOOT, S_GATE, S_BOARD, S_EXIT, S_HUD = 1, 2, 4, 8, 0x10, 0x20, 0x10000
S_TRANS, S_RECFOOT = 0x40, 0x80
S_SWREADY, S_IDLE, S_CDCLEAR, S_INPUT = 0x400, 0x800, 0x1000, 0x2000
S_CANRET, S_FIELDOK = 0x4000, 0x8000
R_CANNOTRETURN = 12
RETURN_BYTE = HUD_OBJ + 0x263A5
R_COOLDOWN, R_STALETARGET = 10, 11
R_OK, R_BADOP, R_STALE, R_CANCEL, R_NA, R_BADH, R_REFUSED, R_NOTREADY, R_TRANS = 0, 2, 3, 4, 5, 6, 7, 8, 9

GP = [getattr(c, f"UC_ARM64_REG_X{i}") for i in range(29)] + [c.UC_ARM64_REG_X29, c.UC_ARM64_REG_X30]
CALLEE_SAVED = list(range(19, 31))


class Game:
    """Fake game state plus the Unicorn instance."""

    def __init__(self, plan):
        self.plan = plan
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc.mem_map(MAIN, 0x4900000)          # main image + bss + code + mailbox
        self.uc.mem_map(HEAP, 0x1000000)
        self.uc.mem_map(STACK_TOP - 0x100000, 0x100000)
        code = vp.apply_relocations(bytes.fromhex(plan["guest_code"]["bytes"]), plan["guest_code"]["relocations"],
                                    code_base=CODE, main_base=MAIN, mailbox_base=MAILBOX)
        self.uc.mem_write(CODE, code)
        self.code_len = len(code)
        self.handles = {}       # handle -> actor address
        self.actor_next = ACTORS
        self.calls = []         # (name, x0, sp)
        self.writes = []        # (address, size)
        self.returns = {"ride": 1, "exit": 1, "can_ride": 1, "can_exit": 1, "cooldown": 0, "can_return": 1, "field_ok": 1}
        self.members = [(1, 0), (2, 11), (2, 12), (2, 14)]
        self.rec_ok = [True] * 4
        self.rec_addr = []
        for sl in range(4):
            a = HEAP + 0x400000 + sl * 0x1000
            self.rec_addr.append(a)
        self.stage_calls = []
        self.tmp_calls = []
        self.w64(CTRL_OBJECT, GATE_OBJ)
        self.w64(INPUT_GLOBAL, INPUT_OBJ)
        self.set_input(0)
        self.sync_members()
        self.uc.hook_add(UC_HOOK_CODE, self._code, begin=MAIN, end=MAIN + 0x600000)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._write)
        self.addr_to_name = {MAIN + v: k for k, v in FUNCS.items()}
        # the HUD update's world
        self.w32(MAILBOX + O["epoch"], 5)
        self.w32(MAILBOX + O["module_magic"], MAGIC_M)
        self.w64(HUD_GLOBAL, HUD_OBJ)
        self.w64(GATE_GLOBAL, GATE_OBJ)
        self.uc.mem_write(GATE_OBJ + 0x158, b"\x01")
        self.w32(SINGLETON + 0x90, 0)

    # --- memory helpers
    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def w64(self, a, v):
        self.uc.mem_write(a, struct.pack("<Q", v))

    def r32(self, a):
        return struct.unpack("<I", bytes(self.uc.mem_read(a, 4)))[0]

    def r64(self, a):
        return struct.unpack("<Q", bytes(self.uc.mem_read(a, 8)))[0]

    def mb(self, name):
        return self.r32(MAILBOX + O[name])

    def set_mb(self, name, v):
        self.w32(MAILBOX + O[name], v)

    def actor(self, handle, type_byte, d0=0x0F):
        a = self.actor_next
        self.actor_next += 0x1000
        self.uc.mem_write(a + 0xB4, bytes([type_byte]))
        self.uc.mem_write(a + 0xD0, bytes([d0]))
        self.handles[handle] = a
        return a

    def set_record(self, slot, pc_handle, skell_handle, controlled):
        rec = PARTY + 0xC + 0x14 * slot
        self.w32(rec, pc_handle)
        self.w32(rec + 4, skell_handle)
        self.uc.mem_write(rec + 9, bytes([1 if controlled else 0]))

    def set_input(self, byte):
        self.uc.mem_write(INPUT_OBJ + 0xB0, bytes([byte]))

    def sync_members(self):
        for sl, (k, i) in enumerate(self.members):
            self.uc.mem_write(self.rec_addr[sl] + 0x78, bytes([k, i]))

    def set_foot_flag(self, slot, on_foot):
        self.uc.mem_write(PARTY + 0xC + 0x14 * slot + 10, bytes([1 if on_foot else 0]))

    def scene(self, slot=0, piloting=False, pc_type=2, skell_type=4, transition=False):
        """A normal field scene: slot `slot` controlled, handles 0x100+slot / 0x200+slot."""
        for s in range(4):
            self.set_record(s, 0x100 + s, 0x200 + s, s == slot)
        # +0xd0 bit5 is the driven-actor mark: the character on foot (2f / skell 0f), the Skell while
        # piloting (2f / character 06), neither during a transition (live 2026-11-21).
        pc_d0 = 0x06 if (piloting or transition) else 0x2F
        sk_d0 = 0x2F if piloting and not transition else 0x0F
        self.pc = self.actor(0x100 + slot, pc_type, pc_d0)
        self.sk = self.actor(0x200 + slot, skell_type, sk_d0)
        self.set_foot_flag(slot, not piloting)
        return self

    # --- emulation
    def _write(self, uc, access, address, size, value, user):
        self.writes.append((address, size))

    def _code(self, uc, address, size, user):
        name = self.addr_to_name.get(address)
        if not name:
            return
        sp = uc.reg_read(c.UC_ARM64_REG_SP)
        x0 = uc.reg_read(c.UC_ARM64_REG_X0)
        self.calls.append((name, x0, sp))
        assert sp % 16 == 0, f"{name}: sp {sp:#x} is not 16-byte aligned"
        if name == "slot_handle":
            slot = x0 & 0xFFFFFFFF
            self.w32(uc.reg_read(c.UC_ARM64_REG_X8), self.r32(PARTY + 0xC + 0x14 * slot) if slot < 4 else 0)
            ret = 0
        elif name == "slot_record":
            slot = x0 & 0xFFFFFFFF
            ret = self.rec_addr[slot] if slot < 4 and self.rec_ok[slot] else 0
        elif name == "init_tmp":
            self.tmp_calls.append(("init", x0))
            ret = 0
        elif name == "lookup":
            ident, tmp = x0 & 0xFFFFFFFF, uc.reg_read(c.UC_ARM64_REG_X1)
            self.tmp_calls.append(("lookup", tmp))
            ok = ident in AUX
            if ok:
                self.uc.mem_write(tmp + 0x3F4, bytes([AUX[ident][0]]))
                self.uc.mem_write(tmp + 0x3F6, struct.pack("<H", AUX[ident][1]))
            ret = 1 if ok else 0
        elif name == "stage":
            regs = [uc.reg_read(GP[i]) & 0xFFFFFFFF for i in range(1, 7)]
            self.stage_calls.append((x0, regs))
            cnt = self.r64(x0 + 0xA8)
            at = x0 + 0x78 + 12 * cnt
            if regs[0] <= 3:                      # 0x41a704: no capacity check, as in the game
                self.uc.mem_write(at, struct.pack("<IHHHH", regs[0], regs[1] & 0xFFFF, regs[2] & 0xFFFF,
                                                  regs[3] & 0xFFFF, regs[4] & 0xFFFF))
                self.w64(x0 + 0xA8, cnt + 1)
            ret = 0
        elif name == "commit":
            self.uc.mem_write(x0 + 0xB7, bytes([uc.reg_read(c.UC_ARM64_REG_X1) & 0xFF]))
            if uc.reg_read(c.UC_ARM64_REG_X1) & 1:
                self.w32(x0 + 0xB0, 0x00030001)
                self.uc.mem_write(x0 + 0xB4, struct.pack("<H", 5))
                self.uc.mem_write(x0 + 0xB6, b"\x01")
            ret = 0
        elif name == "resolve":
            handle = self.r32(x0)
            ret = self.handles.get(handle, 0) if handle & 0x7FFFF else 0
        else:
            ret = self.returns[name]
        for r in list(range(1, 18)):                      # caller-saved garbage
            uc.reg_write(GP[r], 0xBAD0BAD0BAD00000 + r)
        uc.reg_write(c.UC_ARM64_REG_X0, ret)
        uc.reg_write(c.UC_ARM64_REG_PC, uc.reg_read(c.UC_ARM64_REG_X30))

    def hook_call(self, hud=HUD_OBJ):
        """One entry of the hooked HUD update; returns nothing, asserts the resume contract."""
        uc = self.uc
        entry_sp = STACK_TOP - 0x4000
        regs = {i: 0x1111000000000000 + (i << 8) + i for i in range(1, 29)}
        regs[29] = 0xFEEDFACE00000029
        regs[30] = 0xCA11E500000000AB
        for i, v in regs.items():
            uc.reg_write(GP[i] if i < 29 else (c.UC_ARM64_REG_X29 if i == 29 else c.UC_ARM64_REG_X30), v)
        uc.reg_write(c.UC_ARM64_REG_X0, hud)
        uc.reg_write(c.UC_ARM64_REG_SP, entry_sp)
        self.calls.clear()
        self.writes.clear()
        # the displaced instruction runs inside the helper: enter at the helper entry
        uc.emu_start(CODE, RESUME, count=200000)
        assert uc.reg_read(c.UC_ARM64_REG_PC) == RESUME, "did not resume at main+0x409ed8"
        # what `stp x29,x30,[sp,#-0x20]!` alone leaves
        assert uc.reg_read(c.UC_ARM64_REG_SP) == entry_sp - 0x20, "sp differs from the original's"
        assert uc.reg_read(c.UC_ARM64_REG_X0) == hud, "x0 not restored"
        for i in CALLEE_SAVED + [29, 30]:
            reg = GP[i] if i < 29 else (c.UC_ARM64_REG_X29 if i == 29 else c.UC_ARM64_REG_X30)
            want = regs[i]
            assert uc.reg_read(reg) == want, f"x{i} not restored ({uc.reg_read(reg):#x} != {want:#x})"
        assert self.r64(entry_sp - 0x20) == regs[29] and self.r64(entry_sp - 0x18) == regs[30], \
            "reproduced stp pair differs"
        lo, hi = entry_sp - 0x20 - 0x100, entry_sp
        for a, n in self.writes:
            in_mb = MAILBOX <= a and a + n <= MAILBOX + 0x1000
            in_stack = lo <= a and a + n <= hi
            in_ret = a == RETURN_BYTE and n == 1          # the one game byte OP_RETURN may store
            assert in_mb or in_stack or in_ret, f"write outside the mailbox and the helper's frame: {a:#x}+{n}"
        return list(self.calls)


def st(g):
    """The state word without the switch bits (those have their own checks)."""
    return g.mb("state") & ~(S_IDLE | S_CDCLEAR | S_SWREADY | S_INPUT | S_CANRET | S_FIELDOK)


def run_all():
    plan = json.loads(vp.PLAN.read_text())
    n = 0

    def check(cond, msg):
        nonlocal n
        n += 1
        assert cond, msg

    def new():
        return Game(plan)

    def names(calls):
        # the poll's read-only switch probe (slot handle/record, 0xf0388) is not an action call
        return [x[0] for x in calls if x[0] not in ("slot_handle", "slot_record", "cooldown", "can_return", "field_ok")]

    # 1. no module magic: heartbeat only, no game call, no mailbox words beyond beats
    g = new().scene()
    g.set_mb("module_magic", 0)
    calls = g.hook_call()
    check(names(calls) == [], "calls without a module")
    check(g.mb("guest_magic") == MAGIC_G and g.mb("heartbeat") == 1 and st(g) == 0, "idle beat")
    g.hook_call()
    check(g.mb("heartbeat") == 2, "heartbeat counts calls")

    # 2. poll cadence: calls 1, 11, 21 publish
    g = new().scene()
    seen = []
    for i in range(1, 26):
        g.hook_call()
        seen.append(g.mb("poll_beat"))
    check(seen[0] == 1 and seen[9] == 1 and seen[10] == 2 and seen[19] == 2 and seen[20] == 3, f"poll cadence {seen}")
    check(g.mb("heartbeat") == 25, "heartbeat")

    # 3. state words
    for slot in range(4):
        g = new().scene(slot=slot)
        calls = g.hook_call()
        want = S_VALID | S_FOOT | S_GATE | S_HUD | (slot << 8) | S_BOARD | S_RECFOOT
        check(st(g) == want, f"foot state slot {slot}: {g.mb('state'):#x} != {want:#x}")
        check([x for x in calls if x[0] == "can_ride"][-1][1] == g.pc, "canRideOn(character actor)")
        check(g.mb("d_char") == g.pc and g.mb("d_skell") == g.sk and g.r64(MAILBOX + O["d_ctrl"]) == g.pc, "diag")
        g = new().scene(slot=slot, piloting=True)
        calls = g.hook_call()
        want = S_VALID | S_PILOT | S_GATE | S_HUD | (slot << 8) | S_EXIT
        check(st(g) == want, f"pilot state slot {slot}: {g.mb('state'):#x} != {want:#x}")
        check([x for x in calls if x[0] == "can_exit"][-1][1] == g.sk, "canGetOff(Skell actor)")
    g = new().scene()
    g.returns["can_ride"] = 0
    g.hook_call()
    check(st(g) == S_VALID | S_FOOT | S_GATE | S_HUD | S_RECFOOT, "foot, canRideOn no")
    g = new().scene(piloting=True)
    g.returns["can_exit"] = 0
    g.hook_call()
    check(st(g) == S_VALID | S_PILOT | S_GATE | S_HUD, "pilot, canGetOff no")
    g = new().scene(piloting=True)
    g.w32(SINGLETON + 0x90, 1)
    calls = g.hook_call()
    check("can_exit" not in names(calls) and st(g) == S_VALID | S_PILOT | S_GATE | S_HUD,
          "exit singleton gate must skip canGetOff")
    # prompt gate closed / null: diagnostic only, eligibility is still polled and published
    g = new().scene()
    g.uc.mem_write(GATE_OBJ + 0x158, b"\x00")
    calls = g.hook_call()
    check("can_ride" in names(calls) and st(g) == S_VALID | S_FOOT | S_HUD | S_BOARD | S_RECFOOT, "gate closed")
    g = new().scene()
    g.w64(GATE_GLOBAL, 0)
    calls = g.hook_call()
    check("can_ride" in names(calls) and st(g) == S_VALID | S_FOOT | S_HUD | S_BOARD | S_RECFOOT, "gate pointer null")
    # the hook's x0 is not the game's HUD object
    g = new().scene()
    calls = g.hook_call(hud=HUD_OBJ + 0x40)
    check(names(calls) == [] and st(g) == 0, "foreign x0 must publish an empty state without any call")
    g = new().scene()
    g.w64(HUD_GLOBAL, 0)
    calls = g.hook_call(hud=0)
    check(names(calls) == [] and st(g) == 0, "null HUD")
    # no controlled record
    g = new().scene()
    for s in range(4):
        g.set_record(s, 0x100 + s, 0x200 + s, False)
    calls = g.hook_call()
    check(names(calls) == [] and st(g) == S_HUD | S_GATE, "no controlled record")
    # transition: neither actor carries bit5 -> not valid, transitioning, no call
    g = new().scene(transition=True)
    calls = g.hook_call()
    check(names(calls)[-1:] != ["can_ride"] and "can_ride" not in names(calls) and "can_exit" not in names(calls)
          and st(g) == S_HUD | S_GATE | S_TRANS | S_RECFOOT, f"transition {g.mb('state'):#x}")
    g = new().scene(piloting=True, transition=True)
    g.hook_call()
    check(st(g) == S_HUD | S_GATE | S_TRANS, "transition while mounted")
    # both actors marked: contradictory, refused
    g = new().scene()
    g.uc.mem_write(g.sk + 0xD0, b"\x2f")
    calls = g.hook_call()
    check(st(g) & (S_VALID | S_TRANS) == 0 and "can_ride" not in names(calls), "both marked")
    # stale handles (after a native switch the actors are replaced): invalid
    g = new().scene()
    g.set_record(0, 0x105, 0x205, True)            # handles that resolve to nothing
    g.hook_call()
    check(st(g) & (S_VALID | S_TRANS) == 0, "stale handles")
    g = new().scene()
    g.set_record(0, 0, 0, True)
    g.hook_call()
    check(st(g) & (S_VALID | S_TRANS) == 0, "zero handles")
    g = new().scene(pc_type=5)                       # type 5: flying / special object
    g.hook_call()
    check(st(g) & (S_VALID | S_TRANS) == 0, "type 5 actor is not a character")
    g = new().scene(piloting=True, skell_type=2)
    g.hook_call()
    check(st(g) & (S_VALID | S_TRANS) == 0, "a Skell handle whose actor is type 2")
    # no Skell at all (handle 0): on foot is still valid; Board is then the game's call
    g = new().scene()
    g.set_record(0, 0x100, 0, True)
    g.hook_call()
    check(st(g) & S_VALID and st(g) & S_FOOT, "no Skell handle, on foot")
    g = new().scene()
    g.set_record(0, 0x100, 0, True)
    g.uc.mem_write(g.pc + 0xD0, b"\x06")
    g.hook_call()
    check(st(g) & S_TRANS and not st(g) & S_VALID, "no Skell and no mark")

    def request(g, op, seq=7, epoch=5, cancel=0):
        g.set_mb("req_op", op)
        g.set_mb("req_epoch", epoch)
        g.set_mb("cancel", cancel)
        g.set_mb("req_seq", seq)           # written last, as the module does

    # 4. board on foot
    g = new().scene(slot=2)
    request(g, 1)
    calls = g.hook_call()
    check(names(calls)[-1] == "ride" and calls[-1][1] == g.pc, f"ride called with the character actor {calls}")
    check(g.mb("ack") == 7 and g.mb("code") == R_OK and g.mb("value") == 1 and g.mb("answered") == 1, "board result")
    check(g.mb("ack_beat") == g.mb("heartbeat"), "ack beat")
    check(g.mb("poll_beat") == 0 and g.mb("countdown") == 0, "no poll in the call that ran a request")
    calls = g.hook_call()                                  # next frame: answered once only
    check("ride" not in names(calls) and g.mb("answered") == 1, "request must run exactly once")
    check(g.mb("poll_beat") == 1, "poll resumes next call")
    # exit while piloting
    g = new().scene(slot=3, piloting=True)
    request(g, 2)
    calls = g.hook_call()
    check(names(calls)[-1] == "exit" and calls[-1][1] == g.sk and g.mb("code") == R_OK, "exit result")
    check("ride" not in names(calls), "exit must not call ride")
    # the game's own refusal
    g = new().scene()
    g.returns["ride"] = 0
    request(g, 1)
    g.hook_call()
    check(g.mb("code") == R_REFUSED and g.mb("value") == 0 and g.mb("ack") == 7, "RefusedByGame")
    g = new().scene(piloting=True)
    g.returns["exit"] = 0
    request(g, 2)
    g.hook_call()
    check(g.mb("code") == R_REFUSED, "exit RefusedByGame")
    # not applicable
    g = new().scene(piloting=True)
    request(g, 1)
    calls = g.hook_call()
    check(g.mb("code") == R_NA and "ride" not in names(calls), "board while piloting")
    g = new().scene()
    request(g, 2)
    calls = g.hook_call()
    check(g.mb("code") == R_NA and "exit" not in names(calls), "exit on foot")
    # gate closed does not block a request
    g = new().scene()
    g.uc.mem_write(GATE_OBJ + 0x158, b"\x00")
    request(g, 1)
    calls = g.hook_call()
    check(g.mb("code") == R_OK and names(calls)[-1] == "ride" and g.mb("ack") == 7, "gate closed still boards")
    # transitioning: no call, code 9
    g = new().scene(transition=True)
    request(g, 1)
    calls = g.hook_call()
    check(g.mb("code") == R_TRANS and "ride" not in names(calls) and g.mb("ack") == 7, "board in transition")
    g = new().scene(piloting=True, transition=True)
    request(g, 2)
    calls = g.hook_call()
    check(g.mb("code") == R_TRANS and "exit" not in names(calls), "exit in transition")
    # exit with a Skell handle of zero is a bad handle, never a call
    g = new().scene(piloting=True)
    g.set_record(0, 0x100, 0, True)
    request(g, 2)
    calls = g.hook_call()
    check("exit" not in names(calls) and g.mb("code") != R_OK, "exit without a Skell")
    # actors replaced after a native switch: old handles are dead, fresh resolution recovers
    g = new().scene()
    request(g, 1, seq=7)
    g.handles.clear()
    calls = g.hook_call()
    check(g.mb("code") == R_BADH and "ride" not in names(calls), "stale handles after a switch")
    g.scene(slot=1)
    g.set_record(0, 0x100, 0x200, False)
    request(g, 1, seq=8)
    calls = g.hook_call()
    check(g.mb("code") == R_OK and calls[-1][1] == g.pc, "fresh handles after a switch")
    g = new().scene()
    request(g, 1)
    calls = g.hook_call(hud=HUD_OBJ + 8)
    check(g.mb("code") == R_NOTREADY and names(calls) == [], "foreign HUD")
    # bad handle: no controlled record / unresolvable controlled handle
    g = new().scene()
    for s in range(4):
        g.set_record(s, 0x100 + s, 0x200 + s, False)
    request(g, 1)
    calls = g.hook_call()
    check(g.mb("code") == R_BADH and "ride" not in names(calls), "no controlled record")
    g = new().scene(piloting=True, skell_type=2)
    request(g, 2)
    calls = g.hook_call()
    check(g.mb("code") == R_BADH and "exit" not in names(calls), "Skell of the wrong type")
    # protocol refusals before any game call
    g = new().scene()
    request(g, 1, cancel=7)
    calls = g.hook_call()
    check(g.mb("code") == R_CANCEL and names(calls) == [] and g.mb("ack") == 7, "cancelled")
    g = new().scene()
    request(g, 1, epoch=4)
    calls = g.hook_call()
    check(g.mb("code") == R_STALE and names(calls) == [], "stale epoch")
    g = new().scene()
    g.set_mb("epoch", 0)
    request(g, 1, epoch=0)
    calls = g.hook_call()
    check(g.mb("code") == R_STALE and names(calls) == [], "zero epoch")
    for op in (0, 5, 0xFFFFFFFF):
        g = new().scene()
        request(g, op)
        calls = g.hook_call()
        check(g.mb("code") == R_BADOP and names(calls) == [] and g.mb("ack") == 7, f"bad op {op}")
    # a sequence number answered already is never re-run (seq == ack)
    g = new().scene()
    g.set_mb("ack", 9)
    request(g, 1, seq=9)
    calls = g.hook_call()
    check("ride" not in names(calls) and g.mb("answered") == 0, "seq == ack must idle")
    # sequence wrap (0xFFFFFFFF -> 1)
    g = new().scene()
    g.set_mb("ack", 0xFFFFFFFF)
    request(g, 1, seq=1)
    g.hook_call()
    check(g.mb("ack") == 1 and g.mb("code") == R_OK, "seq 1 after wrap")
    # every call into game code had an aligned sp (asserted inside the stubs) and a call happened
    g = new().scene()
    request(g, 1)
    calls = g.hook_call()
    check(len(calls) >= 3 and all(sp % 16 == 0 for _n, _x, sp in calls), "alignment at every call")
    # ---- OP_SWITCH ---------------------------------------------------------------------------
    def rows(g):
        cnt = g.r64(R_BLOCK + 0xA8)
        return cnt, [struct.unpack("<IHHHH", bytes(g.uc.mem_read(R_BLOCK + 0x78 + 12 * i, 12))) for i in range(cnt)]

    def sw_request(g, k, target, seq=7):
        g.set_mb("req_arg", k)
        g.set_mb("req_target", target)
        request(g, 3, seq=seq)

    # poll bits and slot 0's token
    g = new().scene()
    g.hook_call()
    check(g.mb("state") & (S_IDLE | S_CDCLEAR | S_SWREADY) == S_IDLE | S_CDCLEAR | S_SWREADY, "switch ready bits")
    check(g.mb("slot0") == 0x100, "slot 0 token (avatar)")
    g = new().scene(piloting=True)
    g.hook_call()
    check(g.mb("state") & S_SWREADY == 0 and g.mb("state") & S_IDLE, "piloting is not switch ready")
    g = new().scene()
    g.returns["cooldown"] = 1
    g.hook_call()
    check(g.mb("state") & (S_CDCLEAR | S_SWREADY) == 0 and g.mb("state") & S_IDLE, "cooldown bit")
    g = new().scene()
    g.w64(CTRL_OBJECT, 0)
    calls = g.hook_call()
    check("cooldown" not in names(calls) and g.mb("state") & (S_CDCLEAR | S_SWREADY) == 0, "null ctrl object: no 0xf0388 call")
    for off, width, label in ((0xA8, 8, "count C"), (0x38, 8, "count A"), (0x70, 8, "count B"), (0xB0, 2, "phase"),
                              (0xB2, 2, "sub"), (0xB6, 1, "busy")):
        g = new().scene()
        g.uc.mem_write(R_BLOCK + off, b"\x01" + b"\x00" * (width - 1))
        g.hook_call()
        check(g.mb("state") & (S_IDLE | S_SWREADY) == 0, f"not idle ({label}) in the poll")
        g = new().scene()
        g.uc.mem_write(R_BLOCK + off, b"\x01" + b"\x00" * (width - 1))
        sw_request(g, 1, 0x20B)
        calls = g.hook_call()
        check(g.mb("code") == R_NOTREADY and not g.stage_calls, f"switch refused while the block is not idle ({label})")
    g = new().scene()                                   # state: not idle ignores unrelated bytes +0xb4/+0xb7
    g.uc.mem_write(R_BLOCK + 0xB4, b"\x05\x00\x00\x01")
    sw_request(g, 1, 0x20B)
    g.hook_call()
    check(g.mb("code") == R_OK, "+0xb4 / +0xb7 stay set after a switch and must not block")

    # the live Zanemato -> Elma block, exactly
    g = new().scene()
    sw_request(g, 1, 0x20B)
    calls = g.hook_call()
    cnt, got = rows(g)
    check(cnt == 4 and got == [(0, 2, 11, 6, 2), (1, 1, 0, 6, 1), (2, 2, 12, 6, 3), (3, 2, 14, 0, 0)], f"live block {got}")
    check(bytes(g.uc.mem_read(R_BLOCK + 0xB0, 8)) == bytes([1, 0, 3, 0, 5, 0, 1, 1]),
          "phase bytes before consumption: 01 00 03 00 05 00 01 01 (live showed 01 00 00 00 05 00 01 01 after the consumer's sub advanced to 0)")
    check(g.mb("code") == R_OK and g.mb("value") == 0x20B and g.mb("ack") == 7 and g.mb("sub_token") == 0x20B
          and g.mb("sub_count") == 1, "switch result words")
    check(all(a == R_BLOCK and r_[5] == 1 for a, r_ in g.stage_calls) and len(g.stage_calls) == 4, "stage args (R, w6 == 1)")
    check(names(calls).count("commit") == 1 and names(calls)[-1] == "commit", "commit last")
    scratch = MAILBOX + 0x400
    check(all(t == scratch for _k, t in g.tmp_calls) and scratch % 16 == 0 and len(g.tmp_calls) == 8, "scratch is the mailbox's 0x400")
    check(g.mb("poll_beat") == 0, "no poll in the call that switched")
    # Elma -> Zanemato (party is now Elma, Zanemato, Lin, Irina)
    g = new().scene()
    g.members = [(2, 11), (1, 0), (2, 12), (2, 14)]
    g.sync_members()
    sw_request(g, 1, 0x100)
    g.hook_call()
    cnt, got = rows(g)
    check(got == [(0, 1, 0, 6, 1), (1, 2, 11, 6, 2), (2, 2, 12, 6, 3), (3, 2, 14, 0, 0)], f"second live block {got}")
    # slot 2 and 3 targets swap the same way
    g = new().scene()
    sw_request(g, 3, 0x20E)
    g.hook_call()
    cnt, got = rows(g)
    check(got == [(0, 2, 14, 0, 0), (1, 2, 11, 6, 2), (2, 2, 12, 6, 3), (3, 1, 0, 6, 1)], f"slot 3 block {got}")
    # refusals: cooldown, piloting, transition, stale target, bad slot index, empty/invalid slots
    g = new().scene()
    g.returns["cooldown"] = 1
    sw_request(g, 1, 0x20B)
    calls = g.hook_call()
    check(g.mb("code") == R_COOLDOWN and not g.stage_calls and "commit" not in names(calls), "cooldown refused")
    g = new().scene(piloting=True)
    sw_request(g, 1, 0x20B)
    calls = g.hook_call()
    check(g.mb("code") == R_NA and not g.stage_calls, "piloting refused")
    g = new().scene(transition=True)
    sw_request(g, 1, 0x20B)
    g.hook_call()
    check(g.mb("code") == R_TRANS and not g.stage_calls, "transition refused")
    g = new().scene()
    sw_request(g, 1, 0x20C)                              # slot 1 holds Elma (0x20B), the module showed Lin
    g.hook_call()
    check(g.mb("code") == R_STALETARGET and not g.stage_calls and g.mb("ack") == 7, "stale target")
    for k in (0, 4, 0xFFFFFFFF):
        g = new().scene()
        sw_request(g, k, 0x20B)
        g.hook_call()
        check(g.mb("code") == R_BADOP and not g.stage_calls, f"bad slot index {k}")
    g = new().scene()
    g.rec_ok[1] = False
    sw_request(g, 1, 0x20B)
    g.hook_call()
    check(g.mb("code") == R_BADH and not g.stage_calls, "slot k without a valid member record")
    g = new().scene()
    g.members[1] = (3, 11)                               # kind 3 is refused by the native handler
    g.sync_members()
    sw_request(g, 1, 0x30B)
    g.hook_call()
    check(g.mb("code") == R_BADH and not g.stage_calls, "kind 3 refused")
    g = new().scene()
    g.members[0] = (0, 0)
    g.sync_members()
    sw_request(g, 1, 0x20B)
    g.hook_call()
    check(g.mb("code") == R_BADH and not g.stage_calls, "slot 0 kind 0 refused")
    g = new().scene()
    g.set_record(3, 0, 0, False)                         # slot 3 empty: staged as (3, 0, 0, 0, 0)
    g.members[3] = (2, 99)
    g.sync_members()
    sw_request(g, 1, 0x20B)
    g.hook_call()
    cnt, got = rows(g)
    check(g.mb("code") == R_OK and got[3][:3] == (3, 0, 0), f"empty slot staged as kind 0 id 0 {got}")
    # protocol refusals apply to the new op as well
    g = new().scene()
    sw_request(g, 1, 0x20B)
    g.set_mb("cancel", 7)
    g.hook_call()
    check(g.mb("code") == R_CANCEL and not g.stage_calls, "switch cancelled")
    g = new().scene()
    sw_request(g, 1, 0x20B)
    g.hook_call(hud=HUD_OBJ + 8)
    check(g.mb("code") == R_NOTREADY and not g.stage_calls, "switch with a foreign HUD")
    # ---- input readiness (the byte +0xb0 bit2 of the object at main+0x1fefb70) --------------------
    g = new().scene()
    g.hook_call()
    check(g.mb("state") & S_INPUT and g.mb("state") & S_BOARD and g.mb("input") == 0, "input ready by default")
    for byte in (0x04, 0x07, 0xFF):
        g = new().scene()
        g.set_input(byte)
        calls = g.hook_call()
        check(g.mb("state") & (S_INPUT | S_BOARD | S_EXIT | S_SWREADY) == 0 and "can_ride" not in names(calls)
              and g.mb("state") & S_VALID and g.mb("input") == byte, f"input byte {byte:#x} blocks the poll")
        for op, k in ((1, 0), (2, 0), (3, 1)):
            g = new().scene(piloting=(op == 2))
            g.set_input(byte)
            if op == 3:
                g.set_mb("req_arg", 1)
                g.set_mb("req_target", 0x20B)
            request(g, op)
            calls = g.hook_call()
            check(g.mb("code") == R_NOTREADY and not [n_ for n_ in names(calls) if n_ != "resolve"] and not g.stage_calls and g.mb("ack") == 7,
                  f"op {op} refused while input byte is {byte:#x}")
    for byte in (0x00, 0x01, 0x02, 0xFB):
        g = new().scene()
        g.set_input(byte)
        g.hook_call()
        check(g.mb("state") & S_INPUT, f"other bits ({byte:#x}) must not block input")
    g = new().scene()
    g.w64(INPUT_GLOBAL, 0)
    calls = g.hook_call()
    check(g.mb("state") & (S_INPUT | S_BOARD | S_SWREADY) == 0 and g.mb("input") == 0xFFFFFFFF and "can_ride" not in names(calls),
          "no input object: not ready")
    g = new().scene()
    g.w64(INPUT_GLOBAL, 0)
    request(g, 1)
    g.hook_call()
    check(g.mb("code") == R_NOTREADY, "board with no input object")
    # ---- OP_RETURN and the can_return / field_ok / diagnostic words -----------------------------
    def ret_byte(g):
        return bytes(g.uc.mem_read(RETURN_BYTE, 1))[0]

    g = new().scene()
    g.hook_call()
    check(g.mb("state") & (S_CANRET | S_FIELDOK) == S_CANRET | S_FIELDOK, "can_return / field_ok bits")
    g = new().scene()
    g.returns["can_return"] = 0
    g.returns["field_ok"] = 0
    g.hook_call()
    check(g.mb("state") & (S_CANRET | S_FIELDOK) == 0, "can_return / field_ok clear")
    # the diagnostic words: null chains give 0xffffffff, real chains the plain values
    g = new().scene()
    g.hook_call()
    check(g.mb("input") == 0, "input byte word")
    check([g.r32(MAILBOX + 0x74 + 4 * i) for i in (0, 3)][0] == 0xFFFFFFFF, "null action-block pointer -> 0xffffffff")
    g = new().scene()
    g.w64(MAIN + 0x1FF3698, GATE_OBJ + 0x1000)
    g.w32(GATE_OBJ + 0x1000 + 0x44, 0x40A)
    g.w32(MAIN + 0x26E1950, 3)
    g.uc.mem_write(MAIN + 0x26E2308, b"\x01")
    g.w32(MAIN + 0x1DC1618, 2)
    g.w32(GATE_OBJ + 0x10, 0x23)                        # ctrl object +0x10
    g.uc.mem_write(GATE_OBJ + 0x12, b"\x03")
    ui = HEAP + 0x600000
    fade = HEAP + 0x610000
    g.w64(MAIN + 0x1DC1410, ui)
    g.w64(ui + 0x94B98, fade)
    g.w64(fade + 0x8, 0x1000)
    g.w64(fade + 0x10, 0x1018)                          # 3 layers
    g.uc.mem_write(fade + 0x70, b"\x01")
    g.uc.mem_write(HUD_OBJ + 0x210D0, b"\x01\x01")
    zone = HEAP + 0x620000
    g.w64(MAIN + 0x1DF5968, zone)
    g.w32(zone + 0x140, 4)
    g.w32(zone + 0xA8, 0x3E)
    g.uc.mem_write(MAIN + 0x1FEFB38, b"\x01")
    obj = HEAP + 0x630000
    g.w64(MAIN + 0x1F91E30, obj)
    g.uc.mem_write(obj + 0x14, b"\x02")
    o2 = HEAP + 0x640000
    g.w64(MAIN + 0x1FEFA20, o2)
    g.w64(o2 + 0x30, 0x1122334455667788)
    g.uc.mem_write(RETURN_BYTE, b"\x01")
    g.hook_call()
    want = {0x74: 0x40A, 0x78: 3, 0x7C: 1, 0x80: 2, 0x84: 0x30023, 0x88: 3, 0x8C: fade & 0xFFFFFFFF, 0x90: 3, 0x94: 1,
            0x98: 0x101, 0x9C: 4, 0xA0: 0x3E, 0xA4: 1, 0xA8: 2, 0xAC: 0x55667788, 0xB0: 1}
    for off, val in want.items():
        check(g.r32(MAILBOX + off) == val, f"diag word {off:#x}: {g.r32(MAILBOX + off):#x} != {val:#x}")

    def ret_request(g, seq=7):
        request(g, 4, seq=seq)

    g = new().scene()
    ret_request(g)
    calls = g.hook_call()
    raw = [x[0] for x in calls]
    check(ret_byte(g) == 1 and g.mb("code") == R_OK and g.mb("value") == 1 and g.mb("ack") == 7, "return stores the byte")
    check("can_return" in raw and not any(n_ in raw for n_ in ("ride", "exit", "stage", "commit")), "f16b0 asked, no other action")
    check(g.mb("poll_beat") == 0, "no poll in the request call")
    refusals = []
    g = new().scene(piloting=True)
    refusals.append((g, R_NA, "piloting"))
    g = new().scene()
    g.set_foot_flag(0, False)
    refusals.append((g, R_NA, "record byte +10 clear"))
    g = new().scene()
    g.set_record(0, 0x100, 0, True)
    refusals.append((g, R_BADH, "no Skell handle"))
    g = new().scene()
    g.uc.mem_write(RETURN_BYTE, b"\x01")
    refusals.append((g, R_NOTREADY, "a request already pending"))
    g = new().scene()
    g.w32(GATE_OBJ + 0x10, 4)
    refusals.append((g, R_NOTREADY, "world change in progress"))
    g = new().scene()
    g.w64(CTRL_OBJECT, 0)
    refusals.append((g, R_NOTREADY, "no control object"))
    g = new().scene()
    g.returns["can_return"] = 0
    refusals.append((g, R_CANNOTRETURN, "0xf16b0 false"))
    g = new().scene()
    g.set_input(4)
    refusals.append((g, R_NOTREADY, "input not accepted"))
    g = new().scene(transition=True)
    refusals.append((g, R_TRANS, "transition"))
    for g, code, label in refusals:
        ret_request(g)
        g.hook_call()
        pending = label == "a request already pending"
        check(g.mb("code") == code and ret_byte(g) == (1 if pending else 0) and g.mb("ack") == 7, f"return refused: {label}")
    g = new().scene()
    ret_request(g)
    g.hook_call(hud=HUD_OBJ + 8)
    check(g.mb("code") == R_NOTREADY and ret_byte(g) == 0, "return with a foreign HUD")
    g = new().scene()
    ret_request(g)
    g.set_mb("cancel", 7)
    g.hook_call()
    check(g.mb("code") == R_CANCEL and ret_byte(g) == 0, "return cancelled")
    # ---- field_ok (0xf0b44) is a required gate: false during loading ---------------------------
    g = new().scene()
    g.returns["field_ok"] = 0
    calls = g.hook_call()
    check(g.mb("state") & (S_FIELDOK | S_BOARD | S_EXIT | S_SWREADY | S_CANRET) == 0 and g.mb("state") & S_INPUT
          and "can_ride" not in names(calls) and "can_return" not in [x[0] for x in calls], "field_ok 0 clears every bit")
    g = new().scene(piloting=True)
    g.returns["field_ok"] = 0
    calls = g.hook_call()
    check(g.mb("state") & (S_EXIT | S_SWREADY) == 0 and "can_exit" not in names(calls), "field_ok 0 clears can_exit")
    for op in (1, 2, 3, 4):
        g = new().scene(piloting=(op == 2))
        g.returns["field_ok"] = 0
        if op == 3:
            g.set_mb("req_arg", 1)
            g.set_mb("req_target", 0x20B)
        request(g, op)
        calls = g.hook_call()
        check(g.mb("code") == R_NOTREADY and not [n_ for n_ in names(calls) if n_ != "resolve"] and not g.stage_calls
              and ret_byte(g) == 0 and g.mb("ack") == 7, f"op {op} refused while field_ok is 0")
    g = new().scene()
    request(g, 1)
    calls = g.hook_call()
    check(g.mb("code") == R_OK and names(calls).count("ride") == 1, "field_ok 1: board still works")
    print(f"emulate_helper: {n} checks passed ({len(plan['guest_code']['bytes']) // 2} bytes of guest code run)")


if __name__ == "__main__":
    run_all()
