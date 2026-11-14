#!/usr/bin/env python3
"""Synthetic temporal reconstruction checks; no game files required."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
sdk = os.environ.get("SDKROOT") or subprocess.check_output(
    ["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
with tempfile.TemporaryDirectory(prefix="monzamac-temporal-") as tmp:
    binary = Path(tmp) / "temporal_gpu"
    subprocess.run(["xcrun", "clang", "-std=gnu11", "-O2", "-fobjc-arc", "-Wall", "-Wextra", "-Werror",
                    "-mmacosx-version-min=15.0", "-isysroot", sdk, "-I", str(root / "runtime/nvn"),
                    str(root / "tools/test_temporal_upscale_gpu.m"), str(root / "runtime/nvn/frame_generation.m"),
                    "-framework", "Foundation", "-framework", "Metal", "-framework", "MetalFX", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], env=dict(os.environ, MTL_DEBUG_LAYER="1", MTL_SHADER_VALIDATION="1"), check=True, timeout=180)
