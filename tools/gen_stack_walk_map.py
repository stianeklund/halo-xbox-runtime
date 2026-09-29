#!/usr/bin/env python3
"""Write the linker map Halo's own stack walker loads (d:\\cachebeta.map).

  python3 tools/gen_stack_walk_map.py build-seed/halo.map build-seed/cachebeta.map

stack_walk_initialize (0x92d30) reads an MSVC-style map at startup and
stack_walk_with_context names every frame from it; without one each frame
prints as "?????". This emits only what load_symbol_table (0x92710) parses:

  - header lines up to one containing "Lib:Object";
  - then one line per symbol, with no blank lines (the parser treats a line
    whose first ':'-token does not start with a space as corrupt):
      " 0001:<value>       <name> <rva> f   <object>"
    <rva> is the absolute VA, which is what the lookup compares; the parser
    skips 5 characters after it before reading <object>.

Symbols below LEGACY_END are the original image, named from kb.json; the rest
come from the lld-link map of the standalone build.

`_load_symbol_table` is written without its leading underscore: on that exact
name the parser sets stack_walk_bias = rva - 0x92710 (the original address),
which is wrong for a relinked image. Without the match the bias stays 0 and
addresses are looked up as-is.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deps_inventory as inv  # noqa: E402
from gen_legacy_image import LEGACY_END  # noqa: E402
from symbolize import MAP_RE  # noqa: E402

# Matched by the parser but its result is discarded (strstr only).
BUILD_TIMESTAMP = 'Thu Aug 23 16:11:48 2001'
MAX_NAME = 0xff


def clean(token, fallback):
    token = (token or '').split()
    token = token[0] if token else fallback
    return token[:MAX_NAME]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('map', help='lld-link map of the standalone build')
    ap.add_argument('out', help='output path (deployed as cachebeta.map)')
    ap.add_argument('--kb', default=os.path.join(inv.ROOT, 'kb.json'))
    args = ap.parse_args()

    syms = {}
    kb = inv.Kb(args.kb)
    for e in kb.funcs:
        if e['addr'] < LEGACY_END:
            syms[e['addr']] = (clean(e['name'], 'FUN_%08x' % e['addr']),
                               clean(e['object'], 'legacy'))

    with open(args.map) as f:
        for line in f:
            if not line.startswith(' 0001:'):
                continue
            m = MAP_RE.match(line)
            if not m:
                continue
            va = int(m.group(2), 16)
            if va < LEGACY_END:
                continue
            syms[va] = (clean(m.group(1), 'sym_%08x' % va),
                        clean(m.group(3), 'standalone'))

    with open(args.out, 'w', newline='\r\n') as f:
        f.write(' halo standalone\n\n')
        f.write(' Timestamp is 00000000 (%s)\n\n' % BUILD_TIMESTAMP)
        f.write('  Address         Publics by Value              Rva+Base       Lib:Object\n')
        for va in sorted(syms):
            name, obj = syms[va]
            if name == '_load_symbol_table':
                name = 'load_symbol_table'
            f.write(' 0001:%08x       %s %08x f   %s\n' % (va, name, va, obj))
    print('wrote %s (%d symbols)' % (args.out, len(syms)))


if __name__ == '__main__':
    main()
