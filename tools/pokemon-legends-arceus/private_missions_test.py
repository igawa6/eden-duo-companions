#!/usr/bin/env python3
"""Compare the bounded production definition parser to the external researched reference."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
romfs = os.environ.get('PLA_ROMFS')
reference = os.environ.get('PLA_MISSIONS_READER')
if not reference and os.environ.get('PLA_SAVE_READER'):
    reference = str(Path(os.environ['PLA_SAVE_READER']).with_name('pla_missions.py'))
if not romfs or not reference or not Path(reference).is_file():
    print('SKIP: set PLA_ROMFS and external PLA_MISSIONS_READER (or sibling of PLA_SAVE_READER)')
    sys.exit(77)
spec = importlib.util.spec_from_file_location('mission_reference', reference)
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)
expected = []
for kind in ('main', 'sub'):
    for q in reader.load_defs(romfs, kind):
        expected.append(dict(id=q['id'], request=int(kind == 'sub'), hash=q['hash'],
                             title=q['name_hash'], steps=[list(s) for s in q['steps']]))
actual = json.loads(subprocess.check_output([sys.argv[1], '--mission-defs'], text=True))
assert actual == expected
print(f'Private mission definitions: all {len(actual)} IDs, state/title hashes and step triples match reference')
