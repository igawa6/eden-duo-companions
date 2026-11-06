#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 AlterDigga
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/cotl/gen_ui_art.py (python3 tools/cotl/test_gen_ui_art.py)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_ui_art as art  # noqa: E402


class GenUiArtTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        art.write_all(cls.dir)

    def load(self, name):
        return art.read_png(os.path.join(self.dir, name + ".png"))

    def test_png_roundtrip(self):
        px = [[(255, 0, 0, 255), (0, 0, 0, 0)], [(1, 2, 3, 4), (9, 8, 7, 6)]]
        path = os.path.join(self.dir, "rt.png")
        art.write_png(path, px)
        self.assertEqual(art.read_png(path), px)

    def test_frame_is_9slice_size(self):
        px = self.load("frame_gold")
        self.assertEqual((len(px[0]), len(px)), (art.FRAME, art.FRAME))
        self.assertEqual(px[0][0], art.rgba(art.GOLD))
        self.assertEqual(px[art.FRAME // 2][art.FRAME // 2], art.rgba(art.CARD))
        # thin inner line
        self.assertEqual(px[art.FRAME // 2][art.INNER], art.rgba(art.GOLD_LINE))

    def test_selected_frame_has_thicker_border(self):
        px = self.load("frame_gold_sel")
        self.assertEqual(px[3][art.FRAME // 2], art.rgba(art.GOLD))
        self.assertNotEqual(self.load("frame_gold")[3][art.FRAME // 2], art.rgba(art.GOLD))

    def test_line_frame_is_white_with_clear_centre(self):
        px = self.load("frame_line")
        self.assertEqual(px[0][0], (255, 255, 255, 255))
        self.assertEqual(px[art.FRAME // 2][art.FRAME // 2][3], 0)

    def test_icons_white_on_transparent(self):
        for name in art.ICONS:
            px = self.load("icon_" + name)
            self.assertEqual(len(px), 7 * art.ICON_SCALE, name)
            self.assertEqual(len(px[0]), 7 * art.ICON_SCALE, name)
            colors = {p for row in px for p in row}
            self.assertLessEqual(colors, {(255, 255, 255, 255), (0, 0, 0, 0)}, name)
            self.assertIn((255, 255, 255, 255), colors, name)

    def test_empty_portrait_is_faint_silhouette(self):
        px = self.load("portrait_empty")
        self.assertIn(art.rgba(art.FAINT), {p for row in px for p in row})

    def test_diamond_is_red(self):
        px = self.load("diamond")
        mid = len(px) // 2
        self.assertEqual(px[mid][mid], art.rgba(art.RED))
        self.assertEqual(px[0][0][3], 0)


if __name__ == "__main__":
    unittest.main()
