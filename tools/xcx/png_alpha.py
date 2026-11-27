#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""png_alpha.py IN.png OUT.a8 -- the alpha channel of a PNG as a raw mask file (u32 w, u32 h, w*h bytes).

The Skell still test (dsmod-xcx-still, XCX_STILL_REFERENCE_A8) compares its silhouette with the
offline Python render's. The PNG itself stays outside the repository; this only extracts alpha.
Needs Pillow."""
import struct
import sys

from PIL import Image

if len(sys.argv) != 3:
    sys.exit(__doc__)
im = Image.open(sys.argv[1]).convert("RGBA")
with open(sys.argv[2], "wb") as f:
    f.write(struct.pack("<II", im.width, im.height))
    f.write(im.getchannel("A").tobytes())
