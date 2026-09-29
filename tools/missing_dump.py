#!/usr/bin/env python3
"""Print, in first-hit order, the missing symbols a running standalone build hit.

  python3 tools/missing_dump.py build-seed/halo.map --host 10.0.0.21
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symbolize import MAP_RE, rdcp  # noqa: E402


def getmem(host, va, n):
    r = rdcp(host, 'getmem addr=0x%x length=0x%x' % (va, n))
    hexs = ''.join(l.strip() for l in r.splitlines()[1:]
                   if l.strip() and all(c in '0123456789ABCDEF?' for c in l.strip()))
    return bytes.fromhex(hexs.replace('??', '00'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('map')
    ap.add_argument('--host', required=True)
    args = ap.parse_args()
    syms = {}
    with open(args.map) as f:
        for line in f:
            m = MAP_RE.match(line)
            if m:
                syms[m.group(1)] = int(m.group(2), 16)
    count = int.from_bytes(getmem(args.host, syms['_halo_missing_count'], 4), 'little')
    n = min(count, 1024)
    table = getmem(args.host, syms['_halo_missing_order'], 4 * n)
    print('%d missing symbols hit' % count)
    for i in range(n):
        ptr = int.from_bytes(table[4 * i:4 * i + 4], 'little')
        name = getmem(args.host, ptr, 96).split(b'\0')[0].decode(errors='replace')
        print('%4d  %s' % (i, name))


if __name__ == '__main__':
    main()
