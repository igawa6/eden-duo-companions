#!/usr/bin/env python3
"""Locally rebuild the original P3P v1.8.10 UI with user-supplied source-built modules.

The 62 original PNGs are read from an explicitly provided, pinned community
archive, NOT embedded in the repository. This is a local testing bridge and
NOT a license to redistribute third-party assets or a fully standalone build.
"""
from __future__ import annotations
import argparse
import copy
import csv
import hashlib
import json
import os
import struct
import zipfile
from pathlib import Path

ARCHIVE_SHA = '70faf618f1fd4777c78d5204688b5427e80eeb0e57c2d16ec30e9b02ed3e4513'
MANIFEST_SHA = 'd9079a3852e24a24c07608ccfcb32b0b4c9c1283476650204e2a72cb30c9e379'
MODULES = {
    'linux-x86_64': ('dualscreen/modules/linux-x86_64/0100DCD01525A000.so', 62),
    'android-arm64-v8a': ('dualscreen/modules/android-arm64-v8a/0100DCD01525A000.so', 183),
}

def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def validate_elf(binary: bytes, machine: int) -> None:
    if len(binary) < 64 or binary[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Module must be a 64-bit little-endian ELF object')
    actual_type, actual_machine = struct.unpack_from('<HH', binary, 16)
    if (actual_type, actual_machine) != (3, machine):
        raise ValueError(f'Wrong ELF type/architecture: {actual_type}/{actual_machine}, expected 3/{machine}')

def verify_reference(archive: Path, inventory_csv: Path) -> None:
    if digest(archive.read_bytes()) != ARCHIVE_SHA:
        raise ValueError('Input must be exactly the audited original v1.8.10 FINAL ZIP')
    with inventory_csv.open(newline='', encoding='utf-8') as f:
        inventory = {r['asset']:r for r in csv.DictReader(f)}
    if len(inventory) != 62:
        raise ValueError('Expected the 62-entry source provenance inventory')
    with zipfile.ZipFile(archive) as z:
        names = z.namelist()
        if len(names) != len(set(names)) or z.testzip():
            raise ValueError('Duplicate or corrupt ZIP members')
        if set(n for n in names if n.endswith('.png')) != set(inventory):
            raise ValueError('Artwork count/names differ from original inventory')
        if digest(z.read('dualscreen/manifest.json')) != MANIFEST_SHA:
            raise ValueError('Original UI manifest differs from canonical SHA')
        for name, row in inventory.items():
            data = z.read(name)
            if len(data) != int(row['size_bytes']) or digest(data) != row['sha256']:
                raise ValueError(f'Original artwork hash mismatch: {name}')
        manifest = json.loads(z.read('dualscreen/manifest.json'))
        package = json.loads(z.read('package.json'))
        if manifest['title_id'] != '0100DCD01525A000':
            raise ValueError('Wrong title ID')
        if manifest['module'] != package['module']:
            raise ValueError('Original manifest/package module mismatch')
        for platform, (path, machine) in MODULES.items():
            entry = manifest['module']['libraries'][platform]
            if entry['path'] != path.removeprefix('dualscreen/'):
                raise ValueError(f'Unexpected module path: {platform}')
            binary = z.read(path)
            validate_elf(binary, machine)
            if digest(binary) != entry['sha256']:
                raise ValueError(f'Original module digest mismatch: {platform}')

def patch_json_hashes(original: bytes, replacements: dict[str, str]) -> bytes:
    output = original
    for old, new in replacements.items():
        oldb, newb = old.encode('ascii'), new.encode('ascii')
        if len(oldb) != 64 or len(newb) != 64 or output.count(oldb) != 1:
            raise ValueError('Expected exactly one matching original binary digest')
        output = output.replace(oldb, newb)
    return output

def build(reference: Path, linux: Path, android: Path, output: Path, inventory: Path) -> None:
    if reference.resolve() == output.resolve():
        raise ValueError('Refusing to overwrite the original reference ZIP')
    verify_reference(reference, inventory)
    fresh = {'linux-x86_64': linux.read_bytes(), 'android-arm64-v8a': android.read_bytes()}
    for platform, data in fresh.items():
        validate_elf(data, MODULES[platform][1])
    with zipfile.ZipFile(reference) as z:
        manifest = json.loads(z.read('dualscreen/manifest.json'))
        replacements = {manifest['module']['libraries'][k]['sha256']: digest(value)
                        for k,value in fresh.items()}
        if len(replacements) != 2:
            raise ValueError('Original module hashes are not distinct')
        new_manifest = patch_json_hashes(z.read('dualscreen/manifest.json'), replacements)
        new_package = patch_json_hashes(z.read('package.json'), replacements)
        if json.loads(new_manifest)['module'] != json.loads(new_package)['module']:
            raise ValueError('Inconsistent output binary declarations')
        output.parent.mkdir(parents=True, exist_ok=True)
        temp = output.with_name(output.name + '.partial')
        try:
            with zipfile.ZipFile(temp, 'w') as new_zip:
                for info in z.infolist():
                    name = info.filename
                    if name == 'dualscreen/manifest.json':
                        data = new_manifest
                    elif name == 'package.json':
                        data = new_package
                    else:
                        platform = next((k for k, (path, _) in MODULES.items() if path == name), None)
                        data = fresh[platform] if platform else z.read(name)
                    new_zip.writestr(copy.copy(info), data)
            with zipfile.ZipFile(temp) as built:
                if built.testzip():
                    raise ValueError('Repacked ZIP CRC failure')
                if len(built.namelist()) != 67:
                    raise ValueError('Unexpected ZIP entry count')
                for path in z.namelist():
                    if path.endswith('.png') and built.read(path) != z.read(path):
                        raise ValueError(f'Original artwork changed: {path}')
                if json.loads(built.read('dualscreen/manifest.json'))['module'] != json.loads(built.read('package.json'))['module']:
                    raise ValueError('Output package and manifest modules differ')
            os.replace(temp, output)
        finally:
            if temp.exists():
                temp.unlink()

def main() -> None:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True, help='your audited v1.8.10 original ZIP')
    parser.add_argument('--linux-module', type=Path, required=True, help='locally compiled x86-64 .so')
    parser.add_argument('--android-module', type=Path, required=True, help='locally compiled arm64-v8a .so')
    parser.add_argument('--output', type=Path, required=True, help='new local test-only .dsmod.zip')
    parser.add_argument('--inventory', type=Path, default=Path(__file__).with_name('ASSET_PROVENANCE.csv'))
    args=parser.parse_args()
    build(args.reference,args.linux_module,args.android_module,args.output,args.inventory)
    print('Created validated v1.8.10 local source-module test package:',args.output)

if __name__ == '__main__':
    main()
