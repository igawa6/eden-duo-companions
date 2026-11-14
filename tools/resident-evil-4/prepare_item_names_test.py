# SPDX-License-Identifier: GPL-3.0-or-later
"""Decoder regression tests. Optional --core verifies the full native source/table pair."""
import argparse
import hashlib
import os
import importlib.util
import struct
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('names', Path(__file__).with_name('prepare_item_names.py'))
names = importlib.util.module_from_spec(spec)
spec.loader.exec_module(names)
CORE = Path(os.environ["RE4_TEST_ROMFS"])/"bio4/etc/core.udas" if os.environ.get("RE4_TEST_ROMFS") else None


class NameTests(unittest.TestCase):
    def test_native_message_format(self):
        # Archive + language directory + 64-bit offset + native glyph/control words.
        data = bytearray(0x700)
        struct.pack_into('<I', data, 0x2c, 0x420)
        struct.pack_into('<I', data, 0x420+0x15*4, 0x100)
        struct.pack_into('<I', data, 0x520+8, 0x30)
        struct.pack_into('<IIQ', data, 0x550, 0, 1, 16)
        struct.pack_into('<9H', data, 0x560, 0, 0x80+39, 0x80, 0x80+163, 0x80+181, 0xe, 8, 1, 0)
        with patch.object(names, 'SOURCE_SHA', hashlib.sha256(data).hexdigest()):
            self.assertEqual(names.decode(data), [dict(id=0, name="A '×", category='', offset=0x560)])
            struct.pack_into('<H', data, 0x562, 0xffff)
            with patch.object(names, 'SOURCE_SHA', hashlib.sha256(data).hexdigest()):
                with self.assertRaises(KeyError):names.decode(data)

    def test_unsupported_source_refused(self):
        with self.assertRaises(ValueError):names.decode(b'wrong build')

    def test_native_table(self):
        if CORE is None or not CORE.is_file():self.skipTest('set RE4_TEST_ROMFS or pass --core for native data')
        rows = names.decode(CORE.read_bytes())
        self.assertEqual(len(rows), 272)
        self.assertTrue(all(row['category']=='' for row in rows))
        self.assertEqual([r['id'] for r in rows], list(range(272)))
        self.assertTrue(all(r['offset'] % 2 == 0 for r in rows))
        self.assertEqual(names.header(rows), names.header(names.decode(CORE.read_bytes())))



if __name__ == '__main__':
    p=argparse.ArgumentParser();p.add_argument('--core',type=Path)
    args,rest=p.parse_known_args();CORE=args.core or (Path(os.environ["RE4_TEST_ROMFS"])/"bio4/etc/core.udas" if os.environ.get("RE4_TEST_ROMFS") else None)
    unittest.main(argv=[__file__,*rest])
