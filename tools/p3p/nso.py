#!/usr/bin/env python3
"""Decode and hash-check a local NSO; optionally apply local RELATIVE relocations.

The output omits zero-filled BSS. Imported symbols are deliberately unresolved.
"""
import argparse
import ctypes
import hashlib
import json
import struct
from pathlib import Path

LIMIT = 256 * 1024 * 1024


def decode(path, relocate=False):
    path = Path(path)
    if not 256 <= path.stat().st_size <= LIMIT:
        raise ValueError('invalid NSO size')
    data = path.read_bytes()
    if data[:4] != b'NSO0':
        raise ValueError('not an NSO')
    flags = struct.unpack_from('<I', data, 12)[0]
    segments = []
    metadata = []
    for index, name in enumerate(('text', 'rodata', 'data')):
        offset, address, size = struct.unpack_from('<III', data, 16 + index * 16)
        packed = struct.unpack_from('<I', data, 96 + index * 4)[0]
        stored_size = packed if flags & (1 << index) else size
        if address + size > LIMIT or offset + stored_size > len(data):
            raise ValueError('NSO segment exceeds bounds')
        raw = data[offset:offset + stored_size]
        if flags & (1 << index):
            lib = ctypes.CDLL('liblz4.so.1')
            fn = lib.LZ4_decompress_safe
            fn.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
            fn.restype = ctypes.c_int
            output = ctypes.create_string_buffer(size)
            if fn(raw, output, len(raw), size) != size:
                raise ValueError('NSO LZ4 decode failed')
            raw = output.raw
        digest = hashlib.sha256(raw).digest()
        checked = bool(flags & (1 << (index + 3)))
        if checked and digest != data[160 + index * 32:192 + index * 32]:
            raise ValueError(f'NSO {name} hash mismatch')
        if any(address < a + len(b) and a < address + size for a, b in segments):
            raise ValueError('overlapping NSO segments')
        segments.append((address, raw))
        metadata.append({'name': name, 'offset': address, 'size': size,
                         'sha256': digest.hex(), 'hash_verified': checked})
    image = bytearray(max(a + len(b) for a, b in segments))
    for address, raw in segments:
        image[address:address + len(raw)] = raw
    report = {'build_id': data[64:96].hex().upper(), 'segments': metadata,
              'bss_bytes': struct.unpack_from('<I', data, 60)[0],
              'relocations_applied': 0, 'unresolved_relocations': {}}
    if relocate:
        def read(fmt, at):
            if at < 0 or at + struct.calcsize(fmt) > len(image):
                raise ValueError('dynamic metadata outside initialized image')
            return struct.unpack_from(fmt, image, at)

        mod = read('<I', 4)[0]
        if image[mod:mod + 4] != b'MOD0':
            raise ValueError('missing MOD0 header')
        dynamic = mod + read('<i', mod + 4)[0]
        tags = {}
        for _ in range(4096):
            tag, value = read('<QQ', dynamic)
            dynamic += 16
            if tag == 0:
                break
            tags[tag] = value
        else:
            raise ValueError('unterminated dynamic table')
        if tags.get(9) != 24 or tags.get(8, 0) % 24:
            raise ValueError('invalid RELA layout')
        # Apply module-local relocations only; PLT imports stay unresolved too.
        for start, size in ((tags.get(7, 0), tags.get(8, 0)),
                            (tags.get(23, 0), tags.get(2, 0))):
            if size % 24:
                raise ValueError('invalid relocation table size')
            for at in range(start, start + size, 24):
                target, info, addend = read('<QQq', at)
                kind = info & 0xffffffff
                if kind == 1027:
                    if target + 8 > len(image) or addend < 0:
                        raise ValueError('RELATIVE relocation outside initialized image')
                    struct.pack_into('<Q', image, target, addend)
                    report['relocations_applied'] += 1
                else:
                    key = str(kind)
                    report['unresolved_relocations'][key] = report['unresolved_relocations'].get(key, 0) + 1
    return bytes(image), report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('nso', type=Path)
    parser.add_argument('--relocate', action='store_true')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if args.out and args.out.resolve().is_relative_to(Path(__file__).resolve().parents[2]):
        parser.error('game-derived output must be outside the repository')
    image, report = decode(args.nso, args.relocate)
    if args.out:
        args.out.write_bytes(image)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
