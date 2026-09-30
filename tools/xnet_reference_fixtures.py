#!/usr/bin/env python3
"""Execute cachebeta XNET routines to generate independent codec fixtures.

Only kernel crypto, random bytes, allocation and network submission are mocked.
Framing and key-derivation instructions execute from the user's reference XBE.
Requires pyxbe, unicorn and cryptography. No XBE bytes are saved in the fixture.
"""
import argparse
import hashlib
import hmac
import json
from pathlib import Path
import struct
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_EIP
from xbe import Xbe


class Reference:
    def __init__(self, path):
        self.image = Xbe.from_file(str(path))
        self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        self.u.mem_map(0x10000, 0x640000)
        for section in self.image.sections.values():
            self.u.mem_write(section.header.virtual_addr, bytes(section.data))
        # Header is needed for certificate access, and is not a section.
        self.u.mem_write(0x10000, path.read_bytes()[:0x2000])
        self.u.mem_map(0x8000000, 0x100000)
        self.u.mem_map(0xA000000, 0x10000)
        self.u.hook_add(UC_HOOK_CODE, self.hook)
        self.iv = bytes(range(8))
        self.sent = None

    def write32(self, addr, n):
        self.u.mem_write(addr, struct.pack('<I', n))

    def read(self, addr, n):
        return bytes(self.u.mem_read(addr, n))

    def args(self):
        return struct.unpack('<7I', self.read(self.u.reg_read(UC_X86_REG_ESP) + 4, 28))

    def ret(self, n, value=0):
        sp = self.u.reg_read(UC_X86_REG_ESP)
        pc, = struct.unpack('<I', self.read(sp, 4))
        self.u.reg_write(UC_X86_REG_ESP, sp + 4 + n)
        self.u.reg_write(UC_X86_REG_EAX, value)
        self.u.reg_write(UC_X86_REG_EIP, pc)

    def hook(self, _u, pc, _size, _user):
        a = self.args()
        if pc == 0x1D8F2A:  # XcHMAC
            key, kn, x, xn, y, yn, out = a
            self.u.mem_write(out, hmac.digest(self.read(key, kn),
                (self.read(x, xn) if xn else b'') + (self.read(y, yn) if yn else b''), 'sha1'))
            self.ret(28)
        elif pc == 0x1D8F24:  # XcDESKeyParity
            addr, n = a[:2]
            self.u.mem_write(addr, bytes(v ^ (not (v.bit_count() & 1)) for v in self.read(addr, n)))
            self.ret(8)
        elif pc == 0x1D8F30:  # XcModExp, little-endian limbs
            out, b, e, m, n = a[:5]
            v = pow(int.from_bytes(self.read(b, n*4), 'little'),
                    int.from_bytes(self.read(e, n*4), 'little'),
                    int.from_bytes(self.read(m, n*4), 'little'))
            self.u.mem_write(out, v.to_bytes(n*4, 'little'))
            self.ret(20, 1)
        elif pc == 0x1D8F1E:  # XcKeyTable: save key for the subsequent kernel CBC call
            cipher, table, key = a[:3]
            self.u.mem_write(table, struct.pack('<I', cipher) + self.read(key, 8 if cipher == 0 else 24))
            self.ret(12)
        elif pc == 0x1D8F18:  # XcBlockCryptCBC
            cipher, n, out, data, table, encrypt, iv = a
            key = self.read(table + 4, 8 if cipher == 0 else 24)
            if cipher == 0:
                key *= 3  # TripleDES with repeated keys is single DES.
            ctx = Cipher(algorithms.TripleDES(key), modes.CBC(self.read(iv, 8)))
            ctx = ctx.encryptor() if encrypt else ctx.decryptor()
            self.u.mem_write(out, ctx.update(self.read(data, n)) + ctx.finalize())
            self.ret(28)
        elif pc == 0x2262C4:  # XNet's random stream
            data, n = a[:2]
            assert n == 8
            self.u.mem_write(data, self.iv)
            self.ret(8)
        elif pc == 0x22662B:  # packet allocation for the handshake emitter
            assert a[2] == 0xA4
            obj, ip = 0x8007000, 0x8008000
            self.u.mem_write(ip, b'\0' * 256)
            self.write32(obj, ip)
            self.u.mem_write(obj+4, struct.pack('<HH', 20+8+164, a[1]))
            self.ret(20, obj)
        elif pc == 0x2531B4:  # KeRaiseIrqlToDpcLevel import
            self.ret(0)
        elif pc == 0x2271BF:  # local address query (NIC is absent in the fixture)
            addr = a[0]
            self.u.mem_write(addr, b'\x0c\0' + self.read(self.u.reg_read(UC_X86_REG_ECX)+0x1C0, 6) + b'\0'*4)
            self.ret(4, 2)
        elif pc == 0x2273BB:  # Ethernet submission: capture, do not run the driver
            obj = a[0]
            ip, = struct.unpack('<I', self.read(obj, 4))
            n, = struct.unpack('<H', self.read(obj + 4, 2))
            self.sent = self.read(ip + 28, n - 28)
            self.ret(8)
        elif pc == 0xA000000:
            self.u.emu_stop()

    def call(self, pc, argv=(), ecx=0):
        sp = 0x80FF000
        self.u.mem_write(sp, struct.pack('<'+'I'*(len(argv)+1), 0xA000000, *argv))
        self.u.reg_write(UC_X86_REG_ESP, sp)
        self.u.reg_write(UC_X86_REG_ECX, ecx)
        self.u.emu_start(pc, 0xA000001, count=100000)

    def title_keys(self, lan):
        # Enter the certificate/HMAC portion of title initialization; halt just
        # after parity, before timer/driver side effects.
        ctx, stack = 0x8000000, 0x80F0000
        self.write32(0x25330C, 0x8002000)
        self.u.mem_write(0x8002000, lan)
        self.u.reg_write(UC_X86_REG_EBX, ctx)
        self.u.reg_write(UC_X86_REG_EBP, stack)
        self.u.reg_write(UC_X86_REG_ESP, stack - 0x100)
        self.u.emu_start(0x227311, 0x227385, count=10000)
        return self.read(ctx + 0x924, 16), self.read(ctx + 0x934, 24)

    def derive(self, auth, exponent, public, initiator, guest, host):
        peer, key, x, y, b = 0x8004000, 0x8005000, 0x8006000, 0x8006010, 0x8006100
        self.write32(peer + 0x78, key)
        self.u.mem_write(key + 8, auth)
        self.u.mem_write(key + 24, exponent)
        self.u.mem_write(x, guest)
        self.u.mem_write(y, host)
        self.u.mem_write(b, public)
        self.call(0x2268FA, [peer, x, y, b, int(initiator)])
        return {'rx_auth': self.read(peer+0x48,16).hex(), 'tx_auth': self.read(peer+0x58,16).hex(),
                'rx_des': self.read(peer+0x68,8).hex(), 'tx_des': self.read(peer+0x70,8).hex()}

    def encode(self, auth, des, segment, broadcast):
        ctx, obj, ip, peer = 0x8000000, 0x8007000, 0x8008000, 0x800A000
        body = segment + bytes(range(1, ((-len(segment)-2) % 8) + 1))
        body += bytes([(-len(segment)-2) % 8, 17])
        n = 20 + 8 + 8 + 8 + len(body) + 12
        self.u.mem_write(ip, b'\0' * n)
        self.u.mem_write(ip+9, b'\x11')
        self.u.mem_write(ip+44, body)
        self.write32(obj, ip)
        self.u.mem_write(obj+4, struct.pack('<HH', n, 0x380))
        if broadcast:
            self.u.mem_write(ctx+0x924, auth)
            self.u.mem_write(ctx+0x934, des)
        else:
            self.u.mem_write(peer+4, bytes.fromhex('00123405'))
            self.write32(peer+0x10, 6)
            self.u.mem_write(peer+0x58, auth)
            self.u.mem_write(peer+0x70, des)
            self.u.mem_write(peer+0x40, self.iv)
        self.call(0x227746, [obj, 0 if broadcast else peer, 0], ctx)
        assert self.sent is not None
        return self.sent.hex()

    def handshake(self):
        ctx, peer, key = 0x8000000, 0x800A000, 0x800B000
        self.u.mem_write(ctx, b'\0' * 0x1000)
        self.u.mem_write(peer, b'\0' * 0x100)
        self.u.mem_write(key, b'\0' * 0x100)
        self.u.mem_write(ctx+0x1C0, bytes.fromhex('020000000001'))
        self.u.mem_write(peer, bytes.fromhex('00123405'))
        self.u.mem_write(peer+0x14, b'\x01')
        self.u.mem_write(peer+0x38, b'12345678')
        self.write32(peer+0x78, key)
        self.u.mem_write(peer+0x7C, b'\x0c\0' + bytes.fromhex('020000000002') + b'\0'*4)
        self.u.mem_write(key, b'\x01' + b'\0'*7)
        self.u.mem_write(key+8, bytes(range(16)))
        self.u.mem_write(key+0x78, bytes(range(96)))
        # The reference counter is post-incremented.
        self.u.mem_write(ctx+0x958, struct.pack('<Q', 1))
        self.call(0x2278F6, [peer], ctx)
        return self.sent.hex()


def fixtures(path):
    ref = Reference(path)
    lan = bytes(range(16))
    auth, des = ref.title_keys(lan)
    title = ref.read(ref.image.header.certificate_addr + 0xB0, 16)
    assert title == b'TESTTESTTESTTEST', 'fixture addresses require cachebeta 2276'
    exponent = bytes(range(96))
    public = bytes(range(96))
    guest, host = b'12345678', b'abcdefgh'
    segment = bytes.fromhex('14211420000d0000') + b'hello'
    return {'profile': 'cachebeta 01.10.12.2276', 'reference_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'lan': lan.hex(), 'title': title.hex(), 'title_auth': auth.hex(), 'title_des': des.hex(),
            'exponent': exponent.hex(), 'public': public.hex(), 'guest': guest.hex(), 'host': host.hex(),
            'session': ref.derive(bytes(range(16)), exponent, public, True, guest, host),
            'segment': segment.hex(), 'broadcast': ref.encode(auth, des, segment, True),
            'unicast': ref.encode(bytes(range(16)), bytes(range(8)), segment, False),
            'handshake': ref.handshake()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xbe', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.write_text(json.dumps(fixtures(args.xbe), indent=2)+'\n')
