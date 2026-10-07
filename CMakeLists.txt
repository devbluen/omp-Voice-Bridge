cmake_minimum_required(VERSION 3.19)

option(VOICE_BRIDGE_BUILD_SERVER "Build the open.mp component / SA-MP plugin" ON)
option(VOICE_BRIDGE_BUILD_CLIENT "Build the GTA:SA client mod (Windows, 32-bit MSVC only)" OFF)
option(VOICE_BRIDGE_BUILD_32BIT "Build a 32-bit server binary on Linux (open.mp and SA-MP servers are 32-bit)" OFF)
option(VOICE_BRIDGE_STATIC_RUNTIME "Link the C++ runtime statically" ON)
# The version comes from the VERSION file unless -DVOICE_BRIDGE_VERSION=x.y.z
# is given (build.ps1 always passes it, so a cached value never goes stale).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" VOICE_BRIDGE_FILE_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
if(NOT VOICE_BRIDGE_VERSION)
	set(VOICE_BRIDGE_VERSION "${VOICE_BRIDGE_FILE_VERSION}")
endif()
string(STRIP "${VOICE_BRIDGE_VERSION}" VOICE_BRIDGE_VERSION)

if(NOT VOICE_BRIDGE_VERSION MATCHES "^([0-9]+)\.([0-9]+)\.([0-9]+)$")
	message(FATAL_ERROR "VOICE_BRIDGE_VERSION must use MAJOR.MINOR.PATCH format (got '${VOICE_BRIDGE_VERSION}')")
endif()
# The client reports MAJOR*10000 + MINOR*100 + PATCH in a 16-bit field.
if(CMAKE_MATCH_1 GREATER 6 OR CMAKE_MATCH_2 GREATER 99 OR CMAKE_MATCH_3 GREATER 99)
	message(FATAL_ERROR "VOICE_BRIDGE_VERSION ${VOICE_BRIDGE_VERSION} is out of range: MAJOR 0-6, MINOR 0-99, PATCH 0-99")
endif()
message(STATUS "Voice Bridge version ${VOICE_BRIDGE_VERSION}")

# Architecture flags must be known before project() runs compiler detection.
if(VOICE_BRIDGE_BUILD_32BIT AND NOT WIN32)
	set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -m32")
	set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -m32")
endif()

project(voice-bridge LANGUAGES C CXX VERSION ${VOICE_BRIDGE_VERSION})

# The open.mp SDK ships older third-party CMakeLists.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC AND VOICE_BRIDGE_STATIC_RUNTIME)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)

set(VOICE_BRIDGE_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shared")

configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/server/src/version.hpp.in"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp"
	@ONLY
)

if(VOICE_BRIDGE_BUILD_SERVER)
	add_subdirectory(server)
endif()

if(VOICE_BRIDGE_BUILD_CLIENT)
	if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
		message(FATAL_ERROR "The client mod must be built for 32-bit Windows (use the windows-client preset or -A Win32)")
	endif()
	add_subdirectory(client)
endif()
