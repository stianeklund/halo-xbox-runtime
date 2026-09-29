#!/usr/bin/env python3
"""Boot an XBE, break on an exception filter and report the exception.

Used when XBDM's notify channel misses the early boot: sets an execute
breakpoint on `--symbol` (a filter taking (code, EXCEPTION_POINTERS *) as its
first two stack arguments), then decodes the EXCEPTION_RECORD and CONTEXT.

  python3 tools/xbdm_catch_filter.py build-seed/halo.map \
      --host 10.0.0.21 --title 'E:\\GAMES\\halo-patched\\standalone_seed.xbe'
"""
import argparse
import os
import re
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deps_inventory as inv  # noqa: E402
from symbolize import MAP_RE, Symbolizer, rdcp, read_dword  # noqa: E402


def try_rdcp(host, cmd):
    try:
        return rdcp(host, cmd)
    except OSError:
        return ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('map')
    ap.add_argument('--host', required=True)
    ap.add_argument('--title', required=True)
    ap.add_argument('--symbol', default='_FUN_0008e5f0')
    ap.add_argument('--kb', default=os.path.join(inv.ROOT, 'kb.json'))
    ap.add_argument('--seconds', type=float, default=60)
    args = ap.parse_args()

    addr = None
    with open(args.map) as f:
        for line in f:
            m = MAP_RE.match(line)
            if m and m.group(1) == args.symbol:
                addr = int(m.group(2), 16)
    if addr is None:
        sys.exit('symbol %s not in map' % args.symbol)

    print(try_rdcp(args.host, 'magicboot title=%s debug' % args.title).strip())
    deadline = time.time() + args.seconds
    # The reboot drops XBDM; wait for it to go away and come back so the
    # breakpoint lands in the new title (a `debug` boot halts at its entry).
    while time.time() < deadline and try_rdcp(args.host, 'threads'):
        time.sleep(0.3)
    while time.time() < deadline and not try_rdcp(args.host, 'threads'):
        time.sleep(0.5)
    time.sleep(1.5)
    # XBDM only stops on exceptions while a debugger (notify channel) is attached.
    notify = socket.create_connection((args.host, 731), timeout=5)
    notify.recv(1024)
    notify.sendall(b'notify\r\n')
    notify.recv(1024)
    print(try_rdcp(args.host, 'break addr=0x%x' % addr).strip())
    print(try_rdcp(args.host, 'stopon fce').strip())
    print(try_rdcp(args.host, 'go').strip())

    while time.time() < deadline:
        threads = re.findall(r'^(\d+)\s*$', try_rdcp(args.host, 'threads'), re.M)
        for t in threads:
            # A stopped thread answers with its stop reason ("200- break addr=...",
            # "200- exception code=..."); a running one with an error / "not stopped".
            stop = try_rdcp(args.host, 'isstopped thread=%s' % t)
            if not stop.startswith('200') or 'not stopped' in stop.lower():
                continue
            ctx = try_rdcp(args.host, 'getcontext thread=%s control int' % t)
            regs = dict((k.lower(), int(v, 16)) for k, v in re.findall(r'(\w+)=0x([0-9a-fA-F]+)', ctx))
            if regs.get('eip') != addr:
                sym = Symbolizer(args.map, args.kb)
                print('thread %s stopped elsewhere: %s eip %s' % (
                    t, stop.strip(), sym.name(regs.get('eip', 0))))
                ebp = regs.get('ebp', 0)
                for _ in range(16):
                    if not ebp:
                        break
                    ret = read_dword(args.host, ebp + 4)
                    print('    0x%08x  %s' % (ret, sym.name(ret)))
                    ebp = read_dword(args.host, ebp)
                return
            if True:
                esp = regs['esp']
                code = read_dword(args.host, esp + 4)
                ptrs = read_dword(args.host, esp + 8)
                record = read_dword(args.host, ptrs)
                context = read_dword(args.host, ptrs + 4)
                exc_addr = read_dword(args.host, record + 12)
                nparams = read_dword(args.host, record + 16)
                params = [read_dword(args.host, record + 20 + 4 * i) for i in range(min(nparams, 2))]
                sym = Symbolizer(args.map, args.kb)
                print('thread %s: code 0x%08x at 0x%08x %s params %s' % (
                    t, code, exc_addr, sym.name(exc_addr), ['0x%x' % p for p in params]))
                # Xbox CONTEXT: ContextFlags, FloatSave[0x204], Edi..Eax, Ebp, Eip, ...
                names = ['edi', 'esi', 'ebx', 'edx', 'ecx', 'eax', 'ebp', 'eip', 'cs', 'eflags', 'esp']
                vals = [read_dword(args.host, context + 0x208 + 4 * i) for i in range(len(names))]
                print('  ' + ' '.join('%s=%08x' % nv for nv in zip(names, vals)))
                ebp = vals[6]
                for _ in range(16):
                    if not ebp:
                        break
                    ret = read_dword(args.host, ebp + 4)
                    print('    0x%08x  %s' % (ret, sym.name(ret)))
                    ebp = read_dword(args.host, ebp)
                return
        time.sleep(0.5)
    print('no stop on %s within %ss' % (args.symbol, args.seconds))


if __name__ == '__main__':
    main()
