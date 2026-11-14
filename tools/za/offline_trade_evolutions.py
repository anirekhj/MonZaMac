#!/usr/bin/env python3
"""Generate a private offline evolution mod from verified local game resources."""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import mmap
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

PERSONAL_RESOURCE = 'avalon/data/personal_array.bin'
RESOURCES = (PERSONAL_RESOURCE,)
SUPPORTED_PERSONAL_SHA256 = '7b9bee08230d343f662540c52a9f126de8e5f02e80e482298b8e8f3683cea332'
FORMAT = 'monzamac-offline-trade-evolutions'
MANIFEST = 'manifest.json'
MAX_ASSET = 64 << 20
MAX_METADATA = 256 << 20
MAX_ENTRIES = 2_000_000


class ModError(ValueError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fnv1a(path: str) -> int:
    value = 0xcbf29ce484222645
    for byte in path.encode('utf-8'):
        value = ((value ^ byte) * 0x100000001b3) & ((1 << 64) - 1)
    return value


def safe_path(path: Path) -> Path:
    """Reject redirects in caller paths; tolerate macOS's standard /private aliases."""
    expanded = path.expanduser()
    absolute = expanded if expanded.is_absolute() else Path.cwd() / expanded
    aliases = {'/tmp': '/private/tmp', '/var': '/private/var', '/etc': '/private/etc'}
    for component in (*reversed(absolute.parents), absolute):
        if component.is_symlink():
            if aliases.get(str(component)) != str(component.resolve()):
                raise ModError('Symbolic links are not allowed in input or output paths.')
    return absolute.resolve(strict=False)


def read_regular(path: Path, limit: int) -> bytes:
    path = safe_path(path)
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    with os.fdopen(fd, 'rb') as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= limit:
            raise ModError('Input is not a bounded regular file.')
        data = source.read(limit + 1)
        after = os.fstat(source.fileno())
        current = path.stat(follow_symlinks=False)
        fields = lambda info: (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)
        if len(data) != before.st_size or fields(before) != fields(after) or fields(after) != fields(current):
            raise ModError('An input changed while it was being read.')
        return data


class _FlatBuffer:
    def __init__(self, data):
        self.data = data

    def span(self, offset: int, size: int) -> None:
        if offset < 0 or size < 0 or offset > len(self.data) or size > len(self.data) - offset:
            raise ModError('Archive metadata is truncated or out of bounds.')

    def number(self, kind: str, offset: int) -> int:
        self.span(offset, struct.calcsize(kind))
        return struct.unpack_from(kind, self.data, offset)[0]

    def table(self, offset: int) -> list[int | None]:
        vtable = offset - self.number('<i', offset)
        length, object_size = self.number('<H', vtable), self.number('<H', vtable + 2)
        if length < 4 or length & 1 or length > 4096 or object_size < 4:
            raise ModError('Archive has an invalid FlatBuffer table.')
        self.span(vtable, length)
        self.span(offset, object_size)
        fields = []
        for entry in range(vtable + 4, vtable + length, 2):
            relative = self.number('<H', entry)
            if relative and not 4 <= relative < object_size:
                raise ModError('Archive has an invalid FlatBuffer field.')
            fields.append(offset + relative if relative else None)
        return fields

    def vector(self, field: int | None, width: int) -> tuple[int, int]:
        if field is None:
            raise ModError('Archive is missing a required vector.')
        relative = self.number('<I', field)
        if not relative:
            raise ModError('Archive has an invalid vector reference.')
        start = field + relative
        count = self.number('<I', start)
        if count > MAX_ENTRIES and width != 1:
            raise ModError('Archive vector exceeds its entry limit.')
        self.span(start + 4, count * width)
        return count, start + 4

    def indirect_table(self, field: int) -> list[int | None]:
        relative = self.number('<I', field)
        if not relative:
            raise ModError('Archive has an invalid table reference.')
        return self.table(field + relative)


def _field(table: list[int | None], index: int) -> int:
    if index >= len(table) or table[index] is None:
        raise ModError('Archive table is missing a required field.')
    return table[index]


def _check_trpfd(data: bytes, targets: dict[int, str]) -> None:
    fb = _FlatBuffer(data)
    table = fb.indirect_table(0)
    count, hashes = fb.vector(_field(table, 0), 8)
    entries, offsets = fb.vector(_field(table, 2), 4)
    if count != entries:
        raise ModError('TRPFD file hashes and entries have different lengths.')
    found = set()
    for index in range(count):
        value = fb.number('<Q', hashes + index * 8)
        if value in targets:
            if value in found:
                raise ModError('TRPFD contains a duplicate requested resource.')
            fb.indirect_table(offsets + index * 4)
            found.add(value)
    if found != set(targets):
        raise ModError('TRPFD does not contain the requested game resource.')


def decoder_candidates() -> tuple[Path, ...]:
    root = Path(__file__).resolve().parents[2]
    return (
        Path(sys.executable).parent / 'monzamac-shader-decode',
        root / 'build/shader-decode/release/monzamac-shader-decode',
        Path('/Applications/MonZaMac.app/Contents/Resources/lasgen/monzamac-shader-decode'),
        Path.home() / 'Applications/MonZaMac.app/Contents/Resources/lasgen/monzamac-shader-decode',
    )


def discover_decoder(override: Path | None = None) -> Path | None:
    for candidate in (override,) if override is not None else decoder_candidates():
        try:
            candidate = safe_path(candidate)
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return candidate
        except (ModError, OSError):
            if override is not None:
                raise
    if override is not None:
        raise ModError('The requested decoder is not an executable regular file.')
    return None


def extract_resources(romfs: Path, decoder=None) -> dict[str, bytes]:
    """Inspect pack metadata, decoding only the exact requested path hash."""
    root = safe_path(romfs)
    targets = {fnv1a(path): path for path in RESOURCES}
    _check_trpfd(read_regular(root / 'arc/data.trpfd', MAX_METADATA), targets)
    path = safe_path(root / 'arc/data.trpfs')
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    stored = {}
    with os.fdopen(fd, 'rb') as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_size < 16:
            raise ModError('Expected a regular ONEPACK archive.')
        with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            fb = _FlatBuffer(data)
            if data[:8] != b'ONEPACK\0':
                raise ModError('Not a ONEPACK archive.')
            table = fb.indirect_table(fb.number('<Q', 8))
            pack_count, pack_hashes = fb.vector(_field(table, 0), 8)
            count, pack_offsets = fb.vector(_field(table, 1), 8)
            if pack_count != count:
                raise ModError('ONEPACK pack vectors have different lengths.')
            total_files = 0
            for index in range(count):
                pack = fb.indirect_table(fb.number('<Q', pack_offsets + index * 8))
                file_count, hashes = fb.vector(_field(pack, 0), 8)
                entry_count, entries = fb.vector(_field(pack, 1), 4)
                if file_count != entry_count:
                    raise ModError('ONEPACK file vectors have different lengths.')
                total_files += file_count
                if total_files > MAX_ENTRIES:
                    raise ModError('ONEPACK exceeds its total file-entry limit.')
                for item in range(file_count):
                    value = fb.number('<Q', hashes + item * 8)
                    if value not in targets:
                        continue
                    name = targets[value]
                    if name in stored:
                        raise ModError('ONEPACK contains a duplicate requested resource.')
                    entry = fb.indirect_table(entries + item * 4)
                    codec = fb.number('<B', entry[1]) if len(entry) > 1 and entry[1] is not None else 255
                    decoded = fb.number('<Q', entry[3]) if len(entry) > 3 and entry[3] is not None else 0
                    length, start = fb.vector(_field(entry, 4), 1)
                    if not length or length > MAX_ASSET or decoded > MAX_ASSET:
                        raise ModError('Requested resource exceeds its size limit.')
                    stored[name] = (codec, decoded or length, bytes(data[start:start + length]))
            after = os.fstat(source.fileno())
            current = path.stat(follow_symlinks=False)
            fields = lambda info: (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)
            if fields(before) != fields(after) or fields(after) != fields(current):
                raise ModError('The ONEPACK archive changed while it was being read.')
    if set(stored) != set(RESOURCES):
        raise ModError('ONEPACK does not contain the requested game resource.')
    result = {}
    for name, (codec, size, raw) in stored.items():
        if codec == 255 or size == len(raw):
            decoded = raw
        else:
            if decoder is None:
                raise ModError('Compressed resources need the packaged decoder or --decoder.')
            decoded = decoder.unpack(codec, size, raw)
        if len(decoded) != size:
            raise ModError('Decoded resource length does not match its metadata.')
        result[name] = bytes(decoded)
    return result


def _conflicts(parent: Path, output: Path) -> None:
    if not parent.exists():
        return
    for child in parent.iterdir():
        if child == output or child.name.startswith(('.', '_')):
            continue
        if any((child / 'romfs' / resource).exists() or (child / 'romfs' / resource).is_symlink()
               for resource in RESOURCES):
            raise ModError('Another active mod overrides an evolution resource; disable it before installing.')


def _expected_files(files: dict[str, bytes], manifest: dict) -> dict[str, bytes]:
    expected = {}
    for name, data in files.items():
        path = Path(name)
        if path.is_absolute() or '..' in path.parts or not path.parts or path.parts[0] != 'romfs':
            raise ModError('Invalid relative mod output path.')
        expected[path.as_posix()] = bytes(data)
    expected[MANIFEST] = (json.dumps(manifest, sort_keys=True, indent=2) + '\n').encode('utf-8')
    return expected


def _validate_tree(root: Path, expected: dict[str, bytes]) -> None:
    actual = set()
    directories = {Path('.')}
    for name in expected:
        directories.update(Path(name).parents)
    for base, dirs, files in os.walk(root, followlinks=False):
        directory = Path(base)
        info = directory.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) & 0o077:
            raise ModError('Mod directories must be private, owned directories without symbolic links.')
        if directory.relative_to(root) not in directories:
            raise ModError('Existing mod contains unexpected directories.')
        for name in dirs:
            if (directory / name).is_symlink():
                raise ModError('Mod output must not contain symbolic links.')
        for name in files:
            path = directory / name
            relative = path.relative_to(root).as_posix()
            info = path.lstat()
            if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.getuid() or
                    stat.S_IMODE(info.st_mode) & 0o077):
                raise ModError('Mod output must contain only private, owned regular files.')
            if relative not in expected or read_regular(path, max(MAX_ASSET, len(expected[relative]))) != expected[relative]:
                raise ModError('Existing mod differs from the expected output; nothing will be overwritten.')
            actual.add(relative)
    if actual != set(expected):
        raise ModError('Existing mod is incomplete; nothing will be overwritten.')


def _rename_exclusive(source: Path, destination: Path) -> None:
    libc = ctypes.CDLL(None, use_errno=True)
    if sys.platform == 'darwin':
        rename = libc.renamex_np
        rename.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        result = rename(os.fsencode(source), os.fsencode(destination), 4)  # RENAME_EXCL
    elif sys.platform.startswith('linux') and hasattr(libc, 'renameat2'):
        rename = libc.renameat2
        rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
        result = rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1)  # RENAME_NOREPLACE
    else:
        raise ModError('This platform has no supported exclusive directory publication operation.')
    if result:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code))


def _make_parents(path: Path) -> None:
    if not path.exists():
        _make_parents(path.parent)
        path.mkdir(mode=0o700)


def install_mod(out: Path, files: dict[str, bytes], manifest: dict, *, dry_run: bool = False) -> str:
    """Publish one new private mod atomically; an exact rerun is read-only."""
    output = safe_path(out)
    if output.name.startswith(('.', '_')) or not output.name:
        raise ModError('Choose an active mod folder name without a leading dot or underscore.')
    if manifest.get('format') != FORMAT or manifest.get('version') != 1:
        raise ModError('Invalid generated-mod manifest identity.')
    expected = _expected_files(files, manifest)
    parent = output.parent
    if parent.exists() and (not parent.is_dir() or parent.stat().st_uid != os.getuid()):
        raise ModError('The destination parent must be an owned directory.')
    _conflicts(parent, output)
    if output.exists():
        if not output.is_dir() or not (output / MANIFEST).is_file():
            raise ModError('The output exists and is not an owned generated mod.')
        previous = json.loads(read_regular(output / MANIFEST, 1 << 20))
        if not isinstance(previous, dict) or previous.get('format') != FORMAT or previous.get('version') != 1:
            raise ModError('The output exists and is not an owned generated mod.')
        _validate_tree(output, expected)
        return 'unchanged'
    if dry_run:
        return 'would-create'
    _make_parents(parent)
    parent = safe_path(parent)
    if not parent.is_dir() or parent.stat().st_uid != os.getuid():
        raise ModError('The destination parent must be an owned directory.')
    lock = parent / f'_{output.name}.install-lock'
    try:
        lock.mkdir(mode=0o700)
    except FileExistsError as error:
        raise ModError('An install lock exists; another install may be running.') from error
    stage = None
    try:
        _conflicts(parent, output)
        if output.exists() or output.is_symlink():
            raise ModError('The output appeared during installation; nothing was overwritten.')
        stage = Path(tempfile.mkdtemp(prefix=f'_{output.name}.staging-', dir=parent))
        for name, data in expected.items():
            path = stage / name
            _make_parents(path.parent)
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            with os.fdopen(fd, 'wb') as destination:
                destination.write(data)
                destination.flush()
                os.fsync(destination.fileno())
        _validate_tree(stage, expected)
        _conflicts(parent, output)
        _rename_exclusive(stage, output)
        stage = None
        return 'created'
    finally:
        if stage is not None:
            shutil.rmtree(stage)
        lock.rmdir()


def create_mod(personal: bytes, out: Path, level: int = 36, *, dry_run: bool = False) -> dict:
    """Validate the supported stock source before invoking the pure transformation."""
    if sha256(personal) != SUPPORTED_PERSONAL_SHA256:
        raise ModError('Unrecognized personal data: only the verified original Z-A 2.0.2 source is supported.')
    if type(level) is not int or not 36 <= level <= 100:
        raise ModError('The evolution level must be an integer from 36 to 100.')
    try:
        from .evolution_rules import transform_personal
    except ImportError:
        from evolution_rules import transform_personal
    changed, report = transform_personal(personal, minimum_level=level)
    if report.get('rows_scanned') != 1445 or report.get('present_rows') != 594:
        raise ModError('Unexpected personal row counts; the mod was not installed.')
    manifest = dict(format=FORMAT, version=1,
                    source_sha256={PERSONAL_RESOURCE: sha256(personal)},
                    output_sha256={PERSONAL_RESOURCE: sha256(changed)},
                    settings=dict(minimum_level=level, trade_items_required=False,
                                  slowpoke_branches='slowbro-day_slowking-night'),
                    counts={key: value for key, value in report.items()
                            if type(value) is int and key != 'minimum_level'})
    status = install_mod(out, {'romfs/' + PERSONAL_RESOURCE: changed}, manifest, dry_run=dry_run)
    return dict(status=status, manifest=manifest, report=report)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--romfs', type=Path, help='extracted RomFS folder (default: extracted/romfs)')
    source.add_argument('--personal', type=Path, help='verified decompressed personal source instead of a packed RomFS')
    parser.add_argument('--decoder', type=Path, help='external packaged decoder for compressed ONEPACK resources')
    parser.add_argument('--out', type=Path, required=True, help='new private mod folder, normally mods/OfflineTradeEvolutions')
    parser.add_argument('--level', type=int, default=36, help='minimum evolution level, 36 through 100 (default: 36)')
    parser.add_argument('--dry-run', action='store_true', help='validate and report without writing output')
    args = parser.parse_args(argv)
    if args.personal and args.decoder:
        parser.error('--decoder is only used with --romfs')
    try:
        if args.personal:
            personal = read_regular(args.personal, MAX_ASSET)
        else:
            decoder_path = discover_decoder(args.decoder)
            try:
                from .shader_inputs import ExternalDecoder
            except ImportError:
                from shader_inputs import ExternalDecoder
            resources = extract_resources(args.romfs or Path('extracted/romfs'),
                                          ExternalDecoder(decoder_path) if decoder_path else None)
            personal = resources[PERSONAL_RESOURCE]
        result = create_mod(personal, args.out, args.level, dry_run=args.dry_run)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    count = result['manifest']['counts'].get('trade_rules_changed', 0)
    print(f"{result['status']}: {count} trade rules; level {args.level}; one private resource. Original unchanged.")
    if args.dry_run:
        print('Dry run: no files were written.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
