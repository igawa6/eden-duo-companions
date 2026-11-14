#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare every runtime item name against an independent Python fixture decode."""
import os
from pathlib import Path
import subprocess
import sys
from prepare_item_names import decode


def main():
    rom = os.environ.get('RE4_TEST_ROMFS')
    core = Path(rom)/'bio4/etc/core.udas' if rom else None
    if core is None or not core.is_file():
        print('SKIP: set RE4_TEST_ROMFS with bio4/etc/core.udas')
        return 77
    expected = [row['name'] for row in decode(core.read_bytes())]
    actual = subprocess.check_output([sys.argv[1], '--names'], text=True).splitlines()
    assert actual == expected, 'runtime/Python item-name decode differs'
    print(f'{len(expected)} names match the independent fixture decode')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
