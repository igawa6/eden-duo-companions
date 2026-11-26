#!/usr/bin/env python3
"""Validate the checked-in P3P 1.1.0 package without external binaries."""
from __future__ import annotations
import hashlib
import json
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/"tools/p3p/layout_v1_1.json"
PACKAGE=ROOT/"packages/Persona3Portable"
IMAGES=PACKAGE/"dualscreen/img"
ORIGINAL_REPLACEMENTS={
    "bg_start_0.png", "bg_start_1.png", "word_battle.png", "word_map.png",
    "word_persona3.png", "word_portable.png", "word_talk.png"
}


def walk_images(node):
    if isinstance(node,dict):
        for value in node.values():
            yield from walk_images(value)
    elif isinstance(node,list):
        for value in node:
            yield from walk_images(value)
    elif isinstance(node,str) and node.startswith("file:img/"):
        yield node.removeprefix("file:img/")


class SourcePackageTests(unittest.TestCase):
    def test_single_manifest_source(self):
        original=json.loads(SOURCE.read_text(encoding="utf-8"))
        packed=json.loads((PACKAGE/"dualscreen/manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(original,packed)
        self.assertEqual((len(packed["pages"]),len(packed["actions"]),
                          len(packed["module_outputs"])),(12,61,2798))
        self.assertEqual(packed["module"]["libraries"],{})
        self.assertEqual(packed["title_id"],"0100DCD01525A000")

    def test_generated_art_inventory_and_references(self):
        m=json.loads(SOURCE.read_text(encoding="utf-8"))
        files={f.name for f in IMAGES.iterdir() if f.suffix.lower()==".png"}
        self.assertEqual(len(files),62)
        self.assertTrue(ORIGINAL_REPLACEMENTS.issubset(files))
        self.assertFalse(set(walk_images(m))-files)
        for name in files:
            with (IMAGES/name).open("rb") as fh:
                self.assertEqual(fh.read(8),b"\x89PNG\r\n\x1a\n")

    def test_approved_ayn_thor_start_titles(self):
        # SHA-256 of the exact two PNGs installed and approved on hardware.
        expected = {
            "word_persona3.png":
                "18b7243c0ab9797f44c3afdc7e78cb69df0e5f5c70538e2e6808c2a642f5ef0f",
            "word_portable.png":
                "d484df3348b1915ff1c18c774cf4878f508bab6109e8978f10a1f7dadaa5a183",
        }
        for name, sha in expected.items():
            with self.subTest(name=name):
                self.assertEqual(hashlib.sha256((IMAGES/name).read_bytes()).hexdigest(), sha)

    def test_previews_are_explicitly_labeled(self):
        readme=(PACKAGE/"README.md").read_text(encoding="utf-8")
        self.assertIn("illustrative preview",readme.lower())
        for name in ("male-route-preview.png","female-route-preview.png"):
            path=PACKAGE/"docs"/name
            self.assertTrue(path.is_file(),str(path))


if __name__=="__main__":
    unittest.main()
