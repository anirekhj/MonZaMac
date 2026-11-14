# Repair the known HDv2 mip-count error

One HDv2 file, `t1_i012_2_plant01_alb.bntx`, declares 11 mip levels for a 512×512 texture. The valid chain has 10
levels, down to 1×1. This matches a Metal assertion about requesting 11 levels when the maximum is 10. The original
crash log did not name the loaded asset, so this repairs the malformed file without claiming it explains every crash.

The source-only tool recognizes the exact original file by SHA-256. It changes only the mip count at offset `0x20E`
from 11 to 10; image bytes, file size, mip pointers and relocation data stay unchanged. The extra allocated block is
left unused. Different pack versions or modified files are refused. No texture assets are included in the repository.

Quit MonZaMac, then check the pack with Python 3:

```sh
python3 tools/fix_hdv2_mip_count.py "/path/to/HDv2"
```

The default is a dry run. Apply the repair with:

```sh
python3 tools/fix_hdv2_mip_count.py "/path/to/HDv2" --apply
```

You can also pass the full path to the BNTX file. A private original backup is saved before replacement, outside the
pack: on macOS, under `~/Library/Application Support/com.monzamac.app/texture-repair-backups/`. Use `--backup-dir`
to choose another directory outside the pack and `mods`/`romfs`. Backups contain the original texture; keep them local
and do not commit or upload them. Applying the tool again to the repaired file leaves it unchanged. Restart MonZaMac
to reload the corrected metadata; a running process can still hold the old file open. Each separate installed/source
copy must be checked independently.

Verification uses authored synthetic bytes only:

```sh
python3 tools/test_fix_hdv2_mip_count.py
```
