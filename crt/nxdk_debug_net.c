/* XBDM owns the NIC on a development kernel. Use its XBD6 sharing ABI instead
 * of nvnetdrv_init(), which resets the adapter and disconnects the debugger.
 *
 * ABI evidence: cachebeta.xbe 2276, 0x229079 (FS:20 -> +24c, XBD6/attach),
 * 0x228a69 (register callbacks), 0x228db2 (send), 0x228fc2 (receive),
 * 0x228fb1 (completion), 0x228aa9 (debug address). Confirmed against resident
 * XBDM 1.00.5849.4: the same magic, slots, thiscall and 12-byte debug XNADDR.
 * All shared-driver operations execute at DISPATCH_LEVEL, as XNet does.
 */
#include "nxdk_debug_net.h"
#include <lwip/dhcp.h>
#include <lwip/etharp.h>
#include <lwip/ethip6.h>
#include <lwip/timeouts.h>
#include <string.h>
#include <xboxkrnl/xboxkrnl.h>

#define XBD6_MAGIC 0x58424436u
#define ETHERNET_HEADER 14
#define ETHERNET_MAX 1514

#ifdef HALO_DEBUG_NET_HOST_TEST
#define NIC_CALL
extern void *halo_debug_test_interface(void);
#else
#define NIC_CALL __attribute__((thiscall))
#endif

struct title_interface;
struct shared_interface;
struct shared_methods {
  void *destroy;
  int (NIC_CALL *attach)(struct shared_interface *, unsigned, unsigned,
                         void *, unsigned *);
  void (NIC_CALL *register_title)(struct shared_interface *, struct title_interface *);
  void *detach;
  int (NIC_CALL *debug_address)(struct shared_interface *, struct halo_xnaddr *);
  void (NIC_CALL *send)(struct shared_interface *, void *, const void *, unsigned);
  int (NIC_CALL *can_send)(struct shared_interface *);
};
struct shared_interface {
  const struct shared_methods *methods;
  unsigned magic;
};
struct title_methods {
  void (NIC_CALL *complete)(struct title_interface *, void *);
  void (NIC_CALL *receive)(struct title_interface *, unsigned, const void *,
                           unsigned, unsigned);
  void (NIC_CALL *available)(struct title_interface *);
  int (NIC_CALL *address)(struct title_interface *, struct halo_xnaddr *);
};
struct title_interface {
  const struct title_methods *methods;
};
struct transmit_packet {
  unsigned char frame[ETHERNET_MAX];
};

static struct shared_interface *shared;
static struct netif *title_netif;
static struct halo_xnaddr title_address;
static int title_status = 1;
static int initializing;
err_t nvnetif_init(struct netif *netif);

static struct shared_interface *find_shared(void) {
#ifdef HALO_DEBUG_NET_HOST_TEST
  return halo_debug_test_interface();
#else
  unsigned char *processor;
  __asm__ volatile("movl %%fs:0x20, %0" : "=r"(processor));
  return *(struct shared_interface **)(processor + 0x24c);
#endif
}

/* XBDM queries this at DPC level. Never take lwIP's core mutex from a DPC:
 * it could have interrupted the thread holding that mutex. Publish a small
 * snapshot from the TCP/IP thread with DPCs excluded instead. */
void halo_debug_netif_changed(struct netif *netif) {
  KIRQL previous;
  struct halo_xnaddr address = {0};
  int status;
  if (netif != title_netif)
    return;
  address.size = sizeof(address);
  memcpy(address.ethernet, netif->hwaddr, 6);
  address.address = netif_ip4_addr(netif)->addr;
  status = netif_is_link_up(netif) ?
      2 | (address.address ? (netif_dhcp_data(netif) ? 8 : 4) : 0) : 0;
  previous = KeRaiseIrqlToDpcLevel();
  title_address = address;
  title_status = status;
  KfLowerIrql(previous);
}

static void NIC_CALL transmit_complete(struct title_interface *self, void *packet) {
  (void)self;
  ExFreePool(packet);
}

static void NIC_CALL receive_packet(struct title_interface *self, unsigned flags,
                                    const void *payload, unsigned length,
                                    unsigned type) {
  struct pbuf *p;
  (void)self;
  (void)flags;
  (void)type;
  /* XBDM loans the frame only until this callback returns. Copy the Ethernet
   * header too; the packet adapter checks its MACs before queuing to lwIP. */
  if (!title_netif || initializing || length > 1500)
    return;
  p = pbuf_alloc(PBUF_RAW, length + ETHERNET_HEADER + ETH_PAD_SIZE, PBUF_RAM);
  if (!p)
    return;
#if ETH_PAD_SIZE
  pbuf_remove_header(p, ETH_PAD_SIZE);
#endif
  pbuf_take(p, (const unsigned char *)payload - ETHERNET_HEADER,
             length + ETHERNET_HEADER);
#if ETH_PAD_SIZE
  pbuf_add_header(p, ETH_PAD_SIZE);
#endif
  if (title_netif->input(p, title_netif) != ERR_OK)
    pbuf_free(p);
}

static void NIC_CALL transmit_available(struct title_interface *self) {
  /* A full ring returns ERR_MEM; TCP retries through its normal timers. */
  (void)self;
}

static int NIC_CALL get_title_address(struct title_interface *self,
                                      struct halo_xnaddr *address) {
  (void)self;
  *address = title_address;
  return title_status;
}

static const struct title_methods callbacks = {
  transmit_complete, receive_packet, transmit_available, get_title_address
};
static struct title_interface title = {&callbacks};

static err_t shared_output(struct netif *netif, struct pbuf *p) {
  struct transmit_packet *packet;
  unsigned length = p->tot_len;
  KIRQL previous;
  (void)netif;
  if (length < ETHERNET_HEADER + ETH_PAD_SIZE ||
      length > ETHERNET_MAX + ETH_PAD_SIZE)
    return ERR_BUF;
  packet = ExAllocatePoolWithTag(sizeof(*packet), 0x74654e48u); /* HNet */
  if (!packet)
    return ERR_MEM;
  length -= ETH_PAD_SIZE;
  if (pbuf_copy_partial(p, packet->frame, length, ETH_PAD_SIZE) != length) {
    ExFreePool(packet);
    return ERR_BUF;
  }
  previous = KeRaiseIrqlToDpcLevel();
  if (!shared->methods->can_send(shared)) {
    KfLowerIrql(previous);
    ExFreePool(packet);
    return ERR_MEM;
  }
  /* Ownership passes to XBDM until complete(token), including synchronous
   * completion. The original pbuf can be released immediately by lwIP. */
  shared->methods->send(shared, packet, packet->frame, length);
  KfLowerIrql(previous);
  return ERR_OK;
}

static void poll_link(void *context) {
  struct netif *netif = context;
  if (PhyGetLinkState(0) & XNET_ETHERNET_LINK_ACTIVE)
    netif_set_link_up(netif);
  else
    netif_set_link_down(netif);
  halo_debug_netif_changed(netif);
  sys_timeout(250, poll_link, netif);
}

err_t halo_debug_netif_init(struct netif *netif) {
  KIRQL previous;
  unsigned mode = 0;
  int result;
  shared = find_shared();
  if (!shared)
    return nvnetif_init(netif);
  if (shared->magic != XBD6_MAGIC) {
    DbgPrint("[net] unsupported XBDM sharing ABI %08x; preserving debug NIC\n",
             shared->magic);
    shared = NULL;
    return ERR_IF;
  }
  netif->name[0] = 'x';
  netif->name[1] = 'd';
  netif->mtu = 1500;
  netif->hwaddr_len = 6;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  netif->output = etharp_output;
#if LWIP_IPV6
  netif->output_ip6 = ethip6_output;
#endif
  netif->linkoutput = shared_output;
  title_netif = netif;
  initializing = 1;
  previous = KeRaiseIrqlToDpcLevel();
  result = shared->methods->attach(shared, 64, 64, netif->hwaddr, &mode);
  if (result >= 0)
    shared->methods->register_title(shared, &title);
  KfLowerIrql(previous);
  if (result < 0) {
    title_netif = NULL;
    shared = NULL;
    return ERR_IF;
  }
  netif_set_status_callback(netif, halo_debug_netif_changed);
  poll_link(netif);
  initializing = 0;
  DbgPrint("[net] sharing XBDM NIC; separate title/debug IP stacks\n");
  return ERR_OK;
}

int halo_net_debug_address(struct halo_xnaddr *address) {
  KIRQL previous;
  struct shared_interface *interface = find_shared();
  int result;
  memset(address, 0, sizeof(*address));
  address->size = sizeof(*address);
  if (!interface || interface->magic != XBD6_MAGIC)
    return 1;
  previous = KeRaiseIrqlToDpcLevel();
  result = interface->methods->debug_address(interface, address);
  KfLowerIrql(previous);
  return result;
}
