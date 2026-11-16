"""Asset-free SDK clock regression."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="monzamac-clock-test-") as tmp:
    exe = Path(tmp) / "clocks"
    subprocess.run([*shlex.split(os.environ.get("CC", "clang")), "-std=gnu11", "-O1", "-g", "-UNDEBUG",
                    "-Wno-format", "-Wl,-dead_strip", "-Iruntime", "tools/test_time_clocks.c", "-o", str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=5)
