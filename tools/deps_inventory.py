#!/usr/bin/env python3
"""Standalone-build dependency inventory.

Lists everything the reconstructed source still takes from the original
cachebeta.xbe, so the nxdk standalone target (this repository) can burn the list
down.  Three layers:

  1. link    - symbols our objects leave undefined once halo.xbe.lib and the
               generated reg-arg thunks are removed (what lld reports).
  2. fixed   - absolute addresses baked into our compiled code
               (`*(T *)0x253394`, `(void *)0x5a90e0`, ...).  The linker never
               sees these; a standalone image has nothing at those addresses.
  3. closure - the unported Halo functions the layer-1/2 roots pull in
               transitively, walking the ORIGINAL binary's direct calls, plus
               unported functions whose address is stored in .data/.rdata
               (dispatch tables, callbacks).  Library code (D3D, DSOUND, XNET,
               XAPI, CRT, Bink) is a leaf: it gets replaced, not lifted.

Every dependency gets a category:

  halo_code   unported Halo function (must be lifted)
  halo_data   Halo global at a fixed address (must be defined in source,
              initializers recovered from the XBE)
  kernel      xboxkrnl export (nxdk libxboxkrnl provides these)
  xapi        XAPILIB / XPP (nxdk winapi/hal/usb or compat layer)
  d3d8        D3D8 / D3DX8 (pbkit-based compat layer)
  dsound      DirectSound + DOLBY DSP image
  xnet        XNet / winsock (nxdk lwip or stub)
  bink        RAD Bink video (proprietary middleware; replace or stub)
  crt         LIBCMT + compiler runtime helpers (pdclib / xboxrt)
  unknown     not described by kb.json

Usage:
  python3 tools/deps_inventory.py            # after building `halo`
  python3 tools/deps_inventory.py --no-closure --top 20
"""
import argparse
import collections
import concurrent.futures
import glob
import json
import os
import re
import subprocess
import sys

# This repository, and the halo checkout it builds from ($HALO_ROOT, default
# the sibling ../halo-clean-main).
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
ROOT = os.path.abspath(os.environ.get('HALO_ROOT')
                       or os.path.join(REPO, '..', 'halo-clean-main'))

# Objects that exist only to bind the reconstructed code to the original XBE.
# The standalone target replaces them (standalone_entry.c).
XBE_GLUE_OBJECTS = ('thunks.c.obj', 'entrypoint.c.obj')

CATEGORIES = ('halo_code', 'halo_data', 'kernel', 'xapi', 'd3d8', 'dsound',
              'xnet', 'bink', 'crt', 'unknown')

# Compiler-emitted helpers that the original resolved from LIBCMT.
COMPILER_RUNTIME = {
    '_fltused', '_chkstk', '_alloca_probe', '_allmul', '_alldiv', '_allrem',
    '_aulldiv', '_aullrem', '_allshl', '_allshr', '_aullshr', '_ftol',
    '_ftol2', '_ftol2_sse', 'memcpy', 'memset', 'memmove', 'memcmp',
    '_CIpow', '_CIfmod', '_CIsqrt', '_CIsin', '_CIcos', '_CIatan2',
    '_CIacos', '_CIasin', '_CIexp', '_CIlog', '_CItan', '_CIatan',
}

CRT_NAME_RE = re.compile(
    r'^(_[A-Za-z_]|str|wcs|mem|v?s?n?w?printf$|v?sn?printf|sprintf|atoi|atol|'
    r'atof|strtol|strtoul|strtod|qsort|bsearch|s?rand$|tolower|toupper|'
    r'towlower|towupper|is(alpha|digit|space|upper|lower|alnum|print|punct|'
    r'xdigit|cntrl|graph)$|floor|ceil|fmod|pow$|log$|log10|exp$|sin$|cos$|'
    r'tan$|asin|acos|atan|sqrt|fabs|abs$|labs|time$|localtime|gmtime|mktime|'
    r'malloc|free$|calloc|realloc|fopen|fclose|fread|fwrite|fseek|ftell|'
    r'fgets|fputs|fprintf|fflush|setlocale|_control87|_controlfp)')

# Win32/XAPI entry points statically linked into .text.  kb.json sometimes files
# one under the neighbouring Halo object (CloseHandle under d3d_intimacy.obj).
WIN32_API_RE = re.compile(
    r'^(Close|Create|Delete|Find|Get|Set|Read|Write|Wait|Sleep|Query|Reset|'
    r'Resume|Suspend|Exit|Output|Mount|Unmount|Duplicate|Flush|Move|Copy|'
    r'Interlocked|Tls|Local|Global|Heap|Virtual|Queue|Release|Open|Signal|'
    r'Terminate|Switch|Remove|Lock|Unlock|Initialize|Enter|Leave|Map|Free|'
    r'Alloc|Load|Format|Xapi|XInput|XGet|XSet|XLaunch|XMount|XUnmount|XFormat|'
    r'XCreate|XDelete|XFind|XSign|XCalculate|XAuto|XDevice|XQuery|XInit|'
    r'XNet|XOnline|XPhysical|XSave|XFree|XLoad|XRegister)[A-Z][A-Za-z0-9]*$')


# ---------------------------------------------------------------- kb.json

def decl_name(decl, is_func):
    """Identifier a kb.json declaration introduces."""
    d = decl.split('=')[0].strip().rstrip(';').strip()
    if is_func:
        m = re.search(r'\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(', d)  # fn-ptr return
        if m:
            return m.group(1)
        depth = 0
        for i, ch in enumerate(d):
            if ch == '(':
                head = d[:i].rstrip()
                m = re.search(r'([A-Za-z_]\w*)$', head)
                if m and m.group(1) not in ('__declspec', '__attribute__'):
                    if depth == 0:
                        return m.group(1)
                depth += 1
            elif ch == ')':
                depth -= 1
        return None
    m = re.search(r'\(\s*\*\s*([A-Za-z_]\w*)', d)  # function-pointer variable
    if m:
        return m.group(1)
    d = re.sub(r'\[[^\]]*\]', '', d).strip()
    m = re.search(r'([A-Za-z_]\w*)\s*$', d)
    return m.group(1) if m else None


class Kb:
    def __init__(self, path):
        with open(path) as f:
            raw = json.load(f)
        self.by_name = {}
        self.by_addr = {}
        self.funcs = []   # (addr, entry)
        self.data = []

        def add(entry):
            name = decl_name(entry['decl'], entry['kind'] == 'func')
            if not name:
                return
            entry['name'] = name
            self.by_name.setdefault(name, entry)
            self.by_addr.setdefault(entry['addr'], entry)
            (self.funcs if entry['kind'] == 'func' else self.data).append(entry)

        for obj in raw.get('objects', []):
            oname = obj.get('name') or '<null>'
            for fn in obj.get('functions', []):
                add({'kind': 'func', 'addr': int(fn['addr'], 16), 'decl': fn['decl'],
                     'object': oname, 'ported': bool(fn.get('ported'))})
            for dt in obj.get('data', []):
                add({'kind': 'data', 'addr': int(dt['addr'], 16), 'decl': dt['decl'],
                     'object': oname, 'ported': False})
        for key, val in raw.items():
            # Legacy top-level entries: an object entry at the same address
            # wins. Adding both leaves a zero-length duplicate that turns
            # the real body into a missing stub.
            if key.startswith('0x') and int(key, 16) in self.by_addr:
                continue
            if key.startswith('0x') and isinstance(val, dict) and 'decl' in val:
                kind = 'func' if '(' in val['decl'] and 'ported' in val else 'data'
                add({'kind': kind, 'addr': int(key, 16), 'decl': val['decl'],
                     'object': val.get('obj') or '<null>',
                     'ported': bool(val.get('ported'))})
        self.funcs.sort(key=lambda e: e['addr'])
        self.func_starts = [e['addr'] for e in self.funcs]


# ---------------------------------------------------------------- XBE

class Image:
    """Original XBE mapped by virtual address (read through pyxbe)."""

    def __init__(self, path):
        from xbe import Xbe
        x = Xbe.from_file(path)
        self.base = x.header.base_addr
        self.end = self.base + x.header.image_size
        self.sections = []
        for name, s in x.sections.items():
            va = s.header.virtual_addr
            self.sections.append((va, va + s.header.virtual_size, name, bytes(s.data)))
        self.sections.sort()

    def initialized_end(self, va):
        """End of the file-backed (initialized) part of the section holding va."""
        for lo, hi, name, data in self.sections:
            if lo <= va < hi:
                return lo + len(data.rstrip(b'\0'))
        return va

    def section_of(self, va):
        for lo, hi, name, _ in self.sections:
            if lo <= va < hi:
                return name
        return None

    def read(self, va, n):
        for lo, hi, _, data in self.sections:
            if lo <= va < hi:
                off = va - lo
                return data[off:off + n]
        return b''


SECTION_CATEGORY = {
    'D3D': 'd3d8', 'D3DX': 'd3d8', 'DSOUND': 'dsound', 'DOLBY': 'dsound',
    'XNET': 'xnet', 'XPP': 'xapi', 'BINKDATA': 'bink',
}


class Classifier:
    def __init__(self, kb, image, kernel_names):
        self.kb = kb
        self.image = image
        self.kernel = kernel_names
        lib_text = [e['addr'] for e in kb.funcs
                    if image.section_of(e['addr']) == '.text'
                    and (':' in e['object'] or e['object'].startswith('<xdk'))]
        # First statically linked library function in .text; Halo code is below.
        self.halo_text_end = min(lib_text) if lib_text else image.end

    def by_address(self, va, obj, name, is_func):
        sec = self.image.section_of(va)
        if sec is None:
            return 'unknown'
        if sec.startswith('BINK'):
            return 'bink'
        if sec in SECTION_CATEGORY:
            return SECTION_CATEGORY[sec]
        prefix = obj.split(':')[0] if ':' in obj else ''
        if prefix == 'LIBCMT':
            return 'crt'
        if prefix == 'XAPILIB':
            return 'xapi'
        if prefix == 'XNET':
            return 'xnet'
        if prefix in ('D3D8', 'D3DX8'):
            return 'd3d8'
        if prefix in ('DSOUND', 'DSOUNDH'):
            return 'dsound'
        if name and (name.startswith('D3D') or name.startswith('g_pDevice')):
            return 'd3d8'
        if name and (name.startswith('DirectSound') or name.startswith('IDirectSound')):
            return 'dsound'
        if sec == '.text':
            if name and WIN32_API_RE.match(name):
                return 'xapi'
            if va < self.halo_text_end and not obj.startswith('<xdk'):
                return 'halo_code'
            if name and CRT_NAME_RE.match(name):
                return 'crt'
            return 'xapi'
        # .data / .rdata
        if obj.startswith('<xdk'):
            return 'xapi'
        return 'halo_data' if not is_func else 'halo_code'

    def symbol(self, name, is_data_import):
        if name in self.kernel:
            return 'kernel', None
        e = self.kb.by_name.get(name)
        if e is None:
            if name in COMPILER_RUNTIME:
                return 'crt', None
            m = re.match(r'^(FUN|DAT|byte|word|dword|flt|dbl|off|unk)_([0-9A-Fa-f]{5,8})$', name)
            if m:
                va = int(m.group(2), 16)
                return self.by_address(va, '<unnamed>', name, m.group(1) == 'FUN'), va
            return 'unknown', None
        return self.by_address(e['addr'], e['object'], name, e['kind'] == 'func'), e['addr']


# ---------------------------------------------------------------- objects

def normalize(sym):
    if sym.startswith('__imp_'):
        sym = sym[len('__imp_'):]
    if sym.startswith('@'):
        return sym[1:].split('@')[0]
    if sym.startswith('_'):
        sym = sym[1:]
        return re.sub(r'@\d+$', '', sym)
    return None


def nm(path):
    out = subprocess.run(['llvm-nm', '-P', path], capture_output=True, text=True,
                         check=True).stdout
    defined, undefined = set(), {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        raw, typ = parts[0], parts[1]
        name = normalize(raw)
        if name is None:
            continue
        if typ == 'U':
            undefined.setdefault(name, raw)
        elif typ in 'TDBRCWVAG' or typ in 'SI':
            defined.add(name)
    return defined, undefined


HEX_RE = re.compile(r'0x([0-9a-f]+)')
INSN_RE = re.compile(r'^\s*[0-9a-f]+:\s+(\S.*)$')
FUNC_LABEL_RE = re.compile(r'^[0-9a-f]+ <(.+)>:$')


def fixed_address_scan(path, lo, hi):
    """(function, va, kind) for absolute image addresses in compiled code.

    kind is 'mem' for a memory operand ([0x...]) or 'imm' for an immediate.
    Relocated operands are skipped: the linker resolves those."""
    out = subprocess.run(
        ['llvm-objdump', '-d', '-r', '--no-show-raw-insn', '--x86-asm-syntax=intel', path],
        capture_output=True, text=True, check=True).stdout
    hits = []
    func = None
    lines = out.splitlines()
    for i, line in enumerate(lines):
        m = FUNC_LABEL_RE.match(line)
        if m:
            func = normalize(m.group(1)) or m.group(1)
            continue
        im = INSN_RE.match(line)
        if not im or 'IMAGE_REL_I386' in line:
            continue
        nxt = lines[i + 1] if i + 1 < len(lines) else ''
        if 'IMAGE_REL_I386' in nxt:
            continue
        insn = im.group(1).replace('\t', ' ').strip()
        mnem = insn.split(None, 1)[0] if insn else ''
        if mnem.startswith('j') or (mnem == 'call' and '[' not in insn):
            continue
        for hm in HEX_RE.finditer(insn):
            va = int(hm.group(1), 16)
            if lo <= va < hi:
                start = insn.rfind('[', 0, hm.start())
                kind = 'mem' if start != -1 and insn.find(']', start) > hm.start() else 'imm'
                hits.append((func, va, kind))
    return hits


# ---------------------------------------------------------------- closure

def function_extent(kb, idx, image):
    start = kb.func_starts[idx]
    end = kb.func_starts[idx + 1] if idx + 1 < len(kb.func_starts) else start + 0x1000
    sec_end = next((hi for lo, hi, _, _ in image.sections if lo <= start < hi), start)
    return start, min(end, sec_end, start + 0x10000)


def scan_original_function(kb, idx, image, md):
    start, end = function_extent(kb, idx, image)
    code = image.read(start, end - start)
    calls, refs = set(), set()
    for insn in md.disasm(code, start):
        if insn.mnemonic in ('call', 'jmp') and insn.op_str.startswith('0x'):
            tgt = int(insn.op_str, 16)
            if not (start <= tgt < end):
                calls.add(tgt)
            continue
        for hm in HEX_RE.finditer(insn.op_str):
            va = int(hm.group(1), 16)
            if image.base <= va < image.end:
                refs.add(va)
    return end - start, calls, refs


def data_table_refs(kb, image, wanted):
    """Unported Halo functions whose address is stored in .data/.rdata."""
    import struct
    hits = collections.defaultdict(list)
    for lo, hi, name, data in image.sections:
        if name not in ('.data', '.rdata'):
            continue
        n = len(data) // 4
        for i, va in enumerate(struct.unpack_from('<%dI' % n, data)):
            if va in wanted:
                hits[va].append(lo + i * 4)
    return hits


def build_closure(kb, image, clf, roots):
    import bisect
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    idx_of = {a: i for i, a in enumerate(kb.func_starts)}
    need = {}
    lib = collections.defaultdict(set)
    data_refs = collections.defaultdict(set)
    work = list(roots.items())
    while work:
        va, why = work.pop()
        if va in need or va not in idx_of:
            continue
        e = kb.funcs[idx_of[va]]
        if e['ported']:
            continue
        size, calls, refs = scan_original_function(kb, idx_of[va], image, md)
        need[va] = {'name': e['name'], 'object': e['object'], 'size': size, 'why': why}
        for tgt in calls:
            if tgt not in idx_of:
                i = bisect.bisect_right(kb.func_starts, tgt) - 1
                if i < 0:
                    continue
                tgt_e = kb.funcs[i]
            else:
                tgt_e = kb.funcs[idx_of[tgt]]
            cat = clf.by_address(tgt_e['addr'], tgt_e['object'], tgt_e['name'], True)
            if cat == 'halo_code':
                if not tgt_e['ported']:
                    work.append((tgt_e['addr'], 'call:%s' % e['name']))
            else:
                lib[cat].add(tgt_e['name'])
        for ref in refs:
            sec = image.section_of(ref)
            if sec in ('.data', '.rdata'):
                data_refs[ref].add(e['name'])
    return need, lib, data_refs


# ---------------------------------------------------------------- main

def collect_objects(build_dir, object_dir):
    objs = sorted(glob.glob(os.path.join(build_dir, object_dir, '**', '*.obj'), recursive=True))
    return [o for o in objs if os.path.basename(o) not in XBE_GLUE_OBJECTS]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--build-dir', default=os.path.join(REPO, 'build'))
    ap.add_argument('--object-dir', default=os.path.join('CMakeFiles', 'halo.dir'),
                    help='object directory relative to --build-dir')
    ap.add_argument('--kb', default=os.path.join(ROOT, 'kb.json'))
    ap.add_argument('--xbe', default=os.path.join(ROOT, 'halo-patched', 'cachebeta.xbe'))
    ap.add_argument('--kernel-def', default=os.path.join(ROOT, 'src', 'xboxkrnl.exe.def'))
    ap.add_argument('--json-out', default=os.path.join(REPO, 'artifacts',
                                                       'deps_inventory.json'))
    ap.add_argument('--no-closure', action='store_true')
    ap.add_argument('--top', type=int, default=15, help='rows per table in the summary')
    args = ap.parse_args()

    objs = collect_objects(args.build_dir, args.object_dir)
    if not objs:
        sys.exit('no objects under %s; build the `halo` target first' % args.build_dir)

    kb = Kb(args.kb)
    image = Image(args.xbe)
    kernel = set()
    with open(args.kernel_def) as f:
        for line in f:
            tok = line.split()
            if tok and not tok[0].startswith((';', 'LIBRARY', 'EXPORTS')):
                # Undecorated .def entries (DbgPrint) are cdecl: lld adds the '_'.
                n = normalize(tok[0] if tok[0].startswith(('_', '@')) else '_' + tok[0])
                if n:
                    kernel.add(n)
    clf = Classifier(kb, image, kernel)

    defined = set()
    undefined = collections.defaultdict(set)   # name -> referencing objects
    raw_names = {}
    fixed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        nm_results = list(ex.map(nm, objs))
        scans = list(ex.map(lambda o: fixed_address_scan(o, image.base, image.end), objs))
    for o, (d, u), hits in zip(objs, nm_results, scans):
        rel = os.path.relpath(o, os.path.join(args.build_dir, args.object_dir))
        defined |= d
        for name, raw in u.items():
            undefined[name].add(rel)
            raw_names[name] = raw
        for func, va, kind in hits:
            fixed.append((rel, func, va, kind))

    link = {}
    for name in sorted(set(undefined) - defined):
        cat, va = clf.symbol(name, raw_names[name].startswith('__imp_'))
        link[name] = {'category': cat, 'addr': va, 'symbol': raw_names[name],
                      'referenced_by': sorted(undefined[name])}

    fixed_rows = collections.OrderedDict()
    for rel, func, va, kind in fixed:
        e = kb.by_addr.get(va)
        name = e['name'] if e else None
        obj = e['object'] if e else '<unnamed>'
        is_func = bool(e and e['kind'] == 'func')
        cat = clf.by_address(va, obj, name, is_func)
        if kind == 'imm' and not e:
            cat = 'maybe_constant'   # in-range immediate with no kb symbol
        if cat == 'halo_data':
            # Zero-initialized storage needs no recovered initializer.
            cat = 'halo_data_bss' if va >= image.initialized_end(va) else 'halo_data_init'
        row = fixed_rows.setdefault(va, {'addr': va, 'name': name, 'category': cat,
                                         'kinds': set(), 'sites': set()})
        row['kinds'].add(kind)
        row['sites'].add('%s:%s' % (rel, func))

    closure = None
    if not args.no_closure:
        roots = {}
        for name, row in link.items():
            if row['category'] == 'halo_code' and row['addr'] is not None:
                roots[row['addr']] = 'link:%s' % name
        for va, row in fixed_rows.items():
            if row['category'] == 'halo_code':
                roots.setdefault(va, 'fixed:%s' % sorted(row['sites'])[0])
        unported_halo = {e['addr'] for e in kb.funcs
                         if not e['ported'] and clf.by_address(
                             e['addr'], e['object'], e['name'], True) == 'halo_code'}
        for va, where in data_table_refs(kb, image, unported_halo).items():
            roots.setdefault(va, 'data_table:0x%x' % where[0])
        need, lib, data_refs = build_closure(kb, image, clf, roots)
        closure = {'functions': need, 'library_leaves': lib, 'data_refs': data_refs,
                   'unported_halo_total': len(unported_halo)}

    # ------------------------------------------------------------ report
    by_cat = collections.Counter(r['category'] for r in link.values())
    print('== layer 1: link-level undefined symbols (%d objects, glue excluded) ==' % len(objs))
    for c in CATEGORIES:
        if by_cat[c]:
            names = [n for n, r in link.items() if r['category'] == c]
            print('  %-10s %5d  e.g. %s' % (c, by_cat[c], ', '.join(names[:6])))

    fcat = collections.Counter(r['category'] for r in fixed_rows.values())
    nsites = sum(len(r['sites']) for r in fixed_rows.values())
    print('\n== layer 2: fixed image addresses in compiled code (%d distinct, %d sites) =='
          % (len(fixed_rows), nsites))
    for c, n in fcat.most_common():
        print('  %-14s %5d' % (c, n))
    worst = collections.Counter()
    for r in fixed_rows.values():
        for s in r['sites']:
            worst[s.split(':')[0]] += 1
    print('  top TUs:', ', '.join('%s(%d)' % (os.path.basename(t).replace('.c.obj', ''), n)
                                  for t, n in worst.most_common(args.top)))

    if closure:
        need = closure['functions']
        size = sum(v['size'] for v in need.values())
        print('\n== layer 3: transitive closure over the original binary ==')
        print('  unported Halo functions required: %d of %d unported (~%d KiB of x86)'
              % (len(need), closure['unported_halo_total'], size // 1024))
        why = collections.Counter(v['why'].split(':')[0] for v in need.values())
        print('  reached via:', dict(why))
        objc = collections.Counter(v['object'] for v in need.values())
        print('  by object:', ', '.join('%s(%d)' % kv for kv in objc.most_common(args.top)))
        for c in CATEGORIES:
            if closure['library_leaves'].get(c):
                print('  leaf %-7s %4d functions' % (c, len(closure['library_leaves'][c])))
        print('  distinct .data/.rdata addresses touched by required unported code: %d'
              % len(closure['data_refs']))

    os.makedirs(os.path.dirname(args.json_out), exist_ok=True)
    out = {
        'objects': len(objs),
        'link': link,
        'fixed': [dict(r, kinds=sorted(r['kinds']), sites=sorted(r['sites']),
                       addr='0x%x' % r['addr']) for r in fixed_rows.values()],
    }
    for r in out['link'].values():
        if r['addr'] is not None:
            r['addr'] = '0x%x' % r['addr']
    if closure:
        out['closure'] = {
            'unported_halo_total': closure['unported_halo_total'],
            'functions': {'0x%x' % k: v for k, v in sorted(closure['functions'].items())},
            'library_leaves': {k: sorted(v) for k, v in closure['library_leaves'].items()},
            'data_refs': {'0x%x' % k: sorted(v) for k, v in sorted(closure['data_refs'].items())},
        }
    with open(args.json_out, 'w') as f:
        json.dump(out, f, indent=1, sort_keys=True)
    print('\nwrote', os.path.relpath(args.json_out, REPO))


if __name__ == '__main__':
    main()
