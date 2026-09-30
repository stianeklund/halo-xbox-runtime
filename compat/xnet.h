#ifndef HALO_STANDALONE_XNET_H
#define HALO_STANDALONE_XNET_H
#include <stdint.h>
struct halo_sockaddr { uint16_t family, port; uint32_t address; uint8_t zero[8]; };
struct halo_fd_set { uint32_t count; int sockets[64]; };
struct halo_xnaddr {
    uint16_t size;
    uint8_t ethernet[6];
    uint32_t address;
};
#ifdef HALO_NETWORK_HOST_TEST
#define XNET_CALL
#define XNET_SYMBOL(name, bytes)
#else
#define XNET_CALL __stdcall
#define XNET_SYMBOL(name, bytes) __asm__("_" name "@" #bytes)
#endif
#define XNET_DECL(ret, name, bytes, args) ret XNET_CALL halo_##name args XNET_SYMBOL(#name, bytes)
XNET_DECL(int, xnet_socket, 12, (int af, int type, int protocol));
XNET_DECL(int, xnet_bind, 12, (int s, const void *addr, int len));
XNET_DECL(int, xnet_connect, 12, (int s, const void *addr, int len));
XNET_DECL(int, xnet_listen, 8, (int s, int backlog));
XNET_DECL(int, xnet_accept, 12, (int s, void *addr, int *len));
XNET_DECL(int, xnet_closesocket, 4, (int s));
XNET_DECL(int, xnet_getsockname, 12, (int s, void *addr, int *len));
XNET_DECL(int, xnet_getpeername, 12, (int s, void *addr, int *len));
XNET_DECL(int, xnet_ioctlsocket, 12, (int s, int cmd, uint32_t *arg));
XNET_DECL(int, xnet_select, 20, (int ignored, uint32_t *read_set, uint32_t *write_set, uint32_t *except_set, const int32_t *timeout));
XNET_DECL(int, xnet_wsafdisset, 8, (int s, uint32_t *set));
XNET_DECL(int, xnet_getsockopt, 20, (int s, int level, int option, void *value, int *len));
XNET_DECL(int, xnet_setsockopt, 20, (int s, int level, int option, const void *value, int len));
XNET_DECL(int, xnet_recv, 16, (int s, void *buf, int len, int flags));
XNET_DECL(int, xnet_send, 16, (int s, const char *buf, int len, int flags));
XNET_DECL(int, xnet_recvfrom, 24, (int s, void *buf, int len, int flags, void *addr, int *addrlen));
XNET_DECL(int, xnet_sendto, 24, (int s, const void *buf, int len, int flags, const void *addr, int addrlen));
int XNET_CALL halo_XNetStartup(const void *params) XNET_SYMBOL("FUN_002231f8", 4);
int XNET_CALL halo_WSAStartup(int version, void *data) XNET_SYMBOL("FUN_00223206", 8);
int XNET_CALL halo_XNetGetTitleXnAddr(void *addr) XNET_SYMBOL("FUN_00222ecf", 4);
int XNET_CALL halo_XNetRandom(void *data, int len) XNET_SYMBOL("FUN_00222e0e", 8);
int XNET_CALL halo_XNetCreateKey(void *id, void *key) XNET_SYMBOL("FUN_00222da0", 8);
int XNET_CALL halo_XNetRegisterKey(void *id, void *key) XNET_SYMBOL("FUN_00222de0", 8);
int XNET_CALL halo_XNetUnregisterKey(void *id) XNET_SYMBOL("FUN_00222df7", 4);
int XNET_CALL halo_XNetXnAddrToInAddr(const void *addr, const void *id, uint32_t *ip) XNET_SYMBOL("FUN_00222e31", 12);
#ifdef HALO_NETWORK_HOST_TEST
int halo_XNetCleanup(void);
int halo_WSACleanup(void);
uint32_t halo_XNetGetEthernetLinkStatus(void);
#else
int halo_XNetCleanup(void) __asm__("_FUN_002232f5");
int halo_WSACleanup(void) __asm__("_FUN_002232ed");
uint32_t halo_XNetGetEthernetLinkStatus(void) __asm__("_XNetGetEthernetLinkStatus");
#endif
int halo_net_init(void);
int halo_net_security_start(unsigned flags);
void halo_net_security_clear(void);
int halo_net_register(const void *id, const void *key);
int halo_net_unregister(const void *id);
int halo_net_map(const struct halo_xnaddr *addr, const void *id, uint32_t *ip);
int halo_net_address(struct halo_xnaddr *addr);
uint32_t halo_net_link(void);
int halo_net_random(void *data, unsigned len);
#endif
