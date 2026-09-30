#!/usr/bin/env python3
"""Exercise the debug keyboard queue: HID diffing, repeat and remote console."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="halo-keyboard-") as directory:
    binary = Path(directory) / "test-keyboard"
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")), "-std=c99", "-Wall", "-Wextra",
        "-Werror", "-DHALO_KEYBOARD_HOST_TEST", "-fsanitize=address,undefined", "-g",
        str(root / "compat/xapi/keyboard.c"), str(root / "tools/test_keyboard.c"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
print("keyboard reports, modifiers, locks, repeat, queue bounds and remote console: PASS")
