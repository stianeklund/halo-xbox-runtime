#ifndef HALO_NET_ROUTE_H
#define HALO_NET_ROUTE_H
struct netif;
struct ip4_addr;
struct netif *halo_net_secure_route(const struct ip4_addr *dest);
#endif
