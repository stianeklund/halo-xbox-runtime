/* Xbox Winsock ABI -> nxdk lwIP. Built with nxdk headers in the CRT target. */
#include "xnet.h"
#include <errno.h>
#include <string.h>
#include <stdatomic.h>
#ifdef HALO_NETWORK_HOST_TEST
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sched.h>
#define lwip_socket socket
#define lwip_bind bind
#define lwip_connect connect
#define lwip_listen listen
#define lwip_accept accept
#define lwip_close close
#define lwip_getsockname getsockname
#define lwip_getpeername getpeername
#define lwip_ioctl ioctl
#define lwip_select select
#define lwip_setsockopt setsockopt
#define lwip_getsockopt getsockopt
#define lwip_recv recv
#define lwip_send send
#define lwip_recvfrom recvfrom
#define lwip_sendto sendto
void SetLastError(unsigned long error);
#else
#include <windows.h>
#include <lwip/sockets.h>
_Static_assert(FD_SETSIZE == 64 && LWIP_SOCKET_OFFSET == 0,
               "Winsock adapter and lwIP socket limits must agree");
#endif

_Static_assert(sizeof(struct halo_sockaddr) == 16, "Xbox sockaddr ABI");
_Static_assert(sizeof(struct halo_xnaddr) == 12, "Xbox XNADDR ABI");

static atomic_uint xnet_refs, wsa_refs;
static atomic_flag state_lock = ATOMIC_FLAG_INIT;
static void lock_state(void)
{
    while (atomic_flag_test_and_set(&state_lock)) {
#ifdef HALO_NETWORK_HOST_TEST
        sched_yield();
#else
        /* Yield to a lower-priority owner on the Xbox's single CPU. */
        Sleep(1);
#endif
    }
}
static void unlock_state(void) { atomic_flag_clear(&state_lock); }
static int sockets[64], socket_used[64];

static int winsock_error(int error)
{
    switch (error) {
    case 0: return 0;
    case EINTR: return 10004;
    case EACCES: return 10013;
    case EFAULT: return 10014;
    case EINVAL: return 10022;
    case EMFILE: case ENFILE: return 10024;
    case EAGAIN: return 10035;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return 10035;
#endif
    case EINPROGRESS: return 10036;
    case EALREADY: return 10037;
    case EBADF: case ENOTSOCK: return 10038;
    case EDESTADDRREQ: return 10039;
    case EMSGSIZE: return 10040;
    case EPROTOTYPE: return 10041;
    case ENOPROTOOPT: return 10042;
    case EPROTONOSUPPORT: return 10043;
    case EOPNOTSUPP: return 10045;
    case EAFNOSUPPORT: return 10047;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ENETRESET: return 10052;
    case ECONNABORTED: return 10053;
    case ECONNRESET: case EPIPE: return 10054;
    case ENOMEM: case ENOBUFS: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    default: return 10022;
    }
}
static int fail(int error) { SetLastError(error); return -1; }
static int checked(int result) { return result < 0 ? fail(winsock_error(errno)) : result; }
static int ready(void) { return wsa_refs ? 1 : (fail(10093), 0); }
static int remember(int s)
{
    unsigned i;
    if (s < 0) return checked(s);
    lock_state();
    for (i = 0; i < 64; ++i) if (!socket_used[i]) {
        socket_used[i] = 1; sockets[i] = s; unlock_state(); return s;
    }
    unlock_state();
    lwip_close(s);
    return fail(10024);
}
int XNET_CALL halo_XNetStartup(const void *params)
{
    int result;
    if (params && *(const uint8_t *)params != 11) return 10022;
    result = halo_net_init();
    if (!result) {
        lock_state();
        result = halo_net_security_start(params ? ((const uint8_t *)params)[1] : 0);
        if (!result) ++xnet_refs;
        unlock_state();
    }
    return result;
}
int XNET_CALL halo_WSAStartup(int version, void *data)
{
    int result;
    if (!data) return 10014;
    if ((version & 255) != 2) return 10092;
    result = halo_net_init();
    if (result) return result;
    memset(data, 0, 0x190);
    ((uint16_t *)data)[0] = (uint16_t)version;
    ((uint16_t *)data)[1] = 0x0202;
    lock_state(); ++wsa_refs; unlock_state();
    return 0;
}
int halo_XNetCleanup(void)
{
    lock_state();
    if (!xnet_refs) { unlock_state(); return 10093; }
    if (!--xnet_refs) halo_net_security_clear();
    unlock_state();
    return 0;
}
int halo_WSACleanup(void)
{
    unsigned i;
    lock_state();
    if (!wsa_refs) { unlock_state(); return fail(10093); }
    if (!--wsa_refs) for (i = 0; i < 64; ++i) if (socket_used[i]) {
        lwip_close(sockets[i]); socket_used[i] = 0;
    }
    unlock_state();
    return 0;
}
int XNET_CALL halo_XNetGetTitleXnAddr(void *addr)
{
    if (!addr) return fail(10014);
    return halo_net_address(addr);
}
uint32_t halo_XNetGetEthernetLinkStatus(void) { return halo_net_link(); }
int XNET_CALL halo_XNetRandom(void *data, int len)
{
    if (len < 0 || (!data && len)) return 10022;
    return halo_net_random(data, (unsigned)len);
}
int XNET_CALL halo_XNetCreateKey(void *id, void *key)
{
    int result;
    if (!id || !key) return 10014;
    result = halo_net_random(id, 8);
    if (!result) result = halo_net_random(key, 16);
    if (!result && !memcmp(id, (const uint8_t[4]){0}, 4))
        ((uint8_t *)id)[0] = 1; /* cachebeta 0x222dcf: first DWORD cannot be zero. */
    return result;
}
int XNET_CALL halo_XNetRegisterKey(void *id, void *key)
{
    if (!id || !key) return 10014;
    return xnet_refs ? halo_net_register(id, key) : 10093;
}
int XNET_CALL halo_XNetUnregisterKey(void *id)
{
    if (!id) return 10014;
    return xnet_refs ? halo_net_unregister(id) : 10093;
}
int XNET_CALL halo_XNetXnAddrToInAddr(const void *addr, const void *id, uint32_t *ip)
{
    if (!addr || !id || !ip) return 10014;
    *ip = 0;
    return xnet_refs ? halo_net_map(addr, id, ip) : 10093;
}
static int address_in(const void *raw, int len, struct sockaddr_in *addr)
{
    struct halo_sockaddr in;
    if (!raw || len < 16) return fail(10014);
    memcpy(&in, raw, sizeof(in));
    if (in.family != 2) return fail(10047);
    memset(addr, 0, sizeof(*addr));
#ifndef HALO_NETWORK_HOST_TEST
    addr->sin_len = sizeof(*addr);
#endif
    addr->sin_family = AF_INET; addr->sin_port = in.port;
    addr->sin_addr.s_addr = in.address;
    return 0;
}
static int address_out(const struct sockaddr_in *addr, void *raw, int *len)
{
    struct halo_sockaddr out = {0};
    if (!raw || !len || *len < 16) return fail(10014);
    out.family = 2; out.port = addr->sin_port; out.address = addr->sin_addr.s_addr;
    memcpy(raw, &out, sizeof(out)); *len = sizeof(out);
    return 0;
}
int XNET_CALL halo_xnet_socket(int af, int type, int protocol)
{
    if (!ready()) return -1;
    if (af != 2) return fail(10047);
    if (type != 1 && type != 2) return fail(10044);
    if (protocol && protocol != (type == 1 ? 6 : 17)) return fail(10043);
    return remember(lwip_socket(AF_INET, type, protocol));
}
int XNET_CALL halo_xnet_bind(int s, const void *raw, int len)
{
    struct sockaddr_in addr;
    if (!ready() || address_in(raw, len, &addr)) return -1;
    return checked(lwip_bind(s, (struct sockaddr *)&addr, sizeof(addr)));
}
int XNET_CALL halo_xnet_connect(int s, const void *raw, int len)
{
    struct sockaddr_in addr;
    int result;
    if (!ready() || address_in(raw, len, &addr)) return -1;
    result = lwip_connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (result < 0 && errno == EINPROGRESS) return fail(10035);
    return checked(result);
}
int XNET_CALL halo_xnet_listen(int s, int backlog) { return ready() ? checked(lwip_listen(s, backlog)) : -1; }
int XNET_CALL halo_xnet_accept(int s, void *raw, int *len)
{
    struct sockaddr_in addr;
    socklen_t size = sizeof(addr);
    int result;
    if (!ready()) return -1;
    if (raw && (!len || *len < 16)) return fail(10014);
    result = lwip_accept(s, (struct sockaddr *)&addr, &size);
    if (result < 0) return checked(result);
    if (raw) address_out(&addr, raw, len);
    return remember(result);
}
int XNET_CALL halo_xnet_closesocket(int s)
{
    unsigned i;
    int result;
    if (!ready()) return -1;
    lock_state();
    result = checked(lwip_close(s));
    if (!result) for (i = 0; i < 64; ++i) if (socket_used[i] && sockets[i] == s) socket_used[i] = 0;
    unlock_state();
    return result;
}
static int socket_name(int s, void *raw, int *len, int peer)
{
    struct sockaddr_in addr;
    socklen_t size = sizeof(addr);
    int result;
    if (!ready()) return -1;
    if (!raw || !len || *len < 16) return fail(10014);
    result = peer ? lwip_getpeername(s, (struct sockaddr *)&addr, &size) : lwip_getsockname(s, (struct sockaddr *)&addr, &size);
    if (checked(result) < 0) return -1;
    return address_out(&addr, raw, len);
}
int XNET_CALL halo_xnet_getsockname(int s, void *raw, int *len) { return socket_name(s, raw, len, 0); }
int XNET_CALL halo_xnet_getpeername(int s, void *raw, int *len) { return socket_name(s, raw, len, 1); }
int XNET_CALL halo_xnet_ioctlsocket(int s, int cmd, uint32_t *arg)
{
    if (!ready()) return -1;
    if (!arg) return fail(10014);
    if ((uint32_t)cmd == 0x8004667e) {
        int mode = *arg != 0;
        return checked(lwip_ioctl(s, FIONBIO, &mode));
    }
    if ((uint32_t)cmd == 0x4004667f) {
        unsigned long available = 0;
        int result = checked(lwip_ioctl(s, FIONREAD, &available));
        if (!result) *arg = (uint32_t)available;
        return result;
    }
    return fail(10022);
}
static int set_in(uint32_t *raw, fd_set *set, int *maxfd)
{
    struct halo_fd_set *in = (void *)raw;
    unsigned i;
    FD_ZERO(set);
    if (!in) return 0;
    if (in->count > 64) return fail(10022);
    for (i = 0; i < in->count; ++i) {
        int s = in->sockets[i];
        if (s < 0 || s >= FD_SETSIZE) return fail(10038);
        FD_SET(s, set);
        if (s > *maxfd) *maxfd = s;
    }
    return 0;
}
static void set_out(uint32_t *raw, const fd_set *set)
{
    struct halo_fd_set *out = (void *)raw;
    unsigned i, count = 0;
    if (!out) return;
    for (i = 0; i < out->count; ++i) if (FD_ISSET(out->sockets[i], set)) out->sockets[count++] = out->sockets[i];
    out->count = count;
}
int XNET_CALL halo_xnet_select(int ignored, uint32_t *read_set, uint32_t *write_set, uint32_t *except_set, const int32_t *timeout)
{
    fd_set rd, wr, ex;
    struct timeval tv;
    int maxfd = -1, result;
    (void)ignored;
    if (!ready()) return -1;
    if (set_in(read_set, &rd, &maxfd) || set_in(write_set, &wr, &maxfd) || set_in(except_set, &ex, &maxfd)) return -1;
    if (maxfd < 0) return fail(10022);
    if (timeout) {
        if (timeout[0] < 0 || timeout[1] < 0 || timeout[1] >= 1000000) return fail(10022);
        tv.tv_sec = timeout[0]; tv.tv_usec = timeout[1];
    }
    result = checked(lwip_select(maxfd + 1, read_set ? &rd : NULL, write_set ? &wr : NULL, except_set ? &ex : NULL, timeout ? &tv : NULL));
    if (result >= 0) { set_out(read_set, &rd); set_out(write_set, &wr); set_out(except_set, &ex); }
    return result;
}
int XNET_CALL halo_xnet_wsafdisset(int s, uint32_t *raw)
{
    struct halo_fd_set *set = (void *)raw;
    unsigned i;
    if (!set || set->count > 64) return 0;
    for (i = 0; i < set->count; ++i) if (set->sockets[i] == s) return 1;
    return 0;
}
static int socket_option(int level, int option)
{
    if (level == 0xffff) switch (option) {
    case 4: return SO_REUSEADDR;
    case 8: return SO_KEEPALIVE;
    case 0x20: return SO_BROADCAST;
    case 0x80: return SO_LINGER;
    case 0x1001: return SO_SNDBUF;
    case 0x1002: return SO_RCVBUF;
    case 0x1005: return SO_SNDTIMEO;
    case 0x1006: return SO_RCVTIMEO;
    case 0x1007: return SO_ERROR;
    case 0x1008: return SO_TYPE;
    default: return -1;
    }
    if (level == 6 && option == 1) return TCP_NODELAY;
    return -1;
}
int XNET_CALL halo_xnet_getsockopt(int s, int level, int option, void *value, int *len)
{
    int opt = socket_option(level, option), result;
    socklen_t size;
    if (!ready()) return -1;
    if (!value || !len || *len < 4) return fail(10014);
    if (opt < 0) return fail(10042);
    if (level == 0xffff && option == 0x80) {
        struct linger linger;
        uint16_t out[2];
        size = sizeof(linger);
        result = checked(lwip_getsockopt(s, SOL_SOCKET, opt, &linger, &size));
        if (!result) {
            out[0] = (uint16_t)linger.l_onoff; out[1] = (uint16_t)linger.l_linger;
            memcpy(value, out, 4); *len = 4;
        }
        return result;
    }
#ifndef HALO_NETWORK_HOST_TEST
    /* lwIP send capacity is compile-time fixed, not a per-socket setting. */
    if (level == 0xffff && option == 0x1001) {
        int type;
        size = sizeof(type);
        if (checked(lwip_getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &size)) < 0) return -1;
        *(int *)value = TCP_SND_BUF; *len = 4; return 0;
    }
#endif
    if (level == 0xffff && (option == 0x1005 || option == 0x1006)) {
        struct timeval tv;
        size = sizeof(tv);
        result = checked(lwip_getsockopt(s, SOL_SOCKET, opt, &tv, &size));
        if (!result) { *(int *)value = tv.tv_sec * 1000 + tv.tv_usec / 1000; *len = 4; }
        return result;
    }
    size = (socklen_t)*len;
    result = checked(lwip_getsockopt(s, level == 0xffff ? SOL_SOCKET : level, opt, value, &size));
    if (!result) {
        *len = (int)size;
        if (level == 0xffff && option == 0x1007) *(int *)value = winsock_error(*(int *)value);
    }
    return result;
}
int XNET_CALL halo_xnet_setsockopt(int s, int level, int option, const void *value, int len)
{
    int opt = socket_option(level, option);
    if (!ready()) return -1;
    if (!value || len < 4) return fail(10014);
    if (opt < 0) return fail(10042);
    if (level == 0xffff && (option == 0x1001 || option == 0x1002) &&
        *(const int *)value <= 0) return fail(10022);
    if (level == 0xffff && option == 0x80) {
        uint16_t in[2];
        struct linger linger;
        memcpy(in, value, 4); linger.l_onoff = in[0]; linger.l_linger = in[1];
        return checked(lwip_setsockopt(s, SOL_SOCKET, opt, &linger, sizeof(linger)));
    }
    if (level == 0xffff && (option == 0x1005 || option == 0x1006)) {
        int ms = *(const int *)value;
        struct timeval tv;
        if (ms < 0) return fail(10022);
        tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
        return checked(lwip_setsockopt(s, SOL_SOCKET, opt, &tv, sizeof(tv)));
    }
    return checked(lwip_setsockopt(s, level == 0xffff ? SOL_SOCKET : level, opt, value, len));
}
static int message_flags(int flags)
{
    if (flags & ~2) return fail(10045);
    return flags & 2 ? MSG_PEEK : 0;
}
static int buffer_valid(const void *buf, int len)
{
    if (len < 0) return fail(10022);
    if (!buf && len) return fail(10014);
    return 0;
}
int XNET_CALL halo_xnet_recv(int s, void *buf, int len, int flags)
{
    int mode = message_flags(flags);
    if (!ready() || mode < 0 || buffer_valid(buf, len)) return -1;
    return checked(lwip_recv(s, buf, (unsigned)len, mode));
}
int XNET_CALL halo_xnet_send(int s, const char *buf, int len, int flags)
{
    int mode = message_flags(flags);
    if (!ready() || mode < 0 || buffer_valid(buf, len)) return -1;
    return checked(lwip_send(s, buf, (unsigned)len, mode));
}
int XNET_CALL halo_xnet_recvfrom(int s, void *buf, int len, int flags, void *raw, int *addrlen)
{
    struct sockaddr_in addr;
    socklen_t size = sizeof(addr);
    int result, mode = message_flags(flags);
    if (!ready() || mode < 0 || buffer_valid(buf, len)) return -1;
    if (raw && (!addrlen || *addrlen < 16)) return fail(10014);
    result = checked(lwip_recvfrom(s, buf, (unsigned)len, mode, (struct sockaddr *)&addr, &size));
    if (result >= 0 && raw) address_out(&addr, raw, addrlen);
    return result;
}
int XNET_CALL halo_xnet_sendto(int s, const void *buf, int len, int flags, const void *raw, int addrlen)
{
    struct sockaddr_in addr;
    int mode = message_flags(flags);
    if (!ready() || mode < 0 || buffer_valid(buf, len) || address_in(raw, addrlen, &addr)) return -1;
    return checked(lwip_sendto(s, buf, (unsigned)len, mode, (struct sockaddr *)&addr, sizeof(addr)));
}
