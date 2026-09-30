#!/usr/bin/env python3
"""Check legacy address-query dispatch against the original XNet instructions.

Run after building: python3 tools/check_socket_name_abi.py --build build-network-debug
    --reference-xbe /path/to/cachebeta.xbe
Requires pyxbe, pefile and Unicorn. lwIP/socket lookup calls are intercepted;
the original wrappers and the built 32-bit adapter execute unchanged.
"""
import argparse
from pathlib import Path
import re
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP
from xbe import Xbe

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--reference-xbe", type=Path, required=True)
args = parser.parse_args()
symbols = {}
for line in (args.build / "halo.map").read_text().splitlines():
    match = re.match(r"\s*[0-9a-f]+:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\b", line)
    if match:
        symbols[match[1]] = int(match[2], 16)

DATA = 0x20000000
OUT, LENGTH, SOCKET, XNET = DATA, DATA + 0x100, DATA + 0x1000, DATA + 0x2000
STACK, STOP = DATA + 0xe000, DATA + 0xf000
LOCAL_PORT, PEER_PORT = 60000, 5150
reference = Xbe.from_file(str(args.reference_xbe))
candidate = pefile.PE(str(args.build / "halo.exe"))


def invoke(entry, original, transport=False):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(0x10000, 0x1000000)
    uc.mem_map(DATA, 0x10000)
    if original:
        for section in reference.sections.values():
            uc.mem_write(section.header.virtual_addr, bytes(section.data))
        uc.mem_write(0x4ee4b0, struct.pack("<I", XNET))
        uc.mem_write(XNET + 0x964, struct.pack("<I", 1))
        uc.mem_write(SOCKET + 0xc, b"\x30")  # bound and connected
        uc.mem_write(SOCKET + 0x44, b"\x7f\0\0\1")
        uc.mem_write(SOCKET + 0x48, struct.pack("!HH", PEER_PORT, LOCAL_PORT))
    else:
        base = candidate.OPTIONAL_HEADER.ImageBase
        for section in candidate.sections:
            uc.mem_write(base + section.VirtualAddress, section.get_data())
        uc.mem_write(symbols["_wsa_refs"], struct.pack("<I", 1))
    uc.mem_write(OUT, b"\xa5" * 40)
    uc.mem_write(LENGTH, struct.pack("<I", 16))
    if transport:
        endpoint = DATA + 0x4000
        uc.mem_write(endpoint, struct.pack("<I", 7) + b"\0\x12" + bytes(18))
        uc.mem_write(0x335090, b"\x01")
        uc.mem_write(STACK, struct.pack("<3I", STOP, endpoint, OUT))
    else:
        uc.mem_write(STACK, struct.pack("<4I", STOP, 7, OUT, LENGTH))
    uc.reg_write(UC_X86_REG_ESP, STACK)
    calls = []

    def return_from_call(value, pops=0):
        esp = uc.reg_read(UC_X86_REG_ESP)
        target = struct.unpack("<I", uc.mem_read(esp, 4))[0]
        uc.reg_write(UC_X86_REG_EAX, value)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + pops)
        uc.reg_write(UC_X86_REG_EIP, target)

    def intercept(_uc, address, size, user):
        if address == STOP:
            uc.emu_stop()
        elif original and address == 0x22361e:
            return_from_call(SOCKET, 4)
        elif not original and address in (
                symbols["_lwip_getpeername"], symbols["_lwip_getsockname"]):
            peer = address == symbols["_lwip_getpeername"]
            calls.append("peer" if peer else "local")
            esp = uc.reg_read(UC_X86_REG_ESP)
            _, out, length = struct.unpack("<3I", uc.mem_read(esp + 4, 12))
            sockaddr = bytes([16, 2]) + struct.pack("!H", PEER_PORT if peer else LOCAL_PORT)
            sockaddr += b"\x7f\0\0\1" if peer else bytes(4)
            uc.mem_write(out, sockaddr + bytes(8))
            uc.mem_write(length, struct.pack("<I", 16))
            return_from_call(0)

    uc.hook_add(UC_HOOK_CODE, intercept)
    uc.emu_start(entry, STOP, count=10000)
    assert uc.reg_read(UC_X86_REG_EIP) == STOP, "wrapper did not return"
    assert uc.reg_read(UC_X86_REG_ESP) == STACK + (4 if transport else 16), "stack imbalance"
    assert uc.reg_read(UC_X86_REG_EAX) & (0xffff if transport else 0xffffffff) == 0
    assert struct.unpack("<I", uc.mem_read(LENGTH, 4))[0] == 16
    count = 24 if transport else 16
    assert uc.mem_read(OUT + count, 16) == b"\xa5" * 16, "address buffer overrun"
    return bytes(uc.mem_read(OUT, count)), calls


for entry, kind, port, ip in (
        (0x22486b, "local", LOCAL_PORT, bytes(4)),
        (0x224876, "peer", PEER_PORT, b"\x7f\0\0\1")):
    expected = struct.pack("<H", 2) + struct.pack("!H", port) + ip + bytes(8)
    oracle, _ = invoke(entry, True)
    actual, calls = invoke(entry, False)
    assert oracle == expected, (hex(entry), oracle.hex(), expected.hex())
    assert actual == oracle and calls == [kind], (hex(entry), actual.hex(), calls)
oracle, _ = invoke(0x83a60, True, transport=True)
actual, calls = invoke(0x83a60, False, transport=True)
assert actual == oracle and calls == ["peer"], (actual.hex(), oracle.hex(), calls)
assert struct.unpack_from("<I", actual, 0)[0] == 0x7f000001
assert struct.unpack_from("<HH", actual, 16) == (4, PEER_PORT)
print("PASS: original 2276 local/peer queries, built legacy dispatch, sockaddr bytes, calling conventions and Halo's server UDP destination lookup")
