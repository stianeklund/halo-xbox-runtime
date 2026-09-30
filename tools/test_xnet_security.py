#!/usr/bin/env python3
"""Run sanitized protocol tests with independent original-XNET fixtures.

Use --reference-xbe to regenerate fixtures by executing the original routines
and compare them before testing. Without it, tests use the recorded fixtures.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reference-xbe', type=Path)
args = parser.parse_args()
fixtures = json.loads((root / 'tools/fixtures/xnet-cachebeta.json').read_text())
if args.reference_xbe:
    from xnet_reference_fixtures import fixtures as generate
    assert generate(args.reference_xbe) == fixtures, 'original-XNET fixtures changed'
    print('PASS: fixtures regenerated from reference XBE instructions', flush=True)

with tempfile.TemporaryDirectory(prefix='halo-xnet-security-') as temp:
    temp = Path(temp)
    header = []
    for name, value in fixtures.items():
        if name in ('profile', 'reference_sha256'):
            continue
        for field, data in (value.items() if isinstance(value, dict) else [('', value)]):
            symbol = name + ('_' + field if field else '')
            header.append('static const unsigned char fixture_' + symbol + '[] = {' +
                          ','.join(f'0x{x:02x}' for x in bytes.fromhex(data)) + '};')
    (temp / 'xnet_fixtures.h').write_text('\n'.join(header)+'\n')
    binary = temp / 'test-xnet'
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DHALO_NETWORK_HOST_TEST', '-fsanitize=address,undefined', '-g', '-I'+str(temp),
                    str(root / 'compat/xnet_security.c'), str(root / 'tools/test_xnet_security.c'),
                    '-lcrypto', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=60)
