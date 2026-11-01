"""Research probe AArch64 execution test. Requires the optional unicorn package."""
import importlib.util
import struct
import unittest
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

spec = importlib.util.spec_from_file_location("probe", Path(__file__).parents[1] / "gen_heal_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def relocated(data, relocs, address, targets):
    result = bytearray(data)
    for r in relocs:
        at = r["offset"]
        word = struct.unpack_from("<I", result, at)[0]
        target = targets[r["target"]] + r.get("addend", 0)
        if r["kind"] == "aarch64_branch26":
            diff = target - (address + at)
            assert diff % 4 == 0 and -(1 << 27) <= diff < (1 << 27)
            word = (word & 0xfc000000) | ((diff // 4) & 0x03ffffff)
        elif r["kind"] == "aarch64_adrp":
            pages = (target >> 12) - ((address + at) >> 12)
            assert -(1 << 20) <= pages < (1 << 20)
            word = (word & 0x9f00001f) | ((pages & 3) << 29) | (((pages >> 2) & 0x7ffff) << 5)
        else:
            assert r["kind"] == "aarch64_add_lo12"
            word = (word & ~(0xfff << 10)) | ((target & 0xfff) << 10)
        struct.pack_into("<I", result, at, word)
    return bytes(result)


class ProbeTest(unittest.TestCase):
    def test_native_call_heartbeat_and_callee_saved_registers(self):
        p = probe.generate()
        main, code, mailbox = 0x80000000, 0x87000000, 0x88001000
        targets = dict(main=main, code=code, mailbox=mailbox)
        body = bytes.fromhex(p["guest_code"]["bytes"])
        # Match format2 loader's system-instruction rejection rule.
        for word, in struct.iter_unpack("<I", body):
            self.assertNotEqual(word & 0xff000000, 0xd4000000)
            self.assertTrue(word & 0xff000000 != 0xd5000000 or word == 0xd503201f)
        body = relocated(body, p["guest_code"]["relocations"], code, targets)
        patch = p["patches"][0]
        replacement = relocated(bytes.fromhex(patch["replacement"]), patch["relocations"], main+probe.HOOK, targets)
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for address in [code, mailbox, (main+probe.HOOK)&~4095, (main+probe.TICK)&~4095, 0x90000000]:
            cpu.mem_map(address, 4096)
        cpu.mem_write(code, body)
        cpu.mem_write(main+probe.HOOK, replacement)
        cpu.mem_write(mailbox, struct.pack("<II", 41, 777))
        registers = [UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X21, UC_ARM64_REG_X22, UC_ARM64_REG_FP]
        seeds = [0x19191919, 0x20202020, 0x21212121, 0x22222222, 0x29292929]
        for r, v in zip(registers, seeds):
            cpu.reg_write(r, v)
        stack, argument = 0x90000ff0, 0x1234567890
        cpu.reg_write(UC_ARM64_REG_SP, stack)
        cpu.reg_write(UC_ARM64_REG_X0, argument)
        called = []

        def tick(c, address, size, data):
            if address == main+probe.TICK:
                called.append(c.reg_read(UC_ARM64_REG_X0))
                c.reg_write(UC_ARM64_REG_X0, 0xbad)
                c.reg_write(UC_ARM64_REG_X16, 0xbad)
                c.reg_write(UC_ARM64_REG_PC, c.reg_read(UC_ARM64_REG_LR))

        cpu.hook_add(UC_HOOK_CODE, tick)
        cpu.emu_start(main+probe.HOOK, main+probe.HOOK+4, count=100)
        self.assertEqual(called, [argument])
        self.assertEqual(struct.unpack("<II", cpu.mem_read(mailbox, 8)), (42, 777))
        self.assertEqual(cpu.reg_read(UC_ARM64_REG_SP), stack)
        self.assertEqual([cpu.reg_read(r) for r in registers], seeds)


if __name__ == "__main__":
    unittest.main()
