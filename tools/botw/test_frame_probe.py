"""Guard the tail-call ABI and the readback decoder used by the live probe."""
import struct
import tempfile
import unittest
from pathlib import Path

from build_frame_probe import SITE, NATIVE_UI_UPDATE, branch_word, build_plan
from record_frame_probe import branch_target


class FrameProbeTests(unittest.TestCase):
    def test_original_tail_branch(self):
        # Captured instruction from main+E79530, independent of the encoder.
        original = struct.unpack("<I", bytes.fromhex("6fb4ff17"))[0]
        self.assertEqual(branch_target(original, SITE, False), NATIVE_UI_UPDATE)
        with self.assertRaises(ValueError):
            branch_target(original, SITE, True)

    def test_call_cannot_pass_as_tail_branch(self):
        with self.assertRaises(ValueError):
            branch_target(0x94000000, SITE, False)

    def test_signed_branch_boundaries(self):
        for delta in (-(1 << 27), -4, 0, (1 << 27) - 4):
            for link in (False, True):
                word = struct.unpack("<I", branch_word(0x90000000, 0x90000000 + delta, link))[0]
                self.assertEqual(branch_target(word, 0x90000000, link), 0x90000000 + delta)
        for delta in (-(1 << 27) - 4, (1 << 27), 1):
            with self.assertRaises(ValueError):
                branch_word(0x90000000, 0x90000000 + delta)

    def test_assembled_probe_matches_readback_contract(self):
        with tempfile.TemporaryDirectory() as work:
            plan = build_plan(Path(work))
        patch, = plan["patches"]
        self.assertEqual(patch["expected"], "6fb4ff17")
        self.assertEqual(patch["replacement"], "00000014")
        code = bytes.fromhex(plan["guest_code"]["bytes"])
        self.assertEqual(len(code), 92)
        self.assertEqual(struct.unpack_from("<I", code, 60)[0], 0x94000000)
        self.assertEqual(struct.unpack_from("<I", code, 88)[0], 0xD65F03C0)  # ret x30
        self.assertEqual(plan["guest_code"]["relocations"], [
            dict(offset=12, kind="aarch64_adrp", target="mailbox", addend=0),
            dict(offset=16, kind="aarch64_add_lo12", target="mailbox", addend=0),
            dict(offset=60, kind="aarch64_branch26", target="main", addend=NATIVE_UI_UPDATE),
        ])


if __name__ == "__main__":
    unittest.main()
