#!/usr/bin/env python3
"""Decode the first-exception snapshot a standalone build keeps in memory.

standalone_report_exception (standalone_runtime.c) copies the
EXCEPTION_RECORD, CONTEXT and 256 stack words when main()'s __except filter
runs, before the unwind reuses that stack. This reads them back over XBDM and
lists every stack word that points into code, since frame-pointer-omitted
original code leaves no EBP chain to walk.

  python3 tools/crash_report.py build-seed/halo.map --host 10.0.0.24
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deps_inventory as inv  # noqa: E402
from missing_dump import getmem  # noqa: E402
from symbolize import MAP_RE, Symbolizer  # noqa: E402

CONTEXT_REGS_OFFSET = 0x208  # Xbox CONTEXT: ContextFlags, FloatSave[0x204], Edi..
CONTEXT_REGS = ['edi', 'esi', 'ebx', 'edx', 'ecx', 'eax', 'ebp', 'eip', 'cs', 'eflags', 'esp', 'ss']
STACK_WORDS = 256
CODE_LO, CODE_HI = 0x11000, 0x900000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('map')
    ap.add_argument('--host', required=True)
    ap.add_argument('--kb', default=os.path.join(inv.ROOT, 'kb.json'))
    args = ap.parse_args()
    syms = {}
    with open(args.map) as f:
        for line in f:
            m = MAP_RE.match(line)
            if m:
                syms[m.group(1)] = int(m.group(2), 16)
    count = struct.unpack('<I', getmem(args.host, syms['_halo_crash_count'], 4))[0]
    if not count:
        print('no exception captured')
        return
    sym = Symbolizer(args.map, args.kb)
    record = struct.unpack('<20I', getmem(args.host, syms['_halo_crash_record'], 0x50))
    regs = struct.unpack('<12I', getmem(args.host, syms['_halo_crash_context'] + CONTEXT_REGS_OFFSET, 48))
    stack = struct.unpack('<%dI' % STACK_WORDS, getmem(args.host, syms['_halo_crash_stack'], 4 * STACK_WORDS))
    nparams = min(record[4], 15)
    print('%d exception(s); first: code 0x%08x at 0x%08x %s' % (count, record[0], record[3], sym.name(record[3])))
    print('  params %s' % ' '.join('0x%x' % p for p in record[5:5 + nparams]))
    print('  ' + ' '.join('%s=%08x' % nv for nv in zip(CONTEXT_REGS, regs)))
    print('  eip %s' % sym.name(regs[7]))
    for i, w in enumerate(stack):
        if CODE_LO <= w < CODE_HI:
            print('  [esp+%03x] %08x  %s' % (4 * i, w, sym.name(w)))


if __name__ == '__main__':
    main()
