"""Native Healing Skills ABI tests with AArch64 native-function oracles."""
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
from test_heal_probe import probe, relocated


class QueryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = Path(__file__).resolve().parents[3]/"native/guest/ot0_heal_query.c"
        with tempfile.TemporaryDirectory() as temporary:
            obj, binary = Path(temporary)/"q.o", Path(temporary)/"q.bin"
            subprocess.run(["clang", "--target=aarch64-none-elf", "-O2", "-ffreestanding", "-fno-builtin", "-fno-jump-tables", "-fno-stack-protector", "-c", str(source), "-o", str(obj)], check=True)
            subprocess.run(["llvm-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(binary)], check=True)
            cls.code = binary.read_bytes()

    def run_query(self, state=23, menu=0, event=0, learned=1, sp=68, locked=0,
                  missing=1, target=904, op=1, epoch=777, deadline=100, modal=0, system=0,
                  apply=False, corrupt=None, category=5, revive=False):
        p = probe.generate(self.code)
        main, code, mailbox = 0x80000000, 0x87000000, 0x88001000
        targets = dict(main=main, code=code, mailbox=mailbox)
        body = bytes.fromhex(p["guest_code"]["bytes"])
        for word, in struct.iter_unpack("<I", body):
            self.assertNotEqual(word & 0xff000000, 0xd4000000)
            self.assertTrue(word & 0xff000000 != 0xd5000000 or word == 0xd503201f)
        body = relocated(body, p["guest_code"]["relocations"], code, targets)
        patch = p["patches"][0]
        replacement = relocated(bytes.fromhex(patch["replacement"]), patch["relocations"], main+probe.HOOK, targets)
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        mapped = set()

        def map_page(p):
            page = p & ~4095
            if page not in mapped:
                cpu.mem_map(page, 4096)
                mapped.add(page)

        def write(p, value, fmt="<I"):
            map_page(p)
            cpu.mem_write(p, struct.pack(fmt, value))

        for address in [code, mailbox, main+probe.HOOK, main+probe.TICK, 0x90000000]:
            map_page(address)
        for address in range(code,code+len(body),4096):map_page(address)
        cpu.mem_write(code, body)
        cpu.mem_write(main+probe.HOOK, replacement)
        # Bounded native DB/party/ability and isolated transaction oracles.
        app, menus, char_db, learnable, ability = [0x91000000+i*4096 for i in range(5)]
        party, ids, effects, effect_ids, effect = [0x91005000+i*4096 for i in range(5)]
        write(main+0x7cc8cc0, app, "<Q")
        write(main+0x7cc9a18, menus, "<Q")
        write(main+0x7cc6288, 0x9100a000, "<Q")
        write(main+0x7cc8540, 0x91010000, "<Q")
        write(0x91010000+0x2c8, 0x91011000, "<Q")
        write(0x9100a000+0x290, 0x9100b000, "<Q")
        write(0x9100b000+0x2c8, 0x9100c000, "<Q")
        write(app+0x2f0, state, "<B")
        write(menus+0x2a8, 0x9100d000, "<Q")
        write(0x9100d000+0x108, 0x9100e000, "<Q")
        write(0x9100d000+0x110, 0x9100f000, "<Q")
        write(0x9100e000+8, 0x9100e000, "<Q")
        write(0x9100e000+0x184, 0x11 if modal else 2, "<B")
        write(0x9100e000+0x185, 1, "<B")
        write(0x9100f000+8, 1)
        write(menus+0x38a, menu, "<B")
        write(char_db+0x10, learnable, "<Q")
        write(char_db+0x18, 1)
        write(learnable, 900099)
        write(ability+8, 900099)
        write(ability+0xe4, 3)
        write(ability+0xfc, 1, "<B")
        write(party, ids, "<Q")
        write(party+8, 2)
        write(party+12, 2)
        write(ids, 10804)
        write(ids+4, 904)
        write(effects, effect_ids, "<Q")
        # Actual First Aid has20 padded effect slots, first74750/type30/
        # category5 and19 zero IDs. A guessed16-entry cap rejected it.
        write(effects+8, 20)
        write(effects+12, 20)
        write(effect_ids, 1234)
        write(effect+0x14, 30)
        write(effect+0x18, 7 if revive else 0, "<B")
        write(effect+0x1c, 5, "<B")
        fields = [41, 777, op, 2, 0, 0, 10804, 900099, target, 0, 0, 0, 0, 0, 0, 0, epoch, deadline]
        cpu.mem_write(mailbox, struct.pack("<18I", *fields))
        called = []
        functions = {probe.TICK, 0x42e6b4, 0x3942d94, 0x3a70e10, 0x38eb770,
                     0x3af8b68, 0x3a81e48, 0x3af9c00, 0x3b08d28, 0x3a74844,
                     0x3a743a0, 0x3b0c130, 0x3a7bd40, 0x3b01084, 0x3a7bf38,
                     0x3a7bdd4, 0x3af9ee0, 0x3b0d74c, 0x3a742f8, 0x297f790, 0x39db094,
                     0x3afb9ac, 0x3c76af0, 0x3a748c4, 0xa2444, 0x3a7c240}
        for offset in functions:
            map_page(main+offset)

        resources={'hp':0 if revive else 727,'sp':sp}
        def native(c, address, size, data):
            offset = address-main
            if offset not in functions:
                return
            called.append(offset)
            if offset == 0x3a81e48:
                c.mem_write(c.reg_read(UC_ARM64_REG_X0), bytes(16))
            if offset == 0x3c76af0:
                self.assertEqual(c.reg_read(UC_ARM64_REG_X0),0)
                array=c.reg_read(UC_ARM64_REG_X1)
                ptr,n,cap=struct.unpack('<Qii',c.mem_read(array,16))
                self.assertEqual((n,cap),(1,1))
                self.assertEqual(struct.unpack('<I',c.mem_read(ptr,4))[0],904)
                self.assertEqual(c.reg_read(UC_ARM64_REG_X2),ability)
                self.assertEqual(c.reg_read(UC_ARM64_REG_X3),10804)
                resources['hp']=765
            if offset == 0x3a748c4:
                self.assertEqual(c.reg_read(UC_ARM64_REG_X0),10804)
                resources['sp']=c.reg_read(UC_ARM64_REG_W1)
            values = {probe.TICK: 0, 0x42e6b4: 1, 0x3942d94: event,
                      0x3a70e10: party, 0x38eb770: char_db, 0x3af8b68: learned,
                      0x3a81e48: 0, 0x3af9c00: ability, 0x3b08d28: 6,
                      0x3a74844: resources['sp'],
                      0x3a743a0: 500 if revive and c.reg_read(UC_ARM64_REG_X0)==10804 else resources['hp'],
                      0x3b0c130: int(revive),
                      0x3a7bd40: 1, 0x3b01084: effects, 0x3a7bf38: int(revive and c.reg_read(UC_ARM64_REG_X0)==10804 or resources['hp']>0), 0x3a7c240: 1,
                      0x3a7bdd4: missing, 0x3af9ee0: effect, 0x3b0d74c: 0,
                      0x3a742f8: 765, 0x297f790: locked, 0x39db094: system, 0x3afb9ac: category,
                      0x3c76af0:0,0x3a748c4:0,0xa2444:0}
            c.reg_write(UC_ARM64_REG_X0, values[offset])
            c.reg_write(UC_ARM64_REG_PC, c.reg_read(UC_ARM64_REG_LR))

        cpu.hook_add(UC_HOOK_CODE, native)
        cpu.reg_write(UC_ARM64_REG_SP, 0x90000ff0)
        cpu.emu_start(main+probe.HOOK, main+probe.HOOK+4, count=10000)
        result = struct.unpack("<18I", cpu.mem_read(mailbox, 72))
        self.assertEqual(result[0], 42)
        self.assertEqual(result[4], 2)
        if apply:
            self.assertEqual(result[5],1)
            write(mailbox+8,2)
            write(mailbox+12,4)
            if corrupt is not None:
                off,value=corrupt;write(mailbox+off,value)
            cpu.emu_start(main+probe.HOOK,main+probe.HOOK+4,count=10000)
            applied=struct.unpack('<32I',cpu.mem_read(mailbox,128))
            effects=called.count(0x3c76af0)
            # Same committed request and a later query followed by an old
            # APPLY sequence must never duplicate native resource consumption.
            cpu.emu_start(main+probe.HOOK,main+probe.HOOK+4,count=10000)
            self.assertEqual(called.count(0x3c76af0),effects)
            if corrupt is None:
                self.assertEqual(applied[5],16)
                self.assertEqual((resources['hp'],resources['sp']),(765,sp-6))
                self.assertEqual(effects,1)
                self.assertEqual(called.count(0x3a748c4),1)
                self.assertLess(called.index(0x3c76af0),called.index(0x3a748c4))
            else:
                self.assertIn(applied[5],(3,15))
                self.assertEqual(effects,0)
                self.assertEqual(resources,{'hp':727,'sp':sp})
            return applied
        # Queries never invoke effects, setters or inventory transactions.
        for forbidden in [0x3c76af0, 0x3a74420, 0x3a748c4, 0x3c20070, 0x3a9402c]:
            self.assertNotIn(forbidden, called)
        return result

    def test_query_reports_native_cost_and_resources(self):
        r = self.run_query()
        self.assertEqual(r[5], 1)
        self.assertEqual(r[9:14], (6, 727, 68, 765, 1))

    def test_discovery_uses_learned_native_candidate(self):
        r=self.run_query(op=3)
        self.assertEqual(r[5:9],(1,10804,900099,904))

    def test_apply_charges_once_after_native_effect(self):
        self.run_query(apply=True)

    def test_apply_rejects_changed_query_context(self):
        for off,value in [(72,726),(76,67),(80,5),(84,999),(88,0),
                          (96,0),(104,0),(64,778),(68,1)]:
            with self.subTest(offset=off):self.run_query(apply=True,corrupt=(off,value))

    def test_revival_skill_on_ko_target(self):
        # KO'd member (HP 0, native alive false): only a revival skill qualifies, the global gate is
        # AnyDead, and APPLY runs the same native effect + SP charge once.
        r = self.run_query(revive=True)
        self.assertEqual(r[5], 1)
        self.assertEqual(r[10], 0)
        self.run_query(revive=True, apply=True)

    def test_query_refusals(self):
        for options, expected in [({"op": 99}, 2), ({"op": 2}, 15), ({"epoch": 778}, 3),
                                  ({"deadline": 1}, 3), ({"state": 25}, 4),
                                  ({"menu": 31}, 5), ({"event": 1}, 6),
                                  ({"target": 999}, 7), ({"learned": 0}, 8),
                                  ({"sp": 5}, 10), ({"missing": 0}, 11), ({"locked": 1}, 13), ({"modal": 1}, 14), ({"system": 1}, 14)]:
            with self.subTest(options=options):
                self.assertEqual(self.run_query(**options)[5], expected)
        self.assertEqual(self.run_query(category=6)[5],17)


if __name__ == "__main__":
    unittest.main()
