#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the actual AArch64 HUD helper with mocked Unity native methods (Unicorn)."""
import struct
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC, UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X21, UC_ARM64_REG_X22, UC_ARM64_REG_D8, UC_ARM64_REG_D9
import gen_load_plan as plan

BASE, MB, HEAP, STACK = 0x800000000, 0x900000000, 0xA00000000, 0xB00000000


class HelperTests(unittest.TestCase):
    def setUp(self):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for address, size in [(BASE + 0x2407000, 0x2000), (BASE + plan.CAVE // 4096 * 4096, 0x1000),
                              (MB, 0x1000), (HEAP, 0x10000), (STACK, 0x10000)]:
            self.uc.mem_map(address, size)
        raw, reloc, self.labels = plan.assemble(plan.CAVE, plan.source([0xFC1B0FE8, 0xA9BE7BFD, 0x6DBA23E9]))
        code = bytearray(raw)
        for offset in reloc:
            pc = BASE + plan.CAVE + offset
            imm = ((MB >> 12) - (pc >> 12)) & 0x1FFFFF
            word = struct.unpack_from('<I', code, offset)[0]
            struct.pack_into('<I', code, offset, (word & 31) | 0x90000000 |
                             ((imm & 3) << 29) | ((imm >> 2) << 5))
        self.uc.mem_write(BASE + plan.CAVE, bytes(code))
        self.active, self.get_go, self.calls = {}, {}, []
        self.uc.hook_add(UC_HOOK_CODE, self.native_call)
        self.time, self.tracker, self.time_go = HEAP + 0x100, HEAP + 0x200, HEAP + 0x300
        for ptr in (self.time, self.tracker, self.time_go):
            self.put64(ptr, HEAP + 0x8000)
            self.put64(ptr + 16, ptr + 0x4000)
        self.get_go[self.time] = self.time_go
        self.active = {self.time_go: True, self.tracker: True}
        self.register()

    def put64(self, ptr, value):
        self.uc.mem_write(ptr, struct.pack('<Q', value))

    def get32(self, off):
        return struct.unpack('<I', self.uc.mem_read(MB + off, 4))[0]

    def put32(self, off, value):
        self.uc.mem_write(MB + off, struct.pack('<I', value))

    def frame(self):
        self.uc.reg_write(UC_ARM64_REG_SP, STACK + 0x8000)
        self.uc.reg_write(UC_ARM64_REG_LR, BASE + plan.CAVE - 4)
        self.uc.emu_start(BASE + plan.CAVE + self.labels['step'], BASE + plan.CAVE - 4, count=10000)
        assert self.uc.reg_read(UC_ARM64_REG_PC) == BASE + plan.CAVE - 4

    def native_call(self, uc, address, size, data):
        x0 = uc.reg_read(UC_ARM64_REG_X0)
        if address == BASE + 0x24071D0:
            uc.reg_write(UC_ARM64_REG_X0, self.get_go[x0])
        elif address == BASE + 0x2408570:
            uc.reg_write(UC_ARM64_REG_X0, int(self.active[x0]))
        elif address == BASE + 0x2408500:
            value = bool(uc.reg_read(UC_ARM64_REG_X1))
            self.active[x0] = value
            self.calls.append((x0, value))
        else:
            return
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    def register(self):
        for off, ptr in ((24, self.time), (48, self.tracker)):
            self.put64(MB + off, ptr)
            self.put64(MB + off + 8, HEAP + 0x8000)
            self.put64(MB + off + 16, ptr + 0x4000)

    def request(self, mask, lease=120):
        self.put32(12, mask)
        self.put32(8, lease)
        self.frame()

    def test_hooks_replay_prologues_and_preserve_registers(self):
        for name, site in plan.HOOKS:
            with self.subTest(hook=name):
                self.uc.mem_map(BASE + site // 4096 * 4096, 4096)
                self.uc.mem_write(BASE + site, plan.branch(site, plan.CAVE + self.labels[name]))
                if name == 'tracker':
                    obj = HEAP + 0x900
                    self.put64(obj + 0x78, self.tracker)
                else:
                    obj = self.time
                self.uc.reg_write(UC_ARM64_REG_X0, obj)
                self.uc.reg_write(UC_ARM64_REG_X1, 12345)
                preserved = (UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X21,
                             UC_ARM64_REG_X22, UC_ARM64_REG_D8, UC_ARM64_REG_D9)
                for i, reg in enumerate(preserved):
                    self.uc.reg_write(reg, 100 + i)
                self.uc.reg_write(UC_ARM64_REG_SP, STACK + 0x8000)
                self.uc.emu_start(BASE + site, BASE + site + 4, count=10000)
                self.assertEqual(self.uc.reg_read(UC_ARM64_REG_X0), obj)
                self.assertEqual(self.uc.reg_read(UC_ARM64_REG_X1), 12345)
                for i, reg in enumerate(preserved):
                    self.assertEqual(self.uc.reg_read(reg), 100 + i)
                delta = {'manager': 80, 'time': 32, 'tracker': 96}[name]
                self.assertEqual(self.uc.reg_read(UC_ARM64_REG_SP), STACK + 0x8000 - delta)

    def test_defaults_and_independent_restore(self):
        self.request(3)
        self.assertEqual(self.get32(16), 3)
        self.assertEqual(self.active, {self.time_go: False, self.tracker: False})
        self.request(2)
        self.assertEqual(self.get32(16), 2)
        self.assertTrue(self.active[self.time_go])
        self.request(1)
        self.assertEqual(self.get32(16), 1)
        self.assertTrue(self.active[self.tracker])
        self.request(0, 0)
        self.assertTrue(all(self.active.values()))
        self.assertEqual(self.get32(16), 0)

    def test_expiry_and_initially_inactive(self):
        self.active[self.tracker] = False
        self.request(3, 2)
        self.frame()
        self.frame()
        self.assertTrue(self.active[self.time_go])
        self.assertFalse(self.active[self.tracker])
        self.assertNotIn((self.tracker, True), self.calls)

    def test_game_reshows_then_helper_hides(self):
        self.request(3)
        self.active[self.time_go] = True
        self.frame()
        self.assertFalse(self.active[self.time_go])
        self.request(0, 0)
        self.assertTrue(all(self.active.values()))

    def test_destroyed_object_and_reused_wrapper(self):
        self.request(3)
        self.put64(self.time + 16, 0)
        self.frame()
        self.assertTrue(self.active[self.time_go])
        self.put64(self.tracker + 16, self.tracker + 0x5000)
        self.active[self.tracker] = True
        self.put64(MB + 64, self.tracker + 0x5000)
        self.frame()
        self.assertFalse(self.active[self.tracker])
        self.request(0, 0)
        self.assertTrue(self.active[self.tracker])

    def test_new_scene_restores_owned_object(self):
        self.request(3)
        new = HEAP + 0x500
        self.put64(new, HEAP + 0x8000)
        self.put64(new + 16, new + 0x4000)
        self.active[new] = True
        self.put64(MB + 48, new)
        self.put64(MB + 64, new + 0x4000)
        self.frame()
        self.assertTrue(self.active[self.tracker])
        self.assertFalse(self.active[new])
        self.request(0, 0)
        self.assertTrue(self.active[new])


if __name__ == '__main__':
    unittest.main()
