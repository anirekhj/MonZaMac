#!/usr/bin/env python3
"""CPU-only archive guard fixtures; no Metal, game files or existing caches."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ArchivePolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='monzamac-archive-policy-build-')
        cls.binary = Path(cls.temporary.name) / 'check'
        command = shlex.split(os.environ.get('CC', 'cc'))
        command += ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic']
        command += shlex.split(os.environ.get('TEST_CFLAGS', ''))
        subprocess.run(command + [str(ROOT / 'tools/test_pipeline_archive_policy.c'), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def scenario(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS authored archive policy scenario', result.stdout)

    def test_sparse_file_read_boundaries(self): self.scenario('boundaries')
    def test_interrupted_load_and_changed_fingerprint(self): self.scenario('interrupted')
    def test_malformed_markers_fail_closed(self): self.scenario('malformed')
    def test_symlink_hardlink_fifo_and_alias_guards(self): self.scenario('redirects')
    def test_foreign_fingerprint_read_only_and_descriptor_budget(self): self.scenario('identity-budget')


if __name__ == '__main__':
    unittest.main(verbosity=2)
