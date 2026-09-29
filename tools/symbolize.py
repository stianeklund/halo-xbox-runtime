#!/usr/bin/env python3
"""Symbolize addresses in a standalone build (lld-link map + legacy layout).

  python3 tools/symbolize.py build/halo.map 0x6aa0ad 0x7f0656
  python3 tools/symbolize.py MAP --host 10.0.0.21 --thread 48   # EBP walk

Addresses below the legacy end (0x642000) are original cachebeta.xbe VAs and
are named from kb.json.
"""
import argparse
import bisect
import os
import re
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deps_inventory as inv  # noqa: E402
from gen_legacy_image import LEGACY_END  # noqa: E402

MAP_RE = re.compile(r'^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8,16})\s+(.*)$')


class Symbolizer:
    def __init__(self, map_path, kb_path):
        syms = []
        with open(map_path) as f:
            for line in f:
                m = MAP_RE.match(line)
                if m:
                    syms.append((int(m.group(2), 16), m.group(1), m.group(3).strip()))
        syms.sort()
        self.syms = syms
        self.addrs = [s[0] for s in syms]
        self.kb = inv.Kb(kb_path)

    def name(self, va):
        if va < LEGACY_END:
            i = bisect.bisect_right(self.kb.func_starts, va) - 1
            if i >= 0:
                e = self.kb.funcs[i]
                return 'legacy %s+0x%x (%s)' % (e['name'], va - e['addr'], e['object'])
            return 'legacy 0x%x' % va
        i = bisect.bisect_right(self.addrs, va) - 1
        if i < 0:
            return '?'
        a, n, o = self.syms[i]
        return '%s+0x%x (%s)' % (n, va - a, o)


CODE_LO, CODE_HI = 0x11000, 0x900000


def rdcp(host, cmd):
    s = socket.create_connection((host, 731), timeout=10)
    s.recv(1024)
    s.sendall(cmd.encode() + b'\r\n')
    data = b''
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
        if data.startswith(b'200') and data.endswith(b'\r\n'):
            break
        if data.endswith(b'\r\n.\r\n'):
            break
    s.close()
    return data.decode(errors='replace')


def read_dword(host, va):
    r = rdcp(host, 'getmem addr=0x%x length=4' % va)
    hexs = ''.join(l.strip() for l in r.splitlines()[1:] if l.strip() and l.strip() != '.')
    if len(hexs) < 8 or '?' in hexs[:8]:
        return None
    return int.from_bytes(bytes.fromhex(hexs[:8]), 'little')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('map')
    ap.add_argument('addrs', nargs='*')
    ap.add_argument('--kb', default=os.path.join(inv.ROOT, 'kb.json'))
    ap.add_argument('--host')
    ap.add_argument('--thread', type=int)
    ap.add_argument('--depth', type=int, default=32)
    ap.add_argument('--scan', type=int, default=0,
                    help='also list code pointers among this many stack words from Esp '
                         '(original code omits frame pointers, so the EBP walk stops early)')
    args = ap.parse_args()
    s = Symbolizer(args.map, args.kb)
    for a in args.addrs:
        va = int(a, 16)
        print('0x%08x  %s' % (va, s.name(va)))
    if args.host and args.thread is not None:
        ctx = rdcp(args.host, 'getcontext thread=%d control int' % args.thread)
        regs = dict(re.findall(r'(\w+)=0x([0-9a-f]+)', ctx))
        eip, ebp = int(regs['Eip'], 16), int(regs['Ebp'], 16)
        print('eip 0x%08x  %s' % (eip, s.name(eip)))
        for _ in range(args.depth):
            if not ebp:
                break
            ret = read_dword(args.host, ebp + 4)
            nxt = read_dword(args.host, ebp)
            if ret is None:
                break
            print('    0x%08x  %s' % (ret, s.name(ret)))
            if nxt is None or nxt <= ebp:
                break
            ebp = nxt
        esp = int(regs['Esp'], 16)
        for i in range(args.scan):
            w = read_dword(args.host, esp + 4 * i)
            if w is None:
                break
            if CODE_LO <= w < CODE_HI:
                print('  [esp+%03x] 0x%08x  %s' % (4 * i, w, s.name(w)))


if __name__ == '__main__':
    main()
