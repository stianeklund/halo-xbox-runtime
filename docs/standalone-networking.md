# Standalone networking

The standalone build compiles nxdk's lwIP and NVNET driver locally with
`crt/net/lwipopts.h`. It does not modify the SDK checkout or use its default
four-socket network archive. The local build supports 64 sockets, with UDP
and TCP PCB headroom for DHCP, DNS, and closed TCP connections. Packet buffers
and worker/receive queues are also sized for concurrent traffic. All network
objects and consumers use the same options; changing only an adapter header
would break their ABI.

`crt/nxdk_net.c` starts the NIC and TCP/IP worker once. It reads the Xbox
network configuration sector, uses a configured static IPv4 address or
starts DHCP, and reports the assigned IPv4/MAC address to Halo. Address
polling reports a ready Ethernet address without waiting for DHCP, matching
cachebeta System Link. Cleanup closes Halo's sockets and clears session keys;
a later startup reuses the worker
because nxdk does not implement worker teardown.

`compat/xnet.c` translates the original Xbox Winsock ABI to lwIP:

- IPv4 sockaddr and count-plus-array fd sets, including Halo's `select(1)`.
- TCP/UDP creation, binding, listening, accepting, connecting, and I/O.
- Nonblocking I/O, Winsock error codes through nxdk's thread-local last error,
  and Winsock `MSG_PEEK`.
- Broadcast, reuse, receive buffers, keepalive, linger, timeouts, and TCP_NODELAY.
- Startup/cleanup reference counts, LAN key registration and secure peer mappings.

lwIP's send capacity is fixed at compile time. `SO_SNDBUF` queries report
that capacity; changing it per socket returns an unsupported-option error.
Halo's requested 16 KiB fits the configured capacity.

`compat/xnet_security.c` implements cachebeta 2276 System Link security: encrypted
broadcasts, authenticated DH handshakes, directional session keys, DES/3DES-CBC
packet framing, replay checks and reconnect state. `crt/nxdk_xnet.c` wraps inner
TCP/UDP segments in Ethernet-addressed UDP 3074 packets and delivers verified
segments through a separate lwIP interface for virtual 0/8 addresses. Discovery
also uses this interface before DHCP completes. Loopback uses lwIP's local path.
The TCP/IP worker has a 32 KiB stack for the codec and kernel crypto calls;
packet processing is queued out of the NIC interrupt.

The target ABI uses a 12-byte XNADDR, rather than the later Xbox SDK's 36-byte
layout. `tools/gen_xnet_profile.py` carries the supplied layout certificate's
LAN key into the build; runtime kernel keys are not copied into the image.
Random generation persists a rekeyed device-keyed seed on T: before returning
bytes and fails on missing device-key or storage errors. Startup flag 1 allows
plain LAN IPv4 traffic; secure mappings and discovery remain encrypted.

Stock-peer interoperability still needs a live test. See
[xnet-security-brief.md](xnet-security-brief.md) for target evidence and outstanding
runtime checks. Xbox Live is outside this implementation. Raising socket capacity
does not raise Halo's console/player limits.

## Validation

Run `python3 tools/test_network.py` to test the actual adapter against host
BSD sockets with address/undefined-behavior sanitizers. Tests cover TCP/UDP
exchange, address ABI, error translation, nonblocking connect/read, fd-set
conversion, linger, session registration, 64 simultaneous sockets, reference
counting, concurrent creation/closure, cleanup, and restart. These tests do not exercise the NVNET driver
or Xbox DHCP. Run `python3 tools/test_xnet_security.py` for the protocol fixture,
malformed-packet and session lifecycle tests. To regenerate fixtures from the
original instructions, pass `--reference-xbe /path/to/cachebeta.xbe`; this needs
pyxbe, Unicorn and cryptography in addition to OpenSSL development headers.
`python3 tools/test_xnet_stack.py` builds nxdk's real lwIP sources with host
kernel/NIC/scheduling shims and tests the production packet adapter: encrypted
discovery without DHCP, inbound checksum/integrity checks, loopback delivery,
TCP SYN routing to secure peers, and encrypted discovery under startup flag 1.
Its host allocator alignment differs from Xbox's to accommodate 64-bit pointers;
it does not emulate the NVNET driver or TCP/IP worker concurrency.
A seeded Xbox build verifies the nxdk headers, object ABI and linkage;
multiplayer hosting/joining still requires an emulator/device runtime test.

Build with `NXDK_DIR=/mnt/g/dev/nxdk HALO_ROOT=/mnt/g/dev/halo ./build.sh seed --inventory`.
