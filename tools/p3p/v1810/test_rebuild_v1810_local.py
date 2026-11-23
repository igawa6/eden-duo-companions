#!/usr/bin/env python3
"""Synthetic, asset-free tests for packaging the full v1.8.10 UI locally."""
from __future__ import annotations

import csv
import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

import rebuild_v1810_local as builder


def make_elf(machine: int, marker: bytes) -> bytes:
    out = bytearray(128)
    out[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<HH', out, 16, 3, machine)
    out[64:64 + len(marker)] = marker
    return bytes(out)


class LocalPackageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.reference = root / 'original.zip'
        self.output = root / 'rebuilt.dsmod.zip'
        self.linux = root / 'linux.so'
        self.android = root / 'android.so'
        self.inventory = root / 'ASSET_PROVENANCE.csv'
        original = {
            'linux-x86_64': make_elf(62, b'old-linux'),
            'android-arm64-v8a': make_elf(183, b'old-android'),
        }
        module = {'abi': 1, 'build_ids': ['test'],
                  'libraries': {
                      platform: {
                          'path': path.removeprefix('dualscreen/'),
                          'sha256': builder.digest(original[platform]),
                      }
                      for platform, (path, _) in builder.MODULES.items()
                  }}
        manifest = json.dumps({'title_id': '0100DCD01525A000', 'module': module}).encode()
        inventory = []
        with zipfile.ZipFile(self.reference, 'w', zipfile.ZIP_DEFLATED) as archive:
            for index in range(62):
                name = f'dualscreen/img/synthetic_{index:02d}.png'
                data = f'SYNTHETIC FILE {index}'.encode()
                archive.writestr(name, data)
                inventory.append({'index': index, 'asset': name,
                                  'size_bytes': len(data), 'sha256': builder.digest(data),
                                  'origin': 'synthetic, no original art'})
            archive.writestr('dualscreen/manifest.json', manifest)
            archive.writestr('package.json', json.dumps({'module': module}).encode())
            archive.writestr('dualscreen/p3p_font.txt', b'synthetic text')
            for platform, (path, _) in builder.MODULES.items():
                archive.writestr(path, original[platform])
        with self.inventory.open('w', newline='', encoding='utf-8') as handle:
            writer = csv.DictWriter(handle, fieldnames=['index', 'asset', 'size_bytes',
                                                       'sha256', 'origin'])
            writer.writeheader()
            writer.writerows(inventory)
        old_archive_sha, old_manifest_sha = builder.ARCHIVE_SHA, builder.MANIFEST_SHA
        self.addCleanup(lambda: setattr(builder, 'ARCHIVE_SHA', old_archive_sha))
        self.addCleanup(lambda: setattr(builder, 'MANIFEST_SHA', old_manifest_sha))
        builder.ARCHIVE_SHA = builder.digest(self.reference.read_bytes())
        builder.MANIFEST_SHA = builder.digest(manifest)
        self.linux.write_bytes(make_elf(62, b'new-linux'))
        self.android.write_bytes(make_elf(183, b'new-android'))

    def test_output_preserves_all_assets_and_rehashes_modules(self) -> None:
        builder.build(self.reference, self.linux, self.android,
                      self.output, self.inventory)
        with zipfile.ZipFile(self.reference) as original, zipfile.ZipFile(self.output) as rebuilt:
            self.assertEqual(len(rebuilt.namelist()), 67)
            self.assertIsNone(rebuilt.testzip())
            for path in original.namelist():
                if path.endswith('.png') or path.endswith('p3p_font.txt'):
                    self.assertEqual(original.read(path), rebuilt.read(path))
            manifest = json.loads(rebuilt.read('dualscreen/manifest.json'))
            package = json.loads(rebuilt.read('package.json'))
            self.assertEqual(manifest['module'], package['module'])
            self.assertEqual(manifest['module']['libraries']['linux-x86_64']['sha256'],
                             builder.digest(self.linux.read_bytes()))
            self.assertEqual(manifest['module']['libraries']['android-arm64-v8a']['sha256'],
                             builder.digest(self.android.read_bytes()))

    def test_rejects_tampered_reference(self) -> None:
        with self.reference.open('ab') as handle:
            handle.write(b'bad')
        with self.assertRaisesRegex(ValueError, 'audited original'):
            builder.build(self.reference, self.linux, self.android,
                          self.output, self.inventory)
        self.assertFalse(self.output.exists())

    def test_rejects_wrong_elf_architecture(self) -> None:
        self.android.write_bytes(make_elf(62, b'wrong'))
        with self.assertRaisesRegex(ValueError, 'Wrong ELF type/architecture'):
            builder.build(self.reference, self.linux, self.android,
                          self.output, self.inventory)
        self.assertFalse(self.output.exists())

    def test_refuses_to_overwrite_original(self) -> None:
        with self.assertRaisesRegex(ValueError, 'Refusing to overwrite'):
            builder.build(self.reference, self.linux, self.android,
                          self.reference, self.inventory)


if __name__ == '__main__':
    unittest.main()
