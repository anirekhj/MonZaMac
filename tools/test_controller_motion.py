#!/usr/bin/env python3
"""Portable authored inertial fixtures; no SDL, controller or game initialization."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ControllerMotionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="monzamac-native-motion-")
        cls.binary = Path(cls.temporary.name) / "controller_motion"
        command = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(command + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                  str(ROOT / "tools/test_controller_motion.c"),
                                  str(ROOT / "runtime/hle/controller_motion.c"), "-lm", "-o", str(cls.binary)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def scenario(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS native motion math", result.stdout)

    def test_upright_inverted_flat_and_tilted_gravity(self): self.scenario("tilt")
    def test_gyro_integration_and_report_rate_invariance(self): self.scenario("spin")
    def test_fusion_accepts_either_paired_sensor_event_order(self): self.scenario("paired")
    def test_polling_is_pure_and_channels_expire(self): self.scenario("polling")
    def test_invalid_duplicate_backward_and_future_reports(self): self.scenario("bad-data")
    def test_stalls_linear_acceleration_and_accel_only_tilt(self): self.scenario("gaps")


if __name__ == "__main__":
    unittest.main(verbosity=2)
