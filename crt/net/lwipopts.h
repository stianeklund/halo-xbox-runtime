#ifndef HALO_LWIPOPTS_H
#define HALO_LWIPOPTS_H

#include <net/nforceif/include/lwipopts.h>

/* Halo requests 24 datagram and eight stream sockets. Allow 64 sockets of
 * either type, with separate PCB headroom for DHCP/DNS and TCP TIME_WAIT. */
#undef TCPIP_THREAD_STACKSIZE
#define TCPIP_THREAD_STACKSIZE (32 * 1024)
#define MEMP_NUM_NETCONN 64
#define MEMP_NUM_UDP_PCB 68
#define MEMP_NUM_TCP_PCB 80
#define MEMP_NUM_TCP_PCB_LISTEN 16
#define MEMP_NUM_TCP_SEG 256
#define MEMP_NUM_NETBUF 128
#define MEMP_NUM_PBUF 128
#define PBUF_POOL_SIZE 64
#define MEMP_NUM_TCPIP_MSG_INPKT 64
#define MEMP_NUM_TCPIP_MSG_API 64
/* XNet's periodic retry timer shares the pool with lwIP's cyclic timers. */
#define MEMP_NUM_SYS_TIMEOUT (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)
#define TCPIP_MBOX_SIZE 64
#define DEFAULT_TCP_RECVMBOX_SIZE 32
#define DEFAULT_UDP_RECVMBOX_SIZE 32
#define DEFAULT_ACCEPTMBOX_SIZE 16
#undef MEM_SIZE
#define MEM_SIZE (256 * 1024)
#define LWIP_SO_RCVBUF 1
#define LWIP_SO_SNDTIMEO 1
#define LWIP_SO_RCVTIMEO 1
#define SO_REUSE 1
#define IP_SOF_BROADCAST 1
#define IP_SOF_BROADCAST_RECV 1
/* The physical title interface, secure 0/8 interface, and local server need
 * distinct routes. nxdk defaults to one interface and no loopback interface. */
#undef LWIP_SINGLE_NETIF
#define LWIP_SINGLE_NETIF 0
#undef LWIP_HAVE_LOOPIF
#define LWIP_HAVE_LOOPIF 1
#define LWIP_NETIF_LOOPBACK 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_HOOK_FILENAME "halo_net_route.h"
#define LWIP_HOOK_IP4_ROUTE(dest) halo_net_secure_route(dest)
#endif
