#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the authored capsule's supersampled/Lanczos edge coverage. No game input."""
from pathlib import Path
from PIL import Image, ImageDraw


def render():
    mask = Image.new('L', (2560, 376), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 2559, 375), radius=188, fill=255)
    mask = mask.resize((640, 94), Image.Resampling.LANCZOS)
    # Symmetric; beyond the leftmost 50 columns coverage is solid.
    assert all(mask.getpixel((x,y)) == mask.getpixel((639-x,y)) == mask.getpixel((x,93-y))
               for y in range(47) for x in range(640))
    assert all(mask.getpixel((x,y)) == 255 for y in range(94) for x in range(50,590))
    lines = ['// SPDX-FileCopyrightText: Copyright 2026 igawa6',
             '// SPDX-License-Identifier: GPL-3.0-or-later',
             '// Authored capsule coverage from gen_ui_geometry.py (no game files).',
             '#pragma once', '#include <cstdint>', 'namespace sw_ui_images {',
             'inline constexpr std::uint8_t kCapsuleEdge[47][50] = {']
    for y in range(47):
        lines.append('    {' + ','.join(str(mask.getpixel((x,y))) for x in range(50)) + '},')
    lines += ['};', '} // namespace sw_ui_images', '']
    return '\n'.join(lines)


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    out = Path(__file__).resolve().parents[2] / 'native/modules/sw_ui_geometry.h'
    if args.check:
        if out.read_text() != render(): raise SystemExit('capsule coverage is out of date')
    else:
        out.write_text(render())
