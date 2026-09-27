#!/usr/bin/env python3
"""Decode a Metroid Dread .bctex texture to PNG.

The container is trivial -- "MTXT", a short header, then gzip. What is inside is an NVN texture
(the descriptors say so: NvFD / NvBH), which means the pixels are stored block-linear rather than
row by row, the tiling Tegra hardware wants. Read linearly it looks like torn stripes; deswizzled
it is the artwork.

  bctex_decode.py <file.bctex> <out.png> [block_height]
"""
import gzip, pathlib, struct, sys


def gob_address(x, y, bpp, block_height, gobs_per_row):
    """Where pixel (x, y) actually sits in a block-linear image."""
    gob = ((y // (8 * block_height)) * 512 * block_height * gobs_per_row
           + (x * bpp // 64) * 512 * block_height
           + (y % (8 * block_height) // 8) * 512)
    xb = x * bpp
    return (gob + ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64
            + ((xb % 32) // 16) * 32 + (y % 2) * 16 + (xb % 16))


def deswizzle(data, width, height, bpp, block_height):
    out = bytearray(width * height * bpp)
    gobs_per_row = (width * bpp + 63) // 64
    for y in range(height):
        row = y * width * bpp
        for x in range(width):
            src = gob_address(x, y, bpp, block_height, gobs_per_row)
            if src + bpp <= len(data):
                out[row + x * bpp:row + x * bpp + bpp] = data[src:src + bpp]
    return bytes(out)


def load(path):
    d = pathlib.Path(path).read_bytes()
    assert d[:4] == b'MTXT', d[:4]
    raw = gzip.decompress(d[d.find(b'\x1f\x8b\x08'):])
    width, height = struct.unpack_from('<II', raw, 8)
    size = struct.unpack_from('<I', raw, 0xAC)[0]
    # +0x20 is where the pixels start. Taking them from the end of the buffer instead is off by a
    # GOB or two and shreds every sprite, which is exactly what it looks like.
    offset = struct.unpack_from('<I', raw, 0x20)[0]
    if offset + size > len(raw):
        offset = len(raw) - size
    return raw[offset:offset + size], width, height


def block_height_for(width, height, bpp, size):
    """The block height is not a free parameter and not worth guessing at.

    The hardware picks the largest that a texture of this height can use, capped at 16 -- so
    min(16, next power of two of height/8). It must also make the tiled image occupy exactly the
    bytes present; anything else over- or under-runs, which is how an eighth of a 32x32 glyph came
    out blank. Scoring candidates by how the picture "looks" is a trap: sharp artwork scores as
    noisy, and a tie between two valid heights is decided by luck."""
    gobs_per_row = (width * bpp + 63) // 64
    fits = lambda bh: gobs_per_row * 512 * bh * -(-height // (8 * bh)) == size
    wanted = 1
    while wanted < 16 and wanted * 8 < height:
        wanted *= 2
    return wanted if fits(wanted) else next((bh for bh in (16, 8, 4, 2, 1) if fits(bh)), 16)


if __name__ == '__main__':
    from PIL import Image
    body, w, h = load(sys.argv[1])
    bpp = len(body) // (w * h)
    if bpp == 4:
        best = int(sys.argv[3]) if len(sys.argv) > 3 else block_height_for(w, h, bpp, len(body))
        img = Image.frombytes('RGBA', (w, h), deswizzle(body, w, h, bpp, best))
        img.save(sys.argv[2])
        print(f'{w}x{h} bpp={bpp} block_height={best} -> {sys.argv[2]}')
        raise SystemExit(0)

    if bpp == 1:
        # A single byte per pixel is not compression, it is R8 -- which is what a font atlas is:
        # coverage, no colour. Reading the size as "must be block-compressed" cost a detour
        # through a BC7 decode that produced an empty image, which is exactly what decoding the
        # wrong format looks like.
        best = int(sys.argv[3]) if len(sys.argv) > 3 else block_height_for(w, h, bpp, len(body))
        Image.frombytes('L', (w, h), deswizzle(body, w, h, bpp, best)).save(sys.argv[2])
        print(f'{w}x{h} R8 block_height={best} -> {sys.argv[2]}')
        raise SystemExit(0)

    # Block-compressed. The tiling works on 4x4 blocks rather than pixels, so the deswizzle runs
    # over a grid a quarter the size in each direction with the block as its unit -- eight bytes
    # for BC4, sixteen for the rest. Dread's font atlas is BC4: one channel, which is all a glyph
    # mask needs.
    bw, bh_blocks = (w + 3) // 4, (h + 3) // 4
    block_bytes = len(body) // (bw * bh_blocks)
    if block_bytes not in (8, 16):
        raise SystemExit(f'{w}x{h}: {block_bytes} bytes per 4x4 block is not a BC format')
    best = int(sys.argv[3]) if len(sys.argv) > 3 else \
        block_height_for(bw, bh_blocks, block_bytes, len(body))
    blocks = deswizzle(body, bw, bh_blocks, block_bytes, best)

    if block_bytes == 16:
        # Sixteen-byte blocks are BC2/3/5/7, and decoding those in Python is a project in itself.
        # Eden already ships a decoder for them, so write the deswizzled blocks out and let the
        # C++ one do the part it is good at.
        out = pathlib.Path(sys.argv[2]).with_suffix('.blocks')
        out.write_bytes(blocks)
        print(f'{w}x{h} {block_bytes}-byte blocks, block_height={best} -> {out} '
              f'({bw}x{bh_blocks} blocks)')
        raise SystemExit(0)
    if block_bytes != 8:
        raise SystemExit(f'{w}x{h}: {block_bytes} bytes per block is not a BC format')
    # BC4: each block is a min, a max, and three-bit selectors picking between them.
    out = bytearray(w * h)
    for by in range(bh_blocks):
        for bx in range(bw):
            o = (by * bw + bx) * 8
            c0, c1 = blocks[o], blocks[o + 1]
            bits = int.from_bytes(blocks[o + 2:o + 8], 'little')
            if c0 > c1:
                table = [c0, c1] + [((7 - i) * c0 + (1 + i) * c1) // 7 for i in range(6)]
            else:
                table = [c0, c1] + [((5 - i) * c0 + (1 + i) * c1) // 5 for i in range(4)] + [0, 255]
            for i in range(16):
                px, py = bx * 4 + (i % 4), by * 4 + (i // 4)
                if px < w and py < h:
                    out[py * w + px] = table[(bits >> (3 * i)) & 7]
    Image.frombytes('L', (w, h), bytes(out)).save(sys.argv[2])
    print(f'{w}x{h} BC4 block_height={best} -> {sys.argv[2]}')
