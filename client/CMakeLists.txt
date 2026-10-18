include(FetchContent)

FetchContent_Declare(imgui
	GIT_REPOSITORY https://github.com/ocornut/imgui.git
	GIT_TAG v1.91.9
	GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(voice-bridge-imgui STATIC
	"${imgui_SOURCE_DIR}/imgui.cpp"
	"${imgui_SOURCE_DIR}/imgui_draw.cpp"
	"${imgui_SOURCE_DIR}/imgui_tables.cpp"
	"${imgui_SOURCE_DIR}/imgui_widgets.cpp"
	"${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp"
	"${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
)
target_include_directories(voice-bridge-imgui PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(voice-bridge-imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)

# RNNoise 0.1.1 (the last release with its model built in): microphone noise
# suppression.  The repository has no CMake project; only its sources are used.
FetchContent_Declare(rnnoise
	GIT_REPOSITORY https://github.com/xiph/rnnoise.git
	GIT_TAG v0.1.1
	GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(rnnoise)

# MSVC compiles C without variable length arrays: RNNoise's four become
# _alloca (the same stack allocation).
function(voice_bridge_patch_rnnoise file)
	file(READ "${rnnoise_SOURCE_DIR}/src/${file}" source)
	set(patched "${source}")
	math(EXPR last "${ARGC} - 1")
	foreach(i RANGE 1 ${last} 3)
		math(EXPR n "${i} + 1")
		math(EXPR c "${i} + 2")
		set(type "${ARGV${i}}")
		set(name "${ARGV${n}}")
		set(count "${ARGV${c}}")
		string(REPLACE "${type} ${name}[${count}];" "${type} *${name} = (${type} *)_alloca(sizeof(${type}) * (${count}));" patched "${patched}")
	endforeach()
	if(NOT patched MATCHES "#include <malloc.h>")
		set(patched "#include <malloc.h>\n${patched}")
	endif()
	if(NOT patched STREQUAL source)
		file(WRITE "${rnnoise_SOURCE_DIR}/src/${file}" "${patched}")
	endif()
endfunction()
voice_bridge_patch_rnnoise(celt_lpc.c opus_val16 xx n)
voice_bridge_patch_rnnoise(pitch.c
	opus_val16 x_lp4 "len>>2"
	opus_val16 y_lp4 "lag>>2"
	opus_val32 xcorr "max_pitch>>1"
	opus_val32 yy_lookup "maxperiod+1")

add_library(voice-bridge-rnnoise STATIC
	"${rnnoise_SOURCE_DIR}/src/celt_lpc.c"
	"${rnnoise_SOURCE_DIR}/src/denoise.c"
	"${rnnoise_SOURCE_DIR}/src/kiss_fft.c"
	"${rnnoise_SOURCE_DIR}/src/pitch.c"
	"${rnnoise_SOURCE_DIR}/src/rnn.c"
	"${rnnoise_SOURCE_DIR}/src/rnn_data.c"
	"${rnnoise_SOURCE_DIR}/src/rnn_reader.c"
)
target_include_directories(voice-bridge-rnnoise PUBLIC "${rnnoise_SOURCE_DIR}/include")
target_compile_definitions(voice-bridge-rnnoise PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
target_compile_options(voice-bridge-rnnoise PRIVATE /W0)

add_library(voice-bridge-client SHARED
	src/audio.cpp
	src/noise.cpp
	src/crash.cpp
	src/bass.cpp
	src/game.cpp
	src/log.cpp
	src/main.cpp
	src/overlay.cpp
	src/rakclient.cpp
	src/samp.cpp
	src/settings.cpp
	src/voice.cpp
)

set(VB_RC_DESCRIPTION "Voice Bridge client for SA-MP")
set(VB_RC_FILENAME "voice-bridge.asi")
configure_file("${VOICE_BRIDGE_SHARED_DIR}/version.rc.in" "${CMAKE_BINARY_DIR}/generated/client-version.rc" @ONLY)
target_sources(voice-bridge-client PRIVATE "${CMAKE_BINARY_DIR}/generated/client-version.rc")

target_include_directories(voice-bridge-client PRIVATE
	src
	"${VOICE_BRIDGE_SHARED_DIR}"
	"${CMAKE_BINARY_DIR}/generated"
)

target_compile_definitions(voice-bridge-client PRIVATE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	_CRT_SECURE_NO_WARNINGS
	_WINSOCK_DEPRECATED_NO_WARNINGS
	_WIN32_WINNT=0x0601
)

target_compile_options(voice-bridge-client PRIVATE /utf-8 /W3 /MP)
target_link_libraries(voice-bridge-client PRIVATE opus voice-bridge-rnnoise voice-bridge-imgui ws2_32 imm32 dwmapi)

set_target_properties(voice-bridge-client PROPERTIES
	OUTPUT_NAME "voice-bridge"
	PREFIX ""
	SUFFIX ".asi"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client"
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client"
	PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client"
)

# Manual smoke test of the audio engine (needs an audio device and bass.dll).
add_executable(voice-bridge-audio-smoke
	tests/audio-smoke.cpp
	src/audio.cpp
	src/noise.cpp
	src/bass.cpp
	src/game.cpp
	src/log.cpp
	src/samp.cpp
	src/settings.cpp
)
target_include_directories(voice-bridge-audio-smoke PRIVATE src "${VOICE_BRIDGE_SHARED_DIR}" "${CMAKE_BINARY_DIR}/generated")

# Plays a real-time conversation through the playback engine and prints the
# output level, to catch stuck or looping channels.
add_executable(voice-bridge-playback-sim
	tests/playback-sim.cpp
	src/audio.cpp
	src/noise.cpp
	src/bass.cpp
	src/game.cpp
	src/log.cpp
	src/samp.cpp
	src/settings.cpp
)
target_include_directories(voice-bridge-playback-sim PRIVATE src "${VOICE_BRIDGE_SHARED_DIR}" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(voice-bridge-playback-sim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601)
target_compile_options(voice-bridge-playback-sim PRIVATE /utf-8 /W3)
target_link_libraries(voice-bridge-playback-sim PRIVATE opus voice-bridge-rnnoise)
set_target_properties(voice-bridge-playback-sim PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client")

# Continuous speech through a bad network; fails if any frame is heard twice.
add_executable(voice-bridge-repeat-sim
	tests/repeat-sim.cpp
	src/audio.cpp
	src/noise.cpp
	src/bass.cpp
	src/game.cpp
	src/log.cpp
	src/samp.cpp
	src/settings.cpp
)
target_include_directories(voice-bridge-repeat-sim PRIVATE src "${VOICE_BRIDGE_SHARED_DIR}" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(voice-bridge-repeat-sim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601)
target_compile_options(voice-bridge-repeat-sim PRIVATE /utf-8 /W3)
target_link_libraries(voice-bridge-repeat-sim PRIVATE opus voice-bridge-rnnoise)
set_target_properties(voice-bridge-repeat-sim PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client")

# A mobile (SampVoice) speaker through a phone network: every frame heard
# once, no long stalls while talking.
add_executable(voice-bridge-mobile-sim
	tests/mobile-sim.cpp
	src/audio.cpp
	src/noise.cpp
	src/bass.cpp
	src/game.cpp
	src/log.cpp
	src/samp.cpp
	src/settings.cpp
)
target_include_directories(voice-bridge-mobile-sim PRIVATE src "${VOICE_BRIDGE_SHARED_DIR}" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(voice-bridge-mobile-sim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601)
target_compile_options(voice-bridge-mobile-sim PRIVATE /utf-8 /W3)
target_link_libraries(voice-bridge-mobile-sim PRIVATE opus voice-bridge-rnnoise)
set_target_properties(voice-bridge-mobile-sim PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client")
target_compile_definitions(voice-bridge-audio-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601)
target_compile_options(voice-bridge-audio-smoke PRIVATE /utf-8 /W3)
target_link_libraries(voice-bridge-audio-smoke PRIVATE opus voice-bridge-rnnoise)
set_target_properties(voice-bridge-audio-smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client")

# Renders the in-game menu to BMP files (layout check without the game).
add_executable(voice-bridge-menu-preview
	tools/menu-preview.cpp
	src/audio.cpp
	src/noise.cpp
	src/bass.cpp
	src/game.cpp
	src/log.cpp
	src/overlay.cpp
	src/rakclient.cpp
	src/samp.cpp
	src/settings.cpp
	src/voice.cpp
)
target_include_directories(voice-bridge-menu-preview PRIVATE src "${VOICE_BRIDGE_SHARED_DIR}" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(voice-bridge-menu-preview PRIVATE VB_MENU_PREVIEW WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS _WIN32_WINNT=0x0601)
target_compile_options(voice-bridge-menu-preview PRIVATE /utf-8 /W3)
target_link_libraries(voice-bridge-menu-preview PRIVATE opus voice-bridge-rnnoise voice-bridge-imgui ws2_32 imm32 dwmapi d3d9)
set_target_properties(voice-bridge-menu-preview PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client")

# Noise suppression on received voices, through the playback engine.
add_executable(voice-bridge-incoming-noise-sim
	tests/incoming-noise-sim.cpp
	src/audio.cpp
	src/noise.cpp
	src/bass.cpp
	src/game.cpp
	src/log.cpp
	src/samp.cpp
	src/settings.cpp
)
target_include_directories(voice-bridge-incoming-noise-sim PRIVATE src "${VOICE_BRIDGE_SHARED_DIR}" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(voice-bridge-incoming-noise-sim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601)
target_compile_options(voice-bridge-incoming-noise-sim PRIVATE /utf-8 /W3)
target_link_libraries(voice-bridge-incoming-noise-sim PRIVATE opus voice-bridge-rnnoise)
set_target_properties(voice-bridge-incoming-noise-sim PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client")

# Microphone noise suppression: noise removed, voice kept.
add_executable(voice-bridge-noise-check
	tests/noise-check.cpp
	src/noise.cpp
)
target_include_directories(voice-bridge-noise-check PRIVATE src "${VOICE_BRIDGE_SHARED_DIR}")
target_compile_definitions(voice-bridge-noise-check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(voice-bridge-noise-check PRIVATE /utf-8 /W3)
target_link_libraries(voice-bridge-noise-check PRIVATE voice-bridge-rnnoise)
set_target_properties(voice-bridge-noise-check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client")
