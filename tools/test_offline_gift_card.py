#!/usr/bin/env python3
"""CPU-only synthetic card layout checks; no game, event-card, network or save input."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OfflineGiftCardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="monzamac-gift-card-")
        cls.binary = Path(cls.temporary.name) / "check"
        command = shlex.split(os.environ.get("CC", "cc"))
        command += ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic"]
        command += shlex.split(os.environ.get("TEST_CFLAGS", ""))
        command += [str(ROOT / "tools/test_offline_gift_card.c"), str(ROOT / "runtime/offline_gift_card.c"),
                    str(ROOT / "runtime/offline_gift_catalog.c"), "-o", str(cls.binary)]
        subprocess.run(command, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def scenario(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS authored offline gift card scenario", result.stdout)

    def test_item_bytes_and_output_canary(self): self.scenario("items")
    def test_mission_hash_name_and_item_offsets(self): self.scenario("mission")
    def test_clothing_six_u32_fields(self): self.scenario("clothing")
    def test_pokemon_bytes_defaults_and_sentinels(self): self.scenario("pokemon")
    def test_spewpa_variation_cycle_and_only_form_changes(self): self.scenario("variations")
    def test_invalid_inputs_never_modify_output(self): self.scenario("invalid")
    def test_all_local_catalog_entries_build(self): self.scenario("catalog")


if __name__ == "__main__":
    unittest.main(verbosity=2)
