"""Bounded startup-package/name-table decoders; never embeds game text."""
import struct


def pak(data, counted=False):
    result = {}
    at = 4 if counted else 0
    count = struct.unpack_from('<I', data)[0] if counted and len(data) >= 4 else None
    if counted and (count is None or count > 4096):
        raise ValueError('invalid PAK member count')
    while at < len(data) and (count is None or len(result) < count):
        width = 32 if counted else 252
        if not any(data[at:]) and not counted:
            break
        if at + width + 4 > len(data):
            raise ValueError('truncated PAK header')
        raw = data[at:at + width]
        if b'\0' not in raw:
            raise ValueError('unterminated PAK name')
        name = raw.split(b'\0')[0].decode('ascii')
        size = struct.unpack_from('<I', data, at + width)[0]
        start = at + width + 4
        if not name or name in result or size > len(data) - start:
            raise ValueError('invalid PAK member')
        result[name] = data[start:start + size]
        at = start + size
        if not counted:
            at = (at + 63) & ~63
    if counted and len(result) != count:
        raise ValueError('incomplete PAK')
    return result


def strings(data, width):
    if not width or len(data) % width:
        raise ValueError('partial string row')
    result = []
    for at in range(0, len(data), width):
        row = data[at:at + width]
        if b'\0' not in row:
            raise ValueError('unterminated string row')
        # English startup tables are UTF-8/ASCII. Refuse unknown encodings.
        text = row.split(b'\0')[0].decode('utf-8')
        if any(ord(c) < 32 for c in text):
            raise ValueError('control character in name')
        result.append(text)
    return result


def persona_names(msg):
    at = 0
    sizes = (672, 11856, 6384, 4352)
    for index, expected in enumerate(sizes):
        if at + 4 > len(msg):
            raise ValueError('truncated MSG table')
        size = struct.unpack_from('<I', msg, at)[0]
        if size != expected or size > len(msg) - at - 4:
            raise ValueError('unsupported MSG table layout')
        if index == 3:
            return strings(msg[at + 4:at + 4 + size], 17)
        at = (at + 4 + size + 15) & ~15
    raise ValueError('missing Persona names')


def social_names(table):
    if len(table) != 2080 or struct.unpack_from('<II', table) != (2048, 32):
        raise ValueError('unsupported Social Link names layout')
    return strings(table[8:2056], 64)
