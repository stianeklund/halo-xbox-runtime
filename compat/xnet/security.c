/* cachebeta XNET protocol. Evidence addresses and wire fixtures are documented
 * in docs/xnet-security-brief.md. No retail constants or game-message parsing.
 */
#include "security.h"
#include <limits.h>
#include <string.h>
/* Key exchange timer (0x227ac0): a message, then one retry every 2 s, four
 * times; 2 s after the last retry the peer goes back to idle. */
#define XSEC_RETRY_MS 2000
#define XSEC_RETRIES 4
const uint8_t xsec_modulus[96] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc9, 0x0f, 0xda, 0xa2,
    0x21, 0x68, 0xc2, 0x34, 0xc4, 0xc6, 0x62, 0x8b, 0x80, 0xdc, 0x1c, 0xd1,
    0x29, 0x02, 0x4e, 0x08, 0x8a, 0x67, 0xcc, 0x74, 0x02, 0x0b, 0xbe, 0xa6,
    0x3b, 0x13, 0x9b, 0x22, 0x51, 0x4a, 0x08, 0x79, 0x8e, 0x34, 0x04, 0xdd,
    0xef, 0x95, 0x19, 0xb3, 0xcd, 0x3a, 0x43, 0x1b, 0x30, 0x2b, 0x0a, 0x6d,
    0xf2, 0x5f, 0x14, 0x37, 0x4f, 0xe1, 0x35, 0x6d, 0x6d, 0x51, 0xc2, 0x45,
    0xe4, 0x85, 0xb5, 0x76, 0x62, 0x5e, 0x7e, 0xc6, 0xf4, 0x4c, 0x42, 0xe9,
    0xa6, 0x3a, 0x36, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}
static uint64_t le64(const uint8_t *p) {
  uint64_t v = 0;
  unsigned i;
  for (i = 0; i < 8; i++)
    v |= (uint64_t)p[i] << (i * 8);
  return v;
}
static void put64(uint8_t *p, uint64_t v) {
  unsigned i;
  for (i = 0; i < 8; i++)
    p[i] = (uint8_t)(v >> (i * 8));
}
static int equal(const uint8_t *a, const uint8_t *b, unsigned n) {
  unsigned diff = 0, i;
  for (i = 0; i < n; i++)
    diff |= a[i] ^ b[i];
  return !diff;
}
static void wipe(void *p, size_t n) {
  volatile uint8_t *q = p;
  while (n--)
    *q++ = 0;
}
static void parity(uint8_t *p, unsigned n) {
  unsigned i, b, v;
  for (i = 0; i < n; i++) {
    v = 0;
    for (b = 0; b < 8; b++)
      v ^= p[i] >> b;
    if (!(v & 1))
      p[i] ^= 1;
  }
}
static int key_index(struct xsec *s, const uint8_t *id) {
  unsigned i;
  for (i = 0; i < XSEC_KEYS; i++)
    if (s->keys[i].used && equal(s->keys[i].id, id, 8))
      return (int)i;
  return -1;
}
static int segment_valid(uint8_t protocol, const uint8_t *p, unsigned n) {
  if (protocol == 17)
    return n >= 8 && (((unsigned)p[4] << 8) | p[5]) == n && (p[0] || p[1]) &&
           (p[2] || p[3]);
  if (protocol == 6)
    return n >= 20 && (unsigned)(p[12] >> 4) * 4 >= 20 &&
           (unsigned)(p[12] >> 4) * 4 <= n && (p[0] || p[1]) && (p[2] || p[3]);
  return 0;
}
int xsec_init(struct xsec *s, const struct xsec_ops *ops, void *user,
              const uint8_t *mac, const uint8_t *lan, const uint8_t *title) {
  uint8_t input[17], digest[20], seed[2];
  uint64_t previous = s->serial;
  memset(s, 0, sizeof(*s));
  s->ops = *ops;
  s->user = user;
  memcpy(s->mac, mac, 6);
  /* The serial starts at the system time (0x22724d) and only ever increases: a
   * stock peer drops a handshake whose serial is not above the last one it
   * kept for us (signed compare at 0x227d26), across our restarts too. */
  s->serial = ops->system_time(user);
  if (s->serial < previous)
    s->serial = previous;
  if (ops->random(user, seed, sizeof(seed)))
    return 10091;
  s->generation = (uint16_t)(seed[0] | seed[1] << 8);
  memcpy(input + 1, title, 16);
  input[0] = 0;
  ops->hmac(user, lan, input, 17, NULL, 0, digest);
  memcpy(s->title_auth, digest, 16);
  memcpy(s->title_des, digest + 16, 4);
  input[0] = 1;
  ops->hmac(user, lan, input, 17, NULL, 0, digest);
  memcpy(s->title_des + 4, digest, 20);
  parity(s->title_des, 24);
  wipe(digest, sizeof(digest));
  return 0;
}
void xsec_clear(struct xsec *s) {
  wipe(s->keys, sizeof(s->keys));
  wipe(s->peers, sizeof(s->peers));
}
int xsec_register(struct xsec *s, const uint8_t *id, const uint8_t *key) {
  unsigned i;
  uint8_t digest[20], base[96] = {0};
  struct xsec_key *k;
  if (key_index(s, id) >= 0)
    return 10037;
  for (i = 0; i < XSEC_KEYS; i++)
    if (!s->keys[i].used)
      break;
  if (i == XSEC_KEYS)
    return 10055;
  k = &s->keys[i];
  memcpy(k->id, id, 8);
  s->ops.hmac(s->user, s->title_auth, id, 8, key, 16, digest);
  memcpy(k->auth, digest, 16);
  base[95] = 2; /* Exact byte string at 0x22dd48, interpreted little-endian by
                   XcModExp. */
  if (s->ops.random(s->user, k->exponent, 96) ||
      s->ops.modexp(s->user, base, k->exponent, k->public_value)) {
    wipe(k, sizeof(*k));
    return 10091;
  }
  k->used = 1;
  wipe(digest, sizeof(digest));
  return 0;
}
int xsec_unregister(struct xsec *s, const uint8_t *id) {
  int k = key_index(s, id);
  unsigned i;
  if (k < 0)
    return 10022;
  for (i = 0; i < XSEC_PEERS; i++)
    if (s->peers[i].used && s->peers[i].key == k)
      wipe(&s->peers[i], sizeof(s->peers[i]));
  wipe(&s->keys[k], sizeof(s->keys[k]));
  return 0;
}
int xsec_map(struct xsec *s, const struct halo_xnaddr *addr, const uint8_t *id,
             uint32_t *ip) {
  int k = key_index(s, id);
  unsigned i;
  uint8_t raw[4];
  struct xsec_peer *p;
  *ip = 0;
  if (addr->size != 12)
    return 10022;
  if (equal(addr->ethernet, s->mac, 6)) {
    raw[0] = 127;
    raw[1] = 0;
    raw[2] = 0;
    raw[3] = 1;
    memcpy(ip, raw, 4);
    return 0;
  }
  if (k < 0)
    return 10022;
  if ((addr->ethernet[0] & 1) ||
      equal(addr->ethernet, (const uint8_t[6]){0}, 6))
    return 10049;
  for (i = 0; i < XSEC_PEERS; i++)
    if (s->peers[i].used && s->peers[i].key == k &&
        equal(s->peers[i].addr.ethernet, addr->ethernet, 6)) {
      *ip = s->peers[i].local_spi;
      return 0;
    }
  for (i = 0; i < XSEC_PEERS; i++)
    if (!s->peers[i].used)
      break;
  if (i == XSEC_PEERS)
    return 10055;
  if (++s->generation == 0)
    ++s->generation;
  raw[0] = 0;
  raw[1] = (uint8_t)(s->generation >> 8);
  raw[2] = (uint8_t)s->generation;
  raw[3] = (uint8_t)i;
  p = &s->peers[i];
  memset(p, 0, sizeof(*p));
  p->used = 1;
  p->key = k;
  p->addr = *addr;
  memcpy(&p->local_spi, raw, 4);
  *ip = p->local_spi;
  return 0;
}
int xsec_encode(struct xsec *s, const uint8_t *auth, const uint8_t *key,
                unsigned keylen, uint32_t spi, uint32_t sequence,
                const uint8_t *iv, uint8_t protocol, const uint8_t *segment,
                unsigned n, uint8_t *out, unsigned *size) {
  unsigned padding = (8 - ((n + 2) & 7)) & 7, body = n + padding + 2, i,
           total = 16 + body + 12;
  uint8_t digest[20];
  if (n > XSEC_MAX_SEGMENT || !segment_valid(protocol, segment, n) ||
      *size < total)
    return 10040;
  memcpy(out, &spi, 4);
  out[0] |= 1;
  put32(out + 4, sequence);
  memcpy(out + 8, iv, 8);
  memcpy(out + 16, segment, n);
  for (i = 0; i < padding; i++)
    out[16 + n + i] = (uint8_t)(i + 1);
  out[16 + n + padding] = (uint8_t)padding;
  out[17 + n + padding] = protocol;
  if (s->ops.cbc(s->user, 1, key, keylen, iv, out + 16, body))
    return 10091;
  s->ops.hmac(s->user, auth, out, total - 12, NULL, 0, digest);
  memcpy(out + total - 12, digest, 12);
  *size = total;
  return 0;
}
int xsec_decode(struct xsec *s, const uint8_t *auth, const uint8_t *key,
                unsigned keylen, const uint8_t *in, unsigned n, uint8_t *out,
                unsigned *size, uint8_t *protocol) {
  unsigned body, padding, i, offset;
  uint8_t digest[20];
  int encrypted;
  if (n < 32 || (n & 3) || n > XSEC_MAX_PACKET)
    return 10022;
  encrypted = in[0] & 1;
  offset = encrypted ? 16 : 8;
  if (n < offset + 12 + 2 || (encrypted && ((n - 28) & 7)))
    return 10022;
  body = n - offset - 12;
  if (*size < body)
    return 10040;
  s->ops.hmac(s->user, auth, in, n - 12, NULL, 0, digest);
  if (!equal(digest, in + n - 12, 12))
    return 10013;
  memcpy(out, in + offset, body);
  if (encrypted && s->ops.cbc(s->user, 0, key, keylen, in + 8, out, body))
    return 10091;
  padding = out[body - 2];
  *protocol = out[body - 1];
  if (padding > body - 2)
    return 10022;
  for (i = 0; i < padding; i++)
    if (out[body - 2 - padding + i] != i + 1)
      return 10022;
  body -= padding + 2;
  if (!segment_valid(*protocol, out, body))
    return 10022;
  *size = body;
  return 0;
}
static struct xsec_peer *peer_spi(struct xsec *s, uint32_t spi) {
  unsigned i;
  for (i = 0; i < XSEC_PEERS; i++)
    if (s->peers[i].used && s->peers[i].local_spi == spi)
      return &s->peers[i];
  return NULL;
}
static int handshake_send(struct xsec *s, struct xsec_peer *p, int ack) {
  uint8_t out[164] = {0}, digest[20];
  struct xsec_key *k = &s->keys[p->key];
  if (!ack) {
    /* Fixed roles on the wire, whoever sends: initiator SPI, then responder SPI
     * (zero in the first message), then the nonces in the same order. Verified
     * against a cachebeta host's reply in a live capture. */
    memcpy(out + 4, p->initiator ? &p->local_spi : &p->remote_spi, 4);
    memcpy(out + 8, p->initiator ? &p->remote_spi : &p->local_spi, 4);
    if (p->initiator) {
      memcpy(out + 12, p->nonce_local, 8);
      memcpy(out + 20, p->nonce_remote, 8);
    } else {
      memcpy(out + 12, p->nonce_remote, 8);
      memcpy(out + 20, p->nonce_local, 8);
    }
  } else
    memcpy(out + 8, &p->remote_spi, 4);
  memcpy(out + 28, k->public_value, 96);
  memcpy(out + 124, k->id, 8);
  out[132] = 12;
  memcpy(out + 134, s->mac, 6);
  put64(out + 144, ++s->serial);
  s->ops.hmac(s->user, ack ? p->tx_auth : k->auth, out, 152, NULL, 0, digest);
  memcpy(out + 152, digest, 12);
  return s->ops.send(s->user, p->addr.ethernet, out, sizeof(out), 0);
}
static int public_valid(const uint8_t *public_value) {
  unsigned i;
  int comparison = 0;
  for (i = 96; i > 0; i--)
    if (public_value[i - 1] != xsec_modulus[i - 1]) {
      comparison = public_value[i - 1] < xsec_modulus[i - 1] ? -1 : 1;
      break;
    }
  if (comparison >= 0 || equal(public_value, (const uint8_t[96]){0}, 96) ||
      equal(public_value, (const uint8_t[96]){1}, 96))
    return 0;
  return 1;
}
static int derive(struct xsec *s, struct xsec_peer *p,
                  const uint8_t *public_value) {
  uint8_t secret[96], input[17], digest[20];
  unsigned i;
  struct xsec_key *k = &s->keys[p->key];
  /* Reject degenerate DH values exploited by the retail research, and values
   * >= modulus. Both arguments are little-endian limb arrays in this build. */
  if (!public_valid(public_value))
    return 10013;
  if (s->ops.modexp(s->user, public_value, k->exponent, secret))
    return 10091;
  if (p->initiator) {
    memcpy(input + 1, p->nonce_local, 8);
    memcpy(input + 9, p->nonce_remote, 8);
  } else {
    memcpy(input + 1, p->nonce_remote, 8);
    memcpy(input + 9, p->nonce_local, 8);
  }
  for (i = 1; i <= 4; i++) {
    uint8_t *auth = (i <= 2) == p->initiator ? p->tx_auth : p->rx_auth;
    uint8_t *des = (i <= 2) == p->initiator ? p->tx_des : p->rx_des;
    input[0] = (uint8_t)i;
    s->ops.hmac(s->user, k->auth, input, 17, secret, 96, digest);
    if (i & 1)
      memcpy(auth, digest, 16);
    else {
      memcpy(des, digest, 8);
      parity(des, 8);
    }
  }
  wipe(secret, sizeof(secret));
  wipe(digest, sizeof(digest));
  p->rx_base = 0;
  p->rx_bits = 0;
  p->tx_sequence = 0;
  return s->ops.random(s->user, p->iv, 8) ? 10091 : 0;
}
static int send_ready(struct xsec *s, struct xsec_peer *p, uint8_t protocol,
                      const uint8_t *data, unsigned n) {
  uint8_t out[XSEC_MAX_PACKET];
  unsigned size = sizeof(out);
  int r;
  if (p->tx_sequence == UINT32_MAX)
    return 10060;
  r = xsec_encode(s, p->tx_auth, p->tx_des, 8, p->remote_spi, ++p->tx_sequence,
                  p->iv, protocol, data, n, out, &size);
  if (r)
    return r;
  memcpy(p->iv, out + size - 20, 8);
  return s->ops.send(s->user, p->addr.ethernet, out, size, 0);
}
/* Sends the queue in order. The first failure (send_ready's non-zero return:
 * ops.send refusing the frame, e.g. 10055 from a full NIC ring, or an encoding
 * error) stops it; that segment and the rest stay queued, in order, for the
 * next xsec_tick. Returns that failure, or zero once the queue is empty. */
static int flush(struct xsec *s, struct xsec_peer *p) {
  unsigned sent;
  int r = 0;
  for (sent = 0; sent < p->queued; sent++) {
    r = send_ready(s, p, p->queue[sent].protocol, p->queue[sent].data,
                   p->queue[sent].len);
    if (r)
      break;
  }
  p->queued -= sent;
  memmove(p->queue, p->queue + sent, p->queued * sizeof(p->queue[0]));
  wipe(p->queue + p->queued, (XSEC_QUEUE - p->queued) * sizeof(p->queue[0]));
  return r;
}
/* Retries ran out (0x227ac0 -> 0x2268c8): back to idle, not a failed state; the
 * next segment for this peer starts a new exchange. The serial is kept. */
static void reset(struct xsec_peer *p) {
  p->state = 0;
  p->retries = 0;
  wipe(p->queue, sizeof(p->queue));
  p->queued = 0;
}
int xsec_send_segment(struct xsec *s, uint32_t ip, uint8_t protocol,
                      const uint8_t *data, unsigned n) {
  struct xsec_peer *p;
  uint8_t out[XSEC_MAX_PACKET], iv[8];
  unsigned size = sizeof(out);
  int r;
  if (n > XSEC_MAX_SEGMENT || !segment_valid(protocol, data, n))
    return 10040;
  if (ip == UINT32_MAX) {
    if (protocol != 17)
      return 10045;
    if (s->ops.random(s->user, iv, 8))
      return 10091;
    r = xsec_encode(s, s->title_auth, s->title_des, 24, UINT32_MAX, UINT32_MAX,
                    iv, protocol, data, n, out, &size);
    return r ? r
             : s->ops.send(s->user,
                           (const uint8_t[6]){255, 255, 255, 255, 255, 255},
                           out, size, 1);
  }
  p = peer_spi(s, ip);
  if (!p)
    return 10049;
  /* Behind queued segments a new one waits its turn, to keep the order. */
  if (p->state == 3 && (!p->queued || !flush(s, p)))
    return send_ready(s, p, protocol, data, n);
  if (p->queued == XSEC_QUEUE)
    return 10055;
  if (!p->state) {
    if (s->ops.random(s->user, p->nonce_local, 8))
      return 10091;
    p->initiator = 1;
    p->state = 1;
    /* A fresh reading: the tick's clock can be up to a tick old. */
    p->retry_at = s->ops.milliseconds(s->user) + XSEC_RETRY_MS;
    p->retries = 0;
    r = handshake_send(s, p, 0);
    if (r) {
      p->state = 0;
      return r;
    }
  }
  p->queue[p->queued].len = n;
  p->queue[p->queued].protocol = protocol;
  memcpy(p->queue[p->queued++].data, data, n);
  return 0;
}
static int receive_handshake(struct xsec *s, const uint8_t *mac,
                             const uint8_t *in, unsigned n) {
  uint32_t initiator, responder, src, dst, ip;
  uint64_t serial;
  struct xsec_peer *p;
  struct halo_xnaddr addr = {0};
  uint8_t digest[20];
  int k, r;
  if (n != 164 || !equal(in, (const uint8_t[4]){0}, 4) || in[132] != 12 ||
      in[133] || !equal(in + 134, mac, 6))
    return 10022;
  /* The wire carries the initiator's SPI at +4 and the responder's at +8. From
   * our side: a request has no responder yet, a reply carries both, and the
   * initiator's final ack has no initiator SPI. src is the peer's SPI and dst
   * is ours (zero for a request, which names no session of ours yet). */
  memcpy(&initiator, in + 4, 4);
  memcpy(&responder, in + 8, 4);
  src = !initiator ? 0 : responder ? responder : initiator;
  dst = !initiator ? responder : responder ? initiator : 0;
  serial = le64(in + 144);
  k = key_index(s, in + 124);
  if (k < 0)
    return 10022;
  if (!src && !dst)
    return 10022;
  p = dst ? peer_spi(s, dst) : NULL;
  if (dst && (!p || p->key != k || !equal(p->addr.ethernet, mac, 6)))
    return 10013;
  s->ops.hmac(s->user, !src ? p->rx_auth : s->keys[k].auth, in, 152, NULL, 0,
              digest);
  if (!equal(digest, in + 152, 12))
    return 10013;
  if (!src) {
    if (p->state != 2 || serial <= p->remote_serial)
      return 10013;
    p->remote_serial = serial;
    p->state = 3;
    flush(s, p);
    return 0;
  }
  if (((const uint8_t *)&src)[0])
    return 10022;
  if (!public_valid(in + 28))
    return 10013;
  if (!dst) {
    addr.size = 12;
    memcpy(addr.ethernet, mac, 6);
    memcpy(&addr.address, in + 140, 4);
    r = xsec_map(s, &addr, in + 124, &ip);
    if (r)
      return r;
    p = peer_spi(s, ip);
    if (serial <= p->remote_serial)
      return 10013;
    if (p->state == 1 && memcmp(s->mac, mac, 6) > 0)
      return 0; /* Reference collision election at 0x227ec7. */
    if (p->state == 2 && equal(p->nonce_remote, in + 12, 8)) {
      p->remote_serial = serial;
      return handshake_send(s, p, 0);
    }
    if (s->ops.random(s->user, p->nonce_local, 8))
      return 10091;
    p->initiator = 0;
    p->remote_spi = src;
    memcpy(p->nonce_remote, in + 12, 8);
    r = derive(s, p, in + 28);
    if (r) {
      p->state = 0;
      return r;
    }
    p->remote_serial = serial;
    p->state = 2;
    p->retry_at = s->ops.milliseconds(s->user) + XSEC_RETRY_MS;
    p->retries = 0;
    return handshake_send(s, p, 0);
  }
  /* A reply to us: it carries our nonce and, like every handshake, a newer
   * serial, so an exact duplicate is dropped here. A stock responder re-keys on
   * every newer request (0x227d26), so one of our retries can draw a second
   * reply with a new responder nonce (and SPI) after the first completed the
   * exchange. The host now holds those keys: adopt them, as for a first reply.
   * The stock initiator (also 0x227d26) would keep its first keys; it retries
   * only every 2 s, which makes the race rare there. */
  if ((p->state != 1 && (p->state != 3 || !p->initiator)) ||
      !equal(p->nonce_local, in + 12, 8) || serial <= p->remote_serial)
    return 10013;
  if (p->state == 1 || src != p->remote_spi ||
      !equal(p->nonce_remote, in + 20, 8)) {
    p->remote_spi = src;
    memcpy(p->nonce_remote, in + 20, 8);
    r = derive(s, p, in + 28);
    if (r) {
      if (p->state == 3)
        reset(p); /* keys half replaced: start over on the next segment */
      return r;
    }
  }
  p->remote_serial = serial;
  p->state = 3;
  /* 0x227d26 tail: queued segments go out instead of the ack, which is sent
   * only when nothing was waiting. Either one completes the responder. */
  if (p->queued) {
    flush(s, p);
    return 0;
  }
  return handshake_send(s, p, 1);
}
int xsec_receive(struct xsec *s, const uint8_t *mac, const uint8_t *in,
                 unsigned n, int broadcast) {
  uint32_t spi, sequence, delta;
  struct xsec_peer *p = NULL;
  uint8_t segment[XSEC_MAX_PACKET], protocol;
  unsigned size = sizeof(segment);
  int r;
  if (n < 4 || (mac[0] & 1) || equal(mac, s->mac, 6))
    return 10022;
  memcpy(&spi, in, 4);
  if (!spi)
    return broadcast ? 10022 : receive_handshake(s, mac, in, n);
  if (spi == UINT32_MAX) {
    if (!broadcast || n < 8 || be32(in + 4) != UINT32_MAX)
      return 10022;
    r = xsec_decode(s, s->title_auth, s->title_des, 24, in, n, segment, &size,
                    &protocol);
    if (r)
      return r;
    if (protocol != 17)
      return 10022;
  } else {
    ((uint8_t *)&spi)[0] &= 0xfe;
    p = peer_spi(s, spi);
    if (broadcast || !p || p->state < 2 || p->state > 3 ||
        !equal(mac, p->addr.ethernet, 6) || n < 8)
      return 10013;
    sequence = be32(in + 4);
    if (sequence < p->rx_base)
      return 10013;
    delta = sequence - p->rx_base;
    if (delta > 1024 || (delta < 32 && (p->rx_bits & (1u << delta))))
      return 10013;
    r = xsec_decode(s, p->rx_auth, p->rx_des, 8, in, n, segment, &size,
                    &protocol);
    if (r)
      return r;
    /* Advance only after authentication and complete structural validation. */
    if (delta < 32)
      p->rx_bits |= 1u << delta;
    else {
      p->rx_bits = delta >= 63 ? 0x80000000u
                               : (p->rx_bits >> (delta - 31)) | 0x80000000u;
      p->rx_base += delta - 31;
    }
    if (p->state == 2) {
      p->state = 3;
      flush(s, p);
    }
  }
  /* Broadcasts deliberately carry no replay counter in the reference. */
  s->ops.deliver(s->user, p ? p->local_spi : UINT32_MAX, protocol, segment,
                 size);
  return 0;
}
void xsec_tick(struct xsec *s, uint32_t now) {
  unsigned i;
  for (i = 0; i < XSEC_PEERS; i++) {
    struct xsec_peer *p = &s->peers[i];
    if (!p->used)
      continue;
    if (p->state == 3 && p->queued)
      flush(s, p); /* what a busy NIC refused earlier */
    if ((p->state != 1 && p->state != 2) ||
        (int32_t)(now - p->retry_at) < 0)
      continue;
    if (p->retries == XSEC_RETRIES) {
      reset(p);
      continue;
    }
    ++p->retries;
    p->retry_at = now + XSEC_RETRY_MS;
    handshake_send(s, p, 0);
  }
}
