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
static struct xsec_ops ops = {random_bytes, mac, cbc, modexp, wire, deliver};
static void init(struct endpoint *e, int identity) {
  uint8_t ethernet[6] = {2, 0, 0, 0, 0, 0};
  ethernet[5] = (uint8_t)identity;
  memset(e, 0, sizeof(*e));
  assert(xsec_init(&e->sec, &ops, e, ethernet, fixture_lan, fixture_title) ==
         0);
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
  memcpy(hs + 4, (const uint8_t[]){0, 0x22, 0x33, 0}, 4);
  memcpy(hs + 8, &ip, 4);
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
  /* Retry and timeout; an explicit remap permits reconnect after failure. */
  xsec_clear(&a.sec);
  assert(xsec_register(&a.sec, id, key) == 0);
  old = map(&a, &b, id);
  assert(xsec_send_segment(&a.sec, old, 17, fixture_segment,
                           sizeof(fixture_segment)) == 0);
  head = tail;
  xsec_tick(&a.sec, 500);
  assert(tail - head == 1);
  head = tail;
  xsec_tick(&a.sec, 10000);
  assert(a.sec.peers[0].state == 4 && a.sec.peers[0].queued == 0);
  assert(xsec_send_segment(&a.sec, old, 6, tcp, 20) == 10060);
  assert(map(&a, &b, id) == old);
  assert(a.sec.peers[0].state == 0);
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
  lifecycle();
  OSSL_PROVIDER_unload(legacy);
  OSSL_PROVIDER_unload(normal);
  return 0;
}
