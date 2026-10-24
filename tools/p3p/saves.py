#!/usr/bin/env python3
"""Read P3P base-build tagged saves; never modifies inputs.

Field mappings are derived from main build 663052C2C7E8C357. Results are
static-analysis findings, not a replacement for live menu validation.
"""
import argparse
import json
import struct
from pathlib import Path

BUILD = '663052C2C7E8C357BA6EAD36641EE2792397EB1F000000000000000000000000'
MAX_SAVE = 1024 * 1024


def records(data):
    if len(data) < 8 or len(data) > MAX_SAVE:
        raise ValueError('invalid save length')
    if struct.unpack_from('<I', data)[0] != 0:
        raise ValueError('unsupported save prefix')
    result = {}
    at = 4
    while at + 4 <= len(data):
        tag = struct.unpack_from('<I', data, at)[0]
        if tag == 0xffffffff:
            end = at + 4
            if any(data[end:]):
                raise ValueError('nonzero bytes after save terminator')
            return result, end
        if at + 8 > len(data):
            raise ValueError('truncated save record header')
        size = struct.unpack_from('<I', data, at + 4)[0]
        if tag in result or size > len(data) - at - 8:
            raise ValueError('duplicate or truncated save record')
        result[tag] = data[at + 8:at + 8 + size]
        at += 8 + size
    raise ValueError('missing save terminator')


def calendar(counter):
    # Transcription of 0x1e1e40: a 365-day cycle beginning April 1.
    day = counter % 365
    for month, days in ((4, 30), (5, 31), (6, 30), (7, 31), (8, 31),
                        (9, 30), (10, 31), (11, 30), (12, 31),
                        (1, 31), (2, 28), (3, 31)):
        if day < days:
            return f'{month:02d}-{day + 1:02d}'
        day -= days
    raise ValueError('invalid day counter')


def summarize(data):
    rows, end = records(data)

    def take(tag, size):
        value = rows[tag]
        if len(value) != size:
            raise ValueError(f'unexpected length for tag {tag:#x}')
        return value

    def scalar(tag, size, signed=False):
        return int.from_bytes(take(tag, size), 'little', signed=signed)

    unit = take(2, 0x50)
    selected = scalar(0x0e, 2, signed=True)
    stock = take(0x0f, 0x270)
    personas = []
    for index in range(12):
        record = stock[index * 0x34:(index + 1) * 0x34]
        if record[0] & 1:
            personas.append({'slot': index, 'id': struct.unpack_from('<H', record, 2)[0],
                             'level': record[4], 'selected': index == selected})
    # The active-stock getter rejects absent slots; preserve the same invariant.
    if selected not in {p['slot'] for p in personas}:
        raise ValueError('selected Persona slot is absent')
    links = take(3, 0x430)
    social = []
    for index in range(28):
        record = links[4 + index * 16:4 + (index + 1) * 16]
        link_id, rank = struct.unpack_from('<HH', record, 4)
        if link_id:
            social.append({'slot': index, 'id': link_id, 'rank': rank})
    counter = scalar(8, 2)
    return {'status': 'static_only_not_live_validated', 'record_count': len(rows),
            'stream_bytes': end, 'zero_padding_bytes': len(data) - end,
            'day_counter': counter, 'month_day': calendar(counter),
            'time_code': scalar(9, 1), 'money': scalar(0x12, 4),
            'protagonist': {'unit_id': struct.unpack_from('<H', unit, 2)[0],
                            'level': unit[6], 'hp': struct.unpack_from('<H', unit, 8)[0],
                            'sp': struct.unpack_from('<H', unit, 10)[0]},
            'selected_persona_slot': selected, 'personas': personas, 'social_links': social}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main-nso', required=True, type=Path,
                        help='require the executable build used to derive this layout')
    parser.add_argument('save', nargs='+', type=Path)
    args = parser.parse_args()
    with args.main_nso.open('rb') as stream:
        header = stream.read(96)
    if header[:4] != b'NSO0' or header[64:96].hex().upper() != BUILD:
        parser.error('main NSO does not match the researched build')
    for path in args.save:
        if path.stat().st_size > MAX_SAVE:
            parser.error(f'save too large: {path.name}')
        print(json.dumps({'file': path.name, **summarize(path.read_bytes())}))


if __name__ == '__main__':
    main()
