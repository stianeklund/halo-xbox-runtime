# Standalone CRT overrides: nxdk C library sources rebuilt with MSVC-compatible
# behaviour Halo depends on. Compiled with nxdk's own flags (bin/nxdk-cc),
# not the game flags, so pdclib's headers are used unmodified.
set(CMAKE_C_FLAGS "-target i386-pc-win32 -march=pentium3 -ffreestanding -nostdlib -fno-builtin -Wno-builtin-macro-redefined -DNXDK -D__STDC__=1 -U__STDC_NO_THREADS__")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

add_library(standalone_crt OBJECT msvc_print.c pdclib_open.c nxdk_net.c nxdk_debug_net.c nxdk_xnet.c ../compat/xnet/xnet.c ../compat/xnet/security.c)
add_custom_command(
    OUTPUT "${gen_dir}/xnet_profile.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_xnet_profile.py"
        --xbe "${HALO_STANDALONE_LAYOUT_XBE}" --out "${gen_dir}/xnet_profile.h"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_xnet_profile.py" "${HALO_STANDALONE_LAYOUT_XBE}"
    VERBATIM)
target_sources(standalone_crt PRIVATE "${gen_dir}/xnet_profile.h")
if(HALO_STANDALONE_AUDIO)
	target_sources(standalone_crt PRIVATE ../compat/audio/mixer.c)
	if(HALO_STANDALONE_AUDIO_BACKEND STREQUAL "apu")
		include(nxaudio.cmake)
		target_sources(standalone_crt PRIVATE nxdk_audio_apu.c)
		target_compile_definitions(standalone_crt PRIVATE HALO_AUDIO_APU=1)
	else()
		target_sources(standalone_crt PRIVATE nxdk_audio.c)
	endif()
endif()
# USB keyboard for the XAPI debug keyboard. nxdk_usb.lib is built without its
# HID class driver, so compile it here with nxdk's USB configuration.
set(USB_DIR "${NXDK_DIR}/lib/usb")
set(USB_HID_SOURCES
	"${USB_DIR}/libusbohci/src_hid/hid_core.c"
	"${USB_DIR}/libusbohci/src_hid/hid_driver.c")
target_sources(standalone_crt PRIVATE nxdk_keyboard.c nxdk_usb.c ${USB_HID_SOURCES})
set_property(SOURCE nxdk_keyboard.c ${USB_HID_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
	"-I${USB_DIR}/libusbohci/inc" "-I${USB_DIR}/libusbohci_xbox"
	"-DUSBH_USE_EXTERNAL_CONFIG=\"usbh_config_xbox.h\""
	"-Dsysprintf=standalone_hid_log" "-Dget_ticks=usbh_get_ticks")
set_target_properties(standalone_crt PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
# Our sources build warning-clean like the game's; nxdk's HID and nxdk-audio
# sources, and pdclib_open.c (which includes nxdk's _PDCLIB_open.c), are
# compiled as they come.
set(CRT_OWN_SOURCES msvc_print.c nxdk_net.c nxdk_debug_net.c nxdk_xnet.c
	../compat/xnet/xnet.c ../compat/xnet/security.c nxdk_keyboard.c nxdk_usb.c
	../compat/audio/mixer.c nxdk_audio_apu.c nxdk_audio.c)
set_property(SOURCE ${CRT_OWN_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -Wall -Werror)
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
