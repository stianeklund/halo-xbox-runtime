#include "../compat/xnet_security.h"
#include <lwip/init.h>
#include <lwip/ip4.h>
#include <lwip/tcpip.h>
#include <lwip/udp.h>
#include <lwip/tcp.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/provider.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

void halo_xnet_attach(struct netif *);
err_t halo_xnet_input(struct pbuf *, struct netif *);
struct netif *halo_net_secure_route(const ip4_addr_t *);
unsigned char XboxLANKey[16] = "TESTTESTTESTTEST";
unsigned char debug_flags;
static unsigned char frame[1600];
static unsigned frame_size, frames_sent, plain_sent, received;
static unsigned random_counter;
static struct { tcpip_callback_fn fn; void *ctx; } callbacks[64];
static unsigned callback_head, callback_tail;

u32_t sys_now(void) { return 0; }
void sys_init(void) {}
err_t sys_mutex_new(sys_mutex_t *m) { *m = (void *)1; return ERR_OK; }
void sys_mutex_lock(sys_mutex_t *m) { assert(*m == (void *)1); }
void sys_mutex_unlock(sys_mutex_t *m) { assert(*m == (void *)1); }
err_t sys_sem_new(sys_sem_t *s, u8_t count) { (void)s; (void)count; assert(0); return ERR_VAL; }
void sys_sem_free(sys_sem_t *s) { (void)s; assert(0); }
u32_t sys_arch_sem_wait(sys_sem_t *s, u32_t ms) { (void)s; (void)ms; assert(0); return 0; }
err_t tcpip_inpkt(struct pbuf *p, struct netif *n, netif_input_fn input)
{ return input(p, n); }
err_t tcpip_input(struct pbuf *p, struct netif *n) { return ip4_input(p, n); }
err_t tcpip_try_callback(tcpip_callback_fn fn, void *ctx)
{
  assert(callback_tail - callback_head < 64);
  callbacks[callback_tail % 64].fn = fn;
  callbacks[callback_tail++ % 64].ctx = ctx;
  return ERR_OK;
}
static void run_callbacks(void)
{
  while (callback_head != callback_tail) {
    unsigned slot = callback_head++ % 64;
    callbacks[slot].fn(callbacks[slot].ctx);
  }
}
void XcHMAC(void *key, uint32_t kn, void *a, uint32_t an, void *b, uint32_t bn, void *out)
{
  unsigned char buf[2048];
  unsigned n = 20;
  assert(an + bn <= sizeof(buf));
  if (an) memcpy(buf, a, an);
  if (bn) memcpy(buf + an, b, bn);
  assert(HMAC(EVP_sha1(), key, kn, buf, an + bn, out, &n));
}
void XcKeyTable(uint32_t cipher, void *table, void *key)
{ memcpy(table, key, cipher ? 24 : 8); }
void XcBlockCryptCBC(uint32_t kind, uint32_t n, void *out, void *in,
                     void *key, uint32_t enc, void *iv)
{
  int size, end;
  EVP_CIPHER *cipher = EVP_CIPHER_fetch(NULL, kind ? "DES-EDE3-CBC" : "DES-CBC", NULL);
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  assert(cipher && ctx);
  assert(EVP_CipherInit_ex2(ctx, cipher, key, iv, enc, NULL));
  assert(EVP_CIPHER_CTX_set_padding(ctx, 0));
  assert(EVP_CipherUpdate(ctx, out, &size, in, n));
  assert(EVP_CipherFinal_ex(ctx, (unsigned char *)out + size, &end));
  assert((unsigned)(size + end) == n);
  EVP_CIPHER_CTX_free(ctx); EVP_CIPHER_free(cipher);
}
uint32_t XcModExp(uint32_t *out, uint32_t *base, uint32_t *exp, uint32_t *mod, uint32_t n)
{
  BIGNUM *b = BN_lebin2bn((void *)base, n * 4, NULL);
  BIGNUM *e = BN_lebin2bn((void *)exp, n * 4, NULL);
  BIGNUM *m = BN_lebin2bn((void *)mod, n * 4, NULL), *r = BN_new();
  BN_CTX *ctx = BN_CTX_new();
  assert(b && e && m && r && ctx && BN_mod_exp(r, b, e, m, ctx));
  assert(BN_bn2lebinpad(r, (void *)out, n * 4) == (int)(n * 4));
  BN_free(b); BN_clear_free(e); BN_free(m); BN_free(r); BN_CTX_free(ctx);
  return 1;
}
int halo_net_random(void *out, unsigned n)
{
  unsigned i;
  for (i = 0; i < n; i++) ((unsigned char *)out)[i] = ++random_counter;
  return 0;
}
static err_t wire(struct netif *n, struct pbuf *p)
{
  (void)n;
  assert(p->tot_len <= sizeof(frame));
  frame_size = pbuf_copy_partial(p, frame, p->tot_len, 0);
  frames_sent++;
  return ERR_OK;
}
static err_t plain(struct netif *n, struct pbuf *p, const ip4_addr_t *dest)
{ (void)n; (void)p; (void)dest; plain_sent++; return ERR_OK; }
static err_t physical_init(struct netif *n)
{
  static const unsigned char mac[] = {2,0,0,0,0,1};
  n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
  n->mtu = 1500; n->hwaddr_len = 6;
  memcpy(n->hwaddr, mac, 6);
  n->output = plain; n->linkoutput = wire;
  return ERR_OK;
}
static void recv_udp(void *user, struct udp_pcb *pcb, struct pbuf *p,
                     const ip_addr_t *source, u16_t port)
{
  char data[4];
  (void)user; (void)pcb; (void)source;
  assert(port == 1000 && pbuf_copy_partial(p, data, 4, 0) == 4);
  if (memcmp(data, "halo", 4))
    fprintf(stderr, "receive %u: %02x %02x %02x %02x, len=%u\n", received,
            (unsigned char)data[0], (unsigned char)data[1],
            (unsigned char)data[2], (unsigned char)data[3], p->tot_len);
  assert(!memcmp(data, "halo", 4));
  received++; pbuf_free(p);
}
static void inject(struct netif *n, const unsigned char *data, unsigned size)
{
  struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_RAM);
  assert(p && pbuf_take(p, data, size) == ERR_OK);
  assert(halo_xnet_input(p, n) == ERR_OK);
}
static struct tcp_pcb *local_accepted;
static unsigned local_connected, local_received;
static err_t recv_local(void *user, struct tcp_pcb *pcb, struct pbuf *p, err_t error)
{
  char data[4];
  (void)user;
  assert(error == ERR_OK && p && p->tot_len == sizeof(data));
  assert(pbuf_copy_partial(p, data, sizeof(data), 0) == sizeof(data));
  assert(!memcmp(data, "halo", sizeof(data)));
  tcp_recved(pcb, p->tot_len);
  pbuf_free(p);
  local_received++;
  return ERR_OK;
}
static err_t accept_local(void *user, struct tcp_pcb *pcb, err_t error)
{
  (void)user;
  assert(error == ERR_OK && !local_accepted);
  local_accepted = pcb;
  tcp_recv(pcb, recv_local);
  return ERR_OK;
}
static err_t connect_local(void *user, struct tcp_pcb *pcb, err_t error)
{
  (void)user;
  assert(error == ERR_OK && ip4_addr_isloopback(ip_2_ip4(&pcb->local_ip)));
  local_connected++;
  return ERR_OK;
}
static void test_local_server(void)
{
  struct tcp_pcb *listener = tcp_new(), *client = tcp_new();
  ip_addr_t dest;
  unsigned wire_before = frames_sent, plain_before = plain_sent;
  assert(listener && client);
  assert(tcp_bind(listener, IP_ADDR_ANY, 5150) == ERR_OK);
  listener = tcp_listen(listener);
  assert(listener);
  tcp_accept(listener, accept_local);
  IP_ADDR4(&dest, 127,0,0,1);
  assert(tcp_connect(client, &dest, 5150, connect_local) == ERR_OK);
  run_callbacks();
  assert(local_connected == 1 && local_accepted);
  assert(tcp_write(client, "halo", 4, TCP_WRITE_FLAG_COPY) == ERR_OK);
  assert(tcp_output(client) == ERR_OK);
  run_callbacks();
  assert(local_received == 1);
  assert(frames_sent == wire_before && plain_sent == plain_before);
  tcp_abort(client);
  tcp_abort(local_accepted);
  local_accepted = NULL;
  assert(tcp_close(listener) == ERR_OK);
  run_callbacks();
}
int main(void)
{
  struct netif physical;
  struct udp_pcb *udp;
  struct tcp_pcb *tcp;
  struct pbuf *p;
  ip4_addr_t zero = {0};
  ip_addr_t dest;
  struct halo_xnaddr peer = {12, {2,0,0,0,0,2}, 0};
  uint8_t id[8] = {1}, key[16] = {2};
  uint32_t mapped;
  unsigned char damaged[1600];
  OSSL_PROVIDER *legacy = OSSL_PROVIDER_load(NULL, "legacy");
  OSSL_PROVIDER *normal = OSSL_PROVIDER_load(NULL, "default");
  assert(legacy && normal);
  lwip_init();
  assert(netif_add(&physical, &zero, &zero, &zero, NULL, physical_init, ip4_input));
  netif_set_default(&physical); netif_set_up(&physical); netif_set_link_up(&physical);
  halo_xnet_attach(&physical);
  assert(halo_net_security_start(0) == 0);
  udp = udp_new(); assert(udp && udp_bind(udp, IP_ADDR_ANY, 1000) == ERR_OK);
  udp->so_options |= SOF_BROADCAST;
  udp_recv(udp, recv_udp, NULL);
  p = pbuf_alloc(PBUF_TRANSPORT, 4, PBUF_RAM); assert(p);
  assert(pbuf_take(p, "halo", 4) == ERR_OK);
  IP_ADDR4(&dest, 255,255,255,255);
  assert(halo_net_secure_route(ip_2_ip4(&dest)));
  assert(udp_sendto(udp, p, &dest, 1000) == ERR_OK);
  assert(frames_sent == 1 && plain_sent == 0);
  assert(frame[12] == 8 && frame[23] == 17);
  assert(frame[34] == 12 && frame[35] == 2 && frame[36] == 12 && frame[37] == 2);
  assert(!memcmp(frame + 42, "\xff\xff\xff\xff", 4));
  frame[11] = 2; /* Receive the same encrypted discovery from another MAC. */
  inject(&physical, frame, frame_size);
  assert(received == 1);
  memcpy(damaged, frame, frame_size); damaged[60] ^= 1;
  inject(&physical, damaged, frame_size);
  assert(received == 1);
  /* lwIP loopback bypasses the physical adapter and System Link encryption. */
  pbuf_free(p);
  p = pbuf_alloc(PBUF_TRANSPORT, 4, PBUF_RAM); assert(p);
  assert(pbuf_take(p, "halo", 4) == ERR_OK);
  IP_ADDR4(&dest, 127,0,0,1);
  assert(udp_sendto(udp, p, &dest, 1000) == ERR_OK);
  run_callbacks();
  assert(received == 2 && frames_sent == 1);
  test_local_server();
  assert(halo_net_register(id, key) == 0 && halo_net_map(&peer, id, &mapped) == 0);
  ip_addr_set_ip4_u32(&dest, mapped);
  tcp = tcp_new(); assert(tcp);
  assert(tcp_connect(tcp, &dest, 1001, NULL) == ERR_OK);
  assert(frames_sent == 2 && !memcmp(frame + 42, "\0\0\0\0", 4));
  tcp_abort(tcp); udp_remove(udp); pbuf_free(p);
  halo_net_security_clear();
  assert(halo_net_security_start(1) == 0);
  udp = udp_new(); assert(udp && udp_bind(udp, IP_ADDR_ANY, 1000) == ERR_OK);
  udp->so_options |= SOF_BROADCAST;
  p = pbuf_alloc(PBUF_TRANSPORT, 4, PBUF_RAM); assert(p);
  assert(pbuf_take(p, "halo", 4) == ERR_OK);
  IP_ADDR4(&dest, 255,255,255,255);
  assert(udp_sendto(udp, p, &dest, 1000) == ERR_OK);
  assert(plain_sent == 0 && !memcmp(frame + 42, "\xff\xff\xff\xff", 4));
  pbuf_free(p); udp_remove(udp); halo_net_security_clear();
  extern void test_debug_netif(void);
  test_debug_netif();
  OSSL_PROVIDER_unload(legacy); OSSL_PROVIDER_unload(normal);
  puts("PASS: real lwIP discovery without DHCP, packet checksums, tamper rejection, UDP loopback, local TCP server on 5150, TCP secure routing, bypass broadcast and shared XBDM NIC ownership/address separation");
  return 0;
}
