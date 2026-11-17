"""Host-only execution of the original C engine with all device I/O replaced."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Dot1qEngineTests(unittest.TestCase):
    def test_transaction_values_guards_and_failure_recovery(self):
        compiler = os.environ.get("CC", "cc")
        self.assertIsNotNone(shutil.which(compiler), "A host C compiler (cc) is required")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "simulator"
            compile_result = subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests/dot1q_simulator.c"), "-o", str(output)],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            result = subprocess.run([str(output)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("all checks passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
