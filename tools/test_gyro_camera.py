#!/usr/bin/env python3
"""Compile and run authored CPU-only gyro fixtures; no controller or game needed."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GyroCameraTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="monzamac-gyro-")
        cls.binary = Path(cls.temporary.name) / "gyro_camera_test"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(compiler + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(ROOT / "tools/test_gyro_camera.c"), str(ROOT / "runtime/hle/gyro_camera.c"),
                       "-lm", "-o", str(cls.binary)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def check_scenario(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS 1 gyro scenarios", result.stdout)

    def test_player_space_axes_and_gravity(self): self.check_scenario("axes")
    def test_sensitivity_inversion_saturation(self): self.check_scenario("sensitivity")
    def test_radial_deadzone(self): self.check_scenario("deadzone")
    def test_stationary_bias_and_manual_reset(self): self.check_scenario("calibration")
    def test_calibration_requires_quiet_inactive_continuous_reports(self): self.check_scenario("calibration-guards")
    def test_stale_duplicate_backward_and_future_times(self): self.check_scenario("timing")
    def test_release_menu_reconnect_cannot_replay_motion(self): self.check_scenario("release")
    def test_sensor_and_poll_rate_invariance(self): self.check_scenario("rates")
    def test_nonfinite_ranges_and_accel_loss(self): self.check_scenario("bad-data")


if __name__ == "__main__":
    unittest.main(verbosity=2)
