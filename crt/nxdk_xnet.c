/* XNET packet boundary: Ethernet/MAC transport outside, lwIP TCP/UDP inside.
 * APIs and timer/input/output callbacks hold lwIP's core lock. The NIC ISR
 * only enqueues pbufs; no crypto or session mutation runs in interrupt context.
 */
#include "../compat/xnet_security.h"
#include "xnet_profile.h"
#include <lwip/inet_chksum.h>
#include <lwip/ip4.h>
#include <lwip/tcpip.h>
#include <lwip/timeouts.h>
#include <netif/ethernet.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

static struct xsec security;
static struct netif *interface;
static struct netif secure_interface;
static netif_output_fn plain_output;
static unsigned security_refs, startup_flags;

static int random_bytes(void *user, void *out, unsigned n) {
  (void)user;
  return halo_net_random(out, n);
}
static void hmac_sha1(void *user, const uint8_t *key, const void *a,
                      unsigned an, const void *b, unsigned bn, uint8_t *out) {
  (void)user;
  XcHMAC((void *)key, 16, (void *)a, an, (void *)b, bn, out);
}
static int cbc(void *user, int encrypt, const uint8_t *key, unsigned keylen,
               const uint8_t *iv, uint8_t *data, unsigned n) {
  uint8_t table[384], feedback[8];
  (void)user;
  memcpy(feedback, iv, 8);
  XcKeyTable(keylen == 8 ? 0 : 1, table, (void *)key);
  XcBlockCryptCBC(keylen == 8 ? 0 : 1, n, data, data, table, encrypt, feedback);
  return 0;
}
static int modexp(void *user, const uint8_t *base, const uint8_t *exponent,
                  uint8_t *out) {
  ULONG b[24], e[24], m[24], a[24];
  (void)user;
  memcpy(b, base, 96);
  memcpy(e, exponent, 96);
  memcpy(m, xsec_modulus, 96);
  if (!XcModExp(a, b, e, m, 24))
    return 10091;
  memcpy(out, a, 96);
  memset(e, 0, sizeof(e));
  return 0;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t *p, unsigned n) {
  p[0] = (uint8_t)(n >> 8);
  p[1] = (uint8_t)n;
}
static uint16_t sum(const uint8_t *p, unsigned n, uint32_t v) {
  while (n >= 2) {
    v += get16(p);
    p += 2;
    n -= 2;
  }
  if (n)
    v += (uint32_t)p[0] << 8;
  while (v >> 16)
    v = (v & 65535) + (v >> 16);
  return (uint16_t)~v;
}
static uint16_t transport_sum(const uint8_t *ip, const uint8_t *data,
                              unsigned n, uint8_t protocol) {
  uint32_t v = get16(ip + 12) + get16(ip + 14) + get16(ip + 16) +
               get16(ip + 18) + protocol + n;
  return sum(data, n, v);
}
static int wire_send(void *user, const uint8_t *mac, const uint8_t *data,
                     unsigned n, int broadcast) {
  uint8_t frame[14 + 20 + 8 + XSEC_MAX_PACKET] = {0}, *ip = frame + 14,
                              *udp = ip + 20;
  struct pbuf *p;
  err_t r;
  (void)user;
  if (n > XSEC_MAX_PACKET || !interface)
    return 10040;
  memcpy(frame, mac, 6);
  memcpy(frame + 6, interface->hwaddr, 6);
  frame[12] = 8;
  ip[0] = 0x45;
  ip[8] = 64;
  ip[9] = 17;
  ip[15] = 1;
  if (broadcast)
    memset(ip + 16, 255, 4);
  else
    ip[19] = 1;
  put16(ip + 2, 28 + n);
  put16(ip + 10, sum(ip, 20, 0));
  put16(udp, 3074);
  put16(udp + 2, 3074);
  put16(udp + 4, 8 + n);
  memcpy(udp + 8, data, n);
  put16(udp + 6, transport_sum(ip, udp, 8 + n, 17));
  if (!get16(udp + 6))
    put16(udp + 6, 65535);
  p = pbuf_alloc(PBUF_RAW, 14 + 28 + n, PBUF_RAM);
  if (!p)
    return 10055;
  pbuf_take(p, frame, 14 + 28 + n);
  r = interface->linkoutput(interface, p);
  pbuf_free(p);
  return r == ERR_OK ? 0 : 10055;
}
static void deliver(void *user, uint32_t source, uint8_t protocol,
                    const uint8_t *segment, unsigned n) {
  struct pbuf *p;
  uint8_t *ip, *body;
  unsigned checksum_offset = protocol == 6 ? 16 : 6;
  uint16_t checksum;
  (void)user;
  p = pbuf_alloc(PBUF_RAW, 20 + n, PBUF_RAM);
  if (!p)
    return;
  ip = p->payload;
  memset(ip, 0, 20);
  ip[0] = 0x45;
  ip[8] = 64;
  ip[9] = protocol;
  put16(ip + 2, 20 + n);
  if (source == UINT32_MAX) {
    ip[15] = 1;
    memset(ip + 16, 255, 4);
  } else {
    memcpy(ip + 12, &source, 4);
    memcpy(ip + 16, &netif_ip4_addr(&secure_interface)->addr, 4);
  }
  body = ip + 20;
  memcpy(body, segment, n);
  body[checksum_offset] = body[checksum_offset + 1] = 0;
  checksum = transport_sum(ip, body, n, protocol);
  put16(body + checksum_offset, checksum ? checksum : 65535);
  put16(ip + 10, sum(ip, 20, 0));
  ip4_input(p, &secure_interface);
}
static const struct xsec_ops ops = {random_bytes, hmac_sha1, cbc,
                                    modexp,       wire_send, deliver};
static void timer(void *unused) {
  (void)unused;
  if (security_refs)
    xsec_tick(&security, sys_now());
  sys_timeout(100, timer, NULL);
}
static err_t secure_output(struct netif *netif, struct pbuf *p,
                           const ip4_addr_t *dest) {
  uint8_t data[1500], *segment;
  unsigned header, n;
  uint16_t port = 0;
  int r, system_service = 0;
  if (!security_refs)
    return plain_output(netif, p, dest);
  if (p->tot_len > sizeof(data) ||
      pbuf_copy_partial(p, data, p->tot_len, 0) != p->tot_len)
    return ERR_BUF;
  header = (data[0] & 15) * 4;
  n = p->tot_len;
  if (header < 20 || header > n || get16(data + 2) != n ||
      (get16(data + 6) & 0x3fff))
    return ERR_VAL;
  segment = data + header;
  n -= header;
  if (data[9] == 17 && n >= 8) {
    port = get16(segment + 2);
    system_service = port == 67 || port == 68 || port == 53;
  }
  if (system_service ||
      ((startup_flags & 1) && dest->addr != UINT32_MAX &&
       ((const uint8_t *)&dest->addr)[0] != 0))
    return plain_output(netif, p, dest);
  /* Only broadcasts and virtual addresses belong to the secure LAN. */
  if (dest->addr != UINT32_MAX && ((const uint8_t *)&dest->addr)[0] != 0)
    return ERR_RTE;
  if (data[9] != 6 && data[9] != 17)
    return ERR_RTE;
  r = xsec_send_segment(&security, dest->addr, data[9], segment, n);
  return !r ? ERR_OK : r == 10055 ? ERR_MEM : r == 10040 ? ERR_BUF : ERR_RTE;
}
static err_t secure_ethernet_input(struct pbuf *p, struct netif *netif) {
  uint8_t frame[14 + 1500], *ip = frame + 14, *udp;
  unsigned header, total, n;
  int broadcast;
  if (!security_refs || p->tot_len < 14 + 20 || p->tot_len > sizeof(frame) ||
      pbuf_copy_partial(p, frame, p->tot_len, 0) != p->tot_len)
    return ethernet_input(p, netif);
  if (get16(frame + 12) != 0x800 || (ip[0] >> 4) != 4)
    return ethernet_input(p, netif);
  header = (ip[0] & 15) * 4;
  total = get16(ip + 2);
  if (header < 20 || total < header || total > (unsigned)p->tot_len - 14) {
    pbuf_free(p);
    return ERR_OK;
  }
  if (ip[9] == 6 && !(startup_flags & 1)) {
    pbuf_free(p);
    return ERR_OK;
  }
  if (ip[9] != 17 || total < header + 8)
    return ethernet_input(p, netif);
  udp = ip + header;
  n = total - header;
  if (get16(udp) != 3074 || get16(udp + 2) != 3074) {
    if (!(startup_flags & 1) && get16(udp) != 53 && get16(udp) != 67 &&
        get16(udp) != 68) {
      pbuf_free(p);
      return ERR_OK;
    }
    return ethernet_input(p, netif);
  }
  broadcast = !memcmp(ip + 16, (const uint8_t[4]){255, 255, 255, 255}, 4);
  if (!memcmp(ip + 12, (const uint8_t[4]){0, 0, 0, 1}, 4) &&
      (broadcast || !memcmp(ip + 16, ip + 12, 4))) {
    if (!(get16(ip + 6) & 0x3fff) && !sum(ip, header, 0) &&
        get16(udp + 4) == n &&
        (!get16(udp + 6) || !transport_sum(ip, udp, n, 17)))
      xsec_receive(&security, frame + 6, udp + 8, n - 8, broadcast);
    pbuf_free(p);
    return ERR_OK;
  }
  /* Secure startup never delivers plaintext XNet port traffic to Halo. */
  if (!(startup_flags & 1)) {
    pbuf_free(p);
    return ERR_OK;
  }
  return ethernet_input(p, netif);
}
err_t halo_xnet_input(struct pbuf *p, struct netif *netif) {
  return tcpip_inpkt(p, netif, secure_ethernet_input);
}
struct netif *halo_net_secure_route(const ip4_addr_t *dest) {
  /* System Link discovery works before DHCP assigns a physical IPv4 address.
   * DHCP itself explicitly selects the physical interface in lwIP. */
  return security_refs && dest->addr == UINT32_MAX ? &secure_interface : NULL;
}
static err_t secure_netif_init(struct netif *netif) {
  netif->name[0] = 's';
  netif->name[1] = 'l';
  netif->hwaddr_len = 6;
  memcpy(netif->hwaddr, interface->hwaddr, 6);
  netif->flags = NETIF_FLAG_BROADCAST;
  netif->mtu = 1420;
  netif->output = secure_output;
  netif->linkoutput = interface->linkoutput;
  return ERR_OK;
}
void halo_xnet_attach(struct netif *netif) {
  ip4_addr_t ip, mask, gateway;
  interface = netif;
  plain_output = netif->output;
  netif->output = secure_output;
  /* A stable System Link source and route for 0/8. DHCP changes only the
   * physical interface; active TCP PCBs keep 0.0.0.1 throughout the session. */
  IP4_ADDR(&ip, 0, 0, 0, 1);
  IP4_ADDR(&mask, 255, 0, 0, 0);
  ip4_addr_set_zero(&gateway);
  netif_add(&secure_interface, &ip, &mask, &gateway, NULL, secure_netif_init,
            ip4_input);
  netif_set_up(&secure_interface);
  netif_set_link_up(&secure_interface);
  sys_timeout(100, timer, NULL);
}
int halo_net_security_start(unsigned flags) {
  int r = 0;
  if (flags & ~1u)
    return 10022;
  LOCK_TCPIP_CORE();
  if (security_refs && flags != startup_flags)
    r = 10022;
  else if (!security_refs) {
    r = xsec_init(&security, &ops, NULL, interface->hwaddr, XboxLANKey,
                  halo_title_lan_key);
    startup_flags = flags;
  }
  if (!r)
    ++security_refs;
  UNLOCK_TCPIP_CORE();
  return r;
}
void halo_net_security_clear(void) {
  LOCK_TCPIP_CORE();
  security_refs = 0;
  xsec_clear(&security);
  UNLOCK_TCPIP_CORE();
}
int halo_net_register(const void *id, const void *key) {
  int r;
  LOCK_TCPIP_CORE();
  r = xsec_register(&security, id, key);
  UNLOCK_TCPIP_CORE();
  return r;
}
int halo_net_unregister(const void *id) {
  int r;
  LOCK_TCPIP_CORE();
  r = xsec_unregister(&security, id);
  UNLOCK_TCPIP_CORE();
  return r;
}
int halo_net_map(const struct halo_xnaddr *addr, const void *id, uint32_t *ip) {
  int r;
  LOCK_TCPIP_CORE();
  r = xsec_map(&security, addr, id, ip);
  UNLOCK_TCPIP_CORE();
  return r;
}
