# Build a pinned sibling checkout with local patches in the build directory.
# Keep Halo's forced XDK headers and the upstream checkout out of this target.
if(NOT EXISTS "${NXDK_AUDIO_DIR}/include/nxaudio.h")
	message(FATAL_ERROR "Clone https://github.com/Ryzee119/nxdk-audio to NXDK_AUDIO_DIR=${NXDK_AUDIO_DIR}")
endif()
if(NOT EXISTS "${NXDK_AUDIO_ASSEMBLER}")
	message(FATAL_ERROR "Set NXDK_AUDIO_ASSEMBLER to dsp56300-asm 1.0.0. See docs/audio-apu.md")
endif()
set(nxaudio_gen "${CMAKE_CURRENT_BINARY_DIR}/nxaudio")
set(nxaudio_sources "${nxaudio_gen}/audio_core.c" "${nxaudio_gen}/audio_buffer.c" "${nxaudio_gen}/audio_voice.c")
file(GLOB nxaudio_inputs "${NXDK_AUDIO_DIR}/*.h" "${NXDK_AUDIO_DIR}/include/*.h")
add_custom_command(
	OUTPUT ${nxaudio_sources} "${nxaudio_gen}/passthrough.inc"
	COMMAND "${Python3_EXECUTABLE}" "${STANDALONE_TOOLS}/prepare_nxaudio.py"
		--source "${NXDK_AUDIO_DIR}" --revision "${NXDK_AUDIO_REVISION}"
		--output "${nxaudio_gen}" --assembler "${NXDK_AUDIO_ASSEMBLER}"
		--patch "${CMAKE_CURRENT_LIST_DIR}/nxaudio.patch"
	DEPENDS "${STANDALONE_TOOLS}/prepare_nxaudio.py" "${CMAKE_CURRENT_LIST_DIR}/nxaudio.patch"
		"${NXDK_AUDIO_DIR}/audio_core.c" "${NXDK_AUDIO_DIR}/audio_buffer.c" "${NXDK_AUDIO_DIR}/audio_voice.c"
		"${NXDK_AUDIO_DIR}/passthrough.a56" ${nxaudio_inputs}
	VERBATIM)
target_sources(standalone_crt PRIVATE ${nxaudio_sources})
target_include_directories(standalone_crt PRIVATE "${nxaudio_gen}" "${nxaudio_gen}/include")
target_compile_options(standalone_crt PRIVATE -mno-sse)
