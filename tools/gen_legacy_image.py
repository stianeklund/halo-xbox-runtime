#!/usr/bin/env python3
"""Generate the legacy-layout objects for the nxdk standalone build.

The reconstructed source still addresses Halo globals and tables at their
original cachebeta.xbe virtual addresses (both through kb.json data symbols and
through raw `*(T *)0x2c84c0` casts).  Instead of rewriting ~14k sites before the
first boot, the standalone image reserves the original image range
[LEGACY_BASE, LEGACY_END) as the first contribution of the PE .text section
(lld-link keeps same-named input sections in command-line order, so the object
generated here must be linked first).  Our own code and data follow at
LEGACY_END.

Inside the legacy range:

  * original code range  - a `jmp` at every kb.json function start, to our C
                           implementation when one exists, else to a logging
                           stub.  Raw function-pointer constants and function
                           tables in .data keep working without relocation.
  * .rdata/.data/...     - zero, or (dev mode, --seed-xbe) the initialized bytes
                           of the user's cachebeta.xbe.  Seeding is an explicit
                           standalone blocker; the report counts every seeded
                           byte so it can be burned down with recovered data.
  * kb.json data symbols - resolved to their original address inside the range.

Every other unresolved symbol becomes a stub that logs its name once through
DbgPrint and returns 0 with the callee-pops size implied by its decoration, so
bring-up can proceed past missing code.  Symbols the linked nxdk libraries
already define are left to them.

Outputs: <out>/legacy.s, <out>/stubs.s, <out>/seed_*.bin, <out>/link_report.json
"""
import argparse
import bisect
import collections
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deps_inventory as inv  # noqa: E402

try:
    import capstone
except ImportError:  # checked in main(): stub ABI needs it whenever --xbe is given
    capstone = None

LEGACY_BASE = 0x11000      # first byte after the XBE header page
LEGACY_END = 0x642000      # end of the last original section ($$XSIMAGE), page aligned
SEED_SECTIONS = ('.rdata', '.data', 'DOLBY', 'BINKDATA')
REG_ANNOT_RE = re.compile(r'@\s*<\s*[a-z]+\s*>')


def lib_symbols(paths):
    names = {}
    for p in paths:
        out = subprocess.run(['llvm-nm', '-P', '--defined-only', p], capture_output=True,
                             text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            # Absolute definitions are linkable symbols too. nxdk exports
            # __tls_array as A=4 (an FS offset), never as a function address.
            if len(parts) >= 2 and parts[1] in 'TDBRWCA':
                names.setdefault(parts[0], os.path.basename(p))
    return names


def callee_pops(raw):
    m = re.match(r'^_[^@]+@(\d+)$', raw)
    if m:
        return int(m.group(1))
    m = re.match(r'^@[^@]+@(\d+)$', raw)
    if m:
        return max(int(m.group(1)) - 8, 0)
    return 0


def original_pops(image, va, end):
    """Bytes the original routine at `va` pops on return, read from its own
    `ret` / `ret N` instructions (recursive descent bounded by [va, end)).
    None when no return is reachable or the returns disagree."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code = image.read(va, end - va)
    if not code:
        return None
    pops, seen, work = set(), set(), [va]
    while work and len(seen) < 4096:
        pc = work.pop()
        while va <= pc < end and pc not in seen:
            seen.add(pc)
            insn = next(md.disasm(code[pc - va:pc - va + 16], pc), None)
            if insn is None:
                break
            m = insn.mnemonic
            if m in ('ret', 'retn'):
                pops.add(int(insn.op_str, 0) if insn.op_str else 0)
                break
            target = None
            if (m == 'jmp' or m.startswith('j') or m.startswith('loop')) and \
                    insn.op_str.startswith('0x'):
                target = int(insn.op_str, 16)
            if m == 'jmp':
                if target is not None:
                    work.append(target)
                break
            if target is not None:
                work.append(target)
            if m in ('int3', 'hlt', 'ud2'):
                break
            pc += insn.size
    return pops.pop() if len(pops) == 1 else None


def unknown_data_referenced_functions(image, starts, code_end):
    """Function entry points that only data references (function pointer
    tables in .rdata/.data) and kb.json does not list.

    The legacy range holds nothing but trampolines at kb.json starts, so a call
    through such a pointer lands in zero padding and slides into the next
    trampoline, silently running an unrelated function. A candidate counts when
    the preceding kb function's own control flow never reaches it (so it is not
    a switch-table case label) and it follows padding or a return."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    known = set(starts)
    values = set()
    for lo, hi, name, _ in image.sections:
        if name in ('.rdata', '.data'):
            data = image.read(lo, hi - lo) or b''
            for off in range(0, len(data) - 3, 4):
                v = int.from_bytes(data[off:off + 4], 'little')
                if LEGACY_BASE <= v < code_end and v not in known:
                    values.add(v)

    def reached(va, end):
        code = image.read(va, end - va) or b''
        seen, work = set(), [va]
        while work:
            pc = work.pop()
            while va <= pc < end and pc not in seen:
                insn = next(md.disasm(code[pc - va:pc - va + 16], pc), None)
                if insn is None:
                    break
                seen.add(pc)
                m = insn.mnemonic
                if m in ('ret', 'retn'):
                    break
                if (m.startswith('j') or m.startswith('loop')) and insn.op_str.startswith('0x'):
                    work.append(int(insn.op_str, 16))
                if m in ('jmp', 'int3', 'hlt', 'ud2'):
                    break
                pc += insn.size
        return seen

    body, found = {}, []
    for v in sorted(values):
        i = bisect.bisect_right(starts, v) - 1
        if i < 0:
            continue
        s, e = starts[i], starts[i + 1] if i + 1 < len(starts) else code_end
        if s not in body:
            body[s] = reached(s, e)
        if v in body[s] or v - s < 5 or e - v < 5:
            continue
        if found and v - found[-1][0] < 5:
            continue
        if image.read(v - 1, 1) in (b'\xcc', b'\x90', b'\xc3') or image.read(v - 3, 1) == b'\xc2':
            found.append((v, e))
    return found


_WIDE_ARG_RE = re.compile(r'\b(double|__int64|LONGLONG|ULONGLONG|LARGE_INTEGER)\b')


def decl_stack_bytes(decl):
    """Argument bytes of a kb.json decl whose params are all 32-bit, else None."""
    lo, hi = decl.find('('), decl.rfind(')')
    if lo < 0 or hi < lo:
        return None
    params, depth, cur = [], 0, ''
    for c in decl[lo + 1:hi]:
        depth += (c == '(') - (c == ')')
        if c == ',' and depth == 0:
            params.append(cur)
            cur = ''
        else:
            cur += c
    params.append(cur)
    params = [p.strip() for p in params if p.strip() not in ('', 'void')]
    if any('...' in p or ('*' not in p and _WIDE_ARG_RE.search(p)) for p in params):
        return None
    return 4 * len(params)


def asm_str(s):
    return '"%s"' % s.replace('\\', '\\\\').replace('"', '\\"')


_RVTHUNK_MARKER = 0x7eadbe00
_kb_symbols = None


def reverse_thunk_asm(kb_name, impl_raw):
    """Register-ABI entry for an implemented @<reg> function: the reference
    build's reverse thunk (tools/build/patch.py), relocated to `impl_raw`."""
    global _kb_symbols
    sys.path.insert(0, os.path.join(inv.ROOT, 'tools'))
    sys.path.insert(0, os.path.join(inv.ROOT, 'tools', 'build'))
    import patch  # noqa: E402
    from analysis.knowledge import KnowledgeBase  # noqa: E402
    if _kb_symbols is None:
        _kb_symbols = {sy.name: sy for sy in KnowledgeBase.deserialize().symbols}
    sy = _kb_symbols.get(kb_name)
    if sy is None or not getattr(sy, 'requires_reg_thunk', False):
        return None
    code = patch.generate_reverse_thunk(sy, _RVTHUNK_MARKER, 0)
    calls = [i for i in range(len(code) - 4) if code[i] == 0xE8 and
             int.from_bytes(code[i + 1:i + 5], 'little') == (_RVTHUNK_MARKER - (i + 5)) & 0xffffffff]
    if len(calls) != 1:
        return None
    i = calls[0]
    body = ''.join('\t.byte 0x%02x\n' % b for b in code[:i + 1])
    body += '\t.long %s - (. + 4)\n' % impl_raw
    body += ''.join('\t.byte 0x%02x\n' % b for b in code[i + 5:])
    return body


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--kb', required=True)
    ap.add_argument('--objects', required=True, help='file listing object paths (; or newline)')
    ap.add_argument('--libs', default='', help='comma separated nxdk libraries that will be linked')
    ap.add_argument('--xbe', help='cachebeta.xbe, used for section layout and classification')
    ap.add_argument('--seed-xbe', help='DEV ONLY: seed initialized legacy data from this XBE')
    ap.add_argument('--kernel-def', required=True)
    ap.add_argument('--out', required=True)
    args = ap.parse_args()

    with open(args.objects) as f:
        objs = [o for o in re.split(r'[;\n]', f.read()) if o.strip()]
    kb = inv.Kb(args.kb)
    kernel = set()
    with open(args.kernel_def) as f:
        for line in f:
            tok = line.split()
            if tok and not tok[0].startswith((';', 'LIBRARY', 'EXPORTS')):
                n = inv.normalize('_' + tok[0] if not tok[0].startswith(('_', '@')) else tok[0])
                if n:
                    kernel.add(n)

    kernel_by_ordinal = {}
    with open(args.kernel_def) as f:
        for line in f:
            m = re.match(r'\s*(\S+)\s+@\s*(\d+)', line)
            if m:
                sym = m.group(1)
                kernel_by_ordinal[int(m.group(2))] = sym if sym.startswith(('_', '@')) else '_' + sym

    defined_raw = {}          # normalized -> raw decorated name
    undefined_raw = {}
    for o in objs:
        out = subprocess.run(['llvm-nm', '-P', o], capture_output=True, text=True,
                             check=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            raw, typ = parts[0], parts[1]
            name = inv.normalize(raw)
            if name is None:
                continue
            if typ == 'U':
                undefined_raw.setdefault(raw, name)
            elif typ in 'TDBRCWA':
                defined_raw.setdefault(name, raw)
    defined_names = set(defined_raw)
    lib_defs = lib_symbols([p for p in args.libs.split(',') if p])

    if args.xbe and capstone is None:
        # Without it every missing __stdcall stub would silently return with
        # the wrong pop count (see original_pops).
        sys.exit('gen_legacy_image.py: --xbe needs the capstone module (pip install capstone) '
                 'for %s' % sys.executable)
    image = inv.Image(args.xbe) if args.xbe else None
    clf = inv.Classifier(kb, image, kernel) if image else None

    os.makedirs(args.out, exist_ok=True)
    stubs = collections.OrderedDict()   # stub symbol -> (display name, pops)
    report = {'provider': collections.Counter(), 'symbols': {}}

    def stub_for(raw, display):
        stubs.setdefault(raw, (display, callee_pops(raw)))
        return raw

    bridges = collections.OrderedDict()   # symbol -> asm body

    def kernel_forwarder(va):
        """Kernel import an original XAPI routine is a bare `jmp [thunk]` to."""
        if image is None or va is None:
            return None
        code = image.read(va, 6)
        if len(code) < 6 or code[:2] != b'\xff\x25':
            return None
        slot = int.from_bytes(code[2:6], 'little')
        val = int.from_bytes(image.read(slot, 4) or b'\0\0\0\0', 'little')
        if not val & 0x80000000:
            return None
        return kernel_by_ordinal.get(val & 0xffff)

    def kernel_variable_reader(va):
        """Kernel data export read by a `mov eax,[thunk]; mov eax,[eax]; ret` routine."""
        if image is None or va is None:
            return None
        code = image.read(va, 8)
        if len(code) < 8 or code[0] != 0xa1 or code[5:8] != b'\x8b\x00\xc3':
            return None
        slot = int.from_bytes(code[1:5], 'little')
        val = int.from_bytes(image.read(slot, 4) or b'\0\0\0\0', 'little')
        if not val & 0x80000000:
            return None
        return kernel_by_ordinal.get(val & 0xffff)

    def bridge(sym, raw, va, name=None, cat=None):
        """Asm body satisfying `sym` from nxdk/kernel, or None."""
        body = image.read(va, 3) if image is not None and va is not None else b''
        if body[:1] in (b'\xc3', b'\xc2'):
            # The original is an empty body (`ret` / `ret N`): reproduce it.
            n = int.from_bytes(body[1:3], 'little') if body[:1] == b'\xc2' else 0
            bridges[sym] = ('\tret $%d\n' % n if n else '\tret\n', 'empty_body')
            return bridges[sym][1]
        kfn = kernel_forwarder(va)
        if kfn:
            bridges[sym] = ('\tjmp *__imp_%s\n' % kfn, 'kernel_forwarder:' + kfn)
            return bridges[sym][1]
        kvar = kernel_variable_reader(va)
        if kvar:
            bridges[sym] = ('\tmovl __imp_%s, %%eax\n\tmovl (%%eax), %%eax\n\tret\n' % kvar,
                            'kernel_variable:' + kvar)
            return bridges[sym][1]
        if cat == 'crt' and name:
            # LIBCMT routine kb.json names crt_X / _X: same cdecl ABI as pdclib's X.
            base = re.sub(r'^(crt_|_+)', '', name)
            if '_' + base in lib_defs and not raw.startswith('@') and '@' not in raw:
                bridges[sym] = ('\tjmp _%s\n' % base, 'crt_alias:%s' % base)
                return bridges[sym][1]
        m = re.match(r'^_xapi_(\w+?)(?:@(\d+))?$', raw)
        if m and '_' + m.group(1) in lib_defs:
            # kb.json xapi_X: the XAPI routine X under a name that avoids the SDK
            # header's declaration. cdecl kb names share nxdk's ABI outright;
            # __stdcall ones get the same bridge as below.
            if m.group(2) is None:
                bridges[sym] = ('\tjmp _%s\n' % m.group(1),
                                'nxdk_cdecl_bridge:' + lib_defs['_' + m.group(1)])
                return bridges[sym][1]
            n = int(m.group(2))
            body = '\tpushl %ebp\n\tmovl %esp, %ebp\n'
            for k in reversed(range(n // 4)):
                body += '\tpushl %d(%%ebp)\n' % (8 + 4 * k)
            body += '\tcall _%s\n\tmovl %%ebp, %%esp\n\tpopl %%ebp\n\tret $%d\n' % (m.group(1), n)
            bridges[sym] = (body, 'nxdk_cdecl_bridge:' + lib_defs['_' + m.group(1)])
            return bridges[sym][1]
        m = re.match(r'^(_[A-Za-z_]\w*)@(\d+)$', raw)
        if m and m.group(1) in lib_defs:
            # nxdk winapi is cdecl even where the XDK API is __stdcall.
            n = int(m.group(2))
            body = '\tpushl %ebp\n\tmovl %esp, %ebp\n'
            for k in reversed(range(n // 4)):
                body += '\tpushl %d(%%ebp)\n' % (8 + 4 * k)
            body += '\tcall %s\n\tmovl %%ebp, %%esp\n\tpopl %%ebp\n\tret $%d\n' % (m.group(1), n)
            bridges[sym] = (body, 'nxdk_cdecl_bridge:' + lib_defs[m.group(1)])
            return bridges[sym][1]
        return None

    funcs_per_object = collections.Counter(e['object'] for e in kb.funcs)

    def library_bridge(sym, e):
        """Unimplemented XDK library routine with an nxdk/pdclib equivalent:
        XAPI __stdcall entries by kb name, LIBCMT ones by their object name."""
        name = re.sub(r'^xapi_', '', e['name'])
        if '__stdcall' in e['decl']:
            n = decl_stack_bytes(e['decl'])
            if n is not None:
                return bridge(sym, '_%s@%d' % (name, n), None)
            return None
        m = re.match(r'^LIBCMT:(\w+)\.obj$', e['object'])
        if m and funcs_per_object[e['object']] == 1:
            name = m.group(1)
        elif m:
            # kb.json crt_X in a multi-function LIBCMT object (sprintf.obj),
            # or the decorated C name _X (_isalpha in isctype.obj).
            name = re.sub(r'^(crt_|_(?=[a-z]))', '', name)
        # A direct jmp is only ABI-safe for cdecl: LIBCMT code, or a routine
        # taking no arguments (where __stdcall pops nothing either). kb.json's
        # `void X(void)` placeholders say nothing about the arguments
        # (RaiseException, PulseEvent), so they never qualify.
        if not m and (decl_stack_bytes(e['decl']) != 0 or
                      re.match(r'^\s*void\s+\w+\s*\(\s*void\s*\)\s*;?\s*$', e['decl'])):
            return None
        if '_' + name in lib_defs and '__fastcall' not in e['decl']:
            bridges[sym] = ('\tjmp _%s\n' % name, 'crt_alias:%s' % name)
            return bridges[sym][1]
        return None

    aliases = {}   # raw symbol -> legacy VA
    for raw, name in sorted(undefined_raw.items()):
        if name in defined_names:
            continue
        cat, va = (clf.symbol(name, raw.startswith('__imp_')) if clf else ('unknown', None))
        e = kb.by_name.get(name)
        if raw in lib_defs:
            provider = 'nxdk:' + lib_defs[raw]
        elif name in kernel:
            provider = 'kernel'
            cat = 'kernel'
        elif e is not None and e['kind'] == 'data' and LEGACY_BASE <= e['addr'] < LEGACY_END:
            aliases[raw] = e['addr']
            provider = 'legacy_data'
        elif raw.startswith('__imp_'):
            provider = 'unresolved_import'
        else:
            provider = bridge(raw, raw, e['addr'] if e else va, name, cat)
            if provider is None:
                stub_for(raw, name)
                provider = 'stub'
        report['provider'][provider.split(':')[0] if provider.startswith(
            ('kernel_', 'nxdk_cdecl', 'crt_alias')) else provider] += 1
        report['symbols'][raw] = {'name': name, 'category': cat, 'provider': provider,
                                  'addr': '0x%x' % va if va is not None else None}

    # Jump table over the original code range.
    code_end = min(lo for lo, hi, n, _ in image.sections if n == '.rdata') if image else 0x253080
    tramps = []
    skipped_short = 0
    tramp_kind = collections.Counter()
    starts = [e for e in kb.funcs if LEGACY_BASE <= e['addr'] < code_end]
    for i, e in enumerate(starts):
        nxt = starts[i + 1]['addr'] if i + 1 < len(starts) else code_end
        if nxt - e['addr'] < 5:
            skipped_short += 1
            continue
        # A lift that was never renamed from its auto name still defines
        # FUN_<addr> while the catalog names the function, so fall back to it.
        raw = defined_raw.get(e['name']) or defined_raw.get('FUN_%08x' % e['addr'])
        if raw and not REG_ANNOT_RE.search(e['decl']) and raw.startswith(('_', '@')):
            tramps.append((e['addr'], raw))
            tramp_kind['implemented'] += 1
            continue
        sym = '__halo_legacy_%x' % e['addr']
        if raw and raw.startswith(('_', '@')):
            body = reverse_thunk_asm(e['name'], raw)
            if body:
                bridges[sym] = (body, 'reverse_thunk:' + e['name'])
                tramps.append((e['addr'], sym))
                tramp_kind['reverse_thunk'] += 1
                continue
        why = 'reg-abi entry' if raw else 'missing'
        if not raw and bridge(sym, sym, e['addr']):
            tramps.append((e['addr'], sym))
            tramp_kind['kernel_forwarder'] += 1
            continue
        jmp = image.read(e['addr'], 5) if image and not raw else b''
        if jmp[:1] == b'\xc3' or jmp[:1] == b'\xc2':
            # The original is an empty body (`ret` / `ret N`): reproduce it.
            n = int.from_bytes(jmp[1:3], 'little') if jmp[:1] == b'\xc2' else 0
            bridges[sym] = ('\tret $%d\n' % n if n else '\tret\n', 'empty_body')
            tramps.append((e['addr'], sym))
            tramp_kind['empty_body'] += 1
            continue
        if len(jmp) == 5 and jmp[0] == 0xe9:
            # The original is a lone `jmp rel32` thunk: follow it to the
            # target's own trampoline.
            tgt = (e['addr'] + 5 + int.from_bytes(jmp[1:5], 'little', signed=True)) & 0xffffffff
            if tgt in kb.by_addr and LEGACY_BASE <= tgt < code_end:
                bridges[sym] = ('\tjmp __halo_legacy_base + 0x%x\n' % (tgt - LEGACY_BASE),
                                'jmp_thunk:0x%x' % tgt)
                tramps.append((e['addr'], sym))
                tramp_kind['jmp_thunk'] += 1
                continue
        if not raw and library_bridge(sym, e):
            tramps.append((e['addr'], sym))
            tramp_kind['library_bridge'] += 1
            continue
        # Legacy callers of a __stdcall routine expect it to pop its arguments.
        # The original's own `ret N` is the evidence; kb.json decls are often
        # missing or `void f(void)` placeholders for XDK library code.
        pops = original_pops(image, e['addr'], nxt) if image else None
        if pops is None:
            pops = (decl_stack_bytes(e['decl']) or 0) if '__stdcall' in e['decl'] else 0
        else:
            tramp_kind['stub_pops_from_ret'] += 1
        stubs[sym] = ('%s @0x%x (%s)' % (e['name'], e['addr'], why), pops)
        tramps.append((e['addr'], sym))
        tramp_kind['reg_abi_stub' if raw else 'missing_stub'] += 1

    # Entry points kb.json lacks but data tables point at: give each a logging
    # stub so the call reports itself instead of running the next function.
    unknown_functions = []
    if image:
        for va, nxt in unknown_data_referenced_functions(
                image, [e['addr'] for e in starts], code_end):
            sym = '__halo_legacy_%x' % va
            pops = original_pops(image, va, nxt)
            stubs[sym] = ('unknown function @0x%x (not in kb.json)' % va, pops or 0)
            tramps.append((va, sym))
            tramp_kind['unknown_function_stub'] += 1
            unknown_functions.append('0x%x' % va)

    # Seed ranges (dev mode).
    seeds = []
    seeded_bytes = 0
    if args.seed_xbe:
        seed_img = inv.Image(args.seed_xbe)
        from xbe import Xbe
        x = Xbe.from_file(args.seed_xbe)
        for name in SEED_SECTIONS:
            s = x.sections[name]
            raw = bytes(s.data)[:s.header.raw_size].rstrip(b'\0')
            if not raw:
                continue
            path = os.path.join(args.out, 'seed_%s.bin' % name.strip('.').lower())
            with open(path, 'wb') as f:
                f.write(raw)
            seeds.append((s.header.virtual_addr, path, len(raw)))
            seeded_bytes += len(raw)
        del seed_img

    # legacy.s: one monotonically increasing .org stream.
    events = [(va, 'tramp', sym) for va, sym in tramps]
    events += [(va, 'seed', (path, n)) for va, path, n in seeds]
    events.sort(key=lambda ev: ev[0])
    with open(os.path.join(args.out, 'legacy.s'), 'w') as f:
        f.write('# GENERATED by tools/gen_legacy_image.py. DO NOT EDIT.\n'
                '# Must be the FIRST object on the link line: it pins the original\n'
                '# cachebeta.xbe layout at VA 0x%x..0x%x.\n' % (LEGACY_BASE, LEGACY_END))
        f.write('\t.section .text,"xw"\n\t.globl __halo_legacy_base\n__halo_legacy_base:\n')
        for va, kind, what in events:
            f.write('\t.org 0x%x\n' % (va - LEGACY_BASE))
            if kind == 'tramp':
                f.write('\tjmp %s\n' % what)
            else:
                f.write('\t.incbin %s\n' % asm_str(os.path.abspath(what[0])))
        f.write('\t.org 0x%x\n' % (LEGACY_END - LEGACY_BASE))
        for raw, va in sorted(aliases.items()):
            f.write('\t.globl %s\n\t.set %s, __halo_legacy_base + 0x%x\n'
                    % (raw, raw, va - LEGACY_BASE))

    with open(os.path.join(args.out, 'stubs.s'), 'w') as f:
        f.write('# GENERATED by tools/gen_legacy_image.py. DO NOT EDIT.\n'
                '# Placeholder bodies for everything the standalone build cannot yet\n'
                '# resolve. Each logs its name once, then returns 0.\n'
                '\t.text\n')
        for i, (sym, (display, pops)) in enumerate(stubs.items()):
            f.write('\t.globl %s\n%s:\n\tpushl $__halo_missing_name_%d\n'
                    '\tcall _halo_standalone_missing\n\taddl $4, %%esp\n'
                    '\txorl %%eax, %%eax\n\tret%s\n'
                    % (sym, sym, i, ' $%d' % pops if pops else ''))
        for sym, (body, why) in bridges.items():
            f.write('# %s\n\t.globl %s\n%s:\n%s' % (why, sym, sym, body))
        f.write('\t.section .rdata,"dr"\n')
        for i, (sym, (display, pops)) in enumerate(stubs.items()):
            f.write('__halo_missing_name_%d:\n\t.asciz %s\n' % (i, asm_str(display)))

    by_cat = collections.defaultdict(collections.Counter)
    for r in report['symbols'].values():
        by_cat[r['category']][r['provider'].split(':')[0]] += 1
    summary = {
        'objects': len(objs),
        'unresolved_symbols': len(report['symbols']),
        'provider': dict(report['provider']),
        'by_category': {k: dict(v) for k, v in sorted(by_cat.items())},
        'trampolines': dict(tramp_kind),
        'trampolines_skipped_short': skipped_short,
        'seeded_bytes_from_cachebeta': seeded_bytes,
        'stubs': len(stubs),
        'bridges': len(bridges),
        'unknown_functions': unknown_functions,
    }
    with open(os.path.join(args.out, 'link_report.json'), 'w') as f:
        json.dump({'summary': summary, 'symbols': report['symbols']}, f, indent=1,
                  sort_keys=True)
    print('standalone legacy image: %s' % json.dumps(summary, sort_keys=True))


if __name__ == '__main__':
    main()
