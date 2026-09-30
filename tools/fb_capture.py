#!/usr/bin/env python3
"""Capture the framebuffers of a running standalone build as PNGs.

XBDM `screenshot` cannot find nxdk's screen buffer. The compat D3D device
(compat/d3d8/device.c, DEVICE_ADDRESS 0x1fbb10) scans out its own
back/front surfaces, whose headers sit at device+0x2150 in 0x18-byte slots
(slot 0 back, slot 1 front, Data = physical address at +4). This pulls those
with getmem; --pbkit reads pbkit's own (unused) framebuffer table instead.

  python3 tools/fb_capture.py build-seed/halo.map \
      --host 10.0.0.21 --out /tmp/shots/fb
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from missing_dump import getmem  # noqa: E402
from symbolize import MAP_RE  # noqa: E402

CHUNK = 0x8000
DEVICE_BUFFER_SURFACES = 0x1fbb10 + 0x2150
SURFACE_STRIDE = 0x18


def u32(b, i=0):
    return int.from_bytes(b[i:i + 4], 'little')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('map')
    ap.add_argument('--host', required=True)
    ap.add_argument('--out', required=True, help='output path prefix')
    ap.add_argument('--index', type=int, action='append',
                    help='buffer index to capture (default: all)')
    ap.add_argument('--pbkit', action='store_true',
                    help='read pbkit framebuffers instead of the compat device')
    args = ap.parse_args()
    from PIL import Image

    syms = {}
    with open(args.map) as f:
        for line in f:
            m = MAP_RE.match(line)
            if m:
                syms[m.group(1)] = int(m.group(2), 16)
    width = u32(getmem(args.host, syms['_pb_FrameBuffersWidth'], 4))
    height = u32(getmem(args.host, syms['_pb_FrameBuffersHeight'], 4))
    pitch = u32(getmem(args.host, syms['_pb_FrameBuffersPitch'], 4))
    count = u32(getmem(args.host, syms['_pb_FrameBuffersCount'], 4))
    back = u32(getmem(args.host, syms['_pb_BackBufferIndex'], 4))
    table = getmem(args.host, syms['_pb_FBAddr'], 4 * count)
    if not args.pbkit:
        count = 2
        headers = getmem(args.host, DEVICE_BUFFER_SURFACES,
                         SURFACE_STRIDE * count)
        table = b''.join(
            (0x80000000 | u32(headers, SURFACE_STRIDE * i + 4)).to_bytes(
                4, 'little') for i in range(count))
    print('%dx%d pitch=0x%x buffers=%d back=%d' % (width, height, pitch,
                                                   count, back))
    for index in (args.index or range(count)):
        va = u32(table, 4 * index)
        size = pitch * height
        data = bytearray()
        for off in range(0, size, CHUNK):
            data += getmem(args.host, va + off, min(CHUNK, size - off))
        img = Image.frombuffer('RGBA', (width, height), bytes(data), 'raw',
                               'BGRA', pitch, 1).convert('RGB')
        path = '%s%d.png' % (args.out, index)
        img.save(path)
        print('buffer %d @0x%x -> %s' % (index, va, path))


if __name__ == '__main__':
    main()
