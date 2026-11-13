#!/usr/bin/env python3
"""Evidence provenance failures must reject recorded memory probes."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from verify_update_probes import Probes, target_flag


class ProbeProvenanceTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="botw-probe-test-")
        self.addCleanup(self.scratch.cleanup)
        self.directory = Path(self.scratch.name)

    def fixture(self, name="first", **changes):
        data = b"recorded bytes"
        metadata = dict(game="breath-of-the-wild", title_id="01007EF00011E000",
                        label="botw-190", run_id="boot-a",
                        port=6599, game_build_id="update-190", addr="0x12340000",
                        length=len(data), sha256=hashlib.sha256(data).hexdigest())
        metadata.update(changes)
        (self.directory / (name + ".bin")).write_bytes(data)
        (self.directory / (name + ".bin.json")).write_text(json.dumps(metadata))

    def test_hash_validated_before_use(self):
        self.fixture()
        probes = Probes(self.directory)
        self.assertEqual(probes.read("first"), (0x12340000, b"recorded bytes"))
        self.assertIn("first", probes.used)

    def test_changed_bytes_rejected(self):
        self.fixture()
        (self.directory / "first.bin").write_bytes(b"recorded bytEs")
        with self.assertRaisesRegex(ValueError, "length/hash mismatch"):
            Probes(self.directory).read("first")

    def test_truncation_rejected(self):
        self.fixture(length=999)
        with self.assertRaisesRegex(ValueError, "length/hash mismatch"):
            Probes(self.directory).read("first")

    def test_cross_session_probes_rejected(self):
        self.fixture()
        for field, value in (("game", "another-game"), ("title_id", "0000000000000000"),
                             ("label", "another-label"),
                             ("run_id", "boot-b"), ("port", 6600),
                             ("game_build_id", "native-holding-160")):
            with self.subTest(field=field):
                self.fixture("second", **{field: value})
                probes = Probes(self.directory)
                probes.read("first")
                with self.assertRaisesRegex(ValueError, "mixed harness sessions"):
                    probes.read("second")
                self.assertNotIn("second", probes.used)


class TargetFlagTest(unittest.TestCase):
    def record(self, value, kind="i", maximum=999999, flag_type=1):
        return struct.pack("<QBBH" + kind * 4 + "IQ", 0x12340000, flag_type, 0, 0,
                           0, 0, maximum, value, 0x23149BF8, 0)

    def test_currency_contract(self):
        self.assertEqual(target_flag(self.record(347549), 0, "i", 0x12000000)["value"], 347549)

    def test_rehashed_out_of_range_value_rejected(self):
        with self.assertRaisesRegex(ValueError, "invalid type, bounds or value"):
            target_flag(self.record(9999999), 0, "i", 0x12000000)

    def test_changed_type_or_bounds_rejected(self):
        for data in (self.record(100, flag_type=2), self.record(9999999, maximum=99999999)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                target_flag(data, 0, "i", 0x12000000)

    def test_nonfinite_clock_rejected(self):
        with self.assertRaises(ValueError):
            target_flag(self.record(float("nan"), "f", 360.0, 2), 0, "f", 0x12000000)


if __name__ == "__main__":
    unittest.main()
