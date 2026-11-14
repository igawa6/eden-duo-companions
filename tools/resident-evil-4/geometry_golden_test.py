#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every vertex, triangle and derived flag must equal the former generator decode."""
import json
import os
from pathlib import Path
import subprocess
import sys
from geometry_reference import decode


def main():
    rom = os.environ.get('RE4_TEST_ROMFS')
    if not rom or not (Path(rom)/'bio4/ss/cmn/map_obj1.dat').is_file():
        print('SKIP: set RE4_TEST_ROMFS for geometry goldens')
        return 77
    expected = decode(Path(rom), Path(__file__).resolve().parents[2])
    actual = json.loads(subprocess.check_output([sys.argv[1], '--geometry'], text=True))
    assert actual==expected, 'runtime/Python hit-mesh decode differs'
    print(f'{len(actual)} meshes, {sum(len(m["vertices"]) for m in actual)} vertices match')
    return 0


if __name__=='__main__':
    raise SystemExit(main())
