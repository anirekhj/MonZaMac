#!/usr/bin/env python3
"""Synthetic-only tests: no game files or real texture pixels are used."""
import contextlib
import io
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import fix_hdv2_mip_count as repair


def fixture() -> bytes:
    data = bytearray(repair.FILE_SIZE)
    data[:8] = b"BNTX\0\0\0\0"
    struct.pack_into("<I", data, 8, 0x40000)
    data[0xC:0xE] = b"\xff\xfe"
    struct.pack_into("<I", data, 0x18, repair.RELOCATION)
    struct.pack_into("<I", data, 0x1C, len(data))
    data[0x20:0x24] = b"NX  "
    struct.pack_into("<IQQ", data, 0x24, 1, 408, repair.DATA_BLOCK)
    struct.pack_into("<Q", data, 408, repair.BRTI)
    data[repair.BRTI:repair.BRTI + 4] = b"BRTI"
    struct.pack_into("<IQ", data, repair.BRTI + 4, 3576, 3576)
    struct.pack_into("<H", data, repair.MIP_COUNT_OFFSET, 11)
    struct.pack_into("<I", data, repair.BRTI + 0x1C, 0x1A06)
    struct.pack_into("<IIIII", data, repair.BRTI + 0x24, 512, 512, 1, 1, 4)
    struct.pack_into("<II", data, repair.BRTI + 0x50, repair.IMAGE_SIZE, 512)
    struct.pack_into("<I", data, repair.BRTI + 0x5C, 1)
    struct.pack_into("<Q", data, repair.BRTI + 0x70, repair.MIP_TABLE)
    struct.pack_into("<11Q", data, repair.MIP_TABLE, *repair.MIP_POINTERS)
    data[repair.DATA_BLOCK:repair.DATA_BLOCK + 4] = b"BRTD"
    struct.pack_into("<Q", data, repair.DATA_BLOCK + 8, repair.DATA_BLOCK_SIZE)
    # Authored repeating bytes stand in for pixels; these are not a game asset.
    pixels = (bytes(range(256)) * (repair.IMAGE_SIZE // 256 + 1))[:repair.IMAGE_SIZE]
    data[repair.MIP_POINTERS[0]:repair.MIP_POINTERS[0] + len(pixels)] = pixels
    data[repair.RELOCATION:repair.RELOCATION + 4] = b"_RLT"
    struct.pack_into("<II", data, repair.RELOCATION + 4, repair.RELOCATION, 2)
    struct.pack_into("<QIIII", data, repair.RELOCATION + 16, 0, 0, 1264, 0, 8)
    struct.pack_into("<QIIII", data, repair.RELOCATION + 40, 0, repair.DATA_BLOCK, repair.DATA_BLOCK_SIZE, 8, 2)
    struct.pack_into("<IHBB", data, repair.RELOCATION + 136, repair.MIP_TABLE, 1, 11, 0)
    return bytes(data)


class RepairTests(unittest.TestCase):
    def setUp(self):
        self.original = fixture()
        self.digest = repair.sha256(self.original)
        self.temporary = tempfile.TemporaryDirectory(prefix="hdv2-repair-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.pack = self.root / "mods/HDv2"
        self.target = self.pack / repair.RELATIVE_PATH
        self.target.parent.mkdir(parents=True)
        self.target.write_bytes(self.original)
        self.target.chmod(0o640)
        self.backups = self.root / "private-backups"

    def run_repair(self, path=None, **options):
        return repair.repair_file(path or self.pack, backup_dir=options.pop("backup_dir", self.backups),
                                  expected_sha256=self.digest, **options)

    def test_changes_only_count_and_preserves_container_and_payload(self):
        plan = repair.plan_repair(self.original, expected_sha256=self.digest)
        self.assertFalse(plan.already_repaired)
        self.assertEqual([i for i, (a, b) in enumerate(zip(self.original, plan.data)) if a != b], [526])
        self.assertEqual(struct.unpack_from("<H", plan.data, 526)[0], 10)
        self.assertEqual(len(plan.data), len(self.original))
        self.assertEqual(plan.data[4096:], self.original[4096:])
        self.assertEqual(plan.data[repair.MIP_TABLE:repair.MIP_TABLE + 88],
                         self.original[repair.MIP_TABLE:repair.MIP_TABLE + 88])
        again = repair.plan_repair(plan.data, expected_sha256=self.digest)
        self.assertTrue(again.already_repaired)
        self.assertEqual(again.data, plan.data)

    def test_unknown_hash_and_lookalike_repaired_file_are_refused(self):
        with self.assertRaises(repair.RepairError):
            repair.plan_repair(self.original)
        lookalike = bytearray(self.original)
        struct.pack_into("<H", lookalike, 526, 10)
        lookalike[8192] ^= 1
        with self.assertRaises(repair.RepairError):
            repair.plan_repair(lookalike, expected_sha256=self.digest)

    def test_layout_validation_is_independent_of_the_hash_guard(self):
        changes = [(0, b"FAIL"), (0xC, b"\xfe\xff"), (0x24, struct.pack("<I", 2)),
                   (repair.BRTI + 0x24, struct.pack("<I", 1024)),
                   (repair.BRTI + 0x1C, struct.pack("<I", 0x1C06)),
                   (526, struct.pack("<H", 12)), (repair.MIP_TABLE, struct.pack("<Q", repair.FILE_SIZE)),
                   (repair.BRTI + 0x50, struct.pack("<I", repair.FILE_SIZE)),
                   (repair.RELOCATION + 136, struct.pack("<IHBB", repair.MIP_TABLE, 1, 12, 0))]
        for offset, replacement in changes:
            with self.subTest(offset=offset):
                malformed = bytearray(self.original)
                malformed[offset:offset + len(replacement)] = replacement
                with self.assertRaises(repair.RepairError):
                    repair.plan_repair(malformed, expected_sha256=repair.sha256(malformed))
        truncated = self.original[:-1]
        with self.assertRaises(repair.RepairError):
            repair.plan_repair(truncated, expected_sha256=repair.sha256(truncated))

    def test_dry_run_creates_nothing(self):
        before = self.target.stat()
        _, plan, backup = self.run_repair()
        self.assertFalse(plan.already_repaired)
        self.assertIsNone(backup)
        self.assertEqual(self.target.read_bytes(), self.original)
        self.assertEqual(self.target.stat().st_ino, before.st_ino)
        self.assertFalse(self.backups.exists())

    def test_apply_backs_up_privately_and_replaces_atomically(self):
        before = self.target.stat()
        # An open reader must retain the complete old version after replacement.
        with self.target.open("rb") as reader:
            target, plan, backup = self.run_repair(apply=True)
            self.assertEqual(reader.read(), self.original)
        self.assertEqual(target, self.target.resolve())
        self.assertEqual(backup.read_bytes(), self.original)
        self.assertEqual(stat.S_IMODE(backup.stat().st_mode), 0o600)
        self.assertEqual(self.target.read_bytes(), plan.data)
        self.assertNotEqual(self.target.stat().st_ino, before.st_ino)
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o640)
        self.assertFalse(list(self.target.parent.glob(".*.repair-*")))

    def test_apply_is_idempotent_without_a_second_backup_or_replacement(self):
        self.run_repair(apply=True)
        before = self.target.stat()
        _, plan, backup = self.run_repair(self.target, apply=True)
        self.assertTrue(plan.already_repaired)
        self.assertIsNone(backup)
        self.assertEqual(self.target.stat().st_ino, before.st_ino)
        self.assertEqual(len(list(self.backups.iterdir())), 1)

    def test_one_backup_can_cover_identical_copies(self):
        _, _, backup = self.run_repair(apply=True)
        other = self.root / "source/HDv2" / repair.RELATIVE_PATH
        other.parent.mkdir(parents=True)
        other.write_bytes(self.original)
        _, _, second = self.run_repair(other, apply=True)
        self.assertEqual(second, backup)
        self.assertEqual(backup.read_bytes(), self.original)
        self.assertEqual(len(list(self.backups.iterdir())), 1)

    def test_backup_inside_overlay_is_refused_without_writes(self):
        for directory in (self.pack / "backups", self.root / "mods/backup", self.target.parent):
            with self.subTest(directory=directory), self.assertRaises(repair.RepairError):
                self.run_repair(apply=True, backup_dir=directory)
            self.assertFalse(directory.exists() and directory != self.target.parent)
        self.assertEqual(self.target.read_bytes(), self.original)

    def test_existing_backup_is_never_overwritten(self):
        self.backups.mkdir()
        backup = self.backups / f"{repair.RELATIVE_PATH.name}.{self.digest}.bak"
        bad = bytearray(self.original)
        bad[-1] ^= 1
        backup.write_bytes(bad)
        with self.assertRaises(repair.RepairError):
            self.run_repair(apply=True)
        self.assertEqual(backup.read_bytes(), bad)
        self.assertEqual(self.target.read_bytes(), self.original)

    def test_existing_nonprivate_backup_is_refused(self):
        self.backups.mkdir()
        backup = self.backups / f"{repair.RELATIVE_PATH.name}.{self.digest}.bak"
        backup.write_bytes(self.original)
        backup.chmod(0o644)
        with self.assertRaises(repair.RepairError):
            self.run_repair(apply=True)
        self.assertEqual(self.target.read_bytes(), self.original)

    def test_replacement_failure_keeps_source_and_backup(self):
        with mock.patch.object(repair.os, "replace", side_effect=OSError("synthetic rename failure")):
            with self.assertRaises(OSError):
                self.run_repair(apply=True)
        self.assertEqual(self.target.read_bytes(), self.original)
        self.assertEqual(next(self.backups.iterdir()).read_bytes(), self.original)
        self.assertFalse(list(self.target.parent.glob(".*.repair-*")))

    def test_concurrent_change_is_not_overwritten(self):
        write_backup = repair.write_backup
        changed = bytearray(self.original)
        changed[8192] ^= 1
        def alter_after_backup(directory, data):
            backup = write_backup(directory, data)
            self.target.write_bytes(changed)
            return backup
        with mock.patch.object(repair, "write_backup", side_effect=alter_after_backup):
            with self.assertRaises(repair.RepairError):
                self.run_repair(apply=True)
        self.assertEqual(self.target.read_bytes(), changed)
        self.assertEqual(next(self.backups.iterdir()).read_bytes(), self.original)
        self.assertFalse(list(self.target.parent.glob(".*.repair-*")))

    def test_symbolic_link_is_refused(self):
        link = self.root / repair.RELATIVE_PATH.name
        link.symlink_to(self.target)
        with self.assertRaises(repair.RepairError):
            self.run_repair(link, apply=True)
        self.assertEqual(self.target.read_bytes(), self.original)

    def test_cli_defaults_to_dry_run_and_has_no_hash_override(self):
        actual_repair = repair.repair_file
        def synthetic_repair(path, **options):
            self.assertFalse(options["apply"])
            return actual_repair(path, expected_sha256=self.digest, **options)
        with mock.patch.object(repair, "repair_file", side_effect=synthetic_repair), contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(repair.main([str(self.pack), "--backup-dir", str(self.backups)]), 0)
        self.assertIn("Dry run", output.getvalue())
        self.assertFalse(self.backups.exists())
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            repair.main([str(self.pack), "--expected-sha256", self.digest])
        self.assertEqual(error.exception.code, 2)
        # The real CLI rejects the authored fixture without the internal test seam.
        result = subprocess.run([sys.executable, str(Path(repair.__file__)), str(self.target)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("SHA-256", result.stderr)
        self.assertEqual(self.target.read_bytes(), self.original)


if __name__ == "__main__":
    unittest.main()
