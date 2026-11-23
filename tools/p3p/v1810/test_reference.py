#!/usr/bin/env python3
"""Verify the exact community-tested v1.8.10 manifest against upstream P3P."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
from v1810_reference import manifest_bytes, verify_manifest, REFERENCE_SHA256

ROOT = Path(__file__).resolve().parents[3]


def main():
    original = manifest_bytes()
    assert hashlib.sha256(original).hexdigest() == REFERENCE_SHA256
    reference = json.loads(original)
    verify_manifest(reference)

    base = ROOT / 'packages/Persona3Portable/dualscreen/manifest.json'
    if not base.is_file():
        raise AssertionError('Run tools/p3p/gen_manifest.py before this test')
    baseline = json.loads(base.read_text(encoding='utf-8'))
    # The redesign changes presentation and page navigation but must not
    # invent native reader outputs or change the user's game-control actions.
    for key in ('title_id', 'module_outputs', 'module', 'actions',
                'enforce', 'enforce_gate'):
        assert reference[key] == baseline[key], key
    assert [p['id'] for p in reference['pages']] == [
        p['id'] for p in baseline['pages']]
    assert len(reference['pages']) == 12
    assert len(reference['module_outputs']) == 2798
    assert len(reference['actions']) == 61

    assert 'composites' in reference and reference['composites']
    assert 'derived' in reference and reference['derived']
    assert 'preload' in reference and reference['preload']
    assert any(w.get('src', '').startswith('file:img/')
               for p in reference['pages'] for w in p['widgets'])
    # This is deliberately only a REFERENCE. The graphics are not embedded
    # in the source PR, and should not be treated as a ready-to-install build.
    print('Exact v1.8.10 manifest reference verified:', REFERENCE_SHA256)


if __name__ == '__main__':
    main()
