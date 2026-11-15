#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the generated Handbook badge's positive gate and scrolling repeat."""
import json
from pathlib import Path
import unittest


class HandbookBadgeTest(unittest.TestCase):
    def test_native_badge_visibility(self):
        root = Path(__file__).resolve().parents[2]
        manifest = json.loads((root / "packages/MyTimeAtSandrock/dualscreen/manifest.json").read_text())
        page = next(p for p in manifest["pages"] if p["id"] == "handbook")
        badges = [w for w in page["widgets"] if "HbNewBadge" in w.get("src", "")]
        self.assertEqual(len(badges), 1)
        badge = badges[0]
        self.assertEqual(badge["type"], "image")
        self.assertEqual(badge["need_bind"], "bp{i}.fresh")
        self.assertEqual(badge["repeat_bind"], "hb.count")
        self.assertEqual(badge["repeat"], 512)
        self.assertEqual(badge["scroll"], "mtas.handbook")
        self.assertEqual(badge["rect"][2:], [32, 32])


if __name__ == "__main__":
    unittest.main()
