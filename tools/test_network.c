/* Integration tests run the real Winsock adapter over host BSD sockets. */
#include "../compat/xnet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

static _Thread_local unsigned long last_error;
void SetLastError(unsigned long error) { last_error = error; }
int halo_net_init(void) { return 0; }
uint32_t halo_net_link(void) { return 11; }
int halo_net_address(struct halo_xnaddr *addr)
{
    memset(addr, 0, sizeof(*addr)); addr->size=12; addr->address = htonl(INADDR_LOOPBACK);
    memset(addr->ethernet, 0x42, 6); return 6;
}
int halo_net_random(void *data, unsigned len)
{
    static unsigned serial;
    unsigned i;
    for (i = 0; i < len; ++i) ((unsigned char *)data)[i] = (unsigned char)++serial;
    return 0;
}
static struct { uint8_t id[8], key[16]; int used; } keys[128];
int halo_net_security_start(unsigned flags) { return flags & ~1u ? 10022 : 0; }
void halo_net_security_clear(void) { memset(keys, 0, sizeof(keys)); }
int halo_net_register(const void *id, const void *key) {
    unsigned i, free_slot=128;
    for(i=0;i<128;i++) {
        if(keys[i].used && !memcmp(keys[i].id,id,8)) return 10037;
        if(!keys[i].used) free_slot=i;
    }
    if(free_slot==128) return 10055;
    keys[free_slot].used=1;memcpy(keys[free_slot].id,id,8);memcpy(keys[free_slot].key,key,16);return 0;
}
int halo_net_unregister(const void *id) {
    unsigned i;for(i=0;i<128;i++) if(keys[i].used && !memcmp(keys[i].id,id,8)) { memset(&keys[i],0,sizeof(keys[i]));return 0; }
    return 10022;
}
int halo_net_map(const struct halo_xnaddr *addr,const void *id,uint32_t *ip) {
    unsigned i;for(i=0;i<128;i++) if(keys[i].used && !memcmp(keys[i].id,id,8)) { *ip=addr->address;return 0; }
    return 10022;
}
static struct halo_sockaddr bound(int s)
{
    struct halo_sockaddr addr = {2, 0, 0, {0}};
    int len = sizeof(addr);
    addr.address = htonl(INADDR_LOOPBACK);
    assert(halo_xnet_bind(s, &addr, sizeof(addr)) == 0);
    assert(halo_xnet_getsockname(s, &addr, &len) == 0);
    assert(addr.family == 2 && addr.port && len == 16);
    return addr;
}
static void wait_read(int s)
{
    struct halo_fd_set set = {1, {s}};
    int32_t timeout[2] = {1, 0};
    assert(halo_xnet_select(1, (uint32_t *)&set, NULL, NULL, timeout) == 1);
    assert(set.count == 1 && halo_xnet_wsafdisset(s, (uint32_t *)&set));
}
static void udp(void)
{
    int a = halo_xnet_socket(2, 2, 17), b = halo_xnet_socket(2, 2, 17), collision;
    struct halo_sockaddr addr = bound(a), sender;
    uint32_t mode = 1;
    char buffer[16];
    int len = sizeof(sender), option = 1, optlen = 4;
    assert(a >= 0 && b >= 0);
    assert(halo_xnet_ioctlsocket(a, (int)0x8004667e, &mode) == 0);
    assert(halo_xnet_recv(a, buffer, sizeof(buffer), 0) == -1 && last_error == 10035);
    assert(halo_xnet_setsockopt(a, 0xffff, 0x20, &option, 4) == 0);
    option = 0;
    assert(halo_xnet_getsockopt(a, 0xffff, 0x20, &option, &optlen) == 0 && option != 0);
    assert(halo_xnet_sendto(b, "hello", 5, 0, &addr, 16) == 5);
    wait_read(a);
    assert(halo_xnet_recv(a, buffer, 16, 2) == 5); /* Winsock MSG_PEEK */
    assert(halo_xnet_recvfrom(a, buffer, 16, 0, &sender, &len) == 5);
    assert(!memcmp(buffer, "hello", 5) && sender.family == 2 && len == 16);
    assert(halo_xnet_sendto(a, "reply", 5, 0, &sender, 16) == 5);
    wait_read(b);
    assert(halo_xnet_recv(b, buffer, 16, 0) == 5 && !memcmp(buffer, "reply", 5));
    collision = halo_xnet_socket(2, 2, 17);
    assert(halo_xnet_bind(collision, &addr, 16) == -1 && last_error == 10048);
    assert(halo_xnet_closesocket(collision) == 0);
    /* Rebinding an already-bound socket is a different error. */
    assert(halo_xnet_bind(b, &addr, 16) == -1 && last_error == 10022);
    assert(halo_xnet_sendto(b, buffer, -1, 0, &addr, 16) == -1 && last_error == 10022);
    assert(halo_xnet_closesocket(a) == 0 && halo_xnet_closesocket(b) == 0);
}
static void tcp(void)
{
    int listener = halo_xnet_socket(2, 1, 6), client = halo_xnet_socket(2, 1, 6), peer;
    struct halo_sockaddr addr = bound(listener), remote;
    int len = 16, result, error = -1, optlen = 4;
    uint32_t mode = 1;
    char buffer[8];
    uint16_t linger[2] = {1, 1}, out[2] = {0, 0};
    struct halo_fd_set writes = {1, {client}};
    int32_t timeout[2] = {1, 0};
    assert(halo_xnet_listen(listener, 4) == 0);
    assert(halo_xnet_ioctlsocket(client, (int)0x8004667e, &mode) == 0);
    result = halo_xnet_connect(client, &addr, 16);
    assert(result == 0 || (result == -1 && last_error == 10035));
    assert(halo_xnet_select(1, NULL, (uint32_t *)&writes, NULL, timeout) == 1);
    assert(halo_xnet_getsockopt(client, 0xffff, 0x1007, &error, &optlen) == 0 && error == 0);
    wait_read(listener);
    peer = halo_xnet_accept(listener, &remote, &len);
    assert(peer >= 0 && remote.family == 2 && len == 16);
    assert(halo_xnet_send(client, "tcp", 3, 0) == 3);
    wait_read(peer);
    assert(halo_xnet_recv(peer, buffer, 8, 0) == 3 && !memcmp(buffer, "tcp", 3));
    assert(halo_xnet_getpeername(client, &remote, &len) == 0 && remote.port == addr.port);
    assert(halo_xnet_setsockopt(client, 0xffff, 0x80, linger, 4) == 0);
    assert(halo_xnet_getsockopt(client, 0xffff, 0x80, out, &optlen) == 0 && out[0] == 1 && out[1] == 1);
    assert(halo_xnet_closesocket(peer) == 0 && halo_xnet_closesocket(client) == 0);
    assert(halo_xnet_closesocket(listener) == 0);
    /* Keep a bound, non-listening port reserved: no port-reuse race. */
    listener = halo_xnet_socket(2, 1, 6);
    addr = bound(listener);
    client = halo_xnet_socket(2, 1, 6);
    assert(halo_xnet_connect(client, &addr, 16) == -1 && last_error == 10061);
    assert(halo_xnet_closesocket(client) == 0 && halo_xnet_closesocket(listener) == 0);
}
static void *socket_worker(void *unused)
{
    char data[0x190];
    int i, s;
    (void)unused;
    assert(halo_WSAStartup(2, data) == 0);
    for (i = 0; i < 100; ++i) {
        s = halo_xnet_socket(2, i & 1 ? 1 : 2, 0);
        assert(s >= 0 && halo_xnet_closesocket(s) == 0);
    }
    assert(halo_WSACleanup() == 0);
    return NULL;
}
static void concurrent_sockets(void)
{
    pthread_t workers[8];
    unsigned i;
    for (i = 0; i < 8; ++i) assert(pthread_create(&workers[i], NULL, socket_worker, NULL) == 0);
    for (i = 0; i < 8; ++i) assert(pthread_join(workers[i], NULL) == 0);
}
static void capacity_and_cleanup(void)
{
    int sockets[64], i;
    struct halo_fd_set set = {0, {0}};
    int32_t zero[2] = {0, 0};
    char data[0x190];
    for (i = 0; i < 64; ++i) {
        sockets[i] = halo_xnet_socket(2, i < 24 ? 2 : 1, 0);
        assert(sockets[i] >= 0);
    }
    assert(halo_xnet_socket(2, 2, 0) == -1 && last_error == 10024);
    set.count = 65;
    assert(halo_xnet_select(1, (uint32_t *)&set, NULL, NULL, zero) == -1 && last_error == 10022);
    set.count = 1; set.sockets[0] = sockets[23];
    assert(halo_xnet_select(1, (uint32_t *)&set, NULL, NULL, zero) == 0 && set.count == 0);
    assert(halo_WSAStartup(2, data) == 0);
    assert(halo_WSACleanup() == 0); /* Reference retained: sockets stay open. */
    assert(fcntl(sockets[0], F_GETFD) >= 0);
    assert(halo_WSACleanup() == 0);
    for (i = 0; i < 64; ++i) assert(fcntl(sockets[i], F_GETFD) == -1);
    assert(halo_xnet_socket(2, 1, 0) == -1 && last_error == 10093);
    assert(halo_WSAStartup(2, data) == 0);
    i = halo_xnet_socket(2, 1, 0);
    assert(i >= 0 && halo_xnet_closesocket(i) == 0);
    assert(halo_WSACleanup() == 0);
}
int main(void)
{
    unsigned char data[0x191], id[8], key[16], params[11] = {11};
    struct halo_xnaddr addr;
    uint32_t ip = 0;
    alarm(10);
    assert(sizeof(struct halo_sockaddr) == 16 && sizeof(struct halo_xnaddr) == 12);
    assert(halo_xnet_socket(2, 2, 0) == -1 && last_error == 10093);
    assert(halo_XNetStartup(params) == 0);
    assert(halo_WSAStartup(1, data) == 10092);
    memset(data, 0x55, sizeof(data));
    assert(halo_WSAStartup(2, data) == 0 && data[0] == 2 && data[0x190] == 0x55);
    assert(halo_XNetGetTitleXnAddr(&addr) == 6 && addr.address == htonl(INADDR_LOOPBACK));
    assert(halo_XNetCreateKey(id, key) == 0);
    assert(halo_XNetRegisterKey(id, key) == 0);
    assert(halo_XNetRegisterKey(id, key) == 10037);
    assert(halo_XNetXnAddrToInAddr(&addr, id, &ip) == 0 && ip == addr.address);
    assert(halo_XNetUnregisterKey(id) == 0);
    assert(halo_XNetXnAddrToInAddr(&addr, id, &ip) == 10022 && ip == 0);
    udp(); tcp(); concurrent_sockets(); capacity_and_cleanup();
    assert(halo_XNetCleanup() == 0);
    puts("PASS: UDP/TCP, Winsock ABI/errors, nonblocking/select, 64 sockets, cleanup/restart");
    return 0;
}
