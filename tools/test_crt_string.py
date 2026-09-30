#!/usr/bin/env python3
"""Check C-locale UTF-16 comparison and the seeded 2276 icon-name table."""
import ctypes
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile


def wide(text):
    data = text.encode("utf-16-le") + b"\0\0"
    return (ctypes.c_uint16 * (len(data) // 2)).from_buffer_copy(data)


def icon_names(path):
    image = path.read_bytes()
    assert image[:4] == b"XBEH"
    base, = struct.unpack_from("<I", image, 0x104)
    count, table = struct.unpack_from("<II", image, 0x11c)
    sections = [struct.unpack_from("<5I", image, table - base + i * 0x38)
                for i in range(count)]

    def read(address, size):
        for _, va, _, offset, raw_size in sections:
            if va <= address and address + size <= va + raw_size:
                return image[offset + address - va:offset + address - va + size]
        raise ValueError(f"unbacked XBE range {address:#x}+{size:#x}")

    names = []
    for address in struct.unpack("<40I", read(0x31e098, 40 * 4)):
        data = bytearray()
        for i in range(256):
            unit = read(address + i * 2, 2)
            if unit == b"\0\0":
                names.append(data.decode("utf-16-le"))
                break
            data.extend(unit)
        else:
            raise ValueError("unterminated icon name")
    return names


root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="halo-crt-string-") as directory:
    library = Path(directory) / "crt-string.so"
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")), "-std=c89", "-Wall", "-Wextra",
        "-Werror", "-fshort-wchar", "-fPIC", "-shared",
        str(root / "compat/crt/string.c"), "-o", str(library),
    ], check=True)
    crt = ctypes.CDLL(str(library))
    compare = crt.__wcsnicmp
    compare.argtypes = [ctypes.POINTER(ctypes.c_uint16),
                       ctypes.POINTER(ctypes.c_uint16), ctypes.c_size_t]
    compare.restype = ctypes.c_int
    assert compare(None, None, 0) == 0
    for a, b, count, expected in [
        ("right-stick", "RIGHT-STICK", 11, 0),
        ("right-stick", "a-button", 8, ord("r") - ord("a")),
        ("abcX", "ABCY", 3, 0),
        ("abcX", "ABCY", 4, -1),
        ("", "A", 8, -ord("a")),
        ("A", "", 8, ord("a")),
        ("a", "A", 100, 0),
        ("@", "`", 1, -32),
        ("[", "{", 1, -32),
        ("\u00c4", "\u00e4", 1, -32),
        ("\uffff", "\u8000", 1, 0x7fff),
    ]:
        actual = compare(wide(a), wide(b), count)
        assert actual == expected, (a, b, count, actual, expected)
    # Exactly count readable units, with no terminator in either buffer.
    assert compare((ctypes.c_uint16 * 2)(65, 66),
                   (ctypes.c_uint16 * 2)(97, 98), 2) == 0
    crt.__ascii_stricmp.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    assert crt.__ascii_stricmp(b"AbC", b"aBc") == 0
    assert crt.__ascii_stricmp(b"a", b"B") == -1

    halo = Path(os.environ.get("HALO_ROOT", str(root.parent / "halo")))
    seed = Path(os.environ.get("HALO_SEED_XBE",
                               str(halo / "halo-patched/cachebeta.xbe")))
    names = icon_names(seed)

    def lookup(text):
        for i, entry in enumerate(names):
            if compare(wide(text), wide(entry), len(entry)) == 0:
                return i
        return -1

    for text, expected in [("left-stick", 16), ("right-stick", 17),
                           ("move", 30), ("look", 31)]:
        assert lookup(text) == expected, (text, lookup(text))
        assert lookup(text.upper() + " following text") == expected
    assert lookup("unknown-icon") == -1
print("C-locale UTF-16 comparison and seeded stick/move/look icon indices: PASS")
