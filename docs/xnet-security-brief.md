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

The reference fixture covers only the initiator's request. The responder's
reply was first seen in a live capture of this stack joining a stock cachebeta
host (xemu, bridged): the wire always carries the initiator's SPI at +4 and the
responder's at +8, then the initiator's nonce at +12 and the responder's at
+20, whichever side sends. The first implementation sent the sender's own SPI
first, so the host accepted our requests and we rejected every reply.
`capture_fixture` in `tools/test_xnet_security.c` replays the captured request
and reply for both roles. The capture's LAN/kernel keys are unknown, so it
re-signs the captured bytes and pins layout, not the MAC. The initiator's final
ack and the session after it are not yet seen on the wire against a stock host.

Halo gives a TCP connect to a secure address one 1 s select, and its SYN waits
in the peer queue during the key exchange, so the timing follows the original.
A request goes out with the first queued segment, timed from a fresh clock
reading. It is retried every 2 s, four times, and 2 s after the last retry the
peer goes back to idle (`0x227ac0` -> `0x2268c8`). We used to retry after
0.5 s or less. A stock responder re-keys on every request with a newer serial
(`0x227d26`), so such a retry got a second reply with a new nonce while we kept
the first keys. A reply with our nonce, a valid MAC and a newer serial is
therefore accepted after the exchange too: a new responder nonce or SPI
re-derives and adopts the keys. Exact duplicates fail the serial check, and
wrong nonces or MACs are rejected. The stock initiator does not re-derive here.
It avoids the race with its 2 s retry. As at the `0x227d26` tail, the reply
flushes queued segments instead of the ack; the ack goes out only when the
queue is empty. Before, the ack and the SYN were sent back to back. If the send
callback refuses a frame (a full NIC or XBDM ring), the flush stops. That frame
and the frames after it stay queued in order, and later segments wait behind
them. Each 100 ms tick tries again. The serial starts at `KeQuerySystemTime`
(`0x22724d`; the captured host reply carries one). It was random, so after a
restart a host that still held our old serial dropped about half of our
requests. It now never falls below the previous value within a boot. The ack's
MAC uses the initiator's directional key from derivation indices 1-2 (peer
`+0x58` at `0x2278f6`). The responder checks it against its matching receive
key (`+0x48` at `0x227d26`). This is read from the code, not yet seen on the
wire.

The integration sends outer IPv4/UDP 3074 packets directly to peer Ethernet
addresses, routes virtual secure addresses through a separate lwIP interface,
and recomputes decrypted inner checksums for lwIP. Discovery routing works
without an assigned DHCP address. Crypto runs in the TCP/IP worker, outside
the NIC interrupt. Startup flag 1 permits plain LAN traffic while secure
address traffic and System Link broadcasts use XNet framing.

Random generation uses a device-keyed HMAC stream. The first request of each
boot rekeys the seed in `T:\\xnet-random.bin` and persists it before any output
is returned; later requests advance a counter in memory. The original XNet
works the same way (0x2260fb): it rekeys a seed sector at byte 0x1000 of
`\Device\Harddisk0\partition0` once at startup, then generates RC4 output
in memory (0x2262c4). Startup fails if the kernel HDD key is zero or durable
seed writes fail. This assumes a private device key;
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
