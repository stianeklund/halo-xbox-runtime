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

`compat/xnet/xnet.c` translates the original Xbox Winsock ABI to lwIP:

- IPv4 sockaddr and count-plus-array fd sets, including Halo's `select(1)`.
- TCP/UDP creation, binding, listening, accepting, connecting, and I/O.
- Nonblocking I/O, Winsock error codes through nxdk's thread-local last error,
  and Winsock `MSG_PEEK`.
- Broadcast, reuse, receive buffers, keepalive, linger, timeouts, and TCP_NODELAY.
- Startup/cleanup reference counts, LAN key registration and secure peer mappings.

lwIP's send capacity is fixed at compile time. `SO_SNDBUF` queries report
that capacity; changing it per socket returns an unsupported-option error.
Halo's requested 16 KiB fits the configured capacity.

`compat/xnet/security.c` implements cachebeta 2276 System Link security: encrypted
broadcasts, authenticated DH handshakes, directional session keys, DES/3DES-CBC
packet framing, replay checks and reconnect state. `crt/nxdk_xnet.c` wraps inner
TCP/UDP segments in Ethernet-addressed UDP 3074 packets and delivers verified
segments through a separate lwIP interface for virtual 0/8 addresses. Discovery
also uses this interface before DHCP completes. Loopback uses lwIP's local path.
The build explicitly enables multiple netifs and lwIP's loopback interface;
nxdk's default single-netif route rejects 127/8. The local split-screen client
must connect to its server at 127.0.0.1:5150 without using the physical NIC.
The TCP/IP worker has a 32 KiB stack for the codec and kernel crypto calls;
packet processing is queued out of the NIC interrupt.

The target ABI uses a 12-byte XNADDR, rather than the later Xbox SDK's 36-byte
layout. `tools/gen_xnet_profile.py` carries the supplied layout certificate's
LAN key into the build; runtime kernel keys are not copied into the image.
Random generation persists a rekeyed device-keyed seed on T: once per boot,
before returning the first bytes, and fails on missing device-key or storage errors. Startup flag 1 allows
plain LAN IPv4 traffic; secure mappings and discovery remain encrypted.
While XNet is started, inbound filtering fails closed: besides ARP, secure
startup admits only the stack's DHCP replies in plaintext, and no plaintext
frame may claim a virtual 0/8 source or destination under either flag.

The recovered catalog reverses the names of the original socket-address
queries: 0x22486b returns the local bound address, while 0x224876 returns the
connected peer. The adapter binds its semantic C functions to those existing
catalog symbols accordingly. This matters after a split-screen map loads:
Halo queries the TCP peer to address its UDP client updates to the server.
Returning the local TCP port instead leaves those updates undelivered.

## Debugger coexistence

`crt/nxdk_debug_net.c` detects the resident XBDM `XBD6` shared-NIC interface
through the same processor-local pointer as cachebeta XNet. With that interface
present, nxdk registers title callbacks and submits Ethernet packets through
XBDM, instead of resetting the NIC with NVNET. XBDM keeps its own debug IP and
TCP/IP stack; lwIP obtains the title IP with the title MAC and network settings.
Without XBDM, the normal NVNET driver is used. An unknown sharing ABI fails
title network startup while preserving the debugger's adapter.

Receive callbacks copy XBDM's loaned frames before returning. Transmit copies
remain in kernel pool memory until XBDM reports completion. Driver calls run at
DPC level; title-address callbacks use a published snapshot rather than taking
lwIP's core mutex from a DPC. Title link status is polled on the TCP/IP worker.
`XNetGetDebugXnAddr` obtains the separate debug address in the target's 12-byte
layout. Halo's telnet listener remains a title-side TCP service on port 23;
XBDM is the debugger-side service on port 731. NAT forwards must select the
appropriate guest IP and port; host 127.0.0.1 is separate from title loopback.

The sharing ABI was checked against cachebeta 2276 and resident XBDM
1.00.5849.4. A seeded title obtained 10.0.2.16 through DHCP while XBDM at
10.0.2.15 remained accessible through the existing host port forward. Other
XBDM versions and physical devkits still need runtime validation.

The combined networking/APU build was launched with `magicboot` in xemu.
XBDM remained accessible while the game ran, confirmed by the user. APU
telemetry showed buffers completing with no driver errors or stop timeouts.
Remaining game stubs still limit gameplay testing; this does not establish
stock-peer interoperability or complete gameplay/audio behavior.

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
TCP SYN routing to secure peers, encrypted discovery under startup flag 1,
fail-closed plaintext filtering (forged ports and virtual sources),
a local TCP server on port 5150, and shared-NIC buffer ownership, title/debug
address separation, driver selection, and link updates. It uses the production
lwIP options rather than independently chosen routing defaults.
Its host allocator alignment differs from Xbox's to accommodate 64-bit pointers;
it does not emulate the NVNET driver or TCP/IP worker concurrency.
A seeded Xbox build verifies the nxdk headers, object ABI and linkage;
multiplayer hosting/joining still requires an emulator/device runtime test.

After building, run `python3 tools/check_socket_name_abi.py --build BUILD_DIR
--reference-xbe /path/to/cachebeta.xbe` to execute the original local/peer
queries and the built legacy dispatch in Unicorn. It also executes Halo's
endpoint-address lookup and checks that its UDP destination is the remote
server port, rather than the client's local TCP port. This needs pyxbe,
pefile and Unicorn; the lwIP address queries use controlled socket fixtures.

Build with `NXDK_DIR=/mnt/g/dev/nxdk HALO_ROOT=/mnt/g/dev/halo ./build.sh seed --inventory`.
