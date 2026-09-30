#!/usr/bin/env python3
"""Exercise the production XNet packet adapter with nxdk's real lwIP sources.

Host shims replace Xbox kernel crypto, NIC submission, and worker scheduling.
Socket tests and protocol fixtures remain separate. No SDK files are modified.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sdk = Path(os.environ.get("NXDK_DIR", str(root.parent / "nxdk")))
lwip = sdk / "lib/net/lwip/src"
with tempfile.TemporaryDirectory(prefix="halo-xnet-stack-") as directory:
    temp = Path(directory)
    (temp / "arch").mkdir()
    (temp / "xboxkrnl").mkdir()
    # Use the Xbox configuration, overriding only host platform/shim choices.
    # Previously independent defaults hid the production single-netif defect.
    (temp / "lwipopts.h").write_text(f'#include "{root / "crt/net/lwipopts.h"}"\n' + """
#undef SYS_LIGHTWEIGHT_PROT
#define SYS_LIGHTWEIGHT_PROT 0
#undef LWIP_TCPIP_CORE_LOCKING
#define LWIP_TCPIP_CORE_LOCKING 0
#undef LWIP_NETCONN
#define LWIP_NETCONN 0
#undef LWIP_SOCKET
#define LWIP_SOCKET 0
#undef LWIP_IPV6
#define LWIP_IPV6 0
#undef MEM_CUSTOM_ALLOCATOR
#define MEM_CUSTOM_ALLOCATOR 0
#undef MEM_ALIGNMENT
#define MEM_ALIGNMENT 8
#undef LWIP_HOOK_DHCP_APPEND_OPTIONS
""")
    (temp / "arch/cc.h").write_text("""
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define LWIP_RAND() ((unsigned)rand())
#define LWIP_PLATFORM_DIAG(x) do { if (0) printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) assert(!(x))
""")
    (temp / "arch/sys_arch.h").write_text("""
typedef void *sys_mutex_t;
typedef void *sys_sem_t;
typedef void *sys_mbox_t;
typedef void *sys_thread_t;
""")
    (temp / "windows.h").write_text("/* No WinAPI is called by the packet adapter. */\n")
    (temp / "xboxkrnl/xboxkrnl.h").write_text("""
#include <stdint.h>
typedef uint32_t ULONG;
typedef unsigned char KIRQL;
extern unsigned char XboxLANKey[16];
KIRQL KeRaiseIrqlToDpcLevel(void);
void KfLowerIrql(KIRQL);
void *ExAllocatePoolWithTag(unsigned long, ULONG);
void ExFreePool(void *);
unsigned long PhyGetLinkState(unsigned long);
void DbgPrint(const char *, ...);
#define XNET_ETHERNET_LINK_ACTIVE 1
void XcHMAC(void *, ULONG, void *, ULONG, void *, ULONG, void *);
void XcKeyTable(ULONG, void *, void *);
void XcBlockCryptCBC(ULONG, ULONG, void *, void *, void *, ULONG, void *);
ULONG XcModExp(ULONG *, ULONG *, ULONG *, ULONG *, ULONG);
""")
    (temp / "xnet_profile.h").write_text(
        'static const unsigned char halo_title_lan_key[16] = "TESTTESTTESTTEST";\n')
    sources = sorted((lwip / "core").glob("*.c"))
    sources += sorted((lwip / "core/ipv4").glob("*.c"))
    sources += [lwip / "netif/ethernet.c", root / "compat/xnet/security.c",
                root / "crt/nxdk_xnet.c", root / "tools/test_xnet_stack.c",
                root / "tools/test_debug_net.c"]
    binary = temp / "test-stack"
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra",
        "-Werror", "-DHALO_NETWORK_HOST_TEST", "-fsanitize=address,undefined", "-g",
        "-I" + str(temp), "-I" + str(root / "crt/net"),
        "-I" + str(sdk / "lib"),
        "-I" + str(lwip / "include"), *map(str, sources), "-lcrypto", "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=15,
                   env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
