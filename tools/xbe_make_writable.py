#!/usr/bin/env python3
"""Set the XBE 'writable' flag on named sections, in place.

cxbe maps the PE .text section to a non-writable XBE section, but the
standalone .text begins with the legacy layout (original .data/BSS at their
cachebeta.xbe addresses), which the game writes.  See gen_legacy_image.py.

It also clears cxbe's XINIT_LIMIT_DEVKIT_MEMORY init flag. cachebeta.xbe is a
debug build whose init flags are just XINIT_MOUNT_UTILITY_DRIVE (0x1), so on a
devkit (or 128MB xemu) it gets the full 128MB. Its debug-only allocations, e.g.
ai_debug_initialize's 10MB actor/path debug arrays, do not fit in 64MB.

Finally it copies cachebeta.xbe's certificate keys (LAN key, signature key,
alternate signature keys) over cxbe's all-zero ones. The kernel derives
XboxSignatureKey from the certificate's signature key when it loads the
image, and XCalculateSignature* (saved-game and profile checksums) key their
HMAC with it, so only matching keys give checksums the original accepts. The
title id and name are left alone.
"""
import struct
import sys

XBE_SECTION_WRITABLE = 0x1
XBE_INIT_FLAGS_OFFSET = 0x124
XINIT_LIMIT_DEVKIT_MEMORY = 0x4
XBE_CERTIFICATE_ADDRESS_OFFSET = 0x118
CERT_LAN_KEY = 0xb0
CERT_SIGNATURE_KEY = 0xc0
CERT_ALTERNATE_SIGNATURE_KEYS = 0xd0
CERT_KEYS_END = CERT_ALTERNATE_SIGNATURE_KEYS + 16 * 16
# cachebeta.xbe's certificate (title 0x4d530004 "Halo"): LAN and signature
# keys are both "TESTTESTTESTTEST"; the sixteen alternate keys are zero.
HALO_LAN_KEY = b'TESTTESTTESTTEST'
HALO_SIGNATURE_KEY = b'TESTTESTTESTTEST'
HALO_ALTERNATE_SIGNATURE_KEYS = bytes(16 * 16)


def main():
    path, names = sys.argv[1], set(sys.argv[2:]) or {'.text'}
    with open(path, 'r+b') as f:
        data = bytearray(f.read())
        base, = struct.unpack_from('<I', data, 0x104)
        count, hdr_va = struct.unpack_from('<II', data, 0x11C)
        off = hdr_va - base
        for i in range(count):
            h = off + i * 0x38
            flags, = struct.unpack_from('<I', data, h)
            name_va, = struct.unpack_from('<I', data, h + 0x14)
            end = data.index(b'\0', name_va - base)
            name = data[name_va - base:end].decode()
            if name in names:
                struct.pack_into('<I', data, h, flags | XBE_SECTION_WRITABLE)
                print('%s: flags 0x%x -> 0x%x' % (name, flags, flags | XBE_SECTION_WRITABLE))
        init_flags, = struct.unpack_from('<I', data, XBE_INIT_FLAGS_OFFSET)
        if init_flags & XINIT_LIMIT_DEVKIT_MEMORY:
            struct.pack_into('<I', data, XBE_INIT_FLAGS_OFFSET, init_flags & ~XINIT_LIMIT_DEVKIT_MEMORY)
            print('init flags 0x%x -> 0x%x' % (init_flags, init_flags & ~XINIT_LIMIT_DEVKIT_MEMORY))
        cert_va, = struct.unpack_from('<I', data, XBE_CERTIFICATE_ADDRESS_OFFSET)
        cert = cert_va - base
        cert_size, = struct.unpack_from('<I', data, cert)
        if cert_size < CERT_KEYS_END:
            sys.exit('certificate is only 0x%x bytes' % cert_size)
        keys = HALO_LAN_KEY + HALO_SIGNATURE_KEY + HALO_ALTERNATE_SIGNATURE_KEYS
        if data[cert + CERT_LAN_KEY:cert + CERT_KEYS_END] != keys:
            data[cert + CERT_LAN_KEY:cert + CERT_KEYS_END] = keys
            print('certificate keys -> cachebeta.xbe')
        f.seek(0)
        f.write(data)


if __name__ == '__main__':
    main()
