#!/usr/bin/env python3
"""Private mod installation and archive tests using entirely authored inputs."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / 'za'))
import offline_trade_evolutions as mod
from test_offline_trade_evolutions import fixture, table, pointer, vector


def archive_fixture(root: Path, payload=b'authored-personal-data', compressed=False):
    """One requested resource and one unrelated compressed resource."""
    target_hash = mod.fnv1a(mod.PERSONAL_RESOURCE)
    hashes = [target_hash, mod.fnv1a('authored/other.bin')]
    metadata = bytearray(4)
    top, fields = table(metadata, {0: bytes(4), 2: bytes(4)})
    pointer(metadata, 0, top)
    pointer(metadata, fields[0], vector(metadata, [struct.pack('<Q', h) for h in hashes]))
    entries = vector(metadata, [bytes(4), bytes(4)])
    pointer(metadata, fields[2], entries)
    for i in range(2):
        entry, _ = table(metadata, {0: struct.pack('<I', 0)})
        pointer(metadata, entries + 4 + i * 4, entry)

    data = bytearray(b'ONEPACK\0' + bytes(8) + bytes(4))
    struct.pack_into('<Q', data, 8, 16)
    top, fields = table(data, {0: bytes(4), 1: bytes(4)})
    pointer(data, 16, top)
    pointer(data, fields[0], vector(data, [struct.pack('<Q', 12345)]))
    offsets = vector(data, [bytes(8)])
    pointer(data, fields[1], offsets)
    pack_reference = len(data)
    data.extend(bytes(4))
    struct.pack_into('<Q', data, offsets + 4, pack_reference)
    pack, fields = table(data, {0: bytes(4), 1: bytes(4)})
    pointer(data, pack_reference, pack)
    pointer(data, fields[0], vector(data, [struct.pack('<Q', h) for h in hashes]))
    entries = vector(data, [bytes(4), bytes(4)])
    pointer(data, fields[1], entries)
    for i in range(2):
        raw = b'authored-compressed' if compressed or i else payload
        size = len(payload) if i == 0 else 4321
        codec = 3 if compressed or i else 255
        entry, fields = table(data, {1: bytes([codec]), 3: struct.pack('<Q', size), 4: bytes(4)})
        pointer(data, entries + 4 + i * 4, entry)
        pointer(data, fields[4], vector(data, [bytes([byte]) for byte in raw]))
    (root / 'arc').mkdir(parents=True)
    (root / 'arc/data.trpfd').write_bytes(metadata)
    (root / 'arc/data.trpfs').write_bytes(data)
    return bytes(metadata), bytes(data)


class InstallationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Match production row-count guards without copying any game records.
        trade = (0, 6, 888, 0, 0, 0, 208, 0)
        rows = [(95, 0, 1, [trade])]
        rows.extend((1000 + i, 0, int(i < 593), []) for i in range(1444))
        cls.personal, _ = fixture(rows)
        cls.digest = hashlib.sha256(cls.personal).hexdigest()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='offline-evolution-install-')
        self.root = Path(self.temporary.name).resolve()
        self.out = self.root / 'mods/OfflineTradeEvolutions'

    def tearDown(self):
        self.temporary.cleanup()

    def create(self, **kwargs):
        with patch.object(mod, 'SUPPORTED_PERSONAL_SHA256', self.digest):
            return mod.create_mod(self.personal, self.out, **kwargs)

    def test_create_real_transform_then_exact_repeat_is_read_only(self):
        result = self.create()
        self.assertEqual(result['status'], 'created')
        asset = self.out / 'romfs' / mod.PERSONAL_RESOURCE
        before = asset.stat()
        content = asset.read_bytes()
        manifest = json.loads((self.out / mod.MANIFEST).read_text())
        self.assertEqual(manifest['settings']['minimum_level'], 36)
        self.assertFalse(manifest['settings']['trade_items_required'])
        self.assertEqual(manifest['settings']['slowpoke_branches'], 'slowbro-day_slowking-night')
        self.assertEqual(manifest['counts']['trade_rules_changed'], 1)
        self.assertEqual(manifest['output_sha256'][mod.PERSONAL_RESOURCE], hashlib.sha256(content).hexdigest())
        self.assertEqual(asset.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.out.stat().st_mode & 0o777, 0o700)
        self.assertEqual(self.create()['status'], 'unchanged')
        after = asset.stat()
        self.assertEqual((before.st_ino, before.st_mtime_ns), (after.st_ino, after.st_mtime_ns))
        self.assertEqual(asset.read_bytes(), content)

    def test_dry_run_creates_no_parent_or_asset(self):
        self.assertEqual(self.create(dry_run=True)['status'], 'would-create')
        self.assertFalse(self.out.parent.exists())

    def test_unknown_source_or_invalid_level_does_not_publish(self):
        with self.assertRaises(mod.ModError):
            mod.create_mod(b'authored unsupported source', self.out)
        for value in (0, 35, 101, True):
            with self.subTest(value=value), self.assertRaises((mod.ModError, ValueError)):
                self.create(level=value)
        self.assertFalse(self.out.parent.exists())

    def test_wrong_row_counts_fail_before_publication(self):
        small, _ = fixture([(95, 0, 1, [(0, 5, 0, 0, 0, 0, 208, 0)])])
        with patch.object(mod, 'SUPPORTED_PERSONAL_SHA256', hashlib.sha256(small).hexdigest()):
            with self.assertRaises(mod.ModError):
                mod.create_mod(small, self.out)
        self.assertFalse(self.out.exists())

    def test_unowned_or_changed_output_is_preserved(self):
        self.out.mkdir(parents=True)
        sentinel = self.out / 'user-data'
        sentinel.write_bytes(b'authored user data')
        with self.assertRaises(mod.ModError):
            self.create()
        self.assertEqual(sentinel.read_bytes(), b'authored user data')
        sentinel.unlink()
        self.out.rmdir()
        self.create()
        asset = self.out / 'romfs' / mod.PERSONAL_RESOURCE
        asset.write_bytes(b'authored user modification')
        with self.assertRaises(mod.ModError):
            self.create()
        self.assertEqual(asset.read_bytes(), b'authored user modification')

    def test_active_conflict_rejected_disabled_folder_ignored(self):
        conflict = self.out.parent / 'Other/romfs' / mod.PERSONAL_RESOURCE
        conflict.parent.mkdir(parents=True)
        conflict.write_bytes(b'authored conflicting mod')
        with self.assertRaises(mod.ModError):
            self.create(dry_run=True)
        other = self.out.parent / 'Other'
        other.rename(self.out.parent / '_Other')
        self.assertEqual(self.create()['status'], 'created')

    def test_symlink_parent_and_modified_hardlinked_asset_rejected(self):
        real = self.root / 'real'
        real.mkdir()
        self.out.parent.symlink_to(real, target_is_directory=True)
        with self.assertRaises(mod.ModError):
            self.create()
        self.assertFalse((real / self.out.name).exists())
        self.out.parent.unlink()
        self.create()
        asset = self.out / 'romfs' / mod.PERSONAL_RESOURCE
        os.link(asset, self.root / 'outside-link')
        with self.assertRaises(mod.ModError):
            self.create()

    def test_failed_publication_cleans_staging_and_never_replaces_racing_folder(self):
        original = mod._rename_exclusive

        def race(source, destination):
            destination.mkdir()
            (destination / 'sentinel').write_bytes(b'authored concurrent data')
            original(source, destination)

        with patch.object(mod, '_rename_exclusive', side_effect=race):
            with self.assertRaises(OSError):
                self.create()
        self.assertEqual((self.out / 'sentinel').read_bytes(), b'authored concurrent data')
        self.assertEqual([p.name for p in self.out.parent.iterdir()], [self.out.name])

    def test_stored_archive_extracts_exact_requested_file(self):
        original = archive_fixture(self.root)
        self.assertEqual(mod.extract_resources(self.root), {mod.PERSONAL_RESOURCE: b'authored-personal-data'})
        self.assertEqual((self.root / 'arc/data.trpfd').read_bytes(), original[0])
        self.assertEqual((self.root / 'arc/data.trpfs').read_bytes(), original[1])

    def test_compressed_archive_decodes_only_requested_payload(self):
        archive_fixture(self.root, compressed=True)

        class Decoder:
            calls = []

            def unpack(self, codec, size, raw):
                self.calls.append((codec, size, raw))
                return b'authored-personal-data'

        decoder = Decoder()
        self.assertEqual(mod.extract_resources(self.root, decoder),
                         {mod.PERSONAL_RESOURCE: b'authored-personal-data'})
        self.assertEqual(decoder.calls, [(3, len(b'authored-personal-data'), b'authored-compressed')])

    def test_corrupt_archive_and_decoder_length_rejected(self):
        archive_fixture(self.root, compressed=True)
        with self.assertRaises(mod.ModError):
            mod.extract_resources(self.root)

        class Decoder:
            def unpack(self, *args):
                return b'too short'

        with self.assertRaises(mod.ModError):
            mod.extract_resources(self.root, Decoder())
        (self.root / 'arc/data.trpfs').write_bytes(b'ONEPACK\0')
        with self.assertRaises(mod.ModError):
            mod.extract_resources(self.root, Decoder())

    def test_decoder_discovery_skips_unsafe_automatic_candidate(self):
        real = self.root / 'real-decoder-dir'
        real.mkdir()
        executable = real / 'monzamac-shader-decode'
        executable.write_bytes(b'#!/bin/sh\nexit 0\n')
        executable.chmod(0o700)
        redirect = self.root / 'redirect'
        redirect.symlink_to(real, target_is_directory=True)
        with patch.object(mod, 'decoder_candidates', return_value=(redirect / executable.name, executable)):
            self.assertEqual(mod.discover_decoder(), executable)
        with self.assertRaises(mod.ModError):
            mod.discover_decoder(redirect / executable.name)


if __name__ == '__main__':
    unittest.main()
