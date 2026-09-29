/*
 * Standalone compat: an XNet stack with a link but no usable sockets.
 *
 * The debug build requires the transport: transport_initialize() only sets
 * transport_initialized once XNetStartup, WSAStartup and the
 * XNetGetTitleXnAddr poll succeed, and telnet_console_initialize() then calls
 * get_next_endpoint_from_set(), which asserts transport_initialized. Logging
 * stubs returning 0 make XNetGetTitleXnAddr report XNET_GET_XNADDR_PENDING
 * forever, so the poll times out after 10s and the assert fires.
 *
 * Until nxdk's lwIP is wired in, startup succeeds, the address is reported as
 * static Ethernet, and socket() fails with INVALID_SOCKET, so every endpoint
 * creation takes the game's own error path ("bind_endpoint() failed on telnet
 * console endpoint") instead of acting on a fake socket 0.
 *
 * Addresses and callee-pops sizes are the kb.json XNET:wsock.obj entries
 * (transport_endpoint_set_winsock.c documents the call sites).
 */
#include "xbox.h"

#define XNET_GET_XNADDR_ETHERNET 0x02
#define XNET_GET_XNADDR_STATIC 0x04
#define INVALID_SOCKET_VALUE (-1)
#define WINSOCK_VERSION_2_2 0x0202

int __stdcall standalone_XNetStartup(const void *params)
    __asm__("_FUN_002231f8@4");
int __stdcall standalone_WSAStartup(int version, void *wsadata)
    __asm__("_FUN_00223206@8");
int __stdcall standalone_XNetGetTitleXnAddr(void *xnaddr)
    __asm__("_FUN_00222ecf@4");
int standalone_XNetCleanup(void) __asm__("_FUN_002232f5");
int standalone_WSACleanup(void) __asm__("_FUN_002232ed");
int __stdcall standalone_socket(int af, int type, int protocol)
    __asm__("_xnet_socket@12");

/* 0x2231f8 */
int __stdcall standalone_XNetStartup(const void *params)
{
  (void)params;
  return 0;
}

/* 0x223206: WSADATA.wVersion / wHighVersion are the first two words. */
int __stdcall standalone_WSAStartup(int version, void *wsadata)
{
  unsigned short *words = (unsigned short *)wsadata;

  (void)version;
  words[0] = WINSOCK_VERSION_2_2;
  words[1] = WINSOCK_VERSION_2_2;
  return 0;
}

/* 0x222ecf: the XNADDR the caller passes (0x5ab230) stays zeroed. */
int __stdcall standalone_XNetGetTitleXnAddr(void *xnaddr)
{
  (void)xnaddr;
  return XNET_GET_XNADDR_ETHERNET | XNET_GET_XNADDR_STATIC;
}

/* 0x2232f5 */
int standalone_XNetCleanup(void)
{
  return 0;
}

/* 0x2232ed */
int standalone_WSACleanup(void)
{
  return 0;
}

/* 0x2255f6 */
int __stdcall standalone_socket(int af, int type, int protocol)
{
  (void)af;
  (void)type;
  (void)protocol;
  return INVALID_SOCKET_VALUE;
}
