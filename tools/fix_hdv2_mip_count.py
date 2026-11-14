#!/usr/bin/env python3
"""Repair one recognized HDv2 BNTX mip-count error; dry-run unless --apply."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile


RELATIVE_PATH = Path("romfs/field/share_tex/t1/t1_i/t1_i012_2_plant01_alb/"
                     "t1_i012_2_plant01_alb.bntx")
ORIGINAL_SHA256 = "afa466f04f3a46ff36c92628327197c4a62e8f23fdf24c44ee19bf8c35d7afc4"
FILE_SIZE = 184464
BRTI = 0x1F8
MIP_COUNT_OFFSET = BRTI + 0x16
MIP_TABLE = 1176
DATA_BLOCK = 4080
DATA_BLOCK_SIZE = 180240
IMAGE_SIZE = 177664
RELOCATION = 184320
MIP_POINTERS = (4096, 135168, 167936, 176128, 178176, 178688,
                179200, 179712, 180224, 180736, 181248)


class RepairError(ValueError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_layout(data: bytes, mip_count: int) -> None:
    """Check only the known container layout, without decoding texture pixels."""
    def require(condition: bool, message: str) -> None:
        if not condition:
            raise RepairError(message)

    def u16(offset: int) -> int:
        return struct.unpack_from("<H", data, offset)[0]

    def u32(offset: int) -> int:
        return struct.unpack_from("<I", data, offset)[0]

    def u64(offset: int) -> int:
        return struct.unpack_from("<Q", data, offset)[0]

    require(len(data) == FILE_SIZE, "Unexpected BNTX file size.")
    require(data[:8] == b"BNTX\0\0\0\0" and data[0xC:0xE] == b"\xff\xfe",
            "Expected a little-endian BNTX container.")
    require(u32(8) == 0x40000 and u32(0x1C) == len(data), "Unexpected BNTX header.")
    require(data[0x20:0x24] == b"NX  " and u32(0x24) == 1 and u64(0x28) == 408,
            "Expected the known single-texture container.")
    require(u64(408) == BRTI and data[BRTI:BRTI + 4] == b"BRTI",
            "Unexpected BRTI location.")
    require(u32(BRTI + 4) == 3576 and u64(BRTI + 8) == 3576, "Unexpected BRTI size.")
    require(u16(MIP_COUNT_OFFSET) == mip_count, "Unexpected mip count.")
    require(u32(BRTI + 0x1C) == 0x1A06, "Expected BC1 sRGB texture data.")
    dimensions = struct.unpack_from("<III", data, BRTI + 0x24)
    require(dimensions == (512, 512, 1) and u32(BRTI + 0x30) == 1,
            "Expected a 512 x 512 x 1 texture with one array layer.")
    require(u32(BRTI + 0x34) == 4 and u32(BRTI + 0x5C) == 1,
            "Unexpected block height or texture type.")
    require(u32(BRTI + 0x50) == IMAGE_SIZE and u32(BRTI + 0x54) == 512,
            "Unexpected image size or alignment.")
    require(u64(BRTI + 0x70) == MIP_TABLE, "Unexpected mip pointer table.")
    pointers = struct.unpack_from("<11Q", data, MIP_TABLE)
    require(pointers == MIP_POINTERS, "Unexpected mip offsets; this file needs a different repair.")
    require(u64(0x30) == DATA_BLOCK and data[DATA_BLOCK:DATA_BLOCK + 4] == b"BRTD" and
            u64(DATA_BLOCK + 8) == DATA_BLOCK_SIZE, "Unexpected texture data block.")
    require(u32(0x18) == RELOCATION and data[RELOCATION:RELOCATION + 4] == b"_RLT" and
            u32(RELOCATION + 4) == RELOCATION and u32(RELOCATION + 8) == 2,
            "Unexpected relocation table.")
    require(struct.unpack_from("<QIIII", data, RELOCATION + 16) == (0, 0, 1264, 0, 8) and
            struct.unpack_from("<QIIII", data, RELOCATION + 40) == (0, DATA_BLOCK, DATA_BLOCK_SIZE, 8, 2),
            "Unexpected relocation sections.")
    require(struct.unpack_from("<IHBB", data, RELOCATION + 136) == (MIP_TABLE, 1, 11, 0),
            "Unexpected mip pointer relocation entry.")
    require(MIP_TABLE + len(pointers) * 8 == 1264 and
            DATA_BLOCK + DATA_BLOCK_SIZE == RELOCATION and
            pointers[-1] + 512 == pointers[0] + IMAGE_SIZE <= RELOCATION,
            "Texture data or metadata is out of bounds.")
    maximum = max(dimensions).bit_length()
    require(mip_count in (maximum, maximum + 1), "Unexpected mip-chain length.")


@dataclass(frozen=True)
class RepairPlan:
    data: bytes
    already_repaired: bool


def plan_repair(data: bytes, *, expected_sha256: str = ORIGINAL_SHA256) -> RepairPlan:
    """The hash override is for authored synthetic tests, never a CLI option."""
    digest = sha256(data)
    if digest != expected_sha256:
        # Accept only the exact known repair, not arbitrary files with ten mips.
        if len(data) != FILE_SIZE or struct.unpack_from("<H", data, MIP_COUNT_OFFSET)[0] != 10:
            raise RepairError("Unrecognized file: its SHA-256 does not match the known HDv2 texture.")
        original = bytearray(data)
        struct.pack_into("<H", original, MIP_COUNT_OFFSET, 11)
        if sha256(original) != expected_sha256:
            raise RepairError("Unrecognized file: its SHA-256 does not match the known HDv2 texture.")
        validate_layout(data, 10)
        return RepairPlan(data, True)
    validate_layout(data, 11)
    repaired = bytearray(data)
    struct.pack_into("<H", repaired, MIP_COUNT_OFFSET, 10)
    validate_layout(repaired, 10)
    return RepairPlan(bytes(repaired), False)


def locate_texture(path: Path) -> tuple[Path, Path]:
    path = path.expanduser()
    if path.is_symlink():
        raise RepairError("Pass a real pack folder or texture file, not a symbolic link.")
    if path.is_dir():
        pack = path.resolve()
        target = pack / RELATIVE_PATH
    else:
        if path.name != RELATIVE_PATH.name:
            raise RepairError(f"Expected a pack folder or {RELATIVE_PATH.name}.")
        target = path.absolute()
        if target.parts[-len(RELATIVE_PATH.parts):] == RELATIVE_PATH.parts:
            pack = target.parents[len(RELATIVE_PATH.parts) - 1]
        else:
            pack = target.parent
    if target.is_symlink():
        raise RepairError("The texture must be a regular file, not a symbolic link.")
    return target.resolve(strict=True), pack.resolve()


def read_regular(path: Path) -> tuple[bytes, os.stat_result]:
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(fd, "rb") as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size != FILE_SIZE:
            raise RepairError("Expected the known regular BNTX file and size.")
        return source.read(FILE_SIZE + 1), info


def default_backup_dir() -> Path:
    if sys.platform == "darwin":
        return Path.home() / "Library/Application Support/com.monzamac.app/texture-repair-backups"
    return Path.home() / ".local/share/monzamac/texture-repair-backups"


def backup_location(directory: Path | None, target: Path, pack: Path) -> Path:
    directory = (directory or default_backup_dir()).expanduser().resolve()
    # Backups must never become overlay files. Also reject an adjacent file's
    # parent when the caller supplies a standalone texture instead of a pack.
    protected = [pack]
    protected.extend(parent for parent in target.parents if parent.name in ("mods", "romfs"))
    if any(directory == parent or parent in directory.parents for parent in protected):
        raise RepairError("Choose a backup directory outside the texture pack and mods/romfs folders.")
    return directory


def write_backup(directory: Path, data: bytes) -> Path:
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    backup = directory / f"{RELATIVE_PATH.name}.{sha256(data)}.bak"
    try:
        fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
    except FileExistsError:
        previous, info = read_regular(backup)
        if stat.S_IMODE(info.st_mode) & 0o077:
            raise RepairError("The existing backup is not private; choose another backup directory.")
        if previous != data:
            raise RepairError("The existing backup differs; it will not be overwritten.")
        return backup
    try:
        with os.fdopen(fd, "wb") as destination:
            destination.write(data)
            destination.flush()
            os.fsync(destination.fileno())
    except BaseException:
        backup.unlink(missing_ok=True)
        raise
    return backup


def repair_file(path: Path, *, apply: bool = False, backup_dir: Path | None = None,
                expected_sha256: str = ORIGINAL_SHA256) -> tuple[Path, RepairPlan, Path | None]:
    target, pack = locate_texture(path)
    original, before = read_regular(target)
    plan = plan_repair(original, expected_sha256=expected_sha256)
    if plan.already_repaired:
        return target, plan, None
    directory = backup_location(backup_dir, target, pack)
    if not apply:
        return target, plan, None
    backup = write_backup(directory, original)
    temporary: Path | None = None
    try:
        # Same-directory rename is atomic; already-open readers keep the old inode.
        with tempfile.NamedTemporaryFile(dir=target.parent, prefix=f".{target.name}.repair-", delete=False) as output:
            temporary = Path(output.name)
            os.fchmod(output.fileno(), stat.S_IMODE(before.st_mode))
            output.write(plan.data)
            output.flush()
            os.fsync(output.fileno())
        current, now = read_regular(target)
        if current != original or (now.st_dev, now.st_ino, now.st_mtime_ns) != (before.st_dev, before.st_ino, before.st_mtime_ns):
            raise RepairError("The texture changed during repair; the original backup was kept and nothing was replaced.")
        os.replace(temporary, target)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return target, plan, backup


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="HDv2 pack folder, or the recognized BNTX file")
    parser.add_argument("--apply", action="store_true", help="write the repair after making a private backup")
    parser.add_argument("--backup-dir", type=Path, help="backup folder outside the pack (default: MonZaMac's data folder)")
    args = parser.parse_args(argv)
    try:
        target, plan, backup = repair_file(args.path, apply=args.apply, backup_dir=args.backup_dir)
    except (OSError, RepairError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    if plan.already_repaired:
        print(f"Already repaired; unchanged: {target}")
    elif not args.apply:
        print(f"Dry run: {target}\nWould change mip count 11 -> 10 at offset 0x{MIP_COUNT_OFFSET:X}; no files written.")
        print("Run again with --apply to make the repair and save a private backup.")
    else:
        print(f"Repaired: {target}\nBackup: {backup}")
        print("Pixel data, file size, mip offsets and relocation bytes are unchanged. Restart MonZaMac to reload it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
