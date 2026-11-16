#!/usr/bin/env python3
"""CPU-only production-hook tests with authored guest regions; no game, GUI, save or network."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OfflineGiftHookTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="monzamac-gift-hook-")
        cls.binary = Path(cls.temporary.name) / "check"
        command = shlex.split(os.environ.get("CC", "cc"))
        command += ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread"]
        command += shlex.split(os.environ.get("TEST_CFLAGS", ""))
        command += [str(ROOT / "tools/test_offline_gifts.c"), str(ROOT / "runtime/offline_gift_card.c"),
                    str(ROOT / "runtime/offline_gift_catalog.c"), "-o", str(cls.binary)]
        subprocess.run(command, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def scenario(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS authored offline gift production-hook scenario", result.stdout)

    def test_build_offline_and_dynamic_thunk_guards(self): self.scenario("guards")
    def test_unmapped_and_cross_region_captures_fail_closed(self): self.scenario("invalid")
    def test_native_eligibility_polarity_grant_receipt_and_save_instruction(self): self.scenario("claim")
    def test_bag_box_and_unavailable_manager_preflight(self): self.scenario("preflight")
    def test_owned_clothing_checks_all_categories_and_boolean_low_bit(self): self.scenario("wardrobe-owned")
    def test_unready_or_unmapped_wardrobe_fails_closed(self): self.scenario("wardrobe-unavailable")
    def test_clothing_ownership_changed_in_picker_is_rechecked(self): self.scenario("wardrobe-recheck")
    def test_owned_unique_stones_still_allow_mission(self): self.scenario("unique")
    def test_claimed_after_selection_is_rechecked(self): self.scenario("recheck")
    def test_cancel_bad_index_and_unwind_never_grant(self): self.scenario("cancel")
    def test_parallel_picker_does_not_grant_or_enter_twice(self): self.scenario("concurrent")
    def test_native_receipt_failure_is_per_boot_and_reset_releases_pointers(self): self.scenario("reset")
    def test_all_gift_types_never_enter_native_save_from_callback(self): self.scenario("no-save")


if __name__ == "__main__":
    unittest.main(verbosity=2)
