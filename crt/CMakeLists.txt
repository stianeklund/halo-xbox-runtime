# Standalone CRT overrides: nxdk C library sources rebuilt with MSVC-compatible
# behaviour Halo depends on. Compiled with nxdk's own flags (bin/nxdk-cc),
# not the game flags, so pdclib's headers are used unmodified.
set(CMAKE_C_FLAGS "-target i386-pc-win32 -march=pentium3 -ffreestanding -nostdlib -fno-builtin -Wno-builtin-macro-redefined -DNXDK -D__STDC__=1 -U__STDC_NO_THREADS__")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

add_library(standalone_crt OBJECT msvc_print.c pdclib_open.c)
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
