#!/usr/bin/env python3
"""Build/run the Winsock adapter against real host TCP/UDP sockets."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="halo-network-") as directory:
    binary = Path(directory) / "test-network"
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")),
        "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DHALO_NETWORK_HOST_TEST", "-fsanitize=address,undefined", "-g", "-pthread",
        str(root / "compat/xnet/xnet.c"), str(root / "tools/test_network.c"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
