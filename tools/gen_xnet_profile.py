#!/usr/bin/env python3
"""Carry the target certificate's LAN key into the standalone XNet build.

cxbe emits a new homebrew certificate; reading that at runtime would silently
select another discovery key. No Xbox kernel key is copied into the image.
"""
import argparse
from pathlib import Path
import struct


def generate(image: bytes) -> str:
    if len(image) < 0x178 or image[:4] != b"XBEH":
        raise ValueError("invalid XBE header")
    base, = struct.unpack_from("<I", image, 0x104)
    cert, = struct.unpack_from("<I", image, 0x118)
    offset = cert - base
    if offset < 0 or offset + 0xC0 > len(image):
        raise ValueError("XBE certificate is outside the input file")
    size, = struct.unpack_from("<I", image, offset)
    if size < 0xC0:
        raise ValueError("XBE certificate has no LAN key")
    key = image[offset + 0xB0:offset + 0xC0]
    return ("/* Generated from the layout XBE certificate, not the homebrew certificate. */\n"
            "static const unsigned char halo_title_lan_key[16] = {" +
            ",".join(f"0x{b:02x}" for b in key) + "};\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    content = generate(args.xbe.read_bytes())
    if not args.out.exists() or args.out.read_text() != content:
        args.out.write_text(content)
