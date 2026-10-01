#!/usr/bin/env python3
"""Carry the layout XBE's DirectSound HRTF filter bank into the APU build.

Retail DirectSound (cachebeta 0x20a2f5, CSensaura3d_GetFullHRTFFilterPair)
selects one of these filter pairs per 3D voice. The tables are XDK data, so
they are read from the layout XBE at build time and never committed.
"""
import argparse
from pathlib import Path
import struct

FILTERS = 0x20F258  # uint8[2222][32]: 31 int8 taps, then the ITD byte
INDEX = 0x220818  # uint16[61 azimuth][31 elevation], filter numbers
FILTER_COUNT = (INDEX - FILTERS) // 32
AZIMUTHS, ELEVATIONS = 61, 31
# GetFullHRTFFilterPair's own table references; any other image is refused.
CODE = ((0x20A3F3, "0fb70c7518082200"), (0x20A405, "8d8158f22000"),
        (0x20A412, "8d8178f22000"))


def read(image: bytes, va: int, size: int) -> bytes:
    base, = struct.unpack_from("<I", image, 0x104)
    count, headers = struct.unpack_from("<II", image, 0x11C)
    for i in range(count):
        _, start, _, raw, raw_size = struct.unpack_from("<5I", image, headers - base + i * 56)
        if start <= va and va + size <= start + raw_size:
            return image[raw + va - start:raw + va - start + size]
    raise ValueError(f"VA {va:#x}+{size:#x} is outside the XBE's raw sections")


def generate(image: bytes) -> str:
    if len(image) < 0x178 or image[:4] != b"XBEH":
        raise ValueError("invalid XBE header")
    for va, code in CODE:
        if read(image, va, len(code) // 2) != bytes.fromhex(code):
            raise ValueError(f"no XDK HRTF selection code at {va:#x}; not cachebeta 2276")
    filters = read(image, FILTERS, FILTER_COUNT * 32)
    index = struct.unpack("<%dH" % (AZIMUTHS * ELEVATIONS),
                          read(image, INDEX, AZIMUTHS * ELEVATIONS * 2))
    if any(i & 1 or i + 1 >= FILTER_COUNT for i in index):
        raise ValueError("HRTF direction index out of range")
    lines = ["/* Generated from the layout XBE's DirectSound tables. Not redistributable. */",
             f"#define XDK_HRTF_FILTERS {FILTER_COUNT}",
             "static const unsigned char xdk_hrtf_filters[XDK_HRTF_FILTERS][32] = {"]
    for i in range(FILTER_COUNT):
        lines.append("{" + ",".join(str(b) for b in filters[i * 32:i * 32 + 32]) + "},")
    lines.append("};")
    lines.append(f"static const unsigned short xdk_hrtf_index[{AZIMUTHS}][{ELEVATIONS}] = {{")
    for a in range(AZIMUTHS):
        lines.append("{" + ",".join(str(i) for i in index[a * ELEVATIONS:(a + 1) * ELEVATIONS]) + "},")
    lines.append("};")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    content = generate(args.xbe.read_bytes())
    if not args.out.exists() or args.out.read_text() != content:
        args.out.write_text(content)
