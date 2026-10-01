# Standalone Halo build: reconstructed source from a halo checkout + nxdk ->
# default.xbe, with no halo.xbe.lib import library, no reg-arg thunks into the
# original image and no patch step.  The patched-XBE build in the halo repo
# stays the reference/decompilation target; this project only consumes it:
#
#   export NXDK_DIR=/path/to/nxdk            # built nxdk checkout
#   export HALO_ROOT=/path/to/halo           # halo checkout (src/, kb.json, tools/)
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/llvm.cmake
#   cmake --build build -j
#
# Outputs build/default.xbe plus link_report.json and deps_inventory.json
# classifying every dependency still unmet.  See docs/standalone-build.md.
cmake_minimum_required(VERSION 3.16)
project(halo_xbox_runtime C)
set(CMAKE_C_STANDARD 90)

# Any halo checkout or worktree works; the default is the sibling ../halo.
set(HALO_ROOT "$ENV{HALO_ROOT}" CACHE PATH "halo checkout providing src/, kb.json and tools/")
if(NOT HALO_ROOT AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../halo/kb.json")
	get_filename_component(HALO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../halo" ABSOLUTE)
	set(HALO_ROOT "${HALO_ROOT}" CACHE PATH "halo checkout providing src/, kb.json and tools/" FORCE)
endif()
if(NOT HALO_ROOT OR NOT EXISTS "${HALO_ROOT}/kb.json" OR NOT EXISTS "${HALO_ROOT}/src/CMakeLists.txt")
	message(FATAL_ERROR "Set HALO_ROOT to a halo checkout (kb.json and src/CMakeLists.txt missing under '${HALO_ROOT}').")
endif()
message(STATUS "HALO_ROOT: ${HALO_ROOT}")
set(STANDALONE_TOOLS "${CMAKE_CURRENT_LIST_DIR}/tools")

set(NXDK_DIR "$ENV{NXDK_DIR}" CACHE PATH "Built nxdk checkout")
if(NOT NXDK_DIR OR NOT EXISTS "${NXDK_DIR}/lib/xboxkrnl/libxboxkrnl.lib")
	message(FATAL_ERROR "Set NXDK_DIR to a built nxdk checkout (lib/xboxkrnl/libxboxkrnl.lib missing).")
endif()
if(NOT EXISTS "${NXDK_DIR}/tools/cxbe/cxbe")
	message(FATAL_ERROR "nxdk cxbe not built: ${NXDK_DIR}/tools/cxbe/cxbe")
endif()

# DEV ONLY.  Seeds the original image's initialized .rdata/.data into the
# legacy region from a user-supplied cachebeta.xbe so bring-up can run past
# data that has not been reconstructed yet.  Every seeded byte is a standalone
# blocker (counted in link_report.json).  Leave empty for a truly standalone
# (but not yet runnable) image.
set(HALO_STANDALONE_SEED_XBE "" CACHE FILEPATH "DEV ONLY: seed legacy data from this cachebeta.xbe")
option(HALO_STANDALONE_GPU_STATE "Push D3D render/texture-state methods to the GPU (OFF: mirror and device-field updates only)" ON)
option(HALO_STANDALONE_AUDIO "Play PCM/Xbox ADPCM through the selected stereo audio backend" ON)
set(HALO_STANDALONE_AUDIO_BACKEND "apu" CACHE STRING "Audio backend: apu (experimental nxdk-audio) or software")
set_property(CACHE HALO_STANDALONE_AUDIO_BACKEND PROPERTY STRINGS software apu)
if(NOT HALO_STANDALONE_AUDIO_BACKEND MATCHES "^(software|apu)$")
	message(FATAL_ERROR "HALO_STANDALONE_AUDIO_BACKEND must be software or apu")
endif()
set(NXDK_AUDIO_DIR "${CMAKE_CURRENT_LIST_DIR}/../nxdk-audio" CACHE PATH "nxdk-audio checkout (pinned revision; see docs/audio-apu.md)")
set(NXDK_AUDIO_REVISION "fc2deca2cc1e434805ac03ca7c2f500b3b028f36")
set(NXDK_AUDIO_ASSEMBLER "${CMAKE_CURRENT_LIST_DIR}/build-audio-tools/bin/dsp56300-asm" CACHE FILEPATH "dsp56300-asm 1.0.0 executable")
option(HALO_STANDALONE_TRACE_TEXTURE_STAGES "Debug: log shader-stage program and per-stage texture format/control0 before each D3D draw (DbgPrint + COM1)" OFF)
# Section layout / classification only; never linked into the output.
set(HALO_STANDALONE_LAYOUT_XBE "${HALO_ROOT}/halo-patched/cachebeta.xbe" CACHE FILEPATH
	"cachebeta.xbe used to classify dependencies in the reports")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
# The tools/ scripts import halo's own tools (patch.py, knowledge.py) from
# $HALO_ROOT; pass it explicitly so they never fall back to another checkout.
set(STANDALONE_PYTHON "${CMAKE_COMMAND}" -E env "HALO_ROOT=${HALO_ROOT}" "${Python3_EXECUTABLE}")

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
	message(FATAL_ERROR "Use the LLVM toolchain: -DCMAKE_TOOLCHAIN_FILE=toolchains/llvm.cmake")
endif()

set(gen_dir "${PROJECT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${gen_dir}")

add_custom_command(
	OUTPUT "${gen_dir}/decl.h"
	COMMAND "${Python3_EXECUTABLE}" "${HALO_ROOT}/tools/analysis/knowledge.py"
		--gen-header "${gen_dir}/decl.h"
	DEPENDS "${HALO_ROOT}/tools/analysis/knowledge.py" "${HALO_ROOT}/kb.json"
	VERBATIM
)
add_custom_target(standalone_decl DEPENDS "${gen_dir}/decl.h")

# build_info.c embeds halo's git revision: regenerate it whenever halo's HEAD
# moves (its reflog changes on every commit and checkout). An exported tree
# without git keeps the first revision it was built with.
set(_build_info_depends "${HALO_ROOT}/tools/build/gen-build-info.py")
execute_process(COMMAND git -C "${HALO_ROOT}" rev-parse --absolute-git-dir
	OUTPUT_VARIABLE _halo_git_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(_halo_git_dir AND EXISTS "${_halo_git_dir}/logs/HEAD")
	list(APPEND _build_info_depends "${_halo_git_dir}/logs/HEAD")
endif()
add_custom_command(
	OUTPUT "${gen_dir}/build_info.c"
	COMMAND "${Python3_EXECUTABLE}" "${HALO_ROOT}/tools/build/gen-build-info.py" > "${gen_dir}/build_info.c"
	DEPENDS ${_build_info_depends}
	WORKING_DIRECTORY "${HALO_ROOT}"
	VERBATIM
)

# Same code generation flags as the reference build (halo's root CMakeLists.txt),
# except that kb.json data is plain `extern` instead of dllimport: the
# standalone link resolves it to the legacy layout, not to an import table.
set(CMAKE_C_FLAGS "-Wall -Werror -target i386-pc-win32 -march=pentium3 -mno-sse -nostdlib -ffreestanding -fno-builtin -fno-exceptions -fno-omit-frame-pointer -mstack-probe-size=65536 -I${HALO_ROOT}/src -I${HALO_ROOT}/third_party/xbox -include ${HALO_ROOT}/src/common.h")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG -fno-omit-frame-pointer")
if(CMAKE_BUILD_TYPE STREQUAL "")
	set(CMAKE_BUILD_TYPE "Release")
endif()
include_directories(${gen_dir} ${HALO_ROOT}/src ${HALO_ROOT}/third_party/xbox)
add_compile_definitions(HDATA=extern HALO_STANDALONE=1)

# `halo` is the target name src/CMakeLists.txt adds the game sources to.
# Compatibility layers standing in for XDK libraries statically linked into
# cachebeta.xbe (XAPILIB, D3D8, DSOUND, XNET, ...). One file per library area.
set(STANDALONE_COMPAT_SOURCES
	compat/d3d8/device.c
	compat/d3d8/state.c
	compat/d3d8/draw.c
	compat/xapi/fileio.c
	compat/xapi/heap.c
	compat/xapi/input.c
	compat/xapi/keyboard.c
	compat/crt/string.c
	compat/crt/stdio.c
	compat/crt/wprintf.c
	compat/xapi/init.c
	compat/xapi/launch.c
	compat/xapi/memory.c
	compat/xapi/savegame.c
	compat/xapi/nickname.c
	compat/xapi/signature.c
	compat/xapi/thread.c
	compat/xapi/xvutil.c
	compat/bink/audio.c
	compat/bink/blit.c
	compat/bink/file.c
	compat/bink/memory.c
	compat/bink/playback.c
	compat/bink/radcb.c
	compat/bink/sound.c
	compat/bink/video.c
)
if(HALO_STANDALONE_AUDIO)
	list(APPEND STANDALONE_COMPAT_SOURCES compat/audio/dsound_nxdk.c)
else()
	list(APPEND STANDALONE_COMPAT_SOURCES compat/audio/dsound_silent.c compat/audio/dsound_stream_controls.c)
endif()
list(TRANSFORM STANDALONE_COMPAT_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
add_library(halo OBJECT "${gen_dir}/build_info.c" "${CMAKE_CURRENT_LIST_DIR}/standalone_runtime.c"
	${STANDALONE_COMPAT_SOURCES})
add_dependencies(halo standalone_decl)
if(HALO_STANDALONE_AUDIO AND HALO_STANDALONE_AUDIO_BACKEND STREQUAL "apu")
	target_compile_definitions(halo PRIVATE HALO_AUDIO_APU=1)
endif()
add_subdirectory("${HALO_ROOT}/src" src)
add_subdirectory(crt)

set(NXDK_LIBS
	"${NXDK_DIR}/lib/libwinapi.lib"
	"${NXDK_DIR}/lib/xboxkrnl/libxboxkrnl.lib"
	"${NXDK_DIR}/lib/libxboxrt.lib"
	"${NXDK_DIR}/lib/libpdclib.lib"
	"${NXDK_DIR}/lib/libnxdk_hal.lib"
	"${NXDK_DIR}/lib/libpbkit.lib"
	"${NXDK_DIR}/lib/nxdk_usb.lib"
	"${NXDK_DIR}/lib/libnxdk.lib"
	"${NXDK_DIR}/lib/libnxdk_automount_d.lib"
)
string(REPLACE ";" "," _nxdk_libs_arg "${NXDK_LIBS}")
# nxdk's NV2A method names (defines only) for the pbkit-backed D3D device.
set_source_files_properties(compat/d3d8/device.c compat/d3d8/state.c compat/d3d8/draw.c PROPERTIES COMPILE_OPTIONS "-I${NXDK_DIR}/lib/pbkit")
if(NOT HALO_STANDALONE_GPU_STATE)
	set_property(SOURCE compat/d3d8/state.c compat/d3d8/draw.c APPEND PROPERTY COMPILE_DEFINITIONS HALO_STANDALONE_NO_GPU_STATE)
endif()
if(HALO_STANDALONE_TRACE_TEXTURE_STAGES)
	set_property(SOURCE compat/d3d8/draw.c APPEND PROPERTY COMPILE_DEFINITIONS HALO_STANDALONE_TRACE_TEXTURE_STAGES)
endif()

file(GENERATE OUTPUT "${gen_dir}/objects.txt" CONTENT
	"$<JOIN:$<TARGET_OBJECTS:halo>,\n>\n$<JOIN:$<TARGET_OBJECTS:standalone_crt>,\n>\n$<JOIN:$<TARGET_OBJECTS:standalone_net>,\n>\n")

set(_seed_args)
set(_seed_depends)
if(HALO_STANDALONE_SEED_XBE)
	set(_seed_args --seed-xbe "${HALO_STANDALONE_SEED_XBE}")
	set(_seed_depends "${HALO_STANDALONE_SEED_XBE}")
	message(WARNING "HALO_STANDALONE_SEED_XBE is set: legacy data is seeded from ${HALO_STANDALONE_SEED_XBE}. The output is NOT standalone.")
endif()

add_custom_command(
	OUTPUT "${gen_dir}/legacy.s" "${gen_dir}/stubs.s" "${gen_dir}/link_report.json"
	COMMAND ${STANDALONE_PYTHON} "${STANDALONE_TOOLS}/gen_legacy_image.py"
		--kb "${HALO_ROOT}/kb.json"
		--objects "${gen_dir}/objects.txt"
		--libs "${_nxdk_libs_arg}"
		--xbe "${HALO_STANDALONE_LAYOUT_XBE}"
		--kernel-def "${HALO_ROOT}/src/xboxkrnl.exe.def"
		--out "${gen_dir}"
		${_seed_args}
	DEPENDS halo standalone_crt standalone_net $<TARGET_OBJECTS:halo> $<TARGET_OBJECTS:standalone_crt> $<TARGET_OBJECTS:standalone_net>
		"${STANDALONE_TOOLS}/gen_legacy_image.py"
		"${STANDALONE_TOOLS}/deps_inventory.py" "${HALO_ROOT}/kb.json"
		"${HALO_ROOT}/tools/analysis/knowledge.py" "${HALO_ROOT}/src/xboxkrnl.exe.def"
		"${HALO_STANDALONE_LAYOUT_XBE}" ${_seed_depends} ${NXDK_LIBS}
	VERBATIM
)

foreach(_asm legacy stubs)
	add_custom_command(
		OUTPUT "${gen_dir}/${_asm}.obj"
		COMMAND "${CMAKE_C_COMPILER}" -target i386-pc-win32 -c "${gen_dir}/${_asm}.s" -o "${gen_dir}/${_asm}.obj"
		DEPENDS "${gen_dir}/${_asm}.s"
		VERBATIM
	)
endforeach()

# legacy.obj MUST be first: its .text contribution pins VA 0x11000..0x642000.
set(_link_rsp "${gen_dir}/link.rsp")
file(GENERATE OUTPUT "${_link_rsp}" CONTENT
"\"${gen_dir}/legacy.obj\"
$<JOIN:$<TARGET_OBJECTS:halo>,
>
$<JOIN:$<TARGET_OBJECTS:standalone_crt>,
>
$<JOIN:$<TARGET_OBJECTS:standalone_net>,
>
\"${gen_dir}/stubs.obj\"
$<JOIN:${NXDK_LIBS},
>
")

add_custom_command(
	OUTPUT "${PROJECT_BINARY_DIR}/halo.exe"
	BYPRODUCTS "${PROJECT_BINARY_DIR}/halo.map"
	COMMAND lld-link -subsystem:windows -fixed -base:0x00010000 -stack:0x80000
		-merge:.edata=.edataxb -safeseh:no -nodefaultlib -entry:standalone_entry
		-include:_automount_d_drive
		-map:${PROJECT_BINARY_DIR}/halo.map
		-out:${PROJECT_BINARY_DIR}/halo.exe "@${_link_rsp}"
	DEPENDS "${gen_dir}/legacy.obj" "${gen_dir}/stubs.obj" $<TARGET_OBJECTS:halo>
		$<TARGET_OBJECTS:standalone_crt> $<TARGET_OBJECTS:standalone_net> ${NXDK_LIBS}
	VERBATIM
)

add_custom_command(
	OUTPUT "${PROJECT_BINARY_DIR}/default.xbe"
	COMMAND "${NXDK_DIR}/tools/cxbe/cxbe" "-OUT:${PROJECT_BINARY_DIR}/default.xbe"
		"-TITLE:Halo (standalone)" "${PROJECT_BINARY_DIR}/halo.exe"
	COMMAND "${Python3_EXECUTABLE}" "${STANDALONE_TOOLS}/xbe_make_writable.py"
		"${PROJECT_BINARY_DIR}/default.xbe" .text
	DEPENDS "${PROJECT_BINARY_DIR}/halo.exe" "${STANDALONE_TOOLS}/xbe_make_writable.py"
	VERBATIM
)

# The map Halo's stack walker loads from d:\cachebeta.map (D: is the XBE's
# directory); without it every stack frame prints as "?????".
add_custom_command(
	OUTPUT "${PROJECT_BINARY_DIR}/cachebeta.map"
	COMMAND ${STANDALONE_PYTHON} "${STANDALONE_TOOLS}/gen_stack_walk_map.py"
		"${PROJECT_BINARY_DIR}/halo.map" "${PROJECT_BINARY_DIR}/cachebeta.map"
		--kb "${HALO_ROOT}/kb.json"
	DEPENDS "${PROJECT_BINARY_DIR}/halo.exe" "${STANDALONE_TOOLS}/gen_stack_walk_map.py"
		"${STANDALONE_TOOLS}/gen_legacy_image.py" "${STANDALONE_TOOLS}/deps_inventory.py"
		"${STANDALONE_TOOLS}/symbolize.py" "${HALO_ROOT}/kb.json"
	VERBATIM
)

add_custom_target(standalone_xbe ALL DEPENDS "${PROJECT_BINARY_DIR}/default.xbe"
	"${PROJECT_BINARY_DIR}/cachebeta.map")
# Target-level ordering: with the Makefile generator a DEPENDS on another
# target's object files is not rebuilt first, so a changed standalone_crt
# source compiled after the link was already judged up to date.
add_dependencies(standalone_xbe halo standalone_crt)

# Full three-layer dependency inventory over this build's objects.
add_custom_target(standalone_inventory
	COMMAND ${STANDALONE_PYTHON} "${STANDALONE_TOOLS}/deps_inventory.py"
		--build-dir "${PROJECT_BINARY_DIR}" --object-dir "CMakeFiles/halo.dir"
		--xbe "${HALO_STANDALONE_LAYOUT_XBE}"
		--json-out "${PROJECT_BINARY_DIR}/deps_inventory.json"
	DEPENDS halo
	VERBATIM
)
