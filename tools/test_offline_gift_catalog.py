#!/usr/bin/env python3
"""CPU-only authored reward metadata checks; no game, network or save access."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OfflineGiftCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='monzamac-gift-catalog-')
        cls.binary = Path(cls.temporary.name) / 'check'
        command = shlex.split(os.environ.get('CC', 'cc'))
        command += ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic']
        command += shlex.split(os.environ.get('TEST_CFLAGS', ''))
        command += [str(ROOT / 'tools/test_offline_gift_catalog.c'),
                    str(ROOT / 'runtime/offline_gift_catalog.c'), '-o', str(cls.binary)]
        subprocess.run(command, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def scenario(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS authored offline gift catalog scenario', result.stdout)

    def test_registry_and_bounds(self): self.scenario('registry')
    def test_typed_payloads_and_recreation_labels(self): self.scenario('payloads')
    def test_mission_and_native_receipt_contract(self): self.scenario('receipt-contract')


if __name__ == '__main__':
    unittest.main(verbosity=2)
