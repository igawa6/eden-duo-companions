#!/usr/bin/env python3
"""Inspect CPK tables or extract explicitly selected entries from a local dump.

No game files are bundled. Outputs must be kept outside the repository.
"""
import argparse
import json
import struct
from pathlib import Path

LIMIT = 64 * 1024 * 1024


def utf(stream, offset):
    stream.seek(offset)
    header = stream.read(8)
    if len(header) != 8 or header[:4] != b'@UTF':
        raise ValueError('missing @UTF table')
    size = struct.unpack_from('>I', header, 4)[0] + 8
    if not 32 <= size <= LIMIT:
        raise ValueError('invalid UTF table size')
    data = header + stream.read(size - 8)
    if len(data) != size:
        raise ValueError('truncated UTF table')
    rows, strings, blobs = (n + 8 for n in struct.unpack_from('>III', data, 8))
    columns, stride, count = struct.unpack_from('>HHI', data, 24)
    if not 32 <= rows <= strings <= blobs <= size or rows + stride * count > strings:
        raise ValueError('invalid UTF regions')

    def string(relative):
        start = strings + relative
        if not strings <= start < blobs:
            raise ValueError('invalid UTF string offset')
        raw = data[start:data.index(b'\0', start, blobs)]
        try:
            return raw.decode('utf-8')
        except UnicodeDecodeError:
            return raw.decode('cp932')

    def value(kind, at, end):
        formats = {0: 'B', 1: 'b', 2: 'H', 3: 'h', 4: 'I', 5: 'i',
                   6: 'Q', 7: 'q', 8: 'f', 10: 'I', 11: 'II'}
        if kind not in formats:
            raise ValueError('unsupported UTF value kind')
        fmt = '>' + formats[kind]
        width = struct.calcsize(fmt)
        if at + width > end:
            raise ValueError('UTF value crosses region')
        values = struct.unpack_from(fmt, data, at)
        result = values if kind == 11 else values[0]
        if kind == 10:
            result = string(result)
        return result, width

    definitions = []
    at = 32
    for _ in range(columns):
        if at + 5 > rows:
            raise ValueError('truncated UTF column')
        flags = data[at]
        name = string(struct.unpack_from('>I', data, at + 1)[0])
        storage, kind = flags & 0xf0, flags & 0x0f
        at += 5
        constant = 0
        if storage == 0x30:
            constant, width = value(kind, at, rows)
            at += width
        elif storage not in (0x10, 0x50):
            raise ValueError('unsupported UTF storage')
        definitions.append((name, storage, kind, constant))
    output = []
    for index in range(count):
        at = rows + index * stride
        end = at + stride
        row = {}
        for name, storage, kind, constant in definitions:
            result = constant
            if storage == 0x50:
                result, width = value(kind, at, end)
                at += width
            row[name] = result
        if at != end:
            raise ValueError('UTF row width mismatch')
        output.append(row)
    return output


def crilayla(data, expected):
    if len(data) < 0x110 or data[:8] != b'CRILAYLA':
        raise ValueError('invalid CRILAYLA header')
    body, packed = struct.unpack_from('<II', data, 8)
    if body + 256 != expected or expected > LIMIT or 16 + packed + 256 != len(data):
        raise ValueError('invalid CRILAYLA sizes')
    output = bytearray(expected)
    output[:256] = data[16 + packed:]
    pos, pool, remaining = 15 + packed, 0, 0

    def bits(width):
        nonlocal pos, pool, remaining
        value = 0
        while width:
            if not remaining:
                if pos < 16:
                    raise ValueError('truncated CRILAYLA bits')
                pool = data[pos]
                pos -= 1
                remaining = 8
            take = min(width, remaining)
            value = (value << take) | ((pool >> (remaining - take)) & ((1 << take) - 1))
            remaining -= take
            width -= take
        return value

    produced = 0
    while produced < body:
        dst = expected - 1 - produced
        if not bits(1):
            output[dst] = bits(8)
            produced += 1
            continue
        distance = bits(13) + 3
        length = 3
        group = 0
        while True:
            width = (2, 3, 5)[group] if group < 3 else 8
            group += 1
            extra = bits(width)
            length += extra
            if extra != (1 << width) - 1:
                break
        if dst + distance >= expected:
            raise ValueError('CRILAYLA reference outside output')
        length = min(length, body - produced)
        for index in range(length):
            output[dst - index] = output[dst - index + distance]
        produced += length
    return bytes(output)


class Cpk:
    def __init__(self, path):
        self.path = Path(path)
        self.size = self.path.stat().st_size
        with self.path.open('rb') as stream:
            if stream.read(4) != b'CPK ':
                raise ValueError('not a CPK archive')
            self.header = utf(stream, 16)[0]
            toc = self.header['TocOffset']
            self.entries = utf(stream, toc + 16)
        if len(self.entries) != self.header['Files']:
            raise ValueError('CPK entry count mismatch')
        self.base = min(toc, self.header['ContentOffset'])
        self.named = {}
        for entry in self.entries:
            name = '/'.join(filter(None, (entry['DirName'], entry['FileName'])))
            if name in self.named:
                raise ValueError('duplicate archive path')
            self.named[name] = entry

    def read(self, name):
        entry = self.named[name]
        offset = self.base + entry['FileOffset']
        size, expected = entry['FileSize'], entry['ExtractSize']
        if min(size, expected) < 0 or max(size, expected) > LIMIT or offset < 0 or offset + size > self.size:
            raise ValueError('invalid or oversized CPK entry')
        with self.path.open('rb') as stream:
            stream.seek(offset)
            data = stream.read(size)
        if len(data) != size:
            raise ValueError('truncated CPK entry')
        if size != expected:
            data = crilayla(data, expected)
        return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--inventory', type=Path, help='write TOC metadata JSON')
    parser.add_argument('--entry', action='append', default=[], help='exact archive path to extract')
    parser.add_argument('--out', type=Path, help='destination outside the repository')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    for dest in (args.inventory, args.out):
        if dest is not None and dest.resolve().is_relative_to(repo):
            parser.error('game-derived outputs must be outside the repository')
    if args.entry and args.out is None:
        parser.error('--entry requires --out')
    archive = Cpk(args.archive)
    print(json.dumps({'archive': str(args.archive), 'entries': len(archive.entries)}))
    if args.inventory:
        args.inventory.write_text(json.dumps(archive.entries, indent=2) + '\n')
    for name in args.entry:
        path = args.out / name
        if not path.resolve().is_relative_to(args.out.resolve()):
            parser.error('entry path escapes output directory')
        data = archive.read(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(json.dumps({'entry': name, 'decoded_bytes': len(data)}))


if __name__ == '__main__':
    main()
