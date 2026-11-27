#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline assembly/plan/relocation contract. --nso opts in through PLA_NSO."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import build_helper_plan as helper


class PlanTest(unittest.TestCase):
    def test_assembly_and_relocations(self):
        plan=json.loads(helper.OUTPUT.read_text())
        self.assertEqual(plan['format'],2)
        self.assertEqual(plan['build_id'],helper.BUILD)
        self.assertEqual(plan['title_id'],'01001F5010DFA000')
        self.assertEqual(plan['mailbox_size'],4096)
        self.assertEqual(plan['mailbox_epoch_offset'],4092)
        self.assertEqual(len(plan['patches']),1)
        self.assertEqual(plan['patches'][0],dict(offset=helper.HOOK,expected=helper.EXPECTED,
            replacement='00000014',relocations=[dict(offset=0,kind='aarch64_branch26',target='code',addend=0)]))
        self.assertEqual(plan['guest_code'],helper.assemble())
        raw=bytes.fromhex(plan['guest_code']['bytes'])
        external={r['offset'] for r in plan['guest_code']['relocations']}
        for off in range(0,len(raw),4):
            word,=struct.unpack_from('<I',raw,off)
            if off in external: continue
            if word & 0xfc000000 == 0x14000000:
                imm=word & 0x3ffffff
                self.assertTrue(0 < imm < (1<<25), 'local backward branch/loop')
            elif word & 0xff000010 == 0x54000000 or word & 0x7e000000 == 0x34000000:
                imm=(word>>5)&0x7ffff
                self.assertTrue(0 < imm < (1<<18), 'conditional backward branch/loop')
        # Independently link the assembly at three ASLR layouts and compare all bytes
        # with the runtime's documented relocation arithmetic. This tests ADRP page
        # carry, ADD low12, signed BL/B displacement and every external symbol.
        for main,code,mailbox in [(0x8000000,0xc000000,0xd000000),
                                  (0x8404000,0xc103000,0xd00f000),
                                  (0x108000000,0x10c008000,0x10d002000)]:
            bases=dict(main=main,code=code,mailbox=mailbox)
            relocated=bytearray.fromhex(plan['guest_code']['bytes'])
            for r in plan['guest_code']['relocations']:
                off=r['offset'];at=code+off;target=bases[r['target']]+r['addend']
                word,=struct.unpack_from('<I',relocated,off)
                if r['kind']=='aarch64_branch26':
                    delta=target-at
                    self.assertTrue(delta%4==0 and -(1<<27)<=delta<(1<<27))
                    word=(word&0xfc000000)|((delta>>2)&0x3ffffff)
                elif r['kind']=='aarch64_adrp':
                    delta=(target>>12)-(at>>12)
                    self.assertTrue(-(1<<20)<=delta<(1<<20))
                    word=(word&~0x60ffffe0)|((delta&3)<<29)|(((delta>>2)&0x7ffff)<<5)
                else:
                    self.assertEqual(r['kind'],'aarch64_add_lo12')
                    word=(word&~0x003ffc00)|((target&0xfff)<<10)
                struct.pack_into('<I',relocated,off,word)
            with tempfile.TemporaryDirectory(prefix='pla-relocation-') as tmp:
                obj,elf,raw=[Path(tmp)/f'helper.{s}' for s in ['o','elf','bin']]
                subprocess.run(['clang','--target=aarch64-none-elf','-c',str(helper.ROOT/'native/helper/pla_guest_helper.S'),'-o',str(obj)],check=True)
                defs=[f'--defsym={symbol}={bases[target]+addend:#x}' for symbol,(target,addend) in helper.SYMBOLS.items()]
                subprocess.run(['ld.lld','-m','aarch64elf','-e','pla_entry',f'-Ttext={code:#x}',*defs,str(obj),'-o',str(elf)],check=True)
                subprocess.run(['llvm-objcopy','-O','binary','--only-section=.text',str(elf),str(raw)],check=True)
                self.assertEqual(bytes(relocated),raw.read_bytes())
        # No direct stores into the view, no indirect native calls, no loops in helper.
        with tempfile.TemporaryDirectory(prefix='pla-disassembly-') as tmp:
            obj=Path(tmp)/'helper.o'
            subprocess.run(['clang','--target=aarch64-none-elf','-c',str(helper.ROOT/'native/helper/pla_guest_helper.S'),'-o',str(obj)],check=True)
            dis=subprocess.check_output(['llvm-objdump','-dr',str(obj)],text=True)
            self.assertNotIn('blr\t',dis)
            self.assertEqual(dis.count('R_AARCH64_CALL26'),3)
        source=(helper.ROOT/'native/helper/pla_guest_helper.S').read_text()
        self.assertEqual(source.count('ldrb w8, [x19, #0x7fc]'),1)
        # LDRB is the last instruction before resume B (replayed even when idle).
        self.assertEqual(bytes.fromhex(plan['guest_code']['bytes'])[-8:-4].hex(),helper.EXPECTED)


if __name__=='__main__':
    if '--nso' in sys.argv:
        if not os.environ.get('PLA_NSO'):
            print('SKIP: set PLA_NSO to player-owned exefs/main');sys.exit(77)
        expected=helper.generate(os.environ['PLA_NSO'])
        assert expected==json.loads(helper.OUTPUT.read_text())
        # The ID is an exact 32-byte comparison, never a prefix.
        with tempfile.TemporaryDirectory(prefix='pla-build-guard-') as tmp:
            mutated=bytearray(Path(os.environ['PLA_NSO']).read_bytes());mutated[0x5f]^=1
            path=Path(tmp)/'main';path.write_bytes(mutated)
            try: helper.generate(path)
            except ValueError: pass
            else: raise AssertionError('wrong build accepted')
        print('PLA NSO bytes, layout, complete build ID and generated plan verified')
    else:
        unittest.main()
