/* Real protocol core + independent original-XNET fixtures + queued peers. */
#include "../compat/xnet/security.h"
#include "xnet_fixtures.h"
#include <assert.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/provider.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct endpoint {
  struct xsec sec;
  unsigned random_counter, delivered;
  uint8_t last[1500], protocol;
  unsigned last_size;
};
struct frame {
  uint8_t src[6], dest[6], data[XSEC_MAX_PACKET];
  unsigned size;
  int broadcast;
};
static struct frame frames[256];
static unsigned head, tail;
static struct endpoint a, b;
/* The clocks behind ops.milliseconds/system_time, and how many more frames
 * the wire takes before it refuses them like a full NIC ring (-1: all). */
static uint32_t test_clock;
static uint64_t test_time = 0x01dd532380522210ull; /* capture_reply's serial */
static int wire_budget = -1;
static int random_bytes(void *user, void *out, unsigned n) {
  struct endpoint *e = user;
  unsigned i;
  for (i = 0; i < n; i++)
    ((uint8_t *)out)[i] = (uint8_t)++e->random_counter;
  return 0;
}
static void mac(void *user, const uint8_t *key, const void *x, unsigned xn,
                const void *y, unsigned yn, uint8_t *out) {
  uint8_t buf[2048];
  unsigned n = 20;
  (void)user;
  assert(xn + yn <= sizeof(buf));
  if (xn)
    memcpy(buf, x, xn);
  if (yn)
    memcpy(buf + xn, y, yn);
  assert(HMAC(EVP_sha1(), key, 16, buf, xn + yn, out, &n));
  assert(n == 20);
}
static int cbc(void *user, int enc, const uint8_t *key, unsigned keylen,
               const uint8_t *iv, uint8_t *data, unsigned n) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  EVP_CIPHER *cipher =
      EVP_CIPHER_fetch(NULL, keylen == 8 ? "DES-CBC" : "DES-EDE3-CBC", NULL);
  int size = 0, end = 0;
  (void)user;
  assert(ctx && cipher);
  assert(EVP_CipherInit_ex2(ctx, cipher, key, iv, enc, NULL));
  assert(EVP_CIPHER_CTX_set_padding(ctx, 0));
  assert(EVP_CipherUpdate(ctx, data, &size, data, n));
  assert(EVP_CipherFinal_ex(ctx, data + size, &end));
  assert((unsigned)(size + end) == n);
  EVP_CIPHER_CTX_free(ctx);
  EVP_CIPHER_free(cipher);
  return 0;
}
static int modexp(void *user, const uint8_t *base, const uint8_t *exp,
                  uint8_t *out) {
  BIGNUM *b = BN_lebin2bn(base, 96, NULL), *e = BN_lebin2bn(exp, 96, NULL),
         *m = BN_lebin2bn(xsec_modulus, 96, NULL), *r = BN_new();
  BN_CTX *ctx = BN_CTX_new();
  (void)user;
  assert(b && e && m && r && ctx);
  assert(BN_mod_exp(r, b, e, m, ctx));
  assert(BN_bn2lebinpad(r, out, 96) == 96);
  BN_free(b);
  BN_clear_free(e);
  BN_free(m);
  BN_free(r);
  BN_CTX_free(ctx);
  return 0;
}
static int wire(void *user, const uint8_t *dest, const uint8_t *data,
                unsigned n, int broadcast) {
  struct endpoint *e = user;
  struct frame *f;
  if (!wire_budget)
    return 10055;
  if (wire_budget > 0)
    wire_budget--;
  assert(tail - head < 256);
  assert(n <= XSEC_MAX_PACKET);
  f = &frames[tail++ % 256];
  memcpy(f->src, e->sec.mac, 6);
  memcpy(f->dest, dest, 6);
  memcpy(f->data, data, n);
  f->size = n;
  f->broadcast = broadcast;
  return 0;
}
static void deliver(void *user, uint32_t src, uint8_t protocol,
                    const uint8_t *data, unsigned n) {
  struct endpoint *e = user;
  (void)src;
  assert(n <= sizeof(e->last));
  e->delivered++;
  e->protocol = protocol;
  e->last_size = n;
  memcpy(e->last, data, n);
}
static uint32_t milliseconds(void *user) {
  (void)user;
  return test_clock;
}
static uint64_t system_time(void *user) {
  (void)user;
  return test_time;
}
static struct xsec_ops ops = {random_bytes, mac,          cbc,
                              modexp,       wire,         deliver,
                              milliseconds, system_time};
static void init_mac(struct endpoint *e, const uint8_t *ethernet) {
  memset(e, 0, sizeof(*e));
  assert(xsec_init(&e->sec, &ops, e, ethernet, fixture_lan, fixture_title) ==
         0);
}
static void init(struct endpoint *e, int identity) {
  uint8_t ethernet[6] = {2, 0, 0, 0, 0, 0};
  ethernet[5] = (uint8_t)identity;
  init_mac(e, ethernet);
}
static void pump(void) {
  unsigned limit = 100;
  while (head < tail) {
    struct frame f = frames[head++ % 256];
    struct endpoint *e = !memcmp(f.src, a.sec.mac, 6) ? &b : &a;
    assert(limit--);
    assert(f.broadcast || !memcmp(f.dest, e->sec.mac, 6));
    assert(xsec_receive(&e->sec, f.src, f.data, f.size, f.broadcast) == 0);
  }
}
static struct halo_xnaddr address(struct endpoint *e) {
  struct halo_xnaddr addr = {0};
  addr.size = 12;
  memcpy(addr.ethernet, e->sec.mac, 6);
  return addr;
}
static uint32_t map(struct endpoint *local, struct endpoint *remote,
                    const uint8_t *id) {
  struct halo_xnaddr addr = address(remote);
  uint32_t ip;
  assert(xsec_map(&local->sec, &addr, id, &ip) == 0);
  return ip;
}
static void fixtures(void) {
  uint8_t out[XSEC_MAX_PACKET], plain[XSEC_MAX_PACKET],
      iv[8] = {0, 1, 2, 3, 4, 5, 6, 7}, protocol;
  unsigned n = sizeof(out), size = sizeof(plain);
  uint32_t spi;
  unsigned i;
  init(&a, 1);
  assert(!memcmp(a.sec.title_auth, fixture_title_auth, 16));
  assert(!memcmp(a.sec.title_des, fixture_title_des, 24));
  assert(xsec_encode(&a.sec, a.sec.title_auth, a.sec.title_des, 24, UINT32_MAX,
                     UINT32_MAX, iv, 17, fixture_segment,
                     sizeof(fixture_segment), out, &n) == 0);
  assert(n == sizeof(fixture_broadcast) && !memcmp(out, fixture_broadcast, n));
  assert(xsec_decode(&a.sec, a.sec.title_auth, a.sec.title_des, 24,
                     fixture_broadcast, sizeof(fixture_broadcast), plain, &size,
                     &protocol) == 0);
  assert(protocol == 17 && size == sizeof(fixture_segment) &&
         !memcmp(plain, fixture_segment, size));
  memcpy(&spi, (const uint8_t[]){0, 0x12, 0x34, 5}, 4);
  n = sizeof(out);
  assert(xsec_encode(&a.sec, fixture_lan, fixture_lan, 8, spi, 7, iv, 17,
                     fixture_segment, sizeof(fixture_segment), out, &n) == 0);
  assert(n == sizeof(fixture_unicast) && !memcmp(out, fixture_unicast, n));
  /* Each byte is authenticated, including SPI, counter, IV and ciphertext. */
  for (i = 0; i < sizeof(fixture_broadcast); i++) {
    memcpy(out, fixture_broadcast, sizeof(fixture_broadcast));
    out[i] ^= 1;
    size = sizeof(plain);
    assert(xsec_decode(&a.sec, a.sec.title_auth, a.sec.title_des, 24, out,
                       sizeof(fixture_broadcast), plain, &size,
                       &protocol) != 0);
  }
  for (i = 0; i < sizeof(fixture_broadcast); i++) {
    size = sizeof(plain);
    assert(xsec_decode(&a.sec, a.sec.title_auth, a.sec.title_des, 24,
                       fixture_broadcast, i, plain, &size, &protocol) != 0);
  }
  size = 1;
  assert(xsec_decode(&a.sec, a.sec.title_auth, a.sec.title_des, 24,
                     fixture_broadcast, sizeof(fixture_broadcast), plain, &size,
                     &protocol) == 10040);
  puts("PASS: original-XNET title derivation and exact broadcast/DES packet "
       "bytes");
}
static void handshake_fixture(void) {
  uint8_t id[8] = {1}, key[16] = {0};
  uint32_t ip;
  init(&a, 1);
  init(&b, 2);
  head = tail = 0;
  assert(xsec_register(&a.sec, id, key) == 0);
  ip = map(&a, &b, id);
  memcpy(&ip, (const uint8_t[]){0, 0x12, 0x34, 5}, 4);
  a.sec.peers[0].local_spi = ip;
  a.sec.serial = 0;
  a.random_counter = 48;
  memcpy(a.sec.keys[0].auth, fixture_lan, 16);
  memcpy(a.sec.keys[0].public_value, fixture_public, 96);
  assert(xsec_send_segment(&a.sec, ip, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  assert(tail - head == 1 && frames[head].size == sizeof(fixture_handshake));
  assert(
      !memcmp(frames[head].data, fixture_handshake, sizeof(fixture_handshake)));
  head = tail = 0;
  puts("PASS: original-XNET exact 164-byte cachebeta handshake");
}
static void directional_fixture(void) {
  uint8_t id[8] = {1}, key[16] = {0}, hs[164] = {0}, digest[20];
  uint32_t ip;
  struct xsec_peer *p;
  init(&a, 1);
  init(&b, 2);
  assert(xsec_register(&a.sec, id, key) == 0);
  ip = map(&a, &b, id);
  p = &a.sec.peers[0];
  memcpy(a.sec.keys[0].auth, fixture_lan, 16);
  memcpy(a.sec.keys[0].exponent, fixture_exponent, 96);
  p->state = 1;
  p->initiator = 1;
  memcpy(p->nonce_local, fixture_guest, 8);
  /* A reply names the initiator (a, us) first and the responder (b) second. */
  memcpy(hs + 4, &ip, 4);
  memcpy(hs + 8, (const uint8_t[]){0, 0x22, 0x33, 0}, 4);
  memcpy(hs + 12, fixture_guest, 8);
  memcpy(hs + 20, fixture_host, 8);
  memcpy(hs + 28, fixture_public, 96);
  memcpy(hs + 124, id, 8);
  hs[132] = 12;
  memcpy(hs + 134, b.sec.mac, 6);
  hs[144] = 1;
  mac(&a, fixture_lan, hs, 152, NULL, 0, digest);
  memcpy(hs + 152, digest, 12);
  assert(xsec_receive(&a.sec, b.sec.mac, hs, 164, 0) == 0);
  assert(!memcmp(p->rx_auth, fixture_session_rx_auth, 16) &&
         !memcmp(p->tx_auth, fixture_session_tx_auth, 16));
  assert(!memcmp(p->rx_des, fixture_session_rx_des, 8) &&
         !memcmp(p->tx_des, fixture_session_tx_des, 8));
  head = tail = 0;
  puts("PASS: original-XNET Diffie-Hellman and directional session keys");
}
/* Bytes 0-151 (everything the HMAC covers) of two packets from a live capture of
 * this stack joining a stock cachebeta host: the client's request (packet 4880,
 * 00:50:f2:12:2d:3a) and the host's reply (packet 4881, 00:50:f2:3a:88:a8). The
 * host answered every request, so it accepted ours; our old receive path then
 * rejected its reply because we had the two SPIs swapped. The LAN and kernel
 * keys behind the capture are unknown here, so the tests re-sign these bytes
 * with the test keys: they pin the field layout and roles, not the MAC. */
static const uint8_t capture_request[152] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0xe2, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x16, 0xa4, 0x96, 0x8c, 0xeb, 0x79, 0x1d, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1b, 0x7c, 0x76, 0x63, 0xaa, 0x92, 0x8a, 0x85,
    0xe6, 0x29, 0x36, 0x31, 0xdd, 0x5e, 0x0a, 0xd1, 0x6e, 0xa9, 0xc5, 0x4c,
    0xcf, 0xb7, 0xf6, 0x95, 0x0f, 0x2e, 0xb6, 0xdd, 0x60, 0xe4, 0x01, 0x35,
    0x9b, 0xde, 0xca, 0x41, 0xbf, 0x3e, 0x31, 0x0f, 0xe3, 0x1b, 0x45, 0xd2,
    0x1c, 0x9e, 0xcb, 0x19, 0x05, 0x7d, 0xda, 0x56, 0x28, 0x43, 0x58, 0xe4,
    0x96, 0x69, 0x7c, 0xda, 0x0c, 0xae, 0x21, 0xe2, 0xc8, 0x10, 0x00, 0xd4,
    0xf0, 0x7e, 0xa4, 0x3f, 0x0f, 0x52, 0xab, 0xb5, 0x18, 0xbb, 0xbf, 0x06,
    0xe1, 0x3a, 0x7c, 0x9a, 0x18, 0x05, 0x38, 0xb6, 0xf4, 0xd7, 0xc7, 0xfd,
    0x8c, 0xe5, 0x7c, 0x03, 0x33, 0x35, 0xcc, 0x54, 0xfe, 0x41, 0x7c, 0x14,
    0x0c, 0x00, 0x00, 0x50, 0xf2, 0x12, 0x2d, 0x3a, 0x00, 0x00, 0x00, 0x00,
    0x80, 0xf8, 0x13, 0xef, 0x5d, 0xa7, 0x5b, 0x5a,
};
static const uint8_t capture_reply[152] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0xe2, 0x50, 0x00, 0x00, 0x69, 0x7c, 0x00,
    0x16, 0xa4, 0x96, 0x8c, 0xeb, 0x79, 0x1d, 0x0e, 0x32, 0x09, 0x2e, 0x7c,
    0xe0, 0xae, 0xd5, 0x1c, 0xe1, 0x29, 0x16, 0x2d, 0x3e, 0x36, 0x75, 0xa7,
    0x47, 0x0c, 0x80, 0xcc, 0x7a, 0xc0, 0xd8, 0x65, 0x31, 0x2e, 0x8f, 0x3c,
    0xe1, 0xcb, 0xc9, 0x63, 0xc8, 0xf0, 0xb4, 0x2d, 0x47, 0x65, 0x29, 0xb2,
    0xcf, 0xc4, 0xd1, 0xc5, 0xdf, 0x3a, 0xad, 0xf6, 0x56, 0x07, 0x9c, 0x8b,
    0xc3, 0x65, 0x11, 0x6d, 0xb4, 0x16, 0x68, 0x87, 0xa8, 0xae, 0x9f, 0xe9,
    0xc4, 0xf2, 0xd2, 0x0b, 0x53, 0x48, 0x6a, 0x47, 0x0c, 0x2e, 0x55, 0x7b,
    0xee, 0xe6, 0x13, 0x78, 0x41, 0xd2, 0x36, 0x72, 0xa6, 0x80, 0x3b, 0x3a,
    0xe4, 0xe5, 0x59, 0x03, 0x6b, 0x20, 0x5f, 0x6d, 0x2f, 0xda, 0xb4, 0x16,
    0x4d, 0x61, 0xe4, 0x8d, 0x33, 0x35, 0xcc, 0x54, 0xfe, 0x41, 0x7c, 0x14,
    0x0c, 0x00, 0x00, 0x50, 0xf2, 0x3a, 0x88, 0xa8, 0x0a, 0x00, 0x00, 0x17,
    0x10, 0x22, 0x52, 0x80, 0x23, 0x53, 0xdd, 0x01,
};
static void resign(struct endpoint *e, uint8_t *packet) {
  uint8_t digest[20];
  mac(e, e->sec.keys[0].auth, packet, 152, NULL, 0, digest);
  memcpy(packet + 152, digest, 12);
}
static void capture_fixture(void) {
  static const uint8_t client_mac[6] = {0x00, 0x50, 0xf2, 0x12, 0x2d, 0x3a},
                       host_mac[6] = {0x00, 0x50, 0xf2, 0x3a, 0x88, 0xa8},
                       zero[8] = {0};
  uint8_t id[8], key[16] = {0}, packet[164];
  struct halo_xnaddr host = {0};
  struct xsec_peer *p;
  struct frame f;
  uint32_t ip;
  memcpy(id, capture_request + 124, 8);

  /* Initiator: our request has the captured layout, and the host's reply,
   * which carries our SPI first and its own second, completes the handshake. */
  init_mac(&a, client_mac);
  head = tail = 0;
  assert(xsec_register(&a.sec, id, key) == 0);
  host.size = 12;
  memcpy(host.ethernet, host_mac, 6);
  memcpy(&host.address, capture_reply + 140, 4);
  assert(xsec_map(&a.sec, &host, id, &ip) == 0);
  p = &a.sec.peers[0];
  memcpy(&p->local_spi, capture_request + 4, 4);
  assert(xsec_send_segment(&a.sec, p->local_spi, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  assert(tail - head == 1);
  f = frames[head++ % 256];
  assert(f.size == 164 && !memcmp(f.dest, host_mac, 6));
  assert(!memcmp(f.data, capture_request, 12));         /* SPIs; no responder */
  assert(!memcmp(f.data + 20, zero, 8));                /* no responder nonce */
  assert(!memcmp(f.data + 124, capture_request + 124, 20)); /* key, XNADDR */
  memcpy(p->nonce_local, capture_request + 12, 8);
  memcpy(packet, capture_reply, 152);
  resign(&a, packet);
  assert(xsec_receive(&a.sec, host_mac, packet, 164, 0) == 0);
  assert(p->state == 3 && !p->queued);
  assert(!memcmp(&p->remote_spi, capture_reply + 8, 4));
  assert(!memcmp(p->nonce_remote, capture_reply + 20, 8));
  /* With a segment queued, the reply releases it and no ack (0x227d26). */
  assert(tail - head == 1);
  f = frames[head++ % 256];
  assert(f.size != 164 && !memcmp(f.dest, host_mac, 6));
  memcpy(&ip, f.data, 4);
  assert(ip == (p->remote_spi | 1));
  /* The host's retransmitted reply (same keys, newer serial) finds the queue
   * empty and gets the ack, which names only the responder's SPI. */
  packet[144]++;
  resign(&a, packet);
  assert(xsec_receive(&a.sec, host_mac, packet, 164, 0) == 0);
  assert(tail - head == 1);
  f = frames[head++ % 256];
  assert(f.size == 164 && !memcmp(f.dest, host_mac, 6));
  assert(!memcmp(f.data, zero, 8) && !memcmp(f.data + 8, capture_reply + 8, 4));
  head = tail = 0;

  /* Responder: the captured request gets a reply in the captured layout. */
  init_mac(&b, host_mac);
  assert(xsec_register(&b.sec, id, key) == 0);
  memcpy(packet, capture_request, 152);
  resign(&b, packet);
  assert(xsec_receive(&b.sec, client_mac, packet, 164, 0) == 0);
  assert(tail - head == 1);
  f = frames[head++ % 256];
  p = &b.sec.peers[0];
  assert(f.size == 164 && !memcmp(f.dest, client_mac, 6));
  assert(!memcmp(f.data, zero, 4));
  assert(!memcmp(f.data + 4, capture_request + 4, 4));  /* initiator SPI first */
  assert(!memcmp(f.data + 8, &p->local_spi, 4));        /* ours second */
  assert(!memcmp(f.data + 12, capture_request + 12, 8)); /* initiator nonce */
  assert(!memcmp(f.data + 20, p->nonce_local, 8) &&
         memcmp(f.data + 20, zero, 8));                 /* then ours */
  assert(!memcmp(f.data + 124, capture_reply + 124, 10)); /* key id, size */
  assert(!memcmp(f.data + 134, host_mac, 6));
  head = tail = 0;
  puts("PASS: handshake layout matches a live capture with a stock host, both "
       "roles");
}
static struct frame pop(void) {
  assert(head < tail);
  return frames[head++ % 256];
}
static uint64_t serial_of(const struct frame *f) {
  uint64_t v = 0;
  unsigned i;
  for (i = 0; i < 8; i++)
    v |= (uint64_t)f->data[144 + i] << (i * 8);
  return v;
}
static void pair(const uint8_t *id, const uint8_t *key, uint32_t *ip) {
  init(&a, 1);
  init(&b, 2);
  head = tail = 0;
  test_clock = 0;
  assert(xsec_register(&a.sec, id, key) == 0);
  assert(xsec_register(&b.sec, id, key) == 0);
  *ip = map(&a, &b, id);
}
static int same_keys(const struct xsec_peer *x, const struct xsec_peer *y) {
  return !memcmp(x->tx_auth, y->rx_auth, 16) &&
         !memcmp(x->rx_auth, y->tx_auth, 16) &&
         !memcmp(x->tx_des, y->rx_des, 8) && !memcmp(x->rx_des, y->tx_des, 8);
}
/* A stock responder re-keys on every request with a newer serial (0x227d26),
 * so a retry that crosses its first reply draws a second reply with a new
 * responder nonce. The initiator must end up on the keys the host holds. */
static void rekey_fixture(void) {
  uint8_t id[8] = {3}, key[16] = {4}, segment[13], old_tx[16];
  struct frame r1, r2, y1, y2, d1, ack, bad;
  struct xsec_peer *pa = &a.sec.peers[0], *pb = &b.sec.peers[0];
  uint32_t ip, spi;
  unsigned i;
  pair(id, key, &ip);
  memcpy(segment, fixture_segment, sizeof(segment));
  assert(xsec_send_segment(&a.sec, ip, 17, segment, sizeof(segment)) == 0);
  r1 = pop();
  xsec_tick(&a.sec, 2000);
  r2 = pop();
  assert(!memcmp(r1.data + 12, r2.data + 12, 8) &&
         serial_of(&r2) > serial_of(&r1));
  assert(xsec_receive(&b.sec, r1.src, r1.data, 164, 0) == 0);
  y1 = pop();
  /* Ours would resend its first reply for the same nonce; act stock. */
  pb->state = 3;
  assert(xsec_receive(&b.sec, r2.src, r2.data, 164, 0) == 0);
  y2 = pop();
  assert(memcmp(y1.data + 20, y2.data + 20, 8));
  /* The first reply completes the exchange; the queued segment goes out under
   * the first keys, without an ack. */
  assert(xsec_receive(&a.sec, y1.src, y1.data, 164, 0) == 0);
  d1 = pop();
  assert(head == tail && pa->state == 3 && d1.size != 164);
  memcpy(old_tx, pa->tx_auth, 16);
  /* Rejected without effect: an exact duplicate, a bad MAC, and a correctly
   * signed reply that names another initiator nonce. */
  assert(xsec_receive(&a.sec, y1.src, y1.data, 164, 0) == 10013);
  bad = y2;
  bad.data[160] ^= 1;
  assert(xsec_receive(&a.sec, bad.src, bad.data, 164, 0) == 10013);
  bad = y2;
  bad.data[12] ^= 1;
  resign(&a, bad.data);
  assert(xsec_receive(&a.sec, bad.src, bad.data, 164, 0) == 10013);
  assert(head == tail && !memcmp(pa->tx_auth, old_tx, 16) &&
         !memcmp(pa->nonce_remote, y1.data + 20, 8));
  /* The second reply moves us to the host's new keys; nothing is queued, so
   * it is acked. Replaying it changes nothing. */
  assert(xsec_receive(&a.sec, y2.src, y2.data, 164, 0) == 0);
  ack = pop();
  assert(head == tail && ack.size == 164 && !memcmp(ack.data, "\0\0\0\0", 4));
  assert(same_keys(pa, pb) && memcmp(pa->tx_auth, old_tx, 16));
  assert(xsec_receive(&a.sec, y2.src, y2.data, 164, 0) == 10013);
  assert(head == tail && same_keys(pa, pb));
  /* The host drops the segment sent under the old keys (TCP resends it), takes
   * the ack, and decrypts what follows. */
  assert(xsec_receive(&b.sec, d1.src, d1.data, d1.size, 0) == 10013);
  assert(xsec_receive(&b.sec, ack.src, ack.data, 164, 0) == 0 &&
         pb->state == 3);
  segment[12] = '1';
  assert(xsec_send_segment(&a.sec, ip, 17, segment, sizeof(segment)) == 0);
  d1 = pop();
  assert(xsec_receive(&b.sec, d1.src, d1.data, d1.size, 0) == 0);
  assert(b.delivered == 1 && b.last_size == sizeof(segment) &&
         !memcmp(b.last, segment, sizeof(segment)));
  /* Once more with a new responder SPI as well: it is adopted too. */
  spi = pb->local_spi;
  ((uint8_t *)&spi)[1] ^= 0x40;
  pb->local_spi = spi;
  ++a.sec.serial; /* as if a had sent this request: above its ack's */
  for (i = 0; i < 8; i++)
    r2.data[144 + i] = (uint8_t)(a.sec.serial >> (i * 8));
  resign(&b, r2.data);
  assert(xsec_receive(&b.sec, r2.src, r2.data, 164, 0) == 0);
  y2 = pop();
  assert(xsec_receive(&a.sec, y2.src, y2.data, 164, 0) == 0);
  ack = pop();
  assert(pa->remote_spi == spi && same_keys(pa, pb));
  assert(xsec_receive(&b.sec, ack.src, ack.data, 164, 0) == 0);
  segment[12] = '2';
  assert(xsec_send_segment(&a.sec, ip, 17, segment, sizeof(segment)) == 0);
  d1 = pop();
  memcpy(&ip, d1.data, 4);
  assert(ip == (spi | 1));
  assert(xsec_receive(&b.sec, d1.src, d1.data, d1.size, 0) == 0);
  assert(b.delivered == 2 && b.last[12] == '2');
  head = tail = 0;
  puts("PASS: a stock-style re-key after a retried request is adopted; "
       "duplicate, forged and wrong-nonce replies are rejected");
}
/* A refused frame (full NIC ring) keeps itself and everything behind it
 * queued, in order, until a later tick; it never makes way for the ack. */
static void flush_retry(void) {
  uint8_t id[8] = {5}, key[16] = {6}, segment[13];
  struct frame f;
  uint32_t ip;
  unsigned i;
  pair(id, key, &ip);
  memcpy(segment, fixture_segment, sizeof(segment));
  for (i = 0; i < 3; i++) {
    segment[12] = (uint8_t)('0' + i);
    assert(xsec_send_segment(&a.sec, ip, 17, segment, sizeof(segment)) == 0);
  }
  assert(tail - head == 1 && a.sec.peers[0].queued == 3);
  f = pop();
  assert(xsec_receive(&b.sec, f.src, f.data, f.size, 0) == 0);
  f = pop();
  wire_budget = 1;
  assert(xsec_receive(&a.sec, f.src, f.data, f.size, 0) == 0);
  assert(tail - head == 1 && a.sec.peers[0].state == 3 &&
         a.sec.peers[0].queued == 2);
  xsec_tick(&a.sec, 100);
  assert(tail - head == 1 && a.sec.peers[0].queued == 2);
  segment[12] = '3';
  assert(xsec_send_segment(&a.sec, ip, 17, segment, sizeof(segment)) == 0);
  assert(tail - head == 1 && a.sec.peers[0].queued == 3);
  wire_budget = -1;
  xsec_tick(&a.sec, 200);
  assert(tail - head == 4 && a.sec.peers[0].queued == 0);
  for (i = 0; i < 4; i++) {
    f = pop();
    assert(f.size != 164);
    assert(xsec_receive(&b.sec, f.src, f.data, f.size, 0) == 0);
    assert(b.delivered == i + 1 && b.last[12] == '0' + i);
  }
  head = tail = 0;
  puts("PASS: a send failure while flushing keeps the queue; a later tick "
       "delivers it in order");
}
static void serial_restart(void) {
  uint8_t id[8] = {7}, key[16] = {8}, ethernet[6];
  uint64_t before, saved = test_time;
  uint32_t ip;
  struct frame f;
  test_time = 1000000;
  pair(id, key, &ip);
  assert(a.sec.serial == 1000000);
  assert(xsec_send_segment(&a.sec, ip, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  f = pop();
  before = serial_of(&f);
  assert(before > 1000000);
  /* XNet cleanup and startup in the same boot, with the clock now behind. */
  memcpy(ethernet, a.sec.mac, 6);
  xsec_clear(&a.sec);
  test_time = 999;
  assert(xsec_init(&a.sec, &ops, &a, ethernet, fixture_lan, fixture_title) ==
         0);
  assert(xsec_register(&a.sec, id, key) == 0);
  ip = map(&a, &b, id);
  assert(xsec_send_segment(&a.sec, ip, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  f = pop();
  assert(serial_of(&f) > before);
  /* A clock that moved on wins. */
  xsec_clear(&a.sec);
  test_time = 5000000;
  assert(xsec_init(&a.sec, &ops, &a, ethernet, fixture_lan, fixture_title) ==
         0);
  assert(a.sec.serial == 5000000);
  test_time = saved;
  head = tail = 0;
  puts("PASS: the handshake serial starts at the system time and grows "
       "across restarts");
}
static void retry_cadence(void) {
  uint8_t id[8] = {9}, key[16] = {10};
  struct frame f;
  uint64_t serial;
  uint8_t nonce[8];
  uint32_t ip, t;
  unsigned retries = 0;
  pair(id, key, &ip);
  /* The last tick ran 1.9 s before the segment: timing starts at the segment,
   * not at that tick. */
  xsec_tick(&a.sec, 0);
  test_clock = 1900;
  assert(xsec_send_segment(&a.sec, ip, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  f = pop();
  memcpy(nonce, f.data + 12, 8);
  serial = serial_of(&f);
  for (t = 2000; t <= 12000; t += 100) {
    xsec_tick(&a.sec, t);
    if (head != tail) {
      f = pop();
      assert(head == tail && f.size == 164);
      assert(t == 1900 + 2000 * ++retries);
      assert(!memcmp(f.data + 12, nonce, 8) && serial_of(&f) > serial);
      serial = serial_of(&f);
    }
    if (t < 11900)
      assert(a.sec.peers[0].state == 1 && a.sec.peers[0].queued == 1);
    else
      assert(a.sec.peers[0].state == 0 && a.sec.peers[0].queued == 0);
  }
  assert(retries == 4);
  /* Back to idle, not failed: the next segment starts a new exchange. */
  assert(xsec_send_segment(&a.sec, ip, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  f = pop();
  assert(f.size == 164 && memcmp(f.data + 12, nonce, 8) &&
         a.sec.peers[0].state == 1);
  head = tail = 0;
  puts("PASS: key exchange retries every 2 s from the request, four times, "
       "then goes idle");
}
static void lifecycle(void) {
  uint8_t id[8] = {1}, key[16] = {2}, tcp[20] = {0x14, 0x20, 0x14, 0x21},
          corrupt[XSEC_MAX_PACKET];
  uint32_t ip, old;
  struct halo_xnaddr addr;
  struct frame last;
  unsigned count, i;
  init(&a, 1);
  init(&b, 2);
  head = tail = 0;
  assert(xsec_register(&a.sec, id, key) == 0);
  assert(xsec_register(&b.sec, id, key) == 0);
  assert(xsec_register(&a.sec, id, key) == 10037);
  ip = map(&a, &b, id);
  assert(((uint8_t *)&ip)[0] == 0);
  addr = address(&a);
  assert(xsec_map(&a.sec, &addr, id, &old) == 0 && ((uint8_t *)&old)[0] == 127);
  addr.size = 36;
  assert(xsec_map(&a.sec, &addr, id, &old) == 10022 && old == 0);
  assert(xsec_send_segment(&a.sec, UINT32_MAX, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  pump();
  assert(b.delivered == 1);
  assert(xsec_send_segment(&a.sec, ip, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  assert(tail - head == 1 && frames[head % 256].size == 164);
  pump();
  assert(a.sec.peers[0].state == 3 && b.sec.peers[0].state == 3);
  assert(b.delivered == 2);
  last = frames[(tail - 1) % 256];
  count = b.delivered;
  assert(xsec_receive(&b.sec, last.src, last.data, last.size, 0) == 10013 &&
         b.delivered == count);
  assert(xsec_send_segment(&a.sec, ip, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  last = frames[head++ % 256];
  memcpy(corrupt, last.data, last.size);
  corrupt[20] ^= 1;
  assert(xsec_receive(&b.sec, last.src, corrupt, last.size, 0) == 10013 &&
         b.delivered == count);
  assert(xsec_receive(&b.sec, last.src, last.data, last.size, 0) == 0 &&
         b.delivered == count + 1);
  tcp[12] = 0x50;
  assert(xsec_send_segment(&a.sec, ip, 6, tcp, sizeof(tcp)) == 0);
  pump();
  assert(b.protocol == 6 && b.last_size == 20);
  assert(xsec_send_segment(&b.sec, b.sec.peers[0].local_spi, 6, tcp,
                           sizeof(tcp)) == 0);
  pump();
  assert(a.protocol == 6);
  /* Authenticated malformed padding must not consume the sequence number. */
  assert(xsec_send_segment(&a.sec, ip, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  last = frames[head++ % 256];
  memcpy(corrupt, last.data, last.size);
  assert(cbc(&a, 0, a.sec.peers[0].tx_des, 8, corrupt + 8, corrupt + 16,
             last.size - 28) == 0);
  corrupt[last.size - 14] = 255;
  assert(cbc(&a, 1, a.sec.peers[0].tx_des, 8, corrupt + 8, corrupt + 16,
             last.size - 28) == 0);
  {
    uint8_t tag[20];
    mac(&a, a.sec.peers[0].tx_auth, corrupt, last.size - 12, NULL, 0, tag);
    memcpy(corrupt + last.size - 12, tag, 12);
  }
  assert(xsec_receive(&b.sec, last.src, corrupt, last.size, 0) == 10022);
  assert(xsec_receive(&b.sec, last.src, last.data, last.size, 0) == 0);
  a.sec.peers[0].tx_sequence = UINT32_MAX;
  assert(xsec_send_segment(&a.sec, ip, 6, tcp, 20) == 10060);
  assert(xsec_unregister(&a.sec, id) == 0);
  assert(!a.sec.peers[0].used && a.sec.keys[0].used == 0);
  assert(xsec_register(&a.sec, id, key) == 0);
  old = map(&a, &b, id);
  assert(old != ip);
  assert(xsec_send_segment(&a.sec, old, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  pump();
  assert(a.sec.peers[0].state == 3 && b.sec.peers[0].state == 3);
  /* Retry and give-up (cadence in detail: retry_cadence); the peer goes back
   * to idle and the next segment starts a new exchange on the same address. */
  xsec_clear(&a.sec);
  assert(xsec_register(&a.sec, id, key) == 0);
  old = map(&a, &b, id);
  test_clock = 0;
  assert(xsec_send_segment(&a.sec, old, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  head = tail;
  xsec_tick(&a.sec, 2000);
  assert(tail - head == 1);
  head = tail;
  for (i = 4000; i <= 10000; i += 2000)
    xsec_tick(&a.sec, i);
  assert(tail - head == 3);
  head = tail;
  assert(a.sec.peers[0].state == 0 && a.sec.peers[0].queued == 0);
  assert(map(&a, &b, id) == old);
  assert(xsec_send_segment(&a.sec, old, 6, tcp, 20) == 0);
  assert(a.sec.peers[0].state == 1 && tail - head == 1);
  head = tail;
  xsec_clear(&a.sec);
  for (i = 0; i < XSEC_KEYS; i++) {
    id[0] = (uint8_t)i;
    assert(xsec_register(&a.sec, id, key) == 0);
  }
  id[1] = 1;
  assert(xsec_register(&a.sec, id, key) == 10055);
  xsec_clear(&a.sec);
  puts("PASS: discovery, handshake, UDP/TCP, authentication, replay, retries, "
       "teardown and reconnect");
}
int main(void) {
  OSSL_PROVIDER *legacy = OSSL_PROVIDER_load(NULL, "legacy"),
                *normal = OSSL_PROVIDER_load(NULL, "default");
  assert(legacy && normal);
  fixtures();
  handshake_fixture();
  directional_fixture();
  capture_fixture();
  rekey_fixture();
  flush_retry();
  serial_restart();
  retry_cadence();
  lifecycle();
  OSSL_PROVIDER_unload(legacy);
  OSSL_PROVIDER_unload(normal);
  return 0;
}
