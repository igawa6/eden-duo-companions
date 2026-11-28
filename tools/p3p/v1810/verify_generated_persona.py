#!/usr/bin/env python3
"""Check the recovered Persona-detail UI surfaces against the final PNG bytes."""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path

EXPECTED = {
    'persona_detail_blue.png': '35f75cf1d93e794e99aa2b1d3b10856c2b18dc0c9914e74056d730341d000dc5',
    'persona_detail_red.png': 'c609eac69d6bc6f1626706f6e947bf230edaff173c4a0c46ed1402f03c45685b',
}

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    files = {p.name for p in args.directory.glob('*.png')}
    if files != set(EXPECTED):
        raise AssertionError(f'Expected two Persona detail PNGs, got {files}')
    for name, expected in EXPECTED.items():
        sha = hashlib.sha256((args.directory / name).read_bytes()).hexdigest()
        if sha != expected:
            raise AssertionError(f'{name} differs from final v1.8.10: {sha}')
    print('2/2 Persona detail PNGs are byte-identical to v1.8.10 FINAL')

if __name__ == '__main__':
    main()
