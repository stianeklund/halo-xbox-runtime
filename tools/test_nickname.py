#!/usr/bin/env python3
"""Exercise the nickname file contract, valid handles, and failure cleanup."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="halo-nickname-") as directory:
    binary = Path(directory) / "test-nickname"
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")), "-std=c89", "-Wall", "-Wextra",
        "-Werror", "-DHALO_NICKNAME_HOST_TEST", "-fsanitize=address,undefined", "-g",
        str(root / "compat/xapi/nickname.c"), str(root / "tools/test_nickname.c"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
print("nickname enumeration, persistence, invalid files and failure cleanup: PASS")
