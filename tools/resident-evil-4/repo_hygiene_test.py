#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Public-tree policy: no native name table; fewer than 20 long fixture names.

Single names in comments are references. Decoded tables, pixels, glyph metrics and
collision meshes belong in runtime memory or external fixture scratch.
"""
import os
from pathlib import Path
import re
import subprocess
import unittest
from prepare_item_names import decode

ROOT = Path(__file__).resolve().parents[2]


def tracked():
    # An archive has no index; scan its RE4 sources with the same rules.
    if (ROOT/'.git').exists():
        paths = subprocess.check_output(['git', 'ls-files', '-z', '--', 'native/',
            'tools/resident-evil-4/', 'packages/ResidentEvil4/'], cwd=ROOT).decode().split('\0')
    else:
        paths = [str(p.relative_to(ROOT)) for folder in ('native', 'tools/resident-evil-4',
            'packages/ResidentEvil4') for p in (ROOT/folder).rglob('*') if p.is_file()]
    return [(p, (ROOT/p).read_text(errors='replace')) for p in paths if p and
            (ROOT/p).is_file() and ('re4' in p.lower() or 'resident' in p.lower() or
            'ResidentEvil4' in p or '010099A00BC1E000' in p)]


class Hygiene(unittest.TestCase):
    def test_no_native_tables(self):
        for path, text in tracked():
            self.assertNotRegex(path, r're4_item_names.*\.h$')
            if not path.endswith((".h", ".cpp", ".inc")):
                continue
            self.assertNotRegex(text, r'(?:constexpr|const)\s+(?:std::array<)?ItemName\b')
            self.assertNotRegex(text, r'inline constexpr MapVertex\s+\w+_vertices\[')
            self.assertNotRegex(text, r'inline constexpr MapTriangle\s+\w+_triangles\[')

    def test_no_bulk_fixture_names(self):
        rom = os.environ.get('RE4_TEST_ROMFS')
        core = Path(rom)/'bio4/etc/core.udas' if rom else None
        if core is None or not core.is_file():
            self.skipTest('RE4_TEST_ROMFS absent; structural table check still runs')
        names = {r['name'] for r in decode(core.read_bytes()) if len(r['name']) >= 9}
        tree = '\n'.join(text for _, text in tracked())
        count = sum(name in tree for name in names)
        self.assertLess(count, 20, f'{count} distinct native names: possible decoded table')


if __name__ == '__main__':
    unittest.main()
