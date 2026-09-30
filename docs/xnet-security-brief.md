# XNet System Link security brief

Status: cachebeta 2276 protocol and lwIP integration implemented. Independent
reference fixtures and host tests pass; live stock-peer interoperability is
still pending.

## Why this is needed

The original standalone adapter sent plain TCP/UDP traffic and treated session
keys as opaque identifiers. Original Halo System Link encrypts discovery traffic and uses
a handshake to establish encryption and authentication keys for gameplay.
Plain sockets therefore cannot interoperate with an unmodified compatible
Halo build. [Protocol research](https://hllmn.net/blog/2023-09-18_h1x-net/)

The Xbox kernel supplies crypto primitives through nxdk. The protocol core in
`compat/xnet/security.c` implements framing, key derivation, session state and
MAC-based addressing. `crt/nxdk_xnet.c` connects it to lwIP and kernel crypto.
`compat/xnet/xnet.c` preserves Halo's original socket ABI.

## Target evidence and implemented behavior

The target is `cachebeta.xbe`, build 01.10.12.2276. Retail protocol constants
cannot be substituted: this build uses a **12-byte XNADDR** and a **164-byte
handshake**, with the certificate LAN key `TESTTESTTESTTEST`. The build derives
its title profile from the supplied layout XBE certificate; the Xbox kernel
LAN key is read at runtime.

- Title HMAC-SHA1/3DES derivation: original routine `0x227311`.
- Registered-key derivation and 768-bit DH parameters: `0x226fef` and
  data at `0x22dce8` / `0x22dd48`. Kernel modular arithmetic uses little-endian
  32-bit limbs.
- Directional session keys: `0x2268fa`.
- ESP-style DES/3DES-CBC framing, SHA1 authentication tags and padding:
  `0x227746`.
- Handshake layout and authentication: `0x2278f6`.

`tools/xnet_reference_fixtures.py` executes these target instructions in Unicorn
with kernel primitives, allocation and NIC submission replaced by host
callbacks. The checked-in fixture records the input XBE hash and generated
packets, without including XBE code. Tests compare exact reference bytes,
including the handshake; they also cover tampering, malformed lengths/padding,
replay rejection, UDP/TCP segments, retries, timeout, teardown and reconnect.

The integration sends outer IPv4/UDP 3074 packets directly to peer Ethernet
addresses, routes virtual secure addresses through a separate lwIP interface,
and recomputes decrypted inner checksums for lwIP. Discovery routing works
without an assigned DHCP address. Crypto runs in the TCP/IP worker, outside
the NIC interrupt. Startup flag 1 permits plain LAN traffic while secure
address traffic and System Link broadcasts use XNet framing.

Random generation uses a device-keyed HMAC stream and a rekeyed seed persisted
to `T:\\xnet-random.bin` before output is returned. Startup fails if the kernel
HDD key is zero or durable seed writes fail. This assumes a private device key;
an emulator's public/default EEPROM key does not supply that secrecy.

## Original implementation checklist

1. **Confirm the target protocol.** Inspect our reference XNet implementation
   and capture discovery, joining, and gameplay packets. Verify protocol
   revision and certificate LAN-key derivation; our cachebeta debug keys must
   not be assumed compatible with retail Halo.
2. **Implement the packet codec.** Match broadcast and session key derivation,
   encryption/decryption, integrity tags, IVs, padding, and sequence handling.
   Verify exact algorithms and constants against the target build.
3. **Implement session establishment.** Add the handshake, per-peer keys,
   secure-address mappings, timeouts, and teardown. Replace the current
   identifier-only key handling and timing-based random generation with the
   randomness required by the reference protocol.
4. **Connect the codec to the network stack.** Stock traffic wraps inner UDP
   or TCP segments in an outer UDP packet. Integrate at the packet boundary,
   preserve Halo's socket semantics, and handle System Link MAC addressing.
5. **Handle failed sessions correctly.** Reject invalid authentication,
   malformed packets, and prohibited replays. Preserve retry, cleanup, and
   reconnect behavior without silently sending plaintext to stock peers.

## Remaining runtime completion criteria

- A standalone and a compatible unmodified Halo build discover each other,
  join a lobby, exchange gameplay traffic, leave, and reconnect successfully.
- Verify single-player startup and local multiplayer in the integrated image.
  The reported split-screen crash was traced to the missing nickname API
  returning a null find handle; the replacement and its host tests are committed
  separately. The live retry is pending.

Host socket-capacity, concurrency and cleanup tests pass, and the seeded Xbox
image builds. These results do not prove NVNET operation or stock-peer gameplay.
The production packet adapter also passes host tests using the actual lwIP
sources, including discovery before DHCP and virtual-address TCP routing.
Loopback/split screen can exercise local multiplayer setup but cannot validate
encrypted discovery or a two-console handshake.

Xbox Live is outside this scope. Socket capacity and protocol compatibility
do not change Halo's console or player limits. Current transport details are
in [standalone-networking.md](standalone-networking.md).
