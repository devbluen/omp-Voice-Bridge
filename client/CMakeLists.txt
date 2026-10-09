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

add_library(voice-bridge-client SHARED
	src/audio.cpp
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
target_link_libraries(voice-bridge-client PRIVATE opus voice-bridge-imgui ws2_32 imm32 dwmapi)

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
	src/bass.cpp
	src/game.cpp
	src/log.cpp
	src/samp.cpp
	src/settings.cpp
)
target_include_directories(voice-bridge-playback-sim PRIVATE src "${VOICE_BRIDGE_SHARED_DIR}" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(voice-bridge-playback-sim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601)
target_compile_options(voice-bridge-playback-sim PRIVATE /utf-8 /W3)
target_link_libraries(voice-bridge-playback-sim PRIVATE opus)
set_target_properties(voice-bridge-playback-sim PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client")

# Continuous speech through a bad network; fails if any frame is heard twice.
add_executable(voice-bridge-repeat-sim
	tests/repeat-sim.cpp
	src/audio.cpp
	src/bass.cpp
	src/game.cpp
	src/log.cpp
	src/samp.cpp
	src/settings.cpp
)
target_include_directories(voice-bridge-repeat-sim PRIVATE src "${VOICE_BRIDGE_SHARED_DIR}" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(voice-bridge-repeat-sim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601)
target_compile_options(voice-bridge-repeat-sim PRIVATE /utf-8 /W3)
target_link_libraries(voice-bridge-repeat-sim PRIVATE opus)
set_target_properties(voice-bridge-repeat-sim PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client")
target_compile_definitions(voice-bridge-audio-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601)
target_compile_options(voice-bridge-audio-smoke PRIVATE /utf-8 /W3)
target_link_libraries(voice-bridge-audio-smoke PRIVATE opus)
set_target_properties(voice-bridge-audio-smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client")

# Renders the in-game menu to BMP files (layout check without the game).
add_executable(voice-bridge-menu-preview
	tools/menu-preview.cpp
	src/audio.cpp
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
target_link_libraries(voice-bridge-menu-preview PRIVATE opus voice-bridge-imgui ws2_32 imm32 dwmapi d3d9)
set_target_properties(voice-bridge-menu-preview PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client")
