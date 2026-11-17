#!/usr/bin/env python3
"""Offline tests using synthetic operations only; no vendor firmware data."""
import subprocess
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PARSER = ROOT / "scripts" / "inspect-esconfig-switch-profile.py"


class EsconfigProfileParserTest(unittest.TestCase):
    def test_vlan_entry_members_and_untag(self):
        # A synthetic 40-byte operation with the published mode-0 value.
        record = struct.pack(
            "<10I", 1, 1, 0x05830002, 0,
            0x00221F0F, 0, 0, 0, 0, 0
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample"
            path.write_bytes(record)
            result = subprocess.run(
                [
                    sys.executable, str(PARSER), "--elf", str(path),
                    "--offset", "0", "--count", "1"
                ],
                capture_output=True, text=True, check=True
            )
        self.assertIn("page=0x05 reg=0x83 bits=32", result.stdout)
        self.assertIn("members=0x10f", result.stdout)
        self.assertIn("untag=0x10f", result.stdout)
        self.assertIn("additional_bits=0x00200000", result.stdout)

    def test_rejects_out_of_range_record(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample"
            path.write_bytes(b"")
            result = subprocess.run(
                [
                    sys.executable, str(PARSER), "--elf", str(path),
                    "--offset", "0", "--count", "1"
                ],
                capture_output=True, text=True
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exceeds ELF file size", result.stderr)


if __name__ == "__main__":
    unittest.main()
