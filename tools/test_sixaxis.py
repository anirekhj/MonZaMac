#!/usr/bin/env python3
"""Compile and run authored SixAxis ABI fixtures; no controller or game needed."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SixAxisTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="monzamac-sixaxis-")
        cls.binary = Path(cls.temporary.name) / "sixaxis_test"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        result = subprocess.run(compiler + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                                str(ROOT / "tools/test_sixaxis.c"), "-lm", "-o", str(cls.binary)],
                               capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def check_scenario(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS 1 SixAxis scenario", result.stdout)

    def test_native_handles_match_operation_mode(self): self.check_scenario("handles")
    def test_start_stop_aggregate_and_reboot(self): self.check_scenario("lifecycle")
    def test_actual_samples_capacity_stale_and_invalid(self): self.check_scenario("states")
    def test_units_axes_matrix_and_upside_down(self): self.check_scenario("axes")
    def test_fusion_and_drift_preferences_roundtrip(self): self.check_scenario("controls")
    def test_parallel_handles_and_bounded_diagnostics(self): self.check_scenario("parallel")
    def test_hid_ipc_activation_shared_with_sdk_imports(self): self.check_scenario("ipc")
    def test_explicit_always_available_policy_and_reset(self): self.check_scenario("policy")


if __name__ == "__main__":
    unittest.main(verbosity=2)
