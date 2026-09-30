# Standalone CRT overrides: nxdk C library sources rebuilt with MSVC-compatible
# behaviour Halo depends on. Compiled with nxdk's own flags (bin/nxdk-cc),
# not the game flags, so pdclib's headers are used unmodified.
set(CMAKE_C_FLAGS "-target i386-pc-win32 -march=pentium3 -ffreestanding -nostdlib -fno-builtin -Wno-builtin-macro-redefined -DNXDK -D__STDC__=1 -U__STDC_NO_THREADS__")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

add_library(standalone_crt OBJECT msvc_print.c pdclib_open.c nxdk_net.c nxdk_xnet.c ../compat/xnet.c ../compat/xnet_security.c)
add_custom_command(
    OUTPUT "${gen_dir}/xnet_profile.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_xnet_profile.py"
        --xbe "${HALO_STANDALONE_LAYOUT_XBE}" --out "${gen_dir}/xnet_profile.h"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_xnet_profile.py" "${HALO_STANDALONE_LAYOUT_XBE}"
    VERBATIM)
target_sources(standalone_crt PRIVATE "${gen_dir}/xnet_profile.h")
if(HALO_STANDALONE_AUDIO)
	target_sources(standalone_crt PRIVATE ../compat/audio_mixer.c)
	if(HALO_STANDALONE_AUDIO_BACKEND STREQUAL "apu")
		include(nxaudio.cmake)
		target_sources(standalone_crt PRIVATE nxdk_audio_apu.c)
		target_compile_definitions(standalone_crt PRIVATE HALO_AUDIO_APU=1)
	else()
		target_sources(standalone_crt PRIVATE nxdk_audio.c)
	endif()
endif()
set_target_properties(standalone_crt PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_include_directories(standalone_crt BEFORE PRIVATE
	"${NXDK_DIR}/lib/pdclib"
	"${NXDK_DIR}/lib"
	"${NXDK_DIR}/lib/xboxrt/libc_extensions"
	"${NXDK_DIR}/lib/pdclib/platform/xbox/include"
	"${NXDK_DIR}/lib/winapi"
	"${NXDK_DIR}/lib/xboxrt/vcruntime")
target_include_directories(standalone_crt SYSTEM BEFORE PRIVATE
	"${NXDK_DIR}/lib/pdclib/include")

# Rebuild lwIP locally: the SDK archive has four sockets and no SO_RCVBUF.
# Every consumer shares these options because they change lwIP structure layouts.
file(GLOB NET_CORE CONFIGURE_DEPENDS
	"${NXDK_DIR}/lib/net/lwip/src/core/*.c"
	"${NXDK_DIR}/lib/net/lwip/src/core/ipv4/*.c"
	"${NXDK_DIR}/lib/net/lwip/src/core/ipv6/*.c"
	"${NXDK_DIR}/lib/net/lwip/src/api/*.c")
add_library(standalone_net OBJECT ${NET_CORE}
	"${NXDK_DIR}/lib/net/lwip/src/netif/ethernet.c"
	"${NXDK_DIR}/lib/net/nvnetdrv/nvnetdrv.c"
	"${NXDK_DIR}/lib/net/nvnetdrv/nvnetdrv_lwip.c"
	"${NXDK_DIR}/lib/net/nforceif/src/sys_arch.c")
set_target_properties(standalone_net PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
get_target_property(NET_CRT_INCLUDES standalone_crt INCLUDE_DIRECTORIES)
target_include_directories(standalone_net BEFORE PRIVATE ${NET_CRT_INCLUDES})
foreach(net_target standalone_crt standalone_net)
	target_include_directories(${net_target} BEFORE PRIVATE
		"${CMAKE_CURRENT_LIST_DIR}/net"
		"${NXDK_DIR}/lib/net/lwip/src/include"
		"${NXDK_DIR}/lib/net/nforceif/include"
		"${NXDK_DIR}/lib/net/nvnetdrv")
endforeach()
