#ifndef HALO_NXDK_DEBUG_NET_H
#define HALO_NXDK_DEBUG_NET_H
#include "../compat/xnet.h"
#include <lwip/netif.h>

/* Select the resident debugger's NIC when present, otherwise nxdk's driver. */
err_t halo_debug_netif_init(struct netif *netif);
void halo_debug_netif_changed(struct netif *netif);
int halo_net_debug_address(struct halo_xnaddr *addr);
#endif
