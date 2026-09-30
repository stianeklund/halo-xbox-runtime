/* Shared-NIC ownership tests using real lwIP pbufs and the production adapter.
 * The fake XBDM retains TX tokens and lends short-lived RX frames, just as the
 * resident interface does. Target thiscall code is also compiled by CMake. */
#define HALO_DEBUG_NET_HOST_TEST
#include "../crt/nxdk_debug_net.c"
#include <assert.h>
#include <stdlib.h>

static struct shared_interface *resident;
static struct title_interface *registered;
static unsigned irql, link = 1, allocations, native_calls;
static int available = 1, attach_result;
static void *pending;
static const void *pending_frame;
static unsigned pending_length;
static struct pbuf *received_frame;

void *halo_debug_test_interface(void) { return resident; }
KIRQL KeRaiseIrqlToDpcLevel(void) {
  KIRQL old = irql;
  irql = 2;
  return old;
}
void KfLowerIrql(KIRQL old) { irql = old; }
void *ExAllocatePoolWithTag(unsigned long size, ULONG tag) {
  (void)tag;
  void *p = malloc(size);
  if (p) allocations++;
  return p;
}
void ExFreePool(void *p) {
  assert(allocations);
  allocations--;
  free(p);
}
unsigned long PhyGetLinkState(unsigned long flag) { (void)flag; return link; }
void DbgPrint(const char *format, ...) { (void)format; }
err_t nvnetif_init(struct netif *netif) {
  (void)netif;
  native_calls++;
  return ERR_OK;
}
static int attach(struct shared_interface *self, unsigned rx, unsigned tx,
                   void *mac, unsigned *mode) {
  assert(irql == 2 && self == resident && rx == 64 && tx == 64);
  memcpy(mac, "TITLE!", 6);
  *mode = 0;
  return attach_result;
}
static void register_title(struct shared_interface *self, struct title_interface *t) {
  assert(irql == 2 && self == resident);
  registered = t;
}
static int debug_address(struct shared_interface *self, struct halo_xnaddr *out) {
  assert(irql == 2 && self == resident);
  out->size = sizeof(*out);
  memcpy(out->ethernet, "DEBUG!", 6);
  out->address = PP_HTONL(0x0a00020f);
  return 6;
}
static void send_packet(struct shared_interface *self, void *token,
                         const void *frame, unsigned size) {
  assert(irql == 2 && self == resident && !pending);
  pending = token;
  pending_frame = frame;
  pending_length = size;
}
static int can_send(struct shared_interface *self) {
  assert(irql == 2 && self == resident);
  return available;
}
static const struct shared_methods methods = {
  NULL, attach, register_title, NULL, debug_address, send_packet, can_send
};
static err_t capture_receive(struct pbuf *p, struct netif *netif) {
  (void)netif;
  assert(irql == 2 && !received_frame);
  received_frame = p;
  return ERR_OK;
}

void test_debug_netif(void) {
  static struct netif netif;
  struct shared_interface object = {&methods, XBD6_MAGIC};
  struct halo_xnaddr debug, title_addr;
  struct pbuf *p, *tail;
  unsigned char frame[60], copy[60];
  unsigned calls;
  KIRQL old;
  ip4_addr_t ip = {PP_HTONL(0x0a000210)};
  assert(halo_debug_netif_init(&netif) == ERR_OK && native_calls == 1);
  assert(halo_net_debug_address(&debug) == 1 && !debug.address);
  resident = &object;
  object.magic = 0x58424437;
  assert(halo_debug_netif_init(&netif) == ERR_IF && native_calls == 1);
  object.magic = XBD6_MAGIC;
  attach_result = -1;
  assert(halo_debug_netif_init(&netif) == ERR_IF && !registered && native_calls == 1);
  attach_result = 0;
  netif.input = capture_receive;
  netif_set_ipaddr(&netif, &ip);
  assert(halo_debug_netif_init(&netif) == ERR_OK && registered && irql == 0);
  assert(!memcmp(netif.hwaddr, "TITLE!", 6));
  calls = native_calls;
  assert(halo_net_debug_address(&debug) == 6);
  old = KeRaiseIrqlToDpcLevel();
  assert(registered->methods->address(registered, &title_addr) == 6);
  KfLowerIrql(old);
  assert(title_addr.address == PP_HTONL(0x0a000210));
  assert(debug.address == PP_HTONL(0x0a00020f) && debug.address != title_addr.address);
  assert(!memcmp(debug.ethernet, "DEBUG!", 6));
  assert(!memcmp(title_addr.ethernet, "TITLE!", 6));

  memset(frame, 0x5a, sizeof(frame));
  p = pbuf_alloc(PBUF_RAW, 30 + ETH_PAD_SIZE, PBUF_RAM);
  tail = pbuf_alloc(PBUF_RAW, 30, PBUF_RAM);
  assert(p && tail);
  pbuf_cat(p, tail);
  pbuf_take_at(p, frame, sizeof(frame), ETH_PAD_SIZE);
  assert(netif.linkoutput(&netif, p) == ERR_OK && allocations == 1);
  assert(pending_length == sizeof(frame) && !memcmp(pending_frame, frame, sizeof(frame)));
  pbuf_free(p);
  assert(!memcmp(pending_frame, frame, sizeof(frame))); /* DMA still owns copy. */
  old = KeRaiseIrqlToDpcLevel();
  registered->methods->complete(registered, pending);
  KfLowerIrql(old);
  pending = NULL;
  assert(!allocations && irql == 0);

  p = pbuf_alloc(PBUF_RAW, sizeof(frame) + ETH_PAD_SIZE, PBUF_RAM);
  assert(p);
  available = 0;
  assert(netif.linkoutput(&netif, p) == ERR_MEM && !allocations && !pending);
  available = 1;
  pbuf_free(p);

  old = KeRaiseIrqlToDpcLevel();
  registered->methods->receive(registered, 0x601, frame + 14, sizeof(frame) - 14, 8);
  KfLowerIrql(old);
  assert(received_frame);
  memset(frame, 0, sizeof(frame)); /* XBDM immediately recycles loaned frame. */
  assert(pbuf_copy_partial(received_frame, copy, sizeof(copy), ETH_PAD_SIZE) == sizeof(copy));
  for (unsigned i = 0; i < sizeof(copy); i++) assert(copy[i] == 0x5a);
  pbuf_free(received_frame);
  received_frame = NULL;
  old = KeRaiseIrqlToDpcLevel();
  registered->methods->receive(registered, 0, frame + 14, 1501, 8);
  KfLowerIrql(old);
  assert(!received_frame && native_calls == calls);

  link = 0;
  poll_link(&netif);
  old = KeRaiseIrqlToDpcLevel();
  assert(registered->methods->address(registered, &title_addr) == 0);
  KfLowerIrql(old);
  link = 1;
  ip.addr = PP_HTONL(0x0a000211);
  netif_set_ipaddr(&netif, &ip);
  poll_link(&netif);
  old = KeRaiseIrqlToDpcLevel();
  assert(registered->methods->address(registered, &title_addr) == 6);
  KfLowerIrql(old);
  assert(title_addr.address == PP_HTONL(0x0a000211));
  assert(halo_net_debug_address(&debug) == 6 && debug.address == PP_HTONL(0x0a00020f));
  resident = NULL;
}
