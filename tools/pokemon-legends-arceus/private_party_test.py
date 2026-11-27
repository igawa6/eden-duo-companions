#!/usr/bin/env python3
"""Opt-in PLA-F14 check. The offline save container reader exists only in this test."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
save = os.environ.get('PLA_SAVE')
if not save:
    print('SKIP: set PLA_SAVE to the private PLA-F14 main save')
    sys.exit(77)
reference = os.environ.get('PLA_SAVE_READER')
if not reference:
    sys.exit('PLA_SAVE_READER must name the external offline pla_save.py reference')
spec = importlib.util.spec_from_file_location('party_reference', reference)
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)
blocks, valid = reader.decrypt_save(Path(save).read_bytes())
assert valid, 'save trailer hash mismatch'
party = next(b.data for b in blocks if b.key == 0x2985FE5D)
with tempfile.TemporaryDirectory(prefix='pla-party-') as scratch:
    block = Path(scratch) / 'party.bin'
    block.write_bytes(party)
    subprocess.run([sys.argv[1], '--party-block'], env={**os.environ, 'PLA_PARTY_BLOCK': str(block)}, check=True)
    for slot in range(6):
        data = party[slot * 0x178:(slot + 1) * 0x178]
        for kind, payload in [('core', data[:0x168]), ('tail', data[0x168:]), ('flag', b'\x01')]:
            (Path(scratch) / f'native-{kind}-before-{slot}.bin').write_bytes(payload)
    subprocess.run([sys.argv[1], '--native-split'],
                   env={**os.environ, 'PLA_NATIVE_CAPTURE': scratch, 'PLA_NATIVE_PHASE': 'before'}, check=True)
    for key, kind in [(0x9FE2790A, 'everyday'), (0x59A4D0C3, 'key')]:
        block = Path(scratch) / ('inventory-' + kind + '.bin')
        block.write_bytes(next(b.data for b in blocks if b.key == key))
        subprocess.run([sys.argv[1], '--inventory-block'],
                       env={**os.environ, 'PLA_INVENTORY_BLOCK': str(block),
                            'PLA_INVENTORY_KIND': kind}, check=True)
