# The host's two programs, on macOS, Linux and Windows:
#
#   UCMapKitHost   mapkit/host/UCMapKitHost.cpp: the headless host the editor starts (bridge.h, PROTOCOL.md).
#                  Standard C++17 (nlohmann/json + platform.h), no engine header, no SDK.
#   UCOpenBridge   mapkit/host/openbridge: the bridge that runs OpenDE's engine (UCOpenBridge.dylib / .so),
#                  built by openbridge/CMakeLists.txt against a built OpenDE tree (OPENDE_ROOT).
#
#   cmake -S mapkit/host -B build/host -DCMAKE_BUILD_TYPE=Release -DOPENDE_ROOT=/path/to/OpenDE
#   cmake --build build/host
#   -> build/host/bin/UCMapKitHost, build/host/bin/UCOpenBridge.dylib (macOS) / .so (Linux)
#
# Stage the pair with dev/stage.py.
#
# Windows: mapkit/host/build_host.ps1 is still the build the release uses, because the binaries that run Deus Ex's
# own engine must be 32-bit x86 (UCEngineBridge.dll is) and it links the SDK headers and import libraries
# (dev/prepare_sdk.py). This file builds the same host there too - as a 64-bit program unless the generator is given
# -A Win32 - and the OpenDE bridge, which is 64-bit only, so the two Windows bridges are built separately and a run
# uses one of them (--bridge).
cmake_minimum_required(VERSION 3.20)
project(UCMapKitHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the build type (Debug or Release)" FORCE)
endif()

# Apple Silicon needs at least 11.0; without this the host OS's own SDK version is baked in.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version to target")
endif()

# The host and its bridge land in the same folder: the host looks for the bridge beside itself (platform.h).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---- the OpenDE tree the bridge is compiled and linked against (openbridge/CMakeLists.txt)
#
# Overridden with -DOPENDE_ROOT=<path> or the OPENDE_ROOT environment variable. The default is the OpenDE checkout
# that holds DeusEdix (this file is DeusEdix/mapkit/host, the engine is three folders up), when it looks like one.
option(UC_BUILD_OPENBRIDGE "Build the OpenDE bridge (mapkit/host/openbridge)" ON)

if(NOT DEFINED OPENDE_ROOT)
    if(DEFINED ENV{OPENDE_ROOT})
        set(OPENDE_ROOT "$ENV{OPENDE_ROOT}" CACHE PATH "The OpenDE source tree")
    else()
        get_filename_component(_uc_parent "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
        if(EXISTS "${_uc_parent}/src/engine" AND EXISTS "${_uc_parent}/vendor/SDL2-2.32.10")
            set(OPENDE_ROOT "${_uc_parent}" CACHE PATH "The OpenDE source tree")
        endif()
        unset(_uc_parent)
    endif()
endif()

# ---- the host
find_package(Threads REQUIRED)

add_executable(UCMapKitHost UCMapKitHost.cpp resident_host.h)

target_include_directories(UCMapKitHost PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"                   # platform.h, bridge.h, resident_host.h
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"       # nlohmann/json.hpp
)

if(WIN32)
    # bcrypt for the hashing in platform.h, as mapkit/host/build_host.ps1 links it
    target_link_libraries(UCMapKitHost PRIVATE bcrypt)
else()
    # dlopen (the bridge), pthread (the fault handler's thread), the C++ runtime
    target_link_libraries(UCMapKitHost PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---- the bridge: OpenDE's engine behind bridge.h (UCOpenBridge.dylib / UCOpenBridge.so)
if(UC_BUILD_OPENBRIDGE)
    if(NOT OPENDE_ROOT)
        message(FATAL_ERROR
            "OPENDE_ROOT is not set: pass -DOPENDE_ROOT=<the OpenDE tree>, or build only the host with "
            "-DUC_BUILD_OPENBRIDGE=OFF")
    endif()
    add_subdirectory(openbridge)
else()
    message(STATUS "UCOpenBridge: not built (UC_BUILD_OPENBRIDGE=OFF)")
endif()
