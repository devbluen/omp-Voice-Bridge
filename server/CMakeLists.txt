set(VOICE_BRIDGE_OMP_SDK_DIR "${PROJECT_SOURCE_DIR}/omp-sdk" CACHE PATH "open.mp SDK checkout")
set(VOICE_BRIDGE_SAMP_SDK_DIR "${PROJECT_SOURCE_DIR}/samp-sdk" CACHE PATH "SA-MP plugin SDK (contains amx/amx.h)")

if(NOT EXISTS "${VOICE_BRIDGE_OMP_SDK_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "open.mp SDK not found. Run: git submodule update --init --recursive")
endif()
if(NOT EXISTS "${VOICE_BRIDGE_SAMP_SDK_DIR}/amx/amx.h")
	message(FATAL_ERROR "SA-MP plugin SDK not found. Run: git submodule update --init --recursive")
endif()

if(UNIX)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden")
endif()

add_subdirectory("${VOICE_BRIDGE_OMP_SDK_DIR}" "${CMAKE_BINARY_DIR}/omp-sdk")

set(VOICE_BRIDGE_CORE_SOURCES
	src/config.cpp
	src/log.cpp
	src/udp-socket.cpp
	src/voice-server.cpp
)

add_library(voice-bridge SHARED
	${VOICE_BRIDGE_CORE_SOURCES}
	src/natives.cpp
	src/pawn-host.cpp
	src/omp/omp-component.cpp
	src/samp/memory.cpp
	src/samp/samp-main.cpp
	src/samp/samp-raknet.cpp
	src/samp/samp-world.cpp
	src/amx-functions.cpp
)

if(WIN32)
	set(VB_RC_DESCRIPTION "Voice Bridge server (open.mp component and SA-MP plugin)")
	set(VB_RC_FILENAME "voice-bridge.dll")
	configure_file("${VOICE_BRIDGE_SHARED_DIR}/version.rc.in" "${CMAKE_BINARY_DIR}/generated/server-version.rc" @ONLY)
	target_sources(voice-bridge PRIVATE src/plugin.def "${CMAKE_BINARY_DIR}/generated/server-version.rc")
endif()

target_include_directories(voice-bridge PRIVATE
	src
	"${VOICE_BRIDGE_SHARED_DIR}"
	"${VOICE_BRIDGE_SAMP_SDK_DIR}"
	"${CMAKE_BINARY_DIR}/generated"
)

target_compile_definitions(voice-bridge PRIVATE
	HAVE_STDINT_H=1
	PAWN_CELL_SIZE=32
	$<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601>
	$<$<BOOL:${UNIX}>:LINUX>
)

target_link_libraries(voice-bridge PRIVATE OMP-SDK)

if(WIN32)
	target_link_libraries(voice-bridge PRIVATE ws2_32)
else()
	find_package(Threads REQUIRED)
	target_link_libraries(voice-bridge PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
	# 64-bit atomics on 32-bit x86 are normally inlined; link libatomic
	# statically only when the toolchain needs it.
	include(CheckCXXSourceCompiles)
	set(CMAKE_REQUIRED_FLAGS "-std=c++17")
	check_cxx_source_compiles("#include <atomic>
#include <cstdint>
std::atomic<uint64_t> v{0};
int main(){ return static_cast<int>(v.exchange(1) + v.load()); }" VOICE_BRIDGE_INLINE_ATOMICS)
	unset(CMAKE_REQUIRED_FLAGS)
	if(NOT VOICE_BRIDGE_INLINE_ATOMICS)
		target_link_libraries(voice-bridge PRIVATE -Wl,-Bstatic atomic -Wl,-Bdynamic)
	endif()
	if(VOICE_BRIDGE_STATIC_RUNTIME)
		target_link_options(voice-bridge PRIVATE -static-libstdc++ -static-libgcc)
	endif()
	target_link_options(voice-bridge PRIVATE "-Wl,--exclude-libs,ALL")
endif()

if(MSVC)
	target_compile_options(voice-bridge PRIVATE /W4 /permissive- /MP /wd4100)
else()
	target_compile_options(voice-bridge PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
endif()

set_target_properties(voice-bridge PROPERTIES
	OUTPUT_NAME "voice-bridge"
	PREFIX ""
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
	PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
	SKIP_BUILD_RPATH TRUE
)

# Pawn includes next to the binary so a build tree can be copied as is,
# stamped with the build version.
foreach(include voice-bridge.inc sampvoice.inc)
	file(READ "${PROJECT_SOURCE_DIR}/include/${include}" content)
	string(REGEX REPLACE "#define VOICE_BRIDGE_INCLUDE_VERSION \"[^\"]*\"" "#define VOICE_BRIDGE_INCLUDE_VERSION \"${PROJECT_VERSION}\"" content "${content}")
	file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/pawno/include/${include}" CONTENT "${content}")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/${include}")
endforeach()

if(BUILD_TESTING)
	add_executable(voice-bridge-tests
		tests/server-tests.cpp
		${VOICE_BRIDGE_CORE_SOURCES}
	)
	target_include_directories(voice-bridge-tests PRIVATE
		src
		"${VOICE_BRIDGE_SHARED_DIR}"
		"${CMAKE_BINARY_DIR}/generated"
	)
	target_link_libraries(voice-bridge-tests PRIVATE OMP-SDK)
	target_compile_definitions(voice-bridge-tests PRIVATE $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601>)
	if(WIN32)
		target_link_libraries(voice-bridge-tests PRIVATE ws2_32)
	else()
		find_package(Threads REQUIRED)
		target_link_libraries(voice-bridge-tests PRIVATE Threads::Threads)
		if(NOT VOICE_BRIDGE_INLINE_ATOMICS)
			target_link_libraries(voice-bridge-tests PRIVATE -Wl,-Bstatic atomic -Wl,-Bdynamic)
		endif()
	endif()
	add_test(NAME voice-bridge-server COMMAND voice-bridge-tests)
	set_tests_properties(voice-bridge-server PROPERTIES TIMEOUT 60)
endif()
