/* Initialise NIC/lwIP once: nxdk has no TCP/IP-worker teardown support. */
#include "../compat/xnet.h"
#include "nxdk_debug_net.h"
#include <lwip/dhcp.h>
#include <lwip/netifapi.h>
#include <lwip/tcpip.h>
#include <nxdk/configsector.h>
#include <stdatomic.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

_Static_assert(MEMP_NUM_NETCONN >= 64, "Halo needs a 64-socket lwIP build");
_Static_assert(MEMP_NUM_UDP_PCB > MEMP_NUM_NETCONN,
               "Reserve PCBs for DHCP/DNS");

static struct netif halo_netif;
static volatile LONG init_state;
static int init_result, use_dhcp;
err_t halo_xnet_input(struct pbuf *p, struct netif *netif);
void halo_xnet_attach(struct netif *netif);
static void tcpip_ready(void *event) {
  KeSetEvent(event, IO_NO_INCREMENT, FALSE);
}
int halo_net_init(void) {
  nxdk_network_config_sector_t config;
  ip4_addr_t ip = {0}, mask = {0}, gateway = {0};
  KEVENT ready;
  int result;
  if (InterlockedCompareExchange(&init_state, 1, 0) != 0) {
    while (init_state == 1)
      Sleep(1);
    return init_result;
  }
  use_dhcp = !nxLoadNetworkConfig(&config) ||
             !(config.dhcpFlags & NXDK_NETWORK_CONFIG_MANUAL_IP);
  if (!use_dhcp) {
    ip.addr = config.manual.ip;
    mask.addr = config.manual.subnetMask;
    gateway.addr = config.manual.defaultGateway;
  }
  KeInitializeEvent(&ready, SynchronizationEvent, FALSE);
  tcpip_init(tcpip_ready, &ready);
  KeWaitForSingleObject(&ready, Executive, KernelMode, FALSE, NULL);
  result = netifapi_netif_add(&halo_netif, &ip, &mask, &gateway, NULL,
                              halo_debug_netif_init, halo_xnet_input);
  if (result == ERR_OK) {
    LOCK_TCPIP_CORE();
    halo_xnet_attach(&halo_netif);
    UNLOCK_TCPIP_CORE();
    netifapi_netif_set_default(&halo_netif);
    netifapi_netif_set_up(&halo_netif);
    if (use_dhcp)
      result = netifapi_dhcp_start(&halo_netif);
  }
  init_result = result == ERR_OK ? 0 : 10091;
  InterlockedExchange(&init_state, 2);
  return init_result;
}
int halo_net_address(struct halo_xnaddr *addr) {
  uint32_t ip;
  memset(addr, 0, sizeof(*addr));
  if (init_state != 2 || init_result != 0)
    return 1;
  LOCK_TCPIP_CORE();
  ip = netif_ip4_addr(&halo_netif)->addr;
  memcpy(addr->ethernet, halo_netif.hwaddr, 6);
  UNLOCK_TCPIP_CORE();
  addr->size = 12;
  if (!(PhyGetLinkState(0) & 1))
    return 0;
  addr->address = ip;
  return 2 | (ip ? (use_dhcp ? 8 : 4) : 0);
}
uint32_t halo_net_link(void) { return PhyGetLinkState(0); }

/* Device-keyed HMAC generator with a persisted, rekeyed seed. T: is mounted
 * before transport initialization. Refuse generation if durable state cannot
 * be updated: timing/MAC alone is not a cryptographic seed. State is private
 * to this title and output is never used as the next on-disk seed. */
int halo_net_random(void *data, unsigned len) {
  static atomic_flag guard = ATOMIC_FLAG_INIT;
  static uint8_t state[20];
  static int seeded;
  uint8_t digest[20], input[40], old[20], *out = data;
  LARGE_INTEGER now;
  ULONGLONG ticks;
  HANDLE file;
  DWORD transferred;
  IO_STATUS_BLOCK flush_status;
  unsigned count;
  int result = 0;
  if (!len)
    return 0;
  while (atomic_flag_test_and_set(&guard))
    Sleep(1);
  KeQuerySystemTime(&now);
  ticks = KeQueryPerformanceCounter();
  memset(input, 0, sizeof(input));
  if (!seeded) {
    if (!memcmp(XboxHDKey, (const uint8_t[16]){0}, 16)) {
      result = 10091;
      goto done;
    }
    file = CreateFileA("T:\\xnet-random.bin", GENERIC_READ, FILE_SHARE_READ,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
      if (!ReadFile(file, state, sizeof(state), &transferred, NULL) ||
          transferred != sizeof(state))
        memset(state, 0, sizeof(state));
      CloseHandle(file);
    }
  }
  memcpy(input, state, 20);
  memcpy(input + 20, &now, 8);
  memcpy(input + 28, &ticks, 8);
  input[39] = 1;
  XcHMAC(XboxHDKey, 16, input, sizeof(input), NULL, 0, old);
  input[39] = 2;
  XcHMAC(old, 16, input, sizeof(input), NULL, 0, state);
  file = CreateFileA("T:\\xnet-random.bin", GENERIC_WRITE, 0, NULL,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    result = 10091;
    goto done;
  }
  if (!WriteFile(file, state, sizeof(state), &transferred, NULL) ||
      transferred != sizeof(state))
    result = 10091;
  if (NtFlushBuffersFile(file, &flush_status) < 0)
    result = 10091;
  if (!CloseHandle(file))
    result = 10091;
  if (result)
    goto done;
  seeded = 1;
  while (len) {
    input[39] = 3;
    XcHMAC(old, 16, input, sizeof(input), NULL, 0, digest);
    count = len < sizeof(digest) ? len : sizeof(digest);
    memcpy(out, digest, count);
    out += count;
    len -= count;
    memcpy(input, digest, 20);
  }
done:
  memset(digest, 0, sizeof(digest));
  memset(old, 0, sizeof(old));
  atomic_flag_clear(&guard);
  return result;
}
